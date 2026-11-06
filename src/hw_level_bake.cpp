#include "hw_level_bake.h"

#include "hw_cutter.h"
#include "hw_entities.h"
#include "hw_refimage.h"
#include "hw_overlay3d.h"
#include "hewn_mesh.h"

#include <godot_cpp/classes/area3d.hpp>
#include <godot_cpp/classes/array_occluder3d.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/gltf_document.hpp>
#include <godot_cpp/classes/gltf_state.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/occluder_instance3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace {

bool opaque(const Ref<Material> &p_mat) {
	Ref<BaseMaterial3D> bm = p_mat;
	return bm.is_null() || bm->get_transparency() == BaseMaterial3D::TRANSPARENCY_DISABLED;
}

// A copy of one node without its children (placed scenes: the whole instance).
Node *copy_node(Node *p_node, bool p_root) {
	if (!p_root && !p_node->get_scene_file_path().is_empty()) {
		return p_node->duplicate(); // (an instance: its own nodes come along)
	}
	Node *c = p_node->duplicate(Node::DUPLICATE_SIGNALS | Node::DUPLICATE_GROUPS | Node::DUPLICATE_SCRIPTS);
	for (int i = c->get_child_count(true) - 1; i >= 0; i--) {
		Node *ch = c->get_child(i, true);
		c->remove_child(ch);
		memdelete(ch);
	}
	return c;
}

void add_shape(Node3D *p_parent, CollisionObject3D *p_body, const String &p_name, const Ref<Shape3D> &p_shape) {
	p_body->set_name(p_name);
	CollisionShape3D *cs = memnew(CollisionShape3D);
	cs->set_name("CollisionShape3D");
	cs->set_shape(p_shape->duplicate());
	p_body->add_child(cs);
	p_parent->add_child(p_body);
}

Node3D *bake_mesh(HewnMesh3D *p_mesh, bool p_collision, bool p_occluders) {
	Ref<ArrayMesh> am = p_mesh->get_game_mesh();
	Node3D *mi = nullptr;
	if (am->get_surface_count() > 0) {
		MeshInstance3D *m = memnew(MeshInstance3D);
		m->set_mesh(am);
		m->set_material_override(p_mesh->get_material_override());
		m->set_cast_shadows_setting(p_mesh->get_cast_shadows_setting());
		m->set_gi_mode(p_mesh->get_gi_mode());
		m->set_layer_mask(p_mesh->get_layer_mask());
		m->set_visibility_range_end(p_mesh->get_visibility_range_end());
		mi = m;
	} else {
		mi = memnew(Node3D); // (only tool faces: nothing to draw)
		am.unref();
	}
	mi->set_name(p_mesh->get_name());
	mi->set_transform(p_mesh->get_transform());
	mi->set_visible(p_mesh->is_visible());
	for (const Variant &g : p_mesh->get_groups()) {
		if (!String(g).begins_with("_")) {
			mi->add_to_group(g, true);
		}
	}
	if (p_collision) {
		const Ref<Shape3D> sh = p_mesh->get_collision_shape();
		if (sh.is_valid()) {
			add_shape(mi, memnew(StaticBody3D), "StaticBody3D", sh);
		}
		const Ref<Shape3D> tr = p_mesh->get_trigger_shape();
		if (tr.is_valid()) {
			add_shape(mi, memnew(Area3D), "Trigger", tr);
		}
	}
	if (p_occluders && am.is_valid()) {
		PackedVector3Array verts;
		PackedInt32Array idx;
		for (int s = 0; s < am->get_surface_count(); s++) {
			if (!opaque(p_mesh->get_material_override().is_valid() ? p_mesh->get_material_override() : am->surface_get_material(s))) {
				continue;
			}
			const Array arr = am->surface_get_arrays(s);
			const PackedVector3Array v = arr[Mesh::ARRAY_VERTEX];
			const PackedInt32Array ix = arr[Mesh::ARRAY_INDEX];
			const int base = (int)verts.size();
			verts.append_array(v);
			for (int64_t i = 0; i < ix.size(); i++) {
				idx.push_back(base + ix[i]);
			}
		}
		if (!idx.is_empty()) {
			Ref<ArrayOccluder3D> occ;
			occ.instantiate();
			occ->set_arrays(verts, idx);
			OccluderInstance3D *oi = memnew(OccluderInstance3D);
			oi->set_name("Occluder");
			oi->set_occluder(occ);
			mi->add_child(oi);
		}
	}
	return mi;
}

// Overlays found while baking: their world-space geometry by material, merged at the end.
struct OverlayParts {
	std::vector<std::pair<Ref<Material>, Array>> parts;
};
OverlayParts *g_overlays = nullptr;

// The game copy of p_node under p_parent (or the new root when p_parent is null).
Node *bake_node(Node *p_node, Node *p_parent, bool p_collision, bool p_occluders) {
	HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(p_node);
	Node *out = nullptr;
	if (Object::cast_to<HewnCutter3D>(p_node)) {
		return nullptr; // (its cut is in the meshes' game mesh already)
	}
	if (HewnOverlay3D *ov = Object::cast_to<HewnOverlay3D>(p_node)) {
		if (g_overlays && ov->is_visible_in_tree() && ov->get_draw_material().is_valid() && ov->get_triangle_count() > 0) {
			g_overlays->parts.push_back({ ov->get_draw_material(), ov->get_world_arrays() });
		}
		for (int i = 0; i < p_node->get_child_count(); i++) {
			bake_node(p_node->get_child(i), p_parent, p_collision, p_occluders);
		}
		return nullptr;
	}
	if (pm && (pm->is_brush() || pm->is_boolean_cutter())) {
		// (its model / target has it; what it holds goes on under the parent)
		for (int i = 0; i < p_node->get_child_count(); i++) {
			bake_node(p_node->get_child(i), p_parent, p_collision, p_occluders);
		}
		return nullptr;
	}
	if (pm) {
		out = bake_mesh(pm, p_collision, p_occluders);
	} else if (p_node->has_method("make_game_node")) {
		out = Object::cast_to<Node>(p_node->call("make_game_node"));
	}
	if (!out) {
		out = copy_node(p_node, p_parent == nullptr);
	}
	if (p_parent) {
		p_parent->add_child(out);
	}
	if (!p_node->get_scene_file_path().is_empty() && p_parent) {
		return out; // (an instance came whole)
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		bake_node(p_node->get_child(i), out, p_collision, p_occluders);
	}
	return out;
}

void own(Node *p_node, Node *p_root) {
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *c = p_node->get_child(i);
		c->set_owner(p_root);
		if (c->get_scene_file_path().is_empty()) {
			own(c, p_root);
		}
	}
}

} // namespace

Node *HewnLevel::bake_scene(Node *p_root, const Dictionary &p_options) {
	ERR_FAIL_NULL_V(p_root, nullptr);
	const bool collision = p_options.get("collision", true);
	const bool occluders = p_options.get("occluders", true);
	OverlayParts overlays;
	g_overlays = &overlays;
	Node *out = bake_node(p_root, nullptr, collision, occluders);
	g_overlays = nullptr;
	if (!out) {
		out = memnew(Node3D);
		out->set_name(p_root->get_name());
	}
	// all overlays: one mesh per material (one draw call each)
	if (!overlays.parts.empty()) {
		Node3D *r3 = Object::cast_to<Node3D>(p_root);
		const Transform3D to_root = r3 && r3->is_inside_tree() ? r3->get_global_transform().affine_inverse() : Transform3D();
		std::vector<Ref<Material>> mats;
		for (const auto &p : overlays.parts) {
			if (std::find(mats.begin(), mats.end(), p.first) == mats.end()) {
				mats.push_back(p.first);
			}
		}
		Ref<ArrayMesh> am;
		am.instantiate();
		for (const Ref<Material> &mat : mats) {
			PackedVector3Array v, n;
			PackedVector2Array uv;
			PackedInt32Array idx;
			for (const auto &p : overlays.parts) {
				if (p.first != mat) {
					continue;
				}
				const PackedVector3Array pv = p.second[0], pn = p.second[1];
				const PackedVector2Array puv = p.second[2];
				const PackedInt32Array pi = p.second[3];
				const int base = (int)v.size();
				for (int64_t i = 0; i < pv.size(); i++) {
					v.push_back(to_root.xform(pv[i]));
					n.push_back(to_root.basis.xform(pn[i]).normalized());
				}
				uv.append_array(puv);
				for (int64_t i = 0; i < pi.size(); i++) {
					idx.push_back(base + pi[i]);
				}
			}
			Array arr;
			arr.resize(Mesh::ARRAY_MAX);
			arr[Mesh::ARRAY_VERTEX] = v;
			arr[Mesh::ARRAY_NORMAL] = n;
			arr[Mesh::ARRAY_TEX_UV] = uv;
			arr[Mesh::ARRAY_INDEX] = idx;
			am->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr);
			am->surface_set_material(am->get_surface_count() - 1, mat);
		}
		MeshInstance3D *mi = memnew(MeshInstance3D);
		mi->set_name("Overlays");
		mi->set_mesh(am);
		mi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		mi->set_gi_mode(GeometryInstance3D::GI_MODE_DISABLED);
		out->add_child(mi);
	}
	own(out, out);
	return out;
}

namespace {

// What a Hewn node becomes in the game (nullptr: it goes, its children stay).
Node *game_node_of(Node *p_node, bool &r_hewn) {
	r_hewn = true;
	if (HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(p_node)) {
		if (pm->is_brush() || pm->is_boolean_cutter()) {
			return nullptr; // (its model / target has it)
		}
		pm->prepare_bake();
		return bake_mesh(pm, true, false);
	}
	if (Object::cast_to<HewnCutter3D>(p_node)) {
		return nullptr; // (its cut is in the meshes)
	}
	if (HewnOverlay3D *ov = Object::cast_to<HewnOverlay3D>(p_node)) {
		MeshInstance3D *mi = memnew(MeshInstance3D);
		if (ov->get_draw_material().is_valid() && ov->get_triangle_count() > 0) {
			const Array w = ov->get_world_arrays();
			const Transform3D inv = ov->get_global_transform().affine_inverse();
			PackedVector3Array v = w[0], n = w[1];
			for (int64_t i = 0; i < v.size(); i++) {
				v.set(i, inv.xform(v[i]));
				n.set(i, inv.basis.xform(n[i]).normalized());
			}
			Array arr;
			arr.resize(Mesh::ARRAY_MAX);
			arr[Mesh::ARRAY_VERTEX] = v;
			arr[Mesh::ARRAY_NORMAL] = n;
			arr[Mesh::ARRAY_TEX_UV] = w[2];
			arr[Mesh::ARRAY_INDEX] = w[3];
			Ref<ArrayMesh> am;
			am.instantiate();
			am->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr);
			am->surface_set_material(0, ov->get_draw_material());
			mi->set_mesh(am);
		}
		mi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		mi->set_transform(ov->get_transform());
		return mi;
	}
	if (HewnEntity3D *en = Object::cast_to<HewnEntity3D>(p_node)) {
		Node3D *n = memnew(Node3D);
		n->set_transform(en->get_transform());
		if (!en->get_classname().is_empty()) {
			n->add_to_group(en->get_classname(), true);
		}
		const Dictionary props = en->get_properties();
		const Array keys = props.keys();
		for (int64_t i = 0; i < keys.size(); i++) {
			n->set_meta(StringName(String(keys[i])), props[keys[i]]);
		}
		return n;
	}
	if (HewnImage3D *im = Object::cast_to<HewnImage3D>(p_node)) {
		if (!im->get_show_in_game() || !im->get_quad()) {
			return nullptr;
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(im->get_quad()->duplicate());
		mi->set_transform(im->get_transform() * im->get_quad()->get_transform());
		mi->set_visible(true);
		return mi;
	}
	r_hewn = false;
	return nullptr;
}

void post_order(Node *p_node, Node *p_root, std::vector<Node *> &r_out) {
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *c = p_node->get_child(i);
		// (not inside placed scenes: their own scene files are exported, and converted, on their own)
		if (c->get_owner() == p_root || c->get_owner() == nullptr) {
			post_order(c, p_root, r_out);
		}
	}
	r_out.push_back(p_node);
}

} // namespace

Node *HewnLevel::to_game_nodes(Node *p_root, bool &r_changed) {
	r_changed = false;
	ERR_FAIL_NULL_V(p_root, p_root);
	std::vector<Node *> nodes;
	post_order(p_root, p_root, nodes);
	Node *root = p_root;
	std::vector<Node *> gone;
	for (Node *n : nodes) {
		bool hewn = false;
		Node *rep = game_node_of(n, hewn);
		if (!hewn) {
			continue;
		}
		r_changed = true;
		// metadata and the groups saved with the scene come along
		if (rep) {
			for (const Variant &k : n->get_meta_list()) {
				if (!rep->has_meta(k)) {
					rep->set_meta(k, n->get_meta(k));
				}
			}
			if (Node3D *r3 = Object::cast_to<Node3D>(rep)) {
				if (Node3D *n3 = Object::cast_to<Node3D>(n)) {
					r3->set_visible(n3->is_visible());
				}
			}
		}
		Node *parent = n->get_parent();
		const StringName name = n->get_name();
		n->set_name(String(name) + "_hewn_source"); // (its name goes to what replaces it)
		if (rep) {
			rep->set_name(name);
		}
		if (n == root) {
			// the root itself: its children under the new root (or a plain Node3D)
			if (!rep) {
				rep = memnew(Node3D);
				rep->set_name(name);
			}
			while (n->get_child_count() > 0) {
				Node *c = n->get_child(0);
				n->remove_child(c);
				rep->add_child(c);
			}
			if (parent) {
				const int at = n->get_index();
				parent->remove_child(n);
				parent->add_child(rep);
				parent->move_child(rep, at);
			}
			root = rep;
			gone.push_back(n);
			continue;
		}
		const int at = n->get_index();
		if (rep) {
			parent->add_child(rep);
			parent->move_child(rep, at + 1);
			rep->set_owner(root);
		}
		Node *holder = rep ? rep : parent;
		Node3D *n3 = Object::cast_to<Node3D>(n);
		while (n->get_child_count() > 0) {
			Node *c = n->get_child(0);
			Node3D *c3 = Object::cast_to<Node3D>(c);
			const Transform3D keep = c3 && n3 && !rep ? n3->get_transform() * c3->get_transform() : Transform3D();
			n->remove_child(c);
			holder->add_child(c);
			if (c3 && n3 && !rep) {
				c3->set_transform(keep); // (the node it was under goes: where it was stays)
			}
		}
		parent->remove_child(n);
		gone.push_back(n);
	}
	// owners: the nodes of this scene (not those inside placed scenes) belong to the root
	std::vector<Node *> stack = { root };
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		for (int i = 0; i < x->get_child_count(); i++) {
			Node *c = x->get_child(i);
			Node *o = c->get_owner();
			const bool ours = o == nullptr || o == p_root || std::find(gone.begin(), gone.end(), o) != gone.end();
			if (ours) {
				c->set_owner(root);
				stack.push_back(c);
			}
		}
	}
	for (Node *n : gone) {
		memdelete(n);
	}
	return root;
}

Error HewnLevel::save_baked_scene(Node *p_root, const String &p_path, const Dictionary &p_options) {
	Node *baked = bake_scene(p_root, p_options);
	ERR_FAIL_NULL_V(baked, ERR_INVALID_PARAMETER);
	Ref<PackedScene> ps;
	ps.instantiate();
	Error err = ps->pack(baked);
	if (err == OK) {
		err = ResourceSaver::get_singleton()->save(ps, p_path);
	}
	memdelete(baked);
	return err;
}

Error HewnLevel::export_scene_gltf(Node *p_root, const String &p_path, const Dictionary &p_options) {
	Dictionary opt = p_options.duplicate();
	if (!opt.has("occluders")) {
		opt["occluders"] = false; // (glTF has no occluders)
	}
	Node *baked = bake_scene(p_root, opt);
	ERR_FAIL_NULL_V(baked, ERR_INVALID_PARAMETER);
	Ref<GLTFDocument> doc;
	doc.instantiate();
	Ref<GLTFState> state;
	state.instantiate();
	Error err = doc->append_from_scene(baked, state);
	if (err == OK) {
		err = doc->write_to_filesystem(state, p_path);
	}
	memdelete(baked);
	return err;
}

int HewnLevel::enable_lightmap_uv2(Node *p_root, float p_texel_size) {
	ERR_FAIL_NULL_V(p_root, 0);
	int changed = 0;
	HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(p_root);
	if (pm && !pm->is_brush() && !pm->is_boolean_cutter() && (!pm->get_lightmap_uv2() || pm->get_lightmap_texel_size() != p_texel_size)) {
		pm->set_lightmap_texel_size(p_texel_size);
		pm->set_lightmap_uv2(true);
		changed++;
	}
	for (int i = 0; i < p_root->get_child_count(); i++) {
		changed += enable_lightmap_uv2(p_root->get_child(i), p_texel_size);
	}
	return changed;
}

void HewnLevel::_bind_methods() {
	ClassDB::bind_static_method("HewnLevel", D_METHOD("bake_scene", "root", "options"), &HewnLevel::bake_scene, DEFVAL(Dictionary()));
	ClassDB::bind_static_method("HewnLevel", D_METHOD("save_baked_scene", "root", "path", "options"), &HewnLevel::save_baked_scene, DEFVAL(Dictionary()));
	ClassDB::bind_static_method("HewnLevel", D_METHOD("export_scene_gltf", "root", "path", "options"), &HewnLevel::export_scene_gltf, DEFVAL(Dictionary()));
	ClassDB::bind_static_method("HewnLevel", D_METHOD("enable_lightmap_uv2", "root", "texel_size"), &HewnLevel::enable_lightmap_uv2, DEFVAL(0.2f));
}
