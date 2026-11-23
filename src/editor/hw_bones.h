// Hewn editor — the bones of a Skeleton3D as Pose mode, Rigging and Weight Paint show and pick them
// (hw_pose.cpp). A bone's tail: along its Y axis by its length when the skeleton keeps one (meta hw_bone_lengths,
// what Rigging writes), else its child's head (the child most along Y), else half its parent's length.
#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>

#include <set>
#include <vector>

namespace hw_bones {

using namespace godot;

struct BoneShape {
	Vector3 head, tail, x; // (world)
	bool ok = false;
};

Transform3D bone_local(const Skeleton3D *s, int b); // its pose, local
Transform3D bone_global(const Skeleton3D *s, int b); // its pose in the skeleton (worked out here: up to date)
Transform3D rest_global(const Skeleton3D *s, int b); // its rest in the skeleton
void set_bone_global(Skeleton3D *s, int b, const Transform3D &p_global);
// Each bone's head, tail, X axis in the world: as posed, or at rest.
void bone_shapes(const Skeleton3D *s, std::vector<BoneShape> &r_out, bool p_rest = false);
void bone_points(const BoneShape &p_b, Vector3 r_p[6]);
String mirror_name(const String &p_name);
int mirror_bone(const Skeleton3D *s, int p_bone);
// The bone under a point of the screen (the nearest to its line, of those as near the one in front), -1: none.
int pick_bone(Camera3D *p_cam, const std::vector<BoneShape> &p_bs, const std::set<int> &p_hidden, const Vector2 &p_pos);
// The bones drawn over the view: far to near, shaded, the selected / active / hovered ones in their colors, the
// dashed lines to children away from their parent's tail, the names.
void draw_bones(Control *p_overlay, Camera3D *p_cam, const Skeleton3D *s, const std::vector<BoneShape> &p_bs, const std::vector<char> &p_selected, int p_active,
		int p_hover, const std::set<int> &p_hidden, bool p_names, float p_alpha = 1.0f);

} // namespace hw_bones
