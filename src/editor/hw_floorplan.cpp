// Floor plan (tsitski's tools): walls all around a floor with one button.
#include "hw_editor.h"
#include "hw_textures.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>

#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace godot;

namespace {

// The selected edges as runs of connected edges, each running against the winding of a face next to it
// (as the outline of a floor runs): the wall stands on the same side of them as around a whole floor.
// A run that comes back to its start is closed (its first vertex again at the end).
std::vector<std::vector<int>> edge_runs(const hewn::Mesh &m, const std::vector<hewn::Edge> &p_edges) {
	std::unordered_map<uint64_t, std::pair<int, int>> dir; // edge -> from, to
	for (const hewn::Edge &e : p_edges) {
		dir[hewn::ekey(e.a, e.b)] = { e.a, e.b };
	}
	for (const hewn::Face &F : m.faces) {
		const int n = F.size();
		for (int i = 0; i < n; i++) {
			const int a = F.v[i], b = F.v[(i + 1) % n];
			auto it = dir.find(hewn::ekey(a, b));
			if (it != dir.end()) {
				it->second = { b, a };
			}
		}
	}
	std::unordered_map<int, std::vector<int>> out_of; // vertex -> the ends of the edges leaving it
	std::unordered_map<int, int> in_count;
	for (const auto &kv : dir) {
		out_of[kv.second.first].push_back(kv.second.second);
		in_count[kv.second.second]++;
	}
	std::set<std::pair<int, int>> used;
	std::vector<std::vector<int>> runs;
	auto walk = [&](int p_start) {
		std::vector<int> run = { p_start };
		int v = p_start;
		while (true) {
			int next = -1;
			for (int o : out_of[v]) {
				if (!used.count({ v, o })) {
					next = o;
					break;
				}
			}
			if (next < 0) {
				break;
			}
			used.insert({ v, next });
			run.push_back(next);
			v = next;
			if (v == p_start) {
				break; // (closed)
			}
		}
		if (run.size() >= 2) {
			runs.push_back(run);
		}
	};
	// the open runs first (from their free ends), then the closed ones
	for (const auto &kv : out_of) {
		if (!in_count.count(kv.first)) {
			walk(kv.first);
		}
	}
	for (const auto &kv : out_of) {
		for (int o : kv.second) {
			if (!used.count({ kv.first, o })) {
				walk(kv.first);
			}
		}
	}
	return runs;
}

} // namespace

// The selected floors (the selected faces in edit mode, else the faces facing up of the selected meshes):
// a closed wall along each outline, standing on the side of the floor (the Wall's side in the shape panel).
// Edges selected in edge / vertex mode: walls only along them.
void HewnEditorPlugin::walls_around_floor() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return;
	}
	std::vector<HewnMesh3D *> meshes;
	PackedInt32Array picked;
	HewnMesh3D *edited = get_edited();
	std::vector<std::vector<Vector3>> edge_walls; // (world points; closed: the first one again at the end)
	if (edited && (mode == MODE_EDGE || mode == MODE_VERTEX)) {
		const hewn::Mesh &d = edited->get_data();
		std::vector<hewn::Edge> edges = edited->selection.edges;
		if (mode == MODE_VERTEX) {
			std::unordered_set<int> vs(edited->selection.verts.begin(), edited->selection.verts.end());
			edges.clear();
			hewn::Topology t;
			t.build(d);
			for (const hewn::Edge &e : t.edges) {
				if (vs.count(e.a) && vs.count(e.b)) {
					edges.push_back(e);
				}
			}
		}
		if (edges.empty()) {
			ERR_PRINT("Hewn: select the edges to put walls on first.");
			return;
		}
		const Transform3D g = edited->get_global_transform();
		for (const std::vector<int> &run : edge_runs(d, edges)) {
			std::vector<Vector3> pts;
			for (int v : run) {
				pts.push_back(g.xform(d.pos[v]));
			}
			edge_walls.push_back(pts);
		}
	} else if (edited && mode == MODE_FACE && !edited->selection.faces.empty()) {
		meshes.push_back(edited);
		for (int f : edited->selection.faces) {
			picked.push_back(f);
		}
	} else {
		const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
		for (int64_t i = 0; i < sel.size(); i++) {
			if (HewnMesh3D *n = Object::cast_to<HewnMesh3D>(sel[i])) {
				meshes.push_back(n);
			}
		}
		if (meshes.empty() && edited) {
			meshes.push_back(edited);
		}
	}
	if (meshes.empty()) {
		ERR_PRINT("Hewn: select a floor (a mesh, or its faces in face mode) first.");
		return;
	}
	std::vector<HewnMesh3D *> walls;
	// a wall along points (world space; closed when the last one is the first again)
	auto add_wall = [&](const std::vector<Vector3> &pts) {
		Vector3 c;
		const bool closed = pts.size() > 2 && pts.back().distance_to(pts.front()) < 1e-6f;
		const size_t n = closed ? pts.size() - 1 : pts.size();
		for (size_t i = 0; i < n; i++) {
			c += pts[i];
		}
		c /= (float)n;
		float y = pts[0].y;
		for (const Vector3 &p : pts) {
			y = MIN(y, p.y);
		}
		HewnMesh3D *w = memnew(HewnMesh3D);
		w->set_name("Wall");
		w->set_shape(hewn::shapes::WALL);
		w->set_shape_param("side", (float)wall_side);
		if (texture_dock && texture_dock->current_material.is_valid()) {
			w->apply_material(texture_dock->current_material, PackedInt32Array());
		}
		PackedVector3Array local;
		for (const Vector3 &p : pts) {
			local.push_back(Vector3(p.x - c.x, 0.0f, p.z - c.z));
		}
		w->set_poly_points(local);
		w->set_transform(Transform3D(Basis(), Vector3(c.x, y, c.z)));
		walls.push_back(w);
	};
	for (const std::vector<Vector3> &pts : edge_walls) {
		add_wall(pts);
	}
	for (HewnMesh3D *m : meshes) {
		const hewn::Mesh &d = m->get_data();
		const Transform3D g = m->get_global_transform();
		std::vector<int> faces;
		if (!picked.is_empty()) {
			for (int64_t i = 0; i < picked.size(); i++) {
				faces.push_back(picked[i]);
			}
		} else {
			for (int f = 0; f < (int)d.faces.size(); f++) {
				if (g.basis.xform(hewn::face_normal(d, d.faces[f])).normalized().y > 0.9f) {
					faces.push_back(f);
				}
			}
		}
		if (faces.empty()) {
			continue;
		}
		const hewn::Mesh floor = hewn::ops::extract_faces(d, faces);
		for (const std::vector<int> &loop : hewn::ops::holes(floor)) {
			if (loop.size() < 3) {
				continue;
			}
			std::vector<Vector3> pts;
			for (int v : loop) {
				pts.push_back(g.xform(floor.pos[v]));
			}
			pts.push_back(pts[0]); // (back to the start: a closed ring)
			add_wall(pts);
		}
	}
	if (walls.empty()) {
		ERR_PRINT("Hewn: no floor faces (facing up) in the selection.");
		return;
	}
	Node *parent = root;
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Walls Around Floor");
	for (HewnMesh3D *w : walls) {
		ur->add_do_method(parent, "add_child", w, true);
		ur->add_do_method(w, "set_owner", root);
		ur->add_do_reference(w);
		ur->add_undo_method(parent, "remove_child", w);
	}
	ur->commit_action();
	EditorSelection *es = EditorInterface::get_singleton()->get_selection();
	es->clear();
	for (HewnMesh3D *w : walls) {
		es->add_node(w);
	}
}
