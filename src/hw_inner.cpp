// Hewn — the faces no one can see: inside the mesh's own solid (after merging objects that overlap or touch). A
// face is hidden when every ray from points on it, over its front half, first meets some face from behind (the
// ray is inside the solid then); a ray that gets out or meets a face's front is open space (a room's inside stays).
#include "hw_core.h"

#include <algorithm>
#include <cmath>

namespace hewn {

namespace ops {

namespace {

struct Tri {
	Vector3 a, b, c, n; // (n: the face's normal)
	int face;
};

struct Node {
	godot::AABB box;
	int left = -1, right = -1; // (children; a leaf: -1, its triangles from `first`)
	int first = 0, count = 0;
};

struct Bvh {
	std::vector<Tri> tris;
	std::vector<Node> nodes;

	int build(int p_first, int p_count) {
		Node nd;
		for (int i = 0; i < p_count; i++) {
			const Tri &t = tris[p_first + i];
			godot::AABB b(t.a, Vector3());
			b.expand_to(t.b);
			b.expand_to(t.c);
			nd.box = i ? nd.box.merge(b) : b;
		}
		nd.first = p_first;
		nd.count = p_count;
		const int id = (int)nodes.size();
		nodes.push_back(nd);
		if (p_count <= 4) {
			return id;
		}
		const int axis = nd.box.get_longest_axis_index();
		std::sort(tris.begin() + p_first, tris.begin() + p_first + p_count, [axis](const Tri &x, const Tri &y) {
			return (x.a[axis] + x.b[axis] + x.c[axis]) < (y.a[axis] + y.b[axis] + y.c[axis]);
		});
		const int half = p_count / 2;
		const int l = build(p_first, half);
		const int r = build(p_first + half, p_count - half);
		nodes[id].left = l;
		nodes[id].right = r;
		return id;
	}

	static bool box_hit(const godot::AABB &b, const Vector3 &o, const Vector3 &inv, float tmax) {
		float t0 = 0.0f, t1 = tmax;
		for (int k = 0; k < 3; k++) {
			float ta = (b.position[k] - o[k]) * inv[k], tb = (b.position[k] + b.size[k] - o[k]) * inv[k];
			if (ta > tb) {
				std::swap(ta, tb);
			}
			t0 = std::max(t0, ta);
			t1 = std::min(t1, tb);
			if (t0 > t1 + 1e-6f) {
				return false;
			}
		}
		return true;
	}

	// the nearest triangle the ray meets (not those of p_skip), -1: none
	int nearest(const Vector3 &o, const Vector3 &d, int p_skip) const {
		const Vector3 inv(std::fabs(d.x) > 1e-12f ? 1.0f / d.x : 1e12f, std::fabs(d.y) > 1e-12f ? 1.0f / d.y : 1e12f, std::fabs(d.z) > 1e-12f ? 1.0f / d.z : 1e12f);
		float best = 1e30f;
		int hit = -1;
		int stack[128];
		int sp = 0;
		stack[sp++] = 0;
		while (sp > 0) {
			const Node &nd = nodes[stack[--sp]];
			if (!box_hit(nd.box, o, inv, best)) {
				continue;
			}
			if (nd.left < 0) {
				for (int i = nd.first; i < nd.first + nd.count; i++) {
					const Tri &t = tris[i];
					if (t.face == p_skip) {
						continue;
					}
					const Vector3 e1 = t.b - t.a, e2 = t.c - t.a;
					const Vector3 pv = d.cross(e2);
					const float det = e1.dot(pv);
					if (std::fabs(det) < 1e-12f) {
						continue;
					}
					const float id = 1.0f / det;
					const Vector3 tv = o - t.a;
					const float u = tv.dot(pv) * id;
					if (u < -1e-6f || u > 1.0f + 1e-6f) {
						continue;
					}
					const Vector3 qv = tv.cross(e1);
					const float w = d.dot(qv) * id;
					if (w < -1e-6f || u + w > 1.0f + 1e-6f) {
						continue;
					}
					const float tt = e2.dot(qv) * id;
					if (tt > 1e-6f && tt < best) {
						best = tt;
						hit = i;
					}
				}
			} else if (sp + 2 <= 128) {
				stack[sp++] = nd.left;
				stack[sp++] = nd.right;
			}
		}
		return hit;
	}
};

} // namespace

std::vector<int> inner_faces(const Mesh &m) {
	std::vector<int> out;
	Bvh bvh;
	std::vector<Vector3> pts;
	std::vector<int> tri;
	godot::AABB all;
	bool first = true;
	for (int f = 0; f < (int)m.faces.size(); f++) {
		const Face &F = m.faces[f];
		if (F.size() < 3) {
			continue;
		}
		face_points(m, F, pts);
		const Vector3 n = face_normal(m, F);
		tri.clear();
		triangulate(pts, n, tri);
		for (size_t k = 0; k + 2 < tri.size(); k += 3) {
			bvh.tris.push_back({ pts[tri[k]], pts[tri[k + 1]], pts[tri[k + 2]], n, f });
		}
		for (const Vector3 &p : pts) {
			if (first) {
				all = godot::AABB(p, Vector3());
				first = false;
			} else {
				all.expand_to(p);
			}
		}
	}
	if (bvh.tris.empty()) {
		return out;
	}
	bvh.build(0, (int)bvh.tris.size());
	const float eps = std::max(1e-5f, (float)all.size.length() * 1e-5f);
	// directions over a sphere (golden spiral): those on the face's front side are used
	std::vector<Vector3> dirs;
	const int ND = 96;
	for (int i = 0; i < ND; i++) {
		const float y = 1.0f - 2.0f * (i + 0.5f) / ND;
		const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
		const float a = 2.39996323f * i;
		dirs.push_back(Vector3(std::cos(a) * r, y, std::sin(a) * r));
	}
	for (int f = 0; f < (int)m.faces.size(); f++) {
		const Face &F = m.faces[f];
		if (F.size() < 3) {
			continue;
		}
		const Vector3 n = face_normal(m, F);
		if (n.length_squared() < 0.5f) {
			continue;
		}
		const Vector3 c = face_center(m, F);
		std::vector<Vector3> samples{ c };
		for (int v : F.v) {
			samples.push_back(c.lerp(m.pos[v], 0.85f));
		}
		bool hidden = true;
		for (const Vector3 &s : samples) {
			const Vector3 o = s + n * eps;
			for (const Vector3 &d0 : dirs) {
				Vector3 d = d0;
				const float dn = d.dot(n);
				if (dn < 0.05f) {
					continue;
				}
				const int h = bvh.nearest(o, d, f);
				if (h < 0 || bvh.tris[h].n.dot(d) < 0.0f) {
					hidden = false; // (out, or a face seen from its front: open space)
					break;
				}
			}
			if (!hidden) {
				break;
			}
		}
		if (hidden) {
			out.push_back(f);
		}
	}
	return out;
}

} // namespace ops

} // namespace hewn
