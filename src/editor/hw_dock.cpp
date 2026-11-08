// Hewn main dock: Create / Edit / Object / Material / UV tabs.
#include "hw_editor.h"
#include <godot_cpp/classes/editor_settings.hpp>
#include "hw_editor_util.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/button_group.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_flow_container.hpp>
#include <godot_cpp/classes/h_separator.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/v_separator.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/menu_button.hpp>
#include <godot_cpp/classes/popup_panel.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/theme.hpp>

#include "hw_icons.gen.h"
#include <godot_cpp/classes/input_event_mouse_button.hpp>

using namespace godot;

namespace {

Label *header(Container *p_parent, const String &p_text) {
	Label *l = memnew(Label);
	l->set_text(p_text);
	l->add_theme_color_override("font_color", Color(0.7f, 0.8f, 1.0f));
	p_parent->add_child(l);
	return l;
}

HFlowContainer *flow(Container *p_parent) {
	HFlowContainer *f = memnew(HFlowContainer);
	p_parent->add_child(f);
	return f;
}

SpinBox *spin(Container *p_parent, const String &p_label, double p_min, double p_max, double p_step, double p_value) {
	HBoxContainer *h = memnew(HBoxContainer);
	Label *l = memnew(Label);
	l->set_text(p_label);
	l->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	h->add_child(l);
	SpinBox *s = memnew(SpinBox);
	HewnDragField::hook_spinbox(s);
	s->set_min(p_min);
	s->set_max(p_max);
	s->set_step(p_step);
	s->set_value(p_value);
	s->set_allow_greater(true);
	s->set_custom_minimum_size(Vector2(90, 0));
	h->add_child(s);
	p_parent->add_child(h);
	return s;
}

VBoxContainer *tab(TabContainer *p_tabs, const String &p_name) {
	ScrollContainer *sc = memnew(ScrollContainer);
	sc->set_name(p_name);
	sc->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	VBoxContainer *v = memnew(VBoxContainer);
	v->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	sc->add_child(v);
	p_tabs->add_child(sc);
	return v;
}

} // namespace

void HewnDock::setup(HewnEditorPlugin *p_plugin) {
	plugin = p_plugin;
	set_name("Hewn");
	set_title("Hewn");
	set_default_slot(EditorDock::DOCK_SLOT_RIGHT_UL);

	VBoxContainer *root = memnew(VBoxContainer);
	add_child(root);
	status = memnew(Label);
	status->set_text("Select a HewnMesh3D");
	status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	// A wrapping label that has never been laid out has zero width, which makes its minimum height
	// one line per word; inside a hidden dock tab that stretched the whole editor layout.
	status->set_custom_minimum_size(Vector2(160, 0));
	root->add_child(status);

	tabs = memnew(TabContainer);
	tabs->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	root->add_child(tabs);
	_build_create_tab();
	_build_edit_tab();
	_build_object_tab();
	_build_material_tab();
	_build_uv_tab();
	// the Element Inspector: all that is known of the selection, and what can be changed there
	ei_tab_body = tab(tabs, "Inspector");
	ei_tab_body->set_name("ElementInspector");
	// every field: the revert arrow when changed (its default: the value it starts with)
	hw_editor::add_reverts_under(tabs, PackedStringArray({ "ElementInspector" }));
	relax_widths(tabs);
	tabs->set_current_tab(1);
	_build_toolbar();
	_build_tool_column();
	refresh();
}

#define OP_BUTTON(m_parent, m_text, m_op, m_tip)                                               \
	{                                                                                          \
		Button *b_ = memnew(Button);                                                           \
		b_->set_text(m_text);                                                                  \
		b_->set_tooltip_text(m_tip);                                                           \
		b_->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::run_op).bind(m_op)); \
		m_parent->add_child(b_);                                                               \
	}

// Long one-line texts (headers, hints, check boxes) wrap instead of holding the dock wide: it can be made
// as narrow as the other docks. (A wrapping label gets a minimum width: with none, a hidden tab's label
// is a word per line and stretches the layout.)
void HewnDock::relax_widths(Node *p_node) {
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const bool in_column = Object::cast_to<VBoxContainer>(p_node) != nullptr;
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *c = p_node->get_child(i);
		if (Label *l = Object::cast_to<Label>(c)) {
			if (in_column && l->get_autowrap_mode() == TextServer::AUTOWRAP_OFF) {
				l->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
				l->set_custom_minimum_size(Vector2(140 * es, l->get_custom_minimum_size().y));
			} else if (l->get_autowrap_mode() != TextServer::AUTOWRAP_OFF && l->get_custom_minimum_size().x > 140 * es) {
				l->set_custom_minimum_size(Vector2(140 * es, l->get_custom_minimum_size().y));
			}
		} else if (CheckBox *cb = Object::cast_to<CheckBox>(c)) {
			if (in_column) {
				cb->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
				cb->set_custom_minimum_size(Vector2(140 * es, cb->get_custom_minimum_size().y));
			}
		} else if (OptionButton *ob = Object::cast_to<OptionButton>(c)) {
			// (a choice is as wide as the panel lets it, its text cut: not as wide as its longest item)
			ob->set_fit_to_longest_item(false);
			ob->set_clip_text(true);
			ob->set_custom_minimum_size(Vector2(MIN(ob->get_custom_minimum_size().x, 56 * es), ob->get_custom_minimum_size().y));
		} else if (SpinBox *sb = Object::cast_to<SpinBox>(c)) {
			sb->set_custom_minimum_size(Vector2(MIN(sb->get_custom_minimum_size().x, 52 * es), sb->get_custom_minimum_size().y));
			sb->get_line_edit()->add_theme_constant_override("minimum_character_width", 2);
		} else if (HewnDragField *df = Object::cast_to<HewnDragField>(c)) {
			df->set_custom_minimum_size(Vector2(MIN(df->get_custom_minimum_size().x, 52 * es), df->get_custom_minimum_size().y));
			df->add_theme_constant_override("minimum_character_width", 2);
		}
		if (!in_column && Object::cast_to<HBoxContainer>(p_node)) {
			// a row: its labels and buttons give way (cut with "...", the whole text in the tooltip)
			if (Label *rl = Object::cast_to<Label>(c)) {
				if (rl->get_autowrap_mode() == TextServer::AUTOWRAP_OFF) {
					rl->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
					rl->set_custom_minimum_size(Vector2(MAX(rl->get_custom_minimum_size().x, 36 * es), rl->get_custom_minimum_size().y));
					if (rl->get_tooltip_text().is_empty()) {
						rl->set_tooltip_text(rl->get_text());
						rl->set_mouse_filter(Control::MOUSE_FILTER_PASS);
					}
				}
			} else if (Button *rb = Object::cast_to<Button>(c)) {
				if (!rb->get_text().is_empty() && !Object::cast_to<CheckBox>(rb) && !Object::cast_to<OptionButton>(rb)) {
					rb->set_clip_text(true);
					rb->set_custom_minimum_size(Vector2(MAX(rb->get_custom_minimum_size().x, 48 * es), rb->get_custom_minimum_size().y));
					rb->set_h_size_flags(rb->get_h_size_flags() | Control::SIZE_EXPAND);
				}
			}
		}
		relax_widths(c);
	}
}

// Second toolbar row under the 3D editor's own toolbar: element mode,
// gizmo, transform orientation and pivot, snapping, proportional editing, auto merge, X-ray.
// Icons come from the editor theme where Godot has one, otherwise from Tabler Icons (MIT, embedded).
Ref<Texture2D> HewnDock::icon(const String &p_name) {
	if (icon_cache.has(p_name)) {
		return icon_cache[p_name];
	}
	Ref<Texture2D> tex;
	for (const hw_icons::Icon &ic : hw_icons::ICONS) {
		if (p_name == ic.name) {
			// Tabler icons use currentColor: tint them like the editor's own icons.
			const Color fc = EditorInterface::get_singleton()->get_editor_theme()->get_color("font_color", "Editor");
			String svg = String(ic.svg).replace("currentColor", "#" + fc.to_html(false));
			Ref<Image> img;
			img.instantiate();
			const float scale = 16.0f * EditorInterface::get_singleton()->get_editor_scale() / 24.0f;
			if (img->load_svg_from_string(svg, scale) == OK) {
				tex = ImageTexture::create_from_image(img);
			}
			break;
		}
	}
	if (tex.is_null()) {
		tex = EditorInterface::get_singleton()->get_editor_theme()->get_icon(p_name, "EditorIcons");
	}
	icon_cache[p_name] = tex;
	return tex;
}

// Icon of a shape type (Tabler icons, src/editor/icons/shape_*.svg).
Ref<Texture2D> HewnDock::shape_icon(int p_type) {
	if (p_type == hewn::shapes::CURVED_STAIRS || p_type == hewn::shapes::PATH_STAIRS) {
		p_type = hewn::shapes::STAIRS;
	}
	return icon("shape_" + String(hewn::shapes::type_name((hewn::shapes::Type)p_type)).to_lower().replace(" ", "_"));
}

namespace {

Button *icon_button(Container *p_parent, const Ref<Texture2D> &p_icon, const String &p_tip, bool p_toggle) {
	Button *b = memnew(Button);
	b->set_button_icon(p_icon);
	b->set_flat(true);
	b->set_toggle_mode(p_toggle);
	b->set_tooltip_text(p_tip);
	b->set_theme_type_variation("FlatMenuButton");
	p_parent->add_child(b);
	return b;
}

} // namespace

const int HewnDock::SNAP_ORDER[HewnDock::SNAP_TARGET_COUNT] = { SNAP_INCREMENT, SNAP_GRID, SNAP_VERTEX, SNAP_EDGE, SNAP_FACE, SNAP_EDGE_CENTER,
	SNAP_FACE_CENTER };
namespace {
const char *SNAP_TARGET_NAMES[HewnDock::SNAP_TARGET_COUNT] = { "Increment", "Grid", "Vertex", "Edge", "Face", "Edge Center", "Face Center" };
const char *SNAP_TARGET_ICONS[HewnDock::SNAP_TARGET_COUNT] = { "SnapGrid", "Grid", "vertex", "edge", "face", "edge_center", "face_center" };
const char *SNAP_TARGET_TIPS[HewnDock::SNAP_TARGET_COUNT] = { "Move / rotate / scale in steps from where the transform started",
	"Onto the grid: the transformed point lands on grid points (steps from the origin)", "Onto vertices", "Onto edges",
	"Onto faces (surfaces)", "Onto the middle of edges", "Onto the center of faces" };
} // namespace

const char *HewnDock::ORIENT_NAMES[7] = { "Global", "Local", "Normal", "Gimbal", "View", "Cursor", "Parent" };
const char *HewnDock::ORIENT_ICONS[7] = { "global", "local", "normal", "gimbal", "view", "cursor", "parent" };
const char *HewnDock::PIVOT_NAMES[5] = { "Bounding Box Center", "3D Cursor", "Individual Origins", "Median Point", "Active Element" };
const char *HewnDock::PIVOT_ICONS[5] = { "bbox", "cursor", "individual", "median", "active" };

void HewnDock::update_snap_highlight() {
	if (!snap_target_button || !snap_enabled) {
		return;
	}
	const int on = snap_enabled->is_pressed() ? 1 : 0;
	if (on == snap_lit) {
		return;
	}
	snap_lit = on;
	if (on) {
		Ref<Theme> et = EditorInterface::get_singleton()->get_editor_theme();
		const Color accent = et->has_color("accent_color", "Editor") ? et->get_color("accent_color", "Editor") : Color(0.44f, 0.73f, 0.98f);
		for (const char *c : { "icon_normal_color", "icon_hover_color", "icon_pressed_color", "font_color", "font_hover_color", "font_pressed_color" }) {
			snap_target_button->add_theme_color_override(c, accent);
		}
	} else {
		for (const char *c : { "icon_normal_color", "icon_hover_color", "icon_pressed_color", "font_color", "font_hover_color", "font_pressed_color" }) {
			snap_target_button->remove_theme_color_override(c);
		}
	}
}

void HewnDock::_on_snap_step_field(double p_value) {
	if (snap_move) {
		snap_move->set_value(p_value);
	}
}

void HewnDock::_on_snap_move_changed(double p_value) {
	if (snap_step_field) {
		snap_step_field->set_value(p_value);
	}
}

void HewnDock::_build_toolbar() {
	// Inside a horizontal scroll container without a visible bar: on narrow screens the row scrolls
	// with the mouse wheel instead of widening the whole editor.
	toolbar_root = memnew(ScrollContainer);
	toolbar_root->set_name("HewnToolbar");
	toolbar_root->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	toolbar_root->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_SHOW_NEVER);
	toolbar_root->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	toolbar_root->add_theme_stylebox_override("panel", memnew(StyleBoxEmpty)); // floats over the viewport: no background
	toolbar_root->connect("gui_input", callable_mp(this, &HewnDock::_on_toolbar_input));
	toolbar = memnew(HBoxContainer);
	toolbar->add_theme_constant_override("separation", 6);
	toolbar_root->add_child(toolbar);

	// Blocks: buttons that belong together sit on one darker rounded background.
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
		h->add_theme_constant_override("separation", 0);
		p->add_child(h);
		toolbar->add_child(p);
		return h;
	};
	auto divider = [&](HBoxContainer *p_block) {
		VSeparator *s = memnew(VSeparator);
		s->add_theme_constant_override("separation", 2);
		p_block->add_child(s);
	};
	auto arrow = [&](HBoxContainer *p_block, PopupPanel *p_popup, const String &p_tip) {
		Button *b = memnew(Button);
		b->set_text(String::chr(0x25BE)); // small down triangle
		b->set_flat(true);
		b->set_tooltip_text(p_tip);
		p_block->add_child(b);
		b->connect("pressed", callable_mp(this, &HewnDock::_popup_under).bind(p_popup, b));
		toolbar->add_child(p_popup);
		return b;
	};
	// fields of the popups: drag left / right with the mouse (Shift: finer, Ctrl: steps), click to type
	auto row = [&](Container *p_grid, const String &p_label, double p_min, double p_max, double p_step, double p_value, const String &p_suffix) {
		Label *l = memnew(Label);
		l->set_text(p_suffix.is_empty() ? p_label : p_label + String(" (") + p_suffix + String(")"));
		p_grid->add_child(l);
		HewnDragField *s = memnew(HewnDragField);
		s->setup(p_min, MAX(p_max, 1e9), p_step, false, p_value);
		s->set_custom_minimum_size(Vector2(110, 0));
		p_grid->add_child(s);
		return s;
	};
	auto check = [&](Container *p_parent, const String &p_text, bool p_on) {
		CheckBox *c = memnew(CheckBox);
		c->set_text(p_text);
		c->set_pressed(p_on);
		c->connect("toggled", callable_mp(this, &HewnDock::_on_toolbar_toggled));
		p_parent->add_child(c);
		return c;
	};

	// fold button: hides the rest of the row (like the tool column's arrow). Its own block, outside the
	// scrolled row, so it stays visible however far the row is scrolled.
	toolbar_fold = memnew(PanelContainer);
	toolbar_fold->set_name("HewnHeaderFold");
	{
		Ref<StyleBoxFlat> sb;
		sb.instantiate();
		sb->set_bg_color(base);
		sb->set_corner_radius_all(4);
		sb->set_content_margin(SIDE_LEFT, 2);
		sb->set_content_margin(SIDE_RIGHT, 2);
		toolbar_fold->add_theme_stylebox_override("panel", sb);
	}
	toolbar_collapse = memnew(Button);
	toolbar_collapse->set_flat(true);
	toolbar_collapse->set_text(String::chr(0x25C2));
	toolbar_collapse->set_tooltip_text("Hide / Show");
	toolbar_collapse->connect("pressed", callable_mp(this, &HewnDock::_on_collapse_toolbar));
	toolbar_fold->add_child(toolbar_collapse);

	// element modes (Shift+click: several at once)
	HBoxContainer *mb = block();
	static const char *mode_icons[4] = { "Object", "vertex", "edge", "face" };
	static const char *mode_tips[4] = { "Objects (Tab)", "Vertices (1)", "Edges (2)", "Faces (3)" };
	for (int i = 0; i < 4; i++) {
		Button *b = icon_button(mb, icon(mode_icons[i]), mode_tips[i], true);
		b->connect("pressed", callable_mp(this, &HewnDock::_on_mode_button).bind(i));
		mode_buttons[i] = b;
	}

	// the Add menu (the same as Shift+A), opened under the button
	add_menu = memnew(Button);
	add_menu->set_flat(true);
	add_menu->set_button_icon(icon("Add"));
	add_menu->set_text(String::chr(0x25BE));
	add_menu->set_tooltip_text("Add (Shift+A)");
	add_menu->connect("pressed", callable_mp(this, &HewnDock::_on_add_button));
	block()->add_child(add_menu);

	// Mesh menu (one menu: Select / Vertex / Edge / Face functions) and Object menu header
	auto text_menu = [&](const String &p_text, const String &p_tip) {
		MenuButton *m = memnew(MenuButton);
		m->set_flat(true);
		m->set_text(hw_editor::T(p_text) + String(" ") + String::chr(0x25BE));
		m->set_tooltip_text(p_tip);
		m->get_popup()->connect("id_pressed", callable_mp(this, &HewnDock::_on_bar_op));
		block()->add_child(m);
		return m;
	};
	auto fill = [&](PopupMenu *p_menu, const std::vector<std::pair<const char *, const char *>> &p_items) {
		p_menu->connect("id_pressed", callable_mp(this, &HewnDock::_on_bar_op));
		for (const auto &it : p_items) {
			if (String(it.second).is_empty()) {
				p_menu->add_separator(it.first);
				continue;
			}
			p_menu->add_item(it.first, (int)bar_ops.size());
			bar_ops.push_back(it.second);
		}
	};
	mesh_menu = text_menu("Mesh", "Select, Vertex, Edge and Face functions");
	{
		PopupMenu *pm = mesh_menu->get_popup();
		struct Sub {
			const char *title;
			std::vector<std::pair<const char *, const char *>> items;
		};
		const std::vector<Sub> subs = {
			{ "Select", { { "All  A", "select_all" }, { "None  Alt+A", "select_none" }, { "Invert  Ctrl+I", "invert" }, { "", "" }, { "Grow  Ctrl+=", "grow" },
								{ "Shrink  Ctrl+-", "shrink" }, { "Linked  Ctrl+L", "linked" }, { "", "" }, { "Loop", "loop" }, { "Ring", "ring" },
								{ "Border", "select_border" }, { "Checker Deselect", "checker_deselect" }, { "", "" }, { "Same Material", "by_material" }, { "Same Normal", "by_normal" }, { "Invalid UVs", "select_invalid_uvs" } } },
			{ "Vertex", { { "Extrude Vertices  E", "modal:extrude" }, { "Bevel Vertices  Ctrl+Shift+B", "modal:bevel_vertices" }, { "", "" },
								{ "Push/Pull", "modal:push_pull" }, { "Shrink/Fatten  Alt+S", "modal:shrink_fatten" }, { "Shear", "modal:shear" },
								{ "Slide Vertices  Shift+V", "modal:slide" }, { "Spin", "spin_tool" }, { "Randomize", "randomize" }, { "Mirror...", "menu:mirror" }, { "To Circle", "to_circle" },
								{ "", "" }, { "Connect  J", "connect_vertices" }, { "Rip  V", "rip" }, { "Split", "split" }, { "Merge  M...", "menu:merge" },
								{ "Weld (by distance)", "weld" }, { "Vertex Crease", "vertex_crease" }, { "", "" }, { "Separate  P...", "menu:separate" },
								{ "Dissolve Vertices", "dissolve_verts" }, { "Delete Vertices", "delete_verts" } } },
			{ "Edge", { { "Extrude Edges  E", "modal:extrude" }, { "Bevel Edges  Ctrl+B", "modal:bevel" }, { "Bridge Edge Loops", "bridge" },
							  { "New Face from Edges  F", "make_face" }, { "Spin", "spin_tool" }, { "", "" }, { "Subdivide", "subdivide_edges" }, { "Loop Cut and Slide  Ctrl+R", "loop_cut_tool" },
							  { "Offset Edge Slide  Ctrl+Shift+R", "modal:offset_edge_slide" }, { "Connect", "connect_edges" }, { "Insert Loop", "insert_loop" },
							  { "Arch Edges", "arch" }, { "Rotate Edge", "flip_edges" }, { "To Circle", "to_circle" }, { "", "" }, { "Bevel Weight", "bevel_weight" }, { "Crease  Shift+E", "edge_crease" },
							  { "Mark Seam", "mark_seam" }, { "Clear Seam", "clear_seam" }, { "Mark Sharp", "mark_sharp" }, { "Clear Sharp", "clear_sharp" },
							  { "Sharpness by Angle", "sharp_by_angle" }, { "", "" }, { "Split", "split_edges" }, { "Dissolve Edges", "dissolve_edges" },
							  { "Delete Edges", "delete_edges" }, { "Delete Edge Loops", "delete_edge_loops" } } },
			{ "Face", { { "Extrude Faces  E", "modal:extrude" }, { "Extrude Along Normals", "modal:extrude_normals" }, { "Extrude Individual", "modal:extrude_individual" }, { "Extrude Repeat", "extrude_repeat" },
							  { "Inset  I", "modal:inset" }, { "Spin", "spin_tool" }, { "Poke", "poke" }, { "Bridge Faces", "bridge_faces" }, { "To Circle", "to_circle" }, { "", "" },
							  { "Subdivide", "subdivide_faces" }, { "Triangulate", "triangulate" }, { "Tris to Quads", "tris_to_quads" }, { "Merge", "merge_faces" },
							  { "Fill Hole", "fill_holes" }, { "", "" }, { "Shade Smooth", "smooth_faces" }, { "Shade Flat", "flat_faces" }, { "Flip Normals", "flip" },
							  { "Recalculate Outside  Shift+N", "recalc_outside" }, { "Recalculate Inside", "recalc_inside" }, { "", "" }, { "Detach", "detach" },
							  { "Duplicate", "duplicate" }, { "Dissolve Faces", "dissolve_faces" }, { "Limited Dissolve", "limited_dissolve" }, { "Delete Faces", "delete_faces" } } },
		};
		for (const Sub &sub : subs) {
			PopupMenu *sm = memnew(PopupMenu);
			sm->set_name(sub.title);
			fill(sm, sub.items);
			pm->add_child(sm);
			pm->add_submenu_node_item(sub.title, sm);
		}
	}
	object_menu = text_menu("Object", "Object functions");
	{
		PopupMenu *pm = object_menu->get_popup();
		pm->disconnect("id_pressed", callable_mp(this, &HewnDock::_on_bar_op));
		fill(pm, { { "Edit Mode  Tab", "edit_mode" }, { "Transform", "" }, { "Freeze Transform", "freeze" }, { "Center Pivot", "center_pivot" },
						  { "Origin to 3D Cursor", "obj:origin_cursor" }, { "Pivot to Selection", "pivot_to_selection" }, { "Mesh", "" }, { "Subdivide (Catmull-Clark)", "catmull_clark" }, { "Add Subdivision Modifier", "obj:subsurf_modifier" }, { "Auto Smooth", "auto_smooth" },
						  { "Quadify", "quadify" }, { "Conform Normals", "conform_all" }, { "Triangulate All", "triangulate_all" }, { "Flip All Normals", "flip_all" },
						  { "Scene", "" }, { "Apply Modifiers", "apply_modifiers" }, { "Merge Selected Objects", "merge_objects" }, { "Boolean Union", "boolean_union" },
						  { "Boolean Subtract", "boolean_subtract" }, { "Boolean Intersect", "boolean_intersect" }, { "", "" },
						  { "MeshInstance3D -> Hewn", "convert_mesh" }, { "Bake to MeshInstance3D", "bake" }, { "Bake Light to Vertex Colors...", "bake_light" }, { "Export OBJ...", "export_obj" },
						  { "Export glTF...", "export_gltf" } });
	}

	// gizmo: toggle | popup (which gizmos, their orientation)
	HBoxContainer *gb = block();
	show_gizmo = icon_button(gb, icon("ToolTransform"), "Show the gizmo in edit mode", true);
	show_gizmo->set_pressed(true);
	show_gizmo->connect("toggled", callable_mp(this, &HewnDock::_on_toolbar_toggled));
	show_gizmo->connect("toggled", callable_mp(this, &HewnDock::_on_view_state_changed).unbind(1).bind(0.0));
	divider(gb);
	PopupPanel *gizmo_popup = memnew(PopupPanel);
	VBoxContainer *gv = memnew(VBoxContainer);
	gizmo_popup->add_child(gv);
	Label *gl = memnew(Label);
	gl->set_text("Gizmos");
	gv->add_child(gl);
	gizmo_move = check(gv, "Move", true);
	gizmo_rotate = check(gv, "Rotate", false);
	gizmo_scale = check(gv, "Scale", false);
	HBoxContainer *goh = memnew(HBoxContainer);
	gv->add_child(goh);
	Label *gol = memnew(Label);
	gol->set_text("Orientation");
	goh->add_child(gol);
	gizmo_orient = memnew(OptionButton);
	gizmo_orient->connect("item_selected", callable_mp(this, &HewnDock::_on_toolbar_selected));
	goh->add_child(gizmo_orient);
	arrow(gb, gizmo_popup, "Gizmo options");

	// transform orientation and pivot point: one menu each (icon + arrow in one block)
	auto menu = [&]() {
		MenuButton *m = memnew(MenuButton);
		m->set_flat(true);
		m->set_text(String::chr(0x25BE));
		block()->add_child(m);
		return m;
	};
	orient_menu = menu();
	orient_menu->get_popup()->connect("id_pressed", callable_mp(this, &HewnDock::_on_orientation));
	pivot_menu = menu();
	for (int i = 0; i < 5; i++) {
		pivot_menu->get_popup()->add_icon_radio_check_item(icon(PIVOT_ICONS[i]), PIVOT_NAMES[i], i);
	}
	pivot_menu->get_popup()->connect("id_pressed", callable_mp(this, &HewnDock::_on_pivot));
	_on_orientation(orientation);
	_on_pivot(pivot_mode);

	// snapping: Godot's own magnet button switches it; here only the targets and steps
	HBoxContainer *sb = block();
	snap_fallback = memnew(Button);
	snap_fallback->set_toggle_mode(true);
	snap_enabled = snap_fallback; // replaced by Godot's magnet button once the toolbar is placed
	snap_target_button = memnew(Button);
	snap_target_button->set_flat(true);
	snap_target_button->set_text(String::chr(0x25BE));
	snap_target_button->set_tooltip_text("Snapping (Y / Shift+Tab)");
	sb->add_child(snap_target_button);
	snap_step_field = memnew(HewnDragField);
	snap_step_field->setup(0, 1e9, 0.001, false, 0.25);
	snap_step_field->set_flat(true);
	snap_step_field->set_custom_minimum_size(Vector2(52, 0));
	snap_step_field->set_expand_to_text_length_enabled(true);
	snap_step_field->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	snap_step_field->set_tooltip_text("Snap step ([ / ])");
	snap_step_field->connect("value_changed", callable_mp(this, &HewnDock::_on_snap_step_field));
	sb->add_child(snap_step_field);
	snap_popup = memnew(PopupPanel);
	VBoxContainer *sv = memnew(VBoxContainer);
	snap_popup->add_child(sv);
	Label *st = memnew(Label);
	st->set_text("Snap to (Shift+click: several)");
	sv->add_child(st);
	for (int i = 0; i < SNAP_TARGET_COUNT; i++) {
		Button *b = memnew(Button);
		b->set_text(SNAP_TARGET_NAMES[i]);
		b->set_button_icon(icon(SNAP_TARGET_ICONS[i]));
		b->set_tooltip_text(SNAP_TARGET_TIPS[i]);
		b->set_toggle_mode(true);
		b->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		b->connect("pressed", callable_mp(this, &HewnDock::_on_snap_target).bind(i));
		sv->add_child(b);
		snap_target_buttons[i] = b;
	}
	GridContainer *sg = memnew(GridContainer);
	sg->set_columns(2);
	sv->add_child(sg);
	snap_move = row(sg, "Move step", 0, 100, 0.001, 0.25, "");
	snap_move->connect("value_changed", callable_mp(this, &HewnDock::_on_snap_move_changed));
	snap_rotate = row(sg, "Rotate step", 0, 180, 0.1, 15, "deg");
	snap_scale = row(sg, "Scale step", 0, 10, 0.001, 0.1, "");
	for (HewnDragField *f : { snap_move, snap_rotate, snap_scale }) {
		f->connect("value_changed", callable_mp(this, &HewnDock::_on_view_state_changed)); // (kept for the next start)
	}
	toolbar->add_child(snap_popup);
	snap_target_button->connect("pressed", callable_mp(this, &HewnDock::_popup_under).bind(snap_popup, snap_target_button));
	_update_snap_targets();

	// proportional editing (O): toggle | options
	HBoxContainer *pb = block();
	prop_enabled = icon_button(pb, icon("proportional"), "Proportional editing (O)", true);
	prop_enabled->connect("toggled", callable_mp(this, &HewnDock::_on_toolbar_toggled));
	divider(pb);
	PopupPanel *prop_popup = memnew(PopupPanel);
	GridContainer *pg = memnew(GridContainer);
	pg->set_columns(2);
	prop_popup->add_child(pg);
	Label *fl = memnew(Label);
	fl->set_text("Falloff");
	pg->add_child(fl);
	prop_falloff = memnew(OptionButton);
	for (const char *f : { "Smooth", "Sphere", "Root", "Inverse Square", "Sharp", "Linear", "Constant", "Random", "Custom Curve" }) {
		prop_falloff->add_item(f);
	}
	prop_falloff->connect("item_selected", callable_mp(this, &HewnDock::_on_prop_falloff));
	pg->add_child(prop_falloff);
	{
		// the Custom Curve: x the distance from the middle (0) to the edge (1), y the weight
		pg->add_child(memnew(Control));
		prop_curve_picker = memnew(EditorResourcePicker);
		prop_curve_picker->set_base_type("Curve");
		prop_curve_picker->set_custom_minimum_size(Vector2(160, 0) * EditorInterface::get_singleton()->get_editor_scale());
		prop_curve_picker->set_tooltip_text("Custom falloff: x the distance from the middle (0) to the edge (1), y the strength");
		prop_curve_picker->connect("resource_changed", callable_mp(this, &HewnDock::_on_prop_falloff).unbind(1).bind(-1));
		prop_curve_picker->hide();
		pg->add_child(prop_curve_picker);
	}
	prop_radius = row(pg, "Radius of influence", 0.001, 1e6, 0.01, 1.0, "");
	Label *cl = memnew(Label);
	cl->set_text("Connected only (Alt+O)");
	pg->add_child(cl);
	prop_connected = memnew(CheckBox);
	pg->add_child(prop_connected);
	Label *pjl = memnew(Label);
	pjl->set_text("Projected from View");
	pjl->set_tooltip_text("Only the distance across the view");
	pjl->set_mouse_filter(Control::MOUSE_FILTER_PASS);
	pg->add_child(pjl);
	prop_projected = memnew(CheckBox);
	pg->add_child(prop_projected);
	arrow(pb, prop_popup, "Proportional editing options");

	// auto merge: toggle | distance
	HBoxContainer *ab = block();
	automerge = icon_button(ab, icon("automerge"), "Auto merge vertices", true);
	divider(ab);
	PopupPanel *automerge_popup = memnew(PopupPanel);
	GridContainer *ag = memnew(GridContainer);
	ag->set_columns(2);
	automerge_popup->add_child(ag);
	automerge_dist = row(ag, "Merge distance", 0, 1e6, 0.0001, 0.001, "");
	arrow(ab, automerge_popup, "Auto merge options");

	// x-ray
	HBoxContainer *xb = block();
	xray = icon_button(xb, icon("GuiVisibilityXray"), "X-Ray (Alt+Z)", true);
	xray->connect("toggled", callable_mp(this, &HewnDock::_on_xray));
	isolate_btn = icon_button(xb, icon("isolate"), "Isolate (/)", true);
	isolate_btn->connect("toggled", callable_mp(this, &HewnDock::_on_isolate));
	visible_select = icon_button(xb, icon("GuiVisibilityVisible"), "Visible select: clicks go through faces seen from behind", true);
	visible_select->set_pressed(true);
	// UV lock (the Material tab's check box, here too)
	uv_lock_tool = icon_button(xb, icon("Lock"), "UV Lock", true);
	uv_lock_tool->set_pressed(uv_lock_button && uv_lock_button->is_pressed());
	uv_lock_tool->connect("toggled", callable_mp(this, &HewnDock::_on_uv_lock));

	// overlays: toggle | what to show
	HBoxContainer *ob = block();
	overlays = icon_button(ob, icon("overlays"), "Overlays (Shift+Alt+Z)", true);
	overlays->set_pressed(true);
	overlays->connect("toggled", callable_mp(this, &HewnDock::_on_toolbar_toggled));
	divider(ob);
	PopupPanel *overlay_popup = memnew(PopupPanel);
	VBoxContainer *ov = memnew(VBoxContainer);
	overlay_popup->add_child(ov);
	// wireframe: the edges of every Hewn mesh, with an opacity slider
	ov_face_grid = check(ov, "Grid on faces", false);
	ov_face_grid->set_tooltip_text("World grid (Alt+G)");
	ov_face_grid->connect("toggled", callable_mp(plugin, &HewnEditorPlugin::set_face_grid));
	ov_overlap = check(ov, "Show Overlapping Faces", false);
	ov_overlap->set_name("OverlapCheck");
	ov_overlap->set_tooltip_text("Faces of the edited mesh lying on each other");
	HBoxContainer *wr = memnew(HBoxContainer);
	ov->add_child(wr);
	ov_wireframe = check(wr, "Wireframe", false);
	ov_wireframe->set_tooltip_text("Show the edges of all meshes");
	ov_wireframe->connect("toggled", callable_mp(this, &HewnDock::_on_wireframe_changed).unbind(1).bind(0.0));
	ov_wire_opacity = memnew(HSlider);
	ov_wire_opacity->set_min(0.0);
	ov_wire_opacity->set_max(1.0);
	ov_wire_opacity->set_step(0.01);
	ov_wire_opacity->set_value(1.0);
	ov_wire_opacity->set_custom_minimum_size(Vector2(110, 0));
	ov_wire_opacity->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	ov_wire_opacity->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	ov_wire_opacity->set_tooltip_text("Opacity of the wireframe");
	ov_wire_opacity->connect("value_changed", callable_mp(this, &HewnDock::_on_wireframe_changed));
	wr->add_child(ov_wire_opacity);
	ov_cursor = check(ov, "3D cursor", true);
	ov_collision = check(ov, "Collision", false);
	ov_collision->set_tooltip_text("The collision shapes of the objects (their Collision setting)");
	ov_collision->connect("toggled", callable_mp(this, &HewnDock::_on_collision_toggled));
	ov_dims = check(ov, "Dimensions", false);
	ov_dims->set_tooltip_text("The size of the selection along X / Y / Z, written next to its outer edges");
	ov_face_fill = check(ov, "Face selection highlight", true);
	ov_face_centers = check(ov, "Centers of selected faces", false);
	ov_vnormals = check(ov, "Vertex normals", false);
	ov_snormals = check(ov, "Split normals (per face corner)", false);
	ov_fnormals = check(ov, "Face normals", false);
	GridContainer *ong = memnew(GridContainer);
	ong->set_columns(2);
	ov->add_child(ong);
	ov_normal_size = row(ong, "Normal size", 0.001, 1e6, 0.01, 0.2, "");
	ov_normal_size->connect("value_changed", callable_mp(this, &HewnDock::_on_toolbar_value));
	ov_square_size = row(ong, "Color square size", 2, 64, 1, 8, "px");
	ov_square_size->set_tooltip_text("Inside of the color squares");
	ov_square_size->connect("value_changed", callable_mp(this, &HewnDock::_on_toolbar_value));
	ov_indices = check(ov, "Indices of selected faces", false);
	Label *ml = memnew(Label);
	ml->set_text("Measurements (selection)");
	ov->add_child(ml);
	ov_edge_len = check(ov, "Edge length", false);
	ov_face_area = check(ov, "Face area", false);
	ov_edge_angle = check(ov, "Edge angle", false);
	ov_face_angles = check(ov, "Face angles", false);
	arrow(ob, overlay_popup, "Overlay options");
	// the shading of the Hewn meshes: also Vertex Paint's view buttons
	divider(ob);
	shading_menu = memnew(Button);
	shading_menu->set_name("HewnShading");
	shading_menu->set_flat(true);
	shading_menu->set_button_icon(icon("view_material"));
	shading_menu->set_tooltip_text("Viewport shading");
	ob->add_child(shading_menu);
	{
		const float es = EditorInterface::get_singleton()->get_editor_scale();
		shading_popup = memnew(PopupPanel);
		shading_popup->set_name("HewnShadingPopup");
		VBoxContainer *sv = memnew(VBoxContainer);
		shading_popup->add_child(sv);
		static const char *names[6] = { "Materials", "Attribute Color", "Smooth / Flat", "Normals", "Single Color", "Random" };
		static const char *icons[6] = { "view_material", "view_channel", "view_normals", "view_normal_colors", "view_single", "view_random" };
		Ref<ButtonGroup> group;
		group.instantiate();
		for (int i = 0; i < 6; i++) {
			if (i == 4) {
				sv->add_child(memnew(HSeparator));
			}
			Button *b = memnew(Button);
			b->set_name(String("Shading") + itos(i));
			b->set_text(names[i]);
			b->set_button_icon(icon(icons[i]));
			b->set_flat(true);
			b->set_toggle_mode(true);
			b->set_button_group(group);
			b->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
			b->set_pressed(i == 0);
			b->connect("pressed", callable_mp(this, &HewnDock::_on_shading).bind(i));
			sv->add_child(b);
			shading_btns[i] = b;
		}
		// Single Color / Random: the light fixed in the world, turned about Y; Single Color: the cavity
		const HwMeshStyle &st = hw_mesh_style();
		solid_opts = memnew(VBoxContainer);
		solid_opts->set_visible(false);
		sv->add_child(solid_opts);
		solid_opts->add_child(memnew(HSeparator));
		HBoxContainer *wr = memnew(HBoxContainer);
		solid_opts->add_child(wr);
		solid_angle = memnew(HewnDragField);
		solid_angle->setup(-180, 180, 1, false, st.world_light_angle);
		solid_angle->set_custom_minimum_size(Vector2(64, 0) * es);
		solid_angle->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		solid_angle->set_tooltip_text("The turn of the world space lighting about the Y axis (degrees)");
		solid_angle->connect("value_changed", callable_mp(this, &HewnDock::_on_solid_angle));
		wr->add_child(solid_angle);
		solid_world = memnew(Button);
		solid_world->set_name("WorldLight");
		solid_world->set_flat(true);
		solid_world->set_toggle_mode(true);
		solid_world->set_button_icon(icon("world_light"));
		solid_world->set_pressed(st.world_light);
		solid_world->set_tooltip_text("World Space Lighting");
		solid_world->connect("toggled", callable_mp(this, &HewnDock::_on_solid_world));
		wr->add_child(solid_world);
		cavity_check = memnew(CheckBox);
		cavity_check->set_name("Cavity");
		cavity_check->set_text("Cavity");
		cavity_check->set_pressed(st.cavity);
		cavity_check->set_tooltip_text("Ridges lighter, valleys darker (Forward+ or Mobile renderer only)");
		cavity_check->connect("toggled", callable_mp(this, &HewnDock::_on_cavity));
		solid_opts->add_child(cavity_check);
		GridContainer *cg = memnew(GridContainer);
		cg->set_columns(2);
		cavity_opts = cg;
		cavity_opts->set_visible(st.cavity);
		solid_opts->add_child(cg);
		auto field = [&](const char *p_label, float p_value) {
			Label *l = memnew(Label);
			l->set_text(p_label);
			cg->add_child(l);
			HewnDragField *f = memnew(HewnDragField);
			f->setup(0, 2, 0.01, false, p_value);
			f->set_custom_minimum_size(Vector2(64, 0) * es);
			f->connect("value_changed", callable_mp(this, &HewnDock::_on_cavity_value));
			cg->add_child(f);
			return f;
		};
		cavity_ridge = field("Ridge", st.cavity_ridge);
		cavity_valley = field("Valley", st.cavity_valley);
		Label *tl = memnew(Label);
		tl->set_text("Type");
		cg->add_child(tl);
		cavity_type = memnew(OptionButton);
		cavity_type->add_item("World Space");
		cavity_type->add_item("Screen Space");
		cavity_type->add_item("World and Screen");
		cavity_type->select(st.cavity_type);
		cavity_type->connect("item_selected", callable_mp(this, &HewnDock::_on_cavity_type));
		cg->add_child(cavity_type);
		toolbar->add_child(shading_popup);
		shading_menu->connect("pressed", callable_mp(this, &HewnDock::_popup_under).bind(shading_popup, shading_menu));
	}
	// the blocks of edit / object mode, then Vertex Paint's (the mode, X-ray and the overlays stay in both)
	for (int i = 0; i < toolbar->get_child_count(); i++) {
		PanelContainer *pc = Object::cast_to<PanelContainer>(toolbar->get_child(i));
		if (pc && pc != mb->get_parent() && pc != xb->get_parent() && pc != ob->get_parent()) {
			edit_blocks.push_back(pc);
		}
	}
	build_paint_bar(mb);
	build_sculpt_bar();
	// the Box Cutter tool's settings: shown while it is the active tool
	{
		// (a row of its own under the header, as Vertex Paint's: not lost among the other buttons)
		HBoxContainer *cb = block();
		Control *cblock = Object::cast_to<Control>(cb->get_parent());
		toolbar->remove_child(cblock);
		ScrollContainer *cr = memnew(ScrollContainer);
		cr->set_name("HewnCutterRow");
		cr->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
		cr->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_SHOW_NEVER);
		cr->add_theme_stylebox_override("panel", memnew(StyleBoxEmpty));
		HBoxContainer *crow = memnew(HBoxContainer);
		crow->add_theme_constant_override("separation", 6);
		cr->add_child(crow);
		crow->add_child(cblock);
		cutter_bar = cr;
		hdr_cut_shape = memnew(OptionButton);
		for (const char *it : { "Box", "Circle", "Polygon" }) {
			hdr_cut_shape->add_item(it);
		}
		hdr_cut_shape->set_tooltip_text("The Box Cutter's shape");
		hdr_cut_shape->set_flat(true);
		hdr_cut_shape->connect("item_selected", callable_mp(this, &HewnDock::_on_cut_header_int));
		cb->add_child(hdr_cut_shape);
		hdr_cut_op = memnew(OptionButton);
		for (const char *it : { "Cut (X)", "Slice (K)", "Join (J)", "Intersect (I)", "Make (M)" }) {
			hdr_cut_op->add_item(it);
		}
		hdr_cut_op->set_tooltip_text("What it does");
		hdr_cut_op->set_flat(true);
		hdr_cut_op->connect("item_selected", callable_mp(this, &HewnDock::_on_cut_header_int));
		cb->add_child(hdr_cut_op);
		hdr_cut_through = memnew(CheckBox);
		hdr_cut_through->set_text("Cut through");
		hdr_cut_through->set_pressed(true);
		hdr_cut_through->set_tooltip_text("Off: set the depth with the mouse after drawing");
		hdr_cut_through->connect("toggled", callable_mp(this, &HewnDock::_on_cut_header_bool));
		cb->add_child(hdr_cut_through);
		hdr_cut_sides_label = memnew(Label);
		hdr_cut_sides_label->set_text("Circle sides");
		cb->add_child(hdr_cut_sides_label);
		hdr_cut_sides = memnew(SpinBox);
		HewnDragField::hook_spinbox(hdr_cut_sides);
		hdr_cut_sides->set_min(3);
		hdr_cut_sides->set_max(256);
		hdr_cut_sides->set_step(1);
		hdr_cut_sides->set_value(24);
		hdr_cut_sides->connect("value_changed", callable_mp(this, &HewnDock::_on_cut_header_value));
		cb->add_child(hdr_cut_sides);
		cutter_bar->hide();
	}
}

// The Box Cutter's settings: the header's and the Edit tab's show the same (each sets the other).
void HewnDock::_sync_cutter(bool p_from_header) {
	if (!cutter_shape || !hdr_cut_shape) {
		return;
	}
	if (p_from_header) {
		cutter_shape->select(hdr_cut_shape->get_selected());
		cutter_op->select(hdr_cut_op->get_selected());
		cutter_through->set_pressed_no_signal(hdr_cut_through->is_pressed());
		cutter_sides->set_value_no_signal(hdr_cut_sides->get_value());
	} else {
		hdr_cut_shape->select(cutter_shape->get_selected());
		hdr_cut_op->select(cutter_op->get_selected());
		hdr_cut_through->set_pressed_no_signal(cutter_through->is_pressed());
		hdr_cut_sides->set_value_no_signal(cutter_sides->get_value());
	}
	// the circle's sides only for a circle
	const bool circle = cutter_shape->get_selected() == 1;
	hdr_cut_sides->set_visible(circle);
	hdr_cut_sides_label->set_visible(circle);
	if (Control *row = Object::cast_to<Control>(cutter_sides->get_parent())) {
		// the row keeps its height, only what is in it hides: the tab's height stays (a scroll bar coming and
		// going with it made the layout swing back and forth)
		if (row->get_custom_minimum_size().y <= 0.0f) {
			row->set_custom_minimum_size(Vector2(0, row->get_combined_minimum_size().y));
		}
		for (int i = 0; i < row->get_child_count(); i++) {
			if (CanvasItem *c = Object::cast_to<CanvasItem>(row->get_child(i))) {
				c->set_visible(circle);
			}
		}
	}
	if (plugin) {
		plugin->update_overlays();
	}
}

void HewnDock::_on_cut_header_int(int p_unused) {
	_sync_cutter(true);
}

void HewnDock::_on_cut_header_bool(bool p_unused) {
	_sync_cutter(true);
}

void HewnDock::_on_cut_header_value(double p_unused) {
	_sync_cutter(true);
}

void HewnDock::_on_cut_tab_int(int p_unused) {
	_sync_cutter(false);
}

void HewnDock::_on_cut_tab_bool(bool p_unused) {
	_sync_cutter(false);
}

void HewnDock::_on_cut_tab_value(double p_unused) {
	_sync_cutter(false);
}

void HewnDock::_popup_under(PopupPanel *p_popup, Control *p_anchor) {
	const Vector2 at = p_anchor->get_screen_position() + Vector2(0, p_anchor->get_size().y);
	p_popup->reset_size();
	p_popup->popup(Rect2i(Vector2i(at), Vector2i()));
}

void HewnDock::_on_toolbar_selected(int p_unused) {
	plugin->update_overlays();
}

void HewnDock::_on_toolbar_value(double p_unused) {
	plugin->update_overlays();
}

// Click: one element mode; Shift+click: add / remove a mode.
void HewnDock::_on_mode_button(int p_mode) {
	if (p_mode != 0 && Input::get_singleton()->is_key_pressed(KEY_SHIFT)) {
		plugin->toggle_mode(p_mode);
	} else {
		plugin->set_mode(p_mode);
	}
	refresh();
}

String HewnDock::orient_name(int p_orient) const {
	if (p_orient >= 0 && p_orient < 7) {
		return ORIENT_NAMES[p_orient];
	}
	const int c = p_orient - ORIENT_CUSTOM;
	return c >= 0 && c < (int)custom_names.size() ? custom_names[c] : String("Global");
}

// Orientation menu and the gizmo's orientation list: the built-in ones, then the custom ones.
void HewnDock::_rebuild_orient_menus() {
	PopupMenu *pm = orient_menu->get_popup();
	pm->clear();
	for (int i = 0; i < 7; i++) {
		pm->add_icon_radio_check_item(icon(ORIENT_ICONS[i]), ORIENT_NAMES[i], i);
	}
	if (!custom_names.empty()) {
		pm->add_separator("Custom");
		for (size_t i = 0; i < custom_names.size(); i++) {
			pm->add_icon_radio_check_item(icon("normal"), custom_names[i], ORIENT_CUSTOM + (int)i);
		}
	}
	pm->add_separator();
	pm->add_icon_item(icon("Add"), "Create from Selection", ORIENT_ID_ADD); // "+"
	if (orientation >= ORIENT_CUSTOM) {
		pm->add_icon_item(icon("Remove"), "Delete \"" + orient_name(orientation) + "\"", ORIENT_ID_DELETE);
	}
	for (int i = 0; i < pm->get_item_count(); i++) {
		if (pm->is_item_radio_checkable(i)) {
			pm->set_item_checked(i, pm->get_item_id(i) == orientation);
		}
	}
	const int gsel = gizmo_orient->get_item_count() > 0 ? gizmo_orient->get_selected() : 0;
	gizmo_orient->clear();
	gizmo_orient->add_item("Default (transform orientation)");
	for (int i = 0; i < 7; i++) {
		gizmo_orient->add_icon_item(icon(ORIENT_ICONS[i]), ORIENT_NAMES[i]);
	}
	for (const String &nm : custom_names) {
		gizmo_orient->add_icon_item(icon("normal"), nm);
	}
	gizmo_orient->select(gsel < gizmo_orient->get_item_count() ? gsel : 0);
	hw_editor::localize(pm);
	hw_editor::localize(gizmo_orient);
}

void HewnDock::_on_bar_op(int p_id) {
	if (plugin && p_id >= 0 && p_id < (int)bar_ops.size()) {
		plugin->run_bar_op(bar_ops[p_id]);
	}
}

// toolbar: a column of tools at the left of the viewport; the arrow tucks it away.
void HewnDock::_build_tool_column() {
	Ref<Theme> et = EditorInterface::get_singleton()->get_editor_theme();
	const Color base = et->has_color("dark_color_1", "Editor") ? et->get_color("dark_color_1", "Editor") : Color(0.1f, 0.11f, 0.13f);
	// each tool on its own dark rounded block, with gaps between them (like the blocks of the header)
	auto block = [&](Container *p_parent) {
		PanelContainer *p = memnew(PanelContainer);
		Ref<StyleBoxFlat> sb;
		sb.instantiate();
		sb->set_bg_color(base);
		sb->set_corner_radius_all(4);
		sb->set_content_margin_all(1);
		p->add_theme_stylebox_override("panel", sb);
		p_parent->add_child(p);
		return p;
	};
	tool_column = memnew(PanelContainer);
	tool_column->set_name("HewnTools");
	tool_column->add_theme_stylebox_override("panel", memnew(StyleBoxEmpty));
	VBoxContainer *v = memnew(VBoxContainer);
	v->add_theme_constant_override("separation", 6);
	tool_column->add_child(v);
	tool_body = memnew(VBoxContainer);
	tool_body->add_theme_constant_override("separation", 6);
	v->add_child(tool_body);
	static const char *icons[25] = { "tool_select", "cursor", "tool_ruler", "tool_knife", "tool_loopcut", "tool_cutter", "tool_bisect", "tool_spin",
		"tool_select_circle", "tool_lasso", "tool_paint", "tool_pencil", "tool_fill", "tool_picker", "tool_normals", "sc_draw", "sc_smooth", "sc_flatten",
		"sc_grab", "sc_noise", "sc_terrace", "sc_pinch", "sc_inflate", "sc_crease", "sc_density" };
	static const char *tips[25] = { "Select Box", "3D Cursor (Shift+RMB)", "Ruler (Shift+M)", "Knife (K)", "Loop Cut (Ctrl+R)", "Box Cutter", "Bisect", "Spin",
		"Select Circle", "Select Lasso", "Brush", "Pencil", "Paint Fill", "Eyedropper", "Normals", "Draw (Ctrl: lower)", "Smooth (Shift in any brush)",
		"Flatten (Ctrl: to the height where the stroke began)", "Grab", "Noise", "Terrace", "Pinch (Ctrl: spread)", "Inflate (Ctrl: deflate)",
		"Sharp Edge (Ctrl: a ridge)", "Density (the detail of the dynamic topology)" };
	// (the order in the column: the sculpt tools, the paint tools, the selection ones, then the rest)
	static const int order[25] = { 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 10, 11, 12, 13, 14, 0, 8, 9, 1, 2, 3, 4, 5, 6, 7 };
	for (int oi = 0; oi < 25; oi++) {
		const int i = order[oi];
		PanelContainer *bl = block(tool_body);
		HBoxContainer *h = memnew(HBoxContainer);
		bl->add_child(h);
		Button *b = icon_button(h, icon(icons[i]), tips[i], true);
		// width from the icon and the style margins (the same on both sides), the icon in the middle
		b->set_custom_minimum_size(Vector2(0, 28));
		b->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		b->set_icon_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		b->connect("pressed", callable_mp(this, &HewnDock::_on_tool_button).bind(i));
		b->set_name(vformat("Tool%d", i));
		tool_buttons[i] = b;
	}
	PanelContainer *cb = block(v);
	tool_collapse = memnew(Button);
	tool_collapse->set_flat(true);
	tool_collapse->set_text(String::chr(0x25C2)); // small left triangle
	tool_collapse->set_tooltip_text("Hide / Show");
	tool_collapse->connect("pressed", callable_mp(this, &HewnDock::_on_collapse_tools));
	cb->add_child(tool_collapse);
}

void HewnDock::_on_collapse_toolbar() {
	bool show = false;
	for (int i = 0; i < toolbar->get_child_count(); i++) {
		Control *c = Object::cast_to<Control>(toolbar->get_child(i));
		if (c) {
			show = !c->is_visible();
			break;
		}
	}
	for (int i = 0; i < toolbar->get_child_count(); i++) {
		Control *c = Object::cast_to<Control>(toolbar->get_child(i)); // (the popups are windows: not touched)
		if (c) {
			c->set_visible(show);
		}
	}
	toolbar_folded = !show;
	if (!show && paint_root) {
		paint_root->hide(); // (the second row folds with the header)
	}
	if (show) {
		show_paint_bar(paint_bar_shown); // (each mode its own blocks)
	}
	toolbar_root->set_h_scroll(0);
	toolbar_collapse->set_text(String::chr(show ? 0x25C2 : 0x25B8));
}

void HewnDock::_on_prop_falloff(int p_unused) {
	const bool custom = prop_falloff && prop_falloff->get_selected() == 8;
	if (prop_curve_picker) {
		prop_curve_picker->set_visible(custom);
		if (custom && Ref<Curve>(prop_curve_picker->get_edited_resource()).is_null()) {
			Ref<Curve> c; // (a curve to start from: full in the middle, nothing at the edge)
			c.instantiate();
			c->add_point(Vector2(0, 1));
			c->add_point(Vector2(1, 0));
			prop_curve_picker->set_edited_resource(c);
		}
		const Ref<Resource> pc = prop_curve_picker->get_edited_resource();
		HewnEditorPlugin::prop_curve_id = pc.is_valid() ? (uint64_t)pc->get_instance_id() : 0;
	}
	if (plugin) {
		plugin->update_overlays();
	}
}

void HewnDock::_on_tool_button(int p_tool) {
	if (plugin) {
		plugin->select_tool(p_tool);
	}
}

void HewnDock::_on_collapse_tools() {
	const bool show = !tool_body->is_visible();
	tool_body->set_visible(show);
	tool_collapse->set_text(String::chr(show ? 0x25C2 : 0x25B8));
	tool_column->reset_size();
}

void HewnDock::_on_add_button() {
	if (plugin && add_menu) {
		plugin->show_add_menu_at(add_menu->get_screen_position() + Vector2(0, add_menu->get_size().y));
	}
}

void HewnDock::_on_add_shape(int p_type) {
	if (p_type == hewn::shapes::POLY_SHAPE) {
		plugin->start_poly_tool(false);
	} else {
		plugin->start_shape_tool(p_type);
	}
}

void HewnDock::_on_add_orientation() {
	if (plugin) {
		plugin->create_custom_orientation();
	}
}

void HewnDock::add_custom_orientation(const String &p_name, const Basis &p_basis) {
	String nm = p_name;
	for (int k = 1;; k++) {
		if (std::find(custom_names.begin(), custom_names.end(), nm) == custom_names.end()) {
			break;
		}
		nm = p_name + vformat(".%03d", k);
	}
	custom_names.push_back(nm);
	custom_orients.push_back(p_basis);
	_on_orientation(ORIENT_CUSTOM + (int)custom_names.size() - 1);
}

void HewnDock::_on_orientation(int p_id) {
	if (p_id == ORIENT_ID_ADD) {
		_on_add_orientation();
		return;
	}
	if (p_id == ORIENT_ID_DELETE) {
		const int c = orientation - ORIENT_CUSTOM;
		if (c >= 0 && c < (int)custom_names.size()) {
			custom_names.erase(custom_names.begin() + c);
			custom_orients.erase(custom_orients.begin() + c);
			// the gizmo option that pointed at it (or after it) falls back to Default
			if (gizmo_orient->get_selected() >= 1 + ORIENT_CUSTOM + c) {
				gizmo_orient->select(0);
			}
		}
		p_id = ORIENT_GLOBAL;
	}
	orientation = (p_id >= 0 && p_id < 7) || (p_id >= ORIENT_CUSTOM && p_id - ORIENT_CUSTOM < (int)custom_names.size()) ? p_id : ORIENT_GLOBAL;
	_rebuild_orient_menus();
	orient_menu->set_button_icon(icon(orientation < 7 ? ORIENT_ICONS[orientation] : "normal"));
	orient_menu->set_tooltip_text(hw_editor::T("Transform orientation: ") + hw_editor::T(orient_name(orientation)));
	if (plugin) {
		plugin->update_overlays();
	}
}

void HewnDock::_on_pivot(int p_id) {
	pivot_mode = CLAMP(p_id, 0, 4);
	for (int i = 0; i < 5; i++) {
		pivot_menu->get_popup()->set_item_checked(i, i == pivot_mode);
	}
	pivot_menu->set_button_icon(icon(PIVOT_ICONS[pivot_mode]));
	pivot_menu->set_tooltip_text(hw_editor::T("Pivot point: ") + hw_editor::T(PIVOT_NAMES[pivot_mode]));
	if (plugin) {
		plugin->update_overlays();
	}
}

// a click picks one snap target, Shift+click toggles several.
void HewnDock::_on_snap_target(int p_index) {
	const int bit = SNAP_ORDER[p_index];
	if (Input::get_singleton()->is_key_pressed(KEY_SHIFT)) {
		snap_targets ^= bit;
		if (snap_targets == 0) {
			snap_targets = bit;
		}
	} else {
		snap_targets = bit;
	}
	_update_snap_targets();
}

void HewnDock::_update_snap_targets() {
	for (int i = 0; i < SNAP_TARGET_COUNT; i++) {
		snap_target_buttons[i]->set_pressed_no_signal((snap_targets & SNAP_ORDER[i]) != 0);
	}
	for (int i = 0; i < SNAP_TARGET_COUNT; i++) {
		if (snap_targets & SNAP_ORDER[i]) {
			snap_target_button->set_button_icon(icon(SNAP_TARGET_ICONS[i]));
			break;
		}
	}
}

void HewnDock::_on_xray(bool p_on) {
	plugin->set_xray(p_on);
}

void HewnDock::_on_toolbar_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_null() || !mb->is_pressed()) {
		return;
	}
	const MouseButton b = mb->get_button_index();
	int step = 0;
	if (b == MOUSE_BUTTON_WHEEL_UP || b == MOUSE_BUTTON_WHEEL_LEFT) {
		step = -60;
	} else if (b == MOUSE_BUTTON_WHEEL_DOWN || b == MOUSE_BUTTON_WHEEL_RIGHT) {
		step = 60;
	}
	if (step != 0) {
		toolbar_root->set_h_scroll(MAX(0, toolbar_root->get_h_scroll() + step));
		toolbar_root->accept_event();
	}
}

void HewnDock::_on_toolbar_toggled(bool p_unused) {
	plugin->update_overlays();
	if (ov_wireframe && ov_wireframe->is_pressed()) {
		update_all_gizmos(); // overlays on/off hides the wireframe of all meshes
	} else if (HewnMesh3D *n = plugin->get_edited()) {
		n->update_gizmos(); // overlays on/off, face highlight
	}
}

void HewnDock::_on_wireframe_changed(double p_unused) {
	update_all_gizmos();
}

void HewnDock::update_all_gizmos() {
	Node *scene = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!scene) {
		return;
	}
	if (HewnMesh3D *n = Object::cast_to<HewnMesh3D>(scene)) {
		n->update_gizmos();
	}
	const TypedArray<Node> all = scene->find_children("*", "HewnMesh3D", true, false);
	for (int64_t i = 0; i < all.size(); i++) {
		if (HewnMesh3D *n = Object::cast_to<HewnMesh3D>(all[i])) {
			n->update_gizmos();
		}
	}
}

void HewnDock::_on_new_uv_pixels(double p_value) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid()) {
		es->set_project_metadata("hewn", "new_uv_pixels", p_value);
	}
}

void HewnDock::_on_new_uv_world(bool p_on) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid()) {
		es->set_project_metadata("hewn", "new_world_uv", p_on);
	}
}

void HewnDock::_new_uv_scale(float p_factor) {
	if (new_uv_pixels) {
		new_uv_pixels->set_value(MAX(1.0, std::round(new_uv_pixels->get_value() * p_factor)));
	}
}

void HewnDock::_build_create_tab() {
	VBoxContainer *v = tab(tabs, "Create");
	header(v, "Shapes");
	GridContainer *g = memnew(GridContainer);
	g->set_columns(2);
	v->add_child(g);
	for (int t = 1; t < hewn::shapes::TYPE_MAX; t++) {
		if (t == hewn::shapes::POLY_SHAPE || t == hewn::shapes::CURVED_STAIRS) {
			continue; // drawn with the Poly Shape tool; spiral stairs = Stairs with a curvature
		}
		Button *b = memnew(Button);
		b->set_text(hewn::shapes::type_name((hewn::shapes::Type)t));
		b->set_button_icon(shape_icon(t));
		b->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		b->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		b->set_tooltip_text("Click a corner of the base on a surface or the ground, the opposite corner, then the height; Enter = default size at the 3D cursor");
		b->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::start_shape_tool).bind(t));
		g->add_child(b);
	}
	v->add_child(memnew(HSeparator));
	// level design cuts: the surface inside is removed, the hole edges stay for W (corridor / alcove)
	keep_overlap = memnew(CheckBox);
	keep_overlap->set_text("Keep Overlap Faces");
	keep_overlap->set_tooltip_text("Shapes drawn in edit mode join the edited mesh; where their faces lie on faces turned the other way, both are cut away (boxes grow together, rooms open into each other). On: those faces stay.");
	v->add_child(keep_overlap);
	v->add_child(memnew(HSeparator));
	header(v, "Cut");
	GridContainer *cg = memnew(GridContainer);
	cg->set_columns(2);
	v->add_child(cg);
	const char *cut_names[4] = { "Box Cut", "Cylinder Cut", "Prism Cut", "Clip" };
	const char *cut_tips[4] = {
		"Removes the surface inside a box drawn like a cube (a door, a window). Then W: a corridor between two holes, or an alcove",
		"Removes the surface inside a cylinder (round windows, holes in floors)",
		"Removes the surface inside a prism: click its outline (it may be concave), then the depth",
		"Cuts the selected meshes with a plane through a line you click; Q / E: split only, remove the front, remove the back. Then W: close the cut",
	};
	const char *cut_icons[4] = { "shape_cube", "shape_cylinder", "shape_poly_shape", "tool_bisect" };
	for (int i = 0; i < 4; i++) {
		Button *b = memnew(Button);
		b->set_text(cut_names[i]);
		b->set_button_icon(icon(cut_icons[i]));
		b->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		b->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		b->set_tooltip_text(cut_tips[i]);
		if (i < 3) {
			b->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::start_cut_tool).bind(i + 1));
		} else {
			b->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::start_clip));
		}
		cg->add_child(b);
	}
	{
		// non-destructive cuts: a cutter node (also inside prefabs: a door cuts the wall it is put in)
		Button *ac = memnew(Button);
		ac->set_text("Add Cutter");
		ac->set_button_icon(icon("bool_subtract"));
		ac->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		ac->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		ac->set_tooltip_text("A cutter drawn like a box (as in Shift+A): it keeps cutting the meshes it reaches (move it, resize it, delete it). Put one in a door scene: the door cuts the wall it is placed in");
		ac->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::add_from_menu).bind(String("cutter")));
		cg->add_child(ac);
		Button *ap = memnew(Button);
		ap->set_text("Apply Cutter");
		ap->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		ap->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		ap->set_tooltip_text("The selected cutters' holes go into the meshes for good (the cutters are removed)");
		ap->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::apply_cutters));
		cg->add_child(ap);
	}
	v->add_child(memnew(HSeparator));
	header(v, "Poly Shape");
	HFlowContainer *f = flow(v);
	OP_BUTTON(f, "Draw Poly Shape", "poly_shape", "Click points on a surface or the ground, close the outline, then drag the height");
	OP_BUTTON(f, "Edit Poly Shape / Wall", "edit_poly", "Edit the outline of the selected Poly Shape, or the points of a Wall or Sweep (until its geometry is edited)");
	{
		Button *b = memnew(Button);
		b->set_text("Sweep Along Path3D");
		b->set_tooltip_text("Select a Path3D: a Sweep (a profile along a path: rails, curbs, pipes, roads, cornices) follows its curve");
		b->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::sweep_along_selected_path));
		f->add_child(b);
		Button *wb = memnew(Button);
		wb->set_text("Walls Around Floor");
		wb->set_tooltip_text("Walls all along the outline of the selected floor (or the selected faces in face mode), on the inside of it; with edges selected (edge or vertex mode): only along them. The side (inside / center / outside) is the Wall's in the shape panel");
		wb->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::walls_around_floor));
		f->add_child(wb);
	}
	v->add_child(memnew(HSeparator));
	header(v, "Convert");
	f = flow(v);
	OP_BUTTON(f, "MeshInstance3D -> Hewn", "convert_mesh", "Replace the selected MeshInstance3D nodes by editable Hewn meshes");
	OP_BUTTON(f, "Hewn -> Area3D", "convert_area",
			"Replace the selected Hewn meshes by an Area3D with a CollisionShape3D of the same form (convex): a trigger, connect its body_entered / body_exited");
	_build_place_sections(v);
}

void HewnDock::_build_edit_tab() {
	VBoxContainer *v = tab(tabs, "Edit");
	header(v, "Selection");
	HFlowContainer *f = flow(v);
	OP_BUTTON(f, "All", "select_all", "A");
	OP_BUTTON(f, "None", "select_none", "Alt+A");
	OP_BUTTON(f, "Invert", "invert", "Ctrl+I");
	OP_BUTTON(f, "Grow", "grow", "Ctrl+=");
	OP_BUTTON(f, "Shrink", "shrink", "Ctrl+-");
	OP_BUTTON(f, "Linked", "linked", "Ctrl+L (L: linked under the cursor)");
	OP_BUTTON(f, "Loop", "loop", "Edge loop / face loop (Alt+click or double-click)");
	OP_BUTTON(f, "Ring", "ring", "Edge ring / face ring (Ctrl+Alt+click)");
	OP_BUTTON(f, "Border", "select_border", "Open boundary edges");
	OP_BUTTON(f, "Checker Deselect", "checker_deselect", "Every other selected element deselected");
	OP_BUTTON(f, "Same Material", "by_material", "");
	OP_BUTTON(f, "Invalid UVs", "select_invalid_uvs", "Faces whose UVs have no area (the texture is smeared into a line)");
	{
		Button *ob = memnew(Button);
		ob->set_text("Overlapping Faces");
		ob->set_tooltip_text("Selects faces lying on top of each other (z-fighting, walls left inside after joining rooms)");
		ob->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::select_overlapping));
		f->add_child(ob);
	}
	OP_BUTTON(f, "Same Normal", "by_normal", "Faces turned like the selected ones (the angle: in the Last Operation panel)");


	v->add_child(memnew(HSeparator));
	header(v, "Tools");
	f = flow(v);
	OP_BUTTON(f, "Edit Mode", "edit_mode", "Tab");
	OP_BUTTON(f, "Knife", "knife", "K");
	OP_BUTTON(f, "Knife Project", "knife_project", "Cuts the active mesh along the outlines of the other edited meshes, as seen in the view");
	OP_BUTTON(f, "Loop Cut", "loop_cut_tool", "Ctrl+R: wheel = cuts, click, then slide");
	OP_BUTTON(f, "Box Cutter", "cutter", "Draw a shape on a face to cut / slice / join (for holes through walls: Box Cut, C)");
	OP_BUTTON(f, "Bisect", "bisect_tool", "Drag a line across the mesh: cuts it with that plane (options in the Last Operation panel)");
	OP_BUTTON(f, "Ruler", "ruler", "Shift+M: measure between snapped points");
	header(v, "Box Cutter");
	HBoxContainer *ch = memnew(HBoxContainer);
	v->add_child(ch);
	cutter_shape = memnew(OptionButton);
	cutter_shape->add_item("Box");
	cutter_shape->add_item("Circle");
	cutter_shape->add_item("Polygon");
	cutter_shape->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	ch->add_child(cutter_shape);
	cutter_op = memnew(OptionButton);
	cutter_op->add_item("Cut (X)");
	cutter_op->add_item("Slice (K)");
	cutter_op->add_item("Join (J)");
	cutter_op->add_item("Intersect (I)");
	cutter_op->add_item("Make (M)");
	cutter_op->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	cutter_op->connect("item_selected", callable_mp(this, &HewnDock::_on_toolbar_selected));
	ch->add_child(cutter_op);
	cutter_through = memnew(CheckBox);
	cutter_through->set_text("Cut through");
	cutter_through->set_pressed(true);
	cutter_through->set_tooltip_text("Off: set the depth with the mouse after drawing");
	v->add_child(cutter_through);
	cutter_sides = spin(v, "Circle sides", 3, 256, 1, 24);
	cutter_shape->connect("item_selected", callable_mp(this, &HewnDock::_on_cut_tab_int));
	cutter_op->connect("item_selected", callable_mp(this, &HewnDock::_on_cut_tab_int));
	cutter_through->connect("toggled", callable_mp(this, &HewnDock::_on_cut_tab_bool));
	cutter_sides->connect("value_changed", callable_mp(this, &HewnDock::_on_cut_tab_value));
	_sync_cutter(false);

	face_ops = memnew(VBoxContainer);
	v->add_child(face_ops);
	header(face_ops, "Faces");
	f = flow(face_ops);
	OP_BUTTON(f, "Extrude", "extrude_region", "E (interactive), or Shift+drag the move handle. Parameters: Last Operation panel");
	OP_BUTTON(f, "Extrude Individual", "extrude_individual", "");
	OP_BUTTON(f, "Extrude Repeat", "extrude_repeat", "Extrudes the selection again and again (steps, distance and scale: in the Last Operation panel)");
	OP_BUTTON(f, "Inset", "inset_region", "I (interactive)");
	OP_BUTTON(f, "Inset Individual", "inset_individual", "");
	OP_BUTTON(f, "Dissolve", "dissolve", "Ctrl+X: merge the selected faces");
	OP_BUTTON(f, "Limited Dissolve", "limited_dissolve", "Merge nearly flat neighbors (angle in the Last Operation panel)");
	OP_BUTTON(f, "Delete", "delete", "X / Del");
	OP_BUTTON(f, "Flip Normals", "flip", "Alt+N: the Normals menu");
	OP_BUTTON(f, "Recalc Outside", "recalc_outside", "Shift+N");
	OP_BUTTON(f, "Recalc Inside", "recalc_inside", "Ctrl+Shift+N");
	OP_BUTTON(f, "Subdivide", "subdivide_faces", "");
	OP_BUTTON(f, "Face Cutter", "face_cutter", "Lines across the selected faces from one of their border edges, then Q cuts (also in the right-click menu)");
	OP_BUTTON(f, "Triangulate", "triangulate", "");
	OP_BUTTON(f, "Merge", "merge_faces", "M");
	OP_BUTTON(f, "Detach", "detach", "P");
	OP_BUTTON(f, "Duplicate", "duplicate", "Shift+D (interactive)");
	OP_BUTTON(f, "Conform Normals", "conform", "");
	OP_BUTTON(f, "Smooth Group", "smooth_faces", "New smoothing group for the selection");
	OP_BUTTON(f, "Hard (Flat)", "flat_faces", "");

	edge_ops = memnew(VBoxContainer);
	v->add_child(edge_ops);
	header(edge_ops, "Edges");
	f = flow(edge_ops);
	OP_BUTTON(f, "Extrude", "extrude_region", "Border edges; or E / Shift+drag");
	OP_BUTTON(f, "Limited Dissolve", "limited_dissolve", "");
	OP_BUTTON(f, "Bevel", "bevel", "Ctrl+B (interactive, wheel = segments)");
	OP_BUTTON(f, "Bevel Profile...", "bevel_profile", "The bevel's Custom profile as a curve (the profile window); the bevel just made follows it");
	OP_BUTTON(f, "Connect", "connect_edges", "Connect midpoints across faces");
	OP_BUTTON(f, "Arch Edges", "arch", "Bends the selected edges into arcs. The two top edges of a cut passage (opposite edges of its ceiling) make a vault");
	OP_BUTTON(f, "Insert Loop", "insert_loop", "Insert an edge loop across the ring");
	OP_BUTTON(f, "Subdivide", "subdivide_edges", "");
	OP_BUTTON(f, "Bridge", "bridge", "Exactly two border edges");
	OP_BUTTON(f, "Dissolve", "dissolve", "");
	OP_BUTTON(f, "Flip Edge", "flip_edges", "Between two triangles");
	OP_BUTTON(f, "Fill Hole", "fill_holes", "");
	OP_BUTTON(f, "Make Face", "make_face", "F");
	OP_BUTTON(f, "Delete", "delete", "X / Del");

	vertex_ops = memnew(VBoxContainer);
	v->add_child(vertex_ops);
	header(vertex_ops, "Vertices");
	f = flow(vertex_ops);
	OP_BUTTON(f, "Weld", "weld", "Merge selected vertices within the weld distance");
	OP_BUTTON(f, "Collapse", "collapse", "M: merge selected vertices into their center");
	OP_BUTTON(f, "Split", "split", "");
	OP_BUTTON(f, "Connect", "connect_vertices", "J");
	OP_BUTTON(f, "Make Face", "make_face", "F");
	OP_BUTTON(f, "Rip", "rip", "V (interactive)");
	OP_BUTTON(f, "Dissolve", "dissolve", "");
	OP_BUTTON(f, "Fill Hole", "fill_holes", "");
	OP_BUTTON(f, "Delete", "delete", "");
}

void HewnDock::_build_object_tab() {
	VBoxContainer *v = tab(tabs, "Object");
	header(v, "Transform");
	HFlowContainer *f = flow(v);
	OP_BUTTON(f, "Freeze Transform", "freeze", "Apply position / rotation / scale to the vertices");
	OP_BUTTON(f, "Center Pivot", "center_pivot", "");
	OP_BUTTON(f, "Origin to 3D Cursor", "obj:origin_cursor", "The object's center onto the 3D cursor (the vertices stay where they are)");
	OP_BUTTON(f, "Origin to Bottom", "obj:origin_bottom", "The object's center to the middle of its bottom");
	OP_BUTTON(f, "Pivot to Selection", "pivot_to_selection", "");
	header(v, "Mesh");
	f = flow(v);
	OP_BUTTON(f, "Subdivide (Catmull-Clark)", "catmull_clark", "Smooths the whole mesh by subdividing it; levels, type, creases in the panel at the bottom left");
	{
		Button *sm = memnew(Button);
		sm->set_text("Subdivision Modifier");
		sm->set_tooltip_text("Adds a Subdivision Surface modifier to the selected meshes");
		sm->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::add_subdivision_modifier));
		f->add_child(sm);
	}
	OP_BUTTON(f, "Auto Smooth", "auto_smooth", "Smoothing groups by angle (Edit tab)");
	OP_BUTTON(f, "Quadify", "quadify", "Merge triangle pairs into quads");
	OP_BUTTON(f, "Conform Normals", "conform_all", "");
	OP_BUTTON(f, "Triangulate All", "triangulate_all", "");
	OP_BUTTON(f, "Flip All", "flip_all", "");
	header(v, "Remesh");
	_build_remesh_block(v);
	header(v, "Scene");
	f = flow(v);
	OP_BUTTON(f, "Apply Modifiers", "apply_modifiers", "Bake the modifier stack (Modifiers array in the inspector) into the mesh");
	OP_BUTTON(f, "Merge Selected Objects", "merge_objects", "Merge all selected Hewn meshes into the first one");
	auto act = [&](Container *p_parent, const String &p_text, const Callable &p_call, const String &p_tip) {
		Button *b = memnew(Button);
		b->set_text(p_text);
		b->set_tooltip_text(p_tip);
		b->connect("pressed", p_call);
		p_parent->add_child(b);
	};
	header(v, "Arrange");
	f = flow(v);
	for (int a = 0; a < 3; a++) {
		static const char *names[3] = { "Flip X", "Flip Y", "Flip Z" };
		act(f, names[a], callable_mp(plugin, &HewnEditorPlugin::flip_objects).bind(a), "Mirror the selected objects along this world axis around their middle");
	}
	act(f, "Snap to Grid", callable_mp(plugin, &HewnEditorPlugin::snap_selection_to_grid), "Object origins (edit mode: the selected vertices) onto the grid step. [ / ]: halve / double the step");
	f = flow(v);
	act(f, "Hide  H", callable_mp(plugin, &HewnEditorPlugin::hide_objects).bind(0), "Hide the selected objects");
	act(f, "Hide Others  Shift+H", callable_mp(plugin, &HewnEditorPlugin::hide_objects).bind(1), "");
	act(f, "Reveal  Alt+H", callable_mp(plugin, &HewnEditorPlugin::hide_objects).bind(2), "Show the objects hidden with H again");
	header(v, "Boolean (the last selected object is kept)");
	boolean_live = memnew(CheckBox);
	boolean_live->set_text("Non-destructive (modifier)");
	boolean_live->set_tooltip_text("The other meshes stay and can be moved or edited; the result updates live. Apply Modifiers bakes it.");
	boolean_live->set_pressed(true);
	v->add_child(boolean_live);
	f = flow(v);
	OP_BUTTON(f, "Union", "boolean_union", "Joins the other selected meshes into the last selected one");
	OP_BUTTON(f, "Subtract", "boolean_subtract", "Cuts the other selected meshes out of the last selected one");
	OP_BUTTON(f, "Intersect", "boolean_intersect", "Keeps what the selected meshes have in common, in the last selected one");
	for (int i = 0; i < 3; i++) {
		static const char *icons[3] = { "bool_union", "bool_subtract", "bool_intersect" };
		Object::cast_to<Button>(f->get_child(i))->set_button_icon(icon(icons[i]));
	}
	v->add_child(memnew(HSeparator));
	header(v, "Curves (Path3D)");
	f = flow(v);
	OP_BUTTON(f, "Curve: Subdivide", "curve:subdivide", "The selected Path3D curves: a point in the middle of every piece (the shape stays)");
	OP_BUTTON(f, "Curve: Switch Direction", "curve:reverse", "The selected Path3D curves run the other way");
	OP_BUTTON(f, "Curve: Toggle Cyclic", "curve:cyclic", "The selected Path3D curves closed into a loop / open again");
	OP_BUTTON(f, "Curve: Auto Handles", "curve:auto", "Smooth handles on every point");
	OP_BUTTON(f, "Curve: Vector Handles", "curve:vector", "No handles: straight pieces");
	OP_BUTTON(f, "Curve: Smooth", "curve:smooth", "The inner points move toward their neighbors' middle");
	OP_BUTTON(f, "Curve: Extrude End", "curve:extrude", "A new point after the last one, along its direction");
	v->add_child(memnew(HSeparator));
	header(v, "Export");
	f = flow(v);
	OP_BUTTON(f, "Bake to MeshInstance3D", "bake", "Creates a MeshInstance3D with a standalone ArrayMesh");
	OP_BUTTON(f, "Scene Size...", "scene_size",
			"What takes the room in the saved scene: the nodes with the most data, with their embedded resources (meshes, materials, textures saved inside the scene)");
	OP_BUTTON(f, "Export OBJ...", "export_obj", "");
	OP_BUTTON(f, "Export glTF...", "export_gltf", "glTF 2.0: .glb (one file) or .gltf + .bin, with materials and vertex colors");
	v->add_child(memnew(HSeparator));
	header(v, "Game");
	f = flow(v);
	act(f, "Lightmap UV2 for All", callable_mp(plugin, &HewnEditorPlugin::lightmap_all),
			"Every mesh of the scene gets lightmap UVs (saved with the scene): LightmapGI bakes the level as it is. Per mesh: Lightmap in the inspector");
	act(f, "Bake Level...", callable_mp(plugin, &HewnEditorPlugin::bake_level),
			"Saves a copy of the scene for the game: meshes become MeshInstance3D with their collision, tool faces left out, occluders; cutters left out. This scene stays editable");
	act(f, "Export Scene glTF...", callable_mp(plugin, &HewnEditorPlugin::export_scene_gltf), "The whole scene, baked like above, as one glTF 2.0 file");
	act(f, "Check Scene...", callable_mp(plugin, &HewnEditorPlugin::open_issues),
			"Problems of the scene in a list: faces without area, vertices in one place not joined, faces that are not flat or face the other way, edges of more than two faces, triggers whose target is gone... Select them, fix the safe ones");
	bake_occluders = memnew(CheckBox);
	bake_occluders->set_text("Occluders");
	bake_occluders->set_tooltip_text("Bake Level adds an OccluderInstance3D of the opaque faces of each mesh (occlusion culling)");
	bake_occluders->set_pressed(true);
	v->add_child(bake_occluders);
}

void HewnDock::_build_material_tab() {
	VBoxContainer *v = tab(tabs, "Material");
	// One idea / every face has a texture. Pick one in the Textures tab (bottom
	// panel) or here; slots are bookkeeping. Materials follow the faces' roles when the shape changes.
	material_hint = memnew(Label); // (not shown: the textures panel says how it works)
	material_hint->hide();
	v->add_child(material_hint);
	_build_texture_section(v);
	header(v, "Apply material");
	material_picker = memnew(EditorResourcePicker);
	material_picker->set_base_type("Material");
	material_picker->set_tooltip_text("Drag a material here (or pick / create one): it goes onto the selected faces, or the whole selected objects");
	material_picker->connect("resource_changed", callable_mp(this, &HewnDock::_on_material_picked));
	v->add_child(material_picker);
	header(v, "On this mesh");
	material_slots = memnew(VBoxContainer);
	v->add_child(material_slots);
	header(v, "Tool materials");
	HFlowContainer *tf = flow(v);
	const char *tools[1][3] = { { "Clip", "clip", "Invisible faces: not drawn in the game, they still collide (an invisible wall, or the backs of brushes nobody sees)" } };
	for (const auto &t : tools) {
		Button *b = memnew(Button);
		b->set_text(t[0]);
		b->set_tooltip_text(t[2]);
		b->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::apply_tool_material).bind(String(t[1])));
		tf->add_child(b);
	}
	v->add_child(memnew(HSeparator));
	header(v, "Vertex color");
	HFlowContainer *f = flow(v);
	OP_BUTTON(f, "Vertex Paint", "vertex_paint", "Vertex Paint mode (also the mode menu at the left of the header): the colors and the brush are in the header then");
	OP_BUTTON(f, "Blend Material...", "new_blend_material",
			"A material of several textures: a base and up to four layers over it, each shown by a component (R / G / B / A) of a painted channel (CUSTOM0 by default)");
}

void HewnDock::_build_uv_tab() {
	VBoxContainer *v = tab(tabs, "UV");
	header(v, "Auto UV (selected faces)");
	// U and V side by side, linked (the chain): one changes the other alike
	auto pair = [&](const String &p_label, double p_value, SpinBox *&r_u, SpinBox *&r_v, Button *&r_link, bool p_ratio) {
		HBoxContainer *row = memnew(HBoxContainer);
		Label *pl = memnew(Label);
		pl->set_text(p_label);
		pl->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		row->add_child(pl);
		for (SpinBox **s : { &r_u, &r_v }) {
			*s = memnew(SpinBox);
			HewnDragField::hook_spinbox(*s);
			(*s)->set_min(-1000);
			(*s)->set_max(1000);
			(*s)->set_step(0.001);
			(*s)->set_value(p_value);
			(*s)->set_allow_greater(true);
			(*s)->set_allow_lesser(true);
			(*s)->set_prefix(s == &r_u ? "U" : "V");
			(*s)->set_custom_minimum_size(Vector2(74, 0));
			row->add_child(*s);
		}
		r_link = memnew(Button);
		r_link->set_toggle_mode(true);
		r_link->set_pressed(true);
		r_link->set_flat(true);
		r_link->set_button_icon(icon("Instance"));
		r_link->set_tooltip_text("U and V together (click: each on its own)");
		r_link->connect("toggled", callable_mp(this, &HewnDock::_on_uv_link_toggled).bind(r_link));
		row->add_child(r_link);
		r_u->connect("value_changed", callable_mp(this, &HewnDock::_on_uv_pair).bind(true, p_ratio));
		r_v->connect("value_changed", callable_mp(this, &HewnDock::_on_uv_pair).bind(false, p_ratio));
		v->add_child(row);
	};
	pair("Scale", 1.0, uv_scale_u, uv_scale_v, uv_scale_link, true);
	pair("Offset", 0.0, uv_offset_u, uv_offset_v, uv_offset_link, false);
	uv_last_scale = Vector2(1, 1);
	uv_last_offset = Vector2();
	uv_rotation = spin(v, "Rotation", -360, 360, 0.1, 0);
	uv_rotation->set_allow_lesser(true);
	uv_rotation->connect("value_changed", callable_mp(this, &HewnDock::_on_uv_changed));
	HBoxContainer *h = memnew(HBoxContainer);
	Label *l = memnew(Label);
	l->set_text("Fill");
	l->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	h->add_child(l);
	uv_fill = memnew(OptionButton);
	uv_fill->add_item("Tile");
	uv_fill->add_item("Fit");
	uv_fill->add_item("Stretch");
	uv_fill->connect("item_selected", callable_mp(this, &HewnDock::_on_uv_fill));
	h->add_child(uv_fill);
	v->add_child(h);
	HFlowContainer *cf = flow(v);
	uv_world = memnew(CheckBox);
	uv_world->set_text("World space");
	uv_flip_u = memnew(CheckBox);
	uv_flip_u->set_text("Flip U");
	uv_flip_v = memnew(CheckBox);
	uv_flip_v->set_text("Flip V");
	uv_swap = memnew(CheckBox);
	uv_swap->set_text("Swap UV");
	for (CheckBox *c : { uv_world, uv_flip_u, uv_flip_v, uv_swap }) {
		c->connect("toggled", callable_mp(this, &HewnDock::_on_uv_toggled));
		cf->add_child(c);
	}
	HFlowContainer *f = flow(v);
	OP_BUTTON(f, "Group (project together)", "uv_group", "Selected faces share one projection");
	OP_BUTTON(f, "Ungroup", "uv_ungroup", "");
	OP_BUTTON(f, "Reset to Auto", "uv_reset", "Drop manual UVs and auto UV parameters (box projection)");
	v->add_child(memnew(HSeparator));
	// level design texturing
	header(v, "Texture");
	f = flow(v);
	const char *tools[3][3] = { { "Center", "center", "The texture centered on each face" },
		{ "Fit to Face", "fit", "The texture over the whole of each face, exactly one tile (along the face's edge nearest to the texture's U)" },
		{ "Randomize Offset", "random", "A random offset for each face (variety on repeated parts)" } };
	for (const auto &t : tools) {
		Button *b = memnew(Button);
		b->set_text(t[0]);
		b->set_tooltip_text(t[2]);
		b->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::uv_tool).bind(String(t[1])));
		f->add_child(b);
	}
	HBoxContainer *bxh = memnew(HBoxContainer);
	v->add_child(bxh);
	Button *bx = memnew(Button);
	bx->set_text("Box Projection");
	bx->set_tooltip_text("Plain projection from the side each face looks to, at this scale (1: the texel density of the Textures tab); the selected faces, or the whole of the selected objects");
	bx->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	bx->connect("pressed", callable_mp(this, &HewnDock::_on_uv_box_project));
	bxh->add_child(bx);
	uv_box_scale = memnew(SpinBox);
	HewnDragField::hook_spinbox(uv_box_scale);
	uv_box_scale->set_min(0.001);
	uv_box_scale->set_max(1000);
	uv_box_scale->set_step(0.001);
	uv_box_scale->set_value(1.0);
	uv_box_scale->set_tooltip_text("Scale of the box projection");
	bxh->add_child(uv_box_scale);
	{
		// Texture Settings: world space projection and the pixels per meter of new objects
		Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
		v->add_child(memnew(HSeparator));
		header(v, "World UV");
		new_uv_world = memnew(CheckBox);
		new_uv_world->set_name("NewWorldUV");
		new_uv_world->set_text("New Objects in World UV");
		new_uv_world->set_tooltip_text("New objects take their textures from where they are in the world");
		new_uv_world->set_pressed(es.is_valid() ? (bool)es->get_project_metadata("hewn", "new_world_uv", true) : true);
		new_uv_world->connect("toggled", callable_mp(this, &HewnDock::_on_new_uv_world));
		v->add_child(new_uv_world);
		HBoxContainer *ph = memnew(HBoxContainer);
		v->add_child(ph);
		Label *pl = memnew(Label);
		pl->set_text("Pixels");
		pl->set_tooltip_text("Texture pixels per meter on new objects");
		pl->set_mouse_filter(Control::MOUSE_FILTER_PASS);
		ph->add_child(pl);
		Button *half = memnew(Button);
		half->set_name("PixelsHalf");
		half->set_text("/2");
		half->set_tooltip_text("Half the pixels per meter: the texture twice as big");
		half->connect("pressed", callable_mp(this, &HewnDock::_new_uv_scale).bind(0.5f));
		ph->add_child(half);
		new_uv_pixels = memnew(SpinBox);
		new_uv_pixels->set_name("NewUVPixels");
		HewnDragField::hook_spinbox(new_uv_pixels);
		new_uv_pixels->set_min(1);
		new_uv_pixels->set_max(65536);
		new_uv_pixels->set_step(1);
		new_uv_pixels->set_value(es.is_valid() ? (double)es->get_project_metadata("hewn", "new_uv_pixels", 128) : 128.0);
		new_uv_pixels->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		new_uv_pixels->set_tooltip_text(pl->get_tooltip_text());
		new_uv_pixels->connect("value_changed", callable_mp(this, &HewnDock::_on_new_uv_pixels));
		ph->add_child(new_uv_pixels);
		Button *twice = memnew(Button);
		twice->set_name("PixelsDouble");
		twice->set_text("x2");
		twice->set_tooltip_text("Twice the pixels per meter: the texture half as big");
		twice->connect("pressed", callable_mp(this, &HewnDock::_new_uv_scale).bind(2.0f));
		ph->add_child(twice);
		Button *ap = memnew(Button);
		ap->set_name("ApplyWorldUV");
		ap->set_text("Apply World UV");
		ap->set_tooltip_text("The selected faces (edit mode) or the whole of the selected objects: projected from world space at these pixels per meter, as new objects are");
		ap->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::apply_world_uv));
		v->add_child(ap);
		HBoxContainer *alr = memnew(HBoxContainer);
		v->add_child(alr);
		Button *alf = memnew(Button);
		alf->set_name("AlignToFace");
		alf->set_text("Align to Face");
		alf->set_tooltip_text("The selected faces' world UVs on their own plane: a slope shows the texture true to size, not stretched (it may not meet the faces around without a seam)");
		alf->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		alf->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::align_world_uv).bind(true));
		alr->add_child(alf);
		Button *ala = memnew(Button);
		ala->set_name("AlignToAxis");
		ala->set_text("Align to Axis");
		ala->set_tooltip_text("The selected faces' world UVs along the nearest world axis again: the faces meet without seams, a slope stretches the texture");
		ala->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		ala->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::align_world_uv).bind(false));
		alr->add_child(ala);
	}
	uv_lock_button = memnew(CheckBox);
	uv_lock_button->set_name("UVLock");
	uv_lock_button->set_text("UV Lock");
	uv_lock_button->set_tooltip_text("Faces moved, turned or scaled in edit mode keep their UVs: the texture goes along like a sticker (off: it stays in place and the faces slide over it)");
	uv_lock_button->connect("toggled", callable_mp(this, &HewnDock::_on_uv_lock));
	v->add_child(uv_lock_button);
	v->add_child(memnew(HSeparator));
	// the texture of the selected faces, in texture tiles
	header(v, "Texture Tools");
	auto uvb = [&](Container *p_parent, const String &p_text, const String &p_cmd, const String &p_tip) {
		Button *b = memnew(Button);
		b->set_text(p_text);
		b->set_tooltip_text(p_tip);
		b->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::run_uv_command).bind(p_cmd));
		p_parent->add_child(b);
		return b;
	};
	auto step_field = [&](Container *p_parent, double p_min, double p_max, double p_step, double p_value, const String &p_suffix, const String &p_tip) {
		SpinBox *sb = memnew(SpinBox);
		HewnDragField::hook_spinbox(sb);
		sb->set_min(p_min);
		sb->set_max(p_max);
		sb->set_step(p_step);
		sb->set_value(p_value);
		sb->set_suffix(p_suffix);
		sb->set_tooltip_text(p_tip);
		sb->set_custom_minimum_size(Vector2(80, 0));
		p_parent->add_child(sb);
		return sb;
	};
	f = flow(v);
	uvb(f, "Move", "modal:0", "Move the texture with the mouse (U menu in the viewport)");
	uvb(f, "Rotate", "modal:1", "Rotate the texture with the mouse");
	uvb(f, "Scale", "modal:2", "Scale the texture with the mouse");
	f = flow(v);
	uv_step_move = step_field(f, 0.001, 16, 0.001, 0.125, "", "Step of the arrows (tiles)");
	uvb(f, String::chr(0x2190), "step_move:1:0", "The texture to the left by the step");
	uvb(f, String::chr(0x2192), "step_move:-1:0", "The texture to the right");
	uvb(f, String::chr(0x2191), "step_move:0:1", "The texture up");
	uvb(f, String::chr(0x2193), "step_move:0:-1", "The texture down");
	f = flow(v);
	uv_step_rotate = step_field(f, 0.1, 180, 0.1, 90, String::utf8("°"), "Step of the turn");
	uvb(f, String::chr(0x21BA), "step_rotate:1", "Turn the texture");
	uvb(f, String::chr(0x21BB), "step_rotate:-1", "Turn it the other way");
	uvb(f, "Cycle", "cycle", "A quarter turn inside the same bounds");
	f = flow(v);
	uv_step_scale = step_field(f, 1.01, 16, 0.01, 2, "x", "Factor of the scale buttons");
	uvb(f, "+", "step_scale:0:1", "The texture bigger");
	uvb(f, String::chr(0x2212), "step_scale:0:-1", "The texture smaller");
	uvb(f, "+U", "step_scale:1:1", "Bigger along U");
	uvb(f, String::chr(0x2212) + "U", "step_scale:1:-1", "Smaller along U");
	uvb(f, "+V", "step_scale:2:1", "Bigger along V");
	uvb(f, String::chr(0x2212) + "V", "step_scale:2:-1", "Smaller along V");
	f = flow(v);
	uvb(f, "Mirror U", "mirror_u", "Mirror inside the same bounds");
	uvb(f, "Mirror V", "mirror_v", "");
	uvb(f, "Fit U", "fit:1:0", "Exactly one tile across each face along U");
	uvb(f, "Fit V", "fit:0:1", "Exactly one tile along V");
	// justify: the texture's corner / side / middle on the face
	HBoxContainer *jh = memnew(HBoxContainer);
	v->add_child(jh);
	GridContainer *jg = memnew(GridContainer);
	jg->set_columns(3);
	jh->add_child(jg);
	static const int jarrows[9] = { 0x2196, 0x2191, 0x2197, 0x2190, 0x2022, 0x2192, 0x2199, 0x2193, 0x2198 };
	for (int i = 0; i < 9; i++) {
		const int hx = i % 3 - 1, hy = i / 3 - 1;
		uvb(jg, String::chr(jarrows[i]), vformat("justify:%d:%d", hx, hy), "Align the texture to this side / corner of each face (middle: center it)");
	}
	VBoxContainer *ev = memnew(VBoxContainer);
	jh->add_child(ev);
	Label *el = memnew(Label);
	el->set_text("To the tile edge:");
	ev->add_child(el);
	HFlowContainer *ef = memnew(HFlowContainer);
	ev->add_child(ef);
	uvb(ef, "Left", "edge:-1:0", "The whole selection to the left edge of its tile");
	uvb(ef, "Right", "edge:1:0", "");
	uvb(ef, "Top", "edge:0:-1", "");
	uvb(ef, "Bottom", "edge:0:1", "");
	f = flow(v);
	uv_inset_px = step_field(f, 0.5, 512, 0.5, 8, " px", "Pixels of the face's texture");
	uvb(f, "Inset UV", "inset:1", "Shrink the UVs by these pixels on each side (keeps a trim off its neighbors)");
	uvb(f, "Expand UV", "inset:-1", "");
	f = flow(v);
	uvb(f, "Extend from Active", "extend", "The texture of the active face runs on over the other selected faces");
	uvb(f, "Square Fit Unwrap", "unwrap", "Connected quads laid out as a grid fitted to one tile: trims on pipes, arches, strips");
	f = flow(v);
	uvb(f, "Copy UV Rect", "grab", "Remember the UV bounds of the selection");
	uvb(f, "Paste UV Rect", "transfer", "Fit the selection into the remembered UV bounds");
	uv_rect_label = memnew(Label);
	uv_rect_label->set_text("0, 0  -  1, 1");
	f->add_child(uv_rect_label);
	v->add_child(memnew(HSeparator));
	header(v, "Manual UV");
	f = flow(v);
	OP_BUTTON(f, "Rotate 90", "uv_rotate90", "");
	OP_BUTTON(f, "Flip U", "uv_flip_u", "");
	OP_BUTTON(f, "Flip V", "uv_flip_v", "");
	OP_BUTTON(f, "Fit 0..1", "uv_fit", "");
	OP_BUTTON(f, "Open UV Editor", "uv_editor", "");
}

#undef OP_BUTTON

void HewnDock::_on_uv_link_toggled(bool p_on, Button *p_button) {
	p_button->set_button_icon(icon(p_on ? "Instance" : "Unlinked"));
}

// One of a linked pair changed: the other follows (scale: by the same factor, offset: by the same step).
void HewnDock::_on_uv_pair(double p_value, bool p_is_u, bool p_ratio) {
	SpinBox *su = p_ratio ? uv_scale_u : uv_offset_u, *sv = p_ratio ? uv_scale_v : uv_offset_v;
	Button *link = p_ratio ? uv_scale_link : uv_offset_link;
	Vector2 &last = p_ratio ? uv_last_scale : uv_last_offset;
	if (!syncing && link && link->is_pressed()) {
		SpinBox *other = p_is_u ? sv : su;
		const double old_this = p_is_u ? last.x : last.y;
		double o = other->get_value();
		if (p_ratio) {
			o = std::fabs(old_this) > 1e-9 ? o * p_value / old_this : p_value;
		} else {
			o += p_value - old_this;
		}
		syncing = true;
		other->set_value(o);
		syncing = false;
	}
	last = Vector2(su->get_value(), sv->get_value());
	_on_uv_changed(p_value);
}

void HewnDock::_on_uv_changed(double) {
	if (!syncing) {
		plugin->run_op("uv_apply");
	}
}

void HewnDock::_on_uv_toggled(bool) {
	if (!syncing) {
		plugin->run_op("uv_apply");
	}
}

void HewnDock::_on_uv_fill(int) {
	if (!syncing) {
		plugin->run_op("uv_apply");
	}
}

void HewnDock::apply_uv_params() {
	HewnMesh3D *n = plugin->get_edited();
	if (!n) {
		return;
	}
	hewn::Mesh &m = n->get_data();
	for (int f : n->selection.faces) {
		hewn::UVParams &p = m.faces[f].uvp;
		p.scale = Vector2(uv_scale_u->get_value(), uv_scale_v->get_value());
		p.offset = Vector2(uv_offset_u->get_value(), uv_offset_v->get_value());
		p.rotation = (float)uv_rotation->get_value();
		p.fill = uv_fill->get_selected();
		p.world = uv_world->is_pressed();
		p.flip_u = uv_flip_u->is_pressed();
		p.flip_v = uv_flip_v->is_pressed();
		p.swap = uv_swap->is_pressed();
		m.faces[f].manual_uv = false;
	}
}

void HewnDock::_on_paint_channel(int p_index) {
	// the chosen channel's format, as the edited mesh has it
	HewnMesh3D *n = plugin ? plugin->get_edited() : nullptr;
	if (n && p_index > 0 && n->custom_formats[p_index - 1] >= 0) {
		paint_format->select(n->custom_formats[p_index - 1]);
	}
}

void HewnDock::_on_paint_format(int p_index) {
	HewnMesh3D *n = plugin ? plugin->get_edited() : nullptr;
	const int ch = paint_channel ? paint_channel->get_selected() - 1 : -1;
	if (!n || ch < 0) {
		return;
	}
	PackedInt32Array f = n->get_custom_formats();
	f.set(ch, p_index);
	EditorUndoRedoManager *ur = plugin->get_undo_redo();
	ur->create_action("Hewn: Custom Format", UndoRedo::MERGE_DISABLE, n);
	ur->add_do_method(n, "set_custom_formats", f);
	ur->add_undo_method(n, "set_custom_formats", n->get_custom_formats());
	ur->commit_action();
}

Color HewnDock::get_paint_color() const {
	return color_picker->get_pick_color();
}

void HewnDock::_on_material_picked(const Ref<Resource> &p_res) {
	Ref<Material> mat = p_res;
	material_picker->set_edited_resource(Ref<Resource>());
	if (mat.is_valid()) {
		plugin->apply_material_to_selection(mat);
	}
}

void HewnDock::refresh() {
	_on_remesh_changed(); // (the Quad remesh's estimate follows the selection)
	if (!plugin) {
		return;
	}
	HewnMesh3D *n = plugin->get_edited();
	for (int i = 0; i < 4; i++) {
		mode_buttons[i]->set_pressed_no_signal(i == 0 ? plugin->mode == 0 : plugin->has_mode(i));
	}
	if (interaction_menu) {
		show_paint_bar(plugin->vpaint || plugin->sculpt); // (the mode menu's icon)
	}
	// always shown in the 3D editor: shapes can be added with nothing selected (the buttons that need
	// a mesh are disabled without one)
	// (several element modes at once: the operations of each of them)
	const bool any_edit = plugin->mode != HewnEditorPlugin::MODE_OBJECT;
	face_ops->set_visible(any_edit && plugin->has_mode(HewnEditorPlugin::MODE_FACE));
	edge_ops->set_visible(any_edit && plugin->has_mode(HewnEditorPlugin::MODE_EDGE));
	vertex_ops->set_visible(any_edit && plugin->has_mode(HewnEditorPlugin::MODE_VERTEX));

	if (!n) {
		status->set_text(hw_editor::T("Select a HewnMesh3D, or create one in the Create tab."));
	} else {
		const hewn::Selection &s = n->selection;
		status->set_text(vformat(hw_editor::T("%s: %s\nSelected: %d verts, %d edges, %d faces"), n->get_name(), n->get_info(),
				(int)s.verts.size(), (int)s.edges.size(), (int)s.faces.size()));
	}

	// Material slots.
	for (int i = material_slots->get_child_count() - 1; i >= 0; i--) {
		Node *c = material_slots->get_child(i);
		material_slots->remove_child(c);
		c->queue_free();
	}
	const bool edit = n && plugin->mode != HewnEditorPlugin::MODE_OBJECT;
	material_hint->set_text(edit ? hw_editor::T("Click a texture in the Textures tab (bottom panel), or a material below: it goes onto the selected faces.")
								 : hw_editor::T("Click a texture in the Textures tab (bottom panel), or a material below: it goes onto the selected objects. Tab: faces."));
	if (n) {
		// the materials the faces use (slots no face uses are dropped when materials are applied)
		TypedArray<Material> mats = n->get_materials();
		std::vector<int> used(MAX(1, (int)mats.size()), 0);
		for (const hewn::Face &f : n->get_data().faces) {
			if (f.material >= 0 && f.material < (int)used.size()) {
				used[f.material]++;
			}
		}
		for (int i = 0; i < (int)used.size(); i++) {
			if (!used[i]) {
				continue;
			}
			Ref<Material> mat = i < mats.size() ? Ref<Material>(mats[i]) : Ref<Material>();
			String name = mat.is_null() ? hw_editor::T("(default)") : (mat->get_name().is_empty() ? mat->get_path().get_file() : mat->get_name());
			if (name.is_empty()) {
				name = mat->get_class();
			}
			Button *b = memnew(Button);
			b->set_text(vformat(hw_editor::T("%s  (%d faces)"), name, used[i]));
			b->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
			b->set_tooltip_text(edit ? hw_editor::T("Put it on the selected faces") : hw_editor::T("Put it on the selected objects"));
			b->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);
			b->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::apply_material_to_selection).bind(mat));
			material_slots->add_child(b);
		}
	}

	_refresh_texture_section();

	// Auto UV: the parameters of selected faces (face mode); else locked
	const bool uv_on = n && plugin->mode == HewnEditorPlugin::MODE_FACE && !n->selection.faces.empty();
	for (SpinBox *s : { uv_scale_u, uv_scale_v, uv_offset_u, uv_offset_v, uv_rotation }) {
		s->set_editable(uv_on);
		// (not editable still drags the value with the mouse: locked, it takes no mouse at all)
		s->set_mouse_filter(uv_on ? Control::MOUSE_FILTER_STOP : Control::MOUSE_FILTER_IGNORE);
		s->get_line_edit()->set_mouse_filter(uv_on ? Control::MOUSE_FILTER_STOP : Control::MOUSE_FILTER_IGNORE);
		s->set_modulate(Color(1, 1, 1, uv_on ? 1.0f : 0.5f));
	}
	for (BaseButton *b : std::initializer_list<BaseButton *>{ uv_fill, uv_world, uv_flip_u, uv_flip_v, uv_swap, uv_scale_link, uv_offset_link }) {
		b->set_disabled(!uv_on);
	}
	uv_fill->set_tooltip_text(uv_on ? String() : hw_editor::T("Select faces in face mode to change their auto UV"));
	// UV params from the first selected face.
	if (n && !n->selection.faces.empty()) {
		const int f = n->selection.faces[0];
		if (f < (int)n->get_data().faces.size()) {
			const hewn::UVParams &p = n->get_data().faces[f].uvp;
			syncing = true;
			uv_scale_u->set_value(p.scale.x);
			uv_scale_v->set_value(p.scale.y);
			uv_offset_u->set_value(p.offset.x);
			uv_offset_v->set_value(p.offset.y);
			uv_rotation->set_value(p.rotation);
			uv_fill->select(p.fill);
			uv_world->set_pressed(p.world);
			uv_flip_u->set_pressed(p.flip_u);
			uv_flip_v->set_pressed(p.flip_v);
			uv_swap->set_pressed(p.swap);
			syncing = false;
		}
	}
}
