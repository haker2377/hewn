// Level design tools: cuts that remove the surface inside a box / cylinder / prism
// (doors, windows, holes in floors), Clip (a plane through a clicked line), and the "what next" action on
// W after a tool: a box becomes a room, two holes a corridor, one hole an alcove, a clip gets closed.
#include "hw_editor.h"
#include <godot_cpp/classes/editor_toaster.hpp>
#include "../hw_cutter.h"
#include "hw_editor_util.h"
#include "hw_textures.h"

#include <godot_cpp/classes/accept_dialog.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/tree_item.hpp>
#include <godot_cpp/classes/curve3d.hpp>
#include <godot_cpp/classes/path3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>

using namespace godot;
using namespace hw_editor;

namespace {

// Volumes are a hair deeper (the wall the cut is drawn on goes). Across they are exact: their sides are
// flush planes, faces lying on them (a floor under a door) stay.
const float CUT_MARGIN = 1e-4f;

void line3d(Control *p_overlay, Camera3D *p_cam, const Vector3 &a, const Vector3 &b, const Color &c, float w = 2.0f) {
	if (p_cam->is_position_behind(a) || p_cam->is_position_behind(b)) {
		return;
	}
	p_overlay->draw_line(p_cam->unproject_position(a), p_cam->unproject_position(b), c, w, true);
}

// Planes (outward) of a prism: a convex outline in (u, v) around `o`, from height h0 to h1 along n.
std::vector<hewn::ops::CutPlane> prism_planes(const Vector3 &o, const Vector3 &u, const Vector3 &v, const Vector3 &n, std::vector<Vector2> outline, float h0, float h1) {
	std::vector<hewn::ops::CutPlane> out;
	float area = 0.0f;
	for (size_t i = 0; i < outline.size(); i++) {
		area += outline[i].cross(outline[(i + 1) % outline.size()]);
	}
	if (area < 0.0f) {
		std::reverse(outline.begin(), outline.end()); // counter-clockwise seen from +n
	}
	for (size_t i = 0; i < outline.size(); i++) {
		const Vector2 a = outline[i], b = outline[(i + 1) % outline.size()];
		const Vector2 e = b - a;
		if (e.length() < 1e-6f) {
			continue;
		}
		const Vector3 nn = (u * e.y - v * e.x).normalized(); // out of the outline
		out.push_back({ nn, nn.dot(o + u * a.x + v * a.y) });
	}
	const float base = n.dot(o);
	out.push_back({ n, base + std::max(h0, h1) + CUT_MARGIN, false });
	out.push_back({ -n, -(base + std::min(h0, h1)) + CUT_MARGIN, false });
	return out;
}

bool convex_outline(const std::vector<Vector2> &p) {
	int sign = 0;
	for (size_t i = 0; i < p.size(); i++) {
		const Vector2 a = p[i], b = p[(i + 1) % p.size()], c = p[(i + 2) % p.size()];
		const float cr = (b - a).cross(c - b);
		if (std::fabs(cr) < 1e-9f) {
			continue;
		}
		const int s = cr > 0.0f ? 1 : -1;
		if (sign != 0 && s != sign) {
			return false;
		}
		sign = s;
	}
	return true;
}

// World planes to the local space of a node.
std::vector<std::vector<hewn::ops::CutPlane>> to_local(const std::vector<std::vector<hewn::ops::CutPlane>> &p_parts, const Transform3D &p_xf) {
	const Transform3D inv = p_xf.affine_inverse();
	const Basis bt = p_xf.basis.transposed();
	std::vector<std::vector<hewn::ops::CutPlane>> out;
	for (const std::vector<hewn::ops::CutPlane> &part : p_parts) {
		std::vector<hewn::ops::CutPlane> lp;
		for (const hewn::ops::CutPlane &p : part) {
			const Vector3 pl = inv.xform(p.n * p.d);
			const Vector3 nl = bt.xform(p.n).normalized();
			lp.push_back({ nl, nl.dot(pl), p.flush });
		}
		out.push_back(lp);
	}
	return out;
}

} // namespace

// ------------------------------------------------------------------ "what next" (W)

void HewnEditorPlugin::_arm_pending(const String &p_action, HewnMesh3D *p_node) {
	_arm_pending_loops(p_action, { p_node }, {});
}

void HewnEditorPlugin::_arm_pending_loops(const String &p_action, const std::vector<HewnMesh3D *> &p_meshes, const std::vector<std::pair<int, std::vector<int>>> &p_loops) {
	pending_action = p_action;
	pending_meshes.clear();
	for (HewnMesh3D *n : p_meshes) {
		if (n) {
			pending_meshes.push_back({ n->get_instance_id(), n->get_revision() });
		}
	}
	pending_loops = p_loops;
	update_overlays();
}

void HewnEditorPlugin::_keep_pending(HewnMesh3D *p_node) {
	for (PendingMesh &pm : pending_meshes) {
		if (p_node && pm.id == p_node->get_instance_id()) {
			pm.revision = p_node->get_revision();
		}
	}
}

// The first mesh of the armed action, while none of its meshes changed otherwise.
HewnMesh3D *HewnEditorPlugin::_pending_target() const {
	if (pending_action.is_empty() || pending_meshes.empty()) {
		return nullptr;
	}
	HewnMesh3D *first = nullptr;
	for (const PendingMesh &pm : pending_meshes) {
		HewnMesh3D *n = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(pm.id));
		if (!n || !n->is_inside_tree() || n->get_revision() != pm.revision) {
			return nullptr;
		}
		first = first ? first : n;
	}
	return first;
}

// W does something now: the meshes of the action are unchanged (or the prefab placed last is still
// selected and in place).
bool HewnEditorPlugin::_pending_ready() const {
	if (pending_action == "nd_corridor" || pending_action == "nd_alcove") {
		// (its lining follows it: also after it was moved, while it has none)
		HewnCutter3D *c = Object::cast_to<HewnCutter3D>(ObjectDB::get_instance(pending_cutter));
		return c && c->is_inside_tree() && c->get_lining() == HewnCutter3D::LINING_NONE;
	}
	if (pending_action == "prefab") {
		Node3D *n = prefab_last ? Object::cast_to<Node3D>(ObjectDB::get_instance(prefab_last)) : nullptr;
		if (!n || !n->is_inside_tree() || (prefab_path.is_empty() && !place_factory)) {
			return false;
		}
		const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
		return sel.size() == 1 && Object::cast_to<Node>(sel[0]) == n && n->get_global_transform().is_equal_approx(prefab_last_xf);
	}
	HewnMesh3D *t = _pending_target();
	if (!t) {
		return false;
	}
	if (pending_action == "invert") {
		// (a new shape: while it stays selected)
		const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
		return sel.has(t);
	}
	if (pending_action == "invert_into") {
		return const_cast<HewnEditorPlugin *>(this)->_into_panel_valid();
	}
	return true;
}

String HewnEditorPlugin::_pending_text() const {
	if (pending_action == "prefab") {
		return T("W: Place another one");
	}
	if (pending_action == "invert" || pending_action == "invert_into") {
		return T("W: Invert - make it a room (faces inward)");
	}
	if (pending_action == "corridor" || pending_action == "nd_corridor") {
		return T("W: Corridor - join the two holes");
	}
	if (pending_action == "alcove" || pending_action == "nd_alcove") {
		return T("W: Alcove - push the hole into the wall");
	}
	if (pending_action == "fill") {
		return T("W: Fill - close the cut");
	}
	if (pending_action == "folded") {
		return T("W: Complete - the cut follows its volume around the corner");
	}
	return String();
}

bool HewnEditorPlugin::_run_pending() {
	if ((pending_action == "nd_corridor" || pending_action == "nd_alcove" || pending_action == "corridor" || pending_action == "alcove") && last_op == "cut" &&
			_cut_panel_valid() && !(bool)op_param("cut", "lining")) {
		// right after a cut: its panel's Corridor / alcove, so the panel stays (non-destructive or not)
		set_op_param("cut", "lining", true);
		_run_cut(true);
		return true;
	}
	if (pending_action == "nd_corridor" || pending_action == "nd_alcove") {
		const bool corridor = pending_action == "nd_corridor";
		pending_action = String();
		return _line_cutter(corridor);
	}
	if (pending_action == "prefab") {
		pending_action = String();
		if (place_factory) {
			start_placing(place_label, place_factory, place_along, place_random, place_local);
			place_copy_from = ObjectID(prefab_last); // (with the values set in its panel)
		} else if (!prefab_path.is_empty()) {
			start_prefab(prefab_path);
		}
		return true;
	}
	HewnMesh3D *n = _pending_target();
	const String action = pending_action;
	const std::vector<PendingMesh> meshes = pending_meshes;
	const std::vector<std::pair<int, std::vector<int>>> loops = pending_loops;
	pending_action = String();
	pending_meshes.clear();
	pending_loops.clear();
	if (!n) {
		return false;
	}
	auto mesh = [&](int i) { return Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(meshes[i].id)); };
	EditorUndoRedoManager *ur = get_undo_redo();
	if (action == "invert_into") {
		// a shape built into the edited mesh: its own flip, through its Add panel
		if (!_into_panel_valid() || !into_shape) {
			return false;
		}
		into_shape->set_shape_param("flip_normals", into_shape->get_shape_param("flip_normals") > 0.5f ? 0.0f : 1.0f);
		_rebuild_into();
		_update_shape_panel();
		return true;
	}
	if (action == "invert") {
		if (n->get_shape() != hewn::shapes::CUSTOM) {
			// stays parametric: the shape's own flip
			const float old = n->get_shape_param("flip_normals");
			ur->create_action("Hewn: Invert", UndoRedo::MERGE_DISABLE, n);
			ur->add_do_method(n, "set_shape_param", "flip_normals", old > 0.5f ? 0.0f : 1.0f);
			ur->add_undo_method(n, "set_shape_param", "flip_normals", old);
			ur->commit_action();
		} else {
			const Dictionary before = n->get_state();
			PackedInt32Array all;
			for (int f = 0; f < n->get_face_count(); f++) {
				all.push_back(f);
			}
			n->flip_faces(all);
			ur->create_action("Hewn: Invert", UndoRedo::MERGE_DISABLE, n);
			ur->add_do_method(n, "set_state", n->get_state());
			ur->add_undo_method(n, "set_state", before);
			ur->commit_action(false);
		}
	} else if (action == "corridor" && loops.size() == 2) {
		HewnMesh3D *a = mesh(loops[0].first), *b = mesh(loops[1].first);
		if (!a || !b) {
			return false;
		}
		const Dictionary before = a->get_state();
		const TypedArray<Material> mats_before = a->get_materials().duplicate();
		std::vector<int> la = loops[0].second, lb = loops[1].second;
		ur->create_action("Hewn: Corridor", UndoRedo::MERGE_DISABLE, a);
		if (a != b) {
			// the rooms become one mesh (a corridor joins one surface)
			const int offset = (int)a->get_data().pos.size();
			a->merge_from(b);
			for (int &v : lb) {
				v += offset;
			}
			Node *parent = b->get_parent();
			ur->add_do_method(parent, "remove_child", b);
			ur->add_undo_method(parent, "add_child", b, true);
			ur->add_undo_method(parent, "move_child", b, b->get_index());
			ur->add_undo_method(b, "set_owner", b->get_owner());
			ur->add_undo_reference(b);
		}
		hewn::ops::bridge_loops(a->get_data(), la, lb);
		a->commit(true);
		ur->add_do_method(a, "set_materials", a->get_materials().duplicate());
		ur->add_do_method(a, "set_state", a->get_state());
		ur->add_undo_method(a, "set_materials", mats_before);
		ur->add_undo_method(a, "set_state", before);
		ur->commit_action(a != b); // (a removal has to run; the mesh is done)
		if (a != b) {
			EditorInterface::get_singleton()->get_selection()->clear();
			EditorInterface::get_singleton()->get_selection()->add_node(a);
		}
	} else if (action == "folded") {
		// the meshes as before the cut, minus the volume (rooms: their empty space plus it)
		const std::vector<Dictionary> precut = pending_precut;
		const std::vector<std::vector<hewn::ops::CutPlane>> parts = pending_parts;
		ur->create_action("Hewn: Complete Cut", UndoRedo::MERGE_DISABLE, n);
		for (size_t i = 0; i < meshes.size() && i < precut.size(); i++) {
			HewnMesh3D *m = mesh((int)i);
			if (!m) {
				continue;
			}
			const Dictionary after_cut = m->get_state();
			m->set_state(precut[i]);
			hewn::Mesh &md = m->get_data();
			const std::vector<std::vector<hewn::ops::CutPlane>> local = to_local(parts, m->get_global_transform());
			// the volume: a box around everything, cut down to each convex part
			AABB box(md.pos.empty() ? Vector3() : md.pos[0], Vector3());
			for (const Vector3 &p : md.pos) {
				box.expand_to(p);
			}
			box = box.grow(1.0f + box.get_longest_axis_size());
			std::unordered_map<int, int> count;
			int mat = 0, best = -1;
			for (const hewn::Face &F : md.faces) {
				if (++count[F.material] > best) {
					best = count[F.material];
					mat = F.material;
				}
			}
			double vol = 0.0;
			for (const hewn::Face &F : md.faces) {
				for (int k = 1; k + 1 < F.size(); k++) {
					vol += md.pos[F.v[0]].dot(md.pos[F.v[k]].cross(md.pos[F.v[k + 1]]));
				}
			}
			const bool room = vol < 0.0; // (faces inward: the empty space is what is solid here)
			hewn::Mesh result = md;
			if (room) {
				for (hewn::Face &F : result.faces) {
					F.reverse();
				}
			}
			bool ok_all = true;
			for (const std::vector<hewn::ops::CutPlane> &planes : local) {
				hewn::Mesh solid;
				for (int k = 0; k < 8; k++) {
					solid.add_vertex(box.position + box.size * Vector3(k & 1 ? 1 : 0, k & 2 ? 1 : 0, k & 4 ? 1 : 0));
				}
				// (corners: bit 0 x, bit 1 y, bit 2 z; faces counter-clockwise from outside)
				static const int quads[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
				for (const auto &q : quads) {
					solid.faces.push_back(hewn::make_face({ q[0], q[1], q[2], q[3] }));
				}
				for (const hewn::ops::CutPlane &pl : planes) {
					hewn::ops::bisect(solid, {}, pl.n * pl.d, pl.n, false, true, true);
				}
				for (hewn::Face &F : solid.faces) {
					F.material = mat;
				}
				bool ok = false;
				hewn::Mesh r = hewn::csg::boolean(result, solid, room ? hewn::csg::UNION : hewn::csg::SUBTRACT, &ok);
				if (ok) {
					result = r;
				}
				ok_all = ok_all && ok;
			}
			if (room) {
				for (hewn::Face &F : result.faces) {
					F.reverse();
				}
			}
			if (!ok_all) {
				m->set_state(after_cut); // (not a closed mesh: the cut stays as it was)
				continue;
			}
			md = result;
			m->commit(true);
			ur->add_do_method(m, "set_state", m->get_state());
			ur->add_undo_method(m, "set_state", after_cut);
		}
		ur->commit_action(false);
	} else if ((action == "alcove" || action == "fill") && !loops.empty()) {
		std::vector<HewnMesh3D *> done;
		ur->create_action(action == "alcove" ? "Hewn: Alcove" : "Hewn: Fill", UndoRedo::MERGE_DISABLE, n);
		std::vector<Dictionary> befores(meshes.size());
		for (size_t i = 0; i < meshes.size(); i++) {
			if (HewnMesh3D *m = mesh((int)i)) {
				befores[i] = m->get_state();
			}
		}
		for (const std::pair<int, std::vector<int>> &l : loops) {
			HewnMesh3D *m = mesh(l.first);
			if (!m) {
				continue;
			}
			if (action == "alcove") {
				// along the cut volume's side facing into the wall, to that side (a cut turned against the wall)
				std::vector<hewn::ops::CutPlane> planes;
				for (const std::vector<hewn::ops::CutPlane> &part : to_local(pending_parts, m->get_global_transform())) {
					planes.insert(planes.end(), part.begin(), part.end());
				}
				hewn::ops::alcove(m->get_data(), l.second, pending_depth, planes.empty() ? nullptr : &planes);
			} else {
				hewn::ops::fill_loop(m->get_data(), l.second);
			}
		}
		for (size_t i = 0; i < meshes.size(); i++) {
			HewnMesh3D *m = mesh((int)i);
			if (!m) {
				continue;
			}
			m->commit(true);
			ur->add_do_method(m, "set_state", m->get_state());
			ur->add_undo_method(m, "set_state", befores[i]);
		}
		ur->commit_action(false);
	}
	shape_panel_values.clear(); // (the panel shows the new values)
	update_overlays();
	return true;
}

void HewnEditorPlugin::_draw_pending(Control *p_overlay) {
	if (!_pending_ready()) {
		return;
	}
	Ref<Font> font = p_overlay->get_theme_default_font();
	const String t = _pending_text();
	if (font.is_null() || t.is_empty()) {
		return;
	}
	const int fs = p_overlay->get_theme_default_font_size();
	const Vector2 sz = font->get_string_size(t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs);
	const Vector2 at(std::round((p_overlay->get_size().x - sz.x) * 0.5f), p_overlay->get_size().y - 18.0f * EditorInterface::get_singleton()->get_editor_scale());
	p_overlay->draw_rect(Rect2(at - Vector2(10, font->get_ascent(fs) + 5), sz + Vector2(20, 10)), Color(0.1f, 0.1f, 0.12f, 0.8f));
	p_overlay->draw_string(font, at, t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, TOOL_COLOR);
}

// ------------------------------------------------------------------ cuts

// The meshes a level tool works on: the edited mesh in edit mode (its selected faces, if any), the
// selected Hewn meshes in object mode, or all of them when none is selected.
std::vector<HewnMesh3D *> HewnEditorPlugin::_level_targets(PackedInt32Array *r_faces) const {
	std::vector<HewnMesh3D *> out;
	HewnMesh3D *edited = get_edited();
	if (edited && mode != MODE_OBJECT) {
		out.push_back(edited);
		if (r_faces && mode == MODE_FACE) {
			for (int f : edited->selection.faces) {
				r_faces->push_back(f);
			}
		}
		return out;
	}
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	for (int64_t i = 0; i < sel.size(); i++) {
		if (HewnMesh3D *n = Object::cast_to<HewnMesh3D>(sel[i])) {
			out.push_back(n);
		}
	}
	if (out.empty() && edited) {
		out.push_back(edited);
	}
	if (out.empty()) {
		out = _scene_meshes();
	}
	return out;
}

void HewnEditorPlugin::start_cut_tool(int p_kind) {
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	if (p_kind == 3) {
		start_poly_tool(false);
		poly_cut = true;
		return;
	}
	_clear_into();
	_hide_shape_panel();
	_gather_snap_candidates(false);
	shape_cut = p_kind == 2 ? 2 : 1;
	cut_infinite = false;
	shape_type = shape_cut == 2 ? hewn::shapes::CYLINDER : hewn::shapes::CUBE;
	shape_state = 0;
	shape_dir = 0;
	shape_fixed_side = false;
	wall_pts.clear();
	grid_hover_valid = false;
	modal = MODAL_SHAPE; // drawn like the shapes (clicks, Shift: turned, Alt: the last plane)
	update_overlays();
}

// Cuts every target with the world volume; arms W (corridor / alcove). Returns the number of holes.
int HewnEditorPlugin::_apply_cut_world(const std::vector<std::vector<hewn::ops::CutPlane>> &p_parts, float p_depth) {
	PackedInt32Array faces;
	const std::vector<HewnMesh3D *> targets = _level_targets(&faces);
	EditorUndoRedoManager *ur = get_undo_redo();
	std::vector<HewnMesh3D *> changed;
	std::vector<std::pair<int, std::vector<int>>> loops;
	bool any = false;
	std::vector<Dictionary> precut;
	for (HewnMesh3D *n : targets) {
		const Dictionary before = n->get_state();
		const int nf = n->get_face_count();
		const PackedInt32Array packed = n->cut_parts(to_local(p_parts, n->get_global_transform()), faces);
		if (packed.is_empty() && n->get_face_count() == nf) {
			n->set_state(before); // (nothing inside: no change at all)
			continue;
		}
		if (!any) {
			ur->create_action("Hewn: Cut", UndoRedo::MERGE_DISABLE, n);
			any = true;
			last_cut_first = n;
		}
		ur->add_do_method(n, "set_state", n->get_state());
		ur->add_undo_method(n, "set_state", before);
		const int idx = (int)changed.size();
		changed.push_back(n);
		precut.push_back(before);
		for (const std::vector<int> &l : HewnMesh3D::unpack_loops(packed)) {
			loops.push_back({ idx, l });
		}
	}
	if (any) {
		ur->commit_action(false);
	}
	pending_depth = p_depth;
	// one hole that is not flat: the cut went over an edge or a corner
	bool folded = false;
	if (loops.size() == 1 && loops[0].second.size() >= 3) {
		const hewn::Mesh &m = changed[loops[0].first]->get_data();
		Vector3 c, nrm;
		const std::vector<int> &l = loops[0].second;
		for (size_t i = 0; i < l.size(); i++) {
			const Vector3 a = m.pos[l[i]], b = m.pos[l[(i + 1) % l.size()]];
			c += a / (float)l.size();
			nrm += Vector3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
		}
		if (nrm.length() > 1e-9f) {
			nrm.normalize();
			for (int v : l) {
				folded = folded || std::fabs((m.pos[v] - c).dot(nrm)) > 1e-3f;
			}
		}
	}
	pending_parts = p_parts;
	if (loops.size() == 2) {
		_arm_pending_loops("corridor", changed, loops);
	} else if (folded) {
		_arm_pending_loops("folded", changed, loops);
		pending_precut = precut;
	} else if (loops.size() == 1) {
		_arm_pending_loops("alcove", changed, loops);
	}
	dock->refresh();
	update_overlays();
	return (int)loops.size();
}

std::vector<std::vector<hewn::ops::CutPlane>> HewnEditorPlugin::_shape_cut_parts(float p_h, bool p_infinite) const {
	const bool round = shape_cut == 2;
	std::vector<Vector2> outline;
	if (round) {
		const Vector2 r = (shape_b - shape_a).abs(); // (an oval, like the cylinder)
		const int sides = MAX(3, (int)dock->cutter_sides->get_value());
		for (int i = 0; i < sides; i++) {
			const float a = (float)Math::PI * 2.0f * i / sides;
			outline.push_back(shape_a + Vector2(std::cos(a) * r.x, std::sin(a) * r.y));
		}
	} else {
		const Vector2 mn = shape_a.min(shape_b), mx = shape_a.max(shape_b);
		outline = { mn, Vector2(mx.x, mn.y), mx, Vector2(mn.x, mx.y) };
	}
	float h0 = MIN(0.0f, p_h), h1 = MAX(0.0f, p_h);
	if (p_infinite) {
		h0 = -1e5f;
		h1 = 1e5f;
	}
	return { prism_planes(shape_origin, shape_u, shape_v, shape_n, outline, h0, h1) };
}

void HewnEditorPlugin::_apply_shape_cut() {
	const std::vector<std::vector<hewn::ops::CutPlane>> parts = _shape_cut_parts(shape_h, cut_infinite);
	// the same volume as a cutter node (the panel's Non-destructive): X along u, Y along the normal
	const Vector3 Z = shape_u.cross(shape_n).normalized(); // (= -v)
	float h0 = MIN(0.0f, shape_h), h1 = MAX(0.0f, shape_h);
	if (cut_infinite) {
		h0 = -1e4f;
		h1 = 1e4f;
	}
	if (h1 - h0 < 0.01f) {
		h0 -= 0.25f; // (a flat cut: a little depth both ways)
		h1 += 0.25f;
	}
	LastCut lc;
	lc.parts = parts;
	lc.depth = std::fabs(shape_h) > 0.01f ? std::fabs(shape_h) : 0.5f;
	Vector2 c;
	if (shape_cut == 2) {
		const Vector2 r = (shape_b - shape_a).abs();
		lc.shape = 1;
		lc.size = Vector3(MAX(0.001f, r.x * 2.0f), h1 - h0, MAX(0.001f, r.y * 2.0f));
		lc.sides = MAX(3, (int)dock->cutter_sides->get_value());
		c = shape_a;
	} else {
		const Vector2 mn = shape_a.min(shape_b), mx = shape_a.max(shape_b);
		lc.shape = 0;
		lc.size = Vector3(MAX(0.001f, mx.x - mn.x), h1 - h0, MAX(0.001f, mx.y - mn.y));
		c = (mn + mx) * 0.5f;
	}
	Basis b;
	b.set_column(0, shape_u);
	b.set_column(1, shape_n);
	b.set_column(2, Z);
	lc.xf = Transform3D(b, shape_origin + shape_u * c.x + shape_v * c.y + shape_n * ((h0 + h1) * 0.5f));
	modal = MODAL_NONE;
	shape_state = 0;
	shape_fixed_side = false;
	shape_cut = 0;
	cut_infinite = false;
	last_cut = lc;
	_run_cut(false);
}

// The last cut as the panel says: destructive (holes in the meshes, then W) or a HewnCutter3D node
// that keeps cutting them; Keep Side Faces: faces lying on its sides stay (a floor under a door), off:
// they go too (like a boolean).
void HewnEditorPlugin::_run_cut(bool p_redo) {
	EditorUndoRedoManager *ur = get_undo_redo();
	if (p_redo) {
		if (last_cut.history != -2) { // (-2: the cut reached nothing, there is nothing to take back)
			UndoRedo *h = last_cut.history >= 0 ? ur->get_history_undo_redo((int)last_cut.history) : nullptr;
			if (!h || (int64_t)h->get_version() != last_cut.version) {
				_hide_redo();
				return;
			}
			for (int i = 0; i < last_cut.steps; i++) {
				h->undo();
			}
		}
	} else {
		set_op_param("cut", "lining", false); // (a new cut: no corridor until asked for)
	}
	last_cut.steps = 1;
	const bool nd = (bool)op_param("cut", "non_destructive");
	const bool keep = (bool)op_param("cut", "keep_flush");
	const bool line = (bool)op_param("cut", "lining");
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	last_cut.history = -1;
	if (nd && root) {
		HewnCutter3D *c = memnew(HewnCutter3D);
		c->set_name("Cutter");
		c->set_shape(last_cut.shape);
		c->set_size(last_cut.size);
		c->set_sides(last_cut.sides);
		c->set_points(last_cut.points);
		c->set_keep_flush(keep);
		c->set_cut_ends(true); // (the wall it was drawn on goes)
		if (line) {
			c->set_lining_depth(last_cut.depth);
			c->set_lining(HewnCutter3D::LINING_AUTO); // (a corridor between two holes, else an alcove)
		}
		ur->create_action("Hewn: Cutter");
		ur->add_do_method(root, "add_child", c, true);
		ur->add_do_method(c, "set_owner", root);
		ur->add_do_reference(c);
		ur->add_undo_method(root, "remove_child", c);
		ur->commit_action();
		c->set_global_transform(last_cut.xf);
		last_cut.history = ur->get_object_history_id(root);
		pending_action = String();
		// W as after a destructive cut: how many holes it makes (tried on the meshes, then put back)
		PackedInt32Array faces;
		int holes = 0;
		pending_cut_meshes.clear();
		for (HewnMesh3D *n : _level_targets(&faces)) {
			if (!c->cuts(n)) {
				continue;
			}
			hewn::Mesh m = n->get_data();
			const int h = (int)hewn::ops::cut_volumes(m, {}, to_local(last_cut.parts, n->get_global_transform())).size();
			if (h > 0) {
				holes += h;
				pending_cut_meshes.push_back(ObjectID(n->get_instance_id()));
			}
		}
		pending_action = line ? String() : holes == 2 ? "nd_corridor" : "nd_alcove";
		pending_cutter = c->get_instance_id();
		pending_cutter_xf = last_cut.xf;
		pending_depth = last_cut.depth;
	} else {
		std::vector<std::vector<hewn::ops::CutPlane>> parts = last_cut.parts;
		if (!keep) {
			for (std::vector<hewn::ops::CutPlane> &part : parts) {
				for (hewn::ops::CutPlane &p : part) {
					p.flush = false;
				}
			}
		}
		last_cut_first = nullptr;
		_apply_cut_world(parts, last_cut.depth);
		if (last_cut_first) {
			last_cut.history = ur->get_object_history_id(last_cut_first);
			if (line && (pending_action == "corridor" || pending_action == "alcove") && _pending_ready()) {
				last_op = String(); // (W's own way: not back into this panel)
				if (_run_pending()) {
					last_cut.steps = 2;
				}
			}
		}
	}
	UndoRedo *h = last_cut.history >= 0 ? ur->get_history_undo_redo((int)last_cut.history) : nullptr;
	last_cut.version = h ? (int64_t)h->get_version() : -1;
	if (h) {
		_hide_redo();
		last_op = "cut";
		_show_redo();
	} else if (!nd) {
		// a cut drawn where there is no mesh: nothing is made, but its panel offers to keep it as a cutter node
		last_cut.history = -2;
		last_cut.version = -2;
		_hide_redo();
		last_op = "cut";
		_show_redo();
		if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
			t->push_toast(T("The cut reaches no mesh: turn on Non-destructive in its panel (bottom left) to keep it as a cutter"));
		}
	}
	update_overlays();
}

bool HewnEditorPlugin::_cut_panel_valid() {
	if (last_cut.history == -2) {
		return true; // (the empty cut's panel stays until another operation)
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	UndoRedo *h = last_cut.history >= 0 ? ur->get_history_undo_redo((int)last_cut.history) : nullptr;
	return h && (int64_t)h->get_version() == last_cut.version;
}

// while a cut is drawn, where it meets the faces (red): a face it misses shows at once.
void HewnEditorPlugin::_draw_cut_intersections(Control *p_overlay, Camera3D *p_cam, const std::vector<std::vector<hewn::ops::CutPlane>> &p_parts) const {
	int budget = 4000;
	for (HewnMesh3D *n : _level_targets(nullptr)) {
		const hewn::Mesh &m = n->get_data();
		const Transform3D g = n->get_global_transform();
		for (const hewn::Face &F : m.faces) {
			if (budget-- <= 0) {
				return;
			}
			std::vector<Vector3> face;
			for (int v : F.v) {
				face.push_back(g.xform(m.pos[v]));
			}
			for (const std::vector<hewn::ops::CutPlane> &planes : p_parts) {
				std::vector<Vector3> poly = face;
				// Sutherland-Hodgman against each plane (inside: n . p <= d)
				for (const hewn::ops::CutPlane &pl : planes) {
					std::vector<Vector3> out;
					for (size_t i = 0; i < poly.size() && poly.size() >= 3; i++) {
						const Vector3 a = poly[i], b = poly[(i + 1) % poly.size()];
						const float da = pl.n.dot(a) - pl.d, db = pl.n.dot(b) - pl.d;
						if (da <= 0.0f) {
							out.push_back(a);
						}
						if ((da < 0.0f) != (db < 0.0f) && std::fabs(da - db) > 1e-12f) {
							out.push_back(a + (b - a) * (da / (da - db)));
						}
					}
					poly = out;
				}
				if (poly.size() < 3) {
					continue;
				}
				PackedVector2Array pts;
				bool ok = true;
				for (const Vector3 &p : poly) {
					ok = ok && !p_cam->is_position_behind(p);
					if (ok) {
						pts.push_back(p_cam->unproject_position(p));
					}
				}
				if (!ok) {
					continue;
				}
				for (int64_t i = 0; i < pts.size(); i++) {
					p_overlay->draw_line(pts[i], pts[(i + 1) % pts.size()], Color(1.0f, 0.25f, 0.2f, 0.95f), 2.0f, true);
				}
			}
		}
	}
}

void HewnEditorPlugin::_apply_prism_cut(const std::vector<Vector3> &p_pts, float p_height) {
	const Vector3 n = poly_n.normalized();
	Vector3 u, v;
	hewn::plane_basis(n, u, v);
	v = n.cross(u).normalized();
	const Vector3 o = p_pts[0];
	std::vector<Vector2> outline;
	for (const Vector3 &p : p_pts) {
		outline.push_back(Vector2((p - o).dot(u), (p - o).dot(v)));
	}
	const float h0 = MIN(0.0f, p_height), h1 = MAX(0.0f, p_height);
	{
		LastCut lc;
		lc.shape = 2;
		lc.depth = std::fabs(p_height) > 0.01f ? std::fabs(p_height) : 0.5f;
		const Vector3 Z = u.cross(n).normalized();
		for (const Vector2 &q : outline) {
			lc.points.push_back(Vector2(q.x, (v * q.y).dot(Z))); // (local z of the plane point)
		}
		const float y0 = h1 - h0 < 0.01f ? -0.25f : h0;
		lc.size = Vector3(1, h1 - h0 < 0.01f ? 0.5f : h1 - h0, 1);
		Basis b;
		b.set_column(0, u);
		b.set_column(1, n);
		b.set_column(2, Z);
		lc.xf = Transform3D(b, o + n * y0);
		last_cut = lc;
	}
	std::vector<std::vector<hewn::ops::CutPlane>> parts;
	if (convex_outline(outline)) {
		parts.push_back(prism_planes(o, u, v, n, outline, h0, h1));
	} else {
		// a concave outline: its triangles, one convex volume each
		hewn::Mesh tmp;
		std::vector<int> ids;
		for (const Vector2 &q : outline) {
			ids.push_back(tmp.add_vertex(Vector3(q.x, q.y, 0.0f)));
		}
		tmp.faces.push_back(hewn::make_face(ids));
		hewn::ops::triangulate_faces(tmp, { 0 });
		for (const hewn::Face &f : tmp.faces) {
			std::vector<Vector2> tri;
			for (int id : f.v) {
				tri.push_back(Vector2(tmp.pos[id].x, tmp.pos[id].y));
			}
			parts.push_back(prism_planes(o, u, v, n, tri, h0, h1));
		}
	}
	last_cut.parts = parts;
	_run_cut(false);
}

// ------------------------------------------------------------------ clip

void HewnEditorPlugin::start_clip() {
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	_gather_snap_candidates(false);
	clip_pts.clear();
	clip_nrms.clear();
	clip_drag = -1;
	clip_state = 0;
	clip_hover_valid = false;
	grid_hover_valid = false;
	modal = MODAL_CLIP;
	update_overlays();
}

// A clip point under the mouse: a vertex, an edge, the surface (on the snap grid with snapping on) or, off the
// meshes, the view plane through the last point; Ctrl: just the surface. r_n: the surface's normal there.
bool HewnEditorPlugin::_clip_pick(Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_point, Vector3 &r_n, bool p_free) const {
	Vector3 o, d, hit, nrm;
	_ray(p_cam, p_pos, o, d);
	r_n = Vector3();
	const bool on_mesh = _raycast_scene(o, d, hit, nrm);
	if (on_mesh) {
		r_n = nrm.normalized();
	}
	if (p_free && on_mesh) {
		r_point = hit;
		return true;
	}
	if (!p_free && _magnet_target(p_cam, p_pos, r_point)) {
		const bool grid = dock && dock->snap_enabled && dock->snap_enabled->is_pressed();
		const float st = dock && dock->snap_move ? MAX(1e-4f, (float)dock->snap_move->get_value()) : 1.0f;
		if (grid && last_snap_kind == SNAP_KIND_FACE && r_n.length() > 0.5f) {
			// on the grid of the snap step, then back onto the surface
			const Vector3 g = (r_point / st).round() * st;
			r_point = g - r_n * r_n.dot(g - r_point);
		} else if (grid && last_snap_kind == SNAP_KIND_EDGE) {
			// along the edge in grid steps (where the edge crosses the grid along its main axis), not anywhere on it
			for (const std::pair<Vector3, Vector3> &e : cand_edges) {
				const Vector3 ab = e.second - e.first;
				const float l2 = ab.length_squared();
				if (l2 < 1e-12f) {
					continue;
				}
				const float s = (r_point - e.first).dot(ab) / l2;
				if (s < -1e-4f || s > 1.0001f || (e.first + ab * s).distance_to(r_point) > 1e-4f) {
					continue;
				}
				const Vector3 aa = ab.abs();
				const int k = aa.x >= aa.y && aa.x >= aa.z ? 0 : (aa.y >= aa.z ? 1 : 2);
				const float c = std::round(r_point[k] / st) * st;
				const float t = CLAMP((c - e.first[k]) / ab[k], 0.0f, 1.0f);
				r_point = e.first + ab * t;
				break;
			}
		}
		return true;
	}
	if (!clip_pts.empty()) {
		return _plane_hit(p_cam, p_pos, clip_pts.back(), -p_cam->get_global_transform().basis.get_column(2).normalized(), r_point);
	}
	return false;
}

// The clip plane: through three points, or through two and along the surface they lie on
// (straight into it), or along the view. Its normal looks at the camera: the front is the near side.
// Only the placed points: the preview stays put while the mouse looks for the next one.
bool HewnEditorPlugin::_clip_plane(Vector3 &r_point, Vector3 &r_normal) const {
	const std::vector<Vector3> &p = clip_pts;
	const std::vector<Vector3> &nr = clip_nrms;
	if (p.size() < 2) {
		return false;
	}
	Camera3D *cam = _viewport_camera();
	const Vector3 fwd = cam ? -cam->get_global_transform().basis.get_column(2).normalized() : Vector3(0, 0, -1);
	Vector3 n;
	if (p.size() >= 3) {
		n = (p[1] - p[0]).cross(p[2] - p[0]);
	}
	if (n.length() < 1e-6f) {
		Vector3 along;
		for (const Vector3 &x : nr) {
			along += x;
		}
		const Vector3 line = p[1] - p[0];
		if (along.length() < 1e-4f || std::fabs(along.normalized().dot(line.normalized())) > 0.999f) {
			along = fwd;
		}
		n = line.cross(along.normalized());
	}
	if (n.length() < 1e-6f) {
		return false;
	}
	n.normalize();
	if (cam && n.dot(cam->get_global_position() - p[0]) < 0.0f) {
		n = -n;
	}
	r_point = p[0];
	r_normal = n;
	return true;
}

int32_t HewnEditorPlugin::_clip_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	const Key code = k.is_valid() && k->is_pressed() && !k->is_echo() ? key_code(k) : KEY_NONE;
	if (code == KEY_ESCAPE || (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT)) {
		cancel_tool();
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if ((code == KEY_BACKSPACE || code == KEY_DELETE) && !clip_pts.empty()) {
		clip_pts.pop_back(); // (the last point again)
		clip_nrms.pop_back();
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if ((code == KEY_ENTER || code == KEY_KP_ENTER) && (k->is_ctrl_pressed() || k->is_meta_pressed())) {
		// which part stays: the back, the front, both (only split)
		clip_mode = clip_mode == 1 ? 2 : clip_mode == 2 ? 0 : 1;
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if ((code == KEY_ENTER || code == KEY_KP_ENTER) && clip_pts.size() >= 2) {
		clip_hover_valid = false;
		_apply_clip();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mm.is_valid()) {
		const bool free = mm->is_ctrl_pressed() || mm->is_meta_pressed();
		if (clip_drag >= 0 && clip_drag < (int)clip_pts.size()) {
			// a placed point dragged with the left button
			Vector3 p, n;
			if (_clip_pick(p_cam, mouse_pos, p, n, free)) {
				clip_pts[clip_drag] = p;
				clip_nrms[clip_drag] = n;
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		clip_hover_valid = _clip_pick(p_cam, mouse_pos, clip_hover, clip_hover_n, free);
		update_overlays();
		return AFTER_GUI_INPUT_PASS; // (the view still turns / pans with the middle button)
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && !mb->is_pressed() && clip_drag >= 0) {
		clip_drag = -1;
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && mb->is_pressed()) {
		// on a placed point: it is dragged
		const float grab = 9.0f * EditorInterface::get_singleton()->get_editor_scale();
		for (int i = 0; i < (int)clip_pts.size(); i++) {
			if (!p_cam->is_position_behind(clip_pts[i]) && p_cam->unproject_position(clip_pts[i]).distance_to(mb->get_position()) < grab) {
				clip_drag = i;
				clip_hover_valid = false;
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
		}
		Vector3 p, n;
		if (_clip_pick(p_cam, mb->get_position(), p, n, mb->is_ctrl_pressed() || mb->is_meta_pressed())) {
			if (clip_pts.size() >= 3) {
				clip_pts.back() = p; // (a fourth click moves the third point)
				clip_nrms.back() = n;
			} else {
				clip_pts.push_back(p);
				clip_nrms.push_back(n);
			}
		}
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	return AFTER_GUI_INPUT_PASS;
}

void HewnEditorPlugin::_apply_clip() {
	Vector3 point, normal;
	clip_hover_valid = false;
	const bool ok = _clip_plane(point, normal);
	clip_state = 0;
	clip_pts.clear();
	clip_nrms.clear();
	modal = MODAL_NONE;
	if (!ok) {
		update_overlays();
		return;
	}
	PackedInt32Array faces;
	const std::vector<HewnMesh3D *> targets = _level_targets(&faces);
	clip_point = point;
	clip_normal = normal;
	clip_faces.clear();
	for (int64_t i = 0; i < faces.size(); i++) {
		clip_faces.push_back(faces[i]);
	}
	clip_nodes.clear();
	clip_before.clear();
	clip_after.clear();
	for (HewnMesh3D *n : targets) {
		const Transform3D g = n->get_global_transform();
		const Vector3 lp = g.affine_inverse().xform(point);
		const Vector3 ln = g.basis.transposed().xform(normal).normalized();
		bool pos = false, neg = false;
		for (const Vector3 &q : n->get_data().pos) {
			const float s = ln.dot(q - lp);
			pos = pos || s > 1e-5f;
			neg = neg || s < -1e-5f;
		}
		if (!pos || !neg) {
			continue; // the plane misses it
		}
		clip_nodes.push_back(ObjectID(n->get_instance_id()));
		clip_before.push_back(n->get_state());
	}
	set_op_param("clip", "mode", clip_mode);
	_clip_run(false);
}

// Clips the meshes (from their state before it) with the current mode; the first time it makes the
// history step, after that it rewrites it.
void HewnEditorPlugin::_clip_run(bool p_redo) {
	const int cm = CLAMP((int)op_param("clip", "mode"), 0, 2);
	clip_mode = cm; // (the tool starts with it next time)
	EditorUndoRedoManager *ur = get_undo_redo();
	std::vector<HewnMesh3D *> changed;
	std::vector<std::pair<int, std::vector<int>>> loops;
	bool any = false;
	HewnMesh3D *first = nullptr;
	for (size_t k = 0; k < clip_nodes.size(); k++) {
		HewnMesh3D *n = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(clip_nodes[k]));
		if (!n) {
			continue;
		}
		const Transform3D g = n->get_global_transform();
		const Vector3 lp = g.affine_inverse().xform(clip_point);
		const Vector3 ln = g.basis.transposed().xform(clip_normal).normalized();
		n->set_state(clip_before[k]);
		hewn::Mesh &m = n->get_data();
		// a closed mesh stays closed (a brush clipped: the cut gets its face)
		const bool closed = hewn::ops::holes(m).empty();
		hewn::ops::bisect(m, clip_faces, lp, ln, cm == 2, cm == 1, closed && cm != 0);
		n->commit(true);
		if (p_redo) {
			if (k < clip_after.size()) {
				clip_after[k].clear();
				clip_after[k].merge(n->get_state());
			}
		} else {
			if (!any) {
				ur->create_action("Hewn: Clip", UndoRedo::MERGE_DISABLE, n);
				any = true;
				first = n;
			}
			const Dictionary after = n->get_state();
			clip_after.push_back(after);
			ur->add_do_method(n, "set_state", after);
			ur->add_undo_method(n, "set_state", clip_before[k]);
		}
		if (cm != 0) {
			const int idx = (int)changed.size();
			changed.push_back(n);
			const hewn::Mesh &mm = n->get_data();
			// the holes the clip leaves: border loops lying on the plane
			for (const std::vector<int> &l : hewn::ops::holes(mm)) {
				bool on = l.size() >= 3;
				for (int v : l) {
					on = on && std::fabs(ln.dot(mm.pos[v] - lp)) < 1e-4f;
				}
				if (on) {
					loops.push_back({ idx, l });
				}
			}
		}
	}
	if (any) {
		ur->commit_action(false);
		UndoRedo *h = ur->get_history_undo_redo(ur->get_object_history_id(first));
		clip_version = h ? (int64_t)h->get_version() : -1;
		_hide_redo();
		last_op = "clip";
		_show_redo();
	}
	pending_action = String();
	if (!loops.empty()) {
		_arm_pending_loops("fill", changed, loops);
	}
	dock->refresh();
	selection_changed();
	update_overlays();
}

bool HewnEditorPlugin::_clip_panel_valid() {
	HewnMesh3D *n = clip_nodes.empty() ? nullptr : Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(clip_nodes[0]));
	if (!n || clip_after.size() != clip_nodes.size()) {
		return false;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	UndoRedo *h = ur->get_history_undo_redo(ur->get_object_history_id(n));
	return h && (int64_t)h->get_version() == clip_version;
}

void HewnEditorPlugin::_draw_clip(Control *p_overlay, Camera3D *p_cam) {
	Ref<Font> font = p_overlay->get_theme_default_font();
	const char *modes[3] = { "split only", "keep the back", "keep the front" };
	_draw_snap_mark(p_overlay, p_cam);
	Vector3 point, normal;
	if (_clip_plane(point, normal)) {
		// what goes: red; where the plane cuts: the tool's color
		PackedInt32Array faces;
		for (HewnMesh3D *n : _level_targets(&faces)) {
			const Transform3D g = n->get_global_transform();
			const hewn::Mesh &m = n->get_data();
			std::unordered_set<uint64_t> seen;
			for (const hewn::Face &F : m.faces) {
				std::vector<Vector3> on;
				for (int i = 0; i < F.size(); i++) {
					const int a = F.v[i], b = F.v[(i + 1) % F.size()];
					Vector3 pa = g.xform(m.pos[a]), pb = g.xform(m.pos[b]);
					float da = normal.dot(pa - point), db = normal.dot(pb - point);
					if ((da < 0.0f) != (db < 0.0f) && std::fabs(da - db) > 1e-9f) {
						on.push_back(pa.lerp(pb, da / (da - db)));
					}
					if (!seen.insert(hewn::ekey(a, b)).second) {
						continue;
					}
					if (clip_mode == 2) {
						da = -da;
						db = -db;
					}
					if (clip_mode == 0 || (da <= 0.0f && db <= 0.0f)) {
						continue;
					}
					if (da < 0.0f) {
						pa = pa.lerp(pb, da / (da - db));
					} else if (db < 0.0f) {
						pb = pb.lerp(pa, db / (db - da));
					}
					line3d(p_overlay, p_cam, pa, pb, Color(1.0f, 0.25f, 0.2f, 0.9f), 2.0f);
				}
				for (size_t i = 0; i + 1 < on.size(); i += 2) {
					line3d(p_overlay, p_cam, on[i], on[i + 1], TOOL_COLOR, 2.0f);
				}
			}
		}
	}
	// the points (numbered) and the lines between them; where the next one goes under the mouse (a dot only)
	std::vector<Vector3> pts = clip_pts;
	for (size_t i = 0; pts.size() > 1 && i < pts.size(); i++) {
		line3d(p_overlay, p_cam, pts[i], pts[(i + 1) % pts.size()], Color(1, 1, 1, 0.6f), 1.0f);
	}
	if (clip_hover_valid && pts.size() < 3 && clip_drag < 0) {
		pts.push_back(clip_hover);
	}
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	for (size_t i = 0; i < pts.size(); i++) {
		if (p_cam->is_position_behind(pts[i])) {
			continue;
		}
		const Vector2 s2 = p_cam->unproject_position(pts[i]);
		const bool placed = i < clip_pts.size();
		p_overlay->draw_circle(s2, 5.0f * es, Color(0, 0, 0, 0.8f));
		p_overlay->draw_circle(s2, 4.0f * es, placed ? TOOL_COLOR : Color(1, 1, 1, 0.7f));
		if (placed) {
			p_overlay->draw_string(font, s2 + Vector2(7, -7) * es, itos(i + 1), HORIZONTAL_ALIGNMENT_LEFT, -1, p_overlay->get_theme_default_font_size(), Color(1, 1, 1));
		}
	}
	String head = clip_pts.size() < 2 ? T("Clip: click 2 or 3 points") : T("Clip: Enter applies, a third click sets the plane's tilt");
	head += "   |   " + T("Ctrl+Enter: ") + T(modes[clip_mode]);
	draw_header(p_overlay, font, head);
}

// ------------------------------------------------------------------ texture painting

// Where face `p_face` takes the texture from: the face given it last or another of this run next to it
// (sharing an edge), else the selected face. Alt+clicks face by face give what one drag over them gives.
int HewnEditorPlugin::_tex_source(HewnMesh3D *p_node, int p_sel, int p_face) {
	if (tex_chain_sel != p_sel || tex_chain_mesh != (uint64_t)p_node->get_instance_id()) {
		tex_chain.clear();
		tex_chain_sel = p_sel;
		tex_chain_mesh = (uint64_t)p_node->get_instance_id();
		tex_last = -1;
	}
	const hewn::Mesh &m = p_node->get_data();
	auto adjacent = [&](int a, int b) {
		if (a < 0 || b < 0 || a >= (int)m.faces.size() || b >= (int)m.faces.size()) {
			return false;
		}
		const hewn::Face &A = m.faces[a], &B = m.faces[b];
		for (int i = 0; i < A.size(); i++) {
			const int x = A.v[i], y = A.v[(i + 1) % A.size()];
			for (int k = 0; k < B.size(); k++) {
				if ((B.v[k] == y && B.v[(k + 1) % B.size()] == x) || (B.v[k] == x && B.v[(k + 1) % B.size()] == y)) {
					return true;
				}
			}
		}
		return false;
	};
	if (adjacent(tex_last, p_face) && tex_last != p_face) {
		return tex_last;
	}
	if (adjacent(p_sel, p_face)) {
		return p_sel;
	}
	for (int i = (int)tex_chain.size() - 1; i >= 0; i--) {
		if (tex_chain[i] != p_face && adjacent(tex_chain[i], p_face)) {
			return tex_chain[i];
		}
	}
	return p_sel;
}

// -1: not ours (the event goes on to the other tools).
int32_t HewnEditorPlugin::_texture_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	HewnMesh3D *n = get_edited();
	if (!n || mode != MODE_FACE || (modal != MODAL_NONE && !tex_paint)) {
		return -1;
	}
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	Ref<InputEventKey> k = p_event;
	const Transform3D xf = n->get_global_transform();
	auto face_at = [&](const Vector2 &p_pos) {
		Vector3 o, d;
		_ray(p_cam, p_pos, o, d);
		return _raycast_face(o, d, nullptr);
	};
	if (tex_paint) {
		if (mm.is_valid()) {
			const int f = face_at(mm->get_position());
			if (f >= 0 && f != tex_last && !n->selection.faces.empty() && f != n->selection.faces[0]) {
				// (from the last one, or one before next to it: a chain runs on)
				hewn::wrap_uv(n->get_data(), _tex_source(n, n->selection.faces[0], f), f, xf, !tex_uv_only);
				tex_last = f;
				tex_chain.push_back(f);
				n->rebuild();
			}
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && !mb->is_pressed()) {
			tex_paint = false;
			EditorUndoRedoManager *ur = get_undo_redo();
			ur->create_action("Hewn: Apply Texture", UndoRedo::MERGE_DISABLE, n);
			ur->add_do_method(n, "set_state", n->get_state());
			ur->add_undo_method(n, "set_state", tex_before);
			ur->commit_action(false);
			dock->refresh();
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (k.is_valid() && k->is_pressed() && !k->is_echo() && key_code(k) == KEY_T && !k->is_ctrl_pressed() && !k->is_alt_pressed() &&
			!k->is_shift_pressed() && !n->selection.faces.empty()) {
		start_tsnap(p_cam);
		return AFTER_GUI_INPUT_STOP;
	}
	if (k.is_valid() && k->is_pressed() && !k->is_echo() && key_code(k) == KEY_T && k->is_shift_pressed() && !k->is_ctrl_pressed() && !k->is_alt_pressed() &&
			!n->selection.faces.empty()) {
		start_uv_transform(p_cam);
		return AFTER_GUI_INPUT_STOP;
	}
	if (!mb.is_valid() || !mb->is_pressed() || !mb->is_alt_pressed() || mb->is_shift_pressed() || n->selection.faces.size() != 1) {
		return -1;
	}
	const int sel = n->selection.faces[0];
	// another Hewn mesh in front: the texture goes over to it (or comes from it)
	{
		Vector3 o, d;
		_ray(p_cam, mb->get_position(), o, d);
		float own_t = 1e30f;
		if (_raycast_face(o, d, &own_t) < 0) {
			own_t = 1e30f;
		}
		HewnMesh3D *other = nullptr;
		int other_f = -1;
		float best = own_t;
		for (HewnMesh3D *m : _scene_meshes()) {
			if (m == n || m->is_csg_model()) {
				continue;
			}
			float t;
			const int hf = raycast_node(m, o, d, t, nullptr, cull_back());
			if (hf >= 0 && t < best - 1e-4f) {
				best = t;
				other = m;
				other_f = hf;
			}
		}
		const bool uv_only = mb->is_ctrl_pressed() || mb->is_meta_pressed();
		if (other && (mb->get_button_index() == MOUSE_BUTTON_LEFT || mb->get_button_index() == MOUSE_BUTTON_RIGHT)) {
			const bool to_other = mb->get_button_index() == MOUSE_BUTTON_LEFT;
			HewnMesh3D *dst = to_other ? other : n;
			const Dictionary before = dst->get_state();
			const TypedArray<Material> mats_before = dst->get_materials().duplicate();
			if (to_other) {
				_texture_across(n, sel, other, other_f, !uv_only);
			} else {
				_texture_across(other, other_f, n, sel, !uv_only);
			}
			EditorUndoRedoManager *ur = get_undo_redo();
			ur->create_action(to_other ? "Hewn: Apply Texture" : "Hewn: Pick Texture", UndoRedo::MERGE_DISABLE, dst);
			ur->add_do_method(dst, "set_materials", dst->get_materials().duplicate());
			ur->add_do_method(dst, "set_state", dst->get_state());
			ur->add_undo_method(dst, "set_materials", mats_before);
			ur->add_undo_method(dst, "set_state", before);
			ur->commit_action(false);
			dock->refresh();
			return AFTER_GUI_INPUT_STOP;
		}
	}
	const int f = face_at(mb->get_position());
	if (f < 0 || f == sel) {
		return -1;
	}
	if (mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		tex_before = n->get_state();
		tex_uv_only = mb->is_ctrl_pressed() || mb->is_meta_pressed();
		tex_paint = true;
		hewn::wrap_uv(n->get_data(), _tex_source(n, sel, f), f, xf, !tex_uv_only);
		tex_last = f;
		tex_chain.push_back(f);
		n->rebuild();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb->get_button_index() == MOUSE_BUTTON_RIGHT) {
		// pick: the selected face takes the texture of the clicked one
		const Dictionary before = n->get_state();
		hewn::wrap_uv(n->get_data(), f, sel, xf, !(mb->is_ctrl_pressed() || mb->is_meta_pressed()));
		n->rebuild();
		EditorUndoRedoManager *ur = get_undo_redo();
		ur->create_action("Hewn: Pick Texture", UndoRedo::MERGE_DISABLE, n);
		ur->add_do_method(n, "set_state", n->get_state());
		ur->add_undo_method(n, "set_state", before);
		ur->commit_action(false);
		dock->refresh();
		return AFTER_GUI_INPUT_STOP;
	}
	return -1;
}

// Alt+click on another object: face `p_df` of `p_dst` gets the texture of `p_sf` of `p_src`. On the
// same plane the texture runs on in the world; over an edge both faces share in space it folds over that
// edge; else the settings are copied.
void HewnEditorPlugin::_texture_across(HewnMesh3D *p_src, int p_sf, HewnMesh3D *p_dst, int p_df, bool p_material) {
	const hewn::Mesh &sm = p_src->get_data();
	hewn::Mesh &dm = p_dst->get_data();
	if (p_sf < 0 || p_sf >= (int)sm.faces.size() || p_df < 0 || p_df >= (int)dm.faces.size()) {
		return;
	}
	const Transform3D gs = p_src->get_global_transform(), gd = p_dst->get_global_transform();
	const hewn::Face &S = sm.faces[p_sf];
	hewn::Face &D = dm.faces[p_df];
	if (p_material) {
		D.material = p_dst->add_material_slot(p_src->get_material_for_slot(S.material));
	}
	D.uvp = S.uvp;
	D.manual_uv = false;
	D.uv_group = 0;
	// the source's UVs as an affine function of the world on its plane
	std::vector<std::vector<Vector2>> uvs;
	hewn::compute_uvs(sm, gs, uvs);
	const Vector3 ns = gs.basis.inverse().transposed().xform(hewn::face_normal(sm, S)).normalized();
	Vector3 e1, e2;
	hewn::plane_basis(ns, e1, e2);
	const int c = S.size();
	std::vector<Vector3> ps(c);
	Vector2 xc, uc;
	for (int i = 0; i < c; i++) {
		ps[i] = gs.xform(sm.pos[S.v[i]]);
		xc += Vector2(ps[i].dot(e1), ps[i].dot(e2)) / (float)c;
		uc += uvs[p_sf][i] / (float)c;
	}
	double a00 = 0, a01 = 0, a11 = 0, b00 = 0, b01 = 0, b10 = 0, b11 = 0;
	for (int i = 0; i < c; i++) {
		const Vector2 x = Vector2(ps[i].dot(e1), ps[i].dot(e2)) - xc, u = uvs[p_sf][i] - uc;
		a00 += x.x * x.x;
		a01 += x.x * x.y;
		a11 += x.y * x.y;
		b00 += u.x * x.x;
		b01 += u.x * x.y;
		b10 += u.y * x.x;
		b11 += u.y * x.y;
	}
	const double det = a00 * a11 - a01 * a01;
	const Vector3 nd = gd.basis.inverse().transposed().xform(hewn::face_normal(dm, D)).normalized();
	if (std::fabs(det) > 1e-14 && !S.manual_uv) {
		const double i00 = a11 / det, i01 = -a01 / det, i11 = a00 / det;
		const Vector2 r0((float)(b00 * i00 + b01 * i01), (float)(b00 * i01 + b01 * i11));
		const Vector2 r1((float)(b10 * i00 + b11 * i01), (float)(b10 * i01 + b11 * i11));
		auto uv_at = [&](const Vector3 &p) {
			const Vector2 x = Vector2(p.dot(e1), p.dot(e2)) - xc;
			return Vector2(r0.dot(x), r1.dot(x)) + uc;
		};
		// onto the source plane: the same plane, or unfolded over a shared edge
		bool mapped = false;
		Transform3D unfold;
		if (std::fabs(nd.dot(ns)) > 0.999f && std::fabs(ns.dot(gd.xform(dm.pos[D.v[0]]) - ps[0])) < 1e-3f) {
			mapped = true;
		} else {
			for (int i = 0; i < c && !mapped; i++) {
				const Vector3 a = ps[i], b = ps[(i + 1) % c];
				const Vector3 ab = b - a;
				const float len = ab.length();
				if (len < 1e-6f) {
					continue;
				}
				const Vector3 axis = ab / len;
				int on = 0;
				for (int v : D.v) {
					const Vector3 q = gd.xform(dm.pos[v]);
					const float t = (q - a).dot(axis);
					if (t > -1e-3f && t < len + 1e-3f && (q - a - axis * t).length() < 1e-3f) {
						on++;
					}
				}
				if (on >= 2) {
					const float ang = std::atan2(nd.cross(ns).dot(axis), nd.dot(ns));
					unfold = Transform3D(Basis(axis, ang), a - Basis(axis, ang).xform(a));
					mapped = true;
				}
			}
		}
		if (mapped) {
			std::vector<Vector2> du;
			for (int v : D.v) {
				const Vector3 q = unfold.xform(gd.xform(dm.pos[v]));
				du.push_back(uv_at(q - ns * ns.dot(q - ps[0])));
			}
			// (the materials may tile differently)
			Ref<BaseMaterial3D> ms = p_src->get_material_for_slot(S.material), md = p_dst->get_material_for_slot(D.material);
			if (ms.is_valid() && md.is_valid()) {
				const Vector3 a = ms->get_uv1_scale(), b = md->get_uv1_scale();
				for (Vector2 &q : du) {
					q *= Vector2(a.x / MAX(1e-6f, b.x), a.y / MAX(1e-6f, b.y));
				}
			}
			hewn::set_face_uvs(dm, p_df, gd, du);
		}
	}
	p_dst->rebuild();
}

void HewnEditorPlugin::uv_tool(const String &p_kind) {
	std::vector<HewnMesh3D *> targets;
	PackedInt32Array faces;
	HewnMesh3D *edited = get_edited();
	if (edited && mode != MODE_OBJECT) {
		targets.push_back(edited);
		if (mode == MODE_FACE) {
			for (int f : edited->selection.faces) {
				faces.push_back(f);
			}
		}
	} else {
		targets = _level_targets(nullptr);
	}
	if (targets.empty()) {
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: UV " + p_kind.get_slice(":", 0).capitalize(), UndoRedo::MERGE_DISABLE, targets[0]);
	uint32_t seed = 1234567u;
	for (HewnMesh3D *n : targets) {
		const Dictionary before = n->get_state();
		hewn::Mesh &m = n->get_data();
		std::vector<int> fs;
		for (int64_t i = 0; i < faces.size(); i++) {
			fs.push_back(faces[i]);
		}
		if (fs.empty()) {
			for (int f = 0; f < (int)m.faces.size(); f++) {
				fs.push_back(f);
			}
		}
		std::vector<std::vector<Vector2>> uvs;
		hewn::compute_uvs(m, n->get_global_transform(), uvs);
		for (int f : fs) {
			if (f < 0 || f >= (int)m.faces.size()) {
				continue;
			}
			hewn::Face &F = m.faces[f];
			F.manual_uv = false;
			// the material's own tiling (pixels per meter): one tile is 1 / uv1_scale in face UVs
			Vector2 ms(1, 1);
			Ref<BaseMaterial3D> bm = n->get_material_for_slot(F.material);
			if (bm.is_valid()) {
				ms = Vector2(MAX(1e-6f, bm->get_uv1_scale().x), MAX(1e-6f, bm->get_uv1_scale().y));
			}
			if (p_kind.begins_with("box:")) {
				// plain projection from the face's side, at a scale
				const float sc = MAX(1e-4f, (float)p_kind.get_slice(":", 1).to_float());
				F.uv_group = 0;
				F.uvp = hewn::UVParams();
				F.uvp.scale = Vector2(sc, sc);
			} else if (p_kind == "fit" && F.size() >= 3) {
				// one tile over the whole face: its bounds along its edge nearest to the projection's U (a
				// wall's bottom edge, a floor's side), the corners of a rectangle on the tile's corners
				const Vector3 nn = hewn::face_normal(m, F).normalized();
				Vector3 ua, va;
				hewn::uv_axes(nn, ua, va);
				Vector3 u = ua;
				float best = -1.0f;
				for (int i = 0; i < F.size(); i++) {
					Vector3 e = m.pos[F.v[(i + 1) % F.size()]] - m.pos[F.v[i]];
					e -= nn * e.dot(nn);
					if (e.length() < 1e-6f) {
						continue;
					}
					e.normalize();
					const float d = std::fabs(e.dot(ua));
					if (d > best + 1e-4f) {
						best = d;
						u = e.dot(ua) < 0.0f ? -e : e;
					}
				}
				Vector3 vv = nn.cross(u).normalized();
				if (vv.dot(va) < 0.0f) {
					vv = -vv;
				}
				std::vector<Vector2> q(F.size());
				Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
				for (int i = 0; i < F.size(); i++) {
					const Vector3 &p = m.pos[F.v[i]];
					q[i] = Vector2(p.dot(u), -p.dot(vv));
					mn = mn.min(q[i]);
					mx = mx.max(q[i]);
				}
				const Vector2 size(MAX(1e-6f, mx.x - mn.x), MAX(1e-6f, mx.y - mn.y));
				F.uv.resize(F.size());
				for (int i = 0; i < F.size(); i++) {
					F.uv[i] = Vector2((q[i].x - mn.x) / size.x / ms.x, (q[i].y - mn.y) / size.y / ms.y);
				}
				F.manual_uv = true;
			} else if (p_kind == "center" && f < (int)uvs.size() && !uvs[f].empty()) {
				Vector2 mn = uvs[f][0], mx = mn;
				for (const Vector2 &q : uvs[f]) {
					mn = mn.min(q);
					mx = mx.max(q);
				}
				const Vector2 c = (mn + mx) * 0.5f * ms; // in tiles
				const Vector2 frac(c.x - std::floor(c.x), c.y - std::floor(c.y));
				F.uvp.offset += Vector2((0.5f - frac.x) / ms.x, (0.5f - frac.y) / ms.y);
			} else if (p_kind == "random") {
				auto rnd = [&]() {
					seed = seed * 1664525u + 1013904223u;
					return (float)(seed >> 8) / (float)(1u << 24);
				};
				F.uvp.offset += Vector2(rnd() / ms.x, rnd() / ms.y);
			}
		}
		n->rebuild();
		ur->add_do_method(n, "set_state", n->get_state());
		ur->add_undo_method(n, "set_state", before);
	}
	ur->commit_action(false);
	dock->refresh();
}

// ------------------------------------------------------------------ tool materials, face grid, checks

Ref<Material> HewnEditorPlugin::tool_material(const String &p_kind) {
	const String dir = texture_dock ? texture_dock->material_dir() : String("res://materials");
	const String file = dir + String("/hw_tool_") + p_kind + String(".tres");
	if (FileAccess::file_exists(file)) {
		Ref<Material> m = ResourceLoader::get_singleton()->load(file);
		if (m.is_valid()) {
			return m;
		}
	}
	Ref<StandardMaterial3D> m;
	m.instantiate();
	const Color c = p_kind == "trigger" ? Color(1.0f, 0.55f, 0.1f, 0.35f) : (p_kind == "clip" ? Color(0.7f, 0.3f, 1.0f, 0.35f) : Color(1.0f, 0.9f, 0.2f, 0.35f));
	m->set_albedo(c);
	m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	m->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	m->set_meta("hw_tool", p_kind);
	DirAccess::make_dir_recursive_absolute(dir);
	if (ResourceSaver::get_singleton()->save(m, file) == OK) {
		if (EditorFileSystem *efs = EditorInterface::get_singleton()->get_resource_filesystem()) {
			efs->update_file(file);
		}
		Ref<Material> saved = ResourceLoader::get_singleton()->load(file);
		if (saved.is_valid()) {
			return saved;
		}
	}
	return m;
}

void HewnEditorPlugin::apply_tool_material(const String &p_kind) {
	if (texture_dock && texture_dock->ask_material_dir(callable_mp(this, &HewnEditorPlugin::apply_tool_material).bind(p_kind))) {
		return;
	}
	apply_material_to_selection(tool_material(p_kind));
}

void HewnEditorPlugin::set_face_grid(bool p_on) {
	face_grid = p_on;
	if (dock && dock->ov_face_grid && dock->ov_face_grid->is_pressed() != p_on) {
		dock->ov_face_grid->set_pressed_no_signal(p_on);
	}
	_update_face_grid();
}

void HewnEditorPlugin::_update_face_grid() {
	if (face_grid && face_grid_material.is_null()) {
		Ref<Shader> sh;
		sh.instantiate();
		sh->set_code(R"(shader_type spatial;
render_mode unshaded, depth_draw_never, cull_back, shadows_disabled;
uniform float step = 0.25;
uniform vec4 color : source_color = vec4(1.0, 1.0, 1.0, 0.3);
varying vec3 wpos;
void vertex() {
	wpos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}
float lines(vec2 p) {
	vec2 g = abs(fract(p - 0.5) - 0.5) / max(fwidth(p), vec2(1e-5));
	return 1.0 - min(min(g.x, g.y), 1.0);
}
void fragment() {
	// in the face's own plane, as the grid of a box drawn on it (world X along it, else Z): a slanted face is
	// not stretched
	vec3 n = normalize(cross(dFdx(wpos), dFdy(wpos)));
	vec3 u = vec3(1.0, 0.0, 0.0) - n * n.x;
	if (length(u) < 0.3) {
		u = vec3(0.0, 0.0, 1.0) - n * n.z;
	}
	u = normalize(u);
	vec3 v = normalize(cross(n, u));
	vec2 p = vec2(dot(wpos, u), dot(wpos, v));
	float minor = lines(p / step);
	float major = lines(p / (step * 8.0));
	ALBEDO = color.rgb;
	ALPHA = color.a * max(minor * 0.6, major);
}
)");
		face_grid_material.instantiate();
		face_grid_material->set_shader(sh);
	}
	if (face_grid_material.is_valid()) {
		const float st = dock && dock->snap_move ? MAX(0.01f, (float)dock->snap_move->get_value()) : 0.25f;
		face_grid_material->set_shader_parameter("step", st);
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	const RID mat = face_grid && face_grid_material.is_valid() ? face_grid_material->get_rid() : RID();
	for (HewnMesh3D *n : _scene_meshes()) {
		const RID inst = n->get_instance();
		if (inst.is_valid()) {
			rs->instance_geometry_set_material_overlay(inst, mat.is_valid() ? mat : (n->get_material_overlay().is_valid() ? n->get_material_overlay()->get_rid() : RID()));
		}
	}
}

void HewnEditorPlugin::_update_library_dim() {
	HewnMesh3D *n = get_edited();
	const uint64_t key = n && mode != MODE_OBJECT ? (uint64_t)n->get_instance_id() : 0;
	if (key == dim_key) {
		return;
	}
	dim_key = key;
	RenderingServer *rs = RenderingServer::get_singleton();
	for (ObjectID id : dimmed) {
		if (GeometryInstance3D *g = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(id))) {
			if (g->get_instance().is_valid()) {
				rs->instance_geometry_set_material_overlay(g->get_instance(), g->get_material_overlay().is_valid() ? g->get_material_overlay()->get_rid() : RID());
			}
		}
	}
	dimmed.clear();
	if (face_grid) {
		_update_face_grid(); // (Hewn meshes inside placed scenes get the grid back)
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!key || !root) {
		return;
	}
	if (dim_material.is_null()) {
		dim_material.instantiate();
		dim_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		dim_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		dim_material->set_albedo(Color(0.1f, 0.11f, 0.14f, 0.75f)); // (darker: clearly not part of the work)
	}
	std::vector<std::pair<Node *, bool>> stack;
	for (int i = 0; i < root->get_child_count(); i++) {
		stack.push_back({ root->get_child(i), false });
	}
	while (!stack.empty()) {
		Node *x = stack.back().first;
		const bool placed = stack.back().second || !x->get_scene_file_path().is_empty();
		stack.pop_back();
		GeometryInstance3D *g = Object::cast_to<GeometryInstance3D>(x);
		if (placed && g && g != n && g->get_instance().is_valid()) {
			rs->instance_geometry_set_material_overlay(g->get_instance(), dim_material->get_rid());
			dimmed.push_back(ObjectID(g->get_instance_id()));
		}
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back({ x->get_child(i), placed });
		}
	}
}

// Faces lying on top of each other (the same plane, overlapping): z-fighting, or a wall left inside.
int HewnEditorPlugin::select_overlapping() {
	HewnMesh3D *n = get_edited();
	if (!n || mode == MODE_OBJECT) {
		return -1;
	}
	const hewn::Mesh &m = n->get_data();
	const int nf = (int)m.faces.size();
	std::vector<Vector3> nrm(nf), ctr(nf);
	for (int f = 0; f < nf; f++) {
		nrm[f] = hewn::face_normal(m, m.faces[f]);
		ctr[f] = hewn::face_center(m, m.faces[f]);
	}
	auto inside = [&](int f, const Vector3 &p) {
		// p (on the plane of f) inside the polygon of f
		const hewn::Face &F = m.faces[f];
		Vector3 u, v;
		hewn::plane_basis(nrm[f], u, v);
		const Vector2 q(p.dot(u), p.dot(v));
		bool in = false;
		for (int i = 0, j = F.size() - 1; i < F.size(); j = i++) {
			const Vector2 a(m.pos[F.v[i]].dot(u), m.pos[F.v[i]].dot(v)), b(m.pos[F.v[j]].dot(u), m.pos[F.v[j]].dot(v));
			if ((a.y > q.y) != (b.y > q.y) && q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x) {
				in = !in;
			}
		}
		return in;
	};
	std::vector<int> out;
	for (int f = 0; f < nf; f++) {
		for (int g = f + 1; g < nf; g++) {
			if (std::fabs(nrm[f].dot(nrm[g])) < 0.999f || std::fabs(nrm[f].dot(ctr[g] - ctr[f])) > 1e-3f) {
				continue;
			}
			if (inside(f, ctr[g]) || inside(g, ctr[f])) {
				out.push_back(f);
				out.push_back(g);
			}
		}
	}
	set_mode(MODE_FACE);
	n->selection.clear();
	n->selection.faces = hewn::sel::uniq(out);
	n->update_gizmos();
	dock->refresh();
	update_overlays();
	return (int)n->selection.faces.size();
}

// W after a non-destructive cut: the cutter lines its hole (a corridor between its two holes, an alcove behind
// its one hole), made again from what it cuts whenever the cutter or the meshes change.
bool HewnEditorPlugin::_line_cutter(bool p_corridor) {
	HewnCutter3D *c = Object::cast_to<HewnCutter3D>(ObjectDB::get_instance(pending_cutter));
	if (!c) {
		return false;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(p_corridor ? "Hewn: Corridor" : "Hewn: Alcove", UndoRedo::MERGE_DISABLE, c);
	ur->add_do_method(c, "set_lining_depth", pending_depth);
	ur->add_do_method(c, "set_lining", (int)HewnCutter3D::LINING_AUTO);
	ur->add_undo_method(c, "set_lining", c->get_lining());
	ur->add_undo_method(c, "set_lining_depth", c->get_lining_depth());
	ur->commit_action();
	return true;
}

namespace {
// About how many bytes a value takes in the saved scene: resources saved inside it (no file of their own,
// or a sub-resource "file.tscn::id") count with what they hold, once.
int64_t saved_size(const Variant &p_value, std::unordered_set<uint64_t> &r_seen, int p_depth) {
	if (p_value.get_type() == Variant::OBJECT) {
		Resource *r = Object::cast_to<Resource>((Object *)p_value);
		if (!r || p_depth > 8) {
			return 0;
		}
		const String path = r->get_path();
		if (!path.is_empty() && !path.contains("::")) {
			return path.length(); // (a file of its own: only a link)
		}
		if (!r_seen.insert((uint64_t)r->get_instance_id()).second) {
			return 8;
		}
		int64_t total = 0;
		const TypedArray<Dictionary> props = r->get_property_list();
		for (int64_t i = 0; i < props.size(); i++) {
			const Dictionary d = props[i];
			if ((int64_t)d["usage"] & PROPERTY_USAGE_STORAGE) {
				total += saved_size(r->get(d["name"]), r_seen, p_depth + 1);
			}
		}
		return total;
	}
	if (p_value.get_type() == Variant::ARRAY) {
		int64_t total = 0;
		const Array a = p_value;
		for (int64_t i = 0; i < a.size(); i++) {
			total += saved_size(a[i], r_seen, p_depth + 1);
		}
		return total;
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		int64_t total = 0;
		const Dictionary d = p_value;
		const Array keys = d.keys();
		for (int64_t i = 0; i < keys.size(); i++) {
			total += saved_size(keys[i], r_seen, p_depth + 1) + saved_size(d[keys[i]], r_seen, p_depth + 1);
		}
		return total;
	}
	return UtilityFunctions::var_to_bytes(p_value).size();
}
} // namespace

namespace {
String size_text(int64_t b) {
	return b >= 1024 * 1024 ? vformat("%.2f MB", b / 1048576.0) : b >= 1024 ? vformat("%.1f KB", b / 1024.0) : vformat("%d B", (int)b);
}

// A dictionary's keys (Hewn's mesh_data: positions, faces, UVs, colors...), biggest first, with what each is.
void dict_details(Tree *p_tree, TreeItem *p_parent, const Dictionary &p_dict, uint64_t p_node, int p_depth) {
	if (p_depth > 6) {
		return;
	}
	struct Row {
		int64_t size;
		String name;
		Variant value;
	};
	std::vector<Row> rows;
	const Array keys = p_dict.keys();
	for (int64_t i = 0; i < keys.size(); i++) {
		std::unordered_set<uint64_t> seen;
		rows.push_back({ saved_size(p_dict[keys[i]], seen, 0) + saved_size(keys[i], seen, 0), String(keys[i]), p_dict[keys[i]] });
	}
	std::sort(rows.begin(), rows.end(), [](const Row &x, const Row &y) { return x.size > y.size; });
	for (size_t i = 0; i < rows.size() && i < 64; i++) {
		const Variant &v = rows[i].value;
		String what = Variant::get_type_name(v.get_type());
		if (v.get_type() >= Variant::ARRAY && v.get_type() != Variant::OBJECT && v.get_type() != Variant::CALLABLE && v.get_type() != Variant::SIGNAL) {
			bool ok = false;
			const Variant n = v.get_type() == Variant::DICTIONARY ? Variant(Dictionary(v).size()) : Variant(v).call("size");
			ok = n.get_type() == Variant::INT;
			if (ok) {
				what += ", " + vformat(T(v.get_type() == Variant::DICTIONARY ? "%d keys" : "%d items"), (int)(int64_t)n);
			}
		} else if (v.get_type() != Variant::OBJECT) {
			const String s = String(v);
			what += ": " + (s.length() > 40 ? s.substr(0, 40) + "..." : s);
		}
		TreeItem *it = p_tree->create_item(p_parent);
		const String label = rows[i].name + "  (" + what + ")";
		it->set_text(0, label);
		it->set_tooltip_text(0, label);
		it->set_text(1, size_text(rows[i].size));
		it->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
		it->set_metadata(0, (int64_t)p_node);
		it->set_collapsed(true);
		if (v.get_type() == Variant::DICTIONARY) {
			dict_details(p_tree, it, v, p_node, p_depth + 1);
		}
	}
}

// What an object holds, biggest first, under its row: its saved properties, the resources saved inside it
// with theirs (a few levels down), the arrays' items of resources, the dictionaries' keys.
void size_details(Tree *p_tree, TreeItem *p_parent, Object *p_obj, uint64_t p_node, int p_depth) {
	if (!p_obj || p_depth > 4) {
		return;
	}
	struct Row {
		int64_t size;
		String name;
		Variant value;
	};
	std::vector<Row> rows;
	const TypedArray<Dictionary> props = p_obj->get_property_list();
	for (int64_t i = 0; i < props.size(); i++) {
		const Dictionary d = props[i];
		if (!((int64_t)d["usage"] & PROPERTY_USAGE_STORAGE)) {
			continue;
		}
		const String name = d["name"];
		const Variant v = p_obj->get(name);
		std::unordered_set<uint64_t> seen;
		const int64_t sz = saved_size(v, seen, 0);
		if (sz >= 16) {
			rows.push_back({ sz, name, v });
		}
	}
	std::sort(rows.begin(), rows.end(), [](const Row &x, const Row &y) { return x.size > y.size; });
	for (size_t i = 0; i < rows.size() && i < 24; i++) {
		TreeItem *it = p_tree->create_item(p_parent);
		String label = rows[i].name;
		Resource *r = rows[i].value.get_type() == Variant::OBJECT ? Object::cast_to<Resource>((Object *)rows[i].value) : nullptr;
		if (r) {
			const String path = r->get_path();
			label += "  (" + r->get_class() + (!path.is_empty() && !path.contains("::") ? ", " + path.get_file() : String(", ") + T("inside the scene")) + ")";
		} else if (rows[i].value.get_type() == Variant::ARRAY) {
			label += "  (" + vformat(T("%d items"), (int)Array(rows[i].value).size()) + ")";
		} else if (rows[i].value.get_type() == Variant::DICTIONARY) {
			label += "  (" + vformat(T("%d keys"), (int)Dictionary(rows[i].value).size()) + ")";
		}
		it->set_text(0, label);
		it->set_tooltip_text(0, label);
		it->set_text(1, size_text(rows[i].size));
		it->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
		it->set_metadata(0, (int64_t)p_node);
		it->set_collapsed(true);
		if (r && (r->get_path().is_empty() || r->get_path().contains("::"))) {
			size_details(p_tree, it, r, p_node, p_depth + 1);
		} else if (rows[i].value.get_type() == Variant::DICTIONARY) {
			dict_details(p_tree, it, rows[i].value, p_node, p_depth + 1);
		} else if (rows[i].value.get_type() == Variant::ARRAY) {
			const Array arr = rows[i].value;
			for (int64_t k = 0; k < arr.size() && k < 16; k++) {
				Resource *ar = Object::cast_to<Resource>((Object *)arr[k]);
				if (!ar) {
					continue;
				}
				std::unordered_set<uint64_t> seen;
				TreeItem *ai = p_tree->create_item(it);
				ai->set_text(0, vformat("[%d]  %s", (int)k, ar->get_class()));
				ai->set_text(1, size_text(saved_size(arr[k], seen, 0)));
				ai->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
				ai->set_metadata(0, (int64_t)p_node);
				ai->set_collapsed(true);
				size_details(p_tree, ai, ar, p_node, p_depth + 1);
			}
		}
	}
}
} // namespace

// Object tab, Scene Size: the nodes with the most saved data, biggest first; a row unfolds into what of the
// node takes the room (its properties, the meshes / materials saved inside it...), a click selects the node.
void HewnEditorPlugin::_scene_size_report() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return;
	}
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	if (!size_dialog) {
		size_dialog = memnew(AcceptDialog);
		size_dialog->set_name("HewnSceneSize");
		size_dialog->set_title(T("Scene Size"));
		VBoxContainer *v = memnew(VBoxContainer);
		size_dialog->add_child(v);
		size_label = memnew(Label);
		size_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		size_label->set_custom_minimum_size(Vector2(560, 0) * es);
		v->add_child(size_label);
		size_tree = memnew(Tree);
		size_tree->set_name("SizeTree");
		size_tree->set_columns(2);
		size_tree->set_column_expand(1, false);
		size_tree->set_column_custom_minimum_width(1, (int)(90 * es));
		size_tree->set_hide_root(true);
		size_tree->set_custom_minimum_size(Vector2(560, 360) * es);
		size_tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		size_tree->set_tooltip_text(T("A click selects the node; the arrow shows what in it takes the room"));
		size_tree->connect("item_selected", callable_mp(this, &HewnEditorPlugin::_size_item_selected));
		v->add_child(size_tree);
		Label *hint = memnew(Label);
		hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		hint->set_custom_minimum_size(Vector2(560, 0) * es);
		hint->set_text(T("Meshes, textures and materials saved inside the scene count with their node: save them as files (the inspector: Save), or bake big Hewn meshes. Hewn meshes keep their faces in the scene too."));
		v->add_child(hint);
		EditorInterface::get_singleton()->get_base_control()->add_child(size_dialog);
	}
	struct Row {
		int64_t size;
		Node *node;
	};
	std::vector<Row> rows;
	std::unordered_set<uint64_t> seen;
	int64_t total = 0;
	std::vector<Node *> stack{ root };
	while (!stack.empty()) {
		Node *n = stack.back();
		stack.pop_back();
		for (int i = 0; i < n->get_child_count(); i++) {
			stack.push_back(n->get_child(i));
		}
		if (n != root && n->get_owner() != root) {
			continue; // (inside an instanced scene: saved in its own file)
		}
		int64_t size = 0;
		const TypedArray<Dictionary> props = n->get_property_list();
		for (int64_t i = 0; i < props.size(); i++) {
			const Dictionary d = props[i];
			if ((int64_t)d["usage"] & PROPERTY_USAGE_STORAGE) {
				size += saved_size(n->get(d["name"]), seen, 0);
			}
		}
		total += size;
		rows.push_back({ size, n });
	}
	std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.size > b.size; });
	size_label->set_text(vformat(T("About %s of data in the scene (binary; a .tscn is larger). The biggest:"), size_text(total)));
	size_tree->clear();
	TreeItem *top = size_tree->create_item();
	for (size_t i = 0; i < rows.size() && i < 50; i++) {
		TreeItem *it = size_tree->create_item(top);
		const String label = String(root->get_path_to(rows[i].node)) + "  (" + rows[i].node->get_class() + ")";
		it->set_text(0, label);
		it->set_tooltip_text(0, label);
		it->set_text(1, size_text(rows[i].size));
		it->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
		it->set_metadata(0, (int64_t)rows[i].node->get_instance_id());
		it->set_collapsed(true);
		size_details(size_tree, it, rows[i].node, rows[i].node->get_instance_id(), 0);
	}
	size_dialog->reset_size();
	size_dialog->popup_centered();
}

void HewnEditorPlugin::_size_item_selected() {
	TreeItem *it = size_tree ? size_tree->get_selected() : nullptr;
	Node *n = it ? Object::cast_to<Node>(ObjectDB::get_instance(ObjectID((uint64_t)(int64_t)it->get_metadata(0)))) : nullptr;
	if (n && n->is_inside_tree()) {
		EditorInterface::get_singleton()->get_selection()->clear();
		EditorInterface::get_singleton()->get_selection()->add_node(n);
		EditorInterface::get_singleton()->edit_node(n);
	}
}

// curve tools on the selected Path3D nodes' curves (Godot edits single points itself).
void HewnEditorPlugin::curve_op(const String &p_op) {
	std::vector<Path3D *> paths;
	for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_selected_nodes()) {
		Path3D *p = Object::cast_to<Path3D>(v);
		if (p && p->get_curve().is_valid() && p->get_curve()->get_point_count() > 0) {
			paths.push_back(p);
		}
	}
	if (paths.empty()) {
		ERR_PRINT("Hewn: select a Path3D (with points) for the curve tools.");
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Curve " + p_op.capitalize());
	for (Path3D *path : paths) {
		const Ref<Curve3D> old = path->get_curve();
		Ref<Curve3D> c = old->duplicate();
		const int n = c->get_point_count();
		struct Pt {
			Vector3 p, in, out;
			float tilt;
		};
		std::vector<Pt> pts;
		for (int i = 0; i < n; i++) {
			pts.push_back({ c->get_point_position(i), c->get_point_in(i), c->get_point_out(i), c->get_point_tilt(i) });
		}
		const bool closed = c->is_closed();
		if (p_op == "subdivide") {
			// de Casteljau at the middle of each piece: the shape stays
			std::vector<Pt> out;
			const int segs = closed ? n : n - 1;
			for (int i = 0; i < n; i++) {
				out.push_back(pts[i]);
				if (i >= segs) {
					continue;
				}
				const Pt &a = pts[i], &b = pts[(i + 1) % n];
				const Vector3 p0 = a.p, p1 = a.p + a.out, p2 = b.p + b.in, p3 = b.p;
				const Vector3 q0 = (p0 + p1) * 0.5f, q1 = (p1 + p2) * 0.5f, q2 = (p2 + p3) * 0.5f;
				const Vector3 r0 = (q0 + q1) * 0.5f, r1 = (q1 + q2) * 0.5f, m = (r0 + r1) * 0.5f;
				out.back().out = q0 - p0;
				out.push_back({ m, r0 - m, r1 - m, (a.tilt + b.tilt) * 0.5f });
				// (the next point's in-handle is set when it comes)
				const int next = (i + 1) % n;
				pts[next].in = q2 - p3;
			}
			if (closed && !out.empty()) {
				out[0].in = pts[0].in;
			}
			pts = out;
		} else if (p_op == "reverse") {
			std::reverse(pts.begin(), pts.end());
			for (Pt &q : pts) {
				std::swap(q.in, q.out);
			}
		} else if (p_op == "cyclic") {
			c->set_closed(!closed);
		} else if (p_op == "auto" || p_op == "vector") {
			const int m = (int)pts.size();
			for (int i = 0; i < m; i++) {
				if (p_op == "vector") {
					pts[i].in = pts[i].out = Vector3();
					continue;
				}
				const bool has_prev = i > 0 || closed, has_next = i + 1 < m || closed;
				const Vector3 prev = has_prev ? pts[(i + m - 1) % m].p : pts[i].p, next = has_next ? pts[(i + 1) % m].p : pts[i].p;
				const Vector3 dir = (next - prev).normalized();
				pts[i].in = has_prev ? -dir * (pts[i].p.distance_to(prev) / 3.0f) : Vector3();
				pts[i].out = has_next ? dir * (pts[i].p.distance_to(next) / 3.0f) : Vector3();
			}
		} else if (p_op == "smooth") {
			const int m = (int)pts.size();
			std::vector<Pt> out = pts;
			for (int i = 0; i < m; i++) {
				if (!closed && (i == 0 || i == m - 1)) {
					continue;
				}
				const Vector3 mid = (pts[(i + m - 1) % m].p + pts[(i + 1) % m].p) * 0.5f;
				out[i].p = pts[i].p.lerp(mid, 0.5f);
			}
			pts = out;
		} else if (p_op == "extrude") {
			const Pt last = pts.back();
			const Vector3 dir = pts.size() >= 2 ? last.p - pts[pts.size() - 2].p : Vector3(1, 0, 0);
			pts.push_back({ last.p + dir, Vector3(), Vector3(), last.tilt });
		}
		c->clear_points();
		for (const Pt &q : pts) {
			c->add_point(q.p, q.in, q.out);
			c->set_point_tilt(c->get_point_count() - 1, q.tilt);
		}
		ur->add_do_property(path, "curve", c);
		ur->add_undo_property(path, "curve", old);
	}
	ur->commit_action();
}
