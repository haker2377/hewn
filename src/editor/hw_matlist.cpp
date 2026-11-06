// Material List: the materials of the project, what kind each is and the images it shows. Opened from the
// texture browser (all of them, or the ones using a texture: right click on it); double click puts one on the
// selection. The thumbnails show how many materials use an image when more than one does.
#include "hw_textures.h"

#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_resource_preview.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/editor_inspector.hpp>
#include <godot_cpp/classes/editor_resource_picker.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/h_split_container.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>

#include <map>
#include <set>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/visual_shader.hpp>

using namespace godot;
using hw_editor::T;

namespace {

String ml_join(const String &p_dir, const String &p_name) {
	return p_dir.ends_with("/") ? p_dir + p_name : p_dir + String("/") + p_name;
}

// Every image a material shows: its texture slots, or the texture parameters of its shader.
PackedStringArray images_of(const Ref<Material> &p_material) {
	PackedStringArray out;
	auto add = [&](const Ref<Texture2D> &p_tex) {
		if (p_tex.is_valid() && !p_tex->get_path().is_empty() && !p_tex->get_path().contains("::") && !out.has(p_tex->get_path())) {
			out.push_back(p_tex->get_path());
		}
	};
	Ref<BaseMaterial3D> bm = p_material;
	if (bm.is_valid()) {
		for (int t = 0; t < (int)BaseMaterial3D::TEXTURE_MAX; t++) {
			add(bm->get_texture((BaseMaterial3D::TextureParam)t));
		}
		return out;
	}
	Ref<ShaderMaterial> sm = p_material;
	if (sm.is_valid() && sm->get_shader().is_valid()) {
		const Array us = sm->get_shader()->get_shader_uniform_list();
		for (int64_t i = 0; i < us.size(); i++) {
			const Dictionary u = us[i];
			add(sm->get_shader_parameter(u.get("name", "")));
		}
	}
	return out;
}

String kind_of(const Ref<Material> &p_material, int p_images) {
	if (p_material->has_meta("hw_blend_material")) {
		return vformat(T("Blend of %d textures (by a painted channel)"), p_images);
	}
	if (Object::cast_to<BaseMaterial3D>(p_material.ptr())) {
		return p_material->has_meta("hw_image") ? T("Texture material (made for its image)") : T("Standard material");
	}
	Ref<ShaderMaterial> sm = p_material;
	if (sm.is_valid()) {
		return Object::cast_to<VisualShader>(sm->get_shader().ptr()) ? T("Visual shader material") : T("Shader material");
	}
	return p_material->get_class();
}

} // namespace

void HewnTextureBrowser::_build_mat_index() {
	mat_index_dirty = false;
	mat_index.clear();
	std::vector<String> dirs = { "res://" };
	std::vector<String> files;
	while (!dirs.empty()) {
		const String d = dirs.back();
		dirs.pop_back();
		const PackedStringArray sub = DirAccess::get_directories_at(d);
		for (int64_t i = 0; i < sub.size(); i++) {
			if (!String(sub[i]).begins_with(".") && sub[i] != "addons") {
				dirs.push_back(ml_join(d, sub[i]));
			}
		}
		const PackedStringArray fs = DirAccess::get_files_at(d);
		for (int64_t i = 0; i < fs.size(); i++) {
			const String e = String(fs[i]).get_extension();
			if (e == "tres" || e == "res" || e == "material") {
				files.push_back(ml_join(d, fs[i]));
			}
		}
	}
	ResourceLoader *rl = ResourceLoader::get_singleton();
	const String tpl = default_material_path();
	for (const String &f : files) {
		if (f == tpl || !rl->exists(f, "Material")) {
			continue;
		}
		Ref<Material> m = rl->load(f);
		if (m.is_null()) {
			continue;
		}
		MatInfo mi;
		mi.path = f;
		mi.textures = images_of(m);
		mi.kind = kind_of(m, (int)mi.textures.size());
		mat_index.push_back(mi);
	}
}

int HewnTextureBrowser::_uses_of(const String &p_image) {
	if (mat_index_dirty) {
		_build_mat_index();
	}
	int n = 0;
	for (const MatInfo &mi : mat_index) {
		n += mi.textures.has(p_image) ? 1 : 0;
	}
	return n;
}

void HewnTextureBrowser::_matlist_make() {
	matlist_dialog = memnew(AcceptDialog);
	matlist_dialog->set_name("HewnMaterialList");
	matlist_dialog->set_title(T("Material List"));
	matlist_dialog->set_ok_button_text(T("Close"));
	matlist_dialog->add_button(T("Edit in Inspector"), false, "edit");
	matlist_dialog->add_button(T("Put on Selection"), false, "apply");
	matlist_dialog->connect("custom_action", callable_mp(this, &HewnTextureBrowser::_matlist_action));
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	HSplitContainer *split = memnew(HSplitContainer);
	matlist_dialog->add_child(split);
	VBoxContainer *v = memnew(VBoxContainer);
	v->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	split->add_child(v);
	// the right side
	VBoxContainer *side = memnew(VBoxContainer);
	side->set_custom_minimum_size(Vector2(320, 0) * es);
	split->add_child(side);
	if (ClassDBSingleton::get_singleton()->can_instantiate("EditorInspector")) {
		Object *o = ClassDBSingleton::get_singleton()->instantiate("EditorInspector");
		if (EditorInspector *ins = Object::cast_to<EditorInspector>(o)) {
			ins->set_name("MaterialInspector");
			ins->set_v_size_flags(Control::SIZE_EXPAND_FILL);
			ins->set_custom_minimum_size(Vector2(320, 260) * es);
			ins->connect("property_edited", callable_mp(this, &HewnTextureBrowser::_matlist_saved));
			side->add_child(ins);
			matlist_inspector = ins;
		} else if (o) {
			memdelete(o);
		}
	}
	matlist_users_label = memnew(Label);
	side->add_child(matlist_users_label);
	matlist_users = memnew(ItemList);
	matlist_users->set_name("MaterialUsers");
	matlist_users->set_custom_minimum_size(Vector2(0, 90) * es);
	matlist_users->set_tooltip_text(T("The meshes of the open scene with this material; click: select it"));
	matlist_users->connect("item_clicked", callable_mp(this, &HewnTextureBrowser::_matlist_user_clicked).unbind(2));
	side->add_child(matlist_users);
	HBoxContainer *rh = memnew(HBoxContainer);
	side->add_child(rh);
	matlist_replace = memnew(EditorResourcePicker);
	matlist_replace->set_base_type("Material");
	matlist_replace->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	matlist_replace->set_tooltip_text(T("The material to put in its place (drag one here, or pick it)"));
	rh->add_child(matlist_replace);
	Button *rb = memnew(Button);
	rb->set_name("ReplaceMaterial");
	rb->set_text(T("Replace on All"));
	rb->set_tooltip_text(T("This material is replaced by the one picked on the left, on every mesh of the open scene that has it (one undo step)"));
	rb->connect("pressed", callable_mp(this, &HewnTextureBrowser::_matlist_do_replace));
	rh->add_child(rb);
	HBoxContainer *h = memnew(HBoxContainer);
	v->add_child(h);
	matlist_search = memnew(LineEdit);
	matlist_search->set_placeholder(T("Search (name, kind, texture)"));
	matlist_search->set_clear_button_enabled(true);
	matlist_search->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	matlist_search->connect("text_changed", callable_mp(this, &HewnTextureBrowser::_matlist_search));
	h->add_child(matlist_search);
	matlist_only = memnew(CheckBox);
	matlist_only->set_tooltip_text(T("Only the materials using this texture"));
	matlist_only->connect("toggled", callable_mp(this, &HewnTextureBrowser::_matlist_only));
	h->add_child(matlist_only);
	matlist_tree = memnew(Tree);
	matlist_tree->set_columns(4);
	matlist_tree->set_column_titles_visible(true);
	matlist_tree->set_column_title(0, T("Material"));
	matlist_tree->set_column_title(1, T("Kind"));
	matlist_tree->set_column_title(2, T("Textures"));
	matlist_tree->set_column_title(3, T("Meshes"));
	matlist_tree->set_column_title_tooltip_text(3, T("How many meshes of the open scene use it (click a title: sort by it)"));
	matlist_tree->set_column_expand_ratio(0, 2);
	matlist_tree->set_column_expand_ratio(1, 2);
	matlist_tree->set_column_expand_ratio(2, 3);
	matlist_tree->set_column_expand(3, false);
	matlist_tree->set_column_custom_minimum_width(3, (int)(70 * EditorInterface::get_singleton()->get_editor_scale()));
	matlist_tree->connect("column_title_clicked", callable_mp(this, &HewnTextureBrowser::_matlist_sort_by));
	matlist_tree->set_column_custom_minimum_width(0, (int)(200 * EditorInterface::get_singleton()->get_editor_scale()));
	matlist_tree->set_hide_root(true);
	matlist_tree->set_select_mode(Tree::SELECT_ROW);
	matlist_tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	matlist_tree->set_custom_minimum_size(Vector2(560, 360) * es);
	matlist_tree->connect("item_activated", callable_mp(this, &HewnTextureBrowser::_matlist_apply));
	matlist_tree->connect("item_selected", callable_mp(this, &HewnTextureBrowser::_matlist_selected));
	v->add_child(matlist_tree);
	add_child(matlist_dialog);
}

void HewnTextureBrowser::open_material_list(const String &p_texture) {
	if (!matlist_dialog) {
		_matlist_make();
	}
	if (mat_index_dirty) {
		_build_mat_index();
	}
	matlist_texture = p_texture;
	matlist_only->set_visible(!p_texture.is_empty());
	matlist_only->set_text(vformat(T("Only with %s"), p_texture.get_file()));
	matlist_only->set_pressed_no_signal(!p_texture.is_empty());
	_matlist_fill();
	matlist_dialog->popup_centered();
}

namespace {
// How many meshes of the open scene use each material (by its file).
void count_users(Node *p_node, std::map<String, int> &r_count) {
	std::set<String> mine;
	if (HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(p_node)) {
		const TypedArray<Material> ms = pm->get_materials();
		for (int64_t i = 0; i < ms.size(); i++) {
			Ref<Material> m = ms[i];
			if (m.is_valid() && !m->get_path().is_empty()) {
				mine.insert(m->get_path());
			}
		}
	} else if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_node)) {
		auto add = [&](const Ref<Material> &p_m) {
			if (p_m.is_valid() && !p_m->get_path().is_empty()) {
				mine.insert(p_m->get_path());
			}
		};
		add(mi->get_material_override());
		Ref<Mesh> mesh = mi->get_mesh();
		for (int s = 0; mesh.is_valid() && s < mesh->get_surface_count(); s++) {
			add(mi->get_surface_override_material(s));
			add(mesh->surface_get_material(s));
		}
	}
	for (const String &p : mine) {
		r_count[p]++;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		count_users(p_node->get_child(i), r_count);
	}
}
} // namespace

void HewnTextureBrowser::_matlist_sort_by(int p_column, int p_button) {
	if (p_button != MOUSE_BUTTON_LEFT) {
		return;
	}
	matlist_sort_desc = matlist_sort == p_column ? !matlist_sort_desc : p_column == 3; // (the counts: most used first)
	matlist_sort = p_column;
	_matlist_fill();
}

void HewnTextureBrowser::_matlist_fill() {
	matlist_tree->clear();
	TreeItem *root = matlist_tree->create_item();
	const String q = matlist_search->get_text().strip_edges().to_lower();
	const bool only = matlist_only->is_visible() && matlist_only->is_pressed() && !matlist_texture.is_empty();
	std::map<String, int> counts;
	if (Node *sr = EditorInterface::get_singleton()->get_edited_scene_root()) {
		count_users(sr, counts);
	}
	auto tex_names = [](const MatInfo &p_mi) {
		String names;
		for (int64_t i = 0; i < p_mi.textures.size(); i++) {
			names += (i ? ", " : "") + p_mi.textures[i].get_file();
		}
		return names;
	};
	std::vector<const MatInfo *> order;
	for (const MatInfo &mi : mat_index) {
		order.push_back(&mi);
	}
	if (matlist_sort >= 0) {
		auto num = [&](const MatInfo *p_m) { return counts.count(p_m->path) ? counts[p_m->path] : 0; };
		auto key = [&](const MatInfo *p_m) { return matlist_sort == 0 ? p_m->path.get_file().to_lower() : matlist_sort == 1 ? p_m->kind.to_lower() : tex_names(*p_m).to_lower(); };
		auto less = [&](const MatInfo *a, const MatInfo *b) { return matlist_sort == 3 ? num(a) < num(b) : key(a) < key(b); };
		std::stable_sort(order.begin(), order.end(), [&](const MatInfo *a, const MatInfo *b) { return matlist_sort_desc ? less(b, a) : less(a, b); });
	}
	int shown = 0;
	for (const MatInfo *mip : order) {
		const MatInfo &mi = *mip;
		if (only && !mi.textures.has(matlist_texture)) {
			continue;
		}
		String names;
		for (int64_t i = 0; i < mi.textures.size(); i++) {
			names += (i ? ", " : "") + mi.textures[i].get_file();
		}
		if (!q.is_empty() && !mi.path.to_lower().contains(q) && !mi.kind.to_lower().contains(q) && !names.to_lower().contains(q)) {
			continue;
		}
		TreeItem *it = matlist_tree->create_item(root);
		it->set_text(0, mi.path.get_file().get_basename());
		it->set_tooltip_text(0, mi.path);
		it->set_text(1, mi.kind);
		it->set_text(2, names);
		const int used = counts.count(mi.path) ? counts[mi.path] : 0;
		it->set_text(3, itos(used));
		it->set_text_alignment(3, HORIZONTAL_ALIGNMENT_RIGHT);
		String tips;
		for (int64_t i = 0; i < mi.textures.size(); i++) {
			tips += (i ? "\n" : "") + mi.textures[i];
		}
		it->set_tooltip_text(2, tips);
		it->set_metadata(0, mi.path);
		// a small picture of the material on the left of its name (Godot's own preview, made in the background)
		it->set_icon_max_width(0, (int)(24 * EditorInterface::get_singleton()->get_editor_scale()));
		if (EditorResourcePreview *rp = EditorInterface::get_singleton()->get_resource_previewer()) {
			rp->queue_resource_preview(mi.path, this, "_on_mat_preview", mi.path);
		}
		if (shown == 0) {
			it->select(0);
		}
		shown++;
	}
	_matlist_selected();
}

namespace {
// The meshes of the open scene that have the material (Hewn meshes, MeshInstance3D: override, surfaces).
void users_of(Node *p_node, const Ref<Material> &p_mat, std::vector<Node *> &r_out) {
	bool has = false;
	if (HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(p_node)) {
		const TypedArray<Material> ms = pm->get_materials();
		for (int64_t i = 0; i < ms.size() && !has; i++) {
			has = Ref<Material>(ms[i]) == p_mat;
		}
	} else if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_node)) {
		has = mi->get_material_override() == p_mat;
		Ref<Mesh> mesh = mi->get_mesh();
		for (int s = 0; mesh.is_valid() && s < mesh->get_surface_count() && !has; s++) {
			has = mi->get_surface_override_material(s) == p_mat || mesh->surface_get_material(s) == p_mat;
		}
	}
	if (has) {
		r_out.push_back(p_node);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		users_of(p_node->get_child(i), p_mat, r_out);
	}
}
} // namespace

// The selected material: in the small inspector, with the meshes using it.
void HewnTextureBrowser::_matlist_selected() {
	TreeItem *it = matlist_tree ? matlist_tree->get_selected() : nullptr;
	matlist_current = it ? Ref<Material>(ResourceLoader::get_singleton()->load(String(it->get_metadata(0)))) : Ref<Material>();
	if (EditorInspector *ins = Object::cast_to<EditorInspector>(matlist_inspector)) {
		ins->edit(matlist_current.ptr());
	}
	matlist_users->clear();
	matlist_user_ids.clear();
	std::vector<Node *> users;
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (root && matlist_current.is_valid()) {
		users_of(root, matlist_current, users);
	}
	for (Node *u : users) {
		matlist_users->add_item(String(root->get_path_to(u)));
		matlist_user_ids.push_back(ObjectID(u->get_instance_id()));
	}
	matlist_users_label->set_text(matlist_current.is_valid() ? vformat(T("Used by %d meshes in this scene:"), (int)users.size()) : String());
}

// A change in the small inspector goes into the material's file.
void HewnTextureBrowser::_matlist_saved(const String &p_property) {
	if (matlist_current.is_valid() && !matlist_current->get_path().is_empty() && !matlist_current->get_path().contains("::")) {
		ResourceSaver::get_singleton()->save(matlist_current, matlist_current->get_path());
	}
}

void HewnTextureBrowser::_matlist_user_clicked(int p_index) {
	if (p_index < 0 || p_index >= (int)matlist_user_ids.size()) {
		return;
	}
	if (Node *n = Object::cast_to<Node>(ObjectDB::get_instance(matlist_user_ids[p_index]))) {
		EditorInterface::get_singleton()->get_selection()->clear();
		EditorInterface::get_singleton()->get_selection()->add_node(n);
	}
}

// The selected material replaced by the picked one on every mesh of the scene that has it.
void HewnTextureBrowser::_matlist_do_replace() {
	Ref<Material> to = matlist_replace ? Ref<Material>(matlist_replace->get_edited_resource()) : Ref<Material>();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (matlist_current.is_null() || to.is_null() || to == matlist_current || !root || !plugin) {
		set_status(T("Pick the material to put in its place first."));
		return;
	}
	std::vector<Node *> users;
	users_of(root, matlist_current, users);
	if (users.empty()) {
		return;
	}
	EditorUndoRedoManager *ur = plugin->get_undo_redo();
	ur->create_action("Hewn: Replace Material", UndoRedo::MERGE_DISABLE, root);
	for (Node *u : users) {
		if (HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(u)) {
			const TypedArray<Material> before = pm->get_materials();
			TypedArray<Material> after = before.duplicate();
			for (int64_t i = 0; i < after.size(); i++) {
				if (Ref<Material>(after[i]) == matlist_current) {
					after[i] = to;
				}
			}
			ur->add_do_method(pm, "set_materials", after);
			ur->add_undo_method(pm, "set_materials", before);
		} else if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(u)) {
			if (mi->get_material_override() == matlist_current) {
				ur->add_do_method(mi, "set_material_override", to);
				ur->add_undo_method(mi, "set_material_override", matlist_current);
			}
			Ref<Mesh> mesh = mi->get_mesh();
			for (int s = 0; mesh.is_valid() && s < mesh->get_surface_count(); s++) {
				if (mi->get_surface_override_material(s) == matlist_current || (mi->get_surface_override_material(s).is_null() && mesh->surface_get_material(s) == matlist_current)) {
					ur->add_do_method(mi, "set_surface_override_material", s, to);
					ur->add_undo_method(mi, "set_surface_override_material", s, mi->get_surface_override_material(s));
				}
			}
		}
	}
	ur->commit_action();
	set_status(vformat(T("%s replaced on %d meshes"), matlist_current->get_path().get_file(), (int)users.size()));
	_matlist_selected();
}

void HewnTextureBrowser::_on_mat_preview(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_thumb, const Variant &p_userdata) {
	if (!matlist_tree || !matlist_tree->get_root()) {
		return;
	}
	const Ref<Texture2D> pic = p_thumb.is_valid() ? p_thumb : p_preview;
	if (pic.is_null()) {
		return;
	}
	for (TreeItem *it = matlist_tree->get_root()->get_first_child(); it; it = it->get_next()) {
		if (String(it->get_metadata(0)) == String(p_userdata)) {
			it->set_icon(0, pic);
		}
	}
}

void HewnTextureBrowser::_matlist_search(const String &) {
	_matlist_fill();
}

void HewnTextureBrowser::_matlist_only(bool) {
	_matlist_fill();
}

void HewnTextureBrowser::_matlist_apply() {
	TreeItem *it = matlist_tree ? matlist_tree->get_selected() : nullptr;
	if (!it) {
		return;
	}
	const String path = it->get_metadata(0);
	Ref<Material> m = ResourceLoader::get_singleton()->load(path);
	if (m.is_null()) {
		return;
	}
	set_current(m, path.get_file().get_basename());
	const int n = plugin ? plugin->apply_material_to_selection(m) : -1;
	set_status(n < 0 ? T("Select objects, or faces in edit mode, first.") : vformat(T("%s: %d faces"), path.get_file(), n));
}

void HewnTextureBrowser::_matlist_edit() {
	TreeItem *it = matlist_tree ? matlist_tree->get_selected() : nullptr;
	if (!it) {
		return;
	}
	Ref<Material> m = ResourceLoader::get_singleton()->load(String(it->get_metadata(0)));
	if (m.is_valid()) {
		hw_editor::edit_in_inspector(m);
	}
}

void HewnTextureBrowser::_matlist_action(const StringName &p_action) {
	if (p_action == StringName("apply")) {
		_matlist_apply();
	} else if (p_action == StringName("edit")) {
		_matlist_edit();
	}
}

void HewnTextureBrowser::_on_tex_menu(int p_id) {
	if (p_id == 0) {
		open_material_list(tex_menu_path);
	} else if (p_id == 1) {
		EditorInterface::get_singleton()->select_file(tex_menu_path); // (in the FileSystem dock)
	}
}

void HewnTextureBrowser::_on_material_list_button() {
	open_material_list(String());
}
