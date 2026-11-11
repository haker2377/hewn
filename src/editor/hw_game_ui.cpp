// Game: the level baked for the game (HewnLevel), lightmap UV2 for all meshes.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include "../hw_level_bake.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/confirmation_dialog.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/editor_file_dialog.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>

using namespace godot;
using namespace hw_editor;

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
	return HewnLevel::project_bake_options(bake_kind == 1); // (as the window left them)
}

void HewnEditorPlugin::_bake_refresh_warning() {
	if (!bo_warn) {
		return;
	}
	const bool off = !(bool)ProjectSettings::get_singleton()->get_setting("rendering/occlusion_culling/use_occlusion_culling", false);
	const bool show = off && bake_kind == 0 && bo_occl->is_pressed();
	bo_warn->set_visible(show);
	bo_fix->set_visible(show);
}

void HewnEditorPlugin::_on_bake_fix_occlusion() {
	ProjectSettings::get_singleton()->set_setting("rendering/occlusion_culling/use_occlusion_culling", true);
	ProjectSettings::get_singleton()->save();
	_bake_refresh_warning();
}

void HewnEditorPlugin::_open_bake_options(int p_kind) {
	bake_kind = p_kind;
	if (!bake_opts) {
		const float es = EditorInterface::get_singleton()->get_editor_scale();
		bake_opts = memnew(ConfirmationDialog);
		bake_opts->set_name("HewnBakeOptions");
		bo_scroll = memnew(ScrollContainer);
		bo_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
		bake_opts->add_child(bo_scroll);
		VBoxContainer *v = memnew(VBoxContainer);
		v->set_custom_minimum_size(Vector2(400, 0) * es);
		v->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		bo_scroll->add_child(v);
		bo_body = v;
		auto check = [&](Container *p_in, const char *p_text, const char *p_tip) {
			CheckBox *c = memnew(CheckBox);
			c->set_text(T(p_text));
			c->set_tooltip_text(T(p_tip));
			p_in->add_child(c);
			return c;
		};
		auto spin = [&](Container *p_in, const char *p_text, const char *p_tip, double p_min, double p_max) {
			HBoxContainer *h = memnew(HBoxContainer);
			Label *l = memnew(Label);
			l->set_text(T(p_text));
			l->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			h->add_child(l);
			SpinBox *s = memnew(SpinBox);
			s->set_min(p_min);
			s->set_max(p_max);
			s->set_step(0.5);
			s->set_suffix("m");
			s->set_tooltip_text(T(p_tip));
			h->add_child(s);
			p_in->add_child(h);
			return s;
		};
		VBoxContainer *ov = memnew(VBoxContainer);
		v->add_child(ov);
		bo_occl_row = ov;
		bo_occl = check(ov, "Occluders", "An OccluderInstance3D for each big opaque mesh: what is hidden behind them is not drawn");
		bo_occl->connect("toggled", callable_mp(this, &HewnEditorPlugin::_bake_refresh_warning).unbind(1));
		bo_occl_min = spin(ov, "Smallest occluder", "Meshes smaller than this have no occluder: small things hide little and cost as much", 0.0, 100.0);
		bo_warn = memnew(Label);
		bo_warn->set_text(T("Occlusion culling is off in this project: occluders do nothing until it is on."));
		ov->add_child(bo_warn);
		bo_fix = memnew(Button);
		bo_fix->set_text(T("Turn On Occlusion Culling"));
		bo_fix->connect("pressed", callable_mp(this, &HewnEditorPlugin::_on_bake_fix_occlusion));
		ov->add_child(bo_fix);
		bo_merge = check(v, "Merge static meshes", "Small meshes with the same metadata, groups and render settings joined in squares of the map: fewer draw calls. Not those with a script, under a moving parent or with Merge At Bake off");
		bo_cell = spin(v, "Merge square size", "The size of the squares small meshes are joined in", 1.0, 1000.0);
		bo_plain = check(v, "Also plain meshes", "MeshInstance3D nodes that are not Hewn's are joined and split too, by the same rules");
		bo_split = check(v, "Split big meshes", "Detailed meshes bigger than the size below cut into squares of it: what is out of view is not drawn. By the same rules as merging");
		bo_split_size = spin(v, "Split size", "Only meshes bigger than this are cut, into squares of this size", 1.0, 10000.0);
		bo_coll = check(v, "Merge collision", "The static bodies of one square with the same metadata, groups and layers become one body");
		bo_lod = check(v, "Detail levels (LOD)", "Simpler versions of bigger meshes, used from afar. Costs only baking time");
		bo_range = check(v, "Visibility range by size", "Small things stop being drawn from afar: faster, but they can be seen disappearing");
		bo_hidden = check(v, "Remove faces hidden inside other meshes", "Faces wholly inside another closed mesh are left out. Slower to bake, and wrong if one of the two is moved later");
		bake_opts->connect("confirmed", callable_mp(this, &HewnEditorPlugin::_on_bake_options_ok));
		EditorInterface::get_singleton()->get_base_control()->add_child(bake_opts);
	}
	const Dictionary d = HewnLevel::project_bake_options(p_kind == 1);
	bake_opts->set_title(p_kind == 1 ? T("Export Scene glTF") : T("Bake Level"));
	bake_opts->set_ok_button_text(T("Continue..."));
	bo_occl_row->set_visible(p_kind == 0);
	bo_lod->set_visible(p_kind == 0);
	bo_occl->set_pressed(d["occluders"]);
	bo_occl_min->set_value(d["occluder_min_size"]);
	bo_merge->set_pressed(d["merge"]);
	bo_cell->set_value(d["merge_cell"]);
	bo_split->set_pressed(d["split"]);
	bo_split_size->set_value(d["split_size"]);
	bo_plain->set_pressed(d["merge_plain"]);
	bo_coll->set_pressed(d["merge_collision"]);
	bo_lod->set_pressed(d["lod"]);
	bo_range->set_pressed(d["vis_range"]);
	bo_hidden->set_pressed(d["hidden_faces"]);
	_bake_refresh_warning();
	// (all of it shown when the screen has the room, else it scrolls)
	const float room = EditorInterface::get_singleton()->get_base_control()->get_size().y * 0.75f;
	bo_scroll->set_custom_minimum_size(Vector2(0, MIN(bo_body->get_combined_minimum_size().y, room)));
	bake_opts->reset_size();
	bake_opts->popup_centered();
}

void HewnEditorPlugin::_on_bake_options_ok() {
	const String prefix = bake_kind == 1 ? "hewn/bake_gltf/" : "hewn/bake/";
	ProjectSettings *ps = ProjectSettings::get_singleton();
	Dictionary now;
	now["occluders"] = bo_occl->is_pressed();
	now["occluder_min_size"] = bo_occl_min->get_value();
	now["merge"] = bo_merge->is_pressed();
	now["merge_cell"] = bo_cell->get_value();
	now["split"] = bo_split->is_pressed();
	now["split_size"] = bo_split_size->get_value();
	now["merge_plain"] = bo_plain->is_pressed();
	now["merge_collision"] = bo_coll->is_pressed();
	now["lod"] = bo_lod->is_pressed();
	now["vis_range"] = bo_range->is_pressed();
	now["hidden_faces"] = bo_hidden->is_pressed();
	const Array keys = now.keys();
	for (int64_t i = 0; i < keys.size(); i++) {
		ps->set_setting(prefix + String(keys[i]), now[keys[i]]);
	}
	ps->save();
	if (bake_kind == 1) {
		if (!scene_gltf_dialog) {
			scene_gltf_dialog = make_dialog(this, false, PackedStringArray({ "*.glb;glTF Binary", "*.gltf;glTF Text" }), callable_mp(this, &HewnEditorPlugin::_on_scene_gltf_file));
			scene_gltf_dialog->set_title("Export Scene glTF");
		}
		scene_gltf_dialog->set_current_file(scene_base_name() + ".glb");
		scene_gltf_dialog->popup_file_dialog();
		return;
	}
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

void HewnEditorPlugin::play_baked() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return;
	}
	const String path = "res://.godot/hewn_run_baked.scn";
	if (HewnLevel::save_baked_scene(root, path, HewnLevel::project_bake_options()) != OK) {
		ERR_PRINT("Hewn: Run Baked failed.");
		return;
	}
	if (OS::get_singleton()->get_environment("HW_SELFTEST").is_empty()) {
		EditorInterface::get_singleton()->play_custom_scene(path);
	}
}

void HewnEditorPlugin::bake_level() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return;
	}
	_open_bake_options(0);
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
	_open_bake_options(1);
}

void HewnEditorPlugin::_on_scene_gltf_file(const String &p_path) {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (root && HewnLevel::export_scene_gltf(root, p_path, HewnLevel::project_bake_options(true)) != OK) {
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
