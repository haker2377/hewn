// Editing a Hewn curve (Tab on it): its points and their handles. Click / Shift / box: select, A / Alt+A; G / R / S
// (X / Y / Z: an axis, again: local; Ctrl: snapping inverted; Shift+Tab: snapping on / off; B: from one point onto
// another), Alt+S: radius, Ctrl+T: tilt, Alt+T: tilt cleared; E: extrude (a new point after an end; in the middle a
// spline of its own from there), F: make a segment, V: handle types, Alt+C: cyclic, Shift+N: handles worked out again,
// X / Delete: delete (points, segments, dissolve), Ctrl+X: dissolve, P: separate; RMB: the menu of all of it; drag a
// handle; Ctrl+click: a new point at the mouse. Tab: done.
#include "hw_editor.h"
#include "hw_editor_util.h"
#include "../hw_curve.h"
#include "../hw_path_place.h"
#include "hw_textures.h"
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/cylinder_mesh.hpp>

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>

#include <algorithm>
#include <map>
#include <set>

using namespace godot;
using namespace hw_editor;
using hewn::curves::CPoint;
using hewn::curves::Spline;

HewnCurve3D *HewnEditorPlugin::_hc() const {
	HewnCurve3D *c = curve_id.is_valid() ? Object::cast_to<HewnCurve3D>(ObjectDB::get_instance(curve_id)) : nullptr;
	return c && c->is_inside_tree() ? c : nullptr;
}

void HewnEditorPlugin::_hc_begin() {
	if (HewnCurve3D *c = _hc()) {
		if (hc_before.is_empty()) {
			hc_before = c->get_state();
		}
	}
}

// One undo step: the curve as it was when the change began, and as it is.
void HewnEditorPlugin::_hc_commit(const String &p_name) {
	HewnCurve3D *c = _hc();
	if (!c || hc_before.is_empty()) {
		return;
	}
	const Dictionary after = c->get_state();
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: " + p_name, UndoRedo::MERGE_DISABLE, c);
	ur->add_do_method(c, "set_state", after);
	ur->add_undo_method(c, "set_state", hc_before);
	ur->commit_action(false);
	hc_before = Dictionary();
	// (a selection kept as the undo / redo brings the points back: the state has none)
	update_overlays();
}

namespace {

Vector3 cam_forward(Camera3D *p_cam) {
	return -p_cam->get_global_transform().basis.get_column(2).normalized();
}

int selected_count(const std::vector<Spline> &ss) {
	int n = 0;
	for (const Spline &s : ss) {
		for (const CPoint &p : s.pts) {
			n += p.sel ? 1 : 0;
			n += !p.sel && s.type == hewn::curves::BEZIER ? (p.sel_in ? 1 : 0) + (p.sel_out ? 1 : 0) : 0;
		}
	}
	return n;
}

void clear_handle_sel(std::vector<Spline> &ss) {
	for (Spline &s : ss) {
		for (CPoint &p : s.pts) {
			p.sel_in = p.sel_out = false;
		}
	}
}

// A handle moved on its own: Auto / Vector become Aligned (both sides), an Aligned other side keeps the line.
void handle_moved(CPoint &p, bool p_in) {
	int &mine = p_in ? p.hl : p.hr;
	int &other = p_in ? p.hr : p.hl;
	if (mine == hewn::curves::H_AUTO || mine == hewn::curves::H_VECTOR) {
		mine = hewn::curves::H_ALIGNED;
		if (other == hewn::curves::H_AUTO || other == hewn::curves::H_VECTOR) {
			other = hewn::curves::H_ALIGNED;
		}
	}
	const Vector3 h = p_in ? p.in : p.out;
	if (mine == hewn::curves::H_ALIGNED && other == hewn::curves::H_ALIGNED && h.length_squared() > 1e-12f) {
		Vector3 &o = p_in ? p.out : p.in;
		o = -h.normalized() * o.length();
	}
}

bool spline_selected(const Spline &s) {
	for (const CPoint &p : s.pts) {
		if (p.sel) {
			return true;
		}
	}
	return false;
}

// a Bezier segment cut in two at t: the new point between (its handles), the ends' handles shortened
CPoint split_bezier(CPoint &a, CPoint &b, float t) {
	const Vector3 p0 = a.pos, p1 = a.pos + a.out, p2 = b.pos + b.in, p3 = b.pos;
	const Vector3 q0 = p0.lerp(p1, t), q1 = p1.lerp(p2, t), q2 = p2.lerp(p3, t);
	const Vector3 r0 = q0.lerp(q1, t), r1 = q1.lerp(q2, t);
	const Vector3 s = r0.lerp(r1, t);
	CPoint m;
	m.pos = s;
	m.in = r0 - s;
	m.out = r1 - s;
	m.tilt = a.tilt + (b.tilt - a.tilt) * t;
	m.radius = a.radius + (b.radius - a.radius) * t;
	m.weight = a.weight + (b.weight - a.weight) * t;
	m.hl = m.hr = hewn::curves::H_ALIGNED;
	a.out = q0 - p0;
	b.in = q2 - p3;
	if (a.hr == hewn::curves::H_AUTO || a.hr == hewn::curves::H_VECTOR) {
		a.hr = hewn::curves::H_FREE;
	}
	if (b.hl == hewn::curves::H_AUTO || b.hl == hewn::curves::H_VECTOR) {
		b.hl = hewn::curves::H_FREE;
	}
	return m;
}

// Bevel: each selected point (not an open end) cut off, `p_d` from it along its two pieces (at most halfway to a
// selected neighbor), the cut made of `p_segs` pieces bent by `p_profile` (0 straight, 0.5 round, 1 to the corner).
void bevel_splines(std::vector<Spline> &ss, float p_d, int p_segs, float p_profile) {
	p_segs = MAX(1, p_segs);
	for (Spline &sp : ss) {
		const int n = (int)sp.pts.size();
		if (n < 2 || p_d <= 1e-7f) {
			continue;
		}
		std::vector<CPoint> out;
		for (int i = 0; i < n; i++) {
			const CPoint &p = sp.pts[i];
			const bool end = !sp.cyclic && (i == 0 || i == n - 1);
			if (!p.sel || end) {
				out.push_back(p);
				continue;
			}
			const CPoint &pa = sp.pts[(i - 1 + n) % n], &pb = sp.pts[(i + 1) % n];
			const float la = pa.pos.distance_to(p.pos), lb = pb.pos.distance_to(p.pos);
			if (la < 1e-6f || lb < 1e-6f) {
				out.push_back(p);
				continue;
			}
			const float da = MIN(p_d, la * (pa.sel ? 0.5f : 0.999f)), db = MIN(p_d, lb * (pb.sel ? 0.5f : 0.999f));
			const Vector3 c0 = p.pos + (pa.pos - p.pos) / la * da, c1 = p.pos + (pb.pos - p.pos) / lb * db;
			const Vector3 mid = (c0 + c1) * 0.5f;
			const Vector3 q = mid + (p.pos - mid) * CLAMP(p_profile, 0.0f, 1.0f) * 2.0f; // (the bend's pull)
			for (int k = 0; k <= p_segs; k++) {
				const float t = (float)k / p_segs;
				CPoint c = p;
				c.pos = c0 * ((1 - t) * (1 - t)) + q * (2 * t * (1 - t)) + c1 * (t * t);
				c.in = c.out = Vector3();
				c.hl = c.hr = hewn::curves::H_VECTOR;
				out.push_back(c);
			}
		}
		sp.pts = out;
		hewn::curves::recalc_handles(sp);
	}
}

// Subdivide: `p_cuts` points evenly between each two selected neighbors (a Bezier piece keeps its shape).
void subdivide_splines(std::vector<Spline> &ss, int p_cuts) {
	p_cuts = MAX(1, p_cuts);
	for (Spline &s : ss) {
		const int n = (int)s.pts.size();
		if (n < 2) {
			continue;
		}
		std::vector<CPoint> out;
		for (int i = 0; i < n; i++) {
			out.push_back(s.pts[i]);
			const int j = (i + 1) % n;
			if ((j == 0 && !s.cyclic) || !s.pts[i].sel || !s.pts[j].sel) {
				continue;
			}
			if (s.type == hewn::curves::BEZIER) {
				CPoint b = j == 0 ? out[0] : s.pts[j];
				for (int k = p_cuts; k >= 1; k--) {
					CPoint a = out.back();
					CPoint m = split_bezier(a, b, 1.0f / (k + 1));
					out.back() = a;
					m.sel = true;
					out.push_back(m);
				}
				CPoint &dst = j == 0 ? out[0] : s.pts[j];
				dst.in = b.in;
				dst.hl = b.hl;
			} else {
				for (int k = 1; k <= p_cuts; k++) {
					const float t = (float)k / (p_cuts + 1);
					CPoint m = s.pts[i];
					m.pos = s.pts[i].pos.lerp(s.pts[j].pos, t);
					m.tilt = Math::lerp(s.pts[i].tilt, s.pts[j].tilt, t);
					m.radius = Math::lerp(s.pts[i].radius, s.pts[j].radius, t);
					m.sel = true;
					out.push_back(m);
				}
			}
		}
		s.pts = out;
	}
}

void reverse_spline(Spline &s) {
	std::reverse(s.pts.begin(), s.pts.end());
	for (CPoint &p : s.pts) {
		std::swap(p.in, p.out);
		std::swap(p.hl, p.hr);
	}
}

} // namespace

Vector3 HewnEditorPlugin::_hc_center() const {
	HewnCurve3D *c = _hc();
	Vector3 sum;
	int n = 0;
	if (c) {
		const Transform3D g = c->get_global_transform();
		for (const Spline &s : c->get_splines()) {
			for (const CPoint &p : s.pts) {
				if (p.sel) {
					sum += g.xform(p.pos);
					n++;
				} else if (s.type == hewn::curves::BEZIER) {
					if (p.sel_in) {
						sum += g.xform(p.pos + p.in);
						n++;
					}
					if (p.sel_out) {
						sum += g.xform(p.pos + p.out);
						n++;
					}
				}
			}
		}
	}
	return n > 0 ? sum / (float)n : (c ? c->get_global_transform().origin : Vector3());
}

void HewnEditorPlugin::_hc_start_xf(int p_kind) {
	HewnCurve3D *c = _hc();
	if (!c || selected_count(c->get_splines()) == 0) {
		return;
	}
	_hc_begin();
	hc_xf = p_kind;
	hc_axis = -1;
	hc_local = false;
	_reset_precise();
	hc_mouse0 = mouse_pos;
	hc_center = _hc_center();
	hc_start = c->get_splines();
	hc_bpick = 0;
	hc_num = String();
	hc_trackball = false;
	hc_plane = false;
	_gather_snap_candidates(false);
	update_overlays();
}

// G / R / S / radius / tilt from where they began, to the mouse now
void HewnEditorPlugin::_hc_apply_xf(Camera3D *p_cam, bool p_ctrl) {
	HewnCurve3D *c = _hc();
	if (!c) {
		return;
	}
	const Transform3D g = c->get_global_transform();
	const Basis inv = g.basis.inverse();
	std::vector<Spline> &ss = c->get_splines();
	ss = hc_start;
	const bool snap = snapping_active(p_ctrl);
	Vector3 axis_dir;
	if (hc_axis >= 0) {
		axis_dir = hc_local ? g.basis.get_column(hc_axis).normalized() : AXES[hc_axis];
	}
	auto plane_hit = [&](const Vector2 &p_pos, Vector3 &r_hit) {
		const Vector3 o = p_cam->project_ray_origin(p_pos), d = p_cam->project_ray_normal(p_pos);
		const Vector3 n = p_cam->get_global_transform().basis.get_column(2).normalized();
		const float den = n.dot(d);
		if (std::fabs(den) < 1e-6f) {
			return false;
		}
		r_hit = o + d * (n.dot(hc_center - o) / den);
		return true;
	};
	const Vector2 cc = p_cam->unproject_position(hc_center);
	double typed = 0.0;
	const bool has_typed = !hc_num.is_empty() && HewnDragField::eval_expression(hc_num == "-" ? String("0") : hc_num, typed);
	if (hc_xf == 7) {
		// Ctrl+B: each selected point (not an open end) cut off: two points on its sides, as far from it as the mouse
		// is farther from the center than it began (no further than halfway to a selected neighbor)
		float d = 0.0f;
		Vector3 a, b;
		if (has_typed) {
			d = (float)typed;
		} else if (plane_hit(hc_mouse0, a) && plane_hit(mouse_pos, b)) {
			d = b.distance_to(hc_center) - a.distance_to(hc_center);
		}
		if (snap && dock && !has_typed) {
			const float st = MAX(1e-4f, (float)dock->snap_move->get_value());
			d = std::round(d / st) * st;
		}
		d = MAX(0.0f, d / MAX(1e-6f, g.basis.get_scale().x));
		hc_factor = d;
		bevel_splines(ss, d, hc_bevel_segs, (float)op_param("hc_bevel", "profile"));
		c->changed();
		return;
	}
	if (hc_xf == 8) {
		// GG: each selected point slid along its pieces: toward the next point (the mouse along the first one's
		// piece on the screen), or back toward the one before
		float f = 0.0f;
		if (has_typed) {
			f = (float)typed;
		} else if (hc_slide_dir.length_squared() > 1e-6f) {
			f = (mouse_pos - hc_mouse0).dot(hc_slide_dir) / hc_slide_dir.length_squared();
		}
		f = CLAMP(f, -1.0f, 1.0f);
		hc_factor = f;
		for (size_t si = 0; si < ss.size(); si++) {
			Spline &sp = ss[si];
			const std::vector<CPoint> &orig = hc_start[si].pts;
			const int n = (int)orig.size();
			for (int i = 0; i < n; i++) {
				if (!orig[i].sel) {
					continue;
				}
				const int j = f >= 0.0f ? i + 1 : i - 1;
				if (!sp.cyclic && (j < 0 || j >= n)) {
					continue;
				}
				sp.pts[i].pos = orig[i].pos.lerp(orig[(j + n) % n].pos, std::fabs(f));
			}
			hewn::curves::recalc_handles(sp);
		}
		c->changed();
		return;
	}
	if (hc_xf == 1) {
		Vector3 d;
		if (has_typed) {
			d = (hc_axis >= 0 ? axis_dir : AXES[0]) * (float)typed; // (no axis: along X)
		} else if (hc_bpick == 2) {
			d = hc_btarget - hc_bsource; // (B: the source point onto the target)
		} else if (hc_axis >= 0 && hc_plane) {
			// (on the plane across the axis, through the center)
			Vector3 a, b;
			if (_plane_hit(p_cam, hc_mouse0, hc_center, axis_dir, a) && _plane_hit(p_cam, mouse_pos, hc_center, axis_dir, b)) {
				d = b - a;
			}
		} else if (hc_axis >= 0) {
			d = axis_dir * (_axis_param(p_cam, mouse_pos, hc_center, axis_dir) - _axis_param(p_cam, hc_mouse0, hc_center, axis_dir));
		} else {
			Vector3 a, b;
			if (plane_hit(hc_mouse0, a) && plane_hit(mouse_pos, b)) {
				d = b - a;
			}
		}
		Vector3 target;
		// (the points of the curves, this one's not moving and the others', are targets too)
		bool on_point = false;
		if (!has_typed && hc_bpick == 0 && snap && dock && (dock->snap_targets & HewnDock::SNAP_POINTS)) {
			float bd = pick_px;
			std::vector<HewnCurve3D *> curves;
			Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
			std::vector<Node *> st;
			if (root) {
				st.push_back(root);
			}
			while (!st.empty()) {
				Node *x = st.back();
				st.pop_back();
				for (int i = 0; i < x->get_child_count(); i++) {
					st.push_back(x->get_child(i));
				}
				if (HewnCurve3D *oc = Object::cast_to<HewnCurve3D>(x)) {
					if (oc->is_visible_in_tree()) {
						curves.push_back(oc);
					}
				}
			}
			for (HewnCurve3D *oc : curves) {
				const Transform3D og = oc->get_global_transform();
				const std::vector<Spline> &os = oc == c ? hc_start : oc->get_splines_const();
				for (const Spline &sp : os) {
					for (const CPoint &pt : sp.pts) {
						if (oc == c && pt.sel) {
							continue;
						}
						const Vector3 w = og.xform(pt.pos);
						if (p_cam->is_position_behind(w)) {
							continue;
						}
						const float dd = p_cam->unproject_position(w).distance_to(mouse_pos);
						if (dd < bd) {
							bd = dd;
							target = w;
							on_point = true;
						}
					}
				}
			}
		}
		if (has_typed || hc_bpick == 2) {
			hc_snapped = false;
		} else if (hc_bpick == 0 && snap && dock && (on_point || ((dock->snap_targets & HewnDock::SNAP_GEOMETRY) && _snap_target(p_cam, mouse_pos, target)))) {
			// the selected point nearest to the target onto it
			float best = 1e30f;
			Vector3 base = hc_center;
			for (const Spline &s : hc_start) {
				for (const CPoint &p : s.pts) {
					if (p.sel && g.xform(p.pos).distance_to(target) < best) {
						best = g.xform(p.pos).distance_to(target);
						base = g.xform(p.pos);
					}
				}
			}
			d = target - base;
			if (hc_axis >= 0) {
				d = hc_plane ? d - axis_dir * d.dot(axis_dir) : axis_dir * d.dot(axis_dir);
			}
			hc_snapped = true;
			hc_snap_point = target;
		} else if (hc_bpick == 0 && snap && dock && (dock->snap_targets & HewnDock::SNAP_STEPS)) {
			const float st = MAX(1e-4f, (float)dock->snap_move->get_value());
			d = Vector3(std::round(d.x / st) * st, std::round(d.y / st) * st, std::round(d.z / st) * st);
			hc_snapped = false;
		} else {
			hc_snapped = false;
		}
		const Vector3 ld = inv.xform(d);
		for (Spline &s : ss) {
			for (CPoint &p : s.pts) {
				if (p.sel) {
					p.pos += ld;
				} else if (s.type == hewn::curves::BEZIER) {
					// (a handle selected on its own: its end moves, the point stays)
					if (p.sel_in) {
						p.in += ld;
						handle_moved(p, true);
					}
					if (p.sel_out) {
						p.out += ld;
						handle_moved(p, false);
					}
				}
			}
		}
	} else if (hc_xf == 2 || hc_xf == 5) {
		float ang = -((mouse_pos - cc).angle() - (hc_mouse0 - cc).angle());
		Basis track; // (the trackball: turned by the mouse's way across the view, about the view's own axes)
		const bool tb = hc_xf == 2 && hc_trackball && hc_axis < 0 && !has_typed && hc_bpick != 2;
		if (tb) {
			const Vector2 dm = mouse_pos - hc_mouse0;
			const Basis cb = p_cam->get_global_transform().basis;
			const float k = 0.01f;
			track = Basis(cb.get_column(1).normalized(), dm.x * k) * Basis(cb.get_column(0).normalized(), dm.y * k);
		}
		if (hc_bpick == 2) {
			// B: the source turned about the center onto the target's direction (in the view, or about the axis)
			const Vector2 s0 = p_cam->unproject_position(hc_bsource), s1 = p_cam->unproject_position(hc_btarget);
			ang = -((s1 - cc).angle() - (s0 - cc).angle());
		}
		if (has_typed) {
			ang = Math::deg_to_rad((float)typed);
		} else if (snap && dock && hc_bpick != 2) {
			const float st = Math::deg_to_rad(MAX(0.1f, (float)dock->snap_rotate->get_value()));
			ang = std::round(ang / st) * st;
		}
		if (hc_xf == 5) {
			for (Spline &s : ss) {
				for (CPoint &p : s.pts) {
					if (p.sel) {
						p.tilt += ang;
					}
				}
			}
		} else {
			Vector3 ax = hc_axis >= 0 ? axis_dir : p_cam->get_global_transform().basis.get_column(2).normalized();
			if (hc_axis < 0 && ax.dot(p_cam->get_global_position() - hc_center) < 0.0f) {
				ax = -ax;
			}
			const Basis rot = tb ? inv * track * g.basis : Basis(inv.xform(ax).normalized(), ang);
			const Vector3 lc = g.affine_inverse().xform(hc_center);
			for (Spline &s : ss) {
				for (CPoint &p : s.pts) {
					if (p.sel) {
						p.pos = lc + rot.xform(p.pos - lc);
						p.in = rot.xform(p.in);
						p.out = rot.xform(p.out);
					} else if (s.type == hewn::curves::BEZIER) {
						if (p.sel_in) {
							p.in = lc + rot.xform(p.pos + p.in - lc) - p.pos;
							handle_moved(p, true);
						}
						if (p.sel_out) {
							p.out = lc + rot.xform(p.pos + p.out - lc) - p.pos;
							handle_moved(p, false);
						}
					}
				}
			}
		}
	} else if (hc_xf == 3 || hc_xf == 4) {
		float f = mouse_pos.distance_to(cc) / MAX(1.0f, hc_mouse0.distance_to(cc));
		if (hc_bpick == 2) {
			// B: as far from the center as the target is, from as far as the source was
			f = hc_btarget.distance_to(hc_center) / MAX(1e-6f, hc_bsource.distance_to(hc_center));
		} else if (has_typed) {
			f = (float)typed;
		} else if (snap) {
			const float st = dock ? MAX(0.001f, (float)dock->snap_scale->get_value()) : 0.1f;
			f = std::round(f / st) * st;
		}
		const Vector3 lc = g.affine_inverse().xform(hc_center);
		const Vector3 lax = hc_axis >= 0 ? inv.xform(axis_dir).normalized() : Vector3();
		for (Spline &s : ss) {
			for (CPoint &p : s.pts) {
				if (!p.sel && hc_xf == 3 && s.type == hewn::curves::BEZIER && (p.sel_in || p.sel_out)) {
					auto sh = [&](const Vector3 &v) { return hc_axis < 0 ? v * f : hc_plane ? v * f - lax * (v.dot(lax) * (f - 1.0f)) : v + lax * (v.dot(lax) * (f - 1.0f)); };
					if (p.sel_in) {
						p.in = lc + sh(p.pos + p.in - lc) - p.pos;
						handle_moved(p, true);
					}
					if (p.sel_out) {
						p.out = lc + sh(p.pos + p.out - lc) - p.pos;
						handle_moved(p, false);
					}
					continue;
				}
				if (!p.sel) {
					continue;
				}
				if (hc_xf == 4) {
					p.radius = MAX(0.0f, p.radius * f);
					continue;
				}
				// (an axis: along it; Shift: the other two)
				auto sc = [&](const Vector3 &v) { return hc_axis < 0 ? v * f : hc_plane ? v * f - lax * (v.dot(lax) * (f - 1.0f)) : v + lax * (v.dot(lax) * (f - 1.0f)); };
				p.pos = lc + sc(p.pos - lc);
				p.in = sc(p.in);
				p.out = sc(p.out);
			}
		}
		hc_factor = f;
	}
	for (Spline &s : ss) {
		hewn::curves::recalc_handles(s);
	}
	c->changed();
}

void HewnEditorPlugin::_hc_end_xf(bool p_cancel) {
	HewnCurve3D *c = _hc();
	if (!c) {
		hc_xf = 0;
		return;
	}
	if (p_cancel && hc_duplicated) {
		// (a duplicate let go: the copies stay where they began, as one undo step)
		c->get_splines() = hc_start;
		c->changed();
		_hc_commit("Duplicate Curve Points");
	} else if (p_cancel) {
		c->get_splines() = hc_start;
		c->changed();
		hc_before = Dictionary();
	} else if (hc_xf == 7) {
		// (as one step with its panel: width, segments, shape)
		c->get_splines() = hc_start;
		c->changed();
		hc_before = Dictionary();
		hc_xf = 0;
		set_op_param("hc_bevel", "width", hc_factor);
		set_op_param("hc_bevel", "segments", hc_bevel_segs);
		start_scene_op("hc_bevel");
	} else {
		static const char *names[9] = { "", "Move Curve Points", "Rotate Curve Points", "Scale Curve Points", "Curve Radius", "Curve Tilt", "Curve Handle", "Bevel Curve Points", "Slide Curve Points" };
		_hc_commit(hc_extruded ? "Extrude Curve" : hc_duplicated ? "Duplicate Curve Points" : names[CLAMP(hc_xf, 0, 8)]);
	}
	hc_xf = 0;
	hc_extruded = false;
	hc_duplicated = false;
	hc_snapped = false;
	update_overlays();
}

// The curve operations (the menu, the keys): on the selected points / their splines; one undo step each.
void HewnEditorPlugin::hc_op(const String &p_op) {
	HewnCurve3D *c = _hc();
	if (!c) {
		return;
	}
	std::vector<Spline> &ss = c->get_splines();
	const Transform3D g = c->get_global_transform();
	const Transform3D ginv = g.affine_inverse();
	if (p_op == "select_all" || p_op == "select_none" || p_op == "select_invert") {
		for (Spline &s : ss) {
			for (CPoint &p : s.pts) {
				p.sel = p_op == "select_all" ? true : p_op == "select_none" ? false : !p.sel;
				p.sel_in = p.sel_out = false;
			}
		}
		update_overlays();
		return;
	}
	if (p_op == "draw") {
		hc_draw = true;
		hc_draw_pts.clear();
		// (its panel now: on the surfaces or from the 3D cursor chosen before drawing)
		_hide_redo();
		last_op = "hc_draw";
		scene_op.history = -1;
		_show_redo();
		update_overlays();
		return;
	}
	if (p_op == "radius_xf" || p_op == "bevel_xf" || p_op == "tilt_xf" || p_op == "extrude" || p_op == "duplicate") {
		Camera3D *cam = _viewport_camera();
		if (!cam) {
			return;
		}
		if (p_op == "duplicate") {
			_hc_duplicate(cam);
		} else if (p_op == "extrude") {
			_hc_extrude(cam);
		} else {
			_hc_start_xf(p_op == "radius_xf" ? 4 : p_op == "bevel_xf" ? 7 : 5);
		}
		return;
	}
	if (p_op == "subdivide") {
		start_scene_op("hc_subdivide");
		return;
	}
	if (p_op == "merge") {
		_hc_begin();
		_hc_merge();
		c->changed();
		_hc_commit("Merge Curve Points");
		update_overlays();
		return;
	}
	if (p_op == "set_radius" || p_op == "goal_weight" || p_op == "simplify") {
		start_scene_op(p_op == "set_radius" ? "hc_radius" : p_op == "goal_weight" ? "hc_weight" : "hc_simplify");
		return;
	}
	if (p_op == "separate") {
		_hc_separate();
		return;
	}
	_hc_begin();
	bool done = true;
	if (p_op == "make_segment") {
		// two ends of two splines joined (the one end after the other), or both ends of one: closed
		std::vector<std::pair<int, int>> ends;
		for (int si = 0; si < (int)ss.size(); si++) {
			const Spline &s = ss[si];
			if (s.cyclic || s.pts.empty()) {
				continue;
			}
			const int last = (int)s.pts.size() - 1;
			if (s.pts[0].sel) {
				ends.push_back({ si, 0 });
			}
			if (last > 0 && s.pts[last].sel) {
				ends.push_back({ si, last });
			}
		}
		if (ends.size() == 2 && ends[0].first == ends[1].first) {
			ss[ends[0].first].cyclic = true;
		} else if (ends.size() == 2) {
			Spline a = ss[ends[0].first], b = ss[ends[1].first];
			if (ends[0].second == 0) {
				reverse_spline(a); // (its selected end last)
			}
			if (ends[1].second != 0) {
				reverse_spline(b); // (its selected end first)
			}
			if (b.type != a.type) {
				b.type = a.type;
			}
			a.pts.insert(a.pts.end(), b.pts.begin(), b.pts.end());
			const int hi = std::max(ends[0].first, ends[1].first), lo = std::min(ends[0].first, ends[1].first);
			ss.erase(ss.begin() + hi);
			ss[lo] = a;
		} else {
			done = false;
			if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
				t->push_toast(T("Make Segment: select two ends (of two splines, or both ends of one)"));
			}
		}
	} else if (p_op == "clear_tilt") {
		for (Spline &s : ss) {
			for (CPoint &p : s.pts) {
				if (p.sel) {
					p.tilt = 0.0f;
				}
			}
		}
	} else if (p_op == "smooth" || p_op == "smooth_tilt" || p_op == "smooth_radius") {
		for (Spline &s : ss) {
			const int n = (int)s.pts.size();
			const std::vector<CPoint> old = s.pts;
			for (int i = 0; i < n; i++) {
				if (!old[i].sel || (!s.cyclic && (i == 0 || i == n - 1)) || n < 3) {
					continue;
				}
				const CPoint &a = old[(i - 1 + n) % n], &b = old[(i + 1) % n];
				if (p_op == "smooth") {
					s.pts[i].pos = old[i].pos.lerp((a.pos + b.pos) * 0.5f, 0.5f);
				} else if (p_op == "smooth_tilt") {
					s.pts[i].tilt = old[i].tilt * 0.5f + (a.tilt + b.tilt) * 0.25f;
				} else {
					s.pts[i].radius = old[i].radius * 0.5f + (a.radius + b.radius) * 0.25f;
				}
			}
		}
	} else if (p_op.begins_with("mirror_")) {
		// mirror_<x|y|z>_<g|l>: about the selection's middle, along a global / the curve's own axis
		const int axis = p_op[7] == 'x' ? 0 : p_op[7] == 'y' ? 1 : 2;
		const bool local = p_op.ends_with("_l");
		const Vector3 lc = ginv.xform(_hc_center());
		const Vector3 ax = local ? AXES[axis] : g.basis.inverse().xform(AXES[axis]).normalized();
		auto refl = [&](const Vector3 &v) { return v - ax * (2.0f * v.dot(ax)); };
		for (Spline &s : ss) {
			for (CPoint &p : s.pts) {
				if (p.sel) {
					p.pos = lc + refl(p.pos - lc);
					p.in = refl(p.in);
					p.out = refl(p.out);
				}
			}
		}
	} else if (p_op.begins_with("snap_")) {
		const float st = dock ? MAX(1e-4f, (float)dock->snap_move->get_value()) : 1.0f;
		auto grid = [&](const Vector3 &w) { return Vector3(std::round(w.x / st) * st, std::round(w.y / st) * st, std::round(w.z / st) * st); };
		const Vector3 center = _hc_center();
		Vector3 active = center;
		for (const Spline &s : ss) {
			for (const CPoint &p : s.pts) {
				if (p.sel) {
					active = g.xform(p.pos); // (the last selected)
				}
			}
		}
		if (p_op == "snap_sel_grid" || p_op == "snap_sel_cursor" || p_op == "snap_sel_cursor_offset") {
			for (Spline &s : ss) {
				for (CPoint &p : s.pts) {
					if (!p.sel) {
						continue;
					}
					const Vector3 w = g.xform(p.pos);
					const Vector3 to = p_op == "snap_sel_grid" ? grid(w) : p_op == "snap_sel_cursor" ? cursor_pos : cursor_pos + (w - center);
					p.pos = ginv.xform(to);
				}
			}
		} else {
			done = false;
			const Vector3 to = p_op == "snap_cursor_sel" ? center : p_op == "snap_cursor_origin" ? Vector3() : p_op == "snap_cursor_grid" ? grid(cursor_pos) : active;
			cursor_pos = to;
		}
	} else if (p_op.begins_with("type_")) {
		const int t = p_op == "type_poly" ? hewn::curves::POLY : p_op == "type_bezier" ? hewn::curves::BEZIER : hewn::curves::NURBS;
		for (Spline &s : ss) {
			if (spline_selected(s) && s.type != t) {
				s.type = t;
				if (t == hewn::curves::BEZIER) {
					for (CPoint &p : s.pts) {
						p.hl = p.hr = hewn::curves::H_AUTO;
					}
				}
			}
		}
	} else if (p_op.begins_with("handle_")) {
		for (Spline &s : ss) {
			for (CPoint &p : s.pts) {
				if (!p.sel) {
					continue;
				}
				if (p_op == "handle_toggle") {
					const int t = p.hl == hewn::curves::H_FREE ? hewn::curves::H_ALIGNED : hewn::curves::H_FREE;
					p.hl = p.hr = t;
				} else {
					const int t = p_op == "handle_auto" ? hewn::curves::H_AUTO : p_op == "handle_vector" ? hewn::curves::H_VECTOR : p_op == "handle_aligned" ? hewn::curves::H_ALIGNED : hewn::curves::H_FREE;
					p.hl = p.hr = t;
				}
			}
		}
	} else if (p_op == "cyclic") {
		for (Spline &s : ss) {
			if (spline_selected(s)) {
				s.cyclic = !s.cyclic;
			}
		}
	} else if (p_op == "direction") {
		for (Spline &s : ss) {
			if (spline_selected(s)) {
				reverse_spline(s);
			}
		}
	} else if (p_op == "recalc") {
		for (Spline &s : ss) {
			for (CPoint &p : s.pts) {
				if (p.sel) {
					p.hl = p.hr = hewn::curves::H_AUTO;
				}
			}
		}
	} else if (p_op == "split") {
		// the selected points' runs: splines of their own (the rest stays, cut where they were)
		std::vector<Spline> out;
		for (const Spline &s : ss) {
			if (!spline_selected(s)) {
				out.push_back(s);
				continue;
			}
			Spline cur = s, rest = s;
			cur.pts.clear();
			cur.cyclic = false;
			rest.pts.clear();
			rest.cyclic = false;
			for (const CPoint &p : s.pts) {
				if (p.sel) {
					if (!rest.pts.empty()) {
						if (rest.pts.size() >= 2) {
							out.push_back(rest);
						}
						rest.pts.clear();
					}
					cur.pts.push_back(p);
				} else {
					if (!cur.pts.empty()) {
						out.push_back(cur);
						cur.pts.clear();
					}
					rest.pts.push_back(p);
				}
			}
			if (!cur.pts.empty()) {
				out.push_back(cur);
			}
			if (rest.pts.size() >= 2) {
				out.push_back(rest);
			}
		}
		ss = out;
	} else if (p_op == "delete_verts" || p_op == "dissolve") {
		for (Spline &s : ss) {
			s.pts.erase(std::remove_if(s.pts.begin(), s.pts.end(), [](const CPoint &p) { return p.sel; }), s.pts.end());
			if (p_op == "dissolve") {
				for (CPoint &p : s.pts) {
					if (p.hl == hewn::curves::H_VECTOR || p.hr == hewn::curves::H_VECTOR) {
						continue;
					}
					p.hl = p.hr = hewn::curves::H_AUTO; // (the shape kept smooth over the gap)
				}
			}
		}
		ss.erase(std::remove_if(ss.begin(), ss.end(), [](const Spline &s) { return s.pts.empty(); }), ss.end());
	} else if (p_op == "delete_segments") {
		// the segments between selected neighbors gone: the splines cut there
		std::vector<Spline> out;
		for (const Spline &s : ss) {
			const int n = (int)s.pts.size();
			std::vector<int> cuts;
			for (int i = 0; i < s.segments(); i++) {
				if (s.pts[i].sel && s.pts[(i + 1) % n].sel) {
					cuts.push_back(i);
				}
			}
			if (cuts.empty()) {
				out.push_back(s);
				continue;
			}
			int start = 0;
			std::vector<CPoint> pts = s.pts;
			if (s.cyclic) {
				// (a loop: opened at its first cut)
				start = (cuts[0] + 1) % n;
				std::rotate(pts.begin(), pts.begin() + start, pts.end());
				for (int &c2 : cuts) {
					c2 = (c2 - start + n) % n;
				}
				std::sort(cuts.begin(), cuts.end());
			}
			Spline piece = s;
			piece.cyclic = false;
			piece.pts.clear();
			for (int i = 0; i < n; i++) {
				piece.pts.push_back(pts[i]);
				if (std::find(cuts.begin(), cuts.end(), i) != cuts.end()) {
					if (piece.pts.size() >= 2) {
						out.push_back(piece);
					}
					piece.pts.clear();
				}
			}
			if (piece.pts.size() >= 2) {
				out.push_back(piece);
			}
		}
		ss = out;
	} else {
		done = false;
	}
	for (Spline &s : ss) {
		hewn::curves::recalc_handles(s);
	}
	c->changed();
	if (done) {
		static const struct {
			const char *op, *name;
		} names[] = { { "subdivide", "Subdivide Curve" }, { "make_segment", "Make Segment" }, { "clear_tilt", "Clear Tilt" }, { "smooth", "Smooth Curve" },
			{ "smooth_tilt", "Smooth Curve Tilt" }, { "smooth_radius", "Smooth Curve Radius" }, { "cyclic", "Toggle Cyclic" }, { "direction", "Switch Direction" },
			{ "recalc", "Recalculate Handles" }, { "split", "Split Curve" }, { "delete_verts", "Delete Curve Points" }, { "dissolve", "Dissolve Curve Points" },
			{ "delete_segments", "Delete Curve Segments" } };
		String name = "Curve: " + p_op.capitalize();
		for (const auto &nm : names) {
			if (p_op == nm.op) {
				name = nm.name;
			}
		}
		_hc_commit(name);
	} else {
		hc_before = Dictionary();
	}
	update_overlays();
}

// M: two selected open ends (of two splines, or one's both ends): joined in their middle; else each run of selected
// points one point at their middle.
void HewnEditorPlugin::_hc_merge() {
	HewnCurve3D *c = _hc();
	if (!c) {
		return;
	}
	std::vector<Spline> &ss = c->get_splines();
	std::vector<std::pair<int, int>> sel;
	for (int si = 0; si < (int)ss.size(); si++) {
		for (int i = 0; i < (int)ss[si].pts.size(); i++) {
			if (ss[si].pts[i].sel) {
				sel.push_back({ si, i });
			}
		}
	}
	auto is_end = [&](const std::pair<int, int> &e) { return !ss[e.first].cyclic && (e.second == 0 || e.second == (int)ss[e.first].pts.size() - 1); };
	if (sel.size() == 2 && is_end(sel[0]) && is_end(sel[1])) {
		if (sel[0].first == sel[1].first) {
			// (both ends of one: closed, they one point)
			Spline &s = ss[sel[0].first];
			if (s.pts.size() > 2) {
				s.pts.front().pos = (s.pts.front().pos + s.pts.back().pos) * 0.5f;
				s.pts.pop_back();
				s.cyclic = true;
				hewn::curves::recalc_handles(s);
			}
			return;
		}
		Spline &a = ss[sel[0].first];
		Spline b = ss[sel[1].first];
		if (sel[0].second == 0 && a.pts.size() > 1) {
			reverse_spline(a);
		}
		if (sel[1].second != 0 && b.pts.size() > 1) {
			reverse_spline(b);
		}
		const Vector3 mid = (a.pts.back().pos + b.pts.front().pos) * 0.5f;
		b.pts.front().pos = mid;
		b.pts.front().in = a.pts.back().in;
		b.pts.front().hl = a.pts.back().hl;
		a.pts.pop_back();
		a.pts.insert(a.pts.end(), b.pts.begin(), b.pts.end());
		hewn::curves::recalc_handles(a);
		ss.erase(ss.begin() + sel[1].first);
		return;
	}
	for (Spline &s : ss) {
		const int n = (int)s.pts.size();
		if (n < 2) {
			continue;
		}
		// (a run that wraps around a closed one: started after an unselected point)
		int start = 0;
		if (s.cyclic) {
			while (start < n && s.pts[start].sel) {
				start++;
			}
			if (start == n) {
				continue; // (all of it: left as it is)
			}
		}
		std::vector<CPoint> out;
		for (int k = 0; k < n;) {
			const int i = (start + k) % n;
			if (!s.pts[i].sel) {
				out.push_back(s.pts[i]);
				k++;
				continue;
			}
			CPoint m = s.pts[i];
			Vector3 sum;
			int cnt = 0;
			while (k < n && s.pts[(start + k) % n].sel) {
				sum += s.pts[(start + k) % n].pos;
				cnt++;
				k++;
			}
			m.pos = sum / (float)cnt;
			if (cnt > 1) {
				m.in = m.out = Vector3();
				m.hl = m.hr = hewn::curves::H_AUTO;
			}
			out.push_back(m);
		}
		s.pts = out;
		hewn::curves::recalc_handles(s);
	}
}

// Shift+D: the runs of selected points copied as splines of their own (a whole selected spline: as it is), the copies
// selected and moving with the mouse.
void HewnEditorPlugin::_hc_duplicate(Camera3D *p_cam) {
	HewnCurve3D *c = _hc();
	if (!c) {
		return;
	}
	std::vector<Spline> &ss = c->get_splines();
	std::vector<Spline> copies;
	for (const Spline &s : ss) {
		const int n = (int)s.pts.size();
		int nsel = 0;
		for (const CPoint &p : s.pts) {
			nsel += p.sel ? 1 : 0;
		}
		if (nsel == 0) {
			continue;
		}
		if (nsel == n) {
			copies.push_back(s);
			continue;
		}
		int start = 0;
		if (s.cyclic) {
			while (!(s.pts[start].sel && !s.pts[(start + n - 1) % n].sel)) {
				start++;
			}
		}
		Spline run = s;
		run.cyclic = false;
		run.pts.clear();
		for (int k = 0; k < n; k++) {
			const CPoint &p = s.pts[(start + k) % n];
			if (p.sel) {
				run.pts.push_back(p);
			} else if (!run.pts.empty()) {
				copies.push_back(run);
				run.pts.clear();
			}
		}
		if (!run.pts.empty()) {
			copies.push_back(run);
		}
	}
	if (copies.empty()) {
		return;
	}
	_hc_begin();
	for (Spline &s : ss) {
		for (CPoint &p : s.pts) {
			p.sel = p.sel_in = p.sel_out = false;
		}
	}
	for (Spline &s : copies) {
		for (CPoint &p : s.pts) {
			p.sel = true;
			p.sel_in = p.sel_out = false;
		}
		ss.push_back(s);
	}
	c->changed();
	const Dictionary keep = hc_before;
	_hc_start_xf(1);
	hc_before = keep;
	hc_duplicated = true;
}

// E: the selected points pulled out with the mouse, joined by new points to where they were (see below).
void HewnEditorPlugin::_hc_extrude(Camera3D *p_cam) {
	HewnCurve3D *c = _hc();
	if (!c) {
		return;
	}
	std::vector<Spline> &ss = c->get_splines();
	_hc_begin();
	std::vector<Spline> added;
	bool any = false;
	for (Spline &s : ss) {
		const int n = (int)s.pts.size();
		std::vector<int> sel;
		for (int i = 0; i < n; i++) {
			if (s.pts[i].sel) {
				sel.push_back(i);
			}
		}
		if (sel.empty()) {
			continue;
		}
		for (CPoint &p : s.pts) {
			p.sel = false;
		}
		any = true;
		// the whole of an open spline selected: a new point after each end (it grows both ways). Else each run of selected
		// points is pulled out: where it meets a point not selected, a copy of its end stays behind, joined to it (a run
		// at an end of the spline: only on its inner side, so one selected end point simply grows the spline)
		const int n_sel = (int)sel.size();
		if (!s.cyclic && n_sel == n) {
			CPoint pa = s.pts[n - 1], pb = s.pts[0];
			pa.sel = pb.sel = true;
			s.pts.push_back(pa);
			s.pts.insert(s.pts.begin(), pb);
			for (int k = 1; k + 1 < (int)s.pts.size(); k++) {
				s.pts[k].sel = false;
			}
			continue;
		}
		std::vector<bool> was(n);
		for (int k = 0; k < n; k++) {
			was[k] = false;
		}
		for (int k : sel) {
			was[k] = true;
		}
		std::vector<CPoint> pts;
		for (int k = 0; k < n; k++) {
			const bool has_prev = s.cyclic || k > 0, has_next = s.cyclic || k + 1 < n;
			CPoint p = s.pts[k];
			if (was[k] && has_prev && !was[(k + n - 1) % n]) {
				CPoint stay = p;
				stay.sel = false;
				pts.push_back(stay);
			}
			p.sel = was[k];
			pts.push_back(p);
			if (was[k] && has_next && !was[(k + 1) % n]) {
				CPoint stay = p;
				stay.sel = false;
				pts.push_back(stay);
			}
		}
		s.pts = pts;
	}
	if (!any) {
		hc_before = Dictionary();
		return;
	}
	ss.insert(ss.end(), added.begin(), added.end());
	c->changed();
	const Dictionary keep = hc_before;
	_hc_start_xf(1);
	hc_before = keep;
	hc_extruded = true;
}

// P: the selected points as a curve of their own (the same settings), out of this one.
void HewnEditorPlugin::_hc_separate() {
	HewnCurve3D *c = _hc();
	if (!c || !c->get_parent()) {
		return;
	}
	// each spline cut into its runs of selected and not selected points (a loop opened where they change): the
	// selected runs go, the others stay, each a spline of its own
	std::vector<Spline> taken, kept;
	for (const Spline &s : c->get_splines()) {
		const int n = (int)s.pts.size();
		int nsel = 0;
		for (const CPoint &p : s.pts) {
			nsel += p.sel ? 1 : 0;
		}
		if (nsel == 0) {
			kept.push_back(s);
			continue;
		}
		if (nsel == n) {
			taken.push_back(s);
			continue;
		}
		int start = 0;
		if (s.cyclic) {
			while (s.pts[start].sel == s.pts[(start + n - 1) % n].sel) {
				start++; // (from where a run begins)
			}
		}
		Spline run = s;
		run.cyclic = false;
		run.pts.clear();
		bool run_sel = s.pts[start].sel;
		auto flush = [&]() {
			if (run.pts.size() >= (run_sel ? 2u : 1u)) {
				(run_sel ? taken : kept).push_back(run);
			}
			run.pts.clear();
		};
		for (int k = 0; k < n; k++) {
			const CPoint &p = s.pts[(start + k) % n];
			if (p.sel != run_sel) {
				flush();
				run_sel = p.sel;
			}
			run.pts.push_back(p);
		}
		flush();
	}
	if (taken.empty()) {
		return;
	}
	HewnCurve3D *n = Object::cast_to<HewnCurve3D>(c->duplicate());
	if (!n) {
		return;
	}
	n->set_name(String(c->get_name()) + "_part");
	if (c->get_curve().is_valid()) {
		n->set_curve(c->get_curve()->duplicate()); // (its own: the shared one would change this curve too)
	}
	Array td, kd;
	for (const Spline &s : taken) {
		td.push_back(hewn::curves::spline_to_dict(s));
	}
	for (const Spline &s : kept) {
		kd.push_back(hewn::curves::spline_to_dict(s));
	}
	n->set_splines_data(td);
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	Dictionary after;
	after["splines"] = kd;
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Separate Curve", UndoRedo::MERGE_DISABLE, c);
	ur->add_do_method(c->get_parent(), "add_child", n, true);
	ur->add_do_method(n, "set_owner", root);
	ur->add_do_method(c, "set_state", after);
	ur->add_do_reference(n);
	ur->add_undo_method(c, "set_state", c->get_state());
	ur->add_undo_method(c->get_parent(), "remove_child", n);
	ur->commit_action();
	update_overlays();
}

// The curve ops with a value in the panel (bottom left): the radius / goal weight of the selected points, the curve
// made simpler (the share of its points kept).
bool HewnEditorPlugin::_hc_panel_op(const String &p_op) {
	HewnCurve3D *c = _hc();
	if (!c) {
		return false;
	}
	_hc_begin();
	std::vector<Spline> &ss = c->get_splines();
	if (p_op == "hc_radius" || p_op == "hc_weight") {
		const float v = (float)op_param(p_op, "value");
		for (Spline &s : ss) {
			for (CPoint &p : s.pts) {
				if (p.sel) {
					(p_op == "hc_radius" ? p.radius : p.weight) = MAX(0.0f, v);
				}
			}
		}
	} else if (p_op == "hc_simplify") {
		const float ratio = CLAMP((float)op_param(p_op, "ratio"), 0.0f, 1.0f);
		const bool any_sel = selected_count(ss) > 0;
		for (Spline &s : ss) {
			if (any_sel && !spline_selected(s)) {
				continue;
			}
			// the points that change the shape least taken out one by one, until the share is left
			const int keep = std::max(s.cyclic ? 3 : 2, (int)std::round((float)s.pts.size() * ratio));
			while ((int)s.pts.size() > keep) {
				const int n = (int)s.pts.size();
				int worst = -1;
				float wd = 1e30f;
				for (int i = 0; i < n; i++) {
					if (!s.cyclic && (i == 0 || i == n - 1)) {
						continue;
					}
					const Vector3 a = s.pts[(i - 1 + n) % n].pos, b = s.pts[(i + 1) % n].pos, p = s.pts[i].pos;
					const Vector3 ab = b - a;
					const float t = ab.length_squared() > 1e-12f ? CLAMP((p - a).dot(ab) / ab.length_squared(), 0.0f, 1.0f) : 0.0f;
					const float d = p.distance_to(a + ab * t);
					if (d < wd) {
						wd = d;
						worst = i;
					}
				}
				if (worst < 0) {
					break;
				}
				s.pts.erase(s.pts.begin() + worst);
			}
			hewn::curves::recalc_handles(s);
		}
	} else if (p_op == "hc_draw") {
		_hc_make_drawn();
	} else if (p_op == "hc_subdivide") {
		subdivide_splines(ss, (int)op_param(p_op, "cuts"));
		for (Spline &s : ss) {
			hewn::curves::recalc_handles(s);
		}
	} else if (p_op == "hc_bevel") {
		bevel_splines(ss, (float)op_param(p_op, "width"), (int)op_param(p_op, "segments"), (float)op_param(p_op, "profile"));
	}
	c->changed();
	static const char *names[6] = { "Set Curve Radius", "Set Goal Weight", "Simplify Curve", "Draw Curve", "Subdivide Curve", "Bevel Curve Points" };
	_hc_commit(names[p_op == "hc_radius" ? 0 : p_op == "hc_weight" ? 1 : p_op == "hc_simplify" ? 2 : p_op == "hc_subdivide" ? 4 : p_op == "hc_bevel" ? 5 : 3]);
	return true;
}

// The drawn stroke as a new Bezier spline (on the 3D cursor's plane facing the view, or on the surfaces under it),
// with fewer points: as many as keep it within the tolerance.
bool HewnEditorPlugin::_hc_draw_point(const Vector3 &p_from, const Vector3 &p_dir, Vector3 &r_w) {
	Vector3 nrm;
	if ((int)op_param("hc_draw", "depth") == 1) {
		// (on the surfaces, a bit off them)
		if (_raycast_scene(p_from, p_dir, r_w, nrm)) {
			r_w += nrm * (float)op_param("hc_draw", "offset");
			return true;
		}
		Vector3 plain;
		if (_raycast_cand_tris(p_from, p_dir, plain)) {
			r_w = plain;
			return true;
		}
	}
	const float den = hc_draw_plane_n.dot(p_dir);
	if (std::fabs(den) < 1e-7f) {
		return false;
	}
	const float t = hc_draw_plane_n.dot(hc_draw_plane_p - p_from) / den;
	if (t < 0.0f) {
		return false;
	}
	r_w = p_from + p_dir * t;
	return true;
}

void HewnEditorPlugin::_hc_draw_reproject() {
	if (hc_draw_rays.size() < 2) {
		return;
	}
	_gather_snap_candidates(false);
	hc_draw_world.clear();
	for (const auto &r : hc_draw_rays) {
		Vector3 w;
		if (_hc_draw_point(r.first, r.second, w)) {
			hc_draw_world.push_back(w);
		}
	}
}

void HewnEditorPlugin::_hc_make_drawn() {
	HewnCurve3D *c = _hc();
	_hc_draw_reproject(); // (as its panel says now: on the surfaces or the 3D cursor's plane)
	if (!c || hc_draw_world.size() < 2) {
		return;
	}
	const Transform3D ginv = c->get_global_transform().affine_inverse();
	const float tol = MAX(1e-4f, (float)op_param("hc_draw", "tolerance"));
	// (Douglas-Peucker: the points farther than the tolerance from the line of their neighbors kept)
	const std::vector<Vector3> &w = hc_draw_world;
	std::vector<char> keep(w.size(), 0);
	keep.front() = keep.back() = 1;
	std::vector<std::pair<int, int>> stack = { { 0, (int)w.size() - 1 } };
	while (!stack.empty()) {
		const auto r = stack.back();
		stack.pop_back();
		float best = 0.0f;
		int at = -1;
		const Vector3 a = w[r.first], b = w[r.second], ab = b - a;
		for (int i = r.first + 1; i < r.second; i++) {
			const float t = ab.length_squared() > 1e-12f ? CLAMP((w[i] - a).dot(ab) / ab.length_squared(), 0.0f, 1.0f) : 0.0f;
			const float d = w[i].distance_to(a + ab * t);
			if (d > best) {
				best = d;
				at = i;
			}
		}
		if (at >= 0 && best > tol) {
			keep[at] = 1;
			stack.push_back({ r.first, at });
			stack.push_back({ at, r.second });
		}
	}
	Spline s;
	s.type = hewn::curves::BEZIER;
	for (size_t i = 0; i < w.size(); i++) {
		if (keep[i]) {
			CPoint p;
			p.pos = ginv.xform(w[i]);
			p.sel = true;
			s.pts.push_back(p);
		}
	}
	hewn::curves::recalc_handles(s, true);
	for (Spline &o : c->get_splines()) {
		for (CPoint &p : o.pts) {
			p.sel = false;
		}
	}
	c->get_splines().push_back(s);
}

// The point (or a handle: r_handle -1 in, 1 out) of the curve nearest to the mouse on the screen.
bool HewnEditorPlugin::_hc_pick(Camera3D *p_cam, const Vector2 &p_pos, int &r_spline, int &r_point, int &r_handle) const {
	HewnCurve3D *c = _hc();
	r_spline = r_point = -1;
	r_handle = 0;
	if (!c) {
		return false;
	}
	const Transform3D g = c->get_global_transform();
	float best = pick_px;
	const std::vector<Spline> &ss = c->get_splines();
	for (int si = 0; si < (int)ss.size(); si++) {
		for (int pi = 0; pi < (int)ss[si].pts.size(); pi++) {
			const CPoint &p = ss[si].pts[pi];
			struct Cand {
				Vector3 w;
				int h;
			} cands[3] = { { g.xform(p.pos), 0 }, { g.xform(p.pos + p.in), -1 }, { g.xform(p.pos + p.out), 1 } };
			const int nc = ss[si].type == hewn::curves::BEZIER ? 3 : 1;
			for (int k = 0; k < nc; k++) {
				if (k > 0 && (k == 1 ? p.in : p.out).length_squared() < 1e-12f) {
					continue;
				}
				if (p_cam->is_position_behind(cands[k].w)) {
					continue;
				}
				const float d = p_cam->unproject_position(cands[k].w).distance_to(p_pos) + (k > 0 ? 1.5f : 0.0f); // (a point before its handle)
				if (d < best) {
					best = d;
					r_spline = si;
					r_point = pi;
					r_handle = cands[k].h;
				}
			}
		}
	}
	return r_spline >= 0;
}

// Into / out of the curve's edit mode. While in it the curve is out of Godot's selection (like a mesh in edit mode): no
// transform gizmo, and Godot's own Path3D editor neither shows its points nor takes the clicks (a right click there
// would delete a point); the inspector keeps showing it.
void HewnEditorPlugin::_hc_set_edit(bool p_on) {
	_path_detach(p_on);
	hc_edit = p_on;
	if (!p_on) {
		hc_xf = 0;
		hc_draw = false;
	}
	update_overlays();
}

// (a Path3D's points edited - a Hewn curve or a plain one: out of Godot's selection meanwhile)
void HewnEditorPlugin::_path_detach(bool p_on) {
	Path3D *c = Object::cast_to<Path3D>(ObjectDB::get_instance(curve_id));
	c = c && c->is_inside_tree() ? c : nullptr;
	EditorSelection *es = EditorInterface::get_singleton()->get_selection();
	if (p_on) {
		if (c && !hc_detached) {
			hc_detached = true;
			es->clear();
		}
	} else if (hc_detached) {
		hc_detached = false;
		if (c) {
			es->clear();
			es->add_node(c);
		}
	}
}

int32_t HewnEditorPlugin::_hc_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	HewnCurve3D *c = _hc();
	if (!c) {
		hc_edit = false;
		hc_detached = false;
		return -1;
	}
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	const Key code = k.is_valid() && k->is_pressed() && !k->is_echo() ? key_code(k) : KEY_NONE;
	const bool ctrl = k.is_valid() && (k->is_ctrl_pressed() || k->is_meta_pressed());
	const bool shift = k.is_valid() && k->is_shift_pressed(), alt = k.is_valid() && k->is_alt_pressed();
	if (!hc_edit) {
		if (code == KEY_TAB && !ctrl && !shift && !alt) {
			for (Spline &s : c->get_splines()) {
				for (CPoint &p : s.pts) {
					p.sel = false;
				}
			}
			_hc_set_edit(true);
			return AFTER_GUI_INPUT_STOP;
		}
		return -1;
	}
	std::vector<Spline> &ss = c->get_splines();
	const Transform3D g = c->get_global_transform();
	// ---- freehand drawing
	if (hc_draw) {
		auto point_at = [&](const Vector2 &p_pos, Vector3 &r_w) {
			Vector3 o, d;
			_ray(p_cam, p_pos, o, d);
			hc_draw_rays.push_back({ o, d });
			return _hc_draw_point(o, d, r_w);
		};
		if (code == KEY_ESCAPE || (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT)) {
			hc_draw = false;
			hc_draw_pts.clear();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
			if (mb->is_pressed()) {
				_gather_snap_candidates(false);
				hc_draw_pts = { mb->get_position() };
				hc_draw_world.clear();
				hc_draw_rays.clear();
				hc_draw_plane_p = cursor_pos;
				hc_draw_plane_n = cam_forward(p_cam);
				Vector3 w;
				if (point_at(mb->get_position(), w)) {
					hc_draw_world.push_back(w);
				}
			} else if (!hc_draw_pts.empty()) {
				hc_draw_pts.clear();
				hc_draw = false;
				if (hc_draw_world.size() >= 2) {
					start_scene_op("hc_draw");
				}
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mm.is_valid()) {
			mouse_pos = mm->get_position();
			if (!hc_draw_pts.empty() && mouse_pos.distance_to(hc_draw_pts.back()) > 3.0f) {
				hc_draw_pts.push_back(mouse_pos);
				Vector3 w;
				if (point_at(mouse_pos, w)) {
					hc_draw_world.push_back(w);
				}
			}
			update_overlays();
			return hc_draw_pts.empty() ? AFTER_GUI_INPUT_PASS : AFTER_GUI_INPUT_STOP;
		}
		return k.is_valid() ? AFTER_GUI_INPUT_STOP : -1;
	}
	// ---- a transform going on
	if (hc_xf != 0) {
		if (hc_xf == 6) {
			// a handle dragged (Aligned: the other side follows its direction; Auto / Vector become Aligned)
			if (mm.is_valid()) {
				mouse_pos = mm->get_position();
				if (hc_hs >= 0 && hc_hs < (int)ss.size() && hc_hp >= 0 && hc_hp < (int)ss[hc_hs].pts.size()) {
					CPoint &p = ss[hc_hs].pts[hc_hp];
					const Vector3 pw = g.xform(p.pos);
					Vector3 hit;
					if (_plane_hit(p_cam, mouse_pos, g.xform(p.pos + (hc_hside < 0 ? p.in : p.out)), cam_forward(p_cam), hit)) {
						Vector3 h = g.basis.inverse().xform(hit - pw);
						if (snapping_active(mm->is_ctrl_pressed() || mm->is_meta_pressed()) && dock && (dock->snap_targets & HewnDock::SNAP_STEPS)) {
							const float st = MAX(1e-4f, (float)dock->snap_move->get_value());
							h = Vector3(std::round(h.x / st) * st, std::round(h.y / st) * st, std::round(h.z / st) * st);
						}
						int &mine = hc_hside < 0 ? p.hl : p.hr;
						int &other = hc_hside < 0 ? p.hr : p.hl;
						if (mine == hewn::curves::H_AUTO || mine == hewn::curves::H_VECTOR) {
							mine = hewn::curves::H_ALIGNED;
							if (other == hewn::curves::H_AUTO || other == hewn::curves::H_VECTOR) {
								other = hewn::curves::H_ALIGNED;
							}
						}
						(hc_hside < 0 ? p.in : p.out) = h;
						if (mine == hewn::curves::H_ALIGNED && other == hewn::curves::H_ALIGNED && h.length_squared() > 1e-12f) {
							Vector3 &o = hc_hside < 0 ? p.out : p.in;
							o = -h.normalized() * o.length();
						}
						hewn::curves::recalc_handles(ss[hc_hs]);
						c->changed();
					}
				}
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && !mb->is_pressed()) {
				_hc_end_xf(false);
				return AFTER_GUI_INPUT_STOP;
			}
			if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT) {
				_hc_end_xf(true);
				return AFTER_GUI_INPUT_STOP;
			}
			return AFTER_GUI_INPUT_STOP;
		}
		if (mm.is_valid()) {
			mouse_pos = _precise_mouse(_wrap_mouse(p_cam, mm->get_position()), mm->is_shift_pressed());
			if (hc_bpick == 0 || hc_bpick == 2) {
				_hc_apply_xf(p_cam, mm->is_ctrl_pressed() || mm->is_meta_pressed());
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (hc_xf == 1 && code == KEY_G && !ctrl && !shift && !alt && hc_num.is_empty() && hc_bpick == 0) {
			// G again: the points slide along their pieces
			hc_xf = 8;
			hc_axis = -1;
			hc_slide_dir = Vector2();
			for (size_t si = 0; si < hc_start.size() && hc_slide_dir == Vector2(); si++) {
				const Spline &s = hc_start[si];
				const int n = (int)s.pts.size();
				for (int i = 0; i < n; i++) {
					const int j = i + 1 < n ? i + 1 : s.cyclic ? 0 : -1;
					if (!s.pts[i].sel || j < 0) {
						continue;
					}
					const Vector3 a = g.xform(s.pts[i].pos), b = g.xform(s.pts[j].pos);
					if (!p_cam->is_position_behind(a) && !p_cam->is_position_behind(b)) {
						hc_slide_dir = p_cam->unproject_position(b) - p_cam->unproject_position(a);
					}
					break;
				}
			}
			hc_mouse0 = mouse_pos;
			_hc_apply_xf(p_cam, false);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (hc_xf == 7 && mb.is_valid() && (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP || mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN)) {
			// the wheel: the bevel's segments (its release too: not the camera's zoom)
			if (!mb->is_pressed()) {
				return AFTER_GUI_INPUT_STOP;
			}
			hc_bevel_segs = CLAMP(hc_bevel_segs + (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? 1 : -1), 1, 64);
			_hc_apply_xf(p_cam, false);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (hc_xf == 2 && code == KEY_R && !ctrl && !shift && !alt && hc_num.is_empty()) {
			hc_trackball = !hc_trackball; // (R again: the trackball, again: back)
			hc_axis = -1;
			_hc_apply_xf(p_cam, false);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (((hc_xf >= 1 && hc_xf <= 5) || hc_xf == 7 || hc_xf == 8) && k.is_valid() && k->is_pressed() && !ctrl && !alt) {
			// a typed value: digits, a point / comma, minus (first), Backspace; an expression works too (2*3)
			const char32_t u = k->get_unicode();
			const bool digit = (u >= '0' && u <= '9') || u == '.' || u == ',' || (u == '-' && hc_num.is_empty()) || ((u == '*' || u == '/' || u == '+') && !hc_num.is_empty());
			if (digit || (code == KEY_BACKSPACE && !hc_num.is_empty()) || (code == KEY_MINUS && hc_num.is_empty())) {
				if (code == KEY_BACKSPACE) {
					hc_num = hc_num.substr(0, hc_num.length() - 1);
				} else {
					hc_num += String::chr(u == ',' ? '.' : u ? u : '-');
				}
				_hc_apply_xf(p_cam, false);
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
		}
		if (hc_xf <= 3 && code == KEY_B && !ctrl) {
			// B: the next click the source point (a point of the curve or a snapping target), the one after it the target
			hc_bpick = 1;
			c->get_splines() = hc_start;
			c->changed();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_TAB && shift) {
			if (dock) {
				dock->snap_enabled->set_pressed(!dock->snap_enabled->is_pressed());
			}
			_hc_apply_xf(p_cam, false);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if ((code == KEY_X || code == KEY_Y || code == KEY_Z) && hc_xf <= 3) {
			const int a = code == KEY_X ? 0 : code == KEY_Y ? 1 : 2;
			// (Shift: the plane of the other two; again: the curve's own axis, then free)
			if (hc_plane != shift) {
				hc_axis = -1;
				hc_local = false;
			}
			hc_plane = shift;
			if (hc_axis == a && !hc_local) {
				hc_local = true;
			} else if (hc_axis == a) {
				hc_axis = -1;
				hc_local = false;
			} else {
				hc_axis = a;
				hc_local = false;
			}
			_hc_apply_xf(p_cam, false);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		const bool click = mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_LEFT;
		if (click && hc_bpick == 1) {
			// the source: a curve point near the mouse, else what snapping finds there
			int s, p, h;
			Vector3 src;
			if (_hc_pick(p_cam, mb->get_position(), s, p, h) && h == 0) {
				src = g.xform(hc_start[s].pts[p].pos);
			} else if (!_snap_target(p_cam, mb->get_position(), src)) {
				_plane_hit(p_cam, mb->get_position(), hc_center, cam_forward(p_cam), src);
			}
			hc_bsource = src;
			hc_bpick = 3; // (now the target)
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (click && hc_bpick == 3) {
			Vector3 tgt;
			if (!_snap_target(p_cam, mb->get_position(), tgt)) {
				_plane_hit(p_cam, mb->get_position(), hc_center, cam_forward(p_cam), tgt);
			}
			hc_btarget = tgt;
			hc_bpick = 2;
			_hc_apply_xf(p_cam, false);
			_hc_end_xf(false);
			return AFTER_GUI_INPUT_STOP;
		}
		const bool confirm = code == KEY_ENTER || code == KEY_KP_ENTER || click;
		const bool cancel = code == KEY_ESCAPE || (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT);
		if (confirm || cancel) {
			if (cancel && hc_extruded) {
				// (a cancelled extrusion: as before it)
				c->set_state(hc_before);
				hc_before = Dictionary();
				hc_xf = 0;
				hc_extruded = false;
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			_hc_end_xf(cancel);
			return AFTER_GUI_INPUT_STOP;
		}
		if (k.is_valid() && (key_code(k) == KEY_CTRL || key_code(k) == KEY_META)) {
			_hc_apply_xf(p_cam, ctrl_after(k));
			update_overlays();
		}
		return k.is_valid() ? AFTER_GUI_INPUT_STOP : AFTER_GUI_INPUT_PASS;
	}
	// ---- keys
	if (code != KEY_NONE) {
		const bool plain = !ctrl && !shift && !alt;
		if ((code == KEY_TAB || code == KEY_ESCAPE) && plain) {
			_hc_set_edit(false); // (Esc too: out of it, nothing undone)
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_A && !ctrl && !shift) {
			hc_op(alt ? "select_none" : "select_all");
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_I && ctrl && !shift && !alt) {
			hc_op("select_invert");
			return AFTER_GUI_INPUT_STOP;
		}
		if ((code == KEY_G || code == KEY_R || code == KEY_S) && plain) {
			_hc_start_xf(code == KEY_G ? 1 : code == KEY_R ? 2 : 3);
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_S && alt && !ctrl && !shift) {
			_hc_start_xf(4);
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_T && ctrl && !alt && !shift) {
			_hc_start_xf(5);
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_B && ctrl && !alt && !shift) {
			hc_bevel_segs = MAX(1, (int)op_param("hc_bevel", "segments"));
			_hc_start_xf(7); // (bevel the selected points)
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_M && plain) {
			_show_menu(p_cam, "Merge", { { "At Center", "hc:merge" }, { "Make Segment  F", "hc:make_segment" } });
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_T && alt && !ctrl && !shift) {
			hc_op("clear_tilt");
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_E && plain) {
			_hc_extrude(p_cam);
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_F && plain) {
			hc_op("make_segment");
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_C && alt && !ctrl && !shift) {
			hc_op("cyclic");
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_N && shift && !ctrl && !alt) {
			hc_op("recalc");
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_P && plain) {
			hc_op("separate");
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_D && shift && !ctrl && !alt) {
			_hc_duplicate(p_cam);
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_X && ctrl && !alt && !shift) {
			hc_op("dissolve");
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_V && plain) {
			_show_menu(p_cam, "Set Handle Type", { { "Automatic", "hc:handle_auto" }, { "Vector", "hc:handle_vector" }, { "Aligned", "hc:handle_aligned" },
															{ "Free", "hc:handle_free" }, { "", "" }, { "Toggle Free / Aligned", "hc:handle_toggle" } });
			return AFTER_GUI_INPUT_STOP;
		}
		if ((code == KEY_X || code == KEY_DELETE) && plain) {
			_show_menu(p_cam, "Delete", { { "Vertices", "hc:delete_verts" }, { "Segments", "hc:delete_segments" }, { "Dissolve Vertices  Ctrl+X", "hc:dissolve" } });
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_ESCAPE) {
			hc_op("select_none");
			return AFTER_GUI_INPUT_STOP;
		}
		return -1;
	}
	// ---- the mouse: select, box, handles, Ctrl+click adds
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_RIGHT && mb->is_pressed()) {
		hc_rmb = true;
		hc_rmb_pos = mb->get_position();
		return AFTER_GUI_INPUT_PASS; // (a drag: Godot's free look)
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_RIGHT && !mb->is_pressed() && hc_rmb) {
		hc_rmb = false;
		if (hc_rmb_moved < 6.0f) {
			// (the release goes on to Godot too - it began its free look on the press and ends it here, the cursor
			// back; the menu opens after it)
			mouse_pos = hc_rmb_pos;
			hc_menu_cam = p_cam ? ObjectID(p_cam->get_instance_id()) : ObjectID();
			callable_mp(this, &HewnEditorPlugin::_hc_menu_deferred).call_deferred();
			hc_rmb_moved = 0.0f;
			return AFTER_GUI_INPUT_PASS;
		}
		hc_rmb_moved = 0.0f;
		return AFTER_GUI_INPUT_PASS;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		const Vector2 pos = mb->get_position();
		if (mb->is_pressed()) {
			if (mb->is_ctrl_pressed() || mb->is_meta_pressed()) {
				// on the curve (a point or its line): all the points of that spline selected (Shift: added)
				int on = -1;
				{
					int ps, pp, ph;
					if (_hc_pick(p_cam, pos, ps, pp, ph)) {
						on = ps;
					}
					float bestd = pick_px;
					for (int si = 0; on < 0 && si < (int)ss.size(); si++) {
						const std::vector<Vector3> l = c->sampled_points(si);
						for (size_t i = 0; i + 1 < l.size(); i++) {
							const Vector3 a = g.xform(l[i]), b = g.xform(l[i + 1]);
							if (p_cam->is_position_behind(a) || p_cam->is_position_behind(b)) {
								continue;
							}
							const Vector2 sa = p_cam->unproject_position(a), sb = p_cam->unproject_position(b), ab = sb - sa;
							const float kk = ab.length_squared() > 1e-9f ? CLAMP((pos - sa).dot(ab) / ab.length_squared(), 0.0f, 1.0f) : 0.0f;
							const float d = pos.distance_to(sa + ab * kk);
							if (d < bestd) {
								bestd = d;
								on = si;
							}
						}
					}
				}
				if (on >= 0) {
					for (int si = 0; si < (int)ss.size(); si++) {
						for (CPoint &pt : ss[si].pts) {
							pt.sel = si == on || (mb->is_shift_pressed() && pt.sel);
						}
					}
					c->changed();
					update_overlays();
					return AFTER_GUI_INPUT_STOP;
				}
				// off it: a new point at the mouse after the selected end (or after the last spline's end)
				_hc_begin();
				int si = -1, at = -1;
				for (int s = 0; s < (int)ss.size() && si < 0; s++) {
					const int n = (int)ss[s].pts.size();
					if (n > 0 && ss[s].pts[n - 1].sel && !ss[s].cyclic) {
						si = s;
						at = n;
					} else if (n > 1 && ss[s].pts[0].sel && !ss[s].cyclic) {
						si = s;
						at = 0;
					}
				}
				if (si < 0 && !ss.empty()) {
					si = (int)ss.size() - 1;
					at = (int)ss[si].pts.size();
				}
				Vector3 ref = si >= 0 && !ss[si].pts.empty() ? g.xform(ss[si].pts[at == 0 ? 0 : (int)ss[si].pts.size() - 1].pos) : g.origin;
				Vector3 hit, target;
				_gather_snap_candidates(false);
				if (snapping_active(mb->is_ctrl_pressed() != mb->is_ctrl_pressed()) && dock && (dock->snap_targets & HewnDock::SNAP_POINTS) && _snap_target(p_cam, pos, target)) {
					hit = target;
				} else if (!_plane_hit(p_cam, pos, ref, cam_forward(p_cam), hit)) {
					hc_before = Dictionary();
					return AFTER_GUI_INPUT_STOP;
				}
				if (si < 0) {
					Spline s;
					ss.push_back(s);
					si = 0;
					at = 0;
				}
				for (Spline &s : ss) {
					for (CPoint &p : s.pts) {
						p.sel = false;
					}
				}
				CPoint np;
				np.pos = g.affine_inverse().xform(hit);
				np.sel = true;
				ss[si].pts.insert(ss[si].pts.begin() + at, np);
				hewn::curves::recalc_handles(ss[si]);
				c->changed();
				_hc_commit("Add Curve Point");
				return AFTER_GUI_INPUT_STOP;
			}
			int s, p, h;
			if (_hc_pick(p_cam, pos, s, p, h) && h != 0) {
				hc_hs = s;
				hc_hp = p;
				hc_hside = h;
				hc_handle_press = true;
				hc_press = pos;
				return AFTER_GUI_INPUT_STOP;
			}
			hc_press = pos;
			hc_pressing = true;
			hc_box = false;
			return AFTER_GUI_INPUT_STOP;
		}
		if (hc_handle_press) {
			// a click on a handle: it alone selected (Shift: added / taken away), for G / R / S
			hc_handle_press = false;
			const bool add = mb->is_shift_pressed();
			if (hc_hs >= 0 && hc_hs < (int)ss.size() && hc_hp >= 0 && hc_hp < (int)ss[hc_hs].pts.size()) {
				if (!add) {
					for (Spline &sp : ss) {
						for (CPoint &pt : sp.pts) {
							pt.sel = pt.sel_in = pt.sel_out = false;
						}
					}
				}
				CPoint &pt = ss[hc_hs].pts[hc_hp];
				bool &hs = hc_hside < 0 ? pt.sel_in : pt.sel_out;
				hs = add ? !hs : true;
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (hc_pressing) {
			hc_pressing = false;
			const bool add = mb->is_shift_pressed();
			if (!add) {
				clear_handle_sel(ss);
			}
			if (hc_box) {
				const Rect2 r = Rect2(hc_press, Vector2()).expand(pos);
				for (Spline &sp : ss) {
					for (CPoint &pt : sp.pts) {
						const Vector3 w = g.xform(pt.pos);
						const bool in = !p_cam->is_position_behind(w) && r.has_point(p_cam->unproject_position(w));
						pt.sel = in || (add && pt.sel);
					}
				}
			} else {
				int s, p, h;
				const bool hit = _hc_pick(p_cam, pos, s, p, h);
				if (!add) {
					for (Spline &sp : ss) {
						for (CPoint &pt : sp.pts) {
							pt.sel = false;
						}
					}
				}
				if (hit) {
					ss[s].pts[p].sel = add ? !ss[s].pts[p].sel : true;
				}
			}
			hc_box = false;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
	}
	if (mm.is_valid()) {
		mouse_pos = mm->get_position();
		if (hc_handle_press && mouse_pos.distance_to(hc_press) > 4.0f) {
			// (a drag from a handle: it follows the mouse, as before)
			hc_handle_press = false;
			_hc_begin();
			hc_start = ss;
			hc_xf = 6;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (hc_rmb) {
			hc_rmb_moved += mm->get_relative().length();
		}
		if (hc_pressing && mouse_pos.distance_to(hc_press) > 4.0f) {
			hc_box = true;
		}
		if (hc_pressing) {
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
	}
	return -1;
}

// RMB in curve edit mode: everything that can be done to the points.
void HewnEditorPlugin::_hc_menu_deferred() {
	if (Camera3D *cam = Object::cast_to<Camera3D>(ObjectDB::get_instance(hc_menu_cam))) {
		_hc_menu(cam);
	}
}

void HewnEditorPlugin::_hc_menu(Camera3D *p_cam) {
	menu_subs.clear();
	menu_subs["hc_mirror"] = { { "X Global", "hc:mirror_x_g" }, { "Y Global", "hc:mirror_y_g" }, { "Z Global", "hc:mirror_z_g" }, { "", "" }, { "X Local", "hc:mirror_x_l" },
		{ "Y Local", "hc:mirror_y_l" }, { "Z Local", "hc:mirror_z_l" } };
	menu_subs["hc_snap"] = { { "Selection to Grid", "hc:snap_sel_grid" }, { "Selection to Cursor", "hc:snap_sel_cursor" },
		{ "Selection to Cursor (Keep Offset)", "hc:snap_sel_cursor_offset" }, { "", "" }, { "Cursor to Selected", "hc:snap_cursor_sel" },
		{ "Cursor to World Origin", "hc:snap_cursor_origin" }, { "Cursor to Grid", "hc:snap_cursor_grid" }, { "Cursor to Active", "hc:snap_cursor_active" } };
	menu_subs["hc_type"] = { { "Poly", "hc:type_poly" }, { "Bezier", "hc:type_bezier" }, { "NURBS", "hc:type_nurbs" } };
	menu_subs["hc_handle"] = { { "Automatic", "hc:handle_auto" }, { "Vector", "hc:handle_vector" }, { "Aligned", "hc:handle_aligned" }, { "Free", "hc:handle_free" },
		{ "", "" }, { "Toggle Free / Aligned", "hc:handle_toggle" } };
	menu_subs["hc_delete"] = { { "Vertices", "hc:delete_verts" }, { "Segments", "hc:delete_segments" }, { "Dissolve Vertices  Ctrl+X", "hc:dissolve" } };
	if (_selected_building()) {
		menu_subs["hc_side"] = { { "Roof: Gable", "bld:gable" }, { "Roof: Slope", "bld:slope" }, { "Roof: By Style", "bld:by_style" }, { "", "" },
			{ "No Windows", "bld:no_windows" }, { "Windows", "bld:windows" }, { "", "" }, { "Add Door", "bld:door" } };
		_show_menu(p_cam, "Curve",
				{ { "Building Side", "sub:hc_side" }, { "", "" }, { "Subdivide", "hc:subdivide" }, { "Bevel  Ctrl+B", "hc:bevel_xf" }, { "Extrude Curve and Move  E", "hc:extrude" },
						{ "Make Segment  F", "hc:make_segment" }, { "Set Handle Type  V", "sub:hc_handle" }, { "Delete  X", "sub:hc_delete" } });
		return;
	}
	_show_menu(p_cam, "Curve",
			{ { "Subdivide", "hc:subdivide" }, { "Bevel  Ctrl+B", "hc:bevel_xf" }, { "Extrude Curve and Move  E", "hc:extrude" }, { "Duplicate  Shift+D", "hc:duplicate" }, { "Make Segment  F", "hc:make_segment" }, { "Draw", "hc:draw" },
					{ "", "" }, { "Radius  Alt+S", "hc:radius_xf" }, { "Tilt  Ctrl+T", "hc:tilt_xf" }, { "Clear Tilt  Alt+T", "hc:clear_tilt" },
					{ "Set Curve Radius", "hc:set_radius" }, { "Set Goal Weight", "hc:goal_weight" }, { "", "" }, { "Smooth", "hc:smooth" },
					{ "Smooth Curve Tilt", "hc:smooth_tilt" }, { "Smooth Curve Radius", "hc:smooth_radius" }, { "Simplify Curve", "hc:simplify" }, { "", "" },
					{ "Mirror", "sub:hc_mirror" }, { "Snap", "sub:hc_snap" }, { "Set Spline Type", "sub:hc_type" }, { "Set Handle Type  V", "sub:hc_handle" },
					{ "Toggle Cyclic  Alt+C", "hc:cyclic" }, { "Switch Direction", "hc:direction" }, { "Recalculate Handles  Shift+N", "hc:recalc" }, { "", "" },
					{ "Split", "hc:split" }, { "Separate  P", "hc:separate" }, { "Delete  X", "sub:hc_delete" } });
}

// The curves of the scene as lines in object mode (Godot's own line is not drawn): the selected ones orange.
void HewnEditorPlugin::_draw_curve_lines(Control *p_overlay, Camera3D *p_cam) {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return;
	}
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	EditorSelection *sel = EditorInterface::get_singleton()->get_selection();
	HewnCurve3D *edited = hc_edit ? _hc() : nullptr;
	std::vector<Node *> stack = { root };
	while (!stack.empty()) {
		Node *n = stack.back();
		stack.pop_back();
		for (int i = 0; i < n->get_child_count(); i++) {
			stack.push_back(n->get_child(i));
		}
		HewnCurve3D *c = Object::cast_to<HewnCurve3D>(n);
		if (!c || c == edited || !c->is_visible_in_tree()) {
			continue;
		}
		const bool on = sel->get_selected_nodes().has(c);
		const Color col = on ? Color(1.0f, 0.6f, 0.15f, 0.95f) : Color(0.35f, 0.75f, 1.0f, 0.7f);
		const Transform3D g = c->get_global_transform();
		for (const Spline &s : c->get_splines()) {
			std::vector<hewn::curves::Sample> smp;
			hewn::curves::sample(s, MAX(4, c->get_resolution()), smp);
			if (s.cyclic && smp.size() > 2) {
				smp.push_back(smp.front());
			}
			for (size_t i = 1; i < smp.size(); i++) {
				const Vector3 a = g.xform(smp[i - 1].p), b = g.xform(smp[i].p);
				if (!p_cam->is_position_behind(a) && !p_cam->is_position_behind(b)) {
					p_overlay->draw_line(p_cam->unproject_position(a), p_cam->unproject_position(b), col, (on ? 2.0f : 1.5f) * es, true);
				}
			}
		}
	}
}

void HewnEditorPlugin::_draw_hc(Control *p_overlay, Camera3D *p_cam) {
	HewnCurve3D *c = _hc();
	if (!c || !hc_edit) {
		return;
	}
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const Transform3D g = c->get_global_transform();
	const Color line(0.1f, 0.1f, 0.1f, 0.9f), line_sel(1.0f, 0.75f, 0.3f), sel_col(1.0f, 0.6f, 0.1f), handle_col(0.9f, 0.85f, 0.4f);
	auto seg = [&](const Vector3 &a, const Vector3 &b, const Color &col, float w) {
		if (!p_cam->is_position_behind(a) && !p_cam->is_position_behind(b)) {
			p_overlay->draw_line(p_cam->unproject_position(a), p_cam->unproject_position(b), col, w, true);
		}
	};
	for (const Spline &s : c->get_splines()) {
		std::vector<hewn::curves::Sample> smp;
		hewn::curves::sample(s, MAX(4, c->get_resolution()), smp);
		const bool ssel = spline_selected(s);
		for (size_t i = 1; i < smp.size(); i++) {
			seg(g.xform(smp[i - 1].p), g.xform(smp[i].p), ssel ? line_sel : line, 2.0f * es);
		}
		if (s.cyclic && smp.size() > 2) {
			seg(g.xform(smp.back().p), g.xform(smp.front().p), ssel ? line_sel : line, 2.0f * es);
		}
		if (s.type == hewn::curves::NURBS || s.type == hewn::curves::POLY) {
			// (NURBS: the control polygon)
			for (int i = 0; s.type == hewn::curves::NURBS && i < s.segments(); i++) {
				seg(g.xform(s.pts[i].pos), g.xform(s.pts[(i + 1) % s.pts.size()].pos), Color(0.6f, 0.6f, 0.9f, 0.6f), 1.0f);
			}
		}
		for (const CPoint &p : s.pts) {
			const Vector3 w = g.xform(p.pos);
			if (p_cam->is_position_behind(w)) {
				continue;
			}
			const Vector2 sp = p_cam->unproject_position(w);
			if (s.type == hewn::curves::BEZIER) {
				for (int side = 0; side < 2; side++) {
					const Vector3 h = side == 0 ? p.in : p.out;
					if (h.length_squared() < 1e-12f) {
						continue;
					}
					const bool hsel = side == 0 ? p.sel_in : p.sel_out;
					const Vector3 hw = g.xform(p.pos + h);
					if (!p_cam->is_position_behind(hw)) {
						const Vector2 hp = p_cam->unproject_position(hw);
						p_overlay->draw_line(sp, hp, p.sel || hsel ? handle_col : Color(0.6f, 0.6f, 0.6f, 0.6f), 1.0f, true);
						if (hsel) {
							p_overlay->draw_circle(hp, 4.5f * es, Color(0, 0, 0));
						}
						p_overlay->draw_circle(hp, (hsel ? 3.5f : 3.0f) * es, hsel ? sel_col : p.sel ? handle_col : Color(0.6f, 0.6f, 0.6f));
					}
				}
			}
			p_overlay->draw_circle(sp, 5.0f * es, Color(0, 0, 0));
			p_overlay->draw_circle(sp, 4.0f * es, p.sel ? sel_col : Color(0.85f, 0.85f, 0.85f));
		}
	}
	if (hc_xf >= 1 && hc_xf <= 3 && hc_axis >= 0) {
		// the axis the points keep to (Shift: the two others), through where they began
		for (int a = 0; a < 3; a++) {
			if ((a == hc_axis) == hc_plane) {
				continue;
			}
			const Vector3 dir = hc_local ? g.basis.get_column(a).normalized() : AXES[a];
			const float far = p_cam->get_global_position().distance_to(hc_center) * 50.0f + 100.0f;
			Vector3 a0 = hc_center - dir * far, a1 = hc_center + dir * far;
			// (cut to what is in front of the camera)
			const Vector3 cp = p_cam->get_global_position(), cf = cam_forward(p_cam);
			const float near = p_cam->get_near() * 2.0f;
			const float d0 = (a0 - cp).dot(cf) - near, d1 = (a1 - cp).dot(cf) - near;
			if (d0 < 0.0f && d1 < 0.0f) {
				continue;
			}
			if (d0 < 0.0f) {
				a0 = a0.lerp(a1, d0 / (d0 - d1));
			} else if (d1 < 0.0f) {
				a1 = a0.lerp(a1, d0 / (d0 - d1));
			}
			Color col = AXIS_COLORS[a];
			col.a = 0.8f;
			seg(a0, a1, col, 1.5f * es);
		}
	}
	if (hc_pressing && hc_box) {
		const Rect2 r = Rect2(hc_press, Vector2()).expand(mouse_pos);
		p_overlay->draw_rect(r, Color(1, 0.6f, 0.1f, 0.12f));
		p_overlay->draw_rect(r, Color(1, 0.6f, 0.1f, 0.8f), false, 1.0f);
	}
	if (hc_draw && hc_draw_pts.size() >= 2) {
		PackedVector2Array pl;
		for (const Vector2 &q : hc_draw_pts) {
			pl.push_back(q);
		}
		p_overlay->draw_polyline(pl, sel_col, 2.0f * es, true);
	}
	if (hc_snapped && hc_xf != 0 && !p_cam->is_position_behind(hc_snap_point)) {
		draw_snap_symbol(p_overlay, p_cam->unproject_position(hc_snap_point), last_snap_kind, SNAP_COLOR, 7.0f);
	}
	String info = T("Curve: E: extrude, F: segment, Alt+S: radius, Ctrl+T: tilt, V: handles, RMB: menu, Tab / Esc: done");
	if (hc_draw) {
		info = T("Draw: drag a stroke (its settings in the panel after it), RMB / Esc: stop");
	} else if (hc_xf == 6) {
		info = T("Handle: drag, RMB: back");
	} else if (hc_bpick == 1) {
		info = T("Snap: click the SOURCE point");
	} else if (hc_bpick == 3) {
		info = T("Snap: click the TARGET point");
	} else if (hc_xf != 0) {
		static const char *axes[4] = { "", "  X", "  Y", "  Z" };
		info = T("LMB / Enter: done, RMB / Esc: back, X / Y / Z: an axis, B: snap base, Ctrl: snapping, Shift+Tab: snapping on / off") + String(axes[hc_axis + 1]) +
				(hc_local ? String(T(" (local)")) : String());
		if (hc_xf == 7) {
			info += vformat("   %.3f m   ", hc_factor) + vformat(T("Segments: %d (wheel)"), hc_bevel_segs);
		}
		if (hc_xf == 8) {
			info += vformat("   %.3f", hc_factor);
		}
		if (hc_xf == 3 || hc_xf == 4) {
			info += vformat("   %.3f", hc_factor);
		}
		if (hc_trackball && hc_xf == 2) {
			info += String("   ") + T("Trackball");
		}
		if (!hc_num.is_empty()) {
			info += String("   [") + hc_num + "]";
		}
	}
	draw_header(p_overlay, p_overlay->get_theme_default_font(), info);
}

// The Curve normals overlay: arrows along the curves (as long as the radius, turned by the tilt) of the selected
// curves and the one edited.
void HewnEditorPlugin::_draw_curve_normals(Control *p_overlay, Camera3D *p_cam) {
	std::vector<HewnCurve3D *> list;
	if (HewnCurve3D *c = _hc()) {
		list.push_back(c);
	}
	for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_selected_nodes()) {
		HewnCurve3D *c = Object::cast_to<HewnCurve3D>(v);
		if (c && std::find(list.begin(), list.end(), c) == list.end()) {
			list.push_back(c);
		}
	}
	const Color col(0.4f, 0.9f, 1.0f, 0.85f);
	for (HewnCurve3D *c : list) {
		const Transform3D g = c->get_global_transform();
		for (const Spline &s : c->get_splines()) {
			std::vector<hewn::curves::Sample> smp;
			hewn::curves::sample(s, MAX(1, c->get_resolution()), smp);
			std::vector<Vector3> T, N;
			hewn::curves::frames(smp, s.cyclic && smp.size() > 2, c->get_is_2d(), T, N);
			// at every point it is made of (Resolution): a small V along it - its way, its turn (the tilt), its size
			// (the radius)
			float span = 0.0f;
			for (size_t i = 0; i + 1 < smp.size(); i++) {
				span += smp[i].p.distance_to(smp[i + 1].p);
			}
			const float base = CLAMP(span / MAX(1.0f, (float)smp.size()) * 0.9f, 0.01f, 0.5f);
			for (size_t i = 0; i < smp.size() && i < T.size() && i < N.size(); i++) {
				const float L = base * MAX(0.05f, smp[i].radius) * (dock && dock->ov_curve_normals_size ? (float)dock->ov_curve_normals_size->get_value() : 1.0f);
				const Vector3 tip = g.xform(smp[i].p);
				const Vector3 a1 = g.xform(smp[i].p - T[i] * L * 0.7f + N[i] * L * 0.7f);
				const Vector3 a2 = g.xform(smp[i].p - T[i] * L * 0.7f - N[i] * L * 0.7f);
				if (p_cam->is_position_behind(tip) || p_cam->is_position_behind(a1) || p_cam->is_position_behind(a2)) {
					continue;
				}
				const Vector2 st = p_cam->unproject_position(tip);
				p_overlay->draw_line(st, p_cam->unproject_position(a1), col, 1.0f, true);
				p_overlay->draw_line(st, p_cam->unproject_position(a2), col, 1.0f, true);
			}
		}
	}
}

// ------------------------------------------------------------------ objects

// Add > Curve: a Bezier, a circle, a NURBS curve / circle at the 3D cursor.
// A curve of an Add > Curve kind (0 Bezier, 1 circle, 2 NURBS, 3 NURBS circle, 6 / 7 roads, 8 bridge, 10 poly).
HewnCurve3D *HewnEditorPlugin::make_curve_kind(int p_kind) {
	if (p_kind == 10) {
		HewnCurve3D *c = memnew(HewnCurve3D);
		c->set_name("PolyCurve");
		c->add_spline(hewn::curves::POLY, PackedVector3Array({ Vector3(-1, 0, 0), Vector3(0, 0, -0.5f), Vector3(1, 0, 0) }), false);
		return c;
	}
	static const char *names[9] = { "BezierCurve", "BezierCircle", "NurbsCurve", "NurbsCircle", "", "", "Road", "Road", "Bridge" };
	const int kind = CLAMP(p_kind, 0, 8);
	const String name = names[kind];
	const Ref<Material> picked = kind >= 6 && texture_dock ? texture_dock->current_material : Ref<Material>();
	return [kind, name, picked]() -> HewnCurve3D * {
		HewnCurve3D *c = memnew(HewnCurve3D);
		c->set_name(name);
		if (kind < 6) {
			c->make_preset(kind);
			return c;
		}
		// a road or a bridge: a curve with its road on, in the texture picked last
		c->make_preset(0, 5.0f);
		c->set_material(picked);
		c->set_collision(kind != 6);
		if (kind == 6) {
			c->set_road_mode(2);
		} else if (kind == 7) {
			c->set_road_mode(1);
			c->set_road_snap(true);
		} else {
			c->set_road_mode(1);
			c->set_road_width(4.0f);
			c->set_road_segments(1);
			c->set_road_thickness(0.4f);
			c->set_road_curb_height(0.6f);
			c->set_road_curb_width(0.25f);
			// the piers: a cap across under the deck, and a wall of a pier under it down to the ground (wider at
			// its foot), along the bridge
			auto block = [&](float p_along, float p_across_top, float p_across_bottom, float p_top, float p_bottom) {
				Ref<BoxMesh> bx;
				bx.instantiate();
				bx->set_size(Vector3(p_along, 1.0f, 1.0f));
				Array arr = bx->get_mesh_arrays();
				PackedVector3Array v = arr[Mesh::ARRAY_VERTEX];
				for (int64_t i = 0; i < v.size(); i++) {
					const Vector3 q = v[i];
					const bool up = q.y > 0.0f;
					v.set(i, Vector3(q.x, up ? p_top : p_bottom, q.z * (up ? p_across_top : p_across_bottom)));
				}
				arr[Mesh::ARRAY_VERTEX] = v;
				Ref<ArrayMesh> am;
				am.instantiate();
				am->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr);
				if (picked.is_valid()) {
					am->surface_set_material(0, picked);
				}
				Ref<HewnPathPlacement> pp;
				pp.instantiate();
				pp->set_mesh(am);
				pp->set_spacing(10.0f);
				pp->set_start_offset(1.5f);
				pp->set_end_offset(1.5f);
				return pp;
			};
			const float deck = 0.4f;
			Ref<HewnPathPlacement> cap = block(1.4f, 4.4f, 3.6f, -deck, -deck - 0.5f);
			Ref<HewnPathPlacement> pier = block(0.9f, 2.6f, 3.2f, 0.0f, -1.0f);
			pier->set_height(-deck - 0.5f);
			pier->set_reach_ground(true);
			TypedArray<Resource> list;
			list.push_back(cap);
			list.push_back(pier);
			c->set_placements(list);
		}
		return c;
	}();
}

void HewnEditorPlugin::add_curve(int p_kind) {
	// (drawn point by point like a wall; placed at the 3D cursor when the Add menu puts everything there; circles: placed)
	if (!create_at_cursor && p_kind != 1 && p_kind != 3) {
		start_shape_tool(hewn::shapes::WALL);
		shape_make = "curve:" + itos(p_kind);
		return;
	}
	adding_from_menu = true;
	start_placing(p_kind == 10 ? String("PolyCurve") : String("Curve"), [this, p_kind]() -> Node3D * { return make_curve_kind(p_kind); }, false);
	adding_from_menu = false;
}

// The selected edges (all of them in object mode) as the splines of a new curve: one for each chain of them (a loop:
// cyclic), of the given type (0 poly, 1 Bezier, 2 NURBS); the mesh stays.
void HewnEditorPlugin::mesh_to_curve(int p_type) {
	std::vector<HewnMesh3D *> meshes = mode == MODE_OBJECT ? _selected_meshes() : edited_meshes();
	bool any = false;
	for (HewnMesh3D *n : meshes) {
		const hewn::Mesh &m = n->get_data();
		hewn::Topology t;
		t.build(m);
		std::vector<hewn::Edge> edges;
		if (mode == MODE_OBJECT) {
			edges = t.edges;
			for (const std::pair<int, int> &w : m.wires) {
				edges.push_back(hewn::Edge(w.first, w.second));
			}
		} else if (mode == MODE_EDGE && n == get_edited()) {
			// the selected edges themselves (not every edge between two selected vertices: a ring of a quad strip,
			// a face's diagonal)
			for (const hewn::Edge &e : n->selection.edges) {
				if (e.a >= 0 && e.b >= 0 && e.a < (int)m.pos.size() && e.b < (int)m.pos.size()) {
					edges.push_back(e);
				}
			}
		} else if (mode == MODE_FACE && n == get_edited()) {
			// the selected faces' outline
			std::set<int> fs(n->selection.faces.begin(), n->selection.faces.end());
			for (int e = 0; e < (int)t.edges.size(); e++) {
				int in = 0;
				for (int f : t.edge_faces[e]) {
					in += fs.count(f) ? 1 : 0;
				}
				if (in == 1) {
					edges.push_back(t.edges[e]);
				}
			}
		} else {
			// (the edges both of whose ends are selected)
			std::vector<char> sel(m.pos.size(), 0);
			const std::vector<int> sv = n == get_edited() ? _selected_verts() : std::vector<int>();
			for (int v : sv) {
				if (v >= 0 && v < (int)sel.size()) {
					sel[v] = 1;
				}
			}
			for (const hewn::Edge &e : t.edges) {
				if (sel[e.a] && sel[e.b]) {
					edges.push_back(e);
				}
			}
			for (const std::pair<int, int> &w : m.wires) {
				if (sel[w.first] && sel[w.second]) {
					edges.push_back(hewn::Edge(w.first, w.second));
				}
			}
		}
		if (edges.empty()) {
			continue;
		}
		std::map<int, std::vector<int>> adj;
		std::set<std::pair<int, int>> left;
		for (const hewn::Edge &e : edges) {
			const std::pair<int, int> k = { std::min(e.a, e.b), std::max(e.a, e.b) };
			if (left.insert(k).second) {
				adj[e.a].push_back(e.b);
				adj[e.b].push_back(e.a);
			}
		}
		std::vector<Spline> splines;
		auto take = [&](int a, int b) { return left.erase({ std::min(a, b), std::max(a, b) }) > 0; };
		auto walk = [&](int start) {
			Spline sp;
			sp.type = p_type;
			std::vector<int> chain = { start };
			int cur = start;
			bool moved = true;
			while (moved) {
				moved = false;
				for (int nb : adj[cur]) {
					if (take(cur, nb)) {
						chain.push_back(nb);
						cur = nb;
						moved = true;
						break;
					}
				}
			}
			if (chain.size() < 2) {
				return;
			}
			if (chain.size() > 3 && chain.back() == chain.front()) {
				chain.pop_back();
				sp.cyclic = true;
			}
			for (int v : chain) {
				CPoint p;
				p.pos = m.pos[v];
				p.hl = p.hr = p_type == hewn::curves::BEZIER ? hewn::curves::H_AUTO : hewn::curves::H_VECTOR;
				sp.pts.push_back(p);
			}
			hewn::curves::recalc_handles(sp);
			splines.push_back(sp);
		};
		// (from the ends first: the open chains whole, then the loops left)
		for (const auto &kv : adj) {
			if (kv.second.size() != 2) {
				walk(kv.first);
			}
		}
		while (!left.empty()) {
			walk(left.begin()->first);
		}
		if (splines.empty()) {
			continue;
		}
		HewnCurve3D *c = memnew(HewnCurve3D);
		c->set_name(String(n->get_name()) + "_curve");
		Array data;
		for (const Spline &s : splines) {
			data.push_back(hewn::curves::spline_to_dict(s));
		}
		c->set_splines_data(data);
		_add_node_with_undo(c, "Curve from Edges");
		c->set_global_transform(n->get_global_transform());
		any = true;
	}
	if (!any) {
		if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
			t->push_toast(T("Curve from Edges: no edges selected"));
		}
	}
}

void HewnEditorPlugin::_curve_object_menu(Camera3D *p_cam) {
	menu_subs.clear();
	menu_subs["co_origin"] = { { "Origin to Geometry", "cobj:origin_geometry" }, { "Origin to 3D Cursor", "cobj:origin_cursor" }, { "Origin to First Point", "cobj:origin_first" } };
	menu_subs["co_mirror"] = { { "X", "cobj:mirror_x" }, { "Y", "cobj:mirror_y" }, { "Z", "cobj:mirror_z" } };
	menu_subs["co_snap"] = { { "Selection to Cursor", "cobj:snap_sel_cursor" }, { "Selection to Grid", "cobj:snap_sel_grid" }, { "", "" },
		{ "Cursor to Selected", "cobj:snap_cursor_sel" }, { "Cursor to World Origin", "cobj:snap_cursor_origin" }, { "Cursor to Grid", "cobj:snap_cursor_grid" } };
	menu_subs["co_along"] = { { "Wall", "plan:along_wall" }, { "Sweep", "plan:along_sweep" }, { "Path Stairs", "plan:along_stairs" } };
	menu_subs["co_plan"] = { { "Floor", "plan:floor" }, { "Walls", "plan:walls" }, { "Roof", "plan:roof" }, { "Cornice", "plan:cornice" }, { "Windows", "plan:windows" },
		{ "", "" }, { "Next Floor", "plan:next" } };
	if (_selected_building()) {
		// (a building: its floors and its curves; the rest in the Inspector)
		menu_subs["co_building"] = { { "Add Floor", "bld:add_floor" }, { "Remove Top Floor", "bld:remove_floor" }, { "", "" },
			{ "Own Outline for the Top Floor", "bld:own_outline" }, { "Add Courtyard", "bld:courtyard" } };
		_show_menu(p_cam, "Curve Context Menu",
				{ { "Edit Mode  Tab", "cobj:edit" }, { "", "" }, { "Building", "sub:co_building" }, { "", "" }, { "Convert to Mesh", "cobj:to_mesh" }, { "Convert to Path3D", "cobj:to_path3d" },
						{ "Set Origin", "sub:co_origin" }, { "Duplicate", "cobj:duplicate" }, { "Mirror", "sub:co_mirror" }, { "Snap", "sub:co_snap" } });
		return;
	}
	_show_menu(p_cam, "Curve Context Menu",
			{ { "Edit Mode  Tab", "cobj:edit" }, { "", "" }, { "Convert to Mesh", "cobj:to_mesh" }, { "Convert to Path3D", "cobj:to_path3d" }, { "Set Origin", "sub:co_origin" }, { "Duplicate", "cobj:duplicate" },
					{ "Mirror", "sub:co_mirror" }, { "Snap", "sub:co_snap" }, { "", "" }, { "Along the Curve", "sub:co_along" }, { "Build on the Plan", "sub:co_plan" } });
}

void HewnEditorPlugin::curve_object_op(const String &p_op) {
	std::vector<HewnCurve3D *> curves;
	for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
		if (HewnCurve3D *c = Object::cast_to<HewnCurve3D>(v)) {
			curves.push_back(c);
		}
	}
	if (curves.empty()) {
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (p_op == "edit") {
		curve_id = ObjectID(curves[0]->get_instance_id());
		_hc_set_edit(true);
		return;
	}
	if (p_op == "to_path3d") {
		// each curve as Godot's Path3D in its place (its points; its geometry and the rest of it gone), the nodes put
		// under it going along
		ur->create_action("Hewn: Curve to Path3D", UndoRedo::MERGE_DISABLE, root);
		std::vector<Node *> made;
		for (HewnCurve3D *c : curves) {
			Node *parent = c->get_parent();
			if (!parent || c == root) {
				continue;
			}
			Path3D *p = memnew(Path3D);
			p->set_name(c->get_name());
			p->set_transform(c->get_transform());
			p->set_visible(c->is_visible());
			p->set_curve(c->make_curve3d());
			std::vector<Node *> kids;
			for (int i = 0; i < c->get_child_count(); i++) {
				if (c->get_child(i)->get_owner() == root) {
					kids.push_back(c->get_child(i));
				}
			}
			// (the curve out first: the Path3D keeps its name)
			ur->add_do_method(parent, "remove_child", c);
			ur->add_do_method(parent, "add_child", p, true);
			ur->add_do_method(parent, "move_child", p, c->get_index());
			ur->add_do_method(p, "set_owner", root);
			for (Node *k : kids) {
				ur->add_do_method(c, "remove_child", k);
				ur->add_do_method(p, "add_child", k, true);
				ur->add_do_method(k, "set_owner", root);
			}
			ur->add_do_reference(p);
			for (Node *k : kids) {
				ur->add_undo_method(p, "remove_child", k);
				ur->add_undo_method(c, "add_child", k, true);
			}
			ur->add_undo_method(parent, "remove_child", p);
			ur->add_undo_method(parent, "add_child", c, true);
			ur->add_undo_method(parent, "move_child", c, c->get_index());
			ur->add_undo_method(c, "set_owner", root);
			for (Node *k : kids) {
				ur->add_undo_method(k, "set_owner", root);
			}
			ur->add_undo_reference(c);
			made.push_back(p);
		}
		ur->commit_action();
		EditorSelection *es = EditorInterface::get_singleton()->get_selection();
		es->clear();
		for (Node *x : made) {
			es->add_node(x);
		}
		return;
	}
	if (p_op == "to_mesh") {
		// each curve's geometry as a Hewn mesh in its place (the curve gone)
		ur->create_action("Hewn: Curve to Mesh", UndoRedo::MERGE_DISABLE, root);
		bool any = false;
		for (HewnCurve3D *c : curves) {
			Node *parent = c->get_parent();
			hewn::Mesh gm = c->geometry_mesh();
			if (gm.faces.empty()) {
				// no faces (no depth, no fill): its lines as loose edges
				gm.clear();
				const std::vector<Spline> &ss = c->get_splines_const();
				for (int si = 0; si < (int)ss.size(); si++) {
					const std::vector<Vector3> pts = c->sampled_points(si);
					const int base = (int)gm.pos.size();
					for (size_t k = 0; k < pts.size(); k++) {
						if (k > 0 && pts[k].distance_to(pts[k - 1]) < 1e-6f) {
							continue;
						}
						gm.add_vertex(pts[k]);
					}
					const int cnt = (int)gm.pos.size() - base;
					bool closed = ss[si].cyclic;
					if (cnt > 2 && gm.pos[base].distance_to(gm.pos.back()) < 1e-5f) {
						gm.pos.pop_back(); // (a loop drawn back to its start)
						closed = true;
					}
					const int m2 = (int)gm.pos.size() - base;
					for (int k = 0; k + 1 < m2; k++) {
						gm.wires.push_back({ base + k, base + k + 1 });
					}
					if (closed && m2 > 2) {
						gm.wires.push_back({ base + m2 - 1, base });
					}
					if (m2 == 1) {
						gm.points.push_back(base);
					}
				}
			}
			if (!parent || (gm.faces.empty() && gm.wires.empty())) {
				continue;
			}
			HewnMesh3D *m = memnew(HewnMesh3D);
			m->set_name(c->get_name());
			m->set_shape(hewn::shapes::CUSTOM);
			m->get_data() = gm;
			TypedArray<Material> mats;
			mats.push_back(c->get_material());
			m->set_materials(mats);
			m->commit(false);
			m->set_transform(c->get_transform());
			ur->add_do_method(parent, "remove_child", c);
			ur->add_do_method(parent, "add_child", m, true);
			ur->add_do_method(m, "set_owner", root);
			ur->add_do_method(parent, "move_child", m, c->get_index());
			ur->add_do_reference(m);
			ur->add_undo_method(parent, "remove_child", m);
			ur->add_undo_method(parent, "add_child", c, true);
			ur->add_undo_method(parent, "move_child", c, c->get_index());
			ur->add_undo_method(c, "set_owner", root);
			ur->add_undo_reference(c);
			any = true;
		}
		ur->commit_action(any);
		if (!any) {
			if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
				t->push_toast(T("Convert to Mesh: the curve has no geometry (give it a Depth, an Extrude or a 2D fill)"));
			}
		}
		return;
	}
	if (p_op == "duplicate") {
		ur->create_action("Hewn: Duplicate Curve", UndoRedo::MERGE_DISABLE, root);
		for (HewnCurve3D *c : curves) {
			Node *d = c->duplicate();
			if (!d || !c->get_parent()) {
				continue;
			}
			ur->add_do_method(c->get_parent(), "add_child", d, true);
			ur->add_do_method(d, "set_owner", root);
			ur->add_do_reference(d);
			ur->add_undo_method(c->get_parent(), "remove_child", d);
		}
		ur->commit_action();
		return;
	}
	const float st = dock ? MAX(1e-4f, (float)dock->snap_move->get_value()) : 1.0f;
	auto grid = [&](const Vector3 &w) { return Vector3(std::round(w.x / st) * st, std::round(w.y / st) * st, std::round(w.z / st) * st); };
	if (p_op == "snap_cursor_sel" || p_op == "snap_cursor_origin" || p_op == "snap_cursor_grid") {
		Vector3 c0;
		for (HewnCurve3D *c : curves) {
			c0 += c->get_global_transform().origin;
		}
		c0 /= (float)curves.size();
		cursor_pos = p_op == "snap_cursor_sel" ? c0 : p_op == "snap_cursor_origin" ? Vector3() : grid(cursor_pos);
		update_overlays();
		return;
	}
	ur->create_action("Hewn: Curve " + p_op.capitalize(), UndoRedo::MERGE_DISABLE, root);
	for (HewnCurve3D *c : curves) {
		const Dictionary before = c->get_state();
		const Transform3D xf = c->get_global_transform();
		Transform3D nxf = xf;
		std::vector<Spline> ss = c->get_splines();
		if (p_op.begins_with("origin_")) {
			// the points stay where they are, the origin moves
			Vector3 to = xf.origin;
			if (p_op == "origin_cursor") {
				to = cursor_pos;
			} else if (p_op == "origin_first") {
				to = !ss.empty() && !ss[0].pts.empty() ? xf.xform(ss[0].pts[0].pos) : xf.origin;
			} else {
				AABB b;
				bool first = true;
				for (const Spline &s : ss) {
					std::vector<hewn::curves::Sample> smp;
					hewn::curves::sample(s, 8, smp);
					for (const hewn::curves::Sample &q : smp) {
						const Vector3 w = xf.xform(q.p);
						b = first ? AABB(w, Vector3()) : b.expand(w);
						first = false;
					}
				}
				to = b.get_center();
			}
			const Vector3 ld = xf.basis.inverse().xform(to - xf.origin);
			for (Spline &s : ss) {
				for (CPoint &p : s.pts) {
					p.pos -= ld;
				}
			}
			nxf.origin = to;
		} else if (p_op.begins_with("mirror_")) {
			const int a = p_op == "mirror_x" ? 0 : p_op == "mirror_y" ? 1 : 2;
			for (Spline &s : ss) {
				for (CPoint &p : s.pts) {
					p.pos[a] = -p.pos[a];
					p.in[a] = -p.in[a];
					p.out[a] = -p.out[a];
				}
			}
		} else if (p_op == "snap_sel_cursor") {
			nxf.origin = cursor_pos;
		} else if (p_op == "snap_sel_grid") {
			nxf.origin = grid(xf.origin);
		}
		Array data;
		for (const Spline &s : ss) {
			data.push_back(hewn::curves::spline_to_dict(s));
		}
		Dictionary after;
		after["splines"] = data;
		ur->add_do_method(c, "set_state", after);
		ur->add_do_method(c, "set_global_transform", nxf);
		ur->add_undo_method(c, "set_state", before);
		ur->add_undo_method(c, "set_global_transform", xf);
	}
	ur->commit_action();
}
