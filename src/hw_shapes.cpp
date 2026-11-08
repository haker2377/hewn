// Parametric primitives. Every face gets its own vertices and is oriented by an "outward" hint;
// coincident vertices are welded at the end.
#include "hw_core.h"

#include <array>
#include <cstring>

namespace hewn {
namespace shapes {

namespace {

const float TAU = (float)(godot::Math::PI * 2.0);
const float DEG = (float)(godot::Math::PI / 180.0);

// Role of a face from its direction: curved (smoothed) faces are the surface, flat ones top / bottom /
// front / back / left / right, the others slopes.
int auto_role(const Vector3 &p_normal, int p_smooth) {
	if (p_smooth != 0) {
		return ROLE_SURFACE;
	}
	const Vector3 n = p_normal.normalized();
	if (std::fabs(n.y) >= 0.9f) {
		return n.y > 0.0f ? ROLE_TOP : ROLE_BOTTOM;
	}
	if (std::fabs(n.x) >= 0.9f) {
		return n.x > 0.0f ? ROLE_RIGHT : ROLE_LEFT;
	}
	if (std::fabs(n.z) >= 0.9f) {
		return n.z > 0.0f ? ROLE_FRONT : ROLE_BACK;
	}
	return ROLE_SLOPE;
}

struct Builder {
	Mesh &m;
	Vector3 center; // used by convex() as the inside point
	int role = -1; // FaceRole of the next faces; -1: from their direction

	void poly(const std::vector<Vector3> &pts, const Vector3 &hint, int smooth = 0) {
		if (pts.size() < 3) {
			return;
		}
		std::vector<int> ids;
		for (const Vector3 &p : pts) {
			ids.push_back(m.add_vertex(p));
		}
		Face f = make_face(ids);
		f.smooth = smooth;
		Vector3 n = newell(pts);
		if (n.dot(hint) < 0.0f) {
			f.reverse();
			n = -n;
		}
		f.role = role >= 0 ? role : auto_role(n, smooth);
		m.faces.push_back(f);
	}
	void quad(const Vector3 &a, const Vector3 &b, const Vector3 &c, const Vector3 &d, const Vector3 &hint, int smooth = 0) {
		poly({ a, b, c, d }, hint, smooth);
	}
	void tri(const Vector3 &a, const Vector3 &b, const Vector3 &c, const Vector3 &hint, int smooth = 0) {
		poly({ a, b, c }, hint, smooth);
	}
	// Hint pointing away from `center`.
	void convex(const std::vector<Vector3> &pts, int smooth = 0) {
		Vector3 c;
		for (const Vector3 &p : pts) {
			c += p;
		}
		c /= (float)pts.size();
		poly(pts, c - center, smooth);
	}
	// Planar grid from origin spanning u and v.
	void grid(const Vector3 &o, const Vector3 &u, const Vector3 &v, int nu, int nv, const Vector3 &hint) {
		nu = std::max(1, nu);
		nv = std::max(1, nv);
		for (int i = 0; i < nu; i++) {
			for (int j = 0; j < nv; j++) {
				const Vector3 p = o + u * ((float)i / nu) + v * ((float)j / nv);
				const Vector3 du = u / (float)nu, dv = v / (float)nv;
				quad(p, p + du, p + du + dv, p + dv, hint);
			}
		}
	}
	// Surface of revolution around Y. prof = (radius, y), ordered bottom to top.
	void lathe(const std::vector<Vector2> &prof, int sides, int smooth) {
		for (size_t k = 0; k + 1 < prof.size(); k++) {
			for (int s = 0; s < sides; s++) {
				const float a0 = TAU * s / sides, a1 = TAU * (s + 1) / sides;
				auto P = [&](const Vector2 &pr, float a) { return Vector3(std::cos(a) * pr.x, pr.y, std::sin(a) * pr.x); };
				convex({ P(prof[k], a0), P(prof[k], a1), P(prof[k + 1], a1), P(prof[k + 1], a0) }, smooth);
			}
		}
	}
	std::vector<Vector3> circle(float r, float y, int sides) {
		std::vector<Vector3> out;
		for (int s = 0; s < sides; s++) {
			const float a = TAU * s / sides;
			out.push_back(Vector3(std::cos(a) * r, y, std::sin(a) * r));
		}
		return out;
	}
};

const std::vector<Param> P_NONE = {};
const std::vector<Param> P_CUBE = { { "width", 1, 0.001f, 1000, false }, { "height", 1, 0.001f, 1000, false }, { "depth", 1, 0.001f, 1000, false },
	{ "width_segments", 1, 1, 64, true }, { "height_segments", 1, 1, 64, true }, { "depth_segments", 1, 1, 64, true }, { "flip_normals", 0, 0, 1, true } };
const std::vector<Param> P_PLANE = { { "width", 2, 0.001f, 1000, false }, { "depth", 2, 0.001f, 1000, false },
	{ "width_segments", 2, 1, 128, true }, { "depth_segments", 2, 1, 128, true }, { "flip_normals", 0, 0, 1, true } };
// (an oval: `radius` along X, `radius_z` along Z; scenes from before it have the same radius there)
const std::vector<Param> P_CYLINDER = { { "radius", 0.5f, 0.001f, 1000, false }, { "height", 1, 0.001f, 1000, false },
	{ "sides", 16, 3, 128, true }, { "height_segments", 1, 1, 64, true }, { "caps", 1, 0, 1, true }, { "flip_normals", 0, 0, 1, true },
	{ "radius_z", 0.5f, 0.001f, 1000, false } };
const std::vector<Param> P_CONE = { { "radius", 0.5f, 0.001f, 1000, false }, { "height", 1, 0.001f, 1000, false },
	{ "sides", 16, 3, 128, true }, { "cap", 1, 0, 1, true }, { "flip_normals", 0, 0, 1, true },
	{ "radius_z", 0, 0, 1000, false } }; // (0: round)
const std::vector<Param> P_SPHERE = { { "radius", 0.5f, 0.001f, 1000, false }, { "rings", 12, 2, 128, true }, { "segments", 24, 3, 256, true }, { "flip_normals", 0, 0, 1, true } };
const std::vector<Param> P_ICOSPHERE = { { "radius", 0.5f, 0.001f, 1000, false }, { "subdivisions", 2, 0, 5, true }, { "flip_normals", 0, 0, 1, true } };
const std::vector<Param> P_TORUS = { { "radius", 0.5f, 0.001f, 1000, false }, { "tube_radius", 0.2f, 0.001f, 1000, false },
	{ "rings", 24, 3, 256, true }, { "sides", 12, 3, 128, true }, { "flip_normals", 0, 0, 1, true } };
const std::vector<Param> P_PIPE = { { "radius", 0.5f, 0.002f, 1000, false }, { "thickness", 0.1f, 0.001f, 1000, false },
	{ "height", 1, 0.001f, 1000, false }, { "sides", 16, 3, 128, true }, { "height_segments", 1, 1, 64, true }, { "flip_normals", 0, 0, 1, true },
	{ "radius_z", 0.5f, 0.002f, 1000, false } };
const std::vector<Param> P_PRISM = { { "width", 1, 0.001f, 1000, false }, { "height", 1, 0.001f, 1000, false }, { "depth", 1, 0.001f, 1000, false },
	{ "ridge_along_long_side", 1, 0, 1, true }, { "flip_normals", 0, 0, 1, true } };
// `curvature` > 0 bends the flight around a circle (degrees of arc), `build_sides` closes the sides.
const std::vector<Param> P_STAIRS = { { "width", 1, 0.001f, 1000, false }, { "height", 1, 0.001f, 1000, false }, { "depth", 2, 0.001f, 1000, false },
	{ "steps", 6, 1, 256, true }, { "build_sides", 1, 0, 1, true }, { "curvature", 0, -1440, 1440, false }, { "inner_radius", 0.5f, 0, 1000, false },
	{ "smooth_collision", 0, 0, 1, true }, { "climb_along_long_side", 1, 0, 1, true }, { "reverse", 0, 0, 1, true }, { "flip_normals", 0, 0, 1, true },
	// (added later, so after flip_normals: the values are read by position)
	{ "fill_by", 0, 0, 1, true, "Step Count,Step Height" }, { "riser_height", 0.18f, 0.01f, 100, false },
	{ "top_of_box", 0, 0, 1, true, "Final Tread,Destination Floor" },
	{ "left_side", 1, 0, 1, true }, { "right_side", 1, 0, 1, true }, { "back", 1, 0, 1, true },
	{ "underside", 0, 0, 2, true, "Filled,Sloped,Open" }, { "underside_thickness", 0.2f, 0.001f, 100, false },
	{ "border_left", 0, 0, 1, true }, { "border_right", 0, 0, 1, true }, { "border_width", 0.1f, 0.001f, 100, false },
	{ "border_height", 0.1f, 0, 100, false }, { "border_outside", 0, 0, 1, true },
	{ "height_fit", 0, 0, 1, true, "Even Risers,Short First Riser" }, { "create_final_riser", 0, 0, 1, true },
	{ "border_alignment", 0, 0, 1, true, "Step Tips,Riser Bottoms" },
	{ "generate_cutter", 0, 0, 1, true }, { "cutter_headroom", 2.2f, 0.01f, 100, false } };
const std::vector<Param> P_CURVED_STAIRS = { { "inner_radius", 0.5f, 0, 1000, false }, { "width", 1, 0.001f, 1000, false },
	{ "height", 1.5f, 0.001f, 1000, false }, { "steps", 8, 1, 256, true }, { "angle", 180, 1, 359, false }, { "build_sides", 1, 0, 1, true }, { "flip_normals", 0, 0, 1, true } };
const std::vector<Param> P_ARCH = { { "radius", 1, 0.002f, 1000, false }, { "thickness", 0.25f, 0.001f, 1000, false },
	{ "depth", 0.5f, 0.001f, 1000, false }, { "segments", 12, 2, 256, true }, { "angle", 180, 1, 360, false },
	{ "opening_on_long_side", 1, 0, 1, true }, { "flip_normals", 0, 0, 1, true } };
const std::vector<Param> P_DOOR = { { "width", 2, 0.01f, 1000, false }, { "height", 2.5f, 0.01f, 1000, false }, { "depth", 0.2f, 0.001f, 1000, false },
	{ "opening_width", 1, 0.001f, 1000, false }, { "opening_height", 2, 0.001f, 1000, false },
	{ "arch_segments", 0, 0, 128, true }, { "arch_height", 0.5f, 0.001f, 1000, false }, { "opening_on_long_side", 1, 0, 1, true }, { "flip_normals", 0, 0, 1, true } };
const std::vector<Param> P_CAPSULE = { { "radius", 0.5f, 0.001f, 1000, false }, { "height", 2, 0.002f, 1000, false },
	{ "sides", 16, 3, 128, true }, { "rings", 4, 1, 64, true }, { "flip_normals", 0, 0, 1, true },
	{ "radius_z", 0, 0, 1000, false } }; // (0: round)
const std::vector<Param> P_DISC = { { "radius", 0.5f, 0.001f, 1000, false }, { "inner_radius", 0, 0, 1000, false },
	{ "sides", 24, 3, 256, true }, { "rings", 1, 1, 64, true }, { "flip_normals", 0, 0, 1, true } };
const std::vector<Param> P_WEDGE = { { "width", 1, 0.001f, 1000, false }, { "height", 1, 0.001f, 1000, false }, { "depth", 1, 0.001f, 1000, false }, { "flip_normals", 0, 0, 1, true } };
const std::vector<Param> P_POLY_SHAPE = { { "height", 1, -1000, 1000, false }, { "flip_normals", 0, 0, 1, true } };
const std::vector<Param> P_WALL = { { "length", 4, 0.001f, 10000, false }, { "height", 2.5f, 0.001f, 1000, false }, { "thickness", 0.2f, 0.001f, 1000, false },
	{ "length_segments", 1, 1, 256, true }, { "height_segments", 1, 1, 256, true }, { "flip_normals", 0, 0, 1, true },
	{ "side", 1, 0, 2, true, "Inside,Center,Outside" } };
const std::vector<Param> P_SWEEP = { { "profile", 0, 0, 7, true, "Circle,Rectangle,Half Round,L (Curb),Cornice,U (Gutter),Custom,Mesh" },
	{ "width", 0.2f, 0.001f, 1000, false }, { "height", 0.2f, 0.001f, 1000, false }, { "sides", 12, 3, 128, true }, { "closed", 0, 0, 1, true },
	{ "caps", 1, 0, 1, true }, { "smooth", 1, 0, 1, true }, { "anchor", 0, 0, 1, true, "Bottom,Center" }, { "corner_radius", 0, 0, 1000, false },
	{ "corner_segments", 4, 1, 64, true }, { "flip_normals", 0, 0, 1, true },
	// (added later: the values are read by position) Outside: the profile beside the path, not across it (a trim
	// along a wall stays against it whatever its width)
	{ "align", 0, 0, 1, true, "Center,Outside" },
	// how far the profile goes below the path (Anchor Bottom): the height is above it
	{ "drop", 0, 0, 1000, false },
	// accents: stretches of a bigger profile (a pilaster, a bracket, a block) at the corners, halfway between them,
	// or every so far; their length along the path, how much wider / taller (from the path), where the first one is
	{ "accent", 0, 0, 5, true, "None,Every Distance,Corners,Middles,Corners and Middles,Corners and Every Distance" },
	{ "accent_step", 2, 0.01f, 1000, false }, { "accent_length", 0.3f, 0.001f, 1000, false }, { "accent_width", 1.5f, 0.01f, 100, false },
	{ "accent_height", 1.5f, 0.01f, 100, false }, { "accent_offset", 0, -1000, 1000, false },
	// profile Mesh: the node's sweep_mesh set along the path (a fence's posts, the brackets under a cornice): where,
	// how far apart, the first one, its size; Fit: as many as fit evenly
	{ "mesh_place", 0, 0, 4, true, "Every Distance,Corners,Middles,Corners and Middles,Corners and Every Distance" },
	{ "mesh_step", 1, 0.01f, 1000, false }, { "mesh_offset", 0, -1000, 1000, false }, { "mesh_scale", 1, 0.001f, 1000, false },
	{ "mesh_fit", 0, 0, 1, true } };
const std::vector<Param> P_COLUMN = { { "radius", 0.25f, 0.001f, 1000, false }, { "height", 3, 0.002f, 1000, false }, { "sides", 16, 3, 128, true },
	{ "base_height", 0.35f, 0, 1000, false }, { "top_height", 0.35f, 0, 1000, false },
	{ "base_style", 2, 0, 3, true, "None,Simple,Plinth,Attic" }, { "shaft_style", 0, 0, 3, true, "Straight,Tapered,Ringed,Bulged" },
	{ "top_style", 2, 0, 3, true, "None,Simple,Capital,Square Slab" }, { "shaft_repeat", 0, 0, 1, true, "Count,Distance" },
	{ "shaft_count", 1, 1, 256, true }, { "shaft_length", 0.5f, 0.01f, 1000, false }, { "smooth", 1, 0, 1, true }, { "flip_normals", 0, 0, 1, true } };
const std::vector<Param> P_PATH_STAIRS = { { "width", 1.2f, 0.01f, 1000, false }, { "height", 2.5f, 0.01f, 1000, false },
	{ "step_height", 0.18f, 0.01f, 10, false }, { "underside", 0, 0, 1, true, "Solid,Thickness" }, { "thickness", 0.2f, 0.01f, 100, false },
	{ "border", 0, 0, 3, true, "None,Left,Right,Both" }, { "border_width", 0.08f, 0.001f, 100, false }, { "border_height", 0.15f, 0, 100, false },
	{ "generate_cutter", 0, 0, 1, true }, { "cutter_headroom", 2.2f, 0.01f, 100, false },
	{ "fill_by", 0, 0, 1, true, "Step Height,Step Count" }, { "steps", 8, 1, 256, true }, { "landing_borders", 0, 0, 1, true },
	{ "flip_normals", 0, 0, 1, true },
	// (added later: the values are read by position)
	{ "smooth_collision", 0, 0, 1, true } };
const std::vector<Param> P_POLYGON = { { "radius", 0.5f, 0.001f, 1000, false }, { "sides", 6, 3, 256, true }, { "height", 0, 0, 1000, false }, { "flip_normals", 0, 0, 1, true } };

// ------------------------------------------------------------------ generators

void make_cube(Builder &b, float w, float h, float d, int sx, int sy, int sz) {
	const Vector3 o(-w / 2, -h / 2, -d / 2);
	const Vector3 X(w, 0, 0), Y(0, h, 0), Z(0, 0, d);
	b.grid(o, X, Y, sx, sy, Vector3(0, 0, -1));
	b.grid(o + Z, X, Y, sx, sy, Vector3(0, 0, 1));
	b.grid(o, Z, Y, sz, sy, Vector3(-1, 0, 0));
	b.grid(o + X, Z, Y, sz, sy, Vector3(1, 0, 0));
	b.grid(o, X, Z, sx, sz, Vector3(0, -1, 0));
	b.grid(o + Y, X, Z, sx, sz, Vector3(0, 1, 0));
}

void make_cylinder(Builder &b, float r, float h, int sides, int hs, bool caps) {
	std::vector<Vector2> prof;
	for (int i = 0; i <= hs; i++) {
		prof.push_back(Vector2(r, -h / 2 + h * i / hs));
	}
	b.lathe(prof, sides, 1);
	if (caps) {
		b.poly(b.circle(r, h / 2, sides), Vector3(0, 1, 0));
		b.poly(b.circle(r, -h / 2, sides), Vector3(0, -1, 0));
	}
}

void make_cone(Builder &b, float r, float h, int sides, bool cap) {
	const Vector3 apex(0, h / 2, 0);
	std::vector<Vector3> base = b.circle(r, -h / 2, sides);
	b.center = Vector3(0, -h / 6, 0);
	for (int s = 0; s < sides; s++) {
		b.convex({ base[s], base[(s + 1) % sides], apex }, 1);
	}
	if (cap) {
		b.poly(base, Vector3(0, -1, 0));
	}
}

void make_sphere(Builder &b, float r, int rings, int segs) {
	std::vector<Vector2> prof;
	for (int i = 0; i <= rings; i++) {
		const float a = -godot::Math::PI / 2 + godot::Math::PI * i / rings;
		prof.push_back(Vector2(std::cos(a) * r, std::sin(a) * r));
	}
	prof.front().x = 0;
	prof.back().x = 0;
	const size_t first = b.m.faces.size();
	b.lathe(prof, segs, 1);
	// UVs like a globe (meters along the equator and the meridians, the image upright seen from outside): the
	// seam's faces get their corners on one side of it, a pole takes the middle of its face's other corners
	const float PI = (float)godot::Math::PI;
	for (size_t f = first; f < b.m.faces.size(); f++) {
		Face &F = b.m.faces[f];
		const int n = F.size();
		std::vector<float> u(n), v(n);
		std::vector<char> pole(n, 0);
		for (int i = 0; i < n; i++) {
			const Vector3 p = b.m.pos[F.v[i]];
			pole[i] = Vector2(p.x, p.z).length() < r * 1e-4f;
			float a = -std::atan2(p.z, p.x);
			if (a < 0.0f) {
				a += 2.0f * PI;
			}
			u[i] = a;
			v[i] = PI * 0.5f - std::asin(std::fmax(-1.0f, std::fmin(1.0f, p.y / r)));
		}
		float lo = 1e9f, hi = -1e9f;
		for (int i = 0; i < n; i++) {
			if (!pole[i]) {
				lo = std::fmin(lo, u[i]);
				hi = std::fmax(hi, u[i]);
			}
		}
		if (hi - lo > PI) {
			for (int i = 0; i < n; i++) {
				if (!pole[i] && u[i] < PI) {
					u[i] += 2.0f * PI; // (across the seam)
				}
			}
		}
		float mid = 0.0f;
		int count = 0;
		for (int i = 0; i < n; i++) {
			if (!pole[i]) {
				mid += u[i];
				count++;
			}
		}
		mid = count ? mid / count : 0.0f;
		F.uv.resize(n);
		for (int i = 0; i < n; i++) {
			F.uv[i] = Vector2((pole[i] ? mid : u[i]) * r, v[i] * r);
		}
		F.manual_uv = true;
	}
}

void make_icosphere(Builder &b, float r, int subdiv) {
	const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
	std::vector<Vector3> v = { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t },
		{ 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } };
	std::vector<std::array<Vector3, 3>> tris;
	const int idx[20][3] = { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 },
		{ 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 },
		{ 8, 6, 7 }, { 9, 8, 1 } };
	for (auto &i : idx) {
		tris.push_back({ v[i[0]].normalized(), v[i[1]].normalized(), v[i[2]].normalized() });
	}
	for (int s = 0; s < subdiv; s++) {
		std::vector<std::array<Vector3, 3>> nt;
		for (auto &tr : tris) {
			const Vector3 a = ((tr[0] + tr[1]) * 0.5f).normalized();
			const Vector3 bb = ((tr[1] + tr[2]) * 0.5f).normalized();
			const Vector3 c = ((tr[2] + tr[0]) * 0.5f).normalized();
			nt.push_back({ tr[0], a, c });
			nt.push_back({ tr[1], bb, a });
			nt.push_back({ tr[2], c, bb });
			nt.push_back({ a, bb, c });
		}
		tris.swap(nt);
	}
	for (auto &tr : tris) {
		b.convex({ tr[0] * r, tr[1] * r, tr[2] * r }, 1);
	}
}

void make_torus(Builder &b, float R, float r, int rings, int sides) {
	auto P = [&](int i, int j) {
		const float a = TAU * i / rings, c = TAU * j / sides;
		const float rr = R + std::cos(c) * r;
		return Vector3(std::cos(a) * rr, std::sin(c) * r, std::sin(a) * rr);
	};
	for (int i = 0; i < rings; i++) {
		for (int j = 0; j < sides; j++) {
			const float a = TAU * (i + 0.5f) / rings;
			const Vector3 ring_c(std::cos(a) * R, 0, std::sin(a) * R);
			const Vector3 p0 = P(i, j), p1 = P(i + 1, j), p2 = P(i + 1, j + 1), p3 = P(i, j + 1);
			b.quad(p0, p1, p2, p3, (p0 + p1 + p2 + p3) * 0.25f - ring_c, 1);
		}
	}
}

void make_pipe(Builder &b, float R, float Rz, float th, float h, int sides, int hs) {
	th = std::min(th, std::min(R, Rz) - 0.001f);
	const float ri = std::max(0.001f, R - th), riz = std::max(0.001f, Rz - th);
	for (int s = 0; s < sides; s++) {
		const float a0 = TAU * s / sides, a1 = TAU * (s + 1) / sides;
		const Vector3 d0(std::cos(a0), 0, std::sin(a0)), d1(std::cos(a1), 0, std::sin(a1));
		const Vector3 mid = (d0 + d1).normalized();
		// (an oval: X and Z radii)
		const Vector3 o0(d0.x * R, 0, d0.z * Rz), o1(d1.x * R, 0, d1.z * Rz), i0(d0.x * ri, 0, d0.z * riz), i1(d1.x * ri, 0, d1.z * riz);
		for (int k = 0; k < hs; k++) {
			const float y0 = -h / 2 + h * k / hs, y1 = -h / 2 + h * (k + 1) / hs;
			b.role = ROLE_OUTER;
			b.quad(o0 + Vector3(0, y0, 0), o1 + Vector3(0, y0, 0), o1 + Vector3(0, y1, 0), o0 + Vector3(0, y1, 0), mid, 1);
			b.role = ROLE_INNER;
			b.quad(i0 + Vector3(0, y0, 0), i1 + Vector3(0, y0, 0), i1 + Vector3(0, y1, 0), i0 + Vector3(0, y1, 0), -mid, 1);
			b.role = -1;
		}
		for (int top = 0; top < 2; top++) {
			const Vector3 y(0, top ? h / 2 : -h / 2, 0);
			b.quad(i0 + y, i1 + y, o1 + y, o0 + y, Vector3(0, top ? 1.0f : -1.0f, 0));
		}
	}
}

void make_prism(Builder &b, float w, float h, float d) {
	const Vector3 l(-w / 2, -h / 2, 0), rr(w / 2, -h / 2, 0), t(0, h / 2, 0);
	const Vector3 z0(0, 0, -d / 2), z1(0, 0, d / 2);
	b.center = Vector3(0, -h / 6, 0);
	b.convex({ l + z0, rr + z0, t + z0 });
	b.convex({ l + z1, rr + z1, t + z1 });
	b.convex({ l + z0, rr + z0, rr + z1, l + z1 });
	b.convex({ l + z0, t + z0, t + z1, l + z1 });
	b.convex({ rr + z0, t + z0, t + z1, rr + z1 });
}

// Extrudes a closed side profile (points in (z, y)) along X from x0 to x1. edge_roles[i] is the role of
// the strip from point i to i + 1 (-1: left open); the caps at x0 / x1 are optional.
void extrude_profile(Builder &b, const std::vector<Vector2> &prof, const std::vector<int> &edge_roles, float x0, float x1, bool cap0, bool cap1, int cap_role) {
	const size_t n = prof.size();
	if (n < 3 || x1 - x0 < 1e-6f) {
		return;
	}
	float area = 0.0f;
	for (size_t i = 0; i < n; i++) {
		const Vector2 &a = prof[i], &c = prof[(i + 1) % n];
		area += a.x * c.y - c.x * a.y;
	}
	const float sgn = area > 0.0f ? 1.0f : -1.0f; // counter-clockwise in (z, y): outward = (dy, -dz)
	auto P = [](float x, const Vector2 &q) { return Vector3(x, q.y, q.x); };
	for (size_t i = 0; i < n; i++) {
		const int role = i < edge_roles.size() ? edge_roles[i] : -1;
		const Vector2 &a = prof[i], &c = prof[(i + 1) % n];
		if (role < 0 || a.distance_to(c) < 1e-6f) {
			continue;
		}
		const Vector2 e = c - a;
		const Vector3 out = Vector3(0.0f, -e.x, e.y) * sgn;
		b.role = role;
		b.quad(P(x0, a), P(x1, a), P(x1, c), P(x0, c), out);
	}
	b.role = cap_role;
	for (int side = 0; side < 2; side++) {
		if (!(side ? cap1 : cap0)) {
			continue;
		}
		std::vector<Vector3> pts;
		for (const Vector2 &q : prof) {
			pts.push_back(P(side ? x1 : x0, q));
		}
		b.poly(pts, Vector3(side ? 1.0f : -1.0f, 0, 0));
	}
	b.role = -1;
}

// Options of straight stairs (the parameters after `flip_normals`).
struct StairOpts {
	bool destination = false; // the box top is the next floor behind the last riser (no last tread)
	bool final_riser = false; // ... and that riser is built
	float riser = 0.0f; // the risers after the first one (it gets the rest of the height); 0: even
	bool tip_border = true; // the borders follow the tread tips (else the riser bottoms: the steps stand out)
	bool left = true, right = true, back = true;
	int underside = 0; // 0 filled to the ground, 1 sloped slab, 2 open
	float underside_thickness = 0.2f;
	bool border_left = false, border_right = false, border_outside = false;
	float border_width = 0.1f, border_height = 0.1f;
};

void make_stairs(Builder &b, float w, float h, float d, int steps, bool sides, const StairOpts &o) {
	// Steps climb along +Z, starting at z = 0. Profile in (z, y).
	steps = std::max(1, steps);
	const bool dest = o.destination && steps > 1;
	const int treads = dest ? steps - 1 : steps;
	const float sh = o.riser > 0.0f ? o.riser : h / steps, sd = d / treads;
	// the height of riser i's top: the first one takes what the others leave
	auto Y = [&](int i) { return i <= 0 ? 0.0f : h - (steps - i) * sh; };
	const float k = sh / sd; // slope of the flight
	// the bottom: filled to the ground, or a slab under the flight (falls back to filled when too thick)
	float z0 = 0.0f, yb = 0.0f;
	bool sloped = o.underside == 1;
	if (sloped) {
		const float tv = o.underside_thickness * std::sqrt(1.0f + k * k); // vertical thickness
		z0 = tv / k; // where the slab meets the ground
		yb = k * d - tv; // its height at the back
		const float back_top = Y(treads);
		sloped = z0 < d - 1e-4f && yb > 1e-4f && yb < back_top - 1e-4f;
	}
	if (!sloped) {
		z0 = 0.0f;
		yb = 0.0f;
	}
	const bool open = o.underside == 2;
	// the flight
	std::vector<Vector2> prof;
	std::vector<int> roles;
	for (int i = 0; i < treads; i++) {
		prof.push_back(Vector2(sd * i, Y(i)));
		roles.push_back(ROLE_RISER);
		prof.push_back(Vector2(sd * i, Y(i + 1)));
		roles.push_back(ROLE_TREAD);
	}
	prof.push_back(Vector2(d, Y(treads))); // the back top
	roles.push_back(o.back && !open ? ROLE_BACK : -1);
	prof.push_back(Vector2(d, yb));
	if (sloped) {
		roles.push_back(open ? -1 : ROLE_UNDERSIDE);
		prof.push_back(Vector2(z0, 0.0f));
	}
	roles.push_back(open ? -1 : ROLE_BOTTOM); // back along the ground to the start
	// borders (stringers) along the sides: their top follows the nosings, `border_height` above them.
	// Left / right as seen climbing (+Z, Y up): the right is at x = 0, the left at x = w.
	const float bw = std::max(0.001f, o.border_width);
	const bool border_lo = o.border_right, border_hi = o.border_left;
	float x0 = 0.0f, x1 = w;
	if (border_lo && !o.border_outside) {
		x0 = std::min(bw, w * 0.45f);
	}
	if (border_hi && !o.border_outside) {
		x1 = std::max(w - bw, w * 0.55f);
	}
	extrude_profile(b, prof, roles, x0, x1, sides && o.right && !border_lo, sides && o.left && !border_hi, ROLE_SIDE);
	if (dest && o.final_riser) {
		b.role = ROLE_RISER; // up to the next floor
		b.quad(Vector3(x0, Y(treads), d), Vector3(x1, Y(treads), d), Vector3(x1, h, d), Vector3(x0, h, d), Vector3(0, 0, -1));
		b.role = -1;
	}
	if (border_lo || border_hi) {
		const float bh = std::max(0.0f, o.border_height);
		std::vector<Vector2> bp = { Vector2(0, 0) };
		auto add = [&](float z, float y) {
			if (bp.back().distance_to(Vector2(z, y)) > 1e-5f) {
				bp.push_back(Vector2(z, y));
			}
		};
		const float top = Y(treads); // the last tread
		if (o.tip_border) {
			// through the tread tips (in line, also with a short first riser), then the destination
			add(0, Y(1) + bh);
			add(sd * (treads - 1), top + bh);
			add(d, (dest ? h : top) + bh);
		} else {
			// up from the ground under the riser bottoms (steep enough to stay under every tread)
			float slope = top / d;
			for (int i = 1; i < treads; i++) {
				slope = std::max(slope, (top - Y(i)) / (d - sd * i));
			}
			const float zs = std::max(0.0f, d - top / std::max(slope, 1e-6f));
			add(0, bh);
			add(zs, bh);
			add(d, top + bh);
			if (dest) {
				add(d, h + bh);
			}
		}
		add(d, yb);
		if (sloped) {
			bp.push_back(Vector2(z0, 0.0f));
		}
		std::vector<int> br(bp.size(), ROLE_BORDER);
		if (open) {
			br.back() = -1; // (the ground)
		}
		if (border_lo) {
			const float a = o.border_outside ? -bw : 0.0f;
			extrude_profile(b, bp, br, a, o.border_outside ? 0.0f : x0, true, true, ROLE_BORDER);
		}
		if (border_hi) {
			const float a = o.border_outside ? w : x1;
			extrude_profile(b, bp, br, a, o.border_outside ? w + bw : w, true, true, ROLE_BORDER);
		}
	}
}

// Borders of bent stairs: on the inner / outer radius, inside the width or outside it.
struct BentBorders {
	bool inner = false, outer = false, outside = false;
	float width = 0.1f, height = 0.1f;
};

void make_curved_stairs(Builder &b, float ri, float w, float h, int steps, float angle, bool sides, const BentBorders &bb = BentBorders()) {
	const float ro = ri + w;
	const float sh = h / steps;
	const float bw = std::max(0.001f, std::min(bb.width, w * 0.45f));
	// the steps between the borders standing inside the width
	const float ti = bb.inner && !bb.outside ? ri + bw : ri, to = bb.outer && !bb.outside ? ro - bw : ro;
	auto P = [&](float r, float a, float y) { return Vector3(std::cos(a) * r, y, std::sin(a) * r); };
	const float total = angle * DEG, da = total / steps;
	for (int i = 0; i < steps; i++) {
		const float a0 = total * i / steps, a1 = total * (i + 1) / steps;
		const float y = sh * (i + 1), yp = sh * i;
		const Vector3 dir0 = Vector3(-std::sin(a0), 0, std::cos(a0)); // tangent at a0 (direction of climbing)
		const Vector3 rad = Vector3(std::cos((a0 + a1) / 2), 0, std::sin((a0 + a1) / 2));
		b.role = ROLE_TREAD;
		b.quad(P(ti, a0, y), P(to, a0, y), P(to, a1, y), P(ti, a1, y), Vector3(0, 1, 0));
		b.role = ROLE_RISER;
		b.quad(P(ti, a0, yp), P(to, a0, yp), P(to, a0, y), P(ti, a0, y), -dir0);
		b.role = ROLE_BOTTOM;
		b.quad(P(ti, a0, 0), P(to, a0, 0), P(to, a1, 0), P(ti, a1, 0), Vector3(0, -1, 0));
		b.role = ROLE_OUTER;
		if (sides && !(bb.outer && !bb.outside)) {
			b.poly({ P(to, a0, 0), P(to, a1, 0), P(to, a1, y), P(to, a0, y), P(to, a0, yp) }, rad);
		}
		b.role = ROLE_INNER;
		if (sides && ti > 1e-4f && !(bb.inner && !bb.outside)) {
			b.poly({ P(ti, a0, 0), P(ti, a1, 0), P(ti, a1, y), P(ti, a0, y), P(ti, a0, yp) }, -rad);
		}
		b.role = ROLE_BACK;
		if (i == steps - 1) {
			const Vector3 dir1 = Vector3(-std::sin(a1), 0, std::cos(a1));
			b.quad(P(ti, a1, 0), P(to, a1, 0), P(to, a1, y), P(ti, a1, y), dir1);
		}
		// the borders: their top a border height above the line through the tread tips (flat on the last one)
		auto top = [&](float a) { return std::min(sh * (1.0f + a / da), h) + bb.height; };
		const float t0 = top(a0), t1 = top(a1);
		for (int side = 0; side < 2; side++) {
			if (side == 0 ? !bb.inner : !bb.outer) {
				continue;
			}
			float r0, r1;
			if (side == 0) {
				r0 = bb.outside ? std::max(0.0f, ri - bw) : ri;
				r1 = bb.outside ? ri : ri + bw;
			} else {
				r0 = bb.outside ? ro : ro - bw;
				r1 = bb.outside ? ro + bw : ro;
			}
			b.role = ROLE_BORDER;
			b.quad(P(r0, a0, t0), P(r1, a0, t0), P(r1, a1, t1), P(r0, a1, t1), Vector3(0, 1, 0));
			b.quad(P(r0, a0, 0), P(r1, a0, 0), P(r1, a1, 0), P(r0, a1, 0), Vector3(0, -1, 0));
			b.quad(P(r1, a0, 0), P(r1, a1, 0), P(r1, a1, t1), P(r1, a0, t0), rad);
			if (r0 > 1e-4f) {
				b.quad(P(r0, a0, 0), P(r0, a1, 0), P(r0, a1, t1), P(r0, a0, t0), -rad);
			}
			if (i == 0) {
				b.quad(P(r0, a0, 0), P(r1, a0, 0), P(r1, a0, t0), P(r0, a0, t0), -dir0);
			}
			if (i == steps - 1) {
				const Vector3 dir1 = Vector3(-std::sin(a1), 0, std::cos(a1));
				b.quad(P(r0, a1, 0), P(r1, a1, 0), P(r1, a1, t1), P(r0, a1, t1), dir1);
			}
		}
		b.role = -1;
	}
}

// A curve drawn in the profile window for the door's opening, the arch's inner line, the wedge's slope
// (unit square; null: the shape's own).
const std::vector<Vector2> *g_shape_profile = nullptr;

void make_arch(Builder &b, float R, float th, float d, int segs, float angle) {
	const float ri = std::max(0.001f, R - th);
	if (g_shape_profile && g_shape_profile->size() >= 2) {
		// the inner line from the profile (x: across the span, y: up to the inner radius), the outer one a
		// thickness away along its normals
		std::vector<Vector2> in;
		for (const Vector2 &q : *g_shape_profile) {
			in.push_back(Vector2((q.x * 2.0f - 1.0f) * ri, q.y * ri));
		}
		const size_t n = in.size();
		std::vector<Vector2> out(n);
		for (size_t i = 0; i < n; i++) {
			Vector2 nn;
			if (i > 0) {
				const Vector2 d0 = (in[i] - in[i - 1]).normalized();
				nn += Vector2(-d0.y, d0.x);
			}
			if (i + 1 < n) {
				const Vector2 d1 = (in[i + 1] - in[i]).normalized();
				nn += Vector2(-d1.y, d1.x);
			}
			nn = nn.length() > 1e-6f ? nn.normalized() : Vector2(0, 1);
			out[i] = in[i] + nn * th;
		}
		const float z0 = -d / 2, z1 = d / 2;
		auto P = [&](const Vector2 &q, float z) { return Vector3(q.x, q.y, z); };
		for (size_t s = 0; s + 1 < n; s++) {
			const Vector2 mid = (in[s] + in[s + 1]) * 0.5f, dir = (in[s + 1] - in[s]).normalized();
			const Vector3 rad(-dir.y, dir.x, 0);
			b.role = ROLE_OUTER;
			b.quad(P(out[s], z0), P(out[s + 1], z0), P(out[s + 1], z1), P(out[s], z1), rad, 1);
			b.role = ROLE_OPENING;
			b.quad(P(in[s], z0), P(in[s + 1], z0), P(in[s + 1], z1), P(in[s], z1), -rad, 1);
			b.role = -1;
			b.quad(P(in[s], z0), P(out[s], z0), P(out[s + 1], z0), P(in[s + 1], z0), Vector3(0, 0, -1));
			b.quad(P(in[s], z1), P(out[s], z1), P(out[s + 1], z1), P(in[s + 1], z1), Vector3(0, 0, 1));
			(void)mid;
		}
		const Vector2 t0 = (in[0] - in[1]).normalized(), t1 = (in[n - 1] - in[n - 2]).normalized();
		b.quad(P(in[0], z0), P(out[0], z0), P(out[0], z1), P(in[0], z1), Vector3(t0.x, t0.y, 0));
		b.quad(P(in[n - 1], z0), P(out[n - 1], z0), P(out[n - 1], z1), P(in[n - 1], z1), Vector3(t1.x, t1.y, 0));
		return;
	}
	const float span = angle * DEG;
	const float start = (godot::Math::PI - span) / 2; // symmetric around +Y
	auto P = [&](float r, float a, float z) { return Vector3(std::cos(a) * r, std::sin(a) * r, z); };
	const float z0 = -d / 2, z1 = d / 2;
	for (int s = 0; s < segs; s++) {
		const float a0 = start + span * s / segs, a1 = start + span * (s + 1) / segs;
		const Vector3 rad(std::cos((a0 + a1) / 2), std::sin((a0 + a1) / 2), 0);
		b.role = ROLE_OUTER;
		b.quad(P(R, a0, z0), P(R, a1, z0), P(R, a1, z1), P(R, a0, z1), rad);
		b.role = ROLE_OPENING;
		b.quad(P(ri, a0, z0), P(ri, a1, z0), P(ri, a1, z1), P(ri, a0, z1), -rad);
		b.role = -1;
		b.quad(P(ri, a0, z0), P(R, a0, z0), P(R, a1, z0), P(ri, a1, z0), Vector3(0, 0, -1));
		b.quad(P(ri, a0, z1), P(R, a0, z1), P(R, a1, z1), P(ri, a1, z1), Vector3(0, 0, 1));
	}
	if (span < TAU - 1e-4f) {
		const Vector3 t0(std::sin(start), -std::cos(start), 0);
		const float e = start + span;
		const Vector3 t1(-std::sin(e), std::cos(e), 0);
		b.quad(P(ri, start, z0), P(R, start, z0), P(R, start, z1), P(ri, start, z1), t0);
		b.quad(P(ri, e, z0), P(R, e, z0), P(R, e, z1), P(ri, e, z1), t1);
	}
}

// Wall with a door opening; `arch_segments` > 0 rounds the top of the opening (elliptic arch of
// height `arch_height`).
void make_door(Builder &b, float w, float h, float d, float ow, float oh, int arch_segments, float arch_height) {
	ow = std::min(ow, w * 0.98f);
	oh = std::min(oh, h * 0.98f);
	const float x0 = -w / 2, x1 = -ow / 2, x2 = ow / 2, x3 = w / 2;
	const float z0 = -d / 2, z1 = d / 2;
	// Opening outline from the bottom left corner, over the top, to the bottom right corner.
	std::vector<Vector2> path = { Vector2(x1, 0) };
	if (g_shape_profile && g_shape_profile->size() >= 2) {
		// drawn in the profile window: x across the opening, y up its height (its ends on the floor)
		path.clear();
		for (const Vector2 &q : *g_shape_profile) {
			path.push_back(Vector2(x1 + std::max(0.0f, std::min(1.0f, q.x)) * (x2 - x1), std::max(0.0f, std::min(1.0f, q.y)) * oh));
		}
		path.front().y = 0.0f;
		path.back().y = 0.0f;
		arch_segments = 0;
	} else if (arch_segments <= 0) {
		path.push_back(Vector2(x1, oh));
		path.push_back(Vector2(x2, oh));
	} else {
		const float rh = std::max(0.001f, std::min(arch_height, oh));
		const float cy = oh - rh;
		if (cy > 1e-5f) {
			path.push_back(Vector2(x1, cy));
		}
		for (int i = 1; i < arch_segments; i++) {
			const float a = (float)godot::Math::PI * (1.0f - (float)i / arch_segments);
			path.push_back(Vector2(std::cos(a) * ow / 2, cy + std::sin(a) * rh));
		}
		if (cy > 1e-5f) {
			path.push_back(Vector2(x2, cy));
		}
	}
	path.push_back(Vector2(x2, 0));
	const float arch_cy = arch_segments > 0 ? oh - std::max(0.001f, std::min(arch_height, oh)) : oh;
	// Front and back: the wall outline around the opening (one U-shaped n-gon each).
	std::vector<Vector2> outline = { Vector2(x0, 0) };
	outline.insert(outline.end(), path.begin(), path.end());
	outline.push_back(Vector2(x3, 0));
	outline.push_back(Vector2(x3, h));
	outline.push_back(Vector2(x0, h));
	for (int side = 0; side < 2; side++) {
		const float z = side ? z1 : z0;
		std::vector<Vector3> pts;
		for (const Vector2 &p : outline) {
			pts.push_back(Vector3(p.x, p.y, z));
		}
		b.poly(pts, Vector3(0, 0, side ? 1.0f : -1.0f));
	}
	auto wall = [&](const Vector2 &a, const Vector2 &c, const Vector3 &hint, int smooth) {
		b.poly({ Vector3(a.x, a.y, z0), Vector3(c.x, c.y, z0), Vector3(c.x, c.y, z1), Vector3(a.x, a.y, z1) }, hint, smooth);
	};
	// Inside of the opening, facing its center line.
	const bool drawn = g_shape_profile && g_shape_profile->size() >= 2;
	for (size_t i = 0; i + 1 < path.size(); i++) {
		const Vector2 m = (path[i] + path[i + 1]) * 0.5f;
		const Vector2 to = drawn ? Vector2(0.0f, oh * 0.4f) - m : Vector2(0.0f - m.x, std::min(m.y, arch_cy) - m.y);
		const Vector2 dd = path[i + 1] - path[i];
		const bool on_arch = drawn ? std::fabs(dd.x) > 1e-5f && std::fabs(dd.y) > 1e-5f : arch_segments > 0 && m.y > arch_cy + 1e-5f;
		b.role = ROLE_OPENING;
		wall(path[i], path[i + 1], Vector3(to.x, to.y, 0.0f), on_arch ? 1 : 0);
		b.role = -1;
	}
	// Outer sides, top and the two feet.
	wall(Vector2(x0, 0), Vector2(x0, h), Vector3(-1, 0, 0), 0);
	wall(Vector2(x3, 0), Vector2(x3, h), Vector3(1, 0, 0), 0);
	wall(Vector2(x0, h), Vector2(x3, h), Vector3(0, 1, 0), 0);
	wall(Vector2(x0, 0), Vector2(x1, 0), Vector3(0, -1, 0), 0);
	wall(Vector2(x2, 0), Vector2(x3, 0), Vector3(0, -1, 0), 0);
}

void make_capsule(Builder &b, float r, float h, int sides, int rings) {
	const float half = std::max(0.0f, h / 2 - r);
	std::vector<Vector2> prof;
	for (int i = 0; i <= rings; i++) {
		const float a = -godot::Math::PI / 2 + (godot::Math::PI / 2) * i / rings;
		prof.push_back(Vector2(std::cos(a) * r, std::sin(a) * r - half));
	}
	for (int i = 0; i <= rings; i++) {
		const float a = (godot::Math::PI / 2) * i / rings;
		prof.push_back(Vector2(std::cos(a) * r, std::sin(a) * r + half));
	}
	prof.front().x = 0;
	prof.back().x = 0;
	b.lathe(prof, sides, 1);
}

void make_disc(Builder &b, float r, float ri, int sides, int rings) {
	ri = std::min(ri, r * 0.999f);
	const Vector3 up(0, 1, 0);
	for (int k = 0; k < rings; k++) {
		const float r0 = ri + (r - ri) * k / rings, r1 = ri + (r - ri) * (k + 1) / rings;
		std::vector<Vector3> c0 = b.circle(r0, 0, sides), c1 = b.circle(r1, 0, sides);
		for (int s = 0; s < sides; s++) {
			const int t = (s + 1) % sides;
			if (r0 < 1e-6f) {
				b.tri(Vector3(), c1[s], c1[t], up);
			} else {
				b.quad(c0[s], c1[s], c1[t], c0[t], up);
			}
		}
	}
}

void make_wedge(Builder &b, float w, float h, float d) {
	// Ramp rising toward +Z.
	const float x0 = -w / 2, x1 = w / 2, y0 = -h / 2, y1 = h / 2, z0 = -d / 2, z1 = d / 2;
	if (g_shape_profile && g_shape_profile->size() >= 2) {
		// the slope drawn in the profile window: x along the depth, y up the height
		std::vector<Vector2> sl; // (z, y)
		for (const Vector2 &q : *g_shape_profile) {
			sl.push_back(Vector2(z0 + std::max(0.0f, std::min(1.0f, q.x)) * d, y0 + std::max(0.0f, std::min(1.0f, q.y)) * h));
		}
		sl.front() = Vector2(z0, y0);
		sl.back() = Vector2(z1, sl.back().y);
		std::vector<Vector2> outline = sl; // (and back down to the floor)
		outline.push_back(Vector2(z1, y0));
		for (int side = 0; side < 2; side++) {
			const float x = side ? x1 : x0;
			std::vector<Vector3> pts;
			for (const Vector2 &q : outline) {
				pts.push_back(Vector3(x, q.y, q.x));
			}
			b.poly(pts, Vector3(side ? 1.0f : -1.0f, 0, 0));
		}
		b.quad(Vector3(x0, y0, z0), Vector3(x1, y0, z0), Vector3(x1, y0, z1), Vector3(x0, y0, z1), Vector3(0, -1, 0));
		if (sl.back().y > y0 + 1e-5f) {
			b.quad(Vector3(x0, y0, z1), Vector3(x1, y0, z1), Vector3(x1, sl.back().y, z1), Vector3(x0, sl.back().y, z1), Vector3(0, 0, 1));
		}
		for (size_t i = 0; i + 1 < sl.size(); i++) {
			const Vector2 dd = sl[i + 1] - sl[i];
			const Vector3 up(0, dd.x, -dd.y); // (the slope's normal, up and toward the front)
			b.quad(Vector3(x0, sl[i].y, sl[i].x), Vector3(x1, sl[i].y, sl[i].x), Vector3(x1, sl[i + 1].y, sl[i + 1].x), Vector3(x0, sl[i + 1].y, sl[i + 1].x), up,
					sl.size() > 2 ? 1 : 0);
		}
		return;
	}
	b.center = Vector3(0, -h / 6, d / 6);
	b.convex({ { x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 }, { x0, y0, z1 } });
	b.convex({ { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 } });
	b.convex({ { x0, y0, z0 }, { x1, y0, z0 }, { x1, y1, z1 }, { x0, y1, z1 } });
	b.convex({ { x0, y0, z0 }, { x0, y0, z1 }, { x0, y1, z1 } });
	b.convex({ { x1, y0, z0 }, { x1, y0, z1 }, { x1, y1, z1 } });
}

void make_polygon(Builder &b, float r, int sides, float h) {
	std::vector<Vector3> top = b.circle(r, h / 2, sides);
	if (h <= 1e-5f) {
		b.poly(top, Vector3(0, 1, 0));
		return;
	}
	std::vector<Vector3> bot = b.circle(r, -h / 2, sides);
	b.poly(top, Vector3(0, 1, 0));
	b.poly(bot, Vector3(0, -1, 0));
	b.role = ROLE_SIDE;
	for (int s = 0; s < sides; s++) {
		const int t = (s + 1) % sides;
		b.convex({ bot[s], bot[t], top[t], top[s] });
	}
	b.role = -1;
}

// Inner radius of bent stairs: at a low curvature the flight keeps its depth along the arc (a large
// radius) instead of shrinking to a tiny arc around `inner_radius`.
float bent_inner_radius(float w, float d, float curvature, float inner) {
	const float a = curvature * DEG;
	return std::max(inner, a > 1e-4f ? d / a - w * 0.5f : inner);
}

// Footprint width x depth; the steps climb along the depth unless that is the wrong side.
bool stairs_swapped(const std::vector<float> &v) {
	return (v[2] >= v[0]) != (v[8] > 0.5f);
}

// Bent stairs: the flight's width, its run (along the arc) and the inner radius, the footprint turned like
// the straight ones.
void bent_dims(const std::vector<float> &v, float &r_w, float &r_run, float &r_ri) {
	const bool sw = stairs_swapped(v);
	r_w = sw ? v[2] : v[0];
	r_run = sw ? v[0] : v[2];
	r_ri = bent_inner_radius(r_w, r_run, std::fabs(v[5]), v[6]);
}

// Steps: the `steps` parameter, or as many as fit the target riser height (Height Fit: evenly divided
// risers, or the target height with a shorter first one).
int stairs_step_count(const std::vector<float> &v) {
	if (v[11] > 0.5f) {
		const float q = v[1] / std::max(0.01f, v[12]);
		return std::max(1, v[24] > 0.5f ? (int)std::ceil(q - 1e-4f) : (int)std::lround(q));
	}
	return std::max(1, (int)v[3]);
}

// The height of the risers after the first one.
float stairs_riser(const std::vector<float> &v, int steps) {
	if (v[11] > 0.5f && v[24] > 0.5f && std::fabs(v[5]) <= 0.01f) {
		return v[12]; // (the count is rounded up: the first one gets what is left, up to a full one)
	}
	return v[1] / steps;
}

// Raw stairs (before centering): straight ones climb +Z from z = 0, bent ones turn around +Y from +X.
// `r_anchor` gets the middle of the first step's front bottom edge.
void make_stairs_raw(Builder &b, const std::vector<float> &v, Vector3 &r_anchor) {
	const float curv = std::fabs(v[5]);
	const int steps = stairs_step_count(v);
	if (curv > 0.01f) {
		// around a center right of the first step (x < 0): it climbs +Z like the straight flight and bends
		// to its right, so a little curvature changes it a little
		float w, run, ri;
		bent_dims(v, w, run, ri);
		BentBorders bb;
		bb.inner = v[20] > 0.5f; // (the inner side is on the climber's right)
		bb.outer = v[19] > 0.5f;
		bb.outside = v[23] > 0.5f;
		bb.width = v[21];
		bb.height = v[22];
		const int first = (int)b.m.pos.size();
		make_curved_stairs(b, ri, w, v[1], steps, curv, (int)v[4] != 0, bb);
		for (int i = first; i < (int)b.m.pos.size(); i++) {
			b.m.pos[i].x -= ri;
		}
		r_anchor = Vector3(w * 0.5f, 0, 0);
		return;
	}
	StairOpts o;
	o.destination = v[13] > 0.5f;
	o.final_riser = v[25] > 0.5f;
	o.riser = stairs_riser(v, steps);
	o.tip_border = v[26] < 0.5f;
	o.left = v[14] > 0.5f;
	o.right = v[15] > 0.5f;
	o.back = v[16] > 0.5f;
	o.underside = (int)v[17];
	o.underside_thickness = v[18];
	o.border_left = v[19] > 0.5f;
	o.border_right = v[20] > 0.5f;
	o.border_width = v[21];
	o.border_height = v[22];
	o.border_outside = v[23] > 0.5f;
	if (stairs_swapped(v)) {
		// (the footprint turned: left / right of the climber stay)
		make_stairs(b, v[2], v[1], v[0], steps, (int)v[4] != 0, o);
		r_anchor = Vector3(v[2] * 0.5f, 0, 0);
	} else {
		make_stairs(b, v[0], v[1], v[2], steps, (int)v[4] != 0, o);
		r_anchor = Vector3(v[0] * 0.5f, 0, 0);
	}
}

// Direction of the stairs: negative curvature turns the other way (mirror), "climb_along_long_side"
// off climbs across the footprint (quarter turn), "reverse" climbs the other way (half turn).
Vector3 stairs_point(const std::vector<float> &v, Vector3 p) {
	const bool curved = std::fabs(v[5]) > 0.01f;
	if (curved && v[5] < 0.0f) {
		p.x = -p.x; // (bends to the left)
	}
	if (stairs_swapped(v)) {
		p = Vector3(p.z, p.y, -p.x);
	}
	if (v[9] > 0.5f) {
		p = Vector3(-p.x, p.y, -p.z);
	}
	return p;
}

void stairs_transform(Mesh &m, const std::vector<float> &v) {
	for (Vector3 &p : m.pos) {
		p = stairs_point(v, p);
	}
	if (std::fabs(v[5]) > 0.01f && v[5] < 0.0f) {
		for (Face &f : m.faces) {
			f.reverse(); // the mirror flips the winding
		}
	}
}

} // namespace

const char *type_name(Type t) {
	static const char *names[TYPE_MAX] = { "Custom", "Cube", "Plane", "Cylinder", "Cone", "Sphere", "Icosphere", "Torus", "Pipe", "Prism",
		"Stairs", "Curved Stairs", "Arch", "Door", "Capsule", "Disc", "Wedge", "Polygon", "Poly Shape", "Wall", "Sweep", "Column", "Path Stairs" };
	return (t >= 0 && t < TYPE_MAX) ? names[t] : "Custom";
}

const std::vector<Param> &params(Type t) {
	switch (t) {
		case CUBE:
			return P_CUBE;
		case PLANE:
			return P_PLANE;
		case CYLINDER:
			return P_CYLINDER;
		case CONE:
			return P_CONE;
		case SPHERE:
			return P_SPHERE;
		case ICOSPHERE:
			return P_ICOSPHERE;
		case TORUS:
			return P_TORUS;
		case PIPE:
			return P_PIPE;
		case PRISM:
			return P_PRISM;
		case STAIRS:
			return P_STAIRS;
		case CURVED_STAIRS:
			return P_CURVED_STAIRS;
		case ARCH:
			return P_ARCH;
		case DOOR:
			return P_DOOR;
		case CAPSULE:
			return P_CAPSULE;
		case DISC:
			return P_DISC;
		case WEDGE:
			return P_WEDGE;
		case POLYGON:
			return P_POLYGON;
		case POLY_SHAPE:
			return P_POLY_SHAPE;
		case WALL:
			return P_WALL;
		case SWEEP:
			return P_SWEEP;
		case COLUMN:
			return P_COLUMN;
		case PATH_STAIRS:
			return P_PATH_STAIRS;
		default:
			return P_NONE;
	}
}

namespace {
void build_impl(Type t, const std::vector<float> &values, Mesh &out, Vector3 *r_anchor);

std::vector<float> clamped_values(Type t, const std::vector<float> &values) {
	const std::vector<Param> &ps = params(t);
	std::vector<float> v(ps.size());
	for (size_t i = 0; i < ps.size(); i++) {
		float x = i < values.size() ? values[i] : ps[i].def;
		x = std::max(ps[i].min, std::min(ps[i].max, x));
		v[i] = ps[i].is_int ? std::round(x) : x;
	}
	return v;
}
} // namespace

namespace {
struct GroupOf {
	const char *group;
	std::vector<const char *> names;
};
const std::vector<GroupOf> &groups_of(Type t) {
	static const std::vector<GroupOf> none;
	static const std::vector<GroupOf> stairs = {
		{ "Size", { "width", "height", "depth" } },
		{ "Steps", { "fill_by", "steps", "riser_height", "height_fit", "top_of_box", "create_final_riser", "climb_along_long_side", "reverse" } },
		{ "Curve", { "curvature", "inner_radius", "turns" } },
		{ "Sides", { "build_sides", "left_side", "right_side", "back", "underside", "underside_thickness" } },
		{ "Border", { "border_left", "border_right", "border_width", "border_height", "border_outside", "border_alignment" } },
		{ "Landings", { "landing_length" } },
		{ "Cutter", { "generate_cutter", "cutter_headroom" } },
		{ "Other", { "smooth_collision", "flip_normals" } },
	};
	static const std::vector<GroupOf> column = {
		{ "Size", { "radius", "height", "sides" } },
		{ "Base", { "base_height", "base_style" } },
		{ "Shaft", { "shaft_style", "shaft_repeat", "shaft_count", "shaft_length" } },
		{ "Top", { "top_height", "top_style" } },
		{ "Other", { "smooth", "flip_normals" } },
	};
	static const std::vector<GroupOf> path_stairs = {
		{ "Size", { "width", "height" } },
		{ "Steps", { "fill_by", "steps", "step_height" } },
		{ "Underside", { "underside", "thickness" } },
		{ "Border", { "border", "landing_borders", "border_width", "border_height" } },
		{ "Cutter", { "generate_cutter", "cutter_headroom" } },
		{ "Other", { "smooth_collision", "flip_normals" } },
	};
	static const std::vector<GroupOf> sweep = {
		{ "Profile", { "profile", "width", "height", "drop", "sides", "anchor", "align" } },
		{ "Mesh", { "mesh_place", "mesh_step", "mesh_offset", "mesh_fit", "mesh_scale" } },
		{ "Path", { "closed", "caps", "corner_radius", "corner_segments" } },
		{ "Accents", { "accent", "accent_step", "accent_offset", "accent_length", "accent_width", "accent_height" } },
		{ "Other", { "smooth", "flip_normals" } },
	};
	return t == STAIRS ? stairs : t == COLUMN ? column : t == PATH_STAIRS ? path_stairs : t == SWEEP ? sweep : none;
}
} // namespace

godot::String param_group(Type t, const char *name) {
	const std::vector<GroupOf> &gs = groups_of(t);
	for (const GroupOf &g : gs) {
		for (const char *n : g.names) {
			if (std::strcmp(n, name) == 0) {
				return g.group;
			}
		}
	}
	return gs.empty() ? "" : "Other";
}

int param_group_index(Type t, const char *name) {
	const std::vector<GroupOf> &gs = groups_of(t);
	for (size_t i = 0; i < gs.size(); i++) {
		for (const char *n : gs[i].names) {
			if (std::strcmp(n, name) == 0) {
				return (int)i;
			}
		}
	}
	return gs.empty() ? 0 : (int)gs.size() - 1;
}

const char *param_fallback(Type t, const char *name) {
	auto n = [&](const char *p_name) { return std::strcmp(name, p_name) == 0; };
	if ((t == CYLINDER || t == PIPE) && n("radius_z")) {
		return "radius"; // (round before)
	}
	if (t == STAIRS) {
		if (n("fill_by")) {
			return "use_riser_height";
		}
		if (n("top_of_box") || n("create_final_riser")) {
			return "final_riser"; // (it was the destination floor with its riser)
		}
	}
	return nullptr;
}

bool param_shown(Type t, const std::vector<float> &values, const char *name) {
	if (t == COLUMN) {
		const std::vector<float> v = clamped_values(t, values);
		if (std::strcmp(name, "shaft_count") == 0) {
			return (int)v[8] == 0;
		}
		if (std::strcmp(name, "shaft_length") == 0) {
			return (int)v[8] == 1;
		}
		return true;
	}
	if (t == PATH_STAIRS) {
		const std::vector<float> v = clamped_values(t, values);
		if (std::strcmp(name, "thickness") == 0) {
			return (int)v[3] == 1;
		}
		if (std::strcmp(name, "border_width") == 0 || std::strcmp(name, "border_height") == 0) {
			return (int)v[5] != 0 || v[12] > 0.5f;
		}
		if (std::strcmp(name, "cutter_headroom") == 0) {
			return v[8] > 0.5f;
		}
		if (std::strcmp(name, "steps") == 0) {
			return (int)v[10] == 1;
		}
		if (std::strcmp(name, "step_height") == 0) {
			return (int)v[10] == 0;
		}
		return true;
	}
	if (t == SWEEP) {
		const std::vector<float> v = clamped_values(t, values);
		if (std::strcmp(name, "sides") == 0) {
			return (int)v[0] == 0 || (int)v[0] == 2;
		}
		const bool place = (int)v[0] == 7;
		if (std::strncmp(name, "mesh_", 5) == 0) {
			if (!place) {
				return false;
			}
			if (std::strcmp(name, "mesh_step") == 0 || std::strcmp(name, "mesh_offset") == 0 || std::strcmp(name, "mesh_fit") == 0) {
				return (int)v[19] == 0 || (int)v[19] == 4;
			}
			return true;
		}
		if (place) {
			// (a mesh: no profile, no accents, no caps)
			static const char *profile_only[] = { "width", "height", "drop", "sides", "anchor", "align", "caps", "smooth", "accent", "accent_step",
				"accent_offset", "accent_length", "accent_width", "accent_height" };
			for (const char *po : profile_only) {
				if (std::strcmp(name, po) == 0) {
					return false;
				}
			}
		}
		if (std::strcmp(name, "drop") == 0) {
			return (int)v[7] == 0; // (Anchor Bottom)
		}
		if (std::strcmp(name, "accent_step") == 0 || std::strcmp(name, "accent_offset") == 0) {
			return (int)v[13] == 1 || (int)v[13] == 5;
		}
		if (std::strncmp(name, "accent_", 7) == 0) {
			return (int)v[13] != 0;
		}
		return true;
	}
	if (t != STAIRS) {
		return true;
	}
	const std::vector<float> v = clamped_values(t, values);
	auto n = [&](const char *p_name) { return std::strcmp(name, p_name) == 0; };
	const bool curved = std::fabs(v[5]) > 0.01f;
	if (n("steps")) {
		return v[11] < 0.5f;
	}
	if (n("riser_height")) {
		return v[11] > 0.5f;
	}
	if (n("inner_radius")) {
		return curved;
	}
	if (n("underside_thickness")) {
		return !curved && (int)v[17] == 1;
	}
	if (n("border_width") || n("border_height") || n("border_outside")) {
		return !curved && (v[19] > 0.5f || v[20] > 0.5f);
	}
	if (n("height_fit")) {
		return v[11] > 0.5f;
	}
	if (n("create_final_riser")) {
		return !curved && v[13] > 0.5f;
	}
	if (n("border_alignment")) {
		return !curved && (v[19] > 0.5f || v[20] > 0.5f);
	}
	if (n("cutter_headroom")) {
		return v[27] > 0.5f;
	}
	if (n("top_of_box") || n("left_side") || n("right_side") || n("back") || n("underside") || n("border_left") ||
			n("border_right") || n("climb_along_long_side")) {
		return !curved;
	}
	return true;
}

void stairs_info(const std::vector<float> &values, int &r_steps, float &r_tread, float &r_riser) {
	const std::vector<float> v = clamped_values(STAIRS, values);
	r_steps = stairs_step_count(v);
	r_riser = stairs_riser(v, r_steps);
	const bool curved = std::fabs(v[5]) > 0.01f;
	const int treads = !curved && v[13] > 0.5f && r_steps > 1 ? r_steps - 1 : r_steps;
	// (the depth along the climb: the longer or the shorter side of the footprint)
	float bw_, brun_, bri_;
	bent_dims(v, bw_, brun_, bri_);
	const float run = curved ? (bri_ + bw_ * 0.5f) * std::fabs(v[5]) * (float)(godot::Math::PI / 180.0) : (stairs_swapped(v) ? v[0] : v[2]);
	r_tread = run / treads;
}

void build_with_profile(Type t, const std::vector<float> &values, const std::vector<Vector2> &profile, Mesh &out) {
	g_shape_profile = profile.size() >= 2 ? &profile : nullptr;
	build(t, values, out);
	g_shape_profile = nullptr;
}

void build(Type t, const std::vector<float> &values, Mesh &out) {
	build_impl(t, values, out, nullptr);
}

Vector3 anchor_point(Type t, const std::vector<float> &values) {
	Mesh tmp;
	Vector3 a;
	build_impl(t, values, tmp, &a);
	return a;
}

namespace {
void build_impl(Type t, const std::vector<float> &values, Mesh &out, Vector3 *r_anchor) {
	out.clear();
	Vector3 anchor;
	bool has_anchor = false;
	const std::vector<Param> &ps = params(t);
	std::vector<float> v(ps.size());
	for (size_t i = 0; i < ps.size(); i++) {
		float x = i < values.size() ? values[i] : ps[i].def;
		x = std::max(ps[i].min, std::min(ps[i].max, x));
		v[i] = ps[i].is_int ? std::round(x) : x;
	}
	auto I = [&](int i) { return (int)v[i]; };
	Builder b{ out, Vector3() };
	switch (t) {
		case CUBE:
			make_cube(b, v[0], v[1], v[2], I(3), I(4), I(5));
			break;
		case PLANE:
			b.grid(Vector3(-v[0] / 2, 0, -v[1] / 2), Vector3(v[0], 0, 0), Vector3(0, 0, v[1]), I(2), I(3), Vector3(0, 1, 0));
			break;
		case CYLINDER:
			make_cylinder(b, v[0], v[1], I(2), I(3), I(4) != 0);
			for (Vector3 &p : out.pos) {
				p.z *= v[6] / v[0]; // (an oval)
			}
			break;
		case CONE:
			make_cone(b, v[0], v[1], I(2), I(3) != 0);
			if (v[5] > 0.0f) {
				for (Vector3 &p : out.pos) {
					p.z *= v[5] / v[0]; // (an oval)
				}
			}
			break;
		case SPHERE:
			make_sphere(b, v[0], I(1), I(2));
			break;
		case ICOSPHERE:
			make_icosphere(b, v[0], I(1));
			break;
		case TORUS:
			make_torus(b, v[0], v[1], I(2), I(3));
			break;
		case PIPE:
			make_pipe(b, v[0], v[6], v[1], v[2], I(3), I(4));
			break;
		case PRISM:
			// The ridge runs along the long or the short side of the width x depth footprint.
			if ((v[2] >= v[0]) == (I(3) != 0)) {
				make_prism(b, v[0], v[1], v[2]);
			} else {
				make_prism(b, v[2], v[1], v[0]);
				for (Vector3 &p : out.pos) {
					p = Vector3(p.z, p.y, -p.x);
				}
			}
			break;
		case STAIRS:
			make_stairs_raw(b, v, anchor);
			stairs_transform(out, v);
			anchor = stairs_point(v, anchor);
			has_anchor = true;
			break;
		case CURVED_STAIRS:
			make_curved_stairs(b, v[0], v[1], v[2], I(3), v[4], I(5) != 0);
			break;
		case ARCH:
			// Like the door: the opening goes through the long or the short side of the footprint
			// (span 2 * radius along X, depth along Z); the footprint stays.
			if ((v[0] * 2.0f >= v[2]) == (I(5) != 0)) {
				make_arch(b, v[0], v[1], v[2], I(3), v[4]);
			} else {
				make_arch(b, v[2] * 0.5f, v[1], v[0] * 2.0f, I(3), v[4]);
				for (Vector3 &p : out.pos) {
					p = Vector3(p.z, p.y, -p.x);
				}
			}
			break;
		case DOOR:
			// The wall runs along the long or the short side of the width x depth footprint.
			if ((v[0] >= v[2]) == (I(7) != 0)) {
				make_door(b, v[0], v[1], v[2], v[3], v[4], I(5), v[6]);
			} else {
				make_door(b, v[2], v[1], v[0], v[3], v[4], I(5), v[6]);
				for (Vector3 &p : out.pos) {
					p = Vector3(p.z, p.y, -p.x); // a quarter turn: the wall along Z
				}
			}
			break;
		case CAPSULE:
			make_capsule(b, v[0], v[1], I(2), I(3));
			if (v[5] > 0.0f) {
				for (Vector3 &p : out.pos) {
					p.z *= v[5] / v[0]; // (an oval)
				}
			}
			break;
		case DISC:
			make_disc(b, v[0], v[1], I(2), I(3));
			break;
		case WEDGE:
			make_wedge(b, v[0], v[1], v[2]);
			break;
		case POLYGON:
			make_polygon(b, v[0], I(1), v[2]);
			break;
		case WALL:
			make_cube(b, v[0], v[1], v[2], I(3), I(4), 1);
			break;
		case SWEEP: {
			std::vector<float> nf = v;
			nf.back() = 0.0f; // (flipped below like the others)
			build_sweep({ Vector3(-2, 0, 0), Vector3(2, 0, 0) }, nf, out);
		} break;
		case COLUMN: {
			std::vector<float> nf = v;
			nf.back() = 0.0f;
			build_column(nf, {}, {}, {}, out);
		} break;
		case PATH_STAIRS: {
			std::vector<float> nf = v;
			nf.back() = 0.0f;
			build_path_stairs({ Vector3(-1.5f, 0, 0), Vector3(1.5f, 0, 0) }, nf, out);
		} break;
		default:
			break;
	}
	for (size_t i = 0; i < ps.size(); i++) {
		if (std::strcmp(ps[i].name, "flip_normals") == 0 && v[i] > 0.5f) {
			for (Face &f : out.faces) {
				f.reverse();
			}
		}
	}
	if (out.pos.empty()) {
		return;
	}
	// Center the bounding box on the origin.
	Vector3 mn = out.pos[0], mx = out.pos[0];
	for (const Vector3 &p : out.pos) {
		mn = mn.min(p);
		mx = mx.max(p);
	}
	const Vector3 c = (mn + mx) * 0.5f;
	for (Vector3 &p : out.pos) {
		p -= c;
	}
	if (r_anchor) {
		// the anchor of the shape: the first step for stairs, the bottom center otherwise
		*r_anchor = has_anchor ? anchor - c : Vector3(0, mn.y - c.y, 0);
	}
	float size = (mx - mn).length();
	weld_by_distance(out, std::max(1e-6f, size * 1e-5f));
}
} // namespace

namespace {

// Ramp under the step nosings (straight or around the curvature), with the bounds of the stairs.
void make_stairs_ramp(Builder &b, float w, float h, float d, int steps, float curvature, float ri) {
	(void)steps;
	if (curvature > 0.01f) {
		const float ro = ri + w;
		auto P = [&](float r, float a, float y) { return Vector3(std::cos(a) * r, y, std::sin(a) * r); };
		const int segs = std::max(steps, 2);
		for (int i = 0; i < segs; i++) {
			const float a0 = curvature * DEG * i / segs, a1 = curvature * DEG * (i + 1) / segs;
			// from the floor at the first riser up to the full height (never above the nosings)
			const float y0 = h * i / segs, y1 = h * (i + 1) / segs;
			const Vector3 rad = Vector3(std::cos((a0 + a1) / 2), 0, std::sin((a0 + a1) / 2));
			b.quad(P(ri, a0, y0), P(ro, a0, y0), P(ro, a1, y1), P(ri, a1, y1), Vector3(0, 1, 0));
			b.quad(P(ri, a0, 0), P(ro, a0, 0), P(ro, a1, 0), P(ri, a1, 0), Vector3(0, -1, 0));
			b.quad(P(ro, a0, 0), P(ro, a1, 0), P(ro, a1, y1), P(ro, a0, y0), rad);
			b.quad(P(ri, a0, 0), P(ri, a1, 0), P(ri, a1, y1), P(ri, a0, y0), -rad);
		}
		const float a1 = curvature * DEG;
		b.quad(P(ri, a1, 0), P(ro, a1, 0), P(ro, a1, h), P(ri, a1, h), Vector3(-std::sin(a1), 0, std::cos(a1)));
		return;
	}
	const float sd = d / steps;
	// side profile (z, y): bottom front (the slope starts on the floor at the first riser: no step to climb onto it),
	// bottom back, top back, last nosing
	const std::vector<Vector2> prof = { Vector2(0, 0), Vector2(d, 0), Vector2(d, h), Vector2(std::max(d - sd, d * 0.5f), h) };
	std::vector<Vector3> left, right;
	for (const Vector2 &p : prof) {
		left.push_back(Vector3(0, p.y, p.x));
		right.push_back(Vector3(w, p.y, p.x));
	}
	b.poly(left, Vector3(-1, 0, 0));
	b.poly(right, Vector3(1, 0, 0));
	b.center = Vector3(w / 2, h / 3, d / 2);
	for (size_t i = 0; i < prof.size(); i++) {
		const size_t j = (i + 1) % prof.size();
		b.convex({ left[i], left[j], right[j], right[i] });
	}
}

} // namespace

bool build_collision(Type t, const std::vector<float> &values, Mesh &out) {
	out.clear();
	if (t != STAIRS) {
		return false;
	}
	const std::vector<Param> &ps = params(t);
	std::vector<float> v(ps.size());
	for (size_t i = 0; i < ps.size(); i++) {
		float x = i < values.size() ? values[i] : ps[i].def;
		x = std::max(ps[i].min, std::min(ps[i].max, x));
		v[i] = ps[i].is_int ? std::round(x) : x;
	}
	if (v[7] < 0.5f) {
		return false;
	}
	// Same placement as build(): its bounding box center is moved to the origin.
	Mesh stairs;
	build(t, values, stairs);
	Mesh raw;
	Builder rb{ raw, Vector3() };
	Vector3 unused;
	make_stairs_raw(rb, v, unused);
	stairs_transform(raw, v);
	if (raw.pos.empty()) {
		return false;
	}
	Vector3 mn = raw.pos[0], mx = raw.pos[0];
	for (const Vector3 &p : raw.pos) {
		mn = mn.min(p);
		mx = mx.max(p);
	}
	const Vector3 c = (mn + mx) * 0.5f;
	Builder b{ out, Vector3() };
	float rw, rrun, rri;
	bent_dims(v, rw, rrun, rri);
	make_stairs_ramp(b, rw, v[1], rrun, stairs_step_count(v), std::fabs(v[5]), rri);
	if (std::fabs(v[5]) > 0.01f) {
		for (Vector3 &p : out.pos) {
			p.x -= rri; // (like the bent steps)
		}
	}
	stairs_transform(out, v);
	for (Vector3 &p : out.pos) {
		p -= c;
	}
	weld_by_distance(out, 1e-5f);
	return true;
}

void build_poly(const std::vector<Vector3> &p_outline, float height, bool flip, Mesh &out) {
	out.clear();
	std::vector<Vector3> ring;
	for (const Vector3 &p : p_outline) {
		const Vector3 q(p.x, 0.0f, p.z);
		if (ring.empty() || ring.back().distance_to(q) > 1e-6f) {
			ring.push_back(q);
		}
	}
	while (ring.size() > 1 && ring.front().distance_to(ring.back()) <= 1e-6f) {
		ring.pop_back();
	}
	if (ring.size() < 3) {
		return;
	}
	if (newell(ring).y < 0.0f) {
		std::reverse(ring.begin(), ring.end()); // counter-clockwise seen from +Y
	}
	Builder b{ out, Vector3() };
	const Vector3 up(0, 1, 0);
	if (std::fabs(height) <= 1e-6f) {
		b.poly(ring, up);
	} else {
		const float y0 = std::min(0.0f, height), y1 = std::max(0.0f, height);
		std::vector<Vector3> top, bot;
		for (const Vector3 &p : ring) {
			top.push_back(p + Vector3(0, y1, 0));
			bot.push_back(p + Vector3(0, y0, 0));
		}
		b.poly(top, up);
		b.poly(bot, -up);
		const size_t n = ring.size();
		b.role = ROLE_SIDE;
		for (size_t i = 0; i < n; i++) {
			const size_t j = (i + 1) % n;
			b.quad(bot[i], bot[j], top[j], top[i], (ring[j] - ring[i]).cross(up));
		}
	}
	if (flip) {
		for (Face &f : out.faces) {
			f.reverse();
		}
	}
	float size = 0.0f;
	for (const Vector3 &p : out.pos) {
		size = std::max(size, p.length());
	}
	weld_by_distance(out, std::max(1e-6f, size * 1e-5f));
}

namespace {
// Raises each corner of a wall (on the left / right offset of point i) by the point's height.
void lift_to_points(Mesh &out, const std::vector<Vector2> &p_left, const std::vector<Vector2> &p_right, const std::vector<float> &p_ys) {
	bool any = false;
	for (float y : p_ys) {
		any = any || std::fabs(y) > 1e-6f;
	}
	if (!any) {
		return;
	}
	for (Vector3 &p : out.pos) {
		const Vector2 q(p.x, p.z);
		float best = 1e30f;
		int at = 0;
		for (size_t i = 0; i < p_left.size(); i++) {
			const float d = std::min(q.distance_squared_to(p_left[i]), q.distance_squared_to(p_right[i]));
			if (d < best) {
				best = d;
				at = (int)i;
			}
		}
		p.y += p_ys[at];
	}
}
} // namespace

void build_wall(const std::vector<Vector3> &p_path, float height, float thickness, bool flip, Mesh &out, int side) {
	out.clear();
	std::vector<Vector2> pts;
	std::vector<float> ys; // (the points' heights: the wall's foot follows them)
	for (const Vector3 &p : p_path) {
		const Vector2 q(p.x, p.z);
		if (pts.empty() || pts.back().distance_to(q) > 1e-5f) {
			pts.push_back(q);
			ys.push_back(p.y);
		}
	}
	// a path back to its start: a closed ring of walls (no top over the room)
	const bool closed = pts.size() >= 4 && pts.front().distance_to(pts.back()) < 1e-3f;
	if (closed) {
		pts.pop_back();
		ys.pop_back();
	}
	if (pts.size() < 2) {
		return;
	}
	const float t = std::max(2e-4f, thickness);
	const size_t n = pts.size();
	// which side of the line the wall stands on: inside (the side the path turns around), centered, outside
	float area = 0.0f;
	for (size_t i = 0; i < n; i++) {
		area += pts[i].cross(pts[(i + 1) % n]);
	}
	const bool inside_left = area >= 0.0f;
	float off_l = t * 0.5f, off_r = -t * 0.5f; // along the left normal
	if (side == 0 || side == 2) {
		const bool left = (side == 0) == inside_left;
		off_l = left ? t : 0.0f;
		off_r = left ? 0.0f : -t;
	}
	const size_t segs = closed ? n : n - 1;
	auto normal_of = [&](size_t s) { // left normal of segment s -> s + 1
		const Vector2 d = (pts[(s + 1) % n] - pts[s]).normalized();
		return Vector2(-d.y, d.x);
	};
	// both sides offset; corners are mitered (limited, so sharp turns do not spike)
	std::vector<Vector2> left(n), right(n);
	for (size_t i = 0; i < n; i++) {
		Vector2 m;
		float k = 1.0f;
		if (!closed && i == 0) {
			m = normal_of(0);
		} else if (!closed && i == n - 1) {
			m = normal_of(n - 2);
		} else {
			const Vector2 a = normal_of((i + segs - 1) % segs), b = normal_of(i % segs);
			m = a + b;
			if (m.length() < 1e-4f) {
				m = a;
			}
			m = m.normalized();
			k = std::min(1.0f / std::max(0.25f, m.dot(a)), 4.0f);
		}
		left[i] = pts[i] + m * (off_l * k);
		right[i] = pts[i] + m * (off_r * k);
	}
	if (!closed) {
		std::vector<Vector3> outline;
		for (size_t i = 0; i < n; i++) {
			outline.push_back(Vector3(left[i].x, 0.0f, left[i].y));
		}
		for (size_t i = n; i-- > 0;) {
			outline.push_back(Vector3(right[i].x, 0.0f, right[i].y));
		}
		build_poly(outline, height, flip, out);
		lift_to_points(out, left, right, ys);
		return;
	}
	Builder b{ out, Vector3() };
	const float y0 = std::min(0.0f, height), y1 = std::max(0.0f, height);
	const Vector3 up(0, 1, 0);
	auto P = [](const Vector2 &q, float y) { return Vector3(q.x, y, q.y); };
	for (size_t i = 0; i < n; i++) {
		const size_t j = (i + 1) % n;
		const Vector2 nl = normal_of(i);
		const Vector3 hl(nl.x, 0.0f, nl.y);
		b.role = ROLE_TOP;
		b.quad(P(left[i], y1), P(left[j], y1), P(right[j], y1), P(right[i], y1), up);
		b.role = ROLE_BOTTOM;
		b.quad(P(left[i], y0), P(left[j], y0), P(right[j], y0), P(right[i], y0), -up);
		b.role = ROLE_SIDE;
		b.quad(P(left[i], y0), P(left[j], y0), P(left[j], y1), P(left[i], y1), hl);
		b.quad(P(right[i], y0), P(right[j], y0), P(right[j], y1), P(right[i], y1), -hl);
	}
	if (flip) {
		for (Face &f : out.faces) {
			f.reverse();
		}
	}
	lift_to_points(out, left, right, ys);
	float size = 0.0f;
	for (const Vector3 &p : out.pos) {
		size = std::max(size, p.length());
	}
	weld_by_distance(out, std::max(1e-6f, size * 1e-5f));
}

} // namespace shapes
} // namespace hewn
