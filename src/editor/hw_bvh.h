// A bounding volume tree over the triangles of the edited mesh (its local space): the same nearest hit as
// raycast_node, without testing every triangle. Made again only when the positions or the faces change.
#pragma once

#include "../hw_core.h"
#include "hw_editor_util.h"

#include <cstring>
#include <vector>

namespace hw_editor {

// Changes whenever the positions or the faces' corners change (not with the colors / UVs).
inline uint64_t geometry_hash(const hewn::Mesh &m) {
	uint64_t h = 1469598103934665603ull;
	auto mix = [&](const void *p, size_t n) {
		// (eight bytes at a time; the rest one by one)
		const unsigned char *c = (const unsigned char *)p;
		size_t i = 0;
		for (; i + 8 <= n; i += 8) {
			uint64_t w;
			std::memcpy(&w, c + i, 8);
			h = (h ^ w) * 1099511628211ull;
			h ^= h >> 29;
		}
		for (; i < n; i++) {
			h = (h ^ c[i]) * 1099511628211ull;
		}
	};
	if (!m.pos.empty()) {
		mix(m.pos.data(), m.pos.size() * sizeof(Vector3));
	}
	const uint64_t fc = m.faces.size();
	mix(&fc, sizeof(fc));
	for (const hewn::Face &F : m.faces) {
		const int n = F.size();
		mix(&n, sizeof(n));
		if (n) {
			mix(F.v.data(), n * sizeof(int));
		}
	}
	return h;
}

struct TriBVH {
	struct Node {
		Vector3 lo, hi;
		int left = -1, right = -1; // (children; -1: a leaf)
		int start = 0, count = 0; // (a leaf's triangles in `order`)
	};
	struct Tri {
		Vector3 a, b, c;
		int face;
	};
	std::vector<Node> nodes;
	std::vector<Tri> tris;
	std::vector<int> order;
	std::vector<Vector3> face_n; // (per face: its normal, for the culled back faces)

	void build(const hewn::Mesh &m, const hewn::BuildOut &b) {
		nodes.clear();
		tris.clear();
		order.clear();
		face_n.assign(m.faces.size(), Vector3());
		for (int f = 0; f < (int)m.faces.size() && f < (int)b.face_tris.size(); f++) {
			const hewn::Face &F = m.faces[f];
			face_n[f] = hewn::face_normal(m, F);
			const std::vector<int> &tr = b.face_tris[f];
			for (size_t k = 0; k + 2 < tr.size(); k += 3) {
				tris.push_back({ m.pos[F.v[tr[k]]], m.pos[F.v[tr[k + 1]]], m.pos[F.v[tr[k + 2]]], f });
			}
		}
		order.resize(tris.size());
		for (size_t i = 0; i < tris.size(); i++) {
			order[i] = (int)i;
		}
		if (!tris.empty()) {
			nodes.reserve(tris.size() * 2 / 3 + 2);
			split(0, (int)tris.size());
		}
	}

	int split(int p_start, int p_count) {
		Node nd;
		Vector3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f), clo = lo, chi = hi;
		for (int i = p_start; i < p_start + p_count; i++) {
			const Tri &t = tris[order[i]];
			for (const Vector3 &p : { t.a, t.b, t.c }) {
				lo = Vector3(MIN(lo.x, p.x), MIN(lo.y, p.y), MIN(lo.z, p.z));
				hi = Vector3(MAX(hi.x, p.x), MAX(hi.y, p.y), MAX(hi.z, p.z));
			}
			const Vector3 c = (t.a + t.b + t.c) / 3.0f;
			clo = Vector3(MIN(clo.x, c.x), MIN(clo.y, c.y), MIN(clo.z, c.z));
			chi = Vector3(MAX(chi.x, c.x), MAX(chi.y, c.y), MAX(chi.z, c.z));
		}
		const float pad = 1e-5f;
		nd.lo = lo - Vector3(pad, pad, pad);
		nd.hi = hi + Vector3(pad, pad, pad);
		const int id = (int)nodes.size();
		nodes.push_back(nd);
		const Vector3 ext = chi - clo;
		const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;
		if (p_count <= 4 || ext[axis] < 1e-9f) {
			nodes[id].start = p_start;
			nodes[id].count = p_count;
			return id;
		}
		// the middle of the centers along the longest side
		const float mid = (clo[axis] + chi[axis]) * 0.5f;
		int i = p_start, j = p_start + p_count - 1;
		while (i <= j) {
			const Tri &t = tris[order[i]];
			if ((t.a[axis] + t.b[axis] + t.c[axis]) / 3.0f < mid) {
				i++;
			} else {
				std::swap(order[i], order[j]);
				j--;
			}
		}
		int left_count = i - p_start;
		if (left_count == 0 || left_count == p_count) {
			left_count = p_count / 2;
		}
		const int l = split(p_start, left_count);
		const int r = split(p_start + left_count, p_count - left_count);
		nodes[id].left = l;
		nodes[id].right = r;
		return id;
	}

	static bool slab(const Node &n, const Vector3 &o, const Vector3 &inv, float tmax) {
		float t0 = 0.0f, t1 = tmax;
		for (int k = 0; k < 3; k++) {
			float a = (n.lo[k] - o[k]) * inv[k], b = (n.hi[k] - o[k]) * inv[k];
			if (a > b) {
				std::swap(a, b);
			}
			if (a != a || b != b) {
				continue; // (0 * inf: the ray in that slab's plane)
			}
			t0 = MAX(t0, a);
			t1 = MIN(t1, b);
			if (t0 > t1) {
				return false;
			}
		}
		return true;
	}

	// The nearest face hit (local ray), its parameter along d; ties: the lower face (like raycast_node).
	int raycast(const Vector3 &o, const Vector3 &d, float &r_t, bool p_cull_back) const {
		if (nodes.empty()) {
			return -1;
		}
		const Vector3 inv(d.x != 0.0f ? 1.0f / d.x : 1e30f, d.y != 0.0f ? 1.0f / d.y : 1e30f, d.z != 0.0f ? 1.0f / d.z : 1e30f);
		int best = -1;
		float best_t = 1e30f;
		int stack[256];
		int sp = 0;
		bool overflow = false;
		stack[sp++] = 0;
		while (sp) {
			const Node &n = nodes[stack[--sp]];
			if (!slab(n, o, inv, best_t * 1.0001f + 1e-6f)) {
				continue;
			}
			if (n.left < 0) {
				for (int i = n.start; i < n.start + n.count; i++) {
					const Tri &t = tris[order[i]];
					if (p_cull_back && face_n[t.face].dot(d) > 0.0f) {
						continue;
					}
					float tt;
					if (ray_tri(o, d, t.a, t.b, t.c, tt) && (tt < best_t || (tt == best_t && t.face < best))) {
						best_t = tt;
						best = t.face;
					}
				}
				continue;
			}
			if (sp + 2 <= 256) {
				stack[sp++] = n.right;
				stack[sp++] = n.left;
			} else {
				overflow = true;
			}
		}
		if (overflow) {
			// (a very lopsided tree: every triangle, to be sure)
			for (const Tri &t : tris) {
				if (p_cull_back && face_n[t.face].dot(d) > 0.0f) {
					continue;
				}
				float tt;
				if (ray_tri(o, d, t.a, t.b, t.c, tt) && (tt < best_t || (tt == best_t && t.face < best))) {
					best_t = tt;
					best = t.face;
				}
			}
		}
		if (best >= 0) {
			r_t = best_t;
		}
		return best;
	}
};

} // namespace hw_editor
