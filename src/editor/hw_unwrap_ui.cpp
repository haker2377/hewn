// UV unwrapping in the viewport (the U menu): seams by angle, unwrap (angle based / conformal / minimum stretch),
// Smart UV Project, lightmap pack, box / cylinder / sphere / view projections, Quadrify (a grid of quads opened
// along the selected edges' line from seam to seam), Relax. The faces' UVs in meters (as Hewn's materials scale
// them), each island set out next to the others from where the faces' UVs were.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include "../hw_unwrap.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>

#include <unordered_map>
#include <unordered_set>

using namespace godot;

bool HewnEditorPlugin::is_unwrap_op(const String &p_op) {
	return p_op == "seams_by_angle" || p_op == "unwrap_angle" || p_op == "unwrap_conformal" || p_op == "unwrap_minstretch" || p_op == "smart_project" ||
			p_op == "lightmap_unwrap" || p_op == "project_box" || p_op == "project_cylinder" || p_op == "project_sphere" || p_op == "project_view" ||
			p_op == "quadrify" || p_op == "relax_uvs";
}

namespace {
// the faces' UVs as they show now (made or projected): their lowest corner
Vector2 uv_min(HewnMesh3D *p_n, const std::vector<int> &p_faces) {
	const std::vector<std::vector<Vector2>> &uvs = p_n->get_build().face_uvs;
	Vector2 mn(1e30f, 1e30f);
	for (int f : p_faces) {
		const hewn::Face &F = p_n->get_data().faces[f];
		const std::vector<Vector2> &q = F.manual_uv && F.uv.size() == F.v.size() ? F.uv : (f < (int)uvs.size() ? uvs[f] : std::vector<Vector2>());
		for (const Vector2 &p : q) {
			mn = mn.min(p);
		}
	}
	return mn.x > 1e29f ? Vector2() : mn;
}

// the edge loop on from v (coming over the edge from `from`): at a vertex with four edges, the one across; stops at a
// seam, the border, another kind of vertex, or back where it began
std::vector<hewn::Edge> loop_on(const hewn::Mesh &m, const hewn::Topology &t, int from, int v, bool &r_closed) {
	std::vector<hewn::Edge> out;
	r_closed = false;
	const int start = from;
	for (int guard = 0; guard < 100000; guard++) {
		// v on a seam (other than the edge it came by) or on the border: the line has reached it
		bool stop = false;
		for (int e : t.vert_edges[v]) {
			const hewn::Edge &E = t.edges[e];
			auto ea = m.edge_attrs.find(hewn::ekey(E));
			if ((ea != m.edge_attrs.end() && ea->second.seam && !(E.a == from || E.b == from)) || t.edge_faces[e].size() < 2) {
				stop = true;
			}
		}
		if (stop || t.vert_edges[v].size() != 4) {
			return out;
		}
		const int came = t.find_edge(from, v);
		int next = -1;
		for (int e : t.vert_edges[v]) {
			if (e != came && !t.shares_face(e, came)) {
				next = e;
			}
		}
		if (next < 0) {
			return out;
		}
		const int w = t.edges[next].a == v ? t.edges[next].b : t.edges[next].a;
		out.push_back(hewn::Edge(v, w));
		if (w == start) {
			r_closed = true;
			return out;
		}
		from = v;
		v = w;
	}
	return out;
}
} // namespace

void HewnEditorPlugin::_unwrap_op(const String &p_op, HewnMesh3D *n) {
	hewn::Mesh &m = n->get_data();
	hewn::Selection &s = n->selection;
	hewn::Topology t;
	t.build(m);
	auto P = [&](const char *key) { return op_param(p_op, key); };
	std::vector<int> faces = mode == MODE_FACE ? s.faces : hewn::sel::faces_from_verts(m, _selected_verts(), mode == MODE_VERTEX);
	if (p_op == "seams_by_angle") {
		// the edges of the selection (all of them when nothing is): a seam where the faces bend more than the angle
		const float cos_lim = std::cos((float)P("angle") * (float)Math::PI / 180.0f);
		std::vector<hewn::Edge> edges = mode == MODE_EDGE ? s.edges : hewn::sel::edges_from_verts(m, t, _selected_verts());
		if (edges.empty()) {
			edges = t.edges;
		}
		for (const hewn::Edge &e : edges) {
			const int id = t.find_edge(e.a, e.b);
			if (id >= 0 && t.edge_faces[id].size() == 2 &&
					hewn::face_normal(m, m.faces[t.edge_faces[id][0]]).dot(hewn::face_normal(m, m.faces[t.edge_faces[id][1]])) < cos_lim) {
				m.edge_attrs[hewn::ekey(e)].seam = true;
			}
		}
		return;
	}
	if (p_op == "quadrify") {
		// the line to open along: the selected edges (a face: its side whose line runs from seam to seam), on to the
		// seams on both sides; a seam there; then each island of quads around the selection a grid
		const bool by_edge = (bool)P("by_edge");
		const bool along_v = (int)P("orientation") == 1;
		std::vector<hewn::Edge> cut;
		std::vector<int> touched;
		if (mode == MODE_EDGE) {
			cut = s.edges;
			for (const hewn::Edge &e : s.edges) {
				const int id = t.find_edge(e.a, e.b);
				if (id >= 0) {
					touched.insert(touched.end(), t.edge_faces[id].begin(), t.edge_faces[id].end());
				}
			}
		} else {
			touched = faces;
			for (int f : faces) {
				const hewn::Face &F = m.faces[f];
				for (int i = 0; i < F.size() && F.size() == 4; i++) {
					const int a = F.v[i], b = F.v[(i + 1) % 4];
					bool c1 = false, c2 = false;
					loop_on(m, t, a, b, c1);
					loop_on(m, t, b, a, c2);
					if (!c1 && !c2) {
						cut.push_back(hewn::Edge(a, b)); // (its line ends at seams: the way across the band)
						break;
					}
				}
			}
		}
		if (by_edge) {
			std::vector<hewn::Edge> all = cut;
			for (const hewn::Edge &e : cut) {
				bool closed = false;
				for (const hewn::Edge &x : loop_on(m, t, e.a, e.b, closed)) {
					all.push_back(x);
				}
				if (!closed) {
					for (const hewn::Edge &x : loop_on(m, t, e.b, e.a, closed)) {
						all.push_back(x);
					}
				}
			}
			for (const hewn::Edge &e : all) {
				m.edge_attrs[hewn::ekey(e)].seam = true;
			}
		}
		// the islands of quads (by seams) the selection is in
		std::vector<int> quads;
		for (int f = 0; f < (int)m.faces.size(); f++) {
			if (m.faces[f].size() == 4 && !m.faces[f].hidden) {
				quads.push_back(f);
			}
		}
		std::unordered_set<int> want(touched.begin(), touched.end());
		std::vector<std::vector<int>> islands;
		for (const std::vector<int> &is : hewn::unwrap::seam_islands(m, quads)) {
			bool hit = false;
			for (int f : is) {
				hit = hit || want.count(f);
			}
			if (hit) {
				islands.push_back(is);
			}
		}
		std::vector<int> all;
		for (const std::vector<int> &is : islands) {
			all.insert(all.end(), is.begin(), is.end());
		}
		const Vector2 origin = uv_min(n, all);
		hewn::unwrap::Layout lay;
		const float repeats = (float)P("repeats");
		for (const std::vector<int> &is : islands) {
			if (!hewn::unwrap::quad_grid(m, is, along_v, lay)) {
				hewn::unwrap::lscm(m, is, lay); // (not a grid: laid out as well as it can be)
			}
			if (repeats > 0.0f && !is.empty()) {
				// across the band (its shorter side) the texture that many times: in its material's tiles
				Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
				for (int f : is) {
					for (const Vector2 &p : lay[f]) {
						mn = mn.min(p);
						mx = mx.max(p);
					}
				}
				const float across = MIN(mx.x - mn.x, mx.y - mn.y);
				const Ref<BaseMaterial3D> bm = n->get_material_for_slot(m.faces[is[0]].material);
				const float tile = bm.is_valid() ? 1.0f / MAX(1e-4f, bm->get_uv1_scale().x) : 1.0f;
				const float k = across > 1e-9f ? repeats * tile / across : 1.0f;
				for (int f : is) {
					for (Vector2 &p : lay[f]) {
						p = mn + (p - mn) * k;
					}
				}
			}
		}
		hewn::unwrap::row(islands, lay, origin, 0.1f);
		PackedInt32Array laid;
		for (const auto &kv : lay) {
			m.faces[kv.first].uv = kv.second;
			m.faces[kv.first].manual_uv = true;
			m.faces[kv.first].uv_group = 0;
			laid.push_back(kv.first);
		}
		n->keep_uvs_by_hand(laid);
		return;
	}
	if (faces.empty()) {
		return;
	}
	const Vector2 origin = uv_min(n, faces);
	hewn::unwrap::Layout lay;
	std::vector<std::vector<int>> islands;
	if (p_op == "unwrap_angle" || p_op == "unwrap_conformal" || p_op == "unwrap_minstretch") {
		islands = hewn::unwrap::seam_islands(m, faces);
		for (const std::vector<int> &is : islands) {
			if (!hewn::unwrap::lscm(m, is, lay)) {
				hewn::unwrap::planar(m, is, hewn::face_normal(m, m.faces[is[0]]), lay);
			}
		}
		// (angle based: a light relax evening the angles and areas out; minimum stretch: relaxed until it holds)
		if (p_op != "unwrap_conformal") {
			hewn::unwrap::relax(m, faces, lay, p_op == "unwrap_angle" ? 4 : 60);
		}
	} else if (p_op == "smart_project") {
		islands = hewn::unwrap::smart_project(m, faces, (float)P("angle"), lay);
	} else if (p_op == "relax_uvs") {
		for (int f : faces) {
			const hewn::Face &F = m.faces[f];
			const std::vector<std::vector<Vector2>> &uvs = n->get_build().face_uvs;
			lay[f] = F.manual_uv && F.uv.size() == F.v.size() ? F.uv : (f < (int)uvs.size() ? uvs[f] : std::vector<Vector2>(F.v.size()));
		}
		hewn::unwrap::relax(m, faces, lay, MAX(1, (int)P("iterations")));
	} else if (p_op == "lightmap_unwrap") {
		std::vector<char> skip(m.faces.size(), 1);
		for (int f : faces) {
			skip[f] = 0;
		}
		std::vector<std::vector<Vector2>> uv2;
		hewn::lightmap_unwrap(m, MAX(0.001f, n->get_lightmap_texel_size()), skip, uv2);
		for (int f : faces) {
			lay[f] = uv2[f];
		}
	} else if (p_op == "project_box" || p_op == "project_cylinder" || p_op == "project_sphere") {
		// about the faces' middle, the object's own axes
		Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
		for (int f : faces) {
			for (int v : m.faces[f].v) {
				mn = mn.min(m.pos[v]);
				mx = mx.max(m.pos[v]);
			}
		}
		const Transform3D frame(Basis(), (mn + mx) * 0.5f);
		if (p_op == "project_box") {
			hewn::unwrap::project_box(m, faces, frame, lay);
		} else if (p_op == "project_cylinder") {
			hewn::unwrap::project_cylinder(m, faces, frame, lay);
		} else {
			hewn::unwrap::project_sphere(m, faces, frame, lay);
		}
	} else if (p_op == "project_view") {
		// as the view sees them: across the camera, meters, V down the screen
		Camera3D *cam = Object::cast_to<Camera3D>(ObjectDB::get_instance(op_cam));
		const Transform3D g = n->get_global_transform();
		// as the view shows them (with its perspective): where they are on the screen, in meters at the depth of the
		// faces' middle; V down the screen
		Vector3 mid;
		int cnt = 0;
		for (int f : faces) {
			for (int v : m.faces[f].v) {
				mid += g.xform(m.pos[v]);
				cnt++;
			}
		}
		mid /= (float)MAX(1, cnt);
		float k = 0.01f;
		if (cam) {
			const Vector2 c0 = cam->unproject_position(mid);
			const Vector3 side = cam->get_global_transform().basis.get_column(0).normalized();
			const float px = cam->unproject_position(mid + side).distance_to(c0); // (screen pixels of one meter there)
			k = px > 1e-6f ? 1.0f / px : 0.01f;
		}
		for (int f : faces) {
			std::vector<Vector2> l;
			for (int v : m.faces[f].v) {
				const Vector3 w = g.xform(m.pos[v]);
				if (cam && !cam->is_position_behind(w)) {
					l.push_back(cam->unproject_position(w) * k);
				} else {
					const Basis cb = cam ? cam->get_global_transform().basis : Basis();
					l.push_back(Vector2(w.dot(cb.get_column(0).normalized()), -w.dot(cb.get_column(1).normalized())));
				}
			}
			lay[f] = l;
		}
	}
	if (p_op != "relax_uvs" && p_op != "lightmap_unwrap") {
		if (islands.empty()) {
			islands = { faces };
		}
		hewn::unwrap::row(islands, lay, origin, 0.1f);
	}
	PackedInt32Array laid;
	for (const auto &kv : lay) {
		if (kv.second.size() == m.faces[kv.first].v.size()) {
			m.faces[kv.first].uv = kv.second;
			m.faces[kv.first].manual_uv = true;
			m.faces[kv.first].uv_group = 0;
			laid.push_back(kv.first);
		}
	}
	n->keep_uvs_by_hand(laid);
}
