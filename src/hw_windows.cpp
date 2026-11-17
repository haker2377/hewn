#include "hw_windows.h"
#include "hw_curve.h"
#include "hw_cutter.h"

#include <godot_cpp/classes/curve3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/path3d.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;

std::vector<HewnWindows3D *> HewnWindows3D::all;

void HewnWindows3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		all.push_back(this);
		queue_rebuild();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		all.erase(std::remove(all.begin(), all.end(), this), all.end());
	}
}

void HewnWindows3D::queue_rebuild() {
	if (queued || !is_inside_tree()) {
		return;
	}
	queued = true;
	callable_mp(this, &HewnWindows3D::rebuild).call_deferred();
}

void HewnWindows3D::set_follow_curve(const NodePath &p_path) {
	if (follow_curve == p_path) {
		return;
	}
	follow_curve = p_path;
	queue_rebuild();
}

void HewnWindows3D::_clear() {
	for (const ObjectID &id : made) {
		if (Node *n = Object::cast_to<Node>(ObjectDB::get_instance(id))) {
			if (n->get_parent()) {
				n->get_parent()->remove_child(n);
			}
			n->queue_free();
		}
	}
	made.clear();
}

// The plan's sides (in this node's space, raised by the lift) and which way is out of each.
bool HewnWindows3D::_sides(std::vector<std::pair<Vector3, Vector3>> &r_sides, std::vector<Vector3> &r_out) const {
	r_sides.clear();
	r_out.clear();
	if (follow_curve.is_empty() || !is_inside_tree()) {
		return false;
	}
	Path3D *p = Object::cast_to<Path3D>(get_node_or_null(follow_curve));
	if (!p || !p->is_inside_tree()) {
		return false;
	}
	const Transform3D to_me = get_global_transform().affine_inverse() * p->get_global_transform() * Transform3D(Basis(), Vector3(0, follow_lift, 0));
	std::vector<Vector3> pts;
	bool cyclic = false;
	if (HewnCurve3D *hc = Object::cast_to<HewnCurve3D>(p)) {
		for (const Vector3 &q : hc->sampled_points(0)) {
			pts.push_back(to_me.xform(q));
		}
		cyclic = !hc->get_splines_const().empty() && hc->get_splines_const()[0].cyclic;
	} else if (p->get_curve().is_valid()) {
		const PackedVector3Array b = p->get_curve()->get_baked_points();
		for (int64_t i = 0; i < b.size(); i++) {
			pts.push_back(to_me.xform(b[i]));
		}
		cyclic = p->get_curve()->is_closed();
	}
	// (one corner for each run in a straight line)
	std::vector<Vector3> c;
	for (const Vector3 &q : pts) {
		if (c.empty() || c.back().distance_to(q) > 1e-4f) {
			c.push_back(q);
		}
	}
	if (cyclic && c.size() > 2 && c.front().distance_to(c.back()) < 1e-4f) {
		c.pop_back();
	}
	if (c.size() < 2) {
		return false;
	}
	std::vector<Vector3> k;
	const int n = (int)c.size();
	for (int i = 0; i < n; i++) {
		if ((!cyclic && (i == 0 || i == n - 1))) {
			k.push_back(c[i]);
			continue;
		}
		const Vector3 a = c[(i + n - 1) % n], b = c[i], d = c[(i + 1) % n];
		const Vector3 e0 = Vector3(b.x - a.x, 0, b.z - a.z).normalized(), e1 = Vector3(d.x - b.x, 0, d.z - b.z).normalized();
		if (e0.cross(e1).length() > 1e-3f || e0.dot(e1) < 0.0f) {
			k.push_back(b);
		}
	}
	float area = 0.0f;
	for (size_t i = 0; i < k.size(); i++) {
		const Vector3 &a = k[i], &b = k[(i + 1) % k.size()];
		area += a.x * b.z - b.x * a.z;
	}
	const int m = (int)k.size();
	const int sides = cyclic ? m : m - 1;
	for (int i = 0; i < sides; i++) {
		const Vector3 a = k[i], b = k[(i + 1) % m];
		const Vector3 d = Vector3(b.x - a.x, 0, b.z - a.z).normalized();
		Vector3 o(d.z, 0.0f, -d.x); // (right of the way)
		if (area < 0.0f) {
			o = -o;
		}
		r_sides.push_back({ a, b });
		r_out.push_back(o);
	}
	return true;
}

std::vector<Transform3D> HewnWindows3D::openings() const {
	std::vector<Transform3D> out;
	std::vector<std::pair<Vector3, Vector3>> sides;
	std::vector<Vector3> outs;
	if (!_sides(sides, outs)) {
		return out;
	}
	for (int i = 0; i < (int)sides.size(); i++) {
		if (skip_sides.has(i)) {
			continue;
		}
		const Vector3 a = sides[i].first, b = sides[i].second;
		const Vector3 ab = b - a;
		const float L = Vector3(ab.x, 0, ab.z).length();
		const float usable = L - 2.0f * margin;
		if (L < 1e-4f || usable < width - 1e-4f) {
			continue;
		}
		std::vector<float> at; // (the centers along the side)
		if (placing == PLACE_COUNT) {
			int nn = std::max(1, count);
			while (nn > 1 && nn * width > usable + 1e-4f) {
				nn--;
			}
			for (int j = 0; j < nn; j++) {
				at.push_back(nn == 1 ? L * 0.5f : margin + width * 0.5f + (usable - width) * (float)j / (float)(nn - 1));
			}
		} else {
			const float sp = std::max(width, spacing);
			const int nn = std::max(1, (int)std::floor((usable - width) / sp + 1e-4f) + 1);
			const float first = L * 0.5f - (nn - 1) * sp * 0.5f;
			for (int j = 0; j < nn; j++) {
				at.push_back(first + j * sp);
			}
		}
		const Vector3 d = ab / L;
		const Vector3 dh = Vector3(d.x, 0, d.z).normalized();
		for (float s : at) {
			const Vector3 base = a + d * s; // (on the plan's line, at its height there)
			const Basis bs(dh, Vector3(0, 1, 0), outs[i]);
			out.push_back(Transform3D(bs, base + Vector3(0, sill + height * 0.5f, 0) + outs[i] * offset));
		}
	}
	return out;
}

int HewnWindows3D::get_opening_count() const {
	return (int)openings().size();
}

void HewnWindows3D::rebuild() {
	queued = false;
	_clear();
	if (!is_inside_tree()) {
		return;
	}
	const std::vector<Transform3D> ops = openings();
	for (const Transform3D &xf : ops) {
		// the scene / mesh in the opening
		Node3D *content = nullptr;
		if (scene.is_valid()) {
			Node *inst = scene->instantiate();
			content = Object::cast_to<Node3D>(inst);
			if (!content && inst) {
				memdelete(inst); // (not a 3D scene: nothing to set in the opening)
			}
		} else if (mesh.is_valid()) {
			MeshInstance3D *mi = memnew(MeshInstance3D);
			mi->set_mesh(mesh);
			content = mi;
		}
		AABB box(Vector3(-width * 0.5f, -height * 0.5f, -depth * 0.5f), Vector3(width, height, depth)); // (the cutter's, in the opening)
		if (content) {
			// its size: what its meshes cover (fitted to the opening when asked)
			AABB cb;
			bool any = false;
			std::vector<Node *> stack = { content };
			while (!stack.empty()) {
				Node *x = stack.back();
				stack.pop_back();
				for (int c = 0; c < x->get_child_count(); c++) {
					stack.push_back(x->get_child(c));
				}
				if (VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(x)) {
					Transform3D rel;
					for (Node *y = x; y && y != content; y = y->get_parent()) {
						if (Node3D *y3 = Object::cast_to<Node3D>(y)) {
							rel = y3->get_transform() * rel;
						}
					}
					const AABB b = rel.xform(vi->get_aabb());
					cb = any ? cb.merge(b) : b;
					any = true;
				}
			}
			Transform3D cx(Basis(), content_offset);
			if (fit && any && cb.size.x > 1e-5f && cb.size.y > 1e-5f) {
				const Vector3 k(width / cb.size.x, height / cb.size.y, 1.0f);
				cx = Transform3D(Basis::from_scale(k), content_offset - cb.get_center() * k);
				cb = AABB(cx.xform(cb.position), cb.size * k);
			} else if (any) {
				cb.position += content_offset;
			}
			content->set_transform(xf * cx * content->get_transform());
			add_child(content, false, INTERNAL_MODE_BACK);
			made.push_back(ObjectID(content->get_instance_id()));
			if (cut == CUT_CONTENT && any) {
				box = cb.grow(cut_margin);
			}
		}
		if (cut != CUT_NONE && (cut == CUT_BOX || content)) {
			HewnCutter3D *c = memnew(HewnCutter3D);
			c->set_size(box.size.max(Vector3(1e-3f, 1e-3f, 1e-3f)));
			c->set_transform(xf * Transform3D(Basis(), box.get_center()));
			add_child(c, false, INTERNAL_MODE_BACK);
			made.push_back(ObjectID(c->get_instance_id()));
		}
	}
}

Node *HewnWindows3D::make_game_node() {
	if (queued) {
		rebuild();
	}
	Node3D *root = memnew(Node3D);
	root->set_name(get_name());
	root->set_transform(get_transform());
	root->set_visible(is_visible());
	for (const ObjectID &id : made) {
		Node3D *n = Object::cast_to<Node3D>(ObjectDB::get_instance(id));
		if (!n || Object::cast_to<HewnCutter3D>(n)) {
			continue; // (the cuts are in the walls by then)
		}
		Node *copy = nullptr;
		if (!n->get_scene_file_path().is_empty() && scene.is_valid()) {
			copy = scene->instantiate();
		} else {
			copy = n->duplicate();
		}
		if (Node3D *c3 = Object::cast_to<Node3D>(copy)) {
			c3->set_transform(n->get_transform());
			root->add_child(c3);
		} else if (copy) {
			memdelete(copy);
		}
	}
	return root;
}

void HewnWindows3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("rebuild"), &HewnWindows3D::rebuild);
	ClassDB::bind_method(D_METHOD("get_opening_count"), &HewnWindows3D::get_opening_count);
	ClassDB::bind_method(D_METHOD("make_game_node"), &HewnWindows3D::make_game_node);
	ClassDB::bind_method(D_METHOD("set_follow_curve", "path"), &HewnWindows3D::set_follow_curve);
	ClassDB::bind_method(D_METHOD("get_follow_curve"), &HewnWindows3D::get_follow_curve);
#define HW_W_BIND(m_name) \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &HewnWindows3D::set_##m_name); \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &HewnWindows3D::get_##m_name);
	HW_W_BIND(follow_lift)
	HW_W_BIND(width)
	HW_W_BIND(height)
	HW_W_BIND(sill)
	HW_W_BIND(depth)
	HW_W_BIND(offset)
	HW_W_BIND(placing)
	HW_W_BIND(spacing)
	HW_W_BIND(count)
	HW_W_BIND(margin)
	HW_W_BIND(fit)
	HW_W_BIND(content_offset)
	HW_W_BIND(cut)
	HW_W_BIND(cut_margin)
	HW_W_BIND(skip_sides)
	HW_W_BIND(scene)
	HW_W_BIND(mesh)
#undef HW_W_BIND
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_curve", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Path3D"), "set_follow_curve", "get_follow_curve");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "follow_lift", PROPERTY_HINT_RANGE, "-1000,1000,0.01,or_less,or_greater,suffix:m"), "set_follow_lift", "get_follow_lift");
	ADD_GROUP("Opening", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "width", PROPERTY_HINT_RANGE, "0.01,100,0.01,or_greater,suffix:m"), "set_width", "get_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height", PROPERTY_HINT_RANGE, "0.01,100,0.01,or_greater,suffix:m"), "set_height", "get_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sill", PROPERTY_HINT_RANGE, "-100,100,0.01,or_less,or_greater,suffix:m"), "set_sill", "get_sill");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "depth", PROPERTY_HINT_RANGE, "0.01,100,0.01,or_greater,suffix:m"), "set_depth", "get_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "offset", PROPERTY_HINT_RANGE, "-100,100,0.01,or_less,or_greater,suffix:m"), "set_offset", "get_offset");
	ADD_GROUP("Placing", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "placing", PROPERTY_HINT_ENUM, "By Spacing,By Count"), "set_placing", "get_placing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spacing", PROPERTY_HINT_RANGE, "0.01,100,0.01,or_greater,suffix:m"), "set_spacing", "get_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "count", PROPERTY_HINT_RANGE, "1,100,1,or_greater"), "set_count", "get_count");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "margin", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_margin", "get_margin");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "skip_sides"), "set_skip_sides", "get_skip_sides");
	ADD_GROUP("Content", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "scene", PROPERTY_HINT_RESOURCE_TYPE, "PackedScene"), "set_scene", "get_scene");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, "Mesh"), "set_mesh", "get_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fit"), "set_fit", "get_fit");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "content_offset", PROPERTY_HINT_NONE, "suffix:m"), "set_content_offset", "get_content_offset");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cut", PROPERTY_HINT_ENUM, "None,Box,Around the Content"), "set_cut", "get_cut");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cut_margin", PROPERTY_HINT_RANGE, "-10,10,0.001,or_less,or_greater,suffix:m"), "set_cut_margin", "get_cut_margin");
	BIND_ENUM_CONSTANT(PLACE_SPACING);
	BIND_ENUM_CONSTANT(PLACE_COUNT);
	BIND_ENUM_CONSTANT(CUT_NONE);
	BIND_ENUM_CONSTANT(CUT_BOX);
	BIND_ENUM_CONSTANT(CUT_CONTENT);
}
