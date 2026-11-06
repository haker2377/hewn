// Hewn's keys in Godot's own Editor Settings > Shortcuts (section "Hewn"): stored with the
// user's editor settings, not in the project. A key the user gave to an action is handed to the tools as
// that action's built-in key; the built-in key of a changed action is left to Godot.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/shortcut.hpp>

using namespace godot;
using namespace hw_editor;

namespace {

struct Action {
	const char *id;
	const char *name;
	Key key;
	bool shift, ctrl, alt;
};

const Action ACTIONS[] = {
	{ "edit_mode", "Edit Mode", KEY_TAB, false, false, false },
	{ "vertex_mode", "Vertex Select", KEY_1, false, false, false },
	{ "edge_mode", "Edge Select", KEY_2, false, false, false },
	{ "face_mode", "Face Select", KEY_3, false, false, false },
	{ "move", "Move", KEY_G, false, false, false },
	{ "rotate", "Rotate", KEY_R, false, false, false },
	{ "scale", "Scale", KEY_S, false, false, false },
	{ "extrude", "Extrude", KEY_E, false, false, false },
	{ "extrude_menu", "Extrude Menu", KEY_E, false, false, true },
	{ "inset", "Inset", KEY_I, false, false, false },
	{ "bevel", "Bevel", KEY_B, false, true, false },
	{ "bevel_vertices", "Bevel Vertices", KEY_B, true, true, false },
	{ "loop_cut", "Loop Cut", KEY_R, false, true, false },
	{ "offset_edge_slide", "Offset Edge Slide", KEY_R, true, true, false },
	{ "knife", "Knife", KEY_K, false, false, false },
	{ "rip", "Rip", KEY_V, false, false, false },
	{ "slide_vertices", "Slide Vertices", KEY_V, true, false, false },
	{ "make_face", "New Face / Edge", KEY_F, false, false, false },
	{ "connect", "Connect Vertices", KEY_J, false, false, false },
	{ "duplicate", "Duplicate", KEY_D, true, false, false },
	{ "separate", "Separate Menu", KEY_P, false, false, false },
	{ "merge", "Merge Menu", KEY_M, false, false, false },
	{ "delete", "Delete Menu", KEY_X, false, false, false },
	{ "dissolve", "Dissolve", KEY_X, false, true, false },
	{ "select_all", "Select All", KEY_A, false, false, false },
	{ "select_none", "Select None", KEY_A, false, false, true },
	{ "invert", "Invert Selection", KEY_I, false, true, false },
	{ "linked_pick", "Select Linked Under the Mouse", KEY_L, false, false, false },
	{ "linked", "Select Linked", KEY_L, false, true, false },
	{ "grow", "Grow Selection", KEY_EQUAL, false, true, false },
	{ "shrink", "Shrink Selection", KEY_MINUS, false, true, false },
	{ "hide", "Hide Faces", KEY_H, false, false, false },
	{ "hide_others", "Hide Other Faces", KEY_H, true, false, false },
	{ "reveal", "Show Hidden Faces", KEY_H, false, false, true },
	{ "recalc_outside", "Recalculate Normals Outside", KEY_N, true, false, false },
	{ "recalc_inside", "Recalculate Normals Inside", KEY_N, true, true, false },
	{ "normals_menu", "Normals Menu", KEY_N, false, false, true },
	{ "shrink_fatten", "Shrink / Fatten", KEY_S, false, false, true },
	{ "edge_crease", "Edge Crease", KEY_E, true, false, false },
	{ "proportional", "Proportional Editing", KEY_O, false, false, false },
	{ "xray", "X-Ray", KEY_Z, false, false, true },
	{ "snapping", "Snapping On / Off", KEY_TAB, true, false, false },
	{ "cursor_menu", "3D Cursor Menu", KEY_S, true, false, false },
	{ "cursor_tool", "Place the 3D Cursor", KEY_X, true, false, false },
	{ "add_menu", "Add Menu", KEY_A, true, false, false },
	{ "box_builder", "Box Builder", KEY_B, false, false, false },
	{ "box_cut", "Box Cut", KEY_C, false, false, false },
	{ "clip", "Clip", KEY_X, false, false, true },
	{ "uv_transform", "UV Transform", KEY_T, true, false, false },
	{ "ruler", "Ruler", KEY_M, true, false, false },
	{ "context_action", "Context Action / Gizmo Handles", KEY_W, false, false, false },
	{ "grid_smaller", "Snap Step Smaller", KEY_BRACKETLEFT, false, false, false },
	{ "grid_larger", "Snap Step Larger", KEY_BRACKETRIGHT, false, false, false },
	{ "face_grid", "Grid on Faces", KEY_G, false, false, true },
};

String path_of(const Action &a) {
	return String("hewn/") + a.id;
}

Ref<InputEventKey> default_event(const Action &a) {
	Ref<InputEventKey> k;
	k.instantiate();
	k->set_physical_keycode(a.key);
	k->set_shift_pressed(a.shift);
	k->set_ctrl_pressed(a.ctrl);
	k->set_alt_pressed(a.alt);
	return k;
}

bool is_default(const Action &a, const Ref<InputEventKey> &k) {
	return key_code(k) == a.key && k->is_shift_pressed() == a.shift && (k->is_ctrl_pressed() || k->is_meta_pressed()) == a.ctrl && k->is_alt_pressed() == a.alt;
}

// The action's events differ from its built-in key.
bool changed(const Ref<Shortcut> &sc) {
	if (sc.is_null() || !sc->has_meta("original")) {
		return false;
	}
	const Array now = sc->get_events(), orig = sc->get_meta("original");
	if (now.size() != orig.size()) {
		return true;
	}
	for (int64_t i = 0; i < now.size(); i++) {
		Ref<InputEvent> a = now[i], b = orig[i];
		if (a.is_null() || b.is_null() || !a->is_match(b, true)) {
			return true;
		}
	}
	return false;
}

} // namespace

void HewnEditorPlugin::_register_shortcuts() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_null()) {
		return;
	}
	for (const Action &a : ACTIONS) {
		const String path = path_of(a);
		Array events;
		events.push_back(default_event(a));
		Ref<Shortcut> sc = es->has_shortcut(path) ? es->get_shortcut(path) : Ref<Shortcut>();
		if (sc.is_null()) {
			sc.instantiate();
			sc->set_events(events);
			es->add_shortcut(path, sc);
		}
		// (one the user changed comes from the saved settings: it keeps its keys)
		sc->set_name(String("Hewn: ") + T(a.name));
		sc->set_meta("original", events);
	}
}

// A key event in the 3D view: -1 when it goes on as it is, else the result of handling it as the key
// of the action it is bound to.
int32_t HewnEditorPlugin::_remap_key(Camera3D *p_cam, const Ref<InputEventKey> &p_key) {
	if (remapping || modal != MODAL_NONE) {
		return -1; // (inside the tools their own keys stay)
	}
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_null()) {
		return -1;
	}
	for (const Action &a : ACTIONS) {
		Ref<Shortcut> sc = es->get_shortcut(path_of(a));
		if (!changed(sc)) {
			continue;
		}
		if (sc->matches_event(p_key)) {
			Ref<InputEventKey> k = default_event(a);
			k->set_keycode(a.key);
			k->set_pressed(p_key->is_pressed());
			k->set_echo(p_key->is_echo());
			remapping = true;
			const int32_t r = _forward_3d_gui_input(p_cam, k);
			remapping = false;
			return r;
		}
	}
	for (const Action &a : ACTIONS) {
		if (is_default(a, p_key) && changed(es->get_shortcut(path_of(a)))) {
			return AFTER_GUI_INPUT_PASS; // (its action has another key now)
		}
	}
	return -1;
}
