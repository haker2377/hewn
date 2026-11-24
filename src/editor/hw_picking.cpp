// Hewn editor - picking and selecting: clicks, boxes, linked, by normal, checker, separate.

#include "hw_plugin_impl.h"

// ------------------------------------------------------------------ picking

bool HewnEditorPlugin::_ray(Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_from, Vector3 &r_dir) const {
	if (!p_cam) {
		return false;
	}
	r_from = p_cam->project_ray_origin(p_pos);
	r_dir = p_cam->project_ray_normal(p_pos);
	return true;
}

// The tree of the edited mesh's triangles: kept while its positions and faces stay (checked when it is
// rebuilt and once a frame).
const hw_editor::TriBVH *HewnEditorPlugin::_bvh_sync(HewnMesh3D *p_node) const {
	const hewn::BuildOut &b = p_node->get_build();
	const hewn::Mesh &m = p_node->get_data();
	const uint64_t id = p_node->get_instance_id();
	const uint64_t frame = Engine::get_singleton()->get_process_frames();
	if (!bvh || id != bvh_node || p_node->get_revision() != bvh_rev || frame != bvh_frame) {
		const uint64_t h = geometry_hash(m);
		if (!bvh || id != bvh_node || h != bvh_hash) {
			if (!bvh) {
				bvh = std::make_shared<TriBVH>();
			}
			bvh->build(m, b);
			bvh_node = id;
			bvh_hash = h;
		}
		bvh_rev = p_node->get_revision();
		bvh_frame = frame;
	}
	return bvh.get();
}

int HewnEditorPlugin::_raycast_face(const Vector3 &p_from, const Vector3 &p_dir, float *r_dist) const {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return -1;
	}
	const TriBVH *tree = _bvh_sync(n);
	const Transform3D g = n->get_global_transform();
	const Transform3D inv = g.affine_inverse();
	const Vector3 o = inv.xform(p_from);
	const Vector3 d = inv.basis.xform(p_dir);
	float t = 0.0f;
	const int f = tree->raycast(o, d, t, cull_back());
	if (r_dist) {
		*r_dist = f >= 0 ? g.xform(o + d * t).distance_to(p_from) : 1e30f;
	}
	return f;
}

std::vector<HewnMesh3D *> HewnEditorPlugin::_scene_meshes() const {
	std::vector<HewnMesh3D *> out;
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return out;
	}
	std::vector<Node *> stack = { root };
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		HewnMesh3D *m = Object::cast_to<HewnMesh3D>(x);
		if (m && m->is_visible_in_tree()) {
			out.push_back(m);
		}
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
	}
	return out;
}

bool HewnEditorPlugin::_raycast_scene(const Vector3 &p_from, const Vector3 &p_dir, Vector3 &r_hit, Vector3 &r_normal, HewnMesh3D **r_node, int *r_face, bool p_skip_moving) const {
	HewnMesh3D *edited = get_edited();
	float best = 1e30f;
	bool found = false;
	for (HewnMesh3D *m : _scene_meshes()) {
		const std::unordered_set<int> *skip = !p_skip_moving ? nullptr : m == edited ? &cand_skip_faces : nullptr;
		if (p_skip_moving && m != edited) {
			auto so = cand_skip_other.find((uint64_t)m->get_instance_id());
			skip = so != cand_skip_other.end() ? &so->second : nullptr;
		}
		if (p_skip_moving && _moving_node(m)) {
			continue;
		}
		// CSG: the surface is the model's result, not its brushes (but the brush being edited)
		const bool model = m->is_csg_model() && m != edited;
		if (m->is_brush() && m != edited) {
			continue;
		}
		float t;
		const int f = raycast_node(m, p_from, p_dir, t, model ? nullptr : skip, cull_back(), model);
		if (f >= 0 && t < best) {
			best = t;
			found = true;
			r_hit = p_from + p_dir * t;
			const hewn::Mesh &sm = model ? m->get_shown_data() : m->get_data();
			r_normal = (m->get_global_transform().basis.inverse().transposed().xform(hewn::face_normal(sm, sm.faces[f]))).normalized();
			if (r_node) {
				*r_node = m;
			}
			if (r_face) {
				*r_face = model ? -1 : f;
			}
		}
	}
	return found;
}

bool HewnEditorPlugin::_visible(Camera3D *p_cam, const Vector3 &p_world) const {
	if (dock && dock->xray->is_pressed()) {
		return true;
	}
	// the same answer again while nothing that changes it has changed (the mouse moving over the mesh)
	HewnMesh3D *n = get_edited();
	if (n && p_cam) {
		VisKey k;
		k.cam = p_cam->get_instance_id();
		k.node = n->get_instance_id();
		_bvh_sync(n);
		k.geo = bvh_hash;
		k.cam_xf = p_cam->get_global_transform();
		k.node_xf = n->get_global_transform();
		k.fov = (float)p_cam->get_fov();
		k.size = (float)p_cam->get_size();
		k.znear = (float)p_cam->get_near();
		k.zfar = (float)p_cam->get_far();
		k.hoff = (float)p_cam->get_h_offset();
		k.voff = (float)p_cam->get_v_offset();
		k.proj = (int)p_cam->get_projection();
		k.vp = p_cam->get_viewport() ? p_cam->get_viewport()->get_visible_rect().size : Vector2();
		k.cull = cull_back();
		if (!(k == vis_key)) {
			vis_key = k;
			vis_memo.clear();
		}
	}
	uint32_t bits[3];
	std::memcpy(bits, &p_world, sizeof(bits));
	const uint64_t key = (((uint64_t)bits[0] * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)bits[1] * 0xC2B2AE3D27D4EB4Full) ^ ((uint64_t)bits[2] * 0x165667B19E3779F9ull)) + bits[2];
	auto it = vis_memo.find(key);
	if (it != vis_memo.end()) {
		return it->second != 0;
	}
	const Vector2 sp = p_cam->unproject_position(p_world);
	Vector3 from, dir;
	_ray(p_cam, sp, from, dir);
	const float dist = (p_world - from).dot(dir);
	float hit;
	const bool out = _raycast_face(from, dir, &hit) < 0 || hit >= dist - MAX(1e-3f, dist * 2e-3f);
	vis_memo[key] = out ? 1 : 0;
	return out;
}

void HewnEditorPlugin::_pick(Camera3D *p_cam, const Vector2 &p_pos, int &r_vert, hewn::Edge &r_edge, int &r_face) const {
	_pick_mode(p_cam, p_pos, mode, r_vert, r_edge, r_face);
}

void HewnEditorPlugin::_pick_mode(Camera3D *p_cam, const Vector2 &p_pos, Mode p_mode, int &r_vert, hewn::Edge &r_edge, int &r_face) const {
	r_vert = -1;
	r_edge = hewn::Edge(-1, -1);
	r_face = -1;
	HewnMesh3D *n = get_edited();
	if (!n || !p_cam) {
		return;
	}
	const Transform3D g = n->get_global_transform();
	const hewn::Mesh &m = n->get_data();
	if (p_mode == MODE_FACE) {
		if (face_dots_active()) {
			// X-ray: faces are picked by their center dots
			float best = 12.0f;
			for (int f = 0; f < (int)m.faces.size(); f++) {
				const Vector3 w = g.xform(hewn::face_center(m, m.faces[f]));
				if (p_cam->is_position_behind(w) || m.faces[f].hidden) {
					continue;
				}
				const float d = p_cam->unproject_position(w).distance_to(p_pos);
				if (d < best) {
					best = d;
					r_face = f;
				}
			}
			return;
		}
		Vector3 from, dir;
		_ray(p_cam, p_pos, from, dir);
		r_face = _raycast_face(from, dir);
		return;
	}
	if (p_mode == MODE_VERTEX) {
		const std::vector<char> used = visible_verts(m);
		float best = (mode_mask & (mode_mask - 1)) != 0 ? pick_px + 4.0f : pick_px; // (several modes: a vertex first, a bit farther)
		for (int v = 0; v < (int)m.pos.size(); v++) {
			if (!used[v]) {
				continue;
			}
			const Vector3 w = g.xform(m.pos[v]);
			if (p_cam->is_position_behind(w)) {
				continue;
			}
			const float d = p_cam->unproject_position(w).distance_to(p_pos);
			if (d < best && _visible(p_cam, w)) {
				best = d;
				r_vert = v;
			}
		}
		return;
	}
	if (p_mode == MODE_EDGE) {
		hewn::Topology t;
		t.build(m);
		float best = pick_px * 0.8f;
		std::vector<hewn::Edge> edges = t.edges;
		for (const std::pair<int, int> &wr : m.wires) {
			edges.push_back(hewn::Edge(wr.first, wr.second));
		}
		// (an edge going past the camera: the part in front of it; the point checked for being seen: the one on the
		// edge under the closest point on the screen, not its middle, which may be off the screen)
		const Transform3D ct = p_cam->get_global_transform();
		const Vector3 cf = -ct.basis.get_column(2).normalized();
		const float near_d = p_cam->get_near() * 1.5f + 1e-3f;
		for (const hewn::Edge &e : edges) {
			Vector3 wa = g.xform(m.pos[e.a]), wb = g.xform(m.pos[e.b]);
			const float da = (wa - ct.origin).dot(cf) - near_d, db = (wb - ct.origin).dot(cf) - near_d;
			if ((da < 0.0f && db < 0.0f) || !edge_visible(m, t, e)) {
				continue;
			}
			if (da < 0.0f) {
				wa = wa + (wb - wa) * (da / (da - db));
			} else if (db < 0.0f) {
				wb = wb + (wa - wb) * (db / (db - da));
			}
			float tt;
			const Vector2 sa = p_cam->unproject_position(wa), sb = p_cam->unproject_position(wb);
			const float d = seg_dist(p_pos, sa, sb, &tt);
			if (d >= best) {
				continue;
			}
			Vector3 o, dir;
			_ray(p_cam, sa.lerp(sb, tt), o, dir);
			const Vector3 u = wb - wa, w0 = wa - o;
			const float uu = u.dot(u), uv = u.dot(dir), vv = dir.dot(dir), uw = u.dot(w0), vw = dir.dot(w0);
			const float den = uu * vv - uv * uv;
			const float s = std::fabs(den) > 1e-12f ? CLAMP((uv * vw - vv * uw) / den, 0.0f, 1.0f) : tt;
			if (_visible(p_cam, wa.lerp(wb, s))) {
				best = d;
				r_edge = e;
			}
		}
	}
}

// Click: Shift toggles, Ctrl selects the shortest path from the last selected element (Ctrl+Shift: all of
// the paths, the region between them), Alt selects a loop, Ctrl+Alt a ring. Double-click also selects a loop (Alt+double-click a ring).
void HewnEditorPlugin::_click_select(Camera3D *p_cam, const Vector2 &p_pos, bool p_shift, bool p_ctrl, bool p_alt, bool p_double) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	if (!multi_ids.empty()) {
		auto hit = [&]() {
			for (int m = MODE_VERTEX; m <= MODE_FACE; m++) {
				if (!has_mode(m)) {
					continue;
				}
				int v, f;
				hewn::Edge e;
				_pick_mode(p_cam, p_pos, (Mode)m, v, e, f);
				if (v >= 0 || e.a >= 0 || f >= 0) {
					return true;
				}
			}
			return false;
		};
		if (!hit()) {
			const uint64_t keep = edited_id;
			for (HewnMesh3D *o : edited_meshes()) {
				edited_id = o->get_instance_id();
				if (o != n && hit()) {
					edited_id = keep;
					_set_active_mesh(o);
					n = o;
					break;
				}
				edited_id = keep;
			}
		}
		if (!p_shift && !p_ctrl) {
			for (HewnMesh3D *o : edited_meshes()) {
				if (o != n) {
					o->selection.clear(); // (a plain click: only what was clicked stays selected)
					o->update_gizmos();
				}
			}
		}
	}
	const Mode primary = mode;
	Mode target = mode;
	if ((mode_mask & (mode_mask - 1)) != 0) {
		target = MODE_FACE;
		for (int m = MODE_VERTEX; m <= MODE_FACE; m++) {
			if (!has_mode(m)) {
				continue;
			}
			int v, f;
			hewn::Edge e;
			_pick_mode(p_cam, p_pos, (Mode)m, v, e, f);
			if ((m == MODE_VERTEX && v >= 0) || (m == MODE_EDGE && e.a >= 0) || (m == MODE_FACE && f >= 0)) {
				target = (Mode)m;
				break;
			}
		}
		if (!p_shift && !p_ctrl) {
			n->selection.clear(); // a plain click replaces the selection of every kind
		}
	}
	mode = target;
	_click_select_single(p_cam, p_pos, p_shift || ((mode_mask & (mode_mask - 1)) != 0 && !p_ctrl), p_ctrl, p_alt, p_double);
	mode = primary;
	selection_changed();
}

void HewnEditorPlugin::_click_select_single(Camera3D *p_cam, const Vector2 &p_pos, bool p_shift, bool p_ctrl, bool p_alt, bool p_double) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	_hide_redo();
	const bool loop = p_alt || p_double;
	const bool ring = (p_alt && p_ctrl) || (p_double && p_alt);
	const bool path = p_ctrl && !loop;
	const bool region = path && p_shift; // Ctrl+Shift: everything between the last selected element and the clicked one
	hewn::Selection &s = n->selection;
	// Shift+Alt on a loop / ring already selected takes it away again; otherwise it is added
	auto put = [&](auto &r_sel, const auto &p_items) {
		bool all = p_shift && !p_items.empty();
		for (const auto &x : p_items) {
			all = all && std::find(r_sel.begin(), r_sel.end(), x) != r_sel.end();
		}
		if (all) {
			for (const auto &x : p_items) {
				r_sel.erase(std::remove(r_sel.begin(), r_sel.end(), x), r_sel.end());
			}
			return;
		}
		if (!p_shift) {
			r_sel.clear();
		}
		for (const auto &x : p_items) {
			if (std::find(r_sel.begin(), r_sel.end(), x) == r_sel.end()) {
				r_sel.push_back(x);
			}
		}
	};
	const hewn::Mesh &m = n->get_data();
	hewn::Topology t;
	t.build(m);

	if (mode == MODE_VERTEX) {
		if (loop) {
			// Loops are defined on edges: select the vertices of the loop through the nearest edge.
			int v, f;
			hewn::Edge e;
			_pick_mode(p_cam, p_pos, MODE_EDGE, v, e, f);
			if (e.a < 0) {
				return;
			}
			put(s.verts, hewn::sel::verts_of_edges(ring ? hewn::sel::edge_ring(m, t, e) : hewn::sel::edge_loop(m, t, e)));
		} else {
			int v, f;
			hewn::Edge e;
			_pick(p_cam, p_pos, v, e, f);
			if (path && v >= 0 && !s.verts.empty()) {
				for (int x : region ? hewn::sel::path_region_verts(m, t, s.verts.back(), v) : hewn::sel::shortest_path_verts(m, t, s.verts.back(), v)) {
					add_unique(s.verts, x);
				}
				// make the clicked vertex the last (active) one
				s.verts.erase(std::find(s.verts.begin(), s.verts.end(), v));
				s.verts.push_back(v);
			} else {
				if (!p_shift) {
					s.verts.clear();
				}
				if (v >= 0) {
					toggle(s.verts, v, p_shift, false);
				}
			}
			if (v >= 0) {
				active_node = n->get_instance_id();
				active_vert = v;
			}
		}
	} else if (mode == MODE_EDGE) {
		int v, f;
		hewn::Edge e;
		_pick(p_cam, p_pos, v, e, f);
		if (e.a >= 0) {
			active_node = n->get_instance_id();
			active_edge = e;
		}
		if (e.a >= 0 && loop) {
			put(s.edges, ring ? hewn::sel::edge_ring(m, t, e) : hewn::sel::edge_loop(m, t, e));
		} else if (e.a >= 0 && path && !s.edges.empty()) {
			for (const hewn::Edge &x : region ? hewn::sel::path_region_edges(m, t, s.edges.back(), e) : hewn::sel::shortest_path_edges(m, t, s.edges.back(), e)) {
				add_unique(s.edges, x);
			}
			s.edges.erase(std::find(s.edges.begin(), s.edges.end(), e));
			s.edges.push_back(e);
		} else {
			if (!p_shift) {
				s.edges.clear();
			}
			if (e.a >= 0) {
				toggle_edge(s.edges, e, p_shift, false);
			}
		}
	} else if (mode == MODE_FACE) {
		int v, f;
		hewn::Edge e;
		_pick(p_cam, p_pos, v, e, f);
		if (f >= 0) {
			active_node = n->get_instance_id();
			active_face = f;
		}
		if (f >= 0 && loop) {
			// Loop direction: the face edge closest to the click.
			const Transform3D g = n->get_global_transform();
			const hewn::Face &F = m.faces[f];
			int best_i = 0;
			float best_d = 1e30f;
			for (int i = 0; i < F.size(); i++) {
				const Vector2 a = p_cam->unproject_position(g.xform(m.pos[F.v[i]]));
				const Vector2 b = p_cam->unproject_position(g.xform(m.pos[F.v[(i + 1) % F.size()]]));
				const float d = seg_dist(p_pos, a, b);
				if (d < best_d) {
					best_d = d;
					best_i = i;
				}
			}
			const int dir = ring ? (best_i + 1) % F.size() : best_i;
			put(s.faces, hewn::sel::face_loop(m, t, f, hewn::Edge(F.v[dir], F.v[(dir + 1) % F.size()])));
		} else if (f >= 0 && path && !s.faces.empty()) {
			for (int x : region ? hewn::sel::path_region_faces(m, t, s.faces.back(), f) : hewn::sel::shortest_path_faces(m, t, s.faces.back(), f)) {
				add_unique(s.faces, x);
			}
			s.faces.erase(std::find(s.faces.begin(), s.faces.end(), f));
			s.faces.push_back(f);
		} else {
			if (!p_shift) {
				s.faces.clear();
			}
			if (f >= 0) {
				toggle(s.faces, f, p_shift, false);
			}
		}
	}
	selection_changed();
}

// L: select (or deselect) everything linked to the element under the cursor, stopping at the
// delimiters chosen in the Last Operation panel.
void HewnEditorPlugin::_select_linked_at(Camera3D *p_cam, const Vector2 &p_pos) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	int v, f;
	hewn::Edge e;
	_pick(p_cam, p_pos, v, e, f);
	const hewn::Mesh &m = n->get_data();
	hewn::Topology t;
	t.build(m);
	linked_seed_face = -1;
	if (mode == MODE_FACE && f >= 0) {
		linked_seed_face = f;
	} else if (mode == MODE_VERTEX && v >= 0 && !t.vert_faces[v].empty()) {
		linked_seed_face = t.vert_faces[v][0];
	} else if (mode == MODE_EDGE && e.a >= 0) {
		const int id = t.find_edge(e.a, e.b);
		if (id >= 0 && !t.edge_faces[id].empty()) {
			linked_seed_face = t.edge_faces[id][0];
		}
	}
	if (linked_seed_face < 0) {
		return;
	}
	linked_sel_before = n->selection;
	_apply_select_linked();
	last_op = "select_linked";
	if (HewnMesh3D *ln = get_edited()) {
		last_op_node = ln->get_instance_id();
	}
	last_op_mode = mode;
	_show_redo();
}

// Delete by rules from the selection: in vertex mode the edges are those between selected vertices
// (wires too), the faces those with all their vertices selected; in face mode the edges are those of only
// selected faces (Only Edges & Faces leaves the border of the selection).
void HewnEditorPlugin::_delete_selected(hewn::Mesh &m, int p_kind) {
	HewnMesh3D *n = get_edited();
	const hewn::Selection &s = n->selection;
	const std::vector<int> sel = _selected_verts();
	const std::unordered_set<int> vs(sel.begin(), sel.end());
	std::vector<hewn::Edge> edges;
	std::vector<int> faces;
	hewn::Topology t;
	t.build(m);
	if (mode == MODE_EDGE) {
		edges = s.edges;
	} else if (mode == MODE_FACE) {
		faces = s.faces;
		const std::unordered_set<int> fs(faces.begin(), faces.end());
		for (int e = 0; e < (int)t.edges.size(); e++) {
			bool all = !t.edge_faces[e].empty();
			for (int f : t.edge_faces[e]) {
				all = all && fs.count(f);
			}
			if (all) {
				edges.push_back(t.edges[e]);
			}
		}
	} else {
		for (const hewn::Edge &e : t.edges) {
			if (vs.count(e.a) && vs.count(e.b)) {
				edges.push_back(e);
			}
		}
	}
	if (mode != MODE_FACE) {
		for (const std::pair<int, int> &w : m.wires) {
			if (vs.count(w.first) && vs.count(w.second)) {
				edges.push_back(hewn::Edge(w.first, w.second));
			}
		}
		faces = hewn::sel::faces_from_verts(m, sel, true);
	}
	hewn::ops::delete_elements(m, (hewn::ops::DeleteKind)p_kind, sel, edges, faces);
}

void HewnEditorPlugin::checker_deselect() {
	HewnMesh3D *n = get_edited();
	if (!n || mode == MODE_OBJECT) {
		return;
	}
	checker_sel_before = n->selection;
	_apply_checker();
	last_op = "checker_deselect";
	last_op_node = n->get_instance_id();
	last_op_mode = mode;
	_show_redo();
}

// Checker Deselect: the selected elements by how many steps they are from the first one (each part on
// its own); of every Deselected + Selected steps, the first Selected stay, the others go; Offset shifts that.
void HewnEditorPlugin::_apply_checker() {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	const hewn::Mesh &m = n->get_data();
	hewn::Selection &s = n->selection;
	s = checker_sel_before;
	const int nth = MAX(1, (int)op_param("checker_deselect", "nth"));
	const int skip = MAX(1, (int)op_param("checker_deselect", "skip"));
	const int offset = MAX(0, (int)op_param("checker_deselect", "offset"));
	const int period = nth + skip;
	hewn::Topology t;
	t.build(m);
	// steps over the selected elements: n items, their neighbors (only selected ones count)
	auto depths = [&](int count, const std::function<void(int, std::vector<int> &)> &neighbors) {
		std::vector<int> d(count, -1);
		for (int start = 0; start < count; start++) {
			if (d[start] >= 0) {
				continue;
			}
			d[start] = 0;
			std::vector<int> q{ start };
			for (size_t qi = 0; qi < q.size(); qi++) {
				std::vector<int> nb;
				neighbors(q[qi], nb);
				for (int x : nb) {
					if (x >= 0 && x < count && d[x] < 0) {
						d[x] = d[q[qi]] + 1;
						q.push_back(x);
					}
				}
			}
		}
		return d;
	};
	auto keep = [&](int depth) { return (depth + offset) % period < skip; };
	if (mode == MODE_FACE) {
		const std::vector<int> fs = s.faces;
		std::unordered_map<int, int> at;
		for (int i = 0; i < (int)fs.size(); i++) {
			at[fs[i]] = i;
		}
		const std::vector<int> d = depths((int)fs.size(), [&](int i, std::vector<int> &r) {
			const hewn::Face &F = m.faces[fs[i]];
			for (int k = 0; k < F.size(); k++) {
				const int e = t.find_edge(F.v[k], F.v[(k + 1) % F.size()]);
				for (int g : e >= 0 ? t.edge_faces[e] : std::vector<int>()) {
					auto it = at.find(g);
					if (it != at.end()) {
						r.push_back(it->second);
					}
				}
			}
		});
		s.faces.clear();
		for (int i = 0; i < (int)fs.size(); i++) {
			if (keep(d[i])) {
				s.faces.push_back(fs[i]);
			}
		}
		s.verts = hewn::sel::verts_of_faces(m, s.faces);
		s.edges = hewn::sel::edges_of_faces(m, s.faces);
	} else if (mode == MODE_EDGE) {
		const std::vector<hewn::Edge> es = s.edges;
		std::unordered_map<int, std::vector<int>> by_vert;
		for (int i = 0; i < (int)es.size(); i++) {
			by_vert[es[i].a].push_back(i);
			by_vert[es[i].b].push_back(i);
		}
		const std::vector<int> d = depths((int)es.size(), [&](int i, std::vector<int> &r) {
			for (int v : { es[i].a, es[i].b }) {
				for (int j : by_vert[v]) {
					r.push_back(j);
				}
			}
		});
		s.edges.clear();
		for (int i = 0; i < (int)es.size(); i++) {
			if (keep(d[i])) {
				s.edges.push_back(es[i]);
			}
		}
		s.verts = hewn::sel::verts_of_edges(s.edges);
	} else {
		const std::vector<int> vs = s.verts;
		std::unordered_map<int, int> at;
		for (int i = 0; i < (int)vs.size(); i++) {
			at[vs[i]] = i;
		}
		const std::vector<int> d = depths((int)vs.size(), [&](int i, std::vector<int> &r) {
			for (int e : t.vert_edges[vs[i]]) {
				const int o = t.edges[e].a == vs[i] ? t.edges[e].b : t.edges[e].a;
				auto it = at.find(o);
				if (it != at.end()) {
					r.push_back(it->second);
				}
			}
		});
		s.verts.clear();
		for (int i = 0; i < (int)vs.size(); i++) {
			if (keep(d[i])) {
				s.verts.push_back(vs[i]);
			}
		}
	}
	selection_changed();
}

// Same Normal: the faces turned within the angle of a selected face's normal; Connected only: just those
// reached from the selected faces over such faces.
void HewnEditorPlugin::select_by_normal() {
	HewnMesh3D *n = get_edited();
	if (!n || mode == MODE_OBJECT) {
		return;
	}
	if (mode != MODE_FACE) {
		set_mode(MODE_FACE);
	}
	normal_sel_before = n->selection;
	_apply_by_normal();
	last_op = "by_normal";
	last_op_node = n->get_instance_id();
	last_op_mode = mode;
	_show_redo();
}

void HewnEditorPlugin::_apply_by_normal() {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	const hewn::Mesh &m = n->get_data();
	hewn::Selection &s = n->selection;
	s = normal_sel_before;
	const std::vector<int> seeds = s.faces;
	std::vector<int> like = hewn::sel::faces_by_normal(m, seeds, (float)op_param("by_normal", "angle"));
	if ((bool)op_param("by_normal", "connected")) {
		std::vector<char> ok(m.faces.size(), 0), got(m.faces.size(), 0);
		for (int f : like) {
			ok[f] = 1;
		}
		hewn::Topology t;
		t.build(m);
		std::vector<int> q;
		for (int f : seeds) {
			if (f >= 0 && f < (int)m.faces.size() && !got[f]) {
				got[f] = 1;
				q.push_back(f);
			}
		}
		for (size_t qi = 0; qi < q.size(); qi++) {
			const hewn::Face &F = m.faces[q[qi]];
			for (int k = 0; k < F.size(); k++) {
				const int e = t.find_edge(F.v[k], F.v[(k + 1) % F.size()]);
				if (e < 0) {
					continue;
				}
				for (int g : t.edge_faces[e]) {
					if (ok[g] && !got[g]) {
						got[g] = 1;
						q.push_back(g);
					}
				}
			}
		}
		like = q;
	}
	s.faces = like;
	s.verts = hewn::sel::verts_of_faces(m, s.faces);
	s.edges = hewn::sel::edges_of_faces(m, s.faces);
	selection_changed();
}

void HewnEditorPlugin::_apply_select_linked() {
	HewnMesh3D *n = get_edited();
	if (!n || linked_seed_face < 0 || linked_seed_face >= (int)n->get_data().faces.size()) {
		return;
	}
	const hewn::Mesh &m = n->get_data();
	hewn::Selection &s = n->selection;
	s = linked_sel_before;
	hewn::Topology t;
	t.build(m);
	const String op = "select_linked";
	const int delimit = ((bool)op_param(op, "normal") ? 1 : 0) | ((bool)op_param(op, "material") ? 2 : 0) | ((bool)op_param(op, "seam") ? 4 : 0) |
			((bool)op_param(op, "sharp") ? 8 : 0) | ((bool)op_param(op, "uv") ? 16 : 0);
	const std::vector<int> faces = hewn::sel::linked_faces_delimit(m, t, { linked_seed_face }, delimit);
	const bool deselect = (bool)op_param(op, "deselect");
	auto apply_int = [&](std::vector<int> &list, const std::vector<int> &items) {
		for (int x : items) {
			auto it = std::find(list.begin(), list.end(), x);
			if (deselect && it != list.end()) {
				list.erase(it);
			} else if (!deselect && it == list.end()) {
				list.push_back(x);
			}
		}
	};
	if (mode == MODE_FACE) {
		apply_int(s.faces, faces);
	} else if (mode == MODE_VERTEX) {
		apply_int(s.verts, hewn::sel::verts_of_faces(m, faces));
	} else {
		for (const hewn::Edge &x : hewn::sel::edges_of_faces(m, faces)) {
			auto it = std::find(s.edges.begin(), s.edges.end(), x);
			if (deselect && it != s.edges.end()) {
				s.edges.erase(it);
			} else if (!deselect && it == s.edges.end()) {
				s.edges.push_back(x);
			}
		}
	}
	selection_changed();
}

// P: the selection (or every material / loose part) goes to a new Hewn mesh next to this one.
void HewnEditorPlugin::_separate(const String &p_how) {
	HewnMesh3D *n = get_edited();
	Node *parent = n ? n->get_parent() : nullptr;
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!n || !parent || !root) {
		return;
	}
	hewn::Mesh &m = n->get_data();
	std::vector<std::vector<int>> groups;
	if (p_how == "selection") {
		std::vector<int> faces = mode == MODE_FACE ? n->selection.faces : hewn::sel::faces_from_verts(m, _selected_verts(), true);
		if (!faces.empty() && faces.size() < m.faces.size()) {
			groups.push_back(faces);
		}
	} else if (p_how == "material") {
		std::map<int, std::vector<int>> by_mat;
		for (int f = 0; f < (int)m.faces.size(); f++) {
			by_mat[m.faces[f].material].push_back(f);
		}
		for (auto it = std::next(by_mat.begin()); it != by_mat.end(); ++it) {
			groups.push_back(it->second);
		}
	} else {
		hewn::Topology t;
		t.build(m);
		std::vector<int> all = hewn::sel::all_faces(m);
		std::vector<char> seen(m.faces.size(), 0);
		for (int f : all) {
			if (seen[f]) {
				continue;
			}
			std::vector<int> comp = hewn::sel::linked_faces(m, t, { f });
			for (int x : comp) {
				seen[x] = 1;
			}
			groups.push_back(comp);
		}
		if (!groups.empty()) {
			groups.erase(groups.begin()); // the first part stays
		}
	}
	if (groups.empty()) {
		return;
	}
	const Dictionary before = n->get_state();
	std::vector<HewnMesh3D *> created;
	std::vector<int> del;
	for (const std::vector<int> &faces : groups) {
		HewnMesh3D *nn = memnew(HewnMesh3D);
		nn->set_shape(hewn::shapes::CUSTOM);
		nn->get_data() = hewn::ops::extract_faces(m, faces);
		hewn::remove_unused_vertices(nn->get_data());
		nn->set_materials(n->get_materials());
		nn->set_name(String(n->get_name()) + "_part");
		nn->set_transform(n->get_transform());
		nn->commit(true);
		created.push_back(nn);
		del.insert(del.end(), faces.begin(), faces.end());
	}
	hewn::ops::delete_faces(m, del);
	n->selection.clear();
	hewn::remove_unused_vertices(m);
	n->commit(true);
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Separate");
	hw_undo_state(ur, true, n, n->get_state());
	for (HewnMesh3D *nn : created) {
		ur->add_do_method(parent, "add_child", nn, true);
		ur->add_do_method(nn, "set_owner", root);
		ur->add_do_reference(nn);
		ur->add_undo_method(parent, "remove_child", nn);
	}
	hw_undo_state(ur, false, n, before);
	ur->commit_action();
	selection_changed();
}

// Box select takes every element with a visible part inside the rectangle (vertices: the point itself).
bool HewnEditorPlugin::SelRegion::has(const Vector2 &p) const {
	if (kind == 1) {
		return p.distance_to(center) <= radius;
	}
	if (kind == 2) {
		return poly.size() >= 3 && point_in_poly(poly, p);
	}
	return rect.has_point(p);
}

bool HewnEditorPlugin::SelRegion::clip(const Vector2 &a, const Vector2 &b, float &r_t0, float &r_t1) const {
	if (kind == 0) {
		Vector2 ca, cb;
		return clip_segment(rect, a, b, ca, cb, r_t0, r_t1);
	}
	if (kind == 1) {
		// |a + t (b - a) - c| = r
		const Vector2 d = b - a, f = a - center;
		const float A = d.dot(d), B = 2.0f * f.dot(d), C = f.dot(f) - radius * radius;
		if (A < 1e-12f) {
			r_t0 = r_t1 = 0.0f;
			return C <= 0.0f;
		}
		const float disc = B * B - 4.0f * A * C;
		if (disc < 0.0f) {
			return false;
		}
		const float s = std::sqrt(disc);
		r_t0 = MAX(0.0f, (-B - s) / (2.0f * A));
		r_t1 = MIN(1.0f, (-B + s) / (2.0f * A));
		return r_t0 <= r_t1;
	}
	// a lasso: the samples along the segment that are inside
	const int steps = 16;
	int first = -1, last = -1;
	for (int k = 0; k <= steps; k++) {
		if (has(a.lerp(b, (float)k / steps))) {
			first = first < 0 ? k : first;
			last = k;
		}
	}
	if (first < 0) {
		return false;
	}
	r_t0 = (float)first / steps;
	r_t1 = (float)last / steps;
	return true;
}

Vector2 HewnEditorPlugin::SelRegion::middle() const {
	if (kind == 1) {
		return center;
	}
	if (kind == 2) {
		Vector2 c;
		for (const Vector2 &p : poly) {
			c += p;
		}
		return poly.empty() ? c : c / (float)poly.size();
	}
	return rect.get_center();
}

void HewnEditorPlugin::_box_select(Camera3D *p_cam, const Rect2 &p_rect, bool p_shift, bool p_ctrl) {
	SelRegion r;
	r.rect = p_rect;
	_region_select(p_cam, r, p_shift, p_ctrl);
}

void HewnEditorPlugin::_region_select(Camera3D *p_cam, const SelRegion &p_rect, bool p_shift, bool p_ctrl) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	if (!multi_ids.empty() && !multi_running) {
		// every edited mesh
		multi_running = true;
		const uint64_t keep = edited_id;
		for (HewnMesh3D *o : edited_meshes()) {
			edited_id = o->get_instance_id();
			_region_select(p_cam, p_rect, p_shift, p_ctrl);
			o->update_gizmos();
		}
		edited_id = keep;
		multi_running = false;
		selection_changed();
		return;
	}
	const Mode primary = mode;
	bool first = true;
	for (int m = MODE_VERTEX; m <= MODE_FACE; m++) {
		if (!has_mode(m)) {
			continue;
		}
		if (first && !p_shift && !p_ctrl) {
			n->selection.clear();
		}
		mode = (Mode)m;
		_box_select_single(p_cam, p_rect, true, p_ctrl);
		first = false;
	}
	mode = primary;
	selection_changed();
}

void HewnEditorPlugin::_box_select_single(Camera3D *p_cam, const SelRegion &p_rect, bool p_shift, bool p_ctrl) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	_hide_redo();
	const Transform3D g = n->get_global_transform();
	const hewn::Mesh &m = n->get_data();
	hewn::Selection &s = n->selection;
	const bool xray = dock && dock->xray->is_pressed();
	auto point_in = [&](const Vector3 &local) {
		const Vector3 w = g.xform(local);
		if (p_cam->is_position_behind(w)) {
			return false;
		}
		return p_rect.has(p_cam->unproject_position(w)) && _visible(p_cam, w);
	};
	if (!p_shift && !p_ctrl) {
		s.verts.clear();
		s.edges.clear();
		s.faces.clear();
	}
	if (mode == MODE_VERTEX) {
		const std::vector<char> used = visible_verts(m);
		for (int v = 0; v < (int)m.pos.size(); v++) {
			if (used[v] && point_in(m.pos[v])) {
				p_ctrl ? toggle(s.verts, v, false, true) : add_unique(s.verts, v);
			}
		}
	} else if (mode == MODE_EDGE) {
		hewn::Topology t;
		t.build(m);
		std::vector<hewn::Edge> edges = t.edges;
		for (const std::pair<int, int> &wr : m.wires) {
			edges.push_back(hewn::Edge(wr.first, wr.second));
		}
		for (const hewn::Edge &e : edges) {
			const Vector3 wa = g.xform(m.pos[e.a]), wb = g.xform(m.pos[e.b]);
			if (p_cam->is_position_behind(wa) || p_cam->is_position_behind(wb)) {
				continue;
			}
			float t0, t1;
			if (!p_rect.clip(p_cam->unproject_position(wa), p_cam->unproject_position(wb), t0, t1)) {
				continue;
			}
			// Sample the clipped part: any visible sample selects the edge.
			bool vis = false;
			for (int k = 0; k <= 4 && !vis; k++) {
				vis = _visible(p_cam, wa.lerp(wb, t0 + (t1 - t0) * k / 4.0f));
			}
			if (vis) {
				p_ctrl ? toggle_edge(s.edges, e, false, true) : add_unique(s.edges, e);
			}
		}
	} else if (mode == MODE_FACE) {
		auto face_visible_at = [&](int f, const Vector2 &sp) {
			if (xray) {
				return true;
			}
			Vector3 from, dir;
			_ray(p_cam, sp, from, dir);
			return _raycast_face(from, dir) == f;
		};
		for (int f = 0; f < (int)m.faces.size(); f++) {
			const hewn::Face &F = m.faces[f];
			if (F.hidden) {
				continue;
			}
			std::vector<Vector2> poly;
			bool behind = false;
			for (int v : F.v) {
				const Vector3 w = g.xform(m.pos[v]);
				behind = behind || p_cam->is_position_behind(w);
				poly.push_back(p_cam->unproject_position(w));
			}
			if (behind) {
				continue;
			}
			if (face_dots_active()) {
				// X-ray: the center dot decides
				if (point_in(hewn::face_center(m, F))) {
					p_ctrl ? toggle(s.faces, f, false, true) : add_unique(s.faces, f);
				}
				continue;
			}
			bool hit = point_in(hewn::face_center(m, F)) && face_visible_at(f, p_cam->unproject_position(g.xform(hewn::face_center(m, F))));
			// corners inside the box
			for (int i = 0; i < F.size() && !hit; i++) {
				if (p_rect.has(poly[i])) {
					// nudge towards the face center so the ray hits this face rather than a neighbor
					const Vector3 w = g.xform(m.pos[F.v[i]].lerp(hewn::face_center(m, F), 0.05f));
					hit = face_visible_at(f, p_cam->unproject_position(w));
				}
			}
			// box center inside the face, or face edges crossing the box
			if (!hit && point_in_poly(poly, p_rect.middle())) {
				hit = face_visible_at(f, p_rect.middle());
			}
			for (int i = 0; i < F.size() && !hit; i++) {
				float t0, t1;
				if (p_rect.clip(poly[i], poly[(i + 1) % F.size()], t0, t1)) {
					const Vector3 a = m.pos[F.v[i]], b = m.pos[F.v[(i + 1) % F.size()]];
					const Vector3 mid = a.lerp(b, (t0 + t1) * 0.5f).lerp(hewn::face_center(m, F), 0.05f);
					hit = face_visible_at(f, p_cam->unproject_position(g.xform(mid)));
				}
			}
			if (hit) {
				p_ctrl ? toggle(s.faces, f, false, true) : add_unique(s.faces, f);
			}
		}
	}
	selection_changed();
}
