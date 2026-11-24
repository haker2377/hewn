// Hewn editor - the MCP server (hw_mcp.h): HTTP on 127.0.0.1, JSON-RPC, the layer over the tools (merged ones, short answers, batches).

#include "hw_mcp_impl.h"


void HewnMCPServer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("handle_request", "request"), &HewnMCPServer::handle_request);
	ClassDB::bind_method(D_METHOD("get_port"), &HewnMCPServer::get_port);
	ClassDB::bind_method(D_METHOD("is_listening"), &HewnMCPServer::is_listening);
}

void HewnMCPLogger::add(const String &p_kind, const String &p_text, const String &p_where) {
	std::lock_guard<std::mutex> g(lock); // (logs come from any thread)
	Entry e;
	e.id = next_id++;
	e.kind = p_kind;
	e.text = p_text.strip_edges();
	e.where = p_where;
	entries.push_back(e);
	while (entries.size() > 1000) {
		entries.pop_front();
	}
}

void HewnMCPLogger::_log_error(const String &p_function, const String &p_file, int32_t p_line, const String &p_code, const String &p_rationale, bool p_editor_notify,
		int32_t p_error_type, const TypedArray<Ref<ScriptBacktrace>> &p_script_backtraces) {
	static const char *kinds[] = { "error", "warning", "script_error", "shader_error" };
	add(kinds[CLAMP(p_error_type, 0, 3)], p_rationale.is_empty() ? p_code : p_rationale + (p_code.is_empty() ? String() : " (" + p_code + ")"),
			vformat("%s:%d %s", p_file, p_line, p_function));
}

void HewnMCPLogger::_log_message(const String &p_message, bool p_error) {
	add(p_error ? "printerr" : "message", p_message, String());
}

Error HewnMCPServer::start(HewnEditorPlugin *p_plugin, int p_port, const String &p_token, bool p_scripts) {
	stop();
	if (logger.is_null()) {
		logger.instantiate();
		OS::get_singleton()->add_logger(logger);
	}
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
	if (logger.is_valid()) {
		OS::get_singleton()->remove_logger(logger);
		logger.unref();
	}
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
		res["instructions"] = "Changes answer in a line (ok ..., verbose: true for all); batch runs several calls at once. The Godot editor with Hewn, the scene open in it. Paths of nodes are from the scene's root. Start with editor_state and scene_tree; "
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


// ------------------------------------------------------------------ the layer over the tools: merged tools (fewer to
// list), short answers to changes, batches, the scene's changes since the last look

namespace {


// Tools that change the scene: a short answer unless verbose.
bool is_edit_tool(const String &p_name) {
	static const char *edits[] = { "set_properties", "create_node", "delete_nodes", "duplicate_nodes", "select_nodes", "call_method", "edit_mode", "select_elements",
		"run_menu_op", "add_by_points", "cut_opening", "apply_texture", "mesh_edit", "uv_edit", "hotspots", "undo", "redo", "save_scene", "open_scene", "new_scene",
		"play", "stop", "hotspot_atlas_create" };
	for (const char *e : edits) {
		if (p_name == e) {
			return true;
		}
	}
	return false;
}

String num_text(const Variant &p_v) {
	if (p_v.get_type() == Variant::FLOAT && (double)p_v == std::floor((double)p_v) && std::fabs((double)p_v) < 1e15) {
		return itos((int64_t)(double)p_v);
	}
	return String(p_v);
}

// A change's answer in a line: what tells whether it went right, no echo of what was sent.
String terse(const String &p_name, const Variant &p_v) {
	if (p_v.get_type() == Variant::STRING) {
		return "ok: " + String(p_v);
	}
	if (p_v.get_type() == Variant::ARRAY) {
		const Array a = p_v;
		String s = "ok";
		for (int64_t i = 0; i < a.size() && i < 20; i++) {
			s += " " + String(a[i]);
		}
		return a.size() > 20 ? s + vformat(" ... (%d)", a.size()) : s;
	}
	if (p_v.get_type() != Variant::DICTIONARY) {
		return "ok";
	}
	const Dictionary d = p_v;
	String s = "ok";
	if (d.has("path")) {
		s += " " + String(d["path"]);
	}
	if (d.has("faces_before") && d.has("faces_after")) {
		s += vformat(" faces %s->%s", num_text(d["faces_before"]), num_text(d["faces_after"]));
	} else if (d.has("faces")) {
		s += " faces " + num_text(d["faces"]);
	}
	if (d.has("cells")) {
		s += " cells " + num_text(d["cells"]);
	}
	if (d.has("closed")) {
		s += bool(d["closed"]) ? " closed" : " OPEN";
	}
	if (d.has("return") && d["return"].get_type() != Variant::NIL && p_name == "call_method") {
		const String r = JSON::stringify(d["return"], "", false);
		s += " return " + (r.length() > 120 ? r.substr(0, 120) + "..." : r);
	}
	if (d.has("atlas")) {
		s += " " + String(d["atlas"]);
	}
	return s;
}

// Each node of the scene: its class, place, turn, scale, shown - what changes tells.
Dictionary tree_state(Node *p_root) {
	Dictionary now;
	std::vector<Node *> stack = { p_root };
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		for (int i = 0; i < x->get_child_count(); i++) {
			if (x->get_child(i)->get_owner() == p_root) {
				stack.push_back(x->get_child(i));
			}
		}
		String state = x->get_class();
		if (Node3D *n3 = Object::cast_to<Node3D>(x)) {
			const Transform3D t = n3->get_transform();
			state += " " + JSON::stringify(to_json(t.origin), "", false) + " rot" + JSON::stringify(to_json(t.basis.get_euler() * (float)(180.0 / Math::PI)), "", false) +
					" scale" + JSON::stringify(to_json(t.basis.get_scale()), "", false) + (n3->is_visible() ? "" : " hidden");
		}
		now[path_of(x)] = state;
	}
	return now;
}

} // namespace

Array HewnMCPServer::tool_list_merged() {
	Array t;
	{
		Dictionary p;
		p["mode"] = prop("string", "ray (from, direction / to: the first surface; no direction: down), point (point: inside which meshes, the nearest surface), "
								   "overlaps (path: the meshes it meets or touches), measure (a, b: nodes or points)");
		p["from"] = prop("array", "ray: start [x, y, z]");
		p["direction"] = prop("array", "ray: way (default down)");
		p["to"] = prop("array", "ray: end point");
		p["point"] = prop("array", "point: [x, y, z]");
		p["path"] = prop("string", "overlaps: the node");
		p["a"] = prop("", "measure: path or [x, y, z]");
		p["b"] = prop("", "measure: path or [x, y, z]");
		t.push_back(tool("space", "Questions about the scene's space (world coordinates).", schema_obj(p, { "mode" })));
	}
	{
		Dictionary p;
		p["action"] = prop("string", "undo or redo");
		p["steps"] = prop("integer", "How many (default 1)");
		t.push_back(tool("history", "Undo / redo the scene's steps.", schema_obj(p, { "action" })));
	}
	{
		Dictionary p;
		p["action"] = prop("string", "play or stop");
		p["scene"] = prop("string", "play: current (default) or main");
		t.push_back(tool("game", "Run or stop the game.", schema_obj(p, { "action" })));
	}
	{
		Dictionary p;
		p["op"] = prop("string", "Run this operation (else: the list)");
		p["mode"] = prop("string", "The list's mode: object, vertex, edge, face (default: the current)");
		t.push_back(tool("menu", "Hewn's right click menu operations: their list, or run one on the selection.", schema_obj(p)));
	}
	{
		Dictionary p;
		p["action"] = prop("string", "new, open or save");
		p["path"] = prop("string", "res://....tscn (save: as this file; default its own)");
		p["root_type"] = prop("string", "new: the root's class (default Node3D)");
		p["overwrite"] = prop("boolean", "new: replace a scene there");
		t.push_back(tool("scene_file", "Make a new scene, open one, or save the open one.", schema_obj(p, { "action" })));
	}
	{
		Dictionary step;
		step["type"] = "object";
		Dictionary sp;
		sp["tool"] = prop("string", "A tool's name");
		sp["args"] = prop("object", "Its arguments");
		step["properties"] = sp;
		Dictionary steps;
		steps["type"] = "array";
		steps["items"] = step;
		steps["description"] = "The calls, in order";
		Dictionary p;
		p["steps"] = steps;
		p["stop_on_error"] = prop("boolean", "Stop at the first error (default true)");
		t.push_back(tool("batch", "Several tool calls in one: a line of result each (pictures left out). Fewer round trips.", schema_obj(p, { "steps" })));
	}
	return t;
}

Dictionary HewnMCPServer::_tool(const String &p_name, const Dictionary &p_args) {
	if (p_name == "batch") {
		const Array steps = p_args.get("steps", Array());
		const bool stop = p_args.get("stop_on_error", true);
		Node *root = scene_root();
		EditorUndoRedoManager *ur = plugin ? plugin->get_undo_redo() : nullptr;
		UndoRedo *h = root && ur ? ur->get_history_undo_redo(ur->get_object_history_id(root)) : nullptr;
		const int64_t v0 = h ? (int64_t)h->get_version() : 0;
		String out;
		int errors = 0;
		for (int64_t i = 0; i < steps.size(); i++) {
			const Dictionary st = steps[i];
			const String name = st.get("tool", "");
			if (name == "batch") {
				out += vformat("%d error: no batch in a batch\n", i);
				errors++;
				break;
			}
			const Dictionary r = _tool(name, st.get("args", Dictionary()));
			const bool err = r.get("isError", false);
			String text;
			for (const Variant &c : Array(r.get("content", Array()))) {
				const Dictionary cd = c;
				text += String(cd.get("type", "")) == "text" ? String(cd.get("text", "")) : String("[picture left out]");
			}
			out += vformat("%d %s %s\n", i, err ? String("error:") : name + String(":"), text.replace("\n", " ").substr(0, 400));
			errors += err ? 1 : 0;
			if (err && stop) {
				out += vformat("stopped at %d of %d\n", i, steps.size());
				break;
			}
		}
		UndoRedo *h2 = scene_root() && ur ? ur->get_history_undo_redo(ur->get_object_history_id(scene_root())) : nullptr;
		if (h2 && h2 == h) {
			out += vformat("undo steps made: %d (history undo steps N takes them back)", (int64_t)h2->get_version() - v0);
		}
		return text_result(out, errors > 0);
	}
	// the merged tools: to their parts
	if (p_name == "space") {
		const String mode = p_args.get("mode", "");
		return _tool(mode == "ray" ? "raycast" : mode == "point" ? "point_query" : mode == "overlaps" ? "overlaps" : mode == "measure" ? "measure" : "space?", p_args);
	}
	if (p_name == "history") {
		const String action = p_args.get("action", "undo");
		const int steps = CLAMP((int)f(p_args.get("steps", 1)), 1, 1000);
		Dictionary last;
		String done;
		for (int i = 0; i < steps; i++) {
			last = _tool_inner(action == "redo" ? "redo" : "undo", Dictionary());
			if (bool(last.get("isError", false))) {
				break;
			}
			done += String(Dictionary(Array(last["content"])[0]).get("text", "")) + "\n";
		}
		return done.is_empty() ? last : text_result(done.strip_edges());
	}
	if (p_name == "game") {
		return _tool(String(p_args.get("action", "play")) == "stop" ? "stop" : "play", p_args);
	}
	if (p_name == "menu") {
		return _tool(p_args.has("op") ? "run_menu_op" : "menu_ops", p_args);
	}
	if (p_name == "scene_file") {
		const String a = p_args.get("action", "");
		return _tool(a == "new" ? "new_scene" : a == "open" ? "open_scene" : "save_scene", p_args);
	}
	if (p_name == "scene_tree" && bool(p_args.get("changes", false))) {
		// what was added, removed, moved since the last look
		Node *root = scene_root();
		if (!root) {
			return fail("No scene is open");
		}
		const Dictionary now = tree_state(root);
		String out;
		const Array keys = now.keys();
		for (int64_t i = 0; i < keys.size(); i++) {
			if (!seen_tree.has(keys[i])) {
				out += "+ " + String(keys[i]) + " " + String(now[keys[i]]) + "\n";
			} else if (String(seen_tree[keys[i]]) != String(now[keys[i]])) {
				out += "~ " + String(keys[i]) + " " + String(now[keys[i]]) + "\n";
			}
		}
		const Array old = seen_tree.keys();
		for (int64_t i = 0; i < old.size(); i++) {
			if (!now.has(old[i])) {
				out += "- " + String(old[i]) + "\n";
			}
		}
		const bool first = seen_tree.is_empty();
		seen_tree = now;
		return text_result(first ? String("(first look: all nodes are new) ") + vformat("%d nodes; changes from now on", keys.size()) : out.is_empty() ? String("no changes") : out);
	}
	Dictionary r = _tool_inner(p_name, p_args);
	if (p_name == "scene_tree" && scene_root()) {
		seen_tree = tree_state(scene_root()); // (a full look: the changes count from here)
	}
	// a change's answer short (what tells it went right), unless verbose
	if (is_edit_tool(p_name) && !bool(p_args.get("verbose", false)) && !bool(r.get("isError", false))) {
		const Array content = r.get("content", Array());
		if (content.size() == 1 && String(Dictionary(content[0]).get("type", "")) == "text") {
			const String text = Dictionary(content[0]).get("text", "");
			const Variant parsed = text.begins_with("{") || text.begins_with("[") ? JSON::parse_string(text) : Variant(text);
			return text_result(terse(p_name, parsed));
		}
	}
	return r;
}
