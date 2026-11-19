#include "hw_hotspot_atlas.h"

#include <godot_cpp/classes/standard_material3d.hpp>

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
	return texture.is_valid() ? Vector2(MAX(1, texture->get_width()), MAX(1, texture->get_height())) : Vector2(1, 1);
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
	emit_changed();
	return true;
}

void HewnHotspotAtlas::reset() {
	cells.clear();
	cells.push_back(Rect2(0, 0, 1, 1));
	orientations = PackedInt32Array();
	tilings = PackedInt32Array();
	_fit_arrays();
	emit_changed();
}

std::vector<hewn::HotspotRect> HewnHotspotAtlas::get_rects() const {
	std::vector<hewn::HotspotRect> out;
	for (int c = 0; c < (int)cells.size(); c++) {
		hewn::HotspotRect h;
		h.r = Rect2(cells[c]).abs();
		h.tile = get_cell_tiling(c);
		h.orient = get_cell_orientation(c);
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

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_texture", "get_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "Material"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pixels_per_meter", PROPERTY_HINT_RANGE, "0,4096,1,or_greater,suffix:px/m"), "set_pixels_per_meter", "get_pixels_per_meter");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "cells", PROPERTY_HINT_ARRAY_TYPE, "Rect2", PROPERTY_USAGE_STORAGE), "set_cells", "get_cells");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "orientations", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_orientations", "get_orientations");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "tilings", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_tilings", "get_tilings");

	BIND_ENUM_CONSTANT(ORIENT_ANY);
	BIND_ENUM_CONSTANT(ORIENT_UPWARDS);
	BIND_ENUM_CONSTANT(ORIENT_FLOOR);
	BIND_ENUM_CONSTANT(ORIENT_CEILING);
	BIND_ENUM_CONSTANT(TILE_NONE);
	BIND_ENUM_CONSTANT(TILE_U);
	BIND_ENUM_CONSTANT(TILE_V);
}
