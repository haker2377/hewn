// A reference image: a picture standing in the scene to model
// after, not a mesh. Its size, its origin offset, whether it is drawn in front of / behind everything, which side
// shows, in orthographic / perspective views or only when looked at straight along its axis, and its opacity.
// Not shown in the game unless asked.
#pragma once

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>

namespace godot {

class HewnImage3D : public Node3D {
	GDCLASS(HewnImage3D, Node3D)

public:
	enum Depth {
		DEPTH_DEFAULT = 0,
		DEPTH_FRONT = 1,
		DEPTH_BACK = 2,
	};
	enum Side {
		SIDE_BOTH = 0,
		SIDE_FRONT = 1,
		SIDE_BACK = 2,
	};

private:
	Ref<Texture2D> texture;
	float size = 5.0f;
	Vector2 offset = Vector2(-0.5f, -0.5f);
	int depth = DEPTH_DEFAULT;
	int side = SIDE_BOTH;
	bool show_orthographic = true;
	bool show_perspective = true;
	bool only_axis_aligned = false;
	bool use_opacity = false;
	float opacity = 1.0f;
	bool show_in_game = false;
	MeshInstance3D *quad = nullptr; // (internal, not saved)
	Ref<ShaderMaterial> material;
	static Ref<Shader> shaders[9];
	void _update();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static void free_static_resources();
	void set_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_texture() const { return texture; }
	void set_size(float p_size);
	float get_size() const { return size; }
	void set_offset(const Vector2 &p_offset);
	Vector2 get_offset() const { return offset; }
	void set_depth(int p_depth);
	int get_depth() const { return depth; }
	void set_side(int p_side);
	int get_side() const { return side; }
	void set_show_orthographic(bool p_on);
	bool get_show_orthographic() const { return show_orthographic; }
	void set_show_perspective(bool p_on);
	bool get_show_perspective() const { return show_perspective; }
	void set_only_axis_aligned(bool p_on);
	bool get_only_axis_aligned() const { return only_axis_aligned; }
	void set_use_opacity(bool p_on);
	bool get_use_opacity() const { return use_opacity; }
	void set_opacity(float p_opacity);
	float get_opacity() const { return opacity; }
	void set_show_in_game(bool p_on);
	bool get_show_in_game() const { return show_in_game; }
	AABB get_rect_aabb() const; // (local) where the picture is
	MeshInstance3D *get_quad() const { return quad; }
};

} // namespace godot

VARIANT_ENUM_CAST(HewnImage3D::Depth);
VARIANT_ENUM_CAST(HewnImage3D::Side);
