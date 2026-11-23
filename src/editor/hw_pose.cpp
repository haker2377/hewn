// Pose mode: the bones of a Skeleton3D selected in the view (click, Shift+click, a box), moved, turned and scaled
// with G / R / S (the same axes, typed values, pivots and snapping as objects and vertices), with Auto IK (the
// parents turn to bring the bone to the mouse) and X mirror (the bone of the other side follows), cleared back to
// the rest pose, copied and pasted (also flipped), hidden, and keyed into the animation open in the Animation panel
// (by hand with I, or after each change with Auto Keying).
#include "hw_editor.h"
#include "hw_editor_util.h"
#include "hw_bones.h"

#include <godot_cpp/classes/editor_inspector.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;
using namespace hw_editor;

namespace hw_bones {

Vector3 cam_forward(Camera3D *p_cam) {
	return -p_cam->get_global_transform().basis.get_column(2).normalized();
}

void toast(const String &p_text) {
	if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
		t->push_toast(p_text);
	}
}

// The bones' transforms worked out from their poses here (Skeleton3D's global poses are only made again at the
// end of the frame: an IK step must see the turn just made).
Transform3D bone_local(const Skeleton3D *s, int b) {
	return Transform3D(Basis(s->get_bone_pose_rotation(b)).scaled_local(s->get_bone_pose_scale(b)), s->get_bone_pose_position(b));
}

Transform3D bone_global(const Skeleton3D *s, int b) {
	Transform3D t = bone_local(s, b);
	int guard = 0;
	for (int p = s->get_bone_parent(b); p >= 0 && guard++ < 1000; p = s->get_bone_parent(p)) {
		t = bone_local(s, p) * t;
	}
	return t;
}

void set_bone_global(Skeleton3D *s, int b, const Transform3D &p_global) {
	const int p = s->get_bone_parent(b);
	const Transform3D l = p >= 0 ? bone_global(s, p).affine_inverse() * p_global : p_global;
	s->set_bone_pose_position(b, l.origin);
	s->set_bone_pose_rotation(b, l.basis.get_rotation_quaternion());
	s->set_bone_pose_scale(b, l.basis.get_scale());
}

Transform3D rest_global(const Skeleton3D *s, int b) {
	Transform3D t = s->get_bone_rest(b);
	int guard = 0;
	for (int p = s->get_bone_parent(b); p >= 0 && guard++ < 1000; p = s->get_bone_parent(p)) {
		t = s->get_bone_rest(p) * t;
	}
	return t;
}

// The child a bone points to (the one most along its Y axis) and its length at rest, by bone.
void bone_tails(const Skeleton3D *s, std::vector<int> &r_child, std::vector<float> &r_len) {
	const int n = s->get_bone_count();
	r_child.assign(n, -1);
	r_len.assign(n, 0.0f);
	float sum = 0.0f;
	int cnt = 0;
	for (int b = 0; b < n; b++) {
		const PackedInt32Array ch = s->get_bone_children(b);
		float best = -2.0f;
		for (int i = 0; i < ch.size(); i++) {
			const Vector3 o = s->get_bone_rest(ch[i]).origin;
			if (o.length() < 1e-6f) {
				continue;
			}
			const float d = o.normalized().y; // (most along Y: the one it points to)
			if (d > best) {
				best = d;
				r_child[b] = ch[i];
				r_len[b] = o.length();
			}
		}
		if (r_child[b] >= 0) {
			sum += r_len[b];
			cnt++;
		}
	}
	const float avg = cnt > 0 ? sum / (float)cnt : 0.1f;
	for (int b = 0; b < n; b++) {
		if (r_child[b] < 0) {
			const int p = s->get_bone_parent(b);
			r_len[b] = p >= 0 && r_len[p] > 0.0f ? r_len[p] * 0.5f : avg * 0.5f; // (an end bone: half its parent)
		}
	}	// (Rigging's lengths: the tail along Y, wherever the children are)
	if (s->has_meta("hw_bone_lengths")) {
		const Dictionary lens = s->get_meta("hw_bone_lengths");
		for (int b = 0; b < n; b++) {
			const String nm = s->get_bone_name(b);
			if (lens.has(nm) && (float)lens[nm] > 0.0f) {
				r_child[b] = -1;
				r_len[b] = lens[nm];
			}
		}
	}
}

void bone_shapes(const Skeleton3D *s, std::vector<BoneShape> &r_out, bool p_rest) {
	std::vector<int> child;
	std::vector<float> len;
	bone_tails(s, child, len);
	const Transform3D g = s->get_global_transform();
	const int n = s->get_bone_count();
	r_out.assign(n, BoneShape());
	std::vector<Transform3D> gp(n);
	for (int b = 0; b < n; b++) {
		gp[b] = p_rest ? rest_global(s, b) : bone_global(s, b);
	}
	for (int b = 0; b < n; b++) {
		BoneShape &bs = r_out[b];
		bs.head = g.xform(gp[b].origin);
		bs.tail = child[b] >= 0 ? g.xform(gp[child[b]].origin) : g.xform(gp[b].xform(Vector3(0, len[b], 0)));
		bs.x = (g.basis * gp[b].basis).get_column(0);
		bs.ok = bs.head.distance_to(bs.tail) > 1e-7f;
	}
}

// The octahedron of a bone: its head, four corners at a tenth of its length, its tail.
void bone_points(const BoneShape &p_b, Vector3 r_p[6]) {
	const Vector3 d = p_b.tail - p_b.head;
	const float l = d.length();
	const Vector3 dn = d / l;
	Vector3 x = p_b.x - dn * p_b.x.dot(dn);
	if (x.length() < 1e-6f) {
		x = std::fabs(dn.y) < 0.9f ? dn.cross(Vector3(0, 1, 0)) : dn.cross(Vector3(1, 0, 0));
	}
	x = x.normalized() * l * 0.1f;
	const Vector3 z = dn.cross(x).normalized() * l * 0.1f;
	const Vector3 c = p_b.head + d * 0.1f;
	r_p[0] = p_b.head;
	r_p[1] = c + x;
	r_p[2] = c + z;
	r_p[3] = c - x;
	r_p[4] = c - z;
	r_p[5] = p_b.tail;
}

// The other side's name of a bone (Arm.L / Arm.R, LeftArm / RightArm, l_arm / r_arm...); empty: none.
String mirror_name(const String &p_name) {
	static const char *suffix[][2] = { { ".L", ".R" }, { "_L", "_R" }, { "-L", "-R" }, { " L", " R" }, { ".l", ".r" }, { "_l", "_r" }, { "-l", "-r" } };
	for (const auto &sf : suffix) {
		for (int k = 0; k < 2; k++) {
			if (p_name.ends_with(sf[k])) {
				return p_name.substr(0, p_name.length() - 2) + sf[1 - k];
			}
		}
	}
	static const char *prefix[][2] = { { "L_", "R_" }, { "l_", "r_" }, { "L.", "R." }, { "l.", "r." } };
	for (const auto &pf : prefix) {
		for (int k = 0; k < 2; k++) {
			if (p_name.begins_with(pf[k])) {
				return String(pf[1 - k]) + p_name.substr(2);
			}
		}
	}
	static const char *word[][2] = { { "Left", "Right" }, { "left", "right" }, { "LEFT", "RIGHT" } };
	for (const auto &w : word) {
		for (int k = 0; k < 2; k++) {
			const int at = p_name.find(w[k]);
			if (at >= 0) {
				return p_name.substr(0, at) + w[1 - k] + p_name.substr(at + String(w[k]).length());
			}
		}
	}
	return String();
}

int mirror_bone(const Skeleton3D *s, int p_bone) {
	const String m = mirror_name(s->get_bone_name(p_bone));
	return m.is_empty() ? -1 : s->find_bone(m);
}

// A pose (local: position, rotation, scale) of p_from put on p_to mirrored across the skeleton's X = 0 plane: the
// change from its rest, in its own frame, carried over to the other bone's frame (works for any rest that is
// symmetric, whatever the axes of the bones).
void mirror_pose(const Skeleton3D *s, int p_from, int p_to, const Vector3 &p_pos, const Quaternion &p_rot, const Vector3 &p_scale, Vector3 &r_pos,
		Quaternion &r_rot, Vector3 &r_scale) {
	const Basis A = s->get_bone_global_rest(p_from).basis.orthonormalized();
	const Basis B = s->get_bone_global_rest(p_to).basis.orthonormalized();
	Basis S;
	S.set_column(0, Vector3(-1, 0, 0));
	const Basis C = B.inverse() * S * A; // (from's frame into to's, mirrored)
	const Transform3D pose(Basis(p_rot).scaled_local(p_scale), p_pos);
	const Transform3D P = s->get_bone_rest(p_from).affine_inverse() * pose;
	const Transform3D Pm(C * P.basis * C.inverse(), C.xform(P.origin));
	const Transform3D L = s->get_bone_rest(p_to) * Pm;
	r_pos = L.origin;
	r_rot = L.basis.get_rotation_quaternion();
	r_scale = L.basis.get_scale();
}

int bone_depth(const Skeleton3D *s, int b) {
	int d = 0;
	for (int p = s->get_bone_parent(b); p >= 0 && d < 1000; p = s->get_bone_parent(p)) {
		d++;
	}
	return d;
}

bool has_ancestor_in(const Skeleton3D *s, int b, const std::vector<int> &p_set) {
	int guard = 0;
	for (int p = s->get_bone_parent(b); p >= 0 && guard++ < 1000; p = s->get_bone_parent(p)) {
		if (std::find(p_set.begin(), p_set.end(), p) != p_set.end()) {
			return true;
		}
	}
	return false;
}

void sort_parents_first(const Skeleton3D *s, std::vector<int> &r_bones) {
	std::stable_sort(r_bones.begin(), r_bones.end(), [&](int a, int b) { return bone_depth(s, a) < bone_depth(s, b); });
}

String prop(int p_bone, const char *p_what) {
	return vformat("bones/%d/%s", p_bone, p_what);
}

// One track as it is now (kept for the undo): its path, its type, whether it is there, its keys.
Array track_state(const Ref<Animation> &a, const NodePath &p_path, Animation::TrackType p_type) {
	Array st;
	st.push_back(p_path);
	st.push_back((int)p_type);
	const int t = a->find_track(p_path, p_type);
	st.push_back(t >= 0);
	Array keys;
	if (t >= 0) {
		for (int k = 0; k < a->track_get_key_count(t); k++) {
			Array key;
			key.push_back(a->track_get_key_time(t, k));
			key.push_back(a->track_get_key_value(t, k));
			key.push_back(a->track_get_key_transition(t, k));
			keys.push_back(key);
		}
	}
	st.push_back(keys);
	return st;
}

const Color BONE_FILL(0.6f, 0.61f, 0.64f, 0.75f);
const Color BONE_EDGE(0.06f, 0.06f, 0.07f, 0.9f);
const Color BONE_SEL_FILL(1.0f, 0.55f, 0.12f, 0.8f);
const Color BONE_SEL_EDGE(1.0f, 0.5f, 0.05f, 1.0f);
const Color BONE_ACTIVE_FILL(1.0f, 0.76f, 0.36f, 0.88f);
const Color BONE_ACTIVE_EDGE(1.0f, 0.92f, 0.7f, 1.0f);

int pick_bone(Camera3D *p_cam, const std::vector<BoneShape> &p_bs, const std::set<int> &p_hidden, const Vector2 &p_pos) {
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const Vector3 eye = p_cam->get_global_position();
	int best = -1;
	float best_d = 1e30f, best_z = 1e30f;
	for (int b = 0; b < (int)p_bs.size(); b++) {
		if (!p_bs[b].ok || p_hidden.count(b) || p_cam->is_position_behind(p_bs[b].head) || p_cam->is_position_behind(p_bs[b].tail)) {
			continue;
		}
		Vector3 p[6];
		bone_points(p_bs[b], p);
		const Vector2 h = p_cam->unproject_position(p[0]), t = p_cam->unproject_position(p[5]);
		const float w = MAX(5.0f * es, p_cam->unproject_position(p[1]).distance_to(p_cam->unproject_position((p[1] + p[3]) * 0.5f)));
		float tt = 0.0f;
		const float d = seg_dist(p_pos, h, t, &tt);
		if (d > w) {
			continue;
		}
		const float z = eye.distance_to(p_bs[b].head.lerp(p_bs[b].tail, tt));
		if (d < best_d - 2.0f * es || (d < best_d + 2.0f * es && z < best_z)) {
			best = b;
			best_d = d;
			best_z = z;
		}
	}
	return best;
}

void draw_bones(Control *p_overlay, Camera3D *p_cam, const Skeleton3D *s, const std::vector<BoneShape> &p_bs, const std::vector<char> &p_selected, int p_active,
		int p_hover, const std::set<int> &p_hidden, bool p_names, float p_alpha) {
	const Vector3 eye = p_cam->get_global_position();
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	// the dashed lines from a parent's tail to a child that does not start there
	for (int b = 0; b < (int)p_bs.size(); b++) {
		const int p = s->get_bone_parent(b);
		if (p < 0 || !p_bs[b].ok || p_hidden.count(b) || p_hidden.count(p) || p_bs[p].tail.distance_to(p_bs[b].head) < p_bs[b].head.distance_to(p_bs[b].tail) * 0.02f) {
			continue;
		}
		if (p_cam->is_position_behind(p_bs[p].tail) || p_cam->is_position_behind(p_bs[b].head)) {
			continue;
		}
		p_overlay->draw_dashed_line(p_cam->unproject_position(p_bs[p].tail), p_cam->unproject_position(p_bs[b].head), Color(0.8f, 0.8f, 0.8f, 0.45f * p_alpha), 1.0f, 4.0f * es);
	}
	// the bones far to near, each its faces turned to the view (shaded by how much) and its edges
	std::vector<std::pair<float, int>> order;
	for (int b = 0; b < (int)p_bs.size(); b++) {
		if (p_bs[b].ok && !p_hidden.count(b) && !p_cam->is_position_behind(p_bs[b].head) && !p_cam->is_position_behind(p_bs[b].tail)) {
			order.push_back({ -eye.distance_to((p_bs[b].head + p_bs[b].tail) * 0.5f), b });
		}
	}
	std::sort(order.begin(), order.end());
	static const int tris[8][3] = { { 0, 1, 2 }, { 0, 2, 3 }, { 0, 3, 4 }, { 0, 4, 1 }, { 5, 2, 1 }, { 5, 3, 2 }, { 5, 4, 3 }, { 5, 1, 4 } };
	static const int edges[12][2] = { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 0, 4 }, { 1, 2 }, { 2, 3 }, { 3, 4 }, { 4, 1 }, { 5, 1 }, { 5, 2 }, { 5, 3 }, { 5, 4 } };
	for (const std::pair<float, int> &o : order) {
		const int b = o.second;
		Vector3 p[6];
		bone_points(p_bs[b], p);
		Vector2 q[6];
		for (int i = 0; i < 6; i++) {
			q[i] = p_cam->unproject_position(p[i]);
		}
		const Color fill = b == p_active ? BONE_ACTIVE_FILL : p_selected[b] ? BONE_SEL_FILL : BONE_FILL;
		Color edge = b == p_active ? BONE_ACTIVE_EDGE : p_selected[b] ? BONE_SEL_EDGE : BONE_EDGE;
		if (b == p_hover && b != p_active) {
			edge = p_selected[b] ? Color(1.0f, 0.85f, 0.55f) : Color(0.95f, 0.95f, 0.95f, 0.95f);
		}
		const Vector3 mid = (p[0] + p[5]) * 0.5f;
		for (const auto &t : tris) {
			const Vector3 nrm = (p[t[1]] - p[t[0]]).cross(p[t[2]] - p[t[0]]);
			const Vector3 c = (p[t[0]] + p[t[1]] + p[t[2]]) / 3.0f;
			Vector3 out = nrm;
			if (out.dot(c - mid) < 0.0f) {
				out = -out; // (outward whichever way round)
			}
			const float facing = out.normalized().dot((eye - c).normalized());
			if (facing <= 0.0f) {
				continue;
			}
			const float k = 0.55f + 0.45f * facing;
			PackedVector2Array pts;
			pts.push_back(q[t[0]]);
			pts.push_back(q[t[1]]);
			pts.push_back(q[t[2]]);
			draw_poly_safe(p_overlay, pts, Color(fill.r * k, fill.g * k, fill.b * k, fill.a * p_alpha));
		}
		for (const auto &e : edges) {
			p_overlay->draw_line(q[e[0]], q[e[1]], Color(edge.r, edge.g, edge.b, edge.a * p_alpha), (b == p_active || b == p_hover ? 1.5f : 1.0f) * es, true);
		}
	}
	if (p_names) {
		Ref<Font> font = p_overlay->get_theme_default_font();
		const int fs = MAX(8, (int)(p_overlay->get_theme_default_font_size() * 0.85f));
		for (const std::pair<float, int> &o : order) {
			const int b = o.second;
			const Vector2 at = p_cam->unproject_position((p_bs[b].head + p_bs[b].tail) * 0.5f) + Vector2(6, -4) * es;
			const String nm = s->get_bone_name(b);
			p_overlay->draw_string(font, at + Vector2(1, 1), nm, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color(0, 0, 0, 0.8f));
			p_overlay->draw_string(font, at, nm, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, p_selected[b] ? Color(1.0f, 0.8f, 0.5f) : Color(0.92f, 0.92f, 0.92f));
		}
	}
}

} // namespace hw_bones

using namespace hw_bones;

namespace {

} // namespace

void HewnEditorPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_pose_tracks_restore", "animation", "states"), &HewnEditorPlugin::_pose_tracks_restore);
	ClassDB::bind_method(D_METHOD("_tp_undo_apply", "id", "after"), &HewnEditorPlugin::_tp_undo_apply);
	ClassDB::bind_method(D_METHOD("_rig_restore", "skeleton", "snapshot"), &HewnEditorPlugin::_rig_restore);
	ClassDB::bind_method(D_METHOD("_rig_mesh_set", "mesh", "state"), &HewnEditorPlugin::_rig_mesh_set);
	ClassDB::bind_method(D_METHOD("_wp_view"), &HewnEditorPlugin::_wp_view);
}

// ------------------------------------------------------------------ the mode

Skeleton3D *HewnEditorPlugin::pose_skeleton() const {
	Skeleton3D *s = Object::cast_to<Skeleton3D>(ObjectDB::get_instance(pose_skel));
	return s && s->is_inside_tree() ? s : nullptr;
}

// The skeleton a selected node stands for: itself, the one a mesh is skinned to, the first one under it.
Skeleton3D *HewnEditorPlugin::pose_find_skeleton(Node *p_node) {
	if (!p_node) {
		return nullptr;
	}
	if (Skeleton3D *s = Object::cast_to<Skeleton3D>(p_node)) {
		return s;
	}
	if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_node)) {
		if (!mi->get_skeleton_path().is_empty()) {
			if (Skeleton3D *s = Object::cast_to<Skeleton3D>(mi->get_node_or_null(mi->get_skeleton_path()))) {
				return s;
			}
		}
	}
	std::vector<Node *> stack = { p_node };
	while (!stack.empty()) {
		Node *x = stack.front();
		stack.erase(stack.begin());
		if (Skeleton3D *s = Object::cast_to<Skeleton3D>(x)) {
			return s;
		}
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
	}
	return nullptr;
}

bool HewnEditorPlugin::enter_pose() {
	Skeleton3D *s = nullptr;
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	for (int64_t i = 0; i < sel.size() && !s; i++) {
		s = pose_find_skeleton(Object::cast_to<Node>(sel[i]));
	}
	if (!s && pose_skeleton()) {
		s = pose_skeleton();
	}
	if (!s) {
		toast(T("Pose Mode: select a Skeleton3D (or a node with one under it)"));
		return false;
	}
	if (vpaint) {
		exit_vpaint();
	}
	if (sculpt) {
		exit_sculpt();
	}
	if (rig) {
		exit_rig(false);
	}
	if (tpaint) {
		exit_tpaint(false);
	}
	if (wpaint) {
		exit_wpaint(false);
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	if (mode != MODE_OBJECT) {
		set_mode(MODE_OBJECT);
	}
	if (ObjectID(s->get_instance_id()) != pose_skel) {
		pose_sel.clear();
		pose_hidden.clear();
	}
	pose_skel = ObjectID(s->get_instance_id());
	pose = true;
	pose_hover = -1;
	pose_box = pose_press = false;
	// out of Godot's selection meanwhile (no transform gizmo, no box): the inspector keeps showing it
	pose_detached = true;
	EditorInterface::get_singleton()->get_selection()->clear();
	EditorInterface::get_singleton()->get_inspector()->call_deferred("edit", s);
	if (dock) {
		dock->show_paint_bar(true);
		dock->sync_pose_bar();
		dock->refresh();
	}
	update_overlays();
	return true;
}

void HewnEditorPlugin::exit_pose(bool p_reselect) {
	if (!pose) {
		return;
	}
	if (pose_modal) {
		_cancel_modal();
	}
	pose = false;
	pose_box = pose_press = false;
	Skeleton3D *s = pose_skeleton();
	if (pose_detached) {
		pose_detached = false;
		if (p_reselect && s) {
			EditorSelection *es = EditorInterface::get_singleton()->get_selection();
			es->clear();
			es->add_node(s);
		}
	}
	if (dock) {
		dock->show_paint_bar(false);
		dock->refresh();
	}
	update_overlays();
}

// ------------------------------------------------------------------ picking

int HewnEditorPlugin::_pose_pick(Camera3D *p_cam, const Vector2 &p_pos) const {
	Skeleton3D *s = pose_skeleton();
	if (!s || !p_cam) {
		return -1;
	}
	std::vector<BoneShape> bs;
	bone_shapes(s, bs);
	return pick_bone(p_cam, bs, pose_hidden, p_pos);
}

void HewnEditorPlugin::_pose_select(const std::vector<int> &p_bones, bool p_extend) {
	if (!p_extend) {
		pose_sel.clear();
	}
	for (int b : p_bones) {
		pose_sel.erase(std::remove(pose_sel.begin(), pose_sel.end(), b), pose_sel.end());
		pose_sel.push_back(b);
	}
	if (dock) {
		dock->sync_pose_bar();
	}
	update_overlays();
}

Basis HewnEditorPlugin::_pose_bone_basis(int p_bone) const {
	Skeleton3D *s = pose_skeleton();
	if (!s || p_bone < 0 || p_bone >= s->get_bone_count()) {
		return Basis();
	}
	return (s->get_global_transform().basis * bone_global(s, p_bone).basis).orthonormalized();
}

// ------------------------------------------------------------------ input

int32_t HewnEditorPlugin::_pose_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	Skeleton3D *s = pose_skeleton();
	if (!s) {
		exit_pose(false);
		return -1;
	}
	if (modal != MODAL_NONE) {
		return -1; // (G / R / S: the modal's own keys)
	}
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	if (k.is_valid()) {
		if (!k->is_pressed() || k->is_echo()) {
			return AFTER_GUI_INPUT_PASS;
		}
		const Key code = key_code(k);
		const bool ctrl = k->is_ctrl_pressed() || k->is_meta_pressed(), shift = k->is_shift_pressed(), alt = k->is_alt_pressed();
		const bool plain = !ctrl && !shift && !alt;
		if (code == KEY_TAB && !shift && !alt) {
			const ObjectID was = pose_skel;
			exit_pose(!ctrl);
			if (ctrl) {
				rig_skel = was;
				enter_rig(); // (Ctrl+Tab: Rigging)
			}
			return AFTER_GUI_INPUT_STOP;
		}
		if (plain && (code == KEY_G || code == KEY_R || code == KEY_S)) {
			_pose_start_transform(code == KEY_G ? MODAL_MOVE : code == KEY_R ? MODAL_ROTATE : MODAL_SCALE, p_cam);
			return AFTER_GUI_INPUT_STOP;
		}
		struct KeyOp {
			Key code;
			bool ctrl, shift, alt;
			int op;
		};
		static const KeyOp ops[] = {
			{ KEY_G, false, false, true, 1 }, { KEY_R, false, false, true, 2 }, { KEY_S, false, false, true, 3 }, //
			{ KEY_C, true, false, false, 10 }, { KEY_V, true, false, false, 11 }, { KEY_V, true, true, false, 12 }, //
			{ KEY_I, false, false, false, 20 }, { KEY_I, false, false, true, 21 }, //
			{ KEY_A, false, false, false, 30 }, { KEY_A, false, false, true, 31 }, { KEY_I, true, false, false, 32 }, //
			{ KEY_BRACKETLEFT, false, false, false, 33 }, { KEY_BRACKETRIGHT, false, false, false, 34 }, //
			{ KEY_BRACKETLEFT, false, true, false, 36 }, { KEY_BRACKETRIGHT, false, true, false, 37 }, { KEY_M, true, true, false, 35 }, //
			{ KEY_H, false, false, false, 40 }, { KEY_H, false, true, false, 41 }, { KEY_H, false, false, true, 42 },
		};
		for (const KeyOp &o : ops) {
			if (o.code == code && o.ctrl == ctrl && o.shift == shift && o.alt == alt) {
				pose_op(o.op);
				return AFTER_GUI_INPUT_STOP;
			}
		}
		// (B, C, W, Shift+A...: the other modes' tools would start on the skeleton's scene)
		if (plain && (code == KEY_B || code == KEY_C || code == KEY_W || code == KEY_X || code == KEY_E || code == KEY_F || code == KEY_K || code == KEY_DELETE)) {
			return AFTER_GUI_INPUT_STOP;
		}
		if (shift && !ctrl && !alt && (code == KEY_A || code == KEY_D || code == KEY_X)) {
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && !mb->is_alt_pressed()) {
		if (mb->is_pressed()) {
			pose_press = true;
			pose_box = false;
			pose_press_at = mb->get_position();
			mouse_pos = pose_press_at;
			return AFTER_GUI_INPUT_STOP;
		}
		if (!pose_press) {
			return AFTER_GUI_INPUT_STOP;
		}
		pose_press = false;
		const bool shift = mb->is_shift_pressed(), ctrl = mb->is_ctrl_pressed() || mb->is_meta_pressed();
		if (pose_box) {
			pose_box = false;
			const Rect2 r = Rect2(pose_press_at, Vector2()).expand(mb->get_position());
			std::vector<BoneShape> bs;
			bone_shapes(s, bs);
			std::vector<int> in;
			for (int b = 0; b < (int)bs.size(); b++) {
				if (!bs[b].ok || pose_hidden.count(b) || p_cam->is_position_behind(bs[b].head) || p_cam->is_position_behind(bs[b].tail)) {
					continue;
				}
				const Vector2 h = p_cam->unproject_position(bs[b].head), t = p_cam->unproject_position(bs[b].tail);
				if (r.has_point(h) || r.has_point(t) || r.has_point((h + t) * 0.5f)) {
					in.push_back(b);
				}
			}
			if (ctrl) {
				for (int b : in) {
					pose_sel.erase(std::remove(pose_sel.begin(), pose_sel.end(), b), pose_sel.end());
				}
				_pose_select({}, true);
			} else {
				_pose_select(in, shift);
			}
			return AFTER_GUI_INPUT_STOP;
		}
		const int b = _pose_pick(p_cam, mb->get_position());
		if (b < 0) {
			if (!shift && !ctrl) {
				_pose_select({}, false);
			}
		} else if (shift) {
			const bool active = !pose_sel.empty() && pose_sel.back() == b;
			if (active) {
				pose_sel.pop_back(); // (Shift+click on the active one: off)
				_pose_select({}, true);
			} else {
				_pose_select({ b }, true);
			}
		} else if (ctrl) {
			pose_sel.erase(std::remove(pose_sel.begin(), pose_sel.end(), b), pose_sel.end());
			_pose_select({}, true);
		} else {
			_pose_select({ b }, false);
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (mm.is_valid()) {
		mouse_pos = mm->get_position();
		if (pose_press) {
			if (!pose_box && mouse_pos.distance_to(pose_press_at) > 4.0f * es) {
				pose_box = true;
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		const int h = _pose_pick(p_cam, mouse_pos);
		if (h != pose_hover) {
			pose_hover = h;
			update_overlays();
		}
		return AFTER_GUI_INPUT_PASS;
	}
	return AFTER_GUI_INPUT_PASS;
}

// ------------------------------------------------------------------ the Pose menu

void HewnEditorPlugin::pose_op(int p_id) {
	Skeleton3D *s = pose_skeleton();
	if (!s) {
		return;
	}
	const int n = s->get_bone_count();
	std::vector<int> shown;
	for (int b = 0; b < n; b++) {
		if (!pose_hidden.count(b)) {
			shown.push_back(b);
		}
	}
	switch (p_id) {
		case 0:
		case 1:
		case 2:
		case 3:
			_pose_clear(p_id);
			break;
		case 10: {
			pose_clipboard.clear();
			for (int b : pose_sel) {
				Array v;
				v.push_back(s->get_bone_pose_position(b));
				v.push_back(s->get_bone_pose_rotation(b));
				v.push_back(s->get_bone_pose_scale(b));
				pose_clipboard[s->get_bone_name(b)] = v;
			}
			toast(vformat(T("Pose copied: %d bones"), (int)pose_sel.size()));
		} break;
		case 11:
		case 12:
			_pose_paste(p_id == 12);
			break;
		case 20:
		case 21:
			_pose_insert_keys(p_id == 21);
			break;
		case 30: {
			// A: all; everything already selected: none
			bool all = !shown.empty();
			for (int b : shown) {
				all = all && std::find(pose_sel.begin(), pose_sel.end(), b) != pose_sel.end();
			}
			_pose_select(all ? std::vector<int>() : shown, false);
		} break;
		case 31:
			_pose_select({}, false);
			break;
		case 32: {
			std::vector<int> inv;
			for (int b : shown) {
				if (std::find(pose_sel.begin(), pose_sel.end(), b) == pose_sel.end()) {
					inv.push_back(b);
				}
			}
			_pose_select(inv, false);
		} break;
		case 33:
		case 36: {
			// the parents ([), Shift: added
			std::vector<int> up;
			for (int b : pose_sel) {
				const int p = s->get_bone_parent(b);
				if (p >= 0 && !pose_hidden.count(p)) {
					up.push_back(p);
				}
			}
			if (!up.empty()) {
				_pose_select(up, p_id == 36);
			}
		} break;
		case 34:
		case 37: {
			std::vector<int> down;
			for (int b : pose_sel) {
				const PackedInt32Array ch = s->get_bone_children(b);
				for (int i = 0; i < ch.size(); i++) {
					if (!pose_hidden.count(ch[i])) {
						down.push_back(ch[i]);
					}
				}
			}
			if (!down.empty()) {
				_pose_select(down, p_id == 37);
			}
		} break;
		case 35: {
			std::vector<int> mir;
			for (int b : pose_sel) {
				const int m = mirror_bone(s, b);
				mir.push_back(m >= 0 ? m : b);
			}
			_pose_select(mir, false);
		} break;
		case 40:
			for (int b : pose_sel) {
				pose_hidden.insert(b);
			}
			_pose_select({}, false);
			break;
		case 41:
			for (int b : shown) {
				if (std::find(pose_sel.begin(), pose_sel.end(), b) == pose_sel.end()) {
					pose_hidden.insert(b);
				}
			}
			update_overlays();
			break;
		case 42: {
			std::vector<int> back(pose_hidden.begin(), pose_hidden.end());
			pose_hidden.clear();
			_pose_select(back, true);
		} break;
		case 50: {
			// every bone back to its rest pose
			std::vector<int> keep = pose_sel;
			pose_sel = shown;
			_pose_clear(0);
			pose_sel = keep;
			update_overlays();
		} break;
		default:
			break;
	}
}

// Alt+G / Alt+R / Alt+S: the selected bones' position / rotation / scale back to the rest pose (0: all three).
void HewnEditorPlugin::_pose_clear(int p_what) {
	Skeleton3D *s = pose_skeleton();
	if (!s || pose_sel.empty()) {
		return;
	}
	static const char *names[4] = { "Pose: Clear Transform", "Pose: Clear Location", "Pose: Clear Rotation", "Pose: Clear Scale" };
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(names[CLAMP(p_what, 0, 3)], UndoRedo::MERGE_DISABLE, s);
	std::vector<int> bones = pose_sel;
	for (int b : pose_sel) {
		const int m = dock && dock->pose_mirror_btn && dock->pose_mirror_btn->is_pressed() ? mirror_bone(s, b) : -1;
		if (m >= 0 && std::find(bones.begin(), bones.end(), m) == bones.end()) {
			bones.push_back(m); // (X mirror: the other side too)
		}
	}
	for (int b : bones) {
		const Transform3D r = s->get_bone_rest(b);
		if (p_what == 0 || p_what == 1) {
			ur->add_do_property(s, prop(b, "position"), r.origin);
			ur->add_undo_property(s, prop(b, "position"), s->get_bone_pose_position(b));
		}
		if (p_what == 0 || p_what == 2) {
			ur->add_do_property(s, prop(b, "rotation"), r.basis.get_rotation_quaternion());
			ur->add_undo_property(s, prop(b, "rotation"), s->get_bone_pose_rotation(b));
		}
		if (p_what == 0 || p_what == 3) {
			ur->add_do_property(s, prop(b, "scale"), r.basis.get_scale());
			ur->add_undo_property(s, prop(b, "scale"), s->get_bone_pose_scale(b));
		}
	}
	if (dock && dock->pose_autokey_btn && dock->pose_autokey_btn->is_pressed()) {
		// (the keys of the cleared pose: set first, keyed, set back - the action sets them again)
		std::vector<Vector3> p0, s0;
		std::vector<Quaternion> q0;
		for (int b : bones) {
			p0.push_back(s->get_bone_pose_position(b));
			q0.push_back(s->get_bone_pose_rotation(b));
			s0.push_back(s->get_bone_pose_scale(b));
			const Transform3D r = s->get_bone_rest(b);
			if (p_what == 0 || p_what == 1) {
				s->set_bone_pose_position(b, r.origin);
			}
			if (p_what == 0 || p_what == 2) {
				s->set_bone_pose_rotation(b, r.basis.get_rotation_quaternion());
			}
			if (p_what == 0 || p_what == 3) {
				s->set_bone_pose_scale(b, r.basis.get_scale());
			}
		}
		_pose_keys(ur, bones, false);
		for (size_t i = 0; i < bones.size(); i++) {
			s->set_bone_pose_position(bones[i], p0[i]);
			s->set_bone_pose_rotation(bones[i], q0[i]);
			s->set_bone_pose_scale(bones[i], s0[i]);
		}
	}
	ur->commit_action();
	update_overlays();
}

// Ctrl+V: the copied pose onto the bones of the same names (the selected ones, or all with those names);
// Ctrl+Shift+V: flipped - each onto its other side (Arm.L's onto Arm.R), the middle ones mirrored in place.
void HewnEditorPlugin::_pose_paste(bool p_flipped) {
	Skeleton3D *s = pose_skeleton();
	if (!s || pose_clipboard.is_empty()) {
		toast(T("Paste Pose: copy a pose first (Ctrl+C)"));
		return;
	}
	struct Put {
		int bone;
		Vector3 pos, scale;
		Quaternion rot;
	};
	std::vector<Put> puts;
	const Array names = pose_clipboard.keys();
	for (int64_t i = 0; i < names.size(); i++) {
		const String name = names[i];
		const int from = s->find_bone(name);
		const Array v = pose_clipboard[name];
		if (v.size() < 3) {
			continue;
		}
		int to = from;
		Put p;
		p.pos = v[0];
		p.rot = v[1];
		p.scale = v[2];
		if (p_flipped) {
			const int m = from >= 0 ? mirror_bone(s, from) : -1;
			to = m >= 0 ? m : from;
			if (from >= 0) {
				mirror_pose(s, from, to, v[0], v[1], v[2], p.pos, p.rot, p.scale);
			}
		}
		if (to < 0 || (!pose_sel.empty() && !p_flipped && std::find(pose_sel.begin(), pose_sel.end(), to) == pose_sel.end())) {
			continue;
		}
		p.bone = to;
		puts.push_back(p);
	}
	if (puts.empty()) {
		toast(T("Paste Pose: no bone of these names here"));
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(p_flipped ? "Pose: Paste Flipped" : "Pose: Paste", UndoRedo::MERGE_DISABLE, s);
	std::vector<int> bones;
	for (const Put &p : puts) {
		ur->add_do_property(s, prop(p.bone, "position"), p.pos);
		ur->add_do_property(s, prop(p.bone, "rotation"), p.rot);
		ur->add_do_property(s, prop(p.bone, "scale"), p.scale);
		ur->add_undo_property(s, prop(p.bone, "position"), s->get_bone_pose_position(p.bone));
		ur->add_undo_property(s, prop(p.bone, "rotation"), s->get_bone_pose_rotation(p.bone));
		ur->add_undo_property(s, prop(p.bone, "scale"), s->get_bone_pose_scale(p.bone));
		bones.push_back(p.bone);
	}
	if (dock && dock->pose_autokey_btn && dock->pose_autokey_btn->is_pressed()) {
		std::vector<Array> before;
		for (const Put &p : puts) {
			Array b;
			b.push_back(s->get_bone_pose_position(p.bone));
			b.push_back(s->get_bone_pose_rotation(p.bone));
			b.push_back(s->get_bone_pose_scale(p.bone));
			before.push_back(b);
			s->set_bone_pose_position(p.bone, p.pos);
			s->set_bone_pose_rotation(p.bone, p.rot);
			s->set_bone_pose_scale(p.bone, p.scale);
		}
		_pose_keys(ur, bones, false);
		for (size_t i = 0; i < puts.size(); i++) {
			s->set_bone_pose_position(puts[i].bone, before[i][0]);
			s->set_bone_pose_rotation(puts[i].bone, before[i][1]);
			s->set_bone_pose_scale(puts[i].bone, before[i][2]);
		}
	}
	ur->commit_action();
	update_overlays();
}

// ------------------------------------------------------------------ G / R / S

bool HewnEditorPlugin::_pose_start_transform(Modal p_kind, Camera3D *p_cam) {
	Skeleton3D *s = pose_skeleton();
	if (!s || !p_cam || modal != MODAL_NONE) {
		return false;
	}
	std::vector<int> sel;
	for (int b : pose_sel) {
		if (b >= 0 && b < s->get_bone_count() && !pose_hidden.count(b)) {
			sel.push_back(b);
		}
	}
	if (sel.empty()) {
		toast(T("Pose Mode: select a bone first (click it)"));
		return false;
	}
	const bool individual = dock->pivot_mode == HewnDock::PIVOT_INDIVIDUAL && p_kind != MODAL_MOVE;
	// the bones set: the topmost of the selection (their children follow), each of them with individual pivots
	pose_moved.clear();
	for (int b : sel) {
		if (individual || !has_ancestor_in(s, b, sel)) {
			pose_moved.push_back(b);
		}
	}
	sort_parents_first(s, pose_moved);
	// Auto IK: one bone moved, its parents turn to bring it to the mouse (it keeps its own rotation)
	pose_chain.clear();
	const bool ik = p_kind == MODAL_MOVE && pose_moved.size() == 1 && dock->pose_ik_btn && dock->pose_ik_btn->is_pressed();
	if (ik) {
		const int len = dock->pose_chain_field ? (int)dock->pose_chain_field->get_value() : 0;
		int guard = 0;
		for (int p = s->get_bone_parent(pose_moved[0]); p >= 0 && guard++ < 1000 && (len <= 0 || (int)pose_chain.size() < len); p = s->get_bone_parent(p)) {
			pose_chain.push_back(p);
		}
	}
	pose_touched = pose_moved;
	for (int b : pose_chain) {
		pose_touched.push_back(b);
	}
	pose_mirrors.clear();
	if (dock->pose_mirror_btn && dock->pose_mirror_btn->is_pressed()) {
		const std::vector<int> changed = pose_touched;
		for (int b : changed) {
			const int m = mirror_bone(s, b);
			if (m >= 0 && m != b && std::find(changed.begin(), changed.end(), m) == changed.end()) {
				pose_mirrors.push_back({ b, m });
				pose_touched.push_back(m);
			}
		}
	}
	pose_orig_pos.clear();
	pose_orig_rot.clear();
	pose_orig_scale.clear();
	for (int b : pose_touched) {
		pose_orig_pos.push_back(s->get_bone_pose_position(b));
		pose_orig_rot.push_back(s->get_bone_pose_rotation(b));
		pose_orig_scale.push_back(s->get_bone_pose_scale(b));
	}
	const Transform3D g = s->get_global_transform();
	obj_nodes.clear();
	obj_orig.clear();
	for (int b : pose_moved) {
		obj_nodes.push_back(ObjectID(s->get_instance_id())); // (the meshes under the skeleton: not snap targets)
		obj_orig.push_back(g * bone_global(s, b));
	}
	modal_object = true;
	pose_modal = true;
	const int active = pose_sel.back();
	modal_object_xf = g * bone_global(s, active);
	// snap bases: the heads and tails of the moved bones
	std::vector<BoneShape> bs;
	bone_shapes(s, bs);
	modal_orig.clear();
	modal_verts.clear();
	Vector3 median;
	for (int b : pose_moved) {
		median += bs[b].head;
		modal_orig.push_back(bs[b].head);
		modal_orig.push_back(bs[b].tail);
	}
	median /= (float)pose_moved.size();
	modal_sel_count = modal_orig.size();
	modal_weights.assign(modal_orig.size(), 1.0f);
	switch (dock->pivot_mode) {
		case HewnDock::PIVOT_CURSOR:
			modal_pivot = cursor_pos;
			break;
		case HewnDock::PIVOT_ACTIVE:
			modal_pivot = modal_object_xf.origin;
			break;
		default:
			modal_pivot = median;
			break;
	}
	modal_vpivots.assign(modal_orig.size(), modal_pivot);
	modal_orient = _orientation_basis(dock->orientation, p_cam);
	modal_mouse0 = mouse_pos;
	_reset_precise();
	modal_axis = -1;
	modal_plane = false;
	modal_local = false;
	modal_num = "";
	modal_snapped = false;
	modal_origin = 0;
	res_delta = Vector3();
	res_angle = 0.0f;
	res_scale = 1.0f;
	_plane_hit(p_cam, modal_mouse0, modal_pivot, cam_forward(p_cam), modal_hit0);
	modal = p_kind;
	_gather_snap_candidates(true);
	drag = DRAG_NONE;
	_update_modal(p_cam, snap_increment_on(false), false);
	return true;
}

void HewnEditorPlugin::_pose_restore() {
	Skeleton3D *s = pose_skeleton();
	if (!s) {
		return;
	}
	for (size_t i = 0; i < pose_touched.size() && i < pose_orig_pos.size(); i++) {
		s->set_bone_pose_position(pose_touched[i], pose_orig_pos[i]);
		s->set_bone_pose_rotation(pose_touched[i], pose_orig_rot[i]);
		s->set_bone_pose_scale(pose_touched[i], pose_orig_scale[i]);
	}
}

// The moved bones' new world transforms (as the object transform made them, by pose_moved): set on the skeleton,
// through the IK chain, mirrored.
void HewnEditorPlugin::_pose_apply(const std::vector<Transform3D> &p_world) {
	Skeleton3D *s = pose_skeleton();
	if (!s) {
		return;
	}
	_pose_restore();
	const Transform3D inv = s->get_global_transform().affine_inverse();
	const bool individual = dock->pivot_mode == HewnDock::PIVOT_INDIVIDUAL && modal != MODAL_MOVE;
	if (!pose_chain.empty() && !pose_moved.empty() && !p_world.empty()) {
		// Auto IK (cyclic coordinate descent): each parent in turn, nearest first, turned about its head to bring
		// the moved bone's head toward the target
		const int e = pose_moved[0];
		const Vector3 target = inv.xform(p_world[0].origin);
		for (int it = 0; it < 100; it++) {
			for (int j : pose_chain) {
				const Transform3D gj = bone_global(s, j);
				const Vector3 ee = bone_global(s, e).origin;
				const Vector3 a = ee - gj.origin, b = target - gj.origin;
				if (a.length() < 1e-6f || b.length() < 1e-6f) {
					continue;
				}
				const Vector3 an = a.normalized(), bn = b.normalized();
				if (an.dot(bn) > 0.0f && an.cross(bn).length() < 1e-6f) {
					continue;
				}
				const Quaternion q(an, bn);
				set_bone_global(s, j, Transform3D(Basis(q) * gj.basis, gj.origin));
			}
			if (bone_global(s, e).origin.distance_to(target) < 1e-5f) {
				break;
			}
		}
	} else {
		for (size_t i = 0; i < pose_moved.size() && i < p_world.size(); i++) {
			const int b = pose_moved[i];
			Transform3D t = inv * p_world[i];
			if (individual) {
				t.origin = bone_global(s, b).origin; // (each about its own head, where its parent put it)
			}
			set_bone_global(s, b, t);
		}
	}
	for (const std::pair<int, int> &mp : pose_mirrors) {
		Vector3 p, sc;
		Quaternion q;
		mirror_pose(s, mp.first, mp.second, s->get_bone_pose_position(mp.first), s->get_bone_pose_rotation(mp.first), s->get_bone_pose_scale(mp.first), p, q, sc);
		s->set_bone_pose_position(mp.second, p);
		s->set_bone_pose_rotation(mp.second, q);
		s->set_bone_pose_scale(mp.second, sc);
	}
}

void HewnEditorPlugin::_pose_confirm(Modal p_kind) {
	Skeleton3D *s = pose_skeleton();
	pose_modal = false;
	modal_object = false;
	obj_nodes.clear();
	if (!s) {
		return;
	}
	static const char *names[3] = { "Pose: Move Bones", "Pose: Rotate Bones", "Pose: Scale Bones" };
	std::vector<Vector3> ap, as;
	std::vector<Quaternion> aq;
	for (int b : pose_touched) {
		ap.push_back(s->get_bone_pose_position(b));
		aq.push_back(s->get_bone_pose_rotation(b));
		as.push_back(s->get_bone_pose_scale(b));
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(names[CLAMP((int)p_kind - (int)MODAL_MOVE, 0, 2)], UndoRedo::MERGE_DISABLE, s);
	for (size_t i = 0; i < pose_touched.size(); i++) {
		const int b = pose_touched[i];
		ur->add_do_property(s, prop(b, "position"), ap[i]);
		ur->add_do_property(s, prop(b, "rotation"), aq[i]);
		ur->add_do_property(s, prop(b, "scale"), as[i]);
		ur->add_undo_property(s, prop(b, "position"), pose_orig_pos[i]);
		ur->add_undo_property(s, prop(b, "rotation"), pose_orig_rot[i]);
		ur->add_undo_property(s, prop(b, "scale"), pose_orig_scale[i]);
	}
	if (dock && dock->pose_autokey_btn && dock->pose_autokey_btn->is_pressed()) {
		_pose_keys(ur, pose_touched, false);
	}
	ur->commit_action();
	update_overlays();
}

void HewnEditorPlugin::_pose_cancel() {
	_pose_restore();
	pose_modal = false;
	modal_object = false;
	obj_nodes.clear();
	update_overlays();
}

// ------------------------------------------------------------------ keys

// The animation keys go to: the AnimationPlayer over the skeleton (its root node holds it) with an animation open
// in the Animation panel, at the panel's time.
bool HewnEditorPlugin::_pose_animation(AnimationPlayer *&r_player, Ref<Animation> &r_anim, double &r_time, String &r_why) const {
	r_player = nullptr;
	Skeleton3D *s = pose_skeleton();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!s || !root) {
		r_why = T("no skeleton");
		return false;
	}
	std::vector<Node *> stack = { root };
	AnimationPlayer *any = nullptr;
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
		AnimationPlayer *ap = Object::cast_to<AnimationPlayer>(x);
		if (!ap) {
			continue;
		}
		Node *ar = ap->get_node_or_null(ap->get_root_node());
		if (!ar || !(ar == s || ar->is_ancestor_of(s))) {
			continue;
		}
		any = any ? any : ap;
		const StringName an = ap->get_assigned_animation();
		if (an != StringName() && ap->has_animation(an)) {
			r_player = ap;
			r_anim = ap->get_animation(an);
			r_time = ap->get_current_animation_position();
			return r_anim.is_valid();
		}
	}
	r_why = any ? T("open an animation in the Animation panel") : T("no AnimationPlayer above the skeleton");
	return false;
}

String HewnEditorPlugin::pose_anim_text() const {
	AnimationPlayer *ap = nullptr;
	Ref<Animation> a;
	double t = 0.0;
	String why;
	if (_pose_animation(ap, a, t, why)) {
		return vformat("%s  %.2f s", String(ap->get_assigned_animation()), t);
	}
	return why;
}

// The keys of these bones at the animation's time (inserted, or with p_delete taken out) into the open action.
void HewnEditorPlugin::_pose_keys(EditorUndoRedoManager *p_ur, const std::vector<int> &p_bones, bool p_delete) {
	Skeleton3D *s = pose_skeleton();
	AnimationPlayer *ap = nullptr;
	Ref<Animation> a;
	double t = 0.0;
	String why;
	if (!s || !_pose_animation(ap, a, t, why)) {
		toast(T("Keyframes: ") + why);
		return;
	}
	Node *ar = ap->get_node_or_null(ap->get_root_node());
	const String base = String(ar->get_path_to(s));
	const int keying = dock && dock->pose_keying ? dock->pose_keying->get_selected() : 0;
	const bool kp = keying == 0 || keying == 1 || keying == 4, kr = keying == 0 || keying == 2 || keying == 4, ks = keying == 0 || keying == 3;
	Array before, after;
	for (int b : p_bones) {
		const NodePath path(base + String(":") + s->get_bone_name(b));
		const Animation::TrackType types[3] = { Animation::TYPE_POSITION_3D, Animation::TYPE_ROTATION_3D, Animation::TYPE_SCALE_3D };
		const bool on[3] = { kp || p_delete, kr || p_delete, ks || p_delete };
		for (int k = 0; k < 3; k++) {
			if (!on[k]) {
				continue;
			}
			before.push_back(track_state(a, path, types[k]));
			int tr = a->find_track(path, types[k]);
			if (p_delete) {
				if (tr >= 0) {
					const int key = a->track_find_key(tr, t, Animation::FIND_MODE_APPROX);
					if (key >= 0) {
						a->track_remove_key(tr, key);
					}
				}
			} else {
				if (tr < 0) {
					tr = a->add_track(types[k]);
					a->track_set_path(tr, path);
				}
				const Variant v = k == 0 ? Variant(s->get_bone_pose_position(b)) : k == 1 ? Variant(s->get_bone_pose_rotation(b)) : Variant(s->get_bone_pose_scale(b));
				a->track_insert_key(tr, t, v);
			}
			after.push_back(track_state(a, path, types[k]));
		}
	}
	p_ur->add_do_method(this, "_pose_tracks_restore", a, after);
	p_ur->add_undo_method(this, "_pose_tracks_restore", a, before);
}

void HewnEditorPlugin::_pose_insert_keys(bool p_delete) {
	Skeleton3D *s = pose_skeleton();
	if (!s || pose_sel.empty()) {
		toast(T("Keyframes: select the bones first"));
		return;
	}
	AnimationPlayer *ap = nullptr;
	Ref<Animation> a;
	double t = 0.0;
	String why;
	if (!_pose_animation(ap, a, t, why)) {
		toast(T("Keyframes: ") + why);
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(p_delete ? "Pose: Delete Keyframes" : "Pose: Insert Keyframes", UndoRedo::MERGE_DISABLE, s);
	_pose_keys(ur, pose_sel, p_delete);
	ur->commit_action();
	toast(vformat(p_delete ? T("Keyframes deleted: %d bones at %.2f s") : T("Keyframes: %d bones at %.2f s"), (int)pose_sel.size(), t));
}

void HewnEditorPlugin::_pose_tracks_restore(const Ref<Animation> &p_anim, const Array &p_states) {
	if (p_anim.is_null()) {
		return;
	}
	for (int64_t i = 0; i < p_states.size(); i++) {
		const Array st = p_states[i];
		if (st.size() < 4) {
			continue;
		}
		const NodePath path = st[0];
		const Animation::TrackType type = (Animation::TrackType)(int)st[1];
		const bool exists = st[2];
		const Array keys = st[3];
		int t = p_anim->find_track(path, type);
		if (!exists) {
			if (t >= 0) {
				p_anim->remove_track(t);
			}
			continue;
		}
		if (t < 0) {
			t = p_anim->add_track(type);
			p_anim->track_set_path(t, path);
		}
		while (p_anim->track_get_key_count(t) > 0) {
			p_anim->track_remove_key(t, 0);
		}
		for (int64_t k = 0; k < keys.size(); k++) {
			const Array key = keys[k];
			p_anim->track_insert_key(t, key[0], key[1], key[2]);
		}
	}
	update_overlays();
}

// ------------------------------------------------------------------ drawing

void HewnEditorPlugin::_draw_pose(Control *p_overlay, Camera3D *p_cam) {
	Skeleton3D *s = pose_skeleton();
	if (!s || !p_cam) {
		return;
	}
	std::vector<hw_bones::BoneShape> bs;
	hw_bones::bone_shapes(s, bs);
	const int active = pose_sel.empty() ? -1 : pose_sel.back();
	std::vector<char> selected(bs.size(), 0);
	for (int b : pose_sel) {
		if (b >= 0 && b < (int)bs.size()) {
			selected[b] = 1;
		}
	}
	hw_bones::draw_bones(p_overlay, p_cam, s, bs, selected, active, pose_hover, pose_hidden, dock && dock->pose_names_btn && dock->pose_names_btn->is_pressed());
	if (pose_box) {
		const Rect2 r = Rect2(pose_press_at, Vector2()).expand(mouse_pos);
		p_overlay->draw_rect(r, Color(1, 0.6f, 0.1f, 0.12f));
		p_overlay->draw_rect(r, Color(1, 0.6f, 0.1f, 0.8f), false, 1.0f);
	}
	if (modal == MODAL_NONE) {
		Ref<Font> font = p_overlay->get_theme_default_font();
		String head = T("Pose Mode");
		if (active >= 0) {
			head += String(": ") + s->get_bone_name(active);
			if (pose_sel.size() > 1) {
				head += vformat(T(" (%d bones)"), (int)pose_sel.size());
			}
		}
		draw_header(p_overlay, font, head);
	}
	if (dock && dock->pose_anim_label) {
		const String at = pose_anim_text();
		if (dock->pose_anim_label->get_text() != at) {
			dock->pose_anim_label->set_text(at);
		}
	}
}

// ------------------------------------------------------------------ the header's second row

void HewnDock::build_pose_bar() {
	Ref<Theme> et = EditorInterface::get_singleton()->get_editor_theme();
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const Color base = et->has_color("dark_color_1", "Editor") ? et->get_color("dark_color_1", "Editor") : Color(0.1f, 0.11f, 0.13f);
	auto block = [&]() {
		PanelContainer *p = memnew(PanelContainer);
		Ref<StyleBoxFlat> sb;
		sb.instantiate();
		sb->set_bg_color(base);
		sb->set_corner_radius_all(4);
		sb->set_content_margin(SIDE_LEFT, 2);
		sb->set_content_margin(SIDE_RIGHT, 2);
		p->add_theme_stylebox_override("panel", sb);
		HBoxContainer *h = memnew(HBoxContainer);
		h->add_theme_constant_override("separation", 2);
		p->add_child(h);
		paint_row->add_child(p);
		p->hide();
		pose_blocks.push_back(p);
		return h;
	};
	auto toggle = [&](HBoxContainer *p_block, const String &p_icon, const String &p_name, const String &p_tip) {
		Button *b = memnew(Button);
		b->set_flat(true);
		b->set_toggle_mode(true);
		b->set_button_icon(icon(p_icon));
		b->set_tooltip_text(p_tip);
		b->set_name(p_name);
		b->set_theme_type_variation("FlatMenuButton");
		b->connect("toggled", callable_mp(this, &HewnDock::_on_pose_changed).unbind(1));
		p_block->add_child(b);
		return b;
	};
	HBoxContainer *mb = block();
	pose_menu = memnew(MenuButton);
	pose_menu->set_name("HewnPoseMenu");
	pose_menu->set_text(T("Pose") + String(" ") + String::chr(0x25BE));
	pose_menu->set_flat(true);
	pose_menu->set_disable_shortcuts(true); // (the keys are the view's: shown here only)
	PopupMenu *pm = pose_menu->get_popup();
	pm->add_item("Clear Transform", 0);
	pm->add_item("Clear Location", 1, (Key)(KEY_MASK_ALT | KEY_G));
	pm->add_item("Clear Rotation", 2, (Key)(KEY_MASK_ALT | KEY_R));
	pm->add_item("Clear Scale", 3, (Key)(KEY_MASK_ALT | KEY_S));
	pm->add_item("Reset All Bones", 50);
	pm->add_separator();
	pm->add_item("Copy Pose", 10, (Key)(KEY_MASK_CTRL | KEY_C));
	pm->add_item("Paste Pose", 11, (Key)(KEY_MASK_CTRL | KEY_V));
	pm->add_item("Paste Flipped Pose", 12, (Key)(KEY_MASK_CTRL | KEY_MASK_SHIFT | KEY_V));
	pm->add_separator();
	pm->add_item("Insert Keyframes", 20, KEY_I);
	pm->add_item("Delete Keyframes", 21, (Key)(KEY_MASK_ALT | KEY_I));
	pm->add_separator();
	pm->add_item("Select All", 30, KEY_A);
	pm->add_item("Select None", 31, (Key)(KEY_MASK_ALT | KEY_A));
	pm->add_item("Invert Selection", 32, (Key)(KEY_MASK_CTRL | KEY_I));
	pm->add_item("Select Parent", 33, KEY_BRACKETLEFT);
	pm->add_item("Select Children", 34, KEY_BRACKETRIGHT);
	pm->add_item("Select Mirror", 35, (Key)(KEY_MASK_CTRL | KEY_MASK_SHIFT | KEY_M));
	pm->add_separator();
	pm->add_item("Hide", 40, KEY_H);
	pm->add_item("Hide Unselected", 41, (Key)(KEY_MASK_SHIFT | KEY_H));
	pm->add_item("Reveal", 42, (Key)(KEY_MASK_ALT | KEY_H));
	pm->connect("id_pressed", callable_mp(this, &HewnDock::_on_pose_menu));
	mb->add_child(pose_menu);
	HBoxContainer *tb = block();
	pose_mirror_btn = toggle(tb, "pose_mirror", "PoseMirror", "X Mirror: the bone of the other side (Arm.L / Arm.R, LeftArm / RightArm) follows mirrored");
	pose_ik_btn = toggle(tb, "pose_ik", "PoseAutoIK", "Auto IK: G on a bone turns its parents to bring it to the mouse");
	Label *cl = memnew(Label);
	cl->set_text(T("Chain"));
	cl->add_theme_color_override("font_color", Color(0.75f, 0.75f, 0.75f));
	tb->add_child(cl);
	pose_chain_field = memnew(HewnDragField);
	pose_chain_field->setup(0, 64, 1, true, 2);
	pose_chain_field->set_custom_minimum_size(Vector2(40 * es, 0));
	pose_chain_field->set_tooltip_text("Auto IK: how many parents turn (0: all up to the root)");
	pose_chain_field->set_name("PoseChain");
	tb->add_child(pose_chain_field);
	HBoxContainer *kb = block();
	pose_autokey_btn = toggle(kb, "pose_autokey", "PoseAutoKey", "Auto Keying: each change of the pose keyed into the animation open in the Animation panel");
	pose_keying = memnew(OptionButton);
	pose_keying->set_name("PoseKeying");
	for (const char *it : { "Location, Rotation, Scale", "Location", "Rotation", "Scale", "Location and Rotation" }) {
		pose_keying->add_item(it);
	}
	pose_keying->set_flat(true);
	pose_keying->set_fit_to_longest_item(false);
	pose_keying->set_tooltip_text("What the keys hold");
	kb->add_child(pose_keying);
	Button *ins = memnew(Button);
	ins->set_flat(true);
	ins->set_button_icon(icon("pose_key"));
	ins->set_tooltip_text("Insert Keyframes (I)");
	ins->set_theme_type_variation("FlatMenuButton");
	ins->connect("pressed", callable_mp(this, &HewnDock::_on_pose_menu).bind(20));
	kb->add_child(ins);
	pose_anim_label = memnew(Label);
	pose_anim_label->set_name("PoseAnimation");
	pose_anim_label->add_theme_color_override("font_color", Color(0.7f, 0.7f, 0.7f));
	pose_anim_label->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);
	kb->add_child(pose_anim_label);
	HBoxContainer *vb = block();
	pose_names_btn = toggle(vb, "pose_names", "PoseNames", "Bone names");
}

void HewnDock::sync_pose_bar() {
	const bool on = plugin && plugin->pose && !plugin->rig && paint_bar_shown && !toolbar_folded;
	for (Control *c : pose_blocks) {
		c->set_visible(on);
	}
	if (pose_chain_field && pose_ik_btn) {
		pose_chain_field->set_editable(pose_ik_btn->is_pressed());
	}
}

void HewnDock::_on_pose_changed() {
	sync_pose_bar();
	if (plugin) {
		plugin->update_overlays();
	}
}

void HewnDock::_on_pose_menu(int p_id) {
	if (plugin) {
		plugin->pose_op(p_id);
	}
}
