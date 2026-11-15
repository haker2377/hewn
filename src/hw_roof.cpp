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

void build_roof(const std::vector<Vector3> &p_outline, const std::vector<float> &values, Mesh &out) {
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
	float base_y = 0.0f;
	for (const Vector3 &q : p_outline) {
		base_y += q.y / (float)std::max<size_t>(1, p_outline.size());
	}
	std::vector<Vector2> poly;
	for (const Vector3 &q : p_outline) {
		const Vector2 p(q.x, q.z);
		if (poly.empty() || poly.back().distance_to(p) > 1e-4f) {
			poly.push_back(p);
		}
	}
	while (poly.size() > 2 && poly.front().distance_to(poly.back()) <= 1e-4f) {
		poly.pop_back();
	}
	for (int pass = 0; pass < 2 && poly.size() > 3; pass++) {
		// (straight runs: one side)
		std::vector<Vector2> clean;
		const int n = (int)poly.size();
		for (int i = 0; i < n; i++) {
			const Vector2 a = poly[(i + n - 1) % n], b = poly[i], c = poly[(i + 1) % n];
			if (std::fabs(cross2((b - a).normalized(), (c - b).normalized())) > 1e-4f || (b - a).dot(c - b) < 0.0f) {
				clean.push_back(b);
			}
		}
		poly = clean;
	}
	if (poly.size() < 3) {
		return;
	}
	if (signed_area(poly) < 0.0f) {
		std::reverse(poly.begin(), poly.end()); // (positive in x, z: each side's inside on its left)
	}
	const int n = (int)poly.size();
	const float slope = std::tan(std::max(0.5f, std::min(89.0f, pitch)) * 3.14159265f / 180.0f);
	const std::vector<Vector2> eave = overhang > 0.0f ? grow(poly, overhang) : poly;
	const float drop = kind == 3 ? 0.0f : overhang * slope; // (the eaves lower than the walls' top)
	const float up_y = base_y - drop;
	const Vector3 down(0, -thick, 0);
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
		Top t;
		for (const Vector2 &q : eave) {
			t.pts.push_back(Vector3(q.x, std::max(0.0f, shed_in.dot(q - shed_a)) * slope, q.y));
		}
		tops.push_back(t);
	} else {
		Skeleton sk;
		sk.pts = eave;
		sk.slope = slope;
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
	}
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
	for (const Top &t : tops) {
		if (t.side >= 0 && gable[t.side]) {
			// a gable: no face of its own, a strip along its slope edges closing the roof (the verge)
			const size_t m = t.pts.size();
			for (size_t k = 1; k < m; k++) {
				const Vector3 a = t.pts[k] + lift, b = t.pts[(k + 1) % m] + lift;
				push({ a, b, b + down, a + down }, out_of(eave, t.side), ROLE_BORDER);
			}
			continue;
		}
		std::vector<Vector3> top, bot;
		for (const Vector3 &q : t.pts) {
			top.push_back(q + lift);
			bot.push_back(q + lift + down);
		}
		push(top, Vector3(0, 1, 0), kind == 3 ? ROLE_TOP : ROLE_SLOPE);
		if (thick > 1e-5f) {
			push(bot, Vector3(0, -1, 0), ROLE_UNDERSIDE);
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
			// (its slope edges lowered under the roof: the wall stops at the underside)
			std::vector<Vector3> outer;
			for (const Vector3 &q : t.pts) {
				const bool bottom = q.y < 1e-5f;
				const Vector3 p(q.x, base_y + (bottom ? 0.0f : std::max(0.0f, q.y - thick)), q.z);
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
			push(outer, o, ROLE_SIDE);
			push(inner, -o, ROLE_INNER);
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
