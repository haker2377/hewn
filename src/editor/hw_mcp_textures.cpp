// Hewn editor - the MCP texture tools: the project's list, a numbered sheet, UVs, hotspots, an atlas of an image.

#include "hw_mcp_impl.h"

// ------------------------------------------------------------------ textures: the project's list, a sheet of them,
// UVs, seams, hotspots

namespace {

// 3 x 5 digits (a row a byte, its low 3 bits): the numbers on a texture sheet's cells
const uint8_t DIGITS[10][5] = { { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 },
	{ 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 } };

void draw_number(Ref<Image> &img, int p_x, int p_y, int p_n, int p_scale) {
	const String s = itos(p_n);
	const int w = (int)s.length() * 4 * p_scale + p_scale, h = 7 * p_scale;
	img->fill_rect(Rect2i(p_x, p_y, w, h), Color(0, 0, 0, 1));
	for (int i = 0; i < s.length(); i++) {
		const int d = s[i] - '0';
		for (int r = 0; r < 5; r++) {
			for (int c = 0; c < 3; c++) {
				if (DIGITS[d][r] & (4 >> c)) {
					img->fill_rect(Rect2i(p_x + p_scale + (i * 4 + c) * p_scale, p_y + p_scale + r * p_scale, p_scale, p_scale), Color(1, 1, 1, 1));
				}
			}
		}
	}
}

// The project's images and materials under a folder (by the editor's file list: nothing loaded).
void list_textures(EditorFileSystemDirectory *p_dir, const String &p_filter, bool p_images, bool p_materials, PackedStringArray &r_out) {
	if (!p_dir) {
		return;
	}
	for (int i = 0; i < p_dir->get_file_count(); i++) {
		const String type = p_dir->get_file_type(i);
		const String path = p_dir->get_file_path(i);
		const bool img = ClassDB::is_parent_class(type, "Texture2D") || type == "Image";
		const bool mat = ClassDB::is_parent_class(type, "Material");
		if (((img && p_images) || (mat && p_materials)) && (p_filter.is_empty() || path.to_lower().contains(p_filter)) && !path.begins_with("res://addons/") &&
				!path.contains("/.")) {
			r_out.push_back(path);
		}
	}
	for (int i = 0; i < p_dir->get_subdir_count(); i++) {
		list_textures(p_dir->get_subdir(i), p_filter, p_images, p_materials, r_out);
	}
}

// A file's picture (an image; a material: its albedo texture, else its color).
Ref<Image> picture_of(const String &p_path) {
	Ref<Resource> r = ResourceLoader::get_singleton()->load(p_path);
	Ref<Texture2D> tex = r;
	if (Ref<BaseMaterial3D> bm = r; bm.is_valid()) {
		tex = bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
		if (tex.is_null()) {
			Ref<Image> c = Image::create(8, 8, false, Image::FORMAT_RGB8);
			c->fill(bm->get_albedo());
			return c;
		}
	}
	if (tex.is_null()) {
		return Ref<Image>();
	}
	Ref<Image> img = tex->get_image();
	if (img.is_null()) {
		return img;
	}
	img = img->duplicate();
	if (img->is_compressed()) {
		img->decompress();
	}
	img->convert(Image::FORMAT_RGB8);
	return img;
}

} // namespace

Array HewnMCPServer::tool_list_tex() {
	Array t;
	{
		Dictionary p;
		p["folder"] = prop("string", "Under this folder (default res://)");
		p["filter"] = prop("string", "Only paths with this in them (brick, wood...)");
		p["kind"] = prop("string", "images, materials or all (default)");
		t.push_back(tool("textures", "The project's images and materials (paths, numbered): names often tell enough; texture_sheet to see them.", schema_obj(p)));
	}
	{
		Dictionary p;
		p["paths"] = prop("array", "The images / materials to show (else folder + filter as textures does)");
		p["folder"] = prop("string", "Or under this folder");
		p["filter"] = prop("string", "Or the ones with this in their path");
		p["cell"] = prop("integer", "Each one's size in pixels (default 128: enough to tell brick from wood)");
		p["columns"] = prop("integer", "Per row (default 8)");
		p["start"] = prop("integer", "From this one of the list (pages of columns x rows)");
		p["rows"] = prop("integer", "Rows (default 8)");
		t.push_back(tool("texture_sheet", "Many textures on one picture, each small with its number on it, and the list of the numbers' files: about 20 tokens a texture.", schema_obj(p)));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The Hewn mesh");
		p["op"] = prop("string", "world (UVs by the world: tiling textures without seams; scale: meters a texture), unwrap_angle, unwrap_conformal, smart_project, project_box, project_cylinder, project_sphere, relax, pack");
		p["faces"] = prop("array", "Face indices (default: the selected faces, else all)");
		p["scale"] = prop("number", "world: how many meters one texture spans (default 1)");
		t.push_back(tool("uv_edit", "The UVs of a Hewn mesh's faces: world-aligned, or unwrapped / projected by its seams (mesh_edit mark_seam). One undo step.", schema_obj(p, { "path", "op" })));
	}
	{
		Dictionary p;
		p["path"] = prop("string", "The Hewn mesh");
		p["atlas"] = prop("string", "A hotspot atlas res://...tres (hotspot_atlas_create makes one from an image)");
		p["faces"] = prop("array", "Only these faces (default all)");
		t.push_back(tool("hotspots", "Texture a Hewn mesh by a hotspot atlas: each face gets the cell of the atlas's image that fits its shape and size - no unwrap needed. Again: another look.", schema_obj(p, { "path", "atlas" })));
	}
	{
		Dictionary p;
		p["image"] = prop("string", "The atlas's picture res://... (a trim sheet)");
		p["path"] = prop("string", "Where to save it res://...tres");
		p["cut"] = prop("string", "auto (default: cut where the picture's bands change - a trim sheet's strips, repeated along) or grid (rows x columns)");
		p["rows"] = prop("integer", "grid: rows of cells (default 4)");
		p["columns"] = prop("integer", "grid: columns (default 4)");
		p["meters"] = prop("number", "How many meters the whole picture spans (default 2)");
		t.push_back(tool("hotspot_atlas_create", "Make a hotspot atlas of an image: its cells found from its bands (a trim sheet) or an even grid (finer by hand in the Hotspot Atlas panel).", schema_obj(p, { "image", "path" })));
	}
	return t;
}

Dictionary HewnMCPServer::_tool_tex(const String &p_name, const Dictionary &p_args, bool &r_handled) {
	r_handled = true;
	EditorInterface *ei = EditorInterface::get_singleton();
	if (p_name == "textures" || p_name == "texture_sheet") {
		PackedStringArray paths;
		if (p_args.has("paths")) {
			for (const Variant &v : Array(p_args["paths"])) {
				paths.push_back(String(v));
			}
		} else {
			const String kind = p_args.get("kind", p_name == "texture_sheet" ? "all" : "all");
			EditorFileSystemDirectory *dir = ei->get_resource_filesystem()->get_filesystem_path(p_args.get("folder", "res://"));
			list_textures(dir, String(p_args.get("filter", "")).to_lower(), kind != "materials", kind != "images", paths);
		}
		if (p_name == "textures") {
			String out;
			for (int i = 0; i < paths.size(); i++) {
				out += itos(i) + " " + paths[i] + "\n";
			}
			return text_result(paths.is_empty() ? String("None") : out);
		}
		const int cell = CLAMP((int)f(p_args.get("cell", 128)), 32, 512), cols = CLAMP((int)f(p_args.get("columns", 8)), 1, 16),
				  rows = CLAMP((int)f(p_args.get("rows", 8)), 1, 16), start = MAX(0, (int)f(p_args.get("start", 0)));
		const int count = MIN(cols * rows, MAX(0, (int)paths.size() - start));
		if (count == 0) {
			return fail("No textures there");
		}
		const int used_rows = (count + cols - 1) / cols;
		const int gap = 2;
		Ref<Image> sheet = Image::create(cols * (cell + gap) + gap, used_rows * (cell + gap) + gap, false, Image::FORMAT_RGB8);
		sheet->fill(Color(0.15f, 0.15f, 0.15f));
		String legend;
		for (int k = 0; k < count; k++) {
			const String path = paths[start + k];
			Ref<Image> img = picture_of(path);
			const int x = gap + (k % cols) * (cell + gap), y = gap + (k / cols) * (cell + gap);
			if (img.is_valid() && !img->is_empty()) {
				img->resize(cell, cell, Image::INTERPOLATE_BILINEAR);
				sheet->blit_rect(img, Rect2i(0, 0, cell, cell), Vector2i(x, y));
			}
			draw_number(sheet, x, y, start + k, MAX(2, cell / 48));
			legend += itos(start + k) + " " + path + (img.is_null() ? "  (could not load)" : "") + "\n";
		}
		Dictionary c;
		c["type"] = "image";
		c["mimeType"] = "image/jpeg";
		c["data"] = Marshalls::get_singleton()->raw_to_base64(sheet->save_jpg_to_buffer(0.85f));
		Dictionary tx;
		tx["type"] = "text";
		tx["text"] = legend + ((int)paths.size() > start + count ? vformat("(%d more: start %d)", (int64_t)paths.size() - start - count, (int64_t)(start + count)) : String());
		Dictionary r;
		r["content"] = Array::make(c, tx);
		r["isError"] = false;
		return r;
	}
	Node *root = scene_root();
	EditorUndoRedoManager *ur = plugin ? plugin->get_undo_redo() : nullptr;
	if (p_name == "hotspot_atlas_create") {
		const String image = p_args.get("image", ""), path = p_args.get("path", "");
		if (!path.begins_with("res://") || !path.ends_with(".tres")) {
			return fail("path: res://....tres");
		}
		Ref<Texture2D> tex = ResourceLoader::get_singleton()->load(image);
		if (tex.is_null()) {
			return fail("Not an image: " + image);
		}
		const int rows = CLAMP((int)f(p_args.get("rows", 4)), 1, 64), cols = CLAMP((int)f(p_args.get("columns", 4)), 1, 64);
		Ref<HewnHotspotAtlas> at;
		at.instantiate();
		at->set_texture(tex);
		if (plugin && plugin->texture_dock) {
			Ref<Material> mat = plugin->texture_dock->material_for_image(image);
			if (mat.is_valid()) {
				at->set_material(mat);
			}
		}
		TypedArray<Rect2> cells;
		std::vector<int> full_width; // (cells across the whole picture: strips repeated along it)
		const bool autocut = String(p_args.get("cut", p_args.has("rows") || p_args.has("columns") ? "grid" : "auto")) == "auto";
		Ref<Image> pic = autocut ? picture_of(image) : Ref<Image>();
		if (autocut && pic.is_valid() && !pic->is_empty()) {
			// the bands of a trim sheet: where the mean color of the rows jumps; in each band, where its columns' does
			const int W = pic->get_width(), H = pic->get_height();
			const int min_px = MAX(8, MIN(W, H) / 32);
			auto cuts_of = [&](int p_len, const std::function<Color(int)> &p_mean) {
				// the change between the mean of a window before and one after each place (thin lines, repeated or not,
				// sit in both windows alike: no cut); the cuts at the strongest changes, at least min_px apart
				std::vector<Color> m(p_len);
				for (int i = 0; i < p_len; i++) {
					m[i] = p_mean(i);
				}
				const int win = MAX(4, min_px);
				std::vector<float> score(p_len, 0.0f);
				for (int i = win; i <= p_len - win; i++) {
					Color a, b;
					for (int k = 1; k <= win; k++) {
						a += m[i - k];
						b += m[i + k - 1];
					}
					a /= (float)win;
					b /= (float)win;
					score[i] = std::fabs(a.r - b.r) + std::fabs(a.g - b.g) + std::fabs(a.b - b.b);
				}
				std::vector<int> c = { 0 };
				std::vector<int> order;
				for (int i = 0; i < p_len; i++) {
					if (score[i] > 0.1f) {
						order.push_back(i);
					}
				}
				std::sort(order.begin(), order.end(), [&](int x, int y) { return score[x] > score[y]; });
				std::vector<int> picked;
				for (int i : order) {
					bool near = i < min_px || p_len - i < min_px;
					for (int q : picked) {
						near = near || std::abs(q - i) < min_px;
					}
					if (!near) {
						picked.push_back(i);
					}
				}
				std::sort(picked.begin(), picked.end());
				c.insert(c.end(), picked.begin(), picked.end());
				c.push_back(p_len);
				return c;
			};
			const std::vector<int> ys = cuts_of(H, [&](int y) {
				Color m;
				for (int x = 0; x < W; x += MAX(1, W / 128)) {
					m += pic->get_pixel(x, y);
				}
				return m / (float)((W + MAX(1, W / 128) - 1) / MAX(1, W / 128));
			});
			for (size_t r = 0; r + 1 < ys.size(); r++) {
				const int y0 = ys[r], y1 = ys[r + 1];
				const std::vector<int> xs_found = cuts_of(W, [&](int x) {
					Color m;
					int n = 0;
					for (int y = y0; y < y1; y += MAX(1, (y1 - y0) / 32)) {
						m += pic->get_pixel(x, y);
						n++;
					}
					return m / (float)MAX(1, n);
				});
				std::vector<int> xs2 = xs_found;
				for (size_t c = 0; c + 1 < xs_found.size(); c++) {
					if (xs_found[c + 1] - xs_found[c] < W / 8) {
						xs2 = { 0, W }; // (pieces that narrow: a pattern repeated along it - stripes - not cells)
						break;
					}
				}
				const std::vector<int> &xs = xs2;
				for (size_t c = 0; c + 1 < xs.size(); c++) {
					if (xs.size() == 2) {
						full_width.push_back((int)cells.size());
					}
					cells.push_back(Rect2((float)xs[c] / W, (float)y0 / H, (float)(xs[c + 1] - xs[c]) / W, (float)(y1 - y0) / H));
				}
			}
		} else {
			for (int r = 0; r < rows; r++) {
				for (int c = 0; c < cols; c++) {
					cells.push_back(Rect2((float)c / cols, (float)r / rows, 1.0f / cols, 1.0f / rows));
				}
			}
		}
		at->set_cells(cells);
		for (int c : full_width) {
			at->set_cell_tiling(c, 1); // (a strip as wide as the picture: repeated along U)
		}
		const float meters = (float)f(p_args.get("meters", 2.0));
		if (meters > 0.0f && tex->get_width() > 0) {
			at->set_pixels_per_meter(tex->get_width() / meters);
		}
		at->take_over_path(path); // (made again: the one loaded before - and the meshes using it - are this one now)
		if (ResourceSaver::get_singleton()->save(at, path) != OK) {
			return fail("Could not save " + path);
		}
		ei->get_resource_filesystem()->update_file(path);
		Dictionary o;
		o["atlas"] = path;
		o["cells"] = (int64_t)cells.size();
		return text_result(o);
	}
	if (!root) {
		return fail("No scene is open in the editor");
	}
	if (!ur) {
		return fail("The Hewn plugin is not running");
	}
	HewnMesh3D *m = Object::cast_to<HewnMesh3D>(find_node(p_args.get("path", "")));
	if (p_name == "hotspots") {
		if (!m) {
			return fail("Not a Hewn mesh: " + String(p_args.get("path", "")));
		}
		Ref<HewnHotspotAtlas> at = ResourceLoader::get_singleton()->load(p_args.get("atlas", ""));
		if (at.is_null()) {
			return fail("Not a hotspot atlas: " + String(p_args.get("atlas", "")));
		}
		const PackedInt32Array faces = p_args.has("faces") ? PackedInt32Array(from_json(p_args["faces"], Variant::PACKED_INT32_ARRAY)) : PackedInt32Array();
		undoable(ur, m, "MCP: Hotspots", [&]() {
			m->set_hotspot_atlas(at);
			m->randomize_hotspots(faces, true, 0.0f);
		});
		Dictionary o;
		o["faces"] = faces.is_empty() ? Variant(String("all")) : Variant(faces.size());
		o["cells"] = at->get_cells().size();
		return text_result(o);
	}
	if (p_name == "uv_edit") {
		if (!m) {
			return fail("Not a Hewn mesh: " + String(p_args.get("path", "")));
		}
		const String op = p_args.get("op", "");
		PackedInt32Array faces = p_args.has("faces") ? PackedInt32Array(from_json(p_args["faces"], Variant::PACKED_INT32_ARRAY))
													 : PackedInt32Array(m->get_selection_state().get("sel_faces", PackedInt32Array()));
		if (op == "world") {
			const float sc = (float)f(p_args.get("scale", 1.0));
			undoable(ur, m, "MCP: World UV", [&]() { m->set_world_uv_all(sc); });
			return text_result("World UVs on (all faces)");
		}
		static const char *ops[] = { "unwrap_angle", "unwrap_conformal", "unwrap_minstretch", "smart_project", "project_box", "project_cylinder", "project_sphere",
			"relax", "pack", "quadrify" };
		bool known = false;
		for (const char *o : ops) {
			known = known || op == o;
		}
		if (!known) {
			return fail("op: world, unwrap_angle, unwrap_conformal, smart_project, project_box, project_cylinder, project_sphere, relax, pack");
		}
		if (!plugin->uv_canvas) {
			return fail("No UV editor");
		}
		if (faces.is_empty()) {
			for (int i = 0; i < m->get_face_count(); i++) {
				faces.push_back(i);
			}
		}
		// (on the mesh in face mode, as the U menu works; its faces given)
		if (plugin->get_edited() != m) {
			if (plugin->mode != HewnEditorPlugin::MODE_OBJECT) {
				plugin->set_mode(0);
			}
			ei->get_selection()->clear();
			ei->get_selection()->add_node(m);
			plugin->_edit(m);
		}
		if (plugin->mode != HewnEditorPlugin::MODE_FACE) {
			plugin->set_mode(HewnEditorPlugin::MODE_FACE);
		}
		std::vector<int> fv;
		for (int x : faces) {
			if (x >= 0 && x < m->get_face_count()) {
				fv.push_back(x);
			}
		}
		HewnUVCanvas *uv = plugin->uv_canvas;
		uv->op_faces_override = &fv;
		uv->run_uv_op(op);
		uv->op_faces_override = nullptr;
		Dictionary o;
		o["op"] = op;
		o["faces"] = (int64_t)fv.size();
		return text_result(o);
	}
	r_handled = false;
	return Dictionary();
}
