// Floor plans: a closed poly curve on the ground that a building is made on. Its floor, walls, roof, cornice and
// windows are shapes following it (children of it): drag its points and they all follow. Each floor up is the
// same plan raised by the floor's height (their Follow Lift); the roof and the cornice sit on the top floor.
#include "hw_editor.h"
#include "hw_editor_util.h"
#include "hw_textures.h"
#include "../hw_curve.h"
#include "../hw_windows.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>

using namespace godot;

namespace {

float plan_floor_height(HewnCurve3D *c) {
	return c->has_meta("hewn_floor_height") ? (float)c->get_meta("hewn_floor_height") : 3.0f;
}

// The floors already on the plan: the highest wall's lift over the floor height, plus one (none: 0).
int plan_floors(HewnCurve3D *c) {
	const float fh = plan_floor_height(c);
	int top = -1;
	for (int i = 0; i < c->get_child_count(); i++) {
		HewnMesh3D *m = Object::cast_to<HewnMesh3D>(c->get_child(i));
		if (m && m->get_shape() == hewn::shapes::WALL && !m->get_follow_curve().is_empty()) {
			top = MAX(top, (int)std::round(m->get_follow_lift() / MAX(0.01f, fh)));
		}
	}
	return top + 1;
}

} // namespace

// Add > Curve > Floor Plan: a closed rectangle of a poly curve, placed with a click.
void HewnEditorPlugin::add_floor_plan() {
	adding_from_menu = true;
	start_placing("FloorPlan", []() -> Node3D * {
		HewnCurve3D *c = memnew(HewnCurve3D);
		c->set_name("FloorPlan");
		c->add_spline(hewn::curves::POLY, PackedVector3Array({ Vector3(-4, 0, -3), Vector3(4, 0, -3), Vector3(4, 0, 3), Vector3(-4, 0, 3) }), true);
		c->set_meta("hewn_floor_height", 3.0f);
		return c;
	}, false);
	adding_from_menu = false;
}

// A part of the building on the selected plan (the plan curve, or something following it): its floor, its walls
// (the top floor's), its roof, a cornice, the windows, or the next floor up (its floor and walls).
void HewnEditorPlugin::plan_add(const String &p_what) {
	HewnCurve3D *c = nullptr;
	for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
		if (HewnCurve3D *hc = Object::cast_to<HewnCurve3D>(v)) {
			c = hc;
		} else if (Node *n = Object::cast_to<Node>(v)) {
			c = c ? c : Object::cast_to<HewnCurve3D>(n->get_parent());
		}
	}
	if (!c) {
		return;
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	const float fh = plan_floor_height(c);
	int floors = plan_floors(c);
	std::vector<Node3D *> made;
	auto shape = [&](int p_type, const String &p_name, float p_lift) {
		HewnMesh3D *m = memnew(HewnMesh3D);
		m->set_name(p_name);
		m->set_shape(p_type);
		if (texture_dock && texture_dock->current_material.is_valid()) {
			m->apply_material(texture_dock->current_material, PackedInt32Array());
		}
		made.push_back(m);
		return m;
	};
	auto follow = [&](Node3D *n, float p_lift) {
		n->set("follow_lift", p_lift);
		n->set("follow_curve", NodePath(".."));
	};
	const float wall_t = 0.2f;
	auto add_walls = [&](int p_level) {
		HewnMesh3D *w = shape(hewn::shapes::WALL, p_level == 0 ? String("Walls") : vformat("Walls%d", p_level + 1), p_level * fh);
		w->set_shape_param("height", fh);
		w->set_shape_param("thickness", wall_t);
		w->set_shape_param("side", 0); // (inside the plan: its line the outer face)
		follow(w, p_level * fh);
	};
	auto add_floor = [&](int p_level) {
		HewnMesh3D *f = shape(hewn::shapes::POLY_SHAPE, p_level == 0 ? String("Floor") : vformat("Floor%d", p_level + 1), p_level * fh);
		f->set_shape_param("height", 0.2f);
		follow(f, p_level * fh - 0.2f); // (its top where the floor's walls stand)
	};
	auto has_floor = [&](int p_level) {
		for (int i = 0; i < c->get_child_count(); i++) {
			HewnMesh3D *m = Object::cast_to<HewnMesh3D>(c->get_child(i));
			if (m && m->get_shape() == hewn::shapes::POLY_SHAPE && std::fabs(m->get_follow_lift() - (p_level * fh - 0.2f)) < 1e-3f) {
				return true;
			}
		}
		return false;
	};
	if (p_what == "floor") {
		// (the lowest floor without one)
		int lv = 0;
		while (lv < MAX(1, floors) && has_floor(lv)) {
			lv++;
		}
		add_floor(lv);
	} else if (p_what == "walls") {
		add_walls(floors); // (the walls of the next floor up: none yet, the ground floor's)
	} else if (p_what == "next") {
		if (floors == 0) {
			add_floor(0);
			add_walls(0);
		} else {
			add_floor(floors);
			add_walls(floors);
		}
	} else if (p_what == "roof") {
		HewnMesh3D *r = shape(hewn::shapes::ROOF, "Roof", MAX(1, floors) * fh);
		r->set_shape_param("wall_thickness", wall_t);
		follow(r, MAX(1, floors) * fh);
	} else if (p_what == "cornice") {
		HewnMesh3D *s = shape(hewn::shapes::SWEEP, "Cornice", MAX(1, floors) * fh);
		s->set_shape_param("profile", 4);
		s->set_shape_param("align", 1);
		s->set_shape_param("closed", 1);
		s->set_shape_param("width", 0.25f);
		s->set_shape_param("height", 0.25f);
		s->set_shape_param("drop", 0.25f); // (under the walls' top, against them)
		follow(s, MAX(1, floors) * fh);
	} else if (p_what == "windows") {
		HewnWindows3D *w = memnew(HewnWindows3D);
		w->set_name(floors > 1 ? vformat("Windows%d", floors) : String("Windows"));
		w->set_depth(wall_t * 4.0f);
		w->set_offset(-wall_t * 0.5f); // (in the middle of the walls, inside the plan's line)
		made.push_back(w);
		follow(w, MAX(0, floors - 1) * fh);
	}
	if (made.empty()) {
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Build on the Plan");
	for (Node3D *n : made) {
		ur->add_do_method(c, "add_child", n, true);
		ur->add_do_method(n, "set_owner", root);
		ur->add_do_reference(n);
		ur->add_undo_method(c, "remove_child", n);
	}
	ur->commit_action();
	EditorSelection *es = EditorInterface::get_singleton()->get_selection();
	es->clear();
	es->add_node(made.back());
}
