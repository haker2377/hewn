#include "hw_profile_editor.h"

#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

using namespace godot;
using namespace hw_editor;

namespace {
float seg_distance(const Vector2 &p, const Vector2 &a, const Vector2 &b, float &r_t) {
	const Vector2 ab = b - a;
	const float l2 = ab.length_squared();
	r_t = l2 > 1e-12f ? CLAMP((p - a).dot(ab) / l2, 0.0f, 1.0f) : 0.0f;
	return p.distance_to(a + ab * r_t);
}
} // namespace

// ------------------------------------------------------------------ the view

void HewnProfileView::_bind_methods() {
	ADD_SIGNAL(MethodInfo("selection_changed"));
	ADD_SIGNAL(MethodInfo("profile_edited"));
}

Vector2 HewnProfileView::to_screen(const Vector2 &p) const {
	const Vector2 s = get_size();
	const Vector2 t = (p - view.position) / view.size;
	return Vector2(t.x * s.x, (1.0f - t.y) * s.y);
}

Vector2 HewnProfileView::to_profile(const Vector2 &q) const {
	const Vector2 s = get_size().max(Vector2(1, 1));
	return view.position + Vector2(q.x / s.x, 1.0f - q.y / s.y) * view.size;
}

void HewnProfileView::set_profile(const Ref<HewnProfile> &p_profile, const Rect2 &p_clip) {
	profile = p_profile;
	clip_rect = p_clip;
	// the view: the clipping box and the points, with a margin
	Rect2 r = p_clip;
	if (profile.is_valid()) {
		for (const Vector2 &p : profile->get_points()) {
			r.expand_to(p);
		}
	}
	const float m = MAX(r.size.x, r.size.y) * 0.1f + 1e-3f;
	r = r.grow(m);
	// square pixels: the shorter side grows
	const Vector2 s = get_size().max(Vector2(300, 300));
	const float aspect = s.x / s.y;
	if (r.size.x / r.size.y < aspect) {
		const float w = r.size.y * aspect;
		r.position.x -= (w - r.size.x) * 0.5f;
		r.size.x = w;
	} else {
		const float h = r.size.x / aspect;
		r.position.y -= (h - r.size.y) * 0.5f;
		r.size.y = h;
	}
	home = r;
	view = r;
	selected = -1;
	queue_redraw();
}

void HewnProfileView::select(int p_index) {
	selected = p_index;
	emit_signal("selection_changed");
	queue_redraw();
}

void HewnProfileView::zoom(float p_factor, const Vector2 &p_at) {
	const Vector2 c = to_profile(p_at);
	view.size /= p_factor;
	view.position = c - (c - view.position) / p_factor;
	queue_redraw();
}

void HewnProfileView::reset_view() {
	view = home;
	queue_redraw();
}

void HewnProfileView::set_clipping(bool p_on) {
	clipping = p_on;
	queue_redraw();
}

void HewnProfileView::edit(const Dictionary &p_before, const String &p_name) {
	if (profile.is_null() || !plugin) {
		return;
	}
	EditorUndoRedoManager *ur = plugin->get_undo_redo();
	ur->create_action(String("Hewn: ") + p_name);
	ur->add_do_method(profile.ptr(), "set_state", profile->get_state());
	ur->add_undo_method(profile.ptr(), "set_state", p_before);
	ur->commit_action(false);
	emit_signal("profile_edited");
	queue_redraw();
}

void HewnProfileView::_commit() {
	if (moved) {
		edit(before, "Edit Profile");
	}
	moved = false;
}

void HewnProfileView::_gui_input(const Ref<InputEvent> &p_event) {
	if (profile.is_null()) {
		return;
	}
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	Ref<InputEventKey> k = p_event;
	const PackedVector2Array pts = profile->get_points();
	const bool zoomed = view.size.x < home.size.x * 0.99f;
	if (k.is_valid() && k->is_pressed() && (k->get_keycode() == KEY_DELETE || k->get_keycode() == KEY_X) && selected > 0 && selected < pts.size() - 1) {
		const Dictionary b = profile->get_state();
		profile->remove_point(selected);
		select(-1);
		edit(b, "Remove Profile Point");
		accept_event();
		return;
	}
	if (mb.is_valid() && mb->is_pressed() && (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP || mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN)) {
		zoom(mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? 1.15f : 1.0f / 1.15f, mb->get_position());
		accept_event();
		return;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_MIDDLE) {
		drag = mb->is_pressed() ? 4 : 0;
		drag_from = mb->get_position();
		drag_view = view;
		accept_event();
		return;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		if (!mb->is_pressed()) {
			if (drag == 1 || drag == 2 || drag == 3) {
				_commit();
			}
			drag = 0;
			accept_event();
			return;
		}
		grab_focus();
		const Vector2 m = mb->get_position();
		before = profile->get_state();
		moved = false;
		// the selected point's handles, then the points
		if (selected >= 0 && selected < pts.size() && profile->get_handle_type(selected) != HewnProfile::HANDLE_VECTOR) {
			for (int side = 0; side < 2; side++) {
				const Vector2 h = side == 0 ? profile->handle_in(selected) : profile->handle_out(selected);
				if (h != Vector2() && to_screen(pts[selected] + h).distance_to(m) < 7.0f) {
					drag = side == 0 ? 2 : 3;
					accept_event();
					return;
				}
			}
		}
		for (int i = 0; i < pts.size(); i++) {
			if (to_screen(pts[i]).distance_to(m) < 8.0f) {
				select(i);
				drag = 1;
				accept_event();
				return;
			}
		}
		// on the curve: a new point there
		const PackedVector2Array s = profile->sample();
		const int per = MAX(1, (int)(s.size() - 1) / MAX(1, (int)pts.size() - 1));
		float best = 7.0f;
		int seg = -1;
		for (int i = 0; i + 1 < s.size(); i++) {
			float t;
			const float d = seg_distance(m, to_screen(s[i]), to_screen(s[i + 1]), t);
			if (d < best) {
				best = d;
				seg = i;
			}
		}
		if (seg >= 0 && !profile->get_sample_equal()) {
			// (which piece of the curve: the samples come piece after piece)
			int piece = 0;
			int acc = 0;
			for (int i = 0; i + 1 < pts.size(); i++) {
				const bool straight = profile->get_handle_type(i) == HewnProfile::HANDLE_VECTOR && profile->get_handle_type(i + 1) == HewnProfile::HANDLE_VECTOR;
				const int n = straight && !profile->get_sample_straight() ? 1 : profile->get_resolution();
				if (seg < acc + n) {
					piece = i;
					break;
				}
				acc += n;
				piece = i;
			}
			(void)per;
			const int at = profile->insert_point(piece + 1, to_profile(m));
			select(at);
			drag = 1;
			moved = true;
			accept_event();
			return;
		}
		// empty space: pan when zoomed in or not clipped, else nothing selected
		if (zoomed || !clipping) {
			drag = 4;
			drag_from = m;
			drag_view = view;
		} else {
			select(-1);
		}
		accept_event();
		return;
	}
	if (mm.is_valid() && drag != 0) {
		if (drag == 4) {
			const Vector2 d = (mm->get_position() - drag_from) / get_size().max(Vector2(1, 1)) * view.size;
			view.position = drag_view.position + Vector2(-d.x, d.y);
			queue_redraw();
			accept_event();
			return;
		}
		Vector2 p = to_profile(mm->get_position());
		if (mm->is_ctrl_pressed()) {
			p = (p / 0.05f).round() * 0.05f; // (Ctrl: on a grid of 0.05)
		}
		if (drag == 1 && selected >= 0) {
			if (clipping) {
				p = p.clamp(clip_rect.position, clip_rect.get_end());
			}
			profile->set_point(selected, p);
		} else if ((drag == 2 || drag == 3) && selected >= 0) {
			profile->set_point_handle(selected, drag == 3, p - profile->get_points()[selected]);
		}
		moved = true;
		emit_signal("selection_changed");
		queue_redraw();
		accept_event();
	}
}

void HewnProfileView::_draw() {
	const Vector2 s = get_size();
	Ref<Theme> et = EditorInterface::get_singleton()->get_editor_theme();
	const Color bg = et.is_valid() && et->has_color("dark_color_2", "Editor") ? et->get_color("dark_color_2", "Editor") : Color(0.12f, 0.12f, 0.14f);
	draw_rect(Rect2(Vector2(), s), bg);
	// the grid: tenths, the clipping box brighter
	const float step = std::pow(10.0f, std::floor(std::log10(MAX(1e-4f, view.size.x))) - 1.0f);
	for (float x = std::floor(view.position.x / step) * step; x <= view.get_end().x; x += step) {
		const bool major = std::fabs(x / (step * 10.0f) - std::round(x / (step * 10.0f))) < 1e-3f;
		draw_line(Vector2(to_screen(Vector2(x, 0)).x, 0), Vector2(to_screen(Vector2(x, 0)).x, s.y), Color(1, 1, 1, major ? 0.14f : 0.05f));
	}
	for (float y = std::floor(view.position.y / step) * step; y <= view.get_end().y; y += step) {
		const bool major = std::fabs(y / (step * 10.0f) - std::round(y / (step * 10.0f))) < 1e-3f;
		draw_line(Vector2(0, to_screen(Vector2(0, y)).y), Vector2(s.x, to_screen(Vector2(0, y)).y), Color(1, 1, 1, major ? 0.14f : 0.05f));
	}
	if (clipping) {
		const Vector2 a = to_screen(clip_rect.position), b = to_screen(clip_rect.get_end());
		draw_rect(Rect2(Vector2(a.x, b.y), Vector2(b.x - a.x, a.y - b.y)), Color(1, 1, 1, 0.35f), false, 1.0f);
	}
	if (profile.is_null()) {
		return;
	}
	const PackedVector2Array sm = profile->sample();
	PackedVector2Array line;
	for (const Vector2 &p : sm) {
		line.push_back(to_screen(p));
	}
	if (line.size() >= 2) {
		draw_polyline(line, Color(0.9f, 0.9f, 0.9f), 2.0f, true);
		for (const Vector2 &q : line) {
			draw_circle(q, 1.5f, Color(0.9f, 0.9f, 0.9f, 0.5f)); // (where the curve is sampled)
		}
	}
	const PackedVector2Array pts = profile->get_points();
	if (selected >= 0 && selected < pts.size() && profile->get_handle_type(selected) != HewnProfile::HANDLE_VECTOR) {
		for (int side = 0; side < 2; side++) {
			const Vector2 h = side == 0 ? profile->handle_in(selected) : profile->handle_out(selected);
			if (h == Vector2()) {
				continue;
			}
			const Vector2 a = to_screen(pts[selected]), b = to_screen(pts[selected] + h);
			draw_line(a, b, Color(0.9f, 0.6f, 0.9f), 1.5f, true);
			draw_circle(b, 4.0f, Color(0.9f, 0.6f, 0.9f));
		}
	}
	for (int i = 0; i < pts.size(); i++) {
		const bool sel = i == selected;
		draw_circle(to_screen(pts[i]), sel ? 6.0f : 4.5f, sel ? Color(1.0f, 0.6f, 0.15f) : Color(0.1f, 0.1f, 0.1f));
		draw_circle(to_screen(pts[i]), sel ? 4.0f : 3.0f, sel ? Color(1.0f, 0.85f, 0.5f) : Color(0.95f, 0.95f, 0.95f));
	}
}

// ------------------------------------------------------------------ the window

void HewnProfileDialog::_bind_methods() {
}

void HewnProfileDialog::setup(HewnEditorPlugin *p_plugin) {
	set_title("Profile");
	set_exclusive(false);
	set_ok_button_text("Close");
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	Ref<Theme> et = EditorInterface::get_singleton()->get_editor_theme();
	VBoxContainer *v = memnew(VBoxContainer);
	add_child(v);
	// the buttons above: zoom in / out, clipping, reset the view, reset the curve
	HBoxContainer *top = memnew(HBoxContainer);
	v->add_child(top);
	auto tool = [&](const char *p_icon, const char *p_tip, bool p_toggle) {
		Button *b = memnew(Button);
		b->set_flat(true);
		b->set_toggle_mode(p_toggle);
		b->set_button_icon(et->get_icon(p_icon, "EditorIcons"));
		b->set_tooltip_text(p_tip);
		top->add_child(b);
		return b;
	};
	tool("ZoomMore", "Zoom in", false)->connect("pressed", callable_mp(this, &HewnProfileDialog::_on_zoom).bind(1.25f));
	tool("ZoomLess", "Zoom out", false)->connect("pressed", callable_mp(this, &HewnProfileDialog::_on_zoom).bind(0.8f));
	clip_button = tool("Lock", "Toggle profile clipping: the points stay inside the box (off: anywhere, and the view can be moved)", true);
	clip_button->set_pressed(true);
	clip_button->connect("toggled", callable_mp(this, &HewnProfileDialog::_on_clip));
	tool("ZoomReset", "Reset the view", false)->connect("pressed", callable_mp(this, &HewnProfileDialog::_on_reset_view));
	tool("Reload", "Reset the curve (as it was when this window opened)", false)->connect("pressed", callable_mp(this, &HewnProfileDialog::_on_reset_curve));
	Label *hint = memnew(Label);
	hint->set_text("Click on the curve: a point, Del / X: remove it, wheel: zoom, MMB: move the view, Ctrl: steps of 0.05");
	hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	hint->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	hint->set_custom_minimum_size(Vector2(120 * es, 0));
	hint->add_theme_color_override("font_color", Color(0.7f, 0.7f, 0.7f));
	top->add_child(hint);
	canvas = memnew(HewnProfileView);
	canvas->plugin = p_plugin;
	canvas->set_custom_minimum_size(Vector2(340, 300) * es);
	canvas->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	canvas->set_focus_mode(Control::FOCUS_CLICK);
	canvas->set_clip_contents(true);
	canvas->connect("selection_changed", callable_mp(this, &HewnProfileDialog::_sync));
	canvas->connect("profile_edited", callable_mp(this, &HewnProfileDialog::_sync));
	v->add_child(canvas);
	// the selected point: its handle type and position
	HBoxContainer *ht = memnew(HBoxContainer);
	v->add_child(ht);
	static const char *names[4] = { "Auto Handle", "Vector Handle", "Free Handle", "Aligned Free Handles" };
	static const char *icons[4] = { "CurveAuto", "CurveLinear", "CurveCurve", "CurveConstant" };
	for (int i = 0; i < 4; i++) {
		Button *b = memnew(Button);
		b->set_toggle_mode(true);
		b->set_text(names[i]);
		b->set_tooltip_text(names[i]);
		b->set_clip_text(true);
		b->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		b->connect("pressed", callable_mp(this, &HewnProfileDialog::_on_type).bind(i));
		ht->add_child(b);
		types[i] = b;
		(void)icons;
	}
	HBoxContainer *xy = memnew(HBoxContainer);
	v->add_child(xy);
	for (int i = 0; i < 2; i++) {
		Label *l = memnew(Label);
		l->set_text(i == 0 ? "X" : "Y");
		xy->add_child(l);
		SpinBox *sb = memnew(SpinBox);
		HewnDragField::hook_spinbox(sb);
		sb->set_min(-1e6);
		sb->set_max(1e6);
		sb->set_step(0.001);
		sb->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		sb->connect("value_changed", callable_mp(this, &HewnProfileDialog::_on_xy));
		xy->add_child(sb);
		(i == 0 ? px : py) = sb;
	}
	// how the curve is sampled
	HBoxContainer *sh = memnew(HBoxContainer);
	v->add_child(sh);
	straight = memnew(CheckBox);
	straight->set_text("Sample Straight Edges");
	straight->set_tooltip_text("Straight pieces get as many samples as curved ones (else only their ends)");
	straight->connect("toggled", callable_mp(this, &HewnProfileDialog::_on_sampling));
	sh->add_child(straight);
	symmetric = memnew(CheckBox);
	symmetric->set_text("Symmetric");
	symmetric->set_tooltip_text("The two halves mirrored around the middle: a change on one side is copied onto the other");
	symmetric->connect("toggled", callable_mp(this, &HewnProfileDialog::_on_symmetric));
	sh->add_child(symmetric);
	equal = memnew(CheckBox);
	equal->set_text("Sample Equal Lengths");
	equal->set_tooltip_text("The samples at equal distances along the whole curve");
	equal->connect("toggled", callable_mp(this, &HewnProfileDialog::_on_sampling));
	sh->add_child(equal);
	Label *rl = memnew(Label);
	rl->set_text("Samples");
	sh->add_child(rl);
	res = memnew(SpinBox);
	HewnDragField::hook_spinbox(res);
	res->set_min(1);
	res->set_max(64);
	res->set_tooltip_text("Samples of each curved piece");
	res->connect("value_changed", callable_mp(this, &HewnProfileDialog::_on_res));
	sh->add_child(res);
	// Godot's curves: a Curve or Curve2D dropped / picked here becomes this profile; export: a new one
	HBoxContainer *gc = memnew(HBoxContainer);
	v->add_child(gc);
	Label *gl = memnew(Label);
	gl->set_text("Godot curve");
	gc->add_child(gl);
	curve_picker = memnew(EditorResourcePicker);
	curve_picker->set_base_type("Curve,Curve2D");
	curve_picker->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	curve_picker->set_tooltip_text("Drop or pick a Curve / Curve2D: it becomes this profile (its points and tangents)");
	curve_picker->connect("resource_changed", callable_mp(this, &HewnProfileDialog::_on_curve_picked));
	gc->add_child(curve_picker);
	Button *e1 = memnew(Button);
	e1->set_text("To Curve");
	e1->set_tooltip_text("This profile as a new Curve (in the field: save it from its menu)");
	e1->connect("pressed", callable_mp(this, &HewnProfileDialog::_on_export).bind(false));
	gc->add_child(e1);
	Button *e2 = memnew(Button);
	e2->set_text("To Curve2D");
	e2->set_tooltip_text("This profile as a new Curve2D (in the field: save it from its menu)");
	e2->connect("pressed", callable_mp(this, &HewnProfileDialog::_on_export).bind(true));
	gc->add_child(e2);
	localize(this);
}

void HewnProfileDialog::open(const Ref<HewnProfile> &p_profile, const String &p_title, const Rect2 &p_clip, bool p_symmetry) {
	set_title(p_title);
	symmetry_allowed = p_symmetry;
	reset_state = p_profile.is_valid() ? p_profile->get_state() : Dictionary();
	if (!is_visible()) {
		popup_centered();
	}
	canvas->set_profile(p_profile, p_clip);
	_sync();
}

void HewnProfileDialog::_sync() {
	Ref<HewnProfile> p = canvas->get_profile();
	if (p.is_null()) {
		return;
	}
	updating = true;
	const int sel = canvas->get_selected();
	const bool has = sel >= 0 && sel < p->get_points().size();
	for (int i = 0; i < 4; i++) {
		types[i]->set_disabled(!has);
		types[i]->set_pressed_no_signal(has && p->get_handle_type(sel) == i);
	}
	px->set_editable(has);
	py->set_editable(has);
	if (has) {
		px->set_value_no_signal(p->get_points()[sel].x);
		py->set_value_no_signal(p->get_points()[sel].y);
	}
	straight->set_pressed_no_signal(p->get_sample_straight());
	symmetric->set_pressed_no_signal(p->get_symmetric());
	symmetric->set_visible(symmetry_allowed);
	equal->set_pressed_no_signal(p->get_sample_equal());
	res->set_value_no_signal(p->get_resolution());
	updating = false;
	canvas->queue_redraw();
}

void HewnProfileDialog::_on_type(int p_type) {
	Ref<HewnProfile> p = canvas->get_profile();
	const int sel = canvas->get_selected();
	if (p.is_null() || sel < 0) {
		return;
	}
	const Dictionary b = p->get_state();
	p->set_point_handle_type(sel, p_type);
	canvas->edit(b, "Profile Handle Type");
	_sync();
}

void HewnProfileDialog::_on_xy(double p_value) {
	Ref<HewnProfile> p = canvas->get_profile();
	const int sel = canvas->get_selected();
	if (updating || p.is_null() || sel < 0) {
		return;
	}
	const Dictionary b = p->get_state();
	p->set_point(sel, Vector2((float)px->get_value(), (float)py->get_value()));
	canvas->edit(b, "Move Profile Point");
}

void HewnProfileDialog::_on_sampling(bool p_unused) {
	Ref<HewnProfile> p = canvas->get_profile();
	if (updating || p.is_null()) {
		return;
	}
	const Dictionary b = p->get_state();
	p->set_sample_straight(straight->is_pressed());
	p->set_sample_equal(equal->is_pressed());
	canvas->edit(b, "Profile Sampling");
}

void HewnProfileDialog::_on_symmetric(bool p_on) {
	Ref<HewnProfile> p = canvas->get_profile();
	if (updating || p.is_null()) {
		return;
	}
	const Dictionary b = p->get_state();
	p->set_symmetric(p_on);
	canvas->select(-1);
	canvas->edit(b, "Profile Symmetry");
}

void HewnProfileDialog::_on_res(double p_value) {
	Ref<HewnProfile> p = canvas->get_profile();
	if (updating || p.is_null()) {
		return;
	}
	const Dictionary b = p->get_state();
	p->set_resolution((int)p_value);
	canvas->edit(b, "Profile Sampling");
}

void HewnProfileDialog::_on_zoom(float p_factor) {
	canvas->zoom(p_factor, canvas->get_size() * 0.5f);
}

void HewnProfileDialog::_on_reset_view() {
	canvas->reset_view();
}

void HewnProfileDialog::_on_reset_curve() {
	Ref<HewnProfile> p = canvas->get_profile();
	if (p.is_null() || reset_state.is_empty()) {
		return;
	}
	const Dictionary b = p->get_state();
	p->set_state(reset_state);
	canvas->select(-1);
	canvas->edit(b, "Reset Profile");
	_sync();
}

void HewnProfileDialog::_on_clip(bool p_on) {
	canvas->set_clipping(p_on);
}

void HewnProfileDialog::_on_curve_picked(const Ref<Resource> &p_res) {
	Ref<HewnProfile> p = canvas->get_profile();
	if (updating || p.is_null() || p_res.is_null()) {
		return;
	}
	const Dictionary b = p->get_state();
	if (Ref<Curve> c = p_res; c.is_valid()) {
		p->from_curve(c);
	} else if (Ref<Curve2D> c2 = p_res; c2.is_valid()) {
		p->from_curve2d(c2);
	} else {
		return;
	}
	canvas->select(-1);
	canvas->edit(b, "Profile from Curve");
	_sync();
}

void HewnProfileDialog::_on_export(bool p_2d) {
	Ref<HewnProfile> p = canvas->get_profile();
	if (p.is_null()) {
		return;
	}
	updating = true; // (not taken back in)
	curve_picker->set_edited_resource(p_2d ? Ref<Resource>(p->to_curve2d()) : Ref<Resource>(p->to_curve()));
	updating = false;
}

// ------------------------------------------------------------------ the plugin's side

void HewnEditorPlugin::open_profile(const Ref<HewnProfile> &p_profile, const String &p_title, const Rect2 &p_clip, bool p_symmetry) {
	if (p_profile.is_null()) {
		return;
	}
	if (!profile_dialog) {
		profile_dialog = memnew(HewnProfileDialog);
		profile_dialog->setup(this);
		EditorInterface::get_singleton()->get_base_control()->add_child(profile_dialog);
	}
	profile_dialog->open(p_profile, T(p_title), p_clip, p_symmetry);
}

// The bevel's custom profile: default (a quarter round) until it is drawn; kept in the project's
// editor data.
Ref<HewnProfile> HewnEditorPlugin::_bevel_profile() {
	if (bevel_profile.is_null()) {
		bevel_profile.instantiate();
		Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
		const Dictionary saved = es.is_valid() ? (Dictionary)es->get_project_metadata("hewn", "bevel_profile", Dictionary()) : Dictionary();
		if (!saved.is_empty()) {
			bevel_profile->set_state(saved);
		} else {
			PackedVector2Array pts;
			pts.push_back(Vector2(1, 0));
			pts.push_back(Vector2(0.75f, 0.75f));
			pts.push_back(Vector2(0, 1));
			bevel_profile->set_points(pts);
			bevel_profile->set_point_handle_type(1, HewnProfile::HANDLE_AUTO);
		}
		bevel_profile->connect("changed", callable_mp(this, &HewnEditorPlugin::_on_bevel_profile_changed));
	}
	return bevel_profile;
}

void HewnEditorPlugin::_on_bevel_profile_changed() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid() && bevel_profile.is_valid()) {
		es->set_project_metadata("hewn", "bevel_profile", bevel_profile->get_state());
	}
	// the bevel just made follows the curve
	if (last_op == "bevel" && redo_panel && redo_panel->is_visible() && (int)op_param("bevel", "profile_type") == 1) {
		_redo_last();
	}
}

void HewnEditorPlugin::edit_bevel_profile() {
	set_op_param("bevel", "profile_type", 1);
	if (last_op == "bevel" && redo_panel && redo_panel->is_visible()) {
		_redo_last();
		_show_redo();
	}
	open_profile(_bevel_profile(), "Bevel Profile", Rect2(0, 0, 1, 1));
}

void HewnEditorPlugin::_on_shape_profile_button(int p_index) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	const int t = n->get_shape();
	if (t == hewn::shapes::COLUMN) {
		edit_shape_profile(p_index == 0 ? "base" : p_index == 1 ? "shaft" : "top");
	} else if (t == hewn::shapes::SWEEP) {
		edit_shape_profile(p_index == 1 ? "accent" : "sweep");
	} else if (p_index == 1) {
		EditorUndoRedoManager *ur = get_undo_redo();
		ur->create_action("Hewn: Plain Shape", UndoRedo::MERGE_DISABLE, n);
		ur->add_do_method(n, "set_shape_profile", Ref<HewnProfile>());
		ur->add_undo_method(n, "set_shape_profile", n->get_shape_profile());
		ur->commit_action();
		_update_shape_panel();
	} else {
		edit_shape_profile("shape");
	}
}

// The selected mesh's curve in the profile window; one it does not have yet starts from what its
// parameters make (the column's style, a rectangle door, a half round arch, a straight slope).
void HewnEditorPlugin::edit_shape_profile(const String &p_which) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	Ref<HewnProfile> p;
	Rect2 clip(0, 0, 1, 1);
	String title;
	if (p_which == "base" || p_which == "shaft" || p_which == "top") {
		p = p_which == "base" ? n->get_base_profile() : p_which == "shaft" ? n->get_shaft_profile() : n->get_top_profile();
		if (p.is_null()) {
			p = HewnProfile::make_preset(p_which, (int)n->get_shape_param(String(p_which) + "_style"));
			const String setter = "set_" + p_which + "_profile";
			EditorUndoRedoManager *ur = get_undo_redo();
			ur->create_action("Hewn: Column Curve", UndoRedo::MERGE_DISABLE, n);
			ur->add_do_method(n, setter, p);
			ur->add_undo_method(n, setter, Ref<HewnProfile>());
			ur->commit_action();
		}
		clip = Rect2(0, 0, 2, 1); // (x: the radius times 0..2, y: up the part)
		title = p_which == "base" ? "Column Base" : p_which == "shaft" ? "Column Shaft" : "Column Top";
	} else if (p_which == "sweep") {
		if (n->get_sweep_profile().is_null()) {
			EditorUndoRedoManager *ur = get_undo_redo();
			const Dictionary before = n->get_state();
			ur->create_action("Hewn: Sweep Curve", UndoRedo::MERGE_DISABLE, n);
			n->make_profiles_editable();
			ur->add_do_method(n, "set_sweep_profile", n->get_sweep_profile());
			ur->add_do_method(n, "set_shape_param", "profile", 6);
			ur->add_undo_method(n, "set_sweep_profile", Ref<HewnProfile>());
			ur->add_undo_method(n, "set_state", before);
			ur->commit_action(false);
		}
		p = n->get_sweep_profile();
		clip = Rect2(0, 0, 1, 1);
		for (const Vector2 &q : p->get_points()) {
			clip.expand_to(q);
		}
		title = "Sweep Profile";
	} else if (p_which == "accent") {
		p = n->get_shape_profile();
		if (p.is_null() || (int)n->get_shape_param("accent_shape") != 7) {
			// (a block with a bevel on top to start from: the accents' shape Custom)
			EditorUndoRedoManager *ur = get_undo_redo();
			const Dictionary before = n->get_state();
			if (p.is_null()) {
				p.instantiate();
				p->set_points(PackedVector2Array({ Vector2(0, 0), Vector2(1, 0), Vector2(1, 0.8f), Vector2(0.8f, 1), Vector2(0, 1) }));
			}
			ur->create_action("Hewn: Accent Curve", UndoRedo::MERGE_DISABLE, n);
			ur->add_do_method(n, "set_shape_profile", p);
			ur->add_do_method(n, "set_shape_param", "accent_shape", 7);
			ur->add_undo_method(n, "set_state", before);
			ur->add_undo_method(n, "set_shape_profile", n->get_shape_profile());
			ur->commit_action();
		}
		for (const Vector2 &q : p->get_points()) {
			clip.expand_to(q);
		}
		title = "Accent Shape";
	} else {
		p = n->get_shape_profile();
		const int t = n->get_shape();
		if (p.is_null()) {
			p.instantiate();
			PackedVector2Array pts;
			if (t == hewn::shapes::DOOR) {
				pts = PackedVector2Array({ Vector2(0, 0), Vector2(0, 1), Vector2(1, 1), Vector2(1, 0) });
				p->set_points(pts);
				p->set_symmetric(true); // (doors and arches: symmetric unless turned off)
			} else if (t == hewn::shapes::ARCH) {
				pts = PackedVector2Array({ Vector2(0, 0), Vector2(0.5f, 1), Vector2(1, 0) });
				p->set_points(pts);
				p->set_point_handle_type(1, HewnProfile::HANDLE_AUTO);
				p->set_point_handle(0, true, Vector2(0, 0.55f));
				p->set_point_handle(2, false, Vector2(0, 0.55f));
				p->set_point_handle_type(0, HewnProfile::HANDLE_FREE);
				p->set_point_handle_type(2, HewnProfile::HANDLE_FREE);
				p->set_resolution(8);
				p->set_symmetric(true);
			} else {
				pts = PackedVector2Array({ Vector2(0, 0), Vector2(1, 1) });
				p->set_points(pts);
			}
			EditorUndoRedoManager *ur = get_undo_redo();
			ur->create_action("Hewn: Shape Curve", UndoRedo::MERGE_DISABLE, n);
			ur->add_do_method(n, "set_shape_profile", p);
			ur->add_undo_method(n, "set_shape_profile", Ref<HewnProfile>());
			ur->commit_action();
		}
		title = t == hewn::shapes::DOOR ? "Doorway Outline" : t == hewn::shapes::ARCH ? "Arch Curve" : "Wedge Slope";
	}
	const int st = n->get_shape();
	open_profile(p, title, clip, p_which == "shape" && (st == hewn::shapes::DOOR || st == hewn::shapes::ARCH));
	_update_shape_panel();
}
