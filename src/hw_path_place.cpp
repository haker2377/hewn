#include "hw_path_place.h"

#include <godot_cpp/core/class_db.hpp>

#include <cmath>
#include <random>

using namespace godot;

void HewnPathPlacement::set_mesh(const Ref<Mesh> &p_mesh) {
	mesh = p_mesh;
	emit_changed();
}

namespace {
float hash01(int p_i, int p_seed) {
	uint32_t h = (uint32_t)p_i * 374761393u + (uint32_t)p_seed * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return (float)(h & 0xffffff) / (float)0xffffff;
}
// smooth noise along the path: 0..1, changing over about p_len
float noise1(float p_x, int p_seed) {
	const float f = std::floor(p_x);
	const float t = p_x - f;
	const float s = t * t * (3.0f - 2.0f * t);
	return hash01((int)f, p_seed) * (1.0f - s) + hash01((int)f + 1, p_seed) * s;
}
} // namespace

std::vector<Transform3D> HewnPathPlacement::transforms(const std::vector<std::vector<Vector3>> &p_splines, const std::vector<bool> &p_cyclic, const Transform3D &p_global,
		const std::function<bool(const Vector3 &, const Vector3 &, Vector3 &, Vector3 &)> &p_ray) const {
	std::vector<Transform3D> out;
	if (!enabled || mesh.is_null()) {
		return out;
	}
	const AABB mb = mesh->get_aabb();
	float step = spacing > 1e-4f ? spacing : (align ? mb.size.x : MAX(mb.size.x, mb.size.z));
	step = MAX(step, 0.05f);
	const Transform3D inv = p_global.affine_inverse();
	const Vector3 up(0, 1, 0);
	for (size_t si = 0; si < p_splines.size(); si++) {
		std::vector<Vector3> pts = p_splines[si];
		if (si < p_cyclic.size() && p_cyclic[si] && pts.size() > 2 && pts.front().distance_to(pts.back()) > 1e-5f) {
			pts.push_back(pts.front());
		}
		if (pts.size() < 2) {
			continue;
		}
		std::vector<float> acc(pts.size(), 0.0f);
		for (size_t i = 1; i < pts.size(); i++) {
			acc[i] = acc[i - 1] + pts[i - 1].distance_to(pts[i]);
		}
		const float total = acc.back();
		const float a = MAX(0.0f, start_offset), b = total - MAX(0.0f, end_offset);
		if (b - a < 1e-4f) {
			continue;
		}
		std::mt19937 rng((uint32_t)seed * 2654435761u + (uint32_t)si * 7919u + 17u);
		std::uniform_real_distribution<float> u01(0.0f, 1.0f);
		const int count = MAX(1, (int)std::floor((b - a) / step + 1e-4f));
		const float used = (b - a) / (float)count; // (the copies spread evenly over what there is)
		std::vector<int> sides;
		switch (side) {
			case SIDE_LEFT:
				sides = { -1 };
				break;
			case SIDE_RIGHT:
				sides = { 1 };
				break;
			case SIDE_BOTH:
				sides = { -1, 1 };
				break;
			default:
				sides = { 0 };
				break;
		}
		size_t seg = 0;
		for (int k = 0; k < count && out.size() < 100000; k++) {
			for (int sd : sides) {
				const float r_jit = u01(rng), r_chance = u01(rng), r_spread = u01(rng), r_yaw = u01(rng), r_scale = u01(rng);
				float s = a + used * ((float)k + 0.5f) + (r_jit - 0.5f) * spacing_random * used;
				s = CLAMP(s, a, b);
				if (r_chance > chance) {
					continue;
				}
				if (patch_length > 1e-4f && noise1(s / patch_length, seed * 31 + sd + 1000 * (int)si) > patch_amount) {
					continue; // (a stretch without it)
				}
				seg = 0;
				while (seg + 2 < pts.size() && acc[seg + 1] < s) {
					seg++;
				}
				const float len = acc[seg + 1] - acc[seg];
				const float t = len > 1e-7f ? (s - acc[seg]) / len : 0.0f;
				const Vector3 p = pts[seg].lerp(pts[seg + 1], t);
				Vector3 tan = pts[seg + 1] - pts[seg];
				tan = tan.length() > 1e-7f ? tan.normalized() : Vector3(1, 0, 0);
				Vector3 right = tan.cross(up);
				right = right.length() > 1e-5f ? right.normalized() : Vector3(0, 0, 1);
				Vector3 pos = p + right * ((float)sd * offset + spread * (r_spread * 2.0f - 1.0f)) + up * height;
				Basis basis;
				if (align) {
					const Vector3 x = Vector3(tan.x, 0, tan.z).length() > 1e-5f ? Vector3(tan.x, 0, tan.z).normalized() : Vector3(1, 0, 0);
					basis = Basis(x, up, x.cross(up));
					if (sd < 0) {
						basis = Basis(up, (float)Math::PI) * basis; // (the left side faces the other way: both sides alike from the path)
					}
				}
				const float yaw_rad = Math::deg_to_rad(yaw + random_yaw * (r_yaw * 2.0f - 1.0f));
				basis = Basis(up, yaw_rad) * basis;
				if (drop_to_ground && p_ray) {
					const Vector3 gp = p_global.xform(pos - up * height);
					Vector3 hit, nrm;
					if (p_ray(gp + Vector3(0, 2.0f, 0), Vector3(0, -1, 0), hit, nrm)) {
						pos = inv.xform(hit) + up * height;
						if (tilt_to_ground) {
							const Vector3 ln = inv.basis.xform(nrm).normalized();
							const Vector3 ax = up.cross(ln);
							const float ang = std::atan2(ax.length(), up.dot(ln));
							if (ax.length() > 1e-6f) {
								basis = Basis(ax.normalized(), ang) * basis;
							}
						}
					}
				}
				const float sc = scale_min + (scale_max - scale_min) * r_scale;
				out.push_back(Transform3D(basis.scaled_local(Vector3(sc, sc, sc)), pos));
			}
		}
	}
	return out;
}

void HewnPathPlacement::_validate_property(PropertyInfo &p_property) const {
	const String pn = p_property.name;
	if ((pn == "offset" && side == SIDE_CENTER) || (pn == "patch_amount" && patch_length <= 1e-4f) || (pn == "tilt_to_ground" && !drop_to_ground)) {
		p_property.usage = PROPERTY_USAGE_STORAGE;
	}
}

void HewnPathPlacement::_bind_methods() {
#define HW_BIND(m_name)                                                                                     \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &HewnPathPlacement::set_##m_name); \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &HewnPathPlacement::get_##m_name);
	HW_BIND(enabled)
	HW_BIND(chunk_size)
	HW_BIND(visibility_range)
	HW_BIND(game_metadata)
	HW_BIND(game_groups)
	HW_BIND(mesh_script)
	HW_BIND(collision_script)
	HW_BIND(mesh)
	HW_BIND(side)
	HW_BIND(offset)
	HW_BIND(height)
	HW_BIND(spacing)
	HW_BIND(spacing_random)
	HW_BIND(start_offset)
	HW_BIND(end_offset)
	HW_BIND(chance)
	HW_BIND(patch_length)
	HW_BIND(patch_amount)
	HW_BIND(spread)
	HW_BIND(scale_min)
	HW_BIND(scale_max)
	HW_BIND(random_yaw)
	HW_BIND(yaw)
	HW_BIND(align)
	HW_BIND(drop_to_ground)
	HW_BIND(tilt_to_ground)
	HW_BIND(collision)
	HW_BIND(cast_shadows)
	HW_BIND(seed)
#undef HW_BIND
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "get_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, "Mesh"), "set_mesh", "get_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "side", PROPERTY_HINT_ENUM, "Center,Left,Right,Both"), "set_side", "get_side");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "offset", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_offset", "get_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height", PROPERTY_HINT_RANGE, "-100,100,0.01,or_less,or_greater,suffix:m"), "set_height", "get_height");
	ADD_GROUP("Spacing", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spacing", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_spacing", "get_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spacing_random", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_spacing_random", "get_spacing_random");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "start_offset", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_start_offset", "get_start_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "end_offset", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_end_offset", "get_end_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "chance", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_chance", "get_chance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "patch_length", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:m"), "set_patch_length", "get_patch_length");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "patch_amount", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_patch_amount", "get_patch_amount");
	ADD_GROUP("Random", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spread", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_spread", "get_spread");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scale_min", PROPERTY_HINT_RANGE, "0.01,10,0.01,or_greater"), "set_scale_min", "get_scale_min");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scale_max", PROPERTY_HINT_RANGE, "0.01,10,0.01,or_greater"), "set_scale_max", "get_scale_max");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "yaw", PROPERTY_HINT_RANGE, "-180,180,0.1,degrees"), "set_yaw", "get_yaw");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "random_yaw", PROPERTY_HINT_RANGE, "0,180,0.1,degrees"), "set_random_yaw", "get_random_yaw");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_GROUP("Placing", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "align"), "set_align", "get_align");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "drop_to_ground"), "set_drop_to_ground", "get_drop_to_ground");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "tilt_to_ground"), "set_tilt_to_ground", "get_tilt_to_ground");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision", PROPERTY_HINT_ENUM, "None,Box,Convex,Trimesh"), "set_collision", "get_collision");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_shadows"), "set_cast_shadows", "get_cast_shadows");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "chunk_size", PROPERTY_HINT_RANGE, "0,256,0.5,or_greater,suffix:m"), "set_chunk_size", "get_chunk_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range", PROPERTY_HINT_RANGE, "0,10000,0.1,or_greater,suffix:m"), "set_visibility_range", "get_visibility_range");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "game_metadata"), "set_game_metadata", "get_game_metadata");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "game_groups"), "set_game_groups", "get_game_groups");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesh_script", PROPERTY_HINT_RESOURCE_TYPE, "Script"), "set_mesh_script", "get_mesh_script");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "collision_script", PROPERTY_HINT_RESOURCE_TYPE, "Script"), "set_collision_script", "get_collision_script");
	BIND_ENUM_CONSTANT(SIDE_CENTER);
	BIND_ENUM_CONSTANT(SIDE_LEFT);
	BIND_ENUM_CONSTANT(SIDE_RIGHT);
	BIND_ENUM_CONSTANT(SIDE_BOTH);
	BIND_ENUM_CONSTANT(COLLISION_NONE);
	BIND_ENUM_CONSTANT(COLLISION_BOX);
	BIND_ENUM_CONSTANT(COLLISION_CONVEX);
	BIND_ENUM_CONSTANT(COLLISION_TRIMESH);
}
