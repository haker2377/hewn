#include "hw_cutter.h"

#include "hewn_mesh.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;

std::vector<HewnCutter3D *> HewnCutter3D::all;

HewnCutter3D::HewnCutter3D() {}

HewnCutter3D::~HewnCutter3D() {
	all.erase(std::remove(all.begin(), all.end(), this), all.end());
}

void HewnCutter3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (std::find(all.begin(), all.end(), this) == all.end()) {
				all.push_back(this);
			}
			set_notify_transform(true);
			_update_ghost();
			_changed();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			all.erase(std::remove(all.begin(), all.end(), this), all.end());
			// the meshes it cut get their surface back
			for (const ObjectID &id : reached) {
				if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(id))) {
					m->_cuts_changed();
				}
			}
			reached.clear();
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED:
		case NOTIFICATION_VISIBILITY_CHANGED: {
			_changed();
		} break;
	}
}

void HewnCutter3D::_changed() {
	revision++;
	update_gizmos();
	queue_lining();
	if (!is_inside_tree()) {
		return;
	}
	const AABB b = world_bounds();
	std::vector<ObjectID> now;
	for (HewnMesh3D *m : HewnMesh3D::all_meshes) {
		const bool hit = m->is_inside_tree() && cuts(m);
		bool before = false;
		for (const ObjectID &id : reached) {
			before = before || id == ObjectID(m->get_instance_id());
		}
		if (hit) {
			now.push_back(ObjectID(m->get_instance_id()));
		}
		if (hit || before) {
			m->_cuts_changed();
		}
	}
	reached = now;
	last_bounds = b;
}

void HewnCutter3D::_update_ghost() {
	if (!Engine::get_singleton()->is_editor_hint() || !is_inside_tree()) {
		return;
	}
	if (!ghost) {
		ghost = memnew(MeshInstance3D);
		ghost->set_name("_CutterGhost");
		ghost->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		Ref<StandardMaterial3D> m;
		m.instantiate();
		m->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		m->set_depth_draw_mode(BaseMaterial3D::DEPTH_DRAW_DISABLED);
		m->set_albedo(Color(1.0f, 0.3f, 0.2f, 0.12f));
		ghost->set_material_override(m);
		add_child(ghost, false, INTERNAL_MODE_BACK);
	}
	// the outline's faces
	PackedVector3Array v;
	for (const std::vector<Vector3> &poly : local_outline()) {
		for (size_t i = 1; i + 1 < poly.size(); i++) {
			v.push_back(poly[0]);
			v.push_back(poly[i]);
			v.push_back(poly[i + 1]);
		}
	}
	Ref<ArrayMesh> am;
	am.instantiate();
	if (!v.is_empty()) {
		Array arr;
		arr.resize(Mesh::ARRAY_MAX);
		arr[Mesh::ARRAY_VERTEX] = v;
		am->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr);
	}
	ghost->set_mesh(am);
	const Vector3 sz = shape == SHAPE_PRISM ? Vector3(1, 1, 1) : size;
	ghost->set_scale(Vector3(MAX(0.01f, 1.0f - 0.006f / MAX(sz.x, 0.01f)), MAX(0.01f, 1.0f - 0.006f / MAX(sz.y, 0.01f)), MAX(0.01f, 1.0f - 0.006f / MAX(sz.z, 0.01f))));
	ghost->set_visible(custom.empty()); // (the stairs' headroom is not shown)
}

namespace {

// The outline (local XZ, counter-clockwise seen from +Y) and the height range of the prism.
void outline_of(int shape, const Vector3 &size, int sides, const PackedVector2Array &points, std::vector<Vector2> &r_out, float &r_y0, float &r_y1) {
	r_out.clear();
	const float hx = size.x * 0.5f, hz = size.z * 0.5f;
	r_y0 = -size.y * 0.5f;
	r_y1 = size.y * 0.5f;
	if (shape == HewnCutter3D::SHAPE_CYLINDER) {
		for (int i = 0; i < sides; i++) {
			const float a = 6.28318530718f * i / sides;
			r_out.push_back(Vector2(std::cos(a) * hx, std::sin(a) * hz));
		}
	} else if (shape == HewnCutter3D::SHAPE_PRISM && points.size() >= 3) {
		for (int64_t i = 0; i < points.size(); i++) {
			r_out.push_back(points[i]);
		}
		r_y0 = 0.0f;
		r_y1 = size.y;
	} else {
		r_out = { Vector2(-hx, -hz), Vector2(hx, -hz), Vector2(hx, hz), Vector2(-hx, hz) };
	}
	float area = 0.0f;
	for (size_t i = 0; i < r_out.size(); i++) {
		area += r_out[i].cross(r_out[(i + 1) % r_out.size()]);
	}
	if (area < 0.0f) {
		std::reverse(r_out.begin(), r_out.end());
	}
}

bool convex(const std::vector<Vector2> &p) {
	for (size_t i = 0; i < p.size(); i++) {
		if ((p[(i + 1) % p.size()] - p[i]).cross(p[(i + 2) % p.size()] - p[(i + 1) % p.size()]) < -1e-9f) {
			return false;
		}
	}
	return true;
}

} // namespace

std::vector<std::vector<Vector3>> HewnCutter3D::local_outline() const {
	std::vector<std::vector<Vector3>> out;
	if (!custom.empty()) {
		return out;
	}
	std::vector<Vector2> o;
	float y0, y1;
	outline_of(shape, size, sides, points, o, y0, y1);
	if (o.size() < 3) {
		return out;
	}
	std::vector<Vector3> bot, top;
	for (const Vector2 &q : o) {
		bot.push_back(Vector3(q.x, y0, q.y));
		top.push_back(Vector3(q.x, y1, q.y));
	}
	out.push_back(bot);
	out.push_back(top);
	for (size_t i = 0; i < o.size(); i++) {
		const size_t j = (i + 1) % o.size();
		out.push_back({ bot[i], bot[j], top[j], top[i] });
	}
	return out;
}

std::vector<std::vector<hewn::ops::CutPlane>> HewnCutter3D::world_parts() const {
	std::vector<std::vector<hewn::ops::CutPlane>> local = custom;
	if (local.empty()) {
		std::vector<Vector2> o;
		float y0, y1;
		outline_of(shape, size, sides, points, o, y0, y1);
		if (o.size() < 3) {
			return local;
		}
		// convex pieces: the outline, or its triangles
		std::vector<std::vector<Vector2>> pieces;
		if (convex(o)) {
			pieces.push_back(o);
		} else {
			std::vector<Vector3> p3;
			for (const Vector2 &q : o) {
				p3.push_back(Vector3(q.x, 0.0f, -q.y));
			}
			std::vector<int> tris;
			hewn::triangulate(p3, Vector3(0, 1, 0), tris);
			for (size_t t = 0; t + 2 < tris.size(); t += 3) {
				std::vector<Vector2> tri = { o[tris[t]], o[tris[t + 1]], o[tris[t + 2]] };
				if ((tri[1] - tri[0]).cross(tri[2] - tri[0]) < 0.0f) {
					std::swap(tri[1], tri[2]);
				}
				pieces.push_back(tri);
			}
		}
		const float margin = 1e-4f;
		for (const std::vector<Vector2> &pc : pieces) {
			std::vector<hewn::ops::CutPlane> planes;
			for (size_t i = 0; i < pc.size(); i++) {
				const Vector2 a = pc[i], b = pc[(i + 1) % pc.size()];
				const Vector2 e = b - a;
				if (e.length() < 1e-7f) {
					continue;
				}
				// outward in XZ (counter-clockwise seen from +Y: the outside is on the right of a -> b in (x, z))
				const Vector3 n = Vector3(e.y, 0.0f, -e.x).normalized();
				planes.push_back({ n, n.dot(Vector3(a.x, 0.0f, a.y)), keep_flush });
			}
			const bool end_flush = keep_flush && !cut_ends;
			const float em = cut_ends ? margin : 0.0f;
			planes.push_back({ Vector3(0, 1, 0), y1 + em, end_flush });
			planes.push_back({ Vector3(0, -1, 0), -y0 + em, end_flush });
			local.push_back(planes);
		}
	}
	const Transform3D g = get_global_transform();
	const Basis nb = g.basis.inverse().transposed();
	for (std::vector<hewn::ops::CutPlane> &part : local) {
		for (hewn::ops::CutPlane &p : part) {
			const Vector3 pt = g.xform(p.n * p.d);
			p.n = nb.xform(p.n).normalized();
			p.d = p.n.dot(pt);
		}
	}
	return local;
}

AABB HewnCutter3D::world_bounds() const {
	const Transform3D g = is_inside_tree() ? get_global_transform() : get_transform();
	AABB b;
	bool first = true;
	auto add = [&](const Vector3 &p) {
		if (first) {
			b = AABB(g.xform(p), Vector3());
			first = false;
		} else {
			b.expand_to(g.xform(p));
		}
	};
	if (!custom.empty()) {
		// (the parts' corners are not known: the planes' points, generously)
		for (const std::vector<hewn::ops::CutPlane> &part : custom) {
			for (const hewn::ops::CutPlane &p : part) {
				add(p.n * p.d);
			}
		}
		return b.grow(1.0f);
	}
	for (const std::vector<Vector3> &poly : local_outline()) {
		for (const Vector3 &p : poly) {
			add(p);
		}
	}
	return b;
}

bool HewnCutter3D::cuts(const HewnMesh3D *p_mesh) const {
	if (!active || !is_inside_tree() || !is_visible_in_tree() || !p_mesh || !p_mesh->is_inside_tree() || p_mesh->is_boolean_cutter() || p_mesh->is_brush()) {
		return false;
	}
	// its own lining (a corridor / alcove made with W) is not cut
	if (is_ancestor_of(const_cast<HewnMesh3D *>(p_mesh))) {
		return false;
	}
	// what holds it (the stairs that made it, the scene it is in, or any parent with Skip Parent) is not cut
	Node *holder = get_parent();
	if (holder && (skip_parent || Object::cast_to<HewnMesh3D>(holder) || (!holder->get_scene_file_path().is_empty() && holder->get_owner()))) {
		// (a scene's root holds everything: only itself is left alone, not the whole level)
		const bool scene_root = !holder->get_owner() || holder == holder->get_tree()->get_edited_scene_root() || holder == holder->get_tree()->get_current_scene();
		if (holder == p_mesh || (!scene_root && holder->is_ancestor_of(const_cast<HewnMesh3D *>(p_mesh)))) {
			return false;
		}
	}
	if (p_mesh->get_world_3d() != get_world_3d()) {
		return false; // (another world: an exported copy of a scene, a preview)
	}
	const AABB mb = p_mesh->get_global_transform().xform(p_mesh->get_data_aabb());
	return mb.grow(1e-3f).intersects(world_bounds());
}

TypedArray<Node> HewnCutter3D::apply() {
	TypedArray<Node> out;
	const std::vector<std::vector<hewn::ops::CutPlane>> parts = world_parts();
	for (HewnMesh3D *m : HewnMesh3D::all_meshes) {
		if (!cuts(m)) {
			continue;
		}
		const Transform3D inv = m->get_global_transform().affine_inverse();
		const Basis nb = m->get_global_transform().basis.transposed();
		std::vector<std::vector<hewn::ops::CutPlane>> local = parts;
		for (std::vector<hewn::ops::CutPlane> &part : local) {
			for (hewn::ops::CutPlane &p : part) {
				const Vector3 pt = inv.xform(p.n * p.d);
				p.n = nb.xform(p.n).normalized();
				p.d = p.n.dot(pt);
			}
		}
		hewn::ops::cut_volumes(m->get_data(), {}, local);
		hewn::remove_unused_vertices(m->get_data());
		out.push_back(m);
	}
	// the lining as a mesh of its own next to the cutter (kept when the cutter is deleted)
	if (lining != LINING_NONE && lining_mesh && !lining_mesh->get_data().faces.empty() && get_parent()) {
		HewnMesh3D *keep = memnew(HewnMesh3D);
		keep->set_name(lining == LINING_ALCOVE ? "Alcove" : "Corridor");
		keep->set_materials(lining_mesh->get_materials());
		keep->get_data() = lining_mesh->get_data();
		get_parent()->add_child(keep, true);
		keep->set_owner(get_owner() ? get_owner() : get_parent());
		keep->set_transform(get_transform());
		keep->commit(true);
		out.push_back(keep);
	}
	set_active(false); // (its cut is in the meshes now)
	for (int64_t i = 0; i < out.size(); i++) {
		Object::cast_to<HewnMesh3D>(out[i])->commit();
	}
	return out;
}

void HewnCutter3D::queue_lining() {
	if (lining_queued || (lining == LINING_NONE && !lining_mesh)) {
		return;
	}
	lining_queued = true;
	callable_mp(this, &HewnCutter3D::_update_lining).call_deferred();
}

// The corridor between its holes (or the alcove behind its hole) from the meshes it cuts now, in its own
// space; it does not cut it, it moves with it and follows the walls.
void HewnCutter3D::_update_lining() {
	lining_queued = false;
	if (lining == LINING_NONE || !is_inside_tree() || !active) {
		if (lining_mesh) {
			lining_mesh->get_data() = hewn::Mesh();
			lining_mesh->commit(false);
			lining_mesh->set_visible(false);
		}
		return;
	}
	hewn::Mesh all;
	TypedArray<Material> mats;
	for (HewnMesh3D *n : HewnMesh3D::all_meshes) {
		if (n == lining_mesh || n->has_meta("_hw_lining") || !cuts(n)) {
			continue;
		}
		const int offset = (int)mats.size();
		const TypedArray<Material> nm = n->get_materials();
		for (int64_t i = 0; i < nm.size(); i++) {
			mats.push_back(nm[i]);
		}
		if (nm.is_empty()) {
			mats.push_back(Ref<Material>());
		}
		hewn::ops::append(all, n->get_data(), n->get_global_transform(), offset);
	}
	std::vector<int> made;
	if (!all.faces.empty()) {
		const std::vector<std::vector<hewn::ops::CutPlane>> wp = world_parts();
		const std::vector<std::vector<int>> loops = hewn::ops::cut_volumes(all, {}, wp);
		if (loops.size() == 2 && lining != LINING_ALCOVE) {
			made = hewn::ops::bridge_loops(all, loops[0], loops[1]);
		} else if (!loops.empty() && lining != LINING_CORRIDOR) {
			std::vector<hewn::ops::CutPlane> planes;
			for (const std::vector<hewn::ops::CutPlane> &part : wp) {
				planes.insert(planes.end(), part.begin(), part.end());
			}
			made = hewn::ops::alcove(all, loops[0], lining_depth, &planes); // (along the cutter, to its far side)
		}
	}
	if (!lining_mesh) {
		lining_mesh = memnew(HewnMesh3D);
		lining_mesh->set_name("_Lining");
		lining_mesh->set_meta("_hw_lining", true);
		add_child(lining_mesh, false, INTERNAL_MODE_BACK);
	}
	hewn::Mesh lm = made.empty() ? hewn::Mesh() : hewn::ops::extract_faces(all, made);
	hewn::ops::transform(lm, get_global_transform().affine_inverse());
	lining_mesh->set_materials(mats);
	lining_mesh->get_data() = lm;
	lining_mesh->commit(false);
	lining_mesh->set_visible(!made.empty());
}

void HewnCutter3D::set_lining(int p_lining) {
	lining = CLAMP(p_lining, 0, 3);
	queue_lining();
}

void HewnCutter3D::set_lining_depth(float p_depth) {
	lining_depth = MAX(0.001f, p_depth);
	queue_lining();
}

void HewnCutter3D::set_custom_parts(const std::vector<std::vector<hewn::ops::CutPlane>> &p_parts) {
	custom = p_parts;
	_update_ghost();
	_changed();
}

#define HW_CUTTER_SET(m_type, m_name, m_expr)        \
	void HewnCutter3D::set_##m_name(m_type p_v) { \
		m_name = m_expr;                               \
		_update_ghost();                               \
		_changed();                                    \
	}
HW_CUTTER_SET(int, shape, CLAMP(p_v, 0, 2))
HW_CUTTER_SET(const Vector3 &, size, p_v.max(Vector3(0.001f, 0.001f, 0.001f)))
HW_CUTTER_SET(int, sides, CLAMP(p_v, 3, 128))
HW_CUTTER_SET(const PackedVector2Array &, points, p_v)
HW_CUTTER_SET(bool, keep_flush, p_v)
HW_CUTTER_SET(bool, cut_ends, p_v)
HW_CUTTER_SET(bool, skip_parent, p_v)
HW_CUTTER_SET(bool, active, p_v)

void HewnCutter3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_shape", "shape"), &HewnCutter3D::set_shape);
	ClassDB::bind_method(D_METHOD("get_shape"), &HewnCutter3D::get_shape);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &HewnCutter3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &HewnCutter3D::get_size);
	ClassDB::bind_method(D_METHOD("set_sides", "sides"), &HewnCutter3D::set_sides);
	ClassDB::bind_method(D_METHOD("get_sides"), &HewnCutter3D::get_sides);
	ClassDB::bind_method(D_METHOD("set_points", "points"), &HewnCutter3D::set_points);
	ClassDB::bind_method(D_METHOD("get_points"), &HewnCutter3D::get_points);
	ClassDB::bind_method(D_METHOD("set_keep_flush", "on"), &HewnCutter3D::set_keep_flush);
	ClassDB::bind_method(D_METHOD("get_keep_flush"), &HewnCutter3D::get_keep_flush);
	ClassDB::bind_method(D_METHOD("set_cut_ends", "on"), &HewnCutter3D::set_cut_ends);
	ClassDB::bind_method(D_METHOD("get_cut_ends"), &HewnCutter3D::get_cut_ends);
	ClassDB::bind_method(D_METHOD("set_skip_parent", "on"), &HewnCutter3D::set_skip_parent);
	ClassDB::bind_method(D_METHOD("get_skip_parent"), &HewnCutter3D::get_skip_parent);
	ClassDB::bind_method(D_METHOD("set_active", "on"), &HewnCutter3D::set_active);
	ClassDB::bind_method(D_METHOD("is_active"), &HewnCutter3D::is_active);
	ClassDB::bind_method(D_METHOD("apply"), &HewnCutter3D::apply);
	ClassDB::bind_method(D_METHOD("set_lining", "lining"), &HewnCutter3D::set_lining);
	ClassDB::bind_method(D_METHOD("get_lining"), &HewnCutter3D::get_lining);
	ClassDB::bind_method(D_METHOD("set_lining_depth", "depth"), &HewnCutter3D::set_lining_depth);
	ClassDB::bind_method(D_METHOD("get_lining_depth"), &HewnCutter3D::get_lining_depth);
	ClassDB::bind_method(D_METHOD("get_lining_mesh"), &HewnCutter3D::get_lining_mesh);
	ClassDB::bind_method(D_METHOD("update_lining"), &HewnCutter3D::_update_lining);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shape", PROPERTY_HINT_ENUM, "Box,Cylinder,Prism"), "set_shape", "get_shape");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sides", PROPERTY_HINT_RANGE, "3,128,1"), "set_sides", "get_sides");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR2_ARRAY, "points"), "set_points", "get_points");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "keep_flush"), "set_keep_flush", "get_keep_flush");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cut_ends"), "set_cut_ends", "get_cut_ends");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "skip_parent"), "set_skip_parent", "get_skip_parent");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "active"), "set_active", "is_active");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "lining", PROPERTY_HINT_ENUM, "None,Auto,Corridor,Alcove"), "set_lining", "get_lining");
	// (the alcove goes to the cutter's far side: its size is the depth; kept for old scenes and alcoves without one)
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lining_depth", PROPERTY_HINT_RANGE, "0.001,100,0.001,or_greater,suffix:m", PROPERTY_USAGE_STORAGE), "set_lining_depth", "get_lining_depth");
	BIND_ENUM_CONSTANT(LINING_NONE);
	BIND_ENUM_CONSTANT(LINING_AUTO);
	BIND_ENUM_CONSTANT(LINING_CORRIDOR);
	BIND_ENUM_CONSTANT(LINING_ALCOVE);
	BIND_ENUM_CONSTANT(SHAPE_BOX);
	BIND_ENUM_CONSTANT(SHAPE_CYLINDER);
	BIND_ENUM_CONSTANT(SHAPE_PRISM);
}
