// Hewn — a Path3D's points edited curve edit mode: Tab on a selected Path3D, then click /
// Shift+click / drag a box to select points, A / Alt+A, G / R / S (X / Y / Z: an axis, Ctrl: snapping, LMB /
// Enter: done, RMB / Esc: back), E: a new point after the selected end, Ctrl+click: a new point at the mouse,
// X / Delete: remove, V: smooth (auto) / straight (vector) handles; Tab: done.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/curve3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/path3d.hpp>

#include <algorithm>

using namespace godot;
using namespace hw_editor;

Path3D *HewnEditorPlugin::_curve_path() const {
	Path3D *p = curve_id.is_valid() ? Object::cast_to<Path3D>(ObjectDB::get_instance(curve_id)) : nullptr;
	return p && p->is_inside_tree() && p->get_curve().is_valid() ? p : nullptr;
}

// One undo step: the curve as it was when the change began, and as it is.
void HewnEditorPlugin::_curve_commit(const String &p_name) {
	Path3D *p = _curve_path();
	if (!p || curve_before.is_null()) {
		return;
	}
	Ref<Curve3D> after = p->get_curve()->duplicate();
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: " + p_name);
	ur->add_do_property(p, "curve", after);
	ur->add_undo_property(p, "curve", curve_before);
	ur->commit_action();
	curve_before = Ref<Curve3D>();
}

namespace {
bool cam_plane_hit(Camera3D *p_cam, const Vector2 &p_pos, const Vector3 &p_point, Vector3 &r_hit) {
	const Vector3 o = p_cam->project_ray_origin(p_pos), d = p_cam->project_ray_normal(p_pos);
	const Vector3 n = p_cam->get_global_transform().basis.get_column(2).normalized();
	const float den = n.dot(d);
	if (std::fabs(den) < 1e-6f) {
		return false;
	}
	r_hit = o + d * (n.dot(p_point - o) / den);
	return true;
}
} // namespace

int32_t HewnEditorPlugin::_curve_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	Path3D *path = _curve_path();
	if (!path) {
		curve_edit = false;
		return -1;
	}
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	const Key code = k.is_valid() && k->is_pressed() && !k->is_echo() ? key_code(k) : KEY_NONE;
	const bool ctrl = k.is_valid() && (k->is_ctrl_pressed() || k->is_meta_pressed());
	const bool shift = k.is_valid() && k->is_shift_pressed(), alt = k.is_valid() && k->is_alt_pressed();
	if (!curve_edit) {
		if (code == KEY_TAB && !ctrl && !shift && !alt) {
			curve_edit = true;
			curve_sel.clear();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return -1;
	}
	Ref<Curve3D> c = path->get_curve();
	const Transform3D g = path->get_global_transform();
	const int count = c->get_point_count();
	curve_sel.erase(std::remove_if(curve_sel.begin(), curve_sel.end(), [&](int i) { return i < 0 || i >= count; }), curve_sel.end());
	auto world = [&](int i) { return g.xform(c->get_point_position(i)); };
	auto center = [&]() {
		Vector3 s;
		for (int i : curve_sel) {
			s += world(i);
		}
		return curve_sel.empty() ? s : s / (float)curve_sel.size();
	};
	auto nearest = [&](const Vector2 &p_pos) {
		int best = -1;
		float bd = 12.0f * EditorInterface::get_singleton()->get_editor_scale();
		for (int i = 0; i < count; i++) {
			const Vector3 w = world(i);
			if (!p_cam->is_position_behind(w) && p_cam->unproject_position(w).distance_to(p_pos) < bd) {
				bd = p_cam->unproject_position(w).distance_to(p_pos);
				best = i;
			}
		}
		return best;
	};
	auto begin_xf = [&](int p_kind) {
		if (curve_sel.empty()) {
			return;
		}
		curve_xf = p_kind;
		curve_axis = -1;
		curve_mouse0 = mouse_pos;
		curve_center = center();
		curve_start.clear();
		for (int i : curve_sel) {
			curve_start.push_back(c->get_point_position(i));
		}
		if (curve_before.is_null()) {
			curve_before = c->duplicate();
		}
	};
	auto apply_xf = [&](bool p_snap) {
		const Basis inv = g.basis.inverse();
		for (size_t k2 = 0; k2 < curve_sel.size(); k2++) {
			const Vector3 w0 = g.xform(curve_start[k2]);
			Vector3 w = w0;
			if (curve_xf == 1) {
				Vector3 a, b;
				if (curve_axis >= 0 || (cam_plane_hit(p_cam, curve_mouse0, curve_center, a) && cam_plane_hit(p_cam, mouse_pos, curve_center, b))) {
					Vector3 d = b - a;
					if (curve_axis >= 0) {
						// (along the axis line: where the mouse's ray passes it)
						const Vector3 ax = AXES[curve_axis];
						d = ax * (_axis_param(p_cam, mouse_pos, curve_center, ax) - _axis_param(p_cam, curve_mouse0, curve_center, ax));
					}
					if (p_snap && dock) {
						const float st = MAX(1e-4f, (float)dock->snap_move->get_value());
						d = Vector3(std::round(d.x / st) * st, std::round(d.y / st) * st, std::round(d.z / st) * st);
					}
					w = w0 + d;
				}
			} else if (curve_xf == 2) {
				const Vector2 cc = p_cam->unproject_position(curve_center);
				float ang = -((mouse_pos - cc).angle() - (curve_mouse0 - cc).angle());
				if (p_snap && dock) {
					const float st = Math::deg_to_rad(MAX(0.1f, (float)dock->snap_rotate->get_value()));
					ang = std::round(ang / st) * st;
				}
				Vector3 ax = curve_axis >= 0 ? AXES[curve_axis] : p_cam->get_global_transform().basis.get_column(2).normalized();
				if (ax.dot(p_cam->get_global_position() - curve_center) < 0.0f) {
					ax = -ax;
				}
				w = curve_center + Basis(ax, ang).xform(w0 - curve_center);
			} else if (curve_xf == 3) {
				const Vector2 cc = p_cam->unproject_position(curve_center);
				float f = mouse_pos.distance_to(cc) / MAX(1.0f, curve_mouse0.distance_to(cc));
				if (p_snap) {
					f = std::round(f * 10.0f) / 10.0f;
				}
				Vector3 d = w0 - curve_center;
				if (curve_axis >= 0) {
					const Vector3 ax = AXES[curve_axis];
					d += ax * (d.dot(ax) * (f - 1.0f));
				} else {
					d *= f;
				}
				w = curve_center + d;
			}
			c->set_point_position(curve_sel[k2], inv.xform(w - g.origin));
		}
	};
	// a move / turn / scale going on
	if (curve_xf != 0) {
		if (mm.is_valid()) {
			mouse_pos = mm->get_position();
			apply_xf(mm->is_ctrl_pressed() || mm->is_meta_pressed());
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_X || code == KEY_Y || code == KEY_Z) {
			const int a = code == KEY_X ? 0 : code == KEY_Y ? 1 : 2;
			curve_axis = curve_axis == a ? -1 : a;
			apply_xf(false);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		const bool confirm = code == KEY_ENTER || code == KEY_KP_ENTER || (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_LEFT);
		const bool cancel = code == KEY_ESCAPE || (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT);
		if (confirm || cancel) {
			if (cancel) {
				for (size_t k2 = 0; k2 < curve_sel.size(); k2++) {
					c->set_point_position(curve_sel[k2], curve_start[k2]);
				}
				// (a cancelled extrusion: the new point goes again)
				if (curve_extruded >= 0 && curve_extruded < c->get_point_count()) {
					c->remove_point(curve_extruded);
					curve_sel.clear();
				}
			}
			static const char *names[4] = { "", "Move Curve Points", "Rotate Curve Points", "Scale Curve Points" };
			const String name = curve_extruded >= 0 ? String("Extrude Curve Point") : String(names[curve_xf]);
			curve_xf = 0;
			curve_extruded = -1;
			if (cancel && curve_before.is_valid()) {
				path->set_curve(curve_before->duplicate());
				curve_before = Ref<Curve3D>();
			} else {
				_curve_commit(name);
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return k.is_valid() ? AFTER_GUI_INPUT_STOP : AFTER_GUI_INPUT_PASS;
	}
	if (code != KEY_NONE) {
		if (code == KEY_TAB && !ctrl && !shift && !alt) {
			curve_edit = false;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_A && !ctrl && !shift) {
			curve_sel.clear();
			if (!alt) {
				for (int i = 0; i < count; i++) {
					curve_sel.push_back(i);
				}
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if ((code == KEY_G || code == KEY_R || code == KEY_S) && !ctrl && !shift && !alt) {
			begin_xf(code == KEY_G ? 1 : code == KEY_R ? 2 : 3);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_E && !ctrl && !shift && !alt && curve_sel.size() == 1 && count > 0 && (curve_sel[0] == 0 || curve_sel[0] == count - 1)) {
			// a new point after the selected end, then it follows the mouse
			curve_before = c->duplicate();
			const int at = curve_sel[0] == 0 && count > 1 ? 0 : count;
			c->add_point(c->get_point_position(curve_sel[0]), Vector3(), Vector3(), at == 0 ? 0 : -1);
			curve_extruded = at;
			curve_sel = { at };
			begin_xf(1);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if ((code == KEY_X || code == KEY_DELETE) && !ctrl && !shift && !alt && !curve_sel.empty()) {
			curve_before = c->duplicate();
			std::vector<int> s = curve_sel;
			std::sort(s.rbegin(), s.rend());
			for (int i : s) {
				c->remove_point(i);
			}
			curve_sel.clear();
			_curve_commit("Delete Curve Points");
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_V && !ctrl && !shift && !alt && !curve_sel.empty()) {
			// smooth handles, or straight ones when they are smooth already
			curve_before = c->duplicate();
			bool straight = true;
			for (int i : curve_sel) {
				straight = straight && c->get_point_in(i).length() < 1e-6f && c->get_point_out(i).length() < 1e-6f;
			}
			for (int i : curve_sel) {
				if (!straight) {
					c->set_point_in(i, Vector3());
					c->set_point_out(i, Vector3());
					continue;
				}
				const Vector3 p = c->get_point_position(i);
				const Vector3 prev = i > 0 ? c->get_point_position(i - 1) : p, next = i + 1 < count ? c->get_point_position(i + 1) : p;
				const Vector3 dir = (next - prev).normalized();
				c->set_point_in(i, i > 0 ? -dir * (p.distance_to(prev) / 3.0f) : Vector3());
				c->set_point_out(i, i + 1 < count ? dir * (p.distance_to(next) / 3.0f) : Vector3());
			}
			_curve_commit("Curve Handles");
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_ESCAPE) {
			curve_sel.clear();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return -1;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		const Vector2 pos = mb->get_position();
		if (mb->is_pressed()) {
			if (mb->is_ctrl_pressed() || mb->is_meta_pressed()) {
				// a new point at the mouse, after the selected end (or the last point), on the plane through it
				curve_before = c->duplicate();
				const bool at_start = curve_sel.size() == 1 && curve_sel[0] == 0 && count > 1;
				const Vector3 ref = count > 0 ? world(at_start ? 0 : count - 1) : g.origin;
				Vector3 hit;
				if (cam_plane_hit(p_cam, pos, ref, hit)) {
					c->add_point(g.affine_inverse().xform(hit), Vector3(), Vector3(), at_start ? 0 : -1);
					curve_sel = { at_start ? 0 : c->get_point_count() - 1 };
					_curve_commit("Add Curve Point");
				}
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			curve_press = pos;
			curve_pressing = true;
			curve_box = false;
			return AFTER_GUI_INPUT_STOP;
		}
		if (curve_pressing) {
			curve_pressing = false;
			const bool add = mb->is_shift_pressed();
			if (curve_box) {
				const Rect2 r = Rect2(curve_press, Vector2()).expand(pos);
				if (!add) {
					curve_sel.clear();
				}
				for (int i = 0; i < count; i++) {
					const Vector3 w = world(i);
					if (!p_cam->is_position_behind(w) && r.has_point(p_cam->unproject_position(w)) && std::find(curve_sel.begin(), curve_sel.end(), i) == curve_sel.end()) {
						curve_sel.push_back(i);
					}
				}
			} else {
				const int i = nearest(pos);
				if (!add) {
					curve_sel.clear();
				}
				if (i >= 0) {
					auto it = std::find(curve_sel.begin(), curve_sel.end(), i);
					if (it != curve_sel.end() && add) {
						curve_sel.erase(it);
					} else if (it == curve_sel.end()) {
						curve_sel.push_back(i);
					}
				}
			}
			curve_box = false;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
	}
	if (mm.is_valid()) {
		mouse_pos = mm->get_position();
		if (curve_pressing && mouse_pos.distance_to(curve_press) > 4.0f) {
			curve_box = true;
		}
		if (curve_pressing) {
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
	}
	return -1;
}

void HewnEditorPlugin::_draw_curve_edit(Control *p_overlay, Camera3D *p_cam) {
	Path3D *path = _curve_path();
	if (!path || !curve_edit) {
		return;
	}
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	Ref<Curve3D> c = path->get_curve();
	const Transform3D g = path->get_global_transform();
	const int count = c->get_point_count();
	const Color sel_col(1.0f, 0.6f, 0.1f);
	for (int i = 0; i < count; i++) {
		const Vector3 w = g.xform(c->get_point_position(i));
		if (p_cam->is_position_behind(w)) {
			continue;
		}
		const Vector2 p = p_cam->unproject_position(w);
		const bool sel = std::find(curve_sel.begin(), curve_sel.end(), i) != curve_sel.end();
		if (sel) {
			// its handles
			for (const Vector3 &h : { c->get_point_in(i), c->get_point_out(i) }) {
				if (h.length() < 1e-6f) {
					continue;
				}
				const Vector3 hw = g.xform(c->get_point_position(i) + h);
				if (!p_cam->is_position_behind(hw)) {
					const Vector2 hp = p_cam->unproject_position(hw);
					p_overlay->draw_line(p, hp, Color(0.9f, 0.85f, 0.4f, 0.9f), 1.0f, true);
					p_overlay->draw_circle(hp, 3.0f * es, Color(0.9f, 0.85f, 0.4f));
				}
			}
		}
		p_overlay->draw_circle(p, 5.0f * es, Color(0, 0, 0));
		p_overlay->draw_circle(p, 4.0f * es, sel ? sel_col : Color(0.85f, 0.85f, 0.85f));
	}
	if (curve_pressing && curve_box) {
		const Rect2 r = Rect2(curve_press, Vector2()).expand(mouse_pos);
		p_overlay->draw_rect(r, Color(1, 0.6f, 0.1f, 0.12f));
		p_overlay->draw_rect(r, Color(1, 0.6f, 0.1f, 0.8f), false, 1.0f);
	}
	String info = T("Curve: click / Shift / box to select, A, G / R / S (X / Y / Z), E: extrude, Ctrl+click: add, X: delete, V: handles, Tab: done");
	if (curve_xf != 0) {
		static const char *axes[4] = { "", "  X", "  Y", "  Z" };
		info = T("LMB / Enter: done, RMB / Esc: back, X / Y / Z: an axis, Ctrl: snapping") + String(axes[curve_axis + 1]);
	}
	draw_header(p_overlay, p_overlay->get_theme_default_font(), info);
}
