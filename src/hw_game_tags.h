// What a node gives the game's nodes made of it: metadata, groups and scripts (a script goes only on a node of its kind).
#pragma once

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace hewn {

inline void apply_tags(godot::Node *p_node, const godot::Dictionary &p_meta, const godot::PackedStringArray &p_groups) {
	if (!p_node) {
		return;
	}
	const godot::Array keys = p_meta.keys();
	for (int64_t i = 0; i < keys.size(); i++) {
		p_node->set_meta(godot::StringName(godot::String(keys[i])), p_meta[keys[i]]);
	}
	for (int64_t i = 0; i < p_groups.size(); i++) {
		const godot::String g = p_groups[i].strip_edges();
		if (!g.is_empty()) {
			p_node->add_to_group(godot::StringName(g), true);
		}
	}
}

// False when it does not fit (a warning says so: a script made for a Hewn class has no such class in the game).
inline bool apply_script(godot::Node *p_node, const godot::Ref<godot::Script> &p_script) {
	if (!p_node || p_script.is_null()) {
		return false;
	}
	const godot::StringName base = p_script->get_instance_base_type();
	if (!godot::ClassDBSingleton::get_singleton()->is_parent_class(p_node->get_class(), base)) {
		godot::UtilityFunctions::push_warning(godot::vformat("Hewn: the script %s extends %s and cannot go on the game's %s (%s): left out.", p_script->get_path(), base, p_node->get_class(), p_node->get_name()));
		return false;
	}
	p_node->set_script(p_script);
	return true;
}

// The script of p_from on p_to, with the values set on it.
inline void copy_script(godot::Node *p_from, godot::Node *p_to) {
	if (!p_from || !p_to) {
		return;
	}
	const godot::Ref<godot::Script> s = p_from->get_script();
	if (s.is_null() || !apply_script(p_to, s)) {
		return;
	}
	const godot::TypedArray<godot::Dictionary> props = p_from->get_property_list();
	for (int64_t i = 0; i < props.size(); i++) {
		const godot::Dictionary d = props[i];
		if ((int64_t)d.get("usage", 0) & godot::PROPERTY_USAGE_SCRIPT_VARIABLE) {
			const godot::StringName n = d["name"];
			p_to->set(n, p_from->get(n));
		}
	}
}

} // namespace hewn
