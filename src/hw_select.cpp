// Selection queries: conversion between element modes, grow/shrink, loops and rings.
#include "hw_core.h"

#include <deque>

namespace hewn {
namespace sel {

std::vector<int> uniq(std::vector<int> v) {
	std::sort(v.begin(), v.end());
	v.erase(std::unique(v.begin(), v.end()), v.end());
	return v;
}

std::vector<Edge> uniq(std::vector<Edge> v) {
	std::sort(v.begin(), v.end());
	v.erase(std::unique(v.begin(), v.end()), v.end());
	return v;
}

static std::vector<char> mask_of(const std::vector<int> &s, size_t n) {
	std::vector<char> m(n, 0);
	for (int i : s) {
		if (i >= 0 && i < (int)n) {
			m[i] = 1;
		}
	}
	return m;
}

static std::vector<int> from_mask(const std::vector<char> &m) {
	std::vector<int> out;
	for (size_t i = 0; i < m.size(); i++) {
		if (m[i]) {
			out.push_back((int)i);
		}
	}
	return out;
}

// ------------------------------------------------------------------ conversion

std::vector<int> verts_of_faces(const Mesh &m, const std::vector<int> &faces) {
	std::vector<int> out;
	for (int f : faces) {
		if (f < 0 || f >= (int)m.faces.size()) {
			continue;
		}
		out.insert(out.end(), m.faces[f].v.begin(), m.faces[f].v.end());
	}
	return uniq(out);
}

std::vector<int> verts_of_edges(const std::vector<Edge> &edges) {
	std::vector<int> out;
	out.reserve(edges.size() * 2);
	for (const Edge &e : edges) {
		out.push_back(e.a);
		out.push_back(e.b);
	}
	return uniq(out);
}

std::vector<Edge> edges_of_faces(const Mesh &m, const std::vector<int> &faces) {
	std::vector<Edge> out;
	for (int f : faces) {
		if (f < 0 || f >= (int)m.faces.size()) {
			continue;
		}
		const Face &F = m.faces[f];
		const int n = F.size();
		for (int i = 0; i < n; i++) {
			out.push_back(Edge(F.v[i], F.v[(i + 1) % n]));
		}
	}
	return uniq(out);
}

std::vector<Edge> edges_from_verts(const Mesh &m, const Topology &t, const std::vector<int> &verts) {
	const std::vector<char> vm = mask_of(verts, m.pos.size());
	std::vector<Edge> out;
	for (const Edge &e : t.edges) {
		if (vm[e.a] && vm[e.b]) {
			out.push_back(e);
		}
	}
	for (const std::pair<int, int> &w : m.wires) {
		if (vm[w.first] && vm[w.second]) {
			out.push_back(Edge(w.first, w.second));
		}
	}
	return out;
}

std::vector<int> faces_from_verts(const Mesh &m, const std::vector<int> &verts, bool all_corners) {
	const std::vector<char> vm = mask_of(verts, m.pos.size());
	std::vector<int> out;
	for (int f = 0; f < (int)m.faces.size(); f++) {
		const Face &F = m.faces[f];
		int cnt = 0;
		for (int v : F.v) {
			cnt += vm[v] ? 1 : 0;
		}
		if (all_corners ? cnt == F.size() : cnt > 0) {
			out.push_back(f);
		}
	}
	return out;
}

std::vector<int> all_verts(const Mesh &m) {
	std::vector<int> out(m.pos.size());
	for (size_t i = 0; i < out.size(); i++) {
		out[i] = (int)i;
	}
	return out;
}

std::vector<Edge> all_edges(const Mesh &m, const Topology &t) {
	std::vector<Edge> out = t.edges;
	for (const std::pair<int, int> &w : m.wires) {
		out.push_back(Edge(w.first, w.second));
	}
	return out;
}

std::vector<int> all_faces(const Mesh &m) {
	std::vector<int> out(m.faces.size());
	for (size_t i = 0; i < out.size(); i++) {
		out[i] = (int)i;
	}
	return out;
}

std::vector<Edge> border_edges(const Mesh &m, const Topology &t) {
	(void)m;
	std::vector<Edge> out;
	for (size_t e = 0; e < t.edges.size(); e++) {
		if (t.edge_faces[e].size() == 1) {
			out.push_back(t.edges[e]);
		}
	}
	return out;
}

// ------------------------------------------------------------------ grow / shrink

std::vector<int> grow_verts(const Mesh &m, const Topology &t, const std::vector<int> &s) {
	std::vector<char> vm = mask_of(s, m.pos.size());
	std::vector<char> out = vm;
	for (const Edge &e : t.edges) {
		if (vm[e.a]) {
			out[e.b] = 1;
		}
		if (vm[e.b]) {
			out[e.a] = 1;
		}
	}
	return from_mask(out);
}

std::vector<int> shrink_verts(const Mesh &m, const Topology &t, const std::vector<int> &s) {
	std::vector<char> vm = mask_of(s, m.pos.size());
	std::vector<char> out = vm;
	for (const Edge &e : t.edges) {
		if (vm[e.a] != vm[e.b]) {
			out[e.a] = 0;
			out[e.b] = 0;
		}
	}
	return from_mask(out);
}

std::vector<Edge> grow_edges(const Mesh &m, const Topology &t, const std::vector<Edge> &s) {
	// Every edge of a face touching a selected edge.
	std::vector<Edge> out = s;
	for (const Edge &e : s) {
		const int id = t.find_edge(e.a, e.b);
		if (id < 0) {
			continue;
		}
		for (int f : t.edge_faces[id]) {
			const Face &F = m.faces[f];
			const int n = F.size();
			for (int i = 0; i < n; i++) {
				out.push_back(Edge(F.v[i], F.v[(i + 1) % n]));
			}
		}
	}
	return uniq(out);
}

std::vector<Edge> shrink_edges(const Mesh &m, const Topology &t, const std::vector<Edge> &s) {
	std::vector<int> v = shrink_verts(m, t, verts_of_edges(s));
	std::vector<char> vm = mask_of(v, m.pos.size());
	std::vector<Edge> out;
	for (const Edge &e : s) {
		if (vm[e.a] && vm[e.b]) {
			out.push_back(e);
		}
	}
	return out;
}

std::vector<int> grow_faces(const Mesh &m, const Topology &t, const std::vector<int> &s, float max_angle_deg) {
	std::vector<char> fm = mask_of(s, m.faces.size());
	std::vector<char> out = fm;
	const float cos_lim = std::cos(std::min(max_angle_deg, 180.0f) * (float)(godot::Math::PI / 180.0));
	const bool use_angle = max_angle_deg < 180.0f;
	for (int f : s) {
		if (f < 0 || f >= (int)m.faces.size()) {
			continue;
		}
		const Face &F = m.faces[f];
		const Vector3 fn = use_angle ? face_normal(m, F) : Vector3();
		const int n = F.size();
		for (int i = 0; i < n; i++) {
			const int e = t.find_edge(F.v[i], F.v[(i + 1) % n]);
			if (e < 0) {
				continue;
			}
			for (int g : t.edge_faces[e]) {
				if (fm[g]) {
					continue;
				}
				if (use_angle && face_normal(m, m.faces[g]).dot(fn) < cos_lim) {
					continue;
				}
				out[g] = 1;
			}
		}
	}
	return from_mask(out);
}

std::vector<int> shrink_faces(const Mesh &m, const Topology &t, const std::vector<int> &s) {
	std::vector<char> fm = mask_of(s, m.faces.size());
	std::vector<int> out;
	for (int f : uniq(s)) {
		if (f < 0 || f >= (int)m.faces.size()) {
			continue;
		}
		const Face &F = m.faces[f];
		const int n = F.size();
		bool keep = true;
		for (int i = 0; i < n && keep; i++) {
			const int e = t.find_edge(F.v[i], F.v[(i + 1) % n]);
			if (e < 0) {
				continue;
			}
			for (int g : t.edge_faces[e]) {
				if (!fm[g]) {
					keep = false;
					break;
				}
			}
		}
		if (keep) {
			out.push_back(f);
		}
	}
	return out;
}

std::vector<int> linked_verts(const Mesh &m, const Topology &t, const std::vector<int> &s) {
	std::vector<char> vm(m.pos.size(), 0);
	std::deque<int> q;
	for (int v : s) {
		if (v >= 0 && v < (int)vm.size() && !vm[v]) {
			vm[v] = 1;
			q.push_back(v);
		}
	}
	while (!q.empty()) {
		const int v = q.front();
		q.pop_front();
		for (int e : t.vert_edges[v]) {
			const Edge &E = t.edges[e];
			const int o = E.a == v ? E.b : E.a;
			if (!vm[o]) {
				vm[o] = 1;
				q.push_back(o);
			}
		}
	}
	return from_mask(vm);
}

std::vector<int> linked_faces(const Mesh &m, const Topology &t, const std::vector<int> &s) {
	std::vector<char> fm(m.faces.size(), 0);
	std::deque<int> q;
	for (int f : s) {
		if (f >= 0 && f < (int)fm.size() && !fm[f]) {
			fm[f] = 1;
			q.push_back(f);
		}
	}
	while (!q.empty()) {
		const int f = q.front();
		q.pop_front();
		for (int v : m.faces[f].v) {
			for (int g : t.vert_faces[v]) {
				if (!fm[g]) {
					fm[g] = 1;
					q.push_back(g);
				}
			}
		}
	}
	return from_mask(fm);
}

// ------------------------------------------------------------------ loops & rings

static bool is_border(const Topology &t, int e) {
	return t.edge_faces[e].size() == 1;
}

// Next edge of an edge loop continuing through vertex `v` from edge `e`. -1 to stop.
static int loop_next(const Topology &t, int e, int v) {
	const std::vector<int> &ve = t.vert_edges[v];
	if (is_border(t, e)) {
		// Follow the boundary: the only other border edge at v.
		int found = -1;
		int count = 0;
		for (int o : ve) {
			if (o != e && is_border(t, o)) {
				found = o;
				count++;
			}
		}
		return count == 1 ? found : -1;
	}
	if (ve.size() != 4) {
		return -1;
	}
	int found = -1;
	for (int o : ve) {
		if (o == e || t.shares_face(o, e)) {
			continue;
		}
		if (found >= 0) {
			return -1;
		}
		found = o;
	}
	return found;
}

std::vector<Edge> edge_loop(const Mesh &m, const Topology &t, const Edge &start) {
	(void)m;
	std::vector<Edge> out;
	const int e0 = t.find_edge(start.a, start.b);
	if (e0 < 0) {
		return out;
	}
	std::vector<char> seen(t.edges.size(), 0);
	seen[e0] = 1;
	out.push_back(t.edges[e0]);
	const int ends[2] = { t.edges[e0].a, t.edges[e0].b };
	for (int side = 0; side < 2; side++) {
		int e = e0;
		int v = ends[side];
		while (true) {
			const int n = loop_next(t, e, v);
			if (n < 0 || seen[n]) {
				break;
			}
			seen[n] = 1;
			out.push_back(t.edges[n]);
			const Edge &E = t.edges[n];
			v = E.a == v ? E.b : E.a;
			e = n;
		}
	}
	return out;
}

// Opposite edge of `e` inside quad face `f`, or -1.
static int quad_opposite(const Mesh &m, const Topology &t, int f, int e) {
	const Face &F = m.faces[f];
	if (F.size() != 4) {
		return -1;
	}
	const Edge &E = t.edges[e];
	for (int i = 0; i < 4; i++) {
		if (Edge(F.v[i], F.v[(i + 1) % 4]) == E) {
			return t.find_edge(F.v[(i + 2) % 4], F.v[(i + 3) % 4]);
		}
	}
	return -1;
}

std::vector<Edge> edge_ring(const Mesh &m, const Topology &t, const Edge &start) {
	std::vector<Edge> out;
	const int e0 = t.find_edge(start.a, start.b);
	if (e0 < 0) {
		return out;
	}
	std::vector<char> seen(t.edges.size(), 0);
	seen[e0] = 1;
	out.push_back(t.edges[e0]);
	for (int f0 : t.edge_faces[e0]) {
		int e = e0;
		int f = f0;
		while (f >= 0) {
			const int o = quad_opposite(m, t, f, e);
			if (o < 0 || seen[o]) {
				break;
			}
			seen[o] = 1;
			out.push_back(t.edges[o]);
			f = t.edge_faces[o].size() == 2 ? t.other_face(o, f) : -1;
			e = o;
		}
	}
	return out;
}

std::vector<int> face_loop(const Mesh &m, const Topology &t, int face, const Edge &dir_edge) {
	std::vector<int> out;
	if (face < 0 || face >= (int)m.faces.size()) {
		return out;
	}
	out.push_back(face);
	const int e0 = t.find_edge(dir_edge.a, dir_edge.b);
	if (e0 < 0) {
		return out;
	}
	std::vector<char> seen(m.faces.size(), 0);
	seen[face] = 1;
	// Two directions: across dir_edge, and across its opposite in the start face.
	int starts[2] = { e0, quad_opposite(m, t, face, e0) };
	for (int side = 0; side < 2; side++) {
		int e = starts[side];
		int f = face;
		while (e >= 0) {
			if (t.edge_faces[e].size() != 2) {
				break;
			}
			const int g = t.other_face(e, f);
			if (g < 0 || seen[g]) {
				break;
			}
			seen[g] = 1;
			out.push_back(g);
			e = quad_opposite(m, t, g, e);
			f = g;
		}
	}
	return out;
}

std::vector<int> face_loops(const Mesh &m, const Topology &t, const std::vector<int> &faces, bool ring) {
	std::vector<char> fm = mask_of(faces, m.faces.size());
	std::vector<int> out;
	for (int f : faces) {
		if (f < 0 || f >= (int)m.faces.size()) {
			continue;
		}
		const Face &F = m.faces[f];
		const int n = F.size();
		// Direction: an edge shared with another selected face, else the first edge.
		int dir = 0;
		for (int i = 0; i < n; i++) {
			const int e = t.find_edge(F.v[i], F.v[(i + 1) % n]);
			const int g = e >= 0 ? t.other_face(e, f) : -1;
			if (g >= 0 && fm[g]) {
				dir = i;
				break;
			}
		}
		if (ring) {
			dir = (dir + 1) % n;
		}
		std::vector<int> l = face_loop(m, t, f, Edge(F.v[dir], F.v[(dir + 1) % n]));
		out.insert(out.end(), l.begin(), l.end());
	}
	return uniq(out);
}

// ------------------------------------------------------------------ attributes

std::vector<int> faces_by_material(const Mesh &m, const std::vector<int> &like) {
	std::unordered_set<int> mats;
	for (int f : like) {
		if (f >= 0 && f < (int)m.faces.size()) {
			mats.insert(m.faces[f].material);
		}
	}
	std::vector<int> out;
	for (int f = 0; f < (int)m.faces.size(); f++) {
		if (mats.count(m.faces[f].material)) {
			out.push_back(f);
		}
	}
	return out;
}

std::vector<int> faces_by_normal(const Mesh &m, const std::vector<int> &like, float angle_deg) {
	std::vector<Vector3> ns;
	for (int f : like) {
		if (f >= 0 && f < (int)m.faces.size()) {
			ns.push_back(face_normal(m, m.faces[f]));
		}
	}
	const float c = std::cos(angle_deg * (float)(godot::Math::PI / 180.0));
	std::vector<int> out;
	for (int f = 0; f < (int)m.faces.size(); f++) {
		const Vector3 n = face_normal(m, m.faces[f]);
		for (const Vector3 &o : ns) {
			if (n.dot(o) >= c - 1e-6f) {
				out.push_back(f);
				break;
			}
		}
	}
	return out;
}

std::vector<int> invert_faces(const Mesh &m, const std::vector<int> &s) {
	std::vector<char> fm = mask_of(s, m.faces.size());
	for (char &c : fm) {
		c = !c;
	}
	return from_mask(fm);
}

std::vector<int> invert_verts(const Mesh &m, const std::vector<int> &s) {
	std::vector<char> vm = mask_of(s, m.pos.size());
	std::vector<char> used(m.pos.size(), 0);
	for (const Face &F : m.faces) {
		for (int v : F.v) {
			used[v] = 1;
		}
	}
	for (size_t i = 0; i < vm.size(); i++) {
		vm[i] = used[i] && !vm[i];
	}
	return from_mask(vm);
}

std::vector<Edge> invert_edges(const Mesh &m, const Topology &t, const std::vector<Edge> &s) {
	(void)m;
	std::unordered_set<uint64_t> in;
	for (const Edge &e : s) {
		in.insert(ekey(e));
	}
	std::vector<Edge> out;
	for (const Edge &e : t.edges) {
		if (!in.count(ekey(e))) {
			out.push_back(e);
		}
	}
	return out;
}

// ------------------------------------------------------------------ shortest paths

namespace {

// Dijkstra over an implicit graph; returns the node chain from `from` to `to` (empty if unreachable).
template <typename Neighbors>
std::vector<int> dijkstra(int count, int from, int to, Neighbors neighbors) {
	if (from < 0 || to < 0 || from >= count || to >= count) {
		return {};
	}
	std::vector<float> dist(count, 1e30f);
	std::vector<int> prev(count, -1);
	using Item = std::pair<float, int>;
	std::vector<Item> heap;
	auto cmp = [](const Item &a, const Item &b) { return a.first > b.first; };
	dist[from] = 0.0f;
	heap.push_back({ 0.0f, from });
	while (!heap.empty()) {
		std::pop_heap(heap.begin(), heap.end(), cmp);
		const Item cur = heap.back();
		heap.pop_back();
		if (cur.first > dist[cur.second]) {
			continue;
		}
		if (cur.second == to) {
			break;
		}
		neighbors(cur.second, [&](int nb, float w) {
			const float d = cur.first + w;
			if (d < dist[nb]) {
				dist[nb] = d;
				prev[nb] = cur.second;
				heap.push_back({ d, nb });
				std::push_heap(heap.begin(), heap.end(), cmp);
			}
		});
	}
	if (from != to && prev[to] < 0) {
		return {};
	}
	std::vector<int> out;
	for (int v = to; v >= 0; v = prev[v]) {
		out.push_back(v);
		if (v == from) {
			break;
		}
	}
	std::reverse(out.begin(), out.end());
	return out;
}

} // namespace

namespace {

// Steps from `from` to every node (-1: not reached).
template <typename Neighbors>
std::vector<int> steps_from(int count, int from, Neighbors neighbors) {
	std::vector<int> d(count, -1);
	if (from < 0 || from >= count) {
		return d;
	}
	d[from] = 0;
	std::vector<int> q{ from };
	for (size_t i = 0; i < q.size(); i++) {
		const int cur = q[i];
		neighbors(cur, [&](int nb) {
			if (nb >= 0 && nb < count && d[nb] < 0) {
				d[nb] = d[cur] + 1;
				q.push_back(nb);
			}
		});
	}
	return d;
}

// The nodes whose steps from both ends add up to the fewest steps between the ends.
template <typename Neighbors>
std::vector<int> region_between(int count, int from, int to, Neighbors neighbors) {
	const std::vector<int> da = steps_from(count, from, neighbors);
	if (to < 0 || to >= count || da[to] < 0) {
		return {};
	}
	const std::vector<int> db = steps_from(count, to, neighbors);
	std::vector<int> out;
	for (int i = 0; i < count; i++) {
		if (da[i] >= 0 && db[i] >= 0 && da[i] + db[i] == da[to]) {
			out.push_back(i);
		}
	}
	return out;
}

} // namespace

std::vector<int> path_region_verts(const Mesh &m, const Topology &t, int from, int to) {
	return region_between((int)m.pos.size(), from, to, [&](int v, auto &&emit) {
		for (int e : t.vert_edges[v]) {
			emit(t.edges[e].a == v ? t.edges[e].b : t.edges[e].a);
		}
	});
}

std::vector<int> path_region_faces(const Mesh &m, const Topology &t, int from, int to) {
	return region_between((int)m.faces.size(), from, to, [&](int f, auto &&emit) {
		const Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			const int e = t.find_edge(F.v[i], F.v[(i + 1) % F.size()]);
			if (e < 0) {
				continue;
			}
			for (int o : t.edge_faces[e]) {
				if (o != f) {
					emit(o);
				}
			}
		}
	});
}

std::vector<Edge> path_region_edges(const Mesh &m, const Topology &t, const Edge &from, const Edge &to) {
	// the vertices between the nearest ends of the two edges, then the edges joining two of them, and both
	// clicked edges
	std::vector<int> best;
	int best_steps = 1 << 30;
	const int fa[2] = { from.a, from.b }, ta[2] = { to.a, to.b };
	for (int i = 0; i < 2; i++) {
		for (int j = 0; j < 2; j++) {
			const std::vector<int> p = shortest_path_verts(m, t, fa[i], ta[j]);
			if (!p.empty() && (int)p.size() < best_steps) {
				best_steps = (int)p.size();
				best = path_region_verts(m, t, fa[i], ta[j]);
			}
		}
	}
	std::vector<char> in(m.pos.size(), 0);
	for (int v : best) {
		in[v] = 1;
	}
	std::vector<Edge> out;
	for (const Edge &E : t.edges) {
		if (in[E.a] && in[E.b]) {
			out.push_back(E);
		}
	}
	for (const Edge &E : { from, to }) {
		if (std::find(out.begin(), out.end(), E) == out.end()) {
			out.push_back(E);
		}
	}
	return out;
}

std::vector<int> shortest_path_verts(const Mesh &m, const Topology &t, int from, int to) {
	return dijkstra((int)m.pos.size(), from, to, [&](int v, auto &&emit) {
		for (int e : t.vert_edges[v]) {
			const Edge &E = t.edges[e];
			const int o = E.a == v ? E.b : E.a;
			emit(o, m.pos[v].distance_to(m.pos[o]));
		}
	});
}

std::vector<int> shortest_path_faces(const Mesh &m, const Topology &t, int from, int to) {
	std::vector<Vector3> centers(m.faces.size());
	for (size_t f = 0; f < m.faces.size(); f++) {
		centers[f] = face_center(m, m.faces[f]);
	}
	return dijkstra((int)m.faces.size(), from, to, [&](int f, auto &&emit) {
		const Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			const int e = t.find_edge(F.v[i], F.v[(i + 1) % F.size()]);
			if (e < 0) {
				continue;
			}
			for (int o : t.edge_faces[e]) {
				if (o != f) {
					emit(o, centers[f].distance_to(centers[o]));
				}
			}
		}
	});
}

std::vector<Edge> shortest_path_edges(const Mesh &m, const Topology &t, const Edge &from, const Edge &to) {
	// Shortest vertex chain between the closest endpoints, plus both end edges.
	std::vector<int> best;
	const int fa[2] = { from.a, from.b }, ta[2] = { to.a, to.b };
	float best_len = 1e30f;
	for (int i = 0; i < 2; i++) {
		for (int j = 0; j < 2; j++) {
			std::vector<int> p = shortest_path_verts(m, t, fa[i], ta[j]);
			if (p.empty()) {
				continue;
			}
			float len = 0.0f;
			for (size_t k = 1; k < p.size(); k++) {
				len += m.pos[p[k - 1]].distance_to(m.pos[p[k]]);
			}
			if (len < best_len) {
				best_len = len;
				best = p;
			}
		}
	}
	std::vector<Edge> out = { from, to };
	for (size_t k = 1; k < best.size(); k++) {
		out.push_back(Edge(best[k - 1], best[k]));
	}
	return uniq(out);
}

} // namespace sel
} // namespace hewn
