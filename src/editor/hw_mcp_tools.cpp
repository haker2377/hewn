// Hewn editor - the MCP tools: the scene, nodes, methods, Hewn shapes and modes, menus, pictures, undo, files, scripts.

#include "hw_mcp_impl.h"

Array HewnMCPServer::tool_list() {
	Array all = tool_list_all();
	Array t;
	for (const Variant &v : all) {
		const String name = Dictionary(v).get("name", "");
		bool away = false;
		for (const char *m : MERGED_AWAY) {
			away = away || name == m;
		}
		if (!away) {
			t.push_back(v);
		}
	}
	return t;
}

Array HewnMCPServer::tool_list_all() {
	Array t;
	Dictionary none;
	{
		t.push_back(tool("editor_state", "The open scene, the selected nodes, the Hewn mode (object / vertex / edge / face) and the mesh in it, the 3D cursor.", schema_obj(none)));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The node to start at (default: the scene's root)");
		p["depth"] = prop("integer", "How deep (default 4)");
		p["format"] = prop("string", "lines (default: a line a node, few tokens) or json");
		p["changes"] = prop("boolean", "Only what was added (+), removed (-), moved / changed (~) since the last look");
		t.push_back(tool("scene_tree", "The scene's nodes: path, class, name, position of 3D ones, children.", schema_obj(p)));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The node's path from the scene's root");
		p["properties"] = prop("array", "Only these properties (default: all shown in the Inspector)");
		p["expand"] = prop("boolean", "Resources in it shown with their properties (default true)");
		p["all"] = prop("boolean", "Also the properties still at their defaults (default: only the ones set)");
		p["sections"] = prop("array", "Only these parts: properties, hewn (a Hewn mesh's counts, bounds, shape, selection); default both");
		t.push_back(tool("node_info", "A node's properties (as in the Inspector); for a Hewn mesh also its counts, bounds, shape parameters and selection.", schema_obj(p, { "path" })));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The node's path");
		p["values"] = prop("object", "property: value (a 3D node also \"look_at\": [x, y, z] - turned to face that point). Vectors as [x, y, z], colors as [r, g, b, a] or \"#rrggbb\", transforms as {origin, euler_degrees, scale}, "
									 "resources as \"res://...\" or made here as {\"class\": \"Environment\", ...its properties}, or Godot text like \"Vector3(1, 2, 3)\"");
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
		p["path"] = prop("string", "The node to copy (with its children)");
		p["count"] = prop("integer", "How many copies (default 1)");
		p["offset"] = prop("array", "Each copy this much further [x, y, z] (a row)");
		p["rotation_step"] = prop("array", "Each copy turned this much more [x, y, z] degrees");
		t.push_back(tool("duplicate_nodes", "Copies of a node beside it, in a row (one undo step).", schema_obj(p, { "path" })));
	}
	{
		Dictionary p;
		p["scene"] = prop("string", "current (default) or main");
		t.push_back(tool("play", "Run the game: the open scene or the project's main scene.", schema_obj(p)));
		t.push_back(tool("stop", "Stop the running game.", schema_obj(Dictionary())));
	}
	{
		Dictionary p;
		p["since"] = prop("integer", "Only the ones after this id (next_since of the last call)");
		p["kinds"] = prop("string", "problems (default: errors and warnings), errors, all (also messages)");
		p["count"] = prop("integer", "At most this many, the newest (default 50)");
		t.push_back(tool("editor_log", "What the editor printed and reported (the Output panel): errors, warnings, messages - did the last step go well.", schema_obj(p)));
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
		p["filter"] = prop("string", "Only methods with this in their name (bevel, uv, hotspot...): a Hewn mesh has some 300");
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
		p["from"] = prop("array", "A point of view of its own [x, y, z] (the editor's camera stays where it is)");
		p["look_at"] = prop("array", "With from: the point it looks at");
		p["camera"] = prop("string", "Or a Camera3D of the scene to look through");
		p["fov"] = prop("number", "With from: the field of view in degrees (default 70)");
		p["width"] = prop("integer", "With from / camera: the picture's width (default 960)");
		p["height"] = prop("integer", "With from / camera: its height (default 540)");
		p["max_size"] = prop("integer", "Its longer side at most (default 512: what clients take)");
		p["format"] = prop("string", "jpg (default, small) or png");
		p["region"] = prop("array", "Only a part [x, y, width, height] as 0..1 of the picture (a closer look for fewer tokens)");
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
		p["path"] = prop("string", "Its file res://....tscn");
		p["root_type"] = prop("string", "The root node's class (default Node3D)");
		p["name"] = prop("string", "The root's name (default from the file's)");
		p["overwrite"] = prop("boolean", "Replace a scene already there");
		t.push_back(tool("new_scene", "Make a new scene (its root node), save it and open it in the editor.", schema_obj(p, { "path" })));
	}
	{
		Dictionary p;
		p["code"] = prop("string", "The body of a function (GDScript, tabs for its blocks); `scene` is the scene's root; its return value comes back");
		t.append_array(tool_list_more());
		t.append_array(tool_list_tex());
		t.append_array(tool_list_merged());
		t.push_back(tool("run_script", "Run GDScript in the editor (only when Editor Settings > Hewn > MCP > Allow Scripts is on; no undo).", schema_obj(p, { "code" })));
	}
	return t;
}

Dictionary HewnMCPServer::_tool_inner(const String &p_name, const Dictionary &p_args) {
	g_max_items = CLAMP((int)f(p_args.get("max_items", MAX_ITEMS)), 1, 100000);
	g_precise = p_args.get("precise", false);
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
	if (p_name == "new_scene") {
		// a new scene of a root node, saved to its file and opened
		const String path = p_args.get("path", "");
		const String type = p_args.get("root_type", "Node3D");
		if (!path.begins_with("res://") || !path.ends_with(".tscn")) {
			return fail("path: res://....tscn");
		}
		if (ResourceLoader::get_singleton()->exists(path) && !bool(p_args.get("overwrite", false))) {
			return fail("There is a scene there already: " + path + " (open_scene, or overwrite: true)");
		}
		if (!ClassDB::class_exists(type) || !ClassDB::can_instantiate(type) || !ClassDB::is_parent_class(type, "Node")) {
			return fail("Not a node class that can be made: " + type);
		}
		Node *r = Object::cast_to<Node>(ClassDB::instantiate(type));
		const String name = p_args.get("name", "");
		r->set_name(name.is_empty() ? path.get_file().get_basename().capitalize().replace(" ", "") : name);
		Ref<PackedScene> ps;
		ps.instantiate();
		ps->pack(r);
		const Error e = ResourceSaver::get_singleton()->save(ps, path);
		memdelete(r);
		if (e != OK) {
			return fail("Could not save " + path);
		}
		ei->get_resource_filesystem()->update_file(path);
		ei->open_scene_from_path(path);
		return text_result("Made and opened " + path);
	}
	// (these need no scene)
	static const char *no_scene[] = { "open_scene", "shapes", "run_script", "screenshot", "menu_ops", "editor_log", "stop", "textures", "texture_sheet", "hotspot_atlas_create" };
	bool free_of_scene = false;
	for (const char *ns : no_scene) {
		free_of_scene = free_of_scene || p_name == ns;
	}
	if (!root && !free_of_scene) {
		return fail("No scene is open in the editor (new_scene or open_scene)");
	}
	if (p_name == "scene_tree") {
		Node *start = find_node(p_args.get("path", ""));
		if (!start) {
			return fail("No such node: " + String(p_args.get("path", "")));
		}
		const int depth = CLAMP((int)p_args.get("depth", 4), 0, 64);
		int count = 0;
		if (String(p_args.get("format", "lines")) == "lines") {
			// a line a node: indented by its depth, its name, class, position (a few tokens a node)
			String out;
			std::function<void(Node *, int)> line = [&](Node *n, int d) {
				count++;
				String l = String("  ").repeat(d) + String(n->get_name()) + " " + n->get_class();
				if (Node3D *n3 = Object::cast_to<Node3D>(n)) {
					const Vector3 p = n3->get_position();
					l += vformat(" (%s, %s, %s)", String(num(p.x)), String(num(p.y)), String(num(p.z)));
					if (!n3->is_visible()) {
						l += " hidden";
					}
				}
				int kids = 0;
				for (int i = 0; i < n->get_child_count(); i++) {
					kids += n->get_child(i)->get_owner() == root ? 1 : 0;
				}
				if (kids > 0 && (d >= depth || count > 2000)) {
					l += vformat(" [%d children]", kids);
				}
				out += l + "\n";
				if (d < depth && count <= 2000) {
					for (int i = 0; i < n->get_child_count(); i++) {
						if (n->get_child(i)->get_owner() == root) {
							line(n->get_child(i), d + 1);
						}
					}
				}
			};
			line(start, 0);
			return text_result(out + "(paths: names joined with / from the root's children)");
		}
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
			if (only.is_empty() && !bool(p_args.get("all", false))) {
				// (the class's default: left out - what was set is what tells)
				const Variant def = ClassDBSingleton::get_singleton()->class_get_property_default_value(n->get_class(), name);
				const Variant cur = n->get(name);
				if (def.get_type() == cur.get_type() && def == cur) {
					continue;
				}
			}
			const Variant pv = n->get(name);
			Dictionary rj = to_json(pv).get_type() == Variant::DICTIONARY ? Dictionary(to_json(pv)) : Dictionary();
			Resource *res = pv.get_type() == Variant::OBJECT ? Object::cast_to<Resource>((Object *)pv) : nullptr;
			if (res && bool(p_args.get("expand", true))) {
				// (what is in it: an environment's sky, a material's color...)
				Dictionary inner;
				const TypedArray<Dictionary> rl = res->get_property_list();
				for (int64_t k = 0; k < rl.size(); k++) {
					const Dictionary rd = rl[k];
					const int ru = rd["usage"];
					if ((ru & PROPERTY_USAGE_EDITOR) && !(ru & (PROPERTY_USAGE_CATEGORY | PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP)) && String(rd["name"]) != "script" &&
							!String(rd["name"]).begins_with("resource_")) {
						inner[rd["name"]] = to_json(res->get(rd["name"]), 4);
					}
				}
				rj["properties"] = inner;
				props[name] = rj;
			} else {
				props[name] = to_json(pv);
			}
		}
		Dictionary o;
		o["path"] = path_of(n);
		o["class"] = n->get_class();
		const Array sections = p_args.get("sections", Array());
		auto wants = [&](const char *p_s) { return sections.is_empty() || sections.has(p_s); };
		if (wants("properties")) {
			o["properties"] = props;
		}
		if (HewnMesh3D *m = wants("hewn") ? Object::cast_to<HewnMesh3D>(n) : nullptr) {
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
		Dictionary vals = Dictionary(p_args.get("values", Dictionary())).duplicate();
		look_at_value(n, vals);
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
		Dictionary vals = Dictionary(p_args.get("values", Dictionary())).duplicate();
		look_at_value(n, vals);
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
	if (p_name == "duplicate_nodes") {
		Node *n = find_node(p_args.get("path", ""));
		if (!n || n == root) {
			return fail("No such node (not the root): " + String(p_args.get("path", "")));
		}
		const int count = CLAMP((int)f(p_args.get("count", 1)), 1, 1000);
		const Vector3 off = p_args.has("offset") ? v3(p_args["offset"]) : Vector3();
		const Vector3 rot = p_args.has("rotation_step") ? v3(p_args["rotation_step"]) : Vector3();
		Node *parent = n->get_parent();
		Array made;
		ur->create_action("MCP: Duplicate", UndoRedo::MERGE_DISABLE, root);
		for (int i = 1; i <= count; i++) {
			Node *d = n->duplicate();
			if (Node3D *d3 = Object::cast_to<Node3D>(d)) {
				Node3D *n3 = Object::cast_to<Node3D>(n);
				d3->set_position(n3->get_position() + off * (float)i);
				d3->set_rotation_degrees(n3->get_rotation_degrees() + rot * (float)i);
			}
			ur->add_do_method(parent, "add_child", d, true);
			ur->add_do_method(d, "set_owner", root);
			std::vector<Node *> stack;
			for (int c = 0; c < d->get_child_count(); c++) {
				stack.push_back(d->get_child(c));
			}
			while (!stack.empty()) {
				Node *x = stack.back();
				stack.pop_back();
				ur->add_do_method(x, "set_owner", root);
				for (int c = 0; c < x->get_child_count(); c++) {
					stack.push_back(x->get_child(c));
				}
			}
			ur->add_do_reference(d);
			ur->add_undo_method(parent, "remove_child", d);
			made.push_back(d);
		}
		ur->commit_action();
		Array out;
		for (const Variant &v : made) {
			out.push_back(path_of(Object::cast_to<Node>(v)));
		}
		return text_result(out);
	}
	if (p_name == "editor_log") {
		if (logger.is_null()) {
			return fail("No log");
		}
		const int64_t since = (int64_t)f(p_args.get("since", 0));
		const String filter = p_args.get("kinds", "problems");
		const int count = CLAMP((int)f(p_args.get("count", 50)), 1, 1000);
		Array out;
		int64_t last = 0;
		{
			std::lock_guard<std::mutex> g(logger->lock);
			for (auto it = logger->entries.rbegin(); it != logger->entries.rend() && out.size() < count; ++it) {
				if (it->id <= since) {
					break;
				}
				const bool err = it->kind != "message";
				if ((filter == "errors" && !(it->kind.contains("error") || it->kind == "printerr")) || (filter == "problems" && !err)) {
					continue;
				}
				Dictionary d;
				d["id"] = it->id;
				d["kind"] = it->kind;
				d["text"] = it->text;
				if (!it->where.is_empty()) {
					d["where"] = it->where;
				}
				out.push_front(d);
			}
			last = logger->next_id - 1;
		}
		Dictionary o;
		o["entries"] = out;
		o["next_since"] = last; // (give it back as since: only what comes after)
		return text_result(o);
	}
	if (p_name == "play") {
		const String what = p_args.get("scene", "current");
		if (what == "main") {
			ei->play_main_scene();
		} else {
			ei->play_current_scene();
		}
		return text_result("Playing (stop to end it)");
	}
	if (p_name == "stop") {
		if (!ei->is_playing_scene()) {
			return text_result("Nothing is playing");
		}
		ei->stop_playing_scene();
		return text_result("Stopped");
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
			const String flt = String(p_args.get("filter", "")).to_lower();
			for (const Dictionary &d : methods) {
				if (!flt.is_empty() && !String(d["name"]).to_lower().contains(flt)) {
					continue;
				}
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
		Dictionary found; // (one of Godot's own classes': its argument types too)
		if (!md) {
			for (StringName c = n->get_class(); c != StringName() && !md; c = ClassDB::get_parent_class(c)) {
				const TypedArray<Dictionary> list = ClassDBSingleton::get_singleton()->class_get_method_list(c, true);
				for (int64_t i = 0; i < list.size(); i++) {
					if (String(Dictionary(list[i])["name"]) == mname) {
						found = list[i];
						md = &found;
						break;
					}
				}
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
		Ref<Image> img;
		if (p_args.has("from") || p_args.has("camera")) {
			// a point of view of its own: a view of the scene's world drawn right now (the editor's camera stays)
			if (!vp) {
				return fail("No 3D view");
			}
			Transform3D xf;
			float fov = (float)f(p_args.get("fov", 70.0));
			if (p_args.has("camera")) {
				Camera3D *cam = Object::cast_to<Camera3D>(find_node(p_args["camera"]));
				if (!cam) {
					return fail("Not a Camera3D: " + String(p_args["camera"]));
				}
				xf = cam->get_global_transform();
				fov = cam->get_fov();
			} else {
				const Vector3 from = v3(p_args["from"]);
				const Vector3 to = p_args.has("look_at") ? v3(p_args["look_at"]) : from + Vector3(0, 0, -1);
				const Vector3 d = to - from;
				const Vector3 up = std::fabs(d.normalized().y) > 0.999f ? Vector3(0, 0, -1) : Vector3(0, 1, 0);
				xf = Transform3D(Basis::looking_at(d.length() > 1e-6f ? d : Vector3(0, 0, -1), up), from);
			}
			const int w = CLAMP((int)f(p_args.get("width", 960)), 64, 4096), h = CLAMP((int)f(p_args.get("height", 540)), 64, 4096);
			SubViewport *sv = memnew(SubViewport);
			sv->set_size(Vector2i(w, h));
			sv->set_world_3d(vp->get_world_3d());
			sv->set_update_mode(SubViewport::UPDATE_ALWAYS);
			Camera3D *c = memnew(Camera3D);
			c->set_fov(fov);
			c->set_transform(xf); // (before the tree: a transform set later reaches the renderer only next frame)
			c->set_current(true);
			sv->add_child(c);
			add_child(sv);
			// (the camera's place to the renderer now: a new camera's reaches it only with the next frame's
			// transform notifications, after this drawing)
			RenderingServer::get_singleton()->camera_set_transform(c->get_camera_rid(), xf);
			RenderingServer::get_singleton()->force_draw(false, 0.0);
			RenderingServer::get_singleton()->force_draw(false, 0.0); // (twice: shadows and the sky settled)
			img = sv->get_texture()->get_image();
			remove_child(sv);
			sv->queue_free();
		} else {
			img = vp && vp->get_texture().is_valid() ? vp->get_texture()->get_image() : Ref<Image>();
		}
		if (img.is_null() || img->is_empty()) {
			return fail("No picture of the view (an editor without a window?)");
		}
		if (p_args.has("region")) {
			// (a part of it: x, y, width, height as 0..1 of the picture)
			const Array rg = p_args["region"];
			if (rg.size() == 4) {
				const int iw = img->get_width(), ih = img->get_height();
				const Rect2i rr = Rect2i((int)(f(rg[0]) * iw), (int)(f(rg[1]) * ih), MAX(1, (int)(f(rg[2]) * iw)), MAX(1, (int)(f(rg[3]) * ih))).intersection(Rect2i(0, 0, iw, ih));
				if (rr.has_area()) {
					img = img->get_region(rr);
				}
			}
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
	const Dictionary tex = _tool_tex(p_name, p_args, handled);
	if (handled) {
		return tex;
	}
	return fail("No such tool: " + p_name);
}
