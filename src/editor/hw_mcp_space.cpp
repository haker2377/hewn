// Hewn editor - the MCP tools about space and meshes: rays, points, overlaps, checks, shapes through points, openings, textures on faces, mesh_edit.

#include "hw_mcp_impl.h"

// ------------------------------------------------------------------ more tools: space, checks, by points, openings,
// textures, mesh operations by name


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
		p["bounds_only"] = prop("boolean", "Also the ones that only share bounds (default: only meshes whose shapes really meet)");
		t.push_back(tool("overlaps", "The meshes a node really meets (their surfaces cross or one is inside the other: shapes_meet) or touches (one standing on the other: touching).", schema_obj(p, { "path" })));
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
		p["lining"] = prop("boolean", "The opening's sides made (through: a passage, else a niche); default true");
		t.push_back(tool("cut_opening", "Cut an opening (a door, a window, a hole) through a Hewn mesh, as the Box / Cylinder Cut tools do (one undo step).", schema_obj(p, { "path", "center", "size" })));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The Hewn mesh");
		p["texture"] = prop("string", "An image res://... (a material made for it, as the Textures dock does) or a material res://...tres");
		p["material"] = prop("", "Or a material: res://...tres, or made here: {\"class\": \"StandardMaterial3D\", \"albedo_color\": [r, g, b], \"roughness\": 0.8, ...}");
		p["faces"] = prop("array", "Face indices (default: the selected faces, else all)");
		t.push_back(tool("apply_texture", "Put a texture or a material on a Hewn mesh's faces (one undo step).", schema_obj(p, { "path" })));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The Hewn mesh");
		p["op"] = prop("string", "extrude, inset, bevel, delete, subdivide, flip, triangulate, merge, detach, loop_cut, smooth, flat, mark_seam, clear_seam");
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
			if (x == n || !Object::cast_to<GeometryInstance3D>(x) || !Object::cast_to<GeometryInstance3D>(x)->is_visible_in_tree() || (x->get_owner() != root) || n->is_ancestor_of(x) ||
					x->is_ancestor_of(n)) {
				continue;
			}
			const AABB b = world_box(x);
			if (box.grow(1e-3f).intersects(b)) { // (touching ones too: one standing on the other)
				Dictionary d;
				d["node"] = path_of(x);
				d["bounds_overlap"] = to_json(box.intersection(b).size);
				// the real shapes: do their surfaces cross, or is one inside the other (a box in a ring of walls only
				// shares bounds)
				const int real = shapes_meet(n, x);
				if (real >= 0) {
					d["shapes_meet"] = real == 1;
				}
				const Vector3 ov = box.grow(1e-3f).intersection(b).size;
				const bool touching = real == 0 && MIN(ov.x, MIN(ov.y, ov.z)) < 3e-3f; // (one standing on the other)
				if (touching) {
					d["touching"] = true;
				}
				if (real == 0 && !touching && !bool(p_args.get("bounds_only", false))) {
					continue;
				}
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
		const bool lining = p_args.get("lining", true);
		int holes = -1;
		const int f0 = m->get_face_count();
		Dictionary before;
		if (dry) {
			before = snapshot(m);
		}
		bool ok = true;
		auto cut = [&]() {
			if (!cyl) {
				// the opening's sides from the box's own sides inside the mesh (worked out before the cut): a door down to
				// a wall's bottom gets its jambs and head, nothing across its threshold
				hewn::Mesh sides;
				if (lining) {
					const AABB lb(c - size * 0.5f, size);
					const Transform3D g = m->get_global_transform();
					const Basis bt = g.affine_inverse().basis.transposed();
					std::vector<hewn::ops::CutPlane> wp;
					for (int a = 0; a < 3; a++) {
						for (int sgn = -1; sgn <= 1; sgn += 2) {
							Vector3 nl;
							nl[a] = (float)sgn;
							const Vector3 pl = sgn > 0 ? lb.get_end() : lb.position;
							const Vector3 nw = bt.xform(nl).normalized();
							wp.push_back({ nw, nw.dot(g.xform(pl)), true });
						}
					}
					int mat = 0;
					float vol = 0.0f; // (a room - turned inside out: its holes joined / a niche, the old way)
					const hewn::Mesh &dm = m->get_data();
					for (const hewn::Face &F : dm.faces) {
						for (int i = 1; i + 1 < F.size(); i++) {
							vol += dm.pos[F.v[0]].dot(dm.pos[F.v[i]].cross(dm.pos[F.v[i + 1]]));
						}
					}
					if (!dm.faces.empty()) {
						mat = dm.faces[0].material;
					}
					if (vol <= 0.0f || !HewnCutter3D::side_lining({ m }, { wp }, mat, sides)) {
						sides = hewn::Mesh();
					}
				}
				const PackedInt32Array packed = m->cut_box(AABB(c - size * 0.5f, size), PackedInt32Array());
				if (!sides.faces.empty()) {
					holes = (int)HewnMesh3D::unpack_loops(packed).size();
					HewnCutter3D::attach_sides({ m }, sides);
					return;
				}
				const std::vector<std::vector<int>> loops = HewnMesh3D::unpack_loops(packed);
				holes = (int)loops.size();
				if (lining && loops.size() == 2) {
					m->bridge_holes(to_packed_ints(loops[0]), to_packed_ints(loops[1]));
				} else if (lining && loops.size() == 1) {
					m->make_alcove(to_packed_ints(loops[0]), MIN(size.x, MIN(size.y, size.z)));
				}
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
			o["holes"] = holes;
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
		if (holes >= 0) {
			o["holes"] = holes; // (2: through, joined; 1: a niche)
		}
		const Dictionary rep = mesh_report(plugin, m);
		o["closed"] = rep["closed"];
		return text_result(o);
	}
	if (p_name == "apply_texture") {
		HewnMesh3D *m = Object::cast_to<HewnMesh3D>(find_node(p_args.get("path", "")));
		if (!m) {
			return fail("Not a Hewn mesh: " + String(p_args.get("path", "")));
		}
		const String file = p_args.get("texture", "");
		Ref<Material> mat;
		if (p_args.has("material")) {
			mat = from_json(p_args["material"], Variant::OBJECT);
			if (mat.is_null()) {
				return fail("material: a res:// material or {\"class\": \"StandardMaterial3D\", ...}");
			}
		} else if (!file.begins_with("res://") || !ResourceLoader::get_singleton()->exists(file)) {
			return fail("No such file: " + file);
		}
		const String e = file.get_extension().to_lower();
		if (mat.is_valid()) {
		} else if (e == "tres" || e == "res" || e == "material") {
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
		if ((op == "bevel" || op == "loop_cut" || op == "mark_seam" || op == "clear_seam") && edges.size() < 2) {
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
			} else if (op == "mark_seam" || op == "clear_seam") {
				// (each edge's other marks as they were)
				for (int64_t i = 0; i + 1 < edges.size(); i += 2) {
					const Dictionary ea = m->get_edge_attributes(edges[i], edges[i + 1]);
					m->set_edge_attributes(edges[i], edges[i + 1], op == "mark_seam", ea.get("sharp", false), ea.get("crease", 0.0), ea.get("bevel_weight", 0.0));
				}
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
