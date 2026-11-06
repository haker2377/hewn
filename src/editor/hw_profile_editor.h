// A curve profile editor (Bevel's custom profile, Curve Mapping): the curve in a 2D view,
// points and handles dragged with the mouse; zoom, pan, clipping, the handle type of the selected point
// and its position typed; Godot's Curve / Curve2D in and out.
#pragma once

#include "../hw_profile.h"

#include <godot_cpp/classes/accept_dialog.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/editor_resource_picker.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/spin_box.hpp>

namespace godot {

class HewnEditorPlugin;

class HewnProfileView : public Control {
	GDCLASS(HewnProfileView, Control)

	Ref<HewnProfile> profile;
	Rect2 view = Rect2(-0.1f, -0.1f, 1.2f, 1.2f); // (profile space shown)
	Rect2 home = Rect2(-0.1f, -0.1f, 1.2f, 1.2f);
	Rect2 clip_rect = Rect2(0, 0, 1, 1);
	bool clipping = true;
	int selected = -1;
	int drag = 0; // 1 point, 2 handle in, 3 handle out, 4 pan
	Vector2 drag_from;
	Rect2 drag_view;
	Dictionary before;
	bool moved = false;

	Vector2 to_screen(const Vector2 &p) const;
	Vector2 to_profile(const Vector2 &s) const;
	void _commit();

protected:
	static void _bind_methods();

public:
	HewnEditorPlugin *plugin = nullptr;
	void _gui_input(const Ref<InputEvent> &p_event) override;
	void _draw() override;
	void set_profile(const Ref<HewnProfile> &p_profile, const Rect2 &p_clip);
	Ref<HewnProfile> get_profile() const { return profile; }
	int get_selected() const { return selected; }
	void select(int p_index);
	void zoom(float p_factor, const Vector2 &p_at);
	void reset_view();
	void set_clipping(bool p_on);
	bool is_clipping() const { return clipping; }
	void edit(const Dictionary &p_before, const String &p_name); // an undoable step: before -> now
};

class HewnProfileDialog : public AcceptDialog {
	GDCLASS(HewnProfileDialog, AcceptDialog)

	HewnProfileView *canvas = nullptr;
	Button *types[4] = {};
	SpinBox *px = nullptr, *py = nullptr;
	CheckBox *straight = nullptr, *equal = nullptr, *symmetric = nullptr;
	bool symmetry_allowed = false;
	SpinBox *res = nullptr;
	Button *clip_button = nullptr;
	EditorResourcePicker *curve_picker = nullptr;
	Dictionary reset_state;
	bool updating = false;

	void _sync();
	void _on_type(int p_type);
	void _on_xy(double p_value);
	void _on_sampling(bool p_unused);
	void _on_symmetric(bool p_on);
	void _on_res(double p_value);
	void _on_zoom(float p_factor);
	void _on_reset_view();
	void _on_reset_curve();
	void _on_clip(bool p_on);
	void _on_curve_picked(const Ref<Resource> &p_res);
	void _on_export(bool p_2d);

protected:
	static void _bind_methods();

public:
	void setup(HewnEditorPlugin *p_plugin);
	void open(const Ref<HewnProfile> &p_profile, const String &p_title, const Rect2 &p_clip, bool p_symmetry = false);
};

} // namespace godot
