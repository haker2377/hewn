// Hewn "Quad" main screen (next to 2D / 3D / Script): four views like a classic level editor -
// Top, Front and Side 2D views with a grid and a 3D camera view. Select / move / resize / rotate objects
// on the grid, draw blocks, drag vertices and clip meshes in the 2D views.
#pragma once

#include "../hewn_mesh.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/sub_viewport_container.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include <utility>
#include <vector>

namespace godot {

class HewnEditorPlugin;
class HewnQuadView;

// One of the four views: a SubViewport rendering the edited scene, our grid behind it (2D views) and an
// overlay on top that draws the selection / tools and takes the input.
class HewnQuadPane : public Control {
	GDCLASS(HewnQuadPane, Control)

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	enum Kind {
		VIEW_3D,
		VIEW_TOP,
		VIEW_FRONT,
		VIEW_SIDE,
	};
	Kind kind = VIEW_3D;
	HewnQuadView *view = nullptr;
	SubViewportContainer *container = nullptr;
	SubViewport *viewport = nullptr;
	Camera3D *camera = nullptr;
	Control *overlay = nullptr;
	// 2D views: world axes of screen right / screen up / towards the viewer, the center (u, v) and the zoom
	Vector3 u, v, w;
	Vector2 center;
	float zoom = 50.0f; // pixels per meter

	bool is_2d() const { return kind != VIEW_3D; }
	Vector2 to_screen(const Vector3 &p) const;
	Vector2 to_uv(const Vector2 &p_screen) const;
	Vector2 uv_of(const Vector3 &p) const { return Vector2(p.dot(u), p.dot(v)); }
	Vector3 from_uv(const Vector2 &p_uv, float p_depth) const { return u * p_uv.x + v * p_uv.y + w * p_depth; }
	String title() const;
	void setup(Kind p_kind, HewnQuadView *p_view);
	void sync_camera();
	void zoom_at(const Vector2 &p_screen, float p_factor);
};

class HewnQuadView : public VBoxContainer {
	GDCLASS(HewnQuadView, VBoxContainer)

public:
	enum Tool {
		TOOL_SELECT,
		TOOL_BLOCK,
		TOOL_VERTEX,
		TOOL_CLIP,
	};
	enum ClipMode {
		CLIP_BOTH,
		CLIP_FRONT,
		CLIP_BACK,
	};
	HewnEditorPlugin *plugin = nullptr;

private:
	enum Drag {
		DRAG_NONE,
		DRAG_PAN, // 2D views: middle mouse / Space + left mouse
		DRAG_PENDING, // pressed on an object: a move once the mouse travels, a click otherwise
		DRAG_MOVE,
		DRAG_RESIZE,
		DRAG_ROTATE,
		DRAG_MARQUEE,
		DRAG_BLOCK_NEW,
		DRAG_BLOCK_MOVE,
		DRAG_BLOCK_RESIZE,
		DRAG_VERTS_PENDING,
		DRAG_VERTS,
		DRAG_CLIP,
		DRAG_CAMERA, // the 3D camera marker in a 2D view
		DRAG_SPLIT,
		DRAG_ORBIT, // 3D view: middle mouse
		DRAG_PAN_3D, // 3D view: Shift + middle mouse
		DRAG_FLY, // 3D view: right mouse + WASD
	};
	struct Item {
		Node3D *node = nullptr;
		AABB box; // world bounds of its visuals
		HewnMesh3D *hewn = nullptr;
	};
	struct Saved {
		uint64_t id = 0;
		Transform3D xf;
		Dictionary state; // Hewn meshes: the mesh before the drag
		bool hewn = false;
	};

	HewnQuadPane *panes[4] = {};
	Control *area = nullptr;
	Label *status = nullptr;
	Button *tool_buttons[4] = {};
	OptionButton *shape_option = nullptr;
	OptionButton *clip_option = nullptr;
	OptionButton *grid_option = nullptr;
	CheckBox *snap_check = nullptr;
	Button *rotate_button = nullptr;
	Ref<Environment> env_2d;

	Tool tool = TOOL_SELECT;
	ClipMode clip_mode = CLIP_BOTH;
	float grid = 0.25f;
	Vector2 split = Vector2(0.5f, 0.5f);
	int maximized = -1;
	bool handle_rotate = false; // clicking the selection again switches scale / rotate handles

	// the current drag
	Drag drag = DRAG_NONE;
	HewnQuadPane *drag_pane = nullptr;
	Vector2 press_pos; // screen, in the pane
	Vector2 last_pos;
	Vector2 press_uv;
	int drag_handle = -1;
	int split_axes = 0; // DRAG_SPLIT: 1 = vertical line, 2 = horizontal line
	bool drag_ctrl = false;
	bool drag_shift = false;
	Node3D *press_item = nullptr;
	AABB drag_box; // the box being moved / resized, at the press
	std::vector<Saved> saved;
	std::vector<Node *> clones; // Shift + drag: the copies being moved
	std::vector<uint64_t> clone_sources; // (selected again when the copy is cancelled)
	bool pending_toggle = false;
	float rotate_start = 0.0f;
	float drag_angle = 0.0f;
	Vector3 grab_world; // vertex tool: the vertex that was grabbed
	HewnQuadPane *hover_pane = nullptr;
	Vector2 hover_pos;

	// block tool: the box to create (Enter), shown in every view
	bool has_block = false;
	AABB block;
	AABB last_block = AABB(Vector3(0, 0, 0), Vector3(1, 1, 1));
	// vertex tool: selected vertices of the selected Hewn meshes
	std::vector<std::pair<uint64_t, std::vector<int>>> vsel;
	// clip tool: the line in one 2D view
	bool has_clip = false;
	HewnQuadPane *clip_pane = nullptr;
	Vector3 clip_a, clip_b;

	// 3D view camera
	float fly_speed = 6.0f;
	float orbit_distance = 6.0f;
	bool camera_initialized = false;

	// items of the edited scene, rebuilt once per frame
	std::vector<Item> items;
	uint64_t items_frame = ~0ULL;

	// ---- helpers
	const std::vector<Item> &get_items();
	const Item *item_of(Node3D *p_node);
	std::vector<Node3D *> selected_nodes() const;
	bool is_selected(Node3D *p_node) const;
	void set_selection(const std::vector<Node3D *> &p_nodes);
	bool selection_bounds(AABB &r_box);
	bool snapping() const;
	float snap(float p_value) const;
	float min_size() const;
	Rect2 screen_rect(HewnQuadPane *p, const AABB &p_box) const;
	void box_uv(HewnQuadPane *p, const AABB &p_box, Vector2 &r_min, Vector2 &r_max) const;
	AABB box_from_uv(HewnQuadPane *p, const Vector2 &p_min, const Vector2 &p_max, const AABB &p_depth_from) const;
	int handle_at(HewnQuadPane *p, const AABB &p_box, const Vector2 &p_pos) const;
	Vector2 handle_pos(const Rect2 &p_rect, int p_handle) const;
	Node3D *pick_2d(HewnQuadPane *p, const Vector2 &p_pos);
	Node3D *pick_3d(HewnQuadPane *p, const Vector2 &p_pos);
	void world_edges(const Item &p_item, PackedVector3Array &r_lines) const;
	static void mesh_edges(const hewn::Mesh &p_mesh, const Transform3D &p_xf, PackedVector3Array &r_lines);
	std::vector<HewnMesh3D *> selected_meshes() const;
	std::vector<int> *vsel_of(HewnMesh3D *p_mesh);
	void clean_vsel();
	EditorUndoRedoManager *undo_redo() const;

	// ---- edits
	void save_selection();
	void restore_saved();
	void apply_scale(Node3D *p_node, const Saved &p_before, const Basis &p_scale, const Vector3 &p_offset);
	void apply_rotation(Node3D *p_node, const Saved &p_before, const Basis &p_rot, const Vector3 &p_pivot);
	void commit_saved(const String &p_action);
	void start_clone();
	void delete_selection();
	void nudge(HewnQuadPane *p, const Vector2 &p_dir);
	void create_block();
	void apply_clip();
	void focus_selection();
	void move_verts(HewnQuadPane *p, const Vector2 &p_pos);
	void select_verts_in(HewnQuadPane *p, const Rect2 &p_rect, bool p_add);
	void _set_owners(const Array &p_pairs);
	static Array owners_of(Node *p_node);

	// ---- input / drawing
	void _pane_input(const Ref<InputEvent> &p_event, HewnQuadPane *p);
	void _input_2d(HewnQuadPane *p, const Ref<InputEvent> &p_event);
	void _input_3d(HewnQuadPane *p, const Ref<InputEvent> &p_event);
	bool _shortcut(HewnQuadPane *p, const Ref<InputEvent> &p_event);
	void _press_2d(HewnQuadPane *p, const Vector2 &p_pos, bool p_ctrl, bool p_shift);
	void _drag_2d(HewnQuadPane *p, const Vector2 &p_pos);
	void _release_2d(HewnQuadPane *p, const Vector2 &p_pos);
	void _cancel_drag();
	void _pane_draw(HewnQuadPane *p);
	void _draw_2d(HewnQuadPane *p);
	void _draw_3d(HewnQuadPane *p);
	void _draw_box_2d(HewnQuadPane *p, const AABB &p_box, const Color &p_color, bool p_handles, bool p_rotate);
	void _draw_clip(HewnQuadPane *p);
	void _pane_mouse_entered(HewnQuadPane *p);
	void _area_input(const Ref<InputEvent> &p_event);
	void _layout();
	void _update_status();
	void _update_toolbar();
	void _on_tool(int p_tool);
	void _on_grid(int p_index);
	void _on_clip_mode(int p_index);
	void _on_rotate_toggled(bool p_on);
	void _save_settings();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	Ref<Environment> get_env_2d() const { return env_2d; }
	float get_grid() const { return grid; }
	void setup(HewnEditorPlugin *p_plugin);
	void set_tool(int p_tool);
	void shown(bool p_visible);
	void draw_grid(HewnQuadPane *p);
};

class HewnQuadPlugin : public EditorPlugin {
	GDCLASS(HewnQuadPlugin, EditorPlugin)

	HewnQuadView *view = nullptr;
	bool screen_visible = false;

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	bool _has_main_screen() const override { return true; }
	String _get_plugin_name() const override { return "Quad"; }
	Ref<Texture2D> _get_plugin_icon() const override;
	void _make_visible(bool p_visible) override;
	bool _handles(Object *p_object) const override;
	void _edit(Object *p_object) override {}
};

} // namespace godot
