// Texture tools in the viewport: UV steps, cycle / mirror / justify /
// fit / inset, extend from the active face, square fit unwrap, a UV rect clipboard, and G / R / S for the
// texture where the texture follows the mouse on the face.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/spin_box.hpp>

using namespace godot;
using namespace hw_editor;

bool HewnEditorPlugin::_uv_targets(std::vector<HewnMesh3D *> &r_targets, PackedInt32Array &r_faces) const {
	r_targets.clear();
	r_faces.clear();
	HewnMesh3D *edited = get_edited();
	if (edited && mode != MODE_OBJECT) {
		const std::vector<int> fs = mode == MODE_FACE ? edited->selection.faces : hewn::sel::faces_from_verts(edited->get_data(), _selected_verts(), true);
		for (int f : fs) {
			r_faces.push_back(f);
		}
		if (r_faces.is_empty()) {
			return false;
		}
		r_targets.push_back(edited);
		return true;
	}
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	for (int64_t i = 0; i < sel.size(); i++) {
		if (HewnMesh3D *n = Object::cast_to<HewnMesh3D>(sel[i])) {
			r_targets.push_back(n);
		}
	}
	if (r_targets.empty() && edited) {
		r_targets.push_back(edited);
	}
	return !r_targets.empty();
}

void HewnEditorPlugin::run_uv_command(const String &p_command) {
	const PackedStringArray parts = p_command.split(":");
	const String op = parts.size() > 0 ? parts[0] : String();
	auto num = [&](int i, double def) { return i < parts.size() ? parts[i].to_float() : def; };
	if (op == "modal") {
		start_uv_modal((int)num(1, 0));
		return;
	}
	std::vector<HewnMesh3D *> targets;
	PackedInt32Array faces;
	if (!_uv_targets(targets, faces)) {
		return;
	}
	if (op == "grab") {
		// (the first target: the rect is in its tiles)
		uv_clip = targets[0]->get_uv_bounds(faces);
		if (dock && dock->uv_rect_label) {
			dock->uv_rect_label->set_text(vformat("%.3f, %.3f  -  %.3f, %.3f", uv_clip.position.x, uv_clip.position.y, uv_clip.get_end().x, uv_clip.get_end().y));
		}
		return;
	}
	const double step_m = dock && dock->uv_step_move ? dock->uv_step_move->get_value() : 0.125;
	const double step_r = dock && dock->uv_step_rotate ? dock->uv_step_rotate->get_value() : 90.0;
	const double step_s = dock && dock->uv_step_scale ? MAX(1.0001, dock->uv_step_scale->get_value()) : 2.0;
	String edit_op = op;
	Array args;
	if (op == "step_move") {
		edit_op = "move";
		args.push_back(num(1, 0) * step_m);
		args.push_back(num(2, 0) * step_m);
	} else if (op == "step_rotate") {
		edit_op = "rotate";
		args.push_back(num(1, 1) * step_r);
	} else if (op == "step_scale") {
		// +: the texture bigger (the UVs smaller), along U, V or both
		const int axis = (int)num(1, 0);
		const double f = num(2, 1) > 0 ? 1.0 / step_s : step_s;
		edit_op = "scale";
		args.push_back(axis == 2 ? 1.0 : f);
		args.push_back(axis == 1 ? 1.0 : f);
	} else if (op == "inset") {
		args.push_back(num(1, 1) * (dock && dock->uv_inset_px ? dock->uv_inset_px->get_value() : 8.0));
	} else if (op == "transfer") {
		args.push_back(uv_clip.position.x);
		args.push_back(uv_clip.position.y);
		args.push_back(uv_clip.get_end().x);
		args.push_back(uv_clip.get_end().y);
	} else if (op != "extend" && op != "unwrap") {
		for (int i = 1; i < parts.size(); i++) {
			args.push_back(parts[i].to_float());
		}
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: UV " + op.capitalize(), UndoRedo::MERGE_DISABLE, targets[0]);
	for (HewnMesh3D *n : targets) {
		const Dictionary before = n->get_state();
		if (op == "extend") {
			// Extend / Stitch: from the active (last selected) face over the selected ones,
			// across the shared edges, so the texture runs on without a seam
			const hewn::Mesh &m = n->get_data();
			std::vector<int> fs;
			for (int64_t i = 0; i < faces.size(); i++) {
				fs.push_back(faces[i]);
			}
			if (fs.size() < 2) {
				continue;
			}
			std::vector<char> in(m.faces.size(), 0), done(m.faces.size(), 0);
			for (int f : fs) {
				in[f] = 1;
			}
			hewn::Topology t;
			t.build(m);
			std::vector<int> queue = { fs.back() };
			done[fs.back()] = 1;
			for (size_t qi = 0; qi < queue.size(); qi++) {
				const int f = queue[qi];
				const hewn::Face &F = m.faces[f];
				for (int k = 0; k < F.size(); k++) {
					const int g = t.face_of_half(F.v[(k + 1) % F.size()], F.v[k]); // the neighbor across the edge
					if (g >= 0 && in[g] && !done[g]) {
						done[g] = 1;
						n->wrap_texture(f, g, true);
						queue.push_back(g);
					}
				}
			}
		} else if (op == "unwrap") {
			const int start = mode == MODE_FACE && !n->selection.faces.empty() ? n->selection.faces.back() : -1;
			n->unwrap_quads(faces, start);
		} else {
			n->edit_uvs(faces, edit_op, args);
		}
		ur->add_do_method(n, "set_state", n->get_state());
		ur->add_undo_method(n, "set_state", before);
	}
	ur->commit_action(false);
	if (dock) {
		dock->refresh();
	}
	update_overlays();
}

// ------------------------------------------------------------------ G / R / S of the texture

void HewnEditorPlugin::start_uv_modal(int p_kind) {
	HewnMesh3D *n = get_edited();
	Camera3D *cam = _viewport_camera();
	std::vector<HewnMesh3D *> targets;
	PackedInt32Array faces;
	if (!n || mode == MODE_OBJECT || !cam || !_uv_targets(targets, faces)) {
		return;
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	const hewn::Mesh &m = n->get_data();
	const Transform3D g = n->get_global_transform();
	// the reference face: the one under the mouse if it is selected, else the active one
	int ref = faces[faces.size() - 1];
	for (int64_t i = 0; i < faces.size(); i++) {
		if (faces[i] == hover_face) {
			ref = hover_face;
		}
	}
	const hewn::Face &F = m.faces[ref];
	std::vector<std::vector<Vector2>> uvs;
	hewn::compute_uvs(m, g, uvs);
	Vector2 ms(1, 1);
	Ref<BaseMaterial3D> bm = n->get_material_for_slot(F.material);
	if (bm.is_valid()) {
		ms = Vector2(MAX(1e-6f, bm->get_uv1_scale().x), MAX(1e-6f, bm->get_uv1_scale().y));
	}
	uvm_n = g.basis.xform(hewn::face_normal(m, F)).normalized();
	uvm_p = g.xform(hewn::face_center(m, F));
	hewn::plane_basis(uvm_n, uvm_pu, uvm_pv);
	// tiles of the face as an affine function of the plane coordinates (least squares)
	const int c = F.size();
	std::vector<Vector2> xs(c), us(c);
	Vector2 xc, uc;
	for (int i = 0; i < c; i++) {
		const Vector3 p = g.xform(m.pos[F.v[i]]);
		xs[i] = Vector2(p.dot(uvm_pu), p.dot(uvm_pv));
		us[i] = uvs[ref][i] * ms;
		xc += xs[i];
		uc += us[i];
	}
	xc /= (float)c;
	uc /= (float)c;
	double a00 = 0, a01 = 0, a11 = 0, b00 = 0, b01 = 0, b10 = 0, b11 = 0;
	for (int i = 0; i < c; i++) {
		const Vector2 x = xs[i] - xc, u = us[i] - uc;
		a00 += x.x * x.x;
		a01 += x.x * x.y;
		a11 += x.y * x.y;
		b00 += u.x * x.x;
		b01 += u.x * x.y;
		b10 += u.y * x.x;
		b11 += u.y * x.y;
	}
	const double det = a00 * a11 - a01 * a01;
	if (std::fabs(det) < 1e-14) {
		return;
	}
	const double i00 = a11 / det, i01 = -a01 / det, i11 = a00 / det;
	uvm_row0 = Vector2((float)(b00 * i00 + b01 * i01), (float)(b00 * i01 + b01 * i11));
	uvm_row1 = Vector2((float)(b10 * i00 + b11 * i01), (float)(b10 * i01 + b11 * i11));
	uvm_t = uc - Vector2(uvm_row0.dot(xc), uvm_row1.dot(xc));
	if (!_uv_at(cam, mouse_pos, uvm_uv0)) {
		return;
	}
	uvm_faces = faces;
	uvm_before = n->get_state();
	uvm_backup = m;
	uvm_center = n->get_uv_bounds(faces).get_center();
	uvm_center3 = Vector3();
	for (int64_t i = 0; i < faces.size(); i++) {
		uvm_center3 += g.xform(hewn::face_center(m, m.faces[faces[i]]));
	}
	uvm_center3 /= (float)faces.size();
	uvm_kind = CLAMP(p_kind, 0, 2);
	uvm_axis = -1;
	_reset_precise();
	modal = MODAL_UV;
	drag = DRAG_NONE;
	_uv_modal_update(cam, false);
}

bool HewnEditorPlugin::_uv_at(Camera3D *p_cam, const Vector2 &p_pos, Vector2 &r_uv) const {
	Vector3 o, d;
	if (!p_cam || !_ray(p_cam, p_pos, o, d)) {
		return false;
	}
	const float den = d.dot(uvm_n);
	if (std::fabs(den) < 1e-6f) {
		return false;
	}
	const Vector3 p = o + d * ((uvm_p - o).dot(uvm_n) / den);
	const Vector2 x(p.dot(uvm_pu), p.dot(uvm_pv));
	r_uv = Vector2(uvm_row0.dot(x), uvm_row1.dot(x)) + uvm_t;
	return true;
}

void HewnEditorPlugin::_uv_modal_update(Camera3D *p_cam, bool p_ctrl) {
	HewnMesh3D *n = get_edited();
	Vector2 cur;
	if (!n || !_uv_at(p_cam, mouse_pos, cur)) {
		return;
	}
	n->get_data() = uvm_backup;
	const Vector2 c = uvm_center;
	static const char *axes[3] = { "", "  U", "  V" };
	Array args;
	if (uvm_kind == 0) {
		// the texture point under the mouse stays under it
		Vector2 d = uvm_uv0 - cur;
		if (uvm_axis == 0) {
			d.y = 0.0f;
		} else if (uvm_axis == 1) {
			d.x = 0.0f;
		}
		const float st = dock && dock->uv_step_move ? (float)dock->uv_step_move->get_value() : 0.125f;
		if (p_ctrl && st > 0.0f) {
			d = (d / st).round() * st;
		}
		args.push_back(d.x);
		args.push_back(d.y);
		n->edit_uvs(uvm_faces, "move", args);
		uvm_label = vformat(T("UV Move%s  %.4f, %.4f tiles"), axes[uvm_axis + 1], d.x, d.y);
	} else if (uvm_kind == 1) {
		float deg = ((uvm_uv0 - c).angle() - (cur - c).angle()) * (float)(180.0 / Math::PI);
		deg = Math::fposmod(deg + 180.0f, 360.0f) - 180.0f;
		const float st = dock && dock->uv_step_rotate ? MIN(15.0f, (float)dock->uv_step_rotate->get_value()) : 15.0f;
		if (p_ctrl && st > 0.0f) {
			deg = std::round(deg / st) * st;
		}
		args.push_back(deg);
		args.push_back(c.x);
		args.push_back(c.y);
		n->edit_uvs(uvm_faces, "rotate", args);
		uvm_label = vformat(T("UV Rotate  %.2f deg"), deg);
	} else {
		const Vector2 a = uvm_uv0 - c, b = cur - c;
		float s = b.length() > 1e-6f ? a.length() / b.length() : 1.0f;
		Vector2 sv(s, s);
		if (uvm_axis == 0) {
			sv = Vector2(std::fabs(b.x) > 1e-6f ? a.x / b.x : 1.0f, 1.0f);
		} else if (uvm_axis == 1) {
			sv = Vector2(1.0f, std::fabs(b.y) > 1e-6f ? a.y / b.y : 1.0f);
		}
		if (p_ctrl) {
			sv = (sv * 10.0f).round() / 10.0f;
		}
		args.push_back(sv.x);
		args.push_back(sv.y);
		args.push_back(c.x);
		args.push_back(c.y);
		n->edit_uvs(uvm_faces, "scale", args);
		uvm_label = vformat(T("UV Scale%s  %.3f, %.3f"), axes[uvm_axis + 1], sv.x, sv.y);
	}
	uvm_label += T("   (X / Y: axis, Ctrl: steps, Shift: precise, G / R / S: switch)");
	if (uv_canvas) {
		uv_canvas->queue_redraw();
	}
	update_overlays();
}

int32_t HewnEditorPlugin::_uv_modal_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	HewnMesh3D *n = get_edited();
	auto finish = [&](bool p_apply) {
		modal = MODAL_NONE;
		if (!n) {
			return;
		}
		if (p_apply) {
			static const char *names[3] = { "Move", "Rotate", "Scale" };
			EditorUndoRedoManager *ur = get_undo_redo();
			ur->create_action(String("Hewn: UV ") + names[uvm_kind], UndoRedo::MERGE_DISABLE, n);
			ur->add_do_method(n, "set_state", n->get_state());
			ur->add_undo_method(n, "set_state", uvm_before);
			ur->commit_action(false);
		} else {
			n->get_data() = uvm_backup;
			n->rebuild();
		}
		uvm_backup = hewn::Mesh();
		selection_changed();
	};
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		mouse_pos = _precise_mouse(mm->get_position(), mm->is_shift_pressed());
		_uv_modal_update(p_cam, mm->is_ctrl_pressed() || mm->is_meta_pressed());
		return AFTER_GUI_INPUT_STOP;
	}
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid()) {
		if (!mb->is_pressed()) {
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb->get_button_index() == MOUSE_BUTTON_LEFT) {
			finish(true);
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb->get_button_index() == MOUSE_BUTTON_RIGHT) {
			finish(false);
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS; // (the wheel zooms)
	}
	Ref<InputEventKey> k = p_event;
	if (k.is_null() || !k->is_pressed() || k->is_echo()) {
		if (k.is_valid() && !k->is_echo()) {
			_uv_modal_update(p_cam, k->is_ctrl_pressed() && key_code(k) != KEY_CTRL); // (Ctrl released)
		}
		return k.is_valid() ? AFTER_GUI_INPUT_STOP : AFTER_GUI_INPUT_PASS;
	}
	const Key code = key_code(k);
	if (code == KEY_ESCAPE) {
		finish(false);
	} else if (code == KEY_ENTER || code == KEY_KP_ENTER || code == KEY_SPACE) {
		finish(true);
	} else if (code == KEY_X || code == KEY_Y) {
		const int a = code == KEY_X ? 0 : 1;
		uvm_axis = uvm_axis == a ? -1 : a;
		_uv_modal_update(p_cam, k->is_ctrl_pressed());
	} else if (code == KEY_G || code == KEY_R || code == KEY_S) {
		uvm_kind = code == KEY_G ? 0 : code == KEY_R ? 1 : 2;
		if (n) {
			n->get_data() = uvm_backup; // start over from here
		}
		_uv_at(p_cam, mouse_pos, uvm_uv0);
		_uv_modal_update(p_cam, k->is_ctrl_pressed());
	} else if (code == KEY_CTRL || code == KEY_META) {
		_uv_modal_update(p_cam, true);
	}
	return AFTER_GUI_INPUT_STOP;
}

void HewnEditorPlugin::_draw_uv_modal(Control *p_overlay, Camera3D *p_cam) {
	if (uvm_kind != 0 && !p_cam->is_position_behind(uvm_center3)) {
		const Vector2 sp = p_cam->unproject_position(uvm_center3);
		p_overlay->draw_dashed_line(sp, mouse_pos, Color(0, 0, 0, 0.9f), 1.5f, 5.0f);
		p_overlay->draw_circle(sp, 3.0f, Color(0, 0, 0, 0.9f));
	}
	Ref<Font> font = p_overlay->get_theme_default_font();
	if (font.is_valid()) {
		draw_header(p_overlay, font, uvm_label);
	}
}

// U in edit mode: the texture tools under the mouse.
void HewnEditorPlugin::_uv_menu(Camera3D *p_cam) {
	_show_menu(p_cam, "UV",
			{ { "Move Texture", "uv:modal:0" }, { "Rotate Texture", "uv:modal:1" }, { "Scale Texture", "uv:modal:2" }, { "", "" },
					{ "Rotate 90", "uv:rotate:90" }, { "Cycle (turn inside the bounds)", "uv:cycle" }, { "Mirror U", "uv:mirror_u" }, { "Mirror V", "uv:mirror_v" },
					{ "", "" }, { "Fit to Face", "uv:fit:1:1" }, { "Fit U", "uv:fit:1:0" }, { "Fit V", "uv:fit:0:1" }, { "Center", "uv:justify:0:0" }, { "", "" },
					{ "Extend from Active Face", "uv:extend" }, { "Square Fit Unwrap", "uv:unwrap" }, { "", "" }, { "Copy UV Rect", "uv:grab" },
					{ "Paste UV Rect", "uv:transfer" } });
}
