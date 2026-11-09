#include "hw_cutter.h"

#include "hewn_mesh.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cmath>

using namespace godot;

std::vector<HewnCutter3D *> HewnCutter3D::all;
bool HewnCutter3D::show_shapes = true;

void HewnCutter3D::set_show_shapes(bool p_on) {
	show_shapes = p_on;
	for (HewnCutter3D *c : all) {
		if (c->ghost) {
			c->ghost->set_visible(c->custom.empty() && show_shapes);
		}
		c->update_gizmos();
	}
}

HewnCutter3D::HewnCutter3D() {}

HewnCutter3D::~HewnCutter3D() {
	all.erase(std::remove(all.begin(), all.end(), this), all.end());
}

void HewnCutter3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (std::find(all.begin(), all.end(), this) == all.end()) {
				all.push_back(this);
			}
			set_notify_transform(true);
			_update_ghost();
			_changed();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			all.erase(std::remove(all.begin(), all.end(), this), all.end());
			// the meshes it cut get their surface back
			for (const ObjectID &id : reached) {
				if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(id))) {
					m->_cuts_changed();
				}
			}
			reached.clear();
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED:
		case NOTIFICATION_VISIBILITY_CHANGED: {
			_changed();
		} break;
	}
}

void HewnCutter3D::_changed() {
	revision++;
	update_gizmos();
	queue_lining();
	if (!is_inside_tree()) {
		return;
	}
	const AABB b = world_bounds();
	std::vector<ObjectID> now;
	for (HewnMesh3D *m : HewnMesh3D::all_meshes) {
		const bool hit = m->is_inside_tree() && cuts(m);
		bool before = false;
		for (const ObjectID &id : reached) {
			before = before || id == ObjectID(m->get_instance_id());
		}
		if (hit) {
			now.push_back(ObjectID(m->get_instance_id()));
		}
		if (hit || before) {
			m->_cuts_changed();
		}
	}
	reached = now;
	last_bounds = b;
}

void HewnCutter3D::_update_ghost() {
	if (!Engine::get_singleton()->is_editor_hint() || !is_inside_tree()) {
		return;
	}
	if (!ghost) {
		ghost = memnew(MeshInstance3D);
		ghost->set_name("_CutterGhost");
		ghost->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		Ref<StandardMaterial3D> m;
		m.instantiate();
		m->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		m->set_depth_draw_mode(BaseMaterial3D::DEPTH_DRAW_DISABLED);
		m->set_albedo(Color(1.0f, 0.3f, 0.2f, 0.12f));
		ghost->set_material_override(m);
		add_child(ghost, false, INTERNAL_MODE_BACK);
	}
	// the outline's faces
	PackedVector3Array v;
	for (const std::vector<Vector3> &poly : local_outline()) {
		for (size_t i = 1; i + 1 < poly.size(); i++) {
			v.push_back(poly[0]);
			v.push_back(poly[i]);
			v.push_back(poly[i + 1]);
		}
	}
	Ref<ArrayMesh> am;
	am.instantiate();
	if (!v.is_empty()) {
		Array arr;
		arr.resize(Mesh::ARRAY_MAX);
		arr[Mesh::ARRAY_VERTEX] = v;
		am->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr);
	}
	ghost->set_mesh(am);
	const Vector3 sz = shape == SHAPE_PRISM ? Vector3(1, 1, 1) : size;
	ghost->set_scale(Vector3(MAX(0.01f, 1.0f - 0.006f / MAX(sz.x, 0.01f)), MAX(0.01f, 1.0f - 0.006f / MAX(sz.y, 0.01f)), MAX(0.01f, 1.0f - 0.006f / MAX(sz.z, 0.01f))));
	ghost->set_visible(custom.empty() && show_shapes); // (the stairs' headroom is not shown)
}

namespace {

// The outline (local XZ, counter-clockwise seen from +Y) and the height range of the prism.
void outline_of(int shape, const Vector3 &size, int sides, const PackedVector2Array &points, std::vector<Vector2> &r_out, float &r_y0, float &r_y1) {
	r_out.clear();
	const float hx = size.x * 0.5f, hz = size.z * 0.5f;
	r_y0 = -size.y * 0.5f;
	r_y1 = size.y * 0.5f;
	if (shape == HewnCutter3D::SHAPE_CYLINDER) {
		for (int i = 0; i < sides; i++) {
			const float a = 6.28318530718f * i / sides;
			r_out.push_back(Vector2(std::cos(a) * hx, std::sin(a) * hz));
		}
	} else if (shape == HewnCutter3D::SHAPE_PRISM && points.size() >= 3) {
		for (int64_t i = 0; i < points.size(); i++) {
			r_out.push_back(points[i]);
		}
		r_y0 = 0.0f;
		r_y1 = size.y;
	} else {
		r_out = { Vector2(-hx, -hz), Vector2(hx, -hz), Vector2(hx, hz), Vector2(-hx, hz) };
	}
	float area = 0.0f;
	for (size_t i = 0; i < r_out.size(); i++) {
		area += r_out[i].cross(r_out[(i + 1) % r_out.size()]);
	}
	if (area < 0.0f) {
		std::reverse(r_out.begin(), r_out.end());
	}
}

bool convex(const std::vector<Vector2> &p) {
	for (size_t i = 0; i < p.size(); i++) {
		if ((p[(i + 1) % p.size()] - p[i]).cross(p[(i + 2) % p.size()] - p[(i + 1) % p.size()]) < -1e-9f) {
			return false;
		}
	}
	return true;
}

// The convex volume of the planes (n.p <= d) as a closed mesh: its corners where three planes meet inside all.
hewn::Mesh polytope(const std::vector<hewn::ops::CutPlane> &planes) {
	hewn::Mesh m;
	const int np = (int)planes.size();
	std::vector<std::vector<int>> on(np);
	for (int i = 0; i < np; i++) {
		for (int j = i + 1; j < np; j++) {
			for (int k = j + 1; k < np; k++) {
				const Vector3 &a = planes[i].n, &b = planes[j].n, &c = planes[k].n;
				const float det = a.dot(b.cross(c));
				if (std::fabs(det) < 1e-7f) {
					continue;
				}
				const Vector3 x = (b.cross(c) * planes[i].d + c.cross(a) * planes[j].d + a.cross(b) * planes[k].d) / det;
				bool in = true;
				for (const hewn::ops::CutPlane &p : planes) {
					if (p.n.dot(x) > p.d + 1e-4f) {
						in = false;
						break;
					}
				}
				if (!in) {
					continue;
				}
				int id = -1;
				for (int v = 0; v < (int)m.pos.size(); v++) {
					if (m.pos[v].distance_to(x) < 1e-5f) {
						id = v;
						break;
					}
				}
				if (id < 0) {
					id = m.add_vertex(x);
				}
				for (int q = 0; q < np; q++) {
					if (std::fabs(planes[q].n.dot(x) - planes[q].d) < 1e-4f && std::find(on[q].begin(), on[q].end(), id) == on[q].end()) {
						on[q].push_back(id);
					}
				}
			}
		}
	}
	for (int q = 0; q < np; q++) {
		std::vector<int> &ids = on[q];
		if (ids.size() < 3) {
			continue;
		}
		Vector3 c;
		for (int id : ids) {
			c += m.pos[id];
		}
		c /= (float)ids.size();
		const Vector3 n = planes[q].n;
		const Vector3 u = (std::fabs(n.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).cross(n).normalized();
		const Vector3 v = n.cross(u);
		// counter-clockwise seen from outside (along n)
		std::sort(ids.begin(), ids.end(), [&](int x, int y) {
			const Vector3 dx = m.pos[x] - c, dy = m.pos[y] - c;
			return std::atan2(dx.dot(v), dx.dot(u)) < std::atan2(dy.dot(v), dy.dot(u));
		});
		hewn::Face f = hewn::make_face(ids);
		if (hewn::face_normal(m, f).dot(n) < 0.0f) {
			f.reverse();
		}
		m.faces.push_back(f);
	}
	return m;
}

bool inside_part(const std::vector<hewn::ops::CutPlane> &part, const Vector3 &p) {
	for (const hewn::ops::CutPlane &pl : part) {
		if (pl.n.dot(p) > pl.d) {
			return false;
		}
	}
	return true;
}
} // namespace

// A corridor through the meshes: the cutter's own sides where they are inside a mesh's solid (turned into the hole),
// whatever shape the holes have. Each side is cut by the planes of the meshes' triangles that cross it, each piece
// kept when the solid is on both sides of it (rays: odd crossings of the uncut meshes). Closed meshes turned out
// are solid inside; rooms (turned in) make the space outside them solid. False: a mesh that is not closed (the
// corridor between the two holes then).
static bool csg_corridor(const std::vector<HewnMesh3D *> &p_meshes, const std::vector<std::vector<hewn::ops::CutPlane>> &p_parts, int p_material, hewn::Mesh &r_out) {
	struct Tri {
		Vector3 a, b, c, n;
		float d;
		AABB box;
	};
	struct Solid {
		std::vector<Tri> tris;
		AABB box;
		bool room = false;
	};
	std::vector<Solid> solids;
	std::vector<Vector3> pts;
	std::vector<int> tr;
	for (HewnMesh3D *n : p_meshes) {
		hewn::Mesh m;
		hewn::ops::append(m, n->get_data(), n->get_global_transform(), 0);
		hewn::weld_by_distance(m, 1e-5f);
		std::unordered_map<uint64_t, int> uses; // (closed: each edge between two faces)
		for (const hewn::Face &F : m.faces) {
			for (int i = 0; i < F.size(); i++) {
				uses[hewn::ekey(F.v[i], F.v[(i + 1) % F.size()])]++;
			}
		}
		for (const auto &kv : uses) {
			if (kv.second != 2) {
				return false;
			}
		}
		Solid sd;
		float vol = 0.0f;
		bool first = true;
		for (const hewn::Face &F : m.faces) {
			if (F.size() < 3) {
				continue;
			}
			for (int i = 1; i + 1 < F.size(); i++) {
				vol += m.pos[F.v[0]].dot(m.pos[F.v[i]].cross(m.pos[F.v[i + 1]]));
			}
			hewn::face_points(m, F, pts);
			const Vector3 fn = hewn::face_normal(m, F);
			tr.clear();
			hewn::triangulate(pts, fn, tr);
			for (size_t k = 0; k + 2 < tr.size(); k += 3) {
				Tri t{ pts[tr[k]], pts[tr[k + 1]], pts[tr[k + 2]], Vector3(), 0.0f, AABB() };
				t.n = (t.b - t.a).cross(t.c - t.a);
				if (t.n.length_squared() < 1e-14f) {
					continue;
				}
				t.n.normalize();
				t.d = t.n.dot(t.a);
				t.box = AABB(t.a, Vector3()).expand(t.b).expand(t.c);
				sd.box = first ? t.box : sd.box.merge(t.box);
				first = false;
				sd.tris.push_back(t);
			}
		}
		sd.room = vol < 0.0f;
		solids.push_back(sd);
	}
	bool any_room = false;
	for (const Solid &sd : solids) {
		any_room = any_room || sd.room;
	}
	// inside a closed mesh: odd crossings along most of three rays
	auto inside = [](const Solid &sd, const Vector3 &p) {
		if (!sd.box.grow(1e-3f).has_point(p)) {
			return false;
		}
		static const Vector3 dirs[3] = { Vector3(0.5377f, 0.8164f, 0.2097f).normalized(), Vector3(-0.7071f, 0.1233f, 0.6963f).normalized(),
			Vector3(0.1031f, -0.4194f, -0.9019f).normalized() };
		int votes = 0;
		for (const Vector3 &d : dirs) {
			int hits = 0;
			for (const Tri &t : sd.tris) {
				const Vector3 e1 = t.b - t.a, e2 = t.c - t.a;
				const Vector3 pv = d.cross(e2);
				const float det = e1.dot(pv);
				if (std::fabs(det) < 1e-12f) {
					continue;
				}
				const float id = 1.0f / det;
				const Vector3 tv = p - t.a;
				const float u = tv.dot(pv) * id;
				if (u < 0.0f || u > 1.0f) {
					continue;
				}
				const Vector3 qv = tv.cross(e1);
				const float w = d.dot(qv) * id;
				if (w < 0.0f || u + w > 1.0f) {
					continue;
				}
				if (e2.dot(qv) * id > 0.0f) {
					hits++;
				}
			}
			votes += hits & 1;
		}
		return votes >= 2;
	};
	auto solid_at = [&](const Vector3 &p) {
		bool in_room = false;
		for (const Solid &sd : solids) {
			const bool in = inside(sd, p);
			if (in && !sd.room) {
				return true;
			}
			in_room = in_room || (in && sd.room);
		}
		return any_room && !in_room;
	};
	// a convex polygon cut by a plane (n.p = d): its two sides
	auto split = [](const std::vector<Vector3> &poly, const Vector3 &n, float d, std::vector<Vector3> &r_front, std::vector<Vector3> &r_back) {
		r_front.clear();
		r_back.clear();
		const size_t k = poly.size();
		for (size_t i = 0; i < k; i++) {
			const Vector3 &p = poly[i], &q = poly[(i + 1) % k];
			const float dp = n.dot(p) - d, dq = n.dot(q) - d;
			if (dp >= 0.0f) {
				r_front.push_back(p);
			}
			if (dp <= 0.0f) {
				r_back.push_back(p);
			}
			if ((dp > 0.0f && dq < 0.0f) || (dp < 0.0f && dq > 0.0f)) {
				const Vector3 x = p.lerp(q, dp / (dp - dq));
				r_front.push_back(x);
				r_back.push_back(x);
			}
		}
	};
	const float eps = 1e-4f;
	std::vector<Vector3> fr, bk;
	for (size_t pi = 0; pi < p_parts.size(); pi++) {
		const hewn::Mesh pv = polytope(p_parts[pi]);
		for (const hewn::Face &F : pv.faces) {
			if (F.size() < 3) {
				continue;
			}
			hewn::face_points(pv, F, pts);
			const Vector3 fn = hewn::face_normal(pv, F);
			const float fd = fn.dot(pts[0]);
			AABB fb(pts[0], Vector3());
			for (const Vector3 &q : pts) {
				fb.expand_to(q);
			}
			fb = fb.grow(eps);
			std::vector<std::vector<Vector3>> frags = { pts };
			for (const Solid &sd : solids) {
				if (!sd.box.intersects(fb)) {
					continue;
				}
				for (const Tri &t : sd.tris) {
					if (!t.box.grow(eps).intersects(fb) || std::fabs(t.n.dot(fn)) > 0.9999f) {
						continue;
					}
					// (the triangle crosses this side's plane)
					const float da = fn.dot(t.a) - fd, db = fn.dot(t.b) - fd, dc = fn.dot(t.c) - fd;
					if ((da > eps && db > eps && dc > eps) || (da < -eps && db < -eps && dc < -eps)) {
						continue;
					}
					std::vector<std::vector<Vector3>> next;
					for (const std::vector<Vector3> &g : frags) {
						split(g, t.n, t.d, fr, bk);
						bool cut = false;
						if (fr.size() >= 3 && bk.size() >= 3) {
							// (both pieces with an area: a real cut)
							float af = 0.0f, ab = 0.0f;
							for (size_t i = 1; i + 1 < fr.size(); i++) {
								af += (fr[i] - fr[0]).cross(fr[i + 1] - fr[0]).length();
							}
							for (size_t i = 1; i + 1 < bk.size(); i++) {
								ab += (bk[i] - bk[0]).cross(bk[i + 1] - bk[0]).length();
							}
							cut = af > 1e-9f && ab > 1e-9f;
						}
						if (cut) {
							next.push_back(fr);
							next.push_back(bk);
						} else {
							next.push_back(g);
						}
					}
					frags.swap(next);
					if (frags.size() > 4096) {
						break;
					}
				}
			}
			for (const std::vector<Vector3> &g : frags) {
				Vector3 c;
				for (const Vector3 &q : g) {
					c += q;
				}
				c /= (float)g.size();
				// (a side between two parts of the cutter: inside the other one, no wall there)
				bool between = false;
				for (size_t pj = 0; pj < p_parts.size() && !between; pj++) {
					between = pj != pi && inside_part(p_parts[pj], c + fn * 1e-3f);
				}
				// the solid on both sides of it (one side only: the piece lies on the mesh's own surface)
				if (between || !solid_at(c + fn * 1e-3f) || !solid_at(c - fn * 1e-3f)) {
					continue;
				}
				// (on a surface that is there already, facing into the hole: a floor the cutter stands on)
				bool covered = false;
				for (const Solid &sd : solids) {
					if (covered || !sd.box.grow(eps).has_point(c)) {
						continue;
					}
					for (const Tri &t : sd.tris) {
						if (t.n.dot(fn) > -0.9999f || std::fabs(t.n.dot(c) - t.d) > eps || !t.box.grow(eps).has_point(c)) {
							continue;
						}
						const Vector3 e0 = t.b - t.a, e1 = t.c - t.a, w = c - t.a;
						const float d00 = e0.dot(e0), d01 = e0.dot(e1), d11 = e1.dot(e1), d20 = w.dot(e0), d21 = w.dot(e1);
						const float den = d00 * d11 - d01 * d01;
						if (std::fabs(den) < 1e-14f) {
							continue;
						}
						const float bv = (d11 * d20 - d01 * d21) / den, bw = (d00 * d21 - d01 * d20) / den;
						if (bv >= -1e-4f && bw >= -1e-4f && bv + bw <= 1.0001f) {
							covered = true;
							break;
						}
					}
				}
				if (covered) {
					continue;
				}
				std::vector<int> ids;
				for (auto it = g.rbegin(); it != g.rend(); ++it) {
					ids.push_back(r_out.add_vertex(*it)); // (turned into the hole)
				}
				hewn::Face f = hewn::make_face(ids);
				f.material = p_material;
				f.role = hewn::ROLE_SIDE;
				r_out.faces.push_back(f);
			}
		}
	}
	hewn::weld_by_distance(r_out, 1e-5f);
	hewn::ops::limited_dissolve(r_out, {}, 0.5f); // (the pieces of one side back together)
	return true;
}

std::vector<std::vector<Vector3>> HewnCutter3D::local_outline() const {
	std::vector<std::vector<Vector3>> out;
	if (!custom.empty()) {
		return out;
	}
	std::vector<Vector2> o;
	float y0, y1;
	outline_of(shape, size, sides, points, o, y0, y1);
	if (o.size() < 3) {
		return out;
	}
	std::vector<Vector3> bot, top;
	for (const Vector2 &q : o) {
		bot.push_back(Vector3(q.x, y0, q.y));
		top.push_back(Vector3(q.x, y1, q.y));
	}
	out.push_back(bot);
	out.push_back(top);
	for (size_t i = 0; i < o.size(); i++) {
		const size_t j = (i + 1) % o.size();
		out.push_back({ bot[i], bot[j], top[j], top[i] });
	}
	return out;
}

std::vector<std::vector<hewn::ops::CutPlane>> HewnCutter3D::world_parts() const {
	std::vector<std::vector<hewn::ops::CutPlane>> local = custom;
	if (local.empty()) {
		std::vector<Vector2> o;
		float y0, y1;
		outline_of(shape, size, sides, points, o, y0, y1);
		if (o.size() < 3) {
			return local;
		}
		// convex pieces: the outline, or its triangles
		std::vector<std::vector<Vector2>> pieces;
		if (convex(o)) {
			pieces.push_back(o);
		} else {
			std::vector<Vector3> p3;
			for (const Vector2 &q : o) {
				p3.push_back(Vector3(q.x, 0.0f, -q.y));
			}
			std::vector<int> tris;
			hewn::triangulate(p3, Vector3(0, 1, 0), tris);
			for (size_t t = 0; t + 2 < tris.size(); t += 3) {
				std::vector<Vector2> tri = { o[tris[t]], o[tris[t + 1]], o[tris[t + 2]] };
				if ((tri[1] - tri[0]).cross(tri[2] - tri[0]) < 0.0f) {
					std::swap(tri[1], tri[2]);
				}
				pieces.push_back(tri);
			}
		}
		const float margin = 1e-4f;
		for (const std::vector<Vector2> &pc : pieces) {
			std::vector<hewn::ops::CutPlane> planes;
			for (size_t i = 0; i < pc.size(); i++) {
				const Vector2 a = pc[i], b = pc[(i + 1) % pc.size()];
				const Vector2 e = b - a;
				if (e.length() < 1e-7f) {
					continue;
				}
				// outward in XZ (counter-clockwise seen from +Y: the outside is on the right of a -> b in (x, z))
				const Vector3 n = Vector3(e.y, 0.0f, -e.x).normalized();
				planes.push_back({ n, n.dot(Vector3(a.x, 0.0f, a.y)), keep_flush });
			}
			const bool end_flush = keep_flush && !cut_ends;
			const float em = cut_ends ? margin : 0.0f;
			planes.push_back({ Vector3(0, 1, 0), y1 + em, end_flush });
			planes.push_back({ Vector3(0, -1, 0), -y0 + em, end_flush });
			local.push_back(planes);
		}
	}
	const Transform3D g = get_global_transform();
	const Basis nb = g.basis.inverse().transposed();
	for (std::vector<hewn::ops::CutPlane> &part : local) {
		for (hewn::ops::CutPlane &p : part) {
			const Vector3 pt = g.xform(p.n * p.d);
			p.n = nb.xform(p.n).normalized();
			p.d = p.n.dot(pt);
		}
	}
	return local;
}

AABB HewnCutter3D::world_bounds() const {
	const Transform3D g = is_inside_tree() ? get_global_transform() : get_transform();
	AABB b;
	bool first = true;
	auto add = [&](const Vector3 &p) {
		if (first) {
			b = AABB(g.xform(p), Vector3());
			first = false;
		} else {
			b.expand_to(g.xform(p));
		}
	};
	if (!custom.empty()) {
		// (the parts' corners are not known: the planes' points, generously)
		for (const std::vector<hewn::ops::CutPlane> &part : custom) {
			for (const hewn::ops::CutPlane &p : part) {
				add(p.n * p.d);
			}
		}
		return b.grow(1.0f);
	}
	for (const std::vector<Vector3> &poly : local_outline()) {
		for (const Vector3 &p : poly) {
			add(p);
		}
	}
	return b;
}

bool HewnCutter3D::cuts(const HewnMesh3D *p_mesh) const {
	if (!active || !is_inside_tree() || !is_visible_in_tree() || !p_mesh || !p_mesh->is_inside_tree() || p_mesh->is_boolean_cutter() || p_mesh->is_brush()) {
		return false;
	}
	// its own lining (a corridor / alcove made with W) is not cut
	if (is_ancestor_of(const_cast<HewnMesh3D *>(p_mesh))) {
		return false;
	}
	// what holds it (the stairs that made it, the scene it is in, or any parent with Skip Parent) is not cut
	Node *holder = get_parent();
	if (holder && (skip_parent || Object::cast_to<HewnMesh3D>(holder) || (!holder->get_scene_file_path().is_empty() && holder->get_owner()))) {
		// (a scene's root holds everything: only itself is left alone, not the whole level)
		const bool scene_root = !holder->get_owner() || holder == holder->get_tree()->get_edited_scene_root() || holder == holder->get_tree()->get_current_scene();
		if (holder == p_mesh || (!scene_root && holder->is_ancestor_of(const_cast<HewnMesh3D *>(p_mesh)))) {
			return false;
		}
	}
	if (p_mesh->get_world_3d() != get_world_3d()) {
		return false; // (another world: an exported copy of a scene, a preview)
	}
	const AABB mb = p_mesh->get_global_transform().xform(p_mesh->get_data_aabb());
	const AABB wb = world_bounds();
	if (!mb.grow(1e-3f).intersects(wb)) {
		return false;
	}
	// only touching it (a floor the cutter stands on, a wall beside it): not cut, it would get a sliver of a hole and the
	// lining a face over its surface. A flat mesh (a plane) on an axis: cut when it is well inside the cutter there.
	const AABB in = mb.intersection(wb);
	bool touching = false;
	for (int a = 0; a < 3; a++) {
		if (mb.size[a] < 1e-3f) {
			touching = touching || mb.position[a] < wb.position[a] + 1e-3f || mb.position[a] + mb.size[a] > wb.position[a] + wb.size[a] - 1e-3f;
		} else {
			touching = touching || in.size[a] < 1e-3f;
		}
	}
	if (!touching) {
		return true;
	}
	// (a room, turned inside out: its solid is outside its bounds, a cutter against its wall from outside cuts it)
	const hewn::Mesh &m = p_mesh->get_data();
	float vol = 0.0f;
	for (const hewn::Face &F : m.faces) {
		for (int i = 1; i + 1 < F.size(); i++) {
			vol += m.pos[F.v[0]].dot(m.pos[F.v[i]].cross(m.pos[F.v[i + 1]]));
		}
	}
	return vol < 0.0f;
}

TypedArray<Node> HewnCutter3D::apply() {
	TypedArray<Node> out;
	const std::vector<std::vector<hewn::ops::CutPlane>> parts = world_parts();
	for (HewnMesh3D *m : HewnMesh3D::all_meshes) {
		if (!cuts(m)) {
			continue;
		}
		const Transform3D inv = m->get_global_transform().affine_inverse();
		const Basis nb = m->get_global_transform().basis.transposed();
		std::vector<std::vector<hewn::ops::CutPlane>> local = parts;
		for (std::vector<hewn::ops::CutPlane> &part : local) {
			for (hewn::ops::CutPlane &p : part) {
				const Vector3 pt = inv.xform(p.n * p.d);
				p.n = nb.xform(p.n).normalized();
				p.d = p.n.dot(pt);
			}
		}
		hewn::ops::cut_volumes(m->get_data(), {}, local);
		hewn::remove_unused_vertices(m->get_data());
		out.push_back(m);
	}
	// the lining as a mesh of its own next to the cutter (kept when the cutter is deleted)
	if (lining != LINING_NONE && lining_mesh && !lining_mesh->get_data().faces.empty() && get_parent()) {
		HewnMesh3D *keep = memnew(HewnMesh3D);
		keep->set_name(lining == LINING_ALCOVE ? "Alcove" : "Corridor");
		keep->set_materials(lining_mesh->get_materials());
		keep->get_data() = lining_mesh->get_data();
		get_parent()->add_child(keep, true);
		keep->set_owner(get_owner() ? get_owner() : get_parent());
		keep->set_transform(get_transform());
		keep->commit(true);
		out.push_back(keep);
	}
	set_active(false); // (its cut is in the meshes now)
	for (int64_t i = 0; i < out.size(); i++) {
		Object::cast_to<HewnMesh3D>(out[i])->commit();
	}
	return out;
}

void HewnCutter3D::queue_lining() {
	if (lining_queued || (lining == LINING_NONE && !lining_mesh)) {
		return;
	}
	lining_queued = true;
	callable_mp(this, &HewnCutter3D::_update_lining).call_deferred();
}

// The corridor between its holes (or the alcove behind its hole) from the meshes it cuts now, in its own
// space; it does not cut it, it moves with it and follows the walls.
void HewnCutter3D::_update_lining() {
	lining_queued = false;
	if (lining == LINING_NONE || !is_inside_tree() || !active) {
		if (lining_mesh) {
			lining_mesh->get_data() = hewn::Mesh();
			lining_mesh->commit(false);
			lining_mesh->set_visible(false);
		}
		return;
	}
	hewn::Mesh all;
	TypedArray<Material> mats;
	for (HewnMesh3D *n : HewnMesh3D::all_meshes) {
		if (n == lining_mesh || n->has_meta("_hw_lining") || !cuts(n)) {
			continue;
		}
		const int offset = (int)mats.size();
		const TypedArray<Material> nm = n->get_materials();
		for (int64_t i = 0; i < nm.size(); i++) {
			mats.push_back(nm[i]);
		}
		if (nm.is_empty()) {
			mats.push_back(Ref<Material>());
		}
		hewn::ops::append(all, n->get_data(), n->get_global_transform(), offset);
	}
	std::vector<int> made;
	hewn::Mesh corridor; // (made from the cutter's sides: world space)
	bool by_sides = false;
	if (!all.faces.empty()) {
		const std::vector<std::vector<hewn::ops::CutPlane>> wp = world_parts();
		const std::vector<std::vector<int>> loops = hewn::ops::cut_volumes(all, {}, wp);
		if ((loops.size() >= 2 && lining == LINING_AUTO) || (lining == LINING_CORRIDOR && !loops.empty())) {
			// the material of the faces around the holes
			std::unordered_map<int, int> count;
			std::unordered_set<int> lv;
			for (const std::vector<int> &l : loops) {
				lv.insert(l.begin(), l.end());
			}
			for (const hewn::Face &F : all.faces) {
				for (int v : F.v) {
					if (lv.count(v)) {
						count[F.material]++;
						break;
					}
				}
			}
			int mat = 0, best = -1;
			for (const auto &kv : count) {
				if (kv.second > best) {
					best = kv.second;
					mat = kv.first;
				}
			}
			std::vector<HewnMesh3D *> cut;
			for (HewnMesh3D *n : HewnMesh3D::all_meshes) {
				if (n == lining_mesh || n->has_meta("_hw_lining") || !cuts(n)) {
					continue;
				}
				cut.push_back(n);
			}
			by_sides = csg_corridor(cut, wp, mat, corridor) && !corridor.faces.empty();
		}
		if (by_sides) {
			// (made above)
		} else if (loops.size() == 2 && lining != LINING_ALCOVE) {
			made = hewn::ops::bridge_loops(all, loops[0], loops[1]);
		} else if (!loops.empty() && lining != LINING_CORRIDOR) {
			std::vector<hewn::ops::CutPlane> planes;
			for (const std::vector<hewn::ops::CutPlane> &part : wp) {
				planes.insert(planes.end(), part.begin(), part.end());
			}
			made = hewn::ops::alcove(all, loops[0], lining_depth, &planes); // (along the cutter, to its far side)
		}
	}
	if (!lining_mesh) {
		lining_mesh = memnew(HewnMesh3D);
		lining_mesh->set_name("_Lining");
		lining_mesh->set_meta("_hw_lining", true);
		add_child(lining_mesh, false, INTERNAL_MODE_BACK);
	}
	hewn::Mesh lm = by_sides ? corridor : made.empty() ? hewn::Mesh() : hewn::ops::extract_faces(all, made);
	hewn::ops::transform(lm, get_global_transform().affine_inverse());
	lining_mesh->set_materials(mats);
	lining_mesh->get_data() = lm;
	lining_mesh->commit(false);
	lining_mesh->set_visible(!lm.faces.empty());
}

void HewnCutter3D::set_lining(int p_lining) {
	lining = CLAMP(p_lining, 0, 3);
	queue_lining();
}

void HewnCutter3D::set_lining_depth(float p_depth) {
	lining_depth = MAX(0.001f, p_depth);
	queue_lining();
}

void HewnCutter3D::set_custom_parts(const std::vector<std::vector<hewn::ops::CutPlane>> &p_parts) {
	custom = p_parts;
	_update_ghost();
	_changed();
}

#define HW_CUTTER_SET(m_type, m_name, m_expr)        \
	void HewnCutter3D::set_##m_name(m_type p_v) { \
		m_name = m_expr;                               \
		_update_ghost();                               \
		_changed();                                    \
	}
HW_CUTTER_SET(int, shape, CLAMP(p_v, 0, 2))
HW_CUTTER_SET(const Vector3 &, size, p_v.max(Vector3(0.001f, 0.001f, 0.001f)))
HW_CUTTER_SET(int, sides, CLAMP(p_v, 3, 128))
HW_CUTTER_SET(const PackedVector2Array &, points, p_v)
HW_CUTTER_SET(bool, keep_flush, p_v)
HW_CUTTER_SET(bool, cut_ends, p_v)
HW_CUTTER_SET(bool, skip_parent, p_v)
HW_CUTTER_SET(bool, active, p_v)

void HewnCutter3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_shape", "shape"), &HewnCutter3D::set_shape);
	ClassDB::bind_method(D_METHOD("get_shape"), &HewnCutter3D::get_shape);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &HewnCutter3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &HewnCutter3D::get_size);
	ClassDB::bind_method(D_METHOD("set_sides", "sides"), &HewnCutter3D::set_sides);
	ClassDB::bind_method(D_METHOD("get_sides"), &HewnCutter3D::get_sides);
	ClassDB::bind_method(D_METHOD("set_points", "points"), &HewnCutter3D::set_points);
	ClassDB::bind_method(D_METHOD("get_points"), &HewnCutter3D::get_points);
	ClassDB::bind_method(D_METHOD("set_keep_flush", "on"), &HewnCutter3D::set_keep_flush);
	ClassDB::bind_method(D_METHOD("get_keep_flush"), &HewnCutter3D::get_keep_flush);
	ClassDB::bind_method(D_METHOD("set_cut_ends", "on"), &HewnCutter3D::set_cut_ends);
	ClassDB::bind_method(D_METHOD("get_cut_ends"), &HewnCutter3D::get_cut_ends);
	ClassDB::bind_method(D_METHOD("set_skip_parent", "on"), &HewnCutter3D::set_skip_parent);
	ClassDB::bind_method(D_METHOD("get_skip_parent"), &HewnCutter3D::get_skip_parent);
	ClassDB::bind_method(D_METHOD("set_active", "on"), &HewnCutter3D::set_active);
	ClassDB::bind_method(D_METHOD("is_active"), &HewnCutter3D::is_active);
	ClassDB::bind_method(D_METHOD("apply"), &HewnCutter3D::apply);
	ClassDB::bind_method(D_METHOD("set_lining", "lining"), &HewnCutter3D::set_lining);
	ClassDB::bind_method(D_METHOD("get_lining"), &HewnCutter3D::get_lining);
	ClassDB::bind_method(D_METHOD("set_lining_depth", "depth"), &HewnCutter3D::set_lining_depth);
	ClassDB::bind_method(D_METHOD("get_lining_depth"), &HewnCutter3D::get_lining_depth);
	ClassDB::bind_method(D_METHOD("get_lining_mesh"), &HewnCutter3D::get_lining_mesh);
	ClassDB::bind_method(D_METHOD("update_lining"), &HewnCutter3D::_update_lining);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shape", PROPERTY_HINT_ENUM, "Box,Cylinder,Prism"), "set_shape", "get_shape");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sides", PROPERTY_HINT_RANGE, "3,128,1"), "set_sides", "get_sides");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR2_ARRAY, "points"), "set_points", "get_points");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "keep_flush"), "set_keep_flush", "get_keep_flush");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cut_ends"), "set_cut_ends", "get_cut_ends");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "skip_parent"), "set_skip_parent", "get_skip_parent");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "active"), "set_active", "is_active");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "lining", PROPERTY_HINT_ENUM, "None,Auto,Corridor,Alcove"), "set_lining", "get_lining");
	// (the alcove goes to the cutter's far side: its size is the depth; kept for old scenes and alcoves without one)
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lining_depth", PROPERTY_HINT_RANGE, "0.001,100,0.001,or_greater,suffix:m", PROPERTY_USAGE_STORAGE), "set_lining_depth", "get_lining_depth");
	BIND_ENUM_CONSTANT(LINING_NONE);
	BIND_ENUM_CONSTANT(LINING_AUTO);
	BIND_ENUM_CONSTANT(LINING_CORRIDOR);
	BIND_ENUM_CONSTANT(LINING_ALCOVE);
	BIND_ENUM_CONSTANT(SHAPE_BOX);
	BIND_ENUM_CONSTANT(SHAPE_CYLINDER);
	BIND_ENUM_CONSTANT(SHAPE_PRISM);
}
