// UV editor (bottom dock): the UVs of the faces selected in the viewport (all faces when
// none is), selected by vertex / edge / face (1 / 2 / 3; coincident corners go along), moved, turned and
// scaled with G / R / S or by dragging (Godot's magnet snaps: increment = the grid, vertex = other UV
// vertices), islands packed into the 0..1 square. Header menus: View, Select, Image, UV.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/v_separator.hpp>

#include <algorithm>
#include <functional>
#include <numeric>

using namespace godot;
using namespace hw_editor;

namespace {

const float EPS_UV = 1e-5f;

uint64_t loop_key(int f, int i) {
	return ((uint64_t)(uint32_t)f << 32) | (uint32_t)i;
}
int loop_face(uint64_t k) {
	return (int)(k >> 32);
}
int loop_corner(uint64_t k) {
	return (int)(k & 0xffffffffu);
}

bool point_in_poly(const Vector2 &p, const std::vector<Vector2> &poly) {
	bool in = false;
	const size_t n = poly.size();
	for (size_t i = 0, j = n - 1; i < n; j = i++) {
		const Vector2 &a = poly[i], &b = poly[j];
		if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
			in = !in;
		}
	}
	return in;
}

float seg_distance(const Vector2 &p, const Vector2 &a, const Vector2 &b) {
	const Vector2 d = b - a;
	const float l2 = d.length_squared();
	const float t = l2 < 1e-12f ? 0.0f : CLAMP((p - a).dot(d) / l2, 0.0f, 1.0f);
	return p.distance_to(a + d * t);
}

Vector2 rot(const Vector2 &p, float a) {
	return Vector2(p.x * std::cos(a) - p.y * std::sin(a), p.x * std::sin(a) + p.y * std::cos(a));
}

// Andrew's monotone chain.
std::vector<Vector2> convex_hull(std::vector<Vector2> p) {
	std::sort(p.begin(), p.end(), [](const Vector2 &a, const Vector2 &b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
	if (p.size() < 3) {
		return p;
	}
	std::vector<Vector2> h(2 * p.size());
	size_t k = 0;
	for (size_t i = 0; i < p.size(); i++) {
		while (k >= 2 && (h[k - 1] - h[k - 2]).cross(p[i] - h[k - 2]) <= 0.0f) {
			k--;
		}
		h[k++] = p[i];
	}
	for (size_t i = p.size() - 1, t = k + 1; i > 0; i--) {
		while (k >= t && (h[k - 1] - h[k - 2]).cross(p[i - 1] - h[k - 2]) <= 0.0f) {
			k--;
		}
		h[k++] = p[i - 1];
	}
	h.resize(k - 1);
	return h;
}

// Shelf packing of rectangles (tallest first) into a strip of width 1: false if it is taller than 1.
bool shelf_pack(const std::vector<Vector2> &p_sizes, float p_scale, float p_margin, std::vector<Vector2> &r_pos) {
	std::vector<int> order(p_sizes.size());
	std::iota(order.begin(), order.end(), 0);
	std::sort(order.begin(), order.end(), [&](int a, int b) { return p_sizes[a].y > p_sizes[b].y; });
	r_pos.assign(p_sizes.size(), Vector2());
	float x = p_margin, y = p_margin, row = 0.0f;
	for (int i : order) {
		const Vector2 s = p_sizes[i] * p_scale;
		if (s.x + 2.0f * p_margin > 1.0f) {
			return false;
		}
		if (x + s.x + p_margin > 1.0f) {
			x = p_margin;
			y += row + p_margin;
			row = 0.0f;
		}
		r_pos[i] = Vector2(x, y);
		x += s.x + p_margin;
		row = MAX(row, s.y);
	}
	return y + row + p_margin <= 1.0f;
}

} // namespace

void HewnUVCanvas::_bind_methods() {
	ClassDB::bind_method(D_METHOD("feed_input", "event"), &HewnUVCanvas::feed_input);
}

HewnMesh3D *HewnUVCanvas::_mesh() {
	HewnMesh3D *n = plugin ? plugin->get_edited() : nullptr;
	const uint64_t key = n ? (uint64_t)n->get_instance_id() * 1315423911ull + n->get_data().faces.size() : 0;
	if (key != mesh_key) {
		mesh_key = key;
		sel.clear(); // (other faces now)
	}
	if (n && sync_selection) {
		// the 3D face selection is the UV one
		sel.clear();
		const hewn::Mesh &m = n->get_data();
		for (int f : n->selection.faces) {
			if (f >= 0 && f < (int)m.faces.size()) {
				for (int i = 0; i < m.faces[f].size(); i++) {
					sel.insert(loop_key(f, i));
				}
			}
		}
	}
	return n;
}

std::vector<int> HewnUVCanvas::_shown_faces(HewnMesh3D *p_n) const {
	const hewn::Mesh &m = p_n->get_data();
	const hewn::Selection &s = p_n->selection;
	std::vector<int> out;
	if (!sync_selection) {
		if (!s.faces.empty()) {
			out = s.faces;
		} else if (!s.verts.empty() || !s.edges.empty()) {
			out = hewn::sel::faces_from_verts(m, s.verts.empty() ? hewn::sel::verts_of_edges(s.edges) : s.verts, true);
		}
	}
	if (out.empty()) {
		for (int f = 0; f < (int)m.faces.size(); f++) {
			out.push_back(f);
		}
	}
	std::vector<int> shown;
	for (int f : out) {
		if (f >= 0 && f < (int)m.faces.size() && !m.faces[f].hidden && m.faces[f].size() >= 2) {
			shown.push_back(f);
		}
	}
	return shown;
}

Vector2 HewnUVCanvas::_loop_uv(HewnMesh3D *p_n, int p_face, int p_corner) const {
	const hewn::Face &F = p_n->get_data().faces[p_face];
	if (F.manual_uv && p_corner < (int)F.uv.size()) {
		return F.uv[p_corner];
	}
	const std::vector<std::vector<Vector2>> &uvs = p_n->get_build().face_uvs;
	if (p_face < (int)uvs.size() && p_corner < (int)uvs[p_face].size()) {
		return uvs[p_face][p_corner];
	}
	return Vector2();
}

void HewnUVCanvas::_set_loop_uv(HewnMesh3D *p_n, int p_face, int p_corner, const Vector2 &p_uv) {
	hewn::Face &F = p_n->get_data().faces[p_face];
	if (!F.manual_uv) {
		// the projection becomes hand made UVs
		std::vector<Vector2> cur;
		for (int i = 0; i < F.size(); i++) {
			cur.push_back(_loop_uv(p_n, p_face, i));
		}
		F.uv = cur;
		F.manual_uv = true;
	}
	if (p_corner < (int)F.uv.size()) {
		F.uv[p_corner] = p_uv;
	}
}

// Corners at the same place as the given ones (same vertex, same UV): they move together
// (sticky selection, shared location).
void HewnUVCanvas::_sticky(HewnMesh3D *p_n, const std::vector<int> &p_faces, std::set<uint64_t> &r_loops) const {
	const hewn::Mesh &m = p_n->get_data();
	std::unordered_map<int, std::vector<uint64_t>> by_vert;
	for (int f : p_faces) {
		for (int i = 0; i < m.faces[f].size(); i++) {
			by_vert[m.faces[f].v[i]].push_back(loop_key(f, i));
		}
	}
	std::set<uint64_t> add;
	for (uint64_t k : r_loops) {
		const int f = loop_face(k), i = loop_corner(k);
		if (f >= (int)m.faces.size() || i >= m.faces[f].size()) {
			continue;
		}
		const Vector2 uv = _loop_uv(p_n, f, i);
		for (uint64_t o : by_vert[m.faces[f].v[i]]) {
			if (_loop_uv(p_n, loop_face(o), loop_corner(o)).distance_to(uv) < EPS_UV) {
				add.insert(o);
			}
		}
	}
	r_loops.insert(add.begin(), add.end());
}

// The element under the mouse, as corners (empty: nothing there).
bool HewnUVCanvas::_pick(HewnMesh3D *p_n, const Vector2 &p_pos, std::set<uint64_t> &r_loops) const {
	const hewn::Mesh &m = p_n->get_data();
	const std::vector<int> faces = _shown_faces(p_n);
	r_loops.clear();
	if (uv_mode == UV_VERTEX) {
		float best = 10.0f;
		uint64_t found = ~0ull;
		for (int f : faces) {
			for (int i = 0; i < m.faces[f].size(); i++) {
				const float d = to_screen(_loop_uv(p_n, f, i)).distance_to(p_pos);
				if (d < best) {
					best = d;
					found = loop_key(f, i);
				}
			}
		}
		if (found != ~0ull) {
			r_loops.insert(found);
		}
	} else if (uv_mode == UV_EDGE) {
		float best = 8.0f;
		int bf = -1, bi = -1;
		for (int f : faces) {
			const int n = m.faces[f].size();
			for (int i = 0; i < n; i++) {
				const float d = seg_distance(p_pos, to_screen(_loop_uv(p_n, f, i)), to_screen(_loop_uv(p_n, f, (i + 1) % n)));
				if (d < best) {
					best = d;
					bf = f;
					bi = i;
				}
			}
		}
		if (bf >= 0) {
			r_loops.insert(loop_key(bf, bi));
			r_loops.insert(loop_key(bf, (bi + 1) % m.faces[bf].size()));
		}
	} else {
		const Vector2 uv = to_uv(p_pos);
		// selected faces first (drawn on top)
		for (int pass = 0; pass < 2 && r_loops.empty(); pass++) {
			for (int k = (int)faces.size() - 1; k >= 0; k--) {
				const int f = faces[k];
				if ((pass == 0) != _face_selected(p_n, f)) {
					continue;
				}
				std::vector<Vector2> poly;
				for (int i = 0; i < m.faces[f].size(); i++) {
					poly.push_back(_loop_uv(p_n, f, i));
				}
				if (poly.size() >= 3 && point_in_poly(uv, poly)) {
					for (int i = 0; i < m.faces[f].size(); i++) {
						r_loops.insert(loop_key(f, i));
					}
					break;
				}
			}
		}
	}
	if (!r_loops.empty() && !sync_selection) {
		_sticky(p_n, faces, r_loops);
	}
	return !r_loops.empty();
}

bool HewnUVCanvas::_face_selected(HewnMesh3D *p_n, int p_face) const {
	const hewn::Face &F = p_n->get_data().faces[p_face];
	for (int i = 0; i < F.size(); i++) {
		if (!sel.count(loop_key(p_face, i))) {
			return false;
		}
	}
	return F.size() > 0;
}

std::vector<uint64_t> HewnUVCanvas::_selected_loops(HewnMesh3D *p_n) const {
	const hewn::Mesh &m = p_n->get_data();
	std::vector<uint64_t> out;
	for (int f : _shown_faces(p_n)) {
		for (int i = 0; i < m.faces[f].size(); i++) {
			if (sel.count(loop_key(f, i))) {
				out.push_back(loop_key(f, i));
			}
		}
	}
	return out;
}

void HewnUVCanvas::_box_select(HewnMesh3D *p_n, const Rect2 &p_rect, bool p_remove) {
	const hewn::Mesh &m = p_n->get_data();
	const std::vector<int> faces = _shown_faces(p_n);
	std::set<uint64_t> hit;
	for (int f : faces) {
		const int n = m.faces[f].size();
		if (uv_mode == UV_FACE) {
			Vector2 c;
			for (int i = 0; i < n; i++) {
				c += _loop_uv(p_n, f, i) / (float)n;
			}
			if (p_rect.has_point(to_screen(c))) {
				for (int i = 0; i < n; i++) {
					hit.insert(loop_key(f, i));
				}
			}
		} else {
			for (int i = 0; i < n; i++) {
				const bool a = p_rect.has_point(to_screen(_loop_uv(p_n, f, i)));
				if (uv_mode == UV_VERTEX && a) {
					hit.insert(loop_key(f, i));
				} else if (uv_mode == UV_EDGE && a && p_rect.has_point(to_screen(_loop_uv(p_n, f, (i + 1) % n)))) {
					hit.insert(loop_key(f, i));
					hit.insert(loop_key(f, (i + 1) % n));
				}
			}
		}
	}
	if (!sync_selection) {
		_sticky(p_n, faces, hit);
	}
	for (uint64_t k : hit) {
		if (p_remove) {
			sel.erase(k);
		} else {
			sel.insert(k);
		}
	}
}

// Islands: faces joined by edges whose two ends have the same UVs on both sides.
std::vector<std::vector<int>> HewnUVCanvas::_islands(HewnMesh3D *p_n, const std::vector<int> &p_faces) const {
	const hewn::Mesh &m = p_n->get_data();
	std::unordered_map<int, int> index;
	for (int k = 0; k < (int)p_faces.size(); k++) {
		index[p_faces[k]] = k;
	}
	std::vector<int> parent(p_faces.size());
	std::iota(parent.begin(), parent.end(), 0);
	std::function<int(int)> find = [&](int x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
	struct Side {
		int face;
		Vector2 ua, ub; // UV at the lower / higher vertex id
	};
	std::unordered_map<uint64_t, std::vector<Side>> edges;
	for (int f : p_faces) {
		const int n = m.faces[f].size();
		for (int i = 0; i < n; i++) {
			const int a = m.faces[f].v[i], b = m.faces[f].v[(i + 1) % n];
			Vector2 ua = _loop_uv(p_n, f, i), ub = _loop_uv(p_n, f, (i + 1) % n);
			if (a > b) {
				std::swap(ua, ub);
			}
			edges[hewn::ekey(a, b)].push_back({ f, ua, ub });
		}
	}
	for (const auto &it : edges) {
		const std::vector<Side> &sides = it.second;
		for (size_t x = 0; x < sides.size(); x++) {
			for (size_t y = x + 1; y < sides.size(); y++) {
				if (sides[x].ua.distance_to(sides[y].ua) < 1e-4f && sides[x].ub.distance_to(sides[y].ub) < 1e-4f) {
					parent[find(index[sides[x].face])] = find(index[sides[y].face]);
				}
			}
		}
	}
	std::map<int, std::vector<int>> groups;
	for (int k = 0; k < (int)p_faces.size(); k++) {
		groups[find(k)].push_back(p_faces[k]);
	}
	std::vector<std::vector<int>> out;
	for (auto &g : groups) {
		out.push_back(g.second);
	}
	return out;
}

void HewnUVCanvas::_select_linked(HewnMesh3D *p_n, bool p_under_mouse) {
	const hewn::Mesh &m = p_n->get_data();
	std::set<uint64_t> seed;
	if (p_under_mouse) {
		const int keep = uv_mode;
		uv_mode = UV_FACE;
		_pick(p_n, mouse, seed);
		uv_mode = keep;
		if (seed.empty()) {
			_pick(p_n, mouse, seed);
		}
	} else {
		seed = sel;
	}
	for (const std::vector<int> &island : _islands(p_n, _shown_faces(p_n))) {
		bool touched = false;
		for (int f : island) {
			for (int i = 0; i < m.faces[f].size() && !touched; i++) {
				touched = seed.count(loop_key(f, i)) > 0;
			}
		}
		if (touched) {
			for (int f : island) {
				for (int i = 0; i < m.faces[f].size(); i++) {
					sel.insert(loop_key(f, i));
				}
			}
		}
	}
}

float HewnUVCanvas::_grid() const {
	if (grid_div <= 0) {
		HewnMesh3D *n = plugin ? plugin->get_edited() : nullptr;
		Vector2 tiling;
		Ref<Texture2D> t = n ? _texture(n, tiling) : Ref<Texture2D>();
		return t.is_valid() ? 1.0f / MAX(1.0f, t->get_size().x * tiling.x) : 1.0f / 64.0f;
	}
	return 1.0f / (float)grid_div;
}

// The texture of the first shown face's material, and its tiling (UV1 scale).
Ref<Texture2D> HewnUVCanvas::_texture(HewnMesh3D *p_n, Vector2 &r_tiling) const {
	r_tiling = Vector2(1, 1);
	const std::vector<int> faces = _shown_faces(p_n);
	if (faces.empty()) {
		return Ref<Texture2D>();
	}
	Ref<BaseMaterial3D> bm = p_n->get_material_for_slot(p_n->get_data().faces[faces[0]].material);
	if (bm.is_null()) {
		return Ref<Texture2D>();
	}
	r_tiling = Vector2(MAX(1e-4f, bm->get_uv1_scale().x), MAX(1e-4f, bm->get_uv1_scale().y));
	return bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
}

// ------------------------------------------------------------------ G / R / S

void HewnUVCanvas::_start_xf(Xform p_kind, bool p_tweak) {
	HewnMesh3D *n = _mesh();
	if (!n || xf != XF_NONE) {
		return;
	}
	const std::vector<uint64_t> loops = _selected_loops(n);
	if (loops.empty()) {
		return;
	}
	static const char *names[4] = { "", "Move UVs", "Rotate UVs", "Scale UVs" };
	plugin->begin_edit(names[p_kind]);
	xf = p_kind;
	xf_tweak = p_tweak;
	xf_axis = -1;
	xf_mouse0 = p_tweak ? press_pos : mouse;
	xf_start.clear();
	Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
	for (uint64_t k : loops) {
		const Vector2 uv = _loop_uv(n, loop_face(k), loop_corner(k));
		xf_start.push_back({ k, uv });
		mn = mn.min(uv);
		mx = mx.max(uv);
	}
	xf_pivot = (mn + mx) * 0.5f; // (bounding box center)
	_update_xf(false);
}

void HewnUVCanvas::_update_xf(bool p_ctrl) {
	HewnMesh3D *n = _mesh();
	if (!n || xf == XF_NONE) {
		return;
	}
	const bool snap = plugin->snapping_active(p_ctrl);
	const int targets = plugin->dock ? plugin->dock->snap_targets : 0;
	const Vector2 ps = to_screen(xf_pivot);
	Vector2 delta;
	float angle = 0.0f;
	Vector2 factor(1, 1);
	if (xf == XF_MOVE) {
		delta = (mouse - xf_mouse0) / zoom;
		if (xf_axis == 0) {
			delta.y = 0.0f;
		} else if (xf_axis == 1) {
			delta.x = 0.0f;
		}
		bool snapped = false;
		xf_snapped = false;
		if (snap && (targets & (HewnDock::SNAP_VERTEX | HewnDock::SNAP_EDGE))) {
			// the target is the UV vertex (else the point of a UV edge) under the mouse that
			// does not move; the moved corner closest to it lands on it
			std::set<uint64_t> moving;
			for (const auto &it : xf_start) {
				moving.insert(it.first);
			}
			const hewn::Mesh &m = n->get_data();
			const std::vector<int> shown = _shown_faces(n);
			float best = 12.0f;
			Vector2 target;
			int kind = 0;
			if (targets & HewnDock::SNAP_VERTEX) {
				for (int f : shown) {
					for (int i = 0; i < m.faces[f].size(); i++) {
						if (moving.count(loop_key(f, i))) {
							continue;
						}
						const float d = mouse.distance_to(to_screen(_loop_uv(n, f, i)));
						if (d < best) {
							best = d;
							target = _loop_uv(n, f, i);
							kind = SNAP_KIND_VERTEX;
						}
					}
				}
			}
			if (!kind && (targets & HewnDock::SNAP_EDGE)) {
				for (int f : shown) {
					const int c = m.faces[f].size();
					for (int i = 0; i < c; i++) {
						const int j = (i + 1) % c;
						if (moving.count(loop_key(f, i)) || moving.count(loop_key(f, j))) {
							continue; // (it moves along)
						}
						const Vector2 a = _loop_uv(n, f, i), b = _loop_uv(n, f, j);
						float t;
						const float d = seg_dist(mouse, to_screen(a), to_screen(b), &t);
						if (d < best) {
							best = d;
							target = a.lerp(b, t);
							kind = SNAP_KIND_EDGE;
						}
					}
				}
			}
			if (kind) {
				float near = 1e30f;
				Vector2 fix;
				for (const auto &it : xf_start) {
					const float d = (it.second + delta).distance_to(target);
					if (d < near) {
						near = d;
						fix = target - it.second;
					}
				}
				delta = Vector2(xf_axis == 1 ? delta.x : fix.x, xf_axis == 0 ? delta.y : fix.y);
				snapped = true;
				xf_snapped = true;
				xf_snap_uv = target;
				xf_snap_kind = kind;
			}
		}
		if (snap && !snapped && (targets & (HewnDock::SNAP_INCREMENT | HewnDock::SNAP_GRID))) {
			const float g = _grid();
			if (targets & HewnDock::SNAP_GRID) {
				const Vector2 p = ((xf_pivot + delta) / g).round() * g; // the pivot onto the grid
				delta = Vector2(xf_axis == 1 ? delta.x : p.x - xf_pivot.x, xf_axis == 0 ? delta.y : p.y - xf_pivot.y);
			} else {
				delta = (delta / g).round() * g;
			}
		}
		xf_info = vformat(T("Move  U %.4f  V %.4f"), delta.x, delta.y);
	} else if (xf == XF_ROTATE) {
		angle = (mouse - ps).angle() - (xf_mouse0 - ps).angle();
		float deg = Math::rad_to_deg(angle);
		if (snap) {
			const float st = plugin->dock ? MAX(0.01f, (float)plugin->dock->snap_rotate->get_value()) : 15.0f;
			deg = std::round(deg / st) * st;
		}
		angle = Math::deg_to_rad(deg);
		xf_info = vformat(T("Rotate  %.1f deg"), deg);
	} else if (xf == XF_SCALE) {
		const float d0 = MAX(1.0f, (xf_mouse0 - ps).length());
		float s = (mouse - ps).length() / d0;
		if (snap) {
			const float st = plugin->dock ? MAX(0.001f, (float)plugin->dock->snap_scale->get_value()) : 0.1f;
			s = std::round(s / st) * st;
		}
		factor = Vector2(xf_axis == 1 ? 1.0f : s, xf_axis == 0 ? 1.0f : s);
		xf_info = vformat(T("Scale  %.3f, %.3f"), factor.x, factor.y);
	}
	if (xf_axis >= 0) {
		xf_info += xf_axis == 0 ? T("  along U") : T("  along V");
	}
	for (const auto &it : xf_start) {
		Vector2 uv = it.second;
		if (xf == XF_MOVE) {
			uv += delta;
		} else if (xf == XF_ROTATE) {
			uv = xf_pivot + rot(uv - xf_pivot, angle);
		} else {
			uv = xf_pivot + (uv - xf_pivot) * factor;
		}
		_set_loop_uv(n, loop_face(it.first), loop_corner(it.first), uv);
	}
	n->commit(false);
	queue_redraw();
}

void HewnUVCanvas::_end_xf(bool p_apply) {
	HewnMesh3D *n = _mesh();
	if (xf == XF_NONE) {
		return;
	}
	xf = XF_NONE;
	xf_info = String();
	if (!n) {
		plugin->cancel_edit();
		return;
	}
	if (p_apply) {
		plugin->end_edit();
	} else {
		plugin->cancel_edit();
	}
	queue_redraw();
}

// UV menu: 0 rotate 90, 1 flip U, 2 flip V, 3 fit into 0..1, 4 weld (to the middle)
void HewnUVCanvas::_apply_simple(int p_what) {
	HewnMesh3D *n = _mesh();
	if (!n) {
		return;
	}
	const std::vector<uint64_t> loops = _selected_loops(n);
	if (loops.empty()) {
		return;
	}
	static const char *names[5] = { "Rotate UVs 90", "Flip UVs", "Flip UVs", "Fit UVs", "Weld UVs" };
	plugin->begin_edit(names[p_what]);
	Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f), avg;
	std::vector<Vector2> cur;
	for (uint64_t k : loops) {
		cur.push_back(_loop_uv(n, loop_face(k), loop_corner(k)));
		mn = mn.min(cur.back());
		mx = mx.max(cur.back());
		avg += cur.back() / (float)loops.size();
	}
	const Vector2 c = (mn + mx) * 0.5f;
	const float fit = MAX(mx.x - mn.x, mx.y - mn.y);
	for (size_t j = 0; j < loops.size(); j++) {
		const Vector2 d = cur[j] - c;
		Vector2 uv = cur[j];
		switch (p_what) {
			case 0:
				uv = c + Vector2(-d.y, d.x);
				break;
			case 1:
				uv = c + Vector2(-d.x, d.y);
				break;
			case 2:
				uv = c + Vector2(d.x, -d.y);
				break;
			case 3:
				uv = fit > 1e-9f ? (cur[j] - mn) / fit : cur[j];
				break;
			default:
				uv = avg;
				break;
		}
		_set_loop_uv(n, loop_face(loops[j]), loop_corner(loops[j]), uv);
	}
	n->commit(false);
	plugin->end_edit();
	queue_redraw();
}

// Islands of the selection (all shown when nothing is selected): the same texel density (their world
// size), turned to their smallest bounding box, then packed into 0..1 (p_pack) or only scaled in place.
void HewnUVCanvas::_pack(bool p_pack) {
	HewnMesh3D *n = _mesh();
	if (!n) {
		return;
	}
	const hewn::Mesh &m = n->get_data();
	std::vector<int> faces;
	for (int f : _shown_faces(n)) {
		if (sel.empty() || _face_selected(n, f)) {
			faces.push_back(f);
		}
	}
	if (faces.empty()) {
		for (int f : _shown_faces(n)) {
			bool any = false;
			for (int i = 0; i < m.faces[f].size() && !any; i++) {
				any = sel.count(loop_key(f, i)) > 0;
			}
			if (any) {
				faces.push_back(f);
			}
		}
	}
	if (faces.empty()) {
		return;
	}
	const std::vector<std::vector<int>> islands = _islands(n, faces);
	struct Isl {
		std::vector<uint64_t> loops;
		std::vector<Vector2> uv;
		float density = 1.0f;
	};
	std::vector<Isl> isl;
	float total_world = 0.0f, total_uv = 0.0f;
	for (const std::vector<int> &island : islands) {
		Isl I;
		float a3 = 0.0f, a2 = 0.0f;
		for (int f : island) {
			const hewn::Face &F = m.faces[f];
			for (int i = 1; i + 1 < F.size(); i++) {
				a3 += 0.5f * (m.pos[F.v[i]] - m.pos[F.v[0]]).cross(m.pos[F.v[i + 1]] - m.pos[F.v[0]]).length();
			}
			float s2 = 0.0f;
			for (int i = 0; i < F.size(); i++) {
				const Vector2 p = _loop_uv(n, f, i), q = _loop_uv(n, f, (i + 1) % F.size());
				s2 += p.cross(q);
				I.loops.push_back(loop_key(f, i));
				I.uv.push_back(p);
			}
			a2 += 0.5f * std::fabs(s2);
		}
		I.density = a2 > 1e-12f ? std::sqrt(a3 / a2) : 1.0f;
		total_world += a3;
		total_uv += a2;
		isl.push_back(I);
	}
	const float mean = total_uv > 1e-12f ? std::sqrt(total_world / total_uv) : 1.0f;
	plugin->begin_edit(p_pack ? "Pack UV Islands" : "Average UV Islands Scale");
	std::vector<Vector2> sizes;
	for (Isl &I : isl) {
		Vector2 c;
		for (const Vector2 &p : I.uv) {
			c += p / (float)I.uv.size();
		}
		if (pack_scale || !p_pack) {
			const float k = I.density / MAX(1e-9f, mean); // (texel density of the whole selection kept)
			for (Vector2 &p : I.uv) {
				p = c + (p - c) * k;
			}
		}
		if (!p_pack) {
			continue;
		}
		if (pack_rotate) {
			// the smallest bounding box: one of the hull's edges lies on it
			const std::vector<Vector2> hull = convex_hull(I.uv);
			float best_area = 1e30f, best_angle = 0.0f;
			for (size_t i = 0; i < hull.size(); i++) {
				const Vector2 e = hull[(i + 1) % hull.size()] - hull[i];
				if (e.length() < 1e-9f) {
					continue;
				}
				const float a = -e.angle();
				Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
				for (const Vector2 &p : hull) {
					const Vector2 r = rot(p, a);
					mn = mn.min(r);
					mx = mx.max(r);
				}
				const float area = (mx.x - mn.x) * (mx.y - mn.y);
				if (area < best_area - 1e-9f) {
					best_area = area;
					best_angle = a;
				}
			}
			Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
			for (Vector2 &p : I.uv) {
				p = rot(p, best_angle);
				mn = mn.min(p);
				mx = mx.max(p);
			}
			if (mx.y - mn.y > mx.x - mn.x) {
				for (Vector2 &p : I.uv) {
					p = Vector2(-p.y, p.x); // lying down: shelves fill better
				}
			}
		}
		Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
		for (const Vector2 &p : I.uv) {
			mn = mn.min(p);
			mx = mx.max(p);
		}
		for (Vector2 &p : I.uv) {
			p -= mn;
		}
		sizes.push_back((mx - mn).max(Vector2(1e-6f, 1e-6f)));
	}
	if (p_pack) {
		// the largest scale at which the shelves still fit (bisection)
		float big = 0.0f;
		for (const Vector2 &s : sizes) {
			big = MAX(big, MAX(s.x, s.y));
		}
		float lo = 0.0f, hi = 1.0f / MAX(1e-9f, big);
		std::vector<Vector2> pos, best_pos;
		for (int it = 0; it < 40; it++) {
			const float mid = (lo + hi) * 0.5f;
			if (shelf_pack(sizes, mid, pack_margin, pos)) {
				lo = mid;
				best_pos = pos;
			} else {
				hi = mid;
			}
		}
		if (best_pos.empty()) {
			shelf_pack(sizes, lo, pack_margin, best_pos);
		}
		for (size_t i = 0; i < isl.size(); i++) {
			for (Vector2 &p : isl[i].uv) {
				p = best_pos[i] + p * lo;
			}
		}
	}
	for (const Isl &I : isl) {
		for (size_t j = 0; j < I.loops.size(); j++) {
			_set_loop_uv(n, loop_face(I.loops[j]), loop_corner(I.loops[j]), I.uv[j]);
		}
	}
	n->commit(false);
	plugin->end_edit();
	if (p_pack) {
		frame_all();
	}
	queue_redraw();
}

// ------------------------------------------------------------------ header

void HewnUVCanvas::build_header(Control *p_parent) {
	HBoxContainer *h = memnew(HBoxContainer);
	h->set_name("HewnUVHeader");
	p_parent->add_child(h);
	auto menu = [&](const char *p_title, const std::vector<std::pair<const char *, int>> &p_items) {
		MenuButton *mb = memnew(MenuButton);
		mb->set_text(p_title);
		mb->set_flat(true);
		PopupMenu *pm = mb->get_popup();
		for (const auto &it : p_items) {
			if (it.second < 0) {
				pm->add_separator(it.first);
			} else if (it.second >= 1000) {
				pm->add_check_item(it.first, it.second);
			} else {
				pm->add_item(it.first, it.second);
			}
		}
		pm->connect("id_pressed", callable_mp(this, &HewnUVCanvas::on_menu));
		pm->connect("about_to_popup", callable_mp(this, &HewnUVCanvas::on_menu).bind(-1));
		h->add_child(mb);
		return mb;
	};
	menu("View", { { "Frame Selected  F", 100 }, { "Frame All  Home", 101 }, { "Grid", -1 }, { "1 / 4", 1104 }, { "1 / 8", 1108 }, { "1 / 16", 1116 }, { "1 / 32", 1132 },
						 { "1 / 64", 1164 }, { "Texture pixels", 1100 } });
	menu("Select", { { "All  A", 200 }, { "None  Alt+A", 201 }, { "Invert  Ctrl+I", 202 }, { "", -1 }, { "Linked  Ctrl+L", 203 }, { "Linked under the mouse  L", 204 },
						   { "", -1 }, { "Sync with the viewport", 1200 } });
	menu("Image", { { "Show Texture", 1300 } });
	menu("UV", { { "Pack Islands  Ctrl+P", 400 }, { "Average Islands Scale  Ctrl+A", 401 }, { "Pack: Rotate", 1410 }, { "Pack: Same Texel Density", 1411 }, { "", -1 },
					   { "Rotate 90", 402 }, { "Flip Horizontally", 403 }, { "Flip Vertically", 404 }, { "Fit into 0..1", 405 }, { "Weld (to the middle)", 406 },
					   { "", -1 }, { "Reset to the Projection", 407 } });
	VSeparator *s = memnew(VSeparator);
	h->add_child(s);
	static const char *icons[4] = { "", "vertex", "edge", "face" };
	static const char *tips[4] = { "", "UV vertex select (1)", "UV edge select (2)", "UV face select (3)" };
	for (int i = 1; i <= 3; i++) {
		Button *b = memnew(Button);
		b->set_flat(true);
		b->set_toggle_mode(true);
		if (plugin && plugin->dock) {
			b->set_button_icon(plugin->dock->icon(icons[i]));
		}
		b->set_tooltip_text(tips[i]);
		b->set_focus_mode(FOCUS_NONE);
		b->connect("pressed", callable_mp(this, &HewnUVCanvas::set_uv_mode).bind(i));
		h->add_child(b);
		mode_buttons[i] = b;
	}
	h->add_child(memnew(VSeparator));
	snap_button = memnew(Button);
	snap_button->set_flat(true);
	snap_button->set_focus_mode(FOCUS_NONE);
	snap_button->set_tooltip_text("Snapping options (the same as in the viewport's toolbar)");
	if (plugin && plugin->dock && plugin->dock->snap_target_button) {
		snap_button->set_button_icon(plugin->dock->snap_target_button->get_button_icon());
	}
	snap_button->connect("pressed", callable_mp(this, &HewnUVCanvas::on_snap_button));
	h->add_child(snap_button);
	set_uv_mode(uv_mode);
	localize(h);
}

void HewnUVCanvas::on_snap_button() {
	PopupPanel *pp = plugin && plugin->dock ? plugin->dock->snap_popup : nullptr;
	if (!pp || !snap_button) {
		return;
	}
	pp->reset_size();
	pp->popup(Rect2i(Vector2i(snap_button->get_screen_position() + Vector2(0, snap_button->get_size().y)), Vector2i()));
}

void HewnUVCanvas::set_uv_mode(int p_mode) {
	uv_mode = CLAMP(p_mode, 1, 3);
	for (int i = 1; i <= 3; i++) {
		if (mode_buttons[i]) {
			mode_buttons[i]->set_pressed_no_signal(i == uv_mode);
		}
	}
	queue_redraw();
}

void HewnUVCanvas::on_menu(int p_id) {
	HewnMesh3D *n = _mesh();
	if (p_id == -1) {
		// (before a menu opens: its check marks)
		for (Node *c : { get_parent()->find_child("HewnUVHeader", false, false) }) {
			if (!c) {
				continue;
			}
			for (int i = 0; i < c->get_child_count(); i++) {
				MenuButton *mb = Object::cast_to<MenuButton>(c->get_child(i));
				if (!mb) {
					continue;
				}
				PopupMenu *pm = mb->get_popup();
				for (int j = 0; j < pm->get_item_count(); j++) {
					const int id = pm->get_item_id(j);
					if (id >= 1100 && id < 1200) {
						pm->set_item_checked(j, id - 1100 == grid_div);
					} else if (id == 1200) {
						pm->set_item_checked(j, sync_selection);
					} else if (id == 1300) {
						pm->set_item_checked(j, show_texture);
					} else if (id == 1410) {
						pm->set_item_checked(j, pack_rotate);
					} else if (id == 1411) {
						pm->set_item_checked(j, pack_scale);
					}
				}
			}
		}
		return;
	}
	if (p_id >= 1100 && p_id < 1200) {
		grid_div = p_id - 1100;
	} else if (p_id == 1200) {
		sync_selection = !sync_selection;
		sel.clear();
	} else if (p_id == 1300) {
		show_texture = !show_texture;
	} else if (p_id == 1410) {
		pack_rotate = !pack_rotate;
	} else if (p_id == 1411) {
		pack_scale = !pack_scale;
	} else if (p_id == 100) {
		frame_selection();
	} else if (p_id == 101) {
		frame_all();
	} else if (n && p_id >= 200 && p_id <= 204) {
		const std::vector<int> faces = _shown_faces(n);
		if (p_id == 200 || p_id == 202) {
			std::set<uint64_t> all;
			for (int f : faces) {
				for (int i = 0; i < n->get_data().faces[f].size(); i++) {
					all.insert(loop_key(f, i));
				}
			}
			if (p_id == 202) {
				for (uint64_t k : sel) {
					all.erase(k);
				}
			}
			sel = all;
		} else if (p_id == 201) {
			sel.clear();
		} else {
			_select_linked(n, p_id == 204);
		}
		if (sync_selection) {
			// back to the viewport: the faces whose corners are all selected
			n->selection.faces.clear();
			for (int f : faces) {
				if (_face_selected(n, f)) {
					n->selection.faces.push_back(f);
				}
			}
			plugin->selection_changed();
		}
	} else if (p_id == 400 || p_id == 401) {
		_pack(p_id == 400);
	} else if (p_id >= 402 && p_id <= 406) {
		_apply_simple(p_id - 402);
	} else if (n && p_id == 407) {
		plugin->begin_edit("Reset UVs");
		for (uint64_t k : _selected_loops(n)) {
			hewn::Face &F = n->get_data().faces[loop_face(k)];
			F.manual_uv = false;
		}
		n->commit(false);
		plugin->end_edit();
	}
	queue_redraw();
}

// ------------------------------------------------------------------ drawing

void HewnUVCanvas::_draw() {
	const Vector2 size = get_size();
	draw_rect(Rect2(Vector2(), size), Color(0.12f, 0.13f, 0.15f));
	HewnMesh3D *n = _mesh();
	Ref<Font> font = get_theme_default_font();
	const int fs = get_theme_default_font_size();
	const Vector2 uv0 = to_uv(Vector2()), uv1 = to_uv(size);
	// the texture, repeated as the material tiles it
	if (n && show_texture) {
		Vector2 tiling;
		Ref<Texture2D> tex = _texture(n, tiling);
		if (tex.is_valid()) {
			const Vector2 cell(1.0f / tiling.x, 1.0f / tiling.y);
			const int x0 = (int)std::floor(uv0.x / cell.x), x1 = (int)std::floor(uv1.x / cell.x);
			const int y0 = (int)std::floor(uv0.y / cell.y), y1 = (int)std::floor(uv1.y / cell.y);
			if ((int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) <= 400) {
				for (int y = y0; y <= y1; y++) {
					for (int x = x0; x <= x1; x++) {
						const float a = (x == 0 && y == 0) ? 0.55f : 0.3f;
						draw_texture_rect(tex, Rect2(to_screen(Vector2(x * cell.x, y * cell.y)), cell * zoom), false, Color(1, 1, 1, a));
					}
				}
			}
		}
	}
	// grid: the snap step (only where it is not too dense), whole units, the 0..1 square
	const float g = _grid();
	if (g * zoom >= 6.0f) {
		for (float u = std::floor(uv0.x / g) * g; u <= uv1.x; u += g) {
			draw_line(Vector2(to_screen(Vector2(u, 0)).x, 0), Vector2(to_screen(Vector2(u, 0)).x, size.y), Color(1, 1, 1, 0.05f));
		}
		for (float v = std::floor(uv0.y / g) * g; v <= uv1.y; v += g) {
			draw_line(Vector2(0, to_screen(Vector2(0, v)).y), Vector2(size.x, to_screen(Vector2(0, v)).y), Color(1, 1, 1, 0.05f));
		}
	}
	for (float u = std::floor(uv0.x); u <= uv1.x && zoom > 3.0f; u += 1.0f) {
		draw_line(Vector2(to_screen(Vector2(u, 0)).x, 0), Vector2(to_screen(Vector2(u, 0)).x, size.y), Color(1, 1, 1, 0.16f));
	}
	for (float v = std::floor(uv0.y); v <= uv1.y && zoom > 3.0f; v += 1.0f) {
		draw_line(Vector2(0, to_screen(Vector2(0, v)).y), Vector2(size.x, to_screen(Vector2(0, v)).y), Color(1, 1, 1, 0.16f));
	}
	draw_rect(Rect2(to_screen(Vector2()), Vector2(zoom, zoom)), Color(0.4f, 0.7f, 1.0f, 0.6f), false, 1.5f);
	if (!n) {
		if (font.is_valid()) {
			draw_string(font, Vector2(10, 20), T("Select a HewnMesh3D to edit its UVs."));
		}
		return;
	}
	const hewn::Mesh &m = n->get_data();
	const std::vector<int> faces = _shown_faces(n);
	const Color c_sel(1, 0.6f, 0.15f), c_wire(0.75f, 0.8f, 0.9f, 0.45f);
	// faces (selected ones on top)
	for (int pass = 0; pass < 2; pass++) {
		for (int f : faces) {
			const bool fsel = _face_selected(n, f);
			if ((pass == 1) != fsel) {
				continue;
			}
			PackedVector2Array pts;
			for (int i = 0; i < m.faces[f].size(); i++) {
				pts.push_back(to_screen(_loop_uv(n, f, i)));
			}
			if (fsel && pts.size() >= 3 && uv_mode == UV_FACE) {
				for (int i = 1; i + 1 < pts.size(); i++) {
					PackedVector2Array tri;
					tri.push_back(pts[0]);
					tri.push_back(pts[i]);
					tri.push_back(pts[i + 1]);
					draw_colored_polygon(tri, Color(1, 0.55f, 0.1f, 0.2f));
				}
			}
			const int cnt = pts.size();
			for (int i = 0; i < cnt; i++) {
				const bool esel = sel.count(loop_key(f, i)) && sel.count(loop_key(f, (i + 1) % cnt));
				const bool hot = (uv_mode == UV_FACE && fsel) || (uv_mode != UV_FACE && esel);
				draw_line(pts[i], pts[(i + 1) % cnt], hot ? c_sel : c_wire, hot ? 1.6f : 1.0f, true);
			}
		}
	}
	if (uv_mode == UV_VERTEX) {
		for (int f : faces) {
			for (int i = 0; i < m.faces[f].size(); i++) {
				const bool s = sel.count(loop_key(f, i)) > 0;
				draw_rect(Rect2(to_screen(_loop_uv(n, f, i)) - Vector2(2.5f, 2.5f), Vector2(5, 5)), s ? c_sel : Color(0.9f, 0.92f, 1.0f, 0.8f));
			}
		}
	}
	if (xf != XF_NONE && xf_snapped) {
		draw_snap_symbol(this, to_screen(xf_snap_uv), xf_snap_kind, SNAP_COLOR, 6.0f);
	}
	if (boxing) {
		const Rect2 r = Rect2(press_pos, mouse - press_pos).abs();
		draw_rect(r, Color(1, 1, 1, 0.08f));
		draw_rect(r, Color(1, 1, 1, 0.6f), false, 1.0f);
	}
	if (font.is_valid()) {
		static const char *mode_names[4] = { "", "Vertex", "Edge", "Face" };
		String hint = !xf_info.is_empty() ? xf_info + T("   (X / Y: axis, Ctrl: snapping, LMB / Enter: done, RMB / Esc: cancel)")
										  : T(mode_names[uv_mode]) + T(":  click / box: select (Shift: add / remove), drag: move, G / R / S, L: island, Ctrl+P: pack, wheel: zoom, MMB: pan, F: frame");
		// wrapped at the panel's width, the last line at the bottom (a narrow panel: all of it still shows)
		const float w = MAX(60.0f, size.x - 20.0f);
		const Vector2 ts = font->get_multiline_string_size(hint, HORIZONTAL_ALIGNMENT_LEFT, w, fs);
		const float lh = font->get_height(fs);
		draw_multiline_string(font, Vector2(10, size.y - 8 - MAX(0.0f, ts.y - lh)), hint, HORIZONTAL_ALIGNMENT_LEFT, w, fs, -1, Color(1, 1, 1, 0.75f));
	}
}

void HewnUVCanvas::frame_selection() {
	HewnMesh3D *n = _mesh();
	Vector2 mn(0, 0), mx(1, 1);
	if (n) {
		std::vector<uint64_t> loops = _selected_loops(n);
		if (loops.empty()) {
			const hewn::Mesh &m = n->get_data();
			for (int f : _shown_faces(n)) {
				for (int i = 0; i < m.faces[f].size(); i++) {
					loops.push_back(loop_key(f, i));
				}
			}
		}
		if (!loops.empty()) {
			mn = Vector2(1e30f, 1e30f);
			mx = -mn;
			for (uint64_t k : loops) {
				const Vector2 u = _loop_uv(n, loop_face(k), loop_corner(k));
				mn = mn.min(u);
				mx = mx.max(u);
			}
		}
	}
	const Vector2 size = get_size().max(Vector2(64, 64));
	const Vector2 ext = (mx - mn).max(Vector2(0.01f, 0.01f));
	zoom = MIN(size.x / ext.x, size.y / ext.y) * 0.8f;
	pan = size * 0.5f - (mn + mx) * 0.5f * zoom;
	queue_redraw();
}

void HewnUVCanvas::frame_all() {
	const Vector2 size = get_size().max(Vector2(64, 64));
	zoom = MIN(size.x, size.y) * 0.8f;
	pan = size * 0.5f - Vector2(0.5f, 0.5f) * zoom;
	queue_redraw();
}

// ------------------------------------------------------------------ input

void HewnUVCanvas::_gui_input(const Ref<InputEvent> &p_event) {
	HewnMesh3D *n = _mesh();
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	Ref<InputEventKey> k = p_event;
	if (mb.is_valid()) {
		mouse = mb->get_position();
		if (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP || mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN) {
			if (mb->is_pressed()) {
				const float f = mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? 1.15f : 1.0f / 1.15f;
				const Vector2 at = to_uv(mb->get_position());
				zoom = CLAMP(zoom * f, 10.0f, 20000.0f);
				pan = mb->get_position() - at * zoom;
				queue_redraw();
			}
			accept_event();
			return;
		}
		if (mb->get_button_index() == MOUSE_BUTTON_MIDDLE) {
			panning = mb->is_pressed();
			drag_last = mb->get_position();
			accept_event();
			return;
		}
		if (xf != XF_NONE) {
			// a running G / R / S: left button / its release (tweak) applies, right cancels
			if (mb->get_button_index() == MOUSE_BUTTON_RIGHT && mb->is_pressed()) {
				_end_xf(false);
			} else if (mb->get_button_index() == MOUSE_BUTTON_LEFT && (mb->is_pressed() != xf_tweak)) {
				_end_xf(true);
			}
			pressed = false;
			accept_event();
			return;
		}
		if (mb->get_button_index() == MOUSE_BUTTON_LEFT && n) {
			if (mb->is_pressed()) {
				grab_focus();
				pressed = true;
				press_pos = mb->get_position();
				press_shift = mb->is_shift_pressed();
				press_ctrl = mb->is_ctrl_pressed();
			} else if (pressed) {
				pressed = false;
				if (boxing) {
					boxing = false;
					if (!press_shift && !press_ctrl) {
						sel.clear();
					}
					_box_select(n, Rect2(press_pos, mb->get_position() - press_pos).abs(), press_ctrl);
				} else {
					// a click: select what is under the mouse (Shift: add / remove it)
					std::set<uint64_t> hit;
					_pick(n, press_pos, hit);
					bool all_in = !hit.empty();
					for (uint64_t x : hit) {
						all_in = all_in && sel.count(x);
					}
					if (!press_shift) {
						sel.clear();
					}
					for (uint64_t x : hit) {
						if (press_shift && all_in) {
							sel.erase(x);
						} else {
							sel.insert(x);
						}
					}
				}
				if (sync_selection) {
					n->selection.faces.clear();
					for (int f : _shown_faces(n)) {
						if (_face_selected(n, f)) {
							n->selection.faces.push_back(f);
						}
					}
					if (plugin->mode != HewnEditorPlugin::MODE_FACE) {
						plugin->set_mode(HewnEditorPlugin::MODE_FACE);
					}
					plugin->selection_changed();
				}
				queue_redraw();
			}
			accept_event();
			return;
		}
		return;
	}
	if (mm.is_valid()) {
		mouse = mm->get_position();
		if (panning) {
			pan += mm->get_position() - drag_last;
			drag_last = mm->get_position();
			queue_redraw();
			return;
		}
		if (xf != XF_NONE) {
			_update_xf(mm->is_ctrl_pressed() || mm->is_meta_pressed());
			return;
		}
		if (pressed && n && !boxing && mouse.distance_to(press_pos) > 4.0f) {
			// a drag: from a selected element (or one picked now) it moves the selection, else a box
			std::set<uint64_t> hit;
			_pick(n, press_pos, hit);
			bool on_sel = !hit.empty();
			for (uint64_t x : hit) {
				on_sel = on_sel && sel.count(x);
			}
			if (!hit.empty() && !press_shift && !press_ctrl) {
				if (!on_sel) {
					sel = hit;
				}
				pressed = false;
				_start_xf(XF_MOVE, true);
			} else {
				boxing = true;
			}
		}
		if (boxing) {
			queue_redraw();
		}
		return;
	}
	if (k.is_null() || !k->is_pressed() || k->is_echo()) {
		return;
	}
	const Key code = key_code(k);
	const bool ctrl = k->is_ctrl_pressed() || k->is_meta_pressed(), shift = k->is_shift_pressed(), alt = k->is_alt_pressed();
	if (xf == XF_NONE && plugin && !plugin->hotkeys_on) {
		return; // (Editor Settings > Hewn > Interface: Hewn's keys off)
	}
	if (xf != XF_NONE) {
		if (code == KEY_ESCAPE) {
			_end_xf(false);
		} else if (code == KEY_ENTER || code == KEY_KP_ENTER || code == KEY_SPACE) {
			_end_xf(true);
		} else if (code == KEY_X || code == KEY_Y) {
			const int a = code == KEY_X ? 0 : 1;
			xf_axis = xf_axis == a ? -1 : a;
			_update_xf(ctrl);
		} else if (code == KEY_CTRL || code == KEY_META) {
			_update_xf(true);
		}
		accept_event();
		return;
	}
	if (!n) {
		return;
	}
	bool used = true;
	if (code == KEY_G && !ctrl && !alt) {
		_start_xf(XF_MOVE, false);
	} else if (code == KEY_R && !ctrl && !alt) {
		_start_xf(XF_ROTATE, false);
	} else if (code == KEY_S && !ctrl && !alt) {
		_start_xf(XF_SCALE, false);
	} else if (code == KEY_A && alt) {
		on_menu(201);
	} else if (code == KEY_A && ctrl) {
		on_menu(401);
	} else if (code == KEY_A) {
		on_menu(200);
	} else if (code == KEY_I && ctrl) {
		on_menu(202);
	} else if (code == KEY_L && ctrl) {
		on_menu(203);
	} else if (code == KEY_L) {
		on_menu(204);
	} else if (code == KEY_P && ctrl) {
		on_menu(400);
	} else if ((code == KEY_1 || code == KEY_2 || code == KEY_3) && !ctrl && !alt && !shift) {
		set_uv_mode(code == KEY_1 ? UV_VERTEX : code == KEY_2 ? UV_EDGE : UV_FACE);
	} else if (code == KEY_F) {
		frame_selection();
	} else if (code == KEY_HOME) {
		frame_all();
	} else {
		used = false;
	}
	if (used) {
		accept_event();
	}
}
