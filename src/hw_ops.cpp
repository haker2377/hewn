// Modeling operations on the n-gon mesh.
// Operations may leave unreferenced vertices behind; callers run remove_unused_vertices().
#include "hw_core.h"

#include <array>
#include <random>

#include <deque>
#include <functional>
#include <map>
#include <set>

namespace hewn {
namespace ops {

using sel::uniq;

template <typename T>
static inline T clamp01(T x) {
	return x < T(0) ? T(0) : (x > T(1) ? T(1) : x);
}

namespace {

struct Corner {
	int v = 0;
	Vector2 uv;
	Color c = Color(1, 1, 1, 1);
	Color cu[4]; // CUSTOM0..3 (with has_cu)
	bool has_cu = false;
};

std::vector<Corner> corners_of(const Face &f) {
	std::vector<Corner> out(f.v.size());
	for (size_t i = 0; i < f.v.size(); i++) {
		out[i].v = f.v[i];
		out[i].uv = i < f.uv.size() ? f.uv[i] : Vector2();
		out[i].c = i < f.col.size() ? f.col[i] : Color(1, 1, 1, 1);
		if (f.cust.size() >= (i + 1) * 4) {
			out[i].has_cu = true;
			for (int k = 0; k < 4; k++) {
				out[i].cu[k] = f.cust[i * 4 + k];
			}
		}
	}
	return out;
}

Face face_from(const std::vector<Corner> &cs, const Face &attrs) {
	Face f;
	f.copy_attrs_from(attrs);
	f.manual_uv = attrs.manual_uv;
	f.v.resize(cs.size());
	f.uv.resize(cs.size());
	f.col.resize(cs.size());
	bool cu = false;
	for (size_t i = 0; i < cs.size(); i++) {
		f.v[i] = cs[i].v;
		f.uv[i] = cs[i].uv;
		f.col[i] = cs[i].c;
		cu = cu || cs[i].has_cu;
	}
	if (cu) {
		f.cust.resize(cs.size() * 4);
		for (size_t i = 0; i < cs.size(); i++) {
			for (int k = 0; k < 4; k++) {
				f.cust[i * 4 + k] = cs[i].cu[k];
			}
		}
	}
	return f;
}

Corner lerp_corner(const Corner &a, const Corner &b, float t, int v) {
	Corner c;
	c.v = v;
	c.uv = a.uv.lerp(b.uv, t);
	c.c = a.c.lerp(b.c, t);
	c.has_cu = a.has_cu || b.has_cu;
	for (int k = 0; k < 4; k++) {
		c.cu[k] = a.cu[k].lerp(b.cu[k], t);
	}
	return c;
}

Corner with_vertex(const Corner &a, int v) {
	Corner c = a;
	c.v = v;
	return c;
}

std::vector<char> mask_of(const std::vector<int> &s, size_t n) {
	std::vector<char> m(n, 0);
	for (int i : s) {
		if (i >= 0 && i < (int)n) {
			m[i] = 1;
		}
	}
	return m;
}

std::vector<int> valid_faces(const Mesh &m, const std::vector<int> &faces) {
	std::vector<int> out;
	for (int f : uniq(faces)) {
		if (f >= 0 && f < (int)m.faces.size()) {
			out.push_back(f);
		}
	}
	return out;
}

// Removes degenerate faces and remaps `track` (face indices) accordingly.
void finish(Mesh &m, std::vector<int> &track) {
	std::vector<int> remap;
	cleanup(m, &remap);
	std::vector<int> out;
	for (int f : track) {
		if (f >= 0 && f < (int)remap.size() && remap[f] >= 0) {
			out.push_back(remap[f]);
		}
	}
	track = uniq(out);
}

// Circular slice of a ring from i to j inclusive.
std::vector<Corner> slice(const std::vector<Corner> &ring, int i, int j) {
	std::vector<Corner> out;
	const int n = (int)ring.size();
	int k = i;
	while (true) {
		out.push_back(ring[k]);
		if (k == j) {
			break;
		}
		k = (k + 1) % n;
	}
	return out;
}

// Splits a ring at the corner positions `idx` (ascending). With two cuts the ring is split along
// the chord; with more, every piece is closed against `center`.
std::vector<std::vector<Corner>> split_ring(const std::vector<Corner> &ring, const std::vector<int> &idx, const Corner *center) {
	std::vector<std::vector<Corner>> out;
	const int k = (int)idx.size();
	if (k == 2) {
		out.push_back(slice(ring, idx[0], idx[1]));
		out.push_back(slice(ring, idx[1], idx[0]));
		return out;
	}
	for (int j = 0; j < k; j++) {
		std::vector<Corner> p = slice(ring, idx[j], idx[(j + 1) % k]);
		p.push_back(*center);
		out.push_back(p);
	}
	return out;
}

// Boundary of a face group as a single ring. Fails on holes / pinches.
bool merge_boundary(const Mesh &m, const std::vector<int> &group, Face &out) {
	std::unordered_set<uint64_t> halves;
	for (int f : group) {
		const Face &F = m.faces[f];
		const int n = F.size();
		for (int i = 0; i < n; i++) {
			halves.insert(hkey(F.v[i], F.v[(i + 1) % n]));
		}
	}
	struct H {
		int b, f, i;
	};
	std::unordered_map<int, H> next;
	int count = 0;
	int start = -1;
	for (int f : group) {
		const Face &F = m.faces[f];
		const int n = F.size();
		for (int i = 0; i < n; i++) {
			const int a = F.v[i], b = F.v[(i + 1) % n];
			if (halves.count(hkey(b, a))) {
				continue;
			}
			if (next.count(a)) {
				return false;
			}
			next[a] = H{ b, f, i };
			count++;
			if (start < 0) {
				start = a;
			}
		}
	}
	if (count < 3) {
		return false;
	}
	std::vector<Corner> ring;
	int v = start;
	for (int guard = 0; guard <= count; guard++) {
		auto it = next.find(v);
		if (it == next.end()) {
			return false;
		}
		const Face &F = m.faces[it->second.f];
		ring.push_back(corners_of(F)[it->second.i]);
		v = it->second.b;
		if (v == start) {
			break;
		}
	}
	if (v != start || (int)ring.size() != count) {
		return false;
	}
	out = face_from(ring, m.faces[group[0]]);
	return true;
}

// Replaces groups of faces by their merged boundary. Returns new face indices.
std::vector<int> merge_groups(Mesh &m, const std::vector<std::vector<int>> &groups) {
	std::vector<char> del(m.faces.size(), 0);
	std::vector<Face> add;
	for (const std::vector<int> &g : groups) {
		if (g.size() < 2) {
			continue;
		}
		Face nf;
		if (!merge_boundary(m, g, nf)) {
			continue;
		}
		for (int f : g) {
			del[f] = 1;
		}
		add.push_back(nf);
	}
	delete_faces_mask(m, del);
	std::vector<int> out;
	for (Face &f : add) {
		out.push_back((int)m.faces.size());
		m.faces.push_back(f);
	}
	finish(m, out);
	return out;
}

struct UnionFind {
	std::vector<int> p;
	explicit UnionFind(int n) :
			p(n) {
		for (int i = 0; i < n; i++) {
			p[i] = i;
		}
	}
	int find(int x) {
		while (p[x] != x) {
			p[x] = p[p[x]];
			x = p[x];
		}
		return x;
	}
	void unite(int a, int b) { p[find(a)] = find(b); }
};

std::vector<std::vector<int>> groups_of(UnionFind &uf, const std::vector<int> &items) {
	std::map<int, std::vector<int>> g;
	for (int i : items) {
		g[uf.find(i)].push_back(i);
	}
	std::vector<std::vector<int>> out;
	for (auto &kv : g) {
		out.push_back(kv.second);
	}
	return out;
}

// Edge-connected components of a face set.
std::vector<std::vector<int>> face_components(const Mesh &m, const Topology &t, const std::vector<int> &faces) {
	std::vector<char> fm = mask_of(faces, m.faces.size());
	UnionFind uf((int)m.faces.size());
	for (size_t e = 0; e < t.edges.size(); e++) {
		const std::vector<int> &ef = t.edge_faces[e];
		for (size_t i = 1; i < ef.size(); i++) {
			if (fm[ef[0]] && fm[ef[i]]) {
				uf.unite(ef[0], ef[i]);
			}
		}
	}
	return groups_of(uf, faces);
}

std::vector<std::vector<int>> border_loops(const Mesh &m) {
	// Returns rings that would close the holes (orientation matching the surrounding faces).
	std::unordered_set<uint64_t> halves;
	for (const Face &F : m.faces) {
		const int n = F.size();
		for (int i = 0; i < n; i++) {
			halves.insert(hkey(F.v[i], F.v[(i + 1) % n]));
		}
	}
	std::unordered_map<int, int> next;
	std::vector<int> starts;
	for (const Face &F : m.faces) {
		const int n = F.size();
		for (int i = 0; i < n; i++) {
			const int a = F.v[i], b = F.v[(i + 1) % n];
			if (!halves.count(hkey(b, a))) {
				if (!next.count(b)) {
					next[b] = a;
					starts.push_back(b);
				}
			}
		}
	}
	std::vector<std::vector<int>> out;
	std::unordered_set<int> used;
	for (int s : starts) {
		if (used.count(s)) {
			continue;
		}
		std::vector<int> loop;
		int v = s;
		bool ok = false;
		for (size_t guard = 0; guard <= next.size(); guard++) {
			if (used.count(v)) {
				break;
			}
			loop.push_back(v);
			used.insert(v);
			auto it = next.find(v);
			if (it == next.end()) {
				break;
			}
			v = it->second;
			if (v == s) {
				ok = true;
				break;
			}
		}
		if (ok && loop.size() >= 3) {
			out.push_back(loop);
		}
	}
	return out;
}

} // namespace

// ================================================================== faces

std::vector<int> extrude_faces(Mesh &m, const std::vector<int> &p_faces, float dist, bool individual, std::vector<int> *moved_verts) {
	const std::vector<int> faces = valid_faces(m, p_faces);
	std::vector<int> moved;
	if (faces.empty()) {
		return faces;
	}
	if (individual) {
		for (int f : faces) {
			const Face F = m.faces[f];
			const Vector3 n = face_normal(m, F);
			const std::vector<Corner> cs = corners_of(F);
			const int c = F.size();
			std::vector<int> nv(c);
			for (int i = 0; i < c; i++) {
				nv[i] = m.add_vertex(m.pos[F.v[i]] + n * dist);
				moved.push_back(nv[i]);
			}
			for (int i = 0; i < c; i++) {
				const int j = (i + 1) % c;
				std::vector<Corner> w = { cs[i], cs[j], with_vertex(cs[j], nv[j]), with_vertex(cs[i], nv[i]) };
				Face wf = face_from(w, F);
				wf.manual_uv = false;
				m.faces.push_back(wf);
			}
			m.faces[f].v = nv;
		}
	} else {
		Topology t;
		t.build(m);
		std::vector<char> fm = mask_of(faces, m.faces.size());
		std::vector<char> dup(m.pos.size(), 0);
		std::vector<Vector3> dir(m.pos.size());
		std::vector<float> mindot(m.pos.size(), 1.0f);
		for (int v = 0; v < (int)m.pos.size(); v++) {
			for (int g : t.vert_faces[v]) {
				if (!fm[g]) {
					dup[v] = 1;
					break;
				}
			}
		}
		std::vector<std::vector<char>> border(m.faces.size());
		for (int f : faces) {
			const Face &F = m.faces[f];
			const int n = F.size();
			border[f].assign(n, 0);
			const Vector3 fn = face_normal(m, F);
			for (int i = 0; i < n; i++) {
				dir[F.v[i]] += fn;
				const int e = t.find_edge(F.v[i], F.v[(i + 1) % n]);
				bool b = t.edge_faces[e].size() < 2;
				for (int g : t.edge_faces[e]) {
					b = b || !fm[g];
				}
				if (b) {
					border[f][i] = 1;
					dup[F.v[i]] = 1;
					dup[F.v[(i + 1) % n]] = 1;
				}
			}
		}
		std::vector<int> nv(m.pos.size(), -1);
		const int nverts = (int)m.pos.size();
		for (int f : faces) {
			const Face &F = m.faces[f];
			const Vector3 fn = face_normal(m, F);
			for (int v : F.v) {
				const float l = dir[v].length();
				if (l > 1e-9f) {
					mindot[v] = std::min(mindot[v], (dir[v] / l).dot(fn));
				}
			}
		}
		for (int v = 0; v < nverts; v++) {
			if (dir[v].length_squared() < 1e-18f) {
				continue;
			}
			const Vector3 d = dir[v].normalized() * (dist / std::max(mindot[v], 0.34f));
			if (dup[v]) {
				nv[v] = m.add_vertex(m.pos[v] + d);
			} else {
				m.pos[v] += d;
				nv[v] = v;
			}
			moved.push_back(nv[v]);
		}
		for (int f : faces) {
			const Face F = m.faces[f];
			const std::vector<Corner> cs = corners_of(F);
			const int n = F.size();
			for (int i = 0; i < n; i++) {
				if (!border[f][i]) {
					continue;
				}
				const int j = (i + 1) % n;
				std::vector<Corner> w = { cs[i], cs[j], with_vertex(cs[j], nv[F.v[j]]), with_vertex(cs[i], nv[F.v[i]]) };
				Face wf = face_from(w, F);
				wf.manual_uv = false;
				m.faces.push_back(wf);
			}
			for (int &v : m.faces[f].v) {
				v = nv[v];
			}
		}
	}
	if (moved_verts) {
		*moved_verts = uniq(moved);
	}
	return faces;
}

std::vector<int> inset_faces(Mesh &m, const std::vector<int> &p_faces, float amount, bool individual) {
	InsetOptions o;
	o.thickness = amount;
	o.individual = individual;
	o.interpolate = false; // (as it always was: the new corners copy the old ones)
	return inset_faces_ex(m, p_faces, o);
}

namespace {
// Mean value weights of a point in a face (on its plane): the corners' share of it. On a corner: that
// corner; on an edge: its two ends.
std::vector<float> polygon_weights(const Mesh &m, const Face &F, const Vector3 &p_point) {
	const int n = F.size();
	std::vector<float> w(n, 0.0f);
	const Vector3 N = face_normal(m, F);
	std::vector<Vector3> d(n);
	std::vector<float> r(n);
	for (int i = 0; i < n; i++) {
		d[i] = m.pos[F.v[i]] - p_point;
		d[i] -= N * N.dot(d[i]);
		r[i] = d[i].length();
		if (r[i] < 1e-7f) {
			w[i] = 1.0f;
			return w;
		}
	}
	std::vector<float> th(n);
	for (int i = 0; i < n; i++) {
		const int j = (i + 1) % n;
		const float c = d[i].dot(d[j]) / (r[i] * r[j]);
		const float s = N.dot(d[i].cross(d[j])) / (r[i] * r[j]);
		if (std::fabs(s) < 1e-6f && c < 0.0f) {
			const float t = r[i] / (r[i] + r[j]); // (on this edge)
			std::fill(w.begin(), w.end(), 0.0f);
			w[i] = 1.0f - t;
			w[j] = t;
			return w;
		}
		th[i] = std::tan(std::atan2(s, c) * 0.5f);
	}
	float sum = 0.0f;
	for (int i = 0; i < n; i++) {
		w[i] = (th[(i + n - 1) % n] + th[i]) / r[i];
		sum += w[i];
	}
	if (std::fabs(sum) < 1e-12f) {
		std::fill(w.begin(), w.end(), 1.0f / n);
		return w;
	}
	for (float &x : w) {
		x /= sum;
	}
	return w;
}

Corner corner_by_weights(const std::vector<Corner> &cs, const std::vector<float> &w, int v) {
	Corner out;
	out.v = v;
	out.uv = Vector2();
	out.c = Color(0, 0, 0, 0);
	for (int k = 0; k < 4; k++) {
		out.cu[k] = Color(0, 0, 0, 0);
	}
	for (size_t i = 0; i < cs.size() && i < w.size(); i++) {
		out.uv += cs[i].uv * w[i];
		out.c = out.c + cs[i].c * w[i];
		out.has_cu = out.has_cu || cs[i].has_cu;
		for (int k = 0; k < 4; k++) {
			out.cu[k] = out.cu[k] + cs[i].cu[k] * w[i];
		}
	}
	return out;
}
} // namespace

std::vector<int> inset_faces_ex(Mesh &m, const std::vector<int> &p_faces, const InsetOptions &o) {
	const std::vector<int> faces = valid_faces(m, p_faces);
	std::vector<int> ring;
	if (faces.empty()) {
		return faces;
	}
	const float amount = std::fabs(o.thickness);
	const float sign = o.outset ? -1.0f : 1.0f;
	// the corner of a new point in face F: inside it (interpolate) or the old corner's copy
	auto new_corner = [&](const Face &F, const std::vector<Corner> &cs, int i, int v) {
		if (!o.interpolate) {
			return with_vertex(cs[i], v);
		}
		return corner_by_weights(cs, polygon_weights(m, F, m.pos[v]), v);
	};
	// how far along a direction a point goes to be `amount` away from the line along `in`
	auto even_len = [&](const Vector3 &dir, const Vector3 &in) {
		return o.even ? amount / std::max(dir.dot(in), 0.25f) : amount;
	};
	if (o.individual) {
		for (int f : faces) {
			const Face F = m.faces[f];
			const std::vector<Corner> cs = corners_of(F);
			const Vector3 n = face_normal(m, F);
			const int c = F.size();
			std::vector<Vector3> in(c);
			float mean = 0.0f;
			for (int i = 0; i < c; i++) {
				in[i] = n.cross(m.pos[F.v[(i + 1) % c]] - m.pos[F.v[i]]).normalized();
				mean += m.pos[F.v[(i + 1) % c]].distance_to(m.pos[F.v[i]]) / c;
			}
			std::vector<int> nv(c), keep(c);
			for (int i = 0; i < c; i++) {
				const int p = (i + c - 1) % c;
				Vector3 dir = in[p] + in[i];
				dir = dir.length_squared() < 1e-12f ? in[i] : dir.normalized();
				float len = even_len(dir, in[i]);
				if (o.relative && mean > 1e-9f) {
					len *= (m.pos[F.v[i]].distance_to(m.pos[F.v[p]]) + m.pos[F.v[i]].distance_to(m.pos[F.v[(i + 1) % c]])) * 0.5f / mean;
				}
				nv[i] = m.add_vertex(m.pos[F.v[i]] + dir * (len * sign));
				keep[i] = o.outset ? m.add_vertex(m.pos[F.v[i]]) : F.v[i]; // (outset: the face's own copy, its neighbors keep theirs)
			}
			std::vector<Corner> inner(c);
			for (int i = 0; i < c; i++) {
				inner[i] = o.outset ? with_vertex(cs[i], keep[i]) : new_corner(F, cs, i, nv[i]);
			}
			for (int i = 0; i < c; i++) {
				const int j = (i + 1) % c;
				std::vector<Corner> w = o.outset ? std::vector<Corner>{ new_corner(F, cs, i, nv[i]), new_corner(F, cs, j, nv[j]), inner[j], inner[i] }
												 : std::vector<Corner>{ cs[i], cs[j], inner[j], inner[i] };
				ring.push_back((int)m.faces.size());
				m.faces.push_back(face_from(w, F));
			}
			Face nf = face_from(inner, F);
			m.faces[f] = nf;
			if (o.depth != 0.0f) {
				for (int i = 0; i < c; i++) {
					m.pos[inner[i].v] += n * o.depth;
				}
			}
		}
		return o.select_outer ? ring : faces;
	}
	Topology t;
	t.build(m);
	std::vector<char> fm = mask_of(faces, m.faces.size());
	std::unordered_map<int, std::vector<Vector3>> dirs;
	std::unordered_map<int, std::vector<int>> open_rail; // open edges not inset (Boundary off): the point slides along them
	std::unordered_map<int, float> elen;
	struct BH {
		int f, i;
	};
	std::vector<BH> bh;
	float total_len = 0.0f;
	int total_cnt = 0;
	for (int f : faces) {
		const Face &F = m.faces[f];
		const int n = F.size();
		const Vector3 fn = face_normal(m, F);
		for (int i = 0; i < n; i++) {
			const int a = F.v[i], b = F.v[(i + 1) % n];
			const int e = t.find_edge(a, b);
			const bool open = t.edge_faces[e].size() < 2;
			bool border = open && o.boundary;
			for (int g : t.edge_faces[e]) {
				border = border || !fm[g];
			}
			if (!border) {
				if (open) {
					open_rail[a].push_back(b);
					open_rail[b].push_back(a);
				}
				continue;
			}
			const float L = m.pos[a].distance_to(m.pos[b]);
			const Vector3 in = fn.cross(m.pos[b] - m.pos[a]).normalized();
			dirs[a].push_back(in);
			dirs[b].push_back(in);
			elen[a] += L;
			elen[b] += L;
			total_len += L;
			total_cnt++;
			bh.push_back(BH{ f, i });
		}
	}
	const float mean = total_cnt > 0 ? total_len / total_cnt : 0.0f;
	// the inside edges of the region at each point (Edge Rail)
	auto inner_neighbors = [&](int v) {
		std::vector<int> out;
		for (int f : faces) {
			const Face &F = m.faces[f];
			const int n = F.size();
			for (int i = 0; i < n; i++) {
				if (F.v[i] != v) {
					continue;
				}
				for (int x : { F.v[(i + 1) % n], F.v[(i + n - 1) % n] }) {
					const int e = t.find_edge(v, x);
					bool inside = t.edge_faces[e].size() == 2;
					for (int g : t.edge_faces[e]) {
						inside = inside && fm[g];
					}
					if (inside && std::find(out.begin(), out.end(), x) == out.end()) {
						out.push_back(x);
					}
				}
			}
		}
		return out;
	};
	std::unordered_map<int, int> nv;
	for (auto &kv : dirs) {
		const int v = kv.first;
		const std::vector<Vector3> &d = kv.second;
		Vector3 s;
		for (const Vector3 &x : d) {
			s += x;
		}
		Vector3 dir = s.length_squared() > 1e-12f ? s.normalized() : d[0];
		float len = even_len(dir, d[0]);
		if (d.size() > 2) {
			len = amount;
		}
		bool railed = false;
		if (!o.outset) {
			// along an open edge that is not inset, or (Edge Rail) along the inside edge nearest the direction
			std::vector<int> rails = open_rail.count(v) ? open_rail[v] : std::vector<int>();
			if (rails.empty() && o.edge_rail) {
				rails = inner_neighbors(v);
			}
			float best = 0.1f;
			for (int x : rails) {
				const Vector3 r = (m.pos[x] - m.pos[v]).normalized();
				if (r.dot(dir) > best) {
					best = r.dot(dir);
					float rl = amount / std::max(r.dot(d[0]), 0.1f);
					if (!o.even) {
						rl = amount;
					}
					dir = r;
					len = rl;
					railed = true;
				}
			}
		}
		(void)railed;
		if (o.relative && mean > 1e-9f) {
			len *= (elen[v] / d.size()) / mean;
		}
		nv[v] = m.add_vertex(m.pos[v] + dir * (len * sign));
	}
	// the region's normal at its points (Depth moves the inside part along it)
	std::unordered_map<int, Vector3> vnrm;
	if (o.depth != 0.0f) {
		for (int f : faces) {
			const Vector3 fn = face_normal(m, m.faces[f]);
			for (int v : m.faces[f].v) {
				vnrm[v] += fn;
			}
		}
	}
	if (!o.outset) {
		for (const BH &h : bh) {
			const Face F = m.faces[h.f];
			const std::vector<Corner> cs = corners_of(F);
			const int n = F.size();
			const int j = (h.i + 1) % n;
			std::vector<Corner> w = { cs[h.i], cs[j], new_corner(F, cs, j, nv[F.v[j]]), new_corner(F, cs, h.i, nv[F.v[h.i]]) };
			ring.push_back((int)m.faces.size());
			m.faces.push_back(face_from(w, F));
		}
		for (int f : faces) {
			const Face F = m.faces[f];
			const std::vector<Corner> cs = corners_of(F);
			std::vector<Corner> w = cs;
			bool any = false;
			for (int i = 0; i < F.size(); i++) {
				auto it = nv.find(F.v[i]);
				if (it != nv.end()) {
					w[i] = new_corner(F, cs, i, it->second);
					any = true;
				}
			}
			if (any) {
				Face nf = face_from(w, F);
				nf.hidden = F.hidden;
				m.faces[f] = nf;
			}
		}
		if (o.depth != 0.0f) {
			std::unordered_set<int> moved;
			std::unordered_map<int, int> came_from;
			for (const auto &kv : nv) {
				came_from[kv.second] = kv.first;
			}
			for (int f : faces) {
				for (int v : m.faces[f].v) {
					if (moved.insert(v).second) {
						// (a new point: the normal of the old one it came from)
						const int from = came_from.count(v) ? came_from[v] : v;
						const Vector3 nn = vnrm.count(from) ? vnrm[from].normalized() : Vector3();
						m.pos[v] += nn * o.depth;
					}
				}
			}
		}
	} else {
		// outset: the faces keep their points, the ring goes around them outside, the neighbors move onto it
		const size_t first_ring = m.faces.size();
		for (const BH &h : bh) {
			const Face F = m.faces[h.f];
			const std::vector<Corner> cs = corners_of(F);
			const int n = F.size();
			const int j = (h.i + 1) % n;
			std::vector<Corner> w = { new_corner(F, cs, h.i, nv[F.v[h.i]]), new_corner(F, cs, j, nv[F.v[j]]), cs[j], cs[h.i] };
			ring.push_back((int)m.faces.size());
			m.faces.push_back(face_from(w, F));
		}
		for (size_t f = 0; f < first_ring; f++) {
			if (fm[f]) {
				continue;
			}
			for (int &v : m.faces[f].v) {
				auto it = nv.find(v);
				if (it != nv.end()) {
					v = it->second;
				}
			}
		}
		if (o.depth != 0.0f) {
			for (const auto &kv : vnrm) {
				m.pos[kv.first] += kv.second.normalized() * o.depth;
			}
		}
	}
	return o.select_outer ? ring : faces;
}

void delete_faces(Mesh &m, const std::vector<int> &faces) {
	delete_faces_mask(m, mask_of(faces, m.faces.size()));
}

void delete_elements(Mesh &m, DeleteKind kind, const std::vector<int> &p_verts, const std::vector<Edge> &p_edges, const std::vector<int> &p_faces) {
	const int nv = (int)m.pos.size();
	std::unordered_set<int> vs;
	for (int v : p_verts) {
		if (v >= 0 && v < nv) {
			vs.insert(v);
		}
	}
	std::unordered_set<uint64_t> es;
	for (const Edge &e : p_edges) {
		es.insert(ekey(e.a, e.b));
	}
	std::vector<char> del(m.faces.size(), 0);
	if (kind == DEL_FACES || kind == DEL_ONLY_FACES || kind == DEL_ONLY_EDGES_FACES) {
		for (int f : p_faces) {
			if (f >= 0 && f < (int)m.faces.size()) {
				del[f] = 1;
			}
		}
	}
	for (int f = 0; f < (int)m.faces.size(); f++) {
		const Face &F = m.faces[f];
		for (int i = 0; i < F.size() && !del[f]; i++) {
			if (kind == DEL_VERTS) {
				del[f] = vs.count(F.v[i]) ? 1 : 0;
			} else if (kind == DEL_EDGES || kind == DEL_ONLY_EDGES_FACES) {
				del[f] = es.count(ekey(F.v[i], F.v[(i + 1) % F.size()])) ? 1 : 0;
			}
		}
	}
	// the edges the kept faces still use
	std::unordered_set<uint64_t> kept_face_edges;
	std::vector<char> used(nv, 0);
	for (int f = 0; f < (int)m.faces.size(); f++) {
		if (del[f]) {
			continue;
		}
		const Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			kept_face_edges.insert(ekey(F.v[i], F.v[(i + 1) % F.size()]));
			used[F.v[i]] = 1;
		}
	}
	// an edge that goes (by the kind); the others of the deleted faces stay as wires
	auto edge_goes = [&](int a, int b) {
		switch (kind) {
			case DEL_VERTS:
				return vs.count(a) > 0 || vs.count(b) > 0;
			case DEL_EDGES:
			case DEL_ONLY_EDGES_FACES:
				return es.count(ekey(a, b)) > 0;
			default:
				return false;
		}
	};
	std::vector<std::pair<int, int>> wires;
	std::unordered_set<uint64_t> seen;
	auto add_wire = [&](int a, int b) {
		const uint64_t k = ekey(a, b);
		if (a == b || kept_face_edges.count(k) || edge_goes(a, b) || !seen.insert(k).second) {
			return;
		}
		wires.push_back({ a, b });
		used[a] = used[b] = 1;
	};
	for (const std::pair<int, int> &w : m.wires) {
		add_wire(w.first, w.second);
	}
	std::unordered_set<int> loose; // the vertices that may be left alone: kept as points (or not, by the kind)
	for (int f = 0; f < (int)m.faces.size(); f++) {
		if (!del[f]) {
			continue;
		}
		const Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			loose.insert(F.v[i]);
			if (kind != DEL_FACES) {
				add_wire(F.v[i], F.v[(i + 1) % F.size()]);
			}
		}
	}
	for (const std::pair<int, int> &w : m.wires) {
		loose.insert(w.first);
		loose.insert(w.second);
	}
	std::vector<int> points;
	for (int v : m.points) {
		if (v >= 0 && v < nv && !(kind == DEL_VERTS && vs.count(v))) {
			points.push_back(v);
			used[v] = 1;
		}
	}
	for (int v : loose) {
		if (v < 0 || v >= nv || used[v] || (vs.count(v) && kind == DEL_VERTS)) {
			continue;
		}
		// Delete Edges / Faces drop the vertices left alone; the others keep them
		bool keep = kind == DEL_VERTS || kind == DEL_ONLY_EDGES_FACES || kind == DEL_ONLY_FACES;
		if (keep) {
			points.push_back(v);
			used[v] = 1;
		}
	}
	m.wires.swap(wires);
	m.points.swap(points);
	delete_faces_mask(m, del);
}

void flip_faces(Mesh &m, const std::vector<int> &faces) {
	for (int f : valid_faces(m, faces)) {
		m.faces[f].reverse();
	}
}

// Subdivide: every edge of the faces cut `cuts` times; quads become a grid, triangles a grid of
// triangles, other faces a fan around their middle. Neighbors get the new points (n-gons), or are cut into
// triangles (without n-gons). The new points can be pushed onto a smooth surface (smoothness: Phong-like,
// from the vertex normals) and moved at random (fractal, along the normal by `along_normal`).
std::vector<int> subdivide_faces_ex(Mesh &m, const std::vector<int> &p_faces, int p_cuts, float p_smooth, bool p_ngons, float p_fractal, float p_along_normal, int p_seed) {
	const std::vector<int> faces = valid_faces(m, p_faces);
	const int k = std::max(1, std::min(p_cuts, 100));
	const int N = k + 1;
	std::vector<char> fm = mask_of(faces, m.faces.size());
	const std::vector<Vector3> vn = vertex_normals(m);
	const int first_new = (int)m.pos.size();
	// how each new point is made of the old ones (for the smoothing)
	std::vector<std::vector<std::pair<int, float>>> made;
	auto add_point = [&](const std::vector<std::pair<int, float>> &p_w) {
		Vector3 p;
		for (const std::pair<int, float> &w : p_w) {
			p += m.pos[w.first] * w.second;
		}
		made.push_back(p_w);
		return m.add_vertex(p);
	};
	// the points on each edge, from the lower index to the higher
	std::unordered_map<uint64_t, std::vector<int>> cut;
	auto edge_points = [&](int a, int b) {
		const uint64_t key = ekey(a, b);
		auto it = cut.find(key);
		if (it == cut.end()) {
			const int lo = std::min(a, b), hi = std::max(a, b);
			std::vector<int> pts;
			for (int i = 1; i <= k; i++) {
				const float t = (float)i / (float)N;
				pts.push_back(add_point({ { lo, 1.0f - t }, { hi, t } }));
			}
			it = cut.emplace(key, pts).first;
		}
		std::vector<int> pts = it->second;
		if (a > b) {
			std::reverse(pts.begin(), pts.end());
		}
		return pts; // (from a to b)
	};
	for (int f : faces) {
		const Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			edge_points(F.v[i], F.v[(i + 1) % F.size()]);
		}
	}
	std::vector<int> out;
	std::vector<int> neighbors; // (changed, to cut into triangles without n-gons)
	const int nf = (int)m.faces.size();
	auto corner_at = [&](const std::vector<Corner> &cs, const std::vector<float> &w, int v) {
		Corner c;
		c.v = v;
		c.uv = Vector2();
		c.c = Color(0, 0, 0, 0);
		for (size_t i = 0; i < cs.size(); i++) {
			c.uv += cs[i].uv * w[i];
			c.c += cs[i].c * w[i];
		}
		return c;
	};
	for (int f = 0; f < nf; f++) {
		const Face F = m.faces[f];
		const std::vector<Corner> cs = corners_of(F);
		const int n = F.size();
		if (!fm[f]) {
			std::vector<Corner> ring;
			bool changed = false;
			for (int i = 0; i < n; i++) {
				ring.push_back(cs[i]);
				const int q = (i + 1) % n;
				if (cut.count(ekey(cs[i].v, cs[q].v))) {
					const std::vector<int> pts = edge_points(cs[i].v, cs[q].v);
					for (int j = 0; j < k; j++) {
						ring.push_back(lerp_corner(cs[i], cs[q], (float)(j + 1) / (float)N, pts[j]));
					}
					changed = true;
				}
			}
			if (changed) {
				m.faces[f] = face_from(ring, F);
				neighbors.push_back(f);
			}
			continue;
		}
		std::vector<Face> made_faces;
		if (n == 4) {
			// a grid: (i, j) from corner 0 along 0-1 and 0-3
			std::vector<std::vector<int>> g(N + 1, std::vector<int>(N + 1, -1));
			g[0][0] = cs[0].v;
			g[N][0] = cs[1].v;
			g[N][N] = cs[2].v;
			g[0][N] = cs[3].v;
			const std::vector<int> e01 = edge_points(cs[0].v, cs[1].v), e12 = edge_points(cs[1].v, cs[2].v);
			const std::vector<int> e32 = edge_points(cs[3].v, cs[2].v), e03 = edge_points(cs[0].v, cs[3].v);
			for (int i = 1; i <= k; i++) {
				g[i][0] = e01[i - 1];
				g[N][i] = e12[i - 1];
				g[i][N] = e32[i - 1];
				g[0][i] = e03[i - 1];
			}
			for (int i = 1; i <= k; i++) {
				for (int j = 1; j <= k; j++) {
					const float u = (float)i / N, v = (float)j / N;
					g[i][j] = add_point({ { cs[0].v, (1 - u) * (1 - v) }, { cs[1].v, u * (1 - v) }, { cs[2].v, u * v }, { cs[3].v, (1 - u) * v } });
				}
			}
			auto C = [&](int i, int j) {
				const float u = (float)i / N, v = (float)j / N;
				return corner_at(cs, { (1 - u) * (1 - v), u * (1 - v), u * v, (1 - u) * v }, g[i][j]);
			};
			for (int i = 0; i < N; i++) {
				for (int j = 0; j < N; j++) {
					made_faces.push_back(face_from({ C(i, j), C(i + 1, j), C(i + 1, j + 1), C(i, j + 1) }, F));
				}
			}
		} else if (n == 3) {
			// a grid of triangles: (i, j) = corner 0 + i / N toward 1 + j / N toward 2
			std::vector<std::vector<int>> g(N + 1, std::vector<int>(N + 1, -1));
			g[0][0] = cs[0].v;
			g[N][0] = cs[1].v;
			g[0][N] = cs[2].v;
			const std::vector<int> e01 = edge_points(cs[0].v, cs[1].v), e02 = edge_points(cs[0].v, cs[2].v), e12 = edge_points(cs[1].v, cs[2].v);
			for (int i = 1; i <= k; i++) {
				g[i][0] = e01[i - 1];
				g[0][i] = e02[i - 1];
				g[N - i][i] = e12[i - 1];
			}
			for (int i = 1; i < N; i++) {
				for (int j = 1; i + j < N; j++) {
					const float a = (float)i / N, b = (float)j / N;
					g[i][j] = add_point({ { cs[0].v, 1 - a - b }, { cs[1].v, a }, { cs[2].v, b } });
				}
			}
			auto C = [&](int i, int j) {
				const float a = (float)i / N, b = (float)j / N;
				return corner_at(cs, { 1 - a - b, a, b }, g[i][j]);
			};
			for (int i = 0; i < N; i++) {
				for (int j = 0; i + j < N; j++) {
					made_faces.push_back(face_from({ C(i, j), C(i + 1, j), C(i, j + 1) }, F));
					if (i + j < N - 1) {
						made_faces.push_back(face_from({ C(i + 1, j), C(i + 1, j + 1), C(i, j + 1) }, F));
					}
				}
			}
		} else {
			// a fan around the middle over every piece of the outline
			std::vector<std::pair<int, float>> mw;
			std::vector<float> cw(n, 1.0f / n);
			for (int i = 0; i < n; i++) {
				mw.push_back({ cs[i].v, 1.0f / n });
			}
			const Corner mid = corner_at(cs, cw, add_point(mw));
			std::vector<Corner> ring;
			for (int i = 0; i < n; i++) {
				const int q = (i + 1) % n;
				ring.push_back(cs[i]);
				const std::vector<int> pts = edge_points(cs[i].v, cs[q].v);
				for (int j = 0; j < k; j++) {
					ring.push_back(lerp_corner(cs[i], cs[q], (float)(j + 1) / (float)N, pts[j]));
				}
			}
			for (size_t i = 0; i < ring.size(); i++) {
				made_faces.push_back(face_from({ ring[i], ring[(i + 1) % ring.size()], mid }, F));
			}
		}
		for (size_t i = 0; i < made_faces.size(); i++) {
			if (i == 0) {
				m.faces[f] = made_faces[0];
				out.push_back(f);
			} else {
				out.push_back((int)m.faces.size());
				m.faces.push_back(made_faces[i]);
			}
		}
	}
	// the new points: onto the smooth surface through the old ones, then the random part
	if (p_smooth != 0.0f || p_fractal > 0.0f) {
		std::mt19937 rng((uint32_t)p_seed * 2654435761u + 12345u);
		std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
		float avg_len = 0.0f;
		int cnt = 0;
		for (const std::pair<const uint64_t, std::vector<int>> &e : cut) {
			const int a = (int)(e.first >> 32), b = (int)(e.first & 0xffffffffu);
			if (a < first_new && b < first_new) {
				avg_len += m.pos[a].distance_to(m.pos[b]);
				cnt++;
			}
		}
		avg_len = cnt ? avg_len / cnt : 1.0f;
		for (size_t i = 0; i < made.size(); i++) {
			const int v = first_new + (int)i;
			Vector3 p = m.pos[v];
			Vector3 nrm;
			if (p_smooth != 0.0f) {
				Vector3 q;
				for (const std::pair<int, float> &w : made[i]) {
					const Vector3 &c = m.pos[w.first];
					const Vector3 &cn = vn[w.first];
					q += (p - cn * (p - c).dot(cn)) * w.second; // (onto each corner's tangent plane)
					nrm += cn * w.second;
				}
				p = p.lerp(q, p_smooth);
			} else {
				for (const std::pair<int, float> &w : made[i]) {
					nrm += vn[w.first] * w.second;
				}
			}
			if (p_fractal > 0.0f) {
				Vector3 r(unit(rng), unit(rng), unit(rng));
				const Vector3 nn = nrm.length() > 1e-6f ? nrm.normalized() : Vector3();
				r = r.lerp(nn * (r.dot(nn) < 0.0f ? -1.0f : 1.0f) * r.length(), std::max(0.0f, std::min(1.0f, p_along_normal)));
				p += r * (p_fractal * avg_len * 0.25f);
			}
			m.pos[v] = p;
		}
	}
	if (!p_ngons && !neighbors.empty()) {
		triangulate_faces(m, neighbors); // (no faces with more corners than they had: triangles)
	}
	return out;
}

std::vector<int> subdivide_faces(Mesh &m, const std::vector<int> &p_faces) {
	const std::vector<int> faces = valid_faces(m, p_faces);
	std::vector<char> fm = mask_of(faces, m.faces.size());
	std::unordered_map<uint64_t, int> mid;
	for (int f : faces) {
		const Face &F = m.faces[f];
		const int n = F.size();
		for (int i = 0; i < n; i++) {
			const int a = F.v[i], b = F.v[(i + 1) % n];
			if (!mid.count(ekey(a, b))) {
				mid[ekey(a, b)] = m.add_vertex((m.pos[a] + m.pos[b]) * 0.5f);
			}
		}
	}
	std::vector<int> out;
	const int nf = (int)m.faces.size();
	for (int f = 0; f < nf; f++) {
		const Face F = m.faces[f];
		const std::vector<Corner> cs = corners_of(F);
		const int n = F.size();
		if (fm[f]) {
			Corner center;
			center.v = m.add_vertex(face_center(m, F));
			Vector2 uv;
			Color col(0, 0, 0, 0);
			for (const Corner &c : cs) {
				uv += c.uv;
				col += c.c;
			}
			center.uv = uv / (float)n;
			center.c = col / (float)n;
			for (int i = 0; i < n; i++) {
				const int p = (i + n - 1) % n, q = (i + 1) % n;
				const Corner en = lerp_corner(cs[i], cs[q], 0.5f, mid[ekey(cs[i].v, cs[q].v)]);
				const Corner ep = lerp_corner(cs[p], cs[i], 0.5f, mid[ekey(cs[p].v, cs[i].v)]);
				Face nf2 = face_from({ cs[i], en, center, ep }, F);
				if (i == 0) {
					m.faces[f] = nf2;
					out.push_back(f);
				} else {
					out.push_back((int)m.faces.size());
					m.faces.push_back(nf2);
				}
			}
		} else {
			std::vector<Corner> ring;
			bool changed = false;
			for (int i = 0; i < n; i++) {
				ring.push_back(cs[i]);
				auto it = mid.find(ekey(cs[i].v, cs[(i + 1) % n].v));
				if (it != mid.end()) {
					ring.push_back(lerp_corner(cs[i], cs[(i + 1) % n], 0.5f, it->second));
					changed = true;
				}
			}
			if (changed) {
				m.faces[f] = face_from(ring, F);
			}
		}
	}
	return out;
}

std::vector<int> triangulate_faces(Mesh &m, const std::vector<int> &p_faces) {
	const std::vector<int> faces = valid_faces(m, p_faces);
	std::vector<int> out;
	std::vector<Vector3> pts;
	// the texture as it shows: a face whose UVs depend on its whole outline (fitted, stretched, projected with
	// a group, bent) gives its triangles its own final UVs, so nothing moves
	std::vector<std::vector<Vector2>> shown;
	compute_uvs(m, Transform3D(), shown);
	for (int f : faces) {
		const Face F = m.faces[f];
		if (F.size() == 3) {
			out.push_back(f);
			continue;
		}
		std::vector<Corner> cs = corners_of(F);
		face_points(m, F, pts);
		std::vector<int> tris;
		const Vector3 fn = face_normal(m, F);
		triangulate(pts, fn, tris);
		bool keep = !F.manual_uv && !F.uvp.world && (F.uvp.fill != UV_FILL_TILE || F.uv_group > 0);
		for (size_t t = 0; t + 2 < tris.size() && !keep && !F.manual_uv && !F.uvp.world; t += 3) {
			const Vector3 tn = (pts[tris[t + 1]] - pts[tris[t]]).cross(pts[tris[t + 2]] - pts[tris[t]]).normalized();
			keep = std::fabs(tn.dot(fn.normalized())) < 0.99999f; // (a bent face: its triangles would be projected their own way)
		}
		if (keep && f < (int)shown.size() && shown[f].size() == cs.size()) {
			for (size_t i = 0; i < cs.size(); i++) {
				cs[i].uv = shown[f][i];
			}
		} else {
			keep = false;
		}
		for (size_t t = 0; t + 2 < tris.size(); t += 3) {
			Face nf = face_from({ cs[tris[t]], cs[tris[t + 1]], cs[tris[t + 2]] }, F);
			if (keep) {
				nf.manual_uv = true;
			}
			if (t == 0) {
				m.faces[f] = nf;
				out.push_back(f);
			} else {
				out.push_back((int)m.faces.size());
				m.faces.push_back(nf);
			}
		}
	}
	return out;
}

std::vector<int> merge_faces(Mesh &m, const std::vector<int> &p_faces) {
	const std::vector<int> faces = valid_faces(m, p_faces);
	Topology t;
	t.build(m);
	return merge_groups(m, face_components(m, t, faces));
}

std::vector<int> detach_faces(Mesh &m, const std::vector<int> &p_faces) {
	const std::vector<int> faces = valid_faces(m, p_faces);
	std::vector<char> fm = mask_of(faces, m.faces.size());
	std::vector<char> shared(m.pos.size(), 0);
	std::vector<char> in_sel(m.pos.size(), 0);
	for (int f = 0; f < (int)m.faces.size(); f++) {
		for (int v : m.faces[f].v) {
			if (fm[f]) {
				in_sel[v] = 1;
			}
		}
	}
	for (int f = 0; f < (int)m.faces.size(); f++) {
		if (fm[f]) {
			continue;
		}
		for (int v : m.faces[f].v) {
			if (in_sel[v]) {
				shared[v] = 1;
			}
		}
	}
	std::unordered_map<int, int> nv;
	for (int f : faces) {
		for (int &v : m.faces[f].v) {
			if (!shared[v]) {
				continue;
			}
			auto it = nv.find(v);
			if (it == nv.end()) {
				it = nv.emplace(v, m.add_vertex(m.pos[v])).first;
			}
			v = it->second;
		}
	}
	return faces;
}

std::vector<int> duplicate_faces(Mesh &m, const std::vector<int> &p_faces) {
	const std::vector<int> faces = valid_faces(m, p_faces);
	std::unordered_map<int, int> nv;
	std::vector<int> out;
	for (int f : faces) {
		Face nf = m.faces[f];
		for (int &v : nf.v) {
			auto it = nv.find(v);
			if (it == nv.end()) {
				it = nv.emplace(v, m.add_vertex(m.pos[v])).first;
			}
			v = it->second;
		}
		out.push_back((int)m.faces.size());
		m.faces.push_back(nf);
	}
	return out;
}

void conform_normals(Mesh &m, const std::vector<int> &p_faces) {
	std::vector<int> faces = p_faces.empty() ? sel::all_faces(m) : valid_faces(m, p_faces);
	Topology t;
	t.build(m);
	std::vector<char> fm = mask_of(faces, m.faces.size());
	std::vector<char> seen(m.faces.size(), 0);
	auto has_half = [&](int f, int a, int b) {
		const Face &F = m.faces[f];
		const int n = F.size();
		for (int i = 0; i < n; i++) {
			if (F.v[i] == a && F.v[(i + 1) % n] == b) {
				return true;
			}
		}
		return false;
	};
	for (int s : faces) {
		if (seen[s]) {
			continue;
		}
		std::vector<int> comp;
		std::vector<char> flipped;
		std::deque<int> q;
		q.push_back(s);
		seen[s] = 1;
		while (!q.empty()) {
			const int f = q.front();
			q.pop_front();
			comp.push_back(f);
			const Face F = m.faces[f];
			const int n = F.size();
			for (int i = 0; i < n; i++) {
				const int a = F.v[i], b = F.v[(i + 1) % n];
				const int e = t.find_edge(a, b);
				for (int g : t.edge_faces[e]) {
					if (g == f || !fm[g] || seen[g]) {
						continue;
					}
					seen[g] = 1;
					if (has_half(g, a, b)) {
						m.faces[g].reverse();
						flipped.push_back(g);
					}
					q.push_back(g);
				}
			}
		}
		if (flipped.size() * 2 > comp.size()) {
			for (int f : comp) {
				m.faces[f].reverse();
			}
		}
	}
}

Mesh extract_faces(const Mesh &m, const std::vector<int> &faces) {
	Mesh out;
	std::unordered_map<int, int> map;
	for (int f : valid_faces(m, faces)) {
		Face nf = m.faces[f];
		for (int &v : nf.v) {
			auto it = map.find(v);
			if (it == map.end()) {
				it = map.emplace(v, out.add_vertex(m.pos[v])).first;
			}
			v = it->second;
		}
		out.faces.push_back(nf);
	}
	return out;
}

// ================================================================== edges

std::vector<Edge> extrude_edges(Mesh &m, const std::vector<Edge> &edges, float dist, bool along_normal) {
	Topology t;
	t.build(m);
	struct EH {
		int f, i;
	};
	std::vector<EH> hs;
	std::unordered_map<int, Vector3> off;
	std::unordered_map<int, int> cnt;
	for (const Edge &E : uniq(edges)) {
		const int e = t.find_edge(E.a, E.b);
		if (e < 0 || t.edge_faces[e].empty()) {
			continue;
		}
		const int f = t.edge_faces[e][0];
		const Face &F = m.faces[f];
		const int n = F.size();
		int ci = -1;
		for (int i = 0; i < n; i++) {
			if (Edge(F.v[i], F.v[(i + 1) % n]) == E) {
				ci = i;
				break;
			}
		}
		if (ci < 0) {
			continue;
		}
		const int a = F.v[ci], b = F.v[(ci + 1) % n];
		const Vector3 fn = face_normal(m, F);
		Vector3 d = along_normal ? fn : (m.pos[b] - m.pos[a]).cross(fn).normalized();
		off[a] += d;
		off[b] += d;
		cnt[a]++;
		cnt[b]++;
		hs.push_back(EH{ f, ci });
	}
	std::unordered_map<int, int> nv;
	for (auto &kv : off) {
		Vector3 d = kv.second;
		if (d.length_squared() > 1e-12f) {
			d.normalize();
		}
		nv[kv.first] = m.add_vertex(m.pos[kv.first] + d * dist);
	}
	std::vector<Edge> out;
	for (const EH &h : hs) {
		const Face F = m.faces[h.f];
		const std::vector<Corner> cs = corners_of(F);
		const int n = F.size();
		const Corner &ca = cs[h.i], &cb = cs[(h.i + 1) % n];
		const int na = nv[ca.v], nb = nv[cb.v];
		Face wf = face_from({ cb, ca, with_vertex(ca, na), with_vertex(cb, nb) }, F);
		wf.manual_uv = false;
		m.faces.push_back(wf);
		out.push_back(Edge(na, nb));
	}
	return out;
}

void harden_normals(Mesh &m, const std::vector<int> &bevel_faces) {
	if (bevel_faces.empty()) {
		return;
	}
	std::unordered_set<int> bev(bevel_faces.begin(), bevel_faces.end());
	std::unordered_set<int> verts;
	for (int f : bevel_faces) {
		if (f >= 0 && f < (int)m.faces.size()) {
			verts.insert(m.faces[f].v.begin(), m.faces[f].v.end());
		}
	}
	int group = 0;
	for (const Face &F : m.faces) {
		group = std::max(group, F.smooth);
	}
	group++;
	for (int f = 0; f < (int)m.faces.size(); f++) {
		Face &F = m.faces[f];
		bool touches = bev.count(f) > 0;
		for (int i = 0; i < F.size() && !touches; i++) {
			touches = verts.count(F.v[i]) > 0;
		}
		if (!touches) {
			continue;
		}
		if (F.smooth == 0 || bev.count(f)) {
			F.smooth = group;
		}
		F.strength = bev.count(f) ? 1 : 2;
	}
}

std::vector<int> bevel_edges(Mesh &m, const std::vector<Edge> &edges, float amount, int segments, bool keep_middle, const std::vector<Vector2> *p_profile, bool p_percent) {
	Topology t;
	t.build(m);
	std::unordered_set<uint64_t> se;
	std::vector<char> bv(m.pos.size(), 0);
	for (const Edge &E : edges) {
		if (t.find_edge(E.a, E.b) >= 0) {
			se.insert(ekey(E));
			bv[E.a] = bv[E.b] = 1;
		}
	}
	std::vector<int> out;
	if (se.empty() || amount <= 0.0f) {
		return out;
	}
	std::unordered_set<uint64_t> orig_border;
	for (size_t e = 0; e < t.edges.size(); e++) {
		if (t.edge_faces[e].size() == 1) {
			const Edge &E = t.edges[e];
			orig_border.insert(t.face_of_half(E.a, E.b) >= 0 ? hkey(E.a, E.b) : hkey(E.b, E.a));
		}
	}
	auto is_sel = [&](int a, int b) { return se.count(ekey(a, b)) > 0; };
	const int first_new = (int)m.pos.size();
	auto clamp_len = [&](int a, int b) {
		const float len = m.pos[a].distance_to(m.pos[b]);
		// (percent: of the edge it slides along). Up to the other end when that one is not bevelled too (one edge of a
		// box all the way: a prism; the corners that meet there are welded below), else up to the middle.
		return std::min(p_percent ? len * amount * 0.01f : amount, len * (bv[b] || segments > 1 ? 0.49f : 1.0f)); // (rounded: up to the middle)
	};
	std::unordered_map<uint64_t, int> slides; // hkey(v, toward) -> new vertex
	auto slide = [&](int v, int x) {
		const uint64_t k = hkey(v, x);
		auto it = slides.find(k);
		if (it != slides.end()) {
			return it->second;
		}
		const Vector3 d = (m.pos[x] - m.pos[v]).normalized();
		const int nv = m.add_vertex(m.pos[v] + d * clamp_len(v, x));
		slides[k] = nv;
		return nv;
	};
	const int nf = (int)m.faces.size();
	std::vector<std::vector<std::vector<int>>> rep(nf);
	// Pass 1: corners touching a selected edge.
	for (int f = 0; f < nf; f++) {
		const Face &F = m.faces[f];
		const int n = F.size();
		rep[f].resize(n);
		for (int i = 0; i < n; i++) {
			const int u = F.v[(i + n - 1) % n], v = F.v[i], w = F.v[(i + 1) % n];
			const bool su = is_sel(u, v), sw = is_sel(v, w);
			if (su && sw) {
				const Vector3 du = (m.pos[u] - m.pos[v]).normalized() * clamp_len(v, u);
				const Vector3 dw = (m.pos[w] - m.pos[v]).normalized() * clamp_len(v, w);
				rep[f][i] = { m.add_vertex(m.pos[v] + du + dw) };
			} else if (sw) {
				rep[f][i] = { slide(v, u) };
			} else if (su) {
				rep[f][i] = { slide(v, w) };
			}
		}
	}
	// Pass 2: the remaining corners pick up slide points created on their edges.
	for (int f = 0; f < nf; f++) {
		const Face &F = m.faces[f];
		const int n = F.size();
		for (int i = 0; i < n; i++) {
			if (!rep[f][i].empty()) {
				continue;
			}
			const int u = F.v[(i + n - 1) % n], v = F.v[i], w = F.v[(i + 1) % n];
			std::vector<int> &r = rep[f][i];
			if (!bv[v]) {
				r = { v };
				continue;
			}
			auto iu = slides.find(hkey(v, u));
			auto iw = slides.find(hkey(v, w));
			if (iu != slides.end()) {
				r.push_back(iu->second);
			}
			if (iu == slides.end() || iw == slides.end()) {
				if (iu != slides.end() || iw == slides.end()) {
					r.push_back(v);
				} else {
					r.insert(r.begin(), v);
				}
			}
			if (iw != slides.end()) {
				r.push_back(iw->second);
			}
		}
	}
	// Rebuild the faces in place.
	for (int f = 0; f < nf; f++) {
		const Face F = m.faces[f];
		const std::vector<Corner> cs = corners_of(F);
		std::vector<Corner> ring;
		for (int i = 0; i < F.size(); i++) {
			for (int v : rep[f][i]) {
				ring.push_back(with_vertex(cs[i], v));
			}
		}
		m.faces[f] = face_from(ring, F);
	}
	// Rounded profile points between p and q (exclusive), a circular arc tangent to the faces at p and q.
	segments = std::max(1, segments);
	std::unordered_map<uint64_t, std::vector<int>> profiles; // hkey(p, q) -> points from p to q
	auto profile = [&](int p, int q, int orig) {
		std::vector<int> pts;
		if (segments < 2) {
			return pts;
		}
		auto it = profiles.find(hkey(q, p));
		if (it != profiles.end()) {
			pts.assign(it->second.rbegin(), it->second.rend());
			return pts;
		}
		const Vector3 P = m.pos[p], Q = m.pos[q], O = m.pos[orig];
		const Vector3 dp = P - O, dq = Q - O;
		float w = 1.0f;
		if (dp.length() > 1e-9f && dq.length() > 1e-9f) {
			w = std::sin(dp.angle_to(dq) * 0.5f);
		}
		// a custom profile: the points at equal lengths along it, (x, y) -> O + (P - O)(1 - y) + (Q - O)(1 - x)
		std::vector<float> acc;
		if (p_profile && p_profile->size() >= 2) {
			acc.push_back(0.0f);
			for (size_t i = 1; i < p_profile->size(); i++) {
				acc.push_back(acc.back() + (*p_profile)[i - 1].distance_to((*p_profile)[i]));
			}
		}
		for (int s = 1; s < segments; s++) {
			if (keep_middle && segments == 2) {
				// offset edge slide: the original edge stays, two new loops go to both sides
				pts.push_back(m.add_vertex(O));
				continue;
			}
			if (!acc.empty() && acc.back() > 1e-9f) {
				const float d = acc.back() * s / segments;
				size_t i = 0;
				while (i + 2 < acc.size() && acc[i + 1] < d) {
					i++;
				}
				const float len = acc[i + 1] - acc[i];
				const Vector2 c = (*p_profile)[i].lerp((*p_profile)[i + 1], len > 1e-9f ? (d - acc[i]) / len : 0.0f);
				pts.push_back(m.add_vertex(O + (P - O) * (1.0f - c.y) + (Q - O) * (1.0f - c.x)));
				continue;
			}
			const float u = (float)s / segments;
			const float b0 = (1 - u) * (1 - u), b1 = 2 * u * (1 - u) * w, b2 = u * u;
			pts.push_back(m.add_vertex((P * b0 + O * b1 + Q * b2) / (b0 + b1 + b2)));
		}
		profiles[hkey(p, q)] = pts;
		return pts;
	};
	// Bevel strips.
	for (const uint64_t k : se) {
		const int a = (int)(k >> 32), b = (int)(k & 0xffffffffu);
		auto h1 = t.half.find(hkey(a, b));
		auto h2 = t.half.find(hkey(b, a));
		if (h1 == t.half.end() || h2 == t.half.end()) {
			continue;
		}
		const int f1 = h1->second.first, i1 = h1->second.second;
		const int f2 = h2->second.first, i2 = h2->second.second;
		const int n1 = (int)rep[f1].size(), n2 = (int)rep[f2].size();
		const int A1 = rep[f1][i1].back(), B1 = rep[f1][(i1 + 1) % n1].front();
		const int B2 = rep[f2][i2].back(), A2 = rep[f2][(i2 + 1) % n2].front();
		std::vector<int> sa = { A1 }, sb = { B1 };
		for (int v : profile(A1, A2, a)) {
			sa.push_back(v);
		}
		for (int v : profile(B1, B2, b)) {
			sb.push_back(v);
		}
		sa.push_back(A2);
		sb.push_back(B2);
		const Color c = m.faces[f1].col.empty() ? Color(1, 1, 1, 1) : m.faces[f1].col[0];
		for (size_t s = 0; s + 1 < sa.size(); s++) {
			Face bf = make_face({ sb[s], sa[s], sa[s + 1], sb[s + 1] }, &m.faces[f1]);
			bf.manual_uv = false;
			bf.col.assign(4, c);
			if (segments > 1 && bf.smooth == 0) {
				bf.smooth = 1 << 20; // rounded strips shade smooth
			}
			out.push_back((int)m.faces.size());
			m.faces.push_back(bf);
		}
	}
	// Close the holes left around beveled vertices.
	std::unordered_set<uint64_t> halves;
	std::unordered_map<uint64_t, int> half_face;
	for (int f = 0; f < (int)m.faces.size(); f++) {
		const Face &F = m.faces[f];
		const int n = F.size();
		for (int i = 0; i < n; i++) {
			const uint64_t h = hkey(F.v[i], F.v[(i + 1) % n]);
			halves.insert(h);
			half_face[h] = f;
		}
	}
	std::unordered_map<int, int> next;
	std::unordered_map<int, int> src_face;
	for (const uint64_t h : halves) {
		const int a = (int)(h >> 32), b = (int)(h & 0xffffffffu);
		if (halves.count(hkey(b, a)) || orig_border.count(h)) {
			continue;
		}
		next[b] = a;
		src_face[b] = half_face[h];
	}
	std::unordered_set<int> used;
	for (auto &kv : next) {
		const int s = kv.first;
		if (used.count(s)) {
			continue;
		}
		std::vector<int> loop;
		int v = s;
		bool ok = false;
		for (size_t guard = 0; guard <= next.size(); guard++) {
			if (used.count(v)) {
				break;
			}
			used.insert(v);
			loop.push_back(v);
			auto it = next.find(v);
			if (it == next.end()) {
				break;
			}
			v = it->second;
			if (v == s) {
				ok = true;
				break;
			}
		}
		if (!ok || loop.size() < 3) {
			continue;
		}
		Face ff = make_face(loop, &m.faces[src_face[s]]);
		ff.manual_uv = false;
		out.push_back((int)m.faces.size());
		m.faces.push_back(ff);
	}
	weld_new_vertices(m, first_new, 1e-5f); // (slid all the way: onto the old corner)
	finish(m, out);
	return out;
}

std::vector<Edge> connect_edges(Mesh &m, const std::vector<Edge> &edges) {
	std::unordered_set<uint64_t> se;
	for (const Edge &e : edges) {
		se.insert(ekey(e));
	}
	std::unordered_map<uint64_t, int> mid;
	auto midpoint = [&](int a, int b) {
		const uint64_t k = ekey(a, b);
		auto it = mid.find(k);
		if (it != mid.end()) {
			return it->second;
		}
		const int v = m.add_vertex((m.pos[a] + m.pos[b]) * 0.5f);
		mid[k] = v;
		return v;
	};
	std::vector<Edge> out;
	const int nf = (int)m.faces.size();
	for (int f = 0; f < nf; f++) {
		const Face F = m.faces[f];
		const std::vector<Corner> cs = corners_of(F);
		const int n = F.size();
		std::vector<Corner> ring;
		std::vector<int> idx;
		for (int i = 0; i < n; i++) {
			const Corner &a = cs[i], &b = cs[(i + 1) % n];
			ring.push_back(a);
			if (se.count(ekey(a.v, b.v))) {
				idx.push_back((int)ring.size());
				ring.push_back(lerp_corner(a, b, 0.5f, midpoint(a.v, b.v)));
			}
		}
		if (idx.empty()) {
			continue;
		}
		if (idx.size() == 1) {
			m.faces[f] = face_from(ring, F);
			continue;
		}
		Corner center;
		if (idx.size() > 2) {
			center.v = m.add_vertex(face_center(m, F));
			center.uv = Vector2();
			center.c = cs[0].c;
			for (int i : idx) {
				out.push_back(Edge(ring[i].v, center.v));
			}
		} else {
			out.push_back(Edge(ring[idx[0]].v, ring[idx[1]].v));
		}
		std::vector<std::vector<Corner>> pieces = split_ring(ring, idx, &center);
		for (size_t p = 0; p < pieces.size(); p++) {
			Face nf2 = face_from(pieces[p], F);
			if (p == 0) {
				m.faces[f] = nf2;
			} else {
				m.faces.push_back(nf2);
			}
		}
	}
	return out;
}

std::vector<Edge> insert_edge_loops(Mesh &m, const std::vector<Edge> &edges) {
	Topology t;
	t.build(m);
	std::vector<Edge> ring;
	for (const Edge &e : edges) {
		std::vector<Edge> r = sel::edge_ring(m, t, e);
		ring.insert(ring.end(), r.begin(), r.end());
	}
	return connect_edges(m, uniq(ring));
}

std::vector<Edge> subdivide_edges(Mesh &m, const std::vector<Edge> &edges, int cuts) {
	cuts = std::max(1, cuts);
	std::unordered_map<uint64_t, std::vector<int>> pts; // ordered from min to max vertex
	std::vector<Edge> out;
	for (const Edge &e : uniq(edges)) {
		if (e.a < 0 || e.b >= (int)m.pos.size()) {
			continue;
		}
		std::vector<int> &p = pts[ekey(e)];
		int prev = e.a;
		for (int c = 1; c <= cuts; c++) {
			const float t = (float)c / (float)(cuts + 1);
			const int v = m.add_vertex(m.pos[e.a].lerp(m.pos[e.b], t));
			p.push_back(v);
			out.push_back(Edge(prev, v));
			prev = v;
		}
		out.push_back(Edge(prev, e.b));
	}
	for (Face &F : m.faces) {
		const std::vector<Corner> cs = corners_of(F);
		const int n = F.size();
		std::vector<Corner> ring;
		bool changed = false;
		for (int i = 0; i < n; i++) {
			const Corner &a = cs[i], &b = cs[(i + 1) % n];
			ring.push_back(a);
			auto it = pts.find(ekey(a.v, b.v));
			if (it == pts.end()) {
				continue;
			}
			changed = true;
			const std::vector<int> &p = it->second;
			const bool fwd = a.v < b.v;
			for (int c = 0; c < cuts; c++) {
				const int k = fwd ? c : cuts - 1 - c;
				ring.push_back(lerp_corner(a, b, (float)(c + 1) / (float)(cuts + 1), p[k]));
			}
		}
		if (changed) {
			F = face_from(ring, F);
		}
	}
	// wires (edges without faces) are cut into a chain the same way
	std::vector<std::pair<int, int>> nw;
	for (const std::pair<int, int> &w : m.wires) {
		auto it = pts.find(ekey(w.first, w.second));
		if (it == pts.end()) {
			nw.push_back(w);
			continue;
		}
		std::vector<int> chain = it->second;
		if (w.first > w.second) {
			std::reverse(chain.begin(), chain.end());
		}
		int prev = w.first;
		for (int v : chain) {
			nw.push_back({ prev, v });
			prev = v;
		}
		nw.push_back({ prev, w.second });
	}
	m.wires.swap(nw);
	return out;
}

std::vector<int> bridge_edges(Mesh &m, const Edge &ea, const Edge &eb) {
	Topology t;
	t.build(m);
	auto oriented = [&](const Edge &e, int &p, int &q) {
		// p->q as it appears in an adjacent face
		if (t.face_of_half(e.a, e.b) >= 0) {
			p = e.a;
			q = e.b;
		} else {
			p = e.b;
			q = e.a;
		}
		return t.face_of_half(p, q);
	};
	int a0, a1, b0, b1;
	const int fa = oriented(ea, a0, a1);
	oriented(eb, b0, b1);
	std::vector<int> out;
	if (fa < 0 && t.find_edge(ea.a, ea.b) < 0) {
		return out;
	}
	Face nf = make_face({ a1, a0, b1, b0 }, fa >= 0 ? &m.faces[fa] : nullptr);
	nf.manual_uv = false;
	if (!clean_face(nf)) {
		return out;
	}
	out.push_back((int)m.faces.size());
	m.faces.push_back(nf);
	return out;
}

std::vector<int> dissolve_edges(Mesh &m, const std::vector<Edge> &edges) {
	Topology t;
	t.build(m);
	UnionFind uf((int)m.faces.size());
	std::vector<int> touched;
	for (const Edge &E : edges) {
		const int e = t.find_edge(E.a, E.b);
		if (e < 0 || t.edge_faces[e].size() != 2) {
			continue;
		}
		const int f = t.edge_faces[e][0], g = t.edge_faces[e][1];
		uf.unite(f, g);
		touched.push_back(f);
		touched.push_back(g);
	}
	std::vector<int> out = merge_groups(m, groups_of(uf, uniq(touched)));
	// "Dissolve Vertices" option: an end of a dissolved edge that is left with only two edges
	// (a cube corner after one of its edges is gone) is removed too, so a cube becomes a prism.
	Topology t2;
	t2.build(m);
	std::vector<char> rm(m.pos.size(), 0);
	bool any = false;
	for (const Edge &E : edges) {
		for (int v : { E.a, E.b }) {
			if (v < 0 || v >= t2.nverts || t2.vert_edges[v].size() != 2 || t2.vert_faces[v].size() != 2) {
				continue;
			}
			bool ok = true;
			for (int f : t2.vert_faces[v]) {
				ok = ok && m.faces[f].size() > 3;
			}
			if (ok) {
				rm[v] = 1;
				any = true;
			}
		}
	}
	if (any) {
		for (Face &F : m.faces) {
			for (int i = F.size() - 1; i >= 0 && F.size() > 3; i--) {
				if (rm[F.v[i]]) {
					F.erase_corner(i);
				}
			}
		}
		finish(m, out);
	}
	return out;
}

int flip_edges(Mesh &m, const std::vector<Edge> &edges) {
	int count = 0;
	for (const Edge &E : edges) {
		Topology t;
		t.build(m);
		const int e = t.find_edge(E.a, E.b);
		if (e < 0 || t.edge_faces[e].size() != 2) {
			continue;
		}
		int f1 = t.face_of_half(E.a, E.b), f2 = t.face_of_half(E.b, E.a);
		if (f1 < 0 || f2 < 0 || m.faces[f1].size() != 3 || m.faces[f2].size() != 3) {
			continue;
		}
		Face &T1 = m.faces[f1];
		Face &T2 = m.faces[f2];
		T1.ensure_attrs();
		T2.ensure_attrs();
		int i = 0, j = 0;
		while (!(T1.v[i] == E.a && T1.v[(i + 1) % 3] == E.b)) {
			i++;
		}
		while (!(T2.v[j] == E.b && T2.v[(j + 1) % 3] == E.a)) {
			j++;
		}
		const std::vector<Corner> c1 = corners_of(T1), c2 = corners_of(T2);
		const Corner &a = c1[i], &b = c1[(i + 1) % 3], &c = c1[(i + 2) % 3];
		const Corner &b2 = c2[j], &a2 = c2[(j + 1) % 3], &d = c2[(j + 2) % 3];
		const Face attrs1 = T1, attrs2 = T2;
		m.faces[f1] = face_from({ c, a, d }, attrs1);
		m.faces[f2] = face_from({ d, b2, c }, attrs2);
		(void)b;
		(void)a2;
		count++;
	}
	return count;
}

// ================================================================== vertices

std::vector<int> collapse_groups(Mesh &m, const std::vector<std::vector<int>> &groups) {
	std::vector<int> map(m.pos.size());
	for (size_t i = 0; i < map.size(); i++) {
		map[i] = (int)i;
	}
	std::vector<int> out;
	for (const std::vector<int> &g0 : groups) {
		std::vector<int> g;
		for (int v : uniq(g0)) {
			if (v >= 0 && v < (int)m.pos.size()) {
				g.push_back(v);
			}
		}
		if (g.empty()) {
			continue;
		}
		Vector3 c;
		for (int v : g) {
			c += m.pos[v];
		}
		m.pos[g[0]] = c / (float)g.size();
		for (int v : g) {
			map[v] = g[0];
		}
		out.push_back(g[0]);
	}
	for (Face &F : m.faces) {
		for (int &v : F.v) {
			v = map[v];
		}
	}
	remap_vertex_attrs(m, map);
	cleanup(m);
	// a group whose faces all went (everything merged into one point): its point stays
	std::vector<char> used(m.pos.size(), 0);
	for (const Face &F : m.faces) {
		for (int v : F.v) {
			used[v] = 1;
		}
	}
	for (const std::pair<int, int> &w : m.wires) {
		used[w.first] = used[w.second] = 1;
	}
	for (int v : out) {
		if (v >= 0 && v < (int)used.size() && !used[v]) {
			m.points.push_back(v);
		}
	}
	clean_wires(m);
	return out;
}

std::vector<int> weld_vertices(Mesh &m, const std::vector<int> &verts, float dist) {
	std::vector<int> vs;
	for (int v : uniq(verts)) {
		if (v >= 0 && v < (int)m.pos.size()) {
			vs.push_back(v);
		}
	}
	std::vector<char> done(vs.size(), 0);
	std::vector<std::vector<int>> groups;
	const float d2 = dist * dist;
	// a grid of cells as big as the distance: only the 27 cells around a vertex are compared (dragging the
	// distance in the panel stays smooth on big meshes)
	const float cell = std::max(dist, 1e-6f);
	auto key = [&](int x, int y, int z) { return ((uint64_t)(uint32_t)x * 73856093ull) ^ ((uint64_t)(uint32_t)y * 19349663ull) ^ ((uint64_t)(uint32_t)z * 83492791ull); };
	std::unordered_map<uint64_t, std::vector<int>> grid; // (indices into vs, ascending)
	std::vector<std::array<int, 3>> cells(vs.size());
	for (size_t i = 0; i < vs.size(); i++) {
		const Vector3 p = m.pos[vs[i]] / cell;
		cells[i] = { (int)std::floor(p.x), (int)std::floor(p.y), (int)std::floor(p.z) };
		grid[key(cells[i][0], cells[i][1], cells[i][2])].push_back((int)i);
	}
	for (size_t i = 0; i < vs.size(); i++) {
		if (done[i]) {
			continue;
		}
		std::vector<int> near;
		for (int dx = -1; dx <= 1; dx++) {
			for (int dy = -1; dy <= 1; dy++) {
				for (int dz = -1; dz <= 1; dz++) {
					const auto it = grid.find(key(cells[i][0] + dx, cells[i][1] + dy, cells[i][2] + dz));
					if (it == grid.end()) {
						continue;
					}
					for (int j : it->second) {
						if (j > (int)i && !done[j] && m.pos[vs[i]].distance_squared_to(m.pos[vs[j]]) <= d2) {
							near.push_back(j);
						}
					}
				}
			}
		}
		std::sort(near.begin(), near.end()); // (the same groups as comparing them one by one in order)
		near.erase(std::unique(near.begin(), near.end()), near.end());
		std::vector<int> g = { vs[i] };
		done[i] = 1;
		for (int j : near) {
			g.push_back(vs[j]);
			done[j] = 1;
		}
		groups.push_back(g);
	}
	return collapse_groups(m, groups);
}

std::vector<int> split_vertices(Mesh &m, const std::vector<int> &verts) {
	std::vector<char> vm = mask_of(verts, m.pos.size());
	std::vector<char> first(m.pos.size(), 1);
	std::vector<int> out;
	for (Face &F : m.faces) {
		for (int &v : F.v) {
			if (!vm[v]) {
				continue;
			}
			if (first[v]) {
				first[v] = 0;
				out.push_back(v);
				continue;
			}
			const int nv = m.add_vertex(m.pos[v]);
			out.push_back(nv);
			v = nv;
		}
	}
	return out;
}

std::vector<Edge> connect_vertices(Mesh &m, const std::vector<int> &verts) {
	std::vector<char> vm = mask_of(verts, m.pos.size());
	std::vector<Edge> out;
	const int nf = (int)m.faces.size();
	for (int f = 0; f < nf; f++) {
		const Face F = m.faces[f];
		const std::vector<Corner> ring = corners_of(F);
		const int n = F.size();
		std::vector<int> idx;
		for (int i = 0; i < n; i++) {
			if (vm[F.v[i]]) {
				idx.push_back(i);
			}
		}
		if (idx.size() < 2) {
			continue;
		}
		if (idx.size() == 2) {
			const int d = idx[1] - idx[0];
			if (d == 1 || d == n - 1) {
				continue;
			}
		}
		Corner center;
		if (idx.size() > 2) {
			center.v = m.add_vertex(face_center(m, F));
			center.c = ring[0].c;
			for (int i : idx) {
				out.push_back(Edge(ring[i].v, center.v));
			}
		} else {
			out.push_back(Edge(ring[idx[0]].v, ring[idx[1]].v));
		}
		std::vector<std::vector<Corner>> pieces = split_ring(ring, idx, &center);
		for (size_t p = 0; p < pieces.size(); p++) {
			Face nf2 = face_from(pieces[p], F);
			if (p == 0) {
				m.faces[f] = nf2;
			} else {
				m.faces.push_back(nf2);
			}
		}
	}
	return out;
}

void dissolve_vertices(Mesh &m, const std::vector<int> &verts) {
	Topology t;
	t.build(m);
	std::vector<char> vm = mask_of(verts, m.pos.size());
	UnionFind uf((int)m.faces.size());
	std::vector<int> touched;
	for (int v = 0; v < (int)m.pos.size(); v++) {
		if (!vm[v] || t.vert_edges[v].size() <= 2) {
			continue;
		}
		const std::vector<int> &vf = t.vert_faces[v];
		for (size_t i = 1; i < vf.size(); i++) {
			uf.unite(vf[0], vf[i]);
		}
		touched.insert(touched.end(), vf.begin(), vf.end());
	}
	merge_groups(m, groups_of(uf, uniq(touched)));
	for (Face &F : m.faces) {
		for (int i = F.size() - 1; i >= 0 && F.size() > 3; i--) {
			if (F.v[i] < (int)vm.size() && vm[F.v[i]]) {
				F.erase_corner(i);
			}
		}
	}
	cleanup(m);
}

std::vector<int> fill_holes(Mesh &m, const std::vector<int> &verts) {
	std::vector<char> vm = mask_of(verts, m.pos.size());
	std::vector<int> out;
	for (const std::vector<int> &loop : border_loops(m)) {
		bool take = verts.empty();
		for (int v : loop) {
			take = take || vm[v];
		}
		if (!take) {
			continue;
		}
		out.push_back((int)m.faces.size());
		m.faces.push_back(make_face(loop));
	}
	return out;
}

std::vector<int> offset_vertices(Mesh &m, const std::vector<int> &verts, const Vector3 &delta) {
	std::vector<int> out;
	for (int v : uniq(verts)) {
		if (v >= 0 && v < (int)m.pos.size()) {
			m.pos[v] += delta;
			out.push_back(v);
		}
	}
	return out;
}

// ================================================================== object

void subdivide_smooth(Mesh &m) {
	subdivide_smooth(m, SubdivOptions());
}

namespace {
void subdivide_once(Mesh &m, const SubdivOptions &o);
}

void subdivide_smooth(Mesh &m, const SubdivOptions &o) {
	for (int l = 0; l < std::min(std::max(o.levels, 0), 6); l++) {
		subdivide_once(m, o);
	}
}

namespace {
void subdivide_once(Mesh &m, const SubdivOptions &o) {
	Topology t;
	t.build(m);
	const int nv = (int)m.pos.size();
	const int ne = (int)t.edges.size();
	const int nf = (int)m.faces.size();
	std::vector<Vector3> fp(nf), ep(ne), vp(nv);
	for (int f = 0; f < nf; f++) {
		fp[f] = face_center(m, m.faces[f]);
	}
	std::vector<char> vborder(nv, 0);
	std::vector<float> ecrease(ne, 0.0f);
	for (int e = 0; e < ne; e++) {
		const Edge &E = t.edges[e];
		const Vector3 mid = (m.pos[E.a] + m.pos[E.b]) * 0.5f;
		const EdgeAttr *ea = m.edge_attr(E.a, E.b);
		ecrease[e] = ea && o.creases ? clamp01(ea->crease) : 0.0f;
		if (o.simple) {
			ep[e] = mid;
			if (t.edge_faces[e].size() != 2) {
				vborder[E.a] = vborder[E.b] = 1;
			}
		} else if (t.edge_faces[e].size() == 2) {
			const Vector3 smooth = (m.pos[E.a] + m.pos[E.b] + fp[t.edge_faces[e][0]] + fp[t.edge_faces[e][1]]) * 0.25f;
			ep[e] = smooth.lerp(mid, ecrease[e]); // creased edges stay sharp
		} else {
			ep[e] = mid;
			vborder[E.a] = vborder[E.b] = 1;
		}
	}
	for (int v = 0; v < nv; v++) {
		const Vector3 &P = m.pos[v];
		const std::vector<int> &ve = t.vert_edges[v];
		if (ve.empty() || o.simple) {
			vp[v] = P;
			continue;
		}
		if (vborder[v]) {
			if (o.keep_corners && t.vert_faces[v].size() <= 1) {
				vp[v] = P; // (Keep Corners: a corner of the border stays)
				continue;
			}
			Vector3 s;
			int c = 0;
			for (int e : ve) {
				if (t.edge_faces[e].size() != 2) {
					const Edge &E = t.edges[e];
					s += m.pos[E.a == v ? E.b : E.a];
					c++;
				}
			}
			vp[v] = c == 2 ? (P * 6.0f + s) / 8.0f : P;
			continue;
		}
		const float n = (float)ve.size();
		Vector3 F, R;
		for (int f : t.vert_faces[v]) {
			F += fp[f];
		}
		F /= (float)t.vert_faces[v].size();
		for (int e : ve) {
			const Edge &E = t.edges[e];
			R += (m.pos[E.a] + m.pos[E.b]) * 0.5f;
		}
		R /= n;
		vp[v] = (F + R * 2.0f + P * (n - 3.0f)) / n;
		// Creases: two creased edges make a crease vertex, more make a corner (blended by the crease value).
		Vector3 cs;
		int cc = 0;
		float cw = 0.0f;
		for (int e : ve) {
			if (ecrease[e] > 0.0f) {
				const Edge &E = t.edges[e];
				cs += m.pos[E.a == v ? E.b : E.a];
				cc++;
				cw += ecrease[e];
			}
		}
		auto vc = m.vert_crease.find(v);
		const float vcrease = vc == m.vert_crease.end() || !o.creases ? 0.0f : clamp01(vc->second);
		if (cc == 2) {
			vp[v] = vp[v].lerp((P * 6.0f + cs) / 8.0f, clamp01(cw / 2.0f));
		} else if (cc > 2) {
			vp[v] = vp[v].lerp(P, clamp01(cw / (float)cc));
		}
		if (vcrease > 0.0f) {
			vp[v] = vp[v].lerp(P, vcrease);
		}
	}
	Mesh out;
	out.pos = vp;
	for (int e = 0; e < ne; e++) {
		out.pos.push_back(ep[e]);
		const EdgeAttr *ea = m.edge_attr(t.edges[e].a, t.edges[e].b);
		if (ea) {
			out.edge_attrs[ekey(t.edges[e].a, nv + e)] = *ea;
			out.edge_attrs[ekey(nv + e, t.edges[e].b)] = *ea;
		}
	}
	out.vert_crease = m.vert_crease;
	for (int f = 0; f < nf; f++) {
		out.pos.push_back(fp[f]);
	}
	// UVs (manual ones: the projected ones follow the new positions anyway): smoothed like the positions where
	// the texture runs on across the faces (one island), split linearly along its borders (Keep Boundaries)
	const bool uvs = o.uv_smooth > 0 && !o.simple;
	std::vector<Vector2> fuv(nf);
	for (int f = 0; f < nf; f++) {
		const Face &F = m.faces[f];
		Vector2 s;
		for (int i = 0; i < F.size() && i < (int)F.uv.size(); i++) {
			s += F.uv[i];
		}
		fuv[f] = s / (float)std::max(1, F.size());
	}
	auto uv_at = [&](int f, int v, Vector2 &r) {
		const Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			if (F.v[i] == v) {
				r = i < (int)F.uv.size() ? F.uv[i] : Vector2();
				return true;
			}
		}
		return false;
	};
	// an edge inside an island: two manual faces with the same UVs at both its ends
	std::vector<char> euv(ne, 0);
	std::vector<Vector2> euvp(ne);
	if (uvs) {
		for (int e = 0; e < ne; e++) {
			const std::vector<int> &ef = t.edge_faces[e];
			if (ef.size() != 2 || !m.faces[ef[0]].manual_uv || !m.faces[ef[1]].manual_uv || ecrease[e] > 0.0f) {
				continue;
			}
			Vector2 a0, b0, a1, b1;
			if (uv_at(ef[0], t.edges[e].a, a0) && uv_at(ef[0], t.edges[e].b, b0) && uv_at(ef[1], t.edges[e].a, a1) && uv_at(ef[1], t.edges[e].b, b1) &&
					a0.distance_to(a1) < 1e-5f && b0.distance_to(b1) < 1e-5f) {
				euv[e] = 1;
				euvp[e] = (a0 + b0 + fuv[ef[0]] + fuv[ef[1]]) * 0.25f;
			}
		}
	}
	// a vertex inside an island: all its edges inside it
	std::vector<char> vuv(nv, 0);
	std::vector<Vector2> vuvp(nv);
	if (uvs) {
		for (int v = 0; v < nv; v++) {
			const std::vector<int> &ve = t.vert_edges[v];
			if (ve.empty() || vborder[v]) {
				continue;
			}
			bool all = true;
			for (int e : ve) {
				all = all && euv[e];
			}
			Vector2 P;
			if (!all || !uv_at(t.vert_faces[v][0], v, P)) {
				continue;
			}
			Vector2 F, R;
			for (int f : t.vert_faces[v]) {
				F += fuv[f];
			}
			F /= (float)t.vert_faces[v].size();
			for (int e : ve) {
				Vector2 a, b;
				uv_at(t.edge_faces[e][0], t.edges[e].a, a);
				uv_at(t.edge_faces[e][0], t.edges[e].b, b);
				R += (a + b) * 0.5f;
			}
			const float n = (float)ve.size();
			R /= n;
			vuv[v] = 1;
			vuvp[v] = (F + R * 2.0f + P * (n - 3.0f)) / n;
		}
	}
	for (int f = 0; f < nf; f++) {
		const Face &F = m.faces[f];
		std::vector<Corner> cs = corners_of(F);
		const int n = F.size();
		Corner center;
		center.v = nv + ne + f;
		Vector2 uv;
		Color col(0, 0, 0, 0);
		for (const Corner &c : cs) {
			uv += c.uv;
			col += c.c;
		}
		center.uv = uv / (float)n;
		center.c = col / (float)n;
		const std::vector<Corner> orig = cs;
		if (uvs && F.manual_uv) {
			for (Corner &c : cs) {
				if (vuv[c.v]) {
					c.uv = vuvp[c.v];
				}
			}
		}
		for (int i = 0; i < n; i++) {
			const Corner &p = orig[(i + n - 1) % n], &c = orig[i], &q = orig[(i + 1) % n];
			const int e1 = t.find_edge(c.v, q.v), e0 = t.find_edge(p.v, c.v);
			Corner en = lerp_corner(c, q, 0.5f, nv + e1);
			Corner epv = lerp_corner(p, c, 0.5f, nv + e0);
			if (uvs && F.manual_uv) {
				if (e1 >= 0 && euv[e1]) {
					en.uv = euvp[e1];
				}
				if (e0 >= 0 && euv[e0]) {
					epv.uv = euvp[e0];
				}
			}
			out.faces.push_back(face_from({ cs[i], en, center, epv }, F));
		}
	}
	m = out;
}
} // namespace

void auto_smooth(Mesh &m, float angle_deg, const std::vector<int> &p_faces) {
	std::vector<int> faces = p_faces.empty() ? sel::all_faces(m) : valid_faces(m, p_faces);
	std::vector<char> fm = mask_of(faces, m.faces.size());
	Topology t;
	t.build(m);
	const float c = std::cos(angle_deg * (float)(godot::Math::PI / 180.0));
	std::vector<Vector3> n(m.faces.size());
	for (int f : faces) {
		n[f] = face_normal(m, m.faces[f]);
	}
	UnionFind uf((int)m.faces.size());
	for (size_t e = 0; e < t.edges.size(); e++) {
		const std::vector<int> &ef = t.edge_faces[e];
		if (ef.size() != 2 || !fm[ef[0]] || !fm[ef[1]]) {
			continue;
		}
		if (n[ef[0]].dot(n[ef[1]]) >= c - 1e-6f) {
			uf.unite(ef[0], ef[1]);
		}
	}
	int next_group = 1;
	if (!p_faces.empty()) {
		for (int f = 0; f < (int)m.faces.size(); f++) {
			if (!fm[f]) {
				next_group = std::max(next_group, m.faces[f].smooth + 1);
			}
		}
	}
	for (const std::vector<int> &g : groups_of(uf, faces)) {
		const int id = g.size() > 1 ? next_group++ : 0;
		for (int f : g) {
			m.faces[f].smooth = id;
		}
	}
}

void transform(Mesh &m, const Transform3D &xf) {
	for (Vector3 &p : m.pos) {
		p = xf.xform(p);
	}
	if (xf.basis.determinant() < 0.0f) {
		for (Face &f : m.faces) {
			f.reverse();
		}
	}
}

void append(Mesh &dst, const Mesh &src, const Transform3D &xf, int material_offset) {
	const int base = (int)dst.pos.size();
	for (const Vector3 &p : src.pos) {
		dst.pos.push_back(xf.xform(p));
	}
	const bool flip = xf.basis.determinant() < 0.0f;
	for (const Face &f : src.faces) {
		Face nf = f;
		for (int &v : nf.v) {
			v += base;
		}
		nf.material += material_offset;
		if (flip) {
			nf.reverse();
		}
		dst.faces.push_back(nf);
	}
}

int quadify(Mesh &m, float angle_deg, const std::vector<int> *only) {
	Topology t;
	t.build(m);
	std::vector<char> allowed = only ? mask_of(*only, m.faces.size()) : std::vector<char>(m.faces.size(), 1);
	const float c = std::cos(angle_deg * (float)(godot::Math::PI / 180.0));
	struct Cand {
		float score;
		int e;
	};
	std::vector<Cand> cands;
	for (size_t e = 0; e < t.edges.size(); e++) {
		const std::vector<int> &ef = t.edge_faces[e];
		if (ef.size() != 2 || m.faces[ef[0]].size() != 3 || m.faces[ef[1]].size() != 3 || !allowed[ef[0]] || !allowed[ef[1]]) {
			continue;
		}
		if (face_normal(m, m.faces[ef[0]]).dot(face_normal(m, m.faces[ef[1]])) < c - 1e-6f) {
			continue;
		}
		cands.push_back(Cand{ edge_length(m, t.edges[e]), (int)e });
	}
	std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.score > b.score; });
	std::vector<char> used(m.faces.size(), 0), del(m.faces.size(), 0);
	int count = 0;
	for (const Cand &cd : cands) {
		const Edge &E = t.edges[cd.e];
		const int f1 = t.face_of_half(E.a, E.b), f2 = t.face_of_half(E.b, E.a);
		if (f1 < 0 || f2 < 0 || f1 == f2 || used[f1] || used[f2]) {
			continue;
		}
		const std::vector<Corner> c1 = corners_of(m.faces[f1]), c2 = corners_of(m.faces[f2]);
		int i = 0, j = 0;
		while (i < 3 && !(c1[i].v == E.a && c1[(i + 1) % 3].v == E.b)) {
			i++;
		}
		while (j < 3 && !(c2[j].v == E.b && c2[(j + 1) % 3].v == E.a)) {
			j++;
		}
		if (i == 3 || j == 3) {
			continue;
		}
		const std::vector<Corner> q = { c1[i], c2[(j + 2) % 3], c1[(i + 1) % 3], c1[(i + 2) % 3] };
		// convexity check in the quad plane
		std::vector<Vector3> pts = { m.pos[q[0].v], m.pos[q[1].v], m.pos[q[2].v], m.pos[q[3].v] };
		const Vector3 n = newell(pts);
		bool convex = true;
		for (int k = 0; k < 4; k++) {
			const Vector3 a = pts[(k + 1) % 4] - pts[k], b = pts[(k + 2) % 4] - pts[(k + 1) % 4];
			if (a.cross(b).dot(n) <= 1e-9f) {
				convex = false;
				break;
			}
		}
		if (!convex) {
			continue;
		}
		const Face attrs = m.faces[f1];
		m.faces[f1] = face_from(q, attrs);
		used[f1] = used[f2] = 1;
		del[f2] = 1;
		count++;
	}
	delete_faces_mask(m, del);
	return count;
}

// ================================================================== normals / faces

void recalc_normals(Mesh &m, const std::vector<int> &p_faces, bool inside) {
	std::vector<int> faces = p_faces.empty() ? sel::all_faces(m) : valid_faces(m, p_faces);
	conform_normals(m, faces);
	Topology t;
	t.build(m);
	for (const std::vector<int> &comp : face_components(m, t, faces)) {
		// Signed volume relative to the component centroid (works for open patches too).
		Vector3 c;
		int count = 0;
		for (int f : comp) {
			for (int v : m.faces[f].v) {
				c += m.pos[v];
				count++;
			}
		}
		c /= (float)std::max(1, count);
		float vol = 0.0f;
		for (int f : comp) {
			const Face &F = m.faces[f];
			for (int i = 1; i + 1 < F.size(); i++) {
				vol += (m.pos[F.v[0]] - c).dot((m.pos[F.v[i]] - c).cross(m.pos[F.v[i + 1]] - c));
			}
		}
		if ((vol < 0.0f) != inside) {
			for (int f : comp) {
				m.faces[f].reverse();
			}
		}
	}
}

bool add_wire(Mesh &m, int a, int b) {
	if (a == b || a < 0 || b < 0 || a >= (int)m.pos.size() || b >= (int)m.pos.size() || m.has_wire(a, b)) {
		return false;
	}
	for (const Face &F : m.faces) {
		for (int i = 0; i < F.size(); i++) {
			if (ekey(F.v[i], F.v[(i + 1) % F.size()]) == ekey(a, b)) {
				return false; // already an edge
			}
		}
	}
	m.wires.push_back({ a, b });
	return true;
}

std::vector<int> extrude_wires(Mesh &m, const std::vector<Edge> &wire_edges, const std::vector<int> &lone_verts, std::vector<Edge> *r_top_edges) {
	std::unordered_map<int, int> dup;
	std::vector<int> out;
	auto top = [&](int v) {
		auto it = dup.find(v);
		if (it != dup.end()) {
			return it->second;
		}
		const int nv = m.add_vertex(m.pos[v]);
		dup[v] = nv;
		out.push_back(nv);
		return nv;
	};
	std::unordered_set<uint64_t> gone;
	for (const Edge &e : uniq(wire_edges)) {
		if (!m.has_wire(e.a, e.b)) {
			continue;
		}
		const int ta = top(e.a), tb = top(e.b);
		m.faces.push_back(make_face({ e.a, e.b, tb, ta }));
		gone.insert(ekey(e));
		if (r_top_edges) {
			r_top_edges->push_back(Edge(ta, tb));
		}
	}
	if (!gone.empty()) {
		std::vector<std::pair<int, int>> keep;
		for (const std::pair<int, int> &w : m.wires) {
			if (!gone.count(ekey(w.first, w.second))) {
				keep.push_back(w);
			}
		}
		m.wires.swap(keep);
	}
	for (int v : uniq(lone_verts)) {
		if (v < 0 || v >= (int)m.pos.size() || dup.count(v)) {
			continue;
		}
		const int t = top(v);
		m.wires.push_back({ v, t });
	}
	return out;
}

int make_face_from(Mesh &m, const std::vector<int> &p_verts) {
	std::vector<int> verts = uniq(p_verts);
	if (verts.size() < 3) {
		return -1;
	}
	std::unordered_set<int> vs(verts.begin(), verts.end());
	// A closed chain of loose / border edges through all the vertices (e.g. drawn with F on vertex pairs
	// and extruded vertices): follow it. Border edges decide the orientation (hole filling).
	{
		Topology t;
		t.build(m);
		std::unordered_map<int, std::vector<int>> adj;
		std::unordered_set<uint64_t> border_half; // existing face half-edges of border edges
		auto link = [&](int a, int b) {
			adj[a].push_back(b);
			adj[b].push_back(a);
		};
		for (const std::pair<int, int> &w : m.wires) {
			if (vs.count(w.first) && vs.count(w.second)) {
				link(w.first, w.second);
			}
		}
		for (size_t e = 0; e < t.edges.size(); e++) {
			const Edge &E = t.edges[e];
			if (t.edge_faces[e].size() == 1 && vs.count(E.a) && vs.count(E.b)) {
				link(E.a, E.b);
				border_half.insert(t.face_of_half(E.a, E.b) >= 0 ? hkey(E.a, E.b) : hkey(E.b, E.a));
			}
		}
		bool cycle = adj.size() == verts.size();
		for (auto &kv : adj) {
			cycle = cycle && kv.second.size() == 2;
		}
		if (cycle && !m.wires.empty()) {
			std::vector<int> ring = { verts[0] };
			int prev = -1, cur = verts[0];
			while ((int)ring.size() < (int)verts.size()) {
				const std::vector<int> &nb = adj[cur];
				const int nx = nb[0] != prev ? nb[0] : nb[1];
				if (nx == verts[0]) {
					break;
				}
				ring.push_back(nx);
				prev = cur;
				cur = nx;
			}
			if (ring.size() == verts.size()) {
				Face f = make_face(ring);
				bool decided = false;
				for (size_t i = 0; i < ring.size() && !decided; i++) {
					const int a = ring[i], b = ring[(i + 1) % ring.size()];
					if (border_half.count(hkey(a, b))) {
						f.reverse(); // the neighbor face already uses a->b
						decided = true;
					} else if (border_half.count(hkey(b, a))) {
						decided = true;
					}
				}
				if (!decided) {
					const std::vector<Vector3> vn = vertex_normals(m);
					Vector3 nsum;
					for (int v : ring) {
						nsum += vn[v];
					}
					if (nsum.length() > 1e-6f && face_normal_raw(m, f).dot(nsum) < 0.0f) {
						f.reverse();
					}
				}
				m.faces.push_back(f);
				clean_wires(m);
				return (int)m.faces.size() - 1;
			}
		}
	}
	// Selected vertices on one border loop: keep the loop order (and its hole-filling orientation).
	for (const std::vector<int> &loop : border_loops(m)) {
		std::vector<int> ring;
		for (int v : loop) {
			if (vs.count(v)) {
				ring.push_back(v);
			}
		}
		if (ring.size() == verts.size()) {
			m.faces.push_back(make_face(ring));
			return (int)m.faces.size() - 1;
		}
	}
	// Otherwise: sort around the centroid, facing along the average vertex normal.
	const std::vector<Vector3> vn = vertex_normals(m);
	Vector3 c, nrm;
	for (int v : verts) {
		c += m.pos[v];
		nrm += vn[v];
	}
	c /= (float)verts.size();
	std::vector<Vector3> pts;
	for (int v : verts) {
		pts.push_back(m.pos[v]);
	}
	if (nrm.length() < 1e-6f) {
		// best-fit plane through the points (Newell of the centroid fan in input order)
		for (size_t i = 0; i < pts.size(); i++) {
			nrm += (pts[i] - c).cross(pts[(i + 1) % pts.size()] - c);
		}
	}
	if (nrm.length() < 1e-9f) {
		return -1;
	}
	nrm.normalize();
	Vector3 u, w;
	plane_basis(nrm, u, w);
	std::vector<std::pair<float, int>> order;
	for (int v : verts) {
		const Vector3 d = m.pos[v] - c;
		order.push_back({ std::atan2(d.dot(w), d.dot(u)), v });
	}
	std::sort(order.begin(), order.end());
	std::vector<int> ring;
	for (auto &o : order) {
		ring.push_back(o.second);
	}
	Face f = make_face(ring);
	if (face_normal_raw(m, f).dot(nrm) < 0.0f) {
		f.reverse();
	}
	m.faces.push_back(f);
	return (int)m.faces.size() - 1;
}

// ================================================================== cutting

void insert_on_edge(Mesh &m, int a, int b, const std::vector<std::pair<float, int>> &p_points) {
	if (p_points.empty()) {
		return;
	}
	std::vector<std::pair<float, int>> pts = p_points;
	std::sort(pts.begin(), pts.end());
	for (Face &F : m.faces) {
		const int n = F.size();
		for (int i = 0; i < n; i++) {
			const int x = F.v[i], y = F.v[(i + 1) % n];
			if (!((x == a && y == b) || (x == b && y == a))) {
				continue;
			}
			const std::vector<Corner> cs = corners_of(F);
			const Corner ca = cs[i], cb = cs[(i + 1) % n];
			const bool fwd = x == a;
			int at = i + 1;
			for (size_t k = 0; k < pts.size(); k++) {
				const std::pair<float, int> &p = fwd ? pts[k] : pts[pts.size() - 1 - k];
				const float t = fwd ? p.first : 1.0f - p.first;
				const Corner c = lerp_corner(ca, cb, t, p.second);
				F.insert_corner(at++, c.v, c.uv, c.c);
			}
			break; // an edge appears at most once per face
		}
	}
}

int split_face_path(Mesh &m, int f, const std::vector<int> &path) {
	if (f < 0 || f >= (int)m.faces.size() || path.size() < 2) {
		return -1;
	}
	const Face F = m.faces[f];
	const int n = F.size();
	int ia = -1, ib = -1;
	for (int i = 0; i < n; i++) {
		if (F.v[i] == path.front()) {
			ia = i;
		}
		if (F.v[i] == path.back()) {
			ib = i;
		}
	}
	if (ia < 0 || ib < 0 || ia == ib) {
		return -1;
	}
	const bool adjacent = (ia + 1) % n == ib || (ib + 1) % n == ia;
	if (adjacent && path.size() == 2) {
		return -1;
	}
	const std::vector<Corner> ring = corners_of(F);
	// a point of the path inside the face: its UV and colors where it is in the face (not the face's average:
	// the face's own UVs would be torn there)
	auto inside = [&](int v) { return corner_by_weights(ring, polygon_weights(m, F, m.pos[v]), v); };
	std::vector<Corner> p1 = slice(ring, ia, ib), p2 = slice(ring, ib, ia);
	for (int k = (int)path.size() - 2; k >= 1; k--) {
		p1.push_back(inside(path[k]));
	}
	for (size_t k = 1; k + 1 < path.size(); k++) {
		p2.push_back(inside(path[k]));
	}
	Face a = face_from(p1, F), b = face_from(p2, F);
	if (!F.manual_uv && F.uvp.fill != UV_FILL_TILE && F.uv_group <= 0) {
		// (fitted / stretched over the face: the two pieces together, as the whole face was)
		int g = 0;
		for (const Face &x : m.faces) {
			g = std::max(g, x.uv_group);
		}
		a.uv_group = b.uv_group = g + 1;
	}
	m.faces[f] = a;
	m.faces.push_back(b);
	return (int)m.faces.size() - 1;
}

namespace {

// Point in face polygon (projected on the face plane).
bool point_in_face(const Mesh &m, const Face &F, const Vector3 &p) {
	const Vector3 nrm = face_normal(m, F);
	Vector3 u, w;
	plane_basis(nrm, u, w);
	bool inside = false;
	const int n = F.size();
	const Vector2 q(p.dot(u), p.dot(w));
	for (int i = 0, j = n - 1; i < n; j = i++) {
		const Vector2 a(m.pos[F.v[i]].dot(u), m.pos[F.v[i]].dot(w));
		const Vector2 b(m.pos[F.v[j]].dot(u), m.pos[F.v[j]].dot(w));
		if ((a.y > q.y) != (b.y > q.y) && q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x) {
			inside = !inside;
		}
	}
	return inside;
}

// Finds the face that has both `a` and `b` as corners and contains `probe`.
// The face holding vertices a and b whose plane holds `probe` (and that contains it, when several do).
// Two faces share the vertices of a common edge; the plane distance tells the right one.
int face_with(const Mesh &m, int a, int b, const Vector3 &probe) {
	int best = -1;
	float best_d = 1e30f;
	bool best_in = false;
	for (int f = 0; f < (int)m.faces.size(); f++) {
		const Face &F = m.faces[f];
		if (std::find(F.v.begin(), F.v.end(), a) == F.v.end() || std::find(F.v.begin(), F.v.end(), b) == F.v.end()) {
			continue;
		}
		const float d = std::fabs(face_normal(m, F).dot(probe - m.pos[a]));
		const bool in = point_in_face(m, F, probe);
		float ext = 0.0f;
		for (int v : F.v) {
			ext = std::max(ext, m.pos[v].distance_to(m.pos[a]));
		}
		const float tol = std::max(ext, 1e-6f) * 1e-3f;
		// in-plane faces first, then containment, then the nearest plane
		const bool better = best < 0 || (d <= tol && best_d > tol) || ((d <= tol) == (best_d <= tol) && in && !best_in) ||
				((d <= tol) == (best_d <= tol) && in == best_in && d < best_d);
		if (better) {
			best = f;
			best_d = d;
			best_in = in;
		}
	}
	return best;
}

struct RingWalk {
	std::vector<std::pair<int, int>> edges; // oriented
	std::vector<int> faces; // faces between consecutive edges
	bool closed = false;
};

RingWalk walk_ring(const Mesh &m, const Edge &start) {
	Topology t;
	t.build(m);
	RingWalk r;
	const int e0 = t.find_edge(start.a, start.b);
	if (e0 < 0) {
		return r;
	}
	std::deque<std::pair<int, int>> edges = { { start.a, start.b } };
	std::deque<int> faces;
	std::unordered_set<int> visited;
	for (int dir = 0; dir < 2 && !r.closed; dir++) {
		const std::vector<int> &ef = t.edge_faces[e0];
		if ((int)ef.size() <= dir) {
			break;
		}
		int f = ef[dir];
		int p = start.a, q = start.b;
		while (f >= 0 && !visited.count(f)) {
			const Face &F = m.faces[f];
			if (F.size() != 4) {
				break;
			}
			int i = -1;
			for (int k = 0; k < 4; k++) {
				if (F.v[k] == p) {
					i = k;
				}
			}
			if (i < 0) {
				break;
			}
			int k2, l2;
			if (F.v[(i + 1) % 4] == q) {
				k2 = F.v[(i + 3) % 4];
				l2 = F.v[(i + 2) % 4];
			} else if (F.v[(i + 3) % 4] == q) {
				k2 = F.v[(i + 1) % 4];
				l2 = F.v[(i + 2) % 4];
			} else {
				break;
			}
			visited.insert(f);
			if (dir == 0) {
				faces.push_back(f);
			} else {
				faces.push_front(f);
			}
			if (Edge(k2, l2) == start) {
				r.closed = true;
				break;
			}
			if (dir == 0) {
				edges.push_back({ k2, l2 });
			} else {
				edges.push_front({ k2, l2 });
			}
			const int e = t.find_edge(k2, l2);
			int nf = -1;
			for (int o : t.edge_faces[e]) {
				if (o != f) {
					nf = o;
				}
			}
			f = nf;
			p = k2;
			q = l2;
		}
	}
	r.edges.assign(edges.begin(), edges.end());
	r.faces.assign(faces.begin(), faces.end());
	return r;
}

} // namespace

std::vector<std::pair<int, int>> loop_cut_ring(const Mesh &m, const Edge &start) {
	return walk_ring(m, start).edges;
}

std::vector<Edge> loop_cut(Mesh &m, const Edge &p_start, int cuts, float t) {
	cuts = std::max(1, cuts);
	// Orientation is defined by p_start as given (a -> b), not the sorted Edge.
	const int sa = p_start.a, sb = p_start.b;
	RingWalk r = walk_ring(m, p_start);
	std::vector<Edge> out;
	if (r.faces.empty()) {
		return out;
	}
	// Re-orient so that the first vertex matches the side of start.a.
	bool flip = false;
	for (const std::pair<int, int> &e : r.edges) {
		if (Edge(e.first, e.second) == Edge(sa, sb)) {
			flip = e.first != sa;
			break;
		}
	}
	std::vector<std::vector<int>> cut_verts(r.edges.size());
	for (size_t i = 0; i < r.edges.size(); i++) {
		const int p = flip ? r.edges[i].second : r.edges[i].first;
		const int q = flip ? r.edges[i].first : r.edges[i].second;
		std::vector<std::pair<float, int>> pts;
		for (int c = 0; c < cuts; c++) {
			// One cut sits at t; several are spread evenly and slid together by (t - 0.5).
			const float base = (float)(c + 1) / (cuts + 1);
			const float f = cuts == 1 ? t : base + (t - 0.5f) * 2.0f * 0.98f / (cuts + 1);
			const int v = m.add_vertex(m.pos[p].lerp(m.pos[q], f));
			cut_verts[i].push_back(v);
			pts.push_back({ f, v });
		}
		insert_on_edge(m, p, q, pts);
	}
	const size_t ne = r.edges.size();
	const size_t pairs = r.closed ? ne : ne - 1;
	for (size_t i = 0; i < pairs; i++) {
		const std::vector<int> &A = cut_verts[i], &B = cut_verts[(i + 1) % ne];
		for (int c = 0; c < cuts; c++) {
			const Vector3 mid = (m.pos[A[c]] + m.pos[B[c]]) * 0.5f;
			const int f = face_with(m, A[c], B[c], mid);
			if (f >= 0 && split_face_path(m, f, { A[c], B[c] }) >= 0) {
				out.push_back(Edge(A[c], B[c]));
			}
		}
	}
	cleanup(m);
	return out;
}

// ------------------------------------------------------------------ knife

namespace {

float face_scale(const Mesh &m, const Face &F) {
	float s = 0.0f;
	for (int i = 0; i < F.size(); i++) {
		s = std::max(s, m.pos[F.v[i]].distance_to(m.pos[F.v[(i + 1) % F.size()]]));
	}
	return std::max(s, 1e-6f);
}

KnifeHit classify(const Mesh &m, int f, const Vector3 &p) {
	KnifeHit h;
	h.pos = p;
	h.face = f;
	if (f < 0 || f >= (int)m.faces.size()) {
		return h;
	}
	const Face &F = m.faces[f];
	const float eps = face_scale(m, F) * 1e-3f;
	for (int v : F.v) {
		if (m.pos[v].distance_to(p) <= eps) {
			h.vert = v;
			h.face = -1;
			h.pos = m.pos[v];
			return h;
		}
	}
	for (int i = 0; i < F.size(); i++) {
		const Vector3 a = m.pos[F.v[i]], b = m.pos[F.v[(i + 1) % F.size()]];
		const Vector3 ab = b - a;
		const float l2 = ab.length_squared();
		if (l2 < 1e-18f) {
			continue;
		}
		const float t = std::max(0.0f, std::min(1.0f, (p - a).dot(ab) / l2));
		if ((a + ab * t).distance_to(p) <= eps) {
			h.ea = F.v[i];
			h.eb = F.v[(i + 1) % F.size()];
			h.t = t;
			h.face = -1;
			h.pos = a + ab * t;
			return h;
		}
	}
	return h;
}

std::vector<int> faces_of_hit(const Topology &t, const KnifeHit &h) {
	if (h.vert >= 0) {
		return t.vert_faces[h.vert];
	}
	if (h.ea >= 0) {
		const int e = t.find_edge(h.ea, h.eb);
		return e >= 0 ? t.edge_faces[e] : std::vector<int>();
	}
	return h.face >= 0 ? std::vector<int>{ h.face } : std::vector<int>();
}

bool same_hit(const KnifeHit &a, const KnifeHit &b) {
	if (a.vert >= 0 || b.vert >= 0) {
		return a.vert == b.vert;
	}
	if (a.ea >= 0 && b.ea >= 0) {
		return Edge(a.ea, a.eb) == Edge(b.ea, b.eb) && a.pos.distance_to(b.pos) < 1e-6f;
	}
	return a.face == b.face && a.pos.distance_to(b.pos) < 1e-6f;
}

struct Walker {
	const Mesh &m;
	const Topology &t;
	Vector3 n;
	KnifeHit target;
	std::vector<int> target_faces;
	std::unordered_set<int> visited;
	std::vector<KnifeHit> path;
	int steps = 0;
	bool strict = true; // only move towards the target; relaxed when the plane runs sideways

	bool walk(int f, const KnifeHit &cur) {
		if (++steps > 4096) {
			return false;
		}
		if (std::find(target_faces.begin(), target_faces.end(), f) != target_faces.end()) {
			path.push_back(target);
			return true;
		}
		const Face &F = m.faces[f];
		const int fn = F.size();
		const float eps = face_scale(m, F) * 1e-4f;
		std::vector<float> s(fn);
		for (int i = 0; i < fn; i++) {
			s[i] = n.dot(m.pos[F.v[i]] - cur.pos);
		}
		const Vector3 ahead = target.pos - cur.pos;
		struct Cand {
			KnifeHit h;
			float d;
		};
		std::vector<Cand> cands;
		for (int i = 0; i < fn; i++) {
			const int j = (i + 1) % fn;
			KnifeHit h;
			if (std::fabs(s[i]) <= eps) {
				h.vert = F.v[i];
				h.pos = m.pos[F.v[i]];
			} else if (std::fabs(s[j]) > eps && (s[i] > 0) != (s[j] > 0)) {
				const float tt = s[i] / (s[i] - s[j]);
				h.ea = F.v[i];
				h.eb = F.v[j];
				h.t = tt;
				h.pos = m.pos[F.v[i]].lerp(m.pos[F.v[j]], tt);
			} else {
				continue;
			}
			if (same_hit(h, cur) || (cur.vert >= 0 && h.ea >= 0 && (h.ea == cur.vert || h.eb == cur.vert) && h.pos.distance_to(cur.pos) <= eps)) {
				continue;
			}
			if (cur.ea >= 0 && h.ea >= 0 && Edge(h.ea, h.eb) == Edge(cur.ea, cur.eb)) {
				continue;
			}
			if (cur.ea >= 0 && h.vert >= 0 && (h.vert == cur.ea || h.vert == cur.eb) && h.pos.distance_to(cur.pos) <= eps) {
				continue;
			}
			const float fwd = (h.pos - cur.pos).dot(ahead);
			if (strict ? fwd <= 0.0f : fwd < -eps * ahead.length()) {
				continue;
			}
			cands.push_back({ h, h.pos.distance_to(cur.pos) });
		}
		std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.d < b.d; });
		for (const Cand &c : cands) {
			path.push_back(c.h);
			for (int nf : faces_of_hit(t, c.h)) {
				if (nf == f || visited.count(nf)) {
					continue;
				}
				visited.insert(nf);
				if (walk(nf, c.h)) {
					return true;
				}
			}
			path.pop_back();
		}
		return false;
	}
};

} // namespace

namespace {
// The open edges (less than two faces, wires too) around each vertex, with the faces they have.
struct OpenEdges {
	std::unordered_map<uint64_t, std::vector<int>> faces_of; // every edge -> its faces
	std::unordered_map<int, std::vector<int>> nb; // vertex -> the other ends of its open edges
	void build(const Mesh &m) {
		for (int f = 0; f < (int)m.faces.size(); f++) {
			const Face &F = m.faces[f];
			for (int i = 0; i < F.size(); i++) {
				faces_of[ekey(F.v[i], F.v[(i + 1) % F.size()])].push_back(f);
			}
		}
		for (const std::pair<int, int> &w : m.wires) {
			faces_of[ekey(w.first, w.second)]; // (no faces)
		}
		for (const auto &kv : faces_of) {
			if (kv.second.size() < 2) {
				const int x = (int)(kv.first >> 32), y = (int)(kv.first & 0xffffffffu);
				nb[x].push_back(y);
				nb[y].push_back(x);
			}
		}
		for (auto &kv : nb) {
			std::sort(kv.second.begin(), kv.second.end()); // (the same choice every time)
		}
	}
	const std::vector<int> &faces(int a, int b) const {
		static const std::vector<int> none;
		auto it = faces_of.find(ekey(a, b));
		return it == faces_of.end() ? none : it->second;
	}
	bool exists(int a, int b) const { return faces_of.count(ekey(a, b)) > 0; }
	std::vector<int> open_of(int v) const {
		auto it = nb.find(v);
		return it == nb.end() ? std::vector<int>() : it->second;
	}
};

bool has_directed(const Face &F, int a, int b) {
	for (int i = 0; i < F.size(); i++) {
		if (F.v[i] == a && F.v[(i + 1) % F.size()] == b) {
			return true;
		}
	}
	return false;
}

// The new face turned like its neighbors: across each of its edges a face goes the other way. With no
// neighbors it looks at the eye.
void orient_fill(const Mesh &m, const OpenEdges &oe, std::vector<int> &v, const F2View &view) {
	const int n = (int)v.size();
	int votes = 0;
	for (int i = 0; i < n; i++) {
		const int a = v[i], b = v[(i + 1) % n];
		for (int f : oe.faces(a, b)) {
			votes += has_directed(m.faces[f], a, b) ? -1 : 1;
		}
	}
	bool flip = votes < 0;
	if (votes == 0 && view.has_eye) {
		std::vector<Vector3> pts;
		Vector3 c;
		for (int x : v) {
			pts.push_back(m.pos[x]);
			c += m.pos[x];
		}
		c /= (float)n;
		flip = newell(pts).dot(view.eye - c) < 0.0f;
	}
	if (flip) {
		std::reverse(v.begin(), v.end());
	}
}

bool fill_degenerate(const Mesh &m, const std::vector<int> &p_verts) {
	std::vector<Vector3> pts;
	float len = 0.0f;
	for (int i = 0; i < (int)p_verts.size(); i++) {
		pts.push_back(m.pos[p_verts[i]]);
		len = std::max(len, (m.pos[p_verts[i]] - m.pos[p_verts[(i + 1) % p_verts.size()]]).length());
	}
	return newell(pts).length() <= len * len * 1e-3f;
}

// a face with all these vertices already there
bool fill_exists(const Mesh &m, const std::vector<int> &p_verts) {
	for (const Face &F : m.faces) {
		if (F.size() != (int)p_verts.size()) {
			continue;
		}
		bool all = true;
		for (int v : p_verts) {
			all = all && std::find(F.v.begin(), F.v.end(), v) != F.v.end();
		}
		if (all) {
			return true;
		}
	}
	return false;
}

int add_fill_face(Mesh &m, const std::vector<int> &p_verts, int p_like) {
	Face nf;
	if (p_like >= 0) {
		nf.copy_attrs_from(m.faces[p_like]);
	}
	nf.v = p_verts;
	nf.manual_uv = false; // (its own UVs would not fit: projected like its neighbor)
	nf.ensure_attrs();
	m.faces.push_back(nf);
	return (int)m.faces.size() - 1;
}

float f2_dist(const F2View &view, const Vector3 &p) {
	return view.to_screen ? (view.to_screen(p) - view.mouse).length() : 0.0f;
}

int first_face(const OpenEdges &oe, const std::vector<int> &v) {
	for (int i = 0; i < (int)v.size(); i++) {
		const std::vector<int> &fs = oe.faces(v[i], v[(i + 1) % v.size()]);
		if (!fs.empty()) {
			return fs[0];
		}
	}
	return -1;
}
} // namespace

int f2_fill_edge(Mesh &m, int a, int b, const F2View &view, Edge *r_next) {
	if (a < 0 || b < 0 || a >= (int)m.pos.size() || b >= (int)m.pos.size() || a == b) {
		return -1;
	}
	OpenEdges oe;
	oe.build(m);
	if (!oe.exists(a, b)) {
		return -1;
	}
	const std::vector<int> own = oe.faces(a, b);
	// at each end: the open edge (not of the edge's own face) whose other end is nearest to the mouse
	auto pick = [&](int end, int other) {
		int best = -1;
		float bd = 1e30f;
		for (int x : oe.open_of(end)) {
			if (x == other) {
				continue;
			}
			bool shares = false;
			for (int f : oe.faces(end, x)) {
				shares = shares || std::find(own.begin(), own.end(), f) != own.end();
			}
			const float d = f2_dist(view, m.pos[x]);
			if (!shares && d < bd) {
				bd = d;
				best = x;
			}
		}
		return best;
	};
	const int x = pick(a, b), y = pick(b, a);
	std::vector<int> verts;
	if (x >= 0 && y >= 0 && x != y) {
		verts = { x, a, b, y };
		if (fill_degenerate(m, verts) || fill_exists(m, verts)) {
			verts.clear();
		}
	}
	if (verts.empty()) {
		// a triangle: they meet, or only one end goes on
		for (int z : { x == y ? x : -1, x, y }) {
			if (z >= 0 && !fill_degenerate(m, { z, a, b }) && !fill_exists(m, { z, a, b })) {
				verts = { z, a, b };
				break;
			}
		}
	}
	if (verts.empty()) {
		return -1;
	}
	const bool quad = verts.size() == 4;
	orient_fill(m, oe, verts, view);
	const int f = add_fill_face(m, verts, first_face(oe, verts));
	if (r_next) {
		*r_next = quad ? Edge(x, y) : Edge(-1, -1);
	}
	return f;
}

int f2_fill_vertex(Mesh &m, int v, const F2View &view, int *r_new) {
	if (r_new) {
		*r_new = -1;
	}
	if (v < 0 || v >= (int)m.pos.size()) {
		return -1;
	}
	OpenEdges oe;
	oe.build(m);
	const std::vector<int> open = oe.open_of(v);
	// of every two open edges: the quad's new corner (or the triangle's middle when their ends are joined)
	float bd = 1e30f;
	int bi = -1, bj = -1;
	bool tri = false;
	Vector3 bq;
	for (int i = 0; i < (int)open.size(); i++) {
		for (int j = i + 1; j < (int)open.size(); j++) {
			const int p = open[i], n = open[j];
			if (p == n) {
				continue;
			}
			// two edges of one face: that face is there already
			bool same_face = false;
			const std::vector<int> &g = oe.faces(v, n);
			for (int f : oe.faces(v, p)) {
				same_face = same_face || std::find(g.begin(), g.end(), f) != g.end();
			}
			if (same_face) {
				continue;
			}
			const bool joined = oe.exists(p, n);
			if (fill_degenerate(m, { p, v, n }) || (joined && fill_exists(m, { p, v, n }))) {
				continue;
			}
			const Vector3 q = joined ? (m.pos[p] + m.pos[n] + m.pos[v]) / 3.0f : m.pos[p] + m.pos[n] - m.pos[v];
			const float d = f2_dist(view, q);
			if (d < bd) {
				bd = d;
				bi = p;
				bj = n;
				tri = joined;
				bq = q;
			}
		}
	}
	if (bi < 0) {
		return -1;
	}
	std::vector<int> verts;
	int q = -1;
	if (tri) {
		verts = { bi, v, bj };
	} else {
		q = m.add_vertex(bq);
		verts = { bi, v, bj, q };
	}
	orient_fill(m, oe, verts, view);
	if (r_new) {
		*r_new = q;
	}
	return add_fill_face(m, verts, first_face(oe, verts));
}

int face_at_point(const Mesh &m, const Vector3 &p, int hint) {
	auto fits = [&](int f, float *r_d) {
		const Face &F = m.faces[f];
		const float eps = face_scale(m, F) * 1e-3f;
		const float d = std::fabs(face_normal(m, F).dot(p - m.pos[F.v[0]]));
		if (d > eps) {
			return false;
		}
		*r_d = d;
		const KnifeHit h = classify(m, f, p);
		return h.vert >= 0 || h.ea >= 0 || point_in_face(m, F, p);
	};
	float d = 0.0f;
	if (hint >= 0 && hint < (int)m.faces.size() && fits(hint, &d)) {
		return hint;
	}
	int best = -1;
	float bd = 1e30f;
	for (int f = 0; f < (int)m.faces.size(); f++) {
		if (fits(f, &d) && d < bd) {
			bd = d;
			best = f;
		}
	}
	return best;
}

bool knife_path(const Mesh &m, const std::vector<KnifePoint> &pts, const std::vector<Vector3> &cut_normals, std::vector<KnifeHit> &out) {
	out.clear();
	if (pts.empty()) {
		return true;
	}
	Topology t;
	t.build(m);
	std::vector<KnifeHit> hits;
	for (const KnifePoint &p : pts) {
		hits.push_back(classify(m, p.face, p.pos));
	}
	out.push_back(hits[0]);
	bool ok = true;
	for (size_t i = 0; i + 1 < hits.size(); i++) {
		const KnifeHit &a = hits[i], &b = hits[i + 1];
		if (same_hit(a, b)) {
			continue;
		}
		Walker w{ m, t };
		w.n = i < cut_normals.size() ? cut_normals[i].normalized() : Vector3();
		w.target = b;
		w.target_faces = faces_of_hit(t, b);
		// Shortest walk over the start faces; sideways steps only if no direct walk exists.
		std::vector<KnifeHit> best;
		float best_len = 1e30f;
		for (int strict = 1; strict >= 0 && best.empty(); strict--) {
			w.strict = strict != 0;
			for (int f : faces_of_hit(t, a)) {
				w.visited = { f };
				w.path.clear();
				w.steps = 0;
				if (!w.walk(f, a)) {
					continue;
				}
				float len = 0.0f;
				Vector3 prev = a.pos;
				for (const KnifeHit &h : w.path) {
					len += prev.distance_to(h.pos);
					prev = h.pos;
				}
				if (len < best_len) {
					best_len = len;
					best = w.path;
				}
			}
		}
		if (best.empty()) {
			ok = false;
			out.push_back(b); // keep the point; the segment is not cut
			continue;
		}
		for (const KnifeHit &h : best) {
			if (!same_hit(h, out.back())) {
				out.push_back(h);
			}
		}
	}
	return ok;
}

namespace {
std::vector<Edge> knife_along_impl(Mesh &m, const std::vector<KnifeHit> &path);

std::vector<Edge> knife_run(Mesh &m, const std::vector<KnifePoint> &pts, const std::vector<Vector3> &cut_normals) {
	std::vector<KnifeHit> path;
	knife_path(m, pts, cut_normals, path);
	return knife_net(m, { path });
}

std::vector<Edge> knife_along_impl(Mesh &m, const std::vector<KnifeHit> &path) {
	std::vector<Edge> out;
	if (path.size() < 2) {
		return out;
	}
	// New vertices on edges, shared between hits on the same spot.
	std::vector<int> ids(path.size(), -1);
	std::map<uint64_t, std::vector<std::pair<float, int>>> on_edge; // t from the smaller vertex id
	for (size_t i = 0; i < path.size(); i++) {
		const KnifeHit &h = path[i];
		if (h.vert >= 0) {
			ids[i] = h.vert;
		} else if (h.ea >= 0) {
			const float t = h.ea < h.eb ? h.t : 1.0f - h.t;
			std::vector<std::pair<float, int>> &list = on_edge[ekey(h.ea, h.eb)];
			for (const std::pair<float, int> &e : list) {
				if (std::fabs(e.first - t) < 1e-5f) {
					ids[i] = e.second;
				}
			}
			if (ids[i] < 0) {
				ids[i] = m.add_vertex(h.pos);
				list.push_back({ t, ids[i] });
			}
		} else {
			ids[i] = m.add_vertex(h.pos);
		}
	}
	for (auto &kv : on_edge) {
		insert_on_edge(m, (int)(kv.first >> 32), (int)(kv.first & 0xffffffffu), kv.second);
	}
	// Split faces between consecutive boundary hits.
	std::vector<size_t> boundary;
	for (size_t i = 0; i < path.size(); i++) {
		if (path[i].face < 0) {
			boundary.push_back(i);
		}
	}
	// A cut that ends inside a face (no vertex / edge there) is joined to the nearest corner of that face,
	// so the face can still be split.
	auto nearest_corner = [&](int f, const Vector3 &p, int avoid_a, int avoid_b) {
		int best = -1;
		float bd = 1e30f;
		for (int v : m.faces[f].v) {
			if (v == avoid_a || v == avoid_b) {
				continue;
			}
			const float d = m.pos[v].distance_squared_to(p);
			if (d < bd) {
				bd = d;
				best = v;
			}
		}
		return best;
	};
	auto split_open = [&](int f, std::vector<int> chain) {
		if (f < 0 || f >= (int)m.faces.size() || chain.size() < 3) {
			return;
		}
		if (split_face_path(m, f, chain) >= 0) {
			for (size_t x = 1; x < chain.size(); x++) {
				out.push_back(Edge(chain[x - 1], chain[x]));
			}
		}
	};
	if (boundary.empty()) {
		// every point inside one face: corner -> points -> another corner
		const int f = path.front().face;
		bool one_face = f >= 0;
		for (const KnifeHit &h : path) {
			one_face = one_face && h.face == f;
		}
		if (one_face) {
			const int va = nearest_corner(f, path.front().pos, -1, -1);
			const int vb = nearest_corner(f, path.back().pos, va, -1);
			if (va >= 0 && vb >= 0) {
				std::vector<int> chain = { va };
				for (size_t i = 0; i < path.size(); i++) {
					if (chain.back() != ids[i]) {
						chain.push_back(ids[i]);
					}
				}
				chain.push_back(vb);
				split_open(f, chain);
			}
		}
	} else {
		const size_t b0 = boundary.front(), b1 = boundary.back();
		if (b0 > 0 && path[0].face >= 0) {
			const int f = path[0].face;
			const int end = ids[b0];
			const int v = nearest_corner(f, path[0].pos, end, -1);
			if (v >= 0 && std::find(m.faces[f].v.begin(), m.faces[f].v.end(), end) != m.faces[f].v.end()) {
				std::vector<int> chain = { v };
				for (size_t i = 0; i <= b0; i++) {
					if (chain.back() != ids[i]) {
						chain.push_back(ids[i]);
					}
				}
				split_open(f, chain);
			}
		}
		if (b1 + 1 < path.size() && path.back().face >= 0) {
			const int start = ids[b1];
			// the face may have been split above: find the piece holding the boundary vertex and the point
			const int f = face_with(m, start, start, path.back().pos);
			if (f >= 0) {
				const int v = nearest_corner(f, path.back().pos, start, -1);
				if (v >= 0) {
					std::vector<int> chain;
					for (size_t i = b1; i < path.size(); i++) {
						if (chain.empty() || chain.back() != ids[i]) {
							chain.push_back(ids[i]);
						}
					}
					chain.push_back(v);
					split_open(f, chain);
				}
			}
		}
	}
	for (size_t k = 0; k + 1 < boundary.size(); k++) {
		const size_t i = boundary[k], j = boundary[k + 1];
		std::vector<int> chain;
		for (size_t x = i; x <= j; x++) {
			if (chain.empty() || chain.back() != ids[x]) {
				chain.push_back(ids[x]);
			}
		}
		if (chain.size() < 2) {
			continue;
		}
		const Vector3 probe = chain.size() > 2 ? m.pos[chain[1]] : (m.pos[chain.front()] + m.pos[chain.back()]) * 0.5f;
		const int f = face_with(m, chain.front(), chain.back(), probe);
		if (f < 0 || split_face_path(m, f, chain) < 0) {
			continue;
		}
		for (size_t x = 1; x < chain.size(); x++) {
			out.push_back(Edge(chain[x - 1], chain[x]));
		}
	}
	cleanup(m);
	return uniq(out);
}
} // namespace

KnifeHit knife_classify(const Mesh &m, int face, const Vector3 &p) {
	return classify(m, face, p);
}

namespace {

// Face `f` split by the cuts (vertex pairs, in its plane): the cells of the graph of its outline and the cuts.
void split_by_net(Mesh &m, int f, std::vector<std::pair<int, int>> cuts, const Face &orig, std::vector<Edge> &out) {
	Face F = m.faces[f];
	F.ensure_attrs();
	const Vector3 n = face_normal(m, F);
	Vector3 U, V;
	plane_basis(n, U, V);
	// (a frame in which the outline runs counter-clockwise)
	{
		float a = 0.0f;
		for (int i = 0; i < F.size(); i++) {
			const Vector3 &p = m.pos[F.v[i]], &q = m.pos[F.v[(i + 1) % F.size()]];
			a += p.dot(U) * q.dot(V) - q.dot(U) * p.dot(V);
		}
		if (a < 0.0f) {
			V = -V;
		}
	}
	auto P2 = [&](int v) { return Vector2(m.pos[v].dot(U), m.pos[v].dot(V)); };
	float size = 0.0f;
	for (int v : F.v) {
		size = std::max(size, (P2(v) - P2(F.v[0])).length());
	}
	const float eps = std::max(1e-7f, size * 1e-5f);
	std::set<int> bset(F.v.begin(), F.v.end());
	auto norm = [](std::pair<int, int> e) { return e.first < e.second ? e : std::make_pair(e.second, e.first); };
	{
		std::set<std::pair<int, int>> seen;
		std::vector<std::pair<int, int>> uniq_cuts;
		for (const auto &e : cuts) {
			if (e.first != e.second && seen.insert(norm(e)).second) {
				uniq_cuts.push_back(e);
			}
		}
		cuts = uniq_cuts;
	}
	// the boundary edges (no cut along one of them)
	std::set<std::pair<int, int>> bedges;
	for (int i = 0; i < F.size(); i++) {
		bedges.insert(norm({ F.v[i], F.v[(i + 1) % F.size()] }));
	}
	cuts.erase(std::remove_if(cuts.begin(), cuts.end(), [&](const std::pair<int, int> &e) { return bedges.count(norm(e)) > 0; }), cuts.end());
	if (cuts.empty()) {
		return;
	}
	// cuts split where they cross each other, and where a point lies on one
	for (int guard = 0; guard < 4000; guard++) {
		bool changed = false;
		std::set<int> pts(bset);
		for (const auto &e : cuts) {
			pts.insert(e.first);
			pts.insert(e.second);
		}
		for (size_t i = 0; i < cuts.size() && !changed; i++) {
			const Vector2 a = P2(cuts[i].first), b = P2(cuts[i].second);
			const Vector2 ab = b - a;
			const float L2 = ab.length_squared();
			if (L2 < eps * eps) {
				continue;
			}
			for (int p : pts) {
				if (p == cuts[i].first || p == cuts[i].second) {
					continue;
				}
				const float s = (P2(p) - a).dot(ab) / L2;
				if (s > 1e-4f && s < 1.0f - 1e-4f && std::fabs(ab.cross(P2(p) - a)) / std::sqrt(L2) < eps) {
					const std::pair<int, int> e = cuts[i];
					cuts[i] = { e.first, p };
					cuts.push_back({ p, e.second });
					changed = true;
					break;
				}
			}
			for (size_t j = i + 1; j < cuts.size() && !changed; j++) {
				const auto &c = cuts[j];
				if (c.first == cuts[i].first || c.first == cuts[i].second || c.second == cuts[i].first || c.second == cuts[i].second) {
					continue;
				}
				const Vector2 c0 = P2(c.first), cd = P2(c.second) - c0;
				const float den = ab.cross(cd);
				if (std::fabs(den) < 1e-12f) {
					continue;
				}
				const float s = (c0 - a).cross(cd) / den, u = (c0 - a).cross(ab) / den;
				if (s > 1e-4f && s < 1.0f - 1e-4f && u > 1e-4f && u < 1.0f - 1e-4f) {
					const int x = m.add_vertex(m.pos[cuts[i].first].lerp(m.pos[cuts[i].second], s));
					const std::pair<int, int> e1 = cuts[i], e2 = c;
					cuts[i] = { e1.first, x };
					cuts[j] = { e2.first, x };
					cuts.push_back({ x, e1.second });
					cuts.push_back({ x, e2.second });
					changed = true;
				}
			}
		}
		if (!changed) {
			break;
		}
	}
	// segments that cross nothing drawn (but at their ends)
	auto crosses = [&](int p, int q) {
		const Vector2 a = P2(p), ab = P2(q) - a;
		auto hit = [&](int x, int y) {
			if (x == p || x == q || y == p || y == q) {
				return false;
			}
			const Vector2 c0 = P2(x), cd = P2(y) - c0;
			const float den = ab.cross(cd);
			if (std::fabs(den) < 1e-12f) {
				return false;
			}
			const float s = (c0 - a).cross(cd) / den, u = (c0 - a).cross(ab) / den;
			return s > -1e-5f && s < 1.0f + 1e-5f && u > -1e-5f && u < 1.0f + 1e-5f;
		};
		for (int i = 0; i < F.size(); i++) {
			if (hit(F.v[i], F.v[(i + 1) % F.size()])) {
				return true;
			}
		}
		for (const auto &e : cuts) {
			if (hit(e.first, e.second)) {
				return true;
			}
		}
		// (and inside the face: its middle)
		const Vector2 mid = a + ab * 0.5f;
		bool in = false;
		for (int i = 0, j = F.size() - 1; i < F.size(); j = i++) {
			const Vector2 pi = P2(F.v[i]), pj = P2(F.v[j]);
			if ((pi.y > mid.y) != (pj.y > mid.y) && mid.x < (pj.x - pi.x) * (mid.y - pi.y) / (pj.y - pi.y) + pi.x) {
				in = !in;
			}
		}
		return !in;
	};
	auto nearest_corners = [&](int p) {
		std::vector<int> c(F.v.begin(), F.v.end());
		std::sort(c.begin(), c.end(), [&](int x, int y) { return P2(x).distance_squared_to(P2(p)) < P2(y).distance_squared_to(P2(p)); });
		return c;
	};
	// a cut ending inside the face: on to its nearest corner it can reach
	for (int pass = 0; pass < 2; pass++) {
		std::map<int, int> deg;
		for (const auto &e : cuts) {
			deg[e.first]++;
			deg[e.second]++;
		}
		for (const auto &kv : deg) {
			if (kv.second != 1 || bset.count(kv.first)) {
				continue;
			}
			for (int c : nearest_corners(kv.first)) {
				bool joined = false; // (the corner the cut comes from: not again)
				for (const auto &e : cuts) {
					joined = joined || (e.first == kv.first && e.second == c) || (e.second == kv.first && e.first == c);
				}
				if (!joined && !crosses(kv.first, c)) {
					cuts.push_back({ kv.first, c });
					break;
				}
			}
		}
	}
	// a loop inside the face (touching no corner): joined to two corners
	{
		std::map<int, int> parent;
		std::function<int(int)> find = [&](int x) {
			if (!parent.count(x)) {
				parent[x] = x;
			}
			return parent[x] == x ? x : parent[x] = find(parent[x]);
		};
		for (const auto &e : cuts) {
			parent[find(e.first)] = find(e.second);
		}
		std::map<int, std::vector<int>> comps;
		for (const auto &e : cuts) {
			comps[find(e.first)].push_back(e.first);
			comps[find(e.first)].push_back(e.second);
		}
		for (auto &kv : comps) {
			bool touches = false;
			for (int v : kv.second) {
				touches = touches || bset.count(v) > 0;
			}
			if (touches) {
				continue;
			}
			std::vector<std::pair<int, int>> cand; // (island vertex, corner) nearest first
			for (int v : kv.second) {
				for (int c : F.v) {
					cand.push_back({ v, c });
				}
			}
			std::sort(cand.begin(), cand.end(), [&](const std::pair<int, int> &x, const std::pair<int, int> &y) {
				return P2(x.first).distance_squared_to(P2(x.second)) < P2(y.first).distance_squared_to(P2(y.second));
			});
			int made = 0, used_v = -1, used_c = -1;
			for (const auto &c : cand) {
				if (made == 1 && (c.first == used_v || c.second == used_c)) {
					continue;
				}
				if (!crosses(c.first, c.second)) {
					cuts.push_back(c);
					used_v = c.first;
					used_c = c.second;
					if (++made == 2) {
						break;
					}
				}
			}
			if (made < 2) {
				// (not joined: the loop is left out)
				const std::set<int> island(kv.second.begin(), kv.second.end());
				cuts.erase(std::remove_if(cuts.begin(), cuts.end(), [&](const std::pair<int, int> &e) { return island.count(e.first) > 0; }), cuts.end());
			}
		}
	}
	// the cells: walking each edge with the face on its left, turning as little to the right as can be
	std::map<int, std::vector<int>> adj;
	auto link = [&](int a, int b) {
		adj[a].push_back(b);
		adj[b].push_back(a);
	};
	for (int i = 0; i < F.size(); i++) {
		link(F.v[i], F.v[(i + 1) % F.size()]);
	}
	for (const auto &e : cuts) {
		link(e.first, e.second);
	}
	for (auto &kv : adj) {
		const Vector2 c = P2(kv.first);
		std::vector<int> &l = kv.second;
		std::sort(l.begin(), l.end());
		l.erase(std::unique(l.begin(), l.end()), l.end());
		std::sort(l.begin(), l.end(), [&](int x, int y) {
			const Vector2 dx = P2(x) - c, dy = P2(y) - c;
			return std::atan2(dx.y, dx.x) < std::atan2(dy.y, dy.x);
		});
	}
	std::set<std::pair<int, int>> used;
	std::vector<std::vector<int>> cells;
	for (const auto &kv : adj) {
		for (int to : kv.second) {
			if (used.count({ kv.first, to })) {
				continue;
			}
			std::vector<int> cyc;
			int u = kv.first, v = to;
			bool ok = true;
			for (int guard = 0; guard < 100000; guard++) {
				if (!used.insert({ u, v }).second) {
					ok = u == kv.first && v == to;
					break;
				}
				cyc.push_back(u);
				const std::vector<int> &l = adj[v];
				const int k = (int)l.size();
				const int at = (int)(std::find(l.begin(), l.end(), u) - l.begin());
				const int w = l[(at - 1 + k) % k]; // (the next one clockwise from where it came from)
				u = v;
				v = w;
				if (u == kv.first && v == to) {
					break;
				}
			}
			if (!ok || cyc.size() < 3) {
				continue;
			}
			float a = 0.0f;
			for (size_t i = 0; i < cyc.size(); i++) {
				a += P2(cyc[i]).cross(P2(cyc[(i + 1) % cyc.size()]));
			}
			std::set<int> uniq_v(cyc.begin(), cyc.end());
			if (a > eps * eps && uniq_v.size() == cyc.size()) {
				cells.push_back(cyc);
			}
		}
	}
	if (cells.size() < 2) {
		return;
	}
	// the corners' UVs / colors: the outline's as they are, the new ones taken inside the face as it was
	std::map<int, std::pair<Vector2, Color>> attr;
	for (int i = 0; i < F.size(); i++) {
		attr[F.v[i]] = { F.uv[i], F.col[i] };
	}
	std::vector<Vector3> opts;
	for (int v : orig.v) {
		opts.push_back(m.pos[v]);
	}
	std::vector<int> otri;
	triangulate(opts, n, otri);
	auto inside_attr = [&](int v) {
		const Vector3 p = m.pos[v];
		float best = 1e30f;
		std::pair<Vector2, Color> r = { orig.uv.empty() ? Vector2() : orig.uv[0], orig.col.empty() ? Color(1, 1, 1, 1) : orig.col[0] };
		for (size_t k = 0; k + 2 < otri.size(); k += 3) {
			const int i0 = otri[k], i1 = otri[k + 1], i2 = otri[k + 2];
			const Vector3 a = opts[i0], b = opts[i1], c = opts[i2];
			const Vector3 v0 = b - a, v1 = c - a, v2 = p - a;
			const float d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1), d20 = v2.dot(v0), d21 = v2.dot(v1);
			const float den = d00 * d11 - d01 * d01;
			if (std::fabs(den) < 1e-20f) {
				continue;
			}
			const float bv = (d11 * d20 - d01 * d21) / den, bw = (d00 * d21 - d01 * d20) / den, bu = 1.0f - bv - bw;
			const float out_by = std::max(0.0f, -std::min(bu, std::min(bv, bw)));
			if (out_by < best) {
				best = out_by;
				const Vector2 uv = (i0 < (int)orig.uv.size() && i1 < (int)orig.uv.size() && i2 < (int)orig.uv.size()) ? orig.uv[i0] * bu + orig.uv[i1] * bv + orig.uv[i2] * bw : Vector2();
				const Color col = (i0 < (int)orig.col.size() && i1 < (int)orig.col.size() && i2 < (int)orig.col.size()) ? orig.col[i0] * bu + orig.col[i1] * bv + orig.col[i2] * bw : Color(1, 1, 1, 1);
				r = { uv, col };
			}
		}
		return r;
	};
	for (size_t ci = 0; ci < cells.size(); ci++) {
		Face nf = make_face(cells[ci], &F);
		nf.manual_uv = F.manual_uv;
		for (int i = 0; i < nf.size(); i++) {
			auto it = attr.find(nf.v[i]);
			const std::pair<Vector2, Color> a = it != attr.end() ? it->second : inside_attr(nf.v[i]);
			nf.uv[i] = a.first;
			nf.col[i] = a.second;
		}
		if (ci == 0) {
			m.faces[f] = nf;
		} else {
			m.faces.push_back(nf);
		}
	}
	for (const auto &e : cuts) {
		out.push_back(Edge(e.first, e.second));
	}
}

} // namespace

std::vector<Edge> knife_net(Mesh &m, const std::vector<std::vector<KnifeHit>> &paths) {
	std::vector<Edge> out;
	Topology t0;
	t0.build(m);
	// the hits' vertices: the ones there, new ones on edges (shared by hits at one spot), inside faces
	std::vector<std::vector<int>> ids(paths.size());
	std::map<uint64_t, std::vector<std::pair<float, int>>> on_edge; // t from the smaller vertex id
	std::vector<std::pair<int, int>> inner; // (face, vertex)
	for (size_t p = 0; p < paths.size(); p++) {
		for (const KnifeHit &h : paths[p]) {
			int id = -1;
			if (h.vert >= 0) {
				id = h.vert;
			} else if (h.ea >= 0) {
				const float t = h.ea < h.eb ? h.t : 1.0f - h.t;
				std::vector<std::pair<float, int>> &list = on_edge[ekey(h.ea, h.eb)];
				for (const std::pair<float, int> &e : list) {
					if (std::fabs(e.first - t) < 1e-5f) {
						id = e.second;
					}
				}
				if (id < 0) {
					id = m.add_vertex(h.pos);
					list.push_back({ t, id });
				}
			} else {
				for (const std::pair<int, int> &x : inner) {
					if (x.first == h.face && m.pos[x.second].distance_to(h.pos) < 1e-6f) {
						id = x.second;
					}
				}
				if (id < 0) {
					id = m.add_vertex(h.pos);
					inner.push_back({ h.face, id });
				}
			}
			ids[p].push_back(id);
		}
	}
	// the cuts on each face: two hits in a row with one face in common (on one edge both: along it, nothing)
	std::map<int, std::vector<std::pair<int, int>>> per_face;
	for (size_t p = 0; p < paths.size(); p++) {
		for (size_t i = 0; i + 1 < paths[p].size(); i++) {
			if (ids[p][i] == ids[p][i + 1]) {
				continue;
			}
			const std::vector<int> fa = faces_of_hit(t0, paths[p][i]), fb = faces_of_hit(t0, paths[p][i + 1]);
			std::vector<int> common;
			for (int f : fa) {
				if (std::find(fb.begin(), fb.end(), f) != fb.end()) {
					common.push_back(f);
				}
			}
			if (common.size() == 1) {
				per_face[common[0]].push_back({ ids[p][i], ids[p][i + 1] });
			}
		}
	}
	std::map<int, Face> orig;
	for (const auto &kv : per_face) {
		orig[kv.first] = m.faces[kv.first];
		orig[kv.first].ensure_attrs();
	}
	for (auto &kv : on_edge) {
		std::sort(kv.second.begin(), kv.second.end());
		insert_on_edge(m, (int)(kv.first >> 32), (int)(kv.first & 0xffffffffu), kv.second);
	}
	for (const auto &kv : per_face) {
		split_by_net(m, kv.first, kv.second, orig[kv.first], out);
	}
	cleanup(m);
	return uniq(out);
}

bool knife_share_face(const Mesh &m, const KnifeHit &a, const KnifeHit &b) {
	Topology t;
	t.build(m);
	const std::vector<int> fa = faces_of_hit(t, a), fb = faces_of_hit(t, b);
	for (int f : fa) {
		if (std::find(fb.begin(), fb.end(), f) != fb.end()) {
			return true;
		}
	}
	return false;
}

std::vector<Edge> knife_along(Mesh &m, const std::vector<KnifeHit> &p_path) {
	// the hits again by their positions (the edges / faces may have been cut since)
	Topology t;
	t.build(m);
	std::vector<KnifeHit> path;
	for (const KnifeHit &h0 : p_path) {
		KnifeHit h = h0;
		if (h.vert >= 0) {
			// (kept)
		} else if (h.ea >= 0 && t.find_edge(h.ea, h.eb) >= 0) {
			const Vector3 ab = m.pos[h.eb] - m.pos[h.ea];
			h.t = ab.length_squared() > 1e-18f ? std::min(1.0f, std::max(0.0f, (h.pos - m.pos[h.ea]).dot(ab) / ab.length_squared())) : 0.0f;
		} else if (h.ea >= 0) {
			// the edge was cut: the piece holding the point
			const int ea = h.ea, eb = h.eb;
			h.ea = h.eb = -1;
			float best = 1e30f;
			for (const Edge &e : t.edges) {
				const Vector3 a = m.pos[e.a], b = m.pos[e.b];
				const float l2 = (b - a).length_squared();
				if (l2 < 1e-18f) {
					continue;
				}
				const float u = std::min(1.0f, std::max(0.0f, (h0.pos - a).dot(b - a) / l2));
				const float d = (a + (b - a) * u).distance_to(h0.pos);
				if (d < best && d < 1e-4f * (1.0f + std::sqrt(l2))) {
					best = d;
					if (u < 1e-4f || u > 1.0f - 1e-4f) {
						h.vert = u < 0.5f ? e.a : e.b;
						h.ea = h.eb = -1;
					} else {
						h.vert = -1;
						h.ea = e.a;
						h.eb = e.b;
						h.t = u;
					}
				}
			}
			if (h.vert < 0 && h.ea < 0) {
				(void)ea;
				(void)eb;
				continue; // (lost)
			}
		} else {
			h.face = face_at_point(m, h.pos, h.face);
			if (h.face < 0) {
				continue;
			}
		}
		if (path.empty() || !same_hit(path.back(), h)) {
			path.push_back(h);
		}
	}
	return knife_along_impl(m, path);
}

std::vector<Edge> knife(Mesh &m, const std::vector<KnifePoint> &pts, const std::vector<Vector3> &cut_normals) {
	// A segment that can't be walked on the surface (its ends on parts not joined along the cut: a floor and a
	// ceiling seen across a room) splits the cut: the parts are cut one by one, nothing joins them.
	std::vector<std::vector<size_t>> runs = { { 0 } };
	for (size_t i = 0; i + 1 < pts.size(); i++) {
		std::vector<KnifeHit> tmp;
		const std::vector<Vector3> nn = { i < cut_normals.size() ? cut_normals[i] : Vector3() };
		if (knife_path(m, { pts[i], pts[i + 1] }, nn, tmp)) {
			runs.back().push_back(i + 1);
		} else {
			runs.push_back({ i + 1 });
		}
	}
	if (runs.size() == 1) {
		return knife_run(m, pts, cut_normals);
	}
	std::vector<Edge> out;
	bool first = true;
	for (const std::vector<size_t> &r : runs) {
		if (r.size() < 2) {
			continue;
		}
		std::vector<KnifePoint> rp;
		std::vector<Vector3> rn;
		for (size_t k = 0; k < r.size(); k++) {
			KnifePoint p = pts[r[k]];
			if (!first) {
				p.face = face_at_point(m, p.pos, p.face); // (the faces were split by the parts before)
			}
			rp.push_back(p);
			if (k + 1 < r.size()) {
				rn.push_back(r[k] < cut_normals.size() ? cut_normals[r[k]] : Vector3());
			}
		}
		bool ok = true;
		for (const KnifePoint &p : rp) {
			ok = ok && p.face >= 0;
		}
		if (!ok) {
			continue;
		}
		first = false;
		const std::vector<Edge> e = knife_run(m, rp, rn);
		out.insert(out.end(), e.begin(), e.end());
	}
	return uniq(out);
}

// ------------------------------------------------------------------ T-junctions

void repair_tjunctions(Mesh &m, float eps) {
	std::vector<char> used(m.pos.size(), 0);
	for (const Face &F : m.faces) {
		for (int v : F.v) {
			used[v] = 1;
		}
	}
	std::vector<int> cand;
	for (size_t i = 0; i < used.size(); i++) {
		if (used[i]) {
			cand.push_back((int)i);
		}
	}
	// Sort candidates by x for a quick range filter.
	std::sort(cand.begin(), cand.end(), [&](int a, int b) { return m.pos[a].x < m.pos[b].x; });
	std::vector<float> xs(cand.size());
	for (size_t i = 0; i < cand.size(); i++) {
		xs[i] = m.pos[cand[i]].x;
	}
	for (Face &F : m.faces) {
		const std::vector<Corner> cs = corners_of(F);
		const int n = F.size();
		std::vector<Corner> ring;
		bool changed = false;
		for (int i = 0; i < n; i++) {
			const Corner &ca = cs[i], &cb = cs[(i + 1) % n];
			ring.push_back(ca);
			const Vector3 a = m.pos[ca.v], b = m.pos[cb.v];
			const Vector3 ab = b - a;
			const float l2 = ab.length_squared();
			if (l2 < eps * eps) {
				continue;
			}
			const float x0 = std::min(a.x, b.x) - eps, x1 = std::max(a.x, b.x) + eps;
			auto it = std::lower_bound(xs.begin(), xs.end(), x0);
			std::vector<std::pair<float, int>> on;
			for (size_t k = it - xs.begin(); k < xs.size() && xs[k] <= x1; k++) {
				const int v = cand[k];
				if (v == ca.v || v == cb.v) {
					continue;
				}
				const Vector3 p = m.pos[v];
				const float t = (p - a).dot(ab) / l2;
				if (t <= 1e-5f || t >= 1.0f - 1e-5f) {
					continue;
				}
				if ((a + ab * t).distance_squared_to(p) <= eps * eps) {
					on.push_back({ t, v });
				}
			}
			if (on.empty()) {
				continue;
			}
			std::sort(on.begin(), on.end());
			for (const std::pair<float, int> &p : on) {
				ring.push_back(lerp_corner(ca, cb, p.first, p.second));
			}
			changed = true;
		}
		if (changed) {
			F = face_from(ring, F);
		}
	}
}

} // namespace ops
} // namespace hewn

namespace hewn {
namespace ops {

// ================================================================== limited dissolve

// Simplify (Planar): the faces joined into regions over the edges where they meet at less than the angle (same
// material), each region one face when its outline is one ring; then the corners where an outline goes straight on
// dropped. One pass over the edges (no rounds of pairs): fast enough to follow a slider.
void planar_simplify(Mesh &m, float angle_deg) {
	const int nf = (int)m.faces.size();
	if (nf < 2) {
		return;
	}
	const float cos_lim = std::cos(std::max(0.0f, angle_deg) * (float)godot::Math::PI / 180.0f) - 1e-6f;
	std::vector<Vector3> nrm(nf);
	for (int f = 0; f < nf; f++) {
		nrm[f] = face_normal(m, m.faces[f]);
	}
	std::vector<int> parent(nf);
	for (int f = 0; f < nf; f++) {
		parent[f] = f;
	}
	auto find = [&](int x) {
		while (parent[x] != x) {
			parent[x] = parent[parent[x]];
			x = parent[x];
		}
		return x;
	};
	// the faces on each edge (sorted by edge: no hash table)
	struct HE {
		uint64_t k;
		int f;
	};
	std::vector<HE> hes;
	for (int f = 0; f < nf; f++) {
		const Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			hes.push_back({ ekey(F.v[i], F.v[(i + 1) % F.size()]), f });
		}
	}
	std::sort(hes.begin(), hes.end(), [](const HE &a, const HE &b) { return a.k < b.k; });
	for (size_t i = 0; i + 1 < hes.size(); i++) {
		if (hes[i].k != hes[i + 1].k || (i + 2 < hes.size() && hes[i + 2].k == hes[i].k)) {
			continue; // (only edges of two faces)
		}
		const int f = hes[i].f, g = hes[i + 1].f;
		if (f != g && m.faces[f].material == m.faces[g].material && nrm[f].dot(nrm[g]) >= cos_lim) {
			parent[find(f)] = find(g);
		}
	}
	std::unordered_map<int, std::vector<int>> groups_of;
	for (int f = 0; f < nf; f++) {
		groups_of[find(f)].push_back(f);
	}
	std::vector<std::vector<int>> groups;
	for (auto &kv : groups_of) {
		if (kv.second.size() > 1) {
			groups.push_back(kv.second);
		}
	}
	if (!groups.empty()) {
		merge_groups(m, groups);
	}
	// the corners where an outline goes straight on (a vertex of two edges)
	Topology t;
	t.build(m);
	std::vector<char> drop(m.pos.size(), 0);
	for (int v = 0; v < (int)m.pos.size(); v++) {
		if (t.vert_edges[v].size() != 2) {
			continue;
		}
		const Edge &e0 = t.edges[t.vert_edges[v][0]], &e1 = t.edges[t.vert_edges[v][1]];
		const int a = e0.a == v ? e0.b : e0.a, b = e1.a == v ? e1.b : e1.a;
		const Vector3 d0 = (m.pos[v] - m.pos[a]).normalized(), d1 = (m.pos[b] - m.pos[v]).normalized();
		if (d0.dot(d1) >= cos_lim) {
			drop[v] = 1;
		}
	}
	for (Face &F : m.faces) {
		for (int i = F.size() - 1; i >= 0 && F.size() > 3; i--) {
			if (drop[F.v[i]]) {
				F.erase_corner(i);
			}
		}
	}
	cleanup(m);
	remove_unused_vertices(m);
}

int limited_dissolve(Mesh &m, const std::vector<int> &p_faces, float angle_deg, std::vector<int> *track) {
	const float cos_lim = std::cos(std::max(0.0f, angle_deg) * (float)godot::Math::PI / 180.0f);
	int merged = 0;
	std::vector<char> in = p_faces.empty() ? std::vector<char>(m.faces.size(), 1) : mask_of(p_faces, m.faces.size());
	// 1. Merge neighboring faces whose normals are within the angle, while the union stays a simple ring.
	for (int pass = 0; pass < 256; pass++) {
		Topology t;
		t.build(m);
		std::vector<Vector3> nrm(m.faces.size());
		for (size_t f = 0; f < m.faces.size(); f++) {
			nrm[f] = face_normal(m, m.faces[f]);
		}
		std::vector<char> touched(m.faces.size(), 0), del(m.faces.size(), 0);
		bool changed = false;
		for (size_t e = 0; e < t.edges.size(); e++) {
			const std::vector<int> &ef = t.edge_faces[e];
			if (ef.size() != 2) {
				continue;
			}
			const int f = ef[0], g = ef[1];
			if (f == g || !in[f] || !in[g] || touched[f] || touched[g] || m.faces[f].material != m.faces[g].material) {
				continue;
			}
			if (nrm[f].dot(nrm[g]) < cos_lim) {
				continue;
			}
			Face out;
			if (!merge_boundary(m, { f, g }, out)) {
				continue;
			}
			m.faces[f] = out;
			del[g] = 1;
			touched[f] = touched[g] = 1;
			changed = true;
			merged++;
		}
		if (!changed) {
			break;
		}
		std::vector<char> nin;
		for (size_t i = 0; i < in.size(); i++) {
			if (!del[i]) {
				nin.push_back(in[i]);
			}
		}
		std::vector<int> remap;
		delete_faces_mask(m, del, &remap);
		in.swap(nin);
		if (track) {
			std::vector<int> nt;
			for (int f : *track) {
				if (f >= 0 && f < (int)remap.size() && remap[f] >= 0) {
					nt.push_back(remap[f]);
				}
			}
			*track = nt;
		}
	}
	// 2. Remove corners where the outline goes straight on (vertex shared by exactly two edges).
	Topology t;
	t.build(m);
	std::vector<char> drop(m.pos.size(), 0);
	for (int v = 0; v < (int)m.pos.size(); v++) {
		if (t.vert_edges[v].size() != 2) {
			continue;
		}
		bool all_in = true;
		for (int f : t.vert_faces[v]) {
			all_in = all_in && in[f];
		}
		if (!all_in) {
			continue;
		}
		const Edge &e0 = t.edges[t.vert_edges[v][0]], &e1 = t.edges[t.vert_edges[v][1]];
		const int a = e0.a == v ? e0.b : e0.a, b = e1.a == v ? e1.b : e1.a;
		const Vector3 d0 = (m.pos[v] - m.pos[a]).normalized(), d1 = (m.pos[b] - m.pos[v]).normalized();
		if (d0.dot(d1) >= cos_lim) {
			drop[v] = 1;
		}
	}
	for (Face &F : m.faces) {
		for (int i = F.size() - 1; i >= 0 && F.size() > 3; i--) {
			if (drop[F.v[i]]) {
				F.erase_corner(i);
			}
		}
	}
	std::vector<int> remap;
	cleanup(m, &remap);
	if (track) {
		std::vector<int> nt;
		for (int f : *track) {
			if (f >= 0 && f < (int)remap.size() && remap[f] >= 0) {
				nt.push_back(remap[f]);
			}
		}
		*track = nt;
	}
	return merged;
}

} // namespace ops
} // namespace hewn

namespace hewn {
namespace ops {

// ================================================================== modifiers

void mirror(Mesh &m, int axis, float merge) {
	if (axis < 0 || axis > 2 || m.faces.empty()) {
		return;
	}
	const int nv = (int)m.pos.size();
	const int nf = (int)m.faces.size();
	for (int i = 0; i < nv; i++) {
		Vector3 p = m.pos[i];
		p[axis] = -p[axis];
		m.pos.push_back(p);
	}
	for (int f = 0; f < nf; f++) {
		Face F = m.faces[f];
		for (int &v : F.v) {
			v += nv;
		}
		F.reverse();
		m.faces.push_back(F);
	}
	// Join the halves along the mirror plane: snap near-plane vertices onto it, then weld (merge < 0: not).
	if (merge < 0.0f) {
		return;
	}
	const float eps = std::max(1e-6f, merge);
	for (Vector3 &p : m.pos) {
		if (std::fabs(p[axis]) <= eps) {
			p[axis] = 0.0f;
		}
	}
	weld_by_distance(m, eps);
	// Faces lying in the mirror plane now exist twice with opposite winding: drop both copies. A half
	// that was already symmetric gives the same faces again with the same winding: keep one of them.
	auto same_cycle = [](const std::vector<int> &a, std::vector<int> b) {
		const size_t n = a.size();
		for (size_t k = 0; k < n; k++) {
			bool eq = true;
			for (size_t i = 0; i < n && eq; i++) {
				eq = a[i] == b[(i + k) % n];
			}
			if (eq) {
				return true;
			}
		}
		return false;
	};
	std::map<std::vector<int>, int> seen;
	std::vector<char> del(m.faces.size(), 0);
	for (int f = 0; f < (int)m.faces.size(); f++) {
		std::vector<int> key = m.faces[f].v;
		std::sort(key.begin(), key.end());
		auto it = seen.find(key);
		if (it != seen.end() && !del[it->second]) {
			if (same_cycle(m.faces[it->second].v, m.faces[f].v)) {
				del[f] = 1; // a duplicate
			} else {
				del[f] = del[it->second] = 1; // a cap on the plane
			}
		} else {
			seen[key] = f;
		}
	}
	delete_faces_mask(m, del);
	cleanup(m);
	remove_unused_vertices(m);
}

void solidify(Mesh &m, float thickness, float offset) {
	if (std::fabs(thickness) < 1e-9f || m.faces.empty()) {
		return;
	}
	offset = std::max(-1.0f, std::min(1.0f, offset));
	const float out_d = thickness * (1.0f + offset) * 0.5f;
	const float in_d = -thickness * (1.0f - offset) * 0.5f;
	const std::vector<Vector3> vn = vertex_normals(m);
	const int nv = (int)m.pos.size();
	const int nf = (int)m.faces.size();
	// Border half-edges (before the inner shell is added).
	std::unordered_set<uint64_t> halves;
	for (const Face &F : m.faces) {
		for (int i = 0; i < F.size(); i++) {
			halves.insert(hkey(F.v[i], F.v[(i + 1) % F.size()]));
		}
	}
	std::vector<std::pair<std::pair<int, int>, int>> border; // (a -> b, face)
	for (int f = 0; f < nf; f++) {
		const Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			const int a = F.v[i], b = F.v[(i + 1) % F.size()];
			if (!halves.count(hkey(b, a))) {
				border.push_back({ { a, b }, f });
			}
		}
	}
	for (int i = 0; i < nv; i++) {
		m.pos.push_back(m.pos[i] + vn[i] * in_d);
		m.pos[i] += vn[i] * out_d;
	}
	for (int f = 0; f < nf; f++) {
		Face F = m.faces[f];
		for (int &v : F.v) {
			v += nv;
		}
		F.reverse();
		m.faces.push_back(F);
	}
	for (const auto &b : border) {
		const int a = b.first.first, c = b.first.second;
		Face rim = make_face({ c, a, a + nv, c + nv }, &m.faces[b.second]);
		rim.manual_uv = false;
		m.faces.push_back(rim);
	}
}

} // namespace ops
} // namespace hewn

namespace hewn {
namespace ops {

// ================================================================== extrude helpers

// "Dissolve Orthogonal Edges": a new side face that lies in the plane of the old face next to it
// either extends that face (same facing: the edge between them is dissolved) or cancels the part of it
// it overlaps (opposite facing, e.g. a face extruded inwards: the wall is trimmed down to the new cap).
void dissolve_orthogonal(Mesh &m, int first_new_face, std::vector<int> &caps) {
	const float cos_lim = std::cos(0.5f * (float)godot::Math::PI / 180.0f);
	std::unordered_set<int> cap_set(caps.begin(), caps.end());
	Topology t;
	t.build(m);
	std::vector<char> del(m.faces.size(), 0);
	std::vector<int> merged_faces;
	for (int sf = first_new_face; sf < (int)m.faces.size(); sf++) {
		if (cap_set.count(sf) || del[sf] || m.faces[sf].size() != 4) {
			continue;
		}
		const Face S = m.faces[sf];
		const Vector3 ns = face_normal(m, S);
		for (int i = 0; i < 4; i++) {
			const int a = S.v[i], b = S.v[(i + 1) % 4];
			const int e = t.find_edge(a, b);
			int w = -1;
			for (int o : t.edge_faces[e]) {
				if (o != sf && o < first_new_face && !cap_set.count(o) && !del[o]) {
					w = o;
				}
			}
			if (w < 0) {
				continue;
			}
			const Vector3 nw = face_normal(m, m.faces[w]);
			if (std::fabs(ns.dot(nw)) < cos_lim || std::fabs(nw.dot(face_center(m, S) - face_center(m, m.faces[w]))) > 1e-4f) {
				break; // the base edge is not orthogonal: keep it
			}
			if (ns.dot(nw) > 0.0f) {
				Face merged;
				if (merge_boundary(m, { w, sf }, merged)) {
					m.faces[w] = merged;
					del[sf] = 1;
					merged_faces.push_back(w);
				}
			} else {
				// S = [a, b, b', a'] (in some rotation): move the shared edge of W onto the cap edge.
				const int a2 = S.v[(i + 3) % 4], b2 = S.v[(i + 2) % 4];
				for (int &v : m.faces[w].v) {
					if (v == a) {
						v = a2;
					} else if (v == b) {
						v = b2;
					}
				}
				del[sf] = 1;
			}
			break;
		}
	}
	std::vector<int> remap;
	delete_faces_mask(m, del, &remap);
	auto apply = [&](std::vector<int> &list) {
		std::vector<int> nl;
		for (int f : list) {
			if (f >= 0 && f < (int)remap.size() && remap[f] >= 0) {
				nl.push_back(remap[f]);
			}
		}
		list = nl;
	};
	apply(caps);
	apply(merged_faces);
	cleanup(m, &remap);
	apply(caps);
	apply(merged_faces);
	if (!merged_faces.empty()) {
		// the old boundary vertices are now in the middle of straight edges
		std::vector<int> track = caps;
		const size_t nc = track.size();
		track.insert(track.end(), merged_faces.begin(), merged_faces.end());
		limited_dissolve(m, uniq(merged_faces), 0.5f, &track);
		track.resize(std::min(track.size(), nc));
		caps = track;
	}
}

// "Extrude Faces Along Normals": a region extrude whose new vertices move along their normals.
std::vector<int> extrude_along_normals(Mesh &m, const std::vector<int> &faces, float dist) {
	std::vector<int> moved;
	std::vector<int> caps = extrude_faces(m, faces, 0.0f, false, &moved);
	std::unordered_map<int, Vector3> nrm;
	for (int f : caps) {
		const Vector3 n = face_normal_raw(m, m.faces[f]);
		for (int v : m.faces[f].v) {
			nrm[v] += n;
		}
	}
	for (auto &kv : nrm) {
		if (kv.second.length() > 1e-12f) {
			m.pos[kv.first] += kv.second.normalized() * dist;
		}
	}
	return caps;
}

} // namespace ops
} // namespace hewn

namespace hewn {
namespace ops {

// ================================================================== bisect

namespace {
int face_of_half(const Mesh &m, int a, int b); // (below)
} // namespace

std::vector<Edge> bisect(Mesh &m, const std::vector<int> &p_faces, const Vector3 &point, const Vector3 &p_normal, bool clear_inner, bool clear_outer, bool fill) {
	std::vector<Edge> out;
	if (p_normal.length() < 1e-12f || m.faces.empty()) {
		return out;
	}
	const Vector3 n = p_normal.normalized();
	std::vector<char> in = p_faces.empty() ? std::vector<char>(m.faces.size(), 1) : mask_of(p_faces, m.faces.size());
	Vector3 mn = m.pos.empty() ? Vector3() : m.pos[0], mx = mn;
	for (const Vector3 &p : m.pos) {
		mn = mn.min(p);
		mx = mx.max(p);
	}
	const float eps = std::max(1e-6f, (mx - mn).length() * 1e-5f);
	auto side = [&](int v) {
		const float d = n.dot(m.pos[v] - point);
		return std::fabs(d) <= eps ? 0.0f : d;
	};
	// 1. New vertices where edges of the face set cross the plane.
	std::map<uint64_t, int> cross;
	for (int f = 0; f < (int)m.faces.size(); f++) {
		if (!in[f]) {
			continue;
		}
		const Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			const int a = F.v[i], b = F.v[(i + 1) % F.size()];
			const float sa = side(a), sb = side(b);
			if (sa * sb >= 0.0f || cross.count(ekey(a, b))) {
				continue;
			}
			const float t = sa / (sa - sb);
			cross[ekey(a, b)] = m.add_vertex(m.pos[a].lerp(m.pos[b], t));
		}
	}
	for (auto &kv : cross) {
		const int a = (int)(kv.first >> 32), b = (int)(kv.first & 0xffffffffu);
		const float sa = side(a), sb = side(b);
		insert_on_edge(m, a, b, { { sa / (sa - sb), kv.second } });
	}
	// 2. Split every face of the set along its corners lying on the plane.
	const int nf0 = (int)m.faces.size();
	Vector3 u, w;
	plane_basis(n, u, w);
	for (int f = 0; f < nf0; f++) {
		if (!in[f]) {
			continue;
		}
		const Face F = m.faces[f];
		bool pos = false, neg = false;
		std::vector<int> on;
		for (int v : F.v) {
			const float s = side(v);
			pos = pos || s > 0.0f;
			neg = neg || s < 0.0f;
			if (s == 0.0f) {
				on.push_back(v);
			}
		}
		if (!pos || !neg || on.size() < 2) {
			continue;
		}
		// Order along the cut line; each stretch between neighbors that runs inside the face splits it. (Not just
		// pairwise: a concave face touching the line at a corner, or with a side along it, has on-line corners
		// that are no crossings, and pairing them would split across the outside.)
		const Vector3 dir = n.cross(face_normal(m, F)).length() > 1e-6f ? n.cross(face_normal(m, F)).normalized() : u;
		std::sort(on.begin(), on.end(), [&](int a, int b) { return m.pos[a].dot(dir) < m.pos[b].dot(dir); });
		for (size_t k = 0; k + 1 < on.size(); k++) {
			const int a = on[k], b = on[k + 1];
			if (m.pos[a].distance_to(m.pos[b]) < eps) {
				continue;
			}
			const Vector3 mid = (m.pos[a] + m.pos[b]) * 0.5f;
			const int target = face_with(m, a, b, mid);
			if (target < 0 || !point_in_face(m, m.faces[target], mid)) {
				continue; // (outside the face: between two of its parts)
			}
			const Face &T = m.faces[target];
			bool side_edge = false; // (already a side of it: along the line)
			for (int i = 0; i < T.size() && !side_edge; i++) {
				const int x = T.v[i], y = T.v[(i + 1) % T.size()];
				side_edge = (x == a && y == b) || (x == b && y == a);
			}
			if (!side_edge && split_face_path(m, target, { a, b }) >= 0) {
				out.push_back(Edge(a, b));
				in.resize(m.faces.size(), 1);
			}
		}
	}
	in.resize(m.faces.size(), 1);
	// 3. Optionally remove one side.
	if (clear_inner || clear_outer) {
		std::vector<char> del(m.faces.size(), 0);
		for (int f = 0; f < (int)m.faces.size(); f++) {
			if (!in[f]) {
				continue;
			}
			bool pos = false, neg = false;
			for (int v : m.faces[f].v) {
				const float s = side(v);
				pos = pos || s > 0.0f;
				neg = neg || s < 0.0f;
			}
			if ((clear_inner && neg && !pos) || (clear_outer && pos && !neg)) {
				del[f] = 1;
			}
		}
		delete_faces_mask(m, del);
		// 4. Close the cut.
		if (fill) {
			std::vector<int> on_plane;
			for (int v = 0; v < (int)m.pos.size(); v++) {
				if (side(v) == 0.0f) {
					on_plane.push_back(v);
				}
			}
			for (const std::vector<int> &loop : border_loops(m)) {
				bool all = true;
				for (int v : loop) {
					all = all && side(v) == 0.0f;
				}
				if (all) {
					// (the cut's face: the material and texture settings of a face it closes against)
					int tf = -1;
					for (size_t i = 0; i < loop.size() && tf < 0; i++) {
						tf = face_of_half(m, loop[(i + 1) % loop.size()], loop[i]);
						if (tf < 0) {
							tf = face_of_half(m, loop[i], loop[(i + 1) % loop.size()]);
						}
					}
					const Face tmpl = tf >= 0 ? m.faces[tf] : Face();
					Face nf = make_face(loop, tf >= 0 ? &tmpl : nullptr);
					nf.uvp.axis = -1;
					m.faces.push_back(nf);
				}
			}
		}
	}
	cleanup(m);
	if (clear_inner || clear_outer) {
		// (the corners of the side cleared go too: nothing uses them; the cut's edges keep up with the numbers)
		Selection sel;
		sel.edges = uniq(out);
		remove_unused_vertices(m, &sel);
		return sel.edges;
	}
	return uniq(out);
}

} // namespace ops
} // namespace hewn

namespace hewn {
namespace ops {

// ================================================================== more operations

std::vector<int> poke_faces(Mesh &m, const std::vector<int> &p_faces) {
	std::vector<int> out;
	for (int f : valid_faces(m, p_faces)) {
		const Face F = m.faces[f];
		const std::vector<Corner> cs = corners_of(F);
		const int n = F.size();
		Corner c;
		c.v = m.add_vertex(face_center(m, F));
		c.uv = Vector2();
		c.c = Color(0, 0, 0, 0);
		for (const Corner &x : cs) {
			c.uv += x.uv / (float)n;
			c.c += x.c * (1.0f / n);
		}
		for (int i = 0; i < n; i++) {
			Face t = face_from({ cs[i], cs[(i + 1) % n], c }, F);
			if (i == 0) {
				m.faces[f] = t;
				out.push_back(f);
			} else {
				out.push_back((int)m.faces.size());
				m.faces.push_back(t);
			}
		}
	}
	return out;
}

// Bridge Faces: the two selected regions are removed and their outlines joined by a tube.
std::vector<int> bridge_faces(Mesh &m, const std::vector<int> &p_faces) {
	std::vector<int> out;
	Topology t;
	t.build(m);
	std::vector<std::vector<int>> comps = face_components(m, t, valid_faces(m, p_faces));
	if (comps.size() != 2) {
		return out;
	}
	std::vector<std::vector<int>> rings;
	for (const std::vector<int> &c : comps) {
		Face b;
		if (!merge_boundary(m, c, b)) {
			return out;
		}
		rings.push_back(b.v);
	}
	if (rings[0].size() != rings[1].size()) {
		return out;
	}
	const size_t n = rings[0].size();
	// The second outline runs the other way round (it faces the first one); find the best rotation.
	std::vector<int> r1(rings[1].rbegin(), rings[1].rend());
	size_t best_k = 0;
	float best = 1e30f;
	for (size_t k = 0; k < n; k++) {
		float d = 0.0f;
		for (size_t i = 0; i < n; i++) {
			d += m.pos[rings[0][i]].distance_squared_to(m.pos[r1[(i + k) % n]]);
		}
		if (d < best) {
			best = d;
			best_k = k;
		}
	}
	const Face attrs = m.faces[comps[0][0]];
	std::vector<char> del(m.faces.size(), 0);
	for (const std::vector<int> &c : comps) {
		for (int f : c) {
			del[f] = 1;
		}
	}
	delete_faces_mask(m, del);
	for (size_t i = 0; i < n; i++) {
		const int a = rings[0][i], b = rings[0][(i + 1) % n];
		const int c = r1[(i + 1 + best_k) % n], d = r1[(i + best_k) % n];
		Face q = make_face({ a, b, c, d }, &attrs); // a->b continues the outline of the removed region
		q.manual_uv = false;
		out.push_back((int)m.faces.size());
		m.faces.push_back(q);
	}
	return out;
}

// Edge Split: faces on the two sides of a selected edge stop sharing its vertices.
std::vector<int> split_edges(Mesh &m, const std::vector<Edge> &edges) {
	std::unordered_set<uint64_t> se;
	std::unordered_set<int> verts;
	for (const Edge &e : edges) {
		se.insert(ekey(e));
		verts.insert(e.a);
		verts.insert(e.b);
	}
	std::vector<int> out;
	Topology t;
	t.build(m);
	for (int v : verts) {
		const std::vector<int> fs = t.vert_faces[v];
		if (fs.size() < 2) {
			continue;
		}
		// faces around v joined across non-selected edges stay together
		UnionFind uf((int)m.faces.size());
		std::unordered_map<int, std::vector<int>> by_neighbor;
		for (int f : fs) {
			const Face &F = m.faces[f];
			for (int i = 0; i < F.size(); i++) {
				if (F.v[i] != v) {
					continue;
				}
				for (int x : { F.v[(i + 1) % F.size()], F.v[(i + F.size() - 1) % F.size()] }) {
					if (!se.count(ekey(v, x))) {
						by_neighbor[x].push_back(f);
					}
				}
			}
		}
		for (auto &kv : by_neighbor) {
			for (size_t i = 1; i < kv.second.size(); i++) {
				uf.unite(kv.second[i], kv.second[0]);
			}
		}
		std::vector<std::vector<int>> groups = groups_of(uf, fs);
		for (size_t g = 1; g < groups.size(); g++) {
			const int nv = m.add_vertex(m.pos[v]);
			out.push_back(nv);
			for (int f : groups[g]) {
				for (int &x : m.faces[f].v) {
					if (x == v) {
						x = nv;
					}
				}
			}
		}
	}
	return out;
}

// Vertex bevel: every selected vertex is cut off by a small face.
std::vector<int> bevel_vertices(Mesh &m, const std::vector<int> &verts, float amount) {
	std::vector<int> out;
	if (amount <= 0.0f) {
		return out;
	}
	std::unordered_set<int> vs(verts.begin(), verts.end());
	std::unordered_map<uint64_t, int> slide; // hkey(v, toward)
	auto point = [&](int v, int x) {
		const uint64_t k = hkey(v, x);
		auto it = slide.find(k);
		if (it != slide.end()) {
			return it->second;
		}
		const Vector3 d = m.pos[x] - m.pos[v];
		const float l = d.length();
		const int nv = m.add_vertex(m.pos[v] + (l > 1e-9f ? d / l : Vector3()) * std::min(amount, l * (vs.count(x) ? 0.49f : 0.98f)));
		slide[k] = nv;
		return nv;
	};
	const int nf = (int)m.faces.size();
	for (int f = 0; f < nf; f++) {
		const Face F = m.faces[f];
		const std::vector<Corner> cs = corners_of(F);
		const int n = F.size();
		std::vector<Corner> ring;
		bool changed = false;
		for (int i = 0; i < n; i++) {
			const int v = F.v[i];
			if (!vs.count(v)) {
				ring.push_back(cs[i]);
				continue;
			}
			changed = true;
			const int u = F.v[(i + n - 1) % n], w = F.v[(i + 1) % n];
			ring.push_back(with_vertex(cs[i], point(v, u)));
			ring.push_back(with_vertex(cs[i], point(v, w)));
		}
		if (changed) {
			m.faces[f] = face_from(ring, F);
		}
	}
	// close the hole around every beveled vertex
	std::unordered_set<uint64_t> halves;
	for (const Face &F : m.faces) {
		for (int i = 0; i < F.size(); i++) {
			halves.insert(hkey(F.v[i], F.v[(i + 1) % F.size()]));
		}
	}
	for (int v : verts) {
		std::unordered_set<int> pts;
		for (const auto &kv : slide) {
			if ((int)(kv.first >> 32) == v) {
				pts.insert(kv.second);
			}
		}
		std::unordered_map<int, int> next;
		for (const Face &F : m.faces) {
			for (int i = 0; i < F.size(); i++) {
				const int a = F.v[i], b = F.v[(i + 1) % F.size()];
				if (pts.count(a) && pts.count(b) && !halves.count(hkey(b, a))) {
					next[b] = a;
				}
			}
		}
		if (next.size() < 3) {
			continue;
		}
		std::vector<int> loop;
		const int s0 = next.begin()->first;
		int x = s0;
		bool closed = false;
		for (size_t g = 0; g <= next.size(); g++) {
			loop.push_back(x);
			auto it = next.find(x);
			if (it == next.end()) {
				break;
			}
			x = it->second;
			if (x == s0) {
				closed = true;
				break;
			}
		}
		if (closed && loop.size() >= 3) {
			out.push_back((int)m.faces.size());
			m.faces.push_back(make_face(loop));
		}
	}
	cleanup(m);
	return out;
}

// Delete Edge Loops: dissolve the edges, then the vertices left in the middle of straight lines.
void delete_edge_loops(Mesh &m, const std::vector<Edge> &edges) {
	std::vector<int> faces = dissolve_edges(m, edges);
	limited_dissolve(m, faces, 1.0f);
}

// ------------------------------------------------------------------ to circle (LoopTools)

int to_circle(Mesh &m, const std::vector<int> &verts, const std::vector<int> &faces, bool regular, float influence) {
	Topology t;
	t.build(m);
	// the edges the loops run along: the border of the selected faces, or the edges between the vertices
	std::vector<int> loop_edges;
	if (!faces.empty()) {
		std::unordered_set<int> fs(faces.begin(), faces.end());
		for (int e = 0; e < (int)t.edges.size(); e++) {
			int inside = 0;
			for (int f : t.edge_faces[e]) {
				inside += fs.count(f) ? 1 : 0;
			}
			if (inside == 1) {
				loop_edges.push_back(e);
			}
		}
	} else {
		std::unordered_set<int> vs(verts.begin(), verts.end());
		for (int e = 0; e < (int)t.edges.size(); e++) {
			if (vs.count(t.edges[e].a) && vs.count(t.edges[e].b)) {
				loop_edges.push_back(e);
			}
		}
	}
	std::unordered_map<int, std::vector<int>> adj;
	for (int e : loop_edges) {
		adj[t.edges[e].a].push_back(t.edges[e].b);
		adj[t.edges[e].b].push_back(t.edges[e].a);
	}
	// connected groups, ordered along the path / cycle when they are one (else by angle, below)
	std::unordered_set<int> seen;
	std::vector<std::vector<int>> groups;
	std::vector<int> keys;
	for (const auto &kv : adj) {
		keys.push_back(kv.first);
	}
	std::sort(keys.begin(), keys.end());
	for (int start : keys) {
		if (seen.count(start)) {
			continue;
		}
		std::vector<int> comp, stack = { start };
		seen.insert(start);
		while (!stack.empty()) {
			const int v = stack.back();
			stack.pop_back();
			comp.push_back(v);
			for (int w : adj[v]) {
				if (!seen.count(w)) {
					seen.insert(w);
					stack.push_back(w);
				}
			}
		}
		bool simple = true;
		int end = comp[0];
		for (int v : comp) {
			simple = simple && adj[v].size() <= 2;
			if (adj[v].size() == 1) {
				end = v;
			}
		}
		if (simple) {
			std::vector<int> order = { end };
			int prev = -1, cur = end;
			while (true) {
				int next = -1;
				for (int w : adj[cur]) {
					if (w != prev && (order.size() < 2 || w != order[0]) && std::find(order.begin(), order.end(), w) == order.end()) {
						next = w;
						break;
					}
				}
				if (next < 0) {
					break;
				}
				order.push_back(next);
				prev = cur;
				cur = next;
			}
			comp = order;
		} else {
			comp.push_back(-1); // marker: order by angle
		}
		groups.push_back(comp);
	}
	if (groups.empty() && faces.empty() && verts.size() >= 3) {
		std::vector<int> g = verts;
		g.push_back(-1);
		groups.push_back(g); // loose vertices: one ring around their center
	}
	int moved = 0;
	for (std::vector<int> &g : groups) {
		const bool by_angle = !g.empty() && g.back() == -1;
		if (by_angle) {
			g.pop_back();
		}
		if (g.size() < 3) {
			continue;
		}
		Vector3 c;
		for (int v : g) {
			c += m.pos[v];
		}
		c /= (float)g.size();
		Vector3 n;
		if (by_angle) {
			// plane from the spread of the points (largest cross product around the center)
			for (size_t i = 0; i < g.size(); i++) {
				for (size_t j = i + 1; j < g.size(); j++) {
					Vector3 x = (m.pos[g[i]] - c).cross(m.pos[g[j]] - c);
					if (n.length_squared() > 0.0f && x.dot(n) < 0.0f) {
						x = -x;
					}
					n += x;
				}
			}
		} else {
			std::vector<Vector3> pts;
			for (int v : g) {
				pts.push_back(m.pos[v]);
			}
			n = newell(pts);
		}
		if (n.length() < 1e-9f) {
			continue; // all on a line
		}
		n.normalize();
		Vector3 u = m.pos[g[0]] - c;
		u -= n * u.dot(n);
		if (u.length() < 1e-9f) {
			Vector3 tmp;
			plane_basis(n, u, tmp);
		}
		u.normalize();
		const Vector3 w = n.cross(u);
		float r = 0.0f;
		std::vector<float> ang(g.size());
		for (size_t i = 0; i < g.size(); i++) {
			const Vector3 d = m.pos[g[i]] - c;
			r += (d - n * d.dot(n)).length();
			ang[i] = std::atan2(d.dot(w), d.dot(u));
		}
		r /= (float)g.size();
		if (by_angle) {
			std::vector<size_t> idx(g.size());
			for (size_t i = 0; i < idx.size(); i++) {
				idx[i] = i;
			}
			std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return ang[a] < ang[b]; });
			std::vector<int> g2;
			std::vector<float> a2;
			for (size_t i : idx) {
				g2.push_back(g[i]);
				a2.push_back(ang[i]);
			}
			g = g2;
			ang = a2;
		}
		const float step = 6.28318530718f / (float)g.size();
		for (size_t i = 0; i < g.size(); i++) {
			const float a = regular ? ang[0] + step * (float)i : ang[i];
			const Vector3 target = c + (u * std::cos(a) + w * std::sin(a)) * r;
			m.pos[g[i]] = m.pos[g[i]].lerp(target, std::max(0.0f, std::min(1.0f, influence)));
		}
		moved++;
	}
	return moved;
}

} // namespace ops

namespace sel {

// Select Linked with delimiters (bit 1 normal, 2 material, 4 seam, 8 sharp, 16 uv).
std::vector<int> linked_faces_delimit(const Mesh &m, const Topology &t, const std::vector<int> &seeds, int delimit) {
	std::vector<char> fm(m.faces.size(), 0);
	std::deque<int> q;
	for (int f : seeds) {
		if (f >= 0 && f < (int)fm.size() && !fm[f]) {
			fm[f] = 1;
			q.push_back(f);
		}
	}
	while (!q.empty()) {
		const int f = q.front();
		q.pop_front();
		const Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			const int a = F.v[i], b = F.v[(i + 1) % F.size()];
			const int e = t.find_edge(a, b);
			if (e < 0) {
				continue;
			}
			const EdgeAttr *ea = m.edge_attr(a, b);
			if (((delimit & 4) && ea && ea->seam) || ((delimit & 8) && ea && ea->sharp)) {
				continue;
			}
			for (int g : t.edge_faces[e]) {
				if (fm[g]) {
					continue;
				}
				const Face &G = m.faces[g];
				if ((delimit & 2) && G.material != F.material) {
					continue;
				}
				if ((delimit & 16) && (G.uv_group != F.uv_group || G.manual_uv != F.manual_uv)) {
					continue;
				}
				if ((delimit & 1) && face_normal(m, F).dot(face_normal(m, G)) < 0.9999f) {
					continue;
				}
				fm[g] = 1;
				q.push_back(g);
			}
		}
	}
	std::vector<int> out;
	for (size_t i = 0; i < fm.size(); i++) {
		if (fm[i]) {
			out.push_back((int)i);
		}
	}
	return out;
}

} // namespace sel
} // namespace hewn

namespace hewn {
namespace ops {

// ================================================================== level design: cuts, corridors

namespace {

// The border edges as segments (a cut may add vertices on them: the pieces are still old border).
std::vector<std::pair<Vector3, Vector3>> border_segments(const Mesh &m) {
	std::vector<std::pair<Vector3, Vector3>> out;
	for (const std::vector<int> &loop : border_loops(m)) {
		for (size_t i = 0; i < loop.size(); i++) {
			out.push_back({ m.pos[loop[i]], m.pos[loop[(i + 1) % loop.size()]] });
		}
	}
	return out;
}

float seg_point_dist(const std::pair<Vector3, Vector3> &s, const Vector3 &p) {
	const Vector3 ab = s.second - s.first;
	const float l2 = ab.length_squared();
	const float t = l2 > 1e-12f ? std::min(1.0f, std::max(0.0f, (p - s.first).dot(ab) / l2)) : 0.0f;
	return p.distance_to(s.first + ab * t);
}

float plane_dist(const CutPlane &p, const Vector3 &x) {
	return p.n.dot(x) - p.d;
}

// The face of `m` that has the half edge a -> b.
int face_of_half(const Mesh &m, int a, int b) {
	for (int f = 0; f < (int)m.faces.size(); f++) {
		const Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			if (F.v[i] == a && F.v[(i + 1) % F.size()] == b) {
				return f;
			}
		}
	}
	return -1;
}

// Arc length parameters 0..1 of a closed loop starting at `start` (count + 1 values, the last is 1).
std::vector<float> loop_params(const Mesh &m, const std::vector<int> &loop, int start) {
	const int n = (int)loop.size();
	std::vector<float> t(n + 1, 0.0f);
	for (int i = 0; i < n; i++) {
		t[i + 1] = t[i] + m.pos[loop[(start + i) % n]].distance_to(m.pos[loop[(start + i + 1) % n]]);
	}
	const float total = std::max(1e-9f, t[n]);
	for (float &x : t) {
		x /= total;
	}
	return t;
}

Vector3 loop_point(const Mesh &m, const std::vector<int> &loop, int start, const std::vector<float> &t, float s) {
	const int n = (int)loop.size();
	for (int i = 0; i < n; i++) {
		if (s <= t[i + 1] || i == n - 1) {
			const float w = (s - t[i]) / std::max(1e-9f, t[i + 1] - t[i]);
			return m.pos[loop[(start + i) % n]].lerp(m.pos[loop[(start + i + 1) % n]], std::min(1.0f, std::max(0.0f, w)));
		}
	}
	return m.pos[loop[start]];
}

Vector3 loop_center(const Mesh &m, const std::vector<int> &loop) {
	Vector3 c;
	for (int v : loop) {
		c += m.pos[v];
	}
	return loop.empty() ? c : c / (float)loop.size();
}

} // namespace

std::vector<std::vector<int>> cut_volumes(Mesh &m, const std::vector<int> &p_faces, const std::vector<std::vector<CutPlane>> &p_parts) {
	std::vector<std::vector<int>> out;
	if (m.faces.empty() || p_parts.empty()) {
		return out;
	}
	Vector3 mn = m.pos.empty() ? Vector3() : m.pos[0], mx = mn;
	for (const Vector3 &p : m.pos) {
		mn = mn.min(p);
		mx = mx.max(p);
	}
	const float eps = std::max(1e-6f, (mx - mn).length() * 1e-6f);
	const float snap = 1e-3f; // (a millimeter: what float drift and typed sizes leave between planes)
	const int first_new = (int)m.pos.size();
	const std::vector<std::pair<Vector3, Vector3>> old_border = border_segments(m);
	// the faces that may be cut: the subset (tag 2 marks it while faces are split and removed)
	for (int f = 0; f < (int)m.faces.size(); f++) {
		m.faces[f].tag = p_faces.empty() ? 2 : 0;
	}
	for (int f : p_faces) {
		if (f >= 0 && f < (int)m.faces.size()) {
			m.faces[f].tag = 2;
		}
	}
	bool removed = false;
	std::vector<CutPlane> flush_planes; // (its planes: kept faces split on them are joined back below)
	for (std::vector<CutPlane> planes : p_parts) {
		if (planes.empty()) {
			continue;
		}
		// Every plane onto the nearest face / vertex less than `snap` away (one inside the other planes): no
		// hair-thin strips. Faces on a flush side stay (below); on a depth plane they are cut away.
		for (size_t pi = 0; pi < planes.size(); pi++) {
			CutPlane &p = planes[pi];
			float best = snap;
			float shift = 0.0f;
			// faces along it (a floor a hair under the door's bottom) that reach into the volume
			for (const Face &F : m.faces) {
				if (F.tag == 0 || std::fabs(face_normal(m, F).dot(p.n)) < 0.9999f) {
					continue;
				}
				const float df = plane_dist(p, m.pos[F.v[0]]);
				if (std::fabs(df) >= best || std::fabs(df) <= eps * 0.01f) {
					continue;
				}
				bool reach = true;
				for (size_t pj = 0; pj < planes.size() && reach; pj++) {
					bool some = pj == pi;
					for (int v : F.v) {
						some = some || plane_dist(planes[pj], m.pos[v]) <= snap;
					}
					reach = some;
				}
				if (reach) {
					best = std::fabs(df);
					shift = df;
				}
			}
			for (int v = 0; v < first_new; v++) {
				const float dv = plane_dist(p, m.pos[v]);
				if (std::fabs(dv) >= best || std::fabs(dv) <= eps * 0.01f) {
					continue;
				}
				bool inside = true;
				for (size_t pj = 0; pj < planes.size() && inside; pj++) {
					inside = pj == pi || plane_dist(planes[pj], m.pos[v]) <= snap;
				}
				if (inside) {
					best = std::fabs(dv);
					shift = dv;
				}
			}
			p.d += shift;
			// (faces kept on any of its planes join again below: a floor next to the cut stays one face, flush
			// sides or not)
			flush_planes.push_back(p);
		}
		// faces that may reach into the volume (not all outside one of its planes): tag 1
		for (Face &F : m.faces) {
			if (F.tag == 0) {
				continue;
			}
			bool overlap = true;
			for (const CutPlane &p : planes) {
				bool some_in = false;
				for (int v : F.v) {
					some_in = some_in || plane_dist(p, m.pos[v]) <= eps;
				}
				overlap = overlap && some_in;
			}
			F.tag = overlap ? 1 : 2;
		}
		// split by every plane; what is outside one of them stays
		for (const CutPlane &p : planes) {
			std::vector<int> cand;
			for (int f = 0; f < (int)m.faces.size(); f++) {
				if (m.faces[f].tag == 1) {
					cand.push_back(f);
				}
			}
			if (cand.empty()) {
				break;
			}
			bisect(m, cand, p.n * p.d, p.n, false, false, false);
			for (Face &F : m.faces) {
				if (F.tag == 1 && plane_dist(p, face_center(m, F)) > eps) {
					F.tag = 2;
				}
			}
		}
		// faces lying on a flush side are its border, not inside (the floor under a door, a wall at the corner)
		const float on_tol = std::max(eps * 20.0f, 2e-5f);
		for (Face &F : m.faces) {
			if (F.tag != 1) {
				continue;
			}
			for (const CutPlane &p : planes) {
				if (!p.flush) {
					continue;
				}
				bool on = true;
				for (int v : F.v) {
					on = on && std::fabs(plane_dist(p, m.pos[v])) <= on_tol;
				}
				if (on) {
					F.tag = 2;
					break;
				}
			}
		}
		std::vector<int> del;
		for (int f = 0; f < (int)m.faces.size(); f++) {
			if (m.faces[f].tag == 1) {
				del.push_back(f);
			}
		}
		if (!del.empty()) {
			removed = true;
			delete_faces(m, del);
		}
	}
	for (Face &F : m.faces) {
		F.tag = 0;
	}
	// the splits of neighboring faces can land a hair from each other or from old corners
	weld_new_vertices(m, first_new, std::max(eps * 10.0f, 1e-5f));
	// Faces kept on a flush side (a floor under a corridor) were split along the volume's sides: the pieces
	// join again where both ends of the split are new (the floor stays one face, no extra edges).
	if (!flush_planes.empty()) {
		const float on_tol = std::max(eps * 20.0f, 2e-5f);
		for (int pass = 0; pass < 64; pass++) {
			Topology t;
			t.build(m);
			std::vector<char> on(m.faces.size(), 0), touched(m.faces.size(), 0), del(m.faces.size(), 0);
			for (size_t f = 0; f < m.faces.size(); f++) {
				for (const CutPlane &p : flush_planes) {
					bool all = true;
					for (int v : m.faces[f].v) {
						all = all && std::fabs(plane_dist(p, m.pos[v])) <= on_tol;
					}
					if (all) {
						on[f] = 1;
						break;
					}
				}
			}
			bool changed = false;
			for (size_t e = 0; e < t.edges.size(); e++) {
				const std::vector<int> &ef = t.edge_faces[e];
				if (ef.size() != 2 || t.edges[e].a < first_new || t.edges[e].b < first_new) {
					continue;
				}
				const int f = ef[0], g = ef[1];
				if (f == g || !on[f] || !on[g] || touched[f] || touched[g] || m.faces[f].material != m.faces[g].material ||
						face_normal(m, m.faces[f]).dot(face_normal(m, m.faces[g])) < 0.9999f) {
					continue;
				}
				Face merged;
				if (!merge_boundary(m, { f, g }, merged)) {
					continue;
				}
				m.faces[f] = merged;
				del[g] = 1;
				touched[f] = touched[g] = 1;
				changed = true;
			}
			if (!changed) {
				break;
			}
			std::vector<int> dl;
			for (size_t f = 0; f < del.size(); f++) {
				if (del[f]) {
					dl.push_back((int)f);
				}
			}
			delete_faces(m, dl);
		}
		// split points left in the middle of a straight side, used by that face alone
		std::vector<int> uses(m.pos.size(), 0);
		for (const Face &F : m.faces) {
			for (int v : F.v) {
				uses[v]++;
			}
		}
		for (Face &F : m.faces) {
			for (int i = 0; i < F.size() && F.size() > 3;) {
				const int v = F.v[i];
				const Vector3 a = m.pos[F.v[(i + F.size() - 1) % F.size()]], b = m.pos[F.v[(i + 1) % F.size()]];
				if (v >= first_new && uses[v] == 1 && (m.pos[v] - a).normalized().dot((b - m.pos[v]).normalized()) > 0.99999f) {
					F.erase_corner(i);
					continue;
				}
				i++;
			}
		}
	}
	if (!removed) {
		return out;
	}
	// the new holes
	for (const std::vector<int> &loop : border_loops(m)) {
		bool fresh = false;
		const float tol = eps * 10.0f;
		for (size_t i = 0; i < loop.size() && !fresh; i++) {
			const Vector3 a = m.pos[loop[i]], b = m.pos[loop[(i + 1) % loop.size()]];
			bool old = false;
			for (const std::pair<Vector3, Vector3> &sg : old_border) {
				if (seg_point_dist(sg, a) <= tol && seg_point_dist(sg, b) <= tol) {
					old = true;
					break;
				}
			}
			fresh = !old;
		}
		if (fresh && loop.size() >= 3) {
			out.push_back(loop);
		}
	}
	return out;
}

std::vector<int> bridge_loops(Mesh &m, const std::vector<int> &p_a, const std::vector<int> &p_b) {
	std::vector<int> out;
	if (p_a.size() < 3 || p_b.size() < 3) {
		return out;
	}
	// A in the winding of its faces, B the other way round (the loops are hole caps, see border_loops)
	const std::vector<int> A(p_a.rbegin(), p_a.rend());
	const std::vector<int> &B = p_b;
	const int na = (int)A.size(), nb = (int)B.size();
	const std::vector<float> ta = loop_params(m, A, 0);
	const Vector3 ca = loop_center(m, A), cb = loop_center(m, B);
	// where B starts: the best match of the two outlines around their centers
	int best_s = 0;
	float best_cost = 1e30f;
	for (int s = 0; s < nb; s++) {
		const std::vector<float> tb = loop_params(m, B, s);
		float cost = 0.0f;
		for (int i = 0; i < na; i++) {
			cost += ((m.pos[A[i]] - ca) - (loop_point(m, B, s, tb, ta[i]) - cb)).length();
		}
		if (cost < best_cost) {
			best_cost = cost;
			best_s = s;
		}
	}
	const std::vector<float> tb = loop_params(m, B, best_s);
	auto a = [&](int i) { return A[i % na]; };
	auto b = [&](int k) { return B[(best_s + k) % nb]; };
	const int fa = face_of_half(m, A[0], A[1 % na]);
	const Face tmpl = fa >= 0 ? m.faces[fa] : Face();
	auto add = [&](const std::vector<int> &v) {
		Face f = make_face(v, fa >= 0 ? &tmpl : nullptr);
		f.manual_uv = false;
		if (clean_face(f)) {
			out.push_back((int)m.faces.size());
			m.faces.push_back(f);
		}
	};
	// zip the two loops by their arc length: quads where both advance, triangles otherwise
	int i = 0, k = 0;
	while (i < na || k < nb) {
		const float next_a = i < na ? ta[i + 1] : 2.0f;
		const float next_b = k < nb ? tb[k + 1] : 2.0f;
		if (i < na && k < nb && std::fabs(next_a - next_b) < 1e-4f) {
			add({ a(i + 1), a(i), b(k), b(k + 1) });
			i++;
			k++;
		} else if (next_a < next_b) {
			add({ a(i + 1), a(i), b(k) });
			i++;
		} else {
			add({ a(i), b(k), b(k + 1) });
			k++;
		}
	}
	quadify(m, 1.0f, &out);
	return out;
}

std::vector<int> alcove(Mesh &m, const std::vector<int> &p_loop, float p_depth, const std::vector<CutPlane> *p_planes) {
	std::vector<int> out;
	if (p_loop.size() < 3) {
		return out;
	}
	const std::vector<int> A(p_loop.rbegin(), p_loop.rend()); // the winding of the faces around the hole
	const int n = (int)A.size();
	// into the wall: against the faces around the hole
	Vector3 nrm;
	int tf = -1;
	for (int i = 0; i < n; i++) {
		const int f = face_of_half(m, A[i], A[(i + 1) % n]);
		if (f >= 0) {
			nrm += face_normal(m, m.faces[f]);
			tf = tf < 0 ? f : tf;
		}
	}
	// Straight into the hole's own plane: the faces around it can include another wall or the floor (a cut
	// against the room's edge), and their average would push the alcove askew. Their sum only picks the side.
	{
		std::vector<Vector3> p;
		for (int v : p_loop) {
			p.push_back(m.pos[v]);
		}
		const Vector3 plane = newell(p);
		if (plane.length() > 1e-6f) {
			nrm = plane.dot(nrm) < 0.0f ? -plane : plane;
		}
	}
	if (nrm.length() < 1e-6f) {
		nrm = Vector3(0, 1, 0);
	}
	Vector3 dir = -nrm.normalized();
	// the volume's side most into the wall: the alcove along it, up to it
	const CutPlane *back = nullptr;
	if (p_planes) {
		float best = 0.3f;
		for (const CutPlane &cp : *p_planes) {
			const float a = cp.n.dot(dir);
			if (a > best) {
				best = a;
				back = &cp;
			}
		}
	}
	if (back) {
		dir = back->n;
	}
	const Face tmpl = tf >= 0 ? m.faces[tf] : Face();
	std::vector<int> B(n);
	for (int i = 0; i < n; i++) {
		const Vector3 &p = m.pos[A[i]];
		float t = p_depth;
		if (back) {
			const float to_back = back->d - back->n.dot(p);
			t = to_back > 1e-3f ? to_back : p_depth;
		}
		B[i] = m.add_vertex(p + dir * t);
	}
	for (int i = 0; i < n; i++) {
		const int j = (i + 1) % n;
		Face f = make_face({ A[j], A[i], B[i], B[j] }, tf >= 0 ? &tmpl : nullptr);
		f.manual_uv = false;
		out.push_back((int)m.faces.size());
		m.faces.push_back(f);
	}
	Face c = make_face(std::vector<int>(B.rbegin(), B.rend()), tf >= 0 ? &tmpl : nullptr);
	c.manual_uv = false;
	out.push_back((int)m.faces.size());
	m.faces.push_back(c);
	return out;
}

std::vector<std::vector<int>> holes(const Mesh &m) {
	return border_loops(m);
}

std::vector<int> fill_loop(Mesh &m, const std::vector<int> &p_loop) {
	std::vector<int> out;
	if (p_loop.size() < 3) {
		return out;
	}
	const int f = face_of_half(m, p_loop[1], p_loop[0]);
	const Face tmpl = f >= 0 ? m.faces[f] : Face();
	Face nf = make_face(p_loop, f >= 0 ? &tmpl : nullptr);
	nf.manual_uv = false;
	out.push_back((int)m.faces.size());
	m.faces.push_back(nf);
	return out;
}


// Prisms over a face (its triangles when it is not convex): its surface and a hair on both sides.
static std::vector<std::vector<CutPlane>> face_prisms(const Mesh &m, const Face &F, float p_margin) {
	std::vector<std::vector<CutPlane>> out;
	const Vector3 n = face_normal(m, F);
	std::vector<Vector3> pts;
	for (int v : F.v) {
		pts.push_back(m.pos[v]);
	}
	const int c = (int)pts.size();
	if (c < 3 || n.length_squared() < 0.5f) {
		return out;
	}
	bool convex = true;
	for (int i = 0; i < c; i++) {
		const Vector3 e0 = pts[(i + 1) % c] - pts[i], e1 = pts[(i + 2) % c] - pts[(i + 1) % c];
		convex = convex && e0.cross(e1).dot(n) >= -1e-7f;
	}
	std::vector<std::vector<Vector3>> polys;
	if (convex) {
		polys.push_back(pts);
	} else {
		std::vector<int> tris;
		triangulate(pts, n, tris);
		for (size_t k = 0; k + 2 < tris.size(); k += 3) {
			polys.push_back({ pts[tris[k]], pts[tris[k + 1]], pts[tris[k + 2]] });
		}
	}
	const float d = n.dot(pts[0]);
	for (const std::vector<Vector3> &poly : polys) {
		std::vector<CutPlane> planes;
		for (size_t i = 0; i < poly.size(); i++) {
			const Vector3 a = poly[i], b = poly[(i + 1) % poly.size()];
			const Vector3 out_n = (b - a).cross(n);
			if (out_n.length() < 1e-9f) {
				continue;
			}
			planes.push_back({ out_n.normalized(), out_n.normalized().dot(a) });
		}
		planes.push_back({ n, d + p_margin, false });
		planes.push_back({ -n, -d + p_margin, false });
		out.push_back(planes);
	}
	return out;
}

void merge_touching(Mesh &m, Mesh &add, float eps) {
	std::vector<int> old_f, new_f;
	std::vector<char> old_mark(m.faces.size(), 0);
	for (int i = 0; i < (int)add.faces.size(); i++) {
		const Face &A = add.faces[i];
		const Vector3 na = face_normal(add, A);
		const float da = na.dot(add.pos[A.v[0]]);
		godot::AABB ba(add.pos[A.v[0]], Vector3());
		for (int v : A.v) {
			ba.expand_to(add.pos[v]);
		}
		bool any = false;
		for (int j = 0; j < (int)m.faces.size(); j++) {
			const Face &B = m.faces[j];
			if (face_normal(m, B).dot(na) > -0.999f) {
				continue; // (only faces turned against each other)
			}
			bool on = true;
			godot::AABB bb(m.pos[B.v[0]], Vector3());
			for (int v : B.v) {
				on = on && std::fabs(na.dot(m.pos[v]) - da) < eps;
				bb.expand_to(m.pos[v]);
			}
			if (on && ba.grow(eps).intersects(bb.grow(eps))) {
				any = true;
				if (!old_mark[j]) {
					old_mark[j] = 1;
					old_f.push_back(j);
				}
			}
		}
		if (any) {
			new_f.push_back(i);
		}
	}
	if (new_f.empty()) {
		return;
	}
	// each side cut away where the other one lies on it
	std::vector<std::vector<CutPlane>> by_new, by_old;
	for (int i : new_f) {
		const std::vector<std::vector<CutPlane>> p = face_prisms(add, add.faces[i], eps);
		by_new.insert(by_new.end(), p.begin(), p.end());
	}
	for (int j : old_f) {
		const std::vector<std::vector<CutPlane>> p = face_prisms(m, m.faces[j], eps);
		by_old.insert(by_old.end(), p.begin(), p.end());
	}
	cut_volumes(m, old_f, by_new);
	cut_volumes(add, new_f, by_old);
}

} // namespace ops
} // namespace hewn
