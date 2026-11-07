// "Adjust Last Operation" panel: bottom left of the viewport, shows the parameters
// of the operation that has just been applied; changing one undoes it and runs it again.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/style_box.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/undo_redo.hpp>

using namespace godot;

namespace {

enum FieldType {
	F_FLOAT,
	F_INT,
	F_BOOL,
	F_HIDDEN, // stored but not shown (e.g. the edge a loop cut started from)
	F_OPTION, // a choice: the items in `options` ("A,B,C"), the value is the index
	F_BUTTON, // a button running the operation `key` (e.g. the profile window)
	F_COLOR, // a color (the default: white)
};

struct Field {
	const char *key;
	const char *label;
	FieldType type;
	double def;
	double min;
	double max;
	double step;
	const char *options = nullptr;
	const char *show_if = nullptr; // shown only when this field is on ("prop") or has this value ("profile_type=1")
};

const std::vector<Field> &fields_for(const String &p_op) {
	static const std::vector<Field> none;
	static const std::vector<Field> extrude = { { "distance", "Distance", F_FLOAT, 0.5, -1e6, 1e6, 0.001 },
		{ "individual", "Individual faces", F_BOOL, 0, 0, 1, 1 }, { "dissolve_orthogonal", "Dissolve orthogonal edges", F_BOOL, 0, 0, 1, 1 },
		{ "flip_normals", "Flip normals", F_BOOL, 0, 0, 1, 1 } };
	static const std::vector<Field> bisect = { { "clear_inner", "Clear inner", F_BOOL, 0, 0, 1, 1 }, { "clear_outer", "Clear outer", F_BOOL, 0, 0, 1, 1 },
		{ "fill", "Fill", F_BOOL, 0, 0, 1, 1 }, { "px", "", F_HIDDEN, 0, 0, 0, 0 }, { "py", "", F_HIDDEN, 0, 0, 0, 0 }, { "pz", "", F_HIDDEN, 0, 0, 0, 0 },
		{ "nx", "", F_HIDDEN, 0, 0, 0, 0 }, { "ny", "", F_HIDDEN, 1, 0, 0, 0 }, { "nz", "", F_HIDDEN, 0, 0, 0, 0 } };
	static const std::vector<Field> width_value = { { "value", "Width", F_FLOAT, 0.1, 0, 1e6, 0.001 }, { "px", "", F_HIDDEN, 0, 0, 0, 0 },
		{ "py", "", F_HIDDEN, 0, 0, 0, 0 }, { "pz", "", F_HIDDEN, 0, 0, 0, 0 } };
	static const std::vector<Field> distance_value = { { "value", "Distance", F_FLOAT, 0, -1e6, 1e6, 0.001 }, { "px", "", F_HIDDEN, 0, 0, 0, 0 },
		{ "py", "", F_HIDDEN, 0, 0, 0, 0 }, { "pz", "", F_HIDDEN, 0, 0, 0, 0 } };
	static const std::vector<Field> shear = { { "value", "Offset", F_FLOAT, 0, -1e6, 1e6, 0.001 }, { "px", "", F_HIDDEN, 0, 0, 0, 0 },
		{ "py", "", F_HIDDEN, 0, 0, 0, 0 }, { "pz", "", F_HIDDEN, 0, 0, 0, 0 }, { "ax", "", F_HIDDEN, 1, 0, 0, 0 }, { "ay", "", F_HIDDEN, 0, 0, 0, 0 },
		{ "az", "", F_HIDDEN, 0, 0, 0, 0 }, { "bx", "", F_HIDDEN, 0, 0, 0, 0 }, { "by", "", F_HIDDEN, 1, 0, 0, 0 }, { "bz", "", F_HIDDEN, 0, 0, 0, 0 } };
	static const std::vector<Field> factor01 = { { "value", "Factor", F_FLOAT, 1, 0, 1, 0.01 } };
	static const std::vector<Field> sharp_angle = { { "angle", "Angle (deg)", F_FLOAT, 30, 1, 180, 0.5 }, { "extend", "Extend", F_BOOL, 0, 0, 1, 1 } };
	static const std::vector<Field> subsurf = { { "kind", "Type", F_OPTION, 0, 0, 1, 1, "Catmull-Clark,Simple" }, { "levels", "Levels", F_INT, 1, 0, 6, 1 },
		{ "creases", "Use Creases", F_BOOL, 1, 0, 1, 1 }, { "boundary", "Boundary Smooth", F_OPTION, 0, 0, 1, 1, "All,Keep Corners" },
		{ "uv_smooth", "UV Smooth", F_OPTION, 1, 0, 1, 1, "None,Keep Boundaries" } };
	static const std::vector<Field> mark_verts = { { "vertices", "Vertices", F_BOOL, 0, 0, 1, 1 } };
	static const std::vector<Field> randomize = { { "amount", "Amount", F_FLOAT, 0.1, 0, 1e6, 0.001 }, { "seed", "Random seed", F_INT, 0, 0, 10000, 1 } };
	static const std::vector<Field> linked = { { "deselect", "Deselect", F_BOOL, 0, 0, 1, 1 }, { "normal", "Delimit: Normal", F_BOOL, 0, 0, 1, 1 },
		{ "material", "Delimit: Material", F_BOOL, 0, 0, 1, 1 }, { "seam", "Delimit: Seam", F_BOOL, 1, 0, 1, 1 }, { "sharp", "Delimit: Sharp", F_BOOL, 0, 0, 1, 1 },
		{ "uv", "Delimit: UV", F_BOOL, 0, 0, 1, 1 } };
	static const std::vector<Field> extrude_normals = { { "distance", "Distance", F_FLOAT, 0.2, -1e6, 1e6, 0.001 } };
	static const std::vector<Field> extrude_move = { { "x", "Move X", F_FLOAT, 0, -1e6, 1e6, 0.001 }, { "y", "Move Y", F_FLOAT, 0, -1e6, 1e6, 0.001 },
		{ "z", "Move Z", F_FLOAT, 0, -1e6, 1e6, 0.001 }, { "dissolve_orthogonal", "Dissolve orthogonal edges", F_BOOL, 0, 0, 1, 1 },
		{ "flip_normals", "Flip normals", F_BOOL, 0, 0, 1, 1 } };
#define HW_PROP_FIELDS { "prop", "Proportional editing", F_BOOL, 0, 0, 1, 1 }, { "prop_size", "Radius of influence", F_FLOAT, 1, 0.001, 1e6, 0.01, nullptr, "prop" }, \
		{ "prop_falloff", "Falloff", F_OPTION, 0, 0, 7, 1, "Smooth,Sphere,Root,Inverse Square,Sharp,Linear,Constant,Random", "prop" }, \
		{ "prop_connected", "Connected", F_BOOL, 0, 0, 1, 1, nullptr, "prop" }, { "prop_projected", "Projected (2D)", F_BOOL, 0, 0, 1, 1, nullptr, "prop" }, \
		{ "prop_vx", "", F_HIDDEN, 0, 0, 0, 0 }, { "prop_vy", "", F_HIDDEN, 0, 0, 0, 0 }, { "prop_vz", "", F_HIDDEN, 0, 0, 0, 0 }, \
		{ "pivot_mode", "", F_HIDDEN, 3, 0, 0, 0 }
	static const std::vector<Field> translate = { { "x", "Move X", F_FLOAT, 0, -1e6, 1e6, 0.001 }, { "y", "Move Y", F_FLOAT, 0, -1e6, 1e6, 0.001 },
		{ "z", "Move Z", F_FLOAT, 0, -1e6, 1e6, 0.001 }, HW_PROP_FIELDS };
	static const std::vector<Field> moved = { { "x", "Move X", F_FLOAT, 0, -1e6, 1e6, 0.001 }, { "y", "Move Y", F_FLOAT, 0, -1e6, 1e6, 0.001 },
		{ "z", "Move Z", F_FLOAT, 0, -1e6, 1e6, 0.001 } };
	static const std::vector<Field> rotate = { { "angle", "Angle (deg)", F_FLOAT, 0, -1e6, 1e6, 0.5 }, { "ax", "Axis X", F_FLOAT, 0, -1, 1, 0.001 },
		{ "ay", "Axis Y", F_FLOAT, 1, -1, 1, 0.001 }, { "az", "Axis Z", F_FLOAT, 0, -1, 1, 0.001 }, { "px", "", F_HIDDEN, 0, 0, 0, 0 },
		{ "py", "", F_HIDDEN, 0, 0, 0, 0 }, { "pz", "", F_HIDDEN, 0, 0, 0, 0 }, HW_PROP_FIELDS };
	static const std::vector<Field> scale = { { "factor", "Scale", F_FLOAT, 1, -1e6, 1e6, 0.001 }, { "dx", "", F_HIDDEN, 0, 0, 0, 0 },
		{ "dy", "", F_HIDDEN, 0, 0, 0, 0 }, { "dz", "", F_HIDDEN, 0, 0, 0, 0 }, { "constraint", "", F_HIDDEN, 0, 0, 0, 0 },
		{ "px", "", F_HIDDEN, 0, 0, 0, 0 }, { "py", "", F_HIDDEN, 0, 0, 0, 0 }, { "pz", "", F_HIDDEN, 0, 0, 0, 0 }, HW_PROP_FIELDS };
	static const std::vector<Field> inset = { { "amount", "Thickness", F_FLOAT, 0.1, 0, 1e6, 0.001 }, { "depth", "Depth", F_FLOAT, 0, -1e6, 1e6, 0.001 },
		{ "boundary", "Boundary", F_BOOL, 1, 0, 1, 1 }, { "even", "Offset Even", F_BOOL, 1, 0, 1, 1 }, { "relative", "Offset Relative", F_BOOL, 0, 0, 1, 1 },
		{ "edge_rail", "Edge Rail", F_BOOL, 0, 0, 1, 1 }, { "outset", "Outset", F_BOOL, 0, 0, 1, 1 }, { "select_outer", "Select Outer", F_BOOL, 0, 0, 1, 1 },
		{ "individual", "Individual", F_BOOL, 0, 0, 1, 1 }, { "interpolate", "Interpolate", F_BOOL, 1, 0, 1, 1 } };
	static const std::vector<Field> bevel = { { "affect", "Affect", F_OPTION, 1, 0, 1, 1, "Vertices,Edges" },
		{ "width_type", "Width Type", F_OPTION, 0, 0, 3, 1, "Offset,Width,Depth,Percent" }, { "width", "Width", F_FLOAT, 0.1, 0, 1e6, 0.001 },
		{ "segments", "Segments", F_INT, 1, 1, 64, 1, nullptr, "affect=1" }, { "profile_type", "Profile Type", F_OPTION, 0, 0, 1, 1, "Superellipse,Custom", "affect=1" },
		{ "bevel_profile", "Edit Profile...", F_BUTTON, 0, 0, 0, 0, nullptr, "profile_type=1" },
		{ "harden_normals", "Harden Normals", F_BOOL, 0, 0, 1, 1, nullptr, "affect=1" } };
	static const std::vector<Field> arch = { { "segments", "Segments", F_INT, 8, 2, 128, 1 }, { "rise", "Rise (1 = half circle)", F_FLOAT, 1, -10, 10, 0.01 } };
	static const std::vector<Field> loop_cut = { { "cuts", "Number of cuts", F_INT, 1, 1, 64, 1 }, { "factor", "Factor", F_FLOAT, 0, -1, 1, 0.001 },
		{ "a", "", F_HIDDEN, 0, 0, 0, 0 }, { "b", "", F_HIDDEN, 0, 0, 0, 0 } };
	static const std::vector<Field> subdivide_edges = { { "cuts", "Number of cuts", F_INT, 1, 1, 64, 1 } };
	static const std::vector<Field> weld = { { "distance", "Merge distance", F_FLOAT, 0.01, 0, 1e6, 0.0001 } };
	static const std::vector<Field> angle = { { "angle", "Max angle (deg)", F_FLOAT, 5, 0, 180, 0.5 } };
	static const std::vector<Field> smooth = { { "angle", "Angle (deg)", F_FLOAT, 30, 0, 180, 0.5 } };
	if (p_op == "extrude") {
		return extrude;
	}
	if (p_op == "extrude_move") {
		return extrude_move;
	}
	if (p_op == "bisect") {
		return bisect;
	}
	if (p_op == "cut") {
		static const std::vector<Field> cut = { { "non_destructive", "Non-destructive (a cutter node)", F_BOOL, 0, 0, 1, 1 },
			{ "keep_flush", "Keep faces on its sides", F_BOOL, 1, 0, 1, 1 },
			{ "lining", "Corridor / alcove (W)", F_BOOL, 0, 0, 1, 1 } };
		return cut;
	}
	if (p_op == "clip") {
		static const std::vector<Field> clip = { { "mode", "Mode", F_OPTION, 1, 0, 2, 1, "Bisect Only,Remove Front,Remove Back" } };
		return clip;
	}
	if (p_op == "bevel_vertices" || p_op == "offset_edge_slide") {
		return width_value;
	}
	if (p_op == "push_pull" || p_op == "shrink_fatten") {
		return distance_value;
	}
	if (p_op == "shear") {
		return shear;
	}
	if (p_op == "edge_crease" || p_op == "bevel_weight" || p_op == "vertex_crease") {
		return factor01;
	}
	if (p_op == "sharp_by_angle") {
		return sharp_angle;
	}
	if (p_op == "catmull_clark") {
		return subsurf;
	}
	if (p_op == "mark_sharp" || p_op == "clear_sharp" || p_op == "mark_seam" || p_op == "clear_seam") {
		return mark_verts;
	}
	if (p_op == "randomize") {
		return randomize;
	}
	if (p_op == "spin") {
		static const std::vector<Field> spin = { { "steps", "Steps", F_INT, 8, 1, 256, 1 }, { "angle", "Angle (deg)", F_FLOAT, 90, -3600, 3600, 0.5 },
			{ "cx", "Center X", F_FLOAT, 0, -1e6, 1e6, 0.01 }, { "cy", "Center Y", F_FLOAT, 0, -1e6, 1e6, 0.01 }, { "cz", "Center Z", F_FLOAT, 0, -1e6, 1e6, 0.01 },
			{ "ax", "Axis X", F_FLOAT, 0, -1, 1, 0.01 }, { "ay", "Axis Y", F_FLOAT, 1, -1, 1, 0.01 }, { "az", "Axis Z", F_FLOAT, 0, -1, 1, 0.01 } };
		return spin;
	}
	if (p_op == "recalc_normals") {
		static const std::vector<Field> recalc = { { "inside", "Inside", F_BOOL, 0, 0, 1, 1 } };
		return recalc;
	}
	if (p_op == "to_circle") {
		static const std::vector<Field> circle = { { "regular", "Regular", F_BOOL, 1, 0, 1, 1 }, { "influence", "Influence (%)", F_FLOAT, 100, 0, 100, 1 } };
		return circle;
	}
	if (p_op == "select_linked") {
		return linked;
	}
	if (p_op == "checker_deselect") {
		static const std::vector<Field> checker = { { "nth", "Deselected", F_INT, 1, 1, 1000, 1 }, { "skip", "Selected", F_INT, 1, 1, 1000, 1 },
			{ "offset", "Offset", F_INT, 0, 0, 1000, 1 } };
		return checker;
	}
	if (p_op == "extrude_normals") {
		return extrude_normals;
	}
	if (p_op == "translate") {
		return translate;
	}
	if (p_op == "duplicate_move") {
		return moved;
	}
	if (p_op == "rotate") {
		return rotate;
	}
	if (p_op == "scale") {
		return scale;
	}
	if (p_op == "inset") {
		return inset;
	}
	if (p_op == "bevel") {
		return bevel;
	}
	if (p_op == "arch") {
		return arch;
	}
	if (p_op == "loop_cut") {
		return loop_cut;
	}
	if (p_op == "subdivide_edges") {
		return subdivide_edges;
	}
	if (p_op == "subdivide_faces") {
		// Subdivide panel
		static const std::vector<Field> subdivide_faces = { { "cuts", "Number of Cuts", F_INT, 1, 1, 100, 1 }, { "smoothness", "Smoothness", F_FLOAT, 0, 0, 1000, 0.01 },
			{ "ngons", "Create N-Gons", F_BOOL, 1, 0, 1, 1 }, { "fractal", "Fractal", F_FLOAT, 0, 0, 1e6, 0.01 }, { "along_normal", "Along Normal", F_FLOAT, 0, 0, 1, 0.01 },
			{ "seed", "Random Seed", F_INT, 0, 0, 1e6, 1 } };
		return subdivide_faces;
	}
	if (p_op == "weld") {
		return weld;
	}
	if (p_op == "limited_dissolve") {
		return angle;
	}
	if (p_op == "set_vertex_color") {
		static const std::vector<Field> vcol = { { "color", "Color", F_COLOR, 1, 0, 1, 0 },
			{ "channel", "Channel", F_OPTION, 0, 0, 4, 1, "Color,CUSTOM0,CUSTOM1,CUSTOM2,CUSTOM3" },
			{ "faces_only", "Only on the Selected Faces", F_BOOL, 1, 0, 1, 1 } };
		return vcol;
	}
	if (p_op == "auto_smooth" || p_op == "quadify") {
		return smooth;
	}
	return none;
}

String title_for(const String &p_op) {
	if (p_op == "extrude_move") {
		return "Extrude";
	}
	if (p_op == "duplicate_move") {
		return "Duplicate";
	}
	if (p_op == "translate") {
		return "Move";
	}
	if (p_op == "recalc_normals") {
		return "Recalculate Normals";
	}
	if (p_op == "set_vertex_color") {
		return "Set Vertex Color";
	}
	return p_op.capitalize();
}

} // namespace

bool HewnEditorPlugin::is_param_op(const String &p_op) {
	return !fields_for(p_op).empty();
}

Variant HewnEditorPlugin::op_param(const String &p_op, const String &p_key) {
	if (!op_params.has(p_op)) {
		op_params[p_op] = Dictionary();
	}
	Dictionary d = op_params[p_op];
	if (!d.has(p_key)) {
		for (const Field &f : fields_for(p_op)) {
			if (p_key == f.key) {
				d[p_key] = f.type == F_INT ? Variant((int64_t)f.def) : f.type == F_BOOL ? Variant(f.def != 0.0) : f.type == F_COLOR ? Variant(Color(1, 1, 1, 1)) : Variant(f.def);
			}
		}
	}
	return d.get(p_key, Variant());
}

void HewnEditorPlugin::set_op_param(const String &p_op, const String &p_key, const Variant &p_value) {
	if (!op_params.has(p_op)) {
		op_params[p_op] = Dictionary();
	}
	Dictionary d = op_params[p_op];
	d[p_key] = p_value;
}

void HewnEditorPlugin::_record_param_op(const String &p_op, Mode p_mode) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	UndoRedo *h = ur->get_history_undo_redo(ur->get_object_history_id(n));
	last_op = p_op;
	last_op_mode = p_mode;
	last_op_node = n->get_instance_id();
	last_op_version = h ? (int64_t)h->get_version() : -1;
	if (!redo_running) {
		last_op_before = last_commit_before;
		last_op_after = last_commit_after;
		_show_redo();
	}
}

void HewnEditorPlugin::_hide_redo() {
	last_op = String();
	if (redo_panel) {
		redo_panel->hide();
	}
}

void HewnEditorPlugin::_show_redo() {
	if (!redo_panel_on) {
		// (turned off in Editor Settings: the operation is done with the values it has, nothing pops up)
		if (redo_panel) {
			redo_panel->hide();
		}
		return;
	}
	if (!redo_panel) {
		redo_panel = memnew(PanelContainer);
		redo_panel->set_name("HewnLastOperation");
		// The editor's popup style, so it follows the user's interface theme.
		Ref<StyleBox> sb = EditorInterface::get_singleton()->get_editor_theme()->get_stylebox("panel", "PopupPanel");
		if (sb.is_valid()) {
			redo_panel->add_theme_stylebox_override("panel", sb);
		}
		VBoxContainer *v = memnew(VBoxContainer);
		redo_panel->add_child(v);
		redo_header = memnew(Button);
		redo_header->set_name("Header");
		redo_header->connect("pressed", callable_mp(this, &HewnEditorPlugin::_toggle_redo));
		v->add_child(redo_header);
		_style_panel_header(redo_header, redo_panel);
		redo_body = memnew(GridContainer);
		redo_body->set_columns(2);
		v->add_child(redo_body);
		_compact_panel(redo_panel, redo_body);
	}
	// Bottom left of the viewport the operation was done in.
	Control *surface = last_surface ? Object::cast_to<Control>(ObjectDB::get_instance(last_surface)) : nullptr;
	if (!surface) {
		return;
	}
	if (redo_panel->get_parent() != surface) {
		if (redo_panel->get_parent()) {
			redo_panel->get_parent()->remove_child(redo_panel);
		}
		surface->add_child(redo_panel);
	}
	for (int i = redo_body->get_child_count() - 1; i >= 0; i--) {
		Node *c = redo_body->get_child(i);
		redo_body->remove_child(c);
		c->queue_free();
	}
	redo_header->set_text(hw_editor::T(title_for(last_op)));
	_style_panel_header(redo_header, nullptr);
	for (const Field &f : fields_for(last_op)) {
		if (f.type == F_HIDDEN) {
			continue;
		}
		if (f.show_if) {
			const PackedStringArray c = String(f.show_if).split("=");
			const Variant cv = op_param(last_op, c[0]);
			bool shown = c.size() > 1 ? (int)cv == c[1].to_int() : (bool)cv;
			if (c.size() > 1 && c[0] == "profile_type") {
				shown = shown && (int)op_param(last_op, "affect") == 1;
			}
			if (!shown) {
				continue;
			}
		}
		if (f.type == F_BUTTON) {
			redo_body->add_child(memnew(Control));
			Button *b = memnew(Button);
			b->set_text(hw_editor::T(f.label));
			b->connect("pressed", callable_mp(this, &HewnEditorPlugin::run_op).bind(String(f.key)));
			redo_body->add_child(b);
			continue;
		}
		Label *l = memnew(Label);
		_compact_label(l, hw_editor::T(f.label));
		redo_body->add_child(l);
		const Variant v = op_param(last_op, f.key);
		if (f.type == F_OPTION) {
			OptionButton *o = memnew(OptionButton);
			for (const String &it : String(f.options).split(",")) {
				o->add_item(it);
			}
			o->select(CLAMP((int)v, 0, o->get_item_count() - 1));
			o->connect("item_selected", callable_mp(this, &HewnEditorPlugin::_on_redo_option).bind(String(f.key)));
			redo_body->add_child(hw_editor::with_revert(o, (int)f.def));
		} else if (f.type == F_BOOL) {
			CheckBox *c = memnew(CheckBox);
			c->set_pressed((bool)v);
			c->connect("toggled", callable_mp(this, &HewnEditorPlugin::_on_redo_bool).bind(String(f.key)));
			redo_body->add_child(hw_editor::with_revert(c, f.def != 0.0));
		} else if (f.type == F_COLOR) {
			ColorPickerButton *c = memnew(ColorPickerButton);
			c->set_name(String("Field_") + f.key);
			c->set_pick_color(v.get_type() == Variant::COLOR ? Color(v) : Color(1, 1, 1, 1));
			c->set_custom_minimum_size(Vector2(60, 0) * EditorInterface::get_singleton()->get_editor_scale());
			c->connect("color_changed", callable_mp(this, &HewnEditorPlugin::_on_redo_color).bind(String(f.key)));
			redo_body->add_child(hw_editor::with_revert(c, Color(1, 1, 1, 1)));
		} else {
			HewnDragField *s = memnew(HewnDragField);
			s->setup(f.min, f.max, f.step, f.type == F_INT, (double)v);
			s->connect("value_changed", callable_mp(this, &HewnEditorPlugin::_on_redo_value).bind(String(f.key)));
			redo_body->add_child(hw_editor::with_revert(s, f.def));
		}
	}
	redo_body->set_visible(!redo_collapsed);
	hw_editor::localize(redo_body);
	redo_panel->reset_size();
	redo_panel->set_position(_left_panel_pos(surface, redo_panel));
	redo_panel->show();
}

void HewnEditorPlugin::_toggle_redo() {
	_set_panels_collapsed(!redo_collapsed);
}

// One folded / unfolded state for both bottom-left panels,
// remembered in the project's editor metadata.
void HewnEditorPlugin::_set_panels_collapsed(bool p_collapsed) {
	redo_collapsed = p_collapsed;
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid() && OS::get_singleton()->get_environment("HW_SELFTEST").is_empty()) {
		es->set_project_metadata("hewn", "panel_collapsed", p_collapsed);
	}
	if (redo_panel && redo_panel->is_visible() && !last_op.is_empty()) {
		_show_redo();
	}
	if (shape_panel && shape_panel->is_visible()) {
		shape_panel_type = -1; // rebuilt next frame
		_update_shape_panel();
	}
	if (obj_panel && obj_panel->is_visible()) {
		obj_panel_node = 0; // (rebuilt now, folded or not)
		_update_obj_panel();
	}
}

// The header: a flat button with fold arrow; with p_panel, clicks on the panel's margin
// left of / around the header fold it too.
void HewnEditorPlugin::_style_panel_header(Button *p_header, Control *p_panel) {
	Ref<Theme> et = EditorInterface::get_singleton()->get_editor_theme();
	p_header->set_flat(true);
	p_header->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	p_header->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	p_header->set_focus_mode(Control::FOCUS_NONE);
	p_header->set_tooltip_text(hw_editor::T("Fold / unfold this panel"));
	p_header->set_button_icon(et->get_icon(redo_collapsed ? "arrow_collapsed" : "arrow", "Tree"));
	if (p_panel) {
		p_panel->connect("gui_input", callable_mp(this, &HewnEditorPlugin::_on_panel_header_input).bind(p_panel));
	}
}

void HewnEditorPlugin::_on_panel_header_input(const Ref<InputEvent> &p_event, Control *p_panel) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_null() || !mb->is_pressed() || mb->get_button_index() != MOUSE_BUTTON_LEFT) {
		return;
	}
	Button *header = Object::cast_to<Button>(p_panel->find_child(p_panel == redo_panel ? "Header" : "Title", true, false));
	if (!header) {
		return;
	}
	// the strip of the header row, from the panel's left edge
	const float top = header->get_global_position().y - p_panel->get_global_position().y;
	if (mb->get_position().y <= top + header->get_size().y) {
		p_panel->accept_event();
		_set_panels_collapsed(!redo_collapsed);
	}
}

void HewnEditorPlugin::_on_redo_color(const Color &p_value, const String &p_key) {
	set_op_param(last_op, p_key, p_value);
	_redo_last();
}

void HewnEditorPlugin::_on_redo_value(double p_value, const String &p_key) {
	set_op_param(last_op, p_key, p_value);
	_redo_last();
}

namespace {
// Whether other fields of the operation show or hide by this one's value.
bool shows_other_fields(const String &p_op, const String &p_key) {
	for (const Field &f : fields_for(p_op)) {
		if (f.show_if && String(f.show_if).get_slice("=", 0) == p_key) {
			return true;
		}
	}
	return p_key == "affect"; // (the profile's button: by the profile type and the affect)
}
} // namespace

void HewnEditorPlugin::_on_redo_bool(bool p_value, const String &p_key) {
	set_op_param(last_op, p_key, p_value);
	_redo_last();
	if (shows_other_fields(last_op, p_key)) {
		callable_mp(this, &HewnEditorPlugin::_refresh_redo_fields).call_deferred(); // (fields shown only with some values)
	}
}

void HewnEditorPlugin::_on_redo_option(int p_index, const String &p_key) {
	set_op_param(last_op, p_key, p_index);
	_redo_last();
	if (shows_other_fields(last_op, p_key)) {
		callable_mp(this, &HewnEditorPlugin::_refresh_redo_fields).call_deferred();
	}
}

void HewnEditorPlugin::_refresh_redo_fields() {
	if (redo_panel && redo_panel->is_visible() && !last_op.is_empty()) {
		_show_redo();
	}
}

void HewnEditorPlugin::_redo_last() {
	if (last_op == "cut") {
		_run_cut(true);
		return;
	}
	if (last_op == "clip") {
		if (_clip_panel_valid()) {
			_clip_run(true);
		} else {
			_hide_redo();
		}
		return;
	}
	HewnMesh3D *n = get_edited();
	if (!n || last_op.is_empty() || n->get_instance_id() != last_op_node) {
		_hide_redo();
		return;
	}
	if (last_op == "select_linked") {
		_apply_select_linked();
		return;
	}
	if (last_op == "checker_deselect") {
		_apply_checker();
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	UndoRedo *h = ur->get_history_undo_redo(ur->get_object_history_id(n));
	if (!h || (int64_t)h->get_version() != last_op_version) {
		_hide_redo(); // something else happened since
		return;
	}
	mode = last_op_mode;
	if (last_op_mode != MODE_OBJECT) {
		element_mode = last_op_mode; // (an object's operation: the element mode stays what it was)
	}
	redo_running = true;
	if (!last_op_before.is_empty() && !last_op_after.is_empty()) {
		n->set_state(last_op_before);
		redo_inplace = true;
		redo_inplace_done = false;
		run_op(last_op);
		redo_inplace = false;
		if (!redo_inplace_done) {
			n->set_state(last_op_after); // (the operation did not go through: as it was)
		}
	} else {
		h->undo();
		run_op(last_op);
	}
	redo_running = false;
	_set_godot_gizmo_hidden(mode != MODE_OBJECT);
	selection_changed();
}

// Hides the panel when the history moved on (Ctrl+Z, another edit...).
void HewnEditorPlugin::_check_redo_valid() {
	if (last_op.is_empty() || !redo_panel || !redo_panel->is_visible()) {
		return;
	}
	if (last_op == "clip") {
		if (!_clip_panel_valid()) {
			_hide_redo();
		}
		return;
	}
	if (last_op == "cut") {
		if (!_cut_panel_valid()) {
			_hide_redo();
		}
		return;
	}
	HewnMesh3D *n = get_edited();
	// (an object's operation - Auto Smooth, Subdivide - keeps its panel in object mode, until another object)
	if (!n || (mode == MODE_OBJECT && last_op_mode != MODE_OBJECT) || n->get_instance_id() != last_op_node) {
		_hide_redo();
		return;
	}
	if (last_op == "select_linked" || last_op == "checker_deselect") {
		return; // selection only: stays until another operation
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	UndoRedo *h = ur->get_history_undo_redo(ur->get_object_history_id(n));
	if (!h || (int64_t)h->get_version() != last_op_version) {
		_hide_redo();
	}
}
