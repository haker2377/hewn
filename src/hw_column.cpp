// Hewn — a lathed column: a base, a shaft (its profile repeated) and a top. The base and the top
// keep their heights when the column gets taller; only the shaft stretches or repeats.
#include "hw_core.h"
#include "hw_profile.h"

#include <cmath>

namespace hewn {
namespace shapes {

void build_column(const std::vector<float> &values, const std::vector<Vector2> &p_base, const std::vector<Vector2> &p_shaft, const std::vector<Vector2> &p_top, Mesh &out) {
	out.clear();
	const std::vector<Param> &ps = params(COLUMN);
	std::vector<float> v(ps.size());
	for (size_t i = 0; i < ps.size(); i++) {
		float x = i < values.size() ? values[i] : ps[i].def;
		x = std::max(ps[i].min, std::min(ps[i].max, x));
		v[i] = ps[i].is_int ? std::round(x) : x;
	}
	const float R = v[0], H = v[1];
	const int sides = (int)v[2];
	float bh = v[3], th = v[4];
	if (bh + th > H * 0.95f) {
		const float k = H * 0.95f / std::max(1e-6f, bh + th);
		bh *= k;
		th *= k;
	}
	auto prof = [&](const std::vector<Vector2> &custom, const char *part, int style) {
		if (custom.size() >= 2) {
			return custom;
		}
		const godot::PackedVector2Array p = godot::HewnProfile::preset(part, style);
		std::vector<Vector2> o;
		for (int64_t i = 0; i < p.size(); i++) {
			o.push_back(p[i]);
		}
		return o;
	};
	const std::vector<Vector2> base = prof(p_base, "base", (int)v[5]);
	const std::vector<Vector2> shaft = prof(p_shaft, "shaft", (int)v[6]);
	const std::vector<Vector2> top = prof(p_top, "top", (int)v[7]);
	const float shaft_h = std::max(1e-4f, H - bh - th);
	int reps = (int)v[8] == 1 ? std::max(1, (int)std::round(shaft_h / std::max(1e-3f, v[10]))) : std::max(1, (int)v[9]);
	reps = std::min(reps, 256);
	// the whole outline, bottom to top: (radius, height)
	std::vector<Vector2> line;
	auto put = [&](const std::vector<Vector2> &pr, float y0, float h) {
		for (const Vector2 &q : pr) {
			const Vector2 p(std::max(0.0f, q.x) * R, y0 + std::max(0.0f, std::min(1.0f, q.y)) * h);
			if (line.empty() || line.back().distance_to(p) > 1e-5f) {
				line.push_back(p);
			}
		}
	};
	if (bh > 1e-5f) {
		put(base, 0.0f, bh);
	}
	for (int r = 0; r < reps; r++) {
		put(shaft, bh + shaft_h * r / reps, shaft_h / reps);
	}
	if (th > 1e-5f) {
		put(top, H - th, th);
	}
	if (line.size() < 2) {
		return;
	}
	const bool smooth = v[11] > 0.5f;
	// lathe: a ring per outline point (a flat side toward +Z)
	const float a0 = 3.14159265358979f / sides;
	std::vector<std::vector<int>> rings;
	for (const Vector2 &p : line) {
		std::vector<int> ring;
		for (int s = 0; s < sides; s++) {
			const float a = a0 + 2.0f * 3.14159265358979f * s / sides;
			ring.push_back(out.add_vertex(Vector3(std::sin(a) * p.x, p.y, std::cos(a) * p.x)));
		}
		rings.push_back(ring);
	}
	for (size_t i = 0; i + 1 < rings.size(); i++) {
		const bool flat = std::fabs(line[i + 1].y - line[i].y) < 1e-5f; // (a step: no smoothing across it)
		for (int s = 0; s < sides; s++) {
			const int t = (s + 1) % sides;
			Face f = make_face({ rings[i][s], rings[i][t], rings[i + 1][t], rings[i + 1][s] });
			f.role = flat ? (line[i + 1].x < line[i].x ? ROLE_TOP : ROLE_BOTTOM) : ROLE_SURFACE;
			f.smooth = smooth && !flat ? 1 : 0;
			out.faces.push_back(f);
		}
	}
	if (line.front().x > 1e-5f) {
		Face f = make_face(std::vector<int>(rings.front().rbegin(), rings.front().rend()));
		f.role = ROLE_BOTTOM;
		out.faces.push_back(f);
	}
	if (line.back().x > 1e-5f) {
		Face f = make_face(std::vector<int>(rings.back().begin(), rings.back().end()));
		f.role = ROLE_TOP;
		out.faces.push_back(f);
	}
	// outward: a positive volume
	float vol = 0.0f;
	for (const Face &F : out.faces) {
		for (int i = 1; i + 1 < F.size(); i++) {
			vol += out.pos[F.v[0]].dot(out.pos[F.v[i]].cross(out.pos[F.v[i + 1]]));
		}
	}
	if (vol < 0.0f) {
		for (Face &F : out.faces) {
			F.reverse();
		}
	}
	cleanup(out);
}

} // namespace shapes
} // namespace hewn
