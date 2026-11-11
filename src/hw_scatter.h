// Scatter: copies of meshes painted on surfaces (grass, stones, bushes). They are kept in square chunks (by the
// node's local X / Z), each drawn by its own MultiMeshInstance3D per mesh with a visibility range; a brush stroke
// builds again only the chunks it touched. The meshes are picked by their weights; each copy turned at random
// around its up (and tilted, turned around its X / Z), scaled at random, standing up or along the surface's normal.
// A mesh can collide in the game (run or baked: a StaticBody3D per chunk with a shape for each copy).
#pragma once

#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/shape3d.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <unordered_set>
#include <vector>
#include <functional>

namespace godot {

class HewnScatter3D : public Node3D {
	GDCLASS(HewnScatter3D, Node3D)

public:
	static bool show_collision; // (the editor's Collision overlay: the copies' shapes drawn)
	// the outlines of the copies' collision shapes (local), up to p_max copies
	PackedVector3Array collision_lines(int p_max) const;
	void each_collision(const std::function<void(const Ref<Shape3D> &, const Transform3D &)> &p_fn) const; // (world space)
	enum Align {
		ALIGN_UP = 0,
		ALIGN_NORMAL = 1,
	};
	struct Inst {
		Transform3D xf; // (local)
		int item = 0;
	};

private:
	TypedArray<Mesh> meshes;
	PackedFloat32Array weights;
	float density = 2.0f; // copies per square meter
	float brush_radius = 2.0f; // meters
	PackedInt32Array paint_items; // (which of the meshes the brush places; empty: all)
	float erase_strength = 1.0f; // (the eraser takes each copy with this chance: less thins them out)
	float scale_min = 0.8f, scale_max = 1.2f;
	float random_rotation = 360.0f; // degrees, around the up
	float random_tilt = 0.0f; // degrees
	float random_rotation_x = 0.0f, random_rotation_z = 0.0f; // degrees, around the copy's own X / Z
	Vector3 base_rotation; // degrees: the mesh turned first (one modeled lying down stood up)
	Array collisions; // (per mesh: 0 none, 1 convex, 2 trimesh, 3 box)
	uint32_t collision_layer = 1, collision_mask = 1;
	int align = ALIGN_UP;
	float offset = 0.0f; // along the up (below zero: sunk into the ground)
	float chunk_size = 32.0f;
	Dictionary game_metadata;
	PackedStringArray game_groups;
	Ref<Script> mesh_script, collision_script; // (on each copy's MultiMeshInstance3D, each StaticBody3D of the game)
	float visibility_range = 0.0f; // (0: always drawn)
	bool cast_shadows = true;

	std::map<uint64_t, std::vector<Inst>> chunks;
	std::vector<Ref<Shape3D>> _collision_shapes() const; // (per mesh: its shape, null for none)
	std::map<uint64_t, std::vector<ObjectID>> chunk_nodes; // (internal children, one per mesh)
	std::set<uint64_t> dirty;
	bool flush_queued = false;
	int last_rebuilt = 0;
	std::unordered_set<uint64_t> stroke_seen; // (thinning: each copy once per stroke)
	std::mt19937 rng{ std::random_device{}() };

	uint64_t _key_of(const Vector3 &p_local) const;
	void _mark(uint64_t p_key);
	void _rebuild_chunk(uint64_t p_key);
	void _rebuild_all();
	void _clear_nodes();
	int _pick_item();
	float _rand(float p_a, float p_b);
	float _spacing() const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_meshes(const TypedArray<Mesh> &p_meshes);
	TypedArray<Mesh> get_meshes() const { return meshes; }
	void set_weights(const PackedFloat32Array &p_weights);
	PackedFloat32Array get_weights() const { return weights; }
	void set_density(float p_v) { density = MAX(0.001f, p_v); }
	float get_density() const { return density; }
	void set_brush_radius(float p_v) { brush_radius = MAX(0.01f, p_v); }
	float get_brush_radius() const { return brush_radius; }
	void set_paint_items(const PackedInt32Array &p_v) { paint_items = p_v; }
	PackedInt32Array get_paint_items() const { return paint_items; }
	void set_erase_strength(float p_v) { erase_strength = CLAMP(p_v, 0.0f, 1.0f); }
	float get_erase_strength() const { return erase_strength; }
	void set_scale_min(float p_v) { scale_min = MAX(0.001f, p_v); }
	float get_scale_min() const { return scale_min; }
	void set_scale_max(float p_v) { scale_max = MAX(0.001f, p_v); }
	float get_scale_max() const { return scale_max; }
	void set_random_rotation(float p_v) { random_rotation = CLAMP(p_v, 0.0f, 360.0f); }
	float get_random_rotation() const { return random_rotation; }
	void set_random_tilt(float p_v) { random_tilt = CLAMP(p_v, 0.0f, 90.0f); }
	float get_random_tilt() const { return random_tilt; }
	void set_random_rotation_x(float p_v) { random_rotation_x = CLAMP(p_v, 0.0f, 360.0f); }
	float get_random_rotation_x() const { return random_rotation_x; }
	void set_random_rotation_z(float p_v) { random_rotation_z = CLAMP(p_v, 0.0f, 360.0f); }
	float get_random_rotation_z() const { return random_rotation_z; }
	void set_base_rotation(const Vector3 &p_v) { base_rotation = p_v; }
	Vector3 get_base_rotation() const { return base_rotation; }
	void set_collisions(const Array &p_v) {
		collisions = p_v;
		update_gizmos(); // (the Collision overlay)
	}
	Array get_collisions() const { return collisions; }
	void set_collision_layer(uint32_t p_v) { collision_layer = p_v; }
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_v) { collision_mask = p_v; }
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_align(int p_v) { align = CLAMP(p_v, 0, 1); }
	int get_align() const { return align; }
	void set_offset(float p_v) { offset = p_v; }
	float get_offset() const { return offset; }
	void set_game_metadata(const Dictionary &p_v) { game_metadata = p_v; }
	Dictionary get_game_metadata() const { return game_metadata; }
	void set_game_groups(const PackedStringArray &p_v) { game_groups = p_v; }
	PackedStringArray get_game_groups() const { return game_groups; }
	void set_mesh_script(const Ref<Script> &p_v) { mesh_script = p_v; }
	Ref<Script> get_mesh_script() const { return mesh_script; }
	void set_collision_script(const Ref<Script> &p_v) { collision_script = p_v; }
	Ref<Script> get_collision_script() const { return collision_script; }
	void set_chunk_size(float p_v);
	float get_chunk_size() const { return chunk_size; }
	void set_visibility_range(float p_v);
	float get_visibility_range() const { return visibility_range; }
	void set_cast_shadows(bool p_v);
	bool get_cast_shadows() const { return cast_shadows; }

	// every copy: 12 floats of its local transform (the basis' columns, then the origin) and its mesh's index
	void set_instances(const PackedFloat32Array &p_data);
	PackedFloat32Array get_instances() const;

	// A copy at p_pos on a surface turned to p_normal (both global), unless one stands nearer than the density
	// allows; true when it was added.
	bool paint_point(const Vector3 &p_pos, const Vector3 &p_normal);
	// The copies within p_radius of p_center (global) taken away: each with the chance p_fraction (thinning: each
	// copy only once in a stroke, begin_stroke).
	int erase(const Vector3 &p_center, float p_radius, float p_fraction);
	void begin_stroke() { stroke_seen.clear(); }
	void clear();
	// the touched chunks built again now (otherwise at the end of the frame); how many were
	int flush();
	int get_instance_count() const;
	int get_chunk_count() const;
	int get_last_rebuilt() const { return last_rebuilt; }
	// (baking) plain MultiMeshInstance3D nodes under a Node3D standing where this one does
	Node *make_game_node();
	void _add_collision_bodies(Node *p_under) const; // (a body per chunk and mesh that collides, a shape for each copy)
};

} // namespace godot
