// Hewn — a building on its plan: the curve is the ground floor's outline; its floors (the inspector's list) stand
// one on another, each with its floor slab, its walls and windows, the roof on the top one. Each floor may have an
// outline of its own (a curve under the building); curves marked as courtyards are cut out of it all, with walls
// round them. The parts are ordinary shapes following the curves (children), set again whenever a floor, a curve or
// a setting changes: a taller ground floor lifts everything above it.
#pragma once

#include "hw_curve.h"

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

namespace godot {

class HewnMesh3D;
class HewnWindows3D;

class HewnBuildingFloor : public Resource {
	GDCLASS(HewnBuildingFloor, Resource)

	float wall_height = 3.0f;
	float floor_thickness = 0.2f;
	float wall_thickness = 0.2f;
	bool floor = true, walls = true, windows = true;
	float window_width = 1.2f, window_height = 1.4f, window_sill = 0.9f, window_spacing = 3.0f;
	PackedInt32Array no_window_sides; // (the sides from point i on that have none)
	NodePath outline; // (a curve of its own, from the building; empty: the floor below's)

protected:
	static void _bind_methods();

public:
#define HW_BF(m_type, m_name) \
	void set_##m_name(m_type p_v) { \
		m_name = p_v; \
		emit_changed(); \
	} \
	m_type get_##m_name() const { return m_name; }
	HW_BF(float, wall_height)
	HW_BF(float, floor_thickness)
	HW_BF(float, wall_thickness)
	HW_BF(bool, floor)
	HW_BF(bool, walls)
	HW_BF(bool, windows)
	HW_BF(float, window_width)
	HW_BF(float, window_height)
	HW_BF(float, window_sill)
	HW_BF(float, window_spacing)
	HW_BF(const PackedInt32Array &, no_window_sides)
	HW_BF(const NodePath &, outline)
#undef HW_BF
};

class HewnBuilding3D : public HewnCurve3D {
	GDCLASS(HewnBuilding3D, HewnCurve3D)

	TypedArray<Resource> floors; // (HewnBuildingFloor, bottom up)
	bool roof = true;
	bool cornice = false, plinth = false;
	float plinth_height = 0.6f;
	bool gable_windows = false;
	Ref<Material> floor_material, wall_material, roof_material;
	bool layout_queued = false;
	bool laying_out = false;

	void _on_floor_changed() { queue_layout(); }
	Node *_part(Node *p_under, const String &p_key) const;
	template <class T>
	T *_ensure(Node *p_under, const String &p_key, const String &p_name, bool &r_made);
	void _drop(Node *p_under, const String &p_key);
	HewnCurve3D *_floor_outline(int p_floor, HewnCurve3D *p_below) const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void queue_layout();
	void layout();
	// where a floor starts (its slab's bottom) over the plan, and the top of the last one's walls
	float floor_base(int p_floor) const;
	float top() const;
	int get_floor_count() const { return (int)floors.size(); }
	Ref<HewnBuildingFloor> get_floor(int p_i) const;
	HewnCurve3D *top_outline() const;
	HewnMesh3D *get_roof_mesh() const;

	void set_floors(const TypedArray<Resource> &p_v);
	TypedArray<Resource> get_floors() const { return floors; }
#define HW_BP(m_type, m_name) \
	void set_##m_name(m_type p_v) { \
		m_name = p_v; \
		queue_layout(); \
	} \
	m_type get_##m_name() const { return m_name; }
	HW_BP(bool, roof)
	HW_BP(bool, cornice)
	HW_BP(bool, plinth)
	HW_BP(float, plinth_height)
	HW_BP(bool, gable_windows)
#undef HW_BP
	void set_floor_material(const Ref<Material> &p_v);
	Ref<Material> get_floor_material() const { return floor_material; }
	void set_wall_material(const Ref<Material> &p_v);
	Ref<Material> get_wall_material() const { return wall_material; }
	void set_roof_material(const Ref<Material> &p_v);
	Ref<Material> get_roof_material() const { return roof_material; }
};

} // namespace godot
