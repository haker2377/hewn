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

// The heights on the plan (local, over its line): the top of its highest walls (their lift and height), the top of
// its highest floor, and where the highest walls stand (none: -1e9).
struct PlanLevels {
	float walls_top = -1e9f, walls_base = -1e9f, floor_top = -1e9f;
};

PlanLevels plan_levels(HewnCurve3D *c) {
	PlanLevels L;
	for (int i = 0; i < c->get_child_count(); i++) {
		HewnMesh3D *m = Object::cast_to<HewnMesh3D>(c->get_child(i));
		if (!m || m->get_follow_curve().is_empty()) {
			continue;
		}
		if (m->get_shape() == hewn::shapes::WALL) {
			const float top = m->get_follow_lift() + (float)m->get_shape_param("height");
			if (top > L.walls_top) {
				L.walls_top = top;
				L.walls_base = m->get_follow_lift();
			}
		} else if (m->get_shape() == hewn::shapes::POLY_SHAPE) {
			L.floor_top = MAX(L.floor_top, m->get_follow_lift() + (float)m->get_shape_param("height"));
		}
	}
	return L;
}

int plan_count(HewnCurve3D *c, int p_shape) {
	int k = 0;
	for (int i = 0; i < c->get_child_count(); i++) {
		HewnMesh3D *m = Object::cast_to<HewnMesh3D>(c->get_child(i));
		k += m && m->get_shape() == p_shape ? 1 : 0;
	}
	return k;
}

} // namespace

// A floor plan: a closed rectangle of a poly curve with its floor under it (the walls, the roof...: its right click
// menu, Build on the Plan).
HewnCurve3D *HewnEditorPlugin::_make_floor_plan(float p_w, float p_d, const Transform3D &p_xf) {
	const float w = MAX(0.1f, p_w) * 0.5f, d = MAX(0.1f, p_d) * 0.5f;
	HewnCurve3D *c = memnew(HewnCurve3D);
	c->set_name("FloorPlan");
	const PackedVector3Array pts({ Vector3(-w, 0, -d), Vector3(w, 0, -d), Vector3(w, 0, d), Vector3(-w, 0, d) });
	c->add_spline(hewn::curves::POLY, pts, true);
	c->set_meta("hewn_floor_height", 3.0f);
	c->set_bake_path(false);
	c->set_transform(p_xf);
	HewnMesh3D *f = memnew(HewnMesh3D);
	f->set_name("Floor");
	f->set_shape(hewn::shapes::POLY_SHAPE);
	f->set_shape_param("height", 0.2f);
	if (texture_dock && texture_dock->current_material.is_valid()) {
		f->apply_material(texture_dock->current_material, PackedInt32Array());
	}
	PackedVector3Array low;
	for (const Vector3 &q : pts) {
		low.push_back(q - Vector3(0, 0.2f, 0));
	}
	f->set_poly_points(low); // (its faces now, for its UVs; then the plan's points)
	_new_object_uv(f);
	f->set_follow_lift(-0.2f);
	f->set_follow_curve(NodePath(".."));
	c->add_child(f);
	return c;
}

// Add > Curve > Floor Plan: drawn with two clicks (its corners), or placed at the 3D cursor.
void HewnEditorPlugin::add_floor_plan() {
	if (!create_at_cursor) {
		start_shape_tool(hewn::shapes::PLANE);
		shape_make = "plan";
		return;
	}
	adding_from_menu = true;
	start_placing("FloorPlan", [this]() -> Node3D * { return _make_floor_plan(8.0f, 6.0f, Transform3D()); }, false);
	adding_from_menu = false;
}

// The drawn shape as what was asked for instead (see shape_make).
void HewnEditorPlugin::_make_from_drawn(HewnMesh3D *p_node, const String &p_what) {
	const Transform3D xf = p_node->get_transform();
	if (p_what == "plan") {
		HewnCurve3D *c = _make_floor_plan(p_node->get_shape_param("width"), p_node->get_shape_param("depth"), xf);
		memdelete(p_node);
		_add_node_with_undo(c, "Add Floor Plan");
		return;
	}
	if (p_what == "roof") {
		// on the box's bottom, its rectangle the outline; the box's height the ridge's (by its pitch)
		const float w = p_node->get_shape_param("width"), h = p_node->get_shape_param("height"), d = p_node->get_shape_param("depth");
		float min_y = 0.0f;
		for (const Vector3 &q : p_node->get_data().pos) {
			min_y = MIN(min_y, q.y);
		}
		memdelete(p_node);
		HewnMesh3D *r = memnew(HewnMesh3D);
		r->set_name("Roof");
		r->set_shape(hewn::shapes::ROOF);
		const float hw = w * 0.5f, hd = d * 0.5f;
		r->set_poly_points(PackedVector3Array({ Vector3(-hw, 0, -hd), Vector3(hw, 0, -hd), Vector3(hw, 0, hd), Vector3(-hw, 0, hd) }));
		const float half = MAX(0.01f, MIN(w, d) * 0.5f);
		r->set_shape_param("pitch", CLAMP((float)Math::rad_to_deg(std::atan2(std::fabs(h), half)), 1.0f, 89.0f));
		if (texture_dock && texture_dock->current_material.is_valid()) {
			r->apply_material(texture_dock->current_material, PackedInt32Array());
		}
		r->set_transform(xf * Transform3D(Basis(), Vector3(0, min_y, 0)));
		_new_object_uv(r);
		_add_node_with_undo(r, "Create Roof");
		_update_shape_panel();
		return;
	}
	if (p_what.begins_with("curve:")) {
		// a curve through the corners clicked (their heights kept), of that kind
		const int kind = p_what.get_slicec(':', 1).to_int();
		PackedVector3Array pts = p_node->get_poly_points();
		memdelete(p_node);
		if (pts.size() < 2) {
			return;
		}
		bool cyclic = false;
		if (pts.size() >= 4 && pts[0].distance_to(pts[pts.size() - 1]) < 1e-3f) {
			pts.remove_at(pts.size() - 1); // (ended where it began: a loop)
			cyclic = true;
		}
		HewnCurve3D *c = make_curve_kind(kind);
		c->clear_splines();
		const int type = kind == 10 ? hewn::curves::POLY : kind == 2 ? hewn::curves::NURBS : hewn::curves::BEZIER;
		c->add_spline(type, pts, cyclic);
		c->set_transform(xf);
		_add_node_with_undo(c, "Add Curve");
	}
}

HewnCurve3D *HewnEditorPlugin::_shape_on_curve(HewnMesh3D *p_node, bool p_reparent) {
	PackedVector3Array pts = p_node->get_poly_points();
	const int t = p_node->get_shape();
	bool cyclic = t == hewn::shapes::POLY_SHAPE || t == hewn::shapes::ROOF; // (an outline: closed)
	if (!cyclic && pts.size() >= 4 && pts[0].distance_to(pts[pts.size() - 1]) < 1e-3f) {
		pts.remove_at(pts.size() - 1); // (a ring: the curve closed)
		cyclic = true;
	}
	HewnCurve3D *c = memnew(HewnCurve3D);
	c->set_name(String(p_node->get_name()).is_empty() ? String(hewn::shapes::type_name((hewn::shapes::Type)t)) + "Curve" : String(p_node->get_name()) + "Curve");
	c->add_spline(hewn::curves::POLY, pts, cyclic);
	c->set_bake_path(false); // (the shape's: not a Path3D in the game)
	c->set_transform(p_node->get_transform());
	if (!p_reparent) {
		if (String(p_node->get_name()).is_empty()) {
			p_node->set_name(hewn::shapes::type_name((hewn::shapes::Type)t));
		}
		p_node->set_transform(Transform3D());
		p_node->set_follow_lift(0.0f);
		p_node->set_follow_curve(NodePath(".."));
		c->add_child(p_node);
	}
	return c;
}

// Object > Convert to Curve for a shape through points (a wall, a sweep, stairs, a poly shape, a roof): a curve in its
// place with it following (under it): its points are edited as the curve's then.
void HewnEditorPlugin::shape_to_curve() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	EditorUndoRedoManager *ur = get_undo_redo();
	bool any = false;
	for (HewnMesh3D *n : _selected_meshes()) {
		const int t = n->get_shape();
		if ((t != hewn::shapes::WALL && t != hewn::shapes::SWEEP && t != hewn::shapes::PATH_STAIRS && t != hewn::shapes::POLY_SHAPE && t != hewn::shapes::ROOF) ||
				!n->get_follow_curve().is_empty() || n->get_poly_points().size() < 2 || !n->get_parent() || n == root) {
			continue;
		}
		if (!any) {
			ur->create_action("Hewn: Convert to Curve", UndoRedo::MERGE_DISABLE, root);
			any = true;
		}
		Node *parent = n->get_parent();
		HewnCurve3D *c = _shape_on_curve(n, true);
		ur->add_do_method(parent, "add_child", c, true);
		ur->add_do_method(parent, "move_child", c, n->get_index());
		ur->add_do_method(c, "set_owner", root);
		ur->add_do_method(parent, "remove_child", n);
		ur->add_do_method(c, "add_child", n, true);
		ur->add_do_method(n, "set_owner", root);
		ur->add_do_method(n, "set_transform", Transform3D());
		ur->add_do_method(n, "set_follow_lift", 0.0f);
		ur->add_do_method(n, "set_follow_curve", NodePath(".."));
		ur->add_do_reference(c);
		ur->add_undo_method(n, "set_follow_curve", NodePath());
		ur->add_undo_method(c, "remove_child", n);
		ur->add_undo_method(parent, "add_child", n, true);
		ur->add_undo_method(parent, "move_child", n, n->get_index());
		ur->add_undo_method(n, "set_owner", n->get_owner());
		ur->add_undo_method(n, "set_transform", n->get_transform());
		ur->add_undo_method(n, "set_poly_points", n->get_poly_points());
		ur->add_undo_method(parent, "remove_child", c);
	}
	if (any) {
		ur->commit_action();
	}
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
	const PlanLevels L = plan_levels(c);
	const bool has_walls = L.walls_top > -1e8f, has_floor = L.floor_top > -1e8f;
	// where the next walls stand: on the highest floor, or on the highest walls (whichever is higher)
	const float walls_at = MAX(has_floor ? L.floor_top : 0.0f, has_walls ? L.walls_top : 0.0f);
	// where the next floor goes: on the walls' top, else a floor's height over the highest floor
	const float next_at = has_walls && (!has_floor || L.walls_top > L.floor_top + 1e-3f) ? L.walls_top : has_floor ? L.floor_top + fh : 0.0f;
	// the roof: on the highest walls (or over the highest floor)
	const float top_at = has_walls ? MAX(L.walls_top, has_floor ? L.floor_top : -1e9f) : has_floor ? L.floor_top + fh : fh;
	std::vector<Node3D *> made;
	auto shape = [&](int p_type, const String &p_name) {
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
	auto numbered = [&](const String &p_base, int p_shape) {
		const int k = plan_count(c, p_shape);
		return k == 0 ? p_base : vformat("%s%d", p_base, k + 1);
	};
	const float wall_t = 0.2f;
	if (p_what == "floor" || p_what == "next") {
		// a floor level: its floor (no walls: Walls adds them on it)
		HewnMesh3D *f = shape(hewn::shapes::POLY_SHAPE, numbered("Floor", hewn::shapes::POLY_SHAPE));
		f->set_shape_param("height", 0.2f);
		const float at = p_what == "floor" && !has_floor ? 0.0f : next_at;
		follow(f, at - 0.2f); // (its top where the walls on it stand)
	} else if (p_what == "walls") {
		HewnMesh3D *w = shape(hewn::shapes::WALL, numbered("Walls", hewn::shapes::WALL));
		w->set_shape_param("height", fh);
		w->set_shape_param("thickness", wall_t);
		w->set_shape_param("side", 0); // (inside the plan: its line the outer face)
		follow(w, walls_at);
	} else if (p_what == "roof") {
		HewnMesh3D *r = shape(hewn::shapes::ROOF, "Roof");
		r->set_shape_param("wall_thickness", wall_t);
		follow(r, top_at);
	} else if (p_what == "cornice") {
		HewnMesh3D *s = shape(hewn::shapes::SWEEP, "Cornice");
		s->set_shape_param("profile", 4);
		s->set_shape_param("align", 1);
		s->set_shape_param("closed", 1);
		s->set_shape_param("width", 0.25f);
		s->set_shape_param("height", 0.25f);
		s->set_shape_param("drop", 0.25f); // (under the walls' top, against them)
		follow(s, top_at);
	} else if (p_what == "along_wall" || p_what == "along_sweep" || p_what == "along_stairs") {
		// a shape following the curve as it is (not a plan): a wall, a sweep, stairs along it
		const int t = p_what == "along_wall" ? hewn::shapes::WALL : p_what == "along_sweep" ? hewn::shapes::SWEEP : hewn::shapes::PATH_STAIRS;
		HewnMesh3D *m = shape(t, p_what == "along_wall" ? String("Wall") : p_what == "along_sweep" ? String("Sweep") : String("PathStairs"));
		if (t == hewn::shapes::SWEEP && !c->get_splines_const().empty() && c->get_splines_const()[0].cyclic) {
			m->set_shape_param("closed", 1);
		}
		follow(m, 0.0f);
	} else if (p_what == "windows") {
		HewnWindows3D *w = memnew(HewnWindows3D);
		w->set_name("Windows");
		w->set_depth(wall_t * 4.0f);
		w->set_offset(-wall_t * 0.5f); // (in the middle of the walls, inside the plan's line)
		made.push_back(w);
		follow(w, has_walls ? L.walls_base : 0.0f);
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
