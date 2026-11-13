#include "hw_prefabs.h"

#include "hw_editor.h"
#include <godot_cpp/classes/texture_rect.hpp>
#include "hw_editor_util.h"

#include <godot_cpp/classes/h_flow_container.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/popup_panel.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_resource_preview.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/v_separator.hpp>

using namespace godot;
using namespace hw_editor;

namespace {

Ref<Texture2D> theme_icon(const char *p_name) {
	return EditorInterface::get_singleton()->get_editor_theme()->get_icon(p_name, "EditorIcons");
}

String join_path(const String &p_dir, const String &p_name) {
	return p_dir.ends_with("/") ? p_dir + p_name : p_dir + String("/") + p_name;
}

float rand01(uint32_t &r_seed) {
	r_seed = r_seed * 1664525u + 1013904223u;
	return (float)(r_seed >> 8) / (float)(1u << 24);
}

// Ownership for a placed scene: the instance root (its insides belong to the scene file), or everything
// when it is made local.
void own(Node *p_node, Node *p_root, bool p_all) {
	p_node->set_owner(p_root);
	if (!p_all) {
		return;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		own(p_node->get_child(i), p_root, true);
	}
}

} // namespace

// ------------------------------------------------------------------ browser

void HewnPrefabBrowser::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_preview", "path", "preview", "thumbnail", "userdata"), &HewnPrefabBrowser::_on_preview);
	ClassDB::bind_method(D_METHOD("place", "path"), &HewnPrefabBrowser::place); // (tests)
}

bool HewnPrefabBrowser::is_scene(const String &p_path) {
	const String e = p_path.get_extension().to_lower();
	return e == "tscn" || e == "scn" || e == "glb" || e == "gltf" || e == "fbx" || e == "blend" || e == "obj";
}

void HewnPrefabBrowser::setup(HewnEditorPlugin *p_plugin) {
	plugin = p_plugin;
	set_name("HewnPrefabs");
	set_title(T("Scenes"));
	set_default_slot(EditorDock::DOCK_SLOT_BOTTOM);
	VBoxContainer *v = memnew(VBoxContainer);
	v->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	add_child(v);
	HFlowContainer *bar = memnew(HFlowContainer); // (wraps: a narrow bottom panel does not push the side docks)
	v->add_child(bar);
	Button *up = memnew(Button);
	up->set_button_icon(theme_icon("ArrowUp"));
	up->set_flat(true);
	up->set_tooltip_text("Parent folder");
	up->connect("pressed", callable_mp(this, &HewnPrefabBrowser::_on_up));
	bar->add_child(up);
	path_edit = memnew(LineEdit);
	path_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	path_edit->set_custom_minimum_size(Vector2(160, 0));
	path_edit->connect("text_submitted", callable_mp(this, &HewnPrefabBrowser::_on_path));
	bar->add_child(path_edit);
	search = memnew(LineEdit);
	search->set_placeholder("Search (this folder and below)");
	search->set_custom_minimum_size(Vector2(160, 0));
	search->set_clear_button_enabled(true);
	search->connect("text_changed", callable_mp(this, &HewnPrefabBrowser::_on_search));
	bar->add_child(search);
	subfolders = memnew(CheckBox);
	subfolders->set_text("Subfolders");
	subfolders->set_tooltip_text("The scenes of all subfolders at once (no need to open them)");
	subfolders->connect("toggled", callable_mp(this, &HewnPrefabBrowser::_on_search).bind(String()).unbind(1));
	bar->add_child(subfolders);
	placement_button = memnew(Button);
	placement_button->set_name("PlacementButton");
	placement_button->set_text("Placement...");
	placement_button->set_tooltip_text("How the scenes are placed: along the surface, random rotation / size per axis, name suffix, local copies");
	placement_button->connect("pressed", callable_mp(this, &HewnPrefabBrowser::_on_placement));
	bar->add_child(placement_button);

	// the placement options: a popup under the button
	placement_popup = memnew(PopupPanel);
	placement_popup->set_name("PlacementPopup");
	add_child(placement_popup);
	VBoxContainer *pv = memnew(VBoxContainer);
	placement_popup->add_child(pv);
	inherit_normal = memnew(CheckBox);
	inherit_normal->set_text("Along the surface");
	inherit_normal->set_pressed(true);
	inherit_normal->set_tooltip_text("Placed along the normal of the surface under the mouse; off: always upright (N while placing)");
	pv->add_child(inherit_normal);
	auto axes_row = [&](GridContainer *p_grid, const char *p_label, SpinBox **r_boxes, double p_min, double p_max, double p_step, double p_value) {
		Label *l = memnew(Label);
		l->set_text(p_label);
		p_grid->add_child(l);
		HBoxContainer *h = memnew(HBoxContainer);
		p_grid->add_child(h);
		static const char *names[3] = { "x", "y", "z" };
		for (int a = 0; a < 3; a++) {
			SpinBox *s = memnew(SpinBox);
			HewnDragField::hook_spinbox(s);
			s->set_min(p_min);
			s->set_max(p_max);
			s->set_step(p_step);
			s->set_value(p_value);
			s->set_allow_greater(true);
			s->set_allow_lesser(true);
			s->set_prefix(names[a]);
			s->set_custom_minimum_size(Vector2(84, 0));
			s->connect("value_changed", callable_mp(this, &HewnPrefabBrowser::_on_setting_changed).unbind(1));
			h->add_child(s);
			r_boxes[a] = s;
		}
	};
	auto head = [&](CheckBox **r_check, const char *p_text, const char *p_tip, bool p_size) {
		HBoxContainer *h = memnew(HBoxContainer);
		pv->add_child(h);
		*r_check = memnew(CheckBox);
		(*r_check)->set_text(p_text);
		(*r_check)->set_tooltip_text(p_tip);
		(*r_check)->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		(*r_check)->connect("toggled", callable_mp(this, &HewnPrefabBrowser::_on_setting_changed).unbind(1));
		h->add_child(*r_check);
		Button *reset = memnew(Button);
		reset->set_button_icon(theme_icon("Reload"));
		reset->set_flat(true);
		reset->set_tooltip_text("Back to no randomness");
		reset->connect("pressed", callable_mp(this, &HewnPrefabBrowser::_on_reset_random).bind(p_size));
		h->add_child(reset);
		GridContainer *g = memnew(GridContainer);
		g->set_columns(2);
		pv->add_child(g);
		return g;
	};
	GridContainer *rg = head(&random_rotation, "Random Rotation", "Each placed scene turns by a random angle between Min and Max around its own axes, in degrees (R while placing: another one)", false);
	axes_row(rg, "Min", rotation_min, -360, 360, 1, 0);
	axes_row(rg, "Max", rotation_max, -360, 360, 1, 0);
	GridContainer *sg = head(&random_size, "Random Size", "Each placed scene gets a random scale between Min and Max per axis", true);
	axes_row(sg, "Min", size_min, 0.001, 1000, 0.01, 1);
	axes_row(sg, "Max", size_max, 0.001, 1000, 0.01, 1);
	size_uniform = memnew(CheckBox);
	size_uniform->set_text("Uniform (one factor, from X)");
	size_uniform->set_pressed(true);
	size_uniform->set_tooltip_text("The same random factor on all axes, keeping the proportions (X of Min / Max)");
	size_uniform->connect("toggled", callable_mp(this, &HewnPrefabBrowser::_on_setting_changed).unbind(1));
	pv->add_child(size_uniform);
	HBoxContainer *nh = memnew(HBoxContainer);
	pv->add_child(nh);
	suffix = memnew(LineEdit);
	suffix->set_placeholder("Name suffix");
	suffix->set_tooltip_text("Added to the name of placed objects (e.g. -col: Godot's import makes colliders for such names)");
	suffix->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	nh->add_child(suffix);
	make_local = memnew(CheckBox);
	make_local->set_text("Make local");
	make_local->set_tooltip_text("Placed as a copy that belongs to this scene, not a link to the prefab scene");
	nh->add_child(make_local);
	for (CheckBox *c : { inherit_normal, make_local }) {
		c->connect("toggled", callable_mp(this, &HewnPrefabBrowser::_on_setting_changed).unbind(1));
	}
	suffix->connect("text_changed", callable_mp(this, &HewnPrefabBrowser::_on_setting_changed).unbind(1));
	// the settings of the last session (this project)
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	const Dictionary st = es.is_valid() ? Dictionary(es->get_project_metadata("hewn", "prefab_settings", Dictionary())) : Dictionary();
	loading_settings = true;
	inherit_normal->set_pressed(st.get("inherit_normal", true));
	random_rotation->set_pressed(st.get("random_rotation", false));
	random_size->set_pressed(st.get("random_size", false));
	size_uniform->set_pressed(st.get("size_uniform", true));
	make_local->set_pressed(st.get("make_local", false));
	subfolders->set_pressed_no_signal(st.get("subfolders", false));
	suffix->set_text(st.get("suffix", String()));
	for (int a = 0; a < 3; a++) {
		const Vector3 rmin = st.get("rotation_min", Vector3()), rmax = st.get("rotation_max", Vector3());
		const Vector3 smin = st.get("size_min", Vector3(1, 1, 1)), smax = st.get("size_max", Vector3(1, 1, 1));
		rotation_min[a]->set_value(rmin[a]);
		rotation_max[a]->set_value(rmax[a]);
		size_min[a]->set_value(smin[a]);
		size_max[a]->set_value(smax[a]);
	}
	loading_settings = false;

	list = memnew(ItemList);
	list->set_name("PrefabList");
	list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	list->set_max_columns(0);
	list->set_icon_mode(ItemList::ICON_MODE_TOP);
	list->set_same_column_width(true);
	list->set_fixed_icon_size(Vector2i(72, 72));
	list->set_fixed_column_width(88);
	list->set_max_text_lines(2);
	list->set_custom_minimum_size(Vector2(0, 120));
	list->connect("item_clicked", callable_mp(this, &HewnPrefabBrowser::_on_item_clicked));
	list->set_drag_forwarding(callable_mp(this, &HewnPrefabBrowser::_list_drag), Callable(), Callable());
	v->add_child(list);
	status = memnew(Label);
	status->set_clip_text(true);
	v->add_child(status);
	if (EditorFileSystem *fs = EditorInterface::get_singleton()->get_resource_filesystem(); fs && !hw_editor::cmdline_export()) {
		fs->connect("filesystem_changed", callable_mp(this, &HewnPrefabBrowser::_refresh), CONNECT_DEFERRED);
	}
	hw_editor::localize(this);
	_navigate(dir);
}

void HewnPrefabBrowser::_navigate(const String &p_dir) {
	if (DirAccess::dir_exists_absolute(p_dir)) {
		dir = p_dir;
	}
	_refresh();
}

void HewnPrefabBrowser::_on_placement() {
	placement_popup->reset_size();
	const Vector2 at = placement_button->get_screen_position() + Vector2(0, placement_button->get_size().y);
	// (above the button when the bottom panel is low on the screen)
	const Vector2 sz = placement_popup->get_contents_minimum_size();
	Vector2 pos = at;
	if (Viewport *vp = get_viewport()) {
		const float limit = vp->get_visible_rect().size.y;
		if (pos.y + sz.y > limit) {
			pos.y = placement_button->get_screen_position().y - sz.y - 8;
		}
	}
	placement_popup->popup(Rect2i(Vector2i(pos), Vector2i()));
}

void HewnPrefabBrowser::_on_reset_random(bool p_size) {
	for (int a = 0; a < 3; a++) {
		(p_size ? size_min : rotation_min)[a]->set_value(p_size ? 1.0 : 0.0);
		(p_size ? size_max : rotation_max)[a]->set_value(p_size ? 1.0 : 0.0);
	}
	(p_size ? random_size : random_rotation)->set_pressed(false);
}

void HewnPrefabBrowser::_on_setting_changed() {
	if (loading_settings) {
		return;
	}
	_save_settings();
	if (plugin) {
		plugin->prefab_reroll(); // (the scene being placed shows the new settings)
	}
}

void HewnPrefabBrowser::_save_settings() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_null()) {
		return;
	}
	Dictionary st;
	st["inherit_normal"] = inherit_normal->is_pressed();
	st["random_rotation"] = random_rotation->is_pressed();
	st["random_size"] = random_size->is_pressed();
	st["size_uniform"] = size_uniform->is_pressed();
	st["make_local"] = make_local->is_pressed();
	st["subfolders"] = subfolders->is_pressed();
	st["suffix"] = suffix->get_text();
	auto vec = [](SpinBox *const *p_boxes) { return Vector3(p_boxes[0]->get_value(), p_boxes[1]->get_value(), p_boxes[2]->get_value()); };
	st["rotation_min"] = vec(rotation_min);
	st["rotation_max"] = vec(rotation_max);
	st["size_min"] = vec(size_min);
	st["size_max"] = vec(size_max);
	es->set_project_metadata("hewn", "prefab_settings", st);
}

Vector3 HewnPrefabBrowser::random_rotation_value(uint32_t &r_seed) const {
	Vector3 r;
	if (random_rotation && random_rotation->is_pressed()) {
		for (int a = 0; a < 3; a++) {
			const float lo = (float)rotation_min[a]->get_value(), hi = (float)rotation_max[a]->get_value();
			r[a] = lo + (hi - lo) * rand01(r_seed);
		}
	}
	return r;
}

Vector3 HewnPrefabBrowser::random_size_value(uint32_t &r_seed) const {
	if (!random_size || !random_size->is_pressed()) {
		return Vector3(1, 1, 1);
	}
	if (size_uniform->is_pressed()) {
		const float lo = (float)size_min[0]->get_value(), hi = (float)size_max[0]->get_value();
		const float s = lo + (hi - lo) * rand01(r_seed);
		return Vector3(s, s, s);
	}
	Vector3 r;
	for (int a = 0; a < 3; a++) {
		const float lo = (float)size_min[a]->get_value(), hi = (float)size_max[a]->get_value();
		r[a] = lo + (hi - lo) * rand01(r_seed);
	}
	return r;
}

void HewnPrefabBrowser::_on_up() {
	String d = dir.ends_with("/") && dir != "res://" ? dir.substr(0, dir.length() - 1) : dir;
	d = d == "res://" ? d : d.get_base_dir();
	_navigate(d == "res:" || d.is_empty() ? String("res://") : d);
}

void HewnPrefabBrowser::_on_path(const String &p_text) {
	_navigate(p_text.strip_edges());
}

void HewnPrefabBrowser::_on_search(const String &p_text) {
	if (!loading_settings && subfolders) {
		_save_settings();
	}
	_refresh();
}

void HewnPrefabBrowser::_refresh() {
	if (!list || !is_inside_tree()) {
		return;
	}
	generation++;
	path_edit->set_text(dir);
	list->clear();
	const String q = search->get_text().strip_edges().to_lower();
	std::vector<String> folders, scenes;
	std::vector<String> stack = { dir };
	while (!stack.empty() && scenes.size() < 1000) {
		const String d = stack.back();
		stack.pop_back();
		const PackedStringArray ds = DirAccess::get_directories_at(d);
		for (int64_t i = 0; i < ds.size(); i++) {
			if (ds[i].begins_with(".")) {
				continue;
			}
			if (q.is_empty() && !subfolders->is_pressed()) {
				folders.push_back(join_path(d, ds[i]));
			} else {
				stack.push_back(join_path(d, ds[i]));
			}
		}
		const PackedStringArray fs = DirAccess::get_files_at(d);
		for (int64_t i = 0; i < fs.size(); i++) {
			if (is_scene(fs[i]) && (q.is_empty() || fs[i].to_lower().contains(q))) {
				scenes.push_back(join_path(d, fs[i]));
			}
		}
	}
	for (const String &f : folders) {
		const int i = list->add_item(f.get_file(), theme_icon("Folder"));
		list->set_item_metadata(i, f);
	}
	EditorResourcePreview *rp = EditorInterface::get_singleton()->get_resource_previewer();
	for (const String &f : scenes) {
		const int i = list->add_item(f.get_file().get_basename(), theme_icon("PackedScene"));
		list->set_item_metadata(i, f);
		list->set_item_tooltip(i, f);
		if (rp) {
			Array ud;
			ud.push_back((int64_t)generation);
			ud.push_back(i);
			rp->queue_resource_preview(f, this, "_on_preview", ud);
		}
	}
	set_status(vformat(T("%d scenes: click one and place it, or drag it into the 3D view"), (int)scenes.size()));
}

void HewnPrefabBrowser::_on_preview(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_thumb, const Variant &p_userdata) {
	const Array ud = p_userdata;
	if (ud.size() < 2 || (uint64_t)(int64_t)ud[0] != generation || p_preview.is_null()) {
		return;
	}
	const int i = ud[1];
	if (i >= 0 && i < list->get_item_count() && String(list->get_item_metadata(i)) == p_path) {
		list->set_item_icon(i, p_preview);
	}
}

void HewnPrefabBrowser::_on_item_clicked(int p_index, const Vector2 &p_at, int p_button) {
	const String path = list->get_item_metadata(p_index);
	if (DirAccess::dir_exists_absolute(path)) {
		_navigate(path);
		return;
	}
	if (p_button == MOUSE_BUTTON_RIGHT) {
		if (!item_menu) {
			item_menu = memnew(PopupMenu);
			item_menu->connect("id_pressed", callable_mp(this, &HewnPrefabBrowser::_on_item_menu));
			add_child(item_menu);
		}
		item_menu_path = path;
		item_menu->clear();
		const bool fav = favorite_scenes().has(path);
		item_menu->add_item(hw_editor::T(fav ? "Remove from Favorites" : "Add to Favorites"), fav ? 1 : 0);
		item_menu->add_item(hw_editor::T("Show in FileSystem"), 2);
		item_menu->popup(Rect2i(Vector2i(list->get_screen_position() + p_at), Vector2i()));
		return;
	}
	place(path);
}

void HewnPrefabBrowser::_on_item_menu(int p_id) {
	if (p_id == 0 || p_id == 1) {
		set_favorite(item_menu_path, p_id == 0);
		_refresh();
	} else if (p_id == 2) {
		EditorInterface::get_singleton()->select_file(item_menu_path);
	}
}

PackedStringArray HewnPrefabBrowser::favorite_scenes() {
	PackedStringArray out;
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_null()) {
		return out;
	}
	for (const String &f : es->get_favorites()) {
		if (is_scene(f)) {
			out.push_back(f);
		}
	}
	return out;
}

void HewnPrefabBrowser::set_favorite(const String &p_path, bool p_on) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_null()) {
		return;
	}
	PackedStringArray favs = es->get_favorites();
	const int64_t at = favs.find(p_path);
	if (p_on && at < 0) {
		favs.push_back(p_path);
	} else if (!p_on && at >= 0) {
		favs.remove_at(at);
	} else {
		return;
	}
	es->set_favorites(favs);
}

// Dragged into a 3D view: Godot places it there itself (as from its FileSystem dock), the click's placing stops.
Variant HewnPrefabBrowser::_list_drag(const Vector2 &p_at) {
	const int i = list->get_item_at_position(p_at, true);
	if (i < 0) {
		return Variant();
	}
	const String path = list->get_item_metadata(i);
	if (DirAccess::dir_exists_absolute(path)) {
		return Variant();
	}
	if (plugin && plugin->modal == HewnEditorPlugin::MODAL_PREFAB) {
		plugin->cancel_tool();
	}
	TextureRect *tr = memnew(TextureRect);
	tr->set_texture(list->get_item_icon(i));
	tr->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	tr->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	tr->set_size(Vector2(48, 48) * EditorInterface::get_singleton()->get_editor_scale());
	tr->set_modulate(Color(1, 1, 1, 0.8f));
	list->set_drag_preview(tr);
	Dictionary d;
	d["type"] = "files";
	d["files"] = PackedStringArray({ path });
	d["from"] = list;
	return d;
}

void HewnPrefabBrowser::place(const String &p_path) {
	if (plugin) {
		plugin->start_prefab(p_path);
	}
}

void HewnPrefabBrowser::set_status(const String &p_text) {
	if (status) {
		status->set_text(p_text);
	}
}

// ------------------------------------------------------------------ placing (plugin)

void HewnEditorPlugin::start_prefab(const String &p_path) {
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	Ref<PackedScene> ps = ResourceLoader::get_singleton()->load(p_path);
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (ps.is_null() || !root) {
		if (prefab_dock) {
			prefab_dock->set_status(vformat(T("Could not load %s"), p_path));
		}
		return;
	}
	Node *test = ps->instantiate();
	if (!Object::cast_to<Node3D>(test)) {
		if (test) {
			memdelete(test);
		}
		if (prefab_dock) {
			prefab_dock->set_status(T("Only 3D scenes can be placed."));
		}
		return;
	}
	memdelete(test);
	prefab_path = p_path;
	place_factory = nullptr;
	place_label = p_path.get_file().get_basename();
	prefab_turn = 0.0f;
	prefab_valid = false;
	prefab_reroll();
	_make_place_ghost();
	_gather_snap_candidates(false);
	modal = MODAL_PREFAB;
	update_overlays();
}

void HewnEditorPlugin::start_placing(const String &p_label, const std::function<Node3D *()> &p_make, bool p_along_normal, bool p_random, const Transform3D &p_local) {
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	if (!EditorInterface::get_singleton()->get_edited_scene_root() || !p_make) {
		return;
	}
	if (adding_from_menu && create_at_cursor) {
		// the Add menu with "Create at 3D Cursor": there at once, turned like the cursor
		Node3D *n = p_make();
		if (!n) {
			return;
		}
		if (n->get_name().is_empty() || String(n->get_name()).begins_with("@")) {
			n->set_name(p_label.replace(" ", ""));
		}
		n->set_transform(Transform3D(cursor_basis, cursor_pos) * p_local);
		_add_node_with_undo(n, "Add " + p_label);
		return;
	}
	prefab_path = String();
	place_copy_from = ObjectID();
	place_factory = p_make;
	place_label = p_label;
	place_along = p_along_normal;
	place_random = p_random;
	place_local = p_local;
	place_size = 1.0f;
	prefab_turn = 0.0f;
	prefab_valid = false;
	prefab_reroll();
	_make_place_ghost();
	_gather_snap_candidates(false);
	modal = MODAL_PREFAB;
	update_overlays();
}

Node3D *HewnEditorPlugin::_make_place_node() {
	if (place_factory) {
		return place_factory();
	}
	Ref<PackedScene> ps = ResourceLoader::get_singleton()->load(prefab_path);
	if (ps.is_null()) {
		return nullptr;
	}
	Node *n = ps->instantiate();
	Node3D *n3 = Object::cast_to<Node3D>(n);
	if (!n3 && n) {
		memdelete(n);
	}
	return n3;
}

void HewnEditorPlugin::_make_place_ghost() {
	_remove_prefab_ghost();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	Node3D *g3 = root ? _make_place_node() : nullptr;
	if (!g3) {
		return;
	}
	g3->set_name("HewnPrefabGhost");
	root->add_child(g3, false, Node::INTERNAL_MODE_BACK); // (not saved, not in the scene tree dock)
	g3->set_visible(prefab_valid);
	if (prefab_valid) {
		g3->set_global_transform(_prefab_transform());
	}
	prefab_ghost = g3->get_instance_id();
}

void HewnEditorPlugin::prefab_reroll() {
	uint32_t seed = (uint32_t)(++prefab_seed) * 2654435761u;
	const bool random = !place_factory || place_random;
	prefab_random_rot = prefab_dock && random ? prefab_dock->random_rotation_value(seed) : Vector3();
	prefab_scale = prefab_dock && random ? prefab_dock->random_size_value(seed) : Vector3(1, 1, 1);
	if (Node3D *g = prefab_ghost ? Object::cast_to<Node3D>(ObjectDB::get_instance(prefab_ghost)) : nullptr) {
		if (prefab_valid) {
			g->set_global_transform(_prefab_transform());
		}
	}
}

void HewnEditorPlugin::_remove_prefab_ghost() {
	Node *g = prefab_ghost ? Object::cast_to<Node>(ObjectDB::get_instance(prefab_ghost)) : nullptr;
	prefab_ghost = 0;
	if (g) {
		if (g->get_parent()) {
			g->get_parent()->remove_child(g);
		}
		g->queue_free();
	}
}

// Where the prefab goes: on the surface under the mouse (or the ground through the 3D cursor), on the grid.
Transform3D HewnEditorPlugin::_prefab_transform() const {
	const bool along = place_factory ? place_along : prefab_dock && prefab_dock->inherit_normal->is_pressed();
	const Vector3 up = along ? prefab_n : Vector3(0, 1, 0);
	Vector3 x, z;
	_plane_axes(up, x, z);
	Basis b;
	b.set_column(0, x);
	b.set_column(1, up);
	b.set_column(2, x.cross(up).normalized());
	b = Basis(up, prefab_turn * (float)(Math::PI / 180.0)) * b;
	// the random turn around the scene's own axes, then its random size
	b = b * Basis::from_euler(prefab_random_rot * (float)(Math::PI / 180.0)) * Basis::from_scale(prefab_scale);
	return place_factory ? Transform3D(b, prefab_pos) * place_local : Transform3D(b, prefab_pos);
}

int32_t HewnEditorPlugin::_prefab_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	const Key code = k.is_valid() && k->is_pressed() && !k->is_echo() ? key_code(k) : KEY_NONE;
	Node3D *ghost = prefab_ghost ? Object::cast_to<Node3D>(ObjectDB::get_instance(prefab_ghost)) : nullptr;
	if (code == KEY_ESCAPE || (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT)) {
		_remove_prefab_ghost();
		prefab_pressing = false;
		modal = MODAL_NONE;
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (prefab_pressing && mm.is_valid()) {
		// held and dragged: the scene stays and turns toward the mouse (Godot's rotation snap steps with the magnet)
		if (mm->get_position().distance_to(prefab_press_pos) > 8.0f || prefab_turned) {
			prefab_turned = true;
			Vector3 x, z;
			const bool along = place_factory ? place_along : prefab_dock && prefab_dock->inherit_normal->is_pressed();
			const Vector3 up = along ? prefab_n : Vector3(0, 1, 0);
			_plane_axes(up, x, z);
			Vector3 hit;
			if (_plane_hit(p_cam, mm->get_position(), prefab_pos, up, hit) && (hit - prefab_pos).length() > 1e-4f) {
				const Vector3 d = hit - prefab_pos;
				float deg = Math::rad_to_deg(std::atan2(-d.dot(x.cross(up).normalized()), d.dot(x))) - 90.0f;
				if (snapping_active(mm->is_ctrl_pressed() || mm->is_meta_pressed())) {
					const float st = MAX(0.01f, (float)dock->snap_rotate->get_value());
					deg = std::round(deg / st) * st;
				}
				prefab_turn = deg;
			}
			if (ghost) {
				ghost->set_global_transform(_prefab_transform());
			}
			update_overlays();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (code == KEY_Q || code == KEY_E) {
		prefab_turn += (code == KEY_E ? -1.0f : 1.0f) * (k->is_shift_pressed() ? 15.0f : 90.0f);
	} else if (code == KEY_R) {
		prefab_reroll();
	} else if (code == KEY_N && place_factory) {
		place_along = !place_along;
	} else if (code == KEY_N && prefab_dock) {
		prefab_dock->inherit_normal->set_pressed(!prefab_dock->inherit_normal->is_pressed());
	} else if ((code == KEY_MINUS || code == KEY_EQUAL || code == KEY_KP_SUBTRACT || code == KEY_KP_ADD) && place_factory) {
		place_size *= (code == KEY_MINUS || code == KEY_KP_SUBTRACT) ? 0.8f : 1.25f;
		_make_place_ghost(); // (the factory reads the size)
		ghost = prefab_ghost ? Object::cast_to<Node3D>(ObjectDB::get_instance(prefab_ghost)) : nullptr;
	}
	if (mm.is_valid() || code != KEY_NONE) {
		tool_ctrl = mm.is_valid() ? (mm->is_ctrl_pressed() || mm->is_meta_pressed()) : (k.is_valid() && ctrl_after(k));
		tool_alt = mm.is_valid() ? mm->is_alt_pressed() : (k.is_valid() && k->is_alt_pressed());
		Vector2 uv;
		prefab_valid = _shape_begin(p_cam, mouse_pos) && _shape_plane_point(p_cam, mouse_pos, uv);
		if (prefab_valid) {
			prefab_pos = shape_origin + shape_u * uv.x + shape_v * uv.y;
			prefab_n = shape_n;
			plane_lock_valid = true;
			plane_lock_origin = shape_origin;
			plane_lock_n = shape_n;
			plane_lock_u = shape_u;
			plane_lock_v = shape_v;
		}
		if (ghost) {
			ghost->set_visible(prefab_valid);
			if (prefab_valid) {
				ghost->set_global_transform(_prefab_transform());
			}
		}
		update_overlays();
		return mm.is_valid() ? AFTER_GUI_INPUT_PASS : AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		if (mb->is_pressed() && prefab_valid) {
			prefab_pressing = true;
			prefab_turned = false;
			prefab_press_pos = mb->get_position();
			prefab_press_turn = prefab_turn;
		} else if (!mb->is_pressed() && prefab_pressing) {
			prefab_pressing = false;
			_place_prefab(); // (on the release: a drag before it turned the scene)
		}
		return AFTER_GUI_INPUT_STOP;
	}
	return AFTER_GUI_INPUT_PASS;
}

void HewnEditorPlugin::_place_prefab() {
	Ref<PackedScene> ps = place_factory ? Ref<PackedScene>() : Ref<PackedScene>(ResourceLoader::get_singleton()->load(prefab_path));
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	const Transform3D xf = _prefab_transform();
	_remove_prefab_ghost();
	modal = MODAL_NONE;
	if ((ps.is_null() && !place_factory) || !root) {
		return;
	}
	const bool local = place_factory || (prefab_dock && prefab_dock->make_local->is_pressed());
	Node3D *n = place_factory ? place_factory() : Object::cast_to<Node3D>(ps->instantiate(local ? PackedScene::GEN_EDIT_STATE_DISABLED : PackedScene::GEN_EDIT_STATE_INSTANCE));
	if (!n) {
		return;
	}
	String name = (place_factory ? place_label : prefab_path.get_file().get_basename()).validate_node_name().replace(" ", "");
	if (prefab_dock && !prefab_dock->suffix->get_text().strip_edges().is_empty()) {
		name += prefab_dock->suffix->get_text().strip_edges();
	}
	n->set_name(name);
	if (Node *prev = place_factory ? Object::cast_to<Node>(ObjectDB::get_instance(place_copy_from)) : nullptr) {
		// W: another one like the last (what its panel changed: color, size, range...)
		String title;
		if (prev->get_class() == n->get_class()) {
			for (const char *p : hw_obj_props(prev, title)) {
				n->set(p, prev->get(p));
			}
		}
	}
	if (local && !place_factory) {
		n->set_scene_file_path(""); // (a copy of its own)
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Place " + name);
	ur->add_do_method(root, "add_child", n, true);
	ur->add_do_method(n, "set_owner", root);
	ur->add_do_reference(n);
	ur->add_undo_method(root, "remove_child", n);
	ur->commit_action();
	own(n, root, local);
	n->set_global_transform(xf);
	EditorSelection *es = EditorInterface::get_singleton()->get_selection();
	es->clear();
	es->add_node(n);
	pending_action = "prefab";
	pending_meshes.clear();
	pending_loops.clear();
	prefab_last = n->get_instance_id();
	prefab_last_xf = n->get_global_transform();
	prefab_reroll(); // (the next one: another random turn / size)
	update_overlays();
}

void HewnEditorPlugin::_draw_prefab(Control *p_overlay, Camera3D *p_cam) {
	Ref<Font> font = p_overlay->get_theme_default_font();
	if (grid_hover_valid || prefab_valid) {
		_draw_surface_grid(p_overlay, p_cam, prefab_pos, shape_u, shape_v);
	}
	const bool along = place_factory ? place_along : prefab_dock && prefab_dock->inherit_normal->is_pressed();
	if (place_factory) {
		draw_header(p_overlay, font, vformat(T("%s: click  |  Q / E: turn  |  N: %s  |  - / =: size x%.2f  |  Esc"),
				place_label, along ? T("along the surface") : T("upright"), place_size));
		return;
	}
	draw_header(p_overlay, font, vformat(T("%s: click  |  Q / E: turn  |  N: %s  |  R: random  |  Esc"),
			prefab_path.get_file().get_basename(), along ? T("along the surface") : T("upright")));
}
