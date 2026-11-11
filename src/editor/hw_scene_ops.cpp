// Operations on the selected objects with a panel of their own (bottom left, like the Last Operation panel; a value
// changed there takes the operation back and does it again): Convert to Collision, Move to Floor / Ceiling.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <unordered_map>
#include <godot_cpp/classes/area3d.hpp>
#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/capsule_shape3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/concave_polygon_shape3d.hpp>
#include <godot_cpp/classes/convex_polygon_shape3d.hpp>
#include <godot_cpp/classes/csg_shape3d.hpp>
#include <godot_cpp/classes/cylinder_shape3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/sphere_shape3d.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/classes/undo_redo.hpp>

#include <algorithm>

using namespace godot;
using namespace hw_editor;

bool HewnEditorPlugin::is_scene_op(const String &p_op) {
	return p_op == "to_collision" || p_op == "to_floor" || p_op == "rope" || p_op == "hc_radius" || p_op == "hc_weight" || p_op == "hc_simplify" || p_op == "hc_draw";
}

// From a menu / button: the selected objects, the operation done once with its panel's values.
void HewnEditorPlugin::start_scene_op(const String &p_op) {
	scene_op_targets.clear();
	if (p_op.begins_with("hc_")) {
		// (the curve being edited)
		if (HewnCurve3D *c = _hc()) {
			scene_op_targets.push_back(ObjectID(c->get_instance_id()));
		}
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	for (int64_t i = 0; i < sel.size() && !p_op.begins_with("hc_"); i++) {
		Node3D *n = Object::cast_to<Node3D>(sel[i]);
		if (n && n != root && n->is_inside_tree()) {
			scene_op_targets.push_back(ObjectID(n->get_instance_id()));
		}
	}
	if (scene_op_targets.empty() && p_op != "rope") {
		return;
	}
	_hide_redo();
	last_op = p_op;
	scene_op.history = -1;
	_run_scene_op(false);
	if (scene_op.history >= 0) {
		_show_redo();
	} else {
		last_op = String();
	}
}

bool HewnEditorPlugin::_scene_op_valid() {
	if (last_op == "hc_draw" && hc_draw && scene_op.history < 0) {
		return true; // (the drawing tool, before its stroke: its panel holds the settings it will use)
	}
	if (scene_op.history < 0) {
		return false;
	}
	UndoRedo *h = get_undo_redo()->get_history_undo_redo((int)scene_op.history);
	return h && (int64_t)h->get_version() == scene_op.version;
}

void HewnEditorPlugin::_run_scene_op(bool p_redo) {
	EditorUndoRedoManager *ur = get_undo_redo();
	if (p_redo && last_op == "hc_draw" && hc_draw && scene_op.history < 0) {
		return; // (no stroke yet: the value is kept for it)
	}
	if (p_redo) {
		if (!_scene_op_valid()) {
			_hide_redo();
			return;
		}
		if (last_op == "rope" && Object::cast_to<HewnCurve3D>(ObjectDB::get_instance(rope_node))) {
			_scene_rope(); // (in place: the step that added it stays the same)
			update_overlays();
			return;
		}
		ur->get_history_undo_redo((int)scene_op.history)->undo();
	}
	scene_op.history = -1;
	bool done = false;
	if (last_op == "to_collision") {
		done = _scene_to_collision();
	} else if (last_op == "to_floor") {
		done = _scene_to_floor();
	} else if (last_op == "rope") {
		rope_node = ObjectID();
		rope_tris_box = AABB();
		done = _scene_rope();
	} else if (last_op.begins_with("hc_")) {
		done = _hc_panel_op(last_op);
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (done && root) {
		scene_op.history = ur->get_object_history_id(root);
		UndoRedo *h = ur->get_history_undo_redo((int)scene_op.history);
		scene_op.version = h ? (int64_t)h->get_version() : -1;
	}
	update_overlays();
}

namespace {

void own_all(EditorUndoRedoManager *p_ur, Node *p_node, Node *p_root) {
	p_ur->add_do_method(p_node, "set_owner", p_root);
	for (int i = 0; i < p_node->get_child_count(); i++) {
		own_all(p_ur, p_node->get_child(i), p_root);
	}
}

// The meshes of a node and the nodes under it (Hewn meshes: their own points), in world space; at most about p_max.
void world_points(Node *p_node, std::vector<Vector3> &r_out, size_t p_max) {
	std::vector<Node *> stack = { p_node };
	std::vector<Vector3> all;
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
		if (HewnMesh3D *hm = Object::cast_to<HewnMesh3D>(x)) {
			if (!hm->is_visible_in_tree()) {
				continue;
			}
			const Transform3D g = hm->get_global_transform();
			const hewn::Mesh &m = hm->get_data();
			std::vector<char> used = visible_verts(m);
			for (size_t v = 0; v < m.pos.size(); v++) {
				if (used[v]) {
					all.push_back(g.xform(m.pos[v]));
				}
			}
			continue;
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(x);
		if (mi && mi->is_visible_in_tree() && mi->get_mesh().is_valid()) {
			const Transform3D g = mi->get_global_transform();
			const PackedVector3Array f = mi->get_mesh()->get_faces();
			for (int64_t i = 0; i < f.size(); i++) {
				all.push_back(g.xform(f[i]));
			}
		}
	}
	if (all.size() <= p_max) {
		r_out.insert(r_out.end(), all.begin(), all.end());
		return;
	}
	// (many: the lowest / highest ones matter, every n-th of the rest)
	const size_t step = all.size() / p_max + 1;
	for (size_t i = 0; i < all.size(); i += step) {
		r_out.push_back(all[i]);
	}
	auto lo = std::min_element(all.begin(), all.end(), [](const Vector3 &a, const Vector3 &b) { return a.y < b.y; });
	auto hi = std::max_element(all.begin(), all.end(), [](const Vector3 &a, const Vector3 &b) { return a.y < b.y; });
	r_out.push_back(*lo);
	r_out.push_back(*hi);
}

bool under_any(Node *p_node, const std::vector<Node *> &p_roots) {
	for (Node *r : p_roots) {
		if (r == p_node || r->is_ancestor_of(p_node)) {
			return true;
		}
	}
	return false;
}

} // namespace

// The nearest surface of the scene along a ray, but the nodes p_except (and what is under them): Hewn meshes as
// shown, other meshes, CSG shapes; the surface's normal (facing the ray).
bool HewnEditorPlugin::_ray_scene_except(const Vector3 &p_from, const Vector3 &p_dir, const std::vector<Node *> &p_except, float &r_t, Vector3 &r_n) const {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return false;
	}
	float best = 1e30f;
	Vector3 bn;
	std::vector<Node *> stack = { root };
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		if (under_any(x, p_except)) {
			continue;
		}
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
		if (HewnMesh3D *hm = Object::cast_to<HewnMesh3D>(x)) {
			if (!hm->is_visible_in_tree() || hm->is_brush() || hm->is_boolean_cutter()) {
				continue;
			}
			float t;
			const int f = raycast_node(hm, p_from, p_dir, t, nullptr, false, true);
			if (f >= 0 && t < best) {
				best = t;
				const hewn::Mesh &sm = hm->get_shown_data();
				bn = f < (int)sm.faces.size() ? hm->get_global_transform().basis.inverse().transposed().xform(hewn::face_normal(sm, sm.faces[f])).normalized() : -p_dir;
			}
			continue;
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(x);
		CSGShape3D *csg = Object::cast_to<CSGShape3D>(x);
		Ref<Mesh> mesh;
		Transform3D g;
		if (mi && mi->is_visible_in_tree() && mi->get_mesh().is_valid()) {
			mesh = mi->get_mesh();
			g = mi->get_global_transform();
		} else if (csg && csg->is_root_shape() && csg->is_visible_in_tree()) {
			const Array am = csg->get_meshes();
			if (am.size() >= 2 && Object::cast_to<Mesh>(am[1])) {
				mesh = Ref<Mesh>(am[1]);
				g = csg->get_global_transform() * Transform3D(am[0]);
			}
		}
		if (mesh.is_null() || !g.xform(mesh->get_aabb()).grow(1e-3f).intersects_ray(p_from, p_dir)) {
			continue;
		}
		const PackedVector3Array tris = mesh->get_faces();
		for (int64_t i = 0; i + 2 < tris.size(); i += 3) {
			const Vector3 a = g.xform(tris[i]), b = g.xform(tris[i + 1]), c = g.xform(tris[i + 2]);
			float t;
			if (ray_tri(p_from, p_dir, a, b, c, t) && t < best) {
				best = t;
				bn = (b - a).cross(c - a).normalized();
			}
		}
	}
	if (best >= 1e29f) {
		return false;
	}
	r_t = best;
	r_n = bn.dot(p_dir) > 0.0f ? -bn : bn;
	return true;
}

// A rope hung between two points: a chain of points kept apart (its length the distance times the slack), pulled down
// step by step until it settles, against the scene's surfaces if asked; then a curve through them (round, as thick
// as set).
bool HewnEditorPlugin::_scene_rope() {
	const float slack = MAX(1.0f, (float)op_param("rope", "slack"));
	const int segs = CLAMP((int)op_param("rope", "segments"), 2, 256);
	const float thick = MAX(0.0f, (float)op_param("rope", "thickness"));
	const bool collide = (bool)op_param("rope", "collide");
	const Vector3 a = rope_a, b = rope_b;
	const float L = a.distance_to(b) * slack;
	const float seg = L / (float)segs;
	std::vector<Vector3> p(segs + 1), prev;
	for (int i = 0; i <= segs; i++) {
		p[i] = a.lerp(b, (float)i / (float)segs);
	}
	prev = p;
	// (the surfaces near it, as triangles, once)
	std::vector<Vector3> tris;
	AABB box(a, Vector3());
	box.expand_to(b);
	box = box.grow(L * 0.5f + 0.1f);
	box.position.y -= L;
	box.size.y += L;
	const bool rerun = Object::cast_to<HewnCurve3D>(ObjectDB::get_instance(rope_node)) != nullptr;
	if (collide && rerun && rope_tris_box.encloses(box)) {
		tris = rope_tris; // (its panel changed: the same surfaces as a moment ago)
	} else if (collide) {
		box = box.grow(box.get_longest_axis_size() * 0.5f); // (a bit more: a longer rope next time needs no new look)
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
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
			if (!mi || !mi->is_visible_in_tree() || mi->get_mesh().is_null() || mi->has_meta("_hw_curve_mesh")) {
				continue; // (a curve's own geometry - the rope's too - is not what it hangs on)
			}
			const Transform3D g = mi->get_global_transform();
			if (!g.xform(mi->get_mesh()->get_aabb()).intersects(box)) {
				continue;
			}
			const PackedVector3Array f = mi->get_mesh()->get_faces();
			for (int64_t i = 0; i + 2 < f.size(); i += 3) {
				const Vector3 t0 = g.xform(f[i]), t1 = g.xform(f[i + 1]), t2 = g.xform(f[i + 2]);
				AABB tb(t0, Vector3());
				tb.expand_to(t1);
				tb.expand_to(t2);
				if (!tb.intersects(box)) {
					continue; // (only the triangles near it)
				}
				tris.push_back(t0);
				tris.push_back(t1);
				tris.push_back(t2);
			}
		}
		rope_tris = tris;
		rope_tris_box = box;
	}
	const Vector3 grav(0, -9.8f, 0);
	const float dt = 1.0f / 60.0f;
	const float r = MAX(0.005f, thick);
	// (the triangles sorted into cells: each point looks only at the ones near it, not at all of them each step)
	const float cell = MAX(0.05f, MAX(seg * 2.0f, r * 4.0f));
	std::unordered_map<uint64_t, std::vector<int>> grid;
	auto ckey = [&](int x, int y, int z) { return ((uint64_t)(uint32_t)(x + 1048576) << 42) ^ ((uint64_t)(uint32_t)(y + 1048576) << 21) ^ (uint64_t)(uint32_t)(z + 1048576); };
	auto cof = [&](float v) { return (int)std::floor(v / cell); };
	for (size_t t = 0; t + 2 < tris.size(); t += 3) {
		AABB tb(tris[t], Vector3());
		tb.expand_to(tris[t + 1]);
		tb.expand_to(tris[t + 2]);
		tb = tb.grow(r);
		const int x0 = cof(tb.position.x), x1 = cof(tb.get_end().x), y0 = cof(tb.position.y), y1 = cof(tb.get_end().y), z0 = cof(tb.position.z), z1 = cof(tb.get_end().z);
		if ((int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 4096) {
			// (a huge triangle: in the cells along the rope's box only)
			continue;
		}
		for (int x = x0; x <= x1; x++) {
			for (int y = y0; y <= y1; y++) {
				for (int z = z0; z <= z1; z++) {
					grid[ckey(x, y, z)].push_back((int)t);
				}
			}
		}
	}
	std::vector<int> big; // (the huge ones: always looked at)
	for (size_t t = 0; t + 2 < tris.size(); t += 3) {
		AABB tb(tris[t], Vector3());
		tb.expand_to(tris[t + 1]);
		tb.expand_to(tris[t + 2]);
		tb = tb.grow(r);
		if ((int64_t)(cof(tb.get_end().x) - cof(tb.position.x) + 1) * (cof(tb.get_end().y) - cof(tb.position.y) + 1) * (cof(tb.get_end().z) - cof(tb.position.z) + 1) > 4096) {
			big.push_back((int)t);
		}
	}
	std::vector<int> near;
	std::vector<uint32_t> stamp(tris.size() / 3 + 1, 0);
	uint32_t stamp_now = 0;
	for (int step = 0; step < 480; step++) {
		for (int i = 1; i < segs; i++) {
			const Vector3 v = (p[i] - prev[i]) * 0.98f;
			prev[i] = p[i];
			const Vector3 to = p[i] + v + grav * dt * dt;
			Vector3 np = to;
			if (!tris.empty()) {
				// (through a surface: stopped on it)
				const Vector3 d = to - p[i];
				const float len = d.length();
				if (len > 1e-7f) {
					const Vector3 dir = d / len;
					float best = len + r;
					Vector3 bn;
					near.clear();
					stamp_now++;
					AABB qb(p[i], Vector3());
					qb.expand_to(to);
					qb = qb.grow(r);
					for (int x = cof(qb.position.x); x <= cof(qb.get_end().x); x++) {
						for (int y = cof(qb.position.y); y <= cof(qb.get_end().y); y++) {
							for (int z = cof(qb.position.z); z <= cof(qb.get_end().z); z++) {
								auto it = grid.find(ckey(x, y, z));
								if (it == grid.end()) {
									continue;
								}
								for (int t : it->second) {
									if (stamp[t / 3] != stamp_now) {
										stamp[t / 3] = stamp_now;
										near.push_back(t);
									}
								}
							}
						}
					}
					near.insert(near.end(), big.begin(), big.end());
					for (int tt : near) {
						const size_t t = (size_t)tt;
						float th;
						if (ray_tri(p[i], dir, tris[t], tris[t + 1], tris[t + 2], th) && th < best) {
							best = th;
							bn = (tris[t + 1] - tris[t]).cross(tris[t + 2] - tris[t]).normalized();
						}
					}
					if (best < len + r) {
						if (bn.dot(dir) > 0.0f) {
							bn = -bn;
						}
						np = p[i] + dir * MAX(0.0f, best - r);
					}
				}
			}
			p[i] = np;
		}
		for (int it = 0; it < 24; it++) {
			for (int i = 0; i < segs; i++) {
				Vector3 d = p[i + 1] - p[i];
				const float len = d.length();
				if (len < 1e-9f) {
					continue;
				}
				const Vector3 corr = d * ((len - seg) / len * 0.5f);
				if (i > 0) {
					p[i] += corr;
				}
				if (i + 1 < segs) {
					p[i + 1] -= corr;
				}
				if (i == 0) {
					p[i + 1] -= corr; // (the pinned end: all of it on the other)
				}
				if (i + 1 == segs) {
					p[i] += corr;
				}
			}
		}
	}
	// (its panel changed: the same rope again, in place - the step that added it takes it away as it is)
	HewnCurve3D *c = Object::cast_to<HewnCurve3D>(ObjectDB::get_instance(rope_node));
	const bool again = c && c->is_inside_tree();
	if (!again) {
		c = memnew(HewnCurve3D);
		c->set_name("Rope");
	}
	hewn::curves::Spline s;
	s.type = hewn::curves::BEZIER;
	for (const Vector3 &q : p) {
		hewn::curves::CPoint cp;
		cp.pos = q - a;
		s.pts.push_back(cp);
	}
	hewn::curves::recalc_handles(s, true);
	Array data;
	data.push_back(hewn::curves::spline_to_dict(s));
	c->set_splines_data(data);
	c->set_depth(thick);
	c->set_fill_caps((bool)op_param("rope", "caps"));
	c->set_bevel_resolution(1);
	if (again) {
		c->set_global_position(a);
		return true;
	}
	c->set_transform(Transform3D(Basis(), a));
	_add_node_with_undo(c, "Add Rope");
	rope_node = ObjectID(c->get_instance_id());
	return true;
}

// Move to Floor / Ceiling: each object straight down (up) until it touches what is below (above): its mesh, or only its
// origin; turned first to stand on the surface under it (Align to Surface).
bool HewnEditorPlugin::_scene_to_floor() {
	const bool ceiling = (int)op_param("to_floor", "direction") == 1;
	const bool align = (bool)op_param("to_floor", "align");
	const bool by_origin = (int)op_param("to_floor", "touch") == 1;
	const Vector3 dir = ceiling ? Vector3(0, 1, 0) : Vector3(0, -1, 0);
	std::vector<Node3D *> nodes;
	std::vector<Node *> except;
	for (const ObjectID &id : scene_op_targets) {
		if (Node3D *n = Object::cast_to<Node3D>(ObjectDB::get_instance(id))) {
			nodes.push_back(n);
			except.push_back(n);
		}
	}
	std::vector<Transform3D> before, after;
	std::vector<Node3D *> moved;
	for (Node3D *n : nodes) {
		Transform3D xf = n->get_global_transform();
		std::vector<Vector3> pts;
		if (!by_origin) {
			world_points(n, pts, 4000);
		}
		const bool origin_only = pts.empty();
		if (align) {
			// the surface right under (over) the origin
			float t;
			Vector3 nrm;
			if (_ray_scene_except(xf.origin, dir, except, t, nrm)) {
				const Vector3 want = ceiling ? -nrm : nrm; // (on a ceiling: hanging, its up into it)
				const Vector3 up = xf.basis.get_column(1).normalized();
				const Vector3 ax = up.cross(want);
				const float ang = std::atan2(ax.length(), up.dot(want));
				if (ax.length() > 1e-6f && std::fabs(ang) > 1e-5f) {
					const Basis rot(ax.normalized(), ang);
					for (Vector3 &p : pts) {
						p = xf.origin + rot.xform(p - xf.origin);
					}
					xf.basis = rot * xf.basis;
				}
			}
		}
		if (origin_only) {
			pts.push_back(xf.origin);
		}
		float best = 1e30f;
		for (const Vector3 &p : pts) {
			float t;
			Vector3 nrm;
			// (from a hair behind the point: a mesh standing on the floor already stays)
			if (_ray_scene_except(p - dir * 1e-4f, dir, except, t, nrm)) {
				best = MIN(best, t - 1e-4f);
			}
		}
		if (best >= 1e29f && !align) {
			continue; // (nothing below / above it)
		}
		if (best < 1e29f) {
			xf.origin += dir * best;
		}
		moved.push_back(n);
		before.push_back(n->get_global_transform());
		after.push_back(xf);
	}
	if (moved.empty()) {
		return false;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(ceiling ? "Hewn: Move to Ceiling" : "Hewn: Move to Floor", UndoRedo::MERGE_DISABLE, EditorInterface::get_singleton()->get_edited_scene_root());
	for (size_t i = 0; i < moved.size(); i++) {
		ur->add_do_method(moved[i], "set_global_transform", after[i]);
		ur->add_undo_method(moved[i], "set_global_transform", before[i]);
	}
	ur->commit_action();
	return true;
}

// Convert to Collision: each mesh as a collision shape of its form (convex, triangles, or a simple shape around it),
// in a static body / an area / alone; the mesh kept (the body under it) or replaced.
bool HewnEditorPlugin::_scene_to_collision() {
	const int kind = (int)op_param("to_collision", "shape");
	const int body_kind = (int)op_param("to_collision", "body");
	const bool keep = (bool)op_param("to_collision", "keep_mesh");
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	EditorUndoRedoManager *ur = get_undo_redo();
	bool any = false;
	for (const ObjectID &id : scene_op_targets) {
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(id));
		if (!mi || !mi->get_parent() || mi->get_mesh().is_null() || mi == root) {
			continue;
		}
		const Ref<Mesh> mesh = mi->get_mesh();
		const AABB box = mesh->get_aabb();
		Ref<Shape3D> shape;
		Transform3D local;
		int axis = 1; // (the longest: a capsule / cylinder along it)
		if (box.size.x >= box.size.y && box.size.x >= box.size.z) {
			axis = 0;
		} else if (box.size.z >= box.size.y) {
			axis = 2;
		}
		const float len = box.size[axis];
		const float across = MAX(box.size[(axis + 1) % 3], box.size[(axis + 2) % 3]);
		const Basis along = axis == 0 ? Basis(Vector3(0, 0, 1), -(float)Math::PI * 0.5f) : axis == 2 ? Basis(Vector3(1, 0, 0), (float)Math::PI * 0.5f) : Basis();
		switch (kind) {
			case 1:
				shape = mesh->create_trimesh_shape();
				break;
			case 2: {
				Ref<BoxShape3D> b;
				b.instantiate();
				b->set_size(box.size.max(Vector3(0.001f, 0.001f, 0.001f)));
				shape = b;
				local.origin = box.get_center();
			} break;
			case 3: {
				Ref<SphereShape3D> s;
				s.instantiate();
				s->set_radius(MAX(0.001f, MAX(box.size.x, MAX(box.size.y, box.size.z)) * 0.5f));
				shape = s;
				local.origin = box.get_center();
			} break;
			case 4: {
				Ref<CapsuleShape3D> c;
				c.instantiate();
				c->set_radius(MAX(0.001f, across * 0.5f));
				c->set_height(MAX(len, across + 0.002f));
				shape = c;
				local = Transform3D(along, box.get_center());
			} break;
			case 5: {
				Ref<CylinderShape3D> c;
				c.instantiate();
				c->set_radius(MAX(0.001f, across * 0.5f));
				c->set_height(MAX(0.001f, len));
				shape = c;
				local = Transform3D(along, box.get_center());
			} break;
			default:
				shape = mesh->create_convex_shape(true, false);
				break;
		}
		if (shape.is_null()) {
			continue;
		}
		Node *parent = mi->get_parent();
		CollisionShape3D *cs = memnew(CollisionShape3D);
		cs->set_name("CollisionShape3D");
		cs->set_shape(shape);
		Node3D *made = cs;
		if (body_kind == 0 || body_kind == 1) {
			CollisionObject3D *body = body_kind == 0 ? (CollisionObject3D *)memnew(StaticBody3D) : (CollisionObject3D *)memnew(Area3D);
			body->set_name(keep ? String(body_kind == 0 ? "StaticBody3D" : "Area3D") : String(mi->get_name()));
			body->add_child(cs);
			cs->set_transform(local);
			made = body;
		} else {
			cs->set_transform(local);
			if (!keep) {
				cs->set_name(mi->get_name());
			}
		}
		if (!any) {
			ur->create_action("Hewn: Convert to Collision", UndoRedo::MERGE_DISABLE, root);
			any = true;
		}
		if (keep) {
			// (under the mesh: it moves with it, as Godot's own Create Collision does)
			made->set_transform(made == cs ? local : Transform3D());
			ur->add_do_method(mi, "add_child", made, true);
			own_all(ur, made, root);
			ur->add_do_reference(made);
			ur->add_undo_method(mi, "remove_child", made);
		} else {
			made->set_transform(made == cs ? mi->get_transform() * local : mi->get_transform());
			// (the mesh out first: its name free for what takes its place)
			ur->add_do_method(parent, "remove_child", mi);
			ur->add_do_method(parent, "add_child", made, true);
			own_all(ur, made, root);
			ur->add_do_method(parent, "move_child", made, mi->get_index());
			ur->add_do_reference(made);
			ur->add_undo_method(parent, "remove_child", made);
			ur->add_undo_method(parent, "add_child", mi, true);
			ur->add_undo_method(parent, "move_child", mi, mi->get_index());
			ur->add_undo_method(mi, "set_owner", mi->get_owner());
			ur->add_undo_reference(mi);
		}
	}
	if (any) {
		ur->commit_action();
	}
	return any;
}

bool HewnEditorPlugin::_scene_panel_up() const {
	return phys_on || (redo_panel && redo_panel->is_visible() && is_scene_op(last_op));
}

// ------------------------------------------------------------------ circle select (object mode)

// The circle tool in object mode: held down, the objects under the circle get selected (Ctrl: unselected; without
// Shift / Ctrl the selection starts empty); Ctrl+wheel: its size; Esc: back to the plain select.
int32_t HewnEditorPlugin::_obj_circle_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	Ref<InputEventKey> k = p_event;
	if (k.is_valid() && k->is_pressed() && key_code(k) == KEY_ESCAPE) {
		select_kind = 0;
		obj_circle_drag = false;
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mm.is_valid()) {
		mouse_pos = mm->get_position();
		if (obj_circle_drag) {
			_obj_circle_apply();
		}
		update_overlays();
		return obj_circle_drag ? AFTER_GUI_INPUT_STOP : -1;
	}
	if (mb.is_null()) {
		return -1;
	}
	if (mb->is_pressed() && (mb->is_ctrl_pressed() || mb->is_meta_pressed()) && (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP || mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN)) {
		circle_radius = CLAMP(circle_radius * (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? 1.1f : 1.0f / 1.1f), 3.0f, 500.0f);
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb->get_button_index() != MOUSE_BUTTON_LEFT || mb->is_alt_pressed()) {
		return -1;
	}
	if (!mb->is_pressed()) {
		if (!obj_circle_drag) {
			return -1;
		}
		obj_circle_drag = false;
		obj_circle_items.clear();
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	mouse_pos = mb->get_position();
	obj_circle_drag = true;
	obj_circle_deselect = mb->is_ctrl_pressed() || mb->is_meta_pressed();
	EditorSelection *es = EditorInterface::get_singleton()->get_selection();
	if (!mb->is_shift_pressed() && !obj_circle_deselect) {
		es->clear();
	}
	// what can be picked, as a click picks it (a scene placed in this one: its root), and its points on the screen
	obj_circle_items.clear();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (root) {
		std::unordered_map<uint64_t, size_t> at;
		std::vector<Node *> stack = { root };
		while (!stack.empty()) {
			Node *x = stack.back();
			stack.pop_back();
			for (int i = 0; i < x->get_child_count(); i++) {
				stack.push_back(x->get_child(i));
			}
			Node3D *x3 = Object::cast_to<Node3D>(x);
			if (!x3 || x == root || !x3->is_visible_in_tree()) {
				continue;
			}
			Node *s = x;
			while (s && s != root && s->get_owner() && s->get_owner() != root && !root->is_editable_instance(s->get_owner())) {
				s = s->get_owner();
			}
			if (!s || s == root || !Object::cast_to<Node3D>(s)) {
				continue;
			}
			std::vector<Vector3> pts;
			if (Object::cast_to<MeshInstance3D>(x)) {
				world_points(x, pts, 600);
				// (only its own mesh, not the nodes under it: they come by themselves)
			}
			pts.push_back(x3->get_global_transform().origin);
			auto it = at.find(s->get_instance_id());
			if (it == at.end()) {
				at[s->get_instance_id()] = obj_circle_items.size();
				obj_circle_items.push_back({ ObjectID(s->get_instance_id()), {} });
				it = at.find(s->get_instance_id());
			}
			std::vector<Vector2> &sp = obj_circle_items[it->second].second;
			for (const Vector3 &p : pts) {
				if (!p_cam->is_position_behind(p)) {
					sp.push_back(p_cam->unproject_position(p));
				}
			}
		}
	}
	_obj_circle_apply();
	update_overlays();
	return AFTER_GUI_INPUT_STOP;
}

void HewnEditorPlugin::_obj_circle_apply() {
	EditorSelection *es = EditorInterface::get_singleton()->get_selection();
	const float r2 = circle_radius * circle_radius;
	for (const auto &it : obj_circle_items) {
		Node *n = Object::cast_to<Node>(ObjectDB::get_instance(it.first));
		if (!n) {
			continue;
		}
		bool hit = false;
		for (const Vector2 &p : it.second) {
			if (p.distance_squared_to(mouse_pos) <= r2) {
				hit = true;
				break;
			}
		}
		if (!hit) {
			continue;
		}
		const bool selected = es->get_selected_nodes().has(n);
		if (obj_circle_deselect && selected) {
			es->remove_node(n);
		} else if (!obj_circle_deselect && !selected) {
			es->add_node(n);
		}
	}
}
