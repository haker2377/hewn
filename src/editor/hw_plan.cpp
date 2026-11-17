// Floor plans: a closed poly curve on the ground that a building is made on. Its floor, walls, roof, cornice and
// windows are shapes following it (children of it): drag its points and they all follow. Each floor up is the
// same plan raised by the floor's height (their Follow Lift); the roof and the cornice sit on the top floor.
#include "hw_editor.h"
#include "hw_editor_util.h"
#include "hw_textures.h"
#include "../hw_building.h"
#include "../hw_curve.h"
#include "../hw_cutter.h"
#include "../hw_windows.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
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
	// a building: its plan the rectangle, a ground floor (its slab, walls, windows) and a roof; its parts made when it
	// is in the scene
	const float w = MAX(0.1f, p_w) * 0.5f, d = MAX(0.1f, p_d) * 0.5f;
	HewnBuilding3D *c = memnew(HewnBuilding3D);
	c->set_name("Building");
	const PackedVector3Array pts({ Vector3(-w, 0, -d), Vector3(w, 0, -d), Vector3(w, 0, d), Vector3(-w, 0, d) });
	c->add_spline(hewn::curves::POLY, pts, true);
	c->set_bake_path(false);
	c->set_transform(p_xf);
	Ref<HewnBuildingFloor> f;
	f.instantiate();
	TypedArray<Resource> fl;
	fl.push_back(f);
	c->set_floors(fl);
	if (texture_dock && texture_dock->current_material.is_valid()) {
		c->set_floor_material(texture_dock->current_material);
		c->set_wall_material(texture_dock->current_material);
		c->set_roof_material(texture_dock->current_material);
	}
	return c;
}

// The building of what is selected (it, a curve or a part under it).
HewnBuilding3D *HewnEditorPlugin::_selected_building() const {
	if (hc_edit) {
		if (HewnCurve3D *c = _hc()) {
			if (HewnBuilding3D *b = Object::cast_to<HewnBuilding3D>(c)) {
				return b;
			}
			if (HewnBuilding3D *b = Object::cast_to<HewnBuilding3D>(c->get_parent())) {
				return b;
			}
		}
	}
	for (const Variant &v : EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes()) {
		for (Node *n = Object::cast_to<Node>(v); n; n = n->get_parent()) {
			if (HewnBuilding3D *b = Object::cast_to<HewnBuilding3D>(n)) {
				return b;
			}
		}
	}
	return nullptr;
}

// The building's own operations (its right click menu; the sides' ones while its outlines are edited).
void HewnEditorPlugin::building_op(const String &p_op) {
	HewnBuilding3D *b = _selected_building();
	if (!b) {
		return;
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	EditorUndoRedoManager *ur = get_undo_redo();
	const TypedArray<Resource> old = b->get_floors();
	if (p_op == "add_floor" || p_op == "remove_floor") {
		TypedArray<Resource> fl = old.duplicate();
		if (p_op == "add_floor") {
			// (like the one under it)
			Ref<HewnBuildingFloor> f = fl.is_empty() ? Ref<HewnBuildingFloor>() : Ref<HewnBuildingFloor>(fl.back());
			fl.push_back(f.is_valid() ? Ref<HewnBuildingFloor>(f->duplicate()) : Ref<HewnBuildingFloor>(memnew(HewnBuildingFloor)));
		} else if (fl.size() > 1) {
			fl.pop_back();
		} else {
			return;
		}
		ur->create_action(p_op == "add_floor" ? "Hewn: Add Floor" : "Hewn: Remove Floor", UndoRedo::MERGE_DISABLE, b);
		ur->add_do_method(b, "set_floors", fl);
		ur->add_undo_method(b, "set_floors", old);
		ur->commit_action();
		return;
	}
	if (p_op == "own_outline" || p_op == "courtyard") {
		// a curve under the building: the top floor's own outline (a copy of the one it has now), or a courtyard (a
		// rectangle in the middle, a third of the plan's size)
		HewnCurve3D *src = b->top_outline();
		HewnCurve3D *c = memnew(HewnCurve3D);
		c->set_bake_path(false);
		if (p_op == "own_outline") {
			const int top = MAX(0, b->get_floor_count() - 1);
			c->set_name(vformat("Floor%dOutline", top + 1));
			c->set_meta("hewn_floor_outline", true);
			c->set_state(src->get_state());
			c->set_transform(src == b ? Transform3D() : src->get_transform());
		} else {
			c->set_name("Courtyard");
			c->set_meta("hewn_courtyard", true);
			AABB box;
			bool first = true;
			for (const Vector3 &q : b->sampled_points(0)) {
				box = first ? AABB(q, Vector3()) : box.expand(q);
				first = false;
			}
			const Vector3 ctr = box.get_center(), h = box.size / 6.0f;
			c->add_spline(hewn::curves::POLY, PackedVector3Array({ Vector3(ctr.x - h.x, 0, ctr.z - h.z), Vector3(ctr.x + h.x, 0, ctr.z - h.z), Vector3(ctr.x + h.x, 0, ctr.z + h.z), Vector3(ctr.x - h.x, 0, ctr.z + h.z) }), true);
		}
		ur->create_action(p_op == "own_outline" ? "Hewn: Floor Outline" : "Hewn: Add Courtyard", UndoRedo::MERGE_DISABLE, b);
		ur->add_do_method(b, "add_child", c, true);
		ur->add_do_method(c, "set_owner", root);
		ur->add_do_reference(c);
		ur->add_undo_method(b, "remove_child", c);
		if (p_op == "own_outline" && b->get_floor_count() > 0) {
			Ref<HewnBuildingFloor> f = b->get_floor(b->get_floor_count() - 1);
			ur->add_do_method(f.ptr(), "set_outline", NodePath(c->get_name()));
			ur->add_undo_method(f.ptr(), "set_outline", f->get_outline());
		}
		ur->add_do_method(b, "queue_layout");
		ur->add_undo_method(b, "queue_layout");
		ur->commit_action();
		EditorSelection *es = EditorInterface::get_singleton()->get_selection();
		es->clear();
		es->add_node(c);
		return;
	}
	// ---- the selected sides of the outline edited (from each selected point to the next selected one)
	HewnCurve3D *c = hc_edit ? _hc() : nullptr;
	if (!c || c->get_splines_const().empty()) {
		return;
	}
	const hewn::curves::Spline &s = c->get_splines_const()[0];
	const int n = (int)s.pts.size();
	std::vector<int> sides;
	for (int i = 0; i < n; i++) {
		const int j = (i + 1) % n;
		if ((j != 0 || s.cyclic) && s.pts[i].sel && s.pts[j].sel) {
			sides.push_back(i);
		}
	}
	if (sides.empty()) {
		if (EditorToaster *t = EditorInterface::get_singleton()->get_editor_toaster()) {
			t->push_toast(hw_editor::T("Select the two points of a side first"));
		}
		return;
	}
	if (p_op == "gable" || p_op == "slope" || p_op == "by_style") {
		HewnMesh3D *r = b->get_roof_mesh();
		if (!r || c != b->top_outline()) {
			return;
		}
		PackedInt32Array rs = r->get_roof_sides();
		if (rs.size() < n) {
			rs.resize(n);
		}
		for (int i : sides) {
			rs.set(i, p_op == "gable" ? 2 : p_op == "slope" ? 1 : 0);
		}
		ur->create_action("Hewn: Roof Sides", UndoRedo::MERGE_DISABLE, r);
		ur->add_do_method(r, "set_roof_sides", rs);
		ur->add_undo_method(r, "set_roof_sides", r->get_roof_sides());
		ur->add_do_method(b, "queue_layout");
		ur->add_undo_method(b, "queue_layout");
		ur->commit_action();
		return;
	}
	if (p_op == "no_windows" || p_op == "windows") {
		// (the floors on this outline)
		ur->create_action("Hewn: Windows on Sides", UndoRedo::MERGE_DISABLE, b);
		HewnCurve3D *o = b;
		for (int k = 0; k < b->get_floor_count(); k++) {
			const Ref<HewnBuildingFloor> f = b->get_floor(k);
			if (f.is_valid() && !f->get_outline().is_empty()) {
				if (HewnCurve3D *oc = Object::cast_to<HewnCurve3D>(b->get_node_or_null(f->get_outline()))) {
					o = oc;
				}
			}
			if (f.is_null() || o != c) {
				continue;
			}
			PackedInt32Array skip = f->get_no_window_sides();
			for (int i : sides) {
				if (p_op == "no_windows" && !skip.has(i)) {
					skip.push_back(i);
				} else if (p_op == "windows") {
					while (skip.has(i)) {
						skip.remove_at(skip.find(i));
					}
				}
			}
			ur->add_do_method(f.ptr(), "set_no_window_sides", skip);
			ur->add_undo_method(f.ptr(), "set_no_window_sides", f->get_no_window_sides());
		}
		ur->commit_action();
		return;
	}
	if (p_op == "door") {
		// a door: a cutter through the walls in the middle of each side, standing on its floor (move it along)
		int floor_i = 0;
		HewnCurve3D *o = b;
		for (int k = 0; k < b->get_floor_count(); k++) {
			const Ref<HewnBuildingFloor> f = b->get_floor(k);
			if (f.is_valid() && !f->get_outline().is_empty()) {
				if (HewnCurve3D *oc = Object::cast_to<HewnCurve3D>(b->get_node_or_null(f->get_outline()))) {
					o = oc;
				}
			}
			if (o == c) {
				floor_i = k;
				break;
			}
		}
		const Ref<HewnBuildingFloor> f = b->get_floor(floor_i);
		const float ft = f.is_valid() && f->get_floor() ? f->get_floor_thickness() : 0.0f;
		const float wt = f.is_valid() ? f->get_wall_thickness() : 0.2f;
		const Transform3D cb = b->get_global_transform().affine_inverse() * c->get_global_transform();
		ur->create_action("Hewn: Add Door", UndoRedo::MERGE_DISABLE, b);
		for (int i : sides) {
			const Vector3 a = cb.xform(s.pts[i].pos), e = cb.xform(s.pts[(i + 1) % n].pos);
			Vector3 dir = e - a;
			dir.y = 0.0f;
			if (dir.length() < 1e-4f) {
				continue;
			}
			dir.normalize();
			HewnCutter3D *d = memnew(HewnCutter3D);
			d->set_name("Door");
			d->set_meta("hewn_door", true);
			d->set_size(Vector3(1.0f, 2.1f, wt * 3.0f));
			d->set_lining(HewnCutter3D::LINING_AUTO);
			const Vector3 mid = (a + e) * 0.5f;
			const Basis basis(dir, Vector3(0, 1, 0), dir.cross(Vector3(0, 1, 0)));
			d->set_transform(Transform3D(basis, Vector3(mid.x, b->floor_base(floor_i) + ft + 1.05f, mid.z)));
			ur->add_do_method(b, "add_child", d, true);
			ur->add_do_method(d, "set_owner", root);
			ur->add_do_reference(d);
			ur->add_undo_method(b, "remove_child", d);
		}
		ur->commit_action();
	}
}

// Add > Curve > Building: drawn with two clicks (its corners), or placed at the 3D cursor.
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

// A drawn box as a roof (in its place): on the box's bottom, its rectangle the outline, its height the ridge's (by
// the pitch). Also the preview while it is drawn.
void HewnEditorPlugin::_box_to_roof(HewnMesh3D *p_node) const {
	const float w = p_node->get_shape_param("width"), h = p_node->get_shape_param("height"), d = p_node->get_shape_param("depth");
	float min_y = 0.0f;
	for (const Vector3 &q : p_node->get_data().pos) {
		min_y = MIN(min_y, q.y);
	}
	const Transform3D xf = p_node->get_transform();
	p_node->set_shape(hewn::shapes::ROOF);
	const float hw = w * 0.5f, hd = d * 0.5f;
	p_node->set_poly_points(PackedVector3Array({ Vector3(-hw, 0, -hd), Vector3(hw, 0, -hd), Vector3(hw, 0, hd), Vector3(-hw, 0, hd) }));
	const float half = MAX(0.01f, MIN(w, d) * 0.5f);
	p_node->set_shape_param("pitch", CLAMP((float)Math::rad_to_deg(std::atan2(std::fabs(h), half)), 1.0f, 89.0f));
	p_node->set_transform(xf * Transform3D(Basis(), Vector3(0, min_y, 0)));
}

// The drawn shape as what was asked for instead (see shape_make).
void HewnEditorPlugin::_make_from_drawn(HewnMesh3D *p_node, const String &p_what) {
	const Transform3D xf = p_node->get_transform();
	if (p_what == "plan") {
		HewnCurve3D *c = _make_floor_plan(p_node->get_shape_param("width"), p_node->get_shape_param("depth"), xf);
		memdelete(p_node);
		_add_node_with_undo(c, "Add Building");
		return;
	}
	if (p_what == "roof") {
		_box_to_roof(p_node);
		p_node->set_name("Roof");
		_new_object_uv(p_node);
		_add_node_with_undo(p_node, "Create Roof");
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
