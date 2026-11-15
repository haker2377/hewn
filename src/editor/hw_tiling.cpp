// Hiding the repeats of a texture: a shader material made from a standard one (its texture, color, roughness,
// metallic, normal map, UV scale, filtering, alpha), sampling the image in a grid of hexagons, each moved (and
// turned) at random and blended into the next (no straight rows of the same picture), and / or darkened and
// lightened by a big soft noise. It keeps its source: the Material tab edits the standard material, this one follows.
#include "hw_editor.h"
#include "../hw_noise.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>

using namespace godot;

namespace {

enum {
	TF_HEX = 1,
	TF_ROT = 2,
	TF_MACRO = 4,
	// (from the source)
	TF_NEAREST = 8,
	TF_ALPHA = 16,
	TF_VCOLOR = 32,
	TF_NORMAL = 64,
	TF_CULL_OFF = 128,
	TF_EMISSION = 256,
};

int source_flags(const Ref<BaseMaterial3D> &p_src) {
	int f = 0;
	const BaseMaterial3D::TextureFilter tf = p_src->get_texture_filter();
	if (tf == BaseMaterial3D::TEXTURE_FILTER_NEAREST || tf == BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS || tf == BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC) {
		f |= TF_NEAREST;
	}
	f |= p_src->get_transparency() != BaseMaterial3D::TRANSPARENCY_DISABLED ? TF_ALPHA : 0;
	f |= p_src->get_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR) ? TF_VCOLOR : 0;
	f |= p_src->get_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING) && p_src->get_texture(BaseMaterial3D::TEXTURE_NORMAL).is_valid() ? TF_NORMAL : 0;
	f |= p_src->get_cull_mode() == BaseMaterial3D::CULL_DISABLED ? TF_CULL_OFF : 0;
	f |= p_src->get_feature(BaseMaterial3D::FEATURE_EMISSION) ? TF_EMISSION : 0;
	return f;
}

String shader_code(int f) {
	const bool hex = f & (TF_HEX | TF_ROT), rot = f & TF_ROT, macro = f & TF_MACRO;
	const String filt = (f & TF_NEAREST) ? "filter_nearest_mipmap" : "filter_linear_mipmap_anisotropic";
	String c = "// Made by Hewn (hiding the repeats of a texture): regenerated from its settings.\nshader_type spatial;\n";
	c += String("render_mode blend_mix, depth_draw_opaque, ") + ((f & TF_CULL_OFF) ? "cull_disabled" : "cull_back") + ", diffuse_burley, specular_schlick_ggx;\n\n";
	c += "uniform vec4 albedo : source_color = vec4(1.0);\n";
	c += "uniform sampler2D texture_albedo : source_color, " + filt + ", repeat_enable;\n";
	c += "uniform float roughness : hint_range(0.0, 1.0) = 1.0;\n";
	c += "uniform sampler2D texture_roughness : hint_roughness_r, " + filt + ", repeat_enable;\n";
	c += "uniform vec4 roughness_texture_channel = vec4(1.0, 0.0, 0.0, 0.0);\n";
	c += "uniform float metallic : hint_range(0.0, 1.0) = 0.0;\n";
	c += "uniform sampler2D texture_metallic : hint_default_white, " + filt + ", repeat_enable;\n";
	c += "uniform vec4 metallic_texture_channel = vec4(1.0, 0.0, 0.0, 0.0);\n";
	c += "uniform float specular : hint_range(0.0, 1.0) = 0.5;\n";
	if (f & TF_NORMAL) {
		c += "uniform sampler2D texture_normal : hint_roughness_normal, " + filt + ", repeat_enable;\n";
		c += "uniform float normal_scale : hint_range(-16.0, 16.0) = 1.0;\n";
	}
	if (f & TF_EMISSION) {
		c += "uniform vec4 emission : source_color = vec4(0.0, 0.0, 0.0, 1.0);\n";
		c += "uniform float emission_energy : hint_range(0.0, 100.0) = 1.0;\n";
	}
	if (f & TF_ALPHA) {
		c += "uniform float alpha_scissor_threshold : hint_range(0.0, 1.0) = 0.5;\n";
	}
	c += "uniform vec3 uv1_scale = vec3(1.0);\nuniform vec3 uv1_offset = vec3(0.0);\n";
	if (hex) {
		c += "// the hexagons: how big (in textures), how hard their borders\n";
		c += "uniform float tile_size : hint_range(0.05, 8.0) = 1.0;\n";
		c += "uniform float tile_blend : hint_range(0.0, 1.0) = 0.6;\n";
		if (rot) {
			c += "uniform float tile_rotation : hint_range(0.0, 180.0) = 180.0;\n";
		}
	}
	if (macro) {
		c += "// the big soft noise darkening and lightening the surface\n";
		c += "uniform sampler2D variation_noise : filter_linear_mipmap, repeat_enable;\n";
		c += "uniform float variation_strength : hint_range(0.0, 1.0) = 0.25;\n";
		c += "uniform float variation_size : hint_range(0.1, 100.0) = 6.0;\n";
	}
	c += "\n";
	if (hex) {
		c += R"(vec2 hw_hash(vec2 p) {
	p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
	return fract(sin(p) * 43758.5453);
}

// the three hexagon centers around a point and how much of each it takes
void hw_hex(vec2 uv, out vec2 v1, out vec2 v2, out vec2 v3, out vec3 w) {
	vec2 st = uv * 3.4641016 / tile_size;
	vec2 sk = mat2(vec2(1.0, -0.57735027), vec2(0.0, 1.15470054)) * st;
	vec2 base = floor(sk);
	vec3 t = vec3(fract(sk), 0.0);
	t.z = 1.0 - t.x - t.y;
	float s = step(0.0, -t.z);
	float s2 = 2.0 * s - 1.0;
	w = vec3(-t.z * s2, s - t.y * s2, s - t.x * s2);
	v1 = base + vec2(s, s);
	v2 = base + vec2(s, 1.0 - s);
	v3 = base + vec2(1.0 - s, s);
	w = pow(max(w, vec3(0.0)), vec3(1.0 + tile_blend * 7.0));
	w /= max(1e-5, w.x + w.y + w.z);
}

mat2 hw_turn(vec2 cell) {
)";
		if (rot) {
			c += "\tfloat a = (hw_hash(cell + 17.0).x - 0.5) * 2.0 * radians(tile_rotation);\n\treturn mat2(vec2(cos(a), sin(a)), vec2(-sin(a), cos(a)));\n";
		} else {
			c += "\treturn mat2(1.0);\n";
		}
		c += R"(}

vec4 hw_sample(sampler2D tex, vec2 uv, vec2 dx, vec2 dy) {
	vec2 v1, v2, v3;
	vec3 w;
	hw_hex(uv, v1, v2, v3, w);
	mat2 r1 = hw_turn(v1), r2 = hw_turn(v2), r3 = hw_turn(v3);
	vec4 a = textureGrad(tex, r1 * uv + hw_hash(v1), r1 * dx, r1 * dy);
	vec4 b = textureGrad(tex, r2 * uv + hw_hash(v2), r2 * dx, r2 * dy);
	vec4 c = textureGrad(tex, r3 * uv + hw_hash(v3), r3 * dx, r3 * dy);
	return a * w.x + b * w.y + c * w.z;
}
)";
		if (f & TF_NORMAL) {
			c += R"(
// (a turned sample's normal turned back: its slopes as they lie on the surface)
vec3 hw_normal(sampler2D tex, vec2 uv, vec2 dx, vec2 dy) {
	vec2 v1, v2, v3;
	vec3 w;
	hw_hex(uv, v1, v2, v3, w);
	mat2 r1 = hw_turn(v1), r2 = hw_turn(v2), r3 = hw_turn(v3);
	vec3 a = textureGrad(tex, r1 * uv + hw_hash(v1), r1 * dx, r1 * dy).rgb * 2.0 - 1.0;
	vec3 b = textureGrad(tex, r2 * uv + hw_hash(v2), r2 * dx, r2 * dy).rgb * 2.0 - 1.0;
	vec3 c = textureGrad(tex, r3 * uv + hw_hash(v3), r3 * dx, r3 * dy).rgb * 2.0 - 1.0;
	a.xy = transpose(r1) * a.xy;
	b.xy = transpose(r2) * b.xy;
	c.xy = transpose(r3) * c.xy;
	return normalize(a * w.x + b * w.y + c * w.z) * 0.5 + 0.5;
}
)";
		}
	}
	c += "\nvoid fragment() {\n\tvec2 base_uv = UV * uv1_scale.xy + uv1_offset.xy;\n";
	if (hex) {
		c += "\tvec2 dx = dFdx(base_uv), dy = dFdy(base_uv);\n";
		c += "\tvec4 albedo_tex = hw_sample(texture_albedo, base_uv, dx, dy);\n";
		c += "\tfloat rough_tex = dot(hw_sample(texture_roughness, base_uv, dx, dy), roughness_texture_channel);\n";
		c += "\tfloat metal_tex = dot(hw_sample(texture_metallic, base_uv, dx, dy), metallic_texture_channel);\n";
	} else {
		c += "\tvec4 albedo_tex = texture(texture_albedo, base_uv);\n";
		c += "\tfloat rough_tex = dot(texture(texture_roughness, base_uv), roughness_texture_channel);\n";
		c += "\tfloat metal_tex = dot(texture(texture_metallic, base_uv), metallic_texture_channel);\n";
	}
	c += "\tALBEDO = albedo.rgb * albedo_tex.rgb;\n";
	if (f & TF_VCOLOR) {
		c += "\tALBEDO *= COLOR.rgb;\n";
	}
	if (macro) {
		c += "\tfloat vn = texture(variation_noise, base_uv / variation_size).r;\n";
		c += "\tALBEDO *= mix(1.0 - variation_strength, 1.0 + variation_strength, vn);\n";
	}
	c += "\tROUGHNESS = rough_tex * roughness;\n\tMETALLIC = metal_tex * metallic;\n\tSPECULAR = specular;\n";
	if (f & TF_NORMAL) {
		c += hex ? "\tNORMAL_MAP = hw_normal(texture_normal, base_uv, dx, dy);\n" : "\tNORMAL_MAP = texture(texture_normal, base_uv).rgb;\n";
		c += "\tNORMAL_MAP_DEPTH = normal_scale;\n";
	}
	if (f & TF_EMISSION) {
		c += "\tEMISSION = emission.rgb * emission_energy;\n";
	}
	if (f & TF_ALPHA) {
		c += "\tALPHA = albedo.a * albedo_tex.a;\n\tALPHA_SCISSOR_THRESHOLD = alpha_scissor_threshold;\n";
	}
	c += "}\n";
	return c;
}

Ref<Shader> shader_for(int f) {
	const String dir = "res://materials";
	const String path = dir + vformat("/hewn_tiling_%d.gdshader", f);
	const String code = shader_code(f);
	if (!DirAccess::dir_exists_absolute(dir)) {
		DirAccess::make_dir_recursive_absolute(dir);
	}
	// (written again when the code here changed; one file per kind, shared by every such material)
	Ref<Shader> sh = FileAccess::file_exists(path) ? Ref<Shader>(ResourceLoader::get_singleton()->load(path)) : Ref<Shader>();
	if (sh.is_null() || sh->get_code() != code) {
		Ref<FileAccess> fa = FileAccess::open(path, FileAccess::WRITE);
		if (fa.is_valid()) {
			fa->store_string(code);
			fa->close();
		}
		if (sh.is_null()) {
			sh.instantiate();
			sh->set_path(path);
		}
		sh->set_code(code);
	}
	return sh;
}

} // namespace

namespace godot {

int hw_tiling_flags(const Ref<Material> &p_mat) {
	return p_mat.is_valid() && p_mat->has_meta("hewn_tiling") ? (int)p_mat->get_meta("hewn_tiling") : 0;
}

Ref<BaseMaterial3D> hw_tiling_source(const Ref<Material> &p_mat) {
	return p_mat.is_valid() && p_mat->has_meta("hewn_tiling_source") ? Ref<BaseMaterial3D>(p_mat->get_meta("hewn_tiling_source")) : Ref<BaseMaterial3D>();
}

// The shader material's shader and settings from its source, with the user's choices (hexagons, turns, noise).
void hw_tiling_sync(const Ref<ShaderMaterial> &p_sm, const Ref<BaseMaterial3D> &p_src, int p_user) {
	if (p_sm.is_null() || p_src.is_null()) {
		return;
	}
	const int f = (p_user & (TF_HEX | TF_ROT | TF_MACRO)) | source_flags(p_src);
	const Ref<Shader> sh = shader_for(f);
	if (p_sm->get_shader() != sh) {
		p_sm->set_shader(sh);
	}
	p_sm->set_meta("hewn_tiling", p_user & (TF_HEX | TF_ROT | TF_MACRO));
	p_sm->set_meta("hewn_tiling_source", p_src);
	p_sm->set_meta("hw_image", p_src->has_meta("hw_image") ? p_src->get_meta("hw_image") : Variant());
	p_sm->set_shader_parameter("albedo", p_src->get_albedo());
	p_sm->set_shader_parameter("texture_albedo", p_src->get_texture(BaseMaterial3D::TEXTURE_ALBEDO));
	p_sm->set_shader_parameter("roughness", p_src->get_roughness());
	p_sm->set_shader_parameter("texture_roughness", p_src->get_texture(BaseMaterial3D::TEXTURE_ROUGHNESS));
	static const Vector4 chans[5] = { Vector4(1, 0, 0, 0), Vector4(0, 1, 0, 0), Vector4(0, 0, 1, 0), Vector4(0, 0, 0, 1), Vector4(0.333333f, 0.333333f, 0.333333f, 0) };
	p_sm->set_shader_parameter("roughness_texture_channel", chans[CLAMP((int)p_src->get_roughness_texture_channel(), 0, 4)]);
	p_sm->set_shader_parameter("metallic", p_src->get_metallic());
	p_sm->set_shader_parameter("texture_metallic", p_src->get_texture(BaseMaterial3D::TEXTURE_METALLIC));
	p_sm->set_shader_parameter("metallic_texture_channel", chans[CLAMP((int)p_src->get_metallic_texture_channel(), 0, 4)]);
	p_sm->set_shader_parameter("specular", p_src->get_specular());
	p_sm->set_shader_parameter("texture_normal", p_src->get_texture(BaseMaterial3D::TEXTURE_NORMAL));
	p_sm->set_shader_parameter("normal_scale", p_src->get_normal_scale());
	p_sm->set_shader_parameter("emission", p_src->get_emission());
	p_sm->set_shader_parameter("emission_energy", p_src->get_emission_energy_multiplier());
	p_sm->set_shader_parameter("alpha_scissor_threshold", p_src->get_alpha_scissor_threshold());
	p_sm->set_shader_parameter("uv1_scale", p_src->get_uv1_scale());
	p_sm->set_shader_parameter("uv1_offset", p_src->get_uv1_offset());
	if ((p_user & TF_MACRO) && Variant(p_sm->get_shader_parameter("variation_noise")).get_type() != Variant::OBJECT) {
		p_sm->set_shader_parameter("variation_noise", hewn::make_noise_texture());
	}
}

} // namespace godot
