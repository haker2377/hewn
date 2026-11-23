// Hewn — skinning weights of a mesh's vertices (bones by index): kept through every edit (new vertices take them
// from the nearest old ones), made automatically from the bones, normalized, smoothed, mirrored.
#pragma once

#include "hw_core.h"

namespace hewn {
namespace skin {

using Weights = std::vector<std::pair<int, float>>; // (bone, weight) of one vertex

// The vertices of `m` that have no weights (all of them when it has none; the ones past its list) take them from
// the nearest of the reference vertices (an exact place: its own; else the three nearest, by inverse distance).
void fill_weights(Mesh &m, const std::vector<Vector3> &ref_pos, const std::vector<Weights> &ref_w);

// One vertex's weights: summing to 1 (none: left so).
void normalize(Weights &w);
// The four strongest, summing to 1 (what the game's mesh holds).
void top4(const Weights &w, int r_bones[4], float r_weights[4]);
float weight_of(const Weights &w, int p_bone);
// The bone's weight set; with p_normalize the other bones make room (scaled so all sum to 1).
void set_weight(Weights &w, int p_bone, float p_value, bool p_normalize);
// Weights under p_min out, then at most p_limit bones (0: any), normalized.
void clean(Weights &w, float p_min, int p_limit);

// Each vertex's neighbors (by the faces' edges and the loose edges).
std::vector<std::vector<int>> neighbors(const Mesh &m);
// The vertex on the other side (x mirrored) of each, -1 when there is none within p_eps.
std::vector<int> mirror_map(const Mesh &m, float p_eps);

// Weights made from the bones (segments head -> tail, in the mesh's space): each vertex by its nearness to each
// bone (a bone outside the surface there counts much less), smoothed over the surface, the four strongest kept.
// p_kind: 0 automatic, 1 the nearest bone only (rigid), 2 empty.
void auto_weights(Mesh &m, const std::vector<Vector3> &p_heads, const std::vector<Vector3> &p_tails, const std::vector<char> &p_use, int p_kind);
// The bone's weights smoothed over the surface (only the vertices in p_only when it is not empty).
void smooth_bone(Mesh &m, int p_bone, int p_iterations, float p_factor, const std::vector<char> *p_only = nullptr);

} // namespace skin
} // namespace hewn
