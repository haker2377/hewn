// Bake Navigation Mesh: a plain NavigationMesh of the selected objects (none selected: the whole scene), its settings
// in a window like Godot's own (the resource in an inspector), saved to a file; a NavigationRegion3D with it added to
// the scene if asked.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/confirmation_dialog.hpp>
#include <godot_cpp/classes/editor_file_dialog.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_inspector.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/navigation_mesh.hpp>
#include <godot_cpp/classes/navigation_mesh_source_geometry_data3d.hpp>
#include <godot_cpp/classes/navigation_region3d.hpp>
#include <godot_cpp/classes/navigation_server3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

using namespace godot;
using namespace hw_editor;

void HewnEditorPlugin::open_navmesh_bake() {
	if (!navmesh_dialog) {
		const float es = EditorInterface::get_singleton()->get_editor_scale();
		navmesh_dialog = memnew(ConfirmationDialog);
		navmesh_dialog->set_name("HewnNavmeshDialog");
		navmesh_dialog->set_title(T("Bake Navigation Mesh"));
		navmesh_dialog->set_ok_button_text(T("Bake..."));
		VBoxContainer *v = memnew(VBoxContainer);
		navmesh_dialog->add_child(v);
		navmesh_what = memnew(Label);
		navmesh_what->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		navmesh_what->set_custom_minimum_size(Vector2(380, 0) * es);
		v->add_child(navmesh_what);
		// (its settings: a plain NavigationMesh, as Godot shows it)
		navmesh_inspector = memnew(EditorInspector);
		navmesh_inspector->set_custom_minimum_size(Vector2(380, 420) * es);
		navmesh_inspector->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		v->add_child(navmesh_inspector);
		navmesh_region = memnew(CheckBox);
		navmesh_region->set_text(T("Add a NavigationRegion3D with it to the scene"));
		navmesh_region->set_pressed(true);
		v->add_child(navmesh_region);
		navmesh_dialog->connect("confirmed", callable_mp(this, &HewnEditorPlugin::_on_navmesh_ok));
		EditorInterface::get_singleton()->get_base_control()->add_child(navmesh_dialog);
		navmesh_file = memnew(EditorFileDialog);
		navmesh_file->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
		navmesh_file->set_access(EditorFileDialog::ACCESS_RESOURCES);
		navmesh_file->add_filter("*.tres", "Text Resource");
		navmesh_file->add_filter("*.res", "Binary Resource");
		navmesh_file->set_title(T("Save Navigation Mesh"));
		navmesh_file->connect("file_selected", callable_mp(this, &HewnEditorPlugin::_on_navmesh_file));
		EditorInterface::get_singleton()->get_base_control()->add_child(navmesh_file);
	}
	if (navmesh_settings.is_null()) {
		navmesh_settings.instantiate();
		navmesh_settings->set_parsed_geometry_type(NavigationMesh::PARSED_GEOMETRY_STATIC_COLLIDERS); // (where one can walk: what collides)
	}
	// what it is made of: the selected objects, else all of the scene
	navmesh_nodes.clear();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
		Node *n = Object::cast_to<Node>(v);
		if (n && n != root) {
			navmesh_nodes.push_back(ObjectID(n->get_instance_id()));
		}
	}
	navmesh_what->set_text(navmesh_nodes.empty() ? T("Of the whole scene (nothing selected).") : vformat(T("Of the selected objects (%d) and what is under them."), (int)navmesh_nodes.size()));
	navmesh_inspector->edit(navmesh_settings.ptr());
	navmesh_dialog->reset_size();
	navmesh_dialog->popup_centered(Vector2i(Vector2(420, 560) * EditorInterface::get_singleton()->get_editor_scale()));
}

void HewnEditorPlugin::_on_navmesh_ok() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	const String dir = root && !root->get_scene_file_path().is_empty() ? root->get_scene_file_path().get_base_dir() : String("res://");
	const String name = root ? String(root->get_name()).to_snake_case() : String("level");
	navmesh_file->set_current_dir(dir);
	navmesh_file->set_current_file(name + String("_navmesh.tres"));
	navmesh_file->popup_file_dialog();
}

void HewnEditorPlugin::_on_navmesh_file(const String &p_path) {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	Node3D *root3 = Object::cast_to<Node3D>(root);
	if (!root || navmesh_settings.is_null()) {
		return;
	}
	// the settings copied (the window keeps its own); the selected objects through a group of their own, parsed from the
	// scene's root: everything in its space, as a region at the root's origin needs it
	Ref<NavigationMesh> nm = navmesh_settings->duplicate();
	const StringName group = "_hw_navmesh_bake";
	std::vector<Node *> grouped;
	if (!navmesh_nodes.empty()) {
		for (const ObjectID &id : navmesh_nodes) {
			if (Node *n = Object::cast_to<Node>(ObjectDB::get_instance(id))) {
				n->add_to_group(group);
				grouped.push_back(n);
			}
		}
		nm->set_source_geometry_mode(NavigationMesh::SOURCE_GEOMETRY_GROUPS_WITH_CHILDREN);
		nm->set_source_group_name(group);
	} else {
		nm->set_source_geometry_mode(NavigationMesh::SOURCE_GEOMETRY_ROOT_NODE_CHILDREN);
	}
	Ref<NavigationMeshSourceGeometryData3D> src;
	src.instantiate();
	NavigationServer3D *ns = NavigationServer3D::get_singleton();
	ns->parse_source_geometry_data(nm, src, root);
	for (Node *n : grouped) {
		n->remove_from_group(group);
	}
	ns->bake_from_source_geometry_data(nm, src);
	// (the settings the window had, saved as they were chosen)
	nm->set_source_geometry_mode(navmesh_settings->get_source_geometry_mode());
	nm->set_source_group_name(navmesh_settings->get_source_group_name());
	const int polys = nm->get_polygon_count();
	const Error err = ResourceSaver::get_singleton()->save(nm, p_path);
	EditorInterface::get_singleton()->get_resource_filesystem()->scan();
	if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
		if (err != OK) {
			t->push_toast(vformat(T("Could not save %s"), p_path), EditorToaster::SEVERITY_ERROR);
		} else if (polys == 0) {
			t->push_toast(T("Navigation mesh: nothing to walk on (no collision? try Parsed Geometry Type: Mesh Instances or Both)"), EditorToaster::SEVERITY_WARNING);
		} else {
			t->push_toast(vformat(T("Navigation mesh saved: %s (%d polygons)"), p_path, polys));
		}
	}
	if (err != OK || !navmesh_region->is_pressed() || !root3) {
		return;
	}
	const Ref<NavigationMesh> saved = ResourceLoader::get_singleton()->load(p_path, "", ResourceLoader::CACHE_MODE_REPLACE);
	NavigationRegion3D *reg = memnew(NavigationRegion3D);
	reg->set_name("NavigationRegion3D");
	reg->set_navigation_mesh(saved.is_valid() ? saved : nm);
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Add Navigation Region", UndoRedo::MERGE_DISABLE, root);
	ur->add_do_method(root, "add_child", reg, true);
	ur->add_do_method(reg, "set_owner", root);
	ur->add_do_reference(reg);
	ur->add_undo_method(root, "remove_child", reg);
	ur->commit_action();
	EditorInterface::get_singleton()->get_selection()->clear();
	EditorInterface::get_singleton()->get_selection()->add_node(reg);
}
