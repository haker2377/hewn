// The Hotspot tab: the atlas of the selected meshes (a resource of its own; cut into cells, what each cell is for,
// in the Hotspot Atlas panel below), and how the meshes are hotspotted: Randomise, automatic after each
// change, faces joined into one piece, by shape or by size. In edit mode, for the selected faces only; there also a
// cell chosen by hand (the faces become fixed: left as they are) and fixed on / off.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include "../hw_core.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/tab_container.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

using namespace godot;
using hw_editor::T;

namespace {
const char *HS_ORIENT_GLYPHS[4] = { "\xE2\x80\xA2", "\xE2\x86\x91", "\xE2\x96\xB2", "\xE2\x96\xBC" }; // (any, upwards, floor, ceiling)
const char *HS_ORIENT_NAMES[4] = { "Any", "Upwards", "Floor", "Ceiling" };
const char *HS_ORIENT_TIPS[4] = { "Any face, at any rotation", "Near-vertical faces only, kept upright (walls, text)",
	"Faces looking up, at any rotation", "Faces looking down, at any rotation" };
} // namespace

// ------------------------------------------------------------------ canvas

Rect2 HewnHotspotCanvas::image_rect() const {
	const Vector2 s = get_size();
	Ref<HewnHotspotAtlas> a = dock ? dock->hs_atlas() : Ref<HewnHotspotAtlas>();
	const Vector2 img = a.is_valid() ? a->get_image_size() : Vector2(1, 1);
	const float k = MIN((s.x - 16.0f) / img.x, (s.y - 16.0f) / img.y) * (palette ? 1.0f : zoom);
	const Vector2 sz = img * MAX(1e-3f, k);
	return Rect2(((s - sz) * 0.5f + (palette ? Vector2() : pan)).round(), sz);
}

Vector2 HewnHotspotCanvas::to_uv(const Vector2 &p_pos) const {
	const Rect2 r = image_rect();
	return (p_pos - r.position) / r.size;
}

Vector2 HewnHotspotCanvas::to_screen(const Vector2 &p_uv) const {
	const Rect2 r = image_rect();
	return r.position + p_uv * r.size;
}

float HewnHotspotCanvas::snap(float p_uv, bool p_along_x) const {
	Ref<HewnHotspotAtlas> a = dock ? dock->hs_atlas() : Ref<HewnHotspotAtlas>();
	if (!dock || !dock->hs_snap_on || a.is_null()) {
		return CLAMP(p_uv, 0.0f, 1.0f);
	}
	const Vector2 img = a->get_image_size();
	const float px = p_along_x ? img.x : img.y;
	const float step = MAX(1.0f, (float)dock->hs_snap_px) / px;
	return CLAMP(std::round(p_uv / step) * step, 0.0f, 1.0f);
}

bool HewnHotspotCanvas::cut_vertical() const {
	// across the way the mouse last went: going along x picks a place along x, the cut goes down the image
	return std::fabs(move_dir.x) >= std::fabs(move_dir.y);
}

HewnHotspotCanvas::Hit HewnHotspotCanvas::hit_test(const Vector2 &p_pos) const {
	Hit h;
	Ref<HewnHotspotAtlas> a = dock ? dock->hs_atlas() : Ref<HewnHotspotAtlas>();
	if (a.is_null()) {
		return h;
	}
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	const Vector2 uv = to_uv(p_pos);
	const Rect2 ir = image_rect();
	const float grab = 5.0f * es;
	// the round button in a cell's middle, a corner, a side: the nearest thing within reach; else the cell under it
	for (int c = a->get_cell_count() - 1; c >= 0 && !palette; c--) {
		const Rect2 cu = a->get_cell(c);
		const Rect2 cr(to_screen(cu.position), cu.size * ir.size);
		if (p_pos.distance_to(cr.get_center()) < 10.0f * es && cr.size.x > 22.0f * es && cr.size.y > 22.0f * es) {
			h.kind = HIT_BUTTON;
			h.cell = c;
			return h;
		}
	}
	for (int c = a->get_cell_count() - 1; c >= 0 && !palette; c--) {
		const Rect2 cu = a->get_cell(c);
		const Rect2 cr(to_screen(cu.position), cu.size * ir.size);
		const Vector2 corners[4] = { cr.position, Vector2(cr.get_end().x, cr.position.y), cr.get_end(), Vector2(cr.position.x, cr.get_end().y) };
		for (int k = 0; k < 4; k++) {
			if (p_pos.distance_to(corners[k]) < grab * 1.4f) {
				// (a corner shared with other cells is a crossing of cuts: taken as a cut then, below)
				bool shared = false;
				for (int o = 0; o < a->get_cell_count() && !shared; o++) {
					if (o == c) {
						continue;
					}
					const Rect2 ou = a->get_cell(o);
					const Rect2 orr(to_screen(ou.position), ou.size * ir.size);
					shared = orr.grow(1.0f).has_point(corners[k]);
				}
				if (!shared) {
					h.kind = HIT_CORNER;
					h.cell = c;
					h.corner = k;
					return h;
				}
			}
		}
	}
	float best = grab;
	for (int c = 0; c < a->get_cell_count() && !palette; c++) {
		const Rect2 cu = a->get_cell(c);
		const Rect2 cr(to_screen(cu.position), cu.size * ir.size);
		const float xs[2] = { cr.position.x, cr.get_end().x };
		const float ys[2] = { cr.position.y, cr.get_end().y };
		for (int k = 0; k < 2; k++) {
			const float dx = std::fabs(p_pos.x - xs[k]);
			if (dx < best && p_pos.y >= cr.position.y - grab && p_pos.y <= cr.get_end().y + grab) {
				best = dx;
				h.kind = HIT_CUT;
				h.cell = c;
				h.vertical = true;
				h.at = k ? cu.get_end().x : cu.position.x;
			}
			const float dy = std::fabs(p_pos.y - ys[k]);
			if (dy < best && p_pos.x >= cr.position.x - grab && p_pos.x <= cr.get_end().x + grab) {
				best = dy;
				h.kind = HIT_CUT;
				h.cell = c;
				h.vertical = false;
				h.at = k ? cu.get_end().y : cu.position.y;
			}
		}
	}
	if (h.kind == HIT_CUT) {
		const Vector2 ext = a->get_cut_extent(h.vertical, h.at, h.vertical ? uv.y : uv.x);
		h.shared = ext.x <= ext.y;
		if (h.shared) {
			h.from = ext.x;
			h.to = ext.y;
		} else {
			// (the outer side of a cell: only it)
			const Rect2 cu = a->get_cell(h.cell);
			h.from = h.vertical ? cu.position.y : cu.position.x;
			h.to = h.vertical ? cu.get_end().y : cu.get_end().x;
		}
		return h;
	}
	if (uv.x < 0.0f || uv.y < 0.0f || uv.x > 1.0f || uv.y > 1.0f) {
		return h;
	}
	h.cell = a->get_cell_at(uv);
	h.kind = h.cell >= 0 ? HIT_BODY : HIT_EMPTY;
	return h;
}

void HewnHotspotCanvas::_notification(int p_what) {
	if (p_what == NOTIFICATION_MOUSE_EXIT) {
		hover = false;
		queue_redraw();
		return;
	}
	if (p_what != NOTIFICATION_DRAW || !dock) {
		return;
	}
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	// (behind the image: one of the editor's own dark colors, as its panels)
	Ref<Theme> et = EditorInterface::get_singleton()->get_editor_theme();
	draw_rect(Rect2(Vector2(), get_size()), et.is_valid() && et->has_color("dark_color_2", "Editor") ? et->get_color("dark_color_2", "Editor") : Color(0.15f, 0.15f, 0.15f));
	Ref<HewnHotspotAtlas> a = dock->hs_atlas();
	Ref<Font> font = get_theme_default_font();
	const int fs = get_theme_default_font_size();
	if (a.is_null()) {
		if (font.is_valid()) {
			draw_string(font, Vector2(8, 8 + fs), T(dock->hs_meshes().empty() ? "No atlas: select a Hewn mesh" : "No atlas: make one (+)"), HORIZONTAL_ALIGNMENT_LEFT, get_size().x - 16, fs, Color(1, 1, 1, 0.6f));
		}
		return;
	}
	const Rect2 r = image_rect();
	if (a->get_texture().is_valid()) {
		draw_texture_rect(a->get_texture(), r, false);
	} else {
		draw_rect(r, Color(0.25f, 0.25f, 0.27f));
	}
	draw_rect(r, Color(1, 1, 1, 0.25f), false, 1.0f);
	const Color accent = dock->toggle_color;
	const Hit h = hover && !dragging && !drawing && !panning ? hit_test(mouse) : Hit();
	const bool ctrl = Input::get_singleton()->is_key_pressed(KEY_CTRL);
	for (int c = 0; c < a->get_cell_count(); c++) {
		const Rect2 cu = a->get_cell(c);
		const Rect2 cr(to_screen(cu.position), cu.size * r.size);
		const bool picked = c == dock->hs_cell && !palette;
		if (picked) {
			draw_rect(cr, Color(accent.r, accent.g, accent.b, 0.3f));
		}
		if (c == h.cell && (h.kind == HIT_BODY || h.kind == HIT_BUTTON) && palette) {
			draw_rect(cr, Color(1, 1, 1, 0.15f));
		}
		draw_rect(cr.grow(-0.5f), Color(0, 0, 0, 0.6f), false, 3.0f * es);
		draw_rect(cr.grow(-0.5f), picked ? accent : Color(1, 1, 1, 0.9f), false, 1.0f * es);
		if (cr.size.x < 22.0f * es || cr.size.y < 22.0f * es) {
			continue;
		}
		// its number, and the round button: what faces it is for (a circle: any; an arrow up: upwards; an L: the floor;
		// a Г: the ceiling); tiling beside the number
		const Vector2 c0 = cr.get_center();
		const float br = 9.0f * es;
		const bool on_button = !palette && h.kind == HIT_BUTTON && h.cell == c;
		draw_circle(c0, br, on_button ? Color(0.2f, 0.2f, 0.22f, 0.95f) : Color(0.1f, 0.1f, 0.11f, 0.85f));
		draw_arc(c0, br, 0, (float)Math::TAU, 24, Color(1, 1, 1, on_button ? 0.9f : 0.5f), 1.0f * es, true);
		const Color gc(1, 1, 1);
		const float g = 4.5f * es;
		switch (a->get_cell_orientation(c)) {
			case HewnHotspotAtlas::ORIENT_UPWARDS:
				draw_line(c0 + Vector2(0, g), c0 + Vector2(0, -g), gc, 1.5f * es, true);
				draw_line(c0 + Vector2(0, -g), c0 + Vector2(-g * 0.7f, -g * 0.3f), gc, 1.5f * es, true);
				draw_line(c0 + Vector2(0, -g), c0 + Vector2(g * 0.7f, -g * 0.3f), gc, 1.5f * es, true);
				break;
			case HewnHotspotAtlas::ORIENT_FLOOR:
				draw_polyline(PackedVector2Array({ c0 + Vector2(-g * 0.6f, -g), c0 + Vector2(-g * 0.6f, g * 0.8f), c0 + Vector2(g, g * 0.8f) }), gc, 1.5f * es, true);
				break;
			case HewnHotspotAtlas::ORIENT_CEILING:
				draw_polyline(PackedVector2Array({ c0 + Vector2(-g * 0.6f, g), c0 + Vector2(-g * 0.6f, -g * 0.8f), c0 + Vector2(g, -g * 0.8f) }), gc, 1.5f * es, true);
				break;
			default:
				draw_arc(c0, g * 0.75f, 0, (float)Math::TAU, 16, gc, 1.5f * es, true);
				break;
		}
		if (font.is_valid()) {
			String t = itos(c + 1);
			const int tl = a->get_cell_tiling(c);
			if (tl != HewnHotspotAtlas::TILE_NONE) {
				t += String::utf8(tl == HewnHotspotAtlas::TILE_U ? " \xE2\x86\x94" : " \xE2\x86\x95");
			}
			const Vector2 ts = font->get_string_size(t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs);
			// (above the button; in a low cell, left of it)
			const bool low = cr.size.y < br * 2.0f + fs * 3.2f;
			const Vector2 tp = low ? c0 + Vector2(-br - 4.0f * es - ts.x, fs * 0.35f) : c0 - Vector2(ts.x * 0.5f, br + 4.0f * es);
			draw_string_outline(font, tp, t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, (int)(3 * es), Color(0, 0, 0, 0.8f));
			draw_string(font, tp, t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color(1, 1, 1));
		}
	}
	if (palette) {
		return;
	}
	const Color hl(1.0f, 0.85f, 0.2f);
	auto line = [&](const Vector2 &p0, const Vector2 &p1) {
		draw_line(p0, p1, Color(0, 0, 0, 0.7f), 3.0f * es);
		draw_line(p0, p1, hl, 1.0f * es);
	};
	if (drawing) {
		const Rect2 nr = Rect2(new_a, new_b - new_a).abs();
		const Rect2 sr(to_screen(nr.position), nr.size * r.size);
		draw_rect(sr, Color(hl.r, hl.g, hl.b, 0.15f));
		draw_rect(sr, hl, false, 1.0f * es);
	} else if (h.kind == HIT_CUT) {
		// the side / cut under the mouse: drag to move it, X / Delete to take it away
		line(to_screen(h.vertical ? Vector2(h.at, h.from) : Vector2(h.from, h.at)), to_screen(h.vertical ? Vector2(h.at, h.to) : Vector2(h.to, h.at)));
	} else if (h.kind == HIT_CORNER) {
		const Rect2 cu = a->get_cell(h.cell);
		const Vector2 corners[4] = { cu.position, Vector2(cu.get_end().x, cu.position.y), cu.get_end(), Vector2(cu.position.x, cu.get_end().y) };
		draw_circle(to_screen(corners[h.corner]), 4.0f * es, hl);
	} else if (h.kind == HIT_BODY) {
		// the cut a click would make: across the image (Ctrl: this cell), across the way the mouse went
		const bool v = cut_vertical();
		const Vector2 uv = to_uv(mouse);
		const float at = snap(v ? uv.x : uv.y, v);
		const Rect2 cu = ctrl ? a->get_cell(h.cell) : Rect2(0, 0, 1, 1);
		line(to_screen(v ? Vector2(at, cu.position.y) : Vector2(cu.position.x, at)), to_screen(v ? Vector2(at, cu.get_end().y) : Vector2(cu.get_end().x, at)));
	}
}

void HewnHotspotCanvas::_gui_input(const Ref<InputEvent> &p_event) {
	if (!dock) {
		return;
	}
	Ref<HewnHotspotAtlas> a = dock->hs_atlas();
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		const Vector2 d = mm->get_position() - mouse;
		if (d.length() > 0.5f) {
			move_dir = move_dir * 0.6f + d.normalized() * 0.4f; // (the way it went lately, not each little jitter)
		}
		mouse = mm->get_position();
		hover = true;
		if (panning) {
			pan += mm->get_relative();
		} else if (a.is_valid() && press.kind != HIT_NONE && !dragging && !drawing && mouse.distance_to(press_pos) > 4.0f) {
			// a press moved: a drag of what it was on
			if (press_shift || press.kind == HIT_EMPTY) {
				drawing = true;
				const Vector2 p = to_uv(press_pos);
				new_a = Vector2(snap(p.x, true), snap(p.y, false));
			} else {
				dragging = true;
				drag_before = dock->hs_atlas_state();
				if (press.cell >= 0) {
					drag_cell0 = a->get_cell(press.cell);
				}
			}
		}
		if (drawing) {
			const Vector2 p = to_uv(mouse);
			new_b = Vector2(snap(p.x, true), snap(p.y, false));
		} else if (dragging && a.is_valid()) {
			const Vector2 p = to_uv(mouse), p0 = to_uv(press_pos);
			if (press.kind == HIT_CUT) {
				const float to = snap(press.vertical ? p.x : p.y, press.vertical);
				const float now = press.at;
				if (std::fabs(to - now) > 1e-6f) {
					if (press.shared) {
						if (a->move_cut(press.vertical, now, to, press.from, press.to)) {
							press.at = to;
						}
					} else if (press.cell >= 0) {
						// (an outer side: only that cell's)
						Rect2 cu = a->get_cell(press.cell);
						float lo = press.vertical ? cu.position.x : cu.position.y, hi = press.vertical ? cu.get_end().x : cu.get_end().y;
						if (std::fabs(lo - now) < 1e-4f) {
							lo = to;
						} else {
							hi = to;
						}
						if (hi - lo > 1e-3f) {
							if (press.vertical) {
								cu.position.x = lo;
								cu.size.x = hi - lo;
							} else {
								cu.position.y = lo;
								cu.size.y = hi - lo;
							}
							a->set_cell(press.cell, cu);
							press.at = to;
						}
					}
				}
			} else if (press.kind == HIT_CORNER && press.cell >= 0) {
				Rect2 cu = drag_cell0;
				Vector2 lo = cu.position, hi = cu.get_end();
				const Vector2 q(snap(p.x, true), snap(p.y, false));
				if (press.corner == 0 || press.corner == 3) {
					lo.x = q.x;
				} else {
					hi.x = q.x;
				}
				if (press.corner == 0 || press.corner == 1) {
					lo.y = q.y;
				} else {
					hi.y = q.y;
				}
				a->set_cell(press.cell, Rect2(lo, hi - lo));
			} else if ((press.kind == HIT_BODY || press.kind == HIT_BUTTON) && press.cell >= 0) {
				// the cell moved, its corner on the step
				const Vector2 off = p - p0;
				Vector2 np = drag_cell0.position + off;
				np = Vector2(snap(np.x, true), snap(np.y, false));
				np = np.clamp(Vector2(), Vector2(1, 1) - drag_cell0.size);
				a->set_cell(press.cell, Rect2(np, drag_cell0.size));
			}
		}
		queue_redraw();
		return;
	}
	Ref<InputEventKey> k = p_event;
	if (k.is_valid() && !palette) {
		if (k->is_pressed() && (k->get_keycode() == KEY_BRACKETLEFT || k->get_keycode() == KEY_BRACKETRIGHT)) {
			dock->hs_set_snap(k->get_keycode() == KEY_BRACKETLEFT ? dock->hs_snap_px / 2 : dock->hs_snap_px * 2);
			accept_event();
		} else if (k->is_pressed() && !k->is_echo() && (k->get_keycode() == KEY_X || k->get_keycode() == KEY_DELETE) && a.is_valid()) {
			// the cut under the mouse away (its two sides joined), or the cell
			const Hit h = hit_test(mouse);
			if (h.kind == HIT_CUT && h.shared) {
				dock->hs_remove_cut(h.vertical, h.at, h.from, h.to);
			} else if ((h.kind == HIT_BODY || h.kind == HIT_BUTTON || h.kind == HIT_CORNER || h.kind == HIT_CUT) && h.cell >= 0) {
				dock->hs_remove_cell(h.cell);
			}
			accept_event();
		}
		queue_redraw();
		return;
	}
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_null()) {
		return;
	}
	mouse = mb->get_position();
	if (!palette && (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP || mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN) && mb->is_pressed()) {
		// zoom about the mouse
		const Vector2 uv = to_uv(mouse);
		zoom = CLAMP(zoom * (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? 1.15f : 1.0f / 1.15f), 0.2f, 40.0f);
		pan += mouse - to_screen(uv);
		queue_redraw();
		accept_event();
		return;
	}
	if (!palette && mb->get_button_index() == MOUSE_BUTTON_MIDDLE) {
		panning = mb->is_pressed();
		accept_event();
		return;
	}
	if (mb->get_button_index() != MOUSE_BUTTON_LEFT) {
		return;
	}
	if (!palette) {
		grab_focus(); // (X / Delete, [ / ] then)
	}
	if (mb->is_pressed()) {
		press = hit_test(mouse);
		press_pos = mouse;
		press_shift = mb->is_shift_pressed();
		press_ctrl = mb->is_ctrl_pressed();
		dragging = drawing = false;
		if (palette && (press.kind == HIT_BODY) && press.cell >= 0) {
			dock->_on_hs_palette_pick(press.cell);
			press = Hit();
		}
		accept_event();
		queue_redraw();
		return;
	}
	// released: the end of a drag (one undo step), or a click
	if (drawing) {
		drawing = false;
		const Rect2 nr = Rect2(new_a, new_b - new_a).abs();
		if (a.is_valid() && nr.size.x > 1e-3f && nr.size.y > 1e-3f) {
			const Array before = dock->hs_atlas_state();
			const int c = a->add_cell(nr);
			dock->hs_atlas_commit("Hotspot New Cell", before);
			dock->hs_pick_cell(c);
		}
	} else if (dragging) {
		dragging = false;
		const char *names[6] = { "", "", "Hotspot Move Cell", "Hotspot Move Cell", "Hotspot Move Cut", "Hotspot Cell Corner" };
		dock->hs_atlas_commit(names[press.kind], drag_before);
	} else if (a.is_valid()) {
		if (press.kind == HIT_BUTTON) {
			dock->hs_cycle_orientation(press.cell);
			dock->hs_pick_cell(press.cell);
		} else if (press.kind == HIT_BODY) {
			const bool v = cut_vertical();
			const Vector2 uv = to_uv(mouse);
			const float at = snap(v ? uv.x : uv.y, v);
			dock->hs_cut(v, at, press_ctrl ? press.cell : -1);
		}
	}
	press = Hit();
	accept_event();
	queue_redraw();
}

// ------------------------------------------------------------------ tab

Ref<HewnHotspotAtlas> HewnDock::hs_atlas() const {
	if (plugin) {
		for (HewnMesh3D *n : hs_meshes()) {
			const TypedArray<HewnHotspotAtlas> l = n->get_hotspot_atlases();
			if (!l.is_empty()) {
				return Ref<HewnHotspotAtlas>(l[CLAMP(hs_atlas_index, 0, (int)l.size() - 1)]);
			}
		}
	}
	return hs_panel_atlas;
}

std::vector<HewnMesh3D *> HewnDock::hs_meshes() const {
	std::vector<HewnMesh3D *> out;
	if (!plugin) {
		return out;
	}
	if (plugin->mode != HewnEditorPlugin::MODE_OBJECT && plugin->get_edited()) {
		out.push_back(plugin->get_edited());
		return out;
	}
	return plugin->_selected_meshes();
}

float HewnDock::hs_ppm() const {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	return es.is_valid() ? (float)(double)es->get_project_metadata("hewn", "pixels_per_meter", 128) : 128.0f;
}

void HewnDock::_build_hotspot_tab() {
	ScrollContainer *sc = memnew(ScrollContainer);
	sc->set_name("Hotspot");
	sc->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	VBoxContainer *v = memnew(VBoxContainer);
	v->set_name("HewnHotspotTab");
	v->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	sc->add_child(v);
	tabs->add_child(sc);
	tabs->connect("tab_changed", callable_mp(this, &HewnDock::hs_refresh).unbind(1)); // (shown: up to date)
	auto heading = [&](const String &p_text) {
		Label *l = memnew(Label);
		l->set_text(p_text);
		l->add_theme_color_override("font_color", EditorInterface::get_singleton()->get_editor_theme()->get_color("accent_color", "Editor"));
		v->add_child(l);
	};
	auto row = [&](const String &p_label, Control *p_control) {
		HBoxContainer *h = memnew(HBoxContainer);
		Label *l = memnew(Label);
		l->set_text(p_label);
		l->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		h->add_child(l);
		p_control->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		h->add_child(p_control);
		v->add_child(h);
	};
	auto check = [&](const String &p_text, const String &p_tip) {
		CheckBox *c = memnew(CheckBox);
		c->set_text(p_text);
		c->set_tooltip_text(p_tip);
		c->connect("toggled", callable_mp(this, &HewnDock::_on_hs_toggle));
		v->add_child(c);
		return c;
	};

	// ---- the atlases of the selected meshes (each cut into cells in the Hotspot Atlas panel below): a face gets a
	// cell of any of them (the one that fits it: outside, inside...), and that atlas' material
	heading("Atlases");
	hs_atlas_list = memnew(ItemList);
	hs_atlas_list->set_name("HotspotAtlasList");
	hs_atlas_list->set_auto_height(true);
	hs_atlas_list->set_tooltip_text("The mesh's atlases: each face gets a cell of the one that fits it (and its material); the one picked here is edited below");
	hs_atlas_list->connect("item_selected", callable_mp(this, &HewnDock::_on_hs_atlas_list));
	v->add_child(hs_atlas_list);
	HBoxContainer *lb = memnew(HBoxContainer);
	Button *rem = memnew(Button);
	rem->set_text("Remove");
	rem->set_name("HotspotRemoveAtlas");
	rem->set_tooltip_text("The picked atlas off the selected meshes' list");
	rem->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	rem->connect("pressed", callable_mp(this, &HewnDock::_on_hs_remove_atlas));
	lb->add_child(rem);
	v->add_child(lb);
	hs_atlas_picker = memnew(EditorResourcePicker);
	hs_atlas_picker->set_name("HotspotAtlasPicker");
	hs_atlas_picker->set_base_type("HewnHotspotAtlas");
	hs_atlas_picker->set_tooltip_text("The hotspot atlas of the selected meshes (a resource: save it to use it in other scenes)");
	hs_atlas_picker->connect("resource_changed", callable_mp(this, &HewnDock::_on_hs_atlas_changed));
	row("Atlas", hs_atlas_picker);
	HBoxContainer *ab = memnew(HBoxContainer);
	Button *nb = memnew(Button);
	nb->set_text("New Atlas");
	nb->set_name("HotspotNewAtlas");
	nb->set_tooltip_text("A new atlas added to the selected meshes' list (the image of their material when it has one)");
	nb->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	nb->connect("pressed", callable_mp(this, &HewnDock::_on_hs_new_atlas));
	ab->add_child(nb);
	Button *eb2 = memnew(Button);
	eb2->set_text("Edit Atlas");
	eb2->set_name("HotspotEditAtlas");
	eb2->set_tooltip_text("The Hotspot Atlas panel (below): the atlas' image cut into cells");
	eb2->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	eb2->connect("pressed", callable_mp(this, &HewnDock::_on_hs_edit_atlas));
	ab->add_child(eb2);
	v->add_child(ab);

	// ---- hotspotting (object mode: the whole meshes; edit mode: the selected faces)
	heading("Hotspotting");
	Button *rnd = memnew(Button);
	rnd->set_name("HotspotRandomise");
	rnd->set_text("Randomise Hotspots");
	rnd->set_tooltip_text("Object mode: the selected meshes; edit mode: the selected faces. Another look each time; fixed faces stay");
	rnd->connect("pressed", callable_mp(this, &HewnDock::_on_hs_randomise));
	v->add_child(rnd);
	hs_auto = check("Auto Hotspot", "Hotspotted again a moment after each change of the shape");
	hs_combine = check("Combine Faces", "Joined faces bent less than the angle (not over a seam) get one cell together");
	hs_angle = memnew(SpinBox);
	hs_angle->set_min(0);
	hs_angle->set_max(180);
	hs_angle->set_step(0.5);
	hs_angle->set_suffix(String::utf8("\xC2\xB0"));
	hs_angle->connect("value_changed", callable_mp(this, &HewnDock::_on_hs_setting));
	HewnDragField::hook_spinbox(hs_angle);
	row("Combine Face Angle", hs_angle);
	hs_weight = memnew(SpinBox);
	hs_weight->set_min(0);
	hs_weight->set_max(1);
	hs_weight->set_step(0.01);
	HewnDragField::hook_spinbox(hs_weight);
	hs_weight->set_tooltip_text("Left: the cell of the face's shape; right: the cell of its size (pixels per meter); between: both");
	hs_weight->connect("value_changed", callable_mp(this, &HewnDock::_on_hs_setting));
	row("Aspect / Area", hs_weight);
	hs_world = check("World-Space Size Matching", "Sizes compared in meters (the texture's pixels per meter); off: each piece against the largest one");
	hs_show_fixed = check("Show Fixed Faces", "Fixed faces slightly green in the view (the same as in Overlays)");
	hs_show_fixed->disconnect("toggled", callable_mp(this, &HewnDock::_on_hs_toggle));
	hs_show_fixed->connect("toggled", callable_mp(this, &HewnDock::_on_hs_show_fixed));

	// ---- edit mode: by hand
	VBoxContainer *eb = memnew(VBoxContainer);
	hs_edit_box = eb;
	HBoxContainer *eh = memnew(HBoxContainer);
	Button *cb = memnew(Button);
	cb->set_name("HotspotChoose");
	cb->set_text("Choose Hotspot");
	cb->set_tooltip_text("Pick the cell for the selected faces: they become fixed");
	cb->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	cb->connect("pressed", callable_mp(this, &HewnDock::_on_hs_choose));
	eh->add_child(cb);
	hs_fixed = memnew(Button);
	hs_fixed->set_name("HotspotFixed");
	hs_fixed->set_text("Fixed");
	hs_fixed->set_toggle_mode(true);
	hs_fixed->set_tooltip_text("The selected faces keep their hotspot: Randomise and Auto Hotspot leave them");
	hs_fixed->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	hs_fixed->connect("toggled", callable_mp(this, &HewnDock::_on_hs_fixed));
	eh->add_child(hs_fixed);
	eb->add_child(eh);
	HBoxContainer *hh = memnew(HBoxContainer);
	Button *hold = memnew(Button);
	hold->set_name("HotspotHold");
	hold->set_text("Hold to Atlas");
	hold->set_tooltip_text("The selected faces only ever get cells of the picked atlas (hotspotted again with it now)");
	hold->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	hold->connect("pressed", callable_mp(this, &HewnDock::_on_hs_hold).bind(true));
	hh->add_child(hold);
	Button *free_b = memnew(Button);
	free_b->set_name("HotspotFree");
	free_b->set_text("Any Atlas");
	free_b->set_tooltip_text("The selected faces may get cells of any of the atlases again (the one that fits)");
	free_b->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	free_b->connect("pressed", callable_mp(this, &HewnDock::_on_hs_hold).bind(false));
	hh->add_child(free_b);
	eb->add_child(hh);
	v->add_child(eb);

	// the palette for Choose Hotspot
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	hs_palette_popup = memnew(PopupPanel);
	hs_palette = memnew(HewnHotspotCanvas);
	hs_palette->dock = this;
	hs_palette->palette = true;
	hs_palette->set_custom_minimum_size(Vector2(360, 360) * es);
	hs_palette_popup->add_child(hs_palette);
	add_child(hs_palette_popup);
}

// The Hotspot Atlas panel (the bottom one): the atlas' image cut into cells; beside it (its width dragged by the split)
// the image, the step, the cells' list and the picked one's settings.
void HewnDock::build_hotspot_atlas_panel(Control *p_parent) {
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	HSplitContainer *h = memnew(HSplitContainer);
	h->set_name("HewnHotspotAtlasPanel");
	h->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	p_parent->add_child(h);
	hs_canvas = memnew(HewnHotspotCanvas);
	hs_canvas->set_name("HotspotCanvas");
	hs_canvas->dock = this;
	hs_canvas->set_custom_minimum_size(Vector2(120, 60) * es);
	hs_canvas->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	hs_canvas->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	hs_canvas->set_focus_mode(Control::FOCUS_ALL);
	hs_canvas->set_clip_contents(true);
	hs_canvas->set_tooltip_text("Click: a cut across the image, across the way the mouse went (Ctrl: this cell only). Drag: move the cell, a cut, "
								"a corner. Shift + drag: a new cell. The middle button of a cell: the faces it is for. X / Delete: the cut or the "
								"cell away. Wheel: zoom, middle button: pan");
	h->add_child(hs_canvas);
	// (the right side scrolls: the panel can be made as low as wanted)
	ScrollContainer *vs = memnew(ScrollContainer);
	vs->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	vs->set_custom_minimum_size(Vector2(200, 0) * es);
	h->add_child(vs);
	VBoxContainer *v = memnew(VBoxContainer);
	v->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	v->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	vs->add_child(v);
	h->set_split_offset((int)(-280 * es)); // (the right side's width: dragged by the split's handle)
	// the atlas: which of the mesh's, a new one
	HBoxContainer *ar = memnew(HBoxContainer);
	hs_panel_atlases = memnew(OptionButton);
	hs_panel_atlases->set_name("HotspotPanelAtlases");
	hs_panel_atlases->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	hs_panel_atlases->set_clip_text(true);
	hs_panel_atlases->set_tooltip_text("The atlas edited: one of the selected mesh's");
	hs_panel_atlases->connect("item_selected", callable_mp(this, &HewnDock::_on_hs_atlas_list));
	ar->add_child(hs_panel_atlases);
	Button *nb = memnew(Button);
	nb->set_name("HotspotPanelNewAtlas");
	nb->set_button_icon(icon("Add"));
	nb->set_tooltip_text("New Atlas: added to the selected meshes' atlases (from their material's image, when they have one)");
	nb->connect("pressed", callable_mp(this, &HewnDock::_on_hs_new_atlas));
	ar->add_child(nb);
	v->add_child(ar);
	hs_atlas_name = memnew(Label);
	hs_atlas_name->set_clip_text(true);
	hs_atlas_name->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	v->add_child(hs_atlas_name);
	HBoxContainer *ir = memnew(HBoxContainer);
	Label *il = memnew(Label);
	il->set_text("Image");
	ir->add_child(il);
	hs_texture_picker = memnew(EditorResourcePicker);
	hs_texture_picker->set_base_type("Texture2D");
	hs_texture_picker->set_tooltip_text("The atlas' image");
	hs_texture_picker->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	hs_texture_picker->set_custom_minimum_size(Vector2(0, 40) * es);
	hs_texture_picker->connect("resource_changed", callable_mp(this, &HewnDock::_on_hs_texture_changed));
	ir->add_child(hs_texture_picker);
	v->add_child(ir);
	// the step: a field, on / off
	HBoxContainer *sh = memnew(HBoxContainer);
	hs_snap_check = memnew(CheckBox);
	hs_snap_check->set_text("Snap");
	hs_snap_check->set_pressed(hs_snap_on);
	hs_snap_check->set_tooltip_text("Cuts, sides, corners and moved cells on the step");
	hs_snap_check->connect("toggled", callable_mp(this, &HewnDock::_on_hs_snap_check));
	sh->add_child(hs_snap_check);
	hs_snap_field = memnew(SpinBox);
	hs_snap_field->set_min(1);
	hs_snap_field->set_max(512);
	hs_snap_field->set_step(1);
	hs_snap_field->set_suffix("px");
	hs_snap_field->set_value(hs_snap_px);
	hs_snap_field->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	hs_snap_field->set_tooltip_text("The step in the image's pixels ([ / ] over the image: half / twice)");
	hs_snap_field->connect("value_changed", callable_mp(this, &HewnDock::_on_hs_snap_field));
	HewnDragField::hook_spinbox(hs_snap_field);
	sh->add_child(hs_snap_field);
	Button *rb = memnew(Button);
	rb->set_text("Reset Cuts");
	rb->set_tooltip_text("One cell again: the whole image");
	rb->connect("pressed", callable_mp(this, &HewnDock::_on_hs_reset));
	sh->add_child(rb);
	v->add_child(sh);
	// the cells
	hs_cell_list = memnew(Tree);
	hs_cell_list->set_name("HotspotCellList");
	hs_cell_list->set_hide_root(true);
	hs_cell_list->set_columns(2); // (the check box apart: a click on the text picks, it does not switch it)
	hs_cell_list->set_column_expand(0, false);
	hs_cell_list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	hs_cell_list->set_custom_minimum_size(Vector2(0, 60) * es);
	hs_cell_list->set_tooltip_text("The cells: the check box leaves one out of hotspotting (Choose Hotspot still can pick it)");
	hs_cell_list->set_select_mode(Tree::SELECT_MULTI);
	hs_cell_list->connect("multi_selected", callable_mp(this, &HewnDock::_on_hs_list_multi));
	hs_cell_list->connect("item_edited", callable_mp(this, &HewnDock::_on_hs_list_edited));
	v->add_child(hs_cell_list);
	// the picked cell: which faces it is for, and its tiling (only for a cell as wide / tall as the image: else a gap)
	hs_cell_label = memnew(Label);
	hs_cell_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART); // (its text does not widen the side: the image stays put)
	v->add_child(hs_cell_label);
	HBoxContainer *ch = memnew(HBoxContainer);
	hs_cell_orient = memnew(OptionButton);
	hs_cell_orient->set_name("HotspotCellOrientation");
	for (int i = 0; i < 4; i++) {
		hs_cell_orient->add_item(HS_ORIENT_NAMES[i], i);
		hs_cell_orient->set_item_tooltip(i, HS_ORIENT_TIPS[i]);
	}
	hs_cell_orient->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	hs_cell_orient->connect("item_selected", callable_mp(this, &HewnDock::_on_hs_orient));
	ch->add_child(hs_cell_orient);
	hs_cell_tile = memnew(Button);
	hs_cell_tile->set_name("HotspotToggleTiling");
	hs_cell_tile->set_text("Toggle Tiling");
	hs_cell_tile->set_toggle_mode(true);
	hs_cell_tile->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	hs_cell_tile->set_tooltip_text("The cell repeats along the side it spans the whole image on");
	hs_cell_tile->connect("pressed", callable_mp(this, &HewnDock::_on_hs_tile));
	ch->add_child(hs_cell_tile);
	v->add_child(ch);
	HBoxContainer *scr = memnew(HBoxContainer);
	Label *scl = memnew(Label);
	scl->set_text("Scale");
	scl->set_tooltip_text("The cell's own scale: the pixels per meter times it when sizes are matched (2: half as big in the world)");
	scl->set_mouse_filter(Control::MOUSE_FILTER_PASS);
	scr->add_child(scl);
	hs_cell_scale = memnew(SpinBox);
	hs_cell_scale->set_name("HotspotCellScale");
	hs_cell_scale->set_min(0.01);
	hs_cell_scale->set_max(100);
	hs_cell_scale->set_step(0.01);
	hs_cell_scale->set_value(1);
	hs_cell_scale->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	hs_cell_scale->connect("value_changed", callable_mp(this, &HewnDock::_on_hs_cell_scale));
	HewnDragField::hook_spinbox(hs_cell_scale);
	scr->add_child(hs_cell_scale);
	v->add_child(scr);
	// the other settings of the picked cells (all of them at once)
	GridContainer *g = memnew(GridContainer);
	g->set_columns(2);
	v->add_child(g);
	auto glabel = [&](const String &p_text, const String &p_tip) {
		Label *l = memnew(Label);
		l->set_text(p_text);
		l->set_tooltip_text(p_tip);
		l->set_mouse_filter(Control::MOUSE_FILTER_PASS);
		g->add_child(l);
	};
	auto goption = [&](const String &p_label, const char *p_key, std::initializer_list<const char *> p_items, const String &p_tip) {
		glabel(p_label, p_tip);
		OptionButton *o = memnew(OptionButton);
		for (const char *it : p_items) {
			o->add_item(it);
		}
		o->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		o->set_tooltip_text(p_tip);
		o->connect("item_selected", callable_mp(this, &HewnDock::_on_hs_cell_value_int).bind(String(p_key)));
		g->add_child(o);
		return o;
	};
	auto gfield = [&](const String &p_label, const char *p_key, double p_min, double p_max, double p_step, const String &p_suffix, const String &p_tip) {
		glabel(p_label, p_tip);
		SpinBox *sb = memnew(SpinBox);
		sb->set_min(p_min);
		sb->set_max(p_max);
		sb->set_step(p_step);
		sb->set_suffix(p_suffix);
		sb->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		sb->set_tooltip_text(p_tip);
		sb->connect("value_changed", callable_mp(this, &HewnDock::_on_hs_cell_value_float).bind(String(p_key)));
		HewnDragField::hook_spinbox(sb);
		g->add_child(sb);
		return sb;
	};
	hs_cell_side = goption("Sides", "side", { "Any", "Outside", "Inside", "Opening" },
			"Which faces: any; outside (looking out of the object: the outer faces of walls); inside (a room's, the inner faces of walls); "
			"the sides of an opening (a door's, a window's). Cells for a face's side first, then the ones for any");
	hs_cell_turn = goption("Turn", "turn", { "By Orientation", "Never", "Allowed" },
			"A quarter turn when it fits a face better: by its orientation (upwards: never), never, or allowed (upwards too)");
	glabel("Mirror", "Now and then mirrored (another look of the same cell)");
	hs_cell_mirror = memnew(CheckBox);
	hs_cell_mirror->set_tooltip_text("Now and then mirrored (another look of the same cell)");
	hs_cell_mirror->connect("toggled", callable_mp(this, &HewnDock::_on_hs_cell_value_bool).bind(String("mirror")));
	g->add_child(hs_cell_mirror);
	hs_cell_weight = gfield("Weight", "weight", 0, 100, 0.1, "", "How often among the cells that fit a face as well (0: never by chance)");
	hs_cell_min = gfield("Min Size", "min_size", 0, 10000, 0.05, "m", "Only for pieces at least this long (their longer side; 0: no limit)");
	hs_cell_max = gfield("Max Size", "max_size", 0, 10000, 0.05, "m", "Only for pieces at most this long (0: no limit)");
	hs_cell_pad = gfield("Padding", "padding", 0, 256, 1, "px", "Kept off the cell's edges by this many pixels (no bleeding from the cells around)");
	hw_editor::localize(h);
}

void HewnDock::_on_hs_edit_atlas() {
	if (plugin && plugin->hotspot_dock) {
		plugin->hotspot_dock->make_visible();
	}
}

void HewnDock::hs_set_snap(int p_px) {
	hs_snap_px = CLAMP(p_px, 1, 512);
	if (hs_snap_field && (int)hs_snap_field->get_value() != hs_snap_px) {
		hs_snap_field->set_value_no_signal(hs_snap_px);
	}
	if (hs_canvas) {
		hs_canvas->queue_redraw();
	}
}

void HewnDock::_on_hs_snap_field(double p_value) {
	hs_set_snap((int)p_value);
}

void HewnDock::_on_hs_snap_check(bool p_on) {
	hs_snap_on = p_on;
}

void HewnDock::_on_hs_list_multi(Object *p_item, int p_column, bool p_selected) {
	TreeItem *it = Object::cast_to<TreeItem>(p_item);
	if (it && p_selected && !hs_syncing) {
		hs_cell = (int)it->get_metadata(0);
		callable_mp(this, &HewnDock::hs_refresh).call_deferred(); // (after the tree's own selecting)
	}
}

std::vector<int> HewnDock::hs_cells_picked() const {
	std::vector<int> out;
	for (TreeItem *it = hs_cell_list ? hs_cell_list->get_next_selected(nullptr) : nullptr; it; it = hs_cell_list->get_next_selected(it)) {
		out.push_back((int)it->get_metadata(0));
	}
	if (out.empty() && hs_cell >= 0) {
		out.push_back(hs_cell);
	}
	return out;
}

void HewnDock::_hs_cells_set(const String &p_key, const Variant &p_value) {
	if (hs_syncing) {
		return;
	}
	const std::vector<int> cs = hs_cells_picked();
	if (cs.empty()) {
		return;
	}
	hs_atlas_edit("Hotspot Cell " + p_key.capitalize(), [&](HewnHotspotAtlas *at) {
		for (int c : cs) {
			if (c >= 0 && c < at->get_cell_count()) {
				at->set_cell_value(c, p_key, p_value);
			}
		}
	});
}

void HewnDock::_on_hs_cell_value_int(int p_value, const String &p_key) {
	_hs_cells_set(p_key, p_value);
}

void HewnDock::_on_hs_cell_value_float(double p_value, const String &p_key) {
	_hs_cells_set(p_key, p_value);
}

void HewnDock::_on_hs_cell_value_bool(bool p_value, const String &p_key) {
	_hs_cells_set(p_key, p_value);
}

void HewnDock::_on_hs_list_edited() {
	TreeItem *it = hs_cell_list ? hs_cell_list->get_edited() : nullptr;
	if (!it || hs_syncing) {
		return;
	}
	// (later: the list is filled again by it, not while the tree is still in its click)
	callable_mp(this, &HewnDock::_hs_list_edited_now).call_deferred((int)it->get_metadata(0), it->is_checked(0));
}

void HewnDock::_hs_list_edited_now(int p_cell, bool p_on) {
	Ref<HewnHotspotAtlas> a = hs_atlas();
	if (a.is_null() || p_cell < 0 || p_cell >= a->get_cell_count()) {
		return;
	}
	hs_atlas_edit(p_on ? "Hotspot Cell On" : "Hotspot Cell Off", [&](HewnHotspotAtlas *at) { at->set_cell_enabled(p_cell, p_on); });
}

void HewnDock::_on_hs_cell_scale(double p_value) {
	if (hs_syncing || hs_cell < 0) {
		return;
	}
	const std::vector<int> cs = hs_cells_picked();
	hs_atlas_edit("Hotspot Cell Scale", [&](HewnHotspotAtlas *at) {
		for (int c : cs) {
			at->set_cell_scale(c, (float)p_value);
		}
	});
}

Array HewnDock::hs_atlas_state() const {
	Array s;
	Ref<HewnHotspotAtlas> a = hs_atlas();
	if (a.is_valid()) {
		s.push_back(a->get_cells().duplicate());
		s.push_back(a->get_orientations());
		s.push_back(a->get_tilings());
		s.push_back(a->get_enabled_cells());
		s.push_back(a->get_scales());
		s.push_back(a->get_extras().duplicate(true));
	}
	return s;
}

void HewnDock::hs_atlas_commit(const String &p_action, const Array &p_before) {
	Ref<HewnHotspotAtlas> a = hs_atlas();
	if (a.is_null() || !plugin || p_before.size() < 3) {
		return;
	}
	const Array after = hs_atlas_state();
	EditorUndoRedoManager *ur = plugin->get_undo_redo();
	ur->create_action(String("Hewn: ") + p_action, UndoRedo::MERGE_DISABLE, a.ptr());
	ur->add_do_method(this, "_hs_atlas_restore", a, after);
	ur->add_undo_method(this, "_hs_atlas_restore", a, p_before);
	ur->commit_action(false);
	hs_refresh();
	// (the meshes hotspotting by themselves follow the new cells)
	for (HewnMesh3D *n : hs_meshes()) {
		if (n->get_hotspot_auto() && n->get_hotspot_atlas() == a) {
			n->randomize_hotspots(PackedInt32Array(), false, hs_ppm());
		}
	}
}

void HewnDock::hs_cycle_orientation(int p_cell) {
	Ref<HewnHotspotAtlas> a = hs_atlas();
	if (a.is_null() || p_cell < 0 || p_cell >= a->get_cell_count()) {
		return;
	}
	const int next = (a->get_cell_orientation(p_cell) + 1) % 4; // (any, upwards, floor, ceiling)
	hs_atlas_edit("Hotspot Cell Orientation", [&](HewnHotspotAtlas *at) { at->set_cell_orientation(p_cell, next); });
}

void HewnDock::hs_remove_cell(int p_cell) {
	hs_atlas_edit("Hotspot Remove Cell", [&](HewnHotspotAtlas *at) { at->remove_cell(p_cell); });
	hs_cell = -1;
	hs_refresh();
}

void HewnDock::hs_remove_cut(bool p_vertical, float p_at, float p_from, float p_to) {
	hs_atlas_edit("Hotspot Remove Cut", [&](HewnHotspotAtlas *at) { at->remove_cut(p_vertical, p_at, p_from, p_to); });
	hs_cell = -1;
	hs_refresh();
}

void HewnDock::hs_refresh() {
	if (!hs_canvas || !hs_atlas_picker) {
		return;
	}
	hs_syncing = true;
	if (hs_atlas_list) {
		hs_atlas_list->clear();
		const std::vector<HewnMesh3D *> lm = hs_meshes();
		const TypedArray<HewnHotspotAtlas> l = lm.empty() ? TypedArray<HewnHotspotAtlas>() : lm[0]->get_hotspot_atlases();
		hs_atlas_index = CLAMP(hs_atlas_index, 0, MAX(0, (int)l.size() - 1));
		for (int k = 0; k < l.size(); k++) {
			const Ref<HewnHotspotAtlas> at = l[k];
			const String nm = at.is_null() ? String("-") : at->get_path().is_empty() || at->get_path().contains("::") ? vformat(T("Atlas %d (in the scene)"), k + 1) : at->get_path().get_file();
			hs_atlas_list->add_item(vformat("%d  %s", k + 1, nm));
		}
		if (!l.is_empty()) {
			hs_atlas_list->select(hs_atlas_index);
		}
		if (hs_panel_atlases) {
			hs_panel_atlases->clear();
			for (int k = 0; k < hs_atlas_list->get_item_count(); k++) {
				hs_panel_atlases->add_item(hs_atlas_list->get_item_text(k));
			}
			hs_panel_atlases->set_visible(!l.is_empty());
			if (!l.is_empty()) {
				hs_panel_atlases->select(hs_atlas_index);
			}
		}
	}
	Ref<HewnHotspotAtlas> a = hs_atlas();
	if (hs_atlas_picker->get_edited_resource() != a) {
		hs_atlas_picker->set_edited_resource(a);
	}
	hs_texture_picker->set_edited_resource(a.is_valid() ? Ref<Resource>(a->get_texture()) : Ref<Resource>());
	hs_atlas_name->set_text(a.is_null() ? String(T(hs_meshes().empty() ? "No atlas: select a Hewn mesh" : "No atlas: make one (+)")) : a->get_path().is_empty() || a->get_path().contains("::") ? String(T("Atlas (in the scene)")) : a->get_path().get_file());
	hs_atlas_name->set_visible(a.is_null() || !hs_panel_atlases || !hs_panel_atlases->is_visible());
	hs_texture_picker->set_editable(a.is_valid());
	if (a.is_null() || hs_cell >= a->get_cell_count()) {
		hs_cell = -1;
	}
	const bool cell = a.is_valid() && hs_cell >= 0;
	hs_cell_label->set_text(cell ? vformat(T("Cell %d of %d"), hs_cell + 1, a->get_cell_count()) : String(T("Pick a cell (in the list or its middle in the image)")));
	if (hs_cell_list) {
		// each cell: on / off (the check box), its number, size, kind, tiling, scale
		static const char *kinds[4] = { "Any", "Upwards", "Floor", "Ceiling" };
		const std::vector<int> keep = hs_cells_picked();
		const int rows = a.is_valid() ? a->get_cell_count() : 0;
		TreeItem *root = hs_cell_list->get_root();
		if (!root || root->get_child_count() != rows) {
			hs_cell_list->clear();
			root = hs_cell_list->create_item();
			for (int c = 0; c < rows; c++) {
				TreeItem *it = hs_cell_list->create_item(root);
				it->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
				it->set_editable(0, true);
				it->set_selectable(0, false);
			}
		}
		const Vector2 img = a.is_valid() ? a->get_image_size() : Vector2(1, 1);
		for (int c = 0; c < rows; c++) {
			const Rect2 r = a->get_cell(c);
			const int t = a->get_cell_tiling(c);
			const float sc = a->get_cell_scale(c);
			TreeItem *it = root->get_child(c);
			it->set_checked(0, a->is_cell_enabled(c));
			it->set_metadata(0, c);
			it->set_text(1, vformat("%d   %d x %d px   %s%s%s", c + 1, (int)std::round(r.size.x * img.x), (int)std::round(r.size.y * img.y), T(kinds[CLAMP(a->get_cell_orientation(c), 0, 3)]),
					t == HewnHotspotAtlas::TILE_U ? String::utf8("  \xE2\x86\x94") : t == HewnHotspotAtlas::TILE_V ? String::utf8("  \xE2\x86\x95") : String(),
					std::fabs(sc - 1.0f) > 1e-4f ? vformat("   x%s", String::num(sc, 2)) : String()));
			if (!a->is_cell_enabled(c)) {
				it->set_custom_color(1, Color(1, 1, 1, 0.4f));
			} else {
				it->clear_custom_color(1);
			}
			if (c == hs_cell || std::find(keep.begin(), keep.end(), c) != keep.end()) {
				it->select(1);
			} else {
				it->deselect(1);
			}
		}
	}
	if (hs_cell_scale) {
		hs_cell_scale->set_editable(cell);
		hs_cell_scale->set_value_no_signal(cell ? a->get_cell_scale(hs_cell) : 1.0);
		hs_cell_side->set_disabled(!cell);
		hs_cell_side->select(cell ? (int)a->get_cell_value(hs_cell, "side", 0) : 0);
		hs_cell_turn->set_disabled(!cell);
		hs_cell_turn->select(cell ? (int)a->get_cell_value(hs_cell, "turn", 0) : 0);
		hs_cell_mirror->set_disabled(!cell);
		hs_cell_mirror->set_pressed_no_signal(cell && (bool)a->get_cell_value(hs_cell, "mirror", false));
		for (SpinBox *sb : { hs_cell_weight, hs_cell_min, hs_cell_max, hs_cell_pad }) {
			sb->set_editable(cell);
		}
		hs_cell_weight->set_value_no_signal(cell ? (double)a->get_cell_value(hs_cell, "weight", 1.0) : 1.0);
		hs_cell_min->set_value_no_signal(cell ? (double)a->get_cell_value(hs_cell, "min_size", 0.0) : 0.0);
		hs_cell_max->set_value_no_signal(cell ? (double)a->get_cell_value(hs_cell, "max_size", 0.0) : 0.0);
		hs_cell_pad->set_value_no_signal(cell ? (double)a->get_cell_value(hs_cell, "padding", 0.0) : 0.0);
	}
	hs_cell_orient->set_disabled(!cell);
	hs_cell_orient->select(cell ? a->get_cell_orientation(hs_cell) : 0);
	// (tiling: only a cell from side to side of the image; else the button's place stays empty)
	const int possible = cell ? a->get_cell_tilings_possible(hs_cell) : 0;
	hs_cell_tile->set_modulate(Color(1, 1, 1, possible ? 1.0f : 0.0f));
	hs_cell_tile->set_disabled(!possible);
	hs_cell_tile->set_mouse_filter(possible ? Control::MOUSE_FILTER_STOP : Control::MOUSE_FILTER_IGNORE);
	hs_cell_tile->set_pressed_no_signal(cell && a->get_cell_tiling(hs_cell) != HewnHotspotAtlas::TILE_NONE);
	const std::vector<HewnMesh3D *> ms = hs_meshes();
	HewnMesh3D *n = ms.empty() ? nullptr : ms[0];
	for (Control *c : { (Control *)hs_auto, (Control *)hs_combine, (Control *)hs_angle, (Control *)hs_weight, (Control *)hs_world }) {
		if (BaseButton *bb = Object::cast_to<BaseButton>(c)) {
			bb->set_disabled(!n);
		} else if (SpinBox *sb = Object::cast_to<SpinBox>(c)) {
			sb->set_editable(n != nullptr);
		}
	}
	if (n) {
		hs_auto->set_pressed_no_signal(n->get_hotspot_auto());
		hs_combine->set_pressed_no_signal(n->get_hotspot_combine());
		hs_angle->set_value_no_signal(n->get_hotspot_angle());
		hs_weight->set_value_no_signal(n->get_hotspot_weight());
		hs_world->set_pressed_no_signal(n->get_hotspot_world_size());
	}
	const bool edit = plugin && plugin->mode != HewnEditorPlugin::MODE_OBJECT && plugin->get_edited();
	hs_edit_box->set_visible(edit);
	if (edit) {
		// fixed: on when every selected face is
		HewnMesh3D *e = plugin->get_edited();
		const std::vector<int> fs = plugin->mode == HewnEditorPlugin::MODE_FACE ? e->selection.faces : hewn::sel::faces_from_verts(e->get_data(), plugin->_selected_verts(), true);
		bool all = !fs.empty();
		for (int f : fs) {
			all = all && e->is_face_hotspot_fixed(f);
		}
		hs_fixed->set_pressed_no_signal(all);
	}
	if (ov_hs_fixed) {
		hs_show_fixed->set_pressed_no_signal(ov_hs_fixed->is_pressed());
	}
	hs_canvas->queue_redraw();
	hs_syncing = false;
}

void HewnDock::_hs_atlas_restore(const Ref<HewnHotspotAtlas> &p_atlas, const Array &p_state) {
	if (p_atlas.is_null() || p_state.size() < 3) {
		return;
	}
	p_atlas->set_cells(p_state[0]);
	p_atlas->set_orientations(p_state[1]);
	p_atlas->set_tilings(p_state[2]);
	if (p_state.size() >= 5) {
		p_atlas->set_enabled_cells(p_state[3]);
		p_atlas->set_scales(p_state[4]);
	}
	if (p_state.size() >= 6) {
		p_atlas->set_extras(p_state[5]);
	}
	hs_refresh();
}

void HewnDock::hs_atlas_edit(const String &p_action, const std::function<void(HewnHotspotAtlas *)> &p_change) {
	Ref<HewnHotspotAtlas> a = hs_atlas();
	if (a.is_null() || !plugin) {
		return;
	}
	const Array before = hs_atlas_state();
	p_change(a.ptr());
	hs_atlas_commit(p_action, before);
}

void HewnDock::hs_cut(bool p_vertical, float p_at, int p_only_cell) {
	hs_atlas_edit("Hotspot Cut", [&](HewnHotspotAtlas *a) { a->cut(p_vertical, p_at, p_only_cell); });
}

void HewnDock::hs_merge(int p_cell, int p_side) {
	Ref<HewnHotspotAtlas> a = hs_atlas();
	if (a.is_null() || a->get_neighbor(p_cell, p_side) < 0) {
		return;
	}
	const int other = a->get_neighbor(p_cell, p_side);
	hs_atlas_edit("Hotspot Join Cells", [&](HewnHotspotAtlas *at) { at->merge_cells(p_cell, other); });
	hs_cell = -1;
	hs_refresh();
}

void HewnDock::hs_pick_cell(int p_cell) {
	hs_cell = p_cell;
	if (hs_cell_list) {
		hs_cell_list->deselect_all(); // (a cell picked in the image: that one only)
	}
	hs_refresh();
}

void HewnDock::_on_hs_reset() {
	hs_atlas_edit("Hotspot Reset Cuts", [](HewnHotspotAtlas *a) { a->reset(); });
	hs_cell = -1;
	hs_refresh();
}

void HewnDock::_on_hs_orient(int p_index) {
	if (hs_syncing || hs_cell < 0) {
		return;
	}
	const int c = hs_cell;
	hs_atlas_edit("Hotspot Cell Orientation", [&](HewnHotspotAtlas *a) { a->set_cell_orientation(c, p_index); });
}

void HewnDock::_on_hs_tile() {
	Ref<HewnHotspotAtlas> a = hs_atlas();
	if (hs_syncing || hs_cell < 0 || a.is_null()) {
		return;
	}
	const int c = hs_cell;
	const int possible = a->get_cell_tilings_possible(c);
	const int now = a->get_cell_tiling(c);
	// off -> along the side it spans (U first when it spans both) -> V when it spans both -> off
	int next = HewnHotspotAtlas::TILE_NONE;
	if (now == HewnHotspotAtlas::TILE_NONE) {
		next = (possible & 1) ? HewnHotspotAtlas::TILE_U : HewnHotspotAtlas::TILE_V;
	} else if (now == HewnHotspotAtlas::TILE_U && (possible & 2)) {
		next = HewnHotspotAtlas::TILE_V;
	}
	hs_atlas_edit("Hotspot Cell Tiling", [&](HewnHotspotAtlas *at) { at->set_cell_tiling(c, next); });
}

// the meshes' lists of atlases changed, undoable
void HewnDock::_hs_set_lists(const String &p_action, const std::function<void(TypedArray<HewnHotspotAtlas> &)> &p_change) {
	const std::vector<HewnMesh3D *> ms = hs_meshes();
	if (!plugin || ms.empty()) {
		return;
	}
	EditorUndoRedoManager *ur = plugin->get_undo_redo();
	ur->create_action(String("Hewn: ") + p_action);
	for (HewnMesh3D *n : ms) {
		TypedArray<HewnHotspotAtlas> l = n->get_hotspot_atlases().duplicate();
		p_change(l);
		ur->add_do_property(n, "hotspot_atlases", l);
		ur->add_undo_property(n, "hotspot_atlases", n->get_hotspot_atlases().duplicate());
	}
	ur->add_do_method(this, "hs_refresh");
	ur->add_undo_method(this, "hs_refresh");
	ur->commit_action();
}

void HewnDock::_on_hs_atlas_changed(const Ref<Resource> &p_res) {
	Ref<HewnHotspotAtlas> a = p_res;
	hs_panel_atlas = a;
	const int at = hs_atlas_index;
	_hs_set_lists("Hotspot Atlas", [&](TypedArray<HewnHotspotAtlas> &l) {
		if (a.is_null()) {
			if (at < l.size()) {
				l.remove_at(at);
			}
		} else if (at < l.size()) {
			l[at] = a;
		} else {
			l.push_back(a);
		}
	});
	hs_cell = -1;
	hs_refresh();
}

void HewnDock::_on_hs_atlas_list(int p_index) {
	hs_atlas_index = p_index;
	hs_cell = -1;
	hs_refresh();
}

void HewnDock::_on_hs_remove_atlas() {
	const int at = hs_atlas_index;
	_hs_set_lists("Hotspot Remove Atlas", [&](TypedArray<HewnHotspotAtlas> &l) {
		if (at < l.size()) {
			l.remove_at(at);
		}
	});
	hs_atlas_index = MAX(0, hs_atlas_index - 1);
	hs_cell = -1;
	hs_refresh();
}

void HewnDock::_on_hs_hold(bool p_hold) {
	const int at = p_hold ? hs_atlas_index : -1;
	const float ppm = hs_ppm();
	_hs_mesh_action(p_hold ? "Hotspot Hold to Atlas" : "Hotspot Any Atlas", [at, ppm](HewnMesh3D *n, const PackedInt32Array &p_faces) {
		n->set_faces_hotspot_atlas(p_faces, at);
		n->randomize_hotspots(p_faces, false, ppm);
	});
}

void HewnDock::_on_hs_texture_changed(const Ref<Resource> &p_res) {
	Ref<HewnHotspotAtlas> a = hs_atlas();
	if (hs_syncing || a.is_null() || !plugin) {
		return;
	}
	EditorUndoRedoManager *ur = plugin->get_undo_redo();
	ur->create_action("Hewn: Hotspot Image");
	ur->add_do_property(a.ptr(), "texture", p_res);
	ur->add_undo_property(a.ptr(), "texture", a->get_texture());
	ur->add_do_method(this, "hs_refresh");
	ur->add_undo_method(this, "hs_refresh");
	ur->commit_action();
}

void HewnDock::_on_hs_new_atlas() {
	Ref<HewnHotspotAtlas> a;
	a.instantiate();
	// (the image of the selection's material, when it has one)
	for (HewnMesh3D *n : hs_meshes()) {
		const Ref<BaseMaterial3D> bm = n->get_material_for_slot(0);
		if (bm.is_valid() && bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO).is_valid()) {
			a->set_texture(bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO));
			a->set_material(bm);
			break;
		}
	}
	int count = 0;
	for (HewnMesh3D *n : hs_meshes()) {
		count = MAX(count, (int)n->get_hotspot_atlases().size());
	}
	_hs_set_lists("Hotspot New Atlas", [&](TypedArray<HewnHotspotAtlas> &l) { l.push_back(a); });
	hs_panel_atlas = a;
	hs_atlas_index = count;
	hs_cell = -1;
	hs_refresh();
}

void HewnDock::_on_hs_setting(double p_unused) {
	_on_hs_toggle(false);
}

void HewnDock::_on_hs_toggle(bool p_unused) {
	if (hs_syncing || !plugin) {
		return;
	}
	const std::vector<HewnMesh3D *> ms = hs_meshes();
	if (ms.empty()) {
		return;
	}
	EditorUndoRedoManager *ur = plugin->get_undo_redo();
	ur->create_action("Hewn: Hotspot Settings", UndoRedo::MERGE_ENDS);
	for (HewnMesh3D *n : ms) {
		ur->add_do_property(n, "hotspot_combine", hs_combine->is_pressed());
		ur->add_undo_property(n, "hotspot_combine", n->get_hotspot_combine());
		ur->add_do_property(n, "hotspot_angle", hs_angle->get_value());
		ur->add_undo_property(n, "hotspot_angle", n->get_hotspot_angle());
		ur->add_do_property(n, "hotspot_weight", hs_weight->get_value());
		ur->add_undo_property(n, "hotspot_weight", n->get_hotspot_weight());
		ur->add_do_property(n, "hotspot_world_size", hs_world->is_pressed());
		ur->add_undo_property(n, "hotspot_world_size", n->get_hotspot_world_size());
		ur->add_do_property(n, "hotspot_ppm", hs_ppm());
		ur->add_undo_property(n, "hotspot_ppm", n->get_hotspot_ppm());
		ur->add_do_property(n, "hotspot_auto", hs_auto->is_pressed());
		ur->add_undo_property(n, "hotspot_auto", n->get_hotspot_auto());
	}
	ur->commit_action();
	// (hotspotting by itself: the new settings at once)
	for (HewnMesh3D *n : ms) {
		if (n->get_hotspot_auto() && n->get_hotspot_atlas().is_valid()) {
			n->randomize_hotspots(PackedInt32Array(), false, hs_ppm());
		}
	}
}

void HewnDock::_hs_mesh_action(const String &p_name, const std::function<void(HewnMesh3D *, const PackedInt32Array &)> &p_do) {
	if (!plugin) {
		return;
	}
	if (plugin->mode != HewnEditorPlugin::MODE_OBJECT && plugin->get_edited()) {
		HewnMesh3D *n = plugin->get_edited();
		const std::vector<int> fs = plugin->mode == HewnEditorPlugin::MODE_FACE ? n->selection.faces : hewn::sel::faces_from_verts(n->get_data(), plugin->_selected_verts(), true);
		if (fs.empty()) {
			return;
		}
		PackedInt32Array pf;
		for (int f : fs) {
			pf.push_back(f);
		}
		const TypedArray<Material> mats = n->get_materials().duplicate();
		plugin->begin_edit(p_name);
		p_do(n, pf);
		plugin->end_edit();
		(void)mats;
	} else {
		EditorUndoRedoManager *ur = plugin->get_undo_redo();
		const std::vector<HewnMesh3D *> ms = plugin->_selected_meshes();
		if (ms.empty()) {
			return;
		}
		ur->create_action(String("Hewn: ") + p_name);
		for (HewnMesh3D *n : ms) {
			const Dictionary before = n->get_state();
			const TypedArray<Material> mats = n->get_materials().duplicate();
			const int seed = n->get_hotspot_seed();
			p_do(n, PackedInt32Array());
			ur->add_do_property(n, "materials", n->get_materials().duplicate());
			ur->add_undo_property(n, "materials", mats);
			ur->add_do_property(n, "hotspot_seed", n->get_hotspot_seed());
			ur->add_undo_property(n, "hotspot_seed", seed);
			ur->add_do_method(n, "set_state", n->get_state());
			ur->add_undo_method(n, "set_state", before);
		}
		ur->commit_action(false);
	}
	hs_refresh();
}

void HewnDock::_on_hs_randomise() {
	if (hs_atlas().is_null()) {
		if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
			t->push_toast(T("Hotspot: no atlas (pick or make one in the Hotspot tab)"), EditorToaster::SEVERITY_WARNING);
		}
		return;
	}
	const float ppm = hs_ppm();
	_hs_mesh_action("Randomise Hotspots", [ppm](HewnMesh3D *n, const PackedInt32Array &p_faces) { n->randomize_hotspots(p_faces, true, ppm); });
}

void HewnDock::_on_hs_choose() {
	if (hs_atlas().is_null() || !hs_palette_popup) {
		return;
	}
	hs_palette_popup->reset_size();
	hs_palette_popup->popup_centered();
	hs_palette->queue_redraw();
}

void HewnDock::_on_hs_palette_pick(int p_cell) {
	hs_palette_popup->hide();
	const float ppm = hs_ppm();
	const int at = hs_atlas_index;
	_hs_mesh_action("Choose Hotspot", [ppm, p_cell, at](HewnMesh3D *n, const PackedInt32Array &p_faces) { n->choose_hotspot(p_faces, p_cell, ppm, at); });
}

void HewnDock::_on_hs_fixed(bool p_on) {
	if (hs_syncing) {
		return;
	}
	_hs_mesh_action(p_on ? "Hotspot Fixed" : "Hotspot Not Fixed", [p_on](HewnMesh3D *n, const PackedInt32Array &p_faces) { n->set_faces_hotspot_fixed(p_faces, p_on); });
}

void HewnDock::_on_hs_show_fixed(bool p_on) {
	if (hs_syncing || !ov_hs_fixed) {
		return;
	}
	ov_hs_fixed->set_pressed(p_on);
	if (p_on && overlays && !overlays->is_pressed()) {
		overlays->set_pressed(true); // (the overlays shown at all: else nothing would show)
	}
	if (plugin) {
		plugin->update_overlays();
	}
}
