// Rigging: a Skeleton3D's bones made and shaped at rest. The points are the bones' heads and tails (a child that
// starts at its parent's tail is joined to it: they move together); they are selected (click, Shift, a box, a
// bone's body: both its ends) and moved with G / R / S like everything else. Each bone keeps its Y axis toward its
// tail and its roll; its length is kept in the skeleton's meta (hw_bone_lengths). Extrude (E), add (Shift+A),
// delete (X), duplicate (Shift+D), subdivide, parent (Ctrl+P / Alt+P), rename (F2), roll (Ctrl+R), symmetrize,
// X mirror. The skins of the meshes on the skeleton follow the rest (the meshes stay where they are); Hewn meshes
// are bound to it with automatic weights. Every change is one undo step (the skeleton as it was / as it is).
#include "hw_editor.h"
#include "hw_editor_util.h"
#include "hw_bones.h"
#include "../hw_skin.h"

#include <godot_cpp/classes/animation_player.hpp>
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
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/viewport.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;
using namespace hw_editor;

namespace {

void toast(const String &p_text) {
	if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
		t->push_toast(p_text);
	}
}

Vector3 cam_forward(Camera3D *p_cam) {
	return -p_cam->get_global_transform().basis.get_column(2).normalized();
}

// The meshes on this skeleton and their skins.
std::vector<MeshInstance3D *> skinned_meshes(const Skeleton3D *s) {
	std::vector<MeshInstance3D *> out;
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root || !s) {
		return out;
	}
	std::vector<Node *> stack = { root };
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(x);
		if (mi && !mi->get_skeleton_path().is_empty() && mi->get_node_or_null(mi->get_skeleton_path()) == s) {
			out.push_back(mi);
		}
	}
	return out;
}

int bind_bone_of(const Skeleton3D *s, const Ref<Skin> &k, int i) {
	const String nm = k->get_bind_name(i);
	if (!nm.is_empty()) {
		return s->find_bone(nm);
	}
	return k->get_bind_bone(i);
}

// A name no bone has yet: the base and a number, before a side's suffix (Arm.L -> Arm.001.L).
String unique_bone_name(const Skeleton3D *s, const String &p_from) {
	String base = p_from, suffix;
	static const char *suf[] = { ".L", ".R", "_L", "_R", ".l", ".r", "_l", "_r", "-L", "-R" };
	for (const char *x : suf) {
		if (p_from.ends_with(x)) {
			base = p_from.substr(0, p_from.length() - 2);
			suffix = x;
			break;
		}
	}
	// (a number already there: the next one)
	const int dot = base.rfind(".");
	if (dot > 0 && base.substr(dot + 1).is_valid_int()) {
		base = base.substr(0, dot);
	}
	if (s->find_bone(base + suffix) < 0) {
		return base + suffix;
	}
	for (int i = 1; i < 10000; i++) {
		const String c = base + vformat(".%03d", i) + suffix;
		if (s->find_bone(c) < 0) {
			return c;
		}
	}
	return base + suffix;
}

// The rest of a bone from its head and tail: Y toward the tail, X as near its old one as can be (the roll kept).
Transform3D rest_from(const Vector3 &h, const Vector3 &t, const Basis &p_old) {
	Vector3 y = t - h;
	if (y.length() < 1e-7f) {
		y = p_old.get_column(1);
	}
	y.normalize();
	Vector3 x = p_old.get_column(0).normalized();
	x = x - y * x.dot(y);
	if (x.length() < 1e-4f) {
		x = p_old.get_column(2).normalized().cross(y);
		if (x.length() < 1e-4f) {
			x = std::fabs(y.x) < 0.9f ? Vector3(1, 0, 0).cross(y).cross(y) : Vector3(0, 0, 1).cross(y);
		}
	}
	x.normalize();
	const Vector3 z = x.cross(y).normalized();
	Basis b;
	b.set_column(0, x);
	b.set_column(1, y);
	b.set_column(2, z);
	return Transform3D(b, h);
}

Vector3 mirror_x(const Vector3 &p) {
	return Vector3(-p.x, p.y, p.z);
}

} // namespace

// ------------------------------------------------------------------ the mode

Skeleton3D *HewnEditorPlugin::rig_skeleton() const {
	Skeleton3D *s = Object::cast_to<Skeleton3D>(ObjectDB::get_instance(rig_skel));
	return s && s->is_inside_tree() ? s : nullptr;
}

bool HewnEditorPlugin::enter_rig() {
	Skeleton3D *s = nullptr;
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	for (int64_t i = 0; i < sel.size() && !s; i++) {
		s = pose_find_skeleton(Object::cast_to<Node>(sel[i]));
	}
	if (!s && pose && pose_skeleton()) {
		s = pose_skeleton();
	}
	if (!s && rig_skeleton()) {
		s = rig_skeleton();
	}
	if (!s) {
		toast(T("Rigging: select a Skeleton3D (or add one: Shift+A > Skeleton)"));
		return false;
	}
	if (pose) {
		exit_pose(false);
	}
	if (vpaint) {
		exit_vpaint();
	}
	if (sculpt) {
		exit_sculpt();
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
	if (ObjectID(s->get_instance_id()) != rig_skel) {
		rig_sel.clear();
	}
	rig_sel.erase(std::remove_if(rig_sel.begin(), rig_sel.end(), [&](int p) { return p < 0 || p >= s->get_bone_count() * 2; }), rig_sel.end());
	rig_skel = ObjectID(s->get_instance_id());
	rig = true;
	rig_hover = rig_hover_bone = -1;
	rig_box = rig_press = rig_rolling = false;
	rig_rest_was = s->is_show_rest_only();
	s->set_show_rest_only(true); // (the rest shown while it is shaped; back when the mode is left)
	rig_detached = true;
	EditorInterface::get_singleton()->get_selection()->clear();
	EditorInterface::get_singleton()->get_inspector()->call_deferred("edit", s);
	if (dock) {
		dock->show_paint_bar(true);
		dock->refresh();
	}
	update_overlays();
	return true;
}

void HewnEditorPlugin::exit_rig(bool p_reselect) {
	if (!rig) {
		return;
	}
	if (rig_modal) {
		_cancel_modal();
	}
	if (rig_rolling) {
		_rig_cancel();
	}
	rig = false;
	rig_box = rig_press = false;
	Skeleton3D *s = rig_skeleton();
	if (s) {
		s->set_show_rest_only(rig_rest_was);
	}
	if (rig_detached) {
		rig_detached = false;
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

// ------------------------------------------------------------------ the skeleton as it is / was

// Everything a change can touch: the bones (names, parents, rests, poses), the lengths, the binds of the skins on
// it; with p_meshes also the bound Hewn meshes (their weights, their skins).
Array HewnEditorPlugin::rig_snapshot(Skeleton3D *s, bool p_meshes) const {
	Array snap;
	const int n = s->get_bone_count();
	snap.push_back(n);
	for (int b = 0; b < n; b++) {
		Array e;
		e.push_back(s->get_bone_name(b));
		e.push_back(s->get_bone_parent(b));
		e.push_back(s->get_bone_rest(b));
		e.push_back(s->get_bone_pose_position(b));
		e.push_back(s->get_bone_pose_rotation(b));
		e.push_back(s->get_bone_pose_scale(b));
		snap.push_back(e);
	}
	Dictionary extra;
	if (s->has_meta("hw_bone_lengths")) {
		extra["lengths"] = Dictionary(s->get_meta("hw_bone_lengths")).duplicate();
	}
	Array skins, meshes;
	std::vector<Ref<Skin>> seen;
	for (MeshInstance3D *mi : skinned_meshes(s)) {
		Ref<Skin> k = mi->get_skin();
		if (p_meshes) {
			HewnMesh3D *hn = Object::cast_to<HewnMesh3D>(mi);
			if (hn) {
				Array me;
				me.push_back(hn);
				me.push_back(hn->get_state());
				me.push_back(k);
				meshes.push_back(me);
			}
		}
		if (k.is_null() || std::find(seen.begin(), seen.end(), k) != seen.end()) {
			continue;
		}
		seen.push_back(k);
		Array poses, names;
		PackedInt32Array bones;
		for (int i = 0; i < k->get_bind_count(); i++) {
			poses.push_back(k->get_bind_pose(i));
			bones.push_back(k->get_bind_bone(i));
			names.push_back(String(k->get_bind_name(i)));
		}
		Array ke;
		ke.push_back(k);
		ke.push_back(poses);
		ke.push_back(bones);
		ke.push_back(names);
		skins.push_back(ke);
	}
	extra["skins"] = skins;
	extra["meshes"] = meshes;
	snap.push_back(extra);
	return snap;
}

void HewnEditorPlugin::_rig_restore(Object *p_skel, const Array &p_snap) {
	Skeleton3D *s = Object::cast_to<Skeleton3D>(p_skel);
	if (!s || p_snap.is_empty()) {
		return;
	}
	const int n = p_snap[0];
	bool same = s->get_bone_count() == n;
	for (int b = 0; same && b < n; b++) {
		const Array e = p_snap[1 + b];
		same = s->get_bone_name(b) == String(e[0]);
	}
	if (!same) {
		s->clear_bones();
		for (int b = 0; b < n; b++) {
			const Array e = p_snap[1 + b];
			s->add_bone(e[0]);
		}
	}
	for (int b = 0; b < n; b++) {
		const Array e = p_snap[1 + b];
		s->set_bone_parent(b, e[1]);
	}
	for (int b = 0; b < n; b++) {
		const Array e = p_snap[1 + b];
		s->set_bone_rest(b, e[2]);
		s->set_bone_pose_position(b, e[3]);
		s->set_bone_pose_rotation(b, e[4]);
		s->set_bone_pose_scale(b, e[5]);
	}
	const Dictionary extra = p_snap[1 + n];
	if (extra.has("lengths")) {
		s->set_meta("hw_bone_lengths", Dictionary(extra["lengths"]).duplicate());
	} else if (s->has_meta("hw_bone_lengths")) {
		s->remove_meta("hw_bone_lengths");
	}
	const Array skins = extra.get("skins", Array());
	for (int64_t i = 0; i < skins.size(); i++) {
		const Array ke = skins[i];
		Ref<Skin> k = ke[0];
		if (k.is_null()) {
			continue;
		}
		const Array poses = ke[1];
		const PackedInt32Array bones = ke[2];
		const Array names = ke[3];
		k->clear_binds();
		for (int64_t j = 0; j < poses.size(); j++) {
			k->add_bind(bones[j], poses[j]);
			if (!String(names[j]).is_empty()) {
				k->set_bind_name((int)j, String(names[j]));
			}
		}
	}
	const Array meshes = extra.get("meshes", Array());
	for (int64_t i = 0; i < meshes.size(); i++) {
		const Array me = meshes[i];
		HewnMesh3D *hn = Object::cast_to<HewnMesh3D>(me[0]);
		if (hn) {
			hn->set_state(me[1]);
			hn->set_skin(me[2]);
		}
	}
	update_overlays();
}

// The rests, the poses, the binds as they are now: what a change starts from.
void HewnEditorPlugin::_rig_capture() {
	Skeleton3D *s = rig_skeleton();
	RigBase &rb = rig_base;
	rb = RigBase();
	if (!s) {
		return;
	}
	const int n = s->get_bone_count();
	std::vector<hw_bones::BoneShape> bs;
	hw_bones::bone_shapes(s, bs, true);
	const Transform3D inv = s->get_global_transform().affine_inverse();
	for (int b = 0; b < n; b++) {
		rb.g.push_back(hw_bones::rest_global(s, b));
		rb.local.push_back(s->get_bone_rest(b));
		rb.head.push_back(inv.xform(bs[b].head));
		rb.tail.push_back(inv.xform(bs[b].tail));
		rb.pp.push_back(s->get_bone_pose_position(b));
		rb.pr.push_back(s->get_bone_pose_rotation(b));
		rb.ps.push_back(s->get_bone_pose_scale(b));
	}
	std::vector<Ref<Skin>> seen;
	for (MeshInstance3D *mi : skinned_meshes(s)) {
		Ref<Skin> k = mi->get_skin();
		if (k.is_null() || std::find(seen.begin(), seen.end(), k) != seen.end()) {
			continue;
		}
		seen.push_back(k);
		RigBase::SkinBinds sb;
		sb.skin = k;
		for (int i = 0; i < k->get_bind_count(); i++) {
			sb.pose.push_back(k->get_bind_pose(i));
			sb.bone.push_back(bind_bone_of(s, k, i));
		}
		rb.skins.push_back(sb);
	}
}

// The skeleton's rests from the heads and tails (in the skeleton) of the changed bones (and their rolls, radians);
// the others keep theirs in the skeleton. The poses that were the rest follow it; the skins' binds follow too (the
// meshes stay where they are); the lengths kept.
void HewnEditorPlugin::_rig_write(const std::vector<Vector3> &p_head, const std::vector<Vector3> &p_tail, const std::vector<char> &p_changed, const std::vector<float> *p_roll) {
	Skeleton3D *s = rig_skeleton();
	const RigBase &rb = rig_base;
	if (!s) {
		return;
	}
	const int n = MIN(s->get_bone_count(), (int)rb.g.size());
	std::vector<Transform3D> g(n);
	for (int b = 0; b < n; b++) {
		const bool rolled = p_roll && b < (int)p_roll->size() && (*p_roll)[b] != 0.0f;
		if ((b < (int)p_changed.size() && p_changed[b]) || rolled) {
			g[b] = rest_from(p_head[b], p_tail[b], rb.g[b].basis);
			if (rolled) {
				const Vector3 y = g[b].basis.get_column(1).normalized();
				g[b].basis = Basis(y, (*p_roll)[b]) * g[b].basis;
			}
		} else {
			g[b] = rb.g[b];
		}
	}
	Dictionary lens = s->has_meta("hw_bone_lengths") ? Dictionary(s->get_meta("hw_bone_lengths")).duplicate() : Dictionary();
	for (int b = 0; b < n; b++) {
		const int p = s->get_bone_parent(b);
		const Transform3D local = p >= 0 && p < n ? g[p].affine_inverse() * g[b] : g[b];
		s->set_bone_rest(b, local);
		const bool at_rest = rb.pp[b].is_equal_approx(rb.local[b].origin) && std::fabs(rb.pr[b].dot(rb.local[b].basis.get_rotation_quaternion())) > 0.99999f;
		if (at_rest) {
			s->set_bone_pose_position(b, local.origin);
			s->set_bone_pose_rotation(b, local.basis.get_rotation_quaternion());
			s->set_bone_pose_scale(b, local.basis.get_scale());
		}
		lens[s->get_bone_name(b)] = p_head[b].distance_to(p_tail[b]);
	}
	s->set_meta("hw_bone_lengths", lens);
	for (const RigBase::SkinBinds &sb : rb.skins) {
		for (int i = 0; i < (int)sb.pose.size() && i < sb.skin->get_bind_count(); i++) {
			const int bone = sb.bone[i];
			if (bone >= 0 && bone < n) {
				sb.skin->set_bind_pose(i, g[bone].affine_inverse() * rb.g[bone] * sb.pose[i]);
			}
		}
		// (a skin made from the rest of each bone in turn: binds for new bones too)
		bool identity = sb.skin->get_bind_count() < s->get_bone_count();
		for (int i = 0; identity && i < sb.skin->get_bind_count(); i++) {
			identity = sb.skin->get_bind_bone(i) == i && String(sb.skin->get_bind_name(i)).is_empty();
		}
		if (identity) {
			for (int b = sb.skin->get_bind_count(); b < s->get_bone_count(); b++) {
				sb.skin->add_bind(b, hw_bones::rest_global(s, b).affine_inverse());
			}
		}
	}
	update_overlays();
}

void HewnEditorPlugin::_rig_commit(const String &p_name) {
	Skeleton3D *s = rig_skeleton();
	if (!s || rig_before.is_empty()) {
		return;
	}
	const Array after = rig_snapshot(s, Array(Dictionary(rig_before[rig_before.size() - 1]).get("meshes", Array())).size() > 0);
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(p_name, UndoRedo::MERGE_DISABLE, s);
	ur->add_do_method(this, "_rig_restore", s, after);
	ur->add_undo_method(this, "_rig_restore", s, rig_before);
	ur->commit_action(false);
	rig_before = Array();
	update_overlays();
}

// ------------------------------------------------------------------ points, selection

int HewnEditorPlugin::_rig_pick_point(Camera3D *p_cam, const Vector2 &p_pos) const {
	Skeleton3D *s = rig_skeleton();
	if (!s || !p_cam) {
		return -1;
	}
	std::vector<hw_bones::BoneShape> bs;
	hw_bones::bone_shapes(s, bs, true);
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const Vector3 eye = p_cam->get_global_position();
	int best = -1;
	float best_d = 9.0f * es, best_z = 1e30f;
	for (int b = 0; b < (int)bs.size(); b++) {
		for (int k = 0; k < 2; k++) {
			const Vector3 w = k == 0 ? bs[b].head : bs[b].tail;
			if (p_cam->is_position_behind(w)) {
				continue;
			}
			const float d = p_cam->unproject_position(w).distance_to(p_pos);
			const float z = eye.distance_to(w);
			if (d < best_d - 1.0f || (d <= best_d + 1.0f && z < best_z)) {
				best = b * 2 + k;
				best_d = d;
				best_z = z;
			}
		}
	}
	return best;
}

// A point and the ones at its place joined to it: a tail and the heads of the children starting there.
std::vector<int> HewnEditorPlugin::_rig_joined(int p_point) const {
	std::vector<int> out = { p_point };
	Skeleton3D *s = rig_skeleton();
	if (!s || p_point < 0) {
		return out;
	}
	std::vector<hw_bones::BoneShape> bs;
	hw_bones::bone_shapes(s, bs, true);
	const int b = p_point / 2;
	if (b >= (int)bs.size()) {
		return out;
	}
	const float eps = MAX(1e-5f, bs[b].head.distance_to(bs[b].tail) * 1e-3f);
	int owner = -1; // (the bone whose tail it is)
	if (p_point % 2 == 1) {
		owner = b;
	} else {
		const int p = s->get_bone_parent(b);
		if (p >= 0 && bs[p].tail.distance_to(bs[b].head) < eps) {
			owner = p;
			out.push_back(p * 2 + 1);
		}
	}
	if (owner >= 0) {
		const PackedInt32Array ch = s->get_bone_children(owner);
		for (int i = 0; i < ch.size(); i++) {
			if (bs[ch[i]].head.distance_to(bs[owner].tail) < eps && std::find(out.begin(), out.end(), ch[i] * 2) == out.end()) {
				out.push_back(ch[i] * 2);
			}
		}
	}
	return out;
}

void HewnEditorPlugin::_rig_select(const std::vector<int> &p_points, bool p_extend) {
	if (!p_extend) {
		rig_sel.clear();
	}
	for (int p : p_points) {
		for (int q : _rig_joined(p)) {
			rig_sel.erase(std::remove(rig_sel.begin(), rig_sel.end(), q), rig_sel.end());
			rig_sel.push_back(q);
		}
	}
	update_overlays();
}

std::vector<int> HewnEditorPlugin::_rig_selected_bones() const {
	std::vector<int> out;
	Skeleton3D *s = rig_skeleton();
	if (!s) {
		return out;
	}
	for (int b = 0; b < s->get_bone_count(); b++) {
		if (std::find(rig_sel.begin(), rig_sel.end(), b * 2) != rig_sel.end() && std::find(rig_sel.begin(), rig_sel.end(), b * 2 + 1) != rig_sel.end()) {
			out.push_back(b);
		}
	}
	return out;
}

// ------------------------------------------------------------------ G / R / S

bool HewnEditorPlugin::_rig_start_transform(Modal p_kind, Camera3D *p_cam) {
	Skeleton3D *s = rig_skeleton();
	if (!s || !p_cam || modal != MODAL_NONE || rig_sel.empty()) {
		if (rig_sel.empty()) {
			toast(T("Rigging: select a joint or a bone first"));
		}
		return false;
	}
	_rig_capture();
	if (rig_before.is_empty()) {
		rig_before = rig_snapshot(s, false);
		rig_action = p_kind == MODAL_MOVE ? "Rigging: Move" : p_kind == MODAL_ROTATE ? "Rigging: Rotate" : "Rigging: Scale";
	}
	rig_points.clear();
	for (int p : rig_sel) {
		for (int q : _rig_joined(p)) {
			if (std::find(rig_points.begin(), rig_points.end(), q) == rig_points.end()) {
				rig_points.push_back(q);
			}
		}
	}
	const Transform3D g = s->get_global_transform();
	obj_nodes.clear();
	obj_orig.clear();
	modal_orig.clear();
	modal_verts.clear();
	Vector3 median;
	for (int p : rig_points) {
		const Vector3 w = g.xform(p % 2 == 0 ? rig_base.head[p / 2] : rig_base.tail[p / 2]);
		obj_nodes.push_back(ObjectID(s->get_instance_id()));
		obj_orig.push_back(Transform3D(Basis(), w));
		modal_orig.push_back(w);
		median += w;
	}
	median /= (float)MAX(1, (int)rig_points.size());
	modal_object = true;
	rig_modal = true;
	const int active = rig_sel.back();
	modal_object_xf = g * rig_base.g[active / 2];
	modal_sel_count = modal_orig.size();
	modal_weights.assign(modal_orig.size(), 1.0f);
	switch (dock->pivot_mode) {
		case HewnDock::PIVOT_CURSOR:
			modal_pivot = cursor_pos;
			break;
		case HewnDock::PIVOT_ACTIVE:
			modal_pivot = g.xform(active % 2 == 0 ? rig_base.head[active / 2] : rig_base.tail[active / 2]);
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

// The moved points' new places (world, by rig_points), with X mirror the other side's too.
void HewnEditorPlugin::_rig_apply(const std::vector<Transform3D> &p_world) {
	Skeleton3D *s = rig_skeleton();
	if (!s) {
		return;
	}
	const RigBase &rb = rig_base;
	const int n = (int)rb.g.size();
	std::vector<Vector3> head = rb.head, tail = rb.tail;
	std::vector<char> changed(n, 0);
	const Transform3D inv = s->get_global_transform().affine_inverse();
	const bool mirror = dock && dock->rig_mirror_btn && dock->rig_mirror_btn->is_pressed();
	std::vector<char> moved(n * 2, 0);
	for (size_t i = 0; i < rig_points.size() && i < p_world.size(); i++) {
		moved[rig_points[i]] = 1;
	}
	for (size_t i = 0; i < rig_points.size() && i < p_world.size(); i++) {
		const int p = rig_points[i], b = p / 2;
		const Vector3 at = inv.xform(p_world[i].origin);
		(p % 2 == 0 ? head : tail)[b] = at;
		changed[b] = 1;
		if (mirror) {
			const int m = hw_bones::mirror_bone(s, b);
			if (m >= 0 && m < n && m != b && !moved[m * 2 + p % 2]) {
				(p % 2 == 0 ? head : tail)[m] = mirror_x(at);
				changed[m] = 1;
			}
		}
	}
	_rig_write(head, tail, changed);
}

void HewnEditorPlugin::_rig_confirm() {
	rig_modal = false;
	rig_rolling = false;
	modal_object = false;
	obj_nodes.clear();
	_rig_commit(rig_action.is_empty() ? String("Rigging") : rig_action);
	rig_action = "";
}

void HewnEditorPlugin::_rig_cancel() {
	Skeleton3D *s = rig_skeleton();
	if (s && !rig_before.is_empty()) {
		_rig_restore(s, rig_before);
	}
	rig_before = Array();
	rig_modal = false;
	rig_rolling = false;
	modal_object = false;
	obj_nodes.clear();
	rig_action = "";
	update_overlays();
}

// ------------------------------------------------------------------ the operations

// E: a new bone from each selected tail (Shift+D: a copy of each selected bone), then moved with G.
void HewnEditorPlugin::_rig_extrude(Camera3D *p_cam, bool p_duplicate) {
	Skeleton3D *s = rig_skeleton();
	if (!s) {
		return;
	}
	_rig_capture();
	const RigBase rb = rig_base;
	const bool mirror = dock && dock->rig_mirror_btn && dock->rig_mirror_btn->is_pressed();
	std::vector<int> from, primary; // (primary: the selected ones; the others their mirror, moved mirrored)
	if (p_duplicate) {
		from = _rig_selected_bones();
		primary = from;
	} else {
		for (int p : rig_sel) {
			if (p % 2 == 1 && std::find(from.begin(), from.end(), p / 2) == from.end()) {
				from.push_back(p / 2);
			}
		}
		primary = from;
		if (mirror) {
			const std::vector<int> f0 = from;
			for (int b : f0) {
				const int m = hw_bones::mirror_bone(s, b);
				if (m >= 0 && std::find(from.begin(), from.end(), m) == from.end()) {
					from.push_back(m);
				}
			}
		}
	}
	if (from.empty()) {
		toast(p_duplicate ? T("Rigging: select whole bones to duplicate") : T("Rigging: select a bone's tail to extrude from"));
		return;
	}
	rig_before = rig_snapshot(s, false);
	rig_action = p_duplicate ? "Rigging: Duplicate Bones" : "Rigging: Extrude Bones";
	Dictionary lens = s->has_meta("hw_bone_lengths") ? Dictionary(s->get_meta("hw_bone_lengths")).duplicate() : Dictionary();
	for (int b = 0; b < (int)rb.g.size(); b++) {
		lens[s->get_bone_name(b)] = rb.head[b].distance_to(rb.tail[b]);
	}
	std::map<int, int> copy_of;
	std::vector<int> made;
	for (int b : from) {
		const String nb = unique_bone_name(s, s->get_bone_name(b));
		const int c = s->add_bone(nb);
		const float len = rb.head[b].distance_to(rb.tail[b]);
		Transform3D gc;
		int parent;
		float clen;
		if (p_duplicate) {
			const int bp = s->get_bone_parent(b);
			parent = copy_of.count(bp) ? copy_of[bp] : bp;
			gc = rb.g[b];
			clen = len;
		} else {
			parent = b;
			gc = Transform3D(rb.g[b].basis.orthonormalized(), rb.tail[b]);
			clen = MAX(1e-3f, len * 0.5f);
		}
		copy_of[b] = c;
		s->set_bone_parent(c, parent);
		const Transform3D pg = parent >= 0 ? (parent < (int)rb.g.size() ? rb.g[parent] : hw_bones::rest_global(s, parent)) : Transform3D();
		const Transform3D local = parent >= 0 ? pg.affine_inverse() * gc : gc;
		s->set_bone_rest(c, local);
		s->set_bone_pose_position(c, local.origin);
		s->set_bone_pose_rotation(c, local.basis.get_rotation_quaternion());
		s->set_bone_pose_scale(c, local.basis.get_scale());
		lens[nb] = clen;
		made.push_back(c);
	}
	s->set_meta("hw_bone_lengths", lens);
	rig_sel.clear();
	for (size_t i = 0; i < made.size(); i++) {
		if (std::find(primary.begin(), primary.end(), from[i]) == primary.end()) {
			continue; // (the other side's: moved mirrored)
		}
		if (p_duplicate) {
			rig_sel.push_back(made[i] * 2);
		}
		rig_sel.push_back(made[i] * 2 + 1);
	}
	_rig_start_transform(MODAL_MOVE, p_cam);
	update_overlays();
}

// X: the selected bones out; their children go to the nearest bone left above them, the meshes' weights of a
// deleted bone to it too.
void HewnEditorPlugin::_rig_delete() {
	Skeleton3D *s = rig_skeleton();
	if (!s) {
		return;
	}
	const std::vector<int> del = _rig_selected_bones();
	if (del.empty()) {
		toast(T("Rigging: select whole bones to delete (both ends, or click the bone)"));
		return;
	}
	_rig_capture();
	const RigBase rb = rig_base;
	rig_before = rig_snapshot(s, true);
	const int n = s->get_bone_count();
	std::vector<char> gone(n, 0);
	for (int b : del) {
		gone[b] = 1;
	}
	std::vector<int> map(n, -1); // old -> new (the deleted: their nearest kept ancestor's)
	int k = 0;
	for (int b = 0; b < n; b++) {
		if (!gone[b]) {
			map[b] = k++;
		}
	}
	auto kept_up = [&](int b) {
		int guard = 0;
		while (b >= 0 && gone[b] && guard++ < 1000) {
			b = s->get_bone_parent(b);
		}
		return b;
	};
	std::vector<int> to(n);
	for (int b = 0; b < n; b++) {
		const int u = kept_up(b);
		to[b] = u >= 0 ? map[u] : -1;
	}
	struct Keep {
		String name;
		int parent;
		Transform3D g;
		Vector3 pp, ps;
		Quaternion pr;
		bool at_rest;
	};
	std::vector<Keep> keep;
	for (int b = 0; b < n; b++) {
		if (gone[b]) {
			continue;
		}
		Keep e;
		e.name = s->get_bone_name(b);
		const int p = kept_up(s->get_bone_parent(b));
		e.parent = p >= 0 ? map[p] : -1;
		e.g = rb.g[b];
		e.pp = rb.pp[b];
		e.pr = rb.pr[b];
		e.ps = rb.ps[b];
		e.at_rest = rb.pp[b].is_equal_approx(rb.local[b].origin) && std::fabs(rb.pr[b].dot(rb.local[b].basis.get_rotation_quaternion())) > 0.99999f;
		keep.push_back(e);
	}
	std::vector<Transform3D> kg;
	for (const Keep &e : keep) {
		kg.push_back(e.g);
	}
	std::vector<String> del_names;
	for (int b : del) {
		del_names.push_back(s->get_bone_name(b));
	}
	s->clear_bones();
	for (const Keep &e : keep) {
		s->add_bone(e.name);
	}
	for (int i = 0; i < (int)keep.size(); i++) {
		s->set_bone_parent(i, keep[i].parent);
	}
	for (int i = 0; i < (int)keep.size(); i++) {
		const Transform3D local = keep[i].parent >= 0 ? kg[keep[i].parent].affine_inverse() * kg[i] : kg[i];
		s->set_bone_rest(i, local);
		s->set_bone_pose_position(i, keep[i].at_rest ? local.origin : keep[i].pp);
		s->set_bone_pose_rotation(i, keep[i].at_rest ? local.basis.get_rotation_quaternion() : keep[i].pr);
		s->set_bone_pose_scale(i, keep[i].at_rest ? local.basis.get_scale() : keep[i].ps);
	}
	if (s->has_meta("hw_bone_lengths")) {
		Dictionary lens = Dictionary(s->get_meta("hw_bone_lengths")).duplicate();
		for (const String &nm : del_names) {
			lens.erase(nm);
		}
		s->set_meta("hw_bone_lengths", lens);
	}
	// the skins: binds of a deleted bone onto the bone left above it; Hewn meshes: weights moved the same way
	for (const RigBase::SkinBinds &sb : rb.skins) {
		for (int i = 0; i < (int)sb.pose.size() && i < sb.skin->get_bind_count(); i++) {
			const int ob = sb.bone[i];
			if (ob < 0 || ob >= n) {
				continue;
			}
			const int nb = to[ob];
			if (String(sb.skin->get_bind_name(i)).is_empty()) {
				sb.skin->set_bind_bone(i, MAX(0, nb));
			} else if (nb >= 0) {
				sb.skin->set_bind_name(i, s->get_bone_name(nb));
			}
			if (gone[ob] && nb >= 0) {
				sb.skin->set_bind_pose(i, kg[nb].affine_inverse() * rb.g[ob] * sb.pose[i]);
			}
		}
	}
	for (MeshInstance3D *mi : skinned_meshes(s)) {
		HewnMesh3D *hn = Object::cast_to<HewnMesh3D>(mi);
		if (!hn || !hn->has_weights()) {
			continue;
		}
		hewn::Mesh &d = hn->get_data();
		for (auto &w : d.weights) {
			std::map<int, float> acc;
			for (const auto &p : w) {
				if (p.first >= 0 && p.first < n && to[p.first] >= 0) {
					acc[to[p.first]] += p.second;
				}
			}
			w.clear();
			for (const auto &kv : acc) {
				w.push_back({ kv.first, kv.second });
			}
			hewn::skin::normalize(w);
		}
		hn->rebuild();
		hn->set_skin(s->create_skin_from_rest_transforms());
	}
	rig_sel.clear();
	_rig_commit("Rigging: Delete Bones");
}

void HewnEditorPlugin::_on_rig_rename_ok() {
	if (rig_rename_edit) {
		_rig_rename(rig_rename_edit->get_text().strip_edges());
	}
}

// F2: the active bone's name (the lengths, the skins' named binds and the animations' tracks follow).
void HewnEditorPlugin::_rig_rename(const String &p_name) {
	Skeleton3D *s = rig_skeleton();
	if (!s || rig_sel.empty() || p_name.is_empty()) {
		return;
	}
	const int b = rig_sel.back() / 2;
	const String old = s->get_bone_name(b);
	if (old == p_name) {
		return;
	}
	if (s->find_bone(p_name) >= 0) {
		toast(vformat(T("Rigging: there is a bone named %s already"), p_name));
		return;
	}
	rig_before = rig_snapshot(s, false);
	s->set_bone_name(b, p_name);
	if (s->has_meta("hw_bone_lengths")) {
		Dictionary lens = Dictionary(s->get_meta("hw_bone_lengths")).duplicate();
		if (lens.has(old)) {
			lens[p_name] = lens[old];
			lens.erase(old);
		}
		s->set_meta("hw_bone_lengths", lens);
	}
	for (MeshInstance3D *mi : skinned_meshes(s)) {
		Ref<Skin> k = mi->get_skin();
		for (int i = 0; k.is_valid() && i < k->get_bind_count(); i++) {
			if (String(k->get_bind_name(i)) == old) {
				k->set_bind_name(i, p_name);
			}
		}
	}
	const Array after = rig_snapshot(s, false);
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Rigging: Rename Bone", UndoRedo::MERGE_DISABLE, s);
	ur->add_do_method(this, "_rig_restore", s, after);
	ur->add_undo_method(this, "_rig_restore", s, rig_before);
	// the animations' tracks of the bone: renamed too
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	std::vector<Node *> stack = { root };
	while (root && !stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
		AnimationPlayer *ap = Object::cast_to<AnimationPlayer>(x);
		Node *ar = ap ? ap->get_node_or_null(ap->get_root_node()) : nullptr;
		if (!ar || !(ar == s || ar->is_ancestor_of(s))) {
			continue;
		}
		const String base = String(ar->get_path_to(s)) + ":";
		const PackedStringArray names = ap->get_animation_list();
		for (const String &an : names) {
			Ref<Animation> a = ap->get_animation(an);
			for (int t = 0; a.is_valid() && t < a->get_track_count(); t++) {
				if (String(a->track_get_path(t)) == base + old) {
					ur->add_do_method(a.ptr(), "track_set_path", t, NodePath(base + p_name));
					ur->add_undo_method(a.ptr(), "track_set_path", t, NodePath(base + old));
				}
			}
		}
	}
	ur->commit_action(false);
	rig_before = Array();
	update_overlays();
}

// Hewn meshes bound to the skeleton: under it, in its space (their look kept), weights made (0 automatic, 1 the
// nearest bone, 2 empty), a skin from its rest. One undo step.
void HewnEditorPlugin::_rig_bind(Skeleton3D *p_skel, const std::vector<HewnMesh3D *> &p_meshes, int p_kind) {
	if (!p_skel || p_meshes.empty()) {
		toast(T("Bind: select the Hewn meshes and the skeleton"));
		return;
	}
	if (p_skel->get_bone_count() == 0) {
		toast(T("Bind: the skeleton has no bones"));
		return;
	}
	std::vector<hw_bones::BoneShape> bs;
	hw_bones::bone_shapes(p_skel, bs, true);
	const Transform3D sg = p_skel->get_global_transform();
	const Transform3D sinv = sg.affine_inverse();
	std::vector<Vector3> heads, tails;
	for (const hw_bones::BoneShape &b : bs) {
		heads.push_back(sinv.xform(b.head));
		tails.push_back(sinv.xform(b.tail));
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Rigging: Bind Meshes", UndoRedo::MERGE_DISABLE, p_skel);
	for (HewnMesh3D *n : p_meshes) {
		Dictionary before;
		before["parent"] = n->get_parent();
		before["index"] = n->get_index();
		before["transform"] = n->get_transform();
		before["skeleton"] = n->get_skeleton_path();
		before["skin"] = n->get_skin();
		const Dictionary state0 = n->get_state();
		before["state"] = state0;
		// the mesh in the skeleton's space: its vertices where they are seen
		const Transform3D to_skel = sinv * n->get_global_transform();
		hewn::Mesh &d = n->get_data();
		for (Vector3 &p : d.pos) {
			p = to_skel.xform(p);
		}
		std::vector<char> use(heads.size(), 1);
		hewn::skin::auto_weights(d, heads, tails, use, p_kind);
		n->commit(true);
		Dictionary after;
		after["parent"] = p_skel;
		after["index"] = n->get_parent() == p_skel ? n->get_index() : p_skel->get_child_count();
		after["transform"] = Transform3D();
		after["skeleton"] = NodePath("..");
		after["skin"] = p_skel->create_skin_from_rest_transforms();
		after["state"] = n->get_state();
		n->set_state(state0);
		ur->add_do_method(this, "_rig_mesh_set", n, after);
		ur->add_undo_method(this, "_rig_mesh_set", n, before);
	}
	ur->commit_action();
	toast(vformat(T("Bound to %s: %d meshes"), String(p_skel->get_name()), (int)p_meshes.size()));
}

void HewnEditorPlugin::_rig_mesh_set(Object *p_mesh, const Dictionary &p_state) {
	HewnMesh3D *n = Object::cast_to<HewnMesh3D>(p_mesh);
	Node *parent = Object::cast_to<Node>(p_state.get("parent", Variant()));
	if (!n) {
		return;
	}
	if (parent && n->get_parent() != parent) {
		n->reparent(parent, false);
	}
	if (parent && (int)p_state.get("index", -1) >= 0) {
		parent->move_child(n, MIN((int)p_state["index"], parent->get_child_count() - 1));
	}
	n->set_transform(p_state.get("transform", Transform3D()));
	n->set_skeleton_path(p_state.get("skeleton", NodePath("..")));
	n->set_skin(p_state.get("skin", Variant()));
	n->set_state(p_state.get("state", Dictionary()));
	update_overlays();
}

// The Object menu's: the selected Hewn meshes bound to the selected skeleton (or the one in the selected node), or
// unbound (no weights, no skin; they stay where they are).
void HewnEditorPlugin::rig_object_op(const String &p_op) {
	Skeleton3D *s = nullptr;
	std::vector<HewnMesh3D *> meshes;
	for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
		HewnMesh3D *hn = Object::cast_to<HewnMesh3D>(v);
		if (hn) {
			meshes.push_back(hn);
		} else if (!s) {
			s = pose_find_skeleton(Object::cast_to<Node>(v));
		}
	}
	if (p_op == "unbind") {
		if (meshes.empty()) {
			toast(T("Unbind: select the Hewn meshes"));
			return;
		}
		EditorUndoRedoManager *ur = get_undo_redo();
		ur->create_action("Rigging: Unbind Meshes", UndoRedo::MERGE_DISABLE, meshes[0]);
		for (HewnMesh3D *n : meshes) {
			Dictionary before, after;
			before["state"] = n->get_state();
			before["transform"] = n->get_transform();
			before["skeleton"] = n->get_skeleton_path();
			before["skin"] = n->get_skin();
			Dictionary st = n->get_state();
			st.erase("weight_offsets");
			st.erase("weight_bones");
			st.erase("weight_values");
			after["state"] = st;
			after["transform"] = n->get_transform();
			after["skeleton"] = n->get_skeleton_path();
			after["skin"] = Variant();
			ur->add_do_method(this, "_rig_mesh_set", n, after);
			ur->add_undo_method(this, "_rig_mesh_set", n, before);
		}
		ur->commit_action();
		return;
	}
	const int kind = p_op == "bind_nearest" ? 1 : p_op == "bind_empty" ? 2 : 0;
	_rig_bind(s, meshes, kind);
}

// Shift+A > Skeleton: a skeleton with one bone at the 3D cursor, then Rigging.
void HewnEditorPlugin::add_skeleton() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return;
	}
	Skeleton3D *s = memnew(Skeleton3D);
	s->set_name("Skeleton3D");
	s->add_bone("Root");
	s->set_bone_rest(0, Transform3D());
	Dictionary lens;
	lens["Root"] = 0.5f;
	s->set_meta("hw_bone_lengths", lens);
	Node3D *r3 = Object::cast_to<Node3D>(root);
	Transform3D at;
	at.origin = r3 ? r3->get_global_transform().affine_inverse().xform(cursor_pos) : cursor_pos;
	s->set_transform(at);
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Add Skeleton", UndoRedo::MERGE_DISABLE, root);
	ur->add_do_method(root, "add_child", s, true);
	ur->add_do_method(s, "set_owner", root);
	ur->add_do_reference(s);
	ur->add_undo_method(root, "remove_child", s);
	ur->commit_action();
	EditorSelection *es = EditorInterface::get_singleton()->get_selection();
	es->clear();
	es->add_node(s);
	rig_sel = { 0, 1 };
	rig_skel = ObjectID(s->get_instance_id());
	callable_mp(this, &HewnEditorPlugin::enter_rig).call_deferred();
}

void HewnEditorPlugin::rig_op(int p_id) {
	Skeleton3D *s = rig_skeleton();
	if (!s) {
		return;
	}
	Camera3D *cam = _viewport_camera();
	const int n = s->get_bone_count();
	switch (p_id) {
		case 0: // extrude
			_rig_extrude(cam, false);
			break;
		case 1: { // add a bone at the 3D cursor
			_rig_capture();
			rig_before = rig_snapshot(s, false);
			float avg = 0.0f;
			for (int b = 0; b < (int)rig_base.head.size(); b++) {
				avg += rig_base.head[b].distance_to(rig_base.tail[b]);
			}
			avg = rig_base.head.empty() ? 0.5f : avg / rig_base.head.size();
			const String nm = unique_bone_name(s, "Bone");
			const int c = s->add_bone(nm);
			const Vector3 h = s->get_global_transform().affine_inverse().xform(cursor_pos);
			const Transform3D g(Basis(), h);
			s->set_bone_rest(c, g);
			s->set_bone_pose_position(c, h);
			Dictionary lens = s->has_meta("hw_bone_lengths") ? Dictionary(s->get_meta("hw_bone_lengths")).duplicate() : Dictionary();
			for (int b = 0; b < (int)rig_base.head.size(); b++) {
				lens[s->get_bone_name(b)] = rig_base.head[b].distance_to(rig_base.tail[b]);
			}
			lens[nm] = MAX(0.05f, avg);
			s->set_meta("hw_bone_lengths", lens);
			_rig_capture();
			_rig_write(rig_base.head, rig_base.tail, std::vector<char>(), nullptr); // (the skins' binds for it)
			rig_sel = { c * 2, c * 2 + 1 };
			_rig_commit("Rigging: Add Bone");
		} break;
		case 2:
			_rig_delete();
			break;
		case 3:
			_rig_extrude(cam, true);
			break;
		case 4: { // subdivide: each selected bone in two, the second half a new bone its children go under
			const std::vector<int> sel = _rig_selected_bones();
			if (sel.empty()) {
				toast(T("Rigging: select whole bones to subdivide"));
				break;
			}
			_rig_capture();
			const RigBase rb = rig_base;
			rig_before = rig_snapshot(s, false);
			Dictionary lens = s->has_meta("hw_bone_lengths") ? Dictionary(s->get_meta("hw_bone_lengths")).duplicate() : Dictionary();
			for (int b = 0; b < (int)rb.g.size(); b++) {
				lens[s->get_bone_name(b)] = rb.head[b].distance_to(rb.tail[b]);
			}
			std::vector<Transform3D> g = rb.g;
			std::vector<int> parent(rb.g.size());
			for (int b = 0; b < (int)rb.g.size(); b++) {
				parent[b] = s->get_bone_parent(b);
			}
			for (int b : sel) {
				const Vector3 mid = (rb.head[b] + rb.tail[b]) * 0.5f;
				const float half = rb.head[b].distance_to(rb.tail[b]) * 0.5f;
				const String nm = unique_bone_name(s, s->get_bone_name(b));
				const int c = s->add_bone(nm);
				g.push_back(Transform3D(rb.g[b].basis, mid));
				parent.push_back(b);
				const PackedInt32Array ch = s->get_bone_children(b);
				for (int i = 0; i < ch.size(); i++) {
					if (ch[i] != c) {
						parent[ch[i]] = c;
					}
				}
				lens[s->get_bone_name(b)] = half;
				lens[nm] = half;
			}
			for (int b = 0; b < (int)g.size(); b++) {
				s->set_bone_parent(b, parent[b]);
			}
			for (int b = 0; b < (int)g.size(); b++) {
				const Transform3D local = parent[b] >= 0 ? g[parent[b]].affine_inverse() * g[b] : g[b];
				const bool at_rest = b >= (int)rb.g.size() || (rb.pp[b].is_equal_approx(rb.local[b].origin) && std::fabs(rb.pr[b].dot(rb.local[b].basis.get_rotation_quaternion())) > 0.99999f);
				s->set_bone_rest(b, local);
				if (at_rest) {
					s->set_bone_pose_position(b, local.origin);
					s->set_bone_pose_rotation(b, local.basis.get_rotation_quaternion());
					s->set_bone_pose_scale(b, local.basis.get_scale());
				}
			}
			s->set_meta("hw_bone_lengths", lens);
			_rig_capture();
			_rig_write(rig_base.head, rig_base.tail, std::vector<char>(), nullptr);
			_rig_commit("Rigging: Subdivide Bones");
		} break;
		case 5:
		case 6: { // Ctrl+P: the selected bones under the active one; Alt+P: under none (their place kept)
			std::vector<int> sel = _rig_selected_bones();
			const int active = rig_sel.empty() ? -1 : rig_sel.back() / 2;
			if (sel.empty() || (p_id == 5 && active < 0)) {
				toast(T("Rigging: select the bones, the new parent last"));
				break;
			}
			_rig_capture();
			const RigBase rb = rig_base;
			rig_before = rig_snapshot(s, false);
			for (int b : sel) {
				if (p_id == 5) {
					bool loop = b == active;
					for (int p = active, guard = 0; p >= 0 && guard < 1000 && !loop; p = s->get_bone_parent(p), guard++) {
						loop = p == b;
					}
					if (loop) {
						continue;
					}
				}
				const int np = p_id == 5 ? active : -1;
				s->set_bone_parent(b, np);
				const Transform3D local = np >= 0 ? rb.g[np].affine_inverse() * rb.g[b] : rb.g[b];
				const bool at_rest = rb.pp[b].is_equal_approx(rb.local[b].origin) && std::fabs(rb.pr[b].dot(rb.local[b].basis.get_rotation_quaternion())) > 0.99999f;
				s->set_bone_rest(b, local);
				if (at_rest) {
					s->set_bone_pose_position(b, local.origin);
					s->set_bone_pose_rotation(b, local.basis.get_rotation_quaternion());
					s->set_bone_pose_scale(b, local.basis.get_scale());
				}
			}
			_rig_commit(p_id == 5 ? "Rigging: Parent Bones" : "Rigging: Clear Parent");
		} break;
		case 7: { // F2: rename
			if (rig_sel.empty()) {
				toast(T("Rigging: select a bone first"));
				break;
			}
			if (!rig_rename_dialog) {
				rig_rename_dialog = memnew(ConfirmationDialog);
				rig_rename_dialog->set_title(T("Rename Bone"));
				rig_rename_edit = memnew(LineEdit);
				rig_rename_edit->set_custom_minimum_size(Vector2(240, 0) * EditorInterface::get_singleton()->get_editor_scale());
				rig_rename_dialog->add_child(rig_rename_edit);
				rig_rename_dialog->register_text_enter(rig_rename_edit);
				rig_rename_dialog->connect("confirmed", callable_mp(this, &HewnEditorPlugin::_on_rig_rename_ok));
				EditorInterface::get_singleton()->get_base_control()->add_child(rig_rename_dialog);
			}
			rig_rename_edit->set_text(s->get_bone_name(rig_sel.back() / 2));
			rig_rename_dialog->popup_centered();
			rig_rename_edit->grab_focus();
			rig_rename_edit->select_all();
		} break;
		case 8: { // Ctrl+R: roll, with the mouse
			if (_rig_selected_bones().empty()) {
				toast(T("Rigging: select whole bones to roll"));
				break;
			}
			_rig_capture();
			rig_before = rig_snapshot(s, false);
			rig_action = "Rigging: Roll";
			rig_rolling = true;
			rig_roll_from = mouse_pos.x;
			rig_roll_angle = 0.0f;
			update_overlays();
		} break;
		case 9: { // symmetrize: the other side's bones made / put where these are, mirrored
			std::vector<int> sel = _rig_selected_bones();
			if (sel.empty()) {
				for (int b = 0; b < n; b++) {
					sel.push_back(b);
				}
			}
			_rig_capture();
			const RigBase rb = rig_base;
			rig_before = rig_snapshot(s, false);
			Dictionary lens = s->has_meta("hw_bone_lengths") ? Dictionary(s->get_meta("hw_bone_lengths")).duplicate() : Dictionary();
			for (int b = 0; b < n; b++) {
				lens[s->get_bone_name(b)] = rb.head[b].distance_to(rb.tail[b]);
			}
			// (parents first: a new bone's parent may be a new one too)
			std::sort(sel.begin(), sel.end(), [&](int a, int b) {
				int da = 0, db = 0;
				for (int p = s->get_bone_parent(a); p >= 0 && da < 1000; p = s->get_bone_parent(p)) {
					da++;
				}
				for (int p = s->get_bone_parent(b); p >= 0 && db < 1000; p = s->get_bone_parent(p)) {
					db++;
				}
				return da < db;
			});
			Basis S;
			S.set_column(0, Vector3(-1, 0, 0));
			std::map<int, Transform3D> gnew; // (bone: its new rest in the skeleton)
			int made = 0;
			for (int b : sel) {
				const String other = hw_bones::mirror_name(s->get_bone_name(b));
				if (other.is_empty()) {
					continue;
				}
				int m = s->find_bone(other);
				const Transform3D gm(S * rb.g[b].basis * S, mirror_x(rb.g[b].origin));
				if (m < 0) {
					m = s->add_bone(other);
					const int bp = s->get_bone_parent(b);
					int mp = bp;
					if (bp >= 0) {
						const int mpp = s->find_bone(hw_bones::mirror_name(s->get_bone_name(bp)));
						mp = mpp >= 0 ? mpp : bp;
					}
					s->set_bone_parent(m, mp);
					made++;
				}
				gnew[m] = gm;
				lens[other] = rb.head[b].distance_to(rb.tail[b]);
			}
			auto g_of = [&](int b) {
				auto it = gnew.find(b);
				return it != gnew.end() ? it->second : (b < (int)rb.g.size() ? rb.g[b] : Transform3D());
			};
			for (int b = 0; b < s->get_bone_count(); b++) {
				if (!gnew.count(b) && b < (int)rb.g.size()) {
					continue;
				}
				const int p = s->get_bone_parent(b);
				const Transform3D local = p >= 0 ? g_of(p).affine_inverse() * g_of(b) : g_of(b);
				s->set_bone_rest(b, local);
				const bool at_rest = b >= (int)rb.g.size() || (rb.pp[b].is_equal_approx(rb.local[b].origin) && std::fabs(rb.pr[b].dot(rb.local[b].basis.get_rotation_quaternion())) > 0.99999f);
				if (at_rest) {
					s->set_bone_pose_position(b, local.origin);
					s->set_bone_pose_rotation(b, local.basis.get_rotation_quaternion());
					s->set_bone_pose_scale(b, local.basis.get_scale());
				}
			}
			// (the children of a moved bone keep their place)
			for (int b = 0; b < (int)rb.g.size(); b++) {
				const int p = s->get_bone_parent(b);
				if (!gnew.count(b) && p >= 0 && gnew.count(p)) {
					s->set_bone_rest(b, gnew[p].affine_inverse() * rb.g[b]);
				}
			}
			s->set_meta("hw_bone_lengths", lens);
			_rig_commit("Rigging: Symmetrize");
			toast(vformat(T("Symmetrize: %d bones made, %d placed"), made, (int)gnew.size() - made));
		} break;
		case 20: { // select all / none / invert / mirror
			rig_sel.clear();
			for (int b = 0; b < n; b++) {
				rig_sel.push_back(b * 2);
				rig_sel.push_back(b * 2 + 1);
			}
			update_overlays();
		} break;
		case 21:
			rig_sel.clear();
			update_overlays();
			break;
		case 22: {
			std::vector<int> inv;
			for (int p = 0; p < n * 2; p++) {
				if (std::find(rig_sel.begin(), rig_sel.end(), p) == rig_sel.end()) {
					inv.push_back(p);
				}
			}
			rig_sel = inv;
			update_overlays();
		} break;
		case 23: {
			std::vector<int> mir;
			for (int p : rig_sel) {
				const int m = hw_bones::mirror_bone(s, p / 2);
				mir.push_back(m >= 0 ? m * 2 + p % 2 : p);
			}
			rig_sel = mir;
			update_overlays();
		} break;
		case 30:
		case 31:
		case 32: { // the Hewn meshes under the skeleton bound to it
			std::vector<HewnMesh3D *> meshes;
			for (int i = 0; i < s->get_child_count(); i++) {
				if (HewnMesh3D *hn = Object::cast_to<HewnMesh3D>(s->get_child(i))) {
					meshes.push_back(hn);
				}
			}
			if (meshes.empty()) {
				toast(T("Bind: no Hewn mesh under the skeleton (or select meshes and the skeleton in object mode: Object > Bind)"));
				break;
			}
			_rig_bind(s, meshes, p_id - 30);
		} break;
		default:
			break;
	}
	update_overlays();
}

// ------------------------------------------------------------------ input

int32_t HewnEditorPlugin::_rig_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	Skeleton3D *s = rig_skeleton();
	if (!s) {
		exit_rig(false);
		return -1;
	}
	if (modal != MODAL_NONE) {
		return -1;
	}
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	if (rig_rolling) {
		// Ctrl+R: the mouse across turns the selected bones about their axes (Ctrl: by 5 degrees)
		if (mm.is_valid()) {
			mouse_pos = mm->get_position();
			float a = (mouse_pos.x - rig_roll_from) * 0.5f;
			if (mm->is_ctrl_pressed()) {
				a = std::round(a / 5.0f) * 5.0f;
			}
			rig_roll_angle = a;
			std::vector<float> roll(rig_base.g.size(), 0.0f);
			for (int b : _rig_selected_bones()) {
				roll[b] = a * (float)Math::PI / 180.0f;
			}
			_rig_write(rig_base.head, rig_base.tail, std::vector<char>(), &roll);
			return AFTER_GUI_INPUT_STOP;
		}
		const bool ok = (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_LEFT) ||
				(k.is_valid() && k->is_pressed() && (key_code(k) == KEY_ENTER || key_code(k) == KEY_KP_ENTER));
		const bool no = (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT) || (k.is_valid() && k->is_pressed() && key_code(k) == KEY_ESCAPE);
		if (ok) {
			rig_rolling = false;
			_rig_commit("Rigging: Roll");
		} else if (no) {
			_rig_cancel();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (k.is_valid()) {
		if (!k->is_pressed() || k->is_echo()) {
			return AFTER_GUI_INPUT_PASS;
		}
		const Key code = key_code(k);
		const bool ctrl = k->is_ctrl_pressed() || k->is_meta_pressed(), shift = k->is_shift_pressed(), alt = k->is_alt_pressed();
		const bool plain = !ctrl && !shift && !alt;
		if (code == KEY_TAB && !shift && !alt) {
			const ObjectID was = rig_skel;
			exit_rig(!ctrl);
			if (ctrl) {
				if (pose_skel != was) {
					pose_sel.clear();
					pose_hidden.clear();
				}
				pose_skel = was;
				enter_pose(); // (Ctrl+Tab: Pose mode)
			}
			return AFTER_GUI_INPUT_STOP;
		}
		if (plain && (code == KEY_G || code == KEY_R || code == KEY_S)) {
			_rig_start_transform(code == KEY_G ? MODAL_MOVE : code == KEY_R ? MODAL_ROTATE : MODAL_SCALE, p_cam);
			return AFTER_GUI_INPUT_STOP;
		}
		struct KeyOp {
			Key code;
			bool ctrl, shift, alt;
			int op;
		};
		static const KeyOp ops[] = {
			{ KEY_E, false, false, false, 0 }, { KEY_A, false, true, false, 1 }, { KEY_X, false, false, false, 2 }, { KEY_DELETE, false, false, false, 2 },
			{ KEY_D, false, true, false, 3 }, { KEY_P, true, false, false, 5 }, { KEY_P, false, false, true, 6 }, { KEY_F2, false, false, false, 7 },
			{ KEY_R, true, false, false, 8 }, { KEY_A, false, false, false, 20 }, { KEY_A, false, false, true, 21 }, { KEY_I, true, false, false, 22 },
			{ KEY_M, true, true, false, 23 },
		};
		for (const KeyOp &o : ops) {
			if (o.code == code && o.ctrl == ctrl && o.shift == shift && o.alt == alt) {
				if (o.op == 20) {
					// A: all; everything already selected: none
					rig_op(rig_sel.size() >= (size_t)s->get_bone_count() * 2 ? 21 : 20);
				} else {
					rig_op(o.op);
				}
				return AFTER_GUI_INPUT_STOP;
			}
		}
		if (plain && (code == KEY_B || code == KEY_C || code == KEY_W || code == KEY_K || code == KEY_F)) {
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && !mb->is_alt_pressed()) {
		if (mb->is_pressed()) {
			rig_press = true;
			rig_box = false;
			rig_press_at = mb->get_position();
			mouse_pos = rig_press_at;
			return AFTER_GUI_INPUT_STOP;
		}
		if (!rig_press) {
			return AFTER_GUI_INPUT_STOP;
		}
		rig_press = false;
		const bool shift = mb->is_shift_pressed(), ctrl = mb->is_ctrl_pressed() || mb->is_meta_pressed();
		std::vector<hw_bones::BoneShape> bs;
		hw_bones::bone_shapes(s, bs, true);
		if (rig_box) {
			rig_box = false;
			const Rect2 r = Rect2(rig_press_at, Vector2()).expand(mb->get_position());
			std::vector<int> in;
			for (int b = 0; b < (int)bs.size(); b++) {
				for (int e = 0; e < 2; e++) {
					const Vector3 w = e == 0 ? bs[b].head : bs[b].tail;
					if (!p_cam->is_position_behind(w) && r.has_point(p_cam->unproject_position(w))) {
						in.push_back(b * 2 + e);
					}
				}
			}
			if (ctrl) {
				for (int p : in) {
					rig_sel.erase(std::remove(rig_sel.begin(), rig_sel.end(), p), rig_sel.end());
				}
				update_overlays();
			} else {
				_rig_select(in, shift);
			}
			return AFTER_GUI_INPUT_STOP;
		}
		std::vector<int> picked;
		const int pt = _rig_pick_point(p_cam, mb->get_position());
		if (pt >= 0) {
			picked.push_back(pt);
		} else {
			const int b = hw_bones::pick_bone(p_cam, bs, std::set<int>(), mb->get_position());
			if (b >= 0) {
				picked.push_back(b * 2);
				picked.push_back(b * 2 + 1);
			}
		}
		if (picked.empty()) {
			if (!shift && !ctrl) {
				_rig_select({}, false);
			}
		} else if (ctrl) {
			for (int p : picked) {
				for (int q : _rig_joined(p)) {
					rig_sel.erase(std::remove(rig_sel.begin(), rig_sel.end(), q), rig_sel.end());
				}
			}
			update_overlays();
		} else if (shift && picked.size() == 1 && !rig_sel.empty() && rig_sel.back() == picked[0]) {
			for (int q : _rig_joined(picked[0])) {
				rig_sel.erase(std::remove(rig_sel.begin(), rig_sel.end(), q), rig_sel.end()); // (Shift+click on the active one: off)
			}
			update_overlays();
		} else {
			_rig_select(picked, shift);
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (mm.is_valid()) {
		mouse_pos = mm->get_position();
		if (rig_press) {
			if (!rig_box && mouse_pos.distance_to(rig_press_at) > 4.0f * es) {
				rig_box = true;
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		const int h = _rig_pick_point(p_cam, mouse_pos);
		int hb = -1;
		if (h < 0) {
			std::vector<hw_bones::BoneShape> bs;
			hw_bones::bone_shapes(s, bs, true);
			hb = hw_bones::pick_bone(p_cam, bs, std::set<int>(), mouse_pos);
		}
		if (h != rig_hover || hb != rig_hover_bone) {
			rig_hover = h;
			rig_hover_bone = hb;
			update_overlays();
		}
		return AFTER_GUI_INPUT_PASS;
	}
	return AFTER_GUI_INPUT_PASS;
}

// ------------------------------------------------------------------ drawing

void HewnEditorPlugin::_draw_rig(Control *p_overlay, Camera3D *p_cam) {
	Skeleton3D *s = rig_skeleton();
	if (!s || !p_cam) {
		return;
	}
	std::vector<hw_bones::BoneShape> bs;
	hw_bones::bone_shapes(s, bs, true);
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const int n = (int)bs.size();
	std::vector<char> sel_pt(n * 2, 0), sel_bone(n, 0);
	for (int p : rig_sel) {
		if (p >= 0 && p < n * 2) {
			sel_pt[p] = 1;
		}
	}
	for (int b = 0; b < n; b++) {
		sel_bone[b] = sel_pt[b * 2] && sel_pt[b * 2 + 1];
	}
	const int active = rig_sel.empty() || rig_sel.back() / 2 >= n || !sel_bone[rig_sel.back() / 2] ? -1 : rig_sel.back() / 2;
	hw_bones::draw_bones(p_overlay, p_cam, s, bs, sel_bone, active, rig_hover_bone, std::set<int>(), dock && dock->rig_names_btn && dock->rig_names_btn->is_pressed());
	// the joints: the heads and the tails, the selected ones orange (the active one lighter)
	const int act_pt = rig_sel.empty() ? -1 : rig_sel.back();
	for (int b = 0; b < n; b++) {
		for (int e = 0; e < 2; e++) {
			const Vector3 w = e == 0 ? bs[b].head : bs[b].tail;
			if (p_cam->is_position_behind(w)) {
				continue;
			}
			const int p = b * 2 + e;
			const Vector2 q = p_cam->unproject_position(w);
			const float r = (e == 0 ? 4.0f : 3.5f) * es;
			const Color fill = p == act_pt ? Color(1.0f, 0.85f, 0.5f) : sel_pt[p] ? Color(1.0f, 0.55f, 0.1f) : Color(0.75f, 0.75f, 0.78f);
			p_overlay->draw_circle(q, r + 1.0f * es, p == rig_hover ? Color(1, 1, 1) : Color(0.05f, 0.05f, 0.05f, 0.9f), true, -1.0f, true);
			p_overlay->draw_circle(q, r, fill, true, -1.0f, true);
		}
	}
	if (rig_box) {
		const Rect2 r = Rect2(rig_press_at, Vector2()).expand(mouse_pos);
		p_overlay->draw_rect(r, Color(1, 0.6f, 0.1f, 0.12f));
		p_overlay->draw_rect(r, Color(1, 0.6f, 0.1f, 0.8f), false, 1.0f);
	}
	if (modal == MODAL_NONE) {
		String head = T("Rigging");
		if (!rig_sel.empty() && rig_sel.back() / 2 < n) {
			head += String(": ") + s->get_bone_name(rig_sel.back() / 2);
		}
		if (rig_rolling) {
			head = vformat(T("Roll: %d degrees (Ctrl: by 5)"), (int)std::round(rig_roll_angle));
		}
		draw_header(p_overlay, p_overlay->get_theme_default_font(), head);
	}
}

// ------------------------------------------------------------------ the header's second row

void HewnDock::build_rig_bar() {
	Ref<Theme> et = EditorInterface::get_singleton()->get_editor_theme();
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
		rig_blocks.push_back(p);
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
		b->connect("toggled", callable_mp(this, &HewnDock::_on_rig_changed).unbind(1));
		p_block->add_child(b);
		return b;
	};
	HBoxContainer *mb = block();
	rig_menu = memnew(MenuButton);
	rig_menu->set_name("HewnRigMenu");
	rig_menu->set_text(T("Rigging") + String(" ") + String::chr(0x25BE));
	rig_menu->set_flat(true);
	rig_menu->set_disable_shortcuts(true);
	PopupMenu *pm = rig_menu->get_popup();
	pm->add_item("Extrude", 0, KEY_E);
	pm->add_item("Add Bone", 1, (Key)(KEY_MASK_SHIFT | KEY_A));
	pm->add_item("Duplicate", 3, (Key)(KEY_MASK_SHIFT | KEY_D));
	pm->add_item("Delete Bones", 2, KEY_X);
	pm->add_item("Subdivide", 4);
	pm->add_separator();
	pm->add_item("Make Parent (to the Active)", 5, (Key)(KEY_MASK_CTRL | KEY_P));
	pm->add_item("Clear Parent", 6, (Key)(KEY_MASK_ALT | KEY_P));
	pm->add_item("Rename...", 7, KEY_F2);
	pm->add_item("Roll", 8, (Key)(KEY_MASK_CTRL | KEY_R));
	pm->add_item("Symmetrize", 9);
	pm->add_separator();
	pm->add_item("Select All", 20, KEY_A);
	pm->add_item("Select None", 21, (Key)(KEY_MASK_ALT | KEY_A));
	pm->add_item("Invert Selection", 22, (Key)(KEY_MASK_CTRL | KEY_I));
	pm->add_item("Select Mirror", 23, (Key)(KEY_MASK_CTRL | KEY_MASK_SHIFT | KEY_M));
	pm->add_separator();
	pm->add_item("Bind Meshes Under It: Automatic Weights", 30);
	pm->add_item("Bind Meshes Under It: Nearest Bone", 31);
	pm->add_item("Bind Meshes Under It: Empty Weights", 32);
	pm->connect("id_pressed", callable_mp(this, &HewnDock::_on_rig_menu));
	mb->add_child(rig_menu);
	HBoxContainer *tb = block();
	rig_mirror_btn = toggle(tb, "pose_mirror", "RigMirror", "X Mirror: the other side's joint (Arm.L / Arm.R) moves mirrored, extrude makes both sides");
	rig_names_btn = toggle(tb, "pose_names", "RigNames", "Bone names");
}

void HewnDock::sync_rig_bar() {
	const bool on = plugin && plugin->rig && paint_bar_shown && !toolbar_folded;
	for (Control *c : rig_blocks) {
		c->set_visible(on);
	}
}

void HewnDock::_on_rig_menu(int p_id) {
	if (plugin) {
		plugin->rig_op(p_id);
	}
}

void HewnDock::_on_rig_changed() {
	if (plugin) {
		plugin->update_overlays();
	}
}
