#include "hw_hotspot_atlas.h"

#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cmath>

using namespace godot;

namespace {
const float EPS = 1e-5f;
} // namespace

HewnHotspotAtlas::HewnHotspotAtlas() {
	reset();
}

void HewnHotspotAtlas::_fit_arrays() {
	const int n = (int)cells.size();
	if (orientations.size() != n) {
		const int old = (int)orientations.size();
		orientations.resize(n);
		for (int i = old; i < n; i++) {
			orientations[i] = ORIENT_ANY;
		}
	}
	if (enabled.size() != n) {
		const int old = (int)enabled.size();
		enabled.resize(n);
		for (int i = old; i < n; i++) {
			enabled[i] = 1;
		}
	}
	while (extras.size() < n) {
		extras.push_back(Dictionary());
	}
	if (extras.size() > n) {
		extras.resize(n);
	}
	if (scales.size() != n) {
		const int old = (int)scales.size();
		scales.resize(n);
		for (int i = old; i < n; i++) {
			scales[i] = 1.0f;
		}
	}
	if (tilings.size() != n) {
		const int old = (int)tilings.size();
		tilings.resize(n);
		for (int i = old; i < n; i++) {
			tilings[i] = TILE_NONE;
		}
	}
}

void HewnHotspotAtlas::set_texture(const Ref<Texture2D> &p_texture) {
	texture = p_texture;
	Ref<StandardMaterial3D> sm = material;
	if (sm.is_valid() && sm->has_meta("hewn_hotspot_made")) {
		sm->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, texture); // (the one made from the texture follows it)
	}
	emit_changed();
}

void HewnHotspotAtlas::set_material(const Ref<Material> &p_material) {
	material = p_material;
	emit_changed();
}

Ref<Material> HewnHotspotAtlas::get_or_make_material() {
	if (material.is_null()) {
		Ref<StandardMaterial3D> sm;
		sm.instantiate();
		sm->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, texture);
		sm->set_meta("hewn_hotspot_made", true);
		material = sm;
	}
	return material;
}

void HewnHotspotAtlas::set_cells(const TypedArray<Rect2> &p_cells) {
	cells = p_cells;
	_fit_arrays();
	emit_changed();
}

void HewnHotspotAtlas::set_orientations(const PackedInt32Array &p_values) {
	orientations = p_values;
	_fit_arrays();
	emit_changed();
}

void HewnHotspotAtlas::set_tilings(const PackedInt32Array &p_values) {
	tilings = p_values;
	_fit_arrays();
	emit_changed();
}

void HewnHotspotAtlas::set_enabled_cells(const PackedInt32Array &p_values) {
	enabled = p_values;
	_fit_arrays();
	emit_changed();
}

void HewnHotspotAtlas::set_scales(const PackedFloat32Array &p_values) {
	scales = p_values;
	_fit_arrays();
	emit_changed();
}

bool HewnHotspotAtlas::is_cell_enabled(int p_cell) const {
	return p_cell >= 0 && p_cell < enabled.size() ? enabled[p_cell] != 0 : true;
}

void HewnHotspotAtlas::set_cell_enabled(int p_cell, bool p_on) {
	ERR_FAIL_INDEX(p_cell, (int)cells.size());
	_fit_arrays();
	enabled[p_cell] = p_on ? 1 : 0;
	emit_changed();
}

float HewnHotspotAtlas::get_cell_scale(int p_cell) const {
	return p_cell >= 0 && p_cell < scales.size() ? scales[p_cell] : 1.0f;
}

void HewnHotspotAtlas::set_cell_scale(int p_cell, float p_scale) {
	ERR_FAIL_INDEX(p_cell, (int)cells.size());
	_fit_arrays();
	scales[p_cell] = CLAMP(p_scale, 0.01f, 100.0f);
	emit_changed();
}

void HewnHotspotAtlas::set_extras(const TypedArray<Dictionary> &p_values) {
	extras = p_values;
	_fit_arrays();
	emit_changed();
}

Variant HewnHotspotAtlas::get_cell_value(int p_cell, const String &p_key, const Variant &p_default) const {
	if (p_cell < 0 || p_cell >= extras.size()) {
		return p_default;
	}
	const Dictionary d = extras[p_cell];
	return d.get(p_key, p_default);
}

void HewnHotspotAtlas::set_cell_value(int p_cell, const String &p_key, const Variant &p_value) {
	ERR_FAIL_INDEX(p_cell, (int)cells.size());
	_fit_arrays();
	Dictionary d = Dictionary(extras[p_cell]).duplicate();
	d[p_key] = p_value;
	extras[p_cell] = d;
	emit_changed();
}

void HewnHotspotAtlas::set_pixels_per_meter(float p_value) {
	pixels_per_meter = MAX(0.0f, p_value);
	emit_changed();
}

Rect2 HewnHotspotAtlas::get_cell(int p_cell) const {
	ERR_FAIL_INDEX_V(p_cell, (int)cells.size(), Rect2());
	return cells[p_cell];
}

int HewnHotspotAtlas::get_cell_orientation(int p_cell) const {
	return p_cell >= 0 && p_cell < orientations.size() ? orientations[p_cell] : ORIENT_ANY;
}

void HewnHotspotAtlas::set_cell_orientation(int p_cell, int p_orientation) {
	ERR_FAIL_INDEX(p_cell, (int)cells.size());
	_fit_arrays();
	orientations[p_cell] = CLAMP(p_orientation, 0, 3);
	emit_changed();
}

int HewnHotspotAtlas::get_cell_tiling(int p_cell) const {
	return p_cell >= 0 && p_cell < tilings.size() ? tilings[p_cell] : TILE_NONE;
}

int HewnHotspotAtlas::get_cell_tilings_possible(int p_cell) const {
	if (p_cell < 0 || p_cell >= (int)cells.size()) {
		return 0;
	}
	const Rect2 r = cells[p_cell];
	int m = 0;
	if (r.position.x <= EPS && r.get_end().x >= 1.0f - EPS) {
		m |= 1; // (touches the left and the right side: it can repeat along U)
	}
	if (r.position.y <= EPS && r.get_end().y >= 1.0f - EPS) {
		m |= 2;
	}
	return m;
}

void HewnHotspotAtlas::set_cell_tiling(int p_cell, int p_tiling) {
	ERR_FAIL_INDEX(p_cell, (int)cells.size());
	_fit_arrays();
	const int possible = get_cell_tilings_possible(p_cell);
	tilings[p_cell] = (p_tiling == TILE_U && (possible & 1)) ? TILE_U : (p_tiling == TILE_V && (possible & 2)) ? TILE_V : TILE_NONE;
	emit_changed();
}

int HewnHotspotAtlas::get_cell_at(const Vector2 &p_uv) const {
	for (int c = 0; c < (int)cells.size(); c++) {
		const Rect2 r = cells[c];
		if (p_uv.x >= r.position.x - EPS && p_uv.x <= r.get_end().x + EPS && p_uv.y >= r.position.y - EPS && p_uv.y <= r.get_end().y + EPS) {
			return c;
		}
	}
	return -1;
}

Vector2 HewnHotspotAtlas::get_image_size() const {
	const Ref<Texture2D> t = get_image();
	return t.is_valid() ? Vector2(MAX(1, t->get_width()), MAX(1, t->get_height())) : Vector2(1, 1);
}

namespace {
Ref<Texture2D> material_image(const Ref<Material> &p_material) {
	const Ref<BaseMaterial3D> bm = p_material;
	if (bm.is_valid()) {
		return bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
	}
	const Ref<ShaderMaterial> sm = p_material;
	if (sm.is_null() || sm->get_shader().is_null()) {
		return Ref<Texture2D>();
	}
	// a shader's texture parameters: one named like the color first, else the first one set
	Ref<Texture2D> first, named;
	const Array us = sm->get_shader()->get_shader_uniform_list();
	for (int64_t i = 0; i < us.size(); i++) {
		const Dictionary u = us[i];
		const String nm = u.get("name", "");
		const Ref<Texture2D> t = sm->get_shader_parameter(nm);
		if (t.is_null()) {
			continue;
		}
		first = first.is_valid() ? first : t;
		const String l = nm.to_lower();
		if (named.is_null() && (l.contains("albedo") || l.contains("base") || l.contains("color") || l.contains("main") || l.contains("diffuse"))) {
			named = t;
		}
	}
	return named.is_valid() ? named : first;
}
} // namespace

Ref<Texture2D> HewnHotspotAtlas::get_image() const {
	const Ref<Texture2D> t = material.is_valid() ? material_image(material) : Ref<Texture2D>();
	return t.is_valid() ? t : texture;
}

bool HewnHotspotAtlas::has_material_image() const {
	return material.is_valid() && material_image(material).is_valid() && !Ref<Material>(material)->has_meta("hewn_hotspot_made");
}

void HewnHotspotAtlas::set_scale(float p_scale) {
	scale = CLAMP(p_scale, 0.01f, 100.0f);
	emit_changed();
}

int HewnHotspotAtlas::cut(bool p_vertical, float p_at, int p_only_cell) {
	_fit_arrays();
	int done = 0;
	const int n = (int)cells.size();
	for (int c = 0; c < n; c++) {
		if (p_only_cell >= 0 && c != p_only_cell) {
			continue;
		}
		const Rect2 r = cells[c];
		const float lo = p_vertical ? r.position.x : r.position.y;
		const float hi = p_vertical ? r.get_end().x : r.get_end().y;
		if (p_at <= lo + EPS || p_at >= hi - EPS) {
			continue; // (the line does not go through it)
		}
		Rect2 a = r, b = r;
		if (p_vertical) {
			a.size.x = p_at - r.position.x;
			b.position.x = p_at;
			b.size.x = r.get_end().x - p_at;
		} else {
			a.size.y = p_at - r.position.y;
			b.position.y = p_at;
			b.size.y = r.get_end().y - p_at;
		}
		cells[c] = a;
		cells.push_back(b);
		orientations.push_back(orientations[c]);
		tilings.push_back(tilings[c]);
		enabled.push_back(enabled[c]);
		extras.push_back(Dictionary(extras[c]).duplicate());
		scales.push_back(scales[c]);
		done++;
	}
	// (a tiling the pieces can no longer have: off)
	for (int c = 0; c < (int)cells.size(); c++) {
		const int t = tilings[c];
		if ((t == TILE_U && !(get_cell_tilings_possible(c) & 1)) || (t == TILE_V && !(get_cell_tilings_possible(c) & 2))) {
			tilings[c] = TILE_NONE;
		}
	}
	if (done) {
		emit_changed();
	}
	return done;
}

int HewnHotspotAtlas::get_neighbor(int p_cell, int p_side) const {
	if (p_cell < 0 || p_cell >= (int)cells.size()) {
		return -1;
	}
	const Rect2 r = cells[p_cell];
	for (int c = 0; c < (int)cells.size(); c++) {
		if (c == p_cell) {
			continue;
		}
		const Rect2 o = cells[c];
		const bool same_rows = std::fabs(o.position.y - r.position.y) < EPS && std::fabs(o.size.y - r.size.y) < EPS;
		const bool same_cols = std::fabs(o.position.x - r.position.x) < EPS && std::fabs(o.size.x - r.size.x) < EPS;
		if ((p_side == 0 && same_rows && std::fabs(o.get_end().x - r.position.x) < EPS) || (p_side == 1 && same_rows && std::fabs(o.position.x - r.get_end().x) < EPS) ||
				(p_side == 2 && same_cols && std::fabs(o.get_end().y - r.position.y) < EPS) || (p_side == 3 && same_cols && std::fabs(o.position.y - r.get_end().y) < EPS)) {
			return c;
		}
	}
	return -1;
}

bool HewnHotspotAtlas::merge_cells(int p_a, int p_b) {
	if (p_a == p_b || p_a < 0 || p_b < 0 || p_a >= (int)cells.size() || p_b >= (int)cells.size()) {
		return false;
	}
	bool joined = false;
	for (int side = 0; side < 4; side++) {
		joined = joined || get_neighbor(p_a, side) == p_b;
	}
	if (!joined) {
		return false;
	}
	_fit_arrays();
	cells[p_a] = Rect2(cells[p_a]).merge(Rect2(cells[p_b]));
	cells.remove_at(p_b);
	orientations.remove_at(p_b);
	tilings.remove_at(p_b);
	enabled.remove_at(p_b);
	extras.remove_at(p_b);
	scales.remove_at(p_b);
	emit_changed();
	return true;
}

void HewnHotspotAtlas::reset() {
	cells.clear();
	cells.push_back(Rect2(0, 0, 1, 1));
	orientations = PackedInt32Array();
	tilings = PackedInt32Array();
	enabled = PackedInt32Array();
	scales = PackedFloat32Array();
	extras = TypedArray<Dictionary>();
	_fit_arrays();
	emit_changed();
}

int HewnHotspotAtlas::add_cell(const Rect2 &p_rect, int p_orientation, int p_tiling) {
	_fit_arrays();
	cells.push_back(Rect2());
	orientations.push_back(CLAMP(p_orientation, 0, 3));
	tilings.push_back(TILE_NONE);
	enabled.push_back(1);
	extras.push_back(Dictionary());
	scales.push_back(1.0f);
	const int c = (int)cells.size() - 1;
	set_cell(c, p_rect);
	set_cell_tiling(c, p_tiling);
	return c;
}

void HewnHotspotAtlas::set_cell(int p_cell, const Rect2 &p_rect) {
	ERR_FAIL_INDEX(p_cell, (int)cells.size());
	_fit_arrays();
	Rect2 r = p_rect.abs();
	const float mn = 1e-3f;
	r.position = r.position.clamp(Vector2(), Vector2(1.0f - mn, 1.0f - mn));
	r.size = r.size.max(Vector2(mn, mn));
	r.size = r.size.min(Vector2(1, 1) - r.position);
	cells[p_cell] = r;
	const int t = tilings[p_cell];
	if ((t == TILE_U && !(get_cell_tilings_possible(p_cell) & 1)) || (t == TILE_V && !(get_cell_tilings_possible(p_cell) & 2))) {
		tilings[p_cell] = TILE_NONE;
	}
	emit_changed();
}

void HewnHotspotAtlas::remove_cell(int p_cell) {
	ERR_FAIL_INDEX(p_cell, (int)cells.size());
	_fit_arrays();
	cells.remove_at(p_cell);
	orientations.remove_at(p_cell);
	tilings.remove_at(p_cell);
	enabled.remove_at(p_cell);
	extras.remove_at(p_cell);
	scales.remove_at(p_cell);
	emit_changed();
}

namespace {
// a side of a cell on the line (lo .. hi along it), and on which side of the line the cell is (-1 before, 1 after)
struct CutSide {
	int cell;
	float lo, hi;
	int side;
};
std::vector<CutSide> cut_sides(const TypedArray<Rect2> &p_cells, bool p_vertical, float p_at) {
	std::vector<CutSide> out;
	for (int c = 0; c < (int)p_cells.size(); c++) {
		const Rect2 r = p_cells[c];
		const float a = p_vertical ? r.position.x : r.position.y, b = p_vertical ? r.get_end().x : r.get_end().y;
		const float lo = p_vertical ? r.position.y : r.position.x, hi = p_vertical ? r.get_end().y : r.get_end().x;
		if (std::fabs(b - p_at) < 1e-4f) {
			out.push_back({ c, lo, hi, -1 });
		}
		if (std::fabs(a - p_at) < 1e-4f) {
			out.push_back({ c, lo, hi, 1 });
		}
	}
	return out;
}
} // namespace

Vector2 HewnHotspotAtlas::get_cut_extent(bool p_vertical, float p_at, float p_through) const {
	const std::vector<CutSide> sides = cut_sides(cells, p_vertical, p_at);
	// a cut: sides on both sides of the line; its run: the sides touching the one under the point, both ways
	float lo = 1e9f, hi = -1e9f;
	bool any = false;
	for (const CutSide &s : sides) {
		if (p_through >= s.lo - 1e-4f && p_through <= s.hi + 1e-4f) {
			lo = std::min(lo, s.lo);
			hi = std::max(hi, s.hi);
			any = true;
		}
	}
	if (!any) {
		return Vector2(1, 0);
	}
	bool grew = true;
	while (grew) {
		grew = false;
		for (const CutSide &s : sides) {
			if (s.hi >= lo - 1e-4f && s.lo <= hi + 1e-4f && (s.lo < lo - 1e-5f || s.hi > hi + 1e-5f)) {
				lo = std::min(lo, s.lo);
				hi = std::max(hi, s.hi);
				grew = true;
			}
		}
	}
	bool before = false, after = false;
	for (const CutSide &s : sides) {
		if (s.hi > lo + 1e-4f && s.lo < hi - 1e-4f) {
			before = before || s.side < 0;
			after = after || s.side > 0;
		}
	}
	return before && after ? Vector2(lo, hi) : Vector2(1, 0);
}

int HewnHotspotAtlas::remove_cut(bool p_vertical, float p_at, float p_from, float p_to) {
	int joined = 0;
	bool again = true;
	while (again) {
		again = false;
		const std::vector<CutSide> sides = cut_sides(cells, p_vertical, p_at);
		for (const CutSide &a : sides) {
			if (a.side > 0 || a.lo < p_from - 1e-4f || a.hi > p_to + 1e-4f) {
				continue;
			}
			for (const CutSide &b : sides) {
				if (b.side < 0 || std::fabs(a.lo - b.lo) > 1e-4f || std::fabs(a.hi - b.hi) > 1e-4f) {
					continue;
				}
				if (merge_cells(a.cell, b.cell)) {
					joined++;
					again = true;
				}
				break;
			}
			if (again) {
				break;
			}
		}
	}
	return joined;
}

bool HewnHotspotAtlas::move_cut(bool p_vertical, float p_at, float p_to_at, float p_from, float p_to) {
	const std::vector<CutSide> sides = cut_sides(cells, p_vertical, p_at);
	TypedArray<Rect2> next = cells.duplicate();
	bool moved = false;
	for (const CutSide &s : sides) {
		if (s.lo < p_from - 1e-4f || s.hi > p_to + 1e-4f) {
			continue;
		}
		Rect2 r = next[s.cell];
		float a = p_vertical ? r.position.x : r.position.y, b = p_vertical ? r.get_end().x : r.get_end().y;
		if (s.side < 0) {
			b = p_to_at;
		} else {
			a = p_to_at;
		}
		if (b - a < 1e-3f) {
			return false; // (a cell would vanish: not moved)
		}
		if (p_vertical) {
			r.position.x = a;
			r.size.x = b - a;
		} else {
			r.position.y = a;
			r.size.y = b - a;
		}
		next[s.cell] = r;
		moved = true;
	}
	if (moved) {
		cells = next;
		for (int c = 0; c < (int)cells.size(); c++) {
			const int t = get_cell_tiling(c);
			if ((t == TILE_U && !(get_cell_tilings_possible(c) & 1)) || (t == TILE_V && !(get_cell_tilings_possible(c) & 2))) {
				tilings[c] = TILE_NONE;
			}
		}
		emit_changed();
	}
	return moved;
}

std::vector<hewn::HotspotRect> HewnHotspotAtlas::get_rects() const {
	std::vector<hewn::HotspotRect> out;
	for (int c = 0; c < (int)cells.size(); c++) {
		hewn::HotspotRect h;
		h.r = Rect2(cells[c]).abs();
		h.tile = get_cell_tiling(c);
		h.orient = get_cell_orientation(c);
		h.enabled = is_cell_enabled(c);
		h.scale = get_cell_scale(c) * scale; // (the atlas' own scale times the cell's)
		h.side = (int)get_cell_value(c, "side", 0);
		h.weight = (float)(double)get_cell_value(c, "weight", 1.0);
		h.turn = (int)get_cell_value(c, "turn", 0);
		h.mirror = (bool)get_cell_value(c, "mirror", false);
		h.min_size = (float)(double)get_cell_value(c, "min_size", 0.0);
		h.max_size = (float)(double)get_cell_value(c, "max_size", 0.0);
		const Vector2 img = get_image_size();
		const float pad = (float)(double)get_cell_value(c, "padding", 0.0);
		h.padding = Vector2(pad / img.x, pad / img.y);
		if (h.r.size.x > EPS && h.r.size.y > EPS) {
			out.push_back(h);
		}
	}
	return out;
}

void HewnHotspotAtlas::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_texture", "texture"), &HewnHotspotAtlas::set_texture);
	ClassDB::bind_method(D_METHOD("get_texture"), &HewnHotspotAtlas::get_texture);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &HewnHotspotAtlas::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &HewnHotspotAtlas::get_material);
	ClassDB::bind_method(D_METHOD("get_or_make_material"), &HewnHotspotAtlas::get_or_make_material);
	ClassDB::bind_method(D_METHOD("set_cells", "cells"), &HewnHotspotAtlas::set_cells);
	ClassDB::bind_method(D_METHOD("get_cells"), &HewnHotspotAtlas::get_cells);
	ClassDB::bind_method(D_METHOD("set_orientations", "values"), &HewnHotspotAtlas::set_orientations);
	ClassDB::bind_method(D_METHOD("get_orientations"), &HewnHotspotAtlas::get_orientations);
	ClassDB::bind_method(D_METHOD("set_tilings", "values"), &HewnHotspotAtlas::set_tilings);
	ClassDB::bind_method(D_METHOD("get_tilings"), &HewnHotspotAtlas::get_tilings);
	ClassDB::bind_method(D_METHOD("set_enabled_cells", "values"), &HewnHotspotAtlas::set_enabled_cells);
	ClassDB::bind_method(D_METHOD("get_enabled_cells"), &HewnHotspotAtlas::get_enabled_cells);
	ClassDB::bind_method(D_METHOD("set_scales", "values"), &HewnHotspotAtlas::set_scales);
	ClassDB::bind_method(D_METHOD("get_scales"), &HewnHotspotAtlas::get_scales);
	ClassDB::bind_method(D_METHOD("is_cell_enabled", "cell"), &HewnHotspotAtlas::is_cell_enabled);
	ClassDB::bind_method(D_METHOD("set_cell_enabled", "cell", "on"), &HewnHotspotAtlas::set_cell_enabled);
	ClassDB::bind_method(D_METHOD("get_cell_scale", "cell"), &HewnHotspotAtlas::get_cell_scale);
	ClassDB::bind_method(D_METHOD("set_cell_scale", "cell", "scale"), &HewnHotspotAtlas::set_cell_scale);
	ClassDB::bind_method(D_METHOD("set_extras", "values"), &HewnHotspotAtlas::set_extras);
	ClassDB::bind_method(D_METHOD("get_extras"), &HewnHotspotAtlas::get_extras);
	ClassDB::bind_method(D_METHOD("get_cell_value", "cell", "key", "default"), &HewnHotspotAtlas::get_cell_value, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("set_cell_value", "cell", "key", "value"), &HewnHotspotAtlas::set_cell_value);
	ClassDB::bind_method(D_METHOD("set_pixels_per_meter", "value"), &HewnHotspotAtlas::set_pixels_per_meter);
	ClassDB::bind_method(D_METHOD("get_pixels_per_meter"), &HewnHotspotAtlas::get_pixels_per_meter);
	ClassDB::bind_method(D_METHOD("get_cell_count"), &HewnHotspotAtlas::get_cell_count);
	ClassDB::bind_method(D_METHOD("get_cell", "cell"), &HewnHotspotAtlas::get_cell);
	ClassDB::bind_method(D_METHOD("get_cell_orientation", "cell"), &HewnHotspotAtlas::get_cell_orientation);
	ClassDB::bind_method(D_METHOD("set_cell_orientation", "cell", "orientation"), &HewnHotspotAtlas::set_cell_orientation);
	ClassDB::bind_method(D_METHOD("get_cell_tiling", "cell"), &HewnHotspotAtlas::get_cell_tiling);
	ClassDB::bind_method(D_METHOD("set_cell_tiling", "cell", "tiling"), &HewnHotspotAtlas::set_cell_tiling);
	ClassDB::bind_method(D_METHOD("get_cell_tilings_possible", "cell"), &HewnHotspotAtlas::get_cell_tilings_possible);
	ClassDB::bind_method(D_METHOD("get_cell_at", "uv"), &HewnHotspotAtlas::get_cell_at);
	ClassDB::bind_method(D_METHOD("get_image_size"), &HewnHotspotAtlas::get_image_size);
	ClassDB::bind_method(D_METHOD("cut", "vertical", "at", "only_cell"), &HewnHotspotAtlas::cut, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("merge_cells", "a", "b"), &HewnHotspotAtlas::merge_cells);
	ClassDB::bind_method(D_METHOD("get_neighbor", "cell", "side"), &HewnHotspotAtlas::get_neighbor);
	ClassDB::bind_method(D_METHOD("reset"), &HewnHotspotAtlas::reset);
	ClassDB::bind_method(D_METHOD("add_cell", "rect", "orientation", "tiling"), &HewnHotspotAtlas::add_cell, DEFVAL(0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("set_cell", "cell", "rect"), &HewnHotspotAtlas::set_cell);
	ClassDB::bind_method(D_METHOD("remove_cell", "cell"), &HewnHotspotAtlas::remove_cell);
	ClassDB::bind_method(D_METHOD("get_cut_extent", "vertical", "at", "through"), &HewnHotspotAtlas::get_cut_extent);
	ClassDB::bind_method(D_METHOD("remove_cut", "vertical", "at", "from", "to"), &HewnHotspotAtlas::remove_cut);
	ClassDB::bind_method(D_METHOD("move_cut", "vertical", "at", "to_at", "from", "to"), &HewnHotspotAtlas::move_cut);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_texture", "get_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "Material"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pixels_per_meter", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_pixels_per_meter", "get_pixels_per_meter");
	ClassDB::bind_method(D_METHOD("set_scale", "scale"), &HewnHotspotAtlas::set_scale);
	ClassDB::bind_method(D_METHOD("get_scale"), &HewnHotspotAtlas::get_scale);
	ClassDB::bind_method(D_METHOD("get_image"), &HewnHotspotAtlas::get_image);
	ClassDB::bind_method(D_METHOD("has_material_image"), &HewnHotspotAtlas::has_material_image);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scale", PROPERTY_HINT_RANGE, "0.01,100,0.01"), "set_scale", "get_scale");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "cells", PROPERTY_HINT_ARRAY_TYPE, "Rect2", PROPERTY_USAGE_STORAGE), "set_cells", "get_cells");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "orientations", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_orientations", "get_orientations");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "tilings", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_tilings", "get_tilings");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "enabled_cells", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_enabled_cells", "get_enabled_cells");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "scales", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_scales", "get_scales");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "extras", PROPERTY_HINT_ARRAY_TYPE, "Dictionary", PROPERTY_USAGE_STORAGE), "set_extras", "get_extras");

	BIND_ENUM_CONSTANT(ORIENT_ANY);
	BIND_ENUM_CONSTANT(ORIENT_UPWARDS);
	BIND_ENUM_CONSTANT(ORIENT_FLOOR);
	BIND_ENUM_CONSTANT(ORIENT_CEILING);
	BIND_ENUM_CONSTANT(SIDE_ANY);
	BIND_ENUM_CONSTANT(SIDE_OUTSIDE);
	BIND_ENUM_CONSTANT(SIDE_INSIDE);
	BIND_ENUM_CONSTANT(SIDE_OPENING);
	BIND_ENUM_CONSTANT(TURN_AUTO);
	BIND_ENUM_CONSTANT(TURN_NEVER);
	BIND_ENUM_CONSTANT(TURN_ALWAYS);
	BIND_ENUM_CONSTANT(TILE_NONE);
	BIND_ENUM_CONSTANT(TILE_U);
	BIND_ENUM_CONSTANT(TILE_V);
}
