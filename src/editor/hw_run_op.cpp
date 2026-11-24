// Hewn editor - the operations run by name (the menus, the header, the keys).

#include "hw_plugin_impl.h"

// ------------------------------------------------------------------ operations

// Extrude in edge / vertex mode: border edges (or selected edges) into faces, loose edges into faces,
// single vertices into loose edges. Returns false when there was nothing to extrude.
bool HewnEditorPlugin::_extrude_edges_verts(hewn::Mesh &m, hewn::Selection &s, float dist) {
	hewn::Topology t;
	t.build(m);
	std::vector<hewn::Edge> edges, wires;
	std::unordered_set<int> vs(s.verts.begin(), s.verts.end());
	if (mode == MODE_VERTEX) {
		for (const hewn::Edge &e : hewn::sel::border_edges(m, t)) {
			if (vs.count(e.a) && vs.count(e.b)) {
				edges.push_back(e);
			}
		}
		for (const std::pair<int, int> &w : m.wires) {
			if (vs.count(w.first) && vs.count(w.second)) {
				wires.push_back(hewn::Edge(w.first, w.second));
			}
		}
	} else {
		for (const hewn::Edge &e : s.edges) {
			(m.has_wire(e.a, e.b) ? wires : edges).push_back(e);
		}
	}
	// single vertices extrude into loose edges, loose edges into faces.
	std::unordered_set<int> covered;
	for (const hewn::Edge &e : edges) {
		covered.insert(e.a);
		covered.insert(e.b);
	}
	for (const hewn::Edge &e : wires) {
		covered.insert(e.a);
		covered.insert(e.b);
	}
	std::vector<int> lone;
	if (mode == MODE_VERTEX) {
		for (int v : s.verts) {
			if (!covered.count(v)) {
				lone.push_back(v);
			}
		}
	}
	std::vector<hewn::Edge> top;
	s.edges = edges.empty() ? std::vector<hewn::Edge>() : hewn::ops::extrude_edges(m, edges, dist, false);
	const std::vector<int> nv = hewn::ops::extrude_wires(m, wires, lone, &top);
	if (dist != 0.0f) {
		for (int v : nv) {
			m.pos[v] += Vector3(0, dist, 0);
		}
	}
	s.edges.insert(s.edges.end(), top.begin(), top.end());
	if (mode == MODE_VERTEX) {
		s.verts = hewn::sel::verts_of_edges(s.edges);
		for (int v : nv) {
			if (std::find(s.verts.begin(), s.verts.end(), v) == s.verts.end()) {
				s.verts.push_back(v);
			}
		}
	}
	return !s.edges.empty() || !s.verts.empty();
}

// The bridge's shape from its panel; a new bridge (not the panel changing it) starts with the handles of its own
// shape, not the ones dragged for the last.
hewn::ops::BridgeShape HewnEditorPlugin::_bridge_shape(const String &p_op) {
	if (!redo_running) {
		set_op_param(p_op, "own_a", false);
		set_op_param(p_op, "own_b", false);
		bridge_steps.clear();
	}
	hewn::ops::BridgeShape sh;
	sh.segments = MAX(1, (int)op_param(p_op, "segments"));
	sh.smooth = (float)op_param(p_op, "smooth");
	sh.twist = (float)op_param(p_op, "twist");
	sh.own_a = (bool)op_param(p_op, "own_a");
	sh.own_b = (bool)op_param(p_op, "own_b");
	sh.ha = Vector3((float)op_param(p_op, "hax"), (float)op_param(p_op, "hay"), (float)op_param(p_op, "haz"));
	sh.hb = Vector3((float)op_param(p_op, "hbx"), (float)op_param(p_op, "hby"), (float)op_param(p_op, "hbz"));
	sh.profile = (float)op_param(p_op, "profile");
	sh.profile_shape = (int)op_param(p_op, "profile_shape");
	if (sh.profile_shape == 7) {
		sh.profile_curve = _bridge_profile()->to_vector();
	}
	bridge_handles = false;
	return sh;
}

void HewnEditorPlugin::_bridge_shape_made(const hewn::ops::BridgeShape &p_shape, const Transform3D &p_xf) {
	bridge_handles = p_shape.made;
	if (!p_shape.made) {
		return;
	}
	bridge_ca = p_xf.xform(p_shape.out_ca);
	bridge_cb = p_xf.xform(p_shape.out_cb);
	bridge_ha = p_xf.xform(p_shape.out_ca + p_shape.out_ha);
	bridge_hb = p_xf.xform(p_shape.out_cb + p_shape.out_hb);
}

HewnEditorPlugin::BridgeStep HewnEditorPlugin::_bridge_step(int64_t p_version, const Dictionary &p_after) {
	BridgeStep st;
	st.version = p_version;
	st.after = p_after;
	st.after_holder = last_op_after_holder; // (p_after is the step's "after" now)
	st.params = op_params.has(last_op) ? Dictionary(op_params[last_op]).duplicate() : Dictionary();
	st.ca = bridge_ca;
	st.cb = bridge_cb;
	st.ha = bridge_ha;
	st.hb = bridge_hb;
	return st;
}

// Undo / redo landed on a step of the bridge (it made, a handle moved): the panel and the handles as they were then.
bool HewnEditorPlugin::_bridge_step_restore(int64_t p_version) {
	if (last_op != "bridge" && last_op != "bridge_faces") {
		return false;
	}
	for (const BridgeStep &st : bridge_steps) {
		if (st.version != p_version) {
			continue;
		}
		last_op_version = st.version;
		last_op_after = st.after;
		last_op_after_holder = st.after_holder;
		op_params[last_op] = st.params.duplicate();
		bridge_ca = st.ca;
		bridge_cb = st.cb;
		bridge_ha = st.ha;
		bridge_hb = st.hb;
		bridge_handles = true;
		update_overlays();
		return true;
	}
	return false;
}

bool HewnEditorPlugin::_bridge_handles_live() {
	if (!bridge_handles || (last_op != "bridge" && last_op != "bridge_faces") || !redo_panel || !redo_panel->is_visible()) {
		return false;
	}
	HewnMesh3D *n = get_edited();
	return n && n->get_instance_id() == last_op_node && (int)op_param(last_op, "segments") > 1;
}

void HewnEditorPlugin::_draw_bridge_handles(Control *p_overlay, Camera3D *p_cam) {
	const Color c(1.0f, 0.75f, 0.2f, 0.95f);
	const Vector3 ends[2][2] = { { bridge_ca, bridge_ha }, { bridge_cb, bridge_hb } };
	for (int i = 0; i < 2; i++) {
		if (p_cam->is_position_behind(ends[i][1]) || p_cam->is_position_behind(ends[i][0])) {
			continue;
		}
		p_overlay->draw_line(p_cam->unproject_position(ends[i][0]), p_cam->unproject_position(ends[i][1]), c, 1.5f, true);
		const Vector2 sp = p_cam->unproject_position(ends[i][1]);
		p_overlay->draw_circle(sp, bridge_drag == i ? 7.0f : 5.5f, Color(0, 0, 0, 0.6f));
		p_overlay->draw_circle(sp, bridge_drag == i ? 5.5f : 4.0f, bridge_drag == i ? Color(1, 1, 1, 1) : c);
	}
}

// The bridge's two handles: a press near one picks it, dragging moves it (in the plane facing the view through it),
// the bridge made again with it.
int32_t HewnEditorPlugin::_bridge_handle_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		if (mb->is_pressed()) {
			const Vector3 hs[2] = { bridge_ha, bridge_hb };
			for (int i = 0; i < 2; i++) {
				if (!p_cam->is_position_behind(hs[i]) && p_cam->unproject_position(hs[i]).distance_to(mb->get_position()) < 10.0f) {
					bridge_drag = i;
					bridge_moved = false;
					if (HewnMesh3D *bn = get_edited()) {
						bridge_drag_before = bn->get_state();
					}
					if (bridge_steps.empty()) {
						bridge_steps.push_back(_bridge_step(last_op_version, last_op_after)); // (the bridge as it was made)
					}
					update_overlays();
					return AFTER_GUI_INPUT_STOP;
				}
			}
			return AFTER_GUI_INPUT_PASS;
		}
		if (bridge_drag >= 0) {
			bridge_drag = -1;
			HewnMesh3D *bn = get_edited();
			if (bridge_moved && bn) {
				// the handle's move a step of its own: the bridge's step back as it was, a new one from there (the panel
				// changes the new one from now on)
				const Dictionary now = bn->get_state();
				last_op_after.clear();
				last_op_after.merge(bridge_drag_before);
				hw_update_live(last_op_after_holder, last_op_after);
				EditorUndoRedoManager *ur = get_undo_redo();
				ur->create_action("Hewn: Move Bridge Handle", UndoRedo::MERGE_DISABLE, bn);
				last_op_after_holder = hw_undo_state_live(ur, bn, now);
				hw_undo_state(ur, false, bn, bridge_drag_before);
				ur->commit_action(false);
				last_op_after = now;
				UndoRedo *h = ur->get_history_undo_redo(ur->get_object_history_id(bn));
				last_op_version = h ? (int64_t)h->get_version() : last_op_version;
				// (the steps after this one in the history are gone: a new one from here)
				while (!bridge_steps.empty() && bridge_steps.back().version >= last_op_version) {
					bridge_steps.pop_back();
				}
				bridge_steps.push_back(_bridge_step(last_op_version, now));
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (mm.is_valid() && bridge_drag >= 0) {
		HewnMesh3D *n = get_edited();
		if (!n || !_bridge_handles_live()) {
			bridge_drag = -1;
			return AFTER_GUI_INPUT_PASS;
		}
		Vector3 o, d;
		_ray(p_cam, mm->get_position(), o, d);
		const Vector3 h = bridge_drag == 0 ? bridge_ha : bridge_hb, c = bridge_drag == 0 ? bridge_ca : bridge_cb;
		const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2);
		const float den = fwd.dot(d);
		if (std::fabs(den) < 1e-6f) {
			return AFTER_GUI_INPUT_STOP;
		}
		const Vector3 w = o + d * (fwd.dot(h - o) / den);
		const Transform3D inv = n->get_global_transform().affine_inverse();
		const Vector3 v = inv.xform(w) - inv.xform(c);
		const char *k = bridge_drag == 0 ? "a" : "b";
		set_op_param(last_op, bridge_drag == 0 ? "own_a" : "own_b", true);
		set_op_param(last_op, (String("h") + k + "x").utf8().get_data(), v.x);
		set_op_param(last_op, (String("h") + k + "y").utf8().get_data(), v.y);
		set_op_param(last_op, (String("h") + k + "z").utf8().get_data(), v.z);
		const int keep = bridge_drag;
		bridge_moved = true;
		_redo_last();
		bridge_drag = keep;
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	return AFTER_GUI_INPUT_PASS;
}

// A mesh broken into pieces: a Hewn mesh of its own for each (its origin at its middle, the modifiers left on each),
// in the place of the one it was (one undo step).
void HewnEditorPlugin::_pieces_as_objects(HewnMesh3D *n, const hewn::Mesh &p_result, const TypedArray<HewnModifier> &p_rest) {
	Node *parent = n->get_parent();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	const std::vector<hewn::Mesh> parts = hewn::ops::loose_parts(p_result);
	if (!parent || !root || parts.size() < 2) {
		if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
			t->push_toast(T("Fracture: no pieces to make (a closed mesh breaks; more pieces?)"));
		}
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Apply Fracture (Pieces)", UndoRedo::MERGE_DISABLE, root);
	for (size_t k = 0; k < parts.size(); k++) {
		hewn::Mesh pm = parts[k];
		Vector3 c;
		for (const Vector3 &q : pm.pos) {
			c += q / (float)pm.pos.size();
		}
		for (Vector3 &q : pm.pos) {
			q -= c;
		}
		HewnMesh3D *pc = memnew(HewnMesh3D);
		pc->set_name(String(n->get_name()) + "_" + itos((int64_t)k + 1));
		pc->set_shape(hewn::shapes::CUSTOM);
		pc->get_data() = pm;
		pc->set_materials(n->get_materials());
		pc->set_modifiers(p_rest.duplicate(true));
		pc->set_collision_mode(n->get_collision_mode() == HewnMesh3D::COLLISION_NONE ? HewnMesh3D::COLLISION_NONE : HewnMesh3D::COLLISION_CONVEX);
		pc->commit(false);
		pc->set_transform(n->get_transform() * Transform3D(Basis(), c));
		ur->add_do_method(parent, "add_child", pc, true);
		ur->add_do_method(pc, "set_owner", root);
		ur->add_do_reference(pc);
		ur->add_undo_method(parent, "remove_child", pc);
	}
	ur->add_do_method(parent, "remove_child", n);
	ur->add_undo_method(parent, "add_child", n, true);
	ur->add_undo_method(parent, "move_child", n, n->get_index());
	ur->add_undo_method(n, "set_owner", n->get_owner());
	ur->add_undo_reference(n);
	ur->commit_action();
}

void HewnEditorPlugin::run_op(const String &p_op) {
	EditorInterface *ei = EditorInterface::get_singleton();
	EditorUndoRedoManager *ur = get_undo_redo();

	if (p_op.begins_with("scene:")) {
		start_scene_op(p_op.substr(6));
		return;
	}
	if (p_op == "physics") {
		start_physics();
		return;
	}
	if (p_op == "shape_to_curve") {
		shape_to_curve();
		return;
	}
	if (p_op == "curve_edges_poly" && mode == MODE_OBJECT) {
		// a shape through points (a wall...): on a curve of its own, not its edges as one
		for (HewnMesh3D *m : _selected_meshes()) {
			const int t = m->get_shape();
			if ((t == hewn::shapes::WALL || t == hewn::shapes::SWEEP || t == hewn::shapes::PATH_STAIRS || t == hewn::shapes::POLY_SHAPE || t == hewn::shapes::ROOF) &&
					m->get_follow_curve().is_empty() && m->get_poly_points().size() >= 2) {
				shape_to_curve();
				return;
			}
		}
	}
	if (p_op == "curve_edges_poly" && mode == MODE_OBJECT && _selected_meshes().empty()) {
		// (Convert to Curve with curves selected: Godot's Path3D of them)
		bool curves = false;
		for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
			curves = curves || Object::cast_to<HewnCurve3D>(v) != nullptr;
		}
		if (curves) {
			curve_object_op("to_path3d");
			return;
		}
	}
	if (p_op == "curve_edges_poly" || p_op == "curve_edges_bezier" || p_op == "curve_edges_nurbs") {
		mesh_to_curve(p_op == "curve_edges_poly" ? 0 : p_op == "curve_edges_bezier" ? 1 : 2);
		return;
	}
	if (p_op == "phys_apply" || p_op == "phys_cancel") {
		_physics_stop(p_op == "phys_apply");
		return;
	}
	if (p_op == "checker_deselect") {
		checker_deselect();
		return;
	}
	if (p_op == "by_normal") {
		select_by_normal();
		return;
	}
	if (p_op == "knife_project") {
		knife_project();
		return;
	}
	if (p_op == "sweep_edges") {
		sweep_along_edges();
		return;
	}
	if (p_op == "wall_edges") {
		wall_along_edges();
		return;
	}
	if (p_op == "delete_inner") {
		// the faces no one can see (inside the merged solid) of the selected objects / the edited meshes
		const std::vector<HewnMesh3D *> list = mode == MODE_OBJECT ? _selected_meshes() : edited_meshes();
		int total = 0;
		bool any = false;
		for (HewnMesh3D *o : list) {
			const std::vector<int> h = hewn::ops::inner_faces(o->get_data());
			if (h.empty()) {
				continue;
			}
			if (!any) {
				ur->create_action("Hewn: Delete Inner Faces");
				any = true;
			}
			const Dictionary before = o->get_state();
			hewn::Mesh copy = o->get_data();
			hewn::ops::delete_faces(copy, h);
			std::swap(copy, o->get_data());
			o->selection.clear();
			Dictionary after = o->get_state();
			after["shape"] = (int)hewn::shapes::CUSTOM; // (not its parameters' shape any more)
			std::swap(copy, o->get_data());
			hw_undo_state(ur, true, o, after);
			hw_undo_state(ur, false, o, before);
			total += (int)h.size();
		}
		if (any) {
			ur->commit_action();
			selection_changed();
		}
		if (EditorToaster *tt = ei->get_editor_toaster()) {
			tt->push_toast(vformat(T("Inner faces deleted: %d"), total), EditorToaster::SEVERITY_INFO);
		}
		return;
	}
	if (p_op == "bake_light") {
		open_bake_light(); // (its settings first; edit mode: the edited meshes, else the selected objects)
		return;
	}
	if (p_op == "obj:subsurf_modifier") {
		add_subdivision_modifier();
		return;
	}
	if (p_op == "subdivide_faces" || p_op == "catmull_clark") {
		// many faces more (Editor Settings > Hewn: from how many): asked first, nothing done meanwhile; a
		// value dragged in the panel is asked about once the mouse is let go
		heavy_op = p_op;
		const int cuts = (int)op_param(p_op, p_op == "catmull_clark" ? "levels" : "cuts");
		const int64_t limit = subdivide_limit();
		if (!heavy_ok && limit > 0 && _heavy_added(cuts) >= limit) {
			heavy_redo = redo_running;
			heavy_cuts_wanted = cuts;
			if (Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) {
				heavy_wait = true;
			} else {
				callable_mp(this, &HewnEditorPlugin::_heavy_popup).call_deferred();
			}
			return;
		}
		heavy_ok = false;
		heavy_wait = false;
		heavy_prev_cuts = cuts;
	}

	// Dock buttons that only preset a parameter of a redo-able operation.
	if (p_op == "extrude_region" || p_op == "extrude_individual") {
		set_op_param("extrude", "individual", p_op == "extrude_individual");
		run_op("extrude");
		return;
	}
	if (p_op == "face_cutter") {
		start_face_cutter();
		return;
	}
	if (p_op.begins_with("obj:")) {
		_run_object_op(p_op.substr(4));
		return;
	}
	if (p_op == "bevel_profile") {
		edit_bevel_profile();
		return;
	}
	if (p_op == "arch_profile") {
		edit_arch_profile();
		return;
	}
	if (p_op == "bridge_profile") {
		edit_bridge_profile();
		return;
	}
	if (p_op == "new_blend_material") {
		open_blend_dialog();
		return;
	}
	if (p_op == "navmesh_bake") {
		open_navmesh_bake();
		return;
	}
	if (p_op == "scrolling_material") {
		// the image: of the selected faces' material (or the mesh's first), else of the texture picked last
		String image;
		auto image_of = [](const Ref<Material> &p_m) {
			if (Ref<BaseMaterial3D> bm = p_m; bm.is_valid() && bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO).is_valid()) {
				return bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO)->get_path();
			}
			if (Ref<ShaderMaterial> sm = p_m; sm.is_valid()) {
				for (const char *pn : { "albedo_texture", "base_texture", "texture_albedo" }) {
					const Ref<Texture2D> t = sm->get_shader_parameter(pn);
					if (t.is_valid()) {
						return t->get_path();
					}
				}
			}
			return String();
		};
		if (HewnMesh3D *sm = get_edited()) {
			int slot = 0;
			if (mode == MODE_FACE && !sm->selection.faces.empty()) {
				slot = sm->get_data().faces[sm->selection.faces[0]].material;
			}
			image = image_of(sm->get_material_for_slot(slot));
		}
		if (image.is_empty()) {
			// (a curve selected: of its material)
			const TypedArray<Node> csel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
			for (int64_t i = 0; i < csel.size() && image.is_empty(); i++) {
				if (HewnCurve3D *hc = Object::cast_to<HewnCurve3D>(csel[i])) {
					image = image_of(hc->get_material());
				}
			}
		}
		if (image.is_empty() && texture_dock) {
			image = image_of(texture_dock->current_material);
		}
		if (image.is_empty() || !texture_dock) {
			if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
				t->push_toast(T("Scrolling Material: select a mesh with a textured material, or pick a texture first"), EditorToaster::SEVERITY_WARNING);
			}
			return;
		}
		texture_dock->make_scrolling(image);
		return;
	}
	if (p_op == "show_vertex_colors") {
		_toggle_vertex_color_view();
		return;
	}
	if (p_op == "spin_tool") {
		// center: the 3D cursor, axis: the view direction
		Camera3D *cam = _viewport_camera();
		const Vector3 ax = cam ? -cam->get_global_transform().basis.get_column(2) : Vector3(0, 1, 0);
		set_op_param("spin", "cx", cursor_pos.x);
		set_op_param("spin", "cy", cursor_pos.y);
		set_op_param("spin", "cz", cursor_pos.z);
		set_op_param("spin", "ax", ax.x);
		set_op_param("spin", "ay", ax.y);
		set_op_param("spin", "az", ax.z);
		run_op("spin");
		return;
	}
	if (p_op == "recalc_outside" || p_op == "recalc_inside") {
		// one operation, its panel has "Inside"
		set_op_param("recalc_normals", "inside", p_op == "recalc_inside");
		run_op("recalc_normals");
		return;
	}
	if (p_op == "inset_region" || p_op == "inset_individual") {
		set_op_param("inset", "individual", p_op == "inset_individual");
		run_op("inset");
		return;
	}

	// Tools.
	if (p_op == "poly_shape" || p_op == "edit_poly") {
		start_poly_tool(p_op == "edit_poly");
		return;
	}
	if (p_op == "knife") {
		start_knife();
		return;
	}
	if (p_op == "loop_cut_tool") {
		start_loop_cut();
		return;
	}
	if (p_op == "cutter") {
		start_cutter();
		return;
	}
	if (p_op == "clip_tool") {
		start_clip();
		return;
	}
	if (p_op == "bisect_tool") {
		start_bisect();
		return;
	}
	if (p_op == "vertex_paint") {
		start_paint();
		return;
	}
	if (p_op == "ruler") {
		start_ruler();
		return;
	}
	if (p_op == "rip") {
		SubViewport *vp = ei->get_editor_viewport_3d(0);
		if (vp && vp->get_camera_3d()) {
			mouse_pos = Vector2(vp->get_size()) * 0.5f + Vector2(20, 20);
			_start_rip(vp->get_camera_3d());
		}
		return;
	}
	if (p_op == "edit_mode") {
		toggle_edit_mode();
		return;
	}

	// Operations on the editor selection (may not involve an edited Hewn node).
	if (p_op == "scene_size") {
		_scene_size_report();
		return;
	}
	if (p_op.begins_with("curve:")) {
		curve_op(p_op.substr(6));
		return;
	}
	if (p_op == "convert_area") {
		// a trigger volume: an Area3D in its place, with a convex shape of its form
		Node *root = ei->get_edited_scene_root();
		ur->create_action("Hewn: Convert to Area3D");
		bool any = false;
		for (HewnMesh3D *pm : _selected_meshes()) {
			Node *parent = pm->get_parent();
			if (!parent || pm == root) {
				continue;
			}
			Area3D *area = memnew(Area3D);
			area->set_name(pm->get_name());
			area->set_transform(pm->get_transform());
			CollisionShape3D *cs = memnew(CollisionShape3D);
			cs->set_name("CollisionShape3D");
			Ref<ConvexPolygonShape3D> sh;
			sh.instantiate();
			PackedVector3Array pts;
			for (const Vector3 &p : pm->get_data().pos) {
				pts.push_back(p);
			}
			sh->set_points(pts);
			cs->set_shape(sh);
			area->add_child(cs);
			ur->add_do_method(parent, "add_child", area, true);
			ur->add_do_method(area, "set_owner", root);
			ur->add_do_method(cs, "set_owner", root);
			ur->add_do_method(parent, "move_child", area, pm->get_index());
			ur->add_do_method(parent, "remove_child", pm);
			ur->add_do_reference(area);
			ur->add_undo_method(parent, "remove_child", area);
			ur->add_undo_method(parent, "add_child", pm, true);
			ur->add_undo_method(parent, "move_child", pm, pm->get_index());
			ur->add_undo_method(pm, "set_owner", pm->get_owner());
			ur->add_undo_reference(pm);
			any = true;
		}
		ur->commit_action(any);
		return;
	}
	if (p_op == "convert_mesh") {
		TypedArray<Node> sel = ei->get_selection()->get_top_selected_nodes();
		Node *root = ei->get_edited_scene_root();
		for (int i = 0; i < sel.size(); i++) {
			// a MeshInstance3D, or a CSG shape (the whole of it, from its top one: what it makes)
			Node3D *mi = Object::cast_to<Node3D>(sel[i]);
			Ref<Mesh> src;
			if (MeshInstance3D *plain = Object::cast_to<MeshInstance3D>(mi)) {
				src = Object::cast_to<HewnMesh3D>(plain) ? Ref<Mesh>() : plain->get_mesh();
			} else if (CSGShape3D *csg = Object::cast_to<CSGShape3D>(mi)) {
				src = csg->is_root_shape() ? Ref<Mesh>(csg->bake_static_mesh()) : Ref<Mesh>();
			}
			if (!mi || src.is_null() || !mi->get_parent()) {
				continue;
			}
			Node *parent = mi->get_parent();
			HewnMesh3D *made_mesh = memnew(HewnMesh3D);
			made_mesh->set_name(mi->get_name());
			made_mesh->create_from_mesh(src, true);
			made_mesh->set_transform(mi->get_transform());
			ur->create_action("Hewn: Convert to Hewn Mesh");
			ur->add_do_method(parent, "add_child", made_mesh, true);
			ur->add_do_method(made_mesh, "set_owner", root);
			ur->add_do_method(parent, "move_child", made_mesh, mi->get_index());
			ur->add_do_method(parent, "remove_child", mi);
			ur->add_do_reference(made_mesh);
			ur->add_undo_method(parent, "remove_child", made_mesh);
			ur->add_undo_method(parent, "add_child", mi, true);
			ur->add_undo_method(parent, "move_child", mi, mi->get_index());
			ur->add_undo_method(mi, "set_owner", root);
			ur->add_undo_reference(mi);
			ur->commit_action();
		}
		return;
	}
	if (p_op == "merge_objects") {
		TypedArray<Node> sel = ei->get_selection()->get_selected_nodes();
		std::vector<HewnMesh3D *> list;
		for (int i = 0; i < sel.size(); i++) {
			if (HewnMesh3D *p = Object::cast_to<HewnMesh3D>(sel[i])) {
				list.push_back(p);
			}
		}
		if (list.size() < 2) {
			return;
		}
		HewnMesh3D *target = list[0];
		Node *root = ei->get_edited_scene_root();
		const Dictionary before = target->get_state();
		const TypedArray<Material> mats_before = target->get_materials();
		for (size_t i = 1; i < list.size(); i++) {
			target->merge_from(list[i]);
		}
		ur->create_action("Hewn: Merge Objects");
		ur->add_do_method(target, "set_materials", target->get_materials());
		hw_undo_state(ur, true, target, target->get_state());
		ur->add_undo_method(target, "set_materials", mats_before);
		hw_undo_state(ur, false, target, before);
		for (size_t i = 1; i < list.size(); i++) {
			Node *parent = list[i]->get_parent();
			ur->add_do_method(parent, "remove_child", list[i]);
			ur->add_undo_method(parent, "add_child", list[i], true);
			ur->add_undo_method(parent, "move_child", list[i], list[i]->get_index());
			ur->add_undo_method(list[i], "set_owner", root);
			ur->add_undo_reference(list[i]);
		}
		ur->commit_action();
		ei->get_selection()->clear();
		ei->get_selection()->add_node(target);
		return;
	}
	if (p_op == "boolean_union" || p_op == "boolean_subtract" || p_op == "boolean_intersect") {
		// the last selected (active) mesh is kept, the others are combined into it.
		TypedArray<Node> sel = ei->get_selection()->get_selected_nodes();
		std::vector<HewnMesh3D *> list;
		for (int i = 0; i < sel.size(); i++) {
			if (HewnMesh3D *p = Object::cast_to<HewnMesh3D>(sel[i])) {
				list.push_back(p);
			}
		}
		if (list.size() < 2) {
			ERR_PRINT("Hewn: select two or more Hewn meshes; the last selected one is kept.");
			return;
		}
		// the active one first: the one selected last
		HewnMesh3D *active = sel_order.empty() ? nullptr : Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(sel_order.back()));
		auto it = std::find(list.begin(), list.end(), active);
		if (it != list.end()) {
			std::rotate(list.begin(), it, it + 1);
		} else {
			std::rotate(list.begin(), list.end() - 1, list.end());
		}
		const int op = p_op == "boolean_union" ? HewnMesh3D::BOOLEAN_UNION : p_op == "boolean_subtract" ? HewnMesh3D::BOOLEAN_SUBTRACT : HewnMesh3D::BOOLEAN_INTERSECT;
		HewnMesh3D *target = list[0];
		Node *root = ei->get_edited_scene_root();
		if (dock->boolean_live->is_pressed()) {
			// Non-destructive: the others become live operands (cutters).
			const TypedArray<HewnModifier> m0 = target->get_modifiers();
			TypedArray<HewnModifier> m1 = m0.duplicate();
			for (size_t i = 1; i < list.size(); i++) {
				Ref<HewnBooleanModifier> b;
				b.instantiate();
				b->set_object(target->get_path_to(list[i]));
				b->set_operation(op);
				m1.push_back(b);
			}
			ur->create_action("Hewn: Boolean Modifier");
			ur->add_do_method(target, "set_modifiers", m1);
			ur->add_undo_method(target, "set_modifiers", m0);
			for (size_t i = 1; i < list.size(); i++) {
				ur->add_do_method(list[i], "set_boolean_cutter", true);
				ur->add_undo_method(list[i], "set_boolean_cutter", list[i]->is_boolean_cutter());
			}
			ur->commit_action();
			ei->get_selection()->clear();
			ei->get_selection()->add_node(target);
			return;
		}
		const Dictionary before = target->get_state();
		for (size_t i = 1; i < list.size(); i++) {
			target->boolean_with(list[i], op);
		}
		ur->create_action("Hewn: Boolean");
		hw_undo_state(ur, true, target, target->get_state());
		hw_undo_state(ur, false, target, before);
		for (size_t i = 1; i < list.size(); i++) {
			Node *parent = list[i]->get_parent();
			ur->add_do_method(parent, "remove_child", list[i]);
			ur->add_undo_method(parent, "add_child", list[i], true);
			ur->add_undo_method(parent, "move_child", list[i], list[i]->get_index());
			ur->add_undo_method(list[i], "set_owner", root);
			ur->add_undo_reference(list[i]);
		}
		ur->commit_action();
		ei->get_selection()->clear();
		ei->get_selection()->add_node(target);
		return;
	}

	if (p_op == "bake") {
		_bake_to_mesh_instance();
		return;
	}
	HewnMesh3D *n = get_edited();
	if (!n && p_op == "export_gltf") {
		// (no mesh: the selected curves and overlays can go in a file too)
		const TypedArray<Node> gsel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
		for (int64_t i = 0; i < gsel.size(); i++) {
			Node *g = Object::cast_to<Node>(gsel[i]);
			if (Object::cast_to<HewnCurve3D>(g) || Object::cast_to<HewnOverlay3D>(g)) {
				gltf_dialog->set_current_file(String(g->get_name()) + ".glb");
				gltf_dialog->popup_file_dialog();
				return;
			}
		}
	}
	if (!n) {
		return;
	}
	if (!multi_ids.empty() && !multi_running && p_op != "export_gltf" && p_op != "export_obj" && p_op != "uv_editor") {
		// several meshes edited: the operation on each, one undo step
		multi_running = true;
		multi_merge = false;
		const uint64_t keep = edited_id;
		for (HewnMesh3D *o : edited_meshes()) {
			edited_id = o->get_instance_id();
			run_op(p_op);
			multi_merge = true;
		}
		edited_id = keep;
		multi_running = false;
		multi_merge = false;
		for (HewnMesh3D *o : edited_meshes()) {
			o->update_gizmos();
		}
		selection_changed();
		return;
	}
	hewn::Mesh &m = n->get_data();
	hewn::Selection &s = n->selection;
	hewn::Topology t;
	t.build(m);

	// ---- selection (not undoable)
	if (p_op.begins_with("select_") || p_op == "invert" || p_op == "grow" || p_op == "shrink" || p_op == "linked" || p_op == "loop" ||
			p_op == "ring" || p_op == "by_material" || p_op == "by_normal") {
		if (p_op == "select_all") {
			s.verts = hewn::sel::all_verts(m);
			s.edges = hewn::sel::all_edges(m, t);
			s.faces = hewn::sel::all_faces(m);
		} else if (p_op == "select_none") {
			s.clear();
		} else if (p_op == "select_border") {
			std::vector<hewn::Edge> b = hewn::sel::border_edges(m, t);
			if (mode == MODE_VERTEX) {
				s.verts = hewn::sel::verts_of_edges(b);
			} else {
				s.edges = b;
			}
		} else if (p_op == "invert") {
			s.verts = hewn::sel::invert_verts(m, s.verts);
			s.edges = hewn::sel::invert_edges(m, t, s.edges);
			s.faces = hewn::sel::invert_faces(m, s.faces);
		} else if (p_op == "grow") {
			if (mode == MODE_VERTEX) {
				s.verts = hewn::sel::grow_verts(m, t, s.verts);
			} else if (mode == MODE_EDGE) {
				s.edges = hewn::sel::grow_edges(m, t, s.edges);
			} else {
				s.faces = hewn::sel::grow_faces(m, t, s.faces);
			}
		} else if (p_op == "shrink") {
			if (mode == MODE_VERTEX) {
				s.verts = hewn::sel::shrink_verts(m, t, s.verts);
			} else if (mode == MODE_EDGE) {
				s.edges = hewn::sel::shrink_edges(m, t, s.edges);
			} else {
				s.faces = hewn::sel::shrink_faces(m, t, s.faces);
			}
		} else if (p_op == "linked") {
			if (mode == MODE_VERTEX) {
				s.verts = hewn::sel::linked_verts(m, t, s.verts);
			} else if (mode == MODE_EDGE) {
				std::vector<int> v = hewn::sel::linked_verts(m, t, hewn::sel::verts_of_edges(s.edges));
				s.edges = hewn::sel::edges_from_verts(m, t, v);
			} else {
				s.faces = hewn::sel::linked_faces(m, t, s.faces);
			}
		} else if (p_op == "loop" || p_op == "ring") {
			const bool ring = p_op == "ring";
			if (mode == MODE_EDGE) {
				std::vector<hewn::Edge> out;
				for (const hewn::Edge &e : s.edges) {
					std::vector<hewn::Edge> l = ring ? hewn::sel::edge_ring(m, t, e) : hewn::sel::edge_loop(m, t, e);
					out.insert(out.end(), l.begin(), l.end());
				}
				s.edges = hewn::sel::uniq(out);
			} else if (mode == MODE_FACE) {
				s.faces = hewn::sel::face_loops(m, t, s.faces, ring);
			}
		} else if (p_op == "by_material") {
			s.faces = hewn::sel::faces_by_material(m, s.faces);
		} else if (p_op == "select_inner") {
			// the faces no one can see (inside the solid): to look at, then delete
			if (mode != MODE_FACE) {
				set_mode(MODE_FACE);
			}
			s.faces = hewn::ops::inner_faces(m);
		} else if (p_op == "select_invalid_uvs") {
			// faces whose UVs have (almost) no area (the texture is one smeared line or point)
			if (mode != MODE_FACE) {
				set_mode(MODE_FACE);
			}
			std::vector<std::vector<Vector2>> uvs;
			hewn::compute_uvs(m, n->get_global_transform(), uvs);
			s.faces.clear();
			for (int f = 0; f < (int)uvs.size(); f++) {
				double area = 0.0, span = 0.0;
				for (size_t i = 0; i < uvs[f].size(); i++) {
					const Vector2 &a = uvs[f][i], &b = uvs[f][(i + 1) % uvs[f].size()];
					area += (double)a.x * b.y - (double)b.x * a.y;
					span = MAX(span, (double)a.distance_to(b));
				}
				if (std::fabs(area) * 0.5 <= 1e-6 * MAX(1e-6, span * span)) {
					s.faces.push_back(f);
				}
			}
		}
		selection_changed();
		return;
	}

	// ---- object-level operations with their own undo
	if (p_op == "freeze" || p_op == "center_pivot" || p_op == "pivot_to_selection") {
		const Transform3D xf_before = n->get_transform();
		const Dictionary before = n->get_state();
		if (p_op == "freeze") {
			n->freeze_transform();
		} else if (p_op == "center_pivot") {
			n->center_pivot();
		} else {
			std::vector<int> vs = _selected_verts();
			if (vs.empty()) {
				return;
			}
			Vector3 c;
			for (int v : vs) {
				c += m.pos[v];
			}
			n->set_pivot(c / (float)vs.size());
		}
		ur->create_action("Hewn: " + p_op.capitalize(), UndoRedo::MERGE_DISABLE, n);
		ur->add_do_method(n, "set_transform", n->get_transform());
		hw_undo_state(ur, true, n, n->get_state());
		ur->add_undo_method(n, "set_transform", xf_before);
		hw_undo_state(ur, false, n, before);
		ur->commit_action(false);
		selection_changed();
		return;
	}
	if (p_op.begins_with("separate_")) {
		_separate(p_op.substr(9));
		return;
	}
	if (p_op.begins_with("apply_modifier:")) {
		// one modifier alone (the others stay on it); a fracture: a mesh of its own for each piece
		const int idx = p_op.get_slicec(':', 1).to_int();
		const TypedArray<HewnModifier> mods = n->get_modifiers();
		if (idx < 0 || idx >= mods.size()) {
			return;
		}
		const Ref<HewnFractureModifier> fm = mods[idx];
		if (fm.is_valid()) {
			hewn::Mesh m = n->get_data();
			fm->apply(m, n);
			TypedArray<HewnModifier> rest = mods.duplicate();
			rest.remove_at(idx);
			_pieces_as_objects(n, m, rest);
			return;
		}
		const Dictionary before = n->get_state();
		n->apply_modifier(idx);
		ur->create_action("Hewn: Apply Modifier", UndoRedo::MERGE_DISABLE, n);
		ur->add_do_method(n, "set_modifiers", n->get_modifiers());
		hw_undo_state(ur, true, n, n->get_state());
		hw_undo_state(ur, false, n, before);
		ur->add_undo_method(n, "set_modifiers", mods);
		ur->commit_action(false);
		selection_changed();
		return;
	}
	if (p_op == "apply_modifiers") {
		if (!n->has_modifiers()) {
			return;
		}
		const Dictionary before = n->get_state();
		const TypedArray<HewnModifier> mods = n->get_modifiers();
		for (int64_t i = 0; i < mods.size(); i++) {
			const Ref<HewnFractureModifier> fm = mods[i];
			if (fm.is_valid() && fm->is_enabled()) {
				// (broken into pieces: a mesh of its own each)
				_pieces_as_objects(n, n->evaluated_mesh(), TypedArray<HewnModifier>());
				return;
			}
		}
		Ref<HewnArrayModifier> last = mods.is_empty() ? Ref<HewnArrayModifier>() : Ref<HewnArrayModifier>(mods[mods.size() - 1]);
		Node *parent = n->get_parent();
		Node *root = ei->get_edited_scene_root();
		if (last.is_valid() && last->is_enabled() && !last->get_realize_instances() && parent && n != root) {
			// the array's copies as objects of their own (the modifiers before it applied to each), in its place
			TypedArray<HewnModifier> rest = mods.duplicate();
			rest.remove_at(rest.size() - 1);
			n->set_modifiers(rest);
			const hewn::Mesh base = n->evaluated_mesh();
			n->set_modifiers(mods);
			const std::vector<Transform3D> copies = last->copies(base, n);
			ur->create_action("Hewn: Apply Modifiers (Objects)", UndoRedo::MERGE_DISABLE, root);
			for (size_t k = 0; k < copies.size(); k++) {
				HewnMesh3D *c = memnew(HewnMesh3D);
				c->set_name(String(n->get_name()) + "_" + itos((int64_t)k + 1));
				c->set_shape(hewn::shapes::CUSTOM);
				c->get_data() = base;
				c->set_materials(n->get_materials());
				c->commit(false);
				c->set_transform(n->get_transform() * copies[k]);
				ur->add_do_method(parent, "add_child", c, true);
				ur->add_do_method(c, "set_owner", root);
				ur->add_do_reference(c);
				ur->add_undo_method(parent, "remove_child", c);
			}
			ur->add_do_method(parent, "remove_child", n);
			ur->add_undo_method(parent, "add_child", n, true);
			ur->add_undo_method(parent, "move_child", n, n->get_index());
			ur->add_undo_method(n, "set_owner", n->get_owner());
			ur->add_undo_reference(n);
			ur->commit_action();
			return;
		}
		n->apply_modifiers();
		ur->create_action("Hewn: Apply Modifiers", UndoRedo::MERGE_DISABLE, n);
		ur->add_do_method(n, "set_modifiers", TypedArray<HewnModifier>());
		hw_undo_state(ur, true, n, n->get_state());
		hw_undo_state(ur, false, n, before);
		ur->add_undo_method(n, "set_modifiers", mods);
		ur->commit_action(false);
		selection_changed();
		return;
	}
	if (p_op == "bake") {
		bool fractured = false;
		for (const Variant &mv : n->get_modifiers()) {
			const Ref<HewnFractureModifier> fm = mv;
			fractured = fractured || (fm.is_valid() && fm->is_enabled());
		}
		if (fractured) {
			// its pieces: a MeshInstance3D each, under a Node3D in its place
			Node *pieces = HewnLevel::bake_pieces_of(n, false);
			Node *parent = n->get_parent();
			Node *root = ei->get_edited_scene_root();
			if (!pieces || !parent || !root) {
				return;
			}
			pieces->set_name(String(n->get_name()) + "_Baked");
			ur->create_action("Hewn: Bake to MeshInstance3D");
			ur->add_do_method(parent, "add_child", pieces, true);
			ur->add_do_method(pieces, "set_owner", root);
			for (const Variant &cv : pieces->find_children("*", "", true, false)) {
				ur->add_do_method(Object::cast_to<Node>(cv), "set_owner", root);
			}
			ur->add_do_method(n, "set_visible", false);
			ur->add_do_reference(pieces);
			ur->add_undo_method(parent, "remove_child", pieces);
			ur->add_undo_method(n, "set_visible", n->is_visible());
			ur->commit_action();
			return;
		}
		MeshInstance3D *mi = memnew(MeshInstance3D);
		mi->set_name(String(n->get_name()) + "_Baked");
		mi->set_mesh(n->to_array_mesh());
		Node *parent = n->get_parent();
		Node *root = ei->get_edited_scene_root();
		if (!parent || !root) {
			memdelete(mi);
			return;
		}
		mi->set_transform(n->get_transform());
		ur->create_action("Hewn: Bake to MeshInstance3D");
		ur->add_do_method(parent, "add_child", mi, true);
		ur->add_do_method(mi, "set_owner", root);
		ur->add_do_method(n, "set_visible", false);
		ur->add_do_reference(mi);
		ur->add_undo_method(parent, "remove_child", mi);
		ur->add_undo_method(n, "set_visible", n->is_visible());
		ur->commit_action();
		return;
	}
	if (p_op == "export_gltf") {
		gltf_dialog->set_current_file(String(n->get_name()) + ".glb");
		gltf_dialog->popup_file_dialog();
		return;
	}
	if (p_op == "save_mesh_file") {
		// the mesh as it is shown (its modifiers applied) as a mesh resource of its own (.tres / .res / .mesh)
		if (!mesh_file_dialog) {
			mesh_file_dialog = memnew(EditorFileDialog);
			mesh_file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
			mesh_file_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
			mesh_file_dialog->add_filter("*.tres", "Text Resource");
			mesh_file_dialog->add_filter("*.res", "Binary Resource");
			mesh_file_dialog->add_filter("*.mesh", "Mesh");
			mesh_file_dialog->set_title(hw_editor::T("Save Mesh as File"));
			mesh_file_dialog->connect("file_selected", callable_mp(this, &HewnEditorPlugin::_on_mesh_file));
			EditorInterface::get_singleton()->get_base_control()->add_child(mesh_file_dialog);
		}
		mesh_file_node = ObjectID(n->get_instance_id());
		mesh_file_dialog->set_current_file(String(n->get_name()) + ".tres");
		mesh_file_dialog->popup_file_dialog();
		return;
	}
	if (p_op == "export_obj") {
		obj_dialog->set_current_file(String(n->get_name()) + ".obj");
		obj_dialog->popup_file_dialog();
		return;
	}
	if (p_op == "uv_editor") {
		uv_dock->make_visible();
		uv_canvas->frame_selection();
		return;
	}

	// ---- mesh edits (snapshot undo)
	const Mode op_mode = mode;
	begin_edit(p_op == "translate" ? String("Move") : p_op.capitalize());
	bool geometry = true;
	auto P = [&](const char *key) { return op_param(p_op, key); };
	const Transform3D g = n->get_global_transform();
	const Transform3D ginv = g.affine_inverse();
	// Moves the selected vertices by a world-space function.
	auto move_selected = [&](const std::function<Vector3(const Vector3 &)> &fn) {
		for (int v : _selected_verts()) {
			m.pos[v] = ginv.xform(fn(g.xform(m.pos[v])));
		}
	};
	// Extrude of the selection in the current mode (faces, edges, or border edges between vertices).
	auto extrude_selection = [&](float dist, bool individual) {
		const int nf0_extrude = (int)m.faces.size();
		if (mode == MODE_FACE) {
			hewn::ops::extrude_faces(m, s.faces, dist, individual);
		} else {
			_extrude_edges_verts(m, s, dist);
		}
		if ((bool)P("flip_normals")) {
			std::vector<int> fl;
			for (int f = nf0_extrude; f < (int)m.faces.size(); f++) {
				fl.push_back(f);
			}
			if (mode == MODE_FACE) {
				fl.insert(fl.end(), s.faces.begin(), s.faces.end());
			}
			hewn::ops::flip_faces(m, hewn::sel::uniq(fl));
		}
	};
	// "dissolve orthogonal edges": side faces that continue a neighbor's plane are merged into it.
	auto dissolve_orthogonal = [&](int first_new_face) {
		hewn::ops::dissolve_orthogonal(m, first_new_face, s.faces);
	};

	if (p_op == "spin") {
		// Spin: the selection extruded step by step around an axis (lathe, arches, rings)
		const int steps = MAX(1, (int)P("steps"));
		const float total = Math::deg_to_rad((float)P("angle"));
		const Vector3 c = ginv.xform(Vector3((float)P("cx"), (float)P("cy"), (float)P("cz")));
		Vector3 axis = ginv.basis.xform(Vector3((float)P("ax"), (float)P("ay"), (float)P("az")));
		if (axis.length() < 1e-6f) {
			axis = Vector3(0, 1, 0);
		}
		axis.normalize();
		const Basis turn(axis, total / (float)steps);
		const bool full = std::fabs(std::fabs(total) - (float)(Math::PI * 2.0)) < 1e-3f;
		// (UV Along the Turn: the image as it is at the start, bent with the turn; each new vertex's first one and step)
		const bool uv_along = (bool)P("uv_along");
		const int first_new = (int)m.faces.size();
		std::vector<std::vector<Vector2>> old_uvs;
		std::map<int, std::pair<int, int>> ring;
		float step_length = 0.0f;
		if (uv_along) {
			hewn::compute_uvs(m, n->uv_transform(), old_uvs);
			const std::vector<int> first = _selected_verts();
			for (int v : first) {
				ring[v] = { v, 0 };
				const Vector3 d = m.pos[v] - c;
				step_length += (d - axis * d.dot(axis)).length();
			}
			step_length *= std::fabs(total) / (float)steps / MAX(1.0f, (float)first.size());
		}
		for (int k = 0; k < steps; k++) {
			std::map<std::array<float, 3>, int> at_start;
			if (uv_along) {
				for (int v : _selected_verts()) {
					if (ring.count(v)) {
						at_start[{ m.pos[v].x, m.pos[v].y, m.pos[v].z }] = ring[v].first;
					}
				}
			}
			extrude_selection(0.0f, false);
			for (int v : _selected_verts()) {
				if (uv_along && !ring.count(v)) {
					const auto found = at_start.find({ m.pos[v].x, m.pos[v].y, m.pos[v].z });
					if (found != at_start.end()) {
						ring[v] = { found->second, k + 1 };
					}
				}
				m.pos[v] = c + turn.xform(m.pos[v] - c);
			}
		}
		if (uv_along) {
			_spin_bend_uvs(m, old_uvs, first_new, ring, step_length);
		}
		if (full) {
			// round to the start: the last ring is the first one (and faces there would be inside)
			if (mode == MODE_FACE) {
				hewn::ops::delete_faces(m, s.faces);
				s.clear();
			}
			hewn::weld_by_distance(m, 1e-4f);
			s.clear();
		}
	} else if (p_op == "extrude") {
		const int nf0 = (int)m.faces.size();
		extrude_selection((float)P("distance"), (bool)P("individual") && mode == MODE_FACE);
		if ((bool)P("dissolve_orthogonal") && mode == MODE_FACE) {
			dissolve_orthogonal(nf0);
		}
	} else if (p_op == "extrude_repeat") {
		// the selection extruded again and again, each step moved along its normal and scaled about its center
		const int steps = MAX(1, (int)P("steps"));
		const float dist = (float)P("distance");
		const float sc = (float)P("scale");
		const std::vector<int> nf = mode == MODE_FACE ? s.faces : hewn::sel::faces_from_verts(m, _selected_verts(), false);
		Vector3 nrm;
		for (int f : nf) {
			if (f >= 0 && f < (int)m.faces.size()) {
				nrm += hewn::face_normal_raw(m, m.faces[f]);
			}
		}
		nrm = g.basis.inverse().transposed().xform(nrm);
		nrm = nrm.length() > 1e-9f ? nrm.normalized() : Vector3(0, 1, 0);
		for (int k = 0; k < steps; k++) {
			extrude_selection(0.0f, false);
			const std::vector<int> vs = _selected_verts();
			Vector3 c;
			for (int v : vs) {
				c += g.xform(m.pos[v]);
			}
			c = vs.empty() ? c : c / (float)vs.size();
			for (int v : vs) {
				const Vector3 p = g.xform(m.pos[v]);
				m.pos[v] = ginv.xform(c + (p - c) * sc + nrm * dist);
			}
		}
	} else if (p_op == "extrude_block") {
		// a closed block of its own on the selected faces, a step long:
		// its bottom on them, its own vertices - moving it leaves the mesh it came from as it is
		const Vector3 d = g.basis.inverse().xform(Vector3((float)P("x"), (float)P("y"), (float)P("z")));
		std::vector<int> region;
		for (int f : s.faces) {
			if (f >= 0 && f < (int)m.faces.size()) {
				region.push_back(f);
			}
		}
		if (!region.empty() && d.length() > 1e-6f) {
			const std::vector<int> new_top = hw_make_block(m, region, d);
			s.clear();
			for (int k = 0; k < (int)new_top.size(); k++) {
				s.faces.push_back(new_top[k]);
			}
		}
	} else if (p_op == "bisect") {
		std::vector<int> faces = mode == MODE_FACE ? s.faces : hewn::sel::faces_from_verts(m, _selected_verts(), true);
		const Vector3 wp((float)P("px"), (float)P("py"), (float)P("pz"));
		const Vector3 wn((float)P("nx"), (float)P("ny"), (float)P("nz"));
		// Mode: split only; split and fill (two closed parts, each with a face over the cut); the part above (the
		// normal's side) / below removed, the hole filled or left open
		const int bmode = (int)P("mode");
		const Vector3 lp = ginv.xform(wp), ln = g.basis.transposed().xform(wn);
		std::vector<hewn::Edge> cut;
		bisect_sep_faces.clear();
		if (bmode == 1) {
			// (both parts closed: the hole filled on each side; the one below separated after, if asked)
			hewn::Mesh below = m;
			std::vector<hewn::Edge> c1 = hewn::ops::bisect(m, faces, lp, ln, false, true, true);
			std::vector<hewn::Edge> c2 = hewn::ops::bisect(below, faces, lp, ln, true, false, true);
			const int base = (int)m.pos.size();
			const int fbase = (int)m.faces.size();
			hewn::ops::append(m, below, Transform3D(), 0);
			if ((bool)P("separate") && fbase > 0 && (int)m.faces.size() > fbase) {
				for (int f = fbase; f < (int)m.faces.size(); f++) {
					bisect_sep_faces.push_back(f);
				}
			}
			cut = c1;
			for (const hewn::Edge &e : c2) {
				cut.push_back(hewn::Edge(e.a + base, e.b + base));
			}
		} else {
			cut = hewn::ops::bisect(m, faces, lp, ln, bmode == 3, bmode == 2, bmode >= 2 && (bool)P("fill"));
		}
		s.clear();
		// (a side cleared: its corners go too, none of the faces left uses them)
		s.edges = cut;
		hewn::remove_unused_vertices(m, &s);
		cut = s.edges;
		s.clear();
		if (mode == MODE_VERTEX) {
			s.verts = hewn::sel::verts_of_edges(cut);
		} else {
			s.edges = cut;
			if (mode == MODE_FACE) {
				mode = element_mode = MODE_EDGE;
			}
		}
	} else if (p_op == "extrude_normals") {
		s.faces = hewn::ops::extrude_along_normals(m, s.faces, (float)P("distance"));
	} else if (p_op == "extrude_move" || p_op == "duplicate_move" || p_op == "translate" || p_op == "rotate" || p_op == "scale") {
		const int nf0 = (int)m.faces.size();
		if (p_op == "extrude_move") {
			extrude_selection(0.0f, false);
		} else if (p_op == "duplicate_move") {
			s.faces = hewn::ops::duplicate_faces(m, s.faces);
		}
		// Selection plus proportional-editing neighbors, each with its weight and pivot.
		const std::vector<int> sel = _selected_verts();
		const bool xf_op = p_op == "translate" || p_op == "rotate" || p_op == "scale";
		std::vector<int> verts;
		std::vector<float> w;
		// (Projected (2D): along the view the operation was made in)
		const Vector3 view((float)P("prop_vx"), (float)P("prop_vy"), (float)P("prop_vz"));
		prop_proj_axis = (bool)P("prop_projected") && view.length() > 0.5f ? view.normalized() : Vector3();
		_weighted_verts(sel, xf_op && (bool)P("prop"), (float)P("prop_size"), (int)P("prop_falloff"), (bool)P("prop_connected"), verts, w);
		const Vector3 piv((float)P("px"), (float)P("py"), (float)P("pz"));
		std::vector<Vector3> pivots(verts.size(), piv);
		if (xf_op && (int)P("pivot_mode") == HewnDock::PIVOT_INDIVIDUAL) {
			const std::vector<Vector3> ind = _individual_pivots(sel, piv);
			for (size_t i = 0; i < ind.size(); i++) {
				pivots[i] = ind[i];
			}
		}
		const Vector3 d((float)P("x"), (float)P("y"), (float)P("z"));
		Vector3 axis((float)P("ax"), (float)P("ay"), (float)P("az"));
		if (axis.length() < 1e-6f) {
			axis = Vector3(0, 1, 0);
		}
		axis.normalize();
		const float f = (float)P("factor");
		const Vector3 sd = Vector3((float)P("dx"), (float)P("dy"), (float)P("dz")).normalized();
		const int c = (int)P("constraint");
		std::vector<Vector3> before;
		for (int v : verts) {
			before.push_back(m.pos[v]);
		}
		for (size_t i = 0; i < verts.size(); i++) {
			const Vector3 p = g.xform(m.pos[verts[i]]);
			Vector3 q = p;
			if (p_op == "rotate") {
				q = pivots[i] + Basis(axis, (float)P("angle") * w[i] * (float)Math::PI / 180.0f).xform(p - pivots[i]);
			} else if (p_op == "scale") {
				const float fi = 1.0f + (f - 1.0f) * w[i];
				const Vector3 r = p - pivots[i];
				q = c == 1 ? pivots[i] + r + sd * (r.dot(sd) * (fi - 1.0f)) : c == 2 ? pivots[i] + r * fi - sd * (r.dot(sd) * (fi - 1.0f)) : pivots[i] + r * fi;
			} else {
				q = p + d * w[i];
			}
			m.pos[verts[i]] = ginv.xform(q);
		}
		n->apply_mirror_clipping(verts, before); // Mirror modifier with Clipping
		if (xf_op) {
			// symmetric editing (the toolbar's Symmetry when it was made): the twins mirrored
			const int ax = ((bool)P("sym_x") ? 1 : 0) | ((bool)P("sym_y") ? 2 : 0) | ((bool)P("sym_z") ? 4 : 0);
			if (ax) {
				hewn::ops::apply_symmetry(m, verts, hewn::ops::symmetry_links(m, verts, before, &w, ax));
			}
		}
		if (p_op == "extrude_move" && (bool)P("dissolve_orthogonal") && mode == MODE_FACE) {
			dissolve_orthogonal(nf0);
		}
		_auto_merge(m, sel);
	} else if (p_op == "inset") {
		const float amount = (float)P("amount");
		if (amount > 0.0f || (float)P("depth") != 0.0f) {
			s.faces = hewn::ops::inset_faces_ex(m, s.faces, _inset_options(amount));
			s.verts = hewn::sel::verts_of_faces(m, s.faces);
		}
	} else if (p_op == "loop_cut") {
		const hewn::Edge e((int)P("a"), (int)P("b"));
		s.clear();
		s.edges = hewn::ops::loop_cut(m, e, (int)P("cuts"), ((float)P("factor") + 1.0f) * 0.5f);
		// the modes: faces off (the new loops are edges), vertices / edges kept; none left: edges
		int mm = mode_mask & ~(1 << MODE_FACE);
		if (mm == 0) {
			mm = 1 << MODE_EDGE;
		}
		mode_mask = mm;
		mode = element_mode = (mm & (1 << MODE_EDGE)) ? MODE_EDGE : MODE_VERTEX;
		if (mm & (1 << MODE_VERTEX)) {
			s.verts = hewn::sel::verts_of_edges(s.edges);
		}
	} else if (p_op == "set_vertex_color") {
		// the selected vertices' color in a channel; in face mode, only on the side of the selected faces (the
		// neighbor faces keep theirs at the same vertices: a sharp color edge)
		geometry = false;
		const Color c = P("color");
		const int ch = CLAMP((int)P("channel"), 0, 4);
		const bool faces_only = (bool)P("faces_only") && has_mode(MODE_FACE) && !s.faces.empty();
		if (ch > 0 && n->custom_formats[ch - 1] < 0) {
			n->custom_formats[ch - 1] = dock && dock->paint_format ? dock->paint_format->get_selected() : 0;
		}
		std::unordered_set<int> vs;
		for (int v : _selected_verts()) {
			vs.insert(v);
		}
		std::unordered_set<int> fs(s.faces.begin(), s.faces.end());
		for (int f = 0; f < (int)m.faces.size(); f++) {
			hewn::Face &F = m.faces[f];
			if (faces_only && !fs.count(f)) {
				continue;
			}
			for (int i = 0; i < F.size(); i++) {
				if (vs.count(F.v[i])) {
					_paint_value(F, i, ch) = c;
				}
			}
		}
	} else if (p_op == "limited_dissolve") {
		hewn::ops::limited_dissolve(m, mode == MODE_FACE ? s.faces : std::vector<int>(), (float)P("angle"));
		s.clear();
	} else if (p_op == "dissolve") {
		if (mode == MODE_FACE) {
			s.faces = hewn::ops::merge_faces(m, s.faces);
		} else if (mode == MODE_EDGE) {
			s.faces = hewn::ops::dissolve_edges(m, s.edges);
			s.edges.clear();
		} else {
			hewn::ops::dissolve_vertices(m, s.verts);
			s.verts.clear();
		}
	} else if (p_op == "bevel_vertices" || p_op == "offset_edge_slide" || p_op == "push_pull" || p_op == "shrink_fatten" || p_op == "shear") {
		_value_op_apply(p_op, (float)P("value"));
	} else if (p_op == "poke") {
		s.faces = hewn::ops::poke_faces(m, s.faces);
	} else if (p_op == "bridge_faces") {
		hewn::ops::BridgeShape sh = _bridge_shape(p_op);
		s.faces = hewn::ops::bridge_faces(m, s.faces, &sh);
		_bridge_shape_made(sh, g);
	} else if (p_op == "tris_to_quads") {
		std::vector<int> faces = mode == MODE_FACE ? s.faces : hewn::sel::faces_from_verts(m, _selected_verts(), true);
		hewn::ops::quadify(m, 40.0f, faces.empty() ? nullptr : &faces);
		s.clear();
	} else if (p_op == "split_edges") {
		hewn::ops::split_edges(m, mode == MODE_EDGE ? s.edges : hewn::sel::edges_from_verts(m, t, _selected_verts()));
	} else if (p_op == "mark_seam" || p_op == "clear_seam" || p_op == "mark_sharp" || p_op == "clear_sharp" || p_op == "edge_crease" || p_op == "bevel_weight" ||
			p_op == "sharp_by_angle") {
		std::vector<hewn::Edge> edges = mode == MODE_EDGE ? s.edges : hewn::sel::edges_from_verts(m, t, _selected_verts());
		if ((p_op == "mark_sharp" || p_op == "clear_sharp" || p_op == "mark_seam" || p_op == "clear_seam") && (bool)P("vertices")) {
			// Vertices: every edge touching a selected vertex, not only the ones between them
			std::unordered_set<int> vs;
			for (int v : _selected_verts()) {
				vs.insert(v);
			}
			edges.clear();
			for (const hewn::Edge &e : t.edges) {
				if (vs.count(e.a) || vs.count(e.b)) {
					edges.push_back(e);
				}
			}
		}
		const float cos_lim = std::cos((float)P("angle") * (float)Math::PI / 180.0f);
		const bool extend = p_op == "sharp_by_angle" && (bool)P("extend");
		for (const hewn::Edge &e : edges) {
			hewn::EdgeAttr &ea = m.edge_attrs[hewn::ekey(e)];
			if (p_op == "mark_seam" || p_op == "clear_seam") {
				ea.seam = p_op == "mark_seam";
			} else if (p_op == "mark_sharp" || p_op == "clear_sharp") {
				ea.sharp = p_op == "mark_sharp";
			} else if (p_op == "edge_crease") {
				ea.crease = CLAMP((float)P("value"), 0.0f, 1.0f);
			} else if (p_op == "bevel_weight") {
				ea.bevel_weight = CLAMP((float)P("value"), 0.0f, 1.0f);
			} else {
				const int id = t.find_edge(e.a, e.b);
				ea.sharp = (extend && ea.sharp) || (id >= 0 && t.edge_faces[id].size() == 2 &&
						hewn::face_normal(m, m.faces[t.edge_faces[id][0]]).dot(hewn::face_normal(m, m.faces[t.edge_faces[id][1]])) < cos_lim);
			}
		}
		geometry = false;
	} else if (is_unwrap_op(p_op)) {
		_unwrap_op(p_op, n);
		geometry = false;
	} else if (p_op == "vertex_crease") {
		for (int v : _selected_verts()) {
			m.vert_crease[v] = CLAMP((float)P("value"), 0.0f, 1.0f);
		}
		geometry = false;
	} else if (p_op == "thicken") {
		static const float offsets[3] = { -1.0f, 0.0f, 1.0f };
		s.faces = hewn::ops::thicken_faces(m, s.faces, (float)P("thickness"), offsets[CLAMP((int)P("side"), 0, 2)]);
	} else if (p_op == "drop_down" || p_op == "drop_up") {
		// each selected vertex straight down (up) onto the nearest face under (over) it: of the other objects, or of
		// this mesh where no selected vertex is
		const Vector3 dir = p_op == "drop_up" ? Vector3(0, 1, 0) : Vector3(0, -1, 0);
		const std::vector<int> sv = _selected_verts();
		std::unordered_set<int> svs(sv.begin(), sv.end());
		std::vector<Vector3> own; // (this mesh's other faces, as world triangles)
		for (const hewn::Face &F : m.faces) {
			bool touches = false;
			for (int v : F.v) {
				touches = touches || svs.count(v);
			}
			if (touches || F.hidden || F.size() < 3) {
				continue;
			}
			std::vector<Vector3> pts;
			for (int v : F.v) {
				pts.push_back(m.pos[v]);
			}
			std::vector<int> tris;
			hewn::triangulate(pts, hewn::face_normal(m, F), tris);
			for (int k : tris) {
				own.push_back(g.xform(pts[k]));
			}
		}
		const std::vector<Node *> except = { n };
		const float off = (float)P("offset");
		for (int v : sv) {
			const Vector3 w = g.xform(m.pos[v]);
			float best = 1e30f, t;
			Vector3 nrm;
			if (_ray_scene_except(w, dir, except, t, nrm) && t > 1e-5f) {
				best = t;
			}
			for (size_t k = 0; k + 2 < own.size(); k += 3) {
				if (hw_editor::ray_tri(w, dir, own[k], own[k + 1], own[k + 2], t) && t > 1e-5f && t < best) {
					best = t;
				}
			}
			if (best < 1e29f) {
				m.pos[v] = ginv.xform(w + dir * MAX(0.0f, best - off));
			}
		}
	} else if (p_op == "to_circle") {
		hewn::ops::to_circle(m, _selected_verts(), mode == MODE_FACE ? s.faces : std::vector<int>(), (bool)P("regular"), (float)P("influence") / 100.0f);
	} else if (p_op == "randomize") {
		Ref<RandomNumberGenerator> rng;
		rng.instantiate();
		rng->set_seed((uint64_t)(int64_t)P("seed") * 7919u + 17u);
		const float amount = (float)P("amount");
		for (int v : _selected_verts()) {
			const Vector3 d(rng->randf_range(-1, 1), rng->randf_range(-1, 1), rng->randf_range(-1, 1));
			m.pos[v] = ginv.xform(g.xform(m.pos[v]) + d * amount);
		}
	} else if (p_op.begins_with("mirror_")) {
		// mirror the selection around the pivot, along a global or local axis
		const bool local = p_op.length() == 9;
		const int axis = p_op[p_op.length() - 1] - 'x';
		Vector3 ax = local ? g.basis.get_column(axis).normalized() : AXES[axis];
		const std::vector<int> sel = _selected_verts();
		const Vector3 piv = _pivot_point(dock->pivot_mode, sel);
		for (int v : sel) {
			const Vector3 w = g.xform(m.pos[v]);
			m.pos[v] = ginv.xform(w - ax * (2.0f * (w - piv).dot(ax)));
		}
		std::unordered_set<int> vs(sel.begin(), sel.end());
		for (hewn::Face &F : m.faces) {
			bool all = true;
			for (int v : F.v) {
				all = all && vs.count(v);
			}
			if (all) {
				F.reverse(); // mirrored faces keep facing outwards
			}
		}
	} else if (p_op.begins_with("merge_")) {
		std::vector<int> sel = mode == MODE_VERTEX ? s.verts : _selected_verts();
		if (sel.size() >= 2) {
			if (p_op == "merge_collapse") {
				std::vector<std::vector<int>> groups;
				hewn::Topology tt;
				tt.build(m);
				std::unordered_set<int> vs(sel.begin(), sel.end());
				std::unordered_map<int, int> comp;
				for (int v : sel) {
					if (comp.count(v)) {
						continue;
					}
					groups.push_back({});
					std::vector<int> stack = { v };
					comp[v] = (int)groups.size() - 1;
					while (!stack.empty()) {
						const int x = stack.back();
						stack.pop_back();
						groups.back().push_back(x);
						for (int e : tt.vert_edges[x]) {
							const int o = tt.edges[e].a == x ? tt.edges[e].b : tt.edges[e].a;
							if (vs.count(o) && !comp.count(o)) {
								comp[o] = (int)groups.size() - 1;
								stack.push_back(o);
							}
						}
					}
				}
				s.verts = hewn::ops::collapse_groups(m, groups);
			} else {
				Vector3 target;
				if (p_op == "merge_cursor") {
					target = ginv.xform(cursor_pos);
				} else if (p_op == "merge_first") {
					target = m.pos[sel.front()];
				} else if (p_op == "merge_last") {
					target = m.pos[sel.back()];
				} else {
					for (int v : sel) {
						target += m.pos[v];
					}
					target /= (float)sel.size();
				}
				for (int v : sel) {
					m.pos[v] = target;
				}
				s.verts = hewn::ops::collapse_groups(m, { sel });
			}
			s.edges.clear();
			s.faces.clear();
		}
	} else if (p_op == "delete_verts" || p_op == "delete_edges" || p_op == "delete_faces" || p_op == "delete_only_edges_faces" || p_op == "delete_only_faces") {
		const hewn::ops::DeleteKind kind = p_op == "delete_verts" ? hewn::ops::DEL_VERTS : p_op == "delete_edges" ? hewn::ops::DEL_EDGES
				: p_op == "delete_faces"																	 ? hewn::ops::DEL_FACES
				: p_op == "delete_only_faces"																 ? hewn::ops::DEL_ONLY_FACES
																											 : hewn::ops::DEL_ONLY_EDGES_FACES;
		_delete_selected(m, kind);
		s.clear();
	} else if (p_op == "dissolve_verts") {
		hewn::ops::dissolve_vertices(m, _selected_verts());
		s.clear();
	} else if (p_op == "dissolve_edges") {
		s.faces = hewn::ops::dissolve_edges(m, mode == MODE_EDGE ? s.edges : hewn::sel::edges_from_verts(m, t, _selected_verts()));
		s.edges.clear();
	} else if (p_op == "dissolve_faces") {
		s.faces = hewn::ops::merge_faces(m, mode == MODE_FACE ? s.faces : hewn::sel::faces_from_verts(m, _selected_verts(), true));
	} else if (p_op == "collapse_edges_faces") {
		set_op_param("merge_collapse", "x", 0); // (no parameters)
		const std::vector<int> sel = _selected_verts();
		std::vector<std::vector<int>> groups;
		hewn::Topology tt;
		tt.build(m);
		std::unordered_set<int> vs(sel.begin(), sel.end());
		std::unordered_set<int> seen;
		for (int v : sel) {
			if (seen.count(v)) {
				continue;
			}
			groups.push_back({});
			std::vector<int> stack = { v };
			seen.insert(v);
			while (!stack.empty()) {
				const int x = stack.back();
				stack.pop_back();
				groups.back().push_back(x);
				for (int e : tt.vert_edges[x]) {
					const int o = tt.edges[e].a == x ? tt.edges[e].b : tt.edges[e].a;
					if (vs.count(o) && !seen.count(o)) {
						seen.insert(o);
						stack.push_back(o);
					}
				}
			}
		}
		s.verts = hewn::ops::collapse_groups(m, groups);
		s.edges.clear();
		s.faces.clear();
	} else if (p_op == "delete_edge_loops") {
		hewn::ops::delete_edge_loops(m, mode == MODE_EDGE ? s.edges : hewn::sel::edges_from_verts(m, t, _selected_verts()));
		s.clear();
	} else if (p_op == "delete") {
		_delete_selected(m, mode == MODE_FACE ? hewn::ops::DEL_FACES : mode == MODE_EDGE ? hewn::ops::DEL_EDGES : hewn::ops::DEL_VERTS);
		s.clear();
	} else if (p_op == "double_sided") {
		// the selected faces seen from both sides (all of them already: back to one side)
		std::vector<int> faces = mode == MODE_FACE ? s.faces : hewn::sel::faces_from_verts(m, _selected_verts(), true);
		if (faces.empty()) {
			faces = hewn::sel::all_faces(m);
		}
		bool all = true;
		for (int f : faces) {
			all = all && m.faces[f].double_sided;
		}
		for (int f : faces) {
			m.faces[f].double_sided = !all;
		}
	} else if (p_op == "flip" || p_op == "recalc_normals") {
		// Selected faces (any mode); nothing selected means the whole mesh.
		std::vector<int> faces = mode == MODE_FACE ? s.faces : hewn::sel::faces_from_verts(m, _selected_verts(), true);
		if (faces.empty()) {
			faces = hewn::sel::all_faces(m);
		}
		if (p_op == "flip") {
			hewn::ops::flip_faces(m, faces);
		} else {
			hewn::ops::recalc_normals(m, faces, (bool)op_param("recalc_normals", "inside"));
		}
	} else if (p_op == "make_face") {
		const std::vector<int> fv = mode == MODE_EDGE ? hewn::sel::verts_of_edges(s.edges) : hewn::sel::uniq(_selected_verts());
		f2_new_vertex = -1;
		int f2 = -1;
		hewn::Edge next(-1, -1);
		// F2 picks by the mouse: the view in the mesh's space
		hewn::ops::F2View view;
		if (Camera3D *cam = _viewport_camera()) {
			const Transform3D G = n->get_global_transform();
			const Transform3D Gi = G.affine_inverse();
			view.to_screen = [cam, G](const Vector3 &p) { return cam->unproject_position(G.xform(p)); };
			view.mouse = mouse_pos; // (where the mouse was over the viewport last)
			view.eye = Gi.xform(cam->get_global_position());
			view.has_eye = true;
		}
		if (fv.size() == 1 && mode == MODE_VERTEX) {
			// F2: a border vertex -> a new vertex and a quad (moved right away, as F2's autograb)
			int q = -1;
			f2 = hewn::ops::f2_fill_vertex(m, fv[0], view, &q);
			if (f2 >= 0) {
				s.verts.clear();
				if (q >= 0) {
					s.verts.push_back(q);
					f2_new_vertex = q;
				} else {
					s.verts = m.faces[f2].v;
				}
			}
		} else if (fv.size() == 2) {
			// F2: one open edge -> a quad along the open edges at its ends; the edge across selected (F goes on)
			f2 = hewn::ops::f2_fill_edge(m, fv[0], fv[1], view, &next);
		}
		if (f2 >= 0 && fv.size() == 2) {
			s.edges.clear();
			s.verts.clear();
			if (next.a >= 0) {
				s.edges.push_back(next);
				s.verts = { next.a, next.b };
			}
		}
		if (f2 >= 0) {
			// (done)
		} else if (fv.size() == 2) {
			// F on two vertices joins them with an edge
			if (!hewn::ops::add_wire(m, fv[0], fv[1])) {
				editing = false;
				return;
			}
			if (has_mode(MODE_EDGE)) {
				s.edges.push_back(hewn::Edge(fv[0], fv[1]));
			}
		} else {
			const int f = hewn::ops::make_face_from(m, fv);
			if (f < 0) {
				editing = false;
				return;
			}
			if (mode == MODE_EDGE) {
				s.edges = hewn::sel::edges_of_faces(m, { f });
			}
		}
	} else if (p_op == "subdivide_faces") {
		s.faces = hewn::ops::subdivide_faces_ex(m, s.faces, (int)P("cuts"), (float)P("smoothness"), (bool)P("ngons"), (float)P("fractal"), (float)P("along_normal"), (int)P("seed"));
	} else if (p_op == "triangulate") {
		s.faces = hewn::ops::triangulate_faces(m, s.faces);
	} else if (p_op == "merge_faces") {
		s.faces = hewn::ops::merge_faces(m, s.faces);
	} else if (p_op == "detach") {
		hewn::ops::detach_faces(m, s.faces);
	} else if (p_op == "duplicate") {
		if (mode == MODE_FACE) {
			s.faces = hewn::ops::duplicate_faces(m, s.faces);
		} else {
			const std::vector<hewn::Edge> edges = s.edges;
			s = hewn::ops::duplicate_geometry(m, mode == MODE_EDGE ? hewn::sel::verts_of_edges(edges) : s.verts, mode == MODE_EDGE ? &edges : nullptr);
		}
	} else if (p_op == "conform") {
		hewn::ops::conform_normals(m, s.faces);
	} else if (p_op == "smooth_faces" || p_op == "flat_faces") {
		int g = 0;
		if (p_op == "smooth_faces") {
			for (const hewn::Face &F : m.faces) {
				g = MAX(g, F.smooth);
			}
			g++;
		}
		// (vertex / edge mode: the faces whose corners are all selected, else the faces around the selection)
		std::vector<int> fs = s.faces;
		if (mode != MODE_FACE) {
			const std::vector<int> vs = mode == MODE_EDGE ? hewn::sel::verts_of_edges(s.edges) : s.verts;
			fs = hewn::sel::faces_from_verts(m, vs, true);
			if (fs.empty()) {
				fs = hewn::sel::faces_from_verts(m, vs, false);
			}
		}
		if (p_op == "smooth_faces") {
			// smooth next to faces already smooth: one group with them (no hard edge between the two), the groups
			// it touches joined
			std::unordered_set<int> sel_f(fs.begin(), fs.end());
			std::unordered_map<uint64_t, std::vector<int>> by_edge;
			for (int f = 0; f < (int)m.faces.size(); f++) {
				const hewn::Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					by_edge[hewn::ekey(F.v[i], F.v[(i + 1) % F.size()])].push_back(f);
				}
			}
			std::set<int> touched;
			for (int f : fs) {
				if (f < 0 || f >= (int)m.faces.size()) {
					continue;
				}
				const hewn::Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					for (int o : by_edge[hewn::ekey(F.v[i], F.v[(i + 1) % F.size()])]) {
						if (!sel_f.count(o) && m.faces[o].smooth > 0) {
							touched.insert(m.faces[o].smooth);
						}
					}
				}
			}
			if (!touched.empty()) {
				g = *touched.begin();
				for (hewn::Face &F : m.faces) {
					if (touched.count(F.smooth)) {
						F.smooth = g;
					}
				}
			}
		}
		for (int f : fs) {
			if (f >= 0 && f < (int)m.faces.size()) {
				m.faces[f].smooth = g;
			}
		}
		geometry = false;
	} else if (p_op == "bevel") {
		std::vector<hewn::Edge> edges = mode == MODE_EDGE ? s.edges : std::vector<hewn::Edge>();
		if (mode == MODE_FACE) {
			// Boundary of the selected region (edges with exactly one selected face).
			std::unordered_set<int> fs(s.faces.begin(), s.faces.end());
			for (size_t e = 0; e < t.edges.size(); e++) {
				int count = 0;
				for (int f : t.edge_faces[e]) {
					count += fs.count(f) ? 1 : 0;
				}
				if (count == 1 && t.edge_faces[e].size() == 2) {
					edges.push_back(t.edges[e]);
				}
			}
		} else if (mode == MODE_VERTEX) {
			edges = hewn::sel::edges_from_verts(m, t, s.verts);
		}
		// the width width types: along the faces (offset), across the bevel (width), in from the
		// edge (depth) or a share of the edges it slides along (percent); the angle is the mean of the edges'
		const int wtype = (int)P("width_type");
		float amount = (float)P("width");
		if (wtype == 1 || wtype == 2) {
			float half = 0.0f;
			int count = 0;
			for (const hewn::Edge &e : edges) {
				const int ei = t.find_edge(e.a, e.b);
				if (ei >= 0 && t.edge_faces[ei].size() == 2) {
					const Vector3 n0 = hewn::face_normal(m, m.faces[t.edge_faces[ei][0]]), n1 = hewn::face_normal(m, m.faces[t.edge_faces[ei][1]]);
					half += 0.5f * ((float)Math::PI - std::acos(CLAMP(n0.dot(n1), -1.0f, 1.0f)));
					count++;
				}
			}
			half = count ? half / count : (float)Math::PI * 0.25f;
			amount = wtype == 1 ? amount / MAX(1e-4f, 2.0f * std::sin(half)) : amount / MAX(1e-4f, std::cos(half));
		}
		if ((int)P("affect") == 0) {
			// Affect: Vertices (the corners cut off)
			std::vector<int> vs = mode == MODE_VERTEX ? s.verts : _selected_verts();
			s.faces = hewn::ops::bevel_vertices(m, vs, amount);
			hewn::continue_uvs(m, s.faces, g); // (the texture runs on over the new faces)
			s.verts.clear();
		} else {
			const std::vector<Vector2> prof = (int)P("profile_type") == 1 ? _bevel_profile()->to_vector() : std::vector<Vector2>();
			hewn::ops::ProfileFrame bfr;
			s.faces = hewn::ops::bevel_edges(m, edges, amount, (int)P("segments"), false, prof.empty() ? nullptr : &prof, wtype == 3, &bfr);
			if (!prof.empty()) {
				_op_profile_made("bevel", bfr, g);
			}
			hewn::continue_uvs(m, s.faces, g); // (the texture of the faces around runs on over the bevel, no jumps)
			if ((bool)P("harden_normals")) {
				hewn::ops::harden_normals(m, s.faces);
			}
		}
		s.edges.clear();
		if (!s.faces.empty() && mode != MODE_FACE) {
			mode = element_mode = MODE_FACE;
		}
	} else if (p_op == "arch") {
		const std::vector<Vector2> curve = (int)P("shape") == 1 ? _arch_profile()->to_vector() : std::vector<Vector2>();
		hewn::ops::ProfileFrame fr;
		s.edges = hewn::ops::arch_edges(m, s.edges, (int)P("segments"), (float)P("rise"), curve.empty() ? nullptr : &curve, &fr);
		_op_profile_made("arch", fr, g);
		s.faces.clear();
		s.verts.clear();
	} else if (p_op == "connect_edges") {
		s.edges = hewn::ops::connect_edges(m, s.edges);
	} else if (p_op == "insert_loop") {
		s.edges = hewn::ops::insert_edge_loops(m, s.edges);
	} else if (p_op == "subdivide_edges") {
		s.edges = hewn::ops::subdivide_edges(m, mode == MODE_EDGE ? s.edges : hewn::sel::edges_from_verts(m, t, _selected_verts()), (int)P("cuts"), (bool)P("ngons"));
	} else if (p_op == "bridge") {
		// two loops or chains of edges (two single edges: one face between them)
		hewn::ops::BridgeShape sh = _bridge_shape(p_op);
		s.faces = hewn::ops::bridge_edge_loops(m, s.edges, &sh);
		_bridge_shape_made(sh, g);
		if (s.faces.empty() && s.edges.size() == 2) {
			s.faces = hewn::ops::bridge_edges(m, s.edges[0], s.edges[1]);
		}
	} else if (p_op == "dissolve") {
		if (mode == MODE_EDGE) {
			s.faces = hewn::ops::dissolve_edges(m, s.edges);
			s.edges.clear();
		} else if (mode == MODE_VERTEX) {
			hewn::ops::dissolve_vertices(m, s.verts);
			s.verts.clear();
		}
	} else if (p_op == "flip_edges") {
		hewn::ops::flip_edges(m, s.edges);
		s.edges.clear();
	} else if (p_op == "weld") {
		s.verts = hewn::ops::weld_vertices(m, s.verts, (float)P("distance"));
	} else if (p_op == "collapse") {
		s.verts = hewn::ops::collapse_groups(m, { _selected_verts() });
		if (mode != MODE_VERTEX) {
			s.clear();
		}
	} else if (p_op == "split") {
		// the selected faces (in vertex / edge mode: those wholly selected) cut loose from the rest, in the same mesh;
		// no such face: the vertices split apart
		const std::vector<int> vs = mode == MODE_EDGE ? hewn::sel::verts_of_edges(s.edges) : s.verts;
		const std::vector<int> fs = mode == MODE_FACE ? s.faces : hewn::sel::faces_from_verts(m, vs, true);
		if (!fs.empty()) {
			hewn::ops::detach_faces(m, fs);
			s.clear();
			s.faces = fs;
			if (mode != MODE_FACE) {
				for (int f : fs) {
					for (int v : m.faces[f].v) {
						s.verts.push_back(v);
					}
				}
				s.verts = hewn::sel::uniq(s.verts);
				if (mode == MODE_EDGE) {
					for (int f : fs) {
						const int n = m.faces[f].size();
						for (int i = 0; i < n; i++) {
							s.edges.push_back(hewn::Edge(m.faces[f].v[i], m.faces[f].v[(i + 1) % n]));
						}
					}
					s.edges = hewn::sel::uniq(s.edges);
				}
			}
		} else {
			s.verts = hewn::ops::split_vertices(m, s.verts);
		}
	} else if (p_op == "connect_vertices") {
		s.edges = hewn::ops::connect_vertices(m, s.verts);
	} else if (p_op == "fill_holes") {
		s.faces = hewn::ops::fill_holes(m, mode == MODE_VERTEX ? s.verts : mode == MODE_EDGE ? hewn::sel::verts_of_edges(s.edges) : std::vector<int>());
	} else if (p_op == "catmull_clark") {
		hewn::ops::SubdivOptions so;
		so.levels = (int)P("levels");
		so.simple = (int)P("kind") == 1;
		so.creases = (bool)P("creases");
		so.keep_corners = (int)P("boundary") == 1;
		so.uv_smooth = (int)P("uv_smooth");
		hewn::ops::subdivide_smooth(m, so);
		s.clear();
	} else if (p_op == "auto_smooth") {
		hewn::ops::auto_smooth(m, (float)P("angle"), mode == MODE_FACE ? s.faces : std::vector<int>());
		geometry = false;
	} else if (p_op == "quadify") {
		hewn::ops::quadify(m, (float)P("angle"));
		s.clear();
	} else if (p_op == "conform_all") {
		hewn::ops::conform_normals(m, {});
	} else if (p_op == "triangulate_all") {
		hewn::ops::triangulate_faces(m, hewn::sel::all_faces(m));
		s.clear();
	} else if (p_op == "flip_all") {
		hewn::ops::flip_faces(m, hewn::sel::all_faces(m));
	} else if (p_op == "uv_apply") {
		dock->apply_uv_params();
		geometry = false;
	} else if (p_op == "uv_group" || p_op == "uv_ungroup") {
		int g = 0;
		if (p_op == "uv_group") {
			for (const hewn::Face &F : m.faces) {
				g = MAX(g, F.uv_group);
			}
			g++;
		}
		for (int f : s.faces) {
			m.faces[f].uv_group = g;
			m.faces[f].manual_uv = false;
		}
		geometry = false;
	} else if (p_op == "uv_reset") {
		for (int f : s.faces) {
			m.faces[f].manual_uv = false;
			m.faces[f].uvp = hewn::UVParams();
		}
		geometry = false;
	} else if (p_op == "uv_rotate90" || p_op == "uv_flip_u" || p_op == "uv_flip_v" || p_op == "uv_fit") {
		// Bake the current projection into manual UVs, then transform around the selection center.
		const hewn::BuildOut &b = n->get_build();
		Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
		for (int f : s.faces) {
			hewn::Face &F = m.faces[f];
			if (!F.manual_uv && f < (int)b.face_uvs.size()) {
				F.uv = b.face_uvs[f];
				F.manual_uv = true;
			}
			for (const Vector2 &u : F.uv) {
				mn = mn.min(u);
				mx = mx.max(u);
			}
		}
		const Vector2 c = (mn + mx) * 0.5f;
		const Vector2 size = mx - mn;
		const float fit = MAX(size.x, size.y);
		for (int f : s.faces) {
			for (Vector2 &u : m.faces[f].uv) {
				Vector2 d = u - c;
				if (p_op == "uv_rotate90") {
					u = c + Vector2(-d.y, d.x);
				} else if (p_op == "uv_flip_u") {
					u = c + Vector2(-d.x, d.y);
				} else if (p_op == "uv_flip_v") {
					u = c + Vector2(d.x, -d.y);
				} else if (fit > 1e-9f) {
					u = (u - mn) / fit;
				}
			}
		}
		geometry = false;
	} else if (p_op.begins_with("material_slot:")) {
		const int slot = p_op.get_slice(":", 1).to_int();
		for (int f : (mode == MODE_FACE ? s.faces : hewn::sel::faces_from_verts(m, _selected_verts(), true))) {
			m.faces[f].material = slot;
		}
		geometry = false;
	} else if (p_op == "vertex_color") {
		const Color c = _paint_brush_value();
		const int ch = dock->paint_channel ? dock->paint_channel->get_selected() : 0;
		if (ch > 0 && n->custom_formats[ch - 1] < 0) {
			n->custom_formats[ch - 1] = dock->paint_format->get_selected();
		}
		if (mode == MODE_FACE) {
			for (int f : s.faces) {
				m.faces[f].ensure_attrs();
				for (int i = 0; i < m.faces[f].size(); i++) {
					_paint_value(m.faces[f], i, ch) = c;
				}
			}
		} else {
			std::vector<int> vs = _selected_verts();
			std::unordered_set<int> vset(vs.begin(), vs.end());
			for (hewn::Face &F : m.faces) {
				F.ensure_attrs();
				for (int i = 0; i < F.size(); i++) {
					if (vset.count(F.v[i])) {
						_paint_value(F, i, ch) = c;
					}
				}
			}
		}
		geometry = false;
	} else {
		editing = false;
		ERR_PRINT("Hewn: unknown operation " + p_op);
		return;
	}
	hewn::remove_unused_vertices(m, &s);
	sanitize(m, s);
	n->commit(geometry);
	end_edit();
	if (is_param_op(p_op)) {
		_record_param_op(p_op, op_mode);
	} else if (!redo_running) {
		_hide_redo();
	}
}
