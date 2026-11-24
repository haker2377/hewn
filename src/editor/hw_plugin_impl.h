// Hewn editor - what the parts of the plugin share (hw_plugin*.cpp, hw_picking.cpp, hw_handles.cpp, hw_run_op.cpp):
// their includes and small helpers.
#pragma once

#include "../hw_cutter.h"
#include "../hw_undo_state.h"
#include "../hw_fbx.h"
#include "../hw_scatter.h"
#include "hw_editor.h"
#include "hw_mcp.h"
#include "../hw_hotspot.h"
#include <godot_cpp/classes/csg_shape3d.hpp>
#include <godot_cpp/classes/surface_tool.hpp>
#include "../hw_level_bake.h"
#include "hw_scatter_dock.h"
#include "../hw_note.h"

#include <unordered_set>
#include "hw_quad.h"
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/editor_file_dialog.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/editor_plugin_registration.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/gltf_state.hpp>
#include <godot_cpp/classes/gltf_document.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include "../hw_entities.h"
#include "../hw_overlay3d.h"
#include <godot_cpp/classes/range.hpp>
#include <godot_cpp/classes/scroll_bar.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include "hw_prefabs.h"
#include "hw_textures.h"
#include "hw_editor_util.h"
#include "../hw_modifiers.h"
#include "hw_bvh.h"

#include <cstring>

#include <godot_cpp/classes/editor_inspector.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/area3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/convex_polygon_shape3d.hpp>
#include <godot_cpp/classes/path3d.hpp>
#include <godot_cpp/classes/curve3d.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/input_event_magnify_gesture.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/style_box.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/viewport.hpp>

#include <godot_cpp/core/object.hpp>

#include <functional>
#include <map>
#include <godot_cpp/classes/random_number_generator.hpp>

using namespace godot;

void hw_remove_quad_plugin(); // (below, with the settings)
static bool selftest_started = false;
using namespace hw_editor;


namespace hw_plugin_impl {


inline void toggle(std::vector<int> &v, int x, bool add, bool remove) {
	auto it = std::find(v.begin(), v.end(), x);
	if (remove) {
		if (it != v.end()) {
			v.erase(it);
		}
	} else if (add) {
		if (it == v.end()) {
			v.push_back(x);
		} else {
			v.erase(it);
		}
	} else if (it == v.end()) {
		v.push_back(x);
	}
}

inline void toggle_edge(std::vector<hewn::Edge> &v, const hewn::Edge &x, bool add, bool remove) {
	auto it = std::find(v.begin(), v.end(), x);
	if (remove) {
		if (it != v.end()) {
			v.erase(it);
		}
	} else if (add) {
		if (it == v.end()) {
			v.push_back(x);
		} else {
			v.erase(it);
		}
	} else if (it == v.end()) {
		v.push_back(x);
	}
}

template <typename T>
inline void add_unique(std::vector<T> &v, const T &x) {
	if (std::find(v.begin(), v.end(), x) == v.end()) {
		v.push_back(x);
	}
}

inline void sanitize(const hewn::Mesh &m, hewn::Selection &s) {
	HW_PROF("sanitize");
	// (in linear time: a mesh of tens of thousands of elements all selected is checked at once)
	const int nv = (int)m.pos.size(), nf = (int)m.faces.size();
	std::vector<int> v, f;
	{
		std::vector<char> seen(nv, 0);
		for (int x : s.verts) {
			if (x >= 0 && x < nv && !seen[x]) {
				seen[x] = 1;
				v.push_back(x);
			}
		}
	}
	{
		std::vector<char> seen(nf, 0);
		for (int x : s.faces) {
			if (x >= 0 && x < nf && !seen[x]) {
				seen[x] = 1;
				f.push_back(x);
			}
		}
	}
	std::vector<hewn::Edge> e;
	if (!s.edges.empty()) {
		std::unordered_set<uint64_t> exist; // the edges of the faces and the loose ones
		exist.reserve(m.faces.size() * 4 + m.wires.size());
		for (const hewn::Face &F : m.faces) {
			for (int i = 0; i < F.size(); i++) {
				exist.insert(hewn::ekey(F.v[i], F.v[(i + 1) % F.size()]));
			}
		}
		for (const std::pair<int, int> &w : m.wires) {
			exist.insert(hewn::ekey(w.first, w.second));
		}
		std::unordered_set<uint64_t> taken;
		for (const hewn::Edge &x : s.edges) {
			if (x.a >= 0 && x.b < nv && exist.count(hewn::ekey(x)) && taken.insert(hewn::ekey(x)).second) {
				e.push_back(x);
			}
		}
	}
	// Hidden faces (H) and what only they have cannot be selected.
	bool any_hidden = false;
	for (const hewn::Face &F : m.faces) {
		any_hidden = any_hidden || F.hidden;
	}
	if (any_hidden) {
		const std::vector<char> vis = hw_editor::visible_verts(m);
		v.erase(std::remove_if(v.begin(), v.end(), [&](int x) { return !vis[x]; }), v.end());
		f.erase(std::remove_if(f.begin(), f.end(), [&](int x) { return m.faces[x].hidden; }), f.end());
		if (!e.empty()) {
			hewn::Topology t;
			t.build(m);
			e.erase(std::remove_if(e.begin(), e.end(), [&](const hewn::Edge &x) { return !hw_editor::edge_visible(m, t, x); }), e.end());
		}
	}
	// Keep the click order (the last element is the "active" one for path selection).
	s.verts = v;
	s.faces = f;
	s.edges = e;
}

// Clips segment a-b to a rectangle (Liang-Barsky). Returns false if it misses.
inline bool clip_segment(const Rect2 &r, Vector2 a, Vector2 b, Vector2 &ra, Vector2 &rb, float &t0, float &t1) {
	t0 = 0.0f;
	t1 = 1.0f;
	const Vector2 d = b - a;
	const float p[4] = { -d.x, d.x, -d.y, d.y };
	const float q[4] = { a.x - r.position.x, r.position.x + r.size.x - a.x, a.y - r.position.y, r.position.y + r.size.y - a.y };
	for (int i = 0; i < 4; i++) {
		if (std::fabs(p[i]) < 1e-9f) {
			if (q[i] < 0.0f) {
				return false;
			}
			continue;
		}
		const float t = q[i] / p[i];
		if (p[i] < 0.0f) {
			t0 = MAX(t0, t);
		} else {
			t1 = MIN(t1, t);
		}
	}
	if (t0 > t1) {
		return false;
	}
	ra = a + d * t0;
	rb = a + d * t1;
	return true;
}

inline bool point_in_poly(const std::vector<Vector2> &poly, const Vector2 &q) {
	bool inside = false;
	const size_t n = poly.size();
	for (size_t i = 0, j = n - 1; i < n; j = i++) {
		const Vector2 &a = poly[i], &b = poly[j];
		if ((a.y > q.y) != (b.y > q.y) && q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x) {
			inside = !inside;
		}
	}
	return inside;
}


} // namespace hw_plugin_impl

using namespace hw_plugin_impl;

// (made in hw_plugin.cpp: a block of the region pushed out, used by the operations and the keys)
std::vector<int> hw_make_block(hewn::Mesh &m, const std::vector<int> &region, const Vector3 &d);
