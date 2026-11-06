// Overlays: a picture projected along the node's -Y onto the Hewn faces
// under it, clipped to them and kept as geometry (saved with the scene, so the game does not project
// again). All overlays with the same material in a world are drawn as one mesh: one draw call however
// many there are. The outline is the rectangle, or a polygon around the opaque pixels (less overdraw).
#pragma once

#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <vector>

namespace godot {

class HewnMesh3D;

class HewnOverlay3D : public Node3D {
	GDCLASS(HewnOverlay3D, Node3D)

public:
	enum Outline {
		OUTLINE_RECTANGLE = 0,
		OUTLINE_ALPHA = 1, // a convex polygon around the opaque pixels
	};
	enum Blend {
		BLEND_ALPHA_SCISSOR = 0, // hard edges, drawn with the opaque faces (fast)
		BLEND_ALPHA = 1, // soft edges, drawn with the transparent ones
	};

private:
	Ref<Texture2D> texture;
	Ref<Material> material; // instead of the texture's own material
	Vector2 size = Vector2(1, 1);
	float depth = 0.5f; // how far along Y (both ways) it reaches
	Rect2 uv_rect = Rect2(0, 0, 1, 1);
	int outline = OUTLINE_RECTANGLE;
	float alpha_threshold = 0.1f;
	int max_vertices = 8;
	int blend = BLEND_ALPHA_SCISSOR;
	int render_order = 0;
	bool filter_nearest = false;
	float offset = 0.002f; // off the faces (no z-fighting)
	float max_angle = 85.0f; // faces turned further from the overlay's Y are left out
	Color modulate = Color(1, 1, 1, 1);
	bool auto_update = true;

	// the projection, in local space (saved)
	PackedVector3Array cache_verts;
	PackedVector3Array cache_normals;
	PackedVector2Array cache_uvs;
	PackedInt32Array cache_indices;

	void *batch = nullptr; // (the batch it is drawn in)
	mutable PackedVector2Array outline_points;
	mutable bool outline_dirty = true;

	std::vector<ObjectID> watched;
	bool reproject_queued = false;
	void _watch(const std::vector<HewnMesh3D *> &p_meshes);
	void _unwatch();
	void _on_target_changed();
	void _deferred_reproject();
	void _batch_changed();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_texture() const { return texture; }
	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const { return material; }
	void set_size(const Vector2 &p_size);
	Vector2 get_size() const { return size; }
	void set_depth(float p_depth);
	float get_depth() const { return depth; }
	void set_uv_rect(const Rect2 &p_rect);
	Rect2 get_uv_rect() const { return uv_rect; }
	void set_outline(int p_mode);
	int get_outline() const { return outline; }
	void set_alpha_threshold(float p_value);
	float get_alpha_threshold() const { return alpha_threshold; }
	void set_max_vertices(int p_count);
	int get_max_vertices() const { return max_vertices; }
	void set_blend(int p_mode);
	int get_blend() const { return blend; }
	void set_render_order(int p_order);
	int get_render_order() const { return render_order; }
	void set_filter_nearest(bool p_on);
	bool get_filter_nearest() const { return filter_nearest; }
	void set_offset(float p_offset);
	float get_offset() const { return offset; }
	void set_max_angle(float p_deg);
	float get_max_angle() const { return max_angle; }
	void set_modulate(const Color &p_color);
	Color get_modulate() const { return modulate; }
	void set_auto_update(bool p_on) { auto_update = p_on; }
	bool get_auto_update() const { return auto_update; }

	void set_cache(const Dictionary &p_cache);
	Dictionary get_cache() const;

	// The outline in local XZ (meters).
	PackedVector2Array get_outline_points() const;
	// Projects again onto the Hewn meshes of the scene (all of them under `p_root`, or the scene).
	// Returns the number of triangles.
	int reproject(Node *p_root = nullptr);
	int get_triangle_count() const { return (int)cache_indices.size() / 3; }
	// The material the overlay is drawn with (shared by all overlays with the same texture and settings).
	Ref<Material> get_draw_material() const;
	// The projection in world space (for baking): verts, normals, uvs, indices.
	Array get_world_arrays() const;

	// The batches (drawn meshes): how many there are now (tests), and drawing them now.
	static int get_batch_count();
	static void flush_batches();
	static void free_static_resources();

	HewnOverlay3D();
	~HewnOverlay3D();
};

} // namespace godot

VARIANT_ENUM_CAST(HewnOverlay3D::Outline);
VARIANT_ENUM_CAST(HewnOverlay3D::Blend);
