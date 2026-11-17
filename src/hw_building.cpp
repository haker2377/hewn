#include "hw_building.h"
#include "hewn_mesh.h"
#include "hw_cutter.h"
#include "hw_windows.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <cmath>

using namespace godot;

void HewnBuildingFloor::_bind_methods() {
#define HW_BIND(m_name)                                                                                   \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &HewnBuildingFloor::set_##m_name); \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &HewnBuildingFloor::get_##m_name);
	HW_BIND(wall_height)
	HW_BIND(floor_thickness)
	HW_BIND(wall_thickness)
	HW_BIND(floor)
	HW_BIND(walls)
	HW_BIND(windows)
	HW_BIND(window_width)
	HW_BIND(window_height)
	HW_BIND(window_sill)
	HW_BIND(window_spacing)
	HW_BIND(no_window_sides)
	HW_BIND(outline)
#undef HW_BIND
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wall_height", PROPERTY_HINT_RANGE, "0.1,100,0.01,or_greater,suffix:m"), "set_wall_height", "get_wall_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "floor_thickness", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:m"), "set_floor_thickness", "get_floor_thickness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wall_thickness", PROPERTY_HINT_RANGE, "0.01,10,0.01,or_greater,suffix:m"), "set_wall_thickness", "get_wall_thickness");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "floor"), "set_floor", "get_floor");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "walls"), "set_walls", "get_walls");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "windows"), "set_windows", "get_windows");
	ADD_GROUP("Windows", "window_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "window_width", PROPERTY_HINT_RANGE, "0.05,100,0.01,or_greater,suffix:m"), "set_window_width", "get_window_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "window_height", PROPERTY_HINT_RANGE, "0.05,100,0.01,or_greater,suffix:m"), "set_window_height", "get_window_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "window_sill", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_window_sill", "get_window_sill");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "window_spacing", PROPERTY_HINT_RANGE, "0.1,100,0.01,or_greater,suffix:m"), "set_window_spacing", "get_window_spacing");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "no_window_sides"), "set_no_window_sides", "get_no_window_sides");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "outline"), "set_outline", "get_outline");
}

// ------------------------------------------------------------------ the building

void HewnBuilding3D::set_floors(const TypedArray<Resource> &p_v) {
	const Callable c = callable_mp(this, &HewnBuilding3D::_on_floor_changed);
	for (int64_t i = 0; i < floors.size(); i++) {
		Ref<Resource> r = floors[i];
		if (r.is_valid() && r->is_connected("changed", c)) {
			r->disconnect("changed", c);
		}
	}
	floors = p_v;
	for (int64_t i = 0; i < floors.size(); i++) {
		Ref<Resource> r = floors[i];
		if (r.is_valid() && !r->is_connected("changed", c)) {
			r->connect("changed", c);
		}
	}
	queue_layout();
}

Ref<HewnBuildingFloor> HewnBuilding3D::get_floor(int p_i) const {
	return p_i >= 0 && p_i < floors.size() ? Ref<HewnBuildingFloor>(floors[p_i]) : Ref<HewnBuildingFloor>();
}

float HewnBuilding3D::floor_base(int p_floor) const {
	float y = 0.0f;
	for (int i = 0; i < p_floor && i < floors.size(); i++) {
		const Ref<HewnBuildingFloor> f = floors[i];
		if (f.is_valid()) {
			y += (f->get_floor() ? f->get_floor_thickness() : 0.0f) + f->get_wall_height();
		}
	}
	return y;
}

float HewnBuilding3D::top() const {
	return floor_base((int)floors.size());
}

void HewnBuilding3D::queue_layout() {
	if (layout_queued || laying_out || !is_inside_tree() || !Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	layout_queued = true;
	callable_mp(this, &HewnBuilding3D::layout).call_deferred();
}

void HewnBuilding3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		queue_layout();
	}
}

Node *HewnBuilding3D::_part(Node *p_under, const String &p_key) const {
	for (int i = 0; i < p_under->get_child_count(); i++) {
		Node *c = p_under->get_child(i);
		if (c->has_meta("hewn_part") && String(c->get_meta("hewn_part")) == p_key) {
			return c;
		}
	}
	return nullptr;
}

template <class T>
T *HewnBuilding3D::_ensure(Node *p_under, const String &p_key, const String &p_name, bool &r_made) {
	r_made = false;
	if (T *t = Object::cast_to<T>(_part(p_under, p_key))) {
		return t;
	}
	T *t = memnew(T);
	t->set_name(p_name);
	t->set_meta("hewn_part", p_key);
	p_under->add_child(t, true);
	Node *owner = get_owner() ? get_owner() : this;
	t->set_owner(owner);
	r_made = true;
	return t;
}

void HewnBuilding3D::_drop(Node *p_under, const String &p_key) {
	if (Node *n = _part(p_under, p_key)) {
		p_under->remove_child(n);
		n->queue_free();
	}
}

HewnCurve3D *HewnBuilding3D::_floor_outline(int p_floor, HewnCurve3D *p_below) const {
	const Ref<HewnBuildingFloor> f = get_floor(p_floor);
	if (f.is_valid() && !f->get_outline().is_empty()) {
		if (HewnCurve3D *c = Object::cast_to<HewnCurve3D>(get_node_or_null(f->get_outline()))) {
			return c;
		}
	}
	return p_below;
}

HewnCurve3D *HewnBuilding3D::top_outline() const {
	HewnCurve3D *c = const_cast<HewnBuilding3D *>(this);
	for (int i = 0; i < floors.size(); i++) {
		c = _floor_outline(i, c);
	}
	return c;
}

HewnMesh3D *HewnBuilding3D::get_roof_mesh() const {
	return Object::cast_to<HewnMesh3D>(_part(const_cast<HewnBuilding3D *>(this), "roof"));
}

namespace {
void set_param(HewnMesh3D *m, const char *p_name, float p_v) {
	if (std::fabs(m->get_shape_param(p_name) - p_v) > 1e-6f) {
		m->set_shape_param(p_name, p_v);
	}
}
void follow(Node *p_part, Node *p_curve, float p_lift) {
	const NodePath path = p_part->get_parent() == p_curve ? NodePath("..") : p_part->get_path_to(p_curve);
	if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(p_part)) {
		if (std::fabs(m->get_follow_lift() - p_lift) > 1e-6f) {
			m->set_follow_lift(p_lift);
		}
		if (m->get_follow_curve() != path) {
			m->set_follow_curve(path);
		}
	} else if (HewnWindows3D *w = Object::cast_to<HewnWindows3D>(p_part)) {
		if (std::fabs(w->get_follow_lift() - p_lift) > 1e-6f) {
			w->set_follow_lift(p_lift);
		}
		if (w->get_follow_curve() != path) {
			w->set_follow_curve(path);
		}
	}
}
void put_material(HewnMesh3D *m, const Ref<Material> &p_mat, bool p_made) {
	if (p_made && p_mat.is_valid()) {
		m->apply_material(p_mat, PackedInt32Array());
	}
}
int side_count(HewnCurve3D *c) {
	return c->get_splines_const().empty() ? 0 : (int)c->get_splines_const()[0].pts.size();
}
} // namespace

// Every part where it belongs: made when missing, set when off, taken away when no longer wanted.
void HewnBuilding3D::layout() {
	layout_queued = false;
	if (!is_inside_tree() || !Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	laying_out = true;
	bool made = false;
	set_layout_of(ObjectID(get_instance_id()));
	for (int i = 0; i < get_child_count(); i++) {
		if (HewnCurve3D *c = Object::cast_to<HewnCurve3D>(get_child(i))) {
			c->set_layout_of(ObjectID(get_instance_id())); // (its floors' outlines, its courtyards)
		}
	}
	HewnCurve3D *outline = this;
	const int nf = (int)floors.size();
	float last_wall_t = 0.2f;
	std::vector<HewnCurve3D *> holes;
	for (int i = 0; i < get_child_count(); i++) {
		HewnCurve3D *c = Object::cast_to<HewnCurve3D>(get_child(i));
		if (c && c->has_meta("hewn_courtyard")) {
			holes.push_back(c);
		}
	}
	for (int i = 0; i < nf; i++) {
		const Ref<HewnBuildingFloor> f = floors[i];
		const String sfx = i == 0 ? String() : itos(i + 1);
		const String k = itos(i);
		if (f.is_null()) {
			_drop(this, "floor:" + k);
			_drop(this, "walls:" + k);
			_drop(this, "windows:" + k);
			continue;
		}
		outline = _floor_outline(i, outline);
		const float base = floor_base(i);
		const float ft = f->get_floor() ? f->get_floor_thickness() : 0.0f;
		last_wall_t = f->get_wall_thickness();
		if (f->get_floor() && ft > 1e-4f) {
			HewnMesh3D *m = _ensure<HewnMesh3D>(this, "floor:" + k, "Floor" + sfx, made);
			if (made) {
				m->set_shape(hewn::shapes::POLY_SHAPE);
				m->set_collision_mode(HewnMesh3D::COLLISION_TRIMESH);
			}
			put_material(m, floor_material, made);
			set_param(m, "height", ft);
			follow(m, outline, base);
		} else {
			_drop(this, "floor:" + k);
		}
		if (f->get_walls()) {
			HewnMesh3D *m = _ensure<HewnMesh3D>(this, "walls:" + k, "Walls" + sfx, made);
			if (made) {
				m->set_shape(hewn::shapes::WALL);
				m->set_collision_mode(HewnMesh3D::COLLISION_TRIMESH);
			}
			put_material(m, wall_material, made);
			set_param(m, "height", f->get_wall_height());
			set_param(m, "thickness", f->get_wall_thickness());
			set_param(m, "side", 0); // (inside the plan: its line the outer face)
			follow(m, outline, base + ft);
		} else {
			_drop(this, "walls:" + k);
		}
		if (f->get_walls() && f->get_windows()) {
			HewnWindows3D *w = _ensure<HewnWindows3D>(this, "windows:" + k, "Windows" + sfx, made);
			const float t = f->get_wall_thickness();
			w->set_depth(t * 4.0f);
			w->set_offset(-t * 0.5f); // (in the middle of the walls)
			w->set_width(f->get_window_width());
			w->set_height(f->get_window_height());
			w->set_sill(f->get_window_sill());
			w->set_spacing(f->get_window_spacing());
			w->set_skip_sides(f->get_no_window_sides());
			follow(w, outline, base + ft);
		} else {
			_drop(this, "windows:" + k);
		}
		// round each courtyard: walls on this floor, facing into it
		for (HewnCurve3D *h : holes) {
			if (f->get_walls()) {
				HewnMesh3D *m = _ensure<HewnMesh3D>(h, "walls:" + k, "Walls" + sfx, made);
				if (made) {
					m->set_shape(hewn::shapes::WALL);
					m->set_collision_mode(HewnMesh3D::COLLISION_TRIMESH);
				}
				put_material(m, wall_material, made);
				set_param(m, "height", f->get_wall_height());
				set_param(m, "thickness", f->get_wall_thickness());
				set_param(m, "side", 2); // (outside the courtyard's line: in the building)
				follow(m, h, base + ft - h->get_position().y);
			} else {
				_drop(h, "walls:" + k);
			}
		}
	}
	// what is left of floors taken away
	std::vector<Node *> unders = { this };
	unders.insert(unders.end(), holes.begin(), holes.end());
	for (Node *under : unders) {
		for (int i = under->get_child_count() - 1; i >= 0; i--) {
			Node *c = under->get_child(i);
			if (!c->has_meta("hewn_part")) {
				continue;
			}
			const String key = c->get_meta("hewn_part");
			const String kind = key.get_slicec(':', 0);
			if ((kind == "floor" || kind == "walls" || kind == "windows") && key.get_slicec(':', 1).to_int() >= nf) {
				under->remove_child(c);
				c->queue_free();
			}
		}
	}
	const float tp = top();
	HewnCurve3D *top_c = outline;
	if (roof && nf > 0) {
		HewnMesh3D *r = _ensure<HewnMesh3D>(this, "roof", "Roof", made);
		if (made) {
			r->set_shape(hewn::shapes::ROOF);
			r->set_collision_mode(HewnMesh3D::COLLISION_TRIMESH);
		}
		put_material(r, roof_material, made);
		set_param(r, "wall_thickness", last_wall_t);
		follow(r, top_c, tp);
	} else {
		_drop(this, "roof");
	}
	if (cornice && nf > 0) {
		HewnMesh3D *s = _ensure<HewnMesh3D>(this, "cornice", "Cornice", made);
		if (made) {
			s->set_shape(hewn::shapes::SWEEP);
			s->set_shape_param("profile", 4);
			s->set_shape_param("align", 1);
			s->set_shape_param("closed", 1);
			s->set_shape_param("width", 0.25f);
			s->set_shape_param("height", 0.25f);
			s->set_shape_param("drop", 0.25f); // (under the walls' top, against them)
		}
		put_material(s, wall_material, made);
		follow(s, top_c, tp);
	} else {
		_drop(this, "cornice");
	}
	if (plinth && nf > 0) {
		HewnMesh3D *s = _ensure<HewnMesh3D>(this, "plinth", "Plinth", made);
		if (made) {
			s->set_shape(hewn::shapes::SWEEP);
			s->set_shape_param("profile", 1);
			s->set_shape_param("align", 1);
			s->set_shape_param("closed", 1);
			s->set_shape_param("width", 0.08f);
		}
		set_param(s, "height", plinth_height);
		put_material(s, wall_material, made);
		follow(s, this, 0.0f);
	} else {
		_drop(this, "plinth");
	}
	// windows in the gables: one in the middle of each
	HewnMesh3D *rm = get_roof_mesh();
	if (gable_windows && rm && nf > 0) {
		HewnWindows3D *w = _ensure<HewnWindows3D>(this, "gable_windows", "GableWindows", made);
		if (made) {
			w->set_width(0.8f);
			w->set_height(0.9f);
			w->set_sill(0.25f);
		}
		w->set_placing(HewnWindows3D::PLACE_COUNT);
		w->set_count(1);
		w->set_depth(last_wall_t * 4.0f);
		w->set_offset(-last_wall_t * 0.5f);
		const PackedInt32Array g = rm->get_roof_gable_sides();
		PackedInt32Array skip;
		for (int i = 0; i < side_count(top_c); i++) {
			if (!g.has(i)) {
				skip.push_back(i);
			}
		}
		if (skip != w->get_skip_sides()) {
			w->set_skip_sides(skip);
		}
		follow(w, top_c, tp);
	} else {
		_drop(this, "gable_windows");
	}
	// the courtyards cut through it all, from the ground to over the roof
	for (HewnCurve3D *h : holes) {
		HewnCutter3D *cut = _ensure<HewnCutter3D>(h, "cut", "Cut", made);
		if (made) {
			cut->set_shape(HewnCutter3D::SHAPE_PRISM);
			cut->set_lining(HewnCutter3D::LINING_NONE);
			cut->set("cut_ends", false); // (the ground under it stays)
		}
		PackedVector2Array pts;
		for (const Vector3 &q : h->sampled_points(0)) {
			pts.push_back(Vector2(q.x, q.z));
		}
		if (pts != cut->get("points").operator PackedVector2Array()) {
			cut->set_points(pts);
		}
		const Vector3 sz(1.0f, tp + 50.0f - h->get_position().y, 1.0f);
		if (!cut->get("size").operator Vector3().is_equal_approx(sz)) {
			cut->set_size(sz);
		}
	}
	laying_out = false;
}

void HewnBuilding3D::set_floor_material(const Ref<Material> &p_v) {
	floor_material = p_v;
	for (int i = 0; i < floors.size() && p_v.is_valid(); i++) {
		if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(_part(this, "floor:" + itos(i)))) {
			m->apply_material(p_v, PackedInt32Array());
		}
	}
}

void HewnBuilding3D::set_wall_material(const Ref<Material> &p_v) {
	wall_material = p_v;
	if (p_v.is_null()) {
		return;
	}
	// its walls, the courtyards' walls, the cornice and the plinth
	std::vector<Node *> unders = { this };
	for (int i = 0; i < get_child_count(); i++) {
		if (get_child(i)->has_meta("hewn_courtyard")) {
			unders.push_back(get_child(i));
		}
	}
	for (Node *u : unders) {
		for (int i = 0; i < u->get_child_count(); i++) {
			Node *c = u->get_child(i);
			const String key = c->has_meta("hewn_part") ? String(c->get_meta("hewn_part")) : String();
			if (key.begins_with("walls:") || key == "cornice" || key == "plinth") {
				if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(c)) {
					m->apply_material(p_v, PackedInt32Array());
				}
			}
		}
	}
}

void HewnBuilding3D::set_roof_material(const Ref<Material> &p_v) {
	roof_material = p_v;
	if (HewnMesh3D *r = get_roof_mesh(); r && p_v.is_valid()) {
		r->apply_material(p_v, PackedInt32Array());
	}
}

void HewnBuilding3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_floors", "floors"), &HewnBuilding3D::set_floors);
	ClassDB::bind_method(D_METHOD("get_floors"), &HewnBuilding3D::get_floors);
	ClassDB::bind_method(D_METHOD("layout"), &HewnBuilding3D::layout);
	ClassDB::bind_method(D_METHOD("queue_layout"), &HewnBuilding3D::queue_layout);
	ClassDB::bind_method(D_METHOD("floor_base", "floor"), &HewnBuilding3D::floor_base);
	ClassDB::bind_method(D_METHOD("top"), &HewnBuilding3D::top);
#define HW_BIND(m_name)                                                                                 \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &HewnBuilding3D::set_##m_name); \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &HewnBuilding3D::get_##m_name);
	HW_BIND(roof)
	HW_BIND(cornice)
	HW_BIND(plinth)
	HW_BIND(plinth_height)
	HW_BIND(gable_windows)
	HW_BIND(floor_material)
	HW_BIND(wall_material)
	HW_BIND(roof_material)
#undef HW_BIND
	ADD_GROUP("Building", "");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "floors", PROPERTY_HINT_TYPE_STRING, String::num_int64(Variant::OBJECT) + "/" + String::num_int64(PROPERTY_HINT_RESOURCE_TYPE) + ":HewnBuildingFloor"),
			"set_floors", "get_floors");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "roof"), "set_roof", "get_roof");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cornice"), "set_cornice", "get_cornice");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "plinth"), "set_plinth", "get_plinth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "plinth_height", PROPERTY_HINT_RANGE, "0.01,10,0.01,or_greater,suffix:m"), "set_plinth_height", "get_plinth_height");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "gable_windows"), "set_gable_windows", "get_gable_windows");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "floor_material", PROPERTY_HINT_RESOURCE_TYPE, "Material"), "set_floor_material", "get_floor_material");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "wall_material", PROPERTY_HINT_RESOURCE_TYPE, "Material"), "set_wall_material", "get_wall_material");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "roof_material", PROPERTY_HINT_RESOURCE_TYPE, "Material"), "set_roof_material", "get_roof_material");
	ADD_GROUP("", "");
}
