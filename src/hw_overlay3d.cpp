#include "hw_overlay3d.h"

#include "hewn_mesh.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
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
		PackedColorArray col;
		for (HewnOverlay3D *o : b->members) {
			const Array a = o->get_world_arrays();
			const PackedVector3Array ov = a[0], on = a[1];
			const PackedVector2Array ouv = a[2];
			const PackedInt32Array oi = a[3];
			const int base = (int)v.size();
			v.append_array(ov);
			n.append_array(on);
			uv.append_array(ouv);
			const PackedColorArray oc = a[4];
			if (!oc.is_empty() && col.is_empty() && base > 0) {
				col.resize(base);
				col.fill(Color(1, 1, 1, 1));
			}
			if (!oc.is_empty()) {
				col.append_array(oc);
			} else if (!col.is_empty()) {
				const int64_t was = col.size();
				col.resize(was + ov.size());
				for (int64_t k = was; k < col.size(); k++) {
					col.set(k, Color(1, 1, 1, 1));
				}
			}
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
			if (!col.is_empty()) {
				arr[RenderingServer::ARRAY_COLOR] = col;
			}
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

#define HW_OVERLAY_CHANGED(m_outline) \
	if (m_outline) {                  \
		outline_dirty = true;         \
	}                                 \
	if (is_inside_tree()) {           \
		_on_target_changed();         \
	}                                 \
	update_gizmos();

// ------------------------------------------------------------------ outline and projection

bool HewnOverlay3D::_warped() const {
	for (const Vector2 &v : warp) {
		if (v.length_squared() > 1e-12f) {
			return true;
		}
	}
	return false;
}

// Where a place of the picture (0..1 each way) is, in local XZ: the rectangle, its control points' moves spread between them.
Vector2 HewnOverlay3D::_warp_at(float p_s, float p_t) const {
	Vector2 p((p_s - 0.5f) * size.x, (p_t - 0.5f) * size.y);
	const int n = _warp_side();
	if (warp.size() != n * n) {
		return p;
	}
	auto weights = [n](float x, float *w) {
		if (n == 2) {
			w[0] = 1.0f - x;
			w[1] = x;
		} else {
			w[0] = 2.0f * (x - 0.5f) * (x - 1.0f);
			w[1] = -4.0f * x * (x - 1.0f);
			w[2] = 2.0f * x * (x - 0.5f);
		}
	};
	float ws[3], wt[3];
	weights(p_s, ws);
	weights(p_t, wt);
	Vector2 off;
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			off += warp[j * n + i] * ws[i] * wt[j];
		}
	}
	return p + off * size;
}

Vector2 HewnOverlay3D::get_warp_point(int p_index) const {
	const int n = _warp_side();
	if (p_index < 0 || p_index >= n * n) {
		return Vector2();
	}
	return _warp_at((float)(p_index % n) / (n - 1), (float)(p_index / n) / (n - 1));
}

void HewnOverlay3D::set_warp_point(int p_index, const Vector2 &p_local) {
	const int n = _warp_side();
	if (p_index < 0 || p_index >= n * n) {
		return;
	}
	if (warp.size() != n * n) {
		warp.resize(n * n);
		warp.fill(Vector2());
	}
	const Vector2 base(((float)(p_index % n) / (n - 1) - 0.5f) * size.x, ((float)(p_index / n) / (n - 1) - 0.5f) * size.y);
	warp.set(p_index, (p_local - base) / size);
	HW_OVERLAY_CHANGED(true);
}

void HewnOverlay3D::set_control_points(int p_count) {
	const int want = p_count == 9 ? 9 : 4;
	if (want == control_points) {
		return;
	}
	// (the shape kept as far as it can be: the middle points between the corners, or just the corners)
	PackedVector2Array out;
	if (warp.size() == control_points) {
		if (want == 9) {
			const Vector2 a = warp[0], b = warp[1], c = warp[2], d = warp[3];
			out.push_back(a);
			out.push_back((a + b) * 0.5f);
			out.push_back(b);
			out.push_back((a + c) * 0.5f);
			out.push_back((a + b + c + d) * 0.25f);
			out.push_back((b + d) * 0.5f);
			out.push_back(c);
			out.push_back((c + d) * 0.5f);
			out.push_back(d);
		} else {
			out.push_back(warp[0]);
			out.push_back(warp[2]);
			out.push_back(warp[6]);
			out.push_back(warp[8]);
		}
	}
	control_points = want;
	warp = out;
	HW_OVERLAY_CHANGED(true);
	update_gizmos();
}

void HewnOverlay3D::_reset_shape_undoable() {
	EditorUndoRedoManager *ur = EditorInterface::get_singleton()->get_editor_undo_redo();
	if (!ur) {
		reset_warp();
		return;
	}
	ur->create_action("Hewn: Reset Overlay Shape", UndoRedo::MERGE_DISABLE, this);
	ur->add_do_method(this, "set_warp", PackedVector2Array());
	ur->add_undo_method(this, "set_warp", warp);
	ur->commit_action();
}

void HewnOverlay3D::set_warp(const PackedVector2Array &p_warp) {
	warp = p_warp;
	HW_OVERLAY_CHANGED(true);
	update_gizmos();
}

PackedVector2Array HewnOverlay3D::get_outline_points() const {
	if (!outline_dirty) {
		return outline_points;
	}
	outline_dirty = false;
	if (_warped()) {
		// the edge of the bent shape
		const int k = 8;
		outline_points.clear();
		for (int i = 0; i < k; i++) {
			outline_points.push_back(_warp_at((float)i / k, 0.0f));
		}
		for (int i = 0; i < k; i++) {
			outline_points.push_back(_warp_at(1.0f, (float)i / k));
		}
		for (int i = 0; i < k; i++) {
			outline_points.push_back(_warp_at(1.0f - (float)i / k, 1.0f));
		}
		for (int i = 0; i < k; i++) {
			outline_points.push_back(_warp_at(0.0f, 1.0f - (float)i / k));
		}
		return outline_points;
	}
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

// A shape cut out of the faces: a convex polygon with the plain image, or a triangle with its own corners.
struct HewnOverlay3D::Region {
	std::vector<Vector2> poly;
	float orient = 1.0f;
	bool tri = false;
	Vector2 uv[3];
	float a[3] = { 1.0f, 1.0f, 1.0f };
	float den = 1.0f;
	float y_min = 0.0f, y_max = 0.0f;
	Vector2 mn, mx;

	void finish() {
		float area = 0.0f;
		mn = mx = poly[0];
		for (size_t i = 0; i < poly.size(); i++) {
			area += cross2(poly[i], poly[(i + 1) % poly.size()]);
			mn = Vector2(MIN(mn.x, poly[i].x), MIN(mn.y, poly[i].y));
			mx = Vector2(MAX(mx.x, poly[i].x), MAX(mx.y, poly[i].y));
		}
		orient = area >= 0.0f ? 1.0f : -1.0f;
		if (tri) {
			den = cross2(poly[1] - poly[0], poly[2] - poly[0]);
		}
	}
};

int HewnOverlay3D::reproject(Node *p_root) {
	if (!is_inside_tree()) {
		return get_triangle_count();
	}
	const float hw = size.x * 0.5f, hh = size.y * 0.5f, hd = depth * 0.5f;
	std::vector<Region> regs;
	if (_warped()) {
		// a fine grid of triangles: bent as the control points say, the image with them
		const int cells = 8;
		for (int j = 0; j < cells; j++) {
			for (int i = 0; i < cells; i++) {
				const float s0 = (float)i / cells, s1 = (float)(i + 1) / cells, t0 = (float)j / cells, t1 = (float)(j + 1) / cells;
				const float st[6][2] = { { s0, t0 }, { s1, t0 }, { s1, t1 }, { s0, t0 }, { s1, t1 }, { s0, t1 } };
				for (int k = 0; k < 2; k++) {
					Region r;
					r.tri = true;
					r.y_min = -hd;
					r.y_max = hd;
					for (int c = 0; c < 3; c++) {
						const float ss = st[k * 3 + c][0], tt = st[k * 3 + c][1];
						r.poly.push_back(_warp_at(ss, tt));
						r.uv[c] = uv_rect.position + Vector2(ss, tt) * uv_rect.size;
					}
					r.finish();
					if (std::fabs(r.den) > 1e-10f) {
						regs.push_back(r);
					}
				}
			}
		}
	} else {
		Region r;
		const PackedVector2Array outl = get_outline_points();
		for (int64_t i = 0; i < outl.size(); i++) {
			r.poly.push_back(outl[i]);
		}
		r.y_min = -hd;
		r.y_max = hd;
		if (r.poly.size() >= 3) {
			r.finish();
			regs.push_back(r);
		}
	}
	(void)hw;
	(void)hh;
	return _project(regs, p_root);
}

int HewnOverlay3D::project_tris(const std::vector<HewnOverlayTri> &p_tris, Node *p_root) {
	if (!is_inside_tree()) {
		return get_triangle_count();
	}
	std::vector<Region> regs;
	regs.reserve(p_tris.size());
	for (const HewnOverlayTri &t : p_tris) {
		Region r;
		r.tri = true;
		r.y_min = t.y_min;
		r.y_max = t.y_max;
		for (int c = 0; c < 3; c++) {
			r.poly.push_back(t.p[c]);
			r.uv[c] = t.uv[c];
			r.a[c] = t.a[c];
		}
		r.finish();
		if (std::fabs(r.den) > 1e-10f) {
			regs.push_back(r);
		}
	}
	return _project(regs, p_root);
}

int HewnOverlay3D::_project(const std::vector<Region> &p_regions, Node *p_root) {
	Node *root = p_root ? p_root : get_owner() ? get_owner() : (Node *)get_tree()->get_root();
	std::vector<HewnMesh3D *> meshes;
	collect_meshes(root, meshes);
	const Transform3D g = get_global_transform();
	const Transform3D inv = g.affine_inverse();
	cache_verts.clear();
	cache_normals.clear();
	cache_uvs.clear();
	cache_indices.clear();
	cache_alpha.clear();
	std::vector<HewnMesh3D *> used;
	const float cos_max = std::cos(Math::deg_to_rad(CLAMP(max_angle, 0.0f, 89.9f)));
	bool any_alpha = false;
	for (const Region &r : p_regions) {
		any_alpha = any_alpha || r.a[0] < 0.999f || r.a[1] < 0.999f || r.a[2] < 0.999f;
	}
	// the regions sorted into a grid over their bounds (a long road: thousands of them over many faces)
	Vector2 all_mn(1e30f, 1e30f), all_mx(-1e30f, -1e30f);
	float y0 = 1e30f, y1 = -1e30f;
	for (const Region &r : p_regions) {
		all_mn = Vector2(MIN(all_mn.x, r.mn.x), MIN(all_mn.y, r.mn.y));
		all_mx = Vector2(MAX(all_mx.x, r.mx.x), MAX(all_mx.y, r.mx.y));
		y0 = MIN(y0, r.y_min);
		y1 = MAX(y1, r.y_max);
	}
	const int side = CLAMP((int)std::sqrt((double)p_regions.size()) + 1, 1, 256);
	const Vector2 cell = ((all_mx - all_mn) / (float)side).max(Vector2(1e-4f, 1e-4f));
	auto col = [&](float p_v, float p_o, float p_s) { return CLAMP((int)std::floor((p_v - p_o) / p_s), 0, side - 1); };
	std::vector<std::vector<int>> grid(side * side);
	for (int i = 0; i < (int)p_regions.size(); i++) {
		const Region &r = p_regions[i];
		for (int x = col(r.mn.x, all_mn.x, cell.x); x <= col(r.mx.x, all_mn.x, cell.x); x++) {
			for (int z = col(r.mn.y, all_mn.y, cell.y); z <= col(r.mx.y, all_mn.y, cell.y); z++) {
				grid[z * side + x].push_back(i);
			}
		}
	}
	std::vector<int> stamp(p_regions.size(), -1);
	int stamp_now = 0;
	// the box it reaches, in world space
	AABB box;
	for (int i = 0; i < 8 && !p_regions.empty(); i++) {
		const Vector3 c = g.xform(Vector3((i & 1) ? all_mx.x : all_mn.x, (i & 2) ? y1 : y0, (i & 4) ? all_mx.y : all_mn.y));
		if (i == 0) {
			box = AABB(c, Vector3());
		} else {
			box.expand_to(c);
		}
	}
	const float hw = size.x * 0.5f, hh = size.y * 0.5f;
	for (HewnMesh3D *pm : meshes) {
		if (p_regions.empty()) {
			break;
		}
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
		// the smooth normals at the vertices: the overlay shaded as the surface it lies on
		std::vector<Vector3> vn(m.pos.size());
		for (const hewn::Face &F : m.faces) {
			if (F.smooth != 0) {
				const Vector3 fn = hewn::face_normal(m, F);
				for (int v : F.v) {
					vn[v] += fn;
				}
			}
		}
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
				const int vi[3] = { F.v[tris[t]], F.v[tris[t + 1]], F.v[tris[t + 2]] };
				const Vector3 q[3] = { to_local.xform(m.pos[vi[0]]), to_local.xform(m.pos[vi[1]]), to_local.xform(m.pos[vi[2]]) };
				const Vector2 tmn(MIN(q[0].x, MIN(q[1].x, q[2].x)), MIN(q[0].z, MIN(q[1].z, q[2].z)));
				const Vector2 tmx(MAX(q[0].x, MAX(q[1].x, q[2].x)), MAX(q[0].z, MAX(q[1].z, q[2].z)));
				if (tmx.x < all_mn.x || tmx.y < all_mn.y || tmn.x > all_mx.x || tmn.y > all_mx.y) {
					continue;
				}
				const float tden = cross2(Vector2(q[1].x - q[0].x, q[1].z - q[0].z), Vector2(q[2].x - q[0].x, q[2].z - q[0].z));
				stamp_now++;
				for (int x = col(tmn.x, all_mn.x, cell.x); x <= col(tmx.x, all_mn.x, cell.x); x++) {
					for (int z = col(tmn.y, all_mn.y, cell.y); z <= col(tmx.y, all_mn.y, cell.y); z++) {
						for (int ri : grid[z * side + x]) {
							if (stamp[ri] == stamp_now) {
								continue;
							}
							stamp[ri] = stamp_now;
							const Region &r = p_regions[ri];
							if (r.mx.x < tmn.x || r.mx.y < tmn.y || r.mn.x > tmx.x || r.mn.y > tmx.y) {
								continue;
							}
							std::vector<Vector3> poly = { q[0], q[1], q[2] };
							poly = clip(poly, [&](const Vector3 &p) { return r.y_max - p.y; });
							poly = clip(poly, [&](const Vector3 &p) { return p.y - r.y_min; });
							for (size_t e = 0; e < r.poly.size() && poly.size() >= 3; e++) {
								const Vector2 a = r.poly[e], b = r.poly[(e + 1) % r.poly.size()];
								poly = clip(poly, [&](const Vector3 &p) { return r.orient * cross2(b - a, Vector2(p.x, p.z) - a); });
							}
							if (poly.size() < 3) {
								continue;
							}
							const int base = (int)cache_verts.size();
							for (const Vector3 &p : poly) {
								cache_verts.push_back(p + nl * offset);
								// (its normal: between the vertices' on a smooth face)
								Vector3 pn = nl;
								if (F.smooth != 0 && std::fabs(tden) > 1e-12f) {
									const Vector2 d(p.x - q[0].x, p.z - q[0].z);
									const float w1 = cross2(d, Vector2(q[2].x - q[0].x, q[2].z - q[0].z)) / tden, w2 = cross2(Vector2(q[1].x - q[0].x, q[1].z - q[0].z), d) / tden;
									const Vector3 sn = n_to_local.xform(vn[vi[0]] * (1.0f - w1 - w2) + vn[vi[1]] * w1 + vn[vi[2]] * w2);
									if (sn.length_squared() > 1e-12f) {
										pn = sn.normalized();
									}
								}
								cache_normals.push_back(pn);
								const Vector2 at(p.x, p.z);
								if (r.tri) {
									const Vector2 e0 = at - r.poly[0];
									const float w1 = cross2(e0, r.poly[2] - r.poly[0]) / r.den, w2 = cross2(r.poly[1] - r.poly[0], e0) / r.den, w0 = 1.0f - w1 - w2;
									cache_uvs.push_back(r.uv[0] * w0 + r.uv[1] * w1 + r.uv[2] * w2);
									if (any_alpha) {
										cache_alpha.push_back(CLAMP(r.a[0] * w0 + r.a[1] * w1 + r.a[2] * w2, 0.0f, 1.0f));
									}
								} else {
									cache_uvs.push_back(Vector2(uv_rect.position.x + (p.x / (2.0f * hw) + 0.5f) * uv_rect.size.x,
											uv_rect.position.y + (p.z / (2.0f * hh) + 0.5f) * uv_rect.size.y));
									if (any_alpha) {
										cache_alpha.push_back(1.0f);
									}
								}
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
	PackedColorArray cols;
	if (cache_alpha.size() == cache_verts.size()) {
		cols.resize(cache_alpha.size());
		for (int64_t i = 0; i < cache_alpha.size(); i++) {
			cols.set(i, Color(1, 1, 1, cache_alpha[i]));
		}
	}
	a.push_back(cols);
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
	cache_alpha = p_cache.get("alpha", PackedFloat32Array());
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
	if (!cache_alpha.is_empty()) {
		d["alpha"] = cache_alpha;
	}
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
	ClassDB::bind_method(D_METHOD("set_control_points", "count"), &HewnOverlay3D::set_control_points);
	ClassDB::bind_method(D_METHOD("get_control_points"), &HewnOverlay3D::get_control_points);
	ClassDB::bind_method(D_METHOD("set_warp", "warp"), &HewnOverlay3D::set_warp);
	ClassDB::bind_method(D_METHOD("get_warp"), &HewnOverlay3D::get_warp);
	ClassDB::bind_method(D_METHOD("reset_warp"), &HewnOverlay3D::reset_warp);
	ClassDB::bind_method(D_METHOD("get_reset_shape"), &HewnOverlay3D::get_reset_shape);
	ClassDB::bind_method(D_METHOD("get_warp_point", "index"), &HewnOverlay3D::get_warp_point);
	ClassDB::bind_method(D_METHOD("set_warp_point", "index", "local"), &HewnOverlay3D::set_warp_point);
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
	ADD_GROUP("Shape", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "control_points", PROPERTY_HINT_ENUM, "4 Points:4,9 Points:9"), "set_control_points", "get_control_points");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR2_ARRAY, "warp", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_warp", "get_warp");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "reset_shape", PROPERTY_HINT_TOOL_BUTTON, "Reset Shape,Reload", PROPERTY_USAGE_EDITOR), "", "get_reset_shape");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "cache", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_cache", "get_cache");

	BIND_ENUM_CONSTANT(OUTLINE_RECTANGLE);
	BIND_ENUM_CONSTANT(OUTLINE_ALPHA);
	BIND_ENUM_CONSTANT(BLEND_ALPHA_SCISSOR);
	BIND_ENUM_CONSTANT(BLEND_ALPHA);
}
