// Shape curves edited in the view: a column's parts, a sweep's cross section, a door's opening, an arch's inner line, a
// wedge's slope, a roof's slopes (the shape panel's In View), and the curve of the bevel / the arch just made (while
// their Last Operation panel is up) - their points drawn where they shape the mesh. A click picks a point (its handles
// show, they drag), a drag moves it; a column's sizes (its radius, height, base and top) have handles of their own.
#include "hw_editor.h"
#include "../hw_undo_state.h"
#include "hw_editor_util.h"
#include "../hw_profile.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>

using namespace godot;
using namespace hw_editor;

namespace godot {

// One curve and where it lies: a point q at o + ax * q.x + ay * q.y (world space). Click the curve between two points:
// a point there; Delete / X: the picked one out (not the ends).
struct HwProfPlace {
	Ref<HewnProfile> res;
	Vector3 o, ax, ay;
	bool column = false; // (x not below 0, y 0..1)
	Vector3 to3(const Vector2 &q) const { return o + ax * q.x + ay * q.y; }
	Vector2 to2(const Vector3 &w) const {
		// (least squares in its plane)
		const Vector3 d = w - o;
		const float aa = ax.dot(ax), ab = ax.dot(ay), bb = ay.dot(ay);
		const float da = d.dot(ax), db = d.dot(ay);
		const float det = aa * bb - ab * ab;
		if (std::fabs(det) < 1e-12f) {
			return Vector2();
		}
		return Vector2((da * bb - db * ab) / det, (db * aa - da * ab) / det);
	}
	Vector3 normal() const { return ax.cross(ay).normalized(); }
};

// A column's size handle: the parameter it sets and where it sits (world), moving along its axis.
struct HwSizeHandle {
	String param;
	Vector3 at, axis; // (axis: world, a meter of the parameter)
	float value = 0.0f;
};

} // namespace godot

namespace {

void column_places(HewnMesh3D *n, const Transform3D &g, std::vector<HwProfPlace> &r_places, std::vector<HwSizeHandle> &r_sizes) {
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
	const Vector3 c = n->get_column_center();
	const char *props[3] = { "base_profile", "shaft_profile", "top_profile" };
	const float y0s[3] = { 0.0f, bh, H - th }, hs[3] = { bh, shaft_h / reps, th };
	for (int i = 0; i < 3; i++) {
		const Ref<HewnProfile> pr = n->get(props[i]);
		if (pr.is_null() || hs[i] < 1e-5f) {
			continue;
		}
		HwProfPlace pp;
		pp.res = pr;
		pp.column = true;
		pp.o = g.xform(Vector3(0, y0s[i], 0) - c);
		pp.ax = g.basis.xform(Vector3(R, 0, 0));
		pp.ay = g.basis.xform(Vector3(0, hs[i], 0));
		r_places.push_back(pp);
	}
	// the sizes: the radius out to the side, the height on top, where the base ends, where the top starts
	const Vector3 up = g.basis.xform(Vector3(0, 1, 0)), side = g.basis.xform(Vector3(1, 0, 0));
	const float out = R * 1.6f + 0.05f;
	r_sizes.push_back({ "radius", g.xform(Vector3(R, bh + shaft_h * 0.5f, 0) - c), side, R });
	r_sizes.push_back({ "height", g.xform(Vector3(0, H, 0) - c), up, H });
	if (bh > 1e-5f) {
		r_sizes.push_back({ "base_height", g.xform(Vector3(out, bh, 0) - c), up, bh });
	}
	if (th > 1e-5f) {
		r_sizes.push_back({ "top_height", g.xform(Vector3(out, H - th, 0) - c), -up, th });
	}
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
	if (!n) {
		return nullptr;
	}
	const int t = n->get_shape();
	return t == hewn::shapes::COLUMN || t == hewn::shapes::SWEEP || t == hewn::shapes::DOOR || t == hewn::shapes::ARCH || t == hewn::shapes::WEDGE ||
					t == hewn::shapes::ROOF
			? n
			: nullptr;
}

void HewnEditorPlugin::_op_profile_made(const String &p_op, const hewn::ops::ProfileFrame &p_frame, const Transform3D &p_xf) {
	op_prof_op = p_frame.made ? p_op : String();
	op_prof_frame = p_frame;
	op_prof_frame.o = p_xf.xform(p_frame.o);
	op_prof_frame.ax = p_xf.basis.xform(p_frame.ax);
	op_prof_frame.ay = p_xf.basis.xform(p_frame.ay);
}

// Every curve shown now, and the column's sizes.
void HewnEditorPlugin::_prof3d_places(std::vector<HwProfPlace> &r_places, std::vector<HwSizeHandle> &r_sizes, HewnMesh3D **r_node) {
	HewnEditorPlugin *p = this;
	r_places.clear();
	r_sizes.clear();
	*r_node = nullptr;
	if (HewnMesh3D *n = p->_prof3d_target()) {
		*r_node = n;
		const Transform3D g = n->get_global_transform();
		const int t = n->get_shape();
		if (t == hewn::shapes::COLUMN) {
			column_places(n, g, r_places, r_sizes);
		} else if (t == hewn::shapes::SWEEP) {
			// (where the sweep itself put its curve: the middle of its longest segment)
			const Ref<HewnProfile> pr = n->get_sweep_profile();
			const hewn::ops::ProfileFrame &f = n->get_profile_frame();
			if ((int)n->get_shape_param("profile") == 6 && pr.is_valid() && n->get_poly_points().size() >= 2 && f.made) {
				HwProfPlace pp;
				pp.res = pr;
				pp.o = g.xform(f.o);
				pp.ax = g.basis.xform(f.ax);
				pp.ay = g.basis.xform(f.ay);
				r_places.push_back(pp);
			}
		} else if (n->get_shape_profile().is_valid() && n->get_profile_frame().made) {
			const hewn::ops::ProfileFrame &f = n->get_profile_frame();
			HwProfPlace pp;
			pp.res = n->get_shape_profile();
			pp.o = g.xform(f.o);
			pp.ax = g.basis.xform(f.ax);
			pp.ay = g.basis.xform(f.ay);
			r_places.push_back(pp);
		}
		return;
	}
	if (p->_op_prof_live()) {
		HwProfPlace pp;
		pp.res = p->op_prof_op == "bevel" ? p->_bevel_profile() : p->_arch_profile();
		pp.o = p->op_prof_frame.o;
		pp.ax = p->op_prof_frame.ax;
		pp.ay = p->op_prof_frame.ay;
		r_places.push_back(pp);
	}
}

// A sweep's Custom curve: its sizes and offsets as the edit begins (the curve's box with them).
void HewnEditorPlugin::_prof3d_fit_begin(HewnMesh3D *p_node) {
	prof3d_fit = false;
	if (!p_node || p_node->get_shape() != hewn::shapes::SWEEP || prof3d_res.is_null() || prof3d_res != p_node->get_sweep_profile() ||
			(int)p_node->get_shape_param("profile") != 6) {
		return;
	}
	const std::vector<Vector2> smp = prof3d_res->to_vector();
	if (smp.size() < 3) {
		return;
	}
	Vector2 mn = smp[0], mx = smp[0];
	for (const Vector2 &q : smp) {
		mn = mn.min(q);
		mx = mx.max(q);
	}
	prof3d_fit_mn = mn;
	prof3d_fit_sz = (mx - mn).max(Vector2(1e-6f, 1e-6f));
	const char *names[5] = { "width", "height", "drop", "offset_x", "offset_y" };
	for (int i = 0; i < 5; i++) {
		prof3d_fit_v[i] = (float)p_node->get_shape_param(names[i]);
	}
	prof3d_params_before = p_node->get_shape_params().duplicate();
	prof3d_fit = true;
}

// The curve changed: the width / height keep a meter of it a meter, the offsets keep it where it was (its box grew
// or shrank: the sweep fits the box to its sizes). The mesh made again once, with the curve as it is now.
void HewnEditorPlugin::_prof3d_fit_apply(HewnMesh3D *p_node) {
	if (!prof3d_fit || !p_node || prof3d_res.is_null()) {
		return;
	}
	const std::vector<Vector2> smp = prof3d_res->to_vector();
	if (smp.size() < 3) {
		return;
	}
	Vector2 mn = smp[0], mx = smp[0];
	for (const Vector2 &q : smp) {
		mn = mn.min(q);
		mx = mx.max(q);
	}
	const Vector2 sz = (mx - mn).max(Vector2(1e-6f, 1e-6f));
	const bool centered = (int)p_node->get_shape_param("anchor") == 1, outside = (int)p_node->get_shape_param("align") == 1, inside = (int)p_node->get_shape_param("align") == 2;
	const float sgn = (float)p_node->get_shape_param("downward") > 0.5f ? -1.0f : 1.0f;
	const float w0 = prof3d_fit_v[0], drop0 = centered ? 0.0f : prof3d_fit_v[2], H0 = prof3d_fit_v[1] + drop0;
	// (where the curve's box sits: across, up - as the sweep places it)
	auto cx = [&](const Vector2 &p_mn, const Vector2 &p_sz, float p_w) { return -(p_mn.x / p_sz.x + 0.5f) * p_w + (outside ? p_w * 0.5f : inside ? -p_w * 0.5f : 0.0f); };
	auto cy = [&](const Vector2 &p_mn, const Vector2 &p_sz, float p_H, float p_drop) { return -p_mn.y / p_sz.y * p_H - (centered ? p_H * 0.5f : 0.0f) - p_drop; };
	const float w = MAX(0.001f, w0 / prof3d_fit_sz.x * sz.x);
	const float H = H0 / prof3d_fit_sz.y * sz.y;
	float drop = drop0, h = H - drop;
	if (h < 0.001f) {
		h = 0.001f;
		drop = centered ? 0.0f : MAX(0.0f, H - h);
	}
	const float ox = prof3d_fit_v[3] + cx(prof3d_fit_mn, prof3d_fit_sz, w0) - cx(mn, sz, w);
	const float oy = prof3d_fit_v[4] + sgn * (cy(prof3d_fit_mn, prof3d_fit_sz, H0, drop0) - cy(mn, sz, h + drop, drop));
	Dictionary d = p_node->get_shape_params().duplicate();
	d["width"] = w;
	d["height"] = h;
	if (!centered) {
		d["drop"] = drop;
	}
	d["offset_x"] = ox;
	d["offset_y"] = oy;
	p_node->set_shape_params(d);
}

// The edit as one undo step: the curve (and a sweep's sizes) before and now.
void HewnEditorPlugin::_prof3d_record(HewnMesh3D *p_node, const String &p_action) {
	if (!p_node || prof3d_res.is_null()) {
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(p_action, UndoRedo::MERGE_DISABLE, p_node);
	hw_undo_state(ur, true, prof3d_res.ptr(), prof3d_res->get_state());
	if (prof3d_fit) {
		ur->add_do_method(p_node, "set_shape_params", p_node->get_shape_params().duplicate());
	}
	hw_undo_state(ur, false, prof3d_res.ptr(), prof3d_before);
	if (prof3d_fit) {
		ur->add_undo_method(p_node, "set_shape_params", prof3d_params_before);
	}
	ur->commit_action(false);
	if (prof3d_fit) {
		prof3d_res->emit_changed(); // (what shows the curve elsewhere: up to date; it was quiet while it moved)
	}
	prof3d_fit = false;
}

// The curve under the mouse between two of its points: the point before (and where on the curve), or -1.
int HewnEditorPlugin::_prof3d_curve_at(Camera3D *p_cam, const HwProfPlace &p_place, const Vector2 &p_mouse, Vector2 &r_point) {
	const std::vector<Vector2> smp = p_place.res->to_vector();
	const PackedVector2Array pts = p_place.res->get_points();
	if (smp.size() < 2 || pts.size() < 2) {
		return -1;
	}
	float best = 8.0f;
	int seg = -1;
	float seg_t = 0.0f;
	for (size_t i = 0; i + 1 < smp.size(); i++) {
		const Vector3 a = p_place.to3(smp[i]), b = p_place.to3(smp[i + 1]);
		if (p_cam->is_position_behind(a) || p_cam->is_position_behind(b)) {
			continue;
		}
		const Vector2 sa = p_cam->unproject_position(a), sb = p_cam->unproject_position(b);
		const Vector2 ab = sb - sa;
		const float t = ab.length_squared() > 1e-9f ? CLAMP((p_mouse - sa).dot(ab) / ab.length_squared(), 0.0f, 1.0f) : 0.0f;
		const float d = p_mouse.distance_to(sa + ab * t);
		if (d < best) {
			best = d;
			seg = (int)i;
			seg_t = t;
		}
	}
	if (seg < 0) {
		return -1;
	}
	r_point = smp[seg].lerp(smp[seg + 1], seg_t);
	// (which piece: the sample nearest each point, in order along the curve)
	int piece = 0, from = 0;
	for (int k = 0; k < pts.size(); k++) {
		int at = from;
		float bd = 1e30f;
		for (int i = from; i < (int)smp.size(); i++) {
			const float d = smp[i].distance_squared_to(pts[k]);
			if (d < bd) {
				bd = d;
				at = i;
			}
		}
		from = at;
		if (at <= seg) {
			piece = k;
		}
	}
	return MIN(piece, (int)pts.size() - 2);
}

bool HewnEditorPlugin::_op_prof_live() {
	if (op_prof_op.is_empty() || last_op != op_prof_op || !redo_panel || !redo_panel->is_visible() || !op_prof_frame.made) {
		return false;
	}
	if (op_prof_op == "bevel" && (int)op_param("bevel", "profile_type") != 1) {
		return false;
	}
	if (op_prof_op == "arch" && (int)op_param("arch", "shape") != 1) {
		return false;
	}
	HewnMesh3D *n = get_edited();
	return n && n->get_instance_id() == last_op_node;
}

bool HewnEditorPlugin::_prof3d_any() {
	return _prof3d_target() != nullptr || _op_prof_live();
}

void HewnEditorPlugin::set_profile_in_view(bool p_on) {
	prof3d_on = p_on;
	prof3d_drag = -1;
	prof3d_sel_place = prof3d_sel_point = -1;
	HewnMesh3D *n = _prof3d_target();
	prof3d_node = p_on && n ? n->get_instance_id() : 0;
	if (p_on && n) {
		// (a curve of its own to drag: the built-in parts / shapes made curves; one undo step)
		const int t = n->get_shape();
		EditorUndoRedoManager *ur = get_undo_redo();
		if (t == hewn::shapes::COLUMN || t == hewn::shapes::SWEEP) {
			const bool need = t == hewn::shapes::COLUMN ? (n->get_base_profile().is_null() || n->get_shaft_profile().is_null() || n->get_top_profile().is_null())
														: n->get_sweep_profile().is_null() || (int)n->get_shape_param("profile") != 6;
			if (need) {
				ur->create_action("Hewn: Profile in the View");
				ur->add_do_method(n, "make_profiles_editable");
				for (const char *pp : { "base_profile", "shaft_profile", "top_profile", "sweep_profile" }) {
					ur->add_undo_property(n, pp, n->get(pp));
				}
				ur->add_undo_method(n, "set_shape_param", "profile", n->get_shape_param("profile"));
				ur->commit_action();
			}
		} else if (n->get_shape_profile().is_null()) {
			ur->create_action("Hewn: Shape Curve", UndoRedo::MERGE_DISABLE, n);
			ur->add_do_method(n, "set_shape_profile", default_shape_profile(t));
			ur->add_undo_method(n, "set_shape_profile", Ref<HewnProfile>());
			ur->commit_action();
		}
	}
	update_overlays();
}

// The curve a shape starts from when it gets one: what its parameters make (a rectangular door, a round arch, a
// straight slope).
Ref<HewnProfile> HewnEditorPlugin::default_shape_profile(int p_type) {
	Ref<HewnProfile> p;
	p.instantiate();
	if (p_type == hewn::shapes::DOOR) {
		p->set_points(PackedVector2Array({ Vector2(0, 0), Vector2(0, 1), Vector2(1, 1), Vector2(1, 0) }));
		p->set_symmetric(true);
	} else if (p_type == hewn::shapes::ARCH) {
		p->set_points(PackedVector2Array({ Vector2(0, 0), Vector2(0.5f, 1), Vector2(1, 0) }));
		p->set_point_handle_type(1, HewnProfile::HANDLE_AUTO);
		p->set_point_handle(0, true, Vector2(0, 0.55f));
		p->set_point_handle(2, false, Vector2(0, 0.55f));
		p->set_point_handle_type(0, HewnProfile::HANDLE_FREE);
		p->set_point_handle_type(2, HewnProfile::HANDLE_FREE);
		p->set_resolution(8);
		p->set_symmetric(true);
	} else {
		p->set_points(PackedVector2Array({ Vector2(0, 0), Vector2(1, 1) })); // (a wedge's slope, a roof's)
	}
	return p;
}

void HewnEditorPlugin::_draw_profile_in_view(Control *p_overlay, Camera3D *p_cam) {
	std::vector<HwProfPlace> pl;
	std::vector<HwSizeHandle> sizes;
	HewnMesh3D *n = nullptr;
	_prof3d_places(pl, sizes, &n);
	auto scr = [&](const Vector3 &w, Vector2 &r) {
		if (p_cam->is_position_behind(w)) {
			return false;
		}
		r = p_cam->unproject_position(w);
		return true;
	};
	const Color cc(0.3f, 0.9f, 1.0f);
	if (n && !pl.empty()) {
		draw_header(p_overlay, p_overlay->get_theme_default_font(),
				T("Curve in the view: drag a point to move it, click the curve to add one, Delete / X removes the picked point"));
	}
	for (int pi = 0; pi < (int)pl.size(); pi++) {
		const HwProfPlace &pp = pl[pi];
		const std::vector<Vector2> smp = pp.res->to_vector();
		Vector2 prev, s;
		bool has_prev = false;
		for (const Vector2 &q : smp) {
			if (!scr(pp.to3(q), s)) {
				has_prev = false;
				continue;
			}
			if (has_prev) {
				p_overlay->draw_line(prev, s, Color(cc, 0.85f), 1.5f, true);
			}
			prev = s;
			has_prev = true;
		}
		const PackedVector2Array pts = pp.res->get_points();
		for (int i = 0; i < (int)pts.size(); i++) {
			if (!scr(pp.to3(pts[i]), s)) {
				continue;
			}
			const bool on = pi == prof3d_sel_place && i == prof3d_sel_point;
			p_overlay->draw_circle(s, on ? 6.5f : 5.0f, Color(0, 0, 0, 0.6f));
			p_overlay->draw_circle(s, on ? 5.0f : 3.5f, on ? Color(1, 1, 1) : cc);
			if (on) {
				// its two handles
				for (int h = 0; h < 2; h++) {
					const Vector2 off = h == 0 ? pp.res->handle_in(i) : pp.res->handle_out(i);
					Vector2 hs;
					if (off.length() < 1e-6f || !scr(pp.to3(pts[i] + off), hs)) {
						continue;
					}
					p_overlay->draw_line(s, hs, Color(1, 0.75f, 0.2f, 0.9f), 1.2f, true);
					p_overlay->draw_rect(Rect2(hs - Vector2(4, 4), Vector2(8, 8)), Color(0, 0, 0, 0.6f));
					p_overlay->draw_rect(Rect2(hs - Vector2(3, 3), Vector2(6, 6)), Color(1, 0.75f, 0.2f));
				}
			}
		}
	}
	for (int k = 0; k < (int)sizes.size(); k++) {
		Vector2 s;
		if (!scr(sizes[k].at, s)) {
			continue;
		}
		const bool on = prof3d_drag == 1000 + k;
		Vector2 tip;
		if (scr(sizes[k].at + sizes[k].axis.normalized() * 0.15f, tip)) {
			p_overlay->draw_line(s, tip, Color(1, 0.5f, 0.3f, 0.9f), 1.5f, true);
		}
		p_overlay->draw_rect(Rect2(s - Vector2(5, 5), Vector2(10, 10)), Color(0, 0, 0, 0.6f));
		p_overlay->draw_rect(Rect2(s - Vector2(4, 4), Vector2(8, 8)), on ? Color(1, 1, 1) : Color(1, 0.5f, 0.3f));
	}
}

int32_t HewnEditorPlugin::_profile_in_view_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	std::vector<HwProfPlace> pl;
	std::vector<HwSizeHandle> sizes;
	HewnMesh3D *n = nullptr;
	_prof3d_places(pl, sizes, &n);
	if (pl.empty() && sizes.empty()) {
		prof3d_drag = -1;
		return AFTER_GUI_INPUT_PASS;
	}
	auto dist = [&](const Vector3 &w, const Vector2 &at) {
		return p_cam->is_position_behind(w) ? 1e9f : p_cam->unproject_position(w).distance_to(at);
	};
	// the point on the plane through `p_at` facing `p_n` under the mouse
	auto on_plane = [&](const Vector2 &p_mouse, const Vector3 &p_at, const Vector3 &p_n, Vector3 &r_hit) {
		Vector3 o, d;
		_ray(p_cam, p_mouse, o, d);
		const float den = p_n.dot(d);
		if (std::fabs(den) < 1e-6f) {
			return false;
		}
		r_hit = o + d * (p_n.dot(p_at - o) / den);
		return true;
	};
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	Ref<InputEventKey> key = p_event;
	if (key.is_valid()) {
		// Delete / X: the picked point out (the ends stay)
		if (!key->is_pressed() || key->is_echo() || (key_code(key) != KEY_DELETE && key_code(key) != KEY_X) || prof3d_drag >= 0 ||
				prof3d_sel_place < 0 || prof3d_sel_place >= (int)pl.size()) {
			return AFTER_GUI_INPUT_PASS;
		}
		const Ref<HewnProfile> pr = pl[prof3d_sel_place].res;
		if (prof3d_sel_point <= 0 || prof3d_sel_point >= pr->get_points().size() - 1 || pr->get_points().size() <= 2) {
			return AFTER_GUI_INPUT_STOP; // (an end: kept; the key is not the view's either)
		}
		prof3d_res = pr;
		prof3d_before = pr->get_state();
		_prof3d_fit_begin(n);
		if (prof3d_fit) {
			pr->set_block_signals(true);
		}
		pr->remove_point(prof3d_sel_point);
		if (prof3d_fit) {
			pr->set_block_signals(false);
			_prof3d_fit_apply(n);
		}
		if (n) {
			_prof3d_record(n, "Hewn: Remove Profile Point");
		}
		prof3d_sel_point = -1;
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		const Vector2 at = mb->get_position();
		if (mb->is_pressed()) {
			// a size handle, a handle of the picked point, a point (nearest within 10 pixels)
			float best = 10.0f;
			int what = -1, place = -1, point = -1;
			for (int k = 0; k < (int)sizes.size(); k++) {
				const float d = dist(sizes[k].at, at);
				if (d < best) {
					best = d;
					what = 1000 + k;
				}
			}
			if (prof3d_sel_place >= 0 && prof3d_sel_place < (int)pl.size()) {
				const HwProfPlace &pp = pl[prof3d_sel_place];
				const PackedVector2Array pts = pp.res->get_points();
				if (prof3d_sel_point >= 0 && prof3d_sel_point < pts.size()) {
					for (int h = 0; h < 2; h++) {
						const Vector2 off = h == 0 ? pp.res->handle_in(prof3d_sel_point) : pp.res->handle_out(prof3d_sel_point);
						const float d = off.length() > 1e-6f ? dist(pp.to3(pts[prof3d_sel_point] + off), at) : 1e9f;
						if (d < best) {
							best = d;
							what = 2000 + h;
						}
					}
				}
			}
			for (int pi = 0; pi < (int)pl.size(); pi++) {
				const PackedVector2Array pts = pl[pi].res->get_points();
				for (int i = 0; i < (int)pts.size(); i++) {
					const float d = dist(pl[pi].to3(pts[i]), at);
					if (d < best) {
						best = d;
						what = 0;
						place = pi;
						point = i;
					}
				}
			}
			if (what < 0) {
				// the curve between two points: a point there, dragged on
				for (int pi = 0; pi < (int)pl.size(); pi++) {
					Vector2 q;
					const int piece = _prof3d_curve_at(p_cam, pl[pi], at, q);
					if (piece < 0) {
						continue;
					}
					prof3d_res = pl[pi].res;
					prof3d_before = prof3d_res->get_state();
					_prof3d_fit_begin(n);
					if (prof3d_fit) {
						prof3d_res->set_block_signals(true);
					}
					const int idx = prof3d_res->insert_point(piece + 1, q);
					if (prof3d_fit) {
						prof3d_res->set_block_signals(false);
						_prof3d_fit_apply(n);
					}
					prof3d_sel_place = pi;
					prof3d_sel_point = idx;
					prof3d_drag = 0;
					prof3d_press = at;
					prof3d_moved = true; // (one undo step on release, moved or not)
					update_overlays();
					return AFTER_GUI_INPUT_STOP;
				}
				if (prof3d_sel_point >= 0) {
					prof3d_sel_point = -1; // (a click elsewhere: nothing picked, Delete is the view's again)
					update_overlays();
				}
				return AFTER_GUI_INPUT_PASS;
			}
			prof3d_press = at;
			prof3d_moved = false;
			if (what == 0) {
				// a click picks it (its handles show); dragging it moves it
				prof3d_sel_place = place;
				prof3d_sel_point = point;
				prof3d_drag = 0;
				prof3d_res = pl[place].res;
			} else if (what >= 2000) {
				prof3d_drag = what;
				prof3d_res = pl[prof3d_sel_place].res;
			} else {
				prof3d_drag = what;
				prof3d_res.unref();
				prof3d_size_param = sizes[what - 1000].param;
				prof3d_size_before = sizes[what - 1000].value;
				prof3d_size_at = sizes[what - 1000].at;
				prof3d_size_axis = sizes[what - 1000].axis;
			}
			if (prof3d_res.is_valid()) {
				prof3d_before = prof3d_res->get_state();
				_prof3d_fit_begin(n);
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (prof3d_drag < 0) {
			return AFTER_GUI_INPUT_PASS;
		}
		// the release: one undo step for what was dragged (a curve of an operation: its panel redoes it, no step)
		if (prof3d_moved && n) {
			EditorUndoRedoManager *ur = get_undo_redo();
			if (prof3d_drag >= 1000 && prof3d_drag < 2000) {
				ur->create_action("Hewn: Column Size", UndoRedo::MERGE_DISABLE, n);
				ur->add_do_method(n, "set_shape_param", prof3d_size_param, n->get_shape_param(prof3d_size_param));
				ur->add_undo_method(n, "set_shape_param", prof3d_size_param, prof3d_size_before);
				ur->commit_action(false);
			} else if (prof3d_res.is_valid()) {
				_prof3d_record(n, "Hewn: Move Profile Point");
			}
		}
		prof3d_fit = false;
		prof3d_drag = -1;
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mm.is_valid() && prof3d_drag >= 0) {
		if (!prof3d_moved && mm->get_position().distance_to(prof3d_press) < 4.0f) {
			return AFTER_GUI_INPUT_STOP; // (a click: only picked)
		}
		prof3d_moved = true;
		if (prof3d_drag >= 1000 && prof3d_drag < 2000) {
			// a size along its axis: the mouse on the plane through the handle facing the view, along the axis
			if (!n) {
				return AFTER_GUI_INPUT_STOP;
			}
			const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2);
			Vector3 hit;
			if (on_plane(mm->get_position(), prof3d_size_at, fwd, hit)) {
				const Vector3 ax = prof3d_size_axis;
				const float scale = MAX(1e-6f, ax.length());
				float v = prof3d_size_before + (hit - prof3d_size_at).dot(ax.normalized()) / scale;
				if (dock && dock->snap_enabled && dock->snap_enabled->is_pressed() && dock->snap_move) {
					const float st = MAX(1e-4f, (float)dock->snap_move->get_value());
					v = std::round(v / st) * st;
				}
				n->set_shape_param(prof3d_size_param, MAX(0.001f, v));
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (prof3d_sel_place < 0 || prof3d_sel_place >= (int)pl.size()) {
			return AFTER_GUI_INPUT_STOP;
		}
		const HwProfPlace &pp = pl[prof3d_sel_place];
		Vector3 hit;
		if (!on_plane(mm->get_position(), pp.o, pp.normal(), hit)) {
			return AFTER_GUI_INPUT_STOP;
		}
		if (dock && dock->snap_enabled && dock->snap_enabled->is_pressed() && dock->snap_move) {
			// (on the grid of the snap step, in meters, kept in the curve's plane)
			const float st = MAX(1e-4f, (float)dock->snap_move->get_value());
			const Vector3 s = (hit / st).round() * st;
			hit = s - pp.normal() * pp.normal().dot(s - pp.o);
		}
		Vector2 q = pp.to2(hit);
		const PackedVector2Array pts = pp.res->get_points();
		if (prof3d_sel_point < 0 || prof3d_sel_point >= pts.size()) {
			return AFTER_GUI_INPUT_STOP;
		}
		if (prof3d_fit) {
			pp.res->set_block_signals(true); // (the mesh made once, below)
		}
		if (prof3d_drag == 0) {
			if (pp.column) {
				q = Vector2(MAX(0.0f, q.x), CLAMP(q.y, 0.0f, 1.0f));
			}
			pp.res->set_point(prof3d_sel_point, q);
		} else {
			pp.res->set_point_handle(prof3d_sel_point, prof3d_drag == 2001, q - pts[prof3d_sel_point]);
		}
		if (prof3d_fit) {
			pp.res->set_block_signals(false);
			_prof3d_fit_apply(n);
		}
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	return AFTER_GUI_INPUT_PASS;
}
