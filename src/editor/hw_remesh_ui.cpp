// The Object tab's Remesh: the selected meshes made again. Voxel: on a grid of that voxel size (adaptivity: fewer
// faces where it is flat; Fix Poles: the vertices evened out along the surface; Preserve: the volume, the colors).
// Quad: quads of about the size asked for (a face count, a part of the faces now, or an edge length) on a grid along
// the axes, evened out along the surface; Variation moves that grid a little. (Not a flow along the curvature.)
#include "hw_editor.h"
#include "hw_editor_util.h"
#include "hw_textures.h"

#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include <cmath>
#include <unordered_map>

using namespace godot;
using namespace hw_editor;

namespace {

float surface_area(const hewn::Mesh &m) {
	float a = 0.0f;
	for (const hewn::Face &F : m.faces) {
		a += hewn::face_normal_raw(m, F).length() * 0.5f;
	}
	return a;
}

float volume_of(const hewn::Mesh &m) {
	double v = 0.0;
	std::vector<Vector3> pts;
	std::vector<int> tris;
	for (const hewn::Face &F : m.faces) {
		hewn::face_points(m, F, pts);
		tris.clear();
		hewn::triangulate(pts, hewn::face_normal(m, F), tris);
		for (size_t k = 0; k + 2 < tris.size(); k += 3) {
			v += pts[tris[k]].dot(pts[tris[k + 1]].cross(pts[tris[k + 2]]));
		}
	}
	return (float)(v / 6.0);
}

bool has_border(const hewn::Mesh &m) {
	std::unordered_map<uint64_t, int> c;
	for (const hewn::Face &F : m.faces) {
		for (int i = 0; i < F.size(); i++) {
			c[hewn::ekey(F.v[i], F.v[(i + 1) % F.size()])]++;
		}
	}
	for (const auto &kv : c) {
		if (kv.second != 2) {
			return true;
		}
	}
	return false;
}

void toast(const String &p_text) {
	if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
		t->push_toast(p_text, EditorToaster::SEVERITY_WARNING);
	}
}

} // namespace

std::vector<HewnMesh3D *> HewnEditorPlugin::_remesh_targets() const {
	std::vector<HewnMesh3D *> out;
	if (mode != MODE_OBJECT && get_edited()) {
		return edited_meshes();
	}
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	for (int64_t i = 0; i < sel.size(); i++) {
		if (HewnMesh3D *x = Object::cast_to<HewnMesh3D>(sel[i])) {
			out.push_back(x);
		}
	}
	return out;
}

void HewnEditorPlugin::remesh_voxel() {
	const std::vector<HewnMesh3D *> targets = _remesh_targets();
	if (targets.empty() || !dock) {
		toast(T("Remesh: select the meshes first."));
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Voxel Remesh", UndoRedo::MERGE_DISABLE, targets[0]);
	for (HewnMesh3D *n : targets) {
		const Dictionary before = n->get_state();
		hewn::Mesh m = n->get_data();
		const hewn::Mesh src = m;
		hewn::ops::RemeshOptions o;
		o.mode = hewn::ops::REMESH_VOXEL;
		o.voxel_size = (float)dock->remesh_voxel_size->get_value();
		o.adaptivity = (float)dock->remesh_adaptivity->get_value();
		o.remove_disconnected = false;
		if (!hewn::ops::remesh(m, o)) {
			continue;
		}
		if (dock->remesh_fix_poles->is_pressed()) {
			hewn::ops::relax_tangential(m, 4); // (the vertices evened out: fewer bunched-up corners)
		}
		if (dock->remesh_keep_volume->is_pressed()) {
			// the volume as it was: scaled about the middle
			const float v0 = std::fabs(volume_of(src)), v1 = std::fabs(volume_of(m));
			if (v0 > 1e-9f && v1 > 1e-9f) {
				Vector3 c;
				for (const Vector3 &p : m.pos) {
					c += p;
				}
				c /= (float)MAX((size_t)1, m.pos.size());
				const float k = (float)std::cbrt(v0 / v1);
				for (Vector3 &p : m.pos) {
					p = c + (p - c) * k;
				}
			}
		}
		hewn::ops::transfer_attributes(m, src, dock->remesh_keep_attrs->is_pressed(), dock->remesh_keep_attrs->is_pressed());
		n->get_data() = m;
		n->selection = hewn::Selection();
		n->commit(true);
		ur->add_do_method(n, "set_state", n->get_state());
		ur->add_undo_method(n, "set_state", before);
	}
	ur->commit_action(false);
	if (dock) {
		dock->refresh();
	}
}

float HewnEditorPlugin::quad_remesh_edge(HewnMesh3D *p_mesh) const {
	if (!p_mesh || !dock) {
		return 0.1f;
	}
	const hewn::Mesh &m = p_mesh->get_data();
	const float area = MAX(1e-9f, surface_area(m));
	switch (dock->remesh_q_by ? dock->remesh_q_by->get_selected() : 0) {
		case 1: { // a part of the faces there are now
			const float count = MAX(1.0f, (float)m.faces.size() * (float)dock->remesh_q_ratio->get_value());
			return std::sqrt(area / count);
		}
		case 2:
			return MAX(1e-4f, (float)dock->remesh_q_edge->get_value());
		default:
			return std::sqrt(area / MAX(1.0, dock->remesh_q_faces->get_value()));
	}
}

void HewnEditorPlugin::remesh_quad() {
	const std::vector<HewnMesh3D *> targets = _remesh_targets();
	if (targets.empty() || !dock) {
		toast(T("Remesh: select the meshes first."));
		return;
	}
	if (dock->remesh_q_boundary->is_pressed()) {
		for (HewnMesh3D *n : targets) {
			if (has_border(n->get_data())) {
				toast(T("Quad Remesh makes closed surfaces: a mesh with open borders can't keep them (turn Preserve Mesh Boundary off to close it)."));
				return;
			}
		}
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Quad Remesh", UndoRedo::MERGE_DISABLE, targets[0]);
	const int seed = (int)dock->remesh_q_seed->get_value();
	for (HewnMesh3D *n : targets) {
		const Dictionary before = n->get_state();
		hewn::Mesh m = n->get_data();
		const hewn::Mesh src = m;
		hewn::ops::RemeshOptions o;
		const bool sharp = dock->remesh_q_sharp->is_pressed();
		o.mode = sharp ? hewn::ops::REMESH_SHARP : hewn::ops::REMESH_VOXEL;
		o.cell = quad_remesh_edge(n);
		o.sharpness = 1.0f;
		o.remove_disconnected = false;
		o.symmetric_x = dock->remesh_q_symmetry->is_pressed();
		// the variation: the grid moved by a part of a cell, from the seed
		uint32_t h = (uint32_t)seed * 2654435761u + 0x9e3779b9u;
		for (int a = 0; a < 3; a++) {
			h ^= h >> 15;
			h *= 0x2c1b3c6du;
			h ^= h >> 12;
			o.grid_offset[a] = seed == 0 ? 0.0f : (float)(h & 0xffff) / 65536.0f;
		}
		if (!hewn::ops::remesh(m, o)) {
			continue;
		}
		if (!sharp) {
			hewn::ops::relax_tangential(m, 4);
		}
		hewn::ops::transfer_attributes(m, src, dock->remesh_q_attrs->is_pressed(), false);
		for (hewn::Face &F : m.faces) {
			F.smooth = dock->remesh_q_smooth->is_pressed() ? 1 : 0;
		}
		n->get_data() = m;
		n->selection = hewn::Selection();
		n->commit(true);
		ur->add_do_method(n, "set_state", n->get_state());
		ur->add_undo_method(n, "set_state", before);
	}
	ur->commit_action(false);
	if (dock) {
		dock->refresh();
	}
}

// ------------------------------------------------------------------ the block

void HewnDock::_build_remesh_block(VBoxContainer *p_parent) {
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	remesh_mode = memnew(OptionButton);
	remesh_mode->set_name("RemeshMode");
	remesh_mode->add_item("Voxel");
	remesh_mode->add_item("Quad");
	remesh_mode->connect("item_selected", callable_mp(this, &HewnDock::_on_remesh_changed).unbind(1));
	p_parent->add_child(remesh_mode);
	auto field = [&](GridContainer *p_grid, const String &p_label, double p_min, double p_max, double p_step, double p_value, bool p_int, const String &p_tip) {
		Label *l = memnew(Label);
		l->set_text(p_label);
		l->set_tooltip_text(p_tip);
		l->set_mouse_filter(Control::MOUSE_FILTER_PASS);
		p_grid->add_child(l);
		HewnDragField *f = memnew(HewnDragField);
		f->setup(p_min, p_max, p_step, p_int, p_value);
		f->set_custom_minimum_size(Vector2(90 * es, 0));
		f->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		f->set_tooltip_text(p_tip);
		f->connect("value_changed", callable_mp(this, &HewnDock::_on_remesh_changed).unbind(1));
		p_grid->add_child(f);
		return f;
	};
	auto check = [&](Container *p_box, const String &p_text, bool p_on, const String &p_tip) {
		CheckBox *c = memnew(CheckBox);
		c->set_text(p_text);
		c->set_pressed(p_on);
		c->set_tooltip_text(p_tip);
		p_box->add_child(c);
		return c;
	};
	// Voxel
	VBoxContainer *vb = memnew(VBoxContainer);
	remesh_voxel_box = vb;
	p_parent->add_child(vb);
	GridContainer *vg = memnew(GridContainer);
	vg->set_columns(2);
	vb->add_child(vg);
	remesh_voxel_size = field(vg, "Voxel Size", 0.0001, 100, 0.005, 0.1, false, "The size of the cells the shape is made again on (meters)");
	remesh_voxel_size->set_name("RemeshVoxelSize");
	remesh_adaptivity = field(vg, "Adaptivity", 0, 1, 0.01, 0, false, "Fewer faces where the surface is flat");
	remesh_fix_poles = check(vb, "Fix Poles", true, "The vertices evened out along the surface (fewer bunched-up corners)");
	Label *pl = memnew(Label);
	pl->set_text("Preserve");
	vb->add_child(pl);
	remesh_keep_volume = check(vb, "Volume", true, "The volume as it was (the result scaled a little)");
	remesh_keep_attrs = check(vb, "Attributes", true, "The vertex colors and the smoothing of the nearest faces (the materials always)");
	Button *vbtn = memnew(Button);
	vbtn->set_name("VoxelRemesh");
	vbtn->set_text("Voxel Remesh");
	vbtn->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::remesh_voxel));
	vb->add_child(vbtn);
	// Quad
	VBoxContainer *qb = memnew(VBoxContainer);
	remesh_quad_box = qb;
	p_parent->add_child(qb);
	remesh_q_symmetry = check(qb, "Use Mesh Symmetry", false, "The new faces the same on both sides of the object's X = 0");
	remesh_q_sharp = check(qb, "Preserve Sharp", false, "Corners and creases kept sharp");
	remesh_q_boundary = check(qb, "Preserve Mesh Boundary", false, "The result is a closed surface: with this on, a mesh with open borders is not changed");
	remesh_q_attrs = check(qb, "Preserve Attributes", false, "The vertex colors of the nearest faces (the materials always)");
	remesh_q_smooth = check(qb, "Smooth Normals", false, "Smooth shading on the new faces");
	remesh_q_by = memnew(OptionButton);
	remesh_q_by->set_name("QuadRemeshBy");
	remesh_q_by->add_item("Faces");
	remesh_q_by->add_item("Ratio");
	remesh_q_by->add_item("Edge Length");
	remesh_q_by->connect("item_selected", callable_mp(this, &HewnDock::_on_remesh_changed).unbind(1));
	qb->add_child(remesh_q_by);
	GridContainer *qg = memnew(GridContainer);
	qg->set_columns(2);
	qb->add_child(qg);
	remesh_q_faces = field(qg, "Number of Faces", 4, 1000000, 1, 5000, true, "About how many faces the result has");
	remesh_q_ratio = field(qg, "Ratio", 0, 1, 0.01, 1, false, "The faces there are now times this");
	remesh_q_edge = field(qg, "Edge Length", 0.0001, 100, 0.005, 0.1, false, "The length of the new edges (meters)");
	remesh_q_rows[0] = remesh_q_faces;
	remesh_q_rows[1] = remesh_q_ratio;
	remesh_q_rows[2] = remesh_q_edge;
	Label *el = memnew(Label);
	el->set_text("Faces (about)");
	qg->add_child(el);
	remesh_q_estimate = memnew(Label);
	remesh_q_estimate->set_name("QuadRemeshEstimate");
	qg->add_child(remesh_q_estimate);
	remesh_q_seed = field(qg, "Variation", 0, 1000000, 1, 0, true, "Another variation of the same density (the grid moved a little)");
	Button *qbtn = memnew(Button);
	qbtn->set_name("QuadRemesh");
	qbtn->set_text("Quad Remesh");
	qbtn->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::remesh_quad));
	qb->add_child(qbtn);
	_on_remesh_changed();
}

void HewnDock::_on_remesh_changed() {
	if (!remesh_mode) {
		return;
	}
	const bool quad = remesh_mode->get_selected() == 1;
	remesh_voxel_box->set_visible(!quad);
	remesh_quad_box->set_visible(quad);
	const int by = remesh_q_by->get_selected();
	for (int i = 0; i < 3; i++) {
		// (the field and its label)
		Control *f = remesh_q_rows[i];
		f->set_visible(i == by);
		if (Control *l = Object::cast_to<Control>(f->get_parent()->get_child(f->get_index() - 1))) {
			l->set_visible(i == by);
		}
	}
	// about how many faces: for the faces count asked for, the count itself; else from the surface of the mesh
	String est = "-";
	if (plugin) {
		const std::vector<HewnMesh3D *> t = plugin->_remesh_targets();
		if (!t.empty()) {
			if (by == 0) {
				est = itos((int64_t)remesh_q_faces->get_value());
			} else {
				const float e = plugin->quad_remesh_edge(t[0]);
				est = itos((int64_t)std::llround(surface_area(t[0]->get_data()) / MAX(1e-9f, e * e)));
			}
		}
	}
	remesh_q_estimate->set_text(est);
}

// ------------------------------------------------------------------ trims

// A sweep along p_path (in p_source's space), placed with it: a cornice beside the path (Outside), across the edge's
// middle (Center); closed: a loop. Its profile / size: the shape panel then.
HewnMesh3D *HewnEditorPlugin::_make_trim(HewnMesh3D *p_source, const hewn::ops::TrimPath &p_path) {
	if (p_path.points.size() < 2) {
		return nullptr;
	}
	HewnMesh3D *node = memnew(HewnMesh3D);
	node->set_name(String(p_source->get_name()) + "_Trim");
	node->set_shape(hewn::shapes::SWEEP);
	PackedVector3Array pts;
	for (const Vector3 &p : p_path.points) {
		pts.push_back(p);
	}
	node->set_poly_points(pts);
	node->set_shape_param("profile", 4); // (a cornice)
	node->set_shape_param("width", 0.2f);
	node->set_shape_param("height", 0.15f);
	node->set_shape_param("anchor", 1); // (its middle on the edge)
	node->set_shape_param("align", 1); // (beside the path: against the wall)
	node->set_shape_param("closed", p_path.closed ? 1 : 0);
	node->set_transform(p_source->get_global_transform());
	if (texture_dock && texture_dock->current_material.is_valid()) {
		node->apply_material(texture_dock->current_material, PackedInt32Array());
	}
	return node;
}

void HewnEditorPlugin::sweep_along_edges() {
	HewnMesh3D *n = get_edited();
	if (!n || mode == MODE_OBJECT) {
		toast(T("Sweep Along Edges: select the edges in edit mode first."));
		return;
	}
	const hewn::Mesh &m = n->get_data();
	const hewn::Selection &s = n->selection;
	std::vector<hewn::Edge> edges = s.edges;
	if (mode == MODE_VERTEX) {
		hewn::Topology t;
		t.build(m);
		edges = hewn::sel::edges_from_verts(m, t, s.verts);
	} else if (mode == MODE_FACE) {
		// the outline of the selected faces
		std::unordered_map<uint64_t, int> c;
		std::unordered_map<uint64_t, hewn::Edge> e_of;
		for (int f : s.faces) {
			const hewn::Face &F = m.faces[f];
			for (int i = 0; i < F.size(); i++) {
				const hewn::Edge e(F.v[i], F.v[(i + 1) % F.size()]);
				c[hewn::ekey(e)]++;
				e_of[hewn::ekey(e)] = e;
			}
		}
		edges.clear();
		for (const auto &kv : c) {
			if (kv.second == 1) {
				edges.push_back(e_of[kv.first]);
			}
		}
	}
	if (edges.empty()) {
		toast(T("Sweep Along Edges: select the edges in edit mode first."));
		return;
	}
	const std::vector<hewn::ops::TrimPath> paths = hewn::ops::edge_trim_paths(m, edges);
	HewnMesh3D *last = nullptr;
	for (const hewn::ops::TrimPath &p : paths) {
		if (HewnMesh3D *t = _make_trim(n, p)) {
			_add_node_with_undo(t, "Sweep Along Edges", Object::cast_to<Node3D>(n->get_parent()));
			last = t;
		}
	}
	(void)last; // (the last one is selected: its shape panel shows)
}

void HewnEditorPlugin::belt_at_height() {
	const std::vector<HewnMesh3D *> targets = _remesh_targets();
	if (targets.empty() || !dock) {
		toast(T("Belt at Height: select the objects first."));
		return;
	}
	const float h0 = (float)dock->belt_height->get_value();
	const int floors = MAX(1, (int)dock->belt_floors->get_value());
	const float step = (float)dock->belt_step->get_value();
	int made = 0;
	for (HewnMesh3D *n : targets) {
		const hewn::Mesh &m = n->get_data();
		if (m.pos.empty()) {
			continue;
		}
		// (the heights from the object's bottom)
		float bottom = m.pos[0].y;
		for (const Vector3 &p : m.pos) {
			bottom = MIN(bottom, p.y);
		}
		for (int i = 0; i < floors; i++) {
			for (const hewn::ops::TrimPath &p : hewn::ops::section_trim_paths(m, bottom + h0 + step * (float)i)) {
				if (HewnMesh3D *t = _make_trim(n, p)) {
					_add_node_with_undo(t, "Belt at Height", Object::cast_to<Node3D>(n->get_parent()));
					made++;
				}
			}
		}
	}
	if (made == 0) {
		toast(T("Belt at Height: the objects have no walls at that height."));
	}
}

void HewnDock::_build_trims_block(VBoxContainer *p_parent) {
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	GridContainer *g = memnew(GridContainer);
	g->set_columns(2);
	p_parent->add_child(g);
	auto field = [&](const String &p_label, double p_min, double p_max, double p_step, double p_value, bool p_int, const String &p_tip) {
		Label *l = memnew(Label);
		l->set_text(p_label);
		l->set_tooltip_text(p_tip);
		l->set_mouse_filter(Control::MOUSE_FILTER_PASS);
		g->add_child(l);
		HewnDragField *f = memnew(HewnDragField);
		f->setup(p_min, p_max, p_step, p_int, p_value);
		f->set_custom_minimum_size(Vector2(90 * es, 0));
		f->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		f->set_tooltip_text(p_tip);
		g->add_child(f);
		return f;
	};
	belt_height = field("Height", -1000, 1000, 0.05, 3.0, false, "Where the belt goes, from the object's bottom (meters)");
	belt_height->set_name("BeltHeight");
	belt_floors = field("Floors", 1, 100, 1, 1, true, "How many belts, one above the other");
	belt_step = field("Floor Height", 0.01, 1000, 0.05, 3.0, false, "The distance between them (meters)");
	Button *b = memnew(Button);
	b->set_name("BeltAtHeight");
	b->set_text("Belt at Height");
	b->set_tooltip_text("A sweep (a cornice) around the selected objects' walls at that height: its profile and size in the shape panel then");
	b->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::belt_at_height));
	p_parent->add_child(b);
}
