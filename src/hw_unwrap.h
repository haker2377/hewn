// UV unwrapping: the faces' corners laid out flat (meters, V down as the projections), each face's corners in a
// layout (face -> its corners' UVs). Islands by seams; least squares conformal maps (LSCM), relaxed towards the
// faces' own shape (as-rigid-as-possible); planar projections grouped by normal (Smart UV Project); box,
// cylinder and sphere projections; quads laid out as a grid of rows and columns (Quadrify); the islands set out
// side by side, or packed into a square.
#pragma once

#include "hw_core.h"

#include <map>
#include <vector>

namespace hewn {
namespace unwrap {

using Layout = std::map<int, std::vector<Vector2>>;

// the faces in islands: joined over shared sides that are not seams
std::vector<std::vector<int>> seam_islands(const Mesh &m, const std::vector<int> &faces);
// one island laid out by LSCM (angles kept as well as they can be), its area the faces' own; false: it can not be
bool lscm(const Mesh &m, const std::vector<int> &faces, Layout &r_out);
// the layout's islands' triangles brought towards the faces' own shape (as-rigid-as-possible), each island in place
void relax(const Mesh &m, const std::vector<int> &faces, Layout &io, int iterations);
// planar: onto the plane across `n` (its own axes)
void planar(const Mesh &m, const std::vector<int> &faces, const Vector3 &n, Layout &r_out);
// Smart UV Project: faces grouped by normal (within `angle` degrees of a group's), each group projected flat and
// split into its connected parts
std::vector<std::vector<int>> smart_project(const Mesh &m, const std::vector<int> &faces, float angle, Layout &r_out);
// box (each face along the axis its normal is closest to), cylinder / sphere about `frame`'s Y through its origin
void project_box(const Mesh &m, const std::vector<int> &faces, const Transform3D &frame, Layout &r_out);
void project_cylinder(const Mesh &m, const std::vector<int> &faces, const Transform3D &frame, Layout &r_out);
void project_sphere(const Mesh &m, const std::vector<int> &faces, const Transform3D &frame, Layout &r_out);
// Quadrify: quads joined as a grid (each corner at its row and column; columns as wide, rows as tall as their
// sides on average); false: not a grid of quads. `along_v`: the longer way along V (else along U).
bool quad_grid(const Mesh &m, const std::vector<int> &faces, bool along_v, Layout &r_out);
// the islands set out next to each other in a row from `origin` (`gap` between them)
void row(const std::vector<std::vector<int>> &islands, Layout &io, const Vector2 &origin, float gap);
// packed into the 0..1 square: each island turned to its smallest box when `rotate`, all scaled alike (`density`:
// meters per UV unit kept instead when > 0, they may not fit then), `margin` between
void pack(const std::vector<std::vector<int>> &islands, Layout &io, bool rotate, float margin, float density);
// stretch of a face's layout: area (its UV area against its own, over `ref`: 1 = as the rest) and angle (0: kept)
void stretch(const Mesh &m, int face, const std::vector<Vector2> &uv, float ref, float &r_area, float &r_angle);

} // namespace unwrap
} // namespace hewn
