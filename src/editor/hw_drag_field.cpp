// Number field of the Last Operation panel: drag it left / right to change the value (the
// cursor is hidden while dragging and comes back where it was), click to type a value or an expression.
#include "hw_editor.h"

#include <godot_cpp/classes/expression.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/viewport.hpp>

using namespace godot;

void HewnDragField::_bind_methods() {
	ADD_SIGNAL(MethodInfo("value_changed", PropertyInfo(Variant::FLOAT, "value")));
}

int HewnDragField::drag_count = 0;

HewnDragField::~HewnDragField() {
	if (dragging) {
		drag_count = MAX(0, drag_count - 1);
	}
}

HewnDragField::HewnDragField() {
	set_custom_minimum_size(Vector2(100, 0));
	set_select_all_on_focus(true);
	set_default_cursor_shape(CURSOR_HSIZE);
	connect("text_submitted", callable_mp(this, &HewnDragField::_on_submitted));
	connect("focus_exited", callable_mp(this, &HewnDragField::_on_focus_exited));
}

void HewnDragField::setup(double p_min, double p_max, double p_step, bool p_int, double p_value) {
	min_value = p_min;
	max_value = p_max;
	step = p_step;
	is_int = p_int;
	value = p_value;
	_show();
}

void HewnDragField::_show() {
	set_text(is_int ? String::num_int64((int64_t)std::llround(value)) : String::num(value, 4));
}

void HewnDragField::_assign(double p_value, bool p_emit) {
	double v = CLAMP(p_value, min_value, max_value);
	if (is_int) {
		v = (double)std::llround(v);
	}
	const bool changed = v != value;
	value = v;
	_show();
	if (changed && p_emit) {
		emit_signal("value_changed", value);
	}
}

// Typed text: a number or an expression, evaluated by Godot's Expression like its own number fields
// (2*3, sqrt(2), PI/4); starting with * / + it goes on from the value shown: *2, /2, +0.5.
bool HewnDragField::_evaluate(const String &p_text, double &r_value) const {
	String t = p_text.strip_edges().replace(",", ".");
	if (t.is_empty()) {
		return false;
	}
	if (t[0] == '*' || t[0] == '/' || t[0] == '+') {
		t = "(" + String::num(value, 10) + ")" + t;
	}
	return eval_expression(t, r_value);
}

bool HewnDragField::eval_expression(const String &p_text, double &r_value) {
	String t = p_text.strip_edges().replace(",", ".");
	if (t.is_empty()) {
		return false;
	}
	if (t.is_valid_float()) {
		r_value = t.to_float();
		return true;
	}
	// whole numbers as floats: Expression divides integers as integers (1/2 would be 0)
	String f;
	for (int64_t i = 0; i < t.length(); i++) {
		const char32_t c = t[i];
		f += String::chr(c);
		const bool digit = c >= '0' && c <= '9';
		if (!digit || (i + 1 < t.length() && ((t[i + 1] >= '0' && t[i + 1] <= '9') || t[i + 1] == '.' || t[i + 1] == 'e'))) {
			continue;
		}
		int64_t s = i;
		while (s > 0 && ((t[s - 1] >= '0' && t[s - 1] <= '9') || t[s - 1] == '.')) {
			s--;
		}
		const bool in_name = s > 0 && (t[s - 1] == '_' || (t[s - 1] >= 'a' && t[s - 1] <= 'z') || (t[s - 1] >= 'A' && t[s - 1] <= 'Z'));
		if (!in_name && !t.substr(s, i - s + 1).contains(".")) {
			f += ".0";
		}
	}
	Ref<Expression> e;
	e.instantiate();
	if (e->parse(f) != OK) {
		return false;
	}
	const Variant r = e->execute(Array(), nullptr, false, true);
	if (e->has_execute_failed() || (r.get_type() != Variant::FLOAT && r.get_type() != Variant::INT)) {
		return false;
	}
	r_value = (double)r;
	return std::isfinite(r_value);
}

void HewnDragField::_on_submitted(const String &p_text) {
	typing = false;
	double v;
	if (_evaluate(p_text, v)) {
		_assign(v, true);
	} else {
		_show();
	}
	release_focus();
}

void HewnDragField::_on_focus_exited() {
	typing = false;
	double v;
	if (_evaluate(get_text(), v)) {
		_assign(v, true);
	} else {
		_show();
	}
}

void HewnDragField::_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		if (typing && !dragging && !pressed) {
			return; // typing after a click: normal line edit behavior
		}
		if (mb->is_pressed()) {
			pressed = true;
			dragging = false;
			drag_accum = 0.0f;
			press_window_pos = get_viewport()->get_mouse_position();
			skip_motion = false;
			accept_event();
		} else if (pressed) {
			pressed = false;
			if (dragging) {
				dragging = false;
				drag_count = MAX(0, drag_count - 1);
				Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
				Input::get_singleton()->warp_mouse(press_window_pos);
				// no focus left behind: Ctrl+Z would undo the field's text (a value from the middle of the
				// drag) instead of the operation
				release_focus();
			} else {
				typing = true; // plain click: type a value
				grab_focus();
				select_all();
			}
			accept_event();
		}
		return;
	}
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && pressed) {
		if (skip_motion) {
			skip_motion = false; // the jump caused by our own warp
			accept_event();
			return;
		}
		const float dx = mm->get_relative().x;
		if (!dragging) {
			drag_accum += dx;
		}
		if (!dragging && std::fabs(drag_accum) > 3.0f) {
			dragging = true;
			drag_count++;
			drag_start = value;
			// The cursor is hidden (not captured: the field keeps getting the events) and brought back
			// next to the press point whenever it wanders off, so the drag never hits the screen edge.
			Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_HIDDEN);
		}
		if (dragging) {
			// Speed relative to the step; Shift = precise, Ctrl = whole steps. The value moves by each
			// motion's own amount, so pressing / releasing Shift mid-drag continues from where it is.
			double per_px = is_int ? 0.1 : MAX(step, 1e-4) * 2.0;
			if (mm->is_shift_pressed()) {
				per_px *= 0.1;
			}
			drag_start = CLAMP(drag_start + dx * per_px, min_value, max_value); // unsnapped running value
			double v = drag_start;
			if (mm->is_ctrl_pressed() && !is_int) {
				const double s = step * 10.0;
				v = std::round(v / s) * s;
			}
			_assign(v, true);
			if (get_viewport()->get_mouse_position().distance_to(press_window_pos) > 150.0f) {
				Input::get_singleton()->warp_mouse(press_window_pos);
				skip_motion = true;
			}
		}
		accept_event();
	}
}

namespace {

void spin_typed(const String &p_text, LineEdit *p_le) {
	p_le->set_meta("hw_typed", p_text);
}

bool spin_eval(LineEdit *p_le, double &r_value) {
	const String t = p_le->has_meta("hw_typed") ? String(p_le->get_meta("hw_typed")) : p_le->get_text();
	return !t.strip_edges().is_valid_float() && HewnDragField::eval_expression(t, r_value);
}

void spin_key(const Ref<InputEvent> &p_event, LineEdit *p_le, SpinBox *p_spin) {
	Ref<InputEventKey> k = p_event;
	double v = 0.0;
	if (k.is_valid() && k->is_pressed() && (k->get_keycode() == KEY_ENTER || k->get_keycode() == KEY_KP_ENTER) && spin_eval(p_le, v)) {
		// (before the SpinBox reads it: the value as a plain number)
		p_le->set_text(String::num(v, 6));
		p_le->set_meta("hw_typed", p_le->get_text());
	}
}

void spin_focus_out(LineEdit *p_le, SpinBox *p_spin) {
	double v = 0.0;
	if (spin_eval(p_le, v)) {
		p_spin->set_value(v);
	}
	p_le->remove_meta("hw_typed");
}

} // namespace

void HewnDragField::hook_spinbox(SpinBox *p_spin) {
	LineEdit *le = p_spin ? p_spin->get_line_edit() : nullptr;
	if (!le || le->has_meta("hw_hooked")) {
		return;
	}
	le->set_meta("hw_hooked", true);
	le->connect("text_changed", callable_mp_static(&spin_typed).bind(le));
	le->connect("gui_input", callable_mp_static(&spin_key).bind(le, p_spin));
	le->connect("focus_exited", callable_mp_static(&spin_focus_out).bind(le, p_spin));
}
