// The Scatter node's brushes in the tool column (while one is selected): Add (Ctrl: erase), Erase, Thin.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include "../hw_scatter.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/world3d.hpp>

#include <cmath>
#include <random>

using namespace godot;
using namespace hw_editor;

HewnScatter3D *HewnEditorPlugin::get_scatter() const {
	HewnScatter3D *s = Object::cast_to<HewnScatter3D>(ObjectDB::get_instance(scatter_id));
	return s && s->is_inside_tree() ? s : nullptr;
}

// The surface under a ray: the Hewn meshes, else the scene's colliders.
bool HewnEditorPlugin::_scatter_ray(Camera3D *p_cam, const Vector3 &p_from, const Vector3 &p_dir, float p_max, Vector3 &r_hit, Vector3 &r_normal) const {
	Vector3 hit, nrm;
	float best = p_max;
	bool found = false;
	if (_raycast_scene(p_from, p_dir, hit, nrm)) {
		const float t = (hit - p_from).dot(p_dir);
		if (t >= 0.0f && t <= best) {
			best = t;
			r_hit = hit;
			r_normal = nrm;
			found = true;
		}
	}
	if (p_cam && p_cam->get_world_3d().is_valid()) {
		PhysicsDirectSpaceState3D *ss = p_cam->get_world_3d()->get_direct_space_state();
		if (ss) {
			Ref<PhysicsRayQueryParameters3D> q = PhysicsRayQueryParameters3D::create(p_from, p_from + p_dir * best);
			// (the bodies of hidden objects are left out: what is not seen is not ground)
			TypedArray<RID> skip;
			for (int tries = 0; tries < 8; tries++) {
				q->set_exclude(skip);
				const Dictionary d = ss->intersect_ray(q);
				if (d.is_empty()) {
					break;
				}
				Node3D *col = Object::cast_to<Node3D>((Object *)d["collider"]);
				if (col && !col->is_visible_in_tree()) {
					skip.push_back(d["rid"]);
					continue;
				}
				r_hit = d["position"];
				r_normal = d["normal"];
				found = true;
				break;
			}
		}
	}
	if (found && r_normal.dot(p_dir) > 0.0f) {
		r_normal = -r_normal; // (a face seen from behind)
	}
	return found;
}

bool HewnEditorPlugin::_scatter_hit(Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_hit, Vector3 &r_normal) const {
	Vector3 from, dir;
	if (!_ray(p_cam, p_pos, from, dir)) {
		return false;
	}
	return _scatter_ray(p_cam, from, dir, 1e6f, r_hit, r_normal);
}

void HewnEditorPlugin::_scatter_dab(Camera3D *p_cam, const Vector3 &p_hit, const Vector3 &p_normal, int p_kind) {
	HewnScatter3D *sc = get_scatter();
	if (!sc) {
		return;
	}
	const float r = sc->get_brush_radius();
	if (p_kind == 1) {
		sc->erase(p_hit, r, sc->get_erase_strength()); // (less than all: thinned out)
		return;
	}
	if (p_kind == 2) {
		sc->erase(p_hit, r, 0.5f);
		return;
	}
	// random spots in the circle, each dropped onto the surface below it (along the normal where the brush is)
	static std::mt19937 rng{ std::random_device{}() };
	std::uniform_real_distribution<float> u01(0.0f, 1.0f);
	const Vector3 n = p_normal.normalized();
	const Vector3 u = n.cross(std::fabs(n.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).normalized();
	const Vector3 v = n.cross(u);
	const int tries = CLAMP((int)std::ceil(sc->get_density() * (float)Math::PI * r * r * 0.35f), 1, 400);
	for (int i = 0; i < tries; i++) {
		const float rr = r * std::sqrt(u01(rng)), a = (float)Math::PI * 2.0f * u01(rng);
		const Vector3 q = p_hit + (u * std::cos(a) + v * std::sin(a)) * rr;
		Vector3 h, hn;
		if (_scatter_ray(p_cam, q + n * r, -n, r * 2.0f, h, hn)) {
			sc->paint_point(h, hn);
		}
	}
}

int HewnEditorPlugin::_scatter_kind(bool p_ctrl, bool p_shift) const {
	if (scatter_tool == 0) {
		return p_ctrl ? 1 : p_shift ? 2 : 0;
	}
	return scatter_tool;
}

void HewnEditorPlugin::_scatter_end() {
	HewnScatter3D *sc = get_scatter();
	scatter_painting = false;
	if (!sc) {
		return;
	}
	sc->flush();
	const PackedFloat32Array after = sc->get_instances();
	if (after == scatter_before) {
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Scatter Paint", UndoRedo::MERGE_DISABLE, sc);
	ur->add_do_property(sc, "instances", after);
	ur->add_undo_property(sc, "instances", scatter_before);
	ur->commit_action(false);
	scatter_before = PackedFloat32Array();
}

int32_t HewnEditorPlugin::_scatter_input(Camera3D *p_cam, const Ref<InputEvent> &p_event) {
	HewnScatter3D *sc = get_scatter();
	Ref<InputEventMouseButton> mb = p_event;
	Ref<InputEventMouseMotion> mm = p_event;
	Ref<InputEventKey> k = p_event;
	if (k.is_valid()) {
		if (!k->is_pressed() || k->is_echo()) {
			return -1;
		}
		const Key code = key_code(k);
		if (code == KEY_ESCAPE || (code == KEY_TAB && !k->is_shift_pressed() && !k->is_ctrl_pressed() && !k->is_alt_pressed())) {
			if (scatter_painting) {
				_scatter_end();
			}
			scatter_tool = -1;
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if ((code == KEY_BRACKETLEFT || code == KEY_BRACKETRIGHT) && !k->is_ctrl_pressed()) {
			sc->set_brush_radius(sc->get_brush_radius() * (code == KEY_BRACKETRIGHT ? 1.15f : 1.0f / 1.15f));
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return -1;
	}
	if (mb.is_valid()) {
		const MouseButton bi = mb->get_button_index();
		if ((bi == MOUSE_BUTTON_WHEEL_UP || bi == MOUSE_BUTTON_WHEEL_DOWN) && mb->is_pressed() && (mb->is_ctrl_pressed() || mb->is_meta_pressed())) {
			sc->set_brush_radius(sc->get_brush_radius() * (bi == MOUSE_BUTTON_WHEEL_UP ? 1.1f : 1.0f / 1.1f));
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		if (bi != MOUSE_BUTTON_LEFT) {
			return -1; // (the camera)
		}
		if (mb->is_pressed()) {
			mouse_pos = mb->get_position();
			Vector3 hit, hn;
			if (!_scatter_hit(p_cam, mouse_pos, hit, hn)) {
				return AFTER_GUI_INPUT_STOP;
			}
			scatter_before = sc->get_instances();
			sc->begin_stroke();
			scatter_painting = true;
			scatter_last = hit;
			_scatter_dab(p_cam, hit, hn, _scatter_kind(mb->is_ctrl_pressed() || mb->is_meta_pressed(), mb->is_shift_pressed()));
		} else if (scatter_painting) {
			_scatter_end();
		}
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (mm.is_valid()) {
		mouse_pos = mm->get_position();
		if (scatter_painting) {
			Vector3 hit, hn;
			if (_scatter_hit(p_cam, mouse_pos, hit, hn) && hit.distance_to(scatter_last) >= sc->get_brush_radius() * 0.4f) {
				scatter_last = hit;
				_scatter_dab(p_cam, hit, hn, _scatter_kind(mm->is_ctrl_pressed() || mm->is_meta_pressed(), mm->is_shift_pressed()));
			}
		}
		update_overlays();
		return scatter_painting ? AFTER_GUI_INPUT_STOP : -1;
	}
	return -1;
}

void HewnEditorPlugin::_draw_scatter(Control *p_overlay, Camera3D *p_cam) {
	HewnScatter3D *sc = get_scatter();
	if (!sc) {
		return;
	}
	if (!Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)) {
		Input *in = Input::get_singleton();
		const int kind = _scatter_kind(in->is_key_pressed(KEY_CTRL) || in->is_key_pressed(KEY_META), in->is_key_pressed(KEY_SHIFT));
		const Color c = kind == 1 ? Color(0.95f, 0.3f, 0.3f, 0.9f) : kind == 2 ? Color(0.95f, 0.7f, 0.2f, 0.9f) : Color(0.35f, 0.85f, 0.4f, 0.9f);
		Vector3 hit, hn;
		if (_scatter_hit(p_cam, mouse_pos, hit, hn)) {
			const float r = sc->get_brush_radius();
			const Vector3 u = hn.cross(std::fabs(hn.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).normalized();
			const Vector3 v = hn.cross(u);
			PackedVector2Array pts;
			for (int i = 0; i <= 48; i++) {
				const float a = (float)Math::PI * 2.0f * i / 48;
				const Vector3 q = hit + (u * std::cos(a) + v * std::sin(a)) * r;
				if (!p_cam->is_position_behind(q)) {
					pts.push_back(p_cam->unproject_position(q));
				}
			}
			if (pts.size() > 2) {
				p_overlay->draw_polyline(pts, c, 1.5f, true);
			}
			if (!p_cam->is_position_behind(hit)) {
				p_overlay->draw_circle(p_cam->unproject_position(hit), 2.0f, c, true, -1.0f, true);
			}
		}
	}
	draw_header(p_overlay, p_overlay->get_theme_default_font(),
			scatter_tool == 0 ? T("Ctrl: erase, Shift: thin, Ctrl+wheel or [ / ]: radius") : T("Ctrl+wheel or [ / ]: radius"));
}
