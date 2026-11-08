// Hewn — dynamic topology for sculpting: under the brush, edges longer than the detail are split and ones much
// shorter collapsed, so the surface keeps about that much detail where it is sculpted. Works on triangles (the
// faces in reach are triangulated first); the corners' UVs and colors are interpolated on the new vertices.
// Only the faces near the brush are looked at, so a dab costs about as much as its reach, not the whole mesh.
#include "hw_core.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace hewn {
namespace ops {

namespace {

// A corner of a new triangle: a corner of the source face, or a point between two of its corners.
struct CornerRef {
	int vert;
	int ci, cj; // corners of the source face
	float t; // 0: ci, 1: cj
};

Face make_tri(const Face &src, const CornerRef (&c)[3]) {
	Face T;
	T.copy_attrs_from(src);
	const bool has_uv = src.uv.size() == src.v.size();
	const bool has_col = src.col.size() == src.v.size();
	const bool has_cust = src.cust.size() == src.v.size() * 4;
	for (int k = 0; k < 3; k++) {
		const CornerRef &r = c[k];
		T.v.push_back(r.vert);
		if (has_uv) {
			T.uv.push_back(src.uv[r.ci].lerp(src.uv[r.cj], r.t));
		}
		if (has_col) {
			T.col.push_back(src.col[r.ci].lerp(src.col[r.cj], r.t));
		}
		if (has_cust) {
			for (int q = 0; q < 4; q++) {
				T.cust.push_back(src.cust[r.ci * 4 + q].lerp(src.cust[r.cj * 4 + q], r.t));
			}
		}
	}
	return T;
}

// The faces with a corner inside the sphere (not hidden).
std::vector<int> faces_in_reach(const Mesh &m, const Vector3 &c, float r) {
	std::vector<int> out;
	const float r2 = r * r;
	for (int f = 0; f < (int)m.faces.size(); f++) {
		const Face &F = m.faces[f];
		if (F.hidden) {
			continue;
		}
		for (int v : F.v) {
			if (m.pos[v].distance_squared_to(c) <= r2) {
				out.push_back(f);
				break;
			}
		}
	}
	return out;
}

} // namespace

bool dyntopo_refine(Mesh &m, const Vector3 &p_center, float p_radius, float p_max_len, bool p_subdivide, bool p_collapse, int p_budget) {
	if (p_max_len <= 1e-6f || p_radius <= 0.0f) {
		return false;
	}
	bool changed = false;
	// the faces in reach as triangles
	{
		std::vector<int> big;
		for (int f : faces_in_reach(m, p_center, p_radius)) {
			if (m.faces[f].size() > 3) {
				big.push_back(f);
			}
		}
		if (!big.empty()) {
			triangulate_faces(m, big);
			changed = true;
		}
	}
	const float r2 = p_radius * p_radius;
	auto inside = [&](int v) { return m.pos[v].distance_squared_to(p_center) <= r2; };
	int budget = p_budget;
	// ---- split the long edges (longest first), a few passes
	for (int pass = 0; p_subdivide && pass < 12 && budget > 0; pass++) {
		const std::vector<int> reach = faces_in_reach(m, p_center, p_radius);
		std::unordered_map<uint64_t, std::vector<int>> ef;
		struct Long {
			float len;
			int a, b;
		};
		std::vector<Long> longs;
		for (int f : reach) {
			const Face &F = m.faces[f];
			if (F.size() != 3) {
				continue;
			}
			for (int i = 0; i < 3; i++) {
				const int a = F.v[i], b = F.v[(i + 1) % 3];
				std::vector<int> &l = ef[ekey(a, b)];
				l.push_back(f);
				if (l.size() == 1 && (inside(a) || inside(b))) {
					const float len = m.pos[a].distance_to(m.pos[b]);
					if (len > p_max_len) {
						longs.push_back({ len, a, b });
					}
				}
			}
		}
		if (longs.empty()) {
			break;
		}
		std::sort(longs.begin(), longs.end(), [](const Long &x, const Long &y) { return x.len > y.len; });
		std::unordered_set<int> touched; // (faces split in this pass: their edges wait for the next)
		for (const Long &L : longs) {
			if (budget <= 0) {
				break;
			}
			auto it = ef.find(ekey(L.a, L.b));
			if (it == ef.end()) {
				continue;
			}
			bool free = true;
			for (int f : it->second) {
				free = free && !touched.count(f) && m.faces[f].size() == 3;
			}
			if (!free || it->second.empty() || it->second.size() > 2) {
				continue;
			}
			const int mid = (int)m.pos.size();
			m.pos.push_back((m.pos[L.a] + m.pos[L.b]) * 0.5f);
			for (int f : it->second) {
				const Face src = m.faces[f];
				// the corner i where the edge starts (i -> i + 1 along the face)
				int i = -1;
				for (int k = 0; k < 3; k++) {
					const int x = src.v[k], y = src.v[(k + 1) % 3];
					if ((x == L.a && y == L.b) || (x == L.b && y == L.a)) {
						i = k;
					}
				}
				if (i < 0) {
					continue;
				}
				const int j = (i + 1) % 3, o = (i + 2) % 3;
				const CornerRef t1[3] = { { src.v[i], i, i, 0.0f }, { mid, i, j, 0.5f }, { src.v[o], o, o, 0.0f } };
				const CornerRef t2[3] = { { mid, i, j, 0.5f }, { src.v[j], j, j, 0.0f }, { src.v[o], o, o, 0.0f } };
				m.faces[f] = make_tri(src, t1);
				m.faces.push_back(make_tri(src, t2));
				touched.insert(f);
				touched.insert((int)m.faces.size() - 1);
			}
			budget--;
			changed = true;
		}
	}
	// ---- collapse the short edges (shortest first), both ends inside, not on a border, no fold
	const float min_len = p_max_len * 0.4f;
	for (int pass = 0; p_collapse && pass < 3 && budget > 0; pass++) {
		const std::vector<int> reach = faces_in_reach(m, p_center, p_radius);
		std::unordered_map<int, std::vector<int>> vf; // the faces of each vertex (all of them: a vertex inside has all its faces in reach)
		std::unordered_map<uint64_t, int> ecount;
		for (int f : reach) {
			const Face &F = m.faces[f];
			for (int i = 0; i < F.size(); i++) {
				vf[F.v[i]].push_back(f);
				ecount[ekey(F.v[i], F.v[(i + 1) % F.size()])]++;
			}
		}
		struct Short {
			float len;
			int a, b;
		};
		std::vector<Short> shorts;
		for (const auto &kv : ecount) {
			const int a = (int)(kv.first >> 32), b = (int)(kv.first & 0xffffffffu);
			if (kv.second != 2 || !inside(a) || !inside(b)) {
				continue;
			}
			const float len = m.pos[a].distance_to(m.pos[b]);
			if (len < min_len) {
				shorts.push_back({ len, a, b });
			}
		}
		if (shorts.empty()) {
			break;
		}
		std::sort(shorts.begin(), shorts.end(), [](const Short &x, const Short &y) { return x.len < y.len; });
		auto on_border = [&](int v) {
			for (int f : vf[v]) {
				const Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					if ((F.v[i] == v || F.v[(i + 1) % F.size()] == v) && ecount[ekey(F.v[i], F.v[(i + 1) % F.size()])] != 2) {
						return true;
					}
				}
			}
			return false;
		};
		auto neighbors = [&](int v) {
			std::unordered_set<int> out;
			for (int f : vf[v]) {
				for (int x : m.faces[f].v) {
					if (x != v) {
						out.insert(x);
					}
				}
			}
			return out;
		};
		std::unordered_set<int> locked; // (vertices of a collapse in this pass: their faces changed)
		std::vector<char> dead(m.faces.size(), 0);
		bool any = false;
		for (const Short &S : shorts) {
			if (budget <= 0) {
				break;
			}
			const int a = S.a, b = S.b;
			if (locked.count(a) || locked.count(b) || on_border(a) || on_border(b)) {
				continue;
			}
			// the link condition: a and b share exactly the two vertices of the two triangles on the edge
			const std::unordered_set<int> na = neighbors(a), nb = neighbors(b);
			int common = 0;
			for (int x : na) {
				common += nb.count(x) ? 1 : 0;
			}
			if (common != 2) {
				continue;
			}
			const Vector3 mid = (m.pos[a] + m.pos[b]) * 0.5f;
			// no face around turns over (nor gets nearly flat on itself)
			bool ok = true;
			for (int v : { a, b }) {
				for (int f : vf[v]) {
					const Face &F = m.faces[f];
					if (dead[f] || (std::find(F.v.begin(), F.v.end(), a) != F.v.end() && std::find(F.v.begin(), F.v.end(), b) != F.v.end())) {
						continue;
					}
					std::vector<Vector3> before, after;
					for (int x : F.v) {
						before.push_back(m.pos[x]);
						after.push_back(x == a || x == b ? mid : m.pos[x]);
					}
					const Vector3 n0 = newell(before), n1 = newell(after);
					if (n1.length() < 1e-12f || n0.normalized().dot(n1.normalized()) < 0.3f) {
						ok = false;
						break;
					}
				}
				if (!ok) {
					break;
				}
			}
			if (!ok) {
				continue;
			}
			m.pos[a] = mid;
			for (int f : vf[b]) {
				Face &F = m.faces[f];
				const bool has_a = std::find(F.v.begin(), F.v.end(), a) != F.v.end();
				if (has_a) {
					dead[f] = 1; // (one of the two triangles on the edge)
					continue;
				}
				for (int &x : F.v) {
					if (x == b) {
						x = a;
					}
				}
				vf[a].push_back(f);
			}
			for (int x : na) {
				locked.insert(x);
			}
			for (int x : nb) {
				locked.insert(x);
			}
			locked.insert(a);
			locked.insert(b);
			budget--;
			any = true;
		}
		if (!any) {
			break;
		}
		std::vector<Face> kept;
		kept.reserve(m.faces.size());
		for (size_t f = 0; f < m.faces.size(); f++) {
			if (!dead[f]) {
				kept.push_back(std::move(m.faces[f]));
			}
		}
		m.faces.swap(kept);
		changed = true;
	}
	return changed;
}

} // namespace ops
} // namespace hewn
