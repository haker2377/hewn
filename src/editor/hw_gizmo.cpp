// Viewport overlay of the edited mesh: wireframe, vertices and selected faces.
// Lines and points are expanded in the vertex shader so their width is constant in pixels;
// everything is pulled slightly toward the camera to win the depth test against the mesh. Edges whose
// faces all turn their back to the camera (culled: see-through) are drawn black.
#include "hw_editor.h"
#include "hw_bvh.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shape3d.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include <string>

using namespace godot;

namespace {

const char *LINE_SHADER = R"(
shader_type spatial;
render_mode unshaded, cull_disabled, shadows_disabled, depth_draw_never, fog_disabled;
uniform float bias = 0.002;
varying float back;
// CUSTOM1 / CUSTOM2: the normals of the edge's faces (w: 1 = there is that face); CUSTOM1.w = 2: the
// line shows only when it is a back edge
void vertex() {
	vec4 a = MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
	vec4 b = MODELVIEW_MATRIX * vec4(CUSTOM0.xyz, 1.0);
	back = 0.0;
	if (CUSTOM1.w > 0.5) {
		mat3 nm = transpose(inverse(mat3(MODELVIEW_MATRIX)));
		vec3 view = PROJECTION_MATRIX[2][3] == 0.0 ? vec3(0.0, 0.0, -1.0) : (a.xyz + b.xyz) * 0.5;
		bool b1 = dot(nm * CUSTOM1.xyz, view) > 0.0;
		bool b2 = CUSTOM2.w < 0.5 || dot(nm * CUSTOM2.xyz, view) > 0.0;
		back = b1 && b2 ? 1.0 : 0.0;
	}
	a.xyz *= 1.0 - bias;
	b.xyz *= 1.0 - bias;
	vec4 ca = PROJECTION_MATRIX * a;
	vec4 cb = PROJECTION_MATRIX * b;
	vec2 sa = ca.xy / ca.w * VIEWPORT_SIZE * 0.5;
	vec2 sb = cb.xy / cb.w * VIEWPORT_SIZE * 0.5;
	vec2 d = sb - sa;
	d = length(d) > 1e-5 ? normalize(d) : vec2(1.0, 0.0);
	vec2 n = vec2(-d.y, d.x);
	ca.xy += n * UV.x * CUSTOM0.w / VIEWPORT_SIZE * ca.w;
	// only-back lines that are not: outside the view, not drawn
	POSITION = CUSTOM1.w > 1.5 && back < 0.5 ? vec4(2.0, 2.0, 2.0, 1.0) : ca;
}
void fragment() {
	ALBEDO = mix(COLOR.rgb, vec3(0.0), back);
	ALPHA = mix(COLOR.a, 1.0, back);
}
)";

const char *POINT_SHADER = R"(
shader_type spatial;
render_mode unshaded, cull_disabled, shadows_disabled, depth_draw_never, fog_disabled;
uniform float bias = 0.004;
varying vec2 corner;
void vertex() {
	vec4 a = MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
	a.xyz *= 1.0 - bias;
	vec4 c = PROJECTION_MATRIX * a;
	c.xy += UV * CUSTOM0.w / VIEWPORT_SIZE * c.w;
	corner = UV;
	POSITION = c;
}
void fragment() {
	float r = length(corner);
	if (r > 1.0) {
		discard;
	}
	ALBEDO = r > 0.65 ? COLOR.rgb * 0.25 : COLOR.rgb;
	ALPHA = COLOR.a;
}
)";

const char *FACE_SHADER = R"(
shader_type spatial;
render_mode unshaded, cull_back, shadows_disabled, depth_draw_never, fog_disabled;
uniform float bias = 0.001;
void vertex() {
	vec4 a = MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
	a.xyz *= 1.0 - bias;
	POSITION = PROJECTION_MATRIX * a;
}
void fragment() {
	ALBEDO = COLOR.rgb;
	ALPHA = COLOR.a;
}
)";

Ref<ShaderMaterial> make_material(const char *code) {
	Ref<Shader> sh;
	sh.instantiate();
	sh->set_code(code);
	Ref<ShaderMaterial> m;
	m.instantiate();
	m->set_shader(sh);
	m->set_render_priority(10);
	return m;
}

const Color C_WIRE(0.05f, 0.08f, 0.12f, 0.85f);
const Color C_WIRE_FACE_MODE(0.05f, 0.08f, 0.12f, 0.6f);
const Color C_SELECT(1.0f, 0.55f, 0.08f, 1.0f);
const Color C_HOVER(1.0f, 0.95f, 0.35f, 1.0f);
const Color C_VERT(0.0f, 0.0f, 0.0f, 1.0f); // (black, like the wire)
const Color C_ACTIVE(1.0f, 1.0f, 1.0f, 1.0f); // the element clicked last

// The normals of the faces of an edge (for the back edges).
struct EdgeFaces {
	Vector3 n[2];
	int count = 0;
};

struct Lines {
	PackedVector3Array v;
	PackedFloat32Array custom;
	PackedFloat32Array c1, c2; // (lines and points; empty for the faces)
	PackedVector2Array uv;
	PackedColorArray col;
	PackedInt32Array idx;

	// p_back: 0 normal, 1 black when its faces are back faces, 2 only then
	// p_cb: the color at b (the line fades from c to it), else c all along
	void add(const Vector3 &a, const Vector3 &b, float width, const Color &c, const EdgeFaces *p_faces = nullptr, int p_back = 0, const Color *p_cb = nullptr) {
		const int base = (int)v.size();
		const Vector3 ends[4] = { a, a, b, b };
		const Vector3 others[4] = { b, b, a, a };
		// The perpendicular flips when seen from the other end, so the same signs walk around the quad.
		const float side[4] = { -1, 1, -1, 1 };
		for (int i = 0; i < 4; i++) {
			v.push_back(ends[i]);
			custom.push_back(others[i].x);
			custom.push_back(others[i].y);
			custom.push_back(others[i].z);
			custom.push_back(width);
			uv.push_back(Vector2(side[i], 0));
			col.push_back(i >= 2 && p_cb ? *p_cb : c);
			const bool on = p_faces && p_faces->count > 0 && p_back > 0;
			for (int j = 0; j < 2; j++) {
				PackedFloat32Array &dst = j == 0 ? c1 : c2;
				const Vector3 nn = on && j < p_faces->count ? p_faces->n[j] : Vector3();
				dst.push_back(nn.x);
				dst.push_back(nn.y);
				dst.push_back(nn.z);
				dst.push_back(!on || j >= p_faces->count ? 0.0f : j == 0 ? (float)p_back : 1.0f);
			}
		}
		idx.push_back(base);
		idx.push_back(base + 1);
		idx.push_back(base + 2);
		idx.push_back(base);
		idx.push_back(base + 2);
		idx.push_back(base + 3);
	}
	void point(const Vector3 &p, float size, const Color &c) {
		const int base = (int)v.size();
		const Vector2 corners[4] = { Vector2(-1, -1), Vector2(1, -1), Vector2(1, 1), Vector2(-1, 1) };
		for (int i = 0; i < 4; i++) {
			v.push_back(p);
			custom.push_back(0);
			custom.push_back(0);
			custom.push_back(0);
			custom.push_back(size);
			uv.push_back(corners[i]);
			col.push_back(c);
			for (int j = 0; j < 4; j++) {
				c1.push_back(0);
				c2.push_back(0);
			}
		}
		idx.push_back(base);
		idx.push_back(base + 1);
		idx.push_back(base + 2);
		idx.push_back(base);
		idx.push_back(base + 2);
		idx.push_back(base + 3);
	}
	Ref<ArrayMesh> mesh() const {
		Ref<ArrayMesh> m;
		if (v.is_empty()) {
			return m;
		}
		m.instantiate();
		Array a;
		a.resize(Mesh::ARRAY_MAX);
		a[Mesh::ARRAY_VERTEX] = v;
		a[Mesh::ARRAY_CUSTOM0] = custom;
		a[Mesh::ARRAY_TEX_UV] = uv;
		a[Mesh::ARRAY_COLOR] = col;
		a[Mesh::ARRAY_INDEX] = idx;
		int64_t flags = (int64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT;
		if (c1.size() == custom.size()) {
			a[Mesh::ARRAY_CUSTOM1] = c1;
			a[Mesh::ARRAY_CUSTOM2] = c2;
			flags |= ((int64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM1_SHIFT) |
					((int64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM2_SHIFT);
		}
		m->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, a, Array(), Dictionary(), BitField<Mesh::ArrayFormat>(flags));
		return m;
	}
};

std::unordered_map<uint64_t, EdgeFaces> edge_faces(const hewn::Mesh &m) {
	std::unordered_map<uint64_t, EdgeFaces> out;
	for (const hewn::Face &F : m.faces) {
		if (F.hidden) {
			continue;
		}
		const Vector3 n = hewn::face_normal(m, F);
		for (int i = 0; i < F.size(); i++) {
			EdgeFaces &e = out[hewn::ekey(F.v[i], F.v[(i + 1) % F.size()])];
			if (e.count < 2) {
				e.n[e.count++] = n;
			}
		}
	}
	return out;
}

// The edited mesh's overlay in three parts, each kept while what it shows stays (a color painted changes none
// of them: the gizmo is not made again on every brush step).
struct Hasher {
	uint64_t h = 1469598103934665603ull;
	void bytes(const void *p, size_t n) {
		const unsigned char *c = (const unsigned char *)p;
		for (size_t i = 0; i < n; i++) {
			h = (h ^ c[i]) * 1099511628211ull;
		}
	}
	void u(uint64_t x) { bytes(&x, sizeof(x)); }
	void f(float x) { bytes(&x, sizeof(x)); }
};

struct PartCache {
	uint64_t faces = 0, lines = 0, points = 0;
	Ref<ArrayMesh> fm, lm, pm;
};
std::unordered_map<uint64_t, PartCache> part_cache;

} // namespace

// (the meshes kept go before the renderer does)
void hw_gizmo_clear_cache() {
	part_cache.clear();
}

HewnGizmoPlugin::HewnGizmoPlugin() {
	line_material = make_material(LINE_SHADER);
	create_material("collision", Color(0.0f, 0.6f, 0.7f, 0.9f)); // (the color of collision shapes)
	point_material = make_material(POINT_SHADER);
	point_material->set_render_priority(11); // (the vertices over the edges, whatever is selected)
	face_material = make_material(FACE_SHADER);
	face_material->set_render_priority(9);
}

bool HewnGizmoPlugin::_has_gizmo(Node3D *p_node) const {
	return Object::cast_to<HewnMesh3D>(p_node) != nullptr;
}

String HewnGizmoPlugin::_get_gizmo_name() const {
	return "HewnMesh3D";
}

int32_t HewnGizmoPlugin::_get_priority() const {
	return 1;
}

void HewnGizmoPlugin::_redraw(const Ref<EditorNode3DGizmo> &p_gizmo) {
	p_gizmo->clear();
	HewnMesh3D *node = Object::cast_to<HewnMesh3D>(p_gizmo->get_node_3d());
	if (!node || !plugin || !plugin->overlays_on()) {
		return; // overlays off: no wireframe and no selection marks at all
	}
	if (plugin->dock && plugin->dock->overlay(plugin->dock->ov_collision) && node->is_visible_in_tree()) {
		Ref<Shape3D> shape = node->get_collision_shape();
		Ref<ArrayMesh> dm = shape.is_valid() ? shape->get_debug_mesh() : Ref<ArrayMesh>();
		if (dm.is_valid() && dm->get_surface_count() > 0) {
			const Array arr = dm->surface_get_arrays(0);
			const PackedVector3Array lines = arr[Mesh::ARRAY_VERTEX];
			if (!lines.is_empty()) {
				p_gizmo->add_lines(lines, get_material("collision", p_gizmo));
			}
		}
	}
	if (!plugin->is_edited_mesh(node) || plugin->mode == HewnEditorPlugin::MODE_OBJECT) {
		// Wireframe overlay (object mode, other objects): the edges of the evaluated mesh. (The black edges of
		// faces seen from behind: only on the meshes being edited.)
		const float opacity = plugin->wireframe_opacity();
		if (!node->is_visible_in_tree() || opacity <= 0.0f) {
			return;
		}
		const hewn::Mesh em = node->get_modifiers().is_empty() ? node->get_data() : node->evaluated_mesh();
		const bool selected = EditorInterface::get_singleton()->get_selection()->get_selected_nodes().has(node) || plugin->get_edited() == node;
		const Color c = selected ? Color(1.0f, 0.6f, 0.2f, opacity) : Color(0.0f, 0.0f, 0.0f, opacity);
		const std::unordered_map<uint64_t, EdgeFaces> ef = edge_faces(em);
		Lines wire;
		std::unordered_set<uint64_t> seen;
		for (const hewn::Face &F : em.faces) {
			const int n = F.size();
			for (int i = 0; i < n; i++) {
				const int a = F.v[i], b = F.v[(i + 1) % n];
				const uint64_t k = hewn::ekey(a, b);
				if (seen.insert(k).second) {
					auto it = ef.find(k);
					wire.add(em.pos[a], em.pos[b], 1.0f, c, it != ef.end() ? &it->second : nullptr, 0);
				}
			}
		}
		for (const std::pair<int, int> &wr : em.wires) {
			wire.add(em.pos[wr.first], em.pos[wr.second], 1.0f, c);
		}
		Ref<ArrayMesh> wm = wire.mesh();
		if (wm.is_valid()) {
			p_gizmo->add_mesh(wm, line_material);
		}
		return;
	}
	const hewn::Mesh &m = node->get_data();
	const hewn::BuildOut &b = node->get_build();
	const hewn::Selection &s = node->selection;
	const int mode = plugin->mode;

	const bool vmode = plugin->has_mode(HewnEditorPlugin::MODE_VERTEX);
	const bool emode = plugin->has_mode(HewnEditorPlugin::MODE_EDGE);
	const bool fmode = plugin->has_mode(HewnEditorPlugin::MODE_FACE);
	(void)mode;
	const bool edited_here = plugin->get_edited() == node;
	const bool normals_view = plugin->paint_view == 2 && edited_here; // (the Shading menu: in every mode)
	const bool paint_marks = plugin->vpaint && edited_here && plugin->dock &&
			(plugin->paint_view == 2 || (plugin->dock->paint_squares_btn && plugin->dock->paint_squares_btn->is_pressed()));
	const bool fill = fmode && plugin->show_face_fill();
	// the active element (clicked last), while it is still selected
	const bool act_here = edited_here && plugin->active_node == node->get_instance_id();
	const int act_vert = act_here && vmode && std::find(s.verts.begin(), s.verts.end(), plugin->active_vert) != s.verts.end() ? plugin->active_vert : -1;
	const uint64_t act_edge = act_here && emode && plugin->active_edge.a >= 0 && std::find(s.edges.begin(), s.edges.end(), plugin->active_edge) != s.edges.end() ?
			hewn::ekey(plugin->active_edge) :
			~0ull;
	const int act_face = act_here && fmode && std::find(s.faces.begin(), s.faces.end(), plugin->active_face) != s.faces.end() &&
					plugin->active_face < (int)m.faces.size() ?
			plugin->active_face :
			-1;

	// what each part depends on (not the colors / UVs / materials)
	if (part_cache.size() > 64) {
		for (auto it = part_cache.begin(); it != part_cache.end();) {
			it = ObjectDB::get_instance(ObjectID(it->first)) ? std::next(it) : part_cache.erase(it);
		}
	}
	PartCache &pc = part_cache[node->get_instance_id()];
	Hasher base;
	base.u(hw_editor::geometry_hash(m));
	base.u(b.face_tris.size());
	std::vector<char> hidden(m.faces.size());
	for (size_t f = 0; f < m.faces.size(); f++) {
		hidden[f] = m.faces[f].hidden ? 1 : 0;
	}
	if (!hidden.empty()) {
		base.bytes(hidden.data(), hidden.size());
	}
	for (const std::pair<int, int> &wr : m.wires) {
		base.u(((uint64_t)(uint32_t)wr.first << 32) | (uint32_t)wr.second);
	}
	for (int v : m.points) {
		base.u(0xabcdull + (uint64_t)v);
	}
	// what the modifiers add
	const bool has_mods = !node->get_modifiers().is_empty();
	if (has_mods) {
		base.u(node->get_revision());
	}
	Hasher hf = base, hl = base, hp = base;
	// faces: the selected ones (and the one under the mouse), the normals view's smooth / flat
	hf.u((normals_view ? 1 : 0) | (fill ? 2 : 0));
	if (fill) {
		hf.u((uint64_t)(int64_t)act_face);
		hf.bytes(s.faces.data(), s.faces.size() * sizeof(int));
		hf.u((uint64_t)(int64_t)(edited_here ? plugin->hover_face : -2));
	}
	if (normals_view) {
		for (const hewn::Face &F : m.faces) {
			hf.u((uint64_t)(int64_t)F.smooth);
		}
	}
	// lines: the marks of the edges, the selection, the edge under the mouse
	hl.u((vmode ? 1 : 0) | (emode ? 2 : 0) | (fmode ? 4 : 0) | (normals_view ? 8 : 0));
	for (const auto &kv : m.edge_attrs) {
		hl.u(kv.first);
		hl.u((kv.second.seam ? 1 : 0) | (kv.second.sharp ? 2 : 0));
		hl.f(kv.second.crease);
		hl.f(kv.second.bevel_weight);
	}
	for (const hewn::Edge &e : s.edges) {
		hl.u(hewn::ekey(e));
	}
	hl.bytes(s.faces.data(), s.faces.size() * sizeof(int));
	hl.bytes(s.verts.data(), s.verts.size() * sizeof(int));
	hl.u(edited_here && plugin->hover_edge.a >= 0 ? hewn::ekey(plugin->hover_edge) : ~0ull);
	hl.u(act_edge);
	hl.u((uint64_t)(int64_t)act_face);
	// points: the vertices, selected / under the mouse
	hp.u((vmode ? 1 : 0) | (paint_marks ? 2 : 0));
	hp.bytes(s.verts.data(), s.verts.size() * sizeof(int));
	hp.u((uint64_t)(int64_t)(edited_here ? plugin->hover_vert : -2));
	hp.u((uint64_t)(int64_t)act_vert);
	const bool faces_new = pc.faces != hf.h, lines_new = pc.lines != hl.h, points_new = pc.points != hp.h;

	// Vertex Paint's normals view: smooth faces blue, flat ones orange (the sharp edges red below)
	Lines faces;
	if (faces_new && normals_view) {
		for (int f = 0; f < (int)m.faces.size() && f < (int)b.face_tris.size(); f++) {
			const hewn::Face &F = m.faces[f];
			if (F.hidden) {
				continue;
			}
			const Color c = F.smooth != 0 ? Color(0.25f, 0.65f, 1.0f, 0.35f) : Color(1.0f, 0.55f, 0.2f, 0.35f);
			const std::vector<int> &t = b.face_tris[f];
			for (size_t k = 0; k + 2 < t.size(); k += 3) {
				const int ids[3] = { t[k], t[k + 2], t[k + 1] };
				for (int id : ids) {
					faces.v.push_back(m.pos[F.v[id]]);
					for (int z = 0; z < 4; z++) {
						faces.custom.push_back(0);
					}
					faces.uv.push_back(Vector2());
					faces.col.push_back(c);
					faces.idx.push_back((int)faces.idx.size());
				}
			}
		}
	}
	// Selected faces.
	if (faces_new && fill) {
		std::vector<char> fsel(m.faces.size(), 0);
		for (int f : s.faces) {
			if (f >= 0 && f < (int)fsel.size()) {
				fsel[f] = 1;
			}
		}
		for (int f = 0; f < (int)m.faces.size() && f < (int)b.face_tris.size(); f++) {
			Color c;
			if (fsel[f]) {
				const Color sc = f == act_face ? C_SELECT.lerp(C_ACTIVE, 0.4f) : C_SELECT; // (the active one a bit lighter)
				c = Color(sc.r, sc.g, sc.b, f == (plugin->get_edited() == node ? plugin->hover_face : -2) ? 0.5f : 0.38f);
			} else if (f == (plugin->get_edited() == node ? plugin->hover_face : -2)) {
				c = Color(C_HOVER.r, C_HOVER.g, C_HOVER.b, 0.18f);
			} else {
				continue;
			}
			const std::vector<int> &t = b.face_tris[f];
			for (size_t k = 0; k + 2 < t.size(); k += 3) {
				// emit clockwise for Godot
				const int ids[3] = { t[k], t[k + 2], t[k + 1] };
				for (int id : ids) {
					faces.v.push_back(m.pos[m.faces[f].v[id]]);
					faces.custom.push_back(0);
					faces.custom.push_back(0);
					faces.custom.push_back(0);
					faces.custom.push_back(0);
					faces.uv.push_back(Vector2());
					faces.col.push_back(c);
					faces.idx.push_back((int)faces.idx.size());
				}
			}
		}
	}

	// Wireframe.
	Lines lines;
	if (lines_new) {
	std::unordered_set<uint64_t> esel;
	for (const hewn::Edge &e : s.edges) {
		esel.insert(hewn::ekey(e));
	}
	std::unordered_set<uint64_t> fedges;
	if (fmode) {
		for (int f : s.faces) {
			if (f < 0 || f >= (int)m.faces.size()) {
				continue;
			}
			const hewn::Face &F = m.faces[f];
			for (int i = 0; i < F.size(); i++) {
				fedges.insert(hewn::ekey(F.v[i], F.v[(i + 1) % F.size()]));
			}
		}
	}
	const uint64_t hover_key = plugin->get_edited() == node && plugin->hover_edge.a >= 0 ? hewn::ekey(plugin->hover_edge) : ~0ull;
	std::unordered_set<uint64_t> act_face_edges; // (the active face's border: white)
	if (act_face >= 0) {
		const hewn::Face &F = m.faces[act_face];
		for (int i = 0; i < F.size(); i++) {
			act_face_edges.insert(hewn::ekey(F.v[i], F.v[(i + 1) % F.size()]));
		}
	}
	const std::unordered_map<uint64_t, EdgeFaces> ef = edge_faces(m);
	const bool vonly = vmode && !emode && !fmode;
	std::vector<char> vsel_l(m.pos.size(), 0);
	if (vonly) {
		for (int v : s.verts) {
			if (v >= 0 && v < (int)vsel_l.size()) {
				vsel_l[v] = 1;
			}
		}
	}
	std::unordered_set<uint64_t> done;
	for (const hewn::Face &F : m.faces) {
		const int n = F.hidden ? 0 : F.size(); // (hidden: its edges only when a shown face has them)
		for (int i = 0; i < n; i++) {
			const int a = F.v[i], c = F.v[(i + 1) % n];
			const uint64_t k = hewn::ekey(a, c);
			if (!done.insert(k).second) {
				continue;
			}
			float w = 1.0f;
			Color col = fmode && !emode && !vmode ? C_WIRE_FACE_MODE : C_WIRE;
			int back = 1; // (plain wire: black over see-through faces)
			// colors for edge marks: seam red, sharp teal, crease purple, bevel weight blue
			if (const hewn::EdgeAttr *ea = m.edge_attr(a, c)) {
				if (normals_view && ea->sharp) {
					col = Color(1.0f, 0.2f, 0.2f, 0.9f);
				} else if (ea->seam) {
					col = Color::hex(0x8c3228ff);
				} else if (ea->sharp) {
					col = Color::hex(0x209f9fff);
				} else if (ea->crease > 0.0f) {
					col = Color::hex(0x84206bff).lerp(col, 1.0f - ea->crease);
				} else if (ea->bevel_weight > 0.0f) {
					col = Color::hex(0x20719fff).lerp(col, 1.0f - ea->bevel_weight);
				}
				w = 1.6f;
				back = 0;
			}
			if (emode) {
				w = MAX(w, 1.3f);
				if (esel.count(k)) {
					col = k == act_edge ? C_ACTIVE : C_SELECT;
					w = 2.2f;
					back = 0;
				} else if (k == hover_key) {
					col = C_HOVER;
					w = 2.0f;
					back = 0;
				}
			} else if (fedges.count(k)) {
				col = act_face_edges.count(k) ? C_ACTIVE : C_SELECT;
				w = 1.4f;
				back = 0;
			}
			auto it = ef.find(k);
			if (vonly && (vsel_l[a] || vsel_l[c])) {
				// vertex mode: an edge between selected vertices in the selection color, from a selected one to
				// one that is not it fades to the wire color
				const Color ca = vsel_l[a] ? C_SELECT : col, cc = vsel_l[c] ? C_SELECT : col;
				lines.add(m.pos[a], m.pos[c], MAX(w, 1.4f), ca, it != ef.end() ? &it->second : nullptr, 0, &cc);
				continue;
			}
			lines.add(m.pos[a], m.pos[c], w, col, it != ef.end() ? &it->second : nullptr, back);
		}
	}
	if (has_mods) {
		const hewn::Mesh &em = node->get_shown_data();
		// (by position: the modifiers' vertices are new ones; the edges the mesh has itself are drawn above)
		auto qk = [](const Vector3 &p) {
			return Vector3i((int)std::lround(p.x * 1e4f), (int)std::lround(p.y * 1e4f), (int)std::lround(p.z * 1e4f));
		};
		auto pkey = [&](const Vector3 &a, const Vector3 &b) {
			const Vector3i x = qk(a), y = qk(b);
			const Vector3i lo = x < y ? x : y, hi = x < y ? y : x;
			return String::num_int64(lo.x) + "," + String::num_int64(lo.y) + "," + String::num_int64(lo.z) + ";" + String::num_int64(hi.x) + "," + String::num_int64(hi.y) + "," + String::num_int64(hi.z);
		};
		std::unordered_set<std::string> own;
		for (const hewn::Face &F : m.faces) {
			for (int i = 0; i < F.size(); i++) {
				own.insert(pkey(m.pos[F.v[i]], m.pos[F.v[(i + 1) % F.size()]]).utf8().get_data());
			}
		}
		const Color dim(C_WIRE.r, C_WIRE.g, C_WIRE.b, C_WIRE.a * 0.55f);
		const std::unordered_map<uint64_t, EdgeFaces> eef = edge_faces(em);
		std::unordered_set<uint64_t> edone;
		for (const hewn::Face &F : em.faces) {
			for (int i = 0; i < F.size(); i++) {
				const int a = F.v[i], c = F.v[(i + 1) % F.size()];
				const uint64_t k = hewn::ekey(a, c);
				if (!edone.insert(k).second || own.count(pkey(em.pos[a], em.pos[c]).utf8().get_data())) {
					continue;
				}
				auto it = eef.find(k);
				lines.add(em.pos[a], em.pos[c], 1.0f, dim, it != eef.end() ? &it->second : nullptr, 1);
			}
		}
	}
	// Loose (wire) edges.
	for (const std::pair<int, int> &wr : m.wires) {
		const uint64_t k = hewn::ekey(wr.first, wr.second);
		const bool on = esel.count(k) || (vmode && std::find(s.verts.begin(), s.verts.end(), wr.first) != s.verts.end() &&
										   std::find(s.verts.begin(), s.verts.end(), wr.second) != s.verts.end());
		lines.add(m.pos[wr.first], m.pos[wr.second], on ? 2.2f : 1.3f, on ? C_SELECT : C_WIRE);
	}
	} // (lines_new)

	// Vertices.
	Lines points;
	// (Vertex Paint: the color squares / the normals view's marks are the vertices)
	if (points_new && vmode && !paint_marks) {
		std::vector<char> vsel(m.pos.size(), 0);
		for (int v : s.verts) {
			if (v >= 0 && v < (int)vsel.size()) {
				vsel[v] = 1;
			}
		}
		const std::vector<char> used = hw_editor::visible_verts(m);
		for (int v = 0; v < (int)m.pos.size(); v++) {
			if (!used[v]) {
				continue;
			}
			if (vsel[v]) {
				points.point(m.pos[v], 6.5f, v == act_vert ? C_ACTIVE : C_SELECT);
			} else if (v == (plugin->get_edited() == node ? plugin->hover_vert : -2)) {
				points.point(m.pos[v], 6.5f, C_HOVER);
			} else {
				points.point(m.pos[v], 5.0f, C_VERT);
			}
		}
	}

	if (points_new && vmode && !paint_marks && has_mods) {
		// the vertices the modifiers add: dim
		const hewn::Mesh &em = node->get_shown_data();
		std::unordered_set<std::string> own;
		auto qs = [](const Vector3 &p) {
			return String::num_int64(std::lround(p.x * 1e4f)) + "," + String::num_int64(std::lround(p.y * 1e4f)) + "," + String::num_int64(std::lround(p.z * 1e4f));
		};
		for (const Vector3 &p : m.pos) {
			own.insert(qs(p).utf8().get_data());
		}
		const std::vector<char> used = hw_editor::visible_verts(em);
		for (int v = 0; v < (int)em.pos.size(); v++) {
			if (used[v] && !own.count(qs(em.pos[v]).utf8().get_data())) {
				points.point(em.pos[v], 4.0f, Color(C_VERT.r, C_VERT.g, C_VERT.b, C_VERT.a * 0.5f));
			}
		}
	}
	if (faces_new) {
		pc.fm = faces.mesh();
		pc.faces = hf.h;
	}
	if (lines_new) {
		pc.lm = lines.mesh();
		pc.lines = hl.h;
	}
	if (points_new) {
		pc.pm = points.mesh();
		pc.points = hp.h;
	}
	if (pc.fm.is_valid()) {
		p_gizmo->add_mesh(pc.fm, face_material);
	}
	if (pc.lm.is_valid()) {
		p_gizmo->add_mesh(pc.lm, line_material);
	}
	if (pc.pm.is_valid()) {
		p_gizmo->add_mesh(pc.pm, point_material);
	}
}
