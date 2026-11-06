#include "hw_refimage.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

Ref<Shader> HewnImage3D::shaders[9];

void HewnImage3D::free_static_resources() {
	for (Ref<Shader> &s : shaders) {
		s.unref();
	}
}

void HewnImage3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		if (!quad) {
			quad = memnew(MeshInstance3D);
			quad->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
			add_child(quad, false, INTERNAL_MODE_FRONT);
		}
		_update();
	}
}

AABB HewnImage3D::get_rect_aabb() const {
	float w = size, h = size;
	if (texture.is_valid() && texture->get_width() > 0 && texture->get_height() > 0) {
		const float aspect = (float)texture->get_width() / (float)texture->get_height();
		if (aspect >= 1.0f) {
			h = size / aspect;
		} else {
			w = size * aspect;
		}
	}
	return AABB(Vector3(offset.x * w, offset.y * h, 0.0f), Vector3(w, h, 0.0f));
}

// The quad (the bigger side of the picture is `size`; offset -0.5: the origin in its middle) and its material: one
// shader for each depth and side (they are render modes); the views it shows in are checked in the shader.
void HewnImage3D::_update() {
	if (!quad) {
		return;
	}
	const AABB r = get_rect_aabb();
	const Vector3 a = r.position, b = r.position + Vector3(r.size.x, 0, 0), c = r.position + Vector3(r.size.x, r.size.y, 0), d = r.position + Vector3(0, r.size.y, 0);
	PackedVector3Array v = { a, b, c, a, c, d };
	PackedVector2Array uv = { Vector2(0, 1), Vector2(1, 1), Vector2(1, 0), Vector2(0, 1), Vector2(1, 0), Vector2(0, 0) };
	PackedVector3Array nrm = { Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1) };
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = v;
	arrays[Mesh::ARRAY_TEX_UV] = uv;
	arrays[Mesh::ARRAY_NORMAL] = nrm;
	Ref<ArrayMesh> m;
	m.instantiate();
	m->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	quad->set_mesh(m);
	const int variant = CLAMP(depth, 0, 2) * 3 + CLAMP(side, 0, 2);
	if (shaders[variant].is_null()) {
		static const char *cull[3] = { "cull_disabled", "cull_back", "cull_front" };
		String modes = String("unshaded, depth_draw_never, ") + cull[CLAMP(side, 0, 2)];
		if (depth == DEPTH_FRONT) {
			modes += ", depth_test_disabled";
		}
		String code = "shader_type spatial;\nrender_mode " + modes + ";\n";
		code += "uniform sampler2D picture : source_color, filter_linear_mipmap;\nuniform float opacity = 1.0;\n";
		code += "uniform bool show_ortho = true;\nuniform bool show_persp = true;\nuniform bool axis_aligned = false;\n";
		code += "void vertex() {\n\tPOSITION = PROJECTION_MATRIX * (MODELVIEW_MATRIX * vec4(VERTEX, 1.0));\n";
		code += "\tbool ortho = PROJECTION_MATRIX[3][3] > 0.5;\n";
		code += "\tvec3 vn = normalize((MODELVIEW_MATRIX * vec4(0.0, 0.0, 1.0, 0.0)).xyz);\n";
		code += "\tif ((ortho && !show_ortho) || (!ortho && !show_persp) || (axis_aligned && abs(vn.z) < 0.9999)) {\n\t\tPOSITION = vec4(2.0, 2.0, 2.0, 1.0);\n\t}\n";
		if (depth == DEPTH_BACK) {
			code += "\tPOSITION.z = POSITION.w * 0.00001; // (behind everything: the far end of the depth)\n";
		}
		code += "}\nvoid fragment() {\n\tvec4 c = texture(picture, UV);\n\tALBEDO = c.rgb;\n\tALPHA = c.a * opacity;\n}\n";
		shaders[variant].instantiate();
		shaders[variant]->set_code(code);
	}
	if (material.is_null()) {
		material.instantiate();
	}
	material->set_shader(shaders[variant]);
	material->set_render_priority(depth == DEPTH_FRONT ? Material::RENDER_PRIORITY_MAX : depth == DEPTH_BACK ? Material::RENDER_PRIORITY_MIN : 0);
	material->set_shader_parameter("picture", texture);
	material->set_shader_parameter("opacity", use_opacity ? opacity : 1.0f);
	material->set_shader_parameter("show_ortho", show_orthographic);
	material->set_shader_parameter("show_persp", show_perspective);
	material->set_shader_parameter("axis_aligned", only_axis_aligned);
	quad->set_material_override(material);
	quad->set_visible(texture.is_valid() && (show_in_game || Engine::get_singleton()->is_editor_hint()));
	update_gizmos();
}

void HewnImage3D::set_texture(const Ref<Texture2D> &p_texture) {
	texture = p_texture;
	_update();
}
void HewnImage3D::set_size(float p_size) {
	size = MAX(0.001f, p_size);
	_update();
}
void HewnImage3D::set_offset(const Vector2 &p_offset) {
	offset = p_offset;
	_update();
}
void HewnImage3D::set_depth(int p_depth) {
	depth = CLAMP(p_depth, 0, 2);
	_update();
}
void HewnImage3D::set_side(int p_side) {
	side = CLAMP(p_side, 0, 2);
	_update();
}
void HewnImage3D::set_show_orthographic(bool p_on) {
	show_orthographic = p_on;
	_update();
}
void HewnImage3D::set_show_perspective(bool p_on) {
	show_perspective = p_on;
	_update();
}
void HewnImage3D::set_only_axis_aligned(bool p_on) {
	only_axis_aligned = p_on;
	_update();
}
void HewnImage3D::set_use_opacity(bool p_on) {
	use_opacity = p_on;
	_update();
}
void HewnImage3D::set_opacity(float p_opacity) {
	opacity = CLAMP(p_opacity, 0.0f, 1.0f);
	_update();
}
void HewnImage3D::set_show_in_game(bool p_on) {
	show_in_game = p_on;
	_update();
}

void HewnImage3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_texture", "texture"), &HewnImage3D::set_texture);
	ClassDB::bind_method(D_METHOD("get_texture"), &HewnImage3D::get_texture);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &HewnImage3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &HewnImage3D::get_size);
	ClassDB::bind_method(D_METHOD("set_offset", "offset"), &HewnImage3D::set_offset);
	ClassDB::bind_method(D_METHOD("get_offset"), &HewnImage3D::get_offset);
	ClassDB::bind_method(D_METHOD("set_depth", "depth"), &HewnImage3D::set_depth);
	ClassDB::bind_method(D_METHOD("get_depth"), &HewnImage3D::get_depth);
	ClassDB::bind_method(D_METHOD("set_side", "side"), &HewnImage3D::set_side);
	ClassDB::bind_method(D_METHOD("get_side"), &HewnImage3D::get_side);
	ClassDB::bind_method(D_METHOD("set_show_orthographic", "on"), &HewnImage3D::set_show_orthographic);
	ClassDB::bind_method(D_METHOD("get_show_orthographic"), &HewnImage3D::get_show_orthographic);
	ClassDB::bind_method(D_METHOD("set_show_perspective", "on"), &HewnImage3D::set_show_perspective);
	ClassDB::bind_method(D_METHOD("get_show_perspective"), &HewnImage3D::get_show_perspective);
	ClassDB::bind_method(D_METHOD("set_only_axis_aligned", "on"), &HewnImage3D::set_only_axis_aligned);
	ClassDB::bind_method(D_METHOD("get_only_axis_aligned"), &HewnImage3D::get_only_axis_aligned);
	ClassDB::bind_method(D_METHOD("set_use_opacity", "on"), &HewnImage3D::set_use_opacity);
	ClassDB::bind_method(D_METHOD("get_use_opacity"), &HewnImage3D::get_use_opacity);
	ClassDB::bind_method(D_METHOD("set_opacity", "opacity"), &HewnImage3D::set_opacity);
	ClassDB::bind_method(D_METHOD("get_opacity"), &HewnImage3D::get_opacity);
	ClassDB::bind_method(D_METHOD("set_show_in_game", "on"), &HewnImage3D::set_show_in_game);
	ClassDB::bind_method(D_METHOD("get_show_in_game"), &HewnImage3D::get_show_in_game);
	ClassDB::bind_method(D_METHOD("get_rect_aabb"), &HewnImage3D::get_rect_aabb);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_texture", "get_texture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "size", PROPERTY_HINT_RANGE, "0.001,1000,0.01,or_greater,suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "offset"), "set_offset", "get_offset");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "depth", PROPERTY_HINT_ENUM, "Default,Front,Back"), "set_depth", "get_depth");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "side", PROPERTY_HINT_ENUM, "Both,Front,Back"), "set_side", "get_side");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_orthographic"), "set_show_orthographic", "get_show_orthographic");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_perspective"), "set_show_perspective", "get_show_perspective");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "only_axis_aligned"), "set_only_axis_aligned", "get_only_axis_aligned");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_opacity"), "set_use_opacity", "get_use_opacity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "opacity", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_opacity", "get_opacity");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_in_game"), "set_show_in_game", "get_show_in_game");
	BIND_ENUM_CONSTANT(DEPTH_DEFAULT);
	BIND_ENUM_CONSTANT(DEPTH_FRONT);
	BIND_ENUM_CONSTANT(DEPTH_BACK);
	BIND_ENUM_CONSTANT(SIDE_BOTH);
	BIND_ENUM_CONSTANT(SIDE_FRONT);
	BIND_ENUM_CONSTANT(SIDE_BACK);
}
