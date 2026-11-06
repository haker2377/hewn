#include "hw_core.h"

#include <functional>

#include <numeric>

namespace hewn {

// ------------------------------------------------------------------ Face

void Face::ensure_attrs() {
	const size_t n = v.size();
	if (uv.size() != n) {
		uv.resize(n, Vector2());
	}
	if (col.size() != n) {
		col.resize(n, Color(1, 1, 1, 1));
	}
	if (!cust.empty() && cust.size() != n * 4) {
		cust.resize(n * 4, Color(0, 0, 0, 0));
	}
}

namespace {
// the 4 channels of each corner move together
void reverse_groups(std::vector<Color> &p_c) {
	const size_t n = p_c.size() / 4;
	for (size_t i = 0; i < n / 2; i++) {
		for (int k = 0; k < 4; k++) {
			std::swap(p_c[i * 4 + k], p_c[(n - 1 - i) * 4 + k]);
		}
	}
}
} // namespace

void Face::reverse() {
	ensure_attrs();
	std::reverse(v.begin(), v.end());
	std::reverse(uv.begin(), uv.end());
	std::reverse(col.begin(), col.end());
	reverse_groups(cust);
}

void Face::copy_attrs_from(const Face &o) {
	material = o.material;
	smooth = o.smooth;
	uv_group = o.uv_group;
	uvp = o.uvp;
	strength = o.strength;
	role = o.role;
	tag = o.tag;
}

void Face::rotate_to(int corner) {
	ensure_attrs();
	if (corner <= 0 || corner >= (int)v.size()) {
		return;
	}
	std::rotate(v.begin(), v.begin() + corner, v.end());
	std::rotate(uv.begin(), uv.begin() + corner, uv.end());
	std::rotate(col.begin(), col.begin() + corner, col.end());
	if (!cust.empty()) {
		std::rotate(cust.begin(), cust.begin() + corner * 4, cust.end());
	}
}

void Face::erase_corner(int i) {
	ensure_attrs();
	v.erase(v.begin() + i);
	uv.erase(uv.begin() + i);
	col.erase(col.begin() + i);
	if (!cust.empty()) {
		cust.erase(cust.begin() + i * 4, cust.begin() + i * 4 + 4);
	}
}

void Face::insert_corner(int at, int vid, const Vector2 &p_uv, const Color &c) {
	ensure_attrs();
	v.insert(v.begin() + at, vid);
	uv.insert(uv.begin() + at, p_uv);
	col.insert(col.begin() + at, c);
	if (!cust.empty()) {
		cust.insert(cust.begin() + at * 4, 4, Color(0, 0, 0, 0));
	}
}

Face make_face(const std::vector<int> &verts, const Face *attrs_from) {
	Face f;
	f.v = verts;
	if (attrs_from) {
		f.copy_attrs_from(*attrs_from);
	}
	f.ensure_attrs();
	return f;
}

// ------------------------------------------------------------------ Topology

void Topology::build(const Mesh &m) {
	nverts = (int)m.pos.size();
	half.clear();
	edge_id.clear();
	edges.clear();
	edge_faces.clear();
	vert_faces.assign(nverts, {});
	vert_edges.assign(nverts, {});
	half.reserve(m.faces.size() * 4 + 8);
	edge_id.reserve(m.faces.size() * 2 + 8);
	for (int f = 0; f < (int)m.faces.size(); f++) {
		const Face &F = m.faces[f];
		const int n = F.size();
		for (int i = 0; i < n; i++) {
			const int a = F.v[i];
			const int b = F.v[(i + 1) % n];
			half.emplace(hkey(a, b), std::make_pair(f, i));
			const uint64_t k = ekey(a, b);
			auto it = edge_id.find(k);
			int e;
			if (it == edge_id.end()) {
				e = (int)edges.size();
				edge_id.emplace(k, e);
				edges.push_back(Edge(a, b));
				edge_faces.push_back({});
				vert_edges[a].push_back(e);
				if (b != a) {
					vert_edges[b].push_back(e);
				}
			} else {
				e = it->second;
			}
			if (edge_faces[e].empty() || edge_faces[e].back() != f) {
				edge_faces[e].push_back(f);
			}
			std::vector<int> &vf = vert_faces[a];
			if (std::find(vf.begin(), vf.end(), f) == vf.end()) {
				vf.push_back(f);
			}
		}
	}
}

int Topology::find_edge(int a, int b) const {
	auto it = edge_id.find(ekey(a, b));
	return it == edge_id.end() ? -1 : it->second;
}

int Topology::face_of_half(int a, int b) const {
	auto it = half.find(hkey(a, b));
	return it == half.end() ? -1 : it->second.first;
}

bool Topology::shares_face(int e1, int e2) const {
	for (int f1 : edge_faces[e1]) {
		for (int f2 : edge_faces[e2]) {
			if (f1 == f2) {
				return true;
			}
		}
	}
	return false;
}

int Topology::other_face(int e, int f) const {
	for (int g : edge_faces[e]) {
		if (g != f) {
			return g;
		}
	}
	return -1;
}

// ------------------------------------------------------------------ geometry

Vector3 newell(const std::vector<Vector3> &p) {
	Vector3 n;
	const size_t c = p.size();
	for (size_t i = 0; i < c; i++) {
		const Vector3 &a = p[i];
		const Vector3 &b = p[(i + 1) % c];
		n.x += (a.y - b.y) * (a.z + b.z);
		n.y += (a.z - b.z) * (a.x + b.x);
		n.z += (a.x - b.x) * (a.y + b.y);
	}
	return n;
}

void face_points(const Mesh &m, const Face &f, std::vector<Vector3> &out) {
	out.resize(f.v.size());
	for (size_t i = 0; i < f.v.size(); i++) {
		out[i] = m.pos[f.v[i]];
	}
}

Vector3 face_normal_raw(const Mesh &m, const Face &f) {
	Vector3 n;
	const size_t c = f.v.size();
	for (size_t i = 0; i < c; i++) {
		const Vector3 &a = m.pos[f.v[i]];
		const Vector3 &b = m.pos[f.v[(i + 1) % c]];
		n.x += (a.y - b.y) * (a.z + b.z);
		n.y += (a.z - b.z) * (a.x + b.x);
		n.z += (a.x - b.x) * (a.y + b.y);
	}
	return n;
}

Vector3 face_normal(const Mesh &m, const Face &f) {
	Vector3 n = face_normal_raw(m, f);
	const float l = n.length();
	if (l < 1e-12f) {
		return Vector3(0, 1, 0);
	}
	return n / l;
}

Vector3 face_center(const Mesh &m, const Face &f) {
	Vector3 c;
	if (f.v.empty()) {
		return c;
	}
	for (int i : f.v) {
		c += m.pos[i];
	}
	return c / (float)f.v.size();
}

void plane_basis(const Vector3 &n, Vector3 &t, Vector3 &b) {
	Vector3 ref = std::fabs(n.x) < 0.9f ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
	t = ref.cross(n);
	const float l = t.length();
	t = l > 1e-12f ? t / l : Vector3(1, 0, 0);
	b = n.cross(t);
}

float edge_length(const Mesh &m, const Edge &e) {
	return m.pos[e.a].distance_to(m.pos[e.b]);
}

static inline float cross2(const Vector2 &a, const Vector2 &b) {
	return a.x * b.y - a.y * b.x;
}

// Strictly inside: farther than `tol` from every edge.
static bool point_in_tri_strict(const Vector2 &p, const Vector2 &a, const Vector2 &b, const Vector2 &c, float tol) {
	const float d1 = cross2(b - a, p - a);
	const float d2 = cross2(c - b, p - b);
	const float d3 = cross2(a - c, p - c);
	return d1 > tol * (b - a).length() && d2 > tol * (c - b).length() && d3 > tol * (a - c).length();
}

void triangulate(const std::vector<Vector3> &pts, const Vector3 &normal, std::vector<int> &out) {
	const int n = (int)pts.size();
	if (n < 3) {
		return;
	}
	if (n == 3) {
		out.push_back(0);
		out.push_back(1);
		out.push_back(2);
		return;
	}
	Vector3 nn = normal;
	if (nn.length_squared() < 1e-20f) {
		nn = newell(pts);
	}
	if (nn.length_squared() < 1e-20f) {
		for (int k = 1; k < n - 1; k++) {
			out.push_back(0);
			out.push_back(k);
			out.push_back(k + 1);
		}
		return;
	}
	nn.normalize();
	Vector3 t, b;
	plane_basis(nn, t, b);
	std::vector<Vector2> p2(n);
	for (int i = 0; i < n; i++) {
		p2[i] = Vector2(pts[i].dot(t), pts[i].dot(b));
	}
	if (n == 4) {
		// Convex quad: split along the shorter diagonal.
		bool convex = true;
		for (int i = 0; i < 4; i++) {
			if (cross2(p2[(i + 1) % 4] - p2[i], p2[(i + 2) % 4] - p2[(i + 1) % 4]) <= 0.0f) {
				convex = false;
				break;
			}
		}
		if (convex) {
			if (pts[0].distance_squared_to(pts[2]) <= pts[1].distance_squared_to(pts[3])) {
				out.insert(out.end(), { 0, 1, 2, 0, 2, 3 });
			} else {
				out.insert(out.end(), { 1, 2, 3, 1, 3, 0 });
			}
			return;
		}
	}
	// Tolerances relative to the polygon size: after booleans / knife cuts the outlines carry extra
	// vertices on straight edges whose float noise must not block the ears.
	Vector2 mn = p2[0], mx = p2[0];
	for (const Vector2 &p : p2) {
		mn = mn.min(p);
		mx = mx.max(p);
	}
	const float tol = std::max((mx - mn).length(), 1e-12f) * 1e-5f; // distance treated as zero
	std::vector<int> idx(n);
	std::iota(idx.begin(), idx.end(), 0);
	int guard = 0;
	while (idx.size() > 3 && guard++ < n * n + 10) {
		const int m = (int)idx.size();
		int best = -1;
		float best_score = -1.0f;
		int flat = -1; // a (nearly) straight corner: dropping it adds no area
		for (int i = 0; i < m; i++) {
			const int i0 = idx[(i + m - 1) % m];
			const int i1 = idx[i];
			const int i2 = idx[(i + 1) % m];
			const Vector2 &a = p2[i0];
			const Vector2 &bb = p2[i1];
			const Vector2 &c = p2[i2];
			const float cr = cross2(bb - a, c - bb);
			const float off = tol * (c - a).length(); // cr = |c - a| * distance of bb from the line a-c
			if (std::fabs(cr) <= off && (bb - a).dot(c - bb) >= 0.0f) {
				if (flat < 0) {
					flat = i;
				}
				continue;
			}
			if (cr <= off) {
				continue;
			}
			bool inside = false;
			for (int j = 0; j < m; j++) {
				const int k = idx[j];
				if (k == i0 || k == i1 || k == i2) {
					continue;
				}
				if (p2[k].distance_to(a) <= tol || p2[k].distance_to(bb) <= tol || p2[k].distance_to(c) <= tol) {
					continue;
				}
				// a vertex lying on the new diagonal a-c blocks the ear too (the cut would pass through it)
				const Vector2 ac = c - a;
				const float t = ac.dot(p2[k] - a) / std::max(ac.length_squared(), 1e-30f);
				const bool on_diagonal = t > 0.0f && t < 1.0f && (a + ac * t).distance_to(p2[k]) <= tol;
				if (on_diagonal || point_in_tri_strict(p2[k], a, bb, c, tol)) {
					inside = true;
					break;
				}
			}
			if (inside) {
				continue;
			}
			// prefer well shaped ears
			const float l = (bb - a).length_squared() + (c - bb).length_squared() + (a - c).length_squared();
			const float score = l > 0 ? cr / l : 0;
			if (score > best_score) {
				best_score = score;
				best = i;
			}
		}
		if (best < 0 && flat >= 0) {
			// Remove the straight corner. Its neighbors keep the edge, so the outline does not change; the
			// corner still gets covered by the triangles of the remaining polygon.
			const int m2 = (int)idx.size();
			out.push_back(idx[(flat + m2 - 1) % m2]);
			out.push_back(idx[flat]);
			out.push_back(idx[(flat + 1) % m2]);
			idx.erase(idx.begin() + flat);
			continue;
		}
		if (best < 0) {
			break;
		}
		const int m2 = (int)idx.size();
		out.push_back(idx[(best + m2 - 1) % m2]);
		out.push_back(idx[best]);
		out.push_back(idx[(best + 1) % m2]);
		idx.erase(idx.begin() + best);
	}
	if (idx.size() >= 3) {
		for (size_t k = 1; k + 1 < idx.size(); k++) {
			out.push_back(idx[0]);
			out.push_back(idx[k]);
			out.push_back(idx[k + 1]);
		}
	}
}

std::vector<Vector3> vertex_normals(const Mesh &m) {
	std::vector<Vector3> n(m.pos.size());
	for (const Face &f : m.faces) {
		const Vector3 fn = face_normal_raw(m, f);
		for (int v : f.v) {
			n[v] += fn;
		}
	}
	for (Vector3 &x : n) {
		const float l = x.length();
		x = l > 1e-12f ? x / l : Vector3(0, 1, 0);
	}
	return n;
}

// ------------------------------------------------------------------ maintenance

bool clean_face(Face &f) {
	f.ensure_attrs();
	bool changed = true;
	while (changed && f.v.size() >= 2) {
		changed = false;
		const int n = (int)f.v.size();
		for (int i = 0; i < n; i++) {
			if (f.v[i] == f.v[(i + 1) % n]) {
				f.erase_corner((i + 1) % n);
				changed = true;
				break;
			}
		}
		if (changed) {
			continue;
		}
		if (n >= 3) {
			for (int i = 0; i < n; i++) {
				// spike: a b a  -> remove b and the duplicated a
				const int ia = i, ib = (i + 1) % n, ic = (i + 2) % n;
				if (f.v[ia] == f.v[ic]) {
					const int hi = std::max(ib, ic), lo = std::min(ib, ic);
					f.erase_corner(hi);
					f.erase_corner(lo);
					changed = true;
					break;
				}
			}
		}
	}
	if (f.v.size() < 3) {
		return false;
	}
	// Reject faces that visit a vertex twice (figure-eight) only if they are tiny.
	return true;
}

void delete_faces_mask(Mesh &m, const std::vector<char> &del, std::vector<int> *remap) {
	std::vector<Face> nf;
	nf.reserve(m.faces.size());
	if (remap) {
		remap->assign(m.faces.size(), -1);
	}
	for (size_t i = 0; i < m.faces.size(); i++) {
		if (i < del.size() && del[i]) {
			continue;
		}
		if (remap) {
			(*remap)[i] = (int)nf.size();
		}
		nf.push_back(std::move(m.faces[i]));
	}
	m.faces.swap(nf);
}

void cleanup(Mesh &m, std::vector<int> *face_remap) {
	std::vector<char> del(m.faces.size(), 0);
	bool any = false;
	for (size_t i = 0; i < m.faces.size(); i++) {
		if (!clean_face(m.faces[i])) {
			del[i] = 1;
			any = true;
		}
	}
	if (any || face_remap) {
		delete_faces_mask(m, del, face_remap);
	}
}

const EdgeAttr *Mesh::edge_attr(int a, int b) const {
	auto it = edge_attrs.find(ekey(a, b));
	return it == edge_attrs.end() ? nullptr : &it->second;
}

bool Mesh::has_wire(int a, int b) const {
	for (const std::pair<int, int> &w : wires) {
		if ((w.first == a && w.second == b) || (w.first == b && w.second == a)) {
			return true;
		}
	}
	return false;
}

static void clean_points(Mesh &m) {
	if (m.points.empty()) {
		return;
	}
	std::vector<char> used(m.pos.size(), 0);
	for (const Face &f : m.faces) {
		for (int v : f.v) {
			if (v >= 0 && v < (int)used.size()) {
				used[v] = 1;
			}
		}
	}
	for (const std::pair<int, int> &w : m.wires) {
		if (w.first >= 0 && w.first < (int)used.size()) {
			used[w.first] = 1;
		}
		if (w.second >= 0 && w.second < (int)used.size()) {
			used[w.second] = 1;
		}
	}
	std::vector<int> out;
	for (int v : m.points) {
		if (v >= 0 && v < (int)used.size() && !used[v]) {
			used[v] = 1; // (once)
			out.push_back(v);
		}
	}
	m.points.swap(out);
}

void clean_wires(Mesh &m) {
	if (m.wires.empty()) {
		clean_points(m);
		return;
	}
	std::unordered_set<uint64_t> seen;
	for (const Face &f : m.faces) {
		for (int i = 0; i < f.size(); i++) {
			seen.insert(ekey(f.v[i], f.v[(i + 1) % f.size()]));
		}
	}
	const int nv = (int)m.pos.size();
	std::vector<std::pair<int, int>> out;
	for (const std::pair<int, int> &w : m.wires) {
		if (w.first < 0 || w.second < 0 || w.first >= nv || w.second >= nv || w.first == w.second) {
			continue;
		}
		if (seen.insert(ekey(w.first, w.second)).second) {
			out.push_back(w);
		}
	}
	m.wires.swap(out);
	clean_points(m);
}

void remap_vertex_attrs(Mesh &m, const std::vector<int> &map) {
	if (!m.points.empty()) {
		std::vector<int> np;
		for (int v : m.points) {
			if (v >= 0 && v < (int)map.size() && map[v] >= 0) {
				np.push_back(map[v]);
			}
		}
		m.points.swap(np);
	}
	if (!m.wires.empty()) {
		std::vector<std::pair<int, int>> nw;
		for (const std::pair<int, int> &w : m.wires) {
			if (w.first < (int)map.size() && w.second < (int)map.size() && map[w.first] >= 0 && map[w.second] >= 0 && map[w.first] != map[w.second]) {
				nw.push_back({ map[w.first], map[w.second] });
			}
		}
		m.wires.swap(nw);
	}
	if (!m.edge_attrs.empty()) {
		std::unordered_map<uint64_t, EdgeAttr> na;
		for (const auto &kv : m.edge_attrs) {
			const int a = (int)(kv.first >> 32), b = (int)(kv.first & 0xffffffffu);
			if (a >= (int)map.size() || b >= (int)map.size() || map[a] < 0 || map[b] < 0 || map[a] == map[b] || kv.second.empty()) {
				continue;
			}
			na[ekey(map[a], map[b])] = kv.second;
		}
		m.edge_attrs.swap(na);
	}
	if (!m.vert_crease.empty()) {
		std::unordered_map<int, float> nc;
		for (const auto &kv : m.vert_crease) {
			if (kv.first < (int)map.size() && map[kv.first] >= 0 && kv.second > 0.0f) {
				nc[map[kv.first]] = std::max(nc[map[kv.first]], kv.second);
			}
		}
		m.vert_crease.swap(nc);
	}
}

void remove_unused_vertices(Mesh &m, Selection *sel) {
	std::vector<int> map(m.pos.size(), -1);
	std::vector<Vector3> np;
	np.reserve(m.pos.size());
	for (const Face &f : m.faces) {
		for (int v : f.v) {
			if (v >= 0 && v < (int)map.size() && map[v] < 0) {
				map[v] = 0;
			}
		}
	}
	for (const std::pair<int, int> &w : m.wires) {
		for (int v : { w.first, w.second }) {
			if (v >= 0 && v < (int)map.size()) {
				map[v] = 0;
			}
		}
	}
	for (int v : m.points) {
		if (v >= 0 && v < (int)map.size()) {
			map[v] = 0;
		}
	}
	for (size_t i = 0; i < m.pos.size(); i++) {
		if (map[i] == 0) {
			map[i] = (int)np.size();
			np.push_back(m.pos[i]);
		}
	}
	for (Face &f : m.faces) {
		for (int &v : f.v) {
			v = map[v];
		}
	}
	m.pos.swap(np);
	remap_vertex_attrs(m, map);
	if (sel) {
		std::vector<int> nv;
		for (int v : sel->verts) {
			if (v >= 0 && v < (int)map.size() && map[v] >= 0) {
				nv.push_back(map[v]);
			}
		}
		sel->verts = nv;
		std::vector<Edge> ne;
		for (const Edge &e : sel->edges) {
			if (e.a < (int)map.size() && e.b < (int)map.size() && map[e.a] >= 0 && map[e.b] >= 0) {
				ne.push_back(Edge(map[e.a], map[e.b]));
			}
		}
		sel->edges = ne;
	}
}

bool validate(Mesh &m) {
	bool ok = true;
	std::vector<char> del(m.faces.size(), 0);
	const int nv = (int)m.pos.size();
	for (size_t i = 0; i < m.faces.size(); i++) {
		Face &f = m.faces[i];
		f.ensure_attrs();
		for (int v : f.v) {
			if (v < 0 || v >= nv) {
				del[i] = 1;
				ok = false;
				break;
			}
		}
		if (f.v.size() < 3) {
			del[i] = 1;
			ok = false;
		}
	}
	if (!ok) {
		delete_faces_mask(m, del);
	}
	return ok;
}

namespace {
struct GridKey {
	int64_t x, y, z;
	bool operator==(const GridKey &o) const { return x == o.x && y == o.y && z == o.z; }
};
struct GridHash {
	size_t operator()(const GridKey &k) const {
		return (size_t)(k.x * 73856093LL) ^ (size_t)(k.y * 19349663LL) ^ (size_t)(k.z * 83492791LL);
	}
};
} // namespace

int weld_new_vertices(Mesh &m, int first, float dist) {
	if (first >= (int)m.pos.size() || dist <= 0.0f) {
		return 0;
	}
	// Edges shorter than `dist` with a new end collapse onto the older end. Only along edges: two surfaces
	// that touch (walls of two rooms back to back) keep their own vertices.
	std::vector<int> root(m.pos.size());
	for (size_t i = 0; i < root.size(); i++) {
		root[i] = (int)i;
	}
	std::function<int(int)> find = [&](int x) {
		while (root[x] != x) {
			root[x] = root[root[x]];
			x = root[x];
		}
		return x;
	};
	int merged = 0;
	for (const Face &F : m.faces) {
		for (int i = 0; i < F.size(); i++) {
			const int a = F.v[i], b = F.v[(i + 1) % F.size()];
			if (a == b || (a < first && b < first) || m.pos[a].distance_squared_to(m.pos[b]) > dist * dist) {
				continue;
			}
			const int ra = find(a), rb = find(b);
			if (ra != rb) {
				root[std::max(ra, rb)] = std::min(ra, rb);
				merged++;
			}
		}
	}
	if (merged == 0) {
		return 0;
	}
	for (Face &f : m.faces) {
		for (int &v : f.v) {
			v = find(v);
		}
	}
	for (std::pair<int, int> &w : m.wires) {
		w.first = find(w.first);
		w.second = find(w.second);
	}
	for (int &v : m.points) {
		v = find(v);
	}
	cleanup(m);
	clean_wires(m);
	return merged;
}

void weld_by_distance(Mesh &m, float eps) {
	if (eps <= 0.0f) {
		eps = 1e-5f;
	}
	const float inv = 1.0f / eps;
	std::unordered_map<GridKey, std::vector<int>, GridHash> grid;
	std::vector<int> map(m.pos.size());
	std::vector<Vector3> np;
	for (size_t i = 0; i < m.pos.size(); i++) {
		const Vector3 &p = m.pos[i];
		const GridKey base{ (int64_t)std::floor(p.x * inv), (int64_t)std::floor(p.y * inv), (int64_t)std::floor(p.z * inv) };
		int found = -1;
		for (int dx = -1; dx <= 1 && found < 0; dx++) {
			for (int dy = -1; dy <= 1 && found < 0; dy++) {
				for (int dz = -1; dz <= 1 && found < 0; dz++) {
					auto it = grid.find(GridKey{ base.x + dx, base.y + dy, base.z + dz });
					if (it == grid.end()) {
						continue;
					}
					for (int c : it->second) {
						if (np[c].distance_squared_to(p) <= eps * eps) {
							found = c;
							break;
						}
					}
				}
			}
		}
		if (found < 0) {
			found = (int)np.size();
			np.push_back(p);
			grid[base].push_back(found);
		}
		map[i] = found;
	}
	for (Face &f : m.faces) {
		for (int &v : f.v) {
			v = map[v];
		}
	}
	m.pos.swap(np);
	remap_vertex_attrs(m, map);
	cleanup(m);
	clean_wires(m);
}

} // namespace hewn
