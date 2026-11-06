// Spin tool: a ring around the 3D cursor across the chosen axis; dragging it spins the
// selection (extruded step by step around the axis) by the dragged angle. The steps, the angle and the
// rest stay in the panel of the last operation. X / Y / Z / V: the axis (V: the view).
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>

using namespace godot;
using namespace hw_editor;

namespace {
const float RING_PX = 70.0f;
} // namespace

void HewnEditorPlugin::start_spin_tool() {
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	if (!get_edited()) {
		return;
	}
	if (mode == MODE_OBJECT) {
		set_mode(element_mode);
	}
	modal = MODAL_SPIN;
	spin_dragging = false;
	spin_hover = false;
	update_overlays();
}

Vector3 HewnEditorPlugin::_spin_axis(Camera3D *p_cam) const {
	if (spin_axis < 3) {
		return AXES[spin_axis];
	}
	return p_cam ? p_cam->get_global_transform().basis.get_column(2).normalized() : Vector3(0, 1, 0);
}

// The angle of the mouse around the axis (radians, counter-clockwise looking against the axis).
bool HewnEditorPlugin::_spin_mouse_angle(Camera3D *p_cam, const Vector2 &p_pos, float &r_angle) const {
	const Vector3 ax = _spin_axis(p_cam);
	Vector3 u, v;
	hewn::plane_basis(ax, u, v);
	v = ax.cross(u).normalized();
	Vector3 hit;
	if (_plane_hit(p_cam, p_pos, cursor_pos, ax, hit) && (hit - cursor_pos).length() > 1e-6f) {
		const Vector3 d = hit - cursor_pos;
		r_angle = std::atan2(d.dot(v), d.dot(u));
		return true;
	}
	return false;
}

int32_t HewnEditorPlugin::_spin_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		cancel_tool();
		return AFTER_GUI_INPUT_PASS;
	}
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	if (k.is_valid() && k->is_pressed() && !k->is_echo() && !k->is_ctrl_pressed() && !k->is_alt_pressed()) {
		const Key code = key_code(k);
		if (code == KEY_ESCAPE) {
			cancel_tool();
			return AFTER_GUI_INPUT_STOP;
		}
		if (!spin_dragging && (code == KEY_X || code == KEY_Y || code == KEY_Z || code == KEY_V)) {
			spin_axis = code == KEY_X ? 0 : code == KEY_Y ? 1 : code == KEY_Z ? 2 : 3;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}
	const Vector2 cs = p_cam->is_position_behind(cursor_pos) ? Vector2(-1e6f, -1e6f) : p_cam->unproject_position(cursor_pos);
	const float ring = RING_PX * EditorInterface::get_singleton()->get_editor_scale();
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_RIGHT && mb->is_pressed() && !spin_dragging) {
		cancel_tool();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		if (mb->is_pressed()) {
			// on the ring (the projected circle is about as far as the ring's size on screen): a spin
			float a = 0.0f;
			const float d = mb->get_position().distance_to(cs);
			if (d > ring * 0.4f && d < ring * 1.6f && _spin_mouse_angle(p_cam, mb->get_position(), a)) {
				spin_dragging = true;
				spin_started = false;
				spin_angle = 0.0f;
				spin_prev = a;
				spin_from = a;
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			return AFTER_GUI_INPUT_PASS; // (selection clicks go on)
		}
		if (spin_dragging) {
			spin_dragging = false;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (mm.is_valid()) {
		if (!spin_dragging) {
			const float d = mm->get_position().distance_to(cs);
			const bool hover = d > ring * 0.4f && d < ring * 1.6f;
			if (hover != spin_hover) {
				spin_hover = hover;
				update_overlays();
			}
			return AFTER_GUI_INPUT_PASS;
		}
		float a = 0.0f;
		if (!_spin_mouse_angle(p_cam, mm->get_position(), a)) {
			return AFTER_GUI_INPUT_STOP;
		}
		const float delta = Math::wrapf(a - spin_prev, (float)-Math::PI, (float)Math::PI);
		spin_prev = a;
		spin_angle += Math::rad_to_deg(delta);
		float shown = spin_angle;
		if (snapping_active(mm->is_ctrl_pressed() || mm->is_meta_pressed()) && dock) {
			const float st = MAX(0.1f, (float)dock->snap_rotate->get_value());
			shown = std::round(shown / st) * st;
		}
		if (!spin_started && std::fabs(shown) >= 1.0f) {
			const Vector3 ax = _spin_axis(p_cam);
			set_op_param("spin", "cx", cursor_pos.x);
			set_op_param("spin", "cy", cursor_pos.y);
			set_op_param("spin", "cz", cursor_pos.z);
			set_op_param("spin", "ax", ax.x);
			set_op_param("spin", "ay", ax.y);
			set_op_param("spin", "az", ax.z);
			set_op_param("spin", "angle", shown);
			run_op("spin");
			spin_started = true;
			modal = MODAL_SPIN; // (the operation does not end the tool)
		} else if (spin_started && std::fabs(shown - (float)op_param("spin", "angle")) > 1e-3f) {
			set_op_param("spin", "angle", shown);
			_redo_last();
		}
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	return AFTER_GUI_INPUT_PASS;
}

void HewnEditorPlugin::_draw_spin(Control *p_overlay, Camera3D *p_cam) {
	Ref<Font> font = p_overlay->get_theme_default_font();
	const Vector3 ax = _spin_axis(p_cam);
	Vector3 u, v;
	hewn::plane_basis(ax, u, v);
	v = ax.cross(u).normalized();
	const float r = RING_PX * EditorInterface::get_singleton()->get_editor_scale() * _world_per_pixel(p_cam, cursor_pos);
	const Color col = spin_axis < 3 ? AXIS_COLORS[spin_axis] : Color(0.9f, 0.9f, 0.9f);
	auto at = [&](float a) { return cursor_pos + (u * std::cos(a) + v * std::sin(a)) * r; };
	PackedVector2Array ring;
	bool ok = true;
	for (int i = 0; i <= 64 && ok; i++) {
		const Vector3 p = at((float)Math::PI * 2.0f * i / 64);
		ok = !p_cam->is_position_behind(p);
		ring.push_back(p_cam->unproject_position(p));
	}
	if (ok) {
		p_overlay->draw_polyline(ring, col * Color(1, 1, 1, spin_hover || spin_dragging ? 1.0f : 0.7f), spin_hover || spin_dragging ? 3.0f : 2.0f, true);
	}
	if (spin_dragging && ok) {
		// the swept part and its two ends (round handles)
		const float a0 = spin_from, a1 = spin_from + Math::deg_to_rad((float)op_param("spin", "angle")) * (spin_started ? 1.0f : 0.0f);
		PackedVector2Array arc;
		const int seg = MAX(2, (int)(std::fabs(a1 - a0) / 0.05f));
		for (int i = 0; i <= seg; i++) {
			arc.push_back(p_cam->unproject_position(at(a0 + (a1 - a0) * i / seg)));
		}
		p_overlay->draw_polyline(arc, TOOL_COLOR, 5.0f, true);
		p_overlay->draw_circle(p_cam->unproject_position(at(a0)), 6.0f, Color(1, 1, 1));
		p_overlay->draw_circle(p_cam->unproject_position(at(a1)), 7.0f, TOOL_COLOR);
	} else if (ok) {
		p_overlay->draw_circle(p_cam->unproject_position(at(0.0f)), 6.0f, col);
		p_overlay->draw_circle(p_cam->unproject_position(at((float)Math::PI)), 6.0f, col);
	}
	static const char *names[4] = { "X", "Y", "Z", "View" };
	String text = vformat(T("Spin around %s: drag the ring (the 3D cursor is the center)"), T(names[spin_axis]));
	if (spin_dragging && spin_started) {
		text += vformat("   %.1f°", (float)op_param("spin", "angle"));
	}
	text += T("   |   X / Y / Z / V: axis, Ctrl: snapping the other way, RMB / Esc: done");
	draw_header(p_overlay, font, text);
}
