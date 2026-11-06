// Check Scene: problems of the scene's meshes and objects in a list; each can be
// selected (the faces / vertices too) and the safe ones fixed, one undo step each.
#include "hw_editor.h"

#include "../hw_entities.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/tree_item.hpp>

#include <unordered_map>

using namespace godot;
using namespace hw_editor;

namespace {

enum Fix {
	FIX_NONE,
	FIX_DELETE_FACES, // degenerate faces
	FIX_WELD, // vertices in one place
	FIX_UNUSED, // vertices of no face
	FIX_TRIANGULATE, // faces that are not flat
	FIX_CONFORM, // neighbors facing opposite ways
	FIX_DELETE_NODE, // a mesh with nothing in it
};

float face_area(const hewn::Mesh &m, const hewn::Face &F) {
	std::vector<Vector3> pts;
	for (int v : F.v) {
		pts.push_back(m.pos[v]);
	}
	return hewn::newell(pts).length() * 0.5f;
}

} // namespace

void HewnEditorPlugin::open_issues() {
	if (!issues_dialog) {
		const float es = EditorInterface::get_singleton()->get_editor_scale();
		issues_dialog = memnew(AcceptDialog);
		issues_dialog->set_title("Check Scene");
		issues_dialog->set_ok_button_text("Close");
		VBoxContainer *v = memnew(VBoxContainer);
		issues_dialog->add_child(v);
		issues_summary = memnew(Label);
		v->add_child(issues_summary);
		issues_tree = memnew(Tree);
		issues_tree->set_hide_root(true);
		issues_tree->set_columns(2);
		issues_tree->set_column_expand(1, false);
		issues_tree->set_custom_minimum_size(Vector2(520 * es, 320 * es));
		issues_tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		issues_tree->connect("button_clicked", callable_mp(this, &HewnEditorPlugin::_on_issue_button));
		issues_tree->connect("item_activated", callable_mp(this, &HewnEditorPlugin::_on_issue_activated));
		v->add_child(issues_tree);
		HBoxContainer *h = memnew(HBoxContainer);
		v->add_child(h);
		Button *again = memnew(Button);
		again->set_text("Check Again");
		again->connect("pressed", callable_mp(this, &HewnEditorPlugin::_issues_refresh));
		h->add_child(again);
		Button *all = memnew(Button);
		all->set_text("Fix All");
		all->set_tooltip_text("Every problem that can be fixed safely (each its own undo step)");
		all->connect("pressed", callable_mp(this, &HewnEditorPlugin::_issues_fix_all));
		h->add_child(all);
		localize(issues_dialog);
		EditorInterface::get_singleton()->get_base_control()->add_child(issues_dialog);
	}
	_issues_refresh();
	issues_dialog->popup_centered();
}

void HewnEditorPlugin::_issues_scan() {
	issues.clear();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return;
	}
	std::vector<Node *> stack = { root };
	std::vector<Node *> all;
	while (!stack.empty()) {
		Node *x = stack.back();
		stack.pop_back();
		all.push_back(x);
		for (int i = 0; i < x->get_child_count(); i++) {
			stack.push_back(x->get_child(i));
		}
	}
	auto add = [&](Node *p_node, const String &p_text, int p_fix) -> Issue & {
		Issue is;
		is.node = ObjectID(p_node->get_instance_id());
		is.text = p_text;
		is.fix = p_fix;
		issues.push_back(is);
		return issues.back();
	};
	for (Node *x : all) {
		if (Node3D *n3 = Object::cast_to<Node3D>(x)) {
			const float det = n3->get_global_transform().basis.determinant();
			if (std::fabs(det) < 1e-8f) {
				add(x, T("Scale 0: it is flat or gone (and its collision broken)"), FIX_NONE);
			}
		}
		if (HewnEntity3D *en = Object::cast_to<HewnEntity3D>(x)) {
			if (en->get_classname().strip_edges().is_empty()) {
				add(x, T("Entity without a class name (the game cannot find it)"), FIX_NONE);
			}
		}
		HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(x);
		if (!pm) {
			continue;
		}
		const hewn::Mesh &m = pm->get_data();
		if (m.faces.empty() && m.wires.empty()) {
			add(x, T("Empty mesh: no faces"), FIX_DELETE_NODE);
			continue;
		}
		// degenerate faces: fewer than three corners in different places, or no area
		{
			std::vector<int> bad;
			for (int f = 0; f < (int)m.faces.size(); f++) {
				const hewn::Face &F = m.faces[f];
				std::vector<int> u = F.v;
				std::sort(u.begin(), u.end());
				u.erase(std::unique(u.begin(), u.end()), u.end());
				if (u.size() < 3 || face_area(m, F) < 1e-9f) {
					bad.push_back(f);
				}
			}
			if (!bad.empty()) {
				add(x, vformat(T("%d faces without area (a line or a point)"), (int)bad.size()), FIX_DELETE_FACES).faces = bad;
			}
		}
		std::vector<char> used(m.pos.size(), 0);
		for (const hewn::Face &F : m.faces) {
			for (int v : F.v) {
				used[v] = 1;
			}
		}
		for (const std::pair<int, int> &w : m.wires) {
			used[w.first] = used[w.second] = 1;
		}
		// vertices in one place (not joined: a crack the light leaks through)
		{
			std::unordered_map<uint64_t, int> first;
			std::vector<int> dup;
			for (int v = 0; v < (int)m.pos.size(); v++) {
				if (!used[v]) {
					continue;
				}
				const Vector3 q = (m.pos[v] / 1e-4f).round();
				const uint64_t k = ((uint64_t)((int64_t)q.x & 0x1fffff) << 42) | ((uint64_t)((int64_t)q.y & 0x1fffff) << 21) | (uint64_t)((int64_t)q.z & 0x1fffff);
				auto it = first.find(k);
				if (it == first.end()) {
					first[k] = v;
				} else {
					dup.push_back(v);
					dup.push_back(it->second);
				}
			}
			if (!dup.empty()) {
				std::sort(dup.begin(), dup.end());
				dup.erase(std::unique(dup.begin(), dup.end()), dup.end());
				add(x, vformat(T("%d vertices in the same place, not joined"), (int)dup.size()), FIX_WELD).verts = dup;
			}
		}
		// vertices of no face
		{
			int n = 0;
			for (int v = 0; v < (int)m.pos.size(); v++) {
				n += used[v] ? 0 : 1;
			}
			if (n > 0) {
				add(x, vformat(T("%d vertices of no face"), n), FIX_UNUSED);
			}
		}
		hewn::Topology t;
		t.build(m);
		// edges of more than two faces; neighbors facing opposite ways (an edge walked the same way by both)
		{
			std::vector<hewn::Edge> many;
			std::vector<int> flipped;
			for (size_t e = 0; e < t.edges.size(); e++) {
				const std::vector<int> &fs = t.edge_faces[e];
				if (fs.size() > 2) {
					many.push_back(t.edges[e]);
				} else if (fs.size() == 2) {
					const int a = t.edges[e].a, b = t.edges[e].b;
					if ((t.face_of_half(a, b) < 0) || (t.face_of_half(b, a) < 0)) {
						flipped.push_back(fs[0]);
						flipped.push_back(fs[1]);
					}
				}
			}
			if (!many.empty()) {
				add(x, vformat(T("%d edges shared by more than two faces"), (int)many.size()), FIX_NONE).edges = many;
			}
			if (!flipped.empty()) {
				std::sort(flipped.begin(), flipped.end());
				flipped.erase(std::unique(flipped.begin(), flipped.end()), flipped.end());
				add(x, vformat(T("%d faces facing the other way than their neighbors"), (int)flipped.size()), FIX_CONFORM).faces = flipped;
			}
		}
		// faces that are not flat (their triangles fold: shading and collision differ from what is seen)
		{
			std::vector<int> bent;
			for (int f = 0; f < (int)m.faces.size(); f++) {
				const hewn::Face &F = m.faces[f];
				if (F.size() <= 3) {
					continue;
				}
				const Vector3 nrm = hewn::face_normal(m, F);
				const Vector3 c = hewn::face_center(m, F);
				float size = 0.0f, off = 0.0f;
				for (int v : F.v) {
					size = MAX(size, m.pos[v].distance_to(c));
					off = MAX(off, std::fabs((m.pos[v] - c).dot(nrm)));
				}
				if (off > MAX(1e-3f, size * 0.01f)) {
					bent.push_back(f);
				}
			}
			if (!bent.empty()) {
				add(x, vformat(T("%d faces that are not flat"), (int)bent.size()), FIX_TRIANGULATE).faces = bent;
			}
		}
	}
}

void HewnEditorPlugin::_issues_refresh() {
	_issues_scan();
	if (!issues_tree) {
		return;
	}
	issues_tree->clear();
	TreeItem *root = issues_tree->create_item();
	Ref<Theme> et = EditorInterface::get_singleton()->get_editor_theme();
	Ref<Texture2D> warn = et->get_icon("NodeWarning", "EditorIcons");
	Ref<Texture2D> sel = et->get_icon("ToolSelect", "EditorIcons");
	Ref<Texture2D> fix = et->get_icon("Tools", "EditorIcons");
	for (size_t i = 0; i < issues.size(); i++) {
		const Issue &is = issues[i];
		Node *node = Object::cast_to<Node>(ObjectDB::get_instance(is.node));
		TreeItem *it = issues_tree->create_item(root);
		it->set_text(0, (node ? String(node->get_name()) + ": " : String()) + is.text);
		it->set_icon(0, warn);
		it->set_metadata(0, (int)i);
		it->set_auto_translate_mode(0, Node::AUTO_TRANSLATE_MODE_DISABLED);
		it->add_button(1, sel, 0, false, T("Select it"));
		if (is.fix != FIX_NONE) {
			it->add_button(1, fix, 1, false, T("Fix it"));
		}
	}
	issues_summary->set_text(issues.empty() ? String(T("No problems found.")) : vformat(T("Problems: %d (double click or the arrow: select, the wrench: fix)"), (int)issues.size()));
}

void HewnEditorPlugin::_on_issue_activated() {
	TreeItem *it = issues_tree ? issues_tree->get_selected() : nullptr;
	if (it) {
		_issue_select((int)it->get_metadata(0));
	}
}

void HewnEditorPlugin::_on_issue_button(Object *p_item, int p_column, int p_id, int p_mouse) {
	TreeItem *it = Object::cast_to<TreeItem>(p_item);
	if (!it) {
		return;
	}
	const int i = it->get_metadata(0);
	if (p_id == 0) {
		_issue_select(i);
	} else {
		_issue_fix(i);
		_issues_refresh();
	}
}

// The object selected; a mesh's faces / edges / vertices also, in edit mode (once the mesh is the edited one).
void HewnEditorPlugin::_issue_select(int p_index) {
	if (p_index < 0 || p_index >= (int)issues.size()) {
		return;
	}
	Node *node = Object::cast_to<Node>(ObjectDB::get_instance(issues[p_index].node));
	if (!node) {
		return;
	}
	EditorSelection *es = EditorInterface::get_singleton()->get_selection();
	HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(node);
	const Issue &is = issues[p_index];
	if (pm && get_edited() == pm && mode != MODE_OBJECT) {
		_issue_apply_selection(p_index);
		return;
	}
	es->clear();
	es->add_node(node);
	if (pm && (!is.faces.empty() || !is.edges.empty() || !is.verts.empty())) {
		issue_pending = p_index; // (applied in _process once the mesh is edited)
		issue_pending_frames = 30;
	}
}

void HewnEditorPlugin::_issue_apply_selection(int p_index) {
	if (p_index < 0 || p_index >= (int)issues.size()) {
		return;
	}
	HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(issues[p_index].node));
	if (!pm || get_edited() != pm) {
		return;
	}
	const Issue &is = issues[p_index];
	set_mode(!is.faces.empty() ? MODE_FACE : !is.edges.empty() ? MODE_EDGE : MODE_VERTEX);
	pm->selection.clear();
	pm->selection.faces = is.faces;
	pm->selection.edges = is.edges;
	pm->selection.verts = is.verts;
	pm->update_gizmos();
	selection_changed();
	update_overlays();
}

void HewnEditorPlugin::_issue_fix(int p_index) {
	if (p_index < 0 || p_index >= (int)issues.size()) {
		return;
	}
	const Issue &is = issues[p_index];
	Node *node = Object::cast_to<Node>(ObjectDB::get_instance(is.node));
	if (!node || is.fix == FIX_NONE) {
		return;
	}
	EditorUndoRedoManager *ur = get_undo_redo();
	if (is.fix == FIX_DELETE_NODE) {
		Node *parent = node->get_parent();
		if (!parent) {
			return;
		}
		ur->create_action("Hewn: Delete Empty Mesh");
		ur->add_do_method(parent, "remove_child", node);
		ur->add_undo_method(parent, "add_child", node, true);
		ur->add_undo_method(parent, "move_child", node, node->get_index());
		ur->add_undo_method(node, "set_owner", node->get_owner());
		ur->add_undo_reference(node);
		ur->commit_action();
		return;
	}
	HewnMesh3D *pm = Object::cast_to<HewnMesh3D>(node);
	if (!pm) {
		return;
	}
	const Dictionary before = pm->get_state();
	hewn::Mesh &m = pm->get_data();
	static const char *names[7] = { "", "Delete Faces Without Area", "Join Vertices", "Remove Loose Vertices", "Triangulate Bent Faces", "Make Normals Consistent", "" };
	switch (is.fix) {
		case FIX_DELETE_FACES:
			hewn::ops::delete_faces(m, is.faces);
			hewn::remove_unused_vertices(m);
			break;
		case FIX_WELD:
			hewn::weld_by_distance(m, 1e-4f);
			break;
		case FIX_UNUSED:
			hewn::remove_unused_vertices(m);
			break;
		case FIX_TRIANGULATE:
			hewn::ops::triangulate_faces(m, is.faces);
			break;
		case FIX_CONFORM: {
			std::vector<int> fs;
			for (int f = 0; f < (int)m.faces.size(); f++) {
				fs.push_back(f);
			}
			hewn::ops::conform_normals(m, fs);
			break;
		}
		default:
			break;
	}
	pm->selection.clear();
	pm->commit(true);
	ur->create_action(String("Hewn: ") + names[is.fix], UndoRedo::MERGE_DISABLE, pm);
	ur->add_do_method(pm, "set_state", pm->get_state());
	ur->add_undo_method(pm, "set_state", before);
	ur->commit_action(false);
}

void HewnEditorPlugin::_issues_fix_all() {
	// (from the last: deleting a node keeps the indices of the ones before; a mesh fixed is checked again)
	for (int i = (int)issues.size() - 1; i >= 0; i--) {
		if (issues[i].fix != 0) {
			_issue_fix(i);
		}
	}
	_issues_refresh();
}
