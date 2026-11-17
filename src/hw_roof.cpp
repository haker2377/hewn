// Hewn — a roof over an outline: its slopes rising from every side at one pitch until they meet (the straight
// skeleton of the outline: hips over the corners, valleys over the inner ones, ridges), the gable ends standing
// upright (their sides do not rise), a shed (one slope), or flat; out past the walls by the overhang, as thick as
// the thickness. One roof over an L or a T outline joins its wings itself (no faces inside).
#include "hw_core.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace hewn {
namespace shapes {

namespace {

float cross2(const Vector2 &a, const Vector2 &b) {
	return a.x * b.y - a.y * b.x;
}

float signed_area(const std::vector<Vector2> &p) {
	float a = 0.0f;
	for (size_t i = 0; i < p.size(); i++) {
		a += cross2(p[i], p[(i + 1) % p.size()]);
	}
	return a * 0.5f;
}

// The outline moved out by d (its corners mitred, very sharp ones not past 4 d).
std::vector<Vector2> grow(const std::vector<Vector2> &p, float d) {
	const int n = (int)p.size();
	std::vector<Vector2> out(n);
	for (int i = 0; i < n; i++) {
		const Vector2 a = p[(i + n - 1) % n], b = p[i], c = p[(i + 1) % n];
		const Vector2 e0 = (b - a).normalized(), e1 = (c - b).normalized();
		// (counter-clockwise: outward is to the right of the way)
		const Vector2 n0(e0.y, -e0.x), n1(e1.y, -e1.x);
		Vector2 m = n0 + n1;
		const float l = m.length();
		if (l < 1e-6f) {
			out[i] = b + n0 * d;
			continue;
		}
		m /= l;
		const float k = std::min(4.0f, 1.0f / std::max(0.25f, m.dot(n0)));
		out[i] = b + m * (d * k);
	}
	return out;
}

// A weighted straight skeleton: every side moves inward at its own speed (0: a gable, it does not move); the
// corners between them travel along with them, meeting and splitting, until nothing is left. Each side's face:
// the side and the path its two ends went (up by the time, times the slope).
struct WVert {
	Vector2 p; // where it is at time t0
	float t0 = 0.0f;
	Vector2 v; // its speed
	int el = -1, er = -1; // the sides on its left (coming in) and right (going on)
	int prev = -1, next = -1;
	bool alive = true;
	Vector3 start3; // (where it set out from, with its height)
};

struct Skeleton {
	std::vector<Vector2> pts; // the outline (counter-clockwise)
	std::vector<float> w; // the sides' speeds
	std::vector<Vector2> dir, nin; // the sides' way and inward normal
	std::vector<std::vector<std::pair<Vector3, Vector3>>> segs; // per side: the skeleton pieces bounding its face
	float slope = 1.0f;
	std::vector<WVert> V;

	Vector2 speed(int el, int er) const {
		// n_l . v = w_l, n_r . v = w_r
		const Vector2 a = nin[el], b = nin[er];
		const float det = cross2(a, b);
		if (std::fabs(det) < 1e-6f) {
			// (parallel sides: straight inward between them, or along when one stands still)
			if (a.dot(b) > 0.0f) {
				return a * std::max(w[el], w[er]);
			}
			return Vector2();
		}
		return Vector2((w[el] * b.y - w[er] * a.y) / det, (a.x * w[er] - b.x * w[el]) / det);
	}
	Vector2 at(const WVert &x, float t) const { return x.p + x.v * (t - x.t0); }
	Vector3 lift(const Vector2 &q, float t) const { return Vector3(q.x, t * slope, q.y); }

	void trace(int vi, float t) {
		WVert &x = V[vi];
		const Vector2 q = at(x, t);
		const Vector3 e = lift(q, t);
		if (e.distance_to(x.start3) > 1e-6f) {
			segs[x.el].push_back({ x.start3, e });
			segs[x.er].push_back({ x.start3, e });
		}
	}

	int add_vert(const Vector2 &p, float t, int el, int er) {
		WVert x;
		x.p = p;
		x.t0 = t;
		x.el = el;
		x.er = er;
		x.v = speed(el, er);
		x.start3 = lift(p, t);
		V.push_back(x);
		return (int)V.size() - 1;
	}

	// when the side between a and its next closes (its two ends meet)
	float edge_time(int a, float now) const {
		const WVert &A = V[a], &B = V[A.next];
		const Vector2 pa = at(A, now), pb = at(B, now);
		const Vector2 d = pb - pa, dv = B.v - A.v;
		const float l2 = dv.length_squared();
		if (l2 < 1e-12f) {
			return 1e30f;
		}
		float t = -d.dot(dv) / l2;
		if (t < -1e-5f || (d + dv * std::max(0.0f, t)).length() > 1e-3f * std::max(1.0f, d.length())) {
			return 1e30f; // (an end already past the other, or they never meet; at the same moment as another event: now)
		}
		t = std::max(0.0f, t);
		return now + t;
	}

	// when a reflex corner r runs into a side of its loop (between s and its next), and where
	float split_time(int r, int s, float now, Vector2 &r_at) const {
		const WVert &R = V[r];
		const int side = V[s].er;
		if (side == R.el || side == R.er || w[side] <= 0.0f) {
			return 1e30f;
		}
		// the side's line at time t: dot(nin, q) = dot(nin, a0) + w * (t - now) (a0: where it is now)
		const Vector2 a0 = at(V[s], now);
		const Vector2 pr = at(R, now);
		const float den = nin[side].dot(R.v) - w[side];
		if (std::fabs(den) < 1e-7f) {
			return 1e30f;
		}
		float dt = (nin[side].dot(a0) - nin[side].dot(pr)) / den;
		if (dt < -1e-5f) {
			return 1e30f;
		}
		dt = std::max(0.0f, dt);
		const float t = now + dt;
		const Vector2 hit = pr + R.v * dt;
		// within the side as it is then (between its two ends)
		const Vector2 sa = at(V[s], t), sb = at(V[V[s].next], t);
		const Vector2 e = sb - sa;
		const float L = e.length();
		if (L < 1e-6f) {
			return 1e30f;
		}
		const float u = (hit - sa).dot(e) / (L * L);
		if (u < -1e-4f || u > 1.0f + 1e-4f) {
			return 1e30f;
		}
		r_at = hit;
		return t;
	}

	bool reflex(int vi) const {
		const WVert &x = V[vi];
		return cross2(dir[x.el], dir[x.er]) < -1e-6f;
	}

	void run() {
		const int n = (int)pts.size();
		segs.assign(n, {});
		dir.resize(n);
		nin.resize(n);
		for (int i = 0; i < n; i++) {
			dir[i] = (pts[(i + 1) % n] - pts[i]).normalized();
			nin[i] = Vector2(-dir[i].y, dir[i].x); // (counter-clockwise: inward on the left)
		}
		V.clear();
		for (int i = 0; i < n; i++) {
			add_vert(pts[i], 0.0f, (i + n - 1) % n, i);
		}
		for (int i = 0; i < n; i++) {
			V[i].prev = (i + n - 1) % n;
			V[i].next = (i + 1) % n;
		}
		float now = 0.0f;
		for (int guard = 0; guard < 20000; guard++) {
			// the loops still moving
			int alive = 0;
			for (const WVert &x : V) {
				alive += x.alive ? 1 : 0;
			}
			if (alive == 0) {
				break;
			}
			// small loops end at once
			bool ended = false;
			for (int i = 0; i < (int)V.size(); i++) {
				if (!V[i].alive) {
					continue;
				}
				int cnt = 1;
				for (int j = V[i].next; j != i && cnt < 4; j = V[j].next) {
					cnt++;
				}
				if (cnt == 2) {
					// a ridge between the two
					const int j = V[i].next;
					trace(i, now);
					trace(j, now);
					const Vector3 a = lift(at(V[i], now), now), b = lift(at(V[j], now), now);
					if (a.distance_to(b) > 1e-6f) {
						segs[V[i].el].push_back({ a, b });
						segs[V[i].er].push_back({ a, b });
					}
					V[i].alive = V[j].alive = false;
					ended = true;
				} else if (cnt <= 1) {
					V[i].alive = false;
					ended = true;
				}
			}
			if (ended) {
				continue;
			}
			// the next event: a side closing, or a reflex corner splitting a side
			float best = 1e30f;
			int kind = 0, ea = -1, sb = -1;
			Vector2 hit;
			for (int i = 0; i < (int)V.size(); i++) {
				if (!V[i].alive) {
					continue;
				}
				const float t = edge_time(i, now);
				if (t < best - 1e-7f) {
					best = t;
					kind = 1;
					ea = i;
				}
				if (reflex(i)) {
					for (int s = V[i].next; s != V[i].prev && s != i; s = V[s].next) {
						Vector2 h;
						const float ts = split_time(i, s, now, h);
						if (ts < best - 1e-7f) {
							best = ts;
							kind = 2;
							ea = i;
							sb = s;
							hit = h;
						}
					}
				}
			}
			if (kind == 0 || best > 1e6f) {
				// (nothing meets: the corners left run on a little and stop - an outline with nothing to close)
				for (int i = 0; i < (int)V.size(); i++) {
					if (V[i].alive) {
						V[i].alive = false;
					}
				}
				break;
			}
			now = best;
			if (kind == 1) {
				const int a = ea, b = V[a].next;
				const Vector2 q = at(V[a], now);
				trace(a, now);
				trace(b, now);
				V[a].alive = V[b].alive = false;
				const int pa = V[a].prev, nb = V[b].next;
				if (pa == b) {
					continue; // (the last two)
				}
				const int x = add_vert(q, now, V[a].el, V[b].er);
				V[x].prev = pa;
				V[x].next = nb;
				V[pa].next = x;
				V[nb].prev = x;
				if (pa == nb) {
					// (two left: the loop ends at the next pass)
				}
			} else {
				// the reflex corner r splits side s-s.next in two: two loops
				const int r = ea, s = sb, s2 = V[s].next;
				trace(r, now);
				V[r].alive = false;
				const int side = V[s].er;
				const int x1 = add_vert(hit, now, V[r].el, side); // (r's left side, then the split side's far part)
				const int x2 = add_vert(hit, now, side, V[r].er); // (the split side's near part, then r's right side)
				const int rp = V[r].prev, rn = V[r].next;
				// loop 1: rp -> x1 -> s2 ... ; loop 2: s -> x2 -> rn ...
				V[x1].prev = rp;
				V[rp].next = x1;
				V[x1].next = s2;
				V[s2].prev = x1;
				V[x2].prev = s;
				V[s].next = x2;
				V[x2].next = rn;
				V[rn].prev = x2;
				// (the corners where the split side's new ends start: their position now, and they keep going)
				for (int y : { s, s2 }) {
					trace(y, now);
					V[y].p = at(V[y], now);
					V[y].t0 = now;
					V[y].start3 = lift(V[y].p, now);
				}
			}
		}
	}
};

// One face's outline: its side a -> b, then back along its skeleton pieces from b to a.
std::vector<Vector3> face_loop(const Vector3 &a, const Vector3 &b, const std::vector<std::pair<Vector3, Vector3>> &segs) {
	std::vector<Vector3> nodes;
	auto id = [&](const Vector3 &q) {
		for (size_t i = 0; i < nodes.size(); i++) {
			if (nodes[i].distance_to(q) < 1e-4f) {
				return (int)i;
			}
		}
		nodes.push_back(q);
		return (int)nodes.size() - 1;
	};
	const int ia = id(a), ib = id(b);
	std::map<int, std::vector<int>> adj;
	for (const auto &s : segs) {
		const int x = id(s.first), y = id(s.second);
		if (x != y) {
			adj[x].push_back(y);
			adj[y].push_back(x);
		}
	}
	// the path from b to a over the pieces (the shortest one by count: the face's border)
	std::map<int, int> from;
	std::vector<int> q = { ib };
	from[ib] = -1;
	for (size_t k = 0; k < q.size(); k++) {
		const int c = q[k];
		if (c == ia) {
			break;
		}
		for (int nb : adj[c]) {
			if (!from.count(nb)) {
				from[nb] = c;
				q.push_back(nb);
			}
		}
	}
	std::vector<Vector3> out = { a, b };
	if (!from.count(ia)) {
		return out; // (no path: just the side)
	}
	std::vector<int> path;
	for (int c = ia; c != -1; c = from[c]) {
		path.push_back(c);
	}
	// path: a ... b; the loop goes a, b, then b's next back toward a
	for (int i = (int)path.size() - 2; i >= 1; i--) {
		out.push_back(nodes[path[i]]);
	}
	return out;
}

} // namespace

void build_roof(const std::vector<Vector3> &p_outline, const std::vector<float> &values, Mesh &out, const std::vector<Vector2> *p_profile, ops::ProfileFrame *r_frame,
		const std::vector<int> *p_sides, std::vector<int> *r_gables) {
	out.clear();
	const std::vector<Param> &ps = params(ROOF);
	std::vector<float> v(ps.size());
	for (size_t i = 0; i < ps.size(); i++) {
		float x = i < values.size() ? values[i] : ps[i].def;
		x = std::max(ps[i].min, std::min(ps[i].max, x));
		v[i] = ps[i].is_int ? std::round(x) : x;
	}
	const int kind = (int)v[0]; // Hip, Gable, Shed, Flat
	const float pitch = v[1], overhang = v[2], thick = std::max(0.0f, v[3]);
	const int gable_sides = (int)v[4]; // Short, Long
	const int shed_side = (int)v[5];
	const bool gable_walls = v[6] > 0.5f;
	const float wall_t = v[7];
	const bool by_ridge = v.size() > 9 && (int)v[9] == 1;
	const float ridge_h = v.size() > 10 ? v[10] : 2.0f;
	const float flat_top = v.size() > 11 ? v[11] : 0.0f;
	const bool uv_slope = v.size() > 12 && v[12] > 0.5f;
	const float gable_eave = v.size() > 13 ? v[13] : 0.0f;
	float base_y = 0.0f;
	for (const Vector3 &q : p_outline) {
		base_y += q.y / (float)std::max<size_t>(1, p_outline.size());
	}
	std::vector<Vector2> poly;
	std::vector<int> from; // (the outline's point each one is)
	for (int i = 0; i < (int)p_outline.size(); i++) {
		const Vector2 p(p_outline[i].x, p_outline[i].z);
		if (poly.empty() || poly.back().distance_to(p) > 1e-4f) {
			poly.push_back(p);
			from.push_back(i);
		}
	}
	while (poly.size() > 2 && poly.front().distance_to(poly.back()) <= 1e-4f) {
		poly.pop_back();
		from.pop_back();
	}
	for (int pass = 0; pass < 2 && poly.size() > 3; pass++) {
		// (straight runs: one side)
		std::vector<Vector2> clean;
		std::vector<int> cf;
		const int n = (int)poly.size();
		for (int i = 0; i < n; i++) {
			const Vector2 a = poly[(i + n - 1) % n], b = poly[i], c = poly[(i + 1) % n];
			if (std::fabs(cross2((b - a).normalized(), (c - b).normalized())) > 1e-4f || (b - a).dot(c - b) < 0.0f) {
				clean.push_back(b);
				cf.push_back(from[i]);
			}
		}
		poly = clean;
		from = cf;
	}
	if (poly.size() < 3) {
		return;
	}
	const int m_out = (int)p_outline.size();
	// each side's type as asked: the first one given along the outline's sides it covers
	auto side_type = [&](int k) {
		if (!p_sides) {
			return 0;
		}
		const int a = from[k], b = from[(k + 1) % from.size()];
		for (int i = a, guard = 0; guard < m_out; i = (i + 1) % m_out, guard++) {
			if (i == b && guard > 0) {
				break;
			}
			if (i < (int)p_sides->size() && (*p_sides)[i] != 0) {
				return (*p_sides)[i];
			}
		}
		return 0;
	};
	std::vector<int> st(poly.size()), side_from(from);
	for (size_t k = 0; k < poly.size(); k++) {
		st[k] = side_type((int)k);
	}
	if (signed_area(poly) < 0.0f) {
		std::reverse(poly.begin(), poly.end()); // (positive in x, z: each side's inside on its left)
		// (side k now is the old side n - 2 - k, the other way)
		const int pn = (int)poly.size();
		std::vector<int> st2(pn), sf2(pn);
		for (int k = 0; k < pn; k++) {
			const int o = ((pn - 2 - k) % pn + pn) % pn;
			st2[k] = st[o];
			sf2[k] = side_from[o];
		}
		st = st2;
		side_from = sf2;
	}
	const int n = (int)poly.size();
	float slope = std::tan(std::max(0.5f, std::min(89.0f, pitch)) * 3.14159265f / 180.0f);
	const std::vector<Vector2> eave = overhang > 0.0f ? grow(poly, overhang) : poly;
	// the sides that are gables
	std::vector<char> gable(n, 0);
	if (kind == 1) {
		std::vector<float> len(n);
		for (int i = 0; i < n; i++) {
			len[i] = poly[i].distance_to(poly[(i + 1) % n]);
		}
		auto convex = [&](int i) {
			return cross2(poly[i] - poly[(i + n - 1) % n], poly[(i + 1) % n] - poly[i]) > 0.0f;
		};
		std::vector<int> g;
		for (int i = 0; i < n; i++) {
			const float l0 = len[(i + n - 1) % n], l2 = len[(i + 1) % n];
			const bool pick = gable_sides == 0 ? (len[i] <= l0 + 1e-4f && len[i] <= l2 + 1e-4f) : (len[i] >= l0 - 1e-4f && len[i] >= l2 - 1e-4f);
			if (convex(i) && convex((i + 1) % n) && pick) {
				g.push_back(i);
			}
		}
		if ((int)g.size() == n) {
			g.clear(); // (all alike, a square: every other one)
			for (int i = gable_sides == 0 ? 0 : 1; i < n; i += 2) {
				g.push_back(i);
			}
		}
		for (int i : g) {
			gable[i] = 1;
		}
	}
	if (kind == 0 || kind == 1) {
		for (int i = 0; i < n; i++) {
			if (st[i] == 1) {
				gable[i] = 0;
			} else if (st[i] == 2) {
				gable[i] = 1;
			}
		}
		bool all = true;
		for (int i = 0; i < n; i++) {
			all = all && gable[i];
		}
		if (all) {
			gable[0] = 0; // (something has to slope)
		}
	}
	if (r_gables) {
		r_gables->clear();
		for (int i = 0; i < n; i++) {
			if (gable[i]) {
				r_gables->push_back(side_from[i]);
			}
		}
	}
	// ---- the upper faces, in skeleton space (y: the height over the eaves)
	struct Top {
		std::vector<Vector3> pts;
		int side = -1;
	};
	std::vector<Top> tops;
	int shed_lo = 0;
	Vector2 shed_a, shed_in;
	if (kind == 3) {
		Top t;
		for (const Vector2 &q : eave) {
			t.pts.push_back(Vector3(q.x, 0.0f, q.y));
		}
		tops.push_back(t);
	} else if (kind == 2) {
		float best = -1.0f;
		for (int i = 0; i < n; i++) {
			const float l = eave[i].distance_to(eave[(i + 1) % n]);
			if (l > best + 1e-4f) {
				best = l;
				shed_lo = i;
			}
		}
		shed_lo = (shed_lo + shed_side) % n;
		shed_a = eave[shed_lo];
		const Vector2 d = (eave[(shed_lo + 1) % n] - shed_a).normalized();
		shed_in = Vector2(-d.y, d.x);
		if (by_ridge) {
			float far = 0.0f;
			for (const Vector2 &q : eave) {
				far = std::max(far, shed_in.dot(q - shed_a));
			}
			slope = std::max(0.01f, ridge_h) / std::max(1e-4f, far - overhang);
		}
		Top t;
		for (const Vector2 &q : eave) {
			t.pts.push_back(Vector3(q.x, std::max(0.0f, shed_in.dot(q - shed_a)) * slope, q.y));
		}
		tops.push_back(t);
	} else {
		Skeleton sk;
		sk.pts = eave;
		sk.slope = 1.0f; // (the heights by the slope below)
		sk.w.assign(n, 1.0f);
		for (int i = 0; i < n; i++) {
			sk.w[i] = gable[i] ? 0.0f : 1.0f;
		}
		sk.run();
		for (int i = 0; i < n; i++) {
			const Vector3 a(eave[i].x, 0.0f, eave[i].y), b(eave[(i + 1) % n].x, 0.0f, eave[(i + 1) % n].y);
			Top t;
			t.pts = face_loop(a, b, sk.segs[i]);
			t.side = i;
			if (t.pts.size() >= 3) {
				tops.push_back(t);
			}
		}
		if (by_ridge) {
			// (the ridge at its height over the walls' top: the eaves are lower by the overhang's share)
			float run = 0.0f;
			for (const Top &t : tops) {
				for (const Vector3 &q : t.pts) {
					run = std::max(run, q.y);
				}
			}
			const float inner = std::max(1e-4f, run - overhang);
			slope = std::max(0.01f, ridge_h) / inner;
		}
		for (Top &t : tops) {
			for (Vector3 &q : t.pts) {
				q.y *= slope;
			}
		}
	}
	const float drop = kind == 3 ? 0.0f : overhang * slope; // (the eaves lower than the walls' top)
	const float up_y = base_y - drop;
	// (the thickness across the slope: straight down it is more, the steeper the roof)
	const float cos_p = 1.0f / std::sqrt(1.0f + slope * slope);
	const float thick_v = kind == 3 ? thick : thick / std::max(0.05f, cos_p);
	const Vector3 down(0, -thick_v, 0);
	// ---- the mesh: every face turned the way it looks (up, down, out)
	std::map<std::pair<int64_t, std::pair<int64_t, int64_t>>, int> ids;
	auto vid = [&](const Vector3 &q) {
		const std::pair<int64_t, std::pair<int64_t, int64_t>> k = { std::llround(q.x * 1e4), { std::llround(q.y * 1e4), std::llround(q.z * 1e4) } };
		auto it = ids.find(k);
		if (it != ids.end()) {
			return it->second;
		}
		const int id = out.add_vertex(q);
		ids[k] = id;
		return id;
	};
	auto push = [&](const std::vector<Vector3> &p_pts, const Vector3 &p_want, int p_role) {
		std::vector<int> vs;
		for (const Vector3 &q : p_pts) {
			const int id = vid(q);
			if (vs.empty() || (vs.back() != id && vs.front() != id)) {
				vs.push_back(id);
			}
		}
		if (vs.size() < 3) {
			return;
		}
		Face f = make_face(vs);
		if (face_normal_raw(out, f).dot(p_want) < 0.0f) {
			f.reverse();
		}
		f.role = p_role;
		out.faces.push_back(f);
	};
	auto out_of = [&](const std::vector<Vector2> &P, int i) {
		const Vector2 d = (P[(i + 1) % P.size()] - P[i]).normalized();
		return Vector3(d.y, 0.0f, -d.x); // (right of the way: outward)
	};
	const Vector3 lift(0, up_y, 0);
	std::vector<float> eave_y(n, 0.0f); // (each eave corner's height: a shed's rise along it)
	// ---- the slopes' curve: the height over the eaves remapped (the slope curved, a mansard), the faces cut where it
	// bends so each piece stays flat
	float ymax = 0.0f;
	for (const Top &t : tops) {
		for (const Vector3 &q : t.pts) {
			ymax = std::max(ymax, q.y);
		}
	}
	std::vector<Vector2> curve;
	if (p_profile && p_profile->size() >= 2 && kind != 3 && ymax > 1e-5f) {
		for (const Vector2 &q : *p_profile) {
			curve.push_back(Vector2(std::max(0.0f, std::min(1.0f, q.x)), q.y));
		}
		std::stable_sort(curve.begin(), curve.end(), [](const Vector2 &a, const Vector2 &b) { return a.x < b.x; });
		if (curve.front().x > 1e-4f) {
			curve.insert(curve.begin(), Vector2(0, 0));
		}
		if (curve.back().x < 1.0f - 1e-4f) {
			curve.push_back(Vector2(1, curve.back().y));
		}
	}
	auto remap = [&](float y) {
		if (curve.empty()) {
			return y;
		}
		const float u = std::max(0.0f, std::min(1.0f, y / ymax));
		size_t i = 0;
		while (i + 2 < curve.size() && curve[i + 1].x < u) {
			i++;
		}
		const float dx = curve[i + 1].x - curve[i].x;
		const float f = dx > 1e-7f ? (u - curve[i].x) / dx : 0.0f;
		return (curve[i].y + (curve[i + 1].y - curve[i].y) * f) * ymax;
	};
	std::vector<float> levels; // (where it bends: the curve's points, in heights)
	for (size_t i = 1; i + 1 < curve.size(); i++) {
		levels.push_back(curve[i].x * ymax);
	}
	// flat above this height (over the walls' top: the eaves are lower by the drop)
	const float flat_at = flat_top > 1e-4f && kind != 3 ? flat_top + drop : 1e30f;
	if (flat_at < ymax - 1e-4f) {
		levels.push_back(flat_at);
		std::sort(levels.begin(), levels.end());
	}
	// a chain of points with more of them where it crosses the levels
	auto split_chain = [&](const std::vector<Vector3> &p_pts, bool p_closed) {
		std::vector<Vector3> o;
		const size_t m = p_pts.size();
		for (size_t k = 0; k < m; k++) {
			const Vector3 a = p_pts[k];
			o.push_back(a);
			if (!p_closed && k + 1 == m) {
				break;
			}
			const Vector3 b = p_pts[(k + 1) % m];
			std::vector<float> ts;
			for (float L : levels) {
				if ((a.y - L) * (b.y - L) < 0.0f) {
					ts.push_back((L - a.y) / (b.y - a.y));
				}
			}
			std::sort(ts.begin(), ts.end());
			for (float t : ts) {
				o.push_back(a.lerp(b, t));
			}
		}
		return o;
	};
	// a flat polygon cut into the pieces between the levels
	auto bands = [&](const std::vector<Vector3> &p_poly) {
		std::vector<std::vector<Vector3>> out_b;
		if (levels.empty()) {
			out_b.push_back(p_poly);
			return out_b;
		}
		std::vector<float> lv = { -1e30f };
		lv.insert(lv.end(), levels.begin(), levels.end());
		lv.push_back(1e30f);
		for (size_t b = 0; b + 1 < lv.size(); b++) {
			std::vector<Vector3> poly = p_poly;
			for (int side = 0; side < 2 && poly.size() >= 3; side++) {
				const float L = side == 0 ? lv[b] : lv[b + 1];
				std::vector<Vector3> res;
				for (size_t k = 0; k < poly.size(); k++) {
					const Vector3 &a = poly[k], &c = poly[(k + 1) % poly.size()];
					const float da = side == 0 ? a.y - L : L - a.y, dc = side == 0 ? c.y - L : L - c.y;
					if (da >= 0.0f) {
						res.push_back(a);
					}
					if ((da >= 0.0f) != (dc >= 0.0f)) {
						res.push_back(a.lerp(c, da / (da - dc)));
					}
				}
				poly.swap(res);
			}
			if (poly.size() >= 3) {
				out_b.push_back(poly);
			}
		}
		return out_b;
	};
	auto curved = [&](Vector3 q) {
		q.y = remap(std::min(q.y, flat_at));
		return q;
	};
	// a face's UVs along its slope: across along the side, up the slope as long as it is
	auto slope_uv = [&](int p_face, const Vector2 &p_along, const Vector2 &p_in) {
		Face &F = out.faces[p_face];
		F.ensure_attrs();
		for (int k = 0; k < F.size(); k++) {
			const Vector3 &q = out.pos[F.v[k]];
			const Vector2 h(q.x, q.z);
			F.uv[k] = Vector2(h.dot(p_along), -h.dot(p_in) / std::max(0.05f, cos_p));
		}
		F.manual_uv = true;
	};
	for (const Top &t : tops) {
		if (t.side >= 0 && gable[t.side]) {
			// a gable: no face of its own, a strip along its slope edges closing the roof (the verge)
			std::vector<Vector3> chain(t.pts.begin() + 1, t.pts.end()); // (b, its path, back to a)
			chain.push_back(t.pts[0]);
			chain = split_chain(chain, false);
			for (size_t k = 0; k + 1 < chain.size(); k++) {
				const Vector3 a = curved(chain[k]) + lift, b = curved(chain[k + 1]) + lift;
				push({ a, b, b + down, a + down }, out_of(eave, t.side), ROLE_BORDER);
			}
			continue;
		}
		for (const std::vector<Vector3> &piece : bands(t.pts)) {
			std::vector<Vector3> top, bot;
			bool flat = kind == 3;
			if (!flat && flat_at < 1e29f) {
				flat = true;
				for (const Vector3 &q : piece) {
					flat = flat && q.y >= flat_at - 1e-4f;
				}
			}
			for (const Vector3 &q : piece) {
				top.push_back(curved(q) + lift);
				bot.push_back(curved(q) + lift + (flat ? Vector3(0, -thick, 0) : down));
			}
			const size_t f0 = out.faces.size();
			push(top, Vector3(0, 1, 0), flat ? ROLE_TOP : ROLE_SLOPE);
			if (thick > 1e-5f) {
				push(bot, Vector3(0, -1, 0), ROLE_UNDERSIDE);
			}
			if (uv_slope && !flat && kind != 3) {
				Vector2 along, in;
				if (kind == 2) {
					in = shed_in;
					along = Vector2(in.y, -in.x);
				} else if (t.side >= 0) {
					along = (eave[(t.side + 1) % n] - eave[t.side]).normalized();
					in = Vector2(-along.y, along.x);
				}
				for (size_t fi = f0; fi < out.faces.size() && along != Vector2(); fi++) {
					slope_uv((int)fi, along, in);
				}
			}
		}
	}
	if (r_frame) {
		// the curve's place: across the first side that slopes, from its eave's middle in and up
		for (int i = 0; i < n; i++) {
			if (gable[i] || kind == 3) {
				continue;
			}
			const Vector2 mid = (eave[i] + eave[(i + 1) % n]) * 0.5f;
			Vector2 in = kind == 2 ? shed_in : Vector2(-(eave[(i + 1) % n] - eave[i]).normalized().y, (eave[(i + 1) % n] - eave[i]).normalized().x);
			if (kind == 2) {
				i = shed_lo;
			}
			const Vector2 m2 = kind == 2 ? (eave[shed_lo] + eave[(shed_lo + 1) % n]) * 0.5f : mid;
			r_frame->made = ymax > 1e-5f;
			r_frame->o = Vector3(m2.x, up_y, m2.y);
			r_frame->ax = Vector3(in.x, 0.0f, in.y) * (ymax / slope);
			r_frame->ay = Vector3(0.0f, ymax, 0.0f);
			break;
		}
	}
	if (kind == 2) {
		for (int i = 0; i < n; i++) {
			eave_y[i] = std::max(0.0f, shed_in.dot(eave[i] - shed_a)) * slope;
		}
	}
	if (thick > 1e-5f) {
		for (int i = 0; i < n; i++) {
			if (gable[i]) {
				continue; // (its verge reaches down to the eaves)
			}
			const Vector3 a(eave[i].x, up_y + eave_y[i], eave[i].y), b(eave[(i + 1) % n].x, up_y + eave_y[(i + 1) % n], eave[(i + 1) % n].y);
			push({ a, b, b + down, a + down }, out_of(eave, i), ROLE_SIDE);
		}
	}
	// ---- the gable walls: under a gable (or a shed's high sides) from the walls' top up to the roof's underside, on
	// the wall line, as thick as the walls
	if (gable_walls && (kind == 1 || kind == 2) && wall_t > 1e-4f) {
		std::vector<Top> walls;
		if (kind == 1) {
			Skeleton sk;
			sk.pts = poly;
			sk.slope = slope;
			sk.w.assign(n, 1.0f);
			for (int i = 0; i < n; i++) {
				sk.w[i] = gable[i] ? 0.0f : 1.0f;
			}
			sk.run();
			for (int i = 0; i < n; i++) {
				if (!gable[i]) {
					continue;
				}
				const Vector3 a(poly[i].x, 0.0f, poly[i].y), b(poly[(i + 1) % n].x, 0.0f, poly[(i + 1) % n].y);
				Top t;
				t.pts = face_loop(a, b, sk.segs[i]);
				t.side = i;
				walls.push_back(t);
			}
		} else {
			for (int i = 0; i < n; i++) {
				const Vector2 pa = poly[i], pb = poly[(i + 1) % n];
				const float ha = shed_in.dot(pa - shed_a) * slope - drop, hb = shed_in.dot(pb - shed_a) * slope - drop;
				if (ha < thick + 1e-3f && hb < thick + 1e-3f) {
					continue; // (the low side: nothing under it)
				}
				Top t;
				t.side = i;
				t.pts = { Vector3(pa.x, 0.0f, pa.y), Vector3(pb.x, 0.0f, pb.y), Vector3(pb.x, std::max(0.0f, hb), pb.y), Vector3(pa.x, std::max(0.0f, ha), pa.y) };
				walls.push_back(t);
			}
		}
		for (const Top &t : walls) {
			// (its slope edges lowered under the roof: the wall stops at the underside; its height over the eaves is the
			// roof's there, curved as the slopes are)
			std::vector<Vector3> lifted;
			for (const Vector3 &q : t.pts) {
				lifted.push_back(q.y < 1e-5f ? q : q + Vector3(0, drop, 0));
			}
			lifted = split_chain(lifted, true);
			std::vector<Vector3> outer;
			for (const Vector3 &q : lifted) {
				const bool bottom = q.y < 1e-5f;
				const Vector3 p(q.x, bottom ? base_y : std::max(base_y, up_y + remap(std::min(q.y, flat_at)) - (q.y >= flat_at - 1e-4f ? thick : thick_v)), q.z);
				if (outer.empty() || outer.back().distance_to(p) > 1e-5f) {
					outer.push_back(p);
				}
			}
			while (outer.size() > 2 && outer.front().distance_to(outer.back()) <= 1e-5f) {
				outer.pop_back();
			}
			if (outer.size() < 3) {
				continue;
			}
			const Vector3 o = out_of(poly, t.side);
			const Vector3 in = -o * wall_t;
			std::vector<Vector3> inner;
			for (const Vector3 &q : outer) {
				inner.push_back(q + in);
			}
			push(outer, o, ROLE_OUTER); // (the gable's face: a material of its own)
			push(inner, -o, ROLE_INNER);
			if (gable_eave > 1e-4f && kind == 1) {
				// a little roof along its foot: from the wall out, down at the pitch, as thick as the roof
				std::vector<Vector3> foot;
				for (const Vector3 &q : outer) {
					if (q.y <= base_y + 1e-4f) {
						foot.push_back(q);
					}
				}
				if (foot.size() >= 2) {
					const Vector3 p0 = foot[0], p1 = foot[1];
					const float rise = gable_eave * std::min(slope, 1.0f);
					const Vector3 hi(0, rise + thick_v, 0), outv = o * gable_eave;
					const std::vector<Vector3> top_q = { p0 + hi, p1 + hi, p1 + outv + Vector3(0, thick_v, 0), p0 + outv + Vector3(0, thick_v, 0) };
					const std::vector<Vector3> bot_q = { p0 + hi - Vector3(0, thick_v, 0), p1 + hi - Vector3(0, thick_v, 0), p1 + outv, p0 + outv };
					push(top_q, Vector3(0, 1, 0) + o, ROLE_SLOPE);
					push(bot_q, Vector3(0, -1, 0), ROLE_UNDERSIDE);
					push({ top_q[2], top_q[3], bot_q[3], bot_q[2] }, o, ROLE_SIDE);
					push({ top_q[0], top_q[3], bot_q[3], bot_q[0] }, (p0 - p1).normalized(), ROLE_SIDE);
					push({ top_q[1], top_q[2], bot_q[2], bot_q[1] }, (p1 - p0).normalized(), ROLE_SIDE);
				}
			}
			Vector3 mid;
			for (const Vector3 &q : outer) {
				mid += q / (float)outer.size();
			}
			for (size_t k = 0; k < outer.size(); k++) {
				const size_t k2 = (k + 1) % outer.size();
				const Vector3 e = outer[k2] - outer[k];
				if (e.length() < 1e-5f) {
					continue;
				}
				// (its edges' faces outward in its plane: away from its middle)
				Vector3 nrm = e.cross(o).normalized();
				if (nrm.dot((outer[k] + outer[k2]) * 0.5f - mid) < 0.0f) {
					nrm = -nrm;
				}
				push({ outer[k], outer[k2], inner[k2], inner[k] }, nrm, ROLE_BORDER);
			}
		}
	}
	if (v[8] > 0.5f) {
		for (Face &F : out.faces) {
			F.reverse();
		}
	}
}

} // namespace shapes
} // namespace hewn
