// Texture browser: a bottom panel tab with the images of the project. Click: the texture goes onto the
// selected faces (edit mode) or the selected objects; its StandardMaterial3D is created once in the
// materials folder (found again by the image it was made for, so it can be renamed and edited freely).
#include "hw_textures.h"

#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/h_flow_container.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_resource_preview.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/v_separator.hpp>

using namespace godot;
using hw_editor::T;

namespace {

const char *META_IMAGE = "hw_image";

Ref<Texture2D> theme_icon(const char *p_name) {
	return EditorInterface::get_singleton()->get_editor_theme()->get_icon(p_name, "EditorIcons");
}

bool settings_on() {
	return OS::get_singleton()->get_environment("HW_SELFTEST").is_empty();
}

String join_path(const String &p_dir, const String &p_name) {
	return p_dir.ends_with("/") ? p_dir + p_name : p_dir + String("/") + p_name;
}

String parent_of(const String &p_dir) {
	if (p_dir == "res://") {
		return p_dir;
	}
	String d = p_dir.ends_with("/") ? p_dir.substr(0, p_dir.length() - 1) : p_dir;
	d = d.get_base_dir();
	return d == "res:" || d.is_empty() ? String("res://") : d;
}

} // namespace

void HewnTextureBrowser::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_preview", "path", "preview", "thumbnail", "userdata"), &HewnTextureBrowser::_on_preview);
	ClassDB::bind_method(D_METHOD("click_image", "path"), &HewnTextureBrowser::click_image); // (tests)
	ClassDB::bind_method(D_METHOD("drop_image", "path", "screen_position"), &HewnTextureBrowser::drop_image); // (tests)
	ClassDB::bind_method(D_METHOD("_on_mat_preview", "path", "preview", "thumbnail", "userdata"), &HewnTextureBrowser::_on_mat_preview);
}

// A material of the project's own (not one Hewn made for an image: that image stands for it).
bool HewnTextureBrowser::is_material_file(const String &p_path) {
	const String e = p_path.get_extension().to_lower();
	if (e != "tres" && e != "res" && e != "material") {
		return false;
	}
	ResourceLoader *rl = ResourceLoader::get_singleton();
	if (!rl->exists(p_path, "Material")) {
		return false;
	}
	const Ref<Material> m = rl->load(p_path);
	return m.is_valid() && !m->has_meta(META_IMAGE) && p_path.get_file() != "_default_material.tres";
}

bool HewnTextureBrowser::is_image(const String &p_path) {
	const String e = p_path.get_extension().to_lower();
	return e == "png" || e == "jpg" || e == "jpeg" || e == "webp" || e == "svg" || e == "tga" || e == "bmp" || e == "exr" || e == "hdr" || e == "ktx" ||
			e == "dds";
}

void HewnTextureBrowser::setup(HewnEditorPlugin *p_plugin) {
	plugin = p_plugin;
	set_name("HewnTextures");
	set_title(T("Textures"));
	set_default_slot(EditorDock::DOCK_SLOT_BOTTOM);
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid() && settings_on()) {
		dir = es->get_project_metadata("hewn", "texture_dir", dir);
	}
	if (!DirAccess::dir_exists_absolute(dir)) {
		dir = "res://";
	}

	VBoxContainer *v = memnew(VBoxContainer);
	v->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	add_child(v);
	HFlowContainer *bar = memnew(HFlowContainer); // (wraps: a narrow bottom panel does not push the side docks)
	v->add_child(bar);
	auto icon_button = [&](const char *p_icon, const char *p_tip) {
		Button *b = memnew(Button);
		b->set_button_icon(theme_icon(p_icon));
		b->set_flat(true);
		b->set_tooltip_text(p_tip);
		bar->add_child(b);
		return b;
	};
	back_button = icon_button("Back", "Back (mouse button 4)");
	back_button->connect("pressed", callable_mp(this, &HewnTextureBrowser::_on_back));
	forward_button = icon_button("Forward", "Forward (mouse button 5)");
	forward_button->connect("pressed", callable_mp(this, &HewnTextureBrowser::_on_forward));
	up_button = icon_button("ArrowUp", "Parent folder");
	up_button->connect("pressed", callable_mp(this, &HewnTextureBrowser::_on_up));
	path_edit = memnew(LineEdit);
	path_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	path_edit->set_custom_minimum_size(Vector2(160, 0));
	path_edit->set_tooltip_text("Folder (type a path and press Enter)");
	path_edit->connect("text_submitted", callable_mp(this, &HewnTextureBrowser::_on_path_entered));
	bar->add_child(path_edit);
	favorites = memnew(MenuButton);
	favorites->set_button_icon(theme_icon("Favorites"));
	favorites->set_tooltip_text("Favorite folders");
	favorites->get_popup()->connect("id_pressed", callable_mp(this, &HewnTextureBrowser::_on_favorite));
	bar->add_child(favorites);
	search = memnew(LineEdit);
	search->set_placeholder("Search (this folder and below)");
	search->set_custom_minimum_size(Vector2(180, 0));
	search->set_clear_button_enabled(true);
	search->connect("text_changed", callable_mp(this, &HewnTextureBrowser::_on_search_changed));
	bar->add_child(search);
	bar->add_child(memnew(VSeparator));
	thumb_size = memnew(HSlider);
	thumb_size->set_min(32);
	thumb_size->set_max(192);
	thumb_size->set_step(8);
	thumb_size->set_value(80);
	thumb_size->set_custom_minimum_size(Vector2(90, 0));
	thumb_size->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	thumb_size->set_tooltip_text("Thumbnail size");
	thumb_size->connect("value_changed", callable_mp(this, &HewnTextureBrowser::_on_thumb_size));
	bar->add_child(thumb_size);
	density = memnew(SpinBox);
	HewnDragField::hook_spinbox(density);
	density->set_min(0);
	density->set_max(8192);
	density->set_step(1);
	density->set_suffix("px/m");
	density->set_value(es.is_valid() ? (double)es->get_project_metadata("hewn", "pixels_per_meter", 128) : 128.0);
	density->set_tooltip_text("Pixels per meter: at 128 a 256 px texture covers 2 m, so all textures have the same pixel size (0: one tile per meter). Changing it updates the materials made here");
	density->connect("value_changed", callable_mp(this, &HewnTextureBrowser::_on_density));
	bar->add_child(density);
	subfolders = memnew(CheckBox);
	subfolders->set_text("Subfolders");
	subfolders->set_tooltip_text("The images of all subfolders at once (no need to open them)");
	subfolders->set_pressed(es.is_valid() && settings_on() && (bool)es->get_project_metadata("hewn", "texture_subfolders", false));
	subfolders->connect("toggled", callable_mp(this, &HewnTextureBrowser::_on_subfolders));
	bar->add_child(subfolders);
	nearest = memnew(CheckBox);
	nearest->set_text("Nearest");
	nearest->set_tooltip_text("New materials use nearest filtering (crisp pixels). The material of a texture already used: its Closest / Linear in the Material tab");
	nearest->set_pressed(es.is_valid() && (bool)es->get_project_metadata("hewn", "default_nearest", false));
	nearest->connect("toggled", callable_mp(this, &HewnTextureBrowser::_on_nearest));
	bar->add_child(nearest);
	current_button = memnew(Button);
	current_button->set_name("CurrentTexture");
	current_button->set_visible(false);
	current_button->set_tooltip_text("New shapes are made with this texture; click: none (the default material)");
	current_button->connect("pressed", callable_mp(this, &HewnTextureBrowser::_on_clear_current));
	bar->add_child(current_button);
	Button *nt = memnew(Button);
	nt->set_text("New Texture...");
	nt->set_tooltip_text("Make a plain placeholder image (a color with a grid) in this folder");
	nt->connect("pressed", callable_mp(this, &HewnTextureBrowser::_on_new_texture));
	bar->add_child(nt);
	Button *ml = memnew(Button);
	ml->set_name("MaterialListButton");
	ml->set_button_icon(theme_icon("StandardMaterial3D"));
	ml->set_text("Material List...");
	ml->set_tooltip_text("All the materials of the project: what kind each is (made for an image, blend of several textures, shader...) and the images it uses; double click puts one on the selection. Right click on a texture: the materials using it");
	ml->connect("pressed", callable_mp(this, &HewnTextureBrowser::_on_material_list_button));
	bar->add_child(ml);
	Button *md = memnew(Button);
	md->set_button_icon(theme_icon("Folder"));
	md->set_text("Materials");
	md->set_tooltip_text("Folder where the materials made for textures are saved");
	md->connect("pressed", callable_mp(this, &HewnTextureBrowser::_on_material_dir_button));
	bar->add_child(md);

	list = memnew(ItemList);
	list->set_name("TextureList");
	list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	list->set_max_columns(0);
	list->set_icon_mode(ItemList::ICON_MODE_TOP);
	list->set_same_column_width(true);
	list->set_max_text_lines(2);
	list->set_custom_minimum_size(Vector2(0, 140));
	list->connect("item_clicked", callable_mp(this, &HewnTextureBrowser::_on_item_clicked));
	list->connect("gui_input", callable_mp(this, &HewnTextureBrowser::_on_list_input));
	list->set_drag_forwarding(callable_mp(this, &HewnTextureBrowser::_list_drag), Callable(), Callable());
	v->add_child(list);
	tex_menu = memnew(PopupMenu);
	tex_menu->connect("id_pressed", callable_mp(this, &HewnTextureBrowser::_on_tex_menu));
	add_child(tex_menu);
	status = memnew(Label);
	status->set_clip_text(true);
	v->add_child(status);

	material_dir_dialog = memnew(EditorFileDialog);
	material_dir_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_DIR);
	material_dir_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	material_dir_dialog->connect("dir_selected", callable_mp(this, &HewnTextureBrowser::_on_material_dir));
	add_child(material_dir_dialog);

	new_dialog = memnew(ConfirmationDialog);
	new_dialog->set_title(T("New Texture"));
	GridContainer *g = memnew(GridContainer);
	g->set_columns(2);
	new_dialog->add_child(g);
	auto row = [&](const char *p_label, Control *p_field) {
		Label *l = memnew(Label);
		l->set_text(p_label);
		g->add_child(l);
		p_field->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		g->add_child(p_field);
	};
	new_name = memnew(LineEdit);
	new_name->set_text("dev_grey");
	row("Name", new_name);
	new_color = memnew(ColorPickerButton);
	new_color->set_pick_color(Color(0.55f, 0.55f, 0.55f));
	new_color->set_custom_minimum_size(Vector2(120, 28));
	row("Color", new_color);
	new_size = memnew(OptionButton);
	for (int s : { 64, 128, 256, 512, 1024 }) {
		new_size->add_item(String::num_int64(s) + " px", s);
	}
	new_size->select(2);
	row("Size", new_size);
	new_grid = memnew(CheckBox);
	new_grid->set_pressed(true);
	new_grid->set_text("Grid lines");
	row("", new_grid);
	new_dialog->connect("confirmed", callable_mp(this, &HewnTextureBrowser::_on_new_confirmed));
	add_child(new_dialog);

	if (EditorFileSystem *fs = EditorInterface::get_singleton()->get_resource_filesystem(); fs && !hw_editor::cmdline_export()) {
		fs->connect("filesystem_changed", callable_mp(this, &HewnTextureBrowser::_queue_refresh));
	}
	hw_editor::localize(this);
	_update_favorites();
	_navigate(dir, true);
}

void HewnTextureBrowser::_notification(int p_what) {
	if (p_what == NOTIFICATION_DRAG_END && !drag_path.is_empty()) {
		// dropped where nothing takes it (the 3D view does not know this drag): onto what is under the mouse
		const String p = drag_path;
		drag_path = String();
		if (get_viewport() && !get_viewport()->gui_is_drag_successful()) {
			drop_image(p, Vector2(DisplayServer::get_singleton()->mouse_get_position()));
		}
	}
	if (p_what == NOTIFICATION_PROCESS && refresh_queued) {
		refresh_queued = false;
		set_process(false);
		_refresh();
	}
}

void HewnTextureBrowser::_queue_refresh() {
	mat_index_dirty = true; // (a file changed: maybe a material)
	refresh_queued = true;
	set_process(true);
}

void HewnTextureBrowser::_save_settings() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid() && settings_on()) {
		es->set_project_metadata("hewn", "texture_dir", dir);
	}
}

void HewnTextureBrowser::_navigate(const String &p_dir, bool p_history) {
	String d = p_dir.strip_edges();
	if (!d.begins_with("res://")) {
		d = "res://" + d.trim_prefix("/");
	}
	if (!DirAccess::dir_exists_absolute(d)) {
		set_status(vformat(T("No folder %s"), d));
		path_edit->set_text(dir);
		return;
	}
	dir = d;
	if (p_history) {
		if (history_pos >= 0 && history_pos < (int)history.size() && history[history_pos] == dir) {
			// (same place)
		} else {
			history.resize(history_pos + 1);
			history.push_back(dir);
			history_pos = (int)history.size() - 1;
		}
	}
	_save_settings();
	_refresh();
}

void HewnTextureBrowser::_on_back() {
	if (history_pos > 0) {
		history_pos--;
		_navigate(history[history_pos], false);
	}
}

void HewnTextureBrowser::_on_forward() {
	if (history_pos + 1 < (int)history.size()) {
		history_pos++;
		_navigate(history[history_pos], false);
	}
}

void HewnTextureBrowser::_on_up() {
	_navigate(parent_of(dir), true);
}

void HewnTextureBrowser::_on_path_entered(const String &p_text) {
	_navigate(p_text, true);
}

void HewnTextureBrowser::_on_search_changed(const String &p_text) {
	_refresh();
}

void HewnTextureBrowser::_on_thumb_size(double p_value) {
	const int s = (int)p_value;
	list->set_fixed_icon_size(Vector2i(s, s));
	list->set_fixed_column_width(s + 16);
}

void HewnTextureBrowser::_refresh() {
	if (!list) {
		return;
	}
	generation++;
	path_edit->set_text(dir);
	back_button->set_disabled(history_pos <= 0);
	forward_button->set_disabled(history_pos + 1 >= (int)history.size());
	up_button->set_disabled(dir == "res://");
	_on_thumb_size(thumb_size->get_value());
	list->clear();
	const String q = search->get_text().strip_edges().to_lower();
	std::vector<String> folders, images, mats;
	if (q.is_empty() && !subfolders->is_pressed()) {
		const PackedStringArray ds = DirAccess::get_directories_at(dir);
		for (int64_t i = 0; i < ds.size(); i++) {
			if (!ds[i].begins_with(".")) {
				folders.push_back(join_path(dir, ds[i]));
			}
		}
		const PackedStringArray fs = DirAccess::get_files_at(dir);
		for (int64_t i = 0; i < fs.size(); i++) {
			if (is_image(fs[i])) {
				images.push_back(join_path(dir, fs[i]));
			} else if (is_material_file(join_path(dir, fs[i]))) {
				mats.push_back(join_path(dir, fs[i]));
			}
		}
	} else {
		// this folder and below, by name
		std::vector<String> stack = { dir };
		while (!stack.empty() && images.size() < 1000) {
			const String d = stack.back();
			stack.pop_back();
			const PackedStringArray ds = DirAccess::get_directories_at(d);
			for (int64_t i = 0; i < ds.size(); i++) {
				if (!ds[i].begins_with(".")) {
					stack.push_back(join_path(d, ds[i]));
				}
			}
			const PackedStringArray fs = DirAccess::get_files_at(d);
			for (int64_t i = 0; i < fs.size(); i++) {
				if (!q.is_empty() && !fs[i].to_lower().contains(q)) {
					continue;
				}
				if (is_image(fs[i])) {
					images.push_back(join_path(d, fs[i]));
				} else if (is_material_file(join_path(d, fs[i]))) {
					mats.push_back(join_path(d, fs[i]));
				}
			}
		}
	}
	const Ref<Texture2D> folder_icon = theme_icon("Folder");
	const Ref<Texture2D> wait_icon = theme_icon("ImageTexture");
	for (const String &f : folders) {
		const int i = list->add_item(f.get_file(), folder_icon);
		list->set_item_metadata(i, f);
		list->set_item_tooltip(i, f);
	}
	EditorResourcePreview *rp = EditorInterface::get_singleton()->get_resource_previewer();
	for (const String &f : images) {
		// (used by more than one material: how many, the list on a right click)
		const int uses = _uses_of(f);
		const int i = list->add_item(f.get_file().get_basename() + (uses > 1 ? String::utf8("  \u25C6") + itos(uses) : String()), wait_icon);
		list->set_item_metadata(i, f);
		list->set_item_tooltip(i, f + String("\n") + T(String("Click: put it on the selection")) +
						(uses > 0 ? String("\n") + vformat(T("Used by %d materials (right click: their list)"), uses) : String()));
		if (rp) {
			Array ud;
			ud.push_back((int64_t)generation);
			ud.push_back(i);
			rp->queue_resource_preview(f, this, "_on_preview", ud);
		}
	}
	// the project's own materials (a shader, a color...): after the images, applied the same way
	const Ref<Texture2D> mat_icon = theme_icon("StandardMaterial3D");
	for (const String &f : mats) {
		const int i = list->add_item(f.get_file().get_basename(), mat_icon);
		list->set_item_metadata(i, f);
		list->set_item_tooltip(i, f + String("\n") + T(String("Click: put it on the selection")));
		if (rp) {
			Array ud;
			ud.push_back((int64_t)generation);
			ud.push_back(i);
			rp->queue_resource_preview(f, this, "_on_preview", ud);
		}
	}
	if (images.empty() && folders.empty() && mats.empty()) {
		set_status(q.is_empty() ? T("No images in this folder") : T("Nothing found"));
	} else {
		set_status(vformat(T("%d textures. Click one: edit mode - the selected faces, object mode - the selected objects."), (int)images.size()));
	}
}

void HewnTextureBrowser::_on_preview(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_thumb, const Variant &p_userdata) {
	const Array ud = p_userdata;
	if (ud.size() < 2 || (uint64_t)(int64_t)ud[0] != generation || p_preview.is_null()) {
		return;
	}
	const int i = ud[1];
	if (i >= 0 && i < list->get_item_count() && String(list->get_item_metadata(i)) == p_path) {
		list->set_item_icon(i, p_preview);
	}
}

void HewnTextureBrowser::_on_item_clicked(int p_index, const Vector2 &p_at, int p_button) {
	const String path = list->get_item_metadata(p_index);
	if (p_button == MOUSE_BUTTON_XBUTTON1) {
		_on_back();
		return;
	}
	if (p_button == MOUSE_BUTTON_XBUTTON2) {
		_on_forward();
		return;
	}
	if (DirAccess::dir_exists_absolute(path)) {
		if (p_button == MOUSE_BUTTON_LEFT) {
			_navigate(path, true);
		}
		return;
	}
	if (p_button == MOUSE_BUTTON_RIGHT) {
		tex_menu_path = path;
		tex_menu->clear();
		if (is_image(path)) {
			tex_menu->add_item(vformat(T("Materials Using It (%d)..."), _uses_of(path)), 0);
		}
		tex_menu->add_item(T("Show in FileSystem"), 1);

		tex_menu->set_position(Vector2i(list->get_screen_position() + p_at));
		tex_menu->reset_size();
		tex_menu->popup();
		return;
	}
	if (p_button == MOUSE_BUTTON_LEFT) {
		click_pending = path; // (on at the release: a drag puts it where it is dropped instead)
	}
}

void HewnTextureBrowser::_on_list_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && !mb->is_pressed() && !click_pending.is_empty()) {
		const String p = click_pending;
		click_pending = String();
		click_image(p);
	}
}

Variant HewnTextureBrowser::_list_drag(const Vector2 &p_at) {
	click_pending = String();
	const int i = list->get_item_at_position(p_at, true);
	if (i < 0) {
		return Variant();
	}
	const String path = list->get_item_metadata(i);
	if (DirAccess::dir_exists_absolute(path) || (!is_image(path) && !is_material_file(path))) {
		return Variant();
	}
	TextureRect *tr = memnew(TextureRect);
	tr->set_texture(list->get_item_icon(i));
	tr->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	tr->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	tr->set_size(Vector2(48, 48) * EditorInterface::get_singleton()->get_editor_scale());
	tr->set_modulate(Color(1, 1, 1, 0.8f));
	list->set_drag_preview(tr);
	drag_path = path;
	Dictionary d;
	d["type"] = "hewn_texture"; // (not "files": the 3D view would make its own material from it)
	d["path"] = path;
	return d;
}

void HewnTextureBrowser::drop_image(const String &p_path, const Vector2 &p_screen) {
	if (ask_material_dir(callable_mp(this, &HewnTextureBrowser::drop_image).bind(p_path, p_screen))) {
		return;
	}
	Ref<Material> mat = is_image(p_path) ? material_for_image(p_path) : Ref<Material>(ResourceLoader::get_singleton()->load(p_path));
	if (mat.is_null()) {
		set_status(vformat(T("Could not load %s (not imported yet?)"), p_path));
		return;
	}
	const int n = plugin ? plugin->apply_material_at_screen(mat, p_screen) : -1;
	if (n < 0) {
		return; // (not over an object in a 3D view)
	}
	if (plugin->dock && is_image(p_path)) {
		plugin->dock->set_decal_texture(p_path);
	}
	set_current(mat, p_path.get_file().get_basename());
	set_status(vformat(T("%s: %d faces"), p_path.get_file(), n));
}

void HewnTextureBrowser::set_current(const Ref<Material> &p_material, const String &p_name) {
	current_material = p_material;
	if (current_button) {
		current_button->set_visible(p_material.is_valid());
		current_button->set_text(String::utf8("\u2714 ") + p_name + String::utf8("  \u2715"));
	}
}

void HewnTextureBrowser::_on_clear_current() {
	if (list) {
		list->deselect_all(); // (no texture picked any more: none shown as picked)
	}
	set_current(Ref<Material>(), String());
}

void HewnTextureBrowser::click_image(const String &p_path) {
	if (ask_material_dir(callable_mp(this, &HewnTextureBrowser::click_image).bind(p_path))) {
		return;
	}
	const bool own = !is_image(p_path); // (a material file of the project's)
	if (plugin && plugin->dock && !own) {
		plugin->dock->set_decal_texture(p_path); // (the picture for decals / overlays too)
	}
	Ref<Material> mat = own ? Ref<Material>(ResourceLoader::get_singleton()->load(p_path)) : material_for_image(p_path);
	if (mat.is_null()) {
		set_status(vformat(T("Could not load %s (not imported yet?)"), p_path));
		return;
	}
	set_current(mat, p_path.get_file().get_basename());
	const int n = plugin ? plugin->apply_material_to_selection(mat) : -1;
	if (n < 0) {
		set_status(T("Select objects, or faces in edit mode, first."));
	} else {
		set_status(vformat(T("%s: %d faces"), p_path.get_file(), n));
	}
}

namespace {
const char *SETTING_MATERIALS = "hewn/paths/materials";
const char *SETTING_BAKED = "hewn/paths/baked_scenes";

void add_dir_setting(const char *p_name) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps->has_setting(p_name)) {
		ps->set_setting(p_name, String());
	}
	ps->set_initial_value(p_name, String());
	ps->set_as_basic(p_name, true);
	Dictionary info;
	info["name"] = p_name;
	info["type"] = Variant::STRING;
	info["hint"] = PROPERTY_HINT_DIR;
	info["hint_string"] = "";
	ps->add_property_info(info);
}
} // namespace

// Project Settings > Hewn > Paths: where the materials made by Hewn go (empty: asked the first time
// one is made), and the folder Bake Level / the glTF exports open in.
void HewnTextureBrowser::register_settings() {
	add_dir_setting(SETTING_MATERIALS);
	add_dir_setting(SETTING_BAKED);
}

String HewnTextureBrowser::material_dir() const {
	const String set = ProjectSettings::get_singleton()->get_setting(SETTING_MATERIALS, String());
	if (!set.is_empty()) {
		return set;
	}
	String d = "res://materials";
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid()) {
		d = es->get_project_metadata("hewn", "material_dir", d); // (chosen before the setting was there)
	}
	return d;
}

bool HewnTextureBrowser::ask_material_dir(const Callable &p_then) {
	const String set = ProjectSettings::get_singleton()->get_setting(SETTING_MATERIALS, String());
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (!set.is_empty() || (es.is_valid() && !String(es->get_project_metadata("hewn", "material_dir", String())).is_empty()) ||
			!OS::get_singleton()->get_environment("HW_SELFTEST").is_empty()) {
		return false; // (known; the editor tests: the default folder)
	}
	material_dir_then = p_then;
	material_dir_dialog->set_title(T("Where should Hewn save its materials?"));
	material_dir_dialog->set_current_dir("res://");
	material_dir_dialog->popup_file_dialog();
	return true;
}

void HewnTextureBrowser::_on_material_dir_button() {
	material_dir_then = Callable();
	material_dir_dialog->set_title(T("Materials Folder"));
	material_dir_dialog->set_current_dir(material_dir());
	material_dir_dialog->popup_file_dialog();
}

void HewnTextureBrowser::_on_material_dir(const String &p_dir) {
	ProjectSettings::get_singleton()->set_setting(SETTING_MATERIALS, p_dir);
	ProjectSettings::get_singleton()->save();
	set_status(vformat(T("Materials go to %s"), p_dir));
	if (material_dir_then.is_valid()) {
		const Callable then = material_dir_then;
		material_dir_then = Callable();
		then.call_deferred();
	}
}

Ref<Material> HewnTextureBrowser::material_for_image(const String &p_image) {
	ResourceLoader *rl = ResourceLoader::get_singleton();
	const String mdir = material_dir();
	// mapped by Fix Material Mapping (a material anywhere in the project)
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid()) {
		const Dictionary map = es->get_project_metadata("hewn", "image_materials", Dictionary());
		const String mp = map.get(p_image, String());
		if (!mp.is_empty() && FileAccess::file_exists(mp)) {
			Ref<Material> m = rl->load(mp);
			if (m.is_valid() && m->has_meta(META_IMAGE) && String(m->get_meta(META_IMAGE)) == p_image && !m->has_meta("hw_blend_material")) {
				return m;
			}
		}
	}
	// a material made for this image before (by its metadata: it may have been renamed)
	if (DirAccess::dir_exists_absolute(mdir)) {
		const PackedStringArray fs = DirAccess::get_files_at(mdir);
		const String base = p_image.get_file().get_basename();
		// (the file named after the image first: usually it is that one)
		std::vector<String> order;
		for (int64_t i = 0; i < fs.size(); i++) {
			const String f = fs[i];
			if (f.get_extension() == "tres" || f.get_extension() == "res" || f.get_extension() == "material") {
				if (f.get_basename() == base) {
					order.insert(order.begin(), join_path(mdir, f));
				} else {
					order.push_back(join_path(mdir, f));
				}
			}
		}
		for (const String &f : order) {
			Ref<Material> m = rl->load(f);
			// (a blend material is never the one an image click gives: it shows several)
			if (m.is_valid() && m->has_meta(META_IMAGE) && String(m->get_meta(META_IMAGE)) == p_image && !m->has_meta("hw_blend_material")) {
				return m;
			}
		}
	}
	Ref<Texture2D> tex = rl->load(p_image);
	if (tex.is_null()) {
		return Ref<Material>();
	}
	// a copy of the default material (Material tab: Edit Default Material), with this texture
	Ref<StandardMaterial3D> m;
	Ref<StandardMaterial3D> tpl = default_material(false);
	if (tpl.is_valid()) {
		m = tpl->duplicate();
		m->remove_meta(META_IMAGE);
	} else {
		m.instantiate();
	}
	m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, tex);
	m->set_texture_filter(nearest->is_pressed() ? BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS : BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS);
	Ref<Image> img = tex->get_image();
	if (img.is_valid() && img->detect_alpha() != Image::ALPHA_NONE && m->get_transparency() == BaseMaterial3D::TRANSPARENCY_DISABLED) {
		m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR); // (foliage, fences: cut-out)
	}
	m->set_meta(META_IMAGE, p_image);
	_apply_density(m);
	DirAccess::make_dir_recursive_absolute(mdir);
	String file = join_path(mdir, p_image.get_file().get_basename() + ".tres");
	for (int k = 2; FileAccess::file_exists(file) && k < 1000; k++) {
		file = join_path(mdir, p_image.get_file().get_basename() + "_" + String::num_int64(k) + ".tres");
	}
	if (ResourceSaver::get_singleton()->save(m, file) != OK) {
		return m; // (unsaved: lives in the scene)
	}
	if (EditorFileSystem *efs = EditorInterface::get_singleton()->get_resource_filesystem()) {
		efs->update_file(file);
	}
	Ref<Material> saved = rl->load(file);
	return saved.is_valid() ? saved : Ref<Material>(m);
}

String HewnTextureBrowser::texture_of(const Ref<Material> &p_material) {
	Ref<BaseMaterial3D> bm = p_material;
	if (bm.is_valid()) {
		Ref<Texture2D> t = bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
		return t.is_valid() ? t->get_path() : String();
	}
	Ref<ShaderMaterial> sm = p_material;
	if (sm.is_valid() && sm->get_shader().is_valid()) {
		// a shader material (code or visual shader): its first texture parameter
		const Array us = sm->get_shader()->get_shader_uniform_list();
		for (int64_t i = 0; i < us.size(); i++) {
			const Dictionary u = us[i];
			Ref<Texture2D> t = sm->get_shader_parameter(u.get("name", ""));
			if (t.is_valid() && !t->get_path().is_empty()) {
				return t->get_path();
			}
		}
	}
	return String();
}

String HewnTextureBrowser::image_of(const Ref<Material> &p_material) {
	if (p_material.is_null()) {
		return String();
	}
	if (p_material->has_meta(META_IMAGE)) {
		return p_material->get_meta(META_IMAGE);
	}
	return texture_of(p_material);
}

void HewnTextureBrowser::map_material(const Ref<Material> &p_material, const String &p_image) {
	if (p_material.is_null() || p_image.is_empty()) {
		return;
	}
	p_material->set_meta(META_IMAGE, p_image);
	const String path = p_material->get_path();
	if (path.begins_with("res://") && !path.contains("::")) {
		ResourceSaver::get_singleton()->save(p_material, path);
		Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
		if (es.is_valid()) {
			Dictionary map = es->get_project_metadata("hewn", "image_materials", Dictionary());
			map[p_image] = path;
			es->set_project_metadata("hewn", "image_materials", map);
		}
	}
}

// Every material file of the project that shows an image but is not mapped (made by hand, by a script,
// a shader material, from an older version) gets mapped to it; an image already mapped keeps its
// material. Returns how many were mapped.
int HewnTextureBrowser::fix_material_mapping() {
	std::vector<String> files;
	std::vector<String> dirs = { "res://" };
	while (!dirs.empty()) {
		const String d = dirs.back();
		dirs.pop_back();
		const PackedStringArray sub = DirAccess::get_directories_at(d);
		for (int64_t i = 0; i < sub.size(); i++) {
			if (!String(sub[i]).begins_with(".") && sub[i] != "addons") {
				dirs.push_back(join_path(d, sub[i]));
			}
		}
		const PackedStringArray fs = DirAccess::get_files_at(d);
		for (int64_t i = 0; i < fs.size(); i++) {
			const String e = String(fs[i]).get_extension();
			if (e == "tres" || e == "res" || e == "material") {
				files.push_back(join_path(d, fs[i]));
			}
		}
	}
	ResourceLoader *rl = ResourceLoader::get_singleton();
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	Dictionary map = es.is_valid() ? Dictionary(es->get_project_metadata("hewn", "image_materials", Dictionary())) : Dictionary();
	int n = 0;
	const String tpl = default_material_path();
	for (const String &f : files) {
		if (f == tpl || !rl->exists(f, "Material")) {
			continue;
		}
		Ref<Material> m = rl->load(f);
		if (m.is_null()) {
			continue;
		}
		if (m->has_meta("hw_blend_material")) {
			// several textures: not any one image's material (mapped by an older version: unmapped)
			if (m->has_meta(META_IMAGE)) {
				const String img = m->get_meta(META_IMAGE);
				if (String(map.get(img, String())) == f) {
					map.erase(img);
				}
				m->remove_meta(META_IMAGE);
				ResourceSaver::get_singleton()->save(m, f);
			}
			continue;
		}
		if (m->has_meta(META_IMAGE)) {
			map[String(m->get_meta(META_IMAGE))] = f; // (made here: known)
			continue;
		}
		const String img = texture_of(m);
		if (img.is_empty() || img.contains("::") || map.has(img)) {
			continue;
		}
		m->set_meta(META_IMAGE, img);
		ResourceSaver::get_singleton()->save(m, f);
		map[img] = f;
		n++;
	}
	if (es.is_valid()) {
		es->set_project_metadata("hewn", "image_materials", map);
	}
	set_status(vformat(T("Material mapping: %d materials mapped to their image"), n));
	return n;
}

String HewnTextureBrowser::default_material_path() const {
	return join_path(material_dir(), "_default_material.tres");
}

Ref<Material> HewnTextureBrowser::default_material(bool p_create) {
	const String path = default_material_path();
	if (FileAccess::file_exists(path)) {
		return ResourceLoader::get_singleton()->load(path);
	}
	if (!p_create) {
		return Ref<Material>();
	}
	Ref<StandardMaterial3D> m;
	m.instantiate();
	m->set_texture_filter(get_default_nearest() ? BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS : BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS);
	DirAccess::make_dir_recursive_absolute(material_dir());
	if (ResourceSaver::get_singleton()->save(m, path) != OK) {
		return m;
	}
	if (EditorFileSystem *efs = EditorInterface::get_singleton()->get_resource_filesystem()) {
		efs->update_file(path);
	}
	return ResourceLoader::get_singleton()->load(path);
}

bool HewnTextureBrowser::get_default_nearest() const {
	return nearest && nearest->is_pressed();
}

void HewnTextureBrowser::set_default_nearest(bool p_on) {
	if (nearest) {
		nearest->set_pressed(p_on);
	}
}

float HewnTextureBrowser::pixels_per_meter() const {
	return density ? (float)density->get_value() : 128.0f;
}

// Texel density: face UVs are in meters, the material scales them to its image size.
void HewnTextureBrowser::_apply_density(const Ref<Material> &p_material) const {
	Ref<BaseMaterial3D> m = p_material;
	if (m.is_null()) {
		return;
	}
	Ref<Texture2D> tex = m->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
	const float ppm = pixels_per_meter();
	if (tex.is_null() || ppm <= 0.0f || tex->get_width() <= 0 || tex->get_height() <= 0) {
		m->set_uv1_scale(Vector3(1, 1, 1));
		return;
	}
	m->set_uv1_scale(Vector3(ppm / tex->get_width(), ppm / tex->get_height(), 1.0f));
}

void HewnTextureBrowser::_on_subfolders(bool p_on) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid() && settings_on()) {
		es->set_project_metadata("hewn", "texture_subfolders", p_on);
	}
	_refresh();
}

void HewnTextureBrowser::_on_nearest(bool p_on) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid() && settings_on()) {
		es->set_project_metadata("hewn", "default_nearest", p_on);
	}
}

void HewnTextureBrowser::_on_density(double p_value) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid() && settings_on()) {
		es->set_project_metadata("hewn", "pixels_per_meter", p_value);
	}
	// the materials made for textures follow
	const String mdir = material_dir();
	if (!DirAccess::dir_exists_absolute(mdir)) {
		return;
	}
	int n = 0;
	const PackedStringArray fs = DirAccess::get_files_at(mdir);
	for (int64_t i = 0; i < fs.size(); i++) {
		const String f = join_path(mdir, fs[i]);
		if (f.get_extension() != "tres") {
			continue;
		}
		Ref<Material> m = ResourceLoader::get_singleton()->load(f);
		if (m.is_valid() && m->has_meta(META_IMAGE)) {
			_apply_density(m);
			ResourceSaver::get_singleton()->save(m, f);
			n++;
		}
	}
	set_status(vformat(T("%d materials updated to %d px/m"), n, (int)p_value));
}

void HewnTextureBrowser::set_status(const String &p_text) {
	if (status) {
		status->set_text(p_text);
	}
}

void HewnTextureBrowser::_update_favorites() {
	PopupMenu *pm = favorites->get_popup();
	pm->clear();
	pm->add_item(T("Add this folder"), 0);
	pm->add_item(T("Remove this folder"), 1);
	pm->add_separator();
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	const PackedStringArray favs = es.is_valid() ? PackedStringArray(es->get_project_metadata("hewn", "texture_favorites", PackedStringArray())) : PackedStringArray();
	for (int64_t i = 0; i < favs.size(); i++) {
		pm->add_item(favs[i], 100 + (int)i);
	}
}

void HewnTextureBrowser::_on_favorite(int p_id) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_null()) {
		return;
	}
	PackedStringArray favs = es->get_project_metadata("hewn", "texture_favorites", PackedStringArray());
	if (p_id == 0 && !favs.has(dir)) {
		favs.push_back(dir);
	} else if (p_id == 1 && favs.has(dir)) {
		favs.remove_at(favs.find(dir));
	} else if (p_id >= 100 && p_id - 100 < favs.size()) {
		_navigate(favs[p_id - 100], true);
		return;
	}
	if (settings_on()) {
		es->set_project_metadata("hewn", "texture_favorites", favs);
	}
	_update_favorites();
}

void HewnTextureBrowser::_on_new_texture() {
	new_dialog->popup_centered(Vector2i(320, 0));
}

// A placeholder texture: a color with a grid (blockout / dev textures).
void HewnTextureBrowser::_on_new_confirmed() {
	String name = new_name->get_text().strip_edges().validate_filename();
	if (name.is_empty()) {
		name = "texture";
	}
	const int size = new_size->get_selected_id();
	const Color c = new_color->get_pick_color();
	Ref<Image> img = Image::create_empty(size, size, false, Image::FORMAT_RGBA8);
	img->fill(c);
	if (new_grid->is_pressed()) {
		const Color minor = c.darkened(0.12f), major = c.darkened(0.3f);
		const int step = MAX(4, size / 16);
		for (int i = 0; i < size; i += step) {
			const bool big = i % (step * 4) == 0;
			const int w = big ? MAX(1, size / 128) : MAX(1, size / 256);
			for (int t = 0; t < w; t++) {
				for (int k = 0; k < size; k++) {
					img->set_pixel(i + t, k, big ? major : minor);
					img->set_pixel(k, i + t, big ? major : minor);
				}
			}
		}
	}
	String file = join_path(dir, name + ".png");
	for (int k = 2; FileAccess::file_exists(file) && k < 1000; k++) {
		file = join_path(dir, name + "_" + String::num_int64(k) + ".png");
	}
	if (img->save_png(file) != OK) {
		set_status(vformat(T("Could not save %s"), file));
		return;
	}
	if (EditorFileSystem *efs = EditorInterface::get_singleton()->get_resource_filesystem()) {
		efs->scan(); // (imports it)
	}
	set_status(vformat(T("Made %s"), file));
}
