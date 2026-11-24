// Hewn editor - the view's input: clicks, keys, arrows, the right button.

#include "hw_plugin_impl.h"

// A right click (not a free look) in object mode on the selected meshes or curves: their context menu.
int32_t HewnEditorPlugin::_object_rmb(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	// a right click (not a free look) on the selected meshes: the object context menu
	Ref<InputEventMouseButton> omb = p_event;
	if (omb.is_valid() && omb->get_button_index() == MOUSE_BUTTON_RIGHT && !omb->is_shift_pressed() && !omb->is_alt_pressed()) {
		if (omb->is_pressed()) {
			rmb_pending = true;
			rmb_moved = 0.0f;
			rmb_press_ms = Time::get_singleton()->get_ticks_msec();
			rmb_press_pos = omb->get_position();
			// (near a point of a selected curve Godot's own path editor would delete the point: the click is ours, its menu)
			if (!hc_edit) {
				for (const Variant &cv : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
					HewnCurve3D *hc = Object::cast_to<HewnCurve3D>(cv);
					if (!hc) {
						continue;
					}
					const Transform3D g = hc->get_global_transform();
					for (const hewn::curves::Spline &sp : hc->get_splines_const()) {
						for (const hewn::curves::CPoint &cp : sp.pts) {
							const Vector3 w = g.xform(cp.pos);
							if (!p_camera->is_position_behind(w) && p_camera->unproject_position(w).distance_to(omb->get_position()) < 16.0f) {
								return AFTER_GUI_INPUT_STOP;
							}
						}
					}
				}
			}
		} else if (rmb_pending) {
			rmb_pending = false;
			bool any_curve = false;
			for (const Variant &cv : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
				any_curve = any_curve || Object::cast_to<HewnCurve3D>(cv) != nullptr;
			}
			if (Time::get_singleton()->get_ticks_msec() - rmb_press_ms < 300 && rmb_moved < 6.0f && (!_selected_meshes().empty() || (any_curve && !hc_edit))) {
				mouse_pos = rmb_press_pos;
				callable_mp(this, &HewnEditorPlugin::_context_menu_deferred).call_deferred(p_camera->get_instance_id());
			}
		}
	}
	Ref<InputEventMouseMotion> omm = p_event;
	if (omm.is_valid() && rmb_pending) {
		rmb_moved += omm->get_relative().length();
	}
	return AFTER_GUI_INPUT_PASS;
}

int32_t HewnEditorPlugin::_forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	{
		Ref<InputEventMouseButton> pb = p_event;
		if (pb.is_valid() && pb->is_pressed() && pb->get_button_index() == MOUSE_BUTTON_LEFT) {
			plain_press = false; // (set again below when no tool takes the press)
		}
	}
	if (modal == MODAL_NONE && (prof3d_drag >= 0 || _prof3d_any())) {
		Ref<InputEventMouse> pm = p_event;
		Ref<InputEventKey> pk = p_event;
		if (pm.is_valid() || (pk.is_valid() && prof3d_sel_point >= 0)) {
			const int32_t r = _profile_in_view_input(p_camera, p_event);
			if (r != AFTER_GUI_INPUT_PASS) {
				return r;
			}
		}
	}
	if (modal == MODAL_NONE && (bridge_drag >= 0 || _bridge_handles_live())) {
		const int32_t r = _bridge_handle_input(p_camera, p_event);
		if (r != AFTER_GUI_INPUT_PASS) {
			return r;
		}
	}
	Ref<InputEventKey> fk = p_event;
	if (fk.is_valid() && Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT) && modal != MODAL_UVT) {
		// the right button flies the camera (WASD / QE): every key is Godot's then (W must not run the action)
		if (fk->is_pressed()) {
			rmb_pending = false; // (flying: no context menu on release)
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (fk.is_valid() && !hotkeys_on && modal == MODAL_NONE && !remapping) {
		return AFTER_GUI_INPUT_PASS; // (Editor Settings > Hewn > Interface > Hotkeys off: keys inside a running tool still work)
	}
	if (wpaint) {
		const int32_t wr = _wp_input(p_camera, p_event);
		if (wr >= 0) {
			if (wr == AFTER_GUI_INPUT_STOP) {
				if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
					vp->set_input_as_handled();
				}
			}
			return wr;
		}
	}
	if (rig) {
		const int32_t rr = _rig_input(p_camera, p_event);
		if (rr >= 0) {
			if (rr == AFTER_GUI_INPUT_STOP) {
				if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
					vp->set_input_as_handled();
				}
			}
			return rr;
		}
	}
	if (tpaint) {
		const int32_t tr = _tp_input(p_camera, p_event);
		if (tr >= 0) {
			if (tr == AFTER_GUI_INPUT_STOP) {
				if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
					vp->set_input_as_handled();
				}
			}
			return tr;
		}
	}
	if (pose) {
		const int32_t pr = _pose_input(p_camera, p_event);
		if (pr >= 0) {
			if (pr == AFTER_GUI_INPUT_STOP) {
				if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
					vp->set_input_as_handled();
				}
			}
			return pr;
		}
	} else if (fk.is_valid() && fk->is_pressed() && !fk->is_echo() && key_code(fk) == KEY_TAB && !fk->is_ctrl_pressed() && !fk->is_alt_pressed() &&
			!fk->is_shift_pressed() && !fk->is_meta_pressed() && modal == MODAL_NONE && mode == MODE_OBJECT && !get_edited() && !curve_id.is_valid()) {
		// Tab on a skeleton (or a skinned mesh, a character holding one): its Pose mode
		const TypedArray<Node> tsel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
		if (tsel.size() == 1 && pose_find_skeleton(Object::cast_to<Node>(tsel[0])) && enter_pose()) {
			if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
				vp->set_input_as_handled();
			}
			return AFTER_GUI_INPUT_STOP;
		}
	}
	if (fk.is_valid() && key_code(fk) == KEY_Y && !fk->is_ctrl_pressed() && !fk->is_alt_pressed() && !fk->is_shift_pressed() && !fk->is_meta_pressed() &&
			(modal == MODAL_SHAPE || modal == MODAL_POLY || modal == MODAL_KNIFE || modal == MODAL_CUTTER || modal == MODAL_CLIP || modal == MODAL_PREFAB ||
					modal == MODAL_BISECT || modal == MODAL_LOOPCUT || modal == MODAL_RULER || (modal == MODAL_CURSOR && cursor_xf == 0) || modal == MODAL_TSNAP)) {
		// Y (Godot's snapping on / off) works while drawing too (in G / R / S it is the Y axis)
		if (fk->is_pressed() && !fk->is_echo() && dock && dock->snap_enabled) {
			dock->snap_enabled->set_pressed(!dock->snap_enabled->is_pressed());
			update_overlays();
		}
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (fk.is_valid() && fk->is_pressed() && !fk->is_echo() && (key_code(fk) == KEY_KP_DIVIDE || key_code(fk) == KEY_SLASH) && !fk->is_ctrl_pressed() &&
			!fk->is_alt_pressed() && !fk->is_shift_pressed() && modal == MODAL_NONE && hc_xf == 0 && fk->get_unicode() != '.' && fk->get_unicode() != ',') {
		// (not while a value is typed: / divides there; another layout's point on that key: a point)
		if (isolated) {
			set_isolate(false);
		} else if (_anything_selected()) {
			set_isolate(true); // local view: only the selection shown, the camera on it
			if (mode == MODE_OBJECT) {
				focus_selection(p_camera); // (Godot's focus goes to the objects' origins: not in edit mode / Vertex Paint)
			}
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (fk.is_valid() && fk->is_pressed() && !fk->is_echo() && (key_code(fk) == KEY_PERIOD || key_code(fk) == KEY_KP_PERIOD) && !fk->is_ctrl_pressed() &&
			!fk->is_alt_pressed() && !fk->is_shift_pressed() && modal == MODAL_NONE) {
		if (mode == MODE_OBJECT && _anything_selected()) {
			focus_selection(p_camera); // View Selected (edit mode / Vertex Paint: Godot's focus would go to the origin)
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (fk.is_valid() && fk->is_pressed() && !fk->is_echo() && key_code(fk) == KEY_Z && fk->is_alt_pressed() && fk->is_shift_pressed() && !fk->is_ctrl_pressed() &&
			dock && dock->overlays) {
		// Shift+Alt+Z: the overlays on / off, also in the middle of an operation
		dock->overlays->set_pressed(!dock->overlays->is_pressed());
		update_overlays();
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (fk.is_valid() && fk->is_pressed() && !fk->is_echo() && key_code(fk) == KEY_Z && fk->is_alt_pressed() && !fk->is_ctrl_pressed() && !fk->is_shift_pressed() &&
			(modal != MODAL_NONE || mode == MODE_OBJECT) && modal != MODAL_UVT) {
		// Alt+Z: X-ray also while drawing (a cut through a wall) and in object mode
		set_xray(!xray_on);
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (scatter_tool >= 0 && get_scatter() && mode == MODE_OBJECT && modal == MODAL_NONE) {
		const int32_t sr = _scatter_input(p_camera, p_event);
		if (sr >= 0) {
			if (sr == AFTER_GUI_INPUT_STOP) {
				if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
					vp->set_input_as_handled();
				}
			}
			return sr;
		}
	}
	if (curve_id.is_valid() && mode == MODE_OBJECT && modal == MODAL_NONE && _hc()) {
		const int32_t cr = _hc_input(p_camera, p_event);
		if (cr >= 0) {
			if (cr == AFTER_GUI_INPUT_STOP) {
				if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
					vp->set_input_as_handled();
				}
			}
			return cr;
		}
		if (hc_edit) {
			Ref<InputEventMouseButton> cmb = p_event;
			if (cmb.is_valid() && cmb->get_button_index() == MOUSE_BUTTON_LEFT) {
				return AFTER_GUI_INPUT_STOP; // (Godot's own path handles stay out of it)
			}
		}
	} else if (curve_id.is_valid() && mode == MODE_OBJECT && modal == MODAL_NONE) {
		const int32_t cr = _curve_input(p_camera, p_event);
		if (cr >= 0) {
			if (cr == AFTER_GUI_INPUT_STOP) {
				if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
					vp->set_input_as_handled();
				}
			}
			return cr;
		}
		if (curve_edit) {
			Ref<InputEventMouseButton> cmb = p_event;
			if (cmb.is_valid() && cmb->get_button_index() == MOUSE_BUTTON_LEFT) {
				return AFTER_GUI_INPUT_STOP; // (Godot's own path handles stay out of it)
			}
		}
	}
	if (fk.is_valid()) {
		const int32_t rk = _remap_key(p_camera, fk); // (the user's keys from Editor Settings > Shortcuts)
		if (rk >= 0) {
			return rk;
		}
	}
	const int32_t tr = _texture_input(p_camera, p_event);
	if (tr >= 0) {
		if (tr == AFTER_GUI_INPUT_STOP) {
			if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
				vp->set_input_as_handled();
			}
		}
		return tr;
	}
	Ref<InputEventKey> wk = p_event;
	// (also while a tool runs: the knife, bisect, box cut, spin...; not in the middle of a move / turn / scale)
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_G && wk->is_alt_pressed() && !wk->is_ctrl_pressed() && !wk->is_shift_pressed() &&
			modal != MODAL_MOVE && modal != MODAL_ROTATE && modal != MODAL_SCALE && modal != MODAL_UVT && modal != MODAL_UV) {
		set_face_grid(!face_grid); // Alt+G: the grid on faces
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && (key_code(wk) == KEY_BRACKETLEFT || key_code(wk) == KEY_BRACKETRIGHT) && !wk->is_ctrl_pressed() &&
			!wk->is_alt_pressed() && !wk->is_shift_pressed() && !wk->is_meta_pressed() && !vpaint) {
		// [ / ]: the grid step half / double, everywhere (also while drawing)
		if (dock && dock->snap_move) {
			const double v = dock->snap_move->get_value();
			dock->snap_move->set_value(key_code(wk) == KEY_BRACKETRIGHT ? v * 2.0 : v * 0.5);
		}
		if (face_grid) {
			_update_face_grid();
		}
		update_overlays();
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_B && !wk->is_shift_pressed() && !wk->is_ctrl_pressed() && !wk->is_alt_pressed() &&
			!wk->is_meta_pressed() && (modal == MODAL_NONE || (modal == MODAL_SHAPE && shape_cut)) && drag == DRAG_NONE) {
		// B: draw a box (also instead of a cut being drawn); in edit mode it becomes part of the edited mesh. (During G / R / S, B is
		// the snap base: that is the modal's key.)
		start_shape_tool(hewn::shapes::CUBE);
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_X && wk->is_shift_pressed() && !wk->is_ctrl_pressed() && !wk->is_alt_pressed() &&
			modal == MODAL_NONE) {
		// Shift+X: the 3D cursor onto the grid / surface with one click
		start_cursor_tool();
		cursor_once = true;
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_X && wk->is_alt_pressed() && !wk->is_ctrl_pressed() && !wk->is_shift_pressed() &&
			modal == MODAL_NONE) {
		start_clip(); // Alt+X: Clip
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_A && wk->is_shift_pressed() && !wk->is_ctrl_pressed() && !wk->is_alt_pressed() &&
			modal == MODAL_NONE && drag == DRAG_NONE) {
		_add_menu(p_camera); // Shift+A: the Add menu under the mouse
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_C && !wk->is_shift_pressed() && !wk->is_ctrl_pressed() && !wk->is_alt_pressed() &&
			!wk->is_meta_pressed() && (modal == MODAL_NONE || (modal == MODAL_SHAPE && !shape_cut)) && drag == DRAG_NONE) {
		if (modal == MODAL_SHAPE) {
			cancel_tool(); // (the box builder gives way to the cutter)
		}
		start_cut_tool(1); // C: Box Cut
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (aim_light.is_valid()) {
		// a spot light's Aim at a Point: the click turns it toward the surface there, Esc / RMB: never mind
		// (it turns toward the point under the mouse while it moves)
		Node3D *light = Object::cast_to<Node3D>(ObjectDB::get_instance(aim_light));
		Ref<InputEventMouseButton> amb = p_event;
		if (!light || (wk.is_valid() && wk->is_pressed() && key_code(wk) == KEY_ESCAPE) ||
				(amb.is_valid() && amb->is_pressed() && amb->get_button_index() == MOUSE_BUTTON_RIGHT)) {
			if (light) {
				light->set_global_transform(aim_before);
			}
			aim_light = ObjectID();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		auto aimed = [&](const Vector2 &p_at, Transform3D &r_xf) {
			Vector3 o, n, u, v;
			if (!_surface_frame(p_camera, p_at, o, n, u, v) || o.distance_to(aim_before.origin) <= 1e-4f) {
				return false;
			}
			const Vector3 dir = (o - aim_before.origin).normalized();
			const Vector3 up = std::fabs(dir.y) > 0.99f ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
			r_xf = aim_before.looking_at(o, up);
			r_xf.basis.scale_local(aim_before.basis.get_scale());
			return true;
		};
		if (amb.is_valid() && amb->is_pressed() && amb->get_button_index() == MOUSE_BUTTON_LEFT) {
			Transform3D after;
			if (aimed(amb->get_position(), after)) {
				light->set_global_transform(aim_before);
				EditorUndoRedoManager *ur = get_undo_redo();
				ur->create_action("Hewn: Aim Light");
				ur->add_do_method(light, "set_global_transform", after);
				ur->add_undo_method(light, "set_global_transform", aim_before);
				ur->commit_action();
			} else {
				light->set_global_transform(aim_before);
			}
			aim_light = ObjectID();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		Ref<InputEventMouseMotion> amm = p_event;
		if (amm.is_valid()) {
			Transform3D now;
			if (aimed(amm->get_position(), now)) {
				light->set_global_transform(now);
			}
			update_overlays();
		}
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_W && modal == MODAL_NONE && !wk->is_ctrl_pressed() &&
			!wk->is_alt_pressed() && !wk->is_shift_pressed() && !wk->is_meta_pressed() && _pending_ready()) {
		_run_pending();
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_J && modal == MODAL_NONE && mode == MODE_OBJECT && (wk->is_ctrl_pressed() || wk->is_meta_pressed()) &&
			!wk->is_alt_pressed() && !wk->is_shift_pressed() && _selected_meshes().size() >= 2) {
		run_op("merge_objects"); // Ctrl+J: the selected objects into one
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_W && modal == MODAL_NONE && mode == MODE_OBJECT && !wk->is_ctrl_pressed() &&
			!wk->is_alt_pressed() && !wk->is_shift_pressed() && !wk->is_meta_pressed() && drag == DRAG_NONE) {
		// Repeat Prefab: one placed scene selected - W places another one of it
		const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
		Node3D *one = sel.size() == 1 ? Object::cast_to<Node3D>(sel[0]) : nullptr;
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		if (one && one != root && !one->get_scene_file_path().is_empty() && !Object::cast_to<HewnMesh3D>(one)) {
			start_prefab(one->get_scene_file_path());
			if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
				vp->set_input_as_handled();
			}
			return AFTER_GUI_INPUT_STOP;
		}
	}
	// the selection's undo step: before the event (a press: until its release)
	Ref<InputEventMouseButton> sel_mb = p_event;
	Ref<InputEventKey> sel_k = p_event;
	const bool sel_watch = mode != MODE_OBJECT && modal == MODAL_NONE && !editing &&
			((sel_mb.is_valid() && sel_mb->get_button_index() == MOUSE_BUTTON_LEFT) || (sel_k.is_valid() && sel_k->is_pressed() && !sel_k->is_echo()));
	if (sel_watch && (!sel_undo_pending || (sel_mb.is_valid() && sel_mb->is_pressed()))) {
		sel_undo_before = _sel_snapshot();
		sel_undo_version = _history_version();
		sel_undo_pending = true;
	}
	const int32_t r = _input_impl(p_camera, p_event);
	if (sel_undo_pending && (sel_k.is_valid() || (sel_mb.is_valid() && !sel_mb->is_pressed() && sel_mb->get_button_index() == MOUSE_BUTTON_LEFT))) {
		_sel_undo_commit();
	}
	if (r != AFTER_GUI_INPUT_STOP) {
		_note_zoom_bar(p_event); // (Godot zooms with it)
	}
	if (r == AFTER_GUI_INPUT_STOP) {
		// Stopping the 3D viewport is not enough: unhandled key events would still reach the editor
		// shortcuts (numpad-emulated views on 1/3, W/E/R tools, Ctrl+R...).
		Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport();
		if (vp) {
			vp->set_input_as_handled();
		}
	}
	return r;
}

int32_t HewnEditorPlugin::_input_impl(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (phys_on) {
		// physics placement: its keys and the drags of the objects first
		const int32_t pr = _physics_input(p_camera, p_event);
		if (pr >= 0) {
			if (Viewport *v = get_viewport()) {
				v->set_input_as_handled();
			}
			return pr;
		}
	}
	{
		Ref<InputEventMouseButton> press = p_event;
		if (press.is_valid() && press->is_pressed() && (mode != MODE_OBJECT || modal != MODAL_NONE)) {
			_focus_viewport(p_camera);
		}
		Control *surface = _surface_for(p_camera);
		if (surface) {
			last_surface = surface->get_instance_id();
		}
		_check_redo_valid();
	}
	_validate_edited();
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		mouse_pos = mm->get_position();
	}
	{
		// Ctrl / Shift + wheel in Vertex Paint: the brush, whichever tool (but the circle select's Ctrl+wheel)
		Ref<InputEventMouseButton> wb = p_event;
		if (wb.is_valid() && vpaint && modal != MODAL_PAINT && !(select_kind == 1 && (wb->is_ctrl_pressed() || wb->is_meta_pressed())) && _paint_wheel(wb)) {
			return AFTER_GUI_INPUT_STOP;
		}
	}
	if (sculpt && modal == MODAL_SCULPT) {
		return _sculpt_input(p_camera, p_event); // (Sculpt: its brushes, and nothing of the edit mode)
	}
	bool paint_key_on = false; // (Vertex Paint's brush: the selection keys go on to the edit mode's handling)
	if (modal == MODAL_PAINT) {
		Ref<InputEventKey> pk = p_event;
		if (pk.is_valid()) {
			const int32_t r = _vpaint_keys(p_camera, pk);
			if (r >= 0) {
				return r;
			}
			paint_key_on = true;
		}
	}
	if (!paint_key_on && (modal == MODAL_KNIFE || modal == MODAL_LOOPCUT || modal == MODAL_CUTTER || modal == MODAL_POLY || modal == MODAL_RULER || modal == MODAL_SHAPE || modal == MODAL_PAINT || modal == MODAL_BISECT ||
			modal == MODAL_CURSOR || modal == MODAL_CLIP || modal == MODAL_PREFAB || modal == MODAL_SPIN || modal == MODAL_FACECUT)) {
		Ref<InputEventKey> tk = p_event;
		if (tk.is_valid() && tk->is_pressed() && !tk->is_echo() && key_code(tk) == KEY_TAB && tk->is_shift_pressed() && !tk->is_ctrl_pressed() && !tk->is_alt_pressed()) {
			dock->snap_enabled->set_pressed(!dock->snap_enabled->is_pressed()); // snapping on/off works inside the tools too
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (tk.is_valid() && tk->is_pressed() && !tk->is_echo() && mode != MODE_OBJECT && !tk->is_ctrl_pressed() && !tk->is_alt_pressed() &&
				!tk->is_shift_pressed() && (key_code(tk) == KEY_1 || key_code(tk) == KEY_2 || key_code(tk) == KEY_3) && modal != MODAL_POLY &&
				!(modal == MODAL_SHAPE && shape_state == 2)) { // (the height being typed)
			cancel_tool();
			set_mode(key_code(tk) == KEY_1 ? MODE_VERTEX : key_code(tk) == KEY_2 ? MODE_EDGE : MODE_FACE);
			return AFTER_GUI_INPUT_STOP;
		}
		if (tk.is_valid() && !tk->is_echo() && (key_code(tk) == KEY_CTRL || key_code(tk) == KEY_META)) {
			// Ctrl inverts snapping: recompute the snap marker / preview right away, not on the next move.
			Ref<InputEventMouseMotion> fake;
			fake.instantiate();
			fake->set_position(mouse_pos);
			fake->set_global_position(mouse_pos);
			fake->set_ctrl_pressed(ctrl_after(tk));
			fake->set_shift_pressed(tk->is_shift_pressed());
			fake->set_alt_pressed(tk->is_alt_pressed());
			_tool_input(p_camera, fake);
			_update_poly_preview();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		const int32_t r = _tool_input(p_camera, p_event);
		_update_poly_preview();
		if (r == AFTER_GUI_INPUT_PASS && tk.is_valid() && mode != MODE_OBJECT && !tk->is_ctrl_pressed()) {
			return AFTER_GUI_INPUT_STOP; // e.g. 1/3 would otherwise rotate the view, Q/W/E/R switch Godot tools
		}
		// a left click (or its release) the tool did not take: not to Godot either (it would select another object
		// on the release and leave the edit mode)
		Ref<InputEventMouseButton> tmb = p_event;
		if (r == AFTER_GUI_INPUT_PASS && tmb.is_valid() && tmb->get_button_index() == MOUSE_BUTTON_LEFT && mode != MODE_OBJECT) {
			return AFTER_GUI_INPUT_STOP;
		}
		return r;
	}
	if (modal == MODAL_UV) {
		return _uv_modal_input(p_camera, p_event);
	}
	if (modal == MODAL_UVT) {
		return _uvt_input(p_camera, p_event);
	}
	if (modal == MODAL_TSNAP) {
		return _tsnap_input(p_camera, p_event);
	}
	if (modal != MODAL_NONE && !paint_key_on) {
		return _modal_input(p_camera, p_event);
	}
	if (mode == MODE_OBJECT && select_kind == 1 && modal == MODAL_NONE) {
		// (the circle select of objects: also with nothing selected, or no Hewn mesh)
		const int32_t cr = _obj_circle_input(p_camera, p_event);
		if (cr >= 0) {
			return cr;
		}
	}
	{
		const int32_t pr = _face_push_input(p_camera, p_event); // (Shift + drag on a face)
		if (pr >= 0) {
			return pr;
		}
	}
	{
		Ref<InputEventKey> ak = p_event;
		if (ak.is_valid() && modal == MODAL_NONE && !vpaint && drag == DRAG_NONE && _arrow_keys(p_camera, ak)) {
			return AFTER_GUI_INPUT_STOP;
		}
	}
	{
		// a click that selects a CSG model: the brush under the mouse gets selected (see _edit)
		Ref<InputEventMouseButton> lb = p_event;
		if (lb.is_valid() && lb->is_pressed() && lb->get_button_index() == MOUSE_BUTTON_LEFT && mode == MODE_OBJECT && !lb->is_shift_pressed() && !lb->is_ctrl_pressed() &&
				!lb->is_alt_pressed()) {
			brush_click_ms = Time::get_singleton()->get_ticks_msec();
			brush_click_pos = lb->get_position();
			brush_click_cam = p_camera ? p_camera->get_instance_id() : ObjectID();
			plain_press = true;
		}
		// (the release of a click that finished a tool is not a selection click)
		if (lb.is_valid() && !lb->is_pressed() && lb->get_button_index() == MOUSE_BUTTON_LEFT && mode == MODE_OBJECT && !lb->is_shift_pressed() && !lb->is_ctrl_pressed() &&
				!lb->is_alt_pressed() && plain_press && lb->get_position().distance_to(brush_click_pos) < 4.0f) {
			plain_press = false;
			vis_click_frames = 2; // (after Godot has picked: _process)
			vis_click_pos = lb->get_position();
			vis_click_cam = p_camera ? p_camera->get_instance_id() : ObjectID();
		}
	}
	// the 3D cursor: also with no Hewn mesh picked yet
	{
		Ref<InputEventMouseButton> cb = p_event;
		if (cb.is_valid() && cb->is_pressed() && cb->get_button_index() == MOUSE_BUTTON_RIGHT && cb->is_shift_pressed()) {
			// Shift+RMB puts the 3D cursor on the surface under the mouse (or on the ground).
			_gather_snap_candidates(false);
			_place_cursor(p_camera, cb->get_position(), cb->is_ctrl_pressed() || cb->is_meta_pressed());
			return AFTER_GUI_INPUT_STOP;
		}
		Ref<InputEventKey> ck = p_event;
		// (not while the right button flies the camera: Shift+S there is "move down faster")
		if (ck.is_valid() && ck->is_pressed() && !ck->is_echo() && key_code(ck) == KEY_S && ck->is_shift_pressed() && !ck->is_ctrl_pressed() && !ck->is_alt_pressed() &&
				!Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)) {
			Control *surface = _surface_for(p_camera);
			const Vector2 at = surface ? surface->get_screen_position() + mouse_pos : Vector2();
			cursor_menu->set_item_disabled(cursor_menu->get_item_index(0), mode == MODE_OBJECT || _selected_verts().empty());
			cursor_menu->set_item_disabled(cursor_menu->get_item_index(3), mode != MODE_OBJECT && _selected_verts().empty());
			cursor_menu->popup(Rect2i(Vector2i(at), Vector2i()));
			return AFTER_GUI_INPUT_STOP;
		}
	}
	HewnMesh3D *n = get_edited();
	if (!n) {
		// No Hewn mesh edited: G / R / S still move, turn and scale the selected nodes (any Node3D).
		Ref<InputEventKey> ok = p_event;
		if (ok.is_valid() && ok->is_pressed() && !ok->is_echo() && !ok->is_meta_pressed() && drag == DRAG_NONE &&
				!Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)) {
			const Key code = key_code(ok);
			const bool plain = !ok->is_ctrl_pressed() && !ok->is_shift_pressed() && !ok->is_alt_pressed();
			if (plain && (code == KEY_G || code == KEY_R) && modal == MODAL_NONE && _mouse_near_cursor(p_camera)) {
				_start_cursor_xf(p_camera, code == KEY_G ? 1 : 2); // (the mouse over the 3D cursor: it is what moves, selected or not)
				return AFTER_GUI_INPUT_STOP;
			}
			if (plain && (code == KEY_G || code == KEY_R || code == KEY_S) &&
					_start_object_transform(code == KEY_G ? MODAL_MOVE : code == KEY_R ? MODAL_ROTATE : MODAL_SCALE, p_camera)) {
				return AFTER_GUI_INPUT_STOP;
			}
			if (_object_keys(code, ok->is_ctrl_pressed(), ok->is_shift_pressed(), ok->is_alt_pressed())) {
				return AFTER_GUI_INPUT_STOP;
			}
			if (_any_mode_keys(code, ok->is_ctrl_pressed(), ok->is_shift_pressed(), ok->is_alt_pressed())) {
				return AFTER_GUI_INPUT_STOP;
			}
		}
		return mode == MODE_OBJECT ? _object_rmb(p_camera, p_event) : AFTER_GUI_INPUT_PASS; // (a curve picked alone: its menu)
	}
	if (mode == MODE_OBJECT && modal == MODAL_NONE) {
		// (a right click in object mode: the menu of what is selected, whatever was edited before)
		Ref<InputEventMouseButton> rb = p_event;
		if (rb.is_valid() && rb->get_button_index() == MOUSE_BUTTON_RIGHT && !rb->is_shift_pressed()) {
			return _object_rmb(p_camera, p_event);
		}
	}
	// While the right button is held Godot flies the camera (WASD/QE): leave every key to it.
	const bool rmb_held = Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT);

	Ref<InputEventKey> k = p_event;
	if (k.is_valid()) {
		if (rmb_held || !k->is_pressed() || k->is_echo()) {
			if (rmb_held && k->is_pressed()) {
				rmb_pending = false; // flying with WASD: no context menu on release
			}
			return AFTER_GUI_INPUT_PASS;
		}
		const Key code = key_code(k);
		const bool ctrl = k->is_ctrl_pressed() || k->is_meta_pressed();
		const bool shift = k->is_shift_pressed();
		const bool alt = k->is_alt_pressed();
		const bool plain = !ctrl && !shift && !alt;
		if (plain && (code == KEY_G || code == KEY_R) && modal == MODAL_NONE && drag == DRAG_NONE && _mouse_near_cursor(p_camera)) {
			_start_cursor_xf(p_camera, code == KEY_G ? 1 : 2); // (the mouse over the 3D cursor: it is what moves)
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_TAB && plain) {
			toggle_edit_mode();
			return AFTER_GUI_INPUT_STOP;
		}
		if (vpaint) {
			// Vertex Paint with a selection tool: its keys, the selection keys; nothing that changes the mesh
			const int32_t vr = _vpaint_keys(p_camera, k);
			if (vr >= 0) {
				return vr;
			}
		}
		if (_any_mode_keys(code, ctrl, shift, alt)) {
			return AFTER_GUI_INPUT_STOP;
		}
		if (mode == MODE_OBJECT) {
			// G / R / S move, rotate and scale the selected objects
			if (plain && (code == KEY_G || code == KEY_R || code == KEY_S) && drag == DRAG_NONE) {
				_start_object_transform(code == KEY_G ? MODAL_MOVE : code == KEY_R ? MODAL_ROTATE : MODAL_SCALE, p_camera);
				return AFTER_GUI_INPUT_STOP;
			}
			if (_object_keys(code, ctrl, shift, alt)) {
				return AFTER_GUI_INPUT_STOP;
			}
			return AFTER_GUI_INPUT_PASS;
		}
		if (code == KEY_Z && alt && !ctrl && !shift) {
			set_xray(!xray_on);
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_O && !ctrl && !shift) {
			if (alt) {
				dock->prop_connected->set_pressed(!dock->prop_connected->is_pressed()); // Alt+O: connected only
			} else {
				dock->prop_enabled->set_pressed(!dock->prop_enabled->is_pressed());
			}
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_E && alt && !ctrl && !shift && !_selected_verts().empty()) {
			_show_extrude_menu(p_camera);
			return AFTER_GUI_INPUT_STOP;
		}
		if (drag == DRAG_HANDLE) {
			if (code == KEY_ESCAPE) {
				n->set_state(edit_before);
				editing = false;
				drag = DRAG_NONE;
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			return AFTER_GUI_INPUT_PASS;
		}
		const bool has_sel = !_selected_verts().empty();
		switch (code) {
			case KEY_1:
			case KEY_2:
			case KEY_3:
				if (shift && !ctrl && !alt) {
					toggle_mode(code == KEY_1 ? MODE_VERTEX : code == KEY_2 ? MODE_EDGE : MODE_FACE);
					return AFTER_GUI_INPUT_STOP;
				}
				if (plain) {
					set_mode(code == KEY_1 ? MODE_VERTEX : code == KEY_2 ? MODE_EDGE : MODE_FACE);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_G:
				if (plain && has_sel) {
					_start_transform(MODAL_MOVE, p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_R:
				if (ctrl && shift && !alt && has_sel) {
					_start_value_modal(p_camera, "offset_edge_slide");
					return AFTER_GUI_INPUT_STOP;
				}
				if (plain && has_sel) {
					_start_transform(MODAL_ROTATE, p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				if (ctrl && !shift && !alt) {
					start_loop_cut();
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_S:
				if (alt && !ctrl && !shift && has_sel) {
					_start_value_modal(p_camera, "shrink_fatten");
					return AFTER_GUI_INPUT_STOP;
				}
				if (alt && ctrl && shift && has_sel) {
					_start_value_modal(p_camera, "shear");
					return AFTER_GUI_INPUT_STOP;
				}
				if (plain && has_sel) {
					_start_transform(MODAL_SCALE, p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_E:
				if (shift && !ctrl && !alt && mode == MODE_EDGE && has_sel) {
					run_op("edge_crease");
					return AFTER_GUI_INPUT_STOP;
				}
				if (plain && has_sel) {
					set_op_param("extrude_move", "dissolve_orthogonal", false);
				set_op_param("extrude_move", "flip_normals", false);
					_start_extrude(p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_I:
				if (plain && mode == MODE_FACE && has_sel) {
					_start_inset(p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				if (ctrl && !shift && !alt) {
					run_op("invert");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_U:
				if (plain) {
					_uv_menu(p_camera); // texture tools
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_H:
				if (!ctrl) {
					hide_faces(alt ? 2 : shift ? 1 : 0);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_B:
				if (ctrl && shift && !alt && has_sel) {
					_start_value_modal(p_camera, "bevel_vertices");
					return AFTER_GUI_INPUT_STOP;
				}
				if (ctrl && !shift && !alt && has_sel) {
					_start_bevel(p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_K:
				if (plain) {
					start_knife();
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_A:
				if (plain) {
					run_op("select_all");
					return AFTER_GUI_INPUT_STOP;
				}
				if (alt && !ctrl && !shift) {
					run_op("select_none");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_L:
				if (plain) {
					_select_linked_at(p_camera, mouse_pos);
					return AFTER_GUI_INPUT_STOP;
				}
				if (ctrl && !shift && !alt) {
					run_op("linked");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_EQUAL:
			case KEY_KP_ADD:
				if (ctrl) {
					run_op("grow");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_MINUS:
			case KEY_KP_SUBTRACT:
				if (ctrl) {
					run_op("shrink");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_X:
			case KEY_DELETE:
				if (ctrl && !shift && !alt && code == KEY_X && has_sel) {
					run_op("dissolve");
					return AFTER_GUI_INPUT_STOP;
				}
				if (plain && has_sel) {
					_delete_menu(p_camera); // X / Delete open the delete menu
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_F:
				if (plain && mode != MODE_FACE) {
					run_op("make_face");
					if (f2_new_vertex >= 0 && mode == MODE_VERTEX) {
						f2_new_vertex = -1;
						_start_transform(MODAL_MOVE, p_camera);
					}
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_V:
				if (shift && !ctrl && !alt && has_sel) {
					if (_start_transform(MODAL_MOVE, p_camera)) {
						_start_slide(p_camera); // Shift+V: slide vertices
					}
					return AFTER_GUI_INPUT_STOP;
				}
				if (plain && has_sel && mode != MODE_FACE) {
					_start_rip(p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_M:
				if (shift && !ctrl && !alt) {
					start_ruler();
					return AFTER_GUI_INPUT_STOP;
				}
				if (plain && has_sel) {
					_run_menu_op(p_camera, "menu:merge");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_J:
				if (plain && mode == MODE_VERTEX) {
					run_op("connect_vertices");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_D:
				if (shift && !ctrl && !alt && mode == MODE_FACE && has_sel) {
					_start_duplicate(p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_P:
				if (plain && has_sel) {
					_run_menu_op(p_camera, "menu:separate"); // to a new mesh
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_N:
				// Shift+N recalculate outside, Ctrl+Shift+N inside, Alt+N the Normals menu.
				if (shift && !alt) {
					run_op(ctrl ? "recalc_inside" : "recalc_outside");
					return AFTER_GUI_INPUT_STOP;
				}
				if (alt && !ctrl && !shift) {
					_run_menu_op(p_camera, "menu:normals");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_W:
				if (plain) {
					// Move -> Rotate -> Scale -> no gizmo -> Move
					set_tool(!dock->show_gizmo->is_pressed() ? TOOL_MOVE : dock->gizmo_scale->is_pressed() ? TOOL_SELECT : dock->gizmo_rotate->is_pressed() ? TOOL_SCALE : TOOL_ROTATE);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			default:
				break;
		}
		return AFTER_GUI_INPUT_PASS;
	}

	if (mode == MODE_OBJECT) {
		return _object_rmb(p_camera, p_event);
	}

	if (bisect_drag != 0 || _bisect_widget_on()) {
		// the last bisect's plane: its arrow / rings dragged
		const int32_t br = _bisect_widget_input(p_camera, p_event);
		if (br >= 0) {
			if (Ref<InputEventMouseMotion>(p_event).is_valid()) {
				mouse_pos = Ref<InputEventMouseMotion>(p_event)->get_position();
			}
			return br;
		}
	}
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_RIGHT && drag != DRAG_HANDLE && !mb->is_shift_pressed()) {
		if (mb->is_pressed()) {
			rmb_pending = true;
			rmb_moved = 0.0f;
			rmb_press_ms = Time::get_singleton()->get_ticks_msec();
			rmb_press_pos = mb->get_position();
			return AFTER_GUI_INPUT_PASS; // Godot starts flying the camera if the button stays down
		}
		if (rmb_pending) {
			rmb_pending = false;
			// Godot captures the mouse while the right button is down (free look): the release position is
			// meaningless then, so the motion is summed from the relative movements instead.
			if (Time::get_singleton()->get_ticks_msec() - rmb_press_ms < 300 && rmb_moved < 6.0f) {
				mouse_pos = rmb_press_pos;
				// Let Godot finish its free look first (it restores the cursor), then open the menu.
				callable_mp(this, &HewnEditorPlugin::_context_menu_deferred).call_deferred(p_camera->get_instance_id());
				return AFTER_GUI_INPUT_PASS;
			}
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (mm.is_valid() && rmb_pending) {
		rmb_moved += mm->get_relative().length();
	}
	if (mb.is_valid()) {
		if (mb->get_button_index() == MOUSE_BUTTON_RIGHT && mb->is_pressed() && drag == DRAG_HANDLE) {
			n->set_state(edit_before);
			editing = false;
			drag = DRAG_NONE;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (select_kind == 1 && mb->is_pressed() && (mb->is_ctrl_pressed() || mb->is_meta_pressed()) &&
				(mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP || mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN)) {
			circle_radius = CLAMP(circle_radius * (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? 1.1f : 1.0f / 1.1f), 3.0f, 500.0f); // Ctrl+wheel: the circle's size
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb->get_button_index() != MOUSE_BUTTON_LEFT) {
			return AFTER_GUI_INPUT_PASS;
		}
		if (mb->is_pressed()) {
			press_pos = mouse_pos = mb->get_position();
			press_shift = mb->is_shift_pressed();
			press_ctrl = mb->is_ctrl_pressed() || mb->is_meta_pressed();
			press_alt = mb->is_alt_pressed();
			if (mb->is_double_click()) {
				_click_select(p_camera, press_pos, press_shift, false, press_alt, true);
				drag = DRAG_NONE;
				return AFTER_GUI_INPUT_STOP;
			}
			drag_axis = press_alt ? -1 : _handle_hit(p_camera, press_pos);
			if (drag_axis < 0 && select_kind == 1) {
				// the circle select tool: selects under the circle while held (Ctrl: deselects)
				drag = DRAG_CIRCLE;
				SelRegion r;
				r.kind = 1;
				r.center = press_pos;
				r.radius = circle_radius;
				_region_select(p_camera, r, press_shift, press_ctrl);
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			if (drag_axis < 0 && select_kind == 2) {
				lasso_pts.clear();
				lasso_pts.push_back(press_pos);
			}
			drag = DRAG_PENDING;
			return AFTER_GUI_INPUT_STOP;
		}
		// release
		if (drag == DRAG_CIRCLE || (drag == DRAG_LASSO && lasso_pts.size() < 3)) {
			drag = DRAG_NONE;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (drag == DRAG_LASSO) {
			SelRegion r;
			r.kind = 2;
			r.poly = lasso_pts;
			_region_select(p_camera, r, press_shift, press_ctrl);
			lasso_pts.clear();
			drag = DRAG_NONE;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (drag == DRAG_PENDING) {
			if (drag_axis < 0) {
				_click_select(p_camera, mb->get_position(), press_shift, press_ctrl, press_alt, false);
			}
		} else if (drag == DRAG_BOX) {
			_box_select(p_camera, Rect2(press_pos, Vector2()).expand(mb->get_position()), press_shift, press_ctrl);
		} else if (drag == DRAG_HANDLE) {
			_finish_handle_drag();
		}
		drag = DRAG_NONE;
		modal_snapped = false;
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}

	if (mm.is_valid()) {
		if (drag == DRAG_CIRCLE) {
			SelRegion r;
			r.kind = 1;
			r.center = mouse_pos;
			r.radius = circle_radius;
			_region_select(p_camera, r, true, press_ctrl); // (adds to what the press selected)
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (drag == DRAG_LASSO) {
			if (lasso_pts.empty() || lasso_pts.back().distance_to(mouse_pos) > 2.0f) {
				lasso_pts.push_back(mouse_pos);
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (drag == DRAG_PENDING && mouse_pos.distance_to(press_pos) > 3.0f) {
			if (drag_axis >= 0) {
				_begin_handle_drag(p_camera, drag_axis, press_shift);
			} else if (select_kind == 2) {
				drag = DRAG_LASSO;
				lasso_pts.push_back(mouse_pos);
			} else {
				drag = DRAG_BOX;
			}
		}
		if (drag == DRAG_HANDLE) {
			if (!mm->is_shift_pressed()) {
				precise_armed = true;
			}
			// Ctrl snaps to geometry (magnet), Shift slows the mouse down.
			mouse_pos = _precise_mouse(_wrap_mouse(p_camera, mm->get_position()), mm->is_shift_pressed() && precise_armed);
			const bool ctrl_held = mm->is_ctrl_pressed() || mm->is_meta_pressed();
			_update_handle_drag(p_camera, mouse_pos, snap_increment_on(ctrl_held), snapping_active(ctrl_held));
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (drag == DRAG_BOX) {
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if ((int64_t)mm->get_button_mask() == 0) {
			update_overlays(); // handle hover (no element hover highlight: only the selection is shown)
		}
		if (select_kind == 1) {
			update_overlays(); // (the circle follows the mouse)
		}
	}
	return AFTER_GUI_INPUT_PASS;
}

// keyboard moves: the arrows push the selection a grid step away / toward / left / right along the
// world axis nearest to the view, Page Up / Page Down up and down. Objects in object mode (Ctrl: copies of them
// move), the selected elements in edit mode (Ctrl in face mode: the faces extruded by the step, a block out of
// the face).
bool HewnEditorPlugin::_arrow_keys(Camera3D *p_cam, const Ref<InputEventKey> &p_key) {
	if (!p_cam || !p_key->is_pressed() || p_key->is_shift_pressed() || p_key->is_meta_pressed() ||
			Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)) {
		return false;
	}
	const Key code = key_code(p_key);
	if (code != KEY_UP && code != KEY_DOWN && code != KEY_LEFT && code != KEY_RIGHT && code != KEY_PAGEUP && code != KEY_PAGEDOWN) {
		return false;
	}
	if (p_key->is_alt_pressed()) {
		return !p_key->is_ctrl_pressed() && _alt_arrow_rotate(p_cam, code);
	}
	const bool ctrl = p_key->is_ctrl_pressed();
	// the view's axes snapped to the world's (on the ground: X or Z)
	const Basis cb = p_cam->get_global_transform().basis;
	auto ground_axis = [](Vector3 d) {
		d.y = 0.0f;
		if (d.length() < 1e-5f) {
			return Vector3(0, 0, -1);
		}
		return std::fabs(d.x) >= std::fabs(d.z) ? Vector3(SIGN(d.x), 0, 0) : Vector3(0, 0, SIGN(d.z));
	};
	Vector3 fwd = ground_axis(-cb.get_column(2));
	if (fwd == Vector3(0, 0, -1) && std::fabs(cb.get_column(2).y) > 0.99f) {
		fwd = ground_axis(cb.get_column(1)); // (looking straight down: up on the screen)
	}
	const Vector3 right = ground_axis(cb.get_column(0));
	Vector3 dir = code == KEY_UP ? fwd : code == KEY_DOWN ? -fwd : code == KEY_RIGHT ? right : code == KEY_LEFT ? -right : code == KEY_PAGEUP ? Vector3(0, 1, 0) : Vector3(0, -1, 0);
	float step = dock && dock->snap_move ? (float)dock->snap_move->get_value() : 1.0f;
	if (step <= 0.0f) {
		step = 1.0f;
	}
	const Vector3 d = dir * step;
	if (mode == MODE_OBJECT) {
		std::vector<Node3D *> nodes;
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
			Node3D *n3 = Object::cast_to<Node3D>(v);
			if (n3 && n3 != root && n3->is_inside_tree()) {
				nodes.push_back(n3);
			}
		}
		if (nodes.empty()) {
			return false;
		}
		EditorUndoRedoManager *ur = get_undo_redo();
		if (ctrl) {
			// copies, moved; the copies selected (the originals stay)
			ur->create_action("Hewn: Duplicate and Move");
			std::vector<Node *> copies;
			for (Node3D *n3 : nodes) {
				Node3D *c = Object::cast_to<Node3D>(n3->duplicate());
				if (!c) {
					continue;
				}
				Node *parent = n3->get_parent();
				Transform3D t = n3->get_transform();
				const Node3D *p3 = Object::cast_to<Node3D>(parent);
				t.origin += p3 ? p3->get_global_transform().basis.inverse().xform(d) : d;
				c->set_transform(t);
				ur->add_do_method(parent, "add_child", c, true);
				ur->add_do_method(parent, "move_child", c, n3->get_index() + 1);
				ur->add_do_method(c, "set_owner", root);
				// (its own nodes saved with the scene too; the insides of placed scenes keep theirs)
				std::vector<Node *> stack = { c };
				while (!stack.empty()) {
					Node *x = stack.back();
					stack.pop_back();
					for (int i = 0; i < x->get_child_count(); i++) {
						Node *ch = x->get_child(i);
						if (ch->get_owner() == c || ch->get_owner() == nullptr) {
							ur->add_do_method(ch, "set_owner", root);
						}
						if (ch->get_scene_file_path().is_empty()) {
							stack.push_back(ch);
						}
					}
				}
				ur->add_do_reference(c);
				ur->add_undo_method(parent, "remove_child", c);
				copies.push_back(c);
			}
			ur->commit_action();
			EditorSelection *es = EditorInterface::get_singleton()->get_selection();
			es->clear();
			for (Node *c : copies) {
				es->add_node(c);
			}
			return true;
		}
		ur->create_action("Hewn: Move", UndoRedo::MERGE_ENDS); // (arrows pressed in a row: one step)
		for (Node3D *n3 : nodes) {
			const Node3D *p3 = Object::cast_to<Node3D>(n3->get_parent());
			const Vector3 ld = p3 ? p3->get_global_transform().basis.inverse().xform(d) : d;
			ur->add_do_property(n3, "position", n3->get_position() + ld);
			ur->add_undo_property(n3, "position", n3->get_position());
		}
		ur->commit_action();
		return true;
	}
	HewnMesh3D *n = get_edited();
	if (!n || (n->selection.verts.empty() && n->selection.edges.empty() && n->selection.faces.empty())) {
		return false;
	}
	if (ctrl && mode == MODE_FACE) {
		// a brush of its own: a block on the selected faces, a step long, as a new object next to this one;
		// edited along with it, its top selected (again: the next one)
		Node *parent = n->get_parent();
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		std::vector<int> region;
		for (int f : n->selection.faces) {
			if (f >= 0 && f < (int)n->get_data().faces.size()) {
				region.push_back(f);
			}
		}
		if (!parent || !root || region.empty()) {
			return true;
		}
		hewn::Mesh tmp = n->get_data();
		const int nf0 = (int)tmp.faces.size();
		const std::vector<int> tops = hw_make_block(tmp, region, n->get_global_transform().basis.inverse().xform(d));
		std::vector<int> made;
		for (int f = nf0; f < (int)tmp.faces.size(); f++) {
			made.push_back(f);
		}
		if (made.empty()) {
			return true;
		}
		HewnMesh3D *nn = memnew(HewnMesh3D);
		nn->set_shape(hewn::shapes::CUSTOM);
		nn->get_data() = hewn::ops::extract_faces(tmp, made);
		nn->set_materials(n->get_materials());
		String bname = n->get_name();
		if (bname.find("_block") > 0) {
			bname = bname.substr(0, bname.find("_block")); // (a block of a block: Cube_block2, not Cube_block_block)
		}
		nn->set_name(bname + "_block");
		nn->set_transform(n->get_transform());
		nn->commit(true);
		EditorUndoRedoManager *ur = get_undo_redo();
		ur->create_action("Hewn: Block from Faces");
		ur->add_do_method(parent, "add_child", nn, true);
		ur->add_do_method(parent, "move_child", nn, n->get_index() + 1);
		ur->add_do_method(nn, "set_owner", root);
		ur->add_do_reference(nn);
		ur->add_undo_method(parent, "remove_child", nn);
		ur->commit_action();
		n->selection.clear();
		n->update_gizmos();
		multi_ids.push_back(nn->get_instance_id());
		_set_active_mesh(nn);
		nn->selection.clear();
		for (int t : tops) {
			nn->selection.faces.push_back(t - nf0);
		}
		selection_changed();
		return true;
	}
	const String op = "translate";
	set_op_param(op, "x", d.x);
	set_op_param(op, "y", d.y);
	set_op_param(op, "z", d.z);
	set_op_param(op, "prop", false);
	set_op_param(op, "prop_projected", false);
	_store_symmetry(op);
	merge_ends_next = true; // (arrows pressed in a row: one undo step)
	run_op(op);
	merge_ends_next = false;
	return true;
}

// Alt+arrows: a quarter turn of the selection (objects, or the selected elements) about the view's axes snapped to
// the world's: left / right around the vertical, up / down around the view's sideways axis, Page Up / Down around
// the one looking into the view. Objects turn about the middle of their bounds.
bool HewnEditorPlugin::_alt_arrow_rotate(Camera3D *p_cam, Key p_code) {
	const Basis cb = p_cam->get_global_transform().basis;
	auto major = [](const Vector3 &p_d) {
		const Vector3 a = p_d.abs();
		if (a.x >= a.y && a.x >= a.z) {
			return Vector3(SIGN(p_d.x), 0, 0);
		}
		return a.y >= a.z ? Vector3(0, SIGN(p_d.y), 0) : Vector3(0, 0, SIGN(p_d.z));
	};
	Vector3 right = major(Vector3(cb.get_column(0).x, 0, cb.get_column(0).z));
	if (Vector3(cb.get_column(0).x, 0, cb.get_column(0).z).length() < 1e-4f) {
		right = Vector3(1, 0, 0);
	}
	const Vector3 back = right.cross(Vector3(0, 1, 0)); // (toward the viewer, on the ground)
	Vector3 axis;
	float s = 1.0f;
	switch (p_code) {
		case KEY_LEFT:
			axis = Vector3(0, 1, 0);
			s = 1.0f;
			break;
		case KEY_RIGHT:
			axis = Vector3(0, 1, 0);
			s = -1.0f;
			break;
		case KEY_UP:
			axis = right;
			s = -1.0f; // (the top goes away from the viewer)
			break;
		case KEY_DOWN:
			axis = right;
			s = 1.0f;
			break;
		case KEY_PAGEUP:
			axis = back;
			s = 1.0f;
			break;
		default:
			axis = back;
			s = -1.0f;
			break;
	}
	// the quarter turn with whole numbers: R v = a (a.v) + s (a x v)
	Basis rot;
	for (int i = 0; i < 3; i++) {
		Vector3 e;
		e[i] = 1.0f;
		rot.set_column(i, axis * axis.dot(e) + axis.cross(e) * s);
	}
	if (mode == MODE_OBJECT) {
		std::vector<Node3D *> nodes;
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		AABB box;
		bool has_box = false;
		for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
			Node3D *n3 = Object::cast_to<Node3D>(v);
			if (!n3 || n3 == root || !n3->is_inside_tree()) {
				continue;
			}
			nodes.push_back(n3);
			AABB b(n3->get_global_position(), Vector3());
			if (VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(n3)) {
				b = n3->get_global_transform().xform(vi->get_aabb());
			}
			box = has_box ? box.merge(b) : b;
			has_box = true;
		}
		if (nodes.empty()) {
			return false;
		}
		// (the center on the grid: what was on the grid stays on it; a box of odd length shifts by half a step)
		const float st = dock && dock->snap_move ? MAX(1e-4f, (float)dock->snap_move->get_value()) : 1.0f;
		const Vector3 c = (box.get_center() / st).round() * st;
		EditorUndoRedoManager *ur = get_undo_redo();
		ur->create_action("Hewn: Rotate 90");
		for (Node3D *n3 : nodes) {
			Transform3D gt = n3->get_global_transform();
			gt.basis = rot * gt.basis;
			gt.origin = c + rot.xform(gt.origin - c);
			const Node3D *p3 = Object::cast_to<Node3D>(n3->get_parent());
			const Transform3D lt = p3 ? p3->get_global_transform().affine_inverse() * gt : gt;
			ur->add_do_property(n3, "transform", lt);
			ur->add_undo_property(n3, "transform", n3->get_transform());
		}
		ur->commit_action();
		return true;
	}
	HewnMesh3D *n = get_edited();
	Vector3 piv;
	if (!n || vpaint || !_pivot(piv)) {
		return false;
	}
	{
		const float st = dock && dock->snap_move ? MAX(1e-4f, (float)dock->snap_move->get_value()) : 1.0f;
		piv = (piv / st).round() * st; // (on the grid, as for objects)
	}
	set_op_param("rotate", "px", piv.x);
	set_op_param("rotate", "py", piv.y);
	set_op_param("rotate", "pz", piv.z);
	set_op_param("rotate", "ax", axis.x);
	set_op_param("rotate", "ay", axis.y);
	set_op_param("rotate", "az", axis.z);
	set_op_param("rotate", "angle", 90.0 * s);
	set_op_param("rotate", "prop", false);
	set_op_param("rotate", "pivot_mode", 0);
	_store_symmetry("rotate");
	run_op("rotate");
	return true;
}

std::vector<HewnEditorPlugin::SelSnap> HewnEditorPlugin::_sel_snapshot() const {
	std::vector<SelSnap> out;
	for (HewnMesh3D *m : edited_meshes()) {
		out.push_back({ ObjectID(m->get_instance_id()), m->get_selection_state() });
	}
	return out;
}

uint64_t HewnEditorPlugin::_history_version() const {
	HewnMesh3D *n = get_edited();
	EditorUndoRedoManager *ur = const_cast<HewnEditorPlugin *>(this)->get_undo_redo();
	if (!n || !ur) {
		return 0;
	}
	UndoRedo *h = ur->get_history_undo_redo(ur->get_object_history_id(n));
	return h ? h->get_version() : 0;
}

// Only the selection changed (no other step was made meanwhile, nothing being edited): one undo step for it.
void HewnEditorPlugin::_sel_undo_commit() {
	sel_undo_pending = false;
	if (mode == MODE_OBJECT || modal != MODAL_NONE || editing || drag != DRAG_NONE || _history_version() != sel_undo_version) {
		sel_undo_before.clear();
		return;
	}
	const std::vector<SelSnap> after = _sel_snapshot();
	bool changed = after.size() != sel_undo_before.size();
	for (size_t i = 0; i < after.size() && !changed; i++) {
		changed = after[i].id != sel_undo_before[i].id;
		for (const char *k : { "sel_verts", "sel_edges", "sel_faces" }) {
			changed = changed || PackedInt32Array(after[i].sel[k]) != PackedInt32Array(sel_undo_before[i].sel[k]);
		}
	}
	if (!changed) {
		sel_undo_before.clear();
		return;
	}
	HewnMesh3D *n = get_edited();
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Select", UndoRedo::MERGE_DISABLE, n);
	for (const SelSnap &s : after) {
		if (Object *o = ObjectDB::get_instance(s.id)) {
			ur->add_do_method(o, "set_selection_state", s.sel);
		}
	}
	for (const SelSnap &s : sel_undo_before) {
		if (Object *o = ObjectDB::get_instance(s.id)) {
			ur->add_undo_method(o, "set_selection_state", s.sel);
		}
	}
	ur->commit_action(false);
	sel_undo_before.clear();
}

// Keys of every mode, with or without a mesh: Shift+Tab snapping on / off, Shift+M the ruler.
bool HewnEditorPlugin::_any_mode_keys(Key p_code, bool p_ctrl, bool p_shift, bool p_alt) {
	if (p_ctrl || p_alt || !p_shift) {
		return false;
	}
	if (p_code == KEY_TAB && dock && dock->snap_enabled) {
		dock->snap_enabled->set_pressed(!dock->snap_enabled->is_pressed());
		update_overlays();
		return true;
	}
	if (p_code == KEY_M && drag == DRAG_NONE) {
		start_ruler();
		return true;
	}
	return false;
}

// Ctrl / Shift + wheel over a 3D viewport while a brush is out (Vertex Paint, Sculpt, Scatter): the brush's size /
// strength, taken before the viewport would zoom with it.
void HewnEditorPlugin::_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (phys_on && phys_grab >= 0 && _physics_wheel(mb)) {
		if (Viewport *v = get_viewport()) {
			v->set_input_as_handled(); // (before the view would zoom with it)
		}
		return;
	}
	auto shape_has = [&](const char *p_name) {
		for (const hewn::shapes::Param &sp : hewn::shapes::params((hewn::shapes::Type)shape_type)) {
			if (String(sp.name) == p_name) {
				return true;
			}
		}
		return false;
	};
	if (mb.is_valid() && hc_xf == 7 && modal == MODAL_NONE && _hc() && !mb->is_ctrl_pressed() && !mb->is_meta_pressed() && !mb->is_shift_pressed() &&
			!mb->is_alt_pressed() && (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP || mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN)) {
		// a curve's bevel going on: the wheel sets its segments, before the viewport would zoom with it
		for (int i = 0; i < 4; i++) {
			SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(i);
			Control *box = vp ? Object::cast_to<Control>(vp->get_parent()) : nullptr;
			if (!box || !box->is_visible_in_tree() || !box->get_global_rect().has_point(mb->get_position())) {
				continue;
			}
			if (mb->is_pressed() && vp->get_camera_3d()) {
				_hc_input(vp->get_camera_3d(), mb);
			}
			if (Viewport *v = get_viewport()) {
				v->set_input_as_handled();
			}
			return;
		}
	}
	if (mb.is_valid() && modal == MODAL_SHAPE && shape_state >= 1 && (shape_has("steps") || shape_has("sides") || shape_has("segments")) && !mb->is_ctrl_pressed() && !mb->is_meta_pressed() && !mb->is_shift_pressed() &&
			!mb->is_alt_pressed() && (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP || mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN)) {
		// a shape being drawn: the wheel sets its steps / sides, before the viewport would zoom with it
		for (int i = 0; i < 4; i++) {
			SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(i);
			Control *box = vp ? Object::cast_to<Control>(vp->get_parent()) : nullptr;
			if (!box || !box->is_visible_in_tree() || !box->get_global_rect().has_point(mb->get_position())) {
				continue;
			}
			if (mb->is_pressed() && vp->get_camera_3d()) {
				_shape_input(vp->get_camera_3d(), mb);
			}
			if (Viewport *v = get_viewport()) {
				v->set_input_as_handled();
			}
			return;
		}
	}
	const bool sculpt_brush = sculpt && modal == MODAL_SCULPT && dock && dock->sculpt_radius_field;
	const bool scatter_brush = scatter_tool >= 0 && get_scatter() && mode == MODE_OBJECT && modal == MODAL_NONE;
	if (!(vpaint || sculpt_brush || scatter_brush) || mb.is_null() || !(mb->is_ctrl_pressed() || mb->is_meta_pressed() || mb->is_shift_pressed()) ||
			mb->is_alt_pressed() || (mb->get_button_index() != MOUSE_BUTTON_WHEEL_UP && mb->get_button_index() != MOUSE_BUTTON_WHEEL_DOWN)) {
		return;
	}
	if (vpaint && select_kind == 1 && modal != MODAL_PAINT && (mb->is_ctrl_pressed() || mb->is_meta_pressed())) {
		return; // (the circle select's own Ctrl+wheel)
	}
	const bool ctrl = mb->is_ctrl_pressed() || mb->is_meta_pressed();
	if (scatter_brush && !ctrl) {
		return; // (the scatter's brush: only Ctrl+wheel)
	}
	// over one of the 3D viewports?
	bool over = false;
	for (int i = 0; i < 4 && !over; i++) {
		SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(i);
		Control *box = vp ? Object::cast_to<Control>(vp->get_parent()) : nullptr;
		over = box && box->is_visible_in_tree() && box->get_global_rect().has_point(mb->get_position());
	}
	if (!over) {
		return;
	}
	bool used = false;
	if (!mb->is_pressed()) {
		used = true; // (the release of a wheel step taken: nothing of it reaches the camera)
	} else if (sculpt_brush) {
		const bool upw = mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP;
		if (mb->is_shift_pressed()) {
			dock->sculpt_strength_field->set_value(dock->sculpt_strength_field->get_value() + (upw ? 0.05 : -0.05));
		} else {
			dock->sculpt_radius_field->set_value(dock->sculpt_radius_field->get_value() * (upw ? 1.1 : 1.0 / 1.1));
		}
		update_overlays();
		used = true;
	} else if (scatter_brush) {
		HewnScatter3D *sc = get_scatter();
		sc->set_brush_radius(sc->get_brush_radius() * (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? 1.1f : 1.0f / 1.1f));
		update_overlays();
		used = true;
	} else {
		used = _paint_wheel(mb);
	}
	if (used) {
		if (Viewport *v = get_viewport()) {
			v->set_input_as_handled();
		}
	}
}
