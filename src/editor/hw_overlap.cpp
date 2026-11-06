// Overlapping Faces check (Overlays menu): faces of the edited mesh that lie on the same plane and
// cover each other (z-fighting, or a wall inside a wall) are shown in red, or in blue when they face
// opposite ways (a double-sided sheet, a room touching another one).
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/font.hpp>

#include <array>
#include <map>

using namespace godot;
using namespace hw_editor;

namespace {

struct PlaneFace {
	int face;
	Vector3 n; // unit normal
	std::vector<std::array<Vector3, 3>> tris;
	AABB box;
};

float area2d(const std::vector<Vector2> &p) {
	float a = 0.0f;
	for (size_t i = 0; i < p.size(); i++) {
		a += p[i].cross(p[(i + 1) % p.size()]);
	}
	return a * 0.5f;
}

// The part of triangle a inside triangle b (both counter-clockwise).
std::vector<Vector2> clip_tri(const std::vector<Vector2> &a, const std::vector<Vector2> &b) {
	std::vector<Vector2> poly = a;
	for (int e = 0; e < 3 && poly.size() >= 3; e++) {
		const Vector2 p = b[e], q = b[(e + 1) % 3];
		std::vector<Vector2> out;
		for (size_t i = 0; i < poly.size(); i++) {
			const Vector2 c = poly[i], d = poly[(i + 1) % poly.size()];
			const float sc = (q - p).cross(c - p), sd = (q - p).cross(d - p);
			if (sc >= 0.0f) {
				out.push_back(c);
			}
			if ((sc >= 0.0f) != (sd >= 0.0f)) {
				out.push_back(c + (d - c) * (sc / (sc - sd)));
			}
		}
		poly = out;
	}
	return poly;
}

} // namespace

void HewnEditorPlugin::_update_overlaps() {
	overlap_same.clear();
	overlap_opposite.clear();
	overlap_count = 0;
	HewnMesh3D *n = get_edited();
	if (!n || !dock || !dock->ov_overlap || !dock->ov_overlap->is_pressed()) {
		return;
	}
	const hewn::Mesh &m = n->get_data();
	const Transform3D g = n->get_global_transform();
	// faces grouped by their plane (the normal up to its sign, the distance), in world space
	std::map<std::array<int64_t, 4>, std::vector<PlaneFace>> planes;
	for (int f = 0; f < (int)m.faces.size(); f++) {
		const hewn::Face &F = m.faces[f];
		if (F.size() < 3 || F.hidden) {
			continue;
		}
		std::vector<Vector3> pts;
		for (int v : F.v) {
			pts.push_back(g.xform(m.pos[v]));
		}
		Vector3 nr;
		for (size_t i = 0; i < pts.size(); i++) {
			nr += pts[i].cross(pts[(i + 1) % pts.size()]);
		}
		if (nr.length() < 1e-9f) {
			continue;
		}
		nr.normalize();
		Vector3 key_n = nr;
		for (int k = 0; k < 3; k++) {
			if (std::fabs(key_n[k]) > 1e-4f) {
				if (key_n[k] < 0.0f) {
					key_n = -key_n;
				}
				break;
			}
		}
		const float d = key_n.dot(pts[0]);
		const std::array<int64_t, 4> key = { std::llround(key_n.x * 500.0), std::llround(key_n.y * 500.0), std::llround(key_n.z * 500.0), std::llround(d * 500.0) };
		PlaneFace hw_face;
		hw_face.face = f;
		hw_face.n = nr;
		std::vector<int> tris;
		hewn::triangulate(pts, nr, tris);
		for (size_t i = 0; i + 2 < tris.size(); i += 3) {
			hw_face.tris.push_back({ pts[tris[i]], pts[tris[i + 1]], pts[tris[i + 2]] });
		}
		hw_face.box = AABB(pts[0], Vector3());
		for (const Vector3 &p : pts) {
			hw_face.box.expand_to(p);
		}
		hw_face.box = hw_face.box.grow(1e-3f);
		planes[key].push_back(hw_face);
	}
	std::vector<char> hit(m.faces.size(), 0);
	for (auto &it : planes) {
		std::vector<PlaneFace> &fs = it.second;
		if (fs.size() < 2) {
			continue;
		}
		Vector3 u, v;
		hewn::plane_basis(fs[0].n, u, v);
		auto flat = [&](const std::array<Vector3, 3> &t) {
			std::vector<Vector2> r = { Vector2(t[0].dot(u), t[0].dot(v)), Vector2(t[1].dot(u), t[1].dot(v)), Vector2(t[2].dot(u), t[2].dot(v)) };
			if (area2d(r) < 0.0f) {
				std::swap(r[1], r[2]);
			}
			return r;
		};
		const Vector3 nn = u.cross(v).normalized();
		for (size_t i = 0; i < fs.size(); i++) {
			for (size_t j = i + 1; j < fs.size(); j++) {
				if (!fs[i].box.intersects(fs[j].box)) {
					continue;
				}
				const bool opposite = fs[i].n.dot(fs[j].n) < 0.0f;
				const float off = nn.dot(fs[i].tris.empty() ? Vector3() : fs[i].tris[0][0]);
				bool any = false;
				for (const auto &ta : fs[i].tris) {
					const std::vector<Vector2> a = flat(ta);
					for (const auto &tb : fs[j].tris) {
						const std::vector<Vector2> part = clip_tri(a, flat(tb));
						if (part.size() < 3 || area2d(part) < 1e-6f) {
							continue; // (only touching)
						}
						std::vector<Vector3> poly;
						for (const Vector2 &p : part) {
							poly.push_back(u * p.x + v * p.y + nn * off);
						}
						(opposite ? overlap_opposite : overlap_same).push_back(poly);
						any = true;
					}
				}
				if (any) {
					hit[fs[i].face] = hit[fs[j].face] = 1;
				}
			}
		}
	}
	for (char h : hit) {
		overlap_count += h;
	}
}

void HewnEditorPlugin::_draw_overlaps(Control *p_overlay, Camera3D *p_cam) {
	if (!dock || !dock->ov_overlap || !dock->ov_overlap->is_pressed() || !get_edited()) {
		return;
	}
	for (int k = 0; k < 2; k++) {
		const Color c = k == 0 ? Color(1.0f, 0.15f, 0.15f, 0.45f) : Color(0.2f, 0.45f, 1.0f, 0.45f);
		for (const std::vector<Vector3> &poly : k == 0 ? overlap_same : overlap_opposite) {
			PackedVector2Array pts;
			bool ok = true;
			for (const Vector3 &p : poly) {
				ok = ok && !p_cam->is_position_behind(p);
				if (ok) {
					pts.push_back(p_cam->unproject_position(p));
				}
			}
			if (ok && pts.size() >= 3) {
				p_overlay->draw_colored_polygon(pts, c);
			}
		}
	}
	Ref<Font> font = p_overlay->get_theme_default_font();
	if (font.is_valid()) {
		const String t = overlap_count ? vformat(T("Overlapping faces: %d (red: same side, blue: opposite)"), overlap_count) : String(T("No overlapping faces"));
		const int fs = p_overlay->get_theme_default_font_size();
		const Vector2 at(p_overlay->get_size().x * 0.5f - font->get_string_size(t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x * 0.5f, p_overlay->get_size().y - 12.0f);
		p_overlay->draw_string(font, at + Vector2(1, 1), t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color(0, 0, 0, 0.8f));
		p_overlay->draw_string(font, at, t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, overlap_count ? Color(1.0f, 0.6f, 0.5f) : Color(0.7f, 1.0f, 0.7f));
	}
}
