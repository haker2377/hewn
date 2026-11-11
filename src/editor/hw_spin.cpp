// Spin tool: a ring around the 3D cursor across the chosen axis; dragging it spins the
// selection (extruded step by step around the axis) by the dragged angle. The steps, the angle and the
// rest stay in the panel of the last operation. X / Y / Z / V: the axis (V: the view).
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>

using namespace godot;
using namespace hw_editor;

namespace {
const float RING_PX = 70.0f;
} // namespace

void HewnEditorPlugin::start_spin_tool() {
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	if (!get_edited()) {
		return;
	}
	if (mode == MODE_OBJECT) {
		set_mode(element_mode);
	}
	modal = MODAL_SPIN;
	spin_dragging = false;
	spin_hover = false;
	update_overlays();
}

Vector3 HewnEditorPlugin::_spin_axis(Camera3D *p_cam) const {
	if (spin_axis < 3) {
		return AXES[spin_axis];
	}
	return p_cam ? p_cam->get_global_transform().basis.get_column(2).normalized() : Vector3(0, 1, 0);
}

// The angle of the mouse around the axis (radians, counter-clockwise looking against the axis).
bool HewnEditorPlugin::_spin_mouse_angle(Camera3D *p_cam, const Vector2 &p_pos, float &r_angle) const {
	const Vector3 ax = _spin_axis(p_cam);
	Vector3 u, v;
	hewn::plane_basis(ax, u, v);
	v = ax.cross(u).normalized();
	Vector3 hit;
	if (_plane_hit(p_cam, p_pos, cursor_pos, ax, hit) && (hit - cursor_pos).length() > 1e-6f) {
		const Vector3 d = hit - cursor_pos;
		r_angle = std::atan2(d.dot(v), d.dot(u));
		return true;
	}
	return false;
}

// The faces the spin made, with the image laid straight along them and bent by the turn: each strip goes on from the
// face next to the edge it started from (no such face: from nothing, the image across the edges and along the turn).
void HewnEditorPlugin::_spin_bend_uvs(hewn::Mesh &r_mesh, const std::vector<std::vector<Vector2>> &p_old_uvs, int p_first_new, const std::map<int, std::pair<int, int>> &p_ring, float p_step_length) {
	struct Strip {
		Vector2 uv_lo, uv_hi, along; // (at its edge's two vertices, the lower number first; the UV change for each meter along)
		bool found = false;
	};
	typedef std::pair<int, int> EdgeKey;
	auto corners_of = [&](const hewn::Face &p_f, int &r_step) {
		// the two vertices of the edge the strip started from, in the order of the face's corners; none: not a strip's face
		std::vector<int> roots;
		r_step = 1 << 30;
		for (int v : p_f.v) {
			const auto it = p_ring.find(v);
			if (it == p_ring.end()) {
				return std::vector<int>();
			}
			r_step = MIN(r_step, it->second.second);
		}
		for (int v : p_f.v) {
			const std::pair<int, int> &q = p_ring.at(v);
			if (q.second == r_step) {
				roots.push_back(q.first);
			} else if (q.second != r_step + 1) {
				return std::vector<int>();
			}
		}
		return roots.size() == 2 && roots[0] != roots[1] && p_f.size() == 4 ? roots : std::vector<int>();
	};
	std::map<EdgeKey, Strip> strips;
	for (int f = p_first_new; f < (int)r_mesh.faces.size(); f++) {
		int step = 0;
		const std::vector<int> roots = corners_of(r_mesh.faces[f], step);
		if (!roots.empty()) {
			strips[{ MIN(roots[0], roots[1]), MAX(roots[0], roots[1]) }] = Strip();
		}
	}
	// from the faces that were there
	for (int f = 0; f < p_first_new && f < (int)p_old_uvs.size(); f++) {
		const hewn::Face &F = r_mesh.faces[f];
		if (p_old_uvs[f].size() != F.v.size()) {
			continue;
		}
		for (int i = 0; i < F.size(); i++) {
			const int a = F.v[i], b = F.v[(i + 1) % F.size()];
			const auto it = strips.find({ MIN(a, b), MAX(a, b) });
			if (it == strips.end() || it->second.found) {
				continue;
			}
			const Vector3 pa = r_mesh.pos[a], e = r_mesh.pos[b] - pa;
			const float l2 = e.length_squared();
			if (l2 < 1e-12f) {
				continue;
			}
			// the corner farthest from the edge: how the image changes going away from it
			float best = 0.0f, best_along = 0.0f;
			int far = -1;
			for (int c = 0; c < F.size(); c++) {
				const Vector3 d = r_mesh.pos[F.v[c]] - pa;
				const float t = d.dot(e) / l2;
				const float h = (d - e * t).length();
				if (h > best) {
					best = h;
					best_along = t;
					far = c;
				}
			}
			if (far < 0 || best < 1e-6f) {
				continue;
			}
			const Vector2 ua = p_old_uvs[f][i], ub = p_old_uvs[f][(i + 1) % F.size()];
			Strip &S = it->second;
			S.uv_lo = a < b ? ua : ub;
			S.uv_hi = a < b ? ub : ua;
			S.along = -(p_old_uvs[f][far] - ua - (ub - ua) * best_along) / best; // (away from that face)
			S.found = true;
		}
	}
	// from nothing: the length along the edges that started it
	std::map<int, std::vector<std::pair<int, float>>> next;
	for (const auto &kv : strips) {
		const float l = r_mesh.pos[kv.first.first].distance_to(r_mesh.pos[kv.first.second]);
		next[kv.first.first].push_back({ kv.first.second, l });
		next[kv.first.second].push_back({ kv.first.first, l });
	}
	std::map<int, float> across;
	for (const auto &start : next) {
		if (across.count(start.first)) {
			continue;
		}
		across[start.first] = 0.0f;
		std::vector<int> todo = { start.first };
		while (!todo.empty()) {
			const int v = todo.back();
			todo.pop_back();
			for (const std::pair<int, float> &q : next[v]) {
				if (!across.count(q.first)) {
					across[q.first] = across[v] + q.second;
					todo.push_back(q.first);
				}
			}
		}
	}
	for (int f = p_first_new; f < (int)r_mesh.faces.size(); f++) {
		hewn::Face &F = r_mesh.faces[f];
		int step = 0;
		const std::vector<int> roots = corners_of(F, step);
		if (roots.empty()) {
			continue;
		}
		const EdgeKey key(MIN(roots[0], roots[1]), MAX(roots[0], roots[1]));
		Strip S = strips[key];
		if (!S.found) {
			S.uv_lo = Vector2(across[key.first] * F.uvp.scale.x, 0.0f);
			S.uv_hi = Vector2(across[key.second] * F.uvp.scale.x, 0.0f);
			S.along = Vector2(0.0f, F.uvp.scale.y);
		}
		F.uv.resize(F.v.size());
		for (int i = 0; i < F.size(); i++) {
			const std::pair<int, int> &q = p_ring.at(F.v[i]);
			F.uv[i] = (q.first == key.first ? S.uv_lo : S.uv_hi) + S.along * (p_step_length * (float)q.second);
		}
		F.manual_uv = true;
		F.uv_group = 0;
	}
}

int32_t HewnEditorPlugin::_spin_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		cancel_tool();
		return AFTER_GUI_INPUT_PASS;
	}
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	if (k.is_valid() && k->is_pressed() && !k->is_echo() && !k->is_ctrl_pressed() && !k->is_alt_pressed()) {
		const Key code = key_code(k);
		if (code == KEY_ESCAPE) {
			cancel_tool();
			return AFTER_GUI_INPUT_STOP;
		}
		if (!spin_dragging && (code == KEY_X || code == KEY_Y || code == KEY_Z || code == KEY_V)) {
			spin_axis = code == KEY_X ? 0 : code == KEY_Y ? 1 : code == KEY_Z ? 2 : 3;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}
	const Vector2 cs = p_cam->is_position_behind(cursor_pos) ? Vector2(-1e6f, -1e6f) : p_cam->unproject_position(cursor_pos);
	const float ring = RING_PX * EditorInterface::get_singleton()->get_editor_scale();
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_RIGHT && mb->is_pressed() && !spin_dragging) {
		cancel_tool();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		if (mb->is_pressed()) {
			// on the ring (the projected circle is about as far as the ring's size on screen): a spin
			float a = 0.0f;
			const float d = mb->get_position().distance_to(cs);
			if (d > ring * 0.4f && d < ring * 1.6f && _spin_mouse_angle(p_cam, mb->get_position(), a)) {
				spin_dragging = true;
				spin_started = false;
				spin_angle = 0.0f;
				spin_prev = a;
				spin_from = a;
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			return AFTER_GUI_INPUT_PASS; // (selection clicks go on)
		}
		if (spin_dragging) {
			spin_dragging = false;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (mm.is_valid()) {
		if (!spin_dragging) {
			const float d = mm->get_position().distance_to(cs);
			const bool hover = d > ring * 0.4f && d < ring * 1.6f;
			if (hover != spin_hover) {
				spin_hover = hover;
				update_overlays();
			}
			return AFTER_GUI_INPUT_PASS;
		}
		float a = 0.0f;
		if (!_spin_mouse_angle(p_cam, mm->get_position(), a)) {
			return AFTER_GUI_INPUT_STOP;
		}
		const float delta = Math::wrapf(a - spin_prev, (float)-Math::PI, (float)Math::PI);
		spin_prev = a;
		spin_angle += Math::rad_to_deg(delta);
		float shown = spin_angle;
		if (snapping_active(mm->is_ctrl_pressed() || mm->is_meta_pressed()) && dock) {
			const float st = MAX(0.1f, (float)dock->snap_rotate->get_value());
			shown = std::round(shown / st) * st;
		}
		if (!spin_started && std::fabs(shown) >= 1.0f) {
			const Vector3 ax = _spin_axis(p_cam);
			set_op_param("spin", "cx", cursor_pos.x);
			set_op_param("spin", "cy", cursor_pos.y);
			set_op_param("spin", "cz", cursor_pos.z);
			set_op_param("spin", "ax", ax.x);
			set_op_param("spin", "ay", ax.y);
			set_op_param("spin", "az", ax.z);
			set_op_param("spin", "angle", shown);
			run_op("spin");
			spin_started = true;
			modal = MODAL_SPIN; // (the operation does not end the tool)
		} else if (spin_started && std::fabs(shown - (float)op_param("spin", "angle")) > 1e-3f) {
			set_op_param("spin", "angle", shown);
			_redo_last();
		}
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	return AFTER_GUI_INPUT_PASS;
}

void HewnEditorPlugin::_draw_spin(Control *p_overlay, Camera3D *p_cam) {
	Ref<Font> font = p_overlay->get_theme_default_font();
	const Vector3 ax = _spin_axis(p_cam);
	Vector3 u, v;
	hewn::plane_basis(ax, u, v);
	v = ax.cross(u).normalized();
	const float r = RING_PX * EditorInterface::get_singleton()->get_editor_scale() * _world_per_pixel(p_cam, cursor_pos);
	const Color col = spin_axis < 3 ? AXIS_COLORS[spin_axis] : Color(0.9f, 0.9f, 0.9f);
	auto at = [&](float a) { return cursor_pos + (u * std::cos(a) + v * std::sin(a)) * r; };
	PackedVector2Array ring;
	bool ok = true;
	for (int i = 0; i <= 64 && ok; i++) {
		const Vector3 p = at((float)Math::PI * 2.0f * i / 64);
		ok = !p_cam->is_position_behind(p);
		ring.push_back(p_cam->unproject_position(p));
	}
	if (ok) {
		p_overlay->draw_polyline(ring, col * Color(1, 1, 1, spin_hover || spin_dragging ? 1.0f : 0.7f), spin_hover || spin_dragging ? 3.0f : 2.0f, true);
	}
	if (spin_dragging && ok) {
		// the swept part and its two ends (round handles)
		const float a0 = spin_from, a1 = spin_from + Math::deg_to_rad((float)op_param("spin", "angle")) * (spin_started ? 1.0f : 0.0f);
		PackedVector2Array arc;
		const int seg = MAX(2, (int)(std::fabs(a1 - a0) / 0.05f));
		for (int i = 0; i <= seg; i++) {
			arc.push_back(p_cam->unproject_position(at(a0 + (a1 - a0) * i / seg)));
		}
		p_overlay->draw_polyline(arc, TOOL_COLOR, 5.0f, true);
		p_overlay->draw_circle(p_cam->unproject_position(at(a0)), 6.0f, Color(1, 1, 1));
		p_overlay->draw_circle(p_cam->unproject_position(at(a1)), 7.0f, TOOL_COLOR);
	} else if (ok) {
		p_overlay->draw_circle(p_cam->unproject_position(at(0.0f)), 6.0f, col);
		p_overlay->draw_circle(p_cam->unproject_position(at((float)Math::PI)), 6.0f, col);
	}
	static const char *names[4] = { "X", "Y", "Z", "View" };
	String text = vformat(T("Spin around %s: drag the ring (the 3D cursor is the center)"), T(names[spin_axis]));
	if (spin_dragging && spin_started) {
		text += vformat("   %.1f°", (float)op_param("spin", "angle"));
	}
	text += T("   |   X / Y / Z / V: axis, Ctrl: snapping the other way, RMB / Esc: done");
	draw_header(p_overlay, font, text);
}
