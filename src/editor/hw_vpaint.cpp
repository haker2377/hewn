// Hewn — Vertex Paint mode: a mode of its own next to Object and Edit Mode, with its
// header row (two colors like Krita's, the channels painted, the channel / format / blend, the brush), its
// tools (brush, pencil, fill, eyedropper, normals, the selection tools), color squares over the vertices,
// the selection as a mask, and views of the painted channel / of the smooth and flat shading.
#include "../hw_cutter.h"
#include "hw_editor.h"
#include "hw_editor_util.h"
#include "hw_textures.h"

#include <godot_cpp/classes/button_group.hpp>
#include <godot_cpp/classes/concave_polygon_shape3d.hpp>
#include <godot_cpp/classes/convex_polygon_shape3d.hpp>
#include <godot_cpp/classes/height_map_shape3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/collision_polygon3d.hpp>
#include <godot_cpp/classes/collision_object3d.hpp>
#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/sphere_shape3d.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/classes/capsule_shape3d.hpp>
#include <godot_cpp/classes/capsule_mesh.hpp>
#include <godot_cpp/classes/cylinder_shape3d.hpp>
#include <godot_cpp/classes/cylinder_mesh.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include "../hw_scatter.h"
#include <godot_cpp/classes/csg_shape3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/color_picker_button.hpp>
#include <godot_cpp/classes/editor_file_dialog.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_resource_picker.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/popup_panel.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/v_separator.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>

#include <map>
#include <unordered_map>
#include <unordered_set>

#include <functional>

using namespace godot;
using namespace hw_editor;

namespace {

// Godot's custom formats: their names, bytes per vertex and values
const char *FORMAT_NAMES[8] = { "RGBA8 Unorm", "RGBA8 Snorm", "RG Half", "RGBA Half", "R Float", "RG Float", "RGB Float", "RGBA Float" };
const int FORMAT_BYTES[8] = { 4, 4, 4, 8, 4, 8, 12, 16 };
const int FORMAT_COMPS[8] = { 4, 4, 2, 4, 1, 2, 3, 4 };
const Color CHANNEL_COLORS[4] = { Color(0.95f, 0.3f, 0.3f), Color(0.35f, 0.88f, 0.35f), Color(0.4f, 0.58f, 1.0f), Color(0.78f, 0.78f, 0.78f) };
const Color BAR_BACK(0.16f, 0.16f, 0.17f);

Color corner_value(const hewn::Face &F, int i, int ch) {
	if (ch <= 0) {
		return i < (int)F.col.size() ? F.col[i] : Color(1, 1, 1, 1);
	}
	return F.cust.size() >= (size_t)(i + 1) * 4 ? F.cust[i * 4 + ch - 1] : Color(0, 0, 0, 0);
}

uint64_t color_key(const Color &c) {
	auto q = [](float x) { return (uint64_t)(uint16_t)(int16_t)std::lround(CLAMP(x, -30.0f, 30.0f) * 1000.0f); };
	return q(c.r) | (q(c.g) << 16) | (q(c.b) << 32) | (q(c.a) << 48);
}

int g_falloff_type = 0; // (the brush's: Smooth, Sphere, Root, Sharp, Linear, Constant, Custom Curve)
uint64_t g_falloff_curve = 0; // (an id: no Ref left after the engine at exit)

float smooth_falloff(float d, float r) {
	if (d >= r) {
		return 0.0f;
	}
	const float k = 1.0f - d / r;
	switch (g_falloff_type) {
		case 1:
			return std::sqrt(MAX(0.0f, 1.0f - (1.0f - k) * (1.0f - k)));
		case 2:
			return std::sqrt(k);
		case 3:
			return k * k;
		case 4:
			return k;
		case 5:
			return 1.0f;
		case 6:
			return HewnEditorPlugin::custom_falloff(Ref<Curve>(Object::cast_to<Curve>(ObjectDB::get_instance(ObjectID(g_falloff_curve)))), d / r);
		default:
			return k * k * (3.0f - 2.0f * k);
	}
}

float seg_d(const Vector2 &p, const Vector2 &a, const Vector2 &b) {
	const Vector2 ab = b - a;
	const float l = ab.length_squared();
	const float t = l > 1e-12f ? CLAMP((p - a).dot(ab) / l, 0.0f, 1.0f) : 0.0f;
	return p.distance_to(a + ab * t);
}

float seg_dist3(const Vector3 &p, const Vector3 &a, const Vector3 &b) {
	const Vector3 ab = b - a;
	const float l = ab.length_squared();
	const float t = l > 1e-12f ? CLAMP((p - a).dot(ab) / l, 0.0f, 1.0f) : 0.0f;
	return p.distance_to(a + ab * t);
}

bool in_poly(const std::vector<Vector2> &poly, const Vector2 &q) {
	bool inside = false;
	const size_t n = poly.size();
	for (size_t i = 0, j = n - 1; i < n; j = i++) {
		if (((poly[i].y > q.y) != (poly[j].y > q.y)) && (q.x < (poly[j].x - poly[i].x) * (q.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)) {
			inside = !inside;
		}
	}
	return inside;
}

// The color shown for a value of so many components (one: grey, two: red / green).
Color shown_color(const Color &c, int comps) {
	Color s = comps == 1 ? Color(c.r, c.r, c.r) : comps == 2 ? Color(c.r, c.g, 0.0f) : Color(c.r, c.g, c.b);
	s.r = CLAMP(s.r, 0.0f, 1.0f);
	s.g = CLAMP(s.g, 0.0f, 1.0f);
	s.b = CLAMP(s.b, 0.0f, 1.0f);
	s.a = 1.0f;
	return s;
}

// A color square: black border, the color, the top right and bottom left quarters see-through by its alpha
// (over a checker).
const Color SELECTED_BORDER(1.0f, 0.6f, 0.1f);

// A black (or nearly) color that is not see-through: its square gets a white border.
bool dark_value(const Color &p_shown, const Color &p_value, int p_comps) {
	const float lum = p_shown.r * 0.3f + p_shown.g * 0.59f + p_shown.b * 0.11f;
	return lum < 0.08f && (p_comps != 4 || p_value.a > 0.9f);
}

// (a whole number of pixels across, its center on a pixel corner: the border even on all sides)
float square_half(float p_size) {
	return MAX(1.0f, std::round(p_size * 0.5f));
}

void draw_square(Control *p_overlay, const Vector2 &p_center, float p_size, const Color &p_value, int p_comps, bool p_selected = false) {
	const float h = square_half(p_size);
	const Vector2 c0 = p_center.round();
	const Rect2 r(c0 - Vector2(h, h), Vector2(h, h) * 2.0f);
	const Color c = shown_color(p_value, p_comps);
	const float bw = p_selected ? 2.0f : 1.0f;
	// the border as four strips around it (filled under the square, the see-through quarters would show it)
	const Color bc = p_selected ? SELECTED_BORDER : dark_value(c, p_value, p_comps) ? Color(1, 1, 1) : Color(0, 0, 0);
	const Rect2 o = r.grow(bw);
	p_overlay->draw_rect(Rect2(o.position, Vector2(o.size.x, bw)), bc);
	p_overlay->draw_rect(Rect2(Vector2(o.position.x, r.get_end().y), Vector2(o.size.x, bw)), bc);
	p_overlay->draw_rect(Rect2(Vector2(o.position.x, r.position.y), Vector2(bw, r.size.y)), bc);
	p_overlay->draw_rect(Rect2(Vector2(r.get_end().x, r.position.y), Vector2(bw, r.size.y)), bc);
	p_overlay->draw_rect(Rect2(r.position, Vector2(h, h)), c);
	p_overlay->draw_rect(Rect2(c0, Vector2(h, h)), c);
	// the top right and bottom left quarters: as see-through as the color
	const Color ca = p_comps == 4 ? Color(c.r, c.g, c.b, CLAMP(p_value.a, 0.0f, 1.0f)) : c;
	p_overlay->draw_rect(Rect2(Vector2(c0.x, r.position.y), Vector2(h, h)), ca);
	p_overlay->draw_rect(Rect2(Vector2(r.position.x, c0.y), Vector2(h, h)), ca);
}

// The same square as one of the many marks (hw_marks.cpp).
HewnEditorPlugin::Mark square_mark(const Vector2 &p_center, float p_size, const Color &p_value, int p_comps, bool p_selected) {
	HewnEditorPlugin::Mark mk;
	const Color c = shown_color(p_value, p_comps);
	const float bw = p_selected ? 2.0f : 1.0f;
	mk.at = p_center.round();
	mk.half = square_half(p_size) + bw;
	mk.color = c;
	mk.shape = 3.0f;
	mk.alpha = p_comps == 4 ? CLAMP(p_value.a, 0.0f, 1.0f) : 1.0f;
	mk.border = p_selected ? 2.0f : dark_value(c, p_value, p_comps) ? 1.0f : 0.0f;
	return mk;
}

// Out of a face corner along the bisector of its angle (on the screen): where its own square goes.
Vector2 corner_dir(const std::vector<Vector2> &sp, const hewn::Face &F, int i) {
	const int n = F.size();
	const Vector2 p = sp[F.v[i]];
	Vector2 a = sp[F.v[(i + n - 1) % n]] - p, b = sp[F.v[(i + 1) % n]] - p;
	a = a.length() > 1e-4f ? a.normalized() : Vector2();
	b = b.length() > 1e-4f ? b.normalized() : Vector2();
	Vector2 d = a + b;
	if (d.length() < 1e-3f) {
		d = Vector2(-a.y, a.x); // (a straight angle: across)
	}
	return d.length() > 1e-6f ? d.normalized() : Vector2(1, 0);
}

Button *paint_icon_button(Container *p_parent, const Ref<Texture2D> &p_icon, const String &p_tip, bool p_toggle) {
	Button *b = memnew(Button);
	b->set_flat(true);
	b->set_toggle_mode(p_toggle);
	b->set_button_icon(p_icon);
	b->set_tooltip_text(p_tip);
	b->set_theme_type_variation("FlatMenuButton");
	p_parent->add_child(b);
	return b;
}

} // namespace

// ------------------------------------------------------------------ the header row

void HewnDock::build_paint_bar(HBoxContainer *p_modes_block) {
	Ref<Theme> et = EditorInterface::get_singleton()->get_editor_theme();
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const Color base = et->has_color("dark_color_1", "Editor") ? et->get_color("dark_color_1", "Editor") : Color(0.1f, 0.11f, 0.13f);
	auto make_block = [&](Container *p_parent) {
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
		p_parent->add_child(p);
		return h;
	};
	// the mode menu: a block of its own in front of the element modes (instead of the Object button)
	PanelContainer *modes_panel = Object::cast_to<PanelContainer>(p_modes_block->get_parent());
	HBoxContainer *ib = make_block(toolbar);
	toolbar->move_child(ib->get_parent(), modes_panel ? modes_panel->get_index() : 0);
	interaction_menu = memnew(MenuButton);
	interaction_menu->set_name("InteractionMode");
	interaction_menu->set_flat(true);
	interaction_menu->set_tooltip_text("Objects (Tab)");
	interaction_menu->get_popup()->add_icon_item(icon("Object"), "Objects", 0);
	interaction_menu->get_popup()->add_icon_item(icon("Edit"), "Editing", 1);
	interaction_menu->get_popup()->add_icon_item(icon("tool_paint"), "Vertex Paint", 2);
	interaction_menu->get_popup()->add_icon_item(icon("sculpt_mode"), "Sculpt", 3);
	interaction_menu->get_popup()->connect("id_pressed", callable_mp(this, &HewnDock::_on_interaction));
	ib->add_child(interaction_menu);
	if (mode_buttons[0]) {
		mode_buttons[0]->hide();
	}
	elements_block = modes_panel;
	auto option = [&](HBoxContainer *p_block, const std::vector<String> &p_items, const String &p_tip) {
		OptionButton *o = memnew(OptionButton);
		for (const String &it : p_items) {
			o->add_item(it);
		}
		o->set_flat(true);
		o->set_fit_to_longest_item(false);
		o->set_tooltip_text(p_tip);
		o->connect("item_selected", callable_mp(this, &HewnDock::_on_paint_changed).unbind(1));
		p_block->add_child(o);
		return o;
	};
	auto toggle = [&](HBoxContainer *p_block, const String &p_icon, const String &p_tip, bool p_on) {
		Button *b = paint_icon_button(p_block, icon(p_icon), p_tip, true);
		b->set_pressed(p_on);
		b->connect("toggled", callable_mp(this, &HewnDock::_on_paint_changed).unbind(1));
		return b;
	};
	// first row, after the element modes (Vertex Paint only): what the mesh shows, the channel and its format,
	// the squares
	int at = modes_panel ? modes_panel->get_index() + 1 : 1;
	auto row1_block = [&]() {
		HBoxContainer *h = make_block(toolbar);
		toolbar->move_child(h->get_parent(), at++);
		paint_row1_blocks.push_back(Object::cast_to<Control>(h->get_parent()));
		h->get_parent()->call("hide");
		return h;
	};
	HBoxContainer *vb = row1_block();
	Ref<ButtonGroup> vg;
	vg.instantiate();
	static const char *view_icons[4] = { "view_material", "view_channel", "view_normals", "view_normal_colors" };
	static const char *view_tips[4] = { "Materials", "Painted Channel", "Smooth / Flat", "Normals as Colors" };
	for (int i = 0; i < 4; i++) {
		Button *b = paint_icon_button(vb, icon(view_icons[i]), view_tips[i], true);
		b->set_button_group(vg);
		b->set_pressed(i == 0);
		b->connect("toggled", callable_mp(this, &HewnDock::_on_paint_changed).unbind(1));
		paint_view_btns[i] = b;
	}
	HBoxContainer *ch = row1_block();
	paint_channel = option(ch, { "Color", "CUSTOM0", "CUSTOM1", "CUSTOM2", "CUSTOM3" },
			"Channel");
	paint_channel->connect("item_selected", callable_mp(this, &HewnDock::_on_paint_channel));
	std::vector<String> fmts;
	for (int i = 0; i < 8; i++) {
		fmts.push_back(vformat("%s (%d B)", FORMAT_NAMES[i], FORMAT_BYTES[i]));
	}
	paint_format = option(ch, fmts, "Storage format");
	paint_format->select(0); // (the smallest)
	paint_format->connect("item_selected", callable_mp(this, &HewnDock::_on_paint_format));
	paint_format->hide(); // (the vertex color's is fixed)
	// the channel view with alpha: what of it shows (the color always, the alpha as stripes / only the color /
	// only the alpha)
	HBoxContainer *ab = row1_block();
	paint_alpha_block = Object::cast_to<Control>(ab->get_parent());
	Ref<ButtonGroup> ag;
	ag.instantiate();
	static const char *alpha_names[3] = { "RGBA", "RGB", "A" };
	static const char *alpha_tips[3] = { "Color and alpha", "Color", "Alpha" };
	for (int i = 0; i < 3; i++) {
		Button *b = memnew(Button);
		b->set_text(alpha_names[i]);
		b->set_flat(true);
		b->set_toggle_mode(true);
		b->set_button_group(ag);
		b->set_pressed(i == 0);
		b->set_tooltip_text(alpha_tips[i]);
		b->set_theme_type_variation("FlatMenuButton");
		b->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);
		b->connect("toggled", callable_mp(this, &HewnDock::_on_paint_changed).unbind(1));
		ab->add_child(b);
		paint_alpha_btns[i] = b;
	}
	HBoxContainer *sh = row1_block();
	paint_squares_btn = toggle(sh, "vp_squares", "Color squares", true);
	paint_squares_near_btn = toggle(sh, "vp_squares_near", "Only the squares the brush reaches", false);
	paint_squares_near_btn->set_name("SquaresNear");
	paint_split_btn = toggle(sh, "vp_split", "Split vertices", false);

	// the second row: scrolled with the wheel on its own
	paint_root = memnew(ScrollContainer);
	paint_root->set_name("HewnPaintRow");
	paint_root->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	paint_root->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_SHOW_NEVER);
	paint_root->add_theme_stylebox_override("panel", memnew(StyleBoxEmpty));
	paint_root->connect("gui_input", callable_mp(this, &HewnDock::_on_paint_row_input));
	paint_root->hide();
	paint_row = memnew(HBoxContainer);
	paint_row->add_theme_constant_override("separation", 6);
	paint_root->add_child(paint_row);
	auto block = [&]() {
		HBoxContainer *h = make_block(paint_row);
		paint_blocks.push_back(Object::cast_to<Control>(h->get_parent()));
		return h;
	};
	auto label = [&](HBoxContainer *p_block, const String &p_text) {
		Label *l = memnew(Label);
		l->set_text(p_text);
		l->add_theme_color_override("font_color", Color(0.75f, 0.75f, 0.75f));
		p_block->add_child(l);
	};
	auto field = [&](HBoxContainer *p_block, double p_min, double p_max, double p_step, double p_value, const String &p_tip) {
		HewnDragField *f = memnew(HewnDragField);
		f->setup(p_min, p_max, p_step, false, p_value);
		f->set_custom_minimum_size(Vector2(52 * es, 0));
		f->set_tooltip_text(p_tip);
		p_block->add_child(f);
		return f;
	};
	auto popup_button = [&](HBoxContainer *p_block, const String &p_text, const String &p_tip, VBoxContainer *&r_box) {
		PopupPanel *pp = memnew(PopupPanel);
		r_box = memnew(VBoxContainer);
		pp->add_child(r_box);
		paint_row->add_child(pp);
		Button *b = memnew(Button);
		b->set_text(T(p_text) + String(" ") + String::chr(0x25BE));
		b->set_flat(true);
		b->set_tooltip_text(p_tip);
		b->connect("pressed", callable_mp(this, &HewnDock::_popup_under).bind(pp, b));
		p_block->add_child(b);
	};
	// the two colors in one box split down the middle; X swaps them, the button resets them
	HBoxContainer *cb = block();
	PanelContainer *pair = memnew(PanelContainer);
	{
		Ref<StyleBoxEmpty> ps; // (no border: one half one color, the other half the other)
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
			c->add_theme_stylebox_override(st, none); // (the color to the edges)
		}
		c->set_custom_minimum_size(Vector2(26 * es, 18 * es));
		c->set_tooltip_text(p_tip);
		c->connect("color_changed", callable_mp(this, &HewnDock::_on_paint_changed).unbind(1));
		ph->add_child(c);
		return c;
	};
	color_picker = swatch("VertexColor", Color(0, 0, 0), "Color (X: swap)");
	paint_color2 = swatch("VertexColor2", Color(1, 1, 1), "Second color (X: swap)");
	Button *rs = paint_icon_button(cb, icon("Reload"), "Black and white (D)", false);
	rs->connect("pressed", callable_mp(this, &HewnDock::_on_paint_reset));
	// the channels painted (Ctrl+click: only that one / all again)
	HBoxContainer *mk = block();
	static const char *names[4] = { "R", "G", "B", "A" };
	for (int k = 0; k < 4; k++) {
		Button *b = memnew(Button);
		b->set_toggle_mode(true);
		b->set_pressed(true);
		b->set_text(names[k]);
		b->set_custom_minimum_size(Vector2(22 * es, 0));
		auto style = [&](const Color &p_bg, const Color &p_border) {
			Ref<StyleBoxFlat> st;
			st.instantiate();
			st->set_bg_color(p_bg);
			st->set_border_color(p_border);
			st->set_border_width_all(1);
			st->set_corner_radius_all(3);
			st->set_content_margin_all(2);
			return st;
		};
		const Color on = CHANNEL_COLORS[k], off_bg(0.13f, 0.13f, 0.14f);
		b->add_theme_stylebox_override("normal", style(off_bg, on * Color(0.45f, 0.45f, 0.45f)));
		b->add_theme_stylebox_override("hover", style(off_bg.lightened(0.08f), on * Color(0.6f, 0.6f, 0.6f)));
		b->add_theme_stylebox_override("pressed", style(on, on.lightened(0.3f)));
		b->add_theme_stylebox_override("hover_pressed", style(on.lightened(0.12f), on.lightened(0.4f)));
		b->add_theme_stylebox_override("focus", memnew(StyleBoxEmpty));
		b->add_theme_color_override("font_color", Color(0.42f, 0.42f, 0.42f));
		b->add_theme_color_override("font_hover_color", Color(0.6f, 0.6f, 0.6f));
		b->add_theme_color_override("font_pressed_color", Color(0.05f, 0.05f, 0.05f));
		b->add_theme_color_override("font_hover_pressed_color", Color(0.05f, 0.05f, 0.05f));
		b->set_tooltip_text(vformat(T("Paint the %s channel"), names[k]));
		b->connect("toggled", callable_mp(this, &HewnDock::_on_paint_mask).bind(k));
		mk->add_child(b);
		paint_mask_buttons[k] = b;
	}
	// the brush: radius, strength, falloff (and the pen), the cursor
	HBoxContainer *br = block();
	label(br, T("Radius"));
	paint_radius_field = field(br, 2, 2000, 1, 40, "Brush radius");
	paint_radius_field->connect("value_changed", callable_mp(this, &HewnDock::_on_paint_radius));
	label(br, T("Strength"));
	paint_strength_field = field(br, 0.01, 1, 0.01, 0.5, "");
	paint_strength_field->set_name("BrushStrength");
	paint_strength_field->connect("value_changed", callable_mp(this, &HewnDock::_on_paint_strength));
	VBoxContainer *fv;
	popup_button(br, "Falloff", "", fv);
	Label *fl = memnew(Label);
	fl->set_text("Falloff");
	fv->add_child(fl);
	paint_falloff_type = memnew(OptionButton);
	for (const char *it : { "Smooth", "Sphere", "Root", "Sharp", "Linear", "Constant", "Custom Curve" }) {
		paint_falloff_type->add_item(it);
	}
	paint_falloff_type->connect("item_selected", callable_mp(this, &HewnDock::_on_paint_changed).unbind(1));
	fv->add_child(paint_falloff_type);
	paint_falloff_curve_picker = memnew(EditorResourcePicker);
	paint_falloff_curve_picker->set_base_type("Curve");
	paint_falloff_curve_picker->set_custom_minimum_size(Vector2(180 * es, 0));
	paint_falloff_curve_picker->set_tooltip_text("Custom falloff: x the distance from the middle (0) to the edge (1), y the strength");
	paint_falloff_curve_picker->connect("resource_changed", callable_mp(this, &HewnDock::_on_paint_changed).unbind(1));
	paint_falloff_curve_picker->hide();
	fv->add_child(paint_falloff_curve_picker);
	paint_pressure_check = memnew(CheckBox);
	paint_pressure_check->set_text("Pen pressure: strength");
	paint_pressure_check->set_pressed(true);
	fv->add_child(paint_pressure_check);
	Label *cl = memnew(Label);
	cl->set_text("Pressure curve (empty: straight)");
	fv->add_child(cl);
	paint_curve_picker = memnew(EditorResourcePicker);
	paint_curve_picker->set_base_type("Curve");
	paint_curve_picker->set_custom_minimum_size(Vector2(180 * es, 0));
	paint_curve_picker->connect("resource_changed", callable_mp(this, &HewnDock::_on_paint_changed).unbind(1));
	fv->add_child(paint_curve_picker);
	VBoxContainer *cv;
	popup_button(br, "Cursor", "", cv);
	Label *cvl = memnew(Label);
	cvl->set_text("Cursor");
	cv->add_child(cvl);
	paint_falloff = memnew(OptionButton);
	paint_falloff->add_item("Screen");
	paint_falloff->add_item("Surface");
	paint_falloff->select(1);
	paint_falloff->connect("item_selected", callable_mp(this, &HewnDock::_on_paint_changed).unbind(1));
	cv->add_child(paint_falloff);
	paint_circle_check = memnew(CheckBox);
	paint_circle_check->set_text("Show the cursor");
	paint_circle_check->set_pressed(true);
	paint_circle_check->connect("toggled", callable_mp(this, &HewnDock::_on_paint_changed).unbind(1));
	cv->add_child(paint_circle_check);
	paint_bars_check = memnew(CheckBox);
	paint_bars_check->set_text("Show the color's channels under it");
	paint_bars_check->set_pressed(true);
	paint_bars_check->connect("toggled", callable_mp(this, &HewnDock::_on_paint_changed).unbind(1));
	cv->add_child(paint_bars_check);
	paint_circle_picker = memnew(ColorPickerButton);
	paint_circle_picker->set_pick_color(Color(0.0f, 128.0f / 255.0f, 1.0f, 0.9f));
	paint_circle_picker->set_custom_minimum_size(Vector2(120 * es, 24 * es));
	paint_circle_picker->connect("color_changed", callable_mp(this, &HewnDock::_on_paint_changed).unbind(1));
	cv->add_child(paint_circle_picker);
	// how the color goes on
	HBoxContainer *bl = block();
	paint_scale = field(bl, -1e6, 1e6, 0.01, 1.0, "Color values multiplied by this");
	paint_blend = option(bl, { "Mix", "Add", "Subtract", "Multiply", "Lighten", "Darken" }, "");
	// more: fill the selection, replace / round colors, save / load the painting
	HBoxContainer *mo = block();
	paint_more = memnew(MenuButton);
	paint_more->set_text(T("Colors") + String(" ") + String::chr(0x25BE));
	paint_more->set_flat(true);
	paint_more->get_popup()->add_item("Fill the Selection", 0);
	paint_more->get_popup()->add_item("Replace Color...", 1);
	paint_more->get_popup()->add_item("Round Colors...", 4);
	paint_more->get_popup()->add_item("Bake Light...", 5);
	paint_more->get_popup()->add_separator();
	paint_more->get_popup()->add_item("Save the Painting...", 2);
	paint_more->get_popup()->add_item("Load a Painting...", 3);
	paint_more->get_popup()->connect("id_pressed", callable_mp(this, &HewnDock::_on_paint_more));
	mo->add_child(paint_more);
}

// Ctrl+click on a channel: only it painted; on the only one: all again.
void HewnDock::_on_paint_mask(bool p_on, int p_k) {
	if (Input::get_singleton()->is_key_pressed(KEY_CTRL) || Input::get_singleton()->is_key_pressed(KEY_META)) {
		bool others = false;
		for (int k = 0; k < 4; k++) {
			others = others || (k != p_k && paint_mask_buttons[k]->is_pressed());
		}
		for (int k = 0; k < 4; k++) {
			paint_mask_buttons[k]->set_pressed_no_signal(others ? k == p_k : true);
		}
	}
	_on_paint_changed();
}

// The header's blocks of the mode: in Vertex Paint the painting ones (the mode, the element modes, X-ray and
// the overlays stay).
void HewnDock::show_paint_bar(bool p_on) {
	paint_bar_shown = p_on;
	if (paint_root) {
		paint_root->set_visible(p_on && !toolbar_folded);
	}
	if (toolbar_folded) {
		return; // (the header is folded: all of it stays hidden)
	}
	for (Control *c : edit_blocks) {
		c->set_visible(!p_on);
	}
	const bool sc = plugin && plugin->sculpt;
	for (Control *c : paint_row1_blocks) {
		c->set_visible(p_on && !sc);
	}
	for (Control *c : paint_blocks) {
		c->set_visible(p_on && !sc);
	}
	for (Control *c : sculpt_blocks) {
		c->set_visible(p_on && sc);
	}
	sync_sculpt_bar();
	const bool objects = plugin && plugin->mode == HewnEditorPlugin::MODE_OBJECT && !p_on;
	if (elements_block) {
		elements_block->set_visible(!objects); // (no vertices / edges / faces in object mode)
	}
	if (mesh_menu && mesh_menu->get_parent() && mesh_menu->get_parent()->get_parent()) {
		Object::cast_to<Control>(mesh_menu->get_parent()->get_parent())->set_visible(!p_on && !objects);
	}
	if (paint_format && paint_channel) {
		paint_format->set_visible(paint_channel->get_selected() > 0); // (the vertex color: always RGBA8)
	}
	if (paint_alpha_block && plugin) {
		paint_alpha_block->set_visible(p_on && plugin->paint_view == 1 && plugin->_paint_comps() == 4); // (the channel view with alpha only)
	}
	if (interaction_menu && plugin) {
		const int m = plugin->sculpt ? 3 : plugin->vpaint ? 2 : plugin->mode == HewnEditorPlugin::MODE_OBJECT ? 0 : 1;
		static const char *icons[4] = { "Object", "Edit", "tool_paint", "sculpt_mode" };
		interaction_menu->set_button_icon(icon(icons[m]));
		static const char *tips[4] = { "Objects", "Editing", "Vertex Paint", "Sculpt" };
		interaction_menu->set_tooltip_text(T(tips[m]) + (m < 2 ? String(" (Tab)") : String()));
		interaction_menu->set_text(String::chr(0x25BE));
	}
}

void HewnDock::_on_paint_row_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_null() || !mb->is_pressed() || !paint_root) {
		return;
	}
	const MouseButton b = mb->get_button_index();
	const int step = b == MOUSE_BUTTON_WHEEL_UP || b == MOUSE_BUTTON_WHEEL_LEFT ? -60 : b == MOUSE_BUTTON_WHEEL_DOWN || b == MOUSE_BUTTON_WHEEL_RIGHT ? 60 : 0;
	if (step != 0) {
		paint_root->set_h_scroll(MAX(0, paint_root->get_h_scroll() + step));
		paint_root->accept_event();
	}
}

void HewnDock::_on_interaction(int p_id) {
	if (!plugin) {
		return;
	}
	if (p_id == 2) {
		plugin->enter_vpaint();
	} else if (p_id == 3) {
		plugin->enter_sculpt();
	} else {
		if (plugin->vpaint) {
			plugin->exit_vpaint();
		}
		if (plugin->sculpt) {
			plugin->exit_sculpt();
		}
		plugin->set_mode(p_id == 0 ? HewnEditorPlugin::MODE_OBJECT : plugin->element_mode);
	}
	refresh();
}

void HewnDock::_on_paint_swap() {
	const Color a = color_picker->get_pick_color();
	color_picker->set_pick_color(paint_color2->get_pick_color());
	paint_color2->set_pick_color(a);
	_on_paint_changed();
}

void HewnDock::_on_paint_reset() {
	color_picker->set_pick_color(Color(0, 0, 0));
	paint_color2->set_pick_color(Color(1, 1, 1));
	_on_paint_changed();
}

void HewnDock::_on_paint_changed() {
	if (!plugin) {
		return;
	}
	plugin->paint_pressure_curve = paint_curve_picker ? Ref<Curve>(paint_curve_picker->get_edited_resource()) : Ref<Curve>();
	if (paint_squares_near_btn && paint_squares_btn) {
		paint_squares_near_btn->set_visible(paint_squares_btn->is_pressed());
	}
	if (paint_falloff_curve_picker && paint_falloff_type) {
		const bool custom = paint_falloff_type->get_selected() == 6;
		paint_falloff_curve_picker->set_visible(custom);
		if (custom && Ref<Curve>(paint_falloff_curve_picker->get_edited_resource()).is_null()) {
			Ref<Curve> c; // (a curve to start from: full in the middle, nothing at the edge)
			c.instantiate();
			c->add_point(Vector2(0, 1));
			c->add_point(Vector2(1, 0));
			paint_falloff_curve_picker->set_edited_resource(c);
		}
	}
	int view = 0;
	for (int i = 0; i < 4; i++) {
		view = paint_view_btns[i] && paint_view_btns[i]->is_pressed() ? i : view;
	}
	if (paint_format && paint_channel) {
		paint_format->set_visible(paint_channel->get_selected() > 0);
	}
	int alpha_view = 0;
	for (int i = 0; i < 3; i++) {
		alpha_view = paint_alpha_btns[i] && paint_alpha_btns[i]->is_pressed() ? i : alpha_view;
	}
	plugin->paint_alpha_view = alpha_view;
	if (paint_alpha_block && plugin->vpaint) {
		paint_alpha_block->set_visible(view == 1 && plugin->_paint_comps() == 4);
	}
	if (view != plugin->paint_view || view == 1) {
		plugin->paint_view = view;
		plugin->apply_paint_view(); // (the channel shown follows the channel picked)
	}
	sync_shading();
	update_all_gizmos(); // (the normals view's faces, on every mesh outside edit mode; the vertices under the squares)
	plugin->update_overlays();
}

// The header's Shading menu: a view picked there is the one of Vertex Paint's buttons too.
void HewnDock::_on_shading(int p_id) {
	if (p_id >= 0 && p_id < 4 && paint_view_btns[p_id]) {
		paint_view_btns[p_id]->set_pressed(true); // (its toggled: _on_paint_changed)
		_on_paint_changed();
	} else if (p_id >= 4 && p_id < 7 && plugin) {
		for (Button *b : paint_view_btns) {
			if (b) {
				b->set_pressed_no_signal(false);
			}
		}
		plugin->paint_view = p_id;
		plugin->apply_paint_view();
		sync_shading();
		update_all_gizmos(); // (Smooth / Flat is drawn on every mesh outside edit mode)
		plugin->update_overlays();
	}
}

void HewnEditorPlugin::_apply_solid_params() {
	const HwMeshStyle &st = hw_mesh_style();
	if (collision_view_mat.is_valid()) {
		collision_view_mat->set_shader_parameter("world_light", st.world_light);
		collision_view_mat->set_shader_parameter("world_light_angle", st.world_light_angle);
	}
	for (Ref<ShaderMaterial> &mat : solid_view_mat) {
		if (mat.is_valid()) {
			mat->set_shader_parameter("single_color", Vector4(st.single.r, st.single.g, st.single.b, 1.0f)); // (a Color would be made linear)
			mat->set_shader_parameter("world_light", st.world_light);
			mat->set_shader_parameter("world_light_angle", st.world_light_angle);
			if (Ref<ShaderMaterial> cm = mat->get_next_pass(); cm.is_valid()) {
				cm->set_shader_parameter("cavity_ridge", st.cavity_ridge);
				cm->set_shader_parameter("cavity_valley", st.cavity_valley);
				cm->set_shader_parameter("cavity_type", st.cavity_type);
			}
		}
	}
}

std::vector<HewnMesh3D *> HewnEditorPlugin::_view_meshes() const {
	std::vector<HewnMesh3D *> out = _scene_meshes();
	// the shape being drawn (an internal child, not found above); not the cutter's see-through one
	HewnMesh3D *pv = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(shape_preview_id)));
	if (pv && pv->is_inside_tree() && pv->is_visible_in_tree() && pv->get_material_override().is_null()) {
		out.push_back(pv);
	}
	// the cutters' linings (internal children: the corridor / alcove), shown like the meshes around them
	if (Node *root = EditorInterface::get_singleton()->get_edited_scene_root()) {
		std::vector<Node *> stack = { root };
		while (!stack.empty()) {
			Node *x = stack.back();
			stack.pop_back();
			if (HewnCutter3D *c = Object::cast_to<HewnCutter3D>(x)) {
				if (HewnMesh3D *lm = c->get_lining_mesh()) {
					if (lm->is_inside_tree()) {
						out.push_back(lm);
					}
				}
			}
			for (int i = 0; i < x->get_child_count(); i++) {
				stack.push_back(x->get_child(i));
			}
		}
	}
	return out;
}

void HewnEditorPlugin::_on_node_added(Node *p_node) {
	if ((paint_view == 1 || paint_view >= 3) && Object::cast_to<HewnMesh3D>(p_node)) {
		// (once it has its place in the scene: its instance and visibility)
		callable_mp(this, &HewnEditorPlugin::_view_new_mesh_id).call_deferred((uint64_t)p_node->get_instance_id());
	}
}

void HewnEditorPlugin::_view_new_mesh_id(uint64_t p_id) {
	HewnMesh3D *m = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(p_id)));
	if (m && m->is_inside_tree() && (m->is_visible_in_tree() || m->has_meta("_hw_lining"))) {
		_view_new_mesh(m); // (a cutter's lining: made hidden, shown once it has faces)
	}
}

void HewnEditorPlugin::_view_new_mesh(HewnMesh3D *p_mesh) {
	if (p_mesh && (paint_view == 1 || paint_view >= 3) &&
			std::find(paint_view_ids.begin(), paint_view_ids.end(), ObjectID(p_mesh->get_instance_id())) == paint_view_ids.end()) {
		apply_paint_view();
	}
}

void HewnDock::sync_shading() {
	if (!shading_menu || !plugin) {
		return;
	}
	const int v = CLAMP(plugin->paint_view, 0, 6);
	if (collision_legend) {
		collision_legend->set_visible(v == 6);
	}
	if (attr_channel) {
		attr_channel->set_visible(v == 1);
		if (paint_channel && attr_channel->get_selected() != paint_channel->get_selected()) {
			attr_channel->select(paint_channel->get_selected());
		}
	}
	if (solid_opts) {
		solid_opts->set_visible(v == 4 || v == 5 || v == 6);
		cavity_check->set_visible(v == 4);
		cavity_opts->set_visible(v == 4 && cavity_check->is_pressed());
		if (shading_popup && shading_popup->is_visible()) {
			shading_popup->reset_size();
		}
	}
	for (int i = 0; i < 7; i++) {
		if (shading_btns[i]) {
			shading_btns[i]->set_pressed_no_signal(i == v);
		}
	}
	if (shading_popup && shading_popup->is_visible()) {
		shading_popup->reset_size();
	}
	static const char *icons[7] = { "view_material", "view_channel", "view_normals", "view_normal_colors", "view_single", "view_random", "view_collision" };
	shading_menu->set_button_icon(icon(icons[v]));
}

void HewnDock::_on_paint_radius(double p_value) {
	if (plugin) {
		plugin->paint_radius = (float)p_value;
		plugin->update_overlays();
	}
}

void HewnDock::_on_paint_strength(double p_value) {
	if (plugin) {
		plugin->paint_strength = (float)p_value;
		plugin->update_overlays();
	}
}

void HewnDock::_on_paint_more(int p_id) {
	if (plugin) {
		plugin->paint_more_op(p_id);
	}
}

void HewnDock::_on_isolate(bool p_on) {
	if (plugin) {
		plugin->set_isolate(p_on);
	}
}

// ------------------------------------------------------------------ the mode

void HewnEditorPlugin::enter_vpaint() {
	HewnMesh3D *n = get_edited();
	if (!n) {
		ERR_PRINT("Hewn: select a Hewn mesh to paint it.");
		return;
	}
	if (sculpt) {
		exit_sculpt();
		if (mode == MODE_OBJECT) {
			set_mode(element_mode);
		}
	}
	if (mode == MODE_OBJECT) {
		set_mode(element_mode);
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	vpaint = true;
	if (dock) {
		dock->show_paint_bar(true);
		if (dock->paint_radius_field) {
			dock->paint_radius_field->set_value_no_signal(paint_radius);
			dock->paint_strength_field->set_value_no_signal(paint_strength);
		}
		// the channel's format as the mesh has it
		dock->_on_paint_channel(dock->paint_channel ? dock->paint_channel->get_selected() : 0);
	}
	start_paint_tool(paint_tool == PT_PICKER || paint_tool == PT_FILL || paint_tool == PT_PENCIL || paint_tool == PT_NORMALS ? paint_tool : PT_BRUSH);
	apply_paint_view();
	for (HewnMesh3D *m : edited_meshes()) {
		m->update_gizmos();
	}
	update_overlays();
}

void HewnEditorPlugin::exit_vpaint() {
	if (!vpaint) {
		return;
	}
	if (painting) {
		painting = false;
		end_edit();
	}
	vpaint = false;
	if (modal == MODAL_PAINT) {
		modal = MODAL_NONE;
	}
	apply_paint_view();
	if (dock) {
		dock->show_paint_bar(false);
	}
	if (mode != MODE_OBJECT) {
		set_mode(MODE_OBJECT);
	}
	for (HewnMesh3D *m : edited_meshes()) {
		m->update_gizmos();
	}
	update_overlays();
}

void HewnEditorPlugin::start_paint() {
	enter_vpaint(); // (the old Vertex Paint button: the mode now)
}

void HewnEditorPlugin::start_paint_tool(int p_tool) {
	if (!vpaint) {
		enter_vpaint();
		if (!vpaint) {
			return;
		}
	}
	if (modal != MODAL_NONE && modal != MODAL_PAINT) {
		cancel_tool();
	}
	paint_tool = p_tool;
	painting = false;
	modal = MODAL_PAINT;
	update_overlays();
}

int HewnEditorPlugin::_paint_comps() const {
	const int ch = dock && dock->paint_channel ? dock->paint_channel->get_selected() : 0;
	if (ch <= 0) {
		return 4;
	}
	HewnMesh3D *n = get_edited();
	int f = n && n->custom_formats[ch - 1] >= 0 ? n->custom_formats[ch - 1] : (dock->paint_format ? dock->paint_format->get_selected() : 0);
	return FORMAT_COMPS[CLAMP(f, 0, 7)];
}

bool HewnEditorPlugin::_paint_channel_on(int p_k) const {
	return p_k < _paint_comps() && (!dock || !dock->paint_mask_buttons[p_k] || dock->paint_mask_buttons[p_k]->is_pressed());
}

// Keys of Vertex Paint (the paint tools and the selection tools alike): -1 when the edit mode keys may take it.
int32_t HewnEditorPlugin::_vpaint_keys(Camera3D *p_cam, const Ref<InputEventKey> &p_key) {
	if (p_key.is_null() || !p_key->is_pressed() || p_key->is_echo()) {
		return -1;
	}
	const Key code = key_code(p_key);
	const bool ctrl = p_key->is_ctrl_pressed() || p_key->is_meta_pressed(), shift = p_key->is_shift_pressed(), alt = p_key->is_alt_pressed();
	if (code == KEY_TAB && !ctrl && !alt && !shift) {
		exit_vpaint();
		if (dock) {
			dock->refresh();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (code == KEY_X && !ctrl && !alt && !shift && dock) {
		dock->_on_paint_swap();
		return AFTER_GUI_INPUT_STOP;
	}
	if (code == KEY_D && !ctrl && !alt && !shift && dock) {
		dock->_on_paint_reset();
		return AFTER_GUI_INPUT_STOP;
	}
	if ((code == KEY_BRACKETLEFT || code == KEY_BRACKETRIGHT) && !ctrl && !alt) {
		// [ / ]: the brush smaller / bigger (Shift: the strength)
		const float f = code == KEY_BRACKETRIGHT ? 1.15f : 1.0f / 1.15f;
		if (shift) {
			paint_strength = CLAMP(paint_strength * f, 0.01f, 1.0f);
		} else {
			paint_radius = CLAMP(paint_radius * f, 2.0f, 2000.0f);
		}
		if (dock && dock->paint_radius_field) {
			dock->paint_radius_field->set_value_no_signal(paint_radius);
			dock->paint_strength_field->set_value_no_signal(paint_strength);
		}
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if ((code == KEY_1 || code == KEY_2 || code == KEY_3) && !ctrl && !alt) {
		// what the brush paints: vertices, edges or faces (also the selection mask)
		const int tool = modal == MODAL_PAINT ? paint_tool : -1;
		set_mode(code == KEY_1 ? MODE_VERTEX : code == KEY_2 ? MODE_EDGE : MODE_FACE);
		if (tool >= 0) {
			start_paint_tool(tool);
		}
		if (dock) {
			dock->refresh();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	// the selection keys stay (A, Alt+A, Ctrl+I, L, Ctrl+L, H): the rest would change the mesh
	const bool selection_key = code == KEY_A || (code == KEY_I && ctrl) || code == KEY_L || code == KEY_H || (code == KEY_Z && alt);
	if (selection_key) {
		return -1;
	}
	return AFTER_GUI_INPUT_PASS;
}

// ------------------------------------------------------------------ painting

Color &HewnEditorPlugin::_paint_value(hewn::Face &p_face, int p_corner, int p_channel) {
	if (p_channel <= 0) {
		p_face.ensure_attrs();
		return p_face.col[p_corner];
	}
	if (p_face.cust.size() != p_face.v.size() * 4) {
		p_face.cust.assign(p_face.v.size() * 4, Color(0, 0, 0, 0));
	}
	return p_face.cust[p_corner * 4 + (p_channel - 1)];
}

// The first color (times the scale for the custom channels).
Color HewnEditorPlugin::_paint_brush_value() const {
	Color c = dock->get_paint_color();
	const int ch = dock->paint_channel ? dock->paint_channel->get_selected() : 0;
	if (ch > 0 && dock->paint_scale) {
		const float k = (float)dock->paint_scale->get_value();
		c = Color(c.r * k, c.g * k, c.b * k, c.a * k);
	}
	return c;
}

// The brush's weights on the corners (face, corner): by the mode, the selection as a mask. p_single: the
// pencil (the one element under the mouse, all of it).
void HewnEditorPlugin::_paint_weights(Camera3D *p_cam, const Vector2 &p_pos, bool p_single, std::vector<std::vector<float>> &r_w) {
	HewnMesh3D *n = get_edited();
	r_w.clear();
	if (!n) {
		return;
	}
	const hewn::Mesh &m = n->get_data();
	const Transform3D g = n->get_global_transform();
	const hewn::Selection &s = n->selection;
	r_w.resize(m.faces.size());
	for (size_t f = 0; f < m.faces.size(); f++) {
		r_w[f].assign(m.faces[f].size(), 0.0f);
	}
	// the mask
	std::vector<char> vsel(m.pos.size(), 0), fsel(m.faces.size(), 0);
	std::unordered_set<uint64_t> esel;
	for (int v : s.verts) {
		if (v >= 0 && v < (int)vsel.size()) {
			vsel[v] = 1;
		}
	}
	for (int f : s.faces) {
		if (f >= 0 && f < (int)fsel.size()) {
			fsel[f] = 1;
		}
	}
	for (const hewn::Edge &e : s.edges) {
		esel.insert(hewn::ekey(e));
	}
	const bool masked = mode == MODE_VERTEX ? !s.verts.empty() : mode == MODE_EDGE ? !s.edges.empty() : !s.faces.empty();
	// the brush: on the screen, or a sphere around the surface point under the mouse
	const bool surface = dock && dock->paint_falloff && dock->paint_falloff->get_selected() == 1;
	g_falloff_type = dock && dock->paint_falloff_type ? dock->paint_falloff_type->get_selected() : 0;
	{
		const Ref<Resource> fc = dock && dock->paint_falloff_curve_picker ? dock->paint_falloff_curve_picker->get_edited_resource() : Ref<Resource>();
		g_falloff_curve = fc.is_valid() ? (uint64_t)fc->get_instance_id() : 0;
	}
	Vector3 hit;
	float rw = 0.0f;
	if (surface) {
		Vector3 from, dir;
		_ray(p_cam, p_pos, from, dir);
		float d;
		if (_raycast_face(from, dir, &d) < 0) {
			return;
		}
		hit = from + dir * d;
		rw = paint_radius * _world_per_pixel(p_cam, hit);
	}
	const bool split = dock && dock->paint_split_btn && dock->paint_split_btn->is_pressed() && mode == MODE_VERTEX;
	// Split vertices off: an edge / a face paints the whole of its vertices (in every face around them); on: only
	// the corners on its side (each face its own color)
	const bool split_any = dock && dock->paint_split_btn && dock->paint_split_btn->is_pressed();
	auto whole_vertices = [&]() {
		if (split_any || paint_no_spread) {
			return;
		}
		std::vector<float> vw(m.pos.size(), 0.0f);
		for (size_t f = 0; f < m.faces.size(); f++) {
			for (int i = 0; i < m.faces[f].size(); i++) {
				vw[m.faces[f].v[i]] = MAX(vw[m.faces[f].v[i]], r_w[f][i]);
			}
		}
		for (size_t f = 0; f < m.faces.size(); f++) {
			if (m.faces[f].hidden) {
				continue;
			}
			for (int i = 0; i < m.faces[f].size(); i++) {
				r_w[f][i] = vw[m.faces[f].v[i]];
			}
		}
	};
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	// screen points (vis 0: behind / of hidden faces only), kept while the view stays
	const PaintView &pv = _paint_view_cache(p_cam, false);
	const std::vector<Vector2> &sp = pv.sp;
	const std::vector<char> &vis = pv.front;
	auto visible = [&](const Vector3 &local) { return _visible(p_cam, g.xform(local)); };
	auto falloff = [&](float d_screen, float d_world) { return surface ? smooth_falloff(d_world, rw) : smooth_falloff(d_screen, paint_radius); };
	if (mode == MODE_VERTEX) {
		// each vertex (in split mode each face corner, a little toward its face)
		int best_f = -1, best_i = -1;
		float best = 14.0f * es;
		std::vector<float> vw(m.pos.size(), -1.0f);
		for (size_t f = 0; f < m.faces.size(); f++) {
			const hewn::Face &F = m.faces[f];
			if (F.hidden) {
				continue;
			}
			Vector2 fc;
			Vector3 fc3;
			for (int v : F.v) {
				fc += sp[v];
				fc3 += m.pos[v];
			}
			fc /= (float)F.size();
			fc3 /= (float)F.size();
			for (int i = 0; i < F.size(); i++) {
				const int v = F.v[i];
				if (!vis[v] || (masked && !vsel[v])) {
					continue;
				}
				Vector2 p = sp[v];
				Vector3 p3 = m.pos[v];
				if (split && F.smooth == 0) {
					const float base = dock && dock->ov_square_size ? (float)dock->ov_square_size->get_value() : 8.0f;
					p += corner_dir(sp, F, i) * (base + 6.0f) * es;
					p3 = p3.lerp(fc3, 0.15f);
				}
				if (p_single) {
					const float d = p.distance_to(p_pos);
					if (d < best && (split || visible(m.pos[v]))) {
						best = d;
						best_f = (int)f;
						best_i = i;
					}
					continue;
				}
				const bool own = split && F.smooth == 0;
				if (!own && vw[v] >= 0.0f) {
					r_w[f][i] = vw[v];
					continue;
				}
				const float w = falloff(p.distance_to(p_pos), g.xform(p3).distance_to(hit));
				const float wv = w > 0.0f && visible(m.pos[v]) ? w : 0.0f;
				if (!own) {
					vw[v] = wv;
				}
				r_w[f][i] = wv;
			}
		}
		if (p_single) {
			Vector3 from, dir;
			_ray(p_cam, p_pos, from, dir);
			const int under = _raycast_face(from, dir);
			if (under >= 0 && (!masked || best_f >= 0)) {
				// on a face: its corner nearest to the mouse (also far from the vertex)
				const hewn::Face &F = m.faces[under];
				float bd = 1e9f;
				for (int i = 0; i < F.size(); i++) {
					const int v = F.v[i];
					if (vis[v] && (!masked || vsel[v]) && sp[v].distance_to(p_pos) < bd) {
						bd = sp[v].distance_to(p_pos);
						best_f = under;
						best_i = i;
					}
				}
			}
		}
		if (p_single && best_f >= 0) {
			if (split && m.faces[best_f].smooth == 0) {
				r_w[best_f][best_i] = 1.0f;
			} else {
				// the whole vertex (in split mode its corners on smooth faces)
				const int v = m.faces[best_f].v[best_i];
				for (size_t f = 0; f < m.faces.size(); f++) {
					for (int i = 0; i < m.faces[f].size(); i++) {
						if (m.faces[f].v[i] == v && (!split || m.faces[f].smooth != 0)) {
							r_w[f][i] = 1.0f;
						}
					}
				}
			}
		}
		return;
	}
	if (mode == MODE_EDGE) {
		// each edge: its two ends in the faces along it
		std::unordered_map<uint64_t, float> ew;
		uint64_t best_e = 0;
		float best = 10.0f * es;
		for (size_t f = 0; f < m.faces.size(); f++) {
			const hewn::Face &F = m.faces[f];
			if (F.hidden) {
				continue;
			}
			for (int i = 0; i < F.size(); i++) {
				const int a = F.v[i], b = F.v[(i + 1) % F.size()];
				const uint64_t k = hewn::ekey(a, b);
				if (!vis[a] || !vis[b] || (masked && !esel.count(k))) {
					continue;
				}
				const float ds = seg_d(p_pos, sp[a], sp[b]);
				if (p_single) {
					if (ds < best && visible(m.pos[a].lerp(m.pos[b], 0.5f))) {
						best = ds;
						best_e = k;
					}
					continue;
				}
				auto it = ew.find(k);
				if (it == ew.end()) {
					float w = falloff(ds, surface ? seg_dist3(hit, g.xform(m.pos[a]), g.xform(m.pos[b])) : 0.0f);
					if (w > 0.0f && !visible(m.pos[a].lerp(m.pos[b], 0.5f))) {
						w = 0.0f;
					}
					it = ew.emplace(k, w).first;
				}
			}
		}
		if (p_single) {
			// on a face: its edge nearest to the mouse
			Vector3 from, dir;
			_ray(p_cam, p_pos, from, dir);
			const int under = _raycast_face(from, dir);
			if (under >= 0) {
				const hewn::Face &F = m.faces[under];
				float bd = 1e9f;
				for (int i = 0; i < F.size(); i++) {
					const int a = F.v[i], b = F.v[(i + 1) % F.size()];
					const uint64_t k = hewn::ekey(a, b);
					if (vis[a] && vis[b] && (!masked || esel.count(k)) && seg_d(p_pos, sp[a], sp[b]) < bd) {
						bd = seg_d(p_pos, sp[a], sp[b]);
						best_e = k;
					}
				}
			}
			ew.clear();
			if (best_e) {
				ew[best_e] = 1.0f;
			}
		}
		// the colors are the face corners' (a mesh keeps none for its edges): an edge paints its two ends in the
		// faces along it (flat shading: each face its own colors); in smooth faces the whole vertex, or the smooth
		// surface would show a seam
		std::vector<float> vw(m.pos.size(), 0.0f);
		for (const std::pair<const uint64_t, float> &e : ew) {
			if (e.second > 0.0f) {
				const int a = (int)(e.first >> 32), b = (int)(e.first & 0xffffffffull);
				if (a >= 0 && a < (int)vw.size() && b >= 0 && b < (int)vw.size()) {
					vw[a] = MAX(vw[a], e.second);
					vw[b] = MAX(vw[b], e.second);
				}
			}
		}
		for (size_t f = 0; f < m.faces.size(); f++) {
			const hewn::Face &F = m.faces[f];
			for (int i = 0; i < F.size(); i++) {
				const int j = (i + 1) % F.size();
				auto it = ew.find(hewn::ekey(F.v[i], F.v[j]));
				if (it != ew.end() && it->second > 0.0f) {
					r_w[f][i] = MAX(r_w[f][i], it->second);
					r_w[f][j] = MAX(r_w[f][j], it->second);
				}
				if (F.smooth != 0) {
					r_w[f][i] = MAX(r_w[f][i], vw[F.v[i]]);
				}
			}
		}
		whole_vertices();
		return;
	}
	// faces: all their corners (each face its own color)
	Vector3 from, dir;
	_ray(p_cam, p_pos, from, dir);
	const int under = _raycast_face(from, dir);
	for (size_t f = 0; f < m.faces.size(); f++) {
		const hewn::Face &F = m.faces[f];
		if (F.hidden || (masked && !fsel[f])) {
			continue;
		}
		float w = 0.0f;
		if (p_single) {
			w = (int)f == under ? 1.0f : 0.0f;
		} else if ((int)f == under) {
			w = 1.0f;
		} else {
			std::vector<Vector2> poly;
			bool ok = true;
			float ds = 1e9f, dw = 1e9f;
			for (int i = 0; i < F.size(); i++) {
				const int a = F.v[i], b = F.v[(i + 1) % F.size()];
				ok = ok && vis[a];
				poly.push_back(sp[a]);
				ds = MIN(ds, seg_d(p_pos, sp[a], sp[b]));
				if (surface) {
					dw = MIN(dw, seg_dist3(hit, g.xform(m.pos[a]), g.xform(m.pos[b])));
				}
			}
			if (!ok) {
				continue;
			}
			if (!surface && in_poly(poly, p_pos)) {
				ds = 0.0f;
			}
			w = falloff(ds, dw);
			if (w > 0.0f && !visible(hewn::face_center(m, F))) {
				w = 0.0f;
			}
		}
		if (w > 0.0f) {
			r_w[f].assign(F.size(), w);
		}
	}
	whole_vertices();
}

// The color onto the corners by their weights: blend modes, only the channels turned on.
void HewnEditorPlugin::_paint_apply_weights(HewnMesh3D *p_node, const std::vector<std::vector<float>> &p_w, float p_scale) {
	hewn::Mesh &m = p_node->get_data();
	const int ch = dock->paint_channel ? dock->paint_channel->get_selected() : 0;
	if (ch > 0 && p_node->custom_formats[ch - 1] < 0) {
		p_node->custom_formats[ch - 1] = dock->paint_format->get_selected();
	}
	const Color b = _paint_brush_value();
	const int blend = dock->paint_blend ? dock->paint_blend->get_selected() : 0;
	bool on[4];
	for (int k = 0; k < 4; k++) {
		on[k] = _paint_channel_on(k);
	}
	bool any = false;
	for (size_t f = 0; f < m.faces.size() && f < p_w.size(); f++) {
		hewn::Face &F = m.faces[f];
		for (int i = 0; i < F.size() && i < (int)p_w[f].size(); i++) {
			const float w = MIN(1.0f, p_w[f][i] * p_scale);
			if (w <= 0.0f) {
				continue;
			}
			F.ensure_attrs();
			Color &x = _paint_value(F, i, ch);
			Color t = b;
			switch (blend) {
				case 1:
					t = x + b;
					break;
				case 2:
					t = x - b;
					break;
				case 3:
					t = x * b;
					break;
				case 4:
					t = Color(MAX(x.r, b.r), MAX(x.g, b.g), MAX(x.b, b.b), MAX(x.a, b.a));
					break;
				case 5:
					t = Color(MIN(x.r, b.r), MIN(x.g, b.g), MIN(x.b, b.b), MIN(x.a, b.a));
					break;
				default:
					break;
			}
			if (ch == 0) {
				t = Color(CLAMP(t.r, 0.0f, 1.0f), CLAMP(t.g, 0.0f, 1.0f), CLAMP(t.b, 0.0f, 1.0f), CLAMP(t.a, 0.0f, 1.0f));
			}
			for (int k = 0; k < 4; k++) {
				if (on[k]) {
					x[k] = x[k] + (t[k] - x[k]) * w;
				}
			}
			any = true;
		}
	}
	if (any) {
		p_node->commit(false);
	}
}

void HewnEditorPlugin::_paint_stroke(Camera3D *p_cam, const Vector2 &p_pos, bool p_first) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	std::vector<std::vector<float>> w;
	if (paint_tool == PT_NORMALS) {
		// smooth (one group for the stroke) or flat shading on the faces the brush reaches
		const Mode keep = mode;
		mode = MODE_VERTEX;
		_paint_weights(p_cam, p_pos, false, w);
		mode = keep;
		hewn::Mesh &m = n->get_data();
		const bool harden = Input::get_singleton()->is_key_pressed(KEY_CTRL);
		if (p_first) {
			normals_group = 0;
			for (const hewn::Face &F : m.faces) {
				normals_group = MAX(normals_group, F.smooth);
			}
			normals_group++;
		}
		std::vector<char> touched(m.faces.size(), 0);
		bool any = false;
		for (size_t f = 0; f < m.faces.size() && f < w.size(); f++) {
			for (float x : w[f]) {
				if (x > 0.05f) {
					touched[f] = 1;
				}
			}
			if (touched[f]) {
				const int want = harden ? 0 : (m.faces[f].smooth > 0 && !p_first ? m.faces[f].smooth : normals_group);
				if (m.faces[f].smooth != want) {
					m.faces[f].smooth = harden ? 0 : normals_group;
					any = true;
				}
			}
		}
		if (!harden) {
			// the sharp marks between the faces smoothed together go
			for (size_t f = 0; f < m.faces.size(); f++) {
				if (!touched[f]) {
					continue;
				}
				const hewn::Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					auto it = m.edge_attrs.find(hewn::ekey(F.v[i], F.v[(i + 1) % F.size()]));
					if (it != m.edge_attrs.end() && it->second.sharp) {
						it->second.sharp = false;
						any = true;
					}
				}
			}
		}
		if (any) {
			n->commit(false);
		}
		return;
	}
	// several element modes: the brush paints all of them, the pencil the first one it finds (vertices, edges, faces)
	{
		const Mode keep = mode;
		bool first_mode = true;
		for (int em = MODE_VERTEX; em <= MODE_FACE; em++) {
			if (!has_mode(em)) {
				continue;
			}
			mode = (Mode)em;
			std::vector<std::vector<float>> wm;
			_paint_weights(p_cam, p_pos, paint_tool == PT_PENCIL, wm);
			bool any = false;
			for (const std::vector<float> &fw : wm) {
				for (float x : fw) {
					any = any || x > 0.0f;
				}
			}
			if (first_mode || (paint_tool == PT_PENCIL && any)) {
				w = wm; // (the pencil: the first mode that finds something, only it)
			} else if (paint_tool != PT_PENCIL) {
				for (size_t f = 0; f < w.size() && f < wm.size(); f++) {
					for (size_t i = 0; i < w[f].size() && i < wm[f].size(); i++) {
						w[f][i] = MAX(w[f][i], wm[f][i]);
					}
				}
			}
			first_mode = false;
			if (paint_tool == PT_PENCIL && any) {
				break;
			}
		}
		mode = keep;
		if (first_mode) {
			_paint_weights(p_cam, p_pos, paint_tool == PT_PENCIL, w);
		}
	}
	float k = paint_tool == PT_PENCIL ? 1.0f : paint_strength;
	if (paint_tool != PT_PENCIL && dock && dock->paint_pressure_check && dock->paint_pressure_check->is_pressed()) {
		float p = CLAMP(paint_pressure_now, 0.0f, 1.0f);
		if (paint_pressure_curve.is_valid()) {
			p = CLAMP(paint_pressure_curve->sample(p), 0.0f, 1.0f);
		}
		k *= p;
	}
	if (paint_tool == PT_PENCIL) {
		for (std::vector<float> &fw : w) {
			for (float &x : fw) {
				x = x > 0.0f ? 1.0f : 0.0f; // (no falloff)
			}
		}
	}
	_paint_apply_weights(n, w, k);
}

// Fill: the selected elements, else everything linked to the face under the mouse.
void HewnEditorPlugin::_paint_fill(Camera3D *p_cam, const Vector2 &p_pos) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	const hewn::Mesh &m = n->get_data();
	const hewn::Selection &s = n->selection;
	std::vector<std::vector<float>> w(m.faces.size());
	for (size_t f = 0; f < m.faces.size(); f++) {
		w[f].assign(m.faces[f].size(), 0.0f);
	}
	const bool masked = mode == MODE_VERTEX ? !s.verts.empty() : mode == MODE_EDGE ? !s.edges.empty() : !s.faces.empty();
	if (masked) {
		std::unordered_set<int> vs(s.verts.begin(), s.verts.end()), fs(s.faces.begin(), s.faces.end());
		std::unordered_set<uint64_t> es;
		for (const hewn::Edge &e : s.edges) {
			es.insert(hewn::ekey(e));
		}
		for (size_t f = 0; f < m.faces.size(); f++) {
			const hewn::Face &F = m.faces[f];
			for (int i = 0; i < F.size(); i++) {
				bool on = false;
				if (mode == MODE_VERTEX) {
					on = vs.count(F.v[i]) > 0;
				} else if (mode == MODE_EDGE) {
					on = es.count(hewn::ekey(F.v[i], F.v[(i + 1) % F.size()])) || es.count(hewn::ekey(F.v[(i + F.size() - 1) % F.size()], F.v[i]));
				} else {
					on = fs.count((int)f) > 0;
				}
				w[f][i] = on ? 1.0f : 0.0f;
			}
		}
	} else {
		Vector3 from, dir;
		_ray(p_cam, p_pos, from, dir);
		const int under = _raycast_face(from, dir);
		if (under < 0) {
			return;
		}
		hewn::Topology t;
		t.build(m);
		for (int f : hewn::sel::linked_faces(m, t, { under })) {
			w[f].assign(m.faces[f].size(), 1.0f);
		}
	}
	_paint_apply_weights(n, w, 1.0f);
}

// The eyedropper: the color of the square nearest to the mouse (a face corner in split mode) as the first
// color (Shift: the second one).
bool HewnEditorPlugin::_paint_pick(Camera3D *p_cam, const Vector2 &p_pos, bool p_second) {
	HewnMesh3D *n = get_edited();
	if (!n || !dock) {
		return false;
	}
	std::vector<std::vector<float>> w;
	const Mode keep = mode;
	if (mode == MODE_EDGE) {
		mode = MODE_VERTEX; // (an edge's end: the nearest vertex)
	}
	_paint_weights(p_cam, p_pos, true, w);
	mode = keep;
	const int ch = dock->paint_channel ? dock->paint_channel->get_selected() : 0;
	const hewn::Mesh &m = n->get_data();
	for (size_t f = 0; f < w.size(); f++) {
		for (size_t i = 0; i < w[f].size(); i++) {
			if (w[f][i] > 0.0f) {
				Color c = corner_value(m.faces[f], (int)i, ch);
				const float sc = ch > 0 && dock->paint_scale ? (float)dock->paint_scale->get_value() : 1.0f;
				if (ch > 0 && std::fabs(sc) > 1e-6f) {
					c = Color(c.r / sc, c.g / sc, c.b / sc, c.a / sc); // (what the color picker holds)
				}
				(p_second ? dock->paint_color2 : dock->color_picker)->set_pick_color(c);
				return true;
			}
		}
	}
	return false;
}

// ------------------------------------------------------------------ input

// Ctrl+wheel: the brush's radius, Shift+wheel: its strength (also with a selection tool in Vertex Paint, but the
// circle select's own Ctrl+wheel).
bool HewnEditorPlugin::_paint_wheel(const Ref<InputEventMouseButton> &p_mb) {
	if (!vpaint || !p_mb->is_pressed() || !(p_mb->is_ctrl_pressed() || p_mb->is_meta_pressed() || p_mb->is_shift_pressed()) || p_mb->is_alt_pressed() ||
			(p_mb->get_button_index() != MOUSE_BUTTON_WHEEL_UP && p_mb->get_button_index() != MOUSE_BUTTON_WHEEL_DOWN)) {
		return false;
	}
	const float f = p_mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? 1.1f : 1.0f / 1.1f;
	if (p_mb->is_shift_pressed()) {
		paint_strength = CLAMP(paint_strength * f, 0.01f, 1.0f);
	} else {
		paint_radius = CLAMP(paint_radius * f, 2.0f, 2000.0f);
	}
	if (dock && dock->paint_radius_field) {
		dock->paint_radius_field->set_value_no_signal(paint_radius);
		dock->paint_strength_field->set_value_no_signal(paint_strength);
	}
	update_overlays();
	return true;
}

int32_t HewnEditorPlugin::_paint_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	if (!vpaint) {
		modal = MODAL_NONE;
		return AFTER_GUI_INPUT_PASS;
	}
	auto stop_stroke = [&]() {
		if (painting) {
			painting = false;
			end_edit();
		}
	};
	if (k.is_valid()) {
		const int32_t r = _vpaint_keys(p_cam, k);
		if (r >= 0) {
			return r;
		}
		return AFTER_GUI_INPUT_PASS; // (A / Ctrl+I...: the selection keys work with the select tools)
	}
	if (mb.is_valid() && _paint_wheel(mb)) {
		return AFTER_GUI_INPUT_STOP;
	}
	// Shift + drag (Krita): the brush's radius, the circle stays where the drag began (the cursor hidden while
	// dragging, put back after). By the position, not the motion: a tablet's pen has no motion of a captured mouse
	if (paint_resizing) {
		if (mm.is_valid()) {
			paint_resize_dx = mm->get_position().x - paint_resize_at.x;
			if (paint_resize_strength) {
				paint_strength = CLAMP(paint_resize_s0 + paint_resize_dx / 300.0f, 0.01f, 1.0f); // (300 px: all of it)
			} else {
				paint_radius = CLAMP(paint_resize_r0 + paint_resize_dx, 2.0f, 2000.0f);
			}
			if (dock && dock->paint_radius_field) {
				dock->paint_radius_field->set_value_no_signal(paint_radius);
				dock->paint_strength_field->set_value_no_signal(paint_strength);
			}
			mouse_pos = paint_resize_at;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && !mb->is_pressed()) {
			paint_resizing = false;
			Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
			if (Control *surface = _surface_for(p_cam)) {
				Input::get_singleton()->warp_mouse(surface->get_global_rect().position + paint_resize_at);
			}
			mouse_pos = paint_resize_at;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid()) {
			return AFTER_GUI_INPUT_STOP;
		}
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && mb->is_pressed() && mb->is_shift_pressed() && !mb->is_alt_pressed() &&
			(paint_tool == PT_BRUSH || paint_tool == PT_NORMALS)) {
		// Shift: the radius; Ctrl+Shift: the strength
		paint_resize_strength = mb->is_ctrl_pressed() || mb->is_meta_pressed();
		paint_resize_s0 = paint_strength;
		paint_resizing = true;
		paint_resize_at = mb->get_position();
		paint_resize_r0 = paint_radius;
		paint_resize_dx = 0.0f;
		mouse_pos = paint_resize_at;
		if (DisplayServer::get_singleton()->get_name() != "headless") {
			Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_HIDDEN);
		}
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && mb->is_pressed()) {
		mouse_pos = mb->get_position();
		const bool ctrl = mb->is_ctrl_pressed() || mb->is_meta_pressed();
		if (paint_tool == PT_PICKER || (ctrl && paint_tool != PT_NORMALS)) {
			_paint_pick(p_cam, mouse_pos, mb->is_shift_pressed()); // (Ctrl+click with the others: a quick eyedropper)
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (paint_tool == PT_FILL) {
			begin_edit("Vertex Paint: Fill");
			_paint_fill(p_cam, mouse_pos);
			end_edit();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		begin_edit(paint_tool == PT_NORMALS ? "Smooth / Flat Shading" : "Vertex Paint");
		painting = true;
		paint_pressure_now = 1.0f;
		_paint_stroke(p_cam, mouse_pos, true);
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && !mb->is_pressed()) {
		stop_stroke();
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mm.is_valid()) {
		mouse_pos = mm->get_position();
		if (painting) {
			// a pen's pressure (a mouse has none: full)
			const float pr = mm->get_pressure();
			paint_pressure_now = pr > 0.0f ? pr : 1.0f;
			_paint_stroke(p_cam, mouse_pos, false);
		}
		update_overlays();
		return painting ? AFTER_GUI_INPUT_STOP : AFTER_GUI_INPUT_PASS;
	}
	return AFTER_GUI_INPUT_PASS;
}

// ------------------------------------------------------------------ drawing

// The four (or fewer, by the format) bars of the first color: filled from the bottom by its values, crossed
// out when that channel is not painted.
void HewnEditorPlugin::_draw_channel_bars(Control *p_overlay, const Vector2 &p_at) {
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const int comps = _paint_comps();
	const Color c = dock->get_paint_color();
	const float w = std::round(8.0f * es), h = std::round(16.0f * es), gap = std::round(3.0f * es);
	const Vector2 start = (p_at - Vector2((w * comps + gap * (comps - 1)) * 0.5f, 0.0f)).round();
	for (int k = 0; k < comps; k++) {
		const Rect2 r(start + Vector2((w + gap) * k, 0.0f), Vector2(w, h));
		p_overlay->draw_rect(r.grow(1.0f), Color(0, 0, 0, 0.9f));
		p_overlay->draw_rect(r, BAR_BACK);
		const float v = CLAMP(c[k], 0.0f, 1.0f);
		const float fy = r.position.y + h * (1.0f - v);
		p_overlay->draw_rect(Rect2(Vector2(r.position.x, fy), Vector2(w, r.get_end().y - fy)), CHANNEL_COLORS[k]);
		if (!_paint_channel_on(k)) {
			// crossed out in black: two thin bands from corner to corner, exactly inside the box
			const Vector2 a = r.position, b = r.get_end();
			const float t = MAX(1.0f, std::round(es)) * 0.7f;
			for (int side = 0; side < 2; side++) {
				const float x0 = side ? b.x : a.x, x1 = side ? a.x : b.x, dx = side ? -t : t;
				PackedVector2Array band;
				band.push_back(Vector2(x0, a.y));
				band.push_back(Vector2(x0 + dx, a.y));
				band.push_back(Vector2(x1, b.y));
				band.push_back(Vector2(x1 - dx, b.y));
				draw_poly_safe(p_overlay, band, Color(0, 0, 0));
			}
		}
	}
}

// The edited mesh's vertices on the screen (projected here, not one engine call each), their corners, and
// (when asked) which are not behind other faces: kept while the camera, the mesh and the selection stay.
const HewnEditorPlugin::PaintView &HewnEditorPlugin::_paint_view_cache(Camera3D *p_cam, bool p_visibility) {
	HewnMesh3D *n = get_edited();
	PaintView &pv = pview;
	if (!n || !p_cam) {
		pv = PaintView();
		return pv;
	}
	const hewn::Mesh &m = n->get_data();
	const Transform3D g = n->get_global_transform();
	const uint64_t key = _marks_view_key(p_cam, n);
	if (key != pv.key || pv.sp.size() != m.pos.size()) {
		pv.key = key;
		pv.vis_done = false;
		pv.counts_key = 0;
		const size_t nv = m.pos.size();
		pv.off.assign(nv + 1, 0);
		for (const hewn::Face &F : m.faces) {
			if (!F.hidden) {
				for (int v : F.v) {
					pv.off[v + 1]++;
				}
			}
		}
		for (size_t v = 0; v < nv; v++) {
			pv.off[v + 1] += pv.off[v];
		}
		pv.cf.assign(pv.off[nv], 0);
		pv.ci.assign(pv.off[nv], 0);
		std::vector<int> fill(pv.off.begin(), pv.off.end() - 1);
		for (size_t f = 0; f < m.faces.size(); f++) {
			const hewn::Face &F = m.faces[f];
			if (F.hidden) {
				continue;
			}
			for (int i = 0; i < F.size(); i++) {
				const int k = fill[F.v[i]]++;
				pv.cf[k] = (int)f;
				pv.ci[k] = i;
			}
		}
		pv.sp.assign(nv, Vector2());
		pv.front.assign(nv, 0);
		const Transform3D ct = p_cam->get_global_transform();
		const Transform3D view = ct.affine_inverse();
		const Projection proj = p_cam->get_camera_projection();
		const Vector2 vs = p_cam->get_viewport() ? p_cam->get_viewport()->get_visible_rect().size : Vector2();
		const Vector3 eyedir = -ct.basis.get_column(2);
		const float znear = (float)p_cam->get_near();
		bool own = true; // (the projection here, checked once against the camera's)
		bool checked = false;
		for (size_t v = 0; v < nv; v++) {
			if (pv.off[v + 1] == pv.off[v]) {
				continue;
			}
			const Vector3 w = g.xform(m.pos[v]);
			if (eyedir.dot(w - ct.origin) < znear) {
				continue;
			}
			pv.front[v] = 1;
			if (own) {
				const Vector3 cv = view.xform(w);
				const Vector4 clip = proj.xform(Vector4(cv.x, cv.y, cv.z, 1.0f));
				pv.sp[v] = Vector2((clip.x / clip.w * 0.5f + 0.5f) * vs.x, (-clip.y / clip.w * 0.5f + 0.5f) * vs.y);
				if (!checked) {
					checked = true;
					if (pv.sp[v].distance_to(p_cam->unproject_position(w)) > 0.05f) {
						own = false;
					}
				}
			}
			if (!own) {
				pv.sp[v] = p_cam->unproject_position(w);
			}
		}
	}
	if (p_visibility && !pv.vis_done) {
		pv.vis.assign(m.pos.size(), 0);
		for (size_t v = 0; v < m.pos.size(); v++) {
			pv.vis[v] = pv.front[v] && _visible(p_cam, g.xform(m.pos[v]));
		}
		pv.vis_done = true;
	}
	return pv;
}

// Color squares over the visible vertices (the one nearest to the mouse in split mode: a square for each face
// corner, toward its face); the ones the brush reaches a little bigger; the eyedropper's values under its one.
void HewnEditorPlugin::_draw_paint_squares(Control *p_overlay, Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (!n || !dock) {
		return;
	}
	const hewn::Mesh &m = n->get_data();
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const int ch = dock->paint_channel ? dock->paint_channel->get_selected() : 0;
	const int comps = _paint_comps();
	const bool split = dock->paint_split_btn && dock->paint_split_btn->is_pressed();
	const bool squares = dock->paint_squares_btn && dock->paint_squares_btn->is_pressed() && paint_view != 2; // (the normals view: its own marks)
	if (m.pos.size() > 400000) {
		return; // (too many to read)
	}
	// what the tool would paint now: bigger squares
	std::vector<std::vector<float>> w;
	if (modal == MODAL_PAINT && (paint_tool == PT_BRUSH || paint_tool == PT_PENCIL || paint_tool == PT_PICKER)) {
		const Mode keep = mode;
		if (paint_tool == PT_PICKER && mode == MODE_EDGE) {
			mode = MODE_VERTEX;
		}
		_paint_weights(p_cam, mouse_pos, paint_tool != PT_BRUSH, w);
		mode = keep;
	}
	const PaintView &pv = _paint_view_cache(p_cam, squares || split);
	const float base = dock->ov_square_size ? (float)dock->ov_square_size->get_value() : 8.0f;
	const float s0 = std::round(base * es), s1 = std::round((base + 3.0f) * es);
	std::vector<char> vsel(m.pos.size(), 0);
	const std::vector<int> selv = mode == MODE_VERTEX ? n->selection.verts : mode == MODE_EDGE ? hewn::sel::verts_of_edges(n->selection.edges) : hewn::sel::verts_of_faces(m, n->selection.faces);
	for (int v : selv) {
		if (v >= 0 && v < (int)vsel.size()) {
			vsel[v] = 1;
		}
	}
	// one square for a vertex: the color most of its corners have (how many colors there are)
	auto majority = [&](int v, int *r_colors, bool *r_flat) {
		// (a few corners: a small list, no map; ties: the smaller key, as the map gave)
		struct Count {
			uint64_t key;
			int n;
			Color c;
		};
		Count local[16];
		std::vector<Count> more;
		int used = 0;
		bool flat = false;
		for (int k = pv.off[v]; k < pv.off[v + 1]; k++) {
			const hewn::Face &F = m.faces[pv.cf[k]];
			flat = flat || F.smooth == 0;
			const Color x = corner_value(F, pv.ci[k], ch);
			const uint64_t key = color_key(x);
			Count *e = nullptr;
			for (int i = 0; i < used && !e; i++) {
				Count &q = i < 16 ? local[i] : more[i - 16];
				if (q.key == key) {
					e = &q;
				}
			}
			if (!e) {
				if (used < 16) {
					local[used] = { key, 0, x };
					e = &local[used];
				} else {
					more.push_back({ key, 0, x });
					e = &more.back();
				}
				used++;
			}
			e->n++;
			e->c = x;
		}
		int most = 0;
		uint64_t most_key = 0;
		Color mc;
		for (int i = 0; i < used; i++) {
			const Count &q = i < 16 ? local[i] : more[i - 16];
			if (q.n > most || (q.n == most && q.key < most_key)) {
				most = q.n;
				most_key = q.key;
				mc = q.c;
			}
		}
		if (r_colors) {
			*r_colors = used;
		}
		if (r_flat) {
			*r_flat = flat;
		}
		return mc;
	};
	const bool near_only = squares && dock->paint_squares_near_btn && dock->paint_squares_near_btn->is_pressed();
	if (near_only) {
		_marks_set(p_overlay, 2, pv.key ^ 0x51u, std::vector<Mark>()); // (none of the far ones)
	}
	if (squares && !near_only) {
		// all of them in one draw, made again only when the view / the mesh / the selection change
		// (the colors: any change of the mesh)
		const uint64_t key = pv.key ^ ((uint64_t)(ch + 1) * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)comps << 40) ^ ((uint64_t)s0 << 48) ^ (split ? 1ull << 60 : 0ull) ^
				(n->get_revision() * 0xC2B2AE3D27D4EB4Full);
		if (!_marks_cached(p_overlay, 2, key) || pview.counts_key != key) {
			std::vector<Mark> marks;
			marks.reserve(m.pos.size());
			pview.counts.clear();
			for (size_t v = 0; v < m.pos.size(); v++) {
				if (!pv.vis[v]) {
					continue;
				}
				int colors = 1;
				bool flat = false;
				const Color mc = majority((int)v, &colors, &flat);
				marks.push_back(square_mark(pv.sp[v], s0, mc, comps, vsel[v]));
				if (split && flat && colors > 1) {
					pview.counts.push_back({ (int)v, colors });
				}
			}
			_marks_set(p_overlay, 2, key, marks);
			pview.counts_key = key;
		}
	}
	// the vertex the mouse is near (its corners spread out in split mode)
	int hover = -1;
	if (split && squares) {
		float best = 48.0f * es;
		for (size_t v = 0; v < m.pos.size(); v++) {
			if (pv.vis[v]) {
				const float d = pv.sp[v].distance_to(mouse_pos);
				if (d < best) {
					best = d;
					hover = (int)v;
				}
			}
		}
	}
	Ref<Font> font = p_overlay->get_theme_default_font();
	const int fsize = (int)(10 * es);
	Vector2 pick_at;
	Color pick_value;
	bool pick_found = false;
	if (squares) {
		// split mode: how many colors a vertex's corners have
		if (split && font.is_valid()) {
			for (const std::pair<int, int> &vc : pv.counts) {
				if (vc.first == hover) {
					continue;
				}
				const String t = itos((int64_t)vc.second);
				const Vector2 ts = font->get_string_size(t, HORIZONTAL_ALIGNMENT_LEFT, -1, fsize);
				const Vector2 at = pv.sp[vc.first] + Vector2(-ts.x * 0.5f, ts.y * 0.35f);
				p_overlay->draw_string_outline(font, at, t, HORIZONTAL_ALIGNMENT_LEFT, -1, fsize, 3, Color(0, 0, 0));
				p_overlay->draw_string(font, at, t, HORIZONTAL_ALIGNMENT_LEFT, -1, fsize, Color(1, 1, 1));
			}
		}
		// the vertices the tool reaches: bigger, over the others
		std::vector<char> big(m.pos.size(), 0);
		for (size_t f = 0; f < w.size(); f++) {
			for (size_t i = 0; i < w[f].size(); i++) {
				if (w[f][i] > 0.0f) {
					big[m.faces[f].v[i]] = 1;
				}
			}
		}
		for (size_t v = 0; v < m.pos.size(); v++) {
			if (!big[v] || !pv.vis[v] || (int)v == hover) {
				continue;
			}
			const Color mc = majority((int)v, nullptr, nullptr);
			draw_square(p_overlay, pv.sp[v], s1, mc, comps, vsel[v]);
			if (paint_tool == PT_PICKER && !pick_found) {
				pick_found = true;
				pick_at = pv.sp[v];
				pick_value = mc;
			}
		}
		if (hover >= 0) {
			bool flat_here = false;
			const Color hover_color = majority(hover, nullptr, &flat_here);
			if (!flat_here) {
				const bool b = big[hover] != 0;
				const Color mc = hover_color;
				draw_square(p_overlay, pv.sp[hover], b ? s1 : s0, mc, comps, vsel[hover]);
				if (b && paint_tool == PT_PICKER && !pick_found) {
					pick_found = true;
					pick_at = pv.sp[hover];
					pick_value = mc;
				}
			} else {
				// a square for each corner of a flat face, out along its angle's bisector
				const int v = hover;
				bool smooth_drawn = false;
				for (int k = pv.off[v]; k < pv.off[v + 1]; k++) {
					const hewn::Face &F = m.faces[pv.cf[k]];
					const int ci = pv.ci[k];
					if (F.smooth != 0) {
						if (!smooth_drawn) {
							smooth_drawn = true; // (the smooth faces' corners: one square on the vertex)
							draw_square(p_overlay, pv.sp[v], s0 * 0.85f, corner_value(F, ci, ch), comps, vsel[v]);
						}
						continue;
					}
					const Vector2 at = pv.sp[v] + corner_dir(pv.sp, F, ci) * (s1 + 3.0f * es);
					const bool b = pv.cf[k] < (int)w.size() && ci < (int)w[pv.cf[k]].size() && w[pv.cf[k]][ci] > 0.0f;
					draw_square(p_overlay, at, b ? s1 : s0 * 0.85f, corner_value(F, ci, ch), comps, vsel[v]);
					if (b && paint_tool == PT_PICKER && !pick_found) {
						pick_found = true;
						pick_at = at;
						pick_value = corner_value(F, ci, ch);
					}
				}
			}
		}
	}
	// the eyedropper: the values of the square under it, each in its channel's color
	if (paint_tool == PT_PICKER && modal == MODAL_PAINT && font.is_valid()) {
		if (!pick_found) {
			for (size_t f = 0; f < w.size() && !pick_found; f++) {
				for (size_t i = 0; i < w[f].size(); i++) {
					if (w[f][i] > 0.0f) {
						pick_found = true;
						pick_value = corner_value(m.faces[f], (int)i, ch);
						pick_at = mouse_pos;
						break;
					}
				}
			}
		}
		if (pick_found) {
			const int bsize = (int)(13 * es);
			String all;
			for (int k = 0; k < comps; k++) {
				all += (k ? "  " : "") + String::num(pick_value[k], 2);
			}
			const Vector2 ts = font->get_string_size(all, HORIZONTAL_ALIGNMENT_LEFT, -1, bsize);
			float x = pick_at.x - ts.x * 0.5f;
			const float y = pick_at.y + s1 + bsize + 6.0f * es;
			Ref<StyleBox> panel = EditorInterface::get_singleton()->get_editor_theme()->get_stylebox("panel", "TooltipPanel");
			const Rect2 box(Vector2(x - 5.0f * es, y - font->get_ascent(bsize) - 3.0f * es), Vector2(ts.x + 10.0f * es, ts.y + 6.0f * es));
			if (panel.is_valid()) {
				p_overlay->draw_style_box(panel, box);
			} else {
				p_overlay->draw_rect(box, Color(0.13f, 0.14f, 0.16f, 0.95f));
			}
			for (int k = 0; k < comps; k++) {
				const String t = String::num(pick_value[k], 2) + (k + 1 < comps ? "  " : "");
				p_overlay->draw_string(font, Vector2(x, y), t, HORIZONTAL_ALIGNMENT_LEFT, -1, bsize, CHANNEL_COLORS[k].lightened(0.15f));
				x += font->get_string_size(t, HORIZONTAL_ALIGNMENT_LEFT, -1, bsize).x;
			}
		}
	}
}

void HewnEditorPlugin::_draw_paint(Control *p_overlay, Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (!n || !dock) {
		return;
	}
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const hewn::Mesh &m = n->get_data();
	const Transform3D g = n->get_global_transform();
	const int ch = dock->paint_channel ? dock->paint_channel->get_selected() : 0;
	// the normals view: the faces (smooth blue, flat orange) and the sharp edges are in the gizmo; the
	// vertices: a circle where all its faces are smooth, a rounded triangle where one is flat (all in one draw,
	// made again only when the view / the mesh change)
	if (paint_view == 2) {
		const PaintView &pv = _paint_view_cache(p_cam, true);
		uint64_t key = pv.key ^ 0x51ull;
		for (const hewn::Face &F : m.faces) {
			key = (key ^ (uint64_t)(uint32_t)F.smooth) * 1099511628211ull; // (smooth / flat)
		}
		const float sq = dock->ov_square_size ? (float)dock->ov_square_size->get_value() : 8.0f;
		key ^= (uint64_t)(sq * 16.0f) << 52;
		if (!_marks_cached(p_overlay, 1, key)) {
			std::vector<Mark> marks;
			// as big as the color squares; the selected ones outlined in the selection's color
			const float half = std::round(sq * es * 0.5f) + 1.0f;
			std::vector<char> vsel(m.pos.size(), 0);
			const std::vector<int> selv = mode == MODE_VERTEX ? n->selection.verts : mode == MODE_EDGE ? hewn::sel::verts_of_edges(n->selection.edges) : hewn::sel::verts_of_faces(m, n->selection.faces);
			for (int v : selv) {
				if (v >= 0 && v < (int)vsel.size()) {
					vsel[v] = 1;
				}
			}
			for (size_t v = 0; v < m.pos.size(); v++) {
				if (!pv.vis[v]) {
					continue;
				}
				bool flat = false;
				for (int k = pv.off[v]; k < pv.off[v + 1]; k++) {
					flat = flat || m.faces[pv.cf[k]].smooth == 0;
				}
				Mark mk;
				mk.at = pv.sp[v];
				mk.half = flat ? std::round(half * 1.3f) : half + 1.0f;
				mk.color = flat ? Color(1.0f, 0.6f, 0.25f) : Color(0.35f, 0.75f, 1.0f);
				mk.shape = flat ? 2.0f : 1.0f;
				mk.border = vsel[v] ? 2.0f : 0.0f;
				marks.push_back(mk);
			}
			_marks_set(p_overlay, 1, key, marks);
		}
	}
	if (Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)) {
		return; // (the camera flies: the brush's drawing is in the way)
	}
	// the element under the mouse (edges / faces) in its corners' colors
	for (int hm = MODE_EDGE; hm <= MODE_FACE && modal == MODAL_PAINT && paint_tool != PT_NORMALS; hm++) {
		if (!has_mode(hm)) {
			continue; // (several modes: the edge and the face under the mouse both)
		}
		const Mode keep = mode;
		mode = (Mode)hm;
		std::vector<std::vector<float>> w;
		paint_no_spread = true;
		_paint_weights(p_cam, mouse_pos, true, w);
		paint_no_spread = false;
		mode = keep;
		for (size_t f = 0; f < w.size(); f++) {
			const hewn::Face &F = m.faces[f];
			bool any = false;
			for (float x : w[f]) {
				any = any || x > 0.0f;
			}
			if (!any) {
				continue;
			}
			if (hm == MODE_FACE) {
				PackedVector2Array pts;
				PackedColorArray cols;
				for (int i = 0; i < F.size(); i++) {
					pts.push_back(p_cam->unproject_position(g.xform(m.pos[F.v[i]])));
					Color c = shown_color(corner_value(F, i, ch), _paint_comps());
					c.a = 0.55f;
					cols.push_back(c);
				}
				p_overlay->draw_polygon(pts, cols);
				break;
			}
			for (int i = 0; i < F.size(); i++) {
				const int j = (i + 1) % F.size();
				if (w[f][i] > 0.0f && w[f][j] > 0.0f) {
					PackedVector2Array pts;
					pts.push_back(p_cam->unproject_position(g.xform(m.pos[F.v[i]])));
					pts.push_back(p_cam->unproject_position(g.xform(m.pos[F.v[j]])));
					PackedColorArray cols;
					cols.push_back(shown_color(corner_value(F, i, ch), _paint_comps()));
					cols.push_back(shown_color(corner_value(F, j, ch), _paint_comps()));
					p_overlay->draw_polyline_colors(pts, cols, 5.0f * es, true);
				}
			}
			break;
		}
	}
	_draw_paint_squares(p_overlay, p_cam);
	if (modal != MODAL_PAINT) {
		_draw_select_region(p_overlay);
		return;
	}
	// the brush
	const bool circle = (paint_tool == PT_BRUSH || paint_tool == PT_NORMALS) && (!dock->paint_circle_check || dock->paint_circle_check->is_pressed());
	const Color cc = dock->paint_circle_picker ? dock->paint_circle_picker->get_pick_color() : Color(0.0f, 0.5f, 1.0f, 0.9f);
	float below = 14.0f * es; // (where the channel bars go)
	if (circle) {
		const bool surface = dock->paint_falloff && dock->paint_falloff->get_selected() == 1;
		Vector3 from, dir;
		_ray(p_cam, mouse_pos, from, dir);
		float d;
		const int f = surface ? _raycast_face(from, dir, &d) : -1;
		if (surface && f >= 0) {
			// a circle lying on the surface, turned with its normal
			const Vector3 hit = from + dir * d;
			const Vector3 nrm = (g.basis.inverse().transposed().xform(hewn::face_normal(m, m.faces[f]))).normalized();
			const float rw = paint_radius * _world_per_pixel(p_cam, hit);
			Vector3 u = nrm.cross(std::fabs(nrm.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).normalized();
			const Vector3 v = nrm.cross(u);
			for (float scale : { 1.0f, paint_strength }) {
				PackedVector2Array pts;
				for (int i = 0; i <= 48; i++) {
					const float a = (float)Math::PI * 2.0f * i / 48;
					const Vector3 p = hit + (u * std::cos(a) + v * std::sin(a)) * rw * scale;
					if (!p_cam->is_position_behind(p)) {
						pts.push_back(p_cam->unproject_position(p));
					}
				}
				if (pts.size() > 2) {
					p_overlay->draw_polyline(pts, scale == 1.0f ? cc : cc * Color(1, 1, 1, 0.6f), scale == 1.0f ? 1.5f : 1.0f, true);
				}
			}
		} else {
			p_overlay->draw_arc(mouse_pos, paint_radius, 0.0f, (float)Math::PI * 2.0f, 48, cc, 1.5f, true);
			p_overlay->draw_arc(mouse_pos, paint_radius * paint_strength, 0.0f, (float)Math::PI * 2.0f, 32, cc * Color(1, 1, 1, 0.6f), 1.0f, true);
		}
		below = paint_radius + 6.0f * es;
	}
	if ((paint_tool == PT_BRUSH || paint_tool == PT_PENCIL || paint_tool == PT_FILL) && (!dock->paint_bars_check || dock->paint_bars_check->is_pressed())) {
		_draw_channel_bars(p_overlay, mouse_pos + Vector2(0.0f, below));
	}
	static const char *tools[5] = { "Brush", "Pencil", "Fill", "Eyedropper", "Normals" };
	static const char *chans[5] = { "Color", "CUSTOM0", "CUSTOM1", "CUSTOM2", "CUSTOM3" };
	String hint = T("Ctrl+wheel or [ / ]: radius, Shift+wheel: strength, Ctrl+click: pick a color");
	if (paint_tool == PT_PENCIL) {
		hint = T("Click: the vertex / edge / face under the mouse, Ctrl+click: pick a color");
	}
	if (paint_tool == PT_NORMALS) {
		hint = T("LMB: smooth shading, Ctrl+LMB: flat shading, Ctrl+wheel or [ / ]: radius");
	} else if (paint_tool == PT_PICKER) {
		hint = T("Click: the first color, Shift+click: the second one");
	} else if (paint_tool == PT_FILL) {
		hint = String();
	}
	(void)tools;
	(void)chans;
	draw_header(p_overlay, p_overlay->get_theme_default_font(), hint); // (only the keys: the rest is on the header)
}

// ------------------------------------------------------------------ views

namespace {
// Single Color's cavity: drawn over the colors (it needs the depth they leave), ridges lighter, valleys darker.
// Screen: the surface the next pixels around, behind this one's plane a ridge, in front of it a valley. World: the
// same a fixed distance around.
const char *HW_CAVITY_SHADER = R"(
shader_type spatial;
render_mode unshaded, cull_back, depth_draw_never, blend_mix, shadows_disabled, fog_disabled;
uniform float cavity_ridge = 1.0;
uniform float cavity_valley = 1.0;
uniform int cavity_type = 1;
uniform sampler2D screen_depth : hint_depth_texture, repeat_disable, filter_nearest;
vec3 view_at(vec2 uv, mat4 ip) {
	vec4 v = ip * vec4(uv * 2.0 - 1.0, texture(screen_depth, uv).r, 1.0);
	return v.xyz / v.w;
}
// how far the surface around (the next pixels) is behind / in front of this one's plane, for the spacing between
// them: an edge gives a line two pixels wide, a flat surface nothing at any angle
float screen_curvature(vec2 uv, vec2 px, mat4 ip, vec3 n) {
	vec3 c = view_at(uv, ip);
	vec2 offs[4] = vec2[](vec2(-px.x, 0.0), vec2(px.x, 0.0), vec2(0.0, -px.y), vec2(0.0, px.y));
	float off = 0.0;
	float span = 0.0;
	float count = 0.0;
	for (int i = 0; i < 4; i++) {
		vec3 d = view_at(uv + offs[i], ip) - c;
		if (abs(d.z) > abs(c.z) * 0.05) {
			continue; // (off this surface, another object or the sky)
		}
		float h = dot(d, n);
		off += h;
		span += length(d - n * h);
		count += 1.0;
	}
	return count < 2.0 || span < 1e-9 ? 0.0 : -off / span;
}
// the same for the surface a fixed distance around (5 cm, as many pixels as that is where it is seen): 8 ways, at
// that distance and half of it; a step in the depth (another surface) is left out
float world_curvature(vec2 uv, vec2 px, mat4 proj, mat4 ip, vec3 n) {
	vec3 c = view_at(uv, ip);
	float radius = 0.05;
	float rpx = radius * proj[1][1] * 0.5 / px.y;
	if (proj[3][3] < 0.5) {
		rpx /= max(-c.z, 1e-4); // (perspective: smaller further away)
	}
	rpx = clamp(rpx, 1.0, 48.0);
	float off = 0.0;
	float span = 0.0;
	float count = 0.0;
	for (int i = 0; i < 8; i++) {
		float a = float(i) * 0.785398;
		vec2 dir = vec2(cos(a), sin(a));
		for (int s = 1; s <= 2; s++) {
			// (on a pixel's middle: the depth read there and the point made from it are the same place, no stripes)
			vec2 suv = (floor((uv + dir * rpx * (float(s) * 0.5) * px) / px) + 0.5) * px;
			if (suv.x < 0.0 || suv.y < 0.0 || suv.x > 1.0 || suv.y > 1.0) {
				continue;
			}
			vec3 d = view_at(suv, ip) - c;
			float h = dot(d, n);
			if (abs(h) > radius * 1.5) {
				continue;
			}
			off += h;
			span += length(d - n * h);
			count += 1.0;
		}
	}
	return count < 4.0 || span < 1e-9 ? 0.0 : -off / span;
}
void fragment() {
	vec2 px = 1.0 / VIEWPORT_SIZE;
	float k = 0.0;
	if (cavity_type != 0) {
		k += screen_curvature(SCREEN_UV, px, INV_PROJECTION_MATRIX, normalize(NORMAL)) * 3.0;
	}
	if (cavity_type != 1) {
		k += world_curvature(SCREEN_UV, px, PROJECTION_MATRIX, INV_PROJECTION_MATRIX, normalize(NORMAL)) * 2.5;
	}
	k = clamp(k, -1.0, 1.0);
	ALBEDO = k > 0.0 ? vec3(1.0) : vec3(0.0);
	ALPHA = k > 0.0 ? clamp(k * cavity_ridge * 0.6, 0.0, 1.0) : clamp(-k * cavity_valley * 0.7, 0.0, 1.0);
}
)";
} // namespace

// The painted channel unshaded, or the materials again: an override of the display only (not saved).
// The views over the materials (the painted channel, the normals as colors): every Hewn mesh of the scene,
// in every mode viewport shading.
void HewnEditorPlugin::apply_paint_view() {
	_clear_paint_view();
	if (paint_view == 6) {
		// Collision: lit like Random, a color for each kind of collision, none striped
		if (collision_view_mat.is_null()) {
			String code = String("shader_type spatial;\nrender_mode unshaded, cull_back;\ninstance uniform vec4 obj_color = vec4(0.8, 0.8, 0.8, 1.0);\n") +
					"instance uniform float stripes = 0.0;\nuniform bool world_light = false;\nuniform float world_light_angle = 0.0;\n" +
					"float studio(vec3 n) {\n\treturn 0.32 + 0.55 * max(dot(n, normalize(vec3(-0.4, 0.6, 0.7))), 0.0) + 0.22 * max(dot(n, normalize(vec3(0.7, 0.1, 0.4))), 0.0);\n}\n" +
					"vec3 to_linear(vec3 c) {\n\treturn mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));\n}\n" +
					"vec3 light_normal(vec3 n_view, mat4 inv_view) {\n\tif (!world_light) {\n\t\treturn n_view;\n\t}\n" +
					"\tvec3 n = (inv_view * vec4(n_view, 0.0)).xyz;\n\tfloat a = radians(world_light_angle);\n" +
					"\treturn normalize(vec3(cos(a) * n.x + sin(a) * n.z, n.y, -sin(a) * n.x + cos(a) * n.z));\n}\n" +
					"void fragment() {\n\tvec3 c = obj_color.rgb;\n" +
					"\tif (stripes > 0.5 && mod(floor((FRAGCOORD.x + FRAGCOORD.y) / 10.0), 2.0) < 1.0) {\n\t\tc = c * 0.35;\n\t}\n" +
					"\tALBEDO = to_linear(c * studio(light_normal(normalize(NORMAL), INV_VIEW_MATRIX)));\n}\n";
			Ref<Shader> sh;
			sh.instantiate();
			sh->set_code(code);
			collision_view_mat.instantiate();
			collision_view_mat->set_shader(sh);
		}
		const HwMeshStyle &st = hw_mesh_style();
		collision_view_mat->set_shader_parameter("world_light", st.world_light);
		collision_view_mat->set_shader_parameter("world_light_angle", st.world_light_angle);
		std::vector<std::pair<GeometryInstance3D *, int>> items;
		const uint64_t key = _collision_view_scan(&items);
		RenderingServer *rs = RenderingServer::get_singleton();
		for (const auto &it : items) {
			const RID inst = it.first->get_instance();
			const Color c = collision_view_color(it.second);
			rs->instance_geometry_set_material_override(inst, collision_view_mat->get_rid());
			rs->instance_geometry_set_shader_parameter(inst, "obj_color", Vector4(c.r, c.g, c.b, 1.0f));
			rs->instance_geometry_set_shader_parameter(inst, "stripes", it.second == 0 ? 1.0f : 0.0f);
			paint_view_ids.push_back(ObjectID(it.first->get_instance_id()));
		}
		collision_view_key = key ^ (collision_view_shapes ? _collision_shapes_key() : 0);
		if (collision_view_shapes) {
			// only the shapes: the objects not drawn (no color, no depth), the shapes instead
			if (collision_hide_mat.is_null()) {
				Ref<Shader> hs;
				hs.instantiate();
				hs->set_code("shader_type spatial;\nrender_mode unshaded, depth_draw_never, shadows_disabled;\nvoid fragment() {\n\tdiscard;\n}\n");
				collision_hide_mat.instantiate();
				collision_hide_mat->set_shader(hs);
			}
			for (const auto &it : items) {
				if (it.second == 0 || !Object::cast_to<CSGShape3D>(it.first)) {
					rs->instance_geometry_set_material_override(it.first->get_instance(), collision_hide_mat->get_rid());
				}
			}
			_collision_shapes_show();
		}
		return;
	}
	if (paint_view >= 4 && paint_view <= 5) {
		// Solid colors with its studio light: lit from the view (two lights and some ambient), the scene's
		// lights do not matter. Single: one clay grey; Random: a color per object.
		Ref<ShaderMaterial> &mat = solid_view_mat[paint_view - 4];
		if (mat.is_null()) {
			// (the lights: from the view, or fixed in the world and turned about its Y axis; the colors as seen, in
			// sRGB: no source_color, they are made linear here once)
			String code = String("shader_type spatial;\nrender_mode unshaded, cull_back;\ninstance uniform vec4 obj_color = vec4(0.8, 0.8, 0.8, 1.0);\n") +
					"uniform vec4 single_color = vec4(0.773, 0.78, 0.788, 1.0);\nuniform bool world_light = false;\nuniform float world_light_angle = 0.0;\n" +
					"float studio(vec3 n) {\n\treturn 0.32 + 0.55 * max(dot(n, normalize(vec3(-0.4, 0.6, 0.7))), 0.0) + 0.22 * max(dot(n, normalize(vec3(0.7, 0.1, 0.4))), 0.0);\n}\n" +
					"vec3 to_linear(vec3 c) {\n\treturn mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));\n}\n" +
					"vec3 light_normal(vec3 n_view, mat4 inv_view) {\n\tif (!world_light) {\n\t\treturn n_view;\n\t}\n" +
					"\tvec3 n = (inv_view * vec4(n_view, 0.0)).xyz;\n\tfloat a = radians(world_light_angle);\n" +
					"\treturn normalize(vec3(cos(a) * n.x + sin(a) * n.z, n.y, -sin(a) * n.x + cos(a) * n.z));\n}\n";
			const String base = paint_view == 4 ? "single_color.rgb" : "obj_color.rgb";
			code += "void fragment() {\n\tALBEDO = to_linear(" + base + " * studio(light_normal(normalize(NORMAL), INV_VIEW_MATRIX)));\n}\n";
			Ref<Shader> sh;
			sh.instantiate();
			sh->set_code(code);
			mat.instantiate();
			mat->set_shader(sh);
			const bool can_cavity = paint_view == 4 && RenderingServer::get_singleton()->get_current_rendering_method() != "gl_compatibility";
			if (can_cavity && hw_mesh_style().cavity) {
				Ref<Shader> cs;
				cs.instantiate();
				cs->set_code(HW_CAVITY_SHADER);
				Ref<ShaderMaterial> cm;
				cm.instantiate();
				cm->set_shader(cs);
				mat->set_next_pass(cm);
			}
		}
		_apply_solid_params();
		for (HewnMesh3D *m : _view_meshes()) {
			if (m->get_instance().is_valid()) {
				RenderingServer *rs = RenderingServer::get_singleton();
				rs->instance_geometry_set_material_override(m->get_instance(), mat->get_rid());
				// (Random: from the node's name, the same color every time)
				const uint32_t hsh = (uint32_t)String(m->get_name()).hash();
				const Color c = Color::from_hsv((hsh % 1000) / 1000.0f, 0.45f + ((hsh >> 10) % 100) / 400.0f, 0.85f);
				rs->instance_geometry_set_shader_parameter(m->get_instance(), "obj_color", Vector4(c.r, c.g, c.b, 1.0f)); // (as seen: not made linear)
				paint_view_ids.push_back(ObjectID(m->get_instance_id()));
			}
		}
		return;
	}
	if (paint_view == 3) {
		if (normal_view_mat.is_null()) {
			Ref<Shader> sh;
			sh.instantiate();
			// the normal in view space as a color (x red, y green, z blue): facing the camera lilac, up green-ish,
			// sideways pink / blue; pow: shown as those values, not brightened by the sRGB output
			sh->set_code("shader_type spatial;\nrender_mode unshaded, cull_back;\nvoid fragment() {\n\tvec3 n = normalize(NORMAL) * 0.5 + 0.5;\n\tALBEDO = pow(n, vec3(2.2));\n}\n");
			normal_view_mat.instantiate();
			normal_view_mat->set_shader(sh);
		}
		for (HewnMesh3D *m : _view_meshes()) {
			if (m->get_instance().is_valid()) {
				RenderingServer::get_singleton()->instance_geometry_set_material_override(m->get_instance(), normal_view_mat->get_rid());
				paint_view_ids.push_back(ObjectID(m->get_instance_id()));
			}
		}
		return;
	}
	if (paint_view != 1) {
		return;
	}
	const int ch = dock && dock->paint_channel ? dock->paint_channel->get_selected() : 0;
	if (paint_view_mat.is_null()) {
		Ref<Shader> sh;
		sh.instantiate();
		// RGBA: where it is see-through, a checker of white and the color (grey and white for a white color), solid
		// where opaque;
		// RGB: only the color; A: only the alpha (grey)
		sh->set_code(String("shader_type spatial;\nrender_mode unshaded, cull_back;\nuniform int channel = 0;\nuniform float scale = 1.0;\nuniform bool use_alpha = true;\nuniform int alpha_view = 0;\nvarying vec4 v_val;\n") +
				"void vertex() {\n\tv_val = channel == 1 ? CUSTOM0 : channel == 2 ? CUSTOM1 : channel == 3 ? CUSTOM2 : channel == 4 ? CUSTOM3 : COLOR;\n}\n" +
				"void fragment() {\n\tvec3 c = clamp(v_val.rgb * scale, 0.0, 1.0);\n\tfloat a = clamp(v_val.a * (channel > 0 ? scale : 1.0), 0.0, 1.0);\n" +
				"\tif (use_alpha && alpha_view == 2) {\n\t\tc = vec3(a);\n\t} else if (use_alpha && alpha_view == 0) {\n" +
				"\t\tvec2 cell = floor(FRAGCOORD.xy / 8.0);\n\t\tbool light = mod(cell.x + cell.y, 2.0) < 1.0;\n" +
				"\t\tvec3 lc = dot(c, vec3(0.299, 0.587, 0.114)) > 0.8 ? vec3(0.6) : vec3(1.0);\n" +
				"\t\tvec3 checker = light ? lc : c;\n\t\tc = mix(checker, c, a);\n\t}\n\tALBEDO = c;\n}\n");
		paint_view_mat.instantiate();
		paint_view_mat->set_shader(sh);
	}
	paint_view_mat->set_shader_parameter("channel", ch);
	paint_view_mat->set_shader_parameter("use_alpha", _paint_comps() == 4);
	paint_view_mat->set_shader_parameter("alpha_view", paint_alpha_view);
	paint_view_mat->set_shader_parameter("scale", 1.0f);
	for (HewnMesh3D *m : _view_meshes()) {
		if (m->get_instance().is_valid()) {
			RenderingServer::get_singleton()->instance_geometry_set_material_override(m->get_instance(), paint_view_mat->get_rid());
			paint_view_ids.push_back(ObjectID(m->get_instance_id()));
		}
	}
}

void HewnEditorPlugin::_clear_paint_view() {
	for (const ObjectID &id : paint_view_ids) {
		if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(id))) {
			if (m->get_instance().is_valid()) {
				const Ref<Material> own = m->get_material_override();
				RenderingServer::get_singleton()->instance_geometry_set_material_override(m->get_instance(), own.is_valid() ? own->get_rid() : RID());
			}
		} else if (GeometryInstance3D *gi = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(id))) {
			if (gi->get_instance().is_valid()) {
				const Ref<Material> own = gi->get_material_override();
				RenderingServer::get_singleton()->instance_geometry_set_material_override(gi->get_instance(), own.is_valid() ? own->get_rid() : RID());
			}
		}
	}
	paint_view_ids.clear();
	collision_view_key = 0;
	_collision_shapes_clear();
}

void HewnEditorPlugin::_collision_shapes_clear() {
	for (const RID &r : cshape_insts) {
		RenderingServer::get_singleton()->free_rid(r);
	}
	cshape_insts.clear();
	cshape_meshes.clear();
}

Color HewnEditorPlugin::collision_view_color(int p_kind) {
	switch (p_kind) {
		case 0:
			return Color(1.0f, 0.1f, 0.75f);
		case 1:
			return Color(0.35f, 0.8f, 0.4f);
		case 2:
			return Color(0.35f, 0.6f, 1.0f);
		case 3:
			return Color(1.0f, 0.65f, 0.25f);
		default:
			return Color(0.3f, 0.85f, 0.85f);
	}
}

namespace {
// the kind of a collision shape: 1 triangles, 2 convex, 3 a simple shape
int shape_kind(const Ref<Shape3D> &p_shape) {
	if (p_shape.is_null()) {
		return 0;
	}
	if (Object::cast_to<ConcavePolygonShape3D>(p_shape.ptr()) || Object::cast_to<HeightMapShape3D>(p_shape.ptr())) {
		return 1;
	}
	if (Object::cast_to<ConvexPolygonShape3D>(p_shape.ptr())) {
		return 2;
	}
	return 3;
}
// the shapes of a body (CollisionShape3D children with a shape, not disabled): the kind of the first, 4 a body
// with some other shape, 0 none
int body_kind(Node *p_body) {
	for (int i = 0; i < p_body->get_child_count(); i++) {
		if (CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(p_body->get_child(i))) {
			if (!cs->is_disabled() && cs->get_shape().is_valid()) {
				return shape_kind(cs->get_shape());
			}
		}
		if (Object::cast_to<CollisionPolygon3D>(p_body->get_child(i))) {
			return 4;
		}
	}
	return 0;
}
// the collision of the body a mesh belongs to: the nearest body above it (a mesh inside a body of the user's), or a body
// somewhere under it (made under the mesh); 0 none
int body_around(Node *p_node) {
	for (Node *a = p_node->get_parent(); a; a = a->get_parent()) {
		if (Object::cast_to<CollisionObject3D>(a)) {
			const int k = body_kind(a);
			if (k != 0) {
				return k;
			}
			break;
		}
	}
	std::vector<std::pair<Node *, int>> stack = { { p_node, 0 } };
	while (!stack.empty()) {
		Node *x = stack.back().first;
		const int depth = stack.back().second;
		stack.pop_back();
		for (int i = 0; i < x->get_child_count() && depth < 4; i++) {
			Node *c = x->get_child(i);
			if (Object::cast_to<CollisionObject3D>(c)) {
				const int k = body_kind(c);
				if (k != 0) {
					return k;
				}
			}
			if (!Object::cast_to<MeshInstance3D>(c)) {
				stack.push_back({ c, depth + 1 });
			}
		}
	}
	return 0;
}
} // namespace

namespace {
// the convex hull of the points as triangles (outward); incremental
void hull_tris(const PackedVector3Array &p_pts, std::vector<Vector3> &r_tris) {
	std::vector<Vector3> P;
	for (int64_t i = 0; i < p_pts.size(); i++) {
		P.push_back(p_pts[i]);
	}
	const int n = (int)P.size();
	if (n < 4) {
		return;
	}
	AABB box(P[0], Vector3());
	for (const Vector3 &q : P) {
		box.expand_to(q);
	}
	const float eps = MAX(1e-6f, box.get_longest_axis_size() * 1e-5f);
	// a first tetrahedron
	int a = 0, b = -1, c = -1, d = -1;
	for (int i = 1; i < n && b < 0; i++) {
		if (P[i].distance_to(P[a]) > eps) {
			b = i;
		}
	}
	for (int i = 1; i < n && c < 0 && b >= 0; i++) {
		if ((P[b] - P[a]).cross(P[i] - P[a]).length() > eps * box.get_longest_axis_size()) {
			c = i;
		}
	}
	for (int i = 1; i < n && d < 0 && c >= 0; i++) {
		if (std::fabs((P[b] - P[a]).cross(P[c] - P[a]).normalized().dot(P[i] - P[a])) > eps) {
			d = i;
		}
	}
	if (d < 0) {
		return;
	}
	struct F {
		int v[3];
		Vector3 n;
		float off;
		bool alive = true;
	};
	std::vector<F> fs;
	const Vector3 inner = (P[a] + P[b] + P[c] + P[d]) * 0.25f;
	auto add = [&](int x, int y, int z) {
		F f;
		f.v[0] = x;
		f.v[1] = y;
		f.v[2] = z;
		f.n = (P[y] - P[x]).cross(P[z] - P[x]).normalized();
		if (f.n.dot(inner - P[x]) > 0.0f) {
			std::swap(f.v[1], f.v[2]);
			f.n = -f.n;
		}
		f.off = f.n.dot(P[f.v[0]]);
		fs.push_back(f);
	};
	add(a, b, c);
	add(a, b, d);
	add(a, c, d);
	add(b, c, d);
	for (int i = 0; i < n; i++) {
		if (i == a || i == b || i == c || i == d) {
			continue;
		}
		std::vector<int> vis;
		for (int f = 0; f < (int)fs.size(); f++) {
			if (fs[f].alive && fs[f].n.dot(P[i]) - fs[f].off > eps) {
				vis.push_back(f);
			}
		}
		if (vis.empty()) {
			continue;
		}
		std::unordered_set<uint64_t> edges;
		for (int f : vis) {
			for (int k = 0; k < 3; k++) {
				edges.insert(((uint64_t)(uint32_t)fs[f].v[k] << 32) | (uint32_t)fs[f].v[(k + 1) % 3]);
			}
			fs[f].alive = false;
		}
		for (uint64_t e : edges) {
			const int u = (int)(e >> 32), v = (int)(e & 0xffffffffu);
			if (!edges.count(((uint64_t)(uint32_t)v << 32) | (uint32_t)u)) {
				F f;
				f.v[0] = u;
				f.v[1] = v;
				f.v[2] = i;
				f.n = (P[v] - P[u]).cross(P[i] - P[u]).normalized();
				f.off = f.n.dot(P[u]);
				fs.push_back(f);
			}
		}
	}
	for (const F &f : fs) {
		if (f.alive) {
			r_tris.push_back(P[f.v[0]]);
			r_tris.push_back(P[f.v[1]]);
			r_tris.push_back(P[f.v[2]]);
		}
	}
}

// a solid mesh of a collision shape (null: a shape that is not drawn, e.g. a world boundary)
Ref<Mesh> shape_mesh(const Ref<Shape3D> &p_shape) {
	Shape3D *s = p_shape.ptr();
	if (BoxShape3D *bs = Object::cast_to<BoxShape3D>(s)) {
		Ref<BoxMesh> m;
		m.instantiate();
		m->set_size(bs->get_size());
		return m;
	}
	if (SphereShape3D *ss = Object::cast_to<SphereShape3D>(s)) {
		Ref<SphereMesh> m;
		m.instantiate();
		m->set_radius(ss->get_radius());
		m->set_height(ss->get_radius() * 2.0f);
		return m;
	}
	if (CapsuleShape3D *cs = Object::cast_to<CapsuleShape3D>(s)) {
		Ref<CapsuleMesh> m;
		m.instantiate();
		m->set_radius(cs->get_radius());
		m->set_height(cs->get_height());
		return m;
	}
	if (CylinderShape3D *cy = Object::cast_to<CylinderShape3D>(s)) {
		Ref<CylinderMesh> m;
		m.instantiate();
		m->set_top_radius(cy->get_radius());
		m->set_bottom_radius(cy->get_radius());
		m->set_height(cy->get_height());
		return m;
	}
	std::vector<Vector3> tris;
	if (ConcavePolygonShape3D *cc = Object::cast_to<ConcavePolygonShape3D>(s)) {
		const PackedVector3Array f = cc->get_faces();
		for (int64_t i = 0; i < f.size(); i++) {
			tris.push_back(f[i]);
		}
	} else if (ConvexPolygonShape3D *cv = Object::cast_to<ConvexPolygonShape3D>(s)) {
		hull_tris(cv->get_points(), tris);
	} else if (HeightMapShape3D *hm = Object::cast_to<HeightMapShape3D>(s)) {
		const int w = hm->get_map_width(), dp = hm->get_map_depth();
		const PackedFloat32Array h = hm->get_map_data();
		if (w >= 2 && dp >= 2 && h.size() >= (int64_t)w * dp) {
			auto at = [&](int x, int z) { return Vector3(x - (w - 1) * 0.5f, h[z * w + x], z - (dp - 1) * 0.5f); };
			for (int z = 0; z + 1 < dp; z++) {
				for (int x = 0; x + 1 < w; x++) {
					tris.push_back(at(x, z));
					tris.push_back(at(x + 1, z));
					tris.push_back(at(x, z + 1));
					tris.push_back(at(x + 1, z));
					tris.push_back(at(x + 1, z + 1));
					tris.push_back(at(x, z + 1));
				}
			}
		}
	}
	if (tris.size() < 3) {
		return Ref<Mesh>();
	}
	PackedVector3Array v, nr;
	for (size_t i = 0; i + 2 < tris.size(); i += 3) {
		const Vector3 fn = (tris[i + 1] - tris[i]).cross(tris[i + 2] - tris[i]).normalized();
		for (int k = 0; k < 3; k++) {
			v.push_back(tris[i + k]);
			nr.push_back(fn);
		}
	}
	Array arr;
	arr.resize(Mesh::ARRAY_MAX);
	arr[Mesh::ARRAY_VERTEX] = v;
	arr[Mesh::ARRAY_NORMAL] = nr;
	Ref<ArrayMesh> am;
	am.instantiate();
	am->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr);
	return am;
}

// every collision shape of the edited scene (also the ones Hewn makes inside its nodes), with its world transform
void each_shape(Node *p_root, const std::function<void(const Ref<Shape3D> &, const Transform3D &)> &p_fn) {
	std::vector<Node *> stack = { p_root };
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		for (int i = 0; i < x->get_child_count(true); i++) {
			stack.push_back(x->get_child(i, true));
		}
		if (CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(x)) {
			Node3D *owner3d = Object::cast_to<Node3D>(cs->get_parent());
			if (!cs->is_disabled() && cs->get_shape().is_valid() && owner3d && Object::cast_to<CollisionObject3D>(owner3d) &&
					(!Object::cast_to<Node3D>(owner3d->get_parent()) || Object::cast_to<Node3D>(owner3d->get_parent())->is_visible_in_tree())) {
				p_fn(cs->get_shape(), cs->get_global_transform());
			}
		}
		if (HewnScatter3D *sc = Object::cast_to<HewnScatter3D>(x)) {
			if (sc->is_visible_in_tree()) {
				sc->each_collision(p_fn);
			}
		}
	}
}
} // namespace

uint64_t HewnEditorPlugin::_collision_shapes_key() const {
	uint64_t key = 1469598103934665603ull;
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return key;
	}
	auto mix = [&](uint64_t x) { key = (key ^ x) * 1099511628211ull; };
	each_shape(root, [&](const Ref<Shape3D> &p_shape, const Transform3D &p_xf) {
		mix(p_shape->get_instance_id());
		mix((uint64_t)Variant(p_xf).hash());
		// (a shape changed in place: its size, its points)
		Shape3D *sh = p_shape.ptr();
		if (BoxShape3D *b = Object::cast_to<BoxShape3D>(sh)) {
			mix((uint64_t)Variant(b->get_size()).hash());
		} else if (SphereShape3D *sp = Object::cast_to<SphereShape3D>(sh)) {
			mix((uint64_t)Variant(sp->get_radius()).hash());
		} else if (CapsuleShape3D *ca = Object::cast_to<CapsuleShape3D>(sh)) {
			mix((uint64_t)Variant(Vector2(ca->get_radius(), ca->get_height())).hash());
		} else if (CylinderShape3D *cy = Object::cast_to<CylinderShape3D>(sh)) {
			mix((uint64_t)Variant(Vector2(cy->get_radius(), cy->get_height())).hash());
		} else if (ConvexPolygonShape3D *cv = Object::cast_to<ConvexPolygonShape3D>(sh)) {
			const PackedVector3Array pts = cv->get_points();
			mix((uint64_t)pts.size());
			if (!pts.is_empty()) {
				mix((uint64_t)Variant(pts[0] + pts[pts.size() / 2] * 3.0f + pts[pts.size() - 1] * 7.0f).hash());
			}
		} else if (ConcavePolygonShape3D *cc = Object::cast_to<ConcavePolygonShape3D>(sh)) {
			const PackedVector3Array f = cc->get_faces();
			mix((uint64_t)f.size());
			if (!f.is_empty()) {
				mix((uint64_t)Variant(f[0] + f[f.size() / 2] * 3.0f + f[f.size() - 1] * 7.0f).hash());
			}
		}
	});
	return key;
}

void HewnEditorPlugin::_collision_shapes_show() {
	_collision_shapes_clear();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	Node3D *root3 = Object::cast_to<Node3D>(root);
	// (the world the edited scene is drawn in)
	Ref<World3D> world = root3 && root3->is_inside_tree() ? root3->get_world_3d() : Ref<World3D>();
	if (world.is_null()) {
		SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(0);
		world = vp ? vp->get_world_3d() : Ref<World3D>();
	}
	if (!root || world.is_null()) {
		return;
	}
	if (collision_shape_mat.is_null()) {
		// (lit like the view: both sides, a shape seen from inside too)
		Ref<Shader> sh;
		sh.instantiate();
		sh->set_code(String("shader_type spatial;\nrender_mode unshaded, cull_disabled;\ninstance uniform vec4 obj_color = vec4(0.8, 0.8, 0.8, 1.0);\n") +
				"float studio(vec3 n) {\n\treturn 0.32 + 0.55 * max(dot(n, normalize(vec3(-0.4, 0.6, 0.7))), 0.0) + 0.22 * max(dot(n, normalize(vec3(0.7, 0.1, 0.4))), 0.0);\n}\n" +
				"vec3 to_linear(vec3 c) {\n\treturn mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));\n}\n" +
				"void fragment() {\n\tvec3 n = normalize(NORMAL);\n\tALBEDO = to_linear(obj_color.rgb * studio(n));\n}\n");
		collision_shape_mat.instantiate();
		collision_shape_mat->set_shader(sh);
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	const RID scenario = world->get_scenario();
	std::unordered_map<uint64_t, Ref<Mesh>> made;
	each_shape(root, [&](const Ref<Shape3D> &p_shape, const Transform3D &p_xf) {
		auto it = made.find(p_shape->get_instance_id());
		Ref<Mesh> m;
		if (it == made.end()) {
			m = shape_mesh(p_shape);
			made[p_shape->get_instance_id()] = m;
			if (m.is_valid()) {
				cshape_meshes.push_back(m);
			}
		} else {
			m = it->second;
		}
		if (m.is_null() || cshape_insts.size() > 20000) {
			return;
		}
		const RID inst = rs->instance_create2(m->get_rid(), scenario);
		rs->instance_set_transform(inst, p_xf);
		rs->instance_geometry_set_material_override(inst, collision_shape_mat->get_rid());
		rs->instance_geometry_set_cast_shadows_setting(inst, RenderingServer::SHADOW_CASTING_SETTING_OFF);
		const Color c = collision_view_color(shape_kind(p_shape));
		rs->instance_geometry_set_shader_parameter(inst, "obj_color", Vector4(c.r, c.g, c.b, 1.0f));
		cshape_insts.push_back(inst);
	});
}

// Every object shown of the edited scene and the kind of its collision (0 none, 1 triangles, 2 convex, 3 a simple
// shape, 4 other); the key: all of that, to tell when it changed.
uint64_t HewnEditorPlugin::_collision_view_scan(std::vector<std::pair<GeometryInstance3D *, int>> *r_items) const {
	uint64_t key = 1469598103934665603ull;
	auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
	auto add = [&](GeometryInstance3D *p_gi, int p_kind) {
		mix(p_gi->get_instance_id());
		mix((uint64_t)p_kind + 7);
		if (r_items) {
			r_items->push_back({ p_gi, p_kind });
		}
	};
	for (HewnMesh3D *m : _view_meshes()) {
		int k = 0;
		switch (m->get_collision_mode()) {
			case HewnMesh3D::COLLISION_NONE:
				k = 0;
				break;
			case HewnMesh3D::COLLISION_TRIMESH:
			case HewnMesh3D::COLLISION_SIMPLIFIED_TRIMESH:
				k = 1;
				break;
			case HewnMesh3D::COLLISION_CONVEX:
			case HewnMesh3D::COLLISION_SIMPLIFIED_CONVEX:
			case HewnMesh3D::COLLISION_MULTIPLE_CONVEX:
				k = 2;
				break;
			default:
				k = 3;
				break;
		}
		if (k == 0) {
			k = body_around(m); // (a body around it, or one under it)
		}
		add(m, k);
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return key;
	}
	std::vector<Node *> stack = { root };
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
		GeometryInstance3D *gi = Object::cast_to<GeometryInstance3D>(x);
		if (!gi || Object::cast_to<HewnMesh3D>(x) || !gi->is_visible_in_tree() || !gi->get_instance().is_valid()) {
			continue;
		}
		CSGShape3D *csg = Object::cast_to<CSGShape3D>(x);
		if (!Object::cast_to<MeshInstance3D>(x) && !csg) {
			continue; // (particles, sprites, lines: not solid things)
		}
		if (csg && !csg->is_root_shape()) {
			continue; // (its root shows it)
		}
		int k = csg && csg->is_using_collision() ? 1 : 0;
		k = k ? k : body_around(x);
		add(gi, k);
	}
	return key;
}

// The old Show Colors button: the painted channel on / off (in Vertex Paint).
void HewnEditorPlugin::_toggle_vertex_color_view() {
	if (!vpaint) {
		enter_vpaint();
	}
	paint_view = paint_view == 1 ? 0 : 1;
	if (dock && dock->paint_view_btns[paint_view]) {
		dock->paint_view_btns[paint_view]->set_pressed_no_signal(true);
	}
	apply_paint_view();
}

// ------------------------------------------------------------------ the Colors menu

void HewnEditorPlugin::paint_more_op(int p_id) {
	if (p_id == 5) {
		open_bake_light(); // (the scene's lights into the colors)
		return;
	}
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	if (p_id == 0) {
		// the selection with the first color
		const hewn::Selection &s = n->selection;
		const bool masked = mode == MODE_VERTEX ? !s.verts.empty() : mode == MODE_EDGE ? !s.edges.empty() : !s.faces.empty();
		if (!masked) {
			ERR_PRINT("Hewn: select what to fill first (or use the Fill tool on a part).");
			return;
		}
		begin_edit("Vertex Paint: Fill");
		Camera3D *cam = _viewport_camera();
		if (cam) {
			_paint_fill(cam, Vector2());
		}
		end_edit();
		return;
	}
	if (p_id == 1) {
		if (!replace_dialog) {
			replace_dialog = memnew(AcceptDialog);
			replace_dialog->set_title("Replace Color");
			replace_dialog->set_ok_button_text("Replace");
			GridContainer *gc = memnew(GridContainer);
			gc->set_columns(2);
			replace_dialog->add_child(gc);
			Label *l1 = memnew(Label);
			l1->set_text("Replace");
			gc->add_child(l1);
			replace_from = memnew(ColorPickerButton);
			replace_from->set_custom_minimum_size(Vector2(120, 26));
			gc->add_child(replace_from);
			Label *l2 = memnew(Label);
			l2->set_text("With");
			gc->add_child(l2);
			replace_to = memnew(ColorPickerButton);
			replace_to->set_custom_minimum_size(Vector2(120, 26));
			gc->add_child(replace_to);
			Label *l3 = memnew(Label);
			l3->set_text("Tolerance");
			gc->add_child(l3);
			replace_tol = memnew(SpinBox);
			HewnDragField::hook_spinbox(replace_tol);
			replace_tol->set_min(0.0);
			replace_tol->set_max(1.0);
			replace_tol->set_step(0.001);
			replace_tol->set_value(0.01);
			gc->add_child(replace_tol);
			gc->add_child(memnew(Control));
			replace_only_sel = memnew(CheckBox);
			replace_only_sel->set_text("Only the selection");
			replace_only_sel->set_pressed(true);
			gc->add_child(replace_only_sel);
			replace_dialog->connect("confirmed", callable_mp(this, &HewnEditorPlugin::_replace_colors));
			localize(replace_dialog);
			EditorInterface::get_singleton()->get_base_control()->add_child(replace_dialog);
		}
		replace_from->set_pick_color(dock->get_paint_color());
		replace_to->set_pick_color(dock->paint_color2->get_pick_color());
		replace_dialog->popup_centered();
		return;
	}
	if (p_id == 4) {
		if (!round_dialog) {
			round_dialog = memnew(AcceptDialog);
			round_dialog->set_title("Round Colors");
			round_dialog->set_ok_button_text("Round");
			GridContainer *gc = memnew(GridContainer);
			gc->set_columns(2);
			round_dialog->add_child(gc);
			Label *l1 = memnew(Label);
			l1->set_text("Step");
			gc->add_child(l1);
			round_step = memnew(SpinBox);
			HewnDragField::hook_spinbox(round_step);
			round_step->set_min(0.001);
			round_step->set_max(1.0);
			round_step->set_step(0.001);
			round_step->set_value(0.1);
			round_step->set_tooltip_text("The values go to the nearest multiple of this (0.5: 0, 0.5, 1; 1 / 255: whole 8-bit steps)");
			gc->add_child(round_step);
			gc->add_child(memnew(Control));
			round_only_sel = memnew(CheckBox);
			round_only_sel->set_text("Only the selection");
			round_only_sel->set_pressed(true);
			gc->add_child(round_only_sel);
			gc->add_child(memnew(Control));
			Label *l2 = memnew(Label);
			l2->set_text("(the channels turned on in the header)");
			l2->add_theme_color_override("font_color", Color(0.65f, 0.65f, 0.65f));
			gc->add_child(l2);
			round_dialog->connect("confirmed", callable_mp(this, &HewnEditorPlugin::_round_colors));
			localize(round_dialog);
			EditorInterface::get_singleton()->get_base_control()->add_child(round_dialog);
		}
		round_dialog->popup_centered();
		return;
	}
	if (p_id == 2 || p_id == 3) {
		if (!paint_file_dialog) {
			paint_file_dialog = memnew(EditorFileDialog);
			paint_file_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
			paint_file_dialog->add_filter("*.pfpaint", "Hewn painting");
			paint_file_dialog->connect("file_selected", callable_mp(this, &HewnEditorPlugin::_on_paint_file));
			EditorInterface::get_singleton()->get_base_control()->add_child(paint_file_dialog);
		}
		paint_file_save = p_id == 2;
		paint_file_dialog->set_file_mode(paint_file_save ? EditorFileDialog::FILE_MODE_SAVE_FILE : EditorFileDialog::FILE_MODE_OPEN_FILE);
		paint_file_dialog->set_title(paint_file_save ? "Save the Painting" : "Load a Painting");
		paint_file_dialog->popup_file_dialog();
	}
}

// Every corner of the painted channel near the first color (the selection only, when there is one) gets the
// second one; only the channels turned on.
void HewnEditorPlugin::_replace_colors() {
	HewnMesh3D *n = get_edited();
	if (!n || !replace_from) {
		return;
	}
	const int ch = dock->paint_channel ? dock->paint_channel->get_selected() : 0;
	const Color from = replace_from->get_pick_color(), to = replace_to->get_pick_color();
	const float tol = (float)replace_tol->get_value();
	const float sc = ch > 0 && dock->paint_scale ? (float)dock->paint_scale->get_value() : 1.0f;
	begin_edit("Replace Color");
	hewn::Mesh &m = n->get_data();
	const hewn::Selection &s = n->selection;
	std::vector<char> on;
	_paint_selected_corners(n, replace_only_sel && replace_only_sel->is_pressed(), on);
	size_t ci = 0;
	for (size_t f = 0; f < m.faces.size(); f++) {
		hewn::Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++, ci++) {
			if (!on[ci]) {
				continue;
			}
			F.ensure_attrs();
			Color &x = _paint_value(F, i, ch);
			bool near = true;
			for (int k = 0; k < 4; k++) {
				if (_paint_channel_on(k)) {
					near = near && std::fabs(x[k] - from[k] * sc) <= tol + 1e-6f;
				}
			}
			if (near) {
				for (int k = 0; k < 4; k++) {
					if (_paint_channel_on(k)) {
						x[k] = to[k] * sc;
					}
				}
			}
		}
	}
	n->commit(false);
	end_edit();
}

// Which corners an operation changes (in order): the selected elements' (by the mode), or all of them.
void HewnEditorPlugin::_paint_selected_corners(HewnMesh3D *p_node, bool p_only_selection, std::vector<char> &r_on) const {
	const hewn::Mesh &m = p_node->get_data();
	const hewn::Selection &s = p_node->selection;
	r_on.clear();
	std::unordered_set<int> vs(s.verts.begin(), s.verts.end()), fs(s.faces.begin(), s.faces.end());
	std::unordered_set<uint64_t> es;
	for (const hewn::Edge &e : s.edges) {
		es.insert(hewn::ekey(e));
	}
	const bool any = mode == MODE_VERTEX ? !vs.empty() : mode == MODE_EDGE ? !es.empty() : !fs.empty();
	for (size_t f = 0; f < m.faces.size(); f++) {
		const hewn::Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			bool on = true;
			if (p_only_selection && any) {
				if (mode == MODE_VERTEX) {
					on = vs.count(F.v[i]) > 0;
				} else if (mode == MODE_EDGE) {
					on = es.count(hewn::ekey(F.v[i], F.v[(i + 1) % F.size()])) || es.count(hewn::ekey(F.v[(i + F.size() - 1) % F.size()], F.v[i]));
				} else {
					on = fs.count((int)f) > 0;
				}
			} else if (p_only_selection) {
				on = false; // (nothing selected: nothing to do)
			}
			r_on.push_back(on ? 1 : 0);
		}
	}
}

// Every value of the painted channel (the channels turned on) to the nearest multiple of the step.
void HewnEditorPlugin::_round_colors() {
	HewnMesh3D *n = get_edited();
	if (!n || !round_step) {
		return;
	}
	const int ch = dock->paint_channel ? dock->paint_channel->get_selected() : 0;
	const float st = MAX(1e-4f, (float)round_step->get_value());
	std::vector<char> on;
	_paint_selected_corners(n, round_only_sel->is_pressed(), on);
	begin_edit("Round Colors");
	hewn::Mesh &m = n->get_data();
	size_t ci = 0;
	for (hewn::Face &F : m.faces) {
		for (int i = 0; i < F.size(); i++, ci++) {
			if (!on[ci]) {
				continue;
			}
			F.ensure_attrs();
			Color &x = _paint_value(F, i, ch);
			for (int k = 0; k < 4; k++) {
				if (_paint_channel_on(k)) {
					x[k] = std::round(x[k] / st) * st;
				}
			}
		}
	}
	n->commit(false);
	end_edit();
}

// A painting saved (the vertex colors and CUSTOM0..3 of every face corner) or loaded back onto a mesh with
// the same faces.
void HewnEditorPlugin::_on_paint_file(const String &p_path) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	hewn::Mesh &m = n->get_data();
	PackedInt32Array sizes;
	for (const hewn::Face &F : m.faces) {
		sizes.push_back(F.size());
	}
	if (paint_file_save) {
		PackedColorArray col, cust;
		bool any_custom = false;
		for (const hewn::Face &F : m.faces) {
			any_custom = any_custom || !F.cust.empty();
		}
		for (const hewn::Face &F : m.faces) {
			for (int i = 0; i < F.size(); i++) {
				col.push_back(corner_value(F, i, 0));
				if (any_custom) {
					for (int k = 1; k <= 4; k++) {
						cust.push_back(corner_value(F, i, k));
					}
				}
			}
		}
		Dictionary d;
		d["face_sizes"] = sizes;
		d["colors"] = col;
		d["custom"] = cust;
		d["custom_formats"] = n->get_custom_formats();
		Ref<FileAccess> fa = FileAccess::open(p_path, FileAccess::WRITE);
		if (fa.is_null()) {
			ERR_PRINT("Hewn: could not write " + p_path);
			return;
		}
		fa->store_var(d);
		return;
	}
	Ref<FileAccess> fa = FileAccess::open(p_path, FileAccess::READ);
	if (fa.is_null()) {
		ERR_PRINT("Hewn: could not read " + p_path);
		return;
	}
	const Dictionary d = fa->get_var();
	if (d.get("face_sizes", PackedInt32Array()) != Variant(sizes)) {
		ERR_PRINT("Hewn: this painting was made for other faces (the mesh changed since).");
		return;
	}
	const PackedColorArray col = d.get("colors", PackedColorArray()), cust = d.get("custom", PackedColorArray());
	begin_edit("Load Painting");
	int64_t k = 0;
	for (hewn::Face &F : m.faces) {
		F.ensure_attrs();
		if (!cust.is_empty()) {
			F.cust.assign(F.v.size() * 4, Color(0, 0, 0, 0));
		} else {
			F.cust.clear();
		}
		for (int i = 0; i < F.size(); i++, k++) {
			if (k < col.size()) {
				F.col[i] = col[k];
			}
			for (int c = 0; c < 4 && !cust.is_empty(); c++) {
				if (k * 4 + c < cust.size()) {
					F.cust[i * 4 + c] = cust[k * 4 + c];
				}
			}
		}
	}
	const PackedInt32Array fm = d.get("custom_formats", PackedInt32Array());
	for (int c = 0; c < 4 && c < fm.size(); c++) {
		n->custom_formats[c] = fm[c];
	}
	n->commit(false);
	end_edit();
}

// ------------------------------------------------------------------ isolate

// Everything but the selected / edited objects hidden from the editor's view (not saved: only how it is
// drawn); again: all back.
void HewnEditorPlugin::set_isolate(bool p_on) {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (const RID &r : isolate_hidden) {
		rs->instance_set_visible(r, true);
	}
	isolate_hidden.clear();
	isolated = p_on;
	if (dock && dock->isolate_btn && dock->isolate_btn->is_pressed() != p_on) {
		dock->isolate_btn->set_pressed_no_signal(p_on);
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!p_on || !root) {
		update_overlays();
		return;
	}
	std::vector<Node *> keep;
	for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_selected_nodes()) {
		if (Node *x = Object::cast_to<Node>(v)) {
			keep.push_back(x);
		}
	}
	for (HewnMesh3D *m : edited_meshes()) {
		keep.push_back(m);
	}
	for (const Variant &v : root->find_children("*", "VisualInstance3D", true, false)) {
		VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(v);
		if (!vi || !vi->is_visible_in_tree()) {
			continue;
		}
		bool kept = false;
		for (Node *k : keep) {
			kept = kept || k == vi || k->is_ancestor_of(vi);
		}
		if (!kept && vi->get_instance().is_valid()) {
			rs->instance_set_visible(vi->get_instance(), false);
			isolate_hidden.push_back(vi->get_instance());
		}
	}
	update_overlays();
}

// ------------------------------------------------------------------ blend material

void HewnEditorPlugin::open_blend_dialog() {
	if (!blend_dialog) {
		const float es = EditorInterface::get_singleton()->get_editor_scale();
		blend_dialog = memnew(AcceptDialog);
		blend_dialog->set_title("Blend Material");
		blend_dialog->set_ok_button_text("Create");
		VBoxContainer *v = memnew(VBoxContainer);
		blend_dialog->add_child(v);
		HBoxContainer *sr = memnew(HBoxContainer);
		v->add_child(sr);
		Label *sl = memnew(Label);
		sl->set_text("Amounts from");
		sr->add_child(sl);
		blend_source = memnew(OptionButton);
		for (const char *it : { "CUSTOM0", "CUSTOM1", "CUSTOM2", "CUSTOM3", "COLOR" }) {
			blend_source->add_item(it);
		}
		blend_source->set_tooltip_text("The painted channel whose R / G / B / A show the layers (paint it in Vertex Paint)");
		sr->add_child(blend_source);
		GridContainer *gc = memnew(GridContainer);
		gc->set_columns(2);
		v->add_child(gc);
		Label *bl = memnew(Label);
		bl->set_text("Base texture");
		gc->add_child(bl);
		blend_base = memnew(EditorResourcePicker);
		blend_base->set_base_type("Texture2D");
		blend_base->set_custom_minimum_size(Vector2(200 * es, 0));
		gc->add_child(blend_base);
		// layer 1 by R, 2 by G, 3 by B, 4 by A (one layer for each component)
		static const char *comps[4] = { "R", "G", "B", "A" };
		for (int i = 0; i < 4; i++) {
			blend_on[i] = memnew(CheckBox);
			blend_on[i]->set_text(vformat(T("Layer %d (%s)"), i + 1, comps[i]));
			blend_on[i]->set_pressed(i < 2);
			gc->add_child(blend_on[i]);
			blend_tex[i] = memnew(EditorResourcePicker);
			blend_tex[i]->set_base_type("Texture2D");
			blend_tex[i]->set_custom_minimum_size(Vector2(200 * es, 0));
			gc->add_child(blend_tex[i]);
		}
		blend_height = memnew(CheckBox);
		blend_height->set_text("Blend by height");
		blend_height->set_pressed(true);
		blend_height->set_tooltip_text("Each texture's height (its brightness, alpha or a height map: in the Inspector) plus its amount: the highest shows, so where two meet the border follows the stones / bricks instead of a soft smear");
		v->add_child(blend_height);
		HBoxContainer *fr = memnew(HBoxContainer);
		v->add_child(fr);
		Label *fl = memnew(Label);
		fl->set_text("Filter");
		fr->add_child(fl);
		blend_filter = memnew(OptionButton);
		blend_filter->add_item("Linear");
		blend_filter->add_item("Closest");
		blend_filter->set_tooltip_text("Linear: smooth; Closest: crisp pixels");
		fr->add_child(blend_filter);
		blend_vcolor = memnew(CheckBox);
		blend_vcolor->set_text("Vertex colors");
		blend_vcolor->set_tooltip_text("The vertex colors (Vertex Paint, COLOR) multiply the result; not when the amounts come from COLOR");
		v->add_child(blend_vcolor);
		blend_pbr = memnew(CheckBox);
		blend_pbr->set_text("Metallic and roughness");
		blend_pbr->set_pressed(true);
		blend_pbr->set_tooltip_text("Each texture gets its own metallic and roughness (and a roughness map), set in the Inspector");
		v->add_child(blend_pbr);
		blend_normal = memnew(CheckBox);
		blend_normal->set_text("Normal maps");
		blend_normal->set_pressed(true);
		blend_normal->set_tooltip_text("Each texture gets a normal map and its strength, set in the Inspector");
		v->add_child(blend_normal);
		blend_scroll = memnew(CheckBox);
		blend_scroll->set_text("Scrolling");
		blend_scroll->set_tooltip_text("The textures move across the faces (water, lava...): its way (degrees) and speed in the Inspector (a shader of its own)");
		v->add_child(blend_scroll);
		blend_emission = memnew(CheckBox);
		blend_emission->set_text("Emission");
		blend_emission->set_tooltip_text("Each texture gets an emission color, energy and map, set in the Inspector");
		v->add_child(blend_emission);
		blend_dialog->connect("confirmed", callable_mp(this, &HewnEditorPlugin::_make_blend_material));
		localize(blend_dialog);
		EditorInterface::get_singleton()->get_base_control()->add_child(blend_dialog);
	}
	blend_dialog->popup_centered();
}

// The shader for so many layers: made once in res://materials and shared by every blend material with as many
// layers (Godot then keeps one pipeline for them); the material holds the textures and which component each
// layer follows.
void HewnEditorPlugin::_make_blend_material() {
	if (texture_dock && texture_dock->ask_material_dir(callable_mp(this, &HewnEditorPlugin::_make_blend_material))) {
		return; // (made once the folder is chosen)
	}
	const String mdir = texture_dock ? texture_dock->material_dir() : String("res://materials");
	std::vector<int> layers;
	for (int i = 0; i < 4; i++) {
		if (blend_on[i]->is_pressed()) {
			layers.push_back(i);
		}
	}
	const int count = (int)layers.size();
	const bool nearest = blend_filter && blend_filter->get_selected() == 1;
	const String filt = nearest ? "filter_nearest_mipmap" : "filter_linear_mipmap"; // (a sampler's filter is fixed in its shader)
	const bool pbr = blend_pbr && blend_pbr->is_pressed();
	const bool nmap = blend_normal && blend_normal->is_pressed();
	const bool emis = blend_emission && blend_emission->is_pressed();
	const bool scroll = blend_scroll && blend_scroll->is_pressed();
	String fname = "hewn_blend_" + itos(count);
	fname += String(pbr ? "_pbr" : "") + (nmap ? "_normal" : "") + (emis ? "_emission" : "") + (nearest ? "_nearest" : "") + (scroll ? "_scroll" : "");
	const String path = mdir.path_join(fname + ".gdshader");
	Ref<Shader> sh;
	if (ResourceLoader::get_singleton()->exists(path)) {
		sh = ResourceLoader::get_singleton()->load(path);
	}
	const String marker = "// Hewn blend material v5";
	if (sh.is_null() || !sh->get_code().begins_with(marker)) {
		// (made by Hewn: an older one is written again)
		// Height-based splatting: each texture's height plus its amount; only the textures near the highest of
		// these show, so where the amounts meet the higher parts (stones, bricks) win and the border follows them.
		String code = marker + String(": a base and ") + itos(count) + " layer(s), each shown by a component of a painted channel\n";
		code += "shader_type spatial;\n";
		code += "uniform int source : hint_enum(\"CUSTOM0\", \"CUSTOM1\", \"CUSTOM2\", \"CUSTOM3\", \"COLOR\") = 0;\n";
		code += "uniform vec2 uv_scale = vec2(1.0);\n";
		if (scroll) {
			code += "// the textures moving: their way (degrees: 0 along U) and how fast (images per second)\n";
			code += "uniform float scroll_angle : hint_range(-360.0, 360.0) = 0.0;\n";
			code += "uniform float scroll_speed = 0.1;\n";
		}
		if (!pbr) {
			code += "uniform float roughness : hint_range(0.0, 1.0) = 0.8;\n";
		}
		code += "// by height: each texture's height plus its amount, the highest wins; off: a plain mix by the amounts\n";
		code += "uniform bool height_blend = true;\n";
		code += "// how much the heights count against the amounts\n";
		code += "uniform float height_strength : hint_range(0.0, 4.0) = 1.0;\n";
		code += "// the width of the transition (small: a sharp border along the heights)\n";
		code += "uniform float blend_depth : hint_range(0.001, 1.0) = 0.1;\n";
		code += "// the vertex colors multiply the result (not when the amounts come from COLOR)\n";
		code += "uniform bool vertex_color_tint = false;\n";
		code += "uniform sampler2D base_texture : source_color, hint_default_white, filter_linear_mipmap, repeat_enable;\n";
		code += "uniform vec4 base_color : source_color = vec4(1.0);\n";
		code += "uniform int base_height_from : hint_enum(\"Brightness\", \"Height Map\", \"Alpha\") = 0;\n";
		// the extra maps of texture i (0: the base): uniforms named base_x or x_n
		auto un = [](int i, const String &x) { return i == 0 ? String("base_") + x : x + String("_") + itos(i); };
		auto extra_uniforms = [&](int i) {
			String c;
			if (pbr) {
				c += String("uniform float ") + un(i, "metallic") + " : hint_range(0.0, 1.0) = 0.0;\n";
				c += String("uniform float ") + un(i, "roughness") + " : hint_range(0.0, 1.0) = 0.8;\n";
				c += String("uniform sampler2D ") + un(i, "roughness_map") + " : hint_roughness_r, filter_linear_mipmap, repeat_enable;\n";
			}
			if (nmap) {
				c += String("uniform sampler2D ") + un(i, "normal_map") + " : hint_normal, filter_linear_mipmap, repeat_enable;\n";
				c += String("uniform float ") + un(i, "normal_strength") + " : hint_range(-4.0, 4.0) = 1.0;\n";
			}
			if (emis) {
				c += String("uniform vec4 ") + un(i, "emission") + " : source_color = vec4(0.0, 0.0, 0.0, 1.0);\n";
				c += String("uniform float ") + un(i, "emission_energy") + " : hint_range(0.0, 16.0) = 1.0;\n";
				c += String("uniform sampler2D ") + un(i, "emission_map") + " : source_color, hint_default_white, filter_linear_mipmap, repeat_enable;\n";
			}
			return c;
		};
		code += "uniform sampler2D base_height : hint_default_white, filter_linear_mipmap, repeat_enable;\n";
		code += extra_uniforms(0);
		for (int i = 0; i < count; i++) {
			const String n = itos(i + 1);
			code += String("uniform sampler2D layer_") + n + " : source_color, hint_default_white, filter_linear_mipmap, repeat_enable;\n";
			code += String("uniform vec4 color_") + n + " : source_color = vec4(1.0);\n";
			code += String("uniform int amount_") + n + String(" : hint_enum(\"R\", \"G\", \"B\", \"A\") = ") + itos(MIN(i, 3)) + ";\n";
			code += String("uniform int height_from_") + n + " : hint_enum(\"Brightness\", \"Height Map\", \"Alpha\") = 0;\n";
			code += String("uniform sampler2D height_") + n + " : hint_default_white, filter_linear_mipmap, repeat_enable;\n";
			code += extra_uniforms(i + 1);
		}
		code += "varying vec4 amounts;\nvarying vec4 vcolor;\n";
		code += "void vertex() {\n\tamounts = source == 0 ? CUSTOM0 : source == 1 ? CUSTOM1 : source == 2 ? CUSTOM2 : source == 3 ? CUSTOM3 : COLOR;\n\tvcolor = COLOR;\n}\n";
		code += "float height_of(int from, vec4 tex, float map) {\n\treturn from == 1 ? map : from == 2 ? tex.a : dot(tex.rgb, vec3(0.299, 0.587, 0.114));\n}\n";
		code += String("void fragment() {\n\tvec2 uv = UV * uv_scale") + (scroll ? " + vec2(cos(radians(scroll_angle)), sin(radians(scroll_angle))) * scroll_speed * TIME" : "") + ";\n";
		code += "\tvec4 t0 = texture(base_texture, uv);\n";
		code += "\tfloat h0 = height_of(base_height_from, t0, base_height_from == 1 ? texture(base_height, uv).r : 0.0);\n";
		code += "\tvec3 c0 = t0.rgb * base_color.rgb;\n";
		code += "\tfloat sum = 0.0;\n";
		String m_max, b_list, b_sum = "b0";
		for (int i = 0; i < count; i++) {
			const String n = itos(i + 1);
			code += String("\tvec4 t") + n + String(" = texture(layer_") + n + ", uv);\n";
			code += String("\tfloat h") + n + String(" = height_of(height_from_") + n + String(", t") + n + String(", height_from_") + n + String(" == 1 ? texture(height_") + n + ", uv).r : 0.0);\n";
			code += String("\tvec3 c") + n + String(" = t") + n + String(".rgb * color_") + n + ".rgb;\n";
			code += String("\tfloat w") + n + String(" = clamp(amounts[amount_") + n + "], 0.0, 1.0);\n";
			code += String("\tsum += w") + n + ";\n";
			m_max += String("\t\tm = max(m, h") + n + String(" * height_strength + w") + n + ");\n";
			b_list += String("\t\tfloat b") + n + String(" = max(h") + n + String(" * height_strength + w") + n + String(" - m, 0.0) * step(1e-4, w") + n + ");\n";
			b_sum += String(" + b") + n;
		}
		// the base: what the layers leave; more than all of it: shared out
		code += "\tfloat w0 = max(1.0 - sum, 0.0);\n\tfloat total = max(sum + w0, 1e-5);\n\tw0 /= total;\n";
		for (int i = 0; i < count; i++) {
			code += String("\tw") + itos(i + 1) + " /= total;\n";
		}
		// the final weight of each texture (f0: the base), then every output mixed by them
		String f_decl = "\tfloat f0";
		String f_plain = "\t\tf0 = w0;\n", f_height = String("\t\tfloat bs = max(") + b_sum + ", 1e-5);\n\t\tf0 = b0 / bs;\n";
		for (int i = 0; i < count; i++) {
			const String n = itos(i + 1);
			f_decl += String(", f") + n;
			f_plain += String("\t\tf") + n + String(" = w") + n + ";\n";
			f_height += String("\t\tf") + n + String(" = b") + n + " / bs;\n";
		}
		code += f_decl + ";\n";
		code += String("\tif (!height_blend) {\n") + f_plain + "\t} else {\n";
		code += String("\t\tfloat m = h0 * height_strength + w0;\n") + m_max + "\t\tm -= blend_depth;\n";
		code += "\t\t// (a texture with no amount here never shows, however high it is)\n";
		code += String("\t\tfloat b0 = max(h0 * height_strength + w0 - m, 0.0) * step(1e-4, w0);\n") + b_list + f_height + "\t}\n";
		auto sum_of = [&](const std::function<String(int)> &p_term) {
			String r;
			for (int i = 0; i <= count; i++) {
				r += String(i ? " + " : "") + String("(") + p_term(i) + String(") * f") + itos(i);
			}
			return r;
		};
		code += String("\tALBEDO = ") + sum_of([](int i) { return String("c") + itos(i); }) + ";\n";
		code += "\tif (vertex_color_tint && source != 4) {\n\t\tALBEDO *= vcolor.rgb;\n\t}\n";
		if (pbr) {
			code += String("\tMETALLIC = ") + sum_of([&](int i) { return un(i, "metallic"); }) + ";\n";
			code += String("\tROUGHNESS = ") + sum_of([&](int i) { return un(i, "roughness") + String(" * texture(") + un(i, "roughness_map") + ", uv).r"; }) + ";\n";
		} else {
			code += "\tROUGHNESS = roughness;\n";
		}
		if (nmap) {
			// the tangent-space normals, each scaled by its strength, mixed and packed again
			code += String("\tvec3 nm = ") + sum_of([&](int i) {
				return String("vec3((texture(") + un(i, "normal_map") + String(", uv).xy * 2.0 - 1.0) * ") + un(i, "normal_strength") + ", 1.0)";
			}) + ";\n";
			code += "\tNORMAL_MAP = normalize(nm) * 0.5 + 0.5;\n";
		}
		if (emis) {
			code += String("\tEMISSION = ") + sum_of([&](int i) {
				return un(i, "emission") + String(".rgb * ") + un(i, "emission_energy") + String(" * texture(") + un(i, "emission_map") + ", uv).rgb";
			}) + ";\n";
		}
		code += "}\n";
		code = code.replace("filter_linear_mipmap", filt);
		if (sh.is_null()) {
			sh.instantiate();
		}
		sh->set_code(code);
		if (!DirAccess::dir_exists_absolute(mdir)) {
			DirAccess::make_dir_recursive_absolute(mdir);
		}
		if (ResourceSaver::get_singleton()->save(sh, path) == OK) {
			sh = ResourceLoader::get_singleton()->load(path);
		}
	}
	Ref<ShaderMaterial> mat;
	mat.instantiate();
	mat->set_shader(sh);
	mat->set_meta("hw_blend_material", true);
	mat->set_shader_parameter("source", blend_source->get_selected());
	if (Ref<Texture2D>(blend_base->get_edited_resource()).is_valid()) {
		mat->set_shader_parameter("base_texture", blend_base->get_edited_resource());
	}
	for (int k = 0; k < count; k++) {
		const int i = layers[k];
		const String n = itos(k + 1);
		if (Ref<Texture2D>(blend_tex[i]->get_edited_resource()).is_valid()) {
			mat->set_shader_parameter("layer_" + n, blend_tex[i]->get_edited_resource());
		}
		mat->set_shader_parameter("amount_" + n, i); // (layer 1: R, 2: G, 3: B, 4: A)
	}
	mat->set_shader_parameter("height_blend", blend_height && blend_height->is_pressed());
	mat->set_shader_parameter("vertex_color_tint", blend_vcolor && blend_vcolor->is_pressed());
	apply_material_to_selection(mat);
	hw_editor::edit_in_inspector(mat);
	// the channel painted next: the one the material reads
	if (dock && dock->paint_channel && blend_source->get_selected() < 4) {
		dock->paint_channel->select(blend_source->get_selected() + 1);
	}
}
