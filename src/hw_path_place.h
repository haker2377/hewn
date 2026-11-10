// Things placed along a Hewn curve (its Placement list): copies of a mesh at one side of it, at both, or in the
// middle (a fence, posts, rocks scattered along a path, bushes here and there), drawn as one MultiMesh each.
#pragma once

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/resource.hpp>

#include <functional>
#include <vector>

namespace godot {

class HewnPathPlacement : public Resource {
	GDCLASS(HewnPathPlacement, Resource)

public:
	enum Side {
		SIDE_CENTER,
		SIDE_LEFT,
		SIDE_RIGHT,
		SIDE_BOTH,
	};
	enum Collision {
		COLLISION_NONE,
		COLLISION_BOX,
		COLLISION_CONVEX,
		COLLISION_TRIMESH,
	};

private:
	bool enabled = true;
	Ref<Mesh> mesh;
	int side = SIDE_CENTER;
	float offset = 1.0f; // (from the path, sideways)
	float height = 0.0f; // (up from it)
	float spacing = 0.0f; // (0: as long as the mesh is along its X)
	float spacing_random = 0.0f; // (0..1 of the spacing)
	float start_offset = 0.0f, end_offset = 0.0f; // (along the path: left free at its ends)
	float chance = 1.0f; // (each place)
	float patch_length = 0.0f; // (0: everywhere; else stretches with and without it, about this long)
	float patch_amount = 0.5f; // (how much of the path has it)
	float spread = 0.0f; // (sideways at random, either way)
	float scale_min = 1.0f, scale_max = 1.0f;
	float random_yaw = 0.0f; // (degrees, either way)
	float yaw = 0.0f; // (degrees: turned this much on top of the path's way)
	bool align = true; // (its X along the path; else as it is)
	bool drop_to_ground = false; // (each copy down onto what is under it)
	bool tilt_to_ground = false;
	int collision = COLLISION_NONE;
	bool cast_shadows = true;
	int seed = 0;

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	// The copies' transforms (in the curve's local space) along the splines given (their samples, local): p_ray
	// finds the ground under a point (global), for Drop to Ground.
	std::vector<Transform3D> transforms(const std::vector<std::vector<Vector3>> &p_splines, const std::vector<bool> &p_cyclic, const Transform3D &p_global,
			const std::function<bool(const Vector3 &, const Vector3 &, Vector3 &, Vector3 &)> &p_ray) const;

#define HW_PP(m_type, m_name)                    \
	void set_##m_name(m_type p_v) {           \
		m_name = p_v;                         \
		emit_changed();                       \
		notify_property_list_changed();       \
	}                                         \
	m_type get_##m_name() const { return m_name; }
	HW_PP(bool, enabled)
	HW_PP(int, side)
	HW_PP(float, offset)
	HW_PP(float, height)
	HW_PP(float, spacing)
	HW_PP(float, spacing_random)
	HW_PP(float, start_offset)
	HW_PP(float, end_offset)
	HW_PP(float, chance)
	HW_PP(float, patch_length)
	HW_PP(float, patch_amount)
	HW_PP(float, spread)
	HW_PP(float, scale_min)
	HW_PP(float, scale_max)
	HW_PP(float, random_yaw)
	HW_PP(float, yaw)
	HW_PP(bool, align)
	HW_PP(bool, drop_to_ground)
	HW_PP(bool, tilt_to_ground)
	HW_PP(int, collision)
	HW_PP(bool, cast_shadows)
	HW_PP(int, seed)
#undef HW_PP
	void set_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_mesh() const { return mesh; }
};

} // namespace godot

VARIANT_ENUM_CAST(HewnPathPlacement::Side);
VARIANT_ENUM_CAST(HewnPathPlacement::Collision);
