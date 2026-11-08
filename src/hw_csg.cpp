// Mesh booleans with a BSP tree (after csg.js by Evan Wallace), followed by a cleanup that welds the
// fragments, removes T-junctions and merges coplanar pieces of the same source face back into n-gons.
#include "hw_core.h"

#include <memory>

namespace hewn {
namespace csg {

namespace {

const float EPS = 1e-5f;

// Work budget: nearly coplanar input can make the BSP fragment explosively; give up instead of hanging.
const size_t WORK_LIMIT = 4000000;
thread_local size_t g_work = 0;
inline bool over_budget() {
	return g_work > WORK_LIMIT;
}

struct V {
	Vector3 p;
	Vector2 uv;
	Color c;
	V lerp(const V &o, float t) const { return V{ p.lerp(o.p, t), uv.lerp(o.uv, t), c.lerp(o.c, t) }; }
};

struct Plane {
	Vector3 n;
	float w = 0.0f;
	void flip() {
		n = -n;
		w = -w;
	}
};

struct Poly {
	std::vector<V> v;
	Plane plane;
	int src = 0;
	void flip() {
		std::reverse(v.begin(), v.end());
		plane.flip();
	}
};

bool make_plane(const std::vector<V> &v, Plane &out) {
	std::vector<Vector3> pts;
	for (const V &x : v) {
		pts.push_back(x.p);
	}
	const Vector3 n = newell(pts);
	const float l = n.length();
	if (l < 1e-12f) {
		return false;
	}
	out.n = n / l;
	out.w = out.n.dot(v[0].p);
	return true;
}

enum {
	COPLANAR = 0,
	FRONT = 1,
	BACK = 2,
	SPANNING = 3,
};

void split_poly(const Plane &pl, const Poly &poly, std::vector<Poly> &cofront, std::vector<Poly> &coback, std::vector<Poly> &front, std::vector<Poly> &back) {
	g_work++;
	int ptype = 0;
	std::vector<int> types(poly.v.size());
	for (size_t i = 0; i < poly.v.size(); i++) {
		const float t = pl.n.dot(poly.v[i].p) - pl.w;
		const int type = t < -EPS ? BACK : (t > EPS ? FRONT : COPLANAR);
		ptype |= type;
		types[i] = type;
	}
	switch (ptype) {
		case COPLANAR:
			(pl.n.dot(poly.plane.n) > 0 ? cofront : coback).push_back(poly);
			break;
		case FRONT:
			front.push_back(poly);
			break;
		case BACK:
			back.push_back(poly);
			break;
		default: {
			Poly f, b;
			f.plane = b.plane = poly.plane;
			f.src = b.src = poly.src;
			const size_t n = poly.v.size();
			for (size_t i = 0; i < n; i++) {
				const size_t j = (i + 1) % n;
				const int ti = types[i], tj = types[j];
				const V &vi = poly.v[i], &vj = poly.v[j];
				if (ti != BACK) {
					f.v.push_back(vi);
				}
				if (ti != FRONT) {
					b.v.push_back(vi);
				}
				if ((ti | tj) == SPANNING) {
					const float t = (pl.w - pl.n.dot(vi.p)) / pl.n.dot(vj.p - vi.p);
					const V x = vi.lerp(vj, t);
					f.v.push_back(x);
					b.v.push_back(x);
				}
			}
			if (f.v.size() >= 3) {
				front.push_back(f);
			}
			if (b.v.size() >= 3) {
				back.push_back(b);
			}
		} break;
	}
}

struct Node {
	Plane plane;
	bool has_plane = false;
	std::unique_ptr<Node> front, back;
	std::vector<Poly> polys;

	Node() = default;
	// (freed without recursion: a deep tree from nearly coplanar input would run out of stack)
	~Node() {
		std::vector<std::unique_ptr<Node>> stack;
		if (front) {
			stack.push_back(std::move(front));
		}
		if (back) {
			stack.push_back(std::move(back));
		}
		while (!stack.empty()) {
			std::unique_ptr<Node> n = std::move(stack.back());
			stack.pop_back();
			if (n->front) {
				stack.push_back(std::move(n->front));
			}
			if (n->back) {
				stack.push_back(std::move(n->back));
			}
		}
	}

	template <typename F>
	void each(F fn) {
		std::vector<Node *> stack = { this };
		while (!stack.empty()) {
			Node *n = stack.back();
			stack.pop_back();
			fn(n);
			if (n->front) {
				stack.push_back(n->front.get());
			}
			if (n->back) {
				stack.push_back(n->back.get());
			}
		}
	}

	void invert() {
		each([](Node *n) {
			for (Poly &p : n->polys) {
				p.flip();
			}
			n->plane.flip();
			std::swap(n->front, n->back);
		});
	}

	std::vector<Poly> clip_polys(const std::vector<Poly> &in) const {
		std::vector<Poly> result;
		std::vector<std::pair<const Node *, std::vector<Poly>>> stack;
		stack.push_back({ this, in });
		while (!stack.empty() && !over_budget()) {
			const Node *n = stack.back().first;
			std::vector<Poly> list = std::move(stack.back().second);
			stack.pop_back();
			if (!n->has_plane) {
				result.insert(result.end(), list.begin(), list.end());
				continue;
			}
			std::vector<Poly> f, b;
			for (const Poly &p : list) {
				split_poly(n->plane, p, f, b, f, b);
			}
			if (n->front) {
				stack.push_back({ n->front.get(), std::move(f) });
			} else {
				result.insert(result.end(), f.begin(), f.end());
			}
			if (n->back) {
				stack.push_back({ n->back.get(), std::move(b) });
			}
		}
		return result;
	}

	void clip_to(const Node &bsp) {
		each([&](Node *n) { n->polys = bsp.clip_polys(n->polys); });
	}

	std::vector<Poly> all() {
		std::vector<Poly> out;
		each([&](Node *n) { out.insert(out.end(), n->polys.begin(), n->polys.end()); });
		return out;
	}

	void build(const std::vector<Poly> &in) {
		std::vector<std::pair<Node *, std::vector<Poly>>> stack;
		stack.push_back({ this, in });
		while (!stack.empty() && !over_budget()) {
			Node *n = stack.back().first;
			std::vector<Poly> list = std::move(stack.back().second);
			stack.pop_back();
			if (list.empty()) {
				continue;
			}
			if (!n->has_plane) {
				n->plane = list[0].plane;
				n->has_plane = true;
			}
			std::vector<Poly> f, b;
			for (const Poly &p : list) {
				split_poly(n->plane, p, n->polys, n->polys, f, b);
			}
			if (!f.empty()) {
				if (!n->front) {
					n->front = std::make_unique<Node>();
				}
				stack.push_back({ n->front.get(), std::move(f) });
			}
			if (!b.empty()) {
				if (!n->back) {
					n->back = std::make_unique<Node>();
				}
				stack.push_back({ n->back.get(), std::move(b) });
			}
		}
	}
};

void to_polys(const Mesh &m, int src_offset, std::vector<Poly> &out) {
	std::vector<Vector3> pts;
	std::vector<int> tris;
	for (size_t f = 0; f < m.faces.size(); f++) {
		Face F = m.faces[f];
		F.ensure_attrs();
		face_points(m, F, pts);
		tris.clear();
		triangulate(pts, newell(pts), tris);
		for (size_t k = 0; k + 2 < tris.size(); k += 3) {
			Poly p;
			for (int c = 0; c < 3; c++) {
				const int i = tris[k + c];
				p.v.push_back(V{ pts[i], F.uv[i], F.col[i] });
			}
			if (!make_plane(p.v, p.plane)) {
				continue;
			}
			p.src = src_offset + (int)f;
			out.push_back(p);
		}
	}
}

bool same_uvp(const UVParams &a, const UVParams &b) {
	return a.scale == b.scale && a.offset == b.offset && a.rotation == b.rotation && a.fill == b.fill && a.world == b.world &&
			a.flip_u == b.flip_u && a.flip_v == b.flip_v && a.swap == b.swap && a.face_aligned == b.face_aligned;
}

// Pieces of one source face always merge; pieces of different faces merge when coplanar with equal auto-UV attributes.
bool compatible(const Mesh &m, int f, int g, const std::vector<int> &src, const std::vector<char> &locked) {
	if (src[f] == src[g]) {
		return true;
	}
	auto is_locked = [&](int s) { return s >= 0 && s < (int)locked.size() && locked[s]; };
	if (is_locked(src[f]) || is_locked(src[g])) {
		return false; // (faces away from the operation keep their outlines: no merging into the neighbors)
	}
	const Face &A = m.faces[f], &B = m.faces[g];
	if (A.manual_uv || B.manual_uv || A.material != B.material || A.smooth != B.smooth || A.uv_group != B.uv_group || !same_uvp(A.uvp, B.uvp)) {
		return false;
	}
	const Vector3 na = face_normal(m, A), nb = face_normal(m, B);
	if (na.dot(nb) < 0.99999f) {
		return false;
	}
	return std::fabs(na.dot(face_center(m, B) - face_center(m, A))) < 1e-4f;
}

// Merges adjacent faces of the same source face while the union stays a simple ring.
void merge_fragments(Mesh &m, std::vector<int> &src, const std::vector<char> &locked) {
	for (int pass = 0; pass < 64; pass++) {
		Topology t;
		t.build(m);
		std::vector<char> touched(m.faces.size(), 0), del(m.faces.size(), 0);
		bool changed = false;
		for (size_t e = 0; e < t.edges.size(); e++) {
			const std::vector<int> &ef = t.edge_faces[e];
			if (ef.size() != 2) {
				continue;
			}
			const int f = ef[0], g = ef[1];
			if (f == g || touched[f] || touched[g] || !compatible(m, f, g, src, locked)) {
				continue;
			}
			// Union of the two rings, walking the half-edges that have no twin inside the pair.
			std::unordered_set<uint64_t> halves;
			for (int x : { f, g }) {
				const Face &F = m.faces[x];
				for (int i = 0; i < F.size(); i++) {
					halves.insert(hkey(F.v[i], F.v[(i + 1) % F.size()]));
				}
			}
			struct H {
				int b, f, i;
			};
			std::unordered_map<int, H> next;
			bool ok = true;
			int start = -1, count = 0;
			for (int x : { f, g }) {
				const Face &F = m.faces[x];
				for (int i = 0; i < F.size() && ok; i++) {
					const int a = F.v[i], b = F.v[(i + 1) % F.size()];
					if (halves.count(hkey(b, a))) {
						continue;
					}
					if (next.count(a)) {
						ok = false;
						break;
					}
					next[a] = H{ b, x, i };
					count++;
					start = a;
				}
			}
			if (!ok || count < 3) {
				continue;
			}
			Face nf;
			nf.copy_attrs_from(m.faces[f]);
			nf.manual_uv = m.faces[f].manual_uv;
			int v = start;
			for (int guard = 0; guard <= count; guard++) {
				auto it = next.find(v);
				if (it == next.end()) {
					ok = false;
					break;
				}
				const Face &F = m.faces[it->second.f];
				nf.v.push_back(v);
				nf.uv.push_back(F.uv[it->second.i]);
				nf.col.push_back(F.col[it->second.i]);
				v = it->second.b;
				if (v == start) {
					break;
				}
			}
			if (!ok || v != start || nf.size() != count) {
				continue;
			}
			m.faces[f] = nf;
			del[g] = 1;
			touched[f] = touched[g] = 1;
			changed = true;
		}
		if (!changed) {
			break;
		}
		std::vector<int> nsrc;
		for (size_t i = 0; i < src.size(); i++) {
			if (!del[i]) {
				nsrc.push_back(src[i]);
			}
		}
		delete_faces_mask(m, del);
		src.swap(nsrc);
	}
}

} // namespace

Mesh boolean(const Mesh &a, const Mesh &b, Op op, bool *r_ok) {
	g_work = 0;
	if (r_ok) {
		*r_ok = false;
	}
	// A flat or empty operand has no inside: the boolean is meaningless.
	auto extent_ok = [](const Mesh &m) {
		if (m.pos.empty() || m.faces.empty()) {
			return false;
		}
		Vector3 mn = m.pos[0], mx = m.pos[0];
		for (const Vector3 &p : m.pos) {
			mn = mn.min(p);
			mx = mx.max(p);
		}
		const Vector3 e = mx - mn;
		const float size = e.length();
		return size > 1e-6f && std::min(e.x, std::min(e.y, e.z)) > size * 1e-5f;
	};
	if (!extent_ok(a) || !extent_ok(b)) {
		return a;
	}
	std::vector<Poly> pa, pb;
	to_polys(a, 0, pa);
	to_polys(b, (int)a.faces.size(), pb);
	std::vector<const Face *> attrs;
	for (const Face &f : a.faces) {
		attrs.push_back(&f);
	}
	for (const Face &f : b.faces) {
		attrs.push_back(&f);
	}

	Node A, B;
	A.build(pa);
	B.build(pb);
	switch (op) {
		case UNION:
			A.clip_to(B);
			B.clip_to(A);
			B.invert();
			B.clip_to(A);
			B.invert();
			A.build(B.all());
			break;
		case SUBTRACT:
			A.invert();
			A.clip_to(B);
			B.clip_to(A);
			B.invert();
			B.clip_to(A);
			B.invert();
			A.build(B.all());
			A.invert();
			break;
		case INTERSECT:
			A.invert();
			B.clip_to(A);
			B.invert();
			A.clip_to(B);
			B.clip_to(A);
			A.build(B.all());
			A.invert();
			break;
	}
	if (over_budget()) {
		return a;
	}
	const std::vector<Poly> result = A.all();

	// Faces of `a` away from `b` (their box misses b's): the BSP only re-splits them; they are taken from
	// `a` as they were (union / subtract keep them, intersect drops them), so the untouched part of the
	// mesh keeps its exact vertices and outlines.
	godot::AABB bb_b(b.pos.empty() ? Vector3() : b.pos[0], Vector3());
	for (const Vector3 &p : b.pos) {
		bb_b.expand_to(p);
	}
	bb_b = bb_b.grow(1e-4f);
	std::vector<char> far(a.faces.size(), 0);
	for (size_t f = 0; f < a.faces.size(); f++) {
		const Face &F = a.faces[f];
		godot::AABB fb(a.pos[F.v[0]], Vector3());
		for (int v : F.v) {
			fb.expand_to(a.pos[v]);
		}
		far[f] = !fb.intersects(bb_b);
	}

	Mesh out;
	std::vector<int> src;
	Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
	if (op != INTERSECT) {
		for (size_t f = 0; f < a.faces.size(); f++) {
			if (!far[f]) {
				continue;
			}
			Face F = a.faces[f];
			F.ensure_attrs();
			Face nf = F;
			nf.v.clear();
			for (int v : F.v) {
				nf.v.push_back(out.add_vertex(a.pos[v]));
				mn = mn.min(a.pos[v]);
				mx = mx.max(a.pos[v]);
			}
			out.faces.push_back(nf);
			src.push_back((int)f);
		}
	}
	for (const Poly &p : result) {
		if (p.src < (int)a.faces.size() && far[p.src]) {
			continue; // (taken whole above)
		}
		const Face &attr = *attrs[p.src];
		Face f;
		f.copy_attrs_from(attr);
		f.manual_uv = attr.manual_uv;
		for (const V &x : p.v) {
			f.v.push_back(out.add_vertex(x.p));
			f.uv.push_back(x.uv);
			f.col.push_back(x.c);
			mn = mn.min(x.p);
			mx = mx.max(x.p);
		}
		out.faces.push_back(f);
		src.push_back(p.src);
	}
	if (out.faces.empty()) {
		if (r_ok) {
			*r_ok = true; // e.g. subtracting something that swallows everything
		}
		return out;
	}
	const float scale = std::max(1e-3f, (mx - mn).length());
	const float eps = scale * 1e-5f;
	weld_by_distance(out, eps);
	std::vector<int> remap;
	cleanup(out, &remap);
	{
		std::vector<int> nsrc(out.faces.size());
		for (size_t i = 0; i < remap.size(); i++) {
			if (remap[i] >= 0) {
				nsrc[remap[i]] = src[i];
			}
		}
		src.swap(nsrc);
	}
	ops::repair_tjunctions(out, eps * 4.0f);
	merge_fragments(out, src, far);
	cleanup(out);
	// the corners the splitting left on straight edges (two edges at them, in line): gone; the inputs' own stay
	{
		std::vector<Vector3> keep_at(a.pos.begin(), a.pos.end());
		keep_at.insert(keep_at.end(), b.pos.begin(), b.pos.end());
		std::sort(keep_at.begin(), keep_at.end(), [](const Vector3 &x, const Vector3 &y) { return x.x < y.x; });
		Topology t;
		t.build(out);
		std::vector<char> drop(out.pos.size(), 0);
		bool any = false;
		for (int v = 0; v < (int)out.pos.size(); v++) {
			if (v >= (int)t.vert_edges.size() || t.vert_edges[v].size() != 2) {
				continue;
			}
			const Edge &e0 = t.edges[t.vert_edges[v][0]], &e1 = t.edges[t.vert_edges[v][1]];
			const int p = e0.a == v ? e0.b : e0.a, q = e1.a == v ? e1.b : e1.a;
			const Vector3 P = out.pos[p], Q = out.pos[q], X = out.pos[v];
			const Vector3 pq = Q - P;
			const float L2 = pq.length_squared();
			if (L2 < eps * eps) {
				continue;
			}
			const float u = (X - P).dot(pq) / L2;
			if (u <= 0.0f || u >= 1.0f || (P + pq * u).distance_to(X) > eps * 4.0f) {
				continue;
			}
			bool original = false;
			auto it = std::lower_bound(keep_at.begin(), keep_at.end(), X.x - eps * 4.0f, [](const Vector3 &k, float x) { return k.x < x; });
			for (; it != keep_at.end() && it->x <= X.x + eps * 4.0f && !original; ++it) {
				original = it->distance_to(X) <= eps * 4.0f;
			}
			if (!original) {
				drop[v] = 1;
				any = true;
			}
		}
		if (any) {
			for (Face &F : out.faces) {
				Face nf = F;
				nf.v.clear();
				nf.uv.clear();
				nf.col.clear();
				F.ensure_attrs();
				for (int i = 0; i < F.size(); i++) {
					if (!drop[F.v[i]]) {
						nf.v.push_back(F.v[i]);
						nf.uv.push_back(F.uv[i]);
						nf.col.push_back(F.col[i]);
					}
				}
				F = nf;
			}
			cleanup(out);
		}
	}
	remove_unused_vertices(out);
	if (r_ok) {
		*r_ok = true;
	}
	return out;
}

} // namespace csg
} // namespace hewn
