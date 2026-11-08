// Hewn editor: plugin, viewport gizmo, docks.
#pragma once

#include <functional>
#include <memory>
#include <unordered_map>

#include "../hewn_mesh.h"

namespace hw_editor {
struct TriBVH;
}
void hw_gizmo_clear_cache();
// How the edited mesh is drawn (Editor Settings > Hewn > Viewport).
struct HwMeshStyle {
	godot::Color vert = godot::Color(0, 0, 0, 1);
	godot::Color vert_sel = godot::Color::hex(0xff7900ff);
	godot::Color edge = godot::Color(0, 0, 0, 1);
	godot::Color edge_sel = godot::Color::hex(0xff9900ff);
	godot::Color face_sel = godot::Color::hex(0xffa30033);
	float vert_size = 5.0f;
	godot::Color origin = godot::Color(1.0f, 0.55f, 0.1f, 1.0f); // the objects' origins (dots)
	float origin_size = 7.0f; // (diameter, pixels)
	// the Shading menu's Single Color: its color (a face turned to the view shows it a bit darker), the light
	// fixed in the world (turned about Y) instead of following the view
	godot::Color single = godot::Color::hex(0xc5c7c9ff);
	bool world_light = false;
	float world_light_angle = 0.0f;
	// Single Color's cavity: ridges lighter, valleys darker; found in the world (the depth around), on the screen
	// (the normals around) or both
	bool cavity = false;
	float cavity_ridge = 1.0f;
	float cavity_valley = 1.0f;
	int cavity_type = 1; // 0 world, 1 screen, 2 both
};
HwMeshStyle &hw_mesh_style();

#include <map>
#include <set>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/accept_dialog.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/curve.hpp>
#include <godot_cpp/classes/curve3d.hpp>
#include <godot_cpp/classes/path3d.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/color_picker_button.hpp>
#include <godot_cpp/classes/confirmation_dialog.hpp>
#include <godot_cpp/classes/editor_spin_slider.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/editor_dock.hpp>
#include <godot_cpp/classes/editor_file_dialog.hpp>
#include <godot_cpp/classes/editor_node3d_gizmo.hpp>
#include <godot_cpp/classes/editor_node3d_gizmo_plugin.hpp>
#include <godot_cpp/classes/editor_export_platform.hpp>
#include <godot_cpp/classes/editor_export_plugin.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/editor_resource_picker.hpp>
#include <godot_cpp/classes/h_flow_container.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_slider.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/popup_panel.hpp>
#include <godot_cpp/classes/menu_button.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/tab_container.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include <unordered_set>

namespace godot {

class HewnEditorPlugin;
class HewnTextureBrowser;
class HewnPrefabBrowser;

// ------------------------------------------------------------------ gizmo

class HewnGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(HewnGizmoPlugin, EditorNode3DGizmoPlugin)

	Ref<ShaderMaterial> line_material;
	Ref<ShaderMaterial> point_material;
	Ref<ShaderMaterial> face_material;

protected:
	static void _bind_methods() {}

public:
	HewnEditorPlugin *plugin = nullptr;

	bool _has_gizmo(Node3D *p_node) const override;
	String _get_gizmo_name() const override;
	int32_t _get_priority() const override;
	void _redraw(const Ref<EditorNode3DGizmo> &p_gizmo) override;

	HewnGizmoPlugin();
};

// Outlines and click targets of overlays, entities and triggers (hw_entity_gizmo.cpp).
class HewnEntityGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(HewnEntityGizmoPlugin, EditorNode3DGizmoPlugin)

protected:
	static void _bind_methods() {}

public:
	bool _has_gizmo(Node3D *p_node) const override;
	String _get_gizmo_name() const override;
	int32_t _get_priority() const override;
	void _redraw(const Ref<EditorNode3DGizmo> &p_gizmo) override;

	HewnEntityGizmoPlugin();
};

// ------------------------------------------------------------------ UV canvas

class HewnUVCanvas : public Control {
	GDCLASS(HewnUVCanvas, Control)

public:
	enum UVMode {
		UV_VERTEX = 1,
		UV_EDGE = 2,
		UV_FACE = 3,
	};
	enum Xform {
		XF_NONE,
		XF_MOVE,
		XF_ROTATE,
		XF_SCALE,
	};

private:
	Vector2 pan = Vector2(40, 40);
	float zoom = 220.0f;
	bool panning = false;
	Vector2 drag_last;
	Vector2 mouse;
	// The UV selection: face corners, (face << 32) | corner. Dropped when the mesh
	// changes shape.
	std::set<uint64_t> sel;
	uint64_t mesh_key = 0;
	// a press: a click, a drag of the selection (tweak) or a box
	bool pressed = false;
	Vector2 press_pos;
	bool press_shift = false, press_ctrl = false;
	bool boxing = false;
	// G / R / S
	Xform xf = XF_NONE;
	bool xf_tweak = false;
	int xf_axis = -1; // -1 free, 0 U, 1 V
	Vector2 xf_mouse0, xf_pivot;
	std::vector<std::pair<uint64_t, Vector2>> xf_start;
	String xf_info;
	bool xf_snapped = false; // G snapped onto a UV vertex / edge (its mark is drawn there)
	Vector2 xf_snap_uv;
	int xf_snap_kind = 0;
	Button *mode_buttons[4] = {};
	Button *snap_button = nullptr; // the viewport's snapping options, here too
	void on_snap_button();
	CheckBox *sync_check = nullptr;

	Vector2 to_screen(const Vector2 &uv) const { return pan + uv * zoom; }
	Vector2 to_uv(const Vector2 &p) const { return (p - pan) / zoom; }
	HewnMesh3D *_mesh();
	std::vector<int> _shown_faces(HewnMesh3D *p_n) const;
	Vector2 _loop_uv(HewnMesh3D *p_n, int p_face, int p_corner) const;
	void _set_loop_uv(HewnMesh3D *p_n, int p_face, int p_corner, const Vector2 &p_uv);
	void _sticky(HewnMesh3D *p_n, const std::vector<int> &p_faces, std::set<uint64_t> &r_loops) const;
	bool _pick(HewnMesh3D *p_n, const Vector2 &p_pos, std::set<uint64_t> &r_loops) const;
	void _box_select(HewnMesh3D *p_n, const Rect2 &p_rect, bool p_remove);
	std::vector<std::vector<int>> _islands(HewnMesh3D *p_n, const std::vector<int> &p_faces) const;
	void _select_linked(HewnMesh3D *p_n, bool p_under_mouse);
	bool _face_selected(HewnMesh3D *p_n, int p_face) const;
	std::vector<uint64_t> _selected_loops(HewnMesh3D *p_n) const;
	void _start_xf(Xform p_kind, bool p_tweak);
	void _update_xf(bool p_ctrl);
	void _end_xf(bool p_apply);
	void _apply_simple(int p_what); // rotate 90 / flip U / flip V / fit / weld around the selection
	void _pack(bool p_pack);
	float _grid() const;
	Ref<Texture2D> _texture(HewnMesh3D *p_n, Vector2 &r_tiling) const;

protected:
	static void _bind_methods();

public:
	HewnEditorPlugin *plugin = nullptr;
	int uv_mode = UV_FACE;
	int grid_div = 8; // the grid and the increment snap: 1 / grid_div (0: texture pixels)
	bool show_texture = true;
	bool sync_selection = false; // UV selection follows the 3D one (and the other way round)
	bool pack_rotate = true, pack_scale = true;
	float pack_margin = 0.01f;

	void build_header(Control *p_parent);
	void set_uv_mode(int p_mode);
	void on_menu(int p_id);
	void _draw() override;
	void _gui_input(const Ref<InputEvent> &p_event) override;
	void frame_selection();
	void frame_all();
	void feed_input(const Ref<InputEvent> &p_event) { _gui_input(p_event); } // (the self-test)
};

// ------------------------------------------------------------------ drag field (Last Operation panel)

class HewnDragField : public LineEdit {
	GDCLASS(HewnDragField, LineEdit)

	double value = 0.0;
	double min_value = -1e9;
	double max_value = 1e9;
	double step = 0.001;
	bool is_int = false;
	bool pressed = false;
	bool dragging = false;
	double drag_start = 0.0;
	float drag_accum = 0.0f;
	bool skip_motion = false;
	bool typing = false; // entered by a click; until then presses start a drag
	Vector2 press_window_pos;

	void _show();
	void _assign(double p_value, bool p_emit);
	bool _evaluate(const String &p_text, double &r_value) const;

public:
	// A typed number or expression ("0,5", "1/2", "2*3", "sqrt(2)"): decimal commas are points, whole
	// numbers divide as floats.
	static bool eval_expression(const String &p_text, double &r_value);
	// Godot's SpinBox divides whole numbers as whole numbers (1/5 is 0): its typed text goes through
	// eval_expression instead
	static void hook_spinbox(SpinBox *p_spin);

private:
	void _on_submitted(const String &p_text);
	void _on_focus_exited();

protected:
	static void _bind_methods();

public:
	void setup(double p_min, double p_max, double p_step, bool p_int, double p_value);
	double get_value() const { return value; }
	void set_value(double p_value) { _assign(p_value, true); } // (like a SpinBox)
	void set_value_no_signal(double p_value) { _assign(p_value, false); }
	void _gui_input(const Ref<InputEvent> &p_event) override;
	// fields being dragged now (the edits made meanwhile become one undo step)
	static int drag_count;
	HewnDragField();
	~HewnDragField();
};

// ------------------------------------------------------------------ export (hw_export.cpp)

class HewnExportPlugin : public EditorExportPlugin {
	GDCLASS(HewnExportPlugin, EditorExportPlugin)
	String saved_list; // (the extension list while an export runs)

protected:
	static void _bind_methods();

public:
	String _get_name() const override;
	bool _begin_customize_scenes(const Ref<EditorExportPlatform> &p_platform, const PackedStringArray &p_features) const override;
	uint64_t _get_customization_configuration_hash() const override;
	Node *_customize_scene(Node *p_scene, const String &p_path) override;
	void _export_file(const String &p_path, const String &p_type, const PackedStringArray &p_features) override;
	void _export_begin(const PackedStringArray &p_features, bool p_is_debug, const String &p_path, uint32_t p_flags) override;
	void _export_end() override;
};

// ------------------------------------------------------------------ revert arrow (hw_revert.cpp)

class HewnRevertButton : public Button {
	GDCLASS(HewnRevertButton, Button)
	ObjectID field;
	Variant def;
	bool shown = false;
	bool field_visible = true;
	void _refresh();
	void _on_pressed();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static Variant field_value(Control *p_field);
	bool differs() const;
	void setup(Control *p_field, const Variant &p_default);
	HewnRevertButton();
};

// ------------------------------------------------------------------ main dock

class HewnDock : public EditorDock {
	GDCLASS(HewnDock, EditorDock)
	friend class HewnUVCanvas;
	friend class HewnEditorPlugin;
	static void relax_widths(Node *p_node);

	HewnEditorPlugin *plugin = nullptr;
	TabContainer *tabs = nullptr;
	Label *status = nullptr;
	Button *mode_buttons[4] = {};
	Button *snap_target_button = nullptr;
	VBoxContainer *vertex_ops = nullptr;
	VBoxContainer *edge_ops = nullptr;
	VBoxContainer *face_ops = nullptr;
	VBoxContainer *ei_tab_body = nullptr; // the Inspector tab (after UV): the Element Inspector
	VBoxContainer *material_slots = nullptr;
	EditorResourcePicker *material_picker = nullptr;
	Label *material_hint = nullptr;
	ColorPickerButton *color_picker = nullptr;

	// the texture of the selection and its material (hw_matpanel.cpp)
	Label *tex_title = nullptr;
	TextureRect *tex_preview = nullptr;
	VBoxContainer *mat_props = nullptr;
	Button *filter_closest = nullptr;
	Button *filter_linear = nullptr;
	CheckBox *tex_alpha = nullptr;
	CheckBox *tex_vcolor = nullptr;
	EditorSpinSlider *mat_rough = nullptr;
	EditorSpinSlider *mat_metal = nullptr;
	EditorSpinSlider *mat_emit = nullptr;
	EditorSpinSlider *mat_spec = nullptr;
	ColorPickerButton *mat_emit_color = nullptr;
	ConfirmationDialog *bleed_dialog = nullptr;
	SpinBox *box_scale = nullptr;
	CheckBox *uv_lock_button = nullptr;
	Button *uv_lock_tool = nullptr; // (the same in the toolbar)
	Button *default_closest = nullptr;
	Button *default_linear = nullptr;
	Ref<BaseMaterial3D> shown_material;
	Ref<Material> shown_any; // (also a shader material)
	Button *mat_edit = nullptr;
	HBoxContainer *mapping_row = nullptr;
	Label *mapping_label = nullptr;
	bool mat_syncing = false;
	void _on_mat_edit();
	void _on_repair_mapping();
	void _on_fix_mapping();
	void _on_edit_default();
	void _build_texture_section(VBoxContainer *p_parent);
	void _refresh_texture_section();
	Ref<BaseMaterial3D> _selected_material(bool &r_mixed, Ref<Material> &r_any) const;
	void _set_material_props(const String &p_action, const Dictionary &p_props);
	void _save_material(const Ref<Material> &p_material);
	void _on_mat_filter(bool p_nearest);
	void _on_mat_flag(bool p_on, const String &p_which);
	void _on_mat_value(double p_value, const String &p_prop);
	void _on_mat_emit_color(const Color &p_color);
	void _on_fix_bleed();
	void _on_fix_bleed_confirmed();
	void _on_box_project();
	void _on_uv_box_project(); // (the UV tab's own Box Projection and scale)
	SpinBox *uv_box_scale = nullptr;
	void _on_uv_lock(bool p_on);
	void _on_default_filter(bool p_nearest);

	// UV params
	Button *uv_scale_link = nullptr;
	Button *uv_offset_link = nullptr;
	Vector2 uv_last_scale, uv_last_offset; // (the linked pair's other value follows by these)
	void _on_uv_link_toggled(bool p_on, Button *p_button);
	void _on_uv_pair(double p_value, bool p_is_u, bool p_ratio);
	SpinBox *uv_scale_u = nullptr;
	SpinBox *uv_scale_v = nullptr;
	SpinBox *uv_offset_u = nullptr;
	SpinBox *uv_offset_v = nullptr;
	SpinBox *uv_rotation = nullptr;
	OptionButton *uv_fill = nullptr;
	CheckBox *uv_world = nullptr;
	CheckBox *uv_flip_u = nullptr;
	CheckBox *uv_flip_v = nullptr;
	CheckBox *uv_swap = nullptr;
	bool syncing = false;

	void _build_create_tab();
	void _build_place_sections(VBoxContainer *p_tab); // (hw_place_ui.cpp) entities, decals and overlays
	void _rebuild_entities();
	void _build_edit_tab();
	void _build_object_tab();
	void _build_material_tab();
	void _build_uv_tab();
	void _build_toolbar();
	void _on_toolbar_toggled(bool p_unused);
	void _on_toolbar_input(const Ref<InputEvent> &p_event);
	void _popup_under(PopupPanel *p_popup, Control *p_anchor);
	void _on_orientation(int p_id);
	void _rebuild_orient_menus();
	void _on_add_shape(int p_type);
	void _on_add_button();
	Button *add_menu = nullptr;
	std::vector<String> bar_ops; // ops of the Mesh / Object menus, by item id
	void _on_bar_op(int p_id);
	void _on_tool_button(int p_tool);
	void _on_collapse_tools();
	void _on_collapse_toolbar();
	Button *toolbar_collapse = nullptr;
	void _build_tool_column();
	void _on_add_orientation();
	void _on_pivot(int p_id);
	void _on_snap_target(int p_index);
	void _update_snap_targets();
	void _on_xray(bool p_on);
	void _on_toolbar_selected(int p_unused);
	void _on_toolbar_value(double p_unused);
	void _on_mode_button(int p_mode);
	Dictionary icon_cache;
	MenuButton *orient_menu = nullptr;
	MenuButton *pivot_menu = nullptr;
	PopupPanel *snap_popup = nullptr;

	Button *snap_target_buttons[7] = {};
	void _on_uv_changed(double p_unused);
	void _on_uv_toggled(bool p_unused);
	void _on_uv_fill(int p_unused);
	void _on_material_picked(const Ref<Resource> &p_res);

protected:
	static void _bind_methods();

public:
	SpinBox *cutter_sides = nullptr;
	OptionButton *cutter_shape = nullptr;
	OptionButton *cutter_op = nullptr;
	CheckBox *cutter_through = nullptr;
	SpinBox *poly_height = nullptr;
	HewnDragField *snap_move = nullptr; // (drag with the mouse, click to type)
	HewnDragField *snap_step_field = nullptr; // the same step, on the header next to the snap menu
	void _on_snap_step_field(double p_value);
	void _on_snap_move_changed(double p_value);
	HewnDragField *snap_rotate = nullptr; // (drag with the mouse, click to type)
	HewnDragField *snap_scale = nullptr; // (drag with the mouse, click to type)
	Button *snap_enabled = nullptr; // Godot's own snap (magnet) toggle, found by the plugin
	Button *snap_fallback = nullptr; // used when Godot's button can't be found
	CheckBox *gizmo_move = nullptr;
	CheckBox *gizmo_rotate = nullptr;
	CheckBox *gizmo_scale = nullptr;
	OptionButton *gizmo_orient = nullptr; // 0 = transform orientation, 1.. = ORIENT_* + 1
	Button *overlays = nullptr;
	CheckBox *ov_cursor = nullptr;
	CheckBox *ov_collision = nullptr; // the collision shapes of the Hewn objects
	CheckBox *ov_dims = nullptr; // the size of the selection along X / Y / Z, next to its outer edges
	CheckBox *ov_face_fill = nullptr;
	CheckBox *ov_face_centers = nullptr;
	CheckBox *ov_vnormals = nullptr;
	CheckBox *ov_snormals = nullptr;
	CheckBox *ov_fnormals = nullptr;
	HewnDragField *ov_normal_size = nullptr; // (drag with the mouse, click to type)
	CheckBox *ov_indices = nullptr;
	CheckBox *ov_edge_len = nullptr;
	CheckBox *ov_face_area = nullptr;
	CheckBox *ov_edge_angle = nullptr;
	CheckBox *ov_face_angles = nullptr;
	CheckBox *ov_face_grid = nullptr; // a world grid on the faces (Alt+G)
	CheckBox *ov_overlap = nullptr; // Overlapping Faces check (hw_overlap.cpp)
	CheckBox *ov_wireframe = nullptr; // Wireframe overlay (all Hewn meshes), with its opacity
	HSlider *ov_wire_opacity = nullptr;
	void _on_wireframe_changed(double p_unused);
	void update_all_gizmos();
	void _on_collision_toggled(bool p_on) { update_all_gizmos(); }
	bool overlay(CheckBox *p_option) const { return overlays && overlays->is_pressed() && p_option && p_option->is_pressed(); }
	enum {
		SNAP_INCREMENT = 1, // transforms in steps from where they started
		SNAP_VERTEX = 2,
		SNAP_EDGE = 4,
		SNAP_FACE = 8,
		SNAP_GRID = 16, // onto the grid (absolute)
		SNAP_EDGE_CENTER = 32,
		SNAP_FACE_CENTER = 64,
		SNAP_STEPS = SNAP_INCREMENT | SNAP_GRID,
		SNAP_GEOMETRY = SNAP_VERTEX | SNAP_EDGE | SNAP_FACE | SNAP_EDGE_CENTER | SNAP_FACE_CENTER,
		SNAP_POINTS = SNAP_VERTEX | SNAP_EDGE | SNAP_EDGE_CENTER | SNAP_FACE_CENTER, // (drawing tools: not the surface itself)
	};
	static const int SNAP_TARGET_COUNT = 7;
	static const int SNAP_ORDER[SNAP_TARGET_COUNT]; // the menu order
	int snap_targets = SNAP_INCREMENT;
	enum {
		ORIENT_GLOBAL,
		ORIENT_LOCAL,
		ORIENT_NORMAL,
		ORIENT_GIMBAL,
		ORIENT_VIEW,
		ORIENT_CURSOR,
		ORIENT_PARENT,
	};
	enum {
		PIVOT_BBOX,
		PIVOT_CURSOR,
		PIVOT_INDIVIDUAL,
		PIVOT_MEDIAN,
		PIVOT_ACTIVE,
	};
	enum {
		ORIENT_CUSTOM = 7, // custom orientations: ORIENT_CUSTOM + index
		ORIENT_ID_ADD = 900,
		ORIENT_ID_DELETE = 901,
	};
	std::vector<Basis> custom_orients; // world space
	std::vector<String> custom_names;
	String orient_name(int p_orient) const;
	void add_custom_orientation(const String &p_name, const Basis &p_basis);
	static const char *ORIENT_NAMES[7];
	static const char *ORIENT_ICONS[7];
	static const char *PIVOT_NAMES[5];
	static const char *PIVOT_ICONS[5];
	int orientation = ORIENT_GLOBAL;
	int pivot_mode = PIVOT_MEDIAN;
	Button *prop_enabled = nullptr;
	OptionButton *prop_falloff = nullptr;
	HewnDragField *prop_radius = nullptr; // (drag with the mouse, click to type)
	CheckBox *prop_connected = nullptr;
	CheckBox *prop_projected = nullptr; // Projected from View: distances across the view
	Button *automerge = nullptr;
	HewnDragField *automerge_dist = nullptr; // (drag with the mouse, click to type)
	Ref<Texture2D> icon(const String &p_name);
	Ref<Texture2D> shape_icon(int p_type);
	void update_snap_highlight(); // the snap menu button lit (accent color) while Godot's magnet is on
	int snap_lit = -1;
	HBoxContainer *toolbar = nullptr; // the buttons
	ScrollContainer *toolbar_root = nullptr; // placed by the plugin under the 3D editor toolbar; scrolls with the wheel
	PanelContainer *toolbar_fold = nullptr; // the header's fold button, left of toolbar_root (never scrolled away)
	Button *show_gizmo = nullptr;
	// the Box Cutter's settings in the header while it is the active tool (the same as the Edit tab's)
	Control *cutter_bar = nullptr;
	OptionButton *hdr_cut_shape = nullptr;
	OptionButton *hdr_cut_op = nullptr;
	CheckBox *hdr_cut_through = nullptr;
	Label *hdr_cut_sides_label = nullptr;
	SpinBox *hdr_cut_sides = nullptr;
	void _sync_cutter(bool p_from_header);
	void _on_cut_header_int(int p_unused);
	void _on_cut_header_bool(bool p_unused);
	void _on_cut_header_value(double p_unused);
	void _on_cut_tab_int(int p_unused);
	void _on_cut_tab_bool(bool p_unused);
	void _on_cut_tab_value(double p_unused);
	void _on_decal_kind(int p_kind);
	Control *decal_overlay_rows[2] = { nullptr, nullptr };
	Control *decal_mask_rows[2] = { nullptr, nullptr };
	VBoxContainer *decal_opts = nullptr;
	CheckBox *boolean_live = nullptr; // Object tab: booleans as modifiers
	CheckBox *bake_occluders = nullptr; // Object tab, Game: occluders in the baked level
	// Create tab: entities, decals / overlays (hw_place_ui.cpp)
	HFlowContainer *entity_flow = nullptr;
	EditorResourcePicker *decal_texture = nullptr;
	OptionButton *decal_kind = nullptr; // overlay / decal
	OptionButton *decal_outline = nullptr;
	OptionButton *decal_blend = nullptr;
	GridContainer *decal_layers = nullptr; // the decal's cull mask (render layers)
	static Array entity_defs();
	void set_decal_texture(const String &p_path);
	CheckBox *keep_overlap = nullptr; // Create tab: shapes built into the edited mesh keep touching faces
	// Create tab, Texture: new objects project their textures from world space, at
	// this many pixels per meter (only new objects; the ones made keep theirs)
	CheckBox *new_uv_world = nullptr;
	SpinBox *new_uv_pixels = nullptr;
	void _on_new_uv_pixels(double p_value);
	void _on_new_uv_world(bool p_on);
	void _new_uv_scale(float p_factor);
	// texture tools: steps of the buttons, the inset in pixels, the UV rect clipboard
	SpinBox *uv_step_move = nullptr;
	SpinBox *uv_step_rotate = nullptr;
	SpinBox *uv_step_scale = nullptr;
	SpinBox *uv_inset_px = nullptr;
	Label *uv_rect_label = nullptr;
	Button *xray = nullptr;
	Button *visible_select = nullptr; // picking ignores back faces
	MenuButton *mesh_menu = nullptr; // Select / Vertex / Edge / Face functions
	MenuButton *object_menu = nullptr;
	// tool column at the left of the viewport (collapsible)
	PanelContainer *tool_column = nullptr;
	VBoxContainer *tool_body = nullptr;
	Button *tool_collapse = nullptr;
	Button *tool_buttons[15] = {}; // (by ToolId; which ones show depends on the mode)

	void setup(HewnEditorPlugin *p_plugin);
	void refresh(); // mode buttons, status, materials, uv params
	void apply_uv_params();
	Color get_paint_color() const;
	OptionButton *paint_channel = nullptr; // 0 the vertex color, 1..4 CUSTOM0..3
	OptionButton *paint_format = nullptr;
	OptionButton *paint_blend = nullptr;
	HewnDragField *paint_scale = nullptr;
	void _on_paint_channel(int p_index);
	void _on_paint_format(int p_index);

	// (hw_vpaint.cpp) the Vertex Paint header: shown instead of the edit blocks in Vertex Paint mode
	MenuButton *interaction_menu = nullptr; // Object Mode / Edit Mode / Vertex Paint
	ColorPickerButton *paint_color2 = nullptr;
	Button *paint_mask_buttons[4] = {}; // R G B A: off = that channel is left alone
	HewnDragField *paint_radius_field = nullptr, *paint_strength_field = nullptr;
	OptionButton *paint_falloff = nullptr; // the screen's circle / a circle on the surface
	Button *paint_view_btns[4] = {};
	Button *shading_menu = nullptr; // the header's Shading menu: the same views as Vertex Paint's buttons, in every mode
	PopupPanel *shading_popup = nullptr;
	Button *shading_btns[6] = {};
	VBoxContainer *solid_opts = nullptr; // in it with Single Color / Random: the world lighting; Single Color: the cavity
	HewnDragField *solid_angle = nullptr;
	Button *solid_world = nullptr;
	CheckBox *cavity_check = nullptr;
	Control *cavity_opts = nullptr;
	HewnDragField *cavity_ridge = nullptr;
	HewnDragField *cavity_valley = nullptr;
	OptionButton *cavity_type = nullptr;
	void _on_solid_angle(double p_value);
	void _on_solid_world(bool p_on);
	void _on_cavity(bool p_on);
	void _on_cavity_value(double p_unused);
	void _on_cavity_type(int p_index);
	void _on_view_state_changed(double p_unused);
	void _on_shading(int p_id);
	void sync_shading();
	Button *paint_alpha_btns[3] = {}; // (the channel view with alpha: RGBA / RGB / A)
	Control *paint_alpha_block = nullptr;
	HewnDragField *ov_square_size = nullptr; // what the mesh shows: its materials / the painted channel / the normals
	OptionButton *paint_falloff_type = nullptr; // Smooth, Sphere, Root, Sharp, Linear, Constant
	Control *elements_block = nullptr; // (the vertex / edge / face buttons: not in object mode)
	std::vector<Control *> paint_row1_blocks; // (Vertex Paint's blocks in the first row)
	void _on_paint_mask(bool p_on, int p_k);
	Button *paint_squares_btn = nullptr, *paint_split_btn = nullptr;
	CheckBox *paint_pressure_check = nullptr;
	EditorResourcePicker *paint_curve_picker = nullptr;
	CheckBox *paint_circle_check = nullptr;
	CheckBox *paint_bars_check = nullptr;
	ColorPickerButton *paint_circle_picker = nullptr;
	MenuButton *paint_more = nullptr;
	Button *isolate_btn = nullptr;
	std::vector<Control *> paint_blocks, edit_blocks; // (the header's blocks of each mode)
	ScrollContainer *paint_root = nullptr; // Vertex Paint's settings: a second row under the header (its own scrolling)
	HBoxContainer *paint_row = nullptr;
	void _on_paint_row_input(const Ref<InputEvent> &p_event);
	bool paint_bar_shown = false;
	bool toolbar_folded = false;
	void build_paint_bar(HBoxContainer *p_modes_block);
	void show_paint_bar(bool p_on);
	void _on_interaction(int p_id);
	void _on_paint_swap();
	void _on_paint_reset();
	void _on_paint_changed();
	void _on_paint_radius(double p_value);
	void _on_paint_strength(double p_value);
	void _on_paint_more(int p_id);
	void _on_isolate(bool p_on);
};

// ------------------------------------------------------------------ plugin

std::vector<const char *> hw_obj_props(Node *p_node, String &r_title); // (hw_obj_panel.cpp) a node's panel properties
Object *hw_obj_owner(Node *p_node, const String &p_prop); // where a panel property lives (an Area3D's size: its box)
Variant hw_obj_get(Node *p_node, const String &p_prop);
String hw_obj_picture(Node *p_node);
bool hw_obj_has_picture(Node *p_node);

class HewnEditorPlugin : public EditorPlugin {
	GDCLASS(HewnEditorPlugin, EditorPlugin)
	Ref<HewnExportPlugin> export_plugin; // (the game gets plain nodes)
	friend class HewnQuadView;
	friend class HewnGizmoPlugin;
	friend class HewnUVCanvas;
	friend class HewnDock;
	friend class HewnPrefabBrowser;
	friend class HewnTextureBrowser;

public:
	enum Mode {
		MODE_OBJECT = 0, // not editing: Godot handles the node as usual
		MODE_VERTEX = 1,
		MODE_EDGE = 2,
		MODE_FACE = 3,
	};
	enum Tool {
		TOOL_MOVE = 0,
		TOOL_ROTATE = 1,
		TOOL_SCALE = 2,
		TOOL_SELECT = 3, // no handles
	};
	// Interactive (modal) operations.
	enum Modal {
		MODAL_NONE,
		MODAL_MOVE,
		MODAL_ROTATE,
		MODAL_SCALE,
		MODAL_BEVEL,
		MODAL_INSET,
		MODAL_SNAP, // B: point-to-point move
		MODAL_KNIFE,
		MODAL_LOOPCUT,
		MODAL_CUTTER,
		MODAL_POLY,
		MODAL_SLIDE, // GG: vertices slide along their edges
		MODAL_SHAPE, // drawing a new primitive
		MODAL_PAINT, // vertex color brush
		MODAL_EXTRUDE_AMOUNT, // Alt+E along normals / individual faces
		MODAL_BISECT,
		MODAL_CLIP,
		MODAL_PREFAB,
		MODAL_VALUE, // vertex bevel, push/pull, shrink/fatten, shear, offset edge slide
		MODAL_RULER,
		MODAL_CURSOR, // tool: clicks / drags place the 3D cursor
		MODAL_UV, // texture move / rotate / scale in the viewport
		MODAL_UVT, // Shift+T: the texture tile on the face with handles
		MODAL_TSNAP, // T: the texture against the edge under the mouse
		MODAL_SPIN, // the Spin tool: a ring around the 3D cursor to drag
		MODAL_FACECUT, // Face Cutter: lines across the selected faces from one of their border edges
	};
	enum CutterShape {
		CUTTER_RECT = 0,
		CUTTER_CIRCLE = 1,
		CUTTER_NGON = 2,
	};
	enum CutterOp {
		CUTTER_CUT = 0,
		CUTTER_SLICE = 1,
		CUTTER_JOIN = 2,
		CUTTER_INTERSECT = 3,
		CUTTER_MAKE = 4,
	};

private:
	enum Drag {
		DRAG_NONE,
		DRAG_PENDING, // mouse down, not moved enough yet
		DRAG_BOX,
		DRAG_HANDLE,
		DRAG_CIRCLE, // the circle select tool: selects under the circle while held
		DRAG_LASSO, // the lasso select tool: the outline drawn, selected on release
	};

public:
	// A screen region the selection tools pick in: a box, a circle or a lasso outline.
	struct SelRegion {
		int kind = 0; // 0 box, 1 circle, 2 lasso
		Rect2 rect;
		Vector2 center;
		float radius = 0.0f;
		std::vector<Vector2> poly;
		bool has(const Vector2 &p) const;
		// the part of a segment inside (as fractions along it); false: none
		bool clip(const Vector2 &a, const Vector2 &b, float &r_t0, float &r_t1) const;
		Vector2 middle() const;
	};
	int select_kind = 0; // the Select tool: 0 box, 1 circle, 2 lasso
	float circle_radius = 25.0f; // px
	std::vector<Vector2> lasso_pts;
	void _region_select(Camera3D *p_cam, const SelRegion &p_region, bool p_shift, bool p_ctrl);
	void _draw_select_region(Control *p_overlay);

private:

	uint64_t edited_id = 0;
	PopupMenu *cursor_menu = nullptr;
	Ref<HewnGizmoPlugin> gizmo_plugin;
	Ref<HewnEntityGizmoPlugin> entity_gizmo_plugin;
	HewnDock *dock = nullptr;
	EditorDock *uv_dock = nullptr;
	HewnTextureBrowser *texture_dock = nullptr;
	bool uv_lock = false; // UV lock: moved faces keep their UVs (the Material tab)
	std::vector<std::vector<Vector3>> overlap_same, overlap_opposite; // (world polygons)
	int overlap_count = 0;
	double overlap_timer = 0.0;
	void _update_overlaps();
	void _draw_overlaps(Control *p_overlay, Camera3D *p_cam);
	HewnPrefabBrowser *prefab_dock = nullptr;
	// prefab placing (hw_prefabs.cpp)
	String prefab_path;
	uint64_t prefab_ghost = 0;
	uint64_t prefab_last = 0; // the one placed last (W: another one while it is still selected and in place)
	Transform3D prefab_last_xf;
	bool prefab_valid = false;
	Vector3 prefab_pos, prefab_n = Vector3(0, 1, 0);
	float prefab_turn = 0.0f;
	Vector3 prefab_random_rot, prefab_scale = Vector3(1, 1, 1); // (degrees per local axis, scale per axis)
	bool prefab_pressing = false; // LMB held and dragged turns the scene, the release places it
	bool prefab_turned = false;
	Vector2 prefab_press_pos;
	float prefab_press_turn = 0.0f;
	int prefab_seed = 0;
	// Placing other things the same way (entities, decals, overlays): made by `place_factory`.
	std::function<Node3D *()> place_factory;
	String place_label;
	bool place_along = false; // on the surface's normal (decals, overlays, lights), else upright
	bool place_random = false; // the prefab panel's random turn / size
	float place_size = 1.0f; // - / = while placing
	int wall_side = 0; // new walls: inside of the drawn line (the last choice in the shape panel)
	std::vector<ObjectID> sel_order; // the editor's selection in the order it was made
	Transform3D place_local; // after the placing transform (a light a bit off the wall, a spot turned out)
	Node3D *_make_place_node();
	void _make_place_ghost();
	void prefab_reroll();
	void _remove_prefab_ghost();
	Transform3D _prefab_transform() const;
	int32_t _prefab_input(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	void _place_prefab();
	void _draw_prefab(Control *p_overlay, Camera3D *p_cam);
	bool _pending_ready() const;
	// viewport UV transforms: the texture follows the mouse on the plane of the face it started on
	int uvm_kind = 0; // 0 move, 1 rotate, 2 scale
	int uvm_axis = -1; // -1 free, 0 U, 1 V
	Dictionary uvm_before;
	hewn::Mesh uvm_backup;
	PackedInt32Array uvm_faces;
	Vector3 uvm_n, uvm_p, uvm_pu, uvm_pv; // the plane (world) and its axes
	Vector2 uvm_row0, uvm_row1, uvm_t; // tiles = (row0 . x, row1 . x) + t, x = (p . pu, p . pv)
	Vector2 uvm_center, uvm_uv0; // tiles
	Vector3 uvm_center3;
	String uvm_label;
	Rect2 uv_clip = Rect2(0, 0, 1, 1); // Copy / Paste UV Rect
	// Shift+T: one texture tile drawn on the plane of the face, with handles. Raw = the planar projection
	// of the face (compute_uvs), tile = ms * (scale * R(rot) * raw + offset).
	struct UVTransformState {
		int face = -1; // the face whose plane is used
		PackedInt32Array faces; // all get the transform
		Dictionary before;
		bool world = false;
		Vector3 u, v, n; // projection axes (node local, or world when the face uses world UVs)
		float nd = 0.0f; // the plane: n . p = nd
		Vector2 ms = Vector2(1, 1); // the material's tiling
		Vector2 tex = Vector2(64, 64); // texture pixels
		Ref<Texture2D> texture;
		float rot = 0.0f;
		Vector2 scale = Vector2(1, 1), offset;
		Vector2i tile; // the repetition with the handles
		int hover = -1, drag = -1; // handles: 0-3 corners, 4-7 sides, 8 move, 9 move U, 10 move V, 11 rotate, 12 scale (S)
		bool key_drag = false; // G / R / S: the drag follows the mouse until a click
		int scale_axis = -1; // S then X / Y: only along U / V
		Vector2 drag_r0;
		float rot0 = 0.0f;
		Vector2 scale0, offset0;
		bool pixel_snap = false;
		bool shift_free = false; // Shift held: no snapping
		std::vector<Vector2> raw; // the face's corners
		// what the handles snap to: the vertices and edges of all the selected faces, on the plane
		std::vector<Vector2> snap_pts;
		std::vector<std::pair<Vector2, Vector2>> snap_edges;
		std::vector<Vector3> outline; // world corners of all faces, per face
		std::vector<int> outline_sizes;
		String label;
		bool keep_aspect = false; // Alt: corners / sides scale both ways alike
		int base = -1; // B in G / R / S: the tile point that moves (G) or stays (R / S): 0-3 corners, 4-7 sides, 8 middle
		Vector2 base_tile;
		String num; // typed value of G / R / S
		struct Step {
			float rot;
			Vector2 scale, offset;
		};
		std::vector<Step> history; // its own Ctrl+Z (forgotten when it ends: one step in the editor's history)
		int hist_pos = 0;
	} uvt;
	void _uvt_push();
	void _uvt_typed(Camera3D *p_cam);
	void start_uv_transform(Camera3D *p_cam);
	void _uvt_key_drag_end(bool p_keep);
	// T: one face, or quads laid out as a grid (meters per corner), against a border edge
	struct TSnapState {
		Dictionary before;
		hewn::Mesh backup;
		PackedInt32Array faces;
		std::map<int, std::vector<Vector2>> layout;
		std::vector<std::pair<int, int>> border; // (face, corner): its edge corner -> corner + 1
		bool grid_error = false;
		int edge = -1; // in border
		int side = 2; // the image's top / left / bottom / right on the edge
		int fit = 0; // 1: the height, 2: the width
		Vector2 k = Vector2(1, 1), k_before_fit = Vector2(1, 1); // tiles per meter
		String label;
	} ts;
	void start_tsnap(Camera3D *p_cam);
	void _tsnap_update(Camera3D *p_cam);
	void _tsnap_finish(bool p_apply);
	int32_t _tsnap_input(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	void _draw_tsnap(Control *p_overlay, Camera3D *p_cam);
	void _texture_across(HewnMesh3D *p_src, int p_sf, HewnMesh3D *p_dst, int p_df, bool p_material);
	Vector2 _uvt_tile_of(const Vector2 &p_raw) const;
	Vector2 _uvt_raw_of(const Vector2 &p_tile) const;
	Vector3 _uvt_world(const Vector2 &p_raw) const;
	bool _uvt_mouse_raw(Camera3D *p_cam, const Vector2 &p_pos, Vector2 &r_raw) const;
	void _uvt_handles(Camera3D *p_cam, std::vector<Vector2> &r_pos, std::vector<bool> &r_shown) const;
	void _uvt_apply();
	void _uvt_drag(Camera3D *p_cam, const Vector2 &p_pos);
	void _uvt_finish(bool p_apply);
	int32_t _uvt_input(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	void _draw_uvt(Control *p_overlay, Camera3D *p_cam);
	bool _uv_targets(std::vector<HewnMesh3D *> &r_targets, PackedInt32Array &r_faces) const;
	bool _uv_at(Camera3D *p_cam, const Vector2 &p_pos, Vector2 &r_uv) const;
	void _uv_modal_update(Camera3D *p_cam, bool p_ctrl);
	int32_t _uv_modal_input(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	void _draw_uv_modal(Control *p_overlay, Camera3D *p_cam);
	void _uv_menu(Camera3D *p_cam);
	HewnUVCanvas *uv_canvas = nullptr;
	EditorFileDialog *obj_dialog = nullptr;
	bool detached = false; // edit mode: the node is out of Godot's selection (no Godot gizmo / box)

	Drag drag = DRAG_NONE;
	Vector2 press_pos;
	Vector2 mouse_pos;
	bool press_shift = false;
	bool press_ctrl = false;
	bool press_alt = false;
	bool precise_armed = false; // Shift+drag starts an extrude; precision needs a fresh Shift press
	// Shift = precise mouse: motion is scaled down while Shift is held.
	bool prec_active = false;
	Vector2 prec_offset, prec_raw0, prec_eff0;
	Vector2 modal_cursor; // where the cursor really is (Shift's precision moves the value slower than it)
	float bevel_d0 = 0.0f; // the mouse's distance to the bevel's middle when it began
	int drag_axis = -1; // 0..2 axis, 3 = free / uniform
	Vector3 drag_pivot;
	Vector3 drag_axis_dir;
	float drag_t0 = 0.0f;
	Vector3 drag_hit0;
	float drag_screen0 = 1.0f;
	bool drag_extruded = false; // Shift+drag of the move arrows extruded first
	uint64_t hiding_id = 0; // the mesh whose hidden faces (H) are left out now: the one in edit mode
	void hide_faces(int p_kind); // H: the selected, Shift+H: the others, Alt+H: show them all again
	void _finish_handle_drag(); // the result goes to the Last Operation panel, like G / R / S
	std::vector<int> drag_verts;
	std::vector<Vector3> drag_orig;
	Dictionary edit_before;
	String edit_name;
	bool editing = false;

	// ---- modal state
	Modal modal = MODAL_NONE;
	Vector2 modal_mouse0;
	Vector3 modal_pivot; // world
	int modal_axis = -1; // -1 none, 0..2 axis, 3 custom direction
	bool modal_plane = false; // constraint is the plane orthogonal to the axis
	bool modal_local = false;
	Vector3 modal_custom_dir;
	String modal_num;
	bool modal_object = false; // moves the node itself (snap tool in object mode)
	Transform3D modal_object_xf;
	std::vector<int> modal_verts; // selected vertices first, then proportional-editing neighbors
	std::vector<Vector3> modal_orig; // world positions at start
	std::vector<float> modal_weights; // 1 for the selection, falloff for the neighbors
	std::vector<Vector3> modal_vpivots; // pivot per vertex (individual origins)
	size_t modal_sel_count = 0;
	Basis modal_orient; // transform orientation at the start of the operation
	void _setup_modal_set(Camera3D *p_cam, Modal p_kind);
	std::vector<float> drag_weights;
	Vector3 modal_hit0;
	bool modal_snapped = false;
	int modal_snap_kind = 0; // hw_editor::SnapKind
	Vector3 modal_snap_point;
	String modal_label;
	bool modal_keep_on_cancel = false; // extrude / duplicate / rip: cancelling keeps the new geometry in place
	hewn::Mesh modal_backup;
	hewn::Selection modal_sel_backup;
	std::vector<hewn::Edge> modal_edges;
	float modal_scale_k = 1.0f; // world units per pixel of mouse distance
	int bevel_segments = 1;
	bool inset_individual = false;
	hewn::ops::InsetOptions _inset_options(float p_amount); // the Inset panel's options (the modal: its I / O / B too)
	// final values of the modal operation (replayed through run_op on confirm)
	Vector3 res_delta, res_axis, res_dir;
	float res_angle = 0.0f, res_scale = 1.0f, res_amount = 0.0f;
	int res_constraint = 0; // 0 none, 1 line, 2 plane
	int modal_origin = 0; // 0 plain, 1 extrude, 2 duplicate, 3 rip
	Mode edit_mode_before = MODE_FACE;

	// Last Operation panel (hw_redo.cpp)
	Dictionary op_params; // op -> {key: value}
	String last_op;
	Mode last_op_mode = MODE_FACE;
	uint64_t last_op_node = 0; // (the panel's operation was done on it)
	int64_t last_op_version = -1;
	float header_h = 0.0f; // the Hewn header's height when open
	bool redo_running = false;
	// The states the last operation's history step sets (shared with it): a change in the Last Operation
	// panel rewrites the "after" one in place, so Ctrl+Z goes back to before the operation, whatever was
	// tried in between, and Godot's own history stays in step.
	Dictionary last_commit_before, last_commit_after;
	Dictionary last_op_before, last_op_after;
	bool redo_inplace = false, redo_inplace_done = false;
	bool redo_collapsed = false; // the bottom-left panel (last operation and shape parameters, one state; remembered)
	void _style_panel_header(Button *p_header, Control *p_panel);
	void _on_panel_header_input(const Ref<InputEvent> &p_event, Control *p_panel);
	void _set_panels_collapsed(bool p_collapsed);
	PanelContainer *redo_panel = nullptr;
	Button *redo_header = nullptr;
	GridContainer *redo_body = nullptr;
	uint64_t last_surface = 0;
	void _record_param_op(const String &p_op, Mode p_mode);
	void _show_redo();
	void _hide_redo();
	void _toggle_redo();
	void _on_redo_value(double p_value, const String &p_key);
	void _on_redo_bool(bool p_value, const String &p_key);
	void _on_redo_color(const Color &p_value, const String &p_key);
	void _refresh_redo_fields();
	void _on_redo_option(int p_index, const String &p_key);
	void _redo_last();
	void _check_redo_valid();

	// B snap: weighted point accumulation
	struct SnapPick {
		Vector3 p;
		int weight = 1;
	};
	int snap_phase = 0; // 0 = source, 1 = target
	std::vector<SnapPick> snap_picks;
	Vector3 snap_source;
	bool snap_hover_valid = false;
	int snap_hover_kind = 0;
	bool snap_ctrl = false;
	bool snap_avg_valid = false;
	Vector3 snap_avg;
	void _change_prop_radius(Camera3D *p_cam, float p_factor);
	Vector3 prop_proj_axis; // the view direction when proportional editing is Projected (2D); zero: in 3D
	void _show_extrude_menu(Camera3D *p_cam);
	void _on_extrude_menu(int p_id);
	PopupMenu *extrude_menu = nullptr;
	PopupMenu *op_menu = nullptr;
	std::vector<String> menu_ops;
	uint64_t menu_cam_id = 0;
	String value_op;
	Vector3 value_axis_a, value_axis_b;
	uint64_t rmb_press_ms = 0;
	Vector2 rmb_press_pos;
	bool rmb_pending = false;
	float rmb_moved = 0.0f;
	void _context_menu_deferred(uint64_t p_cam_id);
	Dictionary menu_last; // menu title -> op picked last time (the menu opens with it under the cursor)
	String menu_title;
	// Select Linked (L) can be adjusted afterwards (delimit, deselect)
	hewn::Selection linked_sel_before;
	// Checker Deselect: from the selection as it was, by the steps from its first element
	hewn::Selection checker_sel_before;
	void checker_deselect();
	void _delete_selected(hewn::Mesh &m, int p_kind);
	void _apply_checker();
	hewn::Selection normal_sel_before; // Same Normal: what was selected (its angle can be changed afterwards)
	void select_by_normal();
	void knife_project(); // the active mesh cut along the outlines of the other edited meshes, seen from the view
	void _apply_by_normal();
	int linked_seed_face = -1;
	void _show_menu(Camera3D *p_cam, const String &p_title, const std::vector<std::pair<String, String>> &p_items);
	void _on_menu_id(int p_id);
	std::map<String, std::vector<std::pair<String, String>>> menu_subs; // "sub:<key>" items: their categories
	void _object_menu(Camera3D *p_cam);
	void _run_object_op(const String &p_op);
	std::vector<HewnMesh3D *> _selected_meshes() const;
	void _context_menu(Camera3D *p_cam);
	void _delete_menu(Camera3D *p_cam);
	void _run_menu_op(Camera3D *p_cam, const String &p_op);
	void _add_menu(Camera3D *p_cam);
	bool _menu_check(const String &p_key) const;
	void _menu_toggle(const String &p_key);
	Vector2 menu_popup_at = Vector2(-1, -1);
	uint64_t menu_hidden_msec = 0;
	void _on_menu_hidden();
	bool adding_from_menu = false; // (start_placing: at the 3D cursor at once when create_at_cursor)
	bool shape_make_area = false; // (the box drawn becomes an Area3D)
	bool shape_make_cutter = false; // (... or a cutter)
	bool image_add_at_cursor = true;
	bool image_making = false; // (a picture's node made for the placing)
	Node3D *image_made = nullptr;
	PackedStringArray image_place_paths;
	Node3D *_make_kind_node(const String &p_kind);

public:
	void show_add_menu_at(const Vector2 &p_screen_pos);
	bool create_at_cursor = false; // Add menu: everything at the 3D cursor with its defaults (else drawn / placed with the mouse)

private:
	// Editor Settings > Shortcuts > Hewn
	void _register_shortcuts();
	int32_t _remap_key(Camera3D *p_cam, const Ref<InputEventKey> &p_key);
	bool remapping = false;
	void _start_value_modal(Camera3D *p_cam, const String &p_op);
	void _value_op_apply(const String &p_op, float p_value);
	void _store_value_params(const String &p_op);
	float _value_from_mouse(Camera3D *p_cam) const;
	void _on_extrude_menu_cam(Camera3D *p_cam, int p_id);
	void _apply_select_linked();
	void _separate(const String &p_how);
	bool _extrude_edges_verts(hewn::Mesh &m, hewn::Selection &s, float p_dist);
	uint64_t extrude_cam_id = 0;
	int extrude_kind = 1; // 1 along normals, 2 individual faces
	Vector3 snap_hover;

	// snap candidates (world): vertices and edges of every Hewn mesh in the scene
	std::vector<Vector3> cand_points;
	std::vector<std::pair<Vector3, Vector3>> cand_edges;
	std::unordered_set<int> cand_skip_faces; // edited-mesh faces touching moving vertices
	std::vector<Vector3> cand_face_centers;
	// ... and of the other meshes (MeshInstance3D): their triangles, for the surface under the mouse
	std::vector<Vector3> cand_tris;
	void _gather_plain_meshes(bool p_skip_moving);
	bool _raycast_cand_tris(const Vector3 &p_from, const Vector3 &p_dir, Vector3 &r_hit) const;

	// knife
	struct KnifeClick {
		int face = -1; // -1: in empty space (or on a wire): moved onto the first edge the cut meets
		Vector3 local;
	};

	std::vector<KnifeClick> knife_points; // the cut being drawn
	std::vector<std::vector<KnifeClick>> knife_done; // the cuts ended with RMB
	bool knife_hover_valid = false;
	KnifeClick knife_hover;

	// loop cut
	hewn::Edge loop_edge = hewn::Edge(-1, -1);
	int loop_cuts = 1;
	float loop_t = 0.5f;
	int loop_phase = 0; // 0 choose the ring, 1 slide

	// vertex / edge slide (GG)
	std::vector<int> slide_to; // per modal vertex: the neighbor it slides towards (-1 = stays)
	bool slide_unclamped = false; // C: allow sliding past the ends (extends the edge)

	// shape drawing
	int shape_type = 1;
	int shape_state = 0; // 0 waiting, 1 base, 2 height, 3 the first side of a turned box (Shift)
	String shape_num; // the height typed from the keyboard (shape_state 2; Enter: done)
	int f2_new_vertex = -1; // F on a border vertex made this one: it is moved at once (F2's autograb)
	bool shape_fixed_side = false; // turned box: shape_b.x is the drawn side
	float shape_side_len = 0.0f;
	bool tool_alt = false; // Alt held: the drawing plane stays
	bool plane_lock_valid = false;
	Vector3 plane_lock_origin, plane_lock_n, plane_lock_u, plane_lock_v;
	// Ctrl held before the first click: the plane of the surface under the cursor stays, past the
	// face's edges, until Ctrl is let go
	bool ctrl_lock_valid = false;
	Vector3 ctrl_lock_origin, ctrl_lock_n, ctrl_lock_u, ctrl_lock_v;
	Vector3 shape_origin, shape_n, shape_u, shape_v;
	Vector2 shape_a, shape_b;
	float shape_h = 0.0f, shape_t0 = 0.0f;
	Vector2 shape_press;
	std::vector<Vector2> wall_pts; // wall corners (plane coordinates) while drawing a wall
	std::vector<float> wall_hs; // their heights along the surface normal (H while drawing)
	float wall_cur_h = 0.0f, wall_h_t0 = 0.0f; // the next corner's height
	bool wall_h_mode = false; // H: the mouse sets the next corner's height
	bool tool_ctrl = false; // Ctrl held during the creation tools (inverts snapping)
	// Snap marker of the creation tools: where the point goes (or went) when snapping is on.
	bool snap_mark_valid = false;
	bool snap_mark_geom = false; // on a vertex / edge (not just the grid)
	int snap_mark_kind = 0; // hw_editor::SnapKind
	Vector3 snap_mark;
	mutable bool last_snap_geom = false;
	mutable int last_snap_kind = 0; // what _snap_target found (hw_editor::SnapKind)
	void _draw_snap_mark(Control *p_overlay, Camera3D *p_cam);
	bool _anything_selected() const;
	void _uvt_pick_base(Camera3D *p_cam);
	int ruler_hover_kind = 3; // (hw_editor::SnapKind of the ruler's point)
	bool _snap_height(Camera3D *p_cam, const Vector3 &p_base, const Vector3 &p_n, float &r_h);
	// knife angle constraint (A): 0 off, 1 screen space, 2 surface; 30 degree steps.
	int angle_lock = 0;
	Vector2 _angle_constrain(Camera3D *p_cam, const Vector2 &p_pos, const Vector3 &p_last, const Vector3 &p_n, const Vector3 &p_ref) const;
	String _angle_lock_text() const;
	PackedVector3Array poly_edit_orig; // base points before editing a Poly Shape (restored on cancel)
	bool grid_hover_valid = false;
	Vector3 grid_hover, grid_hover_n, grid_hover_u, grid_hover_v;
	bool _surface_frame(Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_origin, Vector3 &r_n, Vector3 &r_u, Vector3 &r_v) const;
	// Axes on a drawing plane: the X (else Z) of the transform orientation, laid onto the plane.
	void _plane_axes(const Vector3 &p_n, Vector3 &r_u, Vector3 &r_v) const;
	void _shape_footprint(Vector2 &r_min, Vector2 &r_max) const;
	void _draw_surface_grid(Control *p_overlay, Camera3D *p_cam, const Vector3 &p_center, const Vector3 &p_u, const Vector3 &p_v);
	// Shape parameters panel (hw_shape_panel.cpp): bottom left of the viewport while a parametric
	// shape is selected in object mode (until its geometry is edited).
	PanelContainer *shape_panel = nullptr;
	GridContainer *shape_body = nullptr;
	Dictionary shape_groups_open; // group title -> shown (the shape panel's foldable groups)
	void _on_shape_group(const String &p_group);
	uint64_t shape_panel_node = 0;
	Dictionary shape_panel_values; // what the panel shows (rebuilt when undo / the inspector changes them)
	int shape_panel_type = -1;
	void _update_shape_panel();
	void _hide_shape_panel();
	// (hw_obj_panel.cpp) the main properties of a selected light / entity / trigger / sound / decal / overlay / cutter
	PanelContainer *obj_panel = nullptr;
	GridContainer *obj_body = nullptr;
	uint64_t obj_panel_node = 0;
	Dictionary obj_panel_values;
	void _update_obj_panel();
	void _set_obj_prop(const Variant &p_value, const String &p_prop);
	void _on_obj_float(double p_value, const String &p_prop, int p_axis);
	void _on_obj_bool(bool p_value, const String &p_prop);
	void _on_obj_option(int p_index, const String &p_prop);
	void _on_obj_color(const Color &p_color, const String &p_prop);
	void _on_obj_text(const String &p_text, const String &p_prop);
	void _on_obj_entity_value(const String &p_text, const String &p_key);
	void _on_obj_apply_cutter();
	bool obj_link_size = true; // a decal's / overlay's size: one factor for all sides (the chain)
	void _on_obj_link(bool p_on);
	void _on_obj_layer(bool p_on, const String &p_prop, int p_bit);
	void _on_obj_aim();
	void _on_obj_picture(const String &p_path);
	void _on_obj_picture_browse();
	EditorFileDialog *obj_pic_dialog = nullptr;
	void _on_obj_camera_to_view();
	class SubViewport *cam_preview_vp = nullptr;
	void _update_camera_preview(Camera3D *p_cam);
	ObjectID aim_light; // the spot light aimed with the next click (its panel's Aim button)
	Transform3D aim_before;
	ObjectID place_copy_from; // W: another one like it - its panel's values
	void _on_shape_value(double p_value, const String &p_name);
	void _on_shape_bool(bool p_value, const String &p_name);
	void _on_shape_option(int p_index, const String &p_name);
	// "What next" action on W after a tool: e.g. a new box turns into a room. It stays
	// armed while the mesh is not changed otherwise.
	struct PendingMesh {
		uint64_t id = 0;
		uint64_t revision = 0;
	};
	String pending_action;
	std::vector<PendingMesh> pending_meshes;
	std::vector<std::pair<int, std::vector<int>>> pending_loops; // (index in pending_meshes, a hole in cap order)
	float pending_depth = 0.5f;
	ObjectID pending_cutter; // a non-destructive cut: W lines it (a corridor / alcove mesh inside the cutter)
	Transform3D pending_cutter_xf;
	std::vector<ObjectID> pending_cut_meshes; // ... the meshes it cuts
	std::vector<Dictionary> pending_precut; // "folded": the meshes before the cut
	std::vector<std::vector<hewn::ops::CutPlane>> pending_parts; // ... and the cut volume (world)
	void _arm_pending(const String &p_action, HewnMesh3D *p_node);
	void _arm_pending_loops(const String &p_action, const std::vector<HewnMesh3D *> &p_meshes, const std::vector<std::pair<int, std::vector<int>>> &p_loops);
	void _keep_pending(HewnMesh3D *p_node); // (changed through the shape panel: still the same action)
	// Level tools (hw_level.cpp): cuts drawn like the shapes and Clip.
	int shape_cut = 0; // the shape tool draws a cut: 1 box, 2 cylinder
	bool poly_cut = false; // the poly tool draws a prism cut
	bool cut_infinite = false; // orthogonal view: through everything
	void _apply_shape_cut();
	void _apply_prism_cut(const std::vector<Vector3> &p_pts, float p_height);
	int _apply_cut_world(const std::vector<std::vector<hewn::ops::CutPlane>> &p_parts, float p_depth);
	// The last Box / Cylinder / Prism Cut, for its panel: destructive (the meshes) or a cutter node.
	struct LastCut {
		std::vector<std::vector<hewn::ops::CutPlane>> parts;
		float depth = 0.5f;
		int shape = 0; // HewnCutter3D::Shape
		Transform3D xf;
		Vector3 size;
		int sides = 16;
		PackedVector2Array points;
		int64_t history = -1, version = -1;
		int steps = 1; // undo steps it took (the cut, and its corridor / alcove)
	} last_cut;
	HewnMesh3D *last_cut_first = nullptr; // (the first mesh the last destructive cut changed)
	void _run_cut(bool p_redo);
	bool _cut_panel_valid();
	std::vector<HewnMesh3D *> _level_targets(PackedInt32Array *r_faces) const;
	int clip_state = 0;
	int clip_mode = 1; // 0 split only, 1 remove the front (keep the back), 2 remove the back (keep the front)
	Vector3 clip_a, clip_b;
	std::vector<Vector3> clip_pts, clip_nrms; // the clicked points (2 or 3) and the surface normals there
	Vector3 clip_hover, clip_hover_n;
	bool clip_hover_valid = false;
	int clip_drag = -1; // (a placed point being dragged)
	bool _clip_pick(Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_point, Vector3 &r_n, bool p_free) const;
	int32_t _clip_input(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	void _draw_clip(Control *p_overlay, Camera3D *p_cam);
	bool _clip_plane(Vector3 &r_point, Vector3 &r_normal) const;
	void _apply_clip();
	// its Last Operation panel: the meshes before and
	// after it (the "after" ones are shared with the history step, so a change rewrites it)
	std::vector<ObjectID> clip_nodes;
	std::vector<Dictionary> clip_before, clip_after;
	std::vector<int> clip_faces;
	Vector3 clip_point, clip_normal;
	int64_t clip_version = -1;
	void _clip_run(bool p_redo);
	bool _clip_panel_valid();
	// texture painting: Alt+LMB from the one selected face onto others, Ctrl+Alt: UV only,
	// Alt+RMB: pick from the clicked face; T: the texture along the face edge under the mouse
	bool tex_paint = false;
	bool tex_uv_only = false;
	int tex_last = -1;
	// faces given the selected face's texture since it was selected (Alt+click or drag alike): a new one takes
	// it from one of them next to it, so clicking face by face runs on like a drag does
	std::vector<int> tex_chain;
	int tex_chain_sel = -1;
	uint64_t tex_chain_mesh = 0;
	int _tex_source(HewnMesh3D *p_node, int p_sel, int p_face);
	Dictionary tex_before;
	int32_t _texture_input(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	HewnMesh3D *_pending_target() const;
	bool _run_pending();
	void _draw_pending(Control *p_overlay);
	String _pending_text() const;
	void _set_shape_value(const String &p_name, float p_value);
	void _on_shape_edit_base();
	Control *_main_surface() const;
	Camera3D *_viewport_camera() const;
	bool bar_shown = false; // the header was laid out since it last appeared
	bool _shape_begin(Camera3D *p_cam, const Vector2 &p_pos);
	bool _shape_plane_point(Camera3D *p_cam, const Vector2 &p_pos, Vector2 &r_uv) const;
	void _finish_shape();
	bool _configure_shape_node(HewnMesh3D *p_node, bool p_preview) const;
	void _new_object_uv(HewnMesh3D *p_node) const; // the UV tab's World UV settings on a new object
	float _world_uv_scale() const;
	int32_t _shape_input(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	int32_t _shape_input_impl(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	// live preview of the shape being drawn (an internal, unsaved node in the edited scene)
	uint64_t shape_preview_id = 0;
	int shape_dir = 0; // R while drawing: the direction variant (stairs, door, arch, prism)
	void _update_shape_preview();
	HashMap<String, float> shape_wheel; // (the steps / sides chosen with the wheel while a shape is drawn)
	void _remove_shape_preview();
	void _draw_shape_tool(Control *p_overlay, Camera3D *p_cam);
	void _draw_length_label(Control *p_overlay, Camera3D *p_cam, const Vector3 &p_a, const Vector3 &p_b);

	// bisect
	int bisect_state = 0;
	Vector2 bisect_a;
	int32_t _bisect_input(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	void _draw_bisect(Control *p_overlay, Camera3D *p_cam);

	// vertex paint
	bool painting = false;
	float paint_radius = 40.0f; // px
	float paint_strength = 0.5f;
	int32_t _paint_input(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	// Shift + drag with the brush (Krita): the radius by the horizontal motion, the cursor kept where it was
	bool paint_resizing = false;
	Vector2 paint_resize_at;
	float paint_resize_r0 = 0.0f, paint_resize_dx = 0.0f;
	bool _paint_wheel(const Ref<InputEventMouseButton> &p_mb); // Ctrl / Shift + wheel: radius / strength
	bool paint_resize_strength = false; // (Ctrl+Shift + drag: the strength instead)
	float paint_resize_s0 = 0.0f;

	void _draw_paint(Control *p_overlay, Camera3D *p_cam);

	// rulers: they stay until removed (X / Del over one); 3 points = protractor
	struct RulerItem {
		std::vector<Vector3> p; // a, b  or  a, vertex (angle), b
	};
	std::vector<RulerItem> rulers;
	int ruler_state = 0; // 1: the new ruler's end follows the mouse until the next click
	int ruler_drag = -1, ruler_drag_pt = -1; // ruler / point being dragged
	int ruler_hover_item = -1, ruler_hover_pt = -1; // under the mouse (point -1: on a segment)
	bool ruler_hover_valid = false;
	Vector3 ruler_hover;
	bool ruler_moved = false;
	Vector2 ruler_press;
	void _ruler_hit(Camera3D *p_cam, const Vector2 &p_pos, int &r_item, int &r_pt, int &r_seg) const;
	void _draw_rulers(Control *p_overlay, Camera3D *p_cam);
	void _place_cursor(Camera3D *p_cam, const Vector2 &p_pos, bool p_ctrl = false);
	void start_cursor_tool();
	bool cursor_once = false; // (Shift+X: done after one click)
	// G / R with the mouse over the 3D cursor (or Shift+S): it moves / turns like an object, X / Y / Z: an
	// axis, snapping as for objects (Ctrl: the other way), click / Enter: done, RMB / Esc: back
	int cursor_xf = 0; // 0 none, 1 move, 2 rotate
	int cursor_xf_axis = -1;
	Vector3 cursor_xf_pos0;
	Basis cursor_xf_basis0;
	Vector2 cursor_xf_mouse0;
	float cursor_xf_t0 = 0.0f;
	bool _mouse_near_cursor(Camera3D *p_cam) const;
	void _start_cursor_xf(Camera3D *p_cam, int p_kind);
	void _cursor_xf_update(Camera3D *p_cam, bool p_ctrl);

	// box cutter
	int cutter_state = 0; // 0 idle, 1 drawing, 2 ngon points, 3 depth
	Vector3 cutter_origin, cutter_n, cutter_u, cutter_v; // drawing plane (world)
	Vector2 cutter_a, cutter_b; // plane coords of the drag
	bool cutter_square = false;
	bool cutter_centered = false;
	std::vector<Vector2> cutter_poly;
	float cutter_depth = 0.0f;
	float cutter_t0 = 0.0f;
	int cutter_material = 0;
	int cutter_op = CUTTER_CUT;

	// poly shape
	std::vector<Vector3> poly_points; // world
	Vector3 poly_origin, poly_n;
	bool poly_cursor_valid = false; // where the next point would go (live preview of a new Poly Shape)
	Vector3 poly_cursor;
	void _setup_poly_node(HewnMesh3D *p_node, const std::vector<Vector3> &p_pts, float p_height) const;
	void _update_poly_preview();
	bool poly_has_plane = false;
	int poly_phase = 0; // 0 points, 1 height
	int poly_drag_point = -1;
	bool poly_dragging = false;
	Vector2 poly_press_pos;
	float poly_height_value = 1.0f;
	float poly_t0 = 0.0f;
	Vector3 poly_hbase; // the height is measured along the normal through this point (under the mouse at the start)
	uint64_t poly_edit_id = 0; // node whose shape is being edited
	bool poly_edit_wall = false; // editing the points of a wall (an open path)
	Transform3D poly_edit_xf; // its transform before (a straight wall becomes a path wall)

	Camera3D *_camera_for_overlay(Control *p_overlay) const;
	bool _ray(Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_from, Vector3 &r_dir) const;
	int _raycast_face(const Vector3 &p_from, const Vector3 &p_dir, float *r_dist = nullptr) const;
	bool _raycast_scene(const Vector3 &p_from, const Vector3 &p_dir, Vector3 &r_hit, Vector3 &r_normal, HewnMesh3D **r_node = nullptr, int *r_face = nullptr, bool p_skip_moving = false) const;
	bool _visible(Camera3D *p_cam, const Vector3 &p_world) const;
	// (the edited mesh's triangle tree, made again when its positions / faces change; the answers of
	// _visible kept while the camera, the mesh and the view stay)
	mutable std::shared_ptr<hw_editor::TriBVH> bvh;
	mutable uint64_t bvh_node = 0, bvh_rev = ~0ull, bvh_hash = 0, bvh_frame = ~0ull;
	struct VisKey {
		uint64_t cam = 0, node = 0, geo = 0;
		Transform3D cam_xf, node_xf;
		float fov = 0, size = 0, znear = 0, zfar = 0, hoff = 0, voff = 0;
		int proj = -1;
		Vector2 vp;
		bool cull = false;
		bool operator==(const VisKey &o) const {
			return cam == o.cam && node == o.node && geo == o.geo && cam_xf == o.cam_xf && node_xf == o.node_xf && fov == o.fov && size == o.size &&
					znear == o.znear && zfar == o.zfar && hoff == o.hoff && voff == o.voff && proj == o.proj && vp == o.vp && cull == o.cull;
		}
	};
	mutable VisKey vis_key;
	mutable std::unordered_map<uint64_t, char> vis_memo;
	const hw_editor::TriBVH *_bvh_sync(HewnMesh3D *p_node) const;
	void _pick(Camera3D *p_cam, const Vector2 &p_pos, int &r_vert, hewn::Edge &r_edge, int &r_face) const;
	void _pick_mode(Camera3D *p_cam, const Vector2 &p_pos, Mode p_mode, int &r_vert, hewn::Edge &r_edge, int &r_face) const;
	void _click_select(Camera3D *p_cam, const Vector2 &p_pos, bool p_shift, bool p_ctrl, bool p_alt, bool p_double);
	void _box_select(Camera3D *p_cam, const Rect2 &p_rect, bool p_shift, bool p_ctrl);
	std::vector<int> _selected_verts() const;
	bool _pivot(Vector3 &r_world) const;
	float _handle_size(Camera3D *p_cam, const Vector3 &p_world) const;
	float _gizmo_length(Camera3D *p_cam, const Vector3 &p_world) const;
	float _world_per_pixel(Camera3D *p_cam, const Vector3 &p_world) const;
	int _handle_hit(Camera3D *p_cam, const Vector2 &p_pos) const;
	void _begin_handle_drag(Camera3D *p_cam, int p_axis, bool p_shift);
	void _update_handle_drag(Camera3D *p_cam, const Vector2 &p_pos, bool p_snap, bool p_magnet);
	float _axis_param(Camera3D *p_cam, const Vector2 &p_pos, const Vector3 &p_origin, const Vector3 &p_dir) const;
	bool _plane_hit(Camera3D *p_cam, const Vector2 &p_pos, const Vector3 &p_origin, const Vector3 &p_n, Vector3 &r_hit) const;
	void _draw_handles(Control *p_overlay, Camera3D *p_cam);
	void _set_godot_gizmo_hidden(bool p_hidden);
	void _on_editor_selection_changed();
	void _validate_edited();
	void _on_obj_file(const String &p_path);
	EditorFileDialog *gltf_dialog = nullptr;
	void _on_gltf_file(const String &p_path);
	// Game (hw_game_ui.cpp): the level baked into a plain scene / glTF, lightmap UV2 for all meshes.
	EditorFileDialog *bake_dialog = nullptr;
	EditorFileDialog *scene_gltf_dialog = nullptr;
	void _on_bake_file(const String &p_path);
	void _on_scene_gltf_file(const String &p_path);
	Dictionary _bake_options() const;
	Node3D *_create_parent() const;
	bool _ensure_scene_root();
	void _on_history_version();
	void _early_notice();
	void _on_into_keep_overlap(bool p_on);
	void _add_node_with_undo(Node3D *p_node, const String &p_action, Node3D *p_parent = nullptr);
	mutable ObjectID surface_hit; // the mesh _surface_frame found (a CSG model: new shapes become its brushes)
	ObjectID shape_surface; // ... at the first click of a shape
	ObjectID shape_into; // a shape started in edit mode is built into that mesh
	int shape_into_mode = 0;
	void _build_into(HewnMesh3D *p_target, HewnMesh3D *p_shape);
	int _merge_into(HewnMesh3D *p_target, HewnMesh3D *p_shape);
	// The shape last built into the edited mesh (kept off the tree): its Add panel changes that step of
	// the history in place, until anything else happens.
	HewnMesh3D *into_shape = nullptr;
	ObjectID into_target;
	Dictionary into_before, into_after;
	int64_t into_version = -1;
	bool _into_panel_valid();
	bool _line_cutter(bool p_corridor);
	void _scene_size_report(); // (hw_level.cpp) the nodes whose saved data is biggest
	AcceptDialog *size_dialog = nullptr;
	Tree *size_tree = nullptr;
	Label *size_label = nullptr;
	void _size_item_selected();
	// multi-object editing: Tab with several meshes selected edits them all. edited_id is the
	// active one (what the tools work on); the others are here. Operations run on each, as one undo step.
	std::vector<uint64_t> multi_ids;
	bool multi_running = false; // (run_op going over them)
	bool multi_merge = false; // (the next undo steps of the run join the first)
	// A field dragged (a number field, a slider, a spin box): the edits made meanwhile are one undo step, made
	// when the drag ends (Godot's inspector does the same). Arrow moves in a row: one step (merged ends).
	bool field_drag_pending = false;
	Dictionary field_drag_before;
	String field_drag_name;
	uint64_t field_drag_node = 0;
	bool merge_ends_next = false;
	bool _field_dragging() const;
	void _flush_field_drag();
	std::vector<std::pair<uint64_t, Dictionary>> multi_before; // G / R / S: the others as they were
	std::vector<int> modal_owner; // G / R / S: whose vertex each modal one is (0: the active, k: multi_ids[k - 1])
	bool modal_multi_ok = false;
	std::vector<HewnMesh3D *> edited_meshes() const;
	bool is_edited_mesh(const HewnMesh3D *p_node) const;
	void _set_active_mesh(HewnMesh3D *p_node);
	void _clear_multi();
	void curve_op(const String &p_op); // (hw_level.cpp) curve tools on the selected Path3D nodes
	// the painted channel of a corner (the vertex color or CUSTOM0..3), made when missing
	Color &_paint_value(hewn::Face &p_face, int p_corner, int p_channel);
	Color _paint_brush_value() const; // the color picker's color (times the scale for custom channels)
	void _toggle_vertex_color_view();
	// (hw_profile_editor.cpp) the profile window: columns, sweeps, the bevel's custom profile, doors / arches / wedges
	class HewnProfileDialog *profile_dialog = nullptr;
	void open_profile(const Ref<HewnProfile> &p_profile, const String &p_title, const Rect2 &p_clip, bool p_symmetry = false);
	Ref<HewnProfile> bevel_profile; // the bevel's Custom profile (saved with the project's editor data)
	Ref<HewnProfile> _bevel_profile();
	void _on_bevel_profile_changed();
	void edit_bevel_profile();
	void edit_shape_profile(const String &p_which); // base / shaft / top / sweep / shape on the selected mesh
	void _on_shape_profile_button(int p_index);
	Ref<ShaderMaterial> vcolor_view[5];
	void _clear_into();
	void _rebuild_into();
	std::vector<std::vector<hewn::ops::CutPlane>> _shape_cut_parts(float p_h, bool p_infinite) const;
	void _draw_cut_intersections(Control *p_overlay, Camera3D *p_cam, const std::vector<std::vector<hewn::ops::CutPlane>> &p_parts) const;
	// CSG models
	void _csg_reparent(HewnMesh3D *p_model, bool p_new, Node *p_model_parent, const std::vector<HewnMesh3D *> &p_nodes, const std::vector<int> &p_ops,
			const String &p_action);
	HewnMesh3D *_new_csg_model(const Node3D *p_like) const;
	HewnMesh3D *_brush_at(Camera3D *p_cam, const Vector2 &p_pos, HewnMesh3D *p_model) const;
	void _select_brush_deferred(uint64_t p_id);
	void _check_brush_click(HewnMesh3D *p_selected);
	void _draw_brushes(Control *p_overlay, Camera3D *p_cam);
	bool _object_keys(Key p_code, bool p_ctrl, bool p_shift, bool p_alt);
	bool _any_mode_keys(Key p_code, bool p_ctrl, bool p_shift, bool p_alt);
	// X in object mode: as Godot's Delete, asks first, one undo step
	ConfirmationDialog *delobj_dialog = nullptr;
	void delete_objects_prompt();
	void _delete_objects();
	// arrows / Page Up / Page Down: the selection a grid step along the view's nearest axis;
	// Ctrl: objects copied first, faces extruded
	bool _arrow_keys(Camera3D *p_cam, const Ref<InputEventKey> &p_key);
	bool _alt_arrow_rotate(Camera3D *p_cam, Key p_code);
	// Shift + drag on a face: it moves along its normal (hw_facepush.cpp)
	int push_state = 0; // 0 none, 1 pressed on a face, 2 moving it
	ObjectID push_node, push_hover_node;
	int push_face = -1, push_hover_face = -1;
	Vector3 push_origin, push_n;
	Vector2 push_press;
	Dictionary push_before;
	std::vector<int> push_verts;
	std::vector<Vector3> push_pos;
	std::vector<float> push_lims; // (the move of the plane that brings a corner to the far end of its edge; 0: none)
	std::vector<Vector3> push_dirs; // (local) how each corner moves when the face's plane moves one unit: along the edge
	// that leaves the face there, so the faces around keep their planes
	Vector3 push_n_local;
	bool push_block = false; // Ctrl+Shift: a new block (an object of its own) on the face instead
	bool push_split = false; // Ctrl+Shift dragged inward: the object cut in two, the part dragged over is the new block
	hewn::Mesh push_orig; // the mesh as it was when the drag began
	hewn::Selection push_sel; // and its selection
	int push_plane_face = -1; // the dragged face in push_source when it moves as a plane
	bool push_direct = true; // (pressed on the face itself, not beside the mesh)
	ObjectID push_block_node;
	hewn::Mesh push_source;
	bool push_planes = false; // a convex block: the face moves as a plane, the block made again from its planes
	float push_dist = 0.0f;
	std::vector<HewnMesh3D *> _face_push_meshes() const;
	bool _face_push_pick(Camera3D *p_cam, const Vector2 &p_pos, ObjectID &r_node, int &r_face, Vector3 &r_hit) const;
	void _face_push_hover(Camera3D *p_cam, bool p_shift);
	int32_t _face_push_input(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	void _draw_face_push(Control *p_overlay, Camera3D *p_cam);
	void _face_push_apply(HewnMesh3D *p_n, float p_t);
	// (hw_issues.cpp) Check Scene: the problems of the scene, selected / fixed from a list
	struct Issue {
		ObjectID node;
		String text;
		int fix = 0;
		std::vector<int> faces, verts;
		std::vector<hewn::Edge> edges;
	};
	std::vector<Issue> issues;
	AcceptDialog *issues_dialog = nullptr;
	Tree *issues_tree = nullptr;
	Label *issues_summary = nullptr;
	int issue_pending = -1, issue_pending_frames = 0;
	void open_issues();
	void _issues_scan();
	void _issues_refresh();
	void _issues_fix_all();
	void _on_issue_activated();
	void _on_issue_button(Object *p_item, int p_column, int p_id, int p_mouse);
	void _issue_select(int p_index);
	void _issue_apply_selection(int p_index);
	void _issue_fix(int p_index);
	// Ctrl+Z undoes the selection too. Taken before a click / key in edit mode, an undo step after it
	// when only the selection changed (a press to its release for the box / circle / lasso)
	struct SelSnap {
		ObjectID id;
		Dictionary sel;
	};
	std::vector<SelSnap> sel_undo_before;
	uint64_t sel_undo_version = 0;
	bool sel_undo_pending = false;
	std::vector<SelSnap> _sel_snapshot() const;
	uint64_t _history_version() const;
	void _sel_undo_commit();
	uint64_t brush_click_ms = 0;
	Vector2 brush_click_pos;
	ObjectID brush_click_cam;
	// Visible select in object mode: a click Godot gave to a mesh seen from behind (a room from
	// outside) goes on to what is seen through it. Checked a few frames after the click.
	int vis_click_frames = 0;
	bool plain_press = false; // the last left press went to selection (no tool took it)
	Vector2 vis_click_pos;
	ObjectID vis_click_cam;
	void _check_visible_click();
	Node3D *_object_through(Camera3D *p_cam, const Vector2 &p_pos, HewnMesh3D **r_behind) const;
	std::vector<ObjectID> hidden_by_us; // H: shown again by Alt+H
	std::vector<HewnMesh3D *> _scene_meshes() const;
	void _select_linked_at(Camera3D *p_cam, const Vector2 &p_pos);
	void _click_select_single(Camera3D *p_cam, const Vector2 &p_pos, bool p_shift, bool p_ctrl, bool p_alt, bool p_double);
	void _box_select_single(Camera3D *p_cam, const SelRegion &p_region, bool p_shift, bool p_ctrl);
	void _find_godot_snap(Node *p_row);
	bool _gizmo_on(Tool p_tool) const;
	Basis _gizmo_basis(Camera3D *p_cam) const;
	void _draw_overlays(Control *p_overlay, Camera3D *p_cam);
	void _draw_dimensions(Control *p_overlay, Camera3D *p_cam);
	int32_t _input_impl(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	void _focus_viewport(Camera3D *p_cam);
	Control *_surface_for(Camera3D *p_cam) const;
	Vector2 _wrap_mouse(Camera3D *p_cam, const Vector2 &p_raw);
	void _warp_to(Control *p_surface, const Vector2 &p_from, const Vector2 &p_to);
	bool warp_pending = false;
	Vector2 warp_jump, warp_target;
	uint64_t warp_ms = 0;
	void _place_toolbar();
	// left panels: at the viewport edge, moved aside while Godot's zoom bar shows
	uint64_t zoom_bar_until = 0;
	float zoom_bar_t = 0.0f; // 0 at the edge .. 1 right of the bar
	void _note_zoom_bar(const Ref<InputEvent> &p_event);
	float _left_offset(Control *p_surface, float p_y, float p_h, bool p_can_move = true, float p_extra = 0.0f) const;
	Vector2 _left_panel_pos(Control *p_surface, Control *p_panel) const;
	void _compact_panel(PanelContainer *p_panel, GridContainer *p_body);
	void _compact_label(Label *p_label, const String &p_text);
	void _place_left_panels(double p_delta);
	void _layout_toolbar();
	uint64_t view_menu_id = 0; // Godot's view menu button of the viewport the toolbar sits next to
	uint64_t toolbar_row_id = 0; // the 3D editor's toolbar row; ours floats over the viewports right under it
	void _on_cursor_menu(int p_id);
	void _draw_cursor(Control *p_overlay, Camera3D *p_cam);
	Vector2 _precise_mouse(const Vector2 &p_raw, bool p_shift);
	void _reset_precise();

	// modal (hw_modal.cpp)
	void _gather_snap_candidates(bool p_skip_moving);
	bool _magnet_target(Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_point) const;
	bool _snap_point_at(Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_point) const;
	bool _snap_point_in(const std::vector<Vector3> &p_points, Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_point) const;
	bool _snap_seen(Camera3D *p_cam, const Vector3 &p_world) const; // (not behind a surface, or X-ray)
	Vector3 _constrain(const Vector3 &p_delta) const;
	Vector3 _axis_dir(int p_axis) const;
	bool _modal_number(float &r_value) const;
	bool _start_transform(Modal p_kind, Camera3D *p_cam);
	// G / R / S in object mode: the selected nodes themselves
	std::vector<ObjectID> obj_nodes;
	std::vector<Transform3D> obj_orig; // global transforms at the start
	bool _start_object_transform(Modal p_kind, Camera3D *p_cam);
	void _apply_object_transforms();
	bool _moving_node(const Node *p_node) const;
	void _start_extrude(Camera3D *p_cam);
	void _start_duplicate(Camera3D *p_cam);
	void _start_bevel(Camera3D *p_cam);
	void _start_inset(Camera3D *p_cam);
	void _start_snap(Camera3D *p_cam);
	Modal snap_kind = MODAL_MOVE; // B: the transform the picked base point drives (move / rotate / scale)
	void _apply_snap_base(const Vector3 &p_target, const String &p_constraint);
	void _apply_rotation(const Vector3 &p_axis, float p_deg);
	void _apply_scaling(float p_s);
	void _start_slide(Camera3D *p_cam);
	void _start_rip(Camera3D *p_cam);
	void _update_modal(Camera3D *p_cam, bool p_snap, bool p_magnet);
	void _apply_positions(const std::vector<Vector3> &p_world);
	void _confirm_modal();
	void _cancel_modal();
	int32_t _modal_input(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	void _draw_modal(Control *p_overlay, Camera3D *p_cam);

	// tools (hw_tools.cpp)
	bool _knife_pick(Camera3D *p_cam, const Vector2 &p_pos, bool p_midpoint, KnifeClick &r_click) const;
	bool _knife_pick_constrained(Camera3D *p_cam, const Vector2 &p_pos, bool p_midpoint, KnifeClick &r_click) const;
	std::vector<Vector3> _knife_normals(Camera3D *p_cam, const std::vector<hewn::ops::KnifePoint> &p_pts) const;
	void _apply_knife(Camera3D *p_cam);
	bool _knife_empty(Camera3D *p_cam, const Vector2 &p_pos, KnifeClick &r_click) const;
	void _knife_pieces(Camera3D *p_cam, const std::vector<KnifeClick> &p_cut, std::vector<std::vector<hewn::ops::KnifeHit>> &r_pieces, std::vector<Vector3> &r_splits) const;
	void _apply_loop_cut();
	bool _cutter_begin(Camera3D *p_cam, const Vector2 &p_pos);
	bool _cutter_plane_point(Camera3D *p_cam, const Vector2 &p_pos, Vector2 &r_uv) const;
	std::vector<Vector2> _cutter_outline() const;
	void _apply_cutter();
	bool _poly_plane_point(Camera3D *p_cam, const Vector2 &p_pos, bool p_snap, Vector3 &r_point);
	bool _poly_lifted_point(Camera3D *p_cam, const Vector2 &p_pos, bool p_snap, int p_index, Vector3 &r_point);
	void _finish_poly();
	void _poly_apply_live();
	int32_t _tool_input(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	void _draw_tools(Control *p_overlay, Camera3D *p_cam);

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	static HewnEditorPlugin *singleton; // (the Quad screen uses the dock's icons and the undo history)
	Mode mode = MODE_OBJECT;
	Mode element_mode = MODE_FACE; // restored by Tab
	Tool tool = TOOL_MOVE;
	Vector3 cursor_pos; // the 3D cursor (world)
	Basis cursor_basis; // its orientation (Y along the surface normal it was placed on)

	// hw_transform.cpp
	Basis _orientation_basis(int p_orient, Camera3D *p_cam) const;
	Vector3 _pivot_point(int p_pivot, const std::vector<int> &p_verts) const;
	std::vector<Vector3> _individual_pivots(const std::vector<int> &p_verts, const Vector3 &p_fallback) const;
	static float falloff(int p_type, float p_t, int p_seed);
	void _weighted_verts(const std::vector<int> &p_selected, bool p_prop, float p_radius, int p_falloff, bool p_connected,
			std::vector<int> &r_verts, std::vector<float> &r_weights) const;
	bool snapping_active(bool p_ctrl) const;
	bool snap_increment_on(bool p_ctrl) const; // increment or grid steps
	bool snap_grid_on() const { return dock && (dock->snap_targets & HewnDock::SNAP_GRID); }
	Vector3 _grid_point(const Vector3 &p_point, const Basis &p_axes, float p_step) const;
	bool _snap_target(Camera3D *p_cam, const Vector2 &p_pos, Vector3 &r_point) const;
	void _auto_merge(hewn::Mesh &m, const std::vector<int> &p_moved) const;
	int hover_vert = -1;
	hewn::Edge hover_edge = hewn::Edge(-1, -1);
	int hover_face = -1;
	// the active element: the one clicked last (drawn white / lighter while it stays selected)
	uint64_t active_node = 0;
	int active_vert = -1;
	hewn::Edge active_edge = hewn::Edge(-1, -1);
	int active_face = -1;

	HewnMesh3D *get_edited() const;
	static bool is_param_op(const String &p_op);
	Variant op_param(const String &p_op, const String &p_key);
	void set_op_param(const String &p_op, const String &p_key, const Variant &p_value);
	bool is_modal() const { return modal != MODAL_NONE; }
	Modal get_modal() const { return modal; }
	void set_mode(int p_mode);
	void toggle_edit_mode();
	void toggle_mode(int p_mode);
	int mode_mask = 1 << 3; // element modes shown together (bits 1..3)
	bool has_mode(int p_mode) const { return mode != MODE_OBJECT && (mode_mask & (1 << p_mode)) != 0; }
	bool face_dots_active() const;
	bool overlays_on() const { return dock && dock->overlays && dock->overlays->is_pressed(); }
	// Visible select: rays go through faces seen from behind (into rooms from outside), not in x-ray
	bool cull_back() const { return dock && dock->visible_select && dock->visible_select->is_pressed() && !(dock->xray && dock->xray->is_pressed()); }
	// the wireframe overlay: its opacity, 0 when off
	float wireframe_opacity() const { return dock && dock->overlay(dock->ov_wireframe) && dock->ov_wire_opacity ? (float)dock->ov_wire_opacity->get_value() : 0.0f; }
	bool show_face_fill() const;
	void set_tool(int p_tool);
	void selection_changed();
	void set_xray(bool p_on);
	void create_custom_orientation();
	enum ToolId {
		TOOLCOL_SELECT,
		TOOLCOL_CURSOR,
		TOOLCOL_RULER,
		TOOLCOL_KNIFE,
		TOOLCOL_LOOPCUT,
		TOOLCOL_CUTTER,
		TOOLCOL_BISECT,
		TOOLCOL_SPIN,
		TOOLCOL_SEL_CIRCLE,
		TOOLCOL_SEL_LASSO,
		TOOLCOL_BRUSH,
		TOOLCOL_PENCIL,
		TOOLCOL_FILL,
		TOOLCOL_PICKER,
		TOOLCOL_NORMALS,
		TOOLCOL_COUNT,
	};
	bool tool_shown(int p_tool) const; // the tools of the mode (object / edit / vertex paint)
	void select_tool(int p_tool);
	// (hw_spin.cpp) the Spin tool
	int spin_axis = 2;
	bool spin_dragging = false, spin_started = false, spin_hover = false;
	float spin_angle = 0.0f, spin_prev = 0.0f, spin_from = 0.0f;
	void start_spin_tool();
	Vector3 _spin_axis(Camera3D *p_cam) const;
	bool _spin_mouse_angle(Camera3D *p_cam, const Vector2 &p_pos, float &r_angle) const;
	int32_t _spin_input(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	void _draw_spin(Control *p_overlay, Camera3D *p_cam);
	// (hw_facecut.cpp) the Face Cutter
	int fc_phase = 0, fc_hover = -1, fc_edge = -1, fc_ticks = 1, fc_d45 = 0;
	float fc_angle = 0.0f; // the line's turn from straight across the edge (degrees, left: +)
	std::vector<std::pair<int, int>> fc_border; // (face, corner) of the selection's border edges
	std::vector<std::pair<Vector3, Vector3>> fc_lines; // the clicked lines: a point on the edge, a direction (local)
	bool fc_cur_valid = false;
	Vector3 fc_cur_point, fc_cur_dir;
	void start_face_cutter();
	bool _fc_edge_frame(int p_border, Vector3 &r_a, Vector3 &r_b, Vector3 &r_n, Vector3 &r_in) const;
	void _fc_update_line(Camera3D *p_cam);
	void _fc_apply();
	int32_t _facecut_input(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	void _draw_facecut(Control *p_overlay, Camera3D *p_cam);
	int active_tool() const;
	void run_bar_op(const String &p_op); // from the Mesh / Object menus of the toolbar
	bool xray_on = false;
	uint64_t origin_dots_hash = 0;
	std::vector<ObjectID> xray_meshes; // (made see-through by it)

	void begin_edit(const String &p_name);
	void end_edit();
	void cancel_edit(); // (back to the state at begin_edit, nothing in the history)
	void run_op(const String &p_op);
	// A material on the selected faces (edit mode) or the selected objects; the number of faces, -1 when
	// nothing is selected.
	int apply_material_to_selection(const Ref<Material> &p_material);
	void add_from_menu(const String &p_kind); // (dock buttons) as the Add menu (Shift+A) adds it
	// A texture dropped on a 3D view (screen position): the face under the mouse in edit mode (the whole
	// selection if that face is in it), else the object (the selected ones if it is one of them); -1: nothing there
	int apply_material_at_screen(const Ref<Material> &p_material, const Vector2 &p_screen);
	void _commit_material(const Ref<Material> &p_material, const std::vector<HewnMesh3D *> &p_targets, const std::vector<PackedInt32Array> &p_faces);
	void create_shape(int p_type);
	void start_poly_tool(bool p_edit_existing);
	void start_knife();
	void apply_world_uv(); // (UV tab) the selected faces / objects in world space at the Pixels of new objects
	void start_loop_cut();
	void start_cutter();
	void start_ruler();
	void start_shape_tool(int p_type);
	void start_paint();
	// (hw_vpaint.cpp) Vertex Paint mode
	bool vpaint = false;
	enum PaintTool {
		PT_BRUSH,
		PT_PENCIL, // one element, no falloff
		PT_FILL, // the selection, or what is linked to the face under the mouse
		PT_PICKER,
		PT_NORMALS, // smooth (LMB) / flat (Ctrl+LMB) shading where it goes
	};
	int paint_tool = PT_BRUSH;
	bool paint_rmb_was = false;
	float paint_pressure_now = 1.0f;
	Ref<Curve> paint_pressure_curve;
	int paint_view = 0; // 0 the materials, 1 the painted channel, 2 smooth / flat, 3 the normals as colors, 4 single color, 5 random
	int paint_view_frames = 0;
	Ref<ShaderMaterial> normal_view_mat;
	Ref<ShaderMaterial> solid_view_mat[2]; // the Shading menu's Single, Random
	void _apply_solid_params(); // their color / world lighting / cavity from the editor settings
	std::vector<HewnMesh3D *> _view_meshes() const; // the scene's meshes and the shape being drawn (the views go on them)
	void _view_new_mesh(HewnMesh3D *p_mesh); // a mesh just shown: the view on it at once (not after the next check)
	void _on_node_added(Node *p_node);
	void _view_new_mesh_id(uint64_t p_id);
	void _save_view_state(); // the gizmo on / off and the snap steps into the editor settings
	int paint_alpha_view = 0; // the channel view: 0 RGBA (the color, stripes where see-through), 1 RGB, 2 A
	std::vector<ObjectID> paint_view_ids;
	Ref<ShaderMaterial> paint_view_mat;
	int normals_group = 0; // (a smoothing stroke: the group it gives)
	AcceptDialog *replace_dialog = nullptr;
	ColorPickerButton *replace_from = nullptr, *replace_to = nullptr;
	SpinBox *replace_tol = nullptr;
	CheckBox *replace_only_sel = nullptr;
	AcceptDialog *round_dialog = nullptr;
	SpinBox *round_step = nullptr;
	CheckBox *round_only_sel = nullptr;
	void _round_colors();
	void _paint_selected_corners(HewnMesh3D *p_node, bool p_only_selection, std::vector<char> &r_on) const;
	EditorFileDialog *paint_file_dialog = nullptr;
	bool paint_file_save = false;
	bool isolated = false;
	std::vector<RID> isolate_hidden;
	void enter_vpaint();
	void exit_vpaint(); // (to object mode)
	void start_paint_tool(int p_tool);
	int _paint_comps() const; // the values of the painted channel (its format)
	bool _paint_channel_on(int p_k) const;
	void _paint_stroke(Camera3D *p_cam, const Vector2 &p_pos, bool p_first);
	void _paint_fill(Camera3D *p_cam, const Vector2 &p_pos);
	bool _paint_pick(Camera3D *p_cam, const Vector2 &p_pos, bool p_second);
	void _paint_apply_weights(HewnMesh3D *p_node, const std::vector<std::vector<float>> &p_w, float p_scale);
	void _paint_weights(Camera3D *p_cam, const Vector2 &p_pos, bool p_single, std::vector<std::vector<float>> &r_w);
	bool paint_no_spread = false; // (the hover highlight: only the element itself, not its whole vertices)
	void apply_paint_view();
	void _clear_paint_view();
	void _replace_colors();
	void _on_paint_file(const String &p_path);
	void paint_more_op(int p_id);
	void set_isolate(bool p_on);
	// (hw_curve_edit.cpp) a Path3D's points edited curves (Tab on it)
	ObjectID curve_id;
	bool curve_edit = false;
	std::vector<int> curve_sel;
	int curve_xf = 0; // 1 move, 2 turn, 3 scale
	int curve_axis = -1;
	int curve_extruded = -1;
	Vector2 curve_mouse0, curve_press;
	bool curve_pressing = false, curve_box = false;
	Vector3 curve_center;
	std::vector<Vector3> curve_start;
	Ref<Curve3D> curve_before;
	Path3D *_curve_path() const;
	void _curve_commit(const String &p_name);
	int32_t _curve_input(Camera3D *p_cam, const Ref<InputEvent> &p_event);
	void _draw_curve_edit(Control *p_overlay, Camera3D *p_cam);
	// the Blend Material window: a base and up to four layers by a painted channel's components
	AcceptDialog *blend_dialog = nullptr;
	OptionButton *blend_source = nullptr;
	EditorResourcePicker *blend_base = nullptr;
	CheckBox *blend_on[4] = {};
	EditorResourcePicker *blend_tex[4] = {};
	CheckBox *blend_height = nullptr;
	OptionButton *blend_filter = nullptr;
	CheckBox *blend_vcolor = nullptr;
	CheckBox *blend_pbr = nullptr; // metallic / roughness per texture
	CheckBox *blend_normal = nullptr; // normal maps
	CheckBox *blend_emission = nullptr;
	void open_blend_dialog();
	void _make_blend_material();
	void focus_selection(Camera3D *p_cam);
	int32_t _vpaint_keys(Camera3D *p_cam, const Ref<InputEventKey> &p_key);
	void _draw_paint_squares(Control *p_overlay, Camera3D *p_cam);
	// (hw_marks.cpp) many small marks in one draw under the overlay: slot 0 the X-ray face dots, 1 the normals
	// view's vertices, 2 the color squares
	struct Mark {
		Vector2 at;
		float half = 4.0f;
		Color color;
		float shape = 0.0f, alpha = 1.0f, border = 0.0f;
	};
	struct MarkSlot {
		RID item, mm;
		uint64_t key = 0;
		int count = -1;
		bool used = false, shown = false;
	};
	struct MarkLayer {
		ObjectID overlay;
		MarkSlot slots[3];
	};
	std::vector<MarkLayer> mark_layers;
	Ref<ShaderMaterial> mark_material;
	Ref<ArrayMesh> mark_quad;
	MarkSlot *_mark_slot(Control *p_overlay, int p_slot);
	void _marks_begin(Control *p_overlay);
	void _marks_end(Control *p_overlay);
	bool _marks_cached(Control *p_overlay, int p_slot, uint64_t p_key);
	void _marks_set(Control *p_overlay, int p_slot, uint64_t p_key, const std::vector<Mark> &p_marks);
	void _marks_free();
	uint64_t _marks_view_key(Camera3D *p_cam, HewnMesh3D *p_node) const;
	void _draw_over_viewport(Control *p_overlay, Camera3D *p_cam);
	// the edited mesh's vertices on the screen and their face corners, kept while the view and the mesh stay
	struct PaintView {
		uint64_t key = 0, counts_key = 0;
		std::vector<Vector2> sp;
		std::vector<char> front, vis; // (front: a shown face has it and it is in front of the camera)
		bool vis_done = false;
		std::vector<int> off, cf, ci; // (the corners of vertex v: off[v]..off[v + 1])
		std::vector<std::pair<int, int>> counts; // (split mode: the vertices with corners of several colors)
	};
	PaintView pview;
	const PaintView &_paint_view_cache(Camera3D *p_cam, bool p_visibility);
	void _draw_channel_bars(Control *p_overlay, const Vector2 &p_at);
	void start_bisect();
	void start_cut_tool(int p_kind); // 1 box, 2 cylinder, 3 prism
	void start_clip();
	void start_prefab(const String &p_path);
	void start_placing(const String &p_label, const std::function<Node3D *()> &p_make, bool p_along_normal, bool p_random = false, const Transform3D &p_local = Transform3D());
	void place_entity(int p_index); // HewnDock::entity_defs()[p_index]
	void place_decal(); // the Create tab's picture as an overlay or a decal
	void place_cutter(); // a HewnCutter3D placed like a prefab
	void place_kind(const String &p_kind); // a light / sound placed like a prefab
	void add_node_kind(const String &p_kind); // Shift+A: a node at the 3D cursor (or placed: lights, decals...)
	String image_add_kind; // the image picked next becomes this (sprite / anim_sprite / image_plane)
	EditorFileDialog *image_dialog = nullptr;
	void _on_images_picked(const PackedStringArray &p_paths);
	void _on_image_picked(const String &p_path);
	void place_decal_kind(bool p_overlay);
	void apply_cutters(); // the selected cutters' holes into the meshes (undoable)
	Ref<Material> tool_material(const String &p_kind); // "clip", "trigger" (made once, saved)
	void apply_tool_material(const String &p_kind);
	int select_overlapping(); // edit mode: selects faces lying on top of each other; their number
	// Grid on faces (Alt+G): a world grid drawn over the Hewn meshes (an overlay of the render
	// instances, not saved in the scene)
	bool face_grid = false;
	Ref<ShaderMaterial> face_grid_material;
	// edit mode: placed scenes (prefabs) are dimmed, they are not edited here
	uint64_t dim_key = 0;
	std::vector<ObjectID> dimmed;
	Ref<StandardMaterial3D> dim_material;
	void _update_library_dim();
	double face_grid_timer = 0.0;
	void set_face_grid(bool p_on);
	void _update_face_grid();
	void uv_tool(const String &p_kind); // "center", "fit", "random" on the selected faces / objects
	// Texture tools on the selected faces / objects, with undo: "op:arg:arg..." (HewnMesh3D::edit_uvs),
	// "step_move:du:dv", "step_rotate:sign", "step_scale:axis:sign" (times the dock steps), "inset:sign",
	// "extend", "unwrap", "grab", "transfer", "modal:kind".
	void run_uv_command(const String &p_command);
	void make_csg_model(); // the selected meshes become the brushes of a new CSG model
	void add_subdivision_modifier(); // a Subdivision Surface modifier on each selected mesh (undoable)
	void _order_bottom_docks();
	void sweep_along_selected_path(); // a new Sweep along the selected Path3D
	void walls_around_floor(); // (hw_floorplan.cpp) closed walls along the outlines of the selected floor
	void bake_level(); // asks where to save the baked copy of the scene
	void export_scene_gltf(); // asks where to save the whole scene as glTF
	void lightmap_all(); // lightmap UV2 on for every mesh of the scene (undoable)
	void csg_set_operation(int p_op); // = - \ on the selected brushes (others join a model)
	void hide_objects(int p_kind); // 0: the selected, 1: the others, 2: show them again
	void flip_objects(int p_axis);
	void snap_selection_to_grid();
	void start_uv_modal(int p_kind);
	void cancel_tool();

	bool _handles(Object *p_object) const override;
	void _edit(Object *p_object) override;
	void _make_visible(bool p_visible) override;
	String _get_plugin_name() const override;
	int32_t _forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) override;
	// Godot's viewport zooms on Ctrl+wheel before it would reach the plugin: in Vertex Paint the wheel over a 3D
	// viewport is taken here first
	void _input(const Ref<InputEvent> &p_event) override;
	// Editor Settings > Hewn: the subdivision warning, the header and the tool column on / off
	void register_editor_settings();
	void _on_editor_settings();
	bool quad_added = false; // the Quad screen (Editor Settings > Hewn > Interface > Quad View) is a plugin of its own
	void _sync_quad();
	Node *selftest = nullptr; // (tests/editor_selftest.gd, only with HW_SELFTEST and the other probe variables)
	bool show_header = true;
	bool show_tools = true;
	// Element Inspector (hw_element_inspector.cpp): all that is known of the selection, bottom right, dragged
	uint64_t ei_hash = 0;
	Dictionary ei_open; // section -> unfolded
	uint64_t _ei_state_hash() const;
	void _ei_update(bool p_force);
	void _ei_section_toggled(bool p_on, const String &p_name);
	void _ei_apply(const String &p_what, const Variant &p_value);
	void _ei_set_float(double p_value, const String &p_what);
	void _ei_set_bool(bool p_on, const String &p_what);
	void _ei_set_int(int p_index, const String &p_what);
	void _ei_set_color(const Color &p_color, const String &p_what);
	void _ei_set_material(const Ref<Resource> &p_res);
	void _ei_open_material(const Ref<Resource> &p_res, bool p_inspect);
	bool redo_panel_on = true; // (Editor Settings > Hewn > Interface > Last Operation Panel)
	bool wrap_cursor_on = true; // (Editor Settings > Hewn > Interface: the cursor comes back at the other edge while dragging)
	bool hotkeys_on = true; // (Editor Settings > Hewn > Interface > Hotkeys: off, no key starts a Hewn tool)
	static int64_t subdivide_limit();
	int64_t _subdiv_added(int p_cuts) const;
	int64_t _heavy_added(int p_value) const;
	String heavy_op = "subdivide_faces";
	Label *heavy_key_label = nullptr;
	bool heavy_ok = false; // (asked and confirmed: this one run goes through)
	bool heavy_redo = false; // (it came from the panel)
	bool heavy_wait = false; // (asked once the mouse is let go)
	int heavy_cuts_wanted = 1;
	int heavy_prev_cuts = 1; // (what the mesh has now)
	ConfirmationDialog *heavy_dialog = nullptr;
	// Bake Light to Vertex Colors (hw_bakelight.cpp): the scene's lights, ambient and occlusion into the colors
	ConfirmationDialog *lbake_dialog = nullptr;
	Label *lbake_info = nullptr;
	OptionButton *lbake_target = nullptr;
	ColorPickerButton *lbake_ambient = nullptr;
	bool lbake_ambient_set = false;
	SpinBox *lbake_ao_rays = nullptr;
	SpinBox *lbake_ao_dist = nullptr;
	CheckBox *lbake_shadows = nullptr;
	SpinBox *lbake_strength = nullptr;
	CheckBox *lbake_only_sel = nullptr;
	CheckBox *lbake_show = nullptr;
	CheckBox *lbake_use_ambient = nullptr;
	CheckBox *lbake_use_dir = nullptr;
	CheckBox *lbake_use_point = nullptr;
	CheckBox *lbake_refine = nullptr;
	SpinBox *lbake_threshold = nullptr;
	SpinBox *lbake_passes = nullptr;
	SpinBox *lbake_budget = nullptr;
	CheckBox *lbake_mesh_shadows = nullptr;
	SpinBox *lbake_amb_k = nullptr;
	SpinBox *lbake_dir_k = nullptr;
	SpinBox *lbake_omni_k = nullptr;
	SpinBox *lbake_spot_k = nullptr;
	CheckBox *lbake_use_spot = nullptr;
	void open_bake_light();
	void bake_light();
	SpinBox *heavy_cuts = nullptr;
	Label *heavy_label = nullptr;
	void _heavy_popup();
	void _on_heavy_cuts(double p_cuts);
	void _on_heavy_ok();
	void _on_heavy_cancel();
	void _forward_3d_force_draw_over_viewport(Control *p_overlay) override;
	void _process(double p_delta) override;
};

} // namespace godot
