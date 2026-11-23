// Hewn — skinning weights: kept through edits, made from bones, normalized, smoothed, mirrored.
#include "hw_skin.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace hewn {
namespace skin {

namespace {

// The points in cells of a grid: the nearest ones found by looking at the cells around, wider and wider.
struct Grid {
	float cell = 1.0f;
	Vector3 origin;
	std::unordered_map<uint64_t, std::vector<int>> cells;
	const std::vector<Vector3> *pts = nullptr;

	static uint64_t key(int x, int y, int z) {
		return ((uint64_t)(uint32_t)(x + 1048576) & 0x1fffff) | (((uint64_t)(uint32_t)(y + 1048576) & 0x1fffff) << 21) |
				(((uint64_t)(uint32_t)(z + 1048576) & 0x1fffff) << 42);
	}
	void build(const std::vector<Vector3> &p) {
		pts = &p;
		cells.clear();
		if (p.empty()) {
			return;
		}
		Vector3 mn = p[0], mx = p[0];
		for (const Vector3 &q : p) {
			mn = mn.min(q);
			mx = mx.max(q);
		}
		origin = mn;
		const Vector3 sz = mx - mn;
		const float vol = std::max(1e-9f, std::max(sz.x, 1e-4f) * std::max(sz.y, 1e-4f) * std::max(sz.z, 1e-4f));
		cell = std::max(1e-4f, std::cbrt(vol / std::max<size_t>(1, p.size()) * 4.0f));
		for (int i = 0; i < (int)p.size(); i++) {
			const Vector3 c = (p[i] - origin) / cell;
			cells[key((int)std::floor(c.x), (int)std::floor(c.y), (int)std::floor(c.z))].push_back(i);
		}
	}
	// up to k nearest (index, distance), nearest first
	void nearest(const Vector3 &q, int k, std::vector<std::pair<float, int>> &r_out) const {
		r_out.clear();
		if (!pts || pts->empty()) {
			return;
		}
		const Vector3 c = (q - origin) / cell;
		const int cx = (int)std::floor(c.x), cy = (int)std::floor(c.y), cz = (int)std::floor(c.z);
		for (int ring = 0; ring < 64; ring++) {
			for (int x = cx - ring; x <= cx + ring; x++) {
				for (int y = cy - ring; y <= cy + ring; y++) {
					for (int z = cz - ring; z <= cz + ring; z++) {
						if (std::max(std::abs(x - cx), std::max(std::abs(y - cy), std::abs(z - cz))) != ring) {
							continue; // (only the new shell)
						}
						auto it = cells.find(key(x, y, z));
						if (it == cells.end()) {
							continue;
						}
						for (int i : it->second) {
							r_out.push_back({ (*pts)[i].distance_to(q), i });
						}
					}
				}
			}
			// enough found, and nothing nearer can be beyond this shell
			if ((int)r_out.size() >= k) {
				std::sort(r_out.begin(), r_out.end());
				if (r_out[k - 1].first <= ring * cell) {
					break;
				}
			}
		}
		std::sort(r_out.begin(), r_out.end());
		if ((int)r_out.size() > k) {
			r_out.resize(k);
		}
	}
};

float seg_dist(const Vector3 &p, const Vector3 &a, const Vector3 &b, Vector3 *r_at = nullptr) {
	const Vector3 ab = b - a;
	const float l = ab.length_squared();
	const float t = l > 1e-12f ? std::clamp((p - a).dot(ab) / l, 0.0f, 1.0f) : 0.0f;
	const Vector3 at = a + ab * t;
	if (r_at) {
		*r_at = at;
	}
	return p.distance_to(at);
}

} // namespace

void normalize(Weights &w) {
	float s = 0.0f;
	for (const auto &p : w) {
		s += std::max(0.0f, p.second);
	}
	if (s <= 1e-9f) {
		return;
	}
	for (auto &p : w) {
		p.second = std::max(0.0f, p.second) / s;
	}
}

float weight_of(const Weights &w, int p_bone) {
	for (const auto &p : w) {
		if (p.first == p_bone) {
			return p.second;
		}
	}
	return 0.0f;
}

void set_weight(Weights &w, int p_bone, float p_value, bool p_normalize) {
	const float v = std::clamp(p_value, 0.0f, 1.0f);
	bool found = false;
	for (auto &p : w) {
		if (p.first == p_bone) {
			p.second = v;
			found = true;
		}
	}
	if (!found && v > 0.0f) {
		w.push_back({ p_bone, v });
	}
	if (p_normalize) {
		float others = 0.0f;
		for (const auto &p : w) {
			if (p.first != p_bone) {
				others += p.second;
			}
		}
		if (others > 1e-9f) {
			const float k = (1.0f - v) / others; // (the others take what is left)
			for (auto &p : w) {
				if (p.first != p_bone) {
					p.second *= k;
				}
			}
		}
	}
	w.erase(std::remove_if(w.begin(), w.end(), [](const std::pair<int, float> &p) { return p.second <= 1e-5f; }), w.end());
}

void clean(Weights &w, float p_min, int p_limit) {
	w.erase(std::remove_if(w.begin(), w.end(), [&](const std::pair<int, float> &p) { return p.second < p_min; }), w.end());
	if (p_limit > 0 && (int)w.size() > p_limit) {
		std::sort(w.begin(), w.end(), [](const std::pair<int, float> &a, const std::pair<int, float> &b) { return a.second > b.second; });
		w.resize(p_limit);
	}
	normalize(w);
}

void top4(const Weights &w, int r_bones[4], float r_weights[4]) {
	for (int k = 0; k < 4; k++) {
		r_bones[k] = 0;
		r_weights[k] = 0.0f;
	}
	Weights s = w;
	std::sort(s.begin(), s.end(), [](const std::pair<int, float> &a, const std::pair<int, float> &b) { return a.second > b.second; });
	float sum = 0.0f;
	int n = 0;
	for (size_t i = 0; i < s.size() && n < 4; i++) {
		if (s[i].second <= 0.0f || s[i].first < 0) {
			continue;
		}
		r_bones[n] = s[i].first;
		r_weights[n] = s[i].second;
		sum += s[i].second;
		n++;
	}
	if (sum > 1e-9f) {
		for (int k = 0; k < n; k++) {
			r_weights[k] /= sum;
		}
	}
}

void fill_weights(Mesh &m, const std::vector<Vector3> &ref_pos, const std::vector<Weights> &ref_w) {
	if (ref_pos.empty() || ref_pos.size() != ref_w.size()) {
		return;
	}
	size_t keep = m.weights.size();
	if (keep > m.pos.size()) {
		keep = 0; // (more weights than vertices: not theirs any more, all taken again)
		m.weights.clear();
	}
	if (keep == m.pos.size()) {
		return;
	}
	m.weights.resize(m.pos.size());
	Grid g;
	g.build(ref_pos);
	std::vector<std::pair<float, int>> near;
	for (size_t v = keep; v < m.pos.size(); v++) {
		g.nearest(m.pos[v], 3, near);
		Weights &out = m.weights[v];
		out.clear();
		if (near.empty()) {
			continue;
		}
		if (near[0].first < 1e-6f) {
			out = ref_w[near[0].second];
			continue;
		}
		std::unordered_map<int, float> acc;
		float tot = 0.0f;
		for (const auto &n : near) {
			const float k = 1.0f / (n.first * n.first + 1e-12f);
			tot += k;
			for (const auto &p : ref_w[n.second]) {
				acc[p.first] += p.second * k;
			}
		}
		for (const auto &kv : acc) {
			out.push_back({ kv.first, kv.second / tot });
		}
		clean(out, 0.005f, 0);
	}
}

std::vector<std::vector<int>> neighbors(const Mesh &m) {
	std::vector<std::vector<int>> nb(m.pos.size());
	auto link = [&](int a, int b) {
		if (a < 0 || b < 0 || a >= (int)nb.size() || b >= (int)nb.size() || a == b) {
			return;
		}
		if (std::find(nb[a].begin(), nb[a].end(), b) == nb[a].end()) {
			nb[a].push_back(b);
			nb[b].push_back(a);
		}
	};
	for (const Face &F : m.faces) {
		for (int i = 0; i < F.size(); i++) {
			link(F.v[i], F.v[(i + 1) % F.size()]);
		}
	}
	for (const auto &w : m.wires) {
		link(w.first, w.second);
	}
	return nb;
}

std::vector<int> mirror_map(const Mesh &m, float p_eps) {
	std::vector<int> out(m.pos.size(), -1);
	Grid g;
	g.build(m.pos);
	std::vector<std::pair<float, int>> near;
	for (size_t v = 0; v < m.pos.size(); v++) {
		const Vector3 q(-m.pos[v].x, m.pos[v].y, m.pos[v].z);
		g.nearest(q, 1, near);
		if (!near.empty() && near[0].first <= p_eps) {
			out[v] = near[0].second;
		}
	}
	return out;
}

void smooth_bone(Mesh &m, int p_bone, int p_iterations, float p_factor, const std::vector<char> *p_only) {
	if (m.weights.size() != m.pos.size()) {
		return;
	}
	const std::vector<std::vector<int>> nb = neighbors(m);
	std::vector<float> w(m.pos.size()), nw;
	for (size_t v = 0; v < m.pos.size(); v++) {
		w[v] = weight_of(m.weights[v], p_bone);
	}
	for (int it = 0; it < p_iterations; it++) {
		nw = w;
		for (size_t v = 0; v < m.pos.size(); v++) {
			if ((p_only && !(*p_only)[v]) || nb[v].empty()) {
				continue;
			}
			float s = 0.0f;
			for (int o : nb[v]) {
				s += w[o];
			}
			nw[v] = w[v] + (s / nb[v].size() - w[v]) * p_factor;
		}
		w.swap(nw);
	}
	for (size_t v = 0; v < m.pos.size(); v++) {
		if (p_only && !(*p_only)[v]) {
			continue;
		}
		if (std::fabs(w[v] - weight_of(m.weights[v], p_bone)) > 1e-6f) {
			set_weight(m.weights[v], p_bone, w[v], true);
		}
	}
}

void auto_weights(Mesh &m, const std::vector<Vector3> &p_heads, const std::vector<Vector3> &p_tails, const std::vector<char> &p_use, int p_kind) {
	const size_t nv = m.pos.size();
	m.weights.assign(nv, Weights());
	if (p_kind == 2 || p_heads.empty()) {
		return;
	}
	// the vertices' normals (the faces' around each)
	std::vector<Vector3> vn(nv);
	for (const Face &F : m.faces) {
		const Vector3 n = face_normal_raw(m, F);
		for (int v : F.v) {
			vn[v] += n;
		}
	}
	for (Vector3 &n : vn) {
		n = n.length() > 1e-12f ? n.normalized() : Vector3();
	}
	const int nb = (int)p_heads.size();
	for (size_t v = 0; v < nv; v++) {
		const Vector3 p = m.pos[v];
		float best = 1e30f;
		int best_b = -1;
		std::vector<std::pair<int, float>> d(nb);
		for (int b = 0; b < nb; b++) {
			if (b < (int)p_use.size() && !p_use[b]) {
				d[b] = { b, 1e30f };
				continue;
			}
			Vector3 at;
			float dist = seg_dist(p, p_heads[b], p_tails[b], &at);
			// a bone outside the surface here (on the side its normal points to) counts much less
			if (vn[v] != Vector3() && (at - p).dot(vn[v]) > dist * 0.2f) {
				dist *= 3.0f;
			}
			d[b] = { b, dist };
			if (dist < best) {
				best = dist;
				best_b = b;
			}
		}
		Weights &w = m.weights[v];
		if (best_b < 0) {
			continue;
		}
		if (p_kind == 1) {
			w.push_back({ best_b, 1.0f });
			continue;
		}
		for (int b = 0; b < nb; b++) {
			if (d[b].second > best * 2.5f + 1e-6f) {
				continue; // (far ones: none)
			}
			const float k = 1.0f / std::pow(d[b].second + best * 0.05f + 1e-6f, 4.0f);
			w.push_back({ b, k });
		}
		normalize(w);
	}
	if (p_kind == 0) {
		// smoothed over the surface: the bones' borders soft
		std::vector<int> used;
		for (int b = 0; b < nb; b++) {
			if (b >= (int)p_use.size() || p_use[b]) {
				used.push_back(b);
			}
		}
		const std::vector<std::vector<int>> nbs = neighbors(m);
		for (int it = 0; it < 3; it++) {
			std::vector<Weights> next(nv);
			for (size_t v = 0; v < nv; v++) {
				std::unordered_map<int, float> acc;
				for (const auto &p : m.weights[v]) {
					acc[p.first] += p.second * 2.0f;
				}
				float tot = 2.0f;
				for (int o : nbs[v]) {
					for (const auto &p : m.weights[o]) {
						acc[p.first] += p.second;
					}
					tot += 1.0f;
				}
				for (const auto &kv : acc) {
					next[v].push_back({ kv.first, kv.second / tot });
				}
			}
			m.weights.swap(next);
		}
	}
	for (Weights &w : m.weights) {
		clean(w, 0.01f, 4);
	}
}

} // namespace skin
} // namespace hewn
