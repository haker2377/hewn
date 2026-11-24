// Hewn — a mesh's state in an undo step, compressed: a dense mesh's state is megabytes (its corners' colors, UVs...),
// mostly the same values, and an undo step keeps two of them. zstd makes it some twenty times smaller in a few ms.
// Other objects (curves, profiles): as they were.
#pragma once

#include "hewn_mesh.h"

#include <godot_cpp/classes/editor_undo_redo_manager.hpp>

namespace godot {

inline void hw_undo_state(EditorUndoRedoManager *p_ur, bool p_do, HewnMesh3D *p_mesh, const Dictionary &p_state) {
	const PackedByteArray b = HewnMesh3D::pack_state(p_state);
	if (p_do) {
		p_ur->add_do_method(p_mesh, "set_state_packed", b);
	} else {
		p_ur->add_undo_method(p_mesh, "set_state_packed", b);
	}
}

inline void hw_undo_state(EditorUndoRedoManager *p_ur, bool p_do, HewnMesh3D *p_mesh, const Variant &p_state) {
	hw_undo_state(p_ur, p_do, p_mesh, Dictionary(p_state));
}

// A do step whose state can still change after it is in the history (the Last Operation panel): its holder, to
// update with hw_update_live (a Dictionary: shared by reference with the step).
inline Dictionary hw_undo_state_live(EditorUndoRedoManager *p_ur, HewnMesh3D *p_mesh, const Dictionary &p_state) {
	Dictionary holder;
	holder["p"] = HewnMesh3D::pack_state(p_state);
	p_ur->add_do_method(p_mesh, "set_state_holder", holder);
	return holder;
}

inline void hw_update_live(Dictionary &p_holder, const Dictionary &p_state) {
	if (p_holder.has("p")) {
		p_holder["p"] = HewnMesh3D::pack_state(p_state);
	}
}

template <class T>
inline void hw_undo_state(EditorUndoRedoManager *p_ur, bool p_do, T *p_obj, const Variant &p_state) {
	if (p_do) {
		p_ur->add_do_method(p_obj, "set_state", p_state);
	} else {
		p_ur->add_undo_method(p_obj, "set_state", p_state);
	}
}

} // namespace godot
