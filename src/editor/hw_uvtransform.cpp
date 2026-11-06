// Shift+T: one tile of the face's texture is drawn on the face's
// plane with a grid of its repetitions; handles scale it (corners: the opposite corner stays, sides: one
// axis), move it (middle: free, arrows: along U / V) and turn it. With Godot's magnet on (Shift: off for
// the moment) the handles snap: to the vertices and edges of the selected faces when they
// come near them on the screen, the tile's sides onto edges parallel to them, square pixels, turning to
// the edge directions. Ctrl snaps to texture pixels. The mouse over another repetition moves the
// handles there. A click away from the handles applies it.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>

#include <set>

using namespace godot;
using namespace hw_editor;

namespace {

const float SNAP_PX = 12.0f;
const float HANDLE_PX = 9.0f;

Vector2 rot2(const Vector2 &p, float p_deg) {
	const float a = p_deg * (float)(Math::PI / 180.0);
	return Vector2(p.x * std::cos(a) - p.y * std::sin(a), p.x * std::sin(a) + p.y * std::cos(a));
}

float signf1(float x) {
	return x < 0.0f ? -1.0f : 1.0f;
}

} // namespace

Vector2 HewnEditorPlugin::_uvt_tile_of(const Vector2 &p_raw) const {
	return (uvt.scale * rot2(p_raw, uvt.rot) + uvt.offset) * uvt.ms;
}

Vector2 HewnEditorPlugin::_uvt_raw_of(const Vector2 &p_tile) const {
	const Vector2 s(std::fabs(uvt.scale.x) > 1e-9f ? uvt.scale.x : 1e-9f, std::fabs(uvt.scale.y) > 1e-9f ? uvt.scale.y : 1e-9f);
	return rot2((p_tile / uvt.ms - uvt.offset) / s, -uvt.rot);
}

Vector3 HewnEditorPlugin::_uvt_world(const Vector2 &p_raw) const {
	const Vector3 p = uvt.u * p_raw.x - uvt.v * p_raw.y + uvt.n * uvt.nd;
	HewnMesh3D *n = get_edited();
	return uvt.world || !n ? p : n->get_global_transform().xform(p);
}

bool HewnEditorPlugin::_uvt_mouse_raw(Camera3D *p_cam, const Vector2 &p_pos, Vector2 &r_raw) const {
	HewnMesh3D *n = get_edited();
	Vector3 o, d;
	if (!n || !p_cam || !_ray(p_cam, p_pos, o, d)) {
		return false;
	}
	if (!uvt.world) {
		const Transform3D inv = n->get_global_transform().affine_inverse();
		o = inv.xform(o);
		d = inv.basis.xform(d);
	}
	const float den = d.dot(uvt.n);
	if (std::fabs(den) < 1e-6f) {
		return false;
	}
	const float t = (uvt.nd - o.dot(uvt.n)) / den;
	if (t < 0.0f) {
		return false;
	}
	const Vector3 p = o + d * t;
	r_raw = Vector2(p.dot(uvt.u), -p.dot(uvt.v));
	return true;
}

void HewnEditorPlugin::start_uv_transform(Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (!n || mode != MODE_FACE || n->selection.faces.empty() || !p_cam) {
		return;
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	hewn::Mesh &m = n->get_data();
	const Transform3D g = n->get_global_transform();
	uvt = UVTransformState();
	uvt.before = n->get_state();
	for (int f : n->selection.faces) {
		uvt.faces.push_back(f);
		if (f == hover_face) {
			uvt.face = f; // the selected face under the mouse
		}
	}
	if (uvt.face < 0) {
		uvt.face = n->selection.faces.back();
	}
	hewn::Face &F = m.faces[uvt.face];
	// as tiled parameters (other fills, manual UVs: fitted first; when they cannot be, a plain tiling)
	{
		std::vector<std::vector<Vector2>> uvs;
		hewn::compute_uvs(m, g, uvs);
		hewn::set_face_uvs(m, uvt.face, g, uvs[uvt.face]);
		if (F.manual_uv) {
			F.manual_uv = false;
			F.uvp.scale = Vector2(1, 1);
			F.uvp.rotation = 0.0f;
			F.uvp.offset = Vector2();
		}
	}
	uvt.world = F.uvp.world;
	Vector3 nrm = hewn::face_normal_raw(m, F);
	Vector3 c = hewn::face_center(m, F);
	if (uvt.world) {
		nrm = g.basis.xform(nrm);
		c = g.xform(c);
	}
	if (nrm.length_squared() < 1e-20f) {
		nrm = Vector3(0, 1, 0);
	}
	uvt.n = nrm.normalized();
	hewn::uv_axes(uvt.n, uvt.u, uvt.v);
	uvt.nd = c.dot(uvt.n);
	for (int v : F.v) {
		const Vector3 p = uvt.world ? g.xform(m.pos[v]) : m.pos[v];
		uvt.raw.push_back(Vector2(p.dot(uvt.u), -p.dot(uvt.v)));
	}
	std::set<int> seen_v;
	std::set<std::pair<int, int>> seen_e;
	auto raw_of_vertex = [&](int v) {
		const Vector3 p = uvt.world ? g.xform(m.pos[v]) : m.pos[v];
		return Vector2(p.dot(uvt.u), -p.dot(uvt.v));
	};
	for (int64_t i = 0; i < uvt.faces.size(); i++) {
		const hewn::Face &G = m.faces[uvt.faces[i]];
		for (int v : G.v) {
			uvt.outline.push_back(g.xform(m.pos[v]));
		}
		uvt.outline_sizes.push_back(G.size());
		for (int j = 0; j < G.size(); j++) {
			const int a = G.v[j], b = G.v[(j + 1) % G.size()];
			if (seen_v.insert(a).second) {
				uvt.snap_pts.push_back(raw_of_vertex(a));
			}
			if (seen_e.insert(std::make_pair(MIN(a, b), MAX(a, b))).second) {
				uvt.snap_edges.push_back(std::make_pair(raw_of_vertex(a), raw_of_vertex(b)));
			}
		}
	}
	Ref<BaseMaterial3D> bm = n->get_material_for_slot(F.material);
	if (bm.is_valid()) {
		uvt.ms = Vector2(MAX(1e-6f, bm->get_uv1_scale().x), MAX(1e-6f, bm->get_uv1_scale().y));
		uvt.texture = bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
		if (uvt.texture.is_valid()) {
			uvt.tex = uvt.texture->get_size().max(Vector2(1, 1));
		}
	}
	uvt.rot = F.uvp.rotation;
	uvt.scale = F.uvp.scale;
	uvt.offset = F.uvp.offset;
	Vector2 rc;
	for (const Vector2 &r : uvt.raw) {
		rc += r / (float)uvt.raw.size();
	}
	const Vector2 t = _uvt_tile_of(rc);
	uvt.tile = Vector2i((int)std::floor(t.x), (int)std::floor(t.y));
	modal = MODAL_UVT;
	drag = DRAG_NONE;
	uvt.history.clear();
	uvt.hist_pos = 0;
	_uvt_push();
	_uvt_apply();
}

// Shift+T's own undo: a step after each drag / G / R / S.
void HewnEditorPlugin::_uvt_push() {
	uvt.history.resize(uvt.history.empty() ? 0 : uvt.hist_pos + 1);
	uvt.history.push_back({ uvt.rot, uvt.scale, uvt.offset });
	uvt.hist_pos = (int)uvt.history.size() - 1;
}

// A typed value during G / R / S: G moves by tiles (along U, or V when constrained to it), R turns by
// degrees, S scales by the factor (around the middle, or the point picked with B).
void HewnEditorPlugin::_uvt_typed(Camera3D *p_cam) {
	double v = 0.0;
	if (uvt.num.is_empty() || !HewnDragField::eval_expression(uvt.num, v)) {
		_uvt_drag(p_cam, mouse_pos);
		return;
	}
	uvt.rot = uvt.rot0;
	uvt.scale = uvt.scale0;
	uvt.offset = uvt.offset0;
	const Vector2 T(uvt.tile.x, uvt.tile.y);
	const Vector2 ct = uvt.base >= 0 ? uvt.base_tile : T + Vector2(0.5f, 0.5f);
	const Vector2 cr = _uvt_raw_of(ct);
	if (uvt.drag >= 8 && uvt.drag <= 10) {
		const Vector2 d = uvt.drag == 10 ? Vector2(0, (float)v) : Vector2((float)v, 0);
		uvt.offset = uvt.offset0 + d / uvt.ms;
	} else if (uvt.drag == 11) {
		uvt.rot = Math::fposmod(uvt.rot0 + (float)v + 180.0f, 360.0f) - 180.0f;
		uvt.offset = ct / uvt.ms - uvt.scale * rot2(cr, uvt.rot);
	} else if (uvt.drag == 12 && std::fabs(v) > 1e-6) {
		uvt.scale = uvt.scale0 / (float)v;
		if (uvt.scale_axis >= 0) {
			uvt.scale[1 - uvt.scale_axis] = uvt.scale0[1 - uvt.scale_axis]; // (SX / SY: one axis)
		}
		uvt.offset = ct / uvt.ms - uvt.scale * rot2(cr, uvt.rot);
	}
	_uvt_apply();
}

// The handles on screen: 0-3 corners, 4-7 sides (top, right, bottom, left of the image), 8 the middle,
// 9 / 10 moves along U / V, 11 the turn.
void HewnEditorPlugin::_uvt_handles(Camera3D *p_cam, std::vector<Vector2> &r_pos, std::vector<bool> &r_shown) const {
	const Vector2 T(uvt.tile.x, uvt.tile.y);
	const Vector2 tc[12] = { T, T + Vector2(1, 0), T + Vector2(1, 1), T + Vector2(0, 1), T + Vector2(0.5f, 0), T + Vector2(1, 0.5f), T + Vector2(0.5f, 1),
		T + Vector2(0, 0.5f), T + Vector2(0.5f, 0.5f), T + Vector2(0.8f, 0.5f), T + Vector2(0.5f, 0.2f), T + Vector2(0.5f, -0.2f) };
	r_pos.assign(12, Vector2());
	r_shown.assign(12, false);
	for (int i = 0; i < 12; i++) {
		const Vector3 w = _uvt_world(_uvt_raw_of(tc[i]));
		if (!p_cam->is_position_behind(w)) {
			r_pos[i] = p_cam->unproject_position(w);
			r_shown[i] = true;
		}
	}
	// small on screen: only the corners and the middle (the rest would be on top of each other)
	const float side = MIN(r_pos[0].distance_to(r_pos[1]), r_pos[0].distance_to(r_pos[3]));
	if (side < 70.0f) {
		for (int i : { 4, 5, 6, 7, 9, 10 }) {
			r_shown[i] = false;
		}
	}
	if (side < 24.0f) {
		r_shown[11] = false;
	}
}

void HewnEditorPlugin::_uvt_apply() {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	hewn::Mesh &m = n->get_data();
	for (int64_t i = 0; i < uvt.faces.size(); i++) {
		hewn::Face &F = m.faces[uvt.faces[i]];
		F.manual_uv = false;
		F.uv_group = 0;
		F.uvp.fill = hewn::UV_FILL_TILE;
		F.uvp.swap = F.uvp.flip_u = F.uvp.flip_v = false;
		F.uvp.rotation = uvt.rot;
		F.uvp.scale = uvt.scale;
		F.uvp.offset = uvt.offset;
	}
	n->rebuild();
	const Vector2 ppm = uvt.tex * uvt.ms * uvt.scale.abs(); // texture pixels per meter (raw units)
	const float shown_rot = std::fabs(uvt.rot) < 0.05f ? 0.0f : uvt.rot;
	uvt.label = vformat(T("UV Transform  %.0f x %.0f px/m  %.1f deg  offset %.3f, %.3f"), ppm.x, ppm.y, shown_rot, uvt.offset.x, uvt.offset.y) + "\n" +
			T("Corners / sides: scale (Alt: keep proportions), middle / arrows: move, circle: turn, G / R / S (typed values: 1/2); magnet: snap to the faces (Shift: free), Ctrl: pixels") +
			"\n" + T("Ctrl+Z / Ctrl+Shift+Z: undo / redo inside Shift+T; click elsewhere / Enter: done, Esc: cancel");
	if (uvt.key_drag && !uvt.num.is_empty()) {
		uvt.label += "   [" + uvt.num + "]";
	}
	if (uv_canvas) {
		uv_canvas->queue_redraw();
	}
	update_overlays();
}

void HewnEditorPlugin::_uvt_drag(Camera3D *p_cam, const Vector2 &p_pos) {
	Vector2 P;
	if (!_uvt_mouse_raw(p_cam, p_pos, P)) {
		return;
	}
	// the parameters at the start of the drag: the fixed points are computed with them
	uvt.rot = uvt.rot0;
	uvt.scale = uvt.scale0;
	uvt.offset = uvt.offset0;
	const Vector2 eu = rot2(Vector2(signf1(uvt.scale0.x), 0), -uvt.rot0), ev = rot2(Vector2(0, signf1(uvt.scale0.y)), -uvt.rot0);
	// screen pixels per raw unit along a direction, near the tile
	const Vector2 mid = _uvt_raw_of(Vector2(uvt.tile.x + 0.5f, uvt.tile.y + 0.5f));
	auto ppu = [&](const Vector2 &p_dir) {
		const Vector3 a = _uvt_world(mid), b = _uvt_world(mid + p_dir * 0.01f);
		if (p_cam->is_position_behind(a) || p_cam->is_position_behind(b)) {
			return 100.0f;
		}
		return MAX(1e-3f, p_cam->unproject_position(a).distance_to(p_cam->unproject_position(b)) / 0.01f);
	};
	const bool snap = snapping_active(false) && !uvt.shift_free && !uvt.pixel_snap;
	auto px = [&](const Vector2 &p_a, const Vector2 &p_b) {
		const Vector3 a = _uvt_world(p_a), b = _uvt_world(p_b);
		if (p_cam->is_position_behind(a) || p_cam->is_position_behind(b)) {
			return 1e9f;
		}
		return p_cam->unproject_position(a).distance_to(p_cam->unproject_position(b));
	};
	auto closest_on = [](const Vector2 &p, const std::pair<Vector2, Vector2> &e) {
		const Vector2 d = e.second - e.first;
		const float l2 = d.length_squared();
		return l2 < 1e-12f ? e.first : e.first + d * CLAMP((p - e.first).dot(d) / l2, 0.0f, 1.0f);
	};
	// a point onto a vertex of the faces near it on the screen, else onto an edge (0: none, 1: vertex, 2: edge)
	auto snap_point = [&](Vector2 &r_p) {
		float best = SNAP_PX;
		int found = 0;
		Vector2 out = r_p;
		for (const Vector2 &w : uvt.snap_pts) {
			const float d = px(r_p, w);
			if (d < best) {
				best = d;
				out = w;
				found = 1;
			}
		}
		if (!found) {
			for (const std::pair<Vector2, Vector2> &e : uvt.snap_edges) {
				const Vector2 c = closest_on(r_p, e);
				const float d = px(r_p, c);
				if (d < best) {
					best = d;
					out = c;
					found = 2;
				}
			}
		}
		r_p = out;
		return found;
	};
	// a length along `dir` from F: the point there onto a face edge crossing that line, or the tile's side
	// through it onto a face edge parallel to that side
	auto snap_len = [&](float p_len, const Vector2 &p_from, const Vector2 &p_dir, bool &r_snapped) {
		float best = SNAP_PX, out = p_len;
		r_snapped = false;
		const Vector2 cur = p_from + p_dir * p_len;
		auto consider = [&](float t) {
			if (t > 1e-4f) {
				const float d = px(cur, p_from + p_dir * t);
				if (d < best) {
					best = d;
					out = t;
					r_snapped = true;
				}
			}
		};
		for (const std::pair<Vector2, Vector2> &e : uvt.snap_edges) {
			const Vector2 ed = e.second - e.first;
			const float el = ed.length();
			if (el < 1e-6f) {
				continue;
			}
			const float den = p_dir.cross(ed);
			if (std::fabs(den) > 1e-9f) {
				const float t = (e.first - p_from).cross(ed) / den, s = (e.first - p_from).cross(p_dir) / den;
				if (s >= -1e-4f && s <= 1.0f + 1e-4f) {
					consider(t);
				}
			}
			if (std::fabs(ed.dot(p_dir) / el) < 0.01f) {
				consider((e.first - p_from).dot(p_dir)); // (a side flush with a parallel edge)
			}
		}
		return out;
	};
	const Vector2 T(uvt.tile.x, uvt.tile.y);
	const Vector2 corners[4] = { T, T + Vector2(1, 0), T + Vector2(1, 1), T + Vector2(0, 1) };
	const float aspect = uvt.tex.y / uvt.tex.x; // square pixels: the V length is the U length times this
	auto set_scale_keep = [&](float p_lu, float p_lv, const Vector2 &p_fixed_tile, const Vector2 &p_fixed_raw) {
		uvt.scale = Vector2(signf1(uvt.scale0.x) / (uvt.ms.x * MAX(1e-4f, p_lu)), signf1(uvt.scale0.y) / (uvt.ms.y * MAX(1e-4f, p_lv)));
		uvt.offset = p_fixed_tile / uvt.ms - uvt.scale * rot2(p_fixed_raw, uvt.rot);
	};
	const float lu0 = 1.0f / (uvt.ms.x * MAX(1e-9f, std::fabs(uvt.scale0.x))), lv0 = 1.0f / (uvt.ms.y * MAX(1e-9f, std::fabs(uvt.scale0.y)));
	const int h = uvt.drag;
	if (h >= 0 && h < 4) {
		// a corner: the opposite one stays; the corner onto a vertex / edge, else the neighbor corners
		// (or the sides) onto edges; near square pixels: square
		const Vector2 ft = corners[(h + 2) % 4];
		const Vector2 fr = _uvt_raw_of(ft);
		const Vector2 dk = corners[h] - ft;
		const Vector2 du = eu * dk.x, dv = ev * dk.y;
		const int on = snap ? snap_point(P) : 0;
		float lu = (P - fr).dot(du), lv = (P - fr).dot(dv);
		bool su = on == 1, sv = on == 1;
		if (snap && on != 1) {
			bool a, b;
			const float slu = snap_len(lu, fr, du, a), slv = snap_len(lv, fr, dv, b);
			if (on == 0 || a) {
				lu = slu;
				su = a;
			}
			if (on == 0 || b) {
				lv = slv;
				sv = b;
			}
		}
		if (uvt.keep_aspect) {
			// Alt: the proportions stay (the axis moved more decides)
			const float ku = lu / MAX(1e-9f, lu0), kv = lv / MAX(1e-9f, lv0);
			const float kk = std::fabs(ku - 1.0f) > std::fabs(kv - 1.0f) ? ku : kv;
			lu = lu0 * kk;
			lv = lv0 * kk;
		} else if (snap && !(su && sv) && on != 2 && std::fabs(lv / MAX(1e-6f, lu * aspect) - 1.0f) < 0.06f) {
			if (sv) {
				lu = lv / aspect;
			} else {
				lv = lu * aspect;
			}
		}
		set_scale_keep(lu, lv, ft, fr);
	} else if (h >= 4 && h < 8) {
		// a side: one axis; the side's point onto a vertex / edge, else its corners onto edges
		const int e = h - 4;
		const Vector2 side_mid[4] = { T + Vector2(0.5f, 0), T + Vector2(1, 0.5f), T + Vector2(0.5f, 1), T + Vector2(0, 0.5f) };
		const Vector2 ft = side_mid[(e + 2) % 4];
		const Vector2 fr = _uvt_raw_of(ft);
		const Vector2 dt = side_mid[e] - ft;
		const bool along_u = e % 2 == 1; // right / left
		const Vector2 dir = along_u ? eu * dt.x : ev * dt.y;
		const Vector2 across = along_u ? ev : eu;
		const float half = 0.5f * (along_u ? lv0 : lu0);
		const int on = snap ? snap_point(P) : 0;
		float l = (P - fr).dot(dir);
		bool snapped = on != 0;
		if (snap && !snapped) {
			float best_l = l;
			for (float sgn : { -1.0f, 1.0f }) {
				bool sn;
				const float c = snap_len(l, fr + across * half * sgn, dir, sn);
				if (sn && (!snapped || std::fabs(c - l) < std::fabs(best_l - l))) {
					best_l = c;
					snapped = true;
				}
			}
			l = best_l;
		}
		if (uvt.keep_aspect) {
			// Alt: the other axis grows alike
			const float kk = l / MAX(1e-9f, along_u ? lu0 : lv0);
			set_scale_keep(lu0 * kk, lv0 * kk, ft, fr);
		} else if (along_u) {
			if (snap && !snapped && std::fabs(lv0 / MAX(1e-6f, l * aspect) - 1.0f) < 0.06f) {
				l = lv0 / aspect;
			}
			set_scale_keep(l, lv0, ft, fr);
		} else {
			if (snap && !snapped && std::fabs(l / MAX(1e-6f, lu0 * aspect) - 1.0f) < 0.06f) {
				l = lu0 * aspect;
			}
			set_scale_keep(lu0, l, ft, fr);
		}
	} else if (h == 8 && uvt.base >= 0) {
		// G with a point picked by B: that point goes under the mouse and follows it,
		// onto a vertex / an edge of the faces near it
		Vector2 target = P;
		if (!uvt.shift_free && !uvt.pixel_snap) {
			snap_point(target); // (B is there to put that point onto something: the magnet or not; Shift: free)
		}
		uvt.offset = uvt.base_tile / uvt.ms - uvt.scale * rot2(target, uvt.rot);
	} else if (h >= 8 && h <= 10) {
		// the texture point under the mouse stays under it
		Vector2 d = uvt.drag_r0 - P;
		if (h == 9) {
			d = eu * d.dot(eu);
		} else if (h == 10) {
			d = ev * d.dot(ev);
		}
		uvt.offset = uvt.offset0 + uvt.scale * rot2(d, uvt.rot);
		if (uvt.pixel_snap && !uvt.raw.empty()) {
			// Ctrl: the first corner of the face on a texture pixel corner
			const Vector2 t = _uvt_tile_of(uvt.raw[0]);
			const Vector2 tp = (t * uvt.tex).round() / uvt.tex;
			if (h != 10) {
				uvt.offset.x += (tp.x - t.x) / uvt.ms.x;
			}
			if (h != 9) {
				uvt.offset.y += (tp.y - t.y) / uvt.ms.y;
			}
		} else if (snap) {
			// the tile (moved by `delta` on the plane): a corner onto a vertex, else corners onto edges,
			// else its sides flush with parallel edges
			const Vector2 axis = h == 9 ? eu : h == 10 ? ev : Vector2();
			Vector2 qc[4];
			for (int i = 0; i < 4; i++) {
				qc[i] = _uvt_raw_of(corners[i]);
			}
			bool found = false;
			Vector2 delta;
			float best = SNAP_PX;
			for (const Vector2 &c : qc) {
				for (const Vector2 &w : uvt.snap_pts) {
					Vector2 dd = w - c;
					if (axis != Vector2() && std::fabs(dd.cross(axis)) > 1e-4f) {
						continue; // (moving along one axis: only what lies on it)
					}
					const float dpx = px(c, w);
					if (dpx < best) {
						best = dpx;
						delta = dd;
						found = true;
					}
				}
			}
			if (!found) {
				for (const Vector2 &c : qc) {
					for (const std::pair<Vector2, Vector2> &e : uvt.snap_edges) {
						Vector2 q;
						if (axis == Vector2()) {
							q = closest_on(c, e);
						} else {
							const Vector2 ed = e.second - e.first;
							const float den = axis.cross(ed);
							if (std::fabs(den) < 1e-9f) {
								continue;
							}
							const float s = (e.first - c).cross(axis) / den;
							if (s < 0.0f || s > 1.0f) {
								continue;
							}
							q = e.first + ed * s;
						}
						const float dpx = px(c, q);
						if (dpx < best && (q - c).length() > 1e-6f) {
							best = dpx;
							delta = q - c;
							found = true;
						}
					}
				}
			}
			if (!found) {
				// each axis on its own: the tile's sides along it onto face edges parallel to them
				for (int a = 0; a < 2; a++) {
					const Vector2 perp = a == 0 ? ev : eu; // the shift goes across the sides
					if (axis != Vector2() && std::fabs(perp.dot(axis)) < 0.5f) {
						continue;
					}
					const Vector2 mids[2] = { a == 0 ? (qc[0] + qc[1]) * 0.5f : (qc[0] + qc[3]) * 0.5f, a == 0 ? (qc[2] + qc[3]) * 0.5f : (qc[1] + qc[2]) * 0.5f };
					float bpx = SNAP_PX, shift = 0.0f;
					for (const Vector2 &mp : mids) {
						for (const std::pair<Vector2, Vector2> &e : uvt.snap_edges) {
							const Vector2 ed = e.second - e.first;
							const float el = ed.length();
							if (el < 1e-6f || std::fabs(ed.dot(perp) / el) > 0.01f) {
								continue;
							}
							const float sh = (e.first - mp).dot(perp);
							const float dpx = px(mp, mp + perp * sh);
							if (std::fabs(sh) > 1e-6f && dpx < bpx) {
								bpx = dpx;
								shift = sh;
								found = true;
							}
						}
					}
					delta += perp * shift;
				}
			}
			if (found) {
				uvt.offset -= uvt.scale * rot2(delta, uvt.rot); // (the tile moves by delta)
			}
		}
	} else if (h == 12) {
		// S: the tile grows / shrinks around its middle (or the B point) as the mouse goes from / to it
		const Vector2 ct = uvt.base >= 0 ? uvt.base_tile : T + Vector2(0.5f, 0.5f);
		const Vector2 cr = _uvt_raw_of(ct);
		float k = (P - cr).length() / MAX(1e-6f, (uvt.drag_r0 - cr).length());
		if (snap && dock) {
			const float st = MAX(0.001f, (float)dock->snap_scale->get_value());
			k = std::round(k / st) * st;
		}
		uvt.scale = uvt.scale0 / MAX(1e-3f, k);
		if (uvt.scale_axis >= 0) {
			uvt.scale[1 - uvt.scale_axis] = uvt.scale0[1 - uvt.scale_axis]; // (SX / SY: one axis)
		}
		uvt.offset = ct / uvt.ms - uvt.scale * rot2(cr, uvt.rot);
	} else if (h == 11) {
		const Vector2 ct = uvt.base >= 0 ? uvt.base_tile : T + Vector2(0.5f, 0.5f);
		const Vector2 cr = _uvt_raw_of(ct);
		const float a0 = (uvt.drag_r0 - cr).angle(), a1 = (P - cr).angle();
		float r = uvt.rot0 - (a1 - a0) * (float)(180.0 / Math::PI);
		if (uvt.pixel_snap) {
			r = std::round(r / 15.0f) * 15.0f;
		} else if (snap) {
			// the tile's sides along an edge of the faces
			const float k = (float)(180.0 / Math::PI);
			float best = 3.0f, snapped = r;
			for (const std::pair<Vector2, Vector2> &e : uvt.snap_edges) {
				const Vector2 ed = e.second - e.first;
				if (ed.length() < 1e-6f) {
					continue;
				}
				const float base = -ed.angle() * k; // rot that turns this edge onto U
				const float cand = base + std::round((r - base) / 90.0f) * 90.0f;
				if (std::fabs(cand - r) < best) {
					best = std::fabs(cand - r);
					snapped = cand;
				}
			}
			r = snapped;
		}
		uvt.rot = Math::fposmod(r + 180.0f, 360.0f) - 180.0f;
		uvt.offset = ct / uvt.ms - uvt.scale * rot2(cr, uvt.rot);
	}
	_uvt_apply();
}

void HewnEditorPlugin::_uvt_finish(bool p_apply) {
	HewnMesh3D *n = get_edited();
	modal = MODAL_NONE;
	uvt.drag = -1;
	if (n) {
		if (p_apply) {
			EditorUndoRedoManager *ur = get_undo_redo();
			ur->create_action("Hewn: UV Transform", UndoRedo::MERGE_DISABLE, n);
			ur->add_do_method(n, "set_state", n->get_state());
			ur->add_undo_method(n, "set_state", uvt.before);
			ur->commit_action(false);
		} else {
			n->set_state(uvt.before);
		}
	}
	selection_changed();
}

void HewnEditorPlugin::_uvt_key_drag_end(bool p_keep) {
	if (!p_keep) {
		uvt.rot = uvt.rot0;
		uvt.scale = uvt.scale0;
		uvt.offset = uvt.offset0;
	}
	uvt.key_drag = false;
	uvt.drag = -1;
	uvt.base = -1;
	uvt.num = String();
	if (p_keep) {
		_uvt_push();
	}
	_uvt_apply();
}

int32_t HewnEditorPlugin::_uvt_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	Input *in = Input::get_singleton();
	const bool flying = in->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT) || in->is_mouse_button_pressed(MOUSE_BUTTON_MIDDLE);
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		mouse_pos = mm->get_position();
		uvt.shift_free = mm->is_shift_pressed();
		uvt.keep_aspect = mm->is_alt_pressed();
		if (uvt.drag >= 0) {
			if (uvt.key_drag && !uvt.num.is_empty()) {
				return AFTER_GUI_INPUT_STOP; // (a typed value holds)
			}
			_uvt_drag(p_cam, mouse_pos);
			return AFTER_GUI_INPUT_STOP;
		}
		std::vector<Vector2> hp;
		std::vector<bool> shown;
		_uvt_handles(p_cam, hp, shown);
		uvt.hover = -1;
		float best = HANDLE_PX;
		for (int i = 0; i < 12; i++) {
			if (shown[i] && hp[i].distance_to(mouse_pos) < best) {
				best = hp[i].distance_to(mouse_pos);
				uvt.hover = i;
			}
		}
		Vector2 r;
		if (uvt.hover < 0 && _uvt_mouse_raw(p_cam, mouse_pos, r)) {
			// another repetition under the mouse: the handles move there (the texture stays). Not near
			// the current one: the turn circle sits outside it, and reaching it must not move it away.
			const Vector2 t = _uvt_tile_of(r);
			const float margin = 0.35f;
			if (t.x < uvt.tile.x - margin || t.x > uvt.tile.x + 1 + margin || t.y < uvt.tile.y - margin || t.y > uvt.tile.y + 1 + margin) {
				uvt.tile = Vector2i((int)std::floor(t.x), (int)std::floor(t.y));
			}
		}
		update_overlays();
		return flying ? AFTER_GUI_INPUT_PASS : AFTER_GUI_INPUT_STOP;
	}
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && uvt.key_drag) {
		// G / R / S: a left click keeps it, the right one goes back
		if (mb->is_pressed() && (mb->get_button_index() == MOUSE_BUTTON_LEFT || mb->get_button_index() == MOUSE_BUTTON_RIGHT)) {
			_uvt_key_drag_end(mb->get_button_index() == MOUSE_BUTTON_LEFT);
			return AFTER_GUI_INPUT_STOP;
		}
		return mb->get_button_index() == MOUSE_BUTTON_LEFT ? AFTER_GUI_INPUT_STOP : AFTER_GUI_INPUT_PASS;
	}
	if (mb.is_valid()) {
		if (mb->get_button_index() != MOUSE_BUTTON_LEFT) {
			return AFTER_GUI_INPUT_PASS; // (the camera: right / middle button, wheel)
		}
		if (!mb->is_pressed()) {
			if (uvt.drag >= 0) {
				_uvt_push(); // (a handle dragged: a step of its own undo)
			}
			uvt.drag = -1;
			return AFTER_GUI_INPUT_STOP;
		}
		if (uvt.hover >= 0 && _uvt_mouse_raw(p_cam, mb->get_position(), uvt.drag_r0)) {
			uvt.drag = uvt.hover;
			uvt.rot0 = uvt.rot;
			uvt.scale0 = uvt.scale;
			uvt.offset0 = uvt.offset;
			return AFTER_GUI_INPUT_STOP;
		}
		_uvt_finish(true);
		return AFTER_GUI_INPUT_STOP;
	}
	Ref<InputEventKey> k = p_event;
	if (k.is_null()) {
		return AFTER_GUI_INPUT_PASS;
	}
	const Key code = key_code(k);
	if (code == KEY_CTRL || code == KEY_META) {
		uvt.pixel_snap = k->is_pressed();
		if (uvt.drag >= 0) {
			_uvt_drag(p_cam, mouse_pos);
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (code == KEY_ALT) {
		uvt.keep_aspect = k->is_pressed();
		if (uvt.drag >= 0 && uvt.drag < 8) {
			_uvt_drag(p_cam, mouse_pos);
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (!k->is_pressed() || k->is_echo()) {
		return flying ? AFTER_GUI_INPUT_PASS : AFTER_GUI_INPUT_STOP;
	}
	if (uvt.key_drag) {
		const char32_t uc = k->get_unicode();
		if (code == KEY_ESCAPE || code == KEY_ENTER || code == KEY_KP_ENTER || code == KEY_SPACE) {
			_uvt_key_drag_end(code != KEY_ESCAPE);
		} else if ((code == KEY_X || code == KEY_Y) && uvt.drag >= 8 && uvt.drag <= 10) {
			// along U / V only (again: free)
			const int want = code == KEY_X ? 9 : 10;
			uvt.drag = uvt.drag == want ? 8 : want;
			uvt.num.is_empty() ? _uvt_drag(p_cam, mouse_pos) : _uvt_typed(p_cam);
		} else if ((code == KEY_X || code == KEY_Y) && uvt.drag == 12) {
			// SX / SY: the scale along U / V only (again: both)
			const int want = code == KEY_X ? 0 : 1;
			uvt.scale_axis = uvt.scale_axis == want ? -1 : want;
			uvt.num.is_empty() ? _uvt_drag(p_cam, mouse_pos) : _uvt_typed(p_cam);
		} else if (code == KEY_BACKSPACE) {
			uvt.num = uvt.num.substr(0, MAX(0, uvt.num.length() - 1));
			_uvt_typed(p_cam);
		} else if ((uc >= '0' && uc <= '9') || uc == '.' || uc == ',' || uc == '-' || uc == '/' || uc == '*' || uc == '+' || uc == '(' || uc == ')' ||
				(code >= KEY_KP_0 && code <= KEY_KP_9) || code == KEY_KP_PERIOD || code == KEY_KP_DIVIDE || code == KEY_KP_MULTIPLY || code == KEY_KP_SUBTRACT ||
				code == KEY_KP_ADD) {
			// a typed value (expressions too: 1/2, 2*3)
			String ch = uc ? String::chr(uc) : String();
			if (code >= KEY_KP_0 && code <= KEY_KP_9) {
				ch = String::chr('0' + (code - KEY_KP_0));
			} else if (code == KEY_KP_PERIOD) {
				ch = ".";
			} else if (code == KEY_KP_DIVIDE) {
				ch = "/";
			} else if (code == KEY_KP_MULTIPLY) {
				ch = "*";
			} else if (code == KEY_KP_SUBTRACT) {
				ch = "-";
			} else if (code == KEY_KP_ADD) {
				ch = "+";
			}
			uvt.num += ch == "," ? String(".") : ch;
			_uvt_typed(p_cam);
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (code == KEY_Z && (k->is_ctrl_pressed() || k->is_meta_pressed()) && uvt.drag < 0) {
		// its own undo / redo (Ctrl+Shift+Z): only inside Shift+T
		const int to = k->is_shift_pressed() ? uvt.hist_pos + 1 : uvt.hist_pos - 1;
		if (to >= 0 && to < (int)uvt.history.size()) {
			uvt.hist_pos = to;
			uvt.rot = uvt.history[to].rot;
			uvt.scale = uvt.history[to].scale;
			uvt.offset = uvt.history[to].offset;
			_uvt_apply();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if ((code == KEY_G || code == KEY_R || code == KEY_S) && !k->is_ctrl_pressed() && !k->is_alt_pressed() && !k->is_shift_pressed() && !flying &&
			_uvt_mouse_raw(p_cam, mouse_pos, uvt.drag_r0)) {
		// G / R / S on the texture: it follows the mouse until a click (from the point B picked, if any)
		uvt.drag = code == KEY_G ? 8 : code == KEY_R ? 11 : 12;
		uvt.key_drag = true;
		uvt.scale_axis = -1;
		uvt.num = String();
		uvt.rot0 = uvt.rot;
		uvt.scale0 = uvt.scale;
		uvt.offset0 = uvt.offset;
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (code == KEY_ESCAPE) {
		_uvt_finish(false);
		return AFTER_GUI_INPUT_STOP;
	}
	if (code == KEY_ENTER || code == KEY_KP_ENTER || code == KEY_SPACE || (code == KEY_T && k->is_shift_pressed())) {
		_uvt_finish(true);
		return AFTER_GUI_INPUT_STOP;
	}
	return flying ? AFTER_GUI_INPUT_PASS : AFTER_GUI_INPUT_STOP; // (WASD while flying the camera)
}

void HewnEditorPlugin::_draw_uvt(Control *p_overlay, Camera3D *p_cam) {
	auto sp = [&](const Vector2 &p_tile, Vector2 &r) {
		const Vector3 w = _uvt_world(_uvt_raw_of(p_tile));
		if (p_cam->is_position_behind(w)) {
			return false;
		}
		r = p_cam->unproject_position(w);
		return true;
	};
	// the repetitions around the face
	Vector2 tmin(uvt.tile.x, uvt.tile.y), tmax = tmin + Vector2(1, 1);
	for (const Vector2 &r : uvt.raw) {
		tmin = tmin.min(_uvt_tile_of(r));
		tmax = tmax.max(_uvt_tile_of(r));
	}
	const int x0 = (int)std::floor(tmin.x) - 1, x1 = MIN((int)std::ceil(tmax.x) + 1, x0 + 40);
	const int y0 = (int)std::floor(tmin.y) - 1, y1 = MIN((int)std::ceil(tmax.y) + 1, y0 + 40);
	const Color grid(1, 1, 1, 0.22f);
	for (int x = x0; x <= x1; x++) {
		Vector2 a, b;
		if (sp(Vector2(x, y0), a) && sp(Vector2(x, y1), b)) {
			p_overlay->draw_line(a, b, grid, 1.0f, true);
		}
	}
	for (int y = y0; y <= y1; y++) {
		Vector2 a, b;
		if (sp(Vector2(x0, y), a) && sp(Vector2(x1, y), b)) {
			p_overlay->draw_line(a, b, grid, 1.0f, true);
		}
	}
	// the ghost of the active tile (in small pieces: near enough to the perspective)
	const Vector2 T(uvt.tile.x, uvt.tile.y);
	if (uvt.texture.is_valid()) {
		const int N = 8;
		for (int i = 0; i < N; i++) {
			for (int j = 0; j < N; j++) {
				const Vector2 q[4] = { Vector2(i, j) / N, Vector2(i + 1, j) / N, Vector2(i + 1, j + 1) / N, Vector2(i, j + 1) / N };
				PackedVector2Array pts, uvs;
				PackedColorArray cols;
				bool ok = true;
				for (const Vector2 &c : q) {
					Vector2 s;
					ok = ok && sp(T + c, s);
					pts.push_back(s);
					uvs.push_back(c);
					cols.push_back(Color(1, 1, 1, 0.7f));
				}
				if (ok) {
					p_overlay->draw_polygon(pts, cols, uvs, uvt.texture);
				}
			}
		}
	}
	// the faces
	size_t at = 0;
	for (int size : uvt.outline_sizes) {
		for (int i = 0; i < size; i++) {
			const Vector3 a = uvt.outline[at + i], b = uvt.outline[at + (i + 1) % size];
			if (!p_cam->is_position_behind(a) && !p_cam->is_position_behind(b)) {
				p_overlay->draw_line(p_cam->unproject_position(a), p_cam->unproject_position(b), Color(1, 1, 1, 0.9f), 1.5f, true);
			}
		}
		at += size;
	}
	// the tile and its handles
	std::vector<Vector2> hp;
	std::vector<bool> shown;
	_uvt_handles(p_cam, hp, shown);
	const Color edge(1.0f, 0.72f, 0.2f), hot(1, 1, 1);
	for (int i = 0; i < 4; i++) {
		if (shown[i] && shown[(i + 1) % 4]) {
			p_overlay->draw_line(hp[i], hp[(i + 1) % 4], edge, 2.0f, true);
		}
	}
	auto col = [&](int i) { return (i == uvt.hover || i == uvt.drag) ? hot : edge; };
	for (int i = 0; i < 12; i++) {
		if (!shown[i]) {
			continue;
		}
		if (i < 4) {
			p_overlay->draw_rect(Rect2(hp[i] - Vector2(5, 5), Vector2(10, 10)), col(i));
		} else if (i < 8) {
			p_overlay->draw_rect(Rect2(hp[i] - Vector2(4, 4), Vector2(8, 8)), col(i), false, 2.0f);
		} else if (i == 8) {
			p_overlay->draw_circle(hp[i], 6.0f, col(i));
		} else if (i < 11) {
			// an arrow from the middle
			const Vector2 d = (hp[i] - hp[8]);
			if (d.length() > 1.0f) {
				const Vector2 dn = d.normalized(), pn(-dn.y, dn.x);
				p_overlay->draw_line(hp[8], hp[i], col(i), 2.0f, true);
				PackedVector2Array tri;
				tri.push_back(hp[i] + dn * 7.0f);
				tri.push_back(hp[i] - dn * 3.0f + pn * 5.0f);
				tri.push_back(hp[i] - dn * 3.0f - pn * 5.0f);
				p_overlay->draw_colored_polygon(tri, col(i));
			}
		} else {
			if (shown[4]) {
				p_overlay->draw_dashed_line(hp[4], hp[i], edge, 1.0f, 4.0f);
			}
			p_overlay->draw_arc(hp[i], 7.0f, 0.0f, (float)Math::PI * 2.0f, 20, col(i), 2.0f, true);
		}
	}
	// the point B picked: a target mark on it (it moves with the tile)
	if (uvt.base >= 0) {
		Vector2 bp;
		if (sp(uvt.base_tile, bp)) {
			const Color bc(0.3f, 0.85f, 1.0f);
			p_overlay->draw_circle(bp, 9.0f, Color(0, 0, 0, 0.35f));
			p_overlay->draw_arc(bp, 8.0f, 0.0f, (float)Math::PI * 2.0f, 24, bc, 2.0f, true);
			p_overlay->draw_line(bp - Vector2(13, 0), bp - Vector2(4, 0), bc, 2.0f);
			p_overlay->draw_line(bp + Vector2(4, 0), bp + Vector2(13, 0), bc, 2.0f);
			p_overlay->draw_line(bp - Vector2(0, 13), bp - Vector2(0, 4), bc, 2.0f);
			p_overlay->draw_line(bp + Vector2(0, 4), bp + Vector2(0, 13), bc, 2.0f);
			p_overlay->draw_circle(bp, 2.5f, bc);
		}
	}
	Ref<Font> font = p_overlay->get_theme_default_font();
	if (font.is_valid()) {
		draw_header(p_overlay, font, uvt.label);
	}
}

// B: the tile point nearest to the mouse (a corner, the middle of a side, the center) is what moves (G)
// or stays (R / S).
void HewnEditorPlugin::_uvt_pick_base(Camera3D *p_cam) {
	const Vector2 T(uvt.tile.x, uvt.tile.y);
	const Vector2 pts[9] = { T, T + Vector2(1, 0), T + Vector2(1, 1), T + Vector2(0, 1), T + Vector2(0.5f, 0), T + Vector2(1, 0.5f), T + Vector2(0.5f, 1),
		T + Vector2(0, 0.5f), T + Vector2(0.5f, 0.5f) };
	// (where the tile is now, not where the drag started: the picked point is on screen)
	float best = 1e30f;
	for (int i = 0; i < 9; i++) {
		const Vector3 w = _uvt_world(_uvt_raw_of(pts[i]));
		if (p_cam->is_position_behind(w)) {
			continue;
		}
		const float d = p_cam->unproject_position(w).distance_to(mouse_pos);
		if (d < best) {
			best = d;
			uvt.base = i;
			uvt.base_tile = pts[i];
		}
	}
	// a drag goes on from here with the parameters it has now
	uvt.rot0 = uvt.rot;
	uvt.scale0 = uvt.scale;
	uvt.offset0 = uvt.offset;
	_uvt_mouse_raw(p_cam, mouse_pos, uvt.drag_r0);
	update_overlays();
}
