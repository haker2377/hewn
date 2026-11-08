// Sculpt mode: brushes that move the vertices of the edited mesh (raise / lower, smooth, flatten, grab, noise,
// terraces, pinch, inflate, crease). The header's second row holds the brush (radius in pixels, strength, the
// direction: up or along the surface, the falloff and the pen); the tool column the tools. A stroke is one undo
// step; while it goes only the positions and normals are shown again (the rest when it ends).
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/curve.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_resource_picker.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/popup_panel.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/theme.hpp>

#include <cmath>

using namespace godot;
using namespace hw_editor;

namespace {

const char *SCULPT_TOOL_NAMES[HewnEditorPlugin::ST_COUNT] = { "Raise / Lower", "Smoothing", "Flatten", "Grab", "Noise", "Terrace", "Pinch", "Inflate", "Sharp Edge",
	"Density" };

// value noise in 3D (stable, -1..1)
float hash3(int x, int y, int z) {
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2147483647u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return (float)(h & 0xffffff) / (float)0x7fffff - 1.0f;
}

float value_noise(const Vector3 &p) {
	const int x0 = (int)std::floor(p.x), y0 = (int)std::floor(p.y), z0 = (int)std::floor(p.z);
	const float fx = p.x - x0, fy = p.y - y0, fz = p.z - z0;
	auto s = [](float t) { return t * t * (3.0f - 2.0f * t); };
	const float ux = s(fx), uy = s(fy), uz = s(fz);
	float c[2][2][2];
	for (int i = 0; i < 2; i++) {
		for (int j = 0; j < 2; j++) {
			for (int k = 0; k < 2; k++) {
				c[i][j][k] = hash3(x0 + i, y0 + j, z0 + k);
			}
		}
	}
	auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	const float x00 = lerp(c[0][0][0], c[1][0][0], ux), x10 = lerp(c[0][1][0], c[1][1][0], ux);
	const float x01 = lerp(c[0][0][1], c[1][0][1], ux), x11 = lerp(c[0][1][1], c[1][1][1], ux);
	return lerp(lerp(x00, x10, uy), lerp(x01, x11, uy), uz);
}

} // namespace

// ------------------------------------------------------------------ the mode

void HewnEditorPlugin::enter_sculpt() {
	HewnMesh3D *n = get_edited();
	if (!n) {
		ERR_PRINT("Hewn: select a Hewn mesh to sculpt it.");
		return;
	}
	if (vpaint) {
		exit_vpaint();
	}
	if (mode == MODE_OBJECT) {
		set_mode(element_mode);
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	sculpt = true;
	if (dock) {
		dock->show_paint_bar(true);
	}
	start_sculpt_tool(sculpt_tool);
	for (HewnMesh3D *m : edited_meshes()) {
		m->update_gizmos(); // (no wire, no vertices while sculpting)
	}
	update_overlays();
}

void HewnEditorPlugin::exit_sculpt() {
	if (!sculpt) {
		return;
	}
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
	if (mode != MODE_OBJECT) {
		set_mode(MODE_OBJECT);
	}
	for (HewnMesh3D *m : edited_meshes()) {
		m->update_gizmos();
	}
	update_overlays();
}

void HewnEditorPlugin::start_sculpt_tool(int p_tool) {
	if (!sculpt) {
		enter_sculpt();
		if (!sculpt) {
			return;
		}
	}
	if (modal != MODAL_NONE && modal != MODAL_SCULPT) {
		cancel_tool();
	}
	sculpt_tool = CLAMP(p_tool, 0, (int)ST_COUNT - 1);
	modal = MODAL_SCULPT;
	if (dock) {
		dock->sync_sculpt_bar();
	}
	update_overlays();
}

// ------------------------------------------------------------------ the brush

float HewnEditorPlugin::_sculpt_falloff(float p_d) const {
	// p_d: the distance from the middle, 0..1
	const float d = CLAMP(p_d, 0.0f, 1.0f);
	const int type = dock && dock->sculpt_falloff_type ? dock->sculpt_falloff_type->get_selected() : 0;
	if (type == 6) {
		return custom_falloff(dock ? dock->sculpt_curve : Ref<Curve>(), d);
	}
	const float t = 1.0f - d;
	switch (type) {
		case 1:
			return std::sqrt(MAX(0.0f, 1.0f - d * d)); // sphere
		case 2:
			return std::sqrt(t); // root
		case 3:
			return t * t; // sharp
		case 4:
			return t; // linear
		case 5:
			return 1.0f; // constant
		default:
			return t * t * (3.0f - 2.0f * t); // smooth
	}
}

// The curve of a Custom falloff: x the distance from the middle (0) to the edge (1), y the strength. None: smooth.
float HewnEditorPlugin::custom_falloff(const Ref<Curve> &p_curve, float p_d) {
	const float d = CLAMP(p_d, 0.0f, 1.0f);
	if (p_curve.is_null() || p_curve->get_point_count() == 0) {
		const float t = 1.0f - d;
		return t * t * (3.0f - 2.0f * t);
	}
	return CLAMP(p_curve->sample(d), 0.0f, 1.0f);
}

// The surface under the mouse (world): brute force over the triangles as they are now (a stroke changes the
// positions every step; a search tree would have to be made again each time).
bool HewnEditorPlugin::_sculpt_hit(Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_world, Vector3 &r_normal) const {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return false;
	}
	Vector3 from, dir;
	_ray(p_cam, p_pos, from, dir);
	const Transform3D g = n->get_global_transform();
	const Transform3D inv = g.affine_inverse();
	const Vector3 o = inv.xform(from), d = inv.basis.xform(dir);
	const hewn::Mesh &m = n->get_data();
	const hewn::BuildOut &b = n->get_build();
	float best = 1e30f;
	int best_f = -1;
	for (int f = 0; f < (int)m.faces.size() && f < (int)b.face_tris.size(); f++) {
		const hewn::Face &F = m.faces[f];
		if (F.hidden) {
			continue;
		}
		const std::vector<int> &tr = b.face_tris[f];
		for (size_t k = 0; k + 2 < tr.size(); k += 3) {
			const Vector3 &a = m.pos[F.v[tr[k]]], &bb = m.pos[F.v[tr[k + 1]]], &c = m.pos[F.v[tr[k + 2]]];
			const Vector3 e1 = bb - a, e2 = c - a;
			const Vector3 pv = d.cross(e2);
			const float det = e1.dot(pv);
			if (std::fabs(det) < 1e-12f) {
				continue;
			}
			const float id = 1.0f / det;
			const Vector3 tv = o - a;
			const float u = tv.dot(pv) * id;
			if (u < 0.0f || u > 1.0f) {
				continue;
			}
			const Vector3 qv = tv.cross(e1);
			const float v = d.dot(qv) * id;
			if (v < 0.0f || u + v > 1.0f) {
				continue;
			}
			const float t = e2.dot(qv) * id;
			if (t > 1e-6f && t < best) {
				best = t;
				best_f = f;
			}
		}
	}
	if (best_f < 0) {
		return false;
	}
	r_world = g.xform(o + d * best);
	r_normal = g.basis.inverse().transposed().xform(hewn::face_normal(m, m.faces[best_f])).normalized();
	return true;
}

void HewnEditorPlugin::_sculpt_begin(Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	const hewn::Mesh &m = n->get_data();
	// the neighbors of each vertex (smoothing) and its faces (normals)
	sculpt_adj.assign(m.pos.size(), {});
	sculpt_vf.assign(m.pos.size(), {});
	for (int f = 0; f < (int)m.faces.size(); f++) {
		const hewn::Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			const int a = F.v[i], c = F.v[(i + 1) % F.size()];
			sculpt_vf[a].push_back(f);
			if (std::find(sculpt_adj[a].begin(), sculpt_adj[a].end(), c) == sculpt_adj[a].end()) {
				sculpt_adj[a].push_back(c);
			}
			if (std::find(sculpt_adj[c].begin(), sculpt_adj[c].end(), a) == sculpt_adj[c].end()) {
				sculpt_adj[c].push_back(a);
			}
		}
	}
	// the mask: what is selected in edit mode, if anything
	const hewn::Selection &s = n->selection;
	std::vector<int> mv = mode == MODE_VERTEX ? s.verts : mode == MODE_EDGE ? hewn::sel::verts_of_edges(s.edges) : hewn::sel::verts_of_faces(m, s.faces);
	sculpt_mask.assign(mv.empty() ? 0 : m.pos.size(), 0);
	for (int v : mv) {
		if (v >= 0 && v < (int)sculpt_mask.size()) {
			sculpt_mask[v] = 1;
		}
	}
	sculpt_plane_set = false;
	sculpt_grab_v.clear();
	(void)p_cam;
}

void HewnEditorPlugin::_sculpt_dab(Camera3D *p_cam, const Vector2 &p_pos, bool p_first, bool p_ctrl, bool p_shift) {
	HewnMesh3D *n = get_edited();
	if (!n || !dock) {
		return;
	}
	hewn::Mesh &m = n->get_data();
	const Transform3D g = n->get_global_transform();
	const Transform3D inv = g.affine_inverse();
	const int tool = p_shift && sculpt_tool != ST_GRAB ? (int)ST_SMOOTH : sculpt_tool;
	const bool pressure_strength = dock->sculpt_pressure_strength && dock->sculpt_pressure_strength->is_pressed();
	const bool pressure_radius = dock->sculpt_pressure_radius && dock->sculpt_pressure_radius->is_pressed();
	const float pressure = CLAMP(sculpt_pressure, 0.0f, 1.0f);
	const float radius_px = (float)dock->sculpt_radius_field->get_value() * (pressure_radius ? MAX(0.05f, pressure) : 1.0f);
	const float strength = (float)dock->sculpt_strength_field->get_value() * (pressure_strength ? pressure : 1.0f);
	const bool up = !dock->sculpt_direction || dock->sculpt_direction->get_selected() == 0;
	if (tool == ST_GRAB && !p_first) {
		// grab: the vertices taken at the start follow the mouse on the plane through the start point
		if (sculpt_grab_v.empty()) {
			return;
		}
		Vector3 from, dir;
		_ray(p_cam, p_pos, from, dir);
		const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2).normalized();
		const float den = dir.dot(fwd);
		if (std::fabs(den) < 1e-6f) {
			return;
		}
		const Vector3 at = from + dir * ((sculpt_grab_start - from).dot(fwd) / den);
		const Vector3 delta = at - sculpt_grab_start;
		for (size_t i = 0; i < sculpt_grab_v.size(); i++) {
			m.pos[sculpt_grab_v[i]] = inv.xform(sculpt_grab_p0[i] + delta * sculpt_grab_w[i]);
		}
		n->commit_positions();
		return;
	}
	Vector3 hit, hit_n;
	if (!_sculpt_hit(p_cam, p_pos, hit, hit_n)) {
		return;
	}
	const float r = radius_px * _world_per_pixel(p_cam, hit);
	if (r <= 0.0f) {
		return;
	}
	// the vertices in reach, their weights and world positions
	std::vector<int> vs;
	std::vector<float> ws;
	std::vector<Vector3> ps;
	for (int v = 0; v < (int)m.pos.size(); v++) {
		if (!sculpt_mask.empty() && !sculpt_mask[v]) {
			continue;
		}
		const Vector3 p = g.xform(m.pos[v]);
		const float dd = p.distance_to(hit);
		if (dd >= r) {
			continue;
		}
		vs.push_back(v);
		ws.push_back(_sculpt_falloff(dd / r));
		ps.push_back(p);
	}
	if (vs.empty()) {
		return;
	}
	const Basis nb = g.basis.inverse().transposed();
	auto vnormal = [&](int v) {
		Vector3 s;
		for (int f : sculpt_vf[v]) {
			s += hewn::face_normal_raw(m, m.faces[f]);
		}
		s = nb.xform(s);
		return s.length() > 1e-12f ? s.normalized() : hit_n;
	};
	// the brush's direction: straight up, or the surface's under the brush (its vertices' normals, weighted)
	Vector3 area_n;
	Vector3 center;
	float wsum = 0.0f;
	for (size_t i = 0; i < vs.size(); i++) {
		area_n += vnormal(vs[i]) * ws[i];
		center += ps[i] * ws[i];
		wsum += ws[i];
	}
	area_n = area_n.length() > 1e-9f ? area_n.normalized() : hit_n;
	center = wsum > 0.0f ? center / wsum : hit;
	const Vector3 dirv = up ? Vector3(0, 1, 0) : area_n;
	const float sign = p_ctrl ? -1.0f : 1.0f;
	if (tool == ST_GRAB) {
		sculpt_grab_v = vs;
		sculpt_grab_w = ws;
		sculpt_grab_p0 = ps;
		sculpt_grab_start = hit;
		return;
	}
	std::vector<Vector3> out = ps;
	switch (tool) {
		case ST_DRAW:
			for (size_t i = 0; i < vs.size(); i++) {
				out[i] += dirv * (sign * strength * r * 0.08f * ws[i]);
			}
			break;
		case ST_SMOOTH:
			for (size_t i = 0; i < vs.size(); i++) {
				const std::vector<int> &nbs = sculpt_adj[vs[i]];
				if (nbs.empty()) {
					continue;
				}
				Vector3 avg;
				for (int o : nbs) {
					avg += g.xform(m.pos[o]);
				}
				avg /= (float)nbs.size();
				out[i] = ps[i].lerp(avg, MIN(1.0f, strength * ws[i]));
			}
			break;
		case ST_FLATTEN: {
			// to the plane under the brush (Ctrl: the one where the stroke began, for flat ground at one height)
			if (!sculpt_plane_set || !p_ctrl) {
				if (!p_ctrl || !sculpt_plane_set) {
					sculpt_plane_c = center;
					sculpt_plane_n = dirv;
					sculpt_plane_set = p_ctrl;
				}
			}
			const Vector3 pc = p_ctrl ? sculpt_plane_c : center, pn = p_ctrl ? sculpt_plane_n : dirv;
			for (size_t i = 0; i < vs.size(); i++) {
				out[i] -= pn * (pn.dot(ps[i] - pc) * MIN(1.0f, strength * ws[i] * 0.5f));
			}
			break;
		}
		case ST_NOISE: {
			const float freq = 2.5f / r;
			for (size_t i = 0; i < vs.size(); i++) {
				out[i] += dirv * (value_noise(ps[i] * freq) * sign * strength * r * 0.05f * ws[i]);
			}
			break;
		}
		case ST_TERRACE: {
			const float step = MAX(1e-4f, dock->sculpt_step_field ? (float)dock->sculpt_step_field->get_value() : 0.25f);
			for (size_t i = 0; i < vs.size(); i++) {
				const float h = dirv.dot(ps[i]);
				const float target = std::round(h / step) * step;
				out[i] += dirv * ((target - h) * MIN(1.0f, strength * ws[i]));
			}
			break;
		}
		case ST_PINCH:
			for (size_t i = 0; i < vs.size(); i++) {
				Vector3 t = hit - ps[i];
				t -= area_n * area_n.dot(t);
				out[i] += t * (sign * MIN(1.0f, strength * ws[i] * 0.2f));
			}
			break;
		case ST_INFLATE:
			for (size_t i = 0; i < vs.size(); i++) {
				out[i] += vnormal(vs[i]) * (sign * strength * r * 0.05f * ws[i]);
			}
			break;
		case ST_CREASE:
			// pinched toward the middle and pushed in (Ctrl: out, a sharp ridge)
			for (size_t i = 0; i < vs.size(); i++) {
				Vector3 t = hit - ps[i];
				t -= area_n * area_n.dot(t);
				out[i] += t * MIN(1.0f, strength * ws[i] * 0.15f) - area_n * (sign * strength * r * 0.04f * ws[i]);
			}
			break;
		default:
			return;
	}
	for (size_t i = 0; i < vs.size(); i++) {
		m.pos[vs[i]] = inv.xform(out[i]);
	}
	n->commit_positions();
}

// ------------------------------------------------------------------ input

int32_t HewnEditorPlugin::_sculpt_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	Ref<InputEventKey> k = p_event;
	if (k.is_valid()) {
		if (!k->is_pressed() || k->is_echo()) {
			return k->is_ctrl_pressed() || k->is_meta_pressed() ? (int32_t)AFTER_GUI_INPUT_PASS : (int32_t)AFTER_GUI_INPUT_STOP;
		}
		const Key code = key_code(k);
		const bool ctrl = k->is_ctrl_pressed() || k->is_meta_pressed();
		if (code == KEY_TAB && !ctrl && !k->is_alt_pressed() && !k->is_shift_pressed()) {
			exit_sculpt();
			if (dock) {
				dock->refresh();
			}
			return AFTER_GUI_INPUT_STOP;
		}
		if ((code == KEY_BRACKETLEFT || code == KEY_BRACKETRIGHT) && dock && dock->sculpt_radius_field) {
			HewnDragField *f = k->is_shift_pressed() ? dock->sculpt_strength_field : dock->sculpt_radius_field;
			const double v = f->get_value();
			f->set_value(code == KEY_BRACKETRIGHT ? (k->is_shift_pressed() ? v + 0.05 : v * 1.15) : (k->is_shift_pressed() ? v - 0.05 : v / 1.15));
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		// (Ctrl / Alt keys and the keypad: the editor's: undo, the views...)
		if (ctrl || k->is_alt_pressed() || (code >= KEY_KP_MULTIPLY && code <= KEY_KP_9)) {
			return AFTER_GUI_INPUT_PASS;
		}
		return AFTER_GUI_INPUT_STOP; // (the edit mode's keys do nothing here)
	}
	if (mb.is_valid()) {
		const MouseButton bi = mb->get_button_index();
		if ((bi == MOUSE_BUTTON_WHEEL_UP || bi == MOUSE_BUTTON_WHEEL_DOWN) && mb->is_pressed() && dock && dock->sculpt_radius_field &&
				(mb->is_ctrl_pressed() || mb->is_meta_pressed() || mb->is_shift_pressed())) {
			// Ctrl+wheel: the radius, Shift+wheel: the strength
			const bool upw = bi == MOUSE_BUTTON_WHEEL_UP;
			if (mb->is_shift_pressed()) {
				dock->sculpt_strength_field->set_value(dock->sculpt_strength_field->get_value() + (upw ? 0.05 : -0.05));
			} else {
				dock->sculpt_radius_field->set_value(dock->sculpt_radius_field->get_value() * (upw ? 1.1 : 1.0 / 1.1));
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (bi != MOUSE_BUTTON_LEFT) {
			return AFTER_GUI_INPUT_PASS; // (the camera)
		}
		if (mb->is_pressed()) {
			mouse_pos = mb->get_position();
			Vector3 hit, hn;
			if (!_sculpt_hit(p_cam, mouse_pos, hit, hn)) {
				return AFTER_GUI_INPUT_STOP;
			}
			begin_edit("Sculpt");
			sculpting = true;
			sculpt_pressure = 1.0f;
			_sculpt_begin(p_cam);
			sculpt_last = mouse_pos;
			_sculpt_dab(p_cam, mouse_pos, true, mb->is_ctrl_pressed() || mb->is_meta_pressed(), mb->is_shift_pressed());
		} else if (sculpting) {
			sculpting = false;
			sculpt_grab_v.clear();
			end_edit(); // (the whole mesh made again, one undo step)
		}
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mm.is_valid()) {
		mouse_pos = mm->get_position();
		if (sculpting) {
			const float pr = mm->get_pressure();
			sculpt_pressure = pr > 0.0f ? pr : 1.0f; // (a mouse has none: full)
			const bool ctrl = mm->is_ctrl_pressed() || mm->is_meta_pressed(), shift = mm->is_shift_pressed();
			if (sculpt_tool == ST_GRAB && !shift) {
				_sculpt_dab(p_cam, mouse_pos, false, ctrl, shift);
			} else {
				// dabs along the way, a fraction of the radius apart
				const float spacing = MAX(2.0f, (float)dock->sculpt_radius_field->get_value() * 0.15f);
				const float dist = sculpt_last.distance_to(mouse_pos);
				const int steps = (int)(dist / spacing);
				for (int i = 1; i <= steps; i++) {
					_sculpt_dab(p_cam, sculpt_last.lerp(mouse_pos, (float)i / (float)MAX(steps, 1)), false, ctrl, shift);
				}
				if (steps > 0) {
					sculpt_last = mouse_pos;
				}
			}
		}
		update_overlays();
		return sculpting ? AFTER_GUI_INPUT_STOP : AFTER_GUI_INPUT_PASS;
	}
	return AFTER_GUI_INPUT_PASS;
}

// ------------------------------------------------------------------ drawing

void HewnEditorPlugin::_draw_sculpt(Control *p_overlay, Camera3D *p_cam) {
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const float r = dock && dock->sculpt_radius_field ? (float)dock->sculpt_radius_field->get_value() : 50.0f;
	Vector3 hit, hn;
	const bool on = _sculpt_hit(p_cam, mouse_pos, hit, hn);
	const Color c = on ? Color(1.0f, 1.0f, 1.0f, 0.85f) : Color(1.0f, 1.0f, 1.0f, 0.35f);
	p_overlay->draw_arc(mouse_pos, r, 0.0f, (float)Math::PI * 2.0f, 64, Color(0, 0, 0, 0.5f), 3.0f * es, true);
	p_overlay->draw_arc(mouse_pos, r, 0.0f, (float)Math::PI * 2.0f, 64, c, 1.5f * es, true);
	const float st = dock && dock->sculpt_strength_field ? (float)dock->sculpt_strength_field->get_value() : 0.5f;
	p_overlay->draw_arc(mouse_pos, r * CLAMP(st, 0.05f, 1.0f), 0.0f, (float)Math::PI * 2.0f, 48, c * Color(1, 1, 1, 0.5f), 1.0f * es, true);
	draw_header(p_overlay, p_overlay->get_theme_default_font(),
			vformat(T("Sculpt: %s  radius: %d px  strength: %.2f"), T(SCULPT_TOOL_NAMES[CLAMP(sculpt_tool, 0, (int)ST_COUNT - 1)]), (int)r, st) + "\n" +
					T("Ctrl: the other way, Shift: smooth, Ctrl+wheel or [ / ]: radius, Shift+wheel: strength"));
}

// ------------------------------------------------------------------ the header's row

void HewnDock::build_sculpt_bar() {
	Ref<Theme> et = EditorInterface::get_singleton()->get_editor_theme();
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const Color base = et->has_color("dark_color_1", "Editor") ? et->get_color("dark_color_1", "Editor") : Color(0.1f, 0.11f, 0.13f);
	auto block = [&]() {
		PanelContainer *p = memnew(PanelContainer);
		Ref<StyleBoxFlat> sb;
		sb.instantiate();
		sb->set_bg_color(base);
		sb->set_corner_radius_all(4);
		sb->set_content_margin(SIDE_LEFT, 2);
		sb->set_content_margin(SIDE_RIGHT, 2);
		p->add_theme_stylebox_override("panel", sb);
		HBoxContainer *h = memnew(HBoxContainer);
		h->add_theme_constant_override("separation", 2);
		p->add_child(h);
		paint_row->add_child(p);
		p->hide();
		sculpt_blocks.push_back(p);
		return h;
	};
	auto label = [&](HBoxContainer *p_block, const String &p_text) {
		Label *l = memnew(Label);
		l->set_text(p_text);
		l->add_theme_color_override("font_color", Color(0.75f, 0.75f, 0.75f));
		p_block->add_child(l);
	};
	auto field = [&](HBoxContainer *p_block, double p_min, double p_max, double p_step, double p_value, const String &p_tip) {
		HewnDragField *f = memnew(HewnDragField);
		f->setup(p_min, p_max, p_step, false, p_value);
		f->set_custom_minimum_size(Vector2(52 * es, 0));
		f->set_tooltip_text(p_tip);
		f->connect("value_changed", callable_mp(this, &HewnDock::_on_sculpt_changed).unbind(1));
		p_block->add_child(f);
		return f;
	};
	HBoxContainer *br = block();
	label(br, T("Radius"));
	sculpt_radius_field = field(br, 2, 2000, 1, 50, "Brush radius (pixels)");
	sculpt_radius_field->set_name("SculptRadius");
	label(br, T("Strength"));
	sculpt_strength_field = field(br, 0.01, 1, 0.01, 0.5, "");
	sculpt_strength_field->set_name("SculptStrength");
	HBoxContainer *dr = block();
	sculpt_direction = memnew(OptionButton);
	sculpt_direction->set_name("SculptDirection");
	sculpt_direction->add_item("Up");
	sculpt_direction->add_item("Along the Surface");
	sculpt_direction->set_flat(true);
	sculpt_direction->set_tooltip_text("Where the brushes move the surface: straight up (ground: slopes never fold over) or out of the surface under the brush");
	sculpt_direction->connect("item_selected", callable_mp(this, &HewnDock::_on_sculpt_changed).unbind(1));
	dr->add_child(sculpt_direction);
	// the falloff and the pen
	HBoxContainer *fb = block();
	PopupPanel *pp = memnew(PopupPanel);
	VBoxContainer *fv = memnew(VBoxContainer);
	pp->add_child(fv);
	paint_row->add_child(pp);
	Button *fbtn = memnew(Button);
	fbtn->set_text(T("Falloff") + String(" ") + String::chr(0x25BE));
	fbtn->set_flat(true);
	fbtn->connect("pressed", callable_mp(this, &HewnDock::_popup_under).bind(pp, fbtn));
	fb->add_child(fbtn);
	Label *fl = memnew(Label);
	fl->set_text("Falloff");
	fv->add_child(fl);
	sculpt_falloff_type = memnew(OptionButton);
	sculpt_falloff_type->set_name("SculptFalloff");
	for (const char *it : { "Smooth", "Sphere", "Root", "Sharp", "Linear", "Constant", "Custom Curve" }) {
		sculpt_falloff_type->add_item(it);
	}
	sculpt_falloff_type->connect("item_selected", callable_mp(this, &HewnDock::_on_sculpt_changed).unbind(1));
	fv->add_child(sculpt_falloff_type);
	sculpt_curve_picker = memnew(EditorResourcePicker);
	sculpt_curve_picker->set_base_type("Curve");
	sculpt_curve_picker->set_custom_minimum_size(Vector2(180 * es, 0));
	sculpt_curve_picker->set_tooltip_text("Custom falloff: x the distance from the middle (0) to the edge (1), y the strength");
	sculpt_curve_picker->connect("resource_changed", callable_mp(this, &HewnDock::_on_sculpt_changed).unbind(1));
	fv->add_child(sculpt_curve_picker);
	sculpt_pressure_strength = memnew(CheckBox);
	sculpt_pressure_strength->set_text("Pen pressure: strength");
	sculpt_pressure_strength->set_pressed(true);
	fv->add_child(sculpt_pressure_strength);
	sculpt_pressure_radius = memnew(CheckBox);
	sculpt_pressure_radius->set_text("Pen pressure: radius");
	fv->add_child(sculpt_pressure_radius);
	// the terraces' step
	HBoxContainer *tb = block();
	sculpt_step_block = Object::cast_to<Control>(tb->get_parent());
	label(tb, T("Step"));
	sculpt_step_field = field(tb, 0.001, 1000, 0.01, 0.25, "The height of each terrace");
	sculpt_step_field->set_name("SculptStep");
	_on_sculpt_changed();
}

void HewnDock::_on_sculpt_changed() {
	sculpt_curve = sculpt_curve_picker ? Ref<Curve>(sculpt_curve_picker->get_edited_resource()) : Ref<Curve>();
	if (sculpt_curve_picker && sculpt_falloff_type) {
		sculpt_curve_picker->set_visible(sculpt_falloff_type->get_selected() == 6);
		if (sculpt_falloff_type->get_selected() == 6 && sculpt_curve.is_null()) {
			// a curve to start from: full in the middle down to nothing at the edge
			Ref<Curve> c;
			c.instantiate();
			c->add_point(Vector2(0, 1));
			c->add_point(Vector2(1, 0));
			sculpt_curve_picker->set_edited_resource(c);
			sculpt_curve = c;
		}
	}
	sync_sculpt_bar();
	if (plugin) {
		plugin->update_overlays();
	}
}

void HewnDock::sync_sculpt_bar() {
	if (sculpt_step_block && plugin) {
		sculpt_step_block->set_visible(plugin->sculpt && plugin->sculpt_tool == HewnEditorPlugin::ST_TERRACE && paint_bar_shown && !toolbar_folded);
	}
}
