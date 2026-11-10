// Menus: right-click context menu per element mode, X delete menu, P separate, M merge,
// mirror; and the "value" modal operations (vertex bevel, push/pull, shrink/fatten, shear, offset edge slide).
#include "hw_editor.h"
#include "hw_editor_util.h"
#include "hw_prefabs.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/style_box.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/random_number_generator.hpp>

using namespace godot;
using namespace hw_editor;

// ------------------------------------------------------------------ menus

void HewnEditorPlugin::_show_menu(Camera3D *p_cam, const String &p_title, const std::vector<std::pair<String, String>> &p_items) {
	if (!op_menu) {
		op_menu = memnew(PopupMenu);
		op_menu->connect("id_pressed", callable_mp(this, &HewnEditorPlugin::_on_menu_id));
		op_menu->connect("popup_hide", callable_mp(this, &HewnEditorPlugin::_on_menu_hidden));
		EditorInterface::get_singleton()->get_base_control()->add_child(op_menu);
	}
	menu_cam_id = p_cam ? p_cam->get_instance_id() : 0;
	menu_title = p_title;
	op_menu->clear();
	op_menu->set_hide_on_checkable_item_selection(false); // (a check item stays open: pick what to add next)
	menu_ops.clear();
	for (int i = op_menu->get_child_count() - 1; i >= 0; i--) {
		if (PopupMenu *old = Object::cast_to<PopupMenu>(op_menu->get_child(i))) {
			op_menu->remove_child(old);
			old->queue_free();
		}
	}
	if (!p_title.is_empty()) {
		op_menu->add_separator(p_title);
	}
	const String last = menu_last.get(p_title, String());
	int last_index = -1;
	auto fill = [&](PopupMenu *p_menu, const std::vector<std::pair<String, String>> &p_list) {
		for (const std::pair<String, String> &it : p_list) {
			if (it.second.is_empty()) {
				p_menu->add_separator();
				continue;
			}
			if (p_menu == op_menu && it.second == last) {
				last_index = p_menu->get_item_count();
			}
			if (it.second.begins_with("check:")) {
				p_menu->add_check_item(it.first, (int)menu_ops.size());
				p_menu->set_item_checked(p_menu->get_item_count() - 1, _menu_check(it.second.substr(6)));
			} else {
				p_menu->add_item(it.first, (int)menu_ops.size());
			}
			if (it.second.begins_with("shape:") && dock) {
				p_menu->set_item_icon(p_menu->get_item_count() - 1, dock->shape_icon(it.second.substr(6).to_int()));
			}
			menu_ops.push_back(it.second);
		}
	};
	for (const std::pair<String, String> &it : p_items) {
		if (it.second.is_empty() && !it.first.is_empty()) {
			continue; // (a category with nothing in it)
		}
		if (it.second.begins_with("sub:")) {
			// a category: its items open beside it on hover
			const auto found = menu_subs.find(it.second.substr(4));
			if (found == menu_subs.end()) {
				continue;
			}
			PopupMenu *sm = memnew(PopupMenu);
			sm->set_hide_on_checkable_item_selection(false);
			sm->connect("id_pressed", callable_mp(this, &HewnEditorPlugin::_on_menu_id));
			op_menu->add_child(sm);
			fill(sm, found->second);
			op_menu->add_submenu_node_item(it.first, sm);
			continue;
		}
		fill(op_menu, { it });
	}
	if (last_index < 0) {
		last_index = p_title.is_empty() ? 0 : 1; // first item
	}
	localize(op_menu);
	// the menu opens with the item used last time under the cursor. Item heights follow
	// PopupMenu's own layout (text / separator height + v_separation).
	const int vsep = op_menu->get_theme_constant("v_separation");
	Ref<Font> font = op_menu->get_theme_font("font");
	const int fsize = op_menu->get_theme_font_size("font_size");
	Ref<Font> sfont = op_menu->get_theme_font("font_separator");
	const int sfsize = op_menu->get_theme_font_size("font_separator_size");
	Ref<StyleBox> panel = op_menu->get_theme_stylebox("panel");
	Ref<StyleBox> sep = op_menu->get_theme_stylebox("separator");
	const float text_h = font.is_valid() ? font->get_height(fsize) : 16.0f;
	const float sep_h = sep.is_valid() ? sep->get_minimum_size().y : 4.0f;
	const float label_h = sfont.is_valid() ? sfont->get_height(sfsize) : text_h;
	// Items are stacked with v_separation between them; the text is centered in its own height.
	const float top = panel.is_valid() ? panel->get_margin(SIDE_TOP) : 4.0f;
	float y = top;
	float center = -1.0f;
	for (int i = 0; i < op_menu->get_item_count(); i++) {
		float h = text_h;
		if (op_menu->is_item_separator(i)) {
			h = op_menu->get_item_text(i).is_empty() ? sep_h : MAX(label_h, sep_h);
		}
		if (i == last_index) {
			center = y + h * 0.5f;
		}
		y += h + (i + 1 < op_menu->get_item_count() ? (float)vsep : 0.0f);
	}
	const float computed = y + (panel.is_valid() ? panel->get_margin(SIDE_BOTTOM) : 4.0f);
	Control *surface = _surface_for(p_cam);
	Vector2 cursor = surface ? surface->get_screen_position() + mouse_pos : Vector2();
	const bool at_point = menu_popup_at.x >= 0.0f; // (under a button: its top left, no item under the cursor)
	if (at_point) {
		cursor = menu_popup_at;
		menu_popup_at = Vector2(-1, -1);
		center = 0.0f;
	}
	op_menu->reset_size();
	// The real height calibrates the estimate (theme scale, fonts measured differently...).
	const float real = op_menu->get_contents_minimum_size().y;
	if (real > 0.0f && computed > 0.0f && real / computed > 0.75f && real / computed < 1.33f) {
		center *= real / computed;
	}
	Vector2 at = at_point ? cursor : cursor - Vector2(40.0f, MAX(0.0f, center));
	// keep it on the screen the cursor is on
	const int screen = DisplayServer::get_singleton()->get_screen_from_rect(Rect2(cursor, Vector2(1, 1)));
	const Rect2 usable = DisplayServer::get_singleton()->screen_get_usable_rect(screen);
	const Vector2 size = op_menu->get_size();
	at.x = CLAMP(at.x, usable.position.x, MAX(usable.position.x, usable.position.x + usable.size.x - size.x));
	at.y = CLAMP(at.y, usable.position.y, MAX(usable.position.y, usable.position.y + usable.size.y - size.y));
	op_menu->popup(Rect2i(Vector2i(at), Vector2i()));
}

void HewnEditorPlugin::_context_menu_deferred(uint64_t p_cam_id) {
	Camera3D *cam = Object::cast_to<Camera3D>(ObjectDB::get_instance(p_cam_id));
	bool curve_sel = false;
	for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
		curve_sel = curve_sel || Object::cast_to<HewnCurve3D>(v) != nullptr;
	}
	if (cam && mode == MODE_OBJECT && modal == MODAL_NONE && curve_sel && !hc_edit) {
		_curve_object_menu(cam);
	} else if (cam && mode == MODE_OBJECT && modal == MODAL_NONE) {
		_object_menu(cam);
	} else if (cam && mode != MODE_OBJECT && modal == MODAL_NONE) {
		_context_menu(cam);
	}
}

// Right click in object mode on the selected Hewn meshes.
void HewnEditorPlugin::_object_menu(Camera3D *p_cam) {
	if (_selected_meshes().empty()) {
		return;
	}
	// Object > Clear (Alt+G / R / S) and Object > Apply (Ctrl+A)
	menu_subs["obj_clear"] = { { "Location", "obj:clear_loc" }, { "Rotation", "obj:clear_rot" }, { "Scale", "obj:clear_scale" }, { "Origin", "obj:clear_origin" } };
	menu_subs["obj_apply"] = { { "Location", "obj:apply_loc" }, { "Rotation", "obj:apply_rot" }, { "Scale", "obj:apply_scale" }, { "All Transforms", "obj:freeze" },
		{ "Rotation & Scale", "obj:apply_rotscale" } };
	_show_menu(p_cam, "Object Context Menu",
			{ { "Edit Mode  Tab", "obj:edit" }, { "", "" }, { "Shade Smooth", "obj:smooth" }, { "Shade Flat", "obj:flat" }, { "Flip Normals", "obj:flip" }, { "", "" },
					{ "Origin to 3D Cursor", "obj:origin_cursor" }, { "Origin to Geometry", "obj:origin_geometry" }, { "Origin to Bottom", "obj:origin_bottom" },
					{ "Cursor to Selected", "obj:cursor_to_selected" }, { "", "" }, { "Move to Floor / Ceiling", "scene:to_floor" }, { "Physics Placement", "obj:physics" }, { "Convert to Collision", "scene:to_collision" },
					{ "", "" }, { "Clear", "sub:obj_clear" }, { "Apply", "sub:obj_apply" }, { "", "" }, { "Duplicate", "obj:duplicate" }, { "Delete", "obj:delete" } });
}

std::vector<HewnMesh3D *> HewnEditorPlugin::_selected_meshes() const {
	std::vector<HewnMesh3D *> out;
	for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_selected_nodes()) {
		if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(v)) {
			out.push_back(m);
		}
	}
	return out;
}

void HewnEditorPlugin::_run_object_op(const String &p_op) {
	EditorUndoRedoManager *ur = get_undo_redo();
	if (p_op == "physics") {
		start_physics();
		return;
	}
	if (p_op == "clear_loc" || p_op == "clear_rot" || p_op == "clear_scale") {
		// every selected 3D node (not only meshes): its position to 0, its rotation to none, its scale to 1
		std::vector<Node3D *> nodes;
		for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
			if (Node3D *n3 = Object::cast_to<Node3D>(v)) {
				nodes.push_back(n3);
			}
		}
		if (nodes.empty()) {
			return;
		}
		ur->create_action("Hewn: Clear " + String(p_op == "clear_loc" ? "Location" : p_op == "clear_rot" ? "Rotation" : "Scale"));
		for (Node3D *n3 : nodes) {
			const Transform3D before = n3->get_transform();
			Transform3D after = before;
			const Vector3 sc = before.basis.get_scale();
			if (p_op == "clear_loc") {
				after.origin = Vector3();
			} else if (p_op == "clear_rot") {
				after.basis = Basis::from_scale(sc);
			} else {
				after.basis = before.basis.orthonormalized();
				if (before.basis.determinant() < 0.0f) {
					after.basis = Basis(before.basis.get_rotation_quaternion()); // (a mirrored one: unmirrored too)
				}
			}
			ur->add_do_method(n3, "set_transform", after);
			ur->add_undo_method(n3, "set_transform", before);
		}
		ur->commit_action();
		update_overlays();
		return;
	}
	const std::vector<HewnMesh3D *> meshes = _selected_meshes();
	if (meshes.empty()) {
		return;
	}
	if (p_op == "edit") {
		set_mode(MODE_FACE);
		return;
	}
	if (p_op == "cursor_to_selected") {
		Vector3 c;
		for (HewnMesh3D *m : meshes) {
			c += m->get_global_transform().origin;
		}
		cursor_pos = c / (float)meshes.size();
		update_overlays();
		return;
	}
	if (p_op == "delete") {
		ur->create_action("Hewn: Delete");
		for (HewnMesh3D *m : meshes) {
			Node *parent = m->get_parent();
			ur->add_do_method(parent, "remove_child", m);
			ur->add_undo_method(parent, "add_child", m, true);
			ur->add_undo_method(parent, "move_child", m, m->get_index());
			ur->add_undo_method(m, "set_owner", m->get_owner());
			ur->add_undo_reference(m);
		}
		ur->commit_action();
		return;
	}
	if (p_op == "duplicate") {
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		EditorSelection *es = EditorInterface::get_singleton()->get_selection();
		std::vector<Node *> copies;
		ur->create_action("Hewn: Duplicate");
		for (HewnMesh3D *m : meshes) {
			Node *c = m->duplicate();
			Node *parent = m->get_parent();
			ur->add_do_method(parent, "add_child", c, true);
			ur->add_do_method(c, "set_owner", root);
			ur->add_do_reference(c);
			ur->add_undo_method(parent, "remove_child", c);
			copies.push_back(c);
		}
		ur->commit_action();
		es->clear();
		for (Node *c : copies) {
			es->add_node(c);
		}
		return;
	}
	ur->create_action("Hewn: " + p_op.capitalize());
	for (HewnMesh3D *m : meshes) {
		const Transform3D xf_before = m->get_transform();
		const Dictionary before = m->get_state();
		PackedInt32Array all;
		for (int f = 0; f < m->get_face_count(); f++) {
			all.push_back(f);
		}
		if (p_op == "smooth" || p_op == "flat") {
			hewn::Mesh &d = m->get_data();
			for (hewn::Face &F : d.faces) {
				F.smooth = p_op == "smooth" ? 1 : 0;
			}
			m->commit(false);
		} else if (p_op == "flip") {
			m->flip_faces(all);
		} else if (p_op == "freeze") {
			m->freeze_transform();
		} else if (p_op == "apply_loc" || p_op == "apply_rot" || p_op == "apply_scale" || p_op == "apply_rotscale" || p_op == "clear_origin") {
			// Apply: that part of the transform goes into the vertices, the object shows the same.
			// Clear Origin: the origin onto the parent's (the scene's) zero, the geometry stays where it is.
			const Transform3D X = m->get_transform();
			const Vector3 sc = X.basis.get_scale();
			const Basis R = X.basis.orthonormalized();
			Transform3D N = X;
			if (p_op == "apply_loc" || p_op == "clear_origin") {
				N.origin = Vector3();
			} else if (p_op == "apply_rot") {
				N.basis = Basis::from_scale(sc);
			} else if (p_op == "apply_scale") {
				N.basis = R;
			} else {
				N.basis = Basis();
			}
			const Transform3D M = N.affine_inverse() * X;
			hewn::Mesh &d = m->get_data();
			for (Vector3 &p : d.pos) {
				p = M.xform(p);
			}
			if (M.basis.determinant() < 0.0f) {
				for (hewn::Face &F : d.faces) {
					F.reverse(); // (mirrored: the faces would look inside)
				}
			}
			m->set_transform(N);
			m->commit(true);
		} else if (p_op == "origin_cursor") {
			m->set_pivot(m->get_global_transform().affine_inverse().xform(cursor_pos));
		} else if (p_op == "origin_geometry") {
			m->center_pivot();
		} else if (p_op == "origin_bottom") {
			const hewn::Mesh &d = m->get_data();
			if (d.pos.empty()) {
				continue;
			}
			Vector3 mn = d.pos[0], mx = d.pos[0];
			for (const Vector3 &p : d.pos) {
				mn = mn.min(p);
				mx = mx.max(p);
			}
			m->set_pivot(Vector3((mn.x + mx.x) * 0.5f, mn.y, (mn.z + mx.z) * 0.5f));
		}
		ur->add_do_method(m, "set_transform", m->get_transform());
		ur->add_do_method(m, "set_state", m->get_state());
		ur->add_undo_method(m, "set_transform", xf_before);
		ur->add_undo_method(m, "set_state", before);
	}
	ur->commit_action(false);
	selection_changed();
	update_overlays();
}

void HewnEditorPlugin::_on_menu_id(int p_id) {
	if (p_id < 0 || p_id >= (int)menu_ops.size()) {
		return;
	}
	if (menu_ops[p_id].begins_with("check:")) {
		const String key = menu_ops[p_id].substr(6);
		_menu_toggle(key);
		std::vector<PopupMenu *> menus = { op_menu };
		for (int i = 0; i < op_menu->get_child_count(); i++) {
			if (PopupMenu *sm = Object::cast_to<PopupMenu>(op_menu->get_child(i))) {
				menus.push_back(sm); // (a check in a category)
			}
		}
		for (PopupMenu *pm : menus) {
			const int idx = pm->get_item_index(p_id);
			if (idx >= 0) {
				pm->set_item_checked(idx, _menu_check(key));
			}
		}
		return;
	}
	menu_last[menu_title] = menu_ops[p_id];
	Camera3D *cam = menu_cam_id ? Object::cast_to<Camera3D>(ObjectDB::get_instance(menu_cam_id)) : nullptr;
	_run_menu_op(cam, menu_ops[p_id]);
}

// Right click (short, without dragging): context menu of the current element mode.
void HewnEditorPlugin::_context_menu(Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	std::vector<std::pair<String, String>> items;
	if (mode == MODE_FACE) {
		items = { { "Face Cutter", "face_cutter" }, { "", "" }, { "Subdivide", "subdivide_faces" }, { "Extrude Faces  E", "modal:extrude" }, { "Extrude Faces Along Normals", "modal:extrude_normals" },
			{ "Extrude Individual Faces", "modal:extrude_individual" }, { "Extrude Repeat", "extrude_repeat" }, { "Inset Faces  I", "modal:inset" }, { "Spin", "spin_tool" }, { "To Circle", "to_circle" }, { "", "" }, { "Poke Faces", "poke" }, { "Duplicate  Shift+D", "modal:duplicate" } };
		if (n->selection.faces.size() >= 2) {
			items.push_back({ "Bridge Faces", "bridge_faces" });
		}
		std::vector<std::pair<String, String>> rest = { { "Triangulate Faces", "triangulate" }, { "Tris to Quads", "tris_to_quads" }, { "", "" },
			{ "Shade Smooth", "smooth_faces" }, { "Shade Flat", "flat_faces" }, { "Set Vertex Color", "set_vertex_color" }, { "", "" }, { "Split", "detach" }, { "Separate  P...", "menu:separate" }, { "", "" },
			{ "Dissolve Faces", "dissolve" }, { "Delete Faces", "delete_faces" } };
		items.insert(items.end(), rest.begin(), rest.end());
	} else if (mode == MODE_EDGE) {
		menu_subs["from_edges"] = { { "Sweep", "sweep_edges" }, { "Wall", "wall_edges" }, { "", "" }, { "Bezier Curve", "curve_edges_bezier" }, { "NURBS Curve", "curve_edges_nurbs" },
			{ "Poly Curve", "curve_edges_poly" } };
		items = { { "Subdivide", "subdivide_edges" }, { "Extrude Edges", "modal:extrude" }, { "Bevel Edges  Ctrl+B", "modal:bevel" }, { "Create from Edges", "sub:from_edges" },
			{ "New Face from Edges  F", "make_face" }, { "Spin", "spin_tool" }, { "", "" }, { "Loop Cut and Slide  Ctrl+R", "loop_cut_tool" },
			{ "Offset Edge Slide  Ctrl+Shift+R", "modal:offset_edge_slide" }, { "To Circle", "to_circle" }, { "", "" }, { "Edge Bevel Weight", "bevel_weight" }, { "Edge Crease  Shift+E", "edge_crease" },
			{ "", "" }, { "Mark Seam", "mark_seam" }, { "Clear Seam", "clear_seam" }, { "Mark Sharp", "mark_sharp" }, { "Clear Sharp", "clear_sharp" },
			{ "Set Sharpness by Angle", "sharp_by_angle" }, { "", "" }, { "Shade Smooth", "smooth_faces" }, { "Shade Flat", "flat_faces" }, { "Set Vertex Color", "set_vertex_color" }, { "", "" },
			{ "Split", "split_edges" }, { "Separate  P...", "menu:separate" }, { "", "" },
			{ "Dissolve Edges", "dissolve" }, { "Delete Edges", "delete_edges" } };
	} else {
		items = { { "Subdivide", "subdivide_edges" }, { "Extrude Vertices", "modal:extrude" }, { "Bevel Vertices  Ctrl+Shift+B", "modal:bevel_vertices" }, { "", "" },
			{ "Push/Pull", "modal:push_pull" }, { "Shrink/Fatten  Alt+S", "modal:shrink_fatten" }, { "Shear  Ctrl+Shift+Alt+S", "modal:shear" },
			{ "Slide Vertices  Shift+V", "modal:slide" }, { "Spin", "spin_tool" }, { "Randomize Vertices", "randomize" }, { "Mirror Vertices...", "menu:mirror" }, { "To Circle", "to_circle" }, { "", "" },
			{ "Vertex Crease", "vertex_crease" }, { "", "" }, { "Shade Smooth", "smooth_faces" }, { "Shade Flat", "flat_faces" }, { "Set Vertex Color", "set_vertex_color" }, { "", "" },
			{ "Merge Vertices  M...", "menu:merge" }, { "Split", "split" }, { "Separate  P...", "menu:separate" }, { "", "" },
			{ "Dissolve Vertices", "dissolve" }, { "Delete Vertices", "delete_verts" } };
	}
	static const char *titles[4] = { "", "Vertex", "Edge", "Face" };
	_show_menu(p_cam, String(titles[mode]) + " Context Menu", items);
}

void HewnEditorPlugin::_delete_menu(Camera3D *p_cam) {
	_show_menu(p_cam, "Delete",
			{ { "Vertices", "delete_verts" }, { "Edges", "delete_edges" }, { "Faces", "delete_faces" }, { "Only Edges & Faces", "delete_only_edges_faces" },
					{ "Only Faces", "delete_only_faces" }, { "", "" }, { "Dissolve Vertices", "dissolve_verts" }, { "Dissolve Edges", "dissolve_edges" },
					{ "Dissolve Faces", "dissolve_faces" }, { "Limited Dissolve", "limited_dissolve" }, { "", "" }, { "Collapse Edges & Faces", "collapse_edges_faces" },
					{ "Edge Loops", "delete_edge_loops" }, { "", "" }, { "Inner Faces", "delete_inner" } });
}

// Shift+A: the Add menu under the mouse - the shapes in Mesh, then other nodes by category.
void HewnEditorPlugin::_add_menu(Camera3D *p_cam) {
	std::vector<std::pair<String, String>> shapes;
	for (int t = 1; t < hewn::shapes::TYPE_MAX; t++) {
		if (t == hewn::shapes::CURVED_STAIRS || t == hewn::shapes::POLY_SHAPE) {
			continue;
		}
		shapes.push_back({ hewn::shapes::type_name((hewn::shapes::Type)t), "shape:" + itos(t) });
	}
	shapes.push_back({ "", "" });
	shapes.push_back({ "Poly Shape", "shape:" + itos(hewn::shapes::POLY_SHAPE) });
	shapes.push_back({ "Vertex", "add:vertex" });
	shapes.push_back({ "Line", "add:line" });
	shapes.push_back({ "", "" });
	shapes.push_back({ "Keep Overlap Faces", "check:keep_overlap" }); // (the Create tab's: the same setting)
	menu_subs.clear();
	menu_subs["mesh"] = shapes;
	// the cuts of the Create tab: holes drawn like shapes, the plane cut, the cutter node
	menu_subs["cut"] = { { "Box Cut", "cut:1" }, { "Cylinder Cut", "cut:2" }, { "Prism Cut", "cut:3" }, { "Clip", "cut:clip" }, { "", "" },
		{ "Apply Cutters", "cut:apply" } };
	// the project's own entities (res://hewn_entities.json), if it has any
	std::vector<std::pair<String, String>> ents;
	const Array defs = HewnDock::entity_defs();
	for (int64_t i = 0; i < defs.size(); i++) {
		ents.push_back({ String(Dictionary(defs[i])["name"]), "entity:" + itos(i) });
	}
	menu_subs["entity"] = ents;
	menu_subs["image"] = { { "Reference Image", "add:ref_image" }, { "", "" }, { "Sprite3D", "add:sprite" }, { "AnimatedSprite3D", "add:anim_sprite" }, { "Image Plane", "add:image_plane" } };
	menu_subs["light"] = { { "Omni Light", "add:omni" }, { "Spot Light", "add:spot" }, { "Directional Light", "add:directional" } };
	menu_subs["decal"] = { { "Overlay", "add:overlay" }, { "Decal", "add:decal" } };
	menu_subs["physics"] = { { "Area3D", "add:area" }, { "Static Body", "add:static_body" }, { "Collision Shape", "add:collision_shape" } };
	// the scenes marked as favorites (RMB in the Scenes panel, or Godot's FileSystem dock): placed with a click
	std::vector<std::pair<String, String>> favs;
	for (const String &f : HewnPrefabBrowser::favorite_scenes()) {
		favs.push_back({ f.get_file().get_basename(), "fav:" + f });
	}
	menu_subs["favorites"] = favs;
	menu_subs["curve"] = { { "Bezier", "addcurve:0" }, { "Circle", "addcurve:1" }, { "NURBS Curve", "addcurve:2" }, { "NURBS Circle", "addcurve:3" }, { "", "" },
		{ "Path3D", "addcurve:4" }, { "", "" }, { "Rope (two points, simulated)", "addcurve:5" } };
	std::vector<std::pair<String, String>> items = { { "Mesh", "sub:mesh" }, { "Curve", "sub:curve" }, { "", "" }, { "Label3D", "add:label" },
		{ "Marker3D", "add:marker" }, { "Node3D", "add:node" }, { "TODO Note", "add:note" }, { "Image", "sub:image" }, { "", "" }, { "Light", "sub:light" }, { "Camera", "add:camera" },
		{ "Sound", "add:sound" }, { "", "" }, { "Decal / Overlay", "sub:decal" }, { "Cut", "sub:cut" }, { "Physics", "sub:physics" }, { "Scatter", "add:scatter" } };
	if (!ents.empty()) {
		items.push_back({ "Entities", "sub:entity" });
	}
	if (!favs.empty()) {
		items.push_back({ "Favorites", "sub:favorites" }); // (none marked: no such entry)
	}
	items.push_back({ "", "" });
	items.push_back({ "Create at 3D Cursor", "check:at_cursor" });
	_show_menu(p_cam, "Add", items);
}

// The Add menu under a button (the "+" in the toolbar).
void HewnEditorPlugin::_on_menu_hidden() {
	menu_hidden_msec = Time::get_singleton()->get_ticks_msec();
}

void HewnEditorPlugin::show_add_menu_at(const Vector2 &p_screen_pos) {
	// the click on "+" that closed the open menu: it stays closed
	if ((op_menu && op_menu->is_visible()) || Time::get_singleton()->get_ticks_msec() - menu_hidden_msec < 300) {
		if (op_menu) {
			op_menu->hide();
		}
		return;
	}
	menu_popup_at = p_screen_pos;
	_add_menu(_viewport_camera());
}

bool HewnEditorPlugin::_menu_check(const String &p_key) const {
	if (p_key == "at_cursor") {
		return create_at_cursor;
	}
	if (p_key == "keep_overlap") {
		return dock && dock->keep_overlap && dock->keep_overlap->is_pressed();
	}
	return false;
}

void HewnEditorPlugin::_menu_toggle(const String &p_key) {
	if (p_key == "keep_overlap" && dock && dock->keep_overlap) {
		dock->keep_overlap->set_pressed(!dock->keep_overlap->is_pressed());
		return;
	}
	if (p_key == "at_cursor") {
		create_at_cursor = !create_at_cursor;
		Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
		if (es.is_valid() && OS::get_singleton()->get_environment("HW_SELFTEST").is_empty()) {
			es->set_project_metadata("hewn", "create_at_cursor", create_at_cursor);
		}
	}
}

void HewnEditorPlugin::_run_menu_op(Camera3D *p_cam, const String &p_op) {
	if (p_op.begins_with("add:")) {
		adding_from_menu = true;
		add_node_kind(p_op.substr(4));
		adding_from_menu = false;
		return;
	}
	if (p_op.begins_with("obj:")) {
		_run_object_op(p_op.substr(4));
		return;
	}
	if (p_op.begins_with("scene:")) {
		start_scene_op(p_op.substr(6));
		return;
	}
	if (p_op.begins_with("hc:")) {
		hc_op(p_op.substr(3));
		return;
	}
	if (p_op.begins_with("cobj:")) {
		curve_object_op(p_op.substr(5));
		return;
	}
	if (p_op.begins_with("addcurve:")) {
		const int kind = p_op.substr(9).to_int();
		if (kind == 4) {
			adding_from_menu = true;
			add_node_kind("path");
			adding_from_menu = false;
		} else if (kind == 5) {
			start_ruler();
			ruler_line = true;
			ruler_rope = true;
			update_overlays();
		} else {
			add_curve(kind);
		}
		return;
	}
	if (p_op.begins_with("fav:")) {
		start_prefab(p_op.substr(4)); // (placed with a click, like from the Scenes panel)
		return;
	}
	if (p_op.begins_with("cut:")) {
		const String c = p_op.substr(4);
		if (c == "clip") {
			start_clip();
		} else if (c == "apply") {
			apply_cutters();
		} else {
			start_cut_tool(c.to_int());
		}
		return;
	}
	if (p_op.begins_with("entity:")) {
		place_entity(p_op.substr(7).to_int());
		return;
	}
	if (p_op == "face_cutter") {
		start_face_cutter();
		return;
	}
	if (p_op.begins_with("shape:") && create_at_cursor && p_op.substr(6).to_int() != hewn::shapes::POLY_SHAPE) {
		create_shape(p_op.substr(6).to_int()); // (at the 3D cursor, its default size)
		return;
	}
	if (p_op.begins_with("shape:")) {
		const int t = p_op.substr(6).to_int();
		if (t == hewn::shapes::POLY_SHAPE) {
			start_poly_tool(false);
		} else {
			start_shape_tool(t);
		}
		return;
	}
	if (p_op == "menu:separate") {
		_show_menu(p_cam, "Separate", { { "Selection", "separate_selection" }, { "By Material", "separate_material" }, { "By Loose Parts", "separate_loose" } });
		return;
	}
	if (p_op == "menu:merge") {
		_show_menu(p_cam, "Merge",
				{ { "At Center", "merge_center" }, { "At Cursor", "merge_cursor" }, { "Collapse", "merge_collapse" }, { "At First", "merge_first" },
						{ "At Last", "merge_last" }, { "By Distance", "weld" } });
		return;
	}
	if (p_op == "menu:normals") {
		_show_menu(p_cam, "Normals",
				{ { "Flip Normals", "flip" }, { "Recalculate Outside  Shift+N", "recalc_outside" }, { "Recalculate Inside  Ctrl+Shift+N", "recalc_inside" }, { "", "" },
						{ "Conform Normals", "conform" } });
		return;
	}
	if (p_op == "menu:mirror") {
		_show_menu(p_cam, "Mirror",
				{ { "X Global", "mirror_x" }, { "Y Global", "mirror_y" }, { "Z Global", "mirror_z" }, { "", "" }, { "X Local", "mirror_lx" },
						{ "Y Local", "mirror_ly" }, { "Z Local", "mirror_lz" } });
		return;
	}
	if (p_op.begins_with("uv:")) {
		run_uv_command(p_op.substr(3));
		return;
	}
	if (p_op.begins_with("modal:")) {
		const String m = p_op.substr(6);
		if (!p_cam) {
			return;
		}
		// from where the mouse is now (on the menu's item), not from the right click that opened the menu: else
		// the first move jumps (Inset and Bevel started already wide)
		if (Control *s = _surface_for(p_cam)) {
			mouse_pos = s->get_local_mouse_position();
		}
		if (m == "duplicate") {
			_start_duplicate(p_cam);
		} else if (m == "extrude") {
			set_op_param("extrude_move", "dissolve_orthogonal", false);
				set_op_param("extrude_move", "flip_normals", false);
			_start_extrude(p_cam);
		} else if (m == "extrude_normals") {
			_on_extrude_menu_cam(p_cam, 1);
		} else if (m == "extrude_individual") {
			_on_extrude_menu_cam(p_cam, 2);
		} else if (m == "inset") {
			_start_inset(p_cam);
		} else if (m == "bevel") {
			_start_bevel(p_cam);
		} else if (m == "slide") {
			if (_start_transform(MODAL_MOVE, p_cam)) {
				_start_slide(p_cam);
			}
		} else {
			_start_value_modal(p_cam, m);
		}
		return;
	}
	run_op(p_op);
}

// ------------------------------------------------------------------ value modals

void HewnEditorPlugin::_start_value_modal(Camera3D *p_cam, const String &p_op) {
	HewnMesh3D *n = get_edited();
	if (!n || !p_cam || _selected_verts().empty()) {
		return;
	}
	begin_edit(p_op.capitalize());
	modal_backup = n->get_data();
	modal_sel_backup = n->selection;
	value_op = p_op;
	modal_verts = _selected_verts();
	modal_pivot = _pivot_point(dock->pivot_mode, modal_verts);
	modal_mouse0 = mouse_pos;
	_reset_precise();
	modal_num = "";
	modal_axis = -1;
	modal_object = false;
	const Basis cb = p_cam->get_global_transform().basis;
	value_axis_a = cb.get_column(0).normalized(); // shear: moves along the view's X ...
	value_axis_b = cb.get_column(1).normalized(); // ... proportionally to the view's Y
	const float d0 = MAX(20.0f, mouse_pos.distance_to(p_cam->unproject_position(modal_pivot)));
	const bool width = p_op == "bevel_vertices" || p_op == "offset_edge_slide";
	modal_scale_k = width ? MAX(1e-4f, (float)op_param(p_op, "value")) / d0 : _world_per_pixel(p_cam, modal_pivot);
	modal = MODAL_VALUE;
	drag = DRAG_NONE;
	_update_modal(p_cam, false, false);
}

// The operations behind the value modals (also replayed by run_op with the stored value).
void HewnEditorPlugin::_value_op_apply(const String &p_op, float p_value) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	hewn::Mesh &m = n->get_data();
	hewn::Selection &s = n->selection;
	const Transform3D g = n->get_global_transform();
	const Transform3D inv = g.affine_inverse();
	const std::vector<int> sel = _selected_verts();
	if (p_op == "bevel_vertices") {
		const std::vector<int> faces = hewn::ops::bevel_vertices(m, sel, p_value);
		s.clear();
		s.faces = faces;
		s.verts = hewn::sel::verts_of_faces(m, faces);
	} else if (p_op == "offset_edge_slide") {
		std::vector<hewn::Edge> edges = s.edges;
		if (mode != MODE_EDGE) {
			hewn::Topology t;
			t.build(m);
			edges = hewn::sel::edges_from_verts(m, t, sel);
		}
		const std::vector<int> faces = hewn::ops::bevel_edges(m, edges, p_value, 2, true);
		s.clear();
		s.edges = hewn::sel::edges_of_faces(m, faces);
	} else if (p_op == "push_pull") {
		const Vector3 piv = Vector3((float)op_param(p_op, "px"), (float)op_param(p_op, "py"), (float)op_param(p_op, "pz"));
		for (int v : sel) {
			const Vector3 w = g.xform(m.pos[v]);
			const Vector3 d = w - piv;
			if (d.length() > 1e-9f) {
				m.pos[v] = inv.xform(w + d.normalized() * p_value);
			}
		}
	} else if (p_op == "shrink_fatten") {
		const std::vector<Vector3> vn = hewn::vertex_normals(m);
		const Basis nb = g.basis.inverse().transposed();
		for (int v : sel) {
			const Vector3 nw = nb.xform(vn[v]);
			if (nw.length() > 1e-9f) {
				m.pos[v] = inv.xform(g.xform(m.pos[v]) + nw.normalized() * p_value);
			}
		}
	} else if (p_op == "shear") {
		const Vector3 a((float)op_param(p_op, "ax"), (float)op_param(p_op, "ay"), (float)op_param(p_op, "az"));
		const Vector3 b((float)op_param(p_op, "bx"), (float)op_param(p_op, "by"), (float)op_param(p_op, "bz"));
		const Vector3 piv((float)op_param(p_op, "px"), (float)op_param(p_op, "py"), (float)op_param(p_op, "pz"));
		for (int v : sel) {
			const Vector3 w = g.xform(m.pos[v]);
			m.pos[v] = inv.xform(w + a * ((w - piv).dot(b) * p_value));
		}
	}
}

void HewnEditorPlugin::_store_value_params(const String &p_op) {
	set_op_param(p_op, "px", modal_pivot.x);
	set_op_param(p_op, "py", modal_pivot.y);
	set_op_param(p_op, "pz", modal_pivot.z);
	if (p_op == "shear") {
		set_op_param(p_op, "ax", value_axis_a.x);
		set_op_param(p_op, "ay", value_axis_a.y);
		set_op_param(p_op, "az", value_axis_a.z);
		set_op_param(p_op, "bx", value_axis_b.x);
		set_op_param(p_op, "by", value_axis_b.y);
		set_op_param(p_op, "bz", value_axis_b.z);
	}
}

float HewnEditorPlugin::_value_from_mouse(Camera3D *p_cam) const {
	const Vector2 sp = p_cam->unproject_position(modal_pivot);
	if (value_op == "bevel_vertices" || value_op == "offset_edge_slide") {
		return MAX(0.0f, mouse_pos.distance_to(sp) * modal_scale_k);
	}
	if (value_op == "shear") {
		return (mouse_pos.x - modal_mouse0.x) / MAX(50.0f, modal_mouse0.distance_to(sp));
	}
	// push/pull, shrink/fatten: away from the pivot on screen = positive
	return (mouse_pos.distance_to(sp) - modal_mouse0.distance_to(sp)) * modal_scale_k;
}
