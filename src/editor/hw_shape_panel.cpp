// Shape parameters panel: while a parametric shape (not edited in edit mode yet) is selected in object
// mode, its parameters are shown at the bottom left of the viewport last-operation panel
// after adding a primitive. Changes are undoable; the shape keeps resting on its base.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/editor_file_dialog.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_resource_picker.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/style_box.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/undo_redo.hpp>

using namespace godot;

// The viewport the panels go to: the one used last, or the first 3D viewport.
Control *HewnEditorPlugin::_main_surface() const {
	Control *surface = last_surface ? Object::cast_to<Control>(ObjectDB::get_instance(last_surface)) : nullptr;
	if (surface) {
		return surface;
	}
	SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(0);
	return vp && vp->get_camera_3d() ? _surface_for(vp->get_camera_3d()) : nullptr;
}

void HewnEditorPlugin::_hide_shape_panel() {
	shape_panel_node = 0;
	shape_panel_type = -1;
	if (shape_panel) {
		shape_panel->hide();
	}
}

void HewnEditorPlugin::_update_shape_panel() {
	HewnMesh3D *n = get_edited();
	Control *surface = _main_surface();
	// a shape just built into the edited mesh: its own panel, until anything else happens
	const bool into = _into_panel_valid();
	if (into) {
		n = into_shape;
	} else if (into_shape && modal == MODAL_NONE) {
		_clear_into();
	}
	const bool want = n && (into || (n->is_inside_tree() && mode == MODE_OBJECT)) && modal == MODAL_NONE && n->get_shape() != hewn::shapes::CUSTOM && surface &&
			surface->is_visible_in_tree() && !_scene_panel_up();
	if (!want) {
		if (shape_panel && shape_panel->is_visible()) {
			_hide_shape_panel();
		}
		return;
	}
	const hewn::shapes::Type type = (hewn::shapes::Type)n->get_shape();
	const std::vector<hewn::shapes::Param> &ps = hewn::shapes::params(type);
	// Up to date already?
	if (shape_panel && shape_panel->is_visible() && shape_panel_node == n->get_instance_id() && shape_panel_type == (int)type &&
			shape_panel->get_parent() == surface) {
		bool same = true;
		for (const hewn::shapes::Param &p : ps) {
			const float cur = n->get_shape_param(p.name);
			if (std::fabs(cur - (float)(double)shape_panel_values.get(p.name, 1e30)) > 1e-6f) {
				same = false;
				break;
			}
		}
		if (same) {
			return;
		}
	}
	if (!shape_panel) {
		shape_panel = memnew(PanelContainer);
		shape_panel->set_name("HewnShapePanel");
		Ref<StyleBox> sb = EditorInterface::get_singleton()->get_editor_theme()->get_stylebox("panel", "PopupPanel");
		if (sb.is_valid()) {
			shape_panel->add_theme_stylebox_override("panel", sb);
		}
		VBoxContainer *v = memnew(VBoxContainer);
		shape_panel->add_child(v);
		Button *title = memnew(Button);
		title->set_name("Title");
		title->connect("pressed", callable_mp(this, &HewnEditorPlugin::_toggle_redo));
		v->add_child(title);
		_style_panel_header(title, shape_panel);
		shape_body = memnew(GridContainer);
		shape_body->set_columns(2);
		v->add_child(shape_body);
		_compact_panel(shape_panel, shape_body);
		HBoxContainer *buttons = memnew(HBoxContainer);
		buttons->set_name("Buttons");
		v->add_child(buttons);
		Button *edit = memnew(Button);
		edit->set_name("EditBase");
		edit->set_text("Edit Base Points");
		edit->set_tooltip_text("Move the outline points (drag), add one (click an outline edge), remove one (click it)");
		edit->connect("pressed", callable_mp(this, &HewnEditorPlugin::_on_shape_edit_base));
		buttons->add_child(edit);
		// the curves of the shape in the profile window (column parts, sweep, door / arch / wedge)
		for (int i = 0; i < 3; i++) {
			Button *pb = memnew(Button);
			pb->set_name("Profile" + itos(i));
			if (dock) {
				pb->set_button_icon(dock->icon("curve"));
			}
			pb->connect("pressed", callable_mp(this, &HewnEditorPlugin::_on_shape_profile_button).bind(i));
			buttons->add_child(pb);
		}
		// a sweep's / column's settings (and its curves) kept in a file, and put on another one
		for (int i = 0; i < 2; i++) {
			Button *b = memnew(Button);
			b->set_name(i == 0 ? "PresetSave" : "PresetLoad");
			if (dock) {
				b->set_button_icon(dock->icon(i == 0 ? "Save" : "Load"));
			}
			b->set_tooltip_text(i == 0 ? "Save its settings (and its curves) as a preset file" : "Load a preset file: its settings (and curves) onto this one");
			b->connect("pressed", callable_mp(this, &HewnEditorPlugin::_on_preset_button).bind(i == 0));
			buttons->add_child(b);
		}
	}
	if (shape_panel->get_parent() != surface) {
		if (shape_panel->get_parent()) {
			shape_panel->get_parent()->remove_child(shape_panel);
		}
		surface->add_child(shape_panel);
	}
	shape_panel_node = n->get_instance_id();
	shape_panel_type = (int)type;
	shape_panel_values = Dictionary();
	Button *title = Object::cast_to<Button>(shape_panel->find_child("Title", true, false));
	if (title) {
		title->set_text(hw_editor::T("Add ") + hw_editor::T(hewn::shapes::type_name(type)));
		_style_panel_header(title, nullptr);
	}
	shape_body->set_visible(!redo_collapsed);
	Node *edit = shape_panel->find_child("EditBase", true, false);
	if (Button *eb = Object::cast_to<Button>(edit)) {
		eb->set_visible(!redo_collapsed && !into && (type == hewn::shapes::POLY_SHAPE || type == hewn::shapes::WALL || type == hewn::shapes::SWEEP || type == hewn::shapes::PATH_STAIRS));
		eb->set_text(type == hewn::shapes::WALL ? "Edit Wall Points" : (type == hewn::shapes::SWEEP || type == hewn::shapes::PATH_STAIRS) ? "Edit Path Points" : "Edit Base Points");
	}
	{
		static const char *column[3] = { "Base", "Shaft", "Top" };
		for (int i = 0; i < 3; i++) {
			Button *pb = Object::cast_to<Button>(shape_panel->find_child("Profile" + itos(i), true, false));
			if (!pb) {
				continue;
			}
			bool show = false;
			String text, tip;
			if (type == hewn::shapes::COLUMN) {
				show = true;
				text = column[i];
				tip = String("The curve of the column's ") + String(column[i]).to_lower() + " (the profile window: points, handles, Godot curves)";
			} else if (i == 0 && type == hewn::shapes::SWEEP) {
				show = (int)n->get_shape_param("profile") != 8;
				text = "Profile";
				tip = "The sweep's cross section as a curve (the profile window)";
			} else if (i == 1 && type == hewn::shapes::SWEEP) {
				show = (int)n->get_shape_param("accent") != 0 && (int)n->get_shape_param("accent_shape") != 8;
				text = "Accent Shape";
				tip = "The accents' own cross section as a curve (the profile window)";
			} else if (i == 0 && (type == hewn::shapes::DOOR || type == hewn::shapes::ARCH || type == hewn::shapes::WEDGE)) {
				show = true;
				text = "Curve";
				tip = type == hewn::shapes::DOOR ? "The opening's outline as a curve (the profile window)" : type == hewn::shapes::ARCH ? "The arch's inner line as a curve (the profile window)" : "The slope as a curve (the profile window)";
			} else if (i == 1 && (type == hewn::shapes::DOOR || type == hewn::shapes::ARCH || type == hewn::shapes::WEDGE) && n->get_shape_profile().is_valid()) {
				show = true;
				text = "Plain";
				tip = "Back to the shape made by its parameters (the curve is dropped)";
			}
			pb->set_visible(show && !redo_collapsed && !into);
			pb->set_text(hw_editor::T(text));
			pb->set_tooltip_text(hw_editor::T(tip));
		}
		for (const char *pn : { "PresetSave", "PresetLoad" }) {
			if (Control *pc = Object::cast_to<Control>(shape_panel->find_child(pn, true, false))) {
				pc->set_visible(!redo_collapsed && !into && (type == hewn::shapes::SWEEP || type == hewn::shapes::COLUMN));
			}
		}
	}
	for (int i = shape_body->get_child_count() - 1; i >= 0; i--) {
		Node *c = shape_body->get_child(i);
		shape_body->remove_child(c);
		c->queue_free();
	}
	const bool path_wall = type == hewn::shapes::WALL && n->get_poly_points().size() >= 2;
	std::vector<float> values;
	for (const hewn::shapes::Param &p : ps) {
		values.push_back(n->get_shape_param(p.name));
	}
	// long parameter lists in foldable groups (stairs: size, steps, curve, sides, border, ...)
	String group;
	bool group_open = true;
	std::vector<int> order;
	for (int i = 0; i < (int)ps.size(); i++) {
		order.push_back(i);
	}
	std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return hewn::shapes::param_group_index(type, ps[a].name) < hewn::shapes::param_group_index(type, ps[b].name); });
	for (int oi : order) {
		const hewn::shapes::Param &p = ps[oi];
		const float v = n->get_shape_param(p.name);
		shape_panel_values[p.name] = v;
		if (path_wall && String(p.name).begins_with("length")) {
			continue; // a wall with corners follows its points
		}
		if (!hewn::shapes::param_shown(type, values, p.name)) {
			continue;
		}
		const String g = hewn::shapes::param_group(type, p.name);
		if (g != group) {
			group = g;
			group_open = (bool)shape_groups_open.get(g, hewn::shapes::param_group_index(type, p.name) <= 1);
			Button *gh = memnew(Button);
			gh->set_flat(true);
			gh->set_text(String::chr(group_open ? 0x25BE : 0x25B8) + " " + hw_editor::T(g));
			gh->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
			gh->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);
			gh->add_theme_color_override("font_color", Color(0.7f, 0.8f, 1.0f));
			gh->connect("pressed", callable_mp(this, &HewnEditorPlugin::_on_shape_group).bind(g));
			shape_body->add_child(gh);
			shape_body->add_child(memnew(Control));
		}
		if (!group_open) {
			continue;
		}
		if (type == hewn::shapes::SWEEP && (String(p.name) == "mesh_place" || String(p.name) == "accent_mesh_scale")) {
			// the mesh set along the sweep / its accents' mesh, picked here too (not only in the inspector)
			const bool accent = String(p.name) == "accent_mesh_scale";
			Label *ml = memnew(Label);
			ml->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);
			_compact_label(ml, hw_editor::T(accent ? "Accent Mesh" : "Mesh"));
			shape_body->add_child(ml);
			EditorResourcePicker *rp = memnew(EditorResourcePicker);
			rp->set_name(accent ? "AccentMesh" : "SweepMesh");
			rp->set_base_type("Mesh");
			rp->set_edited_resource(accent ? Ref<Resource>(n->get_accent_mesh()) : Ref<Resource>(n->get_sweep_mesh()));
			rp->set_custom_minimum_size(Vector2(80 * EditorInterface::get_singleton()->get_editor_scale(), 0));
			rp->connect("resource_changed", callable_mp(this, &HewnEditorPlugin::_on_shape_mesh).bind(String(accent ? "accent_mesh" : "sweep_mesh")));
			shape_body->add_child(rp);
		}
		Label *l = memnew(Label);
		l->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED); // (the editor would translate only some names)
		_compact_label(l, hw_editor::T(String(p.name).capitalize()));
		shape_body->add_child(l);
		if (p.options) {
			OptionButton *o = memnew(OptionButton);
			const PackedStringArray items = String(p.options).split(",");
			for (int64_t i = 0; i < items.size(); i++) {
				o->add_item(items[i], (int)i);
			}
			o->select((int)v);
			o->connect("item_selected", callable_mp(this, &HewnEditorPlugin::_on_shape_option).bind(String(p.name)));
			shape_body->add_child(hw_editor::with_revert(o, (int)p.def));
		} else if (p.is_toggle()) {
			CheckBox *c = memnew(CheckBox);
			c->set_pressed(v > 0.5f);
			c->connect("toggled", callable_mp(this, &HewnEditorPlugin::_on_shape_bool).bind(String(p.name)));
			shape_body->add_child(hw_editor::with_revert(c, p.def > 0.5f));
		} else {
			HewnDragField *f = memnew(HewnDragField);
			// angles (degrees) drag at about a degree per pixel; lengths in thousandths
			const String pn = p.name;
			const double fstep = pn == "curvature" ? 0.1 : pn.contains("angle") ? 0.5 : 0.001; // per pixel: twice the step
			f->setup(p.min, p.max, p.is_int ? 1.0 : fstep, p.is_int, v);
			f->set_custom_minimum_size(Vector2(80 * EditorInterface::get_singleton()->get_editor_scale(), 0));
			f->connect("value_changed", callable_mp(this, &HewnEditorPlugin::_on_shape_value).bind(String(p.name)));
			shape_body->add_child(hw_editor::with_revert(f, (double)p.def));
		}
	}
	if (into && dock && dock->keep_overlap) {
		// built into the edited mesh: whether the faces where it touches the mesh stay (changed: built again)
		Label *kl = memnew(Label);
		kl->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);
		_compact_label(kl, hw_editor::T("Keep Overlap Faces"));
		shape_body->add_child(kl);
		CheckBox *kc = memnew(CheckBox);
		kc->set_name("KeepOverlap");
		kc->set_pressed(dock->keep_overlap->is_pressed());
		kc->connect("toggled", callable_mp(this, &HewnEditorPlugin::_on_into_keep_overlap));
		shape_body->add_child(kc);
	}
	// what the stairs' parameters give: one line under the fields (not a narrow column)
	Container *body_box = Object::cast_to<Container>(shape_body->get_parent());
	if (Node *old = body_box ? body_box->find_child("StairsInfo", false, false) : nullptr) {
		body_box->remove_child(old);
		old->queue_free();
	}
	if (type == hewn::shapes::STAIRS && body_box && !redo_collapsed) {
		int steps = 0;
		float tread = 0.0f, riser = 0.0f;
		hewn::shapes::stairs_info(values, steps, tread, riser);
		Label *r = memnew(Label);
		r->set_name("StairsInfo");
		r->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);
		r->set_autowrap_mode(TextServer::AUTOWRAP_OFF);
		r->set_text(vformat(hw_editor::T("%d steps, tread %.3f, riser %.3f"), steps, tread, riser));
		r->add_theme_color_override("font_color", Color(0.7f, 0.8f, 1.0f));
		body_box->add_child(r);
	}
	hw_editor::localize(shape_panel);
	shape_panel->reset_size();
	shape_panel->set_position(_left_panel_pos(surface, shape_panel));
	shape_panel->show();
}

void HewnEditorPlugin::_on_shape_group(const String &p_group) {
	HewnMesh3D *n = shape_panel_node ? Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(shape_panel_node)) : nullptr;
	const bool open = (bool)shape_groups_open.get(p_group, n && hewn::shapes::param_group_index((hewn::shapes::Type)n->get_shape(), "") <= 1);
	bool cur = open;
	if (n) {
		// (the default of a group not toggled yet: its first parameter's)
		for (const hewn::shapes::Param &p : hewn::shapes::params((hewn::shapes::Type)n->get_shape())) {
			if (hewn::shapes::param_group((hewn::shapes::Type)n->get_shape(), p.name) == p_group) {
				cur = (bool)shape_groups_open.get(p_group, hewn::shapes::param_group_index((hewn::shapes::Type)n->get_shape(), p.name) <= 1);
				break;
			}
		}
	}
	shape_groups_open[p_group] = !cur;
	shape_panel_values.clear(); // (rebuilt)
}

void HewnEditorPlugin::_on_shape_value(double p_value, const String &p_name) {
	_set_shape_value(p_name, (float)p_value);
}

void HewnEditorPlugin::_on_shape_bool(bool p_value, const String &p_name) {
	_set_shape_value(p_name, p_value ? 1.0f : 0.0f);
	shape_panel_values.clear(); // (rebuilt: other parameters may show / hide)
}

void HewnEditorPlugin::_on_shape_option(int p_index, const String &p_name) {
	if (p_name == "side" && shape_panel_type == hewn::shapes::WALL) {
		wall_side = p_index; // (the next walls too)
	}
	_set_shape_value(p_name, (float)p_index);
	shape_panel_values.clear();
}

std::vector<HewnMesh3D *> HewnEditorPlugin::_shape_targets(HewnMesh3D *p_main) const {
	std::vector<HewnMesh3D *> out{ p_main };
	if (p_main == into_shape || mode != MODE_OBJECT) {
		return out;
	}
	for (HewnMesh3D *o : _selected_meshes()) {
		if (o != p_main && o->get_shape() == p_main->get_shape() && o->is_inside_tree()) {
			out.push_back(o);
		}
	}
	return out;
}

void HewnEditorPlugin::_set_shape_value(const String &p_name, float p_value) {
	HewnMesh3D *main = shape_panel_node ? Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(shape_panel_node)) : nullptr;
	if (!main || main->get_shape() == hewn::shapes::CUSTOM) {
		return;
	}
	const hewn::shapes::Type type = (hewn::shapes::Type)main->get_shape();
	struct Change {
		HewnMesh3D *n;
		float old, old_climb, new_climb;
		Transform3D xf_old, xf_new;
	};
	std::vector<Change> changes;
	// (the other selected shapes of the same type change with it)
	for (HewnMesh3D *n : _shape_targets(main)) {
		const float old = n->get_shape_param(p_name);
		if (std::fabs(old - p_value) < 1e-7f) {
			continue;
		}
		auto min_y = [&]() {
			float y = 0.0f;
			bool first = true;
			for (const Vector3 &p : n->get_data().pos) {
				y = first ? p.y : MIN(y, p.y);
				first = false;
			}
			return y;
		};
		auto values = [&]() {
			std::vector<float> v;
			for (const hewn::shapes::Param &p : hewn::shapes::params(type)) {
				v.push_back(n->get_shape_param(p.name));
			}
			return v;
		};
		const Transform3D xf_old = n->get_transform();
		const float y0 = min_y();
		const Vector3 a0 = hewn::shapes::anchor_point(type, values());
		// stairs climb along their long side: a size change that makes the other side the long one would turn them;
		// the setting is flipped instead, so they keep climbing the way they did
		const float old_climb = n->get_shape_param("climb_along_long_side");
		float new_climb = old_climb;
		if (type == hewn::shapes::STAIRS && (p_name == "width" || p_name == "depth")) {
			const float w0 = n->get_shape_param("width"), d0 = n->get_shape_param("depth");
			const float w1 = p_name == "width" ? p_value : w0, d1 = p_name == "depth" ? p_value : d0;
			if ((d0 >= w0) != (d1 >= w1)) {
				new_climb = old_climb > 0.5f ? 0.0f : 1.0f;
				n->set_shape_param("climb_along_long_side", new_climb);
			}
		}
		n->set_shape_param(p_name, p_value);
		const float y1 = min_y();
		Transform3D xf_new = xf_old;
		if (type == hewn::shapes::STAIRS && (p_name == "curvature" || p_name == "inner_radius")) {
			// bending stairs: the first step stays where it is (other changes keep the box in place)
			xf_new.origin += xf_old.basis.xform(a0 - hewn::shapes::anchor_point(type, values()));
		} else if (type != hewn::shapes::SWEEP && n->get_poly_points().size() < 2) {
			// keep the base where it was (the shape grows upwards, like when it was drawn); a sweep, a wall through points
			// stay on their path (Downward: the wall below it)
			xf_new.origin += xf_old.basis.xform(Vector3(0, y0 - y1, 0));
		}
		n->set_transform(xf_new);
		changes.push_back({ n, old, old_climb, new_climb, xf_old, xf_new });
	}
	if (changes.empty()) {
		return;
	}
	shape_panel_values[p_name] = main->get_shape_param(p_name);
	if (main == into_shape) {
		_rebuild_into(); // (built into the edited mesh: its history step changes)
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Shape " + p_name.capitalize(), UndoRedo::MERGE_ENDS, main);
	for (const Change &c : changes) {
		if (c.new_climb != c.old_climb) {
			ur->add_do_method(c.n, "set_shape_param", "climb_along_long_side", c.new_climb);
		}
		ur->add_do_method(c.n, "set_shape_param", p_name, p_value);
		ur->add_do_method(c.n, "set_transform", c.xf_new);
		ur->add_undo_method(c.n, "set_shape_param", p_name, c.old);
		if (c.new_climb != c.old_climb) {
			ur->add_undo_method(c.n, "set_shape_param", "climb_along_long_side", c.old_climb);
		}
		ur->add_undo_method(c.n, "set_transform", c.xf_old);
	}
	ur->commit_action(false);
	_keep_pending(main); // (still the shape just drawn)
}

// The sweep's mesh / accents' mesh picked in the panel (the selected sweeps all get it).
void HewnEditorPlugin::_on_shape_mesh(const Ref<Resource> &p_res, const String &p_prop) {
	HewnMesh3D *main = shape_panel_node ? Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(shape_panel_node)) : nullptr;
	if (!main || main->get_shape() != hewn::shapes::SWEEP) {
		return;
	}
	const String key = p_prop == "accent_mesh" ? "accent_shape" : "profile";
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(p_prop == "accent_mesh" ? "Hewn: Accent Mesh" : "Hewn: Sweep Mesh");
	for (HewnMesh3D *n : _shape_targets(main)) {
		ur->add_do_method(n, "set", p_prop, p_res);
		ur->add_undo_method(n, "set", p_prop, n->get(p_prop));
		ur->add_undo_method(n, "set_shape_param", key, n->get_shape_param(key));
	}
	ur->commit_action();
	shape_panel_values.clear(); // (rebuilt)
}

void HewnEditorPlugin::_on_into_keep_overlap(bool p_on) {
	if (dock && dock->keep_overlap) {
		dock->keep_overlap->set_pressed(p_on);
	}
	_rebuild_into();
}

void HewnEditorPlugin::_on_shape_edit_base() {
	HewnMesh3D *n = shape_panel_node ? Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(shape_panel_node)) : nullptr;
	if (n && (n->get_shape() == hewn::shapes::POLY_SHAPE || n->get_shape() == hewn::shapes::WALL || n->get_shape() == hewn::shapes::SWEEP || n->get_shape() == hewn::shapes::PATH_STAIRS)) {
		start_poly_tool(true);
	}
}

// ------------------------------------------------------------------ presets

namespace {
void preset_toast(const String &p_text) {
	if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
		t->push_toast(p_text);
	}
}
const char *PRESET_RES[] = { "base_profile", "shaft_profile", "top_profile", "sweep_profile", "shape_profile", "sweep_mesh", "accent_mesh" };
}

void HewnEditorPlugin::_on_preset_button(bool p_save) {
	HewnMesh3D *n = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(shape_panel_node)));
	if (!n) {
		return;
	}
	if (!preset_dialog) {
		preset_dialog = memnew(EditorFileDialog);
		preset_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
		preset_dialog->add_filter("*.tres", "Hewn Preset");
		preset_dialog->connect("file_selected", callable_mp(this, &HewnEditorPlugin::_on_preset_file));
		EditorInterface::get_singleton()->get_base_control()->add_child(preset_dialog);
	}
	preset_saving = p_save;
	preset_dialog->set_file_mode(p_save ? EditorFileDialog::FILE_MODE_SAVE_FILE : EditorFileDialog::FILE_MODE_OPEN_FILE);
	preset_dialog->set_title(hw_editor::T(p_save ? "Save Preset" : "Load Preset"));
	if (p_save) {
		preset_dialog->set_current_file(String(n->get_name()).to_snake_case() + "_preset.tres");
	}
	preset_dialog->popup_file_dialog();
}

// A preset: a plain resource holding the shape's kind, its parameters and its curves / meshes (copies of its own).
void HewnEditorPlugin::_on_preset_file(const String &p_path) {
	HewnMesh3D *n = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(shape_panel_node)));
	if (!n) {
		return;
	}
	const hewn::shapes::Type type = (hewn::shapes::Type)n->get_shape();
	if (preset_saving) {
		Ref<Resource> r;
		r.instantiate();
		r->set_meta("hw_preset_shape", (int)type);
		Dictionary vals;
		for (const hewn::shapes::Param &p : hewn::shapes::params(type)) {
			vals[p.name] = n->get_shape_param(p.name);
		}
		r->set_meta("hw_preset_values", vals);
		for (const char *rp : PRESET_RES) {
			const Ref<Resource> v = n->get(rp);
			if (v.is_valid()) {
				r->set_meta(String("hw_preset_") + rp, v->get_path().is_empty() ? v->duplicate(true) : v);
			}
		}
		const Error err = ResourceSaver::get_singleton()->save(r, p_path);
		preset_toast(err == OK ? vformat(hw_editor::T("Saved: %s"), p_path) : vformat(hw_editor::T("Could not save %s"), p_path));
		EditorInterface::get_singleton()->get_resource_filesystem()->scan();
		return;
	}
	const Ref<Resource> r = ResourceLoader::get_singleton()->load(p_path, "", ResourceLoader::CACHE_MODE_IGNORE);
	if (r.is_null() || !r->has_meta("hw_preset_shape")) {
		preset_toast(hw_editor::T("Not a Hewn preset"));
		return;
	}
	if ((int)r->get_meta("hw_preset_shape") != (int)type) {
		preset_toast(hw_editor::T("This preset is for another kind of shape"));
		return;
	}
	const Dictionary vals = r->get_meta("hw_preset_values");
	EditorUndoRedoManager *ur = get_undo_redo();
	const Dictionary before = n->get_state();
	ur->create_action("Hewn: Load Preset", UndoRedo::MERGE_DISABLE, n);
	for (const char *rp : PRESET_RES) {
		const String key = String("hw_preset_") + rp;
		const Variant v = r->has_meta(key) ? r->get_meta(key) : Variant();
		const Ref<Resource> res = v;
		ur->add_do_property(n, rp, res.is_valid() && res->get_path().is_empty() ? Variant(res->duplicate(true)) : v);
		ur->add_undo_property(n, rp, n->get(rp));
	}
	const Array keys = vals.keys();
	for (int64_t i = 0; i < keys.size(); i++) {
		ur->add_do_method(n, "set_shape_param", keys[i], vals[keys[i]]);
	}
	ur->add_undo_method(n, "set_state", before);
	ur->commit_action();
	shape_panel_values = Dictionary(); // (the panel shows the new values)
	update_overlays();
}
