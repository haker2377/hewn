// Hewn — a profile swept along a path (rails, curbs, pipes, roads, cornices) and arches bent from
// edges (a vaulted passage out of a cut door).
#include "hw_core.h"

#include <algorithm>
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
		// custom: its points scaled to the width / height (a closed outline, counter-clockwise)
		if (custom && custom->size() >= 3) {
			Vector2 mn = (*custom)[0], mx = mn;
			for (const Vector2 &q : *custom) {
				mn = mn.min(q);
				mx = mx.max(q);
			}
			const Vector2 sz = (mx - mn).max(Vector2(1e-6f, 1e-6f));
			for (const Vector2 &q : *custom) {
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

void build_sweep(const std::vector<Vector3> &p_path, const std::vector<float> &values, Mesh &out, const std::vector<Vector2> *p_custom) {
	out.clear();
	const std::vector<Param> &ps = params(SWEEP);
	std::vector<float> v(ps.size());
	for (size_t i = 0; i < ps.size(); i++) {
		float x = i < values.size() ? values[i] : ps[i].def;
		x = std::max(ps[i].min, std::min(ps[i].max, x));
		v[i] = ps[i].is_int ? std::round(x) : x;
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
	path = round_corners(path, loop, corner_r, corner_segs);
	const int n = (int)path.size();
	const int segs = loop ? n : n - 1;
	std::vector<Vector2> prof = sweep_profile(kind, w, h, sides, centered, p_custom);
	if (v.size() > 11 && v[11] > 0.5f) {
		for (Vector2 &q : prof) {
			q.x += w * 0.5f; // (Outside: all of it on the right of the path)
		}
	}
	const int m = (int)prof.size();

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
	// rings: the profile at every path point, mitered at the corners
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
		for (int j = 0; j < m; j++) {
			Vector3 o = side[s_frame] * prof[j].x + up[s_frame] * prof[j].y;
			if (corner) {
				// slide along the incoming direction onto the miter plane
				const float dn = miter.dot(dir[s_in]);
				if (std::fabs(dn) > 0.2f) {
					o -= dir[s_in] * (miter.dot(o) / dn);
				}
			}
			rings[i].push_back(out.add_vertex(path[i] + o));
		}
	}
	auto add = [&](const std::vector<int> &ids, int role, int sm) {
		Face f = make_face(ids);
		f.role = role;
		f.smooth = sm;
		out.faces.push_back(f);
	};
	const bool round = kind == 0 || kind == 2;
	for (int s = 0; s < segs; s++) {
		const std::vector<int> &A = rings[s], &B = rings[(s + 1) % n];
		for (int j = 0; j < m; j++) {
			const int k = (j + 1) % m;
			// the flat bottom of the half round stays sharp
			const bool flat = kind == 2 && j == m - 1;
			add({ A[j], B[j], B[k], A[k] }, ROLE_SURFACE, smooth && round && !flat ? 1 : 0);
		}
	}
	if (!loop && caps) {
		add(rings[0], ROLE_SIDE, 0);
		add(std::vector<int>(rings[n - 1].rbegin(), rings[n - 1].rend()), ROLE_SIDE, 0);
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
