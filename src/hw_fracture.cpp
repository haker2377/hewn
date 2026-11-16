// Hewn — Fracture: a closed mesh broken into pieces (Voronoi cells round points scattered inside it): each piece
// the mesh cut by the planes half way to the other points, its cuts closed (their faces in the material of
// the inside), pulled in a little toward its middle (the gap); a seed breaks it another way.
#include "hw_core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace hewn {
namespace ops {

namespace {

struct FRng {
	uint32_t s;
	float next() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return (float)(s & 0xFFFFFF) / (float)0xFFFFFF;
	}
};

// inside a closed mesh: an odd number of its triangles crossed going +X
bool inside(const std::vector<std::array<Vector3, 3>> &tris, const Vector3 &p) {
	int hits = 0;
	const Vector3 d(1.0f, 0.000123f, 0.000257f);
	for (const auto &t : tris) {
		const Vector3 e1 = t[1] - t[0], e2 = t[2] - t[0];
		const Vector3 h = d.cross(e2);
		const float a = e1.dot(h);
		if (std::fabs(a) < 1e-12f) {
			continue;
		}
		const float f = 1.0f / a;
		const Vector3 s = p - t[0];
		const float u = f * s.dot(h);
		if (u < 0.0f || u > 1.0f) {
			continue;
		}
		const Vector3 q = s.cross(e1);
		const float v = f * d.dot(q);
		if (v < 0.0f || u + v > 1.0f) {
			continue;
		}
		if (f * e2.dot(q) > 0.0f) {
			hits++;
		}
	}
	return hits & 1;
}

} // namespace

int fracture(Mesh &m, int p_pieces, int p_seed, float p_gap, int p_inner_material, std::vector<int> *r_piece_of) {
	if (m.faces.empty() || p_pieces < 2) {
		return 1;
	}
	std::vector<std::array<Vector3, 3>> tris;
	for (const Face &F : m.faces) {
		for (int i = 1; i + 1 < F.size(); i++) {
			tris.push_back({ m.pos[F.v[0]], m.pos[F.v[i]], m.pos[F.v[i + 1]] });
		}
	}
	Vector3 mn = m.pos[0], mx = m.pos[0];
	for (const Vector3 &p : m.pos) {
		mn = mn.min(p);
		mx = mx.max(p);
	}
	// the points: at random in its box, kept when inside it (an open mesh: all kept)
	const bool closed = holes(m).empty();
	FRng rng{ (uint32_t)std::max(0, p_seed) * 2654435761u + 0x51ED27u };
	std::vector<Vector3> pts;
	for (int tries = 0; tries < p_pieces * 60 && (int)pts.size() < p_pieces; tries++) {
		const Vector3 q(mn.x + (mx.x - mn.x) * rng.next(), mn.y + (mx.y - mn.y) * rng.next(), mn.z + (mx.z - mn.z) * rng.next());
		if (!closed || inside(tris, q)) {
			pts.push_back(q);
		}
	}
	if (pts.size() < 2) {
		return 1;
	}
	const float size = (mx - mn).length();
	Mesh out;
	int made = 0;
	std::vector<int> piece_of;
	for (size_t i = 0; i < pts.size(); i++) {
		Mesh cell = m;
		std::vector<std::pair<Vector3, Vector3>> planes;
		for (size_t j = 0; j < pts.size(); j++) {
			if (j == i || cell.faces.empty()) {
				continue;
			}
			const Vector3 n = (pts[j] - pts[i]).normalized();
			const Vector3 c = (pts[i] + pts[j]) * 0.5f;
			bool beyond = false;
			for (const Vector3 &p : cell.pos) {
				if (n.dot(p - c) > 1e-6f) {
					beyond = true;
					break;
				}
			}
			if (!beyond) {
				continue; // (the cell does not reach that plane)
			}
			bisect(cell, std::vector<int>(), c, n, false, true, closed);
			remove_unused_vertices(cell);
			planes.push_back({ c, n });
		}
		if (cell.faces.empty()) {
			continue;
		}
		// the cuts: the inside material, their UVs from the world
		const float eps = std::max(1e-5f, size * 1e-5f);
		for (Face &F : cell.faces) {
			bool cut = false; // (all its corners on one of the planes it was cut by)
			for (const auto &pl : planes) {
				bool on = true;
				for (int v : F.v) {
					on = on && std::fabs(pl.second.dot(cell.pos[v] - pl.first)) < eps;
				}
				cut = cut || on;
			}
			if (cut) {
				if (p_inner_material >= 0) {
					F.material = p_inner_material;
				}
				F.manual_uv = false;
				F.uvp.world = true;
				F.smooth = 0;
			}
		}
		// the gap: the piece drawn in toward its middle
		if (p_gap > 0.0f) {
			Vector3 c;
			for (const Vector3 &p : cell.pos) {
				c += p;
			}
			c /= (float)cell.pos.size();
			float r = 0.0f;
			for (const Vector3 &p : cell.pos) {
				r = std::max(r, p.distance_to(c));
			}
			const float k = r > 1e-6f ? std::max(0.0f, 1.0f - p_gap / r) : 1.0f;
			for (Vector3 &p : cell.pos) {
				p = c + (p - c) * k;
			}
		}
		const int base_f = (int)out.faces.size();
		append(out, cell, Transform3D(), 0);
		for (int f = base_f; f < (int)out.faces.size(); f++) {
			piece_of.push_back(made);
		}
		made++;
	}
	if (made == 0) {
		return 1;
	}
	m = out;
	if (r_piece_of) {
		*r_piece_of = piece_of;
	}
	return made;
}

std::vector<Mesh> loose_parts(const Mesh &m) {
	std::vector<int> parent(m.pos.size());
	for (size_t i = 0; i < parent.size(); i++) {
		parent[i] = (int)i;
	}
	auto root_of = [&](int x) {
		while (parent[x] != x) {
			parent[x] = parent[parent[x]];
			x = parent[x];
		}
		return x;
	};
	for (const Face &F : m.faces) {
		for (int i = 1; i < F.size(); i++) {
			parent[root_of(F.v[i])] = root_of(F.v[0]);
		}
	}
	std::vector<Mesh> out;
	std::unordered_map<int, int> part_of;
	std::vector<std::unordered_map<int, int>> remap;
	for (const Face &F : m.faces) {
		if (F.v.empty()) {
			continue;
		}
		const int r = root_of(F.v[0]);
		auto it = part_of.find(r);
		if (it == part_of.end()) {
			it = part_of.emplace(r, (int)out.size()).first;
			out.emplace_back();
			remap.emplace_back();
		}
		Mesh &pm = out[it->second];
		std::unordered_map<int, int> &rm = remap[it->second];
		Face nf = F;
		for (int &v : nf.v) {
			auto vi = rm.find(v);
			if (vi == rm.end()) {
				vi = rm.emplace(v, pm.add_vertex(m.pos[v])).first;
			}
			v = vi->second;
		}
		pm.faces.push_back(nf);
	}
	return out;
}

} // namespace ops
} // namespace hewn
