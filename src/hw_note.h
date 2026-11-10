// A TODO note left in the level: a text at a place, for the one making the game (what is planned there). Shown in
// the editor only (the Notes overlay); the game has nothing of it.
#pragma once

#include <godot_cpp/classes/node3d.hpp>

#include <vector>

namespace godot {

class HewnNote3D : public Node3D {
	GDCLASS(HewnNote3D, Node3D);

	String text;
	Color color = Color(1.0f, 0.85f, 0.25f);
	bool done = false;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static std::vector<HewnNote3D *> all;

	void set_text(const String &p_text);
	String get_text() const { return text; }
	void set_color(const Color &p_color);
	Color get_color() const { return color; }
	void set_done(bool p_done);
	bool get_done() const { return done; }
};

} // namespace godot
