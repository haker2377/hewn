// Hewn editor - the plugin: its life, the edited mesh and its modes, the editor settings.

#include "hw_plugin_impl.h"


// ------------------------------------------------------------------ lifecycle

HewnEditorPlugin *HewnEditorPlugin::singleton = nullptr;

void HewnEditorPlugin::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		singleton = this;
		i18n_init();
		// a mesh added to the scene gets the Shading view at once (not after the next check: no flash of its texture)
		get_tree()->connect("node_added", callable_mp(this, &HewnEditorPlugin::_on_node_added));
		_register_shortcuts();
		HewnTextureBrowser::register_settings();
		{
			Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
			if (es.is_valid() && OS::get_singleton()->get_environment("HW_SELFTEST").is_empty()) {
				redo_collapsed = es->get_project_metadata("hewn", "panel_collapsed", false);
				create_at_cursor = es->get_project_metadata("hewn", "create_at_cursor", false);
				// an early version: said once in each project
				if (!(bool)es->get_project_metadata("hewn", "notice_0_1", false)) {
					es->set_project_metadata("hewn", "notice_0_1", true);
					callable_mp(this, &HewnEditorPlugin::_early_notice).call_deferred();
				}
			}
		}
		gizmo_plugin.instantiate();
		gizmo_plugin->plugin = this;
		add_node_3d_gizmo_plugin(gizmo_plugin);
		entity_gizmo_plugin.instantiate();
		add_node_3d_gizmo_plugin(entity_gizmo_plugin);

		dock = memnew(HewnDock);
		dock->setup(this);
		add_dock(dock);


		texture_dock = memnew(HewnTextureBrowser);
		texture_dock->setup(this);
		add_dock(texture_dock);

		prefab_dock = memnew(HewnPrefabBrowser);
		prefab_dock->setup(this);
		add_dock(prefab_dock);
		scatter_dock = memnew(HewnScatterDock);
		scatter_dock->setup(this);
		add_dock(scatter_dock);
		scatter_dock->close(); // (opened while a Scatter is selected)

		uv_dock = memnew(EditorDock);
		uv_dock->set_title("Hewn UV");
		uv_dock->set_name("HewnUV");
		uv_dock->set_default_slot(EditorDock::DOCK_SLOT_BOTTOM);
		uv_dock->set_available_layouts(EditorDock::DOCK_LAYOUT_ALL); // (dragged to a side, it can be dragged back down)
		uv_canvas = memnew(HewnUVCanvas);
		uv_canvas->plugin = this;
		uv_canvas->set_custom_minimum_size(Vector2(0, 220));
		uv_canvas->set_clip_contents(true);
		uv_canvas->set_focus_mode(Control::FOCUS_ALL);
		uv_canvas->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		{
			VBoxContainer *uv_box = memnew(VBoxContainer);
			uv_dock->add_child(uv_box);
			uv_canvas->build_header(uv_box); // View / Select / Image / UV, element modes
			uv_box->add_child(uv_canvas);
		}
		add_dock(uv_dock);
		hotspot_dock = memnew(EditorDock);
		hotspot_dock->set_title("Hotspot Atlas");
		hotspot_dock->set_name("HewnHotspotAtlas");
		hotspot_dock->set_default_slot(EditorDock::DOCK_SLOT_BOTTOM);
		hotspot_dock->set_available_layouts(EditorDock::DOCK_LAYOUT_ALL); // (dragged to a side, it can be dragged back down)
		dock->build_hotspot_atlas_panel(hotspot_dock);
		add_dock(hotspot_dock);
		export_plugin.instantiate();
		add_export_plugin(export_plugin);



		obj_dialog = memnew(EditorFileDialog);
		obj_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
		obj_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
		obj_dialog->add_filter("*.obj", "Wavefront OBJ");
		obj_dialog->connect("file_selected", callable_mp(this, &HewnEditorPlugin::_on_obj_file));
		EditorInterface::get_singleton()->get_base_control()->add_child(obj_dialog);
		gltf_dialog = memnew(EditorFileDialog);
		gltf_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
		gltf_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
		gltf_dialog->add_filter("*.glb", "glTF Binary");
		gltf_dialog->add_filter("*.gltf", "glTF Text");
		gltf_dialog->add_filter("*.fbx", "FBX");
		gltf_dialog->connect("file_selected", callable_mp(this, &HewnEditorPlugin::_on_gltf_file));
		EditorInterface::get_singleton()->get_base_control()->add_child(gltf_dialog);

		EditorInterface::get_singleton()->get_selection()->connect("selection_changed", callable_mp(this, &HewnEditorPlugin::_on_editor_selection_changed));
		// (undo / redo: what is drawn over the viewport follows the restored mesh and selection at once)
		get_undo_redo()->connect("version_changed", callable_mp(this, &HewnEditorPlugin::_on_history_version));
		extrude_menu = memnew(PopupMenu);
		extrude_menu->add_item("Extrude Region", 0);
		extrude_menu->add_item("Extrude Region (Dissolve Orthogonal Edges)", 3);
		extrude_menu->add_item("Extrude Along Normals", 1);
		extrude_menu->add_item("Extrude Individual Faces", 2);
		extrude_menu->connect("id_pressed", callable_mp(this, &HewnEditorPlugin::_on_extrude_menu));
		EditorInterface::get_singleton()->get_base_control()->add_child(extrude_menu);
		cursor_menu = memnew(PopupMenu);
		cursor_menu->add_item("Cursor to Selected", 0);
		cursor_menu->add_item("Cursor to World Origin", 1);
		cursor_menu->add_item("Cursor to Object Origin", 2);
		cursor_menu->add_separator();
		cursor_menu->add_item("Selection to Cursor", 3);
		cursor_menu->add_separator();
		cursor_menu->add_item("Move Cursor  (G over it)", 4);
		cursor_menu->add_item("Rotate Cursor  (R over it)", 5);
		cursor_menu->connect("id_pressed", callable_mp(this, &HewnEditorPlugin::_on_cursor_menu));
		EditorInterface::get_singleton()->get_base_control()->add_child(cursor_menu);

		_place_toolbar();
		register_editor_settings(); // (the header / the tool column as the settings say)
		// the editor self-test and its probes (a script of the test project, not a plugin of its own)
		for (const char *v : { "HW_SELFTEST", "HW_ICONS", "HW_UI_SHOT", "HW_WIDTHS", "HW_HEIGHTS", "HW_PERF" }) {
			// (HW_PERF: the timing script on a heavy scene)
			const String script = String(v) == "HW_PERF" ? "res://tests/editor_perf.gd" : "res://tests/editor_selftest.gd";
			if (!selftest && !selftest_started && !OS::get_singleton()->get_environment(v).is_empty() && FileAccess::file_exists(script)) {
				Ref<Script> st = ResourceLoader::get_singleton()->load(script);
				if (st.is_valid()) {
					selftest_started = true; // (once: not again when the plugin is switched off and on)
					selftest = memnew(Node);
					selftest->set_name("HewnSelfTest");
					selftest->set_script(st);
					add_child(selftest);
				}
			}
		}
		callable_mp(this, &HewnEditorPlugin::_order_bottom_docks).call_deferred();
		localize(dock);
		localize(dock->toolbar_root);
		localize(dock->tool_column);
		localize(dock->toolbar_fold);
		localize(dock->paint_root);
		localize(dock->cutter_bar);
		localize(extrude_menu);
		localize(cursor_menu);
		for (Node *d : { (Node *)texture_dock, (Node *)prefab_dock }) {
			if (d) {
				localize(d); // (and their number fields take decimal commas)
			}
		}
		uv_dock->set_title(T("Hewn UV"));
		hotspot_dock->set_title(T("Hotspot Atlas"));
		set_process(true);
		set_process_input(true);
		set_input_event_forwarding_always_enabled();
		set_force_draw_over_forwarding_enabled();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		if (mcp) {
			mcp->stop();
			mcp->queue_free();
			mcp = nullptr;
		}
		// (switched off or quitting in the middle of a mode: its skeleton, its hidden mesh, its pictures put right)
		if (wpaint) {
			exit_wpaint(false);
		}
		if (rig) {
			exit_rig(false);
		}
		if (tpaint) {
			exit_tpaint(false);
		}
		if (pose) {
			exit_pose(false);
		}
		if (get_tree()->is_connected("node_added", callable_mp(this, &HewnEditorPlugin::_on_node_added))) {
			get_tree()->disconnect("node_added", callable_mp(this, &HewnEditorPlugin::_on_node_added));
		}
		if (quad_added) {
			// (later: the editor is busy removing this one; when it quits it frees them all itself)
			callable_mp_static(&hw_remove_quad_plugin).call_deferred();
			quad_added = false;
		}
		if (singleton == this) {
			singleton = nullptr;
		}
		_marks_free();
		hw_gizmo_clear_cache();
		_clear_into();
		if (dock && dock->snap_fallback) {
			memdelete(dock->snap_fallback);
			dock->snap_fallback = nullptr;
			dock->snap_enabled = nullptr;
		}
		if (dock && dock->toolbar_fold) {
			if (dock->toolbar_fold->get_parent()) {
				dock->toolbar_fold->get_parent()->remove_child(dock->toolbar_fold);
			}
			memdelete(dock->toolbar_fold);
			dock->toolbar_fold = nullptr;
		}
		if (dock && dock->tool_column) {
			if (dock->tool_column->get_parent()) {
				dock->tool_column->get_parent()->remove_child(dock->tool_column);
			}
			memdelete(dock->tool_column);
			dock->tool_column = nullptr;
		}
		if (dock && dock->cutter_bar) {
			if (dock->cutter_bar->get_parent()) {
				dock->cutter_bar->get_parent()->remove_child(dock->cutter_bar);
			}
			memdelete(dock->cutter_bar);
			dock->cutter_bar = nullptr;
		}
		if (dock && dock->paint_root) {
			if (dock->paint_root->get_parent()) {
				dock->paint_root->get_parent()->remove_child(dock->paint_root);
			}
			memdelete(dock->paint_root);
			dock->paint_root = nullptr;
		}
		if (dock && dock->toolbar_root) {
			if (dock->toolbar_root->get_parent()) {
				dock->toolbar_root->get_parent()->remove_child(dock->toolbar_root);
			}
			memdelete(dock->toolbar_root);
			dock->toolbar_root = nullptr;
			dock->toolbar = nullptr;
		}
		cursor_menu->queue_free();
		cursor_menu = nullptr;
		extrude_menu->queue_free();
		extrude_menu = nullptr;
		_set_godot_gizmo_hidden(false);
		remove_node_3d_gizmo_plugin(gizmo_plugin);
		remove_node_3d_gizmo_plugin(entity_gizmo_plugin);
		entity_gizmo_plugin.unref();
		gizmo_plugin->plugin = nullptr;
		gizmo_plugin.unref();
		remove_dock(hotspot_dock); // (first: its canvas points at the dock)
		memdelete(hotspot_dock);
		hotspot_dock = nullptr;
		remove_dock(dock);
		memdelete(dock);
		dock = nullptr;
		remove_dock(uv_dock);
		if (export_plugin.is_valid()) {
			remove_export_plugin(export_plugin);
			export_plugin.unref();
		}
		memdelete(uv_dock);
		uv_dock = nullptr;
		remove_dock(texture_dock);
		memdelete(texture_dock);
		texture_dock = nullptr;
		_remove_prefab_ghost();
		remove_dock(prefab_dock);
		memdelete(prefab_dock);
		prefab_dock = nullptr;
		if (scatter_dock) {
			remove_dock(scatter_dock);
			memdelete(scatter_dock);
			scatter_dock = nullptr;
		}
		uv_canvas = nullptr;
		obj_dialog->queue_free();
		obj_dialog = nullptr;
		gltf_dialog->queue_free();
		gltf_dialog = nullptr;
		for (EditorFileDialog **d : { &bake_dialog, &scene_gltf_dialog }) {
			if (*d) {
				(*d)->queue_free();
				*d = nullptr;
			}
		}
	}
}

String HewnEditorPlugin::_get_plugin_name() const {
	return "Hewn";
}

// a closed block on `region`, `d` long: its bottom on the faces, its own
// vertices; its faces appended to `m`, the indices of its tops returned
std::vector<int> hw_make_block(hewn::Mesh &m, const std::vector<int> &region, const Vector3 &d) {
	std::unordered_map<int, int> bottom, top;
	Vector3 avg;
	for (int f : region) {
		avg += hewn::face_normal(m, m.faces[f]);
		for (int v : m.faces[f].v) {
			if (!bottom.count(v)) {
				bottom[v] = m.add_vertex(m.pos[v]);
				top[v] = m.add_vertex(m.pos[v] + d);
			}
		}
	}
	const bool outward = avg.dot(d) >= 0.0f; // (into the mesh: every new face turned around)
	std::unordered_set<uint64_t> half;
	for (int f : region) {
		const hewn::Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			half.insert(hewn::hkey(F.v[i], F.v[(i + 1) % F.size()]));
		}
	}
	std::vector<hewn::Face> made;
	std::vector<int> new_top;
	for (int f : region) {
		const hewn::Face F = m.faces[f];
		std::vector<int> bv, tv;
		for (int v : F.v) {
			bv.push_back(bottom[v]);
			tv.push_back(top[v]);
		}
		hewn::Face B = hewn::make_face(bv, &F), Tp = hewn::make_face(tv, &F);
		B.reverse(); // (the bottom looks back at the face it stands on)
		made.push_back(B);
		new_top.push_back((int)(m.faces.size() + made.size()));
		made.push_back(Tp);
		for (int i = 0; i < F.size(); i++) {
			const int a = F.v[i], b = F.v[(i + 1) % F.size()];
			if (half.count(hewn::hkey(b, a))) {
				continue; // (inside the region)
			}
			made.push_back(hewn::make_face({ bottom[a], bottom[b], top[b], top[a] }, &F));
		}
	}
	for (hewn::Face &F : made) {
		if (!outward) {
			F.reverse();
		}
		F.manual_uv = false;
		F.uv.clear();
		m.faces.push_back(F);
	}
	return new_top;
}

// (several nodes selected: Godot edits a MultiNodeEdit; with Hewn meshes among them the plugin
// stays, so Tab edits them all and G / R / S move them)
static HewnMesh3D *last_selected_mesh() {
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
	for (int64_t i = sel.size() - 1; i >= 0; i--) {
		if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(sel[i])) {
			return m;
		}
	}
	return nullptr;
}

bool HewnEditorPlugin::_handles(Object *p_object) const {
	if (p_object && p_object->get_class() == "MultiNodeEdit") {
		return last_selected_mesh() != nullptr;
	}
	return Object::cast_to<HewnMesh3D>(p_object) != nullptr || Object::cast_to<Path3D>(p_object) != nullptr || // (a Path3D: its points with Tab)
			Object::cast_to<HewnScatter3D>(p_object) != nullptr || // (a Scatter: its brushes)
			Object::cast_to<Skeleton3D>(p_object) != nullptr || // (a skeleton: its Pose mode)
			Object::cast_to<MeshInstance3D>(p_object) != nullptr || // (any mesh: its Texture Paint)
			Object::cast_to<HewnCutter3D>(p_object) != nullptr; // (a cutter: G / R / S with the snapping)
}

HewnMesh3D *HewnEditorPlugin::get_edited() const {
	if (edited_id == 0) {
		return nullptr;
	}
	// (another scene tab: its nodes are out of the tree until it comes back)
	HewnMesh3D *n = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(edited_id));
	return n && n->is_inside_tree() ? n : nullptr;
}

std::vector<HewnMesh3D *> HewnEditorPlugin::edited_meshes() const {
	std::vector<HewnMesh3D *> out;
	if (HewnMesh3D *a = get_edited()) {
		out.push_back(a);
	}
	for (uint64_t id : multi_ids) {
		HewnMesh3D *m = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(id)));
		if (m && m->is_inside_tree() && (out.empty() || m != out[0])) {
			out.push_back(m);
		}
	}
	return out;
}

bool HewnEditorPlugin::is_edited_mesh(const HewnMesh3D *p_node) const {
	if (!p_node) {
		return false;
	}
	if (p_node->get_instance_id() == edited_id) {
		return true;
	}
	for (uint64_t id : multi_ids) {
		if (id == p_node->get_instance_id()) {
			return true;
		}
	}
	return false;
}

// Another of the edited meshes becomes the active one (a click on its elements).
void HewnEditorPlugin::_set_active_mesh(HewnMesh3D *p_node) {
	if (!p_node || p_node->get_instance_id() == edited_id) {
		return;
	}
	for (uint64_t &id : multi_ids) {
		if (id == p_node->get_instance_id()) {
			id = edited_id;
			break;
		}
	}
	edited_id = p_node->get_instance_id();
	hover_vert = hover_face = -1;
	hover_edge = hewn::Edge(-1, -1);
	if (EditorInterface::get_singleton()->get_inspector()) {
		EditorInterface::get_singleton()->get_inspector()->call_deferred("edit", p_node);
	}
	for (HewnMesh3D *m : edited_meshes()) {
		m->update_gizmos();
	}
}

void HewnEditorPlugin::_clear_multi() {
	std::vector<HewnMesh3D *> others = edited_meshes();
	multi_ids.clear();
	for (HewnMesh3D *m : others) {
		m->update_gizmos();
	}
}

void HewnEditorPlugin::_edit(Object *p_object) {
	if (!p_object && pose_detached && EditorInterface::get_singleton()->get_selection()->get_selected_nodes().is_empty()) {
		// (our own: the skeleton taken out of the selection for its Pose mode; the inspector shows it again)
		if (Skeleton3D *ps = pose_skeleton()) {
			EditorInterface::get_singleton()->get_inspector()->call_deferred("edit", ps);
		}
		return;
	}
	if (p_object && pose && p_object != pose_skeleton()) {
		pose_detached = false;
		exit_pose(false); // (something else selected: out of Pose mode)
	}
	if (!p_object && (rig_detached || wp_detached) && EditorInterface::get_singleton()->get_selection()->get_selected_nodes().is_empty()) {
		// (our own: the skeleton / the mesh out of the selection for Rigging / Weight Paint; the inspector shows it again)
		Object *o = rig_detached ? (Object *)rig_skeleton() : (Object *)wp_mesh();
		if (o) {
			EditorInterface::get_singleton()->get_inspector()->call_deferred("edit", o);
		}
		return;
	}
	if (p_object && rig && p_object != rig_skeleton()) {
		rig_detached = false;
		exit_rig(false);
	}
	if (p_object && wpaint && p_object != wp_mesh()) {
		wp_detached = false;
		exit_wpaint(false);
	}
	if (!p_object && tp_detached && EditorInterface::get_singleton()->get_selection()->get_selected_nodes().is_empty()) {
		// (our own: the mesh taken out of the selection for Texture Paint; the inspector shows it again)
		if (MeshInstance3D *tm = tp_mesh()) {
			EditorInterface::get_singleton()->get_inspector()->call_deferred("edit", tm);
		}
		return;
	}
	if (p_object && tpaint && p_object != tp_mesh()) {
		tp_detached = false;
		exit_tpaint(false); // (something else selected: out of Texture Paint)
	}
	if (!p_object && hc_detached && EditorInterface::get_singleton()->get_selection()->get_selected_nodes().is_empty()) {
		// (our own: the curve taken out of the selection for its edit mode; the inspector shows it again)
		if (Object *hc = ObjectDB::get_instance(curve_id)) {
			EditorInterface::get_singleton()->get_inspector()->call_deferred("edit", hc);
		}
		return;
	}
	if (p_object && hc_detached && p_object != ObjectDB::get_instance(curve_id)) {
		hc_detached = false; // (something else selected: out of the curve's edit mode)
	}
	{
		Path3D *cp = Object::cast_to<Path3D>(p_object);
		const ObjectID nid = cp ? ObjectID(cp->get_instance_id()) : ObjectID();
		if (nid != curve_id) {
			curve_edit = false;
			curve_sel.clear();
			curve_xf = 0;
			hc_edit = false;
			hc_xf = 0;
			hc_draw = false;
		}
		curve_id = nid;
		HewnScatter3D *sc = Object::cast_to<HewnScatter3D>(p_object);
		const ObjectID sid = sc ? ObjectID(sc->get_instance_id()) : ObjectID();
		if (sid != scatter_id) {
			if (scatter_painting) {
				_scatter_end();
			}
			scatter_tool = sc && scatter_tool_next ? 0 : sc ? scatter_tool : -1;
			scatter_tool_next = false;
		}
		scatter_id = sid;
		if (scatter_dock) {
			scatter_dock->show_scatter(sc);
		}
	}
	HewnMesh3D *old = get_edited();
	HewnMesh3D *n = Object::cast_to<HewnMesh3D>(p_object);
	if (!n && p_object && p_object->get_class() == "MultiNodeEdit") {
		n = last_selected_mesh();
	}
	if (n && n == old) {
		selection_changed(); // re-edit of the same node (selection refresh)
		return;
	}
	if (!n && detached) {
		// We took the node out of Godot's selection ourselves (edit mode). Godot has just cleared the
		// inspector; show the mesh there again.
		if (old) {
			EditorInterface::get_singleton()->get_inspector()->call_deferred("edit", old);
		}
		return;
	}
	if (modal != MODAL_NONE && modal != MODAL_POLY) {
		cancel_tool();
	}
	detached = false; // another node got selected: the selection is already where it should be
	_clear_multi();
	_check_brush_click(n);
	edited_id = n ? n->get_instance_id() : 0;
	mode = MODE_OBJECT;
	hover_vert = hover_face = -1;
	hover_edge = hewn::Edge(-1, -1);
	drag = DRAG_NONE;
	if (old && old != n) {
		old->update_gizmos();
	}
	selection_changed();
}

void HewnEditorPlugin::_make_visible(bool p_visible) {
	if (!p_visible && !detached) {
		_edit(nullptr);
	}
}

void HewnEditorPlugin::selection_changed() {
	HW_PROF("selection_changed");
	HewnMesh3D *n = get_edited();
	if (n) {
		sanitize(n->get_data(), n->selection);
		n->update_gizmos();
	}
	if (dock) {
		dock->refresh();
	}
	if (uv_canvas) {
		uv_canvas->queue_redraw();
	}
	update_overlays();
}

// In edit mode the mesh is taken out of Godot's selection (the inspector keeps showing it), so Godot
// draws neither its transform gizmo nor the orange bounding box. Nothing in the node or in the editor
// settings is changed. Leaving edit mode selects the node again.
void HewnEditorPlugin::_set_godot_gizmo_hidden(bool p_hidden) {
	HewnMesh3D *n = get_edited();
	EditorInterface *ei = EditorInterface::get_singleton();
	EditorSelection *es = ei->get_selection();
	if (p_hidden) {
		if (!n || detached) {
			return;
		}
		detached = true;
		es->clear();
		// The inspector is set back to the mesh in _edit(nullptr), which Godot calls after clearing it
		// (EditorInterface::inspect_object would select the node again).
	} else {
		if (!detached) {
			return;
		}
		detached = false;
		if (n && n->is_inside_tree()) {
			es->clear();
			es->add_node(n);
		}
	}
}

void HewnEditorPlugin::_early_notice() {
	if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
		t->push_toast(T("Hewn is an early version: save your scenes often"), EditorToaster::SEVERITY_WARNING);
	}
}

void HewnEditorPlugin::_on_history_version() {
	if (last_op == "cut" && !cut_running && redo_panel && redo_panel->is_visible()) {
		// (something done in another history - the cut's own steps keep its version - and not by the cut: its panel goes)
		EditorUndoRedoManager *ur = get_undo_redo();
		UndoRedo *h = last_cut.history >= 0 ? ur->get_history_undo_redo((int)last_cut.history) : nullptr;
		if (!h || (int64_t)h->get_version() != last_cut.version || last_cut.history == -2) {
			if (!_cut_panel_valid()) {
				_hide_redo();
			}
		} else {
			_hide_redo();
		}
	}
	for (HewnMesh3D *m : edited_meshes()) {
		m->update_gizmos();
	}
	update_overlays();
	if (dock) {
		dock->refresh();
	}
	if (uv_canvas) {
		uv_canvas->queue_redraw(); // (an undone UV change)
	}
}

void HewnEditorPlugin::_on_editor_selection_changed() {
	{
		// the order things were selected in
		const TypedArray<Node> now = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
		std::vector<ObjectID> keep;
		for (const ObjectID &id : sel_order) {
			for (int64_t i = 0; i < now.size(); i++) {
				if (Object::cast_to<Node>(now[i]) && ObjectID(Object::cast_to<Node>(now[i])->get_instance_id()) == id) {
					keep.push_back(id);
					break;
				}
			}
		}
		for (int64_t i = 0; i < now.size(); i++) {
			Node *nd = Object::cast_to<Node>(now[i]);
			if (nd && std::find(keep.begin(), keep.end(), ObjectID(nd->get_instance_id())) == keep.end()) {
				keep.push_back(ObjectID(nd->get_instance_id()));
			}
		}
		sel_order = keep;
	}
	if (dock && dock->ov_wireframe && dock->ov_wireframe->is_pressed()) {
		dock->update_all_gizmos(); // the selected meshes' wireframe is highlighted
	}
	if (pose_detached && !EditorInterface::get_singleton()->get_selection()->get_selected_nodes().is_empty()) {
		pose_detached = false; // (another node picked in the scene tree: out of Pose mode, that one stays selected)
		exit_pose(false);
	}
	if (tp_detached && !EditorInterface::get_singleton()->get_selection()->get_selected_nodes().is_empty()) {
		tp_detached = false; // (another node picked: out of Texture Paint)
		exit_tpaint(false);
	}
	if (rig_detached && !EditorInterface::get_singleton()->get_selection()->get_selected_nodes().is_empty()) {
		rig_detached = false;
		exit_rig(false);
	}
	if (wp_detached && !EditorInterface::get_singleton()->get_selection()->get_selected_nodes().is_empty()) {
		wp_detached = false;
		exit_wpaint(false);
	}
	// The user selected something else (scene tree, another viewport click...): leave edit mode.
	if (!detached) {
		return;
	}
	TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
	if (sel.is_empty()) {
		return; // our own clear()
	}
	detached = false;
	if (modal != MODAL_NONE && modal != MODAL_POLY) {
		cancel_tool();
	}
	mode = MODE_OBJECT;
	selection_changed();
}

// The edited node can disappear without any selection change (deleted, scene tab switched).
void HewnEditorPlugin::_validate_edited() {
	if (!detached) {
		return;
	}
	HewnMesh3D *n = get_edited();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (n && n->is_inside_tree() && root && (n == root || root->is_ancestor_of(n))) {
		return;
	}
	detached = false;
	if (modal != MODAL_NONE && modal != MODAL_POLY) {
		cancel_tool();
	}
	edited_id = 0;
	mode = MODE_OBJECT;
	selection_changed();
}

void HewnEditorPlugin::set_mode(int p_mode) {
	const Mode old = mode;
	Mode nm = (Mode)CLAMP(p_mode, 0, 3);
	HewnMesh3D *n = get_edited();
	if ((nm == MODE_OBJECT || !n) && sculpt) {
		// leaving Sculpt
		if (sculpting) {
			sculpting = false;
			end_edit();
		}
		sculpt = false;
		if (modal == MODAL_SCULPT) {
			modal = MODAL_NONE;
		}
		if (dock) {
			dock->show_paint_bar(false);
		}
	}
	if ((nm == MODE_OBJECT || !n) && vpaint) {
		// leaving Vertex Paint
		if (painting) {
			painting = false;
			end_edit();
		}
		vpaint = false;
		if (modal == MODAL_PAINT) {
			modal = MODAL_NONE;
		}
		apply_paint_view();
		if (dock) {
			dock->show_paint_bar(false);
		}
	}
	if (nm != MODE_OBJECT) {
		element_mode = nm;
	}
	if (!n) {
		nm = MODE_OBJECT;
	}
	if (modal != MODAL_NONE && modal != MODAL_POLY && !((modal == MODAL_PAINT || modal == MODAL_SCULPT) && nm != MODE_OBJECT)) {
		cancel_tool(); // (the paint tools stay on: vertices / edges / faces only say what they paint)
	}
	if (n && old != nm && old != MODE_OBJECT && nm != MODE_OBJECT) {
		hewn::Mesh &m = n->get_data();
		hewn::Selection &s = n->selection;
		hewn::Topology t;
		t.build(m);
		std::vector<int> verts = old == MODE_VERTEX ? s.verts : old == MODE_EDGE ? hewn::sel::verts_of_edges(s.edges) : hewn::sel::verts_of_faces(m, s.faces);
		if (nm == MODE_VERTEX) {
			s.verts = verts;
		} else if (nm == MODE_EDGE) {
			s.edges = old == MODE_FACE ? hewn::sel::edges_of_faces(m, s.faces) : hewn::sel::edges_from_verts(m, t, verts);
		} else {
			s.faces = hewn::sel::faces_from_verts(m, verts, true);
		}
	}
	if (old == MODE_OBJECT && nm != MODE_OBJECT && n) {
		// Tab with several meshes selected: all of them are edited
		multi_ids.clear();
		for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_selected_nodes()) {
			HewnMesh3D *o = Object::cast_to<HewnMesh3D>(v);
			if (o && o != n && !o->is_csg_model()) {
				multi_ids.push_back(o->get_instance_id());
				o->update_gizmos();
			}
		}
	}
	const std::vector<HewnMesh3D *> leaving = nm == MODE_OBJECT ? edited_meshes() : std::vector<HewnMesh3D *>();
	mode = nm;
	if (nm != MODE_OBJECT) {
		mode_mask = 1 << nm;
	}
	if (!redo_running) {
		_hide_redo();
	}
	set_xray(xray_on);
	_set_godot_gizmo_hidden(mode != MODE_OBJECT);
	if (nm == MODE_OBJECT && leaving.size() > 1) {
		// back to objects: all the edited ones selected again
		EditorSelection *es = EditorInterface::get_singleton()->get_selection();
		for (HewnMesh3D *m : leaving) {
			es->add_node(m);
		}
		_clear_multi();
	}
	hover_vert = hover_face = -1;
	hover_edge = hewn::Edge(-1, -1);
	drag = DRAG_NONE;
	selection_changed();
}

// X-ray: the edited mesh becomes see-through and hidden elements can be selected.
void HewnEditorPlugin::set_xray(bool p_on) {
	xray_on = p_on;
	if (dock && dock->xray->is_pressed() != p_on) {
		dock->xray->set_pressed_no_signal(p_on);
	}
	// see-through: the edited meshes in edit mode, every mesh of the scene otherwise (drawing a cut, a shape)
	for (const ObjectID &id : xray_meshes) {
		if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(id))) {
			if (m->get_instance().is_valid()) {
				RenderingServer::get_singleton()->instance_geometry_set_transparency(m->get_instance(), m->get_transparency());
				m->refresh_display(); // (a boolean operand or brush is see-through again)
			}
		}
	}
	xray_meshes.clear();
	if (p_on) {
		std::vector<HewnMesh3D *> list = mode != MODE_OBJECT ? edited_meshes() : _scene_meshes();
		for (HewnMesh3D *m : list) {
			if (m && m->get_instance().is_valid()) {
				RenderingServer::get_singleton()->instance_geometry_set_transparency(m->get_instance(), MAX(0.6f, m->get_transparency()));
				xray_meshes.push_back(ObjectID(m->get_instance_id()));
			}
		}
	}
	update_overlays();
}

// Shift+1/2/3 or Shift+click on a mode button: several element modes at once.
void HewnEditorPlugin::toggle_mode(int p_mode) {
	HewnMesh3D *n = get_edited();
	if (!n || p_mode < MODE_VERTEX || p_mode > MODE_FACE) {
		return;
	}
	if (mode == MODE_OBJECT) {
		set_mode(p_mode);
		return;
	}
	const int bit = 1 << p_mode;
	if (mode_mask & bit) {
		if (mode_mask == bit) {
			return; // keep at least one
		}
		mode_mask &= ~bit;
		for (int m = MODE_FACE; m >= MODE_VERTEX; m--) {
			if (mode_mask & (1 << m)) {
				mode = (Mode)m;
				break;
			}
		}
	} else {
		// the new element type starts with what the current selection covers
		hewn::Mesh &m = n->get_data();
		hewn::Selection &s = n->selection;
		hewn::Topology t;
		t.build(m);
		const std::vector<int> verts = _selected_verts();
		if (p_mode == MODE_VERTEX) {
			s.verts = verts;
		} else if (p_mode == MODE_EDGE) {
			s.edges = hewn::sel::edges_from_verts(m, t, verts);
		} else {
			s.faces = hewn::sel::faces_from_verts(m, verts, true);
		}
		mode_mask |= bit;
		mode = (Mode)p_mode;
	}
	element_mode = mode;
	selection_changed();
}

void HewnEditorPlugin::toggle_edit_mode() {
	if (vpaint) {
		exit_vpaint(); // (Tab in Vertex Paint: back to object mode)
		return;
	}
	set_mode(mode == MODE_OBJECT ? element_mode : MODE_OBJECT);
}

// The plugin snaps when Godot's own snap (magnet) toggle of the 3D toolbar is on.
void HewnEditorPlugin::_find_godot_snap(Node *p_row) {
	Ref<Texture2D> magnet = EditorInterface::get_singleton()->get_editor_theme()->get_icon("Snap", "EditorIcons");
	std::vector<Node *> stack = { p_row };
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		Button *b = Object::cast_to<Button>(x);
		if (b && b->is_toggle_mode() && b->get_button_icon() == magnet) {
			dock->snap_enabled = b;
			return;
		}
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
	}
}

// Objects selected (object mode) or elements of the edited meshes (edit mode / Vertex Paint).
bool HewnEditorPlugin::_anything_selected() const {
	if (mode == MODE_OBJECT) {
		return !EditorInterface::get_singleton()->get_selection()->get_selected_nodes().is_empty();
	}
	for (HewnMesh3D *m : edited_meshes()) {
		if (!m->selection.verts.empty() || !m->selection.edges.empty() || !m->selection.faces.empty()) {
			return true;
		}
	}
	return false;
}

// The camera onto the selection, turned as it is (Godot's own Focus Selection of the viewport's View menu:
// it moves the camera smoothly).
void HewnEditorPlugin::focus_selection(Camera3D *p_cam) {
	Control *surface = _surface_for(p_cam);
	if (!surface) {
		return;
	}
	for (const Variant &v : surface->find_children("*", "MenuButton", true, false)) {
		MenuButton *mb = Object::cast_to<MenuButton>(v);
		if (!mb) {
			continue;
		}
		PopupMenu *pm = mb->get_popup();
		for (int i = 0; i < pm->get_item_count(); i++) {
			Ref<Shortcut> sc = pm->get_item_shortcut(i);
			if (sc.is_valid() && (sc->get_name() == "Focus Selection" || pm->get_item_text(i) == "Focus Selection")) {
				pm->emit_signal("id_pressed", pm->get_item_id(i));
				return;
			}
		}
	}
}

// Puts the Hewn toolbar on its own row right under the 3D editor's toolbar.
void HewnEditorPlugin::_place_toolbar() {
	Control *bar = dock->toolbar_root;
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, bar);
	Node *row = bar->get_parent();
	// Walk up to the child of the first vertical box: that is the toolbar row; insert ours after it.
	while (row && row->get_parent()) {
		VBoxContainer *vb = Object::cast_to<VBoxContainer>(row->get_parent());
		if (vb) {
			remove_control_from_container(CONTAINER_SPATIAL_EDITOR_MENU, bar);
			// Floating over the viewports (top level: ignored by the box layout), so showing or hiding
			// it never resizes the viewports.
			vb->add_child(bar);
			bar->set_as_top_level(true);
			bar->set_z_index(10);
			vb->add_child(dock->tool_column);
			dock->tool_column->set_as_top_level(true);
			dock->tool_column->set_z_index(10);
			vb->add_child(dock->toolbar_fold);
			dock->toolbar_fold->set_as_top_level(true);
			dock->toolbar_fold->set_z_index(10);
			vb->add_child(dock->paint_root);
			dock->paint_root->set_as_top_level(true);
			dock->paint_root->set_z_index(10);
			vb->add_child(dock->cutter_bar);
			dock->cutter_bar->set_as_top_level(true);
			dock->cutter_bar->set_z_index(10);
			toolbar_row_id = row->get_instance_id();
			_find_godot_snap(row);
			_layout_toolbar();
			return;
		}
		row = row->get_parent();
	}
}

void HewnEditorPlugin::_layout_toolbar() {
	Control *bar = dock ? dock->toolbar_root : nullptr;
	Control *row = toolbar_row_id ? Object::cast_to<Control>(ObjectDB::get_instance(toolbar_row_id)) : nullptr;
	if (!bar || !row || !row->is_visible_in_tree()) {
		bar_shown = false;
		return;
	}
	const float margin = 8.0f;
	const Rect2 r = row->get_global_rect();
	const Vector2 content = dock->toolbar->get_combined_minimum_size();
	Control *fold = dock->toolbar_fold;
	const Vector2 fold_size = fold ? fold->get_combined_minimum_size() : Vector2();
	// the header's height as when it is open: folding it moves nothing else (the tool column stays)
	if (content.x >= 1.0f && content.y > 0.0f) {
		header_h = content.y;
	}
	const float hh = MAX(header_h, MAX(content.y, fold_size.y));
	const float fold_w = fold ? fold_size.x + 6.0f : 0.0f;
	// In the row of the viewport's own view menu ("Perspective" ...): right of it, left of the
	// navigation gizmo in the top right corner. Fallback: under the view menu.
	Vector2 pos(r.position.x + margin, r.position.y + r.size.y + 36.0f);
	float avail = r.size.x - margin * 2.0f;
	Control *surface = _main_surface();
	if (surface && surface->is_visible_in_tree()) {
		const Rect2 sr = surface->get_global_rect();
		// (the view menu is looked up once per viewport, not every frame)
		Control *menu = view_menu_id ? Object::cast_to<Control>(ObjectDB::get_instance(view_menu_id)) : nullptr;
		if (!menu || !surface->is_ancestor_of(menu)) {
			menu = nullptr;
			const TypedArray<Node> menus = surface->find_children("*", "MenuButton", true, false);
			for (int64_t i = 0; i < menus.size(); i++) {
				Control *c = Object::cast_to<Control>(menus[i]);
				if (!c || !c->is_visible_in_tree() || c->get_global_position().y > sr.position.y + 60.0f || c->get_global_position().x > sr.position.x + sr.size.x * 0.5f) {
					continue;
				}
				if (!menu || c->get_global_position().x < menu->get_global_position().x) {
					menu = c;
				}
			}
			view_menu_id = menu ? menu->get_instance_id() : 0;
		}
		if (menu) {
			const Rect2 mr = menu->get_global_rect();
			pos = Vector2(mr.position.x + mr.size.x + margin, mr.position.y + (mr.size.y - hh) * 0.5f);
			avail = sr.position.x + sr.size.x - 110.0f - pos.x; // the navigation gizmo
		}
	}
	if (fold) {
		// open: in the row, as tall as its blocks; folded: a small tab at the top edge of the view
		Vector2 fpos(pos.x, pos.y);
		Vector2 fsz(fold_size.x, MAX(fold_size.y, hh));
		if (dock->toolbar_folded) {
			fsz = fold_size;
			fpos.y = surface && surface->is_visible_in_tree() ? surface->get_global_rect().position.y : pos.y + (hh - fold_size.y) * 0.5f;
		}
		if (fold->get_global_position() != fpos) {
			fold->set_global_position(fpos);
		}
		if (fold->get_size() != fsz) {
			fold->set_size(fsz);
		}
		pos.x += fold_w;
		avail -= fold_w;
	}
	const bool folded = content.x < 1.0f;
	// (and room under it for the blocks' shadow: the row clips what goes out of it)
	const float shadow = hw_editor::shadow_room()->get_content_margin(SIDE_BOTTOM);
	const Vector2 size(MIN(content.x, MAX(folded ? 0.0f : 40.0f, avail)), hh + shadow);
	if (bar->get_global_position() != pos) {
		bar->set_global_position(pos);
	}
	if (bar->get_size() != size) {
		bar->set_size(size);
	}
	if (ScrollContainer *sc = Object::cast_to<ScrollContainer>(bar)) {
		if (!bar_shown || (content.x <= size.x + 0.5f && sc->get_h_scroll() != 0)) {
			sc->set_h_scroll(0); // from the start when shown, or when everything fits again
		}
		bar_shown = true;
	}
	// the second row (Vertex Paint's, the Box Cutter's): under the header, its own width and scrolling. It is
	// right of the view menu, the tool column left of it: the column does not move down for it.
	float extra = 0.0f;
	for (Control *second : { (Control *)dock->paint_root, dock->cutter_bar }) {
		if (!second || !second->is_visible()) {
			continue;
		}
		Control *inner = Object::cast_to<Control>(second->get_child(0));
		const Vector2 pc = inner ? inner->get_combined_minimum_size() : second->get_combined_minimum_size();
		extra = MAX(extra, pc.y + 6.0f); // (the header's text goes under this row)
		const Vector2 ppos(pos.x - fold_w, pos.y + hh + 4.0f);
		const Vector2 psize(MIN(pc.x, MAX(40.0f, avail + fold_w)), pc.y + shadow);
		if (second->get_global_position() != ppos) {
			second->set_global_position(ppos);
		}
		if (second->get_size() != psize) {
			second->set_size(psize);
		}
	}
	if (hw_editor::header_extra() != extra) {
		hw_editor::header_extra() = extra;
		update_overlays();
	}
	// the tool column: left edge of the viewport, under the header row
	Control *col = dock->tool_column;
	if (col && col->is_visible_in_tree() && surface && surface->is_visible_in_tree()) {
		const Rect2 sr = surface->get_global_rect();
		const float cy = MAX(pos.y + hh + 8.0f, sr.position.y + 8.0f);
		// (folded: only the small arrow is left, above the zoom bar; it stays at the edge)
		const bool col_open = dock->tool_body && dock->tool_body->is_visible();
		// (folded: a small tab right at the left edge of the view)
		const float cx = col_open ? _left_offset(surface, cy - sr.position.y, col->get_combined_minimum_size().y, true, -8.0f) : 0.0f;
		const Vector2 cpos(sr.position.x + cx, cy);
		if (col->get_global_position() != cpos) {
			col->set_global_position(cpos);
		}
		const Vector2 csize = col->get_combined_minimum_size();
		if (col->get_size() != csize) {
			col->set_size(csize);
		}
		const int at = active_tool();
		bool resized = false;
		for (int i = 0; i < TOOLCOL_COUNT; i++) {
			Button *b = dock->tool_buttons[i];
			if (!b) {
				continue;
			}
			if (b->is_pressed() != (i == at)) {
				b->set_pressed_no_signal(i == at);
			}
			Control *bl = Object::cast_to<Control>(b->get_parent() ? b->get_parent()->get_parent() : nullptr);
			const bool shown = tool_shown(i);
			if (bl && bl->is_visible() != shown) {
				bl->set_visible(shown);
				resized = true;
			}
		}
		if (resized) {
			col->reset_size();
		}
		const bool mesh_ok = get_edited() != nullptr;
		for (int i = TOOLCOL_KNIFE; i <= TOOLCOL_SPIN; i++) {
			dock->tool_buttons[i]->set_disabled(!mesh_ok);
		}
		if (dock->cutter_bar) {
			const bool cut = at == TOOLCOL_CUTTER && !vpaint && !dock->toolbar_folded && show_header;
			if (dock->cutter_bar->is_visible() != cut) {
				dock->cutter_bar->set_visible(cut);
			}
		}
	}
	if (dock->mesh_menu) {
		dock->mesh_menu->set_disabled(mode == MODE_OBJECT || !get_edited());
	}
	if (dock->curve_menu) {
		// (shown while a curve is selected or edited)
		bool curve = _hc() != nullptr && hc_edit;
		for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_selected_nodes()) {
			curve = curve || Object::cast_to<HewnCurve3D>(v) != nullptr;
		}
		Control *blk = Object::cast_to<Control>(dock->curve_menu->get_parent());
		Control *outer = blk ? Object::cast_to<Control>(blk->get_parent()) : nullptr;
		if (outer && outer->is_visible() != curve) {
			outer->set_visible(curve);
		}
	}
}

// The TODO notes of the edited scene: a pin at each, its text beside it (the first lines; done ones dimmed).
void HewnEditorPlugin::_draw_notes(Control *p_overlay, Camera3D *p_cam) {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	Ref<Font> font = p_overlay->get_theme_default_font();
	if (!root || font.is_null()) {
		return;
	}
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const int fs = p_overlay->get_theme_default_font_size();
	for (HewnNote3D *nt : HewnNote3D::all) {
		if (!nt->is_inside_tree() || !nt->is_visible_in_tree() || !(nt == root || root->is_ancestor_of(nt))) {
			continue;
		}
		const Vector3 w = nt->get_global_position();
		if (p_cam->is_position_behind(w)) {
			continue;
		}
		const Vector2 c = p_cam->unproject_position(w);
		Color col = nt->get_color();
		if (nt->get_done()) {
			col.a = 0.4f;
		}
		p_overlay->draw_circle(c, 6.0f * es, Color(0, 0, 0, 0.6f * col.a));
		p_overlay->draw_circle(c, 4.5f * es, col);
		PackedStringArray lines = nt->get_text().split("\n");
		String t;
		for (int64_t i = 0; i < lines.size() && i < 3; i++) {
			String l = lines[i];
			if (l.length() > 48) {
				l = l.substr(0, 46) + "...";
			}
			t += (i ? "\n" : "") + l;
		}
		if (t.strip_edges().is_empty()) {
			continue;
		}
		const Vector2 at = c + Vector2(10.0f * es, -4.0f * es);
		const Vector2 sz = font->get_multiline_string_size(t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs);
		p_overlay->draw_rect(Rect2(at - Vector2(4, font->get_ascent(fs) + 3), sz + Vector2(8, 6)), Color(0.08f, 0.08f, 0.1f, 0.8f * col.a));
		p_overlay->draw_multiline_string(font, at, t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, -1, Color(col.r, col.g, col.b, MAX(0.5f, col.a)));
	}
}

// The Non-planar faces overlay: every face of 4+ corners of the meshes shown whose corners are not on one plane
// (further from the plane through its middle than a hair of its size), filled red.
void HewnEditorPlugin::_draw_nonplanar(Control *p_overlay, Camera3D *p_cam) {
	int budget = 20000;
	const Color red(1.0f, 0.1f, 0.1f, 0.45f), edge(1.0f, 0.15f, 0.15f, 0.95f);
	for (HewnMesh3D *n : _scene_meshes()) {
		if (!n->is_visible_in_tree() || budget <= 0) {
			continue;
		}
		const hewn::Mesh &m = n->get_data();
		const Transform3D g = n->get_global_transform();
		for (const hewn::Face &F : m.faces) {
			const int k = F.size();
			if (k < 4 || --budget <= 0) {
				continue;
			}
			Vector3 c, nrm;
			float size = 0.0f;
			for (int i = 0; i < k; i++) {
				const Vector3 a = m.pos[F.v[i]], b = m.pos[F.v[(i + 1) % k]];
				c += a / (float)k;
				nrm += Vector3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
				size = MAX(size, a.distance_to(b));
			}
			if (nrm.length() < 1e-12f) {
				continue;
			}
			nrm.normalize();
			bool bent = false;
			for (int i = 0; i < k && !bent; i++) {
				bent = std::fabs((m.pos[F.v[i]] - c).dot(nrm)) > MAX(1e-5f, size * 1e-4f);
			}
			if (!bent) {
				continue;
			}
			PackedVector2Array pts;
			bool behind = false;
			for (int i = 0; i < k; i++) {
				const Vector3 w = g.xform(m.pos[F.v[i]]);
				behind = behind || p_cam->is_position_behind(w);
				pts.push_back(p_cam->unproject_position(w));
			}
			if (behind) {
				continue;
			}
			draw_poly_safe(p_overlay, pts, red);
			for (int i = 0; i < k; i++) {
				p_overlay->draw_line(pts[i], pts[(i + 1) % k], edge, 1.5f, true);
			}
		}
	}
}

// The Fixed hotspot faces overlay: the faces whose hotspot was chosen by hand (Randomise and Auto Hotspot leave them),
// slightly green; the ones turned away from the view left out.
void HewnEditorPlugin::_draw_hs_fixed(Control *p_overlay, Camera3D *p_cam) {
	int budget = 20000;
	const Color green(0.3f, 1.0f, 0.35f, 0.22f);
	const Vector3 eye = p_cam->get_global_position();
	for (HewnMesh3D *n : _scene_meshes()) {
		if (!n->is_visible_in_tree() || budget <= 0) {
			continue;
		}
		const hewn::Mesh &m = n->get_data();
		const Transform3D g = n->get_global_transform();
		for (const hewn::Face &F : m.faces) {
			if (!F.hs_fixed || F.hidden || F.size() < 3 || --budget <= 0) {
				continue;
			}
			const Vector3 c = g.xform(hewn::face_center(m, F));
			const Vector3 nrm = g.basis.inverse().transposed().xform(hewn::face_normal(m, F));
			const Vector3 to_eye = p_cam->get_projection() == Camera3D::PROJECTION_ORTHOGONAL ? p_cam->get_global_transform().basis.get_column(2) : eye - c;
			if (nrm.dot(to_eye) <= 0.0f) {
				continue;
			}
			PackedVector2Array pts;
			bool behind = false;
			for (int v : F.v) {
				const Vector3 w = g.xform(m.pos[v]);
				behind = behind || p_cam->is_position_behind(w);
				pts.push_back(p_cam->unproject_position(w));
			}
			if (!behind) {
				draw_poly_safe(p_overlay, pts, green);
			}
		}
	}
}

// ------------------------------------------------------------------ editor settings, heavy operations

namespace {
const char *SET_SUBDIV = "hewn/warnings/subdivide_face_limit";
const char *SET_HEADER = "hewn/interface/show_header";
const char *SET_TOOLS = "hewn/interface/show_tool_column";
const char *SET_TOGGLE = "hewn/interface/toggle_on_color";
const char *SET_KEYS = "hewn/interface/hotkeys";
const char *SET_WRAP = "hewn/interface/wrap_cursor_at_viewport_edges";
const char *SET_REDO = "hewn/interface/last_operation_panel";
const char *SET_PICK = "hewn/selection/pick_distance";
const char *SET_QUAD = "hewn/interface/quad_view";
const char *SET_VSIZE = "hewn/viewport/vertex_size";
const char *SET_VCOL = "hewn/viewport/vertex_color";
const char *SET_VSEL = "hewn/viewport/selected_vertex_color";
const char *SET_ECOL = "hewn/viewport/edge_color";
const char *SET_ESEL = "hewn/viewport/selected_edge_color";
const char *SET_FSEL = "hewn/viewport/selected_face_color";
const char *SET_OCOL = "hewn/viewport/origin_color";
const char *SET_OSIZE = "hewn/viewport/origin_size";
const char *SET_SINGLE = "hewn/viewport/single_color";
const char *SET_WLIGHT = "hewn/viewport/world_space_lighting";
const char *SET_WANGLE = "hewn/viewport/world_space_lighting_angle";
const char *SET_CAV = "hewn/viewport/cavity";
const char *SET_CAV_RIDGE = "hewn/viewport/cavity_ridge";
const char *SET_CAV_VALLEY = "hewn/viewport/cavity_valley";
const char *SET_CAV_TYPE = "hewn/viewport/cavity_type";
const char *SET_GIZMO = "hewn/viewport/show_gizmo";
const char *SET_SMOVE = "hewn/snapping/move_step";
const char *SET_SROT = "hewn/snapping/rotate_step";
const char *SET_SSCALE = "hewn/snapping/scale_step";
const char *SET_MCP = "hewn/mcp/enabled";
const char *SET_MCP_PORT = "hewn/mcp/port";
const char *SET_MCP_TOKEN = "hewn/mcp/token";
const char *SET_MCP_SCRIPTS = "hewn/mcp/allow_scripts";

// 1234567 -> "1 234 567"
String grouped(int64_t p_n) {
	const String s = itos(p_n < 0 ? -p_n : p_n);
	String out;
	for (int64_t i = 0; i < s.length(); i++) {
		if (i > 0 && (s.length() - i) % 3 == 0) {
			out += " ";
		}
		out += String::chr(s[i]);
	}
	return p_n < 0 ? "-" + out : out;
}

void add_editor_setting(EditorSettings *p_es, const char *p_name, const Variant &p_default, Variant::Type p_type, PropertyHint p_hint, const String &p_hint_string) {
	if (!p_es->has_setting(p_name)) {
		p_es->set_setting(p_name, p_default);
	}
	p_es->set_initial_value(p_name, p_default, false);
	Dictionary info;
	info["name"] = p_name;
	info["type"] = p_type;
	info["hint"] = p_hint;
	info["hint_string"] = p_hint_string;
	p_es->add_property_info(info);
}
} // namespace

// The addon's own settings in Editor Settings > Hewn (the editor's, not the project's).
void HewnEditorPlugin::register_editor_settings() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_null()) {
		return;
	}
	add_editor_setting(es.ptr(), SET_SUBDIV, 200000, Variant::INT, PROPERTY_HINT_RANGE, "0,100000000,1000,or_greater");
	add_editor_setting(es.ptr(), SET_HEADER, true, Variant::BOOL, PROPERTY_HINT_NONE, "");
	add_editor_setting(es.ptr(), SET_TOOLS, true, Variant::BOOL, PROPERTY_HINT_NONE, "");
	add_editor_setting(es.ptr(), SET_TOGGLE, Color::html("5486d1"), Variant::COLOR, PROPERTY_HINT_COLOR_NO_ALPHA, "");
	add_editor_setting(es.ptr(), SET_KEYS, true, Variant::BOOL, PROPERTY_HINT_NONE, "");
	add_editor_setting(es.ptr(), SET_WRAP, true, Variant::BOOL, PROPERTY_HINT_NONE, "");
	add_editor_setting(es.ptr(), SET_REDO, true, Variant::BOOL, PROPERTY_HINT_NONE, "");
	add_editor_setting(es.ptr(), SET_PICK, 14.0, Variant::FLOAT, PROPERTY_HINT_RANGE, "4,64,1,suffix:px");
	add_editor_setting(es.ptr(), SET_QUAD, true, Variant::BOOL, PROPERTY_HINT_NONE, "");
	{
		const HwMeshStyle def;
		add_editor_setting(es.ptr(), SET_VSIZE, def.vert_size, Variant::FLOAT, PROPERTY_HINT_RANGE, "1,32,0.5");
		add_editor_setting(es.ptr(), SET_VCOL, def.vert, Variant::COLOR, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_VSEL, def.vert_sel, Variant::COLOR, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_ECOL, def.edge, Variant::COLOR, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_ESEL, def.edge_sel, Variant::COLOR, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_FSEL, def.face_sel, Variant::COLOR, PROPERTY_HINT_NONE, "");
		// (the first defaults of these colors: what was not changed takes the new ones)
		const struct {
			const char *name;
			Color old_def;
			Color def;
		} moved[3] = { { SET_VSEL, Color(1.0f, 0.55f, 0.08f, 1.0f), def.vert_sel }, { SET_ESEL, Color(1.0f, 0.55f, 0.08f, 1.0f), def.edge_sel },
			{ SET_FSEL, Color(1.0f, 0.55f, 0.08f, 0.38f), def.face_sel } };
		for (const auto &mv : moved) {
			if (Color(es->get_setting(mv.name)).is_equal_approx(mv.old_def)) {
				es->set_setting(mv.name, mv.def);
			}
		}
		add_editor_setting(es.ptr(), SET_OCOL, def.origin, Variant::COLOR, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_OSIZE, def.origin_size, Variant::FLOAT, PROPERTY_HINT_RANGE, "1,32,0.5");
		add_editor_setting(es.ptr(), SET_SINGLE, def.single, Variant::COLOR, PROPERTY_HINT_COLOR_NO_ALPHA, "");
		for (const Color &old_def : { Color(0.788f, 0.798f, 0.803f, 1.0f), Color::hex(0xd4d5d6ff) }) {
			if (Color(es->get_setting(SET_SINGLE)).is_equal_approx(old_def)) {
				es->set_setting(SET_SINGLE, def.single); // (an earlier default)
			}
		}
		add_editor_setting(es.ptr(), SET_CAV, def.cavity, Variant::BOOL, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_CAV_RIDGE, def.cavity_ridge, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,2,0.01");
		add_editor_setting(es.ptr(), SET_CAV_VALLEY, def.cavity_valley, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,2,0.01");
		add_editor_setting(es.ptr(), SET_CAV_TYPE, def.cavity_type, Variant::INT, PROPERTY_HINT_ENUM, "World Space,Screen Space,World and Screen");
		add_editor_setting(es.ptr(), SET_WLIGHT, def.world_light, Variant::BOOL, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_WANGLE, def.world_light_angle, Variant::FLOAT, PROPERTY_HINT_RANGE, "-180,180,1");
		add_editor_setting(es.ptr(), SET_GIZMO, true, Variant::BOOL, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_SMOVE, 0.25, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,100,0.001,or_greater");
		add_editor_setting(es.ptr(), SET_SROT, 15.0, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,180,0.1");
		add_editor_setting(es.ptr(), SET_SSCALE, 0.1, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,10,0.001");
	}
	// the MCP server: off until switched on; this computer only
	add_editor_setting(es.ptr(), SET_MCP, false, Variant::BOOL, PROPERTY_HINT_NONE, "");
	add_editor_setting(es.ptr(), SET_MCP_PORT, 7777, Variant::INT, PROPERTY_HINT_RANGE, "1024,65535,1");
	add_editor_setting(es.ptr(), SET_MCP_TOKEN, "", Variant::STRING, PROPERTY_HINT_PASSWORD, "");
	add_editor_setting(es.ptr(), SET_MCP_SCRIPTS, false, Variant::BOOL, PROPERTY_HINT_NONE, "");
	if (!es->is_connected("settings_changed", callable_mp(this, &HewnEditorPlugin::_on_editor_settings))) {
		es->connect("settings_changed", callable_mp(this, &HewnEditorPlugin::_on_editor_settings));
	}
	_on_editor_settings();
}

// The MCP server as the settings say: started, restarted on another port or token, stopped.
void HewnEditorPlugin::_update_mcp() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_null()) {
		return;
	}
	const bool on = es->has_setting(SET_MCP) && (bool)es->get_setting(SET_MCP);
	const int port = es->has_setting(SET_MCP_PORT) ? (int)es->get_setting(SET_MCP_PORT) : 7777;
	const String token = es->has_setting(SET_MCP_TOKEN) ? String(es->get_setting(SET_MCP_TOKEN)) : String();
	const bool scripts = es->has_setting(SET_MCP_SCRIPTS) && (bool)es->get_setting(SET_MCP_SCRIPTS);
	if (!on) {
		if (mcp) {
			mcp->stop();
			mcp->queue_free();
			mcp = nullptr;
			if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
				t->push_toast(T("MCP server stopped"));
			}
		}
		return;
	}
	if (!mcp) {
		mcp = memnew(HewnMCPServer);
		mcp->set_name("HewnMCP");
		add_child(mcp, false, INTERNAL_MODE_BACK);
	}
	mcp->set_allow_scripts(scripts);
	mcp->set_token(token);
	if (mcp->is_listening() && mcp->get_port() == port) {
		return;
	}
	const bool ok = mcp->start(this, port, token, scripts) == OK;
	if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
		t->push_toast(ok ? vformat(T("MCP server: http://127.0.0.1:%d/mcp"), port) : vformat(T("MCP server: port %d is taken (Editor Settings > Hewn > MCP > Port)"), port),
				ok ? EditorToaster::SEVERITY_INFO : EditorToaster::SEVERITY_ERROR);
	}
}

void HewnEditorPlugin::_on_editor_settings() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_null()) {
		return;
	}
	_update_mcp();
	show_header = es->has_setting(SET_HEADER) ? (bool)es->get_setting(SET_HEADER) : true;
	show_tools = es->has_setting(SET_TOOLS) ? (bool)es->get_setting(SET_TOOLS) : true;
	if (dock) {
		// the background of the buttons that are on, in the header and the tool column
		const Color tc = es->has_setting(SET_TOGGLE) ? (Color)es->get_setting(SET_TOGGLE) : Color::html("5486d1");
		if (tc != dock->toggle_color) {
			dock->toggle_color = tc;
			dock->restyle_toggles();
		}
		if (dock->toolbar_root) {
			dock->toolbar_root->set_visible(show_header);
		}
		if (dock->toolbar_fold) {
			dock->toolbar_fold->set_visible(show_header);
		}
		if (dock->tool_column) {
			dock->tool_column->set_visible(show_tools);
		}
	}
	hotkeys_on = es->has_setting(SET_KEYS) ? (bool)es->get_setting(SET_KEYS) : true;
	wrap_cursor_on = es->has_setting(SET_WRAP) ? (bool)es->get_setting(SET_WRAP) : true;
	redo_panel_on = es->has_setting(SET_REDO) ? (bool)es->get_setting(SET_REDO) : true;
	pick_px = es->has_setting(SET_PICK) ? CLAMP((float)es->get_setting(SET_PICK), 4.0f, 64.0f) : 14.0f;
	if (!redo_panel_on && redo_panel) {
		redo_panel->hide();
	}
	{
		// the colors / vertex size of the edited mesh: its overlay made again with them
		HwMeshStyle &st = hw_mesh_style();
		const HwMeshStyle def;
		auto col = [&](const char *p_name, const Color &p_def) { return es->has_setting(p_name) ? Color(es->get_setting(p_name)) : p_def; };
		st.vert_size = es->has_setting(SET_VSIZE) ? CLAMP((float)es->get_setting(SET_VSIZE), 1.0f, 32.0f) : def.vert_size;
		st.vert = col(SET_VCOL, def.vert);
		st.vert_sel = col(SET_VSEL, def.vert_sel);
		st.edge = col(SET_ECOL, def.edge);
		st.edge_sel = col(SET_ESEL, def.edge_sel);
		st.face_sel = col(SET_FSEL, def.face_sel);
		st.origin = col(SET_OCOL, def.origin);
		st.origin_size = es->has_setting(SET_OSIZE) ? CLAMP((float)es->get_setting(SET_OSIZE), 1.0f, 32.0f) : def.origin_size;
		st.single = col(SET_SINGLE, def.single);
		st.world_light = es->has_setting(SET_WLIGHT) ? (bool)es->get_setting(SET_WLIGHT) : def.world_light;
		st.world_light_angle = es->has_setting(SET_WANGLE) ? CLAMP((float)es->get_setting(SET_WANGLE), -180.0f, 180.0f) : def.world_light_angle;
		const bool cavity_was = st.cavity;
		st.cavity = es->has_setting(SET_CAV) ? (bool)es->get_setting(SET_CAV) : def.cavity;
		if (st.cavity != cavity_was) {
			// (the shader with the cavity reads the screen's normals: made only while it is on)
			solid_view_mat[0].unref();
			if (paint_view == 4) {
				apply_paint_view();
			}
		}
		st.cavity_ridge = es->has_setting(SET_CAV_RIDGE) ? CLAMP((float)es->get_setting(SET_CAV_RIDGE), 0.0f, 2.0f) : def.cavity_ridge;
		st.cavity_valley = es->has_setting(SET_CAV_VALLEY) ? CLAMP((float)es->get_setting(SET_CAV_VALLEY), 0.0f, 2.0f) : def.cavity_valley;
		st.cavity_type = es->has_setting(SET_CAV_TYPE) ? CLAMP((int)es->get_setting(SET_CAV_TYPE), 0, 2) : def.cavity_type;
		_apply_solid_params();
		if (dock && dock->solid_angle) {
			dock->solid_angle->set_value_no_signal(st.world_light_angle);
			dock->solid_world->set_pressed_no_signal(st.world_light);
			dock->cavity_check->set_pressed_no_signal(st.cavity);
			dock->cavity_ridge->set_value_no_signal(st.cavity_ridge);
			dock->cavity_valley->set_value_no_signal(st.cavity_valley);
			dock->cavity_type->select(st.cavity_type);
			dock->sync_shading();
		}
		// the gizmo and the snap steps as they were left (the header's buttons write them back)
		if (dock && dock->show_gizmo && es->has_setting(SET_GIZMO) && dock->show_gizmo->is_pressed() != (bool)es->get_setting(SET_GIZMO)) {
			dock->show_gizmo->set_pressed((bool)es->get_setting(SET_GIZMO));
		}
		const struct {
			const char *name;
			HewnDragField *field;
		} steps[3] = { { SET_SMOVE, dock ? dock->snap_move : nullptr }, { SET_SROT, dock ? dock->snap_rotate : nullptr }, { SET_SSCALE, dock ? dock->snap_scale : nullptr } };
		for (const auto &sf : steps) {
			if (sf.field && es->has_setting(sf.name) && std::fabs(sf.field->get_value() - (double)es->get_setting(sf.name)) > 1e-9) {
				sf.field->set_value((double)es->get_setting(sf.name));
			}
		}
		hw_gizmo_clear_cache();
		for (HewnMesh3D *m : edited_meshes()) {
			m->update_gizmos();
		}
		update_overlays();
	}
	callable_mp(this, &HewnEditorPlugin::_sync_quad).call_deferred(); // (not while the editor adds this plugin)
}

// The header's gizmo button and snap steps written into the editor settings (they come back after a restart).
void HewnEditorPlugin::_save_view_state() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_null() || !dock) {
		return;
	}
	auto put = [&](const char *p_name, const Variant &p_value) {
		if (!es->has_setting(p_name) || es->get_setting(p_name) != p_value) {
			es->set_setting(p_name, p_value);
		}
	};
	if (dock->show_gizmo) {
		put(SET_GIZMO, dock->show_gizmo->is_pressed());
	}
	if (dock->snap_move) {
		put(SET_SMOVE, dock->snap_move->get_value());
		put(SET_SROT, dock->snap_rotate->get_value());
		put(SET_SSCALE, dock->snap_scale->get_value());
	}
}

void HewnDock::_on_view_state_changed(double p_unused) {
	if (plugin) {
		plugin->_save_view_state();
	}
}

void HewnDock::_on_cavity(bool p_on) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid()) {
		es->set_setting(SET_CAV, p_on);
	}
}

void HewnDock::_on_cavity_value(double p_unused) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid()) {
		es->set_setting(SET_CAV_RIDGE, cavity_ridge->get_value());
		es->set_setting(SET_CAV_VALLEY, cavity_valley->get_value());
	}
}

void HewnDock::_on_cavity_type(int p_index) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid()) {
		es->set_setting(SET_CAV_TYPE, p_index);
	}
}

void HewnDock::_on_solid_angle(double p_value) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid()) {
		es->set_setting(SET_WANGLE, p_value);
	}
}

void HewnDock::_on_solid_world(bool p_on) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid()) {
		es->set_setting(SET_WLIGHT, p_on);
	}
}

void hw_remove_quad_plugin() {
	EditorPlugins::remove_by_type<HewnQuadPlugin>();
}

// The Quad main screen comes and goes with its setting (and with this plugin).
void HewnEditorPlugin::_sync_quad() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	const bool want = is_inside_tree() && (es.is_null() || !es->has_setting(SET_QUAD) || (bool)es->get_setting(SET_QUAD));
	if (want && !quad_added) {
		EditorPlugins::add_by_type<HewnQuadPlugin>();
	} else if (!want && quad_added) {
		hw_remove_quad_plugin();
	}
	quad_added = want;
}

int64_t HewnEditorPlugin::subdivide_limit() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	return es.is_valid() && es->has_setting(SET_SUBDIV) ? (int64_t)es->get_setting(SET_SUBDIV) : 200000;
}

// How many faces Subdivide adds with these cuts (counted, nothing made): a quad or a triangle becomes (cuts + 1)^2
// faces, a face of more corners (cuts + 1) triangles for each of its sides.
// Catmull-Clark: a face of k corners becomes k quads, then every level 4 times as many (all the faces).
int64_t HewnEditorPlugin::_heavy_added(int p_value) const {
	if (heavy_op != "catmull_clark") {
		return _subdiv_added(p_value);
	}
	int64_t before = 0, after = 0;
	for (HewnMesh3D *n : edited_meshes()) {
		for (const hewn::Face &F : n->get_data().faces) {
			before++;
			after += F.size();
		}
	}
	if (p_value <= 0) {
		return 0;
	}
	for (int l = 1; l < MIN(p_value, 6); l++) {
		after *= 4;
	}
	return after - before;
}

int64_t HewnEditorPlugin::_subdiv_added(int p_cuts) const {
	const int64_t N = (int64_t)CLAMP(p_cuts, 1, 100) + 1;
	int64_t added = 0;
	for (HewnMesh3D *n : edited_meshes()) {
		const hewn::Mesh &m = n->get_data();
		for (int f : n->selection.faces) {
			if (f >= 0 && f < (int)m.faces.size()) {
				const int64_t k = m.faces[f].size();
				added += (k <= 4 ? N * N : k * N) - 1;
			}
		}
	}
	return added;
}

bool HewnEditorPlugin::_heavy_check(const String &p_what, int64_t p_faces, const Callable &p_go, const Callable &p_cancel) {
	const int64_t limit = subdivide_limit();
	if (heavy_pass || limit <= 0 || p_faces < limit) {
		return true; // (go on now)
	}
	heavy_any_go = p_go;
	heavy_any_cancel = p_cancel;
	heavy_any_text = vformat(T("%s would make about %s faces. That can take long and slow the editor down. Do it?"), p_what, String::num_int64(p_faces));
	if (Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) {
		heavy_any_wait = true; // (a value being dragged: asked when it is let go)
	} else {
		callable_mp(this, &HewnEditorPlugin::_heavy_any_popup).call_deferred();
	}
	return false;
}

// A modifier left out for its many faces (set in the Inspector, or with the mesh): asked, as the other operations are.
void HewnEditorPlugin::_check_modifier_heavy() {
	if (!HewnModifier::pending_mesh.is_valid() || (heavy_any_dialog && heavy_any_dialog->is_visible()) || heavy_any_wait ||
			Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) {
		return;
	}
	const ObjectID mid = HewnModifier::pending_mesh, modid = HewnModifier::pending_modifier;
	const int64_t faces = HewnModifier::pending_faces;
	HewnModifier::pending_mesh = ObjectID();
	HewnModifier *mod = Object::cast_to<HewnModifier>(ObjectDB::get_instance(modid));
	if (!mod || !Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(mid))) {
		return;
	}
	_heavy_check(vformat(T("The %s modifier"), T(mod->get_kind_name())), faces, callable_mp(this, &HewnEditorPlugin::_modifier_allow).bind((uint64_t)mid, (uint64_t)modid, faces),
			callable_mp(this, &HewnEditorPlugin::_modifier_refuse).bind((uint64_t)mid));
}

void HewnEditorPlugin::_modifier_allow(uint64_t p_mesh, uint64_t p_mod, int64_t p_faces) {
	HewnModifier *mod = Object::cast_to<HewnModifier>(ObjectDB::get_instance(ObjectID(p_mod)));
	HewnMesh3D *m = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(p_mesh)));
	if (mod && m) {
		mod->set_meta("hw_allowed_faces", p_faces);
		m->rebuild();
	}
}

void HewnEditorPlugin::_modifier_refuse(uint64_t p_mesh) {
	// (the change that asked for it taken back, when it is the last step: a value set in the Inspector)
	HewnMesh3D *m = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(p_mesh)));
	if (!m) {
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	UndoRedo *h = ur->get_history_undo_redo(ur->get_object_history_id(m));
	if (h && h->get_current_action_name().begins_with("Set ")) {
		h->undo();
	} else if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
		t->push_toast(T("The modifier is left out until it makes fewer faces"), EditorToaster::SEVERITY_WARNING);
	}
	HewnModifier::pending_mesh = ObjectID();
}

void HewnEditorPlugin::_heavy_any_popup() {
	heavy_any_wait = false;
	if (!heavy_any_dialog) {
		heavy_any_dialog = memnew(ConfirmationDialog);
		heavy_any_dialog->set_name("HewnHeavyAnyDialog");
		heavy_any_dialog->set_title(T("Many Faces"));
		heavy_any_dialog->get_label()->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		heavy_any_dialog->get_label()->set_custom_minimum_size(Vector2(360, 0) * EditorInterface::get_singleton()->get_editor_scale());
		heavy_any_dialog->connect("confirmed", callable_mp(this, &HewnEditorPlugin::_on_heavy_any_ok));
		heavy_any_dialog->connect("canceled", callable_mp(this, &HewnEditorPlugin::_on_heavy_any_cancel));
		EditorInterface::get_singleton()->get_base_control()->add_child(heavy_any_dialog);
	}
	heavy_any_dialog->set_text(heavy_any_text + "\n\n" + T("From how many faces this is asked: Editor Settings > Hewn > Warnings (0: never)."));
	heavy_any_dialog->reset_size();
	heavy_any_dialog->popup_centered(Vector2i(Vector2(420, 0) * EditorInterface::get_singleton()->get_editor_scale()));
}

void HewnEditorPlugin::_on_heavy_any_ok() {
	const Callable go = heavy_any_go;
	heavy_any_go = Callable();
	heavy_any_cancel = Callable();
	if (go.is_valid()) {
		go.call();
	}
}

void HewnEditorPlugin::_on_heavy_any_cancel() {
	const Callable back = heavy_any_cancel;
	heavy_any_go = Callable();
	heavy_any_cancel = Callable();
	if (back.is_valid()) {
		back.call();
	}
}

void HewnEditorPlugin::_heavy_popup() {
	if (!heavy_dialog) {
		heavy_dialog = memnew(ConfirmationDialog);
		heavy_dialog->set_name("HewnHeavyDialog");
		heavy_dialog->set_title(T("Subdivide: Many Faces"));
		heavy_dialog->set_ok_button_text(T("Subdivide"));
		VBoxContainer *v = memnew(VBoxContainer);
		heavy_dialog->add_child(v);
		heavy_label = memnew(Label);
		heavy_label->set_custom_minimum_size(Vector2(360, 0) * EditorInterface::get_singleton()->get_editor_scale());
		heavy_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		v->add_child(heavy_label);
		HBoxContainer *h = memnew(HBoxContainer);
		v->add_child(h);
		Label *l = memnew(Label);
		heavy_key_label = l;
		l->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		h->add_child(l);
		heavy_cuts = memnew(SpinBox);
		HewnDragField::hook_spinbox(heavy_cuts);
		heavy_cuts->set_min(1);
		heavy_cuts->set_max(100);
		heavy_cuts->set_step(1);
		heavy_cuts->connect("value_changed", callable_mp(this, &HewnEditorPlugin::_on_heavy_cuts));
		h->add_child(heavy_cuts);
		Label *hint = memnew(Label);
		hint->set_text(T("From how many faces this is asked: Editor Settings > Hewn > Warnings (0: never)."));
		hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		hint->set_custom_minimum_size(Vector2(360, 0) * EditorInterface::get_singleton()->get_editor_scale()); // (a wrapped label with no width: a word a line, a tall window)
		hint->set_modulate(Color(1, 1, 1, 0.6f));
		v->add_child(hint);
		heavy_dialog->connect("confirmed", callable_mp(this, &HewnEditorPlugin::_on_heavy_ok));
		heavy_dialog->connect("canceled", callable_mp(this, &HewnEditorPlugin::_on_heavy_cancel));
		EditorInterface::get_singleton()->get_base_control()->add_child(heavy_dialog);
	}
	const bool levels = heavy_op == "catmull_clark";
	heavy_key_label->set_text(T(levels ? "Levels" : "Number of Cuts"));
	heavy_cuts->set_max(levels ? 6 : 100);
	heavy_cuts->set_min(levels ? 0 : 1);
	if (heavy_dialog->is_visible()) {
		heavy_cuts->set_value_no_signal(heavy_cuts_wanted);
		_on_heavy_cuts(heavy_cuts_wanted);
		return;
	}
	heavy_cuts->set_value_no_signal(heavy_cuts_wanted);
	_on_heavy_cuts(heavy_cuts_wanted);
	heavy_dialog->reset_size();
	heavy_dialog->popup_centered(Vector2i(Vector2(420, 0) * EditorInterface::get_singleton()->get_editor_scale()));
}

void HewnEditorPlugin::_on_heavy_cuts(double p_cuts) {
	int64_t faces = 0;
	for (HewnMesh3D *n : edited_meshes()) {
		faces += (int64_t)n->get_data().faces.size();
	}
	const int64_t added = _heavy_added((int)p_cuts);
	heavy_label->set_text(vformat(T(heavy_op == "catmull_clark" ? "Subdivide would add about %s faces (%s in all): it can take a long while and a lot of memory. Change the levels here, or go on." :
																"Subdivide would add about %s faces (%s in all): it can take a long while and a lot of memory. Change the number of cuts here, or go on."),
			grouped(added), grouped(faces + added)));
}

void HewnEditorPlugin::_on_heavy_ok() {
	set_op_param(heavy_op, heavy_op == "catmull_clark" ? "levels" : "cuts", (int)heavy_cuts->get_value());
	heavy_ok = true;
	if (heavy_redo) {
		_redo_last();
		heavy_ok = false;
		_show_redo();
	} else {
		run_op(heavy_op);
		heavy_ok = false;
	}
}

void HewnEditorPlugin::_on_heavy_cancel() {
	heavy_wait = false;
	if (heavy_redo) {
		// the panel back to the value the mesh has now
		set_op_param(heavy_op, heavy_op == "catmull_clark" ? "levels" : "cuts", heavy_prev_cuts);
		_show_redo();
	}
}
