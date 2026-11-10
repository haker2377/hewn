// Small helpers shared by the editor sources.
#pragma once

#include <godot_cpp/classes/geometry2d.hpp>

#include "../hewn_mesh.h"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/editor_dock.hpp>
#include <godot_cpp/classes/editor_inspector.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event_key.hpp>

#include <unordered_set>

namespace hw_editor {

using namespace godot;

// A resource in the Inspector, its dock's tab brought to the front.
inline void edit_in_inspector(const Ref<Resource> &p_res) {
	EditorInterface::get_singleton()->edit_resource(p_res);
	for (Node *x = EditorInterface::get_singleton()->get_inspector(); x; x = x->get_parent()) {
		if (EditorDock *d = Object::cast_to<EditorDock>(x)) {
			d->make_visible();
			break;
		}
	}
}

const Color AXIS_COLORS[3] = { Color(0.96f, 0.3f, 0.32f), Color(0.45f, 0.86f, 0.25f), Color(0.3f, 0.55f, 1.0f) };
const Color HANDLE_HOVER(1.0f, 0.9f, 0.3f);
const Color SNAP_COLOR(0.3f, 1.0f, 0.9f);
const Color TOOL_COLOR(1.0f, 0.6f, 0.1f);
const Vector3 AXES[3] = { Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1) };

// What a snapped point lies on: its mark is a square on a vertex, an hourglass on an edge, a circle on
// a face and a cross on the grid.
enum SnapKind {
	SNAP_KIND_GRID,
	SNAP_KIND_VERTEX,
	SNAP_KIND_EDGE,
	SNAP_KIND_FACE,
};

inline void draw_snap_symbol(CanvasItem *p_ci, const Vector2 &c, int p_kind, const Color &p_col, float r = 6.0f) {
	const Color shadow(0, 0, 0, 0.8f);
	for (int pass = 0; pass < 2; pass++) {
		const Color col = pass == 0 ? shadow : p_col;
		const float w = pass == 0 ? 3.5f : 1.8f;
		switch (p_kind) {
			case SNAP_KIND_VERTEX:
				p_ci->draw_rect(Rect2(c - Vector2(r, r), Vector2(r, r) * 2.0f), col, false, w);
				break;
			case SNAP_KIND_EDGE: {
				// two triangles meeting in the middle, in a square as wide as it is high
				PackedVector2Array hg;
				hg.push_back(c + Vector2(-r, -r));
				hg.push_back(c + Vector2(r, -r));
				hg.push_back(c + Vector2(-r, r));
				hg.push_back(c + Vector2(r, r));
				hg.push_back(c + Vector2(-r, -r));
				p_ci->draw_polyline(hg, col, w, true);
			} break;
			case SNAP_KIND_FACE:
				p_ci->draw_arc(c, r, 0.0f, (float)Math::PI * 2.0f, 24, col, w, true);
				break;
			default:
				p_ci->draw_line(c - Vector2(r, 0), c + Vector2(r, 0), col, w, true);
				p_ci->draw_line(c - Vector2(0, r), c + Vector2(0, r), col, w, true);
				break;
		}
	}
}

// A filled polygon on the overlay, skipped when its points came out of a projection near the camera's plane (huge
// / not numbers) or it has no area: the renderer would refuse it with an error.
inline void draw_poly_safe(CanvasItem *p_ci, const PackedVector2Array &p_pts, const Color &p_col) {
	if (p_pts.size() < 3) {
		return;
	}
	float area = 0.0f;
	for (int64_t i = 0; i < p_pts.size(); i++) {
		const Vector2 a = p_pts[i], b = p_pts[(i + 1) % p_pts.size()];
		if (!a.is_finite() || std::fabs(a.x) > 1e6f || std::fabs(a.y) > 1e6f) {
			return;
		}
		area += a.cross(b);
	}
	if (std::fabs(area) < 1e-4f) {
		return;
	}
	// (crossing itself - a plane seen across the camera's side - cannot be filled: its outline's hull instead)
	if (p_pts.size() > 3 && godot::Geometry2D::get_singleton()->triangulate_polygon(p_pts).is_empty()) {
		const PackedVector2Array hull = godot::Geometry2D::get_singleton()->convex_hull(p_pts);
		if (hull.size() >= 4 && !godot::Geometry2D::get_singleton()->triangulate_polygon(hull).is_empty()) {
			p_ci->draw_colored_polygon(hull, p_col);
		}
		return;
	}
	p_ci->draw_colored_polygon(p_pts, p_col);
}

inline float seg_dist(const Vector2 &p, const Vector2 &a, const Vector2 &b, float *r_t = nullptr) {
	const Vector2 ab = b - a;
	const float l2 = ab.length_squared();
	float t = l2 > 1e-12f ? (p - a).dot(ab) / l2 : 0.0f;
	t = CLAMP(t, 0.0f, 1.0f);
	if (r_t) {
		*r_t = t;
	}
	return p.distance_to(a + ab * t);
}

inline bool ray_tri(const Vector3 &o, const Vector3 &d, const Vector3 &a, const Vector3 &b, const Vector3 &c, float &t) {
	const Vector3 e1 = b - a, e2 = c - a;
	const Vector3 p = d.cross(e2);
	const float det = e1.dot(p);
	if (std::fabs(det) < 1e-12f) {
		return false;
	}
	const float inv = 1.0f / det;
	const Vector3 s = o - a;
	const float u = s.dot(p) * inv;
	if (u < -1e-6f || u > 1.0f + 1e-6f) {
		return false;
	}
	const Vector3 q = s.cross(e1);
	const float v = d.dot(q) * inv;
	if (v < -1e-6f || u + v > 1.0f + 1e-6f) {
		return false;
	}
	t = e2.dot(q) * inv;
	return t > 0.0f;
}

// Nearest face of `n` hit by a world ray (p_dir normalized). r_t is the world distance.
// p_cull_back: faces pointing away from the ray are not hit (like the culled back faces you see through).
inline int raycast_node(const HewnMesh3D *n, const Vector3 &p_from, const Vector3 &p_dir, float &r_t, const std::unordered_set<int> *p_skip, bool p_cull_back = false,
		bool p_shown = false) {
	const Transform3D g = n->get_global_transform();
	const Transform3D inv = g.affine_inverse();
	const Vector3 o = inv.xform(p_from);
	const Vector3 d = inv.basis.xform(p_dir);
	// (p_shown: what is shown, the modifiers / CSG brushes applied; face indices are of that)
	const hewn::Mesh &m = p_shown ? n->get_shown_data() : n->get_data();
	const hewn::BuildOut &b = p_shown ? n->get_shown_build() : n->get_build();
	int best = -1;
	float best_t = 1e30f;
	for (int f = 0; f < (int)m.faces.size() && f < (int)b.face_tris.size(); f++) {
		if (p_skip && p_skip->count(f)) {
			continue;
		}
		const std::vector<int> &tr = b.face_tris[f];
		const hewn::Face &F = m.faces[f];
		if (p_cull_back && hewn::face_normal(m, F).dot(d) > 0.0f) {
			continue;
		}
		for (size_t k = 0; k + 2 < tr.size(); k += 3) {
			float t;
			if (ray_tri(o, d, m.pos[F.v[tr[k]]], m.pos[F.v[tr[k + 1]]], m.pos[F.v[tr[k + 2]]], t) && t < best_t) {
				best_t = t;
				best = f;
			}
		}
	}
	if (best >= 0) {
		// local parameter -> world distance
		r_t = g.xform(o + d * best_t).distance_to(p_from);
	}
	return best;
}

// Vertices of faces that are not hidden (H), and of loose edges.
inline std::vector<char> visible_verts(const hewn::Mesh &m) {
	std::vector<char> used(m.pos.size(), 0);
	for (const hewn::Face &F : m.faces) {
		if (!F.hidden) {
			for (int v : F.v) {
				used[v] = 1;
			}
		}
	}
	for (const std::pair<int, int> &wr : m.wires) {
		used[wr.first] = used[wr.second] = 1;
	}
	for (int v : m.points) {
		if (v >= 0 && v < (int)used.size()) {
			used[v] = 1;
		}
	}
	return used;
}

// An edge with a face that is not hidden (or a loose one).
inline bool edge_visible(const hewn::Mesh &m, const hewn::Topology &t, const hewn::Edge &e) {
	const int id = t.find_edge(e.a, e.b);
	if (id < 0) {
		return true;
	}
	for (int f : t.edge_faces[id]) {
		if (!m.faces[f].hidden) {
			return true;
		}
	}
	return false;
}

// Parameter along the line (origin, dir) of the point closest to the ray (o, d).
inline float line_ray_param(const Vector3 &p_origin, const Vector3 &p_dir, const Vector3 &o, const Vector3 &d) {
	const Vector3 &u = p_dir;
	const Vector3 w0 = p_origin - o;
	const float a = u.dot(u), b = u.dot(d), c = d.dot(d), dd = u.dot(w0), e = d.dot(w0);
	const float den = a * c - b * b;
	if (std::fabs(den) < 1e-8f) {
		return 0.0f;
	}
	return (b * e - c * dd) / den;
}

// Physical key (layout independent), falling back to the logical key.
inline Key key_code(const Ref<InputEventKey> &k) {
	return k->get_physical_keycode() != KEY_NONE ? k->get_physical_keycode() : k->get_keycode();
}

// Ctrl (Cmd) held after this event; the press / release of the key itself counts too (on some
// platforms its own event does not carry the modifier yet / still).
inline bool ctrl_after(const Ref<InputEventKey> &k) {
	const Key c = key_code(k);
	if (c == KEY_CTRL || c == KEY_META) {
		return k->is_pressed();
	}
	return k->is_ctrl_pressed() || k->is_meta_pressed();
}

// UI language (hw_i18n.cpp): T() translates one of our strings, localize() a built UI tree.
void i18n_init();
void i18n_finish();
String T(const String &p_text);
String T(const char *p_text);
void localize(Node *p_node);

// The revert arrow (hw_revert.cpp): `with_revert` wraps a new field (add the result where the field goes),
// `add_revert` an added one in place, `add_reverts_under` every field of a panel with its value now as default.
Control *with_revert(Control *p_field, const Variant &p_default);
void add_revert(Control *p_field, const Variant &p_default);
void add_reverts_under(Node *p_root, const PackedStringArray &p_skip = PackedStringArray());

// Left margin of our panels in the viewport: clear of Godot's zoom bar at the left edge.
inline float left_gutter() {
	return 34.0f * EditorInterface::get_singleton()->get_editor_scale();
}

// Operation header at the top center of the viewport, in the editor's font and size.
// How much lower the header's text goes: the second row of the toolbar (Vertex Paint, Sculpt, the cutter) is
// over the viewport's top then (set by the plugin as it places that row).
inline float &header_extra() {
	static float v = 0.0f;
	return v;
}

inline void draw_header(Control *p_overlay, const Ref<Font> &p_font, const String &p_text) {
	if (p_font.is_null() || p_text.is_empty()) {
		return;
	}
	const int fs = p_overlay->get_theme_default_font_size();
	const float asc = p_font->get_ascent(fs);
	const float lh = p_font->get_height(fs) + 8.0f;
	// (below the row of the view menu and the Hewn toolbar; "\n": more lines, each centered)
	const PackedStringArray lines = p_text.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		const Vector2 sz = p_font->get_string_size(lines[i], HORIZONTAL_ALIGNMENT_LEFT, -1, fs);
		const Vector2 pos(std::round(MAX(8.0f, (p_overlay->get_size().x - sz.x) * 0.5f)), std::round(48.0f + header_extra() + asc + lh * i));
		p_overlay->draw_rect(Rect2(pos - Vector2(8, asc + 4), sz + Vector2(16, 8)), Color(0, 0, 0, 0.5f));
		p_overlay->draw_string(p_font, pos, lines[i], HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color(1, 1, 1, 0.95f));
	}
}

} // namespace hw_editor
