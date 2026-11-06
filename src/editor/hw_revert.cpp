// Godot inspector's revert arrow next to Hewn's fields: shown when the value differs from its default, a
// click puts the default back (through the field's own signal, so it is applied and undone like an edit).
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/color_picker_button.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/range.hpp>
#include <godot_cpp/classes/theme.hpp>

using namespace godot;

void HewnRevertButton::_bind_methods() {
}

HewnRevertButton::HewnRevertButton() {
	set_flat(true);
	set_focus_mode(FOCUS_NONE);
	set_tooltip_text(hw_editor::T("Revert to the default"));
	connect("pressed", callable_mp(this, &HewnRevertButton::_on_pressed));
}

Variant HewnRevertButton::field_value(Control *p_field) {
	if (HewnDragField *d = Object::cast_to<HewnDragField>(p_field)) {
		return d->get_value();
	}
	if (Range *r = Object::cast_to<Range>(p_field)) {
		return r->get_value();
	}
	if (OptionButton *o = Object::cast_to<OptionButton>(p_field)) {
		return o->get_selected();
	}
	if (ColorPickerButton *c = Object::cast_to<ColorPickerButton>(p_field)) {
		return c->get_pick_color();
	}
	if (BaseButton *b = Object::cast_to<BaseButton>(p_field)) {
		return b->is_pressed();
	}
	return Variant();
}

bool HewnRevertButton::differs() const {
	Control *f = Object::cast_to<Control>(ObjectDB::get_instance(field));
	if (!f || def.get_type() == Variant::NIL) {
		return false;
	}
	const Variant v = field_value(f);
	switch (v.get_type()) {
		case Variant::FLOAT:
		case Variant::INT: {
			const double a = (double)v, b = (double)def;
			return std::fabs(a - b) > 1e-6 * MAX(1.0, std::fabs(b));
		}
		case Variant::COLOR:
			return !Color(v).is_equal_approx(Color(def));
		case Variant::BOOL:
			return (bool)v != (bool)def;
		default:
			return false;
	}
}

void HewnRevertButton::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		set_button_icon(EditorInterface::get_singleton()->get_editor_theme()->get_icon("ReloadSmall", "EditorIcons"));
		set_process(true);
		_refresh();
	} else if (p_what == NOTIFICATION_PROCESS) {
		if (is_visible_in_tree() || (get_parent() && Object::cast_to<Control>(get_parent()) && Object::cast_to<Control>(get_parent())->is_visible_in_tree())) {
			_refresh();
		}
	}
}

// Shown or not without changing the layout (the room stays): no jumping rows, no relayout in the scroll boxes.
void HewnRevertButton::_refresh() {
	Control *f = Object::cast_to<Control>(ObjectDB::get_instance(field));
	if (!f) {
		return;
	}
	const bool on = differs() && !(Object::cast_to<BaseButton>(f) && Object::cast_to<BaseButton>(f)->is_disabled());
	if (on != shown) {
		shown = on;
		set_modulate(Color(1, 1, 1, on ? 1.0f : 0.0f));
		set_mouse_filter(on ? MOUSE_FILTER_STOP : MOUSE_FILTER_IGNORE);
		set_disabled(!on);
	}
	// the holder follows the field's own visibility (code shows / hides the field, not the holder)
	if (Control *holder = Object::cast_to<Control>(get_parent())) {
		if (f->is_visible() != field_visible) {
			field_visible = f->is_visible();
			holder->set_visible(field_visible);
		}
	}
}

void HewnRevertButton::_on_pressed() {
	Control *f = Object::cast_to<Control>(ObjectDB::get_instance(field));
	if (!f || !differs()) {
		return;
	}
	if (HewnDragField *d = Object::cast_to<HewnDragField>(f)) {
		d->set_value((double)def);
	} else if (Range *r = Object::cast_to<Range>(f)) {
		r->set_value((double)def);
	} else if (OptionButton *o = Object::cast_to<OptionButton>(f)) {
		const int i = (int)def;
		o->select(i);
		o->emit_signal("item_selected", i);
	} else if (ColorPickerButton *c = Object::cast_to<ColorPickerButton>(f)) {
		c->set_pick_color(def);
		c->emit_signal("color_changed", Color(def));
	} else if (BaseButton *b = Object::cast_to<BaseButton>(f)) {
		b->set_pressed((bool)def); // (emits toggled)
	}
	_refresh();
}

void HewnRevertButton::setup(Control *p_field, const Variant &p_default) {
	field = p_field ? p_field->get_instance_id() : ObjectID();
	def = p_default;
	field_visible = p_field ? p_field->is_visible() : true;
}

namespace hw_editor {

Control *with_revert(Control *p_field, const Variant &p_default) {
	HBoxContainer *h = memnew(HBoxContainer);
	h->set_name(String(p_field->get_name()).validate_node_name() + "_Revert");
	h->set_meta("hw_revert_holder", true);
	h->add_theme_constant_override("separation", 0);
	h->set_h_size_flags(p_field->get_h_size_flags());
	h->set_v_size_flags(p_field->get_v_size_flags());
	h->set_visible(p_field->is_visible());
	p_field->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	h->add_child(p_field);
	HewnRevertButton *r = memnew(HewnRevertButton);
	r->setup(p_field, p_default);
	r->set_modulate(Color(1, 1, 1, 0));
	r->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	r->set_disabled(true);
	h->add_child(r);
	return h;
}

void add_revert(Control *p_field, const Variant &p_default) {
	Node *parent = p_field ? p_field->get_parent() : nullptr;
	if (!parent || Object::cast_to<HewnRevertButton>(p_field) || parent->has_meta("hw_revert_holder")) {
		return;
	}
	const int at = p_field->get_index();
	parent->remove_child(p_field);
	Control *h = with_revert(p_field, p_default);
	parent->add_child(h);
	parent->move_child(h, at);
}

// Every field under `p_root` gets the arrow, its default being the value it has now (call it when the panel is
// made, before anything changed). Not: fields in the hidden / skipped containers named in `p_skip`.
void add_reverts_under(Node *p_root, const PackedStringArray &p_skip) {
	std::vector<Control *> found;
	std::vector<Node *> stack = { p_root };
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		if (p_skip.has(String(x->get_name())) || Object::cast_to<HewnRevertButton>(x)) {
			continue;
		}
		Control *c = Object::cast_to<Control>(x);
		const bool field = c && (Object::cast_to<HewnDragField>(c) || Object::cast_to<SpinBox>(c) || Object::cast_to<OptionButton>(c) ||
										Object::cast_to<ColorPickerButton>(c) || Object::cast_to<CheckBox>(c) || x->is_class("EditorSpinSlider"));
		if (field && !(Object::cast_to<OptionButton>(c) && Object::cast_to<OptionButton>(c)->get_item_count() == 0)) {
			found.push_back(c);
			continue; // (not inside: a SpinBox's own line edit)
		}
		for (int i = x->get_child_count() - 1; i >= 0; i--) {
			stack.push_back(x->get_child(i));
		}
	}
	for (Control *c : found) {
		add_revert(c, HewnRevertButton::field_value(c));
	}
}

} // namespace hw_editor
