// Hewn — editable polygon mesh core (engine-agnostic part).
// Faces are n-gons stored counter-clockwise when seen from the front (right-hand rule).
// The Godot output stage flips triangles to Godot's clockwise front-face convention.
#pragma once

// ---- profiling: with HW_PROF set (tests/editor_perf.gd) each scope prints its time; otherwise one flag check
#include <chrono>
#include <cstdio>
#include <cstdlib>
struct HwProfScope {
	const char *name;
	std::chrono::steady_clock::time_point t0;
	static bool on() {
		static const bool v = std::getenv("HW_PROF") != nullptr;
		return v;
	}
	explicit HwProfScope(const char *p_name) : name(p_name), t0(std::chrono::steady_clock::now()) {}
	~HwProfScope() {
		if (on()) {
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			if (ms > 0.5) {
				std::printf("PROF %-40s %9.2f ms\n", name, ms);
				std::fflush(stdout);
			}
		}
	}
};
#define HW_PROF(n) HwProfScope hw_prof_scope_##__LINE__(n)
#define HW_PROF_BEGIN(id) const auto id##_t0 = std::chrono::steady_clock::now()
#define HW_PROF_END(id, n) \
	if (HwProfScope::on()) { \
		std::printf("PROF %-40s %9.2f ms\n", n, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - id##_t0).count()); \
		std::fflush(stdout); \
	}

#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hewn {

using godot::Basis;
using godot::Color;
using godot::Transform3D;
using godot::Vector2;
using godot::Vector3;

// What a face of a generated shape is (the top of a cube, a stair tread...). When the shape parameters
// change, the faces are rebuilt and get the material / UV settings their role had. Saved in scenes: the
// values must not change.
enum FaceRole {
	ROLE_NONE = 0,
	ROLE_SURFACE = 1, // curved surface (sphere, cylinder side)
	ROLE_TOP = 2,
	ROLE_BOTTOM = 3,
	ROLE_FRONT = 4, // +Z
	ROLE_BACK = 5, // -Z
	ROLE_LEFT = 6, // -X
	ROLE_RIGHT = 7, // +X
	ROLE_SLOPE = 8,
	ROLE_SIDE = 9, // sides of a prism / polygon / wall
	ROLE_INNER = 10,
	ROLE_OUTER = 11,
	ROLE_TREAD = 12,
	ROLE_RISER = 13,
	ROLE_UNDERSIDE = 14,
	ROLE_BORDER = 15, // stair borders (stringers)
	ROLE_OPENING = 16, // inside of a door / arch opening
	ROLE_PLACED = 17, // a mesh's copy set along a sweep (its material: the mesh's own, its slot in the face)
	ROLE_COUNT
};

enum UVFill {
	UV_FILL_TILE = 0,
	UV_FILL_FIT = 1,
	UV_FILL_STRETCH = 2,
};

struct UVParams {
	Vector2 scale = Vector2(1, 1);
	Vector2 offset = Vector2(0, 0);
	float rotation = 0.0f; // degrees
	int fill = UV_FILL_TILE;
	bool world = false;
	bool flip_u = false;
	bool flip_v = false;
	bool swap = false;
	int axis = -1; // world UVs: the world axis it is projected along (0..5: +X -X +Y -Y +Z -Z), kept until another is closer
	bool face_aligned = false; // world UVs onto the face's own plane (no stretch on a slope), not along the nearest axis
};

struct Face {
	std::vector<int> v; // ring of position indices
	std::vector<Vector2> uv; // per-corner manual UVs (used when manual_uv)
	std::vector<Color> col; // per-corner vertex colors
	std::vector<Color> cust; // per corner the CUSTOM0..3 channels (4 values each); empty: none painted
	int material = 0;
	int smooth = 0; // smoothing group, 0 = flat
	int role = ROLE_NONE; // FaceRole
	int tag = 0; // scratch for operations (not saved)
	int uv_group = 0; // faces sharing a non-zero group are projected together
	bool manual_uv = false;
	bool hidden = false; // H: not shown or picked while the mesh is edited (not copied to new faces)
	int strength = 1; // face strength (0 weak, 1 medium, 2 strong): a smooth vertex takes its normal from the strongest faces only
	UVParams uvp;

	int size() const { return (int)v.size(); }
	void ensure_attrs();
	void reverse();
	void copy_attrs_from(const Face &o); // material, smoothing, uv params (not per-corner data)
	void rotate_to(int corner); // make `corner` the first element
	void erase_corner(int i);
	void insert_corner(int at, int vid, const Vector2 &uv, const Color &c);
};

// Per-edge attributes, keyed by ekey.
struct EdgeAttr {
	bool seam = false;
	bool sharp = false;
	float crease = 0.0f; // 0..1, used by Catmull-Clark
	float bevel_weight = 0.0f; // 0..1
	bool empty() const { return !seam && !sharp && crease <= 0.0f && bevel_weight <= 0.0f; }
};

struct Mesh {
	std::vector<Vector3> pos;
	std::vector<Face> faces;
	std::unordered_map<uint64_t, EdgeAttr> edge_attrs;
	std::unordered_map<int, float> vert_crease; // 0..1, used by Catmull-Clark
	// Loose edges that belong to no face: made by F on two vertices or by
	// extruding single vertices. Not rendered in the game; extruding them makes faces.
	std::vector<std::pair<int, int>> wires;
	// Lone vertices of no face or edge
	std::vector<int> points;
	// Per material slot, the UV period of its texture (1 / the material's UV scale; 0: unknown), filled by
	// the node before it builds. Tiled world UV islands are then moved by whole periods next to the UV
	// origin (it looks the same). Not saved; empty: nothing moved.
	std::vector<Vector2> uv_period;
	int add_vertex(const Vector3 &p) {
		pos.push_back(p);
		return (int)pos.size() - 1;
	}
	void clear() {
		pos.clear();
		faces.clear();
		edge_attrs.clear();
		vert_crease.clear();
		wires.clear();
		points.clear();
	}
	const EdgeAttr *edge_attr(int a, int b) const;
	bool has_wire(int a, int b) const;
};

// Drops wires with bad / equal ends, duplicates and wires that became face edges; lone points that are bad,
// twice there or used by a face / wire.
void clean_wires(Mesh &m);

// Renumbers the vertex keys of the attributes (map: old -> new, -1 = removed).
void remap_vertex_attrs(Mesh &m, const std::vector<int> &map);

struct Edge {
	int a = 0, b = 0;
	Edge() {}
	Edge(int p_a, int p_b) :
			a(std::min(p_a, p_b)), b(std::max(p_a, p_b)) {}
	bool operator==(const Edge &o) const { return a == o.a && b == o.b; }
	bool operator<(const Edge &o) const { return a < o.a || (a == o.a && b < o.b); }
};

inline uint64_t ekey(int a, int b) {
	if (a > b) {
		std::swap(a, b);
	}
	return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b;
}
inline uint64_t ekey(const Edge &e) { return ekey(e.a, e.b); }
inline uint64_t hkey(int a, int b) { return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b; }

struct Selection {
	std::vector<int> verts;
	std::vector<Edge> edges;
	std::vector<int> faces;
	void clear() {
		verts.clear();
		edges.clear();
		faces.clear();
	}
	bool empty() const { return verts.empty() && edges.empty() && faces.empty(); }
};

struct Topology {
	int nverts = 0;
	std::unordered_map<uint64_t, std::pair<int, int>> half; // directed a->b : (face, corner)
	std::unordered_map<uint64_t, int> edge_id; // undirected
	std::vector<Edge> edges;
	std::vector<std::vector<int>> edge_faces;
	std::vector<std::vector<int>> vert_faces;
	std::vector<std::vector<int>> vert_edges;

	void build(const Mesh &m);
	int find_edge(int a, int b) const;
	int face_of_half(int a, int b) const; // face containing directed a->b, -1 if none
	bool shares_face(int e1, int e2) const;
	int other_face(int e, int f) const; // other face of edge e (manifold), -1 if none
};

// ---------- geometry helpers ----------
Vector3 newell(const std::vector<Vector3> &pts);
Vector3 face_normal(const Mesh &m, const Face &f);
Vector3 face_normal_raw(const Mesh &m, const Face &f); // length = 2 * area
Vector3 face_center(const Mesh &m, const Face &f);
void face_points(const Mesh &m, const Face &f, std::vector<Vector3> &out);
void triangulate(const std::vector<Vector3> &pts, const Vector3 &normal, std::vector<int> &out_tris);
void plane_basis(const Vector3 &n, Vector3 &t, Vector3 &b);
std::vector<Vector3> vertex_normals(const Mesh &m); // area weighted
float edge_length(const Mesh &m, const Edge &e);

// ---------- maintenance ----------
bool clean_face(Face &f); // remove consecutive duplicates and spikes; returns validity (>=3 corners)
void cleanup(Mesh &m, std::vector<int> *face_remap = nullptr); // remove invalid faces
void remove_unused_vertices(Mesh &m, Selection *sel = nullptr);
void delete_faces_mask(Mesh &m, const std::vector<char> &del, std::vector<int> *remap = nullptr);
void weld_by_distance(Mesh &m, float eps); // merges coincident positions
// Merges vertices from `first` on with any vertex closer than `dist` (no renumbering: the old positions
// stay, unused); faces that collapse go. Returns how many were merged.
int weld_new_vertices(Mesh &m, int first, float dist);
bool validate(Mesh &m); // drops faces with bad indices, returns true if mesh was ok
Face make_face(const std::vector<int> &verts, const Face *attrs_from = nullptr);

// ---------- UV / build ----------
void uv_axes(const Vector3 &n, Vector3 &u, Vector3 &v);
Vector3 paraxial_axis(const Vector3 &n, int p_keep = -1); // the world axis a world-UV face is projected along
int paraxial_index(const Vector3 &n, int p_keep = -1);
// Each world-UV face keeps its axis while the face turns, up to the point where another axis is strictly closer
// (a face sheared to exactly 45 degrees still shows the old projection).
void update_paraxial_axes(Mesh &m, const Transform3D &xf);
// Computes final per-corner UVs for every face. `xf` is the node global transform (for world-space UVs).
void compute_uvs(const Mesh &m, const Transform3D &xf, std::vector<std::vector<Vector2>> &out);
// Alt+click: face `dst` gets the texture settings of `src` (and its material); on a neighbor the
// texture runs on across the shared edge (rotation / offset). Tiled faces only; others just copy.
bool wrap_uv(Mesh &m, int src, int dst, const Transform3D &xf, bool material);
// The texture of `src` as it shows (`uvs`: compute_uvs) runs on across the shared edge onto `dst` (its own UVs).
bool continue_uv(Mesh &m, int src, int dst, const std::vector<std::vector<Vector2>> &uvs);
// New faces (a bevel's) take the texture of the faces around them, across their edges.
void continue_uvs(Mesh &m, const std::vector<int> &faces, const Transform3D &xf);
// The extent of a face in its texture projection (meters, V downwards).
void face_projection_bounds(const Mesh &m, const Face &f, const Transform3D &xf, Vector2 &r_min, Vector2 &r_max);
// The texture of a tiled face along its edge a-b: the image's bottom on the edge (`side` 1..3: the
// left / top / right instead).
void align_uv_to_edge(Mesh &m, int face, int a, int b, int side, const Transform3D &xf);
// Final UVs written back to a face: tiled parameters when they can express them (a turn, a scale per
// axis and an offset of the planar projection), else manual corner UVs.
void set_face_uvs(Mesh &m, int face, const Transform3D &xf, const std::vector<Vector2> &uv);
// Square Fit Unwrap: the connected quads of `faces` laid out as a grid by their edge lengths
// (manual UVs in meters, from `start`; seams stop it). Returns the faces laid out.
std::vector<int> unwrap_quads(Mesh &m, const std::vector<int> &faces, int start);

struct SurfaceOut {
	int material = 0;
	std::vector<Vector3> verts;
	std::vector<Vector3> normals;
	std::vector<float> tangents; // 4 per vertex
	std::vector<Vector2> uvs;
	std::vector<Vector2> uv2s; // lightmap UVs (only when given to build_mesh)
	std::vector<Color> colors;
	std::vector<Color> customs; // 4 per vertex (CUSTOM0..3), only when a face has them
	std::vector<int> indices; // Godot (clockwise) winding
	// where each vertex comes from (positions_only): the face, its corner, the smooth normal it takes (-1: flat)
	std::vector<int> src_face, src_corner, src_slot;
};
struct BuildOut {
	std::vector<SurfaceOut> surfaces;
	std::vector<std::vector<int>> face_tris; // per face, corner triplets (CCW, internal winding)
	std::vector<std::vector<Vector2>> face_uvs; // per face, per corner final UVs
	std::vector<Vector3> face_normals;
	// the smooth normals: for each, the corners (face, corner) it is made of
	std::vector<std::vector<std::pair<int, int>>> smooth_slots;
};
// `uv2`: per face, per corner lightmap UVs (lightmap_unwrap), or null.
void build_mesh(const Mesh &m, const Transform3D &xf, BuildOut &out, bool p_skip_hidden = false, const std::vector<std::vector<Vector2>> *uv2 = nullptr);
// Only the vertices moved (the faces the same as when `out` was built): their positions, normals and UVs (the
// projected ones follow the surface) in `out` again; the tangents left as they were (a whole build_mesh puts them
// right when the move ends).
void build_positions_only(const Mesh &m, const Transform3D &xf, BuildOut &out);

// ---------- lightmap UVs ----------
// Flat charts (neighbors on one plane) packed into one atlas, `texel` meters per texel (made bigger when
// the atlas would pass `max_size`). Faces with `skip` set get no room (all zero). Returns the atlas size
// in texels (ArrayMesh's lightmap size hint).
godot::Vector2i lightmap_unwrap(const Mesh &m, float texel, const std::vector<char> &skip, std::vector<std::vector<Vector2>> &r_uv2, int max_size = 4096);
// Changes whenever the positions or the faces change (stored UV2 is reused while it stays).
uint64_t lightmap_hash(const Mesh &m, float texel);

// ---------- selection ----------
namespace sel {
std::vector<int> verts_of_faces(const Mesh &m, const std::vector<int> &faces);
std::vector<int> verts_of_edges(const std::vector<Edge> &edges);
std::vector<Edge> edges_of_faces(const Mesh &m, const std::vector<int> &faces);
std::vector<Edge> edges_from_verts(const Mesh &m, const Topology &t, const std::vector<int> &verts);
std::vector<int> faces_from_verts(const Mesh &m, const std::vector<int> &verts, bool all_corners);
std::vector<int> all_verts(const Mesh &m);
std::vector<Edge> all_edges(const Mesh &m, const Topology &t);
std::vector<int> all_faces(const Mesh &m);
std::vector<int> grow_verts(const Mesh &m, const Topology &t, const std::vector<int> &s);
std::vector<int> shrink_verts(const Mesh &m, const Topology &t, const std::vector<int> &s);
std::vector<Edge> grow_edges(const Mesh &m, const Topology &t, const std::vector<Edge> &s);
std::vector<Edge> shrink_edges(const Mesh &m, const Topology &t, const std::vector<Edge> &s);
std::vector<int> grow_faces(const Mesh &m, const Topology &t, const std::vector<int> &s, float max_angle_deg = 181.0f);
std::vector<int> shrink_faces(const Mesh &m, const Topology &t, const std::vector<int> &s);
std::vector<int> linked_verts(const Mesh &m, const Topology &t, const std::vector<int> &s);
std::vector<int> linked_faces(const Mesh &m, const Topology &t, const std::vector<int> &s);
std::vector<Edge> edge_loop(const Mesh &m, const Topology &t, const Edge &start);
std::vector<Edge> edge_ring(const Mesh &m, const Topology &t, const Edge &start);
std::vector<int> face_loop(const Mesh &m, const Topology &t, int face, const Edge &dir_edge);
std::vector<int> face_loops(const Mesh &m, const Topology &t, const std::vector<int> &faces, bool ring);
std::vector<Edge> border_edges(const Mesh &m, const Topology &t);
std::vector<int> faces_by_material(const Mesh &m, const std::vector<int> &like);
std::vector<int> faces_by_normal(const Mesh &m, const std::vector<int> &like, float angle_deg);
std::vector<int> invert_faces(const Mesh &m, const std::vector<int> &s);
std::vector<int> invert_verts(const Mesh &m, const std::vector<int> &s);
std::vector<Edge> invert_edges(const Mesh &m, const Topology &t, const std::vector<Edge> &s);
std::vector<int> uniq(std::vector<int> v);
std::vector<Edge> uniq(std::vector<Edge> v);
// Shortest paths (Dijkstra by length); vertex / face chains include both ends.
std::vector<int> shortest_path_verts(const Mesh &m, const Topology &t, int from, int to);
std::vector<int> shortest_path_faces(const Mesh &m, const Topology &t, int from, int to);
std::vector<Edge> shortest_path_edges(const Mesh &m, const Topology &t, const Edge &from, const Edge &to);
// Every element on some path of the fewest steps between the two (a grid: the rectangle between them).
std::vector<int> path_region_verts(const Mesh &m, const Topology &t, int from, int to);
std::vector<int> path_region_faces(const Mesh &m, const Topology &t, int from, int to);
std::vector<Edge> path_region_edges(const Mesh &m, const Topology &t, const Edge &from, const Edge &to);
std::vector<int> linked_faces_delimit(const Mesh &m, const Topology &t, const std::vector<int> &seeds, int delimit);
} // namespace sel

// ---------- modeling operations ----------
namespace ops {
// faces
std::vector<int> extrude_faces(Mesh &m, const std::vector<int> &faces, float dist, bool individual, std::vector<int> *moved_verts = nullptr);
std::vector<int> inset_faces(Mesh &m, const std::vector<int> &faces, float amount, bool individual);
// Inset Faces with all its options; returns the inner faces (select_outer: the new ring)
struct InsetOptions {
	float thickness = 0.0f;
	float depth = 0.0f; // the inner part along its normal
	bool boundary = true; // open edges of the mesh are inset too (off: the inset slides along them)
	bool even = true; // the same thickness at sharp corners
	bool relative = false; // the thickness follows the size of the edges around
	bool edge_rail = false; // the new points slide along the edges inside the region
	bool outset = false; // the ring grows outside, the faces keep their size
	bool select_outer = false;
	bool individual = false;
	bool interpolate = true; // UVs and colors of the new corners taken inside the faces (else copied)
};
std::vector<int> inset_faces_ex(Mesh &m, const std::vector<int> &faces, const InsetOptions &o);
void delete_faces(Mesh &m, const std::vector<int> &faces);
// Delete (X): what goes with what. Vertices: their faces and edges (the other edges of those faces
// stay as wires). Edges: their faces (the other edges stay), ends left alone go. Faces: with the edges and
// vertices nothing else uses. Only Edges & Faces: those, the vertices stay. Only Faces: the faces, their edges
// and vertices stay. `edges` / `faces`: the selected ones (Only Edges & Faces uses both).
enum DeleteKind { DEL_VERTS, DEL_EDGES, DEL_FACES, DEL_ONLY_EDGES_FACES, DEL_ONLY_FACES };
void delete_elements(Mesh &m, DeleteKind kind, const std::vector<int> &verts, const std::vector<Edge> &edges, const std::vector<int> &faces);
void flip_faces(Mesh &m, const std::vector<int> &faces);
std::vector<int> subdivide_faces(Mesh &m, const std::vector<int> &faces);
std::vector<int> subdivide_faces_ex(Mesh &m, const std::vector<int> &faces, int cuts, float smoothness, bool ngons, float fractal, float along_normal, int seed);
std::vector<int> triangulate_faces(Mesh &m, const std::vector<int> &faces);
std::vector<int> merge_faces(Mesh &m, const std::vector<int> &faces);
std::vector<int> detach_faces(Mesh &m, const std::vector<int> &faces);
std::vector<int> duplicate_faces(Mesh &m, const std::vector<int> &faces);
void conform_normals(Mesh &m, const std::vector<int> &faces);
// Consistent winding, then every connected part faces outside (or inside).
void recalc_normals(Mesh &m, const std::vector<int> &faces, bool inside);
// F: a face through the selected vertices (border loop order if they lie on one).
int make_face_from(Mesh &m, const std::vector<int> &verts);
// A loose edge between two vertices; false if they are already joined.
bool add_wire(Mesh &m, int a, int b);
// Extrude of loose geometry: loose edges become quads, lone vertices get a loose edge to their copy.
// Returns the new vertices; `r_top_edges` gets the copies of the extruded loose edges.
std::vector<int> extrude_wires(Mesh &m, const std::vector<Edge> &wire_edges, const std::vector<int> &lone_verts, std::vector<Edge> *r_top_edges);
// Limited Dissolve: merges neighboring faces within the angle and removes straight-line corners.
// `faces` empty = whole mesh. Returns the number of merges.
int limited_dissolve(Mesh &m, const std::vector<int> &faces, float angle_deg, std::vector<int> *track = nullptr);
// Simplify (Planar): regions of faces meeting at less than the angle joined into one face each, in one pass.
void planar_simplify(Mesh &m, float angle_deg);
// LoopTools "Circle": every loop of the vertices (the border of the faces, if given) onto its best-fit
// circle. regular: evenly spaced, otherwise each vertex keeps its angle. influence 0..1. Returns the loops moved.
int to_circle(Mesh &m, const std::vector<int> &verts, const std::vector<int> &faces, bool regular, float influence);
Mesh extract_faces(const Mesh &m, const std::vector<int> &faces);
// edges
std::vector<Edge> extrude_edges(Mesh &m, const std::vector<Edge> &edges, float dist, bool along_normal);
// `segments` > 1 rounds the bevel with a circular profile.
// p_profile: custom bevel profile, from (1, 0) at one side to (0, 1) at the other, (1, 1) the old corner.
std::vector<int> bevel_edges(Mesh &m, const std::vector<Edge> &edges, float amount, int segments = 1, bool keep_middle = false,
		const std::vector<Vector2> *p_profile = nullptr, bool p_percent = false);
// Harden Normals after a bevel: the bevel faces and the faces around them in one smoothing group,
// the faces around strong (their normals stay flat, the bevel faces bend between them).
void harden_normals(Mesh &m, const std::vector<int> &bevel_faces);
// Cuts `cuts` loops across the quad ring of `start`. One cut is placed at `t` along start (from start.a);
// several are spread evenly and slid together by (t - 0.5).
std::vector<Edge> loop_cut(Mesh &m, const Edge &start, int cuts, float t = 0.5f);
// Ring edges of a loop cut, each oriented so that its first vertex is on the same side as start.a.
std::vector<std::pair<int, int>> loop_cut_ring(const Mesh &m, const Edge &start);
std::vector<Edge> connect_edges(Mesh &m, const std::vector<Edge> &edges);
std::vector<Edge> insert_edge_loops(Mesh &m, const std::vector<Edge> &edges);
// A face with two sides cut (not next to each other): the new points joined across it (straight cuts). p_ngons off:
// the faces with more corners than four after it made of triangles / quads.
std::vector<Edge> subdivide_edges(Mesh &m, const std::vector<Edge> &edges, int cuts, bool p_ngons = true);
std::vector<int> bridge_edges(Mesh &m, const Edge &a, const Edge &b);
std::vector<int> dissolve_edges(Mesh &m, const std::vector<Edge> &edges);
int flip_edges(Mesh &m, const std::vector<Edge> &edges);
// vertices
std::vector<int> weld_vertices(Mesh &m, const std::vector<int> &verts, float dist);
std::vector<int> collapse_groups(Mesh &m, const std::vector<std::vector<int>> &groups);
std::vector<int> split_vertices(Mesh &m, const std::vector<int> &verts);
std::vector<Edge> connect_vertices(Mesh &m, const std::vector<int> &verts);
void dissolve_vertices(Mesh &m, const std::vector<int> &verts);
std::vector<int> fill_holes(Mesh &m, const std::vector<int> &verts); // empty => all holes
std::vector<int> offset_vertices(Mesh &m, const std::vector<int> &verts, const Vector3 &delta);
// object
// Subdivision Surface: Catmull-Clark (or Simple: split only, nothing moves), `levels` times.
struct SubdivOptions {
	int levels = 1;
	bool simple = false;
	bool creases = true; // the edges' / vertices' creases keep them sharp
	bool keep_corners = false; // Boundary Smooth: Keep Corners (a corner of an open border stays where it is)
	int uv_smooth = 1; // 0 none (UVs only split), 1 keep boundaries (UVs smoothed inside their islands)
};
void subdivide_smooth(Mesh &m, const SubdivOptions &o);
void subdivide_smooth(Mesh &m);
void auto_smooth(Mesh &m, float angle_deg, const std::vector<int> &faces);
void transform(Mesh &m, const Transform3D &xf);
void append(Mesh &dst, const Mesh &src, const Transform3D &xf, int material_offset);
int quadify(Mesh &m, float angle_deg, const std::vector<int> *only = nullptr); // merges coplanar triangle pairs into quads
std::vector<int> poke_faces(Mesh &m, const std::vector<int> &faces);
std::vector<int> bridge_faces(Mesh &m, const std::vector<int> &faces);
std::vector<int> split_edges(Mesh &m, const std::vector<Edge> &edges);
std::vector<int> bevel_vertices(Mesh &m, const std::vector<int> &verts, float amount);
void delete_edge_loops(Mesh &m, const std::vector<Edge> &edges);
// Splits face `f` along a path from corner vertex `path.front()` to corner vertex `path.back()`;
// inner path entries are vertices not on the face. Returns the index of the new face, -1 on failure.
int split_face_path(Mesh &m, int f, const std::vector<int> &path);
// Inserts vertex `v` into every face edge a-b (either direction) at parameter t from a.
void insert_on_edge(Mesh &m, int a, int b, const std::vector<std::pair<float, int>> &points);

// Knife: a polyline on the surface. Each point lies on face `face`; consecutive points are joined by
// walking across the surface along the plane with normal `cut_normal` (per segment, i -> i+1).
struct KnifePoint {
	int face = -1;
	Vector3 pos;
};
struct KnifeHit {
	int vert = -1; // existing vertex
	int ea = -1, eb = -1; // or edge a-b
	float t = 0.0f; // parameter from ea
	int face = -1; // or face interior
	Vector3 pos;
};
// Computes the surface path (no mesh changes). Returns false when a segment can't be walked.
bool knife_path(const Mesh &m, const std::vector<KnifePoint> &pts, const std::vector<Vector3> &cut_normals, std::vector<KnifeHit> &out);
// The knife along a path found already (the visible edges under the line drawn on the screen,
// in order; each two hits in a row on one face). The hits are found again by their positions first, so paths
// found before other cuts still fit. Returns the new edges.
std::vector<Edge> knife_along(Mesh &m, const std::vector<KnifeHit> &path);
// A point on face `face`: its vertex, its edge or inside it.
KnifeHit knife_classify(const Mesh &m, int face, const Vector3 &p);
// Two hits on one face.
bool knife_share_face(const Mesh &m, const KnifeHit &a, const KnifeHit &b);
// F2 add-on (F). "Open" edges: with less than two faces (wires too). The view picks among them as
// F2 does: the ones nearest to the mouse; a face with no neighbors looks at `eye` (all in the mesh's space).
struct F2View {
	std::function<Vector2(const Vector3 &)> to_screen; // (empty: the first ones)
	Vector2 mouse;
	Vector3 eye;
	bool has_eye = false;
};
// One edge -> a quad with an open edge at each end (those nearest to the mouse; a triangle when they meet or
// there is one); returns the new face, r_next: its edge across (F again goes on). One vertex -> of its open
// edges, the two whose new corner is nearest to the mouse: a new vertex making a quad with them (a triangle
// when they are joined); r_new: that vertex. -1: nothing.
int f2_fill_edge(Mesh &m, int a, int b, const F2View &view, Edge *r_next);
int f2_fill_vertex(Mesh &m, int v, const F2View &view, int *r_new);
// The face a point lies in (on its plane, inside or on its border): `hint` first; -1: none.
int face_at_point(const Mesh &m, const Vector3 &p, int hint);
// Applies the knife; returns the new edges.
std::vector<Edge> knife(Mesh &m, const std::vector<KnifePoint> &pts, const std::vector<Vector3> &cut_normals);
// The knife's cuts all at once (paths found on the mesh as it is: each two hits in a row on one face): every face
// is split by all the cuts lying in it together (cuts that cross get a vertex where they do), its pieces the cells
// they make; a cut ending inside a face goes on to its nearest corner, a loop inside a face is joined to two corners.
// No piece lies over another. Returns the new edges.
std::vector<Edge> knife_net(Mesh &m, const std::vector<std::vector<KnifeHit>> &paths);

// Extrude helpers.
void dissolve_orthogonal(Mesh &m, int first_new_face, std::vector<int> &caps);
std::vector<int> extrude_along_normals(Mesh &m, const std::vector<int> &faces, float dist);

// Bisect: cuts the faces (empty = all) with a plane; can remove one side and fill the cut.
// "Inner" is the side the normal points away from.
std::vector<Edge> bisect(Mesh &m, const std::vector<int> &faces, const Vector3 &point, const Vector3 &normal, bool clear_inner, bool clear_outer, bool fill);

// A closed convex block of flat faces, no two in one plane (its faces can be moved as planes).
bool is_convex_block(const Mesh &m);
// The face's plane moved along its normal, the block made again from the planes (hw_planepush.cpp): the faces keep
// their numbers and planes, their corners are where the planes now meet. False (out untouched) when a face would
// vanish.
// p_collapse: past where the moved face would vanish it goes into the corner (or edge) and is left out.
bool move_face_plane(const Mesh &src, int face, float delta, Mesh &out, bool p_collapse = false);
// A convex block whose faces may share planes or not be flat (those split into triangles): r_out has one
// face for each plane (the faces of one plane: the first one stands for all), ready for move_face_plane; r_face is
// p_face's (that one by its own plane, even when not quite flat). False when it is no convex block.
bool convex_plane_mesh(const Mesh &m, int p_face, Mesh &r_out, int &r_face);
// Dynamic topology (hw_dyntopo.cpp): in the sphere, edges longer than p_max_len split, ones shorter than 0.4 of it
// collapsed (not on a border, no face turning over); the faces there become triangles. At most p_budget changes.
// True when the faces changed.
bool dyntopo_refine(Mesh &m, const Vector3 &p_center, float p_radius, float p_max_len, bool p_subdivide, bool p_collapse, int p_budget = 4000);
// Simplify (Collapse): as triangles, the shortest edges collapsed until about p_ratio of the triangles are left.
void decimate_collapse(Mesh &m, float p_ratio);
// Remesh (hw_remesh.cpp): the shape made again on a voxel grid.
enum RemeshMode {
	REMESH_BLOCKS,
	REMESH_SMOOTH,
	REMESH_SHARP,
	REMESH_VOXEL,
};
struct RemeshOptions {
	int mode = REMESH_VOXEL;
	float voxel_size = 0.1f; // (Voxel: the cell)
	float adaptivity = 0.0f; // (Voxel: flat parts with fewer faces)
	int octree_depth = 4; // (the others: the cell is the biggest size / scale / 2^depth)
	float scale = 0.9f;
	float sharpness = 1.0f; // (Sharp)
	bool remove_disconnected = true;
	float threshold = 1.0f; // (pieces smaller than this times the biggest go)
	bool smooth_shading = false;
	Vector3 grid_offset; // (in cells: the grid moved a little)
	bool symmetric_x = false; // (the grid the same on both sides of x = 0)
	float cell = 0.0f; // (> 0: this cell size, whatever the mode)
};
bool remesh(Mesh &m, const RemeshOptions &o);
void relax_tangential(Mesh &m, int p_iterations);
// Trims (hw_sweep.cpp): the selected edges in chains, each one turned so the faces along it are on its right (a sweep
// with the profile Outside sticks out of them); closed: a loop. The cut of the mesh by the plane y = p_y as loops /
// lines turned the same way.
struct TrimPath {
	std::vector<Vector3> points;
	bool closed = false;
};
std::vector<TrimPath> edge_trim_paths(const Mesh &m, const std::vector<Edge> &edges);
std::vector<TrimPath> section_trim_paths(const Mesh &m, float p_y);
// The faces no one can see: inside the mesh's own solid (merged objects that overlap or touch); a room's inside
// (faces turned in) stays.
std::vector<int> inner_faces(const Mesh &m);
// Convex pieces of the space the mesh takes (at most p_max_pieces): each piece's points (a collision's hull).
std::vector<std::vector<Vector3>> convex_pieces(const Mesh &m, int p_max_pieces);
void transfer_attributes(Mesh &dst, const Mesh &src, bool p_colors, bool p_smoothing);

// Level design. A cut volume is the intersection of half spaces n.p <= d.
struct CutPlane {
	Vector3 n; // unit, out of the volume
	float d = 0.0f;
	// A side of the volume (not its depth): it moves onto geometry closer than a millimeter (the floor
	// under a door) and the faces lying on it stay, so the cut is flush with them (no hair-thin strips).
	bool flush = true;
};
// Removes the surface inside the volume (a union of convex parts) from the faces (empty = all): faces
// crossing it are split, their neighbors get the new vertices (no T-junctions). Returns the new holes
// as loops in cap order (like a face that would close them).
std::vector<std::vector<int>> cut_volumes(Mesh &m, const std::vector<int> &faces, const std::vector<std::vector<CutPlane>> &parts);
// builders without "Keep Overlap Faces": where faces of `add` lie on faces of `m` turned the other
// way, both are cut away (two touching boxes grow together, two rooms open into each other).
void merge_touching(Mesh &m, Mesh &add, float eps);
// A corridor: connects two holes (cap order loops) with a strip of faces, winding like the faces around.
std::vector<int> bridge_loops(Mesh &m, const std::vector<int> &a, const std::vector<int> &b);
// An alcove: the hole pushed `depth` into the wall (against the faces around it) and closed. With the planes of
// the volume that cut the hole: along the side of it facing most into the wall, as far as that side (a cutter
// turned against the wall, or over its corner, gives an alcove along itself, its own depth).
std::vector<int> alcove(Mesh &m, const std::vector<int> &loop, float depth, const std::vector<CutPlane> *planes = nullptr);
// The holes (border loops) in cap order.
std::vector<std::vector<int>> holes(const Mesh &m);
// Closes a hole (cap order loop) with one face like its neighbors.
std::vector<int> fill_loop(Mesh &m, const std::vector<int> &loop);

// Arches: every edge bends into an arc (`segments` pieces, `rise` 1 = a half circle) across itself, into
// the face next to it that faces up most. Two opposite edges of a quad (the ceiling of a cut passage) bend
// together and the quad becomes a vault of strips. Returns the new edges.
std::vector<Edge> arch_edges(Mesh &m, const std::vector<Edge> &edges, int segments, float rise);

// Modifiers (applied to a copy for display, or baked).
void mirror(Mesh &m, int axis, float merge); // mirror across the local plane axis = 0
void solidify(Mesh &m, float thickness, float offset); // offset -1: shell grows inwards, 1: outwards

// Mesh merging after a boolean: welds, removes T-junctions and merges coplanar pieces of the same source face.
void repair_tjunctions(Mesh &m, float eps);
} // namespace ops

// ---------- boolean (BSP CSG) ----------
namespace csg {
enum Op {
	UNION = 0,
	SUBTRACT = 1,
	INTERSECT = 2,
};
// Both meshes must be closed. Result faces keep the attributes of the faces they came from.
// r_ok is false (and `a` is returned) for flat operands or when the work budget runs out.
Mesh boolean(const Mesh &a, const Mesh &b, Op op, bool *r_ok = nullptr);
} // namespace csg

// ---------- shapes ----------
namespace shapes {
enum Type {
	CUSTOM = 0,
	CUBE,
	PLANE,
	CYLINDER,
	CONE,
	SPHERE,
	ICOSPHERE,
	TORUS,
	PIPE,
	PRISM,
	STAIRS,
	CURVED_STAIRS,
	ARCH,
	DOOR,
	CAPSULE,
	DISC,
	WEDGE,
	POLYGON,
	POLY_SHAPE, // outline points come from the node (not from params)
	WALL, // drawn as a line between two points
	SWEEP, // a profile along a path (the node's points), drawn like a wall
	COLUMN, // a lathed column: base, repeated shaft and top profiles (their heights do not stretch)
	PATH_STAIRS, // stairs along the node's points: flights and landings (drawn like a wall)
	TYPE_MAX
};

struct Param {
	const char *name;
	float def;
	float min;
	float max;
	bool is_int;
	const char *options = nullptr; // a choice: "First,Second,..." (the value is the index)
	bool is_toggle() const { return is_int && !options && min == 0.0f && max == 1.0f; } // on / off (a check box)
};

const char *type_name(Type t);
const std::vector<Param> &params(Type t);
// Whether a parameter does anything with these values (e.g. the riser height only when the steps
// follow it): the panels hide the others.
bool param_shown(Type t, const std::vector<float> &values, const char *name);
// Shape panels fold long parameter lists into groups: the group of a parameter ("" when the shape has none)
// and its place (groups 0 and 1 start open).
godot::String param_group(Type t, const char *name);
int param_group_index(Type t, const char *name);
// The parameter an older one was saved as (read when the new one is missing), or nullptr.
const char *param_fallback(Type t, const char *name);
// Straight or bent stairs as built: number of steps, depth of a tread and height of a riser.
void stairs_info(const std::vector<float> &values, int &r_steps, float &r_tread, float &r_riser);
// `values` follow params(t); missing entries take the defaults. Output is centered on the origin.
void build(Type t, const std::vector<float> &values, Mesh &out);
// The door's opening, the arch's inner line or the wedge's slope from a drawn curve (unit square).
void build_with_profile(Type t, const std::vector<float> &values, const std::vector<Vector2> &profile, Mesh &out);
// A point of the built shape that should stay put when parameters change (the first step of stairs;
// the bottom center of other shapes).
Vector3 anchor_point(Type t, const std::vector<float> &values);
// Simplified collision for a shape (stairs with `smooth_collision`: a ramp instead of the steps), in the
// same space as build(). Returns false when the shape uses its render mesh.
bool build_collision(Type t, const std::vector<float> &values, Mesh &out);
// Outline in the XZ plane (y ignored), extruded along +Y by `height` (negative goes down, 0 = flat face).
void build_poly(const std::vector<Vector3> &outline, float height, bool flip, Mesh &out);
// Wall along a path in the XZ plane (y ignored), `thickness` wide, from y = 0 up to `height`.
// `side`: 0 the wall stands inside the path (the side it turns around), 1 centered, 2 outside. A path back
// to its start makes a closed ring.
void build_wall(const std::vector<Vector3> &path, float height, float thickness, bool flip, Mesh &out, int side = 1);
// A profile (params(SWEEP)) along a path in local space (any direction; up is +Y), mitered at the corners.
// `custom`: the cross section of the Custom profile (x across, y up, meters).
// (profile Mesh: copies of p_place set along the path; accents of the shape Mesh: copies of p_accent)
// p_gaps: the path's segments left out (segment i: from point i to the next), the sweep in pieces between them.
void build_sweep(const std::vector<Vector3> &path, const std::vector<float> &values, Mesh &out, const std::vector<Vector2> *custom = nullptr, const Mesh *p_place = nullptr,
		const Mesh *p_accent = nullptr, const std::vector<int> *p_gaps = nullptr, const std::vector<Vector2> *p_accent_custom = nullptr); // (the accents' Custom shape)
// Stairs along a path (local XZ; it climbs from y = 0): params(PATH_STAIRS).
void build_path_stairs(const std::vector<Vector3> &path, const std::vector<float> &values, Mesh &out, bool p_ramps = false); // ramps: the smooth collision
// The profiles (x = radius factor, y = 0..1 up the part); empty = the style's preset.
void build_column(const std::vector<float> &values, const std::vector<Vector2> &base, const std::vector<Vector2> &shaft, const std::vector<Vector2> &top, Mesh &out);
} // namespace shapes

} // namespace hewn
