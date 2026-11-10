#include "hw_scatter_dock.h"

#include "hw_editor.h"
#include "hw_editor_util.h"

#include "../hw_scatter.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_resource_preview.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/v_separator.hpp>

using namespace godot;
using namespace hw_editor;

void HewnScatterDock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_preview", "path", "preview", "thumb", "index"), &HewnScatterDock::_on_preview);
	ClassDB::bind_method(D_METHOD("refresh"), &HewnScatterDock::refresh);
}

void HewnScatterDock::setup(HewnEditorPlugin *p_plugin) {
	plugin = p_plugin;
	set_name("HewnScatter");
	set_title(T("Scatter"));
	set_default_slot(EditorDock::DOCK_SLOT_BOTTOM);
	set_transient(true);
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	VBoxContainer *v = memnew(VBoxContainer);
	add_child(v);
	HBoxContainer *bar = memnew(HBoxContainer);
	v->add_child(bar);
	auto button = [&](const char *p_text, const char *p_tip, void (HewnScatterDock::*p_fn)()) {
		Button *b = memnew(Button);
		b->set_text(T(p_text));
		b->set_tooltip_text(T(p_tip));
		b->connect("pressed", callable_mp(this, p_fn));
		bar->add_child(b);
		return b;
	};
	button("Add Mesh...", "A mesh (or the first mesh of a scene) from a file, as one more the scatter places", &HewnScatterDock::_on_add);
	button("Remove", "The selected meshes out of the scatter (with their copies)", &HewnScatterDock::_on_remove);
	bar->add_child(memnew(VSeparator));
	button("All", "The brush places all of them", &HewnScatterDock::_on_all);
	button("None", "None selected (the brush places all of them then)", &HewnScatterDock::_on_none);
	bar->add_child(memnew(VSeparator));
	Label *l = memnew(Label);
	l->set_text(T("Erase Strength"));
	bar->add_child(l);
	erase_strength = memnew(HewnDragField);
	erase_strength->setup(0, 1, 0.01, false, 1.0);
	erase_strength->set_custom_minimum_size(Vector2(70 * es, 0));
	erase_strength->set_tooltip_text(T("How many of the copies under it the eraser takes (1: all, less: thinned out)"));
	erase_strength->connect("value_changed", callable_mp(this, &HewnScatterDock::_on_strength));
	bar->add_child(erase_strength);
	list = memnew(ItemList);
	list->set_select_mode(ItemList::SELECT_MULTI); // (Ctrl / Shift: several)
	list->set_max_columns(0);
	list->set_icon_mode(ItemList::ICON_MODE_TOP);
	list->set_fixed_icon_size(Vector2i(64, 64) * es);
	list->set_fixed_column_width((int)(88 * es));
	list->set_same_column_width(true);
	list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	list->set_custom_minimum_size(Vector2(0, 110 * es));
	list->set_tooltip_text(T("The meshes the brush places (Ctrl / Shift: several; none selected: all)"));
	list->connect("multi_selected", callable_mp(this, &HewnScatterDock::_on_selected));
	v->add_child(list);
}

HewnScatter3D *HewnScatterDock::_scatter() const {
	HewnScatter3D *s = Object::cast_to<HewnScatter3D>(ObjectDB::get_instance(scatter));
	return s && s->is_inside_tree() ? s : nullptr;
}

void HewnScatterDock::show_scatter(HewnScatter3D *p_scatter) {
	const ObjectID id = p_scatter ? ObjectID(p_scatter->get_instance_id()) : ObjectID();
	if (id == scatter) {
		return;
	}
	scatter = id;
	if (p_scatter) {
		refresh();
		make_visible();
	} else {
		close();
	}
}

void HewnScatterDock::refresh() {
	HewnScatter3D *sc = _scatter();
	filling = true;
	list->clear();
	if (sc) {
		erase_strength->set_value_no_signal(sc->get_erase_strength());
		const TypedArray<Mesh> ms = sc->get_meshes();
		const PackedInt32Array on = sc->get_paint_items();
		EditorResourcePreview *rp = EditorInterface::get_singleton()->get_resource_previewer();
		for (int64_t i = 0; i < ms.size(); i++) {
			const Ref<Mesh> m = ms[i];
			String name = m.is_valid() ? (m->get_path().is_empty() ? (m->get_name().is_empty() ? vformat("Mesh %d", (int)i + 1) : m->get_name()) : m->get_path().get_file().get_basename()) : String("-");
			const int idx = list->add_item(name);
			list->set_item_tooltip(idx, m.is_valid() ? m->get_path() : String());
			if (on.has((int)i)) {
				list->select(idx, false);
			}
			if (m.is_valid() && rp) {
				rp->queue_edited_resource_preview(m, this, "_on_preview", (int)i);
			}
		}
	}
	filling = false;
}

void HewnScatterDock::_on_preview(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_thumb, const Variant &p_index) {
	const int i = p_index;
	if (i >= 0 && i < list->get_item_count() && p_preview.is_valid()) {
		list->set_item_icon(i, p_preview);
	}
}

void HewnScatterDock::_on_selected(int p_index, bool p_selected) {
	HewnScatter3D *sc = _scatter();
	if (!sc || filling) {
		return;
	}
	PackedInt32Array on;
	for (int i = 0; i < list->get_item_count(); i++) {
		if (list->is_selected(i)) {
			on.push_back(i);
		}
	}
	sc->set_paint_items(on); // (the brush's choice: not an undo step)
}

void HewnScatterDock::_on_all() {
	for (int i = 0; i < list->get_item_count(); i++) {
		list->select(i, false);
	}
	_on_selected(0, true);
}

void HewnScatterDock::_on_none() {
	list->deselect_all();
	_on_selected(0, false);
}

void HewnScatterDock::_on_strength(double p_value) {
	if (HewnScatter3D *sc = _scatter()) {
		sc->set_erase_strength((float)p_value);
	}
}

void HewnScatterDock::_on_add() {
	if (!dialog) {
		dialog = memnew(EditorFileDialog);
		dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILES);
		dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
		for (const char *f : { "*.tres", "*.res", "*.mesh", "*.obj", "*.glb", "*.gltf", "*.fbx", "*.blend", "*.tscn", "*.scn" }) {
			dialog->add_filter(f);
		}
		dialog->set_title(T("Add Mesh"));
		dialog->connect("files_selected", callable_mp(this, &HewnScatterDock::_on_files));
		add_child(dialog);
	}
	dialog->popup_file_dialog();
}

namespace {
// the first mesh of a scene (a model file brought in as a scene)
Ref<Mesh> first_mesh(Node *p_node) {
	if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_node)) {
		if (mi->get_mesh().is_valid()) {
			return mi->get_mesh();
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		const Ref<Mesh> m = first_mesh(p_node->get_child(i));
		if (m.is_valid()) {
			return m;
		}
	}
	return Ref<Mesh>();
}
} // namespace

void HewnScatterDock::_on_files(const PackedStringArray &p_paths) {
	HewnScatter3D *sc = _scatter();
	if (!sc) {
		return;
	}
	TypedArray<Mesh> ms = sc->get_meshes().duplicate();
	PackedFloat32Array ws = sc->get_weights();
	Array cs = sc->get_collisions().duplicate();
	const int64_t n0 = ms.size();
	for (const String &p : p_paths) {
		const Ref<Resource> r = ResourceLoader::get_singleton()->load(p);
		Ref<Mesh> m = r;
		const Ref<PackedScene> ps = r;
		if (m.is_null() && ps.is_valid()) {
			Node *inst = ps->instantiate();
			if (inst) {
				m = first_mesh(inst);
				memdelete(inst);
			}
		}
		if (m.is_valid()) {
			ms.push_back(m);
		}
	}
	if (ms.size() == n0) {
		return;
	}
	while (ws.size() < ms.size()) {
		ws.push_back(1.0f);
	}
	while (cs.size() < ms.size()) {
		cs.push_back(0);
	}
	EditorUndoRedoManager *ur = plugin->get_undo_redo();
	ur->create_action(T("Scatter: Add Mesh"), UndoRedo::MERGE_DISABLE, sc);
	ur->add_do_method(sc, "set_meshes", ms);
	ur->add_do_method(sc, "set_weights", ws);
	ur->add_do_method(sc, "set_collisions", cs);
	ur->add_undo_method(sc, "set_meshes", sc->get_meshes());
	ur->add_undo_method(sc, "set_weights", sc->get_weights());
	ur->add_undo_method(sc, "set_collisions", sc->get_collisions());
	ur->add_do_method(this, "refresh");
	ur->add_undo_method(this, "refresh");
	ur->commit_action();
}

void HewnScatterDock::_on_remove() {
	HewnScatter3D *sc = _scatter();
	if (!sc) {
		return;
	}
	const PackedInt32Array sel = list->get_selected_items();
	if (sel.is_empty()) {
		return;
	}
	// (the meshes after them keep their copies: the copies' indices are moved down)
	TypedArray<Mesh> ms;
	PackedFloat32Array ws;
	Array cs;
	std::vector<int> remap(sc->get_meshes().size(), -1);
	for (int64_t i = 0; i < sc->get_meshes().size(); i++) {
		if (sel.has((int)i)) {
			continue;
		}
		remap[i] = (int)ms.size();
		ms.push_back(sc->get_meshes()[i]);
		ws.push_back(i < sc->get_weights().size() ? sc->get_weights()[i] : 1.0f);
		cs.push_back(i < sc->get_collisions().size() ? sc->get_collisions()[i] : Variant(0));
	}
	const PackedFloat32Array before = sc->get_instances();
	PackedFloat32Array after;
	for (int64_t k = 0; k + 12 < before.size(); k += 13) {
		const int item = (int)before[k + 12];
		if (item < 0 || item >= (int)remap.size() || remap[item] < 0) {
			continue;
		}
		for (int j = 0; j < 12; j++) {
			after.push_back(before[k + j]);
		}
		after.push_back((float)remap[item]);
	}
	EditorUndoRedoManager *ur = plugin->get_undo_redo();
	ur->create_action(T("Scatter: Remove Mesh"), UndoRedo::MERGE_DISABLE, sc);
	ur->add_do_method(sc, "set_meshes", ms);
	ur->add_do_method(sc, "set_weights", ws);
	ur->add_do_method(sc, "set_collisions", cs);
	ur->add_do_method(sc, "set_instances", after);
	ur->add_do_method(sc, "set_paint_items", PackedInt32Array());
	ur->add_undo_method(sc, "set_meshes", sc->get_meshes());
	ur->add_undo_method(sc, "set_weights", sc->get_weights());
	ur->add_undo_method(sc, "set_collisions", sc->get_collisions());
	ur->add_undo_method(sc, "set_instances", before);
	ur->add_undo_method(sc, "set_paint_items", sc->get_paint_items());
	ur->add_do_method(this, "refresh");
	ur->add_undo_method(this, "refresh");
	ur->commit_action();
}
