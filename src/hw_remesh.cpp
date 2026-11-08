// Hewn — remeshing on a voxel grid. Which grid points are inside: rays along X (the crossings counted). Where the
// surface crosses the grid's edges, and its normal there: rays along X, Y and Z (the triangles sorted into the
// columns they cover, so a ray meets only its own). From that:
// - Voxel / Smooth: one vertex in each cell the surface goes through, at the middle of its crossings (surface nets:
//   quads);
// - Sharp: the vertex where the planes of its crossings meet (dual contouring), pulled to the middle by less sharpness;
// - Blocks: the faces between inside and outside voxels.
// Then the small separate pieces may go, the shading set; attributes come back from the nearest face of the source.
#include "hw_core.h"

#include <godot_cpp/variant/aabb.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace hewn {
namespace ops {

namespace {

struct Tri {
	Vector3 a, b, c, n;
};

std::vector<Tri> triangles_of(const Mesh &m) {
	std::vector<Tri> out;
	std::vector<Vector3> pts;
	std::vector<int> tris;
	for (const Face &F : m.faces) {
		if (F.size() < 3) {
			continue;
		}
		face_points(m, F, pts);
		const Vector3 n = face_normal(m, F);
		tris.clear();
		triangulate(pts, n, tris);
		for (size_t k = 0; k + 2 < tris.size(); k += 3) {
			Tri t{ pts[tris[k]], pts[tris[k + 1]], pts[tris[k + 2]], Vector3() };
			const Vector3 tn = (t.b - t.a).cross(t.c - t.a);
			t.n = tn.length() > 1e-20f ? tn.normalized() : n;
			out.push_back(t);
		}
	}
	return out;
}

struct Hit {
	float t;
	Vector3 n;
};

// Rays along axis `a` through every grid line (u, v) of the other two axes: the crossings with the triangles, sorted.
// columns[(u * nv + v)]: the hits along that line (t: the coordinate on axis a).
void cast_columns(const std::vector<Tri> &tris, int a, const Vector3 &origin, float h, int nu, int nv, std::vector<std::vector<Hit>> &r_cols) {
	const int b = (a + 1) % 3, c = (a + 2) % 3;
	r_cols.assign((size_t)nu * nv, {});
	// (off the grid by a little: rays through vertices / edges exactly would count them twice or not at all)
	const float jb = h * 1.137e-4f, jc = h * 0.913e-4f;
	for (const Tri &t : tris) {
		const float minb = std::min({ t.a[b], t.b[b], t.c[b] }), maxb = std::max({ t.a[b], t.b[b], t.c[b] });
		const float minc = std::min({ t.a[c], t.b[c], t.c[c] }), maxc = std::max({ t.a[c], t.b[c], t.c[c] });
		const int u0 = std::max(0, (int)std::ceil((minb - origin[b] - jb) / h)), u1 = std::min(nu - 1, (int)std::floor((maxb - origin[b] - jb) / h));
		const int v0 = std::max(0, (int)std::ceil((minc - origin[c] - jc) / h)), v1 = std::min(nv - 1, (int)std::floor((maxc - origin[c] - jc) / h));
		if (u0 > u1 || v0 > v1) {
			continue;
		}
		// the triangle in the (b, c) plane; barycentric coordinates of each line's point
		const float x0 = t.a[b], y0 = t.a[c], x1 = t.b[b], y1 = t.b[c], x2 = t.c[b], y2 = t.c[c];
		const float det = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
		if (std::fabs(det) < 1e-20f) {
			continue; // (seen edge-on from this axis)
		}
		for (int u = u0; u <= u1; u++) {
			const float px = origin[b] + u * h + jb;
			for (int v = v0; v <= v1; v++) {
				const float py = origin[c] + v * h + jc;
				const float l0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / det;
				const float l1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / det;
				const float l2 = 1.0f - l0 - l1;
				if (l0 < 0.0f || l1 < 0.0f || l2 < 0.0f) {
					continue;
				}
				r_cols[(size_t)u * nv + v].push_back({ l0 * t.a[a] + l1 * t.b[a] + l2 * t.c[a], t.n });
			}
		}
	}
	for (std::vector<Hit> &col : r_cols) {
		std::sort(col.begin(), col.end(), [](const Hit &x, const Hit &y) { return x.t < y.t; });
	}
}

// 3x3 solve (Gaussian elimination with pivoting); false when singular.
bool solve3(float A[3][3], float bvec[3], Vector3 &r_x) {
	float M[3][4];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			M[i][j] = A[i][j];
		}
		M[i][3] = bvec[i];
	}
	for (int col = 0; col < 3; col++) {
		int piv = col;
		for (int r = col + 1; r < 3; r++) {
			if (std::fabs(M[r][col]) > std::fabs(M[piv][col])) {
				piv = r;
			}
		}
		if (std::fabs(M[piv][col]) < 1e-12f) {
			return false;
		}
		for (int j = 0; j < 4; j++) {
			std::swap(M[col][j], M[piv][j]);
		}
		for (int r = 0; r < 3; r++) {
			if (r == col) {
				continue;
			}
			const float f = M[r][col] / M[col][col];
			for (int j = 0; j < 4; j++) {
				M[r][j] -= f * M[col][j];
			}
		}
	}
	r_x = Vector3(M[0][3] / M[0][0], M[1][3] / M[1][1], M[2][3] / M[2][2]);
	return true;
}

} // namespace

bool remesh(Mesh &m, const RemeshOptions &o) {
	const std::vector<Tri> tris = triangles_of(m);
	if (tris.empty()) {
		return false;
	}
	godot::AABB box(tris[0].a, Vector3());
	for (const Tri &t : tris) {
		box.expand_to(t.a);
		box.expand_to(t.b);
		box.expand_to(t.c);
	}
	const float maxdim = std::max({ (float)box.size.x, (float)box.size.y, (float)box.size.z, 1e-4f });
	float h = o.cell > 0.0f ? o.cell : o.mode == REMESH_VOXEL ? std::max(o.voxel_size, 1e-4f) : maxdim / std::max(o.scale, 0.01f) / (float)(1 << std::max(1, std::min(o.octree_depth, 12)));
	// (no more than about 32 million grid points: a voxel too small for the object is made bigger)
	for (int guard = 0; guard < 8; guard++) {
		const double cells = ((double)box.size.x / h + 4) * ((double)box.size.y / h + 4) * ((double)box.size.z / h + 4);
		if (cells <= 32.0e6) {
			break;
		}
		h *= (float)std::cbrt(cells / 32.0e6) * 1.01f;
	}
	int dims[3];
	Vector3 origin;
	for (int a = 0; a < 3; a++) {
		const float off = (o.grid_offset[a] - std::floor(o.grid_offset[a])) * h; // (the variation: the grid moved a little)
		if (a == 0 && o.symmetric_x) {
			// the grid the same on both sides of x = 0 (its points at +-(k + 1/2) h)
			const int half = (int)std::ceil(std::max(std::fabs((float)box.position.x), std::fabs((float)(box.position.x + box.size.x))) / h) + 2;
			origin.x = -(half + 0.5f) * h;
			dims[0] = 2 * half + 2;
			continue;
		}
		origin[a] = box.position[a] - 1.5f * h - off;
		dims[a] = (int)std::ceil((box.size[a] + 3.0f * h + off) / h) + 1;
	}
	const int nx = dims[0], ny = dims[1], nz = dims[2];
	auto pid = [&](int i, int j, int k) { return ((size_t)i * ny + j) * nz + k; };
	auto gp = [&](int i, int j, int k) { return origin + Vector3(i * h, j * h, k * h); };
	// the hits of the lines along each axis: X lines are indexed (j, k), Y lines (k, i), Z lines (i, j)
	std::vector<std::vector<Hit>> cols[3];
	cast_columns(tris, 0, origin, h, ny, nz, cols[0]);
	cast_columns(tris, 1, origin, h, nz, nx, cols[1]);
	cast_columns(tris, 2, origin, h, nx, ny, cols[2]);
	// inside: an odd number of crossings before the point along X
	std::vector<char> in((size_t)nx * ny * nz, 0);
	for (int j = 0; j < ny; j++) {
		for (int k = 0; k < nz; k++) {
			const std::vector<Hit> &col = cols[0][(size_t)j * nz + k];
			size_t q = 0;
			for (int i = 0; i < nx; i++) {
				const float x = origin.x + i * h;
				while (q < col.size() && col[q].t < x) {
					q++;
				}
				in[pid(i, j, k)] = (q & 1) ? 1 : 0;
			}
		}
	}
	Mesh out;
	if (o.mode == REMESH_BLOCKS) {
		// the faces between an inside point (a voxel) and an outside one, on the plane half way
		std::unordered_map<uint64_t, int> corner_id;
		auto corner = [&](int i2, int j2, int k2) { // (in half cells)
			const uint64_t key = ((uint64_t)(uint32_t)i2 << 42) | ((uint64_t)(uint32_t)j2 << 21) | (uint64_t)(uint32_t)k2;
			auto it = corner_id.find(key);
			if (it != corner_id.end()) {
				return it->second;
			}
			const int id = (int)out.pos.size();
			out.pos.push_back(origin + Vector3(i2, j2, k2) * (h * 0.5f));
			corner_id[key] = id;
			return id;
		};
		for (int i = 0; i < nx; i++) {
			for (int j = 0; j < ny; j++) {
				for (int k = 0; k < nz; k++) {
					if (!in[pid(i, j, k)]) {
						continue;
					}
					for (int a = 0; a < 3; a++) {
						for (int s = -1; s <= 1; s += 2) {
							int q[3] = { i, j, k };
							q[a] += s;
							const bool outside = q[0] < 0 || q[1] < 0 || q[2] < 0 || q[0] >= nx || q[1] >= ny || q[2] >= nz || !in[pid(q[0], q[1], q[2])];
							if (!outside) {
								continue;
							}
							const int b = (a + 1) % 3, c = (a + 2) % 3;
							const int base[3] = { 2 * i + 1, 2 * j + 1, 2 * k + 1 }; // (the cell's middle, in half cells, 1 based)
							int pc[4][3];
							const int ub[4] = { -1, 1, 1, -1 }, vc[4] = { -1, -1, 1, 1 };
							for (int n = 0; n < 4; n++) {
								pc[n][a] = base[a] + s;
								pc[n][b] = base[b] + ub[n];
								pc[n][c] = base[c] + vc[n];
							}
							Face F;
							for (int n = 0; n < 4; n++) {
								F.v.push_back(corner(pc[n][0] - 1, pc[n][1] - 1, pc[n][2] - 1));
							}
							if (s < 0) {
								std::reverse(F.v.begin(), F.v.end()); // (the normal out of the voxel)
							}
							out.faces.push_back(F);
						}
					}
				}
			}
		}
	} else {
		// the crossings on the grid's edges: the point and the normal of the surface there
		struct EdgeHit {
			Vector3 p, n;
		};
		auto ekey3 = [&](int a, int i, int j, int k) { return ((uint64_t)a << 62) | ((uint64_t)pid(i, j, k)); };
		std::unordered_map<uint64_t, EdgeHit> ehit;
		auto edge_hit = [&](int a, int i, int j, int k) -> EdgeHit {
			auto it = ehit.find(ekey3(a, i, j, k));
			if (it != ehit.end()) {
				return it->second;
			}
			const Vector3 p0 = gp(i, j, k);
			const float t0 = p0[a], t1 = t0 + h;
			const std::vector<Hit> *col = nullptr;
			if (a == 0) {
				col = &cols[0][(size_t)j * nz + k];
			} else if (a == 1) {
				col = &cols[1][(size_t)k * nx + i];
			} else {
				col = &cols[2][(size_t)i * ny + j];
			}
			EdgeHit e{ p0 + Vector3(a == 0 ? h * 0.5f : 0, a == 1 ? h * 0.5f : 0, a == 2 ? h * 0.5f : 0), Vector3() };
			for (const Hit &x : *col) {
				if (x.t >= t0 && x.t <= t1) {
					e.p = p0;
					e.p[a] = x.t;
					e.n = x.n;
					break;
				}
			}
			ehit[ekey3(a, i, j, k)] = e;
			return e;
		};
		// one vertex in each cell the surface goes through
		std::unordered_map<size_t, int> cell_vert;
		const float sharp_w = 0.002f + 0.5f * std::max(0.0f, 1.0f - std::max(0.0f, o.sharpness)); // (less sharpness: to the middle)
		auto cell_vertex = [&](int i, int j, int k) {
			const size_t cid = pid(i, j, k);
			auto it = cell_vert.find(cid);
			if (it != cell_vert.end()) {
				return it->second;
			}
			// the 12 edges of the cell: those whose ends differ
			std::vector<EdgeHit> hs;
			for (int a = 0; a < 3; a++) {
				const int b = (a + 1) % 3, c = (a + 2) % 3;
				for (int u = 0; u < 2; u++) {
					for (int v = 0; v < 2; v++) {
						int q[3] = { i, j, k };
						q[b] += u;
						q[c] += v;
						int r[3] = { q[0], q[1], q[2] };
						r[a] += 1;
						if (in[pid(q[0], q[1], q[2])] != in[pid(r[0], r[1], r[2])]) {
							hs.push_back(edge_hit(a, q[0], q[1], q[2]));
						}
					}
				}
			}
			Vector3 mass;
			for (const EdgeHit &e : hs) {
				mass += e.p;
			}
			mass = hs.empty() ? gp(i, j, k) + Vector3(h, h, h) * 0.5f : mass / (float)hs.size();
			Vector3 p = mass;
			if (o.mode == REMESH_SHARP && hs.size() >= 2) {
				// where the planes meet, near the middle (relative to it: a small, well scaled system)
				float A[3][3] = {}, bv[3] = {};
				for (const EdgeHit &e : hs) {
					if (e.n.length_squared() < 0.5f) {
						continue;
					}
					const float d = e.n.dot(e.p - mass);
					for (int r = 0; r < 3; r++) {
						for (int s = 0; s < 3; s++) {
							A[r][s] += e.n[r] * e.n[s];
						}
						bv[r] += e.n[r] * d;
					}
				}
				for (int r = 0; r < 3; r++) {
					A[r][r] += sharp_w * (float)hs.size();
				}
				Vector3 x;
				if (solve3(A, bv, x)) {
					const Vector3 lo = gp(i, j, k) - Vector3(h, h, h) * 0.25f, hi = gp(i + 1, j + 1, k + 1) + Vector3(h, h, h) * 0.25f;
					p = (mass + x).clamp(lo, hi); // (it stays about its cell)
				}
			}
			const int id = (int)out.pos.size();
			out.pos.push_back(p);
			cell_vert[cid] = id;
			return id;
		};
		// a quad around each grid edge the surface crosses (its 4 cells' vertices)
		for (int i = 1; i < nx - 1; i++) {
			for (int j = 1; j < ny - 1; j++) {
				for (int k = 1; k < nz - 1; k++) {
					const bool here = in[pid(i, j, k)];
					for (int a = 0; a < 3; a++) {
						int q[3] = { i, j, k };
						q[a] += 1;
						if (q[a] >= dims[a] - 1 || here == (bool)in[pid(q[0], q[1], q[2])]) {
							continue;
						}
						const int b = (a + 1) % 3, c = (a + 2) % 3;
						const int ub[4] = { -1, 0, 0, -1 }, vc[4] = { -1, -1, 0, 0 };
						Face F;
						for (int n = 0; n < 4; n++) {
							int cc[3] = { i, j, k };
							cc[b] += ub[n];
							cc[c] += vc[n];
							F.v.push_back(cell_vertex(cc[0], cc[1], cc[2]));
						}
						if (!here) {
							std::reverse(F.v.begin(), F.v.end()); // (the normal from inside to outside)
						}
						out.faces.push_back(F);
					}
				}
			}
		}
	}
	if (out.faces.empty()) {
		return false;
	}
	// the faces' settings from the source's first face (the materials / colors: transfer_attributes)
	for (Face &F : out.faces) {
		if (!m.faces.empty()) {
			F.copy_attrs_from(m.faces[0]);
		}
		F.manual_uv = false;
		F.smooth = o.smooth_shading ? 1 : 0;
		F.ensure_attrs();
	}
	weld_by_distance(out, h * 1e-3f);
	cleanup(out);
	if (o.remove_disconnected) {
		// the pieces with fewer faces than the threshold times the biggest one's go
		std::vector<int> parent(out.pos.size());
		for (size_t v = 0; v < parent.size(); v++) {
			parent[v] = (int)v;
		}
		auto find = [&](int x) {
			while (parent[x] != x) {
				parent[x] = parent[parent[x]];
				x = parent[x];
			}
			return x;
		};
		for (const Face &F : out.faces) {
			for (int v : F.v) {
				parent[find(v)] = find(F.v[0]);
			}
		}
		std::unordered_map<int, int> count;
		for (const Face &F : out.faces) {
			count[find(F.v[0])]++;
		}
		int biggest = 0;
		for (const auto &kv : count) {
			biggest = std::max(biggest, kv.second);
		}
		const float keep = std::max(0.0f, std::min(o.threshold, 1.0f)) * biggest;
		std::vector<Face> kept;
		for (Face &F : out.faces) {
			const int c = count[find(F.v[0])];
			if (c >= keep || c == biggest) {
				kept.push_back(std::move(F));
			}
		}
		out.faces.swap(kept);
	}
	if (o.mode == REMESH_VOXEL && o.adaptivity > 0.0f) {
		planar_simplify(out, std::min(1.0f, o.adaptivity) * 20.0f); // (flat parts with fewer faces)
	}
	remove_unused_vertices(out);
	out.edge_attrs.clear();
	m = out;
	return true;
}

// Convex pieces for a collision: the space the mesh takes on a coarse voxel grid (inside, or touched by a face)
// joined into boxes (as few as fit p_max_pieces: the grid coarser until they do), the mesh cut by each box; each
// piece's points (their hull is the piece).
std::vector<std::vector<Vector3>> convex_pieces(const Mesh &m, int p_max_pieces) {
	std::vector<std::vector<Vector3>> out;
	const std::vector<Tri> tris = triangles_of(m);
	if (tris.empty()) {
		return out;
	}
	godot::AABB box(tris[0].a, Vector3());
	for (const Tri &t : tris) {
		box.expand_to(t.a);
		box.expand_to(t.b);
		box.expand_to(t.c);
	}
	const float maxdim = std::max({ (float)box.size.x, (float)box.size.y, (float)box.size.z, 1e-4f });
	struct Box {
		int lo[3], hi[3]; // (voxels, hi inclusive)
	};
	std::vector<Box> boxes;
	Vector3 origin;
	float h = 0.0f;
	for (int res = 24; res >= 1; res = res > 4 ? res * 2 / 3 : res - 1) {
		h = maxdim / (float)res;
		origin = box.position - Vector3(h, h, h) * 0.01f;
		int n[3];
		for (int a = 0; a < 3; a++) {
			n[a] = std::max(1, (int)std::ceil((box.size[a] + h * 0.02f) / h));
		}
		auto vid = [&](int i, int j, int k) { return ((size_t)i * n[1] + j) * n[2] + k; };
		std::vector<char> occ((size_t)n[0] * n[1] * n[2], 0);
		// touched by a face (its bounds: generous)
		for (const Tri &t : tris) {
			int lo[3], hi[3];
			for (int a = 0; a < 3; a++) {
				const float mn = std::min({ t.a[a], t.b[a], t.c[a] }), mx = std::max({ t.a[a], t.b[a], t.c[a] });
				lo[a] = std::max(0, std::min(n[a] - 1, (int)std::floor((mn - origin[a]) / h)));
				hi[a] = std::max(0, std::min(n[a] - 1, (int)std::floor((mx - origin[a]) / h)));
			}
			for (int i = lo[0]; i <= hi[0]; i++) {
				for (int j = lo[1]; j <= hi[1]; j++) {
					for (int k = lo[2]; k <= hi[2]; k++) {
						occ[vid(i, j, k)] = 1;
					}
				}
			}
		}
		// inside: the voxels' middles, by the crossings along X
		std::vector<std::vector<Hit>> cols;
		const Vector3 mid_origin = origin + Vector3(h, h, h) * 0.5f;
		cast_columns(tris, 0, mid_origin, h, n[1], n[2], cols);
		for (int j = 0; j < n[1]; j++) {
			for (int k = 0; k < n[2]; k++) {
				const std::vector<Hit> &col = cols[(size_t)j * n[2] + k];
				size_t q = 0;
				for (int i = 0; i < n[0]; i++) {
					const float x = mid_origin.x + i * h;
					while (q < col.size() && col[q].t < x) {
						q++;
					}
					if (q & 1) {
						occ[vid(i, j, k)] = 1;
					}
				}
			}
		}
		// the boxes: grown from each free voxel along X, then Y, then Z while all of it is taken and free
		std::vector<char> used(occ.size(), 0);
		boxes.clear();
		bool too_many = false;
		for (int i = 0; i < n[0] && !too_many; i++) {
			for (int j = 0; j < n[1] && !too_many; j++) {
				for (int k = 0; k < n[2] && !too_many; k++) {
					if (!occ[vid(i, j, k)] || used[vid(i, j, k)]) {
						continue;
					}
					Box b{ { i, j, k }, { i, j, k } };
					auto free_block = [&](const int lo[3], const int hi[3]) {
						for (int x = lo[0]; x <= hi[0]; x++) {
							for (int y = lo[1]; y <= hi[1]; y++) {
								for (int z = lo[2]; z <= hi[2]; z++) {
									if (!occ[vid(x, y, z)] || used[vid(x, y, z)]) {
										return false;
									}
								}
							}
						}
						return true;
					};
					for (int a = 0; a < 3; a++) {
						while (b.hi[a] + 1 < n[a]) {
							int lo[3] = { b.lo[0], b.lo[1], b.lo[2] }, hi[3] = { b.hi[0], b.hi[1], b.hi[2] };
							lo[a] = hi[a] = b.hi[a] + 1;
							if (!free_block(lo, hi)) {
								break;
							}
							b.hi[a]++;
						}
					}
					for (int x = b.lo[0]; x <= b.hi[0]; x++) {
						for (int y = b.lo[1]; y <= b.hi[1]; y++) {
							for (int z = b.lo[2]; z <= b.hi[2]; z++) {
								used[vid(x, y, z)] = 1;
							}
						}
					}
					boxes.push_back(b);
					too_many = (int)boxes.size() > p_max_pieces;
				}
			}
		}
		if (!too_many) {
			break;
		}
	}
	// the mesh cut down to each box
	for (const Box &b : boxes) {
		Mesh piece = m;
		const Vector3 lo = origin + Vector3(b.lo[0], b.lo[1], b.lo[2]) * h, hi = origin + Vector3(b.hi[0] + 1, b.hi[1] + 1, b.hi[2] + 1) * h;
		for (int a = 0; a < 3 && !piece.faces.empty(); a++) {
			Vector3 nrm;
			nrm[a] = 1.0f;
			if (hi[a] < (float)(box.position[a] + box.size[a]) - 1e-6f) {
				bisect(piece, {}, hi, nrm, false, true, true);
			}
			if (lo[a] > (float)box.position[a] + 1e-6f) {
				bisect(piece, {}, lo, -nrm, false, true, true);
			}
		}
		std::vector<Vector3> pts;
		for (const Face &F : piece.faces) {
			for (int v : F.v) {
				pts.push_back(piece.pos[v]);
			}
		}
		if (pts.size() >= 4) {
			out.push_back(pts);
		}
	}
	return out;
}

// The vertices moved along their surface toward the middle of their neighbors (evener faces; the shape stays).
void relax_tangential(Mesh &m, int p_iterations) {
	Topology t;
	t.build(m);
	for (int it = 0; it < p_iterations; it++) {
		const std::vector<Vector3> vn = vertex_normals(m);
		std::vector<Vector3> np = m.pos;
		for (int v = 0; v < (int)m.pos.size(); v++) {
			const std::vector<int> &es = t.vert_edges[v];
			if (es.size() < 3) {
				continue;
			}
			bool border = false;
			Vector3 avg;
			for (int e : es) {
				border = border || t.edge_faces[e].size() != 2;
				avg += m.pos[t.edges[e].a == v ? t.edges[e].b : t.edges[e].a];
			}
			if (border) {
				continue;
			}
			avg /= (float)es.size();
			Vector3 d = avg - m.pos[v];
			d -= vn[v] * vn[v].dot(d);
			np[v] = m.pos[v] + d * 0.5f;
		}
		m.pos = np;
	}
}

// Each face of dst takes the material, the texture settings and the smoothing of the nearest face of src; with
// p_colors also the colors of the nearest corners.
void transfer_attributes(Mesh &dst, const Mesh &src, bool p_colors, bool p_smoothing) {
	if (src.faces.empty() || dst.faces.empty()) {
		return;
	}
	// the source's face middles in a grid of cells about two faces wide
	std::vector<Vector3> sc(src.faces.size());
	godot::AABB box;
	for (size_t f = 0; f < src.faces.size(); f++) {
		sc[f] = face_center(src, src.faces[f]);
		box = f ? box.expand(sc[f]) : godot::AABB(sc[f], Vector3());
	}
	const float cell = std::max(1e-4f, (float)std::cbrt(std::max(1e-9f, (float)(box.size.x + 1e-3f) * (float)(box.size.y + 1e-3f) * (float)(box.size.z + 1e-3f)) / (float)src.faces.size()) * 2.0f);
	auto key = [&](int x, int y, int z) { return ((uint64_t)(uint32_t)(x + 1000000) << 42) ^ ((uint64_t)(uint32_t)(y + 1000000) << 21) ^ (uint64_t)(uint32_t)(z + 1000000); };
	std::unordered_map<uint64_t, std::vector<int>> grid;
	for (size_t f = 0; f < sc.size(); f++) {
		const Vector3 q = sc[f] / cell;
		grid[key((int)std::floor(q.x), (int)std::floor(q.y), (int)std::floor(q.z))].push_back((int)f);
	}
	auto nearest = [&](const Vector3 &p) {
		const Vector3 q = p / cell;
		const int cx = (int)std::floor(q.x), cy = (int)std::floor(q.y), cz = (int)std::floor(q.z);
		int best = -1;
		float bd = 1e30f;
		for (int r = 0; r < 64 && best < 0; r++) {
			for (int x = cx - r; x <= cx + r; x++) {
				for (int y = cy - r; y <= cy + r; y++) {
					for (int z = cz - r; z <= cz + r; z++) {
						if (std::max({ std::abs(x - cx), std::abs(y - cy), std::abs(z - cz) }) != r) {
							continue;
						}
						auto it = grid.find(key(x, y, z));
						if (it == grid.end()) {
							continue;
						}
						for (int f : it->second) {
							const float d = sc[f].distance_squared_to(p);
							if (d < bd) {
								bd = d;
								best = f;
							}
						}
					}
				}
			}
		}
		return best < 0 ? 0 : best;
	};
	for (Face &F : dst.faces) {
		const int sf = nearest(face_center(dst, F));
		const Face &S = src.faces[sf];
		F.material = S.material;
		F.uvp = S.uvp;
		F.uvp.axis = -1;
		F.uv_group = 0;
		F.manual_uv = false;
		if (p_smoothing) {
			F.smooth = S.smooth;
		}
		if (p_colors) {
			F.ensure_attrs();
			const bool cust = S.cust.size() == S.v.size() * 4;
			if (cust) {
				F.cust.assign(F.v.size() * 4, Color(0, 0, 0, 0));
			}
			for (int i = 0; i < F.size(); i++) {
				int bc = 0;
				float bd = 1e30f;
				for (int k = 0; k < S.size(); k++) {
					const float d = src.pos[S.v[k]].distance_squared_to(dst.pos[F.v[i]]);
					if (d < bd) {
						bd = d;
						bc = k;
					}
				}
				if (bc < (int)S.col.size()) {
					F.col[i] = S.col[bc];
				}
				for (int q = 0; cust && q < 4; q++) {
					F.cust[i * 4 + q] = S.cust[bc * 4 + q];
				}
			}
		}
	}
}

} // namespace ops
} // namespace hewn
