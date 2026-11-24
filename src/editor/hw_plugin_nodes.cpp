// Hewn editor - materials on the selection, nodes made with their undo.

#include "hw_plugin_impl.h"

// ------------------------------------------------------------------ materials

int HewnEditorPlugin::apply_material_to_selection(const Ref<Material> &p_material) {
	std::vector<HewnMesh3D *> targets;
	std::vector<PackedInt32Array> faces;
	std::vector<HewnCurve3D *> curves;
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
			} else if (HewnCurve3D *c = Object::cast_to<HewnCurve3D>(sel[i])) {
				curves.push_back(c);
			}
		}
		if (targets.empty() && curves.empty() && edited) {
			targets.push_back(edited);
		}
		for (HewnMesh3D *n : targets) {
			faces.push_back(PackedInt32Array()); // all of them
			count += n->get_face_count();
		}
		count += (int)curves.size();
	}
	if (targets.empty() && curves.empty()) {
		return -1;
	}
	_commit_material(p_material, targets, faces, curves);
	return count;
}

// The bottom panel's tabs: Textures, Scenes, UV (as they belong together), whatever the layout had.
void HewnEditorPlugin::_order_bottom_docks() {
	Control *order[4] = { texture_dock, prefab_dock, uv_dock, hotspot_dock };
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
		if (node->has_meta("_hw_curve_mesh")) {
			// (a curve's geometry: the curve takes the material)
			if (HewnCurve3D *hc = Object::cast_to<HewnCurve3D>(node->get_parent())) {
				_commit_material(p_material, {}, {}, { hc });
				return 1;
			}
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

void HewnEditorPlugin::_commit_material(const Ref<Material> &p_material, const std::vector<HewnMesh3D *> &targets, const std::vector<PackedInt32Array> &faces, const std::vector<HewnCurve3D *> &curves) {
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Apply Material", UndoRedo::MERGE_DISABLE, targets.empty() ? (Object *)curves[0] : (Object *)targets[0]);
	for (HewnCurve3D *c : curves) {
		ur->add_do_method(c, "set_material", p_material);
		ur->add_undo_method(c, "set_material", c->get_material());
		c->set_material(p_material);
	}
	for (size_t i = 0; i < targets.size(); i++) {
		HewnMesh3D *n = targets[i];
		const TypedArray<Material> mats_before = n->get_materials().duplicate();
		const Dictionary before = n->get_state();
		n->apply_material(p_material, faces[i]);
		ur->add_do_method(n, "set_materials", n->get_materials().duplicate());
		hw_undo_state(ur, true, n, n->get_state());
		ur->add_undo_method(n, "set_materials", mats_before);
		hw_undo_state(ur, false, n, before);
	}
	ur->commit_action(false);
	dock->refresh();
}

// Bake to MeshInstance3D: the selected meshes and curves (or the mesh edited) in one MeshInstance3D where the first of
// them is, their surfaces of one material joined; they are hidden (one undo step).
void HewnEditorPlugin::_bake_to_mesh_instance() {
	EditorInterface *ei = EditorInterface::get_singleton();
	std::vector<Node3D *> list;
	const TypedArray<Node> sel = ei->get_selection()->get_top_selected_nodes();
	for (int64_t i = 0; i < sel.size(); i++) {
		Node3D *x = Object::cast_to<Node3D>(sel[i]);
		if ((Object::cast_to<HewnMesh3D>(x) || Object::cast_to<HewnCurve3D>(x)) && x->is_visible_in_tree()) {
			list.push_back(x);
		}
	}
	if (list.empty()) {
		if (HewnMesh3D *e = get_edited()) {
			list.push_back(e);
		}
	}
	Node *root = ei->get_edited_scene_root();
	if (list.empty() || !root || !list[0]->get_parent()) {
		return;
	}
	Node3D *first = list[0];
	const Transform3D to_first = first->get_global_transform().affine_inverse();
	// (by material: one surface each)
	std::vector<Ref<Material>> mats;
	std::vector<Ref<SurfaceTool>> tools;
	for (Node3D *x : list) {
		Ref<ArrayMesh> am;
		if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(x)) {
			am = m->to_array_mesh();
		} else if (HewnCurve3D *c = Object::cast_to<HewnCurve3D>(x)) {
			am = c->get_baked_mesh();
		}
		if (am.is_null()) {
			continue;
		}
		const Transform3D rel = to_first * x->get_global_transform();
		for (int s = 0; s < am->get_surface_count(); s++) {
			const Ref<Material> mat = am->surface_get_material(s);
			size_t k = 0;
			while (k < mats.size() && mats[k] != mat) {
				k++;
			}
			if (k == mats.size()) {
				mats.push_back(mat);
				Ref<SurfaceTool> st;
				st.instantiate();
				tools.push_back(st);
			}
			tools[k]->append_from(am, s, rel);
		}
	}
	Ref<ArrayMesh> out;
	out.instantiate();
	for (size_t k = 0; k < tools.size(); k++) {
		tools[k]->commit(out);
		out->surface_set_material(out->get_surface_count() - 1, mats[k]);
	}
	if (out->get_surface_count() == 0) {
		return;
	}
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_name(list.size() == 1 ? String(first->get_name()) + "_Baked" : String("Baked"));
	mi->set_mesh(out);
	mi->set_transform(first->get_transform());
	Node *parent = first->get_parent();
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Bake to MeshInstance3D");
	ur->add_do_method(parent, "add_child", mi, true);
	ur->add_do_method(mi, "set_owner", root);
	for (Node3D *x : list) {
		ur->add_do_method(x, "set_visible", false);
		ur->add_undo_method(x, "set_visible", x->is_visible());
	}
	ur->add_do_reference(mi);
	ur->add_undo_method(parent, "remove_child", mi);
	ur->commit_action();
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
		while (s && s != root && Object::cast_to<Path3D>(s)) {
			s = s->get_parent(); // (a curve holds only the shapes following it)
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
	std::vector<Node3D *> others;
	const TypedArray<Node> gsel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	for (int64_t i = 0; i < gsel.size(); i++) {
		Node3D *g = Object::cast_to<Node3D>(gsel[i]);
		if (Object::cast_to<HewnCurve3D>(g) || Object::cast_to<HewnOverlay3D>(g)) {
			others.push_back(g);
		}
	}
	const bool fbx = p_path.get_extension().to_lower() == "fbx";
	if (meshes.size() == 1 && others.empty() && !fbx) {
		if (meshes[0]->export_gltf(p_path) != OK) {
			ERR_PRINT("Hewn: glTF export failed: " + p_path);
		}
		return;
	}
	if (meshes.empty() && others.empty()) {
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
	for (Node3D *g : others) {
		Node3D *copy = Object::cast_to<Node3D>(HewnLevel::game_copy(g));
		if (!copy) {
			continue;
		}
		copy->set_name(g->get_name());
		copy->set_transform(g->get_global_transform());
		root->add_child(copy, true);
		std::vector<Node *> stack = { copy };
		while (!stack.empty()) {
			Node *x = stack.back();
			stack.pop_back();
			x->set_owner(root);
			for (int i = 0; i < x->get_child_count(); i++) {
				stack.push_back(x->get_child(i));
			}
		}
	}
	if (fbx) {
		const Error ferr = hewn::write_fbx(root, p_path);
		memdelete(root);
		if (ferr != OK) {
			ERR_PRINT("Hewn: FBX export failed: " + p_path);
		}
		return;
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
