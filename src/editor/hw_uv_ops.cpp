// UV editor operations (its U menu and its UV menu): unwraps and projections of the selected faces, Quadrify,
// Relax, vertices in a row / evenly along their line, the overlapping islands (one layer of them), similar islands
// (colored by group, the selected ones laid onto the one that is not), a fit into a hotspot cell, hiding.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include "../hw_unwrap.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/popup_menu.hpp>

#include <algorithm>
#include <functional>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

using namespace godot;
using namespace hw_editor;

namespace {
uint64_t lkey(int f, int i) {
	return ((uint64_t)(uint32_t)f << 32) | (uint32_t)i;
}

bool in_poly(const Vector2 &p, const std::vector<Vector2> &poly) {
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

bool cross_proper(const Vector2 &a, const Vector2 &b, const Vector2 &c, const Vector2 &d) {
	const float d1 = (b - a).cross(c - a), d2 = (b - a).cross(d - a), d3 = (d - c).cross(a - c), d4 = (d - c).cross(b - c);
	const float e = 1e-9f;
	return ((d1 > e && d2 < -e) || (d1 < -e && d2 > e)) && ((d3 > e && d4 < -e) || (d3 < -e && d4 > e));
}

// two polygons overlap (not only touch)
bool overlap(const std::vector<Vector2> &a, const std::vector<Vector2> &b) {
	Vector2 ca, cb;
	for (const Vector2 &p : a) {
		ca += p / (float)a.size();
	}
	for (const Vector2 &p : b) {
		cb += p / (float)b.size();
	}
	if (in_poly(ca, b) || in_poly(cb, a)) {
		return true;
	}
	for (size_t i = 0; i < a.size(); i++) {
		for (size_t j = 0; j < b.size(); j++) {
			if (cross_proper(a[i], a[(i + 1) % a.size()], b[j], b[(j + 1) % b.size()])) {
				return true;
			}
		}
	}
	return false;
}
} // namespace

const std::vector<std::vector<int>> &HewnUVCanvas::_all_islands(HewnMesh3D *p_n) const {
	const uint64_t key = p_n->get_revision() * 2654435761ull + (uint64_t)p_n->get_instance_id() * 31 + (uint64_t)channel;
	if (key != islands_key) {
		islands_key = key;
		std::vector<int> all;
		for (int f = 0; f < (int)p_n->get_data().faces.size(); f++) {
			if (!p_n->get_data().faces[f].hidden && p_n->get_data().faces[f].size() >= 2) {
				all.push_back(f);
			}
		}
		islands_all = _islands(p_n, all);
	}
	return islands_all;
}

std::vector<int> HewnUVCanvas::_op_faces(HewnMesh3D *p_n) const {
	const std::vector<int> shown = _shown_faces(p_n);
	std::vector<int> full, part;
	for (int f : shown) {
		const hewn::Face &F = p_n->get_data().faces[f];
		int c = 0;
		for (int i = 0; i < F.size(); i++) {
			c += sel.count(lkey(f, i)) ? 1 : 0;
		}
		if (c == F.size() && c > 0) {
			full.push_back(f);
		} else if (c > 0) {
			part.push_back(f);
		}
	}
	if (!full.empty()) {
		return full;
	}
	if (!part.empty()) {
		return part;
	}
	return shown;
}

void HewnUVCanvas::_write_layout(HewnMesh3D *p_n, const std::map<int, std::vector<Vector2>> &p_layout) {
	for (const auto &kv : p_layout) {
		const hewn::Face &F = p_n->get_data().faces[kv.first];
		for (int i = 0; i < F.size() && i < (int)kv.second.size(); i++) {
			_set_loop_uv(p_n, kv.first, i, kv.second[i]);
		}
	}
}

std::vector<int> HewnUVCanvas::_similar_groups(HewnMesh3D *p_n, const std::vector<std::vector<int>> &p_islands) const {
	// alike: as many faces and corners, the same shape whatever its size - each face's share of the island's area and
	// its sides against the island's size, face by face (in order), within 5%
	const hewn::Mesh &m = p_n->get_data();
	struct Sig {
		int faces = 0, corners = 0;
		std::vector<std::vector<float>> each; // (per face: its share of the area, then its sides, longest first)
	};
	std::vector<Sig> sig(p_islands.size());
	for (size_t k = 0; k < p_islands.size(); k++) {
		float total = 0.0f;
		for (int f : p_islands[k]) {
			total += hewn::face_normal_raw(m, m.faces[f]).length() * 0.5f;
		}
		const float size = std::sqrt(MAX(1e-12f, total));
		for (int f : p_islands[k]) {
			const hewn::Face &F = m.faces[f];
			std::vector<float> d;
			for (int i = 0; i < F.size(); i++) {
				d.push_back(m.pos[F.v[i]].distance_to(m.pos[F.v[(i + 1) % F.size()]]) / size);
			}
			std::sort(d.begin(), d.end(), std::greater<float>());
			d.insert(d.begin(), hewn::face_normal_raw(m, F).length() * 0.5f / MAX(1e-12f, total));
			sig[k].each.push_back(d);
			sig[k].faces++;
			sig[k].corners += F.size();
		}
		std::sort(sig[k].each.begin(), sig[k].each.end());
	}
	auto alike = [&](const Sig &a, const Sig &b) {
		if (a.faces != b.faces || a.corners != b.corners) {
			return false;
		}
		for (size_t i = 0; i < a.each.size(); i++) {
			if (a.each[i].size() != b.each[i].size()) {
				return false;
			}
			for (size_t j = 0; j < a.each[i].size(); j++) {
				if (std::fabs(a.each[i][j] - b.each[i][j]) > 0.05f * MAX(a.each[i][j], b.each[i][j]) + 1e-4f) {
					return false;
				}
			}
		}
		return true;
	};
	std::vector<int> group(p_islands.size(), -1);
	int next = 0;
	for (size_t i = 0; i < p_islands.size(); i++) {
		if (group[i] >= 0) {
			continue;
		}
		bool any = false;
		for (size_t j = i + 1; j < p_islands.size(); j++) {
			if (group[j] < 0 && alike(sig[i], sig[j])) {
				group[j] = next;
				any = true;
			}
		}
		if (any) {
			group[i] = next++;
		}
	}
	return group;
}

void HewnUVCanvas::_hide(int p_how) {
	HewnMesh3D *n = _mesh();
	if (!n) {
		return;
	}
	if (p_how == 2) {
		uv_hidden.clear();
	} else {
		const std::vector<int> shown = _shown_faces(n);
		for (int f : shown) {
			if (_face_selected(n, f) == (p_how == 0)) {
				uv_hidden.insert(f);
			}
		}
		sel.clear();
	}
	queue_redraw();
}

void HewnUVCanvas::open_u_menu() {
	if (!u_menu) {
		u_menu = memnew(PopupMenu);
		u_menu->set_name("HewnUVUMenu");
		static const char *items[][2] = { { "Unwrap (Angle Based)", "unwrap_angle" }, { "Unwrap (Conformal)", "unwrap_conformal" },
			{ "Unwrap (Minimum Stretch)", "unwrap_minstretch" }, { "Smart UV Project", "smart_project" }, { "Lightmap Pack", "lightmap_unwrap" }, { "", "" },
			{ "Cube Projection", "project_box" }, { "Cylinder Projection", "project_cylinder" }, { "Sphere Projection", "project_sphere" }, { "", "" },
			{ "Quadrify", "quadrify" }, { "Relax", "relax" }, { "Pack Islands", "pack" }, { "", "" },
			{ "Align Horizontally", "align_h" }, { "Align Vertically", "align_v" }, { "Distribute Evenly", "distribute" }, { "", "" },
			{ "Select Overlapping Islands", "select_overlap" }, { "Stack Similar Islands", "stack_similar" }, { "", "" },
			{ "Fit to Hotspot Cell", "fit_cell" }, { "Fit to Hotspot Cell: Width", "fit_cell_w" }, { "Fit to Hotspot Cell: Height", "fit_cell_h" } };
		for (int i = 0; i < (int)(sizeof(items) / sizeof(items[0])); i++) {
			if (!items[i][0][0]) {
				u_menu->add_separator();
			} else {
				u_menu->add_item(T(items[i][0]), i);
				u_menu->set_item_metadata(u_menu->get_item_count() - 1, String(items[i][1]));
			}
		}
		u_menu->connect("id_pressed", callable_mp(this, &HewnUVCanvas::_on_u_menu));
		add_child(u_menu);
	}
	u_menu->reset_size();
	u_menu->set_position(Vector2i(get_screen_position() + mouse));
	u_menu->popup();
}

void HewnUVCanvas::_on_u_menu(int p_id) {
	if (!u_menu) {
		return;
	}
	const int idx = u_menu->get_item_index(p_id);
	if (idx >= 0) {
		run_uv_op(u_menu->get_item_metadata(idx));
	}
}

void HewnUVCanvas::run_uv_op(const String &p_op) {
	HewnMesh3D *n = _mesh();
	if (!n || !plugin) {
		return;
	}
	const hewn::Mesh &m = n->get_data();
	if (p_op == "select_overlap") {
		// of each set of islands lying over each other, one (the last): moved, the others show
		const std::vector<int> shown = _shown_faces(n);
		const std::vector<std::vector<int>> is = _islands(n, shown);
		std::vector<std::vector<std::vector<Vector2>>> polys(is.size());
		std::vector<Rect2> box(is.size());
		for (size_t k = 0; k < is.size(); k++) {
			Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
			for (int f : is[k]) {
				std::vector<Vector2> q;
				for (int i = 0; i < m.faces[f].size(); i++) {
					q.push_back(_loop_uv(n, f, i));
					mn = mn.min(q.back());
					mx = mx.max(q.back());
				}
				polys[k].push_back(q);
			}
			box[k] = Rect2(mn, mx - mn);
		}
		std::vector<int> parent(is.size());
		std::iota(parent.begin(), parent.end(), 0);
		std::function<int(int)> find = [&](int x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
		for (size_t a = 0; a < is.size(); a++) {
			for (size_t b = a + 1; b < is.size(); b++) {
				if (!box[a].grow(-1e-6f).intersects(box[b].grow(-1e-6f))) {
					continue;
				}
				bool hit = false;
				for (size_t i = 0; i < polys[a].size() && !hit; i++) {
					for (size_t j = 0; j < polys[b].size() && !hit; j++) {
						hit = polys[a][i].size() >= 3 && polys[b][j].size() >= 3 && overlap(polys[a][i], polys[b][j]);
					}
				}
				if (hit) {
					parent[find((int)a)] = find((int)b);
				}
			}
		}
		std::map<int, std::vector<int>> groups;
		for (size_t k = 0; k < is.size(); k++) {
			groups[find((int)k)].push_back((int)k);
		}
		sel.clear();
		for (const auto &g : groups) {
			if (g.second.size() >= 2) {
				for (int f : is[g.second.back()]) {
					for (int i = 0; i < m.faces[f].size(); i++) {
						sel.insert(lkey(f, i));
					}
				}
			}
		}
		queue_redraw();
		return;
	}
	if (p_op == "fit_cell" || p_op == "fit_cell_w" || p_op == "fit_cell_h") {
		fit_mode = p_op == "fit_cell" ? 0 : p_op == "fit_cell_w" ? 1 : 2;
		if (plugin->dock) {
			plugin->dock->hs_palette_for_uv = true; // (the cell picked there: the selection fitted into it)
			plugin->dock->_on_hs_choose();
		}
		return;
	}
	const std::vector<int> faces = _op_faces(n);
	if (faces.empty()) {
		return;
	}
	plugin->begin_edit("UV " + p_op.capitalize());
	std::map<int, std::vector<Vector2>> lay;
	auto current = [&](const std::vector<int> &fs) {
		std::map<int, std::vector<Vector2>> cur;
		for (int f : fs) {
			std::vector<Vector2> q;
			for (int i = 0; i < m.faces[f].size(); i++) {
				q.push_back(_loop_uv(n, f, i));
			}
			cur[f] = q;
		}
		return cur;
	};
	auto origin_of = [&](const std::vector<int> &fs) {
		Vector2 mn(1e30f, 1e30f);
		for (int f : fs) {
			for (int i = 0; i < m.faces[f].size(); i++) {
				mn = mn.min(_loop_uv(n, f, i));
			}
		}
		return mn.x > 1e29f ? Vector2() : mn;
	};
	if (p_op == "unwrap_angle" || p_op == "unwrap_conformal" || p_op == "unwrap_minstretch" || p_op == "smart_project" || p_op == "lightmap_unwrap" ||
			p_op == "project_box" || p_op == "project_cylinder" || p_op == "project_sphere") {
		const Vector2 origin = origin_of(faces);
		std::vector<std::vector<int>> islands;
		if (p_op == "smart_project") {
			islands = hewn::unwrap::smart_project(m, faces, 66.0f, lay);
		} else if (p_op == "lightmap_unwrap") {
			std::vector<char> skip(m.faces.size(), 1);
			for (int f : faces) {
				skip[f] = 0;
			}
			std::vector<std::vector<Vector2>> uv2;
			hewn::lightmap_unwrap(m, MAX(0.001f, n->get_lightmap_texel_size()), skip, uv2);
			for (int f : faces) {
				lay[f] = uv2[f];
			}
		} else if (p_op.begins_with("project_")) {
			Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
			for (int f : faces) {
				for (int v : m.faces[f].v) {
					mn = mn.min(m.pos[v]);
					mx = mx.max(m.pos[v]);
				}
			}
			const Transform3D frame(Basis(), (mn + mx) * 0.5f);
			if (p_op == "project_box") {
				hewn::unwrap::project_box(m, faces, frame, lay);
			} else if (p_op == "project_cylinder") {
				hewn::unwrap::project_cylinder(m, faces, frame, lay);
			} else {
				hewn::unwrap::project_sphere(m, faces, frame, lay);
			}
			islands = { faces };
		} else {
			islands = hewn::unwrap::seam_islands(m, faces);
			for (const std::vector<int> &is : islands) {
				if (!hewn::unwrap::lscm(m, is, lay)) {
					hewn::unwrap::planar(m, is, hewn::face_normal(m, m.faces[is[0]]), lay);
				}
			}
			if (p_op != "unwrap_conformal") {
				hewn::unwrap::relax(m, faces, lay, p_op == "unwrap_angle" ? 4 : 60);
			}
		}
		if (p_op != "lightmap_unwrap") {
			hewn::unwrap::row(islands, lay, origin, 0.1f);
		}
	} else if (p_op == "quadrify") {
		// each selected island a grid of quads: its corners at their rows and columns, its place and size kept
		for (const std::vector<int> &is : _islands(n, faces)) {
			hewn::Mesh t = m;
			t.edge_attrs.clear();
			// (opened where its UVs are: the sides whose two faces' UVs part are seams)
			std::unordered_map<uint64_t, std::vector<std::pair<int, int>>> sides;
			std::unordered_set<int> in(is.begin(), is.end());
			for (int f : is) {
				for (int i = 0; i < m.faces[f].size(); i++) {
					sides[hewn::ekey(m.faces[f].v[i], m.faces[f].v[(i + 1) % m.faces[f].size()])].push_back({ f, i });
				}
			}
			for (const auto &kv : sides) {
				if (kv.second.size() != 2) {
					continue;
				}
				const int f0 = kv.second[0].first, i0 = kv.second[0].second, f1 = kv.second[1].first, i1 = kv.second[1].second;
				const int a0 = m.faces[f0].v[i0];
				const Vector2 ua = _loop_uv(n, f0, i0), ub = _loop_uv(n, f0, (i0 + 1) % m.faces[f0].size());
				const int j = m.faces[f1].v[i1] == a0 ? i1 : (i1 + 1) % m.faces[f1].size();
				const int jo = j == i1 ? (i1 + 1) % m.faces[f1].size() : i1;
				if (ua.distance_to(_loop_uv(n, f1, j)) > 1e-5f || ub.distance_to(_loop_uv(n, f1, jo)) > 1e-5f) {
					t.edge_attrs[kv.first].seam = true;
				}
			}
			Vector2 mn0(1e30f, 1e30f), mx0(-1e30f, -1e30f);
			float area0 = 0.0f;
			for (int f : is) {
				std::vector<Vector2> q;
				for (int i = 0; i < m.faces[f].size(); i++) {
					q.push_back(_loop_uv(n, f, i));
					mn0 = mn0.min(q.back());
					mx0 = mx0.max(q.back());
				}
				for (size_t i = 0; i < q.size(); i++) {
					area0 += q[i].cross(q[(i + 1) % q.size()]) * 0.5f;
				}
			}
			std::map<int, std::vector<Vector2>> g;
			if (!hewn::unwrap::quad_grid(t, is, mx0.y - mn0.y > mx0.x - mn0.x, g)) {
				continue;
			}
			Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
			float area = 0.0f;
			for (const auto &kv : g) {
				for (size_t i = 0; i < kv.second.size(); i++) {
					mn = mn.min(kv.second[i]);
					mx = mx.max(kv.second[i]);
					area += kv.second[i].cross(kv.second[(i + 1) % kv.second.size()]) * 0.5f;
				}
			}
			const float k = std::fabs(area) > 1e-12f ? std::sqrt(std::fabs(area0) / std::fabs(area)) : 1.0f;
			const Vector2 c0 = (mn0 + mx0) * 0.5f, c = (mn + mx) * 0.5f;
			for (auto &kv : g) {
				for (Vector2 &p : kv.second) {
					p = c0 + (p - c) * k;
				}
				lay[kv.first] = kv.second;
			}
		}
	} else if (p_op == "relax") {
		lay = current(faces);
		hewn::unwrap::relax(m, faces, lay, 20);
	} else if (p_op == "pack") {
		plugin->end_edit();
		_pack(true);
		return;
	} else if (p_op == "align_h" || p_op == "align_v" || p_op == "distribute") {
		// the selected corners (each place once): onto one line across, or evenly from one end to the other
		std::vector<uint64_t> loops = _selected_loops(n);
		std::vector<Vector2> pts;
		std::vector<int> of;
		for (uint64_t k : loops) {
			const Vector2 p = _loop_uv(n, (int)(k >> 32), (int)(k & 0xffffffffu));
			int at = -1;
			for (size_t i = 0; i < pts.size(); i++) {
				if (pts[i].distance_to(p) < 1e-6f) {
					at = (int)i;
				}
			}
			if (at < 0) {
				at = (int)pts.size();
				pts.push_back(p);
			}
			of.push_back(at);
		}
		std::vector<Vector2> to = pts;
		if (pts.size() >= 2) {
			if (p_op == "distribute") {
				// the two farthest apart: the ends; the rest evenly between them in their order along it
				int ia = 0, ib = 1;
				float best = -1.0f;
				for (size_t i = 0; i < pts.size(); i++) {
					for (size_t j = i + 1; j < pts.size(); j++) {
						if (pts[i].distance_to(pts[j]) > best) {
							best = pts[i].distance_to(pts[j]);
							ia = (int)i;
							ib = (int)j;
						}
					}
				}
				const Vector2 a = pts[ia], d = pts[ib] - pts[ia];
				std::vector<int> order(pts.size());
				std::iota(order.begin(), order.end(), 0);
				std::sort(order.begin(), order.end(), [&](int x, int y) { return (pts[x] - a).dot(d) < (pts[y] - a).dot(d); });
				for (size_t r = 0; r < order.size(); r++) {
					to[order[r]] = a + d * ((float)r / (float)(order.size() - 1));
				}
			} else {
				float avg = 0.0f;
				for (const Vector2 &p : pts) {
					avg += (p_op == "align_h" ? p.y : p.x) / (float)pts.size();
				}
				for (Vector2 &p : to) {
					(p_op == "align_h" ? p.y : p.x) = avg;
				}
			}
		}
		for (size_t i = 0; i < loops.size(); i++) {
			_set_loop_uv(n, (int)(loops[i] >> 32), (int)(loops[i] & 0xffffffffu), to[of[i]]);
		}
	} else if (p_op == "stack_similar") {
		// the selected islands laid onto a similar one that is not selected (else onto the first selected)
		const std::vector<std::vector<int>> is = _islands(n, _shown_faces(n));
		const std::vector<int> grp = _similar_groups(n, is);
		std::map<int, std::vector<int>> by_group;
		for (size_t k = 0; k < is.size(); k++) {
			if (grp[k] >= 0) {
				by_group[grp[k]].push_back((int)k);
			}
		}
		auto pts_of = [&](int k) {
			std::vector<Vector2> p;
			for (int f : is[k]) {
				for (int i = 0; i < m.faces[f].size(); i++) {
					p.push_back(_loop_uv(n, f, i));
				}
			}
			return p;
		};
		auto selected = [&](int k) {
			for (int f : is[k]) {
				if (!_face_selected(n, f)) {
					return false;
				}
			}
			return true;
		};
		for (const auto &g : by_group) {
			std::vector<int> src, dst;
			for (int k : g.second) {
				(selected(k) ? src : dst).push_back(k);
			}
			if (src.empty()) {
				continue;
			}
			if (dst.empty()) {
				dst.push_back(src.front());
				src.erase(src.begin());
			}
			const std::vector<Vector2> tp = pts_of(dst.front());
			Vector2 tc;
			float tr = 0.0f;
			for (const Vector2 &p : tp) {
				tc += p / (float)tp.size();
			}
			for (const Vector2 &p : tp) {
				tr += (p - tc).length() / (float)tp.size();
			}
			for (int k : src) {
				const std::vector<Vector2> sp = pts_of(k);
				Vector2 sc;
				float sr = 0.0f;
				for (const Vector2 &p : sp) {
					sc += p / (float)sp.size();
				}
				for (const Vector2 &p : sp) {
					sr += (p - sc).length() / (float)sp.size();
				}
				const float scale = sr > 1e-9f ? tr / sr : 1.0f;
				// the turn (and mirror) that lays its points closest onto the other's
				float best = 1e30f, best_a = 0.0f;
				bool best_m = false;
				for (int mir = 0; mir < 2; mir++) {
					for (int step = 0; step < 72; step++) {
						const float a = (float)Math::TAU * step / 72.0f;
						float cost = 0.0f;
						for (const Vector2 &p : sp) {
							Vector2 q = (p - sc) * scale;
							if (mir) {
								q.x = -q.x;
							}
							q = q.rotated(a) + tc;
							float d = 1e30f;
							for (const Vector2 &t : tp) {
								d = MIN(d, q.distance_squared_to(t));
							}
							cost += d;
						}
						if (cost < best) {
							best = cost;
							best_a = a;
							best_m = mir == 1;
						}
					}
				}
				for (int f : is[k]) {
					std::vector<Vector2> q;
					for (int i = 0; i < m.faces[f].size(); i++) {
						Vector2 r = (_loop_uv(n, f, i) - sc) * scale;
						if (best_m) {
							r.x = -r.x;
						}
						q.push_back(r.rotated(best_a) + tc);
					}
					lay[f] = q;
				}
			}
		}
	}
	_write_layout(n, lay);
	n->commit(false);
	plugin->end_edit();
	queue_redraw();
}

void HewnUVCanvas::fit_to_cell(const Rect2 &p_cell_uv, const Ref<Material> &p_material) {
	HewnMesh3D *n = _mesh();
	if (!n || !plugin) {
		return;
	}
	const std::vector<int> faces = _op_faces(n);
	if (faces.empty()) {
		return;
	}
	const hewn::Mesh &m = n->get_data();
	Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
	for (int f : faces) {
		for (int i = 0; i < m.faces[f].size(); i++) {
			mn = mn.min(_loop_uv(n, f, i));
			mx = mx.max(_loop_uv(n, f, i));
		}
	}
	const Vector2 sz = (mx - mn).max(Vector2(1e-9f, 1e-9f));
	Vector2 k(p_cell_uv.size.x / sz.x, p_cell_uv.size.y / sz.y);
	if (fit_mode == 1) {
		k.y = fit_uniform ? k.x : k.y; // (its width; its height in proportion, or the cell's too when stretched)
	} else if (fit_mode == 2) {
		k.x = fit_uniform ? k.y : k.x;
	} else if (fit_uniform) {
		k = Vector2(MIN(k.x, k.y), MIN(k.x, k.y));
	}
	const Vector2 c0 = (mn + mx) * 0.5f, c = p_cell_uv.get_center();
	plugin->begin_edit("UV Fit to Hotspot Cell");
	for (int f : faces) {
		for (int i = 0; i < m.faces[f].size(); i++) {
			_set_loop_uv(n, f, i, c + (_loop_uv(n, f, i) - c0) * k);
		}
	}
	if (p_material.is_valid() && channel == 0) {
		PackedInt32Array pf;
		for (int f : faces) {
			pf.push_back(f);
		}
		n->apply_material(p_material, pf);
	}
	n->commit(false);
	plugin->end_edit();
	queue_redraw();
}

void HewnUVCanvas::_on_stretch_opacity(double p_value) {
	stretch_opacity = (float)p_value;
	queue_redraw();
}

void HewnUVCanvas::_on_density_check(bool p_on) {
	pack_density_on = p_on;
}

void HewnUVCanvas::_on_density_value(double p_value) {
	pack_density = MAX(1.0f, (float)p_value);
}
