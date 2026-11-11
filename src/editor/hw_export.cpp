// Hewn is an editor tool: exported games do not need it. While a project is exported every scene's Hewn nodes
// become plain Godot nodes (meshes, collision, groups and metadata), and Hewn's own files stay out of the export.
#include "hw_editor.h"

#include "../hw_level_bake.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

void HewnExportPlugin::_bind_methods() {}

String HewnExportPlugin::_get_name() const {
	return "Hewn";
}

bool HewnExportPlugin::_begin_customize_scenes(const Ref<EditorExportPlatform> &p_platform, const PackedStringArray &p_features) const {
	return true;
}

uint64_t HewnExportPlugin::_get_customization_configuration_hash() const {
	return 0x4865776e03ull ^ (uint64_t)UtilityFunctions::var_to_str(HewnLevel::project_bake_options()).hash(); // (changes when what the conversion does changes)
}

// Only scenes that have Hewn nodes change. The scene is put in a world of its own for a moment: the cutters cut
// and the collision is made in world space.
Node *HewnExportPlugin::_customize_scene(Node *p_scene, const String &p_path) {
	if (!p_scene) {
		return nullptr;
	}
	bool any = false;
	std::vector<Node *> stack = { p_scene };
	while (!stack.empty() && !any) {
		Node *x = stack.back();
		stack.pop_back();
		any = x->get_class().begins_with("Hewn");
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
	}
	if (!any) {
		return nullptr;
	}
	SubViewport *world = memnew(SubViewport);
	world->set_use_own_world_3d(true);
	world->set_update_mode(SubViewport::UPDATE_DISABLED);
	world->set_size(Vector2i(2, 2));
	EditorInterface::get_singleton()->get_base_control()->add_child(world);
	world->add_child(p_scene);
	bool changed = false;
	Node *out = HewnLevel::to_game_nodes(p_scene, changed);
	world->remove_child(out);
	world->get_parent()->remove_child(world);
	memdelete(world);
	return changed ? out : nullptr;
}

// The list of extensions the game loads, without Hewn's (its file is not exported): written so on disk while the
// export runs, put back when it ends (Godot also writes it again on its own when it scans the project).
void HewnExportPlugin::_export_begin(const PackedStringArray &p_features, bool p_is_debug, const String &p_path, uint32_t p_flags) {
	const String list = "res://.godot/extension_list.cfg";
	saved_list = String();
	if (!FileAccess::file_exists(list)) {
		return;
	}
	saved_list = FileAccess::get_file_as_string(list);
	String keep;
	for (const String &line : saved_list.split(String::chr(10), false)) {
		if (!line.strip_edges().begins_with("res://addons/hewn/")) {
			keep += line + String::chr(10);
		}
	}
	Ref<FileAccess> f = FileAccess::open(list, FileAccess::WRITE);
	if (f.is_valid()) {
		f->store_string(keep);
	}
}

void HewnExportPlugin::_export_end() {
	if (saved_list.is_empty()) {
		return;
	}
	Ref<FileAccess> f = FileAccess::open("res://.godot/extension_list.cfg", FileAccess::WRITE);
	if (f.is_valid()) {
		f->store_string(saved_list);
	}
	saved_list = String();
}

// Hewn's extension file, its libraries and its own scripts are not part of the game.
void HewnExportPlugin::_export_file(const String &p_path, const String &p_type, const PackedStringArray &p_features) {
	if (p_path.begins_with("res://addons/hewn/")) {
		skip();
	}
}
