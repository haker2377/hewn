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
#include <godot_cpp/variant/dictionary.hpp>

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

class HewnCurve3D : public Path3D {
	GDCLASS(HewnCurve3D, Path3D)

	std::vector<hewn::curves::Spline> splines;
	hewn::curves::Geometry geo;
	NodePath bevel_object, taper_object;
	Ref<HewnProfile> bevel_profile;
	Ref<Material> material;
	bool collision = false; // (the game: a static body of its geometry's triangles)
	HewnMesh3D *shown = nullptr; // (internal: the geometry)
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
	void set_collision(bool p_v) { collision = p_v; }
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
