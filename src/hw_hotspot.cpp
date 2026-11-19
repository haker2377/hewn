#include "hw_hotspot.h"

#include <godot_cpp/variant/dictionary.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

using namespace godot;

namespace hewn {

std::vector<HotspotRect> hotspot_rects(const Ref<Material> &p_material) {
	std::vector<HotspotRect> out;
	if (p_material.is_null() || !p_material->has_meta("hewn_hotspots")) {
		return out;
	}
	const Array a = p_material->get_meta("hewn_hotspots");
	for (int64_t i = 0; i < a.size(); i++) {
		const Dictionary d = a[i];
		HotspotRect h;
		h.r = Rect2(d.get("rect", Rect2())).abs();
		h.tile = (int)d.get("tile", 0);
		h.orient = (int)d.get("orient", 0);
		if (h.r.size.x > 1e-6f && h.r.size.y > 1e-6f) {
			out.push_back(h);
		}
	}
	return out;
}

Array hotspot_rects_to_array(const std::vector<HotspotRect> &p_rects) {
	Array a;
	for (const HotspotRect &h : p_rects) {
		Dictionary d;
		d["rect"] = h.r;
		d["tile"] = h.tile;
		d["orient"] = h.orient;
		a.push_back(d);
	}
	return a;
}

namespace {

int find_root(std::vector<int> &p, int x) {
	while (p[x] != x) {
		p[x] = p[p[x]];
		x = p[x];
	}
	return x;
}

// The rectangle for a piece of `w` x `h` (meters, or relative to the largest piece) facing `kind` (1 wall, 2 floor,
// 3 ceiling) on `side` (0 unknown, 1 outside, 2 inside, 3 an opening's): its index, turned or not. The rectangles for
// it: of its kind or of any kind, of its side or of any side - all of them alike, by how they fit (none at all: one of
// another side, then any); off ones, other atlases' (p_atlas >= 0: the piece held to one) and ones for other sizes
// (p_meters: its longer side) not. Among the ones that fit about as well (the spread), one by chance (their weights).
int pick(const std::vector<HotspotRect> &p_rects, float w, float h, int kind, int side, int p_atlas, float p_meters, const HotspotOptions &o,
		const std::vector<Vector2> &p_sizes, uint32_t p_seed, bool &r_turned, bool p_round) {
	r_turned = false;
	w = std::max(1e-4f, w);
	h = std::max(1e-4f, h);
	const float k = std::min(1.0f, std::max(0.0f, o.weight));
	auto usable_any = [&](const HotspotRect &R) {
		if (!R.enabled || R.r.size.x < 1e-6f || R.r.size.y < 1e-6f || (p_atlas >= 0 && R.atlas != p_atlas)) {
			return false;
		}
		if ((R.min_size > 0.0f && p_meters < R.min_size) || (R.max_size > 0.0f && p_meters > R.max_size)) {
			return false;
		}
		return true;
	};
	// (round cells: a round piece gets one when there is one; any other piece never)
	bool round_cells = false;
	for (const HotspotRect &R : p_rects) {
		round_cells = round_cells || (R.radial && usable_any(R));
	}
	const bool want_round = p_round && round_cells;
	auto usable = [&](const HotspotRect &R) { return usable_any(R) && R.radial == want_round; };
	// the first of the levels with any rectangle
	int level = -1;
	auto level_of = [&](const HotspotRect &R) {
		// (the kind and the side are what a cell is for, not an order: one for any face is as good as one for floors)
		const bool s_ok = R.side == 0 || (side != 0 && R.side == side);
		const bool k_ok = R.orient == 0 || R.orient == kind;
		if (s_ok && k_ok) {
			return 0;
		}
		return k_ok ? 1 : 2; // (none for its side at all: one of another side, rather than nothing)
	};
	for (const HotspotRect &R : p_rects) {
		if (usable(R) && level_of(R) < 99) {
			level = level < 0 ? level_of(R) : std::min(level, level_of(R));
		}
	}
	if (level < 0) {
		return -1;
	}
	struct Cand {
		int cell;
		bool turned;
		float score;
	};
	std::vector<Cand> cands;
	float best = 1e30f;
	for (int c = 0; c < (int)p_rects.size(); c++) {
		const HotspotRect &R = p_rects[c];
		if (!usable(R) || level_of(R) != level) {
			continue;
		}
		const float ks = 1.0f / std::max(1e-3f, R.scale);
		const float cw = std::max(1e-6f, p_sizes[c].x * ks), ch = std::max(1e-6f, p_sizes[c].y * ks);
		// (upwards: kept upright unless it may turn; the others at any quarter turn unless they may not)
		const bool may_turn = R.turn == 2 ? true : R.turn == 1 ? false : (o.turn && R.orient != 1);
		for (int turn = 0; turn < (may_turn ? 2 : 1); turn++) {
			const float fw = turn ? h : w, fh = turn ? w : h;
			float shape, size;
			if (R.tile == 1) {
				shape = 0.0f; // (repeats along U: only its height matters)
				size = std::fabs(std::log(fh / ch));
			} else if (R.tile == 2) {
				shape = 0.0f;
				size = std::fabs(std::log(fw / cw));
			} else {
				shape = std::fabs(std::log((fw / fh) / (cw / ch)));
				size = std::fabs(std::log((fw * fh) / (cw * ch))) * 0.5f;
			}
			// (a turned rectangle a little worse: upright ones first when they fit as well)
			const float score = shape * (1.0f - k) + size * k + (turn ? 0.02f : 0.0f);
			cands.push_back({ c, turn == 1, score });
			best = std::min(best, score);
		}
	}
	std::vector<Cand> top;
	float total = 0.0f;
	for (const Cand &c : cands) {
		if (c.score <= best + 0.02f + std::max(0.0f, o.spread) * 1.5f && p_rects[c.cell].weight > 0.0f) {
			top.push_back(c);
			total += p_rects[c.cell].weight;
		}
	}
	if (top.empty()) {
		return -1;
	}
	std::sort(top.begin(), top.end(), [](const Cand &a, const Cand &b) { return a.score < b.score || (a.score == b.score && a.cell < b.cell); });
	// (by chance, the heavier ones more often; another variant: the next)
	const uint32_t r = (p_seed + (uint32_t)std::max(0, o.variant) * 2654435761u) * 2246822519u;
	float at = (float)((r >> 8) & 0xffff) / 65536.0f * total;
	size_t pick_i = 0;
	for (size_t i = 0; i < top.size(); i++) {
		at -= p_rects[top[i].cell].weight;
		if (at < 0.0f) {
			pick_i = i;
			break;
		}
		pick_i = i;
	}
	r_turned = top[pick_i].turned;
	return top[pick_i].cell;
}

struct Piece {
	std::vector<int> faces;
	Vector3 u_ax, v_ax;
	Vector2 mn;
	float w = 0.0f, h = 0.0f;
	int kind = 1;
	int side = 0;
	int lock = -1;
	uint32_t seed = 0;
	bool round = false; // (its outline a circle / an oval: for the round cells)
	bool strip = false; // (a strip of quads: laid out along it, not projected)
	std::vector<std::vector<Vector2>> flat; // (a strip's: each face's corners laid out, meters; as P.faces)
};

} // namespace

int apply_hotspots(Mesh &m, const std::vector<int> &p_faces, const std::function<const std::vector<HotspotRect> *(int)> &p_rects_of,
		const HotspotOptions &o, const Transform3D &p_xf) {
	std::vector<int> faces;
	for (int f : p_faces) {
		if (f >= 0 && f < (int)m.faces.size() && m.faces[f].size() >= 3 && !(o.skip_fixed && m.faces[f].hs_fixed)) {
			faces.push_back(f);
		}
	}
	if (faces.empty()) {
		return 0;
	}
	const bool atlases = !o.atlas_slots.empty();
	const Basis nb = p_xf.basis.inverse().transposed();
	std::vector<Vector3> wn(m.faces.size()); // (world normals)
	for (int f : faces) {
		wn[f] = nb.xform(face_normal(m, m.faces[f])).normalized();
	}
	// the object's middle (in the world): which way is out for a wall without a role
	Vector3 mid;
	{
		Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
		for (const Vector3 &p : m.pos) {
			const Vector3 w = p_xf.xform(p);
			mn = mn.min(w);
			mx = mx.max(w);
		}
		mid = (mn + mx) * 0.5f;
	}
	// the pieces: faces joined over an edge (not a seam), of one material (one atlas held), bent less than the angle
	std::vector<int> parent(m.faces.size());
	std::iota(parent.begin(), parent.end(), 0);
	if (o.islands) {
		const float cos_lim = std::cos(o.angle * 3.14159265f / 180.0f);
		std::unordered_map<uint64_t, int> first;
		for (int f : faces) {
			const Face &F = m.faces[f];
			for (int i = 0; i < F.size(); i++) {
				const uint64_t k = ekey(F.v[i], F.v[(i + 1) % F.size()]);
				if (o.seams) {
					auto ea = m.edge_attrs.find(k);
					if (ea != m.edge_attrs.end() && ea->second.seam) {
						continue;
					}
				}
				auto it = first.find(k);
				if (it == first.end()) {
					first[k] = f;
				} else {
					const int g = it->second;
					const bool same = atlases ? m.faces[g].hs_lock == F.hs_lock && m.faces[g].role == F.role : m.faces[g].material == F.material;
					if (same && wn[g].dot(wn[f]) >= cos_lim) {
						parent[find_root(parent, f)] = find_root(parent, g);
					}
				}
			}
		}
	}
	std::unordered_map<int, std::vector<int>> by_root;
	for (int f : faces) {
		by_root[find_root(parent, f)].push_back(f);
	}
	std::vector<Piece> pieces;
	for (auto &kv : by_root) {
		Piece P;
		P.faces = kv.second;
		std::sort(P.faces.begin(), P.faces.end());
		// (the same piece the same look however it was found: seeded by its first face and the object's seed)
		P.seed = (uint32_t)P.faces[0] * 2654435761u + o.seed * 40503u;
		P.lock = atlases ? m.faces[P.faces[0]].hs_lock : -1;
		// its plane: the area-weighted normal, world up for the walls, the longest outer edge for floors
		Vector3 n;
		Vector3 c;
		int roles[4] = { 0, 0, 0, 0 };
		for (int f : P.faces) {
			n += nb.xform(face_normal_raw(m, m.faces[f]));
			c += p_xf.xform(face_center(m, m.faces[f]));
			const int r = m.faces[f].role;
			roles[r == ROLE_OUTER ? 1 : r == ROLE_INNER ? 2 : r == ROLE_OPENING ? 3 : 0]++;
		}
		c /= (float)P.faces.size();
		float area = 0.0f;
		float kinds[4] = { 0, 0, 0, 0 };
		for (int f : P.faces) {
			const Vector3 fn = nb.xform(face_normal_raw(m, m.faces[f]));
			area += fn.length();
			const Vector3 un = fn.length() > 1e-12f ? fn.normalized() : Vector3(0, 1, 0);
			kinds[un.y > 0.7f ? 2 : un.y < -0.7f ? 3 : 1] += fn.length();
		}
		const bool bent = n.length() < area * 0.5f; // (a ring of faces: its normals cancel out)
		n = n.length() > 1e-9f ? n.normalized() : Vector3(0, 1, 0);
		P.kind = n.y > 0.7f ? 2 : n.y < -0.7f ? 3 : 1;
		if (bent) {
			P.kind = kinds[1] >= kinds[2] && kinds[1] >= kinds[3] ? 1 : kinds[2] >= kinds[3] ? 2 : 3; // (most of its faces')
		}
		// which side: the faces' roles; a wall without one: out of the object's middle or into it
		for (int s = 1; s <= 3; s++) {
			if (roles[s] > roles[P.side] && roles[s] * 2 >= (int)P.faces.size()) {
				P.side = s;
			}
		}
		if (P.side == 0 && P.kind == 1) {
			const Vector3 hn(n.x, 0, n.z), away(c.x - mid.x, 0, c.z - mid.z);
			if (hn.length() > 1e-4f && away.length() > 1e-4f) {
				P.side = hn.dot(away) > 0.0f ? 1 : 2;
			}
		}
		Vector3 v_ax;
		if (P.kind == 1) {
			v_ax = Vector3(0, 1, 0) - n * n.y;
		} else {
			std::unordered_map<uint64_t, int> cnt;
			for (int f : P.faces) {
				const Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					cnt[ekey(F.v[i], F.v[(i + 1) % F.size()])]++;
				}
			}
			float best = -1.0f;
			for (int f : P.faces) {
				const Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					const int a = F.v[i], b = F.v[(i + 1) % F.size()];
					if (cnt[ekey(a, b)] != 1) {
						continue;
					}
					const Vector3 e = p_xf.basis.xform(m.pos[b] - m.pos[a]);
					if (e.length() > best) {
						best = e.length();
						v_ax = e.cross(n); // (across the longest edge: the rectangle's length along it)
					}
				}
			}
		}
		P.v_ax = v_ax.length() > 1e-6f ? v_ax.normalized() : (std::fabs(n.x) < 0.9f ? Vector3(1, 0, 0) : Vector3(0, 0, 1)).cross(n).normalized();
		P.u_ax = P.v_ax.cross(n).normalized();
		Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
		for (int f : P.faces) {
			for (int v : m.faces[f].v) {
				const Vector3 w = p_xf.xform(m.pos[v]);
				const Vector2 q(P.u_ax.dot(w), -P.v_ax.dot(w)); // (V down the texture as it goes down the wall)
				mn = Vector2(std::min(mn.x, q.x), std::min(mn.y, q.y));
				mx = Vector2(std::max(mx.x, q.x), std::max(mx.y, q.y));
			}
		}
		P.mn = mn;
		P.w = mx.x - mn.x;
		P.h = mx.y - mn.y;
		{
			// round: its outline's points (on one edge of the piece only) all on the oval in its rectangle
			std::unordered_map<uint64_t, int> cnt;
			for (int f : P.faces) {
				const Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					cnt[ekey(F.v[i], F.v[(i + 1) % F.size()])]++;
				}
			}
			std::unordered_map<int, char> outline;
			for (int f : P.faces) {
				const Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					if (cnt[ekey(F.v[i], F.v[(i + 1) % F.size()])] == 1) {
						outline[F.v[i]] = 1;
					}
				}
			}
			bool round = outline.size() >= 8 && P.w > 1e-5f && P.h > 1e-5f;
			const Vector2 cen = mn + Vector2(P.w, P.h) * 0.5f;
			for (const auto &kv : outline) {
				if (!round) {
					break;
				}
				const Vector3 w = p_xf.xform(m.pos[kv.first]);
				const Vector2 q((P.u_ax.dot(w) - cen.x) / (P.w * 0.5f), (-P.v_ax.dot(w) - cen.y) / (P.h * 0.5f));
				round = std::fabs(q.length() - 1.0f) < 0.08f;
			}
			P.round = round;
		}
		if (P.faces.size() >= 2) {
			// a strip: all quads, each joined to at most two others of it, by opposite sides
			std::unordered_map<uint64_t, std::vector<std::pair<int, int>>> at_edge; // (face index in P.faces, its side)
			bool quads = true;
			for (int k = 0; k < (int)P.faces.size() && quads; k++) {
				const Face &F = m.faces[P.faces[k]];
				quads = F.size() == 4;
				for (int i = 0; i < F.size() && quads; i++) {
					at_edge[ekey(F.v[i], F.v[(i + 1) % 4])].push_back({ k, i });
				}
			}
			std::vector<std::vector<int>> joined(P.faces.size()); // (the sides joined to another face)
			for (const auto &kv : at_edge) {
				if (kv.second.size() == 2) {
					joined[kv.second[0].first].push_back(kv.second[0].second);
					joined[kv.second[1].first].push_back(kv.second[1].second);
				}
			}
			bool strip = quads;
			for (const std::vector<int> &js : joined) {
				strip = strip && !js.empty() && js.size() <= 2 && (js.size() < 2 || (js[0] + 2) % 4 == js[1]);
			}
			if (strip) {
				std::vector<std::vector<Vector2>> keep_uv;
				std::vector<char> keep_manual;
				for (int f : P.faces) {
					keep_uv.push_back(m.faces[f].uv);
					keep_manual.push_back(m.faces[f].manual_uv ? 1 : 0);
				}
				const std::vector<int> laid = unwrap_quads(m, P.faces, P.faces[0]);
				if (laid.size() == P.faces.size()) {
					// along the strip: U (the sides joining the faces go across it: V); a wall's V down as it goes down
					const float sc = (p_xf.basis.get_scale().x + p_xf.basis.get_scale().y + p_xf.basis.get_scale().z) / 3.0f;
					int k0 = 0;
					while (k0 < (int)P.faces.size() && joined[k0].empty()) {
						k0++;
					}
					const Face &F0 = m.faces[P.faces[k0]];
					const int e = joined[k0][0];
					const Vector2 across = F0.uv[(e + 1) % 4] - F0.uv[e];
					const bool turn = std::fabs(across.x) > std::fabs(across.y);
					const Vector3 wa = p_xf.xform(m.pos[F0.v[e]]), wb = p_xf.xform(m.pos[F0.v[(e + 1) % 4]]);
					const float dv = turn ? -across.x : across.y; // (V along the joining side after the turn, before any flip)
					const bool flip_v = P.kind == 1 && std::fabs(wb.y - wa.y) > 1e-5f && ((wb.y - wa.y) > 0.0f) == (dv > 0.0f);
					P.flat.clear();
					Vector2 fmn(1e30f, 1e30f), fmx(-1e30f, -1e30f);
					for (int f : P.faces) {
						std::vector<Vector2> q;
						for (const Vector2 &u : m.faces[f].uv) {
							Vector2 r = turn ? Vector2(u.y, -u.x) : u; // (turned, not mirrored)
							if (flip_v) {
								r = -r; // (a half turn: V down a wall, still not mirrored)
							}
							r *= sc;
							q.push_back(r);
							fmn = fmn.min(r);
							fmx = fmx.max(r);
						}
						P.flat.push_back(q);
					}
					P.strip = true;
					P.mn = fmn;
					P.w = fmx.x - fmn.x;
					P.h = fmx.y - fmn.y;
					P.round = false;
				} else {
					for (int k = 0; k < (int)P.faces.size(); k++) {
						m.faces[P.faces[k]].uv = keep_uv[k];
						m.faces[P.faces[k]].manual_uv = keep_manual[k] != 0;
					}
				}
			}
		}
		if (!P.strip && !P.round) {
			// bent round a middle: its outline's points on two circles with one middle (a ring's piece, a curved floor,
			// one face or many), at least a little of the arc: laid out round it, U along the arc, V across
			std::unordered_map<uint64_t, int> cnt;
			for (int f : P.faces) {
				const Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					cnt[ekey(F.v[i], F.v[(i + 1) % F.size()])]++;
				}
			}
			// the outline in its order (each boundary side once, as the faces go round)
			std::unordered_map<int, int> next_v;
			for (int f : P.faces) {
				const Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					if (cnt[ekey(F.v[i], F.v[(i + 1) % F.size()])] == 1) {
						next_v[F.v[i]] = F.v[(i + 1) % F.size()];
					}
				}
			}
			std::vector<Vector2> q;
			if (!next_v.empty()) {
				const int start = next_v.begin()->first;
				int v = start;
				for (size_t guard = 0; guard <= next_v.size(); guard++) {
					const Vector3 w = p_xf.xform(m.pos[v]);
					q.push_back(Vector2(P.u_ax.dot(w), -P.v_ax.dot(w)));
					auto it = next_v.find(v);
					if (it == next_v.end() || it->second == start) {
						break;
					}
					v = it->second;
				}
				if (q.size() != next_v.size()) {
					q.clear(); // (more than one loop round it: a hole in it - not this)
				}
			}
			// the middle: where the circles through each three points after another meet (the arcs' all the same one)
			std::vector<float> cxs, cys;
			for (size_t i = 0; q.size() >= 6 && i < q.size(); i++) {
				const Vector2 a = q[i], b = q[(i + 1) % q.size()], d = q[(i + 2) % q.size()];
				const float den = 2.0f * (a.x * (b.y - d.y) + b.x * (d.y - a.y) + d.x * (a.y - b.y));
				if (std::fabs(den) < 1e-6f * std::max(1e-6f, (b - a).length() * (d - b).length())) {
					continue; // (on a straight side)
				}
				const float a2 = a.length_squared(), b2 = b.length_squared(), d2 = d.length_squared();
				cxs.push_back((a2 * (b.y - d.y) + b2 * (d.y - a.y) + d2 * (a.y - b.y)) / den);
				cys.push_back((a2 * (d.x - b.x) + b2 * (a.x - d.x) + d2 * (b.x - a.x)) / den);
			}
			if (cxs.size() >= 4) {
				std::nth_element(cxs.begin(), cxs.begin() + cxs.size() / 2, cxs.end());
				std::nth_element(cys.begin(), cys.begin() + cys.size() / 2, cys.end());
				const Vector2 c(cxs[cxs.size() / 2], cys[cys.size() / 2]);
				float r2 = 0.0f, r1 = 1e30f;
				for (const Vector2 &p : q) {
					r2 = std::max(r2, p.distance_to(c));
					r1 = std::min(r1, p.distance_to(c));
				}
				const float tol = 0.02f * r2 + 1e-4f;
				bool on = r2 - r1 > 3.0f * tol && r1 > 0.05f * r2 && r2 < 1e4f;
				for (const Vector2 &p : q) {
					const float d = p.distance_to(c);
					on = on && (std::fabs(d - r1) < tol || std::fabs(d - r2) < tol);
				}
				// (how far round: angles from the piece's middle's)
				Vector2 mid;
				for (const Vector2 &p : q) {
					mid += p / (float)q.size();
				}
				const float a0 = (mid - c).angle();
				auto ang = [&](const Vector2 &p) {
					float t = (p - c).angle() - a0;
					while (t > 3.14159265f) {
						t -= 2.0f * 3.14159265f;
					}
					while (t <= -3.14159265f) {
						t += 2.0f * 3.14159265f;
					}
					return t;
				};
				float tmin = 1e30f, tmax = -1e30f;
				for (const Vector2 &p : q) {
					tmin = std::min(tmin, ang(p));
					tmax = std::max(tmax, ang(p));
				}
				on = on && tmax - tmin > 0.3f && tmax - tmin < 2.0f * 3.14159265f - 0.2f;
				if (on) {
					// U along the arc (at the middle radius; turned the way that keeps it unmirrored), V out from the middle
					const float rm = (r1 + r2) * 0.5f;
					P.flat.clear();
					Vector2 fmn(1e30f, 1e30f), fmx(-1e30f, -1e30f);
					for (int f : P.faces) {
						std::vector<Vector2> fl;
						for (int v : m.faces[f].v) {
							const Vector3 w = p_xf.xform(m.pos[v]);
							const Vector2 p(P.u_ax.dot(w), -P.v_ax.dot(w));
							const Vector2 r(-ang(p) * rm, p.distance_to(c) - r1);
							fl.push_back(r);
							fmn = fmn.min(r);
							fmx = fmx.max(r);
						}
						P.flat.push_back(fl);
					}
					P.strip = true;
					P.mn = fmn;
					P.w = fmx.x - fmn.x;
					P.h = fmx.y - fmn.y;
				}
			}
		}
		pieces.push_back(P);
	}
	// sizes: in meters (the texture's size in the world), or each piece against the largest one and each rectangle
	// against the largest rectangle (the same look at any scale)
	float largest_piece = 1e-6f;
	for (const Piece &P : pieces) {
		largest_piece = std::max(largest_piece, std::sqrt(std::max(0.0f, P.w * P.h)));
	}
	// Around the corners (Combine Faces): walls of one height side by side (one's right side the next one's left, an
	// upright edge between them) one run; a strip repeating along U picked for the run's whole length goes on round
	// the corners, each piece from where the one before it ended.
	std::vector<int> run_of(pieces.size(), -1);
	std::vector<float> run_at(pieces.size(), 0.0f); // (meters along the run where the piece starts)
	std::vector<std::vector<int>> runs;
	if (o.islands && pieces.size() > 1) {
		std::vector<int> piece_of(m.faces.size(), -1);
		for (int i = 0; i < (int)pieces.size(); i++) {
			for (int f : pieces[i].faces) {
				piece_of[f] = i;
			}
		}
		std::vector<int> right(pieces.size(), -1), left(pieces.size(), -1);
		std::unordered_map<uint64_t, int> seen;
		const Vector3 up(0, 1, 0);
		for (int i = 0; i < (int)pieces.size(); i++) {
			for (int f : pieces[i].faces) {
				const Face &F = m.faces[f];
				for (int k = 0; k < F.size(); k++) {
					const int a = F.v[k], b = F.v[(k + 1) % F.size()];
					const uint64_t key = ekey(a, b);
					auto it = seen.find(key);
					if (it == seen.end()) {
						seen[key] = i;
						continue;
					}
					const int j = it->second;
					if (j == i || pieces[i].kind != 1 || pieces[j].kind != 1) {
						continue;
					}
					const Piece &A = pieces[i], &B = pieces[j];
					const Face &G = m.faces[B.faces[0]];
					const bool same = atlases ? F.hs_lock == G.hs_lock && A.side == B.side : F.material == G.material; // (a box's walls: roles of their own)
					if (!same || std::fabs(A.mn.y - B.mn.y) > 0.01f || std::fabs(A.h - B.h) > 0.01f) {
						continue;
					}
					const Vector3 wa = p_xf.xform(m.pos[a]), wb = p_xf.xform(m.pos[b]);
					if ((wb - wa).length() < 1e-5f || std::fabs((wb - wa).normalized().dot(up)) < 0.95f) {
						continue; // (only an upright edge: the corner of two walls)
					}
					const Vector3 mid = (wa + wb) * 0.5f;
					const float ua = A.u_ax.dot(mid), ub = B.u_ax.dot(mid);
					const bool a_right = std::fabs(ua - (A.mn.x + A.w)) < 1e-3f, a_left = std::fabs(ua - A.mn.x) < 1e-3f;
					const bool b_right = std::fabs(ub - (B.mn.x + B.w)) < 1e-3f, b_left = std::fabs(ub - B.mn.x) < 1e-3f;
					if (a_right && b_left && right[i] < 0 && left[j] < 0) {
						right[i] = j;
						left[j] = i;
					} else if (b_right && a_left && right[j] < 0 && left[i] < 0) {
						right[j] = i;
						left[i] = j;
					}
				}
			}
		}
		std::vector<char> used(pieces.size(), 0);
		auto follow = [&](int p_start) {
			std::vector<int> r;
			for (int i = p_start; i >= 0 && !used[i]; i = right[i]) {
				used[i] = 1;
				r.push_back(i);
			}
			if (r.size() >= 2) {
				runs.push_back(r);
			}
		};
		for (int i = 0; i < (int)pieces.size(); i++) {
			if (left[i] < 0 && right[i] >= 0) {
				follow(i); // (from a run's start)
			}
		}
		for (int i = 0; i < (int)pieces.size(); i++) {
			if (!used[i] && right[i] >= 0) {
				follow(i); // (round and back: a ring of walls)
			}
		}
		for (int r = 0; r < (int)runs.size(); r++) {
			float at = 0.0f;
			for (int i : runs[r]) {
				run_of[i] = r;
				run_at[i] = at;
				at += pieces[i].w;
			}
		}
	}
	std::vector<int> run_cell(runs.size(), -2); // (-2: not picked yet; -1: none goes round, each piece its own)
	int done = 0;
	for (int pi = 0; pi < (int)pieces.size(); pi++) {
		Piece &P = pieces[pi];
		const std::vector<HotspotRect> *rects = p_rects_of(m.faces[P.faces[0]].material);
		if (!rects || rects->empty()) {
			continue;
		}
		// each rectangle's size: in meters (its atlas', or the texture size given), or against the largest one
		const float asp = std::max(1e-4f, o.aspect);
		std::vector<Vector2> sizes(rects->size());
		float largest_cell = 1e-6f;
		for (size_t i = 0; i < rects->size(); i++) {
			const HotspotRect &R = (*rects)[i];
			sizes[i] = R.meters.x > 0.0f ? R.meters : Vector2(R.r.size.x * std::max(1e-4f, o.texture_size), R.r.size.y * std::max(1e-4f, o.texture_size) / asp);
			largest_cell = std::max(largest_cell, std::sqrt(sizes[i].x * sizes[i].y));
		}
		float pw = P.w, ph = P.h;
		if (!o.world_size) {
			for (Vector2 &q : sizes) {
				q /= largest_cell;
			}
			pw /= largest_piece;
			ph /= largest_piece;
		}
		bool turned = false;
		int c = -1;
		if (o.force_cell >= 0 && o.force_cell < (int)rects->size()) {
			c = o.force_cell;
			// (chosen by hand: turned when that fits its shape better, an upright one never)
			const HotspotRect &R = (*rects)[c];
			const bool may_turn = R.turn == 2 ? true : R.turn == 1 ? false : (o.turn && R.orient != 1);
			if (may_turn && R.tile == 0) {
				const float cr = sizes[c].x / std::max(1e-6f, sizes[c].y);
				const float a0 = std::fabs(std::log((pw / std::max(1e-6f, ph)) / cr));
				const float a1 = std::fabs(std::log((ph / std::max(1e-6f, pw)) / cr));
				turned = a1 + 0.02f < a0;
			}
		} else {
			c = pick(*rects, pw, ph, P.kind, P.side, P.lock, std::max(P.w, P.h), o, sizes, P.seed, turned, P.round);
		}
		// a run round corners: the cell picked for its whole length (by its first piece), when that is a strip repeating
		// along it
		const int run = run_of[pi];
		float run_from = -1.0f;
		if (run >= 0) {
			if (run_cell[run] == -2) {
				const Piece &P0 = pieces[runs[run][0]];
				float total = 0.0f;
				for (int i : runs[run]) {
					total += pieces[i].w;
				}
				bool t_run = false;
				int rc = o.force_cell >= 0 && o.force_cell < (int)rects->size() ? o.force_cell :
						pick(*rects, o.world_size ? total : total / largest_piece, o.world_size ? P0.h : P0.h / largest_piece, P0.kind, P0.side, P0.lock, std::max(total, P0.h), o, sizes, P0.seed, t_run, false);
				run_cell[run] = rc >= 0 && (*rects)[rc].tile == 1 && !t_run ? rc : -1;
			}
			if (run_cell[run] >= 0) {
				c = run_cell[run];
				turned = false;
				run_from = run_at[pi];
			}
		}
		if (c < 0) {
			continue;
		}
		const HotspotRect &R = (*rects)[c];
		// (kept off the cell's edges by its padding)
		Rect2 rr = R.r;
		const Vector2 pad = R.padding.min(rr.size * 0.45f);
		rr.position += pad;
		rr.size -= pad * 2.0f;
		const bool mirrored = R.mirror && ((P.seed >> 13) & 1u) && run_from < 0.0f; // (a run round corners: never)
		const float w = P.w, h = P.h;
		for (int fk = 0; fk < (int)P.faces.size(); fk++) {
			const int f = P.faces[fk];
			Face &F = m.faces[f];
			F.ensure_attrs();
			for (int i = 0; i < F.size(); i++) {
				const Vector3 wp = p_xf.xform(m.pos[F.v[i]]);
				// (a strip: where it was laid out along itself; else projected on the piece's plane)
				const Vector2 at = P.strip ? P.flat[fk][i] : Vector2(P.u_ax.dot(wp), -P.v_ax.dot(wp));
				Vector2 s((at.x - P.mn.x) / std::max(1e-6f, w), (at.y - P.mn.y) / std::max(1e-6f, h));
				float lu = w, lv = h; // (meters along the rectangle's U / V)
				if (turned) {
					s = Vector2(s.y, 1.0f - s.x);
					std::swap(lu, lv);
				}
				if (mirrored) {
					s.x = 1.0f - s.x;
				}
				Vector2 uv = rr.position + s * rr.size;
				if (R.tile == 1) {
					// repeats along U: the height fills the strip, the length goes on at the same scale
					const float k = rr.size.y / std::max(1e-6f, lv);
					uv.x = rr.position.x + (s.x * lu + std::max(0.0f, run_from)) * k; // (in a run: from where the last piece ended)
				} else if (R.tile == 2) {
					const float k = rr.size.x / std::max(1e-6f, lu);
					uv.y = rr.position.y + s.y * lv * k;
				}
				F.uv[i] = uv;
			}
			F.manual_uv = true;
			F.hs_cell = atlases ? R.local : c;
			F.hs_atlas = atlases ? R.atlas : 0;
			if (atlases && R.atlas >= 0 && R.atlas < (int)o.atlas_slots.size() && o.atlas_slots[R.atlas] >= 0) {
				F.material = o.atlas_slots[R.atlas];
			}
			if (o.force_cell >= 0) {
				F.hs_fixed = true;
			}
			done++;
		}
	}
	return done;
}

} // namespace hewn
