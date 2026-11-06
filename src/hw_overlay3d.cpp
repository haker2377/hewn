#include "hw_overlay3d.h"

#include "hewn_mesh.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cmath>
#include <map>

using namespace godot;

namespace {

// ------------------------------------------------------------------ batches

struct Batch {
	RID scenario;
	Ref<Material> material;
	RID mesh;
	RID instance;
	std::vector<HewnOverlay3D *> members;
	bool dirty = false;
};

std::vector<Batch *> g_batches;
bool g_flush_queued = false;
std::map<String, Ref<StandardMaterial3D>> g_materials;

void queue_flush() {
	if (!g_flush_queued) {
		g_flush_queued = true;
		callable_mp_static(&HewnOverlay3D::flush_batches).call_deferred();
	}
}

void leave(HewnOverlay3D *p_overlay, void *&r_batch) {
	Batch *b = (Batch *)r_batch;
	r_batch = nullptr;
	if (!b) {
		return;
	}
	b->members.erase(std::remove(b->members.begin(), b->members.end(), p_overlay), b->members.end());
	b->dirty = true;
	queue_flush();
}

// ------------------------------------------------------------------ geometry helpers

// Sutherland-Hodgman: keeps the part where f(p) >= 0 (f linear).
template <typename F>
std::vector<Vector3> clip(const std::vector<Vector3> &poly, F f) {
	std::vector<Vector3> out;
	const size_t n = poly.size();
	for (size_t i = 0; i < n && n >= 3; i++) {
		const Vector3 &a = poly[i], &b = poly[(i + 1) % n];
		const float da = f(a), db = f(b);
		if (da >= 0.0f) {
			out.push_back(a);
		}
		if ((da >= 0.0f) != (db >= 0.0f) && std::fabs(da - db) > 1e-12f) {
			out.push_back(a + (b - a) * (da / (da - db)));
		}
	}
	return out;
}

float cross2(const Vector2 &a, const Vector2 &b) {
	return a.x * b.y - a.y * b.x;
}

std::vector<Vector2> convex_hull(std::vector<Vector2> p) {
	std::sort(p.begin(), p.end(), [](const Vector2 &a, const Vector2 &b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
	p.erase(std::unique(p.begin(), p.end(), [](const Vector2 &a, const Vector2 &b) { return a.is_equal_approx(b); }), p.end());
	if (p.size() < 3) {
		return p;
	}
	std::vector<Vector2> h(p.size() * 2);
	size_t k = 0;
	for (size_t i = 0; i < p.size(); i++) {
		while (k >= 2 && cross2(h[k - 1] - h[k - 2], p[i] - h[k - 2]) <= 0.0f) {
			k--;
		}
		h[k++] = p[i];
	}
	for (size_t i = p.size() - 1, t = k + 1; i > 0; i--) {
		while (k >= t && cross2(h[k - 1] - h[k - 2], p[i - 1] - h[k - 2]) <= 0.0f) {
			k--;
		}
		h[k++] = p[i - 1];
	}
	h.resize(k - 1);
	return h; // counter-clockwise
}

// Fewer corners, still around the whole hull: an edge goes, its neighbors grow to meet (the least area added).
std::vector<Vector2> reduce_hull(std::vector<Vector2> h, int p_max) {
	while ((int)h.size() > std::max(3, p_max)) {
		const int n = (int)h.size();
		int best = -1;
		float best_area = 1e30f;
		Vector2 best_pt;
		for (int i = 0; i < n; i++) {
			// remove edge i -> i+1: lines (i-1, i) and (i+1, i+2) meet at x
			const Vector2 a0 = h[(i + n - 1) % n], a1 = h[i], b0 = h[(i + 1) % n], b1 = h[(i + 2) % n];
			const Vector2 da = a1 - a0, db = b1 - b0;
			const float den = cross2(da, db);
			if (std::fabs(den) < 1e-9f) {
				continue;
			}
			const float t = cross2(b0 - a0, db) / den;
			if (t < 1.0f) {
				continue; // (they meet behind: the corner would cut into the hull)
			}
			const Vector2 x = a0 + da * t;
			const float area = std::fabs(cross2(b0 - a1, x - a1)) * 0.5f;
			if (area < best_area) {
				best_area = area;
				best = i;
				best_pt = x;
			}
		}
		if (best < 0) {
			break;
		}
		const int j = (best + 1) % n;
		h[best] = best_pt;
		h.erase(h.begin() + j);
	}
	return h;
}

void collect_meshes(Node *p_node, std::vector<HewnMesh3D *> &r_out) {
	if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(p_node)) {
		if (!m->is_brush() && !m->is_boolean_cutter() && m->is_visible_in_tree()) {
			r_out.push_back(m);
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		collect_meshes(p_node->get_child(i), r_out);
	}
}

} // namespace

// ------------------------------------------------------------------ lifecycle

HewnOverlay3D::HewnOverlay3D() {}

HewnOverlay3D::~HewnOverlay3D() {
	leave(this, batch);
}

void HewnOverlay3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			set_notify_transform(true);
			if (Engine::get_singleton()->is_editor_hint() && auto_update) {
				_on_target_changed(); // (finds and watches the meshes under it)
			}
			_batch_changed();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			leave(this, batch);
			_unwatch();
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			if (Engine::get_singleton()->is_editor_hint() && auto_update) {
				_on_target_changed();
			}
			_batch_changed();
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			_batch_changed();
		} break;
	}
}

void HewnOverlay3D::_batch_changed() {
	Ref<Material> mat = get_draw_material();
	const bool show = is_inside_tree() && is_visible_in_tree() && mat.is_valid() && !cache_indices.is_empty() && get_world_3d().is_valid();
	Batch *want = nullptr;
	if (show) {
		const RID scen = get_world_3d()->get_scenario();
		for (Batch *b : g_batches) {
			if (b->scenario == scen && b->material == mat) {
				want = b;
				break;
			}
		}
		if (!want) {
			want = new Batch();
			want->scenario = scen;
			want->material = mat;
			g_batches.push_back(want);
		}
	}
	if (want != (Batch *)batch) {
		leave(this, batch);
		if (want) {
			want->members.push_back(this);
			batch = want;
		}
	}
	if (want) {
		want->dirty = true;
		queue_flush();
	}
}

int HewnOverlay3D::get_batch_count() {
	int n = 0;
	for (const Batch *b : g_batches) {
		n += b->members.empty() ? 0 : 1;
	}
	return n;
}

void HewnOverlay3D::flush_batches() {
	g_flush_queued = false;
	RenderingServer *rs = RenderingServer::get_singleton();
	for (size_t i = 0; i < g_batches.size();) {
		Batch *b = g_batches[i];
		if (!b->dirty) {
			i++;
			continue;
		}
		b->dirty = false;
		if (b->members.empty()) {
			if (b->instance.is_valid()) {
				rs->free_rid(b->instance);
			}
			if (b->mesh.is_valid()) {
				rs->free_rid(b->mesh);
			}
			delete b;
			g_batches.erase(g_batches.begin() + i);
			continue;
		}
		PackedVector3Array v, n;
		PackedVector2Array uv;
		PackedInt32Array idx;
		for (HewnOverlay3D *o : b->members) {
			const Array a = o->get_world_arrays();
			const PackedVector3Array ov = a[0], on = a[1];
			const PackedVector2Array ouv = a[2];
			const PackedInt32Array oi = a[3];
			const int base = (int)v.size();
			v.append_array(ov);
			n.append_array(on);
			uv.append_array(ouv);
			for (int64_t k = 0; k < oi.size(); k++) {
				idx.push_back(base + oi[k]);
			}
		}
		if (!b->mesh.is_valid()) {
			b->mesh = rs->mesh_create();
			b->instance = rs->instance_create2(b->mesh, b->scenario);
			rs->instance_geometry_set_cast_shadows_setting(b->instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
		}
		rs->mesh_clear(b->mesh);
		if (!idx.is_empty()) {
			Array arr;
			arr.resize(RenderingServer::ARRAY_MAX);
			arr[RenderingServer::ARRAY_VERTEX] = v;
			arr[RenderingServer::ARRAY_NORMAL] = n;
			arr[RenderingServer::ARRAY_TEX_UV] = uv;
			arr[RenderingServer::ARRAY_INDEX] = idx;
			rs->mesh_add_surface_from_arrays(b->mesh, RenderingServer::PRIMITIVE_TRIANGLES, arr);
			rs->mesh_surface_set_material(b->mesh, 0, b->material->get_rid());
		}
		i++;
	}
}

void HewnOverlay3D::free_static_resources() {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (Batch *b : g_batches) {
		if (rs && b->instance.is_valid()) {
			rs->free_rid(b->instance);
		}
		if (rs && b->mesh.is_valid()) {
			rs->free_rid(b->mesh);
		}
		delete b;
	}
	g_batches.clear();
	g_materials.clear();
}

Ref<Material> HewnOverlay3D::get_draw_material() const {
	if (material.is_valid()) {
		return material;
	}
	if (texture.is_null()) {
		return Ref<Material>();
	}
	const String key = vformat("%d|%d|%d|%d|%s", (int64_t)texture->get_rid().get_id(), blend, render_order, filter_nearest ? 1 : 0, modulate.to_html());
	auto it = g_materials.find(key);
	if (it != g_materials.end()) {
		return it->second;
	}
	Ref<StandardMaterial3D> m;
	m.instantiate();
	m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, texture);
	m->set_albedo(modulate);
	if (blend == BLEND_ALPHA) {
		m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	} else {
		m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
		m->set_alpha_scissor_threshold(0.5f);
	}
	m->set_render_priority(CLAMP(render_order, -128, 127));
	m->set_texture_filter(filter_nearest ? BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS : BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC);
	m->set_roughness(0.9f);
	g_materials[key] = m;
	return m;
}

// ------------------------------------------------------------------ outline and projection

PackedVector2Array HewnOverlay3D::get_outline_points() const {
	if (!outline_dirty) {
		return outline_points;
	}
	outline_dirty = false;
	const float hw = size.x * 0.5f, hh = size.y * 0.5f;
	std::vector<Vector2> fr = { Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 1) }; // (fractions of the picture)
	Ref<Image> img = outline == OUTLINE_ALPHA && texture.is_valid() ? texture->get_image() : Ref<Image>();
	if (img.is_valid() && !img->is_empty()) {
		if (img->is_compressed()) {
			img = img->duplicate();
			img->decompress();
		}
		const int W = img->get_width(), H = img->get_height();
		const int x0 = CLAMP((int)std::floor(uv_rect.position.x * W), 0, W - 1), y0 = CLAMP((int)std::floor(uv_rect.position.y * H), 0, H - 1);
		const int x1 = CLAMP((int)std::ceil(uv_rect.get_end().x * W), x0 + 1, W), y1 = CLAMP((int)std::ceil(uv_rect.get_end().y * H), y0 + 1, H);
		const int rw = x1 - x0, rh = y1 - y0;
		const int step = MAX(1, MAX(rw, rh) / 256);
		std::vector<Vector2> pts;
		for (int y = y0; y < y1; y += step) {
			int mn = -1, mx = -1;
			for (int x = x0; x < x1; x += step) {
				if (img->get_pixel(x, y).a > alpha_threshold) {
					mn = mn < 0 ? x : mn;
					mx = x;
				}
			}
			if (mn >= 0) {
				// the pixels' corners, a step out (the samples skipped between)
				for (int yy : { y - step + 1, y + step }) {
					pts.push_back(Vector2((float)(mn - step + 1 - x0) / rw, (float)(yy - y0) / rh));
					pts.push_back(Vector2((float)(mx + step - x0) / rw, (float)(yy - y0) / rh));
				}
			}
		}
		for (Vector2 &q : pts) {
			q = q.clamp(Vector2(0, 0), Vector2(1, 1));
		}
		std::vector<Vector2> hull = convex_hull(pts);
		if (hull.size() >= 3) {
			hull = reduce_hull(hull, max_vertices);
			// (the corners grown to meet may stick out of the picture: that only costs a little overdraw)
			fr = hull;
		}
	}
	outline_points.clear();
	for (const Vector2 &q : fr) {
		outline_points.push_back(Vector2((q.x - 0.5f) * 2.0f * hw, (q.y - 0.5f) * 2.0f * hh));
	}
	return outline_points;
}

int HewnOverlay3D::reproject(Node *p_root) {
	if (!is_inside_tree()) {
		return get_triangle_count();
	}
	Node *root = p_root ? p_root : get_owner() ? get_owner() : (Node *)get_tree()->get_root();
	std::vector<HewnMesh3D *> meshes;
	collect_meshes(root, meshes);
	const Transform3D g = get_global_transform();
	const Transform3D inv = g.affine_inverse();
	const float hw = size.x * 0.5f, hh = size.y * 0.5f, hd = depth * 0.5f;
	// the box it reaches, in world space
	AABB box;
	for (int i = 0; i < 8; i++) {
		const Vector3 c((i & 1) ? hw : -hw, (i & 2) ? hd : -hd, (i & 4) ? hh : -hh);
		if (i == 0) {
			box = AABB(g.xform(c), Vector3());
		} else {
			box.expand_to(g.xform(c));
		}
	}
	const PackedVector2Array outl = get_outline_points();
	std::vector<Vector2> ol;
	float area = 0.0f;
	for (int64_t i = 0; i < outl.size(); i++) {
		ol.push_back(outl[i]);
	}
	for (size_t i = 0; i < ol.size(); i++) {
		area += cross2(ol[i], ol[(i + 1) % ol.size()]);
	}
	const float orient = area >= 0.0f ? 1.0f : -1.0f;
	const float cos_max = std::cos(Math::deg_to_rad(CLAMP(max_angle, 0.0f, 89.9f)));
	cache_verts.clear();
	cache_normals.clear();
	cache_uvs.clear();
	cache_indices.clear();
	std::vector<HewnMesh3D *> used;
	for (HewnMesh3D *pm : meshes) {
		const Transform3D mg = pm->get_global_transform();
		const AABB mb = mg.xform(pm->get_aabb());
		if (!mb.grow(1e-3f).intersects(box)) {
			continue;
		}
		used.push_back(pm);
		const hewn::Mesh &m = pm->get_shown_data();
		const hewn::BuildOut &bo = pm->get_shown_build();
		const Transform3D to_local = inv * mg;
		const Basis n_to_local = to_local.basis.inverse().transposed();
		for (int f = 0; f < (int)m.faces.size() && f < (int)bo.face_tris.size(); f++) {
			const hewn::Face &F = m.faces[f];
			if (!HewnMesh3D::tool_of(pm->get_material_for_slot(F.material)).is_empty()) {
				continue; // (not drawn in the game)
			}
			Vector3 nl = n_to_local.xform(hewn::face_normal(m, F)).normalized();
			if (nl.y < cos_max) {
				continue; // turned away from the overlay
			}
			const std::vector<int> &tris = bo.face_tris[f];
			for (size_t t = 0; t + 2 < tris.size(); t += 3) {
				std::vector<Vector3> poly = { to_local.xform(m.pos[F.v[tris[t]]]), to_local.xform(m.pos[F.v[tris[t + 1]]]), to_local.xform(m.pos[F.v[tris[t + 2]]]) };
				poly = clip(poly, [&](const Vector3 &p) { return hd - p.y; });
				poly = clip(poly, [&](const Vector3 &p) { return p.y + hd; });
				for (size_t e = 0; e < ol.size() && poly.size() >= 3; e++) {
					const Vector2 a = ol[e], b = ol[(e + 1) % ol.size()];
					poly = clip(poly, [&](const Vector3 &p) { return orient * cross2(b - a, Vector2(p.x, p.z) - a); });
				}
				if (poly.size() < 3) {
					continue;
				}
				const int base = (int)cache_verts.size();
				for (const Vector3 &p : poly) {
					cache_verts.push_back(p + nl * offset);
					cache_normals.push_back(nl);
					cache_uvs.push_back(Vector2(uv_rect.position.x + (p.x / (2.0f * hw) + 0.5f) * uv_rect.size.x,
							uv_rect.position.y + (p.z / (2.0f * hh) + 0.5f) * uv_rect.size.y));
				}
				for (int k = 1; k + 1 < (int)poly.size(); k++) {
					// (counter-clockwise inside, Godot's front faces are clockwise)
					cache_indices.push_back(base);
					cache_indices.push_back(base + k + 1);
					cache_indices.push_back(base + k);
				}
			}
		}
	}
	if (Engine::get_singleton()->is_editor_hint() && auto_update) {
		_watch(used);
	}
	_batch_changed();
	update_gizmos();
	return get_triangle_count();
}

Array HewnOverlay3D::get_world_arrays() const {
	const Transform3D g = is_inside_tree() ? get_global_transform() : get_transform();
	const Basis nb = g.basis.inverse().transposed();
	PackedVector3Array v, n;
	v.resize(cache_verts.size());
	n.resize(cache_normals.size());
	for (int64_t i = 0; i < cache_verts.size(); i++) {
		v.set(i, g.xform(cache_verts[i]));
	}
	for (int64_t i = 0; i < cache_normals.size(); i++) {
		n.set(i, nb.xform(cache_normals[i]).normalized());
	}
	Array a;
	a.push_back(v);
	a.push_back(n);
	a.push_back(cache_uvs);
	a.push_back(cache_indices);
	return a;
}

// ------------------------------------------------------------------ watching the meshes below (editor)

void HewnOverlay3D::_watch(const std::vector<HewnMesh3D *> &p_meshes) {
	_unwatch();
	const Callable c = callable_mp(this, &HewnOverlay3D::_on_target_changed);
	for (HewnMesh3D *m : p_meshes) {
		if (!m->is_connected("mesh_changed", c)) {
			m->connect("mesh_changed", c);
		}
		if (!m->is_connected("geometry_moved", c)) {
			m->connect("geometry_moved", c);
		}
		watched.push_back(ObjectID(m->get_instance_id()));
	}
}

void HewnOverlay3D::_unwatch() {
	const Callable c = callable_mp(this, &HewnOverlay3D::_on_target_changed);
	for (const ObjectID &id : watched) {
		Object *o = ObjectDB::get_instance(id);
		if (o) {
			if (o->is_connected("mesh_changed", c)) {
				o->disconnect("mesh_changed", c);
			}
			if (o->is_connected("geometry_moved", c)) {
				o->disconnect("geometry_moved", c);
			}
		}
	}
	watched.clear();
}

void HewnOverlay3D::_on_target_changed() {
	if (!reproject_queued) {
		reproject_queued = true;
		callable_mp(this, &HewnOverlay3D::_deferred_reproject).call_deferred();
	}
}

void HewnOverlay3D::_deferred_reproject() {
	reproject_queued = false;
	if (is_inside_tree()) {
		reproject();
	}
}

// ------------------------------------------------------------------ properties

#define HW_OVERLAY_CHANGED(m_outline) \
	if (m_outline) {                  \
		outline_dirty = true;         \
	}                                 \
	if (is_inside_tree()) {           \
		_on_target_changed();         \
	}                                 \
	update_gizmos();

void HewnOverlay3D::set_texture(const Ref<Texture2D> &p_texture) {
	texture = p_texture;
	HW_OVERLAY_CHANGED(true);
	_batch_changed();
}

void HewnOverlay3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	_batch_changed();
}

void HewnOverlay3D::set_size(const Vector2 &p_size) {
	size = p_size.max(Vector2(0.001f, 0.001f));
	HW_OVERLAY_CHANGED(true);
}

void HewnOverlay3D::set_depth(float p_depth) {
	depth = MAX(0.001f, p_depth);
	HW_OVERLAY_CHANGED(false);
}

void HewnOverlay3D::set_uv_rect(const Rect2 &p_rect) {
	uv_rect = p_rect;
	HW_OVERLAY_CHANGED(true);
}

void HewnOverlay3D::set_outline(int p_mode) {
	outline = CLAMP(p_mode, 0, 1);
	HW_OVERLAY_CHANGED(true);
}

void HewnOverlay3D::set_alpha_threshold(float p_value) {
	alpha_threshold = CLAMP(p_value, 0.0f, 1.0f);
	HW_OVERLAY_CHANGED(true);
}

void HewnOverlay3D::set_max_vertices(int p_count) {
	max_vertices = CLAMP(p_count, 3, 64);
	HW_OVERLAY_CHANGED(true);
}

void HewnOverlay3D::set_blend(int p_mode) {
	blend = CLAMP(p_mode, 0, 1);
	_batch_changed();
}

void HewnOverlay3D::set_render_order(int p_order) {
	render_order = p_order;
	_batch_changed();
}

void HewnOverlay3D::set_filter_nearest(bool p_on) {
	filter_nearest = p_on;
	_batch_changed();
}

void HewnOverlay3D::set_offset(float p_offset) {
	offset = p_offset;
	HW_OVERLAY_CHANGED(false);
}

void HewnOverlay3D::set_max_angle(float p_deg) {
	max_angle = CLAMP(p_deg, 0.0f, 89.9f);
	HW_OVERLAY_CHANGED(false);
}

void HewnOverlay3D::set_modulate(const Color &p_color) {
	modulate = p_color;
	_batch_changed();
}

void HewnOverlay3D::set_cache(const Dictionary &p_cache) {
	cache_verts = p_cache.get("verts", PackedVector3Array());
	cache_normals = p_cache.get("normals", PackedVector3Array());
	cache_uvs = p_cache.get("uvs", PackedVector2Array());
	cache_indices = p_cache.get("indices", PackedInt32Array());
	if (cache_normals.size() != cache_verts.size() || cache_uvs.size() != cache_verts.size()) {
		cache_verts.clear();
		cache_normals.clear();
		cache_uvs.clear();
		cache_indices.clear();
	}
	_batch_changed();
}

Dictionary HewnOverlay3D::get_cache() const {
	Dictionary d;
	d["verts"] = cache_verts;
	d["normals"] = cache_normals;
	d["uvs"] = cache_uvs;
	d["indices"] = cache_indices;
	return d;
}

void HewnOverlay3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_texture", "texture"), &HewnOverlay3D::set_texture);
	ClassDB::bind_method(D_METHOD("get_texture"), &HewnOverlay3D::get_texture);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &HewnOverlay3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &HewnOverlay3D::get_material);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &HewnOverlay3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &HewnOverlay3D::get_size);
	ClassDB::bind_method(D_METHOD("set_depth", "depth"), &HewnOverlay3D::set_depth);
	ClassDB::bind_method(D_METHOD("get_depth"), &HewnOverlay3D::get_depth);
	ClassDB::bind_method(D_METHOD("set_uv_rect", "rect"), &HewnOverlay3D::set_uv_rect);
	ClassDB::bind_method(D_METHOD("get_uv_rect"), &HewnOverlay3D::get_uv_rect);
	ClassDB::bind_method(D_METHOD("set_outline", "mode"), &HewnOverlay3D::set_outline);
	ClassDB::bind_method(D_METHOD("get_outline"), &HewnOverlay3D::get_outline);
	ClassDB::bind_method(D_METHOD("set_alpha_threshold", "value"), &HewnOverlay3D::set_alpha_threshold);
	ClassDB::bind_method(D_METHOD("get_alpha_threshold"), &HewnOverlay3D::get_alpha_threshold);
	ClassDB::bind_method(D_METHOD("set_max_vertices", "count"), &HewnOverlay3D::set_max_vertices);
	ClassDB::bind_method(D_METHOD("get_max_vertices"), &HewnOverlay3D::get_max_vertices);
	ClassDB::bind_method(D_METHOD("set_blend", "mode"), &HewnOverlay3D::set_blend);
	ClassDB::bind_method(D_METHOD("get_blend"), &HewnOverlay3D::get_blend);
	ClassDB::bind_method(D_METHOD("set_render_order", "order"), &HewnOverlay3D::set_render_order);
	ClassDB::bind_method(D_METHOD("get_render_order"), &HewnOverlay3D::get_render_order);
	ClassDB::bind_method(D_METHOD("set_filter_nearest", "on"), &HewnOverlay3D::set_filter_nearest);
	ClassDB::bind_method(D_METHOD("get_filter_nearest"), &HewnOverlay3D::get_filter_nearest);
	ClassDB::bind_method(D_METHOD("set_offset", "offset"), &HewnOverlay3D::set_offset);
	ClassDB::bind_method(D_METHOD("get_offset"), &HewnOverlay3D::get_offset);
	ClassDB::bind_method(D_METHOD("set_max_angle", "degrees"), &HewnOverlay3D::set_max_angle);
	ClassDB::bind_method(D_METHOD("get_max_angle"), &HewnOverlay3D::get_max_angle);
	ClassDB::bind_method(D_METHOD("set_modulate", "color"), &HewnOverlay3D::set_modulate);
	ClassDB::bind_method(D_METHOD("get_modulate"), &HewnOverlay3D::get_modulate);
	ClassDB::bind_method(D_METHOD("set_auto_update", "on"), &HewnOverlay3D::set_auto_update);
	ClassDB::bind_method(D_METHOD("get_auto_update"), &HewnOverlay3D::get_auto_update);
	ClassDB::bind_method(D_METHOD("set_cache", "cache"), &HewnOverlay3D::set_cache);
	ClassDB::bind_method(D_METHOD("get_cache"), &HewnOverlay3D::get_cache);
	ClassDB::bind_method(D_METHOD("get_outline_points"), &HewnOverlay3D::get_outline_points);
	ClassDB::bind_method(D_METHOD("reproject", "root"), &HewnOverlay3D::reproject, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("get_triangle_count"), &HewnOverlay3D::get_triangle_count);
	ClassDB::bind_method(D_METHOD("get_draw_material"), &HewnOverlay3D::get_draw_material);
	ClassDB::bind_method(D_METHOD("get_world_arrays"), &HewnOverlay3D::get_world_arrays);
	ClassDB::bind_static_method("HewnOverlay3D", D_METHOD("get_batch_count"), &HewnOverlay3D::get_batch_count);
	ClassDB::bind_static_method("HewnOverlay3D", D_METHOD("flush_batches"), &HewnOverlay3D::flush_batches);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_texture", "get_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "depth", PROPERTY_HINT_RANGE, "0.001,10,0.001,or_greater,suffix:m"), "set_depth", "get_depth");
	ADD_PROPERTY(PropertyInfo(Variant::RECT2, "uv_rect"), "set_uv_rect", "get_uv_rect");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "outline", PROPERTY_HINT_ENUM, "Rectangle,Alpha Polygon"), "set_outline", "get_outline");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "alpha_threshold", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_alpha_threshold", "get_alpha_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_vertices", PROPERTY_HINT_RANGE, "3,64,1"), "set_max_vertices", "get_max_vertices");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "blend", PROPERTY_HINT_ENUM, "Alpha Scissor,Alpha Blend"), "set_blend", "get_blend");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "render_order", PROPERTY_HINT_RANGE, "-128,127,1"), "set_render_order", "get_render_order");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "filter_nearest"), "set_filter_nearest", "get_filter_nearest");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "modulate"), "set_modulate", "get_modulate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "offset", PROPERTY_HINT_RANGE, "0,0.1,0.0001,or_greater,suffix:m"), "set_offset", "get_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_angle", PROPERTY_HINT_RANGE, "0,89.9,0.1,degrees"), "set_max_angle", "get_max_angle");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_update"), "set_auto_update", "get_auto_update");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "cache", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_cache", "get_cache");

	BIND_ENUM_CONSTANT(OUTLINE_RECTANGLE);
	BIND_ENUM_CONSTANT(OUTLINE_ALPHA);
	BIND_ENUM_CONSTANT(BLEND_ALPHA_SCISSOR);
	BIND_ENUM_CONSTANT(BLEND_ALPHA);
}
