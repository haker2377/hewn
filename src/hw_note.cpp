#include "hw_note.h"

#include <algorithm>

using namespace godot;

std::vector<HewnNote3D *> HewnNote3D::all;

void HewnNote3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		all.push_back(this);
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		all.erase(std::remove(all.begin(), all.end(), this), all.end());
	}
}

void HewnNote3D::set_text(const String &p_text) {
	text = p_text;
	update_gizmos();
}

void HewnNote3D::set_color(const Color &p_color) {
	color = p_color;
	update_gizmos();
}

void HewnNote3D::set_done(bool p_done) {
	done = p_done;
	update_gizmos();
}

void HewnNote3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_text", "text"), &HewnNote3D::set_text);
	ClassDB::bind_method(D_METHOD("get_text"), &HewnNote3D::get_text);
	ClassDB::bind_method(D_METHOD("set_color", "color"), &HewnNote3D::set_color);
	ClassDB::bind_method(D_METHOD("get_color"), &HewnNote3D::get_color);
	ClassDB::bind_method(D_METHOD("set_done", "done"), &HewnNote3D::set_done);
	ClassDB::bind_method(D_METHOD("get_done"), &HewnNote3D::get_done);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "text", PROPERTY_HINT_MULTILINE_TEXT), "set_text", "get_text");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color"), "set_color", "get_color");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "done"), "set_done", "get_done");
}
