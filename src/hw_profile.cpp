#include "hw_profile.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void HewnProfile::set_points(const PackedVector2Array &p_points) {
	points = p_points;
	emit_changed();
}

std::vector<Vector2> HewnProfile::to_vector() const {
	const PackedVector2Array s = sample();
	std::vector<Vector2> out;
	for (int64_t i = 0; i < s.size(); i++) {
		out.push_back(s[i]);
	}
	return out;
}

void HewnProfile::set_handle_types(const PackedInt32Array &p_types) {
	handle_types = p_types;
	emit_changed();
}

void HewnProfile::set_handles_in(const PackedVector2Array &p_h) {
	handles_in = p_h;
	emit_changed();
}

void HewnProfile::set_handles_out(const PackedVector2Array &p_h) {
	handles_out = p_h;
	emit_changed();
}

void HewnProfile::set_resolution(int p_res) {
	resolution = CLAMP(p_res, 1, 64);
	emit_changed();
}

void HewnProfile::set_sample_straight(bool p_on) {
	sample_straight = p_on;
	emit_changed();
}

void HewnProfile::set_sample_equal(bool p_on) {
	sample_equal = p_on;
	emit_changed();
}

int HewnProfile::get_handle_type(int p_index) const {
	return p_index >= 0 && p_index < handle_types.size() ? handle_types[p_index] : HANDLE_VECTOR;
}

// Auto: along the line through the neighbors, a third of the way to each; Vector: a third of the way
// straight to the neighbor (a straight piece); Free / Aligned: as stored.
Vector2 HewnProfile::handle_in(int p_index) const {
	const int n = (int)points.size();
	if (p_index <= 0 || p_index >= n) {
		return Vector2();
	}
	const int t = get_handle_type(p_index);
	const Vector2 p = points[p_index], prev = points[p_index - 1];
	if (t == HANDLE_FREE || t == HANDLE_ALIGNED) {
		return p_index < handles_in.size() ? handles_in[p_index] : Vector2();
	}
	if (t == HANDLE_AUTO && p_index + 1 < n) {
		const Vector2 dir = (points[p_index + 1] - prev).normalized();
		return -dir * (p.distance_to(prev) / 3.0f);
	}
	return (prev - p) / 3.0f;
}

Vector2 HewnProfile::handle_out(int p_index) const {
	const int n = (int)points.size();
	if (p_index < 0 || p_index >= n - 1) {
		return Vector2();
	}
	const int t = get_handle_type(p_index);
	const Vector2 p = points[p_index], next = points[p_index + 1];
	if (t == HANDLE_FREE || t == HANDLE_ALIGNED) {
		return p_index < handles_out.size() ? handles_out[p_index] : Vector2();
	}
	if (t == HANDLE_AUTO && p_index > 0) {
		const Vector2 dir = (next - points[p_index - 1]).normalized();
		return dir * (p.distance_to(next) / 3.0f);
	}
	return (next - p) / 3.0f;
}

PackedVector2Array HewnProfile::sample() const {
	PackedVector2Array out;
	const int n = (int)points.size();
	if (n == 0) {
		return out;
	}
	out.push_back(points[0]);
	for (int i = 0; i + 1 < n; i++) {
		const Vector2 p0 = points[i], p3 = points[i + 1];
		const Vector2 p1 = p0 + handle_out(i), p2 = p3 + handle_in(i + 1);
		const bool straight = get_handle_type(i) == HANDLE_VECTOR && get_handle_type(i + 1) == HANDLE_VECTOR;
		const int steps = straight && !sample_straight ? 1 : resolution;
		for (int k = 1; k <= steps; k++) {
			const float t = (float)k / steps, u = 1.0f - t;
			out.push_back(p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t));
		}
	}
	if (sample_equal && out.size() > 2) {
		// the same number of points, at equal lengths along the curve
		std::vector<float> acc(out.size(), 0.0f);
		for (int64_t i = 1; i < out.size(); i++) {
			acc[i] = acc[i - 1] + out[i - 1].distance_to(out[i]);
		}
		const float total = acc.back();
		PackedVector2Array eq;
		const int count = (int)out.size();
		int seg = 0;
		for (int k = 0; k < count; k++) {
			const float d = total * k / (count - 1);
			while (seg + 2 < count && acc[seg + 1] < d) {
				seg++;
			}
			const float len = acc[seg + 1] - acc[seg];
			const float t = len > 1e-9f ? CLAMP((d - acc[seg]) / len, 0.0f, 1.0f) : 0.0f;
			eq.push_back(out[seg].lerp(out[seg + 1], t));
		}
		return eq;
	}
	return out;
}

void HewnProfile::set_symmetric(bool p_on) {
	symmetric = p_on;
	if (symmetric) {
		symmetrize(true);
	} else {
		emit_changed();
	}
}

void HewnProfile::symmetrize(bool p_from_left) {
	const int n = (int)points.size();
	struct Pt {
		Vector2 p, hin, hout;
		int type;
	};
	auto get = [&](int i) {
		return Pt{ points[i], i < handles_in.size() ? handles_in[i] : Vector2(), i < handles_out.size() ? handles_out[i] : Vector2(), get_handle_type(i) };
	};
	auto mirror = [](const Pt &q) { return Pt{ Vector2(1.0f - q.p.x, q.p.y), Vector2(-q.hout.x, q.hout.y), Vector2(-q.hin.x, q.hin.y), q.type }; };
	std::vector<Pt> half;
	int i = 0;
	if (p_from_left) {
		while (i < n && points[i].x < 0.5f - 1e-4f) {
			half.push_back(get(i++));
		}
	} else {
		i = n - 1;
		while (i >= 0 && points[i].x > 0.5f + 1e-4f) {
			half.push_back(mirror(get(i--)));
		}
	}
	// a point on the middle line stays (on it, its handles mirrored)
	bool has_mid = false;
	Pt mid;
	if (i >= 0 && i < n && std::fabs(points[i].x - 0.5f) <= 0.02f) {
		mid = get(i);
		mid.p.x = 0.5f;
		mid.hin = p_from_left ? Vector2(-mid.hout.x, mid.hout.y) : mid.hin;
		mid.hout = p_from_left ? mid.hout : Vector2(-mid.hin.x, mid.hin.y);
		has_mid = true;
	}
	std::vector<Pt> all = half;
	if (has_mid) {
		all.push_back(mid);
	}
	for (int k = (int)half.size() - 1; k >= 0; k--) {
		all.push_back(mirror(half[k]));
	}
	if (all.size() < 2) {
		emit_changed();
		return;
	}
	points.resize((int64_t)all.size());
	handle_types.resize((int64_t)all.size());
	handles_in.resize((int64_t)all.size());
	handles_out.resize((int64_t)all.size());
	for (size_t k = 0; k < all.size(); k++) {
		points.set((int64_t)k, all[k].p);
		handle_types.set((int64_t)k, all[k].type);
		handles_in.set((int64_t)k, all[k].hin);
		handles_out.set((int64_t)k, all[k].hout);
	}
	emit_changed();
}

int HewnProfile::insert_point(int p_at, const Vector2 &p_pos) {
	const int at = CLAMP(p_at, 0, (int)points.size());
	const int n = (int)points.size();
	handle_types.resize(n);
	handles_in.resize(n);
	handles_out.resize(n);
	points.insert(at, p_pos);
	handle_types.insert(at, n > 0 && get_handle_type(MAX(0, at - 1)) != HANDLE_VECTOR ? (int)HANDLE_AUTO : (int)HANDLE_VECTOR);
	handles_in.insert(at, Vector2());
	handles_out.insert(at, Vector2());
	if (symmetric) {
		const bool left = p_pos.x <= 0.5f;
		symmetrize(left);
		return left ? at : at + 1; // (on the right: one more point on the left now)
	}
	emit_changed();
	return at;
}

void HewnProfile::remove_point(int p_index) {
	if (p_index < 0 || p_index >= points.size()) {
		return;
	}
	const bool left = !symmetric || _left_index(p_index) || p_index * 2 == (int)points.size() - 1;
	points.remove_at(p_index);
	if (p_index < handle_types.size()) {
		handle_types.remove_at(p_index);
	}
	if (p_index < handles_in.size()) {
		handles_in.remove_at(p_index);
	}
	if (p_index < handles_out.size()) {
		handles_out.remove_at(p_index);
	}
	if (symmetric) {
		symmetrize(left);
		return;
	}
	emit_changed();
}

void HewnProfile::set_point(int p_index, const Vector2 &p_pos) {
	if (p_index >= 0 && p_index < points.size()) {
		Vector2 q = p_pos;
		const int n = (int)points.size();
		if (symmetric) {
			// each half stays on its side; the middle point on the line
			if (p_index * 2 == n - 1) {
				q.x = 0.5f;
			} else if (_left_index(p_index)) {
				q.x = MIN(q.x, 0.5f - 1e-3f);
			} else {
				q.x = MAX(q.x, 0.5f + 1e-3f);
			}
		}
		points.set(p_index, q);
		if (symmetric) {
			symmetrize(p_index * 2 <= n - 1);
			return;
		}
		emit_changed();
	}
}

void HewnProfile::set_point_handle_type(int p_index, int p_type) {
	const int n = (int)points.size();
	if (p_index < 0 || p_index >= n) {
		return;
	}
	// what the handles are now becomes the stored ones (switching to Free / Aligned keeps the shape)
	const Vector2 hin = handle_in(p_index), hout = handle_out(p_index);
	handle_types.resize(n);
	handles_in.resize(n);
	handles_out.resize(n);
	for (int i = 0; i < n; i++) {
		if (i >= 0 && handle_types[i] < 0) {
			handle_types.set(i, HANDLE_VECTOR);
		}
	}
	handle_types.set(p_index, CLAMP(p_type, 0, 3));
	handles_in.set(p_index, hin);
	handles_out.set(p_index, hout);
	if (p_type == HANDLE_ALIGNED && hin.length() > 1e-9f && hout.length() > 1e-9f) {
		handles_in.set(p_index, -hout.normalized() * hin.length());
	}
	if (symmetric) {
		symmetrize(p_index * 2 <= n - 1);
		return;
	}
	emit_changed();
}

void HewnProfile::set_point_handle(int p_index, bool p_out, const Vector2 &p_offset) {
	const int n = (int)points.size();
	if (p_index < 0 || p_index >= n) {
		return;
	}
	if (get_handle_type(p_index) == HANDLE_AUTO || get_handle_type(p_index) == HANDLE_VECTOR) {
		set_point_handle_type(p_index, HANDLE_ALIGNED);
	}
	(p_out ? handles_out : handles_in).set(p_index, p_offset);
	if (get_handle_type(p_index) == HANDLE_ALIGNED) {
		PackedVector2Array &other = p_out ? handles_in : handles_out;
		const float len = other[p_index].length();
		if (p_offset.length() > 1e-9f) {
			other.set(p_index, -p_offset.normalized() * len);
		}
	}
	if (symmetric) {
		// (the middle point: the handle moved is the one kept)
		symmetrize(p_index * 2 == n - 1 ? p_out : p_index * 2 < n - 1);
		return;
	}
	emit_changed();
}

Dictionary HewnProfile::get_state() const {
	Dictionary d;
	d["points"] = points;
	d["handle_types"] = handle_types;
	d["handles_in"] = handles_in;
	d["handles_out"] = handles_out;
	d["resolution"] = resolution;
	d["sample_straight"] = sample_straight;
	d["sample_equal"] = sample_equal;
	d["symmetric"] = symmetric;
	return d;
}

void HewnProfile::set_state(const Dictionary &p_state) {
	points = p_state.get("points", PackedVector2Array());
	handle_types = p_state.get("handle_types", PackedInt32Array());
	handles_in = p_state.get("handles_in", PackedVector2Array());
	handles_out = p_state.get("handles_out", PackedVector2Array());
	resolution = p_state.get("resolution", 8);
	sample_straight = p_state.get("sample_straight", false);
	sample_equal = p_state.get("sample_equal", false);
	symmetric = p_state.get("symmetric", false);
	emit_changed();
}

// Curve: points (x, y) with left / right tangents (slopes); linear tangents are straight pieces.
void HewnProfile::from_curve(const Ref<Curve> &p_curve) {
	if (p_curve.is_null()) {
		return;
	}
	const int n = p_curve->get_point_count();
	points.resize(n);
	handle_types.resize(n);
	handles_in.resize(n);
	handles_out.resize(n);
	for (int i = 0; i < n; i++) {
		const Vector2 p = p_curve->get_point_position(i);
		points.set(i, p);
		const bool lin_l = p_curve->get_point_left_mode(i) == Curve::TANGENT_LINEAR, lin_r = p_curve->get_point_right_mode(i) == Curve::TANGENT_LINEAR;
		handle_types.set(i, lin_l && lin_r ? (int)HANDLE_VECTOR : (int)HANDLE_FREE);
		const float dl = i > 0 ? (p.x - p_curve->get_point_position(i - 1).x) / 3.0f : 0.0f;
		const float dr = i + 1 < n ? (p_curve->get_point_position(i + 1).x - p.x) / 3.0f : 0.0f;
		handles_in.set(i, Vector2(-dl, -dl * p_curve->get_point_left_tangent(i)));
		handles_out.set(i, Vector2(dr, dr * p_curve->get_point_right_tangent(i)));
	}
	emit_changed();
}

void HewnProfile::from_curve2d(const Ref<Curve2D> &p_curve) {
	if (p_curve.is_null()) {
		return;
	}
	const int n = p_curve->get_point_count();
	points.resize(n);
	handle_types.resize(n);
	handles_in.resize(n);
	handles_out.resize(n);
	for (int i = 0; i < n; i++) {
		points.set(i, p_curve->get_point_position(i));
		const Vector2 hin = p_curve->get_point_in(i), hout = p_curve->get_point_out(i);
		handles_in.set(i, hin);
		handles_out.set(i, hout);
		handle_types.set(i, hin == Vector2() && hout == Vector2() ? (int)HANDLE_VECTOR : (int)HANDLE_FREE);
	}
	emit_changed();
}

Ref<Curve> HewnProfile::to_curve() const {
	Ref<Curve> c;
	c.instantiate();
	float lo = 0.0f, hi = 1.0f;
	for (int64_t i = 0; i < points.size(); i++) {
		lo = MIN(lo, points[i].y);
		hi = MAX(hi, points[i].y);
	}
	c->set_min_value(lo);
	c->set_max_value(hi);
	for (int64_t i = 0; i < points.size(); i++) {
		const Vector2 hin = handle_in((int)i), hout = handle_out((int)i);
		const float lt = std::fabs(hin.x) > 1e-9f ? hin.y / hin.x : 0.0f;
		const float rt = std::fabs(hout.x) > 1e-9f ? hout.y / hout.x : 0.0f;
		const bool lin = get_handle_type((int)i) == HANDLE_VECTOR;
		c->add_point(points[i], lt, rt, lin ? Curve::TANGENT_LINEAR : Curve::TANGENT_FREE, lin ? Curve::TANGENT_LINEAR : Curve::TANGENT_FREE);
	}
	return c;
}

Ref<Curve2D> HewnProfile::to_curve2d() const {
	Ref<Curve2D> c;
	c.instantiate();
	for (int64_t i = 0; i < points.size(); i++) {
		c->add_point(points[i], handle_in((int)i), handle_out((int)i));
	}
	return c;
}

String HewnProfile::preset_names(const String &p_part) {
	if (p_part == "shaft") {
		return "Straight,Tapered,Ringed,Bulged";
	}
	if (p_part == "top") {
		return "None,Simple,Capital,Square Slab";
	}
	return "None,Simple,Plinth,Attic";
}

PackedVector2Array HewnProfile::preset(const String &p_part, int p_style) {
	PackedVector2Array p;
	auto add = [&](float x, float y) { p.push_back(Vector2(x, y)); };
	if (p_part == "shaft") {
		switch (p_style) {
			case 1: // tapered: narrower at the top
				add(1.0f, 0.0f);
				add(0.85f, 1.0f);
				break;
			case 2: // a ring at each end
				add(1.08f, 0.0f);
				add(1.08f, 0.04f);
				add(1.0f, 0.06f);
				add(1.0f, 0.94f);
				add(1.08f, 0.96f);
				add(1.08f, 1.0f);
				break;
			case 3: // entasis: a slight bulge
				add(1.0f, 0.0f);
				add(1.04f, 0.33f);
				add(1.03f, 0.6f);
				add(0.92f, 1.0f);
				break;
			default:
				add(1.0f, 0.0f);
				add(1.0f, 1.0f);
				break;
		}
		return p;
	}
	if (p_part == "top") {
		switch (p_style) {
			case 1:
				add(1.0f, 0.0f);
				add(1.25f, 0.5f);
				add(1.25f, 1.0f);
				break;
			case 2: // a capital: neck, echinus, abacus
				add(1.0f, 0.0f);
				add(1.0f, 0.2f);
				add(1.08f, 0.25f);
				add(1.2f, 0.45f);
				add(1.35f, 0.6f);
				add(1.45f, 0.62f);
				add(1.45f, 1.0f);
				break;
			case 3:
				add(1.0f, 0.0f);
				add(1.0f, 0.4f);
				add(1.5f, 0.4f);
				add(1.5f, 1.0f);
				break;
			default:
				add(1.0f, 0.0f);
				add(1.0f, 1.0f);
				break;
		}
		return p;
	}
	switch (p_style) { // base
		case 1:
			add(1.25f, 0.0f);
			add(1.25f, 0.5f);
			add(1.0f, 1.0f);
			break;
		case 2: // a square-ish plinth and a torus
			add(1.45f, 0.0f);
			add(1.45f, 0.4f);
			add(1.3f, 0.45f);
			add(1.35f, 0.6f);
			add(1.3f, 0.75f);
			add(1.1f, 0.85f);
			add(1.0f, 1.0f);
			break;
		case 3: // attic: two tori and a scotia
			add(1.4f, 0.0f);
			add(1.4f, 0.2f);
			add(1.35f, 0.3f);
			add(1.38f, 0.4f);
			add(1.2f, 0.5f);
			add(1.15f, 0.65f);
			add(1.22f, 0.75f);
			add(1.1f, 0.88f);
			add(1.0f, 1.0f);
			break;
		default:
			add(1.0f, 0.0f);
			add(1.0f, 1.0f);
			break;
	}
	return p;
}

Ref<HewnProfile> HewnProfile::make_preset(const String &p_part, int p_style) {
	Ref<HewnProfile> r;
	r.instantiate();
	r->set_points(preset(p_part, p_style));
	return r;
}

void HewnProfile::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_points", "points"), &HewnProfile::set_points);
	ClassDB::bind_method(D_METHOD("get_points"), &HewnProfile::get_points);
	ClassDB::bind_static_method("HewnProfile", D_METHOD("preset", "part", "style"), &HewnProfile::preset);
	ClassDB::bind_static_method("HewnProfile", D_METHOD("make_preset", "part", "style"), &HewnProfile::make_preset);
	ClassDB::bind_method(D_METHOD("set_handle_types", "types"), &HewnProfile::set_handle_types);
	ClassDB::bind_method(D_METHOD("get_handle_types"), &HewnProfile::get_handle_types);
	ClassDB::bind_method(D_METHOD("set_handles_in", "handles"), &HewnProfile::set_handles_in);
	ClassDB::bind_method(D_METHOD("get_handles_in"), &HewnProfile::get_handles_in);
	ClassDB::bind_method(D_METHOD("set_handles_out", "handles"), &HewnProfile::set_handles_out);
	ClassDB::bind_method(D_METHOD("get_handles_out"), &HewnProfile::get_handles_out);
	ClassDB::bind_method(D_METHOD("set_resolution", "resolution"), &HewnProfile::set_resolution);
	ClassDB::bind_method(D_METHOD("get_resolution"), &HewnProfile::get_resolution);
	ClassDB::bind_method(D_METHOD("set_sample_straight", "on"), &HewnProfile::set_sample_straight);
	ClassDB::bind_method(D_METHOD("get_sample_straight"), &HewnProfile::get_sample_straight);
	ClassDB::bind_method(D_METHOD("set_sample_equal", "on"), &HewnProfile::set_sample_equal);
	ClassDB::bind_method(D_METHOD("get_sample_equal"), &HewnProfile::get_sample_equal);
	ClassDB::bind_method(D_METHOD("sample"), &HewnProfile::sample);
	ClassDB::bind_method(D_METHOD("set_symmetric", "on"), &HewnProfile::set_symmetric);
	ClassDB::bind_method(D_METHOD("get_symmetric"), &HewnProfile::get_symmetric);
	ClassDB::bind_method(D_METHOD("symmetrize", "from_left"), &HewnProfile::symmetrize);
	ClassDB::bind_method(D_METHOD("set_point", "index", "position"), &HewnProfile::set_point);
	ClassDB::bind_method(D_METHOD("insert_point", "at", "position"), &HewnProfile::insert_point);
	ClassDB::bind_method(D_METHOD("remove_point", "index"), &HewnProfile::remove_point);
	ClassDB::bind_method(D_METHOD("get_state"), &HewnProfile::get_state);
	ClassDB::bind_method(D_METHOD("set_state", "state"), &HewnProfile::set_state);
	ClassDB::bind_method(D_METHOD("from_curve", "curve"), &HewnProfile::from_curve);
	ClassDB::bind_method(D_METHOD("from_curve2d", "curve"), &HewnProfile::from_curve2d);
	ClassDB::bind_method(D_METHOD("to_curve"), &HewnProfile::to_curve);
	ClassDB::bind_method(D_METHOD("to_curve2d"), &HewnProfile::to_curve2d);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR2_ARRAY, "points"), "set_points", "get_points");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "handle_types"), "set_handle_types", "get_handle_types");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR2_ARRAY, "handles_in"), "set_handles_in", "get_handles_in");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR2_ARRAY, "handles_out"), "set_handles_out", "get_handles_out");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_RANGE, "1,64,1"), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sample_straight"), "set_sample_straight", "get_sample_straight");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sample_equal"), "set_sample_equal", "get_sample_equal");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "symmetric"), "set_symmetric", "get_symmetric");
}
