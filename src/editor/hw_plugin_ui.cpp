// Hewn editor - the tool column and the header menus, the panels beside the view, the 3D cursor, the frame by frame work.

#include "hw_plugin_impl.h"

// ------------------------------------------------------------------ tool column / header menus

// Object mode: selecting and measuring; edit mode: also the mesh tools; Vertex Paint: the paint tools and
// the selection (the mask).
bool HewnEditorPlugin::tool_shown(int p_tool) const {
	const bool tp_tool_id = p_tool >= TOOLCOL_TP_DRAW && p_tool <= TOOLCOL_TP_PICKER;
	const bool wp_tool_id = p_tool >= TOOLCOL_WP_DRAW && p_tool <= TOOLCOL_WP_SAMPLE;
	if (tpaint) {
		return tp_tool_id;
	}
	if (wpaint) {
		return wp_tool_id;
	}
	if (tp_tool_id || wp_tool_id) {
		return false;
	}
	if (pose || rig) {
		return p_tool == TOOLCOL_SELECT;
	}
	const bool paint_tool = p_tool >= TOOLCOL_BRUSH && p_tool <= TOOLCOL_NORMALS;
	const bool sculpt_tool_id = p_tool >= TOOLCOL_SC_DRAW && p_tool <= TOOLCOL_SC_DENSITY;
	if (sculpt) {
		return sculpt_tool_id && (p_tool != TOOLCOL_SC_DENSITY || (dock && dock->sculpt_dyntopo && dock->sculpt_dyntopo->is_pressed()));
	}
	if (sculpt_tool_id) {
		return false;
	}
	if (vpaint) {
		return paint_tool || p_tool == TOOLCOL_SELECT || p_tool == TOOLCOL_SEL_CIRCLE || p_tool == TOOLCOL_SEL_LASSO || p_tool == TOOLCOL_CURSOR;
	}
	if (paint_tool) {
		return false;
	}
	if (p_tool == TOOLCOL_CURVE_DRAW || (hc_edit && _hc())) {
		// (a Hewn curve's edit mode: selecting, the cursor and drawing)
		return hc_edit && _hc() && (p_tool == TOOLCOL_SELECT || p_tool == TOOLCOL_CURSOR || p_tool == TOOLCOL_CURVE_DRAW);
	}
	const bool scatter_tool_id = p_tool >= TOOLCOL_SCATTER_ADD && p_tool <= TOOLCOL_SCATTER_THIN;
	if (scatter_tool_id) {
		return get_scatter() != nullptr && mode == MODE_OBJECT;
	}
	if (mode == MODE_OBJECT || !get_edited()) {
		return p_tool == TOOLCOL_SELECT || p_tool == TOOLCOL_SEL_CIRCLE || p_tool == TOOLCOL_CURSOR || p_tool == TOOLCOL_RULER || p_tool == TOOLCOL_CLIP; // (the circle: objects too)
	}
	return true;
}

int HewnEditorPlugin::active_tool() const {
	if (tpaint) {
		return TOOLCOL_TP_DRAW + CLAMP(tp_tool, 0, 4);
	}
	if (wpaint) {
		return TOOLCOL_WP_DRAW + CLAMP(wp_tool, 0, 4);
	}
	if (pose || rig) {
		return TOOLCOL_SELECT;
	}
	if (modal == MODAL_SCULPT) {
		return TOOLCOL_SC_DRAW + CLAMP(sculpt_tool, 0, (int)ST_COUNT - 1);
	}
	if (modal == MODAL_PAINT) {
		return TOOLCOL_BRUSH + CLAMP(paint_tool, 0, 4);
	}
	if (modal == MODAL_NONE && scatter_tool >= 0 && get_scatter()) {
		return TOOLCOL_SCATTER_ADD + scatter_tool;
	}
	if (hc_edit && hc_draw && _hc()) {
		return TOOLCOL_CURVE_DRAW;
	}
	if (modal == MODAL_NONE && (mode != MODE_OBJECT || select_kind == 1) && select_kind > 0) {
		return select_kind == 1 ? TOOLCOL_SEL_CIRCLE : TOOLCOL_SEL_LASSO; // (the circle: objects too)
	}
	switch (modal) {
		case MODAL_CURSOR:
			return TOOLCOL_CURSOR;
		case MODAL_RULER:
			return TOOLCOL_RULER;
		case MODAL_KNIFE:
			return TOOLCOL_KNIFE;
		case MODAL_LOOPCUT:
			return TOOLCOL_LOOPCUT;
		case MODAL_CUTTER:
			return TOOLCOL_CUTTER;
		case MODAL_BISECT:
			return TOOLCOL_BISECT;
		case MODAL_CLIP:
			return TOOLCOL_CLIP;
		case MODAL_SPIN:
			return TOOLCOL_SPIN;
		default:
			return TOOLCOL_SELECT;
	}
}

void HewnEditorPlugin::select_tool(int p_tool) {
	if (p_tool >= TOOLCOL_TP_DRAW && p_tool <= TOOLCOL_TP_PICKER) {
		tp_tool = p_tool - TOOLCOL_TP_DRAW;
		if (!tpaint) {
			enter_tpaint();
		}
		update_overlays();
		return;
	}
	if (p_tool >= TOOLCOL_WP_DRAW && p_tool <= TOOLCOL_WP_SAMPLE) {
		wp_tool = p_tool - TOOLCOL_WP_DRAW;
		if (!wpaint) {
			enter_wpaint();
		}
		update_overlays();
		return;
	}
	if (tpaint || pose || rig || wpaint) {
		return; // (their own tools only)
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	if (p_tool == TOOLCOL_CURVE_DRAW) {
		hc_op("draw");
		update_overlays();
		return;
	}
	if (p_tool == TOOLCOL_CLIP) {
		start_clip();
		return;
	}
	if (p_tool == TOOLCOL_SELECT && hc_draw) {
		hc_draw = false;
		hc_draw_pts.clear();
	}
	const bool mesh_tool = p_tool == TOOLCOL_KNIFE || p_tool == TOOLCOL_LOOPCUT || p_tool == TOOLCOL_CUTTER || p_tool == TOOLCOL_BISECT || p_tool == TOOLCOL_SPIN;
	if (mesh_tool && get_edited() && mode == MODE_OBJECT) {
		set_mode(element_mode); // the mesh tools work in edit mode
	}
	if (scatter_painting) {
		_scatter_end();
	}
	scatter_tool = -1;
	if (p_tool >= TOOLCOL_SCATTER_ADD && p_tool <= TOOLCOL_SCATTER_THIN) {
		scatter_tool = get_scatter() ? p_tool - TOOLCOL_SCATTER_ADD : -1;
		update_overlays();
		return;
	}
	if (p_tool >= TOOLCOL_SC_DRAW && p_tool <= TOOLCOL_SC_DENSITY) {
		start_sculpt_tool(p_tool - TOOLCOL_SC_DRAW);
		update_overlays();
		return;
	}
	if (p_tool >= TOOLCOL_BRUSH && p_tool <= TOOLCOL_NORMALS) {
		start_paint_tool(p_tool - TOOLCOL_BRUSH);
		update_overlays();
		return;
	}
	if (p_tool == TOOLCOL_SELECT || p_tool == TOOLCOL_SEL_CIRCLE || p_tool == TOOLCOL_SEL_LASSO) {
		select_kind = p_tool == TOOLCOL_SEL_CIRCLE ? 1 : p_tool == TOOLCOL_SEL_LASSO ? 2 : 0;
	}
	switch (p_tool) {
		case TOOLCOL_CURSOR:
			start_cursor_tool();
			break;
		case TOOLCOL_RULER:
			start_ruler();
			break;
		case TOOLCOL_KNIFE:
			start_knife();
			break;
		case TOOLCOL_LOOPCUT:
			start_loop_cut();
			break;
		case TOOLCOL_CUTTER:
			start_cutter();
			break;
		case TOOLCOL_BISECT:
			start_bisect();
			break;
		case TOOLCOL_SPIN:
			start_spin_tool();
			break;
		default:
			break;
	}
	if (Camera3D *cam = _viewport_camera()) {
		_focus_viewport(cam);
	}
	update_overlays();
}

void HewnEditorPlugin::run_bar_op(const String &p_op) {
	if (p_op.begins_with("rig:")) {
		rig_object_op(p_op.substr(4)); // (bind / unbind the selected Hewn meshes)
		return;
	}
	if (p_op == "cutter_to_mesh") {
		cutters_to_mesh();
		return;
	}
	Camera3D *cam = _viewport_camera();
	if (p_op == "edit_mode" || p_op.begins_with("modal:") || p_op.begins_with("menu:") || p_op.begins_with("hc:") || p_op.begins_with("cobj:")) {
		_run_menu_op(cam, p_op);
	} else {
		run_op(p_op);
	}
	if (cam) {
		_focus_viewport(cam);
	}
}

void HewnEditorPlugin::_on_mesh_file(const String &p_path) {
	HewnMesh3D *n = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(mesh_file_node));
	if (!n || n->get_mesh().is_null()) {
		return;
	}
	Ref<Mesh> copy = n->get_mesh()->duplicate(true);
	const Error err = ResourceSaver::get_singleton()->save(copy, p_path);
	if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
		t->push_toast(err == OK ? vformat(T("Saved: %s"), p_path) : vformat(T("Could not save %s"), p_path), err == OK ? EditorToaster::SEVERITY_INFO : EditorToaster::SEVERITY_ERROR);
	}
	EditorInterface::get_singleton()->get_resource_filesystem()->scan();
}

// The camera of the viewport used last (or the first one).
Camera3D *HewnEditorPlugin::_viewport_camera() const {
	Control *surface = last_surface ? Object::cast_to<Control>(ObjectDB::get_instance(last_surface)) : nullptr;
	if (!surface) {
		surface = _main_surface();
	}
	return surface ? _camera_for_overlay(surface) : nullptr;
}

void HewnEditorPlugin::set_tool(int p_tool) {
	if (p_tool == TOOL_SELECT) {
		dock->show_gizmo->set_pressed(false);
	} else {
		tool = (Tool)CLAMP(p_tool, 0, 2);
		dock->show_gizmo->set_pressed(true);
		dock->gizmo_move->set_pressed_no_signal(tool == TOOL_MOVE);
		dock->gizmo_rotate->set_pressed_no_signal(tool == TOOL_ROTATE);
		dock->gizmo_scale->set_pressed_no_signal(tool == TOOL_SCALE);
	}
	if (dock) {
		dock->refresh();
	}
	update_overlays();
}

Control *HewnEditorPlugin::_surface_for(Camera3D *p_cam) const {
	Viewport *vp = p_cam ? p_cam->get_viewport() : nullptr;
	Node *holder = vp ? vp->get_parent() : nullptr;
	holder = holder ? holder->get_parent() : nullptr;
	if (!holder) {
		return nullptr;
	}
	for (int i = 0; i < holder->get_child_count(); i++) {
		Control *c = Object::cast_to<Control>(holder->get_child(i));
		if (c && c->get_class() == "Control") {
			return c;
		}
	}
	return nullptr;
}

// Godot draws a zoom / fly-speed bar at the left edge of the viewport for a moment after the mouse
// wheel (or a zoom drag / gesture) is used; our left panels step aside only while it shows.
void HewnEditorPlugin::_note_zoom_bar(const Ref<InputEvent> &p_event) {
	bool zoom = false;
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->is_pressed()) {
		const MouseButton b = mb->get_button_index();
		zoom = b == MOUSE_BUTTON_WHEEL_UP || b == MOUSE_BUTTON_WHEEL_DOWN;
	}
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && mm->get_button_mask().has_flag(MOUSE_BUTTON_MASK_MIDDLE) && (mm->is_ctrl_pressed() || mm->is_alt_pressed())) {
		zoom = true; // zoom / orbit-zoom navigation drags
	}
	if (Object::cast_to<InputEventMagnifyGesture>(p_event.ptr())) {
		zoom = true;
	}
	if (zoom) {
		zoom_bar_until = Time::get_singleton()->get_ticks_msec() + 1700; // Godot shows it for 1.5 s
	}
}

// x of a left panel in the viewport (pixels from its left edge) whose rows p_y .. p_y + p_h are given:
// at the edge, or right of the zoom bar while it shows next to the panel.
float HewnEditorPlugin::_left_offset(Control *p_surface, float p_y, float p_h, bool p_can_move, float p_extra) const {
	const float s = EditorInterface::get_singleton()->get_editor_scale();
	const float edge = 6.0f * s; // a small gap from the edge of the viewport
	if (!p_can_move || !p_surface) {
		return edge;
	}
	// the bar: the middle half of the viewport height, its icon and text under it
	const float h = p_surface->get_size().y;
	const float top = h * 0.25f - 4.0f * s;
	const float bottom = h * 0.75f + 44.0f * s;
	if (p_y > bottom || p_y + p_h < top) {
		return edge;
	}
	return edge + (left_gutter() + (10.0f + p_extra) * s - edge) * zoom_bar_t; // (clear of the bar's "12 m" too)
}

// Bottom left of the viewport; a panel too tall to stay under the tool column goes right of it.
Vector2 HewnEditorPlugin::_left_panel_pos(Control *p_surface, Control *p_panel) const {
	const Vector2 size = p_panel->get_combined_minimum_size();
	const float y = MAX(4.0f, p_surface->get_size().y - size.y - 10.0f);
	float x = _left_offset(p_surface, y, size.y, true, 15.0f); // (the bottom panels: a little more room)
	Control *col = dock ? dock->tool_column : nullptr;
	if (col && col->is_visible_in_tree()) {
		const Rect2 cr(col->get_global_position() - p_surface->get_global_position(), col->get_size());
		if (y < cr.get_end().y + 4.0f && x < cr.get_end().x) {
			x = cr.get_end().x + 6.0f * EditorInterface::get_singleton()->get_editor_scale();
		}
	}
	return Vector2(x, y);
}

// small panels: thin margins, rows close together, long names cut (the whole one in the
// tooltip), so a shape with many parameters does not cover the view.
void HewnEditorPlugin::_compact_panel(PanelContainer *p_panel, GridContainer *p_body) {
	const float s = EditorInterface::get_singleton()->get_editor_scale();
	Ref<StyleBox> sb = EditorInterface::get_singleton()->get_editor_theme()->get_stylebox("panel", "PopupPanel");
	if (sb.is_valid()) {
		Ref<StyleBox> c = sb->duplicate();
		for (int side = 0; side < 4; side++) {
			c->set_content_margin((Side)side, 4.0f * s);
		}
		p_panel->add_theme_stylebox_override("panel", c);
	}
	if (VBoxContainer *v = Object::cast_to<VBoxContainer>(p_panel->get_child(0))) {
		v->add_theme_constant_override("separation", (int)(2 * s));
	}
	p_body->add_theme_constant_override("h_separation", (int)(6 * s));
	p_body->add_theme_constant_override("v_separation", (int)(1 * s));
}

void HewnEditorPlugin::_compact_label(Label *p_label, const String &p_text) {
	const float s = EditorInterface::get_singleton()->get_editor_scale();
	p_label->set_text(p_text);
	p_label->set_tooltip_text(p_text);
	p_label->set_mouse_filter(Control::MOUSE_FILTER_PASS);
	p_label->set_custom_minimum_size(Vector2(60 * s, 0)); // (the whole name: the column is as wide as the longest)
}

void HewnEditorPlugin::_place_left_panels(double p_delta) {
	const bool bar = Time::get_singleton()->get_ticks_msec() < zoom_bar_until;
	const float target = bar ? 1.0f : 0.0f;
	const float step = (float)p_delta * 8.0f; // slides in about a tenth of a second
	zoom_bar_t = zoom_bar_t < target ? MIN(target, zoom_bar_t + step) : MAX(target, zoom_bar_t - step);
	for (Control *p : { (Control *)redo_panel, (Control *)shape_panel, (Control *)obj_panel }) {
		if (!p || !p->is_visible() || !p->get_parent()) {
			continue;
		}
		Control *surface = Object::cast_to<Control>(p->get_parent());
		const Vector2 pos = _left_panel_pos(surface, p);
		if (p->get_position() != pos) {
			p->set_position(pos);
		}
	}
}

void HewnEditorPlugin::_process(double p_delta) {
	_phys_step();
	if (dock) {
		dock->update_block_hover();
	}
	if (heavy_any_wait && !Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) {
		_heavy_any_popup();
	}
	HewnModifier::face_limit = subdivide_limit();
	_check_modifier_heavy();
	if (heavy_wait && !Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) {
		heavy_wait = false;
		_heavy_popup();
	}
	_layout_toolbar();
	_ei_update(false); // (only while its tab is shown)
	if (modal == MODAL_NONE && !editing) {
		// (a move shown with only the positions that ended some other way: the mesh made whole)
		for (HewnMesh3D *em : edited_meshes()) {
			em->finish_positions();
		}
	}
	if (field_drag_pending && !_field_dragging()) {
		_flush_field_drag();
	}
	if (scatter_dock && redo_check_frames == 5) {
		scatter_dock->poll();
	}
	if (!field_drag_pending && !_field_dragging() && !redo_running && ++redo_check_frames >= 10) {
		// (the panel goes when the history moved on some other way too: a button, a menu, the other viewport)
		redo_check_frames = 0;
		_check_redo_valid();
	}
	if (paint_view == 6 && ++paint_view_frames >= 30) {
		// (new objects, a collision changed: the view made again)
		paint_view_frames = 0;
		if ((_collision_view_scan(nullptr) ^ (collision_view_shapes ? _collision_shapes_key() : 0)) != collision_view_key) {
			apply_paint_view();
		}
	} else if ((paint_view == 1 || paint_view >= 3) && ++paint_view_frames >= 30) {
		// (new meshes, another scene: they get the view too)
		paint_view_frames = 0;
		if (paint_view >= 7 && _stretch_scan() != stretch_key) {
			apply_paint_view(); // (the UVs, the shapes or their places changed: the stretch made again)
		}
		size_t shown = 0;
		const std::vector<HewnMesh3D *> all = _scene_meshes();
		for (HewnMesh3D *m : all) {
			shown += std::find(paint_view_ids.begin(), paint_view_ids.end(), ObjectID(m->get_instance_id())) != paint_view_ids.end() ? 1 : 0;
		}
		if (shown != all.size() || paint_view_ids.size() != all.size()) {
			apply_paint_view();
		}
	}
	if (dock) {
		dock->update_snap_highlight();
	}
	_update_library_dim();
	if (vis_click_frames > 0 && --vis_click_frames == 0) {
		_check_visible_click();
	}
	if (issue_pending >= 0) {
		// Check Scene: the problem's faces / vertices once its mesh is the edited one
		HewnMesh3D *pm = issue_pending < (int)issues.size() ? Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(issues[issue_pending].node)) : nullptr;
		if (pm && get_edited() == pm) {
			_issue_apply_selection(issue_pending);
			issue_pending = -1;
		} else if (--issue_pending_frames <= 0) {
			issue_pending = -1;
		}
	}
	{
		HewnMesh3D *n = get_edited();
		const uint64_t want = n && mode != MODE_OBJECT ? (uint64_t)n->get_instance_id() : 0;
		if (want != hiding_id) {
			if (HewnMesh3D *old = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(hiding_id)))) {
				old->set_hiding_faces(false);
			}
			hiding_id = want;
			if (n && want) {
				n->set_hiding_faces(true);
			}
		}
	}
	if (face_grid) {
		face_grid_timer += p_delta;
		if (face_grid_timer > 0.5) {
			face_grid_timer = 0.0;
			_update_face_grid(); // (new meshes, another snap step)
		}
	}
	if (dock && dock->ov_overlap && dock->ov_overlap->is_pressed()) {
		overlap_timer += p_delta;
		if (overlap_timer > 0.25) {
			overlap_timer = 0.0;
			_update_overlaps(); // (the mesh changes in many ways)
			update_overlays();
		}
	} else if (!overlap_same.empty() || !overlap_opposite.empty() || overlap_count) {
		_update_overlaps();
		update_overlays();
	}
	_update_shape_panel();
	_update_obj_panel();
	if (vpaint && (mode == MODE_OBJECT || !get_edited())) {
		exit_vpaint();
	}
	if (vpaint) {
		// the brush hides while the camera flies, shows again after
		const bool rmb = Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT);
		if (rmb != paint_rmb_was) {
			paint_rmb_was = rmb;
			update_overlays();
		}
	}
	if (dock && dock->paint_bar_shown != (vpaint || sculpt || pose || tpaint || rig || wpaint)) {
		dock->show_paint_bar(vpaint || sculpt || pose || tpaint || rig || wpaint);
	}
	if (wpaint && (!wp_mesh() || !wp_skeleton())) {
		exit_wpaint(false);
	}
	if (tpaint) {
		_tp_flush(); // (the painting shown, once a frame)
		if (!tp_mesh()) {
			exit_tpaint(false);
		}
	}
	if (mode == MODE_OBJECT) {
		// the origin dots follow objects moved with Godot's own gizmo
		uint64_t h = 1469598103934665603ull;
		for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_selected_nodes()) {
			if (Node3D *n3 = Object::cast_to<Node3D>(v)) {
				if (n3->is_inside_tree()) {
					const Vector3 o = n3->get_global_transform().origin;
					h = (h ^ (uint64_t)n3->get_instance_id()) * 1099511628211ull;
					h = (h ^ (uint64_t)std::hash<float>()(o.x + o.y * 3.1f + o.z * 7.3f)) * 1099511628211ull;
				}
			}
		}
		if (h != origin_dots_hash) {
			origin_dots_hash = h;
			update_overlays();
		}
	}
	_place_left_panels(p_delta);
	const bool active = drag == DRAG_HANDLE || modal == MODAL_MOVE || modal == MODAL_ROTATE || modal == MODAL_SCALE || modal == MODAL_BEVEL ||
			modal == MODAL_INSET || modal == MODAL_SLIDE || modal == MODAL_EXTRUDE_AMOUNT || modal == MODAL_VALUE || (modal == MODAL_UVT && uvt.drag >= 0);
	if (!active || !dock || !wrap_cursor_on || DisplayServer::get_singleton()->get_name() == "headless") {
		return;
	}
	Control *surface = last_surface ? Object::cast_to<Control>(ObjectDB::get_instance(last_surface)) : nullptr;
	if (!surface || !surface->is_visible_in_tree()) {
		return;
	}
	const Vector2 local = Vector2(DisplayServer::get_singleton()->mouse_get_position()) - surface->get_screen_position();
	const Vector2 size = surface->get_size();
	const float m = 3.0f;
	if (local.x >= m && local.y >= m && local.x <= size.x - m && local.y <= size.y - m) {
		return;
	}
	// Wrap to the opposite side, keeping how far past the edge the cursor went.
	Vector2 to = local;
	if (local.x < m) {
		to.x = size.x - m * 4.0f + MAX(-size.x * 0.5f, local.x - m);
	} else if (local.x > size.x - m) {
		to.x = m * 4.0f + MIN(size.x * 0.5f, local.x - (size.x - m));
	}
	if (local.y < m) {
		to.y = size.y - m * 4.0f + MAX(-size.y * 0.5f, local.y - m);
	} else if (local.y > size.y - m) {
		to.y = m * 4.0f + MIN(size.y * 0.5f, local.y - (size.y - m));
	}
	_warp_to(surface, local, to);
}

// Moves the system cursor; the precise-mouse offsets absorb the jump. Motion events that were queued
// before the warp still carry old positions: _wrap_mouse shifts those by the jump (see warp_pending).
void HewnEditorPlugin::_warp_to(Control *p_surface, const Vector2 &p_from, const Vector2 &p_to) {
	const Vector2 jump = p_to - p_from;
	wrap_offset -= jump;
	if (prec_active) {
		prec_raw0 += jump;
	} else {
		prec_offset -= jump;
	}
	warp_pending = true;
	warp_jump = jump;
	warp_target = p_to;
	warp_ms = Time::get_singleton()->get_ticks_msec();
	Input::get_singleton()->warp_mouse(p_surface->get_global_rect().position + p_to);
}

// Cursor wrapping during operations: at a viewport edge the system cursor jumps to the
// opposite edge; the precise-mouse offsets absorb the jump so the operation continues smoothly.
Vector2 HewnEditorPlugin::_wrap_mouse(Camera3D *p_cam, const Vector2 &p_raw) {
	Control *surface = _surface_for(p_cam);
	if (!surface || !wrap_cursor_on || DisplayServer::get_singleton()->get_name() == "headless") {
		return p_raw;
	}
	if (warp_pending) {
		// An event from before the last warp is nearer to where the cursor was than to where it went.
		const Vector2 before = warp_target - warp_jump;
		if (p_raw.distance_to(before) < p_raw.distance_to(warp_target) && Time::get_singleton()->get_ticks_msec() - warp_ms < 500) {
			return p_raw + warp_jump;
		}
		warp_pending = false;
	}
	const Vector2 size = surface->get_size();
	const float m = 3.0f;
	Vector2 to = p_raw;
	if (p_raw.x < m) {
		to.x = size.x - m * 3.0f;
	} else if (p_raw.x > size.x - m) {
		to.x = m * 3.0f;
	}
	if (p_raw.y < m) {
		to.y = size.y - m * 3.0f;
	} else if (p_raw.y > size.y - m) {
		to.y = m * 3.0f;
	}
	if (to == p_raw) {
		return p_raw;
	}
	_warp_to(surface, p_raw, to);
	return to;
}

// The 3D cursor onto the surface under the mouse (or the ground), lying on it.
void HewnEditorPlugin::_place_cursor(Camera3D *p_cam, const Vector2 &p_pos, bool p_ctrl) {
	Vector3 o, d, hit, nrm;
	_ray(p_cam, p_pos, o, d);
	if (_raycast_scene(o, d, hit, nrm)) {
		cursor_pos = hit;
		// Y along the surface normal, X as close to world X as possible
		Vector3 y = nrm.normalized();
		Vector3 x = Vector3(1, 0, 0) - y * y.x;
		if (x.length() < 0.3f) {
			x = Vector3(0, 0, 1) - y * y.z;
		}
		x.normalize();
		cursor_basis = Basis();
		cursor_basis.set_column(0, x);
		cursor_basis.set_column(1, y);
		cursor_basis.set_column(2, x.cross(y).normalized());
	} else if (std::fabs(d.y) > 1e-6f && -o.y / d.y > 0.0f) {
		cursor_pos = o + d * (-o.y / d.y);
		cursor_basis = Basis();
	}
	// snapping (Ctrl: the other way): onto a vertex / edge / center of the scene, else in grid steps
	snap_mark_valid = false;
	tool_ctrl = p_ctrl;
	if (snapping_active(p_ctrl)) {
		Vector3 sp;
		const float sm = (float)dock->snap_move->get_value();
		if ((dock->snap_targets & HewnDock::SNAP_POINTS) && _snap_target(p_cam, p_pos, sp)) {
			cursor_pos = sp;
			snap_mark = sp;
			snap_mark_valid = true;
			snap_mark_geom = true;
			snap_mark_kind = last_snap_kind;
		} else if ((dock->snap_targets & HewnDock::SNAP_STEPS) && sm > 0.0f) {
			cursor_pos = (cursor_pos / sm).round() * sm;
			snap_mark = cursor_pos;
			snap_mark_valid = true;
			snap_mark_geom = false;
		}
	}
	update_overlays();
}

bool HewnEditorPlugin::_mouse_near_cursor(Camera3D *p_cam) const {
	if (!p_cam || p_cam->is_position_behind(cursor_pos)) {
		return false;
	}
	if (mode != MODE_OBJECT && (hover_vert >= 0 || hover_edge.a >= 0)) {
		return false; // (a vertex / edge under the mouse is what G moves)
	}
	return p_cam->unproject_position(cursor_pos).distance_to(mouse_pos) < 9.0f * EditorInterface::get_singleton()->get_editor_scale();
}

void HewnEditorPlugin::_start_cursor_xf(Camera3D *p_cam, int p_kind) {
	if (!p_cam) {
		return;
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	modal = MODAL_CURSOR;
	cursor_once = false;
	cursor_xf = p_kind;
	_gather_snap_candidates(false);
	cursor_xf_axis = -1;
	cursor_xf_pos0 = cursor_pos;
	cursor_xf_basis0 = cursor_basis;
	cursor_xf_mouse0 = mouse_pos;
	cursor_xf_t0 = 0.0f;
	update_overlays();
}

void HewnEditorPlugin::_cursor_xf_update(Camera3D *p_cam, bool p_ctrl) {
	const bool snap = snapping_active(p_ctrl);
	if (cursor_xf == 1) {
		if (cursor_xf_axis < 0) {
			// free: onto the surface under the mouse (its vertices / edges with snapping), on the ground else
			const Basis keep = cursor_basis;
			_place_cursor(p_cam, mouse_pos, p_ctrl);
			cursor_basis = keep;
		} else {
			const Vector3 ax = AXES[cursor_xf_axis];
			float t = _axis_param(p_cam, mouse_pos, cursor_xf_pos0, ax) - cursor_xf_t0;
			if (snap && dock) {
				const float st = MAX(1e-4f, (float)dock->snap_move->get_value());
				t = std::round(t / st) * st;
			}
			cursor_pos = cursor_xf_pos0 + ax * t;
		}
	} else if (cursor_xf == 2) {
		const Vector2 c = p_cam->is_position_behind(cursor_xf_pos0) ? Vector2() : p_cam->unproject_position(cursor_xf_pos0);
		float a = (mouse_pos - c).angle() - (cursor_xf_mouse0 - c).angle();
		const Vector3 to_eye = p_cam->get_global_position() - cursor_xf_pos0;
		Vector3 ax = cursor_xf_axis < 0 ? p_cam->get_global_transform().basis.get_column(2).normalized() : AXES[cursor_xf_axis];
		if (ax.dot(to_eye) < 0.0f) {
			ax = -ax; // (turning with the mouse on screen: the axis toward the eye)
		}
		a = -a; // (screen y goes down)
		if (snap && dock) {
			const float st = Math::deg_to_rad(MAX(0.1f, (float)dock->snap_rotate->get_value()));
			a = std::round(a / st) * st;
		}
		cursor_basis = Basis(ax, a) * cursor_xf_basis0;
	}
	update_overlays();
}

void HewnEditorPlugin::_on_cursor_menu(int p_id) {
	if (p_id == 4 || p_id == 5) {
		SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(0);
		_start_cursor_xf(vp ? vp->get_camera_3d() : nullptr, p_id == 4 ? 1 : 2);
		return;
	}
	if (hc_edit && _hc() && (p_id == 0 || p_id == 3)) {
		hc_op(p_id == 0 ? "snap_cursor_sel" : "snap_sel_cursor"); // (a curve's points being edited)
		update_overlays();
		return;
	}
	HewnMesh3D *n = get_edited();
	if (p_id == 1) {
		cursor_pos = Vector3();
	} else if (p_id == 0) {
		Vector3 p;
		if (_pivot(p)) {
			cursor_pos = p;
		}
	} else if (p_id == 2 && n) {
		cursor_pos = n->get_global_position();
	} else if (p_id == 3 && n) {
		if (mode == MODE_OBJECT) {
			Transform3D before = n->get_global_transform();
			Transform3D after = before;
			after.origin = cursor_pos;
			EditorUndoRedoManager *ur = get_undo_redo();
			ur->create_action("Hewn: Selection to Cursor", UndoRedo::MERGE_DISABLE, n);
			ur->add_do_method(n, "set_global_transform", after);
			ur->add_undo_method(n, "set_global_transform", before);
			ur->commit_action();
		} else {
			Vector3 piv;
			if (_pivot(piv)) {
				begin_edit("Selection to Cursor");
				const Vector3 delta = n->get_global_transform().affine_inverse().basis.xform(cursor_pos - piv);
				for (int v : _selected_verts()) {
					n->get_data().pos[v] += delta;
				}
				n->commit(true);
				end_edit();
			}
		}
	}
	update_overlays();
}

void HewnEditorPlugin::_draw_cursor(Control *p_overlay, Camera3D *p_cam) {
	if (p_cam->is_position_behind(cursor_pos)) {
		return;
	}
	const Vector2 c = p_cam->unproject_position(cursor_pos);
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	// moved / turned along an axis: that axis through where it started, in its color
	if (modal == MODAL_CURSOR && cursor_xf != 0 && cursor_xf_axis >= 0) {
		const Vector3 ax = AXES[cursor_xf_axis];
		const Vector3 a = cursor_xf_pos0 - ax * 1000.0f, b = cursor_xf_pos0 + ax * 1000.0f;
		const Color ac = cursor_xf_axis == 0 ? Color(0.95f, 0.3f, 0.3f) : cursor_xf_axis == 1 ? Color(0.45f, 0.9f, 0.3f) : Color(0.35f, 0.55f, 1.0f);
		// (clipped to the part in front of the camera)
		const Vector3 eye = p_cam->get_global_position(), fwd = -p_cam->get_global_transform().basis.get_column(2).normalized();
		const float near = p_cam->get_near() * 1.5f, da = (a - eye).dot(fwd), db = (b - eye).dot(fwd);
		Vector3 ca = a, cb = b;
		if (da < near && db >= near) {
			ca = a.lerp(b, (near - da) / (db - da));
		} else if (db < near && da >= near) {
			cb = b.lerp(a, (near - db) / (da - db));
		}
		if (!p_cam->is_position_behind(ca) && !p_cam->is_position_behind(cb)) {
			p_overlay->draw_line(p_cam->unproject_position(ca), p_cam->unproject_position(cb), ac, 1.0f, true);
		}
	}
	const int segs = 12;
	const float rad = 8.0f * es;
	for (int i = 0; i < segs; i++) {
		const float a0 = (float)Math::PI * 2.0f * i / segs, a1 = (float)Math::PI * 2.0f * (i + 1) / segs;
		p_overlay->draw_line(c + Vector2(std::cos(a0), std::sin(a0)) * rad, c + Vector2(std::cos(a1), std::sin(a1)) * rad,
				i % 2 ? Color(1, 1, 1) : Color(0.9f, 0.15f, 0.15f), 1.5f, true);
	}
	// Cross hair along the cursor's own X / Z axes, in 3D (it lies on the surface it was placed on); its ends
	// in the axes' colors.
	const float wpp = _world_per_pixel(p_cam, cursor_pos);
	for (int axis = 0; axis < 3; axis += 2) {
		const Vector3 d = cursor_basis.get_column(axis).normalized();
		const Color ac = axis == 0 ? Color(0.95f, 0.3f, 0.3f) : Color(0.35f, 0.55f, 1.0f);
		for (int sgn = -1; sgn <= 1; sgn += 2) {
			const Vector3 a = cursor_pos + d * (sgn * 4.0f * wpp), m = cursor_pos + d * (sgn * 12.0f * wpp), b = cursor_pos + d * (sgn * 18.0f * wpp);
			if (!p_cam->is_position_behind(a) && !p_cam->is_position_behind(b)) {
				p_overlay->draw_line(p_cam->unproject_position(a), p_cam->unproject_position(m), Color(0, 0, 0, 0.85f), 1.0f, true);
				p_overlay->draw_line(p_cam->unproject_position(m), p_cam->unproject_position(b), ac, 1.0f, true);
			}
		}
	}
}

// Our clicks never reach Godot's viewport, so it would not take the keyboard focus back after the user
// clicked another panel; then the digits and Q/W/E/R went to the editor shortcuts again.
void HewnEditorPlugin::_focus_viewport(Camera3D *p_cam) {
	Control *c = _surface_for(p_cam);
	if (c && !c->has_focus()) {
		c->grab_focus();
	}
}
