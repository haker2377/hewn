// A column's / a sweep's profile edited in the view: its points drawn where they shape the mesh (a column's parts
// in the plane through its axis, a sweep's cross section where its path starts) and dragged there, the mesh
// following at once; one undo step per drag.
#include "hw_editor.h"
#include "hw_editor_util.h"
#include "../hw_profile.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>

using namespace godot;

namespace {

// How one profile's points lie in the mesh's space: q (profile) -> local point, and back.
struct ProfPlace {
	String prop;
	Ref<HewnProfile> res;
	// a column part: (q.x * R, y0 + q.y * h, 0) - center; a sweep: o + side * x' + up * y'
	bool column = false;
	float R = 1.0f, y0 = 0.0f, h = 1.0f;
	Vector3 center;
	Vector3 o, side, up, nrm;
	Vector2 mn, sz;
	float w = 1.0f, hh = 1.0f, drop = 0.0f;
	bool centered = false, outside = false;

	Vector3 to3(const Vector2 &q) const {
		if (column) {
			return Vector3(q.x * R, y0 + q.y * h, 0.0f) - center;
		}
		Vector2 p(((q.x - mn.x) / sz.x - 0.5f) * w, (q.y - mn.y) / sz.y * (hh + drop));
		if (centered) {
			p.y -= (hh + drop) * 0.5f;
		}
		p.y -= drop;
		if (outside) {
			p.x += w * 0.5f;
		}
		return o + side * p.x + up * p.y;
	}
	Vector2 to2(const Vector3 &l) const {
		if (column) {
			const Vector3 a = l + center;
			return Vector2(a.x / MAX(1e-6f, R), (a.y - y0) / MAX(1e-6f, h));
		}
		Vector2 p(side.dot(l - o), up.dot(l - o));
		if (outside) {
			p.x -= w * 0.5f;
		}
		p.y += drop;
		if (centered) {
			p.y += (hh + drop) * 0.5f;
		}
		return Vector2(mn.x + (p.x / MAX(1e-6f, w) + 0.5f) * sz.x, mn.y + p.y / MAX(1e-6f, hh + drop) * sz.y);
	}
	Vector3 plane_normal() const { return column ? Vector3(0, 0, 1) : nrm; }
	Vector3 plane_point() const { return column ? -center : o; }
};

std::vector<ProfPlace> places_of(HewnMesh3D *n) {
	std::vector<ProfPlace> out;
	if (!n) {
		return out;
	}
	if (n->get_shape() == hewn::shapes::COLUMN) {
		const float R = (float)n->get_shape_param("radius"), H = (float)n->get_shape_param("height");
		float bh = (float)n->get_shape_param("base_height"), th = (float)n->get_shape_param("top_height");
		if (bh + th > H * 0.95f) {
			const float k = H * 0.95f / MAX(1e-6f, bh + th);
			bh *= k;
			th *= k;
		}
		const float shaft_h = MAX(1e-4f, H - bh - th);
		int reps = (int)n->get_shape_param("shaft_repeat") == 1 ? MAX(1, (int)std::round(shaft_h / MAX(1e-3f, (float)n->get_shape_param("shaft_length")))) : MAX(1, (int)n->get_shape_param("shaft_count"));
		reps = MIN(reps, 256);
		const char *props[3] = { "base_profile", "shaft_profile", "top_profile" };
		const float y0s[3] = { 0.0f, bh, H - th }, hs[3] = { bh, shaft_h / reps, th };
		for (int i = 0; i < 3; i++) {
			const Ref<HewnProfile> pr = n->get(props[i]);
			if (pr.is_null() || hs[i] < 1e-5f) {
				continue;
			}
			ProfPlace pp;
			pp.prop = props[i];
			pp.res = pr;
			pp.column = true;
			pp.R = R;
			pp.y0 = y0s[i];
			pp.h = hs[i];
			pp.center = n->get_column_center();
			out.push_back(pp);
		}
		return out;
	}
	if (n->get_shape() == hewn::shapes::SWEEP && (int)n->get_shape_param("profile") == 6 && n->get_sweep_profile().is_valid()) {
		const PackedVector3Array path = n->get_poly_points();
		if (path.size() < 2) {
			return out;
		}
		ProfPlace pp;
		pp.prop = "sweep_profile";
		pp.res = n->get_sweep_profile();
		const std::vector<Vector2> smp = pp.res->to_vector();
		if (smp.size() < 3) {
			return out;
		}
		Vector2 mn = smp[0], mx = smp[0];
		for (const Vector2 &q : smp) {
			mn = mn.min(q);
			mx = mx.max(q);
		}
		pp.mn = mn;
		pp.sz = (mx - mn).max(Vector2(1e-6f, 1e-6f));
		const Vector3 d = (path[1] - path[0]).normalized();
		Vector3 sd = d.cross(Vector3(0, 1, 0));
		if (sd.length() < 1e-4f) {
			sd = Vector3(1, 0, 0).cross(d).normalized().cross(d);
		}
		pp.side = sd.normalized();
		pp.up = pp.side.cross(d).normalized();
		pp.nrm = d;
		pp.o = path[0];
		pp.w = (float)n->get_shape_param("width");
		pp.hh = (float)n->get_shape_param("height");
		pp.centered = (int)n->get_shape_param("anchor") == 1;
		pp.drop = pp.centered ? 0.0f : (float)n->get_shape_param("drop");
		pp.outside = (int)n->get_shape_param("align") == 1;
		out.push_back(pp);
	}
	return out;
}

} // namespace

HewnMesh3D *HewnEditorPlugin::_prof3d_target() const {
	if (!prof3d_on || mode != MODE_OBJECT) {
		return nullptr;
	}
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	if (sel.size() != 1) {
		return nullptr;
	}
	HewnMesh3D *n = Object::cast_to<HewnMesh3D>(sel[0]);
	return n && (n->get_shape() == hewn::shapes::COLUMN || n->get_shape() == hewn::shapes::SWEEP) ? n : nullptr;
}

void HewnEditorPlugin::set_profile_in_view(bool p_on) {
	prof3d_on = p_on;
	prof3d_drag = -1;
	HewnMesh3D *n = _prof3d_target();
	if (p_on && n) {
		// (the built-in parts become curves of its own to drag: one undo step)
		const bool need = n->get_shape() == hewn::shapes::COLUMN ? (n->get_base_profile().is_null() || n->get_shaft_profile().is_null() || n->get_top_profile().is_null())
																   : n->get_sweep_profile().is_null() || (int)n->get_shape_param("profile") != 6;
		if (need) {
			EditorUndoRedoManager *ur = get_undo_redo();
			ur->create_action("Hewn: Profile in the View");
			ur->add_do_method(n, "make_profiles_editable");
			for (const char *p : { "base_profile", "shaft_profile", "top_profile", "sweep_profile" }) {
				ur->add_undo_property(n, p, n->get(p));
			}
			ur->add_undo_method(n, "set_shape_param", "profile", n->get_shape_param("profile"));
			ur->commit_action();
		}
	}
	update_overlays();
}

void HewnEditorPlugin::_draw_profile_in_view(Control *p_overlay, Camera3D *p_cam) {
	HewnMesh3D *n = _prof3d_target();
	if (!n) {
		return;
	}
	const Transform3D g = n->get_global_transform();
	const std::vector<ProfPlace> pl = places_of(n);
	int k = 0;
	for (const ProfPlace &pp : pl) {
		const PackedVector2Array pts = pp.res->get_points();
		Vector2 prev;
		bool has_prev = false;
		// the outline as it is sampled (the line), the points on it (the handles)
		const std::vector<Vector2> smp = pp.res->to_vector();
		for (size_t i = 0; i < smp.size(); i++) {
			const Vector3 w = g.xform(pp.to3(smp[i]));
			if (p_cam->is_position_behind(w)) {
				has_prev = false;
				continue;
			}
			const Vector2 s = p_cam->unproject_position(w);
			if (has_prev) {
				p_overlay->draw_line(prev, s, Color(0.3f, 0.9f, 1.0f, 0.85f), 1.5f, true);
			}
			prev = s;
			has_prev = true;
		}
		for (int64_t i = 0; i < pts.size(); i++, k++) {
			const Vector3 w = g.xform(pp.to3(pts[i]));
			if (p_cam->is_position_behind(w)) {
				continue;
			}
			const Vector2 s = p_cam->unproject_position(w);
			const bool on = k == prof3d_drag;
			p_overlay->draw_circle(s, on ? 6.5f : 5.0f, Color(0, 0, 0, 0.6f));
			p_overlay->draw_circle(s, on ? 5.0f : 3.5f, on ? Color(1, 1, 1) : Color(0.3f, 0.9f, 1.0f));
		}
	}
}

int32_t HewnEditorPlugin::_profile_in_view_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	HewnMesh3D *n = _prof3d_target();
	if (!n) {
		prof3d_drag = -1;
		return AFTER_GUI_INPUT_PASS;
	}
	const Transform3D g = n->get_global_transform();
	const std::vector<ProfPlace> pl = places_of(n);
	auto find = [&](int p_k, const ProfPlace *&r_pp, int &r_i) {
		int k = 0;
		for (const ProfPlace &pp : pl) {
			const int c = (int)pp.res->get_points().size();
			if (p_k < k + c) {
				r_pp = &pp;
				r_i = p_k - k;
				return true;
			}
			k += c;
		}
		return false;
	};
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		if (mb->is_pressed()) {
			int k = 0, best = -1;
			float best_d = 10.0f;
			for (const ProfPlace &pp : pl) {
				const PackedVector2Array pts = pp.res->get_points();
				for (int64_t i = 0; i < pts.size(); i++, k++) {
					const Vector3 w = g.xform(pp.to3(pts[i]));
					if (!p_cam->is_position_behind(w)) {
						const float d = p_cam->unproject_position(w).distance_to(mb->get_position());
						if (d < best_d) {
							best_d = d;
							best = k;
						}
					}
				}
			}
			if (best < 0) {
				return AFTER_GUI_INPUT_PASS;
			}
			const ProfPlace *pp = nullptr;
			int idx = 0;
			find(best, pp, idx);
			prof3d_drag = best;
			prof3d_res = pp->res;
			prof3d_before = pp->res->get_state();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (prof3d_drag >= 0) {
			prof3d_drag = -1;
			if (prof3d_res.is_valid()) {
				EditorUndoRedoManager *ur = get_undo_redo();
				ur->create_action("Hewn: Move Profile Point", UndoRedo::MERGE_DISABLE, n);
				ur->add_do_method(prof3d_res.ptr(), "set_state", prof3d_res->get_state());
				ur->add_undo_method(prof3d_res.ptr(), "set_state", prof3d_before);
				ur->commit_action(false);
			}
			prof3d_res.unref();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (mm.is_valid() && prof3d_drag >= 0) {
		const ProfPlace *pp = nullptr;
		int idx = 0;
		if (!find(prof3d_drag, pp, idx)) {
			return AFTER_GUI_INPUT_STOP;
		}
		Vector3 o, d;
		_ray(p_cam, mm->get_position(), o, d);
		const Transform3D inv = g.affine_inverse();
		const Vector3 lo = inv.xform(o), ld = inv.basis.xform(d);
		const Vector3 pn = pp->plane_normal(), pc = pp->plane_point();
		const float den = pn.dot(ld);
		if (std::fabs(den) < 1e-6f) {
			return AFTER_GUI_INPUT_STOP;
		}
		const Vector3 hit = lo + ld * (pn.dot(pc - lo) / den);
		Vector2 q = pp->to2(hit);
		// (on the grid of the snap step, in meters, when snapping is on)
		if (dock && dock->snap_enabled && dock->snap_enabled->is_pressed() && dock->snap_move) {
			const float st = MAX(1e-4f, (float)dock->snap_move->get_value());
			const Vector3 snapped = ((hit) / st).round() * st;
			q = pp->to2(snapped - pn * pn.dot(snapped - pc));
		}
		if (pp->column) {
			q = Vector2(MAX(0.0f, q.x), CLAMP(q.y, 0.0f, 1.0f));
		}
		pp->res->set_point(idx, q);
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	return AFTER_GUI_INPUT_PASS;
}
