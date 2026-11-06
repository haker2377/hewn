// Viewport overlays drawn by Hewn in edit mode: face center dots,
// normals, indices and measurements of the selection.
#include "hw_editor.h"

#include <godot_cpp/classes/geometry2d.hpp>
#include "hw_editor_util.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/font.hpp>

using namespace godot;
using namespace hw_editor;

namespace {

void text_at(Control *p_overlay, const Ref<Font> &p_font, const Vector2 &p_pos, const String &p_text, const Color &p_color) {
	const int fs = MAX(8, p_overlay->get_theme_default_font_size() - 2);
	const Vector2 sz = p_font->get_string_size(p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs);
	const Vector2 at = p_pos - Vector2(sz.x * 0.5f, -sz.y * 0.25f);
	p_overlay->draw_string(p_font, at + Vector2(1, 1), p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color(0, 0, 0, 0.8f));
	p_overlay->draw_string(p_font, at, p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, p_color);
}

} // namespace

bool HewnEditorPlugin::face_dots_active() const {
	return mode != MODE_OBJECT && has_mode(MODE_FACE) && xray_on;
}

bool HewnEditorPlugin::show_face_fill() const {
	// (Shift+T: the texture is seen as it is)
	return modal != MODAL_UVT && modal != MODAL_TSNAP && (!dock || dock->overlay(dock->ov_face_fill));
}

void HewnEditorPlugin::_draw_overlays(Control *p_overlay, Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (!n || mode == MODE_OBJECT || !dock || !dock->overlays->is_pressed()) {
		return;
	}
	const hewn::Mesh &m = n->get_data();
	const hewn::Selection &s = n->selection;
	const Transform3D g = n->get_global_transform();
	const Basis nb = g.basis.inverse().transposed();
	Ref<Font> font = p_overlay->get_theme_default_font();
	auto P = [&](const Vector3 &local, Vector2 &r) {
		const Vector3 w = g.xform(local);
		if (p_cam->is_position_behind(w)) {
			return false;
		}
		r = p_cam->unproject_position(w);
		return true;
	};
	auto line3 = [&](const Vector3 &a_local, const Vector3 &dir_world, const Color &c) {
		const Vector3 a = g.xform(a_local), b = a + dir_world;
		if (!p_cam->is_position_behind(a) && !p_cam->is_position_behind(b)) {
			p_overlay->draw_line(p_cam->unproject_position(a), p_cam->unproject_position(b), c, 1.0f, true);
		}
	};
	std::vector<char> fsel(m.faces.size(), 0);
	for (int f : s.faces) {
		if (f >= 0 && f < (int)fsel.size()) {
			fsel[f] = 1;
		}
	}

	// the origins of the edited objects, orange dots
	for (HewnMesh3D *em : edited_meshes()) {
		const Vector3 wo = em->get_global_transform().origin;
		if (!p_cam->is_position_behind(wo)) {
			const Vector2 o = p_cam->unproject_position(wo);
			const float es = EditorInterface::get_singleton()->get_editor_scale();
			p_overlay->draw_circle(o, 4.0f * es, Color(0.05f, 0.05f, 0.05f, 0.9f));
			p_overlay->draw_circle(o, 3.0f * es, Color(1.0f, 0.55f, 0.1f, 1.0f));
		}
	}

	// Face center dots: in X-ray these are what you click in face mode; all of them in one draw
	// (made again only when the view / the mesh / the selection change)
	if (face_dots_active()) {
		const uint64_t key = _marks_view_key(p_cam, n) ^ 0x77ull;
		if (!_marks_cached(p_overlay, 0, key)) {
			std::vector<Mark> dots;
			dots.reserve(m.faces.size());
			for (int f = 0; f < (int)m.faces.size(); f++) {
				Vector2 c;
				if (!m.faces[f].hidden && P(hewn::face_center(m, m.faces[f]), c)) {
					Mark d;
					d.at = c;
					d.half = 3.0f;
					d.color = fsel[f] ? Color(1.0f, 0.55f, 0.1f) : Color(0, 0, 0, 0.9f);
					dots.push_back(d);
				}
			}
			_marks_set(p_overlay, 0, key, dots);
		}
	} else if (dock->overlay(dock->ov_face_centers)) {
		for (int f = 0; f < (int)m.faces.size(); f++) {
			if (!fsel[f] || m.faces[f].hidden) {
				continue;
			}
			Vector2 c;
			if (P(hewn::face_center(m, m.faces[f]), c)) {
				p_overlay->draw_circle(c, 2.0f, fsel[f] ? Color(1.0f, 0.55f, 0.1f) : Color(0, 0, 0, 0.9f), true, -1.0f, true);
			}
		}
	}

	// Normals.
	const float len = (float)dock->ov_normal_size->get_value();
	if (dock->overlay(dock->ov_fnormals)) {
		for (const hewn::Face &F : m.faces) {
			line3(hewn::face_center(m, F), nb.xform(hewn::face_normal(m, F)).normalized() * len, Color(0.3f, 0.9f, 1.0f));
		}
	}
	if (dock->overlay(dock->ov_vnormals)) {
		const std::vector<Vector3> vn = hewn::vertex_normals(m);
		std::vector<char> used(m.pos.size(), 0);
		for (const hewn::Face &F : m.faces) {
			for (int v : F.v) {
				used[v] = 1;
			}
		}
		for (int v = 0; v < (int)m.pos.size(); v++) {
			if (used[v] && vn[v].length() > 1e-9f) {
				line3(m.pos[v], nb.xform(vn[v]).normalized() * len, Color(0.4f, 0.5f, 1.0f));
			}
		}
	}
	if (dock->overlay(dock->ov_snormals)) {
		// per face corner: the shading normal (smooth groups share it, flat faces use the face normal)
		const std::vector<Vector3> vn = hewn::vertex_normals(m);
		for (const hewn::Face &F : m.faces) {
			const Vector3 fn = hewn::face_normal(m, F);
			for (int v : F.v) {
				const Vector3 nn = F.smooth != 0 && vn[v].length() > 1e-9f ? vn[v] : fn;
				const Vector3 p = m.pos[v].lerp(hewn::face_center(m, F), 0.08f); // pulled inside the face so the corners are distinguishable
				line3(p, nb.xform(nn).normalized() * len, Color(0.85f, 0.4f, 1.0f));
			}
		}
	}
	if (!font.is_valid()) {
		return;
	}
	// Indices of the selected faces.
	if (dock->overlay(dock->ov_indices)) {
		for (int f : s.faces) {
			Vector2 c;
			if (f < (int)m.faces.size() && P(hewn::face_center(m, m.faces[f]), c)) {
				text_at(p_overlay, font, c, itos(f), Color(1, 1, 1));
			}
		}
	}
	// Measurements of the selection (world units / degrees): only when one is shown (the mesh's topology is
	// not cheap on a big mesh)
	if (!dock->overlay(dock->ov_edge_len) && !dock->overlay(dock->ov_edge_angle) && !dock->overlay(dock->ov_face_area) && !dock->overlay(dock->ov_face_angles)) {
		return;
	}
	std::vector<hewn::Edge> edges = s.edges;
	if (mode == MODE_FACE) {
		edges = hewn::sel::edges_of_faces(m, s.faces);
	} else if (mode == MODE_VERTEX) {
		hewn::Topology t;
		t.build(m);
		edges = hewn::sel::edges_from_verts(m, t, s.verts);
	}
	if (dock->overlay(dock->ov_edge_len)) {
		for (const hewn::Edge &e : edges) {
			Vector2 c;
			if (P((m.pos[e.a] + m.pos[e.b]) * 0.5f, c)) {
				text_at(p_overlay, font, c, String::num(g.xform(m.pos[e.a]).distance_to(g.xform(m.pos[e.b])), 3), Color(1, 1, 1));
			}
		}
	}
	if (dock->overlay(dock->ov_edge_angle)) {
		hewn::Topology t;
		t.build(m);
		for (const hewn::Edge &e : edges) {
			const int id = t.find_edge(e.a, e.b);
			if (id < 0 || t.edge_faces[id].size() != 2) {
				continue;
			}
			const Vector3 n0 = nb.xform(hewn::face_normal(m, m.faces[t.edge_faces[id][0]])).normalized();
			const Vector3 n1 = nb.xform(hewn::face_normal(m, m.faces[t.edge_faces[id][1]])).normalized();
			Vector2 c;
			if (P((m.pos[e.a] + m.pos[e.b]) * 0.5f, c)) {
				text_at(p_overlay, font, c + Vector2(0, 14), String::num(Math::rad_to_deg(n0.angle_to(n1)), 1) + String::chr(0xB0), Color(0.6f, 1.0f, 0.6f));
			}
		}
	}
	std::vector<int> faces = s.faces;
	if (mode != MODE_FACE) {
		faces = hewn::sel::faces_from_verts(m, _selected_verts(), true);
	}
	if (dock->overlay(dock->ov_face_area)) {
		for (int f : faces) {
			std::vector<Vector3> pts;
			for (int v : m.faces[f].v) {
				pts.push_back(g.xform(m.pos[v]));
			}
			Vector2 c;
			if (P(hewn::face_center(m, m.faces[f]), c)) {
				text_at(p_overlay, font, c + Vector2(0, -14), String::num(hewn::newell(pts).length() * 0.5f, 3), Color(1, 0.9f, 0.5f));
			}
		}
	}
	if (dock->overlay(dock->ov_face_angles)) {
		for (int f : faces) {
			const hewn::Face &F = m.faces[f];
			const Vector3 fc = hewn::face_center(m, F);
			for (int i = 0; i < F.size(); i++) {
				const Vector3 p = g.xform(m.pos[F.v[i]]);
				const Vector3 a = g.xform(m.pos[F.v[(i + F.size() - 1) % F.size()]]) - p, b = g.xform(m.pos[F.v[(i + 1) % F.size()]]) - p;
				Vector2 c;
				if (P(m.pos[F.v[i]].lerp(fc, 0.25f), c)) {
					text_at(p_overlay, font, c, String::num(Math::rad_to_deg(a.angle_to(b)), 1) + String::chr(0xB0), Color(0.6f, 0.9f, 1.0f));
				}
			}
		}
	}
}
