// Parameters of a placed light, entity, trigger, sound, decal, overlay or cutter: a small panel at the
// bottom left of the viewport (like the shape panel), its main properties, undoable.
#include "hw_editor.h"
#include "hw_editor_util.h"
#include "hw_textures.h"

#include "../hw_cutter.h"
#include "../hw_entities.h"
#include "../hw_overlay3d.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/animated_sprite3d.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/editor_file_dialog.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/sprite3d.hpp>
#include <godot_cpp/classes/sprite_frames.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/color_picker_button.hpp>
#include <godot_cpp/classes/decal.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/area3d.hpp>
#include <godot_cpp/classes/label3d.hpp>
#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/spot_light3d.hpp>
#include <godot_cpp/classes/style_box.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

using namespace godot;

// Where a shown property lives: the node itself, or (an Area3D's "size") the box shape of its collision shape.
Object *godot::hw_obj_owner(Node *p_node, const String &p_prop) {
	if (Object::cast_to<Area3D>(p_node) && p_prop == "size") {
		for (int i = 0; i < p_node->get_child_count(); i++) {
			if (CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(p_node->get_child(i))) {
				Ref<BoxShape3D> b = cs->get_shape();
				if (b.is_valid()) {
					return b.ptr();
				}
			}
		}
	}
	return p_node;
}

Variant godot::hw_obj_get(Node *p_node, const String &p_prop) {
	Object *o = hw_obj_owner(p_node, p_prop);
	return o ? o->get(p_prop) : Variant();
}

// The properties shown for a node (in order), and its title; empty: no panel.
std::vector<const char *> godot::hw_obj_props(Node *p_node, String &r_title) {
	if (Object::cast_to<SpotLight3D>(p_node)) {
		r_title = "Spot Light";
		return { "light_color", "light_energy", "spot_range", "spot_angle", "shadow_enabled" };
	}
	if (Object::cast_to<OmniLight3D>(p_node)) {
		r_title = "Omni Light";
		return { "light_color", "light_energy", "omni_range", "shadow_enabled" };
	}
	if (Object::cast_to<DirectionalLight3D>(p_node)) {
		r_title = "Directional Light";
		return { "light_color", "light_energy", "shadow_enabled" };
	}
	if (Object::cast_to<HewnEntity3D>(p_node)) {
		r_title = "Entity";
		return { "classname", "properties" };
	}
	if (Object::cast_to<AudioStreamPlayer3D>(p_node)) {
		r_title = "Sound";
		return { "volume_db", "unit_size", "max_distance", "autoplay" };
	}
	if (Object::cast_to<Decal>(p_node)) {
		r_title = "Decal";
		return { "size", "modulate", "albedo_mix", "normal_fade", "cull_mask" };
	}
	if (Object::cast_to<HewnOverlay3D>(p_node)) {
		r_title = "Overlay";
		return { "size", "outline", "max_vertices", "blend", "render_order", "modulate", "max_angle" };
	}
	if (Object::cast_to<HewnCutter3D>(p_node)) {
		r_title = "Cutter";
		return { "shape", "size", "sides", "keep_flush", "cut_ends", "skip_parent", "lining", "active" };
	}
	if (Object::cast_to<Area3D>(p_node)) {
		r_title = "Area3D";
		return { "size", "monitoring", "monitorable", "priority", "collision_layer", "collision_mask" };
	}
	if (Object::cast_to<Label3D>(p_node)) {
		r_title = "Label";
		return { "text", "font_size", "pixel_size", "modulate", "outline_size", "billboard", "double_sided" };
	}
	if (Object::cast_to<Camera3D>(p_node)) {
		r_title = "Camera";
		return { "fov", "near", "far" };
	}
	if (Object::cast_to<AnimatedSprite3D>(p_node)) {
		r_title = "Animated Sprite";
		return { "pixel_size", "billboard", "modulate", "speed_scale" };
	}
	if (Object::cast_to<Sprite3D>(p_node)) {
		r_title = "Sprite";
		return { "pixel_size", "billboard", "modulate", "double_sided" };
	}
	if (Object::cast_to<MeshInstance3D>(p_node) && p_node->has_meta("hw_image_plane")) {
		r_title = "Image Plane";
		return { "cast_shadow" };
	}
	return {};
}

// The picture a node shows (sprites, image planes, decals, overlays): its path, or empty.
String godot::hw_obj_picture(Node *p_node) {
	Ref<Texture2D> t;
	if (Sprite3D *s = Object::cast_to<Sprite3D>(p_node)) {
		t = s->get_texture();
	} else if (AnimatedSprite3D *a = Object::cast_to<AnimatedSprite3D>(p_node)) {
		Ref<SpriteFrames> sf = a->get_sprite_frames();
		if (sf.is_valid() && sf->has_animation("default") && sf->get_frame_count("default") > 0) {
			t = sf->get_frame_texture("default", 0);
		}
	} else if (Decal *d = Object::cast_to<Decal>(p_node)) {
		t = d->get_texture(Decal::TEXTURE_ALBEDO);
	} else if (Object::cast_to<HewnOverlay3D>(p_node)) {
		t = p_node->get("texture");
	} else if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_node)) {
		Ref<QuadMesh> q = mi->get_mesh();
		Ref<BaseMaterial3D> m = q.is_valid() ? Ref<BaseMaterial3D>(q->get_material()) : Ref<BaseMaterial3D>();
		if (m.is_valid()) {
			t = m->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
		}
	}
	return t.is_valid() ? t->get_path() : String();
}

bool godot::hw_obj_has_picture(Node *p_node) {
	return Object::cast_to<Sprite3D>(p_node) || Object::cast_to<AnimatedSprite3D>(p_node) || Object::cast_to<Decal>(p_node) ||
			Object::cast_to<HewnOverlay3D>(p_node) || (Object::cast_to<MeshInstance3D>(p_node) && p_node->has_meta("hw_image_plane"));
}

namespace {

String hint_of(Object *p_obj, const String &p_prop, int &r_hint) {
	const TypedArray<Dictionary> list = p_obj->get_property_list();
	for (int64_t i = 0; i < list.size(); i++) {
		const Dictionary d = list[i];
		if (String(d["name"]) == p_prop) {
			r_hint = d["hint"];
			return d["hint_string"];
		}
	}
	r_hint = 0;
	return String();
}

} // namespace

void HewnEditorPlugin::_set_obj_prop(const Variant &p_value, const String &p_prop) {
	Node *n = obj_panel_node ? Object::cast_to<Node>(ObjectDB::get_instance(obj_panel_node)) : nullptr;
	if (!n) {
		return;
	}
	const Variant old = hw_obj_get(n, p_prop);
	if (old == p_value) {
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Set " + p_prop.capitalize(), UndoRedo::MERGE_ENDS, n);
	Object *owner = hw_obj_owner(n, p_prop);
	ur->add_do_property(owner, p_prop, p_value);
	ur->add_undo_property(owner, p_prop, old);
	ur->commit_action();
	obj_panel_values[p_prop] = hw_obj_get(n, p_prop);
}

void HewnEditorPlugin::_on_obj_float(double p_value, const String &p_prop, int p_axis) {
	Node *n = obj_panel_node ? Object::cast_to<Node>(ObjectDB::get_instance(obj_panel_node)) : nullptr;
	if (!n) {
		return;
	}
	Variant v = hw_obj_get(n, p_prop);
	// a decal's / overlay's size with the chain on: all sides by the same factor
	const bool linked = obj_link_size && p_prop == "size" && (Object::cast_to<Decal>(n) || Object::cast_to<HewnOverlay3D>(n));
	switch (v.get_type()) {
		case Variant::VECTOR3: {
			Vector3 q = v;
			if (linked && std::fabs(q[p_axis]) > 1e-6f) {
				q *= (float)p_value / q[p_axis];
			}
			q[p_axis] = (float)p_value;
			v = q;
		} break;
		case Variant::VECTOR2: {
			Vector2 q = v;
			if (linked && std::fabs(q[p_axis]) > 1e-6f) {
				q *= (float)p_value / q[p_axis];
			}
			q[p_axis] = (float)p_value;
			v = q;
		} break;
		case Variant::INT:
			v = (int64_t)std::llround(p_value);
			break;
		default:
			v = p_value;
			break;
	}
	_set_obj_prop(v, p_prop);
	if (linked && obj_body) {
		// the other sides' fields follow at once (the panel is not rebuilt while one is typed in)
		const Variant now = hw_obj_get(n, p_prop);
		const int count = now.get_type() == Variant::VECTOR3 ? 3 : 2;
		for (int a = 0; a < count; a++) {
			if (a == p_axis) {
				continue;
			}
			if (HewnDragField *f = Object::cast_to<HewnDragField>(obj_body->find_child(p_prop + String("_") + itos(a), true, false))) {
				f->set_value_no_signal(count == 3 ? (double)Vector3(now)[a] : (double)Vector2(now)[a]);
			}
		}
	}
}

void HewnEditorPlugin::_on_obj_bool(bool p_value, const String &p_prop) {
	_set_obj_prop(p_value, p_prop);
}

void HewnEditorPlugin::_on_obj_option(int p_index, const String &p_prop) {
	_set_obj_prop(p_index, p_prop);
}

void HewnEditorPlugin::_on_obj_color(const Color &p_color, const String &p_prop) {
	_set_obj_prop(p_color, p_prop);
}

void HewnEditorPlugin::_on_obj_text(const String &p_text, const String &p_prop) {
	Node *n = obj_panel_node ? Object::cast_to<Node>(ObjectDB::get_instance(obj_panel_node)) : nullptr;
	if (!n) {
		return;
	}
	if (n->get(p_prop).get_type() == Variant::NODE_PATH) {
		_set_obj_prop(NodePath(p_text), p_prop);
	} else {
		_set_obj_prop(p_text, p_prop);
	}
}

// An entity's key / value: numbers stay numbers.
void HewnEditorPlugin::_on_obj_entity_value(const String &p_text, const String &p_key) {
	HewnEntity3D *e = obj_panel_node ? Object::cast_to<HewnEntity3D>(ObjectDB::get_instance(obj_panel_node)) : nullptr;
	if (!e) {
		return;
	}
	Dictionary d = e->get_properties().duplicate();
	const String t = p_text.strip_edges().replace(",", ".");
	d[p_key] = t.is_valid_int() ? Variant(t.to_int()) : t.is_valid_float() ? Variant(t.to_float()) : (t == "true" || t == "false") ? Variant(t == "true") : Variant(p_text);
	_set_obj_prop(d, "properties");
}

void HewnEditorPlugin::_on_obj_apply_cutter() {
	apply_cutters();
}

void HewnEditorPlugin::_on_obj_link(bool p_on) {
	obj_link_size = p_on;
}

void HewnEditorPlugin::_on_obj_layer(bool p_on, const String &p_prop, int p_bit) {
	Node *n = obj_panel_node ? Object::cast_to<Node>(ObjectDB::get_instance(obj_panel_node)) : nullptr;
	if (!n) {
		return;
	}
	int64_t mask = n->get(p_prop);
	mask = p_on ? (mask | (int64_t(1) << p_bit)) : (mask & ~(int64_t(1) << p_bit));
	_set_obj_prop(mask, p_prop);
}

// Another picture for the selected sprite / image plane / decal / overlay (its path), undoable.
void HewnEditorPlugin::_on_obj_picture(const String &p_path) {
	Node *n = obj_panel_node ? Object::cast_to<Node>(ObjectDB::get_instance(obj_panel_node)) : nullptr;
	const String path = p_path.strip_edges();
	Ref<Texture2D> tex = path.is_empty() || !ResourceLoader::get_singleton()->exists(path) ? Ref<Texture2D>() : Ref<Texture2D>(ResourceLoader::get_singleton()->load(path));
	if (!n || tex.is_null()) {
		ERR_PRINT("Hewn: no picture at " + path);
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Set Picture", UndoRedo::MERGE_DISABLE, n);
	if (Sprite3D *s = Object::cast_to<Sprite3D>(n)) {
		ur->add_do_property(s, "texture", tex);
		ur->add_undo_property(s, "texture", s->get_texture());
	} else if (AnimatedSprite3D *a = Object::cast_to<AnimatedSprite3D>(n)) {
		Ref<SpriteFrames> sf;
		sf.instantiate();
		sf->add_frame("default", tex);
		ur->add_do_property(a, "sprite_frames", sf);
		ur->add_undo_property(a, "sprite_frames", a->get_sprite_frames());
	} else if (Decal *d = Object::cast_to<Decal>(n)) {
		ur->add_do_property(d, "texture_albedo", tex);
		ur->add_undo_property(d, "texture_albedo", d->get_texture(Decal::TEXTURE_ALBEDO));
	} else if (Object::cast_to<HewnOverlay3D>(n)) {
		ur->add_do_property(n, "texture", tex);
		ur->add_undo_property(n, "texture", n->get("texture"));
	} else if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(n)) {
		// an image plane: its quad as big as the new picture (the Textures panel's pixels per meter)
		Ref<QuadMesh> q = mi->get_mesh();
		Ref<QuadMesh> nq = q.is_valid() ? Ref<QuadMesh>(q->duplicate(true)) : Ref<QuadMesh>();
		if (nq.is_null()) {
			nq.instantiate();
		}
		const float ppm = texture_dock && texture_dock->pixels_per_meter() > 0.0f ? texture_dock->pixels_per_meter() : 128.0f;
		nq->set_size(Vector2(tex->get_width(), tex->get_height()) / ppm);
		Ref<StandardMaterial3D> m = nq->get_material();
		if (m.is_null()) {
			m.instantiate();
			m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
			m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
			nq->set_material(m);
		}
		m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, tex);
		ur->add_do_property(mi, "mesh", nq);
		ur->add_undo_property(mi, "mesh", mi->get_mesh());
	}
	ur->commit_action();
	obj_panel_values = Dictionary(); // (the panel shows the new one)
}

void HewnEditorPlugin::_on_obj_picture_browse() {
	if (!obj_pic_dialog) {
		obj_pic_dialog = memnew(EditorFileDialog);
		obj_pic_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
		obj_pic_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
		for (const char *e : { "*.png", "*.jpg", "*.jpeg", "*.webp", "*.svg", "*.bmp", "*.tga", "*.exr", "*.hdr", "*.ktx" }) {
			obj_pic_dialog->add_filter(e);
		}
		obj_pic_dialog->connect("file_selected", callable_mp(this, &HewnEditorPlugin::_on_obj_picture));
		EditorInterface::get_singleton()->get_base_control()->add_child(obj_pic_dialog);
	}
	obj_pic_dialog->popup_file_dialog();
}

// The camera: moved to where the viewport's camera is, looking the same way (undoable).
void HewnEditorPlugin::_on_obj_camera_to_view() {
	Camera3D *c = obj_panel_node ? Object::cast_to<Camera3D>(ObjectDB::get_instance(obj_panel_node)) : nullptr;
	Camera3D *view = _viewport_camera();
	if (!c || !view) {
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Camera to the View", UndoRedo::MERGE_DISABLE, c);
	ur->add_do_method(c, "set_global_transform", view->get_global_transform());
	ur->add_undo_method(c, "set_global_transform", c->get_global_transform());
	ur->commit_action();
}

// The small picture of what the selected camera sees (the editor's world, the camera's own layers).
void HewnEditorPlugin::_update_camera_preview(Camera3D *p_cam) {
	if (!cam_preview_vp) {
		return;
	}
	Camera3D *pc = Object::cast_to<Camera3D>(cam_preview_vp->get_child(0));
	SubViewport *ev = EditorInterface::get_singleton()->get_editor_viewport_3d(0);
	if (!p_cam || !pc || !ev) {
		cam_preview_vp->set_update_mode(SubViewport::UPDATE_DISABLED);
		return;
	}
	if (cam_preview_vp->get_world_3d() != ev->get_world_3d()) {
		cam_preview_vp->set_world_3d(ev->get_world_3d());
	}
	pc->set_global_transform(p_cam->get_global_transform());
	pc->set_projection(p_cam->get_projection());
	pc->set_fov(p_cam->get_fov());
	pc->set_size(p_cam->get_size());
	pc->set_near(p_cam->get_near());
	pc->set_far(p_cam->get_far());
	pc->set_cull_mask(p_cam->get_cull_mask() & 0xFFFFF); // (not the editor's gizmos)
	pc->set_environment(p_cam->get_environment());
	cam_preview_vp->set_update_mode(SubViewport::UPDATE_ALWAYS);
}

// The spot light's Aim button: the next click on a surface turns it toward that point.
void HewnEditorPlugin::_on_obj_aim() {
	aim_light = ObjectID(obj_panel_node);
	if (Node3D *l = Object::cast_to<Node3D>(ObjectDB::get_instance(aim_light))) {
		aim_before = l->get_global_transform();
	}
	update_overlays();
}

void HewnEditorPlugin::_update_obj_panel() {
	Node *sel = nullptr;
	const TypedArray<Node> nodes = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
	if (nodes.size() == 1 && modal == MODAL_NONE && mode == MODE_OBJECT) {
		sel = Object::cast_to<Node>(nodes[0]);
	}
	String title;
	const std::vector<const char *> props = sel ? hw_obj_props(sel, title) : std::vector<const char *>();
	Control *surface = _main_surface();
	if (props.empty() || !surface || !surface->is_visible_in_tree() || (shape_panel && shape_panel->is_visible())) {
		if (obj_panel && obj_panel->is_visible()) {
			obj_panel->hide();
		}
		obj_panel_node = 0;
		_update_camera_preview(nullptr);
		return;
	}
	// up to date: the same node, the same values (else rebuilt, unless a field is being typed in)
	if (obj_panel && obj_panel->is_visible() && obj_panel_node == sel->get_instance_id() && obj_panel->get_parent() == surface) {
		bool same = true;
		for (const char *p : props) {
			same = same && hw_obj_get(sel, p) == obj_panel_values.get(p, Variant());
		}
		Control *focus = obj_panel->get_viewport() ? obj_panel->get_viewport()->gui_get_focus_owner() : nullptr;
		if (same && hw_obj_has_picture(sel)) {
			same = hw_obj_picture(sel) == String(obj_panel_values.get("_picture", String()));
		}
		if (same || (focus && obj_panel->is_ancestor_of(focus))) {
			_update_camera_preview(Object::cast_to<Camera3D>(sel));
			return;
		}
	}
	if (!obj_panel) {
		obj_panel = memnew(PanelContainer);
		obj_panel->set_name("HewnObjectPanel");
		VBoxContainer *v = memnew(VBoxContainer);
		obj_panel->add_child(v);
		Button *t = memnew(Button);
		t->set_name("Title");
		t->connect("pressed", callable_mp(this, &HewnEditorPlugin::_toggle_redo));
		v->add_child(t);
		_style_panel_header(t, obj_panel);
		obj_body = memnew(GridContainer);
		obj_body->set_columns(2);
		v->add_child(obj_body);
		_compact_panel(obj_panel, obj_body);
	}
	if (obj_panel->get_parent() != surface) {
		if (obj_panel->get_parent()) {
			obj_panel->get_parent()->remove_child(obj_panel);
		}
		surface->add_child(obj_panel);
	}
	obj_panel_node = sel->get_instance_id();
	obj_panel_values = Dictionary();
	if (Button *t = Object::cast_to<Button>(obj_panel->find_child("Title", true, false))) {
		t->set_text(title + ": " + String(sel->get_name()));
		t->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);
		_style_panel_header(t, nullptr);
	}
	for (int i = obj_body->get_child_count() - 1; i >= 0; i--) {
		Node *c = obj_body->get_child(i);
		obj_body->remove_child(c);
		c->queue_free();
	}
	obj_body->set_visible(!redo_collapsed);
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	auto drag = [&](double p_value, const String &p_prop, int p_axis, bool p_int) {
		HewnDragField *f = memnew(HewnDragField);
		f->setup(-1e6, 1e6, p_int ? 1.0 : 0.01, p_int, p_value);
		f->set_custom_minimum_size(Vector2(60 * es, 0));
		f->connect("value_changed", callable_mp(this, &HewnEditorPlugin::_on_obj_float).bind(p_prop, p_axis));
		return f;
	};
	// the property's default (the revert arrow): as Godot's inspector has it
	auto default_of = [&](const String &p_prop) -> Variant {
		if (sel->property_can_revert(p_prop)) {
			return sel->property_get_revert(p_prop);
		}
		return ClassDBSingleton::get_singleton()->class_get_property_default_value(sel->get_class(), p_prop);
	};
	auto rev = [&](Control *p_field, const Variant &p_def) -> Control * {
		return p_def.get_type() == Variant::NIL ? p_field : hw_editor::with_revert(p_field, p_def);
	};
	for (const char *pc : props) {
		const String p = pc;
		const Variant v = hw_obj_get(sel, p);
		const Variant pdef = default_of(p);
		obj_panel_values[p] = v;
		Label *l = memnew(Label);
		_compact_label(l, hw_editor::T(p.capitalize()));
		obj_body->add_child(l);
		int hint = 0;
		const String hs = hint_of(sel, p, hint);
		switch (v.get_type()) {
			case Variant::BOOL: {
				CheckBox *c = memnew(CheckBox);
				c->set_pressed(v);
				c->connect("toggled", callable_mp(this, &HewnEditorPlugin::_on_obj_bool).bind(p));
				obj_body->add_child(rev(c, pdef));
			} break;
			case Variant::INT: {
				if (hint == PROPERTY_HINT_LAYERS_3D_RENDER || hint == PROPERTY_HINT_LAYERS_3D_PHYSICS) {
					// the layers as in the inspector: two rows of ten
					GridContainer *g = memnew(GridContainer);
					g->set_columns(10);
					g->add_theme_constant_override("h_separation", 1);
					g->add_theme_constant_override("v_separation", 1);
					const int64_t mask = v;
					for (int bit = 0; bit < 20; bit++) {
						Button *b = memnew(Button);
						b->set_toggle_mode(true);
						b->set_pressed((mask >> bit) & 1);
						b->set_text(itos(bit + 1));
						b->set_tooltip_text(vformat(hw_editor::T("Layer %d"), bit + 1));
						b->set_custom_minimum_size(Vector2(16 * es, 16 * es));
						b->add_theme_font_size_override("font_size", (int)(9 * es));
						b->set_focus_mode(Control::FOCUS_NONE);
						b->connect("toggled", callable_mp(this, &HewnEditorPlugin::_on_obj_layer).bind(p, bit));
						g->add_child(b);
					}
					obj_body->add_child(g);
				} else if (hint == PROPERTY_HINT_ENUM) {
					OptionButton *o = memnew(OptionButton);
					const PackedStringArray items = hs.split(",");
					for (int64_t i = 0; i < items.size(); i++) {
						o->add_item(items[i].get_slice(":", 0), (int)i);
					}
					o->select((int)v);
					o->connect("item_selected", callable_mp(this, &HewnEditorPlugin::_on_obj_option).bind(p));
					obj_body->add_child(rev(o, pdef));
				} else {
					obj_body->add_child(rev(drag((int64_t)v, p, 0, true), pdef));
				}
			} break;
			case Variant::FLOAT:
				obj_body->add_child(rev(drag((double)v, p, 0, false), pdef));
				break;
			case Variant::VECTOR2:
			case Variant::VECTOR3: {
				HBoxContainer *h = memnew(HBoxContainer);
				const int n = v.get_type() == Variant::VECTOR3 ? 3 : 2;
				for (int a = 0; a < n; a++) {
					const double c = n == 3 ? (double)Vector3(v)[a] : (double)Vector2(v)[a];
					HewnDragField *f = drag(c, p, a, false);
					f->set_name(p + String("_") + itos(a));
					f->set_custom_minimum_size(Vector2(46 * es, 0));
					const Variant ad = pdef.get_type() == Variant::VECTOR3 ? Variant(Vector3(pdef)[a]) : pdef.get_type() == Variant::VECTOR2 ? Variant(Vector2(pdef)[a]) : Variant();
					h->add_child(rev(f, ad));
				}
				if (p == "size" && (Object::cast_to<Decal>(sel) || Object::cast_to<HewnOverlay3D>(sel))) {
					// the chain: linked sides (default), click: each on its own
					Button *link = memnew(Button);
					link->set_toggle_mode(true);
					link->set_pressed(obj_link_size);
					link->set_flat(true);
					link->set_button_icon(EditorInterface::get_singleton()->get_editor_theme()->get_icon("Instance", "EditorIcons"));
					link->set_tooltip_text("The sides together (click: each on its own)");
					link->connect("toggled", callable_mp(this, &HewnEditorPlugin::_on_obj_link));
					h->add_child(link);
				}
				obj_body->add_child(h);
			} break;
			case Variant::COLOR: {
				ColorPickerButton *c = memnew(ColorPickerButton);
				c->set_pick_color(v);
				c->set_custom_minimum_size(Vector2(60 * es, 0));
				c->connect("color_changed", callable_mp(this, &HewnEditorPlugin::_on_obj_color).bind(p));
				obj_body->add_child(rev(c, pdef));
			} break;
			case Variant::DICTIONARY: {
				// an entity's key / values: one line each
				obj_body->add_child(memnew(Control));
				const Dictionary d = v;
				const Array keys = d.keys();
				for (int64_t i = 0; i < keys.size(); i++) {
					Label *kl = memnew(Label);
					kl->set_text(String("  ") + String(keys[i]));
					kl->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);
					obj_body->add_child(kl);
					LineEdit *le = memnew(LineEdit);
					le->set_text(String(d[keys[i]]));
					le->set_custom_minimum_size(Vector2(80 * es, 0));
					le->connect("text_submitted", callable_mp(this, &HewnEditorPlugin::_on_obj_entity_value).bind(String(keys[i])));
					obj_body->add_child(le);
				}
			} break;
			default: {
				LineEdit *le = memnew(LineEdit);
				le->set_text(String(v));
				le->set_custom_minimum_size(Vector2(80 * es, 0));
				le->connect("text_submitted", callable_mp(this, &HewnEditorPlugin::_on_obj_text).bind(p));
				obj_body->add_child(le);
			} break;
		}
	}
	if (Object::cast_to<SpotLight3D>(sel)) {
		Button *aim = memnew(Button);
		aim->set_text("Aim at a Point");
		aim->set_tooltip_text("Then click where it should shine (on a surface)");
		aim->connect("pressed", callable_mp(this, &HewnEditorPlugin::_on_obj_aim));
		obj_body->add_child(memnew(Control));
		obj_body->add_child(aim);
	}
	if (hw_obj_has_picture(sel)) {
		// the picture: its path (Enter), or picked in a file dialog
		const String pic = hw_obj_picture(sel);
		obj_panel_values["_picture"] = pic;
		Label *l = memnew(Label);
		_compact_label(l, hw_editor::T("Picture"));
		obj_body->add_child(l);
		HBoxContainer *h = memnew(HBoxContainer);
		LineEdit *le = memnew(LineEdit);
		le->set_name("PicturePath");
		le->set_text(pic);
		le->set_custom_minimum_size(Vector2(140 * es, 0));
		le->set_tooltip_text("The picture's path (res://...); Enter: put it on");
		le->connect("text_submitted", callable_mp(this, &HewnEditorPlugin::_on_obj_picture));
		h->add_child(le);
		Button *b = memnew(Button);
		b->set_text("...");
		b->set_tooltip_text("Pick a picture");
		b->connect("pressed", callable_mp(this, &HewnEditorPlugin::_on_obj_picture_browse));
		h->add_child(b);
		obj_body->add_child(h);
	}
	if (Camera3D *cam = Object::cast_to<Camera3D>(sel)) {
		Button *aim = memnew(Button);
		aim->set_text("Aim at a Point");
		aim->set_tooltip_text("Then click where it should look (on a surface)");
		aim->connect("pressed", callable_mp(this, &HewnEditorPlugin::_on_obj_aim));
		obj_body->add_child(memnew(Control));
		obj_body->add_child(aim);
		Button *tv = memnew(Button);
		tv->set_text("To the View");
		tv->set_tooltip_text("The camera goes where the viewport's camera is, looking the same way");
		tv->connect("pressed", callable_mp(this, &HewnEditorPlugin::_on_obj_camera_to_view));
		obj_body->add_child(memnew(Control));
		obj_body->add_child(tv);
		// what it sees
		if (!cam_preview_vp) {
			cam_preview_vp = memnew(SubViewport);
			cam_preview_vp->set_name("HewnCameraPreview");
			cam_preview_vp->set_size(Vector2i(192, 108));
			cam_preview_vp->add_child(memnew(Camera3D));
			EditorInterface::get_singleton()->get_base_control()->add_child(cam_preview_vp);
		}
		TextureRect *tr = memnew(TextureRect);
		tr->set_name("CameraPreview");
		tr->set_texture(cam_preview_vp->get_texture());
		tr->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		tr->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
		tr->set_custom_minimum_size(Vector2(192, 108) * es);
		obj_body->add_child(memnew(Control));
		obj_body->add_child(tr);
		_update_camera_preview(cam);
	} else {
		_update_camera_preview(nullptr);
	}
	if (Object::cast_to<HewnCutter3D>(sel)) {
		Button *ap = memnew(Button);
		ap->set_text("Apply");
		ap->set_tooltip_text("The hole goes into the meshes for good (the cutter is removed)");
		ap->connect("pressed", callable_mp(this, &HewnEditorPlugin::_on_obj_apply_cutter));
		obj_body->add_child(memnew(Control));
		obj_body->add_child(ap);
	}
	hw_editor::localize(obj_panel);
	obj_panel->reset_size();
	obj_panel->set_position(_left_panel_pos(surface, obj_panel));
	obj_panel->show();
}
