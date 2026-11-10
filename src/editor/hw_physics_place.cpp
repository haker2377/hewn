// Physics placement: the selected objects fall and tumble onto what is around them (a space of their own, the scene's
// meshes as static ground); held with the left button they are dragged around, pushing the others. Apply keeps where
// they came to rest (one undo step), Cancel puts them back (its panel, bottom left; Enter / Esc too).
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/csg_shape3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/world3d.hpp>

#include <algorithm>

using namespace godot;
using namespace hw_editor;

namespace {

// a node's points (its meshes and the ones under it), in world space
void node_points(Node *p_node, std::vector<Vector3> &r_out) {
	std::vector<Node *> stack = { p_node };
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
		if (HewnMesh3D *hm = Object::cast_to<HewnMesh3D>(x)) {
			if (!hm->is_visible_in_tree()) {
				continue;
			}
			const Transform3D g = hm->get_global_transform();
			const hewn::Mesh &m = hm->get_data();
			const std::vector<char> used = visible_verts(m);
			for (size_t v = 0; v < m.pos.size(); v++) {
				if (used[v]) {
					r_out.push_back(g.xform(m.pos[v]));
				}
			}
			continue;
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(x);
		if (mi && mi->is_visible_in_tree() && mi->get_mesh().is_valid()) {
			const Transform3D g = mi->get_global_transform();
			const PackedVector3Array f = mi->get_mesh()->get_faces();
			for (int64_t i = 0; i < f.size(); i++) {
				r_out.push_back(g.xform(f[i]));
			}
		}
	}
}

} // namespace

void HewnEditorPlugin::start_physics() {
	if (phys_on) {
		return;
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	Node3D *root3 = Object::cast_to<Node3D>(root);
	if (!root || !root3 || !root3->is_inside_tree()) {
		return;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	std::vector<Node *> moving;
	phys_items.clear();
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	for (int64_t i = 0; i < sel.size(); i++) {
		Node3D *n = Object::cast_to<Node3D>(sel[i]);
		if (!n || n == root || !n->is_inside_tree()) {
			continue;
		}
		std::vector<Vector3> pts;
		node_points(n, pts);
		if (pts.size() < 4) {
			continue; // (no mesh: nothing to fall)
		}
		moving.push_back(n);
		PhysItem it;
		it.node = ObjectID(n->get_instance_id());
		it.start = n->get_global_transform();
		it.scale = it.start.basis.get_scale();
		phys_items.push_back(it);
	}
	if (phys_items.empty()) {
		if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
			t->push_toast(T("Physics: select objects with a mesh first"));
		}
		return;
	}
	phys_space = ps->space_create();
	ps->space_set_active(phys_space, true);
	ps->area_set_param(phys_space, PhysicsServer3D::AREA_PARAM_GRAVITY, (float)op_param("physics", "gravity"));
	ps->area_set_param(phys_space, PhysicsServer3D::AREA_PARAM_GRAVITY_VECTOR, Vector3(0, -1, 0));
	// the moving objects: a convex hull each (around their origin, without their scale: bodies are not scaled)
	AABB around;
	bool first = true;
	for (PhysItem &it : phys_items) {
		Node3D *n = Object::cast_to<Node3D>(ObjectDB::get_instance(it.node));
		std::vector<Vector3> pts;
		node_points(n, pts);
		const Transform3D body_xf(it.start.basis.orthonormalized(), it.start.origin);
		const Transform3D inv = body_xf.affine_inverse();
		PackedVector3Array local;
		const size_t step = pts.size() / 3000 + 1;
		for (size_t i = 0; i < pts.size(); i += step) {
			local.push_back(inv.xform(pts[i]));
			around = first ? AABB(pts[i], Vector3()) : around.expand(pts[i]);
			first = false;
		}
		const RID shape = ps->convex_polygon_shape_create();
		ps->shape_set_data(shape, local);
		const RID body = ps->body_create();
		ps->body_set_mode(body, PhysicsServer3D::BODY_MODE_RIGID);
		ps->body_set_space(body, phys_space);
		ps->body_add_shape(body, shape);
		ps->body_set_param(body, PhysicsServer3D::BODY_PARAM_MASS, 1.0f);
		ps->body_set_param(body, PhysicsServer3D::BODY_PARAM_FRICTION, 0.8f);
		ps->body_set_state(body, PhysicsServer3D::BODY_STATE_TRANSFORM, body_xf);
		it.body = body;
		it.shape = shape;
	}
	// the ground: every other mesh near them, as it is shown (one static body of triangles each)
	const AABB reach = around.grow(MAX(20.0f, around.get_longest_axis_size() * 4.0f));
	std::vector<Node *> stack = { root };
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		bool skip = false;
		for (Node *m : moving) {
			skip = skip || m == x || m->is_ancestor_of(x);
		}
		if (skip) {
			continue;
		}
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
		Ref<Mesh> mesh;
		Transform3D g;
		if (HewnMesh3D *hm = Object::cast_to<HewnMesh3D>(x)) {
			if (hm->is_brush() || hm->is_boolean_cutter()) {
				continue;
			}
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(x);
		CSGShape3D *csg = Object::cast_to<CSGShape3D>(x);
		if (mi && mi->is_visible_in_tree() && mi->get_mesh().is_valid()) {
			mesh = mi->get_mesh();
			g = mi->get_global_transform();
		} else if (csg && csg->is_root_shape() && csg->is_visible_in_tree()) {
			const Array am = csg->get_meshes();
			if (am.size() >= 2 && Object::cast_to<Mesh>(am[1])) {
				mesh = Ref<Mesh>(am[1]);
				g = csg->get_global_transform() * Transform3D(am[0]);
			}
		}
		if (mesh.is_null() || !g.xform(mesh->get_aabb()).intersects(reach)) {
			continue;
		}
		const PackedVector3Array f = mesh->get_faces();
		if (f.size() < 3) {
			continue;
		}
		PackedVector3Array wf;
		wf.resize(f.size());
		for (int64_t i = 0; i < f.size(); i++) {
			wf[i] = g.xform(f[i]);
		}
		Dictionary data;
		data["faces"] = wf;
		data["backface_collision"] = true;
		const RID shape = ps->concave_polygon_shape_create();
		ps->shape_set_data(shape, data);
		const RID body = ps->body_create();
		ps->body_set_mode(body, PhysicsServer3D::BODY_MODE_STATIC);
		ps->body_set_space(body, phys_space);
		ps->body_add_shape(body, shape);
		phys_statics.push_back(body);
		phys_static_shapes.push_back(shape);
	}
	// the server steps only while it is active (the editor keeps it off): on, the scene's own world held still
	phys_world = root3->get_world_3d().is_valid() ? root3->get_world_3d()->get_space() : RID();
	phys_world_was = phys_world.is_valid() && ps->space_is_active(phys_world);
	if (phys_world.is_valid()) {
		ps->space_set_active(phys_world, false);
	}
	ps->set_active(true);
	phys_on = true;
	phys_grab = -1;
	// (the objects unselected while they fall: no gizmo, no outline; their origins drawn; selected again after)
	EditorInterface::get_singleton()->get_selection()->clear();
	_hide_shape_panel();
	if (obj_panel) {
		obj_panel->hide();
	}
	_hide_redo();
	last_op = "physics";
	_show_redo();
	update_overlays();
}

void HewnEditorPlugin::_physics_stop(bool p_apply) {
	if (!phys_on) {
		return;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	std::vector<std::pair<Node3D *, Transform3D>> after;
	for (const PhysItem &it : phys_items) {
		if (Node3D *n = Object::cast_to<Node3D>(ObjectDB::get_instance(it.node))) {
			after.push_back({ n, n->get_global_transform() });
			n->set_global_transform(it.start); // (as it was: the undo step moves it)
		}
	}
	for (const PhysItem &it : phys_items) {
		ps->free_rid(it.body);
		ps->free_rid(it.shape);
	}
	for (const RID &b : phys_statics) {
		ps->free_rid(b);
	}
	for (const RID &s : phys_static_shapes) {
		ps->free_rid(s);
	}
	ps->free_rid(phys_space);
	phys_statics.clear();
	phys_static_shapes.clear();
	ps->set_active(false);
	if (phys_world.is_valid() && phys_world_was) {
		ps->space_set_active(phys_world, true);
	}
	phys_on = false;
	phys_grab = -1;
	if (p_apply && !after.empty()) {
		EditorUndoRedoManager *ur = get_undo_redo();
		ur->create_action("Hewn: Physics Placement", UndoRedo::MERGE_DISABLE, EditorInterface::get_singleton()->get_edited_scene_root());
		for (size_t i = 0; i < after.size(); i++) {
			ur->add_do_method(after[i].first, "set_global_transform", after[i].second);
			ur->add_undo_method(after[i].first, "set_global_transform", phys_items[i].start);
		}
		ur->commit_action();
	}
	EditorSelection *es = EditorInterface::get_singleton()->get_selection();
	es->clear();
	for (const PhysItem &it : phys_items) {
		if (Node *sn = Object::cast_to<Node>(ObjectDB::get_instance(it.node))) {
			es->add_node(sn);
		}
	}
	phys_items.clear();
	if (last_op == "physics") {
		_hide_redo();
		last_op = String();
	}
	update_overlays();
}

// Each frame: the objects where their bodies are (their scale back on); the one held pulled to the mouse.
void HewnEditorPlugin::_phys_step() {
	if (!phys_on) {
		return;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	Camera3D *cam = Object::cast_to<Camera3D>(ObjectDB::get_instance(phys_cam));
	for (int i = 0; i < (int)phys_items.size(); i++) {
		const PhysItem &it = phys_items[i];
		Node3D *n = Object::cast_to<Node3D>(ObjectDB::get_instance(it.node));
		if (!n || !n->is_inside_tree()) {
			continue;
		}
		const Transform3D bx = ps->body_get_state(it.body, PhysicsServer3D::BODY_STATE_TRANSFORM);
		if (i == phys_grab && cam) {
			const Vector3 target = cam->project_position(mouse_pos, phys_grab_depth);
			const Vector3 at = bx.xform(phys_grab_local);
			ps->body_set_state(it.body, PhysicsServer3D::BODY_STATE_SLEEPING, false);
			ps->body_set_state(it.body, PhysicsServer3D::BODY_STATE_LINEAR_VELOCITY, (target - at) * 12.0f);
			const Vector3 av = ps->body_get_state(it.body, PhysicsServer3D::BODY_STATE_ANGULAR_VELOCITY);
			ps->body_set_state(it.body, PhysicsServer3D::BODY_STATE_ANGULAR_VELOCITY, av * 0.85f);
		}
		n->set_global_transform(Transform3D(bx.basis * Basis::from_scale(it.scale), bx.origin));
	}
	update_overlays(); // (their origins drawn where they are now)
}

// The wheel while an object is held: it comes nearer / goes further (the view does not zoom); not held, the view zooms.
bool HewnEditorPlugin::_physics_wheel(const Ref<InputEventMouseButton> &p_mb) {
	if (!phys_on || phys_grab < 0 || p_mb.is_null() || (p_mb->get_button_index() != MOUSE_BUTTON_WHEEL_UP && p_mb->get_button_index() != MOUSE_BUTTON_WHEEL_DOWN)) {
		return false;
	}
	if (p_mb->is_pressed()) {
		const float k = p_mb->is_shift_pressed() ? 1.02f : 1.1f;
		// (the wheel away from you: further, toward you: nearer)
		phys_grab_depth = CLAMP(phys_grab_depth * (p_mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? k : 1.0f / k), 0.05f, 10000.0f);
	}
	return true;
}

void HewnEditorPlugin::_draw_physics(Control *p_overlay, Camera3D *p_cam) {
	if (!phys_on) {
		return;
	}
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	for (const PhysItem &it : phys_items) {
		Node3D *n = Object::cast_to<Node3D>(ObjectDB::get_instance(it.node));
		if (!n || !n->is_inside_tree() || p_cam->is_position_behind(n->get_global_position())) {
			continue;
		}
		const Vector2 c = p_cam->unproject_position(n->get_global_position());
		p_overlay->draw_circle(c, 4.0f * es, Color(0, 0, 0, 0.6f));
		p_overlay->draw_circle(c, 3.0f * es, Color(1.0f, 0.6f, 0.2f));
	}
}

int32_t HewnEditorPlugin::_physics_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	if (!phys_on) {
		return -1;
	}
	phys_cam = p_cam ? ObjectID(p_cam->get_instance_id()) : ObjectID();
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	if (k.is_valid() && k->is_pressed() && !k->is_echo()) {
		const Key code = key_code(k);
		if (code == KEY_ESCAPE) {
			_physics_stop(false);
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_ENTER || code == KEY_KP_ENTER) {
			_physics_stop(true);
			return AFTER_GUI_INPUT_STOP;
		}
		return -1;
	}
	if (mm.is_valid()) {
		mouse_pos = mm->get_position();
		return phys_grab >= 0 ? AFTER_GUI_INPUT_STOP : -1;
	}
	if (_physics_wheel(mb)) {
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		if (!mb->is_pressed()) {
			const bool was = phys_grab >= 0;
			phys_grab = -1;
			return was ? AFTER_GUI_INPUT_STOP : -1;
		}
		mouse_pos = mb->get_position();
		// the object under the mouse: the nearest hit of the moving ones
		Vector3 o, d;
		_ray(p_cam, mouse_pos, o, d);
		const std::vector<Node3D *> under = _objects_under(p_cam, mouse_pos);
		PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
		for (Node3D *u : under) {
			for (int i = 0; i < (int)phys_items.size(); i++) {
				Node3D *n = Object::cast_to<Node3D>(ObjectDB::get_instance(phys_items[i].node));
				if (n != u && !(n && n->is_ancestor_of(u))) {
					continue;
				}
				// (the point grabbed: where the ray meets its middle's depth)
				const Transform3D bx = ps->body_get_state(phys_items[i].body, PhysicsServer3D::BODY_STATE_TRANSFORM);
				const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2).normalized();
				phys_grab_depth = MAX(0.05f, (bx.origin - p_cam->get_global_position()).dot(fwd));
				phys_grab_local = bx.affine_inverse().xform(p_cam->project_position(mouse_pos, phys_grab_depth));
				phys_grab = i;
				return AFTER_GUI_INPUT_STOP;
			}
		}
		return AFTER_GUI_INPUT_STOP; // (not a selection click while it runs)
	}
	return -1;
}
