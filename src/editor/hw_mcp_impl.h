// Hewn editor - what the MCP server's parts share (hw_mcp*.cpp): includes, JSON <-> Godot values, the scene's
// nodes by path, answers, undo of a node's change, the scene's triangles.
#pragma once

#include "../hw_undo_state.h"

#include "hw_editor.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/marshalls.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include "hw_textures.h"
#include "../hw_hotspot_atlas.h"
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/editor_file_system_directory.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include "../hw_cutter.h"
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/undo_redo.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <functional>

using namespace godot;

#include "hw_mcp.h"

namespace hw_mcp {


inline const char *PROTOCOL = "2025-06-18";
inline const int MAX_BODY = 16 << 20;
inline const int MAX_ITEMS = 64; // (of an array in an answer: the rest counted; more with max_items)
inline int g_max_items = MAX_ITEMS;
inline bool g_precise = false;

inline Node *scene_root() {
	return EditorInterface::get_singleton()->get_edited_scene_root();
}

// A node by its path from the scene's root ("" / "." the root itself; "/root/..." as it is).
inline Node *find_node(const String &p_path) {
	Node *root = scene_root();
	if (!root) {
		return nullptr;
	}
	if (p_path.is_empty() || p_path == "." || p_path == String(root->get_name())) {
		return root;
	}
	if (p_path.begins_with("/")) {
		return root->get_tree() ? root->get_tree()->get_root()->get_node_or_null(NodePath(p_path)) : nullptr;
	}
	Node *n = root->get_node_or_null(NodePath(p_path));
	if (!n && p_path.begins_with(String(root->get_name()) + "/")) {
		n = root->get_node_or_null(NodePath(p_path.substr(String(root->get_name()).length() + 1)));
	}
	return n;
}

inline String path_of(Node *p_node) {
	Node *root = scene_root();
	if (!p_node || !root) {
		return String();
	}
	return p_node == root ? String(".") : String(root->get_path_to(p_node));
}

inline Variant num(double p_v) {
	if (std::isnan(p_v) || std::isinf(p_v)) {
		return String::num(p_v);
	}
	// (6 significant digits: 0.1 not 0.100000001490116 - a float's last digits are noise, and tokens; a tiny value
	// stays a tiny value)
	if (g_precise) {
		return p_v;
	}
	if (p_v == 0.0) {
		return (int64_t)0;
	}
	const int mag = (int)std::floor(std::log10(std::fabs(p_v)));
	const int places = CLAMP(5 - mag, 0, 15);
	const double k = std::pow(10.0, places);
	const double r = std::round(p_v * k) / k;
	return r == std::floor(r) && std::fabs(r) < 1e15 ? Variant((int64_t)r) : Variant(r);
}

// Godot values as JSON: vectors as arrays, transforms as their parts, nodes by path, resources by class and file.
inline Variant to_json(const Variant &p_v, int p_depth = 0) {
	switch (p_v.get_type()) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::STRING:
			return p_v;
		case Variant::FLOAT:
			return num(p_v);
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
			return String(p_v);
		case Variant::VECTOR2: {
			const Vector2 v = p_v;
			return Array::make(num(v.x), num(v.y));
		}
		case Variant::VECTOR2I: {
			const Vector2i v = p_v;
			return Array::make(v.x, v.y);
		}
		case Variant::VECTOR3: {
			const Vector3 v = p_v;
			return Array::make(num(v.x), num(v.y), num(v.z));
		}
		case Variant::VECTOR3I: {
			const Vector3i v = p_v;
			return Array::make(v.x, v.y, v.z);
		}
		case Variant::VECTOR4: {
			const Vector4 v = p_v;
			return Array::make(num(v.x), num(v.y), num(v.z), num(v.w));
		}
		case Variant::QUATERNION: {
			const Quaternion q = p_v;
			return Array::make(num(q.x), num(q.y), num(q.z), num(q.w));
		}
		case Variant::COLOR: {
			const Color c = p_v;
			return Array::make(num(c.r), num(c.g), num(c.b), num(c.a));
		}
		case Variant::RECT2: {
			const Rect2 r = p_v;
			Dictionary d;
			d["position"] = to_json(r.position);
			d["size"] = to_json(r.size);
			return d;
		}
		case Variant::AABB: {
			const AABB a = p_v;
			Dictionary d;
			d["position"] = to_json(a.position);
			d["size"] = to_json(a.size);
			return d;
		}
		case Variant::BASIS: {
			const Basis b = p_v;
			return Array::make(to_json(b.get_column(0)), to_json(b.get_column(1)), to_json(b.get_column(2)));
		}
		case Variant::TRANSFORM3D: {
			const Transform3D t = p_v;
			Dictionary d;
			d["origin"] = to_json(t.origin);
			d["basis"] = to_json(t.basis);
			d["euler_degrees"] = to_json(t.basis.get_euler() * (float)(180.0 / Math::PI));
			d["scale"] = to_json(t.basis.get_scale());
			return d;
		}
		case Variant::OBJECT: {
			Object *o = p_v;
			if (!o) {
				return Variant();
			}
			Dictionary d;
			d["class"] = o->get_class();
			if (Node *n = Object::cast_to<Node>(o)) {
				d["node"] = path_of(n);
			} else if (Resource *r = Object::cast_to<Resource>(o)) {
				if (!r->get_path().is_empty()) {
					d["resource"] = r->get_path();
				}
				if (!r->get_name().is_empty()) {
					d["name"] = r->get_name();
				}
			}
			return d;
		}
		case Variant::DICTIONARY: {
			if (p_depth > 6) {
				return String("{...}");
			}
			const Dictionary src = p_v;
			Dictionary d;
			const Array keys = src.keys();
			for (int64_t i = 0; i < keys.size(); i++) {
				d[String(keys[i])] = to_json(src[keys[i]], p_depth + 1);
			}
			return d;
		}
		default:
			break;
	}
	if (p_v.get_type() >= Variant::ARRAY) {
		// arrays and the packed ones
		if (p_depth > 6) {
			return String("[...]");
		}
		const Array a = p_v;
		Array out;
		const int64_t n = a.size();
		for (int64_t i = 0; i < n && i < g_max_items; i++) {
			out.push_back(to_json(a[i], p_depth + 1));
		}
		if (n > g_max_items) {
			out.push_back(vformat("... %d more (max_items for more)", (int64_t)(n - g_max_items)));
		}
		return out;
	}
	return UtilityFunctions::var_to_str(p_v);
}

inline double f(const Variant &p_v) {
	return p_v.get_type() == Variant::STRING ? String(p_v).to_float() : (double)p_v;
}

inline Vector3 v3(const Variant &p_v) {
	if (p_v.get_type() == Variant::VECTOR3) {
		return p_v;
	}
	const Array a = p_v;
	return a.size() >= 3 ? Vector3(f(a[0]), f(a[1]), f(a[2])) : Vector3();
}

// JSON as a Godot value of a type (the property's, the argument's): arrays as vectors, "Vector3(...)" and other Godot
// text too, a node by path, a resource by file.
inline Variant from_json(const Variant &p_v, Variant::Type p_t) {
	if (p_t == Variant::NIL || p_v.get_type() == p_t) {
		return p_v;
	}
	if (p_v.get_type() == Variant::STRING && p_t != Variant::STRING && p_t != Variant::STRING_NAME && p_t != Variant::NODE_PATH) {
		const String s = p_v;
		if (p_t == Variant::OBJECT) {
			if (s.begins_with("res://")) {
				return ResourceLoader::get_singleton()->load(s);
			}
			return find_node(s);
		}
		const Variant parsed = UtilityFunctions::str_to_var(s);
		if (parsed.get_type() == p_t) {
			return parsed;
		}
		if (parsed.get_type() != Variant::NIL) {
			return from_json(parsed, p_t);
		}
		return p_v;
	}
	if (p_t == Variant::OBJECT && p_v.get_type() == Variant::DICTIONARY) {
		// a resource made here: {"class": "StandardMaterial3D", "albedo_color": [1, 0, 0], ...}
		const Dictionary d = p_v;
		const String cls = d.get("class", "");
		if (cls.is_empty() || !ClassDB::class_exists(cls) || !ClassDB::can_instantiate(cls) || !ClassDB::is_parent_class(cls, "Resource")) {
			return Variant();
		}
		Ref<Resource> r = ClassDB::instantiate(cls);
		const Array keys = d.keys();
		for (int64_t i = 0; i < keys.size(); i++) {
			const String k = keys[i];
			if (k == "class") {
				continue;
			}
			const TypedArray<Dictionary> list = r->get_property_list();
			Variant::Type t = Variant::NIL;
			for (int64_t j = 0; j < list.size(); j++) {
				if (String(Dictionary(list[j])["name"]) == k) {
					t = (Variant::Type)(int)Dictionary(list[j])["type"];
				}
			}
			r->set(k, from_json(d[k], t));
		}
		return r;
	}
	const Array a = p_v.get_type() == Variant::ARRAY ? Array(p_v) : Array();
	switch (p_t) {
		case Variant::BOOL:
			return (bool)p_v;
		case Variant::INT:
			return (int64_t)std::llround(f(p_v));
		case Variant::FLOAT:
			return f(p_v);
		case Variant::STRING:
			return p_v.get_type() == Variant::NIL ? String() : String(p_v);
		case Variant::STRING_NAME:
			return StringName(String(p_v));
		case Variant::NODE_PATH:
			return NodePath(String(p_v));
		case Variant::VECTOR2:
			return a.size() >= 2 ? Variant(Vector2(f(a[0]), f(a[1]))) : p_v;
		case Variant::VECTOR2I:
			return a.size() >= 2 ? Variant(Vector2i((int)f(a[0]), (int)f(a[1]))) : p_v;
		case Variant::VECTOR3:
			return a.size() >= 3 ? Variant(v3(a)) : p_v;
		case Variant::VECTOR3I:
			return a.size() >= 3 ? Variant(Vector3i((int)f(a[0]), (int)f(a[1]), (int)f(a[2]))) : p_v;
		case Variant::VECTOR4:
			return a.size() >= 4 ? Variant(Vector4(f(a[0]), f(a[1]), f(a[2]), f(a[3]))) : p_v;
		case Variant::QUATERNION:
			return a.size() >= 4 ? Variant(Quaternion(f(a[0]), f(a[1]), f(a[2]), f(a[3]))) : p_v;
		case Variant::COLOR:
			if (p_v.get_type() == Variant::STRING) {
				return Color::html(String(p_v));
			}
			return a.size() >= 3 ? Variant(Color(f(a[0]), f(a[1]), f(a[2]), a.size() >= 4 ? f(a[3]) : 1.0)) : p_v;
		case Variant::BASIS:
			return a.size() >= 3 ? Variant(Basis(v3(a[0]), v3(a[1]), v3(a[2])).transposed()) : p_v; // (given as its columns)
		case Variant::TRANSFORM3D: {
			if (p_v.get_type() != Variant::DICTIONARY) {
				return p_v;
			}
			const Dictionary d = p_v;
			Transform3D t;
			if (d.has("basis")) {
				t.basis = from_json(d["basis"], Variant::BASIS);
			} else if (d.has("euler_degrees")) {
				t.basis = Basis::from_euler(v3(d["euler_degrees"]) * (float)(Math::PI / 180.0));
			}
			if (d.has("scale")) {
				t.basis.scale_local(v3(d["scale"]));
			}
			t.origin = d.has("origin") ? v3(d["origin"]) : Vector3();
			return t;
		}
		case Variant::AABB: {
			const Dictionary d = p_v;
			return AABB(v3(d.get("position", Array())), v3(d.get("size", Array())));
		}
		case Variant::PACKED_INT32_ARRAY: {
			PackedInt32Array out;
			for (int64_t i = 0; i < a.size(); i++) {
				if (a[i].get_type() == Variant::ARRAY) {
					const Array in = a[i];
					for (int64_t j = 0; j < in.size(); j++) {
						out.push_back((int)f(in[j])); // (pairs given as [a, b])
					}
				} else {
					out.push_back((int)f(a[i]));
				}
			}
			return out;
		}
		case Variant::PACKED_INT64_ARRAY: {
			PackedInt64Array out;
			for (int64_t i = 0; i < a.size(); i++) {
				out.push_back((int64_t)f(a[i]));
			}
			return out;
		}
		case Variant::PACKED_FLOAT32_ARRAY: {
			PackedFloat32Array out;
			for (int64_t i = 0; i < a.size(); i++) {
				out.push_back(f(a[i]));
			}
			return out;
		}
		case Variant::PACKED_FLOAT64_ARRAY: {
			PackedFloat64Array out;
			for (int64_t i = 0; i < a.size(); i++) {
				out.push_back(f(a[i]));
			}
			return out;
		}
		case Variant::PACKED_STRING_ARRAY: {
			PackedStringArray out;
			for (int64_t i = 0; i < a.size(); i++) {
				out.push_back(String(a[i]));
			}
			return out;
		}
		case Variant::PACKED_VECTOR2_ARRAY: {
			PackedVector2Array out;
			for (int64_t i = 0; i < a.size(); i++) {
				out.push_back(from_json(a[i], Variant::VECTOR2));
			}
			return out;
		}
		case Variant::PACKED_VECTOR3_ARRAY: {
			PackedVector3Array out;
			for (int64_t i = 0; i < a.size(); i++) {
				out.push_back(v3(a[i]));
			}
			return out;
		}
		case Variant::PACKED_COLOR_ARRAY: {
			PackedColorArray out;
			for (int64_t i = 0; i < a.size(); i++) {
				out.push_back(from_json(a[i], Variant::COLOR));
			}
			return out;
		}
		default:
			break;
	}
	return p_v;
}

// A property's type on an object (NIL: none such).
inline Variant::Type prop_type(Object *p_obj, const String &p_name, bool *r_found = nullptr) {
	const TypedArray<Dictionary> list = p_obj->get_property_list();
	for (int64_t i = 0; i < list.size(); i++) {
		const Dictionary d = list[i];
		if (String(d["name"]) == p_name) {
			if (r_found) {
				*r_found = true;
			}
			return (Variant::Type)(int)d["type"];
		}
	}
	if (r_found) {
		*r_found = false;
	}
	return Variant::NIL;
}

inline Dictionary schema_obj(const Dictionary &p_props, const PackedStringArray &p_required = PackedStringArray()) {
	Dictionary s;
	s["type"] = "object";
	s["properties"] = p_props;
	if (!p_required.is_empty()) {
		Array r;
		for (const String &x : p_required) {
			r.push_back(x);
		}
		s["required"] = r;
	}
	return s;
}

inline Dictionary prop(const String &p_type, const String &p_desc) {
	Dictionary d;
	if (!p_type.is_empty()) {
		d["type"] = p_type;
	}
	d["description"] = p_desc;
	return d;
}

inline Dictionary tool(const String &p_name, const String &p_desc, const Dictionary &p_schema) {
	Dictionary t;
	t["name"] = p_name;
	t["description"] = p_desc;
	t["inputSchema"] = p_schema;
	return t;
}

inline Dictionary text_result(const Variant &p_value, bool p_error = false) {
	Dictionary c;
	c["type"] = "text";
	c["text"] = p_value.get_type() == Variant::STRING ? String(p_value) : JSON::stringify(p_value, "", false); // (compact: no indents)
	Dictionary r;
	r["content"] = Array::make(c);
	r["isError"] = p_error;
	return r;
}

inline Dictionary fail(const String &p_why) {
	return text_result(p_why, true);
}

// The storage properties of a node (what a scene keeps of it): before and after a call, for its undo.
inline Dictionary snapshot(Object *p_obj) {
	Dictionary out;
	const TypedArray<Dictionary> list = p_obj->get_property_list();
	for (int64_t i = 0; i < list.size(); i++) {
		const Dictionary d = list[i];
		if (!((int)d["usage"] & PROPERTY_USAGE_STORAGE)) {
			continue;
		}
		const String name = d["name"];
		const Variant v = p_obj->get(name);
		out[name] = v.get_type() == Variant::DICTIONARY ? Variant(Dictionary(v).duplicate(true)) : v.get_type() == Variant::ARRAY ? Variant(Array(v).duplicate(true)) : v;
	}
	return out;
}

inline const char *MODE_NAMES[4] = { "object", "vertex", "edge", "face" };

// The tools listed: the merged ones in place of their parts (the parts still answer by their old names).
inline const char *MERGED_AWAY[] = { "raycast", "point_query", "overlaps", "measure", "undo", "redo", "play", "stop", "menu_ops", "run_menu_op", "new_scene", "open_scene",
	"save_scene" };

// "look_at": [x, y, z] among the values of a 3D node: its rotation turned to face that point (from its position in the
// values, else its own; -Z toward it, as cameras and lights look), the key taken out.
inline void look_at_value(Node *p_node, Dictionary &r_vals) {
	Node3D *n3 = Object::cast_to<Node3D>(p_node);
	if (!n3 || !r_vals.has("look_at")) {
		return;
	}
	const Vector3 target = v3(r_vals["look_at"]);
	r_vals.erase("look_at");
	const Vector3 at = r_vals.has("position") ? v3(r_vals["position"]) : n3->get_position();
	Node3D *parent = Object::cast_to<Node3D>(n3->get_parent());
	const Vector3 local_target = parent && parent->is_inside_tree() ? parent->get_global_transform().affine_inverse().xform(target) : target;
	const Vector3 dir = local_target - at;
	if (dir.length() < 1e-6f) {
		return;
	}
	const Vector3 up = std::fabs(dir.normalized().y) > 0.999f ? Vector3(0, 0, -1) : Vector3(0, 1, 0);
	const Basis b = Basis::looking_at(dir, up);
	r_vals["rotation_degrees"] = to_json(b.get_euler() * (float)(180.0 / Math::PI));
}

// A menu operation MCP may run: not one done with the mouse, not a submenu, none that opens a dialog (its label ends in
// "...") or reads / writes files.
inline bool menu_op_allowed(const String &p_label, const String &p_op) {
	if (p_op.begins_with("modal:") || p_op.begins_with("menu:") || p_op.begins_with("sub:") || p_op.begins_with("fav:")) {
		return false;
	}
	if (p_label.strip_edges().split("  ")[0].ends_with("...") || p_label.ends_with(String::chr(0x2026))) {
		return false;
	}
	const String lo = p_op.to_lower();
	for (const char *w : { "export", "import", "save", "load", "open", "file", "bake_scene", "browse" }) {
		if (lo.contains(w)) {
			return false;
		}
	}
	return true;
}



// A change of a node as one undo step: its storage properties before and after what p_do does.
inline int undoable(EditorUndoRedoManager *ur, Node *p_node, const String &p_name, const std::function<void()> &p_do) {
	if (HewnMesh3D *hm = Object::cast_to<HewnMesh3D>(p_node)) {
		// (a Hewn mesh: its whole state, compressed - its mesh_data uncompressed is megabytes on a dense one)
		const Dictionary sb = hm->get_state();
		const Dictionary before = snapshot(p_node);
		p_do();
		const Dictionary sa = hm->get_state();
		const Dictionary after = snapshot(p_node);
		ur->create_action(p_name, UndoRedo::MERGE_DISABLE, p_node);
		hw_undo_state(ur, true, hm, sa);
		hw_undo_state(ur, false, hm, sb);
		int changed = (sa == sb) ? 0 : 1;
		const Array keys = after.keys();
		for (int64_t i = 0; i < keys.size(); i++) {
			const String k = keys[i];
			if (k == "mesh_data" || k == "shape" || k == "shape_params" || k == "poly_points" || k == "roof_sides") {
				continue; // (in the state)
			}
			const Variant a = after[keys[i]], b = before.get(keys[i], Variant());
			if (a.get_type() != b.get_type() || !(a == b)) {
				ur->add_do_property(p_node, keys[i], a);
				ur->add_undo_property(p_node, keys[i], b);
				changed++;
			}
		}
		ur->commit_action(false);
		return changed;
	}
	const Dictionary before = snapshot(p_node);
	p_do();
	const Dictionary after = snapshot(p_node);
	ur->create_action(p_name, UndoRedo::MERGE_DISABLE, p_node);
	int changed = 0;
	const Array keys = after.keys();
	for (int64_t i = 0; i < keys.size(); i++) {
		const Variant a = after[keys[i]], b = before.get(keys[i], Variant());
		if (a.get_type() != b.get_type() || !(a == b)) {
			ur->add_do_property(p_node, keys[i], a);
			ur->add_undo_property(p_node, keys[i], b);
			changed++;
		}
	}
	ur->commit_action(false);
	return changed;
}

// The scene's meshes (Hewn's and Godot's) with their triangles in the world.
struct Tris {
	Node3D *node = nullptr;
	std::vector<Vector3> p; // (three a triangle)
	std::vector<int> face; // (a Hewn mesh's face of each; -1 otherwise)
	AABB box;
};

inline std::vector<Tris> scene_tris(Node *p_root) {
	std::vector<Tris> out;
	std::vector<Node *> stack = { p_root };
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(x);
		if (!mi || !mi->is_visible_in_tree() || (x != p_root && x->get_owner() != p_root)) {
			continue;
		}
		Tris t;
		t.node = mi;
		const Transform3D g = mi->get_global_transform();
		if (HewnMesh3D *hm = Object::cast_to<HewnMesh3D>(mi)) {
			const hewn::Mesh &m = hm->get_data();
			for (int f = 0; f < (int)m.faces.size(); f++) {
				const PackedInt32Array tr = hm->get_face_triangles(f);
				for (int64_t k = 0; k + 2 < tr.size(); k += 3) {
					for (int c = 0; c < 3; c++) {
						t.p.push_back(g.xform(m.pos[tr[k + c]]));
					}
					t.face.push_back(f);
				}
			}
		} else if (mi->get_mesh().is_valid()) {
			const PackedVector3Array fs = mi->get_mesh()->get_faces();
			for (int64_t k = 0; k + 2 < fs.size(); k += 3) {
				for (int c = 0; c < 3; c++) {
					t.p.push_back(g.xform(fs[k + c]));
				}
				t.face.push_back(-1);
			}
		}
		if (t.p.empty()) {
			continue;
		}
		t.box = AABB(t.p[0], Vector3());
		for (const Vector3 &q : t.p) {
			t.box.expand_to(q);
		}
		out.push_back(t);
	}
	return out;
}

// Where a ray meets a triangle (its distance; < 0: not). p_inset: only well inside it (on its edges: a touch, no crossing).
inline float ray_tri(const Vector3 &o, const Vector3 &d, const Vector3 &a, const Vector3 &b, const Vector3 &c, float p_inset = 0.0f) {
	const Vector3 e1 = b - a, e2 = c - a, pv = d.cross(e2);
	const float det = e1.dot(pv);
	if (std::fabs(det) < 1e-5f * MAX(1e-12f, e1.length() * e2.length())) { // (along the plane: no crossing)
		return -1.0f;
	}
	const float inv = 1.0f / det;
	const Vector3 tv = o - a;
	const float u = tv.dot(pv) * inv;
	if (u < p_inset || u > 1.0f - p_inset) {
		return -1.0f;
	}
	const Vector3 qv = tv.cross(e1);
	const float v = d.dot(qv) * inv;
	if (v < p_inset || u + v > 1.0f - p_inset) {
		return -1.0f;
	}
	const float t = e2.dot(qv) * inv;
	return t > 1e-6f ? t : -1.0f;
}

inline Vector3 closest_on_tri(const Vector3 &p, const Vector3 &a, const Vector3 &b, const Vector3 &c) {
	// (Ericson's: by the regions of the triangle)
	const Vector3 ab = b - a, ac = c - a, ap = p - a;
	const float d1 = ab.dot(ap), d2 = ac.dot(ap);
	if (d1 <= 0 && d2 <= 0) {
		return a;
	}
	const Vector3 bp = p - b;
	const float d3 = ab.dot(bp), d4 = ac.dot(bp);
	if (d3 >= 0 && d4 <= d3) {
		return b;
	}
	const float vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) {
		return a + ab * (d1 / (d1 - d3));
	}
	const Vector3 cp = p - c;
	const float d5 = ab.dot(cp), d6 = ac.dot(cp);
	if (d6 >= 0 && d5 <= d6) {
		return c;
	}
	const float vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) {
		return a + ac * (d2 / (d2 - d6));
	}
	const float va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
		return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
	}
	const float den = 1.0f / (va + vb + vc);
	return a + ab * (vb * den) + ac * (vc * den);
}

// Two nodes' meshes: 1 their surfaces cross (an edge of one through a triangle of the other) or one is inside the other,
// 0 not, -1 not both meshes.
inline int shapes_meet(Node *p_a, Node *p_b) {
	std::vector<Tris> ta = scene_tris(p_a), tb = scene_tris(p_b);
	auto own = [](std::vector<Tris> &v, Node *n) {
		Tris *t = nullptr;
		for (Tris &x : v) {
			if (x.node == n) {
				t = &x;
			}
		}
		return t;
	};
	Tris *a = own(ta, p_a), *b = own(tb, p_b);
	if (!a || !b) {
		return -1;
	}
	auto edges_cross = [](const Tris &p, const Tris &q) {
		for (size_t k = 0; k * 3 + 2 < p.p.size(); k++) {
			for (int e = 0; e < 3; e++) {
				const Vector3 s0 = p.p[k * 3 + e], s1 = p.p[k * 3 + (e + 1) % 3];
				const Vector3 d = s1 - s0;
				const float len = d.length();
				if (len < 1e-7f || !q.box.grow(1e-4f).intersects_segment(s0, s1)) {
					continue;
				}
				const Vector3 dn = d / len;
				for (size_t j = 0; j * 3 + 2 < q.p.size(); j++) {
					const float t = ray_tri(s0, dn, q.p[j * 3], q.p[j * 3 + 1], q.p[j * 3 + 2], 1e-3f);
					if (t > 1e-4f && t < len - 1e-4f) {
						return true;
					}
				}
			}
		}
		return false;
	};
	auto inside = [](const Vector3 &pt, const Tris &q) {
		if (!q.box.has_point(pt)) {
			return false;
		}
		const Vector3 dir = Vector3(1.0f, 0.0123f, 0.0071f).normalized();
		int cross = 0;
		for (size_t k = 0; k * 3 + 2 < q.p.size(); k++) {
			cross += ray_tri(pt, dir, q.p[k * 3], q.p[k * 3 + 1], q.p[k * 3 + 2]) >= 0.0f ? 1 : 0;
		}
		return cross % 2 == 1;
	};
	if (edges_cross(*a, *b) || edges_cross(*b, *a)) {
		return 1;
	}
	// (no crossing: one wholly inside the other, or apart)
	return (inside(a->box.get_center(), *b) && inside(a->p[0], *b)) || (inside(b->box.get_center(), *a) && inside(b->p[0], *a)) ? 1 : 0;
}

inline AABB world_box(Node *p_node) {
	VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(p_node);
	if (vi) {
		return vi->get_global_transform().xform(vi->get_aabb());
	}
	Node3D *n3 = Object::cast_to<Node3D>(p_node);
	return n3 ? AABB(n3->get_global_position(), Vector3()) : AABB();
}



inline PackedInt32Array to_packed_ints(const std::vector<int> &p_v) {
	PackedInt32Array out;
	for (int x : p_v) {
		out.push_back(x);
	}
	return out;
}

// Shapes drawn through points: their numbers.
inline int points_shape(const String &p_name) {
	const String n = p_name.to_lower().replace("_", " ");
	if (n == "wall") {
		return hewn::shapes::WALL;
	}
	if (n == "poly shape" || n == "polyshape" || n == "outline") {
		return hewn::shapes::POLY_SHAPE;
	}
	if (n == "path stairs" || n == "stairs") {
		return hewn::shapes::PATH_STAIRS;
	}
	if (n == "sweep") {
		return hewn::shapes::SWEEP;
	}
	return -1;
}


} // namespace hw_mcp

using namespace hw_mcp;
