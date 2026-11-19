// Hotspots (a trim sheet): rectangles of a material's texture, each face (or each flat piece of faces) of the
// selection textured with the one that fits its shape and size best - no unwrapping. The rectangles live on the
// material itself (its metadata "hewn_hotspots"), so every mesh with that material uses the same ones.
#pragma once

#include "hw_core.h"

#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/variant/array.hpp>

#include <functional>
#include <vector>

namespace hewn {

struct HotspotRect {
	godot::Rect2 r; // (UV, 0..1)
	int tile = 0; // 0 none, 1 repeats along U (a strip the width of the texture), 2 along V
	int orient = 0; // 0 any, 1 walls, 2 floors, 3 ceilings
	bool enabled = true; // (off: never picked; a cell chosen by hand still is)
	float scale = 1.0f; // (its pixels per meter times this: 2 = half as big in the world)
	int side = 0; // 0 any, 1 outside, 2 inside, 3 the sides of an opening
	float weight = 1.0f; // (how often among the ones that fit as well)
	int turn = 0; // 0 by its orientation, 1 never, 2 a quarter turn allowed
	bool mirror = false; // (mirrored now and then: another look)
	float min_size = 0.0f, max_size = 0.0f; // (meters, the piece's longer side; 0: no limit)
	godot::Vector2 padding; // (0..1 of the image: kept off the cell's edges)
	int atlas = 0; // (several atlases on a mesh: which one it is of)
	int local = 0; // (its number in its own atlas)
	godot::Vector2 meters; // (its size in the world, from its atlas' pixels per meter; 0: from the options' texture size)
};

struct HotspotOptions {
	bool islands = true; // flat pieces of joined faces as one, not each face on its own ("Combine Faces")
	bool turn = true; // a rectangle may lie turned a quarter (not the "upwards" ones)
	float weight = 0.5f; // 0: the shape matters, 1: the size
	float spread = 0.25f; // 0: only the cells that fit best; more: ones that fit a little worse too, by chance
	float texture_size = 2.0f; // meters the whole texture spans across (the size of a rectangle in the world)
	float aspect = 1.0f; // the texture's width / height (down it spans texture_size / aspect)
	int variant = 0; // the next of the rectangles that fit as well (another look)
	float angle = 20.0f; // degrees: faces bent more than this from each other are not one piece
	bool seams = true; // pieces are not joined over a seam
	bool world_size = true; // sizes compared in meters (texture_size); off: each against the largest of its kind
	uint32_t seed = 0; // another seed: another of the rectangles that fit as well, everywhere
	bool skip_fixed = false; // faces hotspotted by hand left as they are
	int force_cell = -1; // that rectangle for every piece (chosen by hand: the faces become fixed)
	// several atlases: each atlas' material slot (the faces get the one of the cell they get); pieces joined only
	// within one atlas held by hand (hs_lock), and a held one picks only from its own
	std::vector<int> atlas_slots;
};

std::vector<HotspotRect> hotspot_rects(const godot::Ref<godot::Material> &p_material);
godot::Array hotspot_rects_to_array(const std::vector<HotspotRect> &p_rects);

// The faces given textured with the rectangles of their material (`p_rects_of`: a material slot's, or null).
// `p_xf`: the mesh's world transform (what is up for the walls). How many faces got one.
int apply_hotspots(Mesh &m, const std::vector<int> &p_faces, const std::function<const std::vector<HotspotRect> *(int)> &p_rects_of,
		const HotspotOptions &p_options, const godot::Transform3D &p_xf);

} // namespace hewn
