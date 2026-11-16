// Hewn — a rock: a ball of triangles pushed in and out by a noise (its roughness, its size of bumps, how many
// layers of it), cut flat by planes here and there (facets, as split stone), flat at the bottom, narrower at the top,
// leaning; a seed makes another rock of the same kind.
#include "hw_core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>

namespace hewn {
namespace shapes {

namespace {

uint32_t hash3(int x, int y, int z, uint32_t seed) {
	uint32_t h = seed * 0x9E3779B1u;
	h ^= (uint32_t)x * 0x85EBCA6Bu;
	h = (h << 13) | (h >> 19);
	h ^= (uint32_t)y * 0xC2B2AE35u;
	h = (h << 13) | (h >> 19);
	h ^= (uint32_t)z * 0x27D4EB2Fu;
	h ^= h >> 16;
	h *= 0x7FEB352Du;
	h ^= h >> 15;
	h *= 0x846CA68Bu;
	h ^= h >> 16;
	return h;
}

float lattice(int x, int y, int z, uint32_t seed) {
	return (float)(hash3(x, y, z, seed) & 0xFFFFFF) / (float)0xFFFFFF * 2.0f - 1.0f;
}

// smooth value noise, -1..1
float value_noise(const Vector3 &p, uint32_t seed) {
	const int x0 = (int)std::floor(p.x), y0 = (int)std::floor(p.y), z0 = (int)std::floor(p.z);
	const float fx = p.x - x0, fy = p.y - y0, fz = p.z - z0;
	auto s = [](float t) { return t * t * (3.0f - 2.0f * t); };
	const float u = s(fx), v = s(fy), w = s(fz);
	float c[2][2][2];
	for (int i = 0; i < 2; i++) {
		for (int j = 0; j < 2; j++) {
			for (int k = 0; k < 2; k++) {
				c[i][j][k] = lattice(x0 + i, y0 + j, z0 + k, seed);
			}
		}
	}
	auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	const float x00 = lerp(c[0][0][0], c[1][0][0], u), x10 = lerp(c[0][1][0], c[1][1][0], u);
	const float x01 = lerp(c[0][0][1], c[1][0][1], u), x11 = lerp(c[0][1][1], c[1][1][1], u);
	return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

float fbm(const Vector3 &p, int octaves, uint32_t seed) {
	float sum = 0.0f, amp = 1.0f, norm = 0.0f;
	Vector3 q = p;
	for (int o = 0; o < octaves; o++) {
		sum += value_noise(q, seed + (uint32_t)o * 101u) * amp;
		norm += amp;
		amp *= 0.5f;
		q *= 2.03f;
	}
	return norm > 0.0f ? sum / norm : 0.0f;
}

struct Rng {
	uint32_t s;
	float next() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return (float)(s & 0xFFFFFF) / (float)0xFFFFFF;
	}
};

} // namespace

void build_rock(const std::vector<float> &v, Mesh &out) {
	out.clear();
	const float W = v[0], H = v[1], D = v[2];
	const int detail = std::max(0, std::min(5, (int)v[3]));
	const uint32_t seed = (uint32_t)std::max(0, (int)v[4]) * 7919u + 17u;
	const float rough = v[5], freq = v[6];
	const int octaves = std::max(1, std::min(8, (int)v[7]));
	const int facets = std::max(0, (int)v[8]);
	const float facet_depth = v[9], flat_bottom = v[10], taper = v[11], lean = v[12];
	const bool smooth = v[13] > 0.5f;
	// an icosphere, its corners shared
	const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
	std::vector<Vector3> P = { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t }, { 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 },
		{ -t, 0, -1 }, { -t, 0, 1 } };
	for (Vector3 &p : P) {
		p.normalize();
	}
	std::vector<std::array<int, 3>> T = { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 },
		{ 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };
	for (int s = 0; s < detail; s++) {
		std::map<std::pair<int, int>, int> mid;
		auto m = [&](int a, int b) {
			const std::pair<int, int> k = { std::min(a, b), std::max(a, b) };
			auto it = mid.find(k);
			if (it != mid.end()) {
				return it->second;
			}
			P.push_back(((P[a] + P[b]) * 0.5f).normalized());
			mid[k] = (int)P.size() - 1;
			return (int)P.size() - 1;
		};
		std::vector<std::array<int, 3>> nt;
		for (const auto &tr : T) {
			const int a = m(tr[0], tr[1]), b = m(tr[1], tr[2]), c = m(tr[2], tr[0]);
			nt.push_back({ tr[0], a, c });
			nt.push_back({ tr[1], b, a });
			nt.push_back({ tr[2], c, b });
			nt.push_back({ a, b, c });
		}
		T.swap(nt);
	}
	// bumps: out and in along the way from the middle
	const Vector3 off((float)(seed % 997) * 0.37f, (float)(seed % 991) * 0.53f, (float)(seed % 983) * 0.71f);
	for (Vector3 &p : P) {
		const float n = fbm(p * freq + off, octaves, seed);
		p *= std::max(0.2f, 1.0f + n * rough);
	}
	// facets: planes cutting the rock flat here and there (what is past one is put onto it)
	Rng rng{ seed | 1u };
	for (int f = 0; f < facets; f++) {
		const float z = rng.next() * 2.0f - 1.0f, a = rng.next() * 6.2831853f;
		const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
		const Vector3 u(r * std::cos(a), z, r * std::sin(a));
		float far = 0.0f;
		for (const Vector3 &p : P) {
			far = std::max(far, p.dot(u));
		}
		const float d = far * (1.0f - facet_depth * (0.4f + 0.6f * rng.next()));
		for (Vector3 &p : P) {
			const float e = p.dot(u) - d;
			if (e > 0.0f) {
				p -= u * e;
			}
		}
	}
	// flat at the bottom
	if (flat_bottom > 1e-4f) {
		float lo = 1e30f, hi = -1e30f;
		for (const Vector3 &p : P) {
			lo = std::min(lo, p.y);
			hi = std::max(hi, p.y);
		}
		const float cut = lo + (hi - lo) * std::min(0.95f, flat_bottom);
		for (Vector3 &p : P) {
			p.y = std::max(p.y, cut);
		}
	}
	// its size, narrower toward the top, leaning
	float lo = 1e30f, hi = -1e30f;
	for (const Vector3 &p : P) {
		lo = std::min(lo, p.y);
		hi = std::max(hi, p.y);
	}
	for (Vector3 &p : P) {
		const float h01 = hi > lo ? (p.y - lo) / (hi - lo) : 0.5f;
		const float k = std::max(0.05f, 1.0f - taper * h01);
		p = Vector3(p.x * W * 0.5f * k + lean * h01 * H, p.y * H * 0.5f, p.z * D * 0.5f * k);
	}
	out.pos = P;
	for (const auto &tr : T) {
		// (counter-clockwise from outside: the icosphere's triangles turned to face out)
		Face F = make_face({ tr[0], tr[2], tr[1] });
		F.smooth = smooth ? 1 : 0;
		F.role = ROLE_SURFACE;
		out.faces.push_back(F);
	}
	// outward (a positive volume)
	float vol = 0.0f;
	for (const Face &F : out.faces) {
		vol += out.pos[F.v[0]].dot(out.pos[F.v[1]].cross(out.pos[F.v[2]]));
	}
	if (vol < 0.0f) {
		for (Face &F : out.faces) {
			F.reverse();
		}
	}
	// (the flat parts - facets, the bottom - as single faces)
	ops::limited_dissolve(out, std::vector<int>(), smooth ? 0.01f : 1.0f);
}

} // namespace shapes
} // namespace hewn
