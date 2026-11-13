// "Quad" main screen: Top / Front / Side 2D views with a grid and a 3D camera view (classic level editor
// layout). Tools: Select (move, resize and rotate on the grid; Shift + drag clones), Block (draw a box, Enter
// creates a Hewn shape), Vertex (drag the vertices of Hewn meshes) and Clip (cut meshes with a line).
#include "hw_quad.h"

#include "hw_editor.h"
#include "hw_editor_util.h"

#include <godot_cpp/classes/button_group.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/v_separator.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/classes/world3d.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

using namespace godot;
using hw_editor::T;

namespace {

const uint32_t SCENE_LAYERS = (1u << 20) - 1;
// 3D view: also Godot's grid (layer 25; not the gizmos of the hidden 3D screen, they don't update)
const uint32_t GIZMO_LAYERS_3D = 1u << 25;
const float GAP = 4.0f;
const Color BG_COLOR(0.08f, 0.08f, 0.09f);
const Color SEL_COLOR(1.0f, 0.28f, 0.22f);
const Color BLOCK_COLOR(1.0f, 0.85f, 0.3f);
const Color BOUNDS_COLOR(1.0f, 1.0f, 1.0f, 0.9f);
const Color CAMERA_COLOR(0.3f, 0.85f, 1.0f);
const float GRID_SIZES[] = { 0.015625f, 0.03125f, 0.0625f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f };
const int GRID_COUNT = sizeof(GRID_SIZES) / sizeof(GRID_SIZES[0]);
const int BLOCK_SHAPES[] = { hewn::shapes::CUBE, hewn::shapes::WEDGE, hewn::shapes::PRISM, hewn::shapes::STAIRS, hewn::shapes::CURVED_STAIRS,
	hewn::shapes::CYLINDER, hewn::shapes::CONE, hewn::shapes::PIPE, hewn::shapes::SPHERE, hewn::shapes::ICOSPHERE, hewn::shapes::CAPSULE,
	hewn::shapes::TORUS, hewn::shapes::ARCH, hewn::shapes::DOOR, hewn::shapes::PLANE, hewn::shapes::DISC };
// handles around a box on screen: corners and side middles, clockwise from the top left
const int HANDLE_X[8] = { -1, 0, 1, 1, 1, 0, -1, -1 };
const int HANDLE_Y[8] = { -1, -1, -1, 0, 1, 1, 1, 0 };

float edscale() {
	return EditorInterface::get_singleton()->get_editor_scale();
}

AABB xform_box(const Transform3D &p_xf, const AABB &p_box) {
	AABB out(p_xf.xform(p_box.get_endpoint(0)), Vector3());
	for (int i = 1; i < 8; i++) {
		out.expand_to(p_xf.xform(p_box.get_endpoint(i)));
	}
	return out;
}

int axis_index(const Vector3 &p_axis) {
	const Vector3 a = p_axis.abs();
	return a.x >= a.y && a.x >= a.z ? 0 : (a.y >= a.z ? 1 : 2);
}

bool near_eq(float a, float b) {
	return std::fabs(a - b) <= 1e-4f * MAX(1.0f, MAX(std::fabs(a), std::fabs(b)));
}

bool has_param(int p_shape, const char *p_name) {
	for (const hewn::shapes::Param &p : hewn::shapes::params((hewn::shapes::Type)p_shape)) {
		if (String(p.name) == p_name) {
			return true;
		}
	}
	return false;
}

// Resizes a parametric shape by factors along its local axes (keeps it parametric). False when the shape
// can't take these factors through its parameters.
bool scale_params(HewnMesh3D *p_mesh, const Vector3 &k) {
	const int t = p_mesh->get_shape();
	if (t == hewn::shapes::CUSTOM || t == hewn::shapes::POLY_SHAPE || t == hewn::shapes::WALL || t == hewn::shapes::SWEEP || t == hewn::shapes::PATH_STAIRS) {
		return false;
	}
	if (t == hewn::shapes::STAIRS && std::fabs(p_mesh->get_shape_param("curvature")) > 1e-4f) {
		return false;
	}
	Dictionary d = p_mesh->get_shape_params();
	auto mul = [&](const char *p_name, float p_factor) {
		if (has_param(t, p_name)) {
			d[p_name] = p_mesh->get_shape_param(p_name) * p_factor;
		}
	};
	if (has_param(t, "radius")) {
		// round shapes: the same factor across the circle
		if (!near_eq(k.x, k.z)) {
			return false;
		}
		const bool tall = t == hewn::shapes::CYLINDER || t == hewn::shapes::CONE || t == hewn::shapes::PIPE || t == hewn::shapes::POLYGON || t == hewn::shapes::DISC;
		if (!tall && !near_eq(k.x, k.y)) {
			return false;
		}
		mul("radius", k.x);
		mul("tube_radius", k.x);
		mul("thickness", k.x);
		mul("inner_radius", k.x);
		mul("height", k.y);
		mul("depth", k.z);
	} else {
		mul("width", k.x);
		mul("height", k.y);
		mul("depth", k.z);
		mul("inner_radius", k.x);
		mul("opening_width", k.x);
		mul("opening_height", k.y);
		mul("arch_height", k.y);
	}
	p_mesh->set_shape_params(d);
	return true;
}

// Ownership like the original for a duplicate made with Node::duplicate().
void copy_owners(Node *p_orig, Node *p_dup, Node *p_root) {
	if (p_orig->get_owner() == p_root) {
		p_dup->set_owner(p_root);
	}
	const int n = MIN(p_orig->get_child_count(), p_dup->get_child_count());
	for (int i = 0; i < n; i++) {
		copy_owners(p_orig->get_child(i), p_dup->get_child(i), p_root);
	}
}

Node3D *node_of(uint64_t p_id) {
	return Object::cast_to<Node3D>(ObjectDB::get_instance(p_id));
}

bool text_field_focused(Control *p_any) {
	Viewport *vp = p_any->get_viewport();
	Control *f = vp ? vp->gui_get_focus_owner() : nullptr;
	return f && (Object::cast_to<LineEdit>(f) || Object::cast_to<TextEdit>(f));
}

} // namespace

// ================================================================== pane

void HewnQuadPane::_bind_methods() {
	ClassDB::bind_method(D_METHOD("world_to_screen", "point"), &HewnQuadPane::to_screen); // (tests)
}

void HewnQuadPane::setup(Kind p_kind, HewnQuadView *p_view) {
	kind = p_kind;
	view = p_view;
	set_clip_contents(true);
	set_mouse_filter(MOUSE_FILTER_IGNORE);
	switch (kind) {
		case VIEW_TOP:
			u = Vector3(1, 0, 0);
			v = Vector3(0, 0, -1);
			break;
		case VIEW_SIDE:
			u = Vector3(0, 0, -1);
			v = Vector3(0, 1, 0);
			break;
		default:
			u = Vector3(1, 0, 0);
			v = Vector3(0, 1, 0);
			break;
	}
	w = u.cross(v); // towards the viewer

	container = memnew(SubViewportContainer);
	container->set_stretch(true);
	container->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	container->set_mouse_filter(MOUSE_FILTER_IGNORE);
	add_child(container);
	viewport = memnew(SubViewport);
	container->add_child(viewport);
	camera = memnew(Camera3D);
	viewport->add_child(camera);
	if (is_2d()) {
		// the scene as a wireframe over our grid
		viewport->set_transparent_background(true);
		viewport->set_debug_draw(Viewport::DEBUG_DRAW_WIREFRAME);
		camera->set_projection(Camera3D::PROJECTION_ORTHOGONAL);
		camera->set_cull_mask(SCENE_LAYERS);
		camera->set_environment(view->get_env_2d());
	} else {
		camera->set_cull_mask(SCENE_LAYERS | GIZMO_LAYERS_3D);
		camera->set_fov(70.0f);
		camera->set_near(0.05f);
		camera->set_far(4000.0f);
	}
	camera->set_current(true);

	overlay = memnew(Control);
	overlay->set_name("Overlay");
	overlay->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	overlay->set_focus_mode(FOCUS_CLICK);
	overlay->set_mouse_filter(MOUSE_FILTER_STOP);
	add_child(overlay);
}

void HewnQuadPane::_notification(int p_what) {
	if (p_what == NOTIFICATION_DRAW) {
		if (is_2d() && view) {
			view->draw_grid(this);
		}
	} else if (p_what == NOTIFICATION_RESIZED) {
		sync_camera();
	}
}

Vector2 HewnQuadPane::to_screen(const Vector3 &p) const {
	return get_size() * 0.5f + Vector2((p.dot(u) - center.x) * zoom, -(p.dot(v) - center.y) * zoom);
}

Vector2 HewnQuadPane::to_uv(const Vector2 &p_screen) const {
	const Vector2 h = get_size() * 0.5f;
	return Vector2(center.x + (p_screen.x - h.x) / zoom, center.y - (p_screen.y - h.y) / zoom);
}

String HewnQuadPane::title() const {
	switch (kind) {
		case VIEW_TOP:
			return T("Top (X/Z)");
		case VIEW_FRONT:
			return T("Front (X/Y)");
		case VIEW_SIDE:
			return T("Side (Z/Y)");
		default:
			return T("3D Camera");
	}
}

void HewnQuadPane::sync_camera() {
	if (!camera || !is_2d()) {
		return;
	}
	const float h = MAX(1.0f, get_size().y);
	camera->set_orthogonal(h / zoom, 0.05f, 8000.0f);
	Basis b;
	b.set_column(0, u);
	b.set_column(1, v);
	b.set_column(2, w);
	camera->set_transform(Transform3D(b, u * center.x + v * center.y + w * 4000.0f));
}

void HewnQuadPane::zoom_at(const Vector2 &p_screen, float p_factor) {
	const Vector2 before = to_uv(p_screen);
	zoom = CLAMP(zoom * p_factor, 0.5f, 20000.0f);
	center += before - to_uv(p_screen);
	sync_camera();
}

// ================================================================== view: setup

void HewnQuadView::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_set_owners", "pairs"), &HewnQuadView::_set_owners);
}

void HewnQuadView::setup(HewnEditorPlugin *p_plugin) {
	plugin = p_plugin;
	set_name("HewnQuad");
	set_v_size_flags(SIZE_EXPAND_FILL);
	set_h_size_flags(SIZE_EXPAND_FILL);

	// 2D views: flat bright lines, no sky behind (our grid shows through)
	env_2d.instantiate();
	env_2d->set_background(Environment::BG_CLEAR_COLOR);
	env_2d->set_ambient_source(Environment::AMBIENT_SOURCE_COLOR);
	env_2d->set_ambient_light_color(Color(1, 1, 1));
	env_2d->set_ambient_light_energy(2.0f);
	env_2d->set_tonemapper(Environment::TONE_MAPPER_LINEAR);

	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	bool snap_on = true;
	if (es.is_valid() && OS::get_singleton()->get_environment("HW_SELFTEST").is_empty()) {
		grid = es->get_project_metadata("hewn", "quad_grid", grid);
		split = es->get_project_metadata("hewn", "quad_split", split);
		snap_on = es->get_project_metadata("hewn", "quad_snap", true);
	}
	split = Vector2(CLAMP(split.x, 0.1f, 0.9f), CLAMP(split.y, 0.1f, 0.9f));

	HewnDock *dock = plugin ? plugin->dock : nullptr;
	auto icon = [&](const char *p_name) { return dock ? dock->icon(p_name) : Ref<Texture2D>(); };

	// ---- toolbar
	HBoxContainer *bar = memnew(HBoxContainer);
	bar->set_name("QuadToolbar");
	add_child(bar);
	Ref<ButtonGroup> group;
	group.instantiate();
	const char *names[4] = { "Select", "Block", "Vertex", "Clip" };
	const char *icons[4] = { "ToolSelect", "shape_cube", "vertex", "tool_bisect" };
	const char *tips[4] = {
		"Select: click or drag a frame; drag to move on the grid (Shift: copy), handles resize; click the selection again for rotate handles (Shift+S)",
		"Block: drag a box in a 2D view, adjust it with the handles in any view, Enter creates the shape (Shift+B)",
		"Vertex: drag the vertices of the selected Hewn meshes; vertices on top of each other in a view move together (Shift+V)",
		"Clip: draw a line in a 2D view, Enter cuts the selected Hewn meshes; Shift+X switches which part is kept (Shift+X)",
	};
	for (int i = 0; i < 4; i++) {
		Button *b = memnew(Button);
		b->set_text(names[i]);
		b->set_button_icon(icon(icons[i]));
		b->set_toggle_mode(true);
		b->set_button_group(group);
		b->set_flat(true);
		b->set_tooltip_text(tips[i]);
		b->connect("pressed", callable_mp(this, &HewnQuadView::_on_tool).bind(i));
		bar->add_child(b);
		tool_buttons[i] = b;
	}
	bar->add_child(memnew(VSeparator));
	shape_option = memnew(OptionButton);
	shape_option->set_tooltip_text("Shape created by the Block tool");
	for (int t : BLOCK_SHAPES) {
		shape_option->add_icon_item(dock ? dock->shape_icon(t) : Ref<Texture2D>(), hewn::shapes::type_name((hewn::shapes::Type)t), t);
	}
	bar->add_child(shape_option);
	clip_option = memnew(OptionButton);
	clip_option->set_tooltip_text("Which part the Clip tool keeps (Shift+X)");
	clip_option->add_item("Keep both", CLIP_BOTH);
	clip_option->add_item("Keep front", CLIP_FRONT);
	clip_option->add_item("Keep back", CLIP_BACK);
	clip_option->connect("item_selected", callable_mp(this, &HewnQuadView::_on_clip_mode));
	bar->add_child(clip_option);
	rotate_button = memnew(Button);
	rotate_button->set_text("Rotate handles");
	rotate_button->set_toggle_mode(true);
	rotate_button->set_flat(true);
	rotate_button->set_tooltip_text("Corner handles rotate instead of resize (also: click the selection again)");
	rotate_button->connect("toggled", callable_mp(this, &HewnQuadView::_on_rotate_toggled));
	bar->add_child(rotate_button);
	bar->add_child(memnew(VSeparator));
	Label *gl = memnew(Label);
	gl->set_text("Grid");
	bar->add_child(gl);
	grid_option = memnew(OptionButton);
	grid_option->set_tooltip_text("Grid size ([ and ] in the views)");
	int gi = 4;
	for (int i = 0; i < GRID_COUNT; i++) {
		grid_option->add_item(String::num(GRID_SIZES[i], 6) + " m", i);
		if (near_eq(GRID_SIZES[i], grid)) {
			gi = i;
		}
	}
	grid = GRID_SIZES[gi];
	grid_option->select(gi);
	grid_option->connect("item_selected", callable_mp(this, &HewnQuadView::_on_grid));
	bar->add_child(grid_option);
	snap_check = memnew(CheckBox);
	snap_check->set_text("Snap to grid");
	snap_check->set_tooltip_text("Moves, sizes and new blocks land on the grid (hold Alt to move freely)");
	snap_check->set_pressed(snap_on);
	snap_check->connect("toggled", callable_mp(this, &HewnQuadView::_save_settings).unbind(1));
	bar->add_child(snap_check);
	Control *spacer = memnew(Control);
	spacer->set_h_size_flags(SIZE_EXPAND_FILL);
	bar->add_child(spacer);
	Button *help = memnew(Button);
	help->set_text("?");
	help->set_flat(true);
	help->set_tooltip_text("2D views: wheel zooms, middle mouse or Space + drag pans, [ ] grid size, arrows nudge, F frames the selection, "
						   "Delete deletes, Shift+Z maximizes the view, Esc cancels / deselects.\n"
						   "3D view: right mouse + WASD / QE flies (wheel: speed), middle mouse orbits, Shift + middle pans, click selects.\n"
						   "Drag the blue camera marker in a 2D view to move the 3D camera.");
	bar->add_child(help);

	// ---- the four views
	area = memnew(Control);
	area->set_name("QuadArea");
	area->set_v_size_flags(SIZE_EXPAND_FILL);
	area->set_clip_contents(true);
	area->set_mouse_filter(MOUSE_FILTER_STOP);
	area->connect("resized", callable_mp(this, &HewnQuadView::_layout));
	area->connect("gui_input", callable_mp(this, &HewnQuadView::_area_input));
	add_child(area);
	const HewnQuadPane::Kind kinds[4] = { HewnQuadPane::VIEW_3D, HewnQuadPane::VIEW_TOP, HewnQuadPane::VIEW_FRONT,
		HewnQuadPane::VIEW_SIDE };
	for (int i = 0; i < 4; i++) {
		HewnQuadPane *p = memnew(HewnQuadPane);
		p->set_name(String("Pane") + String::num_int64(i));
		p->setup(kinds[i], this);
		area->add_child(p);
		p->overlay->connect("draw", callable_mp(this, &HewnQuadView::_pane_draw).bind(p));
		p->overlay->connect("gui_input", callable_mp(this, &HewnQuadView::_pane_input).bind(p));
		p->overlay->connect("mouse_entered", callable_mp(this, &HewnQuadView::_pane_mouse_entered).bind(p));
		panes[i] = p;
	}
	status = memnew(Label);
	status->set_name("QuadStatus");
	status->set_clip_text(true);
	add_child(status);

	hw_editor::localize(this);
	_update_toolbar();
}

void HewnQuadView::shown(bool p_visible) {
	set_visible(p_visible);
	set_process(p_visible);
	if (!p_visible) {
		if (drag != DRAG_NONE) {
			_cancel_drag();
		}
		return;
	}
	// Edit mode belongs to the 3D view: leave it (the selection is the objects here).
	if (plugin && plugin->mode != HewnEditorPlugin::MODE_OBJECT) {
		plugin->set_mode(HewnEditorPlugin::MODE_OBJECT);
	}
	if (!camera_initialized) {
		camera_initialized = true;
		SubViewport *ev = EditorInterface::get_singleton()->get_editor_viewport_3d(0);
		Camera3D *ec = ev ? ev->get_camera_3d() : nullptr;
		Transform3D t(Basis(), Vector3(6, 5, 6));
		if (ec) {
			t = ec->get_global_transform();
		} else {
			t = t.looking_at(Vector3(), Vector3(0, 1, 0));
		}
		panes[0]->camera->set_transform(t);
		// the 2D views look at what the 3D camera looks at
		const Vector3 target = t.origin - t.basis.get_column(2) * orbit_distance;
		for (int i = 1; i < 4; i++) {
			panes[i]->center = panes[i]->uv_of(target);
			panes[i]->sync_camera();
		}
	}
	_layout();
}

void HewnQuadView::_notification(int p_what) {
	if (p_what != NOTIFICATION_PROCESS || !is_visible_in_tree()) {
		return;
	}
	// render the edited scene
	SubViewport *ev = EditorInterface::get_singleton()->get_editor_viewport_3d(0);
	Ref<World3D> world = ev ? ev->find_world_3d() : Ref<World3D>();
	for (HewnQuadPane *p : panes) {
		if (p->viewport->get_world_3d() != world) {
			p->viewport->set_world_3d(world);
		}
	}
	// 3D view: fly with WASD / QE while the right mouse button is held
	if (drag == DRAG_FLY) {
		Input *in = Input::get_singleton();
		Vector3 dir;
		dir.z -= in->is_physical_key_pressed(KEY_W) ? 1.0f : 0.0f;
		dir.z += in->is_physical_key_pressed(KEY_S) ? 1.0f : 0.0f;
		dir.x -= in->is_physical_key_pressed(KEY_A) ? 1.0f : 0.0f;
		dir.x += in->is_physical_key_pressed(KEY_D) ? 1.0f : 0.0f;
		float up = (in->is_physical_key_pressed(KEY_E) ? 1.0f : 0.0f) - (in->is_physical_key_pressed(KEY_Q) ? 1.0f : 0.0f);
		if (dir.length_squared() > 0.0f || up != 0.0f) {
			const float dt = (float)get_process_delta_time();
			const float speed = fly_speed * (in->is_key_pressed(KEY_SHIFT) ? 3.0f : 1.0f) * dt;
			Camera3D *c = panes[0]->camera;
			Transform3D t = c->get_transform();
			t.origin += t.basis.xform(dir.normalized()) * speed + Vector3(0, up * speed, 0);
			c->set_transform(t);
		}
	}
	clean_vsel();
	for (HewnQuadPane *p : panes) {
		p->sync_camera();
		p->queue_redraw();
		p->overlay->queue_redraw();
	}
	_update_status();
}

void HewnQuadView::_layout() {
	if (!area) {
		return;
	}
	const Vector2 s = area->get_size();
	const float g = GAP * edscale() * 0.5f;
	if (maximized >= 0) {
		for (int i = 0; i < 4; i++) {
			panes[i]->set_visible(i == maximized);
		}
		panes[maximized]->set_position(Vector2());
		panes[maximized]->set_size(s);
		return;
	}
	const float x = std::round(s.x * split.x), y = std::round(s.y * split.y);
	const Rect2 r[4] = { Rect2(0, 0, x - g, y - g), Rect2(x + g, 0, s.x - x - g, y - g), Rect2(0, y + g, x - g, s.y - y - g),
		Rect2(x + g, y + g, s.x - x - g, s.y - y - g) };
	for (int i = 0; i < 4; i++) {
		panes[i]->set_visible(true);
		panes[i]->set_position(r[i].position);
		panes[i]->set_size(r[i].size.max(Vector2(1, 1)));
	}
}

void HewnQuadView::_update_toolbar() {
	for (int i = 0; i < 4; i++) {
		tool_buttons[i]->set_pressed_no_signal(i == (int)tool);
	}
	shape_option->set_visible(tool == TOOL_BLOCK);
	clip_option->set_visible(tool == TOOL_CLIP);
	clip_option->select((int)clip_mode);
	rotate_button->set_visible(tool == TOOL_SELECT);
	rotate_button->set_pressed_no_signal(handle_rotate);
}

void HewnQuadView::set_tool(int p_tool) {
	if (drag != DRAG_NONE) {
		_cancel_drag();
	}
	tool = (Tool)CLAMP(p_tool, 0, 3);
	if (tool != TOOL_CLIP) {
		has_clip = false;
	}
	_update_toolbar();
}

void HewnQuadView::_on_tool(int p_tool) {
	set_tool(p_tool);
}

void HewnQuadView::_on_grid(int p_index) {
	grid = GRID_SIZES[CLAMP(p_index, 0, GRID_COUNT - 1)];
	_save_settings();
}

void HewnQuadView::_on_clip_mode(int p_index) {
	clip_mode = (ClipMode)CLAMP(p_index, 0, 2);
}

void HewnQuadView::_on_rotate_toggled(bool p_on) {
	handle_rotate = p_on;
}

void HewnQuadView::_save_settings() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_valid() && OS::get_singleton()->get_environment("HW_SELFTEST").is_empty()) {
		es->set_project_metadata("hewn", "quad_grid", grid);
		es->set_project_metadata("hewn", "quad_split", split);
		es->set_project_metadata("hewn", "quad_snap", snap_check->is_pressed());
	}
}

void HewnQuadView::_update_status() {
	String s;
	if (hover_pane && hover_pane->is_2d()) {
		const Vector2 uv = hover_pane->to_uv(hover_pos);
		const char *axes = "XYZ";
		s += String::chr(axes[axis_index(hover_pane->u)]) + " " + String::num(snap(uv.x * (hover_pane->u.dot(Vector3(1, 1, 1)) < 0 ? -1.0f : 1.0f)), 3) + "  ";
		s += String::chr(axes[axis_index(hover_pane->v)]) + " " + String::num(snap(uv.y * (hover_pane->v.dot(Vector3(1, 1, 1)) < 0 ? -1.0f : 1.0f)), 3) + "    ";
	}
	AABB b;
	if (tool == TOOL_BLOCK && has_block) {
		b = block;
		s += T("Block") + ": ";
	} else if (selection_bounds(b)) {
		s += T("Selection") + ": ";
	} else {
		b = AABB();
	}
	if (b.size != Vector3()) {
		s += String::num(b.size.x, 3) + " x " + String::num(b.size.y, 3) + " x " + String::num(b.size.z, 3) + " m    ";
	}
	s += T("Grid") + " " + String::num(grid, 6) + " m";
	if (tool == TOOL_BLOCK && has_block) {
		s += "    " + T("Enter: create, Esc: cancel");
	} else if (tool == TOOL_CLIP && has_clip) {
		s += "    " + T("Enter: clip, Shift+X: kept part, Esc: cancel");
	}
	if (status->get_text() != s) {
		status->set_text(s);
	}
}

// ================================================================== scene helpers

EditorUndoRedoManager *HewnQuadView::undo_redo() const {
	return plugin ? plugin->get_undo_redo() : nullptr;
}

const std::vector<HewnQuadView::Item> &HewnQuadView::get_items() {
	const uint64_t frame = Engine::get_singleton()->get_process_frames();
	if (frame == items_frame) {
		return items;
	}
	items_frame = frame;
	items.clear();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return items;
	}
	std::unordered_map<Node3D *, size_t> index;
	std::vector<Node *> stack = { root };
	while (!stack.empty()) {
		Node *n = stack.back();
		stack.pop_back();
		for (int i = n->get_child_count() - 1; i >= 0; i--) {
			stack.push_back(n->get_child(i));
		}
		VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(n);
		if (!vi || !vi->is_visible_in_tree() || !(vi->get_layer_mask() & SCENE_LAYERS)) {
			continue;
		}
		// the node to select: the visual itself, or the instanced scene it belongs to
		Node *s = n;
		while (s != root && s->get_owner() != root && s->get_parent()) {
			s = s->get_parent();
		}
		Node3D *sel = Object::cast_to<Node3D>(s);
		if (!sel) {
			continue;
		}
		const AABB b = xform_box(vi->get_global_transform(), vi->get_aabb());
		auto it = index.find(sel);
		if (it == index.end()) {
			index[sel] = items.size();
			Item item;
			item.node = sel;
			item.box = b;
			item.hewn = Object::cast_to<HewnMesh3D>(sel);
			items.push_back(item);
		} else {
			items[it->second].box.merge_with(b);
		}
	}
	return items;
}

const HewnQuadView::Item *HewnQuadView::item_of(Node3D *p_node) {
	for (const Item &it : get_items()) {
		if (it.node == p_node) {
			return &it;
		}
	}
	return nullptr;
}

std::vector<Node3D *> HewnQuadView::selected_nodes() const {
	std::vector<Node3D *> out;
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return out;
	}
	const TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
	for (int64_t i = 0; i < sel.size(); i++) {
		Node3D *n = Object::cast_to<Node3D>(sel[i]);
		if (n && (n == root || root->is_ancestor_of(n))) {
			out.push_back(n);
		}
	}
	return out;
}

bool HewnQuadView::is_selected(Node3D *p_node) const {
	for (Node3D *n : selected_nodes()) {
		if (n == p_node) {
			return true;
		}
	}
	return false;
}

void HewnQuadView::set_selection(const std::vector<Node3D *> &p_nodes) {
	EditorSelection *es = EditorInterface::get_singleton()->get_selection();
	es->clear();
	for (Node3D *n : p_nodes) {
		es->add_node(n);
	}
	if (p_nodes.size() == 1) {
		EditorInterface::get_singleton()->edit_node(p_nodes[0]); // the inspector
	}
	items_frame = ~0ULL;
}

std::vector<HewnMesh3D *> HewnQuadView::selected_meshes() const {
	std::vector<HewnMesh3D *> out;
	for (Node3D *n : selected_nodes()) {
		if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(n)) {
			out.push_back(m);
		}
	}
	return out;
}

bool HewnQuadView::selection_bounds(AABB &r_box) {
	bool any = false;
	for (Node3D *n : selected_nodes()) {
		const Item *it = item_of(n);
		const AABB b = it ? it->box : AABB(n->get_global_position() - Vector3(0.125f, 0.125f, 0.125f), Vector3(0.25f, 0.25f, 0.25f));
		if (!any) {
			r_box = b;
			any = true;
		} else {
			r_box.merge_with(b);
		}
	}
	return any;
}

bool HewnQuadView::snapping() const {
	return snap_check && snap_check->is_pressed() && !Input::get_singleton()->is_key_pressed(KEY_ALT);
}

float HewnQuadView::snap(float p_value) const {
	return snapping() ? std::round(p_value / grid) * grid : p_value;
}

float HewnQuadView::min_size() const {
	return snapping() ? grid : 0.001f;
}

void HewnQuadView::box_uv(HewnQuadPane *p, const AABB &p_box, Vector2 &r_min, Vector2 &r_max) const {
	r_min = r_max = p->uv_of(p_box.get_endpoint(0));
	for (int i = 1; i < 8; i++) {
		const Vector2 q = p->uv_of(p_box.get_endpoint(i));
		r_min = r_min.min(q);
		r_max = r_max.max(q);
	}
}

Rect2 HewnQuadView::screen_rect(HewnQuadPane *p, const AABB &p_box) const {
	Vector2 mn, mx;
	box_uv(p, p_box, mn, mx);
	const Vector2 a = p->to_screen(p->from_uv(Vector2(mn.x, mx.y), 0.0f)); // top left
	const Vector2 b = p->to_screen(p->from_uv(Vector2(mx.x, mn.y), 0.0f));
	return Rect2(a, b - a).abs();
}

AABB HewnQuadView::box_from_uv(HewnQuadPane *p, const Vector2 &p_min, const Vector2 &p_max, const AABB &p_depth_from) const {
	float d0 = p_depth_from.get_endpoint(0).dot(p->w), d1 = d0;
	for (int i = 1; i < 8; i++) {
		const float d = p_depth_from.get_endpoint(i).dot(p->w);
		d0 = MIN(d0, d);
		d1 = MAX(d1, d);
	}
	const Vector3 a = p->from_uv(p_min, d0), b = p->from_uv(p_max, d1);
	const Vector3 lo = a.min(b), hi = a.max(b);
	return AABB(lo, hi - lo);
}

Vector2 HewnQuadView::handle_pos(const Rect2 &p_rect, int p_handle) const {
	return p_rect.get_center() + Vector2(HANDLE_X[p_handle] * p_rect.size.x, HANDLE_Y[p_handle] * p_rect.size.y) * 0.5f;
}

int HewnQuadView::handle_at(HewnQuadPane *p, const AABB &p_box, const Vector2 &p_pos) const {
	const Rect2 r = screen_rect(p, p_box);
	const float reach = 6.0f * edscale();
	const bool corners_only = tool == TOOL_SELECT && handle_rotate;
	for (int h = 0; h < 8; h++) {
		if (corners_only && h % 2 == 1) {
			continue;
		}
		const Vector2 d = (p_pos - handle_pos(r, h)).abs();
		if (d.x <= reach && d.y <= reach) {
			return h;
		}
	}
	return -1;
}

void HewnQuadView::mesh_edges(const hewn::Mesh &p_mesh, const Transform3D &p_xf, PackedVector3Array &r_lines) {
	std::unordered_set<uint64_t> seen;
	for (const hewn::Face &F : p_mesh.faces) {
		const int n = F.size();
		for (int i = 0; i < n; i++) {
			const int a = F.v[i], b = F.v[(i + 1) % n];
			if (seen.insert(hewn::ekey(a, b)).second) {
				r_lines.push_back(p_xf.xform(p_mesh.pos[a]));
				r_lines.push_back(p_xf.xform(p_mesh.pos[b]));
			}
		}
	}
	for (const std::pair<int, int> &w : p_mesh.wires) {
		r_lines.push_back(p_xf.xform(p_mesh.pos[w.first]));
		r_lines.push_back(p_xf.xform(p_mesh.pos[w.second]));
	}
}

void HewnQuadView::world_edges(const Item &p_item, PackedVector3Array &r_lines) const {
	if (p_item.hewn) {
		const hewn::Mesh em = p_item.hewn->has_modifiers() ? p_item.hewn->evaluated_mesh() : p_item.hewn->get_data();
		mesh_edges(em, p_item.hewn->get_global_transform(), r_lines);
		return;
	}
	static const int E[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
	for (const int *e : E) {
		r_lines.push_back(p_item.box.get_endpoint(e[0]));
		r_lines.push_back(p_item.box.get_endpoint(e[1]));
	}
}

Node3D *HewnQuadView::pick_2d(HewnQuadPane *p, const Vector2 &p_pos) {
	const std::vector<Item> &its = get_items();
	// an edge under the cursor
	float best = 6.0f * edscale();
	Node3D *hit = nullptr;
	for (const Item &it : its) {
		PackedVector3Array lines;
		world_edges(it, lines);
		for (int64_t i = 0; i + 1 < lines.size(); i += 2) {
			const float d = hw_editor::seg_dist(p_pos, p->to_screen(lines[i]), p->to_screen(lines[i + 1]));
			if (d < best) {
				best = d;
				hit = it.node;
			}
		}
	}
	if (hit) {
		return hit;
	}
	// inside the bounds: the smallest object
	float area_best = 1e30f;
	for (const Item &it : its) {
		const Rect2 r = screen_rect(p, it.box);
		if (r.grow(2.0f).has_point(p_pos) && r.get_area() < area_best) {
			area_best = r.get_area();
			hit = it.node;
		}
	}
	return hit;
}

Node3D *HewnQuadView::pick_3d(HewnQuadPane *p, const Vector2 &p_pos) {
	const Vector3 from = p->camera->project_ray_origin(p_pos);
	const Vector3 dir = p->camera->project_ray_normal(p_pos);
	float best = 1e30f;
	Node3D *hit = nullptr;
	for (const Item &it : get_items()) {
		float t = 1e30f;
		if (it.hewn) {
			if (hw_editor::raycast_node(it.hewn, from, dir, t, nullptr) < 0) {
				continue;
			}
		} else {
			Vector3 at;
			if (!it.box.intersects_segment(from, from + dir * 10000.0f, &at)) {
				continue;
			}
			t = at.distance_to(from);
		}
		if (t < best) {
			best = t;
			hit = it.node;
		}
	}
	return hit;
}

std::vector<int> *HewnQuadView::vsel_of(HewnMesh3D *p_mesh) {
	for (std::pair<uint64_t, std::vector<int>> &e : vsel) {
		if (e.first == p_mesh->get_instance_id()) {
			return &e.second;
		}
	}
	vsel.push_back({ p_mesh->get_instance_id(), {} });
	return &vsel.back().second;
}

void HewnQuadView::clean_vsel() {
	const std::vector<HewnMesh3D *> meshes = selected_meshes();
	for (size_t i = 0; i < vsel.size();) {
		HewnMesh3D *m = Object::cast_to<HewnMesh3D>(ObjectDB::get_instance(vsel[i].first));
		bool keep = false;
		for (HewnMesh3D *s : meshes) {
			keep = keep || s == m;
		}
		if (!keep) {
			vsel.erase(vsel.begin() + i);
			continue;
		}
		std::vector<int> &vs = vsel[i].second;
		const int nv = m->get_vertex_count();
		vs.erase(std::remove_if(vs.begin(), vs.end(), [nv](int v) { return v < 0 || v >= nv; }), vs.end());
		i++;
	}
}

void HewnQuadView::_set_owners(const Array &p_pairs) {
	for (int64_t i = 0; i + 1 < p_pairs.size(); i += 2) {
		Node *n = Object::cast_to<Node>(p_pairs[i]);
		Node *o = Object::cast_to<Node>(p_pairs[i + 1]);
		if (n && o) {
			n->set_owner(o);
		}
	}
}

Array HewnQuadView::owners_of(Node *p_node) {
	Array out;
	std::vector<Node *> stack = { p_node };
	while (!stack.empty()) {
		Node *n = stack.back();
		stack.pop_back();
		if (n->get_owner()) {
			out.push_back(n);
			out.push_back(n->get_owner());
		}
		for (int i = 0; i < n->get_child_count(); i++) {
			stack.push_back(n->get_child(i));
		}
	}
	return out;
}

// ================================================================== edits

void HewnQuadView::save_selection() {
	saved.clear();
	for (Node3D *n : selected_nodes()) {
		Saved s;
		s.id = n->get_instance_id();
		s.xf = n->get_global_transform();
		if (HewnMesh3D *m = Object::cast_to<HewnMesh3D>(n)) {
			s.hewn = true;
			s.state = m->get_state();
		}
		saved.push_back(s);
	}
}

void HewnQuadView::restore_saved() {
	for (const Saved &s : saved) {
		Node3D *n = node_of(s.id);
		if (!n) {
			continue;
		}
		if (s.hewn) {
			Object::cast_to<HewnMesh3D>(n)->set_state(s.state);
		}
		if (n->is_inside_tree()) {
			n->set_global_transform(s.xf);
		}
	}
	items_frame = ~0ULL;
}

// The world map p' = S p + offset (S: scale along world axes). Hewn meshes take it into their shape
// parameters (or vertices) and keep their rotation; other nodes get it in their transform.
void HewnQuadView::apply_scale(Node3D *p_node, const Saved &p_before, const Basis &p_scale, const Vector3 &p_offset) {
	const Transform3D G = p_before.xf;
	const Vector3 origin = p_scale.xform(G.origin) + p_offset;
	HewnMesh3D *m = p_before.hewn ? Object::cast_to<HewnMesh3D>(p_node) : nullptr;
	if (!m) {
		p_node->set_global_transform(Transform3D(p_scale * G.basis, origin));
		return;
	}
	m->set_state(p_before.state);
	const AABB old_box = xform_box(G, m->get_aabb());
	AABB expected(p_scale.xform(old_box.position) + p_offset, Vector3());
	expected.expand_to(p_scale.xform(old_box.get_end()) + p_offset);
	const Basis L = G.basis.inverse() * p_scale * G.basis; // the same map in local space
	bool diagonal = true;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			if (i != j && std::fabs(L.rows[i][j]) > 1e-4f) {
				diagonal = false;
			}
		}
	}
	m->set_global_transform(Transform3D(G.basis, origin));
	bool done = false;
	if (diagonal && scale_params(m, Vector3(L.rows[0][0], L.rows[1][1], L.rows[2][2]))) {
		// parameters that don't scale with the box (e.g. door openings swapping sides): use the vertices
		const AABB now = xform_box(m->get_global_transform(), m->get_aabb());
		done = (now.size - expected.size).length() <= 1e-3f * MAX(1.0f, expected.size.length());
		if (!done) {
			m->set_state(p_before.state);
		}
	}
	if (!done) {
		for (Vector3 &q : m->get_data().pos) {
			q = L.xform(q);
		}
		m->commit(true);
	}
	// keep the bounds where the map puts them (shapes keep an anchor point when parameters change)
	const AABB now = xform_box(m->get_global_transform(), m->get_aabb());
	m->set_global_position(origin + expected.get_center() - now.get_center());
}

void HewnQuadView::apply_rotation(Node3D *p_node, const Saved &p_before, const Basis &p_rot, const Vector3 &p_pivot) {
	p_node->set_global_transform(Transform3D(p_rot * p_before.xf.basis, p_pivot + p_rot.xform(p_before.xf.origin - p_pivot)));
}

void HewnQuadView::commit_saved(const String &p_action) {
	EditorUndoRedoManager *ur = undo_redo();
	if (!ur) {
		return;
	}
	std::unordered_set<Node *> cloned(clones.begin(), clones.end());
	ur->create_action(String("Hewn: ") + p_action);
	for (Node *c : clones) {
		Node *parent = c->get_parent();
		ur->add_do_method(parent, "add_child", c, true);
		ur->add_do_method(this, "_set_owners", owners_of(c));
		ur->add_do_reference(c);
		ur->add_undo_method(parent, "remove_child", c);
	}
	for (const Saved &s : saved) {
		Node3D *n = node_of(s.id);
		if (!n || cloned.count(n)) {
			continue;
		}
		if (s.hewn) {
			HewnMesh3D *m = Object::cast_to<HewnMesh3D>(n);
			ur->add_do_method(m, "set_state", m->get_state());
			ur->add_undo_method(m, "set_state", s.state);
		}
		ur->add_do_method(n, "set_global_transform", n->get_global_transform());
		ur->add_undo_method(n, "set_global_transform", s.xf);
	}
	ur->commit_action(false); // (already done while dragging)
	saved.clear();
	clones.clear();
}

// Shift + drag: copies next to the originals, they move instead (and become the selection).
void HewnQuadView::start_clone() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	std::vector<Node3D *> dups;
	clone_sources.clear();
	for (Saved &s : saved) {
		Node3D *n = node_of(s.id);
		Node *parent = n ? n->get_parent() : nullptr;
		if (!n || !parent || n == root) {
			continue;
		}
		Node3D *d = Object::cast_to<Node3D>(n->duplicate());
		if (!d) {
			continue;
		}
		parent->add_child(d, true);
		parent->move_child(d, n->get_index() + 1);
		copy_owners(n, d, root);
		d->set_global_transform(s.xf);
		clone_sources.push_back(s.id);
		s.id = d->get_instance_id();
		clones.push_back(d);
		dups.push_back(d);
	}
	if (!dups.empty()) {
		set_selection(dups);
	}
}

void HewnQuadView::delete_selection() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	EditorUndoRedoManager *ur = undo_redo();
	std::vector<Node3D *> sel = selected_nodes();
	if (!ur || sel.empty()) {
		return;
	}
	set_selection({});
	ur->create_action("Hewn: Delete");
	for (Node3D *n : sel) {
		Node *parent = n->get_parent();
		if (n == root || !parent) {
			continue;
		}
		ur->add_do_method(parent, "remove_child", n);
		ur->add_undo_method(parent, "add_child", n, true);
		ur->add_undo_method(parent, "move_child", n, n->get_index());
		ur->add_undo_method(this, "_set_owners", owners_of(n));
		ur->add_undo_reference(n);
	}
	ur->commit_action();
	items_frame = ~0ULL;
}

void HewnQuadView::nudge(HewnQuadPane *p, const Vector2 &p_dir) {
	const Vector3 off = (p->u * p_dir.x + p->v * p_dir.y) * grid;
	if (tool == TOOL_BLOCK && has_block) {
		block.position += off;
		return;
	}
	save_selection();
	if (saved.empty()) {
		return;
	}
	for (const Saved &s : saved) {
		if (Node3D *n = node_of(s.id)) {
			n->set_global_transform(Transform3D(s.xf.basis, s.xf.origin + off));
		}
	}
	commit_saved("Nudge");
	items_frame = ~0ULL;
}

void HewnQuadView::create_block() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	EditorUndoRedoManager *ur = undo_redo();
	if (!has_block || !root || !ur) {
		return;
	}
	const int type = shape_option->get_selected_id();
	HewnMesh3D *n = memnew(HewnMesh3D);
	n->set_shape(type);
	const String name = String(hewn::shapes::type_name((hewn::shapes::Type)type)).replace(" ", "");
	n->set_name(name);
	root->add_child(n, true);
	n->set_owner(root);
	n->set_global_transform(Transform3D());
	// fit the default shape into the box
	Saved s;
	s.id = n->get_instance_id();
	s.xf = n->get_global_transform();
	s.hewn = true;
	s.state = n->get_state();
	const AABB b0 = xform_box(s.xf, n->get_aabb());
	Vector3 k(1, 1, 1), off;
	for (int i = 0; i < 3; i++) {
		if (b0.size[i] > 1e-6f) {
			k[i] = block.size[i] / b0.size[i];
		}
		off[i] = block.position[i] - k[i] * b0.position[i];
	}
	apply_scale(n, s, Basis::from_scale(k), off);
	ur->create_action("Hewn: Create " + name);
	ur->add_do_method(root, "add_child", n, true);
	ur->add_do_method(n, "set_owner", root);
	ur->add_do_reference(n);
	ur->add_undo_method(root, "remove_child", n);
	ur->commit_action(false);
	last_block = block;
	has_block = false;
	set_selection({ n });
}

void HewnQuadView::apply_clip() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	EditorUndoRedoManager *ur = undo_redo();
	if (!has_clip || !clip_pane || !root || !ur) {
		return;
	}
	const Vector3 d = clip_b - clip_a;
	if (d.length() < 1e-5f) {
		return;
	}
	const Vector3 normal = d.cross(clip_pane->w).normalized();
	// the meshes the line goes through
	std::vector<HewnMesh3D *> cut;
	for (HewnMesh3D *m : selected_meshes()) {
		const Transform3D G = m->get_global_transform();
		const Vector3 lp = G.affine_inverse().xform(clip_a);
		const Vector3 ln = G.basis.transposed().xform(normal).normalized();
		bool pos = false, neg = false;
		for (const Vector3 &q : m->get_data().pos) {
			const float s = ln.dot(q - lp);
			pos = pos || s > 1e-5f;
			neg = neg || s < -1e-5f;
		}
		if (pos && neg) {
			cut.push_back(m);
		}
	}
	has_clip = false;
	if (cut.empty()) {
		return;
	}
	ur->create_action("Hewn: Clip");
	for (HewnMesh3D *m : cut) {
		const Transform3D G = m->get_global_transform();
		const Vector3 lp = G.affine_inverse().xform(clip_a);
		const Vector3 ln = G.basis.transposed().xform(normal).normalized();
		const Dictionary before = m->get_state();
		if (clip_mode == CLIP_BOTH) {
			Node *parent = m->get_parent();
			HewnMesh3D *dup = Object::cast_to<HewnMesh3D>(m->duplicate());
			if (dup && parent) {
				parent->add_child(dup, true);
				parent->move_child(dup, m->get_index() + 1);
				copy_owners(m, dup, root);
				dup->bisect(lp, ln, false, true, true); // the back part
				ur->add_do_method(parent, "add_child", dup, true);
				ur->add_do_method(this, "_set_owners", owners_of(dup));
				ur->add_do_reference(dup);
				ur->add_undo_method(parent, "remove_child", dup);
			}
		}
		m->bisect(lp, ln, clip_mode != CLIP_BACK, clip_mode == CLIP_BACK, true);
		ur->add_do_method(m, "set_state", m->get_state());
		ur->add_undo_method(m, "set_state", before);
	}
	ur->commit_action(false);
	items_frame = ~0ULL;
}

void HewnQuadView::focus_selection() {
	AABB box;
	if (!selection_bounds(box)) {
		if (!has_block) {
			return;
		}
		box = block;
	}
	for (int i = 1; i < 4; i++) {
		HewnQuadPane *p = panes[i];
		Vector2 mn, mx;
		box_uv(p, box, mn, mx);
		p->center = (mn + mx) * 0.5f;
		const Vector2 ext = (mx - mn).max(Vector2(0.01f, 0.01f));
		const Vector2 s = p->get_size();
		p->zoom = CLAMP(MIN(s.x / ext.x, s.y / ext.y) * 0.6f, 0.5f, 20000.0f);
		p->sync_camera();
	}
	Camera3D *c = panes[0]->camera;
	Transform3D t = c->get_transform();
	orbit_distance = MAX(0.5f, box.size.length() * 0.5f) * 2.5f;
	t.origin = box.get_center() + t.basis.get_column(2) * orbit_distance;
	c->set_transform(t);
}

void HewnQuadView::move_verts(HewnQuadPane *p, const Vector2 &p_pos) {
	const Vector2 g = p->uv_of(grab_world);
	Vector2 target = g + (p->to_uv(p_pos) - press_uv);
	target = Vector2(snap(target.x), snap(target.y));
	const Vector3 off = p->u * (target.x - g.x) + p->v * (target.y - g.y);
	for (const Saved &s : saved) {
		HewnMesh3D *m = s.hewn ? Object::cast_to<HewnMesh3D>(node_of(s.id)) : nullptr;
		if (!m) {
			continue;
		}
		const std::vector<int> vs = *vsel_of(m);
		if (vs.empty()) {
			continue;
		}
		m->set_state(s.state);
		const Transform3D inv = s.xf.affine_inverse();
		hewn::Mesh &d = m->get_data();
		for (int i : vs) {
			if (i >= 0 && i < (int)d.pos.size()) {
				d.pos[i] = inv.xform(s.xf.xform(d.pos[i]) + off);
			}
		}
		m->commit(true);
	}
	items_frame = ~0ULL;
}

void HewnQuadView::select_verts_in(HewnQuadPane *p, const Rect2 &p_rect, bool p_add) {
	if (!p_add) {
		vsel.clear();
	}
	for (HewnMesh3D *m : selected_meshes()) {
		std::vector<int> &vs = *vsel_of(m);
		std::unordered_set<int> have(vs.begin(), vs.end());
		const Transform3D G = m->get_global_transform();
		const hewn::Mesh &d = m->get_data();
		for (int i = 0; i < (int)d.pos.size(); i++) {
			if (p_rect.has_point(p->to_screen(G.xform(d.pos[i]))) && !have.count(i)) {
				vs.push_back(i);
			}
		}
	}
}

// ================================================================== input

void HewnQuadView::_pane_mouse_entered(HewnQuadPane *p) {
	hover_pane = p;
	// keys go to the view under the mouse (unless a text field is being typed in)
	if (!text_field_focused(p->overlay)) {
		p->overlay->grab_focus();
	}
}

void HewnQuadView::_area_input(const Ref<InputEvent> &p_event) {
	// the gaps between the views: drag to resize them
	const Vector2 s = area->get_size();
	const float x = std::round(s.x * split.x), y = std::round(s.y * split.y);
	const float reach = GAP * edscale() + 2.0f;
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && maximized < 0) {
		if (mb->is_pressed()) {
			split_axes = (std::fabs(mb->get_position().x - x) <= reach ? 1 : 0) | (std::fabs(mb->get_position().y - y) <= reach ? 2 : 0);
			drag = split_axes ? DRAG_SPLIT : DRAG_NONE;
		} else if (drag == DRAG_SPLIT) {
			drag = DRAG_NONE;
			_save_settings();
		}
		return;
	}
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		const Vector2 pos = mm->get_position();
		if (drag == DRAG_SPLIT) {
			if (split_axes & 1) {
				split.x = CLAMP(pos.x / MAX(1.0f, s.x), 0.1f, 0.9f);
			}
			if (split_axes & 2) {
				split.y = CLAMP(pos.y / MAX(1.0f, s.y), 0.1f, 0.9f);
			}
			_layout();
			return;
		}
		const bool vx = std::fabs(pos.x - x) <= reach, hy = std::fabs(pos.y - y) <= reach;
		area->set_default_cursor_shape(vx && hy ? CURSOR_MOVE : (vx ? CURSOR_HSIZE : (hy ? CURSOR_VSIZE : CURSOR_ARROW)));
	}
}

void HewnQuadView::_pane_input(const Ref<InputEvent> &p_event, HewnQuadPane *p) {
	if (drag == DRAG_SPLIT) {
		return;
	}
	Ref<InputEventMouse> me = p_event;
	if (me.is_valid() && (drag == DRAG_NONE || drag_pane == p)) {
		hover_pane = p;
		hover_pos = me->get_position();
	}
	if (p->is_2d()) {
		_input_2d(p, p_event);
	} else {
		_input_3d(p, p_event);
	}
}

bool HewnQuadView::_shortcut(HewnQuadPane *p, const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> k = p_event;
	if (k.is_null() || !k->is_pressed()) {
		return false;
	}
	const Key c = hw_editor::key_code(k);
	const bool shift = k->is_shift_pressed(), ctrl = k->is_ctrl_pressed() || k->is_meta_pressed(), alt = k->is_alt_pressed();
	if (ctrl || alt) {
		return false; // Godot's shortcuts (undo, save...)
	}
	if (shift) {
		switch (c) {
			case KEY_S:
				set_tool(TOOL_SELECT);
				return true;
			case KEY_B:
				set_tool(TOOL_BLOCK);
				return true;
			case KEY_V:
				set_tool(TOOL_VERTEX);
				return true;
			case KEY_X:
				if (tool == TOOL_CLIP) {
					clip_mode = (ClipMode)((clip_mode + 1) % 3);
					_update_toolbar();
				} else {
					set_tool(TOOL_CLIP);
				}
				return true;
			case KEY_Z: {
				int idx = 0;
				for (int i = 0; i < 4; i++) {
					idx = panes[i] == p ? i : idx;
				}
				maximized = maximized == idx ? -1 : idx;
				_layout();
				return true;
			}
			default:
				return false;
		}
	}
	switch (c) {
		case KEY_BRACKETLEFT:
		case KEY_BRACKETRIGHT: {
			int i = grid_option->get_selected();
			i = CLAMP(i + (c == KEY_BRACKETRIGHT ? 1 : -1), 0, GRID_COUNT - 1);
			grid_option->select(i);
			_on_grid(i);
			return true;
		}
		case KEY_ENTER:
		case KEY_KP_ENTER:
			if (tool == TOOL_BLOCK) {
				create_block();
			} else if (tool == TOOL_CLIP) {
				apply_clip();
			}
			return true;
		case KEY_ESCAPE:
			if (drag != DRAG_NONE) {
				_cancel_drag();
			} else if (tool == TOOL_BLOCK && has_block) {
				has_block = false;
			} else if (tool == TOOL_CLIP && has_clip) {
				has_clip = false;
			} else if (tool == TOOL_VERTEX && !vsel.empty()) {
				vsel.clear();
			} else {
				set_selection({});
			}
			return true;
		case KEY_DELETE:
			if (drag == DRAG_NONE && tool == TOOL_SELECT) {
				delete_selection();
			}
			return true;
		case KEY_F:
			focus_selection();
			return true;
		case KEY_LEFT:
		case KEY_RIGHT:
		case KEY_UP:
		case KEY_DOWN:
			if (p->is_2d() && drag == DRAG_NONE) {
				nudge(p, Vector2(c == KEY_LEFT ? -1 : (c == KEY_RIGHT ? 1 : 0), c == KEY_UP ? 1 : (c == KEY_DOWN ? -1 : 0)));
				return true;
			}
			return false;
		default:
			return false;
	}
}

void HewnQuadView::_input_2d(HewnQuadPane *p, const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid()) {
		const Vector2 pos = mb->get_position();
		const MouseButton btn = mb->get_button_index();
		if (mb->is_pressed()) {
			p->overlay->grab_focus();
		}
		if ((btn == MOUSE_BUTTON_WHEEL_UP || btn == MOUSE_BUTTON_WHEEL_DOWN) && mb->is_pressed()) {
			p->zoom_at(pos, btn == MOUSE_BUTTON_WHEEL_UP ? 1.2f : 1.0f / 1.2f);
			p->overlay->accept_event();
			return;
		}
		if (btn == MOUSE_BUTTON_MIDDLE) {
			if (mb->is_pressed() && drag == DRAG_NONE) {
				drag = DRAG_PAN;
				drag_pane = p;
				last_pos = pos;
			} else if (!mb->is_pressed() && drag == DRAG_PAN) {
				drag = DRAG_NONE;
			}
			p->overlay->accept_event();
			return;
		}
		if (btn == MOUSE_BUTTON_LEFT) {
			if (mb->is_pressed()) {
				if (drag == DRAG_NONE) {
					if (Input::get_singleton()->is_key_pressed(KEY_SPACE)) {
						drag = DRAG_PAN;
						drag_pane = p;
						last_pos = pos;
					} else {
						_press_2d(p, pos, mb->is_ctrl_pressed() || mb->is_meta_pressed(), mb->is_shift_pressed());
					}
				}
			} else if (drag == DRAG_PAN) {
				drag = DRAG_NONE;
			} else if (drag != DRAG_NONE) {
				_release_2d(drag_pane ? drag_pane : p, pos);
			}
			p->overlay->accept_event();
			return;
		}
		if (btn == MOUSE_BUTTON_RIGHT && mb->is_pressed() && drag != DRAG_NONE && drag != DRAG_PAN) {
			_cancel_drag();
			p->overlay->accept_event();
		}
		return;
	}
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		const Vector2 pos = mm->get_position();
		if (drag != DRAG_NONE && drag_pane == p) {
			_drag_2d(p, pos);
			p->overlay->accept_event();
			return;
		}
		// cursor shape over handles / the selection
		CursorShape cs = CURSOR_ARROW;
		AABB box;
		const bool box_on = tool == TOOL_BLOCK ? has_block : (tool == TOOL_SELECT && selection_bounds(box));
		if (tool == TOOL_BLOCK) {
			box = block;
		}
		if (box_on) {
			const int h = handle_at(p, box, pos);
			if (h >= 0) {
				const bool rot = tool == TOOL_SELECT && handle_rotate;
				cs = rot ? CURSOR_POINTING_HAND : (HANDLE_X[h] == 0 ? CURSOR_VSIZE : (HANDLE_Y[h] == 0 ? CURSOR_HSIZE : (HANDLE_X[h] == HANDLE_Y[h] ? CURSOR_FDIAGSIZE : CURSOR_BDIAGSIZE)));
			} else if (screen_rect(p, box).has_point(pos)) {
				cs = CURSOR_MOVE;
			}
		} else if (tool == TOOL_CLIP || tool == TOOL_BLOCK) {
			cs = CURSOR_CROSS;
		}
		p->overlay->set_default_cursor_shape(cs);
		return;
	}
	if (_shortcut(p, p_event)) {
		p->overlay->accept_event();
	}
}

void HewnQuadView::_press_2d(HewnQuadPane *p, const Vector2 &p_pos, bool p_ctrl, bool p_shift) {
	drag_pane = p;
	press_pos = last_pos = p_pos;
	press_uv = p->to_uv(p_pos);
	drag_ctrl = p_ctrl;
	drag_shift = p_shift;
	press_item = nullptr;
	pending_toggle = false;
	drag_handle = -1;
	// the 3D camera marker
	if (p->to_screen(panes[0]->camera->get_position()).distance_to(p_pos) <= 7.0f * edscale()) {
		drag = DRAG_CAMERA;
		return;
	}
	switch (tool) {
		case TOOL_SELECT: {
			AABB box;
			if (selection_bounds(box)) {
				const int h = handle_at(p, box, p_pos);
				if (h >= 0) {
					save_selection();
					drag_box = box;
					drag_handle = h;
					if (handle_rotate) {
						drag = DRAG_ROTATE;
						rotate_start = (p_pos - screen_rect(p, box).get_center()).angle();
						drag_angle = 0.0f;
					} else {
						drag = DRAG_RESIZE;
					}
					return;
				}
				if (!p_ctrl && screen_rect(p, box).has_point(p_pos)) {
					// inside the selection: drag moves it, a click picks / switches the handles
					press_item = pick_2d(p, p_pos);
					pending_toggle = true;
					drag_box = box;
					drag = DRAG_PENDING;
					return;
				}
			}
			Node3D *hit = pick_2d(p, p_pos);
			if (hit) {
				std::vector<Node3D *> sel;
				if (p_ctrl) {
					sel = selected_nodes();
					auto it = std::find(sel.begin(), sel.end(), hit);
					if (it != sel.end()) {
						sel.erase(it);
					} else {
						sel.push_back(hit);
					}
					set_selection(sel);
					drag = DRAG_NONE;
					return;
				}
				set_selection({ hit });
				selection_bounds(drag_box);
				drag = DRAG_PENDING;
				return;
			}
			drag = DRAG_MARQUEE;
			return;
		}
		case TOOL_BLOCK: {
			if (has_block) {
				const int h = handle_at(p, block, p_pos);
				if (h >= 0) {
					drag_box = block;
					drag_handle = h;
					drag = DRAG_BLOCK_RESIZE;
					return;
				}
				if (screen_rect(p, block).has_point(p_pos)) {
					drag_box = block;
					drag = DRAG_BLOCK_MOVE;
					return;
				}
			}
			has_block = false;
			drag = DRAG_BLOCK_NEW;
			return;
		}
		case TOOL_VERTEX: {
			// the vertex under the cursor, with the ones on top of it in this view
			HewnMesh3D *hm = nullptr;
			int hv = -1;
			float best = 7.0f * edscale();
			Vector2 hs;
			const std::vector<HewnMesh3D *> meshes = selected_meshes();
			for (HewnMesh3D *m : meshes) {
				const Transform3D G = m->get_global_transform();
				const hewn::Mesh &d = m->get_data();
				for (int i = 0; i < (int)d.pos.size(); i++) {
					const Vector2 s = p->to_screen(G.xform(d.pos[i]));
					const float dist = s.distance_to(p_pos);
					if (dist < best) {
						best = dist;
						hm = m;
						hv = i;
						hs = s;
					}
				}
			}
			if (!hm) {
				Node3D *hit = pick_2d(p, p_pos);
				if (hit && Object::cast_to<HewnMesh3D>(hit) && !is_selected(hit)) {
					set_selection({ hit });
					drag = DRAG_NONE;
					return;
				}
				drag = DRAG_MARQUEE;
				return;
			}
			grab_world = hm->get_global_transform().xform(hm->get_data().pos[hv]);
			const std::vector<int> &hsel = *vsel_of(hm);
			const bool already = std::find(hsel.begin(), hsel.end(), hv) != hsel.end();
			if (!p_ctrl && !already) {
				vsel.clear();
			}
			for (HewnMesh3D *m : meshes) {
				const Transform3D G = m->get_global_transform();
				const hewn::Mesh &d = m->get_data();
				std::vector<int> &vs = *vsel_of(m);
				for (int i = 0; i < (int)d.pos.size(); i++) {
					if (p->to_screen(G.xform(d.pos[i])).distance_to(hs) > 1.5f) {
						continue;
					}
					auto it = std::find(vs.begin(), vs.end(), i);
					if (p_ctrl && already) {
						if (it != vs.end()) {
							vs.erase(it);
						}
					} else if (it == vs.end()) {
						vs.push_back(i);
					}
				}
			}
			drag = p_ctrl ? DRAG_NONE : DRAG_VERTS_PENDING;
			return;
		}
		case TOOL_CLIP: {
			const Vector3 a = p->from_uv(Vector2(snap(press_uv.x), snap(press_uv.y)), 0.0f);
			has_clip = true;
			clip_pane = p;
			clip_a = clip_b = a;
			drag = DRAG_CLIP;
			return;
		}
	}
}

void HewnQuadView::_drag_2d(HewnQuadPane *p, const Vector2 &p_pos) {
	const Vector2 uv = p->to_uv(p_pos);
	switch (drag) {
		case DRAG_PAN:
			p->center -= Vector2(p_pos.x - last_pos.x, last_pos.y - p_pos.y) / p->zoom;
			p->sync_camera();
			break;
		case DRAG_PENDING:
			if (p_pos.distance_to(press_pos) <= 3.0f * edscale()) {
				break;
			}
			save_selection();
			if (drag_shift) {
				start_clone();
			}
			drag = DRAG_MOVE;
			[[fallthrough]];
		case DRAG_MOVE: {
			Vector2 mn, mx;
			box_uv(p, drag_box, mn, mx);
			const Vector2 d = uv - press_uv;
			// the box corner lands on the grid
			const Vector2 off(snap(mn.x + d.x) - mn.x, snap(mn.y + d.y) - mn.y);
			const Vector3 move = p->u * off.x + p->v * off.y;
			for (const Saved &s : saved) {
				if (Node3D *n = node_of(s.id)) {
					n->set_global_transform(Transform3D(s.xf.basis, s.xf.origin + move));
				}
			}
			break;
		}
		case DRAG_RESIZE:
		case DRAG_BLOCK_RESIZE: {
			Vector2 mn, mx;
			box_uv(p, drag_box, mn, mx);
			const Vector2 d = uv - press_uv;
			const float ms = min_size();
			Vector2 nmn = mn, nmx = mx;
			const int sx = HANDLE_X[drag_handle], sy = HANDLE_Y[drag_handle];
			if (sx > 0) {
				nmx.x = MAX(snap(mx.x + d.x), nmn.x + ms);
			} else if (sx < 0) {
				nmn.x = MIN(snap(mn.x + d.x), nmx.x - ms);
			}
			if (sy < 0) { // screen up = +v
				nmx.y = MAX(snap(mx.y + d.y), nmn.y + ms);
			} else if (sy > 0) {
				nmn.y = MIN(snap(mn.y + d.y), nmx.y - ms);
			}
			if (drag == DRAG_BLOCK_RESIZE) {
				block = box_from_uv(p, nmn, nmx, drag_box);
				break;
			}
			// along u: s' = k s + t (s = p.u), the same for v
			const Vector2 k((nmx.x - nmn.x) / MAX(1e-6f, mx.x - mn.x), (nmx.y - nmn.y) / MAX(1e-6f, mx.y - mn.y));
			const Vector2 t(nmn.x - k.x * mn.x, nmn.y - k.y * mn.y);
			Vector3 sc(1, 1, 1);
			sc[axis_index(p->u)] = k.x;
			sc[axis_index(p->v)] = k.y;
			const Basis S = Basis::from_scale(sc);
			const Vector3 off = p->u * t.x + p->v * t.y;
			for (const Saved &s : saved) {
				if (Node3D *n = node_of(s.id)) {
					apply_scale(n, s, S, off);
				}
			}
			break;
		}
		case DRAG_ROTATE: {
			const Vector2 c = screen_rect(p, drag_box).get_center();
			float a = -((p_pos - c).angle() - rotate_start); // screen y points down
			if (snapping()) {
				const float step = 0.2617993878f; // 15 degrees
				a = std::round(a / step) * step;
			}
			drag_angle = a;
			const Basis R(p->w, a);
			const Vector3 pivot = drag_box.get_center();
			for (const Saved &s : saved) {
				if (Node3D *n = node_of(s.id)) {
					apply_rotation(n, s, R, pivot);
				}
			}
			break;
		}
		case DRAG_BLOCK_NEW: {
			const Vector2 a(snap(press_uv.x), snap(press_uv.y)), b(snap(uv.x), snap(uv.y));
			AABB depth;
			if (!selection_bounds(depth)) {
				depth = last_block; // the depth of the last block (or of the selection)
			}
			has_block = std::fabs(a.x - b.x) > 1e-5f && std::fabs(a.y - b.y) > 1e-5f;
			if (has_block) {
				block = box_from_uv(p, a.min(b), a.max(b), depth);
			}
			break;
		}
		case DRAG_BLOCK_MOVE: {
			Vector2 mn, mx;
			box_uv(p, drag_box, mn, mx);
			const Vector2 d = uv - press_uv;
			const Vector2 off(snap(mn.x + d.x) - mn.x, snap(mn.y + d.y) - mn.y);
			block = AABB(drag_box.position + p->u * off.x + p->v * off.y, drag_box.size);
			break;
		}
		case DRAG_VERTS_PENDING:
			if (p_pos.distance_to(press_pos) <= 3.0f * edscale()) {
				break;
			}
			save_selection();
			drag = DRAG_VERTS;
			[[fallthrough]];
		case DRAG_VERTS:
			move_verts(p, p_pos);
			break;
		case DRAG_CLIP:
			clip_b = p->from_uv(Vector2(snap(uv.x), snap(uv.y)), 0.0f);
			break;
		case DRAG_CAMERA: {
			Camera3D *c = panes[0]->camera;
			const Vector2 d = uv - p->to_uv(last_pos);
			c->set_position(c->get_position() + p->u * d.x + p->v * d.y);
			break;
		}
		default:
			break;
	}
	last_pos = p_pos;
	items_frame = ~0ULL;
}

void HewnQuadView::_release_2d(HewnQuadPane *p, const Vector2 &p_pos) {
	last_pos = p_pos;
	switch (drag) {
		case DRAG_PENDING:
			// a click: the object under the cursor; on the selected one again: scale <-> rotate handles
			if (press_item && is_selected(press_item) && pending_toggle) {
				handle_rotate = !handle_rotate;
				_update_toolbar();
			} else if (press_item && !is_selected(press_item)) {
				set_selection({ press_item });
			}
			break;
		case DRAG_MOVE:
			commit_saved(clones.empty() ? "Move" : "Copy");
			break;
		case DRAG_RESIZE:
			commit_saved("Resize");
			break;
		case DRAG_ROTATE:
			commit_saved("Rotate");
			break;
		case DRAG_VERTS:
			commit_saved("Move Vertices");
			break;
		case DRAG_MARQUEE: {
			const Rect2 r = Rect2(press_pos, p_pos - press_pos).abs();
			if (tool == TOOL_VERTEX) {
				if (r.get_area() > 9.0f) {
					select_verts_in(p, r, drag_ctrl);
				} else if (!drag_ctrl) {
					vsel.clear();
				}
				break;
			}
			if (r.size.x < 3.0f && r.size.y < 3.0f) {
				if (!drag_ctrl) {
					set_selection({});
				}
				break;
			}
			std::vector<Node3D *> sel = drag_ctrl ? selected_nodes() : std::vector<Node3D *>();
			for (const Item &it : get_items()) {
				if (r.encloses(screen_rect(p, it.box)) && std::find(sel.begin(), sel.end(), it.node) == sel.end()) {
					sel.push_back(it.node);
				}
			}
			set_selection(sel);
			break;
		}
		case DRAG_CLIP:
			if (clip_a.distance_to(clip_b) < 1e-5f) {
				has_clip = false;
			}
			break;
		default:
			break;
	}
	drag = DRAG_NONE;
	saved.clear();
	clones.clear();
	press_item = nullptr;
	items_frame = ~0ULL;
}

void HewnQuadView::_cancel_drag() {
	switch (drag) {
		case DRAG_MOVE:
		case DRAG_RESIZE:
		case DRAG_ROTATE:
		case DRAG_VERTS:
			restore_saved();
			break;
		case DRAG_BLOCK_NEW:
			has_block = false;
			break;
		case DRAG_BLOCK_MOVE:
		case DRAG_BLOCK_RESIZE:
			block = drag_box;
			break;
		case DRAG_CLIP:
			has_clip = false;
			break;
		case DRAG_FLY:
			Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
			break;
		default:
			break;
	}
	if (!clones.empty()) {
		std::vector<Node3D *> originals;
		for (uint64_t id : clone_sources) {
			if (Node3D *n = node_of(id)) {
				originals.push_back(n);
			}
		}
		for (Node *c : clones) {
			if (Node *parent = c->get_parent()) {
				parent->remove_child(c);
			}
			memdelete(c);
		}
		set_selection(originals);
	}
	drag = DRAG_NONE;
	saved.clear();
	clones.clear();
	items_frame = ~0ULL;
}

void HewnQuadView::_input_3d(HewnQuadPane *p, const Ref<InputEvent> &p_event) {
	Camera3D *c = p->camera;
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid()) {
		const MouseButton btn = mb->get_button_index();
		if (mb->is_pressed()) {
			p->overlay->grab_focus();
		}
		if (btn == MOUSE_BUTTON_RIGHT) {
			if (mb->is_pressed() && drag == DRAG_NONE) {
				drag = DRAG_FLY;
				drag_pane = p;
				Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_CAPTURED);
			} else if (!mb->is_pressed() && drag == DRAG_FLY) {
				drag = DRAG_NONE;
				Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
			}
		} else if (btn == MOUSE_BUTTON_MIDDLE) {
			if (mb->is_pressed() && drag == DRAG_NONE) {
				drag = mb->is_shift_pressed() ? DRAG_PAN_3D : DRAG_ORBIT;
				drag_pane = p;
			} else if (!mb->is_pressed() && (drag == DRAG_ORBIT || drag == DRAG_PAN_3D)) {
				drag = DRAG_NONE;
			}
		} else if ((btn == MOUSE_BUTTON_WHEEL_UP || btn == MOUSE_BUTTON_WHEEL_DOWN) && mb->is_pressed()) {
			const bool in = btn == MOUSE_BUTTON_WHEEL_UP;
			if (drag == DRAG_FLY) {
				fly_speed = CLAMP(fly_speed * (in ? 1.25f : 0.8f), 0.1f, 500.0f);
			} else {
				const float step = orbit_distance * 0.15f * (in ? 1.0f : -1.0f);
				orbit_distance = MAX(0.2f, orbit_distance - step);
				Transform3D t = c->get_transform();
				t.origin -= t.basis.get_column(2) * step;
				c->set_transform(t);
			}
		} else if (btn == MOUSE_BUTTON_LEFT && mb->is_pressed() && drag == DRAG_NONE) {
			Node3D *hit = pick_3d(p, mb->get_position());
			if (mb->is_ctrl_pressed() || mb->is_meta_pressed()) {
				if (hit) {
					std::vector<Node3D *> sel = selected_nodes();
					auto it = std::find(sel.begin(), sel.end(), hit);
					if (it != sel.end()) {
						sel.erase(it);
					} else {
						sel.push_back(hit);
					}
					set_selection(sel);
				}
			} else {
				set_selection(hit ? std::vector<Node3D *>{ hit } : std::vector<Node3D *>());
			}
		}
		p->overlay->accept_event();
		return;
	}
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		const Vector2 rel = mm->get_relative();
		Transform3D t = c->get_transform();
		if (drag == DRAG_FLY) {
			Basis b = Basis(Vector3(0, 1, 0), -rel.x * 0.004f) * t.basis;
			const Basis pitched = b * Basis(Vector3(1, 0, 0), -rel.y * 0.004f);
			if (std::fabs(pitched.get_column(2).y) < 0.995f) {
				b = pitched;
			}
			t.basis = b.orthonormalized();
			c->set_transform(t);
		} else if (drag == DRAG_ORBIT) {
			const Vector3 pivot = t.origin - t.basis.get_column(2) * orbit_distance;
			Basis b = Basis(Vector3(0, 1, 0), -rel.x * 0.006f) * t.basis;
			const Basis pitched = Basis(b.get_column(0).normalized(), -rel.y * 0.006f) * b;
			if (std::fabs(pitched.get_column(2).y) < 0.995f) {
				b = pitched;
			}
			t.basis = b.orthonormalized();
			t.origin = pivot + t.basis.get_column(2) * orbit_distance;
			c->set_transform(t);
		} else if (drag == DRAG_PAN_3D) {
			t.origin += (-t.basis.get_column(0) * rel.x + t.basis.get_column(1) * rel.y) * orbit_distance * 0.002f;
			c->set_transform(t);
		}
		if (drag != DRAG_NONE) {
			p->overlay->accept_event();
		}
		return;
	}
	Ref<InputEventKey> k = p_event;
	if (k.is_valid() && drag == DRAG_FLY) {
		p->overlay->accept_event(); // WASD / QE fly (read in _process)
		return;
	}
	if (_shortcut(p, p_event)) {
		p->overlay->accept_event();
	}
}

// ================================================================== drawing

void HewnQuadView::draw_grid(HewnQuadPane *p) {
	const Vector2 s = p->get_size();
	p->draw_rect(Rect2(Vector2(), s), BG_COLOR);
	const Vector2 lo = p->to_uv(Vector2(0, s.y)), hi = p->to_uv(Vector2(s.x, 0));
	float step = grid;
	for (int i = 0; i < 40 && step * p->zoom < 8.0f; i++) {
		step *= 2.0f; // too dense: every other line
	}
	const float major = step < 1.0f ? 1.0f : step * 8.0f;
	const int64_t ratio = MAX((int64_t)1, (int64_t)std::llround(major / step));
	PackedVector2Array minor_lines, major_lines;
	const float hx = s.x * 0.5f, hy = s.y * 0.5f;
	for (int64_t i = (int64_t)std::floor(lo.x / step); i <= (int64_t)std::ceil(hi.x / step); i++) {
		const float x = hx + (i * step - p->center.x) * p->zoom;
		PackedVector2Array &a = i % ratio == 0 ? major_lines : minor_lines;
		a.push_back(Vector2(x, 0));
		a.push_back(Vector2(x, s.y));
	}
	for (int64_t i = (int64_t)std::floor(lo.y / step); i <= (int64_t)std::ceil(hi.y / step); i++) {
		const float y = hy - (i * step - p->center.y) * p->zoom;
		PackedVector2Array &a = i % ratio == 0 ? major_lines : minor_lines;
		a.push_back(Vector2(0, y));
		a.push_back(Vector2(s.x, y));
	}
	if (!minor_lines.is_empty()) {
		p->draw_multiline(minor_lines, Color(1, 1, 1, 0.06f));
	}
	if (!major_lines.is_empty()) {
		p->draw_multiline(major_lines, Color(1, 1, 1, 0.14f));
	}
	// the axes through the origin
	const Vector2 o = p->to_screen(Vector3());
	Color cu = hw_editor::AXIS_COLORS[axis_index(p->u)], cv = hw_editor::AXIS_COLORS[axis_index(p->v)];
	cu.a = cv.a = 0.55f;
	p->draw_line(Vector2(0, o.y), Vector2(s.x, o.y), cu);
	p->draw_line(Vector2(o.x, 0), Vector2(o.x, s.y), cv);
}

void HewnQuadView::_pane_draw(HewnQuadPane *p) {
	if (p->is_2d()) {
		_draw_2d(p);
	} else {
		_draw_3d(p);
	}
	Control *o = p->overlay;
	Ref<Font> font = o->get_theme_default_font();
	const int fs = o->get_theme_default_font_size();
	if (font.is_valid()) {
		const String t = p->title();
		const Vector2 pos(8, 6 + font->get_ascent(fs));
		o->draw_string(font, pos + Vector2(1, 1), t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color(0, 0, 0, 0.7f));
		o->draw_string(font, pos, t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color(1, 1, 1, 0.85f));
	}
	if (hover_pane == p) {
		o->draw_rect(Rect2(Vector2(), o->get_size()), Color(1, 1, 1, 0.3f), false, 1.0f);
	}
}

void HewnQuadView::_draw_box_2d(HewnQuadPane *p, const AABB &p_box, const Color &p_color, bool p_handles, bool p_rotate) {
	Control *o = p->overlay;
	const float es = edscale();
	const Rect2 r = screen_rect(p, p_box);
	const Vector2 c[4] = { r.position, Vector2(r.get_end().x, r.position.y), r.get_end(), Vector2(r.position.x, r.get_end().y) };
	for (int i = 0; i < 4; i++) {
		o->draw_dashed_line(c[i], c[(i + 1) % 4], p_color, 1.0f, 4.0f * es);
	}
	if (p_handles) {
		const float hs = 3.5f * es;
		for (int h = 0; h < 8; h++) {
			if (p_rotate && h % 2 == 1) {
				continue;
			}
			// slightly outside the box, like the classic editors
			const Vector2 hp = handle_pos(r, h) + Vector2(HANDLE_X[h], HANDLE_Y[h]) * hs;
			if (p_rotate) {
				o->draw_circle(hp, hs + 1.0f, Color(0, 0, 0, 0.8f));
				o->draw_circle(hp, hs, p_color);
			} else {
				o->draw_rect(Rect2(hp - Vector2(hs + 1, hs + 1), Vector2(hs + 1, hs + 1) * 2.0f), Color(0, 0, 0, 0.8f));
				o->draw_rect(Rect2(hp - Vector2(hs, hs), Vector2(hs, hs) * 2.0f), p_color);
			}
		}
	}
	// sizes along the sides
	Ref<Font> font = o->get_theme_default_font();
	if (font.is_valid()) {
		const int fs = MAX(8, o->get_theme_default_font_size() - 2);
		Vector2 mn, mx;
		box_uv(p, p_box, mn, mx);
		const String wt = String::num(mx.x - mn.x, 3), ht = String::num(mx.y - mn.y, 3);
		const Vector2 ws = font->get_string_size(wt, HORIZONTAL_ALIGNMENT_LEFT, -1, fs);
		const Vector2 hsz = font->get_string_size(ht, HORIZONTAL_ALIGNMENT_LEFT, -1, fs);
		o->draw_string(font, Vector2(r.get_center().x - ws.x * 0.5f, r.position.y - 10.0f * es), wt, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, p_color);
		o->draw_string(font, Vector2(r.position.x - hsz.x - 10.0f * es, r.get_center().y + fs * 0.35f), ht, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, p_color);
	}
}

void HewnQuadView::_draw_clip(HewnQuadPane *p) {
	Control *o = p->overlay;
	const float es = edscale();
	const Vector2 a = p->to_screen(clip_a), b = p->to_screen(clip_b);
	const Vector3 d = clip_b - clip_a;
	if (d.length() > 1e-5f) {
		const Vector3 normal = d.cross(p->w).normalized();
		// what stays (white) and what goes (red); both parts kept: white and blue
		for (HewnMesh3D *m : selected_meshes()) {
			const Transform3D G = m->get_global_transform();
			const Vector3 lp = G.affine_inverse().xform(clip_a);
			const Vector3 ln = G.basis.transposed().xform(normal).normalized();
			for (int side = 0; side < 2; side++) {
				hewn::Mesh piece = m->get_data();
				hewn::ops::bisect(piece, {}, lp, ln, side == 0, side == 1, false);
				PackedVector3Array lines;
				mesh_edges(piece, G, lines);
				PackedVector2Array sl;
				for (const Vector3 &q : lines) {
					sl.push_back(p->to_screen(q));
				}
				const bool kept = clip_mode == CLIP_BOTH || (clip_mode == CLIP_FRONT) == (side == 0);
				const Color col = !kept ? Color(1.0f, 0.3f, 0.25f, 0.6f) : (clip_mode == CLIP_BOTH && side == 1 ? Color(0.45f, 0.75f, 1.0f) : Color(1, 1, 1));
				if (!sl.is_empty()) {
					o->draw_multiline(sl, col);
				}
			}
		}
		// the line through the whole view, the drawn part solid, an arrow to the front side
		const Vector2 dir = (b - a).normalized();
		const float far = (o->get_size().length() + a.length()) * 2.0f;
		o->draw_dashed_line(a - dir * far, b + dir * far, Color(1, 0.85f, 0.3f, 0.6f), 1.0f, 5.0f * es);
		o->draw_line(a, b, BLOCK_COLOR, 2.0f);
		const Vector2 mid = (a + b) * 0.5f;
		const Vector2 n2 = (p->to_screen(clip_a + normal) - a).normalized();
		o->draw_line(mid, mid + n2 * 16.0f * es, BLOCK_COLOR, 2.0f);
		o->draw_line(mid + n2 * 16.0f * es, mid + n2 * 11.0f * es + n2.orthogonal() * 4.0f * es, BLOCK_COLOR, 2.0f);
		o->draw_line(mid + n2 * 16.0f * es, mid + n2 * 11.0f * es - n2.orthogonal() * 4.0f * es, BLOCK_COLOR, 2.0f);
	}
	const float hs = 3.5f * es;
	o->draw_rect(Rect2(a - Vector2(hs, hs), Vector2(hs, hs) * 2.0f), BLOCK_COLOR);
	o->draw_rect(Rect2(b - Vector2(hs, hs), Vector2(hs, hs) * 2.0f), BLOCK_COLOR);
}

void HewnQuadView::_draw_2d(HewnQuadPane *p) {
	Control *o = p->overlay;
	const float es = edscale();
	// the selected objects in red (the rest is the wireframe render)
	const bool clipping = tool == TOOL_CLIP && has_clip && clip_pane == p;
	PackedVector2Array lines;
	for (Node3D *n : selected_nodes()) {
		const Item *it = item_of(n);
		if (!it || (clipping && it->hewn)) {
			continue;
		}
		PackedVector3Array wl;
		world_edges(*it, wl);
		for (const Vector3 &q : wl) {
			lines.push_back(p->to_screen(q));
		}
	}
	if (!lines.is_empty()) {
		o->draw_multiline(lines, SEL_COLOR, 1.0f);
	}
	if (tool == TOOL_VERTEX) {
		const float hs = 3.0f * es;
		for (HewnMesh3D *m : selected_meshes()) {
			const Transform3D G = m->get_global_transform();
			const hewn::Mesh &d = m->get_data();
			const std::vector<int> &vs = *vsel_of(m);
			std::unordered_set<int> on(vs.begin(), vs.end());
			for (int pass = 0; pass < 2; pass++) { // selected ones on top
				for (int i = 0; i < (int)d.pos.size(); i++) {
					if ((on.count(i) != 0) != (pass == 1)) {
						continue;
					}
					const Vector2 s = p->to_screen(G.xform(d.pos[i]));
					o->draw_rect(Rect2(s - Vector2(hs + 1, hs + 1), Vector2(hs + 1, hs + 1) * 2.0f), Color(0, 0, 0, 0.8f));
					o->draw_rect(Rect2(s - Vector2(hs, hs), Vector2(hs, hs) * 2.0f), pass ? SEL_COLOR : Color(1, 1, 1));
				}
			}
		}
	}
	AABB box;
	if (tool == TOOL_SELECT && selection_bounds(box)) {
		_draw_box_2d(p, box, BOUNDS_COLOR, true, handle_rotate);
	}
	if (tool == TOOL_BLOCK && has_block) {
		_draw_box_2d(p, block, BLOCK_COLOR, true, false);
	}
	if (clipping) {
		_draw_clip(p);
	}
	if (drag == DRAG_MARQUEE && drag_pane == p) {
		const Rect2 r = Rect2(press_pos, last_pos - press_pos).abs();
		o->draw_rect(r, Color(1, 1, 1, 0.07f));
		o->draw_rect(r, Color(1, 1, 1, 0.7f), false, 1.0f);
	}
	if (drag == DRAG_ROTATE && drag_pane == p) {
		Ref<Font> font = o->get_theme_default_font();
		if (font.is_valid()) {
			o->draw_string(font, last_pos + Vector2(14, -8) * es, String::num(Math::rad_to_deg(drag_angle), 1) + String::chr(0xB0),
					HORIZONTAL_ALIGNMENT_LEFT, -1, o->get_theme_default_font_size(), BOUNDS_COLOR);
		}
	}
	// the 3D camera: a dot with its view direction (drag it to move the camera)
	Camera3D *c = panes[0]->camera;
	const Vector3 cp = c->get_position();
	const Vector2 cs = p->to_screen(cp);
	Vector2 fd = p->to_screen(cp - c->get_transform().basis.get_column(2)) - cs;
	if (fd.length() > 1e-3f) {
		fd = fd.normalized() * 18.0f * es;
		o->draw_line(cs, cs + fd, CAMERA_COLOR, 2.0f);
	}
	o->draw_circle(cs, 4.5f * es, Color(0, 0, 0, 0.8f));
	o->draw_circle(cs, 3.5f * es, CAMERA_COLOR);
}

void HewnQuadView::_draw_3d(HewnQuadPane *p) {
	Control *o = p->overlay;
	Camera3D *c = p->camera;
	auto draw_lines = [&](const PackedVector3Array &p_lines, const Color &p_color) {
		PackedVector2Array sl;
		for (int64_t i = 0; i + 1 < p_lines.size(); i += 2) {
			if (c->is_position_behind(p_lines[i]) || c->is_position_behind(p_lines[i + 1])) {
				continue;
			}
			sl.push_back(c->unproject_position(p_lines[i]));
			sl.push_back(c->unproject_position(p_lines[i + 1]));
		}
		if (!sl.is_empty()) {
			o->draw_multiline(sl, p_color, 1.0f);
		}
	};
	PackedVector3Array sel;
	for (Node3D *n : selected_nodes()) {
		if (const Item *it = item_of(n)) {
			world_edges(*it, sel);
		}
	}
	draw_lines(sel, Color(1.0f, 0.55f, 0.15f));
	if (tool == TOOL_BLOCK && has_block) {
		Item bi;
		bi.box = block;
		PackedVector3Array bl;
		world_edges(bi, bl);
		draw_lines(bl, BLOCK_COLOR);
	}
	if (drag == DRAG_FLY) {
		Ref<Font> font = o->get_theme_default_font();
		if (font.is_valid()) {
			const int fs = o->get_theme_default_font_size();
			o->draw_string(font, Vector2(8, o->get_size().y - 8), T("Fly speed") + " " + String::num(fly_speed, 1), HORIZONTAL_ALIGNMENT_LEFT, -1, fs,
					Color(1, 1, 1, 0.8f));
		}
	}
}

// ================================================================== plugin

void HewnQuadPlugin::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		view = memnew(HewnQuadView);
		view->setup(HewnEditorPlugin::singleton);
		EditorInterface::get_singleton()->get_editor_main_screen()->add_child(view);
		view->shown(false);
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		if (view) {
			view->queue_free();
			view = nullptr;
		}
	}
}

Ref<Texture2D> HewnQuadPlugin::_get_plugin_icon() const {
	Ref<Theme> th = EditorInterface::get_singleton()->get_editor_theme();
	if (th.is_valid() && th->has_icon("Panels4", "EditorIcons")) {
		return th->get_icon("Panels4", "EditorIcons");
	}
	return Ref<Texture2D>();
}

void HewnQuadPlugin::_make_visible(bool p_visible) {
	screen_visible = p_visible;
	if (view) {
		view->plugin = HewnEditorPlugin::singleton; // (the Hewn plugin may have been switched on after this one)
		view->shown(p_visible);
	}
}

// While our screen is open we take 3D nodes, so that selecting one doesn't switch to the 3D screen.
bool HewnQuadPlugin::_handles(Object *p_object) const {
	return screen_visible && Object::cast_to<Node3D>(p_object) != nullptr;
}
