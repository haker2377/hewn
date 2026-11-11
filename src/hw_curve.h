// Hewn curves: a Path3D with several splines (poly, Bezier, NURBS), a radius / tilt / weight per point, and the
// geometry made along them (a round bevel, another curve's shape or a profile as the cross section, extrude, offset,
// taper, a filled 2D shape). Its first spline is also the Path3D's curve (PathFollow3D and the rest of Godot use it);
// in the exported game it is a plain Path3D with a MeshInstance3D of its geometry.
#pragma once

#include "hw_core.h"
#include "hw_profile.h"

#include <godot_cpp/classes/curve3d.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/path3d.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <functional>
#include <vector>

namespace hewn {
namespace curves {

enum SplineType {
	POLY = 0,
	BEZIER = 1,
	NURBS = 2,
};
enum HandleType {
	H_AUTO = 0,
	H_VECTOR = 1,
	H_ALIGNED = 2,
	H_FREE = 3,
};

struct CPoint {
	Vector3 pos;
	Vector3 in, out; // (Bezier: the handles, offsets from the point)
	float tilt = 0.0f; // radians, about the curve
	float radius = 1.0f; // the cross section's size there
	float weight = 1.0f; // (NURBS: its pull; the others: a goal weight kept for the game)
	int hl = H_AUTO, hr = H_AUTO; // the handles' types
	bool sel = false; // (the editor's selection, not saved)
};

struct Spline {
	int type = BEZIER;
	bool cyclic = false;
	int order = 4; // (NURBS)
	bool endpoint = false; // (NURBS: the curve goes through its first and last points)
	std::vector<CPoint> pts;
	int segments() const { return pts.size() < 2 ? 0 : (cyclic ? (int)pts.size() : (int)pts.size() - 1); }
};

struct Sample {
	Vector3 p;
	float tilt = 0.0f, radius = 1.0f;
	float u = 0.0f; // along the spline: point index + fraction
};

// Points along a spline (`res` per segment; a poly spline only its points); a cyclic one does not repeat its first.
void sample(const Spline &s, int res, std::vector<Sample> &r_out);
// The Auto / Vector handles worked out (and Aligned kept in line) for every point of a Bezier spline.
void recalc_handles(Spline &s, bool p_all = false);
// The frames along the samples: tangents, normals (least twist, then turned by the tilt); p_flat: the normal the
// local Z (2D curves).
void frames(const std::vector<Sample> &smp, bool cyclic, bool p_flat, std::vector<Vector3> &r_t, std::vector<Vector3> &r_n);
float length(const std::vector<Sample> &smp, bool cyclic);
// More samples where they are farther apart than p_step.
void densify_samples(std::vector<Sample> &r_smp, bool p_cyclic, float p_step);

struct Geometry {
	int resolution = 12;
	float offset = 0.0f; // sideways from the curve
	float extrude = 0.0f; // the cross section stretched up / down by this much (alone: a flat ribbon)
	int bevel_mode = 0; // 0 round, 1 another curve's shape, 2 a profile (a quarter, mirrored round)
	float depth = 0.0f; // the round bevel's / profile's size
	int bevel_resolution = 4; // (round: 4 + 2 x this points round)
	bool fill_caps = false;
	bool is_2d = false;
	int fill_mode = 3; // (2D, closed splines: 0 none, 1 back, 2 front, 3 both)
	bool smooth = true;
	std::vector<Vector2> object_section; // (bevel_mode 1)
	bool object_closed = true;
	std::vector<Vector2> profile; // (bevel_mode 2: from (1, 0) to (0, 1))
	std::vector<float> taper; // the radius factor from start to end (empty: 1)
	// UVs: across the section and along the curve (tiles per meter, x across), or projected from the world
	bool world_uv = false;
	godot::Vector2 uv_scale = godot::Vector2(1, 1);
	bool uv_turn = false; // (the image's U along the curve, not across)
	// a road: 0 none, 1 a mesh, 2 an overlay (no geometry of its own here)
	int road = 0;
	float road_width = 3.0f;
	int road_segments = 4; // (across)
	float road_thickness = 0.0f;
	float road_curb_height = 0.0f, road_curb_width = 0.2f;
	float road_length = 3.0f; // (meters to one repeat of the image along it)
	float road_step = 0.0f; // (> 0: at least this often along it, to follow the ground)
	godot::Vector3 road_up = godot::Vector3(0, 1, 0);
	// the ground under a point at the road's level, for a point of the section (p_at) in the ring around p_center; false: none
	std::function<bool(const godot::Vector3 &p_at, const godot::Vector3 &p_center, godot::Vector3 &r_ground)> road_snap;
};

// The geometry along all the splines (local space), material 0.
void build_geometry(const std::vector<Spline> &p_splines, const Geometry &g, Mesh &out);

// conversions
godot::Dictionary spline_to_dict(const Spline &s);
Spline spline_from_dict(const godot::Dictionary &d);

} // namespace curves
} // namespace hewn

namespace godot {

class HewnMesh3D;
class HewnOverlay3D;
class MultiMeshInstance3D;

class HewnCurve3D : public Path3D {
	GDCLASS(HewnCurve3D, Path3D)

	std::vector<hewn::curves::Spline> splines;
	hewn::curves::Geometry geo;
	NodePath bevel_object, taper_object;
	Ref<HewnProfile> bevel_profile;
	Ref<Material> material;
	bool collision = false; // (the game: a static body of its geometry's triangles)
	HewnMesh3D *shown = nullptr; // (internal: the geometry)
	TypedArray<Resource> placements; // (HewnPathPlacement: things along it)
	std::vector<std::vector<MultiMeshInstance3D *>> placed; // (internal: each placement's, one for each square of the map)
	Dictionary game_metadata;
	PackedStringArray game_groups;
	Ref<Script> mesh_script, collision_script; // (on its geometry or road, and that one's collision body, in the game)
	bool merge_at_bake = true;
	void _rebuild_placements();
	// vertex paint: the meshes near it painted (shown, not stored: away from them, their colors come back)
	bool paint_enabled = false;
	int paint_channel = 0; // (0 COLOR, 1..4 CUSTOM0..3)
	Color paint_color = Color(1, 0, 0, 1);
	float paint_width = 1.0f, paint_falloff = 0.5f, paint_strength = 1.0f, paint_height = 1.0f;
	AABB paint_box; // (where it painted last: those meshes made again when it changes)
	bool paint_had = false;
	AABB _paint_reach() const;
	void _repaint_meshes();
	bool _drops() const;
	// the road, when an overlay: the projection (internal, its place the curve's origin, upright), its material from the curve's
	HewnOverlay3D *road_overlay = nullptr;
	Ref<Material> road_mat, road_src;
	bool road_ground_snap = false;
	float road_repeat = 0.0f; // (0: as the image's proportions make it)
	float road_lift = 0.02f, road_reach = 20.0f, road_step = 1.0f, road_softness = 0.3f;
	int road_order = 0;
	std::vector<ObjectID> road_watched;
	bool road_repainting = false;
	float _road_length() const;
	AABB _road_box() const;
	std::vector<HewnMesh3D *> _road_meshes() const;
	void _road_unwatch();
	void _road_watch(const std::vector<HewnMesh3D *> &p_meshes);
	void _road_ground_changed();
	void _road_source_changed();
	void _rebuild_road();
	bool _road_follows_ground() const { return geo.road == 2 || (geo.road == 1 && road_ground_snap); }
	bool queued = false;
	bool syncing = false;
	uint64_t dep_key = 0; // (the bevel / taper objects as they were: the geometry made again when they change)

	void _queue();
	void _sync_path_curve();
	void _on_curve_changed();
	uint64_t _deps_key() const;

protected:
	static void _bind_methods();
	void _notification(int p_what);
	void _validate_property(PropertyInfo &p_property) const;

public:
	static std::vector<HewnCurve3D *> all;
	std::vector<hewn::curves::Spline> &get_splines() { return splines; }
	const std::vector<hewn::curves::Spline> &get_splines_const() const { return splines; }
	// after editing the splines: the Path3D's curve and the geometry made again
	void changed();
	void rebuild();
	hewn::Mesh geometry_mesh() const; // (the geometry now, local)
	HewnMesh3D *get_shown_mesh() const { return shown; }
	// a spline's samples in local space (what the other shapes follow)
	std::vector<Vector3> sampled_points(int p_spline, int p_res = -1) const;

	void set_splines_data(const Array &p_data);
	Array get_splines_data() const;
	Dictionary get_state() const; // (undo: everything that makes it)
	void set_state(const Dictionary &p_state);

	void set_resolution(int p_v);
	int get_resolution() const { return geo.resolution; }
	void set_offset(float p_v);
	float get_offset() const { return geo.offset; }
	void set_extrude(float p_v);
	float get_extrude() const { return geo.extrude; }
	void set_bevel_mode(int p_v);
	int get_bevel_mode() const { return geo.bevel_mode; }
	void set_depth(float p_v);
	float get_depth() const { return geo.depth; }
	void set_bevel_resolution(int p_v);
	int get_bevel_resolution() const { return geo.bevel_resolution; }
	void set_fill_caps(bool p_v);
	bool get_fill_caps() const { return geo.fill_caps; }
	void set_is_2d(bool p_v);
	bool get_is_2d() const { return geo.is_2d; }
	void set_fill_mode(int p_v);
	int get_fill_mode() const { return geo.fill_mode; }
	void set_smooth(bool p_v);
	bool get_smooth() const { return geo.smooth; }
	void set_bevel_object(const NodePath &p_v);
	NodePath get_bevel_object() const { return bevel_object; }
	void set_taper_object(const NodePath &p_v);
	NodePath get_taper_object() const { return taper_object; }
	void set_bevel_profile(const Ref<HewnProfile> &p_v);
	Ref<HewnProfile> get_bevel_profile() const { return bevel_profile; }
	void set_material(const Ref<Material> &p_v);
	Ref<Material> get_material() const { return material; }
	void set_collision(bool p_v);
	void set_game_metadata(const Dictionary &p_v);
	Dictionary get_game_metadata() const { return game_metadata; }
	void set_game_groups(const PackedStringArray &p_v);
	PackedStringArray get_game_groups() const { return game_groups; }
	void set_game_script(const Ref<Script> &p_v) { set_script(p_v); }
	Ref<Script> get_game_script() const { return get_script(); }
	void set_mesh_script(const Ref<Script> &p_v) { mesh_script = p_v; }
	Ref<Script> get_mesh_script() const { return mesh_script; }
	void set_collision_script(const Ref<Script> &p_v);
	Ref<Script> get_collision_script() const { return collision_script; }
	void set_merge_at_bake(bool p_v) { merge_at_bake = p_v; }
	bool get_merge_at_bake() const { return merge_at_bake; }
	void set_world_uv(bool p_v);
	bool get_world_uv() const { return geo.world_uv; }
	void set_uv_scale(const Vector2 &p_v);
	Vector2 get_uv_scale() const { return geo.uv_scale; }
	void set_uv_turn(bool p_v);
	bool get_uv_turn() const { return geo.uv_turn; }
	void set_road_mode(int p_v);
	int get_road_mode() const { return geo.road; }
	void set_road_width(float p_v);
	float get_road_width() const { return geo.road_width; }
	void set_road_repeat(float p_v);
	float get_road_repeat() const { return road_repeat; }
	void set_road_snap(bool p_v);
	bool get_road_snap() const { return road_ground_snap; }
	void set_road_segments(int p_v);
	int get_road_segments() const { return geo.road_segments; }
	void set_road_thickness(float p_v);
	float get_road_thickness() const { return geo.road_thickness; }
	void set_road_curb_height(float p_v);
	float get_road_curb_height() const { return geo.road_curb_height; }
	void set_road_curb_width(float p_v);
	float get_road_curb_width() const { return geo.road_curb_width; }
	void set_road_lift(float p_v);
	float get_road_lift() const { return road_lift; }
	void set_road_reach(float p_v);
	float get_road_reach() const { return road_reach; }
	void set_road_step(float p_v);
	float get_road_step() const { return road_step; }
	void set_road_softness(float p_v);
	float get_road_softness() const { return road_softness; }
	void set_road_order(int p_v);
	int get_road_order() const { return road_order; }
	HewnOverlay3D *get_road_overlay() const { return road_overlay; }
	void set_placements(const TypedArray<Resource> &p_v);
	TypedArray<Resource> get_placements() const { return placements; }
	int get_placed_count(int p_placement) const; // (tests)
	// The paint of every curve onto the vertices of a mesh (p_global: its transform): COLOR / CUSTOM0..3 mixed toward
	// the curve's color near it; r_channels gets the bits of the channels painted.
	static void paint_vertices(const Transform3D &p_global, const std::vector<Vector3> &p_verts, std::vector<Color> &r_colors, std::vector<Color> &r_customs, int &r_channels);
	void set_paint_enabled(bool p_v);
	bool get_paint_enabled() const { return paint_enabled; }
	void set_paint_channel(int p_v);
	int get_paint_channel() const { return paint_channel; }
	void set_paint_color(const Color &p_v);
	Color get_paint_color() const { return paint_color; }
	void set_paint_width(float p_v);
	float get_paint_width() const { return paint_width; }
	void set_paint_falloff(float p_v);
	float get_paint_falloff() const { return paint_falloff; }
	void set_paint_strength(float p_v);
	float get_paint_strength() const { return paint_strength; }
	void set_paint_height(float p_v);
	float get_paint_height() const { return paint_height; }
	bool get_collision() const { return collision; }
	// the game's copy: a plain Path3D (its first spline), its geometry a MeshInstance3D under it
	Node *make_game_node();

	// scripting / tests
	int get_spline_count() const { return (int)splines.size(); }
	int get_point_count_of(int p_spline) const;
	Vector3 get_point_of(int p_spline, int p_point) const;
	void add_spline(int p_type, const PackedVector3Array &p_points, bool p_cyclic);
	void clear_splines();
	int get_face_count() const;
	// the presets of Add > Curve: 0 Bezier, 1 Circle, 2 NURBS curve, 3 NURBS circle
	void make_preset(int p_kind, float p_size = 1.0f);

	HewnCurve3D();
	~HewnCurve3D();
};

} // namespace godot
