#pragma once

// Modifier stack of HewnMesh3D: resources in the node's `modifiers` array,
// applied in order to a copy of the mesh for display, collision and export.

#include "hw_core.h"

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/node_path.hpp>

#include <vector>

namespace godot {

class HewnMesh3D;
class Node;

class HewnModifier : public Resource {
	GDCLASS(HewnModifier, Resource)

	bool enabled = true;

protected:
	static void _bind_methods();

public:
	void set_enabled(bool p_on);
	bool is_enabled() const;
	virtual void apply(hewn::Mesh &r_mesh, const HewnMesh3D *p_owner) const {}
	// Nodes whose changes change the result (boolean operands, paths).
	virtual void get_dependencies(const HewnMesh3D *p_owner, std::vector<Node *> &r_nodes) const {}
	// Too slow to follow every step of a move / stroke: left out while it goes on, applied when it ends.
	virtual bool is_heavy() const { return false; }
};

class HewnMirrorModifier : public HewnModifier {
	GDCLASS(HewnMirrorModifier, HewnModifier)

	bool axes[3] = { true, false, false };
	bool merge = true;
	float merge_distance = 0.001f;
	bool clipping = false; // edit mode: vertices on the mirror plane stay on it, none crosses it
	NodePath mirror_object; // mirrored across this node's planes instead of its own

protected:
	static void _bind_methods();

public:
	void set_axis(int p_axis, bool p_on);
	bool get_axis(int p_axis) const;
	void set_x(bool p_on) { set_axis(0, p_on); }
	bool get_x() const { return axes[0]; }
	void set_y(bool p_on) { set_axis(1, p_on); }
	bool get_y() const { return axes[1]; }
	void set_z(bool p_on) { set_axis(2, p_on); }
	bool get_z() const { return axes[2]; }
	void set_merge(bool p_on);
	bool get_merge() const { return merge; }
	void set_merge_distance(float p_dist);
	float get_merge_distance() const;
	void set_clipping(bool p_on);
	bool get_clipping() const;
	void set_mirror_object(const NodePath &p_path);
	NodePath get_mirror_object() const;
	void apply(hewn::Mesh &r_mesh, const HewnMesh3D *p_owner) const override;
	void get_dependencies(const HewnMesh3D *p_owner, std::vector<Node *> &r_nodes) const override;
};

class HewnSolidifyModifier : public HewnModifier {
	GDCLASS(HewnSolidifyModifier, HewnModifier)

	float thickness = 0.1f;
	float offset = -1.0f;

protected:
	static void _bind_methods();

public:
	void set_thickness(float p_value);
	float get_thickness() const;
	void set_offset(float p_value);
	float get_offset() const;
	void apply(hewn::Mesh &r_mesh, const HewnMesh3D *p_owner) const override;
};

class HewnBooleanModifier : public HewnModifier {
	GDCLASS(HewnBooleanModifier, HewnModifier)

	NodePath object;
	int operation = 1; // HewnMesh3D::BooleanOp (subtract)

protected:
	static void _bind_methods();

public:
	void set_object(const NodePath &p_path);
	NodePath get_object() const;
	void set_operation(int p_op);
	int get_operation() const;
	void apply(hewn::Mesh &r_mesh, const HewnMesh3D *p_owner) const override;
	void get_dependencies(const HewnMesh3D *p_owner, std::vector<Node *> &r_nodes) const override;
};

// Subdivision Surface: Catmull-Clark (or Simple), its levels, creases, boundary and UV smoothing.
class HewnSubdivisionModifier : public HewnModifier {
	GDCLASS(HewnSubdivisionModifier, HewnModifier)

public:
	enum Kind {
		CATMULL_CLARK,
		SIMPLE,
	};
	enum Boundary {
		BOUNDARY_ALL,
		BOUNDARY_KEEP_CORNERS,
	};
	enum UVSmooth {
		UV_NONE,
		UV_KEEP_BOUNDARIES,
	};

private:
	int kind = CATMULL_CLARK;
	int levels = 1;
	bool use_creases = true;
	int boundary_smooth = BOUNDARY_ALL;
	int uv_smooth = UV_KEEP_BOUNDARIES;

protected:
	static void _bind_methods();

public:
	void set_kind(int p_value);
	int get_kind() const { return kind; }
	void set_levels(int p_value);
	int get_levels() const { return levels; }
	void set_use_creases(bool p_value);
	bool get_use_creases() const { return use_creases; }
	void set_boundary_smooth(int p_value);
	int get_boundary_smooth() const { return boundary_smooth; }
	void set_uv_smooth(int p_value);
	int get_uv_smooth() const { return uv_smooth; }
	void apply(hewn::Mesh &r_mesh, const HewnMesh3D *p_owner) const override;
};

// Simplify: Collapse (edges collapsed down to a ratio of the triangles) or Planar (neighbors joined where they meet
// at less than an angle).
class HewnSimplifyModifier : public HewnModifier {
	GDCLASS(HewnSimplifyModifier, HewnModifier)

public:
	enum Mode {
		COLLAPSE,
		PLANAR,
	};

private:
	int mode = COLLAPSE;
	float ratio = 1.0f;
	float angle_limit = 0.0f; // degrees

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_mode(int p_value);
	int get_mode() const { return mode; }
	void set_ratio(float p_value);
	float get_ratio() const { return ratio; }
	void set_angle_limit(float p_value);
	float get_angle_limit() const { return angle_limit; }
	void apply(hewn::Mesh &r_mesh, const HewnMesh3D *p_owner) const override;
	bool is_heavy() const override { return true; }
};

// Remesh: the shape made again on a voxel grid: Blocks, Smooth, Sharp (octree depth, scale, sharpness) or Voxel
// (voxel size, adaptivity); small separate pieces may go; smooth shading or not.
class HewnRemeshModifier : public HewnModifier {
	GDCLASS(HewnRemeshModifier, HewnModifier)

	int mode = hewn::ops::REMESH_VOXEL;
	float voxel_size = 0.1f;
	float adaptivity = 0.0f;
	int octree_depth = 4;
	float scale = 0.9f;
	float sharpness = 1.0f;
	bool remove_disconnected = true;
	float threshold = 1.0f;
	bool smooth_shading = false;

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_mode(int p_value);
	int get_mode() const { return mode; }
	void set_voxel_size(float p_value);
	float get_voxel_size() const { return voxel_size; }
	void set_adaptivity(float p_value);
	float get_adaptivity() const { return adaptivity; }
	void set_octree_depth(int p_value);
	int get_octree_depth() const { return octree_depth; }
	void set_scale(float p_value);
	float get_scale() const { return scale; }
	void set_sharpness(float p_value);
	float get_sharpness() const { return sharpness; }
	void set_remove_disconnected(bool p_value);
	bool get_remove_disconnected() const { return remove_disconnected; }
	void set_threshold(float p_value);
	float get_threshold() const { return threshold; }
	void set_smooth_shading(bool p_value);
	bool get_smooth_shading() const { return smooth_shading; }
	void apply(hewn::Mesh &r_mesh, const HewnMesh3D *p_owner) const override;
	bool is_heavy() const override { return true; }
};

// Array: copies along a line (relative / constant offset or up to an end point), around a
// circle, or along a Path3D; per-copy rotation / scale, randomization, merging.
class HewnArrayModifier : public HewnModifier {
	GDCLASS(HewnArrayModifier, HewnModifier)

public:
	enum Shape {
		SHAPE_LINE,
		SHAPE_CIRCLE,
		SHAPE_CURVE,
	};
	enum OffsetMethod {
		OFFSET_RELATIVE,
		OFFSET_CONSTANT,
		OFFSET_ENDPOINT,
	};
	enum CountMethod {
		COUNT_COUNT,
		COUNT_DISTANCE,
	};

private:
	int shape = SHAPE_LINE;
	int count = 3;
	int count_method = COUNT_COUNT;
	// line
	int offset_method = OFFSET_RELATIVE;
	Vector3 relative_offset = Vector3(1, 0, 0);
	Vector3 translation = Vector3(1, 0, 0);
	Vector3 rotation; // degrees per copy
	Vector3 scale = Vector3(1, 1, 1); // per copy
	// circle
	int central_axis = 1; // X, Y, Z
	float radius = 1.0f;
	bool full_circle = true;
	float sweep_angle = 180.0f;
	float angular_distance = 30.0f;
	// curve
	NodePath path;
	float distance = 1.0f;
	// alignment (circle, curve)
	bool align_rotation = true;
	int forward_axis = 0; // X, Y, Z, -X, -Y, -Z
	int up_axis = 1;
	// randomize
	Vector3 random_offset;
	Vector3 random_rotation; // degrees
	float random_scale = 0.0f;
	bool random_scale_uniform = true;
	bool random_exclude_first = true;
	int seed = 0;
	// merge
	bool merge = false;
	float merge_distance = 0.001f;

	std::vector<Transform3D> _copies(const hewn::Mesh &p_base, const HewnMesh3D *p_owner) const;

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
#define HW_PROP(m_type, m_name)            \
	void set_##m_name(m_type p_value);     \
	m_type get_##m_name() const { return m_name; }
	HW_PROP(int, shape)
	HW_PROP(int, count)
	HW_PROP(int, count_method)
	HW_PROP(int, offset_method)
	HW_PROP(Vector3, relative_offset)
	HW_PROP(Vector3, translation)
	HW_PROP(Vector3, rotation)
	HW_PROP(Vector3, scale)
	HW_PROP(int, central_axis)
	HW_PROP(float, radius)
	HW_PROP(bool, full_circle)
	HW_PROP(float, sweep_angle)
	HW_PROP(float, angular_distance)
	HW_PROP(NodePath, path)
	HW_PROP(float, distance)
	HW_PROP(bool, align_rotation)
	HW_PROP(int, forward_axis)
	HW_PROP(int, up_axis)
	HW_PROP(Vector3, random_offset)
	HW_PROP(Vector3, random_rotation)
	HW_PROP(float, random_scale)
	HW_PROP(bool, random_scale_uniform)
	HW_PROP(bool, random_exclude_first)
	HW_PROP(int, seed)
	HW_PROP(bool, merge)
	HW_PROP(float, merge_distance)
#undef HW_PROP
	void apply(hewn::Mesh &r_mesh, const HewnMesh3D *p_owner) const override;
	void get_dependencies(const HewnMesh3D *p_owner, std::vector<Node *> &r_nodes) const override;
};

} // namespace godot

