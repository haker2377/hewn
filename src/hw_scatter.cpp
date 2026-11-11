#include "hw_scatter.h"
#include "hw_game_tags.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/concave_polygon_shape3d.hpp>
#include <godot_cpp/classes/convex_polygon_shape3d.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace godot;

namespace {
constexpr int STRIDE = 13; // (12 floats of the transform, the mesh's index)

int64_t chunk_coord(uint64_t p_key, bool p_z) {
	return (int64_t)(int32_t)(uint32_t)(p_z ? (p_key & 0xffffffffu) : (p_key >> 32));
}

uint64_t pos_key(const Vector3 &p) {
	uint32_t b[3];
	std::memcpy(b, &p, sizeof(b));
	return ((uint64_t)b[0] * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)b[1] * 0xC2B2AE3D27D4EB4Full) ^ ((uint64_t)b[2] * 0x165667B19E3779F9ull);
}
} // namespace

uint64_t HewnScatter3D::_key_of(const Vector3 &p_local) const {
	const int32_t x = (int32_t)std::floor(p_local.x / chunk_size), z = (int32_t)std::floor(p_local.z / chunk_size);
	return ((uint64_t)(uint32_t)x << 32) | (uint64_t)(uint32_t)z;
}

void HewnScatter3D::_mark(uint64_t p_key) {
	dirty.insert(p_key);
	if (!flush_queued && is_inside_tree()) {
		flush_queued = true;
		call_deferred("flush");
	}
}

float HewnScatter3D::_rand(float p_a, float p_b) {
	return p_a + (p_b - p_a) * std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
}

int HewnScatter3D::_pick_item() {
	const int n = (int)meshes.size();
	// (only the meshes picked in its panel, by their weights)
	auto on = [&](int i) { return paint_items.is_empty() || paint_items.has(i); };
	auto w = [&](int i) { return on(i) ? (i < weights.size() ? MAX(0.0f, weights[i]) : 1.0f) : 0.0f; };
	if (n <= 1) {
		return 0;
	}
	float total = 0.0f;
	int last = -1;
	for (int i = 0; i < n; i++) {
		total += w(i);
		if (w(i) > 0.0f) {
			last = i; // (the fallback: the last one that can be picked, never a weight of 0)
		}
	}
	if (last < 0) {
		for (int i = 0; i < n; i++) {
			last = on(i) && last < 0 ? i : last;
		}
	}
	if (total <= 0.0f) {
		return last >= 0 ? last : 0;
	}
	float r = _rand(0.0f, total);
	for (int i = 0; i < n; i++) {
		r -= w(i);
		if (w(i) > 0.0f && r <= 0.0f) {
			return i;
		}
	}
	return last >= 0 ? last : n - 1;
}

// The nearest two copies may stand: random copies kept that far apart fill about `density` per square meter.
float HewnScatter3D::_spacing() const {
	return std::sqrt(0.6965f / density);
}

void HewnScatter3D::_clear_nodes() {
	for (auto &kv : chunk_nodes) {
		for (ObjectID id : kv.second) {
			if (Node *n = Object::cast_to<Node>(ObjectDB::get_instance(id))) {
				n->queue_free();
				if (n->get_parent()) {
					n->get_parent()->remove_child(n);
				}
			}
		}
	}
	chunk_nodes.clear();
}

void HewnScatter3D::_rebuild_chunk(uint64_t p_key) {
	last_rebuilt++;
	std::vector<ObjectID> &ids = chunk_nodes[p_key];
	auto it = chunks.find(p_key);
	const int n_items = (int)meshes.size();
	if (it != chunks.end() && it->second.empty()) {
		chunks.erase(it);
		it = chunks.end();
	}
	const std::vector<Inst> empty;
	const std::vector<Inst> &list = it != chunks.end() ? it->second : empty;
	std::vector<ObjectID> keep(MAX(n_items, (int)ids.size()));
	for (int i = 0; i < (int)keep.size(); i++) {
		MultiMeshInstance3D *mmi = i < (int)ids.size() ? Object::cast_to<MultiMeshInstance3D>(ObjectDB::get_instance(ids[i])) : nullptr;
		const Ref<Mesh> mesh = i < n_items ? Ref<Mesh>(meshes[i]) : Ref<Mesh>();
		int count = 0;
		if (mesh.is_valid()) {
			for (const Inst &in : list) {
				count += in.item == i ? 1 : 0;
			}
		}
		if (count == 0) {
			if (mmi) {
				remove_child(mmi);
				mmi->queue_free();
			}
			continue;
		}
		if (!mmi) {
			mmi = memnew(MultiMeshInstance3D);
			mmi->set_name(vformat("Chunk_%d_%d_%d", (int)chunk_coord(p_key, false), (int)chunk_coord(p_key, true), i));
			add_child(mmi, false, INTERNAL_MODE_BACK);
		}
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_mesh(mesh);
		mm->set_instance_count(count);
		int k = 0;
		for (const Inst &in : list) {
			if (in.item == i) {
				mm->set_instance_transform(k++, in.xf);
			}
		}
		mmi->set_multimesh(mm);
		mmi->set_visibility_range_end(visibility_range);
		mmi->set_visibility_range_end_margin(visibility_range > 0.0f ? visibility_range * 0.1f : 0.0f);
		mmi->set_visibility_range_fade_mode(visibility_range > 0.0f ? GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF : GeometryInstance3D::VISIBILITY_RANGE_FADE_DISABLED);
		mmi->set_cast_shadows_setting(cast_shadows ? GeometryInstance3D::SHADOW_CASTING_SETTING_ON : GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		if (!Engine::get_singleton()->is_editor_hint()) {
			hewn::apply_tags(mmi, game_metadata, game_groups);
			hewn::apply_script(mmi, mesh_script);
		}
		keep[i] = mmi->get_instance_id();
	}
	while (!keep.empty() && !keep.back().is_valid()) {
		keep.pop_back();
	}
	if (keep.empty()) {
		chunk_nodes.erase(p_key);
	} else {
		chunk_nodes[p_key] = keep;
	}
}

void HewnScatter3D::_rebuild_all() {
	_clear_nodes();
	dirty.clear();
	if (!is_inside_tree()) {
		return;
	}
	for (const auto &kv : chunks) {
		_rebuild_chunk(kv.first);
	}
}

int HewnScatter3D::flush() {
	flush_queued = false;
	last_rebuilt = 0;
	if (!is_inside_tree()) {
		return 0;
	}
	const std::set<uint64_t> d = dirty;
	dirty.clear();
	for (uint64_t k : d) {
		_rebuild_chunk(k);
	}
	if (show_collision && !d.empty()) {
		update_gizmos(); // (the copies' shapes drawn where they are now)
	}
	return last_rebuilt;
}

void HewnScatter3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		_rebuild_all();
	}
}

// ------------------------------------------------------------------ properties

void HewnScatter3D::set_meshes(const TypedArray<Mesh> &p_meshes) {
	meshes = p_meshes;
	_rebuild_all();
}

void HewnScatter3D::set_weights(const PackedFloat32Array &p_weights) {
	weights = p_weights;
}

void HewnScatter3D::set_chunk_size(float p_v) {
	p_v = MAX(1.0f, p_v);
	if (p_v == chunk_size) {
		return;
	}
	const PackedFloat32Array data = get_instances();
	chunk_size = p_v;
	set_instances(data); // (sorted into the new chunks)
}

void HewnScatter3D::set_visibility_range(float p_v) {
	visibility_range = MAX(0.0f, p_v);
	_rebuild_all();
}

void HewnScatter3D::set_cast_shadows(bool p_v) {
	cast_shadows = p_v;
	_rebuild_all();
}

PackedFloat32Array HewnScatter3D::get_instances() const {
	PackedFloat32Array out;
	out.resize((int64_t)get_instance_count() * STRIDE);
	float *w = out.ptrw();
	for (const auto &kv : chunks) {
		for (const Inst &in : kv.second) {
			for (int c = 0; c < 3; c++) {
				const Vector3 col = in.xf.basis.get_column(c);
				*w++ = col.x;
				*w++ = col.y;
				*w++ = col.z;
			}
			*w++ = in.xf.origin.x;
			*w++ = in.xf.origin.y;
			*w++ = in.xf.origin.z;
			*w++ = (float)in.item;
		}
	}
	return out;
}

// Only the chunks whose copies differ are built again (an undo of one stroke: its chunks).
void HewnScatter3D::set_instances(const PackedFloat32Array &p_data) {
	std::map<uint64_t, std::vector<Inst>> next;
	const float *r = p_data.ptr();
	for (int64_t i = 0; i + STRIDE <= p_data.size(); i += STRIDE) {
		Inst in;
		for (int c = 0; c < 3; c++) {
			in.xf.basis.set_column(c, Vector3(r[i + c * 3], r[i + c * 3 + 1], r[i + c * 3 + 2]));
		}
		in.xf.origin = Vector3(r[i + 9], r[i + 10], r[i + 11]);
		in.item = MAX(0, (int)r[i + 12]);
		next[_key_of(in.xf.origin)].push_back(in);
	}
	auto same = [](const std::vector<Inst> &a, const std::vector<Inst> &b) {
		if (a.size() != b.size()) {
			return false;
		}
		for (size_t i = 0; i < a.size(); i++) {
			if (a[i].item != b[i].item || !(a[i].xf == b[i].xf)) {
				return false;
			}
		}
		return true;
	};
	std::set<uint64_t> changed;
	for (const auto &kv : chunks) {
		auto it = next.find(kv.first);
		if (it == next.end() || !same(kv.second, it->second)) {
			changed.insert(kv.first);
		}
	}
	for (const auto &kv : next) {
		if (!chunks.count(kv.first)) {
			changed.insert(kv.first);
		}
	}
	// (a chunk size changed: the old nodes don't match any key now)
	for (const auto &kv : chunk_nodes) {
		if (!next.count(kv.first)) {
			changed.insert(kv.first);
		}
	}
	chunks = std::move(next);
	for (uint64_t k : changed) {
		_mark(k);
	}
	if (!is_inside_tree()) {
		dirty.clear();
	}
}

// ------------------------------------------------------------------ painting

bool HewnScatter3D::paint_point(const Vector3 &p_pos, const Vector3 &p_normal) {
	const Transform3D inv = get_global_transform().affine_inverse();
	const Vector3 lp = inv.xform(p_pos);
	Vector3 ln = inv.basis.xform(p_normal).normalized();
	if (ln.length_squared() < 0.5f) {
		ln = Vector3(0, 1, 0);
	}
	// nearer than the spacing to another copy: none here
	const float sp = _spacing();
	const int32_t x0 = (int32_t)std::floor((lp.x - sp) / chunk_size), x1 = (int32_t)std::floor((lp.x + sp) / chunk_size);
	const int32_t z0 = (int32_t)std::floor((lp.z - sp) / chunk_size), z1 = (int32_t)std::floor((lp.z + sp) / chunk_size);
	const float sp2 = sp * sp;
	for (int32_t x = x0; x <= x1; x++) {
		for (int32_t z = z0; z <= z1; z++) {
			auto it = chunks.find(((uint64_t)(uint32_t)x << 32) | (uint64_t)(uint32_t)z);
			if (it == chunks.end()) {
				continue;
			}
			for (const Inst &in : it->second) {
				if (in.xf.origin.distance_squared_to(lp) < sp2) {
					return false;
				}
			}
		}
	}
	const Vector3 up = align == ALIGN_NORMAL ? ln : Vector3(0, 1, 0);
	// a basis with `up` as its Y
	const Vector3 ref = std::fabs(up.z) < 0.9f ? Vector3(0, 0, 1) : Vector3(1, 0, 0);
	const Vector3 bx = up.cross(ref).normalized();
	const Vector3 bz = bx.cross(up).normalized();
	Basis b(bx, up, bz); // (its columns: X, up, Z)
	b = b * Basis(Vector3(0, 1, 0), Math::deg_to_rad(_rand(0.0f, random_rotation)));
	if (random_tilt > 0.0f) {
		const float ta = _rand(0.0f, (float)Math::PI * 2.0f);
		b = b * Basis(Vector3(std::cos(ta), 0, std::sin(ta)), Math::deg_to_rad(_rand(0.0f, random_tilt)));
	}
	if (random_rotation_x > 0.0f) {
		b = b * Basis(Vector3(1, 0, 0), Math::deg_to_rad(_rand(0.0f, random_rotation_x)));
	}
	if (random_rotation_z > 0.0f) {
		b = b * Basis(Vector3(0, 0, 1), Math::deg_to_rad(_rand(0.0f, random_rotation_z)));
	}
	if (!base_rotation.is_zero_approx()) {
		b = b * Basis::from_euler(Vector3(Math::deg_to_rad(base_rotation.x), Math::deg_to_rad(base_rotation.y), Math::deg_to_rad(base_rotation.z)));
	}
	const float s = _rand(MIN(scale_min, scale_max), MAX(scale_min, scale_max));
	b.scale_local(Vector3(s, s, s));
	Inst in;
	in.xf = Transform3D(b, lp + up * offset);
	in.item = _pick_item();
	const uint64_t k = _key_of(lp);
	chunks[k].push_back(in);
	_mark(k);
	return true;
}

int HewnScatter3D::erase(const Vector3 &p_center, float p_radius, float p_fraction) {
	const Transform3D inv = get_global_transform().affine_inverse();
	const Vector3 lc = inv.xform(p_center);
	// (a scaled node: the radius in its own units)
	const float lr = p_radius * inv.basis.get_scale().x;
	const int32_t x0 = (int32_t)std::floor((lc.x - lr) / chunk_size), x1 = (int32_t)std::floor((lc.x + lr) / chunk_size);
	const int32_t z0 = (int32_t)std::floor((lc.z - lr) / chunk_size), z1 = (int32_t)std::floor((lc.z + lr) / chunk_size);
	int removed = 0;
	const bool all = p_fraction >= 1.0f;
	for (int32_t x = x0; x <= x1; x++) {
		for (int32_t z = z0; z <= z1; z++) {
			const uint64_t key = ((uint64_t)(uint32_t)x << 32) | (uint64_t)(uint32_t)z;
			auto it = chunks.find(key);
			if (it == chunks.end()) {
				continue;
			}
			std::vector<Inst> &list = it->second;
			const size_t before = list.size();
			list.erase(std::remove_if(list.begin(), list.end(), [&](const Inst &in) {
				if (in.xf.origin.distance_to(lc) > lr) {
					return false;
				}
				if (all) {
					return true;
				}
				if (!stroke_seen.insert(pos_key(in.xf.origin)).second) {
					return false; // (already had its chance in this stroke)
				}
				return _rand(0.0f, 1.0f) < p_fraction;
			}),
					list.end());
			if (list.size() != before) {
				removed += (int)(before - list.size());
				if (list.empty()) {
					chunks.erase(it);
				}
				_mark(key);
			}
		}
	}
	return removed;
}

void HewnScatter3D::clear() {
	for (const auto &kv : chunks) {
		_mark(kv.first);
	}
	chunks.clear();
}

int HewnScatter3D::get_instance_count() const {
	int n = 0;
	for (const auto &kv : chunks) {
		n += (int)kv.second.size();
	}
	return n;
}

int HewnScatter3D::get_chunk_count() const {
	return (int)chunk_nodes.size();
}

bool HewnScatter3D::show_collision = false;

std::vector<Ref<Shape3D>> HewnScatter3D::_collision_shapes() const {
	std::vector<Ref<Shape3D>> shapes(meshes.size());
	for (int i = 0; i < (int)meshes.size(); i++) {
		const Ref<Mesh> mesh = meshes[i];
		const int kind = i < collisions.size() ? (int)collisions[i] : 0;
		if (mesh.is_null() || kind <= 0) {
			continue;
		}
		if (kind == 2) {
			shapes[i] = mesh->create_trimesh_shape();
		} else if (kind == 3) {
			const AABB box = mesh->get_aabb();
			Ref<BoxShape3D> bs;
			bs.instantiate();
			bs->set_size(box.size.max(Vector3(0.001f, 0.001f, 0.001f)));
			shapes[i] = bs;
			shapes[i]->set_meta("hw_center", box.get_center());
		} else {
			shapes[i] = mesh->create_convex_shape(true, true);
		}
	}
	return shapes;
}

PackedVector3Array HewnScatter3D::collision_lines(int p_max) const {
	PackedVector3Array out;
	const std::vector<Ref<Shape3D>> shapes = _collision_shapes();
	std::vector<PackedVector3Array> lines(shapes.size());
	for (size_t i = 0; i < shapes.size(); i++) {
		Ref<ArrayMesh> dm = shapes[i].is_valid() ? shapes[i]->get_debug_mesh() : Ref<ArrayMesh>();
		if (dm.is_valid() && dm->get_surface_count() > 0) {
			lines[i] = PackedVector3Array(dm->surface_get_arrays(0)[Mesh::ARRAY_VERTEX]);
		}
	}
	int count = 0;
	for (const auto &kv : chunks) {
		for (const Inst &in : kv.second) {
			if (in.item < 0 || in.item >= (int)lines.size() || lines[in.item].is_empty() || count >= p_max) {
				continue;
			}
			count++;
			Transform3D xf = in.xf;
			if (shapes[in.item]->has_meta("hw_center")) {
				xf = xf * Transform3D(Basis(), (Vector3)shapes[in.item]->get_meta("hw_center"));
			}
			const PackedVector3Array &l = lines[in.item];
			for (int64_t k = 0; k < l.size(); k++) {
				out.push_back(xf.xform(l[k]));
			}
		}
	}
	return out;
}

void HewnScatter3D::each_collision(const std::function<void(const Ref<Shape3D> &, const Transform3D &)> &p_fn) const {
	const std::vector<Ref<Shape3D>> shapes = _collision_shapes();
	const Transform3D g = get_global_transform();
	for (const auto &kv : chunks) {
		for (const Inst &in : kv.second) {
			if (in.item < 0 || in.item >= (int)shapes.size() || shapes[in.item].is_null()) {
				continue;
			}
			Transform3D xf = in.xf;
			if (shapes[in.item]->has_meta("hw_center")) {
				xf = xf * Transform3D(Basis(), (Vector3)shapes[in.item]->get_meta("hw_center"));
			}
			p_fn(shapes[in.item], g * xf);
		}
	}
}

Node *HewnScatter3D::make_game_node() {
	flush();
	Node3D *root = memnew(Node3D);
	root->set_name(get_name());
	root->set_transform(get_transform());
	root->set_visible(is_visible());
	for (int i = 0; i < get_child_count(true); i++) {
		MultiMeshInstance3D *mmi = Object::cast_to<MultiMeshInstance3D>(get_child(i, true));
		if (!mmi || mmi->get_multimesh().is_null()) {
			continue;
		}
		MultiMeshInstance3D *c = memnew(MultiMeshInstance3D);
		c->set_name(mmi->get_name());
		c->set_multimesh(mmi->get_multimesh()); // (never changed: a stroke makes new ones)
		c->set_visibility_range_end(mmi->get_visibility_range_end());
		c->set_visibility_range_end_margin(mmi->get_visibility_range_end_margin());
		c->set_visibility_range_fade_mode(mmi->get_visibility_range_fade_mode());
		c->set_cast_shadows_setting(mmi->get_cast_shadows_setting());
		hewn::apply_tags(c, game_metadata, game_groups);
		hewn::apply_script(c, mesh_script);
		root->add_child(c);
	}
	// the meshes that collide: a body per chunk and mesh, a shape for each copy (one shape per mesh, shared)
	const std::vector<Ref<Shape3D>> shapes = _collision_shapes();
	for (const auto &kv : chunks) {
		std::vector<StaticBody3D *> bodies(shapes.size(), nullptr);
		for (const Inst &in : kv.second) {
			if (in.item < 0 || in.item >= (int)shapes.size() || shapes[in.item].is_null()) {
				continue;
			}
			StaticBody3D *&body = bodies[in.item];
			if (!body) {
				body = memnew(StaticBody3D);
				body->set_name(vformat("Collision_%d_%d_%d", (int)chunk_coord(kv.first, false), (int)chunk_coord(kv.first, true), in.item));
				body->set_collision_layer(collision_layer);
				hewn::apply_tags(body, game_metadata, game_groups);
				hewn::apply_script(body, collision_script);
				root->add_child(body);
			}
			CollisionShape3D *cs = memnew(CollisionShape3D);
			cs->set_shape(shapes[in.item]);
			Transform3D xf = in.xf;
			if (shapes[in.item]->has_meta("hw_center")) {
				xf = xf * Transform3D(Basis(), (Vector3)shapes[in.item]->get_meta("hw_center"));
			}
			cs->set_transform(xf);
			body->add_child(cs);
		}
	}
	return root;
}

void HewnScatter3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_game_metadata", "v"), &HewnScatter3D::set_game_metadata);
	ClassDB::bind_method(D_METHOD("get_game_metadata"), &HewnScatter3D::get_game_metadata);
	ClassDB::bind_method(D_METHOD("set_game_groups", "v"), &HewnScatter3D::set_game_groups);
	ClassDB::bind_method(D_METHOD("get_game_groups"), &HewnScatter3D::get_game_groups);
	ClassDB::bind_method(D_METHOD("set_mesh_script", "v"), &HewnScatter3D::set_mesh_script);
	ClassDB::bind_method(D_METHOD("get_mesh_script"), &HewnScatter3D::get_mesh_script);
	ClassDB::bind_method(D_METHOD("set_collision_script", "v"), &HewnScatter3D::set_collision_script);
	ClassDB::bind_method(D_METHOD("get_collision_script"), &HewnScatter3D::get_collision_script);
	ClassDB::bind_method(D_METHOD("set_meshes", "meshes"), &HewnScatter3D::set_meshes);
	ClassDB::bind_method(D_METHOD("get_meshes"), &HewnScatter3D::get_meshes);
	ClassDB::bind_method(D_METHOD("set_weights", "weights"), &HewnScatter3D::set_weights);
	ClassDB::bind_method(D_METHOD("get_weights"), &HewnScatter3D::get_weights);
	ClassDB::bind_method(D_METHOD("set_density", "v"), &HewnScatter3D::set_density);
	ClassDB::bind_method(D_METHOD("get_density"), &HewnScatter3D::get_density);
	ClassDB::bind_method(D_METHOD("set_brush_radius", "v"), &HewnScatter3D::set_brush_radius);
	ClassDB::bind_method(D_METHOD("set_paint_items", "v"), &HewnScatter3D::set_paint_items);
	ClassDB::bind_method(D_METHOD("get_paint_items"), &HewnScatter3D::get_paint_items);
	ClassDB::bind_method(D_METHOD("set_erase_strength", "v"), &HewnScatter3D::set_erase_strength);
	ClassDB::bind_method(D_METHOD("get_erase_strength"), &HewnScatter3D::get_erase_strength);
	ClassDB::bind_method(D_METHOD("get_brush_radius"), &HewnScatter3D::get_brush_radius);
	ClassDB::bind_method(D_METHOD("set_scale_min", "v"), &HewnScatter3D::set_scale_min);
	ClassDB::bind_method(D_METHOD("get_scale_min"), &HewnScatter3D::get_scale_min);
	ClassDB::bind_method(D_METHOD("set_scale_max", "v"), &HewnScatter3D::set_scale_max);
	ClassDB::bind_method(D_METHOD("get_scale_max"), &HewnScatter3D::get_scale_max);
	ClassDB::bind_method(D_METHOD("set_random_rotation", "v"), &HewnScatter3D::set_random_rotation);
	ClassDB::bind_method(D_METHOD("get_random_rotation"), &HewnScatter3D::get_random_rotation);
	ClassDB::bind_method(D_METHOD("set_random_tilt", "v"), &HewnScatter3D::set_random_tilt);
	ClassDB::bind_method(D_METHOD("get_random_tilt"), &HewnScatter3D::get_random_tilt);
	ClassDB::bind_method(D_METHOD("set_random_rotation_x", "v"), &HewnScatter3D::set_random_rotation_x);
	ClassDB::bind_method(D_METHOD("get_random_rotation_x"), &HewnScatter3D::get_random_rotation_x);
	ClassDB::bind_method(D_METHOD("set_random_rotation_z", "v"), &HewnScatter3D::set_random_rotation_z);
	ClassDB::bind_method(D_METHOD("get_random_rotation_z"), &HewnScatter3D::get_random_rotation_z);
	ClassDB::bind_method(D_METHOD("set_base_rotation", "v"), &HewnScatter3D::set_base_rotation);
	ClassDB::bind_method(D_METHOD("get_base_rotation"), &HewnScatter3D::get_base_rotation);
	ClassDB::bind_method(D_METHOD("set_collisions", "v"), &HewnScatter3D::set_collisions);
	ClassDB::bind_method(D_METHOD("get_collisions"), &HewnScatter3D::get_collisions);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "v"), &HewnScatter3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &HewnScatter3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_align", "v"), &HewnScatter3D::set_align);
	ClassDB::bind_method(D_METHOD("get_align"), &HewnScatter3D::get_align);
	ClassDB::bind_method(D_METHOD("set_offset", "v"), &HewnScatter3D::set_offset);
	ClassDB::bind_method(D_METHOD("get_offset"), &HewnScatter3D::get_offset);
	ClassDB::bind_method(D_METHOD("set_chunk_size", "v"), &HewnScatter3D::set_chunk_size);
	ClassDB::bind_method(D_METHOD("get_chunk_size"), &HewnScatter3D::get_chunk_size);
	ClassDB::bind_method(D_METHOD("set_visibility_range", "v"), &HewnScatter3D::set_visibility_range);
	ClassDB::bind_method(D_METHOD("get_visibility_range"), &HewnScatter3D::get_visibility_range);
	ClassDB::bind_method(D_METHOD("set_cast_shadows", "v"), &HewnScatter3D::set_cast_shadows);
	ClassDB::bind_method(D_METHOD("get_cast_shadows"), &HewnScatter3D::get_cast_shadows);
	ClassDB::bind_method(D_METHOD("set_instances", "data"), &HewnScatter3D::set_instances);
	ClassDB::bind_method(D_METHOD("get_instances"), &HewnScatter3D::get_instances);
	ClassDB::bind_method(D_METHOD("paint_point", "position", "normal"), &HewnScatter3D::paint_point);
	ClassDB::bind_method(D_METHOD("erase", "center", "radius", "fraction"), &HewnScatter3D::erase);
	ClassDB::bind_method(D_METHOD("begin_stroke"), &HewnScatter3D::begin_stroke);
	ClassDB::bind_method(D_METHOD("clear"), &HewnScatter3D::clear);
	ClassDB::bind_method(D_METHOD("flush"), &HewnScatter3D::flush);
	ClassDB::bind_method(D_METHOD("get_instance_count"), &HewnScatter3D::get_instance_count);
	ClassDB::bind_method(D_METHOD("get_chunk_count"), &HewnScatter3D::get_chunk_count);
	ClassDB::bind_method(D_METHOD("get_last_rebuilt"), &HewnScatter3D::get_last_rebuilt);
	ClassDB::bind_method(D_METHOD("make_game_node"), &HewnScatter3D::make_game_node);

	ADD_GROUP("Metadata, Groups and Scripts", "");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "game_metadata"), "set_game_metadata", "get_game_metadata");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "game_groups"), "set_game_groups", "get_game_groups");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesh_script", PROPERTY_HINT_RESOURCE_TYPE, "Script"), "set_mesh_script", "get_mesh_script");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "collision_script", PROPERTY_HINT_RESOURCE_TYPE, "Script"), "set_collision_script", "get_collision_script");
	ADD_GROUP("Meshes", "");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "meshes", PROPERTY_HINT_TYPE_STRING, vformat("%d/%d:Mesh", Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE)), "set_meshes", "get_meshes");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "weights"), "set_weights", "get_weights");
	// (per mesh, in the game)
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "collisions", PROPERTY_HINT_TYPE_STRING, vformat("%d/%d:None,Convex,Trimesh,Box", Variant::INT, PROPERTY_HINT_ENUM)), "set_collisions", "get_collisions");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_GROUP("Brush", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "0.001,100,0.01,or_greater,suffix:/m²"), "set_density", "get_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "brush_radius", PROPERTY_HINT_RANGE, "0.05,100,0.01,or_greater,suffix:m"), "set_brush_radius", "get_brush_radius");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "paint_items", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_paint_items", "get_paint_items");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "erase_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_erase_strength", "get_erase_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scale_min", PROPERTY_HINT_RANGE, "0.01,10,0.01,or_greater"), "set_scale_min", "get_scale_min");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scale_max", PROPERTY_HINT_RANGE, "0.01,10,0.01,or_greater"), "set_scale_max", "get_scale_max");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "random_rotation", PROPERTY_HINT_RANGE, "0,360,1,degrees"), "set_random_rotation", "get_random_rotation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "random_tilt", PROPERTY_HINT_RANGE, "0,90,1,degrees"), "set_random_tilt", "get_random_tilt");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "random_rotation_x", PROPERTY_HINT_RANGE, "0,360,1,degrees"), "set_random_rotation_x", "get_random_rotation_x");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "random_rotation_z", PROPERTY_HINT_RANGE, "0,360,1,degrees"), "set_random_rotation_z", "get_random_rotation_z");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "base_rotation", PROPERTY_HINT_RANGE, "-360,360,0.1,or_less,or_greater,suffix:°"), "set_base_rotation", "get_base_rotation");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "align", PROPERTY_HINT_ENUM, "Up,Normal"), "set_align", "get_align");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "offset", PROPERTY_HINT_RANGE, "-10,10,0.01,or_less,or_greater,suffix:m"), "set_offset", "get_offset");
	ADD_GROUP("Drawing", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "chunk_size", PROPERTY_HINT_RANGE, "1,256,0.5,or_greater,suffix:m"), "set_chunk_size", "get_chunk_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range", PROPERTY_HINT_RANGE, "0,10000,0.1,or_greater,suffix:m"), "set_visibility_range", "get_visibility_range");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_shadows"), "set_cast_shadows", "get_cast_shadows");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "instances", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_instances", "get_instances");
}
