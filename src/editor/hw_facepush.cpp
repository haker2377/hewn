// Shift + drag on a face moves it along its normal (as: the brush's side pushed out or in, the
// faces around it stretch). Ctrl+Shift + drag: outward a new block grows from the face, inward the object is
// cut in two and the part dragged over becomes an object of its own. While Shift is held the face under the
// mouse is outlined in yellow, also where
// something covers it. A Shift click without a drag still selects as before.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>

using namespace godot;
using namespace hw_editor;

std::vector<int> hw_make_block(hewn::Mesh &m, const std::vector<int> &region, const Vector3 &d); // (hw_plugin.cpp)

namespace {
void line3d(Control *p_overlay, Camera3D *p_cam, Vector3 a, Vector3 b, const Color &c, float w) {
	// (clipped at the camera: an end behind it would land anywhere on the screen)
	const Vector3 eye = p_cam->get_global_position(), fwd = -p_cam->get_global_transform().basis.get_column(2).normalized();
	const float near = (float)p_cam->get_near() * 1.5f;
	float da = (a - eye).dot(fwd) - near, db = (b - eye).dot(fwd) - near;
	if (da < 0.0f && db < 0.0f) {
		return;
	}
	if (da < 0.0f) {
		a = a.lerp(b, da / (da - db));
	} else if (db < 0.0f) {
		b = b.lerp(a, db / (db - da));
	}
	p_overlay->draw_line(p_cam->unproject_position(a), p_cam->unproject_position(b), c, w, true);
}
} // namespace

// The meshes whose faces can be pushed: in object mode the selected ones, in edit mode the edited ones.
std::vector<HewnMesh3D *> HewnEditorPlugin::_face_push_meshes() const {
	if (mode != MODE_OBJECT) {
		return edited_meshes();
	}
	std::vector<HewnMesh3D *> out;
	for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
		HewnMesh3D *m = Object::cast_to<HewnMesh3D>(v);
		if (m && m->is_inside_tree() && m->is_visible_in_tree() && !m->is_csg_model()) {
			out.push_back(m);
		}
	}
	return out;
}

// The face under the mouse among them (the nearest along the ray). Off the meshes: the face turned away behind
// the outline edge nearest to the mouse (above a box: its back face), so faces that are not seen can be taken too.
bool HewnEditorPlugin::_face_push_pick(Camera3D *p_cam, const Vector2 &p_pos, ObjectID &r_node, int &r_face, Vector3 &r_hit) const {
	Vector3 o, d;
	_ray(p_cam, p_pos, o, d);
	float best = 1e30f;
	r_face = -1;
	const std::vector<HewnMesh3D *> meshes = _face_push_meshes();
	for (HewnMesh3D *m : meshes) {
		float t;
		const int f = raycast_node(m, o, d, t, nullptr, false);
		if (f >= 0 && t < best && !m->get_data().faces[f].hidden) {
			best = t;
			r_face = f;
			r_node = ObjectID(m->get_instance_id());
			r_hit = o + d * t;
		}
	}
	if (r_face >= 0) {
		return true;
	}
	if (mode != MODE_OBJECT) {
		return false; // (edit mode: Shift + drag from beside the mesh is a box selection)
	}
	const Vector3 eye = p_cam->get_global_position();
	const bool ortho = p_cam->get_projection() == Camera3D::PROJECTION_ORTHOGONAL;
	const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2).normalized();
	float best_px = 80.0f * EditorInterface::get_singleton()->get_editor_scale();
	for (HewnMesh3D *m : meshes) {
		const hewn::Mesh &md = m->get_data();
		const Transform3D g = m->get_global_transform();
		const Basis nb = g.basis.inverse().transposed();
		hewn::Topology tp;
		tp.build(md);
		for (size_t e = 0; e < tp.edges.size(); e++) {
			const std::vector<int> &ef = tp.edge_faces[e];
			if (ef.size() != 2 || md.faces[ef[0]].hidden || md.faces[ef[1]].hidden) {
				continue;
			}
			const Vector3 a = g.xform(md.pos[tp.edges[e].a]), b = g.xform(md.pos[tp.edges[e].b]);
			if (p_cam->is_position_behind(a) || p_cam->is_position_behind(b)) {
				continue;
			}
			bool facing[2];
			for (int k = 0; k < 2; k++) {
				const hewn::Face &F = md.faces[ef[k]];
				const Vector3 n = nb.xform(hewn::face_normal(md, F));
				const Vector3 c = g.xform(hewn::face_center(md, F));
				facing[k] = (ortho ? -fwd : eye - c).dot(n) > 0.0f;
			}
			if (facing[0] == facing[1]) {
				continue; // (not on the outline)
			}
			const Vector2 sa = p_cam->unproject_position(a), sb = p_cam->unproject_position(b);
			const Vector2 ab = sb - sa;
			const float l2 = ab.length_squared();
			const float s = l2 > 1e-6f ? CLAMP((p_pos - sa).dot(ab) / l2, 0.0f, 1.0f) : 0.0f;
			const float px = p_pos.distance_to(sa + ab * s);
			if (px < best_px) {
				best_px = px;
				r_face = facing[0] ? ef[1] : ef[0];
				r_node = ObjectID(m->get_instance_id());
				r_hit = a.lerp(b, s);
			}
		}
	}
	return r_face >= 0;
}

// The face's plane moved by p_t along its normal (world units); or (Ctrl+Shift) the new block that long.
void HewnEditorPlugin::_face_push_apply(HewnMesh3D *p_n, float p_t) {
	const Vector3 ld = p_n->get_global_transform().basis.inverse().xform(push_n * p_t);
	const float delta = ld.dot(push_n_local); // (the plane's move along its local normal)
	if (push_block) {
		HewnMesh3D *blk = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(push_block_node));
		if (!blk) {
			return;
		}
		if (delta < -1e-5f) {
			// dragged inward: the object cut by the plane the face reached; the part between them becomes the
			// block, the rest stays this object
			hewn::Mesh outer = push_orig, inner = push_orig;
			const Vector3 fc = hewn::face_center(push_orig, push_orig.faces[push_face]);
			const Vector3 pt = fc + push_n_local * delta;
			float depth = 0.0f; // (how deep the object goes behind the face: the plane must stay inside)
			for (const Vector3 &q : push_orig.pos) {
				depth = MAX(depth, -(q - fc).dot(push_n_local));
			}
			hewn::ops::bisect(outer, {}, pt, push_n_local, true, false, true);
			hewn::ops::bisect(inner, {}, pt, push_n_local, false, true, true);
			if (-delta < depth - 1e-4f && !outer.faces.empty() && !inner.faces.empty()) {
				blk->get_data() = outer;
				blk->commit(true);
				blk->set_visible(true);
				_view_new_mesh(blk);
				p_n->get_data() = inner;
				p_n->selection = hewn::Selection();
				p_n->commit(true);
				push_split = true;
				return;
			}
		}
		if (push_split) {
			p_n->set_state(push_before);
			push_split = false;
		}
		if (push_planes) {
			// a convex block: the new block is what the face's plane sweeps over, between the planes of the faces
			// around (where they lean in, it gets smaller; it stops where the far face would be gone)
			if (delta <= 1e-5f) {
				blk->get_data() = hewn::Mesh();
				blk->commit(true);
				blk->set_visible(false);
				return;
			}
			hewn::Mesh moved;
			if (!hewn::ops::move_face_plane(push_source, push_plane_face, delta, moved, true)) {
				return; // (as it was last)
			}
			const Vector3 fc = hewn::face_center(push_orig, push_orig.faces[push_face]);
			hewn::ops::bisect(moved, {}, fc, push_n_local, true, false, true);
			if (moved.faces.empty()) {
				return;
			}
			blk->get_data() = moved;
			blk->commit(true);
			blk->set_visible(true);
			_view_new_mesh(blk);
			return;
		}
		hewn::Mesh tmp = push_orig;
		const int nf0 = (int)tmp.faces.size();
		std::vector<int> made;
		if (delta > 1e-5f) {
			hw_make_block(tmp, { push_face }, push_n_local * delta);
			for (int f = nf0; f < (int)tmp.faces.size(); f++) {
				made.push_back(f);
			}
		}
		blk->get_data() = made.empty() ? hewn::Mesh() : hewn::ops::extract_faces(tmp, made);
		blk->commit(true);
		blk->set_visible(!made.empty());
		_view_new_mesh(blk);
		return;
	}
	hewn::Mesh &m = p_n->get_data();
	if (push_planes) {
		// the faces where the planes now meet; no move where a face would vanish (it stays as it was last)
		if (std::fabs(delta) < 1e-6f) {
			m = push_orig; // (not moved: the faces as they were, those sharing a plane not joined)
			p_n->selection = push_sel;
			p_n->commit(true);
			return;
		}
		hewn::Mesh out;
		if (!hewn::ops::move_face_plane(push_source, push_plane_face, delta, out, true)) {
			return;
		}
		const bool same = out.pos.size() == push_orig.pos.size() && out.faces.size() == push_orig.faces.size();
		m = out;
		p_n->selection = same ? push_sel : hewn::Selection();
		p_n->commit(true);
		return;
	}
	for (size_t i = 0; i < push_verts.size(); i++) {
		// (a corner going along its edge stops at the edge's far end: it never runs on past the face beyond)
		const float d = push_lims[i] != 0.0f && delta / push_lims[i] > 1.0f ? push_lims[i] : delta;
		m.pos[push_verts[i]] = push_pos[i] + push_dirs[i] * d;
	}
	p_n->commit(true);
}

void HewnEditorPlugin::_face_push_hover(Camera3D *p_cam, bool p_shift) {
	ObjectID node;
	int face = -1;
	Vector3 hit;
	if (p_shift && modal == MODAL_NONE && drag == DRAG_NONE && !vpaint && !Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)) {
		_face_push_pick(p_cam, mouse_pos, node, face, hit);
	}
	if (node != push_hover_node || face != push_hover_face) {
		push_hover_node = node;
		push_hover_face = face;
		update_overlays();
	}
}

int32_t HewnEditorPlugin::_face_push_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	HewnMesh3D *n = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(push_node));
	if (push_state == 2 && n) {
		// dragging: the face along its normal; Esc / RMB put it back
		if ((k.is_valid() && k->is_pressed() && key_code(k) == KEY_ESCAPE) || (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT)) {
			if (push_block) {
				if (Node *blk = Object::cast_to<Node>(ObjectDB::get_instance(push_block_node))) {
					blk->get_parent()->remove_child(blk);
					memdelete(blk);
				}
				if (push_split) {
					n->set_state(push_before);
					push_split = false;
				}
			} else {
				n->set_state(push_before);
			}
			if (uv_lock && !push_block) {
				n->uv_lock_end();
			}
			push_state = 0;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mm.is_valid() || (k.is_valid() && (key_code(k) == KEY_CTRL || key_code(k) == KEY_META))) {
			// the point of the normal's line nearest to the mouse ray
			Vector3 o, d;
			_ray(p_cam, mouse_pos, o, d);
			const Vector3 w0 = push_origin - o;
			const float b = push_n.dot(d), dd = push_n.dot(w0), e = d.dot(w0);
			const float den = 1.0f - b * b;
			float t = den > 1e-6f ? (b * e - dd) / den : 0.0f;
			const bool ctrl = !push_block && (k.is_valid() ? ctrl_after(k) : (mm->is_ctrl_pressed() || mm->is_meta_pressed())); // (Ctrl+Shift: the block, snapped as usual)
			const bool snap_on = dock && dock->snap_enabled && dock->snap_enabled->is_pressed();
			if (snap_on != ctrl) {
				const float st = dock && dock->snap_move ? MAX(1e-4f, (float)dock->snap_move->get_value()) : 1.0f;
				t = std::round(t / st) * st;
			}
			push_dist = t;
			_face_push_apply(n, t);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && !mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
			push_state = 0;
			if (push_block) {
				// the block as an object of its own (one undo step); nothing when it has no length
				HewnMesh3D *blk = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(push_block_node));
				if (!blk) {
					return AFTER_GUI_INPUT_STOP;
				}
				Node *parent = blk->get_parent();
				parent->remove_child(blk);
				Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
				if (blk->get_data().faces.empty() || !root) {
					memdelete(blk);
					update_overlays();
					return AFTER_GUI_INPUT_STOP;
				}
				blk->set_visible(true);
				EditorUndoRedoManager *ur = get_undo_redo();
				ur->create_action(push_split ? "Hewn: Split Block" : "Hewn: Block from Face");
				ur->add_do_method(parent, "add_child", blk, true);
				ur->add_do_method(parent, "move_child", blk, n->get_index() + 1);
				ur->add_do_method(blk, "set_owner", root);
				ur->add_do_reference(blk);
				ur->add_undo_method(parent, "remove_child", blk);
				if (push_split) {
					// (this object keeps the rest)
					ur->add_do_method(n, "set_state", n->get_state());
					ur->add_undo_method(n, "set_state", push_before);
					push_split = false;
				}
				ur->commit_action();
				if (mode == MODE_OBJECT) {
					EditorSelection *es = EditorInterface::get_singleton()->get_selection();
					es->clear();
					es->add_node(blk);
				}
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			if (uv_lock) {
				n->uv_lock_end();
			}
			n->commit(true);
			const Dictionary after = n->get_state();
			EditorUndoRedoManager *ur = get_undo_redo();
			ur->create_action("Hewn: Move Face", UndoRedo::MERGE_DISABLE, n);
			ur->add_do_method(n, "set_state", after);
			ur->add_undo_method(n, "set_state", push_before);
			ur->commit_action(false);
			if (dock) {
				dock->refresh();
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (push_state == 1) {
		// pressed on a face with Shift: a drag pushes it, a click selects as Shift+click does
		if (mm.is_valid() && mm->get_position().distance_to(push_press) > 4.0f && n) {
			const hewn::Mesh &m = n->get_data();
			if (push_face < 0 || push_face >= (int)m.faces.size()) {
				push_state = 0;
				return AFTER_GUI_INPUT_STOP;
			}
			const hewn::Face &F = m.faces[push_face];
			push_before = n->get_state();
			push_n_local = hewn::face_normal(m, F).normalized();
			push_n = n->get_global_transform().basis.inverse().transposed().xform(push_n_local).normalized();
			push_verts.assign(F.v.begin(), F.v.end());
			push_pos.clear();
			push_dirs.clear();
			push_lims.clear();
			// each corner along the edge that leaves the face there (the faces around keep their planes); else straight
			hewn::Topology tp;
			tp.build(m);
			for (int v : push_verts) {
				push_pos.push_back(m.pos[v]);
				std::vector<int> away;
				for (size_t e = 0; e < tp.edges.size(); e++) {
					const hewn::Edge &E = tp.edges[e];
					if (E.a != v && E.b != v) {
						continue;
					}
					const int w = E.a == v ? E.b : E.a;
					if (std::find(F.v.begin(), F.v.end(), w) == F.v.end() && std::find(away.begin(), away.end(), w) == away.end()) {
						away.push_back(w);
					}
				}
				Vector3 dir = push_n_local;
				float lim = 0.0f;
				if (away.size() == 1) {
					const Vector3 e = m.pos[away[0]] - m.pos[v];
					const float en = e.normalized().dot(push_n_local);
					if (std::fabs(en) > 0.2f) {
						dir = e.normalized() / en;
						lim = en * e.length();
					}
				}
				push_dirs.push_back(dir);
				push_lims.push_back(lim);
			}
			push_dist = 0.0f;
			push_orig = m;
			push_sel = n->selection;
			// a convex block (faces may share a plane, ones that are not flat count as triangles): the face moves
			// as a plane, the faces around keep theirs
			push_planes = hewn::ops::convex_plane_mesh(m, push_face, push_source, push_plane_face);
			if (push_block) {
				// (the new block next to this object, the same transform and materials; seen while it is dragged)
				HewnMesh3D *blk = memnew(HewnMesh3D);
				blk->set_shape(hewn::shapes::CUSTOM);
				blk->set_materials(n->get_materials());
				String bname = n->get_name();
				if (bname.find("_block") > 0) {
					bname = bname.substr(0, bname.find("_block"));
				}
				blk->set_name(bname + "_block");
				n->get_parent()->add_child(blk, true);
				blk->set_transform(n->get_transform());
				blk->set_visible(false);
				push_block_node = blk->get_instance_id();
			} else if (uv_lock) {
				n->uv_lock_begin();
			}
			push_state = 2;
			return _face_push_input(p_cam, p_event);
		}
		if (mb.is_valid() && !mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
			push_state = 0;
			if (mode == MODE_OBJECT) {
				// Godot's Shift+click on a selected object: it leaves the selection
				if (Node *x = Object::cast_to<Node>(ObjectDB::get_instance(push_node)); x && !push_block && push_direct) {
					EditorInterface::get_singleton()->get_selection()->remove_node(x);
				}
			} else {
				if (n && n != get_edited() && is_edited_mesh(n)) {
					_set_active_mesh(n);
				}
				_click_select(p_cam, mb->get_position(), true, push_block, false, false); // (Ctrl+Shift+click: as before)
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return mm.is_valid() ? AFTER_GUI_INPUT_STOP : -1;
	}
	// Shift held: the face under the mouse outlined
	if (mm.is_valid()) {
		_face_push_hover(p_cam, mm->is_shift_pressed() && !mm->is_alt_pressed());
		return -1;
	}
	if (k.is_valid() && (key_code(k) == KEY_SHIFT || key_code(k) == KEY_CTRL)) {
		const bool shift = key_code(k) == KEY_SHIFT ? k->is_pressed() : k->is_shift_pressed();
		_face_push_hover(p_cam, shift && !k->is_alt_pressed());
		return -1;
	}
	if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_LEFT && mb->is_shift_pressed() && !mb->is_alt_pressed() &&
			!mb->is_meta_pressed() && !mb->is_double_click() && modal == MODAL_NONE && drag == DRAG_NONE && !vpaint) {
		ObjectID node;
		int face = -1;
		Vector3 hit;
		// (the gizmo's handles keep their Shift: Shift + drag on the move handle extrudes)
		if ((mode == MODE_OBJECT || _handle_hit(p_cam, mb->get_position()) < 0) && _face_push_pick(p_cam, mb->get_position(), node, face, hit)) {
			push_node = node;
			push_face = face;
			push_origin = hit;
			push_press = mb->get_position();
			{
				// (taken by the outline, off the mesh: a click there is no click on the object)
				Vector3 o, d;
				_ray(p_cam, mb->get_position(), o, d);
				float t;
				HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(node));
				push_direct = pm && raycast_node(pm, o, d, t, nullptr, false) >= 0;
			}
			push_block = mb->is_ctrl_pressed() || mb->is_meta_pressed(); // Ctrl+Shift: a new block on the face
			push_state = 1;
			return AFTER_GUI_INPUT_STOP;
		}
	}
	return -1;
}

// The outline of the face Shift would push (on top of everything), and the distance while it moves.
void HewnEditorPlugin::_draw_face_push(Control *p_overlay, Camera3D *p_cam) {
	const bool dragging = push_state == 2;
	HewnMesh3D *n = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(dragging ? push_node : push_hover_node));
	const int f = dragging ? push_face : push_hover_face;
	if (!n || !n->is_inside_tree() || f < 0 || f >= (int)n->get_data().faces.size()) {
		return;
	}
	if (!dragging && !Input::get_singleton()->is_key_pressed(KEY_SHIFT)) {
		return;
	}
	const hewn::Mesh &m = n->get_data();
	const hewn::Face &F = m.faces[f];
	const Transform3D g = n->get_global_transform();
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const Color yellow(1.0f, 0.85f, 0.1f, 1.0f);
	for (int i = 0; i < F.size(); i++) {
		const Vector3 a = g.xform(m.pos[F.v[i]]), b = g.xform(m.pos[F.v[(i + 1) % F.size()]]);
		line3d(p_overlay, p_cam, a, b, yellow, 2.0f * es);
	}
	if (dragging) {
		draw_header(p_overlay, p_overlay->get_theme_default_font(), vformat(push_split ? T("Split Block  D: %.4f") : T("Move Face  D: %.4f"), push_dist));
	}
}
