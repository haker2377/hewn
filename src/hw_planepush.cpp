// Hewn — a face of a convex block moved along its normal, the block remade from the planes of its faces: the
// faces around keep their planes, and where one of them can't get smaller along one side any more, the corners
// run on along the next plane (the block's outline changes, the moved face gets cut down) - the faces as the
// planes now meet. The move is refused when a face would vanish.
#include "hw_core.h"

#include <godot_cpp/variant/aabb.hpp>

#include <algorithm>
#include <cmath>
#include <set>

namespace hewn {
namespace ops {

namespace {
struct Plane {
	Vector3 n;
	float d = 0.0f; // n.p = d on it, n.p < d inside
};

// The face's corners within its plane, as (x, y) on two axes across the normal.
void plane_axes(const Vector3 &n, Vector3 &r_a, Vector3 &r_b) {
	r_a = (std::fabs(n.x) < 0.9f ? Vector3(1, 0, 0) : Vector3(0, 1, 0)).cross(n).normalized();
	r_b = n.cross(r_a);
}

// UV (or anything 2D) as an affine function of the position on the face's plane, fitted to its corners.
struct Affine {
	bool ok = false;
	Vector3 a, b;
	float m[2][3] = {};
	Vector2 at(const Vector3 &p) const {
		const float x = p.dot(a), y = p.dot(b);
		return Vector2(m[0][0] * x + m[0][1] * y + m[0][2], m[1][0] * x + m[1][1] * y + m[1][2]);
	}
};

Affine fit_uv(const Mesh &m, const Face &F, const Vector3 &n) {
	Affine A;
	if (F.uv.size() != F.v.size() || F.size() < 3) {
		return A;
	}
	plane_axes(n, A.a, A.b);
	// least squares: [x y 1] -> u, v
	double S[3][3] = {}, U[3] = {}, V[3] = {};
	for (int i = 0; i < F.size(); i++) {
		const Vector3 &p = m.pos[F.v[i]];
		const double r[3] = { p.dot(A.a), p.dot(A.b), 1.0 };
		for (int j = 0; j < 3; j++) {
			for (int k = 0; k < 3; k++) {
				S[j][k] += r[j] * r[k];
			}
			U[j] += r[j] * F.uv[i].x;
			V[j] += r[j] * F.uv[i].y;
		}
	}
	const double det = S[0][0] * (S[1][1] * S[2][2] - S[1][2] * S[2][1]) - S[0][1] * (S[1][0] * S[2][2] - S[1][2] * S[2][0]) + S[0][2] * (S[1][0] * S[2][1] - S[1][1] * S[2][0]);
	if (std::fabs(det) < 1e-12) {
		return A;
	}
	auto solve = [&](const double *rhs, float *out) {
		for (int c = 0; c < 3; c++) {
			double M[3][3];
			for (int j = 0; j < 3; j++) {
				for (int k = 0; k < 3; k++) {
					M[j][k] = k == c ? rhs[j] : S[j][k];
				}
			}
			const double dc = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
			out[c] = (float)(dc / det);
		}
	};
	solve(U, A.m[0]);
	solve(V, A.m[1]);
	A.ok = true;
	return A;
}
} // namespace

bool is_convex_block(const Mesh &m) {
	if (m.faces.size() < 4 || m.faces.size() > 256 || !m.wires.empty() || !m.points.empty()) { // (a block, not a dense mesh: each move remakes it)
		return false;
	}
	Topology tp;
	tp.build(m);
	for (const std::vector<int> &ef : tp.edge_faces) {
		if (ef.size() != 2) {
			return false; // (open, or more than two faces on an edge)
		}
	}
	godot::AABB box;
	for (size_t i = 0; i < m.pos.size(); i++) {
		box = i ? box.expand(m.pos[i]) : godot::AABB(m.pos[i], Vector3());
	}
	const float eps = 1e-4f * std::max(1.0f, (float)box.size.length());
	std::vector<Plane> pl;
	for (const Face &F : m.faces) {
		if (F.size() < 3) {
			return false;
		}
		Plane p;
		p.n = face_normal(m, F);
		p.d = p.n.dot(face_center(m, F));
		for (int v : F.v) {
			if (std::fabs(p.n.dot(m.pos[v]) - p.d) > eps) {
				return false; // (not flat)
			}
		}
		for (const Plane &q : pl) {
			if (q.n.dot(p.n) > 0.9999f && std::fabs(q.d - p.d) < eps) {
				return false; // (two faces in one plane: they would become one)
			}
		}
		pl.push_back(p);
	}
	for (const Plane &p : pl) {
		for (const Vector3 &v : m.pos) {
			if (p.n.dot(v) - p.d > eps) {
				return false; // (not convex)
			}
		}
	}
	return true;
}

bool move_face_plane(const Mesh &src, int p_face, float p_delta, Mesh &r_out) {
	if (p_face < 0 || p_face >= (int)src.faces.size()) {
		return false;
	}
	const int nf = (int)src.faces.size();
	std::vector<Plane> pl(nf);
	godot::AABB box;
	for (size_t i = 0; i < src.pos.size(); i++) {
		box = i ? box.expand(src.pos[i]) : godot::AABB(src.pos[i], Vector3());
	}
	const float size = std::max(1.0f, (float)box.size.length());
	const float eps = 1e-4f * size;
	for (int f = 0; f < nf; f++) {
		pl[f].n = face_normal(src, src.faces[f]);
		pl[f].d = pl[f].n.dot(face_center(src, src.faces[f]));
	}
	pl[p_face].d += p_delta;
	const Vector3 mid = box.get_center();
	const float big = (size + std::fabs(p_delta)) * 4.0f;
	// each face: a big square on its plane cut down by all the other planes
	std::vector<std::vector<Vector3>> polys(nf);
	for (int f = 0; f < nf; f++) {
		Vector3 a, b;
		plane_axes(pl[f].n, a, b);
		const Vector3 c = mid + pl[f].n * (pl[f].d - pl[f].n.dot(mid));
		std::vector<Vector3> poly = { c - a * big - b * big, c + a * big - b * big, c + a * big + b * big, c - a * big + b * big };
		for (int g = 0; g < nf && poly.size() >= 3; g++) {
			if (g == f) {
				continue;
			}
			std::vector<Vector3> out;
			for (size_t i = 0; i < poly.size(); i++) {
				const Vector3 &p = poly[i], &q = poly[(i + 1) % poly.size()];
				const float dp = pl[g].n.dot(p) - pl[g].d, dq = pl[g].n.dot(q) - pl[g].d;
				if (dp <= 0.0f) {
					out.push_back(p);
				}
				if ((dp < 0.0f && dq > 0.0f) || (dp > 0.0f && dq < 0.0f)) {
					out.push_back(p.lerp(q, dp / (dp - dq)));
				}
			}
			poly.swap(out);
		}
		// (corners where more planes meet come out several times)
		std::vector<Vector3> clean;
		for (const Vector3 &p : poly) {
			if (clean.empty() || clean.back().distance_to(p) > eps) {
				clean.push_back(p);
			}
		}
		while (clean.size() > 1 && clean.back().distance_to(clean.front()) <= eps) {
			clean.pop_back();
		}
		// every face must stay a face with an area (as a block's side can't vanish)
		Vector3 area;
		for (size_t i = 0; i < clean.size(); i++) {
			area += clean[i].cross(clean[(i + 1) % clean.size()]);
		}
		if (clean.size() < 3 || area.length() * 0.5f < eps * eps * 10.0f || area.dot(pl[f].n) <= 0.0f) {
			return false;
		}
		polys[f] = clean;
	}
	// the corners: one vertex where they meet; a vertex keeps its number when the same faces meet there as before
	std::vector<Vector3> npos;
	std::vector<std::vector<int>> rings(nf);
	for (int f = 0; f < nf; f++) {
		for (const Vector3 &p : polys[f]) {
			int id = -1;
			for (size_t k = 0; k < npos.size(); k++) {
				if (npos[k].distance_to(p) <= eps * 4.0f) {
					id = (int)k;
					break;
				}
			}
			if (id < 0) {
				id = (int)npos.size();
				npos.push_back(p);
			}
			if (rings[f].empty() || (rings[f].back() != id && rings[f].front() != id)) {
				rings[f].push_back(id);
			}
		}
		if (rings[f].size() < 3) {
			return false;
		}
	}
	std::vector<std::set<int>> old_at(src.pos.size()), new_at(npos.size());
	for (int f = 0; f < nf; f++) {
		for (int v : src.faces[f].v) {
			old_at[v].insert(f);
		}
		for (int v : rings[f]) {
			new_at[v].insert(f);
		}
	}
	// numbering: the vertices that stay keep their order, new ones after them
	std::vector<int> new_of_old(src.pos.size(), -1), old_of_new(npos.size(), -1);
	for (size_t o = 0; o < src.pos.size(); o++) {
		for (size_t k = 0; k < npos.size(); k++) {
			if (old_of_new[k] < 0 && !old_at[o].empty() && old_at[o] == new_at[k]) {
				old_of_new[k] = (int)o;
				new_of_old[o] = (int)k;
				break;
			}
		}
	}
	std::vector<int> final_of_new(npos.size(), -1), map(src.pos.size(), -1);
	std::vector<Vector3> fpos;
	for (size_t o = 0; o < src.pos.size(); o++) {
		if (new_of_old[o] >= 0) {
			final_of_new[new_of_old[o]] = (int)fpos.size();
			map[o] = (int)fpos.size();
			fpos.push_back(npos[new_of_old[o]]);
		}
	}
	for (size_t k = 0; k < npos.size(); k++) {
		if (final_of_new[k] < 0) {
			final_of_new[k] = (int)fpos.size();
			fpos.push_back(npos[k]);
		}
	}
	r_out = src;
	r_out.pos = fpos;
	for (int f = 0; f < nf; f++) {
		const Face &S = src.faces[f];
		Face &F = r_out.faces[f];
		const Affine A = fit_uv(src, S, pl[f].n);
		std::vector<int> v;
		std::vector<Vector2> uv;
		std::vector<Color> col, cust;
		const bool has_cust = S.cust.size() == S.v.size() * 4;
		for (int k : rings[f]) {
			const int fv = final_of_new[k];
			v.push_back(fv);
			// the corner it was, else the nearest corner of the face (the moved face: as it was before the move)
			int corner = -1;
			const int o = old_of_new[k];
			for (int i = 0; o >= 0 && i < S.size(); i++) {
				if (S.v[i] == o) {
					corner = i;
				}
			}
			const Vector3 p = f == p_face ? npos[k] - pl[f].n * p_delta : npos[k];
			if (corner < 0) {
				float best = 1e30f;
				for (int i = 0; i < S.size(); i++) {
					const float dd = src.pos[S.v[i]].distance_squared_to(p);
					if (dd < best) {
						best = dd;
						corner = i;
					}
				}
			}
			// UVs: the corner's own where it stays; a new corner on the texture as the face had it
			uv.push_back(corner < (int)S.uv.size() && o >= 0 && S.v[corner] == o ? S.uv[corner] : A.ok ? A.at(p) : (corner < (int)S.uv.size() ? S.uv[corner] : Vector2()));
			col.push_back(corner < (int)S.col.size() ? S.col[corner] : Color(1, 1, 1, 1));
			for (int c = 0; has_cust && c < 4; c++) {
				cust.push_back(S.cust[corner * 4 + c]);
			}
		}
		F.v = v;
		F.uv = uv;
		F.col = col;
		F.cust = cust;
		if (face_normal(r_out, F).dot(pl[f].n) < 0.0f) {
			F.reverse();
		}
	}
	remap_vertex_attrs(r_out, map);
	return true;
}

} // namespace ops
} // namespace hewn
