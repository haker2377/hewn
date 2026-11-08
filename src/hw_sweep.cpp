// Hewn — a profile swept along a path (rails, curbs, pipes, roads, cornices) and arches bent from
// edges (a vaulted passage out of a cut door).
#include "hw_core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace hewn {

namespace shapes {

namespace {

const float PI_F = 3.14159265358979f;

// The profile in its own plane: x across (width), y up (height); counter-clockwise.
std::vector<Vector2> sweep_profile(int kind, float w, float h, int sides, bool centered, const std::vector<Vector2> *custom) {
	std::vector<Vector2> p;
	if (kind == 6) {
		// custom: its points scaled to the width / height (a closed outline, counter-clockwise); a point twice in a
		// row once (no face without an area)
		std::vector<Vector2> pts;
		if (custom) {
			for (const Vector2 &q : *custom) {
				if (pts.empty() || pts.back().distance_to(q) > 1e-6f) {
					pts.push_back(q);
				}
			}
			while (pts.size() > 1 && pts.front().distance_to(pts.back()) <= 1e-6f) {
				pts.pop_back();
			}
		}
		const std::vector<Vector2> *cu = &pts;
		if (cu->size() >= 3) {
			Vector2 mn = (*cu)[0], mx = mn;
			for (const Vector2 &q : *cu) {
				mn = mn.min(q);
				mx = mx.max(q);
			}
			const Vector2 sz = (mx - mn).max(Vector2(1e-6f, 1e-6f));
			for (const Vector2 &q : *cu) {
				p.push_back(Vector2(((q.x - mn.x) / sz.x - 0.5f) * w, (q.y - mn.y) / sz.y * h));
			}
			float area = 0.0f;
			for (size_t i = 0; i < p.size(); i++) {
				area += p[i].cross(p[(i + 1) % p.size()]);
			}
			if (area < 0.0f) {
				std::reverse(p.begin(), p.end());
			}
			if (centered) {
				for (Vector2 &q : p) {
					q.y -= h * 0.5f;
				}
			}
			return p;
		}
		kind = 1; // (no points yet: a rectangle)
	}
	const float hw = w * 0.5f;
	const float t = std::min(w, h) * 0.35f; // (wall of the L / U shapes)
	switch (kind) {
		case 0: // circle (an ellipse when width != height)
			for (int i = 0; i < sides; i++) {
				const float a = -PI_F * 0.5f + 2.0f * PI_F * i / sides;
				p.push_back(Vector2(std::cos(a) * hw, h * 0.5f + std::sin(a) * h * 0.5f));
			}
			break;
		case 2: { // half round: flat bottom
			const int n = std::max(2, sides / 2);
			for (int i = 0; i <= n; i++) {
				const float a = PI_F * i / n;
				p.push_back(Vector2(std::cos(a) * hw, std::sin(a) * h));
			}
		} break;
		case 3: // L (curb, skirting)
			p = { Vector2(-hw, 0), Vector2(hw, 0), Vector2(hw, t), Vector2(-hw + t, t), Vector2(-hw + t, h), Vector2(-hw, h) };
			break;
		case 4: // cornice: steps out
			p = { Vector2(-hw, 0), Vector2(-hw + w / 3, 0), Vector2(-hw + w / 3, h / 3), Vector2(-hw + w * 2 / 3, h / 3), Vector2(-hw + w * 2 / 3, h * 2 / 3),
				Vector2(hw, h * 2 / 3), Vector2(hw, h), Vector2(-hw, h) };
			break;
		case 5: // U (gutter, channel)
			p = { Vector2(-hw, 0), Vector2(hw, 0), Vector2(hw, h), Vector2(hw - t, h), Vector2(hw - t, t), Vector2(-hw + t, t), Vector2(-hw + t, h), Vector2(-hw, h) };
			break;
		default: // rectangle
			p = { Vector2(-hw, 0), Vector2(hw, 0), Vector2(hw, h), Vector2(-hw, h) };
			break;
	}
	if (centered) {
		for (Vector2 &q : p) {
			q.y -= h * 0.5f;
		}
	}
	return p;
}

// Corners rounded with arcs of `radius` (at most half of the shorter side), `segs` pieces each.
std::vector<Vector3> round_corners(const std::vector<Vector3> &pts, bool closed, float radius, int segs) {
	const int n = (int)pts.size();
	if (radius <= 0.0f || segs < 1 || n < 3) {
		return pts;
	}
	std::vector<Vector3> out;
	for (int i = 0; i < n; i++) {
		const bool end = !closed && (i == 0 || i == n - 1);
		if (end) {
			out.push_back(pts[i]);
			continue;
		}
		const Vector3 p = pts[i], a = pts[(i + n - 1) % n], c = pts[(i + 1) % n];
		const Vector3 da = (a - p), dc = (c - p);
		const float la = da.length(), lc = dc.length();
		if (la < 1e-6f || lc < 1e-6f) {
			out.push_back(p);
			continue;
		}
		const Vector3 ua = da / la, uc = dc / lc;
		const float cosang = std::max(-1.0f, std::min(1.0f, ua.dot(uc)));
		const float ang = std::acos(cosang); // inner angle at the corner
		if (ang > PI_F - 1e-3f || ang < 1e-3f) {
			out.push_back(p);
			continue;
		}
		// tangent distance for the radius, limited by the sides
		float r = radius;
		float dist = r / std::tan(ang * 0.5f);
		const float lim = std::min(la, lc) * 0.5f;
		if (dist > lim) {
			dist = lim;
			r = dist * std::tan(ang * 0.5f);
		}
		const Vector3 t0 = p + ua * dist, t1 = p + uc * dist;
		const Vector3 bis = (ua + uc).normalized();
		const Vector3 center = p + bis * (r / std::sin(ang * 0.5f));
		const Vector3 v0 = t0 - center, v1 = t1 - center;
		const Vector3 axis = v0.cross(v1);
		const float sweep = std::atan2(axis.length(), v0.dot(v1));
		const Vector3 ax = axis.length() > 1e-9f ? axis.normalized() : Vector3(0, 1, 0);
		for (int k = 0; k <= segs; k++) {
			out.push_back(center + v0.rotated(ax, sweep * k / segs));
		}
	}
	return out;
}

} // namespace

void build_sweep(const std::vector<Vector3> &p_path, const std::vector<float> &values, Mesh &out, const std::vector<Vector2> *p_custom, const Mesh *p_place,
		const Mesh *p_accent, const std::vector<int> *p_gaps) {
	out.clear();
	const std::vector<Param> &ps = params(SWEEP);
	std::vector<float> v(ps.size());
	for (size_t i = 0; i < ps.size(); i++) {
		float x = i < values.size() ? values[i] : ps[i].def;
		x = std::max(ps[i].min, std::min(ps[i].max, x));
		v[i] = ps[i].is_int ? std::round(x) : x;
	}
	if (p_gaps && !p_gaps->empty() && p_path.size() >= 2) {
		// ---- segments left out: a sweep of its own for each run between them (open, with its caps)
		const int np = (int)p_path.size();
		const bool cl = v[4] > 0.5f && np >= 3;
		const int ns = cl ? np : np - 1;
		std::vector<char> gap(ns, 0);
		bool any = false;
		for (int g : *p_gaps) {
			if (g >= 0 && g < ns) {
				gap[g] = 1;
				any = true;
			}
		}
		if (any) {
			int start = 0;
			if (cl) {
				while (!gap[start]) {
					start++;
				}
				start = (start + 1) % ns; // (a loop: from just after a gap)
			}
			std::vector<float> vr = v;
			vr[4] = 0.0f;
			std::vector<Vector3> run;
			auto flush = [&]() {
				if (run.size() >= 2) {
					Mesh piece;
					build_sweep(run, vr, piece, p_custom, p_place, p_accent, nullptr);
					ops::append(out, piece, Transform3D(), 0);
				}
				run.clear();
			};
			for (int k = 0; k < ns; k++) {
				const int s = (start + k) % ns;
				if (gap[s]) {
					flush();
					continue;
				}
				if (run.empty()) {
					run.push_back(p_path[s]);
				}
				run.push_back(p_path[(s + 1) % np]);
			}
			flush();
			return;
		}
	}
	const int kind = (int)v[0];
	const float w = v[1], h = v[2];
	const int sides = (int)v[3];
	const bool closed = v[4] > 0.5f;
	const bool caps = v[5] > 0.5f;
	const bool smooth = v[6] > 0.5f;
	const bool centered = (int)v[7] == 1;
	const float corner_r = v[8];
	const int corner_segs = (int)v[9];
	const bool flip = v[10] > 0.5f;

	std::vector<Vector3> path;
	for (const Vector3 &p : p_path) {
		if (path.empty() || path.back().distance_to(p) > 1e-5f) {
			path.push_back(p);
		}
	}
	if (closed && path.size() > 2 && path.front().distance_to(path.back()) < 1e-5f) {
		path.pop_back();
	}
	if (path.size() < 2) {
		return;
	}
	const bool loop = closed && path.size() >= 3;
	const std::vector<Vector3> sharp_path = path; // (its corners: the accents')
	path = round_corners(path, loop, corner_r, corner_segs);
	const int n = (int)path.size();
	const int segs = loop ? n : n - 1;
	// Drop (Anchor Bottom): the profile as tall as the height and the drop together, the drop of it below the path
	const float drop = !centered ? v[12] : 0.0f;
	std::vector<Vector2> prof = sweep_profile(kind, w, h + drop, sides, centered, p_custom);
	for (Vector2 &q : prof) {
		q.y -= drop;
		if (v[11] > 0.5f) {
			q.x += w * 0.5f; // (Outside: all of it on the right of the path)
		}
	}
	const int m = (int)prof.size();

	// ---- along the path (its length): the corners as drawn, halfway between them, every so far
	std::vector<float> acc(segs + 1, 0.0f);
	for (int s = 0; s < segs; s++) {
		acc[s + 1] = acc[s] + path[s].distance_to(path[(s + 1) % n]);
	}
	const float total = acc[segs];
	// a point along the rounded path (its length; a loop goes round)
	auto point_at = [&](float s, int *r_seg = nullptr, float *r_t = nullptr) {
		if (loop && total > 1e-6f) {
			s = std::fmod(s, total);
			if (s < 0.0f) {
				s += total;
			}
		}
		int sg = 0;
		while (sg + 1 < segs && acc[sg + 1] < s) {
			sg++;
		}
		const float t = acc[sg + 1] > acc[sg] ? std::max(0.0f, std::min(1.0f, (s - acc[sg]) / (acc[sg + 1] - acc[sg]))) : 0.0f;
		if (r_seg) {
			*r_seg = sg;
		}
		if (r_t) {
			*r_t = t;
		}
		return path[sg].lerp(path[(sg + 1) % n], t);
	};
	// ---- the corners of the path as drawn: turned at least the smallest angle, the points closer than the distance
	// one corner (in their middle), all / outer / inner; where they are along the rounded path, how much they turn
	struct Corner {
		float s, angle;
	};
	std::vector<Corner> corner_list;
	{
		const std::vector<Vector3> &sp = sharp_path;
		const int sn = (int)sp.size();
		const int ssegs = loop ? sn : sn - 1;
		std::vector<float> sacc(ssegs + 1, 0.0f);
		for (int s = 0; s < ssegs; s++) {
			sacc[s + 1] = sacc[s] + sp[s].distance_to(sp[(s + 1) % sn]);
		}
		const float stotal = sacc[ssegs];
		auto sdir = [&](int s) { return (sp[(s + 1) % sn] - sp[s]).normalized(); };
		std::vector<int> cand;
		for (int i = loop ? 0 : 1; i < (loop ? sn : sn - 1); i++) {
			if (sdir((i + ssegs - 1) % ssegs).dot(sdir(i % ssegs)) < 0.99996f) { // (turned more than half a degree)
				cand.push_back(i);
			}
		}
		const float min_dist = v[25];
		std::vector<std::vector<int>> groups;
		for (int c : cand) {
			if (!groups.empty() && min_dist > 0.0f && sacc[c] - sacc[groups.back().back()] < min_dist) {
				groups.back().push_back(c);
			} else {
				groups.push_back({ c });
			}
		}
		if (loop && groups.size() >= 2 && min_dist > 0.0f && sacc[groups.front().front()] + stotal - sacc[groups.back().back()] < min_dist) {
			std::vector<int> g = groups.back(); // (round the loop's start: one corner)
			g.insert(g.end(), groups.front().begin(), groups.front().end());
			groups.front() = g;
			groups.pop_back();
		}
		const float min_cos = std::cos(v[24] * PI_F / 180.0f);
		const int cside = (int)v[26];
		for (const std::vector<int> &g : groups) {
			const Vector3 din = sdir((g.front() + ssegs - 1) % ssegs), dout = sdir(g.back() % ssegs);
			const float cs = std::max(-1.0f, std::min(1.0f, din.dot(dout)));
			if (cs > min_cos + 1e-6f && v[24] > 1e-4f) {
				continue; // (too slight a turn)
			}
			// inner: turned toward the profile's side (dir x up: Outside's side)
			const bool inner = dout.dot(din.cross(Vector3(0, 1, 0))) > 1e-4f;
			if ((cside == 1 && inner) || (cside == 2 && !inner)) {
				continue;
			}
			float a = sacc[g.front()], b = sacc[g.back()];
			if (b < a) {
				b += stotal;
			}
			float mid = (a + b) * 0.5f;
			if (loop && mid >= stotal) {
				mid -= stotal;
			}
			int sg = 0;
			while (sg + 1 < ssegs && sacc[sg + 1] < mid) {
				sg++;
			}
			const float t = sacc[sg + 1] > sacc[sg] ? (mid - sacc[sg]) / (sacc[sg + 1] - sacc[sg]) : 0.0f;
			const Vector3 p = sp[sg].lerp(sp[(sg + 1) % sn], std::max(0.0f, std::min(1.0f, t)));
			// along the rounded path: its nearest point
			float best_d = 1e30f, best_s = 0.0f;
			for (int s = 0; s < segs; s++) {
				const Vector3 A = path[s], B = path[(s + 1) % n];
				const Vector3 AB = B - A;
				const float L2 = AB.length_squared();
				const float u = L2 > 1e-12f ? std::max(0.0f, std::min(1.0f, (p - A).dot(AB) / L2)) : 0.0f;
				const float d = p.distance_squared_to(A + AB * u);
				if (d < best_d) {
					best_d = d;
					best_s = acc[s] + (acc[s + 1] - acc[s]) * u;
				}
			}
			corner_list.push_back({ best_s, std::acos(cs) });
		}
		std::sort(corner_list.begin(), corner_list.end(), [](const Corner &x, const Corner &y) { return x.s < y.s; });
	}
	std::vector<float> corners;
	for (const Corner &c : corner_list) {
		corners.push_back(c.s);
	}
	// p_mode: 1 every p_step from p_off, 2 corners, 3 middles, 4 corners and middles, 5 corners and every p_step;
	// p_fit: the step made a little shorter / longer so they come out even between the ends
	auto places = [&](int p_mode, float p_step, float p_off, bool p_fit) {
		std::vector<float> centers;
		const bool at_corners = p_mode == 2 || p_mode == 4 || p_mode == 5;
		const bool at_middles = p_mode == 3 || p_mode == 4;
		const bool every = p_mode == 1 || p_mode == 5;
		if (at_corners) {
			centers.insert(centers.end(), corners.begin(), corners.end());
		}
		if (at_middles) {
			std::vector<float> stops = corners;
			if (!loop) {
				stops.insert(stops.begin(), 0.0f);
				stops.push_back(total);
			}
			const int ns = (int)stops.size();
			for (int i = 0; i + (loop ? 0 : 1) < ns; i++) {
				const float a = stops[i], b = i + 1 < ns ? stops[i + 1] : stops[0] + total;
				centers.push_back(std::fmod((a + b) * 0.5f, total));
			}
			if (loop && ns == 0) {
				centers.push_back(0.0f);
			}
		}
		if (every && p_step > 1e-4f && total > 1e-5f) {
			float step = p_step, s0 = std::fmod(p_off, p_step);
			if (s0 < 0.0f) {
				s0 += p_step;
			}
			if (p_fit) {
				const float len = loop ? total : total - s0;
				const int k = std::max(1, (int)std::lround(len / p_step));
				step = len / (float)k;
			}
			for (float s = s0; s <= total + 1e-4f && centers.size() < 4096; s += step) {
				if (!(loop && s >= total - 1e-4f)) {
					centers.push_back(std::min(s, total));
				}
			}
		}
		return centers;
	};
	// ---- accents: stretches of the bigger profile (Same), pieces of another profile, or copies of a mesh
	const int amode = (int)v[13];
	const int ashape = (int)v[27];
	const float a_step = v[14], a_len = v[15], a_w = v[16], a_h = v[17], a_off = v[18];
	std::vector<std::pair<float, float>> spans; // (merged, in order)
	Mesh accent_out; // (the pieces / copies: added at the end)
	const bool a_mesh = ashape == 8;
	if (amode != 0 && total > 1e-5f && !a_mesh && (ashape != 0 || std::fabs(a_w - 1.0f) > 1e-3f || std::fabs(a_h - 1.0f) > 1e-3f)) {
		const std::vector<float> centers = places(amode, a_step, a_off, false);
		const float hl = a_len * 0.5f;
		// a corner within an accent: the accent long enough to hold its mitered corner (an inner corner pulls the
		// profile's far side back along the path by its width times tan(half the turn))
		const float xmax = (v[11] > 0.5f ? 1.0f : 0.5f) * w * a_w;
		std::vector<std::pair<float, float>> raw;
		for (float c : centers) {
			float a = c - hl, b = c + hl;
			for (const Corner &k : corner_list) {
				for (float ks : { k.s, k.s - total, k.s + total }) {
					if ((!loop && ks != k.s) || ks < a - 1e-4f || ks > b + 1e-4f) {
						continue;
					}
					const float need = xmax * std::tan(std::min(k.angle, 160.0f * PI_F / 180.0f) * 0.5f) + 1e-3f;
					a = std::min(a, ks - need);
					b = std::max(b, ks + need);
				}
			}
			raw.push_back({ a, b });
		}
		for (const auto &ab : raw) {
			const float a = ab.first, b = ab.second;
			if (!loop) {
				spans.push_back({ std::max(0.0f, a), std::min(total, b) });
			} else if (b - a >= total) {
				spans.push_back({ 0.0f, total });
			} else if (a < 0.0f) {
				spans.push_back({ a + total, total });
				spans.push_back({ 0.0f, b });
			} else if (b > total) {
				spans.push_back({ a, total });
				spans.push_back({ 0.0f, b - total });
			} else {
				spans.push_back({ a, b });
			}
		}
		std::sort(spans.begin(), spans.end());
		std::vector<std::pair<float, float>> merged;
		for (const auto &sp : spans) {
			if (!merged.empty() && sp.first <= merged.back().second + 1e-5f) {
				merged.back().second = std::max(merged.back().second, sp.second);
			} else {
				merged.push_back(sp);
			}
		}
		spans = merged;
		if (ashape != 0) {
			// ---- another profile: a capped piece of its own along each accent (the sweep runs on under it)
			std::vector<std::pair<float, float>> pieces = spans;
			if (loop && pieces.size() >= 2 && pieces.front().first <= 1e-5f && pieces.back().second >= total - 1e-5f) {
				pieces.front().first = pieces.back().first - total; // (across the loop's start: one piece)
				pieces.pop_back();
			}
			std::vector<float> pv = v;
			pv[0] = (float)(ashape - 1);
			pv[1] = w * a_w;
			pv[2] = h * a_h;
			pv[12] = drop * a_h;
			pv[5] = 1.0f;
			pv[8] = 0.0f; // (the path rounded already)
			pv[13] = 0.0f;
			for (const auto &pc : pieces) {
				const bool full = loop && pc.second - pc.first >= total - 1e-4f;
				std::vector<Vector3> sub;
				if (full) {
					sub = path;
				} else {
					sub.push_back(point_at(pc.first));
					for (int rep = 0; rep < (loop ? 3 : 1); rep++) {
						const float base = loop ? (rep - 1) * total : 0.0f;
						for (int i = 0; i < n; i++) {
							const float s = acc[i] + base;
							if (s > pc.first + 1e-5f && s < pc.second - 1e-5f) {
								sub.push_back(path[i]);
							}
						}
					}
					sub.push_back(point_at(pc.second));
				}
				pv[4] = full ? 1.0f : 0.0f;
				Mesh piece;
				build_sweep(sub, pv, piece, p_custom, nullptr, nullptr, nullptr);
				ops::append(accent_out, piece, Transform3D(), 0);
			}
			spans.clear(); // (the sweep itself stays plain)
		}
	}
	auto inside = [&](float s) {
		for (const auto &sp : spans) {
			if (s >= sp.first - 1e-5f && s <= sp.second + 1e-5f) {
				return true;
			}
		}
		return false;
	};
	std::vector<Vector2> prof_acc = prof;
	for (Vector2 &q : prof_acc) {
		q = Vector2(q.x * a_w, q.y * a_h); // (from the path: Outside stays against the wall, Bottom on the floor)
	}
	// where an accent starts or ends between the path's points: a step there (two rings at one place)
	std::vector<std::vector<float>> cuts(segs);
	for (const auto &sp : spans) {
		for (float b : { sp.first, sp.second }) {
			if (loop && (b <= 1e-5f || b >= total - 1e-5f)) {
				continue; // (a span cut at the loop's start: not an end)
			}
			for (int s = 0; s < segs; s++) {
				if (b > acc[s] + 1e-4f && b < acc[s + 1] - 1e-4f) {
					cuts[s].push_back(b);
				}
			}
		}
	}
	for (std::vector<float> &c : cuts) {
		std::sort(c.begin(), c.end());
	}

	// a frame per segment: side (across), up (the profile's y), carried along (parallel transport)
	std::vector<Vector3> dir(segs), side(segs), up(segs);
	Vector3 ref_up(0, 1, 0);
	for (int s = 0; s < segs; s++) {
		dir[s] = (path[(s + 1) % n] - path[s]).normalized();
		Vector3 sd = dir[s].cross(ref_up);
		if (sd.length() < 1e-4f) {
			sd = s > 0 ? side[s - 1] : Vector3(1, 0, 0).cross(dir[s]).normalized().cross(dir[s]);
		}
		side[s] = sd.normalized();
		up[s] = side[s].cross(dir[s]).normalized();
	}
	// a mesh's copy at s along the path: its X along the path, Y up, Z to the side (Outside: away from the wall); on a
	// corner turned halfway between its two sides; its faces keep their material (slot) and UVs
	auto place_mesh = [&](const Mesh &pm, float s, float msc, Mesh &dst) {
		int sg = 0;
		float t = 0.0f;
		const Vector3 p = point_at(s, &sg, &t);
		Vector3 d = dir[sg], sd = side[sg], u = up[sg];
		const bool at_start = t < 1e-4f && (sg > 0 || loop), at_end = t > 1.0f - 1e-4f && (sg + 1 < segs || loop);
		if (at_start || at_end) {
			const int other = at_start ? (sg + segs - 1) % segs : (sg + 1) % segs;
			const Vector3 dm = (dir[sg] + dir[other]).normalized();
			if (dm.length_squared() > 0.5f) {
				d = dm;
				sd = d.cross(Vector3(0, 1, 0)).normalized();
				if (sd.length_squared() < 0.5f) {
					sd = side[sg];
				}
				u = sd.cross(d).normalized();
			}
		}
		const Transform3D xf(Basis(d * msc, u * msc, sd * msc), p);
		const int base = (int)dst.pos.size();
		for (const Vector3 &q : pm.pos) {
			dst.pos.push_back(xf.xform(q));
		}
		for (const Face &F : pm.faces) {
			Face f = F;
			for (int &vi : f.v) {
				vi += base;
			}
			f.role = ROLE_PLACED;
			dst.faces.push_back(f);
		}
	};
	if (amode != 0 && a_mesh && p_accent && !p_accent->faces.empty() && total > 1e-5f) {
		for (float s : places(amode, a_step, a_off, false)) {
			place_mesh(*p_accent, s, v[28], accent_out);
		}
	}
	if (kind == 7) {
		// ---- the mesh set along the path instead of a profile
		if (p_place && !p_place->faces.empty()) {
			for (float s : places((int)v[19] + 1, v[20], v[21], v[23] > 0.5f)) {
				place_mesh(*p_place, s, v[22], out);
			}
		}
		if (flip) {
			for (Face &F : out.faces) {
				F.reverse();
			}
		}
		ops::append(out, accent_out, Transform3D(), 0);
		return;
	}
	// rings: the profile at every path point, mitered at the corners (and two at each accent's step)
	struct Ring {
		std::vector<int> ids;
		bool step = false; // (the faces from the ring before it: the step's)
	};
	std::vector<Ring> seq;
	std::vector<std::vector<int>> rings(n);
	for (int i = 0; i < n; i++) {
		const int s_in = loop ? (i + segs - 1) % segs : std::max(0, i - 1);
		const int s_out = loop ? i % segs : std::min(segs - 1, i);
		const int s_frame = (!loop && i == 0) ? 0 : s_in;
		Vector3 miter = dir[s_in] + dir[s_out];
		const bool corner = (loop || (i > 0 && i < n - 1)) && s_in != s_out;
		if (miter.length() < 1e-4f) {
			miter = dir[s_out];
		}
		miter.normalize();
		const std::vector<Vector2> &pr = inside(acc[i < segs ? i : segs]) ? prof_acc : prof;
		for (int j = 0; j < m; j++) {
			Vector3 o = side[s_frame] * pr[j].x + up[s_frame] * pr[j].y;
			if (corner) {
				// slide along the incoming direction onto the miter plane
				const float dn = miter.dot(dir[s_in]);
				if (std::fabs(dn) > 0.2f) {
					o -= dir[s_in] * (miter.dot(o) / dn);
				}
			}
			rings[i].push_back(out.add_vertex(path[i] + o));
		}
		seq.push_back({ rings[i], false });
		if (i < segs) {
			for (float b : cuts[i]) {
				const float t = (b - acc[i]) / std::max(1e-6f, acc[i + 1] - acc[i]);
				const Vector3 p = path[i].lerp(path[(i + 1) % n], t);
				for (int k = 0; k < 2; k++) {
					const std::vector<Vector2> &pk = inside(b + (k == 0 ? -2e-4f : 2e-4f)) ? prof_acc : prof;
					Ring r;
					r.step = k == 1;
					for (int j = 0; j < m; j++) {
						// (a point the two profiles share, on the path's side / bottom: one vertex)
						const bool same = k == 1 && pk[j].distance_to(prof[j]) < 1e-6f && prof_acc[j].distance_to(prof[j]) < 1e-6f;
						r.ids.push_back(same ? seq.back().ids[j] : out.add_vertex(p + side[i] * pk[j].x + up[i] * pk[j].y));
					}
					seq.push_back(r);
				}
			}
		}
	}
	std::vector<std::vector<int>> slivers;
	auto add = [&](const std::vector<int> &p_ids, int role, int sm) {
		std::vector<int> ids;
		for (int id : p_ids) {
			if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
				ids.push_back(id);
			}
		}
		if (ids.size() < 3) {
			return; // (both corners shared: no step there)
		}
		{
			// a step along a straight side the two profiles share (the wall side of Outside): all its corners on one
			// line, nothing there
			const Vector3 o = out.pos[ids[0]];
			int far_i = 1;
			for (int i = 2; i < (int)ids.size(); i++) {
				if ((out.pos[ids[i]] - o).length_squared() > (out.pos[ids[far_i]] - o).length_squared()) {
					far_i = i;
				}
			}
			const Vector3 d = out.pos[ids[far_i]] - o;
			bool line = d.length_squared() > 1e-12f;
			for (int i = 1; i < (int)ids.size() && line; i++) {
				const Vector3 e = out.pos[ids[i]] - o;
				line = d.cross(e).length() <= 1e-5f * std::max(1e-9f, d.length_squared() + e.length_squared());
			}
			if (line) {
				slivers.push_back(ids);
				return;
			}
		}
		Face f = make_face(ids);
		f.role = role;
		f.smooth = sm;
		out.faces.push_back(f);
	};
	const bool round = kind == 0 || kind == 2;
	const int nr = (int)seq.size();
	for (int r = 0; r < (loop ? nr : nr - 1); r++) {
		const Ring &RB = seq[(r + 1) % nr];
		const std::vector<int> &A = seq[r].ids, &B = RB.ids;
		for (int j = 0; j < m; j++) {
			const int k = (j + 1) % m;
			// the flat bottom of the half round stays sharp
			const bool flat = kind == 2 && j == m - 1;
			if (RB.step) {
				add({ A[j], B[j], B[k], A[k] }, ROLE_SIDE, 0); // (the accent's end)
			} else {
				add({ A[j], B[j], B[k], A[k] }, ROLE_SURFACE, smooth && round && !flat ? 1 : 0);
			}
		}
	}
	if (!loop && caps) {
		add(rings[0], ROLE_SIDE, 0);
		add(std::vector<int>(rings[n - 1].rbegin(), rings[n - 1].rend()), ROLE_SIDE, 0);
	}
	// the middle point of each left-out sliver goes into the face along its long side (no T-junction left)
	for (const std::vector<int> &sv : slivers) {
		// its two ends (the farthest apart), the points between them in order along the line
		int e1 = sv[0], e2 = sv[1];
		for (int i = 0; i < (int)sv.size(); i++) {
			for (int j = i + 1; j < (int)sv.size(); j++) {
				if (out.pos[sv[i]].distance_squared_to(out.pos[sv[j]]) > out.pos[e1].distance_squared_to(out.pos[e2])) {
					e1 = sv[i];
					e2 = sv[j];
				}
			}
		}
		std::vector<int> mids;
		for (int id : sv) {
			if (id != e1 && id != e2) {
				mids.push_back(id);
			}
		}
		bool done = false;
		for (Face &F : out.faces) {
			const int fs = F.size();
			for (int i = 0; i < fs && !done; i++) {
				const int a = F.v[i], b = F.v[(i + 1) % fs];
				if (!((a == e1 && b == e2) || (a == e2 && b == e1))) {
					continue;
				}
				std::vector<int> ms = mids;
				std::sort(ms.begin(), ms.end(), [&](int x, int y) { return out.pos[x].distance_squared_to(out.pos[a]) > out.pos[y].distance_squared_to(out.pos[a]); });
				F.ensure_attrs();
				const Vector2 uva = F.uv[i], uvb = F.uv[(i + 1) % fs];
				const Color ca = F.col[i];
				const float len = std::max(1e-9f, (float)out.pos[a].distance_to(out.pos[b]));
				for (int mid : ms) {
					// (inserted right after a: the farthest from a first, so they end up in order)
					F.insert_corner(i + 1, mid, uva.lerp(uvb, (float)out.pos[a].distance_to(out.pos[mid]) / len), ca);
				}
				done = true;
			}
			if (done) {
				break;
			}
		}
	}
	// outward: a positive volume (caps or a loop make it closed)
	float vol = 0.0f;
	for (const Face &F : out.faces) {
		for (int i = 1; i + 1 < F.size(); i++) {
			vol += out.pos[F.v[0]].dot(out.pos[F.v[i]].cross(out.pos[F.v[i + 1]]));
		}
	}
	if ((vol < 0.0f) != flip) {
		for (Face &F : out.faces) {
			F.reverse();
		}
	}
	ops::append(out, accent_out, Transform3D(), 0);
}

} // namespace shapes

namespace ops {

std::vector<Edge> arch_edges(Mesh &m, const std::vector<Edge> &p_edges, int p_segments, float p_rise) {
	std::vector<Edge> out;
	const int N = std::max(2, p_segments);
	if (p_edges.empty()) {
		return out;
	}
	Topology t;
	t.build(m);
	std::map<uint64_t, char> chosen;
	for (const Edge &e : p_edges) {
		if (t.find_edge(e.a, e.b) >= 0) {
			chosen[ekey(e)] = 1;
		}
	}
	// vaults: quads with two opposite chosen edges
	struct Vault {
		int face;
		int v0, v1, v2, v3; // chosen edges v0-v1 and v3-v2 (in the quad's order v0 v1 v2 v3)
	};
	std::vector<Vault> vaults;
	std::map<uint64_t, int> in_vault;
	for (int f = 0; f < (int)m.faces.size(); f++) {
		const Face &F = m.faces[f];
		if (F.size() != 4) {
			continue;
		}
		for (int s = 0; s < 2; s++) {
			const int a = F.v[s], b = F.v[s + 1], c = F.v[(s + 2) % 4], d = F.v[(s + 3) % 4];
			if (chosen.count(ekey(a, b)) && chosen.count(ekey(c, d)) && !in_vault.count(ekey(a, b)) && !in_vault.count(ekey(c, d))) {
				vaults.push_back({ f, a, b, c, d });
				in_vault[ekey(a, b)] = f;
				in_vault[ekey(c, d)] = f;
				break;
			}
		}
	}
	// the bulge of an edge a -> b: across it, inside the face next to it that is not its vault, upwards
	auto bulge = [&](int a, int b, int skip_face) {
		const Vector3 d = (m.pos[b] - m.pos[a]).normalized();
		const int e = t.find_edge(a, b);
		Vector3 best;
		float best_up = -1e9f;
		for (int f : t.edge_faces[e]) {
			if (f == skip_face) {
				continue;
			}
			const Face &F = m.faces[f];
			const Vector3 n = face_normal(m, F);
			Vector3 in = n.cross(d);
			// toward the face's inside: the face's center on that side
			if (in.dot(face_center(m, F) - m.pos[a]) < 0.0f) {
				in = -in;
			}
			in.normalize();
			if (in.y > best_up) {
				best_up = in.y;
				best = in;
			}
		}
		if (best_up < -1e8f) {
			Vector3 up(0, 1, 0);
			best = (up - d * d.dot(up));
			if (best.length() < 1e-4f) {
				best = Vector3(1, 0, 0) - d * d.x;
			}
			best.normalize();
		}
		return best;
	};
	// the new corners along a -> b on the arc (bulging along `dir`)
	auto bend = [&](int a, int b, const Vector3 &dir) {
		const Vector3 pa = m.pos[a], pb = m.pos[b];
		const float half = pa.distance_to(pb) * 0.5f;
		const Vector3 mid = (pa + pb) * 0.5f, d = (pb - pa).normalized();
		std::vector<int> ids{ a };
		std::vector<std::pair<float, int>> ins;
		for (int k = 1; k < N; k++) {
			const float th = 3.14159265358979f * k / N;
			const int v = m.add_vertex(mid - d * (std::cos(th) * half) + dir * (std::sin(th) * half * p_rise));
			ids.push_back(v);
			ins.push_back({ (float)k / N, v });
		}
		ids.push_back(b);
		insert_on_edge(m, a, b, ins);
		for (size_t i = 0; i + 1 < ids.size(); i++) {
			out.push_back(Edge(ids[i], ids[i + 1]));
		}
		return ids;
	};
	std::vector<std::pair<int, std::vector<std::vector<int>>>> strips; // vault face -> arcs A, B
	for (const Vault &q : vaults) {
		const Vector3 dir = bulge(q.v0, q.v1, q.face);
		const std::vector<int> A = bend(q.v0, q.v1, dir);
		const std::vector<int> B = bend(q.v3, q.v2, dir);
		strips.push_back({ q.face, { A, B } });
	}
	for (const auto &kv : chosen) {
		if (in_vault.count(kv.first)) {
			continue;
		}
		const int a = (int)(kv.first >> 32), b = (int)(kv.first & 0xffffffffu);
		bend(a, b, bulge(a, b, -1));
	}
	// each vault quad becomes a strip of quads under the arcs
	for (const auto &st : strips) {
		const std::vector<int> &A = st.second[0], &B = st.second[1];
		const Face tmpl = m.faces[st.first];
		for (int k = 0; k < N; k++) {
			Face f = make_face({ A[k], A[k + 1], B[k + 1], B[k] }, &tmpl);
			f.manual_uv = false;
			if (k == 0) {
				m.faces[st.first] = f;
			} else {
				m.faces.push_back(f);
			}
		}
		for (int k = 1; k < N; k++) {
			out.push_back(Edge(A[k], B[k]));
		}
	}
	return sel::uniq(out);
}


// ------------------------------------------------------------------ trims

namespace {
// Chains of directed segments (end of one = start of the next) into paths; each segment once.
std::vector<TrimPath> chain_segments(const std::vector<std::pair<Vector3, Vector3>> &segs, float eps) {
	std::vector<TrimPath> out;
	auto key = [&](const Vector3 &p) {
		const int64_t x = (int64_t)std::llround(p.x / eps), y = (int64_t)std::llround(p.y / eps), z = (int64_t)std::llround(p.z / eps);
		return ((uint64_t)(x & 0x1fffff) << 42) | ((uint64_t)(y & 0x1fffff) << 21) | (uint64_t)(z & 0x1fffff);
	};
	std::multimap<uint64_t, int> starts, ends;
	for (int i = 0; i < (int)segs.size(); i++) {
		starts.insert({ key(segs[i].first), i });
		ends.insert({ key(segs[i].second), i });
	}
	std::vector<char> used(segs.size(), 0);
	auto take = [&](std::multimap<uint64_t, int> &mm, const Vector3 &p) {
		auto range = mm.equal_range(key(p));
		for (auto it = range.first; it != range.second; ++it) {
			if (!used[it->second]) {
				return it->second;
			}
		}
		return -1;
	};
	for (int s0 = 0; s0 < (int)segs.size(); s0++) {
		if (used[s0]) {
			continue;
		}
		// back to the start of this chain first (an open line starts at its own end; a loop anywhere)
		int first = s0;
		for (int guard = 0; guard < (int)segs.size(); guard++) {
			const int prev = take(ends, segs[first].first);
			if (prev < 0 || prev == s0) {
				break;
			}
			first = prev;
		}
		out.push_back(TrimPath());
		TrimPath &tp = out.back();
		tp.points.push_back(segs[first].first);
		int cur = first;
		while (cur >= 0 && !used[cur]) {
			used[cur] = 1;
			tp.points.push_back(segs[cur].second);
			cur = take(starts, segs[cur].second);
		}
		if (tp.points.size() > 2 && key(tp.points.front()) == key(tp.points.back())) {
			tp.points.pop_back();
			tp.closed = true;
		}
	}
	return out;
}
} // namespace

std::vector<TrimPath> edge_trim_paths(const Mesh &m, const std::vector<Edge> &edges) {
	Topology t;
	t.build(m);
	// each edge turned so the faces along it are on its right (seen from above): the side (dir x up) toward their
	// outward normals
	std::vector<std::pair<Vector3, Vector3>> segs;
	for (const Edge &e : edges) {
		const int ei = t.find_edge(e.a, e.b);
		Vector3 out_n;
		if (ei >= 0) {
			for (int f : t.edge_faces[ei]) {
				out_n += face_normal(m, m.faces[f]);
			}
		}
		out_n.y = 0.0f;
		Vector3 a = m.pos[e.a], b = m.pos[e.b];
		const Vector3 side = (b - a).cross(Vector3(0, 1, 0));
		if (side.dot(out_n) < 0.0f) {
			std::swap(a, b);
		}
		segs.push_back({ a, b });
	}
	float eps = 1e-4f;
	for (const auto &sg : segs) {
		eps = std::max(eps, sg.first.distance_to(sg.second) * 1e-4f);
	}
	std::vector<TrimPath> paths = chain_segments(segs, eps);
	// (a chain of edges turned both ways can't join into one line: each piece is a line then)
	return paths;
}

std::vector<TrimPath> section_trim_paths(const Mesh &m, float p_y) {
	std::vector<std::pair<Vector3, Vector3>> segs;
	std::vector<Vector3> pts;
	std::vector<int> tris;
	for (const Face &F : m.faces) {
		if (F.size() < 3) {
			continue;
		}
		face_points(m, F, pts);
		const Vector3 n = face_normal(m, F);
		if (std::fabs(n.y) > 0.999f) {
			continue; // (a floor / ceiling: no line across it)
		}
		tris.clear();
		triangulate(pts, n, tris);
		for (size_t k = 0; k + 2 < tris.size(); k += 3) {
			const Vector3 v[3] = { pts[tris[k]], pts[tris[k + 1]], pts[tris[k + 2]] };
			std::vector<Vector3> cut;
			for (int i = 0; i < 3; i++) {
				const Vector3 &p = v[i], &q = v[(i + 1) % 3];
				const float dp = p.y - p_y, dq = q.y - p_y;
				if ((dp < 0.0f && dq >= 0.0f) || (dp >= 0.0f && dq < 0.0f)) {
					cut.push_back(p.lerp(q, dp / (dp - dq)));
				}
			}
			if (cut.size() != 2 || cut[0].distance_to(cut[1]) < 1e-6f) {
				continue;
			}
			Vector3 a = cut[0], b = cut[1];
			if ((b - a).cross(Vector3(0, 1, 0)).dot(Vector3(n.x, 0, n.z)) < 0.0f) {
				std::swap(a, b); // (the wall on the right: its outside)
			}
			segs.push_back({ a, b });
		}
	}
	godot::AABB box;
	for (size_t i = 0; i < m.pos.size(); i++) {
		box = i ? box.expand(m.pos[i]) : godot::AABB(m.pos[i], Vector3());
	}
	std::vector<TrimPath> paths = chain_segments(segs, std::max(1e-5f, (float)box.size.length() * 1e-5f));
	// straight runs joined into one segment each (the triangles' cuts along a wall)
	for (TrimPath &tp : paths) {
		std::vector<Vector3> clean;
		const int n = (int)tp.points.size();
		for (int i = 0; i < n; i++) {
			const bool ends = !tp.closed && (i == 0 || i == n - 1);
			const Vector3 &pr = tp.points[(i + n - 1) % n], &cu = tp.points[i], &nx = tp.points[(i + 1) % n];
			if (!ends && (cu - pr).normalized().dot((nx - cu).normalized()) > 0.9999f) {
				continue;
			}
			clean.push_back(cu);
		}
		tp.points = clean;
	}
	return paths;
}

} // namespace ops

} // namespace hewn
