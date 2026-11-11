// Hewn editor plugin: edit mode, picking, selection, transform handles, operations and undo.
// Modal operations live in hw_modal.cpp, viewport tools in hw_tools.cpp.
#include "../hw_cutter.h"
#include "../hw_scatter.h"
#include "hw_editor.h"
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

namespace {

void toggle(std::vector<int> &v, int x, bool add, bool remove) {
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

void toggle_edge(std::vector<hewn::Edge> &v, const hewn::Edge &x, bool add, bool remove) {
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
void add_unique(std::vector<T> &v, const T &x) {
	if (std::find(v.begin(), v.end(), x) == v.end()) {
		v.push_back(x);
	}
}

void sanitize(const hewn::Mesh &m, hewn::Selection &s) {
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
bool clip_segment(const Rect2 &r, Vector2 a, Vector2 b, Vector2 &ra, Vector2 &rb, float &t0, float &t1) {
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

bool point_in_poly(const std::vector<Vector2> &poly, const Vector2 &q) {
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

} // namespace

// ------------------------------------------------------------------ lifecycle

HewnEditorPlugin *HewnEditorPlugin::singleton = nullptr;

void HewnEditorPlugin::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		singleton = this;
		i18n_init();
		// a mesh added to the scene gets the Shading view at once (not after the next check: no flash of its texture)
		get_tree()->connect("node_added", callable_mp(this, &HewnEditorPlugin::_on_node_added));
		_register_shortcuts();
		HewnTextureBrowser::register_settings();
		{
			Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
			if (es.is_valid() && OS::get_singleton()->get_environment("HW_SELFTEST").is_empty()) {
				redo_collapsed = es->get_project_metadata("hewn", "panel_collapsed", false);
				create_at_cursor = es->get_project_metadata("hewn", "create_at_cursor", false);
				// an early version: said once in each project
				if (!(bool)es->get_project_metadata("hewn", "notice_0_1", false)) {
					es->set_project_metadata("hewn", "notice_0_1", true);
					callable_mp(this, &HewnEditorPlugin::_early_notice).call_deferred();
				}
			}
		}
		gizmo_plugin.instantiate();
		gizmo_plugin->plugin = this;
		add_node_3d_gizmo_plugin(gizmo_plugin);
		entity_gizmo_plugin.instantiate();
		add_node_3d_gizmo_plugin(entity_gizmo_plugin);

		dock = memnew(HewnDock);
		dock->setup(this);
		add_dock(dock);


		texture_dock = memnew(HewnTextureBrowser);
		texture_dock->setup(this);
		add_dock(texture_dock);

		prefab_dock = memnew(HewnPrefabBrowser);
		prefab_dock->setup(this);
		add_dock(prefab_dock);
		scatter_dock = memnew(HewnScatterDock);
		scatter_dock->setup(this);
		add_dock(scatter_dock);
		scatter_dock->close(); // (opened while a Scatter is selected)

		uv_dock = memnew(EditorDock);
		uv_dock->set_title("Hewn UV");
		uv_dock->set_name("HewnUV");
		uv_dock->set_default_slot(EditorDock::DOCK_SLOT_BOTTOM);
		uv_canvas = memnew(HewnUVCanvas);
		uv_canvas->plugin = this;
		uv_canvas->set_custom_minimum_size(Vector2(0, 220));
		uv_canvas->set_clip_contents(true);
		uv_canvas->set_focus_mode(Control::FOCUS_ALL);
		uv_canvas->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		{
			VBoxContainer *uv_box = memnew(VBoxContainer);
			uv_dock->add_child(uv_box);
			uv_canvas->build_header(uv_box); // View / Select / Image / UV, element modes
			uv_box->add_child(uv_canvas);
		}
		add_dock(uv_dock);
		export_plugin.instantiate();
		add_export_plugin(export_plugin);



		obj_dialog = memnew(EditorFileDialog);
		obj_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
		obj_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
		obj_dialog->add_filter("*.obj", "Wavefront OBJ");
		obj_dialog->connect("file_selected", callable_mp(this, &HewnEditorPlugin::_on_obj_file));
		EditorInterface::get_singleton()->get_base_control()->add_child(obj_dialog);
		gltf_dialog = memnew(EditorFileDialog);
		gltf_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
		gltf_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
		gltf_dialog->add_filter("*.glb", "glTF Binary");
		gltf_dialog->add_filter("*.gltf", "glTF Text");
		gltf_dialog->connect("file_selected", callable_mp(this, &HewnEditorPlugin::_on_gltf_file));
		EditorInterface::get_singleton()->get_base_control()->add_child(gltf_dialog);

		EditorInterface::get_singleton()->get_selection()->connect("selection_changed", callable_mp(this, &HewnEditorPlugin::_on_editor_selection_changed));
		// (undo / redo: what is drawn over the viewport follows the restored mesh and selection at once)
		get_undo_redo()->connect("version_changed", callable_mp(this, &HewnEditorPlugin::_on_history_version));
		extrude_menu = memnew(PopupMenu);
		extrude_menu->add_item("Extrude Region", 0);
		extrude_menu->add_item("Extrude Region (Dissolve Orthogonal Edges)", 3);
		extrude_menu->add_item("Extrude Along Normals", 1);
		extrude_menu->add_item("Extrude Individual Faces", 2);
		extrude_menu->connect("id_pressed", callable_mp(this, &HewnEditorPlugin::_on_extrude_menu));
		EditorInterface::get_singleton()->get_base_control()->add_child(extrude_menu);
		cursor_menu = memnew(PopupMenu);
		cursor_menu->add_item("Cursor to Selected", 0);
		cursor_menu->add_item("Cursor to World Origin", 1);
		cursor_menu->add_item("Cursor to Object Origin", 2);
		cursor_menu->add_separator();
		cursor_menu->add_item("Selection to Cursor", 3);
		cursor_menu->add_separator();
		cursor_menu->add_item("Move Cursor  (G over it)", 4);
		cursor_menu->add_item("Rotate Cursor  (R over it)", 5);
		cursor_menu->connect("id_pressed", callable_mp(this, &HewnEditorPlugin::_on_cursor_menu));
		EditorInterface::get_singleton()->get_base_control()->add_child(cursor_menu);

		_place_toolbar();
		register_editor_settings(); // (the header / the tool column as the settings say)
		// the editor self-test and its probes (a script of the test project, not a plugin of its own)
		for (const char *v : { "HW_SELFTEST", "HW_ICONS", "HW_UI_SHOT", "HW_WIDTHS", "HW_HEIGHTS", "HW_PERF" }) {
			// (HW_PERF: the timing script on a heavy scene)
			const String script = String(v) == "HW_PERF" ? "res://tests/editor_perf.gd" : "res://tests/editor_selftest.gd";
			if (!selftest && !selftest_started && !OS::get_singleton()->get_environment(v).is_empty() && FileAccess::file_exists(script)) {
				Ref<Script> st = ResourceLoader::get_singleton()->load(script);
				if (st.is_valid()) {
					selftest_started = true; // (once: not again when the plugin is switched off and on)
					selftest = memnew(Node);
					selftest->set_name("HewnSelfTest");
					selftest->set_script(st);
					add_child(selftest);
				}
			}
		}
		callable_mp(this, &HewnEditorPlugin::_order_bottom_docks).call_deferred();
		localize(dock);
		localize(dock->toolbar_root);
		localize(dock->tool_column);
		localize(dock->toolbar_fold);
		localize(dock->paint_root);
		localize(dock->cutter_bar);
		localize(extrude_menu);
		localize(cursor_menu);
		for (Node *d : { (Node *)texture_dock, (Node *)prefab_dock }) {
			if (d) {
				localize(d); // (and their number fields take decimal commas)
			}
		}
		uv_dock->set_title(T("Hewn UV"));
		set_process(true);
		set_process_input(true);
		set_input_event_forwarding_always_enabled();
		set_force_draw_over_forwarding_enabled();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		if (get_tree()->is_connected("node_added", callable_mp(this, &HewnEditorPlugin::_on_node_added))) {
			get_tree()->disconnect("node_added", callable_mp(this, &HewnEditorPlugin::_on_node_added));
		}
		if (quad_added) {
			// (later: the editor is busy removing this one; when it quits it frees them all itself)
			callable_mp_static(&hw_remove_quad_plugin).call_deferred();
			quad_added = false;
		}
		if (singleton == this) {
			singleton = nullptr;
		}
		_marks_free();
		hw_gizmo_clear_cache();
		_clear_into();
		if (dock && dock->snap_fallback) {
			memdelete(dock->snap_fallback);
			dock->snap_fallback = nullptr;
			dock->snap_enabled = nullptr;
		}
		if (dock && dock->toolbar_fold) {
			if (dock->toolbar_fold->get_parent()) {
				dock->toolbar_fold->get_parent()->remove_child(dock->toolbar_fold);
			}
			memdelete(dock->toolbar_fold);
			dock->toolbar_fold = nullptr;
		}
		if (dock && dock->tool_column) {
			if (dock->tool_column->get_parent()) {
				dock->tool_column->get_parent()->remove_child(dock->tool_column);
			}
			memdelete(dock->tool_column);
			dock->tool_column = nullptr;
		}
		if (dock && dock->cutter_bar) {
			if (dock->cutter_bar->get_parent()) {
				dock->cutter_bar->get_parent()->remove_child(dock->cutter_bar);
			}
			memdelete(dock->cutter_bar);
			dock->cutter_bar = nullptr;
		}
		if (dock && dock->paint_root) {
			if (dock->paint_root->get_parent()) {
				dock->paint_root->get_parent()->remove_child(dock->paint_root);
			}
			memdelete(dock->paint_root);
			dock->paint_root = nullptr;
		}
		if (dock && dock->toolbar_root) {
			if (dock->toolbar_root->get_parent()) {
				dock->toolbar_root->get_parent()->remove_child(dock->toolbar_root);
			}
			memdelete(dock->toolbar_root);
			dock->toolbar_root = nullptr;
			dock->toolbar = nullptr;
		}
		cursor_menu->queue_free();
		cursor_menu = nullptr;
		extrude_menu->queue_free();
		extrude_menu = nullptr;
		_set_godot_gizmo_hidden(false);
		remove_node_3d_gizmo_plugin(gizmo_plugin);
		remove_node_3d_gizmo_plugin(entity_gizmo_plugin);
		entity_gizmo_plugin.unref();
		gizmo_plugin->plugin = nullptr;
		gizmo_plugin.unref();
		remove_dock(dock);
		memdelete(dock);
		dock = nullptr;
		remove_dock(uv_dock);
		if (export_plugin.is_valid()) {
			remove_export_plugin(export_plugin);
			export_plugin.unref();
		}
		memdelete(uv_dock);
		uv_dock = nullptr;
		remove_dock(texture_dock);
		memdelete(texture_dock);
		texture_dock = nullptr;
		_remove_prefab_ghost();
		remove_dock(prefab_dock);
		memdelete(prefab_dock);
		prefab_dock = nullptr;
		if (scatter_dock) {
			remove_dock(scatter_dock);
			memdelete(scatter_dock);
			scatter_dock = nullptr;
		}
		uv_canvas = nullptr;
		obj_dialog->queue_free();
		obj_dialog = nullptr;
		gltf_dialog->queue_free();
		gltf_dialog = nullptr;
		for (EditorFileDialog **d : { &bake_dialog, &scene_gltf_dialog }) {
			if (*d) {
				(*d)->queue_free();
				*d = nullptr;
			}
		}
	}
}

String HewnEditorPlugin::_get_plugin_name() const {
	return "Hewn";
}

// a closed block on `region`, `d` long: its bottom on the faces, its own
// vertices; its faces appended to `m`, the indices of its tops returned
std::vector<int> hw_make_block(hewn::Mesh &m, const std::vector<int> &region, const Vector3 &d) {
	std::unordered_map<int, int> bottom, top;
	Vector3 avg;
	for (int f : region) {
		avg += hewn::face_normal(m, m.faces[f]);
		for (int v : m.faces[f].v) {
			if (!bottom.count(v)) {
				bottom[v] = m.add_vertex(m.pos[v]);
				top[v] = m.add_vertex(m.pos[v] + d);
			}
		}
	}
	const bool outward = avg.dot(d) >= 0.0f; // (into the mesh: every new face turned around)
	std::unordered_set<uint64_t> half;
	for (int f : region) {
		const hewn::Face &F = m.faces[f];
		for (int i = 0; i < F.size(); i++) {
			half.insert(hewn::hkey(F.v[i], F.v[(i + 1) % F.size()]));
		}
	}
	std::vector<hewn::Face> made;
	std::vector<int> new_top;
	for (int f : region) {
		const hewn::Face F = m.faces[f];
		std::vector<int> bv, tv;
		for (int v : F.v) {
			bv.push_back(bottom[v]);
			tv.push_back(top[v]);
		}
		hewn::Face B = hewn::make_face(bv, &F), Tp = hewn::make_face(tv, &F);
		B.reverse(); // (the bottom looks back at the face it stands on)
		made.push_back(B);
		new_top.push_back((int)(m.faces.size() + made.size()));
		made.push_back(Tp);
		for (int i = 0; i < F.size(); i++) {
			const int a = F.v[i], b = F.v[(i + 1) % F.size()];
			if (half.count(hewn::hkey(b, a))) {
				continue; // (inside the region)
			}
			made.push_back(hewn::make_face({ bottom[a], bottom[b], top[b], top[a] }, &F));
		}
	}
	for (hewn::Face &F : made) {
		if (!outward) {
			F.reverse();
		}
		F.manual_uv = false;
		F.uv.clear();
		m.faces.push_back(F);
	}
	return new_top;
}

// (several nodes selected: Godot edits a MultiNodeEdit; with Hewn meshes among them the plugin
// stays, so Tab edits them all and G / R / S move them)
static HewnMesh3D *last_selected_mesh() {
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
	for (int64_t i = sel.size() - 1; i >= 0; i--) {
		if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(sel[i])) {
			return m;
		}
	}
	return nullptr;
}

bool HewnEditorPlugin::_handles(Object *p_object) const {
	if (p_object && p_object->get_class() == "MultiNodeEdit") {
		return last_selected_mesh() != nullptr;
	}
	return Object::cast_to<HewnMesh3D>(p_object) != nullptr || Object::cast_to<Path3D>(p_object) != nullptr || // (a Path3D: its points with Tab)
			Object::cast_to<HewnScatter3D>(p_object) != nullptr || // (a Scatter: its brushes)
			Object::cast_to<HewnCutter3D>(p_object) != nullptr; // (a cutter: G / R / S with the snapping)
}

HewnMesh3D *HewnEditorPlugin::get_edited() const {
	if (edited_id == 0) {
		return nullptr;
	}
	// (another scene tab: its nodes are out of the tree until it comes back)
	HewnMesh3D *n = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(edited_id));
	return n && n->is_inside_tree() ? n : nullptr;
}

std::vector<HewnMesh3D *> HewnEditorPlugin::edited_meshes() const {
	std::vector<HewnMesh3D *> out;
	if (HewnMesh3D *a = get_edited()) {
		out.push_back(a);
	}
	for (uint64_t id : multi_ids) {
		HewnMesh3D *m = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(id)));
		if (m && m->is_inside_tree() && (out.empty() || m != out[0])) {
			out.push_back(m);
		}
	}
	return out;
}

bool HewnEditorPlugin::is_edited_mesh(const HewnMesh3D *p_node) const {
	if (!p_node) {
		return false;
	}
	if (p_node->get_instance_id() == edited_id) {
		return true;
	}
	for (uint64_t id : multi_ids) {
		if (id == p_node->get_instance_id()) {
			return true;
		}
	}
	return false;
}

// Another of the edited meshes becomes the active one (a click on its elements).
void HewnEditorPlugin::_set_active_mesh(HewnMesh3D *p_node) {
	if (!p_node || p_node->get_instance_id() == edited_id) {
		return;
	}
	for (uint64_t &id : multi_ids) {
		if (id == p_node->get_instance_id()) {
			id = edited_id;
			break;
		}
	}
	edited_id = p_node->get_instance_id();
	hover_vert = hover_face = -1;
	hover_edge = hewn::Edge(-1, -1);
	if (EditorInterface::get_singleton()->get_inspector()) {
		EditorInterface::get_singleton()->get_inspector()->call_deferred("edit", p_node);
	}
	for (HewnMesh3D *m : edited_meshes()) {
		m->update_gizmos();
	}
}

void HewnEditorPlugin::_clear_multi() {
	std::vector<HewnMesh3D *> others = edited_meshes();
	multi_ids.clear();
	for (HewnMesh3D *m : others) {
		m->update_gizmos();
	}
}

void HewnEditorPlugin::_edit(Object *p_object) {
	if (!p_object && hc_detached && EditorInterface::get_singleton()->get_selection()->get_selected_nodes().is_empty()) {
		// (our own: the curve taken out of the selection for its edit mode; the inspector shows it again)
		if (Object *hc = ObjectDB::get_instance(curve_id)) {
			EditorInterface::get_singleton()->get_inspector()->call_deferred("edit", hc);
		}
		return;
	}
	if (p_object && hc_detached && p_object != ObjectDB::get_instance(curve_id)) {
		hc_detached = false; // (something else selected: out of the curve's edit mode)
	}
	{
		Path3D *cp = Object::cast_to<Path3D>(p_object);
		const ObjectID nid = cp ? ObjectID(cp->get_instance_id()) : ObjectID();
		if (nid != curve_id) {
			curve_edit = false;
			curve_sel.clear();
			curve_xf = 0;
			hc_edit = false;
			hc_xf = 0;
			hc_draw = false;
		}
		curve_id = nid;
		HewnScatter3D *sc = Object::cast_to<HewnScatter3D>(p_object);
		const ObjectID sid = sc ? ObjectID(sc->get_instance_id()) : ObjectID();
		if (sid != scatter_id) {
			if (scatter_painting) {
				_scatter_end();
			}
			scatter_tool = sc && scatter_tool_next ? 0 : sc ? scatter_tool : -1;
			scatter_tool_next = false;
		}
		scatter_id = sid;
		if (scatter_dock) {
			scatter_dock->show_scatter(sc);
		}
	}
	HewnMesh3D *old = get_edited();
	HewnMesh3D *n = Object::cast_to<HewnMesh3D>(p_object);
	if (!n && p_object && p_object->get_class() == "MultiNodeEdit") {
		n = last_selected_mesh();
	}
	if (n && n == old) {
		selection_changed(); // re-edit of the same node (selection refresh)
		return;
	}
	if (!n && detached) {
		// We took the node out of Godot's selection ourselves (edit mode). Godot has just cleared the
		// inspector; show the mesh there again.
		if (old) {
			EditorInterface::get_singleton()->get_inspector()->call_deferred("edit", old);
		}
		return;
	}
	if (modal != MODAL_NONE && modal != MODAL_POLY) {
		cancel_tool();
	}
	detached = false; // another node got selected: the selection is already where it should be
	_clear_multi();
	_check_brush_click(n);
	edited_id = n ? n->get_instance_id() : 0;
	mode = MODE_OBJECT;
	hover_vert = hover_face = -1;
	hover_edge = hewn::Edge(-1, -1);
	drag = DRAG_NONE;
	if (old && old != n) {
		old->update_gizmos();
	}
	selection_changed();
}

void HewnEditorPlugin::_make_visible(bool p_visible) {
	if (!p_visible && !detached) {
		_edit(nullptr);
	}
}

void HewnEditorPlugin::selection_changed() {
	HW_PROF("selection_changed");
	HewnMesh3D *n = get_edited();
	if (n) {
		sanitize(n->get_data(), n->selection);
		n->update_gizmos();
	}
	if (dock) {
		dock->refresh();
	}
	if (uv_canvas) {
		uv_canvas->queue_redraw();
	}
	update_overlays();
}

// In edit mode the mesh is taken out of Godot's selection (the inspector keeps showing it), so Godot
// draws neither its transform gizmo nor the orange bounding box. Nothing in the node or in the editor
// settings is changed. Leaving edit mode selects the node again.
void HewnEditorPlugin::_set_godot_gizmo_hidden(bool p_hidden) {
	HewnMesh3D *n = get_edited();
	EditorInterface *ei = EditorInterface::get_singleton();
	EditorSelection *es = ei->get_selection();
	if (p_hidden) {
		if (!n || detached) {
			return;
		}
		detached = true;
		es->clear();
		// The inspector is set back to the mesh in _edit(nullptr), which Godot calls after clearing it
		// (EditorInterface::inspect_object would select the node again).
	} else {
		if (!detached) {
			return;
		}
		detached = false;
		if (n && n->is_inside_tree()) {
			es->clear();
			es->add_node(n);
		}
	}
}

void HewnEditorPlugin::_early_notice() {
	if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
		t->push_toast(T("Hewn is an early version: save your scenes often"), EditorToaster::SEVERITY_WARNING);
	}
}

void HewnEditorPlugin::_on_history_version() {
	for (HewnMesh3D *m : edited_meshes()) {
		m->update_gizmos();
	}
	update_overlays();
	if (dock) {
		dock->refresh();
	}
	if (uv_canvas) {
		uv_canvas->queue_redraw(); // (an undone UV change)
	}
}

void HewnEditorPlugin::_on_editor_selection_changed() {
	{
		// the order things were selected in
		const TypedArray<Node> now = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
		std::vector<ObjectID> keep;
		for (const ObjectID &id : sel_order) {
			for (int64_t i = 0; i < now.size(); i++) {
				if (Object::cast_to<Node>(now[i]) && ObjectID(Object::cast_to<Node>(now[i])->get_instance_id()) == id) {
					keep.push_back(id);
					break;
				}
			}
		}
		for (int64_t i = 0; i < now.size(); i++) {
			Node *nd = Object::cast_to<Node>(now[i]);
			if (nd && std::find(keep.begin(), keep.end(), ObjectID(nd->get_instance_id())) == keep.end()) {
				keep.push_back(ObjectID(nd->get_instance_id()));
			}
		}
		sel_order = keep;
	}
	if (dock && dock->ov_wireframe && dock->ov_wireframe->is_pressed()) {
		dock->update_all_gizmos(); // the selected meshes' wireframe is highlighted
	}
	// The user selected something else (scene tree, another viewport click...): leave edit mode.
	if (!detached) {
		return;
	}
	TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
	if (sel.is_empty()) {
		return; // our own clear()
	}
	detached = false;
	if (modal != MODAL_NONE && modal != MODAL_POLY) {
		cancel_tool();
	}
	mode = MODE_OBJECT;
	selection_changed();
}

// The edited node can disappear without any selection change (deleted, scene tab switched).
void HewnEditorPlugin::_validate_edited() {
	if (!detached) {
		return;
	}
	HewnMesh3D *n = get_edited();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (n && n->is_inside_tree() && root && (n == root || root->is_ancestor_of(n))) {
		return;
	}
	detached = false;
	if (modal != MODAL_NONE && modal != MODAL_POLY) {
		cancel_tool();
	}
	edited_id = 0;
	mode = MODE_OBJECT;
	selection_changed();
}

void HewnEditorPlugin::set_mode(int p_mode) {
	const Mode old = mode;
	Mode nm = (Mode)CLAMP(p_mode, 0, 3);
	HewnMesh3D *n = get_edited();
	if ((nm == MODE_OBJECT || !n) && sculpt) {
		// leaving Sculpt
		if (sculpting) {
			sculpting = false;
			end_edit();
		}
		sculpt = false;
		if (modal == MODAL_SCULPT) {
			modal = MODAL_NONE;
		}
		if (dock) {
			dock->show_paint_bar(false);
		}
	}
	if ((nm == MODE_OBJECT || !n) && vpaint) {
		// leaving Vertex Paint
		if (painting) {
			painting = false;
			end_edit();
		}
		vpaint = false;
		if (modal == MODAL_PAINT) {
			modal = MODAL_NONE;
		}
		apply_paint_view();
		if (dock) {
			dock->show_paint_bar(false);
		}
	}
	if (nm != MODE_OBJECT) {
		element_mode = nm;
	}
	if (!n) {
		nm = MODE_OBJECT;
	}
	if (modal != MODAL_NONE && modal != MODAL_POLY && !((modal == MODAL_PAINT || modal == MODAL_SCULPT) && nm != MODE_OBJECT)) {
		cancel_tool(); // (the paint tools stay on: vertices / edges / faces only say what they paint)
	}
	if (n && old != nm && old != MODE_OBJECT && nm != MODE_OBJECT) {
		hewn::Mesh &m = n->get_data();
		hewn::Selection &s = n->selection;
		hewn::Topology t;
		t.build(m);
		std::vector<int> verts = old == MODE_VERTEX ? s.verts : old == MODE_EDGE ? hewn::sel::verts_of_edges(s.edges) : hewn::sel::verts_of_faces(m, s.faces);
		if (nm == MODE_VERTEX) {
			s.verts = verts;
		} else if (nm == MODE_EDGE) {
			s.edges = old == MODE_FACE ? hewn::sel::edges_of_faces(m, s.faces) : hewn::sel::edges_from_verts(m, t, verts);
		} else {
			s.faces = hewn::sel::faces_from_verts(m, verts, true);
		}
	}
	if (old == MODE_OBJECT && nm != MODE_OBJECT && n) {
		// Tab with several meshes selected: all of them are edited
		multi_ids.clear();
		for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_selected_nodes()) {
			HewnMesh3D *o = Object::cast_to<HewnMesh3D>(v);
			if (o && o != n && !o->is_csg_model()) {
				multi_ids.push_back(o->get_instance_id());
				o->update_gizmos();
			}
		}
	}
	const std::vector<HewnMesh3D *> leaving = nm == MODE_OBJECT ? edited_meshes() : std::vector<HewnMesh3D *>();
	mode = nm;
	if (nm != MODE_OBJECT) {
		mode_mask = 1 << nm;
	}
	if (!redo_running) {
		_hide_redo();
	}
	set_xray(xray_on);
	_set_godot_gizmo_hidden(mode != MODE_OBJECT);
	if (nm == MODE_OBJECT && leaving.size() > 1) {
		// back to objects: all the edited ones selected again
		EditorSelection *es = EditorInterface::get_singleton()->get_selection();
		for (HewnMesh3D *m : leaving) {
			es->add_node(m);
		}
		_clear_multi();
	}
	hover_vert = hover_face = -1;
	hover_edge = hewn::Edge(-1, -1);
	drag = DRAG_NONE;
	selection_changed();
}

// X-ray: the edited mesh becomes see-through and hidden elements can be selected.
void HewnEditorPlugin::set_xray(bool p_on) {
	xray_on = p_on;
	if (dock && dock->xray->is_pressed() != p_on) {
		dock->xray->set_pressed_no_signal(p_on);
	}
	// see-through: the edited meshes in edit mode, every mesh of the scene otherwise (drawing a cut, a shape)
	for (const ObjectID &id : xray_meshes) {
		if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(id))) {
			if (m->get_instance().is_valid()) {
				RenderingServer::get_singleton()->instance_geometry_set_transparency(m->get_instance(), m->get_transparency());
				m->refresh_display(); // (a boolean operand or brush is see-through again)
			}
		}
	}
	xray_meshes.clear();
	if (p_on) {
		std::vector<HewnMesh3D *> list = mode != MODE_OBJECT ? edited_meshes() : _scene_meshes();
		for (HewnMesh3D *m : list) {
			if (m && m->get_instance().is_valid()) {
				RenderingServer::get_singleton()->instance_geometry_set_transparency(m->get_instance(), MAX(0.6f, m->get_transparency()));
				xray_meshes.push_back(ObjectID(m->get_instance_id()));
			}
		}
	}
	update_overlays();
}

// Shift+1/2/3 or Shift+click on a mode button: several element modes at once.
void HewnEditorPlugin::toggle_mode(int p_mode) {
	HewnMesh3D *n = get_edited();
	if (!n || p_mode < MODE_VERTEX || p_mode > MODE_FACE) {
		return;
	}
	if (mode == MODE_OBJECT) {
		set_mode(p_mode);
		return;
	}
	const int bit = 1 << p_mode;
	if (mode_mask & bit) {
		if (mode_mask == bit) {
			return; // keep at least one
		}
		mode_mask &= ~bit;
		for (int m = MODE_FACE; m >= MODE_VERTEX; m--) {
			if (mode_mask & (1 << m)) {
				mode = (Mode)m;
				break;
			}
		}
	} else {
		// the new element type starts with what the current selection covers
		hewn::Mesh &m = n->get_data();
		hewn::Selection &s = n->selection;
		hewn::Topology t;
		t.build(m);
		const std::vector<int> verts = _selected_verts();
		if (p_mode == MODE_VERTEX) {
			s.verts = verts;
		} else if (p_mode == MODE_EDGE) {
			s.edges = hewn::sel::edges_from_verts(m, t, verts);
		} else {
			s.faces = hewn::sel::faces_from_verts(m, verts, true);
		}
		mode_mask |= bit;
		mode = (Mode)p_mode;
	}
	element_mode = mode;
	selection_changed();
}

void HewnEditorPlugin::toggle_edit_mode() {
	if (vpaint) {
		exit_vpaint(); // (Tab in Vertex Paint: back to object mode)
		return;
	}
	set_mode(mode == MODE_OBJECT ? element_mode : MODE_OBJECT);
}

// The plugin snaps when Godot's own snap (magnet) toggle of the 3D toolbar is on.
void HewnEditorPlugin::_find_godot_snap(Node *p_row) {
	Ref<Texture2D> magnet = EditorInterface::get_singleton()->get_editor_theme()->get_icon("Snap", "EditorIcons");
	std::vector<Node *> stack = { p_row };
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		Button *b = Object::cast_to<Button>(x);
		if (b && b->is_toggle_mode() && b->get_button_icon() == magnet) {
			dock->snap_enabled = b;
			return;
		}
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
	}
}

// Objects selected (object mode) or elements of the edited meshes (edit mode / Vertex Paint).
bool HewnEditorPlugin::_anything_selected() const {
	if (mode == MODE_OBJECT) {
		return !EditorInterface::get_singleton()->get_selection()->get_selected_nodes().is_empty();
	}
	for (HewnMesh3D *m : edited_meshes()) {
		if (!m->selection.verts.empty() || !m->selection.edges.empty() || !m->selection.faces.empty()) {
			return true;
		}
	}
	return false;
}

// The camera onto the selection, turned as it is (Godot's own Focus Selection of the viewport's View menu:
// it moves the camera smoothly).
void HewnEditorPlugin::focus_selection(Camera3D *p_cam) {
	Control *surface = _surface_for(p_cam);
	if (!surface) {
		return;
	}
	for (const Variant &v : surface->find_children("*", "MenuButton", true, false)) {
		MenuButton *mb = Object::cast_to<MenuButton>(v);
		if (!mb) {
			continue;
		}
		PopupMenu *pm = mb->get_popup();
		for (int i = 0; i < pm->get_item_count(); i++) {
			Ref<Shortcut> sc = pm->get_item_shortcut(i);
			if (sc.is_valid() && (sc->get_name() == "Focus Selection" || pm->get_item_text(i) == "Focus Selection")) {
				pm->emit_signal("id_pressed", pm->get_item_id(i));
				return;
			}
		}
	}
}

// Puts the Hewn toolbar on its own row right under the 3D editor's toolbar.
void HewnEditorPlugin::_place_toolbar() {
	Control *bar = dock->toolbar_root;
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, bar);
	Node *row = bar->get_parent();
	// Walk up to the child of the first vertical box: that is the toolbar row; insert ours after it.
	while (row && row->get_parent()) {
		VBoxContainer *vb = Object::cast_to<VBoxContainer>(row->get_parent());
		if (vb) {
			remove_control_from_container(CONTAINER_SPATIAL_EDITOR_MENU, bar);
			// Floating over the viewports (top level: ignored by the box layout), so showing or hiding
			// it never resizes the viewports.
			vb->add_child(bar);
			bar->set_as_top_level(true);
			bar->set_z_index(10);
			vb->add_child(dock->tool_column);
			dock->tool_column->set_as_top_level(true);
			dock->tool_column->set_z_index(10);
			vb->add_child(dock->toolbar_fold);
			dock->toolbar_fold->set_as_top_level(true);
			dock->toolbar_fold->set_z_index(10);
			vb->add_child(dock->paint_root);
			dock->paint_root->set_as_top_level(true);
			dock->paint_root->set_z_index(10);
			vb->add_child(dock->cutter_bar);
			dock->cutter_bar->set_as_top_level(true);
			dock->cutter_bar->set_z_index(10);
			toolbar_row_id = row->get_instance_id();
			_find_godot_snap(row);
			_layout_toolbar();
			return;
		}
		row = row->get_parent();
	}
}

void HewnEditorPlugin::_layout_toolbar() {
	Control *bar = dock ? dock->toolbar_root : nullptr;
	Control *row = toolbar_row_id ? Object::cast_to<Control>(ObjectDB::get_instance(toolbar_row_id)) : nullptr;
	if (!bar || !row || !row->is_visible_in_tree()) {
		bar_shown = false;
		return;
	}
	const float margin = 8.0f;
	const Rect2 r = row->get_global_rect();
	const Vector2 content = dock->toolbar->get_combined_minimum_size();
	Control *fold = dock->toolbar_fold;
	const Vector2 fold_size = fold ? fold->get_combined_minimum_size() : Vector2();
	// the header's height as when it is open: folding it moves nothing else (the tool column stays)
	if (content.x >= 1.0f && content.y > 0.0f) {
		header_h = content.y;
	}
	const float hh = MAX(header_h, MAX(content.y, fold_size.y));
	const float fold_w = fold ? fold_size.x + 6.0f : 0.0f;
	// In the row of the viewport's own view menu ("Perspective" ...): right of it, left of the
	// navigation gizmo in the top right corner. Fallback: under the view menu.
	Vector2 pos(r.position.x + margin, r.position.y + r.size.y + 36.0f);
	float avail = r.size.x - margin * 2.0f;
	Control *surface = _main_surface();
	if (surface && surface->is_visible_in_tree()) {
		const Rect2 sr = surface->get_global_rect();
		// (the view menu is looked up once per viewport, not every frame)
		Control *menu = view_menu_id ? Object::cast_to<Control>(ObjectDB::get_instance(view_menu_id)) : nullptr;
		if (!menu || !surface->is_ancestor_of(menu)) {
			menu = nullptr;
			const TypedArray<Node> menus = surface->find_children("*", "MenuButton", true, false);
			for (int64_t i = 0; i < menus.size(); i++) {
				Control *c = Object::cast_to<Control>(menus[i]);
				if (!c || !c->is_visible_in_tree() || c->get_global_position().y > sr.position.y + 60.0f || c->get_global_position().x > sr.position.x + sr.size.x * 0.5f) {
					continue;
				}
				if (!menu || c->get_global_position().x < menu->get_global_position().x) {
					menu = c;
				}
			}
			view_menu_id = menu ? menu->get_instance_id() : 0;
		}
		if (menu) {
			const Rect2 mr = menu->get_global_rect();
			pos = Vector2(mr.position.x + mr.size.x + margin, mr.position.y + (mr.size.y - hh) * 0.5f);
			avail = sr.position.x + sr.size.x - 110.0f - pos.x; // the navigation gizmo
		}
	}
	if (fold) {
		const Vector2 fpos(pos.x, pos.y + (hh - fold_size.y) * 0.5f);
		if (fold->get_global_position() != fpos) {
			fold->set_global_position(fpos);
		}
		if (fold->get_size() != fold_size) {
			fold->set_size(fold_size);
		}
		pos.x += fold_w;
		avail -= fold_w;
	}
	const bool folded = content.x < 1.0f;
	const Vector2 size(MIN(content.x, MAX(folded ? 0.0f : 40.0f, avail)), hh);
	if (bar->get_global_position() != pos) {
		bar->set_global_position(pos);
	}
	if (bar->get_size() != size) {
		bar->set_size(size);
	}
	if (ScrollContainer *sc = Object::cast_to<ScrollContainer>(bar)) {
		if (!bar_shown || (content.x <= size.x + 0.5f && sc->get_h_scroll() != 0)) {
			sc->set_h_scroll(0); // from the start when shown, or when everything fits again
		}
		bar_shown = true;
	}
	// the second row (Vertex Paint's, the Box Cutter's): under the header, its own width and scrolling. It is
	// right of the view menu, the tool column left of it: the column does not move down for it.
	float extra = 0.0f;
	for (Control *second : { (Control *)dock->paint_root, dock->cutter_bar }) {
		if (!second || !second->is_visible()) {
			continue;
		}
		Control *inner = Object::cast_to<Control>(second->get_child(0));
		const Vector2 pc = inner ? inner->get_combined_minimum_size() : second->get_combined_minimum_size();
		extra = MAX(extra, pc.y + 6.0f); // (the header's text goes under this row)
		const Vector2 ppos(pos.x - fold_w, pos.y + hh + 4.0f);
		const Vector2 psize(MIN(pc.x, MAX(40.0f, avail + fold_w)), pc.y);
		if (second->get_global_position() != ppos) {
			second->set_global_position(ppos);
		}
		if (second->get_size() != psize) {
			second->set_size(psize);
		}
	}
	if (hw_editor::header_extra() != extra) {
		hw_editor::header_extra() = extra;
		update_overlays();
	}
	// the tool column: left edge of the viewport, under the header row
	Control *col = dock->tool_column;
	if (col && col->is_visible_in_tree() && surface && surface->is_visible_in_tree()) {
		const Rect2 sr = surface->get_global_rect();
		const float cy = MAX(pos.y + hh + 8.0f, sr.position.y + 8.0f);
		// (folded: only the small arrow is left, above the zoom bar; it stays at the edge)
		const bool col_open = dock->tool_body && dock->tool_body->is_visible();
		const float cx = col_open ? _left_offset(surface, cy - sr.position.y, col->get_combined_minimum_size().y, true, -8.0f) : _left_offset(surface, 0, 0, false);
		const Vector2 cpos(sr.position.x + cx, cy);
		if (col->get_global_position() != cpos) {
			col->set_global_position(cpos);
		}
		const Vector2 csize = col->get_combined_minimum_size();
		if (col->get_size() != csize) {
			col->set_size(csize);
		}
		const int at = active_tool();
		bool resized = false;
		for (int i = 0; i < TOOLCOL_COUNT; i++) {
			Button *b = dock->tool_buttons[i];
			if (!b) {
				continue;
			}
			if (b->is_pressed() != (i == at)) {
				b->set_pressed_no_signal(i == at);
			}
			Control *bl = Object::cast_to<Control>(b->get_parent() ? b->get_parent()->get_parent() : nullptr);
			const bool shown = tool_shown(i);
			if (bl && bl->is_visible() != shown) {
				bl->set_visible(shown);
				resized = true;
			}
		}
		if (resized) {
			col->reset_size();
		}
		const bool mesh_ok = get_edited() != nullptr;
		for (int i = TOOLCOL_KNIFE; i <= TOOLCOL_SPIN; i++) {
			dock->tool_buttons[i]->set_disabled(!mesh_ok && i != TOOLCOL_BISECT); // (the clip works on any mesh)
		}
		if (dock->cutter_bar) {
			const bool cut = at == TOOLCOL_CUTTER && !vpaint && !dock->toolbar_folded && show_header;
			if (dock->cutter_bar->is_visible() != cut) {
				dock->cutter_bar->set_visible(cut);
			}
		}
	}
	if (dock->mesh_menu) {
		dock->mesh_menu->set_disabled(mode == MODE_OBJECT || !get_edited());
	}
	if (dock->curve_menu) {
		// (shown while a curve is selected or edited)
		bool curve = _hc() != nullptr && hc_edit;
		for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_selected_nodes()) {
			curve = curve || Object::cast_to<HewnCurve3D>(v) != nullptr;
		}
		Control *blk = Object::cast_to<Control>(dock->curve_menu->get_parent());
		Control *outer = blk ? Object::cast_to<Control>(blk->get_parent()) : nullptr;
		if (outer && outer->is_visible() != curve) {
			outer->set_visible(curve);
		}
	}
}

// The TODO notes of the edited scene: a pin at each, its text beside it (the first lines; done ones dimmed).
void HewnEditorPlugin::_draw_notes(Control *p_overlay, Camera3D *p_cam) {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	Ref<Font> font = p_overlay->get_theme_default_font();
	if (!root || font.is_null()) {
		return;
	}
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const int fs = p_overlay->get_theme_default_font_size();
	for (HewnNote3D *nt : HewnNote3D::all) {
		if (!nt->is_inside_tree() || !nt->is_visible_in_tree() || !(nt == root || root->is_ancestor_of(nt))) {
			continue;
		}
		const Vector3 w = nt->get_global_position();
		if (p_cam->is_position_behind(w)) {
			continue;
		}
		const Vector2 c = p_cam->unproject_position(w);
		Color col = nt->get_color();
		if (nt->get_done()) {
			col.a = 0.4f;
		}
		p_overlay->draw_circle(c, 6.0f * es, Color(0, 0, 0, 0.6f * col.a));
		p_overlay->draw_circle(c, 4.5f * es, col);
		PackedStringArray lines = nt->get_text().split("\n");
		String t;
		for (int64_t i = 0; i < lines.size() && i < 3; i++) {
			String l = lines[i];
			if (l.length() > 48) {
				l = l.substr(0, 46) + "...";
			}
			t += (i ? "\n" : "") + l;
		}
		if (t.strip_edges().is_empty()) {
			continue;
		}
		const Vector2 at = c + Vector2(10.0f * es, -4.0f * es);
		const Vector2 sz = font->get_multiline_string_size(t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs);
		p_overlay->draw_rect(Rect2(at - Vector2(4, font->get_ascent(fs) + 3), sz + Vector2(8, 6)), Color(0.08f, 0.08f, 0.1f, 0.8f * col.a));
		p_overlay->draw_multiline_string(font, at, t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, -1, Color(col.r, col.g, col.b, MAX(0.5f, col.a)));
	}
}

// The Non-planar faces overlay: every face of 4+ corners of the meshes shown whose corners are not on one plane
// (further from the plane through its middle than a hair of its size), filled red.
void HewnEditorPlugin::_draw_nonplanar(Control *p_overlay, Camera3D *p_cam) {
	int budget = 20000;
	const Color red(1.0f, 0.1f, 0.1f, 0.45f), edge(1.0f, 0.15f, 0.15f, 0.95f);
	for (HewnMesh3D *n : _scene_meshes()) {
		if (!n->is_visible_in_tree() || budget <= 0) {
			continue;
		}
		const hewn::Mesh &m = n->get_data();
		const Transform3D g = n->get_global_transform();
		for (const hewn::Face &F : m.faces) {
			const int k = F.size();
			if (k < 4 || --budget <= 0) {
				continue;
			}
			Vector3 c, nrm;
			float size = 0.0f;
			for (int i = 0; i < k; i++) {
				const Vector3 a = m.pos[F.v[i]], b = m.pos[F.v[(i + 1) % k]];
				c += a / (float)k;
				nrm += Vector3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
				size = MAX(size, a.distance_to(b));
			}
			if (nrm.length() < 1e-12f) {
				continue;
			}
			nrm.normalize();
			bool bent = false;
			for (int i = 0; i < k && !bent; i++) {
				bent = std::fabs((m.pos[F.v[i]] - c).dot(nrm)) > MAX(1e-5f, size * 1e-4f);
			}
			if (!bent) {
				continue;
			}
			PackedVector2Array pts;
			bool behind = false;
			for (int i = 0; i < k; i++) {
				const Vector3 w = g.xform(m.pos[F.v[i]]);
				behind = behind || p_cam->is_position_behind(w);
				pts.push_back(p_cam->unproject_position(w));
			}
			if (behind) {
				continue;
			}
			draw_poly_safe(p_overlay, pts, red);
			for (int i = 0; i < k; i++) {
				p_overlay->draw_line(pts[i], pts[(i + 1) % k], edge, 1.5f, true);
			}
		}
	}
}

// ------------------------------------------------------------------ tool column / header menus

// Object mode: selecting and measuring; edit mode: also the mesh tools; Vertex Paint: the paint tools and
// the selection (the mask).
bool HewnEditorPlugin::tool_shown(int p_tool) const {
	const bool paint_tool = p_tool >= TOOLCOL_BRUSH && p_tool <= TOOLCOL_NORMALS;
	const bool sculpt_tool_id = p_tool >= TOOLCOL_SC_DRAW && p_tool <= TOOLCOL_SC_DENSITY;
	if (sculpt) {
		return sculpt_tool_id && (p_tool != TOOLCOL_SC_DENSITY || (dock && dock->sculpt_dyntopo && dock->sculpt_dyntopo->is_pressed()));
	}
	if (sculpt_tool_id) {
		return false;
	}
	if (vpaint) {
		return paint_tool || p_tool == TOOLCOL_SELECT || p_tool == TOOLCOL_SEL_CIRCLE || p_tool == TOOLCOL_SEL_LASSO || p_tool == TOOLCOL_CURSOR;
	}
	if (paint_tool) {
		return false;
	}
	if (p_tool == TOOLCOL_CURVE_DRAW || (hc_edit && _hc())) {
		// (a Hewn curve's edit mode: selecting, the cursor and drawing)
		return hc_edit && _hc() && (p_tool == TOOLCOL_SELECT || p_tool == TOOLCOL_CURSOR || p_tool == TOOLCOL_CURVE_DRAW);
	}
	const bool scatter_tool_id = p_tool >= TOOLCOL_SCATTER_ADD && p_tool <= TOOLCOL_SCATTER_THIN;
	if (scatter_tool_id) {
		return get_scatter() != nullptr && mode == MODE_OBJECT;
	}
	if (mode == MODE_OBJECT || !get_edited()) {
		return p_tool == TOOLCOL_SELECT || p_tool == TOOLCOL_SEL_CIRCLE || p_tool == TOOLCOL_CURSOR || p_tool == TOOLCOL_RULER || p_tool == TOOLCOL_BISECT; // (the circle: objects too; Bisect there: the clip)
	}
	return true;
}

int HewnEditorPlugin::active_tool() const {
	if (modal == MODAL_SCULPT) {
		return TOOLCOL_SC_DRAW + CLAMP(sculpt_tool, 0, (int)ST_COUNT - 1);
	}
	if (modal == MODAL_PAINT) {
		return TOOLCOL_BRUSH + CLAMP(paint_tool, 0, 4);
	}
	if (modal == MODAL_NONE && scatter_tool >= 0 && get_scatter()) {
		return TOOLCOL_SCATTER_ADD + scatter_tool;
	}
	if (hc_edit && hc_draw && _hc()) {
		return TOOLCOL_CURVE_DRAW;
	}
	if (modal == MODAL_NONE && (mode != MODE_OBJECT || select_kind == 1) && select_kind > 0) {
		return select_kind == 1 ? TOOLCOL_SEL_CIRCLE : TOOLCOL_SEL_LASSO; // (the circle: objects too)
	}
	switch (modal) {
		case MODAL_CURSOR:
			return TOOLCOL_CURSOR;
		case MODAL_RULER:
			return TOOLCOL_RULER;
		case MODAL_KNIFE:
			return TOOLCOL_KNIFE;
		case MODAL_LOOPCUT:
			return TOOLCOL_LOOPCUT;
		case MODAL_CUTTER:
			return TOOLCOL_CUTTER;
		case MODAL_BISECT:
		case MODAL_CLIP:
			return TOOLCOL_BISECT;
		case MODAL_SPIN:
			return TOOLCOL_SPIN;
		default:
			return TOOLCOL_SELECT;
	}
}

void HewnEditorPlugin::select_tool(int p_tool) {
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	if (p_tool == TOOLCOL_CURVE_DRAW) {
		hc_op("draw");
		update_overlays();
		return;
	}
	if (p_tool == TOOLCOL_SELECT && hc_draw) {
		hc_draw = false;
		hc_draw_pts.clear();
	}
	const bool mesh_tool = p_tool == TOOLCOL_KNIFE || p_tool == TOOLCOL_LOOPCUT || p_tool == TOOLCOL_CUTTER || p_tool == TOOLCOL_BISECT || p_tool == TOOLCOL_SPIN;
	if (mesh_tool && get_edited() && mode == MODE_OBJECT) {
		set_mode(element_mode); // the mesh tools work in edit mode
	}
	if (scatter_painting) {
		_scatter_end();
	}
	scatter_tool = -1;
	if (p_tool >= TOOLCOL_SCATTER_ADD && p_tool <= TOOLCOL_SCATTER_THIN) {
		scatter_tool = get_scatter() ? p_tool - TOOLCOL_SCATTER_ADD : -1;
		update_overlays();
		return;
	}
	if (p_tool >= TOOLCOL_SC_DRAW && p_tool <= TOOLCOL_SC_DENSITY) {
		start_sculpt_tool(p_tool - TOOLCOL_SC_DRAW);
		update_overlays();
		return;
	}
	if (p_tool >= TOOLCOL_BRUSH && p_tool <= TOOLCOL_NORMALS) {
		start_paint_tool(p_tool - TOOLCOL_BRUSH);
		update_overlays();
		return;
	}
	if (p_tool == TOOLCOL_SELECT || p_tool == TOOLCOL_SEL_CIRCLE || p_tool == TOOLCOL_SEL_LASSO) {
		select_kind = p_tool == TOOLCOL_SEL_CIRCLE ? 1 : p_tool == TOOLCOL_SEL_LASSO ? 2 : 0;
	}
	switch (p_tool) {
		case TOOLCOL_CURSOR:
			start_cursor_tool();
			break;
		case TOOLCOL_RULER:
			start_ruler();
			break;
		case TOOLCOL_KNIFE:
			start_knife();
			break;
		case TOOLCOL_LOOPCUT:
			start_loop_cut();
			break;
		case TOOLCOL_CUTTER:
			start_cutter();
			break;
		case TOOLCOL_BISECT:
			start_bisect();
			break;
		case TOOLCOL_SPIN:
			start_spin_tool();
			break;
		default:
			break;
	}
	if (Camera3D *cam = _viewport_camera()) {
		_focus_viewport(cam);
	}
	update_overlays();
}

void HewnEditorPlugin::run_bar_op(const String &p_op) {
	Camera3D *cam = _viewport_camera();
	if (p_op == "edit_mode" || p_op.begins_with("modal:") || p_op.begins_with("menu:") || p_op.begins_with("hc:") || p_op.begins_with("cobj:")) {
		_run_menu_op(cam, p_op);
	} else {
		run_op(p_op);
	}
	if (cam) {
		_focus_viewport(cam);
	}
}

void HewnEditorPlugin::_on_mesh_file(const String &p_path) {
	HewnMesh3D *n = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(mesh_file_node));
	if (!n || n->get_mesh().is_null()) {
		return;
	}
	Ref<Mesh> copy = n->get_mesh()->duplicate(true);
	const Error err = ResourceSaver::get_singleton()->save(copy, p_path);
	if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
		t->push_toast(err == OK ? vformat(T("Saved: %s"), p_path) : vformat(T("Could not save %s"), p_path), err == OK ? EditorToaster::SEVERITY_INFO : EditorToaster::SEVERITY_ERROR);
	}
	EditorInterface::get_singleton()->get_resource_filesystem()->scan();
}

// The camera of the viewport used last (or the first one).
Camera3D *HewnEditorPlugin::_viewport_camera() const {
	Control *surface = last_surface ? Object::cast_to<Control>(ObjectDB::get_instance(last_surface)) : nullptr;
	if (!surface) {
		surface = _main_surface();
	}
	return surface ? _camera_for_overlay(surface) : nullptr;
}

void HewnEditorPlugin::set_tool(int p_tool) {
	if (p_tool == TOOL_SELECT) {
		dock->show_gizmo->set_pressed(false);
	} else {
		tool = (Tool)CLAMP(p_tool, 0, 2);
		dock->show_gizmo->set_pressed(true);
		dock->gizmo_move->set_pressed_no_signal(tool == TOOL_MOVE);
		dock->gizmo_rotate->set_pressed_no_signal(tool == TOOL_ROTATE);
		dock->gizmo_scale->set_pressed_no_signal(tool == TOOL_SCALE);
	}
	if (dock) {
		dock->refresh();
	}
	update_overlays();
}

// ------------------------------------------------------------------ undo

void HewnEditorPlugin::begin_edit(const String &p_name) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	edit_mode_before = mode;
	edit_before = n->get_state();
	edit_name = p_name;
	editing = true;
	if (uv_lock) {
		n->uv_lock_begin();
	}
}

void HewnEditorPlugin::cancel_edit() {
	HewnMesh3D *n = get_edited();
	if (n) {
		n->uv_lock_end();
	}
	if (editing && n && !edit_before.is_empty()) {
		n->set_state(edit_before);
	}
	editing = false;
	edit_before = Dictionary();
	selection_changed();
}

void HewnEditorPlugin::end_edit() {
	HewnMesh3D *n = get_edited();
	if (!editing || !n) {
		editing = false;
		return;
	}
	editing = false;
	n->uv_lock_end();
	for (HewnMesh3D *em : edited_meshes()) {
		em->finish_positions(); // (moved with only the positions shown: made whole again)
	}
	{
		// (a safety net: an operation that took faces away leaves no corners nothing uses any more)
		hewn::Mesh &m = n->get_data();
		std::vector<char> used(m.pos.size(), 0);
		for (const hewn::Face &F : m.faces) {
			for (int v : F.v) {
				if (v >= 0 && v < (int)used.size()) {
					used[v] = 1;
				}
			}
		}
		for (const std::pair<int, int> &w : m.wires) {
			for (int v : { w.first, w.second }) {
				if (v >= 0 && v < (int)used.size()) {
					used[v] = 1;
				}
			}
		}
		for (int v : m.points) {
			if (v >= 0 && v < (int)used.size()) {
				used[v] = 1;
			}
		}
		if (std::find(used.begin(), used.end(), 0) != used.end()) {
			hewn::remove_unused_vertices(m, &n->selection);
			n->commit(true);
		}
	}
	sanitize(n->get_data(), n->selection);
	Dictionary after = n->get_state();
	if (redo_inplace && !redo_inplace_done) {
		// the Last Operation panel: the step already in the history now leads here
		last_op_after.clear();
		last_op_after.merge(after);
		redo_inplace_done = true;
		edit_before = Dictionary();
		selection_changed();
		return;
	}
	if (field_drag_pending && (field_drag_node != n->get_instance_id() || field_drag_name != edit_name || !_field_dragging())) {
		_flush_field_drag();
	}
	if (multi_before.empty() && _field_dragging()) {
		// (a step of a drag: the history gets the whole drag when it ends)
		if (!field_drag_pending) {
			field_drag_pending = true;
			field_drag_before = edit_before;
			field_drag_name = edit_name;
			field_drag_node = n->get_instance_id();
		}
		modal_owner.clear();
		edit_before = Dictionary();
		selection_changed();
		return;
	}
	last_commit_before = edit_before;
	last_commit_after = after;
	EditorUndoRedoManager *ur = get_undo_redo();
	const bool merge_ends = merge_ends_next;
	merge_ends_next = false;
	ur->create_action("Hewn: " + edit_name, multi_merge ? UndoRedo::MERGE_ALL : merge_ends ? UndoRedo::MERGE_ENDS : UndoRedo::MERGE_DISABLE, n);
	ur->add_do_method(n, "set_state", after);
	ur->add_undo_method(n, "set_state", edit_before);
	for (const std::pair<uint64_t, Dictionary> &mb : multi_before) {
		// (the other edited meshes moved along in G / R / S)
		HewnMesh3D *o = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(mb.first)));
		if (o) {
			ur->add_do_method(o, "set_state", o->get_state());
			ur->add_undo_method(o, "set_state", mb.second);
		}
	}
	multi_before.clear();
	modal_owner.clear();
	ur->commit_action(false);
	edit_before = Dictionary();
	selection_changed();
}

// A drag on a field: the left button held over a number field / slider / spin box (or one of ours dragging).
bool HewnEditorPlugin::_field_dragging() const {
	if (HewnDragField::drag_count > 0) {
		return true;
	}
	if (!Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) {
		return false;
	}
	Control *base = EditorInterface::get_singleton()->get_base_control();
	Viewport *vp = base ? base->get_viewport() : nullptr;
	for (Node *c = vp ? vp->gui_get_hovered_control() : nullptr; c; c = c->get_parent()) {
		if (Object::cast_to<Range>(c) && !Object::cast_to<ScrollBar>(c)) {
			return true;
		}
		if (Object::cast_to<Viewport>(c)) {
			break;
		}
	}
	return false;
}

// The drag ended: its edits as one undo step.
void HewnEditorPlugin::_flush_field_drag() {
	if (!field_drag_pending) {
		return;
	}
	field_drag_pending = false;
	HewnMesh3D *n = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(field_drag_node)));
	if (!n || field_drag_before.is_empty()) {
		field_drag_before = Dictionary();
		return;
	}
	const Dictionary after = n->get_state();
	last_commit_before = field_drag_before;
	last_commit_after = after;
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: " + field_drag_name, UndoRedo::MERGE_DISABLE, n);
	ur->add_do_method(n, "set_state", after);
	ur->add_undo_method(n, "set_state", field_drag_before);
	ur->commit_action(false);
	field_drag_before = Dictionary();
}

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
		for (const hewn::Edge &e : edges) {
			const Vector3 wa = g.xform(m.pos[e.a]), wb = g.xform(m.pos[e.b]);
			if (p_cam->is_position_behind(wa) || p_cam->is_position_behind(wb) || !edge_visible(m, t, e)) {
				continue;
			}
			float tt;
			const float d = seg_dist(p_pos, p_cam->unproject_position(wa), p_cam->unproject_position(wb), &tt);
			if (d < best && _visible(p_cam, wa.lerp(wb, tt))) {
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
	ur->add_do_method(n, "set_state", n->get_state());
	for (HewnMesh3D *nn : created) {
		ur->add_do_method(parent, "add_child", nn, true);
		ur->add_do_method(nn, "set_owner", root);
		ur->add_do_reference(nn);
		ur->add_undo_method(parent, "remove_child", nn);
	}
	ur->add_undo_method(n, "set_state", before);
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

// ------------------------------------------------------------------ handles

std::vector<int> HewnEditorPlugin::_selected_verts() const {
	HewnMesh3D *n = get_edited();
	if (!n || mode == MODE_OBJECT) {
		return {};
	}
	const hewn::Selection &s = n->selection;
	std::vector<int> out;
	if (has_mode(MODE_VERTEX)) {
		out.insert(out.end(), s.verts.begin(), s.verts.end());
	}
	if (has_mode(MODE_EDGE)) {
		const std::vector<int> v = hewn::sel::verts_of_edges(s.edges);
		out.insert(out.end(), v.begin(), v.end());
	}
	if (has_mode(MODE_FACE)) {
		const std::vector<int> v = hewn::sel::verts_of_faces(n->get_data(), s.faces);
		out.insert(out.end(), v.begin(), v.end());
	}
	return hewn::sel::uniq(out);
}

bool HewnEditorPlugin::_pivot(Vector3 &r_world) const {
	HewnMesh3D *n = get_edited();
	const std::vector<int> vs = _selected_verts();
	if (!n || vs.empty()) {
		return false;
	}
	if (dock && dock->pivot_mode != HewnDock::PIVOT_MEDIAN && dock->pivot_mode != HewnDock::PIVOT_INDIVIDUAL) {
		r_world = _pivot_point(dock->pivot_mode, vs);
		return true;
	}
	Vector3 c;
	for (int v : vs) {
		c += n->get_data().pos[v];
	}
	r_world = n->get_global_transform().xform(c / (float)vs.size());
	return true;
}

float HewnEditorPlugin::_handle_size(Camera3D *p_cam, const Vector3 &p_world) const {
	if (p_cam->get_projection() == Camera3D::PROJECTION_ORTHOGONAL) {
		return p_cam->get_size() * 0.12f;
	}
	const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2);
	return MAX(0.01f, (p_world - p_cam->get_global_position()).dot(fwd)) * 0.17f;
}

// The transform gizmo follows Godot's own gizmo size setting (80 = its default).
float HewnEditorPlugin::_gizmo_length(Camera3D *p_cam, const Vector3 &p_world) const {
	float k = 1.0f;
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid() && es->has_setting("editors/3d/manipulator_gizmo_size")) {
		k = CLAMP((float)es->get_setting("editors/3d/manipulator_gizmo_size"), 16.0f, 160.0f) / 80.0f;
	}
	return _handle_size(p_cam, p_world) * k;
}

float HewnEditorPlugin::_world_per_pixel(Camera3D *p_cam, const Vector3 &p_world) const {
	const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2);
	const float depth = MAX(0.01f, (p_world - p_cam->get_global_position()).dot(fwd));
	const Vector2 sp = p_cam->unproject_position(p_world);
	return MAX(1e-6f, p_cam->project_position(sp, depth).distance_to(p_cam->project_position(sp + Vector2(1, 0), depth)));
}

// Gizmo: any combination of move arrows, scale boxes and rotation rings.
// Hit codes are tool * 4 + axis (axis 3 = the center).
bool HewnEditorPlugin::_gizmo_on(Tool p_tool) const {
	if (!dock || !dock->show_gizmo->is_pressed()) {
		return false;
	}
	return p_tool == TOOL_MOVE ? dock->gizmo_move->is_pressed() : p_tool == TOOL_ROTATE ? dock->gizmo_rotate->is_pressed() : dock->gizmo_scale->is_pressed();
}

Basis HewnEditorPlugin::_gizmo_basis(Camera3D *p_cam) const {
	const int sel = dock->gizmo_orient->get_selected();
	return _orientation_basis(sel <= 0 ? dock->orientation : sel - 1, p_cam);
}

int HewnEditorPlugin::_handle_hit(Camera3D *p_cam, const Vector2 &p_pos) const {
	if (vpaint) {
		return -1; // (no gizmo while painting)
	}
	Vector3 piv;
	const bool mv = _gizmo_on(TOOL_MOVE), rt = _gizmo_on(TOOL_ROTATE), sc = _gizmo_on(TOOL_SCALE);
	if (!p_cam || !(mv || rt || sc) || !_pivot(piv) || p_cam->is_position_behind(piv)) {
		return -1;
	}
	const float L = _gizmo_length(p_cam, piv);
	const Vector2 c = p_cam->unproject_position(piv);
	const Basis ob = _gizmo_basis(p_cam);
	if (p_pos.distance_to(c) < 9.0f && (mv || sc)) {
		return (mv ? TOOL_MOVE : TOOL_SCALE) * 4 + 3;
	}
	int best = -1;
	float best_d = 8.0f;
	const float scale_at = mv ? 0.72f : 1.0f;
	const float ring = (mv || sc) ? 1.2f : 1.0f;
	for (int i = 0; i < 3; i++) {
		const Vector3 ax = ob.get_column(i).normalized();
		if (sc) {
			const float d = p_pos.distance_to(p_cam->unproject_position(piv + ax * L * scale_at));
			if (d < best_d) {
				best_d = d;
				best = TOOL_SCALE * 4 + i;
			}
		}
		if (mv) {
			const float d = seg_dist(p_pos, c, p_cam->unproject_position(piv + ax * L));
			if (d < best_d) {
				best_d = d;
				best = TOOL_MOVE * 4 + i;
			}
		} else if (sc) {
			const float d = seg_dist(p_pos, c, p_cam->unproject_position(piv + ax * L)) + 2.0f;
			if (d < best_d) {
				best_d = d;
				best = TOOL_SCALE * 4 + i;
			}
		}
		if (rt) {
			Vector3 u, w;
			hewn::plane_basis(ax, u, w);
			Vector2 prev;
			float d = 1e9f;
			for (int k = 0; k <= 48; k++) {
				const float a = (float)Math::PI * 2.0f * k / 48.0f;
				const Vector2 p = p_cam->unproject_position(piv + (u * std::cos(a) + w * std::sin(a)) * L * ring);
				if (k > 0) {
					d = MIN(d, seg_dist(p_pos, prev, p));
				}
				prev = p;
			}
			if (d < best_d) {
				best_d = d;
				best = TOOL_ROTATE * 4 + i;
			}
		}
	}
	return best;
}


float HewnEditorPlugin::_axis_param(Camera3D *p_cam, const Vector2 &p_pos, const Vector3 &p_origin, const Vector3 &p_dir) const {
	Vector3 o, d;
	_ray(p_cam, p_pos, o, d);
	const float FAR = 2000.0f;
	// An axis going away from the camera ends on the screen at its vanishing point: past it the nearest
	// point would come back; it stays far away instead, like _plane_hit.
	const Vector3 dir = p_dir.normalized();
	const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2).normalized();
	if (p_cam->get_projection() == Camera3D::PROJECTION_PERSPECTIVE && !p_cam->is_position_behind(p_origin)) {
		const Vector2 s0 = p_cam->unproject_position(p_origin);
		for (int sign = -1; sign <= 1; sign += 2) {
			const Vector3 D = dir * (float)sign;
			if (D.dot(fwd) < 1e-3f) {
				continue;
			}
			const Vector3 far = p_origin + D * 1e6f;
			if (p_cam->is_position_behind(far)) {
				continue;
			}
			const Vector2 e = p_cam->unproject_position(far) - s0;
			if (e.length_squared() > 1.0f && (p_pos - s0).dot(e) / e.length_squared() >= 1.0f) {
				return FAR * sign / MAX(p_dir.length(), 1e-6f);
			}
		}
	}
	const float t = line_ray_param(p_origin, p_dir, o, d);
	const float lim = FAR / MAX(p_dir.length(), 1e-6f);
	return CLAMP(t, -lim, lim);
}

// Where the mouse ray meets a plane. Toward the horizon the point goes far away; past it (the ray never
// reaches the plane) it stays far away in the mouse's direction, instead of coming back
// from behind the camera.
bool HewnEditorPlugin::_plane_hit(Camera3D *p_cam, const Vector2 &p_pos, const Vector3 &p_origin, const Vector3 &p_n, Vector3 &r_hit) const {
	const float FAR = 2000.0f;
	Vector3 o, d;
	_ray(p_cam, p_pos, o, d);
	const Vector3 n = p_n.normalized();
	const float den = d.dot(n);
	const float t = std::fabs(den) < 1e-9f ? -1.0f : (p_origin - o).dot(n) / den;
	const Vector3 base = o - n * (o - p_origin).dot(n); // (the camera dropped onto the plane)
	if (t >= 0.0f && (o + d * t).distance_to(base) <= FAR) {
		r_hit = o + d * t;
		return true;
	}
	const Vector3 dp = d - n * d.dot(n);
	if (dp.length() < 1e-6f) {
		return false;
	}
	r_hit = base + dp.normalized() * FAR;
	return true;
}

void HewnEditorPlugin::_begin_handle_drag(Camera3D *p_cam, int p_code, bool p_shift) {
	tool = (Tool)CLAMP(p_code / 4, 0, 2);
	const int p_axis = p_code % 4;
	HewnMesh3D *n = get_edited();
	if (!n || !_pivot(drag_pivot)) {
		return;
	}
	static const char *names[3] = { "Move", "Rotate", "Scale" };
	begin_edit(p_shift && tool == TOOL_MOVE ? "Extrude" : names[tool]);
	drag_verts.clear();
	drag_extruded = false;
	res_delta = Vector3();
	res_angle = 0.0f;
	res_scale = 1.0f;
	hewn::Mesh &m = n->get_data();
	hewn::Selection &s = n->selection;
	drag_extruded = p_shift && tool == TOOL_MOVE && ((mode == MODE_FACE && !s.faces.empty()) || (mode == MODE_EDGE && !s.edges.empty()));
	if (p_shift && tool == TOOL_MOVE && mode == MODE_FACE && !s.faces.empty()) {
		hewn::ops::extrude_faces(m, s.faces, 0.0f, false, &drag_verts);
		n->commit(true);
	} else if (p_shift && tool == TOOL_MOVE && mode == MODE_EDGE && !s.edges.empty()) {
		s.edges = hewn::ops::extrude_edges(m, s.edges, 0.0f, false);
		drag_verts = hewn::sel::verts_of_edges(s.edges);
		n->commit(true);
	}
	if (drag_verts.empty()) {
		drag_verts = _selected_verts();
	}
	{
		const std::vector<int> sel = drag_verts;
		prop_proj_axis = dock->prop_projected && dock->prop_projected->is_pressed() && p_cam ? -p_cam->get_global_transform().basis.get_column(2).normalized() : Vector3();
		_weighted_verts(sel, dock->prop_enabled->is_pressed() && !p_shift, (float)dock->prop_radius->get_value(), dock->prop_falloff->get_selected(),
				dock->prop_connected->is_pressed(), drag_verts, drag_weights);
	}
	drag_orig.clear();
	for (int v : drag_verts) {
		drag_orig.push_back(m.pos[v]);
	}
	// Magnet candidates exclude the dragged vertices.
	modal_verts = drag_verts;
	modal_object = false;
	_gather_snap_candidates(true);
	precise_armed = !p_shift;
	_reset_precise();
	modal_snapped = false;

	drag_axis = p_axis;
	const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2);
	drag_axis_dir = p_axis < 3 ? _gizmo_basis(p_cam).get_column(p_axis).normalized() : fwd;
	if (tool == TOOL_MOVE) {
		if (p_axis < 3) {
			drag_t0 = _axis_param(p_cam, press_pos, drag_pivot, drag_axis_dir);
		} else {
			_plane_hit(p_cam, press_pos, drag_pivot, fwd, drag_hit0);
		}
	} else if (tool == TOOL_ROTATE) {
		_plane_hit(p_cam, press_pos, drag_pivot, drag_axis_dir, drag_hit0);
	} else {
		if (p_axis < 3) {
			drag_t0 = _axis_param(p_cam, press_pos, drag_pivot, drag_axis_dir);
		} else {
			drag_screen0 = MAX(1.0f, press_pos.distance_to(p_cam->unproject_position(drag_pivot)));
		}
	}
	drag = DRAG_HANDLE;
}

void HewnEditorPlugin::_update_handle_drag(Camera3D *p_cam, const Vector2 &p_pos, bool p_snap, bool p_magnet) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	const Transform3D g = n->get_global_transform();
	const Transform3D inv = g.affine_inverse();
	const float sm = (float)dock->snap_move->get_value();
	const float sr = (float)dock->snap_rotate->get_value();
	const float ss = (float)dock->snap_scale->get_value();
	hewn::Mesh &m = n->get_data();
	auto snapf = [&](float x, float step) { return (p_snap && step > 0.0f) ? std::round(x / step) * step : x; };
	modal_snapped = false;

	if (tool == TOOL_MOVE) {
		Vector3 delta;
		Vector3 target;
		if (p_magnet && (dock->snap_targets & HewnDock::SNAP_GEOMETRY) && _snap_target(p_cam, p_pos, target)) {
			// Snap the dragged vertex closest to the target onto it, keeping the axis constraint.
			float best = 1e30f;
			Vector3 base = drag_pivot;
			for (const Vector3 &o : drag_orig) {
				const Vector3 w = g.xform(o);
				if (w.distance_to(target) < best) {
					best = w.distance_to(target);
					base = w;
				}
			}
			delta = target - base;
			if (drag_axis < 3) {
				delta = drag_axis_dir * delta.dot(drag_axis_dir);
			}
			modal_snapped = true;
			modal_snap_point = target;
			modal_snap_kind = last_snap_kind;
		} else if (drag_axis < 3) {
			delta = drag_axis_dir * snapf(_axis_param(p_cam, p_pos, drag_pivot, drag_axis_dir) - drag_t0, sm);
		} else {
			Vector3 hit;
			if (!_plane_hit(p_cam, p_pos, drag_pivot, drag_axis_dir, hit)) {
				return;
			}
			delta = hit - drag_hit0;
			delta = Vector3(snapf(delta.x, sm), snapf(delta.y, sm), snapf(delta.z, sm));
		}
		if (p_snap && snap_grid_on() && sm > 0.0f && !modal_snapped) {
			// Grid: the pivot lands on grid points (along the dragged axis only, when there is one)
			const Vector3 raw = delta;
			delta = _grid_point(drag_pivot + raw, Basis(), sm) - drag_pivot;
			if (drag_axis < 3) {
				delta = drag_axis_dir * delta.dot(drag_axis_dir);
			}
		}
		for (size_t i = 0; i < drag_verts.size(); i++) {
			m.pos[drag_verts[i]] = inv.xform(g.xform(drag_orig[i]) + delta * drag_weights[i]);
		}
		res_delta = delta;
	} else if (tool == TOOL_ROTATE) {
		Vector3 hit;
		if (!_plane_hit(p_cam, p_pos, drag_pivot, drag_axis_dir, hit)) {
			return;
		}
		const Vector3 v0 = drag_hit0 - drag_pivot, v1 = hit - drag_pivot;
		float ang = std::atan2(drag_axis_dir.dot(v0.cross(v1)), v0.dot(v1));
		ang = snapf(ang * 180.0f / (float)Math::PI, sr) * (float)Math::PI / 180.0f;
		res_axis = drag_axis_dir;
		res_angle = ang * 180.0f / (float)Math::PI;
		for (size_t i = 0; i < drag_verts.size(); i++) {
			const Basis rot(drag_axis_dir, ang * drag_weights[i]);
			m.pos[drag_verts[i]] = inv.xform(drag_pivot + rot.xform(g.xform(drag_orig[i]) - drag_pivot));
		}
	} else {
		float s;
		if (drag_axis < 3) {
			s = std::fabs(drag_t0) > 1e-6f ? _axis_param(p_cam, p_pos, drag_pivot, drag_axis_dir) / drag_t0 : 1.0f;
		} else {
			s = p_pos.distance_to(p_cam->unproject_position(drag_pivot)) / drag_screen0;
		}
		s = snapf(s, ss);
		res_scale = s;
		res_dir = drag_axis < 3 ? drag_axis_dir : Vector3(1, 0, 0);
		res_constraint = drag_axis < 3 ? 1 : 0;
		for (size_t i = 0; i < drag_verts.size(); i++) {
			const Vector3 d = g.xform(drag_orig[i]) - drag_pivot;
			const float si = 1.0f + (s - 1.0f) * drag_weights[i];
			Vector3 p;
			if (drag_axis < 3) {
				p = drag_pivot + d + drag_axis_dir * (d.dot(drag_axis_dir) * (si - 1.0f));
			} else {
				p = drag_pivot + d * si;
			}
			m.pos[drag_verts[i]] = inv.xform(p);
		}
	}
	n->apply_mirror_clipping(drag_verts, drag_orig);
	n->commit(true);
}

// The drag is replayed from the start as the operation G / R / S would make (with the same numbers), so
// the Last Operation panel shows it and can change it.
void HewnEditorPlugin::_finish_handle_drag() {
	HewnMesh3D *n = get_edited();
	if (!n || !editing) {
		return;
	}
	String op;
	if (tool == TOOL_MOVE) {
		op = drag_extruded ? "extrude_move" : "translate";
		set_op_param(op, "x", res_delta.x);
		set_op_param(op, "y", res_delta.y);
		set_op_param(op, "z", res_delta.z);
		if (drag_extruded) {
			set_op_param(op, "dissolve_orthogonal", false);
			set_op_param(op, "flip_normals", false);
		}
	} else if (tool == TOOL_ROTATE) {
		op = "rotate";
		set_op_param(op, "angle", res_angle);
		set_op_param(op, "ax", res_axis.x);
		set_op_param(op, "ay", res_axis.y);
		set_op_param(op, "az", res_axis.z);
	} else {
		op = "scale";
		set_op_param(op, "factor", res_scale);
		set_op_param(op, "dx", res_dir.x);
		set_op_param(op, "dy", res_dir.y);
		set_op_param(op, "dz", res_dir.z);
		set_op_param(op, "constraint", res_constraint);
	}
	if (op != "extrude_move") {
		set_op_param(op, "prop", dock->prop_enabled->is_pressed());
		set_op_param(op, "prop_size", dock->prop_radius->get_value());
		set_op_param(op, "prop_falloff", dock->prop_falloff->get_selected());
		set_op_param(op, "prop_connected", dock->prop_connected->is_pressed());
		set_op_param(op, "prop_projected", prop_proj_axis != Vector3());
		set_op_param(op, "prop_vx", prop_proj_axis.x);
		set_op_param(op, "prop_vy", prop_proj_axis.y);
		set_op_param(op, "prop_vz", prop_proj_axis.z);
		// (the handles turn and scale around one pivot)
		set_op_param(op, "pivot_mode", dock->pivot_mode == HewnDock::PIVOT_INDIVIDUAL ? (int)HewnDock::PIVOT_MEDIAN : dock->pivot_mode);
		set_op_param(op, "px", drag_pivot.x);
		set_op_param(op, "py", drag_pivot.y);
		set_op_param(op, "pz", drag_pivot.z);
	}
	n->set_state(edit_before);
	editing = false;
	mode = element_mode = edit_mode_before;
	run_op(op);
	_set_godot_gizmo_hidden(mode != MODE_OBJECT);
	selection_changed();
}

void HewnEditorPlugin::_draw_handles(Control *p_overlay, Camera3D *p_cam) {
	Vector3 piv;
	const bool mv = _gizmo_on(TOOL_MOVE), rt = _gizmo_on(TOOL_ROTATE), sc = _gizmo_on(TOOL_SCALE);
	if (!(mv || rt || sc) || !_pivot(piv) || p_cam->is_position_behind(piv)) {
		return;
	}
	const float L = _gizmo_length(p_cam, piv);
	const Vector2 c = p_cam->unproject_position(piv);
	const int hover = drag == DRAG_HANDLE ? tool * 4 + drag_axis : _handle_hit(p_cam, mouse_pos);
	const Basis ob = _gizmo_basis(p_cam);
	const float scale_at = mv ? 0.72f : 1.0f;
	const float ring = (mv || sc) ? 1.2f : 1.0f;
	for (int i = 0; i < 3; i++) {
		const Vector3 ax = ob.get_column(i).normalized();
		auto col = [&](Tool t) { return hover == t * 4 + i ? HANDLE_HOVER : AXIS_COLORS[i]; };
		if (rt) {
			Vector3 u, w;
			hewn::plane_basis(ax, u, w);
			PackedVector2Array pts;
			for (int k = 0; k <= 64; k++) {
				const float a = (float)Math::PI * 2.0f * k / 64.0f;
				pts.push_back(p_cam->unproject_position(piv + (u * std::cos(a) + w * std::sin(a)) * L * ring));
			}
			p_overlay->draw_polyline(pts, col(TOOL_ROTATE), 2.0f, true);
		}
		const Vector2 e = p_cam->unproject_position(piv + ax * L);
		const Vector2 dir = (e - c).length() > 1e-3f ? (e - c).normalized() : Vector2(1, 0);
		const Vector2 nrm(-dir.y, dir.x);
		if (mv) {
			p_overlay->draw_line(c, e, col(TOOL_MOVE), 2.5f, true);
			PackedVector2Array tri;
			tri.push_back(e + dir * 14.0f);
			tri.push_back(e + nrm * 5.5f);
			tri.push_back(e - nrm * 5.5f);
			draw_poly_safe(p_overlay, tri, col(TOOL_MOVE));
		} else if (sc) {
			p_overlay->draw_line(c, e, col(TOOL_SCALE), 2.5f, true);
		}
		if (sc) {
			const Vector2 b = p_cam->unproject_position(piv + ax * L * scale_at);
			p_overlay->draw_rect(Rect2(b - Vector2(5, 5), Vector2(10, 10)), col(TOOL_SCALE));
		}
	}
	if (mv || sc) {
		const int center = (mv ? TOOL_MOVE : TOOL_SCALE) * 4 + 3;
		const Color cc = hover == center ? HANDLE_HOVER : Color(1, 1, 1, 0.9f);
		p_overlay->draw_rect(Rect2(c - Vector2(5, 5), Vector2(10, 10)), cc, false, 2.0f);
	}
	if (drag == DRAG_HANDLE && modal_snapped && !p_cam->is_position_behind(modal_snap_point)) {
		draw_snap_symbol(p_overlay, p_cam->unproject_position(modal_snap_point), modal_snap_kind, SNAP_COLOR, 7.0f);
	}
}

Camera3D *HewnEditorPlugin::_camera_for_overlay(Control *p_overlay) const {
	Node *container = p_overlay->get_parent();
	for (int i = 0; i < 4; i++) {
		SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(i);
		if (vp && container && container->is_ancestor_of(vp)) {
			return vp->get_camera_3d();
		}
	}
	SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(0);
	return vp ? vp->get_camera_3d() : nullptr;
}

void HewnEditorPlugin::_forward_3d_force_draw_over_viewport(Control *p_overlay) {
	Camera3D *cam = _camera_for_overlay(p_overlay);
	if (!cam) {
		return;
	}
	_marks_begin(p_overlay);
	_draw_over_viewport(p_overlay, cam);
	_marks_end(p_overlay); // (the marks not drawn this time: hidden)
}

void HewnEditorPlugin::_draw_over_viewport(Control *p_overlay, Camera3D *cam) {
	if (dock->overlay(dock->ov_cursor)) {
		_draw_cursor(p_overlay, cam);
	}
	if (curve_edit) {
		_draw_curve_edit(p_overlay, cam);
	}
	if (hc_edit) {
		_draw_hc(p_overlay, cam);
	}
	if (dock->overlay(dock->ov_notes)) {
		_draw_notes(p_overlay, cam);
	}
	if (dock->overlay(dock->ov_nonplanar)) {
		_draw_nonplanar(p_overlay, cam);
	}
	if (dock->overlay(dock->ov_curve_normals)) {
		_draw_curve_normals(p_overlay, cam);
	}
	if (scatter_tool >= 0 && get_scatter() && mode == MODE_OBJECT && modal == MODAL_NONE) {
		_draw_scatter(p_overlay, cam);
	}
	_draw_face_push(p_overlay, cam);
	if (overlays_on()) {
		_draw_physics(p_overlay, cam);
	}
	if (mode == MODE_OBJECT && select_kind == 1 && modal == MODAL_NONE) {
		p_overlay->draw_arc(mouse_pos, circle_radius, 0.0f, (float)Math::PI * 2.0f, 48, Color(1, 1, 1, obj_circle_drag ? 0.9f : 0.55f), 1.0f, true);
	}
	if (dock->overlay(dock->ov_dims) && modal != MODAL_UVT && modal != MODAL_TSNAP) {
		_draw_dimensions(p_overlay, cam);
	}
	if (mode == MODE_OBJECT && dock->overlays->is_pressed()) {
		// the origins of the selected objects, orange dots
		const float es = EditorInterface::get_singleton()->get_editor_scale();
		for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_selected_nodes()) {
			Node3D *n3 = Object::cast_to<Node3D>(v);
			if (!n3 || !n3->is_inside_tree()) {
				continue;
			}
			const Vector3 wo = n3->get_global_transform().origin;
			if (!cam->is_position_behind(wo)) {
				const Vector2 o = cam->unproject_position(wo);
				p_overlay->draw_circle(o, (hw_mesh_style().origin_size * 0.5f + 1.0f) * es, Color(0.05f, 0.05f, 0.05f, 0.9f), true, -1.0f, true); // (its dark ring)
				p_overlay->draw_circle(o, hw_mesh_style().origin_size * 0.5f * es, hw_mesh_style().origin, true, -1.0f, true);
			}
		}
	}
	if (sculpt && modal == MODAL_SCULPT) {
		_draw_sculpt(p_overlay, cam);
		return;
	}
	if (modal == MODAL_NONE) {
		_draw_pending(p_overlay);
	}
	if (modal == MODAL_NONE || modal == MODAL_MOVE || modal == MODAL_ROTATE || modal == MODAL_SCALE || modal == MODAL_SNAP) {
		_draw_brushes(p_overlay, cam);
	}
	if (modal == MODAL_NONE || modal == MODAL_MOVE || modal == MODAL_ROTATE || modal == MODAL_SCALE || modal == MODAL_PAINT) {
		_draw_overlays(p_overlay, cam); // (painting: the selection is the mask)
	}
	_draw_overlaps(p_overlay, cam);
	if (modal == MODAL_KNIFE || modal == MODAL_LOOPCUT || modal == MODAL_CUTTER || modal == MODAL_POLY || modal == MODAL_RULER || modal == MODAL_SHAPE || modal == MODAL_PAINT || modal == MODAL_BISECT ||
			modal == MODAL_CURSOR || modal == MODAL_CLIP || modal == MODAL_PREFAB || modal == MODAL_SPIN || modal == MODAL_FACECUT) {
		if (modal == MODAL_SPIN || modal == MODAL_FACECUT) {
			_draw_overlays(p_overlay, cam); // (the selection stays visible)
		}
		_draw_tools(p_overlay, cam);
		return;
	}
	if (modal == MODAL_UV) {
		_draw_uv_modal(p_overlay, cam);
		return;
	}
	if (modal == MODAL_UVT) {
		_draw_uvt(p_overlay, cam);
		return;
	}
	if (modal == MODAL_TSNAP) {
		_draw_tsnap(p_overlay, cam);
		return;
	}
	if (modal != MODAL_NONE) {
		_draw_modal(p_overlay, cam);
		return;
	}
	HewnMesh3D *n = get_edited();
	if (!n || mode == MODE_OBJECT) {
		return;
	}
	if (vpaint) {
		_draw_paint(p_overlay, cam); // (a selection tool in Vertex Paint: the color squares too)
		return;
	}
	_draw_handles(p_overlay, cam);
	_draw_bisect_widget(p_overlay, cam);
	_draw_select_region(p_overlay);
}

// The box / circle / lasso being drawn (the circle also before the press).
void HewnEditorPlugin::_draw_select_region(Control *p_overlay) {
	if (drag == DRAG_BOX) {
		const Rect2 r = Rect2(press_pos, Vector2()).expand(mouse_pos);
		p_overlay->draw_rect(r, Color(1, 0.6f, 0.1f, 0.12f));
		p_overlay->draw_rect(r, Color(1, 0.6f, 0.1f, 0.8f), false, 1.0f);
	}
	if (select_kind == 1 && (drag == DRAG_NONE || drag == DRAG_CIRCLE) && modal == MODAL_NONE) {
		p_overlay->draw_arc(mouse_pos, circle_radius, 0.0f, (float)Math::PI * 2.0f, 48, Color(1, 1, 1, drag == DRAG_CIRCLE ? 0.9f : 0.55f), 1.0f, true);
	}
	if (drag == DRAG_LASSO && lasso_pts.size() >= 2) {
		PackedVector2Array pts;
		for (const Vector2 &p : lasso_pts) {
			pts.push_back(p);
		}
		pts.push_back(lasso_pts[0]);
		p_overlay->draw_polyline(pts, Color(1, 0.6f, 0.1f, 0.9f), 1.0f, true);
	}
}

// ------------------------------------------------------------------ input

Vector2 HewnEditorPlugin::_precise_mouse(const Vector2 &p_raw, bool p_shift) {
	if (prec_active) {
		const Vector2 eff = prec_eff0 + (p_raw - prec_raw0) * 0.1f;
		if (!p_shift) {
			prec_active = false;
			prec_offset = eff - p_raw;
		}
		return eff;
	}
	const Vector2 eff = p_raw + prec_offset;
	if (p_shift) {
		prec_active = true;
		prec_raw0 = p_raw;
		prec_eff0 = eff;
	}
	return eff;
}

void HewnEditorPlugin::_reset_precise() {
	prec_active = false;
	prec_offset = Vector2();
	modal_cursor = mouse_pos;
}

// ------------------------------------------------------------------ materials

int HewnEditorPlugin::apply_material_to_selection(const Ref<Material> &p_material) {
	std::vector<HewnMesh3D *> targets;
	std::vector<PackedInt32Array> faces;
	HewnMesh3D *edited = get_edited();
	int count = 0;
	if (edited && mode != MODE_OBJECT) {
		// edit mode: the selected faces (or the faces of the selected vertices / edges)
		const hewn::Mesh &m = edited->get_data();
		const std::vector<int> fs = mode == MODE_FACE ? edited->selection.faces : hewn::sel::faces_from_verts(m, _selected_verts(), true);
		PackedInt32Array hw_faces;
		for (int f : fs) {
			hw_faces.push_back(f);
		}
		if (hw_faces.is_empty()) {
			return -1;
		}
		targets.push_back(edited);
		faces.push_back(hw_faces);
		count = (int)hw_faces.size();
	} else {
		const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
		for (int64_t i = 0; i < sel.size(); i++) {
			if (HewnMesh3D *n = Object::cast_to<HewnMesh3D>(sel[i])) {
				targets.push_back(n);
			}
		}
		if (targets.empty() && edited) {
			targets.push_back(edited);
		}
		for (HewnMesh3D *n : targets) {
			faces.push_back(PackedInt32Array()); // all of them
			count += n->get_face_count();
		}
	}
	if (targets.empty()) {
		return -1;
	}
	_commit_material(p_material, targets, faces);
	return count;
}

// The bottom panel's tabs: Textures, Scenes, UV (as they belong together), whatever the layout had.
void HewnEditorPlugin::_order_bottom_docks() {
	Control *order[3] = { texture_dock, prefab_dock, uv_dock };
	Node *parent = order[0] ? order[0]->get_parent() : nullptr;
	for (Control *d : order) {
		if (!d || d->get_parent() != parent) {
			return; // (not all in one place: left as the user put them)
		}
	}
	int at = order[0]->get_index();
	for (Control *d : order) {
		at = MIN(at, d->get_index());
	}
	for (Control *d : order) {
		parent->move_child(d, at++);
	}
}

void HewnEditorPlugin::add_subdivision_modifier() {
	std::vector<HewnMesh3D *> targets;
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	for (int64_t i = 0; i < sel.size(); i++) {
		if (HewnMesh3D *x = Object::cast_to<HewnMesh3D>(sel[i])) {
			targets.push_back(x);
		}
	}
	if (targets.empty() && get_edited()) {
		targets.push_back(get_edited());
	}
	if (targets.empty()) {
		ERR_PRINT("Hewn: select the meshes to subdivide first.");
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Add Subdivision Modifier", UndoRedo::MERGE_DISABLE, targets[0]);
	for (HewnMesh3D *n : targets) {
		const TypedArray<HewnModifier> before = n->get_modifiers();
		TypedArray<HewnModifier> after = before.duplicate();
		Ref<HewnSubdivisionModifier> sm;
		sm.instantiate();
		after.push_back(sm);
		ur->add_do_method(n, "set_modifiers", after);
		ur->add_undo_method(n, "set_modifiers", before);
	}
	ur->commit_action();
}

void HewnEditorPlugin::add_from_menu(const String &p_kind) {
	adding_from_menu = true;
	add_node_kind(p_kind);
	adding_from_menu = false;
}

int HewnEditorPlugin::apply_material_at_screen(const Ref<Material> &p_material, const Vector2 &p_screen) {
	for (int i = 0; i < 4; i++) {
		SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(i);
		Control *box = vp ? Object::cast_to<Control>(vp->get_parent()) : nullptr;
		if (!box || !box->is_visible_in_tree() || !vp->get_camera_3d()) {
			continue;
		}
		const Rect2 r(box->get_screen_position(), box->get_size() * box->get_global_transform_with_canvas().get_scale());
		if (r.size.x <= 0.0f || r.size.y <= 0.0f || !r.has_point(p_screen)) {
			continue;
		}
		const Vector2 at = (p_screen - r.position) / r.size * Vector2(vp->get_size());
		Vector3 from, dir, hit, nrm;
		if (!_ray(vp->get_camera_3d(), at, from, dir)) {
			return -1;
		}
		HewnMesh3D *node = nullptr;
		int face = -1;
		if (!_raycast_scene(from, dir, hit, nrm, &node, &face) || !node) {
			return -1;
		}
		std::vector<HewnMesh3D *> targets;
		std::vector<PackedInt32Array> faces;
		HewnMesh3D *edited = get_edited();
		if (node == edited && mode != MODE_OBJECT && face >= 0) {
			const hewn::Mesh &m = edited->get_data();
			const std::vector<int> fs = mode == MODE_FACE ? edited->selection.faces : hewn::sel::faces_from_verts(m, _selected_verts(), true);
			PackedInt32Array list;
			if (std::find(fs.begin(), fs.end(), face) != fs.end()) {
				for (int f : fs) {
					list.push_back(f);
				}
			} else {
				list.push_back(face);
			}
			targets.push_back(edited);
			faces.push_back(list);
			_commit_material(p_material, targets, faces);
			return (int)list.size();
		}
		bool in_sel = false;
		const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
		for (int64_t k = 0; k < sel.size(); k++) {
			in_sel = in_sel || Object::cast_to<Node>(sel[k]) == node;
		}
		if (in_sel && mode == MODE_OBJECT) {
			for (int64_t k = 0; k < sel.size(); k++) {
				if (HewnMesh3D *x = Object::cast_to<HewnMesh3D>(sel[k])) {
					targets.push_back(x);
				}
			}
		} else {
			targets.push_back(node);
		}
		int count = 0;
		for (HewnMesh3D *x : targets) {
			faces.push_back(PackedInt32Array());
			count += x->get_face_count();
		}
		_commit_material(p_material, targets, faces);
		return count;
	}
	return -1;
}

void HewnEditorPlugin::_commit_material(const Ref<Material> &p_material, const std::vector<HewnMesh3D *> &targets, const std::vector<PackedInt32Array> &faces) {
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Apply Material", UndoRedo::MERGE_DISABLE, targets[0]);
	for (size_t i = 0; i < targets.size(); i++) {
		HewnMesh3D *n = targets[i];
		const TypedArray<Material> mats_before = n->get_materials().duplicate();
		const Dictionary before = n->get_state();
		n->apply_material(p_material, faces[i]);
		ur->add_do_method(n, "set_materials", n->get_materials().duplicate());
		ur->add_do_method(n, "set_state", n->get_state());
		ur->add_undo_method(n, "set_materials", mats_before);
		ur->add_undo_method(n, "set_state", before);
	}
	ur->commit_action(false);
	dock->refresh();
}

int32_t HewnEditorPlugin::_forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	{
		Ref<InputEventMouseButton> pb = p_event;
		if (pb.is_valid() && pb->is_pressed() && pb->get_button_index() == MOUSE_BUTTON_LEFT) {
			plain_press = false; // (set again below when no tool takes the press)
		}
	}
	Ref<InputEventKey> fk = p_event;
	if (fk.is_valid() && Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT) && modal != MODAL_UVT) {
		// the right button flies the camera (WASD / QE): every key is Godot's then (W must not run the action)
		if (fk->is_pressed()) {
			rmb_pending = false; // (flying: no context menu on release)
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (fk.is_valid() && !hotkeys_on && modal == MODAL_NONE && !remapping) {
		return AFTER_GUI_INPUT_PASS; // (Editor Settings > Hewn > Interface > Hotkeys off: keys inside a running tool still work)
	}
	if (fk.is_valid() && key_code(fk) == KEY_Y && !fk->is_ctrl_pressed() && !fk->is_alt_pressed() && !fk->is_shift_pressed() && !fk->is_meta_pressed() &&
			(modal == MODAL_SHAPE || modal == MODAL_POLY || modal == MODAL_KNIFE || modal == MODAL_CUTTER || modal == MODAL_CLIP || modal == MODAL_PREFAB ||
					modal == MODAL_BISECT || modal == MODAL_LOOPCUT || modal == MODAL_RULER || (modal == MODAL_CURSOR && cursor_xf == 0) || modal == MODAL_TSNAP)) {
		// Y (Godot's snapping on / off) works while drawing too (in G / R / S it is the Y axis)
		if (fk->is_pressed() && !fk->is_echo() && dock && dock->snap_enabled) {
			dock->snap_enabled->set_pressed(!dock->snap_enabled->is_pressed());
			update_overlays();
		}
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (fk.is_valid() && fk->is_pressed() && !fk->is_echo() && (key_code(fk) == KEY_KP_DIVIDE || key_code(fk) == KEY_SLASH) && !fk->is_ctrl_pressed() &&
			!fk->is_alt_pressed() && !fk->is_shift_pressed()) {
		if (isolated) {
			set_isolate(false);
		} else if (_anything_selected()) {
			set_isolate(true); // local view: only the selection shown, the camera on it
			if (mode == MODE_OBJECT) {
				focus_selection(p_camera); // (Godot's focus goes to the objects' origins: not in edit mode / Vertex Paint)
			}
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (fk.is_valid() && fk->is_pressed() && !fk->is_echo() && (key_code(fk) == KEY_PERIOD || key_code(fk) == KEY_KP_PERIOD) && !fk->is_ctrl_pressed() &&
			!fk->is_alt_pressed() && !fk->is_shift_pressed() && modal == MODAL_NONE) {
		if (mode == MODE_OBJECT && _anything_selected()) {
			focus_selection(p_camera); // View Selected (edit mode / Vertex Paint: Godot's focus would go to the origin)
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (fk.is_valid() && fk->is_pressed() && !fk->is_echo() && key_code(fk) == KEY_Z && fk->is_alt_pressed() && fk->is_shift_pressed() && !fk->is_ctrl_pressed() &&
			dock && dock->overlays) {
		// Shift+Alt+Z: the overlays on / off, also in the middle of an operation
		dock->overlays->set_pressed(!dock->overlays->is_pressed());
		update_overlays();
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (fk.is_valid() && fk->is_pressed() && !fk->is_echo() && key_code(fk) == KEY_Z && fk->is_alt_pressed() && !fk->is_ctrl_pressed() && !fk->is_shift_pressed() &&
			(modal != MODAL_NONE || mode == MODE_OBJECT) && modal != MODAL_UVT) {
		// Alt+Z: X-ray also while drawing (a cut through a wall) and in object mode
		set_xray(!xray_on);
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (scatter_tool >= 0 && get_scatter() && mode == MODE_OBJECT && modal == MODAL_NONE) {
		const int32_t sr = _scatter_input(p_camera, p_event);
		if (sr >= 0) {
			if (sr == AFTER_GUI_INPUT_STOP) {
				if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
					vp->set_input_as_handled();
				}
			}
			return sr;
		}
	}
	if (curve_id.is_valid() && mode == MODE_OBJECT && modal == MODAL_NONE && _hc()) {
		const int32_t cr = _hc_input(p_camera, p_event);
		if (cr >= 0) {
			if (cr == AFTER_GUI_INPUT_STOP) {
				if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
					vp->set_input_as_handled();
				}
			}
			return cr;
		}
		if (hc_edit) {
			Ref<InputEventMouseButton> cmb = p_event;
			if (cmb.is_valid() && cmb->get_button_index() == MOUSE_BUTTON_LEFT) {
				return AFTER_GUI_INPUT_STOP; // (Godot's own path handles stay out of it)
			}
		}
	} else if (curve_id.is_valid() && mode == MODE_OBJECT && modal == MODAL_NONE) {
		const int32_t cr = _curve_input(p_camera, p_event);
		if (cr >= 0) {
			if (cr == AFTER_GUI_INPUT_STOP) {
				if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
					vp->set_input_as_handled();
				}
			}
			return cr;
		}
		if (curve_edit) {
			Ref<InputEventMouseButton> cmb = p_event;
			if (cmb.is_valid() && cmb->get_button_index() == MOUSE_BUTTON_LEFT) {
				return AFTER_GUI_INPUT_STOP; // (Godot's own path handles stay out of it)
			}
		}
	}
	if (fk.is_valid()) {
		const int32_t rk = _remap_key(p_camera, fk); // (the user's keys from Editor Settings > Shortcuts)
		if (rk >= 0) {
			return rk;
		}
	}
	const int32_t tr = _texture_input(p_camera, p_event);
	if (tr >= 0) {
		if (tr == AFTER_GUI_INPUT_STOP) {
			if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
				vp->set_input_as_handled();
			}
		}
		return tr;
	}
	Ref<InputEventKey> wk = p_event;
	// (also while a tool runs: the knife, bisect, box cut, spin...; not in the middle of a move / turn / scale)
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_G && wk->is_alt_pressed() && !wk->is_ctrl_pressed() && !wk->is_shift_pressed() &&
			modal != MODAL_MOVE && modal != MODAL_ROTATE && modal != MODAL_SCALE && modal != MODAL_UVT && modal != MODAL_UV) {
		set_face_grid(!face_grid); // Alt+G: the grid on faces
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && (key_code(wk) == KEY_BRACKETLEFT || key_code(wk) == KEY_BRACKETRIGHT) && !wk->is_ctrl_pressed() &&
			!wk->is_alt_pressed() && !wk->is_shift_pressed() && !wk->is_meta_pressed() && !vpaint) {
		// [ / ]: the grid step half / double, everywhere (also while drawing)
		if (dock && dock->snap_move) {
			const double v = dock->snap_move->get_value();
			dock->snap_move->set_value(key_code(wk) == KEY_BRACKETRIGHT ? v * 2.0 : v * 0.5);
		}
		if (face_grid) {
			_update_face_grid();
		}
		update_overlays();
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_B && !wk->is_shift_pressed() && !wk->is_ctrl_pressed() && !wk->is_alt_pressed() &&
			!wk->is_meta_pressed() && (modal == MODAL_NONE || (modal == MODAL_SHAPE && shape_cut)) && drag == DRAG_NONE) {
		// B: draw a box (also instead of a cut being drawn); in edit mode it becomes part of the edited mesh. (During G / R / S, B is
		// the snap base: that is the modal's key.)
		start_shape_tool(hewn::shapes::CUBE);
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_X && wk->is_shift_pressed() && !wk->is_ctrl_pressed() && !wk->is_alt_pressed() &&
			modal == MODAL_NONE) {
		// Shift+X: the 3D cursor onto the grid / surface with one click
		start_cursor_tool();
		cursor_once = true;
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_X && wk->is_alt_pressed() && !wk->is_ctrl_pressed() && !wk->is_shift_pressed() &&
			modal == MODAL_NONE) {
		start_clip(); // Alt+X: Clip
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_A && wk->is_shift_pressed() && !wk->is_ctrl_pressed() && !wk->is_alt_pressed() &&
			modal == MODAL_NONE && drag == DRAG_NONE) {
		_add_menu(p_camera); // Shift+A: the Add menu under the mouse
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_C && !wk->is_shift_pressed() && !wk->is_ctrl_pressed() && !wk->is_alt_pressed() &&
			!wk->is_meta_pressed() && (modal == MODAL_NONE || (modal == MODAL_SHAPE && !shape_cut)) && drag == DRAG_NONE) {
		if (modal == MODAL_SHAPE) {
			cancel_tool(); // (the box builder gives way to the cutter)
		}
		start_cut_tool(1); // C: Box Cut
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (aim_light.is_valid()) {
		// a spot light's Aim at a Point: the click turns it toward the surface there, Esc / RMB: never mind
		// (it turns toward the point under the mouse while it moves)
		Node3D *light = Object::cast_to<Node3D>(ObjectDB::get_instance(aim_light));
		Ref<InputEventMouseButton> amb = p_event;
		if (!light || (wk.is_valid() && wk->is_pressed() && key_code(wk) == KEY_ESCAPE) ||
				(amb.is_valid() && amb->is_pressed() && amb->get_button_index() == MOUSE_BUTTON_RIGHT)) {
			if (light) {
				light->set_global_transform(aim_before);
			}
			aim_light = ObjectID();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		auto aimed = [&](const Vector2 &p_at, Transform3D &r_xf) {
			Vector3 o, n, u, v;
			if (!_surface_frame(p_camera, p_at, o, n, u, v) || o.distance_to(aim_before.origin) <= 1e-4f) {
				return false;
			}
			const Vector3 dir = (o - aim_before.origin).normalized();
			const Vector3 up = std::fabs(dir.y) > 0.99f ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
			r_xf = aim_before.looking_at(o, up);
			r_xf.basis.scale_local(aim_before.basis.get_scale());
			return true;
		};
		if (amb.is_valid() && amb->is_pressed() && amb->get_button_index() == MOUSE_BUTTON_LEFT) {
			Transform3D after;
			if (aimed(amb->get_position(), after)) {
				light->set_global_transform(aim_before);
				EditorUndoRedoManager *ur = get_undo_redo();
				ur->create_action("Hewn: Aim Light");
				ur->add_do_method(light, "set_global_transform", after);
				ur->add_undo_method(light, "set_global_transform", aim_before);
				ur->commit_action();
			} else {
				light->set_global_transform(aim_before);
			}
			aim_light = ObjectID();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		Ref<InputEventMouseMotion> amm = p_event;
		if (amm.is_valid()) {
			Transform3D now;
			if (aimed(amm->get_position(), now)) {
				light->set_global_transform(now);
			}
			update_overlays();
		}
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_W && modal == MODAL_NONE && !wk->is_ctrl_pressed() &&
			!wk->is_alt_pressed() && !wk->is_shift_pressed() && !wk->is_meta_pressed() && _pending_ready()) {
		_run_pending();
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_J && modal == MODAL_NONE && mode == MODE_OBJECT && (wk->is_ctrl_pressed() || wk->is_meta_pressed()) &&
			!wk->is_alt_pressed() && !wk->is_shift_pressed() && _selected_meshes().size() >= 2) {
		run_op("merge_objects"); // Ctrl+J: the selected objects into one
		if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
			vp->set_input_as_handled();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (wk.is_valid() && wk->is_pressed() && !wk->is_echo() && key_code(wk) == KEY_W && modal == MODAL_NONE && mode == MODE_OBJECT && !wk->is_ctrl_pressed() &&
			!wk->is_alt_pressed() && !wk->is_shift_pressed() && !wk->is_meta_pressed() && drag == DRAG_NONE) {
		// Repeat Prefab: one placed scene selected - W places another one of it
		const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
		Node3D *one = sel.size() == 1 ? Object::cast_to<Node3D>(sel[0]) : nullptr;
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		if (one && one != root && !one->get_scene_file_path().is_empty() && !Object::cast_to<HewnMesh3D>(one)) {
			start_prefab(one->get_scene_file_path());
			if (Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport()) {
				vp->set_input_as_handled();
			}
			return AFTER_GUI_INPUT_STOP;
		}
	}
	// the selection's undo step: before the event (a press: until its release)
	Ref<InputEventMouseButton> sel_mb = p_event;
	Ref<InputEventKey> sel_k = p_event;
	const bool sel_watch = mode != MODE_OBJECT && modal == MODAL_NONE && !editing &&
			((sel_mb.is_valid() && sel_mb->get_button_index() == MOUSE_BUTTON_LEFT) || (sel_k.is_valid() && sel_k->is_pressed() && !sel_k->is_echo()));
	if (sel_watch && (!sel_undo_pending || (sel_mb.is_valid() && sel_mb->is_pressed()))) {
		sel_undo_before = _sel_snapshot();
		sel_undo_version = _history_version();
		sel_undo_pending = true;
	}
	const int32_t r = _input_impl(p_camera, p_event);
	if (sel_undo_pending && (sel_k.is_valid() || (sel_mb.is_valid() && !sel_mb->is_pressed() && sel_mb->get_button_index() == MOUSE_BUTTON_LEFT))) {
		_sel_undo_commit();
	}
	if (r != AFTER_GUI_INPUT_STOP) {
		_note_zoom_bar(p_event); // (Godot zooms with it)
	}
	if (r == AFTER_GUI_INPUT_STOP) {
		// Stopping the 3D viewport is not enough: unhandled key events would still reach the editor
		// shortcuts (numpad-emulated views on 1/3, W/E/R tools, Ctrl+R...).
		Viewport *vp = EditorInterface::get_singleton()->get_base_control()->get_viewport();
		if (vp) {
			vp->set_input_as_handled();
		}
	}
	return r;
}

Control *HewnEditorPlugin::_surface_for(Camera3D *p_cam) const {
	Viewport *vp = p_cam ? p_cam->get_viewport() : nullptr;
	Node *holder = vp ? vp->get_parent() : nullptr;
	holder = holder ? holder->get_parent() : nullptr;
	if (!holder) {
		return nullptr;
	}
	for (int i = 0; i < holder->get_child_count(); i++) {
		Control *c = Object::cast_to<Control>(holder->get_child(i));
		if (c && c->get_class() == "Control") {
			return c;
		}
	}
	return nullptr;
}

// Godot draws a zoom / fly-speed bar at the left edge of the viewport for a moment after the mouse
// wheel (or a zoom drag / gesture) is used; our left panels step aside only while it shows.
void HewnEditorPlugin::_note_zoom_bar(const Ref<InputEvent> &p_event) {
	bool zoom = false;
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->is_pressed()) {
		const MouseButton b = mb->get_button_index();
		zoom = b == MOUSE_BUTTON_WHEEL_UP || b == MOUSE_BUTTON_WHEEL_DOWN;
	}
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && mm->get_button_mask().has_flag(MOUSE_BUTTON_MASK_MIDDLE) && (mm->is_ctrl_pressed() || mm->is_alt_pressed())) {
		zoom = true; // zoom / orbit-zoom navigation drags
	}
	if (Object::cast_to<InputEventMagnifyGesture>(p_event.ptr())) {
		zoom = true;
	}
	if (zoom) {
		zoom_bar_until = Time::get_singleton()->get_ticks_msec() + 1700; // Godot shows it for 1.5 s
	}
}

// x of a left panel in the viewport (pixels from its left edge) whose rows p_y .. p_y + p_h are given:
// at the edge, or right of the zoom bar while it shows next to the panel.
float HewnEditorPlugin::_left_offset(Control *p_surface, float p_y, float p_h, bool p_can_move, float p_extra) const {
	const float s = EditorInterface::get_singleton()->get_editor_scale();
	const float edge = 6.0f * s; // a small gap from the edge of the viewport
	if (!p_can_move || !p_surface) {
		return edge;
	}
	// the bar: the middle half of the viewport height, its icon and text under it
	const float h = p_surface->get_size().y;
	const float top = h * 0.25f - 4.0f * s;
	const float bottom = h * 0.75f + 44.0f * s;
	if (p_y > bottom || p_y + p_h < top) {
		return edge;
	}
	return edge + (left_gutter() + (10.0f + p_extra) * s - edge) * zoom_bar_t; // (clear of the bar's "12 m" too)
}

// Bottom left of the viewport; a panel too tall to stay under the tool column goes right of it.
Vector2 HewnEditorPlugin::_left_panel_pos(Control *p_surface, Control *p_panel) const {
	const Vector2 size = p_panel->get_combined_minimum_size();
	const float y = MAX(4.0f, p_surface->get_size().y - size.y - 10.0f);
	float x = _left_offset(p_surface, y, size.y, true, 15.0f); // (the bottom panels: a little more room)
	Control *col = dock ? dock->tool_column : nullptr;
	if (col && col->is_visible_in_tree()) {
		const Rect2 cr(col->get_global_position() - p_surface->get_global_position(), col->get_size());
		if (y < cr.get_end().y + 4.0f && x < cr.get_end().x) {
			x = cr.get_end().x + 6.0f * EditorInterface::get_singleton()->get_editor_scale();
		}
	}
	return Vector2(x, y);
}

// small panels: thin margins, rows close together, long names cut (the whole one in the
// tooltip), so a shape with many parameters does not cover the view.
void HewnEditorPlugin::_compact_panel(PanelContainer *p_panel, GridContainer *p_body) {
	const float s = EditorInterface::get_singleton()->get_editor_scale();
	Ref<StyleBox> sb = EditorInterface::get_singleton()->get_editor_theme()->get_stylebox("panel", "PopupPanel");
	if (sb.is_valid()) {
		Ref<StyleBox> c = sb->duplicate();
		for (int side = 0; side < 4; side++) {
			c->set_content_margin((Side)side, 4.0f * s);
		}
		p_panel->add_theme_stylebox_override("panel", c);
	}
	if (VBoxContainer *v = Object::cast_to<VBoxContainer>(p_panel->get_child(0))) {
		v->add_theme_constant_override("separation", (int)(2 * s));
	}
	p_body->add_theme_constant_override("h_separation", (int)(6 * s));
	p_body->add_theme_constant_override("v_separation", (int)(1 * s));
}

void HewnEditorPlugin::_compact_label(Label *p_label, const String &p_text) {
	const float s = EditorInterface::get_singleton()->get_editor_scale();
	p_label->set_text(p_text);
	p_label->set_tooltip_text(p_text);
	p_label->set_mouse_filter(Control::MOUSE_FILTER_PASS);
	p_label->set_custom_minimum_size(Vector2(60 * s, 0)); // (the whole name: the column is as wide as the longest)
}

void HewnEditorPlugin::_place_left_panels(double p_delta) {
	const bool bar = Time::get_singleton()->get_ticks_msec() < zoom_bar_until;
	const float target = bar ? 1.0f : 0.0f;
	const float step = (float)p_delta * 8.0f; // slides in about a tenth of a second
	zoom_bar_t = zoom_bar_t < target ? MIN(target, zoom_bar_t + step) : MAX(target, zoom_bar_t - step);
	for (Control *p : { (Control *)redo_panel, (Control *)shape_panel, (Control *)obj_panel }) {
		if (!p || !p->is_visible() || !p->get_parent()) {
			continue;
		}
		Control *surface = Object::cast_to<Control>(p->get_parent());
		const Vector2 pos = _left_panel_pos(surface, p);
		if (p->get_position() != pos) {
			p->set_position(pos);
		}
	}
}

void HewnEditorPlugin::_process(double p_delta) {
	_phys_step();
	if (heavy_any_wait && !Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) {
		_heavy_any_popup();
	}
	if (heavy_wait && !Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) {
		heavy_wait = false;
		_heavy_popup();
	}
	_layout_toolbar();
	_ei_update(false); // (only while its tab is shown)
	if (modal == MODAL_NONE && !editing) {
		// (a move shown with only the positions that ended some other way: the mesh made whole)
		for (HewnMesh3D *em : edited_meshes()) {
			em->finish_positions();
		}
	}
	if (field_drag_pending && !_field_dragging()) {
		_flush_field_drag();
	}
	if (scatter_dock && redo_check_frames == 5) {
		scatter_dock->poll();
	}
	if (!field_drag_pending && !_field_dragging() && !redo_running && ++redo_check_frames >= 10) {
		// (the panel goes when the history moved on some other way too: a button, a menu, the other viewport)
		redo_check_frames = 0;
		_check_redo_valid();
	}
	if (paint_view == 6 && ++paint_view_frames >= 30) {
		// (new objects, a collision changed: the view made again)
		paint_view_frames = 0;
		if ((_collision_view_scan(nullptr) ^ (collision_view_shapes ? _collision_shapes_key() : 0)) != collision_view_key) {
			apply_paint_view();
		}
	} else if ((paint_view == 1 || paint_view >= 3) && ++paint_view_frames >= 30) {
		// (new meshes, another scene: they get the view too)
		paint_view_frames = 0;
		size_t shown = 0;
		const std::vector<HewnMesh3D *> all = _scene_meshes();
		for (HewnMesh3D *m : all) {
			shown += std::find(paint_view_ids.begin(), paint_view_ids.end(), ObjectID(m->get_instance_id())) != paint_view_ids.end() ? 1 : 0;
		}
		if (shown != all.size() || paint_view_ids.size() != all.size()) {
			apply_paint_view();
		}
	}
	if (dock) {
		dock->update_snap_highlight();
	}
	_update_library_dim();
	if (vis_click_frames > 0 && --vis_click_frames == 0) {
		_check_visible_click();
	}
	if (issue_pending >= 0) {
		// Check Scene: the problem's faces / vertices once its mesh is the edited one
		HewnMesh3D *pm = issue_pending < (int)issues.size() ? Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(issues[issue_pending].node)) : nullptr;
		if (pm && get_edited() == pm) {
			_issue_apply_selection(issue_pending);
			issue_pending = -1;
		} else if (--issue_pending_frames <= 0) {
			issue_pending = -1;
		}
	}
	{
		HewnMesh3D *n = get_edited();
		const uint64_t want = n && mode != MODE_OBJECT ? (uint64_t)n->get_instance_id() : 0;
		if (want != hiding_id) {
			if (HewnMesh3D *old = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(ObjectID(hiding_id)))) {
				old->set_hiding_faces(false);
			}
			hiding_id = want;
			if (n && want) {
				n->set_hiding_faces(true);
			}
		}
	}
	if (face_grid) {
		face_grid_timer += p_delta;
		if (face_grid_timer > 0.5) {
			face_grid_timer = 0.0;
			_update_face_grid(); // (new meshes, another snap step)
		}
	}
	if (dock && dock->ov_overlap && dock->ov_overlap->is_pressed()) {
		overlap_timer += p_delta;
		if (overlap_timer > 0.25) {
			overlap_timer = 0.0;
			_update_overlaps(); // (the mesh changes in many ways)
			update_overlays();
		}
	} else if (!overlap_same.empty() || !overlap_opposite.empty() || overlap_count) {
		_update_overlaps();
		update_overlays();
	}
	_update_shape_panel();
	_update_obj_panel();
	if (vpaint && (mode == MODE_OBJECT || !get_edited())) {
		exit_vpaint();
	}
	if (vpaint) {
		// the brush hides while the camera flies, shows again after
		const bool rmb = Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT);
		if (rmb != paint_rmb_was) {
			paint_rmb_was = rmb;
			update_overlays();
		}
	}
	if (dock && dock->paint_bar_shown != vpaint) {
		dock->show_paint_bar(vpaint || sculpt);
	}
	if (mode == MODE_OBJECT) {
		// the origin dots follow objects moved with Godot's own gizmo
		uint64_t h = 1469598103934665603ull;
		for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_selected_nodes()) {
			if (Node3D *n3 = Object::cast_to<Node3D>(v)) {
				if (n3->is_inside_tree()) {
					const Vector3 o = n3->get_global_transform().origin;
					h = (h ^ (uint64_t)n3->get_instance_id()) * 1099511628211ull;
					h = (h ^ (uint64_t)std::hash<float>()(o.x + o.y * 3.1f + o.z * 7.3f)) * 1099511628211ull;
				}
			}
		}
		if (h != origin_dots_hash) {
			origin_dots_hash = h;
			update_overlays();
		}
	}
	_place_left_panels(p_delta);
	const bool active = drag == DRAG_HANDLE || modal == MODAL_MOVE || modal == MODAL_ROTATE || modal == MODAL_SCALE || modal == MODAL_BEVEL ||
			modal == MODAL_INSET || modal == MODAL_SLIDE || modal == MODAL_EXTRUDE_AMOUNT || modal == MODAL_VALUE || (modal == MODAL_UVT && uvt.drag >= 0);
	if (!active || !dock || !wrap_cursor_on || DisplayServer::get_singleton()->get_name() == "headless") {
		return;
	}
	Control *surface = last_surface ? Object::cast_to<Control>(ObjectDB::get_instance(last_surface)) : nullptr;
	if (!surface || !surface->is_visible_in_tree()) {
		return;
	}
	const Vector2 local = Vector2(DisplayServer::get_singleton()->mouse_get_position()) - surface->get_screen_position();
	const Vector2 size = surface->get_size();
	const float m = 3.0f;
	if (local.x >= m && local.y >= m && local.x <= size.x - m && local.y <= size.y - m) {
		return;
	}
	// Wrap to the opposite side, keeping how far past the edge the cursor went.
	Vector2 to = local;
	if (local.x < m) {
		to.x = size.x - m * 4.0f + MAX(-size.x * 0.5f, local.x - m);
	} else if (local.x > size.x - m) {
		to.x = m * 4.0f + MIN(size.x * 0.5f, local.x - (size.x - m));
	}
	if (local.y < m) {
		to.y = size.y - m * 4.0f + MAX(-size.y * 0.5f, local.y - m);
	} else if (local.y > size.y - m) {
		to.y = m * 4.0f + MIN(size.y * 0.5f, local.y - (size.y - m));
	}
	_warp_to(surface, local, to);
}

// Moves the system cursor; the precise-mouse offsets absorb the jump. Motion events that were queued
// before the warp still carry old positions: _wrap_mouse shifts those by the jump (see warp_pending).
void HewnEditorPlugin::_warp_to(Control *p_surface, const Vector2 &p_from, const Vector2 &p_to) {
	const Vector2 jump = p_to - p_from;
	if (prec_active) {
		prec_raw0 += jump;
	} else {
		prec_offset -= jump;
	}
	warp_pending = true;
	warp_jump = jump;
	warp_target = p_to;
	warp_ms = Time::get_singleton()->get_ticks_msec();
	Input::get_singleton()->warp_mouse(p_surface->get_global_rect().position + p_to);
}

// Cursor wrapping during operations: at a viewport edge the system cursor jumps to the
// opposite edge; the precise-mouse offsets absorb the jump so the operation continues smoothly.
Vector2 HewnEditorPlugin::_wrap_mouse(Camera3D *p_cam, const Vector2 &p_raw) {
	Control *surface = _surface_for(p_cam);
	if (!surface || !wrap_cursor_on || DisplayServer::get_singleton()->get_name() == "headless") {
		return p_raw;
	}
	if (warp_pending) {
		// An event from before the last warp is nearer to where the cursor was than to where it went.
		const Vector2 before = warp_target - warp_jump;
		if (p_raw.distance_to(before) < p_raw.distance_to(warp_target) && Time::get_singleton()->get_ticks_msec() - warp_ms < 500) {
			return p_raw + warp_jump;
		}
		warp_pending = false;
	}
	const Vector2 size = surface->get_size();
	const float m = 3.0f;
	Vector2 to = p_raw;
	if (p_raw.x < m) {
		to.x = size.x - m * 3.0f;
	} else if (p_raw.x > size.x - m) {
		to.x = m * 3.0f;
	}
	if (p_raw.y < m) {
		to.y = size.y - m * 3.0f;
	} else if (p_raw.y > size.y - m) {
		to.y = m * 3.0f;
	}
	if (to == p_raw) {
		return p_raw;
	}
	_warp_to(surface, p_raw, to);
	return to;
}

// The 3D cursor onto the surface under the mouse (or the ground), lying on it.
void HewnEditorPlugin::_place_cursor(Camera3D *p_cam, const Vector2 &p_pos, bool p_ctrl) {
	Vector3 o, d, hit, nrm;
	_ray(p_cam, p_pos, o, d);
	if (_raycast_scene(o, d, hit, nrm)) {
		cursor_pos = hit;
		// Y along the surface normal, X as close to world X as possible
		Vector3 y = nrm.normalized();
		Vector3 x = Vector3(1, 0, 0) - y * y.x;
		if (x.length() < 0.3f) {
			x = Vector3(0, 0, 1) - y * y.z;
		}
		x.normalize();
		cursor_basis = Basis();
		cursor_basis.set_column(0, x);
		cursor_basis.set_column(1, y);
		cursor_basis.set_column(2, x.cross(y).normalized());
	} else if (std::fabs(d.y) > 1e-6f && -o.y / d.y > 0.0f) {
		cursor_pos = o + d * (-o.y / d.y);
		cursor_basis = Basis();
	}
	// snapping (Ctrl: the other way): onto a vertex / edge / center of the scene, else in grid steps
	snap_mark_valid = false;
	tool_ctrl = p_ctrl;
	if (snapping_active(p_ctrl)) {
		Vector3 sp;
		const float sm = (float)dock->snap_move->get_value();
		if ((dock->snap_targets & HewnDock::SNAP_POINTS) && _snap_target(p_cam, p_pos, sp)) {
			cursor_pos = sp;
			snap_mark = sp;
			snap_mark_valid = true;
			snap_mark_geom = true;
			snap_mark_kind = last_snap_kind;
		} else if ((dock->snap_targets & HewnDock::SNAP_STEPS) && sm > 0.0f) {
			cursor_pos = (cursor_pos / sm).round() * sm;
			snap_mark = cursor_pos;
			snap_mark_valid = true;
			snap_mark_geom = false;
		}
	}
	update_overlays();
}

bool HewnEditorPlugin::_mouse_near_cursor(Camera3D *p_cam) const {
	if (!p_cam || p_cam->is_position_behind(cursor_pos)) {
		return false;
	}
	if (mode != MODE_OBJECT && (hover_vert >= 0 || hover_edge.a >= 0)) {
		return false; // (a vertex / edge under the mouse is what G moves)
	}
	return p_cam->unproject_position(cursor_pos).distance_to(mouse_pos) < 9.0f * EditorInterface::get_singleton()->get_editor_scale();
}

void HewnEditorPlugin::_start_cursor_xf(Camera3D *p_cam, int p_kind) {
	if (!p_cam) {
		return;
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	modal = MODAL_CURSOR;
	cursor_once = false;
	cursor_xf = p_kind;
	_gather_snap_candidates(false);
	cursor_xf_axis = -1;
	cursor_xf_pos0 = cursor_pos;
	cursor_xf_basis0 = cursor_basis;
	cursor_xf_mouse0 = mouse_pos;
	cursor_xf_t0 = 0.0f;
	update_overlays();
}

void HewnEditorPlugin::_cursor_xf_update(Camera3D *p_cam, bool p_ctrl) {
	const bool snap = snapping_active(p_ctrl);
	if (cursor_xf == 1) {
		if (cursor_xf_axis < 0) {
			// free: onto the surface under the mouse (its vertices / edges with snapping), on the ground else
			const Basis keep = cursor_basis;
			_place_cursor(p_cam, mouse_pos, p_ctrl);
			cursor_basis = keep;
		} else {
			const Vector3 ax = AXES[cursor_xf_axis];
			float t = _axis_param(p_cam, mouse_pos, cursor_xf_pos0, ax) - cursor_xf_t0;
			if (snap && dock) {
				const float st = MAX(1e-4f, (float)dock->snap_move->get_value());
				t = std::round(t / st) * st;
			}
			cursor_pos = cursor_xf_pos0 + ax * t;
		}
	} else if (cursor_xf == 2) {
		const Vector2 c = p_cam->is_position_behind(cursor_xf_pos0) ? Vector2() : p_cam->unproject_position(cursor_xf_pos0);
		float a = (mouse_pos - c).angle() - (cursor_xf_mouse0 - c).angle();
		const Vector3 to_eye = p_cam->get_global_position() - cursor_xf_pos0;
		Vector3 ax = cursor_xf_axis < 0 ? p_cam->get_global_transform().basis.get_column(2).normalized() : AXES[cursor_xf_axis];
		if (ax.dot(to_eye) < 0.0f) {
			ax = -ax; // (turning with the mouse on screen: the axis toward the eye)
		}
		a = -a; // (screen y goes down)
		if (snap && dock) {
			const float st = Math::deg_to_rad(MAX(0.1f, (float)dock->snap_rotate->get_value()));
			a = std::round(a / st) * st;
		}
		cursor_basis = Basis(ax, a) * cursor_xf_basis0;
	}
	update_overlays();
}

void HewnEditorPlugin::_on_cursor_menu(int p_id) {
	if (p_id == 4 || p_id == 5) {
		SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(0);
		_start_cursor_xf(vp ? vp->get_camera_3d() : nullptr, p_id == 4 ? 1 : 2);
		return;
	}
	HewnMesh3D *n = get_edited();
	if (p_id == 1) {
		cursor_pos = Vector3();
	} else if (p_id == 0) {
		Vector3 p;
		if (_pivot(p)) {
			cursor_pos = p;
		}
	} else if (p_id == 2 && n) {
		cursor_pos = n->get_global_position();
	} else if (p_id == 3 && n) {
		if (mode == MODE_OBJECT) {
			Transform3D before = n->get_global_transform();
			Transform3D after = before;
			after.origin = cursor_pos;
			EditorUndoRedoManager *ur = get_undo_redo();
			ur->create_action("Hewn: Selection to Cursor", UndoRedo::MERGE_DISABLE, n);
			ur->add_do_method(n, "set_global_transform", after);
			ur->add_undo_method(n, "set_global_transform", before);
			ur->commit_action();
		} else {
			Vector3 piv;
			if (_pivot(piv)) {
				begin_edit("Selection to Cursor");
				const Vector3 delta = n->get_global_transform().affine_inverse().basis.xform(cursor_pos - piv);
				for (int v : _selected_verts()) {
					n->get_data().pos[v] += delta;
				}
				n->commit(true);
				end_edit();
			}
		}
	}
	update_overlays();
}

void HewnEditorPlugin::_draw_cursor(Control *p_overlay, Camera3D *p_cam) {
	if (p_cam->is_position_behind(cursor_pos)) {
		return;
	}
	const Vector2 c = p_cam->unproject_position(cursor_pos);
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	// moved / turned along an axis: that axis through where it started, in its color
	if (modal == MODAL_CURSOR && cursor_xf != 0 && cursor_xf_axis >= 0) {
		const Vector3 ax = AXES[cursor_xf_axis];
		const Vector3 a = cursor_xf_pos0 - ax * 1000.0f, b = cursor_xf_pos0 + ax * 1000.0f;
		const Color ac = cursor_xf_axis == 0 ? Color(0.95f, 0.3f, 0.3f) : cursor_xf_axis == 1 ? Color(0.45f, 0.9f, 0.3f) : Color(0.35f, 0.55f, 1.0f);
		// (clipped to the part in front of the camera)
		const Vector3 eye = p_cam->get_global_position(), fwd = -p_cam->get_global_transform().basis.get_column(2).normalized();
		const float near = p_cam->get_near() * 1.5f, da = (a - eye).dot(fwd), db = (b - eye).dot(fwd);
		Vector3 ca = a, cb = b;
		if (da < near && db >= near) {
			ca = a.lerp(b, (near - da) / (db - da));
		} else if (db < near && da >= near) {
			cb = b.lerp(a, (near - db) / (da - db));
		}
		if (!p_cam->is_position_behind(ca) && !p_cam->is_position_behind(cb)) {
			p_overlay->draw_line(p_cam->unproject_position(ca), p_cam->unproject_position(cb), ac, 1.0f, true);
		}
	}
	const int segs = 12;
	const float rad = 8.0f * es;
	for (int i = 0; i < segs; i++) {
		const float a0 = (float)Math::PI * 2.0f * i / segs, a1 = (float)Math::PI * 2.0f * (i + 1) / segs;
		p_overlay->draw_line(c + Vector2(std::cos(a0), std::sin(a0)) * rad, c + Vector2(std::cos(a1), std::sin(a1)) * rad,
				i % 2 ? Color(1, 1, 1) : Color(0.9f, 0.15f, 0.15f), 1.5f, true);
	}
	// Cross hair along the cursor's own X / Z axes, in 3D (it lies on the surface it was placed on); its ends
	// in the axes' colors.
	const float wpp = _world_per_pixel(p_cam, cursor_pos);
	for (int axis = 0; axis < 3; axis += 2) {
		const Vector3 d = cursor_basis.get_column(axis).normalized();
		const Color ac = axis == 0 ? Color(0.95f, 0.3f, 0.3f) : Color(0.35f, 0.55f, 1.0f);
		for (int sgn = -1; sgn <= 1; sgn += 2) {
			const Vector3 a = cursor_pos + d * (sgn * 4.0f * wpp), m = cursor_pos + d * (sgn * 12.0f * wpp), b = cursor_pos + d * (sgn * 18.0f * wpp);
			if (!p_cam->is_position_behind(a) && !p_cam->is_position_behind(b)) {
				p_overlay->draw_line(p_cam->unproject_position(a), p_cam->unproject_position(m), Color(0, 0, 0, 0.85f), 1.0f, true);
				p_overlay->draw_line(p_cam->unproject_position(m), p_cam->unproject_position(b), ac, 1.0f, true);
			}
		}
	}
}

// Our clicks never reach Godot's viewport, so it would not take the keyboard focus back after the user
// clicked another panel; then the digits and Q/W/E/R went to the editor shortcuts again.
void HewnEditorPlugin::_focus_viewport(Camera3D *p_cam) {
	Control *c = _surface_for(p_cam);
	if (c && !c->has_focus()) {
		c->grab_focus();
	}
}

int32_t HewnEditorPlugin::_input_impl(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (phys_on) {
		// physics placement: its keys and the drags of the objects first
		const int32_t pr = _physics_input(p_camera, p_event);
		if (pr >= 0) {
			if (Viewport *v = get_viewport()) {
				v->set_input_as_handled();
			}
			return pr;
		}
	}
	{
		Ref<InputEventMouseButton> press = p_event;
		if (press.is_valid() && press->is_pressed() && (mode != MODE_OBJECT || modal != MODAL_NONE)) {
			_focus_viewport(p_camera);
		}
		Control *surface = _surface_for(p_camera);
		if (surface) {
			last_surface = surface->get_instance_id();
		}
		_check_redo_valid();
	}
	_validate_edited();
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		mouse_pos = mm->get_position();
	}
	{
		// Ctrl / Shift + wheel in Vertex Paint: the brush, whichever tool (but the circle select's Ctrl+wheel)
		Ref<InputEventMouseButton> wb = p_event;
		if (wb.is_valid() && vpaint && modal != MODAL_PAINT && !(select_kind == 1 && (wb->is_ctrl_pressed() || wb->is_meta_pressed())) && _paint_wheel(wb)) {
			return AFTER_GUI_INPUT_STOP;
		}
	}
	if (sculpt && modal == MODAL_SCULPT) {
		return _sculpt_input(p_camera, p_event); // (Sculpt: its brushes, and nothing of the edit mode)
	}
	bool paint_key_on = false; // (Vertex Paint's brush: the selection keys go on to the edit mode's handling)
	if (modal == MODAL_PAINT) {
		Ref<InputEventKey> pk = p_event;
		if (pk.is_valid()) {
			const int32_t r = _vpaint_keys(p_camera, pk);
			if (r >= 0) {
				return r;
			}
			paint_key_on = true;
		}
	}
	if (!paint_key_on && (modal == MODAL_KNIFE || modal == MODAL_LOOPCUT || modal == MODAL_CUTTER || modal == MODAL_POLY || modal == MODAL_RULER || modal == MODAL_SHAPE || modal == MODAL_PAINT || modal == MODAL_BISECT ||
			modal == MODAL_CURSOR || modal == MODAL_CLIP || modal == MODAL_PREFAB || modal == MODAL_SPIN || modal == MODAL_FACECUT)) {
		Ref<InputEventKey> tk = p_event;
		if (tk.is_valid() && tk->is_pressed() && !tk->is_echo() && key_code(tk) == KEY_TAB && tk->is_shift_pressed() && !tk->is_ctrl_pressed() && !tk->is_alt_pressed()) {
			dock->snap_enabled->set_pressed(!dock->snap_enabled->is_pressed()); // snapping on/off works inside the tools too
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (tk.is_valid() && tk->is_pressed() && !tk->is_echo() && mode != MODE_OBJECT && !tk->is_ctrl_pressed() && !tk->is_alt_pressed() &&
				!tk->is_shift_pressed() && (key_code(tk) == KEY_1 || key_code(tk) == KEY_2 || key_code(tk) == KEY_3) && modal != MODAL_POLY &&
				!(modal == MODAL_SHAPE && shape_state == 2)) { // (the height being typed)
			cancel_tool();
			set_mode(key_code(tk) == KEY_1 ? MODE_VERTEX : key_code(tk) == KEY_2 ? MODE_EDGE : MODE_FACE);
			return AFTER_GUI_INPUT_STOP;
		}
		if (tk.is_valid() && !tk->is_echo() && (key_code(tk) == KEY_CTRL || key_code(tk) == KEY_META)) {
			// Ctrl inverts snapping: recompute the snap marker / preview right away, not on the next move.
			Ref<InputEventMouseMotion> fake;
			fake.instantiate();
			fake->set_position(mouse_pos);
			fake->set_global_position(mouse_pos);
			fake->set_ctrl_pressed(ctrl_after(tk));
			fake->set_shift_pressed(tk->is_shift_pressed());
			fake->set_alt_pressed(tk->is_alt_pressed());
			_tool_input(p_camera, fake);
			_update_poly_preview();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		const int32_t r = _tool_input(p_camera, p_event);
		_update_poly_preview();
		if (r == AFTER_GUI_INPUT_PASS && tk.is_valid() && mode != MODE_OBJECT && !tk->is_ctrl_pressed()) {
			return AFTER_GUI_INPUT_STOP; // e.g. 1/3 would otherwise rotate the view, Q/W/E/R switch Godot tools
		}
		// a left click (or its release) the tool did not take: not to Godot either (it would select another object
		// on the release and leave the edit mode)
		Ref<InputEventMouseButton> tmb = p_event;
		if (r == AFTER_GUI_INPUT_PASS && tmb.is_valid() && tmb->get_button_index() == MOUSE_BUTTON_LEFT && mode != MODE_OBJECT) {
			return AFTER_GUI_INPUT_STOP;
		}
		return r;
	}
	if (modal == MODAL_UV) {
		return _uv_modal_input(p_camera, p_event);
	}
	if (modal == MODAL_UVT) {
		return _uvt_input(p_camera, p_event);
	}
	if (modal == MODAL_TSNAP) {
		return _tsnap_input(p_camera, p_event);
	}
	if (modal != MODAL_NONE && !paint_key_on) {
		return _modal_input(p_camera, p_event);
	}
	if (mode == MODE_OBJECT && select_kind == 1 && modal == MODAL_NONE) {
		// (the circle select of objects: also with nothing selected, or no Hewn mesh)
		const int32_t cr = _obj_circle_input(p_camera, p_event);
		if (cr >= 0) {
			return cr;
		}
	}
	{
		const int32_t pr = _face_push_input(p_camera, p_event); // (Shift + drag on a face)
		if (pr >= 0) {
			return pr;
		}
	}
	{
		Ref<InputEventKey> ak = p_event;
		if (ak.is_valid() && modal == MODAL_NONE && !vpaint && drag == DRAG_NONE && _arrow_keys(p_camera, ak)) {
			return AFTER_GUI_INPUT_STOP;
		}
	}
	{
		// a click that selects a CSG model: the brush under the mouse gets selected (see _edit)
		Ref<InputEventMouseButton> lb = p_event;
		if (lb.is_valid() && lb->is_pressed() && lb->get_button_index() == MOUSE_BUTTON_LEFT && mode == MODE_OBJECT && !lb->is_shift_pressed() && !lb->is_ctrl_pressed() &&
				!lb->is_alt_pressed()) {
			brush_click_ms = Time::get_singleton()->get_ticks_msec();
			brush_click_pos = lb->get_position();
			brush_click_cam = p_camera ? p_camera->get_instance_id() : ObjectID();
			plain_press = true;
		}
		// (the release of a click that finished a tool is not a selection click)
		if (lb.is_valid() && !lb->is_pressed() && lb->get_button_index() == MOUSE_BUTTON_LEFT && mode == MODE_OBJECT && !lb->is_shift_pressed() && !lb->is_ctrl_pressed() &&
				!lb->is_alt_pressed() && plain_press && lb->get_position().distance_to(brush_click_pos) < 4.0f) {
			plain_press = false;
			vis_click_frames = 2; // (after Godot has picked: _process)
			vis_click_pos = lb->get_position();
			vis_click_cam = p_camera ? p_camera->get_instance_id() : ObjectID();
		}
	}
	// the 3D cursor: also with no Hewn mesh picked yet
	{
		Ref<InputEventMouseButton> cb = p_event;
		if (cb.is_valid() && cb->is_pressed() && cb->get_button_index() == MOUSE_BUTTON_RIGHT && cb->is_shift_pressed()) {
			// Shift+RMB puts the 3D cursor on the surface under the mouse (or on the ground).
			_gather_snap_candidates(false);
			_place_cursor(p_camera, cb->get_position(), cb->is_ctrl_pressed() || cb->is_meta_pressed());
			return AFTER_GUI_INPUT_STOP;
		}
		Ref<InputEventKey> ck = p_event;
		// (not while the right button flies the camera: Shift+S there is "move down faster")
		if (ck.is_valid() && ck->is_pressed() && !ck->is_echo() && key_code(ck) == KEY_S && ck->is_shift_pressed() && !ck->is_ctrl_pressed() && !ck->is_alt_pressed() &&
				!Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)) {
			Control *surface = _surface_for(p_camera);
			const Vector2 at = surface ? surface->get_screen_position() + mouse_pos : Vector2();
			cursor_menu->set_item_disabled(cursor_menu->get_item_index(0), mode == MODE_OBJECT || _selected_verts().empty());
			cursor_menu->set_item_disabled(cursor_menu->get_item_index(3), mode != MODE_OBJECT && _selected_verts().empty());
			cursor_menu->popup(Rect2i(Vector2i(at), Vector2i()));
			return AFTER_GUI_INPUT_STOP;
		}
	}
	HewnMesh3D *n = get_edited();
	if (!n) {
		// No Hewn mesh edited: G / R / S still move, turn and scale the selected nodes (any Node3D).
		Ref<InputEventKey> ok = p_event;
		if (ok.is_valid() && ok->is_pressed() && !ok->is_echo() && !ok->is_meta_pressed() && drag == DRAG_NONE &&
				!Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)) {
			const Key code = key_code(ok);
			const bool plain = !ok->is_ctrl_pressed() && !ok->is_shift_pressed() && !ok->is_alt_pressed();
			if (plain && (code == KEY_G || code == KEY_R || code == KEY_S) &&
					_start_object_transform(code == KEY_G ? MODAL_MOVE : code == KEY_R ? MODAL_ROTATE : MODAL_SCALE, p_camera)) {
				return AFTER_GUI_INPUT_STOP;
			}
			if (_object_keys(code, ok->is_ctrl_pressed(), ok->is_shift_pressed(), ok->is_alt_pressed())) {
				return AFTER_GUI_INPUT_STOP;
			}
			if (_any_mode_keys(code, ok->is_ctrl_pressed(), ok->is_shift_pressed(), ok->is_alt_pressed())) {
				return AFTER_GUI_INPUT_STOP;
			}
		}
		return AFTER_GUI_INPUT_PASS;
	}
	// While the right button is held Godot flies the camera (WASD/QE): leave every key to it.
	const bool rmb_held = Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT);

	Ref<InputEventKey> k = p_event;
	if (k.is_valid()) {
		if (rmb_held || !k->is_pressed() || k->is_echo()) {
			if (rmb_held && k->is_pressed()) {
				rmb_pending = false; // flying with WASD: no context menu on release
			}
			return AFTER_GUI_INPUT_PASS;
		}
		const Key code = key_code(k);
		const bool ctrl = k->is_ctrl_pressed() || k->is_meta_pressed();
		const bool shift = k->is_shift_pressed();
		const bool alt = k->is_alt_pressed();
		const bool plain = !ctrl && !shift && !alt;
		if (plain && (code == KEY_G || code == KEY_R) && modal == MODAL_NONE && drag == DRAG_NONE && _mouse_near_cursor(p_camera)) {
			_start_cursor_xf(p_camera, code == KEY_G ? 1 : 2); // (the mouse over the 3D cursor: it is what moves)
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_TAB && plain) {
			toggle_edit_mode();
			return AFTER_GUI_INPUT_STOP;
		}
		if (vpaint) {
			// Vertex Paint with a selection tool: its keys, the selection keys; nothing that changes the mesh
			const int32_t vr = _vpaint_keys(p_camera, k);
			if (vr >= 0) {
				return vr;
			}
		}
		if (_any_mode_keys(code, ctrl, shift, alt)) {
			return AFTER_GUI_INPUT_STOP;
		}
		if (mode == MODE_OBJECT) {
			// G / R / S move, rotate and scale the selected objects
			if (plain && (code == KEY_G || code == KEY_R || code == KEY_S) && drag == DRAG_NONE) {
				_start_object_transform(code == KEY_G ? MODAL_MOVE : code == KEY_R ? MODAL_ROTATE : MODAL_SCALE, p_camera);
				return AFTER_GUI_INPUT_STOP;
			}
			if (_object_keys(code, ctrl, shift, alt)) {
				return AFTER_GUI_INPUT_STOP;
			}
			return AFTER_GUI_INPUT_PASS;
		}
		if (code == KEY_Z && alt && !ctrl && !shift) {
			set_xray(!xray_on);
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_O && !ctrl && !shift) {
			if (alt) {
				dock->prop_connected->set_pressed(!dock->prop_connected->is_pressed()); // Alt+O: connected only
			} else {
				dock->prop_enabled->set_pressed(!dock->prop_enabled->is_pressed());
			}
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_E && alt && !ctrl && !shift && !_selected_verts().empty()) {
			_show_extrude_menu(p_camera);
			return AFTER_GUI_INPUT_STOP;
		}
		if (drag == DRAG_HANDLE) {
			if (code == KEY_ESCAPE) {
				n->set_state(edit_before);
				editing = false;
				drag = DRAG_NONE;
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			return AFTER_GUI_INPUT_PASS;
		}
		const bool has_sel = !_selected_verts().empty();
		switch (code) {
			case KEY_1:
			case KEY_2:
			case KEY_3:
				if (shift && !ctrl && !alt) {
					toggle_mode(code == KEY_1 ? MODE_VERTEX : code == KEY_2 ? MODE_EDGE : MODE_FACE);
					return AFTER_GUI_INPUT_STOP;
				}
				if (plain) {
					set_mode(code == KEY_1 ? MODE_VERTEX : code == KEY_2 ? MODE_EDGE : MODE_FACE);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_G:
				if (plain && has_sel) {
					_start_transform(MODAL_MOVE, p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_R:
				if (ctrl && shift && !alt && has_sel) {
					_start_value_modal(p_camera, "offset_edge_slide");
					return AFTER_GUI_INPUT_STOP;
				}
				if (plain && has_sel) {
					_start_transform(MODAL_ROTATE, p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				if (ctrl && !shift && !alt) {
					start_loop_cut();
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_S:
				if (alt && !ctrl && !shift && has_sel) {
					_start_value_modal(p_camera, "shrink_fatten");
					return AFTER_GUI_INPUT_STOP;
				}
				if (alt && ctrl && shift && has_sel) {
					_start_value_modal(p_camera, "shear");
					return AFTER_GUI_INPUT_STOP;
				}
				if (plain && has_sel) {
					_start_transform(MODAL_SCALE, p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_E:
				if (shift && !ctrl && !alt && mode == MODE_EDGE && has_sel) {
					run_op("edge_crease");
					return AFTER_GUI_INPUT_STOP;
				}
				if (plain && has_sel) {
					set_op_param("extrude_move", "dissolve_orthogonal", false);
				set_op_param("extrude_move", "flip_normals", false);
					_start_extrude(p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_I:
				if (plain && mode == MODE_FACE && has_sel) {
					_start_inset(p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				if (ctrl && !shift && !alt) {
					run_op("invert");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_U:
				if (plain) {
					_uv_menu(p_camera); // texture tools
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_H:
				if (!ctrl) {
					hide_faces(alt ? 2 : shift ? 1 : 0);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_B:
				if (ctrl && shift && !alt && has_sel) {
					_start_value_modal(p_camera, "bevel_vertices");
					return AFTER_GUI_INPUT_STOP;
				}
				if (ctrl && !shift && !alt && has_sel) {
					_start_bevel(p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_K:
				if (plain) {
					start_knife();
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_A:
				if (plain) {
					run_op("select_all");
					return AFTER_GUI_INPUT_STOP;
				}
				if (alt && !ctrl && !shift) {
					run_op("select_none");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_L:
				if (plain) {
					_select_linked_at(p_camera, mouse_pos);
					return AFTER_GUI_INPUT_STOP;
				}
				if (ctrl && !shift && !alt) {
					run_op("linked");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_EQUAL:
			case KEY_KP_ADD:
				if (ctrl) {
					run_op("grow");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_MINUS:
			case KEY_KP_SUBTRACT:
				if (ctrl) {
					run_op("shrink");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_X:
			case KEY_DELETE:
				if (ctrl && !shift && !alt && code == KEY_X && has_sel) {
					run_op("dissolve");
					return AFTER_GUI_INPUT_STOP;
				}
				if (plain && has_sel) {
					_delete_menu(p_camera); // X / Delete open the delete menu
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_F:
				if (plain && mode != MODE_FACE) {
					run_op("make_face");
					if (f2_new_vertex >= 0 && mode == MODE_VERTEX) {
						f2_new_vertex = -1;
						_start_transform(MODAL_MOVE, p_camera);
					}
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_V:
				if (shift && !ctrl && !alt && has_sel) {
					if (_start_transform(MODAL_MOVE, p_camera)) {
						_start_slide(p_camera); // Shift+V: slide vertices
					}
					return AFTER_GUI_INPUT_STOP;
				}
				if (plain && has_sel && mode != MODE_FACE) {
					_start_rip(p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_M:
				if (shift && !ctrl && !alt) {
					start_ruler();
					return AFTER_GUI_INPUT_STOP;
				}
				if (plain && has_sel) {
					_run_menu_op(p_camera, "menu:merge");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_J:
				if (plain && mode == MODE_VERTEX) {
					run_op("connect_vertices");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_D:
				if (shift && !ctrl && !alt && mode == MODE_FACE && has_sel) {
					_start_duplicate(p_camera);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_P:
				if (plain && has_sel) {
					_run_menu_op(p_camera, "menu:separate"); // to a new mesh
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_N:
				// Shift+N recalculate outside, Ctrl+Shift+N inside, Alt+N the Normals menu.
				if (shift && !alt) {
					run_op(ctrl ? "recalc_inside" : "recalc_outside");
					return AFTER_GUI_INPUT_STOP;
				}
				if (alt && !ctrl && !shift) {
					_run_menu_op(p_camera, "menu:normals");
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			case KEY_W:
				if (plain) {
					// Move -> Rotate -> Scale -> no gizmo -> Move
					set_tool(!dock->show_gizmo->is_pressed() ? TOOL_MOVE : dock->gizmo_scale->is_pressed() ? TOOL_SELECT : dock->gizmo_rotate->is_pressed() ? TOOL_SCALE : TOOL_ROTATE);
					return AFTER_GUI_INPUT_STOP;
				}
				break;
			default:
				break;
		}
		return AFTER_GUI_INPUT_PASS;
	}

	if (mode == MODE_OBJECT) {
		// a right click (not a free look) on the selected meshes: the object context menu
		Ref<InputEventMouseButton> omb = p_event;
		if (omb.is_valid() && omb->get_button_index() == MOUSE_BUTTON_RIGHT && !omb->is_shift_pressed() && !omb->is_alt_pressed()) {
			if (omb->is_pressed()) {
				rmb_pending = true;
				rmb_moved = 0.0f;
				rmb_press_ms = Time::get_singleton()->get_ticks_msec();
				rmb_press_pos = omb->get_position();
			} else if (rmb_pending) {
				rmb_pending = false;
				bool any_curve = false;
				for (const Variant &cv : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
					any_curve = any_curve || Object::cast_to<HewnCurve3D>(cv) != nullptr;
				}
				if (Time::get_singleton()->get_ticks_msec() - rmb_press_ms < 300 && rmb_moved < 6.0f && (!_selected_meshes().empty() || (any_curve && !hc_edit))) {
					mouse_pos = rmb_press_pos;
					callable_mp(this, &HewnEditorPlugin::_context_menu_deferred).call_deferred(p_camera->get_instance_id());
				}
			}
		}
		Ref<InputEventMouseMotion> omm = p_event;
		if (omm.is_valid() && rmb_pending) {
			rmb_moved += omm->get_relative().length();
		}
		return AFTER_GUI_INPUT_PASS;
	}

	if (bisect_drag != 0 || _bisect_widget_on()) {
		// the last bisect's plane: its arrow / rings dragged
		const int32_t br = _bisect_widget_input(p_camera, p_event);
		if (br >= 0) {
			if (Ref<InputEventMouseMotion>(p_event).is_valid()) {
				mouse_pos = Ref<InputEventMouseMotion>(p_event)->get_position();
			}
			return br;
		}
	}
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_RIGHT && drag != DRAG_HANDLE && !mb->is_shift_pressed()) {
		if (mb->is_pressed()) {
			rmb_pending = true;
			rmb_moved = 0.0f;
			rmb_press_ms = Time::get_singleton()->get_ticks_msec();
			rmb_press_pos = mb->get_position();
			return AFTER_GUI_INPUT_PASS; // Godot starts flying the camera if the button stays down
		}
		if (rmb_pending) {
			rmb_pending = false;
			// Godot captures the mouse while the right button is down (free look): the release position is
			// meaningless then, so the motion is summed from the relative movements instead.
			if (Time::get_singleton()->get_ticks_msec() - rmb_press_ms < 300 && rmb_moved < 6.0f) {
				mouse_pos = rmb_press_pos;
				// Let Godot finish its free look first (it restores the cursor), then open the menu.
				callable_mp(this, &HewnEditorPlugin::_context_menu_deferred).call_deferred(p_camera->get_instance_id());
				return AFTER_GUI_INPUT_PASS;
			}
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (mm.is_valid() && rmb_pending) {
		rmb_moved += mm->get_relative().length();
	}
	if (mb.is_valid()) {
		if (mb->get_button_index() == MOUSE_BUTTON_RIGHT && mb->is_pressed() && drag == DRAG_HANDLE) {
			n->set_state(edit_before);
			editing = false;
			drag = DRAG_NONE;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (select_kind == 1 && mb->is_pressed() && (mb->is_ctrl_pressed() || mb->is_meta_pressed()) &&
				(mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP || mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN)) {
			circle_radius = CLAMP(circle_radius * (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? 1.1f : 1.0f / 1.1f), 3.0f, 500.0f); // Ctrl+wheel: the circle's size
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb->get_button_index() != MOUSE_BUTTON_LEFT) {
			return AFTER_GUI_INPUT_PASS;
		}
		if (mb->is_pressed()) {
			press_pos = mouse_pos = mb->get_position();
			press_shift = mb->is_shift_pressed();
			press_ctrl = mb->is_ctrl_pressed() || mb->is_meta_pressed();
			press_alt = mb->is_alt_pressed();
			if (mb->is_double_click()) {
				_click_select(p_camera, press_pos, press_shift, false, press_alt, true);
				drag = DRAG_NONE;
				return AFTER_GUI_INPUT_STOP;
			}
			drag_axis = press_alt ? -1 : _handle_hit(p_camera, press_pos);
			if (drag_axis < 0 && select_kind == 1) {
				// the circle select tool: selects under the circle while held (Ctrl: deselects)
				drag = DRAG_CIRCLE;
				SelRegion r;
				r.kind = 1;
				r.center = press_pos;
				r.radius = circle_radius;
				_region_select(p_camera, r, press_shift, press_ctrl);
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			if (drag_axis < 0 && select_kind == 2) {
				lasso_pts.clear();
				lasso_pts.push_back(press_pos);
			}
			drag = DRAG_PENDING;
			return AFTER_GUI_INPUT_STOP;
		}
		// release
		if (drag == DRAG_CIRCLE || (drag == DRAG_LASSO && lasso_pts.size() < 3)) {
			drag = DRAG_NONE;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (drag == DRAG_LASSO) {
			SelRegion r;
			r.kind = 2;
			r.poly = lasso_pts;
			_region_select(p_camera, r, press_shift, press_ctrl);
			lasso_pts.clear();
			drag = DRAG_NONE;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (drag == DRAG_PENDING) {
			if (drag_axis < 0) {
				_click_select(p_camera, mb->get_position(), press_shift, press_ctrl, press_alt, false);
			}
		} else if (drag == DRAG_BOX) {
			_box_select(p_camera, Rect2(press_pos, Vector2()).expand(mb->get_position()), press_shift, press_ctrl);
		} else if (drag == DRAG_HANDLE) {
			_finish_handle_drag();
		}
		drag = DRAG_NONE;
		modal_snapped = false;
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}

	if (mm.is_valid()) {
		if (drag == DRAG_CIRCLE) {
			SelRegion r;
			r.kind = 1;
			r.center = mouse_pos;
			r.radius = circle_radius;
			_region_select(p_camera, r, true, press_ctrl); // (adds to what the press selected)
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (drag == DRAG_LASSO) {
			if (lasso_pts.empty() || lasso_pts.back().distance_to(mouse_pos) > 2.0f) {
				lasso_pts.push_back(mouse_pos);
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (drag == DRAG_PENDING && mouse_pos.distance_to(press_pos) > 3.0f) {
			if (drag_axis >= 0) {
				_begin_handle_drag(p_camera, drag_axis, press_shift);
			} else if (select_kind == 2) {
				drag = DRAG_LASSO;
				lasso_pts.push_back(mouse_pos);
			} else {
				drag = DRAG_BOX;
			}
		}
		if (drag == DRAG_HANDLE) {
			if (!mm->is_shift_pressed()) {
				precise_armed = true;
			}
			// Ctrl snaps to geometry (magnet), Shift slows the mouse down.
			mouse_pos = _precise_mouse(_wrap_mouse(p_camera, mm->get_position()), mm->is_shift_pressed() && precise_armed);
			const bool ctrl_held = mm->is_ctrl_pressed() || mm->is_meta_pressed();
			_update_handle_drag(p_camera, mouse_pos, snap_increment_on(ctrl_held), snapping_active(ctrl_held));
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (drag == DRAG_BOX) {
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if ((int64_t)mm->get_button_mask() == 0) {
			update_overlays(); // handle hover (no element hover highlight: only the selection is shown)
		}
		if (select_kind == 1) {
			update_overlays(); // (the circle follows the mouse)
		}
	}
	return AFTER_GUI_INPUT_PASS;
}

// ------------------------------------------------------------------ node creation

Node3D *HewnEditorPlugin::_create_parent() const {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return nullptr;
	}
	TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
	if (sel.size() == 1) {
		Node *s = Object::cast_to<Node>(sel[0]);
		HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(s);
		if (pm && pm->is_csg_model()) {
			return pm; // a new brush of the selected CSG model
		}
		if (pm || Object::cast_to<VisualInstance3D>(s) || Object::cast_to<HewnEntity3D>(s) ||
				Object::cast_to<HewnOverlay3D>(s) || Object::cast_to<AudioStreamPlayer3D>(s)) {
			s = s->get_parent(); // (a sibling: of a brush another brush of its model; meshes, lights, decals, entities hold nothing)
		}
		Node3D *s3 = Object::cast_to<Node3D>(s);
		if (s3 && (s3 == root || root->is_ancestor_of(s3))) {
			return s3;
		}
	}
	return Object::cast_to<Node3D>(root);
}

// A new scene has no root node yet: Godot's own "3D Scene" button in the Scene dock makes one (what a new object
// then goes under). False when there is none and it cannot be made.
bool HewnEditorPlugin::_ensure_scene_root() {
	if (EditorInterface::get_singleton()->get_edited_scene_root()) {
		return true;
	}
	Control *base = EditorInterface::get_singleton()->get_base_control();
	const Ref<Texture2D> n3 = EditorInterface::get_singleton()->get_editor_theme()->get_icon("Node3D", "EditorIcons");
	for (const Variant &v : base->find_children("*", "SceneTreeDock", true, false)) {
		Node *dock_node = Object::cast_to<Node>(v);
		for (const Variant &b : dock_node->find_children("*", "Button", true, false)) {
			Button *bt = Object::cast_to<Button>(b);
			if (bt && bt->is_visible_in_tree() && bt->get_button_icon() == n3) {
				bt->emit_signal("pressed");
				break;
			}
		}
	}
	return EditorInterface::get_singleton()->get_edited_scene_root() != nullptr;
}

void HewnEditorPlugin::_add_node_with_undo(Node3D *p_node, const String &p_action, Node3D *p_parent) {
	_ensure_scene_root();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	Node3D *parent = p_parent ? p_parent : _create_parent();
	if (!root || !parent) {
		if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
			t->push_toast(T("Create a root node in the Scene dock first (3D Scene)"), EditorToaster::SEVERITY_WARNING);
		}
		p_node->queue_free(); // (the caller may still look at it during this call)
		return;
	}
	const Transform3D world = p_node->get_transform();
	if (HewnMesh3D *hm = Object::cast_to<HewnMesh3D>(p_node)) {
		// a new shape collides by default, by what it is: a box a box, a ball a sphere, the rest its triangles
		if (hm->get_shape() != hewn::shapes::CUSTOM && hm->get_collision_mode() == HewnMesh3D::COLLISION_NONE && !hm->is_boolean_cutter() && !hm->is_brush() &&
				(p_action.begins_with("Create") || p_action.begins_with("Add") || p_action.contains("Along") || p_action.begins_with("Belt"))) {
			HewnMesh3D::CollisionMode cm = HewnMesh3D::COLLISION_TRIMESH;
			switch (hm->get_shape()) {
				// (Primitive: exactly its parameters, standing as the shape does; an oval one: its hull)
				case hewn::shapes::CUBE:
					cm = hm->get_shape_param("room") > 0.5f ? HewnMesh3D::COLLISION_TRIMESH : HewnMesh3D::COLLISION_PRIMITIVE;
					break;
				case hewn::shapes::SPHERE:
				case hewn::shapes::CYLINDER:
				case hewn::shapes::CAPSULE:
					cm = HewnMesh3D::COLLISION_PRIMITIVE;
					break;
				case hewn::shapes::ICOSPHERE:
					cm = HewnMesh3D::COLLISION_SPHERE;
					break;
				case hewn::shapes::CONE:
				case hewn::shapes::PRISM:
				case hewn::shapes::WEDGE:
				case hewn::shapes::POLYGON:
					cm = HewnMesh3D::COLLISION_CONVEX;
					break;
				case hewn::shapes::DISC:
					cm = hm->get_shape_param("circle") > 0.5f ? HewnMesh3D::COLLISION_NONE : HewnMesh3D::COLLISION_TRIMESH;
					break;
				default:
					break;
			}
			hm->set_collision_mode(cm);
		}
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action(p_action);
	ur->add_do_method(parent, "add_child", p_node, true);
	ur->add_do_method(p_node, "set_owner", root);
	for (const Variant &v : p_node->find_children("*", "", true, false)) {
		if (Node *c = Object::cast_to<Node>(v)) {
			ur->add_do_method(c, "set_owner", root); // (an Area3D's shape)
		}
	}
	ur->add_do_reference(p_node);
	ur->add_undo_method(parent, "remove_child", p_node);
	ur->commit_action();
	p_node->set_global_transform(world);
	EditorSelection *es = EditorInterface::get_singleton()->get_selection();
	es->clear();
	es->add_node(p_node);
}

void HewnEditorPlugin::sweep_along_selected_path() {
	Path3D *path = nullptr;
	for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_selected_nodes()) {
		path = path ? path : Object::cast_to<Path3D>(v);
	}
	if (!path || path->get_curve().is_null() || path->get_curve()->get_point_count() < 2) {
		ERR_PRINT("Hewn: select a Path3D with at least two points.");
		return;
	}
	HewnMesh3D *node = memnew(HewnMesh3D);
	node->set_name("Sweep");
	node->set_shape(hewn::shapes::SWEEP);
	node->set_shape_param("profile", 1); // (a rectangle)
	const PackedVector3Array pts = path->get_curve()->tessellate(5, 3.0);
	node->set_poly_points(pts);
	node->set_transform(path->get_global_transform());
	if (texture_dock && texture_dock->current_material.is_valid()) {
		node->apply_material(texture_dock->current_material, PackedInt32Array());
	}
	_add_node_with_undo(node, "Create Sweep");
}

void HewnEditorPlugin::create_shape(int p_type) {
	if (p_type == hewn::shapes::POLY_SHAPE) {
		start_poly_tool(false);
		return;
	}
	HewnMesh3D *node = memnew(HewnMesh3D);
	node->set_shape(p_type);
	node->set_name(String(hewn::shapes::type_name((hewn::shapes::Type)p_type)).replace(" ", ""));
	if (texture_dock && texture_dock->current_material.is_valid()) {
		node->apply_material(texture_dock->current_material, PackedInt32Array());
	}
	// New shapes appear at the 3D cursor (Shift+RMB), resting on it.
	Vector3 pos = cursor_pos;
	float min_y = 0.0f;
	for (const Vector3 &p : node->get_data().pos) {
		min_y = MIN(min_y, p.y);
	}
	// turned around the cursor's up axis like the transform orientation
	const Vector3 up = cursor_basis.get_column(1).normalized();
	Vector3 x, z;
	_plane_axes(up, x, z);
	Basis b;
	b.set_column(0, x);
	b.set_column(1, up);
	b.set_column(2, x.cross(up).normalized());
	if (p_type != hewn::shapes::PLANE && p_type != hewn::shapes::DISC) {
		pos -= up * min_y;
	}
	node->set_transform(Transform3D(b, pos));
	_new_object_uv(node);
	_add_node_with_undo(node, String("Create ") + hewn::shapes::type_name((hewn::shapes::Type)p_type));
}

void HewnEditorPlugin::_on_obj_file(const String &p_path) {
	HewnMesh3D *n = get_edited();
	if (n) {
		const Error err = n->export_obj(p_path);
		if (err != OK) {
			ERR_PRINT("Hewn: OBJ export failed: " + p_path);
		}
	}
}

// glTF: the selected meshes (or the edited ones) in one file, each where it is in the world.
void HewnEditorPlugin::_on_gltf_file(const String &p_path) {
	std::vector<HewnMesh3D *> meshes = mode == MODE_OBJECT ? _selected_meshes() : edited_meshes();
	if (meshes.empty() && get_edited()) {
		meshes.push_back(get_edited());
	}
	if (meshes.size() == 1) {
		if (meshes[0]->export_gltf(p_path) != OK) {
			ERR_PRINT("Hewn: glTF export failed: " + p_path);
		}
		return;
	}
	if (meshes.empty()) {
		return;
	}
	Node3D *root = memnew(Node3D);
	root->set_name(p_path.get_file().get_basename());
	for (HewnMesh3D *m : meshes) {
		Ref<ArrayMesh> am = m->get_game_mesh();
		if (am.is_null() || am->get_surface_count() == 0) {
			continue;
		}
		if (m->get_material_override().is_valid()) {
			for (int i = 0; i < am->get_surface_count(); i++) {
				am->surface_set_material(i, m->get_material_override());
			}
		}
		MeshInstance3D *mi = memnew(MeshInstance3D);
		mi->set_name(m->get_name());
		mi->set_mesh(am);
		mi->set_transform(m->get_global_transform());
		root->add_child(mi, true);
		mi->set_owner(root);
	}
	Ref<GLTFDocument> doc;
	doc.instantiate();
	Ref<GLTFState> state;
	state.instantiate();
	Error err = doc->append_from_scene(root, state);
	if (err == OK) {
		err = doc->write_to_filesystem(state, p_path);
	}
	memdelete(root);
	if (err != OK) {
		ERR_PRINT("Hewn: glTF export failed: " + p_path);
	}
}

// ------------------------------------------------------------------ operations

// Extrude in edge / vertex mode: border edges (or selected edges) into faces, loose edges into faces,
// single vertices into loose edges. Returns false when there was nothing to extrude.
bool HewnEditorPlugin::_extrude_edges_verts(hewn::Mesh &m, hewn::Selection &s, float dist) {
	hewn::Topology t;
	t.build(m);
	std::vector<hewn::Edge> edges, wires;
	std::unordered_set<int> vs(s.verts.begin(), s.verts.end());
	if (mode == MODE_VERTEX) {
		for (const hewn::Edge &e : hewn::sel::border_edges(m, t)) {
			if (vs.count(e.a) && vs.count(e.b)) {
				edges.push_back(e);
			}
		}
		for (const std::pair<int, int> &w : m.wires) {
			if (vs.count(w.first) && vs.count(w.second)) {
				wires.push_back(hewn::Edge(w.first, w.second));
			}
		}
	} else {
		for (const hewn::Edge &e : s.edges) {
			(m.has_wire(e.a, e.b) ? wires : edges).push_back(e);
		}
	}
	// single vertices extrude into loose edges, loose edges into faces.
	std::unordered_set<int> covered;
	for (const hewn::Edge &e : edges) {
		covered.insert(e.a);
		covered.insert(e.b);
	}
	for (const hewn::Edge &e : wires) {
		covered.insert(e.a);
		covered.insert(e.b);
	}
	std::vector<int> lone;
	if (mode == MODE_VERTEX) {
		for (int v : s.verts) {
			if (!covered.count(v)) {
				lone.push_back(v);
			}
		}
	}
	std::vector<hewn::Edge> top;
	s.edges = edges.empty() ? std::vector<hewn::Edge>() : hewn::ops::extrude_edges(m, edges, dist, false);
	const std::vector<int> nv = hewn::ops::extrude_wires(m, wires, lone, &top);
	if (dist != 0.0f) {
		for (int v : nv) {
			m.pos[v] += Vector3(0, dist, 0);
		}
	}
	s.edges.insert(s.edges.end(), top.begin(), top.end());
	if (mode == MODE_VERTEX) {
		s.verts = hewn::sel::verts_of_edges(s.edges);
		for (int v : nv) {
			if (std::find(s.verts.begin(), s.verts.end(), v) == s.verts.end()) {
				s.verts.push_back(v);
			}
		}
	}
	return !s.edges.empty() || !s.verts.empty();
}

void HewnEditorPlugin::run_op(const String &p_op) {
	EditorInterface *ei = EditorInterface::get_singleton();
	EditorUndoRedoManager *ur = get_undo_redo();

	if (p_op.begins_with("scene:")) {
		start_scene_op(p_op.substr(6));
		return;
	}
	if (p_op == "physics") {
		start_physics();
		return;
	}
	if (p_op == "curve_edges_poly" || p_op == "curve_edges_bezier" || p_op == "curve_edges_nurbs") {
		mesh_to_curve(p_op == "curve_edges_poly" ? 0 : p_op == "curve_edges_bezier" ? 1 : 2);
		return;
	}
	if (p_op == "phys_apply" || p_op == "phys_cancel") {
		_physics_stop(p_op == "phys_apply");
		return;
	}
	if (p_op == "checker_deselect") {
		checker_deselect();
		return;
	}
	if (p_op == "by_normal") {
		select_by_normal();
		return;
	}
	if (p_op == "knife_project") {
		knife_project();
		return;
	}
	if (p_op == "sweep_edges") {
		sweep_along_edges();
		return;
	}
	if (p_op == "wall_edges") {
		wall_along_edges();
		return;
	}
	if (p_op == "delete_inner") {
		// the faces no one can see (inside the merged solid) of the selected objects / the edited meshes
		const std::vector<HewnMesh3D *> list = mode == MODE_OBJECT ? _selected_meshes() : edited_meshes();
		int total = 0;
		bool any = false;
		for (HewnMesh3D *o : list) {
			const std::vector<int> h = hewn::ops::inner_faces(o->get_data());
			if (h.empty()) {
				continue;
			}
			if (!any) {
				ur->create_action("Hewn: Delete Inner Faces");
				any = true;
			}
			const Dictionary before = o->get_state();
			hewn::Mesh copy = o->get_data();
			hewn::ops::delete_faces(copy, h);
			std::swap(copy, o->get_data());
			o->selection.clear();
			Dictionary after = o->get_state();
			after["shape"] = (int)hewn::shapes::CUSTOM; // (not its parameters' shape any more)
			std::swap(copy, o->get_data());
			ur->add_do_method(o, "set_state", after);
			ur->add_undo_method(o, "set_state", before);
			total += (int)h.size();
		}
		if (any) {
			ur->commit_action();
			selection_changed();
		}
		if (EditorToaster *tt = ei->get_editor_toaster()) {
			tt->push_toast(vformat(T("Inner faces deleted: %d"), total), EditorToaster::SEVERITY_INFO);
		}
		return;
	}
	if (p_op == "bake_light") {
		open_bake_light(); // (its settings first; edit mode: the edited meshes, else the selected objects)
		return;
	}
	if (p_op == "obj:subsurf_modifier") {
		add_subdivision_modifier();
		return;
	}
	if (p_op == "subdivide_faces" || p_op == "catmull_clark") {
		// many faces more (Editor Settings > Hewn: from how many): asked first, nothing done meanwhile; a
		// value dragged in the panel is asked about once the mouse is let go
		heavy_op = p_op;
		const int cuts = (int)op_param(p_op, p_op == "catmull_clark" ? "levels" : "cuts");
		const int64_t limit = subdivide_limit();
		if (!heavy_ok && limit > 0 && _heavy_added(cuts) >= limit) {
			heavy_redo = redo_running;
			heavy_cuts_wanted = cuts;
			if (Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) {
				heavy_wait = true;
			} else {
				callable_mp(this, &HewnEditorPlugin::_heavy_popup).call_deferred();
			}
			return;
		}
		heavy_ok = false;
		heavy_wait = false;
		heavy_prev_cuts = cuts;
	}

	// Dock buttons that only preset a parameter of a redo-able operation.
	if (p_op == "extrude_region" || p_op == "extrude_individual") {
		set_op_param("extrude", "individual", p_op == "extrude_individual");
		run_op("extrude");
		return;
	}
	if (p_op == "face_cutter") {
		start_face_cutter();
		return;
	}
	if (p_op.begins_with("obj:")) {
		_run_object_op(p_op.substr(4));
		return;
	}
	if (p_op == "bevel_profile") {
		edit_bevel_profile();
		return;
	}
	if (p_op == "new_blend_material") {
		open_blend_dialog();
		return;
	}
	if (p_op == "navmesh_bake") {
		open_navmesh_bake();
		return;
	}
	if (p_op == "scrolling_material") {
		// the image: of the selected faces' material (or the mesh's first), else of the texture picked last
		String image;
		auto image_of = [](const Ref<Material> &p_m) {
			if (Ref<BaseMaterial3D> bm = p_m; bm.is_valid() && bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO).is_valid()) {
				return bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO)->get_path();
			}
			if (Ref<ShaderMaterial> sm = p_m; sm.is_valid()) {
				for (const char *pn : { "albedo_texture", "base_texture", "texture_albedo" }) {
					const Ref<Texture2D> t = sm->get_shader_parameter(pn);
					if (t.is_valid()) {
						return t->get_path();
					}
				}
			}
			return String();
		};
		if (HewnMesh3D *sm = get_edited()) {
			int slot = 0;
			if (mode == MODE_FACE && !sm->selection.faces.empty()) {
				slot = sm->get_data().faces[sm->selection.faces[0]].material;
			}
			image = image_of(sm->get_material_for_slot(slot));
		}
		if (image.is_empty() && texture_dock) {
			image = image_of(texture_dock->current_material);
		}
		if (image.is_empty() || !texture_dock) {
			if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
				t->push_toast(T("Scrolling Material: select a mesh with a textured material, or pick a texture first"), EditorToaster::SEVERITY_WARNING);
			}
			return;
		}
		texture_dock->make_scrolling(image);
		return;
	}
	if (p_op == "show_vertex_colors") {
		_toggle_vertex_color_view();
		return;
	}
	if (p_op == "spin_tool") {
		// center: the 3D cursor, axis: the view direction
		Camera3D *cam = _viewport_camera();
		const Vector3 ax = cam ? -cam->get_global_transform().basis.get_column(2) : Vector3(0, 1, 0);
		set_op_param("spin", "cx", cursor_pos.x);
		set_op_param("spin", "cy", cursor_pos.y);
		set_op_param("spin", "cz", cursor_pos.z);
		set_op_param("spin", "ax", ax.x);
		set_op_param("spin", "ay", ax.y);
		set_op_param("spin", "az", ax.z);
		run_op("spin");
		return;
	}
	if (p_op == "recalc_outside" || p_op == "recalc_inside") {
		// one operation, its panel has "Inside"
		set_op_param("recalc_normals", "inside", p_op == "recalc_inside");
		run_op("recalc_normals");
		return;
	}
	if (p_op == "inset_region" || p_op == "inset_individual") {
		set_op_param("inset", "individual", p_op == "inset_individual");
		run_op("inset");
		return;
	}

	// Tools.
	if (p_op == "poly_shape" || p_op == "edit_poly") {
		start_poly_tool(p_op == "edit_poly");
		return;
	}
	if (p_op == "knife") {
		start_knife();
		return;
	}
	if (p_op == "loop_cut_tool") {
		start_loop_cut();
		return;
	}
	if (p_op == "cutter") {
		start_cutter();
		return;
	}
	if (p_op == "clip_tool") {
		start_clip();
		return;
	}
	if (p_op == "bisect_tool") {
		start_bisect();
		return;
	}
	if (p_op == "vertex_paint") {
		start_paint();
		return;
	}
	if (p_op == "ruler") {
		start_ruler();
		return;
	}
	if (p_op == "rip") {
		SubViewport *vp = ei->get_editor_viewport_3d(0);
		if (vp && vp->get_camera_3d()) {
			mouse_pos = Vector2(vp->get_size()) * 0.5f + Vector2(20, 20);
			_start_rip(vp->get_camera_3d());
		}
		return;
	}
	if (p_op == "edit_mode") {
		toggle_edit_mode();
		return;
	}

	// Operations on the editor selection (may not involve an edited Hewn node).
	if (p_op == "scene_size") {
		_scene_size_report();
		return;
	}
	if (p_op.begins_with("curve:")) {
		curve_op(p_op.substr(6));
		return;
	}
	if (p_op == "convert_area") {
		// a trigger volume: an Area3D in its place, with a convex shape of its form
		Node *root = ei->get_edited_scene_root();
		ur->create_action("Hewn: Convert to Area3D");
		bool any = false;
		for (HewnMesh3D *pm : _selected_meshes()) {
			Node *parent = pm->get_parent();
			if (!parent || pm == root) {
				continue;
			}
			Area3D *area = memnew(Area3D);
			area->set_name(pm->get_name());
			area->set_transform(pm->get_transform());
			CollisionShape3D *cs = memnew(CollisionShape3D);
			cs->set_name("CollisionShape3D");
			Ref<ConvexPolygonShape3D> sh;
			sh.instantiate();
			PackedVector3Array pts;
			for (const Vector3 &p : pm->get_data().pos) {
				pts.push_back(p);
			}
			sh->set_points(pts);
			cs->set_shape(sh);
			area->add_child(cs);
			ur->add_do_method(parent, "add_child", area, true);
			ur->add_do_method(area, "set_owner", root);
			ur->add_do_method(cs, "set_owner", root);
			ur->add_do_method(parent, "move_child", area, pm->get_index());
			ur->add_do_method(parent, "remove_child", pm);
			ur->add_do_reference(area);
			ur->add_undo_method(parent, "remove_child", area);
			ur->add_undo_method(parent, "add_child", pm, true);
			ur->add_undo_method(parent, "move_child", pm, pm->get_index());
			ur->add_undo_method(pm, "set_owner", pm->get_owner());
			ur->add_undo_reference(pm);
			any = true;
		}
		ur->commit_action(any);
		return;
	}
	if (p_op == "convert_mesh") {
		TypedArray<Node> sel = ei->get_selection()->get_selected_nodes();
		Node *root = ei->get_edited_scene_root();
		for (int i = 0; i < sel.size(); i++) {
			MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(sel[i]);
			if (!mi || Object::cast_to<HewnMesh3D>(mi) || mi->get_mesh().is_null() || !mi->get_parent()) {
				continue;
			}
			Node *parent = mi->get_parent();
			HewnMesh3D *made_mesh = memnew(HewnMesh3D);
			made_mesh->set_name(mi->get_name());
			made_mesh->create_from_mesh(mi->get_mesh(), true);
			made_mesh->set_transform(mi->get_transform());
			ur->create_action("Hewn: Convert to Hewn Mesh");
			ur->add_do_method(parent, "add_child", made_mesh, true);
			ur->add_do_method(made_mesh, "set_owner", root);
			ur->add_do_method(parent, "move_child", made_mesh, mi->get_index());
			ur->add_do_method(parent, "remove_child", mi);
			ur->add_do_reference(made_mesh);
			ur->add_undo_method(parent, "remove_child", made_mesh);
			ur->add_undo_method(parent, "add_child", mi, true);
			ur->add_undo_method(parent, "move_child", mi, mi->get_index());
			ur->add_undo_method(mi, "set_owner", root);
			ur->add_undo_reference(mi);
			ur->commit_action();
		}
		return;
	}
	if (p_op == "merge_objects") {
		TypedArray<Node> sel = ei->get_selection()->get_selected_nodes();
		std::vector<HewnMesh3D *> list;
		for (int i = 0; i < sel.size(); i++) {
			if (HewnMesh3D *p = Object::cast_to<HewnMesh3D>(sel[i])) {
				list.push_back(p);
			}
		}
		if (list.size() < 2) {
			return;
		}
		HewnMesh3D *target = list[0];
		Node *root = ei->get_edited_scene_root();
		const Dictionary before = target->get_state();
		const TypedArray<Material> mats_before = target->get_materials();
		for (size_t i = 1; i < list.size(); i++) {
			target->merge_from(list[i]);
		}
		ur->create_action("Hewn: Merge Objects");
		ur->add_do_method(target, "set_materials", target->get_materials());
		ur->add_do_method(target, "set_state", target->get_state());
		ur->add_undo_method(target, "set_materials", mats_before);
		ur->add_undo_method(target, "set_state", before);
		for (size_t i = 1; i < list.size(); i++) {
			Node *parent = list[i]->get_parent();
			ur->add_do_method(parent, "remove_child", list[i]);
			ur->add_undo_method(parent, "add_child", list[i], true);
			ur->add_undo_method(parent, "move_child", list[i], list[i]->get_index());
			ur->add_undo_method(list[i], "set_owner", root);
			ur->add_undo_reference(list[i]);
		}
		ur->commit_action();
		ei->get_selection()->clear();
		ei->get_selection()->add_node(target);
		return;
	}
	if (p_op == "boolean_union" || p_op == "boolean_subtract" || p_op == "boolean_intersect") {
		// the last selected (active) mesh is kept, the others are combined into it.
		TypedArray<Node> sel = ei->get_selection()->get_selected_nodes();
		std::vector<HewnMesh3D *> list;
		for (int i = 0; i < sel.size(); i++) {
			if (HewnMesh3D *p = Object::cast_to<HewnMesh3D>(sel[i])) {
				list.push_back(p);
			}
		}
		if (list.size() < 2) {
			ERR_PRINT("Hewn: select two or more Hewn meshes; the last selected one is kept.");
			return;
		}
		// the active one first: the one selected last
		HewnMesh3D *active = sel_order.empty() ? nullptr : Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(sel_order.back()));
		auto it = std::find(list.begin(), list.end(), active);
		if (it != list.end()) {
			std::rotate(list.begin(), it, it + 1);
		} else {
			std::rotate(list.begin(), list.end() - 1, list.end());
		}
		const int op = p_op == "boolean_union" ? HewnMesh3D::BOOLEAN_UNION : p_op == "boolean_subtract" ? HewnMesh3D::BOOLEAN_SUBTRACT : HewnMesh3D::BOOLEAN_INTERSECT;
		HewnMesh3D *target = list[0];
		Node *root = ei->get_edited_scene_root();
		if (dock->boolean_live->is_pressed()) {
			// Non-destructive: the others become live operands (cutters).
			const TypedArray<HewnModifier> m0 = target->get_modifiers();
			TypedArray<HewnModifier> m1 = m0.duplicate();
			for (size_t i = 1; i < list.size(); i++) {
				Ref<HewnBooleanModifier> b;
				b.instantiate();
				b->set_object(target->get_path_to(list[i]));
				b->set_operation(op);
				m1.push_back(b);
			}
			ur->create_action("Hewn: Boolean Modifier");
			ur->add_do_method(target, "set_modifiers", m1);
			ur->add_undo_method(target, "set_modifiers", m0);
			for (size_t i = 1; i < list.size(); i++) {
				ur->add_do_method(list[i], "set_boolean_cutter", true);
				ur->add_undo_method(list[i], "set_boolean_cutter", list[i]->is_boolean_cutter());
			}
			ur->commit_action();
			ei->get_selection()->clear();
			ei->get_selection()->add_node(target);
			return;
		}
		const Dictionary before = target->get_state();
		for (size_t i = 1; i < list.size(); i++) {
			target->boolean_with(list[i], op);
		}
		ur->create_action("Hewn: Boolean");
		ur->add_do_method(target, "set_state", target->get_state());
		ur->add_undo_method(target, "set_state", before);
		for (size_t i = 1; i < list.size(); i++) {
			Node *parent = list[i]->get_parent();
			ur->add_do_method(parent, "remove_child", list[i]);
			ur->add_undo_method(parent, "add_child", list[i], true);
			ur->add_undo_method(parent, "move_child", list[i], list[i]->get_index());
			ur->add_undo_method(list[i], "set_owner", root);
			ur->add_undo_reference(list[i]);
		}
		ur->commit_action();
		ei->get_selection()->clear();
		ei->get_selection()->add_node(target);
		return;
	}

	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	if (!multi_ids.empty() && !multi_running && p_op != "export_gltf" && p_op != "export_obj" && p_op != "uv_editor") {
		// several meshes edited: the operation on each, one undo step
		multi_running = true;
		multi_merge = false;
		const uint64_t keep = edited_id;
		for (HewnMesh3D *o : edited_meshes()) {
			edited_id = o->get_instance_id();
			run_op(p_op);
			multi_merge = true;
		}
		edited_id = keep;
		multi_running = false;
		multi_merge = false;
		for (HewnMesh3D *o : edited_meshes()) {
			o->update_gizmos();
		}
		selection_changed();
		return;
	}
	hewn::Mesh &m = n->get_data();
	hewn::Selection &s = n->selection;
	hewn::Topology t;
	t.build(m);

	// ---- selection (not undoable)
	if (p_op.begins_with("select_") || p_op == "invert" || p_op == "grow" || p_op == "shrink" || p_op == "linked" || p_op == "loop" ||
			p_op == "ring" || p_op == "by_material" || p_op == "by_normal") {
		if (p_op == "select_all") {
			s.verts = hewn::sel::all_verts(m);
			s.edges = hewn::sel::all_edges(m, t);
			s.faces = hewn::sel::all_faces(m);
		} else if (p_op == "select_none") {
			s.clear();
		} else if (p_op == "select_border") {
			std::vector<hewn::Edge> b = hewn::sel::border_edges(m, t);
			if (mode == MODE_VERTEX) {
				s.verts = hewn::sel::verts_of_edges(b);
			} else {
				s.edges = b;
			}
		} else if (p_op == "invert") {
			s.verts = hewn::sel::invert_verts(m, s.verts);
			s.edges = hewn::sel::invert_edges(m, t, s.edges);
			s.faces = hewn::sel::invert_faces(m, s.faces);
		} else if (p_op == "grow") {
			if (mode == MODE_VERTEX) {
				s.verts = hewn::sel::grow_verts(m, t, s.verts);
			} else if (mode == MODE_EDGE) {
				s.edges = hewn::sel::grow_edges(m, t, s.edges);
			} else {
				s.faces = hewn::sel::grow_faces(m, t, s.faces);
			}
		} else if (p_op == "shrink") {
			if (mode == MODE_VERTEX) {
				s.verts = hewn::sel::shrink_verts(m, t, s.verts);
			} else if (mode == MODE_EDGE) {
				s.edges = hewn::sel::shrink_edges(m, t, s.edges);
			} else {
				s.faces = hewn::sel::shrink_faces(m, t, s.faces);
			}
		} else if (p_op == "linked") {
			if (mode == MODE_VERTEX) {
				s.verts = hewn::sel::linked_verts(m, t, s.verts);
			} else if (mode == MODE_EDGE) {
				std::vector<int> v = hewn::sel::linked_verts(m, t, hewn::sel::verts_of_edges(s.edges));
				s.edges = hewn::sel::edges_from_verts(m, t, v);
			} else {
				s.faces = hewn::sel::linked_faces(m, t, s.faces);
			}
		} else if (p_op == "loop" || p_op == "ring") {
			const bool ring = p_op == "ring";
			if (mode == MODE_EDGE) {
				std::vector<hewn::Edge> out;
				for (const hewn::Edge &e : s.edges) {
					std::vector<hewn::Edge> l = ring ? hewn::sel::edge_ring(m, t, e) : hewn::sel::edge_loop(m, t, e);
					out.insert(out.end(), l.begin(), l.end());
				}
				s.edges = hewn::sel::uniq(out);
			} else if (mode == MODE_FACE) {
				s.faces = hewn::sel::face_loops(m, t, s.faces, ring);
			}
		} else if (p_op == "by_material") {
			s.faces = hewn::sel::faces_by_material(m, s.faces);
		} else if (p_op == "select_inner") {
			// the faces no one can see (inside the solid): to look at, then delete
			if (mode != MODE_FACE) {
				set_mode(MODE_FACE);
			}
			s.faces = hewn::ops::inner_faces(m);
		} else if (p_op == "select_invalid_uvs") {
			// faces whose UVs have (almost) no area (the texture is one smeared line or point)
			if (mode != MODE_FACE) {
				set_mode(MODE_FACE);
			}
			std::vector<std::vector<Vector2>> uvs;
			hewn::compute_uvs(m, n->get_global_transform(), uvs);
			s.faces.clear();
			for (int f = 0; f < (int)uvs.size(); f++) {
				double area = 0.0, span = 0.0;
				for (size_t i = 0; i < uvs[f].size(); i++) {
					const Vector2 &a = uvs[f][i], &b = uvs[f][(i + 1) % uvs[f].size()];
					area += (double)a.x * b.y - (double)b.x * a.y;
					span = MAX(span, (double)a.distance_to(b));
				}
				if (std::fabs(area) * 0.5 <= 1e-6 * MAX(1e-6, span * span)) {
					s.faces.push_back(f);
				}
			}
		}
		selection_changed();
		return;
	}

	// ---- object-level operations with their own undo
	if (p_op == "freeze" || p_op == "center_pivot" || p_op == "pivot_to_selection") {
		const Transform3D xf_before = n->get_transform();
		const Dictionary before = n->get_state();
		if (p_op == "freeze") {
			n->freeze_transform();
		} else if (p_op == "center_pivot") {
			n->center_pivot();
		} else {
			std::vector<int> vs = _selected_verts();
			if (vs.empty()) {
				return;
			}
			Vector3 c;
			for (int v : vs) {
				c += m.pos[v];
			}
			n->set_pivot(c / (float)vs.size());
		}
		ur->create_action("Hewn: " + p_op.capitalize(), UndoRedo::MERGE_DISABLE, n);
		ur->add_do_method(n, "set_transform", n->get_transform());
		ur->add_do_method(n, "set_state", n->get_state());
		ur->add_undo_method(n, "set_transform", xf_before);
		ur->add_undo_method(n, "set_state", before);
		ur->commit_action(false);
		selection_changed();
		return;
	}
	if (p_op.begins_with("separate_")) {
		_separate(p_op.substr(9));
		return;
	}
	if (p_op == "apply_modifiers") {
		if (!n->has_modifiers()) {
			return;
		}
		const Dictionary before = n->get_state();
		const TypedArray<HewnModifier> mods = n->get_modifiers();
		Ref<HewnArrayModifier> last = mods.is_empty() ? Ref<HewnArrayModifier>() : Ref<HewnArrayModifier>(mods[mods.size() - 1]);
		Node *parent = n->get_parent();
		Node *root = ei->get_edited_scene_root();
		if (last.is_valid() && last->is_enabled() && !last->get_realize_instances() && parent && n != root) {
			// the array's copies as objects of their own (the modifiers before it applied to each), in its place
			TypedArray<HewnModifier> rest = mods.duplicate();
			rest.remove_at(rest.size() - 1);
			n->set_modifiers(rest);
			const hewn::Mesh base = n->evaluated_mesh();
			n->set_modifiers(mods);
			const std::vector<Transform3D> copies = last->copies(base, n);
			ur->create_action("Hewn: Apply Modifiers (Objects)", UndoRedo::MERGE_DISABLE, root);
			for (size_t k = 0; k < copies.size(); k++) {
				HewnMesh3D *c = memnew(HewnMesh3D);
				c->set_name(String(n->get_name()) + "_" + itos((int64_t)k + 1));
				c->set_shape(hewn::shapes::CUSTOM);
				c->get_data() = base;
				c->set_materials(n->get_materials());
				c->commit(false);
				c->set_transform(n->get_transform() * copies[k]);
				ur->add_do_method(parent, "add_child", c, true);
				ur->add_do_method(c, "set_owner", root);
				ur->add_do_reference(c);
				ur->add_undo_method(parent, "remove_child", c);
			}
			ur->add_do_method(parent, "remove_child", n);
			ur->add_undo_method(parent, "add_child", n, true);
			ur->add_undo_method(parent, "move_child", n, n->get_index());
			ur->add_undo_method(n, "set_owner", n->get_owner());
			ur->add_undo_reference(n);
			ur->commit_action();
			return;
		}
		n->apply_modifiers();
		ur->create_action("Hewn: Apply Modifiers", UndoRedo::MERGE_DISABLE, n);
		ur->add_do_method(n, "set_modifiers", TypedArray<HewnModifier>());
		ur->add_do_method(n, "set_state", n->get_state());
		ur->add_undo_method(n, "set_state", before);
		ur->add_undo_method(n, "set_modifiers", mods);
		ur->commit_action(false);
		selection_changed();
		return;
	}
	if (p_op == "bake") {
		MeshInstance3D *mi = memnew(MeshInstance3D);
		mi->set_name(String(n->get_name()) + "_Baked");
		mi->set_mesh(n->to_array_mesh());
		Node *parent = n->get_parent();
		Node *root = ei->get_edited_scene_root();
		if (!parent || !root) {
			memdelete(mi);
			return;
		}
		mi->set_transform(n->get_transform());
		ur->create_action("Hewn: Bake to MeshInstance3D");
		ur->add_do_method(parent, "add_child", mi, true);
		ur->add_do_method(mi, "set_owner", root);
		ur->add_do_method(n, "set_visible", false);
		ur->add_do_reference(mi);
		ur->add_undo_method(parent, "remove_child", mi);
		ur->add_undo_method(n, "set_visible", n->is_visible());
		ur->commit_action();
		return;
	}
	if (p_op == "export_gltf") {
		gltf_dialog->set_current_file(String(n->get_name()) + ".glb");
		gltf_dialog->popup_file_dialog();
		return;
	}
	if (p_op == "save_mesh_file") {
		// the mesh as it is shown (its modifiers applied) as a mesh resource of its own (.tres / .res / .mesh)
		if (!mesh_file_dialog) {
			mesh_file_dialog = memnew(EditorFileDialog);
			mesh_file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
			mesh_file_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
			mesh_file_dialog->add_filter("*.tres", "Text Resource");
			mesh_file_dialog->add_filter("*.res", "Binary Resource");
			mesh_file_dialog->add_filter("*.mesh", "Mesh");
			mesh_file_dialog->set_title(hw_editor::T("Save Mesh as File"));
			mesh_file_dialog->connect("file_selected", callable_mp(this, &HewnEditorPlugin::_on_mesh_file));
			EditorInterface::get_singleton()->get_base_control()->add_child(mesh_file_dialog);
		}
		mesh_file_node = ObjectID(n->get_instance_id());
		mesh_file_dialog->set_current_file(String(n->get_name()) + ".tres");
		mesh_file_dialog->popup_file_dialog();
		return;
	}
	if (p_op == "export_obj") {
		obj_dialog->set_current_file(String(n->get_name()) + ".obj");
		obj_dialog->popup_file_dialog();
		return;
	}
	if (p_op == "uv_editor") {
		uv_dock->make_visible();
		uv_canvas->frame_selection();
		return;
	}

	// ---- mesh edits (snapshot undo)
	const Mode op_mode = mode;
	begin_edit(p_op == "translate" ? String("Move") : p_op.capitalize());
	bool geometry = true;
	auto P = [&](const char *key) { return op_param(p_op, key); };
	const Transform3D g = n->get_global_transform();
	const Transform3D ginv = g.affine_inverse();
	// Moves the selected vertices by a world-space function.
	auto move_selected = [&](const std::function<Vector3(const Vector3 &)> &fn) {
		for (int v : _selected_verts()) {
			m.pos[v] = ginv.xform(fn(g.xform(m.pos[v])));
		}
	};
	// Extrude of the selection in the current mode (faces, edges, or border edges between vertices).
	auto extrude_selection = [&](float dist, bool individual) {
		const int nf0_extrude = (int)m.faces.size();
		if (mode == MODE_FACE) {
			hewn::ops::extrude_faces(m, s.faces, dist, individual);
		} else {
			_extrude_edges_verts(m, s, dist);
		}
		if ((bool)P("flip_normals")) {
			std::vector<int> fl;
			for (int f = nf0_extrude; f < (int)m.faces.size(); f++) {
				fl.push_back(f);
			}
			if (mode == MODE_FACE) {
				fl.insert(fl.end(), s.faces.begin(), s.faces.end());
			}
			hewn::ops::flip_faces(m, hewn::sel::uniq(fl));
		}
	};
	// "dissolve orthogonal edges": side faces that continue a neighbor's plane are merged into it.
	auto dissolve_orthogonal = [&](int first_new_face) {
		hewn::ops::dissolve_orthogonal(m, first_new_face, s.faces);
	};

	if (p_op == "spin") {
		// Spin: the selection extruded step by step around an axis (lathe, arches, rings)
		const int steps = MAX(1, (int)P("steps"));
		const float total = Math::deg_to_rad((float)P("angle"));
		const Vector3 c = ginv.xform(Vector3((float)P("cx"), (float)P("cy"), (float)P("cz")));
		Vector3 axis = ginv.basis.xform(Vector3((float)P("ax"), (float)P("ay"), (float)P("az")));
		if (axis.length() < 1e-6f) {
			axis = Vector3(0, 1, 0);
		}
		axis.normalize();
		const Basis turn(axis, total / (float)steps);
		const bool full = std::fabs(std::fabs(total) - (float)(Math::PI * 2.0)) < 1e-3f;
		for (int k = 0; k < steps; k++) {
			extrude_selection(0.0f, false);
			for (int v : _selected_verts()) {
				m.pos[v] = c + turn.xform(m.pos[v] - c);
			}
		}
		if (full) {
			// round to the start: the last ring is the first one (and faces there would be inside)
			if (mode == MODE_FACE) {
				hewn::ops::delete_faces(m, s.faces);
				s.clear();
			}
			hewn::weld_by_distance(m, 1e-4f);
			s.clear();
		}
	} else if (p_op == "extrude") {
		const int nf0 = (int)m.faces.size();
		extrude_selection((float)P("distance"), (bool)P("individual") && mode == MODE_FACE);
		if ((bool)P("dissolve_orthogonal") && mode == MODE_FACE) {
			dissolve_orthogonal(nf0);
		}
	} else if (p_op == "extrude_repeat") {
		// the selection extruded again and again, each step moved along its normal and scaled about its center
		const int steps = MAX(1, (int)P("steps"));
		const float dist = (float)P("distance");
		const float sc = (float)P("scale");
		const std::vector<int> nf = mode == MODE_FACE ? s.faces : hewn::sel::faces_from_verts(m, _selected_verts(), false);
		Vector3 nrm;
		for (int f : nf) {
			if (f >= 0 && f < (int)m.faces.size()) {
				nrm += hewn::face_normal_raw(m, m.faces[f]);
			}
		}
		nrm = g.basis.inverse().transposed().xform(nrm);
		nrm = nrm.length() > 1e-9f ? nrm.normalized() : Vector3(0, 1, 0);
		for (int k = 0; k < steps; k++) {
			extrude_selection(0.0f, false);
			const std::vector<int> vs = _selected_verts();
			Vector3 c;
			for (int v : vs) {
				c += g.xform(m.pos[v]);
			}
			c = vs.empty() ? c : c / (float)vs.size();
			for (int v : vs) {
				const Vector3 p = g.xform(m.pos[v]);
				m.pos[v] = ginv.xform(c + (p - c) * sc + nrm * dist);
			}
		}
	} else if (p_op == "extrude_block") {
		// a closed block of its own on the selected faces, a step long:
		// its bottom on them, its own vertices - moving it leaves the mesh it came from as it is
		const Vector3 d = g.basis.inverse().xform(Vector3((float)P("x"), (float)P("y"), (float)P("z")));
		std::vector<int> region;
		for (int f : s.faces) {
			if (f >= 0 && f < (int)m.faces.size()) {
				region.push_back(f);
			}
		}
		if (!region.empty() && d.length() > 1e-6f) {
			const std::vector<int> new_top = hw_make_block(m, region, d);
			s.clear();
			for (int k = 0; k < (int)new_top.size(); k++) {
				s.faces.push_back(new_top[k]);
			}
		}
	} else if (p_op == "bisect") {
		std::vector<int> faces = mode == MODE_FACE ? s.faces : hewn::sel::faces_from_verts(m, _selected_verts(), true);
		const Vector3 wp((float)P("px"), (float)P("py"), (float)P("pz"));
		const Vector3 wn((float)P("nx"), (float)P("ny"), (float)P("nz"));
		// Mode: split only; split and fill (two closed parts, each with a face over the cut); the part above (the
		// normal's side) / below removed, the hole filled or left open
		const int bmode = (int)P("mode");
		const Vector3 lp = ginv.xform(wp), ln = g.basis.transposed().xform(wn);
		std::vector<hewn::Edge> cut;
		bisect_sep_faces.clear();
		if (bmode == 1) {
			// (both parts closed: the hole filled on each side; the one below separated after, if asked)
			hewn::Mesh below = m;
			std::vector<hewn::Edge> c1 = hewn::ops::bisect(m, faces, lp, ln, false, true, true);
			std::vector<hewn::Edge> c2 = hewn::ops::bisect(below, faces, lp, ln, true, false, true);
			const int base = (int)m.pos.size();
			const int fbase = (int)m.faces.size();
			hewn::ops::append(m, below, Transform3D(), 0);
			if ((bool)P("separate") && fbase > 0 && (int)m.faces.size() > fbase) {
				for (int f = fbase; f < (int)m.faces.size(); f++) {
					bisect_sep_faces.push_back(f);
				}
			}
			cut = c1;
			for (const hewn::Edge &e : c2) {
				cut.push_back(hewn::Edge(e.a + base, e.b + base));
			}
		} else {
			cut = hewn::ops::bisect(m, faces, lp, ln, bmode == 3, bmode == 2, bmode >= 2 && (bool)P("fill"));
		}
		s.clear();
		// (a side cleared: its corners go too, none of the faces left uses them)
		s.edges = cut;
		hewn::remove_unused_vertices(m, &s);
		cut = s.edges;
		s.clear();
		if (mode == MODE_VERTEX) {
			s.verts = hewn::sel::verts_of_edges(cut);
		} else {
			s.edges = cut;
			if (mode == MODE_FACE) {
				mode = element_mode = MODE_EDGE;
			}
		}
	} else if (p_op == "extrude_normals") {
		s.faces = hewn::ops::extrude_along_normals(m, s.faces, (float)P("distance"));
	} else if (p_op == "extrude_move" || p_op == "duplicate_move" || p_op == "translate" || p_op == "rotate" || p_op == "scale") {
		const int nf0 = (int)m.faces.size();
		if (p_op == "extrude_move") {
			extrude_selection(0.0f, false);
		} else if (p_op == "duplicate_move") {
			s.faces = hewn::ops::duplicate_faces(m, s.faces);
		}
		// Selection plus proportional-editing neighbors, each with its weight and pivot.
		const std::vector<int> sel = _selected_verts();
		const bool xf_op = p_op == "translate" || p_op == "rotate" || p_op == "scale";
		std::vector<int> verts;
		std::vector<float> w;
		// (Projected (2D): along the view the operation was made in)
		const Vector3 view((float)P("prop_vx"), (float)P("prop_vy"), (float)P("prop_vz"));
		prop_proj_axis = (bool)P("prop_projected") && view.length() > 0.5f ? view.normalized() : Vector3();
		_weighted_verts(sel, xf_op && (bool)P("prop"), (float)P("prop_size"), (int)P("prop_falloff"), (bool)P("prop_connected"), verts, w);
		const Vector3 piv((float)P("px"), (float)P("py"), (float)P("pz"));
		std::vector<Vector3> pivots(verts.size(), piv);
		if (xf_op && (int)P("pivot_mode") == HewnDock::PIVOT_INDIVIDUAL) {
			const std::vector<Vector3> ind = _individual_pivots(sel, piv);
			for (size_t i = 0; i < ind.size(); i++) {
				pivots[i] = ind[i];
			}
		}
		const Vector3 d((float)P("x"), (float)P("y"), (float)P("z"));
		Vector3 axis((float)P("ax"), (float)P("ay"), (float)P("az"));
		if (axis.length() < 1e-6f) {
			axis = Vector3(0, 1, 0);
		}
		axis.normalize();
		const float f = (float)P("factor");
		const Vector3 sd = Vector3((float)P("dx"), (float)P("dy"), (float)P("dz")).normalized();
		const int c = (int)P("constraint");
		std::vector<Vector3> before;
		for (int v : verts) {
			before.push_back(m.pos[v]);
		}
		for (size_t i = 0; i < verts.size(); i++) {
			const Vector3 p = g.xform(m.pos[verts[i]]);
			Vector3 q = p;
			if (p_op == "rotate") {
				q = pivots[i] + Basis(axis, (float)P("angle") * w[i] * (float)Math::PI / 180.0f).xform(p - pivots[i]);
			} else if (p_op == "scale") {
				const float fi = 1.0f + (f - 1.0f) * w[i];
				const Vector3 r = p - pivots[i];
				q = c == 1 ? pivots[i] + r + sd * (r.dot(sd) * (fi - 1.0f)) : c == 2 ? pivots[i] + r * fi - sd * (r.dot(sd) * (fi - 1.0f)) : pivots[i] + r * fi;
			} else {
				q = p + d * w[i];
			}
			m.pos[verts[i]] = ginv.xform(q);
		}
		n->apply_mirror_clipping(verts, before); // Mirror modifier with Clipping
		if (p_op == "extrude_move" && (bool)P("dissolve_orthogonal") && mode == MODE_FACE) {
			dissolve_orthogonal(nf0);
		}
		_auto_merge(m, sel);
	} else if (p_op == "inset") {
		const float amount = (float)P("amount");
		if (amount > 0.0f || (float)P("depth") != 0.0f) {
			s.faces = hewn::ops::inset_faces_ex(m, s.faces, _inset_options(amount));
			s.verts = hewn::sel::verts_of_faces(m, s.faces);
		}
	} else if (p_op == "loop_cut") {
		const hewn::Edge e((int)P("a"), (int)P("b"));
		s.clear();
		s.edges = hewn::ops::loop_cut(m, e, (int)P("cuts"), ((float)P("factor") + 1.0f) * 0.5f);
		mode = element_mode = MODE_EDGE;
	} else if (p_op == "set_vertex_color") {
		// the selected vertices' color in a channel; in face mode, only on the side of the selected faces (the
		// neighbor faces keep theirs at the same vertices: a sharp color edge)
		geometry = false;
		const Color c = P("color");
		const int ch = CLAMP((int)P("channel"), 0, 4);
		const bool faces_only = (bool)P("faces_only") && has_mode(MODE_FACE) && !s.faces.empty();
		if (ch > 0 && n->custom_formats[ch - 1] < 0) {
			n->custom_formats[ch - 1] = dock && dock->paint_format ? dock->paint_format->get_selected() : 0;
		}
		std::unordered_set<int> vs;
		for (int v : _selected_verts()) {
			vs.insert(v);
		}
		std::unordered_set<int> fs(s.faces.begin(), s.faces.end());
		for (int f = 0; f < (int)m.faces.size(); f++) {
			hewn::Face &F = m.faces[f];
			if (faces_only && !fs.count(f)) {
				continue;
			}
			for (int i = 0; i < F.size(); i++) {
				if (vs.count(F.v[i])) {
					_paint_value(F, i, ch) = c;
				}
			}
		}
	} else if (p_op == "limited_dissolve") {
		hewn::ops::limited_dissolve(m, mode == MODE_FACE ? s.faces : std::vector<int>(), (float)P("angle"));
		s.clear();
	} else if (p_op == "dissolve") {
		if (mode == MODE_FACE) {
			s.faces = hewn::ops::merge_faces(m, s.faces);
		} else if (mode == MODE_EDGE) {
			s.faces = hewn::ops::dissolve_edges(m, s.edges);
			s.edges.clear();
		} else {
			hewn::ops::dissolve_vertices(m, s.verts);
			s.verts.clear();
		}
	} else if (p_op == "bevel_vertices" || p_op == "offset_edge_slide" || p_op == "push_pull" || p_op == "shrink_fatten" || p_op == "shear") {
		_value_op_apply(p_op, (float)P("value"));
	} else if (p_op == "poke") {
		s.faces = hewn::ops::poke_faces(m, s.faces);
	} else if (p_op == "bridge_faces") {
		s.faces = hewn::ops::bridge_faces(m, s.faces);
	} else if (p_op == "tris_to_quads") {
		std::vector<int> faces = mode == MODE_FACE ? s.faces : hewn::sel::faces_from_verts(m, _selected_verts(), true);
		hewn::ops::quadify(m, 40.0f, faces.empty() ? nullptr : &faces);
		s.clear();
	} else if (p_op == "split_edges") {
		hewn::ops::split_edges(m, mode == MODE_EDGE ? s.edges : hewn::sel::edges_from_verts(m, t, _selected_verts()));
	} else if (p_op == "mark_seam" || p_op == "clear_seam" || p_op == "mark_sharp" || p_op == "clear_sharp" || p_op == "edge_crease" || p_op == "bevel_weight" ||
			p_op == "sharp_by_angle") {
		std::vector<hewn::Edge> edges = mode == MODE_EDGE ? s.edges : hewn::sel::edges_from_verts(m, t, _selected_verts());
		if ((p_op == "mark_sharp" || p_op == "clear_sharp" || p_op == "mark_seam" || p_op == "clear_seam") && (bool)P("vertices")) {
			// Vertices: every edge touching a selected vertex, not only the ones between them
			std::unordered_set<int> vs;
			for (int v : _selected_verts()) {
				vs.insert(v);
			}
			edges.clear();
			for (const hewn::Edge &e : t.edges) {
				if (vs.count(e.a) || vs.count(e.b)) {
					edges.push_back(e);
				}
			}
		}
		const float cos_lim = std::cos((float)P("angle") * (float)Math::PI / 180.0f);
		const bool extend = p_op == "sharp_by_angle" && (bool)P("extend");
		for (const hewn::Edge &e : edges) {
			hewn::EdgeAttr &ea = m.edge_attrs[hewn::ekey(e)];
			if (p_op == "mark_seam" || p_op == "clear_seam") {
				ea.seam = p_op == "mark_seam";
			} else if (p_op == "mark_sharp" || p_op == "clear_sharp") {
				ea.sharp = p_op == "mark_sharp";
			} else if (p_op == "edge_crease") {
				ea.crease = CLAMP((float)P("value"), 0.0f, 1.0f);
			} else if (p_op == "bevel_weight") {
				ea.bevel_weight = CLAMP((float)P("value"), 0.0f, 1.0f);
			} else {
				const int id = t.find_edge(e.a, e.b);
				ea.sharp = (extend && ea.sharp) || (id >= 0 && t.edge_faces[id].size() == 2 &&
						hewn::face_normal(m, m.faces[t.edge_faces[id][0]]).dot(hewn::face_normal(m, m.faces[t.edge_faces[id][1]])) < cos_lim);
			}
		}
		geometry = false;
	} else if (p_op == "vertex_crease") {
		for (int v : _selected_verts()) {
			m.vert_crease[v] = CLAMP((float)P("value"), 0.0f, 1.0f);
		}
		geometry = false;
	} else if (p_op == "to_circle") {
		hewn::ops::to_circle(m, _selected_verts(), mode == MODE_FACE ? s.faces : std::vector<int>(), (bool)P("regular"), (float)P("influence") / 100.0f);
	} else if (p_op == "randomize") {
		Ref<RandomNumberGenerator> rng;
		rng.instantiate();
		rng->set_seed((uint64_t)(int64_t)P("seed") * 7919u + 17u);
		const float amount = (float)P("amount");
		for (int v : _selected_verts()) {
			const Vector3 d(rng->randf_range(-1, 1), rng->randf_range(-1, 1), rng->randf_range(-1, 1));
			m.pos[v] = ginv.xform(g.xform(m.pos[v]) + d * amount);
		}
	} else if (p_op.begins_with("mirror_")) {
		// mirror the selection around the pivot, along a global or local axis
		const bool local = p_op.length() == 9;
		const int axis = p_op[p_op.length() - 1] - 'x';
		Vector3 ax = local ? g.basis.get_column(axis).normalized() : AXES[axis];
		const std::vector<int> sel = _selected_verts();
		const Vector3 piv = _pivot_point(dock->pivot_mode, sel);
		for (int v : sel) {
			const Vector3 w = g.xform(m.pos[v]);
			m.pos[v] = ginv.xform(w - ax * (2.0f * (w - piv).dot(ax)));
		}
		std::unordered_set<int> vs(sel.begin(), sel.end());
		for (hewn::Face &F : m.faces) {
			bool all = true;
			for (int v : F.v) {
				all = all && vs.count(v);
			}
			if (all) {
				F.reverse(); // mirrored faces keep facing outwards
			}
		}
	} else if (p_op.begins_with("merge_")) {
		std::vector<int> sel = mode == MODE_VERTEX ? s.verts : _selected_verts();
		if (sel.size() >= 2) {
			if (p_op == "merge_collapse") {
				std::vector<std::vector<int>> groups;
				hewn::Topology tt;
				tt.build(m);
				std::unordered_set<int> vs(sel.begin(), sel.end());
				std::unordered_map<int, int> comp;
				for (int v : sel) {
					if (comp.count(v)) {
						continue;
					}
					groups.push_back({});
					std::vector<int> stack = { v };
					comp[v] = (int)groups.size() - 1;
					while (!stack.empty()) {
						const int x = stack.back();
						stack.pop_back();
						groups.back().push_back(x);
						for (int e : tt.vert_edges[x]) {
							const int o = tt.edges[e].a == x ? tt.edges[e].b : tt.edges[e].a;
							if (vs.count(o) && !comp.count(o)) {
								comp[o] = (int)groups.size() - 1;
								stack.push_back(o);
							}
						}
					}
				}
				s.verts = hewn::ops::collapse_groups(m, groups);
			} else {
				Vector3 target;
				if (p_op == "merge_cursor") {
					target = ginv.xform(cursor_pos);
				} else if (p_op == "merge_first") {
					target = m.pos[sel.front()];
				} else if (p_op == "merge_last") {
					target = m.pos[sel.back()];
				} else {
					for (int v : sel) {
						target += m.pos[v];
					}
					target /= (float)sel.size();
				}
				for (int v : sel) {
					m.pos[v] = target;
				}
				s.verts = hewn::ops::collapse_groups(m, { sel });
			}
			s.edges.clear();
			s.faces.clear();
		}
	} else if (p_op == "delete_verts" || p_op == "delete_edges" || p_op == "delete_faces" || p_op == "delete_only_edges_faces" || p_op == "delete_only_faces") {
		const hewn::ops::DeleteKind kind = p_op == "delete_verts" ? hewn::ops::DEL_VERTS : p_op == "delete_edges" ? hewn::ops::DEL_EDGES
				: p_op == "delete_faces"																	 ? hewn::ops::DEL_FACES
				: p_op == "delete_only_faces"																 ? hewn::ops::DEL_ONLY_FACES
																											 : hewn::ops::DEL_ONLY_EDGES_FACES;
		_delete_selected(m, kind);
		s.clear();
	} else if (p_op == "dissolve_verts") {
		hewn::ops::dissolve_vertices(m, _selected_verts());
		s.clear();
	} else if (p_op == "dissolve_edges") {
		s.faces = hewn::ops::dissolve_edges(m, mode == MODE_EDGE ? s.edges : hewn::sel::edges_from_verts(m, t, _selected_verts()));
		s.edges.clear();
	} else if (p_op == "dissolve_faces") {
		s.faces = hewn::ops::merge_faces(m, mode == MODE_FACE ? s.faces : hewn::sel::faces_from_verts(m, _selected_verts(), true));
	} else if (p_op == "collapse_edges_faces") {
		set_op_param("merge_collapse", "x", 0); // (no parameters)
		const std::vector<int> sel = _selected_verts();
		std::vector<std::vector<int>> groups;
		hewn::Topology tt;
		tt.build(m);
		std::unordered_set<int> vs(sel.begin(), sel.end());
		std::unordered_set<int> seen;
		for (int v : sel) {
			if (seen.count(v)) {
				continue;
			}
			groups.push_back({});
			std::vector<int> stack = { v };
			seen.insert(v);
			while (!stack.empty()) {
				const int x = stack.back();
				stack.pop_back();
				groups.back().push_back(x);
				for (int e : tt.vert_edges[x]) {
					const int o = tt.edges[e].a == x ? tt.edges[e].b : tt.edges[e].a;
					if (vs.count(o) && !seen.count(o)) {
						seen.insert(o);
						stack.push_back(o);
					}
				}
			}
		}
		s.verts = hewn::ops::collapse_groups(m, groups);
		s.edges.clear();
		s.faces.clear();
	} else if (p_op == "delete_edge_loops") {
		hewn::ops::delete_edge_loops(m, mode == MODE_EDGE ? s.edges : hewn::sel::edges_from_verts(m, t, _selected_verts()));
		s.clear();
	} else if (p_op == "delete") {
		_delete_selected(m, mode == MODE_FACE ? hewn::ops::DEL_FACES : mode == MODE_EDGE ? hewn::ops::DEL_EDGES : hewn::ops::DEL_VERTS);
		s.clear();
	} else if (p_op == "flip" || p_op == "recalc_normals") {
		// Selected faces (any mode); nothing selected means the whole mesh.
		std::vector<int> faces = mode == MODE_FACE ? s.faces : hewn::sel::faces_from_verts(m, _selected_verts(), true);
		if (faces.empty()) {
			faces = hewn::sel::all_faces(m);
		}
		if (p_op == "flip") {
			hewn::ops::flip_faces(m, faces);
		} else {
			hewn::ops::recalc_normals(m, faces, (bool)op_param("recalc_normals", "inside"));
		}
	} else if (p_op == "make_face") {
		const std::vector<int> fv = mode == MODE_EDGE ? hewn::sel::verts_of_edges(s.edges) : hewn::sel::uniq(_selected_verts());
		f2_new_vertex = -1;
		int f2 = -1;
		hewn::Edge next(-1, -1);
		// F2 picks by the mouse: the view in the mesh's space
		hewn::ops::F2View view;
		if (Camera3D *cam = _viewport_camera()) {
			const Transform3D G = n->get_global_transform();
			const Transform3D Gi = G.affine_inverse();
			view.to_screen = [cam, G](const Vector3 &p) { return cam->unproject_position(G.xform(p)); };
			view.mouse = mouse_pos; // (where the mouse was over the viewport last)
			view.eye = Gi.xform(cam->get_global_position());
			view.has_eye = true;
		}
		if (fv.size() == 1 && mode == MODE_VERTEX) {
			// F2: a border vertex -> a new vertex and a quad (moved right away, as F2's autograb)
			int q = -1;
			f2 = hewn::ops::f2_fill_vertex(m, fv[0], view, &q);
			if (f2 >= 0) {
				s.verts.clear();
				if (q >= 0) {
					s.verts.push_back(q);
					f2_new_vertex = q;
				} else {
					s.verts = m.faces[f2].v;
				}
			}
		} else if (fv.size() == 2) {
			// F2: one open edge -> a quad along the open edges at its ends; the edge across selected (F goes on)
			f2 = hewn::ops::f2_fill_edge(m, fv[0], fv[1], view, &next);
		}
		if (f2 >= 0 && fv.size() == 2) {
			s.edges.clear();
			s.verts.clear();
			if (next.a >= 0) {
				s.edges.push_back(next);
				s.verts = { next.a, next.b };
			}
		}
		if (f2 >= 0) {
			// (done)
		} else if (fv.size() == 2) {
			// F on two vertices joins them with an edge
			if (!hewn::ops::add_wire(m, fv[0], fv[1])) {
				editing = false;
				return;
			}
			if (has_mode(MODE_EDGE)) {
				s.edges.push_back(hewn::Edge(fv[0], fv[1]));
			}
		} else {
			const int f = hewn::ops::make_face_from(m, fv);
			if (f < 0) {
				editing = false;
				return;
			}
			if (mode == MODE_EDGE) {
				s.edges = hewn::sel::edges_of_faces(m, { f });
			}
		}
	} else if (p_op == "subdivide_faces") {
		s.faces = hewn::ops::subdivide_faces_ex(m, s.faces, (int)P("cuts"), (float)P("smoothness"), (bool)P("ngons"), (float)P("fractal"), (float)P("along_normal"), (int)P("seed"));
	} else if (p_op == "triangulate") {
		s.faces = hewn::ops::triangulate_faces(m, s.faces);
	} else if (p_op == "merge_faces") {
		s.faces = hewn::ops::merge_faces(m, s.faces);
	} else if (p_op == "detach") {
		hewn::ops::detach_faces(m, s.faces);
	} else if (p_op == "duplicate") {
		s.faces = hewn::ops::duplicate_faces(m, s.faces);
	} else if (p_op == "conform") {
		hewn::ops::conform_normals(m, s.faces);
	} else if (p_op == "smooth_faces" || p_op == "flat_faces") {
		int g = 0;
		if (p_op == "smooth_faces") {
			for (const hewn::Face &F : m.faces) {
				g = MAX(g, F.smooth);
			}
			g++;
		}
		// (vertex / edge mode: the faces whose corners are all selected, else the faces around the selection)
		std::vector<int> fs = s.faces;
		if (mode != MODE_FACE) {
			const std::vector<int> vs = mode == MODE_EDGE ? hewn::sel::verts_of_edges(s.edges) : s.verts;
			fs = hewn::sel::faces_from_verts(m, vs, true);
			if (fs.empty()) {
				fs = hewn::sel::faces_from_verts(m, vs, false);
			}
		}
		for (int f : fs) {
			if (f >= 0 && f < (int)m.faces.size()) {
				m.faces[f].smooth = g;
			}
		}
		geometry = false;
	} else if (p_op == "bevel") {
		std::vector<hewn::Edge> edges = mode == MODE_EDGE ? s.edges : std::vector<hewn::Edge>();
		if (mode == MODE_FACE) {
			// Boundary of the selected region (edges with exactly one selected face).
			std::unordered_set<int> fs(s.faces.begin(), s.faces.end());
			for (size_t e = 0; e < t.edges.size(); e++) {
				int count = 0;
				for (int f : t.edge_faces[e]) {
					count += fs.count(f) ? 1 : 0;
				}
				if (count == 1 && t.edge_faces[e].size() == 2) {
					edges.push_back(t.edges[e]);
				}
			}
		} else if (mode == MODE_VERTEX) {
			edges = hewn::sel::edges_from_verts(m, t, s.verts);
		}
		// the width width types: along the faces (offset), across the bevel (width), in from the
		// edge (depth) or a share of the edges it slides along (percent); the angle is the mean of the edges'
		const int wtype = (int)P("width_type");
		float amount = (float)P("width");
		if (wtype == 1 || wtype == 2) {
			float half = 0.0f;
			int count = 0;
			for (const hewn::Edge &e : edges) {
				const int ei = t.find_edge(e.a, e.b);
				if (ei >= 0 && t.edge_faces[ei].size() == 2) {
					const Vector3 n0 = hewn::face_normal(m, m.faces[t.edge_faces[ei][0]]), n1 = hewn::face_normal(m, m.faces[t.edge_faces[ei][1]]);
					half += 0.5f * ((float)Math::PI - std::acos(CLAMP(n0.dot(n1), -1.0f, 1.0f)));
					count++;
				}
			}
			half = count ? half / count : (float)Math::PI * 0.25f;
			amount = wtype == 1 ? amount / MAX(1e-4f, 2.0f * std::sin(half)) : amount / MAX(1e-4f, std::cos(half));
		}
		if ((int)P("affect") == 0) {
			// Affect: Vertices (the corners cut off)
			std::vector<int> vs = mode == MODE_VERTEX ? s.verts : _selected_verts();
			s.faces = hewn::ops::bevel_vertices(m, vs, amount);
			hewn::continue_uvs(m, s.faces, g); // (the texture runs on over the new faces)
			s.verts.clear();
		} else {
			const std::vector<Vector2> prof = (int)P("profile_type") == 1 ? _bevel_profile()->to_vector() : std::vector<Vector2>();
			s.faces = hewn::ops::bevel_edges(m, edges, amount, (int)P("segments"), false, prof.empty() ? nullptr : &prof, wtype == 3);
			hewn::continue_uvs(m, s.faces, g); // (the texture of the faces around runs on over the bevel, no jumps)
			if ((bool)P("harden_normals")) {
				hewn::ops::harden_normals(m, s.faces);
			}
		}
		s.edges.clear();
		if (!s.faces.empty() && mode != MODE_FACE) {
			mode = element_mode = MODE_FACE;
		}
	} else if (p_op == "arch") {
		s.edges = hewn::ops::arch_edges(m, s.edges, (int)P("segments"), (float)P("rise"));
		s.faces.clear();
		s.verts.clear();
	} else if (p_op == "connect_edges") {
		s.edges = hewn::ops::connect_edges(m, s.edges);
	} else if (p_op == "insert_loop") {
		s.edges = hewn::ops::insert_edge_loops(m, s.edges);
	} else if (p_op == "subdivide_edges") {
		s.edges = hewn::ops::subdivide_edges(m, mode == MODE_EDGE ? s.edges : hewn::sel::edges_from_verts(m, t, _selected_verts()), (int)P("cuts"), (bool)P("ngons"));
	} else if (p_op == "bridge") {
		if (s.edges.size() == 2) {
			s.faces = hewn::ops::bridge_edges(m, s.edges[0], s.edges[1]);
		}
	} else if (p_op == "dissolve") {
		if (mode == MODE_EDGE) {
			s.faces = hewn::ops::dissolve_edges(m, s.edges);
			s.edges.clear();
		} else if (mode == MODE_VERTEX) {
			hewn::ops::dissolve_vertices(m, s.verts);
			s.verts.clear();
		}
	} else if (p_op == "flip_edges") {
		hewn::ops::flip_edges(m, s.edges);
		s.edges.clear();
	} else if (p_op == "weld") {
		s.verts = hewn::ops::weld_vertices(m, s.verts, (float)P("distance"));
	} else if (p_op == "collapse") {
		s.verts = hewn::ops::collapse_groups(m, { _selected_verts() });
		if (mode != MODE_VERTEX) {
			s.clear();
		}
	} else if (p_op == "split") {
		s.verts = hewn::ops::split_vertices(m, s.verts);
	} else if (p_op == "connect_vertices") {
		s.edges = hewn::ops::connect_vertices(m, s.verts);
	} else if (p_op == "fill_holes") {
		s.faces = hewn::ops::fill_holes(m, mode == MODE_VERTEX ? s.verts : mode == MODE_EDGE ? hewn::sel::verts_of_edges(s.edges) : std::vector<int>());
	} else if (p_op == "catmull_clark") {
		hewn::ops::SubdivOptions so;
		so.levels = (int)P("levels");
		so.simple = (int)P("kind") == 1;
		so.creases = (bool)P("creases");
		so.keep_corners = (int)P("boundary") == 1;
		so.uv_smooth = (int)P("uv_smooth");
		hewn::ops::subdivide_smooth(m, so);
		s.clear();
	} else if (p_op == "auto_smooth") {
		hewn::ops::auto_smooth(m, (float)P("angle"), mode == MODE_FACE ? s.faces : std::vector<int>());
		geometry = false;
	} else if (p_op == "quadify") {
		hewn::ops::quadify(m, (float)P("angle"));
		s.clear();
	} else if (p_op == "conform_all") {
		hewn::ops::conform_normals(m, {});
	} else if (p_op == "triangulate_all") {
		hewn::ops::triangulate_faces(m, hewn::sel::all_faces(m));
		s.clear();
	} else if (p_op == "flip_all") {
		hewn::ops::flip_faces(m, hewn::sel::all_faces(m));
	} else if (p_op == "uv_apply") {
		dock->apply_uv_params();
		geometry = false;
	} else if (p_op == "uv_group" || p_op == "uv_ungroup") {
		int g = 0;
		if (p_op == "uv_group") {
			for (const hewn::Face &F : m.faces) {
				g = MAX(g, F.uv_group);
			}
			g++;
		}
		for (int f : s.faces) {
			m.faces[f].uv_group = g;
			m.faces[f].manual_uv = false;
		}
		geometry = false;
	} else if (p_op == "uv_reset") {
		for (int f : s.faces) {
			m.faces[f].manual_uv = false;
			m.faces[f].uvp = hewn::UVParams();
		}
		geometry = false;
	} else if (p_op == "uv_rotate90" || p_op == "uv_flip_u" || p_op == "uv_flip_v" || p_op == "uv_fit") {
		// Bake the current projection into manual UVs, then transform around the selection center.
		const hewn::BuildOut &b = n->get_build();
		Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
		for (int f : s.faces) {
			hewn::Face &F = m.faces[f];
			if (!F.manual_uv && f < (int)b.face_uvs.size()) {
				F.uv = b.face_uvs[f];
				F.manual_uv = true;
			}
			for (const Vector2 &u : F.uv) {
				mn = mn.min(u);
				mx = mx.max(u);
			}
		}
		const Vector2 c = (mn + mx) * 0.5f;
		const Vector2 size = mx - mn;
		const float fit = MAX(size.x, size.y);
		for (int f : s.faces) {
			for (Vector2 &u : m.faces[f].uv) {
				Vector2 d = u - c;
				if (p_op == "uv_rotate90") {
					u = c + Vector2(-d.y, d.x);
				} else if (p_op == "uv_flip_u") {
					u = c + Vector2(-d.x, d.y);
				} else if (p_op == "uv_flip_v") {
					u = c + Vector2(d.x, -d.y);
				} else if (fit > 1e-9f) {
					u = (u - mn) / fit;
				}
			}
		}
		geometry = false;
	} else if (p_op.begins_with("material_slot:")) {
		const int slot = p_op.get_slice(":", 1).to_int();
		for (int f : (mode == MODE_FACE ? s.faces : hewn::sel::faces_from_verts(m, _selected_verts(), true))) {
			m.faces[f].material = slot;
		}
		geometry = false;
	} else if (p_op == "vertex_color") {
		const Color c = _paint_brush_value();
		const int ch = dock->paint_channel ? dock->paint_channel->get_selected() : 0;
		if (ch > 0 && n->custom_formats[ch - 1] < 0) {
			n->custom_formats[ch - 1] = dock->paint_format->get_selected();
		}
		if (mode == MODE_FACE) {
			for (int f : s.faces) {
				m.faces[f].ensure_attrs();
				for (int i = 0; i < m.faces[f].size(); i++) {
					_paint_value(m.faces[f], i, ch) = c;
				}
			}
		} else {
			std::vector<int> vs = _selected_verts();
			std::unordered_set<int> vset(vs.begin(), vs.end());
			for (hewn::Face &F : m.faces) {
				F.ensure_attrs();
				for (int i = 0; i < F.size(); i++) {
					if (vset.count(F.v[i])) {
						_paint_value(F, i, ch) = c;
					}
				}
			}
		}
		geometry = false;
	} else {
		editing = false;
		ERR_PRINT("Hewn: unknown operation " + p_op);
		return;
	}
	hewn::remove_unused_vertices(m, &s);
	sanitize(m, s);
	n->commit(geometry);
	end_edit();
	if (is_param_op(p_op)) {
		_record_param_op(p_op, op_mode);
	} else if (!redo_running) {
		_hide_redo();
	}
}

// keyboard moves: the arrows push the selection a grid step away / toward / left / right along the
// world axis nearest to the view, Page Up / Page Down up and down. Objects in object mode (Ctrl: copies of them
// move), the selected elements in edit mode (Ctrl in face mode: the faces extruded by the step, a block out of
// the face).
bool HewnEditorPlugin::_arrow_keys(Camera3D *p_cam, const Ref<InputEventKey> &p_key) {
	if (!p_cam || !p_key->is_pressed() || p_key->is_shift_pressed() || p_key->is_meta_pressed() ||
			Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)) {
		return false;
	}
	const Key code = key_code(p_key);
	if (code != KEY_UP && code != KEY_DOWN && code != KEY_LEFT && code != KEY_RIGHT && code != KEY_PAGEUP && code != KEY_PAGEDOWN) {
		return false;
	}
	if (p_key->is_alt_pressed()) {
		return !p_key->is_ctrl_pressed() && _alt_arrow_rotate(p_cam, code);
	}
	const bool ctrl = p_key->is_ctrl_pressed();
	// the view's axes snapped to the world's (on the ground: X or Z)
	const Basis cb = p_cam->get_global_transform().basis;
	auto ground_axis = [](Vector3 d) {
		d.y = 0.0f;
		if (d.length() < 1e-5f) {
			return Vector3(0, 0, -1);
		}
		return std::fabs(d.x) >= std::fabs(d.z) ? Vector3(SIGN(d.x), 0, 0) : Vector3(0, 0, SIGN(d.z));
	};
	Vector3 fwd = ground_axis(-cb.get_column(2));
	if (fwd == Vector3(0, 0, -1) && std::fabs(cb.get_column(2).y) > 0.99f) {
		fwd = ground_axis(cb.get_column(1)); // (looking straight down: up on the screen)
	}
	const Vector3 right = ground_axis(cb.get_column(0));
	Vector3 dir = code == KEY_UP ? fwd : code == KEY_DOWN ? -fwd : code == KEY_RIGHT ? right : code == KEY_LEFT ? -right : code == KEY_PAGEUP ? Vector3(0, 1, 0) : Vector3(0, -1, 0);
	float step = dock && dock->snap_move ? (float)dock->snap_move->get_value() : 1.0f;
	if (step <= 0.0f) {
		step = 1.0f;
	}
	const Vector3 d = dir * step;
	if (mode == MODE_OBJECT) {
		std::vector<Node3D *> nodes;
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
			Node3D *n3 = Object::cast_to<Node3D>(v);
			if (n3 && n3 != root && n3->is_inside_tree()) {
				nodes.push_back(n3);
			}
		}
		if (nodes.empty()) {
			return false;
		}
		EditorUndoRedoManager *ur = get_undo_redo();
		if (ctrl) {
			// copies, moved; the copies selected (the originals stay)
			ur->create_action("Hewn: Duplicate and Move");
			std::vector<Node *> copies;
			for (Node3D *n3 : nodes) {
				Node3D *c = Object::cast_to<Node3D>(n3->duplicate());
				if (!c) {
					continue;
				}
				Node *parent = n3->get_parent();
				Transform3D t = n3->get_transform();
				const Node3D *p3 = Object::cast_to<Node3D>(parent);
				t.origin += p3 ? p3->get_global_transform().basis.inverse().xform(d) : d;
				c->set_transform(t);
				ur->add_do_method(parent, "add_child", c, true);
				ur->add_do_method(parent, "move_child", c, n3->get_index() + 1);
				ur->add_do_method(c, "set_owner", root);
				// (its own nodes saved with the scene too; the insides of placed scenes keep theirs)
				std::vector<Node *> stack = { c };
				while (!stack.empty()) {
					Node *x = stack.back();
					stack.pop_back();
					for (int i = 0; i < x->get_child_count(); i++) {
						Node *ch = x->get_child(i);
						if (ch->get_owner() == c || ch->get_owner() == nullptr) {
							ur->add_do_method(ch, "set_owner", root);
						}
						if (ch->get_scene_file_path().is_empty()) {
							stack.push_back(ch);
						}
					}
				}
				ur->add_do_reference(c);
				ur->add_undo_method(parent, "remove_child", c);
				copies.push_back(c);
			}
			ur->commit_action();
			EditorSelection *es = EditorInterface::get_singleton()->get_selection();
			es->clear();
			for (Node *c : copies) {
				es->add_node(c);
			}
			return true;
		}
		ur->create_action("Hewn: Move", UndoRedo::MERGE_ENDS); // (arrows pressed in a row: one step)
		for (Node3D *n3 : nodes) {
			const Node3D *p3 = Object::cast_to<Node3D>(n3->get_parent());
			const Vector3 ld = p3 ? p3->get_global_transform().basis.inverse().xform(d) : d;
			ur->add_do_property(n3, "position", n3->get_position() + ld);
			ur->add_undo_property(n3, "position", n3->get_position());
		}
		ur->commit_action();
		return true;
	}
	HewnMesh3D *n = get_edited();
	if (!n || (n->selection.verts.empty() && n->selection.edges.empty() && n->selection.faces.empty())) {
		return false;
	}
	if (ctrl && mode == MODE_FACE) {
		// a brush of its own: a block on the selected faces, a step long, as a new object next to this one;
		// edited along with it, its top selected (again: the next one)
		Node *parent = n->get_parent();
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		std::vector<int> region;
		for (int f : n->selection.faces) {
			if (f >= 0 && f < (int)n->get_data().faces.size()) {
				region.push_back(f);
			}
		}
		if (!parent || !root || region.empty()) {
			return true;
		}
		hewn::Mesh tmp = n->get_data();
		const int nf0 = (int)tmp.faces.size();
		const std::vector<int> tops = hw_make_block(tmp, region, n->get_global_transform().basis.inverse().xform(d));
		std::vector<int> made;
		for (int f = nf0; f < (int)tmp.faces.size(); f++) {
			made.push_back(f);
		}
		if (made.empty()) {
			return true;
		}
		HewnMesh3D *nn = memnew(HewnMesh3D);
		nn->set_shape(hewn::shapes::CUSTOM);
		nn->get_data() = hewn::ops::extract_faces(tmp, made);
		nn->set_materials(n->get_materials());
		String bname = n->get_name();
		if (bname.find("_block") > 0) {
			bname = bname.substr(0, bname.find("_block")); // (a block of a block: Cube_block2, not Cube_block_block)
		}
		nn->set_name(bname + "_block");
		nn->set_transform(n->get_transform());
		nn->commit(true);
		EditorUndoRedoManager *ur = get_undo_redo();
		ur->create_action("Hewn: Block from Faces");
		ur->add_do_method(parent, "add_child", nn, true);
		ur->add_do_method(parent, "move_child", nn, n->get_index() + 1);
		ur->add_do_method(nn, "set_owner", root);
		ur->add_do_reference(nn);
		ur->add_undo_method(parent, "remove_child", nn);
		ur->commit_action();
		n->selection.clear();
		n->update_gizmos();
		multi_ids.push_back(nn->get_instance_id());
		_set_active_mesh(nn);
		nn->selection.clear();
		for (int t : tops) {
			nn->selection.faces.push_back(t - nf0);
		}
		selection_changed();
		return true;
	}
	const String op = "translate";
	set_op_param(op, "x", d.x);
	set_op_param(op, "y", d.y);
	set_op_param(op, "z", d.z);
	set_op_param(op, "prop", false);
	set_op_param(op, "prop_projected", false);
	merge_ends_next = true; // (arrows pressed in a row: one undo step)
	run_op(op);
	merge_ends_next = false;
	return true;
}

// Alt+arrows: a quarter turn of the selection (objects, or the selected elements) about the view's axes snapped to
// the world's: left / right around the vertical, up / down around the view's sideways axis, Page Up / Down around
// the one looking into the view. Objects turn about the middle of their bounds.
bool HewnEditorPlugin::_alt_arrow_rotate(Camera3D *p_cam, Key p_code) {
	const Basis cb = p_cam->get_global_transform().basis;
	auto major = [](const Vector3 &p_d) {
		const Vector3 a = p_d.abs();
		if (a.x >= a.y && a.x >= a.z) {
			return Vector3(SIGN(p_d.x), 0, 0);
		}
		return a.y >= a.z ? Vector3(0, SIGN(p_d.y), 0) : Vector3(0, 0, SIGN(p_d.z));
	};
	Vector3 right = major(Vector3(cb.get_column(0).x, 0, cb.get_column(0).z));
	if (Vector3(cb.get_column(0).x, 0, cb.get_column(0).z).length() < 1e-4f) {
		right = Vector3(1, 0, 0);
	}
	const Vector3 back = right.cross(Vector3(0, 1, 0)); // (toward the viewer, on the ground)
	Vector3 axis;
	float s = 1.0f;
	switch (p_code) {
		case KEY_LEFT:
			axis = Vector3(0, 1, 0);
			s = 1.0f;
			break;
		case KEY_RIGHT:
			axis = Vector3(0, 1, 0);
			s = -1.0f;
			break;
		case KEY_UP:
			axis = right;
			s = -1.0f; // (the top goes away from the viewer)
			break;
		case KEY_DOWN:
			axis = right;
			s = 1.0f;
			break;
		case KEY_PAGEUP:
			axis = back;
			s = 1.0f;
			break;
		default:
			axis = back;
			s = -1.0f;
			break;
	}
	// the quarter turn with whole numbers: R v = a (a.v) + s (a x v)
	Basis rot;
	for (int i = 0; i < 3; i++) {
		Vector3 e;
		e[i] = 1.0f;
		rot.set_column(i, axis * axis.dot(e) + axis.cross(e) * s);
	}
	if (mode == MODE_OBJECT) {
		std::vector<Node3D *> nodes;
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		AABB box;
		bool has_box = false;
		for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
			Node3D *n3 = Object::cast_to<Node3D>(v);
			if (!n3 || n3 == root || !n3->is_inside_tree()) {
				continue;
			}
			nodes.push_back(n3);
			AABB b(n3->get_global_position(), Vector3());
			if (VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(n3)) {
				b = n3->get_global_transform().xform(vi->get_aabb());
			}
			box = has_box ? box.merge(b) : b;
			has_box = true;
		}
		if (nodes.empty()) {
			return false;
		}
		// (the center on the grid: what was on the grid stays on it; a box of odd length shifts by half a step)
		const float st = dock && dock->snap_move ? MAX(1e-4f, (float)dock->snap_move->get_value()) : 1.0f;
		const Vector3 c = (box.get_center() / st).round() * st;
		EditorUndoRedoManager *ur = get_undo_redo();
		ur->create_action("Hewn: Rotate 90");
		for (Node3D *n3 : nodes) {
			Transform3D gt = n3->get_global_transform();
			gt.basis = rot * gt.basis;
			gt.origin = c + rot.xform(gt.origin - c);
			const Node3D *p3 = Object::cast_to<Node3D>(n3->get_parent());
			const Transform3D lt = p3 ? p3->get_global_transform().affine_inverse() * gt : gt;
			ur->add_do_property(n3, "transform", lt);
			ur->add_undo_property(n3, "transform", n3->get_transform());
		}
		ur->commit_action();
		return true;
	}
	HewnMesh3D *n = get_edited();
	Vector3 piv;
	if (!n || vpaint || !_pivot(piv)) {
		return false;
	}
	{
		const float st = dock && dock->snap_move ? MAX(1e-4f, (float)dock->snap_move->get_value()) : 1.0f;
		piv = (piv / st).round() * st; // (on the grid, as for objects)
	}
	set_op_param("rotate", "px", piv.x);
	set_op_param("rotate", "py", piv.y);
	set_op_param("rotate", "pz", piv.z);
	set_op_param("rotate", "ax", axis.x);
	set_op_param("rotate", "ay", axis.y);
	set_op_param("rotate", "az", axis.z);
	set_op_param("rotate", "angle", 90.0 * s);
	set_op_param("rotate", "prop", false);
	set_op_param("rotate", "pivot_mode", 0);
	run_op("rotate");
	return true;
}

std::vector<HewnEditorPlugin::SelSnap> HewnEditorPlugin::_sel_snapshot() const {
	std::vector<SelSnap> out;
	for (HewnMesh3D *m : edited_meshes()) {
		out.push_back({ ObjectID(m->get_instance_id()), m->get_selection_state() });
	}
	return out;
}

uint64_t HewnEditorPlugin::_history_version() const {
	HewnMesh3D *n = get_edited();
	EditorUndoRedoManager *ur = const_cast<HewnEditorPlugin *>(this)->get_undo_redo();
	if (!n || !ur) {
		return 0;
	}
	UndoRedo *h = ur->get_history_undo_redo(ur->get_object_history_id(n));
	return h ? h->get_version() : 0;
}

// Only the selection changed (no other step was made meanwhile, nothing being edited): one undo step for it.
void HewnEditorPlugin::_sel_undo_commit() {
	sel_undo_pending = false;
	if (mode == MODE_OBJECT || modal != MODAL_NONE || editing || drag != DRAG_NONE || _history_version() != sel_undo_version) {
		sel_undo_before.clear();
		return;
	}
	const std::vector<SelSnap> after = _sel_snapshot();
	bool changed = after.size() != sel_undo_before.size();
	for (size_t i = 0; i < after.size() && !changed; i++) {
		changed = after[i].id != sel_undo_before[i].id;
		for (const char *k : { "sel_verts", "sel_edges", "sel_faces" }) {
			changed = changed || PackedInt32Array(after[i].sel[k]) != PackedInt32Array(sel_undo_before[i].sel[k]);
		}
	}
	if (!changed) {
		sel_undo_before.clear();
		return;
	}
	HewnMesh3D *n = get_edited();
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Select", UndoRedo::MERGE_DISABLE, n);
	for (const SelSnap &s : after) {
		if (Object *o = ObjectDB::get_instance(s.id)) {
			ur->add_do_method(o, "set_selection_state", s.sel);
		}
	}
	for (const SelSnap &s : sel_undo_before) {
		if (Object *o = ObjectDB::get_instance(s.id)) {
			ur->add_undo_method(o, "set_selection_state", s.sel);
		}
	}
	ur->commit_action(false);
	sel_undo_before.clear();
}

// Keys of every mode, with or without a mesh: Shift+Tab snapping on / off, Shift+M the ruler.
bool HewnEditorPlugin::_any_mode_keys(Key p_code, bool p_ctrl, bool p_shift, bool p_alt) {
	if (p_ctrl || p_alt || !p_shift) {
		return false;
	}
	if (p_code == KEY_TAB && dock && dock->snap_enabled) {
		dock->snap_enabled->set_pressed(!dock->snap_enabled->is_pressed());
		update_overlays();
		return true;
	}
	if (p_code == KEY_M && drag == DRAG_NONE) {
		start_ruler();
		return true;
	}
	return false;
}

// Ctrl / Shift + wheel over a 3D viewport while a brush is out (Vertex Paint, Sculpt, Scatter): the brush's size /
// strength, taken before the viewport would zoom with it.
void HewnEditorPlugin::_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (phys_on && phys_grab >= 0 && _physics_wheel(mb)) {
		if (Viewport *v = get_viewport()) {
			v->set_input_as_handled(); // (before the view would zoom with it)
		}
		return;
	}
	auto shape_has = [&](const char *p_name) {
		for (const hewn::shapes::Param &sp : hewn::shapes::params((hewn::shapes::Type)shape_type)) {
			if (String(sp.name) == p_name) {
				return true;
			}
		}
		return false;
	};
	if (mb.is_valid() && modal == MODAL_SHAPE && shape_state >= 1 && (shape_has("steps") || shape_has("sides") || shape_has("segments")) && !mb->is_ctrl_pressed() && !mb->is_meta_pressed() && !mb->is_shift_pressed() &&
			!mb->is_alt_pressed() && (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP || mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN)) {
		// a shape being drawn: the wheel sets its steps / sides, before the viewport would zoom with it
		for (int i = 0; i < 4; i++) {
			SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(i);
			Control *box = vp ? Object::cast_to<Control>(vp->get_parent()) : nullptr;
			if (!box || !box->is_visible_in_tree() || !box->get_global_rect().has_point(mb->get_position())) {
				continue;
			}
			if (mb->is_pressed() && vp->get_camera_3d()) {
				_shape_input(vp->get_camera_3d(), mb);
			}
			if (Viewport *v = get_viewport()) {
				v->set_input_as_handled();
			}
			return;
		}
	}
	const bool sculpt_brush = sculpt && modal == MODAL_SCULPT && dock && dock->sculpt_radius_field;
	const bool scatter_brush = scatter_tool >= 0 && get_scatter() && mode == MODE_OBJECT && modal == MODAL_NONE;
	if (!(vpaint || sculpt_brush || scatter_brush) || mb.is_null() || !(mb->is_ctrl_pressed() || mb->is_meta_pressed() || mb->is_shift_pressed()) ||
			mb->is_alt_pressed() || (mb->get_button_index() != MOUSE_BUTTON_WHEEL_UP && mb->get_button_index() != MOUSE_BUTTON_WHEEL_DOWN)) {
		return;
	}
	if (vpaint && select_kind == 1 && modal != MODAL_PAINT && (mb->is_ctrl_pressed() || mb->is_meta_pressed())) {
		return; // (the circle select's own Ctrl+wheel)
	}
	const bool ctrl = mb->is_ctrl_pressed() || mb->is_meta_pressed();
	if (scatter_brush && !ctrl) {
		return; // (the scatter's brush: only Ctrl+wheel)
	}
	// over one of the 3D viewports?
	bool over = false;
	for (int i = 0; i < 4 && !over; i++) {
		SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(i);
		Control *box = vp ? Object::cast_to<Control>(vp->get_parent()) : nullptr;
		over = box && box->is_visible_in_tree() && box->get_global_rect().has_point(mb->get_position());
	}
	if (!over) {
		return;
	}
	bool used = false;
	if (!mb->is_pressed()) {
		used = true; // (the release of a wheel step taken: nothing of it reaches the camera)
	} else if (sculpt_brush) {
		const bool upw = mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP;
		if (mb->is_shift_pressed()) {
			dock->sculpt_strength_field->set_value(dock->sculpt_strength_field->get_value() + (upw ? 0.05 : -0.05));
		} else {
			dock->sculpt_radius_field->set_value(dock->sculpt_radius_field->get_value() * (upw ? 1.1 : 1.0 / 1.1));
		}
		update_overlays();
		used = true;
	} else if (scatter_brush) {
		HewnScatter3D *sc = get_scatter();
		sc->set_brush_radius(sc->get_brush_radius() * (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? 1.1f : 1.0f / 1.1f));
		update_overlays();
		used = true;
	} else {
		used = _paint_wheel(mb);
	}
	if (used) {
		if (Viewport *v = get_viewport()) {
			v->set_input_as_handled();
		}
	}
}

// ------------------------------------------------------------------ editor settings, heavy operations

namespace {
const char *SET_SUBDIV = "hewn/warnings/subdivide_face_limit";
const char *SET_HEADER = "hewn/interface/show_header";
const char *SET_TOOLS = "hewn/interface/show_tool_column";
const char *SET_KEYS = "hewn/interface/hotkeys";
const char *SET_WRAP = "hewn/interface/wrap_cursor_at_viewport_edges";
const char *SET_REDO = "hewn/interface/last_operation_panel";
const char *SET_PICK = "hewn/selection/pick_distance";
const char *SET_QUAD = "hewn/interface/quad_view";
const char *SET_VSIZE = "hewn/viewport/vertex_size";
const char *SET_VCOL = "hewn/viewport/vertex_color";
const char *SET_VSEL = "hewn/viewport/selected_vertex_color";
const char *SET_ECOL = "hewn/viewport/edge_color";
const char *SET_ESEL = "hewn/viewport/selected_edge_color";
const char *SET_FSEL = "hewn/viewport/selected_face_color";
const char *SET_OCOL = "hewn/viewport/origin_color";
const char *SET_OSIZE = "hewn/viewport/origin_size";
const char *SET_SINGLE = "hewn/viewport/single_color";
const char *SET_WLIGHT = "hewn/viewport/world_space_lighting";
const char *SET_WANGLE = "hewn/viewport/world_space_lighting_angle";
const char *SET_CAV = "hewn/viewport/cavity";
const char *SET_CAV_RIDGE = "hewn/viewport/cavity_ridge";
const char *SET_CAV_VALLEY = "hewn/viewport/cavity_valley";
const char *SET_CAV_TYPE = "hewn/viewport/cavity_type";
const char *SET_GIZMO = "hewn/viewport/show_gizmo";
const char *SET_SMOVE = "hewn/snapping/move_step";
const char *SET_SROT = "hewn/snapping/rotate_step";
const char *SET_SSCALE = "hewn/snapping/scale_step";

// 1234567 -> "1 234 567"
String grouped(int64_t p_n) {
	const String s = itos(p_n < 0 ? -p_n : p_n);
	String out;
	for (int64_t i = 0; i < s.length(); i++) {
		if (i > 0 && (s.length() - i) % 3 == 0) {
			out += " ";
		}
		out += String::chr(s[i]);
	}
	return p_n < 0 ? "-" + out : out;
}

void add_editor_setting(EditorSettings *p_es, const char *p_name, const Variant &p_default, Variant::Type p_type, PropertyHint p_hint, const String &p_hint_string) {
	if (!p_es->has_setting(p_name)) {
		p_es->set_setting(p_name, p_default);
	}
	p_es->set_initial_value(p_name, p_default, false);
	Dictionary info;
	info["name"] = p_name;
	info["type"] = p_type;
	info["hint"] = p_hint;
	info["hint_string"] = p_hint_string;
	p_es->add_property_info(info);
}
} // namespace

// The addon's own settings in Editor Settings > Hewn (the editor's, not the project's).
void HewnEditorPlugin::register_editor_settings() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_null()) {
		return;
	}
	add_editor_setting(es.ptr(), SET_SUBDIV, 200000, Variant::INT, PROPERTY_HINT_RANGE, "0,100000000,1000,or_greater");
	add_editor_setting(es.ptr(), SET_HEADER, true, Variant::BOOL, PROPERTY_HINT_NONE, "");
	add_editor_setting(es.ptr(), SET_TOOLS, true, Variant::BOOL, PROPERTY_HINT_NONE, "");
	add_editor_setting(es.ptr(), SET_KEYS, true, Variant::BOOL, PROPERTY_HINT_NONE, "");
	add_editor_setting(es.ptr(), SET_WRAP, true, Variant::BOOL, PROPERTY_HINT_NONE, "");
	add_editor_setting(es.ptr(), SET_REDO, true, Variant::BOOL, PROPERTY_HINT_NONE, "");
	add_editor_setting(es.ptr(), SET_PICK, 14.0, Variant::FLOAT, PROPERTY_HINT_RANGE, "4,64,1,suffix:px");
	add_editor_setting(es.ptr(), SET_QUAD, true, Variant::BOOL, PROPERTY_HINT_NONE, "");
	{
		const HwMeshStyle def;
		add_editor_setting(es.ptr(), SET_VSIZE, def.vert_size, Variant::FLOAT, PROPERTY_HINT_RANGE, "1,32,0.5");
		add_editor_setting(es.ptr(), SET_VCOL, def.vert, Variant::COLOR, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_VSEL, def.vert_sel, Variant::COLOR, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_ECOL, def.edge, Variant::COLOR, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_ESEL, def.edge_sel, Variant::COLOR, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_FSEL, def.face_sel, Variant::COLOR, PROPERTY_HINT_NONE, "");
		// (the first defaults of these colors: what was not changed takes the new ones)
		const struct {
			const char *name;
			Color old_def;
			Color def;
		} moved[3] = { { SET_VSEL, Color(1.0f, 0.55f, 0.08f, 1.0f), def.vert_sel }, { SET_ESEL, Color(1.0f, 0.55f, 0.08f, 1.0f), def.edge_sel },
			{ SET_FSEL, Color(1.0f, 0.55f, 0.08f, 0.38f), def.face_sel } };
		for (const auto &mv : moved) {
			if (Color(es->get_setting(mv.name)).is_equal_approx(mv.old_def)) {
				es->set_setting(mv.name, mv.def);
			}
		}
		add_editor_setting(es.ptr(), SET_OCOL, def.origin, Variant::COLOR, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_OSIZE, def.origin_size, Variant::FLOAT, PROPERTY_HINT_RANGE, "1,32,0.5");
		add_editor_setting(es.ptr(), SET_SINGLE, def.single, Variant::COLOR, PROPERTY_HINT_COLOR_NO_ALPHA, "");
		for (const Color &old_def : { Color(0.788f, 0.798f, 0.803f, 1.0f), Color::hex(0xd4d5d6ff) }) {
			if (Color(es->get_setting(SET_SINGLE)).is_equal_approx(old_def)) {
				es->set_setting(SET_SINGLE, def.single); // (an earlier default)
			}
		}
		add_editor_setting(es.ptr(), SET_CAV, def.cavity, Variant::BOOL, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_CAV_RIDGE, def.cavity_ridge, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,2,0.01");
		add_editor_setting(es.ptr(), SET_CAV_VALLEY, def.cavity_valley, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,2,0.01");
		add_editor_setting(es.ptr(), SET_CAV_TYPE, def.cavity_type, Variant::INT, PROPERTY_HINT_ENUM, "World Space,Screen Space,World and Screen");
		add_editor_setting(es.ptr(), SET_WLIGHT, def.world_light, Variant::BOOL, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_WANGLE, def.world_light_angle, Variant::FLOAT, PROPERTY_HINT_RANGE, "-180,180,1");
		add_editor_setting(es.ptr(), SET_GIZMO, true, Variant::BOOL, PROPERTY_HINT_NONE, "");
		add_editor_setting(es.ptr(), SET_SMOVE, 0.25, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,100,0.001,or_greater");
		add_editor_setting(es.ptr(), SET_SROT, 15.0, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,180,0.1");
		add_editor_setting(es.ptr(), SET_SSCALE, 0.1, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,10,0.001");
	}
	if (!es->is_connected("settings_changed", callable_mp(this, &HewnEditorPlugin::_on_editor_settings))) {
		es->connect("settings_changed", callable_mp(this, &HewnEditorPlugin::_on_editor_settings));
	}
	_on_editor_settings();
}

void HewnEditorPlugin::_on_editor_settings() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_null()) {
		return;
	}
	show_header = es->has_setting(SET_HEADER) ? (bool)es->get_setting(SET_HEADER) : true;
	show_tools = es->has_setting(SET_TOOLS) ? (bool)es->get_setting(SET_TOOLS) : true;
	if (dock) {
		if (dock->toolbar_root) {
			dock->toolbar_root->set_visible(show_header);
		}
		if (dock->toolbar_fold) {
			dock->toolbar_fold->set_visible(show_header);
		}
		if (dock->tool_column) {
			dock->tool_column->set_visible(show_tools);
		}
	}
	hotkeys_on = es->has_setting(SET_KEYS) ? (bool)es->get_setting(SET_KEYS) : true;
	wrap_cursor_on = es->has_setting(SET_WRAP) ? (bool)es->get_setting(SET_WRAP) : true;
	redo_panel_on = es->has_setting(SET_REDO) ? (bool)es->get_setting(SET_REDO) : true;
	pick_px = es->has_setting(SET_PICK) ? CLAMP((float)es->get_setting(SET_PICK), 4.0f, 64.0f) : 14.0f;
	if (!redo_panel_on && redo_panel) {
		redo_panel->hide();
	}
	{
		// the colors / vertex size of the edited mesh: its overlay made again with them
		HwMeshStyle &st = hw_mesh_style();
		const HwMeshStyle def;
		auto col = [&](const char *p_name, const Color &p_def) { return es->has_setting(p_name) ? Color(es->get_setting(p_name)) : p_def; };
		st.vert_size = es->has_setting(SET_VSIZE) ? CLAMP((float)es->get_setting(SET_VSIZE), 1.0f, 32.0f) : def.vert_size;
		st.vert = col(SET_VCOL, def.vert);
		st.vert_sel = col(SET_VSEL, def.vert_sel);
		st.edge = col(SET_ECOL, def.edge);
		st.edge_sel = col(SET_ESEL, def.edge_sel);
		st.face_sel = col(SET_FSEL, def.face_sel);
		st.origin = col(SET_OCOL, def.origin);
		st.origin_size = es->has_setting(SET_OSIZE) ? CLAMP((float)es->get_setting(SET_OSIZE), 1.0f, 32.0f) : def.origin_size;
		st.single = col(SET_SINGLE, def.single);
		st.world_light = es->has_setting(SET_WLIGHT) ? (bool)es->get_setting(SET_WLIGHT) : def.world_light;
		st.world_light_angle = es->has_setting(SET_WANGLE) ? CLAMP((float)es->get_setting(SET_WANGLE), -180.0f, 180.0f) : def.world_light_angle;
		const bool cavity_was = st.cavity;
		st.cavity = es->has_setting(SET_CAV) ? (bool)es->get_setting(SET_CAV) : def.cavity;
		if (st.cavity != cavity_was) {
			// (the shader with the cavity reads the screen's normals: made only while it is on)
			solid_view_mat[0].unref();
			if (paint_view == 4) {
				apply_paint_view();
			}
		}
		st.cavity_ridge = es->has_setting(SET_CAV_RIDGE) ? CLAMP((float)es->get_setting(SET_CAV_RIDGE), 0.0f, 2.0f) : def.cavity_ridge;
		st.cavity_valley = es->has_setting(SET_CAV_VALLEY) ? CLAMP((float)es->get_setting(SET_CAV_VALLEY), 0.0f, 2.0f) : def.cavity_valley;
		st.cavity_type = es->has_setting(SET_CAV_TYPE) ? CLAMP((int)es->get_setting(SET_CAV_TYPE), 0, 2) : def.cavity_type;
		_apply_solid_params();
		if (dock && dock->solid_angle) {
			dock->solid_angle->set_value_no_signal(st.world_light_angle);
			dock->solid_world->set_pressed_no_signal(st.world_light);
			dock->cavity_check->set_pressed_no_signal(st.cavity);
			dock->cavity_ridge->set_value_no_signal(st.cavity_ridge);
			dock->cavity_valley->set_value_no_signal(st.cavity_valley);
			dock->cavity_type->select(st.cavity_type);
			dock->sync_shading();
		}
		// the gizmo and the snap steps as they were left (the header's buttons write them back)
		if (dock && dock->show_gizmo && es->has_setting(SET_GIZMO) && dock->show_gizmo->is_pressed() != (bool)es->get_setting(SET_GIZMO)) {
			dock->show_gizmo->set_pressed((bool)es->get_setting(SET_GIZMO));
		}
		const struct {
			const char *name;
			HewnDragField *field;
		} steps[3] = { { SET_SMOVE, dock ? dock->snap_move : nullptr }, { SET_SROT, dock ? dock->snap_rotate : nullptr }, { SET_SSCALE, dock ? dock->snap_scale : nullptr } };
		for (const auto &sf : steps) {
			if (sf.field && es->has_setting(sf.name) && std::fabs(sf.field->get_value() - (double)es->get_setting(sf.name)) > 1e-9) {
				sf.field->set_value((double)es->get_setting(sf.name));
			}
		}
		hw_gizmo_clear_cache();
		for (HewnMesh3D *m : edited_meshes()) {
			m->update_gizmos();
		}
		update_overlays();
	}
	callable_mp(this, &HewnEditorPlugin::_sync_quad).call_deferred(); // (not while the editor adds this plugin)
}

// The header's gizmo button and snap steps written into the editor settings (they come back after a restart).
void HewnEditorPlugin::_save_view_state() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_null() || !dock) {
		return;
	}
	auto put = [&](const char *p_name, const Variant &p_value) {
		if (!es->has_setting(p_name) || es->get_setting(p_name) != p_value) {
			es->set_setting(p_name, p_value);
		}
	};
	if (dock->show_gizmo) {
		put(SET_GIZMO, dock->show_gizmo->is_pressed());
	}
	if (dock->snap_move) {
		put(SET_SMOVE, dock->snap_move->get_value());
		put(SET_SROT, dock->snap_rotate->get_value());
		put(SET_SSCALE, dock->snap_scale->get_value());
	}
}

void HewnDock::_on_view_state_changed(double p_unused) {
	if (plugin) {
		plugin->_save_view_state();
	}
}

void HewnDock::_on_cavity(bool p_on) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid()) {
		es->set_setting(SET_CAV, p_on);
	}
}

void HewnDock::_on_cavity_value(double p_unused) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid()) {
		es->set_setting(SET_CAV_RIDGE, cavity_ridge->get_value());
		es->set_setting(SET_CAV_VALLEY, cavity_valley->get_value());
	}
}

void HewnDock::_on_cavity_type(int p_index) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid()) {
		es->set_setting(SET_CAV_TYPE, p_index);
	}
}

void HewnDock::_on_solid_angle(double p_value) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid()) {
		es->set_setting(SET_WANGLE, p_value);
	}
}

void HewnDock::_on_solid_world(bool p_on) {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid()) {
		es->set_setting(SET_WLIGHT, p_on);
	}
}

void hw_remove_quad_plugin() {
	EditorPlugins::remove_by_type<HewnQuadPlugin>();
}

// The Quad main screen comes and goes with its setting (and with this plugin).
void HewnEditorPlugin::_sync_quad() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	const bool want = is_inside_tree() && (es.is_null() || !es->has_setting(SET_QUAD) || (bool)es->get_setting(SET_QUAD));
	if (want && !quad_added) {
		EditorPlugins::add_by_type<HewnQuadPlugin>();
	} else if (!want && quad_added) {
		hw_remove_quad_plugin();
	}
	quad_added = want;
}

int64_t HewnEditorPlugin::subdivide_limit() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	return es.is_valid() && es->has_setting(SET_SUBDIV) ? (int64_t)es->get_setting(SET_SUBDIV) : 200000;
}

// How many faces Subdivide adds with these cuts (counted, nothing made): a quad or a triangle becomes (cuts + 1)^2
// faces, a face of more corners (cuts + 1) triangles for each of its sides.
// Catmull-Clark: a face of k corners becomes k quads, then every level 4 times as many (all the faces).
int64_t HewnEditorPlugin::_heavy_added(int p_value) const {
	if (heavy_op != "catmull_clark") {
		return _subdiv_added(p_value);
	}
	int64_t before = 0, after = 0;
	for (HewnMesh3D *n : edited_meshes()) {
		for (const hewn::Face &F : n->get_data().faces) {
			before++;
			after += F.size();
		}
	}
	if (p_value <= 0) {
		return 0;
	}
	for (int l = 1; l < MIN(p_value, 6); l++) {
		after *= 4;
	}
	return after - before;
}

int64_t HewnEditorPlugin::_subdiv_added(int p_cuts) const {
	const int64_t N = (int64_t)CLAMP(p_cuts, 1, 100) + 1;
	int64_t added = 0;
	for (HewnMesh3D *n : edited_meshes()) {
		const hewn::Mesh &m = n->get_data();
		for (int f : n->selection.faces) {
			if (f >= 0 && f < (int)m.faces.size()) {
				const int64_t k = m.faces[f].size();
				added += (k <= 4 ? N * N : k * N) - 1;
			}
		}
	}
	return added;
}

bool HewnEditorPlugin::_heavy_check(const String &p_what, int64_t p_faces, const Callable &p_go, const Callable &p_cancel) {
	const int64_t limit = subdivide_limit();
	if (heavy_pass || limit <= 0 || p_faces < limit) {
		return true; // (go on now)
	}
	heavy_any_go = p_go;
	heavy_any_cancel = p_cancel;
	heavy_any_text = vformat(T("%s would make about %s faces. That can take long and slow the editor down. Do it?"), p_what, String::num_int64(p_faces));
	if (Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) {
		heavy_any_wait = true; // (a value being dragged: asked when it is let go)
	} else {
		callable_mp(this, &HewnEditorPlugin::_heavy_any_popup).call_deferred();
	}
	return false;
}

void HewnEditorPlugin::_heavy_any_popup() {
	heavy_any_wait = false;
	if (!heavy_any_dialog) {
		heavy_any_dialog = memnew(ConfirmationDialog);
		heavy_any_dialog->set_name("HewnHeavyAnyDialog");
		heavy_any_dialog->set_title(T("Many Faces"));
		heavy_any_dialog->get_label()->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		heavy_any_dialog->get_label()->set_custom_minimum_size(Vector2(360, 0) * EditorInterface::get_singleton()->get_editor_scale());
		heavy_any_dialog->connect("confirmed", callable_mp(this, &HewnEditorPlugin::_on_heavy_any_ok));
		heavy_any_dialog->connect("canceled", callable_mp(this, &HewnEditorPlugin::_on_heavy_any_cancel));
		EditorInterface::get_singleton()->get_base_control()->add_child(heavy_any_dialog);
	}
	heavy_any_dialog->set_text(heavy_any_text + "\n\n" + T("From how many faces this is asked: Editor Settings > Hewn > Warnings (0: never)."));
	heavy_any_dialog->reset_size();
	heavy_any_dialog->popup_centered(Vector2i(Vector2(420, 0) * EditorInterface::get_singleton()->get_editor_scale()));
}

void HewnEditorPlugin::_on_heavy_any_ok() {
	const Callable go = heavy_any_go;
	heavy_any_go = Callable();
	heavy_any_cancel = Callable();
	if (go.is_valid()) {
		go.call();
	}
}

void HewnEditorPlugin::_on_heavy_any_cancel() {
	const Callable back = heavy_any_cancel;
	heavy_any_go = Callable();
	heavy_any_cancel = Callable();
	if (back.is_valid()) {
		back.call();
	}
}

void HewnEditorPlugin::_heavy_popup() {
	if (!heavy_dialog) {
		heavy_dialog = memnew(ConfirmationDialog);
		heavy_dialog->set_name("HewnHeavyDialog");
		heavy_dialog->set_title(T("Subdivide: Many Faces"));
		heavy_dialog->set_ok_button_text(T("Subdivide"));
		VBoxContainer *v = memnew(VBoxContainer);
		heavy_dialog->add_child(v);
		heavy_label = memnew(Label);
		heavy_label->set_custom_minimum_size(Vector2(360, 0) * EditorInterface::get_singleton()->get_editor_scale());
		heavy_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		v->add_child(heavy_label);
		HBoxContainer *h = memnew(HBoxContainer);
		v->add_child(h);
		Label *l = memnew(Label);
		heavy_key_label = l;
		l->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		h->add_child(l);
		heavy_cuts = memnew(SpinBox);
		HewnDragField::hook_spinbox(heavy_cuts);
		heavy_cuts->set_min(1);
		heavy_cuts->set_max(100);
		heavy_cuts->set_step(1);
		heavy_cuts->connect("value_changed", callable_mp(this, &HewnEditorPlugin::_on_heavy_cuts));
		h->add_child(heavy_cuts);
		Label *hint = memnew(Label);
		hint->set_text(T("From how many faces this is asked: Editor Settings > Hewn > Warnings (0: never)."));
		hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		hint->set_custom_minimum_size(Vector2(360, 0) * EditorInterface::get_singleton()->get_editor_scale()); // (a wrapped label with no width: a word a line, a tall window)
		hint->set_modulate(Color(1, 1, 1, 0.6f));
		v->add_child(hint);
		heavy_dialog->connect("confirmed", callable_mp(this, &HewnEditorPlugin::_on_heavy_ok));
		heavy_dialog->connect("canceled", callable_mp(this, &HewnEditorPlugin::_on_heavy_cancel));
		EditorInterface::get_singleton()->get_base_control()->add_child(heavy_dialog);
	}
	const bool levels = heavy_op == "catmull_clark";
	heavy_key_label->set_text(T(levels ? "Levels" : "Number of Cuts"));
	heavy_cuts->set_max(levels ? 6 : 100);
	heavy_cuts->set_min(levels ? 0 : 1);
	if (heavy_dialog->is_visible()) {
		heavy_cuts->set_value_no_signal(heavy_cuts_wanted);
		_on_heavy_cuts(heavy_cuts_wanted);
		return;
	}
	heavy_cuts->set_value_no_signal(heavy_cuts_wanted);
	_on_heavy_cuts(heavy_cuts_wanted);
	heavy_dialog->reset_size();
	heavy_dialog->popup_centered(Vector2i(Vector2(420, 0) * EditorInterface::get_singleton()->get_editor_scale()));
}

void HewnEditorPlugin::_on_heavy_cuts(double p_cuts) {
	int64_t faces = 0;
	for (HewnMesh3D *n : edited_meshes()) {
		faces += (int64_t)n->get_data().faces.size();
	}
	const int64_t added = _heavy_added((int)p_cuts);
	heavy_label->set_text(vformat(T(heavy_op == "catmull_clark" ? "Subdivide would add about %s faces (%s in all): it can take a long while and a lot of memory. Change the levels here, or go on." :
																"Subdivide would add about %s faces (%s in all): it can take a long while and a lot of memory. Change the number of cuts here, or go on."),
			grouped(added), grouped(faces + added)));
}

void HewnEditorPlugin::_on_heavy_ok() {
	set_op_param(heavy_op, heavy_op == "catmull_clark" ? "levels" : "cuts", (int)heavy_cuts->get_value());
	heavy_ok = true;
	if (heavy_redo) {
		_redo_last();
		heavy_ok = false;
		_show_redo();
	} else {
		run_op(heavy_op);
		heavy_ok = false;
	}
}

void HewnEditorPlugin::_on_heavy_cancel() {
	heavy_wait = false;
	if (heavy_redo) {
		// the panel back to the value the mesh has now
		set_op_param(heavy_op, heavy_op == "catmull_clark" ? "levels" : "cuts", heavy_prev_cuts);
		_show_redo();
	}
}
