#include "hw_hotspot.h"

#include <godot_cpp/variant/dictionary.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

using namespace godot;

namespace hewn {

namespace {

int find_root(std::vector<int> &p, int x) {
	while (p[x] != x) {
		p[x] = p[p[x]];
		x = p[x];
	}
	return x;
}

// The rectangle for a piece of `w` x `h` (meters, or relative to the largest piece) facing `kind` (1 wall, 2 floor,
// 3 ceiling) on `side` (0 unknown, 1 outside, 2 inside, 3 an opening's): its index, turned or not. The rectangles for
// it: of its kind or of any kind, of its side or of any side - all of them alike, by how they fit (none at all: one of
// another side, then any); off ones, other atlases' (p_atlas >= 0: the piece held to one) and ones for other sizes
// (p_meters: its longer side) not. Among the ones that fit about as well (the spread), one by chance (their weights).
int pick(const std::vector<HotspotRect> &p_rects, float w, float h, int kind, int side, int p_atlas, float p_meters, const HotspotOptions &o,
		const std::vector<Vector2> &p_sizes, uint32_t p_seed, bool &r_turned, bool p_round) {
	r_turned = false;
	w = std::max(1e-4f, w);
	h = std::max(1e-4f, h);
	const float k = std::min(1.0f, std::max(0.0f, o.weight));
	auto usable_any = [&](const HotspotRect &R) {
		if (!R.enabled || R.r.size.x < 1e-6f || R.r.size.y < 1e-6f || (p_atlas >= 0 && R.atlas != p_atlas)) {
			return false;
		}
		if ((R.min_size > 0.0f && p_meters < R.min_size) || (R.max_size > 0.0f && p_meters > R.max_size)) {
			return false;
		}
		return true;
	};
	// (round cells: a round piece gets one when there is one; any other piece never)
	bool round_cells = false;
	for (const HotspotRect &R : p_rects) {
		round_cells = round_cells || (R.radial && usable_any(R));
	}
	const bool want_round = p_round && round_cells;
	auto usable = [&](const HotspotRect &R) { return usable_any(R) && R.radial == want_round; };
	// the first of the levels with any rectangle
	int level = -1;
	auto level_of = [&](const HotspotRect &R) {
		// (the kind and the side are what a cell is for, not an order: one for any face is as good as one for floors)
		const bool s_ok = R.side == 0 || (side != 0 && R.side == side);
		const bool k_ok = R.orient == 0 || R.orient == kind;
		if (s_ok && k_ok) {
			return 0;
		}
		return k_ok ? 1 : 2; // (none for its side at all: one of another side, rather than nothing)
	};
	for (const HotspotRect &R : p_rects) {
		if (usable(R) && level_of(R) < 99) {
			level = level < 0 ? level_of(R) : std::min(level, level_of(R));
		}
	}
	if (level < 0) {
		return -1;
	}
	struct Cand {
		int cell;
		bool turned;
		float score;
	};
	std::vector<Cand> cands;
	float best = 1e30f;
	for (int c = 0; c < (int)p_rects.size(); c++) {
		const HotspotRect &R = p_rects[c];
		if (!usable(R) || level_of(R) != level) {
			continue;
		}
		const float ks = 1.0f / std::max(1e-3f, R.scale);
		const float cw = std::max(1e-6f, p_sizes[c].x * ks), ch = std::max(1e-6f, p_sizes[c].y * ks);
		// (upwards: kept upright unless it may turn; the others at any quarter turn unless they may not)
		const bool may_turn = R.turn == 2 ? true : R.turn == 1 ? false : (o.turn && R.orient != 1);
		for (int turn = 0; turn < (may_turn ? 2 : 1); turn++) {
			const float fw = turn ? h : w, fh = turn ? w : h;
			float shape, size;
			if (R.tile == 1) {
				shape = 0.0f; // (repeats along U: only its height matters)
				size = std::fabs(std::log(fh / ch));
			} else if (R.tile == 2) {
				shape = 0.0f;
				size = std::fabs(std::log(fw / cw));
			} else {
				shape = std::fabs(std::log((fw / fh) / (cw / ch)));
				size = std::fabs(std::log((fw * fh) / (cw * ch))) * 0.5f;
			}
			// (a turned rectangle a little worse: upright ones first when they fit as well)
			const float score = shape * (1.0f - k) + size * k + (turn ? 0.02f : 0.0f);
			cands.push_back({ c, turn == 1, score });
			best = std::min(best, score);
		}
	}
	std::vector<Cand> top;
	float total = 0.0f;
	for (const Cand &c : cands) {
		if (c.score <= best + 0.02f + std::max(0.0f, o.spread) * 1.5f && p_rects[c.cell].weight > 0.0f) {
			top.push_back(c);
			total += p_rects[c.cell].weight;
		}
	}
	if (top.empty()) {
		return -1;
	}
	std::sort(top.begin(), top.end(), [](const Cand &a, const Cand &b) { return a.score < b.score || (a.score == b.score && a.cell < b.cell); });
	// (by chance, the heavier ones more often; another variant: the next)
	const uint32_t r = (p_seed + (uint32_t)std::max(0, o.variant) * 2654435761u) * 2246822519u;
	float at = (float)((r >> 8) & 0xffff) / 65536.0f * total;
	size_t pick_i = 0;
	for (size_t i = 0; i < top.size(); i++) {
		at -= p_rects[top[i].cell].weight;
		if (at < 0.0f) {
			pick_i = i;
			break;
		}
		pick_i = i;
	}
	r_turned = top[pick_i].turned;
	return top[pick_i].cell;
}

struct Piece {
	std::vector<int> faces;
	Vector3 u_ax, v_ax;
	Vector2 mn;
	float w = 0.0f, h = 0.0f;
	int kind = 1;
	int side = 0;
	int lock = -1;
	uint32_t seed = 0;
	bool round = false; // (its outline a circle / an oval: for the round cells)
	bool strip = false; // (a strip of quads: laid out along it, not projected)
	std::vector<std::vector<Vector2>> flat; // (a strip's: each face's corners laid out, meters; as P.faces)
};

// A band: faces one after another (each joined to at most two of them, by sides that do not touch - a beam's top bent
// any way, a spun wall, a cylinder's sides), laid out along it: the sides between them (rungs) across, U the way
// along their middles, V across them; every face a straight piece of the band. Each face's corners in `r_flat`
// (meters, unmirrored as a plain projection; a wall's V down). False: not a band.
bool lay_band(const Mesh &m, const std::vector<int> &p_faces, const Transform3D &p_xf, int p_kind, std::vector<std::vector<Vector2>> &r_flat) {
	const int n = (int)p_faces.size();
	if (n < 2) {
		return false;
	}
	// the rungs: sides shared by two of the faces
	struct Rung {
		int a, b; // (its vertices)
		int f0, f1; // (the faces, as indices of p_faces)
	};
	std::unordered_map<uint64_t, std::vector<std::pair<int, int>>> at; // (face index, corner)
	for (int k = 0; k < n; k++) {
		const Face &F = m.faces[p_faces[k]];
		for (int i = 0; i < F.size(); i++) {
			at[ekey(F.v[i], F.v[(i + 1) % F.size()])].push_back({ k, i });
		}
	}
	std::vector<Rung> rungs;
	std::vector<std::vector<int>> of_face(n);
	for (const auto &kv : at) {
		if (kv.second.size() != 2 || kv.second[0].first == kv.second[1].first) {
			continue;
		}
		const Face &F = m.faces[p_faces[kv.second[0].first]];
		const int i = kv.second[0].second;
		Rung r{ F.v[i], F.v[(i + 1) % F.size()], kv.second[0].first, kv.second[1].first };
		of_face[r.f0].push_back((int)rungs.size());
		of_face[r.f1].push_back((int)rungs.size());
		rungs.push_back(r);
	}
	int ends = 0;
	for (int k = 0; k < n; k++) {
		if (of_face[k].empty() || of_face[k].size() > 2) {
			return false;
		}
		if (of_face[k].size() == 2) {
			const Rung &x = rungs[of_face[k][0]], &y = rungs[of_face[k][1]];
			if (x.a == y.a || x.a == y.b || x.b == y.a || x.b == y.b) {
				return false; // (its two rungs meet: a fan, a grid's corner - not a band)
			}
		} else {
			ends++;
		}
	}
	if (ends != 0 && ends != 2) {
		return false;
	}
	// the faces in their order along it, the rung before / after each
	std::vector<int> order, before(n, -1), after(n, -1);
	int start = 0;
	for (int k = 0; k < n; k++) {
		if (of_face[k].size() == 1) {
			start = k;
			break;
		}
	}
	{
		int k = start, came = ends ? -1 : of_face[start][0];
		std::vector<char> seen(n, 0);
		while (k >= 0 && !seen[k]) {
			seen[k] = 1;
			order.push_back(k);
			before[k] = came;
			int go = -1;
			for (int r : of_face[k]) {
				if (r != came) {
					go = r;
				}
			}
			after[k] = go;
			if (go < 0) {
				break;
			}
			const int next = rungs[go].f0 == k ? rungs[go].f1 : rungs[go].f0;
			came = go;
			k = next;
		}
		if ((int)order.size() != n) {
			return false;
		}
	}
	auto W = [&](int v) { return p_xf.xform(m.pos[v]); };
	const Basis nb = p_xf.basis.inverse().transposed();
	// each rung: its middle, its U (the way along the middles), the face side's across
	std::vector<Vector3> mid(rungs.size());
	for (size_t r = 0; r < rungs.size(); r++) {
		mid[r] = (W(rungs[r].a) + W(rungs[r].b)) * 0.5f;
	}
	std::vector<float> u_in(n, 0.0f), u_out(n, 0.0f); // (U at the rung before / after each face)
	float u = 0.0f;
	for (int j = 0; j < n; j++) {
		const int k = order[j];
		u_in[k] = u;
		if (before[k] >= 0 && after[k] >= 0) {
			u += mid[before[k]].distance_to(mid[after[k]]);
		}
		u_out[k] = u;
	}
	auto frame = [&](int k, Vector3 &r_t, Vector3 &r_v, Vector3 &r_o, float &r_u) {
		// along (from the rung before to the one after; an end: from / to its middle), across (t x n), its origin
		const Face &F = m.faces[p_faces[k]];
		Vector3 c;
		for (int v : F.v) {
			c += W(v) / (float)F.size();
		}
		const Vector3 nn = nb.xform(face_normal(m, F)).normalized();
		if (before[k] >= 0 && after[k] >= 0) {
			r_t = mid[after[k]] - mid[before[k]];
			r_o = mid[before[k]];
			r_u = u_in[k];
		} else if (after[k] >= 0) {
			r_t = mid[after[k]] - c;
			r_o = mid[after[k]];
			r_u = u_out[k];
		} else {
			r_t = c - mid[before[k]];
			r_o = mid[before[k]];
			r_u = u_in[k];
		}
		r_t = (r_t - nn * nn.dot(r_t)).normalized();
		r_v = r_t.cross(nn).normalized(); // (t, v as a plain projection turns: not mirrored)
	};
	// each rung's across: its two faces' together (its corners the same in both)
	std::vector<Vector3> rung_v(rungs.size());
	for (int k = 0; k < n; k++) {
		Vector3 t, v, o;
		float uu;
		frame(k, t, v, o, uu);
		for (int r : of_face[k]) {
			rung_v[r] += v;
		}
	}
	// a wall: V down it (the whole band turned half round when it would go up)
	float up = 0.0f;
	for (size_t r = 0; r < rungs.size(); r++) {
		// (the rung itself, turned as its faces' across)
		Vector3 rd = W(rungs[r].a) - W(rungs[r].b);
		rd = rd.length() > 1e-9f ? rd.normalized() : rung_v[r].normalized();
		rung_v[r] = rd.dot(rung_v[r]) >= 0.0f ? rd : -rd;
		up += rung_v[r].y;
	}
	const float sign = p_kind == 1 && up > 0.0f ? -1.0f : 1.0f;
	r_flat.assign(n, std::vector<Vector2>());
	for (int k = 0; k < n; k++) {
		const Face &F = m.faces[p_faces[k]];
		Vector3 t, v, o;
		float u0;
		frame(k, t, v, o, u0);
		for (int i = 0; i < F.size(); i++) {
			const int vi = F.v[i];
			Vector2 q;
			int on = -1;
			for (int r : of_face[k]) {
				if (rungs[r].a == vi || rungs[r].b == vi) {
					on = r;
				}
			}
			if (on >= 0) {
				// (a rung's corner: the rung's U, across it as the rung lies - the same in both its faces)
				q = Vector2(on == before[k] ? u_in[k] : u_out[k], (W(vi) - mid[on]).dot(rung_v[on]));
			} else {
				q = Vector2(u0 + (W(vi) - o).dot(t), (W(vi) - o).dot(v));
			}
			r_flat[k].push_back(q * sign);
		}
	}
	return true;
}

// A grid of quads (a cylinder's sides of many rows, bent or spun, a bent wall of many rows): each face a cell, the rows
// one way, the columns the other; closed round one way (a tube: the seam along it, once). Laid out on a grid of the
// columns' and rows' mean sizes, U round the closed way. False: not such a grid.
bool lay_grid(const Mesh &m, const std::vector<int> &p_faces, const Transform3D &p_xf, int p_kind, std::vector<std::vector<Vector2>> &r_flat) {
	const int n = (int)p_faces.size();
	if (n < 4) {
		return false;
	}
	for (int f : p_faces) {
		if (m.faces[f].size() != 4) {
			return false;
		}
	}
	std::unordered_map<uint64_t, std::vector<std::pair<int, int>>> at;
	for (int k = 0; k < n; k++) {
		const Face &F = m.faces[p_faces[k]];
		for (int i = 0; i < 4; i++) {
			at[ekey(F.v[i], F.v[(i + 1) % 4])].push_back({ k, i });
		}
	}
	// across a face's side: the face there and that side's index in it (-1: none)
	auto across = [&](int k, int e, int &r_e2) {
		const Face &F = m.faces[p_faces[k]];
		auto it = at.find(ekey(F.v[e], F.v[(e + 1) % 4]));
		if (it == at.end() || it->second.size() != 2) {
			return -1;
		}
		const std::pair<int, int> o = it->second[0].first == k ? it->second[1] : it->second[0];
		r_e2 = o.second;
		return o.first == k ? -1 : o.first;
	};
	// a walk across opposite sides: d is the side the walk leaves by; in the next face it comes in by e2, leaves by e2 + 2
	auto walk = [&](int k0, int e0, std::vector<int> &r_faces, std::vector<int> &r_out, bool &r_closed) {
		r_faces.clear();
		r_out.clear();
		r_closed = false;
		int k = k0, e = e0;
		for (int guard = 0; guard <= n; guard++) {
			r_faces.push_back(k);
			r_out.push_back(e);
			int e2;
			const int nk = across(k, e, e2);
			if (nk < 0) {
				return;
			}
			if (nk == k0) {
				r_closed = true;
				return;
			}
			k = nk;
			e = (e2 + 2) % 4;
		}
	};
	// the way round: from the first face, the one of its two ways that comes back (else the first)
	int k0 = 0, ei = 0;
	std::vector<int> row, row_out;
	bool closed = false;
	walk(k0, 0, row, row_out, closed);
	if (!closed) {
		std::vector<int> r2, o2;
		bool c2 = false;
		walk(k0, 1, r2, o2, c2);
		if (c2) {
			ei = 1;
			row = r2;
			row_out = o2;
			closed = true;
		}
	}
	if (!closed) {
		// (open both ways: the row from its start - back from the first face as far as it goes)
		std::vector<int> back, bo;
		bool cb = false;
		walk(k0, 2, back, bo, cb);
		const int s = back.back();
		const int se = (bo.back() + 2) % 4;
		walk(s, se, row, row_out, closed);
	}
	const int cols = (int)row.size();
	if (cols < 2 && !closed) {
		return false;
	}
	// each face: its column, row and the side its column goes on by (i+); the rows from the first row on (j+ is the side
	// after i+ round the face), first back to the first row then on
	std::vector<int> col_of(n, -1), row_of(n, -1), off(n, -1);
	// the first row's j- / j+ walks for each of its faces
	int rows_before = 0;
	{
		// how far back (j-) the first row's first face goes: the grid's first row
		std::vector<int> wf, wo;
		bool c = false;
		walk(row[0], (row_out[0] + 3) % 4, wf, wo, c);
		if (c) {
			return false; // (closed both ways: a torus)
		}
		rows_before = (int)wf.size() - 1;
	}
	int rows = -1;
	for (int i = 0; i < cols; i++) {
		std::vector<int> wf, wo;
		bool c = false;
		walk(row[i], (row_out[i] + 3) % 4, wf, wo, c); // j- from the first row
		if (c || (int)wf.size() - 1 != rows_before) {
			return false;
		}
		const int base = wf.back();
		const int base_jp = (wo.back() + 2) % 4; // (the way back up: j+ in that face)
		std::vector<int> up, uo;
		walk(base, base_jp, up, uo, c);
		if (c) {
			return false;
		}
		if (rows < 0) {
			rows = (int)up.size();
		} else if (rows != (int)up.size()) {
			return false; // (not a regular grid)
		}
		for (int j = 0; j < (int)up.size(); j++) {
			const int k = up[j];
			if (col_of[k] >= 0) {
				return false;
			}
			col_of[k] = i;
			row_of[k] = j;
			off[k] = (uo[j] + 3) % 4; // (i+ is the side before j+ round the face)
		}
	}
	if (rows < 1 || cols * rows != n) {
		return false;
	}
	// the grid's points: (I, J), I up to cols (closed: the last is the first again), J up to rows; their places
	const int NI = cols + 1, NJ = rows + 1;
	std::vector<int> grid(NI * NJ, -1);
	for (int k = 0; k < n; k++) {
		const Face &F = m.faces[p_faces[k]];
		const int i = col_of[k], j = row_of[k], o = off[k];
		grid[(i + 1) * NJ + j] = F.v[o];
		grid[(i + 1) * NJ + j + 1] = F.v[(o + 1) % 4];
		grid[i * NJ + j + 1] = F.v[(o + 2) % 4];
		grid[i * NJ + j] = F.v[(o + 3) % 4];
	}
	for (int g : grid) {
		if (g < 0) {
			return false;
		}
	}
	auto W = [&](int v) { return p_xf.xform(m.pos[v]); };
	std::vector<float> U(NI, 0.0f), V(NJ, 0.0f);
	for (int I = 1; I < NI; I++) {
		float sum = 0.0f;
		for (int J = 0; J < NJ; J++) {
			sum += W(grid[I * NJ + J]).distance_to(W(grid[(I - 1) * NJ + J]));
		}
		U[I] = U[I - 1] + sum / NJ;
	}
	for (int J = 1; J < NJ; J++) {
		float sum = 0.0f;
		for (int I = 0; I < NI; I++) {
			sum += W(grid[I * NJ + J]).distance_to(W(grid[I * NJ + J - 1]));
		}
		V[J] = V[J - 1] + sum / NI;
	}
	r_flat.assign(n, std::vector<Vector2>());
	float area = 0.0f;
	float up_way = 0.0f;
	for (int k = 0; k < n; k++) {
		const int i = col_of[k], j = row_of[k], o = off[k];
		std::vector<Vector2> q(4);
		q[o] = Vector2(U[i + 1], V[j]);
		q[(o + 1) % 4] = Vector2(U[i + 1], V[j + 1]);
		q[(o + 2) % 4] = Vector2(U[i], V[j + 1]);
		q[(o + 3) % 4] = Vector2(U[i], V[j]);
		for (int c = 0; c < 4; c++) {
			area += q[c].cross(q[(c + 1) % 4]);
		}
		const Face &F = m.faces[p_faces[k]];
		up_way += W(F.v[(o + 1) % 4]).y - W(F.v[o]).y; // (V's way up the world)
		r_flat[k] = q;
	}
	// unmirrored (the faces' corners round the same way as a plain projection turns them); a wall's V down
	const bool flip_u = area > 0.0f;
	const float total_u = U[NI - 1];
	float vsign = 1.0f, usign = 1.0f;
	if (p_kind == 1 && up_way > 0.0f) {
		vsign = -1.0f;
		usign = -1.0f;
	}
	for (std::vector<Vector2> &q : r_flat) {
		for (Vector2 &p : q) {
			if (flip_u) {
				p.x = total_u - p.x;
			}
			p = Vector2(p.x * usign, p.y * vsign);
		}
	}
	(void)ei;
	return true;
}

// A ribbon in one face: its outline two long sides of as many points each, one along the other (a wall's top along its
// points, a curved beam's), and two short ends. Laid out as a band: each point with the one across from it a rung, U
// along the rungs' middles, V across. Each corner's place in `r_flat` (meters, unmirrored; a wall's V down).
bool lay_ribbon(const Mesh &m, int p_face, const Transform3D &p_xf, int p_kind, std::vector<Vector2> &r_flat) {
	const Face &F = m.faces[p_face];
	const int k2 = F.size();
	if (k2 < 6 || (k2 % 2) != 0) {
		return false;
	}
	const int n = k2 / 2; // (points on each long side)
	std::vector<Vector3> P(k2);
	for (int i = 0; i < k2; i++) {
		P[i] = p_xf.xform(m.pos[F.v[i]]);
	}
	// where the sides split: the start whose rungs (i-th with the one across) are the most alike
	int best = -1;
	float best_score = 1e30f;
	for (int s = 0; s < n; s++) {
		float lo = 1e30f, hi = 0.0f, sum = 0.0f, len_a = 0.0f;
		for (int i = 0; i < n; i++) {
			const float d = P[(s + i) % k2].distance_to(P[(s + k2 - 1 - i) % k2]);
			lo = std::min(lo, d);
			hi = std::max(hi, d);
			sum += d;
			if (i > 0) {
				len_a += P[(s + i) % k2].distance_to(P[(s + i - 1) % k2]);
			}
		}
		const float mean = sum / (float)n;
		if (lo < 1e-5f || hi > lo * 2.5f || len_a < mean * 1.5f) {
			continue; // (rungs not alike, or the sides no longer than across: not a ribbon this way)
		}
		const float score = (hi - lo) / mean;
		if (score < best_score) {
			best_score = score;
			best = s;
		}
	}
	if (best < 0) {
		return false;
	}
	// the rungs: the i-th point of side A and its partner on side B
	std::vector<int> ra(n), rb(n);
	std::vector<Vector3> mid(n);
	for (int i = 0; i < n; i++) {
		ra[i] = (best + i) % k2;
		rb[i] = (best + k2 - 1 - i) % k2;
		mid[i] = (P[ra[i]] + P[rb[i]]) * 0.5f;
	}
	const Vector3 nn = p_xf.basis.inverse().transposed().xform(face_normal(m, F)).normalized();
	std::vector<float> u(n, 0.0f);
	for (int i = 1; i < n; i++) {
		u[i] = u[i - 1] + mid[i].distance_to(mid[i - 1]);
	}
	r_flat.assign(k2, Vector2());
	float up = 0.0f;
	std::vector<Vector3> across(n);
	for (int i = 0; i < n; i++) {
		Vector3 t = mid[std::min(n - 1, i + 1)] - mid[std::max(0, i - 1)];
		t = (t - nn * nn.dot(t)).normalized();
		// (across: the rung itself, turned as t x n - as a plain projection turns, not mirrored)
		const Vector3 tv = t.cross(nn);
		Vector3 rd = P[ra[i]] - P[rb[i]];
		rd = rd.length() > 1e-9f ? rd.normalized() : tv.normalized();
		across[i] = rd.dot(tv) >= 0.0f ? rd : -rd;
		up += across[i].y;
	}
	const float sign = p_kind == 1 && up > 0.0f ? -1.0f : 1.0f; // (a wall: V down it)
	for (int i = 0; i < n; i++) {
		r_flat[ra[i]] = Vector2(u[i], (P[ra[i]] - mid[i]).dot(across[i])) * sign;
		r_flat[rb[i]] = Vector2(u[i], (P[rb[i]] - mid[i]).dot(across[i])) * sign;
	}
	return true;
}

// (a cell that may lie a quarter turned: its own setting, else not an upwards one; not a repeating one)
bool R_may_turn_any(const std::vector<HotspotRect> &p_rects, int p_c, const HotspotOptions &o) {
	if (p_c < 0 || p_c >= (int)p_rects.size()) {
		return false;
	}
	const HotspotRect &R = p_rects[p_c];
	return R.tile == 0 && (R.turn == 2 || (R.turn == 0 && o.turn && R.orient != 1));
}

} // namespace

int apply_hotspots(Mesh &m, const std::vector<int> &p_faces, const std::function<const std::vector<HotspotRect> *(int)> &p_rects_of,
		const HotspotOptions &o, const Transform3D &p_xf) {
	std::vector<int> faces;
	for (int f : p_faces) {
		if (f >= 0 && f < (int)m.faces.size() && m.faces[f].size() >= 3 && !(o.skip_fixed && m.faces[f].hs_fixed)) {
			faces.push_back(f);
		}
	}
	if (faces.empty()) {
		return 0;
	}
	const bool atlases = !o.atlas_slots.empty();
	const Basis nb = p_xf.basis.inverse().transposed();
	std::vector<Vector3> wn(m.faces.size()); // (world normals)
	for (int f : faces) {
		wn[f] = nb.xform(face_normal(m, m.faces[f])).normalized();
	}
	// the object's middle (in the world): which way is out for a wall without a role
	Vector3 mid;
	{
		Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
		for (const Vector3 &p : m.pos) {
			const Vector3 w = p_xf.xform(p);
			mn = mn.min(w);
			mx = mx.max(w);
		}
		mid = (mn + mx) * 0.5f;
	}
	// the pieces: faces joined over an edge (not a seam), of one material (one atlas held), bent less than the angle
	std::vector<int> parent(m.faces.size());
	std::iota(parent.begin(), parent.end(), 0);
	if (o.islands) {
		const float cos_lim = std::cos(o.angle * 3.14159265f / 180.0f);
		std::unordered_map<uint64_t, int> first;
		for (int f : faces) {
			const Face &F = m.faces[f];
			for (int i = 0; i < F.size(); i++) {
				const uint64_t k = ekey(F.v[i], F.v[(i + 1) % F.size()]);
				if (o.seams) {
					auto ea = m.edge_attrs.find(k);
					if (ea != m.edge_attrs.end() && ea->second.seam) {
						continue;
					}
				}
				auto it = first.find(k);
				if (it == first.end()) {
					first[k] = f;
				} else {
					const int g = it->second;
					const bool same = atlases ? m.faces[g].hs_lock == F.hs_lock && m.faces[g].role == F.role : m.faces[g].material == F.material;
					if (same && wn[g].dot(wn[f]) >= cos_lim) {
						parent[find_root(parent, f)] = find_root(parent, g);
					}
				}
			}
		}
	}
	std::unordered_map<int, std::vector<int>> by_root;
	for (int f : faces) {
		by_root[find_root(parent, f)].push_back(f);
	}
	std::vector<Piece> pieces;
	for (auto &kv : by_root) {
		Piece P;
		P.faces = kv.second;
		std::sort(P.faces.begin(), P.faces.end());
		// (the same piece the same look however it was found: seeded by its first face and the object's seed)
		P.seed = (uint32_t)P.faces[0] * 2654435761u + o.seed * 40503u;
		P.lock = atlases ? m.faces[P.faces[0]].hs_lock : -1;
		// its plane: the area-weighted normal, world up for the walls, the longest outer edge for floors
		Vector3 n;
		Vector3 c;
		int roles[4] = { 0, 0, 0, 0 };
		for (int f : P.faces) {
			n += nb.xform(face_normal_raw(m, m.faces[f]));
			c += p_xf.xform(face_center(m, m.faces[f]));
			const int r = m.faces[f].role;
			roles[r == ROLE_OUTER ? 1 : r == ROLE_INNER ? 2 : r == ROLE_OPENING ? 3 : 0]++;
		}
		c /= (float)P.faces.size();
		float area = 0.0f;
		float kinds[4] = { 0, 0, 0, 0 };
		for (int f : P.faces) {
			const Vector3 fn = nb.xform(face_normal_raw(m, m.faces[f]));
			area += fn.length();
			const Vector3 un = fn.length() > 1e-12f ? fn.normalized() : Vector3(0, 1, 0);
			kinds[un.y > 0.7f ? 2 : un.y < -0.7f ? 3 : 1] += fn.length();
		}
		const bool bent = n.length() < area * 0.5f; // (a ring of faces: its normals cancel out)
		n = n.length() > 1e-9f ? n.normalized() : Vector3(0, 1, 0);
		P.kind = n.y > 0.7f ? 2 : n.y < -0.7f ? 3 : 1;
		if (bent) {
			P.kind = kinds[1] >= kinds[2] && kinds[1] >= kinds[3] ? 1 : kinds[2] >= kinds[3] ? 2 : 3; // (most of its faces')
		}
		// which side: the faces' roles; a wall without one: out of the object's middle or into it
		for (int s = 1; s <= 3; s++) {
			if (roles[s] > roles[P.side] && roles[s] * 2 >= (int)P.faces.size()) {
				P.side = s;
			}
		}
		if (P.side == 0 && P.kind == 1) {
			const Vector3 hn(n.x, 0, n.z), away(c.x - mid.x, 0, c.z - mid.z);
			if (hn.length() > 1e-4f && away.length() > 1e-4f) {
				P.side = hn.dot(away) > 0.0f ? 1 : 2;
			}
		}
		Vector3 v_ax;
		if (P.kind == 1) {
			v_ax = Vector3(0, 1, 0) - n * n.y;
		} else {
			std::unordered_map<uint64_t, int> cnt;
			for (int f : P.faces) {
				const Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					cnt[ekey(F.v[i], F.v[(i + 1) % F.size()])]++;
				}
			}
			float best = -1.0f;
			for (int f : P.faces) {
				const Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					const int a = F.v[i], b = F.v[(i + 1) % F.size()];
					if (cnt[ekey(a, b)] != 1) {
						continue;
					}
					const Vector3 e = p_xf.basis.xform(m.pos[b] - m.pos[a]);
					if (e.length() > best) {
						best = e.length();
						v_ax = e.cross(n); // (across the longest edge: the rectangle's length along it)
					}
				}
			}
		}
		P.v_ax = v_ax.length() > 1e-6f ? v_ax.normalized() : (std::fabs(n.x) < 0.9f ? Vector3(1, 0, 0) : Vector3(0, 0, 1)).cross(n).normalized();
		P.u_ax = P.v_ax.cross(n).normalized();
		Vector2 mn(1e30f, 1e30f), mx(-1e30f, -1e30f);
		for (int f : P.faces) {
			for (int v : m.faces[f].v) {
				const Vector3 w = p_xf.xform(m.pos[v]);
				const Vector2 q(P.u_ax.dot(w), -P.v_ax.dot(w)); // (V down the texture as it goes down the wall)
				mn = Vector2(std::min(mn.x, q.x), std::min(mn.y, q.y));
				mx = Vector2(std::max(mx.x, q.x), std::max(mx.y, q.y));
			}
		}
		P.mn = mn;
		P.w = mx.x - mn.x;
		P.h = mx.y - mn.y;
		{
			// round: its outline's points (on one edge of the piece only) all on the oval in its rectangle
			std::unordered_map<uint64_t, int> cnt;
			for (int f : P.faces) {
				const Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					cnt[ekey(F.v[i], F.v[(i + 1) % F.size()])]++;
				}
			}
			std::unordered_map<int, char> outline;
			for (int f : P.faces) {
				const Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					if (cnt[ekey(F.v[i], F.v[(i + 1) % F.size()])] == 1) {
						outline[F.v[i]] = 1;
					}
				}
			}
			bool round = outline.size() >= 8 && P.w > 1e-5f && P.h > 1e-5f;
			const Vector2 cen = mn + Vector2(P.w, P.h) * 0.5f;
			for (const auto &kv : outline) {
				if (!round) {
					break;
				}
				const Vector3 w = p_xf.xform(m.pos[kv.first]);
				const Vector2 q((P.u_ax.dot(w) - cen.x) / (P.w * 0.5f), (-P.v_ax.dot(w) - cen.y) / (P.h * 0.5f));
				round = std::fabs(q.length() - 1.0f) < 0.08f;
			}
			P.round = round;
		}
		if (P.faces.size() >= 2 && lay_band(m, P.faces, p_xf, P.kind, P.flat)) {
			// a band (faces one after another): laid out along it, each face a straight piece of it
			Vector2 fmn(1e30f, 1e30f), fmx(-1e30f, -1e30f);
			for (const std::vector<Vector2> &fl : P.flat) {
				for (const Vector2 &q : fl) {
					fmn = fmn.min(q);
					fmx = fmx.max(q);
				}
			}
			P.strip = true;
			P.round = false;
			P.mn = fmn;
			P.w = fmx.x - fmn.x;
			P.h = fmx.y - fmn.y;
		}
		float flatness = 0.0f; // (1: all its faces one way; less: bent)
		{
			Vector3 sum;
			float tot = 0.0f;
			for (int f : P.faces) {
				const Vector3 fn = nb.xform(face_normal_raw(m, m.faces[f]));
				sum += fn;
				tot += fn.length();
			}
			flatness = tot > 1e-12f ? sum.length() / tot : 1.0f;
		}
		if (!P.strip && !P.round && flatness < 0.995f && P.faces.size() >= 4 && lay_grid(m, P.faces, p_xf, P.kind, P.flat)) {
			// a grid of quads (a tube of many rows, a bent wall of many rows): laid out on it, round a tube once
			Vector2 fmn(1e30f, 1e30f), fmx(-1e30f, -1e30f);
			for (const std::vector<Vector2> &fl : P.flat) {
				for (const Vector2 &q : fl) {
					fmn = fmn.min(q);
					fmx = fmx.max(q);
				}
			}
			P.strip = true;
			P.round = false;
			P.mn = fmn;
			P.w = fmx.x - fmn.x;
			P.h = fmx.y - fmn.y;
		}
		if (P.faces.size() == 1 && !P.round) {
			// one face a ribbon (two long sides one along the other): laid out along it the same way
			std::vector<Vector2> fl;
			if (lay_ribbon(m, P.faces[0], p_xf, P.kind, fl)) {
				Vector2 fmn(1e30f, 1e30f), fmx(-1e30f, -1e30f);
				for (const Vector2 &q : fl) {
					fmn = fmn.min(q);
					fmx = fmx.max(q);
				}
				P.flat = { fl };
				P.strip = true;
				P.mn = fmn;
				P.w = fmx.x - fmn.x;
				P.h = fmx.y - fmn.y;
			}
		}
		if (!P.strip && !P.round) {
			// bent round a middle: its outline's points on two circles with one middle (a ring's piece, a curved floor,
			// one face or many), at least a little of the arc: laid out round it, U along the arc, V across
			std::unordered_map<uint64_t, int> cnt;
			for (int f : P.faces) {
				const Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					cnt[ekey(F.v[i], F.v[(i + 1) % F.size()])]++;
				}
			}
			// the outline in its order (each boundary side once, as the faces go round)
			std::unordered_map<int, int> next_v;
			for (int f : P.faces) {
				const Face &F = m.faces[f];
				for (int i = 0; i < F.size(); i++) {
					if (cnt[ekey(F.v[i], F.v[(i + 1) % F.size()])] == 1) {
						next_v[F.v[i]] = F.v[(i + 1) % F.size()];
					}
				}
			}
			std::vector<Vector2> q;
			if (!next_v.empty()) {
				const int start = next_v.begin()->first;
				int v = start;
				for (size_t guard = 0; guard <= next_v.size(); guard++) {
					const Vector3 w = p_xf.xform(m.pos[v]);
					q.push_back(Vector2(P.u_ax.dot(w), -P.v_ax.dot(w)));
					auto it = next_v.find(v);
					if (it == next_v.end() || it->second == start) {
						break;
					}
					v = it->second;
				}
				if (q.size() != next_v.size()) {
					q.clear(); // (more than one loop round it: a hole in it - not this)
				}
			}
			// the middle: where the circles through each three points after another meet (the arcs' all the same one)
			std::vector<float> cxs, cys;
			for (size_t i = 0; q.size() >= 6 && i < q.size(); i++) {
				const Vector2 a = q[i], b = q[(i + 1) % q.size()], d = q[(i + 2) % q.size()];
				const float den = 2.0f * (a.x * (b.y - d.y) + b.x * (d.y - a.y) + d.x * (a.y - b.y));
				if (std::fabs(den) < 1e-6f * std::max(1e-6f, (b - a).length() * (d - b).length())) {
					continue; // (on a straight side)
				}
				const float a2 = a.length_squared(), b2 = b.length_squared(), d2 = d.length_squared();
				cxs.push_back((a2 * (b.y - d.y) + b2 * (d.y - a.y) + d2 * (a.y - b.y)) / den);
				cys.push_back((a2 * (d.x - b.x) + b2 * (a.x - d.x) + d2 * (b.x - a.x)) / den);
			}
			if (cxs.size() >= 4) {
				std::nth_element(cxs.begin(), cxs.begin() + cxs.size() / 2, cxs.end());
				std::nth_element(cys.begin(), cys.begin() + cys.size() / 2, cys.end());
				const Vector2 c(cxs[cxs.size() / 2], cys[cys.size() / 2]);
				float r2 = 0.0f, r1 = 1e30f;
				for (const Vector2 &p : q) {
					r2 = std::max(r2, p.distance_to(c));
					r1 = std::min(r1, p.distance_to(c));
				}
				const float tol = 0.02f * r2 + 1e-4f;
				bool on = r2 - r1 > 3.0f * tol && r1 > 0.05f * r2 && r2 < 1e4f;
				for (const Vector2 &p : q) {
					const float d = p.distance_to(c);
					on = on && (std::fabs(d - r1) < tol || std::fabs(d - r2) < tol);
				}
				// (how far round: angles from the piece's middle's)
				Vector2 mid;
				for (const Vector2 &p : q) {
					mid += p / (float)q.size();
				}
				const float a0 = (mid - c).angle();
				auto ang = [&](const Vector2 &p) {
					float t = (p - c).angle() - a0;
					while (t > 3.14159265f) {
						t -= 2.0f * 3.14159265f;
					}
					while (t <= -3.14159265f) {
						t += 2.0f * 3.14159265f;
					}
					return t;
				};
				float tmin = 1e30f, tmax = -1e30f;
				for (const Vector2 &p : q) {
					tmin = std::min(tmin, ang(p));
					tmax = std::max(tmax, ang(p));
				}
				on = on && tmax - tmin > 0.3f && tmax - tmin < 2.0f * 3.14159265f - 0.2f;
				if (on) {
					// U along the arc (at the middle radius; turned the way that keeps it unmirrored), V out from the middle
					const float rm = (r1 + r2) * 0.5f;
					P.flat.clear();
					Vector2 fmn(1e30f, 1e30f), fmx(-1e30f, -1e30f);
					for (int f : P.faces) {
						std::vector<Vector2> fl;
						for (int v : m.faces[f].v) {
							const Vector3 w = p_xf.xform(m.pos[v]);
							const Vector2 p(P.u_ax.dot(w), -P.v_ax.dot(w));
							const Vector2 r(-ang(p) * rm, p.distance_to(c) - r1);
							fl.push_back(r);
							fmn = fmn.min(r);
							fmx = fmx.max(r);
						}
						P.flat.push_back(fl);
					}
					P.strip = true;
					P.mn = fmn;
					P.w = fmx.x - fmn.x;
					P.h = fmx.y - fmn.y;
				}
			}
		}
		pieces.push_back(P);
	}
	// sizes: in meters (the texture's size in the world), or each piece against the largest one and each rectangle
	// against the largest rectangle (the same look at any scale)
	float largest_piece = 1e-6f;
	for (const Piece &P : pieces) {
		largest_piece = std::max(largest_piece, std::sqrt(std::max(0.0f, P.w * P.h)));
	}
	// Around the corners (Combine Faces): walls of one height side by side (one's right side the next one's left, an
	// upright edge between them) one run; a strip repeating along U picked for the run's whole length goes on round
	// the corners, each piece from where the one before it ended.
	std::vector<int> run_of(pieces.size(), -1);
	std::vector<float> run_at(pieces.size(), 0.0f); // (meters along the run where the piece starts)
	std::vector<std::vector<int>> runs;
	if (o.islands && pieces.size() > 1) {
		std::vector<int> piece_of(m.faces.size(), -1);
		for (int i = 0; i < (int)pieces.size(); i++) {
			for (int f : pieces[i].faces) {
				piece_of[f] = i;
			}
		}
		std::vector<int> right(pieces.size(), -1), left(pieces.size(), -1);
		std::unordered_map<uint64_t, int> seen;
		const Vector3 up(0, 1, 0);
		for (int i = 0; i < (int)pieces.size(); i++) {
			for (int f : pieces[i].faces) {
				const Face &F = m.faces[f];
				for (int k = 0; k < F.size(); k++) {
					const int a = F.v[k], b = F.v[(k + 1) % F.size()];
					const uint64_t key = ekey(a, b);
					auto it = seen.find(key);
					if (it == seen.end()) {
						seen[key] = i;
						continue;
					}
					const int j = it->second;
					if (j == i || pieces[i].kind != 1 || pieces[j].kind != 1) {
						continue;
					}
					const Piece &A = pieces[i], &B = pieces[j];
					const Face &G = m.faces[B.faces[0]];
					const bool same = atlases ? F.hs_lock == G.hs_lock && A.side == B.side : F.material == G.material; // (a box's walls: roles of their own)
					if (!same || std::fabs(A.mn.y - B.mn.y) > 0.01f || std::fabs(A.h - B.h) > 0.01f) {
						continue;
					}
					const Vector3 wa = p_xf.xform(m.pos[a]), wb = p_xf.xform(m.pos[b]);
					if ((wb - wa).length() < 1e-5f || std::fabs((wb - wa).normalized().dot(up)) < 0.95f) {
						continue; // (only an upright edge: the corner of two walls)
					}
					const Vector3 mid = (wa + wb) * 0.5f;
					const float ua = A.u_ax.dot(mid), ub = B.u_ax.dot(mid);
					const bool a_right = std::fabs(ua - (A.mn.x + A.w)) < 1e-3f, a_left = std::fabs(ua - A.mn.x) < 1e-3f;
					const bool b_right = std::fabs(ub - (B.mn.x + B.w)) < 1e-3f, b_left = std::fabs(ub - B.mn.x) < 1e-3f;
					if (a_right && b_left && right[i] < 0 && left[j] < 0) {
						right[i] = j;
						left[j] = i;
					} else if (b_right && a_left && right[j] < 0 && left[i] < 0) {
						right[j] = i;
						left[i] = j;
					}
				}
			}
		}
		std::vector<char> used(pieces.size(), 0);
		auto follow = [&](int p_start) {
			std::vector<int> r;
			for (int i = p_start; i >= 0 && !used[i]; i = right[i]) {
				used[i] = 1;
				r.push_back(i);
			}
			if (r.size() >= 2) {
				runs.push_back(r);
			}
		};
		for (int i = 0; i < (int)pieces.size(); i++) {
			if (left[i] < 0 && right[i] >= 0) {
				follow(i); // (from a run's start)
			}
		}
		for (int i = 0; i < (int)pieces.size(); i++) {
			if (!used[i] && right[i] >= 0) {
				follow(i); // (round and back: a ring of walls)
			}
		}
		for (int r = 0; r < (int)runs.size(); r++) {
			float at = 0.0f;
			for (int i : runs[r]) {
				run_of[i] = r;
				run_at[i] = at;
				at += pieces[i].w;
			}
		}
	}
	std::vector<int> run_cell(runs.size(), -2); // (-2: not picked yet; -1: none goes round, each piece its own)
	int done = 0;
	for (int pi = 0; pi < (int)pieces.size(); pi++) {
		Piece &P = pieces[pi];
		const std::vector<HotspotRect> *rects = p_rects_of(m.faces[P.faces[0]].material);
		if (!rects || rects->empty()) {
			continue;
		}
		// each rectangle's size: in meters (its atlas', or the texture size given), or against the largest one
		const float asp = std::max(1e-4f, o.aspect);
		std::vector<Vector2> sizes(rects->size());
		float largest_cell = 1e-6f;
		for (size_t i = 0; i < rects->size(); i++) {
			const HotspotRect &R = (*rects)[i];
			sizes[i] = R.meters.x > 0.0f ? R.meters : Vector2(R.r.size.x * std::max(1e-4f, o.texture_size), R.r.size.y * std::max(1e-4f, o.texture_size) / asp);
			largest_cell = std::max(largest_cell, std::sqrt(sizes[i].x * sizes[i].y));
		}
		float pw = P.w, ph = P.h;
		if (!o.world_size) {
			for (Vector2 &q : sizes) {
				q /= largest_cell;
			}
			pw /= largest_piece;
			ph /= largest_piece;
		}
		bool turned = false;
		int c = -1;
		if (o.force_cell >= 0 && o.force_cell < (int)rects->size()) {
			c = o.force_cell;
			// (chosen by hand: turned when that fits its shape better, an upright one never)
			const HotspotRect &R = (*rects)[c];
			const bool may_turn = R.turn == 2 ? true : R.turn == 1 ? false : (o.turn && R.orient != 1);
			if (may_turn && R.tile == 0) {
				const float cr = sizes[c].x / std::max(1e-6f, sizes[c].y);
				const float a0 = std::fabs(std::log((pw / std::max(1e-6f, ph)) / cr));
				const float a1 = std::fabs(std::log((ph / std::max(1e-6f, pw)) / cr));
				turned = a1 + 0.02f < a0;
			}
		} else {
			c = pick(*rects, pw, ph, P.kind, P.side, P.lock, std::max(P.w, P.h), o, sizes, P.seed, turned, P.round);
			if (!P.strip && R_may_turn_any(*rects, c, o) && std::fabs(std::log(std::max(1e-6f, P.w) / std::max(1e-6f, P.h))) < 0.15f) {
				turned = ((((P.seed ^ 0x85EBCA6Bu) * 2246822519u) >> 19) & 1u) != 0; // (near square: either way, by chance)
			}
			if (P.strip && P.w >= P.h) {
				turned = false; // (a band laid out along itself: the cell along it too, not across)
			}
		}
		// a run round corners: the cell picked for its whole length (by its first piece), when that is a strip repeating
		// along it
		const int run = run_of[pi];
		float run_from = -1.0f;
		if (run >= 0) {
			if (run_cell[run] == -2) {
				const Piece &P0 = pieces[runs[run][0]];
				float total = 0.0f;
				for (int i : runs[run]) {
					total += pieces[i].w;
				}
				bool t_run = false;
				int rc = o.force_cell >= 0 && o.force_cell < (int)rects->size() ? o.force_cell :
						pick(*rects, o.world_size ? total : total / largest_piece, o.world_size ? P0.h : P0.h / largest_piece, P0.kind, P0.side, P0.lock, std::max(total, P0.h), o, sizes, P0.seed, t_run, false);
				run_cell[run] = rc >= 0 && (*rects)[rc].tile == 1 && !t_run ? rc : -1;
			}
			if (run_cell[run] >= 0) {
				c = run_cell[run];
				turned = false;
				run_from = run_at[pi];
			}
		}
		if (c < 0) {
			continue;
		}
		const HotspotRect &R = (*rects)[c];
		// (kept off the cell's edges by its padding)
		Rect2 rr = R.r;
		const Vector2 pad = R.padding.min(rr.size * 0.45f);
		rr.position += pad;
		rr.size -= pad * 2.0f;
		const bool mirrored = R.mirror && ((P.seed >> 13) & 1u) && run_from < 0.0f; // (a run round corners: never)
		// another look each Randomise: a cell that may lie any way round (not upwards, not repeating, not a band's) half
		// turned now and then
		const uint32_t hr = (P.seed ^ 0x9E3779B9u) * 2654435761u;
		const bool half = R.orient != 1 && R.turn != 1 && R.tile == 0 && !P.strip && run_from < 0.0f && ((hr >> 17) & 1u);
		const float w = P.w, h = P.h;
		for (int fk = 0; fk < (int)P.faces.size(); fk++) {
			const int f = P.faces[fk];
			Face &F = m.faces[f];
			F.ensure_attrs();
			for (int i = 0; i < F.size(); i++) {
				const Vector3 wp = p_xf.xform(m.pos[F.v[i]]);
				// (a strip: where it was laid out along itself; else projected on the piece's plane)
				const Vector2 at = P.strip ? P.flat[fk][i] : Vector2(P.u_ax.dot(wp), -P.v_ax.dot(wp));
				Vector2 s((at.x - P.mn.x) / std::max(1e-6f, w), (at.y - P.mn.y) / std::max(1e-6f, h));
				float lu = w, lv = h; // (meters along the rectangle's U / V)
				if (turned) {
					s = Vector2(s.y, 1.0f - s.x);
					std::swap(lu, lv);
				}
				if (half) {
					s = Vector2(1.0f, 1.0f) - s;
				}
				if (mirrored) {
					s.x = 1.0f - s.x;
				}
				Vector2 uv = rr.position + s * rr.size;
				if (R.tile == 1) {
					// repeats along U: the height fills the strip, the length goes on at the same scale
					const float k = rr.size.y / std::max(1e-6f, lv);
					uv.x = rr.position.x + (s.x * lu + std::max(0.0f, run_from)) * k; // (in a run: from where the last piece ended)
				} else if (R.tile == 2) {
					const float k = rr.size.x / std::max(1e-6f, lu);
					uv.y = rr.position.y + s.y * lv * k;
				}
				F.uv[i] = uv;
			}
			F.manual_uv = true;
			F.hs_cell = atlases ? R.local : c;
			F.hs_atlas = atlases ? R.atlas : 0;
			if (atlases && R.atlas >= 0 && R.atlas < (int)o.atlas_slots.size() && o.atlas_slots[R.atlas] >= 0) {
				F.material = o.atlas_slots[R.atlas];
			}
			if (o.force_cell >= 0) {
				F.hs_fixed = true;
			}
			done++;
		}
	}
	return done;
}

} // namespace hewn
