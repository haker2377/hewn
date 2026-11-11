// Things placed along a Hewn curve (its Placement list): copies of a mesh at one side of it, at both, or in the
// middle (a fence, posts, rocks scattered along a path, bushes here and there), drawn as one MultiMesh each.
#pragma once

#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/shape3d.hpp>

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
		COLLISION_CUSTOM,
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
	Ref<Shape3D> custom_shape; // (COLLISION_CUSTOM: for each copy, this far from its origin)
	Vector3 custom_shape_offset;
	int collision_layer = 1, collision_mask = 1;
	bool cast_shadows = true;
	int seed = 0;
	float chunk_size = 32.0f; // (the copies drawn by squares of this size; 0: all together)
	float visibility_range = 0.0f; // (0: always drawn)
	Dictionary game_metadata;
	PackedStringArray game_groups;
	Ref<Script> mesh_script, collision_script;

	// (which fields are shown: the Inspector's list is made again only when that changes, else a value being
	// dragged would stop at each step)
	int _shows() const { return (side == SIDE_CENTER ? 1 : 0) | (patch_length <= 1e-4f ? 2 : 0) | (drop_to_ground ? 4 : 0) | (collision == COLLISION_CUSTOM ? 8 : 0); }

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	// The copies' transforms (in the curve's local space) along the splines given (their samples, local): p_ray
	// finds the ground under a point (global), for Drop to Ground.
	std::vector<Transform3D> transforms(const std::vector<std::vector<Vector3>> &p_splines, const std::vector<bool> &p_cyclic, const Transform3D &p_global,
			const std::function<bool(const Vector3 &, const Vector3 &, Vector3 &, Vector3 &)> &p_ray) const;

#define HW_PP(m_type, m_name)                	void set_##m_name(m_type p_v) {           		const int shown = _shows();           		m_name = p_v;                         		emit_changed();                       		if (_shows() != shown) {              			notify_property_list_changed();   		}                                     	}                                         	m_type get_##m_name() const { return m_name; }
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
	HW_PP(Ref<Shape3D>, custom_shape)
	HW_PP(Vector3, custom_shape_offset)
	HW_PP(int, collision_layer)
	HW_PP(int, collision_mask)
	HW_PP(bool, cast_shadows)
	HW_PP(int, seed)
	HW_PP(float, chunk_size)
	HW_PP(float, visibility_range)
	HW_PP(Dictionary, game_metadata)
	HW_PP(PackedStringArray, game_groups)
	HW_PP(Ref<Script>, mesh_script)
	HW_PP(Ref<Script>, collision_script)
#undef HW_PP
	void set_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_mesh() const { return mesh; }
};

} // namespace godot

VARIANT_ENUM_CAST(HewnPathPlacement::Side);
VARIANT_ENUM_CAST(HewnPathPlacement::Collision);
