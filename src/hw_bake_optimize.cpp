// What the bake does to the game's nodes once they are made: the static meshes joined and cut into squares of the map
// (a draw for each square and material, what is out of view left out), their collision joined, detail levels made,
// a visibility range by size, the faces no one can see left out.
#include "hw_level_bake.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/importer_mesh.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/occluder_instance3d.hpp>
#include <godot_cpp/classes/physics_material.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>
#include <vector>

using namespace godot;

namespace {

struct Opt {
	bool merge = false, plain = false, collision = false, lod = false, range = false, hidden = false;
	float cell = 32.0f;
};

const uint64_t CUSTOM_MASK = ((uint64_t)7 << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT) | ((uint64_t)7 << Mesh::ARRAY_FORMAT_CUSTOM1_SHIFT) |
		((uint64_t)7 << Mesh::ARRAY_FORMAT_CUSTOM2_SHIFT) | ((uint64_t)7 << Mesh::ARRAY_FORMAT_CUSTOM3_SHIFT);

struct Surf {
	Array arr;
	Ref<Material> mat;
	uint64_t flags = 0; // (the custom channels' formats)
	String key; // (what can share a surface with it)
};

struct Item {
	MeshInstance3D *mi = nullptr;
	Transform3D xf;
	std::vector<Surf> surfs;
	AABB box;
	String key;
	int triangles = 0;
	bool split = false, consumed = false;
	std::vector<std::vector<char>> hidden; // (per surface, per triangle)
};

String tag_key(Node *p_node) {
	std::vector<String> parts;
	for (const Variant &k : p_node->get_meta_list()) {
		const String s = k;
		if (!s.begins_with("_hw")) {
			parts.push_back(s + String("=") + String(UtilityFunctions::var_to_str(p_node->get_meta(StringName(s)))));
		}
	}
	for (const Variant &g : p_node->get_groups()) {
		const String s = g;
		if (!s.begins_with("_")) {
			parts.push_back(String("@") + s);
		}
	}
	std::sort(parts.begin(), parts.end(), [](const String &a, const String &b) { return a < b; });
	String out;
	for (const String &p : parts) {
		out += p;
		out += String(";");
	}
	return out;
}

String mesh_key(MeshInstance3D *p_mi) {
	return tag_key(p_mi) + vformat("|%d|%d|%d|%f", (int64_t)p_mi->get_layer_mask(), (int)p_mi->get_cast_shadows_setting(), (int)p_mi->get_gi_mode(), p_mi->get_visibility_range_end()) +
			vformat("|%d", p_mi->get_material_override().is_valid() ? (int64_t)p_mi->get_material_override()->get_instance_id() : (int64_t)0);
}

bool plain_parent(Node *p_node) {
	const String c = p_node->get_class();
	const Ref<Script> s = p_node->get_script();
	return s.is_null() && (c == "Node3D" || c == "Node" || c == "Path3D" || c == "Marker3D");
}

bool read_surfaces(MeshInstance3D *p_mi, Item &r_item) {
	const Ref<Mesh> mesh = p_mi->get_mesh();
	if (mesh.is_null() || mesh->get_surface_count() == 0) {
		return false;
	}
	const Ref<ArrayMesh> am = mesh;
	for (int s = 0; s < mesh->get_surface_count(); s++) {
		if (am.is_valid() && (am->surface_get_primitive_type(s) != Mesh::PRIMITIVE_TRIANGLES || am->surface_get_blend_shape_arrays(s).size() > 0)) {
			return false;
		}
		Surf sf;
		sf.arr = mesh->surface_get_arrays(s);
		const PackedVector3Array v = sf.arr[Mesh::ARRAY_VERTEX];
		const PackedInt32Array idx = sf.arr[Mesh::ARRAY_INDEX];
		const PackedVector2Array uv2 = sf.arr[Mesh::ARRAY_TEX_UV2];
		const PackedInt32Array bones = sf.arr[Mesh::ARRAY_BONES];
		if (v.is_empty() || idx.is_empty() || !uv2.is_empty() || !bones.is_empty()) {
			return false; // (lightmaps have a place of their own in the atlas; skinned ones move)
		}
		sf.mat = p_mi->get_surface_override_material(s).is_valid() ? p_mi->get_surface_override_material(s) : mesh->surface_get_material(s);
		sf.flags = am.is_valid() ? ((uint64_t)am->surface_get_format(s) & CUSTOM_MASK) : 0;
		sf.key = vformat("%d|%d", sf.mat.is_valid() ? (int64_t)sf.mat->get_instance_id() : (int64_t)0, (int64_t)sf.flags);
		for (int a : { Mesh::ARRAY_NORMAL, Mesh::ARRAY_TANGENT, Mesh::ARRAY_COLOR, Mesh::ARRAY_TEX_UV, Mesh::ARRAY_CUSTOM0, Mesh::ARRAY_CUSTOM1, Mesh::ARRAY_CUSTOM2, Mesh::ARRAY_CUSTOM3 }) {
			sf.key += ((Variant)sf.arr[a]).get_type() == Variant::NIL ? "0" : "1";
		}
		r_item.triangles += (int)idx.size() / 3;
		r_item.surfs.push_back(sf);
	}
	return true;
}

bool eligible_children(MeshInstance3D *p_mi) {
	for (int i = 0; i < p_mi->get_child_count(); i++) {
		Node *c = p_mi->get_child(i);
		if (!Object::cast_to<StaticBody3D>(c) && !Object::cast_to<OccluderInstance3D>(c)) {
			return false;
		}
		for (int k = 0; k < c->get_child_count(); k++) {
			if (!Object::cast_to<CollisionShape3D>(c->get_child(k))) {
				return false;
			}
		}
	}
	return true;
}

void collect(Node *p_node, const Transform3D &p_xf, const Opt &p_opt, std::vector<Item> &r_items) {
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *c = p_node->get_child(i);
		if (!c->get_scene_file_path().is_empty()) {
			continue; // (a placed scene: its own)
		}
		Node3D *c3 = Object::cast_to<Node3D>(c);
		const Transform3D xf = c3 ? p_xf * c3->get_transform() : p_xf;
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(c);
		const Ref<Script> script = c->get_script();
		if (mi && (c->has_meta("_hw_merge") || (p_opt.plain && !c->has_meta("_hw_no_merge") && c->get_class() == "MeshInstance3D")) && script.is_null() && mi->is_visible() && mi->get_skin().is_null() && eligible_children(mi)) {
			Item it;
			it.mi = mi;
			it.xf = xf;
			if (read_surfaces(mi, it)) {
				it.box = xf.xform(mi->get_mesh()->get_aabb());
				r_items.push_back(it);
			}
		}
		if (plain_parent(c) && (!c3 || c3->is_visible())) {
			collect(c, xf, p_opt, r_items);
		}
	}
}

// ---- the faces no one can see

struct Solid {
	std::vector<Vector3> tris;
	AABB box;
	const Item *item = nullptr;
};

void world_triangles(const Item &p_item, std::vector<Vector3> &r_tris) {
	for (const Surf &s : p_item.surfs) {
		const PackedVector3Array v = s.arr[Mesh::ARRAY_VERTEX];
		const PackedInt32Array idx = s.arr[Mesh::ARRAY_INDEX];
		for (int64_t i = 0; i + 2 < idx.size(); i += 3) {
			r_tris.push_back(p_item.xf.xform(v[idx[i]]));
			r_tris.push_back(p_item.xf.xform(v[idx[i + 1]]));
			r_tris.push_back(p_item.xf.xform(v[idx[i + 2]]));
		}
	}
}

bool closed(const std::vector<Vector3> &p_tris) {
	std::map<std::pair<std::array<int64_t, 3>, std::array<int64_t, 3>>, int> edges;
	auto q = [](const Vector3 &p) { return std::array<int64_t, 3>{ (int64_t)std::llround(p.x * 1000.0), (int64_t)std::llround(p.y * 1000.0), (int64_t)std::llround(p.z * 1000.0) }; };
	for (size_t t = 0; t + 2 < p_tris.size(); t += 3) {
		for (int e = 0; e < 3; e++) {
			std::array<int64_t, 3> a = q(p_tris[t + e]), b = q(p_tris[t + (e + 1) % 3]);
			if (b < a) {
				std::swap(a, b);
			}
			edges[{ a, b }]++;
		}
	}
	for (const auto &kv : edges) {
		if (kv.second != 2) {
			return false;
		}
	}
	return !edges.empty();
}

bool inside(const Solid &p_s, const Vector3 &p_at) {
	if (!p_s.box.has_point(p_at)) {
		return false;
	}
	int above = 0;
	const float x = p_at.x + 1.3e-5f, z = p_at.z + 0.7e-5f; // (off the edges of a grid of quads)
	for (size_t t = 0; t + 2 < p_s.tris.size(); t += 3) {
		const Vector3 &a = p_s.tris[t], &b = p_s.tris[t + 1], &c = p_s.tris[t + 2];
		const float den = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
		if (std::fabs(den) < 1e-12f) {
			continue;
		}
		const float u = ((x - a.x) * (c.z - a.z) - (c.x - a.x) * (z - a.z)) / den;
		const float v = ((b.x - a.x) * (z - a.z) - (x - a.x) * (b.z - a.z)) / den;
		if (u >= 0.0f && v >= 0.0f && u + v <= 1.0f && a.y + (b.y - a.y) * u + (c.y - a.y) * v > p_at.y) {
			above++;
		}
	}
	return (above & 1) == 1;
}

void find_hidden(std::vector<Item> &r_items) {
	std::vector<Solid> solids;
	for (const Item &it : r_items) {
		Solid s;
		world_triangles(it, s.tris);
		if (closed(s.tris)) {
			s.box = it.box.grow(0.01f);
			s.item = &it;
			solids.push_back(std::move(s));
		}
	}
	for (Item &it : r_items) {
		const AABB ibox = it.box.grow(0.01f);
		std::vector<const Solid *> near;
		for (const Solid &s : solids) {
			if (s.item != &it && s.box.intersects(ibox)) {
				near.push_back(&s);
			}
		}
		it.hidden.assign(it.surfs.size(), {});
		if (near.empty()) {
			continue;
		}
		for (size_t sn = 0; sn < it.surfs.size(); sn++) {
			const PackedVector3Array v = it.surfs[sn].arr[Mesh::ARRAY_VERTEX];
			const PackedVector3Array n = it.surfs[sn].arr[Mesh::ARRAY_NORMAL];
			const PackedInt32Array idx = it.surfs[sn].arr[Mesh::ARRAY_INDEX];
			if (n.is_empty()) {
				continue;
			}
			const Basis nb = it.xf.basis.inverse().transposed();
			it.hidden[sn].assign(idx.size() / 3, 0);
			for (int64_t t = 0; t + 2 < idx.size(); t += 3) {
				const Vector3 a = it.xf.xform(v[idx[t]]), b = it.xf.xform(v[idx[t + 1]]), c = it.xf.xform(v[idx[t + 2]]);
				const Vector3 out = nb.xform(n[idx[t]] + n[idx[t + 1]] + n[idx[t + 2]]).normalized();
				const Vector3 mid = (a + b + c) / 3.0f;
				const Vector3 pts[4] = { mid, mid.lerp(a, 0.9f), mid.lerp(b, 0.9f), mid.lerp(c, 0.9f) };
				bool all = true;
				for (const Vector3 &q : pts) {
					bool in = false;
					for (const Solid *s : near) {
						in = in || inside(*s, q + out * 0.002f);
					}
					all = all && in;
				}
				if (all) {
					it.hidden[sn][t / 3] = 1;
					it.consumed = true;
				}
			}
		}
	}
}

// ---- joining

int64_t cell_key(int p_x, int p_z) {
	return ((int64_t)p_x << 32) | (uint32_t)p_z;
}

struct Buf {
	std::vector<Vector3> v, n;
	std::vector<float> t;
	std::vector<Color> c;
	std::vector<Vector2> uv;
	std::vector<float> cf[4];
	std::vector<uint8_t> cb[4];
	std::vector<int> idx;
	Ref<Material> mat;
	uint64_t flags = 0;
};

void append_vertex(Buf &b, const Array &p_arr, const Item &p_it, const Basis &p_nb, float p_tsign, int p_i, int p_verts) {
	const PackedVector3Array v = p_arr[Mesh::ARRAY_VERTEX];
	b.v.push_back(p_it.xf.xform(v[p_i]));
	const Variant na = p_arr[Mesh::ARRAY_NORMAL];
	if (na.get_type() != Variant::NIL) {
		const PackedVector3Array n = na;
		b.n.push_back(p_nb.xform(n[p_i]).normalized());
	}
	const Variant ta = p_arr[Mesh::ARRAY_TANGENT];
	if (ta.get_type() != Variant::NIL) {
		const PackedFloat32Array t = ta;
		const Vector3 d = p_it.xf.basis.xform(Vector3(t[p_i * 4], t[p_i * 4 + 1], t[p_i * 4 + 2])).normalized();
		b.t.push_back(d.x);
		b.t.push_back(d.y);
		b.t.push_back(d.z);
		b.t.push_back(t[p_i * 4 + 3] * p_tsign);
	}
	const Variant ca = p_arr[Mesh::ARRAY_COLOR];
	if (ca.get_type() != Variant::NIL) {
		const PackedColorArray c = ca;
		b.c.push_back(c[p_i]);
	}
	const Variant ua = p_arr[Mesh::ARRAY_TEX_UV];
	if (ua.get_type() != Variant::NIL) {
		const PackedVector2Array uv = ua;
		b.uv.push_back(uv[p_i]);
	}
	for (int ch = 0; ch < 4; ch++) {
		const Variant x = p_arr[Mesh::ARRAY_CUSTOM0 + ch];
		if (x.get_type() == Variant::PACKED_FLOAT32_ARRAY) {
			const PackedFloat32Array f = x;
			const int k = (int)(f.size() / p_verts);
			for (int e = 0; e < k; e++) {
				b.cf[ch].push_back(f[p_i * k + e]);
			}
		} else if (x.get_type() == Variant::PACKED_BYTE_ARRAY) {
			const PackedByteArray f = x;
			const int k = (int)(f.size() / p_verts);
			for (int e = 0; e < k; e++) {
				b.cb[ch].push_back(f[p_i * k + e]);
			}
		}
	}
}

template <typename P, typename T>
P to_packed(const std::vector<T> &p_v) {
	P out;
	out.resize((int64_t)p_v.size());
	for (size_t i = 0; i < p_v.size(); i++) {
		out.set((int64_t)i, p_v[i]);
	}
	return out;
}

void add_buffer(Ref<ArrayMesh> &r_mesh, const Buf &b) {
	Array arr;
	arr.resize(Mesh::ARRAY_MAX);
	arr[Mesh::ARRAY_VERTEX] = to_packed<PackedVector3Array>(b.v);
	if (!b.n.empty()) {
		arr[Mesh::ARRAY_NORMAL] = to_packed<PackedVector3Array>(b.n);
	}
	if (!b.t.empty()) {
		arr[Mesh::ARRAY_TANGENT] = to_packed<PackedFloat32Array>(b.t);
	}
	if (!b.c.empty()) {
		arr[Mesh::ARRAY_COLOR] = to_packed<PackedColorArray>(b.c);
	}
	if (!b.uv.empty()) {
		arr[Mesh::ARRAY_TEX_UV] = to_packed<PackedVector2Array>(b.uv);
	}
	for (int ch = 0; ch < 4; ch++) {
		if (!b.cf[ch].empty()) {
			arr[Mesh::ARRAY_CUSTOM0 + ch] = to_packed<PackedFloat32Array>(b.cf[ch]);
		} else if (!b.cb[ch].empty()) {
			PackedByteArray pb;
			pb.resize((int64_t)b.cb[ch].size());
			memcpy(pb.ptrw(), b.cb[ch].data(), b.cb[ch].size());
			arr[Mesh::ARRAY_CUSTOM0 + ch] = pb;
		}
	}
	arr[Mesh::ARRAY_INDEX] = to_packed<PackedInt32Array>(b.idx);
	r_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr, TypedArray<Array>(), Dictionary(), (BitField<Mesh::ArrayFormat>)b.flags);
	r_mesh->surface_set_material(r_mesh->get_surface_count() - 1, b.mat);
}

void make_lods(MeshInstance3D *p_mi) {
	const Ref<ArrayMesh> am = p_mi->get_mesh();
	if (am.is_null()) {
		return;
	}
	int tris = 0;
	for (int s = 0; s < am->get_surface_count(); s++) {
		const Variant n = am->surface_get_arrays(s)[Mesh::ARRAY_NORMAL];
		if (n.get_type() == Variant::NIL) {
			return;
		}
		tris += (int)am->surface_get_array_index_len(s) / 3;
	}
	if (tris < 300) {
		return; // (a block: no use)
	}
	Ref<ImporterMesh> im;
	im.instantiate();
	for (int s = 0; s < am->get_surface_count(); s++) {
		im->add_surface(Mesh::PRIMITIVE_TRIANGLES, am->surface_get_arrays(s), TypedArray<Array>(), Dictionary(), am->surface_get_material(s), String(), (uint64_t)am->surface_get_format(s) & CUSTOM_MASK);
	}
	im->generate_lods(25.0f, 60.0f, Array());
	p_mi->set_mesh(im->get_mesh());
}

void strip_marks(Node *p_node) {
	if (p_node->has_meta("_hw_merge")) {
		p_node->remove_meta("_hw_merge");
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		if (p_node->get_child(i)->get_scene_file_path().is_empty()) {
			strip_marks(p_node->get_child(i));
		}
	}
}

void copy_tags(Node *p_from, Node *p_to) {
	for (const Variant &k : p_from->get_meta_list()) {
		const String s = k;
		if (!s.begins_with("_hw")) {
			p_to->set_meta(StringName(s), p_from->get_meta(StringName(s)));
		}
	}
	for (const Variant &g : p_from->get_groups()) {
		if (!String(g).begins_with("_")) {
			p_to->add_to_group(g, true);
		}
	}
}

void leave_parent(Node *p_node) {
	if (p_node->get_parent()) {
		p_node->get_parent()->remove_child(p_node);
	}
}

// (a body, or an occluder, of a mesh that goes: where it was, under the root)
void lift(Node3D *p_node, const Transform3D &p_xf, Node *p_root) {
	const Transform3D at = p_xf * p_node->get_transform();
	leave_parent(p_node);
	p_node->set_transform(at);
	p_root->add_child(p_node, true);
}

void join_bodies(std::vector<Item> &r_items, const Opt &p_opt, Node *p_root) {
	struct B {
		StaticBody3D *body;
		Transform3D xf;
		Item *item;
	};
	std::map<String, std::vector<B>> groups;
	std::vector<B> alone;
	for (Item &it : r_items) {
		for (int i = it.mi->get_child_count() - 1; i >= 0; i--) {
			StaticBody3D *b = Object::cast_to<StaticBody3D>(it.mi->get_child(i));
			if (!b) {
				continue;
			}
			const Ref<Script> sc = b->get_script();
			if (!p_opt.collision || sc.is_valid()) {
				alone.push_back({ b, it.xf, &it });
				continue;
			}
			const Vector3 c = it.box.get_center();
			const Ref<PhysicsMaterial> pm = b->get_physics_material_override();
			const String key = tag_key(b) + vformat("|%d|%d|%d|%d|%d", (int64_t)b->get_collision_layer(), (int64_t)b->get_collision_mask(), pm.is_valid() ? (int64_t)pm->get_instance_id() : (int64_t)0,
					(int)std::floor(c.x / p_opt.cell), (int)std::floor(c.z / p_opt.cell));
			groups[key].push_back({ b, it.xf, &it });
		}
	}
	int n = 0;
	for (auto &kv : groups) {
		if (kv.second.size() < 2) {
			alone.push_back(kv.second[0]);
			continue;
		}
		StaticBody3D *first = kv.second[0].body;
		StaticBody3D *out = memnew(StaticBody3D);
		out->set_name(vformat("MergedBody%d", n++));
		out->set_collision_layer(first->get_collision_layer());
		out->set_collision_mask(first->get_collision_mask());
		out->set_physics_material_override(first->get_physics_material_override());
		copy_tags(first, out);
		p_root->add_child(out, true);
		for (B &b : kv.second) {
			const Transform3D bx = b.xf * b.body->get_transform();
			for (int i = b.body->get_child_count() - 1; i >= 0; i--) {
				CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(b.body->get_child(i));
				const Transform3D at = bx * cs->get_transform();
				b.body->remove_child(cs);
				cs->set_transform(at);
				out->add_child(cs, true);
			}
			leave_parent(b.body);
			memdelete(b.body);
		}
	}
	// (the bodies left: only moved, when their mesh goes)
	for (B &b : alone) {
		if (b.item->consumed) {
			lift(b.body, b.xf, p_root);
		}
	}
}

} // namespace

void HewnLevel::optimize_game_nodes(Node *p_root, const Dictionary &p_options) {
	Opt o;
	o.merge = p_options.get("merge", false);
	o.plain = p_options.get("merge_plain", false);
	o.collision = p_options.get("merge_collision", false);
	o.lod = p_options.get("lod", false);
	o.range = p_options.get("vis_range", false);
	o.hidden = p_options.get("hidden_faces", false);
	o.cell = MAX(1.0f, (float)p_options.get("merge_cell", 32.0f));
	if (!o.merge && !o.collision && !o.lod && !o.range && !o.hidden) {
		strip_marks(p_root);
		return;
	}
	std::vector<Item> items;
	collect(p_root, Transform3D(), o, items);
	if (o.range) {
		// (small things go first; in steps, so that the things of one size can be joined)
		for (Item &it : items) {
			if (it.mi->get_visibility_range_end() > 0.0f) {
				continue;
			}
			const float d = it.box.size.length() * 60.0f;
			float end = 0.0f;
			for (float step : { 25.0f, 50.0f, 100.0f, 200.0f, 400.0f, 800.0f }) {
				if (d <= step) {
					end = step;
					break;
				}
			}
			it.mi->set_visibility_range_end(end);
			it.mi->set_visibility_range_end_margin(end * 0.1f);
			it.mi->set_visibility_range_fade_mode(end > 0.0f ? GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF : GeometryInstance3D::VISIBILITY_RANGE_FADE_DISABLED);
		}
	}
	if (o.hidden) {
		find_hidden(items);
	}
	for (Item &it : items) {
		it.key = mesh_key(it.mi);
		it.split = o.merge && (it.box.size.x > o.cell || it.box.size.z > o.cell) && it.triangles >= 1000;
	}
	std::vector<MeshInstance3D *> lod_list;
	if (o.merge || o.hidden) {
		const float cell = o.cell;
		auto cell_of = [cell](float p_x, float p_z) { return cell_key((int)std::floor(p_x / cell), (int)std::floor(p_z / cell)); };
		// who shares a square: alone in it and whole, a mesh stays as it is
		std::map<std::pair<String, int64_t>, int> members;
		if (o.merge) {
			for (Item &it : items) {
				if (it.split) {
					it.consumed = true;
				} else {
					members[{ it.key, cell_of(it.box.get_center().x, it.box.get_center().z) }]++;
				}
			}
			for (Item &it : items) {
				if (!it.split && members[{ it.key, cell_of(it.box.get_center().x, it.box.get_center().z) }] > 1) {
					it.consumed = true;
				}
			}
		}
		struct Group {
			Item *first = nullptr;
			std::map<String, Buf> surfaces;
		};
		std::map<std::pair<String, int64_t>, Group> groups;
		for (Item &it : items) {
			if (!it.consumed) {
				continue;
			}
			const int64_t whole = cell_of(it.box.get_center().x, it.box.get_center().z);
			const bool flip = it.xf.basis.determinant() < 0.0f;
			const Basis nb = it.xf.basis.inverse().transposed();
			for (size_t sn = 0; sn < it.surfs.size(); sn++) {
				const Surf &s = it.surfs[sn];
				const PackedVector3Array v = s.arr[Mesh::ARRAY_VERTEX];
				const PackedInt32Array idx = s.arr[Mesh::ARRAY_INDEX];
				std::map<int64_t, std::unordered_map<int, int>> remaps;
				for (int64_t t = 0; t + 2 < idx.size(); t += 3) {
					if (sn < it.hidden.size() && !it.hidden[sn].empty() && it.hidden[sn][t / 3]) {
						continue;
					}
					int64_t ck = whole;
					if (it.split) {
						const Vector3 mid = (it.xf.xform(v[idx[t]]) + it.xf.xform(v[idx[t + 1]]) + it.xf.xform(v[idx[t + 2]])) / 3.0f;
						ck = cell_of(mid.x, mid.z);
					}
					Group &g = groups[{ it.key, ck }];
					if (!g.first) {
						g.first = &it;
					}
					Buf &b = g.surfaces[s.key];
					b.mat = s.mat;
					b.flags = s.flags;
					std::unordered_map<int, int> &map = remaps[ck];
					for (int c = 0; c < 3; c++) {
						const int vi = idx[t + (flip ? 2 - c : c)];
						auto f = map.find(vi);
						int nv;
						if (f == map.end()) {
							nv = (int)b.v.size();
							map[vi] = nv;
							append_vertex(b, s.arr, it, nb, flip ? -1.0f : 1.0f, vi, (int)v.size());
						} else {
							nv = f->second;
						}
						b.idx.push_back(nv);
					}
				}
			}
		}
		int n = 0;
		for (auto &kv : groups) {
			Ref<ArrayMesh> am;
			am.instantiate();
			for (auto &sv : kv.second.surfaces) {
				if (!sv.second.idx.empty()) {
					add_buffer(am, sv.second);
				}
			}
			if (am->get_surface_count() == 0) {
				continue;
			}
			MeshInstance3D *src = kv.second.first->mi;
			MeshInstance3D *out = memnew(MeshInstance3D);
			out->set_name(vformat("Merged%d", n++));
			out->set_mesh(am);
			out->set_material_override(src->get_material_override());
			out->set_layer_mask(src->get_layer_mask());
			out->set_cast_shadows_setting(src->get_cast_shadows_setting());
			out->set_gi_mode(src->get_gi_mode());
			out->set_visibility_range_end(src->get_visibility_range_end());
			out->set_visibility_range_end_margin(src->get_visibility_range_end_margin());
			out->set_visibility_range_fade_mode(src->get_visibility_range_fade_mode());
			copy_tags(src, out);
			p_root->add_child(out, true);
			lod_list.push_back(out);
		}
	}
	join_bodies(items, o, p_root);
	for (Item &it : items) {
		if (!it.consumed) {
			lod_list.push_back(it.mi);
			continue;
		}
		for (int i = it.mi->get_child_count() - 1; i >= 0; i--) {
			if (OccluderInstance3D *oc = Object::cast_to<OccluderInstance3D>(it.mi->get_child(i))) {
				lift(oc, it.xf, p_root);
			}
		}
		leave_parent(it.mi);
		memdelete(it.mi);
	}
	if (o.lod) {
		for (MeshInstance3D *mi : lod_list) {
			make_lods(mi);
		}
	}
	strip_marks(p_root);
}
