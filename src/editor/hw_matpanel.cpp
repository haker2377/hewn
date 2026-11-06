// The Material tab's texture panel:
// the texture of the selected faces with the settings of its material, changed right there (undoable,
// saved with the material file), the UV shortcuts and the settings new materials get.
#include "hw_editor.h"
#include "hw_editor_util.h"
#include "hw_textures.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/button_group.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/color_picker_button.hpp>
#include <godot_cpp/classes/confirmation_dialog.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/editor_spin_slider.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_flow_container.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include <unordered_map>

using namespace godot;
using hw_editor::T;

namespace {

Label *section(Container *p_parent, const String &p_text) {
	Label *l = memnew(Label);
	l->set_text(p_text);
	l->add_theme_color_override("font_color", Color(0.7f, 0.8f, 1.0f));
	p_parent->add_child(l);
	return l;
}

EditorSpinSlider *slider(Container *p_parent, const String &p_label, double p_max) {
	EditorSpinSlider *s = memnew(EditorSpinSlider);
	s->set_label(p_label);
	s->set_min(0.0);
	s->set_max(p_max);
	s->set_step(0.01);
	s->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	p_parent->add_child(s);
	return s;
}

} // namespace

void HewnDock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_save_material", "material"), &HewnDock::_save_material);
}

void HewnDock::_build_texture_section(VBoxContainer *v) {
	section(v, "Texture");
	tex_title = memnew(Label);
	tex_title->set_clip_text(true);
	tex_title->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	tex_title->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);
	v->add_child(tex_title);
	mat_edit = memnew(Button);
	mat_edit->set_text("Edit in Inspector");
	mat_edit->set_tooltip_text("All the settings of this material in Godot's Inspector (any material: also shader and visual shader ones)");
	mat_edit->connect("pressed", callable_mp(this, &HewnDock::_on_mat_edit));
	v->add_child(mat_edit);
	// a material whose texture is not the image it is mapped to (or not mapped at all)
	mapping_row = memnew(HBoxContainer);
	mapping_label = memnew(Label);
	mapping_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	mapping_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	mapping_label->set_custom_minimum_size(Vector2(120, 0));
	mapping_label->add_theme_color_override("font_color", Color(1.0f, 0.8f, 0.4f));
	mapping_row->add_child(mapping_label);
	Button *repair = memnew(Button);
	repair->set_text("Repair");
	repair->set_tooltip_text("Map the material to the texture it shows: clicking that image in the Textures tab picks this material");
	repair->connect("pressed", callable_mp(this, &HewnDock::_on_repair_mapping));
	mapping_row->add_child(repair);
	v->add_child(mapping_row);
	tex_preview = memnew(TextureRect);
	tex_preview->set_name("TexturePreview");
	tex_preview->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	tex_preview->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	tex_preview->set_custom_minimum_size(Vector2(0, 128));
	tex_preview->set_texture_filter(CanvasItem::TEXTURE_FILTER_NEAREST); // (its pixels as they are)
	v->add_child(tex_preview);
	mat_props = memnew(VBoxContainer);
	v->add_child(mat_props);
	// the filtering of the texture: crisp pixels or smooth
	HBoxContainer *fh = memnew(HBoxContainer);
	mat_props->add_child(fh);
	Ref<ButtonGroup> fg;
	fg.instantiate();
	filter_closest = memnew(Button);
	filter_closest->set_text("Closest");
	filter_closest->set_tooltip_text("Nearest filtering: crisp pixels");
	filter_linear = memnew(Button);
	filter_linear->set_text("Linear");
	filter_linear->set_tooltip_text("Linear filtering: smooth");
	for (Button *b : { filter_closest, filter_linear }) {
		b->set_toggle_mode(true);
		b->set_button_group(fg);
		b->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		fh->add_child(b);
	}
	filter_closest->connect("pressed", callable_mp(this, &HewnDock::_on_mat_filter).bind(true));
	filter_linear->connect("pressed", callable_mp(this, &HewnDock::_on_mat_filter).bind(false));
	tex_alpha = memnew(CheckBox);
	tex_alpha->set_text("Texture as Alpha");
	tex_alpha->set_tooltip_text("The texture's alpha cuts the surface out (transparent pixels are holes: fences, foliage)");
	tex_alpha->connect("toggled", callable_mp(this, &HewnDock::_on_mat_flag).bind(String("alpha")));
	mat_props->add_child(tex_alpha);
	tex_vcolor = memnew(CheckBox);
	tex_vcolor->set_text("Vertex Colors");
	tex_vcolor->set_tooltip_text("The vertex colors (Vertex Paint) tint the texture");
	tex_vcolor->connect("toggled", callable_mp(this, &HewnDock::_on_mat_flag).bind(String("vertex_color")));
	mat_props->add_child(tex_vcolor);
	mat_rough = slider(mat_props, T("Roughness"), 1.0);
	mat_rough->connect("value_changed", callable_mp(this, &HewnDock::_on_mat_value).bind(String("roughness")));
	mat_metal = slider(mat_props, T("Metallic"), 1.0);
	mat_metal->connect("value_changed", callable_mp(this, &HewnDock::_on_mat_value).bind(String("metallic")));
	// the rest folded
	Button *more = memnew(Button);
	more->set_text("More");
	more->set_flat(true);
	more->set_toggle_mode(true);
	more->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	mat_props->add_child(more);
	VBoxContainer *mv = memnew(VBoxContainer);
	mv->hide();
	mat_props->add_child(mv);
	more->connect("toggled", callable_mp((CanvasItem *)mv, &CanvasItem::set_visible));
	mat_emit = slider(mv, T("Emission"), 16.0);
	mat_emit->set_tooltip_text("Glow strength (0: none)");
	mat_emit->connect("value_changed", callable_mp(this, &HewnDock::_on_mat_value).bind(String("emission_energy_multiplier")));
	HBoxContainer *eh = memnew(HBoxContainer);
	Label *el = memnew(Label);
	el->set_text("Emission Color");
	el->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	eh->add_child(el);
	mat_emit_color = memnew(ColorPickerButton);
	mat_emit_color->set_custom_minimum_size(Vector2(60, 0));
	mat_emit_color->connect("color_changed", callable_mp(this, &HewnDock::_on_mat_emit_color));
	eh->add_child(mat_emit_color);
	mv->add_child(eh);
	mat_spec = slider(mv, T("Specular"), 1.0);
	mat_spec->connect("value_changed", callable_mp(this, &HewnDock::_on_mat_value).bind(String("metallic_specular")));
	Button *bleed = memnew(Button);
	bleed->set_text("Fix Alpha Bleed...");
	bleed->set_tooltip_text("The color of fully transparent pixels becomes black in the image file, so no light or dark seam shows around cut-out edges when the texture is filtered or shrunk");
	bleed->connect("pressed", callable_mp(this, &HewnDock::_on_fix_bleed));
	mat_props->add_child(bleed);
	bleed_dialog = memnew(ConfirmationDialog);
	bleed_dialog->set_title(T("Fix Alpha Bleed"));
	bleed_dialog->connect("confirmed", callable_mp(this, &HewnDock::_on_fix_bleed_confirmed));
	add_child(bleed_dialog);


	// what new materials get (the texture browser's materials)
	section(v, "Default Material Settings");
	HBoxContainer *dh = memnew(HBoxContainer);
	v->add_child(dh);
	Ref<ButtonGroup> dg;
	dg.instantiate();
	default_closest = memnew(Button);
	default_closest->set_text("Closest");
	default_linear = memnew(Button);
	default_linear->set_text("Linear");
	for (Button *b : { default_closest, default_linear }) {
		b->set_toggle_mode(true);
		b->set_button_group(dg);
		b->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		b->set_tooltip_text("The filtering of materials made for textures from now on");
		dh->add_child(b);
	}
	default_closest->connect("pressed", callable_mp(this, &HewnDock::_on_default_filter).bind(true));
	default_linear->connect("pressed", callable_mp(this, &HewnDock::_on_default_filter).bind(false));
	HFlowContainer *df = memnew(HFlowContainer);
	v->add_child(df);
	Button *ed = memnew(Button);
	ed->set_text("Edit Default Material");
	ed->set_tooltip_text("The material new ones are copied from (everything but the texture and the filtering): opens it in the Inspector, made on first use in the materials folder");
	ed->connect("pressed", callable_mp(this, &HewnDock::_on_edit_default));
	df->add_child(ed);
	Button *fm = memnew(Button);
	fm->set_text("Fix Material Mapping");
	fm->set_tooltip_text("Materials of the project made by hand, by a script or as shader materials get mapped to the image they show, so clicking that image uses them instead of making a new material");
	fm->connect("pressed", callable_mp(this, &HewnDock::_on_fix_mapping));
	df->add_child(fm);
	// UV lock: off whenever the editor starts
	plugin->uv_lock = false;
	if (uv_lock_button) {
		uv_lock_button->set_pressed_no_signal(false);
	}
	if (uv_lock_tool) {
		uv_lock_tool->set_pressed_no_signal(false);
	}
}

// The material of the selected faces (edit mode) or objects: null with r_mixed when they differ.
Ref<BaseMaterial3D> HewnDock::_selected_material(bool &r_mixed, Ref<Material> &r_any) const {
	r_mixed = false;
	std::vector<std::pair<HewnMesh3D *, std::vector<int>>> sets;
	HewnMesh3D *edited = plugin->get_edited();
	if (edited && plugin->mode != HewnEditorPlugin::MODE_OBJECT) {
		sets.push_back({ edited, edited->selection.faces });
	} else {
		const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
		for (int64_t i = 0; i < sel.size(); i++) {
			if (HewnMesh3D *n = Object::cast_to<HewnMesh3D>(sel[i])) {
				sets.push_back({ n, {} });
			}
		}
		if (sets.empty() && edited) {
			sets.push_back({ edited, {} });
		}
	}
	bool first = true;
	Ref<Material> found;
	for (auto &s : sets) {
		const hewn::Mesh &m = s.first->get_data();
		std::vector<int> fs = s.second;
		if (fs.empty()) {
			for (int f = 0; f < (int)m.faces.size(); f++) {
				fs.push_back(f);
			}
		}
		for (int f : fs) {
			if (f < 0 || f >= (int)m.faces.size()) {
				continue;
			}
			const Ref<Material> mat = s.first->get_material_for_slot(m.faces[f].material);
			if (first) {
				found = mat;
				first = false;
			} else if (mat != found) {
				r_mixed = true;
				r_any = Ref<Material>();
				return Ref<BaseMaterial3D>();
			}
		}
	}
	r_any = found;
	return found;
}

void HewnDock::_refresh_texture_section() {
	if (!tex_preview) {
		return;
	}
	bool mixed = false;
	Ref<Material> any;
	shown_material = _selected_material(mixed, any);
	shown_any = any;
	Ref<Texture2D> tex = shown_material.is_valid() ? shown_material->get_texture(BaseMaterial3D::TEXTURE_ALBEDO) : Ref<Texture2D>();
	const String shown_tex = HewnTextureBrowser::texture_of(any);
	if (tex.is_null() && !shown_tex.is_empty()) {
		tex = ResourceLoader::get_singleton()->load(shown_tex); // (a shader material: its texture parameter)
	}
	mat_edit->set_visible(any.is_valid());
	const String mapped = any.is_valid() && any->has_meta("hw_image") ? String(any->get_meta("hw_image")) : String();
	mapping_row->set_visible(any.is_valid() && !shown_tex.is_empty() && mapped != shown_tex && !any->get_path().contains("::") && !any->has_meta("hw_blend_material"));
	mapping_label->set_text(mapped.is_empty() ? T("Not mapped to its image") : vformat(T("Mapped to %s, shows another image"), mapped.get_file()));
	tex_preview->set_texture(tex);
	tex_preview->set_visible(tex.is_valid());
	String title;
	if (mixed) {
		title = T("Multiple textures");
	} else if (shown_material.is_valid()) {
		title = tex.is_valid() && !tex->get_path().is_empty() ? tex->get_path().get_file() : shown_material->get_path().get_file();
		if (title.is_empty()) {
			title = T("(built-in material)");
		}
	} else if (any.is_valid()) {
		title = any->get_path().get_file() + "  (" + any->get_class() + ")";
	} else {
		title = T("(default material)");
	}
	tex_title->set_text(title);
	tex_title->set_tooltip_text(shown_material.is_valid() ? shown_material->get_path() : String());
	mat_props->set_visible(shown_material.is_valid());
	if (shown_material.is_null()) {
		return;
	}
	mat_syncing = true;
	const BaseMaterial3D::TextureFilter tf = shown_material->get_texture_filter();
	const bool nearest = tf == BaseMaterial3D::TEXTURE_FILTER_NEAREST || tf == BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS ||
			tf == BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC;
	filter_closest->set_pressed_no_signal(nearest);
	filter_linear->set_pressed_no_signal(!nearest);
	tex_alpha->set_pressed_no_signal(shown_material->get_transparency() != BaseMaterial3D::TRANSPARENCY_DISABLED);
	tex_vcolor->set_pressed_no_signal(shown_material->get_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR));
	mat_rough->set_value_no_signal(shown_material->get_roughness());
	mat_metal->set_value_no_signal(shown_material->get_metallic());
	mat_emit->set_value_no_signal(shown_material->get_feature(BaseMaterial3D::FEATURE_EMISSION) ? shown_material->get_emission_energy_multiplier() : 0.0);
	mat_emit_color->set_pick_color(shown_material->get_emission());
	mat_spec->set_value_no_signal(shown_material->get_specular());
	mat_syncing = false;
	if (plugin->texture_dock) {
		const bool dn = plugin->texture_dock->get_default_nearest();
		default_closest->set_pressed_no_signal(dn);
		default_linear->set_pressed_no_signal(!dn);
	}
}

// A material file is saved at once (it is shared by every face with its texture); one inside a scene
// goes with the scene.
void HewnDock::_save_material(const Ref<Material> &p_material) {
	if (p_material.is_valid() && p_material->get_path().begins_with("res://") && !p_material->get_path().contains("::")) {
		ResourceSaver::get_singleton()->save(p_material, p_material->get_path());
	}
	if (p_material == shown_material) {
		_refresh_texture_section();
	}
}

void HewnDock::_set_material_props(const String &p_action, const Dictionary &p_props) {
	if (shown_material.is_null() || mat_syncing) {
		return;
	}
	Ref<BaseMaterial3D> m = shown_material;
	EditorUndoRedoManager *ur = plugin->get_undo_redo();
	ur->create_action("Hewn: Material " + p_action, UndoRedo::MERGE_ENDS, m.ptr());
	const Array keys = p_props.keys();
	for (int64_t i = 0; i < keys.size(); i++) {
		const String k = keys[i];
		ur->add_do_property(m.ptr(), k, p_props[k]);
		ur->add_undo_property(m.ptr(), k, m->get(k));
	}
	ur->add_do_method(this, "_save_material", m);
	ur->add_undo_method(this, "_save_material", m);
	ur->commit_action();
}

void HewnDock::_on_mat_filter(bool p_nearest) {
	Dictionary d;
	d["texture_filter"] = p_nearest ? BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS : BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS;
	_set_material_props("Filter", d);
}

void HewnDock::_on_mat_flag(bool p_on, const String &p_which) {
	Dictionary d;
	if (p_which == "alpha") {
		d["transparency"] = p_on ? BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR : BaseMaterial3D::TRANSPARENCY_DISABLED;
	} else {
		d["vertex_color_use_as_albedo"] = p_on;
	}
	_set_material_props(p_which.capitalize(), d);
}

void HewnDock::_on_mat_value(double p_value, const String &p_prop) {
	Dictionary d;
	if (p_prop == "emission_energy_multiplier") {
		d["emission_enabled"] = p_value > 0.0;
	}
	d[p_prop] = p_value;
	_set_material_props(p_prop.capitalize(), d);
}

void HewnDock::_on_mat_emit_color(const Color &p_color) {
	Dictionary d;
	d["emission"] = p_color;
	_set_material_props("Emission Color", d);
}

void HewnDock::_on_fix_bleed() {
	Ref<Texture2D> tex = shown_material.is_valid() ? shown_material->get_texture(BaseMaterial3D::TEXTURE_ALBEDO) : Ref<Texture2D>();
	if (tex.is_null() || tex->get_path().get_extension().to_lower() != "png") {
		bleed_dialog->set_text(T("Only a PNG texture file can be fixed."));
		bleed_dialog->get_ok_button()->set_disabled(true);
	} else {
		bleed_dialog->set_text(vformat(T("Fully transparent pixels of %s get a black color (the file is changed)."), tex->get_path().get_file()));
		bleed_dialog->get_ok_button()->set_disabled(false);
	}
	bleed_dialog->popup_centered();
}

void HewnDock::_on_fix_bleed_confirmed() {
	Ref<Texture2D> tex = shown_material.is_valid() ? shown_material->get_texture(BaseMaterial3D::TEXTURE_ALBEDO) : Ref<Texture2D>();
	if (tex.is_null()) {
		return;
	}
	const String path = tex->get_path();
	Ref<Image> img = Image::load_from_file(ProjectSettings::get_singleton()->globalize_path(path));
	if (img.is_null() || img->is_empty()) {
		return;
	}
	img->decompress();
	img->convert(Image::FORMAT_RGBA8);
	int n = 0;
	for (int y = 0; y < img->get_height(); y++) {
		for (int x = 0; x < img->get_width(); x++) {
			const Color c = img->get_pixel(x, y);
			if (c.a < 0.01f && (c.r != 0.0f || c.g != 0.0f || c.b != 0.0f)) {
				img->set_pixel(x, y, Color(0, 0, 0, c.a));
				n++;
			}
		}
	}
	if (n > 0 && img->save_png(ProjectSettings::get_singleton()->globalize_path(path)) == OK) {
		if (EditorFileSystem *fs = EditorInterface::get_singleton()->get_resource_filesystem()) {
			PackedStringArray files;
			files.push_back(path);
			fs->reimport_files(files);
		}
	}
	if (plugin->texture_dock) {
		plugin->texture_dock->set_status(vformat(T("%s: %d pixels fixed"), path.get_file(), n));
	}
}

void HewnDock::_on_mat_edit() {
	if (shown_any.is_valid()) {
		hw_editor::edit_in_inspector(shown_any);
	}
}

void HewnDock::_on_repair_mapping() {
	if (shown_any.is_valid() && plugin->texture_dock) {
		plugin->texture_dock->map_material(shown_any, HewnTextureBrowser::texture_of(shown_any));
		_refresh_texture_section();
	}
}

void HewnDock::_on_fix_mapping() {
	if (plugin->texture_dock) {
		plugin->texture_dock->fix_material_mapping();
		_refresh_texture_section();
	}
}

void HewnDock::_on_edit_default() {
	if (plugin->texture_dock && plugin->texture_dock->ask_material_dir(callable_mp(this, &HewnDock::_on_edit_default))) {
		return;
	}
	if (plugin->texture_dock) {
		Ref<Material> m = plugin->texture_dock->default_material(true);
		if (m.is_valid()) {
			hw_editor::edit_in_inspector(m);
		}
	}
}

void HewnDock::_on_box_project() {
	plugin->uv_tool("box:" + String::num(box_scale->get_value()));
}

void HewnDock::_on_uv_box_project() {
	plugin->uv_tool("box:" + String::num(uv_box_scale->get_value()));
}

void HewnDock::_on_uv_lock(bool p_on) {
	plugin->uv_lock = p_on;
	for (Button *b : { (Button *)uv_lock_button, uv_lock_tool }) {
		if (b && b->is_pressed() != p_on) {
			b->set_pressed_no_signal(p_on);
		}
	}
}

void HewnDock::_on_default_filter(bool p_nearest) {
	if (plugin->texture_dock) {
		plugin->texture_dock->set_default_nearest(p_nearest);
	}
}
