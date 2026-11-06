// Face Cutter: right click on selected faces, Face Cutter. First a border edge of
// the selection is picked (a yellow triangle on it points into the faces), then cut lines are clicked
// across the faces from that edge: the dashed line under the mouse snaps to the marks on the edge (Ctrl +
// wheel: how many), Z / C turn it left (C less), X / V right (V less), D: 45 degrees (again: the other
// 45). Ctrl+Z takes the last line back, Q cuts the faces along all of them, Esc / RMB: cancel.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>

#include <unordered_set>

using namespace godot;
using namespace hw_editor;

namespace {
const Color FC_YELLOW(1.0f, 0.85f, 0.1f, 1.0f);
} // namespace

void HewnEditorPlugin::start_face_cutter() {
	HewnMesh3D *n = get_edited();
	if (!n || n->selection.faces.empty()) {
		return;
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	if (mode != MODE_FACE) {
		set_mode(MODE_FACE);
	}
	const hewn::Mesh &m = n->get_data();
	// the border of the selection: edges whose other side is not selected
	std::unordered_set<uint64_t> half;
	for (int f : n->selection.faces) {
		const hewn::Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			half.insert(((uint64_t)(uint32_t)F.v[i] << 32) | (uint32_t)F.v[(i + 1) % F.size()]);
		}
	}
	fc_border.clear();
	for (int f : n->selection.faces) {
		const hewn::Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			if (!half.count(((uint64_t)(uint32_t)F.v[(i + 1) % F.size()] << 32) | (uint32_t)F.v[i])) {
				fc_border.push_back({ f, i });
			}
		}
	}
	fc_phase = 0;
	fc_hover = -1;
	fc_edge = -1;
	fc_lines.clear();
	fc_angle = 0.0f;
	fc_d45 = 0;
	fc_cur_valid = false;
	modal = MODAL_FACECUT;
	update_overlays();
}

// The base edge (local): its ends, the face normal and the direction into the faces.
bool HewnEditorPlugin::_fc_edge_frame(int p_border, Vector3 &r_a, Vector3 &r_b, Vector3 &r_n, Vector3 &r_in) const {
	HewnMesh3D *n = get_edited();
	if (!n || p_border < 0 || p_border >= (int)fc_border.size()) {
		return false;
	}
	const hewn::Mesh &m = n->get_data();
	const hewn::Face &F = m.faces[fc_border[p_border].first];
	const int c = fc_border[p_border].second;
	r_a = m.pos[F.v[c]];
	r_b = m.pos[F.v[(c + 1) % F.size()]];
	r_n = hewn::face_normal(m, F);
	r_in = r_n.cross(r_b - r_a).normalized(); // (left of the edge in the face's winding: inside)
	return (r_b - r_a).length() > 1e-6f && r_in.length() > 0.5f;
}

void HewnEditorPlugin::_fc_update_line(Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	Vector3 a, b, nrm, in;
	fc_cur_valid = false;
	if (!n || !_fc_edge_frame(fc_edge, a, b, nrm, in)) {
		return;
	}
	const Transform3D g = n->get_global_transform();
	const Transform3D inv = g.affine_inverse();
	const Vector3 nw = g.basis.inverse().transposed().xform(nrm).normalized();
	Vector3 hit;
	if (!_plane_hit(p_cam, mouse_pos, g.xform(a), nw, hit)) {
		return;
	}
	const Vector3 h = inv.xform(hit);
	const Vector3 e = b - a;
	const Vector3 d = Basis(nrm, Math::deg_to_rad(fc_angle)).xform(in).normalized();
	// where the line through the mouse meets the edge; onto a mark when it is near on screen
	const float den = e.cross(d).dot(nrm);
	float t = std::fabs(den) > 1e-8f ? (h - a).cross(d).dot(nrm) / den : (h - a).dot(e) / e.length_squared();
	if (fc_ticks > 0) {
		float best = 12.0f;
		const Vector2 sp = p_cam->unproject_position(g.xform(a + e * t));
		for (int i = 1; i <= fc_ticks; i++) {
			const float ti = (float)i / (fc_ticks + 1);
			const Vector3 w = g.xform(a + e * ti);
			if (p_cam->is_position_behind(w)) {
				continue;
			}
			const float dist = p_cam->unproject_position(w).distance_to(sp);
			if (dist < best) {
				best = dist;
				t = ti;
			}
		}
	}
	fc_cur_point = a + e * t;
	fc_cur_dir = d;
	fc_cur_valid = true;
}

void HewnEditorPlugin::_fc_apply() {
	HewnMesh3D *n = get_edited();
	Vector3 a, b, nrm, in;
	if (!n || fc_lines.empty() || !_fc_edge_frame(fc_edge, a, b, nrm, in)) {
		cancel_tool();
		return;
	}
	const std::vector<std::pair<Vector3, Vector3>> lines = fc_lines;
	modal = MODAL_NONE;
	begin_edit("Face Cutter");
	hewn::Mesh &m = n->get_data();
	for (hewn::Face &F : m.faces) {
		F.tag = 0;
	}
	for (int f : n->selection.faces) {
		m.faces[f].tag = 7;
	}
	for (const std::pair<Vector3, Vector3> &l : lines) {
		std::vector<int> faces;
		for (int f = 0; f < (int)m.faces.size(); f++) {
			if (m.faces[f].tag == 7) {
				faces.push_back(f);
			}
		}
		const Vector3 cut_n = nrm.cross(l.second).normalized(); // (the plane through the line, across the face)
		hewn::ops::bisect(m, faces, l.first, cut_n, false, false, false);
	}
	n->selection.clear();
	for (int f = 0; f < (int)m.faces.size(); f++) {
		if (m.faces[f].tag == 7) {
			n->selection.faces.push_back(f);
		}
		m.faces[f].tag = 0;
	}
	n->selection.verts = hewn::sel::verts_of_faces(m, n->selection.faces);
	n->commit(true);
	end_edit();
	fc_lines.clear();
	selection_changed();
	update_overlays();
}

int32_t HewnEditorPlugin::_facecut_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		cancel_tool();
		return AFTER_GUI_INPUT_PASS;
	}
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	const Transform3D g = n->get_global_transform();
	if (k.is_valid() && k->is_pressed() && !k->is_echo()) {
		const Key code = key_code(k);
		const bool ctrl = k->is_ctrl_pressed() || k->is_meta_pressed();
		if (code == KEY_ESCAPE) {
			cancel_tool();
			return AFTER_GUI_INPUT_STOP;
		}
		if (fc_phase == 1) {
			if (code == KEY_Z && ctrl) {
				if (!fc_lines.empty()) {
					fc_lines.pop_back(); // (only the tool's lines, before they cut)
				}
			} else if ((code == KEY_Q || code == KEY_ENTER || code == KEY_KP_ENTER) && !ctrl) {
				_fc_apply();
				return AFTER_GUI_INPUT_STOP;
			} else if (!ctrl && (code == KEY_Z || code == KEY_C || code == KEY_X || code == KEY_V)) {
				fc_angle += code == KEY_Z ? 15.0f : code == KEY_C ? 5.0f : code == KEY_X ? -15.0f : -5.0f;
				fc_angle = Math::fposmod(fc_angle + 180.0f, 360.0f) - 180.0f;
				fc_d45 = 0;
			} else if (!ctrl && code == KEY_D) {
				fc_d45 = fc_d45 == 1 ? 2 : 1;
				fc_angle = fc_d45 == 1 ? 45.0f : -45.0f; // (45 degrees to the edge, then the other way)
			} else {
				return AFTER_GUI_INPUT_STOP;
			}
			_fc_update_line(p_cam);
			update_overlays();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->is_pressed()) {
		const MouseButton bi = mb->get_button_index();
		if (bi == MOUSE_BUTTON_RIGHT) {
			cancel_tool();
			return AFTER_GUI_INPUT_STOP;
		}
		if ((bi == MOUSE_BUTTON_WHEEL_UP || bi == MOUSE_BUTTON_WHEEL_DOWN) && (mb->is_ctrl_pressed() || mb->is_meta_pressed())) {
			fc_ticks = CLAMP(fc_ticks + (bi == MOUSE_BUTTON_WHEEL_UP ? 1 : -1), 0, 64);
			_fc_update_line(p_cam);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (bi == MOUSE_BUTTON_LEFT) {
			if (fc_phase == 0 && fc_hover >= 0) {
				fc_edge = fc_hover;
				fc_phase = 1;
				_fc_update_line(p_cam);
			} else if (fc_phase == 1 && fc_cur_valid) {
				fc_lines.push_back({ fc_cur_point, fc_cur_dir });
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (mm.is_valid()) {
		if (fc_phase == 0) {
			// the border edge nearest to the mouse
			fc_hover = -1;
			float best = 30.0f;
			const hewn::Mesh &m = n->get_data();
			for (int i = 0; i < (int)fc_border.size(); i++) {
				const hewn::Face &F = m.faces[fc_border[i].first];
				const int c = fc_border[i].second;
				const Vector3 a = g.xform(m.pos[F.v[c]]), b = g.xform(m.pos[F.v[(c + 1) % F.size()]]);
				if (p_cam->is_position_behind(a) || p_cam->is_position_behind(b)) {
					continue;
				}
				const float d = seg_dist(mouse_pos, p_cam->unproject_position(a), p_cam->unproject_position(b));
				if (d < best) {
					best = d;
					fc_hover = i;
				}
			}
		} else {
			_fc_update_line(p_cam);
		}
		update_overlays();
		return AFTER_GUI_INPUT_PASS; // (the view can still turn)
	}
	return AFTER_GUI_INPUT_PASS;
}

void HewnEditorPlugin::_draw_facecut(Control *p_overlay, Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	Ref<Font> font = p_overlay->get_theme_default_font();
	if (!n) {
		return;
	}
	const Transform3D g = n->get_global_transform();
	auto S = [&](const Vector3 &local, Vector2 &r) {
		const Vector3 w = g.xform(local);
		if (p_cam->is_position_behind(w)) {
			return false;
		}
		r = p_cam->unproject_position(w);
		return true;
	};
	// how far a line reaches: across the selection
	const hewn::Mesh &m = n->get_data();
	float reach = 0.0f;
	{
		AABB box;
		bool first = true;
		for (int f : n->selection.faces) {
			for (int v : m.faces[f].v) {
				if (first) {
					box = AABB(m.pos[v], Vector3());
					first = false;
				} else {
					box.expand_to(m.pos[v]);
				}
			}
		}
		reach = MAX(0.01f, box.get_longest_axis_size() * 1.5f);
	}
	Vector3 a, b, nrm, in;
	const int edge = fc_phase == 0 ? fc_hover : fc_edge;
	if (_fc_edge_frame(edge, a, b, nrm, in)) {
		Vector2 sa, sb;
		if (S(a, sa) && S(b, sb)) {
			if (fc_phase == 0) {
				// the triangle: two corners on the edge, the third into the faces
				const Vector3 e = b - a;
				Vector2 p0, p1, p2;
				if (S(a + e * 0.25f, p0) && S(a + e * 0.75f, p1) && S(a + e * 0.5f + in * (e.length() * 0.3f), p2)) {
					PackedVector2Array tri;
					tri.push_back(p0);
					tri.push_back(p1);
					tri.push_back(p2);
					p_overlay->draw_colored_polygon(tri, FC_YELLOW * Color(1, 1, 1, 0.55f));
					tri.push_back(p0);
					p_overlay->draw_polyline(tri, FC_YELLOW, 2.0f, true);
				}
			} else {
				// the edge and its marks
				p_overlay->draw_line(sa, sb, FC_YELLOW, 3.0f, true);
				const Vector2 perp = (sb - sa).orthogonal().normalized() * 6.0f;
				for (int i = 1; i <= fc_ticks; i++) {
					const Vector2 q = sa.lerp(sb, (float)i / (fc_ticks + 1));
					p_overlay->draw_line(q - perp, q + perp, FC_YELLOW, 2.0f, true);
				}
			}
		}
	}
	auto line = [&](const Vector3 &p, const Vector3 &d, bool dashed) {
		Vector2 s0, s1;
		if (S(p, s0) && S(p + d * reach, s1)) {
			if (dashed) {
				p_overlay->draw_dashed_line(s0, s1, FC_YELLOW, 2.0f, 8.0f, true);
			} else {
				p_overlay->draw_line(s0, s1, FC_YELLOW, 2.0f, true);
			}
			p_overlay->draw_circle(s0, 3.0f, FC_YELLOW);
		}
	};
	for (const std::pair<Vector3, Vector3> &l : fc_lines) {
		line(l.first, l.second, false);
	}
	if (fc_phase == 1 && fc_cur_valid) {
		line(fc_cur_point, fc_cur_dir, true);
	}
	String text;
	if (fc_phase == 0) {
		text = T("Face Cutter: click the edge to cut from (the triangle points into the faces)   |   Esc / RMB: cancel");
	} else {
		text = vformat(T("Face Cutter: click: a line (%d), angle %.0f deg, marks %d   |   Z / C: turn left, X / V: right, D: 45 deg, Ctrl+wheel: marks, Ctrl+Z: back, Q: cut, Esc: cancel"),
				(int)fc_lines.size(), 90.0f - fc_angle, fc_ticks);
	}
	draw_header(p_overlay, font, text);
}
