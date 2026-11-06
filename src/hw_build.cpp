// UV projection and conversion of the polygon mesh into renderable triangle surfaces.
#include "hw_core.h"

#include <functional>

#include <map>
#include <unordered_map>
#include <unordered_set>

namespace hewn {

void uv_axes(const Vector3 &n, Vector3 &u, Vector3 &v) {
	const float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
	Vector3 up;
	// The tolerance keeps 45-degree faces (e.g. the middle strip of an odd rounded bevel) from
	// flipping between projections because of float noise.
	if (ay > ax + 1e-4f && ay > az + 1e-4f) {
		up = n.y > 0 ? Vector3(0, 0, -1) : Vector3(0, 0, 1);
	} else {
		up = Vector3(0, 1, 0);
	}
	u = up.cross(n);
	if (u.length_squared() < 1e-12f) {
		u = Vector3(1, 0, 0);
	}
	u.normalize();
	v = n.cross(u).normalized();
}

// World UVs project like classic brush editors (the paraxial projection): onto the world plane
// whose axis is nearest to the face's normal (floor / ceiling first on a tie), not onto the face itself. A
// sloped face shows the texture stretched as seen from that axis, faces of one brush meet without seams, and
// moving a vertex only stretches the texture until the face turns past 45 degrees, where it takes the next axis.
static const Vector3 PARAXIAL[6] = { Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1) };

int paraxial_index(const Vector3 &n, int p_keep) {
	const float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
	const float tie = 1e-4f;
	int best;
	if (ay >= ax - tie && ay >= az - tie) {
		best = n.y >= 0.0f ? 2 : 3;
	} else if (ax >= az - tie) {
		best = n.x >= 0.0f ? 0 : 1;
	} else {
		best = n.z >= 0.0f ? 4 : 5;
	}
	if (p_keep >= 0 && p_keep < 6 && p_keep != best && n.dot(PARAXIAL[p_keep]) >= n.dot(PARAXIAL[best]) - tie) {
		return p_keep; // (not past the middle yet: the one it had)
	}
	return best;
}

Vector3 paraxial_axis(const Vector3 &n, int p_keep) {
	return PARAXIAL[paraxial_index(n, p_keep)];
}

void update_paraxial_axes(Mesh &m, const Transform3D &xf) {
	for (Face &F : m.faces) {
		if (!F.uvp.world || F.manual_uv || F.uv_group > 0 || F.size() < 3) {
			continue;
		}
		const Vector3 n = xf.basis.xform(face_normal_raw(m, F));
		if (n.length_squared() < 1e-20f) {
			continue;
		}
		F.uvp.axis = paraxial_index(n.normalized(), F.uvp.axis);
	}
}

static inline Vector2 rot2(const Vector2 &p, float s, float c) {
	return Vector2(p.x * c - p.y * s, p.x * s + p.y * c);
}

void compute_uvs(const Mesh &m, const Transform3D &xf, std::vector<std::vector<Vector2>> &out) {
	const int nf = (int)m.faces.size();
	out.assign(nf, {});
	// Group normals (area weighted) for texture groups.
	std::unordered_map<int, Vector3> group_n;
	std::unordered_map<int, Vector3> group_nw;
	for (int f = 0; f < nf; f++) {
		const Face &F = m.faces[f];
		if (F.manual_uv || F.uv_group <= 0) {
			continue;
		}
		const Vector3 n = face_normal_raw(m, F);
		group_n[F.uv_group] += n;
		group_nw[F.uv_group] += xf.basis.xform(n);
	}
	// Raw planar projection.
	std::vector<std::vector<Vector2>> raw(nf);
	for (int f = 0; f < nf; f++) {
		const Face &F = m.faces[f];
		const int c = F.size();
		if (F.manual_uv) {
			out[f] = F.uv;
			out[f].resize(c, Vector2());
			continue;
		}
		Vector3 n;
		if (F.uv_group > 0) {
			n = F.uvp.world ? group_nw[F.uv_group] : group_n[F.uv_group];
		} else {
			n = face_normal_raw(m, F);
			if (F.uvp.world) {
				n = xf.basis.xform(n);
			}
		}
		if (n.length_squared() < 1e-20f) {
			n = Vector3(0, 1, 0);
		}
		n.normalize();
		if (F.uvp.world) {
			n = paraxial_axis(n, F.uv_group > 0 ? -1 : F.uvp.axis);
		}
		Vector3 u, v;
		uv_axes(n, u, v);
		raw[f].resize(c);
		for (int i = 0; i < c; i++) {
			Vector3 p = m.pos[F.v[i]];
			if (F.uvp.world) {
				p = xf.xform(p);
			}
			raw[f][i] = Vector2(p.dot(u), -p.dot(v));
		}
	}
	// Bounds for fit/stretch (per face or per group).
	struct B {
		Vector2 mn = Vector2(1e30f, 1e30f), mx = Vector2(-1e30f, -1e30f);
	};
	std::unordered_map<int, B> group_b;
	for (int f = 0; f < nf; f++) {
		const Face &F = m.faces[f];
		if (F.manual_uv || F.uv_group <= 0) {
			continue;
		}
		B &b = group_b[F.uv_group];
		for (const Vector2 &p : raw[f]) {
			b.mn = Vector2(std::min(b.mn.x, p.x), std::min(b.mn.y, p.y));
			b.mx = Vector2(std::max(b.mx.x, p.x), std::max(b.mx.y, p.y));
		}
	}
	for (int f = 0; f < nf; f++) {
		const Face &F = m.faces[f];
		if (F.manual_uv) {
			continue;
		}
		const UVParams &P = F.uvp;
		std::vector<Vector2> &r = raw[f];
		Vector2 center(0, 0);
		if (P.fill != UV_FILL_TILE) {
			B b;
			if (F.uv_group > 0) {
				b = group_b[F.uv_group];
			} else {
				for (const Vector2 &p : r) {
					b.mn = Vector2(std::min(b.mn.x, p.x), std::min(b.mn.y, p.y));
					b.mx = Vector2(std::max(b.mx.x, p.x), std::max(b.mx.y, p.y));
				}
			}
			Vector2 size = b.mx - b.mn;
			if (P.fill == UV_FILL_FIT) {
				float s = std::max(size.x, size.y);
				if (s < 1e-9f) {
					s = 1;
				}
				for (Vector2 &p : r) {
					p = (p - b.mn) / s;
				}
			} else {
				if (size.x < 1e-9f) {
					size.x = 1;
				}
				if (size.y < 1e-9f) {
					size.y = 1;
				}
				for (Vector2 &p : r) {
					p = (p - b.mn) / size;
				}
			}
			center = Vector2(0.5f, 0.5f);
		}
		const float a = P.rotation * (float)(godot::Math::PI / 180.0);
		const float s = std::sin(a), co = std::cos(a);
		for (Vector2 &p : r) {
			Vector2 q = p;
			if (P.swap) {
				q = Vector2(q.y, q.x);
			}
			if (P.flip_u) {
				q.x = 2.0f * center.x - q.x;
			}
			if (P.flip_v) {
				q.y = 2.0f * center.y - q.y;
			}
			q = rot2(q - center, s, co);
			q = Vector2(q.x * P.scale.x, q.y * P.scale.y) + center + P.offset;
			p = q;
		}
		out[f] = r;
	}
}

// The planar projection of a point of a face (before its UV parameters), as compute_uvs does it.
static Vector2 raw_uv(const Mesh &m, const Face &F, const Transform3D &xf, const Vector3 &p_local) {
	Vector3 n = face_normal_raw(m, F);
	Vector3 p = p_local;
	if (F.uvp.world) {
		n = xf.basis.xform(n);
		p = xf.xform(p);
	}
	if (n.length_squared() < 1e-20f) {
		n = Vector3(0, 1, 0);
	}
	n.normalize();
	if (F.uvp.world) {
		n = paraxial_axis(n, F.uvp.axis);
	}
	Vector3 u, v;
	uv_axes(n, u, v);
	return Vector2(p.dot(u), -p.dot(v));
}

// Tiled faces: rotation and offset so that `F` maps `p0` -> `f0` and the direction p0 -> p1 to f1 - f0.
static void place_uv(const Mesh &m, Face &F, const Transform3D &xf, const Vector3 &p0, const Vector3 &p1, const Vector2 &f0, const Vector2 &f1) {
	const Vector2 r0 = raw_uv(m, F, xf, p0), r1 = raw_uv(m, F, xf, p1);
	const Vector2 sc(std::fabs(F.uvp.scale.x) > 1e-6f ? F.uvp.scale.x : 1.0f, std::fabs(F.uvp.scale.y) > 1e-6f ? F.uvp.scale.y : 1.0f);
	const Vector2 g((f1 - f0).x / sc.x, (f1 - f0).y / sc.y);
	const float a = std::atan2(g.y, g.x) - std::atan2((r1 - r0).y, (r1 - r0).x);
	F.uvp.rotation = a * (float)(180.0 / godot::Math::PI);
	const Vector2 q = rot2(r0, std::sin(a), std::cos(a));
	F.uvp.offset = f0 - Vector2(q.x * sc.x, q.y * sc.y);
}

void face_projection_bounds(const Mesh &m, const Face &F, const Transform3D &xf, Vector2 &r_min, Vector2 &r_max) {
	r_min = Vector2(1e30f, 1e30f);
	r_max = Vector2(-1e30f, -1e30f);
	for (int v : F.v) {
		const Vector2 q = raw_uv(m, F, xf, m.pos[v]);
		r_min = r_min.min(q);
		r_max = r_max.max(q);
	}
}

bool wrap_uv(Mesh &m, int src, int dst, const Transform3D &xf, bool material) {
	if (src < 0 || dst < 0 || src >= (int)m.faces.size() || dst >= (int)m.faces.size() || src == dst) {
		return false;
	}
	const Face S = m.faces[src];
	Face &D = m.faces[dst];
	if (material) {
		D.material = S.material;
	}
	D.uvp = S.uvp;
	D.manual_uv = false;
	D.uv_group = 0;
	if (S.manual_uv || S.uvp.fill != UV_FILL_TILE || S.uvp.swap || S.uvp.flip_u || S.uvp.flip_v) {
		return true; // (the same settings)
	}
	// a shared edge: the texture runs on across it
	int a = -1, b = -1;
	for (int i = 0; i < S.size() && a < 0; i++) {
		const int x = S.v[i], y = S.v[(i + 1) % S.size()];
		for (int k = 0; k < D.size(); k++) {
			const int p = D.v[k], q = D.v[(k + 1) % D.size()];
			if ((p == x && q == y) || (p == y && q == x)) {
				a = x;
				b = y;
				break;
			}
		}
	}
	if (a < 0) {
		return true; // not neighbors: the same projection (seamless on the same plane)
	}
	std::vector<std::vector<Vector2>> uvs;
	compute_uvs(m, xf, uvs);
	Vector2 f0, f1;
	for (int i = 0; i < S.size(); i++) {
		if (S.v[i] == a) {
			f0 = uvs[src][i];
		}
		if (S.v[i] == b) {
			f1 = uvs[src][i];
		}
	}
	place_uv(m, D, xf, m.pos[a], m.pos[b], f0, f1);
	return true;
}

// The texture of `src` (as it shows now: projected or its own UVs) runs on across their shared edge onto
// `dst`, as if `dst` were folded flat into its plane; `dst` gets them as its own UVs.
bool continue_uv(Mesh &m, int src, int dst, const std::vector<std::vector<Vector2>> &uvs) {
	if (src < 0 || dst < 0 || src >= (int)m.faces.size() || dst >= (int)m.faces.size() || src == dst || src >= (int)uvs.size()) {
		return false;
	}
	const Face &S = m.faces[src];
	Face &D = m.faces[dst];
	int ia = -1, ib = -1; // the shared edge's corners in S
	for (int i = 0; i < S.size() && ia < 0; i++) {
		const int x = S.v[i], y = S.v[(i + 1) % S.size()];
		for (int k = 0; k < D.size(); k++) {
			const int p = D.v[k], q = D.v[(k + 1) % D.size()];
			if ((p == x && q == y) || (p == y && q == x)) {
				ia = i;
				ib = (i + 1) % S.size();
				break;
			}
		}
	}
	if (ia < 0 || uvs[src].size() != S.v.size()) {
		return false;
	}
	const Vector3 A = m.pos[S.v[ia]], B = m.pos[S.v[ib]];
	const Vector3 e = (B - A).normalized();
	const Vector3 ns = face_normal(m, S).normalized(), nd = face_normal(m, D).normalized();
	if (e.length() < 0.5f || ns.length() < 0.5f || nd.length() < 0.5f) {
		return false;
	}
	// S's map from its plane (along the edge, across it) to its UVs: from A, B and its corner furthest from the edge
	const Vector3 w = ns.cross(e);
	auto plane = [&](const Vector3 &p) { return Vector2((p - A).dot(e), (p - A).dot(w)); };
	int ic = -1;
	float far = 1e-6f;
	for (int i = 0; i < S.size(); i++) {
		const float d = std::fabs(plane(m.pos[S.v[i]]).y);
		if (d > far) {
			far = d;
			ic = i;
		}
	}
	if (ic < 0) {
		return false;
	}
	const Vector2 qb = plane(B), qc = plane(m.pos[S.v[ic]]);
	const float det = qb.x * qc.y - qc.x * qb.y;
	if (std::fabs(det) < 1e-10f) {
		return false;
	}
	const Vector2 ua = uvs[src][ia], db = uvs[src][ib] - ua, dc = uvs[src][ic] - ua;
	// M [qb qc] = [db dc]
	auto map = [&](const Vector2 &q) {
		const float s = (q.x * qc.y - qc.x * q.y) / det, t = (qb.x * q.y - q.x * qb.y) / det;
		return ua + db * s + dc * t;
	};
	// D folded about the edge into S's plane: its side away from S
	const Vector3 wd = nd.cross(e); // (across the edge, in D's plane)
	std::vector<Vector2> out;
	for (int v : D.v) {
		const Vector3 p = m.pos[v] - A;
		out.push_back(map(Vector2(p.dot(e), p.dot(wd))));
	}
	D.uv = out;
	D.manual_uv = true;
	D.uv_group = 0;
	D.material = S.material;
	return true;
}

// New faces (a bevel's) take the texture of the faces around them, running on across their edges: each from
// the neighbor already done that turns least from it.
void continue_uvs(Mesh &m, const std::vector<int> &faces, const Transform3D &xf) {
	std::unordered_set<int> todo;
	for (int f : faces) {
		if (f >= 0 && f < (int)m.faces.size()) {
			todo.insert(f);
		}
	}
	if (todo.empty()) {
		return;
	}
	std::vector<std::vector<Vector2>> uvs;
	compute_uvs(m, xf, uvs);
	std::unordered_map<uint64_t, std::vector<int>> by_edge;
	for (int f = 0; f < (int)m.faces.size(); f++) {
		const Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			by_edge[ekey(F.v[i], F.v[(i + 1) % F.size()])].push_back(f);
		}
	}
	bool progress = true;
	while (progress && !todo.empty()) {
		progress = false;
		std::vector<std::pair<int, int>> step; // (dst, src) of this round
		for (int f : faces) {
			if (!todo.count(f)) {
				continue;
			}
			const Face &F = m.faces[f];
			const Vector3 nf = face_normal(m, F).normalized();
			int best = -1;
			float bd = -2.0f;
			for (int i = 0; i < F.size(); i++) {
				for (int g : by_edge[ekey(F.v[i], F.v[(i + 1) % F.size()])]) {
					if (g == f || todo.count(g)) {
						continue;
					}
					const float d = face_normal(m, m.faces[g]).normalized().dot(nf);
					if (d > bd) {
						bd = d;
						best = g;
					}
				}
			}
			if (best >= 0) {
				step.push_back({ f, best });
			}
		}
		for (const std::pair<int, int> &p : step) {
			const Face &S = m.faces[p.second];
			if (!S.manual_uv && S.uvp.world) {
				// (from world space: the new face is projected the same way, it joins up by itself and stays
				// in world space - an object moved later keeps it)
				Face &D = m.faces[p.first];
				D.manual_uv = false;
				D.uvp = S.uvp;
				D.uv_group = 0;
				todo.erase(p.first);
				progress = true;
				continue;
			}
			if (continue_uv(m, p.second, p.first, uvs)) {
				uvs[p.first] = m.faces[p.first].uv;
			}
			todo.erase(p.first);
			progress = true;
		}
	}
}

void align_uv_to_edge(Mesh &m, int face, int a, int b, int side, const Transform3D &xf) {
	if (face < 0 || face >= (int)m.faces.size()) {
		return;
	}
	Face &F = m.faces[face];
	F.manual_uv = false;
	F.uv_group = 0;
	F.uvp.fill = UV_FILL_TILE;
	F.uvp.swap = F.uvp.flip_u = F.uvp.flip_v = false;
	// the edge along U, the face above it (image V grows downwards); `side` turns by quarters
	const Vector2 r0 = raw_uv(m, F, xf, m.pos[a]), r1 = raw_uv(m, F, xf, m.pos[b]);
	const Vector2 rc = raw_uv(m, F, xf, face_center(m, F));
	float ang = -std::atan2((r1 - r0).y, (r1 - r0).x);
	if (rot2(rc - r0, std::sin(ang), std::cos(ang)).y > 0.0f) {
		ang += (float)godot::Math::PI;
	}
	ang += side * (float)(godot::Math::PI * 0.5);
	F.uvp.rotation = ang * (float)(180.0 / godot::Math::PI);
	const Vector2 q = rot2(r0, std::sin(ang), std::cos(ang));
	F.uvp.offset = -Vector2(q.x * F.uvp.scale.x, q.y * F.uvp.scale.y);
}

void set_face_uvs(Mesh &m, int face, const Transform3D &xf, const std::vector<Vector2> &uv) {
	if (face < 0 || face >= (int)m.faces.size()) {
		return;
	}
	Face &F = m.faces[face];
	const int c = F.size();
	if ((int)uv.size() != c) {
		return;
	}
	auto manual = [&]() {
		F.manual_uv = true;
		F.uv = uv;
	};
	// uv = M * raw + t, fitted (exact for a planar face whose UVs are an affine image of it)
	std::vector<Vector2> raw(c);
	Vector2 rc, uc;
	for (int i = 0; i < c; i++) {
		raw[i] = raw_uv(m, F, xf, m.pos[F.v[i]]);
		rc += raw[i];
		uc += uv[i];
	}
	rc /= (float)c;
	uc /= (float)c;
	double a00 = 0, a01 = 0, a11 = 0, b00 = 0, b01 = 0, b10 = 0, b11 = 0;
	for (int i = 0; i < c; i++) {
		const Vector2 r = raw[i] - rc, u = uv[i] - uc;
		a00 += r.x * r.x;
		a01 += r.x * r.y;
		a11 += r.y * r.y;
		b00 += u.x * r.x;
		b01 += u.x * r.y;
		b10 += u.y * r.x;
		b11 += u.y * r.y;
	}
	const double det = a00 * a11 - a01 * a01;
	if (c < 3 || std::fabs(det) < 1e-12 * std::max(1.0, (a00 + a11) * (a00 + a11))) {
		manual();
		return;
	}
	// M = B * A^-1 (rows: the U and the V of the UVs)
	const double i00 = a11 / det, i01 = -a01 / det, i11 = a00 / det;
	const Vector2 row0((float)(b00 * i00 + b01 * i01), (float)(b00 * i01 + b01 * i11));
	const Vector2 row1((float)(b10 * i00 + b11 * i01), (float)(b10 * i01 + b11 * i11));
	const Vector2 t = uc - Vector2(row0.dot(rc), row1.dot(rc));
	float err = 0.0f, span = 1e-6f;
	for (int i = 0; i < c; i++) {
		err = std::max(err, (Vector2(row0.dot(raw[i]), row1.dot(raw[i])) + t).distance_to(uv[i]));
		span = std::max(span, uv[i].distance_to(uc));
	}
	// M = diag(sx, sy) * R(a): row0 = sx (cos, -sin), row1 = sy (sin, cos)
	const float sx = row0.length();
	if (err > 1e-4f * span || sx < 1e-9f) {
		manual();
		return;
	}
	const float ang = std::atan2(-row0.y, row0.x);
	const Vector2 dir1(std::sin(ang), std::cos(ang));
	const float sy = row1.dot(dir1);
	if ((row1 - dir1 * sy).length() > 1e-4f * std::max(std::fabs(sy), sx) || std::fabs(sy) < 1e-9f) {
		manual(); // skewed: no turn and scale of the projection gives it
		return;
	}
	F.manual_uv = false;
	F.uv_group = 0;
	F.uvp.fill = UV_FILL_TILE;
	F.uvp.swap = F.uvp.flip_u = F.uvp.flip_v = false;
	F.uvp.rotation = ang * (float)(180.0 / godot::Math::PI);
	F.uvp.scale = Vector2(sx, sy);
	F.uvp.offset = t;
}

std::vector<int> unwrap_quads(Mesh &m, const std::vector<int> &faces, int start) {
	std::unordered_set<int> in;
	for (int f : faces) {
		if (f >= 0 && f < (int)m.faces.size() && m.faces[f].size() == 4) {
			in.insert(f);
		}
	}
	if (in.empty()) {
		return {};
	}
	if (!in.count(start)) {
		start = *in.begin();
		for (int f : faces) {
			if (in.count(f)) {
				start = f;
				break;
			}
		}
	}
	std::unordered_map<uint64_t, std::vector<int>> edge_faces;
	for (int f : in) {
		const Face &F = m.faces[f];
		for (int i = 0; i < 4; i++) {
			edge_faces[ekey(F.v[i], F.v[(i + 1) % 4])].push_back(f);
		}
	}
	std::unordered_map<int, std::vector<Vector2>> uvs;
	std::vector<int> order;
	auto len = [&](int a, int b) { return m.pos[a].distance_to(m.pos[b]); };
	// each island of quads from its first face: corner 0 at the origin, edge 0-1 along U, the face
	// above it (V grows downwards in images)
	std::vector<int> pending;
	for (int seed_face : faces) {
		if (!in.count(seed_face) || uvs.count(seed_face)) {
			continue;
		}
		if (order.empty() && seed_face != start) {
			seed_face = start;
		}
		const Face &S = m.faces[seed_face];
		const float w = len(S.v[0], S.v[1]);
		std::vector<Vector2> su = { Vector2(0, 0), Vector2(w, 0), Vector2(w, -len(S.v[1], S.v[2])), Vector2(0, -len(S.v[0], S.v[3])) };
		uvs[seed_face] = su;
		order.push_back(seed_face);
		pending.push_back(seed_face);
		while (!pending.empty()) {
			const int f = pending.back();
			pending.pop_back();
			const Face &A = m.faces[f];
			const std::vector<Vector2> &au = uvs[f];
			for (int i = 0; i < 4; i++) {
				const int x = A.v[i], y = A.v[(i + 1) % 4];
				auto ea = m.edge_attrs.find(ekey(x, y));
				if (ea != m.edge_attrs.end() && ea->second.seam) {
					continue;
				}
				for (int g : edge_faces[ekey(x, y)]) {
					if (g == f || uvs.count(g)) {
						continue;
					}
					const Face &B = m.faces[g];
					int k = -1; // B's corner k, k + 1 is the shared edge (y, x in B's order)
					for (int j = 0; j < 4; j++) {
						if (B.v[j] == y && B.v[(j + 1) % 4] == x) {
							k = j;
						}
					}
					if (k < 0) {
						continue; // (flipped neighbor)
					}
					const Vector2 uy = au[(i + 1) % 4], ux = au[i];
					const Vector2 e = ux - uy;
					if (e.length() < 1e-9f) {
						continue;
					}
					// away from A: A's other corners are on one side of the edge
					Vector2 nrm = Vector2(-e.y, e.x).normalized();
					const Vector2 other = au[(i + 2) % 4];
					if (nrm.dot(other - uy) > 0.0f) {
						nrm = -nrm;
					}
					std::vector<Vector2> bu(4);
					bu[k] = uy;
					bu[(k + 1) % 4] = ux;
					bu[(k + 2) % 4] = ux + nrm * len(B.v[(k + 1) % 4], B.v[(k + 2) % 4]);
					bu[(k + 3) % 4] = uy + nrm * len(B.v[k], B.v[(k + 3) % 4]);
					uvs[g] = bu;
					order.push_back(g);
					pending.push_back(g);
				}
			}
		}
	}
	for (int f : order) {
		m.faces[f].manual_uv = true;
		m.faces[f].uv = uvs[f];
	}
	return order;
}

namespace {
struct SmoothKey {
	int v, g, fan; // fan: faces around v separated by sharp edges get their own normal
	bool operator==(const SmoothKey &o) const { return v == o.v && g == o.g && fan == o.fan; }
};
struct SmoothHash {
	size_t operator()(const SmoothKey &k) const { return ((size_t)k.v * 1000003u ^ (size_t)k.g) * 31u + (size_t)k.fan; }
};
} // namespace

void build_mesh(const Mesh &m, const Transform3D &xf, BuildOut &out, bool p_skip_hidden, const std::vector<std::vector<Vector2>> *uv2) {
	const int nf = (int)m.faces.size();
	out.surfaces.clear();
	out.face_tris.assign(nf, {});
	out.face_normals.assign(nf, Vector3(0, 1, 0));
	compute_uvs(m, xf, out.face_uvs);

	for (int f = 0; f < nf; f++) {
		out.face_normals[f] = face_normal(m, m.faces[f]);
	}

	// Fans around vertices that touch sharp edges: faces joined across non-sharp edges share a normal.
	std::unordered_map<uint64_t, int> corner_fan; // hkey(face, vertex) -> fan id
	if (!m.edge_attrs.empty()) {
		std::unordered_set<int> sharp_v;
		for (const auto &kv : m.edge_attrs) {
			if (kv.second.sharp) {
				sharp_v.insert((int)(kv.first >> 32));
				sharp_v.insert((int)(kv.first & 0xffffffffu));
			}
		}
		if (!sharp_v.empty()) {
			std::unordered_map<int, std::vector<int>> vfaces;
			for (int f = 0; f < nf; f++) {
				for (int v : m.faces[f].v) {
					if (sharp_v.count(v)) {
						vfaces[v].push_back(f);
					}
				}
			}
			for (auto &kv : vfaces) {
				const int v = kv.first;
				std::vector<int> &fs = kv.second;
				std::unordered_map<int, int> parent;
				for (int f : fs) {
					parent[f] = f;
				}
				std::function<int(int)> find = [&](int x) {
					while (parent[x] != x) {
						x = parent[x] = parent[parent[x]];
					}
					return x;
				};
				// neighbors of v in each face; two faces sharing a non-sharp edge v-x are joined
				std::unordered_map<int, std::vector<int>> by_edge;
				for (int f : fs) {
					const Face &F = m.faces[f];
					for (int i = 0; i < F.size(); i++) {
						if (F.v[i] != v) {
							continue;
						}
						for (int x : { F.v[(i + 1) % F.size()], F.v[(i + F.size() - 1) % F.size()] }) {
							const EdgeAttr *ea = m.edge_attr(v, x);
							if (!ea || !ea->sharp) {
								by_edge[x].push_back(f);
							}
						}
					}
				}
				for (auto &e : by_edge) {
					for (size_t i = 1; i < e.second.size(); i++) {
						parent[find(e.second[i])] = find(e.second[0]);
					}
				}
				for (int f : fs) {
					corner_fan[hkey(f, v)] = find(f) + 1;
				}
			}
		}
	}
	auto fan_of = [&](int f, int v) {
		auto it = corner_fan.find(hkey(f, v));
		return it == corner_fan.end() ? 0 : it->second;
	};

	// Smooth normals: angle-weighted per (vertex, group, fan), only from the strongest faces there
	//.
	std::unordered_map<SmoothKey, Vector3, SmoothHash> smooth;
	std::unordered_map<SmoothKey, int, SmoothHash> strongest;
	for (int f = 0; f < nf; f++) {
		const Face &F = m.faces[f];
		if (F.smooth == 0) {
			continue;
		}
		for (int i = 0; i < F.size(); i++) {
			auto it = strongest.emplace(SmoothKey{ F.v[i], F.smooth, fan_of(f, F.v[i]) }, F.strength).first;
			it->second = std::max(it->second, F.strength);
		}
	}
	for (int f = 0; f < nf; f++) {
		const Face &F = m.faces[f];
		if (F.smooth == 0) {
			continue;
		}
		const int c = F.size();
		for (int i = 0; i < c; i++) {
			if (strongest[SmoothKey{ F.v[i], F.smooth, fan_of(f, F.v[i]) }] > F.strength) {
				continue;
			}
			const Vector3 &p = m.pos[F.v[i]];
			Vector3 e0 = m.pos[F.v[(i + c - 1) % c]] - p;
			Vector3 e1 = m.pos[F.v[(i + 1) % c]] - p;
			const float l0 = e0.length(), l1 = e1.length();
			float w = 1.0f;
			if (l0 > 1e-12f && l1 > 1e-12f) {
				float d = e0.dot(e1) / (l0 * l1);
				d = std::max(-1.0f, std::min(1.0f, d));
				w = std::acos(d);
			}
			smooth[SmoothKey{ F.v[i], F.smooth, fan_of(f, F.v[i]) }] += out.face_normals[f] * w;
		}
	}
	for (auto &kv : smooth) {
		const float l = kv.second.length();
		kv.second = l > 1e-12f ? kv.second / l : Vector3(0, 1, 0);
	}

	std::map<int, int> mat_to_surface;
	std::vector<Vector3> pts;
	std::vector<int> tris;
	bool any_custom = false; // (CUSTOM0..3 painted on some face: every vertex gets them)
	for (const Face &F : m.faces) {
		any_custom = any_custom || !F.cust.empty();
	}
	for (int f = 0; f < nf; f++) {
		const Face &F = m.faces[f];
		const int c = F.size();
		if (c < 3 || (p_skip_hidden && F.hidden)) {
			continue; // (hidden: no triangles, so not picked either)
		}
		face_points(m, F, pts);
		tris.clear();
		triangulate(pts, out.face_normals[f], tris);
		out.face_tris[f] = tris;

		int mat = std::max(0, F.material);
		auto it = mat_to_surface.find(mat);
		int si;
		if (it == mat_to_surface.end()) {
			si = (int)out.surfaces.size();
			mat_to_surface[mat] = si;
			out.surfaces.push_back(SurfaceOut());
			out.surfaces.back().material = mat;
		} else {
			si = it->second;
		}
		SurfaceOut &S = out.surfaces[si];
		const int base = (int)S.verts.size();
		const std::vector<Vector2> &fuv = out.face_uvs[f];
		for (int i = 0; i < c; i++) {
			S.verts.push_back(pts[i]);
			Vector3 n = out.face_normals[f];
			if (F.smooth != 0) {
				auto sit = smooth.find(SmoothKey{ F.v[i], F.smooth, fan_of(f, F.v[i]) });
				if (sit != smooth.end()) {
					n = sit->second;
				}
			}
			S.normals.push_back(n);
			S.uvs.push_back(i < (int)fuv.size() ? fuv[i] : Vector2());
			if (uv2) {
				S.uv2s.push_back(f < (int)uv2->size() && i < (int)(*uv2)[f].size() ? (*uv2)[f][i] : Vector2());
			}
			S.colors.push_back(i < (int)F.col.size() ? F.col[i] : Color(1, 1, 1, 1));
			if (any_custom) {
				for (int k = 0; k < 4; k++) {
					S.customs.push_back(F.cust.size() >= (size_t)(i + 1) * 4 ? F.cust[i * 4 + k] : Color(0, 0, 0, 0));
				}
			}
		}
		// tangents (per face corner)
		std::vector<Vector3> tan(c), bit(c);
		for (size_t t = 0; t + 2 < tris.size(); t += 3) {
			const int i0 = tris[t], i1 = tris[t + 1], i2 = tris[t + 2];
			const Vector3 e1 = pts[i1] - pts[i0], e2 = pts[i2] - pts[i0];
			const Vector2 d1 = S.uvs[base + i1] - S.uvs[base + i0];
			const Vector2 d2 = S.uvs[base + i2] - S.uvs[base + i0];
			const float r = d1.x * d2.y - d2.x * d1.y;
			if (std::fabs(r) < 1e-12f) {
				continue;
			}
			const float ir = 1.0f / r;
			const Vector3 tt = (e1 * d2.y - e2 * d1.y) * ir;
			const Vector3 bb = (e2 * d1.x - e1 * d2.x) * ir;
			tan[i0] += tt;
			tan[i1] += tt;
			tan[i2] += tt;
			bit[i0] += bb;
			bit[i1] += bb;
			bit[i2] += bb;
		}
		for (int i = 0; i < c; i++) {
			const Vector3 &n = S.normals[base + i];
			Vector3 t = tan[i] - n * n.dot(tan[i]);
			if (t.length_squared() < 1e-20f) {
				Vector3 b2;
				plane_basis(n, t, b2);
			}
			t.normalize();
			const float w = n.cross(t).dot(-bit[i]) < 0.0f ? -1.0f : 1.0f;
			S.tangents.push_back(t.x);
			S.tangents.push_back(t.y);
			S.tangents.push_back(t.z);
			S.tangents.push_back(w);
		}
		for (size_t t = 0; t + 2 < tris.size(); t += 3) {
			// flip to Godot's clockwise front faces
			S.indices.push_back(base + tris[t]);
			S.indices.push_back(base + tris[t + 2]);
			S.indices.push_back(base + tris[t + 1]);
		}
	}
}

} // namespace hewn
