// CSG models in the editor: brush operations (= - \), models made from the
// selection or around the meshes a brush overlaps, brush outlines, a click on a model selects the brush
// under the mouse; and object helpers: hide (H), flip, snap to the grid.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <algorithm>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/confirmation_dialog.hpp>
#include <godot_cpp/classes/csg_shape3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>

using namespace godot;
using namespace hw_editor;

namespace {

AABB world_aabb(const Node3D *p_node) {
	const VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(p_node);
	return vi ? p_node->get_global_transform().xform(vi->get_aabb()) : AABB(p_node->get_global_position(), Vector3());
}

std::vector<HewnMesh3D *> selected_meshes() {
	std::vector<HewnMesh3D *> out;
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	for (int64_t i = 0; i < sel.size(); i++) {
		if (HewnMesh3D *n = Object::cast_to<HewnMesh3D>(sel[i])) {
			out.push_back(n);
		}
	}
	return out;
}

} // namespace

// Brushes into a model with undo (a new model first when p_model is not in the tree yet).
void HewnEditorPlugin::_csg_reparent(HewnMesh3D *p_model, bool p_new, Node *p_model_parent, const std::vector<HewnMesh3D *> &p_nodes,
		const std::vector<int> &p_ops, const String &p_action) {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(p_action, UndoRedo::MERGE_DISABLE, root);
	if (p_new) {
		ur->add_do_method(p_model_parent, "add_child", p_model, true);
		ur->add_do_method(p_model, "set_owner", root);
		ur->add_do_reference(p_model);
	}
	for (size_t i = 0; i < p_nodes.size(); i++) {
		HewnMesh3D *x = p_nodes[i];
		const int op = i < p_ops.size() ? p_ops[i] : x->get_brush_operation();
		if (x->get_parent() != p_model) {
			ur->add_do_method(x, "reparent", p_model, true);
			ur->add_do_method(x, "set_owner", root);
		}
		ur->add_do_method(x, "set_brush_operation", op);
	}
	for (size_t i = p_nodes.size(); i-- > 0;) {
		HewnMesh3D *x = p_nodes[i];
		ur->add_undo_method(x, "set_brush_operation", x->get_brush_operation());
		if (x->get_parent() != p_model) {
			ur->add_undo_method(x, "reparent", x->get_parent(), true);
			ur->add_undo_method(x->get_parent(), "move_child", x, x->get_index());
			ur->add_undo_method(x, "set_owner", x->get_owner());
		}
	}
	if (p_new) {
		ur->add_undo_method(p_model_parent, "remove_child", p_model);
	}
	ur->commit_action();
}

HewnMesh3D *HewnEditorPlugin::_new_csg_model(const Node3D *p_like) const {
	HewnMesh3D *model = memnew(HewnMesh3D);
	model->set_name("CSGModel");
	model->clear_mesh();
	model->set_csg_model(true);
	model->set_collision_mode(HewnMesh3D::COLLISION_TRIMESH); // (the level collides, brushes do not)
	if (p_like) {
		model->set_transform(Transform3D(Basis(), p_like->get_global_position()));
	}
	return model;
}

void HewnEditorPlugin::make_csg_model() {
	std::vector<HewnMesh3D *> nodes = selected_meshes();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (nodes.empty() || !root) {
		return;
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	if (mode != MODE_OBJECT) {
		set_mode(MODE_OBJECT);
	}
	Node *parent = nodes[0]->get_parent();
	HewnMesh3D *model = _new_csg_model(nodes[0]);
	// (its place: relative to the parent of the first brush)
	if (Node3D *p3 = Object::cast_to<Node3D>(parent)) {
		model->set_transform(p3->get_global_transform().affine_inverse() * model->get_transform());
	}
	_csg_reparent(model, true, parent, nodes, {}, "Hewn: Make CSG Model");
}

// = - \: the operation of the selected brushes. Meshes that are not brushes yet join the
// model they overlap, or a new model around the meshes they overlap (so a box drawn on a wall and
// turned into Subtract cuts the wall).
void HewnEditorPlugin::csg_set_operation(int p_op) {
	std::vector<HewnMesh3D *> nodes = selected_meshes();
	if (nodes.empty()) {
		HewnMesh3D *n = get_edited();
		if (!n) {
			return;
		}
		nodes.push_back(n);
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	if (mode != MODE_OBJECT) {
		set_mode(MODE_OBJECT);
	}
	std::vector<HewnMesh3D *> brushes, loose;
	for (HewnMesh3D *n : nodes) {
		(n->is_brush() ? brushes : loose).push_back(n);
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	if (!brushes.empty()) {
		ur->create_action("Hewn: Brush Operation", UndoRedo::MERGE_DISABLE, brushes[0]);
		for (HewnMesh3D *b : brushes) {
			ur->add_do_method(b, "set_brush_operation", p_op);
			ur->add_undo_method(b, "set_brush_operation", b->get_brush_operation());
		}
		ur->commit_action();
	}
	if (loose.empty()) {
		return;
	}
	AABB box = world_aabb(loose[0]);
	for (HewnMesh3D *n : loose) {
		box = box.merge(world_aabb(n));
	}
	auto is_loose = [&](HewnMesh3D *m) {
		for (HewnMesh3D *n : loose) {
			if (m == n || n->is_ancestor_of(m) || m->is_ancestor_of(n)) {
				return true;
			}
		}
		return false;
	};
	HewnMesh3D *model = nullptr;
	std::vector<HewnMesh3D *> bases;
	for (HewnMesh3D *m : _scene_meshes()) {
		if (is_loose(m) || m->is_brush() || !world_aabb(m).grow(1e-3f).intersects(box)) {
			continue;
		}
		if (m->is_csg_model()) {
			model = m;
			break;
		}
		bases.push_back(m);
	}
	std::vector<int> ops(loose.size(), p_op);
	if (model) {
		_csg_reparent(model, false, nullptr, loose, ops, "Hewn: Brush into CSG Model");
	} else if (!bases.empty()) {
		std::vector<HewnMesh3D *> all = bases;
		std::vector<int> all_ops(bases.size(), HewnMesh3D::BOOLEAN_UNION);
		all.insert(all.end(), loose.begin(), loose.end());
		all_ops.insert(all_ops.end(), ops.begin(), ops.end());
		Node *parent = bases[0]->get_parent();
		HewnMesh3D *nm = _new_csg_model(bases[0]);
		if (Node3D *p3 = Object::cast_to<Node3D>(parent)) {
			nm->set_transform(p3->get_global_transform().affine_inverse() * nm->get_transform());
		}
		_csg_reparent(nm, true, parent, all, all_ops, "Hewn: CSG Model");
	} else {
		// nothing to combine with: the operation is kept for when it becomes a brush
		ur->create_action("Hewn: Brush Operation", UndoRedo::MERGE_DISABLE, loose[0]);
		for (HewnMesh3D *n : loose) {
			ur->add_do_method(n, "set_brush_operation", p_op);
			ur->add_undo_method(n, "set_brush_operation", n->get_brush_operation());
		}
		ur->commit_action();
	}
	if (dock) {
		dock->refresh();
	}
}

// The brush of `p_model` whose surface is shown under the mouse (the last one in order wins).
HewnMesh3D *HewnEditorPlugin::_brush_at(Camera3D *p_cam, const Vector2 &p_pos, HewnMesh3D *p_model) const {
	Vector3 o, d;
	if (!p_cam || !p_model || !_ray(p_cam, p_pos, o, d)) {
		return nullptr;
	}
	float tm;
	if (raycast_node(p_model, o, d, tm, nullptr, false, true) < 0) {
		return nullptr;
	}
	HewnMesh3D *found = nullptr;
	const float eps = MAX(1e-3f, tm * 1e-3f);
	for (HewnMesh3D *b : p_model->get_brushes()) {
		// every hit along the ray, front or back (a subtracting brush shows its inside)
		const Transform3D g = b->get_global_transform();
		const Transform3D inv = g.affine_inverse();
		const Vector3 lo = inv.xform(o), ld = inv.basis.xform(d);
		const hewn::Mesh &m = b->get_shown_data();
		const hewn::BuildOut &bo = b->get_shown_build();
		bool hit = false;
		for (int f = 0; f < (int)m.faces.size() && f < (int)bo.face_tris.size() && !hit; f++) {
			const std::vector<int> &tr = bo.face_tris[f];
			const hewn::Face &F = m.faces[f];
			for (size_t k = 0; k + 2 < tr.size(); k += 3) {
				float t;
				if (ray_tri(lo, ld, m.pos[F.v[tr[k]]], m.pos[F.v[tr[k + 1]]], m.pos[F.v[tr[k + 2]]], t) &&
						std::fabs(g.xform(lo + ld * t).distance_to(o) - tm) < eps) {
					hit = true;
					break;
				}
			}
		}
		if (hit) {
			found = b;
		}
	}
	return found;
}

void HewnEditorPlugin::_select_brush_deferred(uint64_t p_id) {
	Node *b = Object::cast_to<Node>(ObjectDB::get_instance(ObjectID(p_id)));
	if (!b || !b->is_inside_tree()) {
		return;
	}
	EditorSelection *es = EditorInterface::get_singleton()->get_selection();
	es->clear();
	es->add_node(b);
}

// A click that selected a CSG model selects the brush under the mouse instead; the
// model itself: from the scene tree.
void HewnEditorPlugin::_check_brush_click(HewnMesh3D *p_selected) {
	if (!p_selected || !p_selected->is_csg_model() || brush_click_ms == 0 || Time::get_singleton()->get_ticks_msec() - brush_click_ms > 800) {
		return;
	}
	brush_click_ms = 0;
	Camera3D *cam = Object::cast_to<Camera3D>(ObjectDB::get_instance(brush_click_cam));
	if (HewnMesh3D *b = _brush_at(cam, brush_click_pos, p_selected)) {
		callable_mp(this, &HewnEditorPlugin::_select_brush_deferred).call_deferred((uint64_t)b->get_instance_id());
	}
}

std::vector<Node3D *> HewnEditorPlugin::_objects_under(Camera3D *p_cam, const Vector2 &p_pos) const {
	Vector3 from, dir;
	_ray(p_cam, p_pos, from, dir);
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	std::vector<std::pair<float, Node3D *>> hits;
	// (a node inside a scene placed in this one: the click selects the placed scene, unless its children are editable)
	auto selectable = [&](Node *p_node) -> Node3D * {
		Node *n = p_node;
		while (n && n != root && n->get_owner() && n->get_owner() != root && !root->is_editable_instance(n->get_owner())) {
			n = n->get_owner();
		}
		return Object::cast_to<Node3D>(n);
	};
	auto add = [&](float t, Node *p_node) {
		Node3D *s = selectable(p_node);
		if (s) {
			hits.push_back({ t, s });
		}
	};
	for (HewnMesh3D *m : _scene_meshes()) {
		if (!m->is_visible_in_tree() || m->is_brush() || m->is_boolean_cutter() || m->has_meta("_hw_lining")) {
			continue;
		}
		float t;
		if (raycast_node(m, from, dir, t, nullptr, cull_back(), m->is_csg_model()) >= 0) {
			add(t, m);
		}
	}
	std::vector<Node *> stack;
	if (root) {
		stack.push_back(root);
	}
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(x);
		CSGShape3D *csg = Object::cast_to<CSGShape3D>(x);
		Ref<Mesh> mesh;
		Transform3D g;
		if (mi && !Object::cast_to<HewnMesh3D>(x) && mi->is_visible_in_tree() && mi->get_mesh().is_valid()) {
			mesh = mi->get_mesh();
			g = mi->get_global_transform();
		} else if (csg && csg->is_root_shape() && csg->is_visible_in_tree()) {
			const Array am = csg->get_meshes();
			if (am.size() >= 2 && Object::cast_to<Mesh>(am[1])) {
				mesh = Ref<Mesh>(am[1]);
				g = csg->get_global_transform() * Transform3D(am[0]);
			}
		}
		if (mesh.is_null() || !g.xform(mesh->get_aabb()).grow(1e-3f).intersects_ray(from, dir)) {
			continue;
		}
		const PackedVector3Array tris = mesh->get_faces();
		float best = 1e30f;
		for (int64_t i = 0; i + 2 < tris.size(); i += 3) {
			float t;
			if (ray_tri(from, dir, g.xform(tris[i]), g.xform(tris[i + 1]), g.xform(tris[i + 2]), t) && t < best) {
				best = t;
			}
		}
		if (best < 1e29f) {
			add(best, x);
		}
	}
	std::sort(hits.begin(), hits.end(), [](const std::pair<float, Node3D *> &a, const std::pair<float, Node3D *> &b) { return a.first < b.first; });
	std::vector<Node3D *> out;
	for (const auto &h : hits) {
		if (std::find(out.begin(), out.end(), h.second) == out.end()) {
			out.push_back(h.second);
		}
	}
	return out;
}

// After Godot picked an object with a click: the one really seen there (not a room around it hit through its wall
// that is not drawn), and a click again at the same place takes the next object behind it.
void HewnEditorPlugin::_check_visible_click() {
	Camera3D *cam = Object::cast_to<Camera3D>(ObjectDB::get_instance(vis_click_cam));
	if (!cam || mode != MODE_OBJECT || modal != MODAL_NONE) {
		return;
	}
	const std::vector<Node3D *> under = _objects_under(cam, vis_click_pos);
	EditorSelection *es = EditorInterface::get_singleton()->get_selection();
	const TypedArray<Node> sel = es->get_selected_nodes();
	Node *picked = sel.size() == 1 ? Object::cast_to<Node>(sel[0]) : nullptr;
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	const bool again = cycle_node.is_valid() && vis_click_pos.distance_to(cycle_pos) < 4.0f && now - cycle_ms < 4000;
	cycle_pos = vis_click_pos;
	cycle_ms = now;
	if (under.empty() || (picked && std::find(under.begin(), under.end(), picked) == under.end())) {
		cycle_node = picked ? ObjectID(picked->get_instance_id()) : ObjectID(); // (a light, a marker: Godot's pick stays)
		return;
	}
	size_t want = 0;
	if (again) {
		for (size_t i = 0; i < under.size(); i++) {
			if ((uint64_t)under[i]->get_instance_id() == (uint64_t)cycle_node) {
				want = (i + 1) % under.size();
			}
		}
	}
	Node3D *target = under[want];
	cycle_node = ObjectID(target->get_instance_id());
	if (picked == target) {
		return;
	}
	es->clear();
	es->add_node(target);
}

// Outlines of the brushes of the model being worked on: blue adds, red subtracts, green intersects.
void HewnEditorPlugin::_draw_brushes(Control *p_overlay, Camera3D *p_cam) {
	std::vector<HewnMesh3D *> models;
	auto add_model = [&](HewnMesh3D *m) {
		if (m && std::find(models.begin(), models.end(), m) == models.end()) {
			models.push_back(m);
		}
	};
	if (HewnMesh3D *n = get_edited()) {
		add_model(n->is_csg_model() ? n : n->get_csg_model());
	}
	for (HewnMesh3D *n : selected_meshes()) {
		add_model(n->is_csg_model() ? n : n->get_csg_model());
	}
	const HewnMesh3D *edited = get_edited();
	int budget = 20000;
	for (HewnMesh3D *model : models) {
		for (HewnMesh3D *b : model->get_brushes()) {
			const int op = b->get_brush_operation();
			Color c = op == HewnMesh3D::BOOLEAN_SUBTRACT ? Color(1.0f, 0.42f, 0.3f, 0.9f) : op == HewnMesh3D::BOOLEAN_INTERSECT ? Color(0.45f, 1.0f, 0.5f, 0.9f) : Color(0.45f, 0.7f, 1.0f, 0.7f);
			const bool active = b == edited;
			if (active && mode != MODE_OBJECT) {
				continue; // (its edges are drawn by edit mode)
			}
			const Transform3D g = b->get_global_transform();
			const hewn::Mesh &m = b->get_data();
			std::unordered_set<uint64_t> seen;
			for (const hewn::Face &F : m.faces) {
				for (int k = 0; k < F.size() && budget > 0; k++) {
					const int a = F.v[k], e = F.v[(k + 1) % F.size()];
					if (!seen.insert(hewn::ekey(a, e)).second) {
						continue;
					}
					const Vector3 pa = g.xform(m.pos[a]), pb = g.xform(m.pos[e]);
					if (p_cam->is_position_behind(pa) || p_cam->is_position_behind(pb)) {
						continue;
					}
					p_overlay->draw_line(p_cam->unproject_position(pa), p_cam->unproject_position(pb), c, active ? 2.0f : 1.0f, true);
					budget--;
				}
			}
		}
	}
}

// ------------------------------------------------------------------ objects: hide, flip, snap

// H / Shift+H / Alt+H in edit mode: faces (in vertex / edge mode: the faces all of whose
// corners are selected). Revealed faces end up selected.
void HewnEditorPlugin::hide_faces(int p_kind) {
	HewnMesh3D *n = get_edited();
	if (!n || mode == MODE_OBJECT) {
		return;
	}
	hewn::Mesh &m = n->get_data();
	hewn::Selection &s = n->selection;
	std::vector<char> chosen(m.faces.size(), 0);
	if (mode == MODE_FACE) {
		for (int f : s.faces) {
			chosen[f] = 1;
		}
	} else if (mode == MODE_EDGE) {
		// (a hidden edge takes the faces it borders along)
		for (int f = 0; f < (int)m.faces.size(); f++) {
			const hewn::Face &F = m.faces[f];
			for (int i = 0; i < F.size() && !chosen[f]; i++) {
				const hewn::Edge e(F.v[i], F.v[(i + 1) % F.size()]);
				chosen[f] = std::find(s.edges.begin(), s.edges.end(), e) != s.edges.end();
			}
		}
	} else {
		// (a hidden vertex takes every face using it, its edges along)
		for (int f : hewn::sel::faces_from_verts(m, _selected_verts(), false)) {
			chosen[f] = 1;
		}
	}
	PackedInt32Array change;
	for (int f = 0; f < (int)m.faces.size(); f++) {
		const bool hit = p_kind == 2 ? m.faces[f].hidden : p_kind == 1 ? (!chosen[f] && !m.faces[f].hidden) : (chosen[f] && !m.faces[f].hidden);
		if (hit) {
			change.push_back(f);
		}
	}
	if (change.is_empty()) {
		return;
	}
	const Dictionary before = n->get_state();
	n->set_faces_hidden(change, p_kind != 2);
	if (p_kind == 2) {
		s.clear();
		for (int f : change) {
			if (mode == MODE_FACE) {
				s.faces.push_back(f);
			} else {
				for (int v : m.faces[f].v) {
					if (std::find(s.verts.begin(), s.verts.end(), v) == s.verts.end()) {
						s.verts.push_back(v);
					}
				}
			}
		}
		if (mode == MODE_EDGE) {
			s.edges = hewn::sel::edges_of_faces(m, hewn::sel::faces_from_verts(m, s.verts, true));
			s.verts.clear();
		}
	}
	selection_changed(); // (drops what got hidden from the selection)
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(p_kind == 2 ? "Hewn: Reveal Faces" : "Hewn: Hide Faces", UndoRedo::MERGE_DISABLE, n);
	ur->add_do_method(n, "set_state", n->get_state());
	ur->add_undo_method(n, "set_state", before);
	ur->commit_action(false);
	_hide_redo();
}

// H / Shift+H / Alt+H on objects: hide the selected, hide the others, show them again.
void HewnEditorPlugin::hide_objects(int p_kind) {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return;
	}
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	std::vector<Node3D *> targets;
	if (p_kind == 0) {
		for (int64_t i = 0; i < sel.size(); i++) {
			Node3D *x = Object::cast_to<Node3D>(sel[i]);
			if (x && x->is_visible()) {
				targets.push_back(x);
			}
		}
	} else if (p_kind == 1) {
		auto related = [&](Node *x) {
			for (int64_t i = 0; i < sel.size(); i++) {
				Node *s = Object::cast_to<Node>(sel[i]);
				if (s && (s == x || s->is_ancestor_of(x) || x->is_ancestor_of(s))) {
					return true;
				}
			}
			return false;
		};
		std::vector<Node *> stack = { root };
		while (!stack.empty()) {
			Node *x = stack.back();
			stack.pop_back();
			GeometryInstance3D *gi = Object::cast_to<GeometryInstance3D>(x);
			HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(x);
			if (gi && gi != root && gi->is_visible() && !related(gi) && !(pm && pm->is_brush())) {
				targets.push_back(gi); // (brushes stay: hiding one changes its model)
				continue;
			}
			for (int i = 0; i < x->get_child_count(); i++) {
				stack.push_back(x->get_child(i));
			}
		}
	} else {
		for (ObjectID id : hidden_by_us) {
			Node3D *x = Object::cast_to<Node3D>(ObjectDB::get_instance(id));
			if (x && x->is_inside_tree() && !x->is_visible()) {
				targets.push_back(x);
			}
		}
	}
	if (targets.empty()) {
		return;
	}
	const bool show = p_kind == 2;
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(show ? "Hewn: Reveal Hidden" : "Hewn: Hide", UndoRedo::MERGE_DISABLE, root);
	for (Node3D *x : targets) {
		ur->add_do_method(x, "set_visible", show);
		ur->add_undo_method(x, "set_visible", !show);
	}
	ur->commit_action();
	if (show) {
		hidden_by_us.clear();
	} else {
		for (Node3D *x : targets) {
			hidden_by_us.push_back(ObjectID(x->get_instance_id()));
		}
	}
}

// Flip X / Y / Z: the selected objects mirrored along a world axis around their middle.
// Hewn meshes get mirrored geometry (and keep a proper transform); other nodes a mirrored transform.
void HewnEditorPlugin::flip_objects(int p_axis) {
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	std::vector<Node3D *> nodes;
	Vector3 mid;
	for (int64_t i = 0; i < sel.size(); i++) {
		if (Node3D *x = Object::cast_to<Node3D>(sel[i])) {
			nodes.push_back(x);
			mid += world_aabb(x).get_center();
		}
	}
	if (nodes.empty()) {
		return;
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	if (mode != MODE_OBJECT) {
		set_mode(MODE_OBJECT);
	}
	mid /= (float)nodes.size();
	Basis s;
	s.set_column(p_axis, -s.get_column(p_axis));
	const Transform3D S = Transform3D(Basis(), mid) * Transform3D(s, Vector3()) * Transform3D(Basis(), -mid);
	static const char *names[3] = { "X", "Y", "Z" };
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(String("Hewn: Flip ") + names[p_axis], UndoRedo::MERGE_DISABLE, nodes[0]);
	for (Node3D *x : nodes) {
		const Transform3D before = x->get_global_transform();
		HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(x);
		if (pm) {
			// S * T = (S * T * F) * F with F a flip of the local X: the geometry takes F
			Basis fb;
			fb.set_column(0, -fb.get_column(0));
			const Transform3D F(fb, Vector3());
			const Dictionary state_before = pm->get_state();
			hewn::Mesh &m = pm->get_data();
			for (Vector3 &p : m.pos) {
				p.x = -p.x;
			}
			for (hewn::Face &face : m.faces) {
				face.reverse();
			}
			pm->commit(true);
			const Dictionary state_after = pm->get_state();
			const Transform3D after = S * before * F;
			ur->add_do_method(pm, "set_state", state_after);
			ur->add_do_method(pm, "set_global_transform", after);
			ur->add_undo_method(pm, "set_state", state_before);
			ur->add_undo_method(pm, "set_global_transform", before);
		} else {
			ur->add_do_method(x, "set_global_transform", S * before);
			ur->add_undo_method(x, "set_global_transform", before);
		}
	}
	ur->commit_action();
}

// Snap to Grid: object origins (edit mode: the selected vertices) onto the grid step.
void HewnEditorPlugin::snap_selection_to_grid() {
	const float step = dock ? (float)dock->snap_move->get_value() : 0.0f;
	if (step <= 0.0f) {
		return;
	}
	auto snap = [&](const Vector3 &p) { return (p / step).round() * step; };
	HewnMesh3D *n = get_edited();
	if (n && mode != MODE_OBJECT) {
		const std::vector<int> vs = _selected_verts();
		if (vs.empty()) {
			return;
		}
		begin_edit("Snap to Grid");
		const Transform3D g = n->get_global_transform();
		const Transform3D inv = g.affine_inverse();
		for (int v : vs) {
			n->get_data().pos[v] = inv.xform(snap(g.xform(n->get_data().pos[v])));
		}
		n->commit(true);
		end_edit();
		return;
	}
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	EditorUndoRedoManager *ur = get_undo_redo();
	bool any = false;
	for (int64_t i = 0; i < sel.size(); i++) {
		Node3D *x = Object::cast_to<Node3D>(sel[i]);
		if (!x) {
			continue;
		}
		if (!any) {
			ur->create_action("Hewn: Snap to Grid", UndoRedo::MERGE_DISABLE, x);
			any = true;
		}
		const Vector3 p = x->get_global_position();
		ur->add_do_method(x, "set_global_position", snap(p));
		ur->add_undo_method(x, "set_global_position", p);
	}
	if (any) {
		ur->commit_action();
	}
}

// Keys on objects (any Node3D, no Hewn mesh needed): H hides, = - \ set brush operations, [ ] the
// grid step. Returns true when used.
bool HewnEditorPlugin::_object_keys(Key p_code, bool p_ctrl, bool p_shift, bool p_alt) {
	if (p_ctrl) {
		return false;
	}
	if (p_code == KEY_H) {
		hide_objects(p_alt ? 2 : p_shift ? 1 : 0);
		return true;
	}
	if (p_shift || p_alt) {
		return false;
	}
	if (p_code == KEY_X && !EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes().is_empty()) {
		delete_objects_prompt();
		return true;
	}
	return false;
}

namespace {
// The selected nodes that can go (not the scene's root).
std::vector<Node *> deletable_nodes() {
	std::vector<Node *> out;
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	for (int64_t i = 0; i < sel.size(); i++) {
		Node *n = Object::cast_to<Node>(sel[i]);
		if (n && n != root && n->get_parent() && n->is_inside_tree()) {
			out.push_back(n);
		}
	}
	return out;
}

void owned_under(Node *p_node, std::vector<std::pair<Node *, Node *>> &r_out) {
	if (p_node->get_owner()) {
		r_out.push_back({ p_node, p_node->get_owner() });
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		owned_under(p_node->get_child(i), r_out);
	}
}
} // namespace

void HewnEditorPlugin::delete_objects_prompt() {
	const std::vector<Node *> nodes = deletable_nodes();
	if (nodes.empty()) {
		return;
	}
	if (!delobj_dialog) {
		delobj_dialog = memnew(ConfirmationDialog);
		delobj_dialog->set_name("HewnDeleteObjects");
		delobj_dialog->set_title(T("Delete"));
		delobj_dialog->set_ok_button_text(T("Delete"));
		delobj_dialog->connect("confirmed", callable_mp(this, &HewnEditorPlugin::_delete_objects));
		EditorInterface::get_singleton()->get_base_control()->add_child(delobj_dialog);
	}
	delobj_dialog->set_text(nodes.size() == 1 ? vformat(T("Delete node \"%s\"?"), nodes[0]->get_name()) : vformat(T("Delete %d nodes?"), (int)nodes.size()));
	delobj_dialog->reset_size();
	delobj_dialog->popup_centered();
}

void HewnEditorPlugin::_delete_objects() {
	const std::vector<Node *> nodes = deletable_nodes();
	if (nodes.empty()) {
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Delete Nodes", UndoRedo::MERGE_DISABLE, nodes[0]);
	for (Node *n : nodes) {
		Node *parent = n->get_parent();
		std::vector<std::pair<Node *, Node *>> owners;
		owned_under(n, owners);
		ur->add_do_method(EditorInterface::get_singleton()->get_selection(), "remove_node", n);
		ur->add_do_method(parent, "remove_child", n);
		ur->add_undo_method(parent, "add_child", n, true);
		ur->add_undo_method(parent, "move_child", n, n->get_index());
		for (const std::pair<Node *, Node *> &o : owners) {
			ur->add_undo_method(o.first, "set_owner", o.second); // (taken off when it left the scene)
		}
		ur->add_undo_reference(n);
	}
	ur->commit_action();
}
