// Hewn — lightmap UVs (UV2): flat charts packed into one atlas.
//
// Level geometry is mostly flat: neighboring faces on one plane become one chart, projected onto that
// plane (no stretching at all), turned so its longest edge runs along U. Charts are packed into shelves,
// tallest first, with a margin of texels around each so the baked light does not bleed between them.
#include "hw_core.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace hewn {

namespace {

struct Chart {
	std::vector<int> faces;
	Vector3 u, v; // projection axes (meters)
	Vector2 mn, mx; // bounds in texels
	Vector2 at; // place in the atlas (texels, the margin included)
	bool turned = false; // packed rotated by 90 degrees (taller than wide)
	float w() const { return (turned ? mx.y - mn.y : mx.x - mn.x); }
	float h() const { return (turned ? mx.x - mn.x : mx.y - mn.y); }
};

} // namespace

uint64_t lightmap_hash(const Mesh &m, float texel) {
	uint64_t h = 1469598103934665603ull;
	auto mix = [&](const void *p, size_t n) {
		const unsigned char *c = (const unsigned char *)p;
		for (size_t i = 0; i < n; i++) {
			h ^= c[i];
			h *= 1099511628211ull;
		}
	};
	mix(&texel, sizeof(float));
	for (const Vector3 &p : m.pos) {
		mix(&p, sizeof(Vector3));
	}
	for (const Face &F : m.faces) {
		const int n = F.size();
		mix(&n, sizeof(int));
		mix(F.v.data(), sizeof(int) * F.v.size());
	}
	return h;
}

godot::Vector2i lightmap_unwrap(const Mesh &m, float p_texel, const std::vector<char> &p_skip, std::vector<std::vector<Vector2>> &r_uv2, int p_max_size) {
	const int nf = (int)m.faces.size();
	r_uv2.assign(nf, {});
	for (int f = 0; f < nf; f++) {
		r_uv2[f].assign(m.faces[f].v.size(), Vector2());
	}
	if (nf == 0) {
		return { 0, 0 };
	}
	Topology t;
	t.build(m);
	std::vector<Vector3> nrm(nf);
	for (int f = 0; f < nf; f++) {
		nrm[f] = face_normal(m, m.faces[f]);
	}
	// charts: neighbors on one plane
	std::vector<int> chart_of(nf, -1);
	std::vector<Chart> charts;
	for (int s = 0; s < nf; s++) {
		if (chart_of[s] >= 0 || (s < (int)p_skip.size() && p_skip[s]) || m.faces[s].size() < 3) {
			continue;
		}
		Chart c;
		const float d0 = nrm[s].dot(m.pos[m.faces[s].v[0]]);
		std::deque<int> q{ s };
		chart_of[s] = (int)charts.size();
		while (!q.empty()) {
			const int f = q.front();
			q.pop_front();
			c.faces.push_back(f);
			const Face &F = m.faces[f];
			for (int i = 0; i < F.size(); i++) {
				const int e = t.find_edge(F.v[i], F.v[(i + 1) % F.size()]);
				if (e < 0 || t.edge_faces[e].size() != 2) {
					continue;
				}
				for (int g : t.edge_faces[e]) {
					if (chart_of[g] >= 0 || (g < (int)p_skip.size() && p_skip[g]) || nrm[g].dot(nrm[s]) < 0.9998f ||
							std::fabs(nrm[s].dot(m.pos[m.faces[g].v[0]]) - d0) > 1e-4f) {
						continue;
					}
					chart_of[g] = (int)charts.size();
					q.push_back(g);
				}
			}
		}
		// U along the longest edge
		const Vector3 n = nrm[s];
		float best = -1.0f;
		Vector3 u;
		for (int f : c.faces) {
			const Face &F = m.faces[f];
			for (int i = 0; i < F.size(); i++) {
				Vector3 e = m.pos[F.v[(i + 1) % F.size()]] - m.pos[F.v[i]];
				e -= n * n.dot(e);
				if (e.length_squared() > best) {
					best = e.length_squared();
					u = e;
				}
			}
		}
		if (best < 1e-16f) {
			Vector3 b2;
			plane_basis(n, u, b2);
		}
		c.u = u.normalized();
		c.v = n.cross(c.u).normalized();
		charts.push_back(c);
	}
	// sizes in texels; a texel bigger when the atlas would get too big
	const float pad = 2.0f; // texels around each chart
	float texel = std::max(1e-4f, p_texel);
	int W = 0, H = 0;
	for (int attempt = 0; attempt < 12; attempt++) {
		float area = 0.0f, widest = 0.0f;
		for (Chart &c : charts) {
			c.mn = Vector2(1e30f, 1e30f);
			c.mx = Vector2(-1e30f, -1e30f);
			for (int f : c.faces) {
				for (int v : m.faces[f].v) {
					const Vector2 p(m.pos[v].dot(c.u) / texel, m.pos[v].dot(c.v) / texel);
					c.mn = c.mn.min(p);
					c.mx = c.mx.max(p);
				}
			}
			// at least a texel each way
			c.mx.x = std::max(c.mx.x, c.mn.x + 1.0f);
			c.mx.y = std::max(c.mx.y, c.mn.y + 1.0f);
			c.turned = (c.mx.y - c.mn.y) > (c.mx.x - c.mn.x);
			area += (c.w() + pad * 2.0f) * (c.h() + pad * 2.0f);
			widest = std::max(widest, c.w() + pad * 2.0f);
		}
		std::vector<int> order(charts.size());
		for (size_t i = 0; i < order.size(); i++) {
			order[i] = (int)i;
		}
		std::sort(order.begin(), order.end(), [&](int a, int b) { return charts[a].h() > charts[b].h(); });
		const float target = std::max(widest, std::ceil(std::sqrt(area) * 1.1f));
		float x = 0.0f, y = 0.0f, shelf = 0.0f, used_w = 0.0f;
		for (int i : order) {
			Chart &c = charts[i];
			const float cw = std::ceil(c.w() + pad * 2.0f), ch = std::ceil(c.h() + pad * 2.0f);
			if (x > 0.0f && x + cw > target) {
				x = 0.0f;
				y += shelf;
				shelf = 0.0f;
			}
			c.at = Vector2(x, y);
			x += cw;
			shelf = std::max(shelf, ch);
			used_w = std::max(used_w, x);
		}
		W = (int)std::ceil(used_w);
		H = (int)std::ceil(y + shelf);
		if (std::max(W, H) <= p_max_size || charts.empty()) {
			break;
		}
		texel *= (float)std::max(W, H) / (float)p_max_size * 1.05f;
	}
	W = std::max(W, 1);
	H = std::max(H, 1);
	for (const Chart &c : charts) {
		for (int f : c.faces) {
			const Face &F = m.faces[f];
			for (int i = 0; i < F.size(); i++) {
				const Vector3 &p = m.pos[F.v[i]];
				Vector2 q(p.dot(c.u) / texel - c.mn.x, p.dot(c.v) / texel - c.mn.y);
				if (c.turned) {
					q = Vector2(q.y, (c.mx.x - c.mn.x) - q.x);
				}
				r_uv2[f][i] = Vector2((c.at.x + pad + q.x) / (float)W, (c.at.y + pad + q.y) / (float)H);
			}
		}
	}
	return { W, H };
}

} // namespace hewn
