// Prefab placer: a bottom panel tab with the scenes of the project; a click starts placing one
// in the 3D view - on the surface under the mouse (along its normal or upright), on the grid, turned
// with Q / E, with a random size / turn; W after placing places another one.
#pragma once

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/editor_dock.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/popup_panel.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/texture2d.hpp>

namespace godot {

class HewnEditorPlugin;

class HewnPrefabBrowser : public EditorDock {
	GDCLASS(HewnPrefabBrowser, EditorDock)

	HewnEditorPlugin *plugin = nullptr;
	String dir = "res://";
	bool loading_settings = false;
	uint64_t generation = 0;
	LineEdit *path_edit = nullptr;
	LineEdit *search = nullptr;
	ItemList *list = nullptr;
	Label *status = nullptr;

	void _navigate(const String &p_dir);
	void _refresh();
	void _on_up();
	void _on_path(const String &p_text);
	void _on_search(const String &p_text);
	void _on_item_clicked(int p_index, const Vector2 &p_at, int p_button);
	Variant _list_drag(const Vector2 &p_at);
	void _on_preview(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_thumb, const Variant &p_userdata);

protected:
	static void _bind_methods();

public:
	// Placement (a popup: too many for the bar): random rotation / size per axis.
	CheckBox *inherit_normal = nullptr;
	CheckBox *random_rotation = nullptr;
	SpinBox *rotation_min[3] = {}; // degrees
	SpinBox *rotation_max[3] = {};
	CheckBox *random_size = nullptr;
	CheckBox *size_uniform = nullptr; // one random factor for all axes (from X)
	SpinBox *size_min[3] = {};
	SpinBox *size_max[3] = {};
	LineEdit *suffix = nullptr;
	CheckBox *make_local = nullptr;
	CheckBox *subfolders = nullptr; // the scenes of all subfolders at once
	Button *placement_button = nullptr;
	PopupPanel *placement_popup = nullptr;
	void _on_placement();
	void _on_reset_random(bool p_size);
	void _on_setting_changed();
	void _save_settings();
	Vector3 random_rotation_value(uint32_t &r_seed) const;
	Vector3 random_size_value(uint32_t &r_seed) const;

	static bool is_scene(const String &p_path);
	void set_status(const String &p_text);
	void setup(HewnEditorPlugin *p_plugin);
	void place(const String &p_path); // (tests) like a click on it
};

} // namespace godot
