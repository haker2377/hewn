// Hewn — stairs along a path (stairs by points): a flight along each segment, a landing at
// each turn, the whole rise split into steps of about the step height (more steps on longer flights).
// Solid below (down to the ground) or a slab of a thickness; borders along the sides.
#include "hw_core.h"

#include <algorithm>
#include <cmath>

namespace hewn {
namespace shapes {

namespace {

// A polygon in a vertical plane (u along `du`, y up), counter-clockwise, extruded across `ds` from s0 to s1.
void extrude_profile(Mesh &out, const Vector3 &p_origin, const Vector3 &du, const Vector3 &ds, float s0, float s1, std::vector<Vector2> prof, bool p_treads) {
	std::vector<Vector2> clean;
	for (const Vector2 &q : prof) {
		if (clean.empty() || clean.back().distance_to(q) > 1e-5f) {
			clean.push_back(q);
		}
	}
	while (clean.size() > 1 && clean.front().distance_to(clean.back()) <= 1e-5f) {
		clean.pop_back();
	}
	if (clean.size() < 3) {
		return;
	}
	float area = 0.0f;
	for (size_t i = 0; i < clean.size(); i++) {
		area += clean[i].cross(clean[(i + 1) % clean.size()]);
	}
	if (area < 0.0f) {
		std::reverse(clean.begin(), clean.end());
	}
	const Vector3 up(0, 1, 0);
	auto P = [&](const Vector2 &q, float s) { return p_origin + du * q.x + up * q.y + ds * s; };
	std::vector<int> a, b;
	for (const Vector2 &q : clean) {
		a.push_back(out.add_vertex(P(q, s0)));
		b.push_back(out.add_vertex(P(q, s1)));
	}
	const int n = (int)clean.size();
	const size_t first = out.faces.size();
	// the caps (their winding is checked with the volume below)
	Face fa = make_face(a);
	fa.role = ROLE_SIDE;
	out.faces.push_back(fa);
	Face fb = make_face(std::vector<int>(b.rbegin(), b.rend()));
	fb.role = ROLE_SIDE;
	out.faces.push_back(fb);
	for (int i = 0; i < n; i++) {
		const int j = (i + 1) % n;
		Face f = make_face({ a[j], a[i], b[i], b[j] });
		// the role from the edge's direction: up = tread, down = underside, toward the climber = riser
		const Vector2 e = clean[j] - clean[i];
		const Vector2 nrm2(e.y, -e.x); // (outward of a counter-clockwise outline)
		if (!p_treads) {
			f.role = ROLE_BORDER;
		} else if (nrm2.y > 0.9f * nrm2.length()) {
			f.role = ROLE_TREAD;
		} else if (nrm2.y < -0.3f * nrm2.length()) {
			f.role = ROLE_UNDERSIDE;
		} else if (nrm2.x < 0.0f) {
			f.role = ROLE_RISER;
		} else {
			f.role = ROLE_BACK;
		}
		out.faces.push_back(f);
	}
	// outward: a positive volume
	float vol = 0.0f;
	for (size_t fi = first; fi < out.faces.size(); fi++) {
		const Face &F = out.faces[fi];
		for (int i = 1; i + 1 < F.size(); i++) {
			vol += out.pos[F.v[0]].dot(out.pos[F.v[i]].cross(out.pos[F.v[i + 1]]));
		}
	}
	if (vol < 0.0f) {
		for (size_t fi = first; fi < out.faces.size(); fi++) {
			out.faces[fi].reverse();
		}
	}
}

// A vertical prism: the outline (XZ, any winding) from y0 to y1.
void prism(Mesh &out, std::vector<Vector2> poly, float y0, float y1, int p_side_role) {
	if (poly.size() < 3 || y1 - y0 < 1e-5f) {
		return;
	}
	float area = 0.0f;
	for (size_t i = 0; i < poly.size(); i++) {
		area += poly[i].cross(poly[(i + 1) % poly.size()]);
	}
	if (area > 0.0f) {
		std::reverse(poly.begin(), poly.end()); // (x, z) clockwise: counter-clockwise seen from +Y
	}
	const int n = (int)poly.size();
	std::vector<int> lo, hi;
	for (const Vector2 &q : poly) {
		lo.push_back(out.add_vertex(Vector3(q.x, y0, q.y)));
		hi.push_back(out.add_vertex(Vector3(q.x, y1, q.y)));
	}
	const size_t first = out.faces.size();
	Face top = make_face(hi);
	top.role = ROLE_TREAD;
	out.faces.push_back(top);
	Face bot = make_face(std::vector<int>(lo.rbegin(), lo.rend()));
	bot.role = ROLE_BOTTOM;
	out.faces.push_back(bot);
	for (int i = 0; i < n; i++) {
		const int j = (i + 1) % n;
		Face f = make_face({ lo[i], lo[j], hi[j], hi[i] });
		f.role = p_side_role;
		out.faces.push_back(f);
	}
	// outward: a positive volume
	float vol = 0.0f;
	for (size_t fi = first; fi < out.faces.size(); fi++) {
		const Face &F = out.faces[fi];
		for (int i = 1; i + 1 < F.size(); i++) {
			vol += out.pos[F.v[0]].dot(out.pos[F.v[i]].cross(out.pos[F.v[i + 1]]));
		}
	}
	if (vol < 0.0f) {
		for (size_t fi = first; fi < out.faces.size(); fi++) {
			out.faces[fi].reverse();
		}
	}
}

// The convex hull of points (Andrew's monotone chain).
std::vector<Vector2> hull(std::vector<Vector2> p) {
	std::sort(p.begin(), p.end(), [](const Vector2 &a, const Vector2 &b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
	p.erase(std::unique(p.begin(), p.end(), [](const Vector2 &a, const Vector2 &b) { return a.distance_to(b) < 1e-5f; }), p.end());
	if (p.size() < 3) {
		return p;
	}
	std::vector<Vector2> h(2 * p.size());
	size_t k = 0;
	for (size_t i = 0; i < p.size(); i++) {
		while (k >= 2 && (h[k - 1] - h[k - 2]).cross(p[i] - h[k - 2]) <= 0.0f) {
			k--;
		}
		h[k++] = p[i];
	}
	for (size_t i = p.size() - 1, t = k + 1; i > 0; i--) {
		while (k >= t && (h[k - 1] - h[k - 2]).cross(p[i - 1] - h[k - 2]) <= 0.0f) {
			k--;
		}
		h[k++] = p[i - 1];
	}
	h.resize(k - 1);
	return h;
}

// Stairs along a path with its corners rounded (corner_radius): one flight all the way, the steps fanning round
// the bends (no landings), each a block between the two sides of the rounded path.
void build_rounded_stairs(const std::vector<Vector3> &pts, const std::vector<float> &ys, bool by_points, float base, const std::vector<float> &v, Mesh &out, bool p_ramps) {
	const float W = v[0], H = v[1], step_h = v[2];
	const bool solid = (int)v[3] == 0;
	const float thick = v[4];
	const int border = (int)v[5];
	const float bw = v[6], bh = v[7];
	const bool by_count = (int)v[10] == 1;
	const int count = std::max(1, (int)v[11]);
	const float radius = v[15];
	const int arc_n = std::max(1, (int)(v.size() > 16 ? v[16] : 8.0f));
	const float half = W * 0.5f;
	auto xz = [](const Vector3 &q) { return Vector2(q.x, q.z); };
	// the rounded path, and for each point the level it is at (by the points: theirs, the arcs at their corner's)
	std::vector<Vector2> R;
	std::vector<int> anchor_of(pts.size(), 0); // (the rounded point standing for each point of the path)
	R.push_back(xz(pts[0]));
	const int n = (int)pts.size();
	std::vector<float> used(n, 0.0f); // (how much of each segment's start a rounded corner took)
	for (int i = 1; i + 1 < n; i++) {
		const Vector2 c = xz(pts[i]);
		const Vector2 a = xz(pts[i - 1]), b = xz(pts[i + 1]);
		const Vector2 din = (c - a).normalized(), dout = (b - c).normalized();
		const float cosang = std::max(-1.0f, std::min(1.0f, din.dot(dout)));
		const float th = std::acos(cosang); // (the turn)
		if (th < 1e-3f) {
			anchor_of[i] = (int)R.size();
			R.push_back(c);
			continue;
		}
		const float len_in = c.distance_to(a) - used[i - 1], len_out = b.distance_to(c) * 0.5f;
		float t = radius * std::tan(th * 0.5f);
		t = std::min(t, std::min(std::max(0.0f, len_in), len_out));
		used[i] = t;
		const float r = t / std::max(1e-6f, std::tan(th * 0.5f));
		const Vector2 p0 = c - din * t;
		const float side = din.cross(dout) > 0.0f ? 1.0f : -1.0f;
		const Vector2 nrm = Vector2(-din.y, din.x) * side; // (toward the center)
		const Vector2 ctr = p0 + nrm * r;
		const int k = std::max(1, (int)std::ceil(arc_n * th / (3.14159265f * 0.5f)));
		const Vector2 s0 = p0 - ctr;
		for (int j = 0; j <= k; j++) {
			const float ang = side * th * (float)j / (float)k;
			const float cs = std::cos(ang), sn = std::sin(ang);
			const Vector2 q = ctr + Vector2(s0.x * cs - s0.y * sn, s0.x * sn + s0.y * cs);
			if (j == k / 2) {
				anchor_of[i] = (int)R.size();
			}
			if (R.back().distance_to(q) > 1e-5f) {
				R.push_back(q);
			} else if (j == k / 2) {
				anchor_of[i] = (int)R.size() - 1;
			}
		}
	}
	if (R.back().distance_to(xz(pts.back())) > 1e-5f) {
		R.push_back(xz(pts.back()));
	}
	anchor_of[n - 1] = (int)R.size() - 1;
	const int m = (int)R.size();
	std::vector<float> S(m, 0.0f);
	for (int i = 1; i < m; i++) {
		S[i] = S[i - 1] + R[i].distance_to(R[i - 1]);
	}
	const float L = std::max(1e-4f, S.back());
	// the level along the path: by the points (between their rounded stand-ins), or the height over the length
	std::vector<float> lvl(m, 0.0f);
	if (by_points) {
		for (int i = 0; i + 1 < n; i++) {
			const int a = anchor_of[i], b = std::max(anchor_of[i] + 1, anchor_of[i + 1]);
			for (int q = a; q <= b && q < m; q++) {
				const float f = S[b] - S[a] > 1e-6f ? (S[q] - S[a]) / (S[b] - S[a]) : 1.0f;
				lvl[q] = (ys[i] - base) + ((ys[i + 1] - base) - (ys[i] - base)) * f;
			}
		}
	} else {
		for (int q = 0; q < m; q++) {
			lvl[q] = H * S[q] / L;
		}
	}
	const float l0 = lvl[0], l1 = lvl[m - 1], rise = l1 - l0;
	const int N = by_count ? count : std::max(1, (int)std::round(std::fabs(rise) / std::max(0.01f, step_h)));
	const float lift_y = by_points ? base : 0.0f;
	// a point at a length along it, with its sideways
	auto at = [&](float s, Vector2 &r_pos, Vector2 &r_side) {
		s = std::max(0.0f, std::min(L, s));
		int i = 0;
		while (i + 2 < m && S[i + 1] < s) {
			i++;
		}
		const float f = S[i + 1] - S[i] > 1e-7f ? (s - S[i]) / (S[i + 1] - S[i]) : 0.0f;
		r_pos = R[i].lerp(R[i + 1], f);
		const Vector2 d = (R[i + 1] - R[i]).normalized();
		r_side = Vector2(-d.y, d.x);
	};
	auto side_at_point = [&](int i) {
		// (a corner of the rounded path: between its two segments' sideways, as long as it takes to keep the width)
		const Vector2 d0 = i > 0 ? (R[i] - R[i - 1]).normalized() : (R[1] - R[0]).normalized();
		const Vector2 d1 = i + 1 < m ? (R[i + 1] - R[i]).normalized() : d0;
		const Vector2 s0(-d0.y, d0.x), s1(-d1.y, d1.x);
		Vector2 s = (s0 + s1);
		s = s.length() > 1e-6f ? s.normalized() : s0;
		return s / std::max(0.2f, s.dot(s0));
	};
	for (int k = 0; k < N; k++) {
		const float sa = L * (float)k / N, sb = L * (float)(k + 1) / N;
		const float y = l0 + rise * (float)(k + (rise >= 0.0f ? 1 : 0)) / (float)N;
		// the outline of the step: its left side forward, its right side back
		std::vector<Vector2> left, right;
		Vector2 p, sd;
		at(sa, p, sd);
		left.push_back(p + sd * half);
		right.push_back(p - sd * half);
		for (int i = 0; i < m; i++) {
			if (S[i] > sa + 1e-5f && S[i] < sb - 1e-5f) {
				const Vector2 s2 = side_at_point(i);
				left.push_back(R[i] + s2 * half);
				right.push_back(R[i] - s2 * half);
			}
		}
		at(sb, p, sd);
		left.push_back(p + sd * half);
		right.push_back(p - sd * half);
		std::vector<Vector2> poly = left;
		for (int i = (int)right.size() - 1; i >= 0; i--) {
			poly.push_back(right[i]);
		}
		const float top_y = p_ramps ? l0 + rise * (sb / L) : y;
		const float lo = solid ? 0.0f : std::max(0.0f, top_y - thick);
		prism(out, poly, lo + lift_y, top_y + lift_y, ROLE_RISER);
		for (int sdx = 0; sdx < 2; sdx++) {
			if (!(border == 3 || border == sdx + 1)) {
				continue;
			}
			const std::vector<Vector2> &edge = sdx == 0 ? left : right;
			std::vector<Vector2> strip = edge;
			for (int i = (int)edge.size() - 1; i >= 0; i--) {
				// (outward by the border's width, beside the step)
				const Vector2 out_dir = (edge[i] - (sdx == 0 ? right[i] : left[i])).normalized();
				strip.push_back(edge[i] + out_dir * bw);
			}
			prism(out, strip, lo + lift_y, top_y + bh + lift_y, ROLE_BORDER);
		}
	}
}

} // namespace

void build_path_stairs(const std::vector<Vector3> &p_path, const std::vector<float> &values, Mesh &out, bool p_ramps) {
	out.clear();
	const std::vector<Param> &ps = params(PATH_STAIRS);
	std::vector<float> v(ps.size());
	for (size_t i = 0; i < ps.size(); i++) {
		float x = i < values.size() ? values[i] : ps[i].def;
		x = std::max(ps[i].min, std::min(ps[i].max, x));
		v[i] = ps[i].is_int ? std::round(x) : x;
	}
	const float W = v[0], H = v[1], step_h = v[2];
	const bool solid = (int)v[3] == 0;
	const float thick = v[4];
	const int border = (int)v[5];
	const float bw = v[6], bh = v[7];
	const bool by_count = (int)v[10] == 1; // (else about the step height)
	const int count = std::max(1, (int)v[11]);
	const bool landing_borders = v[12] > 0.5f;
	// The points' heights: when they differ, each flight climbs (or goes down) from its point's height to the
	// next one's, the steps about the step height; on one level the whole Height is split along the path.
	std::vector<Vector3> pts;
	std::vector<float> ys;
	for (const Vector3 &p : p_path) {
		const Vector3 q(p.x, 0.0f, p.z);
		if (pts.empty() || pts.back().distance_to(q) > 1e-4f) {
			pts.push_back(q);
			ys.push_back(p.y);
		}
	}
	if (pts.size() < 2) {
		return;
	}
	float base = ys[0], top = ys[0];
	for (float y : ys) {
		base = std::min(base, y);
		top = std::max(top, y);
	}
	const bool by_points = top - base > 1e-3f;
	const int segs = (int)pts.size() - 1;
	const float radius = v.size() > 15 ? v[15] : 0.0f;
	if (radius > 1e-4f && segs >= 2) {
		build_rounded_stairs(pts, ys, by_points, base, v, out, p_ramps);
		if (v[13] > 0.5f && !p_ramps) {
			for (Face &F : out.faces) {
				F.reverse();
			}
		}
		return;
	}
	const float half = W * 0.5f;
	// flights: shortened by half a width at the landings
	std::vector<Vector3> fs(segs), fe(segs), dir(segs);
	std::vector<float> len(segs);
	float total = 0.0f;
	for (int s = 0; s < segs; s++) {
		dir[s] = (pts[s + 1] - pts[s]).normalized();
		fs[s] = pts[s] + (s > 0 ? dir[s] * half : Vector3());
		fe[s] = pts[s + 1] - (s < segs - 1 ? dir[s] * half : Vector3());
		len[s] = std::max(0.01f, (fe[s] - fs[s]).dot(dir[s]));
		total += len[s];
	}
	// the level of each point above the lowest one, and the steps of each flight
	std::vector<float> lv(pts.size());
	std::vector<int> steps(segs);
	if (by_points) {
		for (size_t i = 0; i < pts.size(); i++) {
			lv[i] = ys[i] - base;
		}
		for (int s = 0; s < segs; s++) {
			const float rise = std::fabs(lv[s + 1] - lv[s]);
			steps[s] = rise < 1e-3f ? 0 : by_count ? count : std::max(1, (int)std::round(rise / std::max(0.01f, step_h)));
		}
	} else {
		const int want = std::max(segs, (int)std::round(H / std::max(0.01f, step_h)));
		int sum = 0;
		for (int s = 0; s < segs; s++) {
			steps[s] = by_count ? count : std::max(1, (int)std::round(want * len[s] / total)); // (count: on each flight)
			sum += steps[s];
		}
		const float r = H / (float)sum; // (every riser the same)
		lv[0] = 0.0f;
		for (int s = 0; s < segs; s++) {
			lv[s + 1] = lv[s] + steps[s] * r;
		}
	}
	const Vector3 up(0, 1, 0);
	const Vector3 lift(0.0f, by_points ? base : 0.0f, 0.0f);
	for (int s = 0; s < segs; s++) {
		const int n = steps[s];
		// a flight going down is built climbing from its far end
		const bool down = lv[s + 1] < lv[s];
		const Vector3 d = down ? -dir[s] : dir[s];
		const Vector3 o = (down ? fe[s] : fs[s]) + lift;
		const float y0 = std::min(lv[s], lv[s + 1]), y1 = std::max(lv[s], lv[s + 1]);
		const float r = n > 0 ? (y1 - y0) / (float)n : 0.0f;
		const float t = len[s] / (float)std::max(1, n);
		std::vector<Vector2> prof;
		// the steps (up the risers, along the treads), then back along the underside
		prof.push_back(Vector2(0.0f, y0));
		if (n == 0) {
			prof.push_back(Vector2(len[s], y0)); // (level: a walkway)
		}
		if (p_ramps && n > 0) {
			// (the smooth collision: a slope from the floor at the first riser to the last nosing)
			prof.push_back(Vector2(std::max((n - 1) * t, len[s] * 0.5f), y1));
			prof.push_back(Vector2(len[s], y1));
		} else {
			for (int k = 0; k < n; k++) {
				prof.push_back(Vector2(k * t, y0 + (k + 1) * r));
				prof.push_back(Vector2((k + 1) * t, y0 + (k + 1) * r));
			}
		}
		if (solid) {
			prof.push_back(Vector2(len[s], 0.0f));
			prof.push_back(Vector2(0.0f, 0.0f));
		} else if (n == 0) {
			prof.push_back(Vector2(len[s], std::max(0.0f, y0 - thick)));
			prof.push_back(Vector2(0.0f, std::max(0.0f, y0 - thick)));
		} else {
			// a slab under the inner corners: y(u) = y0 - thick + u * r / t, not below the ground
			const float yl = y1 - thick;
			prof.push_back(Vector2(len[s], std::max(0.0f, yl)));
			const float yb = y0 - thick;
			if (yb < 0.0f) {
				const float u0 = std::min(len[s], -yb * t / r);
				prof.push_back(Vector2(u0, 0.0f));
				prof.push_back(Vector2(0.0f, 0.0f));
			} else {
				prof.push_back(Vector2(0.0f, yb));
			}
		}
		const Vector3 side = up.cross(d).normalized(); // (left of the climb)
		extrude_profile(out, o, d, side, -half, half, prof, true);
		// borders along the sides, following the nosings
		for (int sd = 0; sd < 2; sd++) {
			if (!(border == 3 || border == sd + 1)) {
				continue;
			}
			// (left / right of the path's direction, also on a flight built from its far end)
			const bool left = (sd == 0) != down;
			std::vector<Vector2> bp;
			const float lo0 = solid ? 0.0f : std::max(0.0f, y0 - thick), lo1 = solid ? 0.0f : std::max(0.0f, y1 - thick);
			bp.push_back(Vector2(0.0f, lo0));
			bp.push_back(Vector2(len[s], lo1));
			bp.push_back(Vector2(len[s], y1 + bh));
			bp.push_back(Vector2(0.0f, y0 + r + bh));
			const float s0 = left ? half : -half - bw, s1 = left ? half + bw : -half;
			extrude_profile(out, o, d, side, s0, s1, bp, false);
		}
		// a landing at the turn: the ends of both flights and the outer corner between them, whatever the
		// angle (the flights meet it edge to edge)
		if (s < segs - 1) {
			const float y = lv[s + 1];
			const Vector3 c = pts[s + 1] + lift;
			const float lo = solid ? 0.0f : std::max(0.0f, y - thick);
			const Vector3 din = dir[s], dout = dir[s + 1];
			const Vector3 sin_ = up.cross(din).normalized(), sout = up.cross(dout).normalized();
			auto xz = [](const Vector3 &q) { return Vector2(q.x, q.z); };
			const Vector3 in_l = c - din * half + sin_ * half, in_r = c - din * half - sin_ * half;
			const Vector3 out_l = c + dout * half + sout * half, out_r = c + dout * half - sout * half;
			std::vector<Vector2> pts2 = { xz(in_l), xz(in_r), xz(out_l), xz(out_r) };
			// the outer side's lines meet at the corner (turning left: the right side is outside)
			const float turn = din.cross(dout).y;
			const bool left_turn = turn > 0.0f;
			const Vector3 a0 = left_turn ? in_r : in_l, a1 = left_turn ? out_r : out_l;
			const Vector2 p0 = xz(a0), d0 = Vector2(din.x, din.z), p1 = xz(a1), d1 = Vector2(dout.x, dout.z);
			const float den = d0.cross(d1);
			if (std::fabs(den) > 1e-4f) {
				const float t0 = (p1 - p0).cross(d1) / den;
				if (t0 > 0.0f && t0 < W * 4.0f) {
					pts2.push_back(p0 + d0 * t0);
				}
			}
			const std::vector<Vector2> lp = hull(pts2);
			prism(out, lp, lo, y, ROLE_SIDE);
			if (landing_borders) {
				// borders on its free edges (not where the flights come in / go on), outward
				const Vector2 hc = xz(c);
				for (size_t i = 0; i < lp.size(); i++) {
					const Vector2 e0 = lp[i], e1 = lp[(i + 1) % lp.size()];
					const Vector2 mid = (e0 + e1) * 0.5f;
					auto near_seg = [&](const Vector3 &q0, const Vector3 &q1) {
						const Vector2 m2 = (xz(q0) + xz(q1)) * 0.5f;
						return mid.distance_to(m2) < half * 0.25f;
					};
					if (near_seg(in_l, in_r) || near_seg(out_l, out_r) || e0.distance_to(e1) < 1e-3f) {
						continue;
					}
					Vector2 nrm(e1.y - e0.y, -(e1.x - e0.x));
					nrm = nrm.normalized();
					if (nrm.dot(mid - hc) < 0.0f) {
						nrm = -nrm;
					}
					prism(out, { e0, e1, e1 + nrm * bw, e0 + nrm * bw }, lo, y + bh, ROLE_BORDER);
				}
			}
		}
	}
	if (v[13] > 0.5f && !p_ramps) { // (flip_normals; the ramps of the collision never)
		for (Face &F : out.faces) {
			F.reverse();
		}
	}
}

} // namespace shapes
} // namespace hewn
