// Hewn editor - the move / rotate / scale handles, the drags along an axis or a plane, the drawing over the view.

#include "hw_plugin_impl.h"

// ------------------------------------------------------------------ handles

std::vector<int> HewnEditorPlugin::_selected_verts() const {
	HewnMesh3D *n = get_edited();
	if (!n || mode == MODE_OBJECT) {
		return {};
	}
	const hewn::Selection &s = n->selection;
	std::vector<int> out;
	if (has_mode(MODE_VERTEX)) {
		out.insert(out.end(), s.verts.begin(), s.verts.end());
	}
	if (has_mode(MODE_EDGE)) {
		const std::vector<int> v = hewn::sel::verts_of_edges(s.edges);
		out.insert(out.end(), v.begin(), v.end());
	}
	if (has_mode(MODE_FACE)) {
		const std::vector<int> v = hewn::sel::verts_of_faces(n->get_data(), s.faces);
		out.insert(out.end(), v.begin(), v.end());
	}
	return hewn::sel::uniq(out);
}

bool HewnEditorPlugin::_pivot(Vector3 &r_world) const {
	HewnMesh3D *n = get_edited();
	const std::vector<int> vs = _selected_verts();
	if (!n || vs.empty()) {
		return false;
	}
	if (dock && dock->pivot_mode != HewnDock::PIVOT_MEDIAN && dock->pivot_mode != HewnDock::PIVOT_INDIVIDUAL) {
		r_world = _pivot_point(dock->pivot_mode, vs);
		return true;
	}
	Vector3 c;
	for (int v : vs) {
		c += n->get_data().pos[v];
	}
	r_world = n->get_global_transform().xform(c / (float)vs.size());
	return true;
}

float HewnEditorPlugin::_handle_size(Camera3D *p_cam, const Vector3 &p_world) const {
	if (p_cam->get_projection() == Camera3D::PROJECTION_ORTHOGONAL) {
		return p_cam->get_size() * 0.12f;
	}
	const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2);
	return MAX(0.01f, (p_world - p_cam->get_global_position()).dot(fwd)) * 0.17f;
}

// The transform gizmo follows Godot's own gizmo size setting (80 = its default).
float HewnEditorPlugin::_gizmo_length(Camera3D *p_cam, const Vector3 &p_world) const {
	float k = 1.0f;
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid() && es->has_setting("editors/3d/manipulator_gizmo_size")) {
		k = CLAMP((float)es->get_setting("editors/3d/manipulator_gizmo_size"), 16.0f, 160.0f) / 80.0f;
	}
	return _handle_size(p_cam, p_world) * k;
}

float HewnEditorPlugin::_world_per_pixel(Camera3D *p_cam, const Vector3 &p_world) const {
	const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2);
	const float depth = MAX(0.01f, (p_world - p_cam->get_global_position()).dot(fwd));
	const Vector2 sp = p_cam->unproject_position(p_world);
	return MAX(1e-6f, p_cam->project_position(sp, depth).distance_to(p_cam->project_position(sp + Vector2(1, 0), depth)));
}

// Gizmo: any combination of move arrows, scale boxes and rotation rings.
// Hit codes are tool * 4 + axis (axis 3 = the center).
bool HewnEditorPlugin::_gizmo_on(Tool p_tool) const {
	if (!dock || !dock->show_gizmo->is_pressed()) {
		return false;
	}
	return p_tool == TOOL_MOVE ? dock->gizmo_move->is_pressed() : p_tool == TOOL_ROTATE ? dock->gizmo_rotate->is_pressed() : dock->gizmo_scale->is_pressed();
}

Basis HewnEditorPlugin::_gizmo_basis(Camera3D *p_cam) const {
	const int sel = dock->gizmo_orient->get_selected();
	return _orientation_basis(sel <= 0 ? dock->orientation : sel - 1, p_cam);
}

int HewnEditorPlugin::_handle_hit(Camera3D *p_cam, const Vector2 &p_pos) const {
	if (vpaint) {
		return -1; // (no gizmo while painting)
	}
	Vector3 piv;
	const bool mv = _gizmo_on(TOOL_MOVE), rt = _gizmo_on(TOOL_ROTATE), sc = _gizmo_on(TOOL_SCALE);
	if (!p_cam || !(mv || rt || sc) || !_pivot(piv) || p_cam->is_position_behind(piv)) {
		return -1;
	}
	const float L = _gizmo_length(p_cam, piv);
	const Vector2 c = p_cam->unproject_position(piv);
	const Basis ob = _gizmo_basis(p_cam);
	if (p_pos.distance_to(c) < 9.0f && (mv || sc)) {
		return (mv ? TOOL_MOVE : TOOL_SCALE) * 4 + 3;
	}
	int best = -1;
	float best_d = 8.0f;
	const float scale_at = mv ? 0.72f : 1.0f;
	const float ring = (mv || sc) ? 1.2f : 1.0f;
	for (int i = 0; i < 3; i++) {
		const Vector3 ax = ob.get_column(i).normalized();
		if (sc) {
			const float d = p_pos.distance_to(p_cam->unproject_position(piv + ax * L * scale_at));
			if (d < best_d) {
				best_d = d;
				best = TOOL_SCALE * 4 + i;
			}
		}
		if (mv) {
			const float d = seg_dist(p_pos, c, p_cam->unproject_position(piv + ax * L));
			if (d < best_d) {
				best_d = d;
				best = TOOL_MOVE * 4 + i;
			}
		} else if (sc) {
			const float d = seg_dist(p_pos, c, p_cam->unproject_position(piv + ax * L)) + 2.0f;
			if (d < best_d) {
				best_d = d;
				best = TOOL_SCALE * 4 + i;
			}
		}
		if (rt) {
			Vector3 u, w;
			hewn::plane_basis(ax, u, w);
			Vector2 prev;
			float d = 1e9f;
			for (int k = 0; k <= 48; k++) {
				const float a = (float)Math::PI * 2.0f * k / 48.0f;
				const Vector2 p = p_cam->unproject_position(piv + (u * std::cos(a) + w * std::sin(a)) * L * ring);
				if (k > 0) {
					d = MIN(d, seg_dist(p_pos, prev, p));
				}
				prev = p;
			}
			if (d < best_d) {
				best_d = d;
				best = TOOL_ROTATE * 4 + i;
			}
		}
	}
	return best;
}


float HewnEditorPlugin::_axis_param(Camera3D *p_cam, const Vector2 &p_pos, const Vector3 &p_origin, const Vector3 &p_dir) const {
	Vector3 o, d;
	_ray(p_cam, p_pos, o, d);
	const float FAR = MAX(2000.0f, o.distance_to(p_origin) * 20.0f);
	// An axis going away from the camera ends on the screen at its vanishing point: past it the nearest
	// point would come back; it stays far away instead, like _plane_hit.
	const Vector3 dir = p_dir.normalized();
	const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2).normalized();
	if (p_cam->get_projection() == Camera3D::PROJECTION_PERSPECTIVE && !p_cam->is_position_behind(p_origin)) {
		const Vector2 s0 = p_cam->unproject_position(p_origin);
		for (int sign = -1; sign <= 1; sign += 2) {
			const Vector3 D = dir * (float)sign;
			if (D.dot(fwd) < 1e-3f) {
				continue;
			}
			const Vector3 far = p_origin + D * 1e6f;
			if (p_cam->is_position_behind(far)) {
				continue;
			}
			const Vector2 e = p_cam->unproject_position(far) - s0;
			if (e.length_squared() > 1.0f && (p_pos - s0).dot(e) / e.length_squared() >= 1.0f) {
				return FAR * sign / MAX(p_dir.length(), 1e-6f);
			}
		}
	}
	const float t = line_ray_param(p_origin, p_dir, o, d);
	const float lim = FAR / MAX(p_dir.length(), 1e-6f);
	return CLAMP(t, -lim, lim);
}

namespace {

// The view's way at a point (perspective: from the eye to it), and how many pixels a meter there makes across the view.
Vector3 view_way(Camera3D *p_cam, const Vector3 &p_at) {
	const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2).normalized();
	if (p_cam->get_projection() != Camera3D::PROJECTION_PERSPECTIVE) {
		return fwd;
	}
	const Vector3 w = p_at - p_cam->get_global_position();
	return w.length() > 1e-6f ? w.normalized() : fwd;
}

float step_at(Camera3D *p_cam, const Vector3 &p_at) {
	const Vector3 eye = p_cam->get_global_position();
	const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2).normalized();
	const float depth = p_cam->get_projection() == Camera3D::PROJECTION_PERSPECTIVE ? MAX(1e-3f, (p_at - eye).dot(fwd)) : MAX(1e-3f, p_cam->get_size());
	return depth * 0.01f; // (a short step: its picture straight)
}

} // namespace

float HewnEditorPlugin::_drag_axis(Camera3D *p_cam, const Vector3 &p_pivot, const Vector3 &p_dir, const Vector2 &p_from, const Vector2 &p_to) const {
	const Vector3 d = p_dir.normalized();
	if (p_cam->is_position_behind(p_pivot) || std::fabs(d.dot(view_way(p_cam, p_pivot))) < 0.9f) {
		return _axis_param(p_cam, p_to, p_pivot, p_dir) - _axis_param(p_cam, p_from, p_pivot, p_dir);
	}
	// nearly toward the camera: the axis' way on the screen, its pixels a meter there - never fewer than a quarter of the
	// view's own (or it would race)
	const float L = step_at(p_cam, p_pivot);
	const Vector2 s0 = p_cam->unproject_position(p_pivot);
	const Vector3 right = p_cam->get_global_transform().basis.get_column(0).normalized();
	const float ppu_view = p_cam->unproject_position(p_pivot + right * L).distance_to(s0) / L;
	Vector2 sa = p_cam->is_position_behind(p_pivot + d * L) ? Vector2() : p_cam->unproject_position(p_pivot + d * L) - s0;
	const float ppu_axis = sa.length() / L;
	const Vector2 way = sa.length() > 0.5f ? sa.normalized() : Vector2(0, -1); // (end on: the mouse up moves it away)
	const float ppu = MAX(ppu_axis, ppu_view * 0.25f);
	return ppu > 1e-9f ? (p_to - p_from).dot(way) / ppu / MAX(p_dir.length(), 1e-6f) : 0.0f;
}

bool HewnEditorPlugin::_drag_plane(Camera3D *p_cam, const Vector3 &p_pivot, const Vector3 &p_n, const Vector2 &p_from, const Vector2 &p_to, Vector3 &r_delta) const {
	const Vector3 n = p_n.normalized();
	if (p_cam->is_position_behind(p_pivot) || std::fabs(n.dot(view_way(p_cam, p_pivot))) > 0.25f) {
		Vector3 h0, h1;
		if (_plane_hit(p_cam, p_from, p_pivot, n, h0) && _plane_hit(p_cam, p_to, p_pivot, n, h1)) {
			r_delta = h1 - h0;
			return true;
		}
		return false;
	}
	// seen nearly edge on: the mouse's way on the screen brought into the plane by its two ways' pictures there (the
	// least squares, held back where the plane is seen thin)
	const float L = step_at(p_cam, p_pivot);
	Vector3 a, b;
	hewn::plane_basis(n, a, b);
	const Vector2 s0 = p_cam->unproject_position(p_pivot);
	const Vector3 right = p_cam->get_global_transform().basis.get_column(0).normalized();
	const float ppu_view = p_cam->unproject_position(p_pivot + right * L).distance_to(s0) / L;
	const Vector2 ja = (p_cam->unproject_position(p_pivot + a * L) - s0) / L;
	const Vector2 jb = (p_cam->unproject_position(p_pivot + b * L) - s0) / L;
	const float lam = (ppu_view * 0.25f) * (ppu_view * 0.25f);
	const float aa = ja.dot(ja) + lam, ab = ja.dot(jb), bb = jb.dot(jb) + lam;
	const Vector2 m = p_to - p_from;
	const float ra = ja.dot(m), rb = jb.dot(m);
	const float det = aa * bb - ab * ab;
	if (std::fabs(det) < 1e-12f) {
		return false;
	}
	const float x = (ra * bb - rb * ab) / det, y = (aa * rb - ab * ra) / det;
	r_delta = a * x + b * y;
	return true;
}

// Where the mouse ray meets a plane. Toward the horizon the point goes far away; past it (the ray never
// reaches the plane) it stays far away in the mouse's direction, instead of coming back
// from behind the camera.
bool HewnEditorPlugin::_plane_hit(Camera3D *p_cam, const Vector2 &p_pos, const Vector3 &p_origin, const Vector3 &p_n, Vector3 &r_hit) const {
	Vector3 o, d;
	_ray(p_cam, p_pos, o, d);
	const float FAR = MAX(2000.0f, o.distance_to(p_origin) * 20.0f);
	const Vector3 n = p_n.normalized();
	const float den = d.dot(n);
	const float t = std::fabs(den) < 1e-9f ? -1.0f : (p_origin - o).dot(n) / den;
	const Vector3 base = o - n * (o - p_origin).dot(n); // (the camera dropped onto the plane)
	if (t >= 0.0f && (o + d * t).distance_to(base) <= FAR) {
		r_hit = o + d * t;
		return true;
	}
	const Vector3 dp = d - n * d.dot(n);
	if (dp.length() < 1e-6f) {
		return false;
	}
	r_hit = base + dp.normalized() * FAR;
	return true;
}

void HewnEditorPlugin::_begin_handle_drag(Camera3D *p_cam, int p_code, bool p_shift) {
	tool = (Tool)CLAMP(p_code / 4, 0, 2);
	const int p_axis = p_code % 4;
	HewnMesh3D *n = get_edited();
	if (!n || !_pivot(drag_pivot)) {
		return;
	}
	static const char *names[3] = { "Move", "Rotate", "Scale" };
	begin_edit(p_shift && tool == TOOL_MOVE ? "Extrude" : names[tool]);
	drag_verts.clear();
	drag_extruded = false;
	res_delta = Vector3();
	res_angle = 0.0f;
	res_scale = 1.0f;
	hewn::Mesh &m = n->get_data();
	hewn::Selection &s = n->selection;
	drag_extruded = p_shift && tool == TOOL_MOVE && ((mode == MODE_FACE && !s.faces.empty()) || (mode == MODE_EDGE && !s.edges.empty()));
	if (p_shift && tool == TOOL_MOVE && mode == MODE_FACE && !s.faces.empty()) {
		hewn::ops::extrude_faces(m, s.faces, 0.0f, false, &drag_verts);
		n->commit(true);
	} else if (p_shift && tool == TOOL_MOVE && mode == MODE_EDGE && !s.edges.empty()) {
		s.edges = hewn::ops::extrude_edges(m, s.edges, 0.0f, false);
		drag_verts = hewn::sel::verts_of_edges(s.edges);
		n->commit(true);
	}
	if (drag_verts.empty()) {
		drag_verts = _selected_verts();
	}
	{
		const std::vector<int> sel = drag_verts;
		prop_proj_axis = dock->prop_projected && dock->prop_projected->is_pressed() && p_cam ? -p_cam->get_global_transform().basis.get_column(2).normalized() : Vector3();
		_weighted_verts(sel, dock->prop_enabled->is_pressed() && !p_shift, (float)dock->prop_radius->get_value(), dock->prop_falloff->get_selected(),
				dock->prop_connected->is_pressed(), drag_verts, drag_weights);
	}
	drag_orig.clear();
	for (int v : drag_verts) {
		drag_orig.push_back(m.pos[v]);
	}
	// Magnet candidates exclude the dragged vertices.
	modal_verts = drag_verts;
	modal_object = false;
	_gather_snap_candidates(true);
	precise_armed = !p_shift;
	_reset_precise();
	modal_snapped = false;

	drag_axis = p_axis;
	const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2);
	drag_axis_dir = p_axis < 3 ? _gizmo_basis(p_cam).get_column(p_axis).normalized() : fwd;
	if (tool == TOOL_MOVE) {
		if (p_axis < 3) {
			drag_t0 = _axis_param(p_cam, press_pos, drag_pivot, drag_axis_dir);
		} else {
			_plane_hit(p_cam, press_pos, drag_pivot, fwd, drag_hit0);
		}
	} else if (tool == TOOL_ROTATE) {
		_plane_hit(p_cam, press_pos, drag_pivot, drag_axis_dir, drag_hit0);
	} else {
		if (p_axis < 3) {
			drag_t0 = _axis_param(p_cam, press_pos, drag_pivot, drag_axis_dir);
		} else {
			drag_screen0 = MAX(1.0f, press_pos.distance_to(p_cam->unproject_position(drag_pivot)));
		}
	}
	drag = DRAG_HANDLE;
}

void HewnEditorPlugin::_update_handle_drag(Camera3D *p_cam, const Vector2 &p_pos, bool p_snap, bool p_magnet) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	const Transform3D g = n->get_global_transform();
	const Transform3D inv = g.affine_inverse();
	const float sm = (float)dock->snap_move->get_value();
	const float sr = (float)dock->snap_rotate->get_value();
	const float ss = (float)dock->snap_scale->get_value();
	hewn::Mesh &m = n->get_data();
	auto snapf = [&](float x, float step) { return (p_snap && step > 0.0f) ? std::round(x / step) * step : x; };
	modal_snapped = false;

	if (tool == TOOL_MOVE) {
		Vector3 delta;
		Vector3 target;
		if (p_magnet && (dock->snap_targets & HewnDock::SNAP_GEOMETRY) && _snap_target(p_cam, p_pos, target)) {
			// Snap the dragged vertex closest to the target onto it, keeping the axis constraint.
			float best = 1e30f;
			Vector3 base = drag_pivot;
			for (const Vector3 &o : drag_orig) {
				const Vector3 w = g.xform(o);
				if (w.distance_to(target) < best) {
					best = w.distance_to(target);
					base = w;
				}
			}
			delta = target - base;
			if (drag_axis < 3) {
				delta = drag_axis_dir * delta.dot(drag_axis_dir);
			}
			modal_snapped = true;
			modal_snap_point = target;
			modal_snap_kind = last_snap_kind;
		} else if (drag_axis < 3) {
			delta = drag_axis_dir * snapf(_axis_param(p_cam, p_pos, drag_pivot, drag_axis_dir) - drag_t0, sm);
		} else {
			Vector3 hit;
			if (!_plane_hit(p_cam, p_pos, drag_pivot, drag_axis_dir, hit)) {
				return;
			}
			delta = hit - drag_hit0;
			delta = Vector3(snapf(delta.x, sm), snapf(delta.y, sm), snapf(delta.z, sm));
		}
		if (p_snap && snap_grid_on() && sm > 0.0f && !modal_snapped) {
			// Grid: the pivot lands on grid points (along the dragged axis only, when there is one)
			const Vector3 raw = delta;
			delta = _grid_point(drag_pivot + raw, Basis(), sm) - drag_pivot;
			if (drag_axis < 3) {
				delta = drag_axis_dir * delta.dot(drag_axis_dir);
			}
		}
		for (size_t i = 0; i < drag_verts.size(); i++) {
			m.pos[drag_verts[i]] = inv.xform(g.xform(drag_orig[i]) + delta * drag_weights[i]);
		}
		res_delta = delta;
	} else if (tool == TOOL_ROTATE) {
		Vector3 hit;
		if (!_plane_hit(p_cam, p_pos, drag_pivot, drag_axis_dir, hit)) {
			return;
		}
		const Vector3 v0 = drag_hit0 - drag_pivot, v1 = hit - drag_pivot;
		float ang = std::atan2(drag_axis_dir.dot(v0.cross(v1)), v0.dot(v1));
		ang = snapf(ang * 180.0f / (float)Math::PI, sr) * (float)Math::PI / 180.0f;
		res_axis = drag_axis_dir;
		res_angle = ang * 180.0f / (float)Math::PI;
		for (size_t i = 0; i < drag_verts.size(); i++) {
			const Basis rot(drag_axis_dir, ang * drag_weights[i]);
			m.pos[drag_verts[i]] = inv.xform(drag_pivot + rot.xform(g.xform(drag_orig[i]) - drag_pivot));
		}
	} else {
		float s;
		if (drag_axis < 3) {
			s = std::fabs(drag_t0) > 1e-6f ? _axis_param(p_cam, p_pos, drag_pivot, drag_axis_dir) / drag_t0 : 1.0f;
		} else {
			s = p_pos.distance_to(p_cam->unproject_position(drag_pivot)) / drag_screen0;
		}
		s = snapf(s, ss);
		res_scale = s;
		res_dir = drag_axis < 3 ? drag_axis_dir : Vector3(1, 0, 0);
		res_constraint = drag_axis < 3 ? 1 : 0;
		for (size_t i = 0; i < drag_verts.size(); i++) {
			const Vector3 d = g.xform(drag_orig[i]) - drag_pivot;
			const float si = 1.0f + (s - 1.0f) * drag_weights[i];
			Vector3 p;
			if (drag_axis < 3) {
				p = drag_pivot + d + drag_axis_dir * (d.dot(drag_axis_dir) * (si - 1.0f));
			} else {
				p = drag_pivot + d * si;
			}
			m.pos[drag_verts[i]] = inv.xform(p);
		}
	}
	n->apply_mirror_clipping(drag_verts, drag_orig);
	if (!drag_extruded) {
		_apply_symmetry(n, drag_verts, drag_orig, &drag_weights, dock->symmetry_axes());
	}
	n->commit(true);
}

// The drag is replayed from the start as the operation G / R / S would make (with the same numbers), so
// the Last Operation panel shows it and can change it.
void HewnEditorPlugin::_finish_handle_drag() {
	HewnMesh3D *n = get_edited();
	if (!n || !editing) {
		return;
	}
	String op;
	if (tool == TOOL_MOVE) {
		op = drag_extruded ? "extrude_move" : "translate";
		set_op_param(op, "x", res_delta.x);
		set_op_param(op, "y", res_delta.y);
		set_op_param(op, "z", res_delta.z);
		if (drag_extruded) {
			set_op_param(op, "dissolve_orthogonal", false);
			set_op_param(op, "flip_normals", false);
		}
	} else if (tool == TOOL_ROTATE) {
		op = "rotate";
		set_op_param(op, "angle", res_angle);
		set_op_param(op, "ax", res_axis.x);
		set_op_param(op, "ay", res_axis.y);
		set_op_param(op, "az", res_axis.z);
	} else {
		op = "scale";
		set_op_param(op, "factor", res_scale);
		set_op_param(op, "dx", res_dir.x);
		set_op_param(op, "dy", res_dir.y);
		set_op_param(op, "dz", res_dir.z);
		set_op_param(op, "constraint", res_constraint);
	}
	if (op != "extrude_move") {
		set_op_param(op, "prop", dock->prop_enabled->is_pressed());
		set_op_param(op, "prop_size", dock->prop_radius->get_value());
		set_op_param(op, "prop_falloff", dock->prop_falloff->get_selected());
		set_op_param(op, "prop_connected", dock->prop_connected->is_pressed());
		set_op_param(op, "prop_projected", prop_proj_axis != Vector3());
		set_op_param(op, "prop_vx", prop_proj_axis.x);
		set_op_param(op, "prop_vy", prop_proj_axis.y);
		set_op_param(op, "prop_vz", prop_proj_axis.z);
		// (the handles turn and scale around one pivot)
		set_op_param(op, "pivot_mode", dock->pivot_mode == HewnDock::PIVOT_INDIVIDUAL ? (int)HewnDock::PIVOT_MEDIAN : dock->pivot_mode);
		set_op_param(op, "px", drag_pivot.x);
		set_op_param(op, "py", drag_pivot.y);
		set_op_param(op, "pz", drag_pivot.z);
		_store_symmetry(op);
	}
	n->set_state(edit_before);
	editing = false;
	mode = element_mode = edit_mode_before;
	run_op(op);
	_set_godot_gizmo_hidden(mode != MODE_OBJECT);
	selection_changed();
}

void HewnEditorPlugin::_draw_handles(Control *p_overlay, Camera3D *p_cam) {
	Vector3 piv;
	const bool mv = _gizmo_on(TOOL_MOVE), rt = _gizmo_on(TOOL_ROTATE), sc = _gizmo_on(TOOL_SCALE);
	if (!(mv || rt || sc) || !_pivot(piv) || p_cam->is_position_behind(piv)) {
		return;
	}
	const float L = _gizmo_length(p_cam, piv);
	const Vector2 c = p_cam->unproject_position(piv);
	const int hover = drag == DRAG_HANDLE ? tool * 4 + drag_axis : _handle_hit(p_cam, mouse_pos);
	const Basis ob = _gizmo_basis(p_cam);
	const float scale_at = mv ? 0.72f : 1.0f;
	const float ring = (mv || sc) ? 1.2f : 1.0f;
	for (int i = 0; i < 3; i++) {
		const Vector3 ax = ob.get_column(i).normalized();
		auto col = [&](Tool t) { return hover == t * 4 + i ? HANDLE_HOVER : AXIS_COLORS[i]; };
		if (rt) {
			Vector3 u, w;
			hewn::plane_basis(ax, u, w);
			PackedVector2Array pts;
			for (int k = 0; k <= 64; k++) {
				const float a = (float)Math::PI * 2.0f * k / 64.0f;
				pts.push_back(p_cam->unproject_position(piv + (u * std::cos(a) + w * std::sin(a)) * L * ring));
			}
			p_overlay->draw_polyline(pts, col(TOOL_ROTATE), 2.0f, true);
		}
		const Vector2 e = p_cam->unproject_position(piv + ax * L);
		const Vector2 dir = (e - c).length() > 1e-3f ? (e - c).normalized() : Vector2(1, 0);
		const Vector2 nrm(-dir.y, dir.x);
		if (mv) {
			p_overlay->draw_line(c, e, col(TOOL_MOVE), 2.5f, true);
			PackedVector2Array tri;
			tri.push_back(e + dir * 14.0f);
			tri.push_back(e + nrm * 5.5f);
			tri.push_back(e - nrm * 5.5f);
			draw_poly_safe(p_overlay, tri, col(TOOL_MOVE));
		} else if (sc) {
			p_overlay->draw_line(c, e, col(TOOL_SCALE), 2.5f, true);
		}
		if (sc) {
			const Vector2 b = p_cam->unproject_position(piv + ax * L * scale_at);
			p_overlay->draw_rect(Rect2(b - Vector2(5, 5), Vector2(10, 10)), col(TOOL_SCALE));
		}
	}
	if (mv || sc) {
		const int center = (mv ? TOOL_MOVE : TOOL_SCALE) * 4 + 3;
		const Color cc = hover == center ? HANDLE_HOVER : Color(1, 1, 1, 0.9f);
		p_overlay->draw_rect(Rect2(c - Vector2(5, 5), Vector2(10, 10)), cc, false, 2.0f);
	}
	if (drag == DRAG_HANDLE && modal_snapped && !p_cam->is_position_behind(modal_snap_point)) {
		draw_snap_symbol(p_overlay, p_cam->unproject_position(modal_snap_point), modal_snap_kind, SNAP_COLOR, 7.0f);
	}
}

Camera3D *HewnEditorPlugin::_camera_for_overlay(Control *p_overlay) const {
	Node *container = p_overlay->get_parent();
	for (int i = 0; i < 4; i++) {
		SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(i);
		if (vp && container && container->is_ancestor_of(vp)) {
			return vp->get_camera_3d();
		}
	}
	SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(0);
	return vp ? vp->get_camera_3d() : nullptr;
}

void HewnEditorPlugin::_forward_3d_force_draw_over_viewport(Control *p_overlay) {
	Camera3D *cam = _camera_for_overlay(p_overlay);
	if (!cam) {
		return;
	}
	_marks_begin(p_overlay);
	_draw_over_viewport(p_overlay, cam);
	_marks_end(p_overlay); // (the marks not drawn this time: hidden)
}

void HewnEditorPlugin::_draw_over_viewport(Control *p_overlay, Camera3D *cam) {
	if (dock->overlay(dock->ov_cursor)) {
		_draw_cursor(p_overlay, cam);
	}
	if (pose) {
		_draw_pose(p_overlay, cam);
	}
	if (tpaint) {
		_draw_tpaint(p_overlay, cam);
		return;
	}
	if (wpaint) {
		_draw_wpaint(p_overlay, cam);
		return;
	}
	if (rig) {
		_draw_rig(p_overlay, cam);
	}
	if (curve_edit) {
		_draw_curve_edit(p_overlay, cam);
	}
	if (overlays_on()) {
		_draw_curve_lines(p_overlay, cam);
	}
	if (hc_edit) {
		_draw_hc(p_overlay, cam);
	}
	if (dock->overlay(dock->ov_notes)) {
		_draw_notes(p_overlay, cam);
	}
	if (dock->overlay(dock->ov_hs_fixed)) {
		_draw_hs_fixed(p_overlay, cam);
	}
	if (dock->overlay(dock->ov_nonplanar)) {
		_draw_nonplanar(p_overlay, cam);
	}
	if (dock->overlay(dock->ov_curve_normals)) {
		_draw_curve_normals(p_overlay, cam);
	}
	if (scatter_tool >= 0 && get_scatter() && mode == MODE_OBJECT && modal == MODAL_NONE) {
		_draw_scatter(p_overlay, cam);
	}
	_draw_face_push(p_overlay, cam);
	if (overlays_on()) {
		_draw_physics(p_overlay, cam);
	}
	if (mode == MODE_OBJECT && select_kind == 1 && modal == MODAL_NONE) {
		p_overlay->draw_arc(mouse_pos, circle_radius, 0.0f, (float)Math::PI * 2.0f, 48, Color(1, 1, 1, obj_circle_drag ? 0.9f : 0.55f), 1.0f, true);
	}
	if (dock->overlay(dock->ov_dims) && modal != MODAL_UVT && modal != MODAL_TSNAP) {
		_draw_dimensions(p_overlay, cam);
	}
	if (mode == MODE_OBJECT && dock->overlays->is_pressed()) {
		// the origins of the selected objects, orange dots
		const float es = EditorInterface::get_singleton()->get_editor_scale();
		for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_selected_nodes()) {
			Node3D *n3 = Object::cast_to<Node3D>(v);
			if (!n3 || !n3->is_inside_tree()) {
				continue;
			}
			const Vector3 wo = n3->get_global_transform().origin;
			if (!cam->is_position_behind(wo)) {
				const Vector2 o = cam->unproject_position(wo);
				p_overlay->draw_circle(o, (hw_mesh_style().origin_size * 0.5f + 1.0f) * es, Color(0.05f, 0.05f, 0.05f, 0.9f), true, -1.0f, true); // (its dark ring)
				p_overlay->draw_circle(o, hw_mesh_style().origin_size * 0.5f * es, hw_mesh_style().origin, true, -1.0f, true);
			}
		}
	}
	if (sculpt && modal == MODAL_SCULPT) {
		_draw_sculpt(p_overlay, cam);
		return;
	}
	if (modal == MODAL_NONE) {
		_draw_pending(p_overlay);
		if (_bridge_handles_live()) {
			_draw_bridge_handles(p_overlay, cam);
		}
		_draw_profile_in_view(p_overlay, cam);
	}
	if (modal == MODAL_NONE || modal == MODAL_MOVE || modal == MODAL_ROTATE || modal == MODAL_SCALE || modal == MODAL_SNAP) {
		_draw_brushes(p_overlay, cam);
	}
	if (modal == MODAL_NONE || modal == MODAL_MOVE || modal == MODAL_ROTATE || modal == MODAL_SCALE || modal == MODAL_PAINT) {
		_draw_overlays(p_overlay, cam); // (painting: the selection is the mask)
	}
	_draw_overlaps(p_overlay, cam);
	if (modal == MODAL_KNIFE || modal == MODAL_LOOPCUT || modal == MODAL_CUTTER || modal == MODAL_POLY || modal == MODAL_RULER || modal == MODAL_SHAPE || modal == MODAL_PAINT || modal == MODAL_BISECT ||
			modal == MODAL_CURSOR || modal == MODAL_CLIP || modal == MODAL_PREFAB || modal == MODAL_SPIN || modal == MODAL_FACECUT) {
		if (modal == MODAL_SPIN || modal == MODAL_FACECUT) {
			_draw_overlays(p_overlay, cam); // (the selection stays visible)
		}
		_draw_tools(p_overlay, cam);
		return;
	}
	if (modal == MODAL_UV) {
		_draw_uv_modal(p_overlay, cam);
		return;
	}
	if (modal == MODAL_UVT) {
		_draw_uvt(p_overlay, cam);
		return;
	}
	if (modal == MODAL_TSNAP) {
		_draw_tsnap(p_overlay, cam);
		return;
	}
	if (modal != MODAL_NONE) {
		_draw_modal(p_overlay, cam);
		return;
	}
	HewnMesh3D *n = get_edited();
	if (!n || mode == MODE_OBJECT) {
		return;
	}
	if (vpaint) {
		_draw_paint(p_overlay, cam); // (a selection tool in Vertex Paint: the color squares too)
		return;
	}
	_draw_handles(p_overlay, cam);
	_draw_bisect_widget(p_overlay, cam);
	_draw_select_region(p_overlay);
}

// The box / circle / lasso being drawn (the circle also before the press).
void HewnEditorPlugin::_draw_select_region(Control *p_overlay) {
	if (drag == DRAG_BOX) {
		const Rect2 r = Rect2(press_pos, Vector2()).expand(mouse_pos);
		p_overlay->draw_rect(r, Color(1, 0.6f, 0.1f, 0.12f));
		p_overlay->draw_rect(r, Color(1, 0.6f, 0.1f, 0.8f), false, 1.0f);
	}
	if (select_kind == 1 && (drag == DRAG_NONE || drag == DRAG_CIRCLE) && modal == MODAL_NONE) {
		p_overlay->draw_arc(mouse_pos, circle_radius, 0.0f, (float)Math::PI * 2.0f, 48, Color(1, 1, 1, drag == DRAG_CIRCLE ? 0.9f : 0.55f), 1.0f, true);
	}
	if (drag == DRAG_LASSO && lasso_pts.size() >= 2) {
		PackedVector2Array pts;
		for (const Vector2 &p : lasso_pts) {
			pts.push_back(p);
		}
		pts.push_back(lasso_pts[0]);
		p_overlay->draw_polyline(pts, Color(1, 0.6f, 0.1f, 0.9f), 1.0f, true);
	}
}

// ------------------------------------------------------------------ input

Vector2 HewnEditorPlugin::_precise_mouse(const Vector2 &p_raw, bool p_shift) {
	if (prec_active) {
		const Vector2 eff = prec_eff0 + (p_raw - prec_raw0) * 0.1f;
		if (!p_shift) {
			prec_active = false;
			prec_offset = eff - p_raw;
		}
		return eff;
	}
	const Vector2 eff = p_raw + prec_offset;
	if (p_shift) {
		prec_active = true;
		prec_raw0 = p_raw;
		prec_eff0 = eff;
	}
	return eff;
}

void HewnEditorPlugin::_reset_precise() {
	prec_active = false;
	prec_offset = Vector2();
	wrap_offset = Vector2();
	modal_cursor = mouse_pos;
}
