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

// The rectangle for a piece of `w` x `h` meters facing `kind` (1 wall, 2 floor, 3 ceiling): its index, turned or not.
int pick(const std::vector<HotspotRect> &p_rects, float w, float h, int kind, const HotspotOptions &o, uint32_t p_seed, bool &r_turned) {
	r_turned = false;
	w = std::max(1e-4f, w);
	h = std::max(1e-4f, h);
	const float k = std::min(1.0f, std::max(0.0f, o.weight));
	const float tm = std::max(1e-4f, o.texture_size);
	struct Cand {
		int cell;
		bool turned;
		float score;
	};
	std::vector<Cand> cands;
	float best = 1e30f;
	for (int c = 0; c < (int)p_rects.size(); c++) {
		const HotspotRect &R = p_rects[c];
		if (R.orient != 0 && R.orient != kind) {
			continue;
		}
		const float cw = std::max(1e-4f, R.r.size.x * tm), ch = std::max(1e-4f, R.r.size.y * tm);
		for (int turn = 0; turn < (o.turn ? 2 : 1); turn++) {
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
	for (const Cand &c : cands) {
		if (c.score <= best + 0.05f) {
			top.push_back(c);
		}
	}
	if (top.empty()) {
		return -1;
	}
	std::sort(top.begin(), top.end(), [](const Cand &a, const Cand &b) { return a.score < b.score || (a.score == b.score && a.cell < b.cell); });
	const Cand &c = top[(p_seed + (uint32_t)std::max(0, o.variant)) % top.size()];
	r_turned = c.turned;
	return c.cell;
}

} // namespace

int apply_hotspots(Mesh &m, const std::vector<int> &p_faces, const std::function<const std::vector<HotspotRect> *(int)> &p_rects_of,
		const HotspotOptions &o, const Transform3D &p_xf) {
	std::vector<int> faces;
	for (int f : p_faces) {
		if (f >= 0 && f < (int)m.faces.size() && m.faces[f].size() >= 3) {
			faces.push_back(f);
		}
	}
	if (faces.empty()) {
		return 0;
	}
	const Basis nb = p_xf.basis.inverse().transposed();
	std::vector<Vector3> wn(m.faces.size()); // (world normals)
	for (int f : faces) {
		wn[f] = nb.xform(face_normal(m, m.faces[f])).normalized();
	}
	// the pieces: faces joined over an edge, of one material, bent less than the angle
	std::vector<int> parent(m.faces.size());
	std::iota(parent.begin(), parent.end(), 0);
	if (o.islands) {
		std::vector<char> in(m.faces.size(), 0);
		for (int f : faces) {
			in[f] = 1;
		}
		const float cos_lim = std::cos(o.angle * 3.14159265f / 180.0f);
		std::unordered_map<uint64_t, int> first;
		for (int f : faces) {
			const Face &F = m.faces[f];
			for (int i = 0; i < F.size(); i++) {
				const uint64_t k = ekey(F.v[i], F.v[(i + 1) % F.size()]);
				auto it = first.find(k);
				if (it == first.end()) {
					first[k] = f;
				} else {
					const int g = it->second;
					if (m.faces[g].material == F.material && wn[g].dot(wn[f]) >= cos_lim) {
						parent[find_root(parent, f)] = find_root(parent, g);
					}
				}
			}
		}
	}
	std::unordered_map<int, std::vector<int>> pieces;
	for (int f : faces) {
		pieces[find_root(parent, f)].push_back(f);
	}
	int done = 0;
	for (auto &kv : pieces) {
		const std::vector<int> &pf = kv.second;
		const std::vector<HotspotRect> *rects = p_rects_of(m.faces[pf[0]].material);
		if (!rects || rects->empty()) {
			continue;
		}
		// its plane: the area-weighted normal, world up for the walls, the longest outer edge for floors
		Vector3 n;
		for (int f : pf) {
			n += nb.xform(face_normal_raw(m, m.faces[f]));
		}
		n = n.length() > 1e-9f ? n.normalized() : Vector3(0, 1, 0);
		const int kind = n.y > 0.7f ? 2 : n.y < -0.7f ? 3 : 1;
		Vector3 v_ax;
		if (kind == 1) {
			v_ax = Vector3(0, 1, 0) - n * n.y;
		} else {
			std::unordered_map<uint64_t, int> cnt;
			for (int f : pf) {
				const Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					cnt[ekey(F.v[i], F.v[(i + 1) % F.size()])]++;
				}
			}
			float best = -1.0f;
			for (int f : pf) {
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
		v_ax = v_ax.length() > 1e-6f ? v_ax.normalized() : (std::fabs(n.x) < 0.9f ? Vector3(1, 0, 0) : Vector3(0, 0, 1)).cross(n).normalized();
		const Vector3 u_ax = v_ax.cross(n).normalized();
		Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
		for (int f : pf) {
			for (int v : m.faces[f].v) {
				const Vector3 w = p_xf.xform(m.pos[v]);
				const Vector2 q(u_ax.dot(w), -v_ax.dot(w)); // (V down the texture as it goes down the wall)
				mn = Vector2(std::min(mn.x, q.x), std::min(mn.y, q.y));
				mx = Vector2(std::max(mx.x, q.x), std::max(mx.y, q.y));
			}
		}
		const float w = mx.x - mn.x, h = mx.y - mn.y;
		bool turned = false;
		const int c = pick(*rects, w, h, kind, o, (uint32_t)kv.first * 2654435761u, turned);
		if (c < 0) {
			continue;
		}
		const HotspotRect &R = (*rects)[c];
		for (int f : pf) {
			Face &F = m.faces[f];
			F.ensure_attrs();
			for (int i = 0; i < F.size(); i++) {
				const Vector3 wp = p_xf.xform(m.pos[F.v[i]]);
				Vector2 s((u_ax.dot(wp) - mn.x) / std::max(1e-6f, w), (-v_ax.dot(wp) - mn.y) / std::max(1e-6f, h));
				float lu = w, lv = h; // (meters along the rectangle's U / V)
				if (turned) {
					s = Vector2(s.y, 1.0f - s.x);
					std::swap(lu, lv);
				}
				Vector2 uv = R.r.position + s * R.r.size;
				if (R.tile == 1) {
					// repeats along U: the height fills the strip, the length goes on at the same scale
					const float k = R.r.size.y / std::max(1e-6f, lv);
					uv.x = R.r.position.x + s.x * lu * k;
				} else if (R.tile == 2) {
					const float k = R.r.size.x / std::max(1e-6f, lu);
					uv.y = R.r.position.y + s.y * lv * k;
				}
				F.uv[i] = uv;
			}
			F.manual_uv = true;
			done++;
		}
	}
	return done;
}

} // namespace hewn
