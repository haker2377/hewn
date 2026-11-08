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

// One pass of collapsing the edges shorter than p_min_len among the faces of p_reach (shortest first; both ends
// p_inside, not on a border, the two ends sharing just the two triangles' corners, no face around turning over).
// Returns how many were collapsed (budget lowered by as many).
template <typename Inside>
int collapse_pass(Mesh &m, const std::vector<int> &reach, Inside inside, float min_len, int &budget) {
	// (arrays by vertex and a sorted list of edges: no hash tables, the whole mesh may be in reach)
	std::vector<std::vector<int>> vf(m.pos.size()); // the faces of each vertex (all of them: a vertex inside has all its faces in reach)
	std::vector<uint64_t> ekeys;
	for (int f : reach) {
		const Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			vf[F.v[i]].push_back(f);
			ekeys.push_back(ekey(F.v[i], F.v[(i + 1) % F.size()]));
		}
	}
	std::sort(ekeys.begin(), ekeys.end());
	struct Short {
		float len;
		int a, b;
	};
	std::vector<Short> shorts;
	std::vector<char> border(m.pos.size(), 0);
	for (size_t i = 0; i < ekeys.size();) {
		size_t j = i + 1;
		while (j < ekeys.size() && ekeys[j] == ekeys[i]) {
			j++;
		}
		const int a = (int)(ekeys[i] >> 32), b = (int)(ekeys[i] & 0xffffffffu);
		if (j - i != 2) {
			border[a] = border[b] = 1; // (an open edge, or one of more than two faces)
		} else if (inside(a) && inside(b)) {
			const float len = m.pos[a].distance_to(m.pos[b]);
			if (len < min_len) {
				shorts.push_back({ len, a, b });
			}
		}
		i = j;
	}
	if (shorts.empty()) {
		return 0;
	}
	std::sort(shorts.begin(), shorts.end(), [](const Short &x, const Short &y) { return x.len < y.len; });
	auto on_border = [&](int v) { return border[v] != 0; };
	auto neighbors = [&](int v) {
		std::vector<int> out;
		for (int f : vf[v]) {
			for (int x : m.faces[f].v) {
				if (x != v) {
					out.push_back(x);
				}
			}
		}
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	};
	std::vector<char> locked(m.pos.size(), 0); // (vertices of a collapse in this pass: their faces changed)
	std::vector<char> dead(m.faces.size(), 0);
	bool any = false;
	int done = 0;
	for (const Short &S : shorts) {
		if (budget <= 0) {
			break;
		}
		const int a = S.a, b = S.b;
		if (locked[a] || locked[b] || on_border(a) || on_border(b)) {
			continue;
		}
		// the link condition: a and b share exactly the two vertices of the two triangles on the edge
		const std::vector<int> na = neighbors(a), nb = neighbors(b);
		int common = 0;
		for (int x : na) {
			common += std::binary_search(nb.begin(), nb.end(), x) ? 1 : 0;
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
			locked[x] = 1;
		}
		for (int x : nb) {
			locked[x] = 1;
		}
		locked[a] = locked[b] = 1;
		budget--;
		done++;
		any = true;
	}
	if (!any) {
		return 0;
	}
	std::vector<Face> kept;
	kept.reserve(m.faces.size());
	for (size_t f = 0; f < m.faces.size(); f++) {
		if (!dead[f]) {
			kept.push_back(std::move(m.faces[f]));
		}
	}
	m.faces.swap(kept);
	return done;
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
		if (collapse_pass(m, faces_in_reach(m, p_center, p_radius), inside, min_len, budget) == 0) {
			break;
		}
		changed = true;
	}
	return changed;
}

// Simplify: the mesh as triangles, its shortest edges collapsed until about p_ratio of the triangles are left (the
// borders kept; nothing turns over, so it may stop before).
void decimate_collapse(Mesh &m, float p_ratio) {
	if (p_ratio >= 0.9999f || m.faces.empty()) {
		return;
	}
	std::vector<int> all(m.faces.size());
	for (size_t f = 0; f < all.size(); f++) {
		all[f] = (int)f;
	}
	triangulate_faces(m, all);
	const int target = std::max(2, (int)std::round((float)m.faces.size() * std::max(0.0f, p_ratio)));
	auto any = [](int) { return true; };
	for (int pass = 0; pass < 64 && (int)m.faces.size() > target; pass++) {
		std::vector<int> reach(m.faces.size());
		for (size_t f = 0; f < reach.size(); f++) {
			reach[f] = (int)f;
		}
		int budget = std::max(1, ((int)m.faces.size() - target) / 2); // (each collapse takes two triangles)
		if (collapse_pass(m, reach, any, 1e30f, budget) == 0) {
			break;
		}
	}
	remove_unused_vertices(m);
}

} // namespace ops
} // namespace hewn
