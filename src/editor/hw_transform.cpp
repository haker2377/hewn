// Transform settings shared by the modal operations, the gizmo and the Last Operation replays:
// orientation, pivot point, snapping targets, proportional editing and auto merge.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/node3d.hpp>

#include <functional>

using namespace godot;
using namespace hw_editor;

namespace {

Basis orthonormal(const Basis &b) {
	Basis o = b.orthonormalized();
	if (!std::isfinite(o[0][0]) || o.determinant() == 0.0f) {
		return Basis();
	}
	return o;
}

} // namespace

// ------------------------------------------------------------------ orientation

// Columns are the X / Y / Z axes of the orientation, in world space.
Basis HewnEditorPlugin::_orientation_basis(int p_orient, Camera3D *p_cam) const {
	HewnMesh3D *n = get_edited();
	switch (p_orient) {
		case HewnDock::ORIENT_LOCAL:
		case HewnDock::ORIENT_GIMBAL: // no per-element rotations: same axes as the object
			return n ? orthonormal(n->get_global_transform().basis) : Basis();
		case HewnDock::ORIENT_VIEW:
			return p_cam ? orthonormal(p_cam->get_global_transform().basis) : Basis();
		case HewnDock::ORIENT_CURSOR:
			return orthonormal(cursor_basis);
		case HewnDock::ORIENT_PARENT: {
			Node3D *parent = n ? Object::cast_to<Node3D>(n->get_parent()) : nullptr;
			return parent ? orthonormal(parent->get_global_transform().basis) : Basis();
		}
		case HewnDock::ORIENT_NORMAL: {
			if (!n || mode == MODE_OBJECT) {
				return n ? orthonormal(n->get_global_transform().basis) : Basis();
			}
			// Z along the selection normal, Y along the first selected edge when there is one.
			const hewn::Mesh &m = n->get_data();
			const hewn::Selection &s = n->selection;
			const Transform3D g = n->get_global_transform();
			const Basis nb = g.basis.inverse().transposed();
			Vector3 z, y;
			if (mode == MODE_FACE) {
				for (int f : s.faces) {
					z += hewn::face_normal_raw(m, m.faces[f]);
				}
			} else {
				const std::vector<Vector3> vn = hewn::vertex_normals(m);
				for (int v : _selected_verts()) {
					z += vn[v];
				}
				if (mode == MODE_EDGE && !s.edges.empty()) {
					y = g.basis.xform(m.pos[s.edges.back().b] - m.pos[s.edges.back().a]);
				}
			}
			z = nb.xform(z);
			if (z.length() < 1e-9f) {
				return Basis();
			}
			z.normalize();
			y = y - z * y.dot(z);
			if (y.length() < 1e-6f) {
				Vector3 t;
				hewn::plane_basis(z, t, y);
			}
			y.normalize();
			const Vector3 x = y.cross(z).normalized();
			Basis b;
			b.set_column(0, x);
			b.set_column(1, y);
			b.set_column(2, z);
			return b;
		}
		default: {
			const int c = p_orient - HewnDock::ORIENT_CUSTOM;
			return c >= 0 && c < (int)dock->custom_orients.size() ? dock->custom_orients[c] : Basis();
		}
	}
}

// "+" in the orientation menu: the Normal orientation of the selection, kept under a name.
void HewnEditorPlugin::create_custom_orientation() {
	HewnMesh3D *n = get_edited();
	String name = "Orientation";
	Basis b;
	if (n && mode != MODE_OBJECT && !_selected_verts().empty()) {
		b = _orientation_basis(HewnDock::ORIENT_NORMAL, nullptr);
		name = has_mode(MODE_FACE) && !n->selection.faces.empty() ? "Face" : !n->selection.edges.empty() ? "Edge" : "Vertex";
	} else if (n) {
		b = _orientation_basis(HewnDock::ORIENT_LOCAL, nullptr);
		name = n->get_name();
	} else {
		return;
	}
	dock->add_custom_orientation(name, b);
}

// ------------------------------------------------------------------ pivot

Vector3 HewnEditorPlugin::_pivot_point(int p_pivot, const std::vector<int> &p_verts) const {
	HewnMesh3D *n = get_edited();
	if (!n || p_verts.empty()) {
		return cursor_pos;
	}
	const Transform3D g = n->get_global_transform();
	const hewn::Mesh &m = n->get_data();
	const hewn::Selection &s = n->selection;
	if (p_pivot == HewnDock::PIVOT_CURSOR) {
		return cursor_pos;
	}
	if (p_pivot == HewnDock::PIVOT_BBOX) {
		Vector3 mn = g.xform(m.pos[p_verts[0]]), mx = mn;
		for (int v : p_verts) {
			const Vector3 w = g.xform(m.pos[v]);
			mn = mn.min(w);
			mx = mx.max(w);
		}
		return (mn + mx) * 0.5f;
	}
	if (p_pivot == HewnDock::PIVOT_ACTIVE) {
		// The element selected last.
		if (mode == MODE_VERTEX && !s.verts.empty()) {
			return g.xform(m.pos[s.verts.back()]);
		}
		if (mode == MODE_EDGE && !s.edges.empty()) {
			return g.xform((m.pos[s.edges.back().a] + m.pos[s.edges.back().b]) * 0.5f);
		}
		if (mode == MODE_FACE && !s.faces.empty()) {
			return g.xform(hewn::face_center(m, m.faces[s.faces.back()]));
		}
	}
	Vector3 c;
	for (int v : p_verts) {
		c += g.xform(m.pos[v]);
	}
	return c / (float)p_verts.size();
}

// Individual origins: every connected piece of the selection turns around its own center.
std::vector<Vector3> HewnEditorPlugin::_individual_pivots(const std::vector<int> &p_verts, const Vector3 &p_fallback) const {
	std::vector<Vector3> out(p_verts.size(), p_fallback);
	HewnMesh3D *n = get_edited();
	if (!n || mode == MODE_VERTEX) {
		return out;
	}
	const hewn::Mesh &m = n->get_data();
	const hewn::Selection &s = n->selection;
	const Transform3D g = n->get_global_transform();
	// union-find over the selected vertices, joined by selected edges / faces
	std::unordered_map<int, int> parent;
	std::function<int(int)> find = [&](int x) {
		while (parent[x] != x) {
			parent[x] = parent[parent[x]];
			x = parent[x];
		}
		return x;
	};
	for (int v : p_verts) {
		parent[v] = v;
	}
	auto unite = [&](int a, int b) {
		if (parent.count(a) && parent.count(b)) {
			parent[find(a)] = find(b);
		}
	};
	if (mode == MODE_EDGE) {
		for (const hewn::Edge &e : s.edges) {
			unite(e.a, e.b);
		}
	} else {
		for (int f : s.faces) {
			const hewn::Face &F = m.faces[f];
			for (int i = 1; i < F.size(); i++) {
				unite(F.v[0], F.v[i]);
			}
		}
	}
	std::unordered_map<int, std::pair<Vector3, int>> sum;
	for (int v : p_verts) {
		std::pair<Vector3, int> &e = sum[find(v)];
		e.first += g.xform(m.pos[v]);
		e.second++;
	}
	for (size_t i = 0; i < p_verts.size(); i++) {
		const std::pair<Vector3, int> &e = sum[find(p_verts[i])];
		out[i] = e.first / (float)e.second;
	}
	return out;
}

// ------------------------------------------------------------------ proportional editing

float HewnEditorPlugin::falloff(int p_type, float p_t, int p_seed) {
	const float t = CLAMP(p_t, 0.0f, 1.0f);
	switch (p_type) {
		case 0: // smooth
			return t * t * (3.0f - 2.0f * t);
		case 1: // sphere
			return std::sqrt(MAX(0.0f, 2.0f * t - t * t));
		case 2: // root
			return std::sqrt(t);
		case 3: // inverse square
			return t * (2.0f - t);
		case 4: // sharp
			return t * t;
		case 5: // linear
			return t;
		case 6: // constant
			return 1.0f;
		default: { // random (stable per vertex)
			uint32_t h = (uint32_t)p_seed * 2654435761u;
			h ^= h >> 13;
			h *= 0x5bd1e995u;
			h ^= h >> 15;
			return t * (float)(h & 0xffff) / 65535.0f;
		}
	}
}

// The selected vertices (weight 1) plus the vertices inside the proportional radius.
void HewnEditorPlugin::_weighted_verts(const std::vector<int> &p_selected, bool p_prop, float p_radius, int p_falloff, bool p_connected,
		std::vector<int> &r_verts, std::vector<float> &r_weights) const {
	r_verts = p_selected;
	r_weights.assign(p_selected.size(), 1.0f);
	HewnMesh3D *n = get_edited();
	if (!n || !p_prop || p_radius <= 0.0f || p_selected.empty()) {
		return;
	}
	const hewn::Mesh &m = n->get_data();
	const Transform3D g = n->get_global_transform();
	std::vector<char> is_sel(m.pos.size(), 0), used(m.pos.size(), 0);
	for (int v : p_selected) {
		is_sel[v] = 1;
	}
	for (const hewn::Face &F : m.faces) {
		for (int v : F.v) {
			used[v] = 1;
		}
	}
	std::vector<char> allowed = used;
	if (p_connected) {
		hewn::Topology t;
		t.build(m);
		allowed.assign(m.pos.size(), 0);
		for (int v : hewn::sel::linked_verts(m, t, p_selected)) {
			allowed[v] = 1;
		}
	}
	std::vector<Vector3> sel_w;
	for (int v : p_selected) {
		sel_w.push_back(g.xform(m.pos[v]));
	}
	for (int v = 0; v < (int)m.pos.size(); v++) {
		if (is_sel[v] || !allowed[v]) {
			continue;
		}
		const Vector3 w = g.xform(m.pos[v]);
		float d = 1e30f;
		for (const Vector3 &s : sel_w) {
			Vector3 dv = w - s;
			if (prop_proj_axis != Vector3()) {
				dv -= prop_proj_axis * dv.dot(prop_proj_axis); // Projected (2D): across the view only
			}
			d = MIN(d, dv.length());
		}
		if (d >= p_radius) {
			continue;
		}
		const float wt = falloff(p_falloff, 1.0f - d / p_radius, v);
		if (wt > 0.0f) {
			r_verts.push_back(v);
			r_weights.push_back(wt);
		}
	}
}

// ------------------------------------------------------------------ snapping

bool HewnEditorPlugin::snapping_active(bool p_ctrl) const {
	return dock->snap_enabled->is_pressed() != p_ctrl;
}

bool HewnEditorPlugin::_snap_target(Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_point) const {
	const int t = dock->snap_targets;
	last_snap_kind = SNAP_KIND_VERTEX;
	if ((t & HewnDock::SNAP_VERTEX) && _snap_point_at(p_cam, p_pos, r_point)) {
		return true;
	}
	last_snap_kind = SNAP_KIND_EDGE;
	if (t & HewnDock::SNAP_EDGE_CENTER) {
		std::vector<Vector3> mids;
		mids.reserve(cand_edges.size());
		for (const std::pair<Vector3, Vector3> &e : cand_edges) {
			mids.push_back((e.first + e.second) * 0.5f);
		}
		if (_snap_point_in(mids, p_cam, p_pos, r_point)) {
			return true;
		}
	}
	last_snap_kind = SNAP_KIND_FACE;
	if ((t & HewnDock::SNAP_FACE_CENTER) && _snap_point_in(cand_face_centers, p_cam, p_pos, r_point)) {
		return true;
	}
	last_snap_kind = SNAP_KIND_EDGE;
	if (t & HewnDock::SNAP_EDGE) {
		float best = 10.0f;
		bool found = false;
		for (const std::pair<Vector3, Vector3> &e : cand_edges) {
			if (p_cam->is_position_behind(e.first) || p_cam->is_position_behind(e.second)) {
				continue;
			}
			float tt;
			const float d = seg_dist(p_pos, p_cam->unproject_position(e.first), p_cam->unproject_position(e.second), &tt);
			if (d < best) {
				// closest point of the 3D edge to the mouse ray (screen interpolation is not linear in depth)
				Vector3 o, dir;
				_ray(p_cam, p_pos, o, dir);
				const float u = CLAMP(line_ray_param(e.first, e.second - e.first, o, dir), 0.0f, 1.0f);
				if (!_snap_seen(p_cam, e.first.lerp(e.second, u))) {
					continue; // (behind a surface)
				}
				best = d;
				r_point = e.first.lerp(e.second, u);
				found = true;
			}
		}
		if (found) {
			return true;
		}
	}
	last_snap_kind = SNAP_KIND_FACE;
	if (t & HewnDock::SNAP_FACE) {
		Vector3 from, dir, nrm;
		_ray(p_cam, p_pos, from, dir);
		const bool ours = _raycast_scene(from, dir, r_point, nrm, nullptr, nullptr, true);
		Vector3 plain;
		if (_raycast_cand_tris(from, dir, plain) && (!ours || from.distance_to(plain) < from.distance_to(r_point))) {
			r_point = plain;
			return true;
		}
		return ours;
	}
	return false;
}

bool HewnEditorPlugin::snap_increment_on(bool p_ctrl) const {
	return snapping_active(p_ctrl) && (dock->snap_targets & HewnDock::SNAP_STEPS);
}

// The grid point nearest to p_point: steps of p_step from the world origin along p_axes.
Vector3 HewnEditorPlugin::_grid_point(const Vector3 &p_point, const Basis &p_axes, float p_step) const {
	const Basis b = p_axes.orthonormalized();
	Vector3 l = b.transposed().xform(p_point);
	l = Vector3(std::round(l.x / p_step) * p_step, std::round(l.y / p_step) * p_step, std::round(l.z / p_step) * p_step);
	return b.xform(l);
}

// ------------------------------------------------------------------ auto merge

void HewnEditorPlugin::_auto_merge(hewn::Mesh &m, const std::vector<int> &p_moved) const {
	if (!dock->automerge->is_pressed() || p_moved.empty()) {
		return;
	}
	const float dist = (float)dock->automerge_dist->get_value();
	std::unordered_set<int> moved(p_moved.begin(), p_moved.end());
	std::vector<char> used(m.pos.size(), 0);
	for (const hewn::Face &F : m.faces) {
		for (int v : F.v) {
			used[v] = 1;
		}
	}
	std::vector<std::vector<int>> groups;
	for (int v : p_moved) {
		int best = -1;
		float bd = dist;
		for (int u = 0; u < (int)m.pos.size(); u++) {
			if (!used[u] || moved.count(u) || u == v) {
				continue;
			}
			const float d = m.pos[u].distance_to(m.pos[v]);
			if (d <= bd) {
				bd = d;
				best = u;
			}
		}
		if (best >= 0) {
			m.pos[v] = m.pos[best];
			groups.push_back({ best, v });
		}
	}
	if (!groups.empty()) {
		hewn::ops::collapse_groups(m, groups);
	}
}
