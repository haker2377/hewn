// Modal operations: G/R/S with axis constraints and typed values, E extrude, I inset,
// Ctrl+B bevel, Shift+D duplicate and the B point-to-point snap move. Shift while moving snaps to geometry.
#include "hw_editor.h"
#include "hw_editor_util.h"
#include "../hw_cutter.h"


#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>

using namespace godot;
using namespace hw_editor;

namespace {

const float SNAP_RADIUS = 16.0f; // px, vertices
const float EDGE_RADIUS = 10.0f; // px, edges

Vector3 cam_forward(Camera3D *p_cam) {
	return -p_cam->get_global_transform().basis.get_column(2).normalized();
}

bool is_number_key(Key k, String &r_char) {
	if (k >= KEY_0 && k <= KEY_9) {
		r_char = String::chr('0' + (k - KEY_0));
		return true;
	}
	if (k >= KEY_KP_0 && k <= KEY_KP_9) {
		r_char = String::chr('0' + (k - KEY_KP_0));
		return true;
	}
	if (k == KEY_PERIOD || k == KEY_KP_PERIOD || k == KEY_COMMA) {
		r_char = ".";
		return true;
	}
	if (k == KEY_MINUS || k == KEY_KP_SUBTRACT) {
		r_char = "-";
		return true;
	}
	return false;
}

} // namespace

// ------------------------------------------------------------------ snap candidates

void HewnEditorPlugin::_gather_snap_candidates(bool p_skip_moving) {
	cand_points.clear();
	cand_edges.clear();
	cand_skip_faces.clear();
	cand_face_centers.clear();
	HewnMesh3D *edited = get_edited();
	std::unordered_set<int> moving;
	if (p_skip_moving && !modal_object) {
		moving.insert(modal_verts.begin(), modal_verts.end());
	}
	if (edited && !moving.empty()) {
		const hewn::Mesh &m = edited->get_data();
		for (int f = 0; f < (int)m.faces.size(); f++) {
			for (int v : m.faces[f].v) {
				if (moving.count(v)) {
					cand_skip_faces.insert(f);
					break;
				}
			}
		}
	}
	for (HewnMesh3D *node : _scene_meshes()) {
		const bool is_edited = node == edited;
		if (p_skip_moving && _moving_node(node)) {
			continue; // the whole node moves
		}
		const Transform3D g = node->get_global_transform();
		const hewn::Mesh &m = node->get_data();
		std::vector<char> used(m.pos.size(), 0);
		for (const hewn::Face &F : m.faces) {
			for (int v : F.v) {
				used[v] = 1;
			}
		}
		for (int v = 0; v < (int)m.pos.size(); v++) {
			if (used[v] && !(is_edited && moving.count(v))) {
				cand_points.push_back(g.xform(m.pos[v]));
			}
		}
		hewn::Topology t;
		t.build(m);
		for (const hewn::Edge &e : t.edges) {
			if (is_edited && (moving.count(e.a) || moving.count(e.b))) {
				continue;
			}
			cand_edges.push_back({ g.xform(m.pos[e.a]), g.xform(m.pos[e.b]) });
		}
		for (int f = 0; f < (int)m.faces.size(); f++) {
			if (is_edited && cand_skip_faces.count(f)) {
				continue;
			}
			Vector3 c;
			for (int v : m.faces[f].v) {
				c += m.pos[v];
			}
			cand_face_centers.push_back(g.xform(c / (float)MAX(1, m.faces[f].size())));
		}
	}
	if (modal_object && !p_skip_moving) {
		// B on objects: their origins are source points too
		for (size_t i = 0; i < obj_nodes.size(); i++) {
			cand_points.push_back(obj_orig[i].origin);
		}
	}
	_gather_plain_meshes(p_skip_moving);
}

// The other meshes of the scene (MeshInstance3D, not Hewn): their corners and the edges between
// faces that are not flat (not the diagonals of quads) snap like ours; their triangles are surfaces.
void HewnEditorPlugin::_gather_plain_meshes(bool p_skip_moving) {
	cand_tris.clear();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return;
	}
	const int MAX_TRIS = 20000; // per mesh (bigger ones: the corners of their box only)
	int budget = 200000;
	std::vector<std::pair<Node *, bool>> stack = { { root, false } };
	while (!stack.empty()) {
		Node *x = stack.back().first;
		bool moving = stack.back().second;
		stack.pop_back();
		Node3D *x3 = Object::cast_to<Node3D>(x);
		if (x3 && modal_object) {
			moving = moving || _moving_node(x3); // (children move with their parent)
		}
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back({ x->get_child(i), moving });
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(x);
		if (!mi || Object::cast_to<HewnMesh3D>(x) || !mi->is_visible_in_tree() || mi->get_mesh().is_null() || (p_skip_moving && moving)) {
			continue;
		}
		const Transform3D g = mi->get_global_transform();
		const PackedVector3Array faces = mi->get_mesh()->get_faces();
		if (faces.size() / 3 > MAX_TRIS || budget <= 0) {
			const AABB b = mi->get_aabb();
			for (int k = 0; k < 8; k++) {
				cand_points.push_back(g.xform(b.get_endpoint(k)));
			}
			continue;
		}
		budget -= (int)faces.size();
		// welded corners (the arrays repeat them per face / per normal)
		std::unordered_map<uint64_t, int> ids;
		std::vector<Vector3> pts;
		auto id_of = [&](const Vector3 &p) {
			const int64_t qx = (int64_t)std::llround(p.x * 1e4), qy = (int64_t)std::llround(p.y * 1e4), qz = (int64_t)std::llround(p.z * 1e4);
			const uint64_t k = ((uint64_t)(qx & 0x1fffff) << 42) | ((uint64_t)(qy & 0x1fffff) << 21) | (uint64_t)(qz & 0x1fffff);
			auto it = ids.find(k);
			if (it != ids.end()) {
				return it->second;
			}
			const int id = (int)pts.size();
			ids[k] = id;
			pts.push_back(p);
			return id;
		};
		std::unordered_map<uint64_t, std::vector<Vector3>> edge_normals;
		for (int64_t t = 0; t + 2 < faces.size(); t += 3) {
			const Vector3 a = faces[t], b = faces[t + 1], c = faces[t + 2];
			const Vector3 nrm = (b - a).cross(c - a);
			if (nrm.length() < 1e-12f) {
				continue;
			}
			const int ia = id_of(a), ib = id_of(b), ic = id_of(c);
			for (const std::pair<int, int> &e : { std::pair<int, int>(ia, ib), std::pair<int, int>(ib, ic), std::pair<int, int>(ic, ia) }) {
				edge_normals[hewn::ekey(e.first, e.second)].push_back(nrm.normalized());
			}
			cand_tris.push_back(g.xform(a));
			cand_tris.push_back(g.xform(b));
			cand_tris.push_back(g.xform(c));
		}
		std::vector<char> on_edge(pts.size(), 0);
		for (const auto &kv : edge_normals) {
			const std::vector<Vector3> &ns = kv.second;
			bool feature = ns.size() != 2;
			for (size_t i = 1; i < ns.size() && !feature; i++) {
				feature = ns[i].dot(ns[0]) < 0.999f;
			}
			if (!feature) {
				continue; // (inside a flat face: a diagonal)
			}
			const int a = (int)(kv.first >> 32), b = (int)(kv.first & 0xffffffffu);
			cand_edges.push_back({ g.xform(pts[a]), g.xform(pts[b]) });
			on_edge[a] = on_edge[b] = 1;
		}
		for (size_t i = 0; i < pts.size(); i++) {
			if (on_edge[i]) {
				cand_points.push_back(g.xform(pts[i]));
			}
		}
	}
}

bool HewnEditorPlugin::_raycast_cand_tris(const Vector3 &p_from, const Vector3 &p_dir, Vector3 &r_hit) const {
	float best = 1e30f;
	for (size_t i = 0; i + 2 < cand_tris.size(); i += 3) {
		float t;
		if (ray_tri(p_from, p_dir, cand_tris[i], cand_tris[i + 1], cand_tris[i + 2], t) && t < best) {
			best = t;
		}
	}
	if (best < 1e29f) {
		r_hit = p_from + p_dir * best;
		return true;
	}
	return false;
}

// A snap target seen from the camera: no surface of the scene in front of it (all of them in X-ray).
bool HewnEditorPlugin::_snap_seen(Camera3D *p_cam, const Vector3 &p_world) const {
	if (xray_on || (dock && dock->xray && dock->xray->is_pressed())) {
		return true;
	}
	const Vector2 sp = p_cam->unproject_position(p_world);
	Vector3 from, dir;
	_ray(p_cam, sp, from, dir);
	const float dist = (p_world - from).dot(dir);
	const float tol = MAX(1e-3f, dist * 2e-3f);
	Vector3 hit, nrm;
	if (_raycast_scene(from, dir, hit, nrm, nullptr, nullptr, true) && (hit - from).dot(dir) < dist - tol) {
		return false;
	}
	Vector3 plain;
	if (_raycast_cand_tris(from, dir, plain) && (plain - from).dot(dir) < dist - tol) {
		return false;
	}
	return true;
}

bool HewnEditorPlugin::_snap_point_at(Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_point) const {
	return _snap_point_in(cand_points, p_cam, p_pos, r_point);
}

// The point of p_points nearest to the mouse on screen (within the snap radius), the nearer one in depth
// when several overlap.
bool HewnEditorPlugin::_snap_point_in(const std::vector<Vector3> &p_points, Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_point) const {
	float best = SNAP_RADIUS;
	float best_depth = 1e30f;
	bool found = false;
	const Vector3 eye = p_cam->get_global_position();
	for (const Vector3 &p : p_points) {
		if (p_cam->is_position_behind(p)) {
			continue;
		}
		const float d = p_cam->unproject_position(p).distance_to(p_pos);
		const float depth = eye.distance_to(p);
		// Prefer the nearer point when several project to (almost) the same pixel; hidden ones do not snap.
		if ((d < best - 1.0f || (d < best + 1.0f && d < SNAP_RADIUS && depth < best_depth)) && _snap_seen(p_cam, p)) {
			best = MIN(best, d);
			best_depth = depth;
			r_point = p;
			found = true;
		}
	}
	return found;
}

bool HewnEditorPlugin::_magnet_target(Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_point) const {
	last_snap_kind = SNAP_KIND_VERTEX; // (what it lies on: the mark's symbol)
	if (_snap_point_at(p_cam, p_pos, r_point)) {
		return true;
	}
	last_snap_kind = SNAP_KIND_EDGE;
	float best = EDGE_RADIUS;
	bool found = false;
	for (const std::pair<Vector3, Vector3> &e : cand_edges) {
		if (p_cam->is_position_behind(e.first) || p_cam->is_position_behind(e.second)) {
			continue;
		}
		float t;
		const float d = seg_dist(p_pos, p_cam->unproject_position(e.first), p_cam->unproject_position(e.second), &t);
		if (d < best && _snap_seen(p_cam, e.first.lerp(e.second, t))) {
			best = d;
			r_point = e.first.lerp(e.second, t);
			found = true;
		}
	}
	if (found) {
		return true;
	}
	last_snap_kind = SNAP_KIND_FACE;
	Vector3 from, dir, hit, nrm;
	_ray(p_cam, p_pos, from, dir);
	if (_raycast_scene(from, dir, hit, nrm, nullptr, nullptr, true)) {
		r_point = hit;
		return true;
	}
	return false;
}

// ------------------------------------------------------------------ constraints

Vector3 HewnEditorPlugin::_axis_dir(int p_axis) const {
	if (p_axis == 3) {
		return modal_custom_dir;
	}
	if (p_axis < 0 || p_axis > 2) {
		return Vector3();
	}
	// the first press uses the chosen orientation, the second the alternative one
	// (Local when the orientation is Global, Global otherwise).
	Basis b = modal_orient;
	if (modal_local) {
		if (dock->orientation == HewnDock::ORIENT_GLOBAL) {
			const HewnMesh3D *n = get_edited();
			b = (modal_object ? modal_object_xf.basis : (n ? n->get_global_transform().basis : Basis())).orthonormalized();
		} else {
			b = Basis();
		}
	}
	const Vector3 c = b.get_column(p_axis);
	return c.length() > 1e-9f ? c.normalized() : AXES[p_axis];
}

Vector3 HewnEditorPlugin::_constrain(const Vector3 &p_delta) const {
	if (modal_axis < 0) {
		return p_delta;
	}
	const Vector3 d = _axis_dir(modal_axis);
	if (modal_plane) {
		return p_delta - d * p_delta.dot(d);
	}
	return d * p_delta.dot(d);
}

bool HewnEditorPlugin::_modal_number(float &r_value) const {
	if (modal_num.is_empty() || modal_num == "-" || modal_num == "." || modal_num == "-.") {
		return false;
	}
	double v = 0.0;
	if (!HewnDragField::eval_expression(modal_num, v)) {
		return false; // (an expression still being typed: "1/")
	}
	r_value = (float)v;
	return true;
}

// ------------------------------------------------------------------ start

// Vertices, weights (proportional editing) and pivots of a G / R / S operation.
void HewnEditorPlugin::_setup_modal_set(Camera3D *p_cam, Modal p_kind) {
	HewnMesh3D *n = get_edited();
	const std::vector<int> sel = _selected_verts();
	modal_sel_count = sel.size();
	prop_proj_axis = dock->prop_projected && dock->prop_projected->is_pressed() && p_cam ? -p_cam->get_global_transform().basis.get_column(2).normalized() : Vector3();
	_weighted_verts(sel, dock->prop_enabled->is_pressed(), (float)dock->prop_radius->get_value(), dock->prop_falloff->get_selected(),
			dock->prop_connected->is_pressed(), modal_verts, modal_weights);
	const Transform3D g = n->get_global_transform();
	modal_orig.clear();
	for (int v : modal_verts) {
		modal_orig.push_back(g.xform(n->get_data().pos[v]));
	}
	modal_pivot = _pivot_point(dock->pivot_mode, sel);
	modal_vpivots.assign(modal_verts.size(), modal_pivot);
	if (dock->pivot_mode == HewnDock::PIVOT_INDIVIDUAL && p_kind != MODAL_MOVE) {
		const std::vector<Vector3> ind = _individual_pivots(sel, modal_pivot);
		for (size_t i = 0; i < ind.size(); i++) {
			modal_vpivots[i] = ind[i];
		}
	}
	// the other edited meshes' selections move along (multi-object editing)
	modal_owner.assign(modal_verts.size(), 0);
	multi_before.clear();
	if (!modal_multi_ok || multi_ids.empty()) {
		return;
	}
	const uint64_t keep = edited_id;
	Vector3 sum;
	int cnt = 0;
	for (size_t i = 0; i < modal_orig.size(); i++) {
		if (modal_weights[i] >= 1.0f) {
			sum += modal_orig[i];
			cnt++;
		}
	}
	for (size_t k = 0; k < multi_ids.size(); k++) {
		HewnMesh3D *o = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(multi_ids[k])));
		if (!o || !o->is_inside_tree()) {
			continue;
		}
		edited_id = multi_ids[k];
		const std::vector<int> osel = _selected_verts();
		if (osel.empty()) {
			continue;
		}
		std::vector<int> vs;
		std::vector<float> ws;
		_weighted_verts(osel, dock->prop_enabled->is_pressed(), (float)dock->prop_radius->get_value(), dock->prop_falloff->get_selected(),
				dock->prop_connected->is_pressed(), vs, ws);
		multi_before.push_back({ multi_ids[k], o->get_state() });
		const Transform3D og = o->get_global_transform();
		for (size_t i = 0; i < vs.size(); i++) {
			modal_verts.push_back(vs[i]);
			modal_weights.push_back(ws[i]);
			const Vector3 w = og.xform(o->get_data().pos[vs[i]]);
			modal_orig.push_back(w);
			modal_owner.push_back((int)k + 1);
			if (ws[i] >= 1.0f) {
				sum += w;
				cnt++;
			}
		}
	}
	edited_id = keep;
	if (cnt > 0 && dock->pivot_mode != HewnDock::PIVOT_CURSOR && dock->pivot_mode != HewnDock::PIVOT_ACTIVE) {
		modal_pivot = sum / (float)cnt; // (the middle of all the selections)
	}
	modal_vpivots.assign(modal_verts.size(), modal_pivot);
}

bool HewnEditorPlugin::_start_transform(Modal p_kind, Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (!n || mode == MODE_OBJECT || !p_cam) {
		return false;
	}
	modal_object = false;
	if (_selected_verts().empty()) {
		return false;
	}
	modal_multi_ok = !editing; // (G / R / S of the selections as they are: the other edited meshes too)
	if (!editing) {
		modal_origin = 0;
	}
	static const char *names[3] = { "Move", "Rotate", "Scale" };
	if (!editing) {
		begin_edit(names[p_kind - MODAL_MOVE]);
	}
	_setup_modal_set(p_cam, p_kind);
	modal_orient = _orientation_basis(dock->orientation, p_cam);
	modal_mouse0 = mouse_pos;
	_reset_precise();
	modal_axis = -1;
	modal_plane = false;
	modal_local = false;
	modal_num = "";
	modal_snapped = false;
	_plane_hit(p_cam, modal_mouse0, modal_pivot, cam_forward(p_cam), modal_hit0);
	_gather_snap_candidates(true);
	modal = p_kind;
	drag = DRAG_NONE;
	update_overlays();
	return true;
}

void HewnEditorPlugin::_start_extrude(Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	hewn::Mesh &m = n->get_data();
	hewn::Selection &s = n->selection;
	const Transform3D g = n->get_global_transform();
	Vector3 normal;
	if (mode == MODE_FACE) {
		if (s.faces.empty()) {
			return;
		}
		for (int f : s.faces) {
			normal += hewn::face_normal_raw(m, m.faces[f]);
		}
		begin_edit("Extrude");
		hewn::ops::extrude_faces(m, s.faces, 0.0f, false);
	} else {
		if (_selected_verts().empty()) {
			return;
		}
		begin_edit("Extrude");
		if (!_extrude_edges_verts(m, s, 0.0f)) {
			n->set_state(edit_before);
			editing = false;
			return;
		}
	}
	n->commit(true);
	if (!_start_transform(MODAL_MOVE, p_cam)) {
		n->set_state(edit_before);
		editing = false;
		return;
	}
	modal_keep_on_cancel = true;
	modal_origin = 1;
	const Vector3 wn = g.basis.inverse().transposed().xform(normal);
	if (wn.length() > 1e-6f) {
		modal_axis = 3;
		modal_custom_dir = wn.normalized();
	}
}

void HewnEditorPlugin::_start_duplicate(Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (!n || n->selection.faces.empty()) {
		return;
	}
	begin_edit("Duplicate");
	n->selection.faces = hewn::ops::duplicate_faces(n->get_data(), n->selection.faces);
	n->commit(true);
	if (!_start_transform(MODAL_MOVE, p_cam)) {
		n->set_state(edit_before);
		editing = false;
	}
	modal_keep_on_cancel = modal == MODAL_MOVE;
	modal_origin = 2;
}

void HewnEditorPlugin::_start_bevel(Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	hewn::Mesh &m = n->get_data();
	hewn::Selection &s = n->selection;
	hewn::Topology t;
	t.build(m);
	std::vector<hewn::Edge> edges;
	if (mode == MODE_EDGE) {
		edges = s.edges;
	} else if (mode == MODE_VERTEX) {
		edges = hewn::sel::edges_from_verts(m, t, s.verts);
	} else {
		// Boundary of the selected region (edges with exactly one selected face).
		std::unordered_set<int> fs(s.faces.begin(), s.faces.end());
		for (size_t e = 0; e < t.edges.size(); e++) {
			int count = 0;
			for (int f : t.edge_faces[e]) {
				count += fs.count(f) ? 1 : 0;
			}
			if (count == 1 && t.edge_faces[e].size() == 2) {
				edges.push_back(t.edges[e]);
			}
		}
	}
	if (edges.empty()) {
		return;
	}
	begin_edit("Bevel");
	modal_backup = m;
	modal_sel_backup = s;
	modal_edges = edges;
	const Transform3D g = n->get_global_transform();
	Vector3 c;
	std::vector<int> vs = hewn::sel::verts_of_edges(edges);
	for (int v : vs) {
		c += g.xform(m.pos[v]);
	}
	modal_pivot = c / (float)vs.size();
	modal_mouse0 = mouse_pos;
	_reset_precise();
	// from 0, a pixel further from the middle than at the start is a pixel's size in the world there
	// (so alike for a big object far away and a small one near)
	bevel_d0 = mouse_pos.distance_to(p_cam->unproject_position(modal_pivot));
	modal_scale_k = _world_per_pixel(p_cam, modal_pivot);
	bevel_segments = MAX(1, (int)op_param("bevel", "segments"));
	modal_num = "";
	modal_axis = -1;
	modal_object = false;
	modal = MODAL_BEVEL;
	drag = DRAG_NONE;
	_update_modal(p_cam, false, false);
}

hewn::ops::InsetOptions HewnEditorPlugin::_inset_options(float p_amount) {
	hewn::ops::InsetOptions o;
	o.thickness = p_amount;
	o.depth = (float)op_param("inset", "depth");
	o.boundary = (bool)op_param("inset", "boundary");
	o.even = (bool)op_param("inset", "even");
	o.relative = (bool)op_param("inset", "relative");
	o.edge_rail = (bool)op_param("inset", "edge_rail");
	o.outset = (bool)op_param("inset", "outset");
	o.select_outer = (bool)op_param("inset", "select_outer");
	o.interpolate = (bool)op_param("inset", "interpolate");
	o.individual = modal == MODAL_INSET ? inset_individual : (bool)op_param("inset", "individual");
	return o;
}

void HewnEditorPlugin::_start_inset(Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (!n || n->selection.faces.empty()) {
		return;
	}
	begin_edit("Inset");
	set_op_param("inset", "depth", 0.0); // (a new inset starts flat; the panel sets the depth afterwards)
	modal_backup = n->get_data();
	modal_sel_backup = n->selection;
	const Transform3D g = n->get_global_transform();
	Vector3 c;
	std::vector<int> vs = hewn::sel::verts_of_faces(n->get_data(), n->selection.faces);
	for (int v : vs) {
		c += g.xform(n->get_data().pos[v]);
	}
	modal_pivot = c / (float)vs.size();
	modal_mouse0 = mouse_pos;
	_reset_precise();
	modal_scale_k = _world_per_pixel(p_cam, modal_pivot);
	modal_num = "";
	modal_axis = -1;
	modal_object = false;
	modal = MODAL_INSET;
	drag = DRAG_NONE;
	_update_modal(p_cam, false, false);
}

// B during G: pick the source point, then the target point. Points are averaged
// with weights (A adds the hovered point again); they snap like the toolbar snapping says.
void HewnEditorPlugin::_start_snap(Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if ((!n && !modal_object) || !p_cam || (modal != MODAL_MOVE && modal != MODAL_ROTATE && modal != MODAL_SCALE)) {
		return;
	}
	snap_kind = modal;
	// start over from the original positions (objects: keep moving the nodes, see _apply_snap_base)
	if (modal_object) {
		for (size_t i = 0; i < obj_nodes.size(); i++) {
			if (Node3D *x = Object::cast_to<Node3D>(ObjectDB::get_instance(obj_nodes[i]))) {
				x->set_global_transform(obj_orig[i]);
			}
		}
	} else {
		_apply_positions(modal_orig);
	}
	modal_axis = -1;
	modal_plane = false;
	modal_local = false;
	modal_num = "";
	snap_phase = 0;
	snap_picks.clear();
	_gather_snap_candidates(false);
	modal = MODAL_SNAP;
	drag = DRAG_NONE;
	_update_modal(p_cam, false, false);
}

// ------------------------------------------------------------------ object mode G / R / S

bool HewnEditorPlugin::_moving_node(const Node *p_node) const {
	if (!modal_object || !p_node) {
		return false;
	}
	for (const ObjectID &id : obj_nodes) {
		if ((uint64_t)id == p_node->get_instance_id()) {
			return true;
		}
	}
	return false;
}

// G / R / S on objects: the selected nodes move, turn or scale with the same constraints,
// typed values, snapping and pivot as the vertices in edit mode.
bool HewnEditorPlugin::_start_object_transform(Modal p_kind, Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (!p_cam || mode != MODE_OBJECT || modal != MODAL_NONE) {
		return false;
	}
	obj_nodes.clear();
	obj_orig.clear();
	TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	for (int64_t i = 0; i < sel.size(); i++) {
		Node3D *x = Object::cast_to<Node3D>(sel[i].operator Object *());
		if (x && x->is_inside_tree()) {
			obj_nodes.push_back(ObjectID(x->get_instance_id()));
			obj_orig.push_back(x->get_global_transform());
		}
	}
	if (obj_nodes.empty() && n && n->is_inside_tree()) {
		obj_nodes.push_back(ObjectID(n->get_instance_id()));
		obj_orig.push_back(n->get_global_transform());
	}
	if (obj_nodes.empty()) {
		return false;
	}
	modal_object = true;
	modal_object_xf = n && _moving_node(n) ? n->get_global_transform() : obj_orig[0];
	// snap bases (the point brought onto a snap target): the vertices of the meshes, and the origins
	modal_orig.clear();
	modal_verts.clear();
	Vector3 median;
	for (size_t i = 0; i < obj_nodes.size(); i++) {
		median += obj_orig[i].origin;
		HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(obj_nodes[i]));
		if (pm) {
			for (const Vector3 &p : pm->get_data().pos) {
				modal_orig.push_back(obj_orig[i].xform(p));
			}
		}
		if (HewnCutter3D *cu = Object::cast_to<HewnCutter3D>(ObjectDB::get_instance(obj_nodes[i]))) {
			// (a cutter's corners: brought onto vertices, edges, faces)
			const std::vector<std::vector<Vector3>> ol = cu->local_outline();
			for (size_t k = 0; k < ol.size() && k < 2; k++) {
				for (const Vector3 &p : ol[k]) {
					modal_orig.push_back(obj_orig[i].xform(p));
				}
			}
		}
		modal_orig.push_back(obj_orig[i].origin);
	}
	median /= (float)obj_nodes.size();
	modal_sel_count = modal_orig.size();
	modal_weights.assign(modal_orig.size(), 1.0f);
	switch (dock->pivot_mode) {
		case HewnDock::PIVOT_CURSOR:
			modal_pivot = cursor_pos;
			break;
		case HewnDock::PIVOT_ACTIVE:
			modal_pivot = modal_object_xf.origin;
			break;
		default:
			modal_pivot = median;
			break;
	}
	modal_vpivots.assign(modal_orig.size(), modal_pivot);
	modal_orient = _orientation_basis(dock->orientation, p_cam);
	modal_mouse0 = mouse_pos;
	_reset_precise();
	modal_axis = -1;
	modal_plane = false;
	modal_local = false;
	modal_num = "";
	modal_snapped = false;
	modal_origin = 0;
	res_delta = Vector3();
	res_angle = 0.0f;
	res_scale = 1.0f;
	_plane_hit(p_cam, modal_mouse0, modal_pivot, cam_forward(p_cam), modal_hit0);
	modal = p_kind;
	_gather_snap_candidates(true);
	drag = DRAG_NONE;
	_update_modal(p_cam, snap_increment_on(false), false);
	return true;
}

void HewnEditorPlugin::_apply_object_transforms() {
	const bool individual = dock->pivot_mode == HewnDock::PIVOT_INDIVIDUAL;
	Basis M; // rotation / scale around the pivot
	if (modal == MODAL_ROTATE && res_axis.length() > 1e-9f) {
		M = Basis(res_axis.normalized(), res_angle * (float)Math::PI / 180.0f);
	} else if (modal == MODAL_SCALE) {
		const float s = res_scale;
		const Vector3 d = res_dir.length() > 1e-9f ? res_dir.normalized() : Vector3(1, 0, 0);
		for (int j = 0; j < 3; j++) {
			Vector3 e;
			e[j] = 1.0f;
			if (res_constraint == 0) {
				M.set_column(j, e * s);
			} else if (res_constraint == 1) {
				M.set_column(j, e + d * (d[j] * (s - 1.0f))); // along the axis
			} else {
				M.set_column(j, e * s - d * (d[j] * (s - 1.0f))); // across the plane
			}
		}
	}
	for (size_t i = 0; i < obj_nodes.size(); i++) {
		Node3D *x = Object::cast_to<Node3D>(ObjectDB::get_instance(obj_nodes[i]));
		if (!x || !x->is_inside_tree()) {
			continue;
		}
		const Transform3D &o = obj_orig[i];
		Transform3D t = o;
		if (modal == MODAL_MOVE) {
			t.origin += res_delta;
		} else {
			const Vector3 p = individual ? o.origin : modal_pivot;
			t.basis = M * o.basis;
			t.origin = p + M.xform(o.origin - p);
		}
		x->set_global_transform(t);
	}
}

// ------------------------------------------------------------------ update

void HewnEditorPlugin::_apply_positions(const std::vector<Vector3> &p_world) {
	HW_PROF("apply_positions");
	HewnMesh3D *n = get_edited();
	if (!n || modal_object) {
		return; // (objects: _apply_object_transforms)
	}
	const Transform3D inv = n->get_global_transform().affine_inverse();
	hewn::Mesh &m = n->get_data();
	std::vector<Vector3> before;
	std::vector<int> own;
	const bool multi = modal_owner.size() == modal_verts.size() && !multi_before.empty();
	for (size_t i = 0; i < modal_verts.size() && i < p_world.size(); i++) {
		if (multi && modal_owner[i] != 0) {
			continue;
		}
		m.pos[modal_verts[i]] = inv.xform(p_world[i]);
		own.push_back(modal_verts[i]);
		before.push_back(inv.xform(i < modal_orig.size() ? modal_orig[i] : p_world[i]));
	}
	n->apply_mirror_clipping(own, before);
	// (a dense mesh: only the positions shown while it moves, the whole of it made again when the move ends)
	if (m.faces.size() >= 3000 || n->has_heavy_modifiers()) {
		n->commit_positions();
	} else {
		n->commit(true);
	}
	if (!multi) {
		return;
	}
	for (size_t k = 0; k < multi_ids.size(); k++) {
		HewnMesh3D *o = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(multi_ids[k])));
		if (!o) {
			continue;
		}
		const Transform3D oinv = o->get_global_transform().affine_inverse();
		bool any = false;
		for (size_t i = 0; i < modal_verts.size() && i < p_world.size(); i++) {
			if (modal_owner[i] == (int)k + 1) {
				o->get_data().pos[modal_verts[i]] = oinv.xform(p_world[i]);
				any = true;
			}
		}
		if (any) {
			if (o->get_data().faces.size() >= 3000) {
				o->commit_positions();
			} else {
				o->commit(true);
			}
		}
	}
}

// Rotation of the modal set around its pivots (weights: proportional editing).
void HewnEditorPlugin::_apply_rotation(const Vector3 &p_axis, float p_deg) {
	res_axis = p_axis;
	res_angle = p_deg;
	std::vector<Vector3> pos;
	for (size_t i = 0; i < modal_orig.size(); i++) {
		const Basis rot(p_axis, p_deg * modal_weights[i] * (float)Math::PI / 180.0f);
		pos.push_back(modal_vpivots[i] + rot.xform(modal_orig[i] - modal_vpivots[i]));
	}
	_apply_positions(pos);
}

// Scale of the modal set: uniform, along the constraint axis, or across its plane.
void HewnEditorPlugin::_apply_scaling(float p_s) {
	const Vector3 d = _axis_dir(modal_axis);
	std::vector<Vector3> pos;
	for (size_t i = 0; i < modal_orig.size(); i++) {
		const float si = 1.0f + (p_s - 1.0f) * modal_weights[i];
		const Vector3 pv = modal_vpivots[i];
		const Vector3 r = modal_orig[i] - pv;
		Vector3 p;
		if (modal_axis < 0) {
			p = pv + r * si;
		} else if (modal_plane) {
			p = pv + r * si - d * (r.dot(d) * (si - 1.0f));
		} else {
			p = pv + r + d * (r.dot(d) * (si - 1.0f));
		}
		pos.push_back(p);
	}
	_apply_positions(pos);
	res_scale = p_s;
	res_dir = d;
	res_constraint = modal_axis < 0 ? 0 : modal_plane ? 2 : 1;
}

// B (snap base): the picked source point is brought to the target by the transform B was pressed in:
// moved onto it, turned towards it around the pivot, or scaled to its distance from the pivot.
void HewnEditorPlugin::_apply_snap_base(const Vector3 &p_target, const String &p_constraint) {
	if (snap_kind == MODAL_ROTATE) {
		Vector3 a = snap_source - modal_pivot, b = p_target - modal_pivot;
		Vector3 ax = modal_axis >= 0 ? _axis_dir(modal_axis) : a.cross(b);
		float deg = 0.0f;
		if (ax.length() > 1e-9f) {
			ax.normalize();
			a -= ax * a.dot(ax);
			b -= ax * b.dot(ax);
			if (a.length() > 1e-6f && b.length() > 1e-6f) {
				deg = std::atan2(ax.dot(a.cross(b)), a.dot(b)) * 180.0f / (float)Math::PI;
			}
		} else {
			ax = Vector3(0, 1, 0);
		}
		_apply_rotation(ax, deg);
		modal_label = vformat(T("Snap Rotate: pick the TARGET point  %s  %.2f deg"), p_constraint, deg);
	} else if (snap_kind == MODAL_SCALE) {
		const Vector3 d = _axis_dir(modal_axis);
		const Vector3 a = snap_source - modal_pivot, b = p_target - modal_pivot;
		float la, lb;
		if (modal_axis >= 0 && !modal_plane) {
			la = a.dot(d);
			lb = b.dot(d);
		} else if (modal_axis >= 0) {
			la = (a - d * a.dot(d)).length();
			lb = (b - d * b.dot(d)).length();
		} else {
			la = a.length();
			lb = b.length();
		}
		const float s = std::fabs(la) > 1e-6f ? lb / la : 1.0f;
		_apply_scaling(s);
		modal_label = vformat(T("Snap Scale: pick the TARGET point  %s  %.4f"), p_constraint, s);
	} else {
		const Vector3 delta = _constrain(p_target - snap_source);
		std::vector<Vector3> pos;
		for (size_t i = 0; i < modal_orig.size(); i++) {
			pos.push_back(modal_orig[i] + delta * modal_weights[i]);
		}
		_apply_positions(pos);
		res_delta = delta;
		modal_label = vformat(T("Snap: pick the TARGET point  %s  D: %.4f"), p_constraint, delta.length());
	}
	if (modal_object) {
		const Modal was = modal;
		modal = snap_kind; // (the transform of the nodes is the one B was pressed in)
		_apply_object_transforms();
		modal = was;
	}
}

void HewnEditorPlugin::_update_modal(Camera3D *p_cam, bool p_snap, bool p_magnet) {
	HW_PROF("update_modal");
	HewnMesh3D *n = get_edited();
	if ((!n && !modal_object) || !p_cam) {
		return;
	}
	const float sm = (float)dock->snap_move->get_value();
	const float sr = (float)dock->snap_rotate->get_value();
	const float ss = (float)dock->snap_scale->get_value();
	auto snapf = [&](float x, float step) { return (p_snap && step > 0.0f) ? std::round(x / step) * step : x; };
	const Vector3 fwd = cam_forward(p_cam);
	const Vector2 sp = p_cam->unproject_position(modal_pivot);
	static const char *axis_names[4] = { "X", "Y", "Z", "N" };
	String constraint;
	if (modal_axis >= 0) {
		constraint = String(modal_plane ? T("plane ") : T("along ")) + (modal_plane && modal_axis < 3 ? (modal_axis == 0 ? "YZ" : modal_axis == 1 ? "XZ" : "XY") : axis_names[modal_axis]);
		if (modal_axis < 3) {
			const int o = dock->orientation;
			constraint += String(" (") + (modal_local ? (o == HewnDock::ORIENT_GLOBAL ? T("Local") : T("Global")) : T(dock->orient_name(o))) + ")";
		}
	}
	modal_snapped = false;
	float num;
	const bool has_num = _modal_number(num);
	// Snapping to geometry (vertex / edge / face targets): the selected vertex closest to the target is
	// the one brought onto it, by moving, rotating or scaling.
	Vector3 snap_tgt, snap_src;
	bool geo_snap = false;
	if (!has_num && p_magnet && (dock->snap_targets & HewnDock::SNAP_GEOMETRY) && _snap_target(p_cam, mouse_pos, snap_tgt)) {
		modal_snap_kind = last_snap_kind;
		float best = 1e30f;
		snap_src = modal_pivot;
		for (size_t i = 0; i < modal_sel_count; i++) {
			if (modal_orig[i].distance_to(snap_tgt) < best) {
				best = modal_orig[i].distance_to(snap_tgt);
				snap_src = modal_orig[i];
			}
		}
		geo_snap = true;
	}

	if (modal == MODAL_MOVE) {
		Vector3 delta;
		Vector3 target;
		if (has_num) {
			const Vector3 d = modal_axis >= 0 && !modal_plane ? _axis_dir(modal_axis) : AXES[0];
			delta = d * num;
		} else if (p_magnet && (dock->snap_targets & HewnDock::SNAP_GEOMETRY) && _snap_target(p_cam, mouse_pos, target)) {
			// Snap the selected vertex closest to the target onto it (keeping the constraint).
			float best = 1e30f;
			Vector3 base = modal_pivot;
			for (size_t i = 0; i < modal_sel_count; i++) {
				if (modal_orig[i].distance_to(target) < best) {
					best = modal_orig[i].distance_to(target);
					base = modal_orig[i];
				}
			}
			delta = _constrain(target - base);
			modal_snapped = true;
			modal_snap_point = target;
			modal_snap_kind = last_snap_kind;
		} else if (p_snap && snap_grid_on() && sm > 0.0f) {
			// Grid: the pivot (object origin, selection center...) moves onto grid points
			Vector3 raw;
			if (modal_axis >= 0 && !modal_plane) {
				const Vector3 d = _axis_dir(modal_axis);
				raw = d * (_axis_param(p_cam, mouse_pos, modal_pivot, d) - _axis_param(p_cam, modal_mouse0, modal_pivot, d));
			} else {
				const Vector3 pn = modal_axis >= 0 ? _axis_dir(modal_axis) : fwd;
				Vector3 h0, h1;
				if (_plane_hit(p_cam, modal_mouse0, modal_pivot, pn, h0) && _plane_hit(p_cam, mouse_pos, modal_pivot, pn, h1)) {
					raw = h1 - h0;
				}
			}
			delta = _grid_point(modal_pivot + raw, modal_orient, sm) - modal_pivot;
			if (modal_axis >= 0) {
				delta = _constrain(delta);
			}
			modal_snapped = true;
			modal_snap_point = modal_pivot + delta;
			modal_snap_kind = SNAP_KIND_GRID;
		} else if (modal_axis >= 0 && !modal_plane) {
			const Vector3 d = _axis_dir(modal_axis);
			delta = d * snapf(_axis_param(p_cam, mouse_pos, modal_pivot, d) - _axis_param(p_cam, modal_mouse0, modal_pivot, d), sm);
		} else {
			const Vector3 pn = modal_axis >= 0 ? _axis_dir(modal_axis) : fwd;
			Vector3 h0, h1;
			if (_plane_hit(p_cam, modal_mouse0, modal_pivot, pn, h0) && _plane_hit(p_cam, mouse_pos, modal_pivot, pn, h1)) {
				delta = h1 - h0;
				if (p_snap && sm > 0.0f) {
					// increments along the orientation axes
					Vector3 l = modal_orient.transposed().xform(delta);
					l = Vector3(snapf(l.x, sm), snapf(l.y, sm), snapf(l.z, sm));
					delta = modal_orient.xform(l);
				}
				if (modal_axis >= 0) {
					delta = _constrain(delta);
				}
			}
		}
		std::vector<Vector3> pos;
		for (size_t i = 0; i < modal_orig.size(); i++) {
			pos.push_back(modal_orig[i] + delta * modal_weights[i]);
		}
		_apply_positions(pos);
		res_delta = delta;
		modal_label = vformat(T("Move  %s  D: %.4f  (%.3f, %.3f, %.3f)"), constraint, delta.length(), delta.x, delta.y, delta.z);
	} else if (modal == MODAL_ROTATE) {
		const Vector3 axis = modal_axis >= 0 ? _axis_dir(modal_axis) : fwd;
		float deg;
		if (has_num) {
			deg = num;
		} else {
			const Vector2 a = modal_mouse0 - sp, b = mouse_pos - sp;
			float ang = std::atan2(a.cross(b), a.dot(b)); // clockwise on screen is positive (y down)
			if (axis.dot(fwd) < 0.0f) {
				ang = -ang;
			}
			deg = snapf(ang * 180.0f / (float)Math::PI, sr);
			if (geo_snap) {
				// the angle that turns the closest selected vertex towards the target, around the axis
				const Vector3 ax = axis.normalized();
				Vector3 a3 = snap_src - modal_pivot, b3 = snap_tgt - modal_pivot;
				a3 -= ax * a3.dot(ax);
				b3 -= ax * b3.dot(ax);
				if (a3.length() > 1e-6f && b3.length() > 1e-6f) {
					deg = std::atan2(ax.dot(a3.cross(b3)), a3.dot(b3)) * 180.0f / (float)Math::PI;
					modal_snapped = true;
					modal_snap_point = snap_tgt;
				}
			}
		}
		_apply_rotation(axis, deg);
		modal_label = vformat(T("Rotate  %s  %.2f deg"), constraint.is_empty() ? String(T("view")) : constraint, deg);
	} else if (modal == MODAL_SCALE) {
		float s;
		if (has_num) {
			s = num;
		} else {
			const float d0 = MAX(1.0f, modal_mouse0.distance_to(sp));
			s = snapf(mouse_pos.distance_to(sp) / d0, ss);
		}
		const Vector3 d = _axis_dir(modal_axis);
		if (geo_snap) {
			// the factor that puts the closest selected vertex at the target's distance (along the axis /
			// across the plane / overall)
			const Vector3 a3 = snap_src - modal_pivot, b3 = snap_tgt - modal_pivot;
			float la, lb;
			if (modal_axis >= 0 && !modal_plane) {
				la = a3.dot(d);
				lb = b3.dot(d);
			} else if (modal_axis >= 0) {
				la = (a3 - d * a3.dot(d)).length();
				lb = (b3 - d * b3.dot(d)).length();
			} else {
				la = a3.length();
				lb = b3.length();
			}
			if (std::fabs(la) > 1e-6f) {
				s = lb / la;
				modal_snapped = true;
				modal_snap_point = snap_tgt;
			}
		}
		_apply_scaling(s);
		modal_label = vformat(T("Scale  %s  %.4f"), constraint, s);
	} else if (modal == MODAL_SLIDE) {
		// Each vertex slides along the incident edge that best matches the mouse direction.
		const hewn::Mesh &m = n->get_data();
		const Transform3D g = n->get_global_transform();
		hewn::Topology t;
		t.build(m);
		std::unordered_set<int> moving(modal_verts.begin(), modal_verts.end());
		const Vector2 dir = mouse_pos - modal_mouse0;
		std::unordered_map<int, Vector3> orig_of;
		for (size_t i = 0; i < modal_verts.size(); i++) {
			orig_of[modal_verts[i]] = modal_orig[i];
		}
		auto world_of = [&](int v) {
			auto it = orig_of.find(v);
			return it != orig_of.end() ? it->second : g.xform(m.pos[v]);
		};
		bool picked = false;
		for (int t : slide_to) {
			picked = picked || t >= 0;
		}
		// C (unclamped): the edges are locked, the vertices stay on the lines they slid along
		if (dir.length() > 3.0f && !(slide_unclamped && picked)) {
			for (size_t i = 0; i < modal_verts.size(); i++) {
				const int v = modal_verts[i];
				const Vector2 sv = p_cam->unproject_position(modal_orig[i]);
				float best = -2.0f;
				int pick = -1;
				for (int e : t.vert_edges[v]) {
					const hewn::Edge &E = t.edges[e];
					const int o = E.a == v ? E.b : E.a;
					if (moving.size() > 1 && moving.count(o)) {
						continue; // slide towards the neighboring loop, not along the selection
					}
					const Vector2 d = p_cam->unproject_position(world_of(o)) - sv;
					if (d.length() < 1e-3f) {
						continue;
					}
					float score = d.normalized().dot(dir.normalized());
					if (slide_unclamped) {
						score = std::fabs(score);
					}
					if (score > best) {
						best = score;
						pick = o;
					}
				}
				slide_to[i] = pick;
			}
		}
		// The slide factor comes from the vertex nearest to where the drag started. The mouse moves it as fast
		// as it moves a free point there (the mouse along the edge on screen, in meters at the vertex's
		// depth): an edge going away from the view does not race.
		float t_val = 0.0f;
		if (has_num) {
			t_val = num;
		} else {
			float best = 1e30f;
			int ref = -1;
			for (size_t i = 0; i < modal_verts.size(); i++) {
				if (slide_to[i] < 0) {
					continue;
				}
				const Vector2 sv = p_cam->unproject_position(modal_orig[i]);
				const float d = sv.distance_to(modal_mouse0);
				if (d < best) {
					best = d;
					ref = (int)i;
					const Vector2 es = p_cam->unproject_position(world_of(slide_to[i])) - sv;
					const float len = world_of(slide_to[i]).distance_to(modal_orig[i]);
					t_val = es.length_squared() > 1e-6f && len > 1e-6f ? dir.dot(es.normalized()) * _world_per_pixel(p_cam, modal_orig[i]) / len : 0.0f;
				}
			}
			if (ref >= 0) {
				// snapping: onto the point of the edge nearest to a snap target, or the distance slid in
				// steps of the grid
				const Vector3 a = modal_orig[ref];
				const Vector3 e = world_of(slide_to[ref]) - a;
				const float len = e.length();
				if (len > 1e-6f && geo_snap) {
					t_val = (snap_tgt - a).dot(e) / (len * len);
					modal_snapped = true;
					modal_snap_point = snap_tgt;
				} else if (len > 1e-6f && p_snap && sm > 0.0f) {
					t_val = std::round(t_val * len / sm) * sm / len;
				}
			}
		}
		if (!slide_unclamped) {
			t_val = CLAMP(t_val, 0.0f, 1.0f);
		}
		std::vector<Vector3> pos;
		for (size_t i = 0; i < modal_verts.size(); i++) {
			pos.push_back(slide_to[i] < 0 ? modal_orig[i] : modal_orig[i] + (world_of(slide_to[i]) - modal_orig[i]) * t_val);
		}
		_apply_positions(pos);
		modal_label = vformat(T("Slide  %.3f  %s   (C: %s)"), t_val, slide_unclamped ? T("unclamped, edges locked") : T("clamped"), slide_unclamped ? T("clamp") : T("extend along the current edges"));
	} else if (modal == MODAL_BEVEL) {
		float amount = has_num ? num : snapf(MAX(0.0f, mouse_pos.distance_to(sp) - bevel_d0) * modal_scale_k, sm > 0.0f ? sm * 0.1f : 0.0f);
		amount = MAX(0.0f, amount);
		hewn::Mesh &m = n->get_data();
		m = modal_backup;
		n->selection = modal_sel_backup;
		const std::vector<Vector2> prof = (int)op_param("bevel", "profile_type") == 1 ? _bevel_profile()->to_vector() : std::vector<Vector2>();
		std::vector<int> faces = hewn::ops::bevel_edges(m, modal_edges, amount, bevel_segments, false, prof.empty() ? nullptr : &prof);
		hewn::continue_uvs(m, faces, n->get_global_transform());
		n->selection.clear();
		n->selection.faces = faces;
		hewn::remove_unused_vertices(m, &n->selection);
		n->commit(true);
		res_amount = amount;
		modal_label = vformat(T("Bevel  width: %.4f  segments: %d   (wheel / +- segments)"), amount, bevel_segments);
	} else if (modal == MODAL_VALUE) {
		float value = has_num ? num : _value_from_mouse(p_cam);
		if (p_snap && sm > 0.0f && value_op != "shear") {
			value = std::round(value / (sm * 0.1f)) * (sm * 0.1f);
		}
		hewn::Mesh &m = n->get_data();
		m = modal_backup;
		n->selection = modal_sel_backup;
		_store_value_params(value_op);
		_value_op_apply(value_op, value);
		hewn::remove_unused_vertices(m, &n->selection);
		n->commit(true);
		res_amount = value;
		modal_label = vformat(T("%s  %.4f"), value_op.capitalize(), value);
	} else if (modal == MODAL_EXTRUDE_AMOUNT) {
		// moving away from the selection center extrudes outwards
		float amount = has_num ? num : snapf((mouse_pos.distance_to(sp) - modal_mouse0.distance_to(sp)) * modal_scale_k, sm);
		hewn::Mesh &m = n->get_data();
		m = modal_backup;
		n->selection = modal_sel_backup;
		if (extrude_kind == 1) {
			n->selection.faces = hewn::ops::extrude_along_normals(m, modal_sel_backup.faces, amount);
		} else {
			n->selection.faces = hewn::ops::extrude_faces(m, modal_sel_backup.faces, amount, true);
		}
		hewn::remove_unused_vertices(m, &n->selection);
		n->commit(true);
		res_amount = amount;
		modal_label = vformat(T("%s  %.4f"), extrude_kind == 1 ? T("Extrude Along Normals") : T("Extrude Individual"), amount);
	} else if (modal == MODAL_INSET) {
		float amount = has_num ? num : snapf((modal_mouse0.distance_to(sp) - mouse_pos.distance_to(sp)) * modal_scale_k, sm > 0.0f ? sm * 0.1f : 0.0f);
		amount = MAX(0.0f, amount);
		hewn::Mesh &m = n->get_data();
		m = modal_backup;
		n->selection = modal_sel_backup;
		const hewn::ops::InsetOptions io = _inset_options(amount);
		if (amount > 0.0f || io.depth != 0.0f) {
			n->selection.faces = hewn::ops::inset_faces_ex(m, modal_sel_backup.faces, io);
			n->selection.verts = hewn::sel::verts_of_faces(m, n->selection.faces);
		}
		hewn::remove_unused_vertices(m, &n->selection);
		n->commit(true);
		res_amount = amount;
		modal_label = vformat(T("Inset  thickness: %.4f  %s%s%s   (I: individual, O: outset, B: boundary)"), amount, inset_individual ? T("individual") : T("region"),
				io.outset ? String(", ") + T("outset") : String(), io.boundary ? String() : String(", ") + T("boundary off"));
	} else if (modal == MODAL_SNAP) {
		snap_hover_valid = snapping_active(snap_ctrl) && _snap_target(p_cam, mouse_pos, snap_hover);
		snap_hover_kind = last_snap_kind;
		if (!snap_hover_valid) {
			Vector3 from, dir, nrm;
			_ray(p_cam, mouse_pos, from, dir);
			snap_hover_valid = _raycast_scene(from, dir, snap_hover, nrm, nullptr, nullptr, snap_phase == 1);
			Vector3 plain;
			if (_raycast_cand_tris(from, dir, plain) && (!snap_hover_valid || from.distance_to(plain) < from.distance_to(snap_hover))) {
				snap_hover = plain;
				snap_hover_valid = true;
			}
			if (!snap_hover_valid) {
				snap_hover_valid = _plane_hit(p_cam, mouse_pos, modal_pivot, fwd, snap_hover);
			}
		}
		// Weighted average of the picked points (plus the hovered one, as a preview).
		Vector3 sum;
		int w = 0;
		for (const SnapPick &p : snap_picks) {
			sum += p.p * (float)p.weight;
			w += p.weight;
		}
		if (snap_hover_valid) {
			sum += snap_hover;
			w += 1;
		}
		snap_avg_valid = w > 0;
		if (w > 0) {
			snap_avg = sum / (float)w;
		}
		const String kind = snap_kind == MODAL_ROTATE ? T("Snap Rotate") : snap_kind == MODAL_SCALE ? T("Snap Scale") : T("Snap");
		if (snap_phase == 1 && w > 0) {
			_apply_snap_base(sum / (float)w, constraint);
		} else {
			modal_label = kind + (snap_phase == 0 ? String(T(": pick the SOURCE point")) : String(T(": pick the TARGET point")));
		}
		modal_label += T("   (A: add point / weight, LMB: last point, X/Y/Z: axis)");
	}
	if (modal_object && (modal == MODAL_MOVE || modal == MODAL_ROTATE || modal == MODAL_SCALE)) {
		_apply_object_transforms();
		const int cnt = (int)obj_nodes.size();
		if (cnt > 1) {
			modal_label += vformat(T("   (%d objects)"), cnt);
		}
	}
	update_overlays();
}

void HewnEditorPlugin::_change_prop_radius(Camera3D *p_cam, float p_factor) {
	dock->prop_radius->set_value(MAX(0.001, dock->prop_radius->get_value() * p_factor));
	_apply_positions(modal_orig); // restore, then rebuild the affected set with the new radius
	_setup_modal_set(p_cam, modal);
}

// ------------------------------------------------------------------ finish

void HewnEditorPlugin::_confirm_modal() {
	HewnMesh3D *n = get_edited();
	const Modal was = modal;
	modal = MODAL_NONE;
	modal_keep_on_cancel = false;
	modal_snapped = false;
	modal_backup = hewn::Mesh();
	const Modal kind = was == MODAL_SNAP ? snap_kind : was; // (B: the transform it was pressed in)
	if (modal_object && (kind == MODAL_MOVE || kind == MODAL_ROTATE || kind == MODAL_SCALE)) {
		static const char *names[3] = { "Move", "Rotate", "Scale" };
		EditorUndoRedoManager *ur = get_undo_redo();
		std::vector<Transform3D> after;
		for (size_t i = 0; i < obj_nodes.size(); i++) {
			Node3D *x = Object::cast_to<Node3D>(ObjectDB::get_instance(obj_nodes[i]));
			after.push_back(x ? x->get_global_transform() : obj_orig[i]);
			if (x) {
				x->set_global_transform(obj_orig[i]);
			}
		}
		ur->create_action(String("Hewn: ") + names[kind - MODAL_MOVE] + " Objects", UndoRedo::MERGE_DISABLE, n);
		for (size_t i = 0; i < obj_nodes.size(); i++) {
			Node3D *x = Object::cast_to<Node3D>(ObjectDB::get_instance(obj_nodes[i]));
			if (x) {
				ur->add_do_method(x, "set_global_transform", after[i]);
				ur->add_undo_method(x, "set_global_transform", obj_orig[i]);
			}
		}
		ur->commit_action();
		modal_object = false;
		obj_nodes.clear();
		selection_changed();
		return;
	}
	if (n && editing) {
		// Parametric operations are replayed from the start state through run_op, so that the
		// Last Operation panel can adjust them afterwards (and the result is exactly reproducible).
		String op;
		auto store_xform = [&](const String &p_op) {
			set_op_param(p_op, "prop", dock->prop_enabled->is_pressed());
			set_op_param(p_op, "prop_size", dock->prop_radius->get_value());
			set_op_param(p_op, "prop_falloff", dock->prop_falloff->get_selected());
			set_op_param(p_op, "prop_connected", dock->prop_connected->is_pressed());
			set_op_param(p_op, "prop_projected", prop_proj_axis != Vector3());
			set_op_param(p_op, "prop_vx", prop_proj_axis.x);
			set_op_param(p_op, "prop_vy", prop_proj_axis.y);
			set_op_param(p_op, "prop_vz", prop_proj_axis.z);
			set_op_param(p_op, "pivot_mode", dock->pivot_mode);
		};
		if (was == MODAL_MOVE) {
			op = modal_origin == 1 ? "extrude_move" : modal_origin == 2 ? "duplicate_move" : modal_origin == 0 ? "translate" : "";
			set_op_param(op, "x", res_delta.x);
			set_op_param(op, "y", res_delta.y);
			set_op_param(op, "z", res_delta.z);
			if (op == "translate") {
				store_xform(op);
			}
		} else if (was == MODAL_ROTATE && modal_origin == 0) {
			op = "rotate";
			store_xform(op);
			set_op_param(op, "angle", res_angle);
			set_op_param(op, "ax", res_axis.x);
			set_op_param(op, "ay", res_axis.y);
			set_op_param(op, "az", res_axis.z);
			set_op_param(op, "px", modal_pivot.x);
			set_op_param(op, "py", modal_pivot.y);
			set_op_param(op, "pz", modal_pivot.z);
		} else if (was == MODAL_SCALE && modal_origin == 0) {
			op = "scale";
			store_xform(op);
			set_op_param(op, "factor", res_scale);
			set_op_param(op, "dx", res_dir.x);
			set_op_param(op, "dy", res_dir.y);
			set_op_param(op, "dz", res_dir.z);
			set_op_param(op, "constraint", res_constraint);
			set_op_param(op, "px", modal_pivot.x);
			set_op_param(op, "py", modal_pivot.y);
			set_op_param(op, "pz", modal_pivot.z);
		} else if (was == MODAL_VALUE) {
			op = value_op;
			set_op_param(op, "value", res_amount);
		} else if (was == MODAL_EXTRUDE_AMOUNT) {
			if (extrude_kind == 1) {
				op = "extrude_normals";
			} else {
				op = "extrude";
				set_op_param(op, "individual", true);
				set_op_param(op, "dissolve_orthogonal", false);
			}
			set_op_param(op, "distance", res_amount);
		} else if (was == MODAL_BEVEL) {
			op = "bevel";
			set_op_param(op, "width", res_amount);
			set_op_param(op, "segments", bevel_segments);
		} else if (was == MODAL_INSET) {
			op = "inset";
			set_op_param(op, "amount", res_amount);
			set_op_param(op, "individual", inset_individual);
		}
		if (op.is_empty()) {
			end_edit();
			_hide_redo();
		} else {
			n->set_state(edit_before);
			for (const std::pair<uint64_t, Dictionary> &mb : multi_before) {
				if (HewnMesh3D *o = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(mb.first)))) {
					o->set_state(mb.second); // (run_op does it on each of them again)
				}
			}
			multi_before.clear();
			modal_owner.clear();
			editing = false;
			mode = element_mode = edit_mode_before;
			run_op(op);
			_set_godot_gizmo_hidden(mode != MODE_OBJECT);
		}
	}
	selection_changed();
}

void HewnEditorPlugin::_cancel_modal() {
	HW_PROF("cancel_modal");
	HewnMesh3D *n = get_edited();
	if (n && modal_keep_on_cancel && editing && (modal == MODAL_MOVE || modal == MODAL_ROTATE || modal == MODAL_SCALE || modal == MODAL_SLIDE)) {
		// cancelling the move after an extrude / duplicate / rip keeps the new geometry in place.
		_apply_positions(modal_orig);
		modal = MODAL_NONE;
		modal_keep_on_cancel = false;
		modal_snapped = false;
		end_edit();
		return;
	}
	modal_keep_on_cancel = false;
	if (modal_object && (modal == MODAL_MOVE || modal == MODAL_ROTATE || modal == MODAL_SCALE || modal == MODAL_SNAP)) {
		for (size_t i = 0; i < obj_nodes.size(); i++) {
			Node3D *x = Object::cast_to<Node3D>(ObjectDB::get_instance(obj_nodes[i]));
			if (x) {
				x->set_global_transform(obj_orig[i]);
			}
		}
		modal_object = false;
		obj_nodes.clear();
	} else if (n && editing) {
		n->set_state(edit_before);
		editing = false;
	}
	for (const std::pair<uint64_t, Dictionary> &mb : multi_before) {
		if (HewnMesh3D *o = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(mb.first)))) {
			o->set_state(mb.second);
		}
	}
	multi_before.clear();
	modal_owner.clear();
	modal = MODAL_NONE;
	modal_snapped = false;
	modal_backup = hewn::Mesh();
	selection_changed();
}

// ------------------------------------------------------------------ input

int32_t HewnEditorPlugin::_modal_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	const bool is_transform = modal == MODAL_MOVE || modal == MODAL_ROTATE || modal == MODAL_SCALE;
	// Ctrl snaps to geometry (magnet), Shift slows the mouse down; grid snapping is the dock option.
	auto refresh = [&](bool ctrl_held) {
		snap_ctrl = ctrl_held;
		_update_modal(p_cam, snap_increment_on(ctrl_held), snapping_active(ctrl_held) && (is_transform || modal == MODAL_SLIDE));
	};

	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		mouse_pos = _precise_mouse(_wrap_mouse(p_cam, mm->get_position()), mm->is_shift_pressed());
		modal_cursor = mm->get_position();
		refresh(mm->is_ctrl_pressed() || mm->is_meta_pressed());
		return AFTER_GUI_INPUT_STOP;
	}

	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid()) {
		const bool wheel = mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP || mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN;
		if (!mb->is_pressed()) {
			const bool wheel_used = modal == MODAL_BEVEL || ((modal == MODAL_MOVE || modal == MODAL_ROTATE || modal == MODAL_SCALE) && dock->prop_enabled->is_pressed() && !modal_object);
			return wheel && !wheel_used ? AFTER_GUI_INPUT_PASS : AFTER_GUI_INPUT_STOP;
		}
		const MouseButton bi = mb->get_button_index();
		if (bi == MOUSE_BUTTON_RIGHT) {
			_cancel_modal();
			return AFTER_GUI_INPUT_STOP;
		}
		if (bi == MOUSE_BUTTON_LEFT) {
			if (modal == MODAL_SNAP) {
				if (snap_hover_valid) {
					snap_picks.push_back({ snap_hover, 1 });
				}
				if (snap_picks.empty()) {
					return AFTER_GUI_INPUT_STOP;
				}
				Vector3 sum;
				int w = 0;
				for (const SnapPick &p : snap_picks) {
					sum += p.p * (float)p.weight;
					w += p.weight;
				}
				const Vector3 point = sum / (float)w;
				snap_picks.clear();
				if (snap_phase == 0) {
					snap_source = point;
					snap_phase = 1;
					_gather_snap_candidates(true);
					refresh(false);
				} else {
					_apply_snap_base(point, String());
					_confirm_modal();
				}
				return AFTER_GUI_INPUT_STOP;
			}
			_confirm_modal();
			return AFTER_GUI_INPUT_STOP;
		}
		if ((modal == MODAL_MOVE || modal == MODAL_ROTATE || modal == MODAL_SCALE) && dock->prop_enabled->is_pressed() && !modal_object &&
				(bi == MOUSE_BUTTON_WHEEL_UP || bi == MOUSE_BUTTON_WHEEL_DOWN)) {
			_change_prop_radius(p_cam, bi == MOUSE_BUTTON_WHEEL_DOWN ? 1.1f : 1.0f / 1.1f);
			refresh(mb->is_ctrl_pressed());
			return AFTER_GUI_INPUT_STOP;
		}
		if (modal == MODAL_BEVEL && (bi == MOUSE_BUTTON_WHEEL_UP || bi == MOUSE_BUTTON_WHEEL_DOWN)) {
			bevel_segments = CLAMP(bevel_segments + (bi == MOUSE_BUTTON_WHEEL_UP ? 1 : -1), 1, 64);
			refresh(false);
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS; // let the camera zoom
	}

	Ref<InputEventKey> k = p_event;
	if (k.is_null()) {
		return AFTER_GUI_INPUT_PASS;
	}
	const bool ctrl = ctrl_after(k);
	if (!k->is_pressed()) {
		refresh(ctrl); // modifier released
		return AFTER_GUI_INPUT_STOP;
	}
	const Key code = key_code(k);
	if (code == KEY_ESCAPE) {
		_cancel_modal();
		return AFTER_GUI_INPUT_STOP;
	}
	if (code == KEY_ENTER || code == KEY_KP_ENTER || code == KEY_SPACE) {
		if (modal == MODAL_SNAP) {
			if (snap_phase == 1 && !snap_picks.empty()) {
				Ref<InputEventMouseButton> fake;
				fake.instantiate();
				fake->set_button_index(MOUSE_BUTTON_LEFT);
				fake->set_pressed(true);
				snap_hover_valid = false;
				return _modal_input(p_cam, fake);
			}
			return AFTER_GUI_INPUT_STOP;
		}
		_confirm_modal();
		return AFTER_GUI_INPUT_STOP;
	}
	if (k->is_echo()) {
		return AFTER_GUI_INPUT_STOP;
	}
	if (code == KEY_G && k->is_alt_pressed() && !ctrl && !k->is_shift_pressed()) {
		set_face_grid(!face_grid); // Alt+G: the grid on faces, also in the middle of G / R / S (not a G)
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	// Axis constraints: X/Y/Z, Shift = plane, pressing the same axis again switches to local, then off.
	if ((is_transform || (modal == MODAL_SNAP && snap_phase == 1)) && (code == KEY_X || code == KEY_Y || code == KEY_Z)) {
		const int axis = code == KEY_X ? 0 : code == KEY_Y ? 1 : 2;
		const bool plane = k->is_shift_pressed();
		if (modal_axis == axis && modal_plane == plane) {
			if (!modal_local) {
				modal_local = true;
			} else {
				modal_axis = -1;
				modal_local = false;
			}
		} else {
			modal_axis = axis;
			modal_plane = plane;
			modal_local = false;
		}
		refresh(ctrl);
		return AFTER_GUI_INPUT_STOP;
	}
	if (modal == MODAL_MOVE && !ctrl && code == KEY_G && !modal_object) {
		_start_slide(p_cam); // GG
		return AFTER_GUI_INPUT_STOP;
	}
	if (is_transform && !ctrl && code == KEY_B) {
		_start_snap(p_cam); // snap base points
		return AFTER_GUI_INPUT_STOP;
	}
	if (is_transform && !modal_object && !ctrl && code == KEY_O) {
		// O during G / R / S: proportional editing on / off for this operation and the next ones
		dock->prop_enabled->set_pressed(!dock->prop_enabled->is_pressed());
		_apply_positions(modal_orig);
		_setup_modal_set(p_cam, modal);
		refresh(ctrl);
		return AFTER_GUI_INPUT_STOP;
	}
	if (is_transform && dock->prop_enabled->is_pressed() && !modal_object && (code == KEY_PAGEUP || code == KEY_PAGEDOWN)) {
		_change_prop_radius(p_cam, code == KEY_PAGEUP ? 1.1f : 1.0f / 1.1f);
		refresh(ctrl);
		return AFTER_GUI_INPUT_STOP;
	}
	if (modal == MODAL_SLIDE && code == KEY_C) {
		slide_unclamped = !slide_unclamped;
		refresh(ctrl);
		return AFTER_GUI_INPUT_STOP;
	}
	if (modal == MODAL_SLIDE && code == KEY_G) {
		_apply_positions(modal_orig);
		modal = MODAL_MOVE;
		modal_mouse0 = mouse_pos;
	_reset_precise();
		refresh(ctrl);
		return AFTER_GUI_INPUT_STOP;
	}
	if (is_transform && !ctrl && (code == KEY_G || code == KEY_R || code == KEY_S)) {
		// Switch the transform kind, starting over from the original positions.
		_apply_positions(modal_orig);
		modal = code == KEY_G ? MODAL_MOVE : code == KEY_R ? MODAL_ROTATE : MODAL_SCALE;
		modal_mouse0 = mouse_pos;
	_reset_precise();
		modal_num = "";
		refresh(ctrl);
		return AFTER_GUI_INPUT_STOP;
	}
	if (modal == MODAL_SNAP && code == KEY_A) {
		if (snap_hover_valid) {
			bool merged = false;
			for (SnapPick &p : snap_picks) {
				if (p.p.distance_to(snap_hover) < 1e-5f) {
					p.weight++;
					merged = true;
				}
			}
			if (!merged) {
				snap_picks.push_back({ snap_hover, 1 });
			}
		}
		refresh(ctrl);
		return AFTER_GUI_INPUT_STOP;
	}
	if (modal == MODAL_INSET && code == KEY_I) {
		inset_individual = !inset_individual;
		refresh(ctrl);
		return AFTER_GUI_INPUT_STOP;
	}
	if (modal == MODAL_INSET && (code == KEY_O || code == KEY_B)) {
		const String key = code == KEY_O ? "outset" : "boundary";
		set_op_param("inset", key, !(bool)op_param("inset", key));
		refresh(ctrl);
		return AFTER_GUI_INPUT_STOP;
	}
	if (modal == MODAL_BEVEL && (code == KEY_EQUAL || code == KEY_KP_ADD || ((code == KEY_MINUS || code == KEY_KP_SUBTRACT) && modal_num.is_empty()))) {
		bevel_segments = CLAMP(bevel_segments + ((code == KEY_EQUAL || code == KEY_KP_ADD) ? 1 : -1), 1, 64);
		refresh(ctrl);
		return AFTER_GUI_INPUT_STOP;
	}
	if (modal != MODAL_SNAP) {
		String ch;
		if (code == KEY_BACKSPACE) {
			modal_num = modal_num.substr(0, MAX(0, modal_num.length() - 1));
			refresh(ctrl);
			return AFTER_GUI_INPUT_STOP;
		}
		const char32_t uc = k.is_valid() ? k->get_unicode() : 0;
		if (uc == '/' || uc == '*' || uc == '(' || uc == ')' || uc == '+' || code == KEY_KP_DIVIDE || code == KEY_KP_MULTIPLY ||
				(code == KEY_KP_ADD && !modal_num.is_empty())) {
			// expressions: 1/2, 2*3
			modal_num += code == KEY_KP_DIVIDE ? String("/") : code == KEY_KP_MULTIPLY ? String("*") : code == KEY_KP_ADD ? String("+") : String::chr(uc);
			refresh(ctrl);
			return AFTER_GUI_INPUT_STOP;
		}
		if (is_number_key(code, ch)) {
			const bool expr = modal_num.contains("/") || modal_num.contains("*") || modal_num.contains("+") || modal_num.contains("(");
			if (ch == "-" && !expr) {
				modal_num = modal_num.begins_with("-") ? modal_num.substr(1) : "-" + modal_num;
			} else {
				modal_num += ch;
			}
			refresh(ctrl);
			return AFTER_GUI_INPUT_STOP;
		}
	}
	refresh(ctrl);
	return AFTER_GUI_INPUT_STOP;
}

// ------------------------------------------------------------------ drawing

void HewnEditorPlugin::_draw_modal(Control *p_overlay, Camera3D *p_cam) {
	Ref<Font> font = p_overlay->get_theme_default_font();
	const Vector2 size = p_overlay->get_size();
	auto line_through = [&](const Vector3 &p, const Vector3 &d, const Color &c) {
		const float L = _handle_size(p_cam, p) * 40.0f;
		Vector3 a = p - d * L, b = p + d * L;
		// keep both ends in front of the camera
		const Vector3 eye = p_cam->get_global_position();
		const Vector3 fwd = cam_forward(p_cam);
		const float near = p_cam->get_near() * 2.0f + 0.01f; // strictly in front (is_position_behind)
		auto clip = [&](Vector3 &x) {
			const float dx = (x - eye).dot(fwd);
			const float dp = (p - eye).dot(fwd);
			if (dx < near && std::fabs(dx - dp) > 1e-6f) {
				x = p + (x - p) * ((dp - near) / (dp - dx));
			}
		};
		clip(a);
		clip(b);
		if (!p_cam->is_position_behind(a) && !p_cam->is_position_behind(b)) {
			p_overlay->draw_line(p_cam->unproject_position(a), p_cam->unproject_position(b), c, 1.5f, true);
		}
	};
	const Vector3 pivot = modal == MODAL_SNAP ? snap_source : modal_pivot;
	if (modal_axis >= 0 && modal != MODAL_BEVEL && modal != MODAL_INSET && (modal != MODAL_SNAP || snap_phase == 1)) {
		// Axis colors: X red, Y green, Z blue; the normal of an extrude is the local Z (blue).
		const Color c = modal_axis < 3 ? AXIS_COLORS[modal_axis] : AXIS_COLORS[2];
		if (modal_plane) {
			for (int i = 0; i < 3; i++) {
				if (i != modal_axis) {
					line_through(pivot, _axis_dir(i), AXIS_COLORS[i] * Color(1, 1, 1, 0.6f));
				}
			}
		} else {
			line_through(pivot, _axis_dir(modal_axis), c);
		}
	}
	if (modal == MODAL_SLIDE) {
		// GG: the edges the vertices slide along, in yellow.
		HewnMesh3D *n = get_edited();
		if (n) {
			const Transform3D g = n->get_global_transform();
			for (size_t i = 0; i < modal_verts.size() && i < slide_to.size(); i++) {
				if (slide_to[i] < 0) {
					continue;
				}
				const Vector3 a = modal_orig[i], b = g.xform(n->get_data().pos[slide_to[i]]);
				const Vector3 d = (b - a);
				if (slide_unclamped) {
					line_through(a, d.normalized(), HANDLE_HOVER * Color(1, 1, 1, 0.35f)); // (C: past the edge's ends)
				}
				if (!p_cam->is_position_behind(a) && !p_cam->is_position_behind(b)) {
					p_overlay->draw_line(p_cam->unproject_position(a), p_cam->unproject_position(b), HANDLE_HOVER, 2.0f, true);
				}
			}
		}
	}
	if (modal == MODAL_ROTATE || modal == MODAL_SCALE || modal == MODAL_BEVEL || modal == MODAL_INSET || modal == MODAL_EXTRUDE_AMOUNT || modal == MODAL_VALUE) {
		if (!p_cam->is_position_behind(modal_pivot)) {
			const Vector2 sp = p_cam->unproject_position(modal_pivot);
			// a black dashed line from the center of the operation to the cursor (the real one, also
			// while Shift slows the value down)
			p_overlay->draw_dashed_line(sp, modal_cursor, Color(0, 0, 0, 0.9f), 1.5f, 5.0f);
			p_overlay->draw_circle(sp, 3.0f, Color(0, 0, 0, 0.9f));
		}
	}
	if ((modal == MODAL_MOVE || modal == MODAL_ROTATE || modal == MODAL_SCALE) && dock->prop_enabled->is_pressed() && !modal_object && !p_cam->is_position_behind(modal_pivot)) {
		const float r = (float)dock->prop_radius->get_value() / _world_per_pixel(p_cam, modal_pivot);
		p_overlay->draw_arc(p_cam->unproject_position(modal_pivot), r, 0.0f, (float)Math::PI * 2.0f, 64, Color(1, 1, 1, 0.5f), 1.0f, true);
	}
	if (modal == MODAL_SNAP && snap_avg_valid && !p_cam->is_position_behind(snap_avg)) {
		// the point the picks currently average to
		const Vector2 c = p_cam->unproject_position(snap_avg);
		PackedVector2Array dia;
		dia.push_back(c + Vector2(0, -7));
		dia.push_back(c + Vector2(7, 0));
		dia.push_back(c + Vector2(0, 7));
		dia.push_back(c + Vector2(-7, 0));
		p_overlay->draw_colored_polygon(dia, HANDLE_HOVER);
	}
	if (modal_snapped && !p_cam->is_position_behind(modal_snap_point)) {
		draw_snap_symbol(p_overlay, p_cam->unproject_position(modal_snap_point), modal_snap_kind, SNAP_COLOR, 7.0f);
	}
	if (modal == MODAL_SNAP) {
		for (const SnapPick &p : snap_picks) {
			if (p_cam->is_position_behind(p.p)) {
				continue;
			}
			const Vector2 s = p_cam->unproject_position(p.p);
			p_overlay->draw_circle(s, 5.0f, TOOL_COLOR);
			if (font.is_valid() && p.weight > 1) {
				p_overlay->draw_string(font, s + Vector2(7, -7), vformat("x%d", p.weight), HORIZONTAL_ALIGNMENT_LEFT, -1, 13, TOOL_COLOR);
			}
		}
		if (snap_hover_valid && !p_cam->is_position_behind(snap_hover)) {
			draw_snap_symbol(p_overlay, p_cam->unproject_position(snap_hover), snap_hover_kind, SNAP_COLOR, 7.0f);
		}
		if (snap_phase == 1 && !p_cam->is_position_behind(snap_source)) {
			const Vector2 s = p_cam->unproject_position(snap_source);
			p_overlay->draw_rect(Rect2(s - Vector2(5, 5), Vector2(10, 10)), SNAP_COLOR, false, 2.0f);
			if (snap_hover_valid && !p_cam->is_position_behind(snap_hover)) {
				p_overlay->draw_dashed_line(s, p_cam->unproject_position(snap_hover), SNAP_COLOR, 1.0f, 5.0f);
			}
		}
	}
	if (font.is_valid()) {
		String text = modal_label;
		if (!modal_num.is_empty()) {
			text += "   [" + modal_num + "]";
		}
		draw_header(p_overlay, font, text);
	}
}

// ------------------------------------------------------------------ GG slide and V rip

void HewnEditorPlugin::_start_slide(Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (!n || modal_verts.empty()) {
		return;
	}
	_apply_positions(modal_orig);
	modal_verts.resize(modal_sel_count);
	modal_orig.resize(modal_sel_count);
	modal_weights.assign(modal_sel_count, 1.0f);
	modal = MODAL_SLIDE;
	modal_mouse0 = mouse_pos;
	_reset_precise();
	modal_num = "";
	modal_axis = -1;
	slide_unclamped = false;
	slide_to.assign(modal_verts.size(), -1);
	_update_modal(p_cam, false, false);
}

void HewnEditorPlugin::_start_rip(Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (!n || (mode != MODE_VERTEX && mode != MODE_EDGE)) {
		return;
	}
	hewn::Mesh &m = n->get_data();
	hewn::Selection &s = n->selection;
	const std::vector<int> verts = _selected_verts();
	if (verts.empty()) {
		return;
	}
	const Transform3D g = n->get_global_transform();
	hewn::Topology t;
	t.build(m);
	std::unordered_set<int> vs(verts.begin(), verts.end());
	// The ripped side is the one under the mouse, relative to the selection polyline on screen.
	std::vector<std::pair<Vector2, Vector2>> segs;
	for (const hewn::Edge &e : t.edges) {
		if (vs.count(e.a) && vs.count(e.b)) {
			segs.push_back({ p_cam->unproject_position(g.xform(m.pos[e.a])), p_cam->unproject_position(g.xform(m.pos[e.b])) });
		}
	}
	for (int v : verts) {
		const Vector2 p = p_cam->unproject_position(g.xform(m.pos[v]));
		segs.push_back({ p, p });
	}
	auto nearest = [&](const Vector2 &q) {
		float best = 1e30f;
		Vector2 r = q;
		for (const std::pair<Vector2, Vector2> &sg : segs) {
			float tt;
			const float d = seg_dist(q, sg.first, sg.second, &tt);
			if (d < best) {
				best = d;
				r = sg.first.lerp(sg.second, tt);
			}
		}
		return r;
	};
	const Vector2 side = mouse_pos - nearest(mouse_pos);
	begin_edit("Rip");
	std::unordered_map<int, int> copy;
	for (int v : verts) {
		std::vector<int> near_side, far_side;
		for (int f : t.vert_faces[v]) {
			const Vector2 c = p_cam->unproject_position(g.xform(hewn::face_center(m, m.faces[f])));
			((c - nearest(c)).dot(side) > 0.0f ? near_side : far_side).push_back(f);
		}
		if (near_side.empty() || far_side.empty()) {
			continue;
		}
		const int nv = m.add_vertex(m.pos[v]);
		copy[v] = nv;
		for (int f : near_side) {
			for (int &x : m.faces[f].v) {
				if (x == v) {
					x = nv;
				}
			}
		}
	}
	if (copy.empty()) {
		editing = false;
		return;
	}
	auto mapped = [&](int v) {
		auto it = copy.find(v);
		return it == copy.end() ? v : it->second;
	};
	if (mode == MODE_VERTEX) {
		std::vector<int> nsel;
		for (int v : s.verts) {
			nsel.push_back(mapped(v));
		}
		s.verts = nsel;
	} else {
		std::vector<hewn::Edge> nsel;
		for (const hewn::Edge &e : s.edges) {
			nsel.push_back(hewn::Edge(mapped(e.a), mapped(e.b)));
		}
		s.edges = nsel;
	}
	n->commit(true);
	selection_changed();
	if (!_start_transform(MODAL_MOVE, p_cam)) {
		n->set_state(edit_before);
		editing = false;
	}
	modal_keep_on_cancel = modal == MODAL_MOVE;
	modal_origin = 3;
}

// ------------------------------------------------------------------ Alt+E extrude menu

void HewnEditorPlugin::_show_extrude_menu(Camera3D *p_cam) {
	extrude_cam_id = p_cam ? p_cam->get_instance_id() : 0;
	const bool faces = mode == MODE_FACE;
	for (int i = 0; i < extrude_menu->get_item_count(); i++) {
		extrude_menu->set_item_disabled(i, !faces && extrude_menu->get_item_id(i) != 0);
	}
	Control *surface = _surface_for(p_cam);
	const Vector2 at = surface ? surface->get_screen_position() + mouse_pos : Vector2();
	extrude_menu->popup(Rect2i(Vector2i(at), Vector2i()));
}

void HewnEditorPlugin::_on_extrude_menu(int p_id) {
	Camera3D *cam = extrude_cam_id ? Object::cast_to<Camera3D>(ObjectDB::get_instance(extrude_cam_id)) : nullptr;
	_on_extrude_menu_cam(cam, p_id);
}

void HewnEditorPlugin::_on_extrude_menu_cam(Camera3D *cam, int p_id) {
	if (!cam) {
		return;
	}
	if (p_id == 0 || p_id == 3) {
		set_op_param("extrude_move", "dissolve_orthogonal", p_id == 3);
		_start_extrude(cam);
		return;
	}
	// Along normals / individual faces: the mouse sets the distance (like inset).
	HewnMesh3D *n = get_edited();
	if (!n || n->selection.faces.empty()) {
		return;
	}
	begin_edit(p_id == 1 ? "Extrude Along Normals" : "Extrude Individual");
	modal_backup = n->get_data();
	modal_sel_backup = n->selection;
	extrude_kind = p_id;
	const Transform3D g = n->get_global_transform();
	Vector3 c;
	const std::vector<int> vs = hewn::sel::verts_of_faces(n->get_data(), n->selection.faces);
	for (int v : vs) {
		c += g.xform(n->get_data().pos[v]);
	}
	modal_pivot = c / (float)vs.size();
	modal_mouse0 = mouse_pos;
	_reset_precise();
	modal_scale_k = _world_per_pixel(cam, modal_pivot);
	modal_num = "";
	modal_axis = -1;
	modal_object = false;
	modal = MODAL_EXTRUDE_AMOUNT;
	drag = DRAG_NONE;
	_update_modal(cam, false, false);
}
