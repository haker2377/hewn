// Windows along a plan: openings set out along every side of a followed curve (a floor plan), so many to a side
// by their spacing or count, kept off the corners; each a box cutter through the walls there, or a scene / a mesh
// of its own set in it (a window frame), with cutters fitted to that too if wanted. Raised by the floor's height
// for the floors above. Made again whenever the plan or a setting changes; the game gets the scenes / meshes.
#pragma once

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include <vector>

namespace godot {

class HewnWindows3D : public Node3D {
	GDCLASS(HewnWindows3D, Node3D)

public:
	enum Placing {
		PLACE_SPACING = 0, // as many as fit, this far apart
		PLACE_COUNT = 1, // so many on each side, spread out
	};
	enum Cut {
		CUT_NONE = 0,
		CUT_BOX = 1, // a box of the opening's size
		CUT_CONTENT = 2, // a box around the scene / mesh set in it
	};
	static std::vector<HewnWindows3D *> all; // (in the tree: the curves tell them when they change)

private:
	NodePath follow_curve;
	float follow_lift = 0.0f;
	float width = 1.2f, height = 1.4f, sill = 0.9f, depth = 1.0f;
	float offset = 0.0f; // across the wall (out of the plan)
	int placing = PLACE_SPACING;
	float spacing = 3.0f;
	int count = 2;
	float margin = 0.8f; // from the corners
	PackedInt32Array skip_sides;
	Ref<PackedScene> scene;
	Ref<Mesh> mesh;
	bool fit = false; // the scene / mesh scaled to the opening
	Vector3 content_offset;
	int cut = CUT_BOX;
	float cut_margin = 0.0f; // (a fitted cutter: this much bigger all round)
	bool queued = false;
	std::vector<ObjectID> made;

	void _clear();
	bool _sides(std::vector<std::pair<Vector3, Vector3>> &r_sides, std::vector<Vector3> &r_out) const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void rebuild();
	void queue_rebuild();
	void follow_changed() { queue_rebuild(); }
	int get_opening_count() const;
	// every opening's frame (X along the wall, Y up, Z out), in this node's space
	std::vector<Transform3D> openings() const;
	Node *make_game_node();

	void set_follow_curve(const NodePath &p_path);
	NodePath get_follow_curve() const { return follow_curve; }
#define HW_W_PROP(m_type, m_name) \
	void set_##m_name(m_type p_v) { \
		m_name = p_v; \
		queue_rebuild(); \
	} \
	m_type get_##m_name() const { return m_name; }
	HW_W_PROP(float, follow_lift)
	HW_W_PROP(float, width)
	HW_W_PROP(float, height)
	HW_W_PROP(float, sill)
	HW_W_PROP(float, depth)
	HW_W_PROP(float, offset)
	HW_W_PROP(int, placing)
	HW_W_PROP(float, spacing)
	HW_W_PROP(int, count)
	HW_W_PROP(float, margin)
	HW_W_PROP(bool, fit)
	HW_W_PROP(Vector3, content_offset)
	HW_W_PROP(int, cut)
	HW_W_PROP(float, cut_margin)
#undef HW_W_PROP
	void set_skip_sides(const PackedInt32Array &p_v) {
		skip_sides = p_v;
		queue_rebuild();
	}
	PackedInt32Array get_skip_sides() const { return skip_sides; }
	void set_scene(const Ref<PackedScene> &p_v) {
		scene = p_v;
		queue_rebuild();
	}
	Ref<PackedScene> get_scene() const { return scene; }
	void set_mesh(const Ref<Mesh> &p_v) {
		mesh = p_v;
		queue_rebuild();
	}
	Ref<Mesh> get_mesh() const { return mesh; }
};

} // namespace godot

VARIANT_ENUM_CAST(HewnWindows3D::Placing);
VARIANT_ENUM_CAST(HewnWindows3D::Cut);
