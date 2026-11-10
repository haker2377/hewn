// The Scatter's own bottom panel (opened while a Scatter is selected): its meshes, which of them the brush places
// (several with Ctrl / Shift), a mesh added from a file, and how much the eraser takes.
#pragma once

#include <godot_cpp/classes/editor_dock.hpp>
#include <godot_cpp/classes/editor_file_dialog.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/texture2d.hpp>

namespace godot {

class HewnEditorPlugin;
class HewnScatter3D;
class HewnDragField;

class HewnScatterDock : public EditorDock {
	GDCLASS(HewnScatterDock, EditorDock)

	HewnEditorPlugin *plugin = nullptr;
	ObjectID scatter;
	ItemList *list = nullptr;
	HewnDragField *erase_strength = nullptr;
	EditorFileDialog *dialog = nullptr;
	bool filling = false;

	HewnScatter3D *_scatter() const;
	void _on_selected(int p_index, bool p_selected);
	void _on_add();
	void _on_files(const PackedStringArray &p_paths);
	void _on_all();
	void _on_none();
	void _on_remove();
	void _on_strength(double p_value);
	void _on_preview(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_thumb, const Variant &p_index);

protected:
	static void _bind_methods();

public:
	void setup(HewnEditorPlugin *p_plugin);
	void show_scatter(HewnScatter3D *p_scatter); // (null: closed)
	void refresh();
};

} // namespace godot
