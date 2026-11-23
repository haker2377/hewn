// Texture Paint mode: brushes on a mesh's textures (any MeshInstance3D, Hewn's or not). What is painted: the
// material's albedo texture (UV1) or a Detail layer of the material (UV1 or UV2: a picture over the tiled
// material, on the lightmap's unwrap). Each texel under the brush is found from the mesh's triangles: its point on
// the surface brought to the screen, inside the brush (a round falloff or a stamp picture, turned, scattered), not
// behind the mesh itself; a stencil picture over the view gives the colors. The pen's pressure, tilt and speed
// change the size and the strength. A stroke is one undo step (the 64x64 tiles it changed); the files are written
// when the scene is saved and when the mode is left.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/curve.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/editor_inspector.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;
using namespace hw_editor;

namespace {

const int TILE = 64;

void toast(const String &p_text, bool p_warn = false) {
	if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
		t->push_toast(p_text, p_warn ? EditorToaster::SEVERITY_WARNING : EditorToaster::SEVERITY_INFO);
	}
}

float falloff_of(int p_type, const Ref<Curve> &p_curve, float d) {
	if (d >= 1.0f) {
		return 0.0f;
	}
	const float k = 1.0f - d;
	switch (p_type) {
		case 1:
			return std::sqrt(MAX(0.0f, 1.0f - d * d));
		case 2:
			return std::sqrt(k);
		case 3:
			return k * k;
		case 4:
			return k;
		case 5:
			return 1.0f;
		case 6:
			return HewnEditorPlugin::custom_falloff(p_curve, d);
		default:
			return k * k * (3.0f - 2.0f * k);
	}
}

uint32_t hash32(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

float rnd(uint32_t &r_seed) {
	r_seed = hash32(r_seed + 0x9e3779b9u);
	return (float)(r_seed & 0xffffff) / (float)0xffffff; // 0..1
}

// A picture's RGBA8 pixels (decompressed, without mipmaps).
bool image_pixels(const Ref<Image> &p_img, std::vector<uint8_t> &r_px, int &r_w, int &r_h) {
	if (p_img.is_null() || p_img->is_empty()) {
		return false;
	}
	Ref<Image> img = p_img->duplicate();
	if (img->is_compressed() && img->decompress() != OK) {
		return false;
	}
	if (img->has_mipmaps()) {
		img->clear_mipmaps();
	}
	if (img->get_format() != Image::FORMAT_RGBA8) {
		img->convert(Image::FORMAT_RGBA8);
	}
	r_w = img->get_width();
	r_h = img->get_height();
	const PackedByteArray d = img->get_data();
	r_px.assign(d.ptr(), d.ptr() + d.size());
	return (int)r_px.size() == r_w * r_h * 4;
}

Ref<Image> make_image(const std::vector<uint8_t> &p_px, int p_w, int p_h) {
	PackedByteArray d;
	d.resize((int64_t)p_px.size());
	memcpy(d.ptrw(), p_px.data(), p_px.size());
	return Image::create_from_data(p_w, p_h, false, Image::FORMAT_RGBA8, d);
}

// A bilinear sample of RGBA8 pixels at (u, v) in 0..1 (outside: nothing).
Color sample(const std::vector<uint8_t> &p_px, int p_w, int p_h, float u, float v) {
	if (p_w <= 0 || p_h <= 0 || u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f) {
		return Color(0, 0, 0, 0);
	}
	const float x = u * p_w - 0.5f, y = v * p_h - 0.5f;
	const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
	const float fx = x - x0, fy = y - y0;
	auto at = [&](int xx, int yy) {
		xx = CLAMP(xx, 0, p_w - 1);
		yy = CLAMP(yy, 0, p_h - 1);
		const uint8_t *p = &p_px[((size_t)yy * p_w + xx) * 4];
		return Color(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
	};
	return at(x0, y0).lerp(at(x0 + 1, y0), fx).lerp(at(x0, y0 + 1).lerp(at(x0 + 1, y0 + 1), fx), fy);
}

// Where a picture can be written back as it is: a file of its own (not inside another resource / an imported scene).
bool writable_picture(const String &p_path) {
	if (p_path.is_empty() || p_path.contains("::")) {
		return false;
	}
	const String e = p_path.get_extension().to_lower();
	return e == "png" || e == "jpg" || e == "jpeg" || e == "webp";
}

// A material changed in place: one in the scene, or a material file of its own (not one an imported model holds).
bool editable_material(const Ref<Material> &p_mat) {
	const String p = p_mat->get_path();
	if (p.is_empty()) {
		return true;
	}
	if (p.contains("::")) {
		const String owner = p.get_slice("::", 0).get_extension().to_lower();
		return owner == "tscn" || owner == "scn" || owner == "tres" || owner == "res";
	}
	const String e = p.get_extension().to_lower();
	return e == "tres" || e == "res" || e == "material";
}

// Is this material on any other node of the scene than p_self?
bool material_shared(const Ref<Material> &p_mat, Node *p_self) {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return false;
	}
	if (!p_mat->get_path().is_empty() && !p_mat->get_path().contains("::")) {
		return true; // (a file of its own: other scenes may use it)
	}
	std::vector<Node *> stack = { root };
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(x);
		if (!mi || mi == p_self || mi->get_mesh().is_null()) {
			continue;
		}
		for (int s = 0; s < mi->get_mesh()->get_surface_count(); s++) {
			if (mi->get_active_material(s) == p_mat) {
				return true;
			}
		}
	}
	return false;
}

// A new picture's file: next to the scene, named after the node and what it holds.
String new_picture_path(Node *p_node, const String &p_what) {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	String dir = root && !root->get_scene_file_path().is_empty() ? root->get_scene_file_path().get_base_dir() : String("res://");
	if (!dir.ends_with("/")) {
		dir += "/";
	}
	const String base = String(p_node->get_name()).to_snake_case().validate_filename() + "_" + p_what;
	String path = dir + base + ".png";
	for (int i = 2; FileAccess::file_exists(path) && i < 10000; i++) {
		path = dir + base + vformat("_%d", i) + ".png";
	}
	return path;
}

bool save_picture(const Ref<Image> &p_img, const String &p_path) {
	const String g = ProjectSettings::get_singleton()->globalize_path(p_path);
	const String e = p_path.get_extension().to_lower();
	Error err;
	if (e == "jpg" || e == "jpeg") {
		err = p_img->save_jpg(g, 0.92f);
	} else if (e == "webp") {
		err = p_img->save_webp(g, false);
	} else {
		err = p_img->save_png(g);
	}
	return err == OK;
}

// A new picture written and imported: its texture (null when that failed).
Ref<Texture2D> new_picture(const Ref<Image> &p_img, const String &p_path) {
	if (!save_picture(p_img, p_path)) {
		return Ref<Texture2D>();
	}
	EditorFileSystem *fs = EditorInterface::get_singleton()->get_resource_filesystem();
	fs->update_file(p_path);
	PackedStringArray files;
	files.push_back(p_path);
	fs->reimport_files(files);
	return ResourceLoader::get_singleton()->load(p_path, "Texture2D", ResourceLoader::CACHE_MODE_REPLACE);
}

} // namespace

// ------------------------------------------------------------------ the mode

MeshInstance3D *HewnEditorPlugin::tp_mesh() const {
	MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(tp_node));
	return mi && mi->is_inside_tree() && mi->get_mesh().is_valid() ? mi : nullptr;
}

bool HewnEditorPlugin::enter_tpaint() {
	MeshInstance3D *mi = get_edited();
	if (!mi) {
		const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
		for (int64_t i = 0; i < sel.size() && !mi; i++) {
			mi = Object::cast_to<MeshInstance3D>(sel[i].operator Object *());
		}
	}
	if (!mi && tp_mesh()) {
		mi = tp_mesh();
	}
	if (!mi || mi->get_mesh().is_null()) {
		toast(T("Texture Paint: select a mesh"));
		return false;
	}
	if (vpaint) {
		exit_vpaint();
	}
	if (sculpt) {
		exit_sculpt();
	}
	if (pose) {
		exit_pose(false);
	}
	if (rig) {
		exit_rig(false);
	}
	if (wpaint) {
		exit_wpaint(false);
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	if (mode != MODE_OBJECT) {
		set_mode(MODE_OBJECT);
	}
	tp_node = ObjectID(mi->get_instance_id());
	tpaint = true;
	tp_painting = tp_resizing = false;
	tp_stencil_drag = 0;
	tp_surf.clear();
	// out of Godot's selection meanwhile (no gizmo, no box): the inspector keeps showing it
	tp_detached = true;
	EditorInterface::get_singleton()->get_selection()->clear();
	EditorInterface::get_singleton()->get_inspector()->call_deferred("edit", mi);
	EditorFileSystem *fs = EditorInterface::get_singleton()->get_resource_filesystem();
	const Callable re = callable_mp(this, &HewnEditorPlugin::_tp_reimported);
	if (!fs->is_connected("resources_reimported", re)) {
		fs->connect("resources_reimported", re);
	}
	if (dock) {
		dock->show_paint_bar(true);
		dock->refresh();
	}
	_tp_prepare(false);
	update_overlays();
	return true;
}

void HewnEditorPlugin::exit_tpaint(bool p_reselect) {
	if (!tpaint) {
		return;
	}
	if (tp_painting) {
		_tp_stroke_end();
	}
	tpaint = false;
	tp_resizing = false;
	tp_stencil_drag = 0;
	if (Input::get_singleton()->get_mouse_mode() == Input::MOUSE_MODE_HIDDEN) {
		Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
	}
	// the pictures written; the files' own textures shown again (with their mipmaps)
	tp_save_all();
	PackedStringArray shown;
	for (HwPaintTexture &t : tp_tex) {
		if (t.texture.is_valid() && !t.path.is_empty() && FileAccess::file_exists(t.path)) {
			shown.push_back(t.path);
		}
	}
	if (!shown.is_empty()) {
		EditorInterface::get_singleton()->get_resource_filesystem()->reimport_files(shown);
	}
	for (HwPaintTexture &t : tp_tex) {
		t.px.clear();
		t.orig.clear();
		t.px.shrink_to_fit();
		t.orig.shrink_to_fit();
		t.w = t.h = 0; // (loaded again from its file when painted again)
	}
	EditorFileSystem *fs = EditorInterface::get_singleton()->get_resource_filesystem();
	const Callable re = callable_mp(this, &HewnEditorPlugin::_tp_reimported);
	if (fs->is_connected("resources_reimported", re)) {
		fs->disconnect("resources_reimported", re);
	}
	MeshInstance3D *mi = tp_mesh();
	if (tp_detached) {
		tp_detached = false;
		if (p_reselect && mi) {
			EditorSelection *es = EditorInterface::get_singleton()->get_selection();
			es->clear();
			es->add_node(mi);
		}
	}
	tp_surf.clear();
	tp_zbuf.clear();
	if (dock) {
		dock->show_paint_bar(false);
		dock->refresh();
	}
	update_overlays();
}

// A texture of ours imported again (saved, or the file changed): its painting shown again over it.
void HewnEditorPlugin::_tp_reimported(const PackedStringArray &p_files) {
	if (!tpaint) {
		return;
	}
	for (HwPaintTexture &t : tp_tex) {
		if (!t.px.empty() && p_files.has(t.path)) {
			t.dirty = true;
		}
	}
}

void HewnEditorPlugin::tp_settings_changed() {
	tp_surf.clear();
	if (tpaint) {
		_tp_prepare(false);
	}
	update_overlays();
}

// ------------------------------------------------------------------ what is painted

// The surfaces of the painted mesh and their textures. With p_setup what is missing is made first (one undo step):
// a material, the picture (a new file next to the scene, or a copy of one that cannot be written), the Detail
// layer, a copy of the material when it must be this mesh's own (UV2), Hewn's UV2.
bool HewnEditorPlugin::_tp_prepare(bool p_setup) {
	MeshInstance3D *mi = tp_mesh();
	if (!mi) {
		return false;
	}
	HewnMesh3D *hn = Object::cast_to<HewnMesh3D>(mi);
	const int into = dock && dock->tp_into ? dock->tp_into->get_selected() : 0; // 0 albedo, 1 detail
	const int uvch = into == 0 ? 0 : (dock && dock->tp_uv ? dock->tp_uv->get_selected() : 0); // 0 UV1, 1 UV2
	const String prop = into == 0 ? "albedo_texture" : "detail_albedo";
	const int new_size = dock && dock->tp_size ? (512 << CLAMP(dock->tp_size->get_selected(), 0, 3)) : 1024;
	if (p_setup) {
		EditorUndoRedoManager *ur = get_undo_redo();
		bool any = false;
		auto begin = [&]() {
			if (!any) {
				any = true;
				ur->create_action("Texture Paint: Set Up", UndoRedo::MERGE_DISABLE, mi);
			}
		};
		PackedStringArray made;
		if (uvch == 1 && hn && !hn->get_lightmap_uv2()) {
			begin();
			ur->add_do_property(hn, "lightmap_uv2", true);
			ur->add_undo_property(hn, "lightmap_uv2", false);
		}
		Ref<Mesh> mesh = mi->get_mesh();
		TypedArray<Material> hn_mats = hn ? hn->get_materials() : TypedArray<Material>();
		bool hn_mats_changed = false;
		std::vector<std::pair<int, Ref<Material>>> slot_uses;
		Dictionary hn_state_before;
		for (int s = 0; s < mesh->get_surface_count(); s++) {
			Ref<Material> m = mi->get_active_material(s);
			Ref<BaseMaterial3D> bm = m;
			if (m.is_valid() && bm.is_null()) {
				continue; // (a shader material: not ours to paint)
			}
			if (uvch == 1 && !hn) {
				const Array arr = mesh->surface_get_arrays(s);
				if (arr.size() <= Mesh::ARRAY_TEX_UV2 || PackedVector2Array(arr[Mesh::ARRAY_TEX_UV2]).is_empty()) {
					toast(T("Texture Paint: this mesh has no UV2 (Godot's Mesh menu: Unwrap UV2 for Lightmap/AO)"), true);
					continue;
				}
			}
			const bool hewn_default = hn && m.is_valid() && m == HewnMesh3D::get_default_material();
			Ref<BaseMaterial3D> use = bm;
			if (bm.is_null() || hewn_default) {
				Ref<StandardMaterial3D> nm;
				nm.instantiate();
				if (bm.is_valid()) {
					nm->set_albedo(bm->get_albedo());
				}
				use = nm;
			} else if (!editable_material(bm) || (uvch == 1 && material_shared(bm, mi))) {
				use = bm->duplicate(); // (this mesh's own: the model's file is not written, a UV2 layer is not shared)
			}
			// the picture
			Ref<Texture2D> tex = use->get(prop);
			if (tex.is_null() || !writable_picture(tex->get_path())) {
				Ref<Image> img;
				if (tex.is_valid() && tex->get_image().is_valid()) {
					img = tex->get_image();
					if (img->is_compressed()) {
						img->decompress();
					}
					img->clear_mipmaps();
					img->convert(Image::FORMAT_RGBA8);
				} else {
					img = Image::create_empty(new_size, new_size, false, Image::FORMAT_RGBA8);
					img->fill(into == 0 ? Color(use->get_albedo().r, use->get_albedo().g, use->get_albedo().b, 1.0f) : Color(0, 0, 0, 0));
				}
				const String path = new_picture_path(mi, into == 0 ? "albedo" : (uvch == 1 ? "detail_uv2" : "detail"));
				Ref<Texture2D> nt = new_picture(img, path);
				if (nt.is_null()) {
					toast(vformat(T("Texture Paint: could not write %s"), path), true);
					continue;
				}
				made.push_back(path);
				if (use == bm) {
					begin();
					ur->add_do_property(bm.ptr(), prop, nt);
					ur->add_undo_property(bm.ptr(), prop, tex);
					if (into == 0 && tex.is_null() && bm->get_albedo() != Color(1, 1, 1)) {
						ur->add_do_property(bm.ptr(), "albedo_color", Color(1, 1, 1, bm->get_albedo().a)); // (the picture holds the color now)
						ur->add_undo_property(bm.ptr(), "albedo_color", bm->get_albedo());
					}
				} else {
					use->set(prop, nt);
					if (into == 0 && tex.is_null()) {
						use->set_albedo(Color(1, 1, 1, use->get_albedo().a));
					}
				}
			}
			if (into == 1) {
				const int layer = uvch == 1 ? BaseMaterial3D::DETAIL_UV_2 : BaseMaterial3D::DETAIL_UV_1;
				if (use == bm) {
					if (!bm->get_feature(BaseMaterial3D::FEATURE_DETAIL) || bm->get_detail_uv() != layer) {
						begin();
						ur->add_do_property(bm.ptr(), "detail_enabled", true);
						ur->add_undo_property(bm.ptr(), "detail_enabled", bm->get_feature(BaseMaterial3D::FEATURE_DETAIL));
						ur->add_do_property(bm.ptr(), "detail_uv_layer", layer);
						ur->add_undo_property(bm.ptr(), "detail_uv_layer", bm->get_detail_uv());
						ur->add_do_property(bm.ptr(), "detail_blend_mode", BaseMaterial3D::BLEND_MODE_MIX);
						ur->add_undo_property(bm.ptr(), "detail_blend_mode", bm->get_detail_blend_mode());
					}
				} else {
					use->set_feature(BaseMaterial3D::FEATURE_DETAIL, true);
					use->set_detail_uv((BaseMaterial3D::DetailUV)layer);
					use->set_detail_blend_mode(BaseMaterial3D::BLEND_MODE_MIX);
				}
			}
			if (use == bm) {
				continue;
			}
			// the new / copied material on this surface: a Hewn mesh's slot (its faces with the built-in one get
			// it), a plain mesh's surface override
			begin();
			if (hn) {
				int slot = -1;
				for (int64_t i = 0; i < hn_mats.size(); i++) {
					if (Ref<Material>(hn_mats[i]) == m && m.is_valid() && !hewn_default) {
						slot = (int)i;
					}
				}
				if (slot >= 0) {
					hn_mats[slot] = use;
					slot_uses.push_back({ slot, use });
					hn_mats_changed = true;
				} else {
					if (hn_state_before.is_empty()) {
						hn_state_before = hn->get_state();
					}
					PackedInt32Array faces;
					for (int f = 0; f < hn->get_face_count(); f++) {
						if (hn->get_material_for_slot(hn->get_face_material(f)) == HewnMesh3D::get_default_material()) {
							faces.push_back(f);
						}
					}
					hn->apply_material(use, faces);
					hn_mats = hn->get_materials();
					for (const auto &su : slot_uses) {
						if (su.first < hn_mats.size()) {
							hn_mats[su.first] = su.second; // (the copies made before: kept)
						}
					}
				}
			} else {
				const String sp = vformat("surface_material_override/%d", s);
				ur->add_do_property(mi, sp, use);
				ur->add_undo_property(mi, sp, mi->get_surface_override_material(s));
			}
		}
		if (hn && !hn_state_before.is_empty()) {
			const Dictionary after = hn->get_state();
			hn->set_state(hn_state_before);
			ur->add_do_method(hn, "set_state", after);
			ur->add_undo_method(hn, "set_state", hn_state_before);
		}
		if (hn && hn_mats_changed) {
			ur->add_do_property(hn, "materials", hn_mats);
			ur->add_undo_property(hn, "materials", hn->get_materials());
		}
		if (any) {
			ur->commit_action();
		}
		if (!made.is_empty()) {
			toast(T("Texture Paint: new pictures ") + String(", ").join(made));
		}
	}
	// the surfaces as the mesh is now
	tp_surf.clear();
	Ref<Mesh> mesh = mi->get_mesh();
	const Transform3D g = mi->get_global_transform();
	const Basis nb = g.basis.inverse().transposed();
	bool any_target = false;
	for (int s = 0; s < mesh->get_surface_count(); s++) {
		HwPaintSurface ps;
		Ref<BaseMaterial3D> bm = mi->get_active_material(s);
		if (bm.is_null() || (into == 1 && !bm->get_feature(BaseMaterial3D::FEATURE_DETAIL))) {
			tp_surf.push_back(ps);
			continue;
		}
		Ref<Texture2D> tex = bm->get(prop);
		if (tex.is_null()) {
			tp_surf.push_back(ps);
			continue;
		}
		// its picture: one entry per material and property (loaded again when the material holds another one now)
		int ti = -1;
		for (int i = 0; i < (int)tp_tex.size(); i++) {
			if (tp_tex[i].material == bm && tp_tex[i].prop == prop) {
				ti = i;
			}
		}
		if (ti < 0) {
			tp_tex.push_back(HwPaintTexture());
			ti = (int)tp_tex.size() - 1;
			tp_tex[ti].material = bm;
			tp_tex[ti].prop = prop;
		}
		HwPaintTexture &t = tp_tex[ti];
		if (t.texture != tex || t.px.empty()) {
			if (t.unsaved) {
				_tp_save(ti);
			}
			t.texture = tex;
			t.path = tex->get_path();
			t.dirty = t.unsaved = false;
			if (!image_pixels(tex->get_image(), t.px, t.w, t.h)) {
				t.px.clear();
				tp_surf.push_back(ps);
				continue;
			}
			t.orig = t.px;
		}
		ps.target = ti;
		const Array arr = mesh->surface_get_arrays(s);
		const PackedVector3Array v = arr[Mesh::ARRAY_VERTEX];
		const PackedVector3Array n = arr.size() > Mesh::ARRAY_NORMAL ? PackedVector3Array(arr[Mesh::ARRAY_NORMAL]) : PackedVector3Array();
		const PackedVector2Array uv = arr[uvch == 1 ? Mesh::ARRAY_TEX_UV2 : Mesh::ARRAY_TEX_UV];
		const PackedInt32Array idx = arr[Mesh::ARRAY_INDEX];
		if (uv.size() != v.size()) {
			ps.target = -1;
			tp_surf.push_back(ps);
			continue;
		}
		const Vector3 sc = uvch == 1 ? bm->get_uv2_scale() : bm->get_uv1_scale();
		const Vector3 of = uvch == 1 ? bm->get_uv2_offset() : bm->get_uv1_offset();
		ps.pos.resize(v.size());
		ps.nrm.resize(v.size());
		ps.uv.resize(v.size());
		for (int64_t i = 0; i < v.size(); i++) {
			ps.pos[i] = g.xform(v[i]);
			ps.nrm[i] = n.size() == v.size() ? nb.xform(n[i]).normalized() : Vector3();
			ps.uv[i] = Vector2((uv[i].x * sc.x + of.x) * t.w, (uv[i].y * sc.y + of.y) * t.h);
		}
		if (idx.is_empty()) {
			for (int64_t i = 0; i + 2 < v.size(); i += 3) {
				ps.idx.push_back((int)i);
				ps.idx.push_back((int)i + 1);
				ps.idx.push_back((int)i + 2);
			}
		} else {
			ps.idx.assign(idx.ptr(), idx.ptr() + idx.size());
		}
		any_target = true;
		tp_surf.push_back(ps);
	}
	return any_target;
}

// ------------------------------------------------------------------ the view

bool HewnEditorPlugin::_tp_screen(const Vector4 &p_clip, Vector2 &r_px) const {
	if (p_clip.w <= 1e-6f) {
		return false;
	}
	r_px.x = (p_clip.x / p_clip.w * 0.5f + 0.5f) * tp_vsize.x;
	r_px.y = (0.5f - p_clip.y / p_clip.w * 0.5f) * tp_vsize.y;
	return true;
}

// This view: the clip coordinates of every vertex, the depth of the mesh by pixel (what the brush cannot reach).
void HewnEditorPlugin::_tp_view(Camera3D *p_cam) {
	tp_proj = p_cam->get_camera_projection() * Projection(p_cam->get_camera_transform().affine_inverse());
	tp_vsize = p_cam->get_viewport() ? p_cam->get_viewport()->get_visible_rect().size : Vector2(1, 1);
	tp_zcam = p_cam->get_global_transform();
	tp_zw = MAX(1, (int)tp_vsize.x);
	tp_zh = MAX(1, (int)tp_vsize.y);
	tp_zbuf.assign((size_t)tp_zw * tp_zh, 1e30f);
	for (HwPaintSurface &ps : tp_surf) {
		ps.clip.resize(ps.pos.size());
		for (size_t i = 0; i < ps.pos.size(); i++) {
			ps.clip[i] = tp_proj.xform(Vector4(ps.pos[i].x, ps.pos[i].y, ps.pos[i].z, 1.0f));
		}
	}
	// the depth: every triangle in front of the camera, by its pixels (1/w goes straight across the screen)
	for (const HwPaintSurface &ps : tp_surf) {
		for (size_t t = 0; t + 2 < ps.idx.size(); t += 3) {
			Vector2 p[3];
			float iw[3];
			bool ok = true;
			for (int k = 0; k < 3; k++) {
				const Vector4 &c = ps.clip[ps.idx[t + k]];
				ok = ok && _tp_screen(c, p[k]);
				iw[k] = c.w > 1e-6f ? 1.0f / c.w : 0.0f;
			}
			if (!ok) {
				continue;
			}
			const float area = (p[1] - p[0]).cross(p[2] - p[0]);
			if (std::fabs(area) < 1e-9f) {
				continue;
			}
			const int x0 = MAX(0, (int)std::floor(MIN(p[0].x, MIN(p[1].x, p[2].x)))), x1 = MIN(tp_zw - 1, (int)std::ceil(MAX(p[0].x, MAX(p[1].x, p[2].x))));
			const int y0 = MAX(0, (int)std::floor(MIN(p[0].y, MIN(p[1].y, p[2].y)))), y1 = MIN(tp_zh - 1, (int)std::ceil(MAX(p[0].y, MAX(p[1].y, p[2].y))));
			if (x0 > x1 || y0 > y1 || (int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) > (int64_t)tp_zw * tp_zh) {
				continue;
			}
			for (int y = y0; y <= y1; y++) {
				for (int x = x0; x <= x1; x++) {
					const Vector2 q(x + 0.5f, y + 0.5f);
					const float l0 = (p[1] - q).cross(p[2] - q) / area, l1 = (p[2] - q).cross(p[0] - q) / area;
					const float l2 = 1.0f - l0 - l1;
					if (l0 < 0.0f || l1 < 0.0f || l2 < 0.0f) {
						continue;
					}
					const float s = l0 * iw[0] + l1 * iw[1] + l2 * iw[2];
					if (s <= 0.0f) {
						continue;
					}
					float &z = tp_zbuf[(size_t)y * tp_zw + x];
					z = MIN(z, 1.0f / s);
				}
			}
		}
	}
}

// The mesh's triangle under a point of the screen (the nearest), with the point's place in it.
bool HewnEditorPlugin::_tp_hit(Camera3D *p_cam, const Vector2 &p_pos, int &r_surf, int &r_tri, Vector3 &r_bary) const {
	float best = 1e30f;
	r_surf = r_tri = -1;
	for (int si = 0; si < (int)tp_surf.size(); si++) {
		const HwPaintSurface &ps = tp_surf[si];
		if (ps.target < 0 || ps.clip.size() != ps.pos.size()) {
			continue;
		}
		for (size_t t = 0; t + 2 < ps.idx.size(); t += 3) {
			Vector2 p[3];
			float iw[3];
			bool ok = true;
			for (int k = 0; k < 3; k++) {
				const Vector4 &c = ps.clip[ps.idx[t + k]];
				ok = ok && _tp_screen(c, p[k]);
				iw[k] = c.w > 1e-6f ? 1.0f / c.w : 0.0f;
			}
			if (!ok) {
				continue;
			}
			const float area = (p[1] - p[0]).cross(p[2] - p[0]);
			if (std::fabs(area) < 1e-9f) {
				continue;
			}
			const float l0 = (p[1] - p_pos).cross(p[2] - p_pos) / area, l1 = (p[2] - p_pos).cross(p[0] - p_pos) / area;
			const float l2 = 1.0f - l0 - l1;
			if (l0 < 0.0f || l1 < 0.0f || l2 < 0.0f) {
				continue;
			}
			const float s = l0 * iw[0] + l1 * iw[1] + l2 * iw[2];
			if (s <= 0.0f || 1.0f / s >= best) {
				continue;
			}
			best = 1.0f / s;
			r_surf = si;
			r_tri = (int)(t / 3);
			r_bary = Vector3(l0 * iw[0] / s, l1 * iw[1] / s, l2 * iw[2] / s); // (in the triangle, not on the screen)
		}
	}
	return r_surf >= 0;
}

// ------------------------------------------------------------------ painting

HewnEditorPlugin::TPTile &HewnEditorPlugin::_tp_tile(int p_tex, int p_tile) {
	auto it = tp_stroke.find({ p_tex, p_tile });
	if (it != tp_stroke.end()) {
		return it->second;
	}
	TPTile &tl = tp_stroke[{ p_tex, p_tile }];
	tl.tex = p_tex;
	tl.tile = p_tile;
	const HwPaintTexture &t = tp_tex[p_tex];
	const int tx = (t.w + TILE - 1) / TILE;
	const int x0 = (p_tile % tx) * TILE, y0 = (p_tile / tx) * TILE;
	const int w = MIN(TILE, t.w - x0), h = MIN(TILE, t.h - y0);
	tl.before.resize((size_t)w * h * 4);
	for (int y = 0; y < h; y++) {
		memcpy(&tl.before[(size_t)y * w * 4], &t.px[((size_t)(y0 + y) * t.w + x0) * 4], (size_t)w * 4);
	}
	tl.w.assign((size_t)w * h, 0.0f);
	return tl;
}

// One dab of the brush at a point of the screen.
void HewnEditorPlugin::_tp_dab(Camera3D *p_cam, const Vector2 &p_pos, float p_pressure, const Vector2 &p_dir) {
	if (!dock) {
		return;
	}
	HewnDock *d = dock;
	// the brush now: its size, strength, turn and flattening from the settings, the pen and chance
	float radius = tp_radius;
	float strength = tp_strength;
	float pr = CLAMP(p_pressure, 0.0f, 1.0f);
	if (d->tp_pr_curve) {
		Ref<Curve> pc = d->tp_pr_curve->get_edited_resource();
		if (pc.is_valid() && pc->get_point_count() > 0) {
			pr = CLAMP(pc->sample(pr), 0.0f, 1.0f);
		}
	}
	if (d->tp_pr_size && d->tp_pr_size->is_pressed()) {
		radius *= MAX(0.05f, pr);
	}
	if (d->tp_pr_strength && d->tp_pr_strength->is_pressed()) {
		strength *= pr;
	}
	const float full = d->tp_speed_full ? MAX(1.0f, (float)d->tp_speed_full->get_value()) : 2000.0f;
	const float sp = 1.0f - 0.75f * CLAMP(tp_speed / full, 0.0f, 1.0f); // (faster: less)
	if (d->tp_speed_size && d->tp_speed_size->is_pressed()) {
		radius *= sp;
	}
	if (d->tp_speed_strength && d->tp_speed_strength->is_pressed()) {
		strength *= sp;
	}
	float angle = d->tp_angle ? (float)d->tp_angle->get_value() * (float)Math::PI / 180.0f : 0.0f;
	if (d->tp_follow && d->tp_follow->is_pressed() && p_dir.length() > 1e-3f) {
		angle += p_dir.angle();
	}
	float flat = 1.0f;
	const float tilt = MIN(1.0f, tp_tilt.length());
	if (tilt > 0.02f) {
		if (d->tp_tilt_turn && d->tp_tilt_turn->is_pressed()) {
			angle += tp_tilt.angle();
		}
		if (d->tp_tilt_flat && d->tp_tilt_flat->is_pressed()) {
			flat = 1.0f - 0.7f * tilt; // (laid down: the stamp squashed across)
		}
	}
	Vector2 center = p_pos;
	if (d->tp_rand_angle) {
		angle += (rnd(tp_seed) * 2.0f - 1.0f) * (float)d->tp_rand_angle->get_value() * (float)Math::PI / 180.0f;
	}
	if (d->tp_scatter && d->tp_scatter->get_value() > 0.0) {
		const float a = rnd(tp_seed) * (float)Math::TAU, r = rnd(tp_seed) * (float)d->tp_scatter->get_value() * 0.01f * radius;
		center += Vector2(std::cos(a), std::sin(a)) * r;
	}
	if (d->tp_rand_size && d->tp_rand_size->get_value() > 0.0) {
		radius *= 1.0f - rnd(tp_seed) * (float)d->tp_rand_size->get_value() * 0.01f;
	}
	if (d->tp_rand_strength && d->tp_rand_strength->get_value() > 0.0) {
		strength *= 1.0f - rnd(tp_seed) * (float)d->tp_rand_strength->get_value() * 0.01f;
	}
	if (radius < 0.5f || strength <= 0.0f) {
		return;
	}
	_tp_load_stamp();
	const bool stamped = tp_stamp_w > 0;
	const bool stamp_colors = stamped && d->tp_stamp_colors && d->tp_stamp_colors->is_pressed();
	const bool stencil = tp_stencil_w > 0 && d->tp_stencil_btn && d->tp_stencil_btn->is_pressed();
	const int ftype = d->tp_falloff ? d->tp_falloff->get_selected() : 0;
	const Ref<Curve> fcurve = d->tp_falloff_curve ? Ref<Curve>(d->tp_falloff_curve->get_edited_resource()) : Ref<Curve>();
	const bool occlude = !d->tp_occlude || d->tp_occlude->is_pressed();
	const bool backfaces = d->tp_backfaces && d->tp_backfaces->is_pressed();
	const int blend = d->tp_blend ? d->tp_blend->get_selected() : 0; // Mix, Add, Subtract, Multiply, Lighten, Darken
	const Color brush = d->tp_color ? d->tp_color->get_pick_color() : Color(0, 0, 0);
	const float ca = std::cos(-angle), sa = std::sin(-angle);
	const float reach = stamped ? radius * 1.4143f : radius; // (a turned square stamp: its corners)
	const Vector3 eye = tp_zcam.origin;
	// the stencil: where a point of the screen is in its picture
	const float st_sc = MAX(1e-4f, tp_stencil_scale);
	const float st_c = std::cos(-tp_stencil_angle), st_s = std::sin(-tp_stencil_angle);
	for (const HwPaintSurface &ps : tp_surf) {
		if (ps.target < 0 || ps.clip.size() != ps.pos.size()) {
			continue;
		}
		HwPaintTexture &tx = tp_tex[ps.target];
		if (tx.px.empty()) {
			continue;
		}
		const int W = tx.w, H = tx.h;
		const int tiles_x = (W + TILE - 1) / TILE;
		for (size_t t = 0; t + 2 < ps.idx.size(); t += 3) {
			const int i0 = ps.idx[t], i1 = ps.idx[t + 1], i2 = ps.idx[t + 2];
			Vector2 p[3];
			if (!_tp_screen(ps.clip[i0], p[0]) || !_tp_screen(ps.clip[i1], p[1]) || !_tp_screen(ps.clip[i2], p[2])) {
				continue;
			}
			const Rect2 sb = Rect2(p[0], Vector2()).expand(p[1]).expand(p[2]);
			if (sb.position.x > center.x + reach || sb.position.y > center.y + reach || sb.get_end().x < center.x - reach || sb.get_end().y < center.y - reach) {
				continue;
			}
			if (!backfaces) {
				const Vector3 c3 = (ps.pos[i0] + ps.pos[i1] + ps.pos[i2]) / 3.0f;
				Vector3 nn = ps.nrm[i0] + ps.nrm[i1] + ps.nrm[i2];
				if (nn.length_squared() < 1e-12f) {
					nn = -(ps.pos[i1] - ps.pos[i0]).cross(ps.pos[i2] - ps.pos[i0]); // (Godot's front faces: clockwise)
				}
				if (nn.dot(eye - c3) <= 0.0f) {
					continue;
				}
			}
			const Vector2 t0 = ps.uv[i0], t1 = ps.uv[i1], t2 = ps.uv[i2];
			const float det = (t1 - t0).cross(t2 - t0);
			if (std::fabs(det) < 1e-9f) {
				continue;
			}
			// the texels to look at: the part of the triangle in the brush's square, brought back into the picture
			const float sarea = (p[1] - p[0]).cross(p[2] - p[0]);
			if (std::fabs(sarea) < 1e-9f) {
				continue;
			}
			const float iw[3] = { 1.0f / ps.clip[i0].w, 1.0f / ps.clip[i1].w, 1.0f / ps.clip[i2].w };
			std::vector<Vector2> poly = { p[0], p[1], p[2] };
			const Rect2 box(center - Vector2(reach, reach), Vector2(reach, reach) * 2.0f);
			for (int side = 0; side < 4 && !poly.empty(); side++) {
				std::vector<Vector2> out;
				auto inside = [&](const Vector2 &q) {
					switch (side) {
						case 0:
							return q.x >= box.position.x;
						case 1:
							return q.x <= box.get_end().x;
						case 2:
							return q.y >= box.position.y;
						default:
							return q.y <= box.get_end().y;
					}
				};
				auto cut = [&](const Vector2 &a, const Vector2 &b) {
					const float ta = side == 0 ? box.position.x - a.x : side == 1 ? box.get_end().x - a.x : side == 2 ? box.position.y - a.y : box.get_end().y - a.y;
					const float db = side < 2 ? b.x - a.x : b.y - a.y;
					return std::fabs(db) < 1e-12f ? a : a + (b - a) * (ta / db);
				};
				for (size_t k = 0; k < poly.size(); k++) {
					const Vector2 &a = poly[k], &b = poly[(k + 1) % poly.size()];
					const bool ia = inside(a), ib = inside(b);
					if (ia) {
						out.push_back(a);
					}
					if (ia != ib) {
						out.push_back(cut(a, b));
					}
				}
				poly = out;
			}
			if (poly.empty()) {
				continue;
			}
			Vector2 umin(1e30f, 1e30f), umax(-1e30f, -1e30f);
			for (const Vector2 &q : poly) {
				const float l0 = (p[1] - q).cross(p[2] - q) / sarea, l1 = (p[2] - q).cross(p[0] - q) / sarea;
				const float l2 = 1.0f - l0 - l1;
				const float s = l0 * iw[0] + l1 * iw[1] + l2 * iw[2];
				if (std::fabs(s) < 1e-12f) {
					continue;
				}
				const Vector2 u = t0 * (l0 * iw[0] / s) + t1 * (l1 * iw[1] / s) + t2 * (l2 * iw[2] / s);
				umin = umin.min(u);
				umax = umax.max(u);
			}
			if (umin.x > umax.x) {
				continue;
			}
			const int x0 = (int)std::floor(umin.x) - 2, x1 = (int)std::ceil(umax.x) + 2;
			const int y0 = (int)std::floor(umin.y) - 2, y1 = (int)std::ceil(umax.y) + 2;
			if ((int64_t)(x1 - x0) * (y1 - y0) > 4000000) {
				continue; // (seen edge on: too many texels for a dab)
			}
			// a texel's margin beyond the triangle's edges (no seams where islands meet)
			const float e0 = 1.0f / MAX(1e-6f, std::fabs(det) / MAX(1e-6f, (t2 - t1).length()));
			const float e1 = 1.0f / MAX(1e-6f, std::fabs(det) / MAX(1e-6f, (t0 - t2).length()));
			const float e2 = 1.0f / MAX(1e-6f, std::fabs(det) / MAX(1e-6f, (t1 - t0).length()));
			for (int y = y0; y <= y1; y++) {
				for (int x = x0; x <= x1; x++) {
					const Vector2 q(x + 0.5f, y + 0.5f);
					const float b1 = (q - t0).cross(t2 - t0) / det, b2 = (t1 - t0).cross(q - t0) / det;
					const float b0 = 1.0f - b1 - b2;
					if (b0 < -e0 || b1 < -e1 || b2 < -e2) {
						continue;
					}
					const Vector4 c = ps.clip[i0] * b0 + ps.clip[i1] * b1 + ps.clip[i2] * b2;
					Vector2 sp2;
					if (!_tp_screen(c, sp2)) {
						continue;
					}
					const Vector2 l = sp2 - center;
					if (std::fabs(l.x) > reach || std::fabs(l.y) > reach) {
						continue;
					}
					// the brush there
					const Vector2 lr(l.x * ca - l.y * sa, l.x * sa + l.y * ca); // (in the stamp's own axes)
					const Vector2 ls(lr.x, lr.y / flat);
					float mask;
					Color col = brush;
					if (stamped) {
						const Vector2 su = ls / (radius * 2.0f) + Vector2(0.5f, 0.5f);
						const Color sc = sample(tp_stamp_px, tp_stamp_w, tp_stamp_h, su.x, su.y);
						if (stamp_colors) {
							mask = sc.a;
							col = Color(sc.r, sc.g, sc.b, 1.0f);
						} else {
							mask = tp_stamp_alpha ? sc.a : 1.0f - (sc.r * 0.3f + sc.g * 0.59f + sc.b * 0.11f) * sc.a - (1.0f - sc.a);
						}
					} else {
						const float dd = ls.length() / radius;
						mask = falloff_of(ftype, fcurve, dd);
					}
					if (mask <= 0.0f) {
						continue;
					}
					if (stencil) {
						Vector2 o = sp2 - tp_stencil_pos;
						o = Vector2(o.x * st_c - o.y * st_s, o.x * st_s + o.y * st_c) / st_sc;
						const Color sc = sample(tp_stencil_px, tp_stencil_w, tp_stencil_h, o.x / tp_stencil_w + 0.5f, o.y / tp_stencil_h + 0.5f);
						if (sc.a <= 0.0f) {
							continue;
						}
						mask *= sc.a;
						col = Color(sc.r, sc.g, sc.b, 1.0f);
					}
					if (occlude) {
						const int px = (int)sp2.x, py = (int)sp2.y;
						if (px >= 0 && py >= 0 && px < tp_zw && py < tp_zh) {
							const float z = tp_zbuf[(size_t)py * tp_zw + px];
							if (c.w > z * 1.01f + 1e-3f) {
								continue; // (behind the mesh itself)
							}
						}
					}
					const float wgt = CLAMP(mask * strength, 0.0f, 1.0f);
					// the texel (the picture repeats: wrapped)
					const int X = ((x % W) + W) % W, Y = ((y % H) + H) % H;
					const int tile = (Y / TILE) * tiles_x + X / TILE;
					TPTile &tl = _tp_tile(ps.target, tile);
					const int tw = MIN(TILE, W - (X / TILE) * TILE);
					const size_t li = (size_t)(Y % TILE) * tw + (X % TILE);
					uint8_t *dst = &tx.px[((size_t)Y * W + X) * 4];
					if (tp_tool == 2) {
						// blur: toward the mean of the texels around (it goes on adding up)
						float sum[4] = { 0, 0, 0, 0 };
						for (int oy = -1; oy <= 1; oy++) {
							for (int ox = -1; ox <= 1; ox++) {
								const int xx = ((X + ox) % W + W) % W, yy = ((Y + oy) % H + H) % H;
								const uint8_t *s = &tx.px[((size_t)yy * W + xx) * 4];
								for (int k = 0; k < 4; k++) {
									sum[k] += s[k];
								}
							}
						}
						const float a = wgt * 0.5f;
						for (int k = 0; k < 4; k++) {
							dst[k] = (uint8_t)CLAMP((int)std::lround(dst[k] + (sum[k] / 9.0f - dst[k]) * a), 0, 255);
						}
						tx.dirty = tx.unsaved = true;
						continue;
					}
					// draw / erase: toward the stroke's strongest so far over what was there when it began
					float &acc = tl.w[li];
					if (wgt <= acc) {
						continue;
					}
					acc = wgt;
					const uint8_t *base = &tl.before[li * 4];
					float bc[4] = { base[0] / 255.0f, base[1] / 255.0f, base[2] / 255.0f, base[3] / 255.0f };
					float out[4];
					if (tp_tool == 1) {
						const bool detail = tx.prop == "detail_albedo";
						if (detail) {
							out[0] = bc[0];
							out[1] = bc[1];
							out[2] = bc[2];
							out[3] = bc[3] * (1.0f - acc); // (the layer see-through again)
						} else {
							const uint8_t *o = &tx.orig[((size_t)Y * W + X) * 4];
							for (int k = 0; k < 4; k++) {
								out[k] = bc[k] + (o[k] / 255.0f - bc[k]) * acc; // (back to the picture as it was loaded)
							}
						}
					} else {
						const float cc[3] = { col.r, col.g, col.b };
						for (int k = 0; k < 3; k++) {
							float r;
							switch (blend) {
								case 1:
									r = bc[k] + cc[k];
									break;
								case 2:
									r = bc[k] - cc[k];
									break;
								case 3:
									r = bc[k] * cc[k];
									break;
								case 4:
									r = MAX(bc[k], cc[k]);
									break;
								case 5:
									r = MIN(bc[k], cc[k]);
									break;
								default:
									r = cc[k];
									break;
							}
							out[k] = bc[k] + (CLAMP(r, 0.0f, 1.0f) - bc[k]) * acc;
						}
						const float ta = blend == 0 ? col.a : MAX(bc[3], col.a); // (a Detail layer: the paint makes it seen)
						out[3] = bc[3] + (ta - bc[3]) * acc;
					}
					for (int k = 0; k < 4; k++) {
						dst[k] = (uint8_t)CLAMP((int)std::lround(out[k] * 255.0f), 0, 255);
					}
					tx.dirty = tx.unsaved = true;
				}
			}
		}
	}
}

// The stroke from where it was to p_pos: dabs at the brush's spacing.
void HewnEditorPlugin::_tp_stroke_to(Camera3D *p_cam, const Vector2 &p_pos) {
	const float spacing = dock && dock->tp_spacing ? MAX(1.0f, (float)dock->tp_spacing->get_value()) : 15.0f;
	const float step = MAX(1.0f, tp_radius * spacing * 0.01f);
	const Vector2 d = p_pos - tp_last;
	float len = d.length();
	if (len < 1e-4f) {
		return;
	}
	const Vector2 dir = d / len;
	tp_dir = dir;
	Vector2 at = tp_last;
	float left = tp_left;
	while (len >= left) {
		at += dir * left;
		len -= left;
		_tp_dab(p_cam, at, tp_pressure, dir);
		left = step;
	}
	tp_left = left - len;
	tp_last = p_pos;
}

// The stroke's undo step: the tiles it changed, before and after.
void HewnEditorPlugin::_tp_stroke_end() {
	tp_painting = false;
	if (tp_stroke.empty()) {
		return;
	}
	std::map<int, TPUndo> by_tex;
	for (auto &it : tp_stroke) {
		const TPTile &tl = it.second;
		const HwPaintTexture &t = tp_tex[tl.tex];
		const int tx = (t.w + TILE - 1) / TILE;
		const int x0 = (tl.tile % tx) * TILE, y0 = (tl.tile / tx) * TILE;
		const int w = MIN(TILE, t.w - x0), h = MIN(TILE, t.h - y0);
		std::vector<uint8_t> after((size_t)w * h * 4);
		for (int y = 0; y < h; y++) {
			memcpy(&after[(size_t)y * w * 4], &t.px[((size_t)(y0 + y) * t.w + x0) * 4], (size_t)w * 4);
		}
		if (after == tl.before) {
			continue;
		}
		TPUndo &u = by_tex[tl.tex];
		u.tex = tl.tex;
		u.tiles.push_back(tl.tile);
		u.before.push_back(tl.before);
		u.after.push_back(after);
	}
	tp_stroke.clear();
	if (by_tex.empty()) {
		return;
	}
	static const char *names[5] = { "Texture Paint: Draw", "Texture Paint: Erase", "Texture Paint: Blur", "Texture Paint: Fill", "Texture Paint: Draw" };
	EditorUndoRedoManager *ur = get_undo_redo();
	MeshInstance3D *mi = tp_mesh();
	ur->create_action(names[CLAMP(tp_tool, 0, 4)], UndoRedo::MERGE_DISABLE, mi ? (Object *)mi : (Object *)this);
	for (auto &bt : by_tex) {
		const int id = tp_undo_next++;
		tp_undo[id] = bt.second;
		ur->add_do_method(this, "_tp_undo_apply", id, true);
		ur->add_undo_method(this, "_tp_undo_apply", id, false);
	}
	ur->commit_action(false);
	// the history kept to 256 MB: the oldest strokes' tiles let go (their undo steps then do nothing)
	const size_t LIMIT = (size_t)256 << 20;
	size_t total = 0;
	for (const auto &u : tp_undo) {
		for (size_t i = 0; i < u.second.before.size(); i++) {
			total += u.second.before[i].size() + u.second.after[i].size();
		}
	}
	while (total > LIMIT && tp_undo.size() > 1) {
		auto oldest = tp_undo.begin();
		for (size_t i = 0; i < oldest->second.before.size(); i++) {
			total -= oldest->second.before[i].size() + oldest->second.after[i].size();
		}
		tp_undo.erase(oldest);
	}
}

void HewnEditorPlugin::_tp_undo_apply(int p_id, bool p_after) {
	auto it = tp_undo.find(p_id);
	if (it == tp_undo.end() || it->second.tex < 0 || it->second.tex >= (int)tp_tex.size()) {
		return;
	}
	const TPUndo &u = it->second;
	HwPaintTexture &t = tp_tex[u.tex];
	if (t.px.empty()) {
		// (out of the mode: the picture loaded again from its texture)
		if (t.texture.is_null() || !image_pixels(t.texture->get_image(), t.px, t.w, t.h)) {
			return;
		}
		t.orig = t.px;
	}
	const int tx = (t.w + TILE - 1) / TILE;
	for (size_t i = 0; i < u.tiles.size(); i++) {
		const int x0 = (u.tiles[i] % tx) * TILE, y0 = (u.tiles[i] / tx) * TILE;
		const int w = MIN(TILE, t.w - x0), h = MIN(TILE, t.h - y0);
		const std::vector<uint8_t> &src = p_after ? u.after[i] : u.before[i];
		if (src.size() != (size_t)w * h * 4) {
			continue;
		}
		for (int y = 0; y < h; y++) {
			memcpy(&t.px[((size_t)(y0 + y) * t.w + x0) * 4], &src[(size_t)y * w * 4], (size_t)w * 4);
		}
	}
	t.dirty = t.unsaved = true;
	if (!tpaint) {
		_tp_show(u.tex); // (and written when the scene is saved)
	}
	update_overlays();
}

// Fill: the whole piece of the surface under the click (its triangles joined by their vertices: an island of the
// unwrap) in the brush's color.
void HewnEditorPlugin::_tp_fill(Camera3D *p_cam, const Vector2 &p_pos) {
	int si, tri;
	Vector3 bary;
	if (!_tp_hit(p_cam, p_pos, si, tri, bary)) {
		return;
	}
	const HwPaintSurface &ps = tp_surf[si];
	HwPaintTexture &tx = tp_tex[ps.target];
	const int nt = (int)ps.idx.size() / 3;
	std::vector<std::vector<int>> by_vert(ps.pos.size());
	for (int t = 0; t < nt; t++) {
		for (int k = 0; k < 3; k++) {
			by_vert[ps.idx[t * 3 + k]].push_back(t);
		}
	}
	std::vector<char> in(nt, 0);
	std::vector<int> stack = { tri };
	in[tri] = 1;
	while (!stack.empty()) {
		const int t = stack.back();
		stack.pop_back();
		for (int k = 0; k < 3; k++) {
			for (int o : by_vert[ps.idx[t * 3 + k]]) {
				if (!in[o]) {
					in[o] = 1;
					stack.push_back(o);
				}
			}
		}
	}
	const Color col = dock && dock->tp_color ? dock->tp_color->get_pick_color() : Color(0, 0, 0);
	const float a = CLAMP(tp_strength, 0.0f, 1.0f);
	const int W = tx.w, H = tx.h, tiles_x = (W + TILE - 1) / TILE;
	tp_stroke.clear();
	for (int t = 0; t < nt; t++) {
		if (!in[t]) {
			continue;
		}
		const Vector2 t0 = ps.uv[ps.idx[t * 3]], t1 = ps.uv[ps.idx[t * 3 + 1]], t2 = ps.uv[ps.idx[t * 3 + 2]];
		const float det = (t1 - t0).cross(t2 - t0);
		if (std::fabs(det) < 1e-9f) {
			continue;
		}
		const Vector2 mn = t0.min(t1).min(t2), mx = t0.max(t1).max(t2);
		const int x0 = (int)std::floor(mn.x) - 1, y0 = (int)std::floor(mn.y) - 1;
		const int x1 = MIN((int)std::ceil(mx.x) + 1, x0 + W + 2), y1 = MIN((int)std::ceil(mx.y) + 1, y0 + H + 2);
		const float e0 = 1.0f / MAX(1e-6f, std::fabs(det) / MAX(1e-6f, (t2 - t1).length()));
		const float e1 = 1.0f / MAX(1e-6f, std::fabs(det) / MAX(1e-6f, (t0 - t2).length()));
		const float e2 = 1.0f / MAX(1e-6f, std::fabs(det) / MAX(1e-6f, (t1 - t0).length()));
		for (int y = y0; y <= y1; y++) {
			for (int x = x0; x <= x1; x++) {
				const Vector2 q(x + 0.5f, y + 0.5f);
				const float b1 = (q - t0).cross(t2 - t0) / det, b2 = (t1 - t0).cross(q - t0) / det;
				if (1.0f - b1 - b2 < -e0 || b1 < -e1 || b2 < -e2) {
					continue;
				}
				const int X = ((x % W) + W) % W, Y = ((y % H) + H) % H;
				TPTile &tl = _tp_tile(ps.target, (Y / TILE) * tiles_x + X / TILE);
				const int tw = MIN(TILE, W - (X / TILE) * TILE);
				float &acc = tl.w[(size_t)(Y % TILE) * tw + (X % TILE)];
				if (acc >= a) {
					continue;
				}
				acc = a;
				const uint8_t *base = &tl.before[((size_t)(Y % TILE) * tw + (X % TILE)) * 4];
				uint8_t *dst = &tx.px[((size_t)Y * W + X) * 4];
				const float cc[4] = { col.r, col.g, col.b, col.a };
				for (int k = 0; k < 4; k++) {
					dst[k] = (uint8_t)CLAMP((int)std::lround((base[k] / 255.0f + (cc[k] - base[k] / 255.0f) * a) * 255.0f), 0, 255);
				}
			}
		}
	}
	tx.dirty = tx.unsaved = true;
	const int was = tp_tool;
	tp_tool = 3;
	_tp_stroke_end();
	tp_tool = was;
}

// The eyedropper: the picture's color under the mouse (p_second: into the second color).
bool HewnEditorPlugin::_tp_pick(Camera3D *p_cam, const Vector2 &p_pos, bool p_second) {
	int si, tri;
	Vector3 b;
	if (!_tp_hit(p_cam, p_pos, si, tri, b) || !dock) {
		return false;
	}
	const HwPaintSurface &ps = tp_surf[si];
	const HwPaintTexture &tx = tp_tex[ps.target];
	const Vector2 uv = ps.uv[ps.idx[tri * 3]] * b.x + ps.uv[ps.idx[tri * 3 + 1]] * b.y + ps.uv[ps.idx[tri * 3 + 2]] * b.z;
	const int X = (((int)std::floor(uv.x) % tx.w) + tx.w) % tx.w, Y = (((int)std::floor(uv.y) % tx.h) + tx.h) % tx.h;
	const uint8_t *p = &tx.px[((size_t)Y * tx.w + X) * 4];
	const Color c(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, 1.0f);
	ColorPickerButton *to = p_second ? dock->tp_color2 : dock->tp_color;
	if (to) {
		to->set_pick_color(c);
	}
	return true;
}

// ------------------------------------------------------------------ showing, writing

void HewnEditorPlugin::_tp_show(int p_tex) {
	HwPaintTexture &t = tp_tex[p_tex];
	t.dirty = false;
	if (t.texture.is_null() || t.px.empty()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	const RID nt = rs->texture_2d_create(make_image(t.px, t.w, t.h));
	rs->texture_replace(t.texture->get_rid(), nt); // (the material keeps its texture; what it shows is the painting)
}

void HewnEditorPlugin::_tp_flush() {
	for (int i = 0; i < (int)tp_tex.size(); i++) {
		if (tp_tex[i].dirty) {
			_tp_show(i);
		}
	}
}

bool HewnEditorPlugin::_tp_save(int p_tex) {
	HwPaintTexture &t = tp_tex[p_tex];
	if (!t.unsaved || t.px.empty()) {
		return true;
	}
	if (!writable_picture(t.path)) {
		return false;
	}
	if (!save_picture(make_image(t.px, t.w, t.h), t.path)) {
		toast(vformat(T("Texture Paint: could not write %s"), t.path), true);
		return false;
	}
	t.unsaved = false;
	PackedStringArray files;
	files.push_back(t.path);
	EditorInterface::get_singleton()->get_resource_filesystem()->reimport_files(files);
	t.dirty = tpaint; // (the import shows the file: the painting over it again)
	return true;
}

void HewnEditorPlugin::tp_save_all() {
	for (int i = 0; i < (int)tp_tex.size(); i++) {
		_tp_save(i);
	}
}

void HewnEditorPlugin::_save_external_data() {
	tp_save_all();
}

// A save: Rigging / Weight Paint show the skeleton at rest (show_rest_only) - the scene gets it as it was, the rest shown
// again after the save.
void HewnEditorPlugin::_apply_changes() {
	bool any = false;
	if (rig) {
		if (Skeleton3D *s = rig_skeleton()) {
			s->set_show_rest_only(rig_rest_was);
			any = true;
		}
	}
	if (wpaint) {
		if (Skeleton3D *s = wp_skeleton()) {
			s->set_show_rest_only(wp_rest_was);
			any = true;
		}
	}
	if (any) {
		callable_mp(this, &HewnEditorPlugin::_rest_again).call_deferred();
	}
}

void HewnEditorPlugin::_rest_again() {
	if (rig) {
		if (Skeleton3D *s = rig_skeleton()) {
			s->set_show_rest_only(true);
		}
	}
	if (wpaint) {
		if (Skeleton3D *s = wp_skeleton()) {
			s->set_show_rest_only(true);
		}
	}
}

// ------------------------------------------------------------------ the stamp and the stencil

void HewnEditorPlugin::_tp_load_stamp() {
	if (!dock) {
		return;
	}
	Ref<Texture2D> st = dock->tp_stamp ? Ref<Texture2D>(dock->tp_stamp->get_edited_resource()) : Ref<Texture2D>();
	const uint64_t sid = st.is_valid() ? st->get_instance_id() : 0;
	if (sid != tp_stamp_id) {
		tp_stamp_id = sid;
		tp_stamp_w = tp_stamp_h = 0;
		tp_stamp_px.clear();
		if (st.is_valid() && image_pixels(st->get_image(), tp_stamp_px, tp_stamp_w, tp_stamp_h)) {
			tp_stamp_alpha = false;
			for (size_t i = 3; i < tp_stamp_px.size(); i += 4) {
				if (tp_stamp_px[i] < 250) {
					tp_stamp_alpha = true; // (a picture with see-through parts: its alpha is the shape)
					break;
				}
			}
		} else {
			tp_stamp_w = tp_stamp_h = 0;
		}
	}
	Ref<Texture2D> sc = dock->tp_stencil ? Ref<Texture2D>(dock->tp_stencil->get_edited_resource()) : Ref<Texture2D>();
	const uint64_t cid = sc.is_valid() ? sc->get_instance_id() : 0;
	if (cid != tp_stencil_id) {
		tp_stencil_id = cid;
		tp_stencil_px.clear();
		if (!(sc.is_valid() && image_pixels(sc->get_image(), tp_stencil_px, tp_stencil_w, tp_stencil_h))) {
			tp_stencil_w = tp_stencil_h = 0;
		}
		tp_stencil_placed = false;
	}
}

String HewnEditorPlugin::tp_status() const {
	const int into = dock && dock->tp_into ? dock->tp_into->get_selected() : 0;
	const int uvch = into == 0 ? 0 : (dock && dock->tp_uv ? dock->tp_uv->get_selected() : 0);
	String s = T("Texture Paint") + ": " + (into == 0 ? T("Albedo") : T("Detail Layer")) + (uvch == 1 ? " (UV2)" : " (UV1)");
	std::vector<int> seen;
	for (const HwPaintSurface &ps : tp_surf) {
		if (ps.target >= 0 && std::find(seen.begin(), seen.end(), ps.target) == seen.end()) {
			seen.push_back(ps.target);
		}
	}
	if (seen.empty()) {
		s += String("  ") + T("(the picture made with the first stroke)");
	} else {
		const HwPaintTexture &t = tp_tex[seen[0]];
		s += vformat("  %s %dx%d", t.path.get_file(), t.w, t.h);
		if (seen.size() > 1) {
			s += vformat(T(" (+%d more)"), (int)seen.size() - 1);
		}
	}
	return s;
}
