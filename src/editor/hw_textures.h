// Texture browser (bottom panel): the images of the project as a palette. Clicking one puts it on the
// selected faces (edit mode) or the selected objects; its material is made once and saved next to the
// others.
#pragma once

#include <godot_cpp/classes/accept_dialog.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/color_picker_button.hpp>
#include <godot_cpp/classes/confirmation_dialog.hpp>
#include <godot_cpp/classes/editor_dock.hpp>
#include <godot_cpp/classes/editor_file_dialog.hpp>
#include <godot_cpp/classes/h_slider.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/menu_button.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include <vector>

namespace godot {

class HewnEditorPlugin;

class HewnTextureBrowser : public EditorDock {
	GDCLASS(HewnTextureBrowser, EditorDock)

	HewnEditorPlugin *plugin = nullptr;
	String dir = "res://";
	std::vector<String> history;
	int history_pos = -1;
	uint64_t generation = 0; // previews of an older listing are dropped

	Button *back_button = nullptr;
	Button *forward_button = nullptr;
	Button *up_button = nullptr;
	LineEdit *path_edit = nullptr;
	MenuButton *favorites = nullptr;
	LineEdit *search = nullptr;
	HSlider *thumb_size = nullptr;
	CheckBox *nearest = nullptr;
	CheckBox *subfolders = nullptr; // the images of all subfolders at once
	Button *current_button = nullptr; // the texture new shapes get ("x": none)
	void _on_clear_current();
	SpinBox *density = nullptr; // pixels per meter
	ItemList *list = nullptr;
	Label *status = nullptr;
	EditorFileDialog *material_dir_dialog = nullptr;
	ConfirmationDialog *new_dialog = nullptr;
	LineEdit *new_name = nullptr;
	ColorPickerButton *new_color = nullptr;
	OptionButton *new_size = nullptr;
	CheckBox *new_grid = nullptr;
	bool refresh_queued = false;

	// The materials of the project and the images they show (hw_matlist.cpp): the Material List window, the
	// counts on the thumbnails
	struct MatInfo {
		String path;
		String kind;
		PackedStringArray textures;
	};
	std::vector<MatInfo> mat_index;
	bool mat_index_dirty = true;
	void _build_mat_index();
	int _uses_of(const String &p_image);
	AcceptDialog *matlist_dialog = nullptr;
	Tree *matlist_tree = nullptr;
	LineEdit *matlist_search = nullptr;
	CheckBox *matlist_only = nullptr;
	String matlist_texture;
	// the right side: the selected material in a small inspector, the meshes of the scene using it, and
	// another material in its place on all of them
	Control *matlist_inspector = nullptr; // (an EditorInspector, when Godot lets one be made)
	ItemList *matlist_users = nullptr;
	Label *matlist_users_label = nullptr;
	class EditorResourcePicker *matlist_replace = nullptr;
	Ref<Material> matlist_current;
	void _matlist_selected();
	void _matlist_saved(const String &p_property);
	void _matlist_user_clicked(int p_index);
	void _matlist_do_replace();
	std::vector<ObjectID> matlist_user_ids;
	PopupMenu *tex_menu = nullptr;
	String tex_menu_path;
	String click_pending; // (a click puts the image on at the release: a drag does not)
	String drag_path;
	Variant _list_drag(const Vector2 &p_at);
	void _on_list_input(const Ref<InputEvent> &p_event);
	void _matlist_make();
	void _matlist_fill();
	int matlist_sort = -1; // the column the list is sorted by (-1: as found), and which way
	bool matlist_sort_desc = false;
	void _matlist_sort_by(int p_column, int p_button);
	void _matlist_apply();
	void _matlist_edit();
	void _matlist_action(const StringName &p_action);
	void _matlist_search(const String &p_text);
	void _matlist_only(bool p_on);
	void _on_tex_menu(int p_id);
	void _on_material_list_button();
	void _on_mat_preview(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_thumb, const Variant &p_userdata);

	void _navigate(const String &p_dir, bool p_history);
	void _refresh();
	void _queue_refresh();
	void _on_back();
	void _on_forward();
	void _on_up();
	void _on_path_entered(const String &p_text);
	void _on_search_changed(const String &p_text);
	void _on_thumb_size(double p_value);
	void _on_item_clicked(int p_index, const Vector2 &p_at, int p_button);
	void _on_favorite(int p_id);
	void _update_favorites();
	void _on_material_dir(const String &p_dir);
	void _on_material_dir_button();
	void _on_new_texture();
	void _on_new_confirmed();
	void _on_preview(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_thumb, const Variant &p_userdata);
	void _save_settings();
	void _on_density(double p_value);
	void _on_nearest(bool p_on);
	void _on_subfolders(bool p_on);
	void _apply_density(const Ref<Material> &p_material) const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static bool is_image(const String &p_path);
	static bool is_material_file(const String &p_path);
	// The material of an image: found by its metadata in the materials folder, or made and saved there.
	Ref<Material> material_for_image(const String &p_image);
	String material_dir() const;
	// The materials' folder not chosen yet: asks for it (Project Settings > Hewn > Paths) and calls
	// p_then once it is; false when it is known already.
	bool ask_material_dir(const Callable &p_then);
	Callable material_dir_then;
	static void register_settings();
	float pixels_per_meter() const; // 0: one tile per meter
	void set_status(const String &p_text);
	// material mapping: a material knows its primary image (meta "hw_image"); clicking an image
	// takes the material mapped to it. Fix Material Mapping maps the project's own materials (made by hand,
	// by script, shader materials with a texture parameter) to their image.
	static String image_of(const Ref<Material> &p_material); // the mapped image, else the texture it shows
	static String texture_of(const Ref<Material> &p_material); // the texture it shows (albedo / first texture parameter)
	int fix_material_mapping();
	void map_material(const Ref<Material> &p_material, const String &p_image); // (saved)
	// The template of new materials (all their settings but the texture), made on first use.
	String default_material_path() const;
	Ref<Material> default_material(bool p_create);
	bool get_default_nearest() const;
	void set_default_nearest(bool p_on); // the filtering of new materials
	void setup(HewnEditorPlugin *p_plugin);
	String get_dir() const { return dir; }
	void open_dir(const String &p_dir) { _navigate(p_dir, true); }
	void click_image(const String &p_path); // (tests) like a click on its thumbnail
	// A thumbnail dragged onto a 3D view: onto the object / the face under the mouse (screen position)
	void drop_image(const String &p_path, const Vector2 &p_screen);
	// The materials of the project, what kind each is and the images it shows (only those showing
	// `p_texture`, if any); double click: on the selection
	void open_material_list(const String &p_texture);
	// The texture picked last: new shapes, walls, sweeps, columns are made with it.
	Ref<Material> current_material;
	void set_current(const Ref<Material> &p_material, const String &p_name);
	void make_scrolling(const String &p_image); // (a material moving its image, on the selection)
};

} // namespace godot
