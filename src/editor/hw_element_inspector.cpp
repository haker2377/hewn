// The Element Inspector: a window over the viewport (bottom right, dragged by its title) with everything known
// of the selected vertices, edges and faces, in sections that fold, and the values that can be changed there.
#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/color_picker_button.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_resource_picker.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/tab_container.hpp>
#include <godot_cpp/classes/style_box.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include <unordered_set>

using namespace godot;
using namespace hw_editor;

namespace {
String v3(const Vector3 &p) {
	return vformat("%.4f, %.4f, %.4f", p.x, p.y, p.z);
}
String col_text(const Color &c) {
	return vformat("%.3f, %.3f, %.3f, %.3f", c.r, c.g, c.b, c.a);
}
String index_list(const std::vector<int> &p_list) {
	String out;
	for (size_t i = 0; i < p_list.size() && i < 40; i++) {
		out += (i ? ", " : "") + itos(p_list[i]);
	}
	if (p_list.size() > 40) {
		out += vformat(" ... (+%d)", (int)p_list.size() - 40);
	}
	return out;
}
const char *ROLE_NAMES[] = { "None", "Surface", "Top", "Bottom", "Front", "Back", "Left", "Right", "Slope", "Side", "Inner", "Outer", "Tread", "Riser",
	"Underside", "Border", "Opening" };
} // namespace

uint64_t HewnEditorPlugin::_ei_state_hash() const {
	HewnMesh3D *n = get_edited();
	if (!n || mode == MODE_OBJECT) {
		return 1;
	}
	uint64_t h = 1469598103934665603ull;
	auto mix = [&](uint64_t x) { h = (h ^ x) * 1099511628211ull; };
	mix(n->get_instance_id());
	mix(n->get_revision());
	mix((uint64_t)mode_mask);
	const hewn::Selection &s = n->selection;
	for (int v : s.verts) {
		mix((uint64_t)v + 1);
	}
	mix(0xffff);
	for (const hewn::Edge &e : s.edges) {
		mix(hewn::ekey(e));
	}
	mix(0xfffe);
	for (int f : s.faces) {
		mix((uint64_t)f + 7);
	}
	return h;
}

// The Inspector tab (after UV): rebuilt when the selection or the mesh changed, only while the tab is shown (not
// during an interactive operation: once it is done).
void HewnEditorPlugin::_ei_update(bool p_force) {
	VBoxContainer *ei_body = dock ? dock->ei_tab_body : nullptr;
	if (!ei_body || !ei_body->is_visible_in_tree()) {
		if (!p_force) {
			return;
		}
	}
	if (!ei_body) {
		return;
	}
	HewnMesh3D *n = get_edited();
	const uint64_t h = _ei_state_hash();
	if (!p_force && (h == ei_hash || modal != MODAL_NONE)) {
		return;
	}
	ei_hash = h;
	const float es = EditorInterface::get_singleton()->get_editor_scale();
	for (int i = ei_body->get_child_count() - 1; i >= 0; i--) {
		Node *c = ei_body->get_child(i);
		ei_body->remove_child(c);
		c->queue_free();
	}
	if (!n || mode == MODE_OBJECT) {
		Label *l = memnew(Label);
		l->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		l->set_custom_minimum_size(Vector2(160, 0) * es);
		l->set_text(T("Select vertices, edges or faces of a Hewn mesh in edit mode: everything known of them shows here"));
		ei_body->add_child(l);
		return;
	}
	const hewn::Mesh &m = n->get_data();
	const hewn::Selection &s = n->selection;
	const Transform3D g = n->get_global_transform();
	const std::vector<int> verts = _selected_verts();
	std::vector<int> faces;
	for (int f : s.faces) {
		if (f >= 0 && f < (int)m.faces.size()) {
			faces.push_back(f);
		}
	}
	std::vector<hewn::Edge> edges = s.edges;

	// ---- the builders
	auto section = [&](const char *p_name) -> GridContainer * {
		const bool open = (bool)ei_open.get(p_name, String(p_name) != "Mesh Data");
		Button *b = memnew(Button);
		b->set_name(String("Section_") + String(p_name).replace(" ", ""));
		b->set_text(T(p_name));
		b->set_flat(true);
		b->set_toggle_mode(true);
		b->set_pressed_no_signal(open);
		b->set_focus_mode(Control::FOCUS_NONE);
		b->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		b->set_button_icon(EditorInterface::get_singleton()->get_editor_theme()->get_icon(open ? "arrow" : "arrow_collapsed", "Tree"));
		b->connect("toggled", callable_mp(this, &HewnEditorPlugin::_ei_section_toggled).bind(String(p_name)));
		ei_body->add_child(b);
		GridContainer *gc = memnew(GridContainer);
		gc->set_columns(2);
		gc->add_theme_constant_override("h_separation", (int)(6 * es));
		gc->add_theme_constant_override("v_separation", (int)(1 * es));
		gc->set_visible(open);
		ei_body->add_child(gc);
		return gc;
	};
	auto label = [&](GridContainer *p_g, const String &p_text) {
		Label *l = memnew(Label);
		l->set_text(T(p_text));
		l->set_tooltip_text(T(p_text));
		l->set_mouse_filter(Control::MOUSE_FILTER_PASS);
		l->add_theme_color_override("font_color", Color(0.72f, 0.72f, 0.72f));
		// (cut with "…" when the panel is narrow: it can be made as narrow as the other tabs; the tooltip has it all)
		l->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		l->set_custom_minimum_size(Vector2(48 * es, 0));
		l->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		p_g->add_child(l);
	};
	auto info = [&](GridContainer *p_g, const String &p_label, const String &p_value) {
		label(p_g, p_label);
		Label *l = memnew(Label);
		l->set_text(p_value);
		l->set_tooltip_text(p_value);
		l->set_mouse_filter(Control::MOUSE_FILTER_PASS);
		l->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		l->set_custom_minimum_size(Vector2(60 * es, 0));
		l->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		p_g->add_child(l);
	};
	// the defaults (the revert arrow): a new face's / edge's / vertex's values; none for positions
	static const Dictionary defs = []() {
		Dictionary d;
		d["vcolor"] = Color(1, 1, 1, 1);
		d["vcrease"] = 0.0;
		d["eseam"] = false;
		d["esharp"] = false;
		d["ecrease"] = 0.0;
		d["ebevel"] = 0.0;
		d["fmat"] = 0;
		d["fsmooth"] = 0.0;
		d["fstrength"] = 1;
		d["fmanual"] = false;
		d["fworld"] = false;
		d["ffill"] = 0;
		d["fscalex"] = 1.0;
		d["fscaley"] = 1.0;
		d["foffx"] = 0.0;
		d["foffy"] = 0.0;
		d["frot"] = 0.0;
		d["fflipu"] = false;
		d["fflipv"] = false;
		d["fswap"] = false;
		d["fgroup"] = 0.0;
		return d;
	}();
	auto place = [&](GridContainer *p_g, Control *p_field, const String &p_what) {
		p_g->add_child(defs.has(p_what) ? hw_editor::with_revert(p_field, defs[p_what]) : p_field);
	};
	auto num = [&](GridContainer *p_g, const String &p_label, double p_value, double p_min, double p_max, double p_step, bool p_int, const String &p_what) {
		label(p_g, p_label);
		HewnDragField *d = memnew(HewnDragField);
		d->set_name(String("Field_") + p_what);
		d->setup(p_min, p_max, p_step, p_int, p_value);
		d->set_custom_minimum_size(Vector2(56 * es, 0));
		d->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		d->connect("value_changed", callable_mp(this, &HewnEditorPlugin::_ei_set_float).bind(p_what));
		place(p_g, d, p_what);
	};
	auto check = [&](GridContainer *p_g, const String &p_label, bool p_on, const String &p_what) {
		label(p_g, p_label);
		CheckBox *c = memnew(CheckBox);
		c->set_name(String("Field_") + p_what);
		c->set_pressed(p_on);
		c->connect("toggled", callable_mp(this, &HewnEditorPlugin::_ei_set_bool).bind(p_what));
		place(p_g, c, p_what);
	};
	auto option = [&](GridContainer *p_g, const String &p_label, const PackedStringArray &p_items, int p_sel, const String &p_what) {
		label(p_g, p_label);
		OptionButton *o = memnew(OptionButton);
		o->set_name(String("Field_") + p_what);
		for (const String &it : p_items) {
			o->add_item(it);
		}
		o->select(CLAMP(p_sel, 0, MAX(0, o->get_item_count() - 1)));
		o->set_clip_text(true);
		o->set_fit_to_longest_item(false);
		o->set_custom_minimum_size(Vector2(56 * es, 0));
		o->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		o->set_tooltip_text(o->get_item_text(o->get_selected()));
		o->connect("item_selected", callable_mp(this, &HewnEditorPlugin::_ei_set_int).bind(p_what));
		place(p_g, o, p_what);
	};
	auto color = [&](GridContainer *p_g, const String &p_label, const Color &p_c, const String &p_what) {
		label(p_g, p_label);
		ColorPickerButton *c = memnew(ColorPickerButton);
		c->set_name(String("Field_") + p_what);
		c->set_pick_color(p_c);
		c->set_custom_minimum_size(Vector2(60, 0) * es);
		c->connect("color_changed", callable_mp(this, &HewnEditorPlugin::_ei_set_color).bind(p_what));
		place(p_g, c, p_what);
	};

	// ---- selection
	{
		GridContainer *sg = section("Selection");
		int tris = 0;
		for (const hewn::Face &F : m.faces) {
			tris += MAX(0, F.size() - 2);
		}
		hewn::Topology t;
		t.build(m);
		info(sg, "Object", String(n->get_name()));
		info(sg, "Selected", vformat(T("%d vertices, %d edges, %d faces"), (int)verts.size(), (int)edges.size(), (int)faces.size()));
		int used = 0;
		{
			std::vector<char> u(m.pos.size(), 0);
			for (const hewn::Face &F : m.faces) {
				for (int v : F.v) {
					u[v] = 1;
				}
			}
			for (char c : u) {
				used += c;
			}
		}
		info(sg, "Mesh", vformat(T("%d vertices, %d edges, %d faces, %d triangles"), used, (int)t.edges.size(), (int)m.faces.size(), tris));
		if (verts.empty() && edges.empty() && faces.empty()) {
			info(sg, "", T("Nothing selected"));
		}
	}

	// ---- vertices
	if (!verts.empty()) {
		GridContainer *vg = section("Vertices");
		Vector3 c, mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
		for (int v : verts) {
			c += m.pos[v];
			mn = mn.min(m.pos[v]);
			mx = mx.max(m.pos[v]);
		}
		c /= (float)verts.size();
		if (verts.size() == 1) {
			info(vg, "Index", itos(verts[0]));
		} else {
			info(vg, "Count", itos((int)verts.size()));
		}
		const bool one = verts.size() == 1;
		num(vg, String(one ? "Position X" : "Center X"), c.x, -1e6, 1e6, 0.001, false, String("vx"));
		num(vg, String(one ? "Position Y" : "Center Y"), c.y, -1e6, 1e6, 0.001, false, String("vy"));
		num(vg, String(one ? "Position Z" : "Center Z"), c.z, -1e6, 1e6, 0.001, false, String("vz"));
		info(vg, "World", v3(g.xform(c)));
		if (verts.size() > 1) {
			info(vg, "Size", v3(mx - mn));
		}
		// the color of their corners (the first one shown; changed: all of them)
		std::unordered_set<int> vs(verts.begin(), verts.end());
		Color first(1, 1, 1, 1);
		bool have = false, differ = false;
		int corners = 0;
		for (const hewn::Face &F : m.faces) {
			for (int i = 0; i < F.size(); i++) {
				if (!vs.count(F.v[i])) {
					continue;
				}
				const Color cc = i < (int)F.col.size() ? F.col[i] : Color(1, 1, 1, 1);
				corners++;
				if (!have) {
					first = cc;
					have = true;
				} else if (!cc.is_equal_approx(first)) {
					differ = true;
				}
			}
		}
		color(vg, "Color", first, "vcolor");
		info(vg, "Colors", differ ? vformat(T("differ (%d corners); the first: %s"), corners, col_text(first)) : vformat(T("%d corners: %s"), corners, col_text(first)));
		float crease = 0.0f;
		auto it = m.vert_crease.find(verts[0]);
		if (it != m.vert_crease.end()) {
			crease = it->second;
		}
		num(vg, "Vertex Crease", crease, 0, 1, 0.01, false, "vcrease");
		if (verts.size() == 1) {
			hewn::Topology t;
			t.build(m);
			info(vg, "Faces Around", index_list(t.vert_faces[verts[0]]));
		}
	}

	// ---- edges
	if (!edges.empty()) {
		GridContainer *eg = section("Edges");
		float len = 0.0f;
		for (const hewn::Edge &e : edges) {
			len += hewn::edge_length(m, e);
		}
		info(eg, edges.size() == 1 ? "Vertices" : "Count", edges.size() == 1 ? vformat("%d - %d", edges[0].a, edges[0].b) : itos((int)edges.size()));
		info(eg, edges.size() == 1 ? "Length" : "Total Length", vformat("%.4f", len));
		hewn::EdgeAttr a;
		auto it = m.edge_attrs.find(hewn::ekey(edges[0]));
		if (it != m.edge_attrs.end()) {
			a = it->second;
		}
		check(eg, "Seam", a.seam, "eseam");
		check(eg, "Sharp", a.sharp, "esharp");
		num(eg, "Crease", a.crease, 0, 1, 0.01, false, "ecrease");
		num(eg, "Bevel Weight", a.bevel_weight, 0, 1, 0.01, false, "ebevel");
		if (edges.size() == 1) {
			hewn::Topology t;
			t.build(m);
			const int id = t.find_edge(edges[0].a, edges[0].b);
			info(eg, "Faces", id >= 0 ? index_list(t.edge_faces[id]) : String("-"));
		}
	}

	// ---- faces
	if (!faces.empty()) {
		GridContainer *fg = section("Faces");
		const hewn::Face &F = m.faces[faces[0]];
		float area = 0.0f;
		for (int f : faces) {
			area += hewn::face_normal_raw(m, m.faces[f]).length() * 0.5f;
		}
		if (faces.size() == 1) {
			info(fg, "Index", itos(faces[0]));
			info(fg, "Corners", vformat("%d: %s", F.size(), index_list(F.v)));
		} else {
			info(fg, "Count", vformat(T("%d (the values of the first)"), (int)faces.size()));
		}
		info(fg, faces.size() == 1 ? "Area" : "Total Area", vformat("%.4f", area));
		info(fg, "Normal", v3(hewn::face_normal(m, F)));
		info(fg, "Center", v3(hewn::face_center(m, F)) + "  (" + T("world") + ": " + v3(g.xform(hewn::face_center(m, F))) + ")");
		// the material: the slot (from the object's list), or another one picked (added as a slot)
		const TypedArray<Material> mats = n->get_materials();
		PackedStringArray slots;
		const int count = MAX((int)mats.size(), F.material + 1);
		for (int i = 0; i < count; i++) {
			Ref<Material> mt = i < mats.size() ? Ref<Material>(mats[i]) : Ref<Material>();
			const String nm = mt.is_null() ? T("(default)") : !mt->get_path().is_empty() && !mt->get_path().contains("::") ? mt->get_path().get_file() : mt->get_class();
			slots.push_back(vformat("%d: %s", i, nm));
		}
		option(fg, "Material", slots, F.material, "fmat");
		label(fg, "Other Material");
		EditorResourcePicker *rp = memnew(EditorResourcePicker);
		rp->set_name("Field_fmat_res");
		rp->set_base_type("Material");
		rp->set_edited_resource(n->get_material_for_slot(F.material));
		rp->set_custom_minimum_size(Vector2(56 * es, 0));
		rp->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		rp->connect("resource_changed", callable_mp(this, &HewnEditorPlugin::_ei_set_material));
		rp->connect("resource_selected", callable_mp(this, &HewnEditorPlugin::_ei_open_material));
		fg->add_child(rp);
		num(fg, "Smoothing Group", F.smooth, 0, 1e6, 1, true, "fsmooth");
		option(fg, "Face Strength", PackedStringArray({ T("Weak"), T("Medium"), T("Strong") }), F.strength, "fstrength");
		info(fg, "Role", F.role >= 0 && F.role < (int)(sizeof(ROLE_NAMES) / sizeof(ROLE_NAMES[0])) ? T(ROLE_NAMES[F.role]) : itos(F.role));
		info(fg, "Hidden", F.hidden ? T("yes") : T("no"));

		GridContainer *ug = section("Texture (UV)");
		check(ug, "Manual UVs", F.manual_uv, "fmanual");
		if (!F.manual_uv) {
			check(ug, "World UV", F.uvp.world, "fworld");
			option(ug, "Fill", PackedStringArray({ T("Tile"), T("Fit"), T("Stretch") }), F.uvp.fill, "ffill");
			num(ug, "Scale X", F.uvp.scale.x, -1e4, 1e4, 0.001, false, "fscalex");
			num(ug, "Scale Y", F.uvp.scale.y, -1e4, 1e4, 0.001, false, "fscaley");
			num(ug, "Offset X", F.uvp.offset.x, -1e4, 1e4, 0.001, false, "foffx");
			num(ug, "Offset Y", F.uvp.offset.y, -1e4, 1e4, 0.001, false, "foffy");
			num(ug, "Rotation", F.uvp.rotation, -360, 360, 0.1, false, "frot");
			check(ug, "Flip U", F.uvp.flip_u, "fflipu");
			check(ug, "Flip V", F.uvp.flip_v, "fflipv");
			check(ug, "Swap U / V", F.uvp.swap, "fswap");
			num(ug, "Texture Group", F.uv_group, 0, 1e6, 1, true, "fgroup");
		}
		std::vector<std::vector<Vector2>> uvs;
		hewn::compute_uvs(m, g, uvs);
		const std::vector<Vector2> &fu = uvs[faces[0]];
		String ut;
		for (size_t i = 0; i < fu.size() && i < 12; i++) {
			ut += (i ? "\n" : "") + vformat("%d: %.4f, %.4f", F.v[i], fu[i].x, fu[i].y);
		}
		info(ug, "Corner UVs", ut);
		// the face's corner colors and CUSTOM channels
		GridContainer *cg = section("Corner Colors");
		for (int i = 0; i < F.size() && i < 12; i++) {
			String line = col_text(i < (int)F.col.size() ? F.col[i] : Color(1, 1, 1, 1));
			if (F.cust.size() >= (size_t)(i + 1) * 4) {
				for (int k = 0; k < 4; k++) {
					line += "\nCUSTOM" + itos(k) + ": " + col_text(F.cust[i * 4 + k]);
				}
			}
			info(cg, vformat(T("Corner %d (vertex %d)"), i, F.v[i]), line);
		}
	}

	// ---- technical
	{
		GridContainer *tg = section("Mesh Data");
		info(tg, "Revision", itos((int64_t)n->get_revision()));
		info(tg, "Shape", String(hewn::shapes::type_name((hewn::shapes::Type)n->get_shape())));
		info(tg, "Materials", itos((int)n->get_materials().size()));
		int manual = 0, world = 0, hidden = 0;
		for (const hewn::Face &F : m.faces) {
			manual += F.manual_uv ? 1 : 0;
			world += F.uvp.world && !F.manual_uv ? 1 : 0;
			hidden += F.hidden ? 1 : 0;
		}
		info(tg, "Faces with Manual UVs", itos(manual));
		info(tg, "Faces with World UVs", itos(world));
		info(tg, "Hidden Faces", itos(hidden));
		info(tg, "Marked Edges", itos((int)m.edge_attrs.size()));
		info(tg, "Loose Edges", itos((int)m.wires.size()));
		info(tg, "Vertex Positions (all)", itos((int)m.pos.size()));
		String cf;
		for (int k = 0; k < 4; k++) {
			cf += (k ? ", " : "") + (n->custom_formats[k] < 0 ? String("-") : itos(n->custom_formats[k]));
		}
		info(tg, "CUSTOM Formats", cf);
		info(tg, "Modifiers", n->has_modifiers() ? T("yes") : T("no"));
		if (!verts.empty()) {
			info(tg, "Selected Vertices", index_list(verts));
		}
		if (!faces.empty()) {
			info(tg, "Selected Faces", index_list(faces));
		}
		info(tg, "Transform", v3(g.origin) + "  |  " + T("scale") + " " + v3(g.basis.get_scale()));
	}
}

void HewnEditorPlugin::_ei_section_toggled(bool p_on, const String &p_name) {
	ei_open[p_name] = p_on;
	callable_mp(this, &HewnEditorPlugin::_ei_update).call_deferred(true); // (not while its button is in the middle of its signal)
}

// A change made in the window: one undo step, the window keeps its fields (no rebuild under the mouse).
void HewnEditorPlugin::_ei_apply(const String &p_what, const Variant &p_value) {
	HewnMesh3D *n = get_edited();
	if (!n || mode == MODE_OBJECT) {
		return;
	}
	begin_edit("Element Inspector");
	hewn::Mesh &m = n->get_data();
	hewn::Selection &s = n->selection;
	bool geometry = false;
	const std::vector<int> verts = _selected_verts();
	std::vector<int> faces;
	for (int f : s.faces) {
		if (f >= 0 && f < (int)m.faces.size()) {
			faces.push_back(f);
		}
	}
	if (p_what == "vx" || p_what == "vy" || p_what == "vz") {
		const int a = p_what == "vx" ? 0 : p_what == "vy" ? 1 : 2;
		float c = 0.0f;
		for (int v : verts) {
			c += m.pos[v][a];
		}
		c /= MAX(1, (int)verts.size());
		const float d = (float)(double)p_value - c; // (several: all of them move, their center to the value)
		for (int v : verts) {
			m.pos[v][a] += d;
		}
		geometry = true;
	} else if (p_what == "vcolor") {
		std::unordered_set<int> vs(verts.begin(), verts.end());
		for (hewn::Face &F : m.faces) {
			for (int i = 0; i < F.size(); i++) {
				if (vs.count(F.v[i])) {
					F.ensure_attrs();
					F.col[i] = p_value;
				}
			}
		}
	} else if (p_what == "vcrease") {
		for (int v : verts) {
			if ((float)(double)p_value <= 0.0f) {
				m.vert_crease.erase(v);
			} else {
				m.vert_crease[v] = (float)(double)p_value;
			}
		}
	} else if (p_what == "eseam" || p_what == "esharp" || p_what == "ecrease" || p_what == "ebevel") {
		for (const hewn::Edge &e : s.edges) {
			hewn::EdgeAttr &a = m.edge_attrs[hewn::ekey(e)];
			if (p_what == "eseam") {
				a.seam = p_value;
			} else if (p_what == "esharp") {
				a.sharp = p_value;
			} else if (p_what == "ecrease") {
				a.crease = (float)(double)p_value;
			} else {
				a.bevel_weight = (float)(double)p_value;
			}
			if (a.empty()) {
				m.edge_attrs.erase(hewn::ekey(e));
			}
		}
	} else {
		for (int f : faces) {
			hewn::Face &F = m.faces[f];
			if (p_what == "fmat") {
				F.material = (int)p_value;
			} else if (p_what == "fsmooth") {
				F.smooth = (int)(double)p_value;
			} else if (p_what == "fstrength") {
				F.strength = (int)p_value;
			} else if (p_what == "fmanual") {
				if ((bool)p_value && !F.manual_uv) {
					// (its UVs as they show now become its own)
					std::vector<std::vector<Vector2>> uvs;
					hewn::compute_uvs(m, n->get_global_transform(), uvs);
					F.uv = uvs[f];
				}
				F.manual_uv = p_value;
			} else if (p_what == "fworld") {
				F.uvp.world = p_value;
			} else if (p_what == "ffill") {
				F.uvp.fill = (int)p_value;
			} else if (p_what == "fscalex") {
				F.uvp.scale.x = (float)(double)p_value;
			} else if (p_what == "fscaley") {
				F.uvp.scale.y = (float)(double)p_value;
			} else if (p_what == "foffx") {
				F.uvp.offset.x = (float)(double)p_value;
			} else if (p_what == "foffy") {
				F.uvp.offset.y = (float)(double)p_value;
			} else if (p_what == "frot") {
				F.uvp.rotation = (float)(double)p_value;
			} else if (p_what == "fflipu") {
				F.uvp.flip_u = p_value;
			} else if (p_what == "fflipv") {
				F.uvp.flip_v = p_value;
			} else if (p_what == "fswap") {
				F.uvp.swap = p_value;
			} else if (p_what == "fgroup") {
				F.uv_group = (int)(double)p_value;
			}
		}
	}
	n->commit(geometry);
	end_edit();
	ei_hash = _ei_state_hash();
	if (p_what == "fmanual" || p_what == "fmat") {
		callable_mp(this, &HewnEditorPlugin::_ei_update).call_deferred(true); // (fields shown by it)
	}
}

void HewnEditorPlugin::_ei_set_float(double p_value, const String &p_what) {
	_ei_apply(p_what, p_value);
}

void HewnEditorPlugin::_ei_set_bool(bool p_on, const String &p_what) {
	_ei_apply(p_what, p_on);
}

void HewnEditorPlugin::_ei_set_int(int p_index, const String &p_what) {
	_ei_apply(p_what, p_index);
}

void HewnEditorPlugin::_ei_set_color(const Color &p_color, const String &p_what) {
	_ei_apply(p_what, p_color);
}

// A click on the material: it opens in Godot's inspector, and that dock's tab is shown.
void HewnEditorPlugin::_ei_open_material(const Ref<Resource> &p_res, bool p_inspect) {
	(void)p_inspect;
	if (p_res.is_null()) {
		return;
	}
	EditorInterface::get_singleton()->edit_resource(p_res);
	Control *insp = EditorInterface::get_singleton()->get_inspector();
	// (the tab of the dock holding the inspector)
	for (Node *c = insp; c && c->get_parent(); c = c->get_parent()) {
		if (TabContainer *tc = Object::cast_to<TabContainer>(c->get_parent())) {
			Control *page = Object::cast_to<Control>(c);
			if (page && page->get_index() >= 0) {
				tc->set_current_tab(page->get_index());
			}
			break;
		}
	}
}

// Another material on the selected faces (a new slot when the object does not have it yet).
void HewnEditorPlugin::_ei_set_material(const Ref<Resource> &p_res) {
	HewnMesh3D *n = get_edited();
	Ref<Material> mat = p_res;
	if (!n || mat.is_null() || n->selection.faces.empty()) {
		return;
	}
	PackedInt32Array fs;
	for (int f : n->selection.faces) {
		fs.push_back(f);
	}
	begin_edit("Element Inspector: Material");
	n->apply_material(mat, fs);
	end_edit();
	callable_mp(this, &HewnEditorPlugin::_ei_update).call_deferred(true);
}
