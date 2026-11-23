// Hewn editor — the MCP server (hw_mcp.h): HTTP on 127.0.0.1, JSON-RPC, the tools.

#include "hw_mcp.h"

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
#include <godot_cpp/classes/visual_instance3d.hpp>
#include "hw_textures.h"
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
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

namespace {

const char *PROTOCOL = "2025-06-18";
const int MAX_BODY = 16 << 20;
const int MAX_ITEMS = 512; // (of an array in an answer: the rest counted)

Node *scene_root() {
	return EditorInterface::get_singleton()->get_edited_scene_root();
}

// A node by its path from the scene's root ("" / "." the root itself; "/root/..." as it is).
Node *find_node(const String &p_path) {
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

String path_of(Node *p_node) {
	Node *root = scene_root();
	if (!p_node || !root) {
		return String();
	}
	return p_node == root ? String(".") : String(root->get_path_to(p_node));
}

Variant num(double p_v) {
	if (std::isnan(p_v) || std::isinf(p_v)) {
		return String::num(p_v);
	}
	return p_v;
}

// Godot values as JSON: vectors as arrays, transforms as their parts, nodes by path, resources by class and file.
Variant to_json(const Variant &p_v, int p_depth = 0) {
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
		for (int64_t i = 0; i < n && i < MAX_ITEMS; i++) {
			out.push_back(to_json(a[i], p_depth + 1));
		}
		if (n > MAX_ITEMS) {
			out.push_back(vformat("... %d more", (int64_t)(n - MAX_ITEMS)));
		}
		return out;
	}
	return UtilityFunctions::var_to_str(p_v);
}

double f(const Variant &p_v) {
	return p_v.get_type() == Variant::STRING ? String(p_v).to_float() : (double)p_v;
}

Vector3 v3(const Variant &p_v) {
	if (p_v.get_type() == Variant::VECTOR3) {
		return p_v;
	}
	const Array a = p_v;
	return a.size() >= 3 ? Vector3(f(a[0]), f(a[1]), f(a[2])) : Vector3();
}

// JSON as a Godot value of a type (the property's, the argument's): arrays as vectors, "Vector3(...)" and other Godot
// text too, a node by path, a resource by file.
Variant from_json(const Variant &p_v, Variant::Type p_t) {
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
Variant::Type prop_type(Object *p_obj, const String &p_name, bool *r_found = nullptr) {
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

Dictionary schema_obj(const Dictionary &p_props, const PackedStringArray &p_required = PackedStringArray()) {
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

Dictionary prop(const String &p_type, const String &p_desc) {
	Dictionary d;
	if (!p_type.is_empty()) {
		d["type"] = p_type;
	}
	d["description"] = p_desc;
	return d;
}

Dictionary tool(const String &p_name, const String &p_desc, const Dictionary &p_schema) {
	Dictionary t;
	t["name"] = p_name;
	t["description"] = p_desc;
	t["inputSchema"] = p_schema;
	return t;
}

Dictionary text_result(const Variant &p_value, bool p_error = false) {
	Dictionary c;
	c["type"] = "text";
	c["text"] = p_value.get_type() == Variant::STRING ? String(p_value) : JSON::stringify(p_value, "  ", false);
	Dictionary r;
	r["content"] = Array::make(c);
	r["isError"] = p_error;
	return r;
}

Dictionary fail(const String &p_why) {
	return text_result(p_why, true);
}

// The storage properties of a node (what a scene keeps of it): before and after a call, for its undo.
Dictionary snapshot(Object *p_obj) {
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

const char *MODE_NAMES[4] = { "object", "vertex", "edge", "face" };

// A menu operation MCP may run: not one done with the mouse, not a submenu, none that opens a dialog (its label ends in
// "...") or reads / writes files.
bool menu_op_allowed(const String &p_label, const String &p_op) {
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

} // namespace

void HewnMCPServer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("handle_request", "request"), &HewnMCPServer::handle_request);
	ClassDB::bind_method(D_METHOD("get_port"), &HewnMCPServer::get_port);
	ClassDB::bind_method(D_METHOD("is_listening"), &HewnMCPServer::is_listening);
}

Error HewnMCPServer::start(HewnEditorPlugin *p_plugin, int p_port, const String &p_token, bool p_scripts) {
	stop();
	plugin = p_plugin;
	token = p_token;
	allow_scripts = p_scripts;
	server.instantiate();
	const Error e = server->listen(p_port, "127.0.0.1"); // (this computer only)
	if (e != OK) {
		server.unref();
		return e;
	}
	port = p_port;
	session = String::num_int64((int64_t)Time::get_singleton()->get_ticks_usec(), 16) + String::num_int64(UtilityFunctions::randi(), 16);
	set_process(true);
	return OK;
}

void HewnMCPServer::stop() {
	for (Conn &c : conns) {
		c.peer->disconnect_from_host();
	}
	conns.clear();
	if (server.is_valid()) {
		server->stop();
		server.unref();
	}
	set_process(false);
}

bool HewnMCPServer::is_listening() const {
	return server.is_valid() && server->is_listening();
}

void HewnMCPServer::_notification(int p_what) {
	if (p_what == NOTIFICATION_EXIT_TREE) {
		stop();
		return;
	}
	if (p_what != NOTIFICATION_PROCESS || server.is_null()) {
		return;
	}
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	while (server->is_connection_available()) {
		Conn c;
		c.peer = server->take_connection();
		if (c.peer.is_null()) {
			break;
		}
		c.peer->set_no_delay(true);
		c.last = now;
		conns.push_back(c);
	}
	for (size_t i = 0; i < conns.size();) {
		Conn &c = conns[i];
		c.peer->poll();
		bool keep = c.peer->get_status() == StreamPeerTCP::STATUS_CONNECTED;
		const int avail = keep ? c.peer->get_available_bytes() : 0;
		if (avail > 0) {
			const Array got = c.peer->get_partial_data(avail);
			if (got.size() == 2 && (int)got[0] == OK) {
				c.buf.append_array(PackedByteArray(got[1]));
				c.last = now;
			}
		}
		// whole requests in the buffer: answered one by one
		while (keep) {
			// (the head ends at the blank line)
			int64_t end = -1;
			for (int64_t k = 0; k + 3 < c.buf.size(); k++) {
				if (c.buf[k] == '\r' && c.buf[k + 1] == '\n' && c.buf[k + 2] == '\r' && c.buf[k + 3] == '\n') {
					end = k;
					break;
				}
			}
			if (end < 0) {
				if (c.buf.size() > 65536) {
					keep = false; // (no head that long)
				}
				break;
			}
			const String head = c.buf.slice(0, end).get_string_from_ascii();
			const PackedStringArray lines = head.split("\r\n");
			const PackedStringArray first = lines.size() > 0 ? lines[0].split(" ") : PackedStringArray();
			Dictionary headers;
			for (int64_t l = 1; l < lines.size(); l++) {
				const int colon = lines[l].find(":");
				if (colon > 0) {
					headers[lines[l].substr(0, colon).strip_edges().to_lower()] = lines[l].substr(colon + 1).strip_edges();
				}
			}
			const int64_t len = String(headers.get("content-length", "0")).to_int();
			if (first.size() < 2 || len < 0 || len > MAX_BODY) {
				_send(c, 400, "Bad Request", "");
				keep = false;
				break;
			}
			if (c.buf.size() < end + 4 + len) {
				break; // (the body still coming)
			}
			const PackedByteArray body = c.buf.slice(end + 4, end + 4 + len);
			c.buf = c.buf.slice(end + 4 + len);
			keep = _http(c, first[0], first[1], headers, body);
		}
		if (keep && now - c.last > 30000) {
			keep = false; // (idle)
		}
		if (!keep) {
			c.peer->disconnect_from_host();
			conns.erase(conns.begin() + (int64_t)i);
			continue;
		}
		i++;
	}
}

void HewnMCPServer::_send(Conn &c, int p_code, const String &p_status, const String &p_body, const Dictionary &p_headers) {
	const PackedByteArray body = p_body.to_utf8_buffer();
	String head = vformat("HTTP/1.1 %d %s\r\n", p_code, p_status);
	if (!p_body.is_empty()) {
		head += "Content-Type: application/json\r\n";
	}
	head += vformat("Content-Length: %d\r\n", (int64_t)body.size());
	const Array keys = p_headers.keys();
	for (int64_t i = 0; i < keys.size(); i++) {
		head += String(keys[i]) + ": " + String(p_headers[keys[i]]) + "\r\n";
	}
	head += "\r\n";
	PackedByteArray out = head.to_utf8_buffer();
	out.append_array(body);
	c.peer->put_data(out);
}

bool HewnMCPServer::_http(Conn &c, const String &p_method, const String &p_path, const Dictionary &p_headers, const PackedByteArray &p_body) {
	const bool close = String(p_headers.get("connection", "")).to_lower() == "close";
	// only from this computer's pages and tools: a page of a site can't reach it through the browser
	const String origin = p_headers.get("origin", "");
	if (!origin.is_empty() && !(origin.begins_with("http://127.0.0.1") || origin.begins_with("http://localhost") || origin.begins_with("https://127.0.0.1") ||
										origin.begins_with("https://localhost"))) {
		_send(c, 403, "Forbidden", "");
		return false;
	}
	const String host = String(p_headers.get("host", "")).to_lower();
	if (!host.is_empty() && !(host.begins_with("127.0.0.1") || host.begins_with("localhost") || host.begins_with("[::1]"))) {
		_send(c, 403, "Forbidden", "");
		return false;
	}
	if (!token.is_empty() && String(p_headers.get("authorization", "")) != "Bearer " + token) {
		_send(c, 401, "Unauthorized", "");
		return false;
	}
	const String path = p_path.split("?")[0];
	if (path != "/mcp" && path != "/mcp/") {
		_send(c, 404, "Not Found", "");
		return !close;
	}
	if (p_method == "GET") {
		Dictionary h;
		h["Allow"] = "POST, DELETE";
		_send(c, 405, "Method Not Allowed", "", h); // (no stream of its own to the client)
		return !close;
	}
	if (p_method == "DELETE") {
		_send(c, 200, "OK", "");
		return !close;
	}
	if (p_method != "POST") {
		_send(c, 405, "Method Not Allowed", "");
		return !close;
	}
	const Variant req = JSON::parse_string(p_body.get_string_from_utf8());
	Dictionary h;
	h["Mcp-Session-Id"] = session;
	if (req.get_type() == Variant::ARRAY) {
		// a batch
		const Array in = req;
		Array out;
		for (int64_t i = 0; i < in.size(); i++) {
			bool reply = false;
			const Variant r = _rpc(in[i], reply);
			if (reply) {
				out.push_back(r);
			}
		}
		if (out.is_empty()) {
			_send(c, 202, "Accepted", "", h);
		} else {
			_send(c, 200, "OK", JSON::stringify(out, "", false), h);
		}
		return !close;
	}
	if (req.get_type() != Variant::DICTIONARY) {
		Dictionary err;
		err["jsonrpc"] = "2.0";
		err["id"] = Variant();
		Dictionary e;
		e["code"] = -32700;
		e["message"] = "Parse error";
		err["error"] = e;
		_send(c, 400, "Bad Request", JSON::stringify(err, "", false), h);
		return !close;
	}
	bool reply = false;
	const Variant r = _rpc(req, reply);
	if (reply) {
		_send(c, 200, "OK", JSON::stringify(r, "", false), h);
	} else {
		_send(c, 202, "Accepted", "", h);
	}
	return !close;
}

Dictionary HewnMCPServer::handle_request(const Dictionary &p_req) {
	bool reply = false;
	const Variant r = _rpc(p_req, reply);
	return reply ? Dictionary(r) : Dictionary();
}

Variant HewnMCPServer::_rpc(const Dictionary &p_req, bool &r_reply) {
	const String method = p_req.get("method", "");
	r_reply = p_req.has("id"); // (a notification: no answer)
	Dictionary out;
	out["jsonrpc"] = "2.0";
	Variant id = p_req.get("id", Variant());
	if (id.get_type() == Variant::FLOAT && (double)id == std::floor((double)id) && std::fabs((double)id) < 9e15) {
		id = (int64_t)(double)id; // (JSON's numbers come as floats: the client's 1 goes back as 1, not 1.0)
	}
	out["id"] = id;
	const Dictionary params = p_req.get("params", Dictionary());
	if (method == "initialize") {
		Dictionary res;
		const String asked = params.get("protocolVersion", "");
		res["protocolVersion"] = (asked == "2025-06-18" || asked == "2025-03-26" || asked == "2024-11-05") ? asked : String(PROTOCOL);
		Dictionary caps;
		Dictionary tools;
		tools["listChanged"] = false;
		caps["tools"] = tools;
		res["capabilities"] = caps;
		Dictionary info;
		info["name"] = "hewn";
		info["title"] = "Hewn (Godot editor)";
		info["version"] = "0.6";
		res["serverInfo"] = info;
		res["instructions"] = "The Godot editor with Hewn, the scene open in it. Paths of nodes are from the scene's root. Start with editor_state and scene_tree; "
							  "Hewn meshes (HewnMesh3D) are shapes with parameters (shapes) or edited meshes: list_methods / call_method on them for their "
							  "operations (extrude_faces, bevel_edges, ...), select_elements first; every change is one undo step (undo / redo).";
		out["result"] = res;
		return out;
	}
	if (method == "ping") {
		out["result"] = Dictionary();
		return out;
	}
	if (method == "tools/list") {
		Dictionary res;
		res["tools"] = tool_list();
		out["result"] = res;
		return out;
	}
	if (method == "tools/call") {
		out["result"] = _tool(params.get("name", ""), params.get("arguments", Dictionary()));
		return out;
	}
	if (method.begins_with("notifications/")) {
		r_reply = false;
		return Variant();
	}
	Dictionary e;
	e["code"] = -32601;
	e["message"] = "Method not found: " + method;
	out["error"] = e;
	return out;
}

Array HewnMCPServer::tool_list() {
	Array t;
	Dictionary none;
	{
		t.push_back(tool("editor_state", "The open scene, the selected nodes, the Hewn mode (object / vertex / edge / face) and the mesh in it, the 3D cursor.", schema_obj(none)));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The node to start at (default: the scene's root)");
		p["depth"] = prop("integer", "How deep (default 4)");
		t.push_back(tool("scene_tree", "The scene's nodes: path, class, name, position of 3D ones, children.", schema_obj(p)));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The node's path from the scene's root");
		p["properties"] = prop("array", "Only these properties (default: all shown in the Inspector)");
		t.push_back(tool("node_info", "A node's properties (as in the Inspector); for a Hewn mesh also its counts, bounds, shape parameters and selection.", schema_obj(p, { "path" })));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The node's path");
		p["values"] = prop("object", "property: value. Vectors as [x, y, z], colors as [r, g, b, a] or \"#rrggbb\", transforms as {origin, euler_degrees, scale}, "
									 "resources as \"res://...\", or Godot text like \"Vector3(1, 2, 3)\"");
		t.push_back(tool("set_properties", "Set properties of a node (one undo step).", schema_obj(p, { "path", "values" })));
	}
	{
		Dictionary p;
		p["type"] = prop("string", "A node class (Node3D, OmniLight3D, HewnMesh3D, ...)");
		p["parent"] = prop("string", "The parent's path (default: the scene's root)");
		p["name"] = prop("string", "Its name");
		p["values"] = prop("object", "Properties to set (as set_properties)");
		p["shape"] = prop("string", "HewnMesh3D: the shape's name or number (see shapes)");
		p["shape_params"] = prop("object", "HewnMesh3D: the shape's parameters by name");
		p["dry_run"] = prop("boolean", "Only tell its size, add nothing");
		t.push_back(tool("create_node", "Add a node to the scene (one undo step).", schema_obj(p, { "type" })));
	}
	{
		Dictionary p;
		p["paths"] = prop("array", "The nodes' paths");
		t.push_back(tool("delete_nodes", "Delete nodes (one undo step).", schema_obj(p, { "paths" })));
	}
	{
		Dictionary p;
		p["paths"] = prop("array", "The nodes' paths (empty: none)");
		t.push_back(tool("select_nodes", "Select nodes in the editor.", schema_obj(p, { "paths" })));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The node's path");
		p["all"] = prop("boolean", "Also Godot's own classes' methods (default: only Hewn's)");
		t.push_back(tool("list_methods", "The methods of a node with their arguments (a Hewn mesh's operations: extrude_faces, bevel_edges, loop_cut, ...).", schema_obj(p, { "path" })));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The node's path");
		p["method"] = prop("string", "The method");
		p["args"] = prop("", "Its arguments: an array in order, or an object by name (converted as set_properties)");
		p["dry_run"] = prop("boolean", "Do it, tell the result (and a Hewn mesh's check), take it back: nothing changes");
		t.push_back(tool("call_method", "Call a method of a node; what it changes of the node is one undo step.", schema_obj(p, { "path", "method" })));
	}
	{
		t.push_back(tool("shapes", "Hewn's shapes (for create_node type HewnMesh3D) with their parameters: default, min, max, choices.", schema_obj(none)));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The Hewn mesh (not needed for object)");
		p["mode"] = prop("string", "object, vertex, edge or face");
		t.push_back(tool("edit_mode", "Hewn's mode: object, or editing a mesh's vertices / edges / faces.", schema_obj(p, { "mode" })));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The Hewn mesh");
		p["vertices"] = prop("array", "Vertex indices");
		p["edges"] = prop("array", "Edges as [a, b] vertex pairs");
		p["faces"] = prop("array", "Face indices");
		p["add"] = prop("boolean", "Added to what is selected (default: in its place)");
		t.push_back(tool("select_elements", "Select a Hewn mesh's vertices / edges / faces (what its operations work on).", schema_obj(p, { "path" })));
	}
	{
		Dictionary p;
		p["mode"] = prop("string", "object, vertex, edge or face (default: the current one)");
		t.push_back(tool("menu_ops", "Hewn's menu operations for a mode (what the right click menu has, without the ones done with the mouse).", schema_obj(p)));
	}
	{
		Dictionary p;
		p["op"] = prop("string", "The operation's id (from menu_ops)");
		t.push_back(tool("run_menu_op", "Run a Hewn menu operation on the selection (as the menu does; one undo step).", schema_obj(p, { "op" })));
	}
	{
		Dictionary p;
		p["viewport"] = prop("integer", "Which 3D view (0..3, default 0)");
		p["max_size"] = prop("integer", "Its longer side at most (default 512: what clients take)");
		p["format"] = prop("string", "jpg (default, small) or png");
		p["quality"] = prop("number", "jpg: 0.1..1 (default 0.8)");
		t.push_back(tool("screenshot", "A picture of a 3D view of the editor.", schema_obj(p)));
	}
	{
		t.push_back(tool("undo", "Undo the scene's last step.", schema_obj(none)));
		t.push_back(tool("redo", "Redo the scene's step undone last.", schema_obj(none)));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "Save as this res:// file (default: its own)");
		t.push_back(tool("save_scene", "Save the open scene.", schema_obj(p)));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The res:// scene file");
		t.push_back(tool("open_scene", "Open a scene in the editor.", schema_obj(p, { "path" })));
	}
	{
		Dictionary p;
		p["code"] = prop("string", "The body of a function (GDScript, tabs for its blocks); `scene` is the scene's root; its return value comes back");
		t.append_array(tool_list_more());
		t.push_back(tool("run_script", "Run GDScript in the editor (only when Editor Settings > Hewn > MCP > Allow Scripts is on; no undo).", schema_obj(p, { "code" })));
	}
	return t;
}

Dictionary HewnMCPServer::_tool(const String &p_name, const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	Node *root = scene_root();
	EditorUndoRedoManager *ur = plugin ? plugin->get_undo_redo() : nullptr;
	if (p_name == "editor_state") {
		Dictionary d;
		d["scene"] = root ? Variant(root->get_scene_file_path()) : Variant();
		d["scene_root"] = root ? Variant(String(root->get_name())) : Variant();
		d["open_scenes"] = to_json(ei->get_open_scenes());
		Array sel;
		for (const Variant &v : ei->get_selection()->get_selected_nodes()) {
			sel.push_back(path_of(Object::cast_to<Node>(v)));
		}
		d["selected"] = sel;
		if (plugin) {
			d["mode"] = MODE_NAMES[CLAMP((int)plugin->mode, 0, 3)];
			HewnMesh3D *m = plugin->get_edited();
			d["edited_mesh"] = m ? Variant(path_of(m)) : Variant();
			d["cursor"] = to_json(plugin->cursor_pos);
			d["tool_running"] = plugin->is_modal();
		}
		return text_result(d);
	}
	if (!root && p_name != "open_scene") {
		return fail("No scene is open in the editor");
	}
	if (p_name == "scene_tree") {
		Node *start = find_node(p_args.get("path", ""));
		if (!start) {
			return fail("No such node: " + String(p_args.get("path", "")));
		}
		const int depth = CLAMP((int)p_args.get("depth", 4), 0, 64);
		int count = 0;
		std::function<Dictionary(Node *, int)> walk = [&](Node *n, int d) {
			Dictionary o;
			o["path"] = path_of(n);
			o["class"] = n->get_class();
			if (Node3D *n3 = Object::cast_to<Node3D>(n)) {
				o["position"] = to_json(n3->get_position());
				if (!n3->is_visible()) {
					o["visible"] = false;
				}
			}
			count++;
			const int kids = n->get_child_count();
			if (kids > 0) {
				if (d >= depth || count > 2000) {
					o["children_count"] = kids;
				} else {
					Array ch;
					for (int i = 0; i < kids; i++) {
						Node *c = n->get_child(i);
						if (c->get_owner() != root && c != root) {
							continue; // (the plugin's own and instanced scenes' insides)
						}
						ch.push_back(walk(c, d + 1));
					}
					if (!ch.is_empty()) {
						o["children"] = ch;
					}
				}
			}
			return o;
		};
		return text_result(walk(start, 0));
	}
	if (p_name == "node_info") {
		Node *n = find_node(p_args.get("path", ""));
		if (!n) {
			return fail("No such node: " + String(p_args.get("path", "")));
		}
		const Array only = p_args.get("properties", Array());
		Dictionary props;
		const TypedArray<Dictionary> list = n->get_property_list();
		for (int64_t i = 0; i < list.size(); i++) {
			const Dictionary d = list[i];
			const String name = d["name"];
			const int usage = d["usage"];
			if (!only.is_empty() ? !only.has(name) : !(usage & PROPERTY_USAGE_EDITOR) || (usage & (PROPERTY_USAGE_CATEGORY | PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP))) {
				continue;
			}
			props[name] = to_json(n->get(name));
		}
		Dictionary o;
		o["path"] = path_of(n);
		o["class"] = n->get_class();
		o["properties"] = props;
		if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(n)) {
			Dictionary h;
			h["vertices"] = m->get_vertex_count();
			h["faces"] = m->get_face_count();
			h["aabb"] = to_json(m->get_aabb());
			const int st = m->get_shape();
			h["shape"] = st;
			h["shape_name"] = hewn::shapes::type_name((hewn::shapes::Type)st);
			Dictionary sp;
			if (st > 0 && st < hewn::shapes::TYPE_MAX) {
				for (const hewn::shapes::Param &pp : hewn::shapes::params((hewn::shapes::Type)st)) {
					sp[pp.name] = num(m->get_shape_param(pp.name));
				}
			}
			h["shape_params"] = sp;
			h["selection"] = to_json(m->get_selection_state());
			o["hewn"] = h;
		}
		return text_result(o);
	}
	if (p_name == "set_properties") {
		Node *n = find_node(p_args.get("path", ""));
		if (!n) {
			return fail("No such node: " + String(p_args.get("path", "")));
		}
		const Dictionary vals = p_args.get("values", Dictionary());
		const Array keys = vals.keys();
		ur->create_action("MCP: Set Properties", UndoRedo::MERGE_DISABLE, n);
		Array done;
		for (int64_t i = 0; i < keys.size(); i++) {
			const String k = keys[i];
			bool found = false;
			const Variant::Type t = prop_type(n, k, &found);
			if (!found) {
				return fail("No property " + k + " on " + n->get_class());
			}
			const Variant v = from_json(vals[keys[i]], t);
			ur->add_do_property(n, k, v);
			ur->add_undo_property(n, k, n->get(k));
			done.push_back(k);
		}
		ur->commit_action();
		Dictionary o;
		for (const Variant &k : done) {
			o[k] = to_json(n->get(String(k)));
		}
		return text_result(o);
	}
	if (p_name == "create_node") {
		const String type = p_args.get("type", "");
		if (!ClassDB::class_exists(type) || !ClassDB::can_instantiate(type) || !ClassDB::is_parent_class(type, "Node")) {
			return fail("Not a node class that can be made: " + type);
		}
		Node *parent = find_node(p_args.get("parent", ""));
		if (!parent) {
			return fail("No such parent: " + String(p_args.get("parent", "")));
		}
		Node *n = Object::cast_to<Node>(ClassDB::instantiate(type));
		if (!n) {
			return fail("Could not make a " + type);
		}
		const String name = p_args.get("name", "");
		n->set_name(name.is_empty() ? type : name);
		if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(n)) {
			const Variant sv = p_args.get("shape", Variant());
			if (sv.get_type() != Variant::NIL) {
				int st = -1;
				if (sv.get_type() == Variant::STRING && !String(sv).is_valid_int()) {
					for (int k = 0; k < hewn::shapes::TYPE_MAX; k++) {
						if (String(hewn::shapes::type_name((hewn::shapes::Type)k)).to_lower() == String(sv).to_lower()) {
							st = k;
						}
					}
				} else {
					st = (int)f(sv);
				}
				if (st < 0 || st >= hewn::shapes::TYPE_MAX) {
					memdelete(n);
					return fail("No such shape: " + String(sv) + " (see shapes)");
				}
				m->set_shape(st);
			}
			const Dictionary sp = p_args.get("shape_params", Dictionary());
			const Array sk = sp.keys();
			for (int64_t i = 0; i < sk.size(); i++) {
				m->set_shape_param(sk[i], f(sp[sk[i]]));
			}
		}
		const Dictionary vals = p_args.get("values", Dictionary());
		const Array keys = vals.keys();
		for (int64_t i = 0; i < keys.size(); i++) {
			bool found = false;
			const Variant::Type t = prop_type(n, keys[i], &found);
			if (!found) {
				memdelete(n);
				return fail("No property " + String(keys[i]) + " on " + type);
			}
			n->set(keys[i], from_json(vals[keys[i]], t));
		}
		if (bool(p_args.get("dry_run", false))) {
			Dictionary o;
			o["class"] = n->get_class();
			if (VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(n)) {
				o["aabb_local"] = to_json(vi->get_aabb());
			}
			if (HewnMesh3D *hm = Object::cast_to<HewnMesh3D>(n)) {
				o["faces"] = hm->get_face_count();
			}
			memdelete(n);
			o["dry_run"] = true;
			return text_result(o);
		}
		ur->create_action("MCP: Add " + type, UndoRedo::MERGE_DISABLE, parent);
		ur->add_do_method(parent, "add_child", n, true);
		ur->add_do_method(n, "set_owner", root);
		ur->add_do_reference(n);
		ur->add_undo_method(parent, "remove_child", n);
		ur->commit_action();
		Dictionary o;
		o["path"] = path_of(n);
		o["class"] = n->get_class();
		return text_result(o);
	}
	if (p_name == "delete_nodes") {
		const Array paths = p_args.get("paths", Array());
		std::vector<Node *> nodes;
		for (const Variant &pv : paths) {
			Node *n = find_node(pv);
			if (!n) {
				return fail("No such node: " + String(pv));
			}
			if (n == root) {
				return fail("The scene's root can't be deleted");
			}
			nodes.push_back(n);
		}
		ur->create_action("MCP: Delete Nodes", UndoRedo::MERGE_DISABLE, root);
		for (Node *n : nodes) {
			Node *parent = n->get_parent();
			ur->add_do_method(parent, "remove_child", n);
			ur->add_undo_method(parent, "add_child", n, true);
			ur->add_undo_method(parent, "move_child", n, n->get_index());
			// (owners come back: the node and what of it the scene keeps)
			std::vector<Node *> stack = { n };
			while (!stack.empty()) {
				Node *x = stack.back();
				stack.pop_back();
				if (x == n || x->get_owner() == root) {
					ur->add_undo_method(x, "set_owner", root);
				}
				for (int i = 0; i < x->get_child_count(); i++) {
					stack.push_back(x->get_child(i));
				}
			}
			ur->add_undo_reference(n);
		}
		ur->commit_action();
		return text_result(vformat("Deleted %d nodes", (int64_t)nodes.size()));
	}
	if (p_name == "select_nodes") {
		EditorSelection *es = ei->get_selection();
		es->clear();
		Array done;
		for (const Variant &pv : Array(p_args.get("paths", Array()))) {
			Node *n = find_node(pv);
			if (!n) {
				return fail("No such node: " + String(pv));
			}
			es->add_node(n);
			done.push_back(path_of(n));
		}
		return text_result(done);
	}
	if (p_name == "list_methods" || p_name == "call_method") {
		Node *n = find_node(p_args.get("path", ""));
		if (!n) {
			return fail("No such node: " + String(p_args.get("path", "")));
		}
		const bool all = p_args.get("all", false);
		// the methods of the node's class and its parents (Hewn's only, unless all)
		std::vector<Dictionary> methods;
		StringName cls = n->get_class();
		while (cls != StringName()) {
			if (!all && !String(cls).begins_with("Hewn")) {
				break;
			}
			const TypedArray<Dictionary> list = ClassDBSingleton::get_singleton()->class_get_method_list(cls, true);
			for (int64_t i = 0; i < list.size(); i++) {
				const Dictionary d = list[i];
				if (!String(d["name"]).begins_with("_")) {
					methods.push_back(d);
				}
			}
			cls = ClassDB::get_parent_class(cls);
		}
		if (p_name == "list_methods") {
			static const char *TN[] = { "nil", "bool", "int", "float", "String", "Vector2", "Vector2i", "Rect2", "Rect2i", "Vector3", "Vector3i", "Transform2D",
				"Vector4", "Vector4i", "Plane", "Quaternion", "AABB", "Basis", "Transform3D", "Projection", "Color", "StringName", "NodePath", "RID", "Object",
				"Callable", "Signal", "Dictionary", "Array", "PackedByteArray", "PackedInt32Array", "PackedInt64Array", "PackedFloat32Array",
				"PackedFloat64Array", "PackedStringArray", "PackedVector2Array", "PackedVector3Array", "PackedColorArray", "PackedVector4Array" };
			Array out;
			for (const Dictionary &d : methods) {
				String sig = String(d["name"]) + "(";
				const Array args = d["args"];
				const Array defs = d.get("default_args", Array());
				for (int64_t i = 0; i < args.size(); i++) {
					const Dictionary a = args[i];
					const int t = a["type"];
					sig += (i ? ", " : "") + String(a["name"]) + ": " + (t >= 0 && t < (int)(sizeof(TN) / sizeof(TN[0])) ? TN[t] : "Variant");
					const int64_t di = i - (args.size() - defs.size());
					if (di >= 0) {
						sig += " = " + UtilityFunctions::var_to_str(defs[di]);
					}
				}
				const Dictionary ret = d.get("return", Dictionary());
				const int rt = ret.get("type", 0);
				sig += String(")") + (rt > 0 && rt < (int)(sizeof(TN) / sizeof(TN[0])) ? String(" -> ") + TN[rt] : String());
				out.push_back(sig);
			}
			return text_result(out);
		}
		const String mname = p_args.get("method", "");
		const Dictionary *md = nullptr;
		for (const Dictionary &d : methods) {
			if (String(d["name"]) == mname) {
				md = &d;
			}
		}
		if (!md && !n->has_method(mname)) {
			return fail("No method " + mname + " on " + n->get_class() + " (see list_methods)");
		}
		// the arguments: in order or by name, each as its type
		const Array margs = md ? Array((*md)["args"]) : Array();
		Array args;
		const Variant given = p_args.get("args", Array());
		if (given.get_type() == Variant::DICTIONARY) {
			const Dictionary g = given;
			const Array defs = md ? Array(md->get("default_args", Array())) : Array();
			for (int64_t i = 0; i < margs.size(); i++) {
				const Dictionary a = margs[i];
				if (g.has(a["name"])) {
					args.push_back(from_json(g[a["name"]], (Variant::Type)(int)a["type"]));
				} else {
					const int64_t di = i - (margs.size() - defs.size());
					if (di < 0) {
						return fail("Missing argument " + String(a["name"]) + " of " + mname);
					}
					args.push_back(defs[di]);
				}
			}
		} else {
			const Array g = given;
			for (int64_t i = 0; i < g.size(); i++) {
				const Variant::Type t = i < margs.size() ? (Variant::Type)(int)Dictionary(margs[i])["type"] : Variant::NIL;
				args.push_back(from_json(g[i], t));
			}
		}
		const Dictionary before = snapshot(n);
		const Variant ret = n->callv(mname, args);
		const Dictionary after = snapshot(n);
		if (bool(p_args.get("dry_run", false))) {
			// (done, told, taken back)
			Dictionary o;
			o["return"] = to_json(ret);
			if (HewnMesh3D *hm = Object::cast_to<HewnMesh3D>(n)) {
				o["check"] = mesh_report(plugin, hm);
			}
			const Array bk = before.keys();
			for (int64_t i = 0; i < bk.size(); i++) {
				n->set(bk[i], before[bk[i]]);
			}
			o["dry_run"] = true;
			return text_result(o);
		}
		// what changed: one undo step
		ur->create_action("MCP: " + mname, UndoRedo::MERGE_DISABLE, n);
		int changed = 0;
		const Array keys = after.keys();
		for (int64_t i = 0; i < keys.size(); i++) {
			const Variant a = after[keys[i]], b = before.get(keys[i], Variant());
			if (a.get_type() != b.get_type() || !(a == b)) {
				ur->add_do_property(n, keys[i], a);
				ur->add_undo_property(n, keys[i], b);
				changed++;
			}
		}
		ur->commit_action(false);
		Dictionary o;
		o["return"] = to_json(ret);
		o["changed_properties"] = changed;
		if (plugin) {
			plugin->update_overlays();
		}
		return text_result(o);
	}
	if (p_name == "shapes") {
		Array out;
		for (int k = 1; k < hewn::shapes::TYPE_MAX; k++) {
			Dictionary s;
			s["id"] = k;
			s["name"] = hewn::shapes::type_name((hewn::shapes::Type)k);
			Array ps;
			for (const hewn::shapes::Param &pp : hewn::shapes::params((hewn::shapes::Type)k)) {
				Dictionary d;
				d["name"] = pp.name;
				d["default"] = num(pp.def);
				d["min"] = num(pp.min);
				d["max"] = num(pp.max);
				if (pp.is_int) {
					d["int"] = true;
				}
				if (pp.options && pp.options[0]) {
					d["choices"] = String(pp.options);
				}
				ps.push_back(d);
			}
			s["params"] = ps;
			out.push_back(s);
		}
		return text_result(out);
	}
	if (!plugin) {
		return fail("The Hewn plugin is not running");
	}
	if (p_name == "edit_mode") {
		const String mname = String(p_args.get("mode", "")).to_lower();
		int m = -1;
		for (int k = 0; k < 4; k++) {
			if (mname == MODE_NAMES[k]) {
				m = k;
			}
		}
		if (m < 0) {
			return fail("mode: object, vertex, edge or face");
		}
		if (plugin->is_modal()) {
			plugin->cancel_tool();
		}
		if (m == 0) {
			plugin->set_mode(0);
			return text_result("object");
		}
		HewnMesh3D *mesh = Object::cast_to<HewnMesh3D>(find_node(p_args.get("path", "")));
		if (!mesh && !plugin->get_edited()) {
			return fail("Give the Hewn mesh to edit (path)");
		}
		if (mesh && mesh != plugin->get_edited()) {
			if (plugin->mode != HewnEditorPlugin::MODE_OBJECT) {
				plugin->set_mode(0);
			}
			EditorSelection *es = ei->get_selection();
			es->clear();
			es->add_node(mesh);
			plugin->_edit(mesh);
		}
		plugin->set_mode(m);
		return text_result(vformat("%s mode on %s", MODE_NAMES[m], path_of(plugin->get_edited())));
	}
	if (p_name == "select_elements") {
		HewnMesh3D *m = Object::cast_to<HewnMesh3D>(find_node(p_args.get("path", "")));
		if (!m) {
			return fail("Not a Hewn mesh: " + String(p_args.get("path", "")));
		}
		Dictionary st = bool(p_args.get("add", false)) ? m->get_selection_state() : Dictionary();
		auto merge = [&](const char *p_key, const char *p_arg) {
			if (!p_args.has(p_arg)) {
				return;
			}
			PackedInt32Array cur = st.get(p_key, PackedInt32Array());
			cur.append_array(from_json(p_args[p_arg], Variant::PACKED_INT32_ARRAY));
			st[p_key] = cur;
		};
		merge("sel_verts", "vertices");
		merge("sel_edges", "edges");
		merge("sel_faces", "faces");
		m->set_selection_state(st);
		plugin->update_overlays();
		return text_result(to_json(m->get_selection_state()));
	}
	if (p_name == "menu_ops") {
		String mname = String(p_args.get("mode", "")).to_lower();
		int m = (int)plugin->mode;
		for (int k = 0; k < 4; k++) {
			if (mname == MODE_NAMES[k]) {
				m = k;
			}
		}
		const std::vector<std::pair<String, String>> items = m == 0 ? plugin->object_menu_items() : hw_mesh_menu_items(m);
		Array out;
		for (const auto &it : items) {
			if (it.second.is_empty() || !menu_op_allowed(it.first, it.second)) {
				continue;
			}
			Dictionary d;
			d["op"] = it.second;
			d["label"] = it.first.split("  ")[0];
			out.push_back(d);
		}
		return text_result(out);
	}
	if (p_name == "run_menu_op") {
		const String op = p_args.get("op", "");
		if (op.is_empty() || !menu_op_allowed(String(), op)) {
			return fail("Not an operation that runs by itself here: " + op + " (see menu_ops; the ones done with the mouse: mesh_edit, add_by_points, cut_opening; files and dialogs: not over MCP)");
		}
		if (plugin->is_modal()) {
			plugin->cancel_tool();
		}
		plugin->_run_menu_op(plugin->_viewport_camera(), op);
		return text_result(plugin->is_modal() ? "Started: it is waiting for the mouse in the view (cancel with edit_mode)" : "Done: " + op);
	}
	if (p_name == "screenshot") {
		const int idx = CLAMP((int)p_args.get("viewport", 0), 0, 3);
		SubViewport *vp = ei->get_editor_viewport_3d(idx);
		Ref<Image> img = vp && vp->get_texture().is_valid() ? vp->get_texture()->get_image() : Ref<Image>();
		if (img.is_null() || img->is_empty()) {
			return fail("No picture of the view (an editor without a window?)");
		}
		const int maxs = CLAMP((int)p_args.get("max_size", 512), 64, 4096);
		const int w = img->get_width(), h = img->get_height();
		if (MAX(w, h) > maxs) {
			const float k = (float)maxs / (float)MAX(w, h);
			img->resize(MAX(1, (int)(w * k)), MAX(1, (int)(h * k)), Image::INTERPOLATE_BILINEAR);
		}
		Dictionary c;
		c["type"] = "image";
		const bool png = String(p_args.get("format", "jpg")).to_lower() == "png";
		c["mimeType"] = png ? "image/png" : "image/jpeg";
		c["data"] = Marshalls::get_singleton()->raw_to_base64(png ? img->save_png_to_buffer() : img->save_jpg_to_buffer(CLAMP((float)f(p_args.get("quality", 0.8)), 0.1f, 1.0f)));
		Dictionary r;
		r["content"] = Array::make(c);
		r["isError"] = false;
		return r;
	}
	if (p_name == "undo" || p_name == "redo") {
		UndoRedo *u = ur->get_history_undo_redo(ur->get_object_history_id(root));
		if (!u) {
			return fail("No history");
		}
		const String was = p_name == "undo" ? u->get_current_action_name() : String();
		const bool ok = p_name == "undo" ? u->undo() : u->redo();
		plugin->update_overlays();
		if (!ok) {
			return fail(p_name == "undo" ? "Nothing to undo" : "Nothing to redo");
		}
		return text_result(p_name == "undo" ? "Undone: " + was : "Redone: " + u->get_current_action_name());
	}
	if (p_name == "save_scene") {
		const String path = p_args.get("path", "");
		if (path.is_empty()) {
			if (root->get_scene_file_path().is_empty()) {
				return fail("The scene has no file yet: give a path");
			}
			return ei->save_scene() == OK ? text_result("Saved " + root->get_scene_file_path()) : fail("Could not save");
		}
		if (!path.begins_with("res://")) {
			return fail("A res:// path");
		}
		ei->save_scene_as(path);
		return text_result("Saved as " + path);
	}
	if (p_name == "open_scene") {
		const String path = p_args.get("path", "");
		if (!path.begins_with("res://") || !ResourceLoader::get_singleton()->exists(path)) {
			return fail("No such scene: " + path);
		}
		ei->open_scene_from_path(path);
		return text_result("Opened " + path);
	}
	if (p_name == "run_script") {
		if (!allow_scripts) {
			return fail("Scripts are off: Editor Settings > Hewn > MCP > Allow Scripts");
		}
		const String code = p_args.get("code", "");
		String src = "@tool\nextends RefCounted\n\nfunc run(scene: Node) -> Variant:\n";
		for (const String &l : code.split("\n")) {
			src += "\t" + l + "\n";
		}
		src += "\treturn null\n";
		Ref<Script> sc = ClassDB::instantiate("GDScript");
		if (sc.is_null()) {
			return fail("No GDScript");
		}
		sc->set_source_code(src);
		if (sc->reload() != OK) {
			return fail("The script does not compile (see the editor's Output)");
		}
		Ref<RefCounted> o;
		o.instantiate();
		o->set_script(sc);
		if (!o->has_method("run")) {
			return fail("The script does not compile (see the editor's Output)");
		}
		const Variant ret = o->call("run", root);
		Dictionary d;
		d["return"] = to_json(ret);
		return text_result(d);
	}
	bool handled = false;
	const Dictionary more = _tool_more(p_name, p_args, handled);
	if (handled) {
		return more;
	}
	return fail("No such tool: " + p_name);
}

// ------------------------------------------------------------------ more tools: space, checks, by points, openings,
// textures, mesh operations by name

namespace {

// A change of a node as one undo step: its storage properties before and after what p_do does.
int undoable(EditorUndoRedoManager *ur, Node *p_node, const String &p_name, const std::function<void()> &p_do) {
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

std::vector<Tris> scene_tris(Node *p_root) {
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

// Where a ray meets a triangle (its distance; < 0: not).
float ray_tri(const Vector3 &o, const Vector3 &d, const Vector3 &a, const Vector3 &b, const Vector3 &c) {
	const Vector3 e1 = b - a, e2 = c - a, pv = d.cross(e2);
	const float det = e1.dot(pv);
	if (std::fabs(det) < 1e-12f) {
		return -1.0f;
	}
	const float inv = 1.0f / det;
	const Vector3 tv = o - a;
	const float u = tv.dot(pv) * inv;
	if (u < 0.0f || u > 1.0f) {
		return -1.0f;
	}
	const Vector3 qv = tv.cross(e1);
	const float v = d.dot(qv) * inv;
	if (v < 0.0f || u + v > 1.0f) {
		return -1.0f;
	}
	const float t = e2.dot(qv) * inv;
	return t > 1e-6f ? t : -1.0f;
}

Vector3 closest_on_tri(const Vector3 &p, const Vector3 &a, const Vector3 &b, const Vector3 &c) {
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

AABB world_box(Node *p_node) {
	VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(p_node);
	if (vi) {
		return vi->get_global_transform().xform(vi->get_aabb());
	}
	Node3D *n3 = Object::cast_to<Node3D>(p_node);
	return n3 ? AABB(n3->get_global_position(), Vector3()) : AABB();
}

} // namespace

// A mesh's problems (as Check Scene finds them) and whether it is closed.
Dictionary HewnMCPServer::mesh_report(HewnEditorPlugin *p_plugin, HewnMesh3D *m) {
	Dictionary o;
	const hewn::Mesh &d = m->get_data();
	o["vertices"] = (int64_t)d.pos.size();
	o["faces"] = (int64_t)d.faces.size();
	o["aabb"] = to_json(m->get_aabb());
	hewn::Topology t;
	t.build(d);
	int open = 0;
	for (size_t e = 0; e < t.edges.size(); e++) {
		open += t.edge_faces[e].size() == 1 ? 1 : 0;
	}
	o["open_edges"] = open;
	o["closed"] = open == 0 && !d.faces.empty();
	// (its volume, when closed: positive turned outward)
	float vol = 0.0f;
	for (const hewn::Face &F : d.faces) {
		for (int i = 1; i + 1 < F.size(); i++) {
			vol += d.pos[F.v[0]].dot(d.pos[F.v[i]].cross(d.pos[F.v[i + 1]]));
		}
	}
	o["volume"] = num(vol / 6.0f);
	Array problems;
	if (p_plugin) {
		p_plugin->_issues_scan();
		for (const HewnEditorPlugin::Issue &is : p_plugin->issues) {
			if (is.node != ObjectID(m->get_instance_id())) {
				continue;
			}
			Dictionary pr;
			pr["problem"] = is.text;
			if (!is.faces.empty()) {
				Array fs;
				for (int f : is.faces) {
					fs.push_back(f);
				}
				pr["faces"] = fs;
			}
			if (!is.verts.empty()) {
				Array vs;
				for (int v : is.verts) {
					vs.push_back(v);
				}
				pr["vertices"] = vs;
			}
			problems.push_back(pr);
		}
		p_plugin->issues.clear();
	}
	o["problems"] = problems;
	return o;
}

namespace {

// Shapes drawn through points: their numbers.
int points_shape(const String &p_name) {
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

} // namespace

Array HewnMCPServer::tool_list_more() {
	Array t;
	{
		Dictionary p;
		p["from"] = prop("array", "The ray's start [x, y, z] (world)");
		p["direction"] = prop("array", "Its way [x, y, z] (default straight down)");
		p["to"] = prop("array", "Or its end point instead of a way");
		p["max_distance"] = prop("number", "How far (default 1000)");
		t.push_back(tool("raycast", "The first surface a ray meets in the scene: the node, the point, the normal, the distance, a Hewn mesh's face. Straight down from a point: the floor under it.", schema_obj(p, { "from" })));
	}
	{
		Dictionary p;
		p["point"] = prop("array", "[x, y, z] (world)");
		t.push_back(tool("point_query", "What is at a point: the closed meshes it is inside of, the nearest surface (node, point, distance).", schema_obj(p, { "point" })));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The node");
		t.push_back(tool("overlaps", "The nodes whose bounds overlap a node's bounds (boxes, not the exact shapes) with the overlap's size.", schema_obj(p, { "path" })));
	}
	{
		Dictionary p;
		p["a"] = prop("", "A node's path or a point [x, y, z]");
		p["b"] = prop("", "A node's path or a point [x, y, z]");
		t.push_back(tool("measure", "The distance between two nodes or points: between their centers, and the gap between their bounds on each axis.", schema_obj(p, { "a", "b" })));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The Hewn mesh");
		t.push_back(tool("mesh_check", "A Hewn mesh's state: closed or not (open edges), volume, and its problems as Check Scene finds them (faces without area, vertices in one place, faces turned the other way, not flat...).", schema_obj(p, { "path" })));
	}
	{
		Dictionary p;
		p["shape"] = prop("string", "wall, poly_shape (an outline raised by its height), path_stairs or sweep");
		p["points"] = prop("array", "The points [[x, y, z], ...] in the parent's space (the node stands at the first)");
		p["params"] = prop("object", "The shape's parameters by name (see shapes: height, thickness, ...)");
		p["parent"] = prop("string", "The parent's path (default: the scene's root)");
		p["name"] = prop("string", "Its name");
		p["dry_run"] = prop("boolean", "Only tell its size and faces, add nothing");
		t.push_back(tool("add_by_points", "Add a shape drawn through points (as the Wall / Poly Shape / Path Stairs tools draw it with clicks; one undo step).", schema_obj(p, { "shape", "points" })));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The Hewn mesh to cut (a wall...)");
		p["center"] = prop("array", "The opening's center [x, y, z] (world)");
		p["size"] = prop("array", "Its size [x, y, z] along the mesh's own axes (through the wall: more than its thickness)");
		p["shape"] = prop("string", "box (default) or cylinder (its axis along the mesh's Z, size [diameter, diameter, length])");
		p["dry_run"] = prop("boolean", "Only tell what it would do");
		t.push_back(tool("cut_opening", "Cut an opening (a door, a window, a hole) through a Hewn mesh, as the Box / Cylinder Cut tools do (one undo step).", schema_obj(p, { "path", "center", "size" })));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The Hewn mesh");
		p["texture"] = prop("string", "An image res://... (a material made for it, as the Textures dock does) or a material res://...tres");
		p["faces"] = prop("array", "Face indices (default: the selected faces, else all)");
		t.push_back(tool("apply_texture", "Put a texture or a material on a Hewn mesh's faces (one undo step).", schema_obj(p, { "path", "texture" })));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The Hewn mesh");
		p["op"] = prop("string", "extrude, inset, bevel, delete, subdivide, flip, triangulate, merge, detach, loop_cut, smooth, flat");
		p["faces"] = prop("array", "Face indices (default: the selected faces)");
		p["edges"] = prop("array", "bevel: edges as [a, b] vertex pairs (default: the selected edges); loop_cut: one edge [a, b]");
		p["amount"] = prop("number", "extrude: the distance; inset / bevel: the width");
		p["segments"] = prop("integer", "bevel: segments; loop_cut: cuts");
		p["individual"] = prop("boolean", "extrude / inset: each face on its own");
		p["dry_run"] = prop("boolean", "Do it, tell the result, take it back (nothing changes)");
		t.push_back(tool("mesh_edit", "A Hewn mesh operation by name on faces / edges (the same as call_method on its methods, simpler; one undo step).", schema_obj(p, { "path", "op" })));
	}
	return t;
}

Dictionary HewnMCPServer::_tool_more(const String &p_name, const Dictionary &p_args, bool &r_handled) {
	r_handled = true;
	Node *root = scene_root();
	EditorUndoRedoManager *ur = plugin ? plugin->get_undo_redo() : nullptr;
	if (!root) {
		return fail("No scene is open in the editor");
	}
	if (p_name == "raycast") {
		const Vector3 from = v3(p_args.get("from", Array()));
		Vector3 dir = p_args.has("to") ? v3(p_args["to"]) - from : p_args.has("direction") ? v3(p_args["direction"]) : Vector3(0, -1, 0);
		float maxd = p_args.has("to") ? dir.length() : (float)f(p_args.get("max_distance", 1000.0));
		if (dir.length() < 1e-9f) {
			return fail("No way to cast along");
		}
		dir.normalize();
		float best = maxd;
		Tris *hit = nullptr;
		int hit_tri = -1;
		std::vector<Tris> all = scene_tris(root);
		for (Tris &t : all) {
			if (!t.box.grow(1e-3f).intersects_segment(from, from + dir * maxd)) {
				continue;
			}
			for (size_t k = 0; k * 3 + 2 < t.p.size(); k++) {
				const float d = ray_tri(from, dir, t.p[k * 3], t.p[k * 3 + 1], t.p[k * 3 + 2]);
				if (d >= 0.0f && d < best) {
					best = d;
					hit = &t;
					hit_tri = (int)k;
				}
			}
		}
		if (!hit) {
			Dictionary o;
			o["hit"] = false;
			return text_result(o);
		}
		const Vector3 a = hit->p[hit_tri * 3], b = hit->p[hit_tri * 3 + 1], c = hit->p[hit_tri * 3 + 2];
		Vector3 nrm = (b - a).cross(c - a).normalized();
		if (nrm.dot(dir) > 0.0f) {
			nrm = -nrm; // (toward where it came from)
		}
		Dictionary o;
		o["hit"] = true;
		o["node"] = path_of(hit->node);
		o["point"] = to_json(from + dir * best);
		o["normal"] = to_json(nrm);
		o["distance"] = num(best);
		if (hit->face[hit_tri] >= 0) {
			o["face"] = hit->face[hit_tri];
		}
		return text_result(o);
	}
	if (p_name == "point_query") {
		const Vector3 p = v3(p_args.get("point", Array()));
		std::vector<Tris> all = scene_tris(root);
		Array inside;
		float best = 1e30f;
		Vector3 best_at;
		Node *best_node = nullptr;
		for (Tris &t : all) {
			// inside: an odd count of crossings along a slightly tilted ray (closed meshes only make sense)
			if (t.box.has_point(p)) {
				const Vector3 dir = Vector3(1.0f, 0.0123f, 0.0071f).normalized();
				int cross = 0;
				for (size_t k = 0; k * 3 + 2 < t.p.size(); k++) {
					cross += ray_tri(p, dir, t.p[k * 3], t.p[k * 3 + 1], t.p[k * 3 + 2]) >= 0.0f ? 1 : 0;
				}
				if (cross % 2 == 1) {
					inside.push_back(path_of(t.node));
				}
			}
			for (size_t k = 0; k * 3 + 2 < t.p.size(); k++) {
				const Vector3 q = closest_on_tri(p, t.p[k * 3], t.p[k * 3 + 1], t.p[k * 3 + 2]);
				const float d = q.distance_to(p);
				if (d < best) {
					best = d;
					best_at = q;
					best_node = t.node;
				}
			}
		}
		Dictionary o;
		o["inside"] = inside;
		if (best_node) {
			Dictionary s;
			s["node"] = path_of(best_node);
			s["point"] = to_json(best_at);
			s["distance"] = num(best);
			o["nearest_surface"] = s;
		}
		return text_result(o);
	}
	if (p_name == "overlaps") {
		Node *n = find_node(p_args.get("path", ""));
		if (!n) {
			return fail("No such node: " + String(p_args.get("path", "")));
		}
		const AABB box = world_box(n);
		Array out;
		std::vector<Node *> stack = { root };
		while (!stack.empty()) {
			Node *x = stack.back();
			stack.pop_back();
			for (int i = 0; i < x->get_child_count(); i++) {
				stack.push_back(x->get_child(i));
			}
			if (x == n || !Object::cast_to<VisualInstance3D>(x) || (x->get_owner() != root) || n->is_ancestor_of(x) || x->is_ancestor_of(n)) {
				continue;
			}
			const AABB b = world_box(x);
			if (box.intersects(b)) {
				Dictionary d;
				d["node"] = path_of(x);
				d["overlap_size"] = to_json(box.intersection(b).size);
				out.push_back(d);
			}
		}
		return text_result(out);
	}
	if (p_name == "measure") {
		auto box_of = [&](const Variant &v, bool &ok) {
			ok = true;
			if (v.get_type() == Variant::ARRAY) {
				return AABB(v3(v), Vector3());
			}
			Node *x = find_node(v);
			ok = x != nullptr;
			return x ? world_box(x) : AABB();
		};
		bool oka = false, okb = false;
		const AABB a = box_of(p_args.get("a", Variant()), oka), b = box_of(p_args.get("b", Variant()), okb);
		if (!oka || !okb) {
			return fail("a and b: node paths or [x, y, z] points");
		}
		Vector3 gap;
		for (int ax = 0; ax < 3; ax++) {
			gap[ax] = MAX(0.0f, MAX(a.position[ax] - b.get_end()[ax], b.position[ax] - a.get_end()[ax]));
		}
		Dictionary o;
		o["center_distance"] = num(a.get_center().distance_to(b.get_center()));
		o["center_offset"] = to_json(b.get_center() - a.get_center());
		o["gap"] = to_json(gap); // (0 on an axis: they overlap along it)
		o["gap_distance"] = num(gap.length());
		return text_result(o);
	}
	if (p_name == "mesh_check") {
		HewnMesh3D *m = Object::cast_to<HewnMesh3D>(find_node(p_args.get("path", "")));
		if (!m) {
			return fail("Not a Hewn mesh: " + String(p_args.get("path", "")));
		}
		return text_result(mesh_report(plugin, m));
	}
	if (!ur) {
		return fail("The Hewn plugin is not running");
	}
	if (p_name == "add_by_points") {
		const int st = points_shape(p_args.get("shape", ""));
		if (st < 0) {
			return fail("shape: wall, poly_shape, path_stairs or sweep");
		}
		const Array pts = p_args.get("points", Array());
		if (pts.size() < (st == hewn::shapes::POLY_SHAPE ? 3 : 2)) {
			return fail(st == hewn::shapes::POLY_SHAPE ? "At least 3 points" : "At least 2 points");
		}
		Node *parent = find_node(p_args.get("parent", ""));
		if (!parent) {
			return fail("No such parent: " + String(p_args.get("parent", "")));
		}
		HewnMesh3D *m = memnew(HewnMesh3D);
		const String name = p_args.get("name", "");
		m->set_name(name.is_empty() ? String(hewn::shapes::type_name((hewn::shapes::Type)st)).replace(" ", "") : name);
		const Vector3 at = v3(pts[0]);
		PackedVector3Array local;
		for (const Variant &q : pts) {
			local.push_back(v3(q) - at);
		}
		m->set_position(at);
		m->set_shape(st);
		const Dictionary sp = p_args.get("params", Dictionary());
		const Array sk = sp.keys();
		for (int64_t i = 0; i < sk.size(); i++) {
			m->set_shape_param(sk[i], f(sp[sk[i]]));
		}
		m->set_poly_points(local);
		if (bool(p_args.get("dry_run", false))) {
			Dictionary o;
			o["faces"] = m->get_face_count();
			o["aabb_local"] = to_json(m->get_aabb());
			memdelete(m);
			return text_result(o);
		}
		ur->create_action("MCP: Add " + String(hewn::shapes::type_name((hewn::shapes::Type)st)), UndoRedo::MERGE_DISABLE, parent);
		ur->add_do_method(parent, "add_child", m, true);
		ur->add_do_method(m, "set_owner", root);
		ur->add_do_reference(m);
		ur->add_undo_method(parent, "remove_child", m);
		ur->commit_action();
		Dictionary o;
		o["path"] = path_of(m);
		o["faces"] = m->get_face_count();
		o["aabb"] = to_json(world_box(m));
		return text_result(o);
	}
	if (p_name == "cut_opening") {
		HewnMesh3D *m = Object::cast_to<HewnMesh3D>(find_node(p_args.get("path", "")));
		if (!m) {
			return fail("Not a Hewn mesh: " + String(p_args.get("path", "")));
		}
		const Vector3 c = m->get_global_transform().affine_inverse().xform(v3(p_args.get("center", Array())));
		const Vector3 size = v3(p_args.get("size", Array())).abs();
		if (size.x <= 0 || size.y <= 0 || size.z <= 0) {
			return fail("size: [x, y, z], all more than 0");
		}
		const bool cyl = String(p_args.get("shape", "box")).to_lower() == "cylinder";
		const bool dry = p_args.get("dry_run", false);
		const int f0 = m->get_face_count();
		Dictionary before;
		if (dry) {
			before = snapshot(m);
		}
		bool ok = true;
		auto cut = [&]() {
			if (!cyl) {
				m->cut_box(AABB(c - size * 0.5f, size), PackedInt32Array());
				return;
			}
			// (a cylinder subtracted: a mesh of it next to this one, for a moment)
			HewnMesh3D *tool = memnew(HewnMesh3D);
			tool->set_shape(hewn::shapes::CYLINDER);
			tool->set_shape_param("radius", size.x * 0.5f);
			tool->set_shape_param("height", size.z);
			tool->set_shape_param("sides", 24);
			m->add_child(tool);
			// (a cylinder stands along Y: turned to lie along Z, at the center)
			tool->set_transform(Transform3D(Basis(Vector3(1, 0, 0), (float)(Math::PI * 0.5)).scaled(Vector3(1, 1, 1)), c));
			ok = m->boolean_with(tool, 1);
			m->remove_child(tool);
			memdelete(tool);
		};
		int changed = 0;
		if (dry) {
			cut();
		} else {
			changed = undoable(ur, m, cyl ? "MCP: Cylinder Cut" : "MCP: Box Cut", cut);
		}
		if (dry) {
			Dictionary o;
			o["faces_before"] = f0;
			o["faces_after"] = m->get_face_count();
			o["ok"] = ok && m->get_face_count() != f0;
			const Array keys = before.keys();
			for (int64_t i = 0; i < keys.size(); i++) {
				m->set(keys[i], before[keys[i]]); // (back as it was)
			}
			return text_result(o);
		}
		if (!ok || changed == 0) {
			return fail("Nothing was cut: the opening does not reach the mesh (its center and size are in the world / along the mesh's axes)");
		}
		Dictionary o;
		o["faces_before"] = f0;
		o["faces_after"] = m->get_face_count();
		return text_result(o);
	}
	if (p_name == "apply_texture") {
		HewnMesh3D *m = Object::cast_to<HewnMesh3D>(find_node(p_args.get("path", "")));
		if (!m) {
			return fail("Not a Hewn mesh: " + String(p_args.get("path", "")));
		}
		const String file = p_args.get("texture", "");
		if (!file.begins_with("res://") || !ResourceLoader::get_singleton()->exists(file)) {
			return fail("No such file: " + file);
		}
		const String e = file.get_extension().to_lower();
		Ref<Material> mat;
		if (e == "tres" || e == "res" || e == "material") {
			mat = ResourceLoader::get_singleton()->load(file);
		} else if (plugin && plugin->texture_dock) {
			mat = plugin->texture_dock->material_for_image(file); // (the one the Textures dock makes: made once, then taken again)
		}
		if (mat.is_null()) {
			return fail("Could not make a material of " + file + " (an image not imported yet, or no materials folder set in the Textures dock)");
		}
		PackedInt32Array faces = p_args.has("faces") ? PackedInt32Array(from_json(p_args["faces"], Variant::PACKED_INT32_ARRAY))
													 : PackedInt32Array(m->get_selection_state().get("sel_faces", PackedInt32Array()));
		undoable(ur, m, "MCP: Apply Texture", [&]() { m->apply_material(mat, faces); });
		Dictionary o;
		o["material"] = mat->get_path().is_empty() ? Variant(mat->get_class()) : Variant(mat->get_path());
		o["faces"] = faces.is_empty() ? Variant(String("all")) : Variant(faces.size());
		return text_result(o);
	}
	if (p_name == "mesh_edit") {
		HewnMesh3D *m = Object::cast_to<HewnMesh3D>(find_node(p_args.get("path", "")));
		if (!m) {
			return fail("Not a Hewn mesh: " + String(p_args.get("path", "")));
		}
		const String op = String(p_args.get("op", "")).to_lower();
		const Dictionary sel = m->get_selection_state();
		const PackedInt32Array faces = p_args.has("faces") ? PackedInt32Array(from_json(p_args["faces"], Variant::PACKED_INT32_ARRAY)) : PackedInt32Array(sel.get("sel_faces", PackedInt32Array()));
		const PackedInt32Array edges = p_args.has("edges") ? PackedInt32Array(from_json(p_args["edges"], Variant::PACKED_INT32_ARRAY)) : PackedInt32Array(sel.get("sel_edges", PackedInt32Array()));
		const float amount = f(p_args.get("amount", 0.25));
		const int segs = MAX(1, (int)f(p_args.get("segments", 1)));
		const bool individual = p_args.get("individual", false);
		static const char *face_ops[] = { "extrude", "inset", "delete", "subdivide", "flip", "triangulate", "merge", "detach", "smooth", "flat" };
		bool needs_faces = false;
		for (const char *fo : face_ops) {
			needs_faces = needs_faces || op == fo;
		}
		if (needs_faces && faces.is_empty()) {
			return fail("No faces: give faces, or select some (select_elements)");
		}
		if ((op == "bevel" || op == "loop_cut") && edges.size() < 2) {
			return fail("No edges: give edges as [a, b] vertex pairs, or select some");
		}
		Variant ret;
		bool known = true;
		auto run = [&]() {
			if (op == "extrude") {
				ret = m->extrude_faces(faces, amount, individual, false);
			} else if (op == "inset") {
				ret = m->inset_faces(faces, amount, individual);
			} else if (op == "bevel") {
				ret = m->bevel_edges(edges, amount, segs, PackedVector2Array());
			} else if (op == "delete") {
				m->delete_faces(faces);
			} else if (op == "subdivide") {
				m->subdivide_faces(faces);
			} else if (op == "flip") {
				m->flip_faces(faces);
			} else if (op == "triangulate") {
				m->triangulate_faces(faces);
			} else if (op == "merge") {
				m->merge_faces(faces);
			} else if (op == "detach") {
				m->detach_faces(faces);
			} else if (op == "smooth" || op == "flat") {
				for (int fi : faces) {
					m->set_face_smoothing(fi, op == "smooth" ? 1 : 0);
				}
			} else if (op == "loop_cut") {
				ret = m->loop_cut(edges[0], edges[1], segs, 0.5f);
			} else {
				known = false;
			}
		};
		const int f0 = m->get_face_count(), v0 = m->get_vertex_count();
		if (bool(p_args.get("dry_run", false))) {
			const Dictionary before = snapshot(m);
			run();
			Dictionary o;
			o["faces_before"] = f0;
			o["faces_after"] = m->get_face_count();
			o["vertices_after"] = m->get_vertex_count();
			o["check"] = mesh_report(plugin, m);
			const Array keys = before.keys();
			for (int64_t i = 0; i < keys.size(); i++) {
				m->set(keys[i], before[keys[i]]);
			}
			o["dry_run"] = true;
			return known ? text_result(o) : fail("op: extrude, inset, bevel, delete, subdivide, flip, triangulate, merge, detach, loop_cut, smooth, flat");
		}
		undoable(ur, m, "MCP: " + op, run);
		if (!known) {
			return fail("op: extrude, inset, bevel, delete, subdivide, flip, triangulate, merge, detach, loop_cut, smooth, flat");
		}
		if (plugin) {
			plugin->update_overlays();
		}
		Dictionary o;
		o["faces"] = vformat("%d -> %d", f0, m->get_face_count());
		o["vertices"] = vformat("%d -> %d", v0, m->get_vertex_count());
		o["return"] = to_json(ret);
		return text_result(o);
	}
	r_handled = false;
	return Dictionary();
}

