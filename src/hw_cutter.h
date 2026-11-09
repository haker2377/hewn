// A non-destructive cut: a volume that removes the surface of every
// Hewn mesh it reaches, while their own geometry stays as it is. Moving, resizing or deleting the
// cutter updates them; Apply makes it permanent. Inside a prefab (a door) it cuts the walls the prefab
// is placed in, not the prefab itself; stairs make one to open the floor above them.
#pragma once

#include "hw_core.h"

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <vector>

namespace godot {

class HewnMesh3D;

class HewnCutter3D : public Node3D {
	GDCLASS(HewnCutter3D, Node3D)

public:
	enum Shape {
		SHAPE_BOX = 0,
		SHAPE_CYLINDER = 1,
		SHAPE_PRISM = 2, // `points` (local XZ), from y = 0 to size.y
	};
	// The faces lining the hole (W after a cut): made from what it cuts now, again whenever that changes.
	enum Lining {
		LINING_NONE = 0,
		LINING_AUTO = 1, // a corridor between two holes, an alcove behind one
		LINING_CORRIDOR = 2,
		LINING_ALCOVE = 3,
	};

private:
	int shape = SHAPE_BOX;
	Vector3 size = Vector3(1, 2, 1); // box / cylinder: centered on the origin
	int sides = 16;
	PackedVector2Array points;
	bool keep_flush = true; // faces lying on its sides stay (a floor under a door); off: like a boolean
	bool cut_ends = true; // faces lying on its top / bottom (local Y) are cut too (the wall a Box Cut was drawn on: an opening)
	bool skip_parent = false; // what it is placed under is not cut (a cutter in a door scene: the door's own frame)
	bool active = true;
	std::vector<std::vector<hewn::ops::CutPlane>> custom; // (set by code: the stairs' headroom; not saved)
	int lining = LINING_NONE;
	float lining_depth = 1.0f; // (an alcove)
	HewnMesh3D *lining_mesh = nullptr; // (internal child, made again from the cut meshes)
	bool lining_queued = false;
	void _update_lining();
	uint64_t revision = 1;
	std::vector<ObjectID> reached; // the meshes it cut last
	AABB last_bounds;
	MeshInstance3D *ghost = nullptr; // (editor only)
	void _changed();
	void _update_ghost();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static std::vector<HewnCutter3D *> all; // in the tree
	static bool show_shapes; // (the editor's overlay: the cutters' shapes drawn)
	static void set_show_shapes(bool p_on);

	void set_shape(int p_shape);
	int get_shape() const { return shape; }
	void set_size(const Vector3 &p_size);
	Vector3 get_size() const { return size; }
	void set_sides(int p_sides);
	int get_sides() const { return sides; }
	void set_points(const PackedVector2Array &p_points);
	PackedVector2Array get_points() const { return points; }
	void set_keep_flush(bool p_on);
	void set_skip_parent(bool p_on);
	bool get_skip_parent() const { return skip_parent; }
	bool get_keep_flush() const { return keep_flush; }
	void set_cut_ends(bool p_on);
	bool get_cut_ends() const { return cut_ends; }
	void set_active(bool p_on);
	bool is_active() const { return active; }
	void set_custom_parts(const std::vector<std::vector<hewn::ops::CutPlane>> &p_parts); // local space
	void set_lining(int p_lining);
	int get_lining() const { return lining; }
	void set_lining_depth(float p_depth);
	float get_lining_depth() const { return lining_depth; }
	void queue_lining(); // (a mesh it cuts changed)
	HewnMesh3D *get_lining_mesh() const { return lining_mesh; }
	uint64_t get_revision() const { return revision; }

	// Its convex parts in world space.
	std::vector<std::vector<hewn::ops::CutPlane>> world_parts() const;
	AABB world_bounds() const;
	// Whether it cuts this mesh: active, reaching it, and the mesh is not part of what holds the cutter
	// (the prefab it is in, the stairs that made it).
	bool cuts(const HewnMesh3D *p_mesh) const;
	// The outline (local), for drawing.
	std::vector<std::vector<Vector3>> local_outline() const;
	// Makes the cut permanent in the meshes it reaches (they keep the hole) and returns them.
	TypedArray<Node> apply();

	HewnCutter3D();
	~HewnCutter3D();
};

} // namespace godot

VARIANT_ENUM_CAST(HewnCutter3D::Shape);
VARIANT_ENUM_CAST(HewnCutter3D::Lining);
