#include "hewn_mesh.h"

#include <cstring>
#include <godot_cpp/classes/mesh_convex_decomposition_settings.hpp>
#include <godot_cpp/classes/capsule_shape3d.hpp>
#include <godot_cpp/classes/cylinder_shape3d.hpp>
#include <godot_cpp/classes/sphere_shape3d.hpp>
#include <godot_cpp/classes/box_shape3d.hpp>
#include <unordered_set>
#include <map>

#include <godot_cpp/classes/resource_loader.hpp>

#include "hw_cutter.h"

#include <godot_cpp/classes/base_material3d.hpp>

#include <algorithm>
#include <unordered_map>

#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/concave_polygon_shape3d.hpp>
#include <godot_cpp/classes/convex_polygon_shape3d.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/path3d.hpp>
#include <godot_cpp/classes/curve3d.hpp>
#include <godot_cpp/classes/gltf_document.hpp>
#include <godot_cpp/classes/gltf_state.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

Ref<StandardMaterial3D> HewnMesh3D::default_material;
std::vector<HewnMesh3D *> HewnMesh3D::all_meshes;

namespace {

std::vector<hewn::Edge> to_edges(const PackedInt32Array &p) {
	std::vector<hewn::Edge> out;
	for (int64_t i = 0; i + 1 < p.size(); i += 2) {
		out.push_back(hewn::Edge(p[i], p[i + 1]));
	}
	return out;
}

std::vector<int> to_vec(const PackedInt32Array &p) {
	std::vector<int> out(p.size());
	for (int64_t i = 0; i < p.size(); i++) {
		out[i] = p[i];
	}
	return out;
}

PackedInt32Array to_packed(const std::vector<int> &v) {
	PackedInt32Array out;
	out.resize(v.size());
	for (size_t i = 0; i < v.size(); i++) {
		out.set(i, v[i]);
	}
	return out;
}

PackedInt32Array edges_to_packed(const std::vector<hewn::Edge> &v) {
	PackedInt32Array out;
	for (const hewn::Edge &e : v) {
		out.push_back(e.a);
		out.push_back(e.b);
	}
	return out;
}

const char *SHAPE_ENUM = "Custom,Cube,Plane,Cylinder,Cone,Sphere,Icosphere,Torus,Pipe,Prism,Stairs,Curved Stairs,Arch,Door,Capsule,Disc,Wedge,Polygon,Poly Shape,Wall,Sweep,Column,Path Stairs";

} // namespace

// ------------------------------------------------------------------ static resources

Ref<Material> HewnMesh3D::get_default_material() {
	if (default_material.is_null()) {
		const int S = 64;
		Ref<Image> img = Image::create_empty(S, S, false, Image::FORMAT_RGBA8);
		for (int y = 0; y < S; y++) {
			for (int x = 0; x < S; x++) {
				const bool cell = ((x / (S / 2)) + (y / (S / 2))) % 2 == 0;
				const bool line = (x % (S / 2)) == 0 || (y % (S / 2)) == 0;
				float v = cell ? 0.62f : 0.52f;
				if (line) {
					v = 0.40f;
				}
				img->set_pixel(x, y, Color(v, v, v, 1));
			}
		}
		img->generate_mipmaps();
		Ref<ImageTexture> tex = ImageTexture::create_from_image(img);
		default_material.instantiate();
		default_material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, tex);
		default_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
		default_material->set_texture_filter(BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC);
		default_material->set_roughness(0.85f);
	}
	return default_material;
}

void HewnMesh3D::free_static_resources() {
	default_material.unref();
}

// ------------------------------------------------------------------ lifecycle

HewnMesh3D::HewnMesh3D() {
	_regenerate_shape();
}

HewnMesh3D::~HewnMesh3D() {
	all_meshes.erase(std::remove(all_meshes.begin(), all_meshes.end(), this), all_meshes.end());
}

void HewnMesh3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (std::find(all_meshes.begin(), all_meshes.end(), this) == all_meshes.end()) {
				all_meshes.push_back(this);
			}
			if (!uv_anchored) {
				uv_last_xf = get_global_transform();
			}
			uv_enter_frame = Engine::get_singleton()->get_process_frames();
			if (!HewnCutter3D::all.empty()) {
				set_notify_transform(true);
				_cuts_changed(); // (cutters already there)
			}
			if (get_mesh().is_null()) {
				rebuild();
			} else if (has_world_uv) {
				// (only the world projected UVs follow where it is: the mesh itself did not change, its revision stays)
				const uint64_t r = revision;
				rebuild();
				revision = r;
			}
			_update_collision();
			_apply_cutter_display();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			all_meshes.erase(std::remove(all_meshes.begin(), all_meshes.end(), this), all_meshes.end());
		} break;
		case NOTIFICATION_READY: {
			if (has_modifiers()) {
				_on_operand_changed(); // the operands / paths (later siblings too) exist now
			}
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			if (has_world_uv && !uv_anchored) {
				if (Engine::get_singleton()->get_process_frames() == uv_enter_frame) {
					uv_last_xf = get_global_transform(); // (just made and put in its place)
				} else {
					uv_anchor = uv_last_xf; // (moved: the world UVs stay as they were)
					uv_anchored = true;
				}
			}
			if (has_world_uv) {
				const uint64_t r = revision; // (see ENTER_TREE)
				rebuild();
				revision = r;
			} else if (!bool_watched.empty()) {
				_on_operand_changed(); // the operands moved relative to us
			}
			if (!HewnCutter3D::all.empty() || !cut_signature.empty()) {
				_cuts_changed(); // (moved into / out of a cutter)
			}
			emit_signal("geometry_moved");
		} break;
		case NOTIFICATION_CHILD_ORDER_CHANGED: {
			if (csg_model && is_inside_tree()) {
				bool_force = true; // brushes added, removed or reordered
				_on_operand_changed();
			}
		} break;
		case NOTIFICATION_PROCESS: {
			if (path_watched.empty()) {
				break;
			}
			const Transform3D inv = get_global_transform().affine_inverse();
			for (const std::pair<ObjectID, Transform3D> &w : path_watched) {
				Node3D *p = Object::cast_to<Node3D>(ObjectDB::get_instance(w.first));
				if (!p || !p->is_inside_tree() || !(inv * p->get_global_transform()).is_equal_approx(w.second)) {
					_on_operand_changed(); // a path moved (or went away)
					break;
				}
			}
		} break;
		default:
			break;
	}
}

// ------------------------------------------------------------------ shape

std::vector<float> HewnMesh3D::_shape_values() const {
	const std::vector<hewn::shapes::Param> &ps = hewn::shapes::params((hewn::shapes::Type)shape);
	std::vector<float> v;
	for (const hewn::shapes::Param &p : ps) {
		v.push_back(get_shape_param(p.name));
	}
	return v;
}

// Stairs with a cutter (tsitski): over every tread a prism as high as the headroom, so a floor above the
// stairs gets its opening (and moves with them).
void HewnMesh3D::_update_stairs_cutter() {
	const bool want = (shape == hewn::shapes::STAIRS || shape == hewn::shapes::PATH_STAIRS) && get_shape_param("generate_cutter") > 0.5f;
	HewnCutter3D *c = Object::cast_to<HewnCutter3D>(stairs_cutter);
	if (!want) {
		if (c) {
			remove_child(c);
			memdelete(c);
			stairs_cutter = nullptr;
		}
		return;
	}
	if (!c) {
		c = memnew(HewnCutter3D);
		c->set_name("_StairsCutter");
		add_child(c, false, INTERNAL_MODE_BACK);
		stairs_cutter = c;
	}
	const float head = get_shape_param("cutter_headroom");
	std::vector<std::vector<hewn::ops::CutPlane>> parts;
	for (const hewn::Face &F : data.faces) {
		if (F.role != hewn::ROLE_TREAD || F.size() < 3) {
			continue;
		}
		std::vector<Vector2> poly;
		float y = -1e30f;
		for (int v : F.v) {
			poly.push_back(Vector2(data.pos[v].x, data.pos[v].z));
			y = MAX(y, data.pos[v].y);
		}
		float area = 0.0f;
		for (size_t i = 0; i < poly.size(); i++) {
			area += poly[i].cross(poly[(i + 1) % poly.size()]);
		}
		if (std::fabs(area) < 1e-8f) {
			continue;
		}
		if (area < 0.0f) {
			std::reverse(poly.begin(), poly.end());
		}
		std::vector<hewn::ops::CutPlane> planes;
		for (size_t i = 0; i < poly.size(); i++) {
			const Vector2 e = poly[(i + 1) % poly.size()] - poly[i];
			if (e.length() < 1e-7f) {
				continue;
			}
			const Vector3 n = Vector3(e.y, 0.0f, -e.x).normalized();
			planes.push_back({ n, n.dot(Vector3(poly[i].x, 0.0f, poly[i].y)), false });
		}
		planes.push_back({ Vector3(0, 1, 0), y + head, false });
		planes.push_back({ Vector3(0, -1, 0), -(y + 0.01f), false }); // (from just above the tread)
		parts.push_back(planes);
	}
	c->set_custom_parts(parts);
}

void HewnMesh3D::_regenerate_shape() {
	if (shape == hewn::shapes::CUSTOM) {
		return;
	}
	// The faces are rebuilt: each role keeps the material and UV settings its faces had (the most used
	// material of the role); new roles get the most used material of the mesh.
	std::vector<std::unordered_map<int, int>> role_count(hewn::ROLE_COUNT);
	std::vector<hewn::UVParams> role_uvp(hewn::ROLE_COUNT);
	std::unordered_map<int, int> all_count;
	for (const hewn::Face &f : data.faces) {
		all_count[f.material]++;
		if (f.role > 0 && f.role < hewn::ROLE_COUNT) {
			if (role_count[f.role].empty()) {
				role_uvp[f.role] = f.uvp;
			}
			role_count[f.role][f.material]++;
		}
	}
	auto most = [](const std::unordered_map<int, int> &p_count) {
		int best = -1, n = 0;
		for (const std::pair<const int, int> &kv : p_count) {
			if (kv.second > n || (kv.second == n && kv.first < best)) {
				best = kv.first;
				n = kv.second;
			}
		}
		return best;
	};
	const int common = most(all_count);
	_build_shape();
	_update_stairs_cutter();
	if (common < 0) {
		selection.clear();
		center_auto_textures();
		return;
	}
	for (hewn::Face &f : data.faces) {
		const bool known = f.role > 0 && f.role < hewn::ROLE_COUNT && !role_count[f.role].empty();
		f.material = known ? most(role_count[f.role]) : common;
		if (known) {
			f.uvp = role_uvp[f.role];
		}
	}
	selection.clear();
	center_auto_textures();
}

// A parametric shape's tiled textures: the middle of the texture on the middle of the faces (Center), the faces
// of one plane together (a staircase's treads: one texture running over them). Only the offset changes, by
// less than a tile; faces with manual or world UVs, stretched / fitted ones are left alone.
void HewnMesh3D::set_world_uv_all(float p_scale) {
	const float sc = p_scale > 1e-6f ? p_scale : 1.0f;
	for (hewn::Face &F : data.faces) {
		if (F.manual_uv) {
			continue;
		}
		F.uvp.world = true;
		F.uvp.fill = hewn::UV_FILL_TILE;
		F.uvp.scale = Vector2(sc, sc);
		F.uvp.offset = Vector2();
		F.uvp.rotation = 0.0f;
	}
	rebuild();
}

void HewnMesh3D::center_auto_textures() {
	if (shape == hewn::shapes::CUSTOM || data.faces.empty()) {
		return;
	}
	std::vector<std::vector<Vector2>> uvs;
	hewn::compute_uvs(data, Transform3D(), uvs);
	struct G {
		Vector2 mn = Vector2(1e30f, 1e30f), mx = Vector2(-1e30f, -1e30f);
		Vector2 ms = Vector2(1, 1);
		std::vector<int> faces;
	};
	std::map<std::vector<int64_t>, G> groups;
	for (int f = 0; f < (int)data.faces.size(); f++) {
		const hewn::Face &F = data.faces[f];
		if (F.manual_uv || F.uvp.world || F.uvp.fill != hewn::UV_FILL_TILE || F.size() < 3 || f >= (int)uvs.size()) {
			continue;
		}
		const Vector3 n = hewn::face_normal(data, F).normalized();
		const hewn::UVParams &P = F.uvp;
		const std::vector<int64_t> key = { F.material, F.uv_group, (int64_t)std::llround(n.x * 1000.0f), (int64_t)std::llround(n.y * 1000.0f),
			(int64_t)std::llround(n.z * 1000.0f), (int64_t)std::llround(P.rotation * 100.0f), (int64_t)std::llround(P.scale.x * 10000.0f),
			(int64_t)std::llround(P.scale.y * 10000.0f), (int64_t)std::llround(P.offset.x * 10000.0f), (int64_t)std::llround(P.offset.y * 10000.0f),
			P.flip_u ? 1 : 0, P.flip_v ? 1 : 0, P.swap ? 1 : 0 };
		G &g = groups[key];
		for (const Vector2 &q : uvs[f]) {
			g.mn = g.mn.min(q);
			g.mx = g.mx.max(q);
		}
		Ref<BaseMaterial3D> bm = get_material_for_slot(F.material);
		if (bm.is_valid()) {
			g.ms = Vector2(MAX(1e-6f, bm->get_uv1_scale().x), MAX(1e-6f, bm->get_uv1_scale().y));
		}
		g.faces.push_back(f);
	}
	for (const auto &kv : groups) {
		const G &g = kv.second;
		const Vector2 c = (g.mn + g.mx) * 0.5f * g.ms; // (in tiles)
		const Vector2 frac(c.x - std::floor(c.x), c.y - std::floor(c.y));
		const Vector2 d((0.5f - frac.x) / g.ms.x, (0.5f - frac.y) / g.ms.y);
		for (int f : g.faces) {
			data.faces[f].uvp.offset += d;
		}
	}
}

void HewnMesh3D::_build_shape() {
	if (shape == hewn::shapes::POLY_SHAPE) {
		std::vector<Vector3> pts;
		for (int64_t i = 0; i < poly_points.size(); i++) {
			pts.push_back(poly_points[i]);
		}
		const std::vector<float> v = _shape_values();
		hewn::shapes::build_poly(pts, v[0], v[1] > 0.5f, data);
	} else if (shape == hewn::shapes::WALL && poly_points.size() >= 2) {
		// a wall through its points (drawn with corners, or edited); without points: the plain `length` wall
		std::vector<Vector3> pts;
		for (int64_t i = 0; i < poly_points.size(); i++) {
			pts.push_back(poly_points[i]);
		}
		const std::vector<float> v = _shape_values();
		hewn::shapes::build_wall(pts, v[1], v[2], v[5] > 0.5f, data, (int)v[6]);
	} else if (shape == hewn::shapes::SWEEP && poly_points.size() >= 2) {
		std::vector<Vector3> pts;
		for (int64_t i = 0; i < poly_points.size(); i++) {
			pts.push_back(poly_points[i]);
		}
		std::vector<Vector2> custom = sweep_profile.is_valid() ? sweep_profile->to_vector() : std::vector<Vector2>();
		hewn::shapes::build_sweep(pts, _shape_values(), data, &custom);
	} else if (shape == hewn::shapes::PATH_STAIRS && poly_points.size() >= 2) {
		std::vector<Vector3> pts;
		for (int64_t i = 0; i < poly_points.size(); i++) {
			pts.push_back(poly_points[i]);
		}
		hewn::shapes::build_path_stairs(pts, _shape_values(), data); // (flights and landings: closed pieces of their own)
	} else if (shape == hewn::shapes::COLUMN) {
		std::vector<float> v = _shape_values();
		const bool flip = v.back() > 0.5f;
		hewn::shapes::build_column(v, base_profile.is_valid() ? base_profile->to_vector() : std::vector<Vector2>(),
				shaft_profile.is_valid() ? shaft_profile->to_vector() : std::vector<Vector2>(), top_profile.is_valid() ? top_profile->to_vector() : std::vector<Vector2>(), data);
		// (centered like the other shapes: on its bounding box)
		Vector3 mn, mx;
		for (size_t i = 0; i < data.pos.size(); i++) {
			mn = i ? mn.min(data.pos[i]) : data.pos[i];
			mx = i ? mx.max(data.pos[i]) : data.pos[i];
		}
		const Vector3 c = (mn + mx) * 0.5f;
		for (Vector3 &p : data.pos) {
			p -= c;
		}
		if (flip) {
			for (hewn::Face &f : data.faces) {
				f.reverse();
			}
		}
		hewn::weld_by_distance(data, 1e-5f);
	} else if (shape_profile.is_valid() && (shape == hewn::shapes::DOOR || shape == hewn::shapes::ARCH || shape == hewn::shapes::WEDGE)) {
		hewn::shapes::build_with_profile((hewn::shapes::Type)shape, _shape_values(), shape_profile->to_vector(), data);
	} else {
		hewn::shapes::build((hewn::shapes::Type)shape, _shape_values(), data);
	}
}

void HewnMesh3D::set_shape(int p_shape) {
	p_shape = CLAMP(p_shape, 0, (int)hewn::shapes::TYPE_MAX - 1);
	if (p_shape == shape) {
		return;
	}
	shape = p_shape;
	if (shape != hewn::shapes::CUSTOM) {
		_regenerate_shape();
		rebuild();
	}
	notify_property_list_changed();
}

int HewnMesh3D::get_shape() const {
	return shape;
}

void HewnMesh3D::set_shape_params(const Dictionary &p_params) {
	shape_params = p_params.duplicate();
	if (shape != hewn::shapes::CUSTOM) {
		_regenerate_shape();
		rebuild();
	}
}

Dictionary HewnMesh3D::get_shape_params() const {
	return shape_params;
}

void HewnMesh3D::_on_profile_changed() {
	if (shape == hewn::shapes::COLUMN || shape == hewn::shapes::SWEEP || shape == hewn::shapes::DOOR || shape == hewn::shapes::ARCH || shape == hewn::shapes::WEDGE) {
		_regenerate_shape();
		rebuild();
	}
}

#define HW_PROFILE_SETTER(m_name)                                                          \
	void HewnMesh3D::set_##m_name(const Ref<HewnProfile> &p_profile) {                  \
		const Callable c = callable_mp(this, &HewnMesh3D::_on_profile_changed);       \
		if (m_name.is_valid() && m_name->is_connected("changed", c)) {                     \
			m_name->disconnect("changed", c);                                              \
		}                                                                                  \
		m_name = p_profile;                                                                \
		if (m_name.is_valid()) {                                                           \
			m_name->connect("changed", c);                                                 \
		}                                                                                  \
		_on_profile_changed();                                                             \
	}
HW_PROFILE_SETTER(base_profile)
HW_PROFILE_SETTER(shaft_profile)
HW_PROFILE_SETTER(top_profile)
HW_PROFILE_SETTER(sweep_profile)
HW_PROFILE_SETTER(shape_profile)

void HewnMesh3D::make_profiles_editable() {
	if (shape == hewn::shapes::COLUMN) {
		if (base_profile.is_null()) {
			set_base_profile(HewnProfile::make_preset("base", (int)get_shape_param("base_style")));
		}
		if (shaft_profile.is_null()) {
			set_shaft_profile(HewnProfile::make_preset("shaft", (int)get_shape_param("shaft_style")));
		}
		if (top_profile.is_null()) {
			set_top_profile(HewnProfile::make_preset("top", (int)get_shape_param("top_style")));
		}
	} else if (shape == hewn::shapes::SWEEP && sweep_profile.is_null()) {
		Ref<HewnProfile> p;
		p.instantiate();
		p->set_points(PackedVector2Array({ Vector2(0, 0), Vector2(1, 0), Vector2(1, 0.6f), Vector2(0.6f, 1), Vector2(0, 1) }));
		set_sweep_profile(p);
		set_shape_param("profile", 6);
	}
	notify_property_list_changed();
}

void HewnMesh3D::set_poly_points(const PackedVector3Array &p_points) {
	poly_points = p_points;
	if (shape == hewn::shapes::POLY_SHAPE || shape == hewn::shapes::WALL || shape == hewn::shapes::SWEEP || shape == hewn::shapes::PATH_STAIRS) {
		_regenerate_shape();
		rebuild();
	}
}

PackedVector3Array HewnMesh3D::get_poly_points() const {
	return poly_points;
}

void HewnMesh3D::set_shape_param(const String &p_name, float p_value) {
	shape_params[p_name] = p_value;
	if (shape != hewn::shapes::CUSTOM) {
		_regenerate_shape();
		rebuild();
		for (const hewn::shapes::Param &p : hewn::shapes::params((hewn::shapes::Type)shape)) {
			if (p_name == p.name && (p.is_int && (p.options || p.is_toggle()))) {
				notify_property_list_changed(); // (other parameters may show / hide)
			}
		}
	}
}

float HewnMesh3D::get_shape_param(const String &p_name) const {
	for (const hewn::shapes::Param &p : hewn::shapes::params((hewn::shapes::Type)shape)) {
		if (p_name == p.name) {
			const char *old = hewn::shapes::param_fallback((hewn::shapes::Type)shape, p.name);
			if (!shape_params.has(p_name) && old && shape_params.has(old)) {
				return (float)(double)shape_params[old];
			}
			return (float)(double)shape_params.get(p_name, p.def);
		}
	}
	return (float)(double)shape_params.get(p_name, 0.0);
}

bool HewnMesh3D::_set(const StringName &p_name, const Variant &p_value) {
	const String n = p_name;
	if (n.begins_with("shape_") && n != "shape_params" && n != "shape_profile") {
		set_shape_param(n.substr(6), p_value.get_type() == Variant::BOOL ? ((bool)p_value ? 1.0f : 0.0f) : (float)(double)p_value);
		return true;
	}
	return false;
}

bool HewnMesh3D::_get(const StringName &p_name, Variant &r_ret) const {
	const String n = p_name;
	if (n.begins_with("shape_") && n != "shape_params" && n != "shape_profile") {
		const String key = n.substr(6);
		for (const hewn::shapes::Param &p : hewn::shapes::params((hewn::shapes::Type)shape)) {
			if (key == p.name) {
				const float v = get_shape_param(key);
				r_ret = p.is_toggle() ? Variant(v > 0.5f) : p.is_int ? Variant((int64_t)std::lround(v)) : Variant(v);
				return true;
			}
		}
	}
	return false;
}

void HewnMesh3D::_get_property_list(List<PropertyInfo> *p_list) const {
	const std::vector<hewn::shapes::Param> &ps = hewn::shapes::params((hewn::shapes::Type)shape);
	if (ps.empty()) {
		return;
	}
	p_list->push_back(PropertyInfo(Variant::NIL, "Shape Parameters", PROPERTY_HINT_NONE, "shape_", PROPERTY_USAGE_GROUP));
	const std::vector<float> values = _shape_values();
	for (const hewn::shapes::Param &p : ps) {
		if (!hewn::shapes::param_shown((hewn::shapes::Type)shape, values, p.name)) {
			continue;
		}
		if (p.options) {
			p_list->push_back(PropertyInfo(Variant::INT, String("shape_") + p.name, PROPERTY_HINT_ENUM, p.options, PROPERTY_USAGE_EDITOR));
			continue;
		}
		if (p.is_toggle()) {
			p_list->push_back(PropertyInfo(Variant::BOOL, String("shape_") + p.name, PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR));
			continue;
		}
		const String range = String::num(p.min) + "," + String::num(p.max) + "," + (p.is_int ? "1" : "0.001") + ",or_greater";
		p_list->push_back(PropertyInfo(p.is_int ? Variant::INT : Variant::FLOAT, String("shape_") + p.name, PROPERTY_HINT_RANGE, range, PROPERTY_USAGE_EDITOR));
	}
}

void HewnMesh3D::_validate_property(PropertyInfo &p_property) const {
	// The mesh is rebuilt from mesh_data; materials come from the `materials` slots.
	if (p_property.name == StringName("mesh") || String(p_property.name).begins_with("surface_material_override/")) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}
	// the profiles of the shape it is (others are still saved, but not shown)
	const String pn = p_property.name;
	if ((pn == "base_profile" || pn == "shaft_profile" || pn == "top_profile") && shape != hewn::shapes::COLUMN) {
		p_property.usage = PROPERTY_USAGE_STORAGE;
	}
	if (pn == "shape_profile" && shape != hewn::shapes::DOOR && shape != hewn::shapes::ARCH && shape != hewn::shapes::WEDGE) {
		p_property.usage = PROPERTY_USAGE_STORAGE;
	}
	if (pn == "sweep_profile" && shape != hewn::shapes::SWEEP) {
		p_property.usage = PROPERTY_USAGE_STORAGE;
	}
	if (pn == "collision_mode") {
		// (Primitive only for a parametric box / cylinder / sphere / capsule)
		p_property.hint_string = String("None:0,Trimesh:1,Convex:2,Simplified Convex:3,Multiple Convex:4,Box:5,Capsule:6,Cylinder:7,Sphere:8,") +
				(can_primitive_collision() || collision_mode == COLLISION_PRIMITIVE ? "Primitive:9," : "") + "Simplified Trimesh:10";
	}
	if ((pn == "collision_ratio" && collision_mode != COLLISION_SIMPLIFIED_TRIMESH) || (pn == "collision_max_pieces" && collision_mode != COLLISION_MULTIPLE_CONVEX) ||
			(pn == "collision_axis" && collision_mode != COLLISION_CAPSULE && collision_mode != COLLISION_CYLINDER) ||
			((pn == "collision_metadata" || pn == "collision_groups") && collision_mode == COLLISION_NONE)) {
		p_property.usage = PROPERTY_USAGE_STORAGE;
	}
}

// ------------------------------------------------------------------ build

void HewnMesh3D::commit(bool p_geometry_edit) {
	HW_PROF("commit");
	if (p_geometry_edit && shape != hewn::shapes::CUSTOM) {
		shape = hewn::shapes::CUSTOM;
		notify_property_list_changed();
	}
	if (p_geometry_edit) {
		_uv_lock_apply();
	}
	rebuild();
}

void HewnMesh3D::commit_positions() {
	HW_PROF("commit_positions");
	size_t corners = 0;
	for (const hewn::Face &F : data.faces) {
		corners += F.v.size();
	}
	size_t built = 0;
	for (const hewn::SurfaceOut &S : last_build.surfaces) {
		built += S.src_face.size();
	}
	if (has_eval && has_heavy_modifiers()) {
		commit_interactive(); // (the modifiers again, but not the heavy ones until the change ends)
		return;
	}
	// (the faces as they were built, nothing else changing with the positions)
	if (has_eval || uv_lock_on || last_build.face_tris.size() != data.faces.size() || built != corners || shown_array_mesh.is_null() ||
			get_mesh() != shown_array_mesh) {
		if (HwProfScope::on()) {
			std::printf("PROF fallback eval=%d lock=%d hide=%d tris=%d built=%d/%d mesh=%d\n", (int)has_eval, (int)uv_lock_on, (int)hiding_faces,
					(int)(last_build.face_tris.size() == data.faces.size()), (int)built, (int)corners, (int)(get_mesh() == shown_array_mesh));
			std::fflush(stdout);
		}
		commit(true);
		positions_pending = false;
		return;
	}
	if (shape != hewn::shapes::CUSTOM) {
		shape = hewn::shapes::CUSTOM;
		notify_property_list_changed();
	}
	revision++;
	HW_PROF_BEGIN(cp_pos);
	hewn::build_positions_only(data, uv_transform(), last_build);
	HW_PROF_END(cp_pos, "cp positions");
	HW_PROF_BEGIN(cp_fill);
	shown_array_mesh->clear_surfaces();
	_fill_array_mesh(shown_array_mesh, last_build, !Engine::get_singleton()->is_editor_hint());
	HW_PROF_END(cp_fill, "cp fill");
	update_gizmos();
	positions_pending = true;
}

bool HewnMesh3D::dyntopo_refine(const Vector3 &p_center, float p_radius, float p_max_len, bool p_subdivide, bool p_collapse) {
	const bool changed = hewn::ops::dyntopo_refine(data, p_center, p_radius, p_max_len, p_subdivide, p_collapse);
	if (changed) {
		hewn::remove_unused_vertices(data, &selection);
		commit(true);
	}
	return changed;
}

void HewnMesh3D::finish_positions() {
	if (positions_pending) {
		positions_pending = false;
		interactive_light = false;
		commit(true);
	}
}

bool HewnMesh3D::has_heavy_modifiers() const {
	for (int64_t i = 0; i < modifiers.size(); i++) {
		Ref<HewnModifier> m = modifiers[i];
		if (m.is_valid() && m->is_enabled() && m->is_heavy()) {
			return true;
		}
	}
	return false;
}

void HewnMesh3D::commit_interactive() {
	if (!has_heavy_modifiers()) {
		commit(true);
		return;
	}
	interactive_light = true;
	commit(true);
	positions_pending = true; // (rebuild() cleared it: finish_positions() applies them all)
}

void HewnMesh3D::uv_lock_begin() {
	uv_lock_on = true;
	uv_lock_pos = data.pos;
	uv_lock_faces.clear();
	for (const hewn::Face &F : data.faces) {
		uv_lock_faces.push_back(F.v);
	}
	hewn::compute_uvs(data, uv_transform(), uv_lock_uvs);
}

void HewnMesh3D::uv_lock_end() {
	uv_lock_on = false;
	uv_lock_pos.clear();
	uv_lock_faces.clear();
	uv_lock_uvs.clear();
}

void HewnMesh3D::_uv_lock_apply() {
	if (!uv_lock_on || data.faces.size() != uv_lock_faces.size()) {
		return; // (other faces now: an extrusion, a cut...)
	}
	const Transform3D xf = uv_transform();
	for (int f = 0; f < (int)data.faces.size(); f++) {
		const hewn::Face &F = data.faces[f];
		if (F.v != uv_lock_faces[f] || f >= (int)uv_lock_uvs.size()) {
			continue;
		}
		bool moved = false;
		for (int v : F.v) {
			moved = moved || v >= (int)uv_lock_pos.size() || !data.pos[v].is_equal_approx(uv_lock_pos[v]);
		}
		if (moved) {
			hewn::set_face_uvs(data, f, xf, uv_lock_uvs[f]); // (back to the tile settings when it only moved or turned)
		}
	}
}

void HewnMesh3D::set_faces_hidden(const PackedInt32Array &p_faces, bool p_hidden) {
	for (int f : to_vec(p_faces)) {
		if (f >= 0 && f < (int)data.faces.size()) {
			data.faces[f].hidden = p_hidden;
		}
	}
	rebuild();
}

bool HewnMesh3D::is_face_hidden(int p_face) const {
	return p_face >= 0 && p_face < (int)data.faces.size() && data.faces[p_face].hidden;
}

void HewnMesh3D::set_hiding_faces(bool p_on) {
	if (hiding_faces == p_on) {
		return;
	}
	hiding_faces = p_on;
	bool any = false;
	for (const hewn::Face &F : data.faces) {
		any = any || F.hidden;
	}
	if (any) {
		rebuild();
	}
}

// The UV period of each slot's texture, for the world UV islands (see recenter_world_uvs). A material
// changed later (its UV scale) builds again.
void HewnMesh3D::_update_uv_periods() {
	const Callable cb = callable_mp(this, &HewnMesh3D::_on_period_material_changed);
	for (const Ref<Material> &m : period_materials) {
		if (m.is_valid() && m->is_connected("changed", cb)) {
			m->disconnect("changed", cb);
		}
	}
	period_materials.clear();
	data.uv_period.clear();
	if (!has_world_uv) {
		return;
	}
	int slots = 0;
	for (const hewn::Face &f : data.faces) {
		slots = MAX(slots, f.material + 1);
	}
	data.uv_period.resize(slots, Vector2(1, 1));
	for (int s = 0; s < slots; s++) {
		Ref<Material> m = get_material_for_slot(s);
		Ref<BaseMaterial3D> bm = m;
		if (bm.is_valid()) {
			const Vector3 sc = bm->get_uv1_scale();
			data.uv_period[s] = bm->get_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR) || std::fabs(sc.x) < 1e-6f || std::fabs(sc.y) < 1e-6f
					? Vector2()
					: Vector2(1.0f / std::fabs(sc.x), 1.0f / std::fabs(sc.y));
		} else if (m.is_valid()) {
			data.uv_period[s] = Vector2(); // (a shader of its own: its scale is unknown, nothing moved)
		}
		if (m.is_valid() && !m->is_connected("changed", cb)) {
			m->connect("changed", cb);
			period_materials.push_back(m);
		}
	}
}

void HewnMesh3D::_on_period_material_changed() {
	std::vector<Vector2> old = data.uv_period;
	_update_uv_periods();
	if (data.uv_period != old) {
		rebuild();
	}
}

void HewnMesh3D::rebuild() {
	HW_PROF("rebuild");
	positions_pending = false;
	revision++;
	HW_PROF_BEGIN(rb_val);
	hewn::validate(data);
	hewn::clean_wires(data);
	HW_PROF_END(rb_val, "rb validate");
	has_world_uv = false;
	for (const hewn::Face &f : data.faces) {
		if (f.uvp.world && !f.manual_uv) {
			has_world_uv = true;
			break;
		}
	}
	if (has_world_uv) {
		set_notify_transform(true); // (never turned off: VisualInstance3D needs it to move the render instance)
	}
	if (is_inside_tree() && !uv_anchored) {
		uv_last_xf = get_global_transform();
	}
	const Transform3D xf = uv_transform();
	if (has_world_uv) {
		hewn::update_paraxial_axes(data, xf);
	}
	_update_uv_periods();
	has_eval = has_modifiers();
	HW_PROF_BEGIN(rb_build);
	hewn::build_mesh(data, xf, last_build, hiding_faces, has_eval ? nullptr : _lightmap_uvs(data));
	HW_PROF_END(rb_build, "rb build_mesh");
	// Editing (picking, UVs) works on `data`; what is shown includes the modifiers.
	const hewn::BuildOut *shown = &last_build;
	const hewn::Mesh *shown_mesh = &data;
	if (has_eval) {
		eval_mesh = evaluated_mesh();
		hewn::build_mesh(eval_mesh, xf, eval_build, false, _lightmap_uvs(eval_mesh));
		shown = &eval_build;
		shown_mesh = &eval_mesh;
	} else {
		eval_mesh = hewn::Mesh();
		eval_build = hewn::BuildOut();
	}

	// The same ArrayMesh is refilled: a new one (set_mesh) would make the inspector rebuild itself and
	// fold the sub-resource being edited (a modifier field changed -> its section closed).
	Ref<ArrayMesh> am = shown_array_mesh;
	const bool reuse = am.is_valid() && get_mesh() == am;
	const int surfaces_before = reuse ? am->get_surface_count() : -1;
	if (reuse) {
		am->clear_surfaces();
	} else {
		am.instantiate();
		shown_array_mesh = am;
	}
	HW_PROF_BEGIN(rb_fill);
	_fill_array_mesh(am, *shown, !Engine::get_singleton()->is_editor_hint()); // (tool faces are seen in the editor only)
	HW_PROF_END(rb_fill, "rb fill array mesh");
	if (!reuse) {
		set_mesh(am);
	} else if (am->get_surface_count() != surfaces_before) {
		notify_property_list_changed(); // (the surface material override slots)
	}
	HW_PROF_BEGIN(rb_col);
	_update_collision(shown, shown_mesh);
	HW_PROF_END(rb_col, "rb collision");
	update_gizmos();
	emit_signal("mesh_changed");
}

const std::vector<std::vector<Vector2>> *HewnMesh3D::_lightmap_uvs(const hewn::Mesh &p_mesh) {
	if (!lightmap_uv2) {
		return nullptr;
	}
	const uint64_t h = hewn::lightmap_hash(p_mesh, lightmap_texel);
	if (h != uv2_hash || uv2_faces.size() != p_mesh.faces.size()) {
		std::vector<char> skip(p_mesh.faces.size(), 0);
		for (size_t f = 0; f < skip.size(); f++) {
			skip[f] = !tool_of(get_material_for_slot(p_mesh.faces[f].material)).is_empty(); // (not drawn in the game)
		}
		uv2_size = hewn::lightmap_unwrap(p_mesh, lightmap_texel, skip, uv2_faces);
		uv2_hash = h;
	}
	return &uv2_faces;
}

namespace {
// float -> IEEE half (the custom channels' half formats)
uint16_t hw_half(float p_v) {
	uint32_t x;
	memcpy(&x, &p_v, 4);
	const uint32_t sign = (x >> 16) & 0x8000u;
	int32_t e = (int32_t)((x >> 23) & 0xff) - 127 + 15;
	uint32_t mant = x & 0x7fffffu;
	if (e <= 0) {
		return (uint16_t)sign; // (too small: zero)
	}
	if (e >= 31) {
		return (uint16_t)(sign | 0x7c00u); // (too big: infinity)
	}
	return (uint16_t)(sign | ((uint32_t)e << 10) | (mant >> 13));
}
} // namespace

void HewnMesh3D::set_custom_formats(const PackedInt32Array &p_formats) {
	for (int i = 0; i < 4; i++) {
		custom_formats[i] = i < p_formats.size() ? CLAMP(p_formats[i], -1, 7) : -1;
	}
	rebuild();
}

PackedInt32Array HewnMesh3D::get_custom_formats() const {
	PackedInt32Array out;
	for (int i = 0; i < 4; i++) {
		out.push_back(custom_formats[i]);
	}
	return out;
}

// Paints CUSTOM<channel> of the corners of the given vertices (all faces: empty) with the value.
void HewnMesh3D::set_custom_value(int p_channel, const PackedInt32Array &p_verts, const Color &p_value) {
	ERR_FAIL_INDEX(p_channel, 4);
	std::vector<char> on(data.pos.size(), p_verts.is_empty() ? 1 : 0);
	for (int64_t i = 0; i < p_verts.size(); i++) {
		if (p_verts[i] >= 0 && p_verts[i] < (int)on.size()) {
			on[p_verts[i]] = 1;
		}
	}
	for (hewn::Face &F : data.faces) {
		for (int i = 0; i < F.size(); i++) {
			if (!on[F.v[i]]) {
				continue;
			}
			if (F.cust.size() != F.v.size() * 4) {
				F.cust.assign(F.v.size() * 4, Color(0, 0, 0, 0));
			}
			F.cust[i * 4 + p_channel] = p_value;
		}
	}
	if (custom_formats[p_channel] < 0) {
		custom_formats[p_channel] = 7; // (RGBA float until another one is chosen)
	}
	commit(false);
}

void HewnMesh3D::_fill_array_mesh(const Ref<ArrayMesh> &am, const hewn::BuildOut &p_build, bool p_skip_tools) const {
	bool any_uv2 = false;
	for (const hewn::SurfaceOut &s : p_build.surfaces) {
		if (s.indices.empty()) {
			continue;
		}
		if (p_skip_tools && !tool_of(get_material_for_slot(s.material)).is_empty()) {
			continue;
		}
		PackedVector3Array v, n;
		PackedFloat32Array t;
		PackedVector2Array uv;
		PackedColorArray c;
		PackedInt32Array idx;
		v.resize(s.verts.size());
		n.resize(s.normals.size());
		uv.resize(s.uvs.size());
		c.resize(s.colors.size());
		t.resize(s.tangents.size());
		idx.resize(s.indices.size());
		memcpy(v.ptrw(), s.verts.data(), sizeof(Vector3) * s.verts.size());
		memcpy(n.ptrw(), s.normals.data(), sizeof(Vector3) * s.normals.size());
		memcpy(uv.ptrw(), s.uvs.data(), sizeof(Vector2) * s.uvs.size());
		memcpy(c.ptrw(), s.colors.data(), sizeof(Color) * s.colors.size());
		memcpy(t.ptrw(), s.tangents.data(), sizeof(float) * s.tangents.size());
		memcpy(idx.ptrw(), s.indices.data(), sizeof(int) * s.indices.size());
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = v;
		arrays[Mesh::ARRAY_NORMAL] = n;
		arrays[Mesh::ARRAY_TANGENT] = t;
		arrays[Mesh::ARRAY_TEX_UV] = uv;
		// (no colors painted: no color array at all - shaders read white, as with it)
		bool painted = false;
		for (const Color &col : s.colors) {
			if (col.r != 1.0f || col.g != 1.0f || col.b != 1.0f || col.a != 1.0f) {
				painted = true;
				break;
			}
		}
		if (painted) {
			arrays[Mesh::ARRAY_COLOR] = c;
		}
		arrays[Mesh::ARRAY_INDEX] = idx;
		if (s.uv2s.size() == s.verts.size() && !s.uv2s.empty()) {
			PackedVector2Array uv2;
			uv2.resize(s.uv2s.size());
			memcpy(uv2.ptrw(), s.uv2s.data(), sizeof(Vector2) * s.uv2s.size());
			arrays[Mesh::ARRAY_TEX_UV2] = uv2;
			any_uv2 = true;
		}
		// CUSTOM0..3 in the formats chosen for the node (painted values; zero where none)
		int64_t flags = 0;
		for (int ch = 0; ch < 4; ch++) {
			const int fmt = custom_formats[ch];
			if (fmt < 0 || fmt > 7) {
				continue;
			}
			const size_t nv = s.verts.size();
			auto val = [&](size_t i) { return s.customs.size() >= (i + 1) * 4 ? s.customs[i * 4 + ch] : Color(0, 0, 0, 0); };
			if (fmt <= 3) {
				PackedByteArray b;
				const int per = fmt == 3 ? 8 : 4;
				b.resize(nv * per);
				uint8_t *w = b.ptrw();
				for (size_t i = 0; i < nv; i++) {
					const Color c = val(i);
					const float comp[4] = { c.r, c.g, c.b, c.a };
					if (fmt == 0) {
						for (int k = 0; k < 4; k++) {
							w[i * 4 + k] = (uint8_t)CLAMP((int)std::lround(comp[k] * 255.0f), 0, 255);
						}
					} else if (fmt == 1) {
						for (int k = 0; k < 4; k++) {
							w[i * 4 + k] = (uint8_t)(int8_t)CLAMP((int)std::lround(comp[k] * 127.0f), -127, 127);
						}
					} else {
						const int count = fmt == 2 ? 2 : 4;
						for (int k = 0; k < count; k++) {
							const uint16_t h = hw_half(comp[k]);
							w[i * per + k * 2] = (uint8_t)(h & 0xff);
							w[i * per + k * 2 + 1] = (uint8_t)(h >> 8);
						}
					}
				}
				arrays[Mesh::ARRAY_CUSTOM0 + ch] = b;
			} else {
				const int count = fmt - 3; // R, RG, RGB, RGBA float
				PackedFloat32Array f;
				f.resize(nv * count);
				float *w = f.ptrw();
				for (size_t i = 0; i < nv; i++) {
					const Color c = val(i);
					const float comp[4] = { c.r, c.g, c.b, c.a };
					for (int k = 0; k < count; k++) {
						w[i * count + k] = comp[k];
					}
				}
				arrays[Mesh::ARRAY_CUSTOM0 + ch] = f;
			}
			flags |= (int64_t)fmt << (Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT + ch * Mesh::ARRAY_FORMAT_CUSTOM_BITS);
		}
		am->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), BitField<Mesh::ArrayFormat>(flags));
		am->surface_set_material(am->get_surface_count() - 1, get_material_for_slot(s.material));
	}
	am->set_lightmap_size_hint(any_uv2 ? uv2_size : Vector2i());
}

Transform3D HewnMesh3D::uv_transform() const {
	if (uv_anchored) {
		return uv_anchor;
	}
	return is_inside_tree() ? get_global_transform() : get_transform();
}

void HewnMesh3D::set_uv_anchored(bool p_on) {
	uv_anchored = p_on;
	if (p_on && is_inside_tree()) {
		uv_anchor = get_global_transform();
	}
	if (has_world_uv) {
		rebuild();
	}
}

Ref<ArrayMesh> HewnMesh3D::get_game_mesh() {
	const Transform3D xf = uv_transform();
	const hewn::Mesh shown = evaluated_mesh();
	hewn::BuildOut b;
	hewn::build_mesh(shown, xf, b, false, _lightmap_uvs(shown));
	Ref<ArrayMesh> am;
	am.instantiate();
	_fill_array_mesh(am, b, true);
	for (int s = 0; s < am->get_surface_count(); s++) {
		Ref<Material> m = am->surface_get_material(s);
		if (m.is_valid() && m == default_material) {
			am->surface_set_material(s, m->duplicate()); // (the built-in one is not saved with scenes)
		}
	}
	return am;
}

void HewnMesh3D::prepare_bake() {
	if (cuts_dirty) {
		_cuts_refresh();
	}
	_update_collision();
}

Ref<Shape3D> HewnMesh3D::get_collision_shape() const {
	return collision_parts.empty() ? Ref<Shape3D>() : collision_parts[0].shape;
}

Ref<Shape3D> HewnMesh3D::get_trigger_shape() const {
	return Ref<Shape3D>(); // (trigger faces are gone: Hewn -> Area3D makes a trigger)
}

void HewnMesh3D::set_lightmap_uv2(bool p_on) {
	if (lightmap_uv2 == p_on) {
		return;
	}
	lightmap_uv2 = p_on;
	rebuild();
}

bool HewnMesh3D::get_lightmap_uv2() const {
	return lightmap_uv2;
}

void HewnMesh3D::set_lightmap_texel_size(float p_size) {
	p_size = MAX(0.001f, p_size);
	if (lightmap_texel == p_size) {
		return;
	}
	lightmap_texel = p_size;
	if (lightmap_uv2) {
		rebuild();
	}
}

float HewnMesh3D::get_lightmap_texel_size() const {
	return lightmap_texel;
}

void HewnMesh3D::_update_collision() {
	if (has_modifiers()) {
		rebuild(); // collision follows the evaluated mesh
		return;
	}
	_update_collision(&last_build, &data);
}

String HewnMesh3D::tool_of(const Ref<Material> &p_material) {
	return p_material.is_valid() && p_material->has_meta("hw_tool") ? String(p_material->get_meta("hw_tool")) : String();
}

void HewnMesh3D::_update_collision(const hewn::BuildOut *p_build, const hewn::Mesh *p_mesh) {
	// Stairs with "smooth collision": a ramp instead of the steps (also turns the collision on).
	hewn::Mesh ramp;
	hewn::BuildOut ramp_build;
	CollisionMode mode = collision_mode;
	bool ramped = false;
	if (shape == hewn::shapes::PATH_STAIRS && poly_points.size() >= 2 && get_shape_param("smooth_collision") > 0.5f) {
		std::vector<Vector3> pts;
		for (int64_t i = 0; i < poly_points.size(); i++) {
			pts.push_back(poly_points[i]);
		}
		hewn::shapes::build_path_stairs(pts, _shape_values(), ramp, true);
		ramped = !ramp.faces.empty();
	} else if (shape != hewn::shapes::CUSTOM && hewn::shapes::build_collision((hewn::shapes::Type)shape, _shape_values(), ramp)) {
		ramped = true;
	}
	if (ramped) {
		hewn::build_mesh(ramp, Transform3D(), ramp_build);
		p_build = &ramp_build;
		p_mesh = &ramp;
		if (mode == COLLISION_NONE) {
			mode = COLLISION_TRIMESH;
		}
	}
	if (boolean_cutter || is_brush()) {
		mode = COLLISION_NONE; // a boolean operand / brush is not part of the level (the model collides)
	}
	if (mode == COLLISION_NONE) {
		if (body) {
			body->queue_free();
			body = nullptr;
		}
		collision_parts.clear();
		collision_key = 0;
		return;
	}
	// the triangles that collide (not the triggers' faces)
	PackedVector3Array faces;
	for (const hewn::SurfaceOut &s : p_build->surfaces) {
		if (tool_of(get_material_for_slot(s.material)) == "trigger") {
			continue; // (an area, not a wall)
		}
		for (int i : s.indices) {
			faces.push_back(s.verts[i]);
		}
	}
	// (kept while the triangles and the settings stay: some of the shapes take a while to make)
	uint64_t key = 1469598103934665603ull;
	auto mix = [&](uint64_t x) { key = (key ^ x) * 1099511628211ull; };
	mix((uint64_t)mode);
	mix((uint64_t)collision_max_pieces);
	mix((uint64_t)collision_axis);
	mix((uint64_t)std::llround(collision_ratio * 1000.0f));
	mix((uint64_t)shape);
	mix((uint64_t)faces.size());
	{
		const uint8_t *bytes = (const uint8_t *)faces.ptr();
		const size_t nb = (size_t)faces.size() * sizeof(Vector3);
		for (size_t i = 0; i < nb; i += 4) {
			uint32_t w = 0;
			std::memcpy(&w, bytes + i, MIN((size_t)4, nb - i));
			mix(w);
		}
	}
	if (mode == COLLISION_PRIMITIVE) {
		const Dictionary sp = shape_params;
		mix((uint64_t)sp.hash());
	}
	// (the slow ones wait while a change goes on: made when it ends)
	const bool slow = mode == COLLISION_MULTIPLE_CONVEX || mode == COLLISION_SIMPLIFIED_TRIMESH;
	if (key != collision_key && !(slow && interactive_light && !collision_parts.empty())) {
		collision_key = key;
		collision_parts.clear();
		AABB box;
		for (int64_t i = 0; i < faces.size(); i++) {
			box = i ? box.expand(faces[i]) : AABB(faces[i], Vector3());
		}
		const Vector3 sz = box.size, mid = box.get_center();
		// the axis of a capsule / cylinder: the one asked for, or the longest
		int ax = collision_axis - 1;
		if (ax < 0) {
			ax = sz.x >= sz.y && sz.x >= sz.z ? 0 : sz.y >= sz.z ? 1 : 2;
		}
		const float length = sz[ax];
		const float radius = MAX(sz[(ax + 1) % 3], sz[(ax + 2) % 3]) * 0.5f;
		// (Godot's capsules / cylinders stand along Y: turned onto the axis)
		const Basis turn = ax == 0 ? Basis(Vector3(0, 0, 1), (float)Math::PI * 0.5f) : ax == 2 ? Basis(Vector3(1, 0, 0), (float)Math::PI * 0.5f) : Basis();
		Ref<ArrayMesh> am;
		auto mesh_of_faces = [&]() {
			if (am.is_null()) {
				am.instantiate();
				Array arr;
				arr.resize(Mesh::ARRAY_MAX);
				arr[Mesh::ARRAY_VERTEX] = faces;
				if (!faces.is_empty()) {
					am->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr);
				}
			}
			return am;
		};
		switch (mode) {
			case COLLISION_TRIMESH: {
				Ref<ConcavePolygonShape3D> sh;
				sh.instantiate();
				sh->set_faces(faces);
				collision_parts.push_back({ sh, Transform3D() });
			} break;
			case COLLISION_SIMPLIFIED_TRIMESH: {
				hewn::Mesh simple = *p_mesh;
				hewn::ops::decimate_collapse(simple, collision_ratio);
				hewn::BuildOut sb;
				hewn::build_mesh(simple, Transform3D(), sb);
				PackedVector3Array sf;
				for (const hewn::SurfaceOut &s : sb.surfaces) {
					if (tool_of(get_material_for_slot(s.material)) == "trigger") {
						continue;
					}
					for (int i : s.indices) {
						sf.push_back(s.verts[i]);
					}
				}
				Ref<ConcavePolygonShape3D> sh;
				sh.instantiate();
				sh->set_faces(sf);
				collision_parts.push_back({ sh, Transform3D() });
			} break;
			case COLLISION_SIMPLIFIED_CONVEX:
				if (!faces.is_empty()) {
					collision_parts.push_back({ mesh_of_faces()->create_convex_shape(true, true), Transform3D() });
				}
				break;
			case COLLISION_MULTIPLE_CONVEX: {
				// (pieces of boxes on a voxel grid, the mesh cut by each: their hulls)
				for (const std::vector<Vector3> &pts : hewn::ops::convex_pieces(*p_mesh, collision_max_pieces)) {
					Ref<ConvexPolygonShape3D> sh;
					sh.instantiate();
					PackedVector3Array pp;
					for (const Vector3 &p : pts) {
						pp.push_back(p);
					}
					sh->set_points(pp);
					collision_parts.push_back({ sh, Transform3D() });
				}
			} break;
			case COLLISION_BOX: {
				Ref<BoxShape3D> b;
				b.instantiate();
				b->set_size(sz.max(Vector3(0.001f, 0.001f, 0.001f)));
				collision_parts.push_back({ b, Transform3D(Basis(), mid) });
			} break;
			case COLLISION_CAPSULE: {
				Ref<CapsuleShape3D> c;
				c.instantiate();
				c->set_radius(MAX(0.001f, radius));
				c->set_height(MAX(length, radius * 2.0f));
				collision_parts.push_back({ c, Transform3D(turn, mid) });
			} break;
			case COLLISION_CYLINDER: {
				Ref<CylinderShape3D> c;
				c.instantiate();
				c->set_radius(MAX(0.001f, radius));
				c->set_height(MAX(0.001f, length));
				collision_parts.push_back({ c, Transform3D(turn, mid) });
			} break;
			case COLLISION_SPHERE: {
				Ref<SphereShape3D> sp;
				sp.instantiate();
				sp->set_radius(MAX(0.001f, MAX(sz.x, MAX(sz.y, sz.z)) * 0.5f));
				collision_parts.push_back({ sp, Transform3D(Basis(), mid) });
			} break;
			case COLLISION_PRIMITIVE:
				if (_primitive_collision(collision_parts, mid)) {
					break;
				}
				[[fallthrough]]; // (not a primitive any more: its hull)
			default: {
				Ref<ConvexPolygonShape3D> sh;
				sh.instantiate();
				PackedVector3Array pts;
				for (const Vector3 &p : p_mesh->pos) {
					pts.push_back(p);
				}
				sh->set_points(pts);
				collision_parts.push_back({ sh, Transform3D() });
			} break;
		}
	}
	if (!body) {
		body = memnew(StaticBody3D);
		body->set_name("_HewnCollision");
		add_child(body, false, INTERNAL_MODE_BACK);
	}
	// one CollisionShape3D for each part
	while (body->get_child_count() > (int)collision_parts.size()) {
		Node *c = body->get_child(body->get_child_count() - 1);
		body->remove_child(c);
		memdelete(c);
	}
	while (body->get_child_count() < (int)collision_parts.size()) {
		body->add_child(memnew(CollisionShape3D));
	}
	for (int i = 0; i < (int)collision_parts.size(); i++) {
		if (CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(body->get_child(i))) {
			if (cs->get_shape() != collision_parts[i].shape) {
				cs->set_shape(collision_parts[i].shape);
			}
			cs->set_transform(collision_parts[i].xf);
		}
	}
	apply_collision_tags(body);
}

// ------------------------------------------------------------------ modifiers

void HewnMesh3D::_cuts_changed() {
	if (cuts_dirty) {
		return;
	}
	cuts_dirty = true;
	callable_mp(this, &HewnMesh3D::_cuts_refresh).call_deferred(); // once per frame at most
}

void HewnMesh3D::_cuts_refresh() {
	cuts_dirty = false;
	if (!is_inside_tree()) {
		return;
	}
	std::vector<std::pair<uint64_t, Transform3D>> sig;
	const Transform3D inv = get_global_transform().affine_inverse();
	for (HewnCutter3D *c : HewnCutter3D::all) {
		if (c->cuts(this)) {
			sig.push_back({ c->get_instance_id() * 1000003ull + c->get_revision(), inv * c->get_global_transform() });
		}
	}
	bool same = sig.size() == cut_signature.size();
	for (size_t i = 0; same && i < sig.size(); i++) {
		same = sig[i].first == cut_signature[i].first && sig[i].second.is_equal_approx(cut_signature[i].second);
	}
	if (!same) {
		cut_signature = sig;
		rebuild();
	}
}

bool HewnMesh3D::_has_cutters() const {
	for (const HewnCutter3D *c : HewnCutter3D::all) {
		if (c->cuts(this)) {
			return true;
		}
	}
	return false;
}

void HewnMesh3D::_apply_cutters(hewn::Mesh &r_mesh) const {
	const Transform3D inv = get_global_transform().affine_inverse();
	const Basis nb = get_global_transform().basis.transposed();
	const bool lining = has_meta("_hw_lining");
	for (HewnCutter3D *c : HewnCutter3D::all) {
		if (!c->cuts(this)) {
			continue;
		}
		if (!lining && c->get_lining() != HewnCutter3D::LINING_NONE) {
			c->queue_lining(); // (its corridor follows this mesh)
		}
		std::vector<std::vector<hewn::ops::CutPlane>> parts = c->world_parts();
		for (std::vector<hewn::ops::CutPlane> &part : parts) {
			for (hewn::ops::CutPlane &p : part) {
				const Vector3 pt = inv.xform(p.n * p.d);
				p.n = nb.xform(p.n).normalized();
				p.d = p.n.dot(pt);
			}
		}
		hewn::ops::cut_volumes(r_mesh, {}, parts);
	}
}

AABB HewnMesh3D::get_data_aabb() const {
	AABB b;
	for (size_t i = 0; i < data.pos.size(); i++) {
		if (i == 0) {
			b = AABB(data.pos[0], Vector3());
		} else {
			b.expand_to(data.pos[i]);
		}
	}
	return b;
}

bool HewnMesh3D::has_modifiers() const {
	if (csg_model) {
		return true;
	}
	if (is_inside_tree() && !HewnCutter3D::all.empty() && _has_cutters()) {
		return true;
	}
	for (int64_t i = 0; i < modifiers.size(); i++) {
		Ref<HewnModifier> m = modifiers[i];
		if (m.is_valid() && m->is_enabled()) {
			return true;
		}
	}
	return false;
}

hewn::Mesh HewnMesh3D::evaluated_mesh() const {
	hewn::Mesh m = data;
	if (csg_model) {
		_apply_brushes(m);
	}
	for (int64_t i = 0; i < modifiers.size(); i++) {
		Ref<HewnModifier> mod = modifiers[i];
		if (mod.is_valid() && mod->is_enabled() && !(interactive_light && mod->is_heavy())) {
			mod->apply(m, this);
		}
	}
	if (is_inside_tree() && !HewnCutter3D::all.empty()) {
		_apply_cutters(m); // (the non-destructive cuts last)
	}
	return m;
}

Node *HewnMesh3D::resolve_path(const NodePath &p_path) const {
	if (p_path.is_empty() || !is_inside_tree()) {
		return nullptr;
	}
	if (Node *n = get_node_or_null(p_path)) {
		return n;
	}
	Node *root = get_owner();
	return root ? root->get_node_or_null(p_path) : nullptr;
}

// One boolean step with another mesh as it is now (its own modifiers included; nesting is limited).
void HewnMesh3D::apply_boolean(hewn::Mesh &m, HewnMesh3D *o, int op) const {
	static thread_local int depth = 0;
	if (!o || o == this || !o->is_inside_tree() || !is_inside_tree() || depth >= 4) {
		return;
	}
	depth++;
	hewn::Mesh om = o->evaluated_mesh();
	depth--;
	hewn::ops::transform(om, get_global_transform().affine_inverse() * o->get_global_transform());
	boolean_step(m, om, op);
}

bool HewnMesh3D::boolean_step(hewn::Mesh &m, const hewn::Mesh &om, int op) {
	if (m.faces.empty() || om.faces.empty()) {
		if (op == BOOLEAN_UNION) {
			hewn::ops::append(m, om, Transform3D(), 0);
		} else if (op == BOOLEAN_INTERSECT) {
			m.clear();
		}
		return true;
	}
	// Operands that do not touch: no BSP (it would only re-split the faces).
	AABB ba(m.pos[0], Vector3()), bb(om.pos[0], Vector3());
	for (const Vector3 &p : m.pos) {
		ba.expand_to(p);
	}
	for (const Vector3 &p : om.pos) {
		bb.expand_to(p);
	}
	if (!ba.grow(1e-4f).intersects(bb)) {
		if (op == BOOLEAN_UNION) {
			hewn::ops::append(m, om, Transform3D(), 0);
		} else if (op == BOOLEAN_INTERSECT) {
			m.clear();
		}
		return true;
	}
	bool ok = false;
	hewn::Mesh r = hewn::csg::boolean(m, om, (hewn::csg::Op)CLAMP(op, 0, 2), &ok);
	if (ok) {
		m = r;
	}
	return ok;
}

// ------------------------------------------------------------------ CSG model

HewnMesh3D *HewnMesh3D::get_csg_model() const {
	HewnMesh3D *p = Object::cast_to<HewnMesh3D>(get_parent());
	return p && p->csg_model ? p : nullptr;
}

std::vector<HewnMesh3D *> HewnMesh3D::get_brushes() const {
	std::vector<HewnMesh3D *> out;
	if (!csg_model) {
		return out;
	}
	for (int i = 0; i < get_child_count(); i++) {
		HewnMesh3D *b = Object::cast_to<HewnMesh3D>(get_child(i));
		if (b && b->is_visible()) {
			out.push_back(b);
		}
	}
	return out;
}

TypedArray<Node> HewnMesh3D::get_brush_nodes() const {
	TypedArray<Node> a;
	for (HewnMesh3D *b : get_brushes()) {
		a.push_back(b);
	}
	return a;
}

void HewnMesh3D::_apply_brushes(hewn::Mesh &m) const {
	csg_materials.clear();
	static thread_local int depth = 0;
	if (!is_inside_tree() || depth >= 8) {
		return;
	}
	const Transform3D inv = get_global_transform().affine_inverse();
	hewn::Mesh own;
	if (csg_inverted) {
		own = m; // (our own geometry stays solid: added at the end)
		m.clear();
	}
	for (HewnMesh3D *b : get_brushes()) {
		depth++;
		hewn::Mesh bm = b->evaluated_mesh();
		depth--;
		hewn::ops::transform(bm, inv * b->get_global_transform());
		// the brush's materials become our slots (each material once; none: the default one)
		std::unordered_map<int, int> remap;
		for (hewn::Face &F : bm.faces) {
			auto it = remap.find(F.material);
			if (it == remap.end()) {
				Ref<Material> mat;
				if (F.material >= 0 && F.material < b->materials.size()) {
					mat = b->materials[F.material];
				}
				int slot = -1;
				for (int k = 0; k < materials.size() && mat.is_valid(); k++) {
					if (Ref<Material>(materials[k]) == mat) {
						slot = k;
						break;
					}
				}
				for (size_t k = 0; k < csg_materials.size() && slot < 0; k++) {
					if (csg_materials[k] == mat) {
						slot = (int)(materials.size() + k);
					}
				}
				if (slot < 0) {
					csg_materials.push_back(mat);
					slot = (int)(materials.size() + csg_materials.size() - 1);
				}
				it = remap.emplace(F.material, slot).first;
			}
			F.material = it->second;
		}
		int op = b->brush_operation;
		if (csg_inverted && op != BOOLEAN_INTERSECT) {
			op = op == BOOLEAN_UNION ? BOOLEAN_SUBTRACT : BOOLEAN_UNION; // (we build the empty space)
		}
		boolean_step(m, bm, op);
	}
	if (csg_inverted) {
		for (hewn::Face &F : m.faces) {
			F.reverse(); // the inside of the empty space
		}
		if (!own.faces.empty()) {
			hewn::ops::append(m, own, Transform3D(), 0);
		}
	}
}

void HewnMesh3D::set_csg_model(bool p_on) {
	if (csg_model == p_on) {
		return;
	}
	csg_model = p_on;
	for (int i = 0; i < get_child_count(); i++) {
		if (HewnMesh3D *b = Object::cast_to<HewnMesh3D>(get_child(i))) {
			b->_update_brush_display();
		}
	}
	bool_force = true;
	if (is_inside_tree()) {
		_watch_operands();
		rebuild();
	}
	notify_property_list_changed();
}

bool HewnMesh3D::is_csg_model() const {
	return csg_model;
}

void HewnMesh3D::set_csg_inverted(bool p_on) {
	csg_inverted = p_on;
	if (is_inside_tree() && csg_model) {
		rebuild();
	}
}

bool HewnMesh3D::is_csg_inverted() const {
	return csg_inverted;
}

void HewnMesh3D::set_brush_operation(int p_op) {
	brush_operation = CLAMP(p_op, 0, 2);
	_apply_cutter_display();
	if (HewnMesh3D *model = get_csg_model()) {
		model->bool_force = true;
		model->_on_operand_changed();
	}
}

int HewnMesh3D::get_brush_operation() const {
	return brush_operation;
}

void HewnMesh3D::_update_brush_display() {
	_apply_cutter_display();
	_update_collision();
}

void HewnMesh3D::set_modifiers(const TypedArray<HewnModifier> &p_modifiers) {
	const Callable cb = callable_mp(this, &HewnMesh3D::_on_modifier_changed);
	for (int64_t i = 0; i < modifiers.size(); i++) {
		Ref<HewnModifier> m = modifiers[i];
		if (m.is_valid() && m->is_connected("changed", cb)) {
			m->disconnect("changed", cb);
		}
	}
	modifiers = p_modifiers.duplicate(); // (the array; the modifiers themselves are shared)
	for (int64_t i = 0; i < modifiers.size(); i++) {
		Ref<HewnModifier> m = modifiers[i];
		if (m.is_valid() && !m->is_connected("changed", cb)) {
			m->connect("changed", cb);
		}
	}
	_on_modifier_changed();
}

TypedArray<HewnModifier> HewnMesh3D::get_modifiers() const {
	return modifiers;
}

void HewnMesh3D::add_modifier(const Ref<HewnModifier> &p_modifier) {
	TypedArray<HewnModifier> a = modifiers.duplicate();
	a.push_back(p_modifier);
	set_modifiers(a);
}

void HewnMesh3D::apply_mirror_clipping(const std::vector<int> &p_verts, const std::vector<Vector3> &p_before) {
	for (int64_t i = 0; i < modifiers.size(); i++) {
		Ref<HewnModifier> mod = modifiers[i];
		HewnMirrorModifier *mm = Object::cast_to<HewnMirrorModifier>(mod.ptr());
		if (!mm || !mm->is_enabled() || !mm->get_clipping()) {
			continue;
		}
		const float eps = MAX(1e-6f, mm->get_merge_distance());
		for (int a = 0; a < 3; a++) {
			if (!mm->get_axis(a)) {
				continue;
			}
			for (size_t k = 0; k < p_verts.size() && k < p_before.size(); k++) {
				Vector3 &p = data.pos[p_verts[k]];
				const float b = p_before[k][a];
				if (std::fabs(b) <= eps || (b > 0.0f) != (p[a] > 0.0f)) {
					p[a] = 0.0f;
				}
			}
		}
	}
}

void HewnMesh3D::_on_modifier_changed() {
	if (is_inside_tree()) {
		_watch_operands();
		rebuild();
	}
}

template <typename T>
T *HewnMesh3D::_first_modifier() const {
	for (int64_t i = 0; i < modifiers.size(); i++) {
		Ref<HewnModifier> m = modifiers[i];
		if (T *t = Object::cast_to<T>(m.ptr())) {
			return t;
		}
	}
	return nullptr;
}

template <typename T>
T *HewnMesh3D::_ensure_modifier() {
	if (T *t = _first_modifier<T>()) {
		return t;
	}
	Ref<T> t;
	t.instantiate();
	add_modifier(t);
	return t.ptr();
}

void HewnMesh3D::_remove_modifier(const Ref<HewnModifier> &p_mod) {
	TypedArray<HewnModifier> a;
	for (int64_t i = 0; i < modifiers.size(); i++) {
		Ref<HewnModifier> m = modifiers[i];
		if (m != p_mod) {
			a.push_back(m);
		}
	}
	set_modifiers(a);
}

// ---- the old modifier properties (scripts, scenes saved before the stack): they edit the stack

void HewnMesh3D::set_boolean_targets(const TypedArray<NodePath> &p_targets) {
	TypedArray<HewnModifier> a;
	int k = 0;
	for (int64_t i = 0; i < modifiers.size(); i++) {
		Ref<HewnModifier> m = modifiers[i];
		if (!Object::cast_to<HewnBooleanModifier>(m.ptr())) {
			a.push_back(m);
		}
	}
	for (int64_t i = 0; i < p_targets.size(); i++, k++) {
		Ref<HewnBooleanModifier> b;
		b.instantiate();
		b->set_object(p_targets[i]);
		b->set_operation(k < legacy_bool_ops.size() ? legacy_bool_ops[k] : (int)BOOLEAN_SUBTRACT);
		a.push_back(b);
	}
	set_modifiers(a);
}

TypedArray<NodePath> HewnMesh3D::get_boolean_targets() const {
	TypedArray<NodePath> t;
	for (int64_t i = 0; i < modifiers.size(); i++) {
		Ref<HewnModifier> m = modifiers[i];
		if (HewnBooleanModifier *b = Object::cast_to<HewnBooleanModifier>(m.ptr())) {
			t.push_back(b->get_object());
		}
	}
	return t;
}

void HewnMesh3D::set_boolean_ops(const PackedInt32Array &p_ops) {
	legacy_bool_ops = p_ops;
	int k = 0;
	for (int64_t i = 0; i < modifiers.size(); i++) {
		Ref<HewnModifier> m = modifiers[i];
		if (HewnBooleanModifier *b = Object::cast_to<HewnBooleanModifier>(m.ptr())) {
			if (k < p_ops.size()) {
				b->set_operation(p_ops[k]);
			}
			k++;
		}
	}
}

PackedInt32Array HewnMesh3D::get_boolean_ops() const {
	PackedInt32Array o;
	for (int64_t i = 0; i < modifiers.size(); i++) {
		Ref<HewnModifier> m = modifiers[i];
		if (HewnBooleanModifier *b = Object::cast_to<HewnBooleanModifier>(m.ptr())) {
			o.push_back(b->get_operation());
		}
	}
	return o;
}

void HewnMesh3D::add_boolean(HewnMesh3D *p_other, int p_op) {
	if (!p_other || p_other == this || !is_inside_tree() || !p_other->is_inside_tree()) {
		return;
	}
	Ref<HewnBooleanModifier> b;
	b.instantiate();
	b->set_object(get_path_to(p_other));
	b->set_operation(p_op);
	add_modifier(b);
}

// Follows the operands and paths: their geometry or their place changes our result.
void HewnMesh3D::_watch_operands() {
	const Callable cb = callable_mp(this, &HewnMesh3D::_on_operand_changed);
	for (ObjectID id : bool_watched) {
		Object *o = ObjectDB::get_instance(id);
		for (const char *sig : { "mesh_changed", "geometry_moved", "curve_changed", "visibility_changed" }) {
			if (o && o->has_signal(sig) && o->is_connected(sig, cb)) {
				o->disconnect(sig, cb);
			}
		}
	}
	bool_watched.clear();
	path_watched.clear();
	if (!is_inside_tree()) {
		set_process(false);
		return;
	}
	std::vector<Node *> deps;
	for (int64_t i = 0; i < modifiers.size(); i++) {
		Ref<HewnModifier> m = modifiers[i];
		if (m.is_valid() && m->is_enabled()) {
			m->get_dependencies(this, deps);
		}
	}
	if (csg_model) {
		for (int i = 0; i < get_child_count(); i++) {
			if (HewnMesh3D *b = Object::cast_to<HewnMesh3D>(get_child(i))) {
				b->connect("visibility_changed", cb); // (a hidden brush is left out)
				if (b->is_visible()) {
					deps.push_back(b);
				} else {
					bool_watched.push_back(ObjectID(b->get_instance_id()));
				}
			}
		}
	}
	const Transform3D inv = get_global_transform().affine_inverse();
	for (Node *d : deps) {
		if (HewnMesh3D *o = Object::cast_to<HewnMesh3D>(d)) {
			if (o == this || o->get_boolean_targets().has(o->get_path_to(this))) {
				continue; // (two meshes cutting each other would update forever)
			}
			o->set_notify_transform(true);
			o->connect("mesh_changed", cb);
			o->connect("geometry_moved", cb);
			bool_watched.push_back(ObjectID(o->get_instance_id()));
		} else if (Node3D *p = Object::cast_to<Node3D>(d)) {
			// a path, a mirror object...: moved without a signal, checked every frame
			if (Path3D *path = Object::cast_to<Path3D>(d)) {
				path->connect("curve_changed", cb);
			}
			bool_watched.push_back(ObjectID(p->get_instance_id()));
			path_watched.push_back({ ObjectID(p->get_instance_id()), inv * p->get_global_transform() });
		}
	}
	set_process(!path_watched.empty()); // a moved Path3D sends no signal: checked every frame
}

void HewnMesh3D::_on_operand_changed() {
	if (bool_dirty) {
		return;
	}
	bool_dirty = true;
	callable_mp(this, &HewnMesh3D::_bool_refresh).call_deferred(); // once per frame at most
}

void HewnMesh3D::_bool_refresh() {
	bool_dirty = false;
	if (!is_inside_tree()) {
		return;
	}
	_watch_operands();
	// The operands as they are now: when none changed or moved relative to us (the whole model was
	// moved), there is nothing to rebuild.
	std::vector<std::pair<ObjectID, std::pair<uint64_t, Transform3D>>> sig;
	bool comparable = path_watched.empty();
	const Transform3D inv = get_global_transform().affine_inverse();
	for (ObjectID id : bool_watched) {
		HewnMesh3D *o = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(id));
		if (!o) {
			comparable = false;
			continue;
		}
		sig.push_back({ id, { o->revision + (o->is_visible() ? 0 : (1ull << 62)), inv * o->get_global_transform() } });
	}
	bool same = comparable && !bool_force && sig.size() == bool_signature.size();
	for (size_t i = 0; same && i < sig.size(); i++) {
		same = sig[i].first == bool_signature[i].first && sig[i].second.first == bool_signature[i].second.first &&
				sig[i].second.second.is_equal_approx(bool_signature[i].second.second);
	}
	bool_signature = sig;
	bool_force = false;
	if (!same) {
		rebuild();
	}
}

void HewnMesh3D::set_boolean_cutter(bool p_on) {
	boolean_cutter = p_on;
	_apply_cutter_display();
	_update_collision();
}

bool HewnMesh3D::is_boolean_cutter() const {
	return boolean_cutter;
}

// A boolean operand is drawn see-through in the editor and not at all in the game (like the cutter
// objects, shown as wire / bounds).
void HewnMesh3D::_apply_cutter_display() {
	if (!is_inside_tree() || !get_instance().is_valid()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	const bool brush = is_brush();
	const bool ghost = boolean_cutter || brush;
	if (Engine::get_singleton()->is_editor_hint()) {
		// an adding brush is what the model shows already: invisible (still picked by clicks), the
		// others see-through
		const float t = brush && brush_operation == BOOLEAN_UNION ? 1.0f : 0.75f;
		rs->instance_geometry_set_transparency(get_instance(), ghost ? t : get_transparency());
		rs->instance_geometry_set_cast_shadows_setting(get_instance(),
				ghost ? RenderingServer::SHADOW_CASTING_SETTING_OFF : (RenderingServer::ShadowCastingSetting)get_cast_shadows_setting());
	} else {
		rs->instance_set_visible(get_instance(), !ghost && is_visible_in_tree());
	}
}

#define HW_MOD_BOOL(m_name, m_index)                                                \
	void HewnMesh3D::set_##m_name(bool p_on) {                                 \
		HewnMirrorModifier *mm = p_on ? _ensure_modifier<HewnMirrorModifier>() : _first_modifier<HewnMirrorModifier>(); \
		if (mm) {                                                                   \
			mm->set_axis(m_index, p_on);                                            \
			if (!mm->get_x() && !mm->get_y() && !mm->get_z()) {                    \
				_remove_modifier(Ref<HewnModifier>(mm));                              \
			}                                                                       \
		}                                                                           \
	}                                                                               \
	bool HewnMesh3D::get_##m_name() const {                                    \
		HewnMirrorModifier *mm = _first_modifier<HewnMirrorModifier>();                \
		return mm && mm->get_axis(m_index);                                         \
	}
HW_MOD_BOOL(mirror_x, 0)
HW_MOD_BOOL(mirror_y, 1)
HW_MOD_BOOL(mirror_z, 2)
#undef HW_MOD_BOOL

void HewnMesh3D::set_mirror_merge(float p_dist) {
	if (HewnMirrorModifier *mm = _first_modifier<HewnMirrorModifier>()) {
		mm->set_merge_distance(p_dist);
	}
}

float HewnMesh3D::get_mirror_merge() const {
	HewnMirrorModifier *mm = _first_modifier<HewnMirrorModifier>();
	return mm ? mm->get_merge_distance() : 0.001f;
}

void HewnMesh3D::set_solidify_thickness(float p_thickness) {
	const bool on = std::fabs(p_thickness) > 1e-9f;
	HewnSolidifyModifier *sm = on ? _ensure_modifier<HewnSolidifyModifier>() : _first_modifier<HewnSolidifyModifier>();
	if (sm && on) {
		sm->set_thickness(p_thickness);
	} else if (sm) {
		_remove_modifier(Ref<HewnModifier>(sm));
	}
}

float HewnMesh3D::get_solidify_thickness() const {
	HewnSolidifyModifier *sm = _first_modifier<HewnSolidifyModifier>();
	return sm ? sm->get_thickness() : 0.0f;
}

void HewnMesh3D::set_solidify_offset(float p_offset) {
	if (HewnSolidifyModifier *sm = _first_modifier<HewnSolidifyModifier>()) {
		sm->set_offset(p_offset);
	}
}

float HewnMesh3D::get_solidify_offset() const {
	HewnSolidifyModifier *sm = _first_modifier<HewnSolidifyModifier>();
	return sm ? sm->get_offset() : -1.0f;
}

void HewnMesh3D::apply_modifiers() {
	if (!has_modifiers()) {
		return;
	}
	data = evaluated_mesh();
	set_modifiers(TypedArray<HewnModifier>());
	selection.clear();
	commit(true);
	notify_property_list_changed();
}

// ------------------------------------------------------------------ serialization

namespace {
template <typename P, typename T>
P hw_packed(const std::vector<T> &p_src) {
	P out;
	out.resize((int64_t)p_src.size());
	if (!p_src.empty()) {
		std::memcpy(out.ptrw(), p_src.data(), p_src.size() * sizeof(T));
	}
	return out;
}
} // namespace

Dictionary HewnMesh3D::get_mesh_data() const {
	// (built in std::vector and copied over at once: a Packed array's push_back goes through the engine each time)
	std::vector<Vector3> positions(data.pos.begin(), data.pos.end());
	std::vector<int32_t> sizes, indices, mats, roles, smooth, uv_group, flags;
	std::vector<Vector2> uvs;
	std::vector<Color> colors, customs;
	std::vector<float> uvp;
	size_t corners = 0;
	for (const hewn::Face &f : data.faces) {
		corners += f.v.size();
	}
	indices.reserve(corners);
	uvs.reserve(corners);
	colors.reserve(corners);
	sizes.reserve(data.faces.size());
	uvp.reserve(data.faces.size() * 5);
	bool any_custom = false;
	for (const hewn::Face &f : data.faces) {
		any_custom = any_custom || !f.cust.empty();
	}
	for (const hewn::Face &f : data.faces) {
		sizes.push_back(f.size());
		for (int i = 0; i < f.size(); i++) {
			indices.push_back(f.v[i]);
			uvs.push_back(i < (int)f.uv.size() ? f.uv[i] : Vector2());
			colors.push_back(i < (int)f.col.size() ? f.col[i] : Color(1, 1, 1, 1));
			for (int c = 0; any_custom && c < 4; c++) {
				customs.push_back(f.cust.size() >= (size_t)(i + 1) * 4 ? f.cust[i * 4 + c] : Color(0, 0, 0, 0));
			}
		}
		mats.push_back(f.material);
		roles.push_back(f.role);
		smooth.push_back(f.smooth);
		uv_group.push_back(f.uv_group);
		int fl = (f.manual_uv ? 1 : 0) | (f.uvp.world ? 2 : 0) | (f.uvp.flip_u ? 4 : 0) | (f.uvp.flip_v ? 8 : 0) | (f.uvp.swap ? 16 : 0) | (f.hidden ? 64 : 0);
		fl |= (f.uvp.axis >= 0 && f.uvp.axis < 6 ? f.uvp.axis + 1 : 0) << 20; // (world UVs: the axis it is projected along)
		fl |= f.uvp.fill << 8;
		fl |= (f.strength == 2 ? 1 : f.strength == 0 ? 2 : 0) << 16;
		fl |= (f.uvp.face_aligned ? 1 : 0) << 23;
		flags.push_back(fl);
		uvp.push_back(f.uvp.scale.x);
		uvp.push_back(f.uvp.scale.y);
		uvp.push_back(f.uvp.offset.x);
		uvp.push_back(f.uvp.offset.y);
		uvp.push_back(f.uvp.rotation);
	}
	Dictionary d;
	d["positions"] = hw_packed<PackedVector3Array>(positions);
	d["face_sizes"] = hw_packed<PackedInt32Array>(sizes);
	d["face_indices"] = hw_packed<PackedInt32Array>(indices);
	d["face_uvs"] = hw_packed<PackedVector2Array>(uvs);
	bool any_color = false; // (nothing painted: no colors saved, they load as white)
	for (int64_t i = 0; i < colors.size() && !any_color; i++) {
		any_color = colors[i] != Color(1, 1, 1, 1);
	}
	if (any_color) {
		d["face_colors"] = hw_packed<PackedColorArray>(colors);
	}
	if (any_custom) {
		d["face_custom"] = hw_packed<PackedColorArray>(customs); // (CUSTOM0..3, 4 per corner)
	}
	d["face_materials"] = hw_packed<PackedInt32Array>(mats);
	d["face_roles"] = hw_packed<PackedInt32Array>(roles);
	d["face_smooth"] = hw_packed<PackedInt32Array>(smooth);
	d["face_uv_groups"] = hw_packed<PackedInt32Array>(uv_group);
	d["face_flags"] = hw_packed<PackedInt32Array>(flags);
	// edge / vertex attributes (only edges that still exist)
	{
		// (the edges of the faces: only looked up when there are marks to keep)
		std::unordered_set<uint64_t> exist;
		if (!data.edge_attrs.empty()) {
			for (const hewn::Face &F : data.faces) {
				for (int i = 0; i < F.size(); i++) {
					exist.insert(hewn::ekey(F.v[i], F.v[(i + 1) % F.size()]));
				}
			}
		}
		PackedInt32Array pairs, eflags, vids;
		PackedFloat32Array crease, bevel, vcrease;
		for (const auto &kv : data.edge_attrs) {
			const int a = (int)(kv.first >> 32), b = (int)(kv.first & 0xffffffffu);
			if (kv.second.empty() || !exist.count(hewn::ekey(a, b))) {
				continue;
			}
			pairs.push_back(a);
			pairs.push_back(b);
			eflags.push_back((kv.second.seam ? 1 : 0) | (kv.second.sharp ? 2 : 0));
			crease.push_back(kv.second.crease);
			bevel.push_back(kv.second.bevel_weight);
		}
		for (const auto &kv : data.vert_crease) {
			if (kv.second > 0.0f && kv.first < (int)data.pos.size()) {
				vids.push_back(kv.first);
				vcrease.push_back(kv.second);
			}
		}
		if (!pairs.is_empty()) {
			d["edge_attr_pairs"] = pairs;
			d["edge_attr_flags"] = eflags;
			d["edge_attr_crease"] = crease;
			d["edge_attr_bevel"] = bevel;
		}
		if (!vids.is_empty()) {
			d["vert_crease_ids"] = vids;
			d["vert_crease_values"] = vcrease;
		}
	}
	if (!data.wires.empty()) {
		PackedInt32Array wires;
		for (const std::pair<int, int> &w : data.wires) {
			wires.push_back(w.first);
			wires.push_back(w.second);
		}
		d["wires"] = wires;
	}
	if (!data.points.empty()) {
		PackedInt32Array pts;
		for (int v : data.points) {
			pts.push_back(v);
		}
		d["points"] = pts;
	}
	d["face_uv_params"] = hw_packed<PackedFloat32Array>(uvp);
	if (lightmap_uv2 && !uv2_faces.empty()) {
		PackedVector2Array uv2;
		for (const std::vector<Vector2> &fu : uv2_faces) {
			for (const Vector2 &q : fu) {
				uv2.push_back(q);
			}
		}
		d["lightmap_uv2"] = uv2;
		d["lightmap_hash"] = (int64_t)uv2_hash;
		d["lightmap_size"] = uv2_size;
	}
	return d;
}

void HewnMesh3D::set_mesh_data(const Dictionary &d) {
	HW_PROF("set_mesh_data");
	const PackedVector3Array positions = d.get("positions", PackedVector3Array());
	const PackedInt32Array sizes = d.get("face_sizes", PackedInt32Array());
	const PackedInt32Array indices = d.get("face_indices", PackedInt32Array());
	const PackedVector2Array uvs = d.get("face_uvs", PackedVector2Array());
	const PackedColorArray colors = d.get("face_colors", PackedColorArray());
	const PackedColorArray customs = d.get("face_custom", PackedColorArray());
	const PackedInt32Array mats = d.get("face_materials", PackedInt32Array());
	const PackedInt32Array roles = d.get("face_roles", PackedInt32Array());
	const PackedInt32Array smooth = d.get("face_smooth", PackedInt32Array());
	const PackedInt32Array uv_group = d.get("face_uv_groups", PackedInt32Array());
	const PackedInt32Array flags = d.get("face_flags", PackedInt32Array());
	const PackedFloat32Array uvp = d.get("face_uv_params", PackedFloat32Array());
	// (read through raw pointers: operator[] goes through the engine for every element)
	const auto *positions_p = positions.ptr();
	const auto *sizes_p = sizes.ptr();
	const auto *indices_p = indices.ptr();
	const auto *uvs_p = uvs.ptr();
	const auto *colors_p = colors.ptr();
	const auto *customs_p = customs.ptr();
	const auto *mats_p = mats.ptr();
	const auto *roles_p = roles.ptr();
	const auto *smooth_p = smooth.ptr();
	const auto *uv_group_p = uv_group.ptr();
	const auto *flags_p = flags.ptr();
	const auto *uvp_p = uvp.ptr();
	const int64_t positions_n = positions.size();
	const int64_t sizes_n = sizes.size();
	const int64_t indices_n = indices.size();
	const int64_t uvs_n = uvs.size();
	const int64_t colors_n = colors.size();
	const int64_t customs_n = customs.size();
	const int64_t mats_n = mats.size();
	const int64_t roles_n = roles.size();
	const int64_t smooth_n = smooth.size();
	const int64_t uv_group_n = uv_group.size();
	const int64_t flags_n = flags.size();
	const int64_t uvp_n = uvp.size();
	data.clear();
	data.pos.reserve(positions_n);
	data.faces.reserve(sizes_n);
	for (int64_t i = 0; i < positions_n; i++) {
		data.pos.push_back(positions_p[i]);
	}
	int64_t k = 0;
	for (int64_t f = 0; f < sizes_n; f++) {
		const int n = sizes_p[f];
		if (n < 0 || k + n > indices_n) {
			break;
		}
		hewn::Face F;
		F.v.reserve(n);
		F.uv.reserve(n);
		F.col.reserve(n);
		for (int i = 0; i < n; i++, k++) {
			F.v.push_back(indices_p[k]);
			F.uv.push_back(k < uvs_n ? uvs_p[k] : Vector2());
			F.col.push_back(k < colors_n ? colors_p[k] : Color(1, 1, 1, 1));
			if ((k + 1) * 4 <= customs_n) {
				for (int c = 0; c < 4; c++) {
					F.cust.push_back(customs_p[k * 4 + c]);
				}
			}
		}
		F.material = f < mats_n ? mats_p[f] : 0;
		F.role = f < roles_n ? roles_p[f] : 0;
		F.smooth = f < smooth_n ? smooth_p[f] : 0;
		F.uv_group = f < uv_group_n ? uv_group_p[f] : 0;
		const int fl = f < flags_n ? flags_p[f] : 0;
		F.manual_uv = fl & 1;
		F.uvp.world = fl & 2;
		F.uvp.flip_u = fl & 4;
		F.uvp.flip_v = fl & 8;
		F.uvp.swap = fl & 16;
		F.hidden = fl & 64;
		F.uvp.fill = (fl >> 8) & 0xff;
		F.strength = ((fl >> 16) & 3) == 1 ? 2 : ((fl >> 16) & 3) == 2 ? 0 : 1;
		F.uvp.axis = ((fl >> 20) & 7) - 1;
		F.uvp.face_aligned = (fl >> 23) & 1;
		if ((f + 1) * 5 <= uvp_n) {
			F.uvp.scale = Vector2(uvp_p[f * 5], uvp_p[f * 5 + 1]);
			F.uvp.offset = Vector2(uvp_p[f * 5 + 2], uvp_p[f * 5 + 3]);
			F.uvp.rotation = uvp_p[f * 5 + 4];
		}
		data.faces.push_back(std::move(F));
	}
	const PackedInt32Array pairs = d.get("edge_attr_pairs", PackedInt32Array());
	const PackedInt32Array eflags = d.get("edge_attr_flags", PackedInt32Array());
	const PackedFloat32Array crease = d.get("edge_attr_crease", PackedFloat32Array());
	const PackedFloat32Array bevel = d.get("edge_attr_bevel", PackedFloat32Array());
	for (int64_t i = 0; i + 1 < pairs.size(); i += 2) {
		const int64_t k = i / 2;
		hewn::EdgeAttr ea;
		const int fl = k < eflags.size() ? eflags[k] : 0;
		ea.seam = fl & 1;
		ea.sharp = fl & 2;
		ea.crease = k < crease.size() ? crease[k] : 0.0f;
		ea.bevel_weight = k < bevel.size() ? bevel[k] : 0.0f;
		data.edge_attrs[hewn::ekey(pairs[i], pairs[i + 1])] = ea;
	}
	const PackedInt32Array vids = d.get("vert_crease_ids", PackedInt32Array());
	const PackedFloat32Array vcrease = d.get("vert_crease_values", PackedFloat32Array());
	for (int64_t i = 0; i < vids.size() && i < vcrease.size(); i++) {
		data.vert_crease[vids[i]] = vcrease[i];
	}
	const PackedInt32Array wires = d.get("wires", PackedInt32Array());
	for (int64_t i = 0; i + 1 < wires.size(); i += 2) {
		data.wires.push_back({ wires[i], wires[i + 1] });
	}
	const PackedInt32Array pts = d.get("points", PackedInt32Array());
	for (int64_t i = 0; i < pts.size(); i++) {
		data.points.push_back(pts[i]);
	}
	hewn::clean_wires(data);
	// lightmap UVs as saved (kept while the geometry they were made for is the same)
	const PackedVector2Array luv = d.get("lightmap_uv2", PackedVector2Array());
	if (!luv.is_empty()) {
		uv2_hash = (uint64_t)(int64_t)d.get("lightmap_hash", 0);
		uv2_size = d.get("lightmap_size", Vector2i());
		uv2_faces.clear();
		int64_t q = 0;
		for (int64_t f = 0; f < sizes.size(); f++) {
			std::vector<Vector2> fu;
			for (int i = 0; i < sizes[f]; i++) {
				fu.push_back(q < luv.size() ? luv[q] : Vector2());
				q++;
			}
			uv2_faces.push_back(fu);
		}
	}
	selection.clear();
	rebuild();
}

Dictionary HewnMesh3D::get_state() const {
	HW_PROF("get_state");
	Dictionary d = get_mesh_data();
	d["shape"] = shape;
	d["shape_params"] = shape_params.duplicate();
	d["poly_points"] = poly_points;
	d["sel_verts"] = to_packed(selection.verts);
	d["sel_edges"] = edges_to_packed(selection.edges);
	d["sel_faces"] = to_packed(selection.faces);
	return d;
}

void HewnMesh3D::set_state(const Dictionary &p_state) {
	HW_PROF("set_state");
	const int old_shape = shape;
	shape = p_state.get("shape", shape);
	shape_params = Dictionary(p_state.get("shape_params", shape_params)).duplicate();
	poly_points = p_state.get("poly_points", poly_points);
	set_mesh_data(p_state);
	const int nv = (int)data.pos.size(), nf = (int)data.faces.size();
	for (int v : to_vec(p_state.get("sel_verts", PackedInt32Array()))) {
		if (v >= 0 && v < nv) {
			selection.verts.push_back(v);
		}
	}
	for (const hewn::Edge &e : to_edges(p_state.get("sel_edges", PackedInt32Array()))) {
		if (e.a >= 0 && e.b < nv) {
			selection.edges.push_back(e);
		}
	}
	for (int f : to_vec(p_state.get("sel_faces", PackedInt32Array()))) {
		if (f >= 0 && f < nf) {
			selection.faces.push_back(f);
		}
	}
	if (old_shape != shape) {
		notify_property_list_changed();
	}
	update_gizmos();
}

Dictionary HewnMesh3D::get_selection_state() const {
	Dictionary d;
	d["sel_verts"] = to_packed(selection.verts);
	d["sel_edges"] = edges_to_packed(selection.edges);
	d["sel_faces"] = to_packed(selection.faces);
	return d;
}

void HewnMesh3D::set_selection_state(const Dictionary &p_state) {
	selection.clear();
	const int nv = (int)data.pos.size(), nf = (int)data.faces.size();
	for (int v : to_vec(p_state.get("sel_verts", PackedInt32Array()))) {
		if (v >= 0 && v < nv) {
			selection.verts.push_back(v);
		}
	}
	for (const hewn::Edge &e : to_edges(p_state.get("sel_edges", PackedInt32Array()))) {
		if (e.a >= 0 && e.a < nv && e.b >= 0 && e.b < nv) {
			selection.edges.push_back(e);
		}
	}
	for (int f : to_vec(p_state.get("sel_faces", PackedInt32Array()))) {
		if (f >= 0 && f < nf) {
			selection.faces.push_back(f);
		}
	}
	update_gizmos();
	emit_signal("selection_restored");
}

// ------------------------------------------------------------------ selection queries

PackedInt32Array HewnMesh3D::get_edge_loop(int p_a, int p_b) const {
	hewn::Topology t;
	t.build(data);
	return edges_to_packed(hewn::sel::edge_loop(data, t, hewn::Edge(p_a, p_b)));
}

PackedInt32Array HewnMesh3D::get_edge_ring(int p_a, int p_b) const {
	hewn::Topology t;
	t.build(data);
	return edges_to_packed(hewn::sel::edge_ring(data, t, hewn::Edge(p_a, p_b)));
}

PackedInt32Array HewnMesh3D::get_face_loop(int p_face, int p_a, int p_b) const {
	hewn::Topology t;
	t.build(data);
	return to_packed(hewn::sel::face_loop(data, t, p_face, hewn::Edge(p_a, p_b)));
}

PackedInt32Array HewnMesh3D::grow_faces(const PackedInt32Array &p_faces, float p_max_angle) const {
	hewn::Topology t;
	t.build(data);
	return to_packed(hewn::sel::grow_faces(data, t, to_vec(p_faces), p_max_angle));
}

PackedInt32Array HewnMesh3D::get_linked_faces(const PackedInt32Array &p_faces) const {
	hewn::Topology t;
	t.build(data);
	return to_packed(hewn::sel::linked_faces(data, t, to_vec(p_faces)));
}

// ------------------------------------------------------------------ materials & collision

void HewnMesh3D::set_materials(const TypedArray<Material> &p_materials) {
	materials = p_materials;
	rebuild();
}

TypedArray<Material> HewnMesh3D::get_materials() const {
	return materials;
}

Ref<Material> HewnMesh3D::get_material_for_slot(int p_slot) const {
	if (p_slot >= 0 && p_slot < materials.size()) {
		Ref<Material> m = materials[p_slot];
		if (m.is_valid()) {
			return m;
		}
	} else if (p_slot >= materials.size() && p_slot - materials.size() < (int64_t)csg_materials.size()) {
		Ref<Material> m = csg_materials[p_slot - materials.size()]; // a brush's material
		if (m.is_valid()) {
			return m;
		}
	}
	return get_default_material();
}

int HewnMesh3D::add_material_slot(const Ref<Material> &p_material) {
	for (int i = 0; i < materials.size(); i++) {
		if (Ref<Material>(materials[i]) == p_material) {
			return i;
		}
	}
	materials.push_back(p_material);
	return materials.size() - 1;
}

void HewnMesh3D::set_collision_mode(CollisionMode p_mode) {
	collision_mode = (CollisionMode)CLAMP((int)p_mode, 0, (int)COLLISION_SIMPLIFIED_TRIMESH);
	notify_property_list_changed(); // (each mode its own fields)
	if (is_inside_tree()) {
		_update_collision();
	}
}

void HewnMesh3D::set_collision_ratio(float p_value) {
	collision_ratio = CLAMP(p_value, 0.01f, 1.0f);
	if (is_inside_tree()) {
		_update_collision();
	}
}

void HewnMesh3D::set_collision_max_pieces(int p_value) {
	collision_max_pieces = CLAMP(p_value, 1, 64);
	if (is_inside_tree()) {
		_update_collision();
	}
}

void HewnMesh3D::set_collision_axis(int p_value) {
	collision_axis = CLAMP(p_value, 0, 3);
	if (is_inside_tree()) {
		_update_collision();
	}
}

void HewnMesh3D::set_collision_metadata(const Dictionary &p_value) {
	collision_metadata = p_value;
	if (body) {
		apply_collision_tags(body);
	}
}

void HewnMesh3D::set_collision_groups(const PackedStringArray &p_value) {
	collision_groups = p_value;
	if (body) {
		apply_collision_tags(body);
	}
}

void HewnMesh3D::apply_collision_tags(Node *p_body) const {
	if (!p_body) {
		return;
	}
	const Array keys = collision_metadata.keys();
	for (int64_t i = 0; i < keys.size(); i++) {
		p_body->set_meta(StringName(String(keys[i])), collision_metadata[keys[i]]);
	}
	for (int64_t i = 0; i < collision_groups.size(); i++) {
		if (!collision_groups[i].strip_edges().is_empty()) {
			p_body->add_to_group(StringName(collision_groups[i].strip_edges()), true);
		}
	}
}

bool HewnMesh3D::can_primitive_collision() const {
	return shape == hewn::shapes::CUBE || shape == hewn::shapes::CYLINDER || shape == hewn::shapes::SPHERE || shape == hewn::shapes::CAPSULE;
}

// A parametric shape's own collision: its parameters exactly (an oval cylinder / capsule: no).
bool HewnMesh3D::_primitive_collision(std::vector<CollisionPart> &r_parts, const Vector3 &p_mid) const {
	const Transform3D at(Basis(), p_mid); // (the shape centered on its triangles)
	if (shape == hewn::shapes::CUBE) {
		Ref<BoxShape3D> b;
		b.instantiate();
		b->set_size(Vector3(get_shape_param("width"), get_shape_param("height"), get_shape_param("depth")));
		r_parts.push_back({ b, at });
		return true;
	}
	if (shape == hewn::shapes::SPHERE) {
		Ref<SphereShape3D> sp;
		sp.instantiate();
		sp->set_radius(get_shape_param("radius"));
		r_parts.push_back({ sp, at });
		return true;
	}
	const float rz = get_shape_param("radius_z");
	if (shape == hewn::shapes::CYLINDER && (rz <= 0.0f || std::fabs(rz - get_shape_param("radius")) < 1e-5f)) {
		Ref<CylinderShape3D> c;
		c.instantiate();
		c->set_radius(get_shape_param("radius"));
		c->set_height(get_shape_param("height"));
		r_parts.push_back({ c, at });
		return true;
	}
	if (shape == hewn::shapes::CAPSULE && (rz <= 0.0f || std::fabs(rz - get_shape_param("radius")) < 1e-5f)) {
		Ref<CapsuleShape3D> c;
		c.instantiate();
		c->set_radius(get_shape_param("radius"));
		c->set_height(MAX(get_shape_param("height"), get_shape_param("radius") * 2.0f));
		r_parts.push_back({ c, at });
		return true;
	}
	return false;
}

Array HewnMesh3D::get_collision_shapes() const {
	Array out;
	for (const CollisionPart &p : collision_parts) {
		Dictionary d;
		d["shape"] = p.shape;
		d["transform"] = p.xf;
		out.push_back(d);
	}
	return out;
}

HewnMesh3D::CollisionMode HewnMesh3D::get_collision_mode() const {
	return collision_mode;
}

// ------------------------------------------------------------------ script API: elements

int HewnMesh3D::get_vertex_count() const {
	return (int)data.pos.size();
}

int HewnMesh3D::get_face_count() const {
	return (int)data.faces.size();
}

PackedVector3Array HewnMesh3D::get_vertices() const {
	PackedVector3Array out;
	for (const Vector3 &p : data.pos) {
		out.push_back(p);
	}
	return out;
}

void HewnMesh3D::set_vertices(const PackedVector3Array &p_vertices, bool p_positions_only) {
	for (int64_t i = 0; i < p_vertices.size() && i < (int64_t)data.pos.size(); i++) {
		data.pos[i] = p_vertices[i];
	}
	if (p_positions_only) {
		commit_positions(); // (finish_positions() later makes the whole of it)
	} else {
		commit();
	}
}

Vector3 HewnMesh3D::get_vertex(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)data.pos.size(), Vector3());
	return data.pos[p_index];
}

void HewnMesh3D::set_vertex(int p_index, const Vector3 &p_pos) {
	ERR_FAIL_INDEX(p_index, (int)data.pos.size());
	data.pos[p_index] = p_pos;
	commit();
}

PackedInt32Array HewnMesh3D::get_face(int p_face) const {
	ERR_FAIL_INDEX_V(p_face, (int)data.faces.size(), PackedInt32Array());
	return to_packed(data.faces[p_face].v);
}

int HewnMesh3D::add_vertex(const Vector3 &p_pos) {
	return data.add_vertex(p_pos);
}

int HewnMesh3D::add_face(const PackedInt32Array &p_indices, int p_material) {
	ERR_FAIL_COND_V(p_indices.size() < 3, -1);
	hewn::Face f = hewn::make_face(to_vec(p_indices));
	f.material = p_material;
	for (int v : f.v) {
		ERR_FAIL_INDEX_V(v, (int)data.pos.size(), -1);
	}
	data.faces.push_back(f);
	commit();
	return (int)data.faces.size() - 1;
}

void HewnMesh3D::clear_mesh() {
	data.clear();
	selection.clear();
	commit();
}

int HewnMesh3D::get_face_role(int p_face) const {
	ERR_FAIL_INDEX_V(p_face, (int)data.faces.size(), 0);
	return data.faces[p_face].role;
}

// A material on faces (all of them when p_faces is empty), like painting a texture: the slots are
// bookkeeping (found or added here, the unused ones dropped).
void HewnMesh3D::apply_material(const Ref<Material> &p_material, const PackedInt32Array &p_faces) {
	int used = (int)materials.size();
	for (const hewn::Face &f : data.faces) {
		used = MAX(used, f.material + 1);
	}
	while (materials.size() < used) {
		materials.push_back(Ref<Material>()); // slots the faces use without a material (the default one)
	}
	int slot = -1;
	for (int i = 0; i < materials.size(); i++) {
		if (Ref<Material>(materials[i]) == p_material) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		materials.push_back(p_material);
		slot = materials.size() - 1;
	}
	if (p_faces.is_empty()) {
		for (hewn::Face &f : data.faces) {
			f.material = slot;
		}
		center_auto_textures(); // (a parametric shape: its new texture's middle on the faces' middle)
	} else {
		for (int64_t i = 0; i < p_faces.size(); i++) {
			if (p_faces[i] >= 0 && p_faces[i] < (int)data.faces.size()) {
				data.faces[p_faces[i]].material = slot;
			}
		}
	}
	compact_materials();
}

// Drops the slots no face uses; equal materials share one slot.
void HewnMesh3D::compact_materials() {
	int count = MAX(1, (int)materials.size());
	for (const hewn::Face &f : data.faces) {
		count = MAX(count, f.material + 1);
	}
	std::vector<int> remap(count, -1);
	TypedArray<Material> kept;
	std::vector<char> used(remap.size(), 0);
	for (const hewn::Face &f : data.faces) {
		if (f.material >= 0 && f.material < (int)used.size()) {
			used[f.material] = 1;
		}
	}
	for (int i = 0; i < (int)remap.size(); i++) {
		if (!used[i]) {
			continue;
		}
		const Ref<Material> m = i < materials.size() ? Ref<Material>(materials[i]) : Ref<Material>();
		for (int j = 0; j < kept.size(); j++) {
			if (Ref<Material>(kept[j]) == m) {
				remap[i] = j;
				break;
			}
		}
		if (remap[i] < 0) {
			kept.push_back(m);
			remap[i] = kept.size() - 1;
		}
	}
	for (hewn::Face &f : data.faces) {
		f.material = f.material >= 0 && f.material < (int)remap.size() && remap[f.material] >= 0 ? remap[f.material] : 0;
	}
	materials = kept;
	rebuild();
}

int HewnMesh3D::get_face_material(int p_face) const {
	ERR_FAIL_INDEX_V(p_face, (int)data.faces.size(), 0);
	return data.faces[p_face].material;
}

void HewnMesh3D::set_face_material(int p_face, int p_material) {
	ERR_FAIL_INDEX(p_face, (int)data.faces.size());
	data.faces[p_face].material = MAX(0, p_material);
	commit(false);
}

int HewnMesh3D::get_face_smoothing(int p_face) const {
	ERR_FAIL_INDEX_V(p_face, (int)data.faces.size(), 0);
	return data.faces[p_face].smooth;
}

void HewnMesh3D::set_face_smoothing(int p_face, int p_group) {
	ERR_FAIL_INDEX(p_face, (int)data.faces.size());
	data.faces[p_face].smooth = MAX(0, p_group);
	commit(false);
}

void HewnMesh3D::set_face_color(int p_face, const Color &p_color) {
	ERR_FAIL_INDEX(p_face, (int)data.faces.size());
	hewn::Face &f = data.faces[p_face];
	f.ensure_attrs();
	for (Color &c : f.col) {
		c = p_color;
	}
	commit(false);
}

Vector3 HewnMesh3D::get_face_normal(int p_face) const {
	ERR_FAIL_INDEX_V(p_face, (int)data.faces.size(), Vector3());
	return hewn::face_normal(data, data.faces[p_face]);
}

Vector3 HewnMesh3D::get_face_center(int p_face) const {
	ERR_FAIL_INDEX_V(p_face, (int)data.faces.size(), Vector3());
	return hewn::face_center(data, data.faces[p_face]);
}

// ------------------------------------------------------------------ script API: operations

#define HW_FINISH()                                \
	hewn::remove_unused_vertices(data, &selection); \
	commit()

PackedInt32Array HewnMesh3D::extrude_faces(const PackedInt32Array &p_faces, float p_distance, bool p_individual, bool p_dissolve_orthogonal) {
	const int nf0 = (int)data.faces.size();
	std::vector<int> r = hewn::ops::extrude_faces(data, to_vec(p_faces), p_distance, p_individual);
	if (p_dissolve_orthogonal && !p_individual) {
		hewn::ops::dissolve_orthogonal(data, nf0, r);
	}
	HW_FINISH();
	return to_packed(r);
}

PackedInt32Array HewnMesh3D::inset_faces(const PackedInt32Array &p_faces, float p_amount, bool p_individual) {
	std::vector<int> r = hewn::ops::inset_faces(data, to_vec(p_faces), p_amount, p_individual);
	HW_FINISH();
	return to_packed(r);
}

void HewnMesh3D::delete_faces(const PackedInt32Array &p_faces) {
	hewn::ops::delete_faces(data, to_vec(p_faces));
	selection.faces.clear();
	HW_FINISH();
}

void HewnMesh3D::flip_faces(const PackedInt32Array &p_faces) {
	hewn::ops::flip_faces(data, to_vec(p_faces));
	commit();
}

PackedInt32Array HewnMesh3D::subdivide_faces(const PackedInt32Array &p_faces) {
	std::vector<int> r = hewn::ops::subdivide_faces(data, to_vec(p_faces));
	HW_FINISH();
	return to_packed(r);
}

PackedInt32Array HewnMesh3D::subdivide_faces_ex(const PackedInt32Array &p_faces, int p_cuts, float p_smoothness, bool p_ngons, float p_fractal, float p_along_normal, int p_seed) {
	std::vector<int> r = hewn::ops::subdivide_faces_ex(data, to_vec(p_faces), p_cuts, p_smoothness, p_ngons, p_fractal, p_along_normal, p_seed);
	HW_FINISH();
	return to_packed(r);
}

PackedInt32Array HewnMesh3D::triangulate_faces(const PackedInt32Array &p_faces) {
	std::vector<int> r = hewn::ops::triangulate_faces(data, to_vec(p_faces));
	HW_FINISH();
	return to_packed(r);
}

PackedInt32Array HewnMesh3D::merge_faces(const PackedInt32Array &p_faces) {
	std::vector<int> r = hewn::ops::merge_faces(data, to_vec(p_faces));
	HW_FINISH();
	return to_packed(r);
}

PackedInt32Array HewnMesh3D::detach_faces(const PackedInt32Array &p_faces) {
	std::vector<int> r = hewn::ops::detach_faces(data, to_vec(p_faces));
	HW_FINISH();
	return to_packed(r);
}

void HewnMesh3D::conform_normals() {
	hewn::ops::conform_normals(data, {});
	commit();
}

void HewnMesh3D::recalculate_normals(bool p_inside) {
	hewn::ops::recalc_normals(data, {}, p_inside);
	commit();
}

PackedInt32Array HewnMesh3D::bisect(const Vector3 &p_point, const Vector3 &p_normal, bool p_clear_inner, bool p_clear_outer, bool p_fill) {
	std::vector<hewn::Edge> r = hewn::ops::bisect(data, {}, p_point, p_normal, p_clear_inner, p_clear_outer, p_fill);
	HW_FINISH();
	return edges_to_packed(r);
}

bool HewnMesh3D::move_face_plane(int p_face, float p_delta, bool p_collapse) {
	hewn::Mesh planes, out;
	int pf = -1;
	if (!hewn::ops::convex_plane_mesh(data, p_face, planes, pf) || !hewn::ops::move_face_plane(planes, pf, p_delta, out, p_collapse)) {
		return false;
	}
	const bool same = out.faces.size() == data.faces.size();
	data = out;
	selection.verts.clear();
	selection.edges.clear();
	if (!same) {
		selection.faces.clear();
	}
	HW_FINISH();
	return true;
}

// ------------------------------------------------------------------ level design

std::vector<std::vector<int>> HewnMesh3D::unpack_loops(const PackedInt32Array &p_packed) {
	std::vector<std::vector<int>> out;
	for (int64_t i = 0; i < p_packed.size();) {
		const int n = p_packed[i++];
		std::vector<int> loop;
		for (int k = 0; k < n && i < p_packed.size(); k++) {
			loop.push_back(p_packed[i++]);
		}
		out.push_back(loop);
	}
	return out;
}

PackedInt32Array HewnMesh3D::cut_parts(const std::vector<std::vector<hewn::ops::CutPlane>> &p_parts, const PackedInt32Array &p_faces) {
	const std::vector<std::vector<int>> loops = hewn::ops::cut_volumes(data, to_vec(p_faces), p_parts);
	// unused vertices go: the hole loops follow the new indices
	std::vector<char> used(data.pos.size(), 0);
	for (const hewn::Face &f : data.faces) {
		for (int v : f.v) {
			used[v] = 1;
		}
	}
	for (const std::pair<int, int> &w : data.wires) {
		used[w.first] = used[w.second] = 1;
	}
	for (int v : data.points) {
		used[v] = 1;
	}
	std::vector<int> map(data.pos.size(), -1);
	int k = 0;
	for (size_t i = 0; i < used.size(); i++) {
		map[i] = used[i] ? k++ : -1;
	}
	PackedInt32Array out;
	for (const std::vector<int> &loop : loops) {
		out.push_back((int)loop.size());
		for (int v : loop) {
			out.push_back(map[v]);
		}
	}
	selection.clear();
	HW_FINISH();
	return out;
}

PackedInt32Array HewnMesh3D::cut_box(const AABB &p_box, const PackedInt32Array &p_faces) {
	// flush sides: faces lying on them (a floor under a door) stay
	const Vector3 lo = p_box.position, hi = p_box.get_end();
	std::vector<hewn::ops::CutPlane> planes;
	for (int a = 0; a < 3; a++) {
		Vector3 n;
		n[a] = 1.0f;
		planes.push_back({ n, hi[a] });
		planes.push_back({ -n, -lo[a] });
	}
	return cut_parts({ planes }, p_faces);
}

PackedInt32Array HewnMesh3D::bridge_holes(const PackedInt32Array &p_a, const PackedInt32Array &p_b) {
	const std::vector<int> r = hewn::ops::bridge_loops(data, to_vec(p_a), to_vec(p_b));
	commit();
	return to_packed(r);
}

PackedInt32Array HewnMesh3D::make_alcove(const PackedInt32Array &p_loop, float p_depth) {
	const std::vector<int> r = hewn::ops::alcove(data, to_vec(p_loop), p_depth);
	commit();
	return to_packed(r);
}

bool HewnMesh3D::wrap_texture(int p_from, int p_to, bool p_material) {
	const bool ok = hewn::wrap_uv(data, p_from, p_to, uv_transform(), p_material);
	rebuild();
	return ok;
}

PackedVector2Array HewnMesh3D::get_face_uvs(int p_face) const {
	PackedVector2Array out;
	std::vector<std::vector<Vector2>> uvs;
	hewn::compute_uvs(data, uv_transform(), uvs);
	if (p_face >= 0 && p_face < (int)uvs.size()) {
		for (const Vector2 &v : uvs[p_face]) {
			out.push_back(v);
		}
	}
	return out;
}

namespace {
// The material's tiling (pixels per meter): one texture tile is 1 / uv1_scale in face UVs.
Vector2 tile_of(const Ref<Material> &p_material) {
	Ref<BaseMaterial3D> bm = p_material;
	return bm.is_valid() ? Vector2(MAX(1e-6f, bm->get_uv1_scale().x), MAX(1e-6f, bm->get_uv1_scale().y)) : Vector2(1, 1);
}
} // namespace

Rect2 HewnMesh3D::get_uv_bounds(const PackedInt32Array &p_faces) const {
	std::vector<std::vector<Vector2>> uvs;
	hewn::compute_uvs(data, uv_transform(), uvs);
	std::vector<int> fs = to_vec(p_faces);
	if (fs.empty()) {
		for (int f = 0; f < (int)data.faces.size(); f++) {
			fs.push_back(f);
		}
	}
	Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
	for (int f : fs) {
		if (f < 0 || f >= (int)uvs.size()) {
			continue;
		}
		const Vector2 ms = tile_of(get_material_for_slot(data.faces[f].material));
		for (const Vector2 &q : uvs[f]) {
			mn = mn.min(q * ms);
			mx = mx.max(q * ms);
		}
	}
	return mn.x > mx.x ? Rect2() : Rect2(mn, mx - mn);
}

bool HewnMesh3D::edit_uvs(const PackedInt32Array &p_faces, const String &p_op, const Array &p_args) {
	const Transform3D xf = uv_transform();
	std::vector<int> fs;
	for (int f : to_vec(p_faces)) {
		if (f >= 0 && f < (int)data.faces.size()) {
			fs.push_back(f);
		}
	}
	if (p_faces.is_empty()) {
		for (int f = 0; f < (int)data.faces.size(); f++) {
			fs.push_back(f);
		}
	}
	if (fs.empty()) {
		return false;
	}
	auto arg = [&](int i, float def) { return i < p_args.size() ? (float)(double)p_args[i] : def; };
	std::vector<std::vector<Vector2>> uvs;
	hewn::compute_uvs(data, xf, uvs);
	// in tiles
	std::vector<Vector2> ms(fs.size());
	Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
	for (size_t i = 0; i < fs.size(); i++) {
		ms[i] = tile_of(get_material_for_slot(data.faces[fs[i]].material));
		for (Vector2 &q : uvs[fs[i]]) {
			q *= ms[i];
			mn = mn.min(q);
			mx = mx.max(q);
		}
	}
	const Vector2 size = mx - mn;
	Vector2 c = (mn + mx) * 0.5f;
	const bool rotate_like = p_op == "rotate" || p_op == "cycle";
	if ((rotate_like && p_args.size() >= 3) || ((p_op == "scale") && p_args.size() >= 4)) {
		c = rotate_like ? Vector2(arg(1, c.x), arg(2, c.y)) : Vector2(arg(2, c.x), arg(3, c.y));
	} else if ((p_op == "mirror_u" || p_op == "mirror_v") && p_args.size() >= 2) {
		c = Vector2(arg(0, c.x), arg(1, c.y));
	}
	auto rot = [&](const Vector2 &q, float deg) {
		const float a = deg * (float)(Math::PI / 180.0);
		const Vector2 d = q - c;
		return c + Vector2(d.x * std::cos(a) - d.y * std::sin(a), d.x * std::sin(a) + d.y * std::cos(a));
	};
	auto tile_edge = [](float lo, float hi, float h, bool center) {
		const float t = std::floor((lo + hi) * 0.5f); // the tile the middle is in
		if (h < 0.0f) {
			return t - lo;
		}
		if (h > 0.0f) {
			return t + 1.0f - hi;
		}
		return center ? t + 0.5f - (lo + hi) * 0.5f : 0.0f;
	};
	for (size_t i = 0; i < fs.size(); i++) {
		std::vector<Vector2> &u = uvs[fs[i]];
		if (p_op == "rotate") {
			// the texture turns on the face: its parameters, so a scale that differs per
			// axis does not shear it and it keeps following the geometry
			std::vector<Vector2> fu = u;
			for (Vector2 &q : fu) {
				q /= ms[i];
			}
			hewn::set_face_uvs(data, fs[i], xf, fu);
			hewn::Face &F = data.faces[fs[i]];
			if (!F.manual_uv) {
				const Vector2 cf = c / ms[i];
				const float a = F.uvp.rotation * (float)(Math::PI / 180.0), phi = arg(0, 90) * (float)(Math::PI / 180.0);
				auto r2 = [](const Vector2 &p, float ang) { return Vector2(p.x * std::cos(ang) - p.y * std::sin(ang), p.x * std::sin(ang) + p.y * std::cos(ang)); };
				const Vector2 s = F.uvp.scale;
				const Vector2 d = cf - F.uvp.offset;
				const Vector2 raw_c = r2(Vector2(std::fabs(s.x) > 1e-9f ? d.x / s.x : 0.0f, std::fabs(s.y) > 1e-9f ? d.y / s.y : 0.0f), -a);
				const Vector2 q = r2(raw_c, a + phi);
				F.uvp.rotation += arg(0, 90);
				F.uvp.offset = cf - Vector2(q.x * s.x, q.y * s.y);
				continue;
			}
		}
		Vector2 fmn(1e30f, 1e30f), fmx(-1e30f, -1e30f);
		for (const Vector2 &q : u) {
			fmn = fmn.min(q);
			fmx = fmx.max(q);
		}
		const Vector2 fsz = fmx - fmn;
		for (Vector2 &q : u) {
			if (p_op == "move") {
				q += Vector2(arg(0, 0), arg(1, 0));
			} else if (p_op == "rotate") {
				q = rot(q, arg(0, 90));
			} else if (p_op == "scale") {
				q = c + (q - c) * Vector2(arg(0, 1), arg(1, arg(0, 1)));
			} else if (p_op == "mirror_u") {
				q.x = 2.0f * c.x - q.x;
			} else if (p_op == "mirror_v") {
				q.y = 2.0f * c.y - q.y;
			} else if (p_op == "cycle") {
				// a quarter turn that stays inside the same bounds
				q = rot(q, arg(0, 90));
				const Vector2 s(size.y > 1e-9f ? size.x / size.y : 1.0f, size.x > 1e-9f ? size.y / size.x : 1.0f);
				q = c + (q - c) * s;
			} else if (p_op == "edge") {
				q += Vector2(tile_edge(mn.x, mx.x, arg(0, 0), false), tile_edge(mn.y, mx.y, arg(1, 0), false));
			} else if (p_op == "justify") {
				q += Vector2(tile_edge(fmn.x, fmx.x, arg(0, 0), true), tile_edge(fmn.y, fmx.y, arg(1, 0), true));
			} else if (p_op == "fit") {
				const bool fu = arg(0, 1) != 0.0f, fv = arg(1, 1) != 0.0f;
				if (fu && fsz.x > 1e-9f) {
					q.x = std::floor(fmn.x) + (q.x - fmn.x) / fsz.x;
				}
				if (fv && fsz.y > 1e-9f) {
					q.y = std::floor(fmn.y) + (q.y - fmn.y) / fsz.y;
				}
			} else if (p_op == "inset") {
				// pixels of the face's texture on each side
				Vector2 tex(1024, 1024);
				Ref<BaseMaterial3D> bm = get_material_for_slot(data.faces[fs[i]].material);
				if (bm.is_valid() && bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO).is_valid()) {
					tex = bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO)->get_size();
				}
				const Vector2 d = Vector2(arg(0, 1), arg(0, 1)) / tex.max(Vector2(1, 1));
				const Vector2 s((size.x > 1e-9f) ? (size.x - 2.0f * d.x) / size.x : 1.0f, (size.y > 1e-9f) ? (size.y - 2.0f * d.y) / size.y : 1.0f);
				q = c + (q - c) * s;
			} else if (p_op == "transfer") {
				const Vector2 r0(arg(0, 0), arg(1, 0)), r1(arg(2, 1), arg(3, 1));
				q = r0 + Vector2(size.x > 1e-9f ? (q.x - mn.x) / size.x : 0.0f, size.y > 1e-9f ? (q.y - mn.y) / size.y : 0.0f) * (r1 - r0);
			} else {
				return false;
			}
		}
		for (Vector2 &q : u) {
			q /= ms[i];
		}
		hewn::set_face_uvs(data, fs[i], xf, u);
	}
	rebuild();
	return true;
}

PackedInt32Array HewnMesh3D::unwrap_quads(const PackedInt32Array &p_faces, int p_start) {
	std::vector<int> fs = to_vec(p_faces);
	if (fs.empty()) {
		for (int f = 0; f < (int)data.faces.size(); f++) {
			fs.push_back(f);
		}
	}
	const std::vector<int> done = hewn::unwrap_quads(data, fs, p_start);
	if (done.empty()) {
		return PackedInt32Array();
	}
	// fitted to one tile (a trim strip runs along U)
	Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
	for (int f : done) {
		for (const Vector2 &q : data.faces[f].uv) {
			mn = mn.min(q);
			mx = mx.max(q);
		}
	}
	const Vector2 sz = (mx - mn).max(Vector2(1e-6f, 1e-6f));
	for (int f : done) {
		const Vector2 ms = tile_of(get_material_for_slot(data.faces[f].material));
		for (Vector2 &q : data.faces[f].uv) {
			q = (q - mn) / sz / ms;
		}
	}
	rebuild();
	return to_packed(done);
}

int HewnMesh3D::join_mesh(HewnMesh3D *p_other, bool p_keep_overlap) {
	if (!p_other || p_other == this) {
		return -1;
	}
	hewn::Mesh add = p_other->get_data();
	const Transform3D mine = is_inside_tree() ? get_global_transform() : get_transform();
	const Transform3D theirs = p_other->is_inside_tree() ? p_other->get_global_transform() : p_other->get_transform();
	hewn::ops::transform(add, mine.affine_inverse() * theirs);
	for (hewn::Face &F : add.faces) {
		F.material = add_material_slot(p_other->get_material_for_slot(F.material));
	}
	if (!p_keep_overlap) {
		hewn::ops::merge_touching(data, add, 1e-3f);
	}
	const int first = (int)data.faces.size();
	hewn::ops::append(data, add, Transform3D(), 0);
	hewn::weld_by_distance(data, 1e-4f);
	hewn::remove_unused_vertices(data);
	selection.clear();
	commit(true);
	return first;
}

PackedInt32Array HewnMesh3D::fill_hole(const PackedInt32Array &p_loop) {
	const std::vector<int> r = hewn::ops::fill_loop(data, to_vec(p_loop));
	commit();
	return to_packed(r);
}

PackedInt32Array HewnMesh3D::poke_faces(const PackedInt32Array &p_faces) {
	std::vector<int> r = hewn::ops::poke_faces(data, to_vec(p_faces));
	HW_FINISH();
	return to_packed(r);
}

PackedInt32Array HewnMesh3D::bridge_faces(const PackedInt32Array &p_faces) {
	std::vector<int> r = hewn::ops::bridge_faces(data, to_vec(p_faces));
	HW_FINISH();
	return to_packed(r);
}

PackedInt32Array HewnMesh3D::split_edges(const PackedInt32Array &p_edge_pairs) {
	std::vector<int> r = hewn::ops::split_edges(data, to_edges(p_edge_pairs));
	HW_FINISH();
	return to_packed(r);
}

PackedInt32Array HewnMesh3D::bevel_vertices(const PackedInt32Array &p_vertices, float p_amount) {
	std::vector<int> r = hewn::ops::bevel_vertices(data, to_vec(p_vertices), p_amount);
	HW_FINISH();
	return to_packed(r);
}

void HewnMesh3D::set_edge_attributes(int p_a, int p_b, bool p_seam, bool p_sharp, float p_crease, float p_bevel_weight) {
	hewn::EdgeAttr ea;
	ea.seam = p_seam;
	ea.sharp = p_sharp;
	ea.crease = CLAMP(p_crease, 0.0f, 1.0f);
	ea.bevel_weight = CLAMP(p_bevel_weight, 0.0f, 1.0f);
	if (ea.empty()) {
		data.edge_attrs.erase(hewn::ekey(p_a, p_b));
	} else {
		data.edge_attrs[hewn::ekey(p_a, p_b)] = ea;
	}
	commit(false);
}

Dictionary HewnMesh3D::get_edge_attributes(int p_a, int p_b) const {
	Dictionary d;
	const hewn::EdgeAttr *ea = data.edge_attr(p_a, p_b);
	d["seam"] = ea ? ea->seam : false;
	d["sharp"] = ea ? ea->sharp : false;
	d["crease"] = ea ? ea->crease : 0.0f;
	d["bevel_weight"] = ea ? ea->bevel_weight : 0.0f;
	return d;
}

int HewnMesh3D::limited_dissolve(float p_angle) {
	const int r = hewn::ops::limited_dissolve(data, {}, p_angle);
	HW_FINISH();
	return r;
}

int HewnMesh3D::make_face(const PackedInt32Array &p_vertices) {
	const int f = hewn::ops::make_face_from(data, to_vec(p_vertices));
	commit();
	return f;
}

PackedInt32Array HewnMesh3D::bevel_edges(const PackedInt32Array &p_edge_pairs, float p_amount, int p_segments) {
	std::vector<int> r = hewn::ops::bevel_edges(data, to_edges(p_edge_pairs), p_amount, p_segments);
	selection.edges.clear();
	HW_FINISH();
	return to_packed(r);
}

PackedInt32Array HewnMesh3D::arch_edges(const PackedInt32Array &p_edge_pairs, int p_segments, float p_rise) {
	const std::vector<hewn::Edge> r = hewn::ops::arch_edges(data, to_edges(p_edge_pairs), p_segments, p_rise);
	selection.clear();
	commit();
	return edges_to_packed(r);
}

void HewnMesh3D::sweep_along(Node3D *p_path) {
	Path3D *path = Object::cast_to<Path3D>(p_path);
	ERR_FAIL_COND_MSG(!path || path->get_curve().is_null(), "sweep_along needs a Path3D with a curve");
	const PackedVector3Array pts = path->get_curve()->tessellate(5, 3.0);
	const Transform3D rel = (is_inside_tree() && path->is_inside_tree()) ? get_global_transform().affine_inverse() * path->get_global_transform() : Transform3D();
	PackedVector3Array local;
	for (int64_t i = 0; i < pts.size(); i++) {
		local.push_back(rel.xform(pts[i]));
	}
	poly_points = local;
	shape = hewn::shapes::SWEEP;
	_regenerate_shape();
	rebuild();
	notify_property_list_changed();
}

PackedInt32Array HewnMesh3D::loop_cut(int p_a, int p_b, int p_cuts, float p_t) {
	std::vector<hewn::Edge> r = hewn::ops::loop_cut(data, hewn::Edge(p_a, p_b), p_cuts, p_a < p_b ? p_t : 1.0f - p_t);
	HW_FINISH();
	return edges_to_packed(r);
}

PackedInt32Array HewnMesh3D::knife_cut(const PackedVector3Array &p_points, const PackedInt32Array &p_faces, const Vector3 &p_view_dir) {
	std::vector<hewn::ops::KnifePoint> pts;
	for (int64_t i = 0; i < p_points.size() && i < p_faces.size(); i++) {
		pts.push_back(hewn::ops::KnifePoint{ p_faces[i], p_points[i] });
	}
	std::vector<Vector3> normals;
	for (size_t i = 0; i + 1 < pts.size(); i++) {
		normals.push_back((pts[i + 1].pos - pts[i].pos).cross(p_view_dir));
	}
	std::vector<hewn::Edge> r = hewn::ops::knife(data, pts, normals);
	HW_FINISH();
	return edges_to_packed(r);
}

bool HewnMesh3D::boolean_with(HewnMesh3D *p_other, int p_op) {
	ERR_FAIL_NULL_V(p_other, false);
	hewn::Mesh other;
	hewn::ops::append(other, p_other->get_data(), get_global_transform().affine_inverse() * p_other->get_global_transform(), 0);
	bool ok = false;
	hewn::Mesh r = hewn::csg::boolean(data, other, (hewn::csg::Op)CLAMP(p_op, 0, 2), &ok);
	if (!ok) {
		return false;
	}
	data = r;
	selection.clear();
	commit();
	return true;
}

PackedInt32Array HewnMesh3D::extrude_edges(const PackedInt32Array &p_edge_pairs, float p_distance) {
	std::vector<hewn::Edge> r = hewn::ops::extrude_edges(data, to_edges(p_edge_pairs), p_distance, false);
	HW_FINISH();
	return edges_to_packed(r);
}

PackedInt32Array HewnMesh3D::connect_edges(const PackedInt32Array &p_edge_pairs) {
	std::vector<hewn::Edge> r = hewn::ops::connect_edges(data, to_edges(p_edge_pairs));
	HW_FINISH();
	return edges_to_packed(r);
}

PackedInt32Array HewnMesh3D::insert_edge_loops(const PackedInt32Array &p_edge_pairs) {
	std::vector<hewn::Edge> r = hewn::ops::insert_edge_loops(data, to_edges(p_edge_pairs));
	HW_FINISH();
	return edges_to_packed(r);
}

PackedInt32Array HewnMesh3D::subdivide_edges(const PackedInt32Array &p_edge_pairs, int p_cuts) {
	std::vector<hewn::Edge> r = hewn::ops::subdivide_edges(data, to_edges(p_edge_pairs), p_cuts);
	HW_FINISH();
	return edges_to_packed(r);
}

PackedInt32Array HewnMesh3D::bridge_edges(const PackedInt32Array &p_edge_pairs) {
	std::vector<hewn::Edge> e = to_edges(p_edge_pairs);
	ERR_FAIL_COND_V(e.size() != 2, PackedInt32Array());
	std::vector<int> r = hewn::ops::bridge_edges(data, e[0], e[1]);
	commit();
	return to_packed(r);
}

Vector3 HewnMesh3D::get_shape_anchor() const {
	return shape == hewn::shapes::CUSTOM ? Vector3() : hewn::shapes::anchor_point((hewn::shapes::Type)shape, _shape_values());
}

PackedInt32Array HewnMesh3D::get_face_triangles(int p_face) const {
	PackedInt32Array out;
	if (p_face >= 0 && p_face < (int)last_build.face_tris.size() && p_face < (int)data.faces.size()) {
		for (int c : last_build.face_tris[p_face]) {
			out.push_back(data.faces[p_face].v[c]);
		}
	}
	return out;
}

void HewnMesh3D::dissolve_vertices(const PackedInt32Array &p_vertices) {
	hewn::ops::dissolve_vertices(data, to_vec(p_vertices));
	hewn::remove_unused_vertices(data);
	selection.clear();
	HW_FINISH();
}

int HewnMesh3D::to_circle(const PackedInt32Array &p_vertices, const PackedInt32Array &p_faces, bool p_regular, float p_influence) {
	const int r = hewn::ops::to_circle(data, to_vec(p_vertices), to_vec(p_faces), p_regular, p_influence);
	HW_FINISH();
	return r;
}

PackedInt32Array HewnMesh3D::dissolve_edges(const PackedInt32Array &p_edge_pairs) {
	std::vector<int> r = hewn::ops::dissolve_edges(data, to_edges(p_edge_pairs));
	selection.edges.clear();
	HW_FINISH();
	return to_packed(r);
}

PackedInt32Array HewnMesh3D::weld_vertices(const PackedInt32Array &p_vertices, float p_distance) {
	std::vector<int> r = hewn::ops::weld_vertices(data, to_vec(p_vertices), p_distance);
	hewn::Selection s;
	s.verts = r;
	hewn::remove_unused_vertices(data, &s);
	selection.clear();
	commit();
	return to_packed(s.verts);
}

PackedInt32Array HewnMesh3D::collapse_vertices(const PackedInt32Array &p_vertices) {
	std::vector<int> r = hewn::ops::collapse_groups(data, { to_vec(p_vertices) });
	hewn::Selection s;
	s.verts = r;
	hewn::remove_unused_vertices(data, &s);
	selection.clear();
	commit();
	return to_packed(s.verts);
}

PackedInt32Array HewnMesh3D::split_vertices(const PackedInt32Array &p_vertices) {
	std::vector<int> r = hewn::ops::split_vertices(data, to_vec(p_vertices));
	commit();
	return to_packed(r);
}

PackedInt32Array HewnMesh3D::connect_vertices(const PackedInt32Array &p_vertices) {
	std::vector<hewn::Edge> r = hewn::ops::connect_vertices(data, to_vec(p_vertices));
	HW_FINISH();
	return edges_to_packed(r);
}

PackedInt32Array HewnMesh3D::fill_holes() {
	std::vector<int> r = hewn::ops::fill_holes(data, {});
	commit();
	return to_packed(r);
}

void HewnMesh3D::subdivide_smooth() {
	hewn::ops::subdivide_smooth(data);
	selection.clear();
	commit();
}

void HewnMesh3D::auto_smooth(float p_angle) {
	hewn::ops::auto_smooth(data, p_angle, {});
	commit(false);
}

int HewnMesh3D::quadify(float p_angle) {
	const int r = hewn::ops::quadify(data, p_angle);
	selection.clear();
	commit();
	return r;
}

#undef HW_FINISH

// ------------------------------------------------------------------ object operations

void HewnMesh3D::freeze_transform() {
	const Transform3D xf = get_transform();
	hewn::ops::transform(data, Transform3D(xf.basis, xf.origin));
	set_transform(Transform3D());
	commit();
}

void HewnMesh3D::set_pivot(const Vector3 &p_local_point) {
	for (Vector3 &p : data.pos) {
		p -= p_local_point;
	}
	Transform3D xf = get_transform();
	xf.origin += xf.basis.xform(p_local_point);
	set_transform(xf);
	commit(false);
}

void HewnMesh3D::center_pivot() {
	if (data.pos.empty()) {
		return;
	}
	Vector3 mn = data.pos[0], mx = data.pos[0];
	for (const Vector3 &p : data.pos) {
		mn = mn.min(p);
		mx = mx.max(p);
	}
	set_pivot((mn + mx) * 0.5f);
}

void HewnMesh3D::merge_from(HewnMesh3D *p_other) {
	ERR_FAIL_NULL(p_other);
	Transform3D rel;
	if (is_inside_tree() && p_other->is_inside_tree()) {
		rel = get_global_transform().affine_inverse() * p_other->get_global_transform();
	} else {
		rel = get_transform().affine_inverse() * p_other->get_transform();
	}
	const hewn::Mesh &src = p_other->get_data();
	std::vector<int> slot_map;
	int max_slot = 0;
	for (const hewn::Face &f : src.faces) {
		max_slot = MAX(max_slot, f.material);
	}
	for (int i = 0; i <= max_slot; i++) {
		Ref<Material> m;
		if (i < p_other->materials.size()) {
			m = p_other->materials[i];
		}
		if (m.is_null()) {
			// Keep "default" slots pointing at an empty slot of ours.
			int empty = -1;
			for (int j = 0; j < materials.size(); j++) {
				if (Ref<Material>(materials[j]).is_null()) {
					empty = j;
					break;
				}
			}
			slot_map.push_back(empty >= 0 ? empty : (materials.size() == 0 ? 0 : add_material_slot(Ref<Material>())));
		} else {
			slot_map.push_back(add_material_slot(m));
		}
	}
	const size_t first = data.faces.size();
	hewn::ops::append(data, src, rel, 0);
	for (size_t f = first; f < data.faces.size(); f++) {
		int &m = data.faces[f].material;
		m = (m >= 0 && m < (int)slot_map.size()) ? slot_map[m] : 0;
	}
	commit();
}

Ref<ArrayMesh> HewnMesh3D::to_array_mesh() const {
	Ref<ArrayMesh> src = get_mesh();
	Ref<ArrayMesh> out;
	out.instantiate();
	if (src.is_null()) {
		return out;
	}
	for (int s = 0; s < src->get_surface_count(); s++) {
		out->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, src->surface_get_arrays(s));
		Ref<Material> m = src->surface_get_material(s);
		if (m == default_material) {
			m = m->duplicate();
		}
		out->surface_set_material(s, m);
	}
	return out;
}

String HewnMesh3D::to_obj() const {
	const hewn::Mesh data = evaluated_mesh(); // shadows the member: export what is shown
	hewn::BuildOut last_build;
	hewn::build_mesh(data, uv_transform(), last_build);
	String s = "# Hewn OBJ export\n";
	for (const Vector3 &p : data.pos) {
		s += vformat("v %f %f %f\n", p.x, p.y, p.z);
	}
	const std::vector<std::vector<Vector2>> &uvs = last_build.face_uvs;
	for (size_t f = 0; f < data.faces.size(); f++) {
		for (int i = 0; i < data.faces[f].size(); i++) {
			const Vector2 uv = f < uvs.size() && i < (int)uvs[f].size() ? uvs[f][i] : Vector2();
			s += vformat("vt %f %f\n", uv.x, 1.0 - uv.y);
		}
	}
	int vt = 1;
	int cur_mat = -1;
	for (const hewn::Face &f : data.faces) {
		if (f.material != cur_mat) {
			cur_mat = f.material;
			s += vformat("usemtl material_%d\n", cur_mat);
		}
		s += "f";
		for (int i = 0; i < f.size(); i++) {
			s += vformat(" %d/%d", f.v[i] + 1, vt++);
		}
		s += "\n";
	}
	return s;
}

// glTF 2.0 (.gltf with its .bin, or a single .glb): the mesh as shown (modifiers applied), with its
// materials and vertex colors, at the origin.
Error HewnMesh3D::export_gltf(const String &p_path) const {
	Ref<ArrayMesh> mesh = get_mesh();
	if (mesh.is_null() || mesh->get_surface_count() == 0) {
		return ERR_UNCONFIGURED;
	}
	Ref<ArrayMesh> copy = mesh->duplicate();
	if (get_material_override().is_valid()) {
		for (int i = 0; i < copy->get_surface_count(); i++) {
			copy->surface_set_material(i, get_material_override());
		}
	}
	Node3D *root = memnew(Node3D);
	root->set_name(String(get_name()).is_empty() ? String("Hewn") : String(get_name()));
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_name(root->get_name());
	mi->set_mesh(copy);
	root->add_child(mi);
	mi->set_owner(root);
	Ref<GLTFDocument> doc;
	doc.instantiate();
	Ref<GLTFState> state;
	state.instantiate();
	Error err = doc->append_from_scene(root, state);
	if (err == OK) {
		err = doc->write_to_filesystem(state, p_path);
	}
	memdelete(root);
	return err;
}

Error HewnMesh3D::export_obj(const String &p_path) const {
	Ref<FileAccess> fa = FileAccess::open(p_path, FileAccess::WRITE);
	if (fa.is_null()) {
		return FileAccess::get_open_error();
	}
	fa->store_string(to_obj());
	return OK;
}

void HewnMesh3D::create_from_mesh(const Ref<Mesh> &p_mesh, bool p_quadify) {
	ERR_FAIL_COND(p_mesh.is_null());
	data.clear();
	materials.clear();
	for (int s = 0; s < p_mesh->get_surface_count(); s++) {
		const Array arrays = p_mesh->surface_get_arrays(s);
		const PackedVector3Array v = arrays[Mesh::ARRAY_VERTEX];
		const PackedInt32Array idx = arrays[Mesh::ARRAY_INDEX];
		const PackedVector2Array uv = arrays[Mesh::ARRAY_TEX_UV];
		const PackedColorArray col = arrays[Mesh::ARRAY_COLOR];
		materials.push_back(p_mesh->surface_get_material(s));
		const int base = (int)data.pos.size();
		for (int64_t i = 0; i < v.size(); i++) {
			data.pos.push_back(v[i]);
		}
		const int64_t n = idx.size() > 0 ? idx.size() : v.size();
		for (int64_t t = 0; t + 2 < n; t += 3) {
			int c[3];
			for (int k = 0; k < 3; k++) {
				c[k] = idx.size() > 0 ? idx[t + k] : (int)(t + k);
			}
			// Godot fronts are clockwise; store counter-clockwise.
			const int order[3] = { c[0], c[2], c[1] };
			hewn::Face f;
			for (int k = 0; k < 3; k++) {
				f.v.push_back(base + order[k]);
				f.uv.push_back(order[k] < uv.size() ? uv[order[k]] : Vector2());
				f.col.push_back(order[k] < col.size() ? col[order[k]] : Color(1, 1, 1, 1));
			}
			f.manual_uv = uv.size() > 0;
			f.material = s;
			data.faces.push_back(f);
		}
	}
	hewn::weld_by_distance(data, 1e-5f);
	if (p_quadify) {
		hewn::ops::quadify(data, 1.0f);
	}
	// UVs that are the faces' own projection turned / scaled / moved (a baked Hewn mesh) become that
	// projection again: moved faces keep tiling instead of stretching
	for (int f = 0; f < (int)data.faces.size(); f++) {
		if (data.faces[f].manual_uv && data.faces[f].uv.size() == data.faces[f].v.size()) {
			const std::vector<Vector2> uv = data.faces[f].uv;
			hewn::set_face_uvs(data, f, Transform3D(), uv);
		}
	}
	selection.clear();
	shape = hewn::shapes::CUSTOM;
	notify_property_list_changed();
	rebuild();
}

String HewnMesh3D::get_info() const {
	int tris = 0;
	for (const hewn::Face &f : data.faces) {
		tris += MAX(0, f.size() - 2);
	}
	return vformat("%d vertices, %d faces, %d triangles", (int)data.pos.size(), (int)data.faces.size(), tris);
}

// ------------------------------------------------------------------ bindings

void HewnMesh3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_shape", "shape"), &HewnMesh3D::set_shape);
	ClassDB::bind_method(D_METHOD("get_shape"), &HewnMesh3D::get_shape);
	ClassDB::bind_method(D_METHOD("set_shape_params", "params"), &HewnMesh3D::set_shape_params);
	ClassDB::bind_method(D_METHOD("get_shape_params"), &HewnMesh3D::get_shape_params);
	ClassDB::bind_method(D_METHOD("set_poly_points", "points"), &HewnMesh3D::set_poly_points);
	ClassDB::bind_method(D_METHOD("get_poly_points"), &HewnMesh3D::get_poly_points);
	ClassDB::bind_method(D_METHOD("set_shape_param", "name", "value"), &HewnMesh3D::set_shape_param);
	ClassDB::bind_method(D_METHOD("get_shape_param", "name"), &HewnMesh3D::get_shape_param);
	ClassDB::bind_method(D_METHOD("set_mesh_data", "data"), &HewnMesh3D::set_mesh_data);
	ClassDB::bind_method(D_METHOD("get_mesh_data"), &HewnMesh3D::get_mesh_data);
	ClassDB::bind_method(D_METHOD("set_materials", "materials"), &HewnMesh3D::set_materials);
	ClassDB::bind_method(D_METHOD("get_materials"), &HewnMesh3D::get_materials);
	ClassDB::bind_method(D_METHOD("set_collision_mode", "mode"), &HewnMesh3D::set_collision_mode);
	ClassDB::bind_method(D_METHOD("get_collision_mode"), &HewnMesh3D::get_collision_mode);
	ClassDB::bind_method(D_METHOD("get_state"), &HewnMesh3D::get_state);
	ClassDB::bind_method(D_METHOD("set_state", "state"), &HewnMesh3D::set_state);
	ClassDB::bind_method(D_METHOD("get_selection_state"), &HewnMesh3D::get_selection_state);
	ClassDB::bind_method(D_METHOD("set_selection_state", "state"), &HewnMesh3D::set_selection_state);
	ADD_SIGNAL(MethodInfo("selection_restored"));
	ClassDB::bind_method(D_METHOD("rebuild"), &HewnMesh3D::rebuild);
	ClassDB::bind_method(D_METHOD("commit_positions"), &HewnMesh3D::commit_positions);
	ClassDB::bind_method(D_METHOD("dyntopo_refine", "center", "radius", "max_length", "subdivide", "collapse"), &HewnMesh3D::dyntopo_refine, DEFVAL(true), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("finish_positions"), &HewnMesh3D::finish_positions);
	ClassDB::bind_method(D_METHOD("get_edge_loop", "a", "b"), &HewnMesh3D::get_edge_loop);
	ClassDB::bind_method(D_METHOD("get_edge_ring", "a", "b"), &HewnMesh3D::get_edge_ring);
	ClassDB::bind_method(D_METHOD("get_face_loop", "face", "a", "b"), &HewnMesh3D::get_face_loop);
	ClassDB::bind_method(D_METHOD("grow_faces", "faces", "max_angle"), &HewnMesh3D::grow_faces, DEFVAL(181.0));
	ClassDB::bind_method(D_METHOD("get_linked_faces", "faces"), &HewnMesh3D::get_linked_faces);

	ClassDB::bind_method(D_METHOD("get_vertex_count"), &HewnMesh3D::get_vertex_count);
	ClassDB::bind_method(D_METHOD("get_face_count"), &HewnMesh3D::get_face_count);
	ClassDB::bind_method(D_METHOD("get_vertices"), &HewnMesh3D::get_vertices);
	ClassDB::bind_method(D_METHOD("set_vertices", "vertices", "positions_only"), &HewnMesh3D::set_vertices, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_vertex", "index"), &HewnMesh3D::get_vertex);
	ClassDB::bind_method(D_METHOD("set_vertex", "index", "position"), &HewnMesh3D::set_vertex);
	ClassDB::bind_method(D_METHOD("get_face", "face"), &HewnMesh3D::get_face);
	ClassDB::bind_method(D_METHOD("add_vertex", "position"), &HewnMesh3D::add_vertex);
	ClassDB::bind_method(D_METHOD("add_face", "indices", "material"), &HewnMesh3D::add_face, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("clear_mesh"), &HewnMesh3D::clear_mesh);
	ClassDB::bind_method(D_METHOD("get_face_material", "face"), &HewnMesh3D::get_face_material);
	ClassDB::bind_method(D_METHOD("get_face_role", "face"), &HewnMesh3D::get_face_role);
	ClassDB::bind_method(D_METHOD("apply_material", "material", "faces"), &HewnMesh3D::apply_material, DEFVAL(PackedInt32Array()));
	ClassDB::bind_method(D_METHOD("compact_materials"), &HewnMesh3D::compact_materials);
	ClassDB::bind_method(D_METHOD("uv_lock_begin"), &HewnMesh3D::uv_lock_begin);
	ClassDB::bind_method(D_METHOD("uv_lock_end"), &HewnMesh3D::uv_lock_end);
	ClassDB::bind_method(D_METHOD("set_face_material", "face", "material"), &HewnMesh3D::set_face_material);
	ClassDB::bind_method(D_METHOD("get_face_smoothing", "face"), &HewnMesh3D::get_face_smoothing);
	ClassDB::bind_method(D_METHOD("set_face_smoothing", "face", "group"), &HewnMesh3D::set_face_smoothing);
	ClassDB::bind_method(D_METHOD("set_face_color", "face", "color"), &HewnMesh3D::set_face_color);
	ClassDB::bind_method(D_METHOD("get_face_normal", "face"), &HewnMesh3D::get_face_normal);
	ClassDB::bind_method(D_METHOD("get_face_center", "face"), &HewnMesh3D::get_face_center);

	ClassDB::bind_method(D_METHOD("extrude_faces", "faces", "distance", "individual", "dissolve_orthogonal"), &HewnMesh3D::extrude_faces, DEFVAL(false), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("inset_faces", "faces", "amount", "individual"), &HewnMesh3D::inset_faces, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("delete_faces", "faces"), &HewnMesh3D::delete_faces);
	ClassDB::bind_method(D_METHOD("flip_faces", "faces"), &HewnMesh3D::flip_faces);
	ClassDB::bind_method(D_METHOD("subdivide_faces", "faces"), &HewnMesh3D::subdivide_faces);
	ClassDB::bind_method(D_METHOD("subdivide_faces_ex", "faces", "cuts", "smoothness", "ngons", "fractal", "along_normal", "seed"), &HewnMesh3D::subdivide_faces_ex,
			DEFVAL(1), DEFVAL(0.0f), DEFVAL(true), DEFVAL(0.0f), DEFVAL(0.0f), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("triangulate_faces", "faces"), &HewnMesh3D::triangulate_faces);
	ClassDB::bind_method(D_METHOD("merge_faces", "faces"), &HewnMesh3D::merge_faces);
	ClassDB::bind_method(D_METHOD("detach_faces", "faces"), &HewnMesh3D::detach_faces);
	ClassDB::bind_method(D_METHOD("conform_normals"), &HewnMesh3D::conform_normals);
	ClassDB::bind_method(D_METHOD("bevel_edges", "edge_pairs", "amount", "segments"), &HewnMesh3D::bevel_edges, DEFVAL(1));
	ClassDB::bind_method(D_METHOD("recalculate_normals", "inside"), &HewnMesh3D::recalculate_normals, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("make_face", "vertices"), &HewnMesh3D::make_face);
	ClassDB::bind_method(D_METHOD("bisect", "point", "normal", "clear_inner", "clear_outer", "fill"), &HewnMesh3D::bisect, DEFVAL(false), DEFVAL(false), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("move_face_plane", "face", "delta", "collapse"), &HewnMesh3D::move_face_plane, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("cut_box", "box", "faces"), &HewnMesh3D::cut_box, DEFVAL(PackedInt32Array()));
	ClassDB::bind_method(D_METHOD("bridge_holes", "a", "b"), &HewnMesh3D::bridge_holes);
	ClassDB::bind_method(D_METHOD("make_alcove", "loop", "depth"), &HewnMesh3D::make_alcove);
	ClassDB::bind_method(D_METHOD("fill_hole", "loop"), &HewnMesh3D::fill_hole);
	ClassDB::bind_method(D_METHOD("wrap_texture", "from_face", "to_face", "material"), &HewnMesh3D::wrap_texture, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("get_face_uvs", "face"), &HewnMesh3D::get_face_uvs);
	ClassDB::bind_method(D_METHOD("edit_uvs", "faces", "op", "args"), &HewnMesh3D::edit_uvs, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("set_faces_hidden", "faces", "hidden"), &HewnMesh3D::set_faces_hidden);
	ClassDB::bind_method(D_METHOD("is_face_hidden", "face"), &HewnMesh3D::is_face_hidden);
	ClassDB::bind_method(D_METHOD("join_mesh", "other", "keep_overlap"), &HewnMesh3D::join_mesh, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_uv_bounds", "faces"), &HewnMesh3D::get_uv_bounds);
	ClassDB::bind_method(D_METHOD("unwrap_quads", "faces", "start"), &HewnMesh3D::unwrap_quads, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("poke_faces", "faces"), &HewnMesh3D::poke_faces);
	ClassDB::bind_method(D_METHOD("bridge_faces", "faces"), &HewnMesh3D::bridge_faces);
	ClassDB::bind_method(D_METHOD("split_edges", "edge_pairs"), &HewnMesh3D::split_edges);
	ClassDB::bind_method(D_METHOD("bevel_vertices", "vertices", "amount"), &HewnMesh3D::bevel_vertices);
	ClassDB::bind_method(D_METHOD("set_edge_attributes", "a", "b", "seam", "sharp", "crease", "bevel_weight"), &HewnMesh3D::set_edge_attributes);
	ClassDB::bind_method(D_METHOD("get_edge_attributes", "a", "b"), &HewnMesh3D::get_edge_attributes);
	ClassDB::bind_method(D_METHOD("limited_dissolve", "angle"), &HewnMesh3D::limited_dissolve, DEFVAL(5.0f));
	ClassDB::bind_method(D_METHOD("loop_cut", "a", "b", "cuts", "t"), &HewnMesh3D::loop_cut, DEFVAL(1), DEFVAL(0.5f));
	ClassDB::bind_method(D_METHOD("knife_cut", "points", "faces", "view_direction"), &HewnMesh3D::knife_cut);
	ClassDB::bind_method(D_METHOD("set_world_uv_all", "scale"), &HewnMesh3D::set_world_uv_all);
	ClassDB::bind_method(D_METHOD("boolean_with", "other", "operation"), &HewnMesh3D::boolean_with);
	BIND_ENUM_CONSTANT(BOOLEAN_UNION);
	BIND_ENUM_CONSTANT(BOOLEAN_SUBTRACT);
	BIND_ENUM_CONSTANT(BOOLEAN_INTERSECT);
	ClassDB::bind_method(D_METHOD("extrude_edges", "edge_pairs", "distance"), &HewnMesh3D::extrude_edges);
	ClassDB::bind_method(D_METHOD("connect_edges", "edge_pairs"), &HewnMesh3D::connect_edges);
	ClassDB::bind_method(D_METHOD("insert_edge_loops", "edge_pairs"), &HewnMesh3D::insert_edge_loops);
	ClassDB::bind_method(D_METHOD("subdivide_edges", "edge_pairs", "cuts"), &HewnMesh3D::subdivide_edges, DEFVAL(1));
	ClassDB::bind_method(D_METHOD("bridge_edges", "edge_pairs"), &HewnMesh3D::bridge_edges);
	ClassDB::bind_method(D_METHOD("dissolve_edges", "edge_pairs"), &HewnMesh3D::dissolve_edges);
	ClassDB::bind_method(D_METHOD("dissolve_vertices", "vertices"), &HewnMesh3D::dissolve_vertices);
	ClassDB::bind_method(D_METHOD("to_circle", "vertices", "faces", "regular", "influence"), &HewnMesh3D::to_circle, DEFVAL(true), DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("get_face_triangles", "face"), &HewnMesh3D::get_face_triangles);
	ClassDB::bind_method(D_METHOD("get_shape_anchor"), &HewnMesh3D::get_shape_anchor);
	ClassDB::bind_method(D_METHOD("weld_vertices", "vertices", "distance"), &HewnMesh3D::weld_vertices);
	ClassDB::bind_method(D_METHOD("collapse_vertices", "vertices"), &HewnMesh3D::collapse_vertices);
	ClassDB::bind_method(D_METHOD("split_vertices", "vertices"), &HewnMesh3D::split_vertices);
	ClassDB::bind_method(D_METHOD("connect_vertices", "vertices"), &HewnMesh3D::connect_vertices);
	ClassDB::bind_method(D_METHOD("fill_holes"), &HewnMesh3D::fill_holes);
	ClassDB::bind_method(D_METHOD("subdivide_smooth"), &HewnMesh3D::subdivide_smooth);
	ClassDB::bind_method(D_METHOD("auto_smooth", "angle"), &HewnMesh3D::auto_smooth, DEFVAL(30.0));
	ClassDB::bind_method(D_METHOD("quadify", "angle"), &HewnMesh3D::quadify, DEFVAL(1.0));

	ClassDB::bind_method(D_METHOD("freeze_transform"), &HewnMesh3D::freeze_transform);
	ClassDB::bind_method(D_METHOD("set_pivot", "local_point"), &HewnMesh3D::set_pivot);
	ClassDB::bind_method(D_METHOD("center_pivot"), &HewnMesh3D::center_pivot);
	ClassDB::bind_method(D_METHOD("merge_from", "other"), &HewnMesh3D::merge_from);
	ClassDB::bind_method(D_METHOD("update_cuts"), &HewnMesh3D::_cuts_refresh);
	ClassDB::bind_method(D_METHOD("is_cut"), &HewnMesh3D::is_cut);
	ClassDB::bind_method(D_METHOD("arch_edges", "edge_pairs", "segments", "rise"), &HewnMesh3D::arch_edges, DEFVAL(8), DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("sweep_along", "path"), &HewnMesh3D::sweep_along);
	ClassDB::bind_method(D_METHOD("to_array_mesh"), &HewnMesh3D::to_array_mesh);
	ClassDB::bind_method(D_METHOD("to_obj"), &HewnMesh3D::to_obj);
	ClassDB::bind_method(D_METHOD("export_obj", "path"), &HewnMesh3D::export_obj);
	ClassDB::bind_method(D_METHOD("export_gltf", "path"), &HewnMesh3D::export_gltf);
	ClassDB::bind_method(D_METHOD("create_from_mesh", "mesh", "quadify"), &HewnMesh3D::create_from_mesh, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("set_custom_formats", "formats"), &HewnMesh3D::set_custom_formats);
	ClassDB::bind_method(D_METHOD("get_custom_formats"), &HewnMesh3D::get_custom_formats);
	ClassDB::bind_method(D_METHOD("set_custom_value", "channel", "vertices", "value"), &HewnMesh3D::set_custom_value);
	ClassDB::bind_method(D_METHOD("get_info"), &HewnMesh3D::get_info);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "shape", PROPERTY_HINT_ENUM, SHAPE_ENUM), "set_shape", "get_shape");
	// the formats of CUSTOM0..3 in the built mesh (Mesh.ArrayCustomFormat; -1: not written)
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "custom_formats", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_custom_formats", "get_custom_formats");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "shape_params", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_shape_params", "get_shape_params");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "poly_points", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_poly_points", "get_poly_points");
	// (the slots are resource pickers: a click opens the material, as for Array[Material] in a script)
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "materials", PROPERTY_HINT_ARRAY_TYPE,
						 String::num_int64(Variant::OBJECT) + "/" + String::num_int64(PROPERTY_HINT_RESOURCE_TYPE) + ":Material"),
			"set_materials", "get_materials");
	ClassDB::bind_method(D_METHOD("set_collision_ratio", "ratio"), &HewnMesh3D::set_collision_ratio);
	ClassDB::bind_method(D_METHOD("get_collision_ratio"), &HewnMesh3D::get_collision_ratio);
	ClassDB::bind_method(D_METHOD("set_collision_max_pieces", "count"), &HewnMesh3D::set_collision_max_pieces);
	ClassDB::bind_method(D_METHOD("get_collision_max_pieces"), &HewnMesh3D::get_collision_max_pieces);
	ClassDB::bind_method(D_METHOD("set_collision_axis", "axis"), &HewnMesh3D::set_collision_axis);
	ClassDB::bind_method(D_METHOD("get_collision_axis"), &HewnMesh3D::get_collision_axis);
	ClassDB::bind_method(D_METHOD("set_collision_metadata", "metadata"), &HewnMesh3D::set_collision_metadata);
	ClassDB::bind_method(D_METHOD("get_collision_metadata"), &HewnMesh3D::get_collision_metadata);
	ClassDB::bind_method(D_METHOD("set_collision_groups", "groups"), &HewnMesh3D::set_collision_groups);
	ClassDB::bind_method(D_METHOD("get_collision_groups"), &HewnMesh3D::get_collision_groups);
	ClassDB::bind_method(D_METHOD("get_collision_shapes"), &HewnMesh3D::get_collision_shapes);
	ClassDB::bind_method(D_METHOD("can_primitive_collision"), &HewnMesh3D::can_primitive_collision);
	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mode", PROPERTY_HINT_ENUM, "None:0,Trimesh:1,Convex:2,Simplified Convex:3,Multiple Convex:4,Box:5,Capsule:6,Cylinder:7,Sphere:8,Primitive:9,Simplified Trimesh:10"), "set_collision_mode", "get_collision_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_ratio", PROPERTY_HINT_RANGE, "0.01,1,0.01"), "set_collision_ratio", "get_collision_ratio");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_max_pieces", PROPERTY_HINT_RANGE, "1,64,1"), "set_collision_max_pieces", "get_collision_max_pieces");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_axis", PROPERTY_HINT_ENUM, "Longest,X,Y,Z"), "set_collision_axis", "get_collision_axis");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "collision_metadata"), "set_collision_metadata", "get_collision_metadata");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "collision_groups"), "set_collision_groups", "get_collision_groups");
	ADD_GROUP("", "");
	ClassDB::bind_method(D_METHOD("set_lightmap_uv2", "on"), &HewnMesh3D::set_lightmap_uv2);
	ClassDB::bind_method(D_METHOD("get_lightmap_uv2"), &HewnMesh3D::get_lightmap_uv2);
	ClassDB::bind_method(D_METHOD("set_lightmap_texel_size", "size"), &HewnMesh3D::set_lightmap_texel_size);
	ClassDB::bind_method(D_METHOD("get_lightmap_texel_size"), &HewnMesh3D::get_lightmap_texel_size);
	ClassDB::bind_method(D_METHOD("get_lightmap_size"), &HewnMesh3D::get_lightmap_size);
	ClassDB::bind_method(D_METHOD("get_game_mesh"), &HewnMesh3D::get_game_mesh);
	ClassDB::bind_method(D_METHOD("get_collision_shape"), &HewnMesh3D::get_collision_shape);
	ClassDB::bind_method(D_METHOD("get_trigger_shape"), &HewnMesh3D::get_trigger_shape);
	// Lightmap UVs (UV2) for LightmapGI, saved with the scene.
	ADD_GROUP("Lightmap", "lightmap_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lightmap_uv2"), "set_lightmap_uv2", "get_lightmap_uv2");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lightmap_texel_size", PROPERTY_HINT_RANGE, "0.001,10,0.001,or_greater,suffix:m"), "set_lightmap_texel_size", "get_lightmap_texel_size");
	ADD_GROUP("", "");
	ClassDB::bind_method(D_METHOD("set_base_profile", "profile"), &HewnMesh3D::set_base_profile);
	ClassDB::bind_method(D_METHOD("get_base_profile"), &HewnMesh3D::get_base_profile);
	ClassDB::bind_method(D_METHOD("set_shaft_profile", "profile"), &HewnMesh3D::set_shaft_profile);
	ClassDB::bind_method(D_METHOD("get_shaft_profile"), &HewnMesh3D::get_shaft_profile);
	ClassDB::bind_method(D_METHOD("set_top_profile", "profile"), &HewnMesh3D::set_top_profile);
	ClassDB::bind_method(D_METHOD("get_top_profile"), &HewnMesh3D::get_top_profile);
	ClassDB::bind_method(D_METHOD("set_shape_profile", "profile"), &HewnMesh3D::set_shape_profile);
	ClassDB::bind_method(D_METHOD("get_shape_profile"), &HewnMesh3D::get_shape_profile);
	ClassDB::bind_method(D_METHOD("set_sweep_profile", "profile"), &HewnMesh3D::set_sweep_profile);
	ClassDB::bind_method(D_METHOD("get_sweep_profile"), &HewnMesh3D::get_sweep_profile);
	ClassDB::bind_method(D_METHOD("make_profiles_editable"), &HewnMesh3D::make_profiles_editable);
	ClassDB::bind_method(D_METHOD("_on_profile_changed"), &HewnMesh3D::_on_profile_changed);
	ADD_GROUP("Profiles", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "base_profile", PROPERTY_HINT_RESOURCE_TYPE, "HewnProfile"), "set_base_profile", "get_base_profile");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "shaft_profile", PROPERTY_HINT_RESOURCE_TYPE, "HewnProfile"), "set_shaft_profile", "get_shaft_profile");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "top_profile", PROPERTY_HINT_RESOURCE_TYPE, "HewnProfile"), "set_top_profile", "get_top_profile");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "sweep_profile", PROPERTY_HINT_RESOURCE_TYPE, "HewnProfile"), "set_sweep_profile", "get_sweep_profile");
	// the door's opening / the arch's inner line / the wedge's slope, drawn (empty: the parameters')
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "shape_profile", PROPERTY_HINT_RESOURCE_TYPE, "HewnProfile"), "set_shape_profile", "get_shape_profile");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "mesh_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_mesh_data", "get_mesh_data");

	ClassDB::bind_method(D_METHOD("set_mirror_x", "on"), &HewnMesh3D::set_mirror_x);
	ClassDB::bind_method(D_METHOD("get_mirror_x"), &HewnMesh3D::get_mirror_x);
	ClassDB::bind_method(D_METHOD("set_mirror_y", "on"), &HewnMesh3D::set_mirror_y);
	ClassDB::bind_method(D_METHOD("get_mirror_y"), &HewnMesh3D::get_mirror_y);
	ClassDB::bind_method(D_METHOD("set_mirror_z", "on"), &HewnMesh3D::set_mirror_z);
	ClassDB::bind_method(D_METHOD("get_mirror_z"), &HewnMesh3D::get_mirror_z);
	ClassDB::bind_method(D_METHOD("set_mirror_merge", "distance"), &HewnMesh3D::set_mirror_merge);
	ClassDB::bind_method(D_METHOD("get_mirror_merge"), &HewnMesh3D::get_mirror_merge);
	ClassDB::bind_method(D_METHOD("set_solidify_thickness", "thickness"), &HewnMesh3D::set_solidify_thickness);
	ClassDB::bind_method(D_METHOD("get_solidify_thickness"), &HewnMesh3D::get_solidify_thickness);
	ClassDB::bind_method(D_METHOD("set_solidify_offset", "offset"), &HewnMesh3D::set_solidify_offset);
	ClassDB::bind_method(D_METHOD("get_solidify_offset"), &HewnMesh3D::get_solidify_offset);
	ClassDB::bind_method(D_METHOD("apply_modifiers"), &HewnMesh3D::apply_modifiers);
	ClassDB::bind_method(D_METHOD("set_modifiers", "modifiers"), &HewnMesh3D::set_modifiers);
	ClassDB::bind_method(D_METHOD("get_modifiers"), &HewnMesh3D::get_modifiers);
	ClassDB::bind_method(D_METHOD("add_modifier", "modifier"), &HewnMesh3D::add_modifier);
	ClassDB::bind_method(D_METHOD("has_modifiers"), &HewnMesh3D::has_modifiers);
	// The stack: Add Element, then New HewnArrayModifier / HewnMirrorModifier / ...; applied top to bottom.
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "modifiers", PROPERTY_HINT_ARRAY_TYPE,
						 String::num_int64(Variant::OBJECT) + "/" + String::num_int64(PROPERTY_HINT_RESOURCE_TYPE) + ":HewnModifier"),
			"set_modifiers", "get_modifiers");
	// old modifier properties: still settable (scripts, older scenes), they edit the stack
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mirror_x", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_mirror_x", "get_mirror_x");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mirror_y", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_mirror_y", "get_mirror_y");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mirror_z", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_mirror_z", "get_mirror_z");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mirror_merge", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_mirror_merge", "get_mirror_merge");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "solidify_thickness", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_solidify_thickness", "get_solidify_thickness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "solidify_offset", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_solidify_offset", "get_solidify_offset");

	ClassDB::bind_method(D_METHOD("set_boolean_targets", "targets"), &HewnMesh3D::set_boolean_targets);
	ClassDB::bind_method(D_METHOD("get_boolean_targets"), &HewnMesh3D::get_boolean_targets);
	ClassDB::bind_method(D_METHOD("set_boolean_ops", "ops"), &HewnMesh3D::set_boolean_ops);
	ClassDB::bind_method(D_METHOD("get_boolean_ops"), &HewnMesh3D::get_boolean_ops);
	ClassDB::bind_method(D_METHOD("set_boolean_cutter", "on"), &HewnMesh3D::set_boolean_cutter);
	ClassDB::bind_method(D_METHOD("is_boolean_cutter"), &HewnMesh3D::is_boolean_cutter);
	ClassDB::bind_method(D_METHOD("add_boolean", "other", "op"), &HewnMesh3D::add_boolean);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "boolean_targets", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_boolean_targets", "get_boolean_targets");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "boolean_ops", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_boolean_ops", "get_boolean_ops");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "boolean_cutter", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_boolean_cutter", "is_boolean_cutter"); // (set by the boolean operations themselves)
	ClassDB::bind_method(D_METHOD("set_csg_model", "on"), &HewnMesh3D::set_csg_model);
	ClassDB::bind_method(D_METHOD("is_csg_model"), &HewnMesh3D::is_csg_model);
	ClassDB::bind_method(D_METHOD("set_csg_inverted", "on"), &HewnMesh3D::set_csg_inverted);
	ClassDB::bind_method(D_METHOD("is_csg_inverted"), &HewnMesh3D::is_csg_inverted);
	ClassDB::bind_method(D_METHOD("set_brush_operation", "operation"), &HewnMesh3D::set_brush_operation);
	ClassDB::bind_method(D_METHOD("get_brush_operation"), &HewnMesh3D::get_brush_operation);
	ClassDB::bind_method(D_METHOD("get_brush_nodes"), &HewnMesh3D::get_brush_nodes);
	ClassDB::bind_method(D_METHOD("is_brush"), &HewnMesh3D::is_brush);
	// (CSG models have no UI any more: kept in the file only, so older scenes still load)
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "csg_model", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_csg_model", "is_csg_model");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "csg_inverted", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_csg_inverted", "is_csg_inverted");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "brush_operation", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_brush_operation", "get_brush_operation");

	ClassDB::bind_method(D_METHOD("set_uv_anchor", "transform"), &HewnMesh3D::set_uv_anchor);
	ClassDB::bind_method(D_METHOD("get_uv_anchor"), &HewnMesh3D::get_uv_anchor);
	ClassDB::bind_method(D_METHOD("set_uv_anchored", "on"), &HewnMesh3D::set_uv_anchored);
	ClassDB::bind_method(D_METHOD("is_uv_anchored"), &HewnMesh3D::is_uv_anchored);
	ADD_PROPERTY(PropertyInfo(Variant::TRANSFORM3D, "uv_anchor", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_uv_anchor", "get_uv_anchor");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "uv_anchored", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_uv_anchored", "is_uv_anchored");
	ADD_SIGNAL(MethodInfo("mesh_changed"));
	ADD_SIGNAL(MethodInfo("geometry_moved"));

	BIND_ENUM_CONSTANT(COLLISION_NONE);
	BIND_ENUM_CONSTANT(COLLISION_TRIMESH);
	BIND_ENUM_CONSTANT(COLLISION_CONVEX);
	BIND_ENUM_CONSTANT(COLLISION_SIMPLIFIED_CONVEX);
	BIND_ENUM_CONSTANT(COLLISION_MULTIPLE_CONVEX);
	BIND_ENUM_CONSTANT(COLLISION_BOX);
	BIND_ENUM_CONSTANT(COLLISION_CAPSULE);
	BIND_ENUM_CONSTANT(COLLISION_CYLINDER);
	BIND_ENUM_CONSTANT(COLLISION_SPHERE);
	BIND_ENUM_CONSTANT(COLLISION_PRIMITIVE);
	BIND_ENUM_CONSTANT(COLLISION_SIMPLIFIED_TRIMESH);
}
