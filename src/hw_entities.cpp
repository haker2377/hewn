#include "hw_entities.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace {

Ref<StandardMaterial3D> unshaded(const Color &p_color, bool p_transparent) {
	Ref<StandardMaterial3D> m;
	m.instantiate();
	m->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	m->set_albedo(p_color);
	m->set_flag(BaseMaterial3D::FLAG_DISABLE_FOG, true);
	if (p_transparent) {
		m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		m->set_depth_draw_mode(BaseMaterial3D::DEPTH_DRAW_DISABLED);
	}
	return m;
}

// A diamond on a stem with an arrow along -Z (where the entity faces).
Ref<ArrayMesh> marker_mesh(float s) {
	PackedVector3Array v;
	const float h = s * 0.5f;
	const Vector3 c(0, h, 0);
	const Vector3 tip_u = c + Vector3(0, h, 0), tip_d = c - Vector3(0, h, 0);
	const Vector3 ring[4] = { c + Vector3(h * 0.6f, 0, 0), c + Vector3(0, 0, h * 0.6f), c + Vector3(-h * 0.6f, 0, 0), c + Vector3(0, 0, -h * 0.6f) };
	for (int i = 0; i < 4; i++) {
		const Vector3 a = ring[i], b = ring[(i + 1) % 4];
		v.push_back(tip_u);
		v.push_back(b);
		v.push_back(a);
		v.push_back(tip_d);
		v.push_back(a);
		v.push_back(b);
	}
	// the arrow on the ground
	const Vector3 f(0, 0.01f, -s * 1.2f), l(-s * 0.3f, 0.01f, -s * 0.6f), r(s * 0.3f, 0.01f, -s * 0.6f);
	v.push_back(f);
	v.push_back(r);
	v.push_back(l);
	Array arr;
	arr.resize(Mesh::ARRAY_MAX);
	arr[Mesh::ARRAY_VERTEX] = v;
	Ref<ArrayMesh> am;
	am.instantiate();
	am->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr);
	return am;
}

} // namespace

// ------------------------------------------------------------------ entity

void HewnEntity3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		if (!classname.is_empty()) {
			add_to_group(classname);
		}
		_apply_meta();
		_update_marker();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		if (!classname.is_empty() && is_in_group(classname)) {
			remove_from_group(classname);
		}
	}
}

void HewnEntity3D::_update_marker() {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return; // (seen in the editor only)
	}
	if (!marker) {
		marker = memnew(MeshInstance3D);
		marker->set_name("_Marker");
		marker->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(marker, false, INTERNAL_MODE_BACK);
	}
	marker->set_mesh(marker_mesh(size));
	marker->set_material_override(unshaded(color, false));
}

void HewnEntity3D::set_classname(const String &p_name) {
	if (is_inside_tree() && !classname.is_empty() && is_in_group(classname)) {
		remove_from_group(classname);
	}
	classname = p_name.strip_edges();
	if (is_inside_tree() && !classname.is_empty()) {
		add_to_group(classname);
	}
}

void HewnEntity3D::set_properties(const Dictionary &p_props) {
	properties = p_props.duplicate();
	_apply_meta();
}

// The properties also as metadata (get_meta("team")): the same in the game with or without Hewn.
void HewnEntity3D::_apply_meta() {
	const Array keys = properties.keys();
	for (int64_t i = 0; i < keys.size(); i++) {
		set_meta(StringName(String(keys[i])), properties[keys[i]]);
	}
}

Variant HewnEntity3D::get_value(const String &p_key, const Variant &p_default) const {
	return properties.get(p_key, p_default);
}

void HewnEntity3D::set_value(const String &p_key, const Variant &p_value) {
	properties[p_key] = p_value;
	set_meta(StringName(p_key), p_value);
}

void HewnEntity3D::set_color(const Color &p_color) {
	color = p_color;
	if (marker) {
		_update_marker();
	}
}

void HewnEntity3D::set_size(float p_size) {
	size = MAX(0.01f, p_size);
	if (marker) {
		_update_marker();
	}
}

void HewnEntity3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_classname", "name"), &HewnEntity3D::set_classname);
	ClassDB::bind_method(D_METHOD("get_classname"), &HewnEntity3D::get_classname);
	ClassDB::bind_method(D_METHOD("set_properties", "properties"), &HewnEntity3D::set_properties);
	ClassDB::bind_method(D_METHOD("get_properties"), &HewnEntity3D::get_properties);
	ClassDB::bind_method(D_METHOD("get_value", "key", "default"), &HewnEntity3D::get_value, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("set_value", "key", "value"), &HewnEntity3D::set_value);
	ClassDB::bind_method(D_METHOD("set_color", "color"), &HewnEntity3D::set_color);
	ClassDB::bind_method(D_METHOD("get_color"), &HewnEntity3D::get_color);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &HewnEntity3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &HewnEntity3D::get_size);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "classname"), "set_classname", "get_classname");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "properties"), "set_properties", "get_properties");
	ADD_GROUP("Editor", "");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_color", "get_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "size", PROPERTY_HINT_RANGE, "0.01,10,0.01,or_greater,suffix:m"), "set_size", "get_size");
}
