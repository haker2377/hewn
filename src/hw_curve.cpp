// Hewn curves: sampling (poly, Bezier, NURBS), frames, the geometry along them; the HewnCurve3D node.
#include "hw_curve.h"
#include "hewn_mesh.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/concave_polygon_shape3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
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
		std::vector<Vector3> T, N;
		frames(smp, cyc, g.is_2d, T, N);
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
			for (int i = 0; i < rings; i++) {
				const Vector3 B = T[i].cross(N[i]).normalized();
				const float sc = smp[i].radius * taper_at(g.taper, along[i] / total);
				const Vector3 c = smp[i].p + B * g.offset;
				for (int j = 0; j < m; j++) {
					out.add_vertex(c + (B * sec[j].x + N[i] * sec[j].y) * sc);
				}
			}
			const int ring_segs = cyc ? rings : rings - 1;
			const int sec_segs = sec_closed ? m : m - 1;
			for (int i = 0; i < ring_segs; i++) {
				const int a = i, b = (i + 1) % rings;
				for (int j = 0; j < sec_segs; j++) {
					const int jn = (j + 1) % m;
					Face f = make_face({ base + a * m + j, base + b * m + j, base + b * m + jn, base + a * m + jn });
					f.smooth = g.smooth ? 1 : 0;
					f.role = ROLE_SIDE;
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
				if (fn.dot(fc - ref) < 0.0f) {
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
	} else if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
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
				c->add_point(p.pos, bz ? p.in : Vector3(), bz ? p.out : Vector3());
				c->set_point_tilt(c->get_point_count() - 1, p.tilt);
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
	hewn::Mesh m;
	hewn::curves::build_geometry(splines, g, m);
	return m;
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
		shown->set_materials(mats);
		shown->commit(false);
		shown->set_visible(!shown->get_data().faces.empty());
	}
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
	return d;
}

void HewnCurve3D::set_state(const Dictionary &p_state) {
	set_splines_data(p_state.get("splines", Array()));
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
HW_CURVE_SET(const Ref<Material> &, material, material, p_v)
#undef HW_CURVE_SET

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
	if (shown && !shown->get_data().faces.empty()) {
		MeshInstance3D *mi = memnew(MeshInstance3D);
		mi->set_name("Mesh");
		const Ref<ArrayMesh> am = shown->to_array_mesh();
		mi->set_mesh(am);
		p->add_child(mi);
		if (collision && am.is_valid()) {
			StaticBody3D *sb = memnew(StaticBody3D);
			sb->set_name("StaticBody3D");
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
}
