// T: the selected face, or a grid of selected quads, gets its texture
// laid against the edge under the mouse. W / A / S / D: the image's top / left / bottom / right on that
// edge, Q / E: the image fits the height / the width, R: the scale back to 1 (the pixels per meter).
// A click applies it, Esc cancels.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>

#include <cmath>
#include <unordered_map>
#include <unordered_set>

using namespace godot;
using namespace hw_editor;

namespace {

Vector2 tile_scale(HewnMesh3D *p_node, const hewn::Face &p_face) {
	Ref<BaseMaterial3D> bm = p_node->get_material_for_slot(p_face.material);
	return bm.is_valid() ? Vector2(MAX(1e-6f, bm->get_uv1_scale().x), MAX(1e-6f, bm->get_uv1_scale().y)) : Vector2(1, 1);
}

// Neighboring faces that are not a grid of quads: each laid flat on its own plane, then turned and moved
// so it meets the face before it along their shared edge (unfolded across folds, like paper). False when
// they are not all connected.
bool unfold_faces(const hewn::Mesh &m, const std::vector<int> &p_faces, std::map<int, std::vector<Vector2>> &r_layout) {
	std::map<uint64_t, std::vector<int>> by_edge;
	for (int f : p_faces) {
		const hewn::Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			by_edge[hewn::ekey(F.v[i], F.v[(i + 1) % F.size()])].push_back(f);
		}
	}
	auto flat = [&](int f) {
		const hewn::Face &F = m.faces[f];
		Vector3 u, v;
		const Vector3 nrm = hewn::face_normal(m, F);
		hewn::uv_axes(nrm.length_squared() > 0.5f ? nrm : Vector3(0, 1, 0), u, v);
		std::vector<Vector2> l;
		for (int vi : F.v) {
			l.push_back(Vector2(m.pos[vi].dot(u), -m.pos[vi].dot(v)));
		}
		return l;
	};
	// (faces laid out already, e.g. the quads of a grid: the rest unfold from them)
	std::vector<int> queue;
	if (r_layout.empty()) {
		const int start = p_faces.back();
		r_layout[start] = flat(start);
		queue.push_back(start);
	} else {
		for (const std::pair<const int, std::vector<Vector2>> &it : r_layout) {
			queue.push_back(it.first);
		}
	}
	for (size_t qi = 0; qi < queue.size(); qi++) {
		const int f = queue[qi];
		const hewn::Face &F = m.faces[f];
		const std::vector<Vector2> lf = r_layout[f];
		for (int i = 0; i < F.size(); i++) {
			const int a = F.v[i], b = F.v[(i + 1) % F.size()];
			for (int g : by_edge[hewn::ekey(a, b)]) {
				if (r_layout.count(g)) {
					continue;
				}
				const hewn::Face &G = m.faces[g];
				std::vector<Vector2> lg = flat(g);
				int ia = -1, ib = -1;
				for (int k = 0; k < G.size(); k++) {
					ia = G.v[k] == a ? k : ia;
					ib = G.v[k] == b ? k : ib;
				}
				if (ia < 0 || ib < 0) {
					continue;
				}
				// the edge of G onto the same edge of F: a turn and a move (a mirror when it would land on F)
				const Vector2 A = lf[i], B = lf[(i + 1) % F.size()];
				auto place = [&](std::vector<Vector2> l, bool mirror) {
					if (mirror) {
						for (Vector2 &q : l) {
							q.x = -q.x;
						}
					}
					const float ang = (B - A).angle() - (l[ib] - l[ia]).angle();
					const Vector2 o = l[ia];
					for (Vector2 &q : l) {
						q = A + (q - o).rotated(ang);
					}
					return l;
				};
				// both laid out turning the same way (the faces wind alike): else its plane's axes were mirrored
				auto area = [](const std::vector<Vector2> &l) {
					float s = 0.0f;
					for (size_t k = 0; k < l.size(); k++) {
						s += l[k].cross(l[(k + 1) % l.size()]);
					}
					return s;
				};
				r_layout[g] = place(lg, (area(lg) > 0.0f) != (area(lf) > 0.0f));
				queue.push_back(g);
			}
		}
	}
	return r_layout.size() == p_faces.size();
}

// A smooth curved patch (a part of a sphere, a cylinder): least squares conformal map (LSCM) - each vertex
// one point of the layout, so no seams between the faces; angles kept as well as they can be (exact on
// what unrolls flat, a cylinder). In meters (scaled to the faces' area), V down like the projections.
bool lscm_layout(const hewn::Mesh &m, const std::vector<int> &p_faces, std::map<int, std::vector<Vector2>> &r_layout) {
	std::unordered_map<int, int> idx;
	std::vector<int> verts;
	for (int f : p_faces) {
		for (int v : m.faces[f].v) {
			if (!idx.count(v)) {
				idx[v] = (int)verts.size();
				verts.push_back(v);
			}
		}
	}
	const int N = (int)verts.size();
	if (N < 3) {
		return false;
	}
	// two residual rows per triangle (Cauchy-Riemann), weighted by the root of its area
	struct Row {
		int c[6];
		float w[6];
	};
	std::vector<Row> rows;
	double area3 = 0.0;
	for (int f : p_faces) {
		const hewn::Face &F = m.faces[f];
		for (int i = 1; i + 1 < F.size(); i++) {
			const int tv[3] = { F.v[0], F.v[i], F.v[i + 1] };
			const Vector3 p0 = m.pos[tv[0]], p1 = m.pos[tv[1]], p2 = m.pos[tv[2]];
			const Vector3 nn = (p1 - p0).cross(p2 - p0);
			if (nn.length() < 1e-10f || (p1 - p0).length() < 1e-7f) {
				continue;
			}
			const Vector3 e1 = (p1 - p0).normalized(), e2 = nn.normalized().cross(e1);
			const Vector2 q[3] = { Vector2(), Vector2((p1 - p0).dot(e1), 0.0f), Vector2((p2 - p0).dot(e1), (p2 - p0).dot(e2)) };
			const float A = 0.5f * (q[1].x * q[2].y - q[2].x * q[1].y);
			if (A < 1e-12f) {
				continue;
			}
			area3 += A;
			const float sw = std::sqrt(A);
			Row r1, r2;
			for (int j = 0; j < 3; j++) {
				const Vector2 e = q[(j + 2) % 3] - q[(j + 1) % 3];
				const Vector2 gj = Vector2(-e.y, e.x) / (2.0f * A); // the gradient of the corner's hat function
				const int k = idx[tv[j]];
				r1.c[2 * j] = 2 * k;
				r1.w[2 * j] = gj.x * sw; // du/dx
				r1.c[2 * j + 1] = 2 * k + 1;
				r1.w[2 * j + 1] = -gj.y * sw; // - dv/dy
				r2.c[2 * j] = 2 * k;
				r2.w[2 * j] = gj.y * sw; // du/dy
				r2.c[2 * j + 1] = 2 * k + 1;
				r2.w[2 * j + 1] = gj.x * sw; // + dv/dx
			}
			rows.push_back(r1);
			rows.push_back(r2);
		}
	}
	if (rows.empty()) {
		return false;
	}
	// two vertices far apart pinned (the rest follows, a similarity of the true map)
	auto farthest = [&](int from) {
		int best = from;
		float bd = -1.0f;
		for (int i = 0; i < N; i++) {
			const float d = m.pos[verts[i]].distance_squared_to(m.pos[verts[from]]);
			if (d > bd) {
				bd = d;
				best = i;
			}
		}
		return best;
	};
	const int pa = farthest(0), pb = farthest(pa);
	if (pa == pb) {
		return false;
	}
	std::vector<double> x(2 * N, 0.0);
	std::vector<char> pinned(2 * N, 0);
	const float dab = m.pos[verts[pa]].distance_to(m.pos[verts[pb]]);
	x[2 * pb] = dab;
	for (int k : { pa, pb }) {
		pinned[2 * k] = pinned[2 * k + 1] = 1;
	}
	// a start: the projection onto the average plane, moved onto the pins
	{
		Vector3 avg;
		for (int f : p_faces) {
			avg += hewn::face_normal_raw(m, m.faces[f]);
		}
		Vector3 u, v;
		hewn::uv_axes(avg.length() > 1e-8f ? avg.normalized() : Vector3(0, 1, 0), u, v);
		auto proj = [&](int i) { return Vector2(m.pos[verts[i]].dot(u), m.pos[verts[i]].dot(v)); };
		const Vector2 a = proj(pa), b = proj(pb);
		const float ang = -(b - a).angle(), sc = (b - a).length() > 1e-6f ? dab / (b - a).length() : 1.0f;
		for (int i = 0; i < N; i++) {
			if (i != pa && i != pb) {
				const Vector2 q = (proj(i) - a).rotated(ang) * sc;
				x[2 * i] = q.x;
				x[2 * i + 1] = q.y;
			}
		}
	}
	// conjugate gradients on the normal equations of the free values
	auto apply = [&](const std::vector<double> &p_in, std::vector<double> &r_out, bool p_free_only) {
		std::fill(r_out.begin(), r_out.end(), 0.0);
		for (const Row &r : rows) {
			double s = 0.0;
			for (int j = 0; j < 6; j++) {
				if (!p_free_only || !pinned[r.c[j]]) {
					s += r.w[j] * p_in[r.c[j]];
				}
			}
			for (int j = 0; j < 6; j++) {
				r_out[r.c[j]] += r.w[j] * s;
			}
		}
		for (int i = 0; i < 2 * N; i++) {
			if (pinned[i]) {
				r_out[i] = 0.0;
			}
		}
	};
	std::vector<double> fixed(2 * N, 0.0), b(2 * N), ax(2 * N), res(2 * N), p(2 * N), ap(2 * N);
	for (int i = 0; i < 2 * N; i++) {
		fixed[i] = pinned[i] ? x[i] : 0.0;
	}
	// b = -A^T A x_pinned (the pinned values' pull on the free ones)
	{
		std::fill(b.begin(), b.end(), 0.0);
		for (const Row &r : rows) {
			double s = 0.0;
			for (int j = 0; j < 6; j++) {
				s += r.w[j] * fixed[r.c[j]];
			}
			for (int j = 0; j < 6; j++) {
				b[r.c[j]] -= r.w[j] * s;
			}
		}
		for (int i = 0; i < 2 * N; i++) {
			if (pinned[i]) {
				b[i] = 0.0;
			}
		}
	}
	apply(x, ax, true);
	double bb = 0.0, rr = 0.0;
	for (int i = 0; i < 2 * N; i++) {
		res[i] = pinned[i] ? 0.0 : b[i] - ax[i];
		p[i] = res[i];
		rr += res[i] * res[i];
		bb += b[i] * b[i];
	}
	const int iters = MIN(20000, 8 * N + 200);
	for (int it = 0; it < iters && rr > 1e-20 * MAX(bb, 1e-30); it++) {
		apply(p, ap, true);
		double pap = 0.0;
		for (int i = 0; i < 2 * N; i++) {
			pap += p[i] * ap[i];
		}
		if (pap <= 1e-300) {
			break;
		}
		const double alpha = rr / pap;
		double rr2 = 0.0;
		for (int i = 0; i < 2 * N; i++) {
			if (!pinned[i]) {
				x[i] += alpha * p[i];
				res[i] -= alpha * ap[i];
				rr2 += res[i] * res[i];
			}
		}
		const double beta = rr2 / rr;
		rr = rr2;
		for (int i = 0; i < 2 * N; i++) {
			p[i] = pinned[i] ? 0.0 : res[i] + beta * p[i];
		}
	}
	// meters: the layout's area as the faces' own
	double area2 = 0.0;
	for (int f : p_faces) {
		const hewn::Face &F = m.faces[f];
		for (int i = 1; i + 1 < F.size(); i++) {
			const int a = idx[F.v[0]], b2 = idx[F.v[i]], c = idx[F.v[i + 1]];
			area2 += 0.5 * ((x[2 * b2] - x[2 * a]) * (x[2 * c + 1] - x[2 * a + 1]) - (x[2 * c] - x[2 * a]) * (x[2 * b2 + 1] - x[2 * a + 1]));
		}
	}
	if (!(area2 > 1e-12) || !std::isfinite(area2)) {
		return false; // (folded over itself: not a patch it can lay out)
	}
	const double s = std::sqrt(area3 / area2);
	r_layout.clear();
	for (int f : p_faces) {
		std::vector<Vector2> l;
		for (int v : m.faces[f].v) {
			const int k = idx[v];
			l.push_back(Vector2((float)(x[2 * k] * s), (float)(-x[2 * k + 1] * s))); // (V down)
		}
		r_layout[f] = l;
	}
	return true;
}

// The faces are one smooth curved patch: connected, not all in one plane, no fold sharper than 50 degrees
// between neighbors (a brush's corners: unfolded like paper instead).
bool smooth_patch(const hewn::Mesh &m, const std::vector<int> &p_faces) {
	std::map<uint64_t, std::vector<int>> by_edge;
	for (int f : p_faces) {
		const hewn::Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			by_edge[hewn::ekey(F.v[i], F.v[(i + 1) % F.size()])].push_back(f);
		}
	}
	std::unordered_map<int, Vector3> nrm;
	for (int f : p_faces) {
		nrm[f] = hewn::face_normal(m, m.faces[f]);
	}
	bool flat = true;
	for (int f : p_faces) {
		flat = flat && nrm[f].dot(nrm[p_faces[0]]) > 0.9999f;
	}
	if (flat) {
		return false;
	}
	std::unordered_map<int, std::vector<int>> nb;
	for (const std::pair<const uint64_t, std::vector<int>> &e : by_edge) {
		if (e.second.size() != 2) {
			continue;
		}
		if (nrm[e.second[0]].dot(nrm[e.second[1]]) < 0.643f) {
			return false;
		}
		nb[e.second[0]].push_back(e.second[1]);
		nb[e.second[1]].push_back(e.second[0]);
	}
	std::unordered_set<int> seen = { p_faces[0] };
	std::vector<int> q = { p_faces[0] };
	for (size_t i = 0; i < q.size(); i++) {
		for (int g : nb[q[i]]) {
			if (seen.insert(g).second) {
				q.push_back(g);
			}
		}
	}
	return seen.size() == p_faces.size();
}

} // namespace

void HewnEditorPlugin::start_tsnap(Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (!n || mode != MODE_FACE || n->selection.faces.empty() || !p_cam) {
		return;
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	hewn::Mesh &m = n->get_data();
	const Transform3D g = n->get_global_transform();
	ts = TSnapState();
	ts.before = n->get_state();
	ts.backup = m;
	for (int f : n->selection.faces) {
		ts.faces.push_back(f);
	}
	// the layout (meters): the face's own projection, or the quads laid out as a grid
	if (ts.faces.size() == 1) {
		const hewn::Face &F = m.faces[ts.faces[0]];
		Vector3 nrm = hewn::face_normal_raw(m, F);
		if (F.uvp.world) {
			nrm = g.basis.xform(nrm);
		}
		Vector3 u, v;
		hewn::uv_axes(nrm.length_squared() > 1e-20f ? nrm.normalized() : Vector3(0, 1, 0), u, v);
		std::vector<Vector2> l;
		for (int vi : F.v) {
			const Vector3 p = F.uvp.world ? g.xform(m.pos[vi]) : m.pos[vi];
			l.push_back(Vector2(p.dot(u), -p.dot(v)));
		}
		ts.layout[ts.faces[0]] = l;
	} else {
		hewn::Mesh tmp = m;
		const std::vector<int> fs(n->selection.faces.begin(), n->selection.faces.end());
		std::vector<int> done;
		if (smooth_patch(m, fs) && lscm_layout(m, fs, ts.layout)) {
			done = fs; // (a smooth curved patch: one seamless layout, nothing else to do)
		} else {
			ts.layout.clear();
			done = hewn::unwrap_quads(tmp, fs, fs.back());
		}
		if (ts.layout.size() == fs.size()) {
			// laid out already
		} else {
			// faces turned only a little from their average (a part of a sphere): one projection onto the average
			// plane - unfolding a curved patch piece by piece turns the faces far from the start
			Vector3 avg;
			for (int f : fs) {
				avg += hewn::face_normal_raw(m, m.faces[f]);
			}
			bool gentle = avg.length() > 1e-8f;
			avg = gentle ? avg.normalized() : Vector3(0, 1, 0);
			for (int f : fs) {
				gentle = gentle && hewn::face_normal(m, m.faces[f]).dot(avg) > 0.5f; // (within 60 degrees)
			}
			if (gentle) {
				// (first: a grid of quads on a sphere would drift away from it)
				Vector3 u, v;
				hewn::uv_axes(avg, u, v);
				for (int f : fs) {
					std::vector<Vector2> l;
					for (int vi : m.faces[f].v) {
						l.push_back(Vector2(m.pos[vi].dot(u), -m.pos[vi].dot(v)));
					}
					ts.layout[f] = l;
				}
			} else if (done.size() == fs.size()) {
				for (int f : done) {
					ts.layout[f] = tmp.faces[f].uv;
				}
			} else {
				// mostly quads (a sphere: its poles are triangles): the quads as a grid, the others unfolded onto them
				ts.layout.clear();
				if (done.size() * 2 >= fs.size()) {
					for (int f : done) {
						ts.layout[f] = tmp.faces[f].uv;
					}
				}
				if (!unfold_faces(m, fs, ts.layout)) {
					ts.grid_error = true; // (not connected)
				}
			}
		}
	}
	// the border of the selection: edges with one selected face
	std::unordered_set<uint64_t> half;
	for (int f : n->selection.faces) {
		const hewn::Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			half.insert(((uint64_t)(uint32_t)F.v[i] << 32) | (uint32_t)F.v[(i + 1) % F.size()]);
		}
	}
	for (int f : n->selection.faces) {
		const hewn::Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			const int a = F.v[i], b = F.v[(i + 1) % F.size()];
			if (!half.count(((uint64_t)(uint32_t)b << 32) | (uint32_t)a)) {
				ts.border.push_back({ f, i });
			}
		}
	}
	const hewn::Face &F0 = m.faces[n->selection.faces.back()];
	ts.k = tile_scale(n, F0) * F0.uvp.scale.abs();
	if (F0.manual_uv && F0.uv.size() == F0.v.size() && F0.size() >= 3) {
		// the tiles per meter of its own UVs: the map from the
		// face's plane to its UVs, the length of each row
		Vector3 u, v;
		const Vector3 nrm = hewn::face_normal(m, F0);
		hewn::uv_axes(nrm.length_squared() > 0.5f ? nrm : Vector3(0, 1, 0), u, v);
		const Vector2 ms = tile_scale(n, F0);
		bool found = false;
		for (int i = 1; i + 1 < F0.size() && !found; i++) {
			const Vector3 p0 = m.pos[F0.v[0]], p1 = m.pos[F0.v[i]], p2 = m.pos[F0.v[i + 1]];
			const Vector2 q1((p1 - p0).dot(u), (p1 - p0).dot(v)), q2((p2 - p0).dot(u), (p2 - p0).dot(v));
			const float det = q1.cross(q2);
			if (std::fabs(det) < 1e-8f) {
				continue;
			}
			const Vector2 d1 = F0.uv[i] - F0.uv[0], d2 = F0.uv[i + 1] - F0.uv[0];
			// A [q1 q2] = [d1 d2]: the rows of A
			const Vector2 row_u = Vector2(d1.x * q2.y - d2.x * q1.y, d2.x * q1.x - d1.x * q2.x) / det;
			const Vector2 row_v = Vector2(d1.y * q2.y - d2.y * q1.y, d2.y * q1.x - d1.y * q2.x) / det;
			if (row_u.length() > 1e-6f && row_v.length() > 1e-6f) {
				ts.k = Vector2(row_u.length(), row_v.length()) * ms;
				found = true;
			}
		}
		if (!found) {
			ts.k = ms;
		}
	} else if (F0.manual_uv || F0.uvp.fill != hewn::UV_FILL_TILE) {
		ts.k = tile_scale(n, F0);
	}
	ts.k = ts.k.max(Vector2(1e-4f, 1e-4f));
	ts.k_before_fit = ts.k;
	modal = MODAL_TSNAP;
	drag = DRAG_NONE;
	_tsnap_update(p_cam);
}

void HewnEditorPlugin::_tsnap_update(Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	hewn::Mesh &m = n->get_data();
	m = ts.backup;
	const Transform3D g = n->get_global_transform();
	static const char *sides[4] = { "top", "left", "bottom", "right" };
	static const char *fits[3] = { "", "  [fit height]", "  [fit width]" };
	ts.label = vformat(T("Texture to Edge: the image's %s on the edge under the mouse%s"), T(sides[ts.side]), T(fits[ts.fit])) + "\n" +
			T("W / A / S / D: top / left / bottom / right, Q: fit height, E: fit width, R: scale 1; click: done, Esc: cancel");
	if (ts.grid_error) {
		ts.label = T("T: select one face, or faces next to each other") + "\n" + T("Esc: back");
		update_overlays();
		return;
	}
	// the border edge nearest to the mouse
	ts.edge = -1;
	float best = 1e30f;
	for (size_t i = 0; i < ts.border.size(); i++) {
		const hewn::Face &F = m.faces[ts.border[i].first];
		const int c = ts.border[i].second;
		const Vector3 a = g.xform(m.pos[F.v[c]]), b = g.xform(m.pos[F.v[(c + 1) % F.size()]]);
		if (p_cam->is_position_behind(a) || p_cam->is_position_behind(b)) {
			continue;
		}
		const float d = seg_dist(mouse_pos, p_cam->unproject_position(a), p_cam->unproject_position(b));
		if (d < best) {
			best = d;
			ts.edge = (int)i;
		}
	}
	if (ts.edge < 0) {
		n->rebuild();
		update_overlays();
		return;
	}
	const int ef = ts.border[ts.edge].first, ec = ts.border[ts.edge].second;
	const std::vector<Vector2> &el = ts.layout[ef];
	const Vector2 la = el[ec], lb = el[(ec + 1) % el.size()];
	Vector2 centroid;
	for (const Vector2 &q : el) {
		centroid += q / (float)el.size();
	}
	// a turn of the layout: the edge along the image's side, the faces inside the image
	const bool along_u = ts.side == 0 || ts.side == 2;
	const Vector2 axis = along_u ? Vector2(1, 0) : Vector2(0, 1);
	const Vector2 dir = (lb - la).normalized();
	float ang = axis.angle() - dir.angle();
	auto turned = [&](const Vector2 &p, float a) { return (p - la).rotated(a); };
	const Vector2 ci = turned(centroid, ang);
	const float inside = along_u ? ci.y : ci.x; // the side of the edge the faces are on
	const bool want_positive = ts.side == 0 || ts.side == 1; // top / left: the image runs on from the edge
	if ((inside > 0.0f) != want_positive) {
		ang += (float)Math::PI;
	}
	// all corners in the turned frame (meters), their extent
	Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
	for (const std::pair<const int, std::vector<Vector2>> &it : ts.layout) {
		for (const Vector2 &q : it.second) {
			const Vector2 t = turned(q, ang);
			mn = mn.min(t);
			mx = mx.max(t);
		}
	}
	Vector2 k = ts.k;
	const Vector2 size = (mx - mn).max(Vector2(1e-6f, 1e-6f));
	const float ratio = ts.k.y / ts.k.x; // (square pixels stay square)
	if (ts.fit == 1) {
		k.y = 1.0f / size.y;
		k.x = k.y / ratio;
	} else if (ts.fit == 2) {
		k.x = 1.0f / size.x;
		k.y = k.x * ratio;
	}
	// the edge on the image's side, the image's corner at the start of the edge (fitted: at the faces' end)
	const Vector2 ta = turned(la, ang) * k, tb = turned(lb, ang) * k;
	Vector2 shift;
	if (along_u) {
		shift.y = (ts.side == 0 ? 0.0f : 1.0f) - ta.y;
		shift.x = -(ts.fit == 2 ? mn.x * k.x : MIN(ta.x, tb.x));
	} else {
		shift.x = (ts.side == 1 ? 0.0f : 1.0f) - ta.x;
		shift.y = -(ts.fit == 1 ? mn.y * k.y : MIN(ta.y, tb.y));
	}
	for (const std::pair<const int, std::vector<Vector2>> &it : ts.layout) {
		const Vector2 ms = tile_scale(n, m.faces[it.first]);
		std::vector<Vector2> uv;
		for (const Vector2 &q : it.second) {
			uv.push_back((turned(q, ang) * k + shift) / ms);
		}
		hewn::set_face_uvs(m, it.first, g, uv);
	}
	n->rebuild();
	if (uv_canvas) {
		uv_canvas->queue_redraw();
	}
	update_overlays();
}

void HewnEditorPlugin::_tsnap_finish(bool p_apply) {
	HewnMesh3D *n = get_edited();
	modal = MODAL_NONE;
	if (n) {
		if (p_apply && !ts.grid_error && ts.edge >= 0) {
			EditorUndoRedoManager *ur = get_undo_redo();
			ur->create_action("Hewn: Texture to Edge", UndoRedo::MERGE_DISABLE, n);
			ur->add_do_method(n, "set_state", n->get_state());
			ur->add_undo_method(n, "set_state", ts.before);
			ur->commit_action(false);
		} else {
			n->set_state(ts.before);
		}
	}
	ts.backup = hewn::Mesh();
	selection_changed();
}

int32_t HewnEditorPlugin::_tsnap_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	Input *in = Input::get_singleton();
	const bool flying = in->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT) || in->is_mouse_button_pressed(MOUSE_BUTTON_MIDDLE);
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		mouse_pos = mm->get_position();
		if (!flying) {
			_tsnap_update(p_cam);
		}
		return flying ? AFTER_GUI_INPUT_PASS : AFTER_GUI_INPUT_STOP;
	}
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid()) {
		if (mb->get_button_index() == MOUSE_BUTTON_LEFT) {
			if (mb->is_pressed()) {
				_tsnap_finish(true);
			}
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS; // (the camera)
	}
	Ref<InputEventKey> k = p_event;
	if (k.is_null()) {
		return AFTER_GUI_INPUT_PASS;
	}
	if (flying) {
		return AFTER_GUI_INPUT_PASS; // (WASD flies the camera)
	}
	if (!k->is_pressed() || k->is_echo()) {
		return AFTER_GUI_INPUT_STOP;
	}
	switch (key_code(k)) {
		case KEY_ESCAPE:
			_tsnap_finish(false);
			return AFTER_GUI_INPUT_STOP;
		case KEY_ENTER:
		case KEY_KP_ENTER:
		case KEY_SPACE:
		case KEY_T:
			_tsnap_finish(true);
			return AFTER_GUI_INPUT_STOP;
		case KEY_W:
			ts.side = 0;
			break;
		case KEY_A:
			ts.side = 1;
			break;
		case KEY_S:
			ts.side = 2;
			break;
		case KEY_D:
			ts.side = 3;
			break;
		case KEY_Q:
			ts.fit = ts.fit == 1 ? 0 : 1;
			break;
		case KEY_E:
			ts.fit = ts.fit == 2 ? 0 : 2;
			break;
		case KEY_R: {
			HewnMesh3D *n = get_edited();
			ts.fit = 0;
			ts.k = n ? tile_scale(n, ts.backup.faces[ts.faces[ts.faces.size() - 1]]) : Vector2(1, 1);
		} break;
		default:
			return AFTER_GUI_INPUT_STOP;
	}
	_tsnap_update(p_cam);
	return AFTER_GUI_INPUT_STOP;
}

void HewnEditorPlugin::_draw_tsnap(Control *p_overlay, Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (n && ts.edge >= 0 && ts.edge < (int)ts.border.size()) {
		const hewn::Mesh &m = n->get_data();
		const Transform3D g = n->get_global_transform();
		for (size_t i = 0; i < ts.border.size(); i++) {
			const hewn::Face &F = m.faces[ts.border[i].first];
			const int c = ts.border[i].second;
			const Vector3 a = g.xform(m.pos[F.v[c]]), b = g.xform(m.pos[F.v[(c + 1) % F.size()]]);
			if (!p_cam->is_position_behind(a) && !p_cam->is_position_behind(b)) {
				const bool hot = (int)i == ts.edge;
				p_overlay->draw_line(p_cam->unproject_position(a), p_cam->unproject_position(b), hot ? Color(1.0f, 0.72f, 0.2f) : Color(1, 1, 1, 0.5f),
						hot ? 4.0f : 1.5f, true);
			}
		}
	}
	Ref<Font> font = p_overlay->get_theme_default_font();
	if (font.is_valid()) {
		draw_header(p_overlay, font, ts.label);
	}
}
