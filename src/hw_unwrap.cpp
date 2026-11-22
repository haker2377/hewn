#include "hw_unwrap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/vector2i.hpp>

using godot::Vector2i;

namespace hewn {
namespace unwrap {

bool lscm(const Mesh &m, const std::vector<int> &p_faces, Layout &r_layout) {
	std::unordered_map<int, int> idx;
	std::vector<int> verts;
	for (int f : p_faces) {
		for (int v : m.faces[f].v) {
			if (!idx.count(v)) {
				idx[v] = (int)verts.size();
				verts.push_back(v);
			}
		}
	}
	const int N = (int)verts.size();
	if (N < 3) {
		return false;
	}
	// two residual rows per triangle (Cauchy-Riemann), weighted by the root of its area
	struct Row {
		int c[6];
		float w[6];
	};
	std::vector<Row> rows;
	double area3 = 0.0;
	for (int f : p_faces) {
		const Face &F = m.faces[f];
		for (int i = 1; i + 1 < F.size(); i++) {
			const int tv[3] = { F.v[0], F.v[i], F.v[i + 1] };
			const Vector3 p0 = m.pos[tv[0]], p1 = m.pos[tv[1]], p2 = m.pos[tv[2]];
			const Vector3 nn = (p1 - p0).cross(p2 - p0);
			if (nn.length() < 1e-10f || (p1 - p0).length() < 1e-7f) {
				continue;
			}
			const Vector3 e1 = (p1 - p0).normalized(), e2 = nn.normalized().cross(e1);
			const Vector2 q[3] = { Vector2(), Vector2((p1 - p0).dot(e1), 0.0f), Vector2((p2 - p0).dot(e1), (p2 - p0).dot(e2)) };
			const float A = 0.5f * (q[1].x * q[2].y - q[2].x * q[1].y);
			if (A < 1e-12f) {
				continue;
			}
			area3 += A;
			const float sw = std::sqrt(A);
			Row r1, r2;
			for (int j = 0; j < 3; j++) {
				const Vector2 e = q[(j + 2) % 3] - q[(j + 1) % 3];
				const Vector2 gj = Vector2(-e.y, e.x) / (2.0f * A); // the gradient of the corner's hat function
				const int k = idx[tv[j]];
				r1.c[2 * j] = 2 * k;
				r1.w[2 * j] = gj.x * sw; // du/dx
				r1.c[2 * j + 1] = 2 * k + 1;
				r1.w[2 * j + 1] = -gj.y * sw; // - dv/dy
				r2.c[2 * j] = 2 * k;
				r2.w[2 * j] = gj.y * sw; // du/dy
				r2.c[2 * j + 1] = 2 * k + 1;
				r2.w[2 * j + 1] = gj.x * sw; // + dv/dx
			}
			rows.push_back(r1);
			rows.push_back(r2);
		}
	}
	if (rows.empty()) {
		return false;
	}
	// two vertices far apart pinned (the rest follows, a similarity of the true map)
	auto farthest = [&](int from) {
		int best = from;
		float bd = -1.0f;
		for (int i = 0; i < N; i++) {
			const float d = m.pos[verts[i]].distance_squared_to(m.pos[verts[from]]);
			if (d > bd) {
				bd = d;
				best = i;
			}
		}
		return best;
	};
	const int pa = farthest(0), pb = farthest(pa);
	if (pa == pb) {
		return false;
	}
	std::vector<double> x(2 * N, 0.0);
	std::vector<char> pinned(2 * N, 0);
	const float dab = m.pos[verts[pa]].distance_to(m.pos[verts[pb]]);
	x[2 * pb] = dab;
	for (int k : { pa, pb }) {
		pinned[2 * k] = pinned[2 * k + 1] = 1;
	}
	// a start: the projection onto the average plane, moved onto the pins
	{
		Vector3 avg;
		for (int f : p_faces) {
			avg += face_normal_raw(m, m.faces[f]);
		}
		Vector3 u, v;
		uv_axes(avg.length() > 1e-8f ? avg.normalized() : Vector3(0, 1, 0), u, v);
		auto proj = [&](int i) { return Vector2(m.pos[verts[i]].dot(u), m.pos[verts[i]].dot(v)); };
		const Vector2 a = proj(pa), b = proj(pb);
		const float ang = -(b - a).angle(), sc = (b - a).length() > 1e-6f ? dab / (b - a).length() : 1.0f;
		for (int i = 0; i < N; i++) {
			if (i != pa && i != pb) {
				const Vector2 q = (proj(i) - a).rotated(ang) * sc;
				x[2 * i] = q.x;
				x[2 * i + 1] = q.y;
			}
		}
	}
	// conjugate gradients on the normal equations of the free values
	auto apply = [&](const std::vector<double> &p_in, std::vector<double> &r_out, bool p_free_only) {
		std::fill(r_out.begin(), r_out.end(), 0.0);
		for (const Row &r : rows) {
			double s = 0.0;
			for (int j = 0; j < 6; j++) {
				if (!p_free_only || !pinned[r.c[j]]) {
					s += r.w[j] * p_in[r.c[j]];
				}
			}
			for (int j = 0; j < 6; j++) {
				r_out[r.c[j]] += r.w[j] * s;
			}
		}
		for (int i = 0; i < 2 * N; i++) {
			if (pinned[i]) {
				r_out[i] = 0.0;
			}
		}
	};
	std::vector<double> fixed(2 * N, 0.0), b(2 * N), ax(2 * N), res(2 * N), p(2 * N), ap(2 * N);
	for (int i = 0; i < 2 * N; i++) {
		fixed[i] = pinned[i] ? x[i] : 0.0;
	}
	// b = -A^T A x_pinned (the pinned values' pull on the free ones)
	{
		std::fill(b.begin(), b.end(), 0.0);
		for (const Row &r : rows) {
			double s = 0.0;
			for (int j = 0; j < 6; j++) {
				s += r.w[j] * fixed[r.c[j]];
			}
			for (int j = 0; j < 6; j++) {
				b[r.c[j]] -= r.w[j] * s;
			}
		}
		for (int i = 0; i < 2 * N; i++) {
			if (pinned[i]) {
				b[i] = 0.0;
			}
		}
	}
	apply(x, ax, true);
	double bb = 0.0, rr = 0.0;
	for (int i = 0; i < 2 * N; i++) {
		res[i] = pinned[i] ? 0.0 : b[i] - ax[i];
		p[i] = res[i];
		rr += res[i] * res[i];
		bb += b[i] * b[i];
	}
	const int iters = std::min(20000, 8 * N + 200);
	for (int it = 0; it < iters && rr > 1e-20 * std::max(bb, 1e-30); it++) {
		apply(p, ap, true);
		double pap = 0.0;
		for (int i = 0; i < 2 * N; i++) {
			pap += p[i] * ap[i];
		}
		if (pap <= 1e-300) {
			break;
		}
		const double alpha = rr / pap;
		double rr2 = 0.0;
		for (int i = 0; i < 2 * N; i++) {
			if (!pinned[i]) {
				x[i] += alpha * p[i];
				res[i] -= alpha * ap[i];
				rr2 += res[i] * res[i];
			}
		}
		const double beta = rr2 / rr;
		rr = rr2;
		for (int i = 0; i < 2 * N; i++) {
			p[i] = pinned[i] ? 0.0 : res[i] + beta * p[i];
		}
	}
	// meters: the layout's area as the faces' own
	double area2 = 0.0;
	for (int f : p_faces) {
		const Face &F = m.faces[f];
		for (int i = 1; i + 1 < F.size(); i++) {
			const int a = idx[F.v[0]], b2 = idx[F.v[i]], c = idx[F.v[i + 1]];
			area2 += 0.5 * ((x[2 * b2] - x[2 * a]) * (x[2 * c + 1] - x[2 * a + 1]) - (x[2 * c] - x[2 * a]) * (x[2 * b2 + 1] - x[2 * a + 1]));
		}
	}
	if (!(area2 > 1e-12) || !std::isfinite(area2)) {
		return false; // (folded over itself: not a patch it can lay out)
	}
	const double s = std::sqrt(area3 / area2);
	r_layout.clear();
	for (int f : p_faces) {
		std::vector<Vector2> l;
		for (int v : m.faces[f].v) {
			const int k = idx[v];
			l.push_back(Vector2((float)(x[2 * k] * s), (float)(-x[2 * k + 1] * s))); // (V down)
		}
		r_layout[f] = l;
	}
	return true;
}

std::vector<std::vector<int>> seam_islands(const Mesh &m, const std::vector<int> &faces) {
	std::unordered_map<int, int> idx;
	for (int k = 0; k < (int)faces.size(); k++) {
		idx[faces[k]] = k;
	}
	std::vector<int> parent(faces.size());
	std::iota(parent.begin(), parent.end(), 0);
	std::function<int(int)> find = [&](int x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
	std::unordered_map<uint64_t, int> first;
	for (int k = 0; k < (int)faces.size(); k++) {
		const Face &F = m.faces[faces[k]];
		for (int i = 0; i < F.size(); i++) {
			const uint64_t key = ekey(F.v[i], F.v[(i + 1) % F.size()]);
			auto ea = m.edge_attrs.find(key);
			if (ea != m.edge_attrs.end() && ea->second.seam) {
				continue;
			}
			auto it = first.find(key);
			if (it == first.end()) {
				first[key] = k;
			} else {
				parent[find(k)] = find(it->second);
			}
		}
	}
	std::map<int, std::vector<int>> groups;
	for (int k = 0; k < (int)faces.size(); k++) {
		groups[find(k)].push_back(faces[k]);
	}
	std::vector<std::vector<int>> out;
	for (auto &g : groups) {
		out.push_back(g.second);
	}
	return out;
}

void relax(const Mesh &m, const std::vector<int> &faces, Layout &io, int iterations) {
	Mesh t = m;
	std::vector<int> fs;
	for (int f : faces) {
		auto it = io.find(f);
		if (it != io.end() && it->second.size() == t.faces[f].v.size()) {
			t.faces[f].uv = it->second;
			t.faces[f].manual_uv = true;
			fs.push_back(f);
		}
	}
	reshape_uvs(t, fs, std::max(1, iterations));
	for (int f : fs) {
		io[f] = t.faces[f].uv;
	}
}

void planar(const Mesh &m, const std::vector<int> &faces, const Vector3 &n, Layout &r_out) {
	Vector3 u, v;
	uv_axes(n.length() > 1e-8f ? n.normalized() : Vector3(0, 1, 0), u, v);
	for (int f : faces) {
		std::vector<Vector2> l;
		for (int vi : m.faces[f].v) {
			l.push_back(Vector2(m.pos[vi].dot(u), -m.pos[vi].dot(v)));
		}
		r_out[f] = l;
	}
}

std::vector<std::vector<int>> smart_project(const Mesh &m, const std::vector<int> &faces, float angle, Layout &r_out) {
	const float cos_lim = std::cos(angle * 3.14159265f / 180.0f);
	std::vector<Vector3> nrm(m.faces.size());
	std::vector<float> area(m.faces.size(), 0.0f);
	for (int f : faces) {
		const Vector3 r = face_normal_raw(m, m.faces[f]);
		area[f] = r.length();
		nrm[f] = r.length() > 1e-12f ? r.normalized() : Vector3(0, 1, 0);
	}
	// group normals: the largest face not in one yet starts a group; its normal the group's (area weighted)
	std::vector<int> order = faces;
	std::sort(order.begin(), order.end(), [&](int a, int b) { return area[a] > area[b]; });
	std::vector<Vector3> groups;
	std::unordered_map<int, int> group_of;
	for (int f : order) {
		int best = -1;
		float bd = cos_lim;
		for (int g = 0; g < (int)groups.size(); g++) {
			const float d = groups[g].normalized().dot(nrm[f]);
			if (d >= bd) {
				bd = d;
				best = g;
			}
		}
		if (best < 0) {
			best = (int)groups.size();
			groups.push_back(Vector3());
		}
		groups[best] += nrm[f] * std::max(1e-6f, area[f]);
		group_of[f] = best;
	}
	// each group: projected along its normal, split into the parts joined by sides
	std::vector<std::vector<int>> islands;
	for (int g = 0; g < (int)groups.size(); g++) {
		std::vector<int> fs;
		for (int f : faces) {
			if (group_of[f] == g) {
				fs.push_back(f);
			}
		}
		planar(m, fs, groups[g], r_out);
		Mesh none = m;
		none.edge_attrs.clear();
		for (const std::vector<int> &part : seam_islands(none, fs)) {
			islands.push_back(part);
		}
	}
	return islands;
}

void project_box(const Mesh &m, const std::vector<int> &faces, const Transform3D &frame, Layout &r_out) {
	const Transform3D inv = frame.affine_inverse();
	for (int f : faces) {
		const Vector3 n = frame.basis.inverse().xform(face_normal(m, m.faces[f]));
		const int ax = std::fabs(n.x) >= std::fabs(n.y) && std::fabs(n.x) >= std::fabs(n.z) ? 0 : std::fabs(n.y) >= std::fabs(n.z) ? 1 : 2;
		std::vector<Vector2> l;
		for (int vi : m.faces[f].v) {
			const Vector3 p = inv.xform(m.pos[vi]);
			// (each side seen from outside: unmirrored)
			if (ax == 0) {
				l.push_back(Vector2(n.x > 0 ? -p.z : p.z, -p.y));
			} else if (ax == 1) {
				l.push_back(Vector2(p.x, n.y > 0 ? p.z : -p.z));
			} else {
				l.push_back(Vector2(n.z > 0 ? p.x : -p.x, -p.y));
			}
		}
		r_out[f] = l;
	}
}

namespace {
// a face's angles about Y made one run (no jump across the back): each corner within half a turn of the first
void unwrap_angles(std::vector<float> &a) {
	for (size_t i = 1; i < a.size(); i++) {
		while (a[i] - a[0] > 3.14159265f) {
			a[i] -= 6.2831853f;
		}
		while (a[i] - a[0] < -3.14159265f) {
			a[i] += 6.2831853f;
		}
	}
}
} // namespace

void project_cylinder(const Mesh &m, const std::vector<int> &faces, const Transform3D &frame, Layout &r_out) {
	const Transform3D inv = frame.affine_inverse();
	float rsum = 0.0f;
	int rn = 0;
	for (int f : faces) {
		for (int vi : m.faces[f].v) {
			const Vector3 p = inv.xform(m.pos[vi]);
			rsum += Vector2(p.x, p.z).length();
			rn++;
		}
	}
	const float r = rn ? std::max(1e-4f, rsum / rn) : 1.0f;
	for (int f : faces) {
		std::vector<float> a;
		std::vector<float> y;
		for (int vi : m.faces[f].v) {
			const Vector3 p = inv.xform(m.pos[vi]);
			a.push_back(std::atan2(p.x, p.z));
			y.push_back(p.y);
		}
		unwrap_angles(a);
		std::vector<Vector2> l;
		for (size_t i = 0; i < a.size(); i++) {
			l.push_back(Vector2(a[i] * r, -y[i]));
		}
		r_out[f] = l;
	}
}

void project_sphere(const Mesh &m, const std::vector<int> &faces, const Transform3D &frame, Layout &r_out) {
	const Transform3D inv = frame.affine_inverse();
	float rsum = 0.0f;
	int rn = 0;
	for (int f : faces) {
		for (int vi : m.faces[f].v) {
			rsum += inv.xform(m.pos[vi]).length();
			rn++;
		}
	}
	const float r = rn ? std::max(1e-4f, rsum / rn) : 1.0f;
	for (int f : faces) {
		std::vector<float> a, b;
		for (int vi : m.faces[f].v) {
			const Vector3 p = inv.xform(m.pos[vi]);
			a.push_back(std::atan2(p.x, p.z));
			b.push_back(std::asin(std::clamp(p.length() > 1e-9f ? p.y / p.length() : 0.0f, -1.0f, 1.0f)));
		}
		unwrap_angles(a);
		std::vector<Vector2> l;
		for (size_t i = 0; i < a.size(); i++) {
			l.push_back(Vector2(a[i] * r, -b[i] * r));
		}
		r_out[f] = l;
	}
}

bool quad_grid(const Mesh &m, const std::vector<int> &faces, bool along_v, Layout &r_out) {
	std::unordered_set<int> in;
	for (int f : faces) {
		if (m.faces[f].size() != 4) {
			return false;
		}
		in.insert(f);
	}
	if (in.empty()) {
		return false;
	}
	std::unordered_map<uint64_t, std::vector<int>> by_edge;
	for (int f : faces) {
		const Face &F = m.faces[f];
		for (int i = 0; i < 4; i++) {
			by_edge[ekey(F.v[i], F.v[(i + 1) % 4])].push_back(f);
		}
	}
	// each face's corners at a row and a column (a corner per face: a seam has two places)
	std::unordered_map<int, std::array<Vector2i, 4>> grid;
	const int s = faces[0];
	grid[s] = { Vector2i(0, 0), Vector2i(1, 0), Vector2i(1, 1), Vector2i(0, 1) };
	std::vector<int> queue = { s };
	for (size_t qi = 0; qi < queue.size(); qi++) {
		const int f = queue[qi];
		const Face &A = m.faces[f];
		const std::array<Vector2i, 4> ga = grid[f];
		for (int i = 0; i < 4; i++) {
			const int a = A.v[i], b = A.v[(i + 1) % 4];
			auto ea = m.edge_attrs.find(ekey(a, b));
			if (ea != m.edge_attrs.end() && ea->second.seam) {
				continue;
			}
			for (int g : by_edge[ekey(a, b)]) {
				if (g == f || grid.count(g)) {
					continue;
				}
				const Face &B = m.faces[g];
				int ja = -1, jb = -1;
				for (int j = 0; j < 4; j++) {
					ja = B.v[j] == a ? j : ja;
					jb = B.v[j] == b ? j : jb;
				}
				if (ja < 0 || jb < 0) {
					continue;
				}
				// away from A: across from a and b, the step from A's corners before a / after b
				const Vector2i pa = ga[i], pb = ga[(i + 1) % 4];
				const Vector2i out = pa - ga[(i + 3) % 4];
				std::array<Vector2i, 4> gb;
				gb[ja] = pa;
				gb[jb] = pb;
				const int jc = (jb + 1) % 4 == ja ? (jb + 3) % 4 : (jb + 1) % 4; // (after b in B, away from a)
				const int jd = (ja + 1) % 4 == jb ? (ja + 3) % 4 : (ja + 1) % 4;
				gb[jc] = pb + out;
				gb[jd] = pa + out;
				grid[g] = gb;
				queue.push_back(g);
			}
		}
	}
	if (grid.size() != in.size()) {
		return false; // (not one island)
	}
	// each face a unit square of the grid
	for (const auto &kv : grid) {
		const std::array<Vector2i, 4> &q = kv.second;
		int mnx = q[0].x, mny = q[0].y, mxx = q[0].x, mxy = q[0].y;
		for (const Vector2i &c : q) {
			mnx = std::min(mnx, c.x);
			mny = std::min(mny, c.y);
			mxx = std::max(mxx, c.x);
			mxy = std::max(mxy, c.y);
		}
		if (mxx - mnx != 1 || mxy - mny != 1) {
			return false;
		}
	}
	// the columns' widths, the rows' heights: their sides' lengths on average
	std::map<int, std::pair<float, int>> cw, rh;
	for (const auto &kv : grid) {
		const Face &F = m.faces[kv.first];
		const std::array<Vector2i, 4> &q = kv.second;
		for (int i = 0; i < 4; i++) {
			const Vector2i a = q[i], b = q[(i + 1) % 4];
			const float len = m.pos[F.v[i]].distance_to(m.pos[F.v[(i + 1) % 4]]);
			if (a.y == b.y) {
				auto &e = cw[std::min(a.x, b.x)];
				e.first += len;
				e.second++;
			} else {
				auto &e = rh[std::min(a.y, b.y)];
				e.first += len;
				e.second++;
			}
		}
	}
	std::map<int, float> ux, vy;
	{
		const int x0 = cw.begin()->first, x1 = cw.rbegin()->first + 1;
		float at = 0.0f;
		for (int x = x0; x <= x1; x++) {
			ux[x] = at;
			auto it = cw.find(x);
			at += it != cw.end() ? it->second.first / it->second.second : 0.0f;
		}
		const int y0 = rh.begin()->first, y1 = rh.rbegin()->first + 1;
		at = 0.0f;
		for (int y = y0; y <= y1; y++) {
			vy[y] = at;
			auto it = rh.find(y);
			at += it != rh.end() ? it->second.first / it->second.second : 0.0f;
		}
	}
	const float W = ux.rbegin()->second - ux.begin()->second, H = vy.rbegin()->second - vy.begin()->second;
	const bool turn = along_v ? W > H : H > W;
	for (const auto &kv : grid) {
		std::vector<Vector2> l;
		for (const Vector2i &c : kv.second) {
			Vector2 p(ux[c.x], vy[c.y]);
			if (turn) {
				p = Vector2(p.y, -p.x); // (a quarter turn, not a mirror)
			}
			l.push_back(p);
		}
		r_out[kv.first] = l;
	}
	// unmirrored as the faces are seen: the first face's turn as its plain projection's
	{
		const int f = faces[0];
		Layout pl;
		planar(m, { f }, face_normal(m, m.faces[f]), pl);
		auto signed_area = [](const std::vector<Vector2> &q) {
			float a = 0.0f;
			for (size_t i = 0; i < q.size(); i++) {
				a += q[i].cross(q[(i + 1) % q.size()]);
			}
			return a;
		};
		if ((signed_area(r_out[f]) > 0.0f) != (signed_area(pl[f]) > 0.0f)) {
			for (int g : faces) {
				for (Vector2 &p : r_out[g]) {
					p.y = -p.y;
				}
			}
		}
	}
	// a wall: V down it, as the projections lay it (else half turned: still not mirrored)
	{
		double cov = 0.0, ym = 0.0, vm = 0.0;
		int cnt = 0;
		for (int g : faces) {
			const Face &F = m.faces[g];
			for (int i = 0; i < F.size(); i++) {
				ym += m.pos[F.v[i]].y;
				vm += r_out[g][i].y;
				cnt++;
			}
		}
		ym /= std::max(1, cnt);
		vm /= std::max(1, cnt);
		for (int g : faces) {
			const Face &F = m.faces[g];
			for (int i = 0; i < F.size(); i++) {
				cov += (m.pos[F.v[i]].y - ym) * (r_out[g][i].y - vm);
			}
		}
		if (cov > 1e-6) {
			for (int g : faces) {
				for (Vector2 &p : r_out[g]) {
					p = -p;
				}
			}
		}
	}
	return true;
}

namespace {
void bounds_of(const std::vector<int> &island, const Layout &io, Vector2 &r_mn, Vector2 &r_mx) {
	r_mn = Vector2(1e30f, 1e30f);
	r_mx = Vector2(-1e30f, -1e30f);
	for (int f : island) {
		auto it = io.find(f);
		if (it == io.end()) {
			continue;
		}
		for (const Vector2 &p : it->second) {
			r_mn = r_mn.min(p);
			r_mx = r_mx.max(p);
		}
	}
}
} // namespace

void row(const std::vector<std::vector<int>> &islands, Layout &io, const Vector2 &origin, float gap) {
	float x = origin.x;
	for (const std::vector<int> &is : islands) {
		Vector2 mn, mx;
		bounds_of(is, io, mn, mx);
		if (mn.x > mx.x) {
			continue;
		}
		const Vector2 d = Vector2(x, origin.y) - mn;
		for (int f : is) {
			for (Vector2 &p : io[f]) {
				p += d;
			}
		}
		x += (mx.x - mn.x) + gap;
	}
}

void pack(const std::vector<std::vector<int>> &islands, Layout &io, bool rotate, float margin, float density) {
	struct Isl {
		std::vector<int> faces;
		Vector2 size;
	};
	std::vector<Isl> is;
	for (const std::vector<int> &faces : islands) {
		std::vector<Vector2> pts;
		for (int f : faces) {
			for (const Vector2 &p : io[f]) {
				pts.push_back(p);
			}
		}
		if (pts.empty()) {
			continue;
		}
		if (rotate) {
			// the turn of its smallest box: one of its outline's sides along it (tried on all the sides between points)
			Vector2 c;
			for (const Vector2 &p : pts) {
				c += p / (float)pts.size();
			}
			float best = 1e30f, best_a = 0.0f;
			for (int f : faces) {
				const std::vector<Vector2> &q = io[f];
				for (size_t i = 0; i < q.size(); i++) {
					const Vector2 e = q[(i + 1) % q.size()] - q[i];
					if (e.length() < 1e-9f) {
						continue;
					}
					const float a = -e.angle();
					Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
					for (const Vector2 &p : pts) {
						const Vector2 r = (p - c).rotated(a);
						mn = mn.min(r);
						mx = mx.max(r);
					}
					const float ar = (mx.x - mn.x) * (mx.y - mn.y);
					if (ar < best - 1e-12f) {
						best = ar;
						best_a = a;
					}
				}
			}
			for (int f : faces) {
				for (Vector2 &p : io[f]) {
					p = (p - c).rotated(best_a) + c;
				}
			}
		}
		Vector2 mn, mx;
		bounds_of(faces, io, mn, mx);
		if (rotate && mx.y - mn.y > mx.x - mn.x) {
			for (int f : faces) {
				for (Vector2 &p : io[f]) {
					p = Vector2(p.y, -p.x); // (lying down: the shelves fill better)
				}
			}
			bounds_of(faces, io, mn, mx);
		}
		for (int f : faces) {
			for (Vector2 &p : io[f]) {
				p -= mn;
			}
		}
		is.push_back({ faces, (mx - mn).max(Vector2(1e-6f, 1e-6f)) });
	}
	if (is.empty()) {
		return;
	}
	std::vector<int> order(is.size());
	std::iota(order.begin(), order.end(), 0);
	std::sort(order.begin(), order.end(), [&](int a, int b) { return is[a].size.y > is[b].size.y; });
	auto shelves = [&](float k, std::vector<Vector2> &r_pos, float width) {
		r_pos.assign(is.size(), Vector2());
		float x = margin, y = margin, rowh = 0.0f;
		for (int i : order) {
			const Vector2 sz = is[i].size * k;
			if (x + sz.x + margin > width && x > margin) {
				x = margin;
				y += rowh + margin;
				rowh = 0.0f;
			}
			r_pos[i] = Vector2(x, y);
			x += sz.x + margin;
			rowh = std::max(rowh, sz.y);
		}
		return y + rowh + margin;
	};
	std::vector<Vector2> pos;
	float k = 1.0f;
	if (density > 0.0f) {
		k = 1.0f / density; // (meters to UV: as asked; the shelves as wide as the square, as tall as they come)
		shelves(k, pos, 1.0f);
	} else {
		float big = 0.0f;
		for (const Isl &I : is) {
			big = std::max(big, std::max(I.size.x, I.size.y));
		}
		float lo = 0.0f, hi = 1.0f / std::max(1e-9f, big);
		std::vector<Vector2> best;
		for (int it = 0; it < 40; it++) {
			const float mid = (lo + hi) * 0.5f;
			std::vector<Vector2> p;
			if (shelves(mid, p, 1.0f) <= 1.0f) {
				lo = mid;
				best = p;
			} else {
				hi = mid;
			}
		}
		k = lo;
		pos = best.empty() ? std::vector<Vector2>(is.size()) : best;
		if (best.empty()) {
			shelves(k, pos, 1.0f);
		}
	}
	for (size_t i = 0; i < is.size(); i++) {
		for (int f : is[i].faces) {
			for (Vector2 &p : io[f]) {
				p = pos[i] + p * k;
			}
		}
	}
}

void stretch_corners(const Mesh &m, int face, const std::vector<Vector2> &uv, float ref, int kind, std::vector<float> &r_out) {
	const Face &F = m.faces[face];
	const int n = F.size();
	r_out.assign(n, 0.0f);
	if (n < 3 || (int)uv.size() != n) {
		return;
	}
	if (kind == 1) {
		float a = 1.0f, g = 0.0f;
		stretch(m, face, uv, ref, a, g);
		const float t = std::min(1.0f, std::fabs(std::log2(std::max(1e-6f, a))) * 0.5f);
		r_out.assign(n, t);
		return;
	}
	auto angle = [](const Vector3 &a, const Vector3 &b) {
		const float la = a.length(), lb = b.length();
		return la < 1e-12f || lb < 1e-12f ? 0.0f : std::acos(std::clamp(a.dot(b) / (la * lb), -1.0f, 1.0f));
	};
	for (int i = 0; i < n; i++) {
		const int p = (i + n - 1) % n, q = (i + 1) % n;
		const float a3 = angle(m.pos[F.v[p]] - m.pos[F.v[i]], m.pos[F.v[q]] - m.pos[F.v[i]]);
		const Vector2 up = uv[p] - uv[i], uq = uv[q] - uv[i];
		const float a2 = angle(Vector3(up.x, up.y, 0), Vector3(uq.x, uq.y, 0));
		r_out[i] = std::min(1.0f, std::fabs(a3 - a2) / std::max(0.05f, a3) * 1.5f);
	}
}

godot::Color stretch_color(float t) {
	t = std::clamp(t, 0.0f, 1.0f);
	// (along the hues from blue to red, soft: not too bright, not too grey)
	return godot::Color::from_hsv(0.62f * (1.0f - t), 0.42f + 0.18f * t, 0.9f);
}

void stretch(const Mesh &m, int face, const std::vector<Vector2> &uv, float ref, float &r_area, float &r_angle) {
	const Face &F = m.faces[face];
	r_area = 1.0f;
	r_angle = 0.0f;
	if (F.size() < 3 || uv.size() != F.v.size()) {
		return;
	}
	float a3 = 0.0f, a2 = 0.0f, ang = 0.0f;
	int n = 0;
	for (int i = 1; i + 1 < F.size(); i++) {
		const int t[3] = { 0, i, i + 1 };
		const Vector3 p0 = m.pos[F.v[t[0]]], p1 = m.pos[F.v[t[1]]], p2 = m.pos[F.v[t[2]]];
		const Vector3 nn = (p1 - p0).cross(p2 - p0);
		if (nn.length() < 1e-12f) {
			continue;
		}
		const Vector3 e1 = (p1 - p0).normalized(), e2 = nn.normalized().cross(e1);
		const Vector2 q1((p1 - p0).dot(e1), 0.0f), q2((p2 - p0).dot(e1), (p2 - p0).dot(e2));
		const Vector2 u1 = uv[t[1]] - uv[t[0]], u2 = uv[t[2]] - uv[t[0]];
		a3 += 0.5f * nn.length();
		a2 += 0.5f * std::fabs(u1.cross(u2));
		// the map from the face's plane to UV: J = [u1 u2] [q1 q2]^-1; its two stretches s1 >= s2
		const float det = q1.x * q2.y - q2.x * q1.y;
		if (std::fabs(det) < 1e-12f) {
			continue;
		}
		const float i00 = q2.y / det, i01 = -q2.x / det, i10 = -q1.y / det, i11 = q1.x / det;
		const float j00 = u1.x * i00 + u2.x * i10, j01 = u1.x * i01 + u2.x * i11;
		const float j10 = u1.y * i00 + u2.y * i10, j11 = u1.y * i01 + u2.y * i11;
		const float a = j00 * j00 + j10 * j10, b = j00 * j01 + j10 * j11, c = j01 * j01 + j11 * j11;
		const float root = std::sqrt(std::max(0.0f, (a - c) * (a - c) * 0.25f + b * b));
		const float s1 = std::sqrt(std::max(0.0f, (a + c) * 0.5f + root)), s2 = std::sqrt(std::max(0.0f, (a + c) * 0.5f - root));
		ang += s1 > 1e-12f ? 1.0f - s2 / s1 : 1.0f;
		n++;
	}
	r_area = a3 > 1e-12f ? (a2 / a3) / std::max(1e-12f, ref) : 1.0f;
	r_angle = n ? ang / n : 0.0f;
}

} // namespace unwrap
} // namespace hewn
