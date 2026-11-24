// Weight Paint: the weights of one bone on a skinned Hewn mesh, painted. The mesh is drawn by a copy colored by the
// weight (blue none, green half, red all), lit like Single Color; the skeleton shows its rest meanwhile and its
// bones are drawn over (the active one picked with Ctrl+click or in the header). Tools: draw toward the weight,
// add, subtract, smooth, sample. The other bones make room (auto normalize); X mirror paints the other side's
// vertex for the other side's bone. A stroke is one undo step (the mesh's state before / after).
#include "hw_editor.h"
#include "../hw_undo_state.h"
#include "hw_editor_util.h"
#include "hw_bones.h"
#include "../hw_skin.h"

#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_inspector.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/popup_panel.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/world3d.hpp>

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

// blue (none) - cyan - green (half) - yellow - red (all)
Color weight_color(float w) {
	const float t = CLAMP(w, 0.0f, 1.0f);
	return Color::from_hsv(0.66f * (1.0f - t), 0.85f, 0.35f + 0.6f * MIN(1.0f, t * 4.0f + 0.25f));
}

float wp_falloff(int p_type, float d) {
	if (d >= 1.0f) {
		return 0.0f;
	}
	const float k = 1.0f - d;
	switch (p_type) {
		case 1:
			return std::sqrt(MAX(0.0f, 1.0f - d * d));
		case 2:
			return std::sqrt(k);
		case 3:
			return k * k;
		case 4:
			return k;
		case 5:
			return 1.0f;
		default:
			return k * k * (3.0f - 2.0f * k);
	}
}


} // namespace

// ------------------------------------------------------------------ the mode

HewnMesh3D *HewnEditorPlugin::wp_mesh() const {
	HewnMesh3D *n = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(wp_node));
	return n && n->is_inside_tree() ? n : nullptr;
}

Skeleton3D *HewnEditorPlugin::wp_skeleton() const {
	HewnMesh3D *n = wp_mesh();
	if (!n || n->get_skeleton_path().is_empty()) {
		return nullptr;
	}
	return Object::cast_to<Skeleton3D>(n->get_node_or_null(n->get_skeleton_path()));
}

bool HewnEditorPlugin::enter_wpaint() {
	HewnMesh3D *n = get_edited();
	if (!n) {
		const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
		for (int64_t i = 0; i < sel.size() && !n; i++) {
			n = Object::cast_to<HewnMesh3D>(sel[i].operator Object *());
		}
	}
	if (!n && wp_mesh()) {
		n = wp_mesh();
	}
	Skeleton3D *s = n && !n->get_skeleton_path().is_empty() ? Object::cast_to<Skeleton3D>(n->get_node_or_null(n->get_skeleton_path())) : nullptr;
	if (!n || !n->has_weights() || !s) {
		toast(T("Weight Paint: bind a Hewn mesh to a skeleton first (Object > Bind to Skeleton)"));
		return false;
	}
	if (vpaint) {
		exit_vpaint();
	}
	if (sculpt) {
		exit_sculpt();
	}
	if (pose) {
		exit_pose(false);
	}
	if (tpaint) {
		exit_tpaint(false);
	}
	if (rig) {
		exit_rig(false);
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	if (mode != MODE_OBJECT) {
		set_mode(MODE_OBJECT);
	}
	wp_node = ObjectID(n->get_instance_id());
	wp_skel = ObjectID(s->get_instance_id());
	wpaint = true;
	wp_painting = wp_resizing = false;
	wp_rest_was = s->is_show_rest_only();
	s->set_show_rest_only(true); // (the mesh at rest: as its weights are painted)
	if (wp_bone < 0 || wp_bone >= s->get_bone_count()) {
		wp_bone = 0;
	}
	wp_detached = true;
	EditorInterface::get_singleton()->get_selection()->clear();
	EditorInterface::get_singleton()->get_inspector()->call_deferred("edit", n);
	if (dock) {
		dock->show_paint_bar(true);
		dock->refresh();
	}
	_wp_view();
	update_overlays();
	return true;
}

void HewnEditorPlugin::exit_wpaint(bool p_reselect) {
	if (!wpaint) {
		return;
	}
	if (wp_painting) {
		_wp_stroke_end();
	}
	wpaint = false;
	wp_resizing = false;
	if (Input::get_singleton()->get_mouse_mode() == Input::MOUSE_MODE_HIDDEN) {
		Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
	}
	_wp_view_clear();
	wp_view_mat.unref();
	if (Skeleton3D *s = Object::cast_to<Skeleton3D>(ObjectDB::get_instance(wp_skel))) {
		s->set_show_rest_only(wp_rest_was);
	}
	HewnMesh3D *n = wp_mesh();
	if (wp_detached) {
		wp_detached = false;
		if (p_reselect && n) {
			EditorSelection *es = EditorInterface::get_singleton()->get_selection();
			es->clear();
			es->add_node(n);
		}
	}
	if (dock) {
		dock->show_paint_bar(false);
		dock->refresh();
	}
	update_overlays();
}

void HewnEditorPlugin::wp_set_bone(int p_bone) {
	Skeleton3D *s = wp_skeleton();
	if (!s || p_bone < 0 || p_bone >= s->get_bone_count()) {
		return;
	}
	wp_bone = p_bone;
	if (dock) {
		dock->sync_wpaint_bar();
	}
	_wp_view();
	update_overlays();
}

// ------------------------------------------------------------------ the colors

void HewnEditorPlugin::_wp_view_clear() {
	if (wp_inst.is_valid()) {
		RenderingServer::get_singleton()->free_rid(wp_inst);
		wp_inst = RID();
	}
	wp_view_mesh.unref();
	if (HewnMesh3D *n = wp_mesh()) {
		if (n->get_instance().is_valid()) {
			RenderingServer::get_singleton()->instance_set_visible(n->get_instance(), n->is_visible_in_tree());
		}
	}
}

void HewnEditorPlugin::_wp_view() {
	HewnMesh3D *n = wp_mesh();
	if (!n || !wpaint || !n->get_world_3d().is_valid()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (wp_view_mat.is_null()) {
		Ref<Shader> sh;
		sh.instantiate();
		sh->set_code(String("shader_type spatial;\nrender_mode unshaded, cull_back;\n") +
				"float studio(vec3 n) {\n\treturn 0.42 + 0.5 * max(dot(n, normalize(vec3(-0.4, 0.6, 0.7))), 0.0) + 0.2 * max(dot(n, normalize(vec3(0.7, 0.1, 0.4))), 0.0);\n}\n" +
				"vec3 to_linear(vec3 c) {\n\treturn mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));\n}\n" +
				"void fragment() {\n\tALBEDO = to_linear(COLOR.rgb * studio(normalize(NORMAL)));\n}\n");
		wp_view_mat.instantiate();
		wp_view_mat->set_shader(sh);
	}
	const hewn::Mesh &m = n->get_data();
	PackedVector3Array pos, nrm;
	PackedColorArray col;
	std::vector<Vector3> pts;
	std::vector<int> tris;
	const bool have = m.weights.size() == m.pos.size();
	for (const hewn::Face &F : m.faces) {
		if (F.hidden || F.size() < 3) {
			continue;
		}
		const Vector3 fn = hewn::face_normal(m, F);
		hewn::face_points(m, F, pts);
		tris.clear();
		hewn::triangulate(pts, fn, tris);
		for (size_t k = 0; k + 2 < tris.size(); k += 3) {
			for (int c : { tris[k], tris[k + 2], tris[k + 1] }) { // (Godot's front: the other way round)
				const int v = F.v[c];
				pos.push_back(m.pos[v]);
				nrm.push_back(fn);
				col.push_back(weight_color(have ? hewn::skin::weight_of(m.weights[v], wp_bone) : 0.0f));
			}
		}
	}
	if (wp_inst.is_valid()) {
		rs->free_rid(wp_inst);
		wp_inst = RID();
	}
	if (pos.is_empty()) {
		return;
	}
	Array arr;
	arr.resize(Mesh::ARRAY_MAX);
	arr[Mesh::ARRAY_VERTEX] = pos;
	arr[Mesh::ARRAY_NORMAL] = nrm;
	arr[Mesh::ARRAY_COLOR] = col;
	wp_view_mesh.instantiate();
	wp_view_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr);
	wp_inst = rs->instance_create2(wp_view_mesh->get_rid(), n->get_world_3d()->get_scenario());
	rs->instance_set_transform(wp_inst, n->get_global_transform());
	rs->instance_geometry_set_material_override(wp_inst, wp_view_mat->get_rid());
	rs->instance_geometry_set_cast_shadows_setting(wp_inst, RenderingServer::SHADOW_CASTING_SETTING_OFF);
	if (n->get_instance().is_valid()) {
		rs->instance_set_visible(n->get_instance(), false); // (the mesh itself: not drawn while its copy is)
	}
}

// ------------------------------------------------------------------ painting

void HewnEditorPlugin::_wp_dab(Camera3D *p_cam, const Vector2 &p_pos, float p_pressure) {
	HewnMesh3D *n = wp_mesh();
	Skeleton3D *s = wp_skeleton();
	if (!n || !s || !p_cam) {
		return;
	}
	hewn::Mesh &m = n->get_data();
	if (m.weights.size() != m.pos.size()) {
		m.weights.resize(m.pos.size());
	}
	const bool norm = !dock || !dock->wp_normalize || dock->wp_normalize->is_pressed();
	const bool front = dock && dock->wp_front && dock->wp_front->is_pressed();
	const bool mirror = dock && dock->wp_mirror_btn && dock->wp_mirror_btn->is_pressed();
	const int ftype = dock && dock->wp_falloff ? dock->wp_falloff->get_selected() : 0;
	float radius = wp_radius, strength = wp_strength;
	const float pr = CLAMP(p_pressure, 0.0f, 1.0f);
	if (dock && dock->wp_pr_size && dock->wp_pr_size->is_pressed()) {
		radius *= MAX(0.05f, pr);
	}
	if (!dock || !dock->wp_pr_strength || dock->wp_pr_strength->is_pressed()) {
		strength *= pr;
	}
	const Transform3D g = n->get_global_transform();
	const Basis nb = g.basis.inverse().transposed();
	const Vector3 eye = p_cam->get_global_position();
	const int mb = mirror ? hw_bones::mirror_bone(s, wp_bone) : -1;
	std::vector<Vector3> vn;
	if (front) {
		vn.assign(m.pos.size(), Vector3());
		for (const hewn::Face &F : m.faces) {
			const Vector3 fnn = hewn::face_normal_raw(m, F);
			for (int v : F.v) {
				vn[v] += fnn;
			}
		}
	}
	std::vector<std::pair<int, float>> hits;
	for (int v = 0; v < (int)m.pos.size(); v++) {
		const Vector3 w = g.xform(m.pos[v]);
		if (p_cam->is_position_behind(w)) {
			continue;
		}
		const float d = p_cam->unproject_position(w).distance_to(p_pos);
		if (d >= radius) {
			continue;
		}
		if (front && nb.xform(vn[v]).dot(eye - w) <= 0.0f) {
			continue;
		}
		const float f = wp_falloff(ftype, d / radius) * strength;
		if (f > 0.0f) {
			hits.push_back({ v, f });
		}
	}
	if (hits.empty()) {
		return;
	}
	std::vector<float> now(m.pos.size(), -1.0f); // (smooth: the neighbors as they were before this dab)
	auto wt = [&](int v) { return hewn::skin::weight_of(m.weights[v], wp_bone); };
	if (wp_tool == 3) {
		for (const auto &h : hits) {
			now[h.first] = wt(h.first);
			for (int o : wp_nb[h.first]) {
				now[o] = wt(o);
			}
		}
	}
	for (const auto &h : hits) {
		const int v = h.first;
		const float f = h.second;
		const float w0 = wt(v);
		float w1 = w0;
		switch (wp_tool) {
			case 1:
				w1 = w0 + wp_weight * f;
				break;
			case 2:
				w1 = w0 - wp_weight * f;
				break;
			case 3: {
				float sum = 0.0f;
				int cnt = 0;
				for (int o : wp_nb[v]) {
					sum += now[o] >= 0.0f ? now[o] : wt(o);
					cnt++;
				}
				if (cnt) {
					w1 = w0 + (sum / cnt - w0) * f;
				}
			} break;
			default: {
				// draw: toward the weight, as strong as the stroke's strongest touch of it so far
				float &reach = wp_reach[v];
				if (f <= reach) {
					continue;
				}
				reach = f;
				w1 = w0 + (wp_weight - w0) * f;
			} break;
		}
		w1 = CLAMP(w1, 0.0f, 1.0f);
		hewn::skin::set_weight(m.weights[v], wp_bone, w1, norm);
		if (mirror && v < (int)wp_mirror.size() && wp_mirror[v] >= 0 && wp_mirror[v] != v) {
			hewn::skin::set_weight(m.weights[wp_mirror[v]], mb >= 0 ? mb : wp_bone, w1, norm);
		}
	}
	_wp_view();
}

void HewnEditorPlugin::_wp_stroke_end() {
	wp_painting = false;
	HewnMesh3D *n = wp_mesh();
	if (!n || wp_before.is_empty()) {
		return;
	}
	n->rebuild(); // (the mesh's bones and weights for the game)
	_wp_view();
	const Dictionary after = n->get_state();
	static const char *names[5] = { "Weight Paint: Draw", "Weight Paint: Add", "Weight Paint: Subtract", "Weight Paint: Smooth", "Weight Paint" };
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(names[CLAMP(wp_tool, 0, 4)], UndoRedo::MERGE_DISABLE, n);
	hw_undo_state(ur, true, n, after);
	hw_undo_state(ur, false, n, wp_before);
	ur->add_do_method(this, "_wp_view");
	ur->add_undo_method(this, "_wp_view");
	ur->commit_action(false);
	wp_before = Dictionary();
}

void HewnEditorPlugin::wp_menu(int p_id) {
	HewnMesh3D *n = wp_mesh();
	Skeleton3D *s = wp_skeleton();
	if (!n || !s) {
		return;
	}
	const Dictionary before = n->get_state();
	hewn::Mesh &m = n->get_data();
	if (m.weights.size() != m.pos.size()) {
		m.weights.resize(m.pos.size());
	}
	String name = "Weight Paint";
	switch (p_id) {
		case 0:
			for (auto &w : m.weights) {
				hewn::skin::normalize(w);
			}
			name = "Weight Paint: Normalize All";
			break;
		case 1:
			for (auto &w : m.weights) {
				hewn::skin::clean(w, 0.01f, 4);
			}
			name = "Weight Paint: Clean";
			break;
		case 2:
			hewn::skin::smooth_bone(m, wp_bone, 3, 0.5f);
			name = "Weight Paint: Smooth the Bone";
			break;
		case 3: {
			// the other side's vertices take this side's weights (the side of the active bone), bones mirrored
			const std::vector<int> mir = hewn::skin::mirror_map(m, 1e-3f);
			const float side = hw_bones::rest_global(s, wp_bone).origin.x >= 0.0f ? 1.0f : -1.0f;
			std::vector<hewn::skin::Weights> src = m.weights;
			for (int v = 0; v < (int)m.pos.size(); v++) {
				if (mir[v] < 0 || m.pos[v].x * side >= -1e-5f) {
					continue; // (this side's, or on the middle)
				}
				hewn::skin::Weights out;
				for (const auto &p : src[mir[v]]) {
					const int mbn = hw_bones::mirror_bone(s, p.first);
					out.push_back({ mbn >= 0 ? mbn : p.first, p.second });
				}
				m.weights[v] = out;
			}
			name = "Weight Paint: Mirror Weights";
		} break;
		case 4: {
			std::vector<hw_bones::BoneShape> bs;
			hw_bones::bone_shapes(s, bs, true);
			const Transform3D inv = n->get_global_transform().affine_inverse();
			std::vector<Vector3> heads, tails;
			for (const hw_bones::BoneShape &b : bs) {
				heads.push_back(inv.xform(b.head));
				tails.push_back(inv.xform(b.tail));
			}
			hewn::skin::auto_weights(m, heads, tails, std::vector<char>(heads.size(), 1), 0);
			name = "Weight Paint: Automatic Weights";
		} break;
		case 5:
			for (auto &w : m.weights) {
				hewn::skin::set_weight(w, wp_bone, 0.0f, true);
			}
			name = "Weight Paint: Clear the Bone";
			break;
		default:
			return;
	}
	n->rebuild();
	const Dictionary after = n->get_state();
	n->set_state(before);
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(name, UndoRedo::MERGE_DISABLE, n);
	hw_undo_state(ur, true, n, after);
	hw_undo_state(ur, false, n, before);
	ur->add_do_method(this, "_wp_view");
	ur->add_undo_method(this, "_wp_view");
	ur->commit_action();
}

// ------------------------------------------------------------------ input

int32_t HewnEditorPlugin::_wp_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	HewnMesh3D *n = wp_mesh();
	Skeleton3D *s = wp_skeleton();
	if (!n || !s) {
		exit_wpaint(false);
		return -1;
	}
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	auto sync = [&]() {
		if (dock) {
			dock->sync_wpaint_bar();
		}
		update_overlays();
	};
	if (k.is_valid()) {
		if (!k->is_pressed() || k->is_echo()) {
			return AFTER_GUI_INPUT_PASS;
		}
		const Key code = key_code(k);
		const bool ctrl = k->is_ctrl_pressed() || k->is_meta_pressed(), shift = k->is_shift_pressed(), alt = k->is_alt_pressed();
		if (code == KEY_TAB && !ctrl && !shift && !alt) {
			exit_wpaint();
			return AFTER_GUI_INPUT_STOP;
		}
		if ((code == KEY_BRACKETLEFT || code == KEY_BRACKETRIGHT) && !ctrl && !alt) {
			const float f = code == KEY_BRACKETRIGHT ? 1.15f : 1.0f / 1.15f;
			if (shift) {
				wp_strength = CLAMP(wp_strength * f, 0.01f, 1.0f);
			} else {
				wp_radius = CLAMP(wp_radius * f, 2.0f, 2000.0f);
			}
			sync();
			return AFTER_GUI_INPUT_STOP;
		}
		const bool plain = !ctrl && !shift && !alt;
		if (plain && (code == KEY_B || code == KEY_C || code == KEY_W || code == KEY_G || code == KEY_R || code == KEY_S || code == KEY_K || code == KEY_X || code == KEY_E ||
							 code == KEY_DELETE)) {
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (mb.is_valid() && mb->is_pressed() && (mb->is_ctrl_pressed() || mb->is_meta_pressed() || mb->is_shift_pressed()) && !mb->is_alt_pressed() &&
			(mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP || mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN)) {
		const float f = mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? 1.1f : 1.0f / 1.1f;
		if (mb->is_shift_pressed()) {
			wp_strength = CLAMP(wp_strength * f, 0.01f, 1.0f);
		} else {
			wp_radius = CLAMP(wp_radius * f, 2.0f, 2000.0f);
		}
		sync();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mm.is_valid()) {
		const float pr = mm->get_pressure();
		wp_pressure = pr > 0.0f ? pr : 1.0f;
	}
	if (wp_resizing) {
		if (mm.is_valid()) {
			const float dx = mm->get_position().x - wp_resize_at.x;
			if (wp_resize_strength) {
				wp_strength = CLAMP(wp_resize_s0 + dx / 300.0f, 0.01f, 1.0f);
			} else {
				wp_radius = CLAMP(wp_resize_r0 + dx, 2.0f, 2000.0f);
			}
			mouse_pos = wp_resize_at;
			sync();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && !mb->is_pressed()) {
			wp_resizing = false;
			Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
			if (Control *surface = _surface_for(p_cam)) {
				Input::get_singleton()->warp_mouse(surface->get_global_rect().position + wp_resize_at);
			}
			mouse_pos = wp_resize_at;
			update_overlays();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && mb->is_pressed() && !mb->is_alt_pressed()) {
		mouse_pos = mb->get_position();
		const bool ctrl = mb->is_ctrl_pressed() || mb->is_meta_pressed(), shift = mb->is_shift_pressed();
		if (shift && wp_tool != 4) {
			wp_resize_strength = ctrl;
			wp_resizing = true;
			wp_resize_at = mouse_pos;
			wp_resize_r0 = wp_radius;
			wp_resize_s0 = wp_strength;
			if (DisplayServer::get_singleton()->get_name() != "headless") {
				Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_HIDDEN);
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (ctrl || wp_tool == 4) {
			// Ctrl+click: the bone there active (or the strongest bone of the vertex there); Sample: its weight
			const hewn::Mesh &m = n->get_data();
			const Transform3D g = n->get_global_transform();
			int best = -1;
			float bd = 12.0f * EditorInterface::get_singleton()->get_editor_scale();
			for (int v = 0; v < (int)m.pos.size(); v++) {
				const Vector3 w = g.xform(m.pos[v]);
				if (p_cam->is_position_behind(w)) {
					continue;
				}
				const float d = p_cam->unproject_position(w).distance_to(mouse_pos);
				if (d < bd) {
					bd = d;
					best = v;
				}
			}
			if (wp_tool == 4 && !ctrl) {
				if (best >= 0 && best < (int)m.weights.size()) {
					wp_weight = hewn::skin::weight_of(m.weights[best], wp_bone);
					sync();
				}
				return AFTER_GUI_INPUT_STOP;
			}
			std::vector<hw_bones::BoneShape> bs;
			hw_bones::bone_shapes(s, bs, true);
			int b = hw_bones::pick_bone(p_cam, bs, std::set<int>(), mouse_pos);
			if (b < 0 && best >= 0 && best < (int)m.weights.size()) {
				float top = 0.0f;
				for (const auto &p : m.weights[best]) {
					if (p.second > top) {
						top = p.second;
						b = p.first;
					}
				}
			}
			if (b >= 0) {
				wp_set_bone(b);
			}
			return AFTER_GUI_INPUT_STOP;
		}
		const hewn::Mesh &m = n->get_data();
		wp_before = n->get_state();
		wp_painting = true;
		wp_nb = hewn::skin::neighbors(m);
		wp_reach.assign(m.pos.size(), 0.0f);
		wp_mirror = dock && dock->wp_mirror_btn && dock->wp_mirror_btn->is_pressed() ? hewn::skin::mirror_map(m, 1e-3f) : std::vector<int>();
		wp_last = mouse_pos;
		wp_left = 0.0f;
		_wp_dab(p_cam, mouse_pos, 1.0f);
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && !mb->is_pressed()) {
		if (wp_painting) {
			_wp_stroke_end();
			update_overlays();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (mm.is_valid()) {
		mouse_pos = mm->get_position();
		if (wp_painting) {
			const float step = MAX(1.0f, wp_radius * 0.2f);
			Vector2 d = mouse_pos - wp_last;
			float len = d.length();
			if (len > 1e-4f) {
				const Vector2 dir = d / len;
				Vector2 at = wp_last;
				float left = wp_left > 0.0f ? wp_left : step;
				while (len >= left) {
					at += dir * left;
					len -= left;
					_wp_dab(p_cam, at, wp_pressure);
					left = step;
				}
				wp_left = left - len;
				wp_last = mouse_pos;
			}
		}
		update_overlays();
		return wp_painting ? AFTER_GUI_INPUT_STOP : AFTER_GUI_INPUT_PASS;
	}
	return AFTER_GUI_INPUT_PASS;
}

// ------------------------------------------------------------------ drawing

void HewnEditorPlugin::_draw_wpaint(Control *p_overlay, Camera3D *p_cam) {
	Skeleton3D *s = wp_skeleton();
	if (!s) {
		return;
	}
	std::vector<hw_bones::BoneShape> bs;
	hw_bones::bone_shapes(s, bs, true);
	std::vector<char> sel(bs.size(), 0);
	if (wp_bone >= 0 && wp_bone < (int)bs.size()) {
		sel[wp_bone] = 1;
	}
	hw_bones::draw_bones(p_overlay, p_cam, s, bs, sel, wp_bone, -1, std::set<int>(), false, 0.55f);
	const Vector2 at = wp_resizing ? wp_resize_at : mouse_pos;
	if (wp_tool != 4) {
		p_overlay->draw_arc(at, wp_radius + 1.0f, 0.0f, (float)Math::TAU, 64, Color(0, 0, 0, 0.6f), 1.0f, true);
		p_overlay->draw_arc(at, wp_radius, 0.0f, (float)Math::TAU, 64, Color(1, 1, 1, 0.9f), 1.0f, true);
		const float inner = wp_radius * wp_strength;
		if (inner > 2.0f && inner < wp_radius - 1.0f) {
			p_overlay->draw_arc(at, inner, 0.0f, (float)Math::TAU, 48, Color(1, 1, 1, 0.35f), 1.0f, true);
		}
	}
	String head = T("Weight Paint") + ": " + (wp_bone >= 0 && wp_bone < s->get_bone_count() ? String(s->get_bone_name(wp_bone)) : String("-"));
	head += vformat("  (%.2f)", wp_weight);
	draw_header(p_overlay, p_overlay->get_theme_default_font(), head);
}

// ------------------------------------------------------------------ the header's second row

void HewnDock::build_wpaint_bar() {
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
		wp_blocks.push_back(p);
		return h;
	};
	auto label = [&](Container *p_box, const String &p_text) {
		Label *l = memnew(Label);
		l->set_text(p_text);
		l->add_theme_color_override("font_color", Color(0.75f, 0.75f, 0.75f));
		p_box->add_child(l);
	};
	auto field = [&](Container *p_box, double p_min, double p_max, double p_step, double p_value, const String &p_name, bool p_int) {
		HewnDragField *f = memnew(HewnDragField);
		f->setup(p_min, p_max, p_step, p_int, p_value);
		f->set_custom_minimum_size(Vector2(52 * es, 0));
		f->set_name(p_name);
		f->connect("value_changed", callable_mp(this, &HewnDock::_on_wp_value));
		p_box->add_child(f);
		return f;
	};
	auto check = [&](Container *p_box, const String &p_text, bool p_on, const String &p_name) {
		CheckBox *c = memnew(CheckBox);
		c->set_text(p_text);
		c->set_pressed(p_on);
		c->set_name(p_name);
		c->connect("toggled", callable_mp(this, &HewnDock::_on_wp_changed).unbind(1));
		p_box->add_child(c);
		return c;
	};
	HBoxContainer *bb = block();
	label(bb, T("Bone"));
	wp_bone_opt = memnew(OptionButton);
	wp_bone_opt->set_name("WPBone");
	wp_bone_opt->set_flat(true);
	wp_bone_opt->set_fit_to_longest_item(false);
	wp_bone_opt->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);
	wp_bone_opt->set_tooltip_text("The bone painted (Ctrl+click on a bone or the mesh: that one)");
	wp_bone_opt->connect("item_selected", callable_mp(this, &HewnDock::_on_wp_bone));
	bb->add_child(wp_bone_opt);
	HBoxContainer *wb = block();
	label(wb, T("Weight"));
	wp_weight_field = field(wb, 0, 1, 0.01, 1.0, "WPWeight", false);
	label(wb, T("Radius"));
	wp_radius_field = field(wb, 2, 2000, 1, 40, "WPRadius", true);
	label(wb, T("Strength"));
	wp_strength_field = field(wb, 0.01, 1, 0.01, 0.5, "WPStrength", false);
	HBoxContainer *ob = block();
	PopupPanel *pp = memnew(PopupPanel);
	VBoxContainer *ov = memnew(VBoxContainer);
	pp->add_child(ov);
	paint_row->add_child(pp);
	Button *obtn = memnew(Button);
	obtn->set_text(T("Options") + String(" ") + String::chr(0x25BE));
	obtn->set_flat(true);
	obtn->set_name("WPOptions");
	obtn->connect("pressed", callable_mp(this, &HewnDock::_popup_under).bind(pp, obtn));
	ob->add_child(obtn);
	popup_opener(obtn, pp);
	wp_normalize = check(ov, T("Auto normalize (the other bones make room)"), true, "WPNormalize");
	wp_front = check(ov, T("Only the faces turned to the view"), false, "WPFront");
	label(ov, T("Falloff"));
	wp_falloff = memnew(OptionButton);
	for (const char *it : { "Smooth", "Sphere", "Root", "Sharp", "Linear", "Constant" }) {
		wp_falloff->add_item(it);
	}
	ov->add_child(wp_falloff);
	label(ov, T("Pen pressure"));
	wp_pr_size = check(ov, T("Size"), false, "WPPrSize");
	wp_pr_strength = check(ov, T("Strength"), true, "WPPrStrength");
	HBoxContainer *mbk = block();
	wp_mirror_btn = memnew(Button);
	wp_mirror_btn->set_flat(true);
	wp_mirror_btn->set_toggle_mode(true);
	wp_mirror_btn->set_button_icon(icon("pose_mirror"));
	wp_mirror_btn->set_name("WPMirror");
	wp_mirror_btn->set_tooltip_text("X Mirror: the vertex on the other side gets the same weight of the other side's bone");
	wp_mirror_btn->set_theme_type_variation("FlatMenuButton");
	mbk->add_child(wp_mirror_btn);
	wp_menu_btn = memnew(MenuButton);
	wp_menu_btn->set_name("WPMenu");
	wp_menu_btn->set_text(T("Weights") + String(" ") + String::chr(0x25BE));
	wp_menu_btn->set_flat(true);
	PopupMenu *pm = wp_menu_btn->get_popup();
	pm->add_item("Normalize All", 0);
	pm->add_item("Clean (under 0.01, at most 4 bones)", 1);
	pm->add_item("Smooth the Bone", 2);
	pm->add_item("Clear the Bone", 5);
	pm->add_separator();
	pm->add_item("Mirror Weights to the Other Side", 3);
	pm->add_item("Automatic Weights Again", 4);
	pm->connect("id_pressed", callable_mp(this, &HewnDock::_on_wp_menu));
	mbk->add_child(wp_menu_btn);
}

void HewnDock::sync_wpaint_bar() {
	const bool on = plugin && plugin->wpaint && paint_bar_shown && !toolbar_folded;
	for (Control *c : wp_blocks) {
		c->set_visible(on);
	}
	if (!plugin || !on) {
		return;
	}
	Skeleton3D *s = plugin->wp_skeleton();
	if (wp_bone_opt && s) {
		if (wp_bone_opt->get_item_count() != s->get_bone_count() || (s->get_bone_count() > 0 && wp_bone_opt->get_item_text(0) != s->get_bone_name(0))) {
			wp_bone_opt->clear();
			for (int b = 0; b < s->get_bone_count(); b++) {
				wp_bone_opt->add_item(s->get_bone_name(b), b);
			}
		}
		if (plugin->wp_bone >= 0 && plugin->wp_bone < wp_bone_opt->get_item_count()) {
			wp_bone_opt->select(plugin->wp_bone);
		}
	}
	if (wp_weight_field) {
		wp_weight_field->set_value_no_signal(plugin->wp_weight);
		wp_radius_field->set_value_no_signal(plugin->wp_radius);
		wp_strength_field->set_value_no_signal(plugin->wp_strength);
	}
}

void HewnDock::_on_wp_bone(int p_index) {
	if (plugin) {
		plugin->wp_set_bone(p_index);
	}
}

void HewnDock::_on_wp_value(double p_unused) {
	if (plugin && wp_weight_field) {
		plugin->wp_weight = (float)wp_weight_field->get_value();
		plugin->wp_radius = (float)wp_radius_field->get_value();
		plugin->wp_strength = (float)wp_strength_field->get_value();
		plugin->update_overlays();
	}
}

void HewnDock::_on_wp_menu(int p_id) {
	if (plugin) {
		plugin->wp_menu(p_id);
	}
}

void HewnDock::_on_wp_changed() {
	if (plugin) {
		plugin->update_overlays();
	}
}
