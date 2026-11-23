// Hewn editor — an MCP server (Model Context Protocol) in the editor: assistants on this computer read the scene and
// work in it through tools. Streamable HTTP on 127.0.0.1 (POST /mcp, JSON-RPC 2.0, a JSON answer to each request),
// polled on the editor's main thread. Off by default (Editor Settings > Hewn > MCP); every change it makes goes through
// the editor's undo.
#pragma once

#include <godot_cpp/classes/logger.hpp>
#include <godot_cpp/classes/script_backtrace.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/stream_peer_tcp.hpp>
#include <godot_cpp/classes/tcp_server.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <deque>
#include <mutex>
#include <vector>

namespace godot {

class HewnEditorPlugin;
class HewnMesh3D;

// What the editor prints and reports (the Output panel's lines), kept for the editor_log tool: the last ones.
class HewnMCPLogger : public Logger {
	GDCLASS(HewnMCPLogger, Logger)

protected:
	static void _bind_methods() {}

public:
	struct Entry {
		int64_t id = 0;
		String kind; // error, warning, script_error, shader_error, message, printerr
		String text, where;
	};
	std::mutex lock;
	std::deque<Entry> entries;
	int64_t next_id = 1;
	void add(const String &p_kind, const String &p_text, const String &p_where);
	void _log_error(const String &p_function, const String &p_file, int32_t p_line, const String &p_code, const String &p_rationale, bool p_editor_notify,
			int32_t p_error_type, const TypedArray<Ref<ScriptBacktrace>> &p_script_backtraces) override;
	void _log_message(const String &p_message, bool p_error) override;
};

class HewnMCPServer : public Node {
	GDCLASS(HewnMCPServer, Node)

	struct Conn {
		Ref<StreamPeerTCP> peer;
		PackedByteArray buf;
		uint64_t last = 0; // (ms: dropped when idle long)
	};
	Ref<TCPServer> server;
	std::vector<Conn> conns;
	HewnEditorPlugin *plugin = nullptr;
	int port = 0;
	String token; // (not empty: asked for as "Authorization: Bearer <token>")
	bool allow_scripts = false;
	String session;
	Ref<HewnMCPLogger> logger;

	// one HTTP request: false when the connection is to close
	bool _http(Conn &c, const String &p_method, const String &p_path, const Dictionary &p_headers, const PackedByteArray &p_body);
	void _send(Conn &c, int p_code, const String &p_status, const String &p_body, const Dictionary &p_headers = Dictionary());
	Variant _rpc(const Dictionary &p_req, bool &r_reply);
	Dictionary _tool(const String &p_name, const Dictionary &p_args);
	Dictionary _tool_more(const String &p_name, const Dictionary &p_args, bool &r_handled);
	Dictionary _tool_tex(const String &p_name, const Dictionary &p_args, bool &r_handled);
	static Dictionary mesh_report(HewnEditorPlugin *p_plugin, HewnMesh3D *p_mesh);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static Array tool_list(); // (tools/list: name, description, inputSchema)
	static Array tool_list_more();
	static Array tool_list_tex();
	Error start(HewnEditorPlugin *p_plugin, int p_port, const String &p_token, bool p_scripts);
	void stop();
	bool is_listening() const;
	int get_port() const { return port; }
	void set_allow_scripts(bool p_on) { allow_scripts = p_on; }
	void set_token(const String &p_token) { token = p_token; }
	// a request handled as if it came over HTTP (the self-test): its JSON-RPC answer
	Dictionary handle_request(const Dictionary &p_req);
};

} // namespace godot
