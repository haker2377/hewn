// Hewn — light baked into the vertex colors: the scene's lights (directional, omni, spot, with their
// shadows), an ambient color and ambient occlusion, computed at every corner and written into the color
// (or a CUSTOM channel). Where the light changes more than the corners can show (a shadow's edge across a
// big face), the faces can be subdivided first, within a budget. One undo step.
#include "hw_bvh.h"
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/color_picker_button.hpp>
#include <godot_cpp/classes/confirmation_dialog.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_separator.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/spot_light3d.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/world_environment.hpp>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace godot;
using namespace hw_editor;

namespace {

struct BakeLight {
	int kind = 0; // 0 directional, 1 omni, 2 spot
	Vector3 pos, dir; // dir: where the light shines (directional / spot)
	Color color;
	float energy = 1.0f, range = 5.0f, decay = 1.0f, cos_angle = 0.0f, spot_att = 1.0f;
	bool shadow = false;
};

struct Occluder {
	hw_editor::TriBVH tree;
	Transform3D inv;
	uint64_t node = 0;
};

void gather(Node *p_node, std::vector<BakeLight> &r_lights, Color &r_ambient, bool &r_env) {
	if (Light3D *l = Object::cast_to<Light3D>(p_node)) {
		if (l->is_visible_in_tree() && l->get_bake_mode() != Light3D::BAKE_DISABLED) {
			BakeLight b;
			const Transform3D g = l->get_global_transform();
			b.pos = g.origin;
			b.dir = (-g.basis.get_column(2)).normalized();
			b.color = l->get_color();
			b.energy = (float)l->get_param(Light3D::PARAM_ENERGY);
			b.shadow = l->has_shadow();
			if (Object::cast_to<DirectionalLight3D>(l)) {
				b.kind = 0;
			} else if (Object::cast_to<SpotLight3D>(l)) {
				b.kind = 2;
				b.range = (float)l->get_param(Light3D::PARAM_RANGE);
				b.decay = (float)l->get_param(Light3D::PARAM_ATTENUATION);
				b.cos_angle = std::cos((float)l->get_param(Light3D::PARAM_SPOT_ANGLE) * (float)Math::PI / 180.0f);
				b.spot_att = (float)l->get_param(Light3D::PARAM_SPOT_ATTENUATION);
			} else if (Object::cast_to<OmniLight3D>(l)) {
				b.kind = 1;
				b.range = (float)l->get_param(Light3D::PARAM_RANGE);
				b.decay = (float)l->get_param(Light3D::PARAM_ATTENUATION);
			}
			r_lights.push_back(b);
		}
	}
	if (WorldEnvironment *we = Object::cast_to<WorldEnvironment>(p_node)) {
		Ref<Environment> env = we->get_environment();
		if (env.is_valid() && !r_env) {
			r_env = true;
			r_ambient = env->get_ambient_light_color() * (float)env->get_ambient_light_energy();
			r_ambient.a = 1.0f;
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		gather(p_node->get_child(i), r_lights, r_ambient, r_env);
	}
}

// Godot's own falloff of omni / spot lights (the same numbers the renderer gives)
float omni_falloff(float p_dist, float p_range, float p_decay) {
	if (p_range <= 0.0f) {
		return 0.0f;
	}
	float nd = p_dist / p_range;
	nd *= nd;
	nd *= nd;
	nd = MAX(1.0f - nd, 0.0f);
	nd *= nd;
	return nd * std::pow(MAX(p_dist, 0.0001f), -p_decay);
}

float area_of(const hewn::Mesh &m, const hewn::Face &F) {
	Vector3 a;
	for (int i = 1; i + 1 < F.size(); i++) {
		a += (m.pos[F.v[i]] - m.pos[F.v[0]]).cross(m.pos[F.v[i + 1]] - m.pos[F.v[0]]);
	}
	return a.length() * 0.5f;
}

uint32_t hash32(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

// A MeshInstance3D's triangles as a mesh of triangles (its own space), to block the light too.
bool triangles_of(const Ref<Mesh> &p_mesh, hewn::Mesh &r_m, hewn::BuildOut &r_b) {
	if (p_mesh.is_null()) {
		return false;
	}
	for (int s = 0; s < p_mesh->get_surface_count(); s++) {
		Ref<ArrayMesh> am = p_mesh;
		if (am.is_valid() && am->surface_get_primitive_type(s) != Mesh::PRIMITIVE_TRIANGLES) {
			continue; // (lines, points: nothing to block the light with)
		}
		const Array arr = p_mesh->surface_get_arrays(s);
		if (arr.size() <= Mesh::ARRAY_INDEX) {
			continue;
		}
		const PackedVector3Array vs = arr[Mesh::ARRAY_VERTEX];
		const PackedInt32Array idx = arr[Mesh::ARRAY_INDEX];
		const int base = (int)r_m.pos.size();
		for (int64_t i = 0; i < vs.size(); i++) {
			r_m.pos.push_back(vs[i]);
		}
		const int64_t n = idx.is_empty() ? vs.size() : idx.size();
		for (int64_t i = 0; i + 2 < n; i += 3) {
			hewn::Face F;
			for (int k = 0; k < 3; k++) {
				F.v.push_back(base + (idx.is_empty() ? (int)(i + k) : idx[i + k]));
			}
			r_m.faces.push_back(F);
			r_b.face_tris.push_back({ 0, 1, 2 });
		}
	}
	return !r_m.faces.empty();
}

void gather_meshes(Node *p_node, std::vector<MeshInstance3D *> &r_out) {
	MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_node);
	if (mi && !Object::cast_to<HewnMesh3D>(p_node) && mi->is_visible_in_tree() &&
			mi->get_cast_shadows_setting() != GeometryInstance3D::SHADOW_CASTING_SETTING_OFF) {
		r_out.push_back(mi);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		gather_meshes(p_node->get_child(i), r_out);
	}
}

} // namespace

// The settings, then the bake (the dialog's OK).
void HewnEditorPlugin::open_bake_light() {
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	if (!lbake_dialog) {
		lbake_dialog = memnew(ConfirmationDialog);
		lbake_dialog->set_name("HewnBakeLight");
		lbake_dialog->set_title(T("Bake Light to Vertex Colors"));
		lbake_dialog->set_ok_button_text(T("Bake"));
		VBoxContainer *v = memnew(VBoxContainer);
		lbake_dialog->add_child(v);
		lbake_info = memnew(Label);
		lbake_info->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		lbake_info->set_custom_minimum_size(Vector2(380, 0) * es);
		v->add_child(lbake_info);
		GridContainer *g = memnew(GridContainer);
		g->set_columns(2);
		v->add_child(g);
		auto row = [&](const char *p_text, Control *p_c, const char *p_tip) {
			Label *l = memnew(Label);
			l->set_text(T(p_text));
			l->set_tooltip_text(T(p_tip));
			l->set_mouse_filter(Control::MOUSE_FILTER_PASS);
			g->add_child(l);
			p_c->set_tooltip_text(T(p_tip));
			p_c->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			g->add_child(p_c);
		};
		auto spin = [&](double p_min, double p_max, double p_step, double p_val) {
			SpinBox *s = memnew(SpinBox);
			HewnDragField::hook_spinbox(s);
			s->set_min(p_min);
			s->set_max(p_max);
			s->set_step(p_step);
			s->set_value(p_val);
			return s;
		};
		auto check = [&](bool p_on) {
			CheckBox *c = memnew(CheckBox);
			c->set_pressed(p_on);
			return c;
		};
		lbake_target = memnew(OptionButton);
		for (const char *it : { "Color", "CUSTOM0", "CUSTOM1", "CUSTOM2", "CUSTOM3" }) {
			lbake_target->add_item(it);
		}
		row("Into", lbake_target, "Where the light goes: the vertex color (RGB; the alpha is kept) or a CUSTOM channel");
		{
			HBoxContainer *h = memnew(HBoxContainer);
			lbake_use_ambient = check(true);
			lbake_use_ambient->set_tooltip_text(T("Off: no light from everywhere, only the lights"));
			h->add_child(lbake_use_ambient);
			lbake_ambient = memnew(ColorPickerButton);
			lbake_ambient->set_edit_alpha(false);
			lbake_ambient->set_custom_minimum_size(Vector2(60, 0) * es);
			lbake_ambient->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			h->add_child(lbake_ambient);
			lbake_amb_k = spin(0, 100, 0.01, 1.0);
			lbake_amb_k->set_name("AmbientStrength");
			lbake_amb_k->set_tooltip_text(T("Its strength"));
			h->add_child(lbake_amb_k);
			row("Ambient", h, "The light from everywhere: the scene's WorldEnvironment ambient color (its color times its energy); without one a gray of 0.3");
		}
		// each kind of light: on / off and its own strength
		auto kind_row = [&](const char *p_text, CheckBox *&r_check, SpinBox *&r_k, const char *p_name, const char *p_tip) {
			HBoxContainer *h = memnew(HBoxContainer);
			r_check = check(true);
			r_check->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			h->add_child(r_check);
			r_k = spin(0, 100, 0.01, 1.0);
			r_k->set_name(p_name);
			r_k->set_tooltip_text(T("Its strength"));
			h->add_child(r_k);
			row(p_text, h, p_tip);
		};
		kind_row("Directional Lights", lbake_use_dir, lbake_dir_k, "DirectionalStrength", "The DirectionalLight3D nodes (the sun)");
		kind_row("Omni Lights", lbake_use_point, lbake_omni_k, "OmniStrength", "The OmniLight3D nodes");
		kind_row("Spot Lights", lbake_use_spot, lbake_spot_k, "SpotStrength", "The SpotLight3D nodes");
		lbake_shadows = check(true);
		row("Shadows", lbake_shadows, "The lights with Shadow on are blocked by what is in the way: Hewn objects and MeshInstance3D nodes with Cast Shadow on");
		lbake_mesh_shadows = check(true);
		lbake_mesh_shadows->set_name("MeshInstanceShadows");
		row("MeshInstance3D Shadows", lbake_mesh_shadows, "Off: only Hewn objects block the light and darken the corners, the MeshInstance3D nodes do not");
		lbake_ao_rays = spin(0, 512, 1, 32);
		row("Occlusion Rays", lbake_ao_rays, "Ambient occlusion: rays from each corner (0: off); more: smoother, slower");
		lbake_ao_dist = spin(0.01, 1000, 0.01, 1.0);
		lbake_ao_dist->set_suffix("m");
		row("Occlusion Distance", lbake_ao_dist, "How far something darkens a corner");
		lbake_strength = spin(0, 100, 0.01, 1.0);
		row("Strength", lbake_strength, "The whole light times this");
		lbake_refine = check(false);
		row("Subdivide Where It Changes", lbake_refine,
				"Faces too big for the light on them (a shadow's edge, a spot's circle across them: the corners alone cannot show it) are subdivided, again and again, only there; the light is looked at in their middle and in the middle of their edges");
		lbake_threshold = spin(0.01, 1, 0.01, 0.1);
		row("Difference", lbake_threshold, "How much the light in a face may differ from what its corners show before it is subdivided (0.1: a tenth of white); bigger: fewer faces");
		lbake_passes = spin(1, 8, 1, 3);
		row("Times at Most", lbake_passes, "How many times a face can be subdivided again (each time its parts in four)");
		lbake_budget = spin(10, 1000000, 10, 5000);
		row("New Faces at Most", lbake_budget, "No more faces than this are added; the faces where the light differs most go first");
		lbake_only_sel = check(false);
		row("Only the Selection", lbake_only_sel, "Edit mode: only the corners of the selected elements");
		lbake_show = check(true);
		row("Show in the Materials", lbake_show, "Into the color: turn on Vertex Color > Use as Albedo in the objects' standard materials (without it they do not show the vertex colors; the material files change for every object using them)");
		lbake_dialog->connect("confirmed", callable_mp(this, &HewnEditorPlugin::bake_light));
		EditorInterface::get_singleton()->get_base_control()->add_child(lbake_dialog);
		localize(lbake_dialog);
	}
	// what is in the scene now
	std::vector<BakeLight> lights;
	Color amb(0.3f, 0.3f, 0.3f);
	bool env = false;
	if (Node *root = EditorInterface::get_singleton()->get_edited_scene_root()) {
		gather(root, lights, amb, env);
	}
	if (!lbake_ambient_set || env) {
		lbake_ambient->set_pick_color(amb); // (the scene's ambient each time there is one)
		lbake_ambient_set = true;
	}
	if (dock && dock->paint_channel && modal == MODAL_PAINT) {
		lbake_target->select(dock->paint_channel->get_selected());
	}
	HewnMesh3D *n = get_edited();
	const bool sel = n && mode != MODE_OBJECT && (!n->selection.verts.empty() || !n->selection.edges.empty() || !n->selection.faces.empty());
	lbake_only_sel->set_disabled(!sel);
	if (!sel) {
		lbake_only_sel->set_pressed(false);
	}
	int dirs = 0;
	for (const BakeLight &L : lights) {
		dirs += L.kind == 0 ? 1 : 0;
	}
	String info = lights.empty() ? T("No lights in the scene: only the ambient color and the occlusion.")
								 : vformat(T("Lights in the scene: %d (directional: %d)."), (int)lights.size(), dirs);
	lbake_info->set_text(info);
	lbake_dialog->reset_size();
	lbake_dialog->popup_centered(Vector2i(Vector2(460, 0) * es));
}

// The meshes: in edit mode the edited ones, else the selected Hewn objects. The light at each corner.
void HewnEditorPlugin::bake_light() {
	std::vector<HewnMesh3D *> targets;
	const bool edit = get_edited() && mode != MODE_OBJECT;
	if (edit) {
		targets = edited_meshes();
	} else {
		const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
		for (int64_t i = 0; i < sel.size(); i++) {
			if (HewnMesh3D *x = Object::cast_to<HewnMesh3D>(sel[i])) {
				targets.push_back(x);
			}
		}
		if (targets.empty() && get_edited()) {
			targets.push_back(get_edited());
		}
	}
	if (targets.empty()) {
		ERR_PRINT("Hewn: select the objects to bake the light into first.");
		return;
	}
	std::vector<BakeLight> all_lights, lights;
	Color amb_scene;
	bool env = false;
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (root) {
		gather(root, all_lights, amb_scene, env);
	}
	const bool use_dir = !lbake_use_dir || lbake_use_dir->is_pressed();
	const bool use_point = !lbake_use_point || lbake_use_point->is_pressed();
	const bool use_spot = !lbake_use_spot || lbake_use_spot->is_pressed();
	const float kinds_k[3] = { lbake_dir_k ? (float)lbake_dir_k->get_value() : 1.0f, lbake_omni_k ? (float)lbake_omni_k->get_value() : 1.0f,
		lbake_spot_k ? (float)lbake_spot_k->get_value() : 1.0f };
	for (BakeLight L : all_lights) {
		if (L.kind == 0 ? use_dir : L.kind == 1 ? use_point : use_spot) {
			L.energy *= kinds_k[CLAMP(L.kind, 0, 2)];
			lights.push_back(L);
		}
	}
	const bool use_amb = !lbake_use_ambient || lbake_use_ambient->is_pressed();
	Color amb = !use_amb ? Color(0, 0, 0) : lbake_ambient ? lbake_ambient->get_pick_color() : Color(0.3f, 0.3f, 0.3f);
	if (lbake_amb_k) {
		const float ak = (float)lbake_amb_k->get_value();
		amb = Color(amb.r * ak, amb.g * ak, amb.b * ak, 1.0f);
	}
	const int rays = lbake_ao_rays ? (int)lbake_ao_rays->get_value() : 0;
	const float ao_dist = lbake_ao_dist ? (float)lbake_ao_dist->get_value() : 1.0f;
	const bool shadows = !lbake_shadows || lbake_shadows->is_pressed();
	const float strength = lbake_strength ? (float)lbake_strength->get_value() : 1.0f;
	const int ch = lbake_target ? lbake_target->get_selected() : 0;
	const bool only_sel = edit && lbake_only_sel && lbake_only_sel->is_pressed() && !lbake_only_sel->is_disabled();
	const bool refine = lbake_refine && lbake_refine->is_pressed();
	const float threshold = lbake_threshold ? (float)lbake_threshold->get_value() : 0.1f;
	const int passes = lbake_passes ? (int)lbake_passes->get_value() : 3;
	int64_t budget = lbake_budget ? (int64_t)lbake_budget->get_value() : 5000;

	// what blocks the light: every shown Hewn object (a CSG model as its result, not its brushes) and the
	// MeshInstance3D nodes that cast shadows
	std::vector<std::unique_ptr<Occluder>> occ;
	auto occluder_of = [&](HewnMesh3D *o) {
		std::unique_ptr<Occluder> x = std::make_unique<Occluder>();
		x->tree.build(o->get_shown_data(), o->get_shown_build());
		x->inv = o->get_global_transform().affine_inverse();
		x->node = o->get_instance_id();
		return x;
	};
	if ((shadows && !lights.empty()) || rays > 0) {
		for (HewnMesh3D *o : _scene_meshes()) {
			if (o->is_brush() || o->get_cast_shadows_setting() == GeometryInstance3D::SHADOW_CASTING_SETTING_OFF) {
				continue;
			}
			occ.push_back(occluder_of(o));
		}
		std::vector<MeshInstance3D *> mis;
		if (root && (!lbake_mesh_shadows || lbake_mesh_shadows->is_pressed())) {
			gather_meshes(root, mis);
		}
		for (MeshInstance3D *mi : mis) {
			hewn::Mesh tm;
			hewn::BuildOut tb;
			if (!triangles_of(mi->get_mesh(), tm, tb)) {
				continue;
			}
			std::unique_ptr<Occluder> x = std::make_unique<Occluder>();
			x->tree.build(tm, tb);
			x->inv = mi->get_global_transform().affine_inverse();
			occ.push_back(std::move(x));
		}
	}
	auto blocked = [&](const Vector3 &p_from, const Vector3 &p_dir, float p_max, float *r_t) {
		float best = p_max;
		bool hit = false;
		for (const std::unique_ptr<Occluder> &o : occ) {
			float t = 0.0f;
			if (o->tree.raycast(o->inv.xform(p_from), o->inv.basis.xform(p_dir), t, false) >= 0 && t < best) {
				best = t;
				hit = true;
			}
		}
		if (r_t) {
			*r_t = best;
		}
		return hit;
	};
	// the light at a point (world) with its normal
	auto light_at = [&](const Vector3 &P, const Vector3 &N, uint32_t p_seed) {
		Color sum = amb;
		if (rays > 0) {
			// cosine weighted directions over the normal, turned per point (no banding between corners)
			const Vector3 t1 = (std::fabs(N.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).cross(N).normalized();
			const Vector3 t2 = N.cross(t1);
			const float turn = (hash32(p_seed) & 0xffff) / 65536.0f;
			float occl = 0.0f;
			for (int k = 0; k < rays; k++) {
				const float u1 = (k + 0.5f) / rays;
				const float u2 = std::fmod(k * 0.6180339887f + turn, 1.0f);
				const float r = std::sqrt(u1), phi = 2.0f * (float)Math::PI * u2;
				const Vector3 d = (t1 * (r * std::cos(phi)) + t2 * (r * std::sin(phi)) + N * std::sqrt(MAX(0.0f, 1.0f - u1))).normalized();
				float t;
				if (blocked(P, d, ao_dist, &t)) {
					occl += 1.0f - t / ao_dist; // (near things darken more)
				}
			}
			const float ao = 1.0f - occl / rays;
			sum = Color(sum.r * ao, sum.g * ao, sum.b * ao, 1.0f);
		}
		for (const BakeLight &L : lights) {
			Vector3 to_l;
			float dist = 1e6f, att = 1.0f;
			if (L.kind == 0) {
				to_l = -L.dir;
			} else {
				to_l = L.pos - P;
				dist = to_l.length();
				if (dist < 1e-5f) {
					continue;
				}
				to_l /= dist;
				att = omni_falloff(dist, L.range, L.decay);
				if (L.kind == 2) {
					const float scos = MAX(L.dir.dot(-to_l), L.cos_angle);
					const float rim = MAX(0.0001f, (1.0f - scos) / MAX(1.0f - L.cos_angle, 1e-5f));
					att *= 1.0f - std::pow(rim, L.spot_att);
				}
			}
			const float ndl = N.dot(to_l);
			if (ndl <= 0.0f || att <= 0.0f) {
				continue;
			}
			if (shadows && L.shadow && blocked(P, to_l, dist, nullptr)) {
				continue;
			}
			const float k = ndl * att * L.energy;
			sum.r += L.color.r * k;
			sum.g += L.color.g * k;
			sum.b += L.color.b * k;
		}
		Color c(sum.r * strength, sum.g * strength, sum.b * strength, 1.0f);
		if (ch == 0) {
			c = Color(CLAMP(c.r, 0.0f, 1.0f), CLAMP(c.g, 0.0f, 1.0f), CLAMP(c.b, 0.0f, 1.0f), 1.0f);
		}
		return c;
	};

	EditorUndoRedoManager *ur = get_undo_redo();
	ur->create_action("Hewn: Bake Light to Vertex Colors", UndoRedo::MERGE_DISABLE, targets[0]);
	for (HewnMesh3D *n : targets) {
		const Dictionary before = n->get_state();
		const TypedArray<Material> mats_before = n->get_materials().duplicate();
		if (ch > 0 && n->custom_formats[ch - 1] < 0) {
			n->custom_formats[ch - 1] = dock && dock->paint_format ? dock->paint_format->get_selected() : 0;
		}
		// which corners: per face, per corner (only the selection: its corners; faces cut out of them: all theirs)
		std::vector<std::vector<char>> on;
		{
			const hewn::Mesh &m0 = n->get_data();
			std::vector<char> flat;
			if (only_sel) {
				_paint_selected_corners(n, true, flat);
			}
			size_t k = 0;
			on.resize(m0.faces.size());
			for (size_t f = 0; f < m0.faces.size(); f++) {
				on[f].assign(m0.faces[f].size(), 1);
				for (int i = 0; i < m0.faces[f].size(); i++, k++) {
					if (only_sel) {
						on[f][i] = k < flat.size() ? flat[k] : 0;
					}
				}
			}
		}
		bool changed_geometry = false;
		std::vector<std::vector<Color>> cl;
		for (int pass = 0;; pass++) {
			hewn::Mesh &m = n->get_data();
			const Transform3D g = n->get_global_transform();
			const Basis nb = g.basis.inverse().transposed();
			std::vector<Vector3> fn(m.faces.size());
			for (size_t f = 0; f < m.faces.size(); f++) {
				fn[f] = hewn::face_normal(m, m.faces[f]) * MAX(area_of(m, m.faces[f]), 1e-8f);
			}
			std::vector<std::vector<int>> vfaces(m.pos.size());
			for (size_t f = 0; f < m.faces.size(); f++) {
				for (int v : m.faces[f].v) {
					vfaces[v].push_back((int)f);
				}
			}
			const float scale = MAX(g.basis.get_scale().length() / 1.7320508f, 1e-4f);
			const float eps = 0.002f * scale;
			// the normal of a corner: its face's, or (smooth) its smoothing group's around the vertex
			auto corner_normal = [&](int f, int i) {
				const hewn::Face &F = m.faces[f];
				Vector3 nl = fn[f];
				if (F.smooth != 0) {
					nl = Vector3();
					for (int o : vfaces[F.v[i]]) {
						if (m.faces[o].smooth == F.smooth) {
							nl += fn[o];
						}
					}
				}
				Vector3 N = nb.xform(nl).normalized();
				return N.length_squared() < 0.5f ? nb.xform(fn[f]).normalized() : N;
			};
			cl.assign(m.faces.size(), {});
			for (size_t f = 0; f < m.faces.size(); f++) {
				hewn::Face &F = m.faces[f];
				if (F.hidden) {
					continue;
				}
				Vector3 center;
				for (int v : F.v) {
					center += m.pos[v];
				}
				center /= (float)MAX(F.size(), 1);
				const Vector3 fnw = nb.xform(fn[f]).normalized();
				cl[f].resize(F.size());
				for (int i = 0; i < F.size(); i++) {
					if (f < on.size() && i < (int)on[f].size() && !on[f][i]) {
						continue;
					}
					const int v = F.v[i];
					// a little in from the corner and off the surface: not shadowed by its own face / its neighbors
					const Vector3 P = g.xform(m.pos[v] + (center - m.pos[v]) * 0.01f) + fnw * eps;
					cl[f][i] = light_at(P, corner_normal((int)f, i), (uint32_t)v * 2654435761U ^ (uint32_t)f);
				}
			}
			if (!refine || pass >= passes || budget <= 0) {
				break;
			}
			// where the corners cannot show the light: its middle and its edges' middles against theirs
			std::vector<std::pair<float, int>> bad;
			for (size_t f = 0; f < m.faces.size(); f++) {
				const hewn::Face &F = m.faces[f];
				if (F.hidden || cl[f].empty() || F.size() < 3) {
					continue;
				}
				bool all = true;
				for (int i = 0; i < F.size(); i++) {
					all = all && (f >= on.size() || i >= (int)on[f].size() || on[f][i]);
				}
				if (!all) {
					continue; // (the selection's border: left as it is)
				}
				const Vector3 fnw = nb.xform(fn[f]).normalized();
				Vector3 c_l;
				Vector3 c_n;
				Color c_mean(0, 0, 0, 0);
				for (int i = 0; i < F.size(); i++) {
					c_l += m.pos[F.v[i]];
					c_n += corner_normal((int)f, i);
					c_mean += cl[f][i];
				}
				c_l /= (float)F.size();
				c_mean /= (float)F.size();
				auto diff = [](const Color &a, const Color &b) {
					return MAX(std::fabs(a.r - b.r), MAX(std::fabs(a.g - b.g), std::fabs(a.b - b.b)));
				};
				float err = diff(light_at(g.xform(c_l) + fnw * eps, c_n.normalized(), (uint32_t)f * 7919U), c_mean);
				for (int i = 0; i < F.size(); i++) {
					const int j = (i + 1) % F.size();
					const Vector3 mid = (m.pos[F.v[i]] + m.pos[F.v[j]]) * 0.5f;
					const Vector3 P = g.xform(mid + (c_l - mid) * 0.01f) + fnw * eps;
					const Vector3 N = (corner_normal((int)f, i) + corner_normal((int)f, j)).normalized();
					err = MAX(err, diff(light_at(P, N, (uint32_t)f * 31U + (uint32_t)i), (cl[f][i] + cl[f][j]) * 0.5f));
				}
				if (err > threshold) {
					bad.push_back({ err, (int)f });
				}
			}
			if (bad.empty()) {
				break;
			}
			std::sort(bad.begin(), bad.end(), [](const std::pair<float, int> &a, const std::pair<float, int> &b) { return a.first > b.first; });
			std::vector<int> split;
			for (const std::pair<float, int> &b : bad) {
				const int add = m.faces[b.second].size() == 4 ? 3 : m.faces[b.second].size(); // (a quad: four; a triangle or more: one per corner)
				if (add > budget) {
					break;
				}
				budget -= add;
				split.push_back(b.second);
			}
			if (split.empty()) {
				break;
			}
			const size_t nf0 = m.faces.size();
			const std::vector<int> made = hewn::ops::subdivide_faces_ex(m, split, 1, 0.0f, true, 0.0f, 0.0f, 0);
			// the new faces are all baked; the ones that were there keep what they had
			on.resize(m.faces.size());
			for (size_t f = 0; f < m.faces.size(); f++) {
				if (f >= nf0 || on[f].size() != (size_t)m.faces[f].size()) {
					on[f].assign(m.faces[f].size(), 1);
				}
			}
			for (int f : made) {
				if (f >= 0 && f < (int)on.size()) {
					on[f].assign(m.faces[f].size(), 1);
				}
			}
			n->selection.clear();
			n->commit(true);
			changed_geometry = true;
			for (std::unique_ptr<Occluder> &o : occ) {
				if (o->node == n->get_instance_id()) {
					o = occluder_of(n); // (its own new faces block the light as they are now)
				}
			}
		}
		hewn::Mesh &m = n->get_data();
		for (size_t f = 0; f < m.faces.size() && f < cl.size(); f++) {
			hewn::Face &F = m.faces[f];
			for (int i = 0; i < F.size() && i < (int)cl[f].size(); i++) {
				if (f < on.size() && i < (int)on[f].size() && !on[f][i]) {
					continue;
				}
				F.ensure_attrs();
				Color &x = _paint_value(F, i, ch);
				x = Color(cl[f][i].r, cl[f][i].g, cl[f][i].b, x.a);
			}
		}
		n->commit(changed_geometry);
		ur->add_do_method(n, "set_state", n->get_state());
		ur->add_undo_method(n, "set_state", before);
		if (ch == 0 && lbake_show && lbake_show->is_pressed()) {
			const TypedArray<Material> mats = n->get_materials();
			for (int64_t i = 0; i < mats.size(); i++) {
				Ref<BaseMaterial3D> bm = mats[i];
				if (bm.is_valid() && !bm->get_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR)) {
					bm->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
					ur->add_do_method(bm.ptr(), "set_flag", BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
					ur->add_undo_method(bm.ptr(), "set_flag", BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, false);
				}
			}
		}
		(void)mats_before;
	}
	ur->commit_action(false);
	if (dock) {
		dock->refresh();
	}
	update_overlays();
}
