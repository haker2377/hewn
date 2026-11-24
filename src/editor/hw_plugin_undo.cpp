// Hewn editor - an edit as one undo step.

#include "hw_plugin_impl.h"

// ------------------------------------------------------------------ undo

void HewnEditorPlugin::begin_edit(const String &p_name) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	edit_mode_before = mode;
	edit_mask_before = mode_mask;
	edit_before = n->get_state();
	edit_name = p_name;
	editing = true;
	if (uv_lock) {
		n->uv_lock_begin();
	}
}

void HewnEditorPlugin::restore_modes(int p_mode, int p_mask) {
	if (mode == MODE_OBJECT || !get_edited() || p_mode <= MODE_OBJECT || p_mode > MODE_FACE) {
		return;
	}
	mode = element_mode = (Mode)p_mode;
	mode_mask = (p_mask & 0xE) ? (p_mask & 0xE) : (1 << p_mode);
	if (dock) {
		dock->refresh();
	}
	update_overlays();
	if (HewnMesh3D *n = get_edited()) {
		n->update_gizmos();
	}
}

void HewnEditorPlugin::cancel_edit() {
	HewnMesh3D *n = get_edited();
	if (n) {
		n->uv_lock_end();
	}
	if (editing && n && !edit_before.is_empty()) {
		n->set_state(edit_before);
	}
	editing = false;
	edit_before = Dictionary();
	selection_changed();
}

void HewnEditorPlugin::end_edit() {
	HewnMesh3D *n = get_edited();
	if (!editing || !n) {
		editing = false;
		return;
	}
	editing = false;
	n->uv_lock_end();
	for (HewnMesh3D *em : edited_meshes()) {
		em->finish_positions(); // (moved with only the positions shown: made whole again)
	}
	{
		// (a safety net: an operation that took faces away leaves no corners nothing uses any more)
		hewn::Mesh &m = n->get_data();
		std::vector<char> used(m.pos.size(), 0);
		for (const hewn::Face &F : m.faces) {
			for (int v : F.v) {
				if (v >= 0 && v < (int)used.size()) {
					used[v] = 1;
				}
			}
		}
		for (const std::pair<int, int> &w : m.wires) {
			for (int v : { w.first, w.second }) {
				if (v >= 0 && v < (int)used.size()) {
					used[v] = 1;
				}
			}
		}
		for (int v : m.points) {
			if (v >= 0 && v < (int)used.size()) {
				used[v] = 1;
			}
		}
		if (std::find(used.begin(), used.end(), 0) != used.end()) {
			hewn::remove_unused_vertices(m, &n->selection);
			n->commit(true);
		}
	}
	sanitize(n->get_data(), n->selection);
	Dictionary after = n->get_state();
	if (redo_inplace && !redo_inplace_done) {
		// the Last Operation panel: the step already in the history now leads here
		last_op_after.clear();
		last_op_after.merge(after);
		hw_update_live(last_op_after_holder, last_op_after); // (the step's own copy too)
		redo_inplace_done = true;
		edit_before = Dictionary();
		selection_changed();
		return;
	}
	if (field_drag_pending && (field_drag_node != n->get_instance_id() || field_drag_name != edit_name || !_field_dragging())) {
		_flush_field_drag();
	}
	if (multi_before.empty() && !field_drag_pending && edit_name.begins_with("Element Inspector") && after == edit_before) {
		// (a field giving back the value it has, as when a drag ends: no step in the history)
		modal_owner.clear();
		edit_before = Dictionary();
		return;
	}
	if (multi_before.empty() && _field_dragging()) {
		// (a step of a drag: the history gets the whole drag when it ends)
		if (!field_drag_pending) {
			field_drag_pending = true;
			field_drag_before = edit_before;
			field_drag_name = edit_name;
			field_drag_node = n->get_instance_id();
		}
		modal_owner.clear();
		edit_before = Dictionary();
		selection_changed();
		return;
	}
	last_commit_before = edit_before;
	last_commit_after = after;
	EditorUndoRedoManager *ur = get_undo_redo();
	const bool merge_ends = merge_ends_next;
	merge_ends_next = false;
	ur->create_action("Hewn: " + edit_name, multi_merge ? UndoRedo::MERGE_ALL : merge_ends ? UndoRedo::MERGE_ENDS : UndoRedo::MERGE_DISABLE, n);
	last_commit_after_holder = hw_undo_state_live(ur, n, after);
	hw_undo_state(ur, false, n, edit_before);
	if (dock && mode != MODE_OBJECT && edit_mode_before != MODE_OBJECT && (mode != edit_mode_before || mode_mask != edit_mask_before)) {
		// (the operation changed the element modes, as a bevel to faces: undo puts them back with the selection, so
		// the same keys do the same again)
		ur->add_do_method(dock, "_restore_modes", (int)mode, mode_mask);
		ur->add_undo_method(dock, "_restore_modes", (int)edit_mode_before, edit_mask_before);
	}
	for (const std::pair<uint64_t, Dictionary> &mb : multi_before) {
		// (the other edited meshes moved along in G / R / S)
		HewnMesh3D *o = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(mb.first)));
		if (o) {
			hw_undo_state(ur, true, o, o->get_state());
			hw_undo_state(ur, false, o, mb.second);
		}
	}
	multi_before.clear();
	modal_owner.clear();
	ur->commit_action(false);
	edit_before = Dictionary();
	selection_changed();
}

// A drag on a field: the left button held over a number field / slider / spin box (or one of ours dragging).
bool HewnEditorPlugin::_field_dragging() const {
	if (HewnDragField::drag_count > 0) {
		return true;
	}
	if (!Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) {
		return false;
	}
	if (field_drag_pending) {
		return true; // (a drag begun on a field goes on until the button is let go, wherever the mouse is)
	}
	Control *base = EditorInterface::get_singleton()->get_base_control();
	Viewport *vp = base ? base->get_viewport() : nullptr;
	for (Node *c = vp ? vp->gui_get_hovered_control() : nullptr; c; c = c->get_parent()) {
		if (Object::cast_to<Range>(c) && !Object::cast_to<ScrollBar>(c)) {
			return true;
		}
		if (Object::cast_to<Viewport>(c)) {
			break;
		}
	}
	return false;
}

// The drag ended: its edits as one undo step.
void HewnEditorPlugin::_flush_field_drag() {
	if (!field_drag_pending) {
		return;
	}
	field_drag_pending = false;
	HewnMesh3D *n = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(field_drag_node)));
	if (!n || field_drag_before.is_empty()) {
		field_drag_before = Dictionary();
		return;
	}
	const Dictionary after = n->get_state();
	last_commit_before = field_drag_before;
	last_commit_after = after;
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: " + field_drag_name, UndoRedo::MERGE_DISABLE, n);
	hw_undo_state(ur, true, n, after);
	hw_undo_state(ur, false, n, field_drag_before);
	ur->commit_action(false);
	field_drag_before = Dictionary();
}
