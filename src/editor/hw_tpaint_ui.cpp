// Texture Paint mode: its input (the brush, the eyedropper, the fill, the stencil moved / scaled / turned with Alt),
// what it draws over the view (the stencil, the brush), its header (what is painted, the brush, the pen, the
// stamp, the stencil) and its menu.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/color_picker_button.hpp>
#include <godot_cpp/classes/curve.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_inspector.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_resource_picker.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/popup_panel.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include <cmath>

using namespace godot;
using namespace hw_editor;

// ------------------------------------------------------------------ input

int32_t HewnEditorPlugin::_tp_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	MeshInstance3D *mi = tp_mesh();
	if (!mi) {
		exit_tpaint(false);
		return -1;
	}
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	auto sync_fields = [&]() {
		if (dock && dock->tp_radius_field) {
			dock->tp_radius_field->set_value_no_signal(tp_radius);
			dock->tp_strength_field->set_value_no_signal(tp_strength);
		}
	};
	if (k.is_valid()) {
		if (!k->is_pressed() || k->is_echo()) {
			return AFTER_GUI_INPUT_PASS;
		}
		const Key code = key_code(k);
		const bool ctrl = k->is_ctrl_pressed() || k->is_meta_pressed(), shift = k->is_shift_pressed(), alt = k->is_alt_pressed();
		const bool plain = !ctrl && !shift && !alt;
		if (code == KEY_TAB && !shift && !alt && !ctrl) {
			exit_tpaint();
			return AFTER_GUI_INPUT_STOP;
		}
		if (plain && code == KEY_X && dock) {
			dock->_on_tp_swap();
			return AFTER_GUI_INPUT_STOP;
		}
		if (plain && code == KEY_D && dock) {
			dock->_on_tp_reset();
			return AFTER_GUI_INPUT_STOP;
		}
		if ((code == KEY_BRACKETLEFT || code == KEY_BRACKETRIGHT) && !ctrl && !alt) {
			const float f = code == KEY_BRACKETRIGHT ? 1.15f : 1.0f / 1.15f;
			if (shift) {
				tp_strength = CLAMP(tp_strength * f, 0.01f, 1.0f);
			} else {
				tp_radius = CLAMP(tp_radius * f, 1.0f, 2000.0f);
			}
			sync_fields();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (plain && (code == KEY_B || code == KEY_E)) {
			tp_tool = code == KEY_B ? 0 : 1;
			if (dock) {
				dock->refresh();
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		// (the other modes' tools would start on the scene)
		if (plain && (code == KEY_C || code == KEY_W || code == KEY_G || code == KEY_R || code == KEY_S || code == KEY_K || code == KEY_DELETE)) {
			return AFTER_GUI_INPUT_STOP;
		}
		if (shift && !ctrl && !alt && (code == KEY_A || code == KEY_D || code == KEY_X)) {
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (mb.is_valid() && mb->is_pressed() && (mb->is_ctrl_pressed() || mb->is_meta_pressed() || mb->is_shift_pressed()) && !mb->is_alt_pressed() &&
			(mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP || mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN)) {
		// Ctrl+wheel: the radius, Shift+wheel: the strength
		const float f = mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? 1.1f : 1.0f / 1.1f;
		if (mb->is_shift_pressed()) {
			tp_strength = CLAMP(tp_strength * f, 0.01f, 1.0f);
		} else {
			tp_radius = CLAMP(tp_radius * f, 1.0f, 2000.0f);
		}
		sync_fields();
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	const bool stencil_on = dock && dock->tp_stencil_btn && dock->tp_stencil_btn->is_pressed() && tp_stencil_w > 0;
	if (mm.is_valid()) {
		const float pr = mm->get_pressure();
		tp_pressure = pr > 0.0f ? pr : 1.0f; // (a mouse has no pressure: full)
		tp_tilt = mm->get_tilt();
		tp_speed = mm->get_velocity().length();
	}
	// the stencil moved (Alt+drag), scaled (Alt+Shift+drag), turned (Alt+Ctrl+drag)
	if (tp_stencil_drag) {
		if (mm.is_valid()) {
			mouse_pos = mm->get_position();
			if (tp_stencil_drag == 1) {
				tp_stencil_pos = tp_stencil_pos0 + (mouse_pos - tp_stencil_from);
			} else if (tp_stencil_drag == 2) {
				const float d0 = MAX(1.0f, tp_stencil_from.distance_to(tp_stencil_pos0));
				tp_stencil_scale = MAX(0.01f, tp_stencil_scale0 * mouse_pos.distance_to(tp_stencil_pos0) / d0);
			} else {
				tp_stencil_angle = tp_stencil_angle0 + (mouse_pos - tp_stencil_pos0).angle() - (tp_stencil_from - tp_stencil_pos0).angle();
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && !mb->is_pressed()) {
			tp_stencil_drag = 0;
			update_overlays();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	// Shift+drag: the radius (Ctrl+Shift: the strength), the circle where the drag began
	if (tp_resizing) {
		if (mm.is_valid()) {
			const float dx = mm->get_position().x - tp_resize_at.x;
			if (tp_resize_strength) {
				tp_strength = CLAMP(tp_resize_s0 + dx / 300.0f, 0.01f, 1.0f);
			} else {
				tp_radius = CLAMP(tp_resize_r0 + dx, 1.0f, 2000.0f);
			}
			sync_fields();
			mouse_pos = tp_resize_at;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && !mb->is_pressed()) {
			tp_resizing = false;
			Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
			if (Control *surface = _surface_for(p_cam)) {
				Input::get_singleton()->warp_mouse(surface->get_global_rect().position + tp_resize_at);
			}
			mouse_pos = tp_resize_at;
			update_overlays();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && mb->is_pressed()) {
		mouse_pos = mb->get_position();
		const bool ctrl = mb->is_ctrl_pressed() || mb->is_meta_pressed(), shift = mb->is_shift_pressed(), alt = mb->is_alt_pressed();
		if (alt && stencil_on) {
			tp_stencil_drag = shift ? 2 : ctrl ? 3 : 1;
			tp_stencil_from = mouse_pos;
			tp_stencil_pos0 = tp_stencil_pos;
			tp_stencil_scale0 = tp_stencil_scale;
			tp_stencil_angle0 = tp_stencil_angle;
			return AFTER_GUI_INPUT_STOP;
		}
		if (alt) {
			return AFTER_GUI_INPUT_PASS;
		}
		if (shift && (tp_tool <= 2)) {
			tp_resize_strength = ctrl;
			tp_resizing = true;
			tp_resize_at = mouse_pos;
			tp_resize_r0 = tp_radius;
			tp_resize_s0 = tp_strength;
			if (DisplayServer::get_singleton()->get_name() != "headless") {
				Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_HIDDEN);
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (!_tp_prepare(tp_tool <= 3)) {
			EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster();
			if (t) {
				t->push_toast(T("Texture Paint: nothing to paint on this mesh (no UVs, or a shader material)"), EditorToaster::SEVERITY_WARNING);
			}
			return AFTER_GUI_INPUT_STOP;
		}
		_tp_view(p_cam);
		if (tp_tool == 4 || ctrl) {
			_tp_pick(p_cam, mouse_pos, shift); // (Ctrl+click: a quick eyedropper)
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (tp_tool == 3) {
			_tp_fill(p_cam, mouse_pos);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		tp_painting = true;
		tp_moved = false;
		tp_stroke.clear();
		tp_last = mouse_pos;
		tp_left = 0.0f;
		tp_seed = (uint32_t)Time::get_singleton()->get_ticks_usec();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && !mb->is_pressed()) {
		if (tp_painting) {
			if (!tp_moved) {
				_tp_dab(p_cam, mb->get_position(), 1.0f, Vector2()); // (a click: one dab)
			}
			_tp_stroke_end();
			update_overlays();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (mm.is_valid()) {
		mouse_pos = mm->get_position();
		if (tp_painting) {
			if (!tp_moved) {
				tp_moved = true;
				_tp_dab(p_cam, tp_last, tp_pressure, mouse_pos - tp_last); // (the first dab: with the pen's pressure now)
				const float spacing = dock && dock->tp_spacing ? MAX(1.0f, (float)dock->tp_spacing->get_value()) : 15.0f;
				tp_left = MAX(1.0f, tp_radius * spacing * 0.01f);
			}
			_tp_stroke_to(p_cam, mouse_pos);
		}
		update_overlays();
		return tp_painting ? AFTER_GUI_INPUT_STOP : AFTER_GUI_INPUT_PASS;
	}
	return AFTER_GUI_INPUT_PASS;
}

// ------------------------------------------------------------------ drawing

void HewnEditorPlugin::_draw_tpaint(Control *p_overlay, Camera3D *p_cam) {
	if (!dock) {
		return;
	}
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	_tp_load_stamp();
	// the stencil: its picture where it lies, see-through
	if (dock->tp_stencil_btn && dock->tp_stencil_btn->is_pressed() && tp_stencil_w > 0) {
		Ref<Texture2D> st = dock->tp_stencil->get_edited_resource();
		if (!tp_stencil_placed) {
			const Vector2 sz = p_overlay->get_size();
			tp_stencil_pos = sz * 0.5f;
			tp_stencil_scale = MIN(sz.x * 0.6f / tp_stencil_w, sz.y * 0.6f / tp_stencil_h);
			tp_stencil_angle = 0.0f;
			tp_stencil_placed = true;
		}
		if (st.is_valid()) {
			const float op = dock->tp_stencil_opacity ? (float)dock->tp_stencil_opacity->get_value() : 0.5f;
			p_overlay->draw_set_transform(tp_stencil_pos, tp_stencil_angle, Vector2(tp_stencil_scale, tp_stencil_scale));
			const Vector2 half(tp_stencil_w * 0.5f, tp_stencil_h * 0.5f);
			p_overlay->draw_texture_rect(st, Rect2(-half, half * 2.0f), false, Color(1, 1, 1, op));
			p_overlay->draw_rect(Rect2(-half, half * 2.0f), Color(1, 1, 1, 0.35f), false, 1.0f / tp_stencil_scale);
			p_overlay->draw_set_transform(Vector2(), 0.0f, Vector2(1, 1));
		}
	}
	// the brush
	if (tp_tool <= 2 && !tp_stencil_drag) {
		const Vector2 at = tp_resizing ? tp_resize_at : mouse_pos;
		const Color ring = tp_tool == 1 ? Color(1.0f, 0.55f, 0.55f, 0.9f) : tp_tool == 2 ? Color(0.6f, 0.85f, 1.0f, 0.9f) : Color(1, 1, 1, 0.9f);
		p_overlay->draw_arc(at, tp_radius + 1.0f, 0.0f, (float)Math::TAU, 64, Color(0, 0, 0, 0.6f), 1.0f, true);
		p_overlay->draw_arc(at, tp_radius, 0.0f, (float)Math::TAU, 64, ring, 1.0f, true);
		const float inner = tp_radius * tp_strength;
		if (inner > 2.0f && inner < tp_radius - 1.0f) {
			p_overlay->draw_arc(at, inner, 0.0f, (float)Math::TAU, 48, Color(ring.r, ring.g, ring.b, 0.35f), 1.0f, true);
		}
		if (tp_stamp_w > 0) {
			const float a = dock->tp_angle ? (float)dock->tp_angle->get_value() * (float)Math::PI / 180.0f : 0.0f;
			const Vector2 c[4] = { Vector2(-1, -1), Vector2(1, -1), Vector2(1, 1), Vector2(-1, 1) };
			for (int i = 0; i < 4; i++) {
				p_overlay->draw_line(at + (c[i] * tp_radius).rotated(a), at + (c[(i + 1) % 4] * tp_radius).rotated(a), Color(ring.r, ring.g, ring.b, 0.4f), 1.0f, true);
			}
		}
		if (tp_resizing) {
			Ref<Font> font = p_overlay->get_theme_default_font();
			const String txt = tp_resize_strength ? vformat(T("Strength %.2f"), tp_strength) : vformat(T("Radius %d px"), (int)tp_radius);
			p_overlay->draw_string(font, at + Vector2(tp_radius + 8.0f * es, 4.0f * es), txt, HORIZONTAL_ALIGNMENT_LEFT, -1, p_overlay->get_theme_default_font_size(), Color(1, 1, 1));
		}
	}
	draw_header(p_overlay, p_overlay->get_theme_default_font(), tp_status());
}

// ------------------------------------------------------------------ the menu

void HewnEditorPlugin::tp_menu(int p_id) {
	switch (p_id) {
		case 0:
			tp_save_all();
			break;
		case 1:
		case 2: {
			// the whole picture: filled with the color / cleared (a Detail layer see-through, the albedo as loaded)
			if (!_tp_prepare(true)) {
				return;
			}
			std::vector<int> done;
			const Color col = dock && dock->tp_color ? dock->tp_color->get_pick_color() : Color(0, 0, 0);
			tp_stroke.clear();
			for (const HwPaintSurface &ps : tp_surf) {
				if (ps.target < 0 || std::find(done.begin(), done.end(), ps.target) != done.end()) {
					continue;
				}
				done.push_back(ps.target);
				HwPaintTexture &t = tp_tex[ps.target];
				const int tx = (t.w + 63) / 64, ty = (t.h + 63) / 64;
				for (int i = 0; i < tx * ty; i++) {
					_tp_tile(ps.target, i);
				}
				const bool detail = t.prop == "detail_albedo";
				for (size_t i = 0; i < t.px.size(); i += 4) {
					if (p_id == 1) {
						t.px[i] = (uint8_t)CLAMP((int)std::lround(col.r * 255.0f), 0, 255);
						t.px[i + 1] = (uint8_t)CLAMP((int)std::lround(col.g * 255.0f), 0, 255);
						t.px[i + 2] = (uint8_t)CLAMP((int)std::lround(col.b * 255.0f), 0, 255);
						t.px[i + 3] = (uint8_t)CLAMP((int)std::lround(col.a * 255.0f), 0, 255);
					} else if (detail) {
						t.px[i + 3] = 0;
					} else {
						memcpy(&t.px[i], &t.orig[i], 4);
					}
				}
				t.dirty = t.unsaved = true;
			}
			const int was = tp_tool;
			tp_tool = 3;
			_tp_stroke_end();
			tp_tool = was;
		} break;
		case 3:
			for (const HwPaintSurface &ps : tp_surf) {
				if (ps.target >= 0 && tp_tex[ps.target].texture.is_valid()) {
					EditorInterface::get_singleton()->get_inspector()->call_deferred("edit", tp_tex[ps.target].texture);
					break;
				}
			}
			break;
		case 4:
			tp_stencil_placed = false;
			break;
		default:
			break;
	}
	update_overlays();
}

// ------------------------------------------------------------------ the header

void HewnDock::build_tpaint_bar() {
	Ref<Theme> et = EditorInterface::get_singleton()->get_editor_theme();
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const Color base = et->has_color("dark_color_1", "Editor") ? et->get_color("dark_color_1", "Editor") : Color(0.1f, 0.11f, 0.13f);
	auto styled = [&](PanelContainer *p) {
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
		p->hide();
		return h;
	};
	int at = elements_block ? elements_block->get_index() + 1 : 1;
	auto row1 = [&]() {
		PanelContainer *p = memnew(PanelContainer);
		HBoxContainer *h = styled(p);
		toolbar->add_child(p);
		toolbar->move_child(p, at++);
		tp_row1_blocks.push_back(p);
		return h;
	};
	auto row2 = [&]() {
		PanelContainer *p = memnew(PanelContainer);
		HBoxContainer *h = styled(p);
		paint_row->add_child(p);
		tp_blocks.push_back(p);
		return h;
	};
	auto label = [&](Container *p_box, const String &p_text) {
		Label *l = memnew(Label);
		l->set_text(p_text);
		l->add_theme_color_override("font_color", Color(0.75f, 0.75f, 0.75f));
		p_box->add_child(l);
		return l;
	};
	auto field = [&](Container *p_box, double p_min, double p_max, double p_step, double p_value, const String &p_tip, bool p_int = false) {
		HewnDragField *f = memnew(HewnDragField);
		f->setup(p_min, p_max, p_step, p_int, p_value);
		f->set_custom_minimum_size(Vector2(52 * es, 0));
		f->set_tooltip_text(p_tip);
		p_box->add_child(f);
		return f;
	};
	auto option = [&](Container *p_box, const std::vector<String> &p_items, const String &p_tip) {
		OptionButton *o = memnew(OptionButton);
		for (const String &it : p_items) {
			o->add_item(it);
		}
		o->set_flat(true);
		o->set_fit_to_longest_item(false);
		o->set_tooltip_text(p_tip);
		p_box->add_child(o);
		return o;
	};
	auto check = [&](Container *p_box, const String &p_text, bool p_on) {
		CheckBox *c = memnew(CheckBox);
		c->set_text(p_text);
		c->set_pressed(p_on);
		c->connect("toggled", callable_mp(this, &HewnDock::_on_tp_changed).unbind(1));
		p_box->add_child(c);
		return c;
	};
	auto popup = [&](HBoxContainer *p_block, const String &p_text, const String &p_tip, const String &p_name) {
		PopupPanel *pp = memnew(PopupPanel);
		VBoxContainer *box = memnew(VBoxContainer);
		pp->add_child(box);
		paint_row->add_child(pp);
		Button *b = memnew(Button);
		b->set_text(T(p_text) + String(" ") + String::chr(0x25BE));
		b->set_flat(true);
		b->set_tooltip_text(p_tip);
		b->set_name(p_name);
		b->connect("pressed", callable_mp(this, &HewnDock::_popup_under).bind(pp, b));
		p_block->add_child(b);
		popup_opener(b, pp);
		return box;
	};
	auto picker = [&](Container *p_box, const String &p_type, const String &p_tip) {
		EditorResourcePicker *rp = memnew(EditorResourcePicker);
		rp->set_base_type(p_type);
		rp->set_custom_minimum_size(Vector2(180 * es, 0));
		rp->set_tooltip_text(p_tip);
		rp->connect("resource_changed", callable_mp(this, &HewnDock::_on_tp_changed).unbind(1));
		p_box->add_child(rp);
		return rp;
	};
	// the first row: what is painted
	HBoxContainer *wb = row1();
	tp_into = option(wb, { "Albedo", "Detail Layer" }, "Paint into the material's albedo texture (UV1), or into a Detail layer over it (UV1 or UV2: a picture of its own over a tiled material)");
	tp_into->set_name("TPInto");
	tp_into->connect("item_selected", callable_mp(this, &HewnDock::_on_tp_target));
	tp_uv = option(wb, { "UV1", "UV2" }, "The Detail layer's UVs: UV1 (as the material's), UV2 (the lightmap's unwrap: every face its own place)");
	tp_uv->set_name("TPUV");
	tp_uv->connect("item_selected", callable_mp(this, &HewnDock::_on_tp_target));
	HBoxContainer *sb2 = row1();
	label(sb2, T("New"));
	tp_size = option(sb2, { "512", "1024", "2048", "4096" }, "The size of a new picture (made with the first stroke when the material has none)");
	tp_size->set_name("TPSize");
	tp_size->select(1);
	HBoxContainer *ob = row1();
	VBoxContainer *ov = popup(ob, "Options", "", "TPOptions");
	tp_occlude = check(ov, T("Only what is seen (not through the mesh)"), true);
	tp_backfaces = check(ov, T("Faces turned away too"), false);
	HBoxContainer *mb = row1();
	tp_menu = memnew(MenuButton);
	tp_menu->set_name("TPMenu");
	tp_menu->set_text(T("Texture") + String(" ") + String::chr(0x25BE));
	tp_menu->set_flat(true);
	PopupMenu *pm = tp_menu->get_popup();
	pm->add_item("Save the Pictures", 0);
	pm->add_separator();
	pm->add_item("Fill the Picture", 1);
	pm->add_item("Clear the Picture", 2);
	pm->add_separator();
	pm->add_item("Show the Texture", 3);
	pm->add_item("Stencil to the Middle", 4);
	pm->connect("id_pressed", callable_mp(this, &HewnDock::_on_tp_menu));
	mb->add_child(tp_menu);
	// the second row: the brush
	HBoxContainer *cb = row2();
	PanelContainer *pair = memnew(PanelContainer);
	{
		Ref<StyleBoxEmpty> ps;
		ps.instantiate();
		ps->set_content_margin_all(0);
		pair->add_theme_stylebox_override("panel", ps);
	}
	pair->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	HBoxContainer *ph = memnew(HBoxContainer);
	ph->add_theme_constant_override("separation", 0);
	pair->add_child(ph);
	cb->add_child(pair);
	auto swatch = [&](const String &p_name, const Color &p_color, const String &p_tip) {
		ColorPickerButton *c = memnew(ColorPickerButton);
		c->set_name(p_name);
		c->set_pick_color(p_color);
		c->set_flat(true);
		Ref<StyleBoxEmpty> none;
		none.instantiate();
		none->set_content_margin_all(0);
		for (const char *st : { "normal", "hover", "pressed", "focus", "disabled", "hover_pressed" }) {
			c->add_theme_stylebox_override(st, none);
		}
		c->set_custom_minimum_size(Vector2(26 * es, 18 * es));
		c->set_tooltip_text(p_tip);
		ph->add_child(c);
		return c;
	};
	tp_color = swatch("TPColor", Color(0, 0, 0), "Color (X: swap)");
	tp_color2 = swatch("TPColor2", Color(1, 1, 1), "Second color (X: swap)");
	Button *rs = memnew(Button);
	rs->set_flat(true);
	rs->set_button_icon(icon("Reload"));
	rs->set_tooltip_text("Black and white (D)");
	rs->set_theme_type_variation("FlatMenuButton");
	rs->connect("pressed", callable_mp(this, &HewnDock::_on_tp_reset));
	cb->add_child(rs);
	HBoxContainer *br = row2();
	label(br, T("Radius"));
	tp_radius_field = field(br, 1, 2000, 1, 30, "Brush radius (pixels)", true);
	tp_radius_field->set_name("TPRadius");
	tp_radius_field->connect("value_changed", callable_mp(this, &HewnDock::_on_tp_radius));
	label(br, T("Strength"));
	tp_strength_field = field(br, 0.01, 1, 0.01, 1.0, "");
	tp_strength_field->set_name("TPStrength");
	tp_strength_field->connect("value_changed", callable_mp(this, &HewnDock::_on_tp_strength));
	VBoxContainer *fv = popup(br, "Falloff", "", "TPFalloffMenu");
	label(fv, T("Falloff"));
	tp_falloff = option(fv, { "Smooth", "Sphere", "Root", "Sharp", "Linear", "Constant", "Custom Curve" }, "");
	tp_falloff->connect("item_selected", callable_mp(this, &HewnDock::_on_tp_changed).unbind(1));
	tp_falloff_curve = picker(fv, "Curve", "Custom falloff: x the distance from the middle (0) to the edge (1), y the strength");
	tp_falloff_curve->hide();
	// the pen
	VBoxContainer *pv = popup(br, "Pen", "The pen's pressure, tilt and speed", "TPPen");
	label(pv, T("Pressure"));
	tp_pr_size = check(pv, T("Size"), false);
	tp_pr_strength = check(pv, T("Strength"), true);
	label(pv, T("Pressure curve (empty: straight)"));
	tp_pr_curve = picker(pv, "Curve", "");
	label(pv, T("Tilt"));
	tp_tilt_turn = check(pv, T("Turns the stamp"), false);
	tp_tilt_flat = check(pv, T("Flattens the stamp"), false);
	label(pv, T("Speed (faster: less)"));
	tp_speed_size = check(pv, T("Size"), false);
	tp_speed_strength = check(pv, T("Strength"), false);
	HBoxContainer *sfr = memnew(HBoxContainer);
	pv->add_child(sfr);
	label(sfr, T("Fastest (px/s)"));
	tp_speed_full = field(sfr, 50, 20000, 10, 2000, "");
	// the stamp
	VBoxContainer *sv = popup(br, "Stamp", "A picture as the brush's tip, its spacing and its chance", "TPStamp");
	label(sv, T("Stamp picture (empty: round)"));
	tp_stamp = picker(sv, "Texture2D", "");
	tp_stamp->set_name("TPStampPicture");
	tp_stamp_colors = check(sv, T("Its own colors"), false);
	tp_follow = check(sv, T("Turns along the stroke"), false);
	GridContainer *sg = memnew(GridContainer);
	sg->set_columns(2);
	sv->add_child(sg);
	label(sg, T("Spacing (%)"));
	tp_spacing = field(sg, 1, 500, 1, 15, "", true);
	label(sg, T("Angle"));
	tp_angle = field(sg, -360, 360, 1, 0, "");
	label(sg, T("Random angle"));
	tp_rand_angle = field(sg, 0, 180, 1, 0, "");
	label(sg, T("Scatter (%)"));
	tp_scatter = field(sg, 0, 300, 1, 0, "", true);
	label(sg, T("Random size (%)"));
	tp_rand_size = field(sg, 0, 100, 1, 0, "", true);
	label(sg, T("Random strength (%)"));
	tp_rand_strength = field(sg, 0, 100, 1, 0, "", true);
	HBoxContainer *bb = row2();
	tp_blend = option(bb, { "Mix", "Add", "Subtract", "Multiply", "Lighten", "Darken" }, "");
	tp_blend->set_name("TPBlend");
	// the stencil
	HBoxContainer *stb = row2();
	tp_stencil_btn = memnew(Button);
	tp_stencil_btn->set_flat(true);
	tp_stencil_btn->set_toggle_mode(true);
	tp_stencil_btn->set_button_icon(icon("tp_stencil"));
	tp_stencil_btn->set_tooltip_text("Stencil: a picture over the view, painted onto the mesh where the brush goes (Alt+drag: move, Alt+Shift+drag: scale, Alt+Ctrl+drag: turn)");
	tp_stencil_btn->set_name("TPStencilOn");
	tp_stencil_btn->set_theme_type_variation("FlatMenuButton");
	tp_stencil_btn->connect("toggled", callable_mp(this, &HewnDock::_on_tp_changed).unbind(1));
	stb->add_child(tp_stencil_btn);
	VBoxContainer *stv = popup(stb, "Stencil", "", "TPStencilMenu");
	label(stv, T("Stencil picture"));
	tp_stencil = picker(stv, "Texture2D", "");
	tp_stencil->set_name("TPStencilPicture");
	HBoxContainer *sor = memnew(HBoxContainer);
	stv->add_child(sor);
	label(sor, T("Opacity"));
	tp_stencil_opacity = field(sor, 0, 1, 0.01, 0.5, "How see-through it is drawn (not how it paints)");
	tp_stencil_opacity->connect("value_changed", callable_mp(this, &HewnDock::_on_tp_changed).unbind(1));
}

void HewnDock::sync_tpaint_bar() {
	const bool on = plugin && plugin->tpaint && paint_bar_shown && !toolbar_folded;
	for (Control *c : tp_row1_blocks) {
		c->set_visible(on);
	}
	for (Control *c : tp_blocks) {
		c->set_visible(on);
	}
	if (tp_uv && tp_into) {
		tp_uv->set_visible(tp_into->get_selected() == 1); // (the albedo: UV1 only)
	}
	if (tp_falloff_curve && tp_falloff) {
		tp_falloff_curve->set_visible(tp_falloff->get_selected() == 6);
	}
	if (tp_radius_field && plugin) {
		tp_radius_field->set_value_no_signal(plugin->tp_radius);
		tp_strength_field->set_value_no_signal(plugin->tp_strength);
	}
}

void HewnDock::_on_tp_changed() {
	sync_tpaint_bar();
	if (tp_falloff_curve && tp_falloff && tp_falloff->get_selected() == 6 && Ref<Curve>(tp_falloff_curve->get_edited_resource()).is_null()) {
		Ref<Curve> c;
		c.instantiate();
		c->add_point(Vector2(0, 1));
		c->add_point(Vector2(1, 0));
		tp_falloff_curve->set_edited_resource(c);
	}
	if (plugin) {
		plugin->update_overlays();
	}
}

void HewnDock::_on_tp_target(int p_unused) {
	sync_tpaint_bar();
	if (plugin) {
		plugin->tp_settings_changed();
	}
}

void HewnDock::_on_tp_menu(int p_id) {
	if (plugin) {
		plugin->tp_menu(p_id);
	}
}

void HewnDock::_on_tp_swap() {
	const Color a = tp_color->get_pick_color();
	tp_color->set_pick_color(tp_color2->get_pick_color());
	tp_color2->set_pick_color(a);
}

void HewnDock::_on_tp_reset() {
	tp_color->set_pick_color(Color(0, 0, 0));
	tp_color2->set_pick_color(Color(1, 1, 1));
}

void HewnDock::_on_tp_radius(double p_value) {
	if (plugin) {
		plugin->tp_radius = (float)p_value;
		plugin->update_overlays();
	}
}

void HewnDock::_on_tp_strength(double p_value) {
	if (plugin) {
		plugin->tp_strength = (float)p_value;
		plugin->update_overlays();
	}
}
