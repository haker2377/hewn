#include "hw_modifiers.h"

#include "hewn_mesh.h"

#include <godot_cpp/classes/curve3d.hpp>
#include <godot_cpp/classes/path3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>

#include <cmath>
#include <random>

using namespace godot;

// ------------------------------------------------------------------ base

void HewnModifier::set_enabled(bool p_on) {
	enabled = p_on;
	emit_changed();
}

bool HewnModifier::is_enabled() const {
	return enabled;
}

void HewnModifier::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "on"), &HewnModifier::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &HewnModifier::is_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
}

// ------------------------------------------------------------------ mirror

void HewnMirrorModifier::set_axis(int p_axis, bool p_on) {
	if (p_axis >= 0 && p_axis < 3) {
		axes[p_axis] = p_on;
		emit_changed();
	}
}

bool HewnMirrorModifier::get_axis(int p_axis) const {
	return p_axis >= 0 && p_axis < 3 && axes[p_axis];
}

void HewnMirrorModifier::set_merge(bool p_on) {
	merge = p_on;
	emit_changed();
}

void HewnMirrorModifier::set_merge_distance(float p_dist) {
	merge_distance = MAX(0.0f, p_dist);
	emit_changed();
}

float HewnMirrorModifier::get_merge_distance() const {
	return merge_distance;
}

void HewnMirrorModifier::set_clipping(bool p_on) {
	clipping = p_on;
	emit_changed();
}

bool HewnMirrorModifier::get_clipping() const {
	return clipping;
}

void HewnMirrorModifier::set_mirror_object(const NodePath &p_path) {
	mirror_object = p_path;
	emit_changed();
}

NodePath HewnMirrorModifier::get_mirror_object() const {
	return mirror_object;
}

void HewnMirrorModifier::apply(hewn::Mesh &r_mesh, const HewnMesh3D *p_owner) const {
	// a Mirror Object: the mesh in its space (its origin and axes are the mirror's), mirrored, back
	Node3D *o = mirror_object.is_empty() || !p_owner ? nullptr : Object::cast_to<Node3D>(p_owner->resolve_path(mirror_object));
	Transform3D to;
	const bool other = o && o != (const Node3D *)p_owner && o->is_inside_tree() && p_owner->is_inside_tree();
	if (other) {
		to = o->get_global_transform().affine_inverse() * p_owner->get_global_transform();
		for (Vector3 &p : r_mesh.pos) {
			p = to.xform(p);
		}
	}
	for (int a = 0; a < 3; a++) {
		if (axes[a]) {
			hewn::ops::mirror(r_mesh, a, merge ? merge_distance : -1.0f); // (-1: the halves stay apart)
		}
	}
	if (other) {
		const Transform3D back = to.affine_inverse();
		for (Vector3 &p : r_mesh.pos) {
			p = back.xform(p); // (both halves go back alike: their faces stay turned the same way)
		}
	}
}

void HewnMirrorModifier::get_dependencies(const HewnMesh3D *p_owner, std::vector<Node *> &r_nodes) const {
	if (!mirror_object.is_empty() && p_owner) {
		if (Node *o = p_owner->resolve_path(mirror_object)) {
			r_nodes.push_back(o);
		}
	}
}

void HewnMirrorModifier::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_x", "on"), &HewnMirrorModifier::set_x);
	ClassDB::bind_method(D_METHOD("get_x"), &HewnMirrorModifier::get_x);
	ClassDB::bind_method(D_METHOD("set_y", "on"), &HewnMirrorModifier::set_y);
	ClassDB::bind_method(D_METHOD("get_y"), &HewnMirrorModifier::get_y);
	ClassDB::bind_method(D_METHOD("set_z", "on"), &HewnMirrorModifier::set_z);
	ClassDB::bind_method(D_METHOD("get_z"), &HewnMirrorModifier::get_z);
	ClassDB::bind_method(D_METHOD("set_merge", "on"), &HewnMirrorModifier::set_merge);
	ClassDB::bind_method(D_METHOD("get_merge"), &HewnMirrorModifier::get_merge);
	ClassDB::bind_method(D_METHOD("set_merge_distance", "distance"), &HewnMirrorModifier::set_merge_distance);
	ClassDB::bind_method(D_METHOD("get_merge_distance"), &HewnMirrorModifier::get_merge_distance);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "x"), "set_x", "get_x");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "y"), "set_y", "get_y");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "z"), "set_z", "get_z");
	ClassDB::bind_method(D_METHOD("set_clipping", "on"), &HewnMirrorModifier::set_clipping);
	ClassDB::bind_method(D_METHOD("get_clipping"), &HewnMirrorModifier::get_clipping);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "merge"), "set_merge", "get_merge");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "merge_distance", PROPERTY_HINT_RANGE, "0,1,0.0001,or_greater"), "set_merge_distance", "get_merge_distance");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "clipping"), "set_clipping", "get_clipping");
	ClassDB::bind_method(D_METHOD("set_mirror_object", "path"), &HewnMirrorModifier::set_mirror_object);
	ClassDB::bind_method(D_METHOD("get_mirror_object"), &HewnMirrorModifier::get_mirror_object);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "mirror_object", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_mirror_object", "get_mirror_object");
}

// ------------------------------------------------------------------ solidify

void HewnSolidifyModifier::set_thickness(float p_value) {
	thickness = p_value;
	emit_changed();
}

float HewnSolidifyModifier::get_thickness() const {
	return thickness;
}

void HewnSolidifyModifier::set_offset(float p_value) {
	offset = CLAMP(p_value, -1.0f, 1.0f);
	emit_changed();
}

float HewnSolidifyModifier::get_offset() const {
	return offset;
}

void HewnSolidifyModifier::apply(hewn::Mesh &r_mesh, const HewnMesh3D *p_owner) const {
	if (std::fabs(thickness) > 1e-9f) {
		hewn::ops::solidify(r_mesh, thickness, offset);
	}
}

void HewnSolidifyModifier::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_thickness", "thickness"), &HewnSolidifyModifier::set_thickness);
	ClassDB::bind_method(D_METHOD("get_thickness"), &HewnSolidifyModifier::get_thickness);
	ClassDB::bind_method(D_METHOD("set_offset", "offset"), &HewnSolidifyModifier::set_offset);
	ClassDB::bind_method(D_METHOD("get_offset"), &HewnSolidifyModifier::get_offset);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "thickness", PROPERTY_HINT_RANGE, "-10,10,0.001,or_greater,or_less"), "set_thickness", "get_thickness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "offset", PROPERTY_HINT_RANGE, "-1,1,0.01"), "set_offset", "get_offset");
}

// ------------------------------------------------------------------ subdivision surface

void HewnSubdivisionModifier::set_kind(int p_value) {
	kind = CLAMP(p_value, 0, 1);
	emit_changed();
}

void HewnSubdivisionModifier::set_levels(int p_value) {
	levels = CLAMP(p_value, 0, 6);
	emit_changed();
}

void HewnSubdivisionModifier::set_use_creases(bool p_value) {
	use_creases = p_value;
	emit_changed();
}

void HewnSubdivisionModifier::set_boundary_smooth(int p_value) {
	boundary_smooth = CLAMP(p_value, 0, 1);
	emit_changed();
}

void HewnSubdivisionModifier::set_uv_smooth(int p_value) {
	uv_smooth = CLAMP(p_value, 0, 1);
	emit_changed();
}

void HewnSubdivisionModifier::apply(hewn::Mesh &r_mesh, const HewnMesh3D *p_owner) const {
	hewn::ops::SubdivOptions o;
	o.levels = levels;
	o.simple = kind == SIMPLE;
	o.creases = use_creases;
	o.keep_corners = boundary_smooth == BOUNDARY_KEEP_CORNERS;
	o.uv_smooth = uv_smooth;
	hewn::ops::subdivide_smooth(r_mesh, o);
}

void HewnSubdivisionModifier::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_kind", "kind"), &HewnSubdivisionModifier::set_kind);
	ClassDB::bind_method(D_METHOD("get_kind"), &HewnSubdivisionModifier::get_kind);
	ClassDB::bind_method(D_METHOD("set_levels", "levels"), &HewnSubdivisionModifier::set_levels);
	ClassDB::bind_method(D_METHOD("get_levels"), &HewnSubdivisionModifier::get_levels);
	ClassDB::bind_method(D_METHOD("set_use_creases", "on"), &HewnSubdivisionModifier::set_use_creases);
	ClassDB::bind_method(D_METHOD("get_use_creases"), &HewnSubdivisionModifier::get_use_creases);
	ClassDB::bind_method(D_METHOD("set_boundary_smooth", "mode"), &HewnSubdivisionModifier::set_boundary_smooth);
	ClassDB::bind_method(D_METHOD("get_boundary_smooth"), &HewnSubdivisionModifier::get_boundary_smooth);
	ClassDB::bind_method(D_METHOD("set_uv_smooth", "mode"), &HewnSubdivisionModifier::set_uv_smooth);
	ClassDB::bind_method(D_METHOD("get_uv_smooth"), &HewnSubdivisionModifier::get_uv_smooth);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "kind", PROPERTY_HINT_ENUM, "Catmull-Clark,Simple"), "set_kind", "get_kind");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "levels", PROPERTY_HINT_RANGE, "0,6,1"), "set_levels", "get_levels");
	ADD_GROUP("Advanced", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_creases"), "set_use_creases", "get_use_creases");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "boundary_smooth", PROPERTY_HINT_ENUM, "All,Keep Corners"), "set_boundary_smooth", "get_boundary_smooth");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "uv_smooth", PROPERTY_HINT_ENUM, "None,Keep Boundaries"), "set_uv_smooth", "get_uv_smooth");
}

// ------------------------------------------------------------------ simplify

void HewnSimplifyModifier::set_mode(int p_value) {
	mode = CLAMP(p_value, 0, 1);
	notify_property_list_changed(); // (each mode its own field)
	emit_changed();
}

void HewnSimplifyModifier::set_ratio(float p_value) {
	ratio = CLAMP(p_value, 0.0f, 1.0f);
	emit_changed();
}

void HewnSimplifyModifier::set_angle_limit(float p_value) {
	angle_limit = CLAMP(p_value, 0.0f, 180.0f);
	emit_changed();
}

void HewnSimplifyModifier::apply(hewn::Mesh &r_mesh, const HewnMesh3D *p_owner) const {
	if (mode == PLANAR) {
		hewn::ops::limited_dissolve(r_mesh, std::vector<int>(), angle_limit);
		hewn::remove_unused_vertices(r_mesh);
	} else {
		hewn::ops::decimate_collapse(r_mesh, ratio);
	}
}

void HewnSimplifyModifier::_validate_property(PropertyInfo &p_property) const {
	if ((p_property.name == StringName("ratio") && mode != COLLAPSE) || (p_property.name == StringName("angle_limit") && mode != PLANAR)) {
		p_property.usage = PROPERTY_USAGE_STORAGE;
	}
}

void HewnSimplifyModifier::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_mode", "mode"), &HewnSimplifyModifier::set_mode);
	ClassDB::bind_method(D_METHOD("get_mode"), &HewnSimplifyModifier::get_mode);
	ClassDB::bind_method(D_METHOD("set_ratio", "ratio"), &HewnSimplifyModifier::set_ratio);
	ClassDB::bind_method(D_METHOD("get_ratio"), &HewnSimplifyModifier::get_ratio);
	ClassDB::bind_method(D_METHOD("set_angle_limit", "degrees"), &HewnSimplifyModifier::set_angle_limit);
	ClassDB::bind_method(D_METHOD("get_angle_limit"), &HewnSimplifyModifier::get_angle_limit);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mode", PROPERTY_HINT_ENUM, "Collapse,Planar"), "set_mode", "get_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ratio", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_ratio", "get_ratio");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "angle_limit", PROPERTY_HINT_RANGE, "0,180,0.1,degrees"), "set_angle_limit", "get_angle_limit");
}

// ------------------------------------------------------------------ boolean

void HewnBooleanModifier::set_object(const NodePath &p_path) {
	object = p_path;
	emit_changed();
}

NodePath HewnBooleanModifier::get_object() const {
	return object;
}

void HewnBooleanModifier::set_operation(int p_op) {
	operation = CLAMP(p_op, 0, 2);
	emit_changed();
}

int HewnBooleanModifier::get_operation() const {
	return operation;
}

void HewnBooleanModifier::apply(hewn::Mesh &r_mesh, const HewnMesh3D *p_owner) const {
	HewnMesh3D *o = Object::cast_to<HewnMesh3D>(p_owner->resolve_path(object));
	if (o && o != p_owner) {
		p_owner->apply_boolean(r_mesh, o, operation);
	}
}

void HewnBooleanModifier::get_dependencies(const HewnMesh3D *p_owner, std::vector<Node *> &r_nodes) const {
	if (Node *o = p_owner->resolve_path(object)) {
		r_nodes.push_back(o);
	}
}

void HewnBooleanModifier::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_object", "path"), &HewnBooleanModifier::set_object);
	ClassDB::bind_method(D_METHOD("get_object"), &HewnBooleanModifier::get_object);
	ClassDB::bind_method(D_METHOD("set_operation", "op"), &HewnBooleanModifier::set_operation);
	ClassDB::bind_method(D_METHOD("get_operation"), &HewnBooleanModifier::get_operation);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "object", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "HewnMesh3D"), "set_object", "get_object");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "operation", PROPERTY_HINT_ENUM, "Union,Difference,Intersect"), "set_operation", "get_operation");
}

// ------------------------------------------------------------------ array

#define HW_SET(m_type, m_name, m_expr, m_relist)        \
	void HewnArrayModifier::set_##m_name(m_type p_value) { \
		m_name = m_expr;                                 \
		if (m_relist) {                                  \
			notify_property_list_changed();              \
		}                                                \
		emit_changed();                                  \
	}
HW_SET(int, shape, CLAMP(p_value, 0, 2), true)
HW_SET(int, count, CLAMP(p_value, 1, 10000), false)
HW_SET(int, count_method, CLAMP(p_value, 0, 1), true)
HW_SET(int, offset_method, CLAMP(p_value, 0, 2), true)
HW_SET(Vector3, relative_offset, p_value, false)
HW_SET(Vector3, translation, p_value, false)
HW_SET(Vector3, rotation, p_value, false)
HW_SET(Vector3, scale, p_value, false)
HW_SET(int, central_axis, CLAMP(p_value, 0, 2), false)
HW_SET(float, radius, p_value, false)
HW_SET(bool, full_circle, p_value, true)
HW_SET(float, sweep_angle, p_value, false)
HW_SET(float, angular_distance, MAX(0.01f, p_value), false)
HW_SET(NodePath, path, p_value, false)
HW_SET(float, distance, MAX(0.001f, p_value), false)
HW_SET(bool, align_rotation, p_value, true)
HW_SET(int, forward_axis, CLAMP(p_value, 0, 5), false)
HW_SET(int, up_axis, CLAMP(p_value, 0, 2), false)
HW_SET(Vector3, random_offset, p_value, false)
HW_SET(Vector3, random_rotation, p_value, false)
HW_SET(float, random_scale, MAX(0.0f, p_value), false)
HW_SET(bool, random_scale_uniform, p_value, false)
HW_SET(bool, random_exclude_first, p_value, false)
HW_SET(int, seed, p_value, false)
HW_SET(bool, merge, p_value, true)
HW_SET(float, merge_distance, MAX(0.0f, p_value), false)
#undef HW_SET

namespace {

// Basis with the chosen forward axis along f and the up axis as close to u as possible.
Basis aligned_basis(Vector3 f, Vector3 u, int p_forward, int p_up) {
	if (f.length() < 1e-9f) {
		return Basis();
	}
	f.normalize();
	u -= f * u.dot(f);
	if (u.length() < 1e-6f) {
		u = std::fabs(f.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
		u -= f * u.dot(f);
	}
	u.normalize();
	const int fa = p_forward % 3;
	const float fs = p_forward >= 3 ? -1.0f : 1.0f;
	int ua = p_up % 3;
	if (ua == fa) {
		ua = (fa + 1) % 3;
	}
	const int ra = 3 - fa - ua;
	Vector3 cols[3];
	cols[fa] = f * fs;
	cols[ua] = u;
	cols[ra] = cols[(ra + 1) % 3].cross(cols[(ra + 2) % 3]); // right-handed
	Basis b;
	for (int i = 0; i < 3; i++) {
		b.set_column(i, cols[i]);
	}
	return b;
}

Basis euler_deg(const Vector3 &p_deg) {
	return Basis::from_euler(p_deg * ((float)Math::PI / 180.0f));
}

} // namespace

std::vector<Transform3D> HewnArrayModifier::_copies(const hewn::Mesh &p_base, const HewnMesh3D *p_owner) const {
	std::vector<Transform3D> out;
	if (shape == SHAPE_LINE) {
		const int n = count;
		if (offset_method == OFFSET_ENDPOINT) {
			for (int i = 0; i < n; i++) {
				const float t = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
				const Basis b = euler_deg(rotation * t) * Basis::from_scale(Vector3(1, 1, 1).lerp(scale, t));
				out.push_back(Transform3D(b, translation * t));
			}
		} else {
			Vector3 step = translation;
			if (offset_method == OFFSET_RELATIVE) {
				AABB box;
				for (size_t i = 0; i < p_base.pos.size(); i++) {
					if (i == 0) {
						box = AABB(p_base.pos[0], Vector3());
					} else {
						box.expand_to(p_base.pos[i]);
					}
				}
				step = relative_offset * box.size;
			}
			const Transform3D T(euler_deg(rotation) * Basis::from_scale(scale), step);
			Transform3D acc;
			for (int i = 0; i < n; i++) {
				out.push_back(acc);
				acc = acc * T; // each copy follows the previous one (rotation makes a spiral)
			}
		}
	} else if (shape == SHAPE_CIRCLE) {
		const float full = (float)Math::TAU;
		const float sweep = full_circle ? full : sweep_angle * (float)Math::PI / 180.0f;
		int n = count;
		float step;
		if (count_method == COUNT_DISTANCE) {
			step = angular_distance * (float)Math::PI / 180.0f;
			n = MAX(1, (int)std::floor(std::fabs(sweep) / step + (full_circle ? 0.0f : 1.0f) + 1e-4f));
			if (sweep < 0.0f) {
				step = -step;
			}
		} else {
			step = full_circle ? full / (float)n : (n > 1 ? sweep / (float)(n - 1) : 0.0f);
		}
		Vector3 axis, U, V;
		axis[central_axis] = 1.0f;
		U[(central_axis + 1) % 3] = 1.0f;
		V = axis.cross(U);
		for (int i = 0; i < n; i++) {
			const float a = step * (float)i;
			const Vector3 radial = U * std::cos(a) + V * std::sin(a);
			const Vector3 tangent = axis.cross(radial);
			const Basis b = align_rotation ? aligned_basis(tangent, axis, forward_axis, up_axis) : Basis();
			out.push_back(Transform3D(b, radial * radius)); // around the object's origin
		}
	} else {
		Path3D *p = Object::cast_to<Path3D>(p_owner->resolve_path(path));
		if (!p || p->get_curve().is_null() || !p->is_inside_tree() || !p_owner->is_inside_tree()) {
			out.push_back(Transform3D());
			return out;
		}
		Ref<Curve3D> c = p->get_curve();
		const float L = c->get_baked_length();
		if (L <= 0.0f) {
			out.push_back(Transform3D());
			return out;
		}
		const Transform3D rel = p_owner->get_global_transform().affine_inverse() * p->get_global_transform();
		int n = count;
		float step;
		if (count_method == COUNT_DISTANCE) {
			step = distance;
			n = (int)std::floor(L / step + 1e-4f) + 1;
		} else {
			step = n > 1 ? L / (float)(n - 1) : 0.0f;
		}
		n = MIN(n, 10000);
		const float eps = MIN(0.01f, L * 0.001f);
		for (int i = 0; i < n; i++) {
			const float o = MIN(L, step * (float)i);
			const Vector3 pt = c->sample_baked(o, true);
			const Vector3 tangent = c->sample_baked(MIN(L, o + eps), true) - c->sample_baked(MAX(0.0f, o - eps), true);
			const Vector3 up = c->is_up_vector_enabled() ? c->sample_baked_up_vector(o, true) : Vector3(0, 1, 0);
			Basis b;
			if (align_rotation) {
				b = aligned_basis(rel.basis.xform(tangent), rel.basis.xform(up), forward_axis, up_axis);
			}
			out.push_back(Transform3D(b, rel.xform(pt)));
		}
	}
	// randomize (each copy around its own origin; the offset in the object's space)
	if (random_offset != Vector3() || random_rotation != Vector3() || random_scale > 0.0f) {
		for (size_t i = 0; i < out.size(); i++) {
			if (i == 0 && random_exclude_first) {
				continue;
			}
			std::mt19937 rng((uint32_t)(seed * 7919 + (int)i * 104729 + 17));
			std::uniform_real_distribution<float> u(-1.0f, 1.0f);
			const Vector3 ro(u(rng) * random_offset.x, u(rng) * random_offset.y, u(rng) * random_offset.z);
			const Vector3 rr(u(rng) * random_rotation.x, u(rng) * random_rotation.y, u(rng) * random_rotation.z);
			Vector3 rs;
			if (random_scale_uniform) {
				const float s = MAX(0.001f, 1.0f + u(rng) * random_scale);
				rs = Vector3(s, s, s);
			} else {
				rs = Vector3(MAX(0.001f, 1.0f + u(rng) * random_scale), MAX(0.001f, 1.0f + u(rng) * random_scale), MAX(0.001f, 1.0f + u(rng) * random_scale));
			}
			out[i] = Transform3D(Basis(), ro) * out[i] * Transform3D(euler_deg(rr) * Basis::from_scale(rs), Vector3());
		}
	}
	return out;
}

void HewnArrayModifier::apply(hewn::Mesh &r_mesh, const HewnMesh3D *p_owner) const {
	const hewn::Mesh base = r_mesh;
	const std::vector<Transform3D> copies = _copies(base, p_owner);
	if (copies.size() == 1 && copies[0] == Transform3D()) {
		return;
	}
	r_mesh.clear();
	for (const Transform3D &t : copies) {
		hewn::ops::append(r_mesh, base, t, 0);
	}
	if (merge) {
		hewn::weld_by_distance(r_mesh, MAX(1e-6f, merge_distance));
	}
}

void HewnArrayModifier::get_dependencies(const HewnMesh3D *p_owner, std::vector<Node *> &r_nodes) const {
	if (shape == SHAPE_CURVE) {
		if (Node *p = p_owner->resolve_path(path)) {
			r_nodes.push_back(p);
		}
	}
}

void HewnArrayModifier::_validate_property(PropertyInfo &p_property) const {
	const String n = p_property.name;
	bool show = true;
	if (n == "offset_method" || n == "relative_offset" || n == "translation" || n == "rotation" || n == "scale") {
		show = shape == SHAPE_LINE;
		if (n == "relative_offset") {
			show = show && offset_method == OFFSET_RELATIVE;
		} else if (n == "translation") {
			show = show && offset_method != OFFSET_RELATIVE;
		}
	} else if (n == "central_axis" || n == "radius" || n == "full_circle") {
		show = shape == SHAPE_CIRCLE;
	} else if (n == "sweep_angle") {
		show = shape == SHAPE_CIRCLE && !full_circle;
	} else if (n == "angular_distance") {
		show = shape == SHAPE_CIRCLE && count_method == COUNT_DISTANCE;
	} else if (n == "path") {
		show = shape == SHAPE_CURVE;
	} else if (n == "distance") {
		show = shape == SHAPE_CURVE && count_method == COUNT_DISTANCE;
	} else if (n == "count_method") {
		show = shape != SHAPE_LINE;
	} else if (n == "count") {
		show = shape == SHAPE_LINE || count_method == COUNT_COUNT;
	} else if (n == "align_rotation") {
		show = shape != SHAPE_LINE;
	} else if (n == "forward_axis" || n == "up_axis") {
		show = shape != SHAPE_LINE && align_rotation;
	} else if (n == "merge_distance") {
		show = merge;
	}
	if (!show) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

void HewnArrayModifier::_bind_methods() {
#define HW_BIND(m_name)                                                                       \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &HewnArrayModifier::set_##m_name); \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &HewnArrayModifier::get_##m_name);
	HW_BIND(shape)
	HW_BIND(count)
	HW_BIND(count_method)
	HW_BIND(offset_method)
	HW_BIND(relative_offset)
	HW_BIND(translation)
	HW_BIND(rotation)
	HW_BIND(scale)
	HW_BIND(central_axis)
	HW_BIND(radius)
	HW_BIND(full_circle)
	HW_BIND(sweep_angle)
	HW_BIND(angular_distance)
	HW_BIND(path)
	HW_BIND(distance)
	HW_BIND(align_rotation)
	HW_BIND(forward_axis)
	HW_BIND(up_axis)
	HW_BIND(random_offset)
	HW_BIND(random_rotation)
	HW_BIND(random_scale)
	HW_BIND(random_scale_uniform)
	HW_BIND(random_exclude_first)
	HW_BIND(seed)
	HW_BIND(merge)
	HW_BIND(merge_distance)
#undef HW_BIND
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shape", PROPERTY_HINT_ENUM, "Line,Circle,Curve (Path3D)"), "set_shape", "get_shape");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "count_method", PROPERTY_HINT_ENUM, "Count,Distance"), "set_count_method", "get_count_method");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "count", PROPERTY_HINT_RANGE, "1,1000,1,or_greater"), "set_count", "get_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "offset_method", PROPERTY_HINT_ENUM, "Relative,Constant,Endpoint"), "set_offset_method", "get_offset_method");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "relative_offset"), "set_relative_offset", "get_relative_offset");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "translation", PROPERTY_HINT_NONE, "suffix:m"), "set_translation", "get_translation");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "rotation", PROPERTY_HINT_NONE, "suffix:" + String::chr(0x00B0)), "set_rotation", "get_rotation");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "scale", PROPERTY_HINT_LINK), "set_scale", "get_scale");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "central_axis", PROPERTY_HINT_ENUM, "X,Y,Z"), "set_central_axis", "get_central_axis");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "0,100,0.001,or_greater,suffix:m"), "set_radius", "get_radius");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "full_circle"), "set_full_circle", "get_full_circle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sweep_angle", PROPERTY_HINT_RANGE, "-360,360,0.1,degrees"), "set_sweep_angle", "get_sweep_angle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "angular_distance", PROPERTY_HINT_RANGE, "0.1,360,0.1,degrees"), "set_angular_distance", "get_angular_distance");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Path3D"), "set_path", "get_path");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "distance", PROPERTY_HINT_RANGE, "0.001,100,0.001,or_greater,suffix:m"), "set_distance", "get_distance");
	ADD_GROUP("Align Rotation", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "align_rotation"), "set_align_rotation", "get_align_rotation");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "forward_axis", PROPERTY_HINT_ENUM, "X,Y,Z,-X,-Y,-Z"), "set_forward_axis", "get_forward_axis");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "up_axis", PROPERTY_HINT_ENUM, "X,Y,Z"), "set_up_axis", "get_up_axis");
	ADD_GROUP("Randomize", "random_");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "random_offset", PROPERTY_HINT_NONE, "suffix:m"), "set_random_offset", "get_random_offset");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "random_rotation", PROPERTY_HINT_NONE, "suffix:" + String::chr(0x00B0)), "set_random_rotation", "get_random_rotation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "random_scale", PROPERTY_HINT_RANGE, "0,1,0.01,or_greater"), "set_random_scale", "get_random_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "random_scale_uniform"), "set_random_scale_uniform", "get_random_scale_uniform");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "random_exclude_first"), "set_random_exclude_first", "get_random_exclude_first");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "random_seed"), "set_seed", "get_seed"); // ("Seed" in the Randomize group)
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_seed", "get_seed"); // (older scenes)
	ADD_GROUP("Merge", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "merge"), "set_merge", "get_merge");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "merge_distance", PROPERTY_HINT_RANGE, "0,1,0.0001,or_greater"), "set_merge_distance", "get_merge_distance");
}
