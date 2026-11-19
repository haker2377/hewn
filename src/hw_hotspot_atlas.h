// A hotspot atlas (a trim sheet): an image cut into rectangular cells by straight cuts. Faces are textured with the
// cell that fits them best (their shape, their size, which way they face), so a level is textured without
// unwrapping. A resource of its own (.tres): one atlas for many meshes and scenes; a mesh keeps which it uses.
#pragma once

#include "hw_hotspot.h"

#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

class HewnHotspotAtlas : public Resource {
	GDCLASS(HewnHotspotAtlas, Resource)

public:
	enum Orientation {
		ORIENT_ANY = 0, // any face, at any rotation
		ORIENT_UPWARDS = 1, // near-vertical faces only, kept upright (walls, text)
		ORIENT_FLOOR = 2, // faces looking up, at any rotation
		ORIENT_CEILING = 3, // faces looking down, at any rotation
	};
	enum Tiling {
		TILE_NONE = 0,
		TILE_U = 1, // a cell as wide as the image repeats along its width
		TILE_V = 2, // a cell as tall as the image repeats along its height
	};

private:
	Ref<Texture2D> texture;
	Ref<Material> material; // the material faces get with it (made from the texture when not given)
	TypedArray<Rect2> cells; // in the image's 0..1
	PackedInt32Array orientations;
	PackedInt32Array tilings;
	PackedInt32Array enabled; // (0: the cell left out of hotspotting)
	PackedFloat32Array scales; // (each cell's pixels per meter times this)
	float pixels_per_meter = 0.0f; // 0: the editor's texture density

	void _fit_arrays();

protected:
	static void _bind_methods();

public:
	HewnHotspotAtlas();

	void set_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_texture() const { return texture; }
	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const { return material; }
	// the material to give faces: the one set, or a plain one with the texture (kept on the atlas)
	Ref<Material> get_or_make_material();
	void set_cells(const TypedArray<Rect2> &p_cells);
	TypedArray<Rect2> get_cells() const { return cells; }
	void set_orientations(const PackedInt32Array &p_values);
	PackedInt32Array get_orientations() const { return orientations; }
	void set_tilings(const PackedInt32Array &p_values);
	PackedInt32Array get_tilings() const { return tilings; }
	void set_enabled_cells(const PackedInt32Array &p_values);
	PackedInt32Array get_enabled_cells() const { return enabled; }
	void set_scales(const PackedFloat32Array &p_values);
	PackedFloat32Array get_scales() const { return scales; }
	bool is_cell_enabled(int p_cell) const;
	void set_cell_enabled(int p_cell, bool p_on);
	float get_cell_scale(int p_cell) const;
	void set_cell_scale(int p_cell, float p_scale);
	void set_pixels_per_meter(float p_value);
	float get_pixels_per_meter() const { return pixels_per_meter; }

	int get_cell_count() const { return (int)cells.size(); }
	Rect2 get_cell(int p_cell) const;
	int get_cell_orientation(int p_cell) const;
	void set_cell_orientation(int p_cell, int p_orientation);
	int get_cell_tiling(int p_cell) const;
	// a tiling the cell may have: TILE_U only when it is as wide as the image, TILE_V as tall (else it stays none)
	void set_cell_tiling(int p_cell, int p_tiling);
	int get_cell_tilings_possible(int p_cell) const; // (1: along U, 2: along V, 3: both)
	int get_cell_at(const Vector2 &p_uv) const; // -1: none
	Vector2 get_image_size() const; // pixels (the texture's, 1x1 without one)

	// The cuts: a line across the whole image (every cell it goes through is split), or across one cell; a cell
	// split keeps its orientation, its tiling when still possible. `p_vertical`: the line goes down the image.
	// How many cells were split.
	int cut(bool p_vertical, float p_at, int p_only_cell = -1);
	// two cells sharing a whole side made one again (the first one's settings); false when they do not
	bool merge_cells(int p_a, int p_b);
	// the cell across a side of one (0 left, 1 right, 2 top, 3 bottom) sharing that whole side, or -1
	int get_neighbor(int p_cell, int p_side) const;
	void reset(); // one cell: the whole image

	// Free cells: any rectangle in the image (not only what cuts make; the image need not be covered)
	int add_cell(const Rect2 &p_rect, int p_orientation = ORIENT_ANY, int p_tiling = TILE_NONE);
	void set_cell(int p_cell, const Rect2 &p_rect); // (kept in the image, at least a hair of it; a tiling it can no longer have: off)
	void remove_cell(int p_cell);
	// A cut: the line between cells along x (p_vertical) or y = p_at. Its extent through `p_through` (the other
	// coordinate): the run of cell sides on that line touching each other (x: from, y: to; empty: none there).
	Vector2 get_cut_extent(bool p_vertical, float p_at, float p_through) const;
	// the cut taken away: the cells on its two sides with the same extent joined; how many pairs
	int remove_cut(bool p_vertical, float p_at, float p_from, float p_to);
	// the cut moved to p_to_at: the sides of the cells on it (within from..to) follow; false when a cell would vanish
	bool move_cut(bool p_vertical, float p_at, float p_to_at, float p_from, float p_to);

	// the cells for hotspotting (UV rectangles in the image's 0..1, their tiling and orientation)
	std::vector<hewn::HotspotRect> get_rects() const;
};

} // namespace godot

VARIANT_ENUM_CAST(HewnHotspotAtlas::Orientation);
VARIANT_ENUM_CAST(HewnHotspotAtlas::Tiling);
