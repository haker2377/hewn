// UI language of the Hewn editor: English, or Russian when the
// editor runs in Russian. Our controls are translated by us only (Godot's own catalog would translate
// just the words it happens to know, mixing the languages).
#include "hw_editor_util.h"
#include "hw_i18n.gen.h"

#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/menu_button.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/tab_container.hpp>
#include <godot_cpp/classes/translation.hpp>
#include <godot_cpp/classes/translation_domain.hpp>
#include <godot_cpp/classes/translation_server.hpp>

#include <string>
#include <unordered_map>

using namespace godot;

namespace hw_editor {

namespace {
bool russian = false;
std::unordered_map<std::string, String> table;
Ref<Translation> property_translation;
} // namespace

void i18n_init() {
	String lang;
	OS *os = OS::get_singleton();
	if (!os->get_environment("HW_SELFTEST").is_empty()) {
		lang = "en"; // the self-test finds the controls by their English text
	} else if (!os->get_environment("HW_LANG").is_empty()) {
		lang = os->get_environment("HW_LANG");
	} else {
		Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
		if (es.is_valid() && es->has_setting("interface/editor/editor_language")) {
			lang = es->get_setting("interface/editor/editor_language");
		}
	}
	russian = lang.begins_with("ru");
	table.clear();
	if (!russian) {
		return;
	}
	property_translation.instantiate();
	property_translation->set_locale("ru");
	for (const auto &pair : HW_I18N_RU) {
		const String ru = String::utf8(pair[1]);
		table[pair[0]] = ru;
		property_translation->add_message(String::utf8(pair[0]), ru);
	}
	// Our property names in the inspector (when it shows localized names).
	TranslationServer::get_singleton()->get_or_add_domain("godot.properties")->add_translation(property_translation);
}

void i18n_finish() {
	if (property_translation.is_valid()) {
		TranslationServer::get_singleton()->get_or_add_domain("godot.properties")->remove_translation(property_translation);
		property_translation.unref();
	}
	table.clear();
}

String T(const String &p_text) {
	if (!russian || p_text.is_empty()) {
		return p_text;
	}
	auto it = table.find(std::string(p_text.utf8().get_data()));
	return it != table.end() ? it->second : p_text;
}

String T(const char *p_text) {
	if (!russian) {
		return String(p_text);
	}
	auto it = table.find(p_text);
	return it != table.end() ? it->second : String(p_text);
}

namespace {
// "0,5" typed into a number field becomes "0.5" at once (Godot's fields read only points).
void comma_to_point(const String &p_text, LineEdit *p_edit) {
	if (!p_text.contains(",")) {
		return;
	}
	const int caret = p_edit->get_caret_column();
	p_edit->set_text(p_text.replace(",", "."));
	p_edit->set_caret_column(caret);
}

// a number field follows the mouse dragged sideways with the left button held (a click without
// moving: typing as before). Shift: finer.
struct SpinDrag {
	bool down = false, dragging = false;
	float press_x = 0.0f;
	double start = 0.0;
};
SpinDrag g_spin_drag;

void spin_drag_input(const Ref<InputEvent> &p_event, SpinBox *p_box) {
	LineEdit *le = p_box->get_line_edit();
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		if (mb->is_pressed()) {
			g_spin_drag.down = true;
			g_spin_drag.dragging = false;
			g_spin_drag.press_x = mb->get_global_position().x;
			g_spin_drag.start = p_box->get_value();
		} else {
			if (g_spin_drag.dragging) {
				le->accept_event();
				le->release_focus();
			}
			g_spin_drag.down = false;
			g_spin_drag.dragging = false;
		}
		return;
	}
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_null() || !g_spin_drag.down || !(mm->get_button_mask().has_flag(MOUSE_BUTTON_MASK_LEFT))) {
		return;
	}
	const float dx = mm->get_global_position().x - g_spin_drag.press_x;
	if (!g_spin_drag.dragging && std::fabs(dx) > 4.0f) {
		g_spin_drag.dragging = true;
		le->deselect();
	}
	if (g_spin_drag.dragging) {
		const double step = p_box->get_step() > 0.0 ? p_box->get_step() : 0.01;
		const double per_px = MAX(step, 0.005) * (mm->is_shift_pressed() ? 0.1 : 1.0);
		p_box->set_value(g_spin_drag.start + per_px * dx);
		le->accept_event();
	}
}
} // namespace

void localize(Node *p_node) {
	if (!p_node) {
		return;
	}
	if (SpinBox *sb = Object::cast_to<SpinBox>(p_node)) {
		LineEdit *le = sb->get_line_edit();
		const Callable c = callable_mp_static(&comma_to_point).bind(le);
		if (le && !le->is_connected("text_changed", c)) {
			le->connect("text_changed", c);
			le->connect("gui_input", callable_mp_static(&spin_drag_input).bind(sb));
			le->set_default_cursor_shape(Control::CURSOR_HSIZE);
		}
	}
	if (p_node->has_meta("hw_no_translate")) {
		return; // (names that stay as they are: entity classes)
	}
	p_node->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED); // (we translate)
	if (!russian) {
		for (int i = 0; i < p_node->get_child_count(); i++) {
			localize(p_node->get_child(i));
		}
		if (MenuButton *mb = Object::cast_to<MenuButton>(p_node)) {
			localize(mb->get_popup());
		}
		return;
	}
	if (Control *c = Object::cast_to<Control>(p_node)) {
		c->set_tooltip_text(T(c->get_tooltip_text()));
	}
	if (OptionButton *ob = Object::cast_to<OptionButton>(p_node)) {
		for (int i = 0; i < ob->get_item_count(); i++) {
			ob->set_item_text(i, T(ob->get_item_text(i)));
		}
	} else if (Button *b = Object::cast_to<Button>(p_node)) {
		b->set_text(T(b->get_text()));
	} else if (Label *l = Object::cast_to<Label>(p_node)) {
		l->set_text(T(l->get_text()));
	}
	if (MenuButton *mb = Object::cast_to<MenuButton>(p_node)) {
		localize(mb->get_popup());
	}
	if (PopupMenu *pm = Object::cast_to<PopupMenu>(p_node)) {
		for (int i = 0; i < pm->get_item_count(); i++) {
			pm->set_item_text(i, T(pm->get_item_text(i)));
			pm->set_item_tooltip(i, T(pm->get_item_tooltip(i)));
		}
	}
	if (TabContainer *tc = Object::cast_to<TabContainer>(p_node)) {
		for (int i = 0; i < tc->get_tab_count(); i++) {
			tc->set_tab_title(i, T(tc->get_tab_title(i)));
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		localize(p_node->get_child(i));
	}
}

} // namespace hw_editor
