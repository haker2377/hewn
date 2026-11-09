// Viewport tools: knife (K), loop cut (Ctrl+R), box cutter (Q) and the poly shape.
#include "hw_editor.h"
#include "hw_textures.h"
#include "hw_editor_util.h"
#include "../hw_cutter.h"

#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/area3d.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>

using namespace godot;
using namespace hw_editor;

namespace {

Vector3 cam_forward(Camera3D *p_cam) {
	return -p_cam->get_global_transform().basis.get_column(2).normalized();
}

void draw_world_line(Control *p_overlay, Camera3D *p_cam, const Vector3 &a, const Vector3 &b, const Color &c, float w = 2.0f) {
	if (p_cam->is_position_behind(a) || p_cam->is_position_behind(b)) {
		return;
	}
	p_overlay->draw_line(p_cam->unproject_position(a), p_cam->unproject_position(b), c, w, true);
}

float signed_area(const std::vector<Vector2> &p) {
	float a = 0.0f;
	for (size_t i = 0; i < p.size(); i++) {
		a += p[i].cross(p[(i + 1) % p.size()]);
	}
	return a * 0.5f;
}

const char *CUTTER_OP_NAMES[5] = { "Cut", "Slice", "Join", "Intersect", "Make" };
const char *CUTTER_SHAPE_NAMES[3] = { "Box", "Circle", "Polygon" };

} // namespace

// ------------------------------------------------------------------ common

void HewnEditorPlugin::cancel_tool() {
	ruler_line = false;
	ruler_rope = false;
	switch (modal) {
		case MODAL_MOVE:
		case MODAL_ROTATE:
		case MODAL_SCALE:
		case MODAL_BEVEL:
		case MODAL_INSET:
		case MODAL_SNAP:
		case MODAL_SLIDE:
		case MODAL_EXTRUDE_AMOUNT:
		case MODAL_VALUE:
			_cancel_modal();
			return;
		case MODAL_UVT:
			_uvt_finish(false);
			return;
		case MODAL_TSNAP:
			_tsnap_finish(false);
			return;
		case MODAL_UV:
			if (HewnMesh3D *n = get_edited()) {
				n->get_data() = uvm_backup;
				n->rebuild();
			}
			uvm_backup = hewn::Mesh();
			modal = MODAL_NONE;
			return;
		default:
			break;
	}
	knife_points.clear();
	knife_done.clear();
	knife_hover_valid = false;
	loop_edge = hewn::Edge(-1, -1);
	loop_phase = 0;
	if (modal == MODAL_RULER && ruler_state == 1 && !rulers.empty()) {
		rulers.pop_back(); // its end was never placed
	}
	ruler_state = 0;
	ruler_drag = ruler_drag_pt = -1;
	shape_cut = 0;
	poly_cut = false;
	clip_state = 0;
	if (modal == MODAL_PREFAB) {
		_remove_prefab_ghost();
	}
	cutter_state = 0;
	cutter_poly.clear();
	if (modal == MODAL_POLY && poly_edit_id) {
		HewnMesh3D *pe = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(poly_edit_id));
		if (pe) {
			if (poly_edit_sweep) {
				pe->set_path_gaps(poly_edit_gaps_orig);
			}
			pe->set_poly_points(poly_edit_orig); // cancelled: the base as it was
			pe->set_transform(poly_edit_xf);
		}
	}
	poly_edit_id = 0;
	poly_edit_wall = false;
	poly_edit_sweep = false;
	poly_points.clear();
	poly_phase = 0;
	poly_drag_point = -1;
	shape_state = 0;
	wall_pts.clear();
	modal = MODAL_NONE;
	_remove_shape_preview();
	update_overlays();
	if (dock) {
		dock->refresh();
	}
}

int32_t HewnEditorPlugin::_tool_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	if (modal == MODAL_CUTTER) {
		cutter_op = dock->cutter_op->get_selected(); // changed in the dock while the tool runs
	}
	HewnMesh3D *n = get_edited();
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	const Key code = k.is_valid() && k->is_pressed() && !k->is_echo() ? key_code(k) : KEY_NONE;
	const bool ctrl_held = (k.is_valid() && ctrl_after(k)) || (mm.is_valid() && (mm->is_ctrl_pressed() || mm->is_meta_pressed())) ||
			(mb.is_valid() && (mb->is_ctrl_pressed() || mb->is_meta_pressed()));
	const bool snap = dock->snap_enabled->is_pressed() != ctrl_held;
	tool_ctrl = ctrl_held;
	if (modal == MODAL_SPIN) {
		return _spin_input(p_cam, p_event);
	}
	if (modal == MODAL_FACECUT) {
		return _facecut_input(p_cam, p_event);
	}
	if (code == KEY_ESCAPE) {
		if (modal == MODAL_CUTTER && cutter_state != 0) {
			cutter_state = 0;
			cutter_poly.clear();
			update_overlays();
		} else {
			cancel_tool();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (modal == MODAL_SHAPE) {
		return _shape_input(p_cam, p_event);
	}
	if (modal == MODAL_PAINT) {
		return _paint_input(p_cam, p_event);
	}
	if (modal == MODAL_BISECT) {
		return _bisect_input(p_cam, p_event);
	}
	if (modal == MODAL_CLIP) {
		return _clip_input(p_cam, p_event);
	}
	if (modal == MODAL_PREFAB) {
		return _prefab_input(p_cam, p_event);
	}

	// ---------------------------------------------------------------- 3D cursor tool
	if (modal == MODAL_CURSOR && cursor_xf != 0) {
		Ref<InputEventKey> ck = p_event;
		const bool ctrl = (ck.is_valid() && (ck->is_ctrl_pressed() || ck->is_meta_pressed())) || (mm.is_valid() && (mm->is_ctrl_pressed() || mm->is_meta_pressed()));
		auto done = [&](bool p_keep) {
			if (!p_keep) {
				cursor_pos = cursor_xf_pos0;
				cursor_basis = cursor_xf_basis0;
			}
			cursor_xf = 0;
			modal = MODAL_NONE;
			update_overlays();
		};
		if (mm.is_valid()) {
			if (cursor_xf == 2 || cursor_xf_axis >= 0) {
				mouse_pos = _wrap_mouse(p_cam, mm->get_position()); // (at the viewport's edge: on the other side, like G)
			}
			_cursor_xf_update(p_cam, ctrl);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && mb->is_pressed()) {
			done(mb->get_button_index() == MOUSE_BUTTON_LEFT);
			return AFTER_GUI_INPUT_STOP;
		}
		if (ck.is_valid() && ck->is_pressed() && !ck->is_echo()) {
			const Key code = key_code(ck);
			if (code == KEY_ESCAPE || code == KEY_ENTER || code == KEY_KP_ENTER) {
				done(code != KEY_ESCAPE);
			} else if (code == KEY_X || code == KEY_Y || code == KEY_Z) {
				const int a = code == KEY_X ? 0 : code == KEY_Y ? 1 : 2;
				cursor_xf_axis = cursor_xf_axis == a ? -1 : a;
				cursor_pos = cursor_xf_pos0;
				cursor_basis = cursor_xf_basis0;
				cursor_xf_t0 = cursor_xf_axis >= 0 ? _axis_param(p_cam, mouse_pos, cursor_xf_pos0, AXES[cursor_xf_axis]) : 0.0f;
				_cursor_xf_update(p_cam, ctrl);
			} else if (code == KEY_CTRL || code == KEY_META) {
				_cursor_xf_update(p_cam, true);
			}
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_STOP;
	}
	if (modal == MODAL_CURSOR) {
		if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
			if (mb->is_pressed()) {
				_place_cursor(p_cam, mb->get_position(), mb->is_ctrl_pressed() || mb->is_meta_pressed());
			} else if (cursor_once) {
				cursor_once = false;
				modal = MODAL_NONE; // (Shift+X: one placement)
				update_overlays();
			}
			return AFTER_GUI_INPUT_STOP;
		}
		if (mm.is_valid() && (mm->get_button_mask() & MOUSE_BUTTON_MASK_LEFT)) {
			_place_cursor(p_cam, mouse_pos, mm->is_ctrl_pressed() || mm->is_meta_pressed()); // drag: the cursor follows
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT) {
			cancel_tool();
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}

	// ---------------------------------------------------------------- ruler (works without an edited mesh)
	if (modal == MODAL_RULER) {
		auto hover_point = [&](bool p_free) {
			// Snaps to vertices, edges and surfaces; Ctrl measures freely on the surface only.
			if (p_free) {
				Vector3 o, d, nrm;
				_ray(p_cam, mouse_pos, o, d);
				ruler_hover_valid = _raycast_scene(o, d, ruler_hover, nrm);
				ruler_hover_kind = SNAP_KIND_FACE;
			} else {
				ruler_hover_valid = _magnet_target(p_cam, mouse_pos, ruler_hover);
				ruler_hover_kind = last_snap_kind;
			}
			if (!ruler_hover_valid && (ruler_drag >= 0 || ruler_state == 1)) {
				ruler_hover_kind = SNAP_KIND_GRID;
				// off the geometry: on the view plane through the point being moved
				const RulerItem &r = rulers[ruler_drag >= 0 ? ruler_drag : rulers.size() - 1];
				const Vector3 ref = r.p[ruler_drag >= 0 ? ruler_drag_pt : r.p.size() - 1];
				ruler_hover_valid = _plane_hit(p_cam, mouse_pos, ref, cam_forward(p_cam), ruler_hover);
			}
		};
		if ((code == KEY_X || code == KEY_DELETE) && ruler_drag < 0) {
			int item, pt, seg;
			_ruler_hit(p_cam, mouse_pos, item, pt, seg);
			if (item >= 0) {
				rulers.erase(rulers.begin() + item);
				ruler_hover_item = ruler_hover_pt = -1;
				if (ruler_state == 1 && item == (int)rulers.size()) {
					ruler_state = 0;
				}
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mm.is_valid()) {
			hover_point(mm->is_ctrl_pressed() || mm->is_meta_pressed());
			if (ruler_drag >= 0 && ruler_hover_valid) {
				rulers[ruler_drag].p[ruler_drag_pt] = ruler_hover;
				ruler_moved = ruler_moved || mouse_pos.distance_to(ruler_press) > 4.0f;
			} else if (ruler_state == 1 && ruler_hover_valid && !rulers.empty()) {
				rulers.back().p.back() = ruler_hover;
			}
			int seg;
			_ruler_hit(p_cam, mouse_pos, ruler_hover_item, ruler_hover_pt, seg);
			update_overlays();
			return ruler_drag >= 0 ? AFTER_GUI_INPUT_STOP : AFTER_GUI_INPUT_PASS;
		}
		if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
			if (!mb->is_pressed()) {
				if (ruler_drag >= 0 && ruler_drag == (int)rulers.size() - 1 && ruler_drag_pt == 1 && rulers[ruler_drag].p.size() == 2 &&
						rulers[ruler_drag].p[0].distance_to(rulers[ruler_drag].p[1]) < 1e-6f && !ruler_moved) {
					ruler_state = 1; // a click without dragging: the next click puts the end
				} else if (ruler_line && ruler_drag >= 0 && ruler_drag == (int)rulers.size() - 1 && rulers.back().p.size() == 2 && ruler_moved) {
					ruler_drag = ruler_drag_pt = -1;
					_ruler_make_line(); // (dragged from one point to the other)
					return AFTER_GUI_INPUT_STOP;
				}
				ruler_drag = ruler_drag_pt = -1;
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			ruler_press = mb->get_position();
			ruler_moved = false;
			hover_point(mb->is_ctrl_pressed() || mb->is_meta_pressed());
			if (ruler_state == 1) {
				if (ruler_hover_valid && !rulers.empty()) {
					rulers.back().p.back() = ruler_hover;
				}
				ruler_state = 0;
				if (ruler_line && !rulers.empty()) {
					_ruler_make_line();
					return AFTER_GUI_INPUT_STOP;
				}
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			int item, pt, seg;
			_ruler_hit(p_cam, mb->get_position(), item, pt, seg);
			if (item >= 0 && pt >= 0) {
				ruler_drag = item; // move an end (or the angle's vertex)
				ruler_drag_pt = pt;
			} else if (item >= 0 && rulers[item].p.size() == 2) {
				// dragging the middle of a ruler makes a protractor, the angle at the new point
				RulerItem &r = rulers[item];
				const Vector2 sa = p_cam->unproject_position(r.p[0]), sb = p_cam->unproject_position(r.p[1]);
				float t;
				seg_dist(mb->get_position(), sa, sb, &t);
				r.p.insert(r.p.begin() + 1, r.p[0].lerp(r.p[1], t));
				ruler_drag = item;
				ruler_drag_pt = 1;
			} else if (ruler_hover_valid) {
				rulers.push_back({ { ruler_hover, ruler_hover } });
				ruler_drag = (int)rulers.size() - 1;
				ruler_drag_pt = 1;
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT) {
			cancel_tool();
			return AFTER_GUI_INPUT_STOP;
		}
		return code != KEY_NONE ? AFTER_GUI_INPUT_STOP : AFTER_GUI_INPUT_PASS;
	}

	if (modal != MODAL_POLY && !n) {
		cancel_tool();
		return AFTER_GUI_INPUT_PASS;
	}

	// ---------------------------------------------------------------- knife
	if (modal == MODAL_KNIFE) {
		if (code == KEY_A && !k->is_ctrl_pressed() && !k->is_alt_pressed() && !k->is_shift_pressed()) {
			angle_lock = (angle_lock + 1) % 3; // off -> screen -> surface -> off
			knife_hover_valid = _knife_pick_constrained(p_cam, mouse_pos, false, knife_hover);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mm.is_valid()) {
			knife_hover_valid = _knife_pick_constrained(p_cam, mouse_pos, mm->is_ctrl_pressed() || mm->is_meta_pressed(), knife_hover);
			update_overlays();
			// (a drag with the middle / right button moves the view: Godot gets it)
			return (mm->get_button_mask() & (MOUSE_BUTTON_MASK_MIDDLE | MOUSE_BUTTON_MASK_RIGHT)) ? AFTER_GUI_INPUT_PASS : AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && mb->is_pressed()) {
			if (mb->get_button_index() == MOUSE_BUTTON_LEFT) {
				KnifeClick c;
				if (_knife_pick_constrained(p_cam, mb->get_position(), mb->is_ctrl_pressed() || mb->is_meta_pressed(), c)) {
					knife_points.push_back(c);
				}
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			if (mb->get_button_index() == MOUSE_BUTTON_RIGHT) {
				// RMB ends the cut being drawn (it stays, a click starts another one; Enter cuts them all);
				// with no cut at all it leaves the knife
				if (knife_points.empty() && knife_done.empty()) {
					cancel_tool();
				} else {
					if (knife_points.size() >= 2) {
						knife_done.push_back(knife_points);
					}
					knife_points.clear();
					update_overlays();
				}
				return AFTER_GUI_INPUT_STOP;
			}
			return AFTER_GUI_INPUT_PASS;
		}
		if (code == KEY_ENTER || code == KEY_KP_ENTER || code == KEY_SPACE) {
			_apply_knife(p_cam);
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_BACKSPACE && (!knife_points.empty() || !knife_done.empty())) {
			if (knife_points.empty()) {
				knife_points = knife_done.back(); // (the cut ended last: drawn on again)
				knife_done.pop_back();
			}
			knife_points.pop_back();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return k.is_valid() && code != KEY_NONE ? AFTER_GUI_INPUT_STOP : AFTER_GUI_INPUT_PASS;
	}

	// ---------------------------------------------------------------- loop cut
	if (modal == MODAL_LOOPCUT) {
		if (mm.is_valid()) {
			int v, f;
			hewn::Edge e = loop_edge;
			if (loop_phase == 0) {
				_pick_mode(p_cam, mouse_pos, MODE_EDGE, v, e, f);
				if (e.a < 0) {
					// Over a face: its edge closest to the cursor.
					_pick_mode(p_cam, mouse_pos, MODE_FACE, v, e, f);
					if (f >= 0) {
						const hewn::Face &F = n->get_data().faces[f];
						const Transform3D g = n->get_global_transform();
						float best = 1e30f;
						for (int i = 0; i < F.size(); i++) {
							const int a = F.v[i], b = F.v[(i + 1) % F.size()];
							const float dd = seg_dist(mouse_pos, p_cam->unproject_position(g.xform(n->get_data().pos[a])), p_cam->unproject_position(g.xform(n->get_data().pos[b])));
							if (dd < best) {
								best = dd;
								e = hewn::Edge(a, b);
							}
						}
					}
				}
				loop_edge = e;
				loop_t = 0.5f;
			}
			if (e.a >= 0 && loop_phase == 1) {
				const Transform3D g = n->get_global_transform();
				const Vector3 a = g.xform(n->get_data().pos[e.a]), b = g.xform(n->get_data().pos[e.b]);
				Vector3 o, d;
				_ray(p_cam, mouse_pos, o, d);
				loop_t = CLAMP(line_ray_param(a, b - a, o, d), 0.02f, 0.98f);
				if (mm->is_ctrl_pressed() || mm->is_meta_pressed()) {
					loop_t = 0.5f;
				}
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP || mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN)) {
			// The wheel sets the number of cuts and never zooms while the tool is active.
			if (mb->is_pressed()) {
				loop_cuts = CLAMP(loop_cuts + (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? 1 : -1), 1, 64);
				update_overlays();
			}
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && mb->is_pressed()) {
			const MouseButton bi = mb->get_button_index();
			if (bi == MOUSE_BUTTON_LEFT) {
				// the first click fixes the ring, then the cut slides with the mouse.
				if (loop_phase == 0 && loop_edge.a >= 0) {
					loop_phase = 1;
				} else if (loop_phase == 1) {
					_apply_loop_cut();
				}
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			if (bi == MOUSE_BUTTON_RIGHT) {
				if (loop_phase == 1) {
					loop_t = 0.5f; // right click: keep the cut centered
					_apply_loop_cut();
				} else {
					cancel_tool();
				}
				return AFTER_GUI_INPUT_STOP;
			}
			if (bi == MOUSE_BUTTON_WHEEL_UP || bi == MOUSE_BUTTON_WHEEL_DOWN) {
				loop_cuts = CLAMP(loop_cuts + (bi == MOUSE_BUTTON_WHEEL_UP ? 1 : -1), 1, 64);
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			return AFTER_GUI_INPUT_PASS;
		}
		if (code == KEY_EQUAL || code == KEY_KP_ADD || code == KEY_MINUS || code == KEY_KP_SUBTRACT) {
			loop_cuts = CLAMP(loop_cuts + ((code == KEY_EQUAL || code == KEY_KP_ADD) ? 1 : -1), 1, 64);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return k.is_valid() && code != KEY_NONE ? AFTER_GUI_INPUT_STOP : AFTER_GUI_INPUT_PASS;
	}

	// ---------------------------------------------------------------- box cutter
	if (modal == MODAL_CUTTER) {
		if (code == KEY_X || code == KEY_K || code == KEY_J || code == KEY_I || code == KEY_M) {
			cutter_op = code == KEY_X ? CUTTER_CUT : code == KEY_K ? CUTTER_SLICE : code == KEY_J ? CUTTER_JOIN : code == KEY_I ? CUTTER_INTERSECT : CUTTER_MAKE;
			dock->cutter_op->select(cutter_op);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_Q && cutter_state == 0) {
			cancel_tool();
			return AFTER_GUI_INPUT_STOP;
		}
		const int shape = dock->cutter_shape->get_selected();
		const bool needs_depth = !dock->cutter_through->is_pressed() || cutter_op == CUTTER_JOIN || cutter_op == CUTTER_MAKE;
		auto finish_outline = [&]() {
			if (needs_depth) {
				cutter_state = 3;
				cutter_t0 = _axis_param(p_cam, mouse_pos, cutter_origin, cutter_n);
				cutter_depth = 0.0f;
			} else {
				_apply_cutter();
			}
			update_overlays();
		};
		if (mm.is_valid()) {
			if (cutter_state == 1) {
				Vector2 uv;
				if (_cutter_plane_point(p_cam, mouse_pos, uv)) {
					cutter_b = uv;
				}
				cutter_square = mm->is_shift_pressed();
				cutter_centered = mm->is_alt_pressed();
			} else if (cutter_state == 3) {
				cutter_depth = std::fabs(_axis_param(p_cam, mouse_pos, cutter_origin, cutter_n) - cutter_t0);
				const float sm = (float)dock->snap_move->get_value();
				if (snap && sm > 0.0f) {
					cutter_depth = std::round(cutter_depth / sm) * sm;
				}
			}
			update_overlays();
			return cutter_state == 0 ? AFTER_GUI_INPUT_PASS : AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
			if (mb->is_pressed()) {
				if (cutter_state == 0) {
					if (_cutter_begin(p_cam, mb->get_position())) {
						Vector2 uv;
						_cutter_plane_point(p_cam, mb->get_position(), uv);
						if (shape == CUTTER_NGON) {
							cutter_poly = { uv };
							cutter_state = 2;
						} else {
							cutter_a = cutter_b = uv;
							cutter_square = mb->is_shift_pressed();
							cutter_centered = mb->is_alt_pressed();
							cutter_state = 1;
						}
					}
				} else if (cutter_state == 2) {
					Vector2 uv;
					if (_cutter_plane_point(p_cam, mb->get_position(), uv)) {
						const Vector3 first = cutter_origin + cutter_u * cutter_poly[0].x + cutter_v * cutter_poly[0].y;
						if (cutter_poly.size() >= 3 && p_cam->unproject_position(first).distance_to(mb->get_position()) < 10.0f) {
							finish_outline();
						} else {
							cutter_poly.push_back(uv);
						}
					}
				} else if (cutter_state == 3) {
					_apply_cutter();
				}
			} else if (cutter_state == 1) {
				const std::vector<Vector2> o = _cutter_outline();
				if (o.size() >= 3 && std::fabs(signed_area(o)) > 1e-8f) {
					finish_outline();
				} else {
					cutter_state = 0;
				}
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT && cutter_state != 0) {
			cutter_state = 0;
			cutter_poly.clear();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (cutter_state == 2 && (code == KEY_ENTER || code == KEY_KP_ENTER) && cutter_poly.size() >= 3) {
			finish_outline();
			return AFTER_GUI_INPUT_STOP;
		}
		if (cutter_state == 2 && code == KEY_BACKSPACE) {
			cutter_poly.pop_back();
			if (cutter_poly.empty()) {
				cutter_state = 0;
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (cutter_state == 3 && (code == KEY_ENTER || code == KEY_KP_ENTER)) {
			_apply_cutter();
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}

	// ---------------------------------------------------------------- poly shape
	if (modal == MODAL_POLY && poly_edit_id != 0) {
		// Editing the base of an existing Poly Shape: drag a point to move it, click it to remove it,
		// click an outline edge to add a point there; click outside to apply, RMB / Esc cancel.
		const int count = (int)poly_points.size();
		if (mm.is_valid()) {
			if (poly_drag_point >= 0 && !poly_dragging && mouse_pos.distance_to(poly_press_pos) > 4.0f) {
				poly_dragging = true;
			}
			if (poly_drag_point >= 0 && poly_dragging) {
				Vector3 p;
				if (_poly_lifted_point(p_cam, mouse_pos, snap, poly_drag_point, p)) {
					poly_points[poly_drag_point] = p;
					_poly_apply_live();
				}
			}
			update_overlays();
			return poly_drag_point >= 0 ? AFTER_GUI_INPUT_STOP : AFTER_GUI_INPUT_PASS;
		}
		if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
			const Vector2 pos = mb->get_position();
			if (!mb->is_pressed()) {
				if (poly_drag_point >= 0 && !poly_dragging && count > (poly_edit_wall ? 2 : 3)) {
					poly_points.erase(poly_points.begin() + poly_drag_point);
					if (poly_drag_point < (int)poly_gaps.size()) {
						poly_gaps.erase(poly_gaps.begin() + poly_drag_point); // (its two segments one: the one before's)
					}
					_poly_apply_live();
				}
				poly_drag_point = -1;
				poly_dragging = false;
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			for (int i = 0; i < count; i++) {
				if (!p_cam->is_position_behind(poly_points[i]) && p_cam->unproject_position(poly_points[i]).distance_to(pos) < 10.0f) {
					poly_drag_point = i;
					poly_dragging = false;
					poly_press_pos = pos;
					return AFTER_GUI_INPUT_STOP;
				}
			}
			for (int i = 0; i < count - (poly_edit_wall && !poly_edit_loop ? 1 : 0); i++) { // a wall is an open path
				const Vector3 a = poly_points[i], b = poly_points[(i + 1) % count];
				if (p_cam->is_position_behind(a) || p_cam->is_position_behind(b)) {
					continue;
				}
				if (seg_dist(pos, p_cam->unproject_position(a), p_cam->unproject_position(b)) < 8.0f) {
					if (poly_edit_sweep && mb->is_alt_pressed()) {
						// Alt+click: the segment left out of the sweep (or back in)
						poly_gaps.resize(count, 0);
						poly_gaps[i] = poly_gaps[i] ? 0 : 1;
						_poly_apply_live();
						update_overlays();
						return AFTER_GUI_INPUT_STOP;
					}
					if (poly_edit_sweep) {
						poly_gaps.resize(count, 0);
						poly_gaps.insert(poly_gaps.begin() + i + 1, poly_gaps[i]); // (both halves as it was)
					}
					Vector3 p;
					if (_poly_plane_point(p_cam, pos, snap, p)) {
						// (a new corner of a path: between its neighbors' heights)
						const Vector3 nn = poly_n.normalized();
						const float ha = (a - poly_origin).dot(nn), hb = (b - poly_origin).dot(nn);
						p += nn * ((ha + hb) * 0.5f - (p - poly_origin).dot(nn));
						poly_points.insert(poly_points.begin() + i + 1, p);
						_poly_apply_live();
						poly_drag_point = i + 1;
						poly_dragging = true; // a new point follows the mouse until released
						poly_press_pos = pos;
					}
					update_overlays();
					return AFTER_GUI_INPUT_STOP;
				}
			}
			_finish_poly(); // click outside: apply and leave
			return AFTER_GUI_INPUT_STOP;
		}
		if ((mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT)) {
			cancel_tool();
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_ENTER || code == KEY_KP_ENTER) {
			_finish_poly();
			return AFTER_GUI_INPUT_STOP;
		}
		return k.is_valid() && code != KEY_NONE ? AFTER_GUI_INPUT_STOP : AFTER_GUI_INPUT_PASS;
	}
	if (modal == MODAL_POLY) {
		// angle constraint (A) from the last point: on the screen, then in the outline plane
		auto constrained = [&](const Vector2 &p_pos) {
			if (angle_lock == 0 || poly_points.empty() || poly_phase != 0) {
				return p_pos;
			}
			const Vector3 ref = poly_points.size() >= 2 ? poly_points[poly_points.size() - 1] - poly_points[poly_points.size() - 2] : Vector3(1, 0, 0);
			return _angle_constrain(p_cam, p_pos, poly_points.back(), poly_n, ref);
		};
		const bool poly_snap = snap && angle_lock == 0; // the grid would break the angle
		if (code == KEY_A && poly_phase == 0 && !k->is_ctrl_pressed() && !k->is_alt_pressed() && !k->is_shift_pressed()) {
			angle_lock = (angle_lock + 1) % 3;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (poly_phase == 0 && mm.is_valid() && poly_drag_point < 0) {
			// marker: where a click puts the next point
			Vector3 p;
			// (before the first point: on the surface under the cursor, like that point)
			const bool ok = _poly_plane_point(p_cam, constrained(mouse_pos), poly_snap, p);
			snap_mark_valid = (poly_snap || angle_lock != 0) && ok;
			snap_mark = p;
			snap_mark_geom = last_snap_geom;
			snap_mark_kind = last_snap_kind;
			poly_cursor_valid = ok;
			poly_cursor = p;
		} else if (poly_phase == 0 && poly_drag_point >= 0) {
			poly_cursor_valid = false;
		}
		if (poly_phase == 0) {
			if (mm.is_valid()) {
				if (poly_drag_point >= 0) {
					Vector3 p;
					if (_poly_plane_point(p_cam, mouse_pos, snap, p)) {
						poly_points[poly_drag_point] = p;
					}
				}
				update_overlays();
				return AFTER_GUI_INPUT_PASS;
			}
			if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
				if (!mb->is_pressed()) {
					poly_drag_point = -1;
					return AFTER_GUI_INPUT_STOP;
				}
				const Vector2 pos = mb->get_position();
				for (int i = 0; i < (int)poly_points.size(); i++) {
					if (p_cam->is_position_behind(poly_points[i]) || p_cam->unproject_position(poly_points[i]).distance_to(pos) >= 10.0f) {
						continue;
					}
					if (i == 0 && poly_points.size() >= 3 && poly_edit_id == 0) {
						// the height along the edge at the clicked point: its top follows the mouse
						poly_phase = 1;
						poly_hbase = poly_points[0];
						poly_t0 = 0.0f;
						poly_height_value = _axis_param(p_cam, pos, poly_hbase, poly_n);
					} else {
						poly_drag_point = i;
					}
					update_overlays();
					return AFTER_GUI_INPUT_STOP;
				}
				Vector3 p;
				if (_poly_plane_point(p_cam, constrained(pos), poly_snap, p)) {
					poly_points.push_back(p);
					poly_has_plane = true;
				}
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT) {
				if (poly_points.empty()) {
					cancel_tool();
				} else {
					poly_points.pop_back();
					if (poly_points.empty()) {
						poly_has_plane = false;
					}
				}
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			if ((code == KEY_ENTER || code == KEY_KP_ENTER) && poly_points.size() >= 3) {
				poly_phase = 1;
				poly_hbase = poly_points.back(); // (the last point)
				poly_t0 = 0.0f;
				poly_height_value = _axis_param(p_cam, mouse_pos, poly_hbase, poly_n);
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			if (code == KEY_BACKSPACE && !poly_points.empty()) {
				poly_points.pop_back();
				update_overlays();
				return AFTER_GUI_INPUT_STOP;
			}
			return AFTER_GUI_INPUT_PASS;
		}
		// height
		if (mm.is_valid()) {
			poly_height_value = _axis_param(p_cam, mouse_pos, poly_hbase, poly_n) - poly_t0;
			_snap_height(p_cam, poly_hbase, poly_n.normalized(), poly_height_value);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if ((mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_LEFT) || code == KEY_ENTER || code == KEY_KP_ENTER) {
			_finish_poly();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT) {
			poly_phase = 0;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}
	return AFTER_GUI_INPUT_PASS;
}

// ------------------------------------------------------------------ angle constraint, snap marker

// knife / poly angle constraint (A): the new segment from `p_last` turns in 30 degree steps,
// measured on the screen (mode 1) or in the plane `p_n` from the direction `p_ref` (mode 2).
// Returns the screen position to use instead of the mouse.
Vector2 HewnEditorPlugin::_angle_constrain(Camera3D *p_cam, const Vector2 &p_pos, const Vector3 &p_last, const Vector3 &p_n, const Vector3 &p_ref) const {
	const float step = (float)Math::PI / 6.0f;
	if (angle_lock == 0 || p_cam->is_position_behind(p_last)) {
		return p_pos;
	}
	if (angle_lock == 1 || p_n.length() < 1e-6f) {
		const Vector2 ls = p_cam->unproject_position(p_last);
		const Vector2 d = p_pos - ls;
		if (d.length() < 1.0f) {
			return p_pos;
		}
		const float a = std::round(std::atan2(d.y, d.x) / step) * step;
		const Vector2 dir(std::cos(a), std::sin(a));
		return ls + dir * d.dot(dir);
	}
	const Vector3 n = p_n.normalized();
	Vector3 hit;
	if (!_plane_hit(p_cam, p_pos, p_last, n, hit)) {
		return p_pos;
	}
	Vector3 u = p_ref - n * p_ref.dot(n);
	if (u.length() < 1e-6f) {
		Vector3 tmp;
		hewn::plane_basis(n, u, tmp);
	}
	u.normalize();
	const Vector3 w = n.cross(u);
	const Vector3 r = hit - p_last;
	const float a = std::round(std::atan2(r.dot(w), r.dot(u)) / step) * step;
	const Vector3 dir = u * std::cos(a) + w * std::sin(a);
	const Vector3 q = p_last + dir * r.dot(dir);
	return p_cam->is_position_behind(q) ? p_pos : p_cam->unproject_position(q);
}

String HewnEditorPlugin::_angle_lock_text() const {
	const String deg = String("30") + String::chr(0x00B0);
	return angle_lock == 1 ? T("  |  angle: screen, ") + deg + " (A)" : angle_lock == 2 ? T("  |  angle: surface, ") + deg + " (A)" : String(T("  |  A: angle constraint"));
}

// Height of a drawn shape with snapping: to a vertex / edge height, or in grid steps.
bool HewnEditorPlugin::_snap_height(Camera3D *p_cam, const Vector3 &p_base, const Vector3 &p_n, float &r_h) {
	snap_mark_valid = false;
	if (!snapping_active(tool_ctrl)) {
		return false;
	}
	Vector3 sp;
	// a point only when the height it gives is near the one under the mouse: a floor or an edge far
	// behind the box (the cursor below the horizon) would pull the top down to their height
	const Vector3 raw = p_base + p_n * r_h;
	if ((dock->snap_targets & HewnDock::SNAP_POINTS) && _snap_target(p_cam, mouse_pos, sp) && !p_cam->is_position_behind(raw) &&
			!p_cam->is_position_behind(p_base + p_n * (sp - p_base).dot(p_n)) &&
			p_cam->unproject_position(p_base + p_n * (sp - p_base).dot(p_n)).distance_to(p_cam->unproject_position(raw)) < 24.0f) {
		r_h = (sp - p_base).dot(p_n);
		snap_mark = sp;
		snap_mark_valid = true;
		snap_mark_geom = true;
		snap_mark_kind = last_snap_kind;
		return true;
	}
	const float sm = (float)dock->snap_move->get_value();
	if ((dock->snap_targets & HewnDock::SNAP_STEPS) && sm > 0.0f) {
		r_h = std::round(r_h / sm) * sm;
		snap_mark = p_base + p_n * r_h;
		snap_mark_valid = true;
		snap_mark_geom = false;
		return true;
	}
	return false;
}

void HewnEditorPlugin::_draw_snap_mark(Control *p_overlay, Camera3D *p_cam) {
	if (!snap_mark_valid || !snapping_active(tool_ctrl) || p_cam->is_position_behind(snap_mark)) {
		return;
	}
	const Vector2 c = p_cam->unproject_position(snap_mark);
	// the snapped point: its symbol tells what it is on (brighter on geometry than on the grid)
	const Color col = snap_mark_geom ? Color(1.0f, 0.85f, 0.2f) : Color(1, 1, 1, 0.8f);
	draw_snap_symbol(p_overlay, c, snap_mark_geom ? snap_mark_kind : SNAP_KIND_GRID, col);
}

// ------------------------------------------------------------------ knife

void HewnEditorPlugin::start_knife() {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	if (mode == MODE_OBJECT) {
		set_mode(element_mode);
	}
	knife_points.clear();
	knife_done.clear();
	knife_hover_valid = false;
	angle_lock = 0;
	modal = MODAL_KNIFE;
	update_overlays();
}

bool HewnEditorPlugin::_knife_pick(Camera3D *p_cam, const Vector2 &p_pos, bool p_midpoint, KnifeClick &r_click) const {
	HewnMesh3D *n = get_edited();
	if (!n || !p_cam) {
		return false;
	}
	const hewn::Mesh &m = n->get_data();
	const Transform3D g = n->get_global_transform();
	// the knife's own points first, then vertices, then edges
	std::vector<KnifeClick> own = knife_points;
	for (const std::vector<KnifeClick> &d : knife_done) {
		own.insert(own.end(), d.begin(), d.end());
	}
	for (const KnifeClick &c : own) {
		const Vector3 w = g.xform(c.local);
		if (!p_cam->is_position_behind(w) && p_cam->unproject_position(w).distance_to(p_pos) < 10.0f) {
			r_click = c;
			return true;
		}
	}
	hewn::Topology t;
	t.build(m);
	int v, f;
	hewn::Edge e;
	_pick_mode(p_cam, p_pos, MODE_VERTEX, v, e, f);
	if (v >= 0) {
		// (a vertex of wires only: a point "in space" exactly on it)
		r_click.face = t.vert_faces[v].empty() ? -1 : t.vert_faces[v][0];
		r_click.local = m.pos[v];
		return true;
	}
	// the knife's own lines drawn so far (not cut yet): the point pulled onto them too
	{
		float best = 8.0f;
		bool found = false;
		KnifeClick on;
		Vector3 o, d;
		_ray(p_cam, p_pos, o, d);
		auto try_line = [&](const std::vector<KnifeClick> &l) {
			for (size_t i = 0; i + 1 < l.size(); i++) {
				const Vector3 a = g.xform(l[i].local), b = g.xform(l[i + 1].local);
				if (p_cam->is_position_behind(a) || p_cam->is_position_behind(b)) {
					continue;
				}
				const Vector2 sa = p_cam->unproject_position(a), sb = p_cam->unproject_position(b);
				const Vector2 sd = sb - sa;
				const float L2 = sd.length_squared();
				if (L2 < 1.0f) {
					continue;
				}
				const float st = (p_pos - sa).dot(sd) / L2;
				if (st <= 0.02f || st >= 0.98f) {
					continue; // (its ends: the points themselves, above)
				}
				const float px = (sa + sd * st).distance_to(p_pos);
				if (px < best) {
					const float tt = CLAMP(line_ray_param(a, b - a, o, d), 0.0f, 1.0f);
					best = px;
					on.local = l[i].local.lerp(l[i + 1].local, tt);
					on.face = l[i].face >= 0 ? l[i].face : l[i + 1].face;
					if (on.face >= 0) {
						const int fa = hewn::ops::face_at_point(m, on.local, on.face);
						on.face = fa >= 0 ? fa : (l[i + 1].face >= 0 ? hewn::ops::face_at_point(m, on.local, l[i + 1].face) : -1);
					}
					found = true;
				}
			}
		};
		std::vector<KnifeClick> cur = knife_points;
		try_line(cur);
		for (const std::vector<KnifeClick> &dl : knife_done) {
			try_line(dl);
		}
		if (found) {
			r_click = on;
			return true;
		}
	}
	_pick_mode(p_cam, p_pos, MODE_EDGE, v, e, f);
	if (e.a >= 0) {
		const int id = t.find_edge(e.a, e.b);
		const Vector3 a = g.xform(m.pos[e.a]), b = g.xform(m.pos[e.b]);
		Vector3 o, d;
		_ray(p_cam, p_pos, o, d);
		const float tt = p_midpoint ? 0.5f : CLAMP(line_ray_param(a, b - a, o, d), 0.0f, 1.0f);
		r_click.face = id >= 0 ? t.edge_faces[id][0] : -1; // (a wire: in space, on it)
		r_click.local = m.pos[e.a].lerp(m.pos[e.b], tt);
		return true;
	}
	Vector3 o, d;
	_ray(p_cam, p_pos, o, d);
	float dist;
	const int face = _raycast_face(o, d, &dist);
	if (face < 0) {
		return _knife_empty(p_cam, p_pos, r_click);
	}
	r_click.face = face;
	Vector3 w = o + d * dist;
	const float sm = (float)dock->snap_move->get_value();
	if (snap_increment_on(tool_ctrl) && sm > 0.0f) {
		// onto the grid drawn on the face (like new shapes): its world-aligned axes, steps from the origin
		const Vector3 nrm = g.basis.inverse().transposed().xform(hewn::face_normal(m, m.faces[face])).normalized();
		Vector3 u, v;
		_grid_axes(nrm, u, v);
		const float a = w.dot(u), b = w.dot(v);
		const Vector3 snapped = w + u * (std::round(a / sm) * sm - a) + v * (std::round(b / sm) * sm - b);
		// (still on this face: else the plain point)
		Vector3 so, sd;
		_ray(p_cam, p_cam->unproject_position(snapped), so, sd);
		float sdist;
		if (_raycast_face(so, sd, &sdist) == face) {
			w = snapped;
		}
	}
	r_click.local = g.affine_inverse().xform(w);
	return true;
}

// A knife point in empty space: as deep as the point before it (else the mesh's middle).
bool HewnEditorPlugin::_knife_empty(Camera3D *p_cam, const Vector2 &p_pos, KnifeClick &r_click) const {
	HewnMesh3D *n = get_edited();
	if (!n || !p_cam) {
		return false;
	}
	const Transform3D g = n->get_global_transform();
	const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2).normalized();
	const Vector3 at = knife_points.empty() ? g.xform(n->get_data().pos.empty() ? Vector3() : n->get_aabb().get_center()) : g.xform(knife_points.back().local);
	Vector3 o, d;
	if (!_ray(p_cam, p_pos, o, d)) {
		return false;
	}
	const float den = d.dot(fwd);
	if (std::fabs(den) < 1e-6f) {
		return false;
	}
	const float tt = (at - o).dot(fwd) / den;
	if (tt <= 0.0f) {
		return false;
	}
	r_click.face = -1;
	r_click.local = g.affine_inverse().xform(o + d * tt);
	return true;
}

// knife: the cut follows the line drawn on the screen. Each segment cuts the visible edges under it, in
// order along the line, from its click to the next one (a click on the mesh is a point of the cut, one in empty
// space only shows where the line goes). Hits in a row on one face are joined across it; where the line leaves
// the surface (empty space, or onto a part not joined to it: a ceiling seen past a wall's edge) the cut ends and
// another starts. A part of one edge crossed alone (and every wire crossed) gets a vertex there.
void HewnEditorPlugin::_knife_pieces(Camera3D *p_cam, const std::vector<KnifeClick> &p_cut, std::vector<std::vector<hewn::ops::KnifeHit>> &r_pieces, std::vector<Vector3> &r_splits) const {
	HewnMesh3D *n = get_edited();
	if (!n || !p_cam || p_cut.size() < 2) {
		return;
	}
	const hewn::Mesh &m = n->get_data();
	const Transform3D g = n->get_global_transform();
	const Vector3 eye = p_cam->get_global_position();
	const bool ortho = p_cam->get_projection() == Camera3D::PROJECTION_ORTHOGONAL;
	const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2).normalized();
	hewn::Topology t;
	t.build(m);
	struct Hit {
		float s;
		hewn::ops::KnifeHit h;
		bool wire;
	};
	std::vector<hewn::ops::KnifeHit> line; // the whole cut, in order
	std::vector<char> gap_before; // (a click in empty space between: never joined over it)
	auto push = [&](const hewn::ops::KnifeHit &h, bool gap) {
		if (!line.empty()) {
			const hewn::ops::KnifeHit &b = line.back();
			const bool same = (h.vert >= 0 && h.vert == b.vert) || (h.vert < 0 && b.vert < 0 && h.pos.distance_to(b.pos) < 1e-5f);
			if (same) {
				return;
			}
		}
		line.push_back(h);
		gap_before.push_back(gap ? 1 : 0);
	};
	auto click_hit = [&](const KnifeClick &c, hewn::ops::KnifeHit &r) {
		if (c.face < 0) {
			return false;
		}
		const int f = hewn::ops::face_at_point(m, c.local, c.face);
		if (f < 0) {
			return false;
		}
		r = hewn::ops::knife_classify(m, f, c.local);
		return true;
	};
	bool gap = false;
	for (size_t i = 0; i + 1 < p_cut.size(); i++) {
		const KnifeClick &P = p_cut[i], &Q = p_cut[i + 1];
		std::vector<Hit> seg;
		hewn::ops::KnifeHit ch;
		if (click_hit(P, ch)) {
			seg.push_back({ 0.0f, ch, false });
		}
		const Vector3 A = g.xform(P.local), B = g.xform(Q.local);
		const Vector3 N = ortho ? (B - A).cross(fwd) : (A - eye).cross(B - eye);
		const Vector2 sa = p_cam->unproject_position(A), sb = p_cam->unproject_position(B);
		const float sl = (sb - sa).length_squared();
		if (!p_cam->is_position_behind(A) && !p_cam->is_position_behind(B) && N.length_squared() > 1e-12f && sl > 1e-6f) {
			const Vector3 Nn = N.normalized();
			auto test = [&](int a, int b, bool wire) {
				const Vector3 Pa = g.xform(m.pos[a]), Pb = g.xform(m.pos[b]);
				const float da = Nn.dot(Pa - A), db = Nn.dot(Pb - A);
				const float tol = 1e-5f * (1.0f + Pa.distance_to(Pb));
				if (std::fabs(da) <= tol && std::fabs(db) <= tol) {
					return; // (lies in the cut: an edge made by the knife itself)
				}
				if ((da > tol && db > tol) || (da < -tol && db < -tol)) {
					return;
				}
				const float u = std::fabs(da - db) > 1e-12f ? CLAMP(da / (da - db), 0.0f, 1.0f) : 0.0f;
				const Vector3 X = Pa.lerp(Pb, u);
				if (p_cam->is_position_behind(X)) {
					return;
				}
				const float s = (p_cam->unproject_position(X) - sa).dot(sb - sa) / sl;
				if (s < -1e-4f || s > 1.0f + 1e-4f) {
					return;
				}
				// seen: nothing of the mesh in front of it
				Vector3 o, d;
				if (ortho) {
					d = fwd;
					o = X - fwd * 1000.0f;
				} else {
					o = eye;
					d = (X - eye).normalized();
				}
				float dist;
				const float dx = o.distance_to(X);
				if (_raycast_face(o, d, &dist) >= 0 && dist < dx - MAX(1e-3f, dx * 2e-3f)) {
					return;
				}
				hewn::ops::KnifeHit h;
				h.pos = m.pos[a].lerp(m.pos[b], u);
				if (u <= 1e-4f || u >= 1.0f - 1e-4f) {
					h.vert = u < 0.5f ? a : b;
					h.pos = m.pos[h.vert];
				} else {
					h.ea = a;
					h.eb = b;
					h.t = u;
				}
				seg.push_back({ CLAMP(s, 0.0f, 1.0f), h, wire });
			};
			for (int e = 0; e < (int)t.edges.size(); e++) {
				test(t.edges[e].a, t.edges[e].b, false);
			}
			for (const std::pair<int, int> &w : m.wires) {
				test(w.first, w.second, true);
			}
		}
		if (click_hit(Q, ch)) {
			seg.push_back({ 1.0f, ch, false });
		}
		std::stable_sort(seg.begin(), seg.end(), [](const Hit &x, const Hit &y) { return x.s < y.s; });
		for (const Hit &x : seg) {
			if (x.wire) {
				if (x.h.ea >= 0) {
					r_splits.push_back(x.h.pos);
				}
				continue;
			}
			push(x.h, gap);
			gap = false;
		}
		gap = Q.face < 0; // (the line goes through empty space at Q)
	}
	// into parts: each hit joined to the one before only on a face they share
	auto faces_of = [&](const hewn::ops::KnifeHit &h) {
		if (h.vert >= 0) {
			return h.vert < (int)t.vert_faces.size() ? t.vert_faces[h.vert] : std::vector<int>();
		}
		if (h.ea >= 0) {
			const int e = t.find_edge(h.ea, h.eb);
			return e >= 0 ? t.edge_faces[e] : std::vector<int>();
		}
		return h.face >= 0 ? std::vector<int>{ h.face } : std::vector<int>();
	};
	auto share = [&](const hewn::ops::KnifeHit &x, const hewn::ops::KnifeHit &y) {
		const std::vector<int> fx = faces_of(x), fy = faces_of(y);
		for (int f : fx) {
			if (std::find(fy.begin(), fy.end(), f) != fy.end()) {
				return true;
			}
		}
		return false;
	};
	std::vector<hewn::ops::KnifeHit> piece;
	auto end_piece = [&]() {
		if (piece.size() >= 2) {
			r_pieces.push_back(piece);
		} else if (piece.size() == 1 && piece[0].ea >= 0) {
			r_splits.push_back(piece[0].pos); // (one edge crossed alone)
		}
		piece.clear();
	};
	for (size_t i = 0; i < line.size(); i++) {
		if (!piece.empty() && (gap_before[i] || !share(piece.back(), line[i]))) {
			end_piece();
		}
		piece.push_back(line[i]);
	}
	end_piece();
}

// Knife point with the angle constraint (A): exactly on the constrained line, landing exactly on an edge
// (or a vertex) when the cursor is near one, so cuts that come back to an edge do not leave slivers.
bool HewnEditorPlugin::_knife_pick_constrained(Camera3D *p_cam, const Vector2 &p_pos, bool p_midpoint, KnifeClick &r_click) const {
	HewnMesh3D *n = get_edited();
	if (angle_lock == 0 || !n || knife_points.empty() || knife_points.back().face < 0 || knife_points.back().face >= (int)n->get_data().faces.size()) {
		return _knife_pick(p_cam, p_pos, p_midpoint, r_click);
	}
	const hewn::Mesh &km = n->get_data();
	const Transform3D g = n->get_global_transform();
	const Transform3D inv = g.affine_inverse();
	const Vector3 last = knife_points.back().local;
	const float eps = 1e-4f * MAX(1.0f, last.length());
	const float step = (float)Math::PI / 6.0f;
	const float near_px = 10.0f;
	Vector3 o, d;
	_ray(p_cam, p_pos, o, d);
	if (angle_lock == 1) {
		// Screen space: the cursor goes onto the 30 degree line; an edge crossing that line near the
		// cursor gives the exact point on the edge.
		const Vector2 P = _angle_constrain(p_cam, p_pos, g.xform(last), Vector3(), Vector3());
		const Vector2 Ls = p_cam->unproject_position(g.xform(last));
		const Vector2 ld = (P - Ls).length() > 1e-6f ? (P - Ls).normalized() : Vector2(1, 0);
		Vector3 po, pd;
		_ray(p_cam, P, po, pd);
		float dist;
		const int hf = _raycast_face(po, pd, &dist);
		if (hf < 0) {
			return false;
		}
		const hewn::Face &F = km.faces[hf];
		float best = near_px;
		bool on_edge = false;
		Vector3 best_pt;
		for (int i = 0; i < F.size(); i++) {
			const Vector3 ea = g.xform(km.pos[F.v[i]]), eb = g.xform(km.pos[F.v[(i + 1) % F.size()]]);
			if (p_cam->is_position_behind(ea) || p_cam->is_position_behind(eb)) {
				continue;
			}
			const Vector2 sa = p_cam->unproject_position(ea), sb = p_cam->unproject_position(eb);
			const Vector2 e = sb - sa;
			const float den = ld.cross(e);
			if (std::fabs(den) < 1e-9f) {
				continue;
			}
			const float te = (sa - Ls).cross(ld) / den; // along the edge on screen
			if (te < -1e-4f || te > 1.0f + 1e-4f) {
				continue;
			}
			const Vector2 X = sa + e * CLAMP(te, 0.0f, 1.0f);
			if (X.distance_to(P) < best) {
				Vector3 xo, xd;
				_ray(p_cam, X, xo, xd);
				const float u = CLAMP(line_ray_param(ea, eb - ea, xo, xd), 0.0f, 1.0f);
				best = X.distance_to(P);
				best_pt = ea.lerp(eb, u);
				on_edge = true;
			}
		}
		r_click.face = hf;
		r_click.local = on_edge ? inv.xform(best_pt) : inv.xform(po + pd * dist);
		return true;
	}
	// Surface: the line lies in the plane of the face under the cursor that holds the last point (a point
	// on an edge or a vertex belongs to several faces), else the face it was picked on; angles count from
	// the edge the point lies on.
	int f = knife_points.back().face;
	{
		const int hf = _raycast_face(o, d);
		if (hf >= 0 && std::fabs(hewn::face_normal(km, km.faces[hf]).dot(last - hewn::face_center(km, km.faces[hf]))) < eps) {
			f = hf;
		}
	}
	const hewn::Face &F = km.faces[f];
	Vector3 ref = km.pos[F.v[1]] - km.pos[F.v[0]];
	for (int i = 0; i < F.size(); i++) {
		const Vector3 ea = km.pos[F.v[i]], ab = km.pos[F.v[(i + 1) % F.size()]] - ea;
		const float tt = ab.length_squared() > 0.0f ? CLAMP((last - ea).dot(ab) / ab.length_squared(), 0.0f, 1.0f) : 0.0f;
		if ((ea + ab * tt).distance_to(last) < eps) {
			ref = ab;
			break;
		}
	}
	const Vector3 nl = hewn::face_normal(km, F);
	Vector3 u = ref - nl * ref.dot(nl);
	if (u.length() < 1e-9f) {
		Vector3 tmp;
		hewn::plane_basis(nl, u, tmp);
	}
	u.normalize();
	const Vector3 w = nl.cross(u);
	// the cursor ray in local space, on the face plane
	const Vector3 ol = inv.xform(o), dl = inv.basis.xform(d);
	const float den = dl.dot(nl);
	if (std::fabs(den) < 1e-9f) {
		return _knife_pick(p_cam, p_pos, p_midpoint, r_click);
	}
	const Vector3 H = ol + dl * ((last - ol).dot(nl) / den);
	const Vector3 rv = H - last;
	const float ang = std::round(std::atan2(rv.dot(w), rv.dot(u)) / step) * step;
	const Vector3 dir = u * std::cos(ang) + w * std::sin(ang);
	const Vector3 Q = last + dir * MAX(0.0f, rv.dot(dir));
	auto to2 = [&](const Vector3 &p) { return Vector2((p - last).dot(u), (p - last).dot(w)); };
	const Vector2 d2 = to2(last + dir);
	// where the line leaves the face through an edge (exact), if the cursor is near it
	float best = near_px;
	bool hit_edge = false;
	Vector3 best_pt;
	for (int i = 0; i < F.size(); i++) {
		const Vector3 ea = km.pos[F.v[i]], eb = km.pos[F.v[(i + 1) % F.size()]];
		const Vector2 a2 = to2(ea), e2 = to2(eb) - a2;
		const float dd = d2.cross(e2);
		if (std::fabs(dd) < 1e-9f) {
			continue;
		}
		const float sl = a2.cross(e2) / dd; // along the line
		const float te = a2.cross(d2) / dd; // along the edge
		if (sl <= eps || te < -1e-4f || te > 1.0f + 1e-4f) {
			continue;
		}
		const Vector3 X = ea.lerp(eb, CLAMP(te, 0.0f, 1.0f));
		const Vector3 Xw = g.xform(X);
		if (p_cam->is_position_behind(Xw)) {
			continue;
		}
		const float px = p_cam->unproject_position(Xw).distance_to(p_pos);
		if (px < best) {
			best = px;
			best_pt = X;
			hit_edge = true;
		}
	}
	if (hit_edge) {
		r_click.face = f;
		r_click.local = best_pt;
		return true;
	}
	// inside the face: the point on the line
	std::vector<Vector2> poly;
	for (int v : F.v) {
		poly.push_back(to2(km.pos[v]));
	}
	const Vector2 q2 = to2(Q);
	bool inside = false;
	for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
		if ((poly[i].y > q2.y) != (poly[j].y > q2.y) && q2.x < (poly[j].x - poly[i].x) * (q2.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x) {
			inside = !inside;
		}
	}
	if (inside) {
		r_click.face = f;
		r_click.local = Q;
		return true;
	}
	// beyond the face: an edge near the cursor where the line's plane (along it, across the face) meets it, exactly
	// there (the point does not slide along the edge as the mouse moves)
	{
		const Vector3 cn = dir.cross(nl).normalized();
		hewn::Topology t;
		t.build(km);
		float bestx = near_px;
		bool found = false;
		Vector3 bx;
		int bf = -1;
		for (int ei = 0; ei < (int)t.edges.size(); ei++) {
			const Vector3 pa = km.pos[t.edges[ei].a], pb = km.pos[t.edges[ei].b];
			const float da = cn.dot(pa - last), db = cn.dot(pb - last);
			if ((da > eps && db > eps) || (da < -eps && db < -eps) || std::fabs(da - db) < 1e-12f) {
				continue;
			}
			const Vector3 X = pa.lerp(pb, CLAMP(da / (da - db), 0.0f, 1.0f));
			if ((X - last).dot(dir) <= eps) {
				continue;
			}
			const Vector3 Xw = g.xform(X);
			if (p_cam->is_position_behind(Xw)) {
				continue;
			}
			const float px = p_cam->unproject_position(Xw).distance_to(p_pos);
			if (px < bestx && !t.edge_faces[ei].empty()) {
				bestx = px;
				bx = X;
				bf = t.edge_faces[ei][0];
				found = true;
			}
		}
		if (found) {
			r_click.face = bf;
			r_click.local = bx;
			return true;
		}
	}
	// else on the surface under the constrained screen point
	const Vector3 Qw = g.xform(Q);
	return !p_cam->is_position_behind(Qw) && _knife_pick(p_cam, p_cam->unproject_position(Qw), false, r_click);
}

std::vector<Vector3> HewnEditorPlugin::_knife_normals(Camera3D *p_cam, const std::vector<hewn::ops::KnifePoint> &p_pts) const {
	HewnMesh3D *n = get_edited();
	std::vector<Vector3> out;
	if (!n || !p_cam) {
		return out;
	}
	const Transform3D inv = n->get_global_transform().affine_inverse();
	const Vector3 eye = inv.xform(p_cam->get_global_position());
	const Vector3 view = inv.basis.xform(cam_forward(p_cam));
	const bool ortho = p_cam->get_projection() == Camera3D::PROJECTION_ORTHOGONAL;
	for (size_t i = 0; i + 1 < p_pts.size(); i++) {
		const Vector3 a = p_pts[i].pos, b = p_pts[i + 1].pos;
		out.push_back(ortho ? (b - a).cross(view) : (a - eye).cross(b - eye));
	}
	return out;
}

void HewnEditorPlugin::_apply_knife(Camera3D *p_cam) {
	HewnMesh3D *n = get_edited();
	std::vector<std::vector<KnifeClick>> cuts = knife_done;
	if (knife_points.size() >= 2) {
		cuts.push_back(knife_points);
	}
	modal = MODAL_NONE;
	knife_points.clear();
	knife_done.clear();
	if (!n || cuts.empty()) {
		update_overlays();
		return;
	}
	// the parts of the cuts as seen now (each one finds its edges / faces again after the ones before)
	std::vector<std::vector<hewn::ops::KnifeHit>> pieces;
	std::vector<Vector3> splits;
	for (const std::vector<KnifeClick> &c : cuts) {
		_knife_pieces(p_cam, c, pieces, splits);
	}
	begin_edit("Knife");
	hewn::Mesh &m = n->get_data();
	std::vector<hewn::Edge> edges;
	std::vector<int> split_verts;
	{
		// all the cuts at once: each face split by all of them lying in it (no pieces over each other)
		const std::vector<hewn::Edge> e = hewn::ops::knife_net(m, pieces);
		edges.insert(edges.end(), e.begin(), e.end());
	}
	for (const Vector3 &at : splits) {
		// (the edge may have been cut already: the piece holding the point)
		const int v = m.add_vertex(at);
		bool done = false;
		for (std::pair<int, int> &w : m.wires) {
			const Vector3 P = m.pos[w.first], Q = m.pos[w.second];
			const float L = (Q - P).length_squared();
			const float u = L > 1e-12f ? (at - P).dot(Q - P) / L : -1.0f;
			if (u > 1e-4f && u < 1.0f - 1e-4f && P.lerp(Q, u).distance_to(at) < 1e-4f * (1.0f + std::sqrt(L))) {
				const int b = w.second;
				w.second = v;
				m.wires.push_back({ v, b });
				done = true;
				break;
			}
		}
		if (!done) {
			hewn::Topology tt;
			tt.build(m);
			for (const hewn::Edge &e : tt.edges) {
				const Vector3 P = m.pos[e.a], Q = m.pos[e.b];
				const float L = (Q - P).length_squared();
				const float u = L > 1e-12f ? (at - P).dot(Q - P) / L : -1.0f;
				if (u > 1e-4f && u < 1.0f - 1e-4f && P.lerp(Q, u).distance_to(at) < 1e-4f * (1.0f + std::sqrt(L))) {
					hewn::ops::insert_on_edge(m, e.a, e.b, { { u, v } });
					done = true;
					break;
				}
			}
		}
		if (done) {
			split_verts.push_back(v);
		}
	}
	if (edges.empty() && split_verts.empty()) {
		n->set_state(edit_before);
		editing = false;
		update_overlays();
		return;
	}
	n->selection.clear();
	n->selection.edges = edges;
	if (edges.empty()) {
		n->selection.verts = split_verts; // (only edges split: their new vertices)
	}
	hewn::remove_unused_vertices(m, &n->selection);
	mode = element_mode = edges.empty() ? MODE_VERTEX : MODE_EDGE;
	n->commit(true);
	end_edit();
}

// Knife Project: the outlines of the other edited meshes (their open borders and loose edges; a closed mesh:
// its outline as seen from here) laid onto the active mesh from the view, and cut there like a knife would.
void HewnEditorPlugin::knife_project() {
	HewnMesh3D *n = get_edited();
	Camera3D *cam = _viewport_camera();
	if (!n || !cam || mode == MODE_OBJECT) {
		return;
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	const Transform3D ginv = n->get_global_transform().affine_inverse();
	const Vector3 eye = cam->get_global_position();
	const Vector3 fwd = -cam->get_global_transform().basis.get_column(2).normalized();
	const bool ortho = cam->get_projection() == Camera3D::PROJECTION_ORTHOGONAL;
	std::vector<std::vector<KnifeClick>> cuts;
	for (HewnMesh3D *o : edited_meshes()) {
		if (o == n) {
			continue;
		}
		const hewn::Mesh &cm = o->get_data();
		const Transform3D cg = o->get_global_transform();
		const Basis cnb = cg.basis.inverse().transposed();
		hewn::Topology t;
		t.build(cm);
		std::vector<hewn::Edge> outline = hewn::sel::border_edges(cm, t);
		for (const std::pair<int, int> &w : cm.wires) {
			outline.push_back(hewn::Edge(w.first, w.second));
		}
		if (outline.empty()) {
			// closed: the edges between a face turned to the view and one turned away
			for (size_t e = 0; e < t.edges.size(); e++) {
				if (t.edge_faces[e].size() != 2) {
					continue;
				}
				const Vector3 mid = cg.xform((cm.pos[t.edges[e].a] + cm.pos[t.edges[e].b]) * 0.5f);
				const Vector3 view = ortho ? fwd : (mid - eye);
				bool front[2];
				for (int k = 0; k < 2; k++) {
					const hewn::Face &F = cm.faces[t.edge_faces[e][k]];
					front[k] = cnb.xform(hewn::face_normal(cm, F)).dot(view) < 0.0f;
				}
				if (front[0] != front[1]) {
					outline.push_back(t.edges[e]);
				}
			}
		}
		// the edges chained into lines (closed ones end where they began)
		std::unordered_map<int, std::vector<int>> at;
		for (int i = 0; i < (int)outline.size(); i++) {
			at[outline[i].a].push_back(i);
			at[outline[i].b].push_back(i);
		}
		std::vector<char> used(outline.size(), 0);
		auto walk = [&](int start_edge, int start_vert) {
			std::vector<int> chain{ start_vert };
			int e = start_edge, v = start_vert;
			while (e >= 0 && !used[e]) {
				used[e] = 1;
				v = outline[e].a == v ? outline[e].b : outline[e].a;
				chain.push_back(v);
				int next = -1;
				for (int x : at[v]) {
					if (!used[x]) {
						next = x;
						break;
					}
				}
				e = next;
			}
			return chain;
		};
		std::vector<std::vector<int>> chains;
		for (const auto &kv : at) {
			if (kv.second.size() == 1 && !used[kv.second[0]]) {
				chains.push_back(walk(kv.second[0], kv.first)); // open lines from their ends
			}
		}
		for (int i = 0; i < (int)outline.size(); i++) {
			if (!used[i]) {
				chains.push_back(walk(i, outline[i].a));
			}
		}
		for (const std::vector<int> &ch : chains) {
			if (ch.size() < 2) {
				continue;
			}
			// each point onto the active mesh along the view; long edges in short steps (so the points land
			// on its faces)
			std::vector<KnifeClick> cut;
			for (size_t i = 0; i + 1 < ch.size(); i++) {
				const Vector3 a = cg.xform(cm.pos[ch[i]]), b = cg.xform(cm.pos[ch[i + 1]]);
				const float px = cam->is_position_behind(a) || cam->is_position_behind(b) ? 0.0f : cam->unproject_position(a).distance_to(cam->unproject_position(b));
				const int steps = CLAMP((int)(px / 12.0f), 1, 256);
				for (int k = 0; k < steps || (k == steps && i + 2 == ch.size()); k++) {
					const Vector3 p = a.lerp(b, (float)k / (float)steps);
					KnifeClick c;
					if (!cam->is_position_behind(p)) {
						const Vector2 sp = cam->unproject_position(p);
						const Vector3 ro = cam->project_ray_origin(sp), rd = cam->project_ray_normal(sp).normalized();
						float d = 0.0f;
						const int f = raycast_node(n, ro, rd, d, nullptr, false);
						c.face = f;
						c.local = ginv.xform(f >= 0 ? ro + rd * d : p);
					} else {
						c.local = ginv.xform(p);
					}
					cut.push_back(c);
				}
			}
			if (cut.size() >= 2) {
				cuts.push_back(cut);
			}
		}
	}
	if (cuts.empty()) {
		if (EditorToaster *et = EditorInterface::get_singleton()->get_editor_toaster()) {
			et->push_toast(T("Knife Project: edit the mesh to cut (the active one) together with the meshes whose outlines cut it"), EditorToaster::SEVERITY_WARNING);
		}
		return;
	}
	knife_points.clear();
	knife_done = cuts;
	_apply_knife(cam);
	last_op = "";
}

// ------------------------------------------------------------------ loop cut

void HewnEditorPlugin::start_loop_cut() {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	if (mode == MODE_OBJECT) {
		set_mode(element_mode);
	}
	loop_edge = hewn::Edge(-1, -1);
	loop_cuts = 1; //, the number of cuts is not remembered
	loop_phase = 0;
	loop_t = 0.5f;
	modal = MODAL_LOOPCUT;
	update_overlays();
}

void HewnEditorPlugin::_apply_loop_cut() {
	HewnMesh3D *n = get_edited();
	modal = MODAL_NONE;
	if (!n || loop_edge.a < 0) {
		update_overlays();
		return;
	}
	set_op_param("loop_cut", "a", loop_edge.a);
	set_op_param("loop_cut", "b", loop_edge.b);
	set_op_param("loop_cut", "cuts", loop_cuts);
	set_op_param("loop_cut", "factor", loop_t * 2.0f - 1.0f);
	run_op("loop_cut");
	_set_godot_gizmo_hidden(mode != MODE_OBJECT);
}

// ------------------------------------------------------------------ box cutter

void HewnEditorPlugin::start_cutter() {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return;
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	cutter_state = 0;
	cutter_poly.clear();
	cutter_op = dock->cutter_op->get_selected();
	modal = MODAL_CUTTER;
	update_overlays();
}

bool HewnEditorPlugin::_cutter_begin(Camera3D *p_cam, const Vector2 &p_pos) {
	HewnMesh3D *n = get_edited();
	if (!n) {
		return false;
	}
	const Transform3D g = n->get_global_transform();
	const hewn::Mesh &m = n->get_data();
	Vector3 o, d;
	_ray(p_cam, p_pos, o, d);
	float dist;
	const int f = _raycast_face(o, d, &dist);
	if (f >= 0) {
		// Draw on the face: plane through the hit, aligned with the closest face edge.
		const hewn::Face &F = m.faces[f];
		cutter_origin = o + d * dist;
		cutter_n = g.basis.inverse().transposed().xform(hewn::face_normal(m, F)).normalized();
		const Vector3 local = g.affine_inverse().xform(cutter_origin);
		float best = 1e30f;
		Vector3 dir(1, 0, 0);
		for (int i = 0; i < F.size(); i++) {
			const Vector3 a = m.pos[F.v[i]], b = m.pos[F.v[(i + 1) % F.size()]];
			const Vector3 ab = b - a;
			const float l2 = ab.length_squared();
			if (l2 < 1e-12f) {
				continue;
			}
			const float t = CLAMP((local - a).dot(ab) / l2, 0.0f, 1.0f);
			const float dd = (a + ab * t).distance_to(local);
			if (dd < best) {
				best = dd;
				dir = g.basis.xform(ab);
			}
		}
		cutter_u = (dir - cutter_n * dir.dot(cutter_n)).normalized();
		if (cutter_u.length() < 0.5f) {
			Vector3 tmp;
			hewn::plane_basis(cutter_n, cutter_u, tmp);
		}
		if (snapping_active(tool_ctrl) && snap_grid_on()) {
			Vector3 tmp;
			_grid_axes(cutter_n, cutter_u, tmp); // (on the grid: the frame drawn along the grid's lines)
		}
		cutter_material = F.material;
	} else {
		// Empty space: a view-aligned plane through the object center; the cut goes through it.
		Vector3 c;
		for (const Vector3 &p : m.pos) {
			c += p;
		}
		cutter_origin = g.xform(m.pos.empty() ? Vector3() : c / (float)m.pos.size());
		const Basis cb = p_cam->get_global_transform().basis;
		cutter_n = cb.get_column(2).normalized();
		cutter_u = cb.get_column(0).normalized();
		cutter_material = 0;
	}
	cutter_v = cutter_n.cross(cutter_u).normalized();
	return true;
}

bool HewnEditorPlugin::_cutter_plane_point(Camera3D *p_cam, const Vector2 &p_pos, Vector2 &r_uv) const {
	Vector3 hit;
	if (!_plane_hit(p_cam, p_pos, cutter_origin, cutter_n, hit)) {
		return false;
	}
	const float sm = (float)dock->snap_move->get_value();
	const bool on = snapping_active(tool_ctrl) && sm > 0.0f;
	if (on && snap_grid_on()) {
		// the grid: its lines from the world origin, in the plane (as drawn on the faces), not from the click
		Vector3 pu, pv;
		_grid_axes(cutter_n, pu, pv);
		const float a = hit.dot(pu), b = hit.dot(pv);
		hit += pu * (std::round(a / sm) * sm - a) + pv * (std::round(b / sm) * sm - b);
	}
	Vector2 uv((hit - cutter_origin).dot(cutter_u), (hit - cutter_origin).dot(cutter_v));
	if (on && !snap_grid_on()) {
		uv = Vector2(std::round(uv.x / sm) * sm, std::round(uv.y / sm) * sm); // (steps from the click)
	}
	r_uv = uv;
	return true;
}

std::vector<Vector2> HewnEditorPlugin::_cutter_outline() const {
	std::vector<Vector2> out;
	const int shape = dock->cutter_shape->get_selected();
	if (shape == CUTTER_NGON) {
		out = cutter_poly;
	} else {
		Vector2 d = cutter_b - cutter_a;
		if (cutter_square) {
			const float s = MAX(std::fabs(d.x), std::fabs(d.y));
			d = Vector2(d.x < 0 ? -s : s, d.y < 0 ? -s : s);
		}
		Vector2 lo = cutter_centered ? cutter_a - d : cutter_a;
		Vector2 hi = cutter_a + d;
		const Vector2 mn = lo.min(hi), mx = lo.max(hi);
		if (shape == CUTTER_RECT) {
			out = { mn, Vector2(mx.x, mn.y), mx, Vector2(mn.x, mx.y) };
		} else {
			const Vector2 c = (mn + mx) * 0.5f, r = (mx - mn) * 0.5f;
			const int sides = MAX(3, (int)dock->cutter_sides->get_value());
			for (int i = 0; i < sides; i++) {
				const float a = (float)Math::PI * 2.0f * i / sides;
				out.push_back(c + Vector2(std::cos(a) * r.x, std::sin(a) * r.y));
			}
		}
	}
	if (out.size() >= 3 && signed_area(out) < 0.0f) {
		std::reverse(out.begin(), out.end());
	}
	return out;
}

void HewnEditorPlugin::_apply_cutter() {
	HewnMesh3D *n = get_edited();
	cutter_op = dock->cutter_op->get_selected();
	const std::vector<Vector2> outline = _cutter_outline();
	cutter_state = 0;
	cutter_poly.clear();
	update_overlays();
	if (!n || outline.size() < 3 || std::fabs(signed_area(outline)) < 1e-8f) {
		return;
	}
	const Transform3D g = n->get_global_transform();
	const Transform3D inv = g.affine_inverse();
	hewn::Mesh &m = n->get_data();
	// Object extent along the plane normal decides how far "through" goes.
	float lo = 0.0f, hi = 0.0f, size = 0.0f;
	for (const Vector3 &p : m.pos) {
		const Vector3 w = g.xform(p);
		const float h = (w - cutter_origin).dot(cutter_n);
		lo = MIN(lo, h);
		hi = MAX(hi, h);
		size = MAX(size, w.distance_to(cutter_origin));
	}
	const float eps = MAX(1e-4f, size * 1e-3f);
	{
		// Tiny or flat outlines make the boolean numerically unstable: ignore them.
		Vector2 mn = outline[0], mx = outline[0];
		for (const Vector2 &p : outline) {
			mn = mn.min(p);
			mx = mx.max(p);
		}
		const Vector2 ext = mx - mn;
		if (MIN(ext.x, ext.y) < eps * 2.0f || std::fabs(signed_area(outline)) < eps * eps * 4.0f) {
			return;
		}
		for (size_t i = 0; i < outline.size(); i++) {
			if (outline[i].distance_to(outline[(i + 1) % outline.size()]) < eps * 0.5f) {
				return;
			}
		}
	}
	const bool additive = cutter_op == CUTTER_JOIN || cutter_op == CUTTER_MAKE;
	const bool through = dock->cutter_through->is_pressed() && !additive;
	float h0, h1; // along the normal
	if (additive) {
		h0 = -eps;
		h1 = MAX(cutter_depth, eps * 2.0f);
	} else if (through) {
		h0 = hi + eps * 10.0f;
		h1 = lo - eps * 10.0f;
	} else {
		h0 = eps;
		h1 = -MAX(cutter_depth, eps * 2.0f);
	}
	if (h0 < h1) {
		std::swap(h0, h1);
	}
	// Prism: outline is counter-clockwise around cutter_n.
	hewn::Mesh cutter;
	const size_t k = outline.size();
	std::vector<int> top, bot;
	for (const Vector2 &p : outline) {
		const Vector3 base = cutter_origin + cutter_u * p.x + cutter_v * p.y;
		top.push_back(cutter.add_vertex(inv.xform(base + cutter_n * h0)));
		bot.push_back(cutter.add_vertex(inv.xform(base + cutter_n * h1)));
	}
	const bool round = dock->cutter_shape->get_selected() == CUTTER_CIRCLE;
	auto add = [&](const std::vector<int> &v, bool side) {
		hewn::Face f = hewn::make_face(v);
		f.material = cutter_material;
		f.smooth = side && round ? (1 << 21) : 0;
		cutter.faces.push_back(f);
	};
	add(top, false);
	std::vector<int> rb(bot.rbegin(), bot.rend());
	add(rb, false);
	for (size_t i = 0; i < k; i++) {
		const size_t j = (i + 1) % k;
		add({ bot[i], bot[j], top[j], top[i] }, true);
	}
	// The prism must face outwards in node space too (mirrored transforms flip it).
	{
		float vol = 0.0f;
		for (const hewn::Face &f : cutter.faces) {
			for (int i = 1; i + 1 < f.size(); i++) {
				vol += cutter.pos[f.v[0]].dot(cutter.pos[f.v[i]].cross(cutter.pos[f.v[i + 1]]));
			}
		}
		if (vol < 0.0f) {
			for (hewn::Face &f : cutter.faces) {
				f.reverse();
			}
		}
	}

	if (cutter_op == CUTTER_MAKE) {
		HewnMesh3D *node = memnew(HewnMesh3D);
		node->set_name("Cutter");
		node->set_shape(hewn::shapes::CUSTOM);
		Vector3 c;
		for (const Vector3 &p : cutter.pos) {
			c += p;
		}
		c /= (float)cutter.pos.size();
		hewn::Mesh &dst = node->get_data();
		dst = cutter;
		for (Vector3 &p : dst.pos) {
			p -= c;
		}
		node->commit(true);
		Transform3D xf = g;
		xf.origin = g.xform(c);
		node->set_transform(xf);
		_add_node_with_undo(node, "Hewn: Box Cutter Make");
		return;
	}

	begin_edit(String("Box Cutter ") + CUTTER_OP_NAMES[cutter_op]);
	hewn::Mesh result;
	bool ok = false;
	switch (cutter_op) {
		case CUTTER_CUT:
			result = hewn::csg::boolean(m, cutter, hewn::csg::SUBTRACT, &ok);
			break;
		case CUTTER_SLICE: {
			result = hewn::csg::boolean(m, cutter, hewn::csg::SUBTRACT, &ok);
			bool ok2 = false;
			hewn::Mesh piece = hewn::csg::boolean(m, cutter, hewn::csg::INTERSECT, &ok2);
			ok = ok && ok2;
			hewn::ops::append(result, piece, Transform3D(), 0);
		} break;
		case CUTTER_JOIN:
			result = hewn::csg::boolean(m, cutter, hewn::csg::UNION, &ok);
			break;
		default:
			result = hewn::csg::boolean(m, cutter, hewn::csg::INTERSECT, &ok);
			break;
	}
	if (!ok) {
		ERR_PRINT("Hewn: the box cutter could not compute this cut (too thin or numerically unstable).");
		n->set_state(edit_before);
		editing = false;
		return;
	}
	m = result;
	n->selection.clear();
	n->commit(true);
	end_edit();
}

// ------------------------------------------------------------------ poly shape

// Base editing of an existing Poly Shape updates the mesh while dragging (undo is recorded on apply).
void HewnEditorPlugin::_poly_apply_live() {
	HewnMesh3D *n = poly_edit_id ? Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(poly_edit_id)) : nullptr;
	if (!n || poly_points.size() < (poly_edit_wall ? 2u : 3u)) {
		return;
	}
	const Transform3D inv = n->get_global_transform().affine_inverse();
	PackedVector3Array local;
	for (const Vector3 &p : poly_points) {
		const Vector3 l = inv.xform(p);
		local.push_back(Vector3(l.x, poly_edit_wall ? l.y : 0.0f, l.z)); // (a path keeps its corners' heights)
	}
	if (poly_edit_sweep) {
		n->set_path_gaps(_poly_gaps_packed());
	}
	n->set_poly_points(local);
}

PackedInt32Array HewnEditorPlugin::_poly_gaps_packed() const {
	PackedInt32Array out;
	for (int i = 0; i < (int)poly_gaps.size(); i++) {
		if (poly_gaps[i]) {
			out.push_back(i);
		}
	}
	return out;
}

void HewnEditorPlugin::start_poly_tool(bool p_edit_existing) {
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	HewnMesh3D *n = get_edited();
	poly_points.clear();
	poly_phase = 0;
	poly_drag_point = -1;
	poly_edit_id = 0;
	poly_has_plane = false;
	poly_height_value = 1.0f; // (the height is drawn with the mouse)
	poly_edit_wall = false;
	poly_edit_loop = false;
	poly_edit_sweep = false;
	poly_gaps.clear();
	poly_cursor_valid = false;
	if (p_edit_existing) {
		if (!n || (n->get_shape() != hewn::shapes::POLY_SHAPE && n->get_shape() != hewn::shapes::WALL && n->get_shape() != hewn::shapes::SWEEP && n->get_shape() != hewn::shapes::PATH_STAIRS)) {
			ERR_PRINT("Hewn: the selected mesh is not an editable Poly Shape, Wall or Sweep.");
			return;
		}
		poly_edit_wall = n->get_shape() == hewn::shapes::WALL || n->get_shape() == hewn::shapes::SWEEP || n->get_shape() == hewn::shapes::PATH_STAIRS; // (open paths)
		poly_edit_xf = n->get_transform();
		poly_edit_orig = n->get_poly_points();
		if ((n->get_shape() == hewn::shapes::SWEEP || n->get_shape() == hewn::shapes::PATH_STAIRS) && poly_edit_orig.size() < 2) {
			const float hl = n->get_shape() == hewn::shapes::SWEEP ? 2.0f : 1.5f;
			PackedVector3Array two; // (the straight piece it is without points)
			two.push_back(Vector3(-hl, 0.0f, 0.0f));
			two.push_back(Vector3(hl, 0.0f, 0.0f));
			n->set_poly_points(two);
		} else if (poly_edit_wall && poly_edit_orig.size() < 2) {
			// a straight wall: the same wall as a path through its two ends (origin at its base)
			const float L = n->get_shape_param("length"), h = n->get_shape_param("height");
			Transform3D xf = poly_edit_xf;
			xf.origin += xf.basis.xform(Vector3(0.0f, -h * 0.5f, 0.0f));
			n->set_transform(xf);
			PackedVector3Array two;
			two.push_back(Vector3(-L * 0.5f, 0.0f, 0.0f));
			two.push_back(Vector3(L * 0.5f, 0.0f, 0.0f));
			n->set_poly_points(two);
		}
		const Transform3D g = n->get_global_transform();
		const PackedVector3Array pts = n->get_poly_points();
		for (int64_t i = 0; i < pts.size(); i++) {
			poly_points.push_back(g.xform(Vector3(pts[i].x, poly_edit_wall ? pts[i].y : 0.0f, pts[i].z))); // (a path's corners keep their heights)
		}
		poly_origin = g.origin;
		poly_n = g.basis.get_column(1).normalized();
		poly_has_plane = true;
		poly_height_value = n->get_shape_param("height");
		poly_edit_id = n->get_instance_id();
		if (n->get_shape() == hewn::shapes::SWEEP) {
			// (its gaps: Alt+click a segment)
			poly_edit_sweep = true;
			poly_edit_loop = n->get_shape_param("closed") > 0.5f && poly_points.size() >= 3;
			poly_edit_gaps_orig = n->get_path_gaps();
			poly_gaps.assign(poly_points.size(), 0);
			for (int64_t i = 0; i < poly_edit_gaps_orig.size(); i++) {
				if (poly_edit_gaps_orig[i] >= 0 && poly_edit_gaps_orig[i] < (int)poly_gaps.size()) {
					poly_gaps[poly_edit_gaps_orig[i]] = 1;
				}
			}
		}
	}
	angle_lock = 0;
	snap_mark_valid = false;
	_gather_snap_candidates(false);
	modal = MODAL_POLY;
	update_overlays();
}

// A point being dragged keeps its height above the outline plane: the mouse moves it in its own plane.
bool HewnEditorPlugin::_poly_lifted_point(Camera3D *p_cam, const Vector2 &p_pos, bool p_snap, int p_index, Vector3 &r_point) {
	if (!poly_edit_wall || !poly_has_plane || p_index < 0 || p_index >= (int)poly_points.size()) {
		return _poly_plane_point(p_cam, p_pos, p_snap, r_point);
	}
	const Vector3 nn = poly_n.normalized();
	const float h = (poly_points[p_index] - poly_origin).dot(nn);
	const Vector3 base = poly_origin;
	poly_origin = base + nn * h;
	const bool ok = _poly_plane_point(p_cam, p_pos, p_snap, r_point);
	poly_origin = base;
	if (ok) {
		r_point += nn * (h - (r_point - base).dot(nn)); // (a snap onto geometry: still at its height)
	}
	return ok;
}

bool HewnEditorPlugin::_poly_plane_point(Camera3D *p_cam, const Vector2 &p_pos, bool p_snap, Vector3 &r_point) {
	Vector3 o, d;
	_ray(p_cam, p_pos, o, d);
	if (!poly_has_plane) {
		// First point: on the surface under the cursor, or on the ground plane.
		Vector3 hit, nrm;
		if (_raycast_scene(o, d, hit, nrm)) {
			poly_origin = hit;
			poly_n = nrm;
		} else if (std::fabs(d.y) > 1e-6f && -o.y / d.y > 0.0f) {
			poly_origin = o + d * (-o.y / d.y);
			poly_n = Vector3(0, 1, 0);
		} else {
			return false;
		}
	}
	Vector3 hit;
	if (!_plane_hit(p_cam, p_pos, poly_origin, poly_n, hit)) {
		return false;
	}
	last_snap_geom = false;
	Vector3 sp;
	if (p_snap && (dock->snap_targets & HewnDock::SNAP_POINTS) && _snap_target(p_cam, p_pos, sp)) {
		r_point = sp - poly_n.normalized() * (sp - poly_origin).dot(poly_n.normalized()); // onto the outline plane
		last_snap_geom = true;
		return true;
	}
	const float sm = (float)dock->snap_move->get_value();
	if (p_snap && sm > 0.0f && (dock->snap_targets & HewnDock::SNAP_STEPS)) {
		Vector3 u, v;
		_plane_axes(poly_n, u, v);
		if (poly_n.is_equal_approx(Vector3(0, 1, 0)) || poly_n.is_equal_approx(Vector3(0, -1, 0))) {
			hit = u * (std::round(hit.dot(u) / sm) * sm) + v * (std::round(hit.dot(v) / sm) * sm) + poly_n * hit.dot(poly_n); // grid through the origin
		} else {
			const Vector3 r = hit - poly_origin;
			hit = poly_origin + u * (std::round(r.dot(u) / sm) * sm) + v * (std::round(r.dot(v) / sm) * sm);
		}
	}
	r_point = hit;
	return true;
}

// A new Poly Shape node through world points (origin at their centroid, local Y along the outline normal).
void HewnEditorPlugin::_setup_poly_node(HewnMesh3D *p_node, const std::vector<Vector3> &p_pts, float p_height) const {
	Vector3 c;
	for (const Vector3 &p : p_pts) {
		c += p;
	}
	c /= (float)p_pts.size();
	const Vector3 Y = poly_n.normalized();
	Vector3 X, tmp;
	_plane_axes(Y, X, tmp); // turned like the transform orientation
	const Vector3 Z = X.cross(Y).normalized();
	Basis b;
	b.set_column(0, X);
	b.set_column(1, Y);
	b.set_column(2, Z);
	const Transform3D xf(b, c);
	const Transform3D inv = xf.affine_inverse();
	PackedVector3Array local;
	for (const Vector3 &p : p_pts) {
		const Vector3 l = inv.xform(p);
		local.push_back(Vector3(l.x, 0.0f, l.z));
	}
	p_node->set_poly_points(local);
	p_node->set_shape(hewn::shapes::POLY_SHAPE);
	p_node->set_shape_param("height", p_height);
	p_node->set_transform(xf);
}

// A new Poly Shape shows while it is drawn: the outline so far (plus the point under the mouse) flat,
// then with its height.
void HewnEditorPlugin::_update_poly_preview() {
	if (modal != MODAL_POLY || poly_edit_id != 0) {
		return;
	}
	std::vector<Vector3> pts = poly_points;
	if (poly_phase == 0 && poly_cursor_valid && (pts.empty() || pts.back().distance_to(poly_cursor) > 1e-4f)) {
		pts.push_back(poly_cursor);
	}
	HewnMesh3D *pv = shape_preview_id ? Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(shape_preview_id)) : nullptr;
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (pts.size() < 3 || !root) {
		if (pv) {
			pv->set_visible(false);
		}
		return;
	}
	const bool fresh = pv == nullptr;
	if (fresh) {
		pv = memnew(HewnMesh3D);
		pv->set_name("HewnPreview");
	}
	_setup_poly_node(pv, pts, poly_phase == 1 ? poly_height_value : 0.0f);
	if (fresh && texture_dock && texture_dock->current_material.is_valid()) {
		pv->apply_material(texture_dock->current_material, PackedInt32Array()); // (as it will be made: the texture picked last)
	}
	const Transform3D xf = pv->get_transform();
	if (fresh) {
		root->add_child(pv, false, Node::INTERNAL_MODE_BACK);
		shape_preview_id = pv->get_instance_id();
	}
	pv->set_visible(true);
	pv->set_global_transform(xf);
	_view_new_mesh(pv); // (Single Color...: the preview shows it too, not the texture)
}

void HewnEditorPlugin::_finish_poly() {
	_remove_shape_preview();
	std::vector<Vector3> pts = poly_points;
	const float height = poly_height_value;
	const uint64_t edit_id = poly_edit_id;
	const bool wall = poly_edit_wall && edit_id;
	modal = MODAL_NONE;
	poly_points.clear();
	poly_phase = 0;
	poly_edit_id = 0;
	poly_edit_wall = false;
	update_overlays();
	if (pts.size() < (wall ? 2u : 3u)) {
		poly_cut = false;
		return;
	}
	if (poly_cut) {
		poly_cut = false;
		_apply_prism_cut(pts, height);
		return;
	}
	HewnMesh3D *existing = edit_id ? Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(edit_id)) : nullptr;
	const bool sweep_gaps = poly_edit_sweep && existing;
	const PackedInt32Array gaps_now = _poly_gaps_packed();
	poly_edit_sweep = false;
	if (existing) {
		const Transform3D inv = existing->get_global_transform().affine_inverse();
		PackedVector3Array local;
		for (const Vector3 &p : pts) {
			const Vector3 l = inv.xform(p);
			local.push_back(Vector3(l.x, wall ? l.y : 0.0f, l.z));
		}
		EditorUndoRedoManager *ur = get_undo_redo();
		ur->create_action(wall ? "Hewn: Edit Wall Points" : "Hewn: Edit Poly Shape", UndoRedo::MERGE_DISABLE, existing);
		ur->add_do_method(existing, "set_transform", existing->get_transform());
		if (sweep_gaps) {
			ur->add_do_method(existing, "set_path_gaps", gaps_now);
		}
		ur->add_do_method(existing, "set_poly_points", local);
		if (!wall) {
			ur->add_do_method(existing, "set_shape_param", "height", height);
		}
		if (sweep_gaps) {
			ur->add_undo_method(existing, "set_path_gaps", poly_edit_gaps_orig);
		}
		ur->add_undo_method(existing, "set_poly_points", poly_edit_orig);
		ur->add_undo_method(existing, "set_transform", poly_edit_xf);
		if (!wall) {
			ur->add_undo_method(existing, "set_shape_param", "height", existing->get_shape_param("height"));
		}
		ur->commit_action();
		_update_shape_panel();
		return;
	}
	HewnMesh3D *node = memnew(HewnMesh3D);
	node->set_name("PolyShape");
	_setup_poly_node(node, pts, height);
	if (texture_dock && texture_dock->current_material.is_valid()) {
		node->apply_material(texture_dock->current_material, PackedInt32Array()); // (the texture picked last)
	}
	_new_object_uv(node);
	_add_node_with_undo(node, "Create Poly Shape");
	if (std::fabs(height) > 1e-5f) {
		_arm_pending("invert", node); // W: faces inward
	}
}

// ------------------------------------------------------------------ drawing

void HewnEditorPlugin::_draw_tools(Control *p_overlay, Camera3D *p_cam) {
	if (modal == MODAL_CUTTER) {
		cutter_op = dock->cutter_op->get_selected();
	}
	Ref<Font> font = p_overlay->get_theme_default_font();
	HewnMesh3D *n = get_edited();
	String status;

	if (modal == MODAL_KNIFE && n) {
		const Transform3D g = n->get_global_transform();
		// Preview of the real cuts on the surface: the ended ones, then the one being drawn (to the mouse)
		std::vector<std::vector<KnifeClick>> all = knife_done;
		all.push_back(knife_points);
		if (knife_hover_valid && !knife_points.empty()) {
			all.back().push_back(knife_hover);
		}
		for (size_t ci = 0; ci < all.size(); ci++) {
			const Color lc = ci + 1 < all.size() ? TOOL_COLOR * Color(1, 1, 1, 0.7f) : TOOL_COLOR;
			// the line as drawn (thin: through empty space too), the cut on the surface over it
			for (size_t i = 0; i + 1 < all[ci].size(); i++) {
				draw_world_line(p_overlay, p_cam, g.xform(all[ci][i].local), g.xform(all[ci][i + 1].local), lc * Color(1, 1, 1, 0.45f), 1.0f);
			}
			std::vector<std::vector<hewn::ops::KnifeHit>> pieces;
			std::vector<Vector3> splits;
			_knife_pieces(p_cam, all[ci], pieces, splits);
			for (const Vector3 &sp : splits) {
				const Vector3 w = g.xform(sp);
				if (!p_cam->is_position_behind(w)) {
					p_overlay->draw_rect(Rect2(p_cam->unproject_position(w) - Vector2(3, 3), Vector2(6, 6)), TOOL_COLOR);
				}
			}
			for (const std::vector<hewn::ops::KnifeHit> &hits : pieces) {
				for (size_t i = 0; i + 1 < hits.size(); i++) {
					draw_world_line(p_overlay, p_cam, g.xform(hits[i].pos), g.xform(hits[i + 1].pos), lc, 2.0f);
				}
				for (const hewn::ops::KnifeHit &h : hits) {
					const Vector3 w = g.xform(h.pos);
					if (!p_cam->is_position_behind(w)) {
						const Vector2 s = p_cam->unproject_position(w);
						p_overlay->draw_rect(Rect2(s - Vector2(3, 3), Vector2(6, 6)), h.face >= 0 ? Color(1, 1, 1) : TOOL_COLOR);
					}
				}
			}
			for (const KnifeClick &c : all[ci]) {
				const Vector3 w = g.xform(c.local);
				if (!p_cam->is_position_behind(w)) {
					p_overlay->draw_circle(p_cam->unproject_position(w), 4.0f, Color(1, 1, 1));
				}
			}
		}
		if (knife_hover_valid) {
			const Vector3 w = g.xform(knife_hover.local);
			if (!p_cam->is_position_behind(w)) {
				p_overlay->draw_arc(p_cam->unproject_position(w), 7.0f, 0.0f, (float)Math::PI * 2.0f, 20, SNAP_COLOR, 2.0f, true);
			}
		}
		status = vformat(T("Knife  %d point(s)"), (int)knife_points.size()) + (knife_done.empty() ? String() : vformat(T(", %d cut(s) ended"), (int)knife_done.size())) +
				T("   (RMB: end this cut, Enter: cut)") + _angle_lock_text();
	} else if (modal == MODAL_LOOPCUT && n) {
		if (loop_edge.a >= 0) {
			const Transform3D g = n->get_global_transform();
			const hewn::Mesh &m = n->get_data();
			std::vector<std::pair<int, int>> ring = hewn::ops::loop_cut_ring(m, loop_edge);
			bool flip = false;
			for (const std::pair<int, int> &e : ring) {
				if (hewn::Edge(e.first, e.second) == loop_edge) {
					flip = e.first != loop_edge.a;
				}
			}
			// Closed if the last ring edge shares a face with the first one.
			bool closed = false;
			if (ring.size() > 2) {
				hewn::Topology t;
				t.build(m);
				const int e0 = t.find_edge(ring.front().first, ring.front().second);
				const int e1 = t.find_edge(ring.back().first, ring.back().second);
				closed = e0 >= 0 && e1 >= 0 && t.shares_face(e0, e1);
			}
			for (int c = 0; c < loop_cuts; c++) {
				const float base = (float)(c + 1) / (loop_cuts + 1);
				const float f = loop_cuts == 1 ? loop_t : base + (loop_t - 0.5f) * 2.0f * 0.98f / (loop_cuts + 1);
				std::vector<Vector3> pts;
				for (const std::pair<int, int> &e : ring) {
					const int p = flip ? e.second : e.first, q = flip ? e.first : e.second;
					pts.push_back(g.xform(m.pos[p].lerp(m.pos[q], f)));
				}
				for (size_t i = 0; i + 1 < pts.size(); i++) {
					draw_world_line(p_overlay, p_cam, pts[i], pts[i + 1], HANDLE_HOVER, 2.0f);
				}
				if (closed && pts.size() > 2) {
					draw_world_line(p_overlay, p_cam, pts.back(), pts.front(), HANDLE_HOVER, 2.0f);
				}
			}
		}
		status = loop_phase == 0 ? vformat(T("Loop Cut  cuts: %d"), loop_cuts)
								 : vformat(T("Loop Cut  cuts: %d  factor: %.3f"), loop_cuts, loop_t * 2.0f - 1.0f);
	} else if (modal == MODAL_CUTTER) {
		const std::vector<Vector2> outline = cutter_state == 0 ? std::vector<Vector2>() : _cutter_outline();
		auto W = [&](const Vector2 &p, float h) { return cutter_origin + cutter_u * p.x + cutter_v * p.y + cutter_n * h; };
		const float h = cutter_state == 3 ? (cutter_op == CUTTER_JOIN || cutter_op == CUTTER_MAKE ? cutter_depth : -cutter_depth) : 0.0f;
		for (size_t i = 0; i < outline.size(); i++) {
			const size_t j = (i + 1) % outline.size();
			if (cutter_state == 2 && j == 0) {
				continue; // polygon still open
			}
			draw_world_line(p_overlay, p_cam, W(outline[i], 0.0f), W(outline[j], 0.0f), TOOL_COLOR, 2.0f);
			if (cutter_state == 3) {
				draw_world_line(p_overlay, p_cam, W(outline[i], h), W(outline[j], h), TOOL_COLOR, 1.5f);
				draw_world_line(p_overlay, p_cam, W(outline[i], 0.0f), W(outline[i], h), TOOL_COLOR * Color(1, 1, 1, 0.6f), 1.0f);
			}
		}
		if (cutter_state == 2 && !cutter_poly.empty()) {
			const Vector3 last = W(cutter_poly.back(), 0.0f);
			if (!p_cam->is_position_behind(last)) {
				p_overlay->draw_line(p_cam->unproject_position(last), mouse_pos, TOOL_COLOR * Color(1, 1, 1, 0.5f), 1.5f, true);
			}
			const Vector3 first = W(cutter_poly[0], 0.0f);
			if (!p_cam->is_position_behind(first)) {
				p_overlay->draw_circle(p_cam->unproject_position(first), 5.0f, TOOL_COLOR);
			}
		}
		status = T("Box Cutter") + vformat("  %s  %s%s", T(CUTTER_OP_NAMES[cutter_op]), T(CUTTER_SHAPE_NAMES[CLAMP(dock->cutter_shape->get_selected(), 0, 2)]), dock->cutter_through->is_pressed() ? T("  through") : String());
		if (cutter_state == 3) {
			status = vformat(T("Box Cutter  depth: %.4f"), cutter_depth);
		}
	} else if (modal == MODAL_SHAPE) {
		_draw_shape_tool(p_overlay, p_cam);
		return;
	} else if (modal == MODAL_PAINT) {
		_draw_paint(p_overlay, p_cam);
		return;
	} else if (modal == MODAL_BISECT) {
		_draw_bisect(p_overlay, p_cam);
		return;
	} else if (modal == MODAL_CLIP) {
		_draw_clip(p_overlay, p_cam);
		return;
	} else if (modal == MODAL_SPIN) {
		_draw_spin(p_overlay, p_cam);
		return;
	} else if (modal == MODAL_FACECUT) {
		_draw_facecut(p_overlay, p_cam);
		return;
	} else if (modal == MODAL_PREFAB) {
		_draw_prefab(p_overlay, p_cam);
		return;
	} else if (modal == MODAL_CURSOR && cursor_xf != 0) {
		if (cursor_xf == 1 && cursor_xf_axis < 0) {
			_draw_snap_mark(p_overlay, p_cam);
		}
		static const char *axes[4] = { "", "  [X]", "  [Y]", "  [Z]" };
		status = (cursor_xf == 1 ? T("Move the 3D cursor: the surface under the mouse") : T("Rotate the 3D cursor")) + String(axes[cursor_xf_axis + 1]) +
				T("   |   X / Y / Z: axis, Ctrl: snapping the other way, click / Enter: done, RMB / Esc: cancel");
	} else if (modal == MODAL_CURSOR) {
		if (Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) {
			_draw_snap_mark(p_overlay, p_cam);
		}
		status = T("3D Cursor: click / drag places it on the surface under the mouse   (RMB / Esc: done)");
	} else if (modal == MODAL_RULER) {
		_draw_rulers(p_overlay, p_cam);
		if (ruler_hover_valid && ruler_hover_item < 0 && !p_cam->is_position_behind(ruler_hover)) {
			// (its symbol: a square on a vertex, an hourglass on an edge, a circle on a face)
			draw_snap_symbol(p_overlay, p_cam->unproject_position(ruler_hover), ruler_hover_kind, SNAP_COLOR);
		}
		status = ruler_line && ruler_rope ? String(T("Rope: click the two points it hangs from (snapping as set)")) :
				ruler_line					 ? String(T("Line: click its two points (snapping as set)")) :
				rulers.empty()	? String(T("Ruler: drag to measure (or click two points)")) :
								  vformat(T("Ruler (%d)"), (int)rulers.size());
		status += T("   |   drag a ruler's middle: angle, its ends: move;  X / Del over a ruler: remove;  Ctrl: free on surfaces;  RMB / Esc: done");
	} else if (modal == MODAL_POLY) {
		PackedVector2Array pts;
		for (const Vector3 &p : poly_points) {
			if (!p_cam->is_position_behind(p)) {
				pts.push_back(p_cam->unproject_position(p));
			}
		}
		const Color c(1, 0.6f, 0.1f);
		_draw_snap_mark(p_overlay, p_cam);
		if (poly_phase == 0 && poly_edit_id == 0) {
			// placement grid on the surface (or the outline's plane) under the cursor
			Vector3 o, nrm, u, v;
			bool ok = false;
			if (poly_has_plane) {
				ok = _plane_hit(p_cam, mouse_pos, poly_origin, poly_n, o);
				nrm = poly_n.normalized();
				_plane_axes(nrm, u, v);
			} else {
				ok = _surface_frame(p_cam, mouse_pos, o, nrm, u, v);
			}
			if (ok) {
				_draw_surface_grid(p_overlay, p_cam, o, u, v);
			}
		}
		if (poly_edit_id != 0) {
			// (a sweep's gaps: thin and gray)
			const int cnt = (int)poly_points.size();
			const int segs = poly_edit_wall && !poly_edit_loop ? cnt - 1 : (cnt >= 3 ? cnt : cnt - 1);
			for (int i = 0; i < segs; i++) {
				const bool gap = i < (int)poly_gaps.size() && poly_gaps[i];
				draw_world_line(p_overlay, p_cam, poly_points[i], poly_points[(i + 1) % cnt], gap ? Color(0.6f, 0.6f, 0.6f, 0.6f) : c, gap ? 1.0f : 2.0f);
			}
		} else {
			for (int i = 0; i + 1 < pts.size(); i++) {
				p_overlay->draw_line(pts[i], pts[i + 1], c, 2.0f, true);
			}
		}
		if (poly_edit_id != 0) {
		} else if (poly_phase == 0 && !pts.is_empty() && poly_drag_point < 0) {
			p_overlay->draw_line(pts[pts.size() - 1], mouse_pos, c * Color(1, 1, 1, 0.5f), 1.5f, true);
			p_overlay->draw_line(mouse_pos, pts[0], c * Color(1, 1, 1, 0.25f), 1.0f, true);
		} else if (pts.size() >= 3) {
			p_overlay->draw_line(pts[pts.size() - 1], pts[0], c, 2.0f, true);
		}
		if (poly_phase == 1) {
			const Vector3 off = poly_n * poly_height_value;
			for (size_t i = 0; i < poly_points.size(); i++) {
				const size_t j = (i + 1) % poly_points.size();
				draw_world_line(p_overlay, p_cam, poly_points[i] + off, poly_points[j] + off, c, 2.0f);
				draw_world_line(p_overlay, p_cam, poly_points[i], poly_points[i] + off, c * Color(1, 1, 1, 0.6f), 1.0f);
			}
			// the edge the height is taken along (from the clicked point): its top is under the mouse
			draw_world_line(p_overlay, p_cam, poly_hbase, poly_hbase + off, c, 2.0f);
			if (!p_cam->is_position_behind(poly_hbase + off)) {
				p_overlay->draw_circle(p_cam->unproject_position(poly_hbase + off), 5.0f, Color(1, 0.75f, 0.2f));
			}
			_draw_length_label(p_overlay, p_cam, poly_hbase, poly_hbase + off);
		}
		for (int i = 0; i < pts.size(); i++) {
			p_overlay->draw_circle(pts[i], i == 0 ? 6.0f : 4.0f, Color(1, 0.75f, 0.2f));
		}
		status = poly_edit_id != 0 ? String(poly_edit_wall ? T("Edit wall points") : T("Edit base")) + T(": drag a point to move it, click it to remove it, click a segment to add one; click outside applies, RMB / Esc cancels") +
						(poly_edit_sweep ? String(T(";  Alt+click a segment: a gap")) : String())
				: poly_phase == 0 ? vformat(T("Poly Shape  %d point(s)"), (int)poly_points.size()) + _angle_lock_text() : vformat(T("Poly Shape  height: %.4f"), poly_height_value);
	}
	if (font.is_valid() && !status.is_empty()) {
		draw_header(p_overlay, font, status);
	}
}

// ------------------------------------------------------------------ ruler

void HewnEditorPlugin::start_ruler() {
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	modal_verts.clear();
	modal_object = false;
	_gather_snap_candidates(false);
	ruler_state = 0;
	ruler_line = false;
	ruler_drag = ruler_drag_pt = -1;
	ruler_hover_valid = false;
	modal = MODAL_RULER;
	update_overlays();
}

// The ruler's last two points as a mesh of its own: one edge (a loose one), its origin at the first point; a rope:
// hung between them (its panel: the slack, the segments...).
void HewnEditorPlugin::_ruler_make_line() {
	if (rulers.empty() || rulers.back().p.size() < 2) {
		return;
	}
	const Vector3 a = rulers.back().p.front(), b = rulers.back().p.back();
	rulers.pop_back();
	ruler_line = false;
	modal = MODAL_NONE;
	ruler_state = 0;
	if (a.distance_to(b) < 1e-5f) {
		ruler_rope = false;
		update_overlays();
		return;
	}
	if (ruler_rope) {
		ruler_rope = false;
		rope_a = a;
		rope_b = b;
		start_scene_op("rope");
		return;
	}
	HewnMesh3D *n = memnew(HewnMesh3D);
	n->set_name("Line");
	n->set_shape(hewn::shapes::CUSTOM);
	hewn::Mesh m;
	m.add_vertex(Vector3());
	m.add_vertex(b - a);
	m.wires.push_back({ 0, 1 });
	n->get_data() = m;
	n->commit(false);
	n->set_transform(Transform3D(Basis(), a));
	_add_node_with_undo(n, "Add Line");
	update_overlays();
}

// Add > Mesh > Vertex: a mesh of one loose vertex at the 3D cursor.
void HewnEditorPlugin::add_vertex_mesh() {
	HewnMesh3D *n = memnew(HewnMesh3D);
	n->set_name("Vertex");
	n->set_shape(hewn::shapes::CUSTOM);
	hewn::Mesh m;
	m.add_vertex(Vector3());
	m.points.push_back(0);
	n->get_data() = m;
	n->commit(false);
	n->set_transform(Transform3D(cursor_basis, cursor_pos));
	_add_node_with_undo(n, "Add Vertex");
}

void HewnEditorPlugin::start_cursor_tool() {
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	modal = MODAL_CURSOR;
	cursor_once = false;
	_gather_snap_candidates(false); // (its snapping targets)
	update_overlays();
}

// Ruler (or its point) under the mouse: r_pt = end / angle vertex, otherwise r_seg = segment index.
void HewnEditorPlugin::_ruler_hit(Camera3D *p_cam, const Vector2 &p_pos, int &r_item, int &r_pt, int &r_seg) const {
	r_item = r_pt = r_seg = -1;
	float best_pt = 9.0f, best_seg = 6.0f;
	for (int i = 0; i < (int)rulers.size(); i++) {
		const std::vector<Vector3> &p = rulers[i].p;
		for (int j = 0; j < (int)p.size(); j++) {
			if (p_cam->is_position_behind(p[j])) {
				continue;
			}
			const float d = p_cam->unproject_position(p[j]).distance_to(p_pos);
			if (d < best_pt) {
				best_pt = d;
				r_item = i;
				r_pt = j;
				r_seg = -1;
			}
		}
	}
	if (r_pt >= 0) {
		return;
	}
	for (int i = 0; i < (int)rulers.size(); i++) {
		const std::vector<Vector3> &p = rulers[i].p;
		for (int j = 0; j + 1 < (int)p.size(); j++) {
			if (p_cam->is_position_behind(p[j]) || p_cam->is_position_behind(p[j + 1])) {
				continue;
			}
			const float d = seg_dist(p_pos, p_cam->unproject_position(p[j]), p_cam->unproject_position(p[j + 1]));
			if (d < best_seg) {
				best_seg = d;
				r_item = i;
				r_seg = j;
			}
		}
	}
}

void HewnEditorPlugin::_draw_rulers(Control *p_overlay, Camera3D *p_cam) {
	Ref<Font> font = p_overlay->get_theme_default_font();
	const int fs = p_overlay->get_theme_default_font_size();
	auto label = [&](const Vector2 &p_at, const String &p_text) {
		if (font.is_valid()) {
			p_overlay->draw_string(font, p_at + Vector2(1, 1), p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color(0, 0, 0, 0.8f));
			p_overlay->draw_string(font, p_at, p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color(1, 1, 1));
		}
	};
	const bool tool = modal == MODAL_RULER;
	for (int i = 0; i < (int)rulers.size(); i++) {
		const std::vector<Vector3> &p = rulers[i].p;
		bool behind = false;
		for (const Vector3 &q : p) {
			behind = behind || p_cam->is_position_behind(q);
		}
		if (behind) {
			continue;
		}
		const bool hot = tool && i == ruler_hover_item;
		const Color lc = hot ? Color(1, 0.85f, 0.4f) : Color(1, 1, 1, tool ? 1.0f : 0.8f);
		std::vector<Vector2> s;
		for (const Vector3 &q : p) {
			s.push_back(p_cam->unproject_position(q));
		}
		for (size_t j = 0; j + 1 < s.size(); j++) {
			p_overlay->draw_line(s[j], s[j + 1], Color(0, 0, 0, 0.6f), hot ? 4.0f : 3.5f, true);
			p_overlay->draw_line(s[j], s[j + 1], lc, hot ? 2.5f : 1.5f, true);
		}
		for (size_t j = 0; j < s.size(); j++) {
			const bool hot_pt = hot && (int)j == ruler_hover_pt;
			p_overlay->draw_circle(s[j], hot_pt ? 5.5f : 4.0f, hot_pt ? HANDLE_HOVER : TOOL_COLOR);
		}
		if (p.size() == 2) {
			const Vector3 d = p[1] - p[0];
			if (tool && d.length() > 1e-6f) {
				// axis components as a colored corner path
				Vector3 q = p[0];
				for (int a = 0; a < 3; a++) {
					const Vector3 q2 = q + AXES[a] * d[a];
					draw_world_line(p_overlay, p_cam, q, q2, AXIS_COLORS[a] * Color(1, 1, 1, 0.7f), 1.0f);
					q = q2;
				}
			}
			label((s[0] + s[1]) * 0.5f + Vector2(8, -8), vformat("%.4f", d.length()));
		} else {
			// protractor: the angle at the middle point, with an arc between the two arms
			const Vector3 a = p[0] - p[1], b = p[2] - p[1];
			float deg = 0.0f;
			if (a.length() > 1e-9f && b.length() > 1e-9f) {
				deg = std::acos(CLAMP(a.normalized().dot(b.normalized()), -1.0f, 1.0f)) * 180.0f / (float)Math::PI;
			}
			const Vector2 da = s[0] - s[1], db = s[2] - s[1];
			if (da.length() > 1.0f && db.length() > 1.0f) {
				const float r = MIN(24.0f, MIN(da.length(), db.length()) * 0.6f);
				const float a0 = da.angle();
				float sweep = Math::wrapf(db.angle() - a0, -(float)Math::PI, (float)Math::PI);
				p_overlay->draw_arc(s[1], r, a0, a0 + sweep, 24, lc, 1.5f, true);
				const Vector2 mid_dir = Vector2(1, 0).rotated(a0 + sweep * 0.5f);
				label(s[1] + mid_dir * (r + 10.0f) + Vector2(-12, 5), vformat("%.2f", deg) + String::chr(0x00B0));
			} else {
				label(s[1] + Vector2(10, -10), vformat("%.2f", deg) + String::chr(0x00B0));
			}
		}
	}
}

// ------------------------------------------------------------------ shape drawing

namespace {

// Shapes that only need a footprint (no height drag).
bool flat_shape(int p_type) {
	return p_type == hewn::shapes::PLANE || p_type == hewn::shapes::DISC || p_type == hewn::shapes::SPHERE || p_type == hewn::shapes::ICOSPHERE ||
			p_type == hewn::shapes::TORUS || p_type == hewn::shapes::ARCH;
}

// Round shapes are drawn from the center out to the radius.
bool round_shape(int p_type) {
	return p_type == hewn::shapes::CYLINDER || p_type == hewn::shapes::CONE || p_type == hewn::shapes::SPHERE || p_type == hewn::shapes::ICOSPHERE ||
			p_type == hewn::shapes::CAPSULE || p_type == hewn::shapes::PIPE || p_type == hewn::shapes::TORUS || p_type == hewn::shapes::DISC ||
			p_type == hewn::shapes::POLYGON || p_type == hewn::shapes::COLUMN;
}

// the center, then a corner of the box around a quarter of the oval (cylinders, pipes, their cut).
bool oval_shape(int p_type) {
	return p_type == hewn::shapes::CYLINDER || p_type == hewn::shapes::PIPE || p_type == hewn::shapes::CONE || p_type == hewn::shapes::CAPSULE;
}

// Shapes whose width follows the longer side of the drawn base (the opening faces across it).
// (none now: the door and the arch keep the drawn footprint and have "opening_on_long_side")
bool long_side_shape(int p_type) {
	return false;
}

// Directions a drawn shape can take inside the same box (R while drawing).
int shape_dir_variants(int p_type) {
	switch (p_type) {
		case hewn::shapes::STAIRS:
			return 4;
		case hewn::shapes::DOOR:
		case hewn::shapes::ARCH:
		case hewn::shapes::PRISM:
			return 2;
		default:
			return 1;
	}
}

bool has_param(int p_type, const char *p_name) {
	for (const hewn::shapes::Param &p : hewn::shapes::params((hewn::shapes::Type)p_type)) {
		if (String(p.name) == p_name) {
			return true;
		}
	}
	return false;
}

} // namespace

void HewnEditorPlugin::start_shape_tool(int p_type) {
	shape_wheel.clear();
	shape_make_area = false;
	shape_make_cutter = false;
	shape_make_body = 0;
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	// in edit mode the shape becomes part of the edited mesh (not for cuts: they work on the meshes)
	HewnMesh3D *into = get_edited();
	shape_into = into && mode != MODE_OBJECT ? into->get_instance_id() : ObjectID();
	shape_into_mode = mode; // (edit mode stays while it is drawn)
	_clear_into();
	_hide_shape_panel();
	_gather_snap_candidates(false);
	shape_cut = 0;
	shape_type = p_type;
	shape_state = 0;
	shape_dir = 0;
	wall_pts.clear();
	grid_hover_valid = false;
	modal = MODAL_SHAPE;
	update_overlays();
}

bool HewnEditorPlugin::_surface_frame(Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_origin, Vector3 &r_n, Vector3 &r_u, Vector3 &r_v) const {
	Vector3 o, d, hit, nrm;
	_ray(p_cam, p_pos, o, d);
	const float ground = cursor_pos.y; // the ground plane goes through the 3D cursor
	HewnMesh3D *hit_node = nullptr;
	surface_hit = ObjectID();
	if (_raycast_scene(o, d, hit, nrm, &hit_node)) {
		surface_hit = hit_node ? hit_node->get_instance_id() : ObjectID();
		r_origin = hit;
		r_n = nrm.normalized();
	} else if (std::fabs(d.y) > 1e-6f && (ground - o.y) / d.y > 0.0f) {
		r_origin = o + d * ((ground - o.y) / d.y);
		r_n = Vector3(0, 1, 0);
	} else {
		return false;
	}
	_plane_axes(r_n, r_u, r_v);
	return true;
}

// New shapes (and the placement grid) turn with the transform orientation: Global keeps world X / Z,
// Local / Cursor / a custom one turns them like their axes.
// The grid drawn on the faces (Alt+G) in a plane: world X along it (else Z), steps from the world origin; the
// points snapped to the grid go onto those lines whatever the gizmo's orientation.
void HewnEditorPlugin::_grid_axes(const Vector3 &p_n, Vector3 &r_u, Vector3 &r_v) const {
	const Vector3 n = p_n.normalized();
	Vector3 u = Vector3(1, 0, 0) - n * n.x;
	if (u.length() < 0.3f) {
		u = Vector3(0, 0, 1) - n * n.z;
	}
	r_u = u.normalized();
	r_v = n.cross(r_u).normalized();
}

void HewnEditorPlugin::_plane_axes(const Vector3 &p_n, Vector3 &r_u, Vector3 &r_v) const {
	const Vector3 n = p_n.normalized();
	const Basis ob = _orientation_basis(dock->orientation, _viewport_camera()).orthonormalized();
	const Vector3 x = ob.get_column(0), z = ob.get_column(2);
	Vector3 u = x - n * n.dot(x);
	if (u.length() < 0.3f) {
		u = z - n * n.dot(z);
	}
	if (u.length() < 1e-6f) {
		Vector3 tmp;
		hewn::plane_basis(n, u, tmp);
	}
	r_u = u.normalized();
	r_v = n.cross(r_u).normalized();
}

bool HewnEditorPlugin::_shape_begin(Camera3D *p_cam, const Vector2 &p_pos) {
	if (ctrl_lock_valid) {
		// Ctrl: on the plane locked when it was pressed, wherever the cursor points
		Vector3 hit;
		if (!_plane_hit(p_cam, p_pos, ctrl_lock_origin, ctrl_lock_n, hit)) {
			return false;
		}
		shape_origin = hit;
		shape_n = ctrl_lock_n;
		shape_u = ctrl_lock_u;
		shape_v = ctrl_lock_v;
	} else if (tool_alt && plane_lock_valid) {
		// Alt: on the plane of the last shape, wherever the cursor points
		Vector3 hit;
		if (!_plane_hit(p_cam, p_pos, plane_lock_origin, plane_lock_n, hit)) {
			return false;
		}
		shape_origin = hit;
		shape_n = plane_lock_n;
		shape_u = plane_lock_u;
		shape_v = plane_lock_v;
	} else if (!_surface_frame(p_cam, p_pos, shape_origin, shape_n, shape_u, shape_v)) {
		return false;
	} else {
		shape_surface = surface_hit;
	}
	const float sm = (float)dock->snap_move->get_value();
	if (snap_increment_on(tool_ctrl) && sm > 0.0f) {
		// onto the grid of the plane (walls too: the grid drawn on them)
		const float a = std::round(shape_origin.dot(shape_u) / sm) * sm, b = std::round(shape_origin.dot(shape_v) / sm) * sm;
		shape_origin = shape_u * a + shape_v * b + shape_n * shape_origin.dot(shape_n);
	}
	return true;
}

bool HewnEditorPlugin::_shape_plane_point(Camera3D *p_cam, const Vector2 &p_pos, Vector2 &r_uv) const {
	Vector3 hit;
	if (!_plane_hit(p_cam, p_pos, shape_origin, shape_n, hit)) {
		return false;
	}
	// Snapping like the transforms: Godot's magnet (Shift+Tab), Ctrl inverts it; the targets of the
	// magnet menu (vertices / edges of the scene meshes, or the increment grid).
	if (snapping_active(tool_ctrl)) {
		Vector3 sp;
		if ((dock->snap_targets & HewnDock::SNAP_POINTS) &&
				_snap_target(p_cam, p_pos, sp) && (sp - hit).length() > 0.0f) {
			hit = sp - shape_n * (sp - shape_origin).dot(shape_n); // onto the drawing plane
			r_uv = Vector2((hit - shape_origin).dot(shape_u), (hit - shape_origin).dot(shape_v));
			last_snap_geom = true;
			return true;
		}
	}
	last_snap_geom = false;
	Vector2 uv((hit - shape_origin).dot(shape_u), (hit - shape_origin).dot(shape_v));
	const float sm = (float)dock->snap_move->get_value();
	if (snap_increment_on(tool_ctrl) && sm > 0.0f) {
		uv = Vector2(std::round(uv.x / sm) * sm, std::round(uv.y / sm) * sm);
	}
	r_uv = uv;
	return true;
}

// Footprint of the drawn base in plane coordinates: a rectangle, or a square around the circle.
void HewnEditorPlugin::_shape_footprint(Vector2 &r_min, Vector2 &r_max) const {
	if (oval_shape(shape_type)) {
		const Vector2 r = (shape_b - shape_a).abs();
		r_min = shape_a - r;
		r_max = shape_a + r;
		return;
	}
	if (round_shape(shape_type)) {
		const float r = shape_a.distance_to(shape_b);
		r_min = shape_a - Vector2(r, r);
		r_max = shape_a + Vector2(r, r);
		return;
	}
	r_min = shape_a.min(shape_b);
	r_max = shape_a.max(shape_b);
}

// Sets up a node for the shape being drawn (the final one, or the live preview). False when nothing
// has been drawn yet.
bool HewnEditorPlugin::_configure_shape_node(HewnMesh3D *p_node, bool p_preview) const {
	Vector2 sa = shape_a, sb = shape_b;
	std::vector<Vector2> pts = wall_pts;
	std::vector<float> hs = wall_hs;
	hs.resize(pts.size(), 0.0f);
	const bool is_wall = shape_type == hewn::shapes::WALL || shape_type == hewn::shapes::SWEEP || shape_type == hewn::shapes::PATH_STAIRS; // (drawn as a path)
	if (is_wall && p_preview && shape_state == 1 && (pts.empty() || pts.back().distance_to(shape_b) > 1e-3f)) {
		pts.push_back(shape_b); // the segment to the cursor
		hs.push_back(wall_cur_h);
	}
	bool raised = false; // (corners with heights: the path keeps them, not set onto the surface)
	for (float h : hs) {
		raised = raised || std::fabs(h) > 1e-6f;
	}
	if (is_wall && pts.size() < 2) {
		return false;
	}
	if (p_preview && shape_state == 3) {
		return false; // (a turned shape's first side: only its line, the shape once its frame is known)
	}
	Vector2 mn = sa.min(sb), mx = sa.max(sb);
	const Vector2 oval = (sb - sa).abs(); // (the radii along X / Z)
	if (oval_shape(shape_type)) {
		mn = sa - oval;
		mx = sa + oval;
		if (p_preview && (oval.x < 1e-3f || oval.y < 1e-3f)) {
			return false;
		}
	} else if (round_shape(shape_type)) {
		const float r = sa.distance_to(sb);
		mn = sa - Vector2(r, r);
		mx = sa + Vector2(r, r);
		if (p_preview && r < 1e-3f) {
			return false;
		}
	} else if (p_preview && !is_wall && (mx.x - mn.x < 1e-3f || mx.y - mn.y < 1e-3f)) {
		return false;
	}
	float W = MAX(0.01f, mx.x - mn.x), D = MAX(0.01f, mx.y - mn.y);
	const float H = flat_shape(shape_type) || shape_state != 2 ? 0.0f : shape_h;
	const float aH = MAX(0.01f, std::fabs(H));
	const float R = round_shape(shape_type) ? MAX(0.005f, sa.distance_to(sb)) : MIN(W, D) * 0.5f;
	// Local X along shape_u, or along the wall line / the longer side of the base.
	Vector3 X = shape_u;
	const bool path_wall = is_wall && pts.size() >= 2; // (a wall stands on a side of its line: built from its points)
	if (is_wall && pts.size() == 2) {
		sa = pts[0];
		sb = pts[1];
	}
	if (is_wall && !path_wall) {
		const Vector2 d = sb - sa;
		X = (shape_u * d.x + shape_v * d.y).normalized();
		W = MAX(0.01f, d.length());
	} else if (long_side_shape(shape_type) && D > W) {
		X = shape_v;
		SWAP(W, D);
	}
	HewnMesh3D *node = p_node;
	node->set_shape(shape_type);
	if (texture_dock && texture_dock->current_material.is_valid() && node->get_materials().size() == 0) {
		node->apply_material(texture_dock->current_material, PackedInt32Array()); // (the texture picked last)
	}
	if (!p_preview) {
		node->set_name(String(hewn::shapes::type_name((hewn::shapes::Type)shape_type)).replace(" ", ""));
	}
	auto setp = [&](const char *name, float v) {
		if (has_param(shape_type, name)) {
			node->set_shape_param(name, v);
		}
	};
	auto defp = [&](const char *name) {
		for (const hewn::shapes::Param &p : hewn::shapes::params((hewn::shapes::Type)shape_type)) {
			if (String(p.name) == name) {
				return p.def;
			}
		}
		return 0.0f;
	};
	switch (shape_type) {
		case hewn::shapes::TORUS:
			setp("tube_radius", R * 0.25f);
			setp("radius", R * 0.75f);
			break;
		case hewn::shapes::CURVED_STAIRS:
			setp("inner_radius", R * 0.3f);
			setp("width", R * 0.7f);
			setp("height", aH);
			break;
		case hewn::shapes::ARCH:
			setp("radius", W * 0.5f);
			setp("thickness", MIN(W * 0.5f * 0.3f, defp("thickness")));
			setp("depth", D);
			break;
		case hewn::shapes::PIPE:
			setp("radius", MAX(0.005f, oval.x));
			setp("radius_z", MAX(0.005f, oval.y));
			setp("thickness", MIN(MIN(oval.x, oval.y) * 0.5f, defp("thickness")));
			setp("height", aH);
			break;
		case hewn::shapes::CYLINDER:
			setp("radius", MAX(0.005f, oval.x));
			setp("radius_z", MAX(0.005f, oval.y));
			setp("height", aH);
			break;
		case hewn::shapes::CONE:
			setp("radius", MAX(0.005f, oval.x));
			setp("radius_z", std::fabs(oval.x - oval.y) < 1e-4f ? 0.0f : MAX(0.005f, oval.y));
			setp("height", aH);
			break;
		case hewn::shapes::CAPSULE:
			setp("radius", MAX(0.005f, oval.x));
			setp("radius_z", std::fabs(oval.x - oval.y) < 1e-4f ? 0.0f : MAX(0.005f, oval.y));
			setp("height", MAX(aH, MAX(oval.x, oval.y) * 2.0f + 0.01f));
			break;
		case hewn::shapes::DOOR:
			setp("width", W);
			setp("depth", D);
			setp("height", aH);
			setp("opening_width", MAX(W, D) * 0.5f);
			setp("opening_height", aH * 0.8f);
			setp("arch_height", MAX(W, D) * 0.25f);
			break;
		case hewn::shapes::WALL:
			setp("length", W);
			setp("side", (float)wall_side);
			break;
		case hewn::shapes::SWEEP:
		case hewn::shapes::PATH_STAIRS:
			break; // (drawn by their points: the width, the profile and the heights stay the panel's)
		default:
			if (round_shape(shape_type)) {
				setp("radius", R);
			} else {
				setp("width", W);
				setp("depth", D);
			}
			setp("height", aH);
			break;
	}
	// the steps / sides set with the mouse wheel while it is drawn
	for (const KeyValue<String, float> &kv : shape_wheel) {
		setp(kv.key.utf8().get_data(), kv.value);
	}
	// R while drawing: the direction inside the same box
	const int k = shape_dir % MAX(1, shape_dir_variants(shape_type));
	if (shape_type == hewn::shapes::STAIRS) {
		setp("climb_along_long_side", k % 2 == 0 ? 1.0f : 0.0f);
		setp("reverse", k >= 2 ? 1.0f : 0.0f);
	} else if (shape_type == hewn::shapes::DOOR || shape_type == hewn::shapes::ARCH) {
		setp("opening_on_long_side", k == 0 ? 1.0f : 0.0f);
	} else if (shape_type == hewn::shapes::PRISM) {
		setp("ridge_along_long_side", k == 0 ? 1.0f : 0.0f);
	}
	// Local Y along the surface normal; the shape sits on the surface (or hangs below for a negative height).
	Basis b;
	b.set_column(0, X);
	b.set_column(1, shape_n);
	b.set_column(2, X.cross(shape_n).normalized());
	Vector2 c = is_wall ? (sa + sb) * 0.5f : round_shape(shape_type) ? sa : (mn + mx) * 0.5f;
	if (path_wall) {
		// corners in local space around their center (local Z = -v)
		c = Vector2();
		for (const Vector2 &p : pts) {
			c += p;
		}
		c /= (float)pts.size();
		PackedVector3Array local;
		for (size_t i = 0; i < pts.size(); i++) {
			local.push_back(Vector3(pts[i].x - c.x, hs[i], -(pts[i].y - c.y)));
		}
		node->set_poly_points(local);
	} else if (is_wall && !node->get_poly_points().is_empty()) {
		node->set_poly_points(PackedVector3Array()); // (a reused preview) back to the straight wall
	}
	Vector3 pos = shape_origin + shape_u * c.x + shape_v * c.y;
	float lift = 0.0f;
	if (shape_type != hewn::shapes::PLANE && shape_type != hewn::shapes::DISC && !(path_wall && raised)) {
		float min_y = 0.0f;
		for (const Vector3 &p : node->get_data().pos) {
			min_y = MIN(min_y, p.y);
		}
		lift = -min_y;
		if (H < 0.0f) {
			lift = min_y; // drawn downwards: hang below the surface
		}
	}
	pos += shape_n * lift;
	node->set_transform(Transform3D(b, pos));
	_new_object_uv(node);
	return true;
}

// World space projection at the pixels per meter of the Create tab: the materials made in the Textures panel
// show their image at its own pixels per meter (1 tile = 1 m there), so the faces scale by the ratio.
float HewnEditorPlugin::_world_uv_scale() const {
	const float ref = texture_dock && texture_dock->pixels_per_meter() > 0.0f ? texture_dock->pixels_per_meter() : 128.0f;
	const float px = dock && dock->new_uv_pixels ? (float)dock->new_uv_pixels->get_value() : 128.0f;
	return px / ref;
}

void HewnEditorPlugin::_new_object_uv(HewnMesh3D *p_node) const {
	if (!p_node || !dock || !dock->new_uv_world || !dock->new_uv_world->is_pressed()) {
		return;
	}
	p_node->set_world_uv_all(_world_uv_scale());
}

// Existing faces / objects as new ones are made: projected from world space (their own UVs dropped).
// Align to Face: the selected faces' world UVs projected onto their own plane (a slope shows the texture true to
// size, not stretched as seen along an axis; its level lines stay level). Align to Axis: back along the nearest axis
// (the faces meet without seams again). Faces not in world UVs are put in them.
void HewnEditorPlugin::align_world_uv(bool p_to_face) {
	HewnMesh3D *n = get_edited();
	if (!n || mode == MODE_OBJECT) {
		ERR_PRINT("Hewn: select the faces to align in edit mode first.");
		return;
	}
	std::vector<int> faces = n->selection.faces;
	if (faces.empty()) {
		faces = hewn::sel::faces_from_verts(n->get_data(), _selected_verts(), true);
	}
	if (faces.empty()) {
		ERR_PRINT("Hewn: select the faces to align first.");
		return;
	}
	const float sc = _world_uv_scale();
	begin_edit(p_to_face ? "Align to Face" : "Align to Axis");
	hewn::Mesh &m = n->get_data();
	for (int f : faces) {
		if (f < 0 || f >= (int)m.faces.size()) {
			continue;
		}
		hewn::Face &F = m.faces[f];
		if (F.manual_uv || !F.uvp.world) {
			F.manual_uv = false;
			F.uv_group = 0;
			F.uvp = hewn::UVParams();
			F.uvp.world = true;
			F.uvp.scale = Vector2(sc, sc);
		}
		F.uv_group = 0; // (each face by its own plane)
		F.uvp.face_aligned = p_to_face;
	}
	n->commit(false);
	end_edit();
}

void HewnEditorPlugin::apply_world_uv() {
	const float sc = _world_uv_scale();
	HewnMesh3D *n = get_edited();
	if (n && mode != MODE_OBJECT) {
		std::vector<int> faces = n->selection.faces;
		if (faces.empty()) {
			faces = hewn::sel::faces_from_verts(n->get_data(), _selected_verts(), true);
		}
		if (faces.empty()) {
			ERR_PRINT("Hewn: select the faces to put in World UV first.");
			return;
		}
		begin_edit("World UV");
		hewn::Mesh &m = n->get_data();
		for (int f : faces) {
			if (f < 0 || f >= (int)m.faces.size()) {
				continue;
			}
			hewn::Face &F = m.faces[f];
			F.manual_uv = false;
			F.uv_group = 0;
			F.uvp = hewn::UVParams();
			F.uvp.world = true;
			F.uvp.scale = Vector2(sc, sc);
		}
		n->commit(false);
		end_edit();
		return;
	}
	std::vector<HewnMesh3D *> meshes;
	for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
		if (HewnMesh3D *x = Object::cast_to<HewnMesh3D>(v)) {
			meshes.push_back(x);
		}
	}
	if (meshes.empty()) {
		ERR_PRINT("Hewn: select the objects (or faces in edit mode) to put in World UV first.");
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: World UV", UndoRedo::MERGE_DISABLE, meshes[0]);
	for (HewnMesh3D *x : meshes) {
		const Dictionary before = x->get_state();
		hewn::Mesh &m = x->get_data();
		for (hewn::Face &F : m.faces) {
			F.manual_uv = false;
			F.uv_group = 0;
		}
		const bool was_anchored = x->is_uv_anchored();
		const Transform3D was_anchor = x->get_uv_anchor();
		x->set_uv_anchored(false); // (from where it is now)
		x->set_world_uv_all(sc);
		ur->add_do_method(x, "set_uv_anchored", false);
		ur->add_do_method(x, "set_state", x->get_state());
		ur->add_undo_method(x, "set_state", before);
		if (was_anchored) {
			ur->add_undo_method(x, "set_uv_anchor", was_anchor);
		}
	}
	ur->commit_action(false);
}

void HewnEditorPlugin::_finish_shape() {
	_remove_shape_preview();
	if (shape_cut) {
		_apply_shape_cut();
		return;
	}
	HewnMesh3D *node = memnew(HewnMesh3D);
	_configure_shape_node(node, false);
	modal = MODAL_NONE;
	shape_state = 0;
	wall_pts.clear();
	shape_fixed_side = false;
	if (shape_make_area && shape_make_cutter) {
		// Add > Cutter: the drawn box as a cutter
		shape_make_area = shape_make_cutter = false;
		shape_into = ObjectID();
		shape_surface = ObjectID();
		HewnCutter3D *c = memnew(HewnCutter3D);
		c->set_name("Cutter");
		c->set_size(Vector3(node->get_shape_param("width"), node->get_shape_param("height"), node->get_shape_param("depth")));
		c->set_transform(node->get_transform());
		memdelete(node);
		_add_node_with_undo(c, "Add Cutter");
		_hide_redo();
		update_overlays();
		return;
	}
	if (shape_make_area) {
		// Add > Physics: the drawn box as the shape of an area / a static body / alone
		shape_make_area = false;
		shape_into = ObjectID();
		shape_surface = ObjectID();
		CollisionShape3D *cs = memnew(CollisionShape3D);
		cs->set_name("CollisionShape3D");
		Ref<BoxShape3D> box;
		box.instantiate();
		box->set_size(Vector3(node->get_shape_param("width"), node->get_shape_param("height"), node->get_shape_param("depth")));
		cs->set_shape(box);
		Node3D *top = cs;
		if (shape_make_body < 2) {
			Node3D *a = shape_make_body == 0 ? (Node3D *)memnew(Area3D) : (Node3D *)memnew(StaticBody3D);
			a->set_name(shape_make_body == 0 ? "Area3D" : "StaticBody3D");
			a->add_child(cs);
			top = a;
		}
		top->set_transform(node->get_transform());
		memdelete(node);
		_add_node_with_undo(top, shape_make_body == 0 ? "Add Area3D" : shape_make_body == 1 ? "Add Static Body" : "Add Collision Shape");
		shape_make_body = 0;
		_hide_redo();
		update_overlays();
		return;
	}
	if (HewnMesh3D *into = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(shape_into))) {
		shape_into = ObjectID();
		shape_surface = ObjectID();
		const bool flat = flat_shape(shape_type);
		_build_into(into, node);
		if (!flat) {
			_arm_pending("invert_into", into); // W: the new part faces inward
		}
		_hide_redo();
		update_overlays();
		return;
	}
	// drawn on a CSG model (or one of its brushes): a new brush of it
	HewnMesh3D *on = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(shape_surface));
	HewnMesh3D *model = on ? (on->is_csg_model() ? on : on->get_csg_model()) : nullptr;
	shape_surface = ObjectID();
	_add_node_with_undo(node, String("Create ") + hewn::shapes::type_name((hewn::shapes::Type)shape_type), model);
	if (!flat_shape(shape_type)) {
		_arm_pending("invert", node); // W: the box becomes a room (faces inward)
	}
	_hide_redo();
	_update_shape_panel(); // its parameters right away
	update_overlays();
}

// a shape drawn in edit mode joins the edited mesh; faces lying on its faces the other way round
// are cut away from both (unless Keep Overlap Faces), so boxes grow together and rooms open into each
// other. The new faces end up selected. Its Add panel shows until something else happens.
void HewnEditorPlugin::_build_into(HewnMesh3D *p_target, HewnMesh3D *p_shape) {
	const Dictionary before = p_target->get_state();
	if (mode == MODE_OBJECT) {
		set_mode(shape_into_mode == MODE_OBJECT ? MODE_FACE : shape_into_mode);
	}
	_merge_into(p_target, p_shape);
	const Dictionary after = p_target->get_state();
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Build into Mesh", UndoRedo::MERGE_DISABLE, p_target);
	ur->add_do_method(p_target, "set_state", after);
	ur->add_undo_method(p_target, "set_state", before);
	ur->commit_action(false);
	selection_changed();
	_clear_into();
	into_shape = p_shape;
	into_target = p_target->get_instance_id();
	into_before = before;
	into_after = after; // (shared with the history step)
	UndoRedo *h = ur->get_history_undo_redo(ur->get_object_history_id(p_target));
	into_version = h ? (int64_t)h->get_version() : -1;
	_update_shape_panel();
}

bool HewnEditorPlugin::_into_panel_valid() {
	HewnMesh3D *t = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(into_target));
	if (!into_shape || !t || t != get_edited() || mode == MODE_OBJECT) {
		return false;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	UndoRedo *h = ur->get_history_undo_redo(ur->get_object_history_id(t));
	return h && (int64_t)h->get_version() == into_version;
}

void HewnEditorPlugin::_clear_into() {
	if (into_shape) {
		memdelete(into_shape);
		into_shape = nullptr;
	}
	into_target = ObjectID();
	into_before = Dictionary();
	into_after = Dictionary();
}

// The Add panel of a shape built into the mesh: the mesh as before it, built with the new shape.
void HewnEditorPlugin::_rebuild_into() {
	HewnMesh3D *t = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(into_target));
	if (!_into_panel_valid() || !t) {
		return;
	}
	t->set_state(into_before);
	_merge_into(t, into_shape);
	const Dictionary now = t->get_state();
	into_after.clear();
	into_after.merge(now);
	selection_changed();
	update_overlays();
}

// Adds the shape's faces to the mesh (they get selected); returns the first new face.
int HewnEditorPlugin::_merge_into(HewnMesh3D *p_target, HewnMesh3D *p_shape) {
	const Transform3D world = p_shape->get_transform(); // (not in the tree: its transform is the world one)
	hewn::Mesh add = p_shape->get_data();
	hewn::ops::transform(add, p_target->get_global_transform().affine_inverse() * world);
	hewn::Mesh &m = p_target->get_data();
	// the new faces: the mesh's most used material
	std::unordered_map<int, int> count;
	int mat = 0, best = -1;
	for (const hewn::Face &F : m.faces) {
		if (++count[F.material] > best) {
			best = count[F.material];
			mat = F.material;
		}
	}
	for (hewn::Face &F : add.faces) {
		F.material = mat;
	}
	if (!(dock && dock->keep_overlap && dock->keep_overlap->is_pressed())) {
		hewn::ops::merge_touching(m, add, 1e-3f);
	}
	const int first = (int)m.faces.size();
	hewn::ops::append(m, add, Transform3D(), 0);
	hewn::weld_by_distance(m, 1e-4f);
	hewn::remove_unused_vertices(m);
	hewn::Selection &s = p_target->selection;
	s.clear();
	for (int f = first; f < (int)m.faces.size(); f++) {
		s.faces.push_back(f);
	}
	if (mode == MODE_VERTEX) {
		s.verts = hewn::sel::verts_of_faces(m, s.faces);
	} else if (mode == MODE_EDGE) {
		s.edges = hewn::sel::edges_of_faces(m, s.faces);
	}
	p_target->commit(true);
	return first;
}

// The shape appears in the scene while it is drawn: an internal child of the edited scene root (not
// saved, not in the scene tree dock, ignored by picking and snapping).
void HewnEditorPlugin::_update_shape_preview() {
	HewnMesh3D *pv = shape_preview_id ? Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(shape_preview_id)) : nullptr;
	if (modal != MODAL_SHAPE || shape_state == 0 || shape_cut) {
		_remove_shape_preview();
		return;
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return;
	}
	const bool fresh = pv == nullptr;
	if (fresh) {
		pv = memnew(HewnMesh3D);
		pv->set_name("HewnPreview");
	}
	if (!_configure_shape_node(pv, true)) {
		if (fresh) {
			memdelete(pv);
		} else {
			pv->set_visible(false);
		}
		return;
	}
	if (fresh && shape_make_area) {
		// a cutter / an Area3D being drawn: its own see-through look, not the texture of meshes
		Ref<StandardMaterial3D> gm;
		gm.instantiate();
		gm->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		gm->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		gm->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		gm->set_albedo(shape_make_cutter ? Color(1.0f, 0.3f, 0.2f, 0.25f) : Color(0.3f, 0.7f, 1.0f, 0.25f));
		pv->set_material_override(gm);
	}
	const Transform3D xf = pv->get_transform();
	if (fresh) {
		root->add_child(pv, false, Node::INTERNAL_MODE_BACK);
		shape_preview_id = pv->get_instance_id();
	}
	pv->set_visible(true);
	pv->set_global_transform(xf);
	_view_new_mesh(pv); // (Single Color...: the preview shows it too, not the texture)
}

void HewnEditorPlugin::_remove_shape_preview() {
	HewnMesh3D *pv = shape_preview_id ? Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(shape_preview_id)) : nullptr;
	shape_preview_id = 0;
	if (pv) {
		if (pv->get_parent()) {
			pv->get_parent()->remove_child(pv);
		}
		pv->queue_free();
	}
}

int32_t HewnEditorPlugin::_shape_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	const int32_t r = _shape_input_impl(p_cam, p_event);
	_update_shape_preview();
	return r;
}

int32_t HewnEditorPlugin::_shape_input_impl(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	const Key code = k.is_valid() && k->is_pressed() && !k->is_echo() ? key_code(k) : KEY_NONE;
	const bool wall = shape_type == hewn::shapes::WALL || shape_type == hewn::shapes::SWEEP || shape_type == hewn::shapes::PATH_STAIRS;
	if (mb.is_valid() && mb->is_pressed() && (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP || mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN) &&
			!mb->is_ctrl_pressed() && !mb->is_shift_pressed() && !mb->is_alt_pressed() && shape_state >= 1) {
		// while it is drawn, the wheel sets the steps of stairs / the sides of round shapes (not the zoom)
		const char *key = has_param(shape_type, "steps") ? "steps" : has_param(shape_type, "sides") ? "sides" : has_param(shape_type, "segments") ? "segments" : nullptr;
		if (key) {
			float cur = 0.0f, lo = 1.0f, hi = 256.0f;
			for (const hewn::shapes::Param &p : hewn::shapes::params((hewn::shapes::Type)shape_type)) {
				if (String(p.name) == key) {
					cur = p.def;
					lo = p.min;
					hi = p.max;
				}
			}
			if (shape_wheel.has(key)) {
				cur = shape_wheel[key];
			}
			shape_wheel[key] = CLAMP(cur + (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? 1.0f : -1.0f), lo, hi);
			_update_shape_preview();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
	}
	if (k.is_valid()) {
		tool_ctrl = ctrl_after(k);
		tool_alt = key_code(k) == KEY_ALT ? k->is_pressed() : k->is_alt_pressed();
	} else if (mm.is_valid()) {
		tool_ctrl = mm->is_ctrl_pressed() || mm->is_meta_pressed();
		tool_alt = mm->is_alt_pressed();
	} else if (mb.is_valid()) {
		tool_ctrl = mb->is_ctrl_pressed() || mb->is_meta_pressed();
		tool_alt = mb->is_alt_pressed();
	}
	// Ctrl pressed before the first click locks the plane of the surface under the cursor, so a
	// shape can start past the face's edges. Held for that, it does not invert snapping.
	if (!tool_ctrl) {
		ctrl_lock_valid = false;
	} else if (shape_state == 0 && !ctrl_lock_valid) {
		ctrl_lock_valid = _surface_frame(p_cam, mouse_pos, ctrl_lock_origin, ctrl_lock_n, ctrl_lock_u, ctrl_lock_v);
	}
	if (ctrl_lock_valid) {
		tool_ctrl = false;
	}
	if (wall && shape_state == 1) {
		// Wall corners: every click adds one; Enter, double click or RMB finishes, Esc cancels.
		const bool finish = code == KEY_ENTER || code == KEY_KP_ENTER || (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT) ||
				(mb.is_valid() && mb->is_pressed() && mb->is_double_click() && mb->get_button_index() == MOUSE_BUTTON_LEFT);
		if (finish) {
			if (wall_pts.size() >= 2) {
				_finish_shape();
			} else {
				shape_state = 0;
				wall_pts.clear();
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_ESCAPE) {
			shape_state = 0;
			wall_pts.clear();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (code == KEY_BACKSPACE && wall_pts.size() > 1) {
			wall_pts.pop_back();
			wall_hs.resize(wall_pts.size());
			wall_cur_h = wall_hs.back();
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		const Vector3 corner = shape_origin + shape_u * shape_b.x + shape_v * shape_b.y;
		if (code == KEY_H && !k->is_ctrl_pressed() && !k->is_alt_pressed()) {
			// H: the next corner stays where it is, the mouse sets its height (H again or a click: done)
			wall_h_mode = !wall_h_mode;
			if (wall_h_mode) {
				wall_h_t0 = _axis_param(p_cam, mouse_pos, corner, shape_n) - wall_cur_h;
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (wall_h_mode && mm.is_valid()) {
			wall_cur_h = _axis_param(p_cam, mouse_pos, corner, shape_n) - wall_h_t0;
			_snap_height(p_cam, corner, shape_n, wall_cur_h);
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
			// a press adds a corner; so does releasing a drag that started at the first point
			if ((mb->is_pressed() || (wall_pts.size() == 1 && mb->get_position().distance_to(shape_press) > 6.0f)) &&
					wall_pts.back().distance_to(shape_b) > 1e-3f) {
				wall_hs.resize(wall_pts.size(), 0.0f);
				wall_pts.push_back(shape_b);
				wall_hs.push_back(wall_cur_h); // (the next one starts at this height)
			}
			wall_h_mode = false;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
	}
	if ((code == KEY_R || code == KEY_E || code == KEY_Q) && !k->is_ctrl_pressed() && !k->is_alt_pressed() && !k->is_shift_pressed() &&
			shape_dir_variants(shape_type) > 1) {
		// turn the direction, the box stays
		const int nv = shape_dir_variants(shape_type);
		shape_dir = (shape_dir + (code == KEY_Q ? nv - 1 : 1)) % nv;
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (code == KEY_ESCAPE || (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT)) {
		if (shape_state == 2 || shape_state == 3 || (wall && shape_state == 1) || (shape_state == 1 && shape_fixed_side)) {
			shape_state = 0; // back to drawing the footprint
			shape_fixed_side = false;
		} else {
			cancel_tool();
		}
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if ((code == KEY_ENTER || code == KEY_KP_ENTER) && shape_state == 0) {
		// Enter without drawing: default size at the 3D cursor.
		modal = MODAL_NONE;
		create_shape(shape_type);
		return AFTER_GUI_INPUT_STOP;
	}
	if (shape_state != 2) {
		shape_num = String();
	} else if (code != KEY_NONE && !k->is_ctrl_pressed() && !k->is_alt_pressed()) {
		// the height typed: digits, a point or comma, minus (the other way), Backspace
		bool typed = false;
		if (code == KEY_BACKSPACE && !shape_num.is_empty()) {
			shape_num = shape_num.substr(0, shape_num.length() - 1);
			typed = true;
		} else if ((code >= KEY_0 && code <= KEY_9) || (code >= KEY_KP_0 && code <= KEY_KP_9)) {
			shape_num += String::chr('0' + (code >= KEY_KP_0 ? code - KEY_KP_0 : code - KEY_0));
			typed = true;
		} else if ((code == KEY_PERIOD || code == KEY_KP_PERIOD || code == KEY_COMMA) && !shape_num.contains(".")) {
			shape_num += ".";
			typed = true;
		} else if (code == KEY_MINUS || code == KEY_KP_SUBTRACT) {
			shape_num = shape_num.begins_with("-") ? shape_num.substr(1) : "-" + shape_num;
			typed = true;
		}
		if (typed) {
			double v = 0.0;
			if (!shape_num.is_empty() && shape_num != "-" && HewnDragField::eval_expression(shape_num, v)) {
				shape_h = (float)v;
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
	}
	if ((code == KEY_ENTER || code == KEY_KP_ENTER) && shape_state == 2) {
		_finish_shape();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mm.is_valid()) {
		snap_mark_valid = false;
		if (shape_state == 0) {
			grid_hover_valid = _surface_frame(p_cam, mouse_pos, grid_hover, grid_hover_n, grid_hover_u, grid_hover_v);
			if (ctrl_lock_valid) {
				grid_hover_valid = _plane_hit(p_cam, mouse_pos, ctrl_lock_origin, ctrl_lock_n, grid_hover);
				grid_hover_n = ctrl_lock_n;
				grid_hover_u = ctrl_lock_u;
				grid_hover_v = ctrl_lock_v;
			} else if (tool_alt && plane_lock_valid && _plane_hit(p_cam, mouse_pos, plane_lock_origin, plane_lock_n, grid_hover)) {
				grid_hover_n = plane_lock_n;
				grid_hover_u = plane_lock_u;
				grid_hover_v = plane_lock_v;
				grid_hover_valid = true;
			}
			// where a press would start, when snapping is on
			Vector2 uv;
			if (snapping_active(tool_ctrl) && _shape_begin(p_cam, mouse_pos) && _shape_plane_point(p_cam, mouse_pos, uv)) {
				snap_mark = shape_origin + shape_u * uv.x + shape_v * uv.y;
				snap_mark_geom = last_snap_geom;
				snap_mark_kind = last_snap_kind;
				snap_mark_valid = true;
			}
		} else if (shape_state == 1 || shape_state == 3) {
			Vector2 uv;
			if (_shape_plane_point(p_cam, mouse_pos, uv)) {
				shape_b = shape_fixed_side ? Vector2(shape_side_len, uv.y) : uv;
				snap_mark = shape_origin + shape_u * uv.x + shape_v * uv.y;
				snap_mark_geom = last_snap_geom;
				snap_mark_kind = last_snap_kind;
				snap_mark_valid = snapping_active(tool_ctrl);
				if (shape_state == 1 && mm->is_alt_pressed() && oval_shape(shape_type)) {
					const Vector2 d = shape_b - shape_a; // Alt: a circle (Shift: turned, like a box)
					if (shape_fixed_side) {
						shape_b.y = d.y < 0 ? -shape_side_len : shape_side_len;
					} else {
						const float s = MAX(std::fabs(d.x), std::fabs(d.y));
						shape_b = shape_a + Vector2(d.x < 0 ? -s : s, d.y < 0 ? -s : s);
					}
				}
				if (shape_state == 1 && !shape_fixed_side && mm->is_shift_pressed() && !round_shape(shape_type) && !wall) {
					const Vector2 d = shape_b - shape_a;
					const float s = MAX(std::fabs(d.x), std::fabs(d.y));
					shape_b = shape_a + Vector2(d.x < 0 ? -s : s, d.y < 0 ? -s : s);
				}
			}
		} else if (shape_state == 2 && shape_num.is_empty()) { // (a typed height stays)
			// along the edge at the last clicked corner: it is under the mouse, so the top follows it
			// one to one (through the middle of a box drawn away from the camera it lagged behind)
			const Vector3 base = shape_origin + shape_u * shape_b.x + shape_v * shape_b.y;
			shape_h = _axis_param(p_cam, mouse_pos, base, shape_n) - shape_t0;
			_snap_height(p_cam, base, shape_n, shape_h); // to a vertex / edge height or grid steps
		}
		update_overlays();
		return shape_state == 0 ? AFTER_GUI_INPUT_PASS : AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		// Clicks: a corner, the opposite corner, the height (a drag from the first corner works too).
		const bool dragged = mb->get_position().distance_to(shape_press) > 6.0f;
		auto base_done = [&]() {
			const Vector2 d = shape_b - shape_a;
			const bool empty = round_shape(shape_type) && !oval_shape(shape_type) ? d.length() < 1e-3f : (std::fabs(d.x) < 1e-3f || std::fabs(d.y) < 1e-3f);
			if (empty) {
				return false; // (keep drawing)
			}
			if (flat_shape(shape_type)) {
				_finish_shape();
				return true;
			}
			if (shape_cut && p_cam->get_projection() == Camera3D::PROJECTION_ORTHOGONAL) {
				cut_infinite = true;
				_finish_shape();
				return true;
			}
			const Vector3 base = shape_origin + shape_u * shape_b.x + shape_v * shape_b.y;
			shape_h = 0.0f;
			// the top is where the mouse is along that edge (no offset from the click: with snapping the
			// corner is not under the click, and the box would lag by that much)
			shape_t0 = 0.0f;
			shape_state = 2;
			return false;
		};

		if (mb->is_pressed() && shape_state == 0) {
			if (_shape_begin(p_cam, mb->get_position())) {
				shape_a = shape_b = Vector2();
				_shape_plane_point(p_cam, mb->get_position(), shape_a);
				shape_b = shape_a;
				shape_fixed_side = false;
				// Shift: a turned box - its first side, then the other side, then the height
				// (an oval too: its center, then a radius along a direction, then the other radius)
				shape_state = mb->is_shift_pressed() && !wall && (!round_shape(shape_type) || oval_shape(shape_type)) ? 3 : 1;
				shape_press = mb->get_position();
				wall_pts = { shape_a };
				wall_hs = { 0.0f };
				wall_cur_h = 0.0f;
				wall_h_mode = false;
				plane_lock_valid = true;
				plane_lock_origin = shape_origin;
				plane_lock_n = shape_n;
				plane_lock_u = shape_u;
				plane_lock_v = shape_v;
			}
		} else if (shape_state == 3 && (mb->is_pressed() || dragged)) {
			const Vector2 d = shape_b - shape_a;
			if (d.length() > 1e-3f) {
				// the drawn side becomes the X axis of the shape
				const Vector3 start = shape_origin + shape_u * shape_a.x + shape_v * shape_a.y;
				const Vector3 dir = (shape_u * d.x + shape_v * d.y).normalized();
				shape_origin = start;
				shape_u = dir;
				shape_v = shape_n.cross(dir).normalized();
				shape_side_len = d.length();
				shape_a = Vector2();
				shape_b = Vector2(shape_side_len, 0.0f);
				shape_fixed_side = true;
				shape_press = mb->get_position();
				shape_state = 1;
			}
		} else if (shape_state == 1 && (mb->is_pressed() || dragged)) {
			if (base_done()) {
				return AFTER_GUI_INPUT_STOP;
			}
		} else if (mb->is_pressed() && shape_state == 2) {
			_finish_shape();
			return AFTER_GUI_INPUT_STOP;
		}
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	return AFTER_GUI_INPUT_PASS;
}

// placement grid: a small grid around the cursor, lying on the surface under it. Its cells are
// the snap step ([ / ]); only when they would be a few pixels wide do they double.
void HewnEditorPlugin::_draw_surface_grid(Control *p_overlay, Camera3D *p_cam, const Vector3 &p_center, const Vector3 &p_u, const Vector3 &p_v) {
	float step = (float)dock->snap_move->get_value();
	const float wpp = MAX(_world_per_pixel(p_cam, p_center), 1e-6f);
	if (step <= 0.0f) {
		step = 0.25f;
	}
	while (step / wpp < 5.0f) {
		step *= 2.0f;
	}
	const int n = CLAMP((int)std::ceil(110.0f * wpp / step), 2, 24); // about the same size on the screen
	const float ext = step * n;
	// lines of the plane's own grid (world aligned on the ground), faded out in a disc around the cursor
	const float cu = std::floor(p_center.dot(p_u) / step) * step - p_center.dot(p_u);
	const float cv = std::floor(p_center.dot(p_v) / step) * step - p_center.dot(p_v);
	for (int i = -n; i <= n + 1; i++) {
		for (int axis = 0; axis < 2; axis++) {
			const float o = (axis == 0 ? cu : cv) + i * step;
			const float fade = 1.0f - MIN(1.0f, std::fabs(o) / ext);
			if (fade <= 0.0f) {
				continue;
			}
			const float half = ext * std::sqrt(MAX(0.0f, 1.0f - (o / ext) * (o / ext)));
			const Vector3 dir = axis == 0 ? p_v : p_u;
			const Vector3 off = (axis == 0 ? p_u : p_v) * o;
			const Vector3 a = p_center + off - dir * half, b = p_center + off + dir * half;
			if (p_cam->is_position_behind(a) || p_cam->is_position_behind(b)) {
				continue;
			}
			p_overlay->draw_line(p_cam->unproject_position(a), p_cam->unproject_position(b), Color(1, 1, 1, 0.35f * fade), 1.0f, true);
		}
	}
}

// The length of a segment next to its middle.
void HewnEditorPlugin::_draw_length_label(Control *p_overlay, Camera3D *p_cam, const Vector3 &p_a, const Vector3 &p_b) {
	const Vector3 m = (p_a + p_b) * 0.5f;
	const float len = p_a.distance_to(p_b);
	if (len < 1e-4f || p_cam->is_position_behind(m)) {
		return;
	}
	Ref<Font> font = p_overlay->get_theme_default_font();
	if (font.is_null()) {
		return;
	}
	const int fs = MAX(8, p_overlay->get_theme_default_font_size() - 1);
	const String t = String::num(len, 3);
	const Vector2 sz = font->get_string_size(t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs);
	const Vector2 at = p_cam->unproject_position(m) - Vector2(sz.x * 0.5f, -fs * 0.35f);
	p_overlay->draw_rect(Rect2(at - Vector2(3, fs * 0.95f), sz + Vector2(6, fs * 0.25f + 2)), Color(0, 0, 0, 0.55f));
	p_overlay->draw_string(font, at, t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color(1, 1, 1, 0.95f));
}

void HewnEditorPlugin::_draw_shape_tool(Control *p_overlay, Camera3D *p_cam) {
	Ref<Font> font = p_overlay->get_theme_default_font();
	const String name = T(hewn::shapes::type_name((hewn::shapes::Type)shape_type));
	const bool wall = shape_type == hewn::shapes::WALL || shape_type == hewn::shapes::SWEEP || shape_type == hewn::shapes::PATH_STAIRS;
	_draw_snap_mark(p_overlay, p_cam);
	if (shape_state == 0) {
		if (grid_hover_valid) {
			_draw_surface_grid(p_overlay, p_cam, grid_hover, grid_hover_u, grid_hover_v);
		}
		if (shape_cut) {
			draw_header(p_overlay, font, (shape_cut == 2 ? T("Cylinder Cut") : T("Box Cut")) + T(": click on a wall / floor, then the size, then the depth (no move: a flat hole)"));
			return;
		}
		draw_header(p_overlay, font, name + String(wall ? (shape_type == hewn::shapes::WALL ? T(": click the start of the wall") : T(": click the start of the path")) : oval_shape(shape_type) ? T(": click the center, then a corner of the oval (Alt: a circle, Shift: turned - a radius along a direction, then the other)") : round_shape(shape_type) ? T(": click the center, then the radius") : T(": click a corner of the base (Shift: a turned box, Ctrl: keep this plane, Alt: the last plane)")));
		return;
	}
	if (shape_state == 3) {
		// the first side of a turned box
		const Vector3 a = shape_origin + shape_u * shape_a.x + shape_v * shape_a.y, b = shape_origin + shape_u * shape_b.x + shape_v * shape_b.y;
		draw_world_line(p_overlay, p_cam, a, b, TOOL_COLOR, 2.0f);
		_draw_length_label(p_overlay, p_cam, a, b);
		draw_header(p_overlay, font, vformat(T("%s  side %.3f  -  click: its end"), name, shape_a.distance_to(shape_b)));
		return;
	}
	auto W = [&](float x, float y, float h) { return shape_origin + shape_u * x + shape_v * y + shape_n * h; };
	{
		Vector3 hit;
		if (shape_state == 1 && _plane_hit(p_cam, mouse_pos, shape_origin, shape_n, hit)) {
			_draw_surface_grid(p_overlay, p_cam, hit, shape_u, shape_v);
		}
	}
	const float h = shape_state == 2 ? shape_h : 0.0f;
	if (shape_cut && (shape_state == 1 || shape_state == 2) && shape_a.distance_to(shape_b) > 1e-4f) {
		_draw_cut_intersections(p_overlay, p_cam, _shape_cut_parts(h, false));
	}
	std::vector<Vector2> outline;
	std::vector<float> outline_h; // (a path's corner heights)
	if (wall) {
		outline = wall_pts;
		outline.push_back(shape_b);
		outline_h = wall_hs;
		outline_h.resize(wall_pts.size(), 0.0f);
		outline_h.push_back(wall_cur_h);
		for (size_t i = 0; i < outline.size(); i++) {
			if (std::fabs(outline_h[i]) > 1e-5f) {
				draw_world_line(p_overlay, p_cam, W(outline[i].x, outline[i].y, 0), W(outline[i].x, outline[i].y, outline_h[i]), TOOL_COLOR * Color(1, 1, 1, 0.5f), 1.0f);
			}
		}
		if (std::fabs(wall_cur_h) > 1e-5f || wall_h_mode) {
			_draw_length_label(p_overlay, p_cam, W(shape_b.x, shape_b.y, 0), W(shape_b.x, shape_b.y, wall_cur_h));
		}
	} else if (oval_shape(shape_type)) {
		const Vector2 r = (shape_b - shape_a).abs();
		for (int i = 0; i < 32; i++) {
			const float a = (float)Math::PI * 2.0f * i / 32;
			outline.push_back(shape_a + Vector2(std::cos(a) * r.x, std::sin(a) * r.y));
		}
		// the box of the quarter: center to the corner
		const Vector2 c = shape_b;
		draw_world_line(p_overlay, p_cam, W(shape_a.x, shape_a.y, 0), W(c.x, shape_a.y, 0), TOOL_COLOR * Color(1, 1, 1, 0.6f), 1.0f);
		draw_world_line(p_overlay, p_cam, W(c.x, shape_a.y, 0), W(c.x, c.y, 0), TOOL_COLOR * Color(1, 1, 1, 0.4f), 1.0f);
		draw_world_line(p_overlay, p_cam, W(c.x, c.y, 0), W(shape_a.x, c.y, 0), TOOL_COLOR * Color(1, 1, 1, 0.4f), 1.0f);
		draw_world_line(p_overlay, p_cam, W(shape_a.x, c.y, 0), W(shape_a.x, shape_a.y, 0), TOOL_COLOR * Color(1, 1, 1, 0.6f), 1.0f);
		_draw_length_label(p_overlay, p_cam, W(shape_a.x, shape_a.y, 0), W(c.x, shape_a.y, 0));
		_draw_length_label(p_overlay, p_cam, W(shape_a.x, shape_a.y, 0), W(shape_a.x, c.y, 0));
	} else if (round_shape(shape_type)) {
		const float r = shape_a.distance_to(shape_b);
		for (int i = 0; i < 32; i++) {
			const float a = (float)Math::PI * 2.0f * i / 32;
			outline.push_back(shape_a + Vector2(std::cos(a), std::sin(a)) * r);
		}
		draw_world_line(p_overlay, p_cam, W(shape_a.x, shape_a.y, 0), W(shape_b.x, shape_b.y, 0), TOOL_COLOR * Color(1, 1, 1, 0.6f), 1.0f);
		_draw_length_label(p_overlay, p_cam, W(shape_a.x, shape_a.y, 0), W(shape_b.x, shape_b.y, 0));
	} else {
		const Vector2 mn = shape_a.min(shape_b), mx = shape_a.max(shape_b);
		outline = { mn, Vector2(mx.x, mn.y), mx, Vector2(mn.x, mx.y) };
	}
	const int cnt = (int)outline.size();
	for (int i = 0; i < cnt; i++) {
		if (wall && i == cnt - 1) {
			break;
		}
		const Vector2 a = outline[i], b = outline[(i + 1) % cnt];
		const float ha = wall ? outline_h[i] : 0.0f, hb = wall ? outline_h[(i + 1) % cnt] : 0.0f;
		draw_world_line(p_overlay, p_cam, W(a.x, a.y, ha), W(b.x, b.y, hb), TOOL_COLOR, 2.0f);
		if (!round_shape(shape_type) && (wall || i < 2)) {
			_draw_length_label(p_overlay, p_cam, W(a.x, a.y, ha), W(b.x, b.y, hb));
		}
		if (shape_state == 2 && i == 0 && std::fabs(h) > 1e-5f) {
			_draw_length_label(p_overlay, p_cam, W(a.x, a.y, 0), W(a.x, a.y, h));
		}
		if (shape_state == 2) {
			draw_world_line(p_overlay, p_cam, W(a.x, a.y, h), W(b.x, b.y, h), TOOL_COLOR, 1.5f);
			if (!round_shape(shape_type) || i % 4 == 0) {
				draw_world_line(p_overlay, p_cam, W(a.x, a.y, 0), W(a.x, a.y, h), TOOL_COLOR * Color(1, 1, 1, 0.6f), 1.0f);
			}
		}
	}
	String info;
	if (wall) {
		const Vector2 last = wall_pts.empty() ? shape_a : wall_pts.back();
		info = vformat(T("%s  segment %.3f  -  click: next corner, Enter / RMB / double click: finish, Backspace: undo a corner"), name, last.distance_to(shape_b)) +
				(wall_h_mode ? vformat(T("   |   height %.3f (mouse; H / click: done)"), wall_cur_h) : String(T("   |   H: the corner's height")));
	} else if (oval_shape(shape_type)) {
		const Vector2 r = (shape_b - shape_a).abs();
		info = shape_state == 1 ? vformat(T("%s  radius %.3f x %.3f"), name, r.x, r.y) : vformat(T("%s  radius %.3f x %.3f  height %.3f"), name, r.x, r.y, shape_h);
	} else if (round_shape(shape_type)) {
		info = shape_state == 1 ? vformat(T("%s  radius %.3f"), name, shape_a.distance_to(shape_b)) : vformat(T("%s  radius %.3f  height %.3f"), name, shape_a.distance_to(shape_b), shape_h);
	} else {
		const Vector2 mn = shape_a.min(shape_b), mx = shape_a.max(shape_b);
		info = shape_state == 1 ? vformat("%s  %.3f x %.3f", name, mx.x - mn.x, mx.y - mn.y) : vformat("%s  %.3f x %.3f x %.3f", name, mx.x - mn.x, mx.y - mn.y, shape_h);
	}
	if (shape_dir_variants(shape_type) > 1) {
		info += T("   |   Q / E: turn the direction");
	}
	if (shape_state == 2 && !wall && !shape_num.is_empty()) {
		info += vformat(T("   |   height: %s (Enter)"), shape_num); // (only once typed)
	}
	draw_header(p_overlay, font, info);
}

// ------------------------------------------------------------------ bisect

// Bisect: in edit mode a line dragged across the mesh, the plane through it and the view cuts the selection (its
// panel: split, split and fill, remove above / below; its plane can be moved and turned after); in object mode the clip
// tool (points on the meshes, the plane through them).
void HewnEditorPlugin::start_bisect() {
	if (mode == MODE_OBJECT || !get_edited()) {
		start_clip();
		return;
	}
	if (modal != MODAL_NONE) {
		cancel_tool();
	}
	modal = MODAL_BISECT;
	bisect_state = 0;
	update_overlays();
}

bool HewnEditorPlugin::_bisect_widget_on() const {
	return modal == MODAL_NONE && mode != MODE_OBJECT && last_op == "bisect" && redo_panel && redo_panel->is_visible() && get_edited() &&
			get_edited()->get_instance_id() == last_op_node;
}

// The last bisect's plane: its point, normal, two axes in it and its size (the selection's around it).
bool HewnEditorPlugin::_bisect_frame(Vector3 &r_p, Vector3 &r_n, Vector3 &r_u, Vector3 &r_v, float &r_ext) {
	r_p = Vector3((float)op_param("bisect", "px"), (float)op_param("bisect", "py"), (float)op_param("bisect", "pz"));
	r_n = Vector3((float)op_param("bisect", "nx"), (float)op_param("bisect", "ny"), (float)op_param("bisect", "nz"));
	if (r_n.length() < 1e-6f) {
		return false;
	}
	r_n.normalize();
	r_u = std::fabs(r_n.y) < 0.9f ? r_n.cross(Vector3(0, 1, 0)).normalized() : r_n.cross(Vector3(1, 0, 0)).normalized();
	r_v = r_n.cross(r_u).normalized();
	r_ext = MAX(0.05f, (float)op_param("bisect", "ext"));
	return true;
}

// 1 the arrow, 2 / 3 the rings, 0 nothing under the mouse
int HewnEditorPlugin::_bisect_widget_hit(Camera3D *p_cam, const Vector2 &p_pos) {
	Vector3 p, n, u, v;
	float ext;
	if (!_bisect_frame(p, n, u, v, ext) || p_cam->is_position_behind(p)) {
		return 0;
	}
	const float L = _gizmo_length(p_cam, p);
	const Vector2 c = p_cam->unproject_position(p);
	if (!p_cam->is_position_behind(p + n * L) && seg_dist(p_pos, c, p_cam->unproject_position(p + n * L)) < 8.0f) {
		return 1;
	}
	for (int k = 0; k < 2; k++) {
		const Vector3 ax = k == 0 ? u : v;
		const Vector3 a = ax.cross(n).normalized(), b = n;
		float best = 1e30f;
		Vector2 prev;
		for (int i = 0; i <= 48; i++) {
			const float t = (float)Math::PI * 2.0f * i / 48.0f;
			const Vector3 w = p + (a * std::cos(t) + b * std::sin(t)) * L * 0.8f;
			const Vector2 s = p_cam->unproject_position(w);
			if (i > 0) {
				best = MIN(best, seg_dist(p_pos, prev, s));
			}
			prev = s;
		}
		if (best < 7.0f) {
			return 2 + k;
		}
	}
	return 0;
}

int32_t HewnEditorPlugin::_bisect_widget_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	if (bisect_drag == 0) {
		if (!_bisect_widget_on() || mb.is_null() || !mb->is_pressed() || mb->get_button_index() != MOUSE_BUTTON_LEFT || mb->is_shift_pressed() || mb->is_ctrl_pressed() ||
				mb->is_alt_pressed()) {
			return -1;
		}
		const int h = _bisect_widget_hit(p_cam, mb->get_position());
		if (h == 0) {
			return -1;
		}
		Vector3 u, v;
		float ext;
		_bisect_frame(bisect_drag_p, bisect_drag_n, u, v, ext);
		bisect_drag = h;
		bisect_drag_mouse = mb->get_position();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_RIGHT && mb->is_pressed()) {
		// back to where the drag began
		set_op_param("bisect", "px", bisect_drag_p.x);
		set_op_param("bisect", "py", bisect_drag_p.y);
		set_op_param("bisect", "pz", bisect_drag_p.z);
		set_op_param("bisect", "nx", bisect_drag_n.x);
		set_op_param("bisect", "ny", bisect_drag_n.y);
		set_op_param("bisect", "nz", bisect_drag_n.z);
		bisect_drag = 0;
		_redo_last();
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && !mb->is_pressed()) {
		bisect_drag = 0;
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mm.is_null()) {
		return AFTER_GUI_INPUT_STOP;
	}
	const Vector2 pos = mm->get_position();
	Vector3 p = bisect_drag_p, n = bisect_drag_n;
	if (bisect_drag == 1) {
		// along the normal: as far as the mouse went along the arrow on the screen
		const Vector2 c = p_cam->unproject_position(p), e = p_cam->unproject_position(p + n);
		const Vector2 sd = e - c;
		if (sd.length() > 1e-3f) {
			float d = (pos - bisect_drag_mouse).dot(sd.normalized()) / sd.length(); // (screen pixels per meter along it)
			if (snapping_active(mm->is_ctrl_pressed() || mm->is_meta_pressed()) && dock && dock->snap_move) {
				const float st = MAX(1e-4f, (float)dock->snap_move->get_value());
				d = std::round(d / st) * st;
			}
			p = bisect_drag_p + n * d;
		}
	} else {
		// turned about the ring's axis by the angle the mouse went round the plane's point
		const Vector3 un = std::fabs(bisect_drag_n.y) < 0.9f ? bisect_drag_n.cross(Vector3(0, 1, 0)).normalized() : bisect_drag_n.cross(Vector3(1, 0, 0)).normalized();
		const Vector3 ax = bisect_drag == 2 ? un : bisect_drag_n.cross(un).normalized();
		const Vector2 c = p_cam->unproject_position(p);
		float ang = (pos - c).angle() - (bisect_drag_mouse - c).angle();
		const Vector3 eye = p_cam->get_global_position();
		if (ax.dot(eye - p) > 0.0f) {
			ang = -ang;
		}
		if (snapping_active(mm->is_ctrl_pressed() || mm->is_meta_pressed()) && dock && dock->snap_rotate) {
			const float st = MAX(0.01f, (float)dock->snap_rotate->get_value()) * (float)Math::PI / 180.0f;
			ang = std::round(ang / st) * st;
		}
		n = Basis(ax, ang).xform(bisect_drag_n).normalized();
	}
	set_op_param("bisect", "px", p.x);
	set_op_param("bisect", "py", p.y);
	set_op_param("bisect", "pz", p.z);
	set_op_param("bisect", "nx", n.x);
	set_op_param("bisect", "ny", n.y);
	set_op_param("bisect", "nz", n.z);
	_redo_last();
	update_overlays();
	return AFTER_GUI_INPUT_STOP;
}

void HewnEditorPlugin::_draw_bisect_widget(Control *p_overlay, Camera3D *p_cam) {
	if (!_bisect_widget_on() && bisect_drag == 0) {
		return;
	}
	Vector3 p, n, u, v;
	float ext;
	if (!_bisect_frame(p, n, u, v, ext) || p_cam->is_position_behind(p)) {
		return;
	}
	const int hover = bisect_drag != 0 ? bisect_drag : _bisect_widget_hit(p_cam, mouse_pos);
	// the plane: a square as big as the selection, its outline
	PackedVector2Array quad;
	bool ok = true;
	for (int i = 0; i < 4; i++) {
		const Vector3 w = p + u * (i == 0 || i == 3 ? -ext : ext) + v * (i < 2 ? -ext : ext);
		ok = ok && !p_cam->is_position_behind(w);
		quad.push_back(p_cam->unproject_position(w));
	}
	if (ok) {
		draw_poly_safe(p_overlay, quad, Color(1.0f, 1.0f, 1.0f, 0.08f));
		quad.push_back(quad[0]);
		p_overlay->draw_polyline(quad, Color(1.0f, 1.0f, 1.0f, 0.6f), 1.5f, true);
	}
	const float L = _gizmo_length(p_cam, p);
	const Vector2 c = p_cam->unproject_position(p);
	if (!p_cam->is_position_behind(p + n * L)) {
		const Vector2 e = p_cam->unproject_position(p + n * L);
		const Color ac = hover == 1 ? HANDLE_HOVER : Color(0.3f, 0.7f, 1.0f);
		p_overlay->draw_line(c, e, ac, 2.5f, true);
		const Vector2 dir = (e - c).length() > 1e-3f ? (e - c).normalized() : Vector2(1, 0);
		const Vector2 nrm(-dir.y, dir.x);
		PackedVector2Array tri;
		tri.push_back(e + dir * 14.0f);
		tri.push_back(e + nrm * 5.5f);
		tri.push_back(e - nrm * 5.5f);
		draw_poly_safe(p_overlay, tri, ac);
	}
	for (int k = 0; k < 2; k++) {
		const Vector3 ax = k == 0 ? u : v;
		const Vector3 a = ax.cross(n).normalized(), b = n;
		PackedVector2Array ring;
		for (int i = 0; i <= 48; i++) {
			const float t = (float)Math::PI * 2.0f * i / 48.0f;
			const Vector3 w = p + (a * std::cos(t) + b * std::sin(t)) * L * 0.8f;
			if (p_cam->is_position_behind(w)) {
				ring.clear();
				break;
			}
			ring.push_back(p_cam->unproject_position(w));
		}
		if (ring.size() > 1) {
			p_overlay->draw_polyline(ring, hover == 2 + k ? HANDLE_HOVER : Color(1.0f, 0.75f, 0.3f, 0.85f), 2.0f, true);
		}
	}
}

int32_t HewnEditorPlugin::_bisect_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> k = p_event;
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	if ((k.is_valid() && k->is_pressed() && key_code(k) == KEY_ESCAPE) || (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_RIGHT)) {
		cancel_tool();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mm.is_valid()) {
		update_overlays();
		return bisect_state == 1 ? AFTER_GUI_INPUT_STOP : AFTER_GUI_INPUT_PASS;
	}
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		if (mb->is_pressed()) {
			bisect_a = mb->get_position();
			bisect_state = 1;
		} else if (bisect_state == 1) {
			const Vector2 b = mb->get_position();
			bisect_state = 0;
			modal = MODAL_NONE;
			HewnMesh3D *n = get_edited();
			if (n && b.distance_to(bisect_a) > 4.0f) {
				// The cutting plane contains the drawn line and the view direction.
				Vector3 piv;
				if (!_pivot(piv)) {
					piv = n->get_global_transform().origin;
				}
				const Vector3 eye = p_cam->get_global_position();
				const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(2).normalized();
				const float depth = MAX(0.05f, (piv - eye).dot(fwd));
				const Vector3 p0 = p_cam->project_position(bisect_a, depth), p1 = p_cam->project_position(b, depth);
				Vector3 nrm = p_cam->get_projection() == Camera3D::PROJECTION_ORTHOGONAL ? (p1 - p0).cross(fwd) : (p0 - eye).cross(p1 - eye);
				nrm.normalize();
				if (nrm.dot(p_cam->get_global_transform().basis.get_column(1)) < 0.0f) {
					nrm = -nrm; // ("above": the side up on the screen)
				}
				// (the plane's square: as big as the selection)
				float ext = 0.5f;
				{
					const hewn::Mesh &bm = n->get_data();
					const Transform3D bg = n->get_global_transform();
					for (int v : _selected_verts()) {
						ext = MAX(ext, bg.xform(bm.pos[v]).distance_to(p0) * 0.75f);
					}
					ext = MIN(ext, 1e4f);
				}
				// (the plane through the middle of the line, on the selection)
				const Vector3 mid = p0.lerp(p1, 0.5f);
				set_op_param("bisect", "ext", ext);
				set_op_param("bisect", "px", mid.x);
				set_op_param("bisect", "py", mid.y);
				set_op_param("bisect", "pz", mid.z);
				set_op_param("bisect", "nx", nrm.x);
				set_op_param("bisect", "ny", nrm.y);
				set_op_param("bisect", "nz", nrm.z);
				run_op("bisect");
			}
			update_overlays();
		}
		return AFTER_GUI_INPUT_STOP;
	}
	return AFTER_GUI_INPUT_PASS;
}

void HewnEditorPlugin::_draw_bisect(Control *p_overlay, Camera3D *p_cam) {
	draw_header(p_overlay, p_overlay->get_theme_default_font(), T("Bisect: drag a line across the mesh (then: its panel, the plane's arrow and rings)"));
	if (bisect_state == 1) {
		const Vector2 d = (mouse_pos - bisect_a);
		p_overlay->draw_line(bisect_a - d * 4.0f, mouse_pos + d * 4.0f, Color(1, 1, 1, 0.25f), 1.0f, true);
		p_overlay->draw_line(bisect_a, mouse_pos, TOOL_COLOR, 2.0f, true);
	}
}
