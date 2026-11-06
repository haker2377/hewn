// Point entities: a spawn point, a target, anything the game looks up by its class (its group) and
// its properties (metadata). Placed like prefabs.
#pragma once

#include <godot_cpp/classes/area3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace godot {

// A point with a class name and key / values. In the game it is in the group of its class
// (get_tree().get_nodes_in_group("spawn_point")) and its values are get_value("team").
class HewnEntity3D : public Node3D {
	GDCLASS(HewnEntity3D, Node3D)

	String classname = "info_target";
	Dictionary properties;
	Color color = Color(1.0f, 0.6f, 0.1f);
	float size = 0.5f; // the marker in the editor
	MeshInstance3D *marker = nullptr; // (editor only, internal)
	void _update_marker();
	void _apply_meta();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_classname(const String &p_name);
	String get_classname() const { return classname; }
	void set_properties(const Dictionary &p_props);
	Dictionary get_properties() const { return properties; }
	Variant get_value(const String &p_key, const Variant &p_default = Variant()) const;
	void set_value(const String &p_key, const Variant &p_value);
	void set_color(const Color &p_color);
	Color get_color() const { return color; }
	void set_size(float p_size);
	float get_size() const { return size; }
};

} // namespace godot
