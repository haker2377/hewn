// Hewn curves: sampling (poly, Bezier, NURBS), frames, the geometry along them; the HewnCurve3D node.
#include "hw_curve.h"
#include "hw_game_tags.h"
#include "hw_overlay3d.h"
#include "hewn_mesh.h"
#include "hw_path_place.h"
#include "hw_noise.h"

#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/convex_polygon_shape3d.hpp>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/concave_polygon_shape3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <map>
#include <array>
#include <cmath>

using namespace godot;

namespace hewn {
namespace curves {

namespace {

const float PI_F = 3.14159265358979f;

Vector3 bezier(const Vector3 &a, const Vector3 &b, const Vector3 &c, const Vector3 &d, float t) {
	const float u = 1.0f - t;
	return a * (u * u * u) + b * (3.0f * u * u * t) + c * (3.0f * u * t * t) + d * (t * t * t);
}

// de Boor: a point of a (rational) B-spline of order k with the knots, its control values (x, y, z, tilt, radius) and
// weights
void deboor(int k, const std::vector<float> &knots, const std::vector<std::array<float, 6>> &ctrl, float t, std::array<float, 6> &r_out) {
	const int n = (int)ctrl.size();
	int span = k - 1;
	while (span + 1 < n && t >= knots[span + 1]) {
		span++;
	}
	std::vector<std::array<float, 6>> d(k);
	for (int j = 0; j < k; j++) {
		const int idx = span - k + 1 + j;
		d[j] = ctrl[CLAMP(idx, 0, n - 1)];
	}
	for (int r = 1; r < k; r++) {
		for (int j = k - 1; j >= r; j--) {
			const int i = span - k + 1 + j;
			const float lo = knots[i], hi = knots[i + k - r];
			const float a = hi - lo > 1e-9f ? (t - lo) / (hi - lo) : 0.0f;
			for (int c = 0; c < 6; c++) {
				d[j][c] = d[j - 1][c] * (1.0f - a) + d[j][c] * a;
			}
		}
	}
	r_out = d[k - 1];
}

} // namespace

void sample(const Spline &s, int res, std::vector<Sample> &r_out) {
	r_out.clear();
	const int n = (int)s.pts.size();
	if (n == 0) {
		return;
	}
	res = std::max(1, res);
	if (s.type == POLY || n == 1) {
		for (int i = 0; i < n; i++) {
			r_out.push_back({ s.pts[i].pos, s.pts[i].tilt, s.pts[i].radius, (float)i });
		}
		return;
	}
	if (s.type == BEZIER) {
		const int segs = s.segments();
		for (int k = 0; k < segs; k++) {
			const CPoint &a = s.pts[k], &b = s.pts[(k + 1) % n];
			const bool straight = a.out.length_squared() < 1e-12f && b.in.length_squared() < 1e-12f;
			const int steps = straight ? 1 : res;
			for (int i = 0; i < steps; i++) {
				const float t = (float)i / (float)steps;
				r_out.push_back({ bezier(a.pos, a.pos + a.out, b.pos + b.in, b.pos, t), a.tilt + (b.tilt - a.tilt) * t, a.radius + (b.radius - a.radius) * t, (float)k + t });
			}
		}
		if (!s.cyclic) {
			r_out.push_back({ s.pts[n - 1].pos, s.pts[n - 1].tilt, s.pts[n - 1].radius, (float)(n - 1) });
		}
		return;
	}
	// NURBS
	const int k = std::max(2, std::min(s.order, s.cyclic ? n : n));
	std::vector<std::array<float, 6>> ctrl;
	for (const CPoint &p : s.pts) {
		const float w = std::max(1e-4f, p.weight);
		ctrl.push_back({ p.pos.x * w, p.pos.y * w, p.pos.z * w, p.tilt * w, p.radius * w, w });
	}
	if (s.cyclic) {
		for (int i = 0; i < k - 1; i++) {
			ctrl.push_back(ctrl[i]);
		}
	}
	const int m = (int)ctrl.size();
	std::vector<float> knots(m + k);
	float t0, t1;
	if (s.endpoint && !s.cyclic) {
		for (int i = 0; i < m + k; i++) {
			knots[i] = (float)CLAMP(i - (k - 1), 0, m - k + 1);
		}
		t0 = 0.0f;
		t1 = (float)(m - k + 1);
	} else {
		for (int i = 0; i < m + k; i++) {
			knots[i] = (float)i;
		}
		t0 = (float)(k - 1);
		t1 = (float)m;
	}
	const int segs = s.cyclic ? n : std::max(1, m - k + 1);
	const int total = segs * res;
	for (int i = 0; i <= total; i++) {
		if (s.cyclic && i == total) {
			break;
		}
		float t = t0 + (t1 - t0) * (float)i / (float)total;
		if (i == total) {
			t = t1 - 1e-5f;
		}
		std::array<float, 6> v;
		deboor(k, knots, ctrl, t, v);
		const float w = std::max(1e-6f, v[5]);
		r_out.push_back({ Vector3(v[0], v[1], v[2]) / w, v[3] / w, v[4] / w, (float)(n - 1) * (float)i / (float)total });
	}
}

void recalc_handles(Spline &s, bool p_all) {
	if (s.type != BEZIER) {
		return;
	}
	const int n = (int)s.pts.size();
	for (int i = 0; i < n; i++) {
		CPoint &p = s.pts[i];
		const bool has_prev = i > 0 || (s.cyclic && n > 2), has_next = i + 1 < n || (s.cyclic && n > 2);
		const Vector3 prev = has_prev ? s.pts[(i - 1 + n) % n].pos : p.pos, next = has_next ? s.pts[(i + 1) % n].pos : p.pos;
		const int hl = p_all ? H_AUTO : p.hl, hr = p_all ? H_AUTO : p.hr;
		Vector3 dir;
		if (has_prev && has_next) {
			dir = (next - prev).normalized();
		} else if (has_next) {
			dir = (next - p.pos).normalized();
		} else if (has_prev) {
			dir = (p.pos - prev).normalized();
		}
		const float k = 0.390464f; // (round: four points make a near circle)
		if (hl == H_AUTO) {
			p.in = has_prev ? -dir * (p.pos.distance_to(prev) * (has_next ? k : 1.0f / 3.0f)) : Vector3();
		} else if (hl == H_VECTOR) {
			p.in = has_prev ? (prev - p.pos) / 3.0f : Vector3();
		}
		if (hr == H_AUTO) {
			p.out = has_next ? dir * (p.pos.distance_to(next) * (has_prev ? k : 1.0f / 3.0f)) : Vector3();
		} else if (hr == H_VECTOR) {
			p.out = has_next ? (next - p.pos) / 3.0f : Vector3();
		}
		if (hl == H_ALIGNED && hr != H_FREE && p.out.length_squared() > 1e-12f) {
			p.in = -p.out.normalized() * p.in.length();
		} else if (hr == H_ALIGNED && hl != H_FREE && p.in.length_squared() > 1e-12f) {
			p.out = -p.in.normalized() * p.out.length();
		}
	}
}

void frames(const std::vector<Sample> &smp, bool cyclic, bool p_flat, std::vector<Vector3> &r_t, std::vector<Vector3> &r_n) {
	const int n = (int)smp.size();
	r_t.assign(n, Vector3(1, 0, 0));
	r_n.assign(n, Vector3(0, 1, 0));
	if (n < 2) {
		return;
	}
	for (int i = 0; i < n; i++) {
		const int a = cyclic ? (i - 1 + n) % n : std::max(0, i - 1), b = cyclic ? (i + 1) % n : std::min(n - 1, i + 1);
		Vector3 t = smp[b].p - smp[a].p;
		if (t.length_squared() < 1e-14f) {
			t = i > 0 ? r_t[i - 1] : Vector3(1, 0, 0);
		}
		r_t[i] = t.normalized();
	}
	if (p_flat) {
		for (int i = 0; i < n; i++) {
			Vector3 nn = Vector3(0, 1, 0) - r_t[i] * r_t[i].y;
			r_n[i] = nn.length_squared() > 1e-10f ? nn.normalized() : Vector3(0, 1, 0);
		}
	} else {
		// least twist: the normal carried along (turned as the tangent turns), starting as near up as it can
		const Vector3 up(0, 1, 0);
		Vector3 n0 = up - r_t[0] * r_t[0].dot(up);
		if (n0.length_squared() < 1e-6f) {
			n0 = Vector3(1, 0, 0) - r_t[0] * r_t[0].x;
		}
		r_n[0] = n0.normalized();
		for (int i = 1; i < n; i++) {
			const Vector3 ax = r_t[i - 1].cross(r_t[i]);
			Vector3 nn = r_n[i - 1];
			if (ax.length_squared() > 1e-12f) {
				const float ang = std::atan2(ax.length(), r_t[i - 1].dot(r_t[i]));
				nn = Basis(ax.normalized(), ang).xform(nn);
			}
			nn -= r_t[i] * r_t[i].dot(nn);
			r_n[i] = nn.length_squared() > 1e-12f ? nn.normalized() : r_n[i - 1];
		}
		if (cyclic) {
			// (round the loop the normal comes back turned: that turn spread along it)
			const Vector3 ax = r_t[n - 1].cross(r_t[0]);
			Vector3 back = r_n[n - 1];
			if (ax.length_squared() > 1e-12f) {
				back = Basis(ax.normalized(), std::atan2(ax.length(), r_t[n - 1].dot(r_t[0]))).xform(back);
			}
			const float mis = std::atan2(r_t[0].dot(back.cross(r_n[0])), back.dot(r_n[0]));
			for (int i = 1; i < n; i++) {
				r_n[i] = Basis(r_t[i], mis * (float)i / (float)n).xform(r_n[i]).normalized();
			}
		}
	}
	for (int i = 0; i < n; i++) {
		if (std::fabs(smp[i].tilt) > 1e-7f) {
			r_n[i] = Basis(r_t[i], smp[i].tilt).xform(r_n[i]).normalized();
		}
	}
}

float length(const std::vector<Sample> &smp, bool cyclic) {
	float l = 0.0f;
	for (size_t i = 1; i < smp.size(); i++) {
		l += smp[i].p.distance_to(smp[i - 1].p);
	}
	if (cyclic && smp.size() > 2) {
		l += smp.back().p.distance_to(smp.front().p);
	}
	return l;
}

namespace {

// the cross section (x: to the side, y: along the normal); r_closed: a ring
void section(const Geometry &g, std::vector<Vector2> &r_sec, bool &r_closed) {
	r_sec.clear();
	r_closed = true;
	if (g.road == 1) {
		// its top first, left to right (the image laid across it); then, if it has any, the curbs, the sides and the underside
		const float W = std::max(1e-3f, g.road_width), hw = W * 0.5f;
		const int n = std::max(1, g.road_segments);
		for (int i = 0; i <= n; i++) {
			r_sec.push_back(Vector2(-hw + W * i / n, 0.0f));
		}
		const float t = std::max(0.0f, g.road_thickness), ch = std::max(0.0f, g.road_curb_height), cw = ch > 1e-5f ? std::max(0.0f, g.road_curb_width) : 0.0f;
		if (t <= 1e-5f && ch <= 1e-5f) {
			r_closed = false;
			return;
		}
		if (ch > 1e-5f) {
			r_sec.push_back(Vector2(hw, ch));
			if (cw > 1e-5f) {
				r_sec.push_back(Vector2(hw + cw, ch));
			}
		}
		r_sec.push_back(Vector2(hw + cw, -t));
		r_sec.push_back(Vector2(-hw - cw, -t));
		if (ch > 1e-5f) {
			if (cw > 1e-5f) {
				r_sec.push_back(Vector2(-hw - cw, ch));
			}
			r_sec.push_back(Vector2(-hw, ch));
		}
		return;
	}
	const float e = std::max(0.0f, g.extrude);
	auto stretch = [&](std::vector<Vector2> &p_pts) {
		if (e <= 0.0f) {
			return;
		}
		for (Vector2 &q : p_pts) {
			q.y += q.y > 1e-6f ? e : q.y < -1e-6f ? -e : 0.0f;
		}
	};
	if (g.bevel_mode == 1 && g.object_section.size() >= 2) {
		r_sec = g.object_section;
		r_closed = g.object_closed;
		return;
	}
	if (g.bevel_mode == 2 && g.profile.size() >= 2 && g.depth > 1e-6f) {
		// the profile is a quarter: turned round four times
		std::vector<Vector2> q = g.profile;
		for (int k = 0; k < 4; k++) {
			for (size_t i = 0; i < q.size(); i++) {
				if (k > 0 && i == 0) {
					continue;
				}
				if (k == 3 && i + 1 == q.size()) {
					continue;
				}
				Vector2 p = q[i];
				for (int r = 0; r < k; r++) {
					p = Vector2(-p.y, p.x);
				}
				r_sec.push_back(p * g.depth);
			}
		}
		stretch(r_sec);
		return;
	}
	if (g.depth > 1e-6f) {
		const int n = 4 + 2 * std::max(0, g.bevel_resolution);
		const int half = n / 2;
		// the top half and the bottom half (with an extrude: apart, straight sides between)
		for (int i = 0; i <= half; i++) {
			const float a = PI_F * (float)i / (float)half;
			r_sec.push_back(Vector2(std::cos(a), std::sin(a)) * g.depth + Vector2(0, e));
		}
		for (int i = 0; i <= half; i++) {
			const float a = PI_F + PI_F * (float)i / (float)half;
			const Vector2 p = Vector2(std::cos(a), std::sin(a)) * g.depth - Vector2(0, e);
			if (r_sec.back().distance_to(p) > 1e-6f) {
				r_sec.push_back(p);
			}
		}
		if (r_sec.size() > 2 && r_sec.back().distance_to(r_sec.front()) < 1e-6f) {
			r_sec.pop_back();
		}
		return;
	}
	if (e > 0.0f) {
		r_sec = { Vector2(0, -e), Vector2(0, e) }; // (a ribbon)
		r_closed = false;
	}
}

// The points of a road at one place across it (r_p), for each point of its middle (p_mid): where the curve bends tighter
// than that place is far from the middle, they run backwards and the road folds over itself. They are brought together
// where the part before the fold crosses the part after it.
void untangle_road_column(std::vector<Vector3> &r_p, const std::vector<Vector3> &p_mid, bool p_cyclic, const Vector3 &p_up) {
	const int n = (int)r_p.size();
	const int segs = p_cyclic ? n : n - 1;
	Vector3 e1, e2;
	plane_basis(p_up, e1, e2);
	auto wrap = [n](int p_i) { return ((p_i % n) + n) % n; };
	auto flat = [&](const Vector3 &p_v) { return Vector2(p_v.dot(e1), p_v.dot(e2)); };
	auto back = [&](int p_i) {
		const int a = wrap(p_i), b = wrap(p_i + 1);
		return (flat(r_p[b]) - flat(r_p[a])).dot(flat(p_mid[b]) - flat(p_mid[a])) < -1e-9f;
	};
	const int limit = MIN(p_cyclic ? n / 2 : n, 512);
	for (int i = 0; i < segs; i++) {
		if (!back(i)) {
			continue;
		}
		int last = i;
		while (last + 1 < segs && back(last + 1)) {
			last++;
		}
		int best = 1 << 30, from = 0, to = 0;
		Vector3 meet;
		for (int s = i - 1; s >= i - limit && (p_cyclic || s >= 0); s--) {
			for (int q = last + 1; q <= last + limit && (p_cyclic || q < segs); q++) {
				const int cost = (i - s) + (q - last);
				if (cost >= best) {
					break;
				}
				const Vector2 a0 = flat(r_p[wrap(s)]), a1 = flat(r_p[wrap(s + 1)]), b0 = flat(r_p[wrap(q)]), b1 = flat(r_p[wrap(q + 1)]);
				const Vector2 ra = a1 - a0, rb = b1 - b0;
				const float den = ra.cross(rb);
				if (std::fabs(den) < 1e-12f) {
					continue;
				}
				const float t = (b0 - a0).cross(rb) / den, u = (b0 - a0).cross(ra) / den;
				if (t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f) {
					best = cost;
					from = s + 1;
					to = q;
					meet = r_p[wrap(s)].lerp(r_p[wrap(s + 1)], t);
				}
			}
		}
		if (best == 1 << 30) {
			// (at an end of the road, nothing to cross: its middle)
			from = i;
			to = last + 1;
			meet = (r_p[wrap(i)] + r_p[wrap(last + 1)]) * 0.5f;
		}
		for (int k = from; k <= to; k++) {
			r_p[wrap(k)] = meet;
		}
		i = MAX(i, MIN(to, segs - 1));
	}
}

float taper_at(const std::vector<float> &taper, float u) {
	if (taper.empty()) {
		return 1.0f;
	}
	if (taper.size() == 1) {
		return taper[0];
	}
	const float x = CLAMP(u, 0.0f, 1.0f) * (float)(taper.size() - 1);
	const int i = std::min((int)x, (int)taper.size() - 2);
	return taper[i] + (taper[i + 1] - taper[i]) * (x - (float)i);
}

} // namespace

// More samples where they are far apart (a road following the ground).
void densify_samples(std::vector<Sample> &r_smp, bool p_cyclic, float p_step) {
	std::vector<Sample> out;
	const size_t n = r_smp.size();
	for (size_t i = 0; i < n; i++) {
		out.push_back(r_smp[i]);
		if (i + 1 == n && !p_cyclic) {
			break;
		}
		const Sample &a = r_smp[i], &b = r_smp[(i + 1) % n];
		const int parts = MIN(1000, (int)std::ceil(a.p.distance_to(b.p) / p_step));
		for (int k = 1; k < parts; k++) {
			const float t = (float)k / parts;
			Sample q = a;
			q.p = a.p.lerp(b.p, t);
			q.tilt = a.tilt + (b.tilt - a.tilt) * t;
			q.radius = a.radius + (b.radius - a.radius) * t;
			out.push_back(q);
		}
	}
	r_smp.swap(out);
}

void build_geometry(const std::vector<Spline> &p_splines, const Geometry &g, Mesh &out) {
	out.clear();
	std::vector<Vector2> sec;
	bool sec_closed = true;
	section(g, sec, sec_closed);
	for (const Spline &s : p_splines) {
		std::vector<Sample> smp;
		sample(s, g.resolution, smp);
		if (g.is_2d) {
			for (Sample &q : smp) {
				q.p.y = 0.0f; // (a 2D curve lies on its local XZ plane, its up the Y)
			}
		}
		if (smp.size() < 2) {
			continue;
		}
		const bool cyc = s.cyclic && smp.size() > 2;
		if (g.road == 1 && g.road_step > 1e-4f) {
			densify_samples(smp, cyc, g.road_step);
		}
		std::vector<Vector3> T, N;
		frames(smp, cyc, g.is_2d, T, N);
		if (g.road == 1) {
			// (a road's up is the world's, however the curve turns; its tilt banks it)
			for (size_t i = 0; i < smp.size(); i++) {
				Vector3 nn = g.road_up - T[i] * T[i].dot(g.road_up);
				if (nn.length_squared() > 1e-8f) {
					N[i] = nn.normalized();
					if (std::fabs(smp[i].tilt) > 1e-7f) {
						N[i] = Basis(T[i], smp[i].tilt).xform(N[i]).normalized();
					}
				}
			}
		}
		// the length along it (the taper)
		std::vector<float> along(smp.size(), 0.0f);
		for (size_t i = 1; i < smp.size(); i++) {
			along[i] = along[i - 1] + smp[i].p.distance_to(smp[i - 1].p);
		}
		const float total = std::max(1e-6f, along.back());
		const int rings = (int)smp.size();
		const int m = (int)sec.size();
		if (m >= 2) {
			const int base = (int)out.pos.size();
			const int f0 = (int)out.faces.size();
			// (a road's places, seen from above: not folded over itself in the tight bends)
			std::vector<Vector3> road_at;
			if (g.road == 1) {
				road_at.resize(rings * m);
				std::vector<Vector3> column(rings), middle(rings);
				for (int j = 0; j < m; j++) {
					for (int i = 0; i < rings; i++) {
						const Vector3 B = T[i].cross(N[i]).normalized();
						middle[i] = smp[i].p + B * g.offset;
						column[i] = middle[i] + B * sec[j].x * smp[i].radius * taper_at(g.taper, along[i] / total);
					}
					untangle_road_column(column, middle, cyc, g.road_up);
					for (int i = 0; i < rings; i++) {
						road_at[i * m + j] = column[i];
					}
				}
			}
			for (int i = 0; i < rings; i++) {
				const Vector3 B = T[i].cross(N[i]).normalized();
				const float sc = smp[i].radius * taper_at(g.taper, along[i] / total);
				const Vector3 c = smp[i].p + B * g.offset;
				for (int j = 0; j < m; j++) {
					const Vector3 at = g.road == 1 ? road_at[i * m + j] : c + B * sec[j].x * sc;
					Vector3 v = at + N[i] * sec[j].y * sc;
					Vector3 ground;
					if (g.road == 1 && g.road_snap && g.road_snap(at, c, ground)) {
						v = ground + g.road_up * sec[j].y * sc; // (on what is under it: the section's own height above that)
					}
					out.add_vertex(v);
				}
			}
			const int ring_segs = cyc ? rings : rings - 1;
			const int sec_segs = sec_closed ? m : m - 1;
			// (a road: its top first, then the rest; the underside can be left out)
			const int road_top = g.road == 1 ? std::max(1, g.road_segments) : m;
			int no_face = -1;
			if (g.road == 1 && !g.road_bottom && sec_closed) {
				for (int j = road_top; j + 1 < m; j++) {
					if (std::fabs(sec[j].y - sec[j + 1].y) < 1e-6f && sec[j].x > 0.0f && sec[j + 1].x < 0.0f) {
						no_face = j;
					}
				}
			}
			// the UVs: the length round the section (a road: its width is one image) and along the curve
			std::vector<float> sec_u(m + 1, 0.0f);
			for (int j = 1; j <= m; j++) {
				sec_u[j] = sec_u[j - 1] + sec[j % m].distance_to(sec[j - 1]);
			}
			const float u_k = g.road == 1 ? 1.0f / std::max(1e-3f, g.road_width) : g.uv_scale.x;
			const float v_k = g.road == 1 ? 1.0f / std::max(1e-3f, g.road_length) : g.uv_scale.y;
			auto uv_of = [&](float p_u, float p_v) {
				const Vector2 q(p_u * u_k, p_v * v_k);
				return g.uv_turn ? Vector2(q.y, q.x) : q;
			};
			for (int i = 0; i < ring_segs; i++) {
				const int a = i, b = (i + 1) % rings;
				const float va = along[a], vb = b != 0 ? along[b] : along[a] + smp[a].p.distance_to(smp[0].p);
				for (int j = 0; j < sec_segs; j++) {
					if (j == no_face) {
						continue;
					}
					const int jn = (j + 1) % m;
					Face f = make_face({ base + a * m + j, base + b * m + j, base + b * m + jn, base + a * m + jn });
					f.smooth = g.smooth ? 1 : 0;
					f.role = ROLE_SIDE;
					f.material = g.road_side_slot && j >= road_top ? 1 : 0;
					if (g.world_uv) {
						f.uvp.world = true;
						f.uvp.scale = g.uv_scale;
					} else {
						f.uv = { uv_of(sec_u[j], va), uv_of(sec_u[j], vb), uv_of(sec_u[j + 1], vb), uv_of(sec_u[j + 1], va) };
						f.manual_uv = true;
					}
					out.faces.push_back(f);
				}
			}
			// (outward: away from the curve where it starts)
			if ((int)out.faces.size() > f0) {
				const Face &F = out.faces[f0];
				const Vector3 fn = face_normal(out, F);
				const Vector3 fc = face_center(out, F);
				// (a ribbon round a closed 2D shape: its walls away from the shape's middle)
				Vector3 ref = smp[0].p + T[0].cross(N[0]).normalized() * g.offset;
				if (!sec_closed && g.is_2d && cyc) {
					ref = Vector3();
					for (const Sample &q : smp) {
						ref += q.p;
					}
					ref /= (float)smp.size();
					ref.y = fc.y;
				}
				if (g.road == 1 ? fn.dot(g.road_up) < 0.0f : fn.dot(fc - ref) < 0.0f) { // (a road: its first face is its top)
					for (int f = f0; f < (int)out.faces.size(); f++) {
						out.faces[f].reverse();
					}
				}
			}
			if (g.fill_caps && sec_closed && !cyc && m >= 3) {
				std::vector<int> c0, c1;
				for (int j = 0; j < m; j++) {
					c0.push_back(base + j);
					c1.push_back(base + (rings - 1) * m + j);
				}
				Face a = make_face(c0), b = make_face(c1);
				a.role = b.role = ROLE_OUTER;
				a.uvp.world = b.uvp.world = g.world_uv;
				a.uvp.scale = b.uvp.scale = g.uv_scale;
				a.material = b.material = g.road_side_slot ? 1 : 0;
				if (face_normal(out, a).dot(T[0]) > 0.0f) {
					a.reverse();
				}
				if (face_normal(out, b).dot(T[rings - 1]) < 0.0f) {
					b.reverse();
				}
				out.faces.push_back(a);
				out.faces.push_back(b);
			}
		}
		if (g.is_2d && cyc && g.fill_mode > 0 && rings >= 3) {
			// a closed 2D shape filled: its top (+Y) and bottom, as far apart as the extrude
			const float e = std::max(0.0f, g.extrude);
			for (int side = 0; side < 2; side++) {
				if (!(g.fill_mode & (side == 0 ? 1 : 2))) {
					continue;
				}
				const float y = side == 0 ? -e : e;
				std::vector<int> ids;
				for (int i = 0; i < rings; i++) {
					const Vector3 B = T[i].cross(N[i]).normalized();
					ids.push_back(out.add_vertex(Vector3(smp[i].p.x, y, smp[i].p.z) + B * g.offset));
				}
				Face f = make_face(ids);
				f.role = side == 0 ? ROLE_BOTTOM : ROLE_TOP;
				f.uvp.world = g.world_uv;
				f.uvp.scale = g.uv_scale;
				if ((face_normal(out, f).y > 0.0f) != (side == 1)) {
					f.reverse();
				}
				out.faces.push_back(f);
			}
		}
	}
	weld_by_distance(out, 1e-6f);
}

godot::Dictionary spline_to_dict(const Spline &s) {
	Dictionary d;
	d["type"] = s.type;
	d["cyclic"] = s.cyclic;
	d["order"] = s.order;
	d["endpoint"] = s.endpoint;
	PackedVector3Array pos, in, out;
	PackedFloat32Array tilt, radius, weight;
	PackedInt32Array hl, hr;
	for (const CPoint &p : s.pts) {
		pos.push_back(p.pos);
		in.push_back(p.in);
		out.push_back(p.out);
		tilt.push_back(p.tilt);
		radius.push_back(p.radius);
		weight.push_back(p.weight);
		hl.push_back(p.hl);
		hr.push_back(p.hr);
	}
	d["pos"] = pos;
	d["in"] = in;
	d["out"] = out;
	d["tilt"] = tilt;
	d["radius"] = radius;
	d["weight"] = weight;
	d["hl"] = hl;
	d["hr"] = hr;
	return d;
}

Spline spline_from_dict(const godot::Dictionary &d) {
	Spline s;
	s.type = CLAMP((int)d.get("type", BEZIER), 0, 2);
	s.cyclic = d.get("cyclic", false);
	s.order = CLAMP((int)d.get("order", 4), 2, 6);
	s.endpoint = d.get("endpoint", false);
	const PackedVector3Array pos = d.get("pos", PackedVector3Array()), in = d.get("in", PackedVector3Array()), out = d.get("out", PackedVector3Array());
	const PackedFloat32Array tilt = d.get("tilt", PackedFloat32Array()), radius = d.get("radius", PackedFloat32Array()), weight = d.get("weight", PackedFloat32Array());
	const PackedInt32Array hl = d.get("hl", PackedInt32Array()), hr = d.get("hr", PackedInt32Array());
	for (int64_t i = 0; i < pos.size(); i++) {
		CPoint p;
		p.pos = pos[i];
		p.in = i < in.size() ? in[i] : Vector3();
		p.out = i < out.size() ? out[i] : Vector3();
		p.tilt = i < tilt.size() ? tilt[i] : 0.0f;
		p.radius = i < radius.size() ? radius[i] : 1.0f;
		p.weight = i < weight.size() ? weight[i] : 1.0f;
		p.hl = i < hl.size() ? CLAMP(hl[i], 0, 3) : H_AUTO;
		p.hr = i < hr.size() ? CLAMP(hr[i], 0, 3) : H_AUTO;
		s.pts.push_back(p);
	}
	return s;
}

} // namespace curves
} // namespace hewn

// ================================================================== HewnCurve3D

std::vector<HewnCurve3D *> HewnCurve3D::all;

namespace {

// The triangles of the meshes under a road, sorted into columns: what is below a point, fast.
struct GroundGrid {
	std::vector<Vector3> tris;
	float x0 = 0.0f, z0 = 0.0f, sx = 1.0f, sz = 1.0f;
	int nx = 1, nz = 1;
	std::vector<std::vector<int>> cells;

	void build(const std::vector<HewnMesh3D *> &p_meshes) {
		for (HewnMesh3D *m : p_meshes) {
			const Transform3D g = m->get_global_transform();
			const PackedVector3Array f = m->get_mesh()->get_faces();
			for (int64_t i = 0; i + 2 < f.size(); i += 3) {
				const Vector3 a = g.xform(f[i]), b = g.xform(f[i + 1]), c = g.xform(f[i + 2]);
				if ((b - a).cross(c - a).length_squared() < 1e-14f) {
					continue;
				}
				tris.push_back(a);
				tris.push_back(b);
				tris.push_back(c);
			}
		}
		if (tris.empty()) {
			return;
		}
		float x1 = tris[0].x, z1 = tris[0].z;
		x0 = tris[0].x;
		z0 = tris[0].z;
		for (const Vector3 &p : tris) {
			x0 = MIN(x0, p.x);
			z0 = MIN(z0, p.z);
			x1 = MAX(x1, p.x);
			z1 = MAX(z1, p.z);
		}
		const int side = CLAMP((int)std::sqrt((double)tris.size() / 3.0) + 1, 1, 512);
		nx = nz = side;
		sx = MAX(1e-4f, (x1 - x0) / nx);
		sz = MAX(1e-4f, (z1 - z0) / nz);
		cells.assign(nx * nz, std::vector<int>());
		for (int t = 0; t + 2 < (int)tris.size(); t += 3) {
			const float ax = MIN(tris[t].x, MIN(tris[t + 1].x, tris[t + 2].x)), bx = MAX(tris[t].x, MAX(tris[t + 1].x, tris[t + 2].x));
			const float az = MIN(tris[t].z, MIN(tris[t + 1].z, tris[t + 2].z)), bz = MAX(tris[t].z, MAX(tris[t + 1].z, tris[t + 2].z));
			for (int i = _col(ax, x0, sx, nx); i <= _col(bx, x0, sx, nx); i++) {
				for (int j = _col(az, z0, sz, nz); j <= _col(bz, z0, sz, nz); j++) {
					cells[j * nx + i].push_back(t);
				}
			}
		}
	}

	static int _col(float p_v, float p_o, float p_s, int p_n) { return CLAMP((int)std::floor((p_v - p_o) / p_s), 0, p_n - 1); }

	// The highest surface at (x, z) between the two heights.
	bool below(float p_x, float p_z, float p_from, float p_to, float &r_y) const {
		if (cells.empty()) {
			return false;
		}
		bool found = false;
		for (int t : cells[_col(p_z, z0, sz, nz) * nx + _col(p_x, x0, sx, nx)]) {
			const Vector3 &a = tris[t], &b = tris[t + 1], &c = tris[t + 2];
			const float den = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
			if (std::fabs(den) < 1e-12f) {
				continue; // (upright)
			}
			const float u = ((p_x - a.x) * (c.z - a.z) - (c.x - a.x) * (p_z - a.z)) / den;
			const float v = ((b.x - a.x) * (p_z - a.z) - (p_x - a.x) * (b.z - a.z)) / den;
			if (u < -1e-5f || v < -1e-5f || u + v > 1.00001f) {
				continue;
			}
			const float y = a.y + (b.y - a.y) * u + (c.y - a.y) * v;
			if (y <= p_from && y >= p_to && (!found || y > r_y)) {
				r_y = y;
				found = true;
			}
		}
		return found;
	}
};

} // namespace

float HewnCurve3D::_road_length() const {
	if (road_repeat > 1e-4f) {
		return road_repeat;
	}
	// (as long as the image's proportions make it at this width)
	Ref<Texture2D> tex;
	if (Ref<BaseMaterial3D> bm = material; bm.is_valid()) {
		tex = bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
	} else if (Ref<ShaderMaterial> sm = material; sm.is_valid()) {
		for (const char *pn : { "albedo_texture", "base_texture", "texture_albedo" }) {
			tex = sm->get_shader_parameter(pn);
			if (tex.is_valid()) {
				break;
			}
		}
	}
	if (tex.is_null() || tex->get_width() <= 0 || tex->get_height() <= 0) {
		return geo.road_width;
	}
	return geo.road_width * (geo.uv_turn ? (float)tex->get_width() / tex->get_height() : (float)tex->get_height() / tex->get_width());
}

AABB HewnCurve3D::_road_box() const {
	const Transform3D g = get_global_transform();
	AABB box;
	bool first = true;
	float rmax = 1.0f;
	for (int si = 0; si < (int)splines.size(); si++) {
		for (const hewn::curves::CPoint &p : splines[si].pts) {
			rmax = MAX(rmax, p.radius);
		}
		for (const Vector3 &p : sampled_points(si)) {
			const Vector3 w = g.xform(p);
			if (first) {
				box = AABB(w, Vector3());
				first = false;
			} else {
				box.expand_to(w);
			}
		}
	}
	return box.grow(geo.road_width * rmax + 1.0f);
}

std::vector<HewnMesh3D *> HewnCurve3D::_road_meshes() const {
	std::vector<HewnMesh3D *> out;
	AABB box = _road_box();
	box.position.y = -1e6f; // (any height: only the columns under it)
	box.size.y = 2e6f;
	for (HewnMesh3D *m : HewnMesh3D::all_meshes) {
		if (m->is_inside_tree() && m->is_visible_in_tree() && !m->has_meta("_hw_curve_mesh") && !m->is_brush() && !m->is_boolean_cutter() && m->get_mesh().is_valid() &&
				m->get_global_transform().xform(m->get_mesh()->get_aabb()).grow(0.01f).intersects(box)) {
			out.push_back(m);
		}
	}
	return out;
}

void HewnCurve3D::_road_unwatch() {
	const Callable c = callable_mp(this, &HewnCurve3D::_road_ground_changed);
	for (const ObjectID &id : road_watched) {
		if (Object *o = ObjectDB::get_instance(id)) {
			for (const char *sig : { "mesh_changed", "geometry_moved" }) {
				if (o->is_connected(sig, c)) {
					o->disconnect(sig, c);
				}
			}
		}
	}
	road_watched.clear();
}

void HewnCurve3D::_road_watch(const std::vector<HewnMesh3D *> &p_meshes) {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	const Callable c = callable_mp(this, &HewnCurve3D::_road_ground_changed);
	for (HewnMesh3D *m : p_meshes) {
		for (const char *sig : { "mesh_changed", "geometry_moved" }) {
			if (!m->is_connected(sig, c)) {
				m->connect(sig, c);
			}
		}
		road_watched.push_back(ObjectID(m->get_instance_id()));
	}
}

void HewnCurve3D::_road_ground_changed() {
	if (!road_repainting) {
		_queue();
	}
}

void HewnCurve3D::_road_source_changed() {
	road_mat.unref();
	_queue();
}

// An overlay road's material when its edges are broken up: its image, color, roughness, metallic and normal map as the
// curve's material has them; the soft edge goes by a noise and by the image's height.
static Ref<Material> road_edge_material(const Ref<BaseMaterial3D> &p_from, float p_noise, float p_size, float p_height, const Ref<Texture2D> &p_height_map) {
	const bool nearest = p_from.is_valid() && (p_from->get_texture_filter() == BaseMaterial3D::TEXTURE_FILTER_NEAREST || p_from->get_texture_filter() == BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS ||
							  p_from->get_texture_filter() == BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC);
	String code = "shader_type spatial;\nrender_mode blend_mix, depth_draw_opaque, cull_back;\n";
	code += "uniform vec4 albedo : source_color = vec4(1.0);\n";
	code += "uniform sampler2D texture_albedo : source_color, hint_default_white, FILTER, repeat_enable;\n";
	code += "uniform float roughness : hint_range(0.0, 1.0) = 1.0;\nuniform float metallic : hint_range(0.0, 1.0) = 0.0;\n";
	code += "uniform bool use_normal = false;\nuniform sampler2D texture_normal : hint_roughness_normal, FILTER, repeat_enable;\nuniform float normal_scale = 1.0;\n";
	code += "uniform vec3 uv1_scale = vec3(1.0);\nuniform vec3 uv1_offset = vec3(0.0);\n";
	code += "// the edge: a noise over the world (its size in meters), mixed with the image's height\n";
	code += "uniform sampler2D edge_noise : hint_default_black, filter_linear_mipmap, repeat_enable;\n";
	code += "uniform float edge_noise_scale = 0.5;\nuniform float edge_noise_strength : hint_range(0.0, 1.0) = 1.0;\n";
	code += "uniform float edge_height : hint_range(0.0, 1.0) = 0.5;\nuniform bool use_height_map = false;\n";
	code += "uniform sampler2D edge_height_map : hint_default_black, FILTER, repeat_enable;\nuniform float edge_softness : hint_range(0.01, 0.5) = 0.12;\n";
	code += "varying vec3 world_pos;\n";
	code += "void vertex() {\n\tworld_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;\n}\n";
	code += "void fragment() {\n\tvec2 uv = UV * uv1_scale.xy + uv1_offset.xy;\n\tvec4 t = texture(texture_albedo, uv);\n";
	code += "\tALBEDO = albedo.rgb * t.rgb * COLOR.rgb;\n";
	code += "\tfloat h = use_height_map ? texture(edge_height_map, uv).r : dot(t.rgb, vec3(0.299, 0.587, 0.114));\n";
	code += "\tfloat n = mix(texture(edge_noise, world_pos.xz * edge_noise_scale).r, h, edge_height);\n";
	code += "\tfloat d = edge_softness;\n\tfloat at = mix(d, 1.0 - d, n);\n";
	code += "\t// (all of the road stays whole, the soft edge only goes ragged)\n";
	code += "\tALPHA = albedo.a * t.a * mix(COLOR.a, smoothstep(at - d, at + d, COLOR.a), edge_noise_strength);\n";
	code += "\tROUGHNESS = roughness;\n\tMETALLIC = metallic;\n";
	code += "\tif (use_normal) {\n\t\tNORMAL_MAP = texture(texture_normal, uv).rgb;\n\t\tNORMAL_MAP_DEPTH = normal_scale;\n\t}\n}\n";
	code = code.replace("FILTER", nearest ? "filter_nearest_mipmap" : "filter_linear_mipmap_anisotropic");
	Ref<Shader> sh;
	sh.instantiate();
	sh->set_code(code);
	Ref<ShaderMaterial> m;
	m.instantiate();
	m->set_shader(sh);
	if (p_from.is_valid()) {
		m->set_shader_parameter("albedo", p_from->get_albedo());
		m->set_shader_parameter("texture_albedo", p_from->get_texture(BaseMaterial3D::TEXTURE_ALBEDO));
		m->set_shader_parameter("roughness", p_from->get_roughness());
		m->set_shader_parameter("metallic", p_from->get_metallic());
		if (p_from->get_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING) && p_from->get_texture(BaseMaterial3D::TEXTURE_NORMAL).is_valid()) {
			m->set_shader_parameter("use_normal", true);
			m->set_shader_parameter("texture_normal", p_from->get_texture(BaseMaterial3D::TEXTURE_NORMAL));
			m->set_shader_parameter("normal_scale", p_from->get_normal_scale());
		}
		m->set_shader_parameter("uv1_scale", p_from->get_uv1_scale());
		m->set_shader_parameter("uv1_offset", p_from->get_uv1_offset());
	} else {
		m->set_shader_parameter("albedo", Color(0.45f, 0.42f, 0.38f));
	}
	m->set_shader_parameter("edge_noise", hewn::make_noise_texture());
	m->set_shader_parameter("edge_noise_scale", 1.0f / MAX(0.01f, p_size));
	m->set_shader_parameter("edge_noise_strength", CLAMP(p_noise, 0.0f, 1.0f));
	m->set_shader_parameter("edge_height", CLAMP(p_height, 0.0f, 1.0f));
	m->set_shader_parameter("use_height_map", p_height_map.is_valid());
	m->set_shader_parameter("edge_height_map", p_height_map);
	return m;
}

// The road as an overlay: a ribbon of triangles along each spline, laid on the highest surface under it.
void HewnCurve3D::_rebuild_road() {
	_road_unwatch();
	if (geo.road != 2) {
		if (road_overlay) {
			remove_child(road_overlay);
			road_overlay->queue_free();
			road_overlay = nullptr;
		}
		road_mat.unref();
		road_src.unref();
		if (geo.road == 1 && road_ground_snap) {
			_road_watch(_road_meshes());
		}
		return;
	}
	if (!road_overlay) {
		road_overlay = memnew(HewnOverlay3D);
		road_overlay->set_name("_RoadOverlay");
		road_overlay->set_meta("_hw_road", true);
		road_overlay->set_auto_update(false); // (the curve lays it again)
		road_overlay->set_as_top_level(true);
		add_child(road_overlay, false, INTERNAL_MODE_BACK);
	}
	if (road_mat.is_null() || road_src != material) {
		road_src = material;
		if (road_edge_noise > 1e-4f && (material.is_null() || Ref<BaseMaterial3D>(material).is_valid())) {
			road_mat = road_edge_material(material, road_edge_noise, road_edge_noise_size, road_edge_height, road_edge_height_map);
		} else if (Ref<BaseMaterial3D> bm = material; bm.is_valid()) {
			Ref<BaseMaterial3D> d = bm->duplicate();
			d->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA); // (its soft edges over what is under it)
			d->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
			road_mat = d;
		} else if (material.is_valid()) {
			road_mat = material; // (a shader of its own)
		} else {
			Ref<StandardMaterial3D> d;
			d.instantiate();
			d->set_albedo(Color(0.45f, 0.42f, 0.38f));
			d->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
			d->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
			road_mat = d;
		}
	}
	if (road_mat != material) {
		road_mat->set_render_priority(CLAMP(road_order, -128, 127));
	}
	road_overlay->set_material(road_mat);
	const Transform3D g = get_global_transform();
	const Vector3 origin = g.origin;
	road_overlay->set_global_transform(Transform3D(Basis(), origin));
	const std::vector<HewnMesh3D *> meshes = _road_meshes();
	_road_watch(meshes);
	GroundGrid ground;
	ground.build(meshes);
	const float W = MAX(1e-3f, geo.road_width);
	const float repeat = MAX(1e-3f, _road_length());
	const float step = MAX(0.05f, road_step);
	const float tol = MAX(1.0f, step);
	std::vector<HewnOverlayTri> tris;
	for (const hewn::curves::Spline &s : splines) {
		std::vector<hewn::curves::Sample> smp;
		hewn::curves::sample(s, geo.resolution, smp);
		if (smp.size() < 2) {
			continue;
		}
		const bool cyc = s.cyclic && smp.size() > 2;
		hewn::curves::densify_samples(smp, cyc, step);
		const int n = (int)smp.size();
		std::vector<Vector3> P(n), L(n);
		std::vector<float> along(n, 0.0f), lo(n, 1e30f), hi(n, -1e30f), half(n);
		for (int i = 0; i < n; i++) {
			P[i] = g.xform(smp[i].p);
			if (i > 0) {
				along[i] = along[i - 1] + P[i].distance_to(P[i - 1]);
			}
		}
		for (int i = 0; i < n; i++) {
			const Vector3 t = P[cyc ? (i + 1) % n : MIN(n - 1, i + 1)] - P[cyc ? (i - 1 + n) % n : MAX(0, i - 1)];
			Vector3 l(t.z, 0.0f, -t.x);
			L[i] = l.length_squared() > 1e-12f ? l.normalized() : (i > 0 ? L[i - 1] : Vector3(1, 0, 0));
			half[i] = W * 0.5f * smp[i].radius;
			for (int k = -1; k <= 1; k++) {
				const Vector3 q = P[i] + L[i] * half[i] * (float)k;
				float y;
				if (ground.below(q.x, q.z, P[i].y + W + 0.1f, P[i].y - road_reach, y)) {
					lo[i] = MIN(lo[i], y);
					hi[i] = MAX(hi[i], y);
				}
			}
		}
		const float close_len = cyc ? P[n - 1].distance_to(P[0]) : 0.0f;
		const int segs = cyc ? n : n - 1;
		const float soft = CLAMP(road_softness, 0.0f, W * 0.5f);
		const std::vector<float> offs = soft > 1e-4f ? std::vector<float>{ -0.5f, -0.5f + soft / W, 0.5f - soft / W, 0.5f } : std::vector<float>{ -0.5f, 0.5f };
		const std::vector<float> alph = soft > 1e-4f ? std::vector<float>{ 0.0f, 1.0f, 1.0f, 0.0f } : std::vector<float>{ 1.0f, 1.0f };
		auto to2 = [&](int p_ring, float p_off) {
			const Vector3 q = P[p_ring] + L[p_ring] * (half[p_ring] * 2.0f * p_off) - origin;
			return Vector2(q.x, q.z);
		};
		auto uv_at = [&](float p_off, float p_v) {
			const Vector2 q(p_off + 0.5f, p_v / repeat);
			return geo.uv_turn ? Vector2(q.y, q.x) : q;
		};
		for (int i = 0; i < segs; i++) {
			const int b = (i + 1) % n;
			const float va = along[i], vb = b != 0 ? along[b] : along[i] + close_len;
			const float mn = MIN(lo[i], lo[b]), mx = MAX(hi[i], hi[b]);
			if (mn > mx) {
				continue; // (nothing under this stretch)
			}
			for (size_t c = 0; c + 1 < offs.size(); c++) {
				const Vector2 p0 = to2(i, offs[c]), p1 = to2(b, offs[c]), p2 = to2(b, offs[c + 1]), p3 = to2(i, offs[c + 1]);
				const Vector2 u0 = uv_at(offs[c], va), u1 = uv_at(offs[c], vb), u2 = uv_at(offs[c + 1], vb), u3 = uv_at(offs[c + 1], va);
				HewnOverlayTri a, d;
				a.p[0] = p0;
				a.p[1] = p1;
				a.p[2] = p2;
				a.uv[0] = u0;
				a.uv[1] = u1;
				a.uv[2] = u2;
				a.a[0] = alph[c];
				a.a[1] = alph[c];
				a.a[2] = alph[c + 1];
				d.p[0] = p0;
				d.p[1] = p2;
				d.p[2] = p3;
				d.uv[0] = u0;
				d.uv[1] = u2;
				d.uv[2] = u3;
				d.a[0] = alph[c];
				d.a[1] = alph[c + 1];
				d.a[2] = alph[c + 1];
				a.y_min = d.y_min = mn - tol - origin.y;
				a.y_max = d.y_max = mx + tol - origin.y;
				tris.push_back(a);
				tris.push_back(d);
			}
		}
	}
	road_overlay->project_tris(tris, get_owner() ? get_owner() : get_parent());
}

HewnCurve3D::HewnCurve3D() {
	set_notify_transform(true);
}

HewnCurve3D::~HewnCurve3D() {
	all.erase(std::remove(all.begin(), all.end(), this), all.end());
}

void HewnCurve3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		if (std::find(all.begin(), all.end(), this) == all.end()) {
			all.push_back(this);
		}
		if (splines.empty() && get_curve().is_valid() && get_curve()->get_point_count() > 0) {
			_on_curve_changed(); // (a Path3D's curve: its first spline)
		}
		// a copy (Ctrl+D, Shift+D, copy and paste) has the other's Curve3D: a curve of its own, else editing one
		// would change both
		if (get_curve().is_valid()) {
			for (HewnCurve3D *o : all) {
				if (o != this && o->get_curve() == get_curve()) {
					syncing = true;
					set_curve(get_curve()->duplicate());
					syncing = false;
					break;
				}
			}
		}
		if (get_curve().is_valid() && !get_curve()->is_connected("changed", callable_mp(this, &HewnCurve3D::_on_curve_changed))) {
			get_curve()->connect("changed", callable_mp(this, &HewnCurve3D::_on_curve_changed));
		}
		if (!shown) {
			shown = memnew(HewnMesh3D);
			shown->set_name("_CurveMesh");
			shown->set_meta("_hw_curve_mesh", true);
			add_child(shown, false, INTERNAL_MODE_BACK);
		}
		_queue();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		all.erase(std::remove(all.begin(), all.end(), this), all.end());
		_road_unwatch();
	} else if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		if (paint_enabled || paint_had) {
			_repaint_meshes(); // (moved: the paint goes along, the colors it left come back)
		}
		if (_drops() || _road_follows_ground()) {
			_queue(); // (things dropped onto the ground under it, a road laid on it: again)
		}
		for (HewnMesh3D *m : HewnMesh3D::all_meshes) {
			const NodePath fp = m->get_follow_curve();
			if (!fp.is_empty() && m->is_inside_tree() && m->get_node_or_null(fp) == this) {
				m->follow_changed(); // (moved: what follows it goes along)
			}
		}
	}
}

void HewnCurve3D::_queue() {
	if (queued || !is_inside_tree()) {
		return;
	}
	queued = true;
	callable_mp(this, &HewnCurve3D::rebuild).call_deferred();
}

void HewnCurve3D::changed() {
	_sync_path_curve();
	_queue();
	update_gizmos();
}

// Godot's own curve edited (its Path3D tools): the first spline follows it.
void HewnCurve3D::_on_curve_changed() {
	if (syncing) {
		return;
	}
	Ref<Curve3D> c = get_curve();
	if (c.is_null()) {
		return;
	}
	if (!splines.empty() && splines[0].type == hewn::curves::NURBS) {
		return; // (its curve is only samples of it)
	}
	hewn::curves::Spline s = splines.empty() ? hewn::curves::Spline() : splines[0];
	const bool same_count = (int)s.pts.size() == c->get_point_count();
	std::vector<hewn::curves::CPoint> pts;
	for (int i = 0; i < c->get_point_count(); i++) {
		hewn::curves::CPoint p = same_count ? s.pts[i] : hewn::curves::CPoint();
		p.pos = c->get_point_position(i);
		p.in = c->get_point_in(i);
		p.out = c->get_point_out(i);
		p.tilt = c->get_point_tilt(i);
		if (!same_count) {
			p.hl = p.hr = hewn::curves::H_FREE;
		}
		pts.push_back(p);
	}
	s.pts = pts;
	s.cyclic = c->is_closed();
	if (splines.empty()) {
		splines.push_back(s);
	} else {
		splines[0] = s;
	}
	_queue();
}

void HewnCurve3D::_sync_path_curve() {
	Ref<Curve3D> c = get_curve();
	if (c.is_null()) {
		c.instantiate();
		syncing = true;
		set_curve(c);
		syncing = false;
		c->connect("changed", callable_mp(this, &HewnCurve3D::_on_curve_changed));
	}
	syncing = true;
	c->clear_points();
	if (!splines.empty()) {
		const hewn::curves::Spline &s = splines[0];
		if (s.type == hewn::curves::NURBS) {
			std::vector<hewn::curves::Sample> smp;
			hewn::curves::sample(s, 4, smp);
			for (const hewn::curves::Sample &q : smp) {
				c->add_point(q.p);
				c->set_point_tilt(c->get_point_count() - 1, q.tilt);
			}
		} else {
			for (const hewn::curves::CPoint &p : s.pts) {
				const bool bz = s.type == hewn::curves::BEZIER;
				const int last = c->get_point_count() - 1;
				if (last >= 0 && c->get_point_position(last).distance_to(p.pos) < 1e-5f) {
					continue; // (two points at one place: Godot's curve has no length between them and complains)
				}
				c->add_point(p.pos, bz ? p.in : Vector3(), bz ? p.out : Vector3());
				c->set_point_tilt(c->get_point_count() - 1, p.tilt);
			}
			if (s.cyclic && c->get_point_count() > 1 && c->get_point_position(0).distance_to(c->get_point_position(c->get_point_count() - 1)) < 1e-5f) {
				c->remove_point(c->get_point_count() - 1);
			}
		}
		c->set_closed(s.cyclic);
	}
	syncing = false;
}

uint64_t HewnCurve3D::_deps_key() const {
	uint64_t k = 0;
	for (const NodePath &np : { bevel_object, taper_object }) {
		HewnCurve3D *o = np.is_empty() || !is_inside_tree() ? nullptr : Object::cast_to<HewnCurve3D>(get_node_or_null(np));
		k = k * 1000003u + (o ? (uint64_t)o->get_instance_id() : 7u);
	}
	return k;
}

hewn::Mesh HewnCurve3D::geometry_mesh() const {
	hewn::curves::Geometry g = geo;
	// another curve's first spline as the cross section (its local X / Y), the taper curve's Y along its X
	if (g.bevel_mode == 1) {
		HewnCurve3D *o = bevel_object.is_empty() || !is_inside_tree() ? nullptr : Object::cast_to<HewnCurve3D>(get_node_or_null(bevel_object));
		if (o && o != this && !o->splines.empty()) {
			std::vector<hewn::curves::Sample> smp;
			hewn::curves::sample(o->splines[0], std::max(1, o->geo.resolution), smp);
			// (drawn on the ground, its X / -Z; else its X / Y)
			bool flat = true;
			for (const hewn::curves::Sample &q : smp) {
				flat = flat && std::fabs(q.p.y) < 1e-4f;
			}
			for (const hewn::curves::Sample &q : smp) {
				g.object_section.push_back(flat ? Vector2(q.p.x, -q.p.z) : Vector2(q.p.x, q.p.y));
			}
			g.object_closed = o->splines[0].cyclic;
		}
	} else if (g.bevel_mode == 2 && bevel_profile.is_valid()) {
		g.profile = bevel_profile->to_vector();
	} else if (g.bevel_mode == 2) {
		// (no profile yet: a quarter round)
		for (int i = 0; i <= 4; i++) {
			const float a = 1.5707963f * i / 4.0f;
			g.profile.push_back(Vector2(std::cos(a), std::sin(a)));
		}
	}
	HewnCurve3D *tc = taper_object.is_empty() || !is_inside_tree() ? nullptr : Object::cast_to<HewnCurve3D>(get_node_or_null(taper_object));
	if (tc && tc != this && !tc->splines.empty()) {
		std::vector<hewn::curves::Sample> smp;
		hewn::curves::sample(tc->splines[0], 8, smp);
		if (smp.size() >= 2) {
			std::vector<std::pair<float, float>> xy;
			bool flat = true;
			for (const hewn::curves::Sample &q : smp) {
				flat = flat && std::fabs(q.p.y) < 1e-4f;
			}
			for (const hewn::curves::Sample &q : smp) {
				xy.push_back({ q.p.x, flat ? -q.p.z : q.p.y });
			}
			std::sort(xy.begin(), xy.end());
			const float x0 = xy.front().first, x1 = xy.back().first;
			for (int i = 0; i <= 32; i++) {
				const float x = x0 + (x1 - x0) * i / 32.0f;
				size_t j = 1;
				while (j + 1 < xy.size() && xy[j].first < x) {
					j++;
				}
				const float span = xy[j].first - xy[j - 1].first;
				const float t = span > 1e-6f ? CLAMP((x - xy[j - 1].first) / span, 0.0f, 1.0f) : 0.0f;
				g.taper.push_back(std::max(0.0f, xy[j - 1].second + (xy[j].second - xy[j - 1].second) * t));
			}
		}
	}
	// a road: its height from the ground under it, if it follows that
	GroundGrid ground;
	if (g.road == 2) {
		return hewn::Mesh(); // (an overlay: laid on the meshes, no geometry here)
	}
	if (g.road == 1) {
		g.road_length = _road_length();
		g.road_side_slot = road_side_material.is_valid();
		if (is_inside_tree()) {
			const Transform3D gx = get_global_transform();
			const Transform3D inv = gx.affine_inverse();
			g.road_up = inv.basis.xform(Vector3(0, 1, 0)).normalized();
			if (road_ground_snap) {
				ground.build(_road_meshes());
				g.road_step = MAX(0.05f, road_step);
				const float reach = road_reach, lift = road_lift, width = g.road_width;
				g.road_snap = [&ground, gx, inv, reach, lift, width](const Vector3 &p_at, const Vector3 &p_center, Vector3 &r_ground) {
					const Vector3 a = gx.xform(p_at), c = gx.xform(p_center);
					float y;
					if (!ground.below(a.x, a.z, c.y + width + 0.1f, c.y - reach, y)) {
						return false;
					}
					r_ground = inv.xform(Vector3(a.x, y + lift, a.z));
					return true;
				};
			}
		}
	}
	hewn::Mesh m;
	hewn::curves::build_geometry(splines, g, m);
	return m;
}

// ---- vertex paint

#define HW_PAINT_SET(m_name, m_type, m_expr)          \
	void HewnCurve3D::set_##m_name(m_type p_v) {      \
		m_name = m_expr;                              \
		_repaint_meshes();                            \
	}
HW_PAINT_SET(paint_enabled, bool, p_v)
HW_PAINT_SET(paint_channel, int, CLAMP(p_v, 0, 4))
HW_PAINT_SET(paint_color, const Color &, p_v)
HW_PAINT_SET(paint_width, float, MAX(0.0f, p_v))
HW_PAINT_SET(paint_falloff, float, MAX(0.0f, p_v))
HW_PAINT_SET(paint_strength, float, CLAMP(p_v, 0.0f, 1.0f))
HW_PAINT_SET(paint_height, float, MAX(0.01f, p_v))
#undef HW_PAINT_SET

AABB HewnCurve3D::_paint_reach() const {
	AABB box;
	bool first = true;
	const Transform3D g = get_global_transform();
	for (int si = 0; si < (int)splines.size(); si++) {
		for (const Vector3 &p : sampled_points(si)) {
			const Vector3 w = g.xform(p);
			box = first ? AABB(w, Vector3()) : box.expand(w);
			first = false;
		}
	}
	const float r = paint_width * 0.5f + paint_falloff;
	return first ? AABB() : box.grow(MAX(r, paint_height));
}

// The meshes it paints now and the ones it painted before: their arrays made again (each with every curve's paint).
void HewnCurve3D::_repaint_meshes() {
	if (!is_inside_tree()) {
		return;
	}
	const AABB now = paint_enabled ? _paint_reach() : AABB();
	for (HewnMesh3D *m : HewnMesh3D::all_meshes) {
		if (!m->is_inside_tree() || m->has_meta("_hw_curve_mesh") || m->get_mesh().is_null()) {
			continue;
		}
		const AABB mb = m->get_global_transform().xform(m->get_mesh()->get_aabb()).grow(0.01f);
		if ((paint_enabled && mb.intersects(now)) || (paint_had && mb.intersects(paint_box))) {
			road_repainting = true; // (its own paint is no change of the ground)
			m->commit(false);
			road_repainting = false;
		}
	}
	paint_box = now;
	paint_had = paint_enabled;
}

void HewnCurve3D::paint_vertices(const Transform3D &p_global, const std::vector<Vector3> &p_verts, std::vector<Color> &r_colors, std::vector<Color> &r_customs, int &r_channels) {
	for (HewnCurve3D *c : all) {
		if (!c->paint_enabled || !c->is_inside_tree() || p_verts.empty()) {
			continue;
		}
		const AABB reach = c->_paint_reach();
		const Transform3D g = c->get_global_transform();
		std::vector<std::pair<Vector3, Vector3>> segs;
		for (int si = 0; si < (int)c->splines.size(); si++) {
			std::vector<Vector3> l = c->sampled_points(si);
			if (c->splines[si].cyclic && l.size() > 2) {
				l.push_back(l.front());
			}
			for (size_t i = 0; i + 1 < l.size(); i++) {
				segs.push_back({ g.xform(l[i]), g.xform(l[i + 1]) });
			}
		}
		if (segs.empty()) {
			continue;
		}
		const float half = c->paint_width * 0.5f, fall = c->paint_falloff;
		const int ch = c->paint_channel;
		bool any = false;
		for (size_t v = 0; v < p_verts.size(); v++) {
			const Vector3 w = p_global.xform(p_verts[v]);
			if (!reach.has_point(w)) {
				continue;
			}
			// the distance across (on the ground, X and Z) to the nearest point of it, within its height above / below
			float best = 1e30f;
			for (const auto &sg : segs) {
				const Vector2 a(sg.first.x, sg.first.z), b(sg.second.x, sg.second.z), p(w.x, w.z);
				const Vector2 ab = b - a;
				const float k = ab.length_squared() > 1e-12f ? CLAMP((p - a).dot(ab) / ab.length_squared(), 0.0f, 1.0f) : 0.0f;
				const float y = sg.first.y + (sg.second.y - sg.first.y) * k;
				if (std::fabs(w.y - y) > c->paint_height) {
					continue;
				}
				best = MIN(best, p.distance_to(a + ab * k));
			}
			if (best > half + fall) {
				continue;
			}
			const float wgt = c->paint_strength * (best <= half || fall <= 1e-6f ? 1.0f : 1.0f - (best - half) / fall);
			if (wgt <= 0.0f) {
				continue;
			}
			any = true;
			if (ch == 0) {
				if (r_colors.size() < p_verts.size()) {
					r_colors.resize(p_verts.size(), Color(1, 1, 1, 1));
				}
				r_colors[v] = r_colors[v].lerp(c->paint_color, wgt);
			} else {
				if (r_customs.size() < p_verts.size() * 4) {
					r_customs.resize(p_verts.size() * 4, Color(0, 0, 0, 0));
				}
				Color &cv = r_customs[v * 4 + ch - 1];
				cv = cv.lerp(c->paint_color, wgt);
			}
		}
		if (any) {
			r_channels |= 1 << ch;
		}
	}
}

bool HewnCurve3D::_drops() const {
	for (int64_t i = 0; i < placements.size(); i++) {
		const Ref<HewnPathPlacement> pp = placements[i];
		if (pp.is_valid() && pp->get_enabled() && pp->get_drop_to_ground()) {
			return true;
		}
	}
	return false;
}

void HewnCurve3D::set_placements(const TypedArray<Resource> &p_v) {
	const Callable c = callable_mp(this, &HewnCurve3D::_queue);
	for (int64_t i = 0; i < placements.size(); i++) {
		Ref<Resource> r = placements[i];
		if (r.is_valid() && r->is_connected("changed", c)) {
			r->disconnect("changed", c);
		}
	}
	placements = p_v;
	for (int64_t i = 0; i < placements.size(); i++) {
		Ref<Resource> r = placements[i];
		if (r.is_null() && Engine::get_singleton()->is_editor_hint()) {
			// (a new slot in the Inspector: a placement to fill in)
			Ref<HewnPathPlacement> np;
			np.instantiate();
			placements[i] = np;
			r = np;
		}
		if (r.is_valid() && !r->is_connected("changed", c)) {
			r->connect("changed", c);
		}
	}
	_queue();
}

int HewnCurve3D::get_placed_count(int p_placement) const {
	if (p_placement < 0 || p_placement >= (int)placed.size()) {
		return 0;
	}
	int n = 0;
	for (MultiMeshInstance3D *m : placed[p_placement]) {
		n += m->get_multimesh().is_valid() ? m->get_multimesh()->get_instance_count() : 0;
	}
	return n;
}

// The copies of each placement along the splines (one MultiMesh each); Drop to Ground: onto the Hewn meshes under them.
void HewnCurve3D::_rebuild_placements() {
	for (const std::vector<MultiMeshInstance3D *> &list : placed) {
		for (MultiMeshInstance3D *m : list) {
			remove_child(m);
			m->queue_free();
		}
	}
	placed.clear();
	if (placements.is_empty()) {
		return;
	}
	std::vector<std::vector<Vector3>> lines;
	std::vector<bool> cyc;
	for (int si = 0; si < (int)splines.size(); si++) {
		lines.push_back(sampled_points(si));
		cyc.push_back(splines[si].cyclic);
	}
	const Transform3D g = get_global_transform();
	// (the ground: the triangles of the Hewn meshes shown, not this curve's own)
	std::vector<std::pair<AABB, PackedVector3Array>> ground;
	bool ground_made = false;
	auto ray = [&](const Vector3 &p_from, const Vector3 &p_dir, Vector3 &r_hit, Vector3 &r_n) {
		if (!ground_made) {
			ground_made = true;
			for (HewnMesh3D *m : HewnMesh3D::all_meshes) {
				if (!m->is_inside_tree() || !m->is_visible_in_tree() || m == shown || m->get_mesh().is_null() || m->has_meta("_hw_curve_mesh")) {
					continue;
				}
				const Transform3D mg = m->get_global_transform();
				PackedVector3Array f = m->get_mesh()->get_faces();
				for (int64_t i = 0; i < f.size(); i++) {
					f.set(i, mg.xform(f[i]));
				}
				ground.push_back({ mg.xform(m->get_mesh()->get_aabb()), f });
			}
		}
		float best = 1e30f;
		for (const auto &gr : ground) {
			if (!gr.first.grow(0.01f).intersects_ray(p_from, p_dir)) {
				continue;
			}
			const PackedVector3Array &f = gr.second;
			for (int64_t i = 0; i + 2 < f.size(); i += 3) {
				const Vector3 e1 = f[i + 1] - f[i], e2 = f[i + 2] - f[i];
				const Vector3 pv = p_dir.cross(e2);
				const float det = e1.dot(pv);
				if (std::fabs(det) < 1e-12f) {
					continue;
				}
				const float id = 1.0f / det;
				const Vector3 tv = p_from - f[i];
				const float u = tv.dot(pv) * id;
				if (u < 0.0f || u > 1.0f) {
					continue;
				}
				const Vector3 qv = tv.cross(e1);
				const float v = p_dir.dot(qv) * id;
				if (v < 0.0f || u + v > 1.0f) {
					continue;
				}
				const float t = e2.dot(qv) * id;
				if (t > 0.0f && t < best) {
					best = t;
					r_n = e1.cross(e2).normalized();
				}
			}
		}
		if (best >= 1e29f) {
			return false;
		}
		r_hit = p_from + p_dir * best;
		if (r_n.dot(p_dir) > 0.0f) {
			r_n = -r_n;
		}
		return true;
	};
	for (int64_t i = 0; i < placements.size(); i++) {
		const Ref<HewnPathPlacement> pp = placements[i];
		std::vector<MultiMeshInstance3D *> list;
		if (pp.is_valid() && pp->get_enabled() && pp->get_mesh().is_valid()) {
			const std::vector<Transform3D> xs = pp->transforms(lines, cyc, g, ray);
			// by squares of the map, as the scatter's: what is out of view is not drawn
			const float cs = pp->get_chunk_size();
			std::map<uint64_t, std::vector<Transform3D>> parts;
			for (const Transform3D &x : xs) {
				const uint64_t key = cs > 1e-3f ? (((uint64_t)(uint32_t)(int32_t)std::floor(x.origin.x / cs) << 32) | (uint32_t)(int32_t)std::floor(x.origin.z / cs)) : 0;
				parts[key].push_back(x);
			}
			const float vr = pp->get_visibility_range();
			for (const auto &kv : parts) {
				Ref<MultiMesh> mm;
				mm.instantiate();
				mm->set_transform_format(MultiMesh::TRANSFORM_3D);
				mm->set_mesh(pp->get_mesh());
				mm->set_instance_count((int)kv.second.size());
				for (int k = 0; k < (int)kv.second.size(); k++) {
					mm->set_instance_transform(k, kv.second[k]);
				}
				MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
				mmi->set_name(vformat("_Placed%d_%d", (int)i, (int)list.size()));
				mmi->set_multimesh(mm);
				mmi->set_cast_shadows_setting(pp->get_cast_shadows() ? GeometryInstance3D::SHADOW_CASTING_SETTING_ON : GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
				mmi->set_visibility_range_end(vr);
				mmi->set_visibility_range_end_margin(vr > 0.0f ? vr * 0.1f : 0.0f);
				mmi->set_visibility_range_fade_mode(vr > 0.0f ? GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF : GeometryInstance3D::VISIBILITY_RANGE_FADE_DISABLED);
				if (!Engine::get_singleton()->is_editor_hint()) {
					hewn::apply_tags(mmi, pp->get_game_metadata(), pp->get_game_groups());
					hewn::apply_script(mmi, pp->get_mesh_script());
					if (StaticBody3D *body = _placement_body(pp, mm)) {
						mmi->add_child(body); // (run without baking: the same bodies as the baked level's)
					}
				}
				add_child(mmi, false, INTERNAL_MODE_BACK);
				list.push_back(mmi);
			}
		}
		placed.push_back(list);
	}
}

void HewnCurve3D::set_collision(bool p_v) {
	collision = p_v;
	if (is_inside_tree()) {
		rebuild();
	}
}

void HewnCurve3D::rebuild() {
	queued = false;
	if (!is_inside_tree()) {
		return;
	}
	dep_key = _deps_key();
	if (shown) {
		shown->get_data() = geometry_mesh();
		TypedArray<Material> mats;
		mats.push_back(material);
		if (geo.road == 1 && road_side_material.is_valid()) {
			mats.push_back(road_side_material);
		}
		shown->set_materials(mats);
		if (shown->get_modifiers() != modifiers) {
			shown->set_modifiers(modifiers);
		}
		shown->set_collision_metadata(game_metadata); // (its collision body's, here and in the game)
		shown->set_collision_groups(game_groups);
		shown->set_collision_script(collision_script);
		shown->set_collision_layer(collision_layer);
		shown->set_collision_mask(collision_mask);
		// (its collision in the editor too: the overlay, the Collision view, the physics placement see it)
		const HewnMesh3D::CollisionMode cm = collision ? HewnMesh3D::COLLISION_TRIMESH : HewnMesh3D::COLLISION_NONE;
		if (shown->get_collision_mode() != cm) {
			shown->set_collision_mode(cm);
		}
		shown->commit(false);
		shown->set_visible(!shown->get_data().faces.empty());
	}
	_rebuild_road();
	_rebuild_placements();
	_repaint_meshes();
	// the shapes that follow it (walls, sweeps, stairs along it) and the curves that use this one (as their cross section
	// or taper) made again too
	for (HewnMesh3D *m : HewnMesh3D::all_meshes) {
		const NodePath fp = m->get_follow_curve();
		if (!fp.is_empty() && m->is_inside_tree() && m->get_node_or_null(fp) == this) {
			m->follow_changed();
		}
	}
	for (HewnCurve3D *o : all) {
		if (o == this || !o->is_inside_tree()) {
			continue;
		}
		for (const NodePath &np : { o->bevel_object, o->taper_object }) {
			if (!np.is_empty() && o->get_node_or_null(np) == this) {
				o->_queue();
			}
		}
	}
}

std::vector<Vector3> HewnCurve3D::sampled_points(int p_spline, int p_res) const {
	std::vector<Vector3> out;
	if (p_spline < 0 || p_spline >= (int)splines.size()) {
		return out;
	}
	std::vector<hewn::curves::Sample> smp;
	hewn::curves::sample(splines[p_spline], p_res > 0 ? p_res : geo.resolution, smp);
	for (const hewn::curves::Sample &q : smp) {
		out.push_back(geo.is_2d ? Vector3(q.p.x, 0.0f, q.p.z) : q.p);
	}
	return out;
}

void HewnCurve3D::set_splines_data(const Array &p_data) {
	splines.clear();
	for (int64_t i = 0; i < p_data.size(); i++) {
		splines.push_back(hewn::curves::spline_from_dict(p_data[i]));
	}
	changed();
}

Array HewnCurve3D::get_splines_data() const {
	Array a;
	for (const hewn::curves::Spline &s : splines) {
		a.push_back(hewn::curves::spline_to_dict(s));
	}
	return a;
}

Dictionary HewnCurve3D::get_state() const {
	Dictionary d;
	d["splines"] = get_splines_data();
	// (which points are selected: an undo, or a change in the panel of what was just done, keeps them)
	PackedByteArray sel;
	for (const hewn::curves::Spline &s : splines) {
		for (const hewn::curves::CPoint &p : s.pts) {
			sel.push_back(p.sel ? 1 : 0);
		}
	}
	d["sel"] = sel;
	return d;
}

void HewnCurve3D::set_state(const Dictionary &p_state) {
	set_splines_data(p_state.get("splines", Array()));
	const PackedByteArray sel = p_state.get("sel", PackedByteArray());
	int64_t k = 0;
	for (hewn::curves::Spline &s : splines) {
		for (hewn::curves::CPoint &p : s.pts) {
			p.sel = k < sel.size() && sel[k] != 0;
			k++;
		}
	}
}

#define HW_CURVE_SET(m_type, m_name, m_field, m_expr) \
	void HewnCurve3D::set_##m_name(m_type p_v) {     \
		m_field = m_expr;                             \
		_queue();                                     \
	}
HW_CURVE_SET(int, resolution, geo.resolution, CLAMP(p_v, 1, 64))
HW_CURVE_SET(float, offset, geo.offset, p_v)
HW_CURVE_SET(float, extrude, geo.extrude, MAX(0.0f, p_v))
HW_CURVE_SET(float, depth, geo.depth, MAX(0.0f, p_v))
HW_CURVE_SET(int, bevel_resolution, geo.bevel_resolution, CLAMP(p_v, 0, 32))
HW_CURVE_SET(bool, fill_caps, geo.fill_caps, p_v)
HW_CURVE_SET(int, fill_mode, geo.fill_mode, CLAMP(p_v, 0, 3))
HW_CURVE_SET(bool, smooth, geo.smooth, p_v)
HW_CURVE_SET(const NodePath &, taper_object, taper_object, p_v)
HW_CURVE_SET(bool, world_uv, geo.world_uv, p_v)
HW_CURVE_SET(const Vector2 &, uv_scale, geo.uv_scale, p_v)
HW_CURVE_SET(bool, uv_turn, geo.uv_turn, p_v)
HW_CURVE_SET(float, road_width, geo.road_width, MAX(0.01f, p_v))
HW_CURVE_SET(float, road_repeat, road_repeat, MAX(0.0f, p_v))
HW_CURVE_SET(int, road_segments, geo.road_segments, CLAMP(p_v, 1, 64))
HW_CURVE_SET(bool, road_bottom, geo.road_bottom, p_v)
HW_CURVE_SET(float, road_curb_width, geo.road_curb_width, MAX(0.0f, p_v))
HW_CURVE_SET(float, road_lift, road_lift, p_v)
HW_CURVE_SET(float, road_reach, road_reach, MAX(0.01f, p_v))
HW_CURVE_SET(float, road_step, road_step, MAX(0.05f, p_v))
HW_CURVE_SET(float, road_softness, road_softness, MAX(0.0f, p_v))
HW_CURVE_SET(int, road_order, road_order, CLAMP(p_v, -128, 127))
#undef HW_CURVE_SET

void HewnCurve3D::set_game_metadata(const Dictionary &p_v) {
	game_metadata = p_v;
	_queue();
}

void HewnCurve3D::set_game_groups(const PackedStringArray &p_v) {
	game_groups = p_v;
	_queue();
}

void HewnCurve3D::set_collision_script(const Ref<Script> &p_v) {
	collision_script = p_v;
	_queue();
}

void HewnCurve3D::set_material(const Ref<Material> &p_v) {
	const Callable c = callable_mp(this, &HewnCurve3D::_road_source_changed);
	if (material.is_valid() && material->is_connected("changed", c)) {
		material->disconnect("changed", c);
	}
	material = p_v;
	if (material.is_valid()) {
		material->connect("changed", c);
	}
	_queue();
}

void HewnCurve3D::set_road_thickness(float p_v) {
	const int shows = _road_shows();
	geo.road_thickness = MAX(0.0f, p_v);
	if (_road_shows() != shows) {
		notify_property_list_changed();
	}
	_queue();
}

void HewnCurve3D::set_road_curb_height(float p_v) {
	const int shows = _road_shows();
	geo.road_curb_height = MAX(0.0f, p_v);
	if (_road_shows() != shows) {
		notify_property_list_changed();
	}
	_queue();
}

void HewnCurve3D::set_modifiers(const TypedArray<HewnModifier> &p_v) {
	const Callable cb = callable_mp(this, &HewnCurve3D::_on_modifiers_changed);
	for (int64_t i = 0; i < modifiers.size(); i++) {
		Ref<HewnModifier> m = modifiers[i];
		if (m.is_valid() && m->is_connected("changed", cb)) {
			m->disconnect("changed", cb);
		}
	}
	modifiers = p_v.duplicate();
	for (int64_t i = 0; i < modifiers.size(); i++) {
		Ref<HewnModifier> m = modifiers[i];
		if (m.is_valid() && !m->is_connected("changed", cb)) {
			m->connect("changed", cb);
		}
	}
	_queue();
}

void HewnCurve3D::set_collision_layer(uint32_t p_v) {
	collision_layer = p_v;
	_queue();
}

void HewnCurve3D::set_collision_mask(uint32_t p_v) {
	collision_mask = p_v;
	_queue();
}

#define HW_ROAD_EDGE_SET(m_type, m_name, m_expr) \
	void HewnCurve3D::set_##m_name(m_type p_v) {  \
		m_name = m_expr;                          \
		road_mat.unref();                         \
		_queue();                                 \
	}
HW_ROAD_EDGE_SET(float, road_edge_noise, CLAMP(p_v, 0.0f, 1.0f))
HW_ROAD_EDGE_SET(float, road_edge_noise_size, MAX(0.01f, p_v))
HW_ROAD_EDGE_SET(float, road_edge_height, CLAMP(p_v, 0.0f, 1.0f))
HW_ROAD_EDGE_SET(const Ref<Texture2D> &, road_edge_height_map, p_v)
#undef HW_ROAD_EDGE_SET

void HewnCurve3D::set_road_side_material(const Ref<Material> &p_v) {
	road_side_material = p_v;
	_queue();
}

void HewnCurve3D::set_road_mode(int p_v) {
	geo.road = CLAMP(p_v, 0, 2);
	notify_property_list_changed();
	_queue();
}

void HewnCurve3D::set_road_snap(bool p_v) {
	road_ground_snap = p_v;
	notify_property_list_changed();
	_queue();
}

void HewnCurve3D::set_bevel_mode(int p_v) {
	geo.bevel_mode = CLAMP(p_v, 0, 2);
	notify_property_list_changed();
	_queue();
}

void HewnCurve3D::set_is_2d(bool p_v) {
	geo.is_2d = p_v;
	notify_property_list_changed();
	_queue();
}

void HewnCurve3D::set_bevel_object(const NodePath &p_v) {
	bevel_object = p_v;
	_queue();
}

void HewnCurve3D::set_bevel_profile(const Ref<HewnProfile> &p_v) {
	const Callable c = callable_mp(this, &HewnCurve3D::_queue);
	if (bevel_profile.is_valid() && bevel_profile->is_connected("changed", c)) {
		bevel_profile->disconnect("changed", c);
	}
	bevel_profile = p_v;
	if (bevel_profile.is_valid()) {
		bevel_profile->connect("changed", c);
	}
	_queue();
}

int HewnCurve3D::get_point_count_of(int p_spline) const {
	return p_spline >= 0 && p_spline < (int)splines.size() ? (int)splines[p_spline].pts.size() : 0;
}

Vector3 HewnCurve3D::get_point_of(int p_spline, int p_point) const {
	if (p_spline < 0 || p_spline >= (int)splines.size() || p_point < 0 || p_point >= (int)splines[p_spline].pts.size()) {
		return Vector3();
	}
	return splines[p_spline].pts[p_point].pos;
}

void HewnCurve3D::add_spline(int p_type, const PackedVector3Array &p_points, bool p_cyclic) {
	hewn::curves::Spline s;
	s.type = CLAMP(p_type, 0, 2);
	s.cyclic = p_cyclic;
	for (const Vector3 &p : p_points) {
		hewn::curves::CPoint c;
		c.pos = p;
		s.pts.push_back(c);
	}
	hewn::curves::recalc_handles(s, true);
	splines.push_back(s);
	changed();
}

void HewnCurve3D::clear_splines() {
	splines.clear();
	changed();
}

int HewnCurve3D::get_face_count() const {
	return shown ? (int)shown->get_data().faces.size() : 0;
}

void HewnCurve3D::make_preset(int p_kind, float p_size) {
	splines.clear();
	hewn::curves::Spline s;
	const float r = p_size;
	if (p_kind == 1 || p_kind == 3) {
		// a circle: four points (Bezier: round handles), eight for NURBS
		s.type = p_kind == 1 ? hewn::curves::BEZIER : hewn::curves::NURBS;
		s.cyclic = true;
		const int n = p_kind == 1 ? 4 : 8;
		for (int i = 0; i < n; i++) {
			const float a = 6.2831853f * i / n;
			hewn::curves::CPoint c;
			c.pos = Vector3(std::cos(a) * r, 0.0f, -std::sin(a) * r);
			if (p_kind == 3 && (i % 2) == 1) {
				c.pos *= 1.41421356f; // (a NURBS circle: its corners out, weighted)
				c.weight = 0.70710678f;
			}
			s.pts.push_back(c);
		}
		if (p_kind == 1) {
			// (the exact round handles of a four point circle)
			const float k = 0.5522847f * r;
			for (int i = 0; i < n; i++) {
				const float a = 6.2831853f * i / n;
				const Vector3 t(-std::sin(a), 0.0f, -std::cos(a));
				s.pts[i].in = -t * k;
				s.pts[i].out = t * k;
				s.pts[i].hl = s.pts[i].hr = hewn::curves::H_ALIGNED;
			}
		} else {
			s.order = 3;
		}
	} else {
		s.type = p_kind == 2 ? hewn::curves::NURBS : hewn::curves::BEZIER;
		const Vector3 pts[4] = { Vector3(-1.5f, 0, 0), Vector3(-0.5f, 0, -0.8f), Vector3(0.5f, 0, 0.8f), Vector3(1.5f, 0, 0) };
		const int n = p_kind == 2 ? 4 : 2;
		for (int i = 0; i < n; i++) {
			hewn::curves::CPoint c;
			c.pos = (p_kind == 2 ? pts[i] : (i == 0 ? Vector3(-1, 0, 0) : Vector3(1, 0, 0))) * r;
			s.pts.push_back(c);
		}
		if (p_kind == 0) {
			s.pts[0].out = Vector3(0.5f, 0, -0.5f) * r;
			s.pts[0].in = -s.pts[0].out;
			s.pts[1].in = Vector3(-0.5f, 0, 0.5f) * r;
			s.pts[1].out = -s.pts[1].in;
			s.pts[0].hl = s.pts[0].hr = s.pts[1].hl = s.pts[1].hr = hewn::curves::H_ALIGNED;
		}
	}
	splines.push_back(s);
	changed();
}

Ref<ArrayMesh> HewnCurve3D::get_baked_mesh() {
	if (queued) {
		rebuild();
	}
	if (!shown || shown->get_data().faces.empty()) {
		return Ref<ArrayMesh>();
	}
	return shown->to_array_mesh();
}

// The collision of a chunk of copies: a shape at each one. None when the placement has none.
StaticBody3D *HewnCurve3D::_placement_body(const Ref<HewnPathPlacement> &p_placement, const Ref<MultiMesh> &p_copies) const {
	const int kind = p_placement->get_collision();
	if (kind == HewnPathPlacement::COLLISION_NONE || p_placement->get_mesh().is_null() || p_copies->get_instance_count() == 0) {
		return nullptr;
	}
	Ref<Shape3D> sh;
	Vector3 center;
	if (kind == HewnPathPlacement::COLLISION_BOX) {
		Ref<BoxShape3D> bs;
		bs.instantiate();
		const AABB box = p_placement->get_mesh()->get_aabb();
		bs->set_size(box.size.max(Vector3(0.001f, 0.001f, 0.001f)));
		center = box.get_center();
		sh = bs;
	} else if (kind == HewnPathPlacement::COLLISION_CONVEX) {
		sh = p_placement->get_mesh()->create_convex_shape(true, true);
	} else if (kind == HewnPathPlacement::COLLISION_CUSTOM) {
		sh = p_placement->get_custom_shape();
		center = p_placement->get_custom_shape_offset();
	} else {
		sh = p_placement->get_mesh()->create_trimesh_shape();
	}
	if (sh.is_null()) {
		return nullptr;
	}
	StaticBody3D *sb = memnew(StaticBody3D);
	sb->set_name("StaticBody3D");
	hewn::apply_tags(sb, p_placement->get_game_metadata(), p_placement->get_game_groups());
	hewn::apply_script(sb, p_placement->get_collision_script());
	sb->set_collision_layer(p_placement->get_collision_layer());
	sb->set_collision_mask(p_placement->get_collision_mask());
	for (int k = 0; k < p_copies->get_instance_count(); k++) {
		CollisionShape3D *cs = memnew(CollisionShape3D);
		cs->set_shape(sh);
		cs->set_transform(p_copies->get_instance_transform(k) * Transform3D(Basis(), center));
		sb->add_child(cs);
	}
	return sb;
}

Node *HewnCurve3D::make_game_node() {
	_sync_path_curve();
	Path3D *p = memnew(Path3D);
	p->set_name(get_name());
	p->set_transform(get_transform());
	p->set_visible(is_visible());
	if (get_curve().is_valid()) {
		p->set_curve(get_curve()->duplicate());
	}
	if (queued) {
		rebuild();
	}
	hewn::apply_tags(p, game_metadata, game_groups);
	struct PlacedCopy {
		int64_t index;
		MultiMeshInstance3D *src;
		int part;
	};
	std::vector<PlacedCopy> copies;
	for (int64_t i = 0; i < (int64_t)placed.size() && i < placements.size(); i++) {
		for (int k = 0; k < (int)placed[i].size(); k++) {
			copies.push_back({ i, placed[i][k], k });
		}
	}
	for (const PlacedCopy &pc : copies) {
		const int64_t i = pc.index;
		const Ref<HewnPathPlacement> pp = placements[i];
		MultiMeshInstance3D *src = pc.src;
		if (!src || src->get_multimesh().is_null() || pp.is_null()) {
			continue;
		}
		MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
		mmi->set_name(vformat("Placed%d_%d", (int)i + 1, pc.part + 1));
		hewn::apply_tags(mmi, pp->get_game_metadata(), pp->get_game_groups());
		hewn::apply_script(mmi, pp->get_mesh_script());
		mmi->set_visibility_range_end(src->get_visibility_range_end());
		mmi->set_visibility_range_end_margin(src->get_visibility_range_end_margin());
		mmi->set_visibility_range_fade_mode(src->get_visibility_range_fade_mode());
		// (made anew: a MultiMesh copied takes its buffer before its count)
		const Ref<MultiMesh> from = src->get_multimesh();
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_mesh(from->get_mesh());
		mm->set_instance_count(from->get_instance_count());
		for (int k = 0; k < from->get_instance_count(); k++) {
			mm->set_instance_transform(k, from->get_instance_transform(k));
		}
		mmi->set_multimesh(mm);
		mmi->set_cast_shadows_setting(src->get_cast_shadows_setting());
		p->add_child(mmi);
		if (StaticBody3D *sb = _placement_body(pp, mm)) {
			mmi->add_child(sb);
		}
	}
	if (road_overlay && road_overlay->get_triangle_count() > 0) {
		// the road laid on the meshes: as it is now, in the curve's space
		const Array wa = road_overlay->get_world_arrays();
		const Transform3D inv = get_global_transform().affine_inverse();
		const Basis nb = inv.basis.inverse().transposed();
		PackedVector3Array rv = wa[0], rn = wa[1];
		for (int64_t i = 0; i < rv.size(); i++) {
			rv.set(i, inv.xform(rv[i]));
			rn.set(i, nb.xform(rn[i]).normalized());
		}
		Array arr;
		arr.resize(Mesh::ARRAY_MAX);
		arr[Mesh::ARRAY_VERTEX] = rv;
		arr[Mesh::ARRAY_NORMAL] = rn;
		arr[Mesh::ARRAY_TEX_UV] = wa[2];
		arr[Mesh::ARRAY_INDEX] = wa[3];
		const PackedColorArray rc = wa[4];
		if (!rc.is_empty()) {
			arr[Mesh::ARRAY_COLOR] = rc;
		}
		Ref<ArrayMesh> ram;
		ram.instantiate();
		ram->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr);
		ram->surface_set_material(0, road_overlay->get_draw_material());
		MeshInstance3D *rmi = memnew(MeshInstance3D);
		rmi->set_name("Road");
		hewn::apply_tags(rmi, game_metadata, game_groups);
		hewn::apply_script(rmi, mesh_script);
		if (merge_at_bake) {
			rmi->set_meta("_hw_merge", true);
		}
		rmi->set_mesh(ram);
		rmi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		p->add_child(rmi);
	}
	if (shown && !shown->get_data().faces.empty()) {
		MeshInstance3D *mi = memnew(MeshInstance3D);
		mi->set_name("Mesh");
		const Ref<ArrayMesh> am = shown->to_array_mesh();
		mi->set_mesh(am);
		hewn::apply_tags(mi, game_metadata, game_groups);
		hewn::apply_script(mi, mesh_script);
		if (merge_at_bake) {
			mi->set_meta("_hw_merge", true);
		}
		p->add_child(mi);
		if (collision && am.is_valid()) {
			StaticBody3D *sb = memnew(StaticBody3D);
			sb->set_name("StaticBody3D");
			hewn::apply_tags(sb, game_metadata, game_groups);
			hewn::apply_script(sb, collision_script);
			sb->set_collision_layer(collision_layer);
			sb->set_collision_mask(collision_mask);
			CollisionShape3D *cs = memnew(CollisionShape3D);
			cs->set_name("CollisionShape3D");
			cs->set_shape(am->create_trimesh_shape());
			sb->add_child(cs);
			mi->add_child(sb);
		}
	}
	return p;
}

void HewnCurve3D::_validate_property(PropertyInfo &p_property) const {
	const String pn = p_property.name;
	if ((pn == "bevel_object" && geo.bevel_mode != 1) || (pn == "bevel_profile" && geo.bevel_mode != 2) || (pn == "bevel_resolution" && geo.bevel_mode == 1) ||
			(pn == "fill_mode" && !geo.is_2d)) {
		p_property.usage = PROPERTY_USAGE_STORAGE;
	}
	const bool mesh_road = geo.road == 1, over_road = geo.road == 2;
	if (((pn == "road_width" || pn == "road_repeat") && geo.road == 0) ||
			((pn == "road_segments" || pn == "road_thickness" || pn == "road_curb_height" || pn == "road_snap") && !mesh_road) ||
			(pn == "road_curb_width" && (!mesh_road || geo.road_curb_height <= 1e-5f)) ||
			((pn == "road_bottom" || pn == "road_side_material") && (!mesh_road || _road_shows() == 0)) ||
			(pn == "road_lift" && !(mesh_road && road_ground_snap)) ||
			((pn == "road_reach" || pn == "road_step") && !_road_follows_ground()) ||
			((pn == "road_softness" || pn == "road_order" || pn == "road_edge_noise" || pn == "road_edge_noise_size" || pn == "road_edge_height" || pn == "road_edge_height_map") && !over_road) ||
			(pn == "uv_scale" && geo.road != 0 && !geo.world_uv)) {
		p_property.usage = PROPERTY_USAGE_STORAGE;
	}
	if (pn == "curve") {
		p_property.usage = PROPERTY_USAGE_STORAGE; // (made from the first spline)
	}
}

void HewnCurve3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_splines_data", "data"), &HewnCurve3D::set_splines_data);
	ClassDB::bind_method(D_METHOD("get_splines_data"), &HewnCurve3D::get_splines_data);
	ClassDB::bind_method(D_METHOD("get_state"), &HewnCurve3D::get_state);
	ClassDB::bind_method(D_METHOD("set_state", "state"), &HewnCurve3D::set_state);
	ClassDB::bind_method(D_METHOD("rebuild"), &HewnCurve3D::rebuild);
	ClassDB::bind_method(D_METHOD("get_spline_count"), &HewnCurve3D::get_spline_count);
	ClassDB::bind_method(D_METHOD("get_point_count_of", "spline"), &HewnCurve3D::get_point_count_of);
	ClassDB::bind_method(D_METHOD("get_point_of", "spline", "point"), &HewnCurve3D::get_point_of);
	ClassDB::bind_method(D_METHOD("add_spline", "type", "points", "cyclic"), &HewnCurve3D::add_spline, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("clear_splines"), &HewnCurve3D::clear_splines);
	ClassDB::bind_method(D_METHOD("get_face_count"), &HewnCurve3D::get_face_count);
	ClassDB::bind_method(D_METHOD("make_preset", "kind", "size"), &HewnCurve3D::make_preset, DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("make_game_node"), &HewnCurve3D::make_game_node);
	ClassDB::bind_method(D_METHOD("set_paint_enabled", "v"), &HewnCurve3D::set_paint_enabled);
	ClassDB::bind_method(D_METHOD("get_paint_enabled"), &HewnCurve3D::get_paint_enabled);
	ClassDB::bind_method(D_METHOD("set_paint_channel", "v"), &HewnCurve3D::set_paint_channel);
	ClassDB::bind_method(D_METHOD("get_paint_channel"), &HewnCurve3D::get_paint_channel);
	ClassDB::bind_method(D_METHOD("set_paint_color", "v"), &HewnCurve3D::set_paint_color);
	ClassDB::bind_method(D_METHOD("get_paint_color"), &HewnCurve3D::get_paint_color);
	ClassDB::bind_method(D_METHOD("set_paint_width", "v"), &HewnCurve3D::set_paint_width);
	ClassDB::bind_method(D_METHOD("get_paint_width"), &HewnCurve3D::get_paint_width);
	ClassDB::bind_method(D_METHOD("set_paint_falloff", "v"), &HewnCurve3D::set_paint_falloff);
	ClassDB::bind_method(D_METHOD("get_paint_falloff"), &HewnCurve3D::get_paint_falloff);
	ClassDB::bind_method(D_METHOD("set_paint_strength", "v"), &HewnCurve3D::set_paint_strength);
	ClassDB::bind_method(D_METHOD("get_paint_strength"), &HewnCurve3D::get_paint_strength);
	ClassDB::bind_method(D_METHOD("set_paint_height", "v"), &HewnCurve3D::set_paint_height);
	ClassDB::bind_method(D_METHOD("get_paint_height"), &HewnCurve3D::get_paint_height);
	ClassDB::bind_method(D_METHOD("set_placements", "placements"), &HewnCurve3D::set_placements);
	ClassDB::bind_method(D_METHOD("get_placements"), &HewnCurve3D::get_placements);
	ClassDB::bind_method(D_METHOD("get_placed_count", "placement"), &HewnCurve3D::get_placed_count);
	ClassDB::bind_method(D_METHOD("set_collision", "on"), &HewnCurve3D::set_collision);
	ClassDB::bind_method(D_METHOD("get_collision"), &HewnCurve3D::get_collision);
#define HW_BIND(m_name, m_arg)                                                                                  \
	ClassDB::bind_method(D_METHOD("set_" #m_name, m_arg), &HewnCurve3D::set_##m_name);                    \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &HewnCurve3D::get_##m_name);
	HW_BIND(resolution, "resolution")
	HW_BIND(offset, "offset")
	HW_BIND(extrude, "extrude")
	HW_BIND(bevel_mode, "mode")
	HW_BIND(depth, "depth")
	HW_BIND(bevel_resolution, "resolution")
	HW_BIND(fill_caps, "on")
	HW_BIND(is_2d, "on")
	HW_BIND(fill_mode, "mode")
	HW_BIND(smooth, "on")
	HW_BIND(bevel_object, "path")
	HW_BIND(taper_object, "path")
	HW_BIND(bevel_profile, "profile")
	HW_BIND(material, "material")
	HW_BIND(game_metadata, "metadata")
	HW_BIND(game_groups, "groups")
	HW_BIND(game_script, "script")
	HW_BIND(mesh_script, "script")
	HW_BIND(collision_script, "script")
	HW_BIND(merge_at_bake, "on")
	HW_BIND(world_uv, "on")
	HW_BIND(uv_scale, "scale")
	HW_BIND(uv_turn, "on")
	HW_BIND(road_mode, "mode")
	HW_BIND(road_width, "width")
	HW_BIND(road_repeat, "length")
	HW_BIND(road_snap, "on")
	HW_BIND(road_segments, "segments")
	HW_BIND(road_thickness, "thickness")
	HW_BIND(road_curb_height, "height")
	HW_BIND(road_curb_width, "width")
	HW_BIND(road_bottom, "on")
	HW_BIND(road_side_material, "material")
	HW_BIND(road_lift, "lift")
	HW_BIND(road_reach, "reach")
	HW_BIND(road_step, "step")
	HW_BIND(road_softness, "meters")
	HW_BIND(road_order, "order")
	HW_BIND(road_edge_noise, "amount")
	HW_BIND(road_edge_noise_size, "meters")
	HW_BIND(road_edge_height, "amount")
	HW_BIND(road_edge_height_map, "texture")
	HW_BIND(collision_layer, "layer")
	HW_BIND(modifiers, "modifiers")
	HW_BIND(collision_mask, "mask")
	ClassDB::bind_method(D_METHOD("get_road_overlay"), &HewnCurve3D::get_road_overlay);
#undef HW_BIND
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "splines_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_splines_data", "get_splines_data");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_RANGE, "1,64,1"), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "is_2d"), "set_is_2d", "get_is_2d");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "fill_mode", PROPERTY_HINT_ENUM, "None,Back,Front,Both"), "set_fill_mode", "get_fill_mode");
	ADD_GROUP("Geometry", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "offset", PROPERTY_HINT_RANGE, "-100,100,0.001,or_greater,or_less,suffix:m"), "set_offset", "get_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "extrude", PROPERTY_HINT_RANGE, "0,100,0.001,or_greater,suffix:m"), "set_extrude", "get_extrude");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "taper_object", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "HewnCurve3D"), "set_taper_object", "get_taper_object");
	ADD_GROUP("Bevel", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bevel_mode", PROPERTY_HINT_ENUM, "Round,Object,Profile"), "set_bevel_mode", "get_bevel_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "depth", PROPERTY_HINT_RANGE, "0,10,0.001,or_greater,suffix:m"), "set_depth", "get_depth");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bevel_resolution", PROPERTY_HINT_RANGE, "0,32,1"), "set_bevel_resolution", "get_bevel_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "bevel_object", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "HewnCurve3D"), "set_bevel_object", "get_bevel_object");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "bevel_profile", PROPERTY_HINT_RESOURCE_TYPE, "HewnProfile"), "set_bevel_profile", "get_bevel_profile");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fill_caps"), "set_fill_caps", "get_fill_caps");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "smooth"), "set_smooth", "get_smooth");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "Material"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision"), "set_collision", "get_collision");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "modifiers", PROPERTY_HINT_ARRAY_TYPE,
						 String::num_int64(Variant::OBJECT) + "/" + String::num_int64(PROPERTY_HINT_RESOURCE_TYPE) + ":HewnModifier"),
			"set_modifiers", "get_modifiers");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_GROUP("Game Node", "");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "game_metadata"), "set_game_metadata", "get_game_metadata");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "game_groups"), "set_game_groups", "get_game_groups");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "game_script", PROPERTY_HINT_RESOURCE_TYPE, "Script", PROPERTY_USAGE_EDITOR), "set_game_script", "get_game_script");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesh_script", PROPERTY_HINT_RESOURCE_TYPE, "Script"), "set_mesh_script", "get_mesh_script");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "collision_script", PROPERTY_HINT_RESOURCE_TYPE, "Script"), "set_collision_script", "get_collision_script");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "merge_at_bake"), "set_merge_at_bake", "get_merge_at_bake");
	ADD_GROUP("UV", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "world_uv"), "set_world_uv", "get_world_uv");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "uv_scale", PROPERTY_HINT_LINK), "set_uv_scale", "get_uv_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "uv_turn"), "set_uv_turn", "get_uv_turn");
	ADD_GROUP("Road", "road_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "road_mode", PROPERTY_HINT_ENUM, "Off,Mesh,Overlay"), "set_road_mode", "get_road_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "road_width", PROPERTY_HINT_RANGE, "0.01,100,0.01,or_greater,suffix:m"), "set_road_width", "get_road_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "road_repeat", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_road_repeat", "get_road_repeat");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "road_snap"), "set_road_snap", "get_road_snap");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "road_segments", PROPERTY_HINT_RANGE, "1,64,1"), "set_road_segments", "get_road_segments");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "road_thickness", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:m"), "set_road_thickness", "get_road_thickness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "road_curb_height", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:m"), "set_road_curb_height", "get_road_curb_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "road_curb_width", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:m"), "set_road_curb_width", "get_road_curb_width");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "road_bottom"), "set_road_bottom", "get_road_bottom");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "road_side_material", PROPERTY_HINT_RESOURCE_TYPE, "Material"), "set_road_side_material", "get_road_side_material");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "road_lift", PROPERTY_HINT_RANGE, "-1,1,0.001,or_greater,or_less,suffix:m"), "set_road_lift", "get_road_lift");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "road_reach", PROPERTY_HINT_RANGE, "0.01,200,0.01,or_greater,suffix:m"), "set_road_reach", "get_road_reach");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "road_step", PROPERTY_HINT_RANGE, "0.05,20,0.01,or_greater,suffix:m"), "set_road_step", "get_road_step");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "road_softness", PROPERTY_HINT_RANGE, "0,20,0.01,or_greater,suffix:m"), "set_road_softness", "get_road_softness");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "road_order", PROPERTY_HINT_RANGE, "-128,127,1"), "set_road_order", "get_road_order");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "road_edge_noise", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_road_edge_noise", "get_road_edge_noise");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "road_edge_noise_size", PROPERTY_HINT_RANGE, "0.01,100,0.01,or_greater,suffix:m"), "set_road_edge_noise_size", "get_road_edge_noise_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "road_edge_height", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_road_edge_height", "get_road_edge_height");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "road_edge_height_map", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_road_edge_height_map", "get_road_edge_height_map");
	ADD_GROUP("Vertex Paint", "paint_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "paint_enabled"), "set_paint_enabled", "get_paint_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "paint_channel", PROPERTY_HINT_ENUM, "Color,CUSTOM0,CUSTOM1,CUSTOM2,CUSTOM3"), "set_paint_channel", "get_paint_channel");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "paint_color"), "set_paint_color", "get_paint_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "paint_width", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_paint_width", "get_paint_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "paint_falloff", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_paint_falloff", "get_paint_falloff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "paint_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_paint_strength", "get_paint_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "paint_height", PROPERTY_HINT_RANGE, "0.01,100,0.01,or_greater,suffix:m"), "set_paint_height", "get_paint_height");
	ADD_GROUP("Placement", "");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "placements", PROPERTY_HINT_TYPE_STRING, String::num_int64(Variant::OBJECT) + "/" + String::num_int64(PROPERTY_HINT_RESOURCE_TYPE) + ":HewnPathPlacement"), "set_placements", "get_placements");
}
