// Floor plan (tsitski's tools): walls all around a floor with one button.
#include "hw_editor.h"
#include "hw_textures.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>

using namespace godot;

// The selected floors (the selected faces in edit mode, else the faces facing up of the selected meshes):
// a closed wall along each outline, standing on the side of the floor (the Wall's side in the shape panel).
void HewnEditorPlugin::walls_around_floor() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return;
	}
	std::vector<HewnMesh3D *> meshes;
	PackedInt32Array picked;
	HewnMesh3D *edited = get_edited();
	if (edited && mode == MODE_FACE && !edited->selection.faces.empty()) {
		meshes.push_back(edited);
		for (int f : edited->selection.faces) {
			picked.push_back(f);
		}
	} else {
		const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
		for (int64_t i = 0; i < sel.size(); i++) {
			if (HewnMesh3D *n = Object::cast_to<HewnMesh3D>(sel[i])) {
				meshes.push_back(n);
			}
		}
		if (meshes.empty() && edited) {
			meshes.push_back(edited);
		}
	}
	if (meshes.empty()) {
		ERR_PRINT("Hewn: select a floor (a mesh, or its faces in face mode) first.");
		return;
	}
	std::vector<HewnMesh3D *> walls;
	for (HewnMesh3D *m : meshes) {
		const hewn::Mesh &d = m->get_data();
		const Transform3D g = m->get_global_transform();
		std::vector<int> faces;
		if (!picked.is_empty()) {
			for (int64_t i = 0; i < picked.size(); i++) {
				faces.push_back(picked[i]);
			}
		} else {
			for (int f = 0; f < (int)d.faces.size(); f++) {
				if (g.basis.xform(hewn::face_normal(d, d.faces[f])).normalized().y > 0.9f) {
					faces.push_back(f);
				}
			}
		}
		if (faces.empty()) {
			continue;
		}
		const hewn::Mesh floor = hewn::ops::extract_faces(d, faces);
		for (const std::vector<int> &loop : hewn::ops::holes(floor)) {
			if (loop.size() < 3) {
				continue;
			}
			std::vector<Vector3> pts;
			Vector3 c;
			for (int v : loop) {
				pts.push_back(g.xform(floor.pos[v]));
				c += pts.back();
			}
			c /= (float)pts.size();
			float y = pts[0].y;
			for (const Vector3 &p : pts) {
				y = MIN(y, p.y);
			}
			HewnMesh3D *w = memnew(HewnMesh3D);
			w->set_name("Wall");
			w->set_shape(hewn::shapes::WALL);
			w->set_shape_param("side", (float)wall_side);
			if (texture_dock && texture_dock->current_material.is_valid()) {
				w->apply_material(texture_dock->current_material, PackedInt32Array());
			}
			PackedVector3Array local;
			for (const Vector3 &p : pts) {
				local.push_back(Vector3(p.x - c.x, 0.0f, p.z - c.z));
			}
			local.push_back(local[0]); // (back to the start: a closed ring)
			w->set_poly_points(local);
			w->set_transform(Transform3D(Basis(), Vector3(c.x, y, c.z)));
			walls.push_back(w);
		}
	}
	if (walls.empty()) {
		ERR_PRINT("Hewn: no floor faces (facing up) in the selection.");
		return;
	}
	Node *parent = root;
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Walls Around Floor");
	for (HewnMesh3D *w : walls) {
		ur->add_do_method(parent, "add_child", w, true);
		ur->add_do_method(w, "set_owner", root);
		ur->add_do_reference(w);
		ur->add_undo_method(parent, "remove_child", w);
	}
	ur->commit_action();
	EditorSelection *es = EditorInterface::get_singleton()->get_selection();
	es->clear();
	for (HewnMesh3D *w : walls) {
		es->add_node(w);
	}
}
