// Game: the level baked for the game (HewnLevel), lightmap UV2 for all meshes.
#include "hw_editor.h"

#include "../hw_level_bake.h"

#include <godot_cpp/classes/editor_file_dialog.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>

using namespace godot;

namespace {

void collect_meshes(Node *p_node, std::vector<HewnMesh3D *> &r_out) {
	if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(p_node)) {
		if (!m->is_brush() && !m->is_boolean_cutter()) {
			r_out.push_back(m);
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		collect_meshes(p_node->get_child(i), r_out);
	}
}

EditorFileDialog *make_dialog(HewnEditorPlugin *p_plugin, bool p_resources, const PackedStringArray &p_filters, const Callable &p_on_file) {
	EditorFileDialog *d = memnew(EditorFileDialog);
	d->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	d->set_access(p_resources ? EditorFileDialog::ACCESS_RESOURCES : EditorFileDialog::ACCESS_FILESYSTEM);
	for (const String &f : p_filters) {
		d->add_filter(f.get_slice(";", 0), f.get_slice(";", 1));
	}
	d->connect("file_selected", p_on_file);
	EditorInterface::get_singleton()->get_base_control()->add_child(d);
	return d;
}

String scene_base_name() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return "level";
	}
	const String path = root->get_scene_file_path();
	return path.is_empty() ? String(root->get_name()) : path.get_file().get_basename();
}

} // namespace

Dictionary HewnEditorPlugin::_bake_options() const {
	Dictionary o;
	o["occluders"] = dock && dock->bake_occluders ? dock->bake_occluders->is_pressed() : true;
	return o;
}

void HewnEditorPlugin::bake_level() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return;
	}
	if (!bake_dialog) {
		bake_dialog = make_dialog(this, true, PackedStringArray({ "*.tscn;Text Scene", "*.scn;Binary Scene" }), callable_mp(this, &HewnEditorPlugin::_on_bake_file));
		bake_dialog->set_title("Bake Level");
	}
	const String path = root->get_scene_file_path();
	const String baked_dir = ProjectSettings::get_singleton()->get_setting("hewn/paths/baked_scenes", String());
	if (!baked_dir.is_empty()) {
		bake_dialog->set_current_dir(baked_dir); // (Project Settings > Hewn > Paths)
	} else if (!path.is_empty()) {
		bake_dialog->set_current_dir(path.get_base_dir());
	}
	bake_dialog->set_current_file(scene_base_name() + "_baked.tscn");
	bake_dialog->popup_file_dialog();
}

void HewnEditorPlugin::_on_bake_file(const String &p_path) {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return;
	}
	if (root->get_scene_file_path() == p_path) {
		ERR_PRINT("Hewn: Bake Level would overwrite the edited scene; pick another file.");
		return;
	}
	if (String(ProjectSettings::get_singleton()->get_setting("hewn/paths/baked_scenes", String())).is_empty()) {
		// (the first bake: its folder is the one next time; changed in Project Settings > Hewn > Paths)
		ProjectSettings::get_singleton()->set_setting("hewn/paths/baked_scenes", p_path.get_base_dir());
		ProjectSettings::get_singleton()->save();
	}
	const Error err = HewnLevel::save_baked_scene(root, p_path, _bake_options());
	if (err != OK) {
		ERR_PRINT("Hewn: Bake Level failed: " + p_path);
		return;
	}
	EditorInterface::get_singleton()->get_resource_filesystem()->update_file(p_path);
	print_line("Hewn: level baked to " + p_path);
}

void HewnEditorPlugin::export_scene_gltf() {
	if (!EditorInterface::get_singleton()->get_edited_scene_root()) {
		return;
	}
	if (!scene_gltf_dialog) {
		scene_gltf_dialog = make_dialog(this, false, PackedStringArray({ "*.glb;glTF Binary", "*.gltf;glTF Text" }), callable_mp(this, &HewnEditorPlugin::_on_scene_gltf_file));
		scene_gltf_dialog->set_title("Export Scene glTF");
	}
	scene_gltf_dialog->set_current_file(scene_base_name() + ".glb");
	scene_gltf_dialog->popup_file_dialog();
}

void HewnEditorPlugin::_on_scene_gltf_file(const String &p_path) {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (root && HewnLevel::export_scene_gltf(root, p_path) != OK) {
		ERR_PRINT("Hewn: scene glTF export failed: " + p_path);
	}
}

void HewnEditorPlugin::lightmap_all() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return;
	}
	std::vector<HewnMesh3D *> meshes;
	collect_meshes(root, meshes);
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Lightmap UV2 for All", UndoRedo::MERGE_DISABLE, root);
	int n = 0;
	for (HewnMesh3D *m : meshes) {
		if (m->get_lightmap_uv2()) {
			continue;
		}
		ur->add_do_property(m, "lightmap_uv2", true);
		ur->add_undo_property(m, "lightmap_uv2", false);
		n++;
	}
	ur->commit_action();
	print_line(vformat("Hewn: lightmap UV2 on for %d meshes", n));
}
