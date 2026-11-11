// Overlays, entities and triggers have no mesh of their own to click (an overlay is drawn in a shared
// batch, the markers are internal): their gizmo gives them an outline and a click target.
#include "hw_editor.h"

#include "../hw_cutter.h"
#include "../hw_entities.h"
#include "../hw_overlay3d.h"
#include "../hw_scatter.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/editor_node3d_gizmo.hpp>
#include <godot_cpp/classes/triangle_mesh.hpp>

using namespace godot;

HewnEntityGizmoPlugin::HewnEntityGizmoPlugin() {
	create_material("overlay", Color(0.3f, 0.8f, 1.0f, 0.9f));
	create_handle_material("overlay_handle");
	create_material("entity", Color(1.0f, 0.6f, 0.1f, 0.9f));
	create_material("trigger", Color(1.0f, 0.55f, 0.1f, 0.9f));
	create_material("cutter", Color(1.0f, 0.3f, 0.2f, 0.95f));
	create_material("collision", Color(0.0f, 0.6f, 0.7f, 0.9f)); // (as the meshes' collision)
}

bool HewnEntityGizmoPlugin::_has_gizmo(Node3D *p_node) const {
	return Object::cast_to<HewnOverlay3D>(p_node) || Object::cast_to<HewnEntity3D>(p_node) ||
			Object::cast_to<HewnCutter3D>(p_node) || Object::cast_to<HewnScatter3D>(p_node);
}

String HewnEntityGizmoPlugin::_get_gizmo_name() const {
	return "HewnEntity";
}

int32_t HewnEntityGizmoPlugin::_get_priority() const {
	return 0;
}

namespace {

Ref<TriangleMesh> tri_mesh(const PackedVector3Array &p_faces) {
	Ref<ArrayMesh> am;
	am.instantiate();
	Array arr;
	arr.resize(Mesh::ARRAY_MAX);
	arr[Mesh::ARRAY_VERTEX] = p_faces;
	am->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr);
	return am->generate_triangle_mesh();
}

void box_lines(PackedVector3Array &r_lines, const Vector3 &h) {
	for (int i = 0; i < 12; i++) {
		static const int E[12][2] = { { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 }, { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
		for (int k = 0; k < 2; k++) {
			const int c = E[i][k];
			r_lines.push_back(Vector3((c & 1) ? h.x : -h.x, (c & 2) ? h.y : -h.y, (c & 4) ? h.z : -h.z));
		}
	}
}

} // namespace

String HewnEntityGizmoPlugin::_get_handle_name(const Ref<EditorNode3DGizmo> &p_gizmo, int32_t p_handle_id, bool p_secondary) const {
	return "Control Point";
}

Variant HewnEntityGizmoPlugin::_get_handle_value(const Ref<EditorNode3DGizmo> &p_gizmo, int32_t p_handle_id, bool p_secondary) const {
	HewnOverlay3D *ov = Object::cast_to<HewnOverlay3D>(p_gizmo->get_node_3d());
	return ov ? Variant(ov->get_warp()) : Variant();
}

void HewnEntityGizmoPlugin::_set_handle(const Ref<EditorNode3DGizmo> &p_gizmo, int32_t p_handle_id, bool p_secondary, Camera3D *p_camera, const Vector2 &p_screen_pos) {
	HewnOverlay3D *ov = Object::cast_to<HewnOverlay3D>(p_gizmo->get_node_3d());
	if (!ov || !p_camera) {
		return;
	}
	// the mouse on the overlay's own plane
	const Transform3D inv = ov->get_global_transform().affine_inverse();
	const Vector3 from = inv.xform(p_camera->project_ray_origin(p_screen_pos));
	const Vector3 to = inv.xform(p_camera->project_ray_origin(p_screen_pos) + p_camera->project_ray_normal(p_screen_pos));
	const float dy = to.y - from.y;
	if (std::fabs(dy) < 1e-7f) {
		return;
	}
	const Vector3 hit = from + (to - from) * (-from.y / dy);
	ov->set_warp_point(p_handle_id, Vector2(hit.x, hit.z));
}

void HewnEntityGizmoPlugin::_commit_handle(const Ref<EditorNode3DGizmo> &p_gizmo, int32_t p_handle_id, bool p_secondary, const Variant &p_restore, bool p_cancel) {
	HewnOverlay3D *ov = Object::cast_to<HewnOverlay3D>(p_gizmo->get_node_3d());
	if (!ov) {
		return;
	}
	const PackedVector2Array before = p_restore;
	if (p_cancel) {
		ov->set_warp(before);
		return;
	}
	EditorUndoRedoManager *ur = EditorInterface::get_singleton()->get_editor_undo_redo();
	ur->create_action("Hewn: Move Overlay Point", UndoRedo::MERGE_DISABLE, ov);
	ur->add_do_method(ov, "set_warp", ov->get_warp());
	ur->add_undo_method(ov, "set_warp", before);
	ur->commit_action(false);
}

void HewnEntityGizmoPlugin::_redraw(const Ref<EditorNode3DGizmo> &p_gizmo) {
	p_gizmo->clear();
	Node3D *n = p_gizmo->get_node_3d();
	if (HewnOverlay3D *ov = Object::cast_to<HewnOverlay3D>(n)) {
		if (ov->has_meta("_hw_road")) {
			// (a curve's road: nothing to draw, a click on it selects the curve)
			const Dictionary rc = ov->get_cache();
			const PackedVector3Array rv = rc["verts"];
			const PackedInt32Array ri = rc["indices"];
			PackedVector3Array rf;
			for (int64_t i = 0; i < ri.size(); i++) {
				rf.push_back(rv[ri[i]]);
			}
			if (!rf.is_empty()) {
				p_gizmo->add_collision_triangles(tri_mesh(rf));
			}
			return;
		}
		// the control points, to pull the shape about
		{
			PackedVector3Array hp;
			PackedInt32Array ids;
			for (int i = 0; i < ov->get_control_points(); i++) {
				const Vector2 q = ov->get_warp_point(i);
				hp.push_back(Vector3(q.x, 0, q.y));
				ids.push_back(i);
			}
			p_gizmo->add_handles(hp, get_material("overlay_handle", p_gizmo), ids);
		}
		PackedVector3Array lines;
		const PackedVector2Array o = ov->get_outline_points();
		for (int64_t i = 0; i < o.size(); i++) {
			const Vector2 a = o[i], b = o[(i + 1) % o.size()];
			lines.push_back(Vector3(a.x, 0, a.y));
			lines.push_back(Vector3(b.x, 0, b.y));
		}
		lines.push_back(Vector3());
		lines.push_back(Vector3(0, ov->get_depth() * 0.5f, 0)); // (the way it faces)
		p_gizmo->add_lines(lines, get_material("overlay", p_gizmo));
		// click: the projected triangles, else the outline
		const Dictionary c = ov->get_cache();
		const PackedVector3Array v = c["verts"];
		const PackedInt32Array idx = c["indices"];
		PackedVector3Array faces;
		for (int64_t i = 0; i < idx.size(); i++) {
			faces.push_back(v[idx[i]]);
		}
		for (int64_t i = 1; i + 1 < o.size(); i++) {
			faces.push_back(Vector3(o[0].x, 0, o[0].y));
			faces.push_back(Vector3(o[i].x, 0, o[i].y));
			faces.push_back(Vector3(o[i + 1].x, 0, o[i + 1].y));
		}
		if (!faces.is_empty()) {
			p_gizmo->add_collision_triangles(tri_mesh(faces));
		}
		return;
	}
	if (HewnScatter3D *sc = Object::cast_to<HewnScatter3D>(n)) {
		// the Collision overlay: the copies' shapes (the game gets a body of them per chunk)
		if (HewnScatter3D::show_collision && sc->is_visible_in_tree()) {
			const PackedVector3Array lines = sc->collision_lines(4000);
			if (!lines.is_empty()) {
				p_gizmo->add_lines(lines, get_material("collision", p_gizmo));
			}
		}
		return;
	}
	if (HewnCutter3D *cu = Object::cast_to<HewnCutter3D>(n)) {
		if (!HewnCutter3D::show_shapes) {
			return; // (the overlay off)
		}
		PackedVector3Array lines, faces;
		for (const std::vector<Vector3> &poly : cu->local_outline()) {
			for (size_t i = 0; i < poly.size(); i++) {
				lines.push_back(poly[i]);
				lines.push_back(poly[(i + 1) % poly.size()]);
			}
			for (size_t i = 1; i + 1 < poly.size(); i++) {
				faces.push_back(poly[0]);
				faces.push_back(poly[i]);
				faces.push_back(poly[i + 1]);
			}
		}
		if (!lines.is_empty()) {
			p_gizmo->add_lines(lines, get_material("cutter", p_gizmo));
			p_gizmo->add_collision_triangles(tri_mesh(faces));
		}
		return;
	}
	if (HewnEntity3D *en = Object::cast_to<HewnEntity3D>(n)) {
		const float s = en->get_size();
		PackedVector3Array lines;
		box_lines(lines, Vector3(s * 0.3f, s * 0.5f, s * 0.3f));
		for (Vector3 &p : lines) {
			p.y += s * 0.5f;
		}
		p_gizmo->add_lines(lines, get_material("entity", p_gizmo));
		Ref<BoxMesh> bm;
		bm.instantiate();
		bm->set_size(Vector3(s * 0.6f, s, s * 0.6f));
		Ref<TriangleMesh> tm = bm->generate_triangle_mesh();
		// (the box mesh is centered: lift the triangles onto the marker)
		PackedVector3Array f = tm.is_valid() ? tm->get_faces() : PackedVector3Array();
		for (int64_t i = 0; i < f.size(); i++) {
			f.set(i, f[i] + Vector3(0, s * 0.5f, 0));
		}
		if (!f.is_empty()) {
			p_gizmo->add_collision_triangles(tri_mesh(f));
		}
	}
}
