// Many small marks over the viewport (the color squares, the normals view's vertices, the X-ray face dots) in
// one draw: a 2D multimesh under the overlay, its shape drawn by a canvas shader. Made again only when what
// it shows changes (not when the mouse moves); the overlay's own drawing goes over it.
#include "hw_editor.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>

using namespace godot;

namespace {

const char *MARK_SHADER = R"(
shader_type canvas_item;
render_mode blend_mix, unshaded;
// INSTANCE_CUSTOM: x the shape (0 a dot, 1 a circle with an outline, 2 a rounded triangle with an outline,
// 3 a color square), y the alpha of a square's see-through quarters, z its border (0 black, 1 white,
// 2 selected), w the half size in pixels
varying vec4 info;
varying vec2 corner;
void vertex() {
	info = INSTANCE_CUSTOM;
	corner = VERTEX;
}
float tri(vec2 p, float r) {
	const float k = sqrt(3.0);
	p.y = -p.y - r * 0.25;
	p.x = abs(p.x) - r;
	p.y = p.y + r / k;
	if (p.x + k * p.y > 0.0) {
		p = vec2(p.x - k * p.y, -k * p.x - p.y) / 2.0;
	}
	p.x -= clamp(p.x, -2.0 * r, 0.0);
	return -length(p) * sign(p.y);
}
void fragment() {
	float h = info.w;
	vec2 p = corner * h;
	vec4 c = COLOR;
	int shape = int(info.x + 0.5);
	if (shape == 3) {
		float bw = info.z > 1.5 ? 2.0 : 1.0;
		vec3 bc = info.z > 1.5 ? vec3(1.0, 0.6, 0.1) : (info.z > 0.5 ? vec3(1.0) : vec3(0.0));
		if (max(abs(p.x), abs(p.y)) > h - bw) {
			COLOR = vec4(bc, 1.0);
		} else {
			// the top right and the bottom left quarters: as see-through as the color
			COLOR = vec4(c.rgb, p.x * p.y < 0.0 ? info.y : 1.0);
		}
	} else if (shape == 2) {
		// (selected: a thicker outline in the selection's color)
		float ow = info.z > 1.5 ? 2.0 : 1.0;
		vec3 oc = info.z > 1.5 ? vec3(1.0, 0.6, 0.1) : vec3(0.0);
		float d = tri(p, (h - 1.5) * 0.8) - (h - 1.5) * 0.12;
		float outer = clamp(0.5 - d, 0.0, 1.0);
		float inner = clamp(0.5 - (d + ow), 0.0, 1.0);
		COLOR = vec4(mix(oc, c.rgb, inner), c.a * outer);
	} else {
		float r = h - 1.0;
		float l = length(p);
		float outer = clamp(r - l + 0.5, 0.0, 1.0);
		if (shape == 1) {
			float ow = info.z > 1.5 ? 2.0 : 1.0;
			vec3 oc = info.z > 1.5 ? vec3(1.0, 0.6, 0.1) : vec3(0.0);
			float inner = clamp(r - ow - l + 0.5, 0.0, 1.0);
			COLOR = vec4(mix(oc, c.rgb, inner), c.a * outer);
		} else {
			COLOR = vec4(c.rgb, c.a * outer);
		}
	}
}
)";

} // namespace

HewnEditorPlugin::MarkSlot *HewnEditorPlugin::_mark_slot(Control *p_overlay, int p_slot) {
	RenderingServer *rs = RenderingServer::get_singleton();
	const ObjectID id(p_overlay->get_instance_id());
	MarkLayer *layer = nullptr;
	for (MarkLayer &l : mark_layers) {
		if (l.overlay == id) {
			layer = &l;
		}
	}
	if (!layer) {
		mark_layers.push_back(MarkLayer());
		layer = &mark_layers.back();
		layer->overlay = id;
	}
	MarkSlot &s = layer->slots[p_slot];
	if (!s.item.is_valid()) {
		if (mark_material.is_null()) {
			Ref<Shader> sh;
			sh.instantiate();
			sh->set_code(MARK_SHADER);
			mark_material.instantiate();
			mark_material->set_shader(sh);
			// the quad: -1..1, white
			PackedVector2Array v;
			v.push_back(Vector2(-1, -1));
			v.push_back(Vector2(1, -1));
			v.push_back(Vector2(1, 1));
			v.push_back(Vector2(-1, 1));
			PackedColorArray c;
			for (int i = 0; i < 4; i++) {
				c.push_back(Color(1, 1, 1, 1));
			}
			PackedInt32Array idx;
			for (int i : { 0, 1, 2, 0, 2, 3 }) {
				idx.push_back(i);
			}
			Array a;
			a.resize(Mesh::ARRAY_MAX);
			a[Mesh::ARRAY_VERTEX] = v;
			a[Mesh::ARRAY_COLOR] = c;
			a[Mesh::ARRAY_INDEX] = idx;
			mark_quad.instantiate();
			mark_quad->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, a);
		}
		s.item = rs->canvas_item_create();
		rs->canvas_item_set_parent(s.item, p_overlay->get_canvas_item());
		rs->canvas_item_set_draw_behind_parent(s.item, true);
		rs->canvas_item_set_material(s.item, mark_material->get_rid());
		s.mm = rs->multimesh_create();
		rs->multimesh_set_mesh(s.mm, mark_quad->get_rid());
		s.key = 0;
		s.count = -1;
	}
	return &s;
}

void HewnEditorPlugin::_marks_begin(Control *p_overlay) {
	for (MarkLayer &l : mark_layers) {
		if (l.overlay == ObjectID(p_overlay->get_instance_id())) {
			for (MarkSlot &s : l.slots) {
				s.used = false;
			}
		}
	}
}

void HewnEditorPlugin::_marks_end(Control *p_overlay) {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (MarkLayer &l : mark_layers) {
		if (l.overlay != ObjectID(p_overlay->get_instance_id())) {
			continue;
		}
		for (MarkSlot &s : l.slots) {
			if (!s.used && s.item.is_valid() && s.shown) {
				rs->canvas_item_set_visible(s.item, false);
				s.shown = false;
			}
		}
	}
}

bool HewnEditorPlugin::_marks_cached(Control *p_overlay, int p_slot, uint64_t p_key) {
	MarkSlot *s = _mark_slot(p_overlay, p_slot);
	s->used = true;
	if (s->key != p_key || s->count < 0) {
		return false;
	}
	if (!s->shown) {
		RenderingServer::get_singleton()->canvas_item_set_visible(s->item, true);
		s->shown = true;
	}
	return true;
}

void HewnEditorPlugin::_marks_set(Control *p_overlay, int p_slot, uint64_t p_key, const std::vector<Mark> &p_marks) {
	RenderingServer *rs = RenderingServer::get_singleton();
	MarkSlot *s = _mark_slot(p_overlay, p_slot);
	s->used = true;
	s->key = p_key;
	const int n = (int)p_marks.size();
	if (n != s->count) {
		rs->multimesh_allocate_data(s->mm, n, RenderingServer::MULTIMESH_TRANSFORM_2D, true, true);
		s->count = n;
	}
	if (n > 0) {
		PackedFloat32Array buf;
		buf.resize(n * 16);
		float *w = buf.ptrw();
		for (const Mark &m : p_marks) {
			// (the 2D transform's rows, then the color, then the custom data)
			const float vals[16] = { m.half, 0.0f, 0.0f, m.at.x, 0.0f, m.half, 0.0f, m.at.y, m.color.r, m.color.g, m.color.b, m.color.a, m.shape, m.alpha, m.border, m.half };
			for (float x : vals) {
				*w++ = x;
			}
		}
		rs->multimesh_set_buffer(s->mm, buf);
	}
	rs->canvas_item_clear(s->item);
	if (n > 0) {
		rs->canvas_item_add_multimesh(s->item, s->mm);
	}
	rs->canvas_item_set_visible(s->item, true);
	s->shown = true;
}

void HewnEditorPlugin::_marks_free() {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (MarkLayer &l : mark_layers) {
		for (MarkSlot &s : l.slots) {
			if (s.item.is_valid()) {
				rs->free_rid(s.item);
			}
			if (s.mm.is_valid()) {
				rs->free_rid(s.mm);
			}
		}
	}
	mark_layers.clear();
}

// A key for what the marks of the edited mesh depend on: the camera and the view, the mesh (any change), its
// selection, the X-ray.
uint64_t HewnEditorPlugin::_marks_view_key(Camera3D *p_cam, HewnMesh3D *p_node) const {
	uint64_t h = 1469598103934665603ull;
	auto mix = [&](const void *p, size_t n) {
		const unsigned char *c = (const unsigned char *)p;
		for (size_t i = 0; i < n; i++) {
			h = (h ^ c[i]) * 1099511628211ull;
		}
	};
	const Transform3D cx = p_cam->get_global_transform();
	const Transform3D nx = p_node->get_global_transform();
	mix(&cx, sizeof(cx));
	mix(&nx, sizeof(nx));
	const float cam[6] = { (float)p_cam->get_fov(), (float)p_cam->get_size(), (float)p_cam->get_near(), (float)p_cam->get_far(), (float)p_cam->get_h_offset(), (float)p_cam->get_v_offset() };
	mix(cam, sizeof(cam));
	const int proj = (int)p_cam->get_projection();
	mix(&proj, sizeof(proj));
	const Vector2 vp = p_cam->get_viewport() ? p_cam->get_viewport()->get_visible_rect().size : Vector2();
	mix(&vp, sizeof(vp));
	const uint64_t ids[2] = { (uint64_t)p_cam->get_instance_id(), (uint64_t)p_node->get_instance_id() };
	mix(ids, sizeof(ids));
	// (the positions and the faces, which are hidden: not the colors, so a brush step keeps the vertices' places
	// and which are seen)
	_bvh_sync(p_node);
	mix(&bvh_hash, sizeof(bvh_hash));
	for (const hewn::Face &F : p_node->get_data().faces) {
		h = (h ^ (F.hidden ? 0x9Bu : 0x17u)) * 1099511628211ull;
	}
	const hewn::Selection &s = p_node->selection;
	if (!s.verts.empty()) {
		mix(s.verts.data(), s.verts.size() * sizeof(int));
	}
	for (const hewn::Edge &e : s.edges) {
		mix(&e.a, sizeof(int));
		mix(&e.b, sizeof(int));
	}
	if (!s.faces.empty()) {
		mix(s.faces.data(), s.faces.size() * sizeof(int));
	}
	const int flags[4] = { (int)mode, mode_mask, xray_on ? 1 : 0, cull_back() ? 1 : 0 };
	mix(flags, sizeof(flags));
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	mix(&es, sizeof(es));
	return h;
}
