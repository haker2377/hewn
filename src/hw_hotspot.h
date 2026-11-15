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
};

struct HotspotOptions {
	bool islands = true; // flat pieces of joined faces as one, not each face on its own
	bool turn = true; // a rectangle may lie turned a quarter
	float weight = 0.5f; // 0: the shape matters, 1: the size
	float texture_size = 2.0f; // meters the whole texture spans (the size of a rectangle in the world)
	int variant = 0; // the next of the rectangles that fit as well (another look)
	float angle = 20.0f; // degrees: faces bent more than this from each other are not one piece
};

std::vector<HotspotRect> hotspot_rects(const godot::Ref<godot::Material> &p_material);
godot::Array hotspot_rects_to_array(const std::vector<HotspotRect> &p_rects);

// The faces given textured with the rectangles of their material (`p_rects_of`: a material slot's, or null).
// `p_xf`: the mesh's world transform (what is up for the walls). How many faces got one.
int apply_hotspots(Mesh &m, const std::vector<int> &p_faces, const std::function<const std::vector<HotspotRect> *(int)> &p_rects_of,
		const HotspotOptions &p_options, const godot::Transform3D &p_xf);

} // namespace hewn
