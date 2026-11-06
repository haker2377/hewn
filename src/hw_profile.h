// A 2D profile (saved as a resource): a column's base, shaft or top (x = radius as a factor of the
// column's radius, y = 0..1 up its part), or a Sweep's custom cross section (x across, y up, meters).
#pragma once

#include <godot_cpp/classes/curve.hpp>
#include <godot_cpp/classes/curve2d.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <vector>

namespace godot {

class HewnProfile : public Resource {
	GDCLASS(HewnProfile, Resource)

	PackedVector2Array points;
	// curve profile: each point's handle type and its handles (offsets from the point; used by
	// Free and Aligned). No types: straight lines between the points (Vector).
	PackedInt32Array handle_types;
	PackedVector2Array handles_in, handles_out;
	int resolution = 8; // samples of a curved piece
	bool sample_straight = false; // straight pieces sampled too (else only their ends)
	bool sample_equal = false; // the samples at equal lengths along the whole curve
	bool symmetric = false; // mirrored around x = 0.5: an edit on one half is copied onto the other (a door, an arch)
	bool _left_index(int p_index) const { return p_index * 2 < (int)points.size() - 1; } // (of a symmetric curve)

protected:
	static void _bind_methods();

public:
	enum HandleType {
		HANDLE_AUTO = 0,
		HANDLE_VECTOR = 1,
		HANDLE_FREE = 2,
		HANDLE_ALIGNED = 3,
	};
	void set_points(const PackedVector2Array &p_points);
	PackedVector2Array get_points() const { return points; }
	void set_handle_types(const PackedInt32Array &p_types);
	PackedInt32Array get_handle_types() const { return handle_types; }
	void set_handles_in(const PackedVector2Array &p_h);
	PackedVector2Array get_handles_in() const { return handles_in; }
	void set_handles_out(const PackedVector2Array &p_h);
	PackedVector2Array get_handles_out() const { return handles_out; }
	void set_resolution(int p_res);
	int get_resolution() const { return resolution; }
	void set_sample_straight(bool p_on);
	bool get_sample_straight() const { return sample_straight; }
	void set_sample_equal(bool p_on);
	bool get_sample_equal() const { return sample_equal; }
	void set_symmetric(bool p_on);
	bool get_symmetric() const { return symmetric; }
	// One half copied mirrored onto the other (from the left half, or the right one).
	void symmetrize(bool p_from_left);
	// The curve as a polyline (what the shapes use).
	std::vector<Vector2> to_vector() const;
	PackedVector2Array sample() const;
	// The handles in effect (Auto / Vector worked out), offsets from point i.
	int get_handle_type(int p_index) const;
	Vector2 handle_in(int p_index) const;
	Vector2 handle_out(int p_index) const;
	// Editing (keeps the arrays the same length)
	int insert_point(int p_at, const Vector2 &p_pos);
	void remove_point(int p_index);
	void set_point(int p_index, const Vector2 &p_pos);
	void set_point_handle_type(int p_index, int p_type);
	void set_point_handle(int p_index, bool p_out, const Vector2 &p_offset);
	Dictionary get_state() const;
	void set_state(const Dictionary &p_state);
	// Godot's curves: Curve (y over x, tangents), Curve2D (Bezier)
	void from_curve(const Ref<Curve> &p_curve);
	void from_curve2d(const Ref<Curve2D> &p_curve);
	Ref<Curve> to_curve() const;
	Ref<Curve2D> to_curve2d() const;

	// Built-in shapes for the column parts (the Column's base / shaft / top styles).
	static PackedVector2Array preset(const String &p_part, int p_style);
	static String preset_names(const String &p_part);
	static Ref<HewnProfile> make_preset(const String &p_part, int p_style);
};

} // namespace godot
