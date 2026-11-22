// UV editor (bottom dock): the UVs of the faces selected in the viewport (all faces when
// none is), selected by vertex / edge / face (1 / 2 / 3; coincident corners go along), moved, turned and
// scaled with G / R / S or by dragging (Godot's magnet snaps: increment = the grid, vertex = other UV
// vertices), islands packed into the 0..1 square. Header menus: View, Select, Image, UV.
#include "hw_editor.h"
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/undo_redo.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include "../hw_hotspot.h"
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/style_box.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <unordered_set>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/h_slider.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include "../hw_unwrap.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/v_separator.hpp>

#include <algorithm>
#include <map>
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
	if (n && sync_selection && plugin && plugin->mode != HewnEditorPlugin::MODE_OBJECT && _sel3d_key(n, plugin->mode) != synced_key) {
		// the viewport's selection is the UV one, in its element mode (the UV editor's mode follows it); not when it is
		// what the UV selection made it (an island picked here: not the corners of other islands at its vertices)
		synced_key = _sel3d_key(n, plugin->mode);
		sel.clear();
		const hewn::Mesh &m = n->get_data();
		const int pm = plugin->mode;
		if (uv_mask != (1 << pm)) {
			uv_mode = pm;
			uv_mask = 1 << pm;
			for (int i = 1; i <= 3; i++) {
				if (mode_buttons[i]) {
					mode_buttons[i]->set_pressed_no_signal(i == pm);
				}
			}
		}
		if (pm == HewnEditorPlugin::MODE_FACE) {
			for (int f : n->selection.faces) {
				if (f >= 0 && f < (int)m.faces.size()) {
					for (int i = 0; i < m.faces[f].size(); i++) {
						sel.insert(loop_key(f, i));
					}
				}
			}
		} else if (pm == HewnEditorPlugin::MODE_VERTEX) {
			std::unordered_set<int> vs(n->selection.verts.begin(), n->selection.verts.end());
			for (int f = 0; f < (int)m.faces.size(); f++) {
				for (int i = 0; i < m.faces[f].size(); i++) {
					if (vs.count(m.faces[f].v[i])) {
						sel.insert(loop_key(f, i));
					}
				}
			}
		} else {
			std::unordered_set<uint64_t> es;
			for (const hewn::Edge &e : n->selection.edges) {
				es.insert(hewn::ekey(e));
			}
			for (int f = 0; f < (int)m.faces.size(); f++) {
				const int c = m.faces[f].size();
				for (int i = 0; i < c; i++) {
					if (es.count(hewn::ekey(m.faces[f].v[i], m.faces[f].v[(i + 1) % c]))) {
						sel.insert(loop_key(f, i));
						sel.insert(loop_key(f, (i + 1) % c));
					}
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
	const bool all = !plugin || plugin->mode == HewnEditorPlugin::MODE_OBJECT || sync_selection;
	if (!all) {
		// (Isolate Islands: the whole islands of the faces selected in the viewport)
		std::vector<int> picked;
		if (!s.faces.empty()) {
			picked = s.faces;
		} else if (!s.verts.empty() || !s.edges.empty()) {
			picked = hewn::sel::faces_from_verts(m, s.verts.empty() ? hewn::sel::verts_of_edges(s.edges) : s.verts, false); // (the faces they touch)
		}
		if (!picked.empty() && !isolate_islands) {
			out = picked;
		} else if (!picked.empty()) {
			std::unordered_set<int> want(picked.begin(), picked.end());
			for (const std::vector<int> &is : _all_islands(p_n)) {
				bool hit = false;
				for (int f : is) {
					hit = hit || want.count(f);
				}
				if (hit) {
					out.insert(out.end(), is.begin(), is.end());
				}
			}
		}
	}
	if (all) {
		// (sync on, object mode: every island; else only what the viewport selects)
		for (int f = 0; f < (int)m.faces.size(); f++) {
			out.push_back(f);
		}
	}
	std::vector<int> shown;
	for (int f : out) {
		if (f >= 0 && f < (int)m.faces.size() && !m.faces[f].hidden && m.faces[f].size() >= 2 && !uv_hidden.count(f)) {
			shown.push_back(f);
		}
	}
	return shown;
}

Vector2 HewnUVCanvas::_loop_uv(HewnMesh3D *p_n, int p_face, int p_corner) const {
	const hewn::Face &F = p_n->get_data().faces[p_face];
	if (channel == 1) {
		// UV2: made by hand, else the lightmap's (when it has the same faces)
		if (F.uv2.size() == F.v.size() && p_corner < (int)F.uv2.size()) {
			return F.uv2[p_corner];
		}
		const std::vector<std::vector<Vector2>> &l = p_n->lightmap_face_uvs();
		if (l.size() == p_n->get_data().faces.size() && p_corner < (int)l[p_face].size()) {
			return l[p_face][p_corner];
		}
		return Vector2();
	}
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
	if (channel == 1) {
		if (F.uv2.size() != F.v.size()) {
			std::vector<Vector2> cur;
			for (int i = 0; i < F.size(); i++) {
				cur.push_back(_loop_uv(p_n, p_face, i));
			}
			F.uv2 = cur;
		}
		if (p_corner < (int)F.uv2.size()) {
			F.uv2[p_corner] = p_uv;
		}
		return;
	}
	if (F.hs_cell >= 0 && !F.hs_fixed && p_n->get_hotspot_auto()) {
		F.hs_fixed = true; // (hotspotted, then edited here: Auto Hotspot leaves it)
	}
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
// (several modes on: a vertex near the mouse first, then an edge, then a face)
bool HewnUVCanvas::_pick(HewnMesh3D *p_n, const Vector2 &p_pos, std::set<uint64_t> &r_loops) const {
	r_loops.clear();
	for (int k = UV_VERTEX; k <= UV_FACE; k++) {
		if (has_uv_mode(k) && _pick_kind(p_n, p_pos, k, r_loops)) {
			return true;
		}
	}
	return false;
}

bool HewnUVCanvas::_pick_kind(HewnMesh3D *p_n, const Vector2 &p_pos, int p_kind, std::set<uint64_t> &r_loops) const {
	const hewn::Mesh &m = p_n->get_data();
	const std::vector<int> faces = _shown_faces(p_n);
	r_loops.clear();
	if (p_kind == UV_VERTEX) {
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
	} else if (p_kind == UV_EDGE) {
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
		if (has_uv_mode(UV_FACE)) {
			Vector2 c;
			for (int i = 0; i < n; i++) {
				c += _loop_uv(p_n, f, i) / (float)n;
			}
			if (p_rect.has_point(to_screen(c))) {
				for (int i = 0; i < n; i++) {
					hit.insert(loop_key(f, i));
				}
			}
		}
		if (has_uv_mode(UV_VERTEX) || has_uv_mode(UV_EDGE)) {
			for (int i = 0; i < n; i++) {
				const bool a = p_rect.has_point(to_screen(_loop_uv(p_n, f, i)));
				if (has_uv_mode(UV_VERTEX) && a) {
					hit.insert(loop_key(f, i));
				} else if (has_uv_mode(UV_EDGE) && a && p_rect.has_point(to_screen(_loop_uv(p_n, f, (i + 1) % n)))) {
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
		_pick_kind(p_n, mouse, UV_FACE, seed);
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
	xf_typed = String();
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
	if (!xf_typed.is_empty() && xf_typed != "-" && xf_typed != ".") {
		// a value typed: G - along U (Y: along V), R - degrees, S - the factor (X / Y: along that axis only)
		const float val = (float)xf_typed.to_float();
		if (xf == XF_MOVE) {
			delta = xf_axis == 1 ? Vector2(0, val) : Vector2(val, 0);
			xf_info = vformat(T("Move  U %.4f  V %.4f"), delta.x, delta.y);
		} else if (xf == XF_ROTATE) {
			angle = Math::deg_to_rad(val);
			xf_info = vformat(T("Rotate  %.1f deg"), val);
		} else {
			factor = Vector2(xf_axis == 1 ? 1.0f : val, xf_axis == 0 ? 1.0f : val);
			xf_info = vformat(T("Scale  %.3f, %.3f"), factor.x, factor.y);
		}
		xf_info += "   [" + xf_typed + "]";
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
		if (p_pack && pack_density_on) {
			for (Vector2 &p : I.uv) {
				p = c + (p - c) * I.density; // (meters: its size in the world)
			}
		} else if (pack_scale || !p_pack) {
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
		if (pack_density_on) {
			// (the pixel density asked for: a UV unit is the texture's size in meters at it; the shelves as they come)
			Vector2 tiling;
			Ref<Texture2D> tex = _texture(n, tiling);
			const float tex_px = tex.is_valid() ? (float)tex->get_width() : 256.0f;
			lo = MAX(1e-9f, pack_density / (tex_px * MAX(1e-6f, tiling.x)));
			shelf_pack(sizes, lo, pack_margin, best_pos);
		}
		for (int it = 0; it < 40 && !pack_density_on; it++) {
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
		_show_pack_panel();
	}
	queue_redraw();
}

// ------------------------------------------------------------------ header

void HewnUVCanvas::build_header(Control *p_parent) {
	HBoxContainer *h = memnew(HBoxContainer);
	h->set_name("HewnUVHeader");
	p_parent->add_child(h);
	sync_button = memnew(Button);
	sync_button->set_name("UVSyncSelection");
	sync_button->set_toggle_mode(true);
	sync_button->set_theme_type_variation("FlatButton");
	sync_button->set_focus_mode(FOCUS_NONE);
	sync_button->set_tooltip_text("UV selection sync: all the islands shown, what is selected here is selected in the viewport too (and the other way "
								  "round). Off: only what the viewport selects is shown");
	if (plugin && plugin->dock) {
		sync_button->set_button_icon(plugin->dock->icon("uv_sync"));
	}
	sync_button->connect("toggled", callable_mp(this, &HewnUVCanvas::_on_sync_button));
	h->add_child(sync_button);
	auto menu = [&](const char *p_title, const std::vector<std::pair<const char *, int>> &p_items) {
		MenuButton *mb = memnew(MenuButton);
		mb->set_text(p_title);
		mb->set_flat(false);
		mb->set_theme_type_variation("FlatMenuButton"); // (as the editor's toolbars: pushed in while open, outlined under the mouse)
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
	menu("View", { { "Frame Selected  F", 100 }, { "Frame All  Home", 101 }, { "Isolate Islands", 1520 }, { "Grid", -1 }, { "1 / 4", 1104 }, { "1 / 8", 1108 },
						 { "1 / 16", 1116 }, { "1 / 32", 1132 }, { "1 / 64", 1164 }, { "Texture pixels", 1100 }, { "Overlays", -1 }, { "Stretch: Angle", 1501 },
						 { "Stretch: Area", 1502 }, { "Similar Islands", 1510 } });
	menu("Select", { { "All  A", 200 }, { "None  Alt+A", 201 }, { "Invert  Ctrl+I", 202 }, { "", -1 }, { "Linked  Ctrl+L", 203 }, { "Island under the mouse  L", 204 },
						   { "Overlapping Islands", 205 }, { "", -1 }, { "Hide Selected  H", 206 }, { "Hide Unselected  Shift+H", 207 }, { "Reveal  Alt+H", 208 },
						   });
	menu("Image", { { "Show Texture", 1300 }, { "Tile Texture", 1301 } });
	menu("UV", { { "Unwrap...  U", 450 }, { "", -1 }, { "Pack Islands  Ctrl+P", 400 }, { "Average Islands Scale  Ctrl+A", 401 }, { "Pack: Rotate", 1410 },
					   { "Pack: Same Texel Density", 1411 }, { "", -1 }, { "Quadrify", 451 }, { "Relax", 452 }, { "Align Horizontally", 453 }, { "Align Vertically", 454 },
					   { "Distribute Evenly", 455 }, { "Stack Similar Islands", 456 }, { "", -1 }, { "Fit to Hotspot Cell", 457 }, { "Fit to Hotspot Cell: Width", 458 },
					   { "Fit to Hotspot Cell: Height", 459 }, { "Fit: Keep Proportions", 1420 }, { "", -1 }, { "Rotate 90", 402 }, { "Flip Horizontally", 403 },
					   { "Flip Vertically", 404 }, { "Fit into 0..1", 405 }, { "Weld (to the middle)", 406 }, { "", -1 }, { "Reset to the Projection", 407 } });
	VSeparator *s = memnew(VSeparator);
	h->add_child(s);
	static const char *icons[4] = { "", "vertex", "edge", "face" };
	static const char *tips[4] = { "", "UV vertex select (1; Shift+1 / Shift+click: with the other modes)", "UV edge select (2; Shift+2 / Shift+click: with the other modes)",
		"UV face select (3; Shift+3 / Shift+click: with the other modes)" };
	for (int i = 1; i <= 3; i++) {
		Button *b = memnew(Button);
		b->set_flat(false);
		b->set_theme_type_variation("FlatButton");
		b->set_toggle_mode(true);
		if (plugin && plugin->dock) {
			b->set_button_icon(plugin->dock->icon(icons[i]));
		}
		b->set_tooltip_text(tips[i]);
		b->set_focus_mode(FOCUS_NONE);
		b->connect("pressed", callable_mp(this, &HewnUVCanvas::_on_uv_mode_button).bind(i));
		h->add_child(b);
		mode_buttons[i] = b;
	}
	h->add_child(memnew(VSeparator));
	channel_button = memnew(OptionButton);
	channel_button->add_item("UV", 0);
	channel_button->add_item("UV2", 1);
	channel_button->set_focus_mode(FOCUS_NONE);
	channel_button->set_tooltip_text("Which UVs are edited: the texture's (UV) or the second channel (UV2: the lightmap's, or a mask's). The viewport shows the change at once");
	channel_button->connect("item_selected", callable_mp(this, &HewnUVCanvas::_on_channel));
	h->add_child(channel_button);
	h->add_child(memnew(VSeparator));
	snap_button = memnew(Button);
	snap_button->set_flat(false);
	snap_button->set_theme_type_variation("FlatButton");
	snap_button->set_focus_mode(FOCUS_NONE);
	snap_button->set_tooltip_text("Snapping options (the same as in the viewport's toolbar). Blue: snapping is on.\nSnapping on / off: Y or Shift+Tab (here or in the 3D view); Ctrl while moving: the other way for that move");
	if (plugin && plugin->dock && plugin->dock->snap_target_button) {
		snap_button->set_button_icon(plugin->dock->snap_target_button->get_button_icon());
	}
	snap_button->connect("pressed", callable_mp(this, &HewnUVCanvas::on_snap_button));
	h->add_child(snap_button);
	// the stretch overlay's opacity (shown while it is on); Pack at a pixel density
	stretch_slider = memnew(HSlider);
	stretch_slider->set_name("UVStretchOpacity");
	stretch_slider->set_min(0.05);
	stretch_slider->set_max(1.0);
	stretch_slider->set_step(0.01);
	stretch_slider->set_value(stretch_opacity);
	stretch_slider->set_custom_minimum_size(Vector2(80, 0) * EditorInterface::get_singleton()->get_editor_scale());
	stretch_slider->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	stretch_slider->set_tooltip_text("The stretch overlay's opacity");
	stretch_slider->set_visible(false);
	stretch_slider->connect("value_changed", callable_mp(this, &HewnUVCanvas::_on_stretch_opacity));
	h->add_child(stretch_slider);
	set_uv_mode(uv_mode);
	localize(h);
}

void HewnUVCanvas::_on_channel(int p_index) {
	channel = CLAMP(p_index, 0, 1);
	sel.clear();
	queue_redraw();
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
	uv_mask = 1 << uv_mode;
	for (int i = 1; i <= 3; i++) {
		if (mode_buttons[i]) {
			mode_buttons[i]->set_pressed_no_signal(has_uv_mode(i));
		}
	}
	queue_redraw();
}

// A mode on / off with the others (one stays on).
void HewnUVCanvas::toggle_uv_mode(int p_mode) {
	p_mode = CLAMP(p_mode, 1, 3);
	const int bit = 1 << p_mode;
	if (uv_mask & bit) {
		if (uv_mask != bit) {
			uv_mask &= ~bit;
			for (int k = UV_FACE; k >= UV_VERTEX; k--) {
				if (has_uv_mode(k)) {
					uv_mode = k;
					break;
				}
			}
		}
	} else {
		uv_mask |= bit;
		uv_mode = p_mode;
	}
	for (int i = 1; i <= 3; i++) {
		if (mode_buttons[i]) {
			mode_buttons[i]->set_pressed_no_signal(has_uv_mode(i));
		}
	}
	queue_redraw();
}

void HewnUVCanvas::_on_uv_mode_button(int p_mode) {
	if (Input::get_singleton()->is_key_pressed(KEY_SHIFT)) {
		toggle_uv_mode(p_mode);
	} else {
		set_uv_mode(p_mode);
	}
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
					} else if (id == 1301) {
						pm->set_item_checked(j, tile_texture);
					} else if (id == 1501 || id == 1502) {
						pm->set_item_checked(j, stretch_view == id - 1500);
					} else if (id == 1510) {
						pm->set_item_checked(j, show_similar);
					} else if (id == 1520) {
						pm->set_item_checked(j, isolate_islands);
					} else if (id == 1420) {
						pm->set_item_checked(j, fit_uniform);
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
	} else if (p_id == 1301) {
		tile_texture = !tile_texture;
	} else if (p_id == 1501 || p_id == 1502) {
		stretch_view = stretch_view == p_id - 1500 ? 0 : p_id - 1500;
		if (stretch_slider) {
			stretch_slider->set_visible(stretch_view != 0);
		}
	} else if (p_id == 1510) {
		show_similar = !show_similar;
	} else if (p_id == 1520) {
		isolate_islands = !isolate_islands;
		sel.clear();
	} else if (p_id == 1420) {
		fit_uniform = !fit_uniform;
	} else if (p_id >= 450 && p_id <= 459) {
		static const char *ops[10] = { "", "quadrify", "relax", "align_h", "align_v", "distribute", "stack_similar", "fit_cell", "fit_cell_w", "fit_cell_h" };
		if (p_id == 450) {
			mouse = get_size() * 0.5f;
			open_u_menu();
		} else {
			run_uv_op(ops[p_id - 450]);
		}
	} else if (p_id == 205) {
		run_uv_op("select_overlap");
	} else if (p_id >= 206 && p_id <= 208) {
		_hide(p_id - 206);
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
			_sync_to_3d(n);
		}
	} else if (p_id == 400 || p_id == 401) {
		_pack(p_id == 400);
	} else if (p_id >= 402 && p_id <= 406) {
		_apply_simple(p_id - 402);
	} else if (n && p_id == 407) {
		plugin->begin_edit("Reset UVs");
		for (uint64_t k : _selected_loops(n)) {
			hewn::Face &F = n->get_data().faces[loop_face(k)];
			if (channel == 1) {
				F.uv2.clear(); // (back to the lightmap's own)
			} else {
				F.manual_uv = false;
			}
		}
		n->commit(false);
		plugin->end_edit();
	}
	queue_redraw();
}

// ------------------------------------------------------------------ sync, redraw, Pack's panel

void HewnUVCanvas::_on_sync_button(bool p_on) {
	sync_selection = p_on;
	sel.clear();
	queue_redraw();
}

uint64_t HewnUVCanvas::_sel3d_key(HewnMesh3D *p_n, int p_mode) {
	uint64_t key = 1469598103934665603ull + (uint64_t)p_mode * 31 + (uint64_t)p_n->get_instance_id();
	auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
	mix(p_n->get_data().faces.size());
	for (int f : p_n->selection.faces) {
		mix((uint64_t)f + 1);
	}
	for (int v : p_n->selection.verts) {
		mix((uint64_t)v + 7);
	}
	for (const hewn::Edge &e : p_n->selection.edges) {
		mix(hewn::ekey(e));
	}
	return key;
}

void HewnUVCanvas::_sync_to_3d(HewnMesh3D *p_n) {
	// the UV selection back to the viewport, in its element mode
	if (!plugin || plugin->mode == HewnEditorPlugin::MODE_OBJECT) {
		return;
	}
	const hewn::Mesh &m = p_n->get_data();
	hewn::Selection &s = p_n->selection;
	const std::vector<int> faces = _shown_faces(p_n);
	if (plugin->mode == HewnEditorPlugin::MODE_FACE) {
		s.faces.clear();
		for (int f : faces) {
			if (_face_selected(p_n, f)) {
				s.faces.push_back(f);
			}
		}
	} else if (plugin->mode == HewnEditorPlugin::MODE_VERTEX) {
		std::set<int> vs;
		for (uint64_t k : sel) {
			const int f = loop_face(k), i = loop_corner(k);
			if (f < (int)m.faces.size() && i < m.faces[f].size()) {
				vs.insert(m.faces[f].v[i]);
			}
		}
		s.verts.assign(vs.begin(), vs.end());
	} else {
		std::set<uint64_t> seen;
		s.edges.clear();
		for (int f : faces) {
			const int c = m.faces[f].size();
			for (int i = 0; i < c; i++) {
				if (sel.count(loop_key(f, i)) && sel.count(loop_key(f, (i + 1) % c))) {
					const hewn::Edge e(m.faces[f].v[i], m.faces[f].v[(i + 1) % c]);
					if (seen.insert(hewn::ekey(e)).second) {
						s.edges.push_back(e);
					}
				}
			}
		}
	}
	plugin->selection_changed();
	synced_key = _sel3d_key(p_n, plugin->mode); // (this selection is the UV one's: kept as it is here)
}

void HewnUVCanvas::_process(double p_delta) {
	if (!is_visible_in_tree() || !plugin) {
		return;
	}
	// (the sync button: only in edit mode - in object mode every island shows)
	if (sync_button) {
		sync_button->set_visible(plugin->mode != HewnEditorPlugin::MODE_OBJECT);
	}
	// drawn again when the mesh, its UVs or the viewport's selection changed (an operation from the viewport)
	HewnMesh3D *n = plugin->get_edited();
	uint64_t key = 1469598103934665603ull;
	auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
	if (n) {
		mix((uint64_t)n->get_instance_id());
		mix(n->get_revision());
		mix((uint64_t)plugin->mode);
		for (int f : n->selection.faces) {
			mix((uint64_t)f + 1);
		}
		for (int v : n->selection.verts) {
			mix((uint64_t)v + 7);
		}
		for (const hewn::Edge &e : n->selection.edges) {
			mix(hewn::ekey(e));
		}
	}
	if (key != seen_key) {
		seen_key = key;
		queue_redraw();
	}
	// Pack's panel: while its pack is the last step
	if (pack_panel && pack_panel->is_visible()) {
		EditorUndoRedoManager *ur = plugin->get_undo_redo();
		HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(pack_mesh)));
		UndoRedo *h = pm ? ur->get_history_undo_redo(ur->get_object_history_id(pm)) : nullptr;
		if (!h || (int64_t)h->get_version() != pack_version || pm != n) {
			pack_panel->hide();
		}
	}
}

void HewnUVCanvas::_show_pack_panel() {
	HewnMesh3D *n = _mesh();
	if (!n || !plugin) {
		return;
	}
	if (!pack_panel) {
		pack_panel = memnew(PanelContainer);
		pack_panel->set_name("UVPackPanel");
		Ref<StyleBox> sb = EditorInterface::get_singleton()->get_editor_theme()->get_stylebox("panel", "PopupPanel");
		if (sb.is_valid()) {
			pack_panel->add_theme_stylebox_override("panel", sb);
		}
		VBoxContainer *v = memnew(VBoxContainer);
		pack_panel->add_child(v);
		Label *t = memnew(Label);
		t->set_text("Pack Islands");
		v->add_child(t);
		GridContainer *g = memnew(GridContainer);
		g->set_columns(2);
		v->add_child(g);
		auto row = [&](const String &p_label, Control *p_field) {
			Label *l = memnew(Label);
			l->set_text(p_label);
			g->add_child(l);
			g->add_child(p_field);
		};
		pack_rotate_check = memnew(CheckBox);
		pack_rotate_check->connect("toggled", callable_mp(this, &HewnUVCanvas::_on_pack_toggle));
		row("Rotate", pack_rotate_check);
		pack_scale_check = memnew(CheckBox);
		pack_scale_check->connect("toggled", callable_mp(this, &HewnUVCanvas::_on_pack_toggle));
		row("Same Texel Density", pack_scale_check);
		density_check = memnew(CheckBox);
		density_check->set_name("UVPackDensity");
		density_check->set_tooltip_text("At this pixel density (the texture's pixels per meter of the faces) instead of filling the square");
		density_check->connect("toggled", callable_mp(this, &HewnUVCanvas::_on_pack_toggle));
		row("Pixel Density", density_check);
		density_field = memnew(SpinBox);
		density_field->set_min(1);
		density_field->set_max(65536);
		density_field->set_step(1);
		density_field->set_suffix("px/m");
		density_field->set_custom_minimum_size(Vector2(110, 0) * EditorInterface::get_singleton()->get_editor_scale());
		density_field->connect("value_changed", callable_mp(this, &HewnUVCanvas::_on_pack_option));
		row("", density_field);
		pack_margin_field = memnew(SpinBox);
		pack_margin_field->set_min(0);
		pack_margin_field->set_max(0.25);
		pack_margin_field->set_step(0.001);
		pack_margin_field->connect("value_changed", callable_mp(this, &HewnUVCanvas::_on_pack_option));
		row("Margin", pack_margin_field);
		localize(pack_panel);
		add_child(pack_panel);
	}
	pack_rotate_check->set_pressed_no_signal(pack_rotate);
	pack_scale_check->set_pressed_no_signal(pack_scale);
	density_check->set_pressed_no_signal(pack_density_on);
	density_field->set_value_no_signal(pack_density);
	density_field->set_editable(pack_density_on);
	pack_margin_field->set_value_no_signal(pack_margin);
	pack_panel->reset_size();
	pack_panel->set_position(Vector2(8, get_size().y - pack_panel->get_combined_minimum_size().y - 8));
	pack_panel->show();
	EditorUndoRedoManager *ur = plugin->get_undo_redo();
	UndoRedo *h = ur->get_history_undo_redo(ur->get_object_history_id(n));
	pack_version = h ? (int64_t)h->get_version() : -1;
	pack_mesh = (uint64_t)n->get_instance_id();
}

void HewnUVCanvas::_on_pack_toggle(bool p_unused) {
	_on_pack_option(0.0);
}

void HewnUVCanvas::_on_pack_option(double p_unused) {
	HewnMesh3D *n = _mesh();
	if (!n || !plugin || !pack_panel) {
		return;
	}
	pack_rotate = pack_rotate_check->is_pressed();
	pack_scale = pack_scale_check->is_pressed();
	pack_density_on = density_check->is_pressed();
	pack_density = MAX(1.0f, (float)density_field->get_value());
	pack_margin = (float)pack_margin_field->get_value();
	// (the last pack taken back, packed again with these)
	EditorUndoRedoManager *ur = plugin->get_undo_redo();
	UndoRedo *h = ur->get_history_undo_redo(ur->get_object_history_id(n));
	if (!h || (int64_t)h->get_version() != pack_version) {
		pack_panel->hide();
		return;
	}
	const std::set<uint64_t> keep = sel;
	h->undo();
	sel = keep;
	_pack(true);
}

// ------------------------------------------------------------------ drawing

void HewnUVCanvas::_draw() {
	const Vector2 size = get_size();
	// (behind it: one of the editor's own dark colors, as its panels and the hotspot atlas')
	Ref<Theme> et = EditorInterface::get_singleton()->get_editor_theme();
	draw_rect(Rect2(Vector2(), size), et.is_valid() && et->has_color("dark_color_2", "Editor") ? et->get_color("dark_color_2", "Editor") : Color(0.15f, 0.15f, 0.15f));
	HewnMesh3D *n = _mesh();
	Ref<Font> font = get_theme_default_font();
	const int fs = get_theme_default_font_size();
	const Vector2 uv0 = to_uv(Vector2()), uv1 = to_uv(size);
	// the texture, repeated as the material tiles it
	if (n && show_texture) {
		Vector2 tiling;
		Ref<Texture2D> tex = _texture(n, tiling);
		if (tex.is_valid() && !tile_texture) {
			draw_texture_rect(tex, Rect2(to_screen(Vector2()), Vector2(1.0f / tiling.x, 1.0f / tiling.y) * zoom), false, Color(1, 1, 1, 0.55f));
		} else if (tex.is_valid()) {
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
	// the viewport's colors (Editor Settings > Hewn > Viewport): points, selected points, selected edges and faces
	const HwMeshStyle &st = hw_mesh_style();
	const bool by_elements = has_uv_mode(UV_EDGE) || has_uv_mode(UV_FACE);
	const Color c_wire = by_elements ? Color::hex(0x6a6a6aff) : Color::hex(0x9a9a9aff);
	const Color c_esel = st.edge_sel, c_vsel = st.vert_sel;
	const float dot = MAX(1.5f, st.vert_size * 0.32f);
	// overlays under the wires: the stretch (smooth from corner to corner), similar islands in their group's color
	if (stretch_view != 0) {
		float a3 = 0.0f, a2 = 0.0f;
		for (int f : faces) {
			a3 += hewn::face_normal_raw(m, m.faces[f]).length() * 0.5f;
			for (int i = 0; i < m.faces[f].size(); i++) {
				a2 += _loop_uv(n, f, i).cross(_loop_uv(n, f, (i + 1) % m.faces[f].size())) * 0.5f;
			}
		}
		const float ref = a3 > 1e-12f ? std::fabs(a2) / a3 : 1.0f;
		// (each corner's value, averaged over the corners at one place: one vertex, one UV)
		std::map<std::pair<int, std::pair<int64_t, int64_t>>, std::pair<float, int>> at;
		std::vector<std::vector<float>> vals(m.faces.size());
		auto key = [&](int f, int i) {
			const Vector2 u = _loop_uv(n, f, i);
			return std::make_pair(m.faces[f].v[i], std::make_pair((int64_t)std::llround(u.x * 1e5), (int64_t)std::llround(u.y * 1e5)));
		};
		for (int f : faces) {
			std::vector<Vector2> q;
			for (int i = 0; i < m.faces[f].size(); i++) {
				q.push_back(_loop_uv(n, f, i));
			}
			hewn::unwrap::stretch_corners(m, f, q, ref, stretch_view == 1 ? 0 : 1, vals[f]);
			if (stretch_view == 1) {
				for (int i = 0; i < (int)vals[f].size(); i++) {
					auto &e = at[key(f, i)];
					e.first += vals[f][i];
					e.second++;
				}
			}
		}
		for (int f : faces) {
			PackedVector2Array pts;
			PackedColorArray cols;
			for (int i = 0; i < m.faces[f].size() && i < (int)vals[f].size(); i++) {
				pts.push_back(to_screen(_loop_uv(n, f, i)));
				float t = vals[f][i];
				if (stretch_view == 1) {
					const auto &e = at[key(f, i)];
					t = e.second ? e.first / e.second : t;
				}
				const Color c = hewn::unwrap::stretch_color(t);
				cols.push_back(Color(c.r, c.g, c.b, stretch_opacity));
			}
			for (int i = 1; i + 1 < pts.size(); i++) {
				draw_polygon(PackedVector2Array({ pts[0], pts[i], pts[i + 1] }), PackedColorArray({ cols[0], cols[i], cols[i + 1] }));
			}
		}
	}
	if (show_similar) {
		const std::vector<std::vector<int>> is = _islands(n, faces);
		const std::vector<int> grp = _similar_groups(n, is);
		for (size_t k = 0; k < is.size(); k++) {
			if (grp[k] < 0) {
				continue;
			}
			const Color c = Color::from_hsv(std::fmod(grp[k] * 0.61803f, 1.0f), 0.5f, 0.9f, 0.35f);
			for (int f : is[k]) {
				PackedVector2Array pts;
				for (int i = 0; i < m.faces[f].size(); i++) {
					pts.push_back(to_screen(_loop_uv(n, f, i)));
				}
				for (int i = 1; i + 1 < pts.size(); i++) {
					draw_colored_polygon(PackedVector2Array({ pts[0], pts[i], pts[i + 1] }), c);
				}
			}
		}
	}
	// the faces all of whose corners are selected: filled as selected (in any mode)
	for (int f : faces) {
		if (_face_selected(n, f) && m.faces[f].size() >= 3) {
			PackedVector2Array pts;
			for (int i = 0; i < m.faces[f].size(); i++) {
				pts.push_back(to_screen(_loop_uv(n, f, i)));
			}
			for (int i = 1; i + 1 < pts.size(); i++) {
				draw_colored_polygon(PackedVector2Array({ pts[0], pts[i], pts[i + 1] }), st.face_sel);
			}
		}
	}
	// the wires: the plain ones first, the selected ones (and a selected point's) over them, so they show where corners
	// lie on each other
	struct Seg {
		Vector2 a, b;
		Color ca, cb;
		float w;
	};
	std::vector<Seg> plain, hot;
	for (int f : faces) {
		const bool fsel = _face_selected(n, f);
		const int cnt = m.faces[f].size();
		for (int i = 0; i < cnt; i++) {
			const Vector2 pa = to_screen(_loop_uv(n, f, i)), pb = to_screen(_loop_uv(n, f, (i + 1) % cnt));
			const bool sa = sel.count(loop_key(f, i)) > 0, sb = sel.count(loop_key(f, (i + 1) % cnt)) > 0;
			if ((has_uv_mode(UV_FACE) && fsel) || (has_uv_mode(UV_EDGE) && sa && sb) || (has_uv_mode(UV_VERTEX) && sa && sb)) {
				hot.push_back({ pa, pb, c_esel, c_esel, 1.6f });
			} else if (has_uv_mode(UV_VERTEX) && (sa || sb)) {
				// (as in the viewport: from a selected point the edge fades to the next one's color)
				hot.push_back({ pa, pb, sa ? c_esel : c_wire, sb ? c_esel : c_wire, 1.4f });
			} else {
				plain.push_back({ pa, pb, c_wire, c_wire, 1.0f });
			}
		}
	}
	// (under the plain ones a thin dark line: they show on any texture)
	for (const Seg &s : plain) {
		draw_line(s.a, s.b, Color(0, 0, 0, 0.45f), 2.6f, true);
	}
	for (const std::vector<Seg> *list : { &plain, &hot }) {
		for (const Seg &s : *list) {
			if (s.ca == s.cb) {
				draw_line(s.a, s.b, s.ca, s.w, true);
			} else {
				draw_polyline_colors(PackedVector2Array({ s.a, s.b }), PackedColorArray({ s.ca, s.cb }), s.w, true);
			}
		}
	}
	if (has_uv_mode(UV_FACE)) {
		// the faces' middles: dots, as in the viewport's face mode (the selected ones last)
		for (int pass = 0; pass < 2; pass++) {
			for (int f : faces) {
				const bool fsel = _face_selected(n, f);
				if ((pass == 1) != fsel || m.faces[f].size() < 3) {
					continue;
				}
				Vector2 c;
				for (int i = 0; i < m.faces[f].size(); i++) {
					c += to_screen(_loop_uv(n, f, i)) / (float)m.faces[f].size();
				}
				draw_circle(c, dot * 0.85f, fsel ? c_vsel : st.vert, true, -1.0f, true);
			}
		}
	}
	if (has_uv_mode(UV_VERTEX)) {
		for (int pass = 0; pass < 2; pass++) {
			for (int f : faces) {
				for (int i = 0; i < m.faces[f].size(); i++) {
					const bool s = sel.count(loop_key(f, i)) > 0;
					if ((pass == 1) == s) {
						draw_circle(to_screen(_loop_uv(n, f, i)), dot, s ? c_vsel : st.vert, true, -1.0f, true);
					}
				}
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
	if (font.is_valid() && !xf_info.is_empty()) {
		// (a running move / turn / scale: its numbers only)
		draw_string(font, Vector2(10, size.y - 10), xf_info, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color(1, 1, 1, 0.8f));
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
					_sync_to_3d(n);
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
		} else if ((code >= KEY_0 && code <= KEY_9) || (code >= KEY_KP_0 && code <= KEY_KP_9)) {
			xf_typed += String::num_int64(code >= KEY_KP_0 ? (int)(code - KEY_KP_0) : (int)(code - KEY_0));
			_update_xf(ctrl);
		} else if (code == KEY_PERIOD || code == KEY_KP_PERIOD || code == KEY_COMMA) {
			if (!xf_typed.contains(".")) {
				xf_typed += ".";
			}
			_update_xf(ctrl);
		} else if (code == KEY_MINUS || code == KEY_KP_SUBTRACT) {
			xf_typed = xf_typed.begins_with("-") ? xf_typed.substr(1) : "-" + xf_typed;
			_update_xf(ctrl);
		} else if (code == KEY_BACKSPACE) {
			xf_typed = xf_typed.substr(0, MAX(0, xf_typed.length() - 1));
			_update_xf(ctrl);
		} else if (code == KEY_CTRL || code == KEY_META) {
			_update_xf(true);
		}
		accept_event();
		return;
	}
	if (((code == KEY_Y && !ctrl && !alt && !shift) || (code == KEY_TAB && shift && !ctrl && !alt)) && plugin && plugin->dock && plugin->dock->snap_enabled) {
		// snapping on / off, as in the 3D view
		plugin->dock->snap_enabled->set_pressed(!plugin->dock->snap_enabled->is_pressed());
		plugin->dock->update_snap_highlight();
		queue_redraw();
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
	} else if (code == KEY_U && !ctrl && !alt && !shift) {
		open_u_menu(); // (the UV editor's own: unwraps, projections, Quadrify, Relax, align, fit)
	} else if (code == KEY_H && !ctrl) {
		_hide(alt ? 2 : shift ? 1 : 0);
	} else if ((code == KEY_1 || code == KEY_2 || code == KEY_3) && !ctrl && !alt) {
		const int md = code == KEY_1 ? UV_VERTEX : code == KEY_2 ? UV_EDGE : UV_FACE;
		if (shift) {
			toggle_uv_mode(md); // (Shift: with the other modes)
		} else {
			set_uv_mode(md);
		}
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
