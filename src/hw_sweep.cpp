// Hewn — a profile swept along a path (rails, curbs, pipes, roads, cornices) and arches bent from
// edges (a vaulted passage out of a cut door).
#include "hw_core.h"

#include <algorithm>
#include <unordered_map>
#include <array>
#include <cmath>
#include <map>
#include <string>

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

// (a frame's sweep over a curved surface: each segment's own up - the surface's normal there, always across the path -
// set by build_frame / frame_sweep_values around their build_sweep)
thread_local const std::vector<Vector3> *g_seg_up = nullptr;

void build_sweep(const std::vector<Vector3> &p_path, const std::vector<float> &values, Mesh &out, const std::vector<Vector2> *p_custom, const Mesh *p_place,
		const Mesh *p_accent, const std::vector<int> *p_gaps, const std::vector<Vector2> *p_accent_custom, ops::ProfileFrame *r_frame) {
	if (r_frame) {
		r_frame->made = false;
	}
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
					ops::ProfileFrame fr;
					build_sweep(run, vr, piece, p_custom, p_place, p_accent, nullptr, p_accent_custom, r_frame && !r_frame->made ? &fr : nullptr);
					if (fr.made && r_frame && !r_frame->made) {
						*r_frame = fr; // (the first piece's)
					}
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
	const int align = (int)std::round(v[11]); // (0 center, 1 outside: on the right of the way, 2 inside: on its left)
	if (loop && align != 0) {
		// (a closed one, Outside: out of the loop whichever way it was drawn; the profile goes on the right of the way)
		float area = 0.0f;
		for (size_t i = 0; i < path.size(); i++) {
			const Vector3 &a = path[i], &b = path[(i + 1) % path.size()];
			area += a.x * b.z - b.x * a.z;
		}
		if (area > 0.0f) {
			std::reverse(path.begin(), path.end());
		}
	}
	const std::vector<Vector3> sharp_path = path; // (its corners: the accents')
	path = round_corners(path, loop, corner_r, corner_segs);
	const int n = (int)path.size();
	const int segs = loop ? n : n - 1;
	// Drop (Anchor Bottom): the profile as tall as the height and the drop together, the drop of it below the path
	const float drop = !centered ? v[12] : 0.0f;
	std::vector<Vector2> prof = sweep_profile(kind, w, h + drop, sides, centered, p_custom);
	for (Vector2 &q : prof) {
		q.y -= drop;
		if (align == 1) {
			q.x += w * 0.5f; // (Outside: all of it on the right of the path)
		} else if (align == 2) {
			q.x -= w * 0.5f; // (Inside: all of it on the left)
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
	const float a_step = v[14], a_len = v[15], a_w = v[16], a_h = v[17], a_off = v[18], a_drop = v[29];
	// (an open path: an accent at each of its ends too, inside it, if asked)
	auto accent_places = [&]() {
		std::vector<float> c = places(amode, a_step, a_off, false);
		if (!loop && v.size() > 31 && v[31] > 0.5f && total > 1e-5f) {
			const float hl = std::min(a_len * 0.5f, total * 0.5f);
			for (float e : { hl, total - hl }) {
				bool near = false;
				for (float x : c) {
					near = near || std::fabs(x - e) < a_len * 0.5f;
				}
				if (!near) {
					c.push_back(e);
				}
			}
			std::sort(c.begin(), c.end());
		}
		return c;
	};
	std::vector<std::pair<float, float>> spans; // (merged, in order)
	Mesh accent_out; // (the pieces / copies: added at the end)
	const bool a_mesh = ashape == 8;
	if (amode != 0 && total > 1e-5f && !a_mesh && (ashape != 0 || std::fabs(a_w - 1.0f) > 1e-3f || std::fabs(a_h - 1.0f) > 1e-3f || a_drop > 1e-5f) &&
			(ashape != 0 || kind != 8)) {
		const std::vector<float> centers = accent_places();
		const float hl = a_len * 0.5f;
		// a corner within an accent: the accent long enough to hold its mitered corner (an inner corner pulls the
		// profile's far side back along the path by its width times tan(half the turn))
		const float xmax = (align != 0 ? 1.0f : 0.5f) * w * a_w;
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
			// (its own height above the path, its drop below it: the sweep's, the accent's taller / more)
			std::vector<float> pv = v;
			pv[0] = (float)(ashape - 1);
			pv[1] = w * a_w;
			pv[2] = (centered ? h * 0.5f : h) * a_h;
			pv[7] = 0.0f;
			pv[12] = (centered ? h * 0.5f : drop) + a_drop;
			pv[29] = 0.0f;
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
				build_sweep(sub, pv, piece, ashape == 7 && p_accent_custom && !p_accent_custom->empty() ? p_accent_custom : p_custom, nullptr, nullptr, nullptr);
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
	// the accent's profile: as wide times its width, the part above the path times its height, below it down by its drop
	// more (from the path: Outside stays against the wall, Bottom on the floor)
	std::vector<Vector2> prof_acc = prof;
	{
		float ymin = 0.0f, ymax = 0.0f;
		for (const Vector2 &q : prof) {
			ymin = std::min(ymin, q.y);
			ymax = std::max(ymax, q.y);
		}
		for (Vector2 &q : prof_acc) {
			float y = q.y;
			if (ymin < -1e-6f) {
				y = y >= 0.0f ? y * a_h : y * (-ymin + a_drop) / -ymin;
			} else if (ymax > 1e-6f) {
				y = -a_drop + y * (ymax * a_h + a_drop) / ymax; // (all of it above: stretched down to the drop)
			}
			q = Vector2(q.x * a_w, y);
		}
	}
	if (v[30] > 0.5f) {
		// Downward: the profile upside down (its height below the path, the drop above it)
		for (std::vector<Vector2> *pp : { &prof, &prof_acc }) {
			for (Vector2 &q : *pp) {
				q.y = -q.y;
			}
			std::reverse(pp->begin(), pp->end());
		}
	}
	const float off_x = v.size() > 33 ? v[32] : 0.0f, off_y = v.size() > 33 ? v[33] : 0.0f;
	if (off_x != 0.0f || off_y != 0.0f) {
		for (std::vector<Vector2> *pp : { &prof, &prof_acc }) {
			for (Vector2 &q : *pp) {
				q += Vector2(off_x, off_y);
			}
		}
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

	// a frame per segment: side (across), up (the profile's y), carried along (parallel transport). Up is the world's;
	// for a path standing in a wall (a door frame: upright parts) the wall's normal: the profile out of the wall all
	// round, not twisted where the path goes up (the normal from the path's turns: out of the side it turns to the right)
	std::vector<Vector3> dir(segs), side(segs), up(segs);
	Vector3 ref_up(0, 1, 0);
	{
		bool upright = false;
		Vector3 nrm;
		for (int s = 0; s < segs; s++) {
			const Vector3 d0 = (path[(s + 1) % n] - path[s]).normalized();
			upright = upright || std::fabs(d0.y) > 0.7f;
			if (s + 1 < segs || loop) {
				const Vector3 d1 = (path[(s + 2) % n] - path[(s + 1) % n]).normalized();
				nrm += d0.cross(d1);
			}
		}
		if (upright && nrm.length() > 1e-3f && std::fabs(nrm.normalized().y) < 0.7f) {
			ref_up = -nrm.normalized();
		}
	}
	if (v.size() > 36 && Vector3(v[34], v[35], v[36]).length() > 0.5f) {
		ref_up = Vector3(v[34], v[35], v[36]).normalized(); // (set: a frame on a wall, its normal; the profile out of it)
	}
	const bool seg_up = g_seg_up && (int)g_seg_up->size() == segs;
	for (int s = 0; s < segs; s++) {
		dir[s] = (path[(s + 1) % n] - path[s]).normalized();
		Vector3 sd = dir[s].cross(seg_up ? (*g_seg_up)[s] : ref_up);
		if (sd.length() < 1e-4f) {
			sd = s > 0 ? side[s - 1] : Vector3(1, 0, 0).cross(dir[s]).normalized().cross(dir[s]);
		}
		side[s] = sd.normalized();
		up[s] = side[s].cross(dir[s]).normalized();
	}
	if (r_frame && kind == 6 && p_custom && p_custom->size() >= 3 && segs > 0) {
		// the Custom curve at the middle of the longest segment the sweep's own profile is on (no miter, no accent):
		// a point q at o + ax q.x + ay q.y, as sweep_profile() and the steps after it place it
		Vector2 mn = (*p_custom)[0], mx = mn;
		for (const Vector2 &q : *p_custom) {
			mn = mn.min(q);
			mx = mx.max(q);
		}
		const Vector2 sz = (mx - mn).max(Vector2(1e-6f, 1e-6f));
		const float H = h + drop, sgn = v[30] > 0.5f ? -1.0f : 1.0f;
		const float cx = -(mn.x / sz.x + 0.5f) * w + (align == 1 ? w * 0.5f : align == 2 ? -w * 0.5f : 0.0f) + off_x;
		const float cy = sgn * (-mn.y / sz.y * H - (centered ? H * 0.5f : 0.0f) - drop) + off_y;
		int best = 0;
		float best_len = -1.0f;
		bool best_plain = false;
		for (int s = 0; s < segs; s++) {
			const float len = acc[s + 1] - acc[s];
			const bool plain = !inside((acc[s] + acc[s + 1]) * 0.5f);
			if ((plain && !best_plain) || (plain == best_plain && len > best_len)) {
				best = s;
				best_len = len;
				best_plain = plain;
			}
		}
		const Vector3 mid = path[best].lerp(path[(best + 1) % n], 0.5f);
		r_frame->made = true;
		r_frame->o = mid + side[best] * cx + up[best] * cy;
		r_frame->ax = side[best] * (w / sz.x);
		r_frame->ay = up[best] * (sgn * H / sz.y);
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
		for (float s : accent_places()) {
			place_mesh(*p_accent, s, v[28], accent_out);
		}
	}
	if (kind == 8) {
		// ---- no profile: only the accents (their pieces flipped by themselves)
		ops::append(out, accent_out, Transform3D(), 0);
		return;
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

std::vector<Edge> arch_edges(Mesh &m, const std::vector<Edge> &p_edges, int p_segments, float p_rise, const std::vector<Vector2> *p_curve, ProfileFrame *r_frame) {
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
		const float len = half * 2.0f;
		const bool curved = p_curve && p_curve->size() >= 2;
		const Vector3 cx = d * len, cy = dir * (half * p_rise);
		if (r_frame && !r_frame->made) {
			r_frame->made = true;
			r_frame->o = pa;
			r_frame->ax = cx;
			r_frame->ay = cy;
		}
		std::vector<float> acc;
		if (curved) {
			acc.push_back(0.0f);
			for (size_t i = 1; i < p_curve->size(); i++) {
				acc.push_back(acc.back() + (*p_curve)[i - 1].distance_to((*p_curve)[i]));
			}
		}
		for (int k = 1; k < N; k++) {
			const float th = 3.14159265358979f * k / N;
			Vector3 at = mid - d * (std::cos(th) * half) + dir * (std::sin(th) * half * p_rise);
			if (curved && acc.back() > 1e-9f) {
				// (at equal lengths along the drawn curve)
				const float dd = acc.back() * k / N;
				size_t i = 0;
				while (i + 2 < acc.size() && acc[i + 1] < dd) {
					i++;
				}
				const float l = acc[i + 1] - acc[i];
				const Vector2 q = (*p_curve)[i].lerp((*p_curve)[i + 1], l > 1e-9f ? (dd - acc[i]) / l : 0.0f);
				at = pa + cx * q.x + cy * q.y;
			}
			const int v = m.add_vertex(at);
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
	// The edges chained by their vertices whatever way each one goes (a loop stays one path), then each chain turned
	// so the faces along it are on its right (seen from above), as most of its edges want.
	std::unordered_map<int, std::vector<int>> at;
	for (int i = 0; i < (int)edges.size(); i++) {
		at[edges[i].a].push_back(i);
		at[edges[i].b].push_back(i);
	}
	std::vector<char> used(edges.size(), 0);
	auto walk = [&](int p_edge, int p_from) {
		std::vector<int> chain{ p_from };
		int e = p_edge, v = p_from;
		while (e >= 0 && !used[e]) {
			used[e] = 1;
			v = edges[e].a == v ? edges[e].b : edges[e].a;
			chain.push_back(v);
			int next = -1;
			if (at[v].size() == 2) { // (a fork ends the line)
				for (int x : at[v]) {
					if (!used[x]) {
						next = x;
					}
				}
			}
			e = next;
		}
		return chain;
	};
	std::vector<std::vector<int>> chains;
	for (const auto &kv : at) {
		if (kv.second.size() != 2) {
			for (int e : kv.second) {
				if (!used[e]) {
					chains.push_back(walk(e, kv.first)); // (from the ends and forks)
				}
			}
		}
	}
	for (int i = 0; i < (int)edges.size(); i++) {
		if (!used[i]) {
			chains.push_back(walk(i, edges[i].a)); // (loops)
		}
	}
	std::vector<TrimPath> paths;
	for (std::vector<int> &ch : chains) {
		if (ch.size() < 2) {
			continue;
		}
		float vote = 0.0f;
		for (size_t i = 0; i + 1 < ch.size(); i++) {
			const int ei = t.find_edge(ch[i], ch[i + 1]);
			Vector3 out_n;
			if (ei >= 0) {
				for (int f : t.edge_faces[ei]) {
					out_n += face_normal(m, m.faces[f]);
				}
			}
			out_n.y = 0.0f;
			vote += (m.pos[ch[i + 1]] - m.pos[ch[i]]).cross(Vector3(0, 1, 0)).dot(out_n);
		}
		TrimPath tp;
		tp.closed = ch.size() > 3 && ch.front() == ch.back();
		if (tp.closed) {
			ch.pop_back();
		}
		if (vote < 0.0f) {
			std::reverse(ch.begin(), ch.end());
		}
		for (int v : ch) {
			tp.points.push_back(m.pos[v]);
		}
		paths.push_back(tp);
	}
	return paths;
}

std::vector<TrimPath> wall_trim_paths(const Mesh &m, const std::vector<Edge> &edges) {
	std::vector<TrimPath> paths = edge_trim_paths(m, edges);
	std::unordered_map<uint64_t, char> want;
	for (const Edge &e : edges) {
		want[ekey(e)] = 1;
	}
	// the faces along the edges (their normals: which way is out of the wall)
	Vector3 face_n;
	for (const Face &F : m.faces) {
		for (int i = 0; i < F.size(); i++) {
			if (want.count(ekey(F.v[i], F.v[(i + 1) % F.size()]))) {
				face_n += face_normal(m, F);
			}
		}
	}
	for (TrimPath &p : paths) {
		// (a path standing in a wall: the way round that turns the profile out of the faces along it, not into them)
		Vector3 nrm;
		const size_t k = p.points.size();
		for (size_t i = 0; k >= 3 && i + 2 < k + (p.closed ? 2 : 0); i++) {
			nrm += (p.points[(i + 1) % k] - p.points[i % k]).cross(p.points[(i + 2) % k] - p.points[(i + 1) % k]);
		}
		if (nrm.length() > 1e-6f && std::fabs(nrm.normalized().y) < 0.7f && face_n.dot(-nrm) < 0.0f) {
			std::reverse(p.points.begin(), p.points.end());
		}
	}
	return paths;
}

namespace {
// the faces' normal around a hole's loop (the faces its edges are on)
Vector3 loop_normal(const Mesh &m, const Topology &t, const std::vector<int> &loop) {
	Vector3 n;
	for (size_t i = 0; i < loop.size(); i++) {
		const int e = t.find_edge(loop[i], loop[(i + 1) % loop.size()]);
		if (e < 0) {
			continue;
		}
		for (int f : t.edge_faces[e]) {
			n += face_normal_raw(m, m.faces[f]);
		}
	}
	return n.length() > 1e-12f ? n.normalized() : Vector3();
}
} // namespace

std::vector<TrimPath> hole_frame_paths(const Mesh &m, const std::vector<int> &loop, const Vector3 &p_up, int sides, const Vector3 *p_axis) {
	std::vector<TrimPath> none;
	if (loop.size() < 3 || (sides & 15) == 0) {
		return none;
	}
	Topology t;
	t.build(m);
	Vector3 n = loop_normal(m, t, loop);
	Vector3 axis;
	if (p_axis && p_axis->length() > 1e-6f) {
		// (the hole's plane: across the cut, turned the way the loop's own normal goes)
		axis = p_axis->normalized();
		n = n.dot(axis) < 0.0f ? -axis : axis;
	}
	Vector3 c;
	for (int v : loop) {
		c += m.pos[v] / (float)loop.size();
	}
	// up and right in the hole's plane (a hole in a floor or a ceiling: -Z is its top)
	Vector3 up = p_up - n * n.dot(p_up);
	if (up.length() < 0.3f) {
		up = Vector3(0, 0, -1) - n * n.dot(Vector3(0, 0, -1));
		if (up.length() < 0.3f) {
			up = Vector3(1, 0, 0) - n * n.x;
		}
	}
	up.normalize();
	const Vector3 right = up.cross(n).normalized(); // (seen from the side the faces look to)
	// the loop's corners only: points at one place joined (a cut leaves the faces' own vertices there), points on a
	// straight run left out - a side is one piece, mitered only at the corners
	std::vector<Vector3> pts;
	for (int v : loop) {
		if (pts.empty() || pts.back().distance_to(m.pos[v]) > 1e-3f) {
			pts.push_back(m.pos[v]);
		}
	}
	while (pts.size() > 1 && pts.front().distance_to(pts.back()) <= 1e-3f) {
		pts.pop_back();
	}
	for (bool again = true; again && pts.size() > 3;) {
		again = false;
		for (size_t i = 0; i < pts.size() && pts.size() > 3; i++) {
			const Vector3 &a = pts[(i + pts.size() - 1) % pts.size()], &b = pts[i], &d = pts[(i + 1) % pts.size()];
			const Vector3 ad = (d - a).normalized();
			const Vector3 off = (b - a) - ad * ad.dot(b - a);
			if (off.length() < 1e-3f && ad.dot(b - a) > 0.0f && ad.dot(d - b) > 0.0f) { // (on the line from a to d, between them)
				pts.erase(pts.begin() + i);
				again = true;
				break;
			}
		}
	}
	const int k = (int)pts.size();
	if (k < 3) {
		return none;
	}
	// each piece: on which side of the hole (by `up` / `right` in its plane), in the hole's plane or along the cut (a
	// groove open at the top: its edges over the top - framed lying on the surface, by the side they go on from)
	std::vector<int> side_of(k, 0);
	std::vector<char> along(k, 0);
	for (int i = 0; i < k; i++) {
		const Vector3 d = pts[(i + 1) % k] - pts[i];
		const Vector3 mid = (pts[i] + pts[(i + 1) % k]) * 0.5f - c;
		if (std::fabs(d.dot(up)) <= std::fabs(d.dot(right))) {
			side_of[i] = mid.dot(up) >= 0.0f ? FRAME_TOP : FRAME_BOTTOM; // (going across: the top or the bottom)
		} else {
			side_of[i] = mid.dot(right) >= 0.0f ? FRAME_RIGHT : FRAME_LEFT;
		}
		along[i] = axis != Vector3() && d.length() > 1e-6f && std::fabs(d.normalized().dot(axis)) > 0.5f;
	}
	for (int i = 0; i < k; i++) {
		if (along[i]) {
			side_of[i] = FRAME_TOP; // (a groove's edges over the top: its top)
		}
	}
	std::vector<char> on(k, 0); // (the side from point i to the next is asked for)
	bool all = true;
	for (int i = 0; i < k; i++) {
		on[i] = (sides & side_of[i]) ? 1 : 0;
		all = all && on[i];
	}
	float extent = 0.0f;
	for (const Vector3 &q : pts) {
		extent = std::max(extent, q.distance_to(c));
	}
	// each piece's "out": a wall's normal (each end of a cut through two walls its own), over the surface its normal
	std::vector<Vector3> seg_up(k, n);
	bool mixed = false;
	if (axis != Vector3()) {
		for (int i = 0; i < k; i++) {
			const Vector3 a = pts[i], b = pts[(i + 1) % k];
			if (along[i]) {
				Vector3 sn;
				for (const Face &F : m.faces) {
					const Vector3 fn = face_normal(m, F);
					if (fn.length() < 1e-6f || std::fabs(fn.normalized().dot(axis)) > 0.5f) {
						continue;
					}
					int touch = 0;
					for (int v : F.v) {
						touch += (m.pos[v].distance_squared_to(a) < 1e-6f ? 1 : 0) + (m.pos[v].distance_squared_to(b) < 1e-6f ? 1 : 0);
					}
					if (touch >= 2) {
						sn += fn.normalized(); // (the faces along that piece)
					}
				}
				if (sn.length() > 1e-4f) {
					seg_up[i] = sn.normalized();
				}
			} else {
				const float al = ((a + b) * 0.5f - c).dot(axis);
				if (std::fabs(al) > std::max(1e-4f, extent * 0.05f)) {
					seg_up[i] = al > 0.0f ? axis : -axis;
				}
			}
		}
		for (int i = 0; i < k; i++) {
			mixed = mixed || seg_up[i].distance_to(seg_up[0]) > 1e-3f;
		}
	}
	std::vector<TrimPath> paths;
	if (all) {
		TrimPath tp; // (all round: one closed path)
		tp.points = pts;
		tp.closed = true;
		tp.ups = seg_up;
		paths.push_back(tp);
	} else {
		// the runs of sides asked for, from just after one that is not
		int start = 0;
		for (int i = 0; i < k; i++) {
			if (!on[i]) {
				start = i;
				break;
			}
		}
		TrimPath cur;
		for (int s = 1; s <= k; s++) {
			const int i = (start + s) % k;
			if (on[i]) {
				if (cur.points.empty()) {
					cur.points.push_back(pts[i]);
				}
				cur.points.push_back(pts[(i + 1) % k]);
				cur.ups.push_back(seg_up[i]);
			} else if (!cur.points.empty()) {
				paths.push_back(cur);
				cur = TrimPath();
			}
		}
		if (!cur.points.empty()) {
			paths.push_back(cur);
		}
	}
	for (TrimPath &p : paths) {
		p.up = p.ups.empty() ? n : p.ups[0];
		p.hole = c;
		bool same = true;
		for (const Vector3 &u : p.ups) {
			same = same && u.distance_to(p.up) < 1e-3f;
		}
		if (same) {
			p.ups.clear(); // (one way out all along: as before)
			if (axis != Vector3() && mixed) {
				// (the opening at one end of a cut through two walls: round the hole's middle in that wall)
				Vector3 mid;
				for (const Vector3 &q : p.points) {
					mid += q / (float)p.points.size();
				}
				p.hole = c + axis * (mid - c).dot(axis);
			}
		}
	}
	return paths;
}

std::vector<int> hole_wall_sides(const Mesh &m, const std::vector<std::vector<int>> &loops, const Vector3 &p_toward) {
	std::vector<int> out(loops.size(), 1);
	if (loops.empty() || m.pos.empty()) {
		return out;
	}
	Topology t;
	t.build(m);
	Vector3 lo = m.pos[0], hi = m.pos[0];
	for (const Vector3 &p : m.pos) {
		lo = lo.min(p);
		hi = hi.max(p);
	}
	const Vector3 mid = (lo + hi) * 0.5f;
	std::vector<Vector3> ns(loops.size());
	for (size_t k = 0; k < loops.size(); k++) {
		const std::vector<int> &loop = loops[k];
		const Vector3 n = loop_normal(m, t, loop);
		ns[k] = n;
		Vector3 c;
		int roles[3] = { 0, 0, 0 };
		for (size_t i = 0; i < loop.size(); i++) {
			c += m.pos[loop[i]] / (float)loop.size();
			const int e = t.find_edge(loop[i], loop[(i + 1) % loop.size()]);
			for (int f : e >= 0 ? t.edge_faces[e] : std::vector<int>()) {
				const int r = m.faces[f].role;
				roles[r == ROLE_OUTER ? 1 : r == ROLE_INNER ? 2 : 0]++;
			}
		}
		if (roles[1] != roles[2]) {
			out[k] = roles[1] > roles[2] ? 1 : 2; // (the faces say: a building's outer / inner walls)
			continue;
		}
		// out of the mesh's middle or into it (a wall: across, the floor and ceiling: up and down)
		Vector3 hn(n.x, 0, n.z), away(c.x - mid.x, 0, c.z - mid.z);
		if (hn.length() < 0.3f) {
			hn = Vector3(0, n.y, 0);
			away = Vector3(0, c.y - mid.y, 0);
		}
		out[k] = hn.dot(away) >= 0.0f ? 1 : 2;
	}
	// (all of one side - a wall on its own, both its faces look out of its middle: the side the cut came from is the outside)
	bool same = loops.size() >= 2;
	for (size_t k = 1; k < loops.size(); k++) {
		same = same && out[k] == out[0];
	}
	if (same && p_toward.length() > 1e-6f) {
		size_t best = 0;
		for (size_t k = 1; k < loops.size(); k++) {
			if (ns[k].dot(p_toward) > ns[best].dot(p_toward)) {
				best = k;
			}
		}
		for (size_t k = 0; k < loops.size(); k++) {
			out[k] = k == best ? 1 : 2;
		}
	}
	return out;
}

} // namespace ops

namespace shapes {
std::vector<float> frame_sweep_values(const ops::TrimPath &p, int profile, float width, float depth, const std::vector<Vector2> *custom) {
	const std::vector<Param> &ps = params(SWEEP);
	std::vector<float> v(ps.size());
	const Vector3 up = p.up.length() > 1e-6f ? p.up.normalized() : Vector3();
	for (size_t i = 0; i < ps.size(); i++) {
		const std::string nm = ps[i].name;
		v[i] = nm == "profile" ? (float)profile : nm == "width" ? width : nm == "height" ? depth : nm == "anchor" ? 0.0f : nm == "align" ? 1.0f :
				nm == "closed" ? (p.closed ? 1.0f : 0.0f) : nm == "smooth" ? 0.0f : nm == "up_x" ? up.x : nm == "up_y" ? up.y : nm == "up_z" ? up.z : ps[i].def;
	}
	if (up == Vector3() || p.points.size() < 2) {
		return v;
	}
	// (on the side away from the hole: its points further off the hole's middle than the path's; else the other side)
	auto spread = [&](const std::vector<Vector3> &pts) {
		float d = 0.0f;
		for (const Vector3 &q : pts) {
			const Vector3 r = q - p.hole;
			d += (r - up * up.dot(r)).length();
		}
		return pts.empty() ? 0.0f : d / (float)pts.size();
	};
	Mesh piece;
	g_seg_up = p.ups.empty() ? nullptr : &p.ups;
	build_sweep(p.points, v, piece, custom);
	g_seg_up = nullptr;
	if (spread(piece.pos) < spread(p.points)) {
		for (size_t i = 0; i < ps.size(); i++) {
			if (std::string(ps[i].name) == "align") {
				v[i] = 2.0f; // (Inside)
			}
		}
	}
	return v;
}

const std::vector<Vector2> &frame_cornice() {
	// (a frame's cornice: its wide side on the wall, stepping in as it comes out of it - the sweep's cornice upside
	// down, whose wide side is its top)
	static const std::vector<Vector2> c = { Vector2(0, 1), Vector2(1.0f / 3, 1), Vector2(1.0f / 3, 2.0f / 3), Vector2(2.0f / 3, 2.0f / 3),
		Vector2(2.0f / 3, 1.0f / 3), Vector2(1, 1.0f / 3), Vector2(1, 0), Vector2(0, 0) };
	return c;
}

std::vector<ops::TrimPath> frame_pieces(const std::vector<ops::TrimPath> &paths, float extend) {
	std::vector<ops::TrimPath> out;
	for (const ops::TrimPath &fp : paths) {
		const int segs = (int)fp.ups.size();
		if (segs == 0) {
			out.push_back(fp);
			continue;
		}
		// (a closed one from a place where the way out changes)
		// (a sharp turn of the way out - off a wall onto the top: a new piece; over a curve it turns little by little and
		// stays one piece, each segment its own way out)
		auto sharp = [&](int i) { return fp.ups[i].dot(fp.ups[(i + segs - 1) % segs]) < 0.5f; };
		int first = 0;
		bool any_sharp = false;
		if (fp.closed) {
			for (int i = 0; i < segs; i++) {
				if (sharp(i)) {
					first = i;
					any_sharp = true;
					break;
				}
			}
		}
		const int np = (int)fp.points.size();
		std::vector<ops::TrimPath> mine;
		ops::TrimPath cur;
		for (int s = 0; s < segs; s++) {
			const int i = (first + s) % segs;
			if (!cur.points.empty() && sharp(i)) {
				mine.push_back(cur);
				cur = ops::TrimPath();
			}
			if (cur.points.empty()) {
				cur.points.push_back(fp.points[i % np]);
				cur.up = fp.ups[i];
				cur.hole = fp.hole;
			}
			cur.points.push_back(fp.points[(i + 1) % np]);
			cur.ups.push_back(fp.ups[i]);
		}
		if (!cur.points.empty()) {
			mine.push_back(cur);
		}
		if (fp.closed && !any_sharp && mine.size() == 1) {
			mine[0] = fp; // (no sharp turn: as it was, closed)
		}
		for (ops::TrimPath &q : mine) {
			bool same = true;
			for (const Vector3 &u : q.ups) {
				same = same && u.distance_to(q.up) < 1e-3f;
			}
			if (same) {
				q.ups.clear();
			}
		}
		// where two pieces meet (the way out turns: up a wall, onto the top), each goes on past the corner by the frame's
		// size, into the other: one solid corner, no gap
		const bool ring = fp.closed && mine.size() > 1;
		for (size_t k = 0; k < mine.size(); k++) {
			std::vector<Vector3> &q = mine[k].points;
			if (q.size() < 2) {
				continue;
			}
			if (k > 0 || ring) {
				q.front() += (q[0] - q[1]).normalized() * extend;
			}
			if (k + 1 < mine.size() || ring) {
				q.back() += (q[q.size() - 1] - q[q.size() - 2]).normalized() * extend;
			}
		}
		out.insert(out.end(), mine.begin(), mine.end());
	}
	return out;
}

void build_frame(const std::vector<ops::TrimPath> &p_paths, int profile, float width, float depth, Mesh &out, const std::vector<Vector2> *custom) {
	const std::vector<ops::TrimPath> paths = frame_pieces(p_paths, std::max(width, depth));
	if (profile == 6 && (!custom || custom->size() < 3)) {
		profile = 1; // (Custom without a shape: the rectangle)
		custom = nullptr;
	}
	if (profile == 4 && !custom) {
		profile = 6;
		custom = &frame_cornice();
	}
	out.clear();
	for (const ops::TrimPath &p : paths) {
		if (p.points.size() < 2) {
			continue;
		}
		Mesh piece;
		const std::vector<float> fv = frame_sweep_values(p, profile, width, depth, custom);
		g_seg_up = p.ups.empty() ? nullptr : &p.ups;
		build_sweep(p.points, fv, piece, custom);
		g_seg_up = nullptr;
		ops::append(out, piece, Transform3D(), 0);
	}
}
} // namespace shapes

namespace ops {

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
