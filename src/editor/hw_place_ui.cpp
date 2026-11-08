// Create tab: point entities and decals / overlays, placed like prefabs.
#include "../hw_scatter.h"
#include "hw_editor.h"
#include "../hw_refimage.h"
#include "hw_textures.h"

#include "../hw_cutter.h"
#include "../hw_entities.h"
#include "../hw_overlay3d.h"

#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/decal.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/editor_resource_picker.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_flow_container.hpp>
#include <godot_cpp/classes/h_separator.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/spot_light3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/animated_sprite3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/curve3d.hpp>
#include <godot_cpp/classes/area3d.hpp>
#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_file_dialog.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/label3d.hpp>
#include <godot_cpp/classes/marker3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/path3d.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/sprite3d.hpp>
#include <godot_cpp/classes/sprite_frames.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>

using namespace godot;

namespace {

const char *ENTITY_FILE = "res://hewn_entities.json";

Label *header(Container *p_parent, const String &p_text) {
	Label *l = memnew(Label);
	l->set_text(p_text);
	l->add_theme_color_override("font_color", Color(0.7f, 0.8f, 1.0f));
	p_parent->add_child(l);
	return l;
}

Dictionary entity(const String &p_name, const String &p_kind, const String &p_class, const Dictionary &p_props, const Color &p_color, bool p_along, float p_offset) {
	Dictionary d;
	d["name"] = p_name;
	d["kind"] = p_kind;
	d["classname"] = p_class;
	d["properties"] = p_props;
	d["color"] = p_color;
	d["along_normal"] = p_along;
	d["offset"] = p_offset;
	return d;
}

} // namespace

// Built-in entities, then the project's own (res://hewn_entities.json: a list of
// { "name", "classname", "properties": {}, "color": "#rrggbb", "along_normal": false, "offset": 0 }).
Array HewnDock::entity_defs() {
	Array out;
	// (lights and sounds are in Shift+A; here the project's own entities)
	if (FileAccess::file_exists(ENTITY_FILE)) {
		const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(ENTITY_FILE));
		const Array list = parsed.get_type() == Variant::ARRAY ? Array(parsed) : Array();
		for (int64_t i = 0; i < list.size(); i++) {
			if (list[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary e = list[i];
			const String cls = e.get("classname", "");
			if (cls.is_empty()) {
				continue;
			}
			const String col = e.get("color", "#ff9919");
			out.push_back(entity(e.get("name", cls), "entity", cls, e.get("properties", Dictionary()), Color::html(col), e.get("along_normal", false),
					(float)(double)e.get("offset", 0.0)));
		}
	}
	return out;
}

void HewnDock::_build_place_sections(VBoxContainer *v) {
	v->add_child(memnew(HSeparator));
	header(v, "Decals & Overlays");
	decal_texture = memnew(EditorResourcePicker);
	decal_texture->set_base_type("Texture2D");
	decal_texture->set_name("DecalTexture");
	decal_texture->set_tooltip_text("The picture (a click in the Textures panel sets it too)");
	v->add_child(decal_texture);
	auto option = [&](const char *p_label, const std::vector<const char *> &p_items, const char *p_tip) {
		HBoxContainer *h = memnew(HBoxContainer);
		Label *l = memnew(Label);
		l->set_text(p_label);
		l->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		h->add_child(l);
		OptionButton *o = memnew(OptionButton);
		for (const char *it : p_items) {
			o->add_item(it);
		}
		o->set_tooltip_text(p_tip);
		h->add_child(o);
		v->add_child(h);
		return o;
	};
	decal_kind = option("Type", { "Overlay", "Decal" },
			"Overlay: geometry on the faces under it, all overlays of a material drawn at once (one draw call). "
			"Decal: Godot's Decal node, projected in the shader (also onto models)");
	decal_outline = option("Outline", { "Rectangle", "Alpha Polygon" }, "Overlay: the rectangle, or a polygon around the opaque pixels (fewer transparent pixels drawn)");
	decal_blend = option("Edges", { "Alpha Scissor", "Alpha Blend" }, "Overlay: hard edges with the opaque faces (fast), or soft edges with the transparent ones");
	decal_overlay_rows[0] = Object::cast_to<Control>(decal_outline->get_parent());
	decal_overlay_rows[1] = Object::cast_to<Control>(decal_blend->get_parent());
	decal_kind->connect("item_selected", callable_mp(this, &HewnDock::_on_decal_kind));
	// the decal's cull mask: the render layers it is projected onto (as in the inspector; only decals have it)
	Label *ll = memnew(Label);
	ll->set_text("Cull mask");
	ll->set_tooltip_text("The render layers the decal is projected onto: put objects that must not get it (things the player drops) on another layer");
	ll->set_mouse_filter(Control::MOUSE_FILTER_PASS);
	v->add_child(ll);
	decal_layers = memnew(GridContainer);
	decal_layers->set_columns(10);
	decal_layers->add_theme_constant_override("h_separation", 1);
	decal_layers->add_theme_constant_override("v_separation", 1);
	for (int bit = 0; bit < 20; bit++) {
		Button *lb = memnew(Button);
		lb->set_toggle_mode(true);
		lb->set_pressed(bit == 0); // (the level's layer)
		lb->set_text(itos(bit + 1));
		lb->set_custom_minimum_size(Vector2(16, 16) * EditorInterface::get_singleton()->get_editor_scale());
		lb->add_theme_font_size_override("font_size", (int)(9 * EditorInterface::get_singleton()->get_editor_scale()));
		lb->set_focus_mode(Control::FOCUS_NONE);
		decal_layers->add_child(lb);
	}
	v->add_child(decal_layers);
	decal_mask_rows[0] = ll;
	decal_mask_rows[1] = decal_layers;
	// the overlay's rows and the decal's mask in one box of a fixed height: switching between them does not
	// change the tab's height (a scroll bar coming and going with it can make the layout swing)
	decal_opts = memnew(VBoxContainer);
	v->add_child(decal_opts);
	v->move_child(decal_opts, decal_overlay_rows[0]->get_index());
	for (Control *c : { decal_overlay_rows[0], decal_overlay_rows[1], decal_mask_rows[0], decal_mask_rows[1] }) {
		c->reparent(decal_opts, false);
	}
	_on_decal_kind(decal_kind->get_selected());
	Button *b = memnew(Button);
	b->set_text("Place");
	b->set_name("PlaceDecal");
	b->set_tooltip_text("Click on a surface (hold and drag: turn it); - / =: size; the size comes from the picture and the Textures panel's pixels per meter");
	b->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::place_decal));
	v->add_child(b);
}

// Overlay: its outline and edges; Decal: its cull mask (each only what it has)
void HewnDock::_on_decal_kind(int p_kind) {
	if (decal_opts && decal_opts->is_inside_tree() && decal_opts->get_custom_minimum_size().y <= 0.0f) {
		float over = 0.0f, mask = 0.0f;
		const float sep = (float)decal_opts->get_theme_constant("separation");
		for (Control *c : decal_overlay_rows) {
			over += c ? c->get_combined_minimum_size().y + sep : 0.0f;
		}
		for (Control *c : decal_mask_rows) {
			mask += c ? c->get_combined_minimum_size().y + sep : 0.0f;
		}
		decal_opts->set_custom_minimum_size(Vector2(0, MAX(over, mask)));
	}
	for (Control *c : decal_overlay_rows) {
		if (c) {
			c->set_visible(p_kind == 0);
		}
	}
	for (Control *c : decal_mask_rows) {
		if (c) {
			c->set_visible(p_kind == 1);
		}
	}
}

void HewnDock::_rebuild_entities() {
	if (!entity_flow) {
		return;
	}
	for (int i = entity_flow->get_child_count() - 1; i >= 0; i--) {
		Node *c = entity_flow->get_child(i);
		entity_flow->remove_child(c);
		c->queue_free();
	}
	const Array defs = entity_defs();
	for (int64_t i = 0; i < defs.size(); i++) {
		const Dictionary d = defs[i];
		Button *b = memnew(Button);
		b->set_text(d["name"]);
		b->set_meta("hw_no_translate", true); // (class names, the same in every language)
		b->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);
		const String cls = d["classname"];
		b->set_tooltip_text(cls.is_empty() ? String("Click on a surface to place it") : String("classname: ") + cls + " (the node is in this group in the game)");
		b->connect("pressed", callable_mp(plugin, &HewnEditorPlugin::place_entity).bind((int)i));
		entity_flow->add_child(b);
	}
	Button *r = memnew(Button);
	r->set_text(String::chr(0x21BB));
	r->set_tooltip_text(String("Reload ") + ENTITY_FILE + ": your own entities, a list of { \"name\", \"classname\", \"properties\": {}, \"color\", \"along_normal\", \"offset\" }");
	r->connect("pressed", callable_mp(this, &HewnDock::_rebuild_entities));
	entity_flow->add_child(r);
}

void HewnDock::set_decal_texture(const String &p_path) {
	if (!decal_texture) {
		return;
	}
	Ref<Texture2D> t = ResourceLoader::get_singleton()->exists(p_path) ? ResourceLoader::get_singleton()->load(p_path) : Ref<Resource>();
	if (t.is_valid()) {
		decal_texture->set_edited_resource(t);
	}
}

// ------------------------------------------------------------------ plugin side

void HewnEditorPlugin::place_entity(int p_index) {
	const Array defs = HewnDock::entity_defs();
	if (p_index < 0 || p_index >= defs.size()) {
		return;
	}
	const Dictionary d = defs[p_index];
	const String kind = d["kind"];
	const String name = d["name"];
	const String cls = d["classname"];
	const Dictionary props = d["properties"];
	const Color color = d["color"];
	const float off = (float)(double)d["offset"];
	Transform3D local(Basis(), Vector3(0, off, 0));
	if (kind == "spot") {
		local.basis = Basis(Vector3(1, 0, 0), (float)Math::PI * 0.5f); // (shines away from the surface)
	}
	start_placing(name, [kind, cls, props, color]() -> Node3D * {
		if (kind == "omni") {
			OmniLight3D *l = memnew(OmniLight3D);
			l->set_param(Light3D::PARAM_RANGE, 5.0f);
			return l;
		}
		if (kind == "spot") {
			SpotLight3D *l = memnew(SpotLight3D);
			l->set_param(Light3D::PARAM_RANGE, 8.0f);
			return l;
		}
		if (kind == "sound") {
			return memnew(AudioStreamPlayer3D);
		}
		HewnEntity3D *e = memnew(HewnEntity3D);
		e->set_classname(cls);
		e->set_properties(props);
		e->set_color(color);
		return e;
	}, (bool)d["along_normal"], false, local);
}

void HewnEditorPlugin::place_decal() {
	Ref<Texture2D> tex = dock && dock->decal_texture ? Ref<Texture2D>(dock->decal_texture->get_edited_resource()) : Ref<Texture2D>();
	if (tex.is_null()) {
		ERR_PRINT("Hewn: pick a picture for the decal first (Create tab, or a click in the Textures panel).");
		return;
	}
	const bool overlay = dock->decal_kind->get_selected() == 0;
	const int outline = dock->decal_outline->get_selected();
	const int blend = dock->decal_blend->get_selected();
	uint32_t mask = 0;
	for (int i = 0; dock->decal_layers && i < dock->decal_layers->get_child_count(); i++) {
		Button *lb = Object::cast_to<Button>(dock->decal_layers->get_child(i));
		mask |= lb && lb->is_pressed() ? (1u << i) : 0u;
	}
	const float ppm = texture_dock ? texture_dock->pixels_per_meter() : 128.0f;
	const bool nearest = texture_dock && texture_dock->get_default_nearest();
	Vector2 size = ppm > 0.0f ? Vector2(tex->get_width(), tex->get_height()) / ppm : Vector2(1, (float)tex->get_height() / MAX(1, tex->get_width()));
	HewnEditorPlugin *self = this;
	start_placing(overlay ? "Overlay" : "Decal", [self, tex, overlay, outline, blend, mask, size, nearest]() -> Node3D * {
		const Vector2 sz = size * self->place_size;
		if (overlay) {
			HewnOverlay3D *o = memnew(HewnOverlay3D);
			o->set_texture(tex);
			o->set_size(sz);
			o->set_depth(MAX(sz.x, sz.y)); // (deep enough for curved surfaces: spheres, cylinders)
			o->set_outline(outline);
			o->set_blend(blend);
			o->set_filter_nearest(nearest);
			return o;
		}
		Decal *d = memnew(Decal);
		d->set_texture(Decal::TEXTURE_ALBEDO, tex);
		d->set_size(Vector3(sz.x, MAX(sz.x, sz.y), sz.y));
		d->set_cull_mask(mask);
		return d;
	}, true, false);
}

void HewnEditorPlugin::place_cutter() {
	// a door-sized box standing on the surface (along its normal: through a wall it is drawn on)
	start_placing("Cutter", []() -> Node3D * {
		HewnCutter3D *c = memnew(HewnCutter3D);
		c->set_size(Vector3(1.0f, 2.0f, 1.0f));
		return c;
	}, false, false, Transform3D(Basis(), Vector3(0, 1.0f, 0)));
}

void HewnEditorPlugin::apply_cutters() {
	std::vector<HewnCutter3D *> cutters;
	for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_selected_nodes()) {
		if (HewnCutter3D *c = Object::cast_to<HewnCutter3D>(v)) {
			cutters.push_back(c);
		}
	}
	if (cutters.empty()) {
		ERR_PRINT("Hewn: select the cutters to apply.");
		return;
	}
	// the meshes as they are, then with the holes for good
	std::vector<std::pair<HewnMesh3D *, Dictionary>> before;
	for (HewnCutter3D *c : cutters) {
		for (HewnMesh3D *m : HewnMesh3D::all_meshes) {
			if (c->cuts(m)) {
				bool seen = false;
				for (const auto &b : before) {
					seen = seen || b.first == m;
				}
				if (!seen) {
					before.push_back({ m, m->get_state() });
				}
			}
		}
	}
	for (HewnCutter3D *c : cutters) {
		c->apply();
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Apply Cutter");
	for (const auto &b : before) {
		ur->add_do_method(b.first, "set_state", b.first->get_state());
		ur->add_undo_method(b.first, "set_state", b.second);
	}
	for (HewnCutter3D *c : cutters) {
		Node *parent = c->get_parent();
		ur->add_do_method(parent, "remove_child", c);
		ur->add_undo_method(parent, "add_child", c, true);
		ur->add_undo_method(parent, "move_child", c, c->get_index());
		ur->add_undo_method(c, "set_owner", c->get_owner());
		ur->add_undo_method(c, "set_active", true);
		ur->add_undo_reference(c);
	}
	ur->commit_action();
}

// A light / sound placed with clicks like a prefab (W: another one, with the last one's panel values).
void HewnEditorPlugin::place_kind(const String &p_kind) {
	Transform3D local(Basis(), Vector3(0, 0.3f, 0));
	if (p_kind == "spot") {
		local = Transform3D(Basis(Vector3(1, 0, 0), (float)Math::PI * 0.5f), Vector3(0, 0.05f, 0)); // (shines away from the surface)
	}
	const String kind = p_kind;
	const String label = kind == "omni" ? "Omni Light" : kind == "spot" ? "Spot Light" : "Sound";
	start_placing(label, [kind]() -> Node3D * {
		if (kind == "omni") {
			OmniLight3D *l = memnew(OmniLight3D);
			l->set_param(Light3D::PARAM_RANGE, 5.0f);
			return l;
		}
		if (kind == "spot") {
			SpotLight3D *l = memnew(SpotLight3D);
			l->set_param(Light3D::PARAM_RANGE, 8.0f);
			return l;
		}
		return memnew(AudioStreamPlayer3D);
	}, true, false, local);
}

void HewnEditorPlugin::place_decal_kind(bool p_overlay) {
	if (dock && dock->decal_kind) {
		dock->decal_kind->select(p_overlay ? 0 : 1);
	}
	place_decal();
}

// Shift+A's nodes: at the 3D cursor, turned like it (lights, sounds, decals and cutters are placed with a click).
void HewnEditorPlugin::add_node_kind(const String &p_kind) {
	if (p_kind == "omni" || p_kind == "spot" || p_kind == "sound") {
		place_kind(p_kind);
		return;
	}
	if (p_kind == "overlay" || p_kind == "decal") {
		place_decal_kind(p_kind == "overlay");
		return;
	}
	if (p_kind == "cutter" && adding_from_menu && !create_at_cursor) {
		// drawn like a box with the Box Builder, then a cutter of that size
		start_shape_tool(hewn::shapes::CUBE);
		shape_make_area = true;
		shape_make_cutter = true;
		return;
	}
	if (p_kind == "cutter") {
		place_cutter();
		return;
	}
	if (p_kind == "area") {
		if (create_at_cursor || !adding_from_menu) {
			Area3D *a = memnew(Area3D);
			a->set_name("Area3D");
			CollisionShape3D *cs = memnew(CollisionShape3D);
			cs->set_name("CollisionShape3D");
			Ref<BoxShape3D> box;
			box.instantiate();
			box->set_size(Vector3(1, 1, 1));
			cs->set_shape(box);
			a->add_child(cs);
			a->set_transform(Transform3D(cursor_basis, cursor_pos + cursor_basis.get_column(1).normalized() * 0.5f));
			_add_node_with_undo(a, "Add Area3D");
		} else {
			// drawn like a box with the Box Builder, then an Area3D of that size
			start_shape_tool(hewn::shapes::CUBE);
			shape_make_area = true;
		}
		return;
	}
	if (p_kind == "sprite" || p_kind == "anim_sprite" || p_kind == "image_plane" || p_kind == "ref_image") {
		// the picture is asked for first (several: the frames of the animated sprite); another one later in its panel
		image_add_kind = p_kind;
		if (!image_dialog) {
			image_dialog = memnew(EditorFileDialog);
			image_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
			const char *exts[] = { "*.png", "*.jpg", "*.jpeg", "*.webp", "*.svg", "*.bmp", "*.tga", "*.exr", "*.hdr", "*.ktx" };
			for (const char *e : exts) {
				image_dialog->add_filter(e);
			}
			image_dialog->connect("file_selected", callable_mp(this, &HewnEditorPlugin::_on_image_picked));
			image_dialog->connect("files_selected", callable_mp(this, &HewnEditorPlugin::_on_images_picked));
			EditorInterface::get_singleton()->get_base_control()->add_child(image_dialog);
		}
		image_dialog->set_file_mode(p_kind == "anim_sprite" ? EditorFileDialog::FILE_MODE_OPEN_FILES : EditorFileDialog::FILE_MODE_OPEN_FILE);
		image_dialog->set_title(p_kind == "anim_sprite" ? "Pick the frames" : "Pick a picture");
		image_add_at_cursor = create_at_cursor || !adding_from_menu;
		image_dialog->popup_file_dialog();
		return;
	}
	if (p_kind == "scatter") {
		// at the scene's origin (its copies stand where they were painted), its brush ready
		HewnScatter3D *sc = memnew(HewnScatter3D);
		sc->set_name("Scatter");
		scatter_tool_next = true;
		_add_node_with_undo(sc, "Add Scatter");
		return;
	}
	Node3D *n = nullptr;
	String name;
	if (p_kind == "path") {
		Path3D *p = memnew(Path3D);
		Ref<Curve3D> c;
		c.instantiate();
		c->add_point(Vector3(-1, 0, 0), Vector3(), Vector3(0.5f, 0, -0.5f));
		c->add_point(Vector3(1, 0, 0), Vector3(-0.5f, 0, 0.5f), Vector3());
		p->set_curve(c);
		n = p;
		name = "Path3D";
	} else if (p_kind == "label") {
		Label3D *l = memnew(Label3D);
		l->set_text("Text");
		n = l;
		name = "Label3D";
	} else if (p_kind == "marker") {
		n = memnew(Marker3D);
		name = "Marker3D";
	} else if (p_kind == "node") {
		n = memnew(Node3D);
		name = "Node3D";
	} else if (p_kind == "camera") {
		n = memnew(Camera3D);
		name = "Camera3D";
	} else if (p_kind == "directional") {
		DirectionalLight3D *l = memnew(DirectionalLight3D);
		l->set_shadow(true);
		n = l;
		name = "DirectionalLight3D";
	}
	if (!n) {
		return;
	}
	n->set_name(name);
	if (adding_from_menu && !create_at_cursor && p_kind != "directional") {
		// placed with a click (on a surface, like the lights); the factory makes another one each time
		memdelete(n);
		const String kind = p_kind;
		const bool was = adding_from_menu;
		adding_from_menu = false;
		start_placing(name, [this, kind]() -> Node3D * { return _make_kind_node(kind); }, kind == "label" || kind == "camera" ? false : true, false, Transform3D(Basis(), Vector3(0, kind == "label" || kind == "camera" ? 1.0f : 0.0f, 0)));
		adding_from_menu = was;
		return;
	}
	Basis b = cursor_basis;
	if (p_kind == "directional") {
		b = Basis(Vector3(1, 0, 0), (float)Math::PI * -0.3f); // (down at an angle)
	}
	n->set_transform(Transform3D(b, cursor_pos));
	_add_node_with_undo(n, "Add " + name);
}

// One of the Add menu's plain nodes (for placing them with clicks).
Node3D *HewnEditorPlugin::_make_kind_node(const String &p_kind) {
	Node3D *n = nullptr;
	if (p_kind == "path") {
		Path3D *p = memnew(Path3D);
		Ref<Curve3D> c;
		c.instantiate();
		c->add_point(Vector3(-1, 0, 0), Vector3(), Vector3(0.5f, 0, -0.5f));
		c->add_point(Vector3(1, 0, 0), Vector3(-0.5f, 0, 0.5f), Vector3());
		p->set_curve(c);
		n = p;
		n->set_name("Path3D");
	} else if (p_kind == "label") {
		Label3D *l = memnew(Label3D);
		l->set_text("Text");
		n = l;
		n->set_name("Label3D");
	} else if (p_kind == "marker") {
		n = memnew(Marker3D);
		n->set_name("Marker3D");
	} else if (p_kind == "node") {
		n = memnew(Node3D);
		n->set_name("Node3D");
	} else if (p_kind == "camera") {
		n = memnew(Camera3D);
		n->set_name("Camera3D");
	}
	return n;
}

void HewnEditorPlugin::_on_image_picked(const String &p_path) {
	PackedStringArray one;
	one.push_back(p_path);
	_on_images_picked(one);
}

void HewnEditorPlugin::_on_images_picked(const PackedStringArray &p_paths) {
	std::vector<Ref<Texture2D>> frames;
	for (const String &p : p_paths) {
		Ref<Texture2D> t = ResourceLoader::get_singleton()->load(p);
		if (t.is_valid()) {
			frames.push_back(t);
		}
	}
	if (frames.empty()) {
		return;
	}
	const Ref<Texture2D> tex = frames[0];
	const float ppm = texture_dock && texture_dock->pixels_per_meter() > 0.0f ? texture_dock->pixels_per_meter() : 128.0f;
	const String base = p_paths[0].get_file().get_basename().validate_node_name();
	if (image_add_kind == "ref_image") {
		// Add > Image > Reference: at the 3D cursor, facing the view; made in an orthographic view it is
		// not shown in perspective ones
		HewnImage3D *ri = memnew(HewnImage3D);
		ri->set_name(base.is_empty() ? String("Image") : base);
		ri->set_texture(tex);
		Basis b;
		if (Camera3D *cam = _viewport_camera()) {
			b = cam->get_global_transform().basis.orthonormalized();
			if (cam->get_projection() == Camera3D::PROJECTION_ORTHOGONAL) {
				ri->set_show_perspective(false);
			}
		}
		ri->set_transform(Transform3D(b, cursor_pos));
		_add_node_with_undo(ri, "Add Reference Image");
		return;
	}
	Node3D *n = nullptr;
	if (image_add_kind == "sprite") {
		Sprite3D *s = memnew(Sprite3D);
		s->set_texture(tex);
		s->set_pixel_size(1.0f / ppm);
		n = s;
	} else if (image_add_kind == "anim_sprite") {
		AnimatedSprite3D *s = memnew(AnimatedSprite3D);
		Ref<SpriteFrames> sf;
		sf.instantiate();
		for (const Ref<Texture2D> &f : frames) {
			sf->add_frame("default", f);
		}
		s->set_sprite_frames(sf);
		s->set_autoplay("default");
		s->set_pixel_size(1.0f / ppm);
		n = s;
	} else {
		MeshInstance3D *mi = memnew(MeshInstance3D);
		Ref<QuadMesh> q;
		q.instantiate();
		q->set_size(Vector2(tex->get_width(), tex->get_height()) / ppm);
		Ref<StandardMaterial3D> mat;
		mat.instantiate();
		mat->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, tex);
		mat->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
		mat->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		if (texture_dock && texture_dock->get_default_nearest()) {
			mat->set_texture_filter(BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS);
		}
		q->set_material(mat);
		mi->set_mesh(q);
		mi->set_meta("hw_image_plane", true); // (its panel: the picture can be changed)
		n = mi;
	}
	n->set_name(base.is_empty() ? String("Image") : base);
	if (image_making) {
		image_made = n; // (for the placing: made, not added)
		return;
	}
	if (!image_add_at_cursor) {
		// placed with clicks: made again from the same pictures each time
		const float lift = tex->get_height() / ppm * 0.5f;
		const PackedStringArray paths = p_paths;
		const String label = n->get_name();
		memdelete(n);
		image_add_at_cursor = true;
		image_place_paths = paths;
		start_placing(label, [this, paths]() -> Node3D * {
			image_making = true;
			_on_images_picked(paths);
			image_making = false;
			Node3D *made = image_made;
			image_made = nullptr;
			return made;
		}, false, false, Transform3D(Basis(), Vector3(0, lift, 0)));
		image_add_at_cursor = false;
		return;
	}
	// standing on the cursor, facing along its forward axis
	n->set_transform(Transform3D(cursor_basis, cursor_pos + cursor_basis.get_column(1).normalized() * (tex->get_height() / ppm * 0.5f)));
	_add_node_with_undo(n, "Add " + String(n->get_name()));
}
