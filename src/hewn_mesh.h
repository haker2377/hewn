// HewnMesh3D — editable polygon mesh node.
#pragma once

#include "hw_core.h"
#include "hw_modifiers.h"
#include "hw_profile.h"

#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/area3d.hpp>
#include <godot_cpp/classes/shape3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

class HewnMesh3D : public MeshInstance3D {
	GDCLASS(HewnMesh3D, MeshInstance3D)

public:
	enum CollisionMode {
		COLLISION_NONE = 0,
		COLLISION_TRIMESH = 1,
		COLLISION_CONVEX = 2,
		COLLISION_SIMPLIFIED_CONVEX = 3, // the hull with fewer points
		COLLISION_MULTIPLE_CONVEX = 4, // a few convex pieces (Godot's decomposition)
		COLLISION_BOX = 5, // the bounds
		COLLISION_CAPSULE = 6, // fitted to the bounds, along an axis
		COLLISION_CYLINDER = 7,
		COLLISION_SPHERE = 8,
		COLLISION_PRIMITIVE = 9, // a box / cylinder / sphere / capsule shape: exactly its parameters
		COLLISION_SIMPLIFIED_TRIMESH = 10, // the triangles simplified to a ratio
	};
	enum BooleanOp {
		BOOLEAN_UNION = 0,
		BOOLEAN_SUBTRACT = 1,
		BOOLEAN_INTERSECT = 2,
	};

private:
	hewn::Mesh data;
	int shape = hewn::shapes::CUBE;
	Dictionary shape_params;
	PackedVector3Array poly_points; // outline of the Poly Shape (local XZ plane)
	NodePath follow_curve; // (Wall, Sweep, Path Stairs: their points from this curve's first spline, kept up with it)
	bool _pull_follow_points();
	// Column parts and the Sweep's custom cross section (saved resources: a profile can be reused).
	Ref<HewnProfile> base_profile, shaft_profile, top_profile, sweep_profile, shape_profile;
	Ref<Mesh> sweep_mesh; // (a sweep with the profile Mesh: set along its path)
	Ref<Mesh> accent_mesh; // (a sweep's accents of the shape Mesh)
	PackedInt32Array path_gaps; // (a sweep's segments left out: segment i from point i to the next)
	void _place_mesh(const Ref<Mesh> &p_mesh, hewn::Mesh &r_out); // its faces' material: the slot of their surface's
	void _on_profile_changed();
	TypedArray<Material> materials;
	CollisionMode collision_mode = COLLISION_NONE;
	float collision_ratio = 0.5f; // (Simplified Trimesh)
	int collision_max_pieces = 8; // (Multiple Convex)
	int collision_axis = 0; // (Capsule, Cylinder: 0 the longest, 1 X, 2 Y, 3 Z)
	Dictionary collision_metadata; // put on the collision body (here and in the baked level)
	PackedStringArray collision_groups;
	Ref<Script> collision_script; // (on the game's collision body)
	bool merge_at_bake = true;
	uint64_t collision_key = 0; // (the shapes kept while the mesh and the settings stay)
	struct CollisionPart {
		Ref<Shape3D> shape;
		Transform3D xf;
	};
	std::vector<CollisionPart> collision_parts;
	bool _primitive_collision(std::vector<CollisionPart> &r_parts, const Vector3 &p_mid) const;
	StaticBody3D *body = nullptr;
	bool has_world_uv = false;
	// World UVs stay put when the object moves (object mode, the game): the first move keeps where it was before
	// as the place they are projected from; "Apply World UV" lets them follow the world again.
	Transform3D uv_anchor;
	bool uv_anchored = false;
	Transform3D uv_last_xf; // (where it was at the last build: the anchor of the first move)
	uint64_t uv_enter_frame = 0; // (moved in the frame it came in: it is being placed, not moved)
	bool initialized = false;

	hewn::BuildOut last_build;
	hewn::Mesh eval_mesh; // what is shown when it differs from `data` (modifiers, CSG brushes)
	hewn::BuildOut eval_build;
	bool has_eval = false;
	Ref<ArrayMesh> shown_array_mesh; // the mesh shown, refilled by rebuild()
	uint64_t revision = 0;

	// Modifier stack: applied in order to a copy for display, collision
	// and export.
	TypedArray<HewnModifier> modifiers;
	PackedInt32Array legacy_bool_ops; // (old boolean_ops property, used when boolean_targets is set)
	std::vector<std::pair<ObjectID, Transform3D>> path_watched; // Path3D nodes and their place relative to us
	bool boolean_cutter = false; // used as a boolean operand: see-through in the editor, hidden in the game
	// a CSG model shows its own geometry combined with its HewnMesh3D children (the
	// brushes) in tree order, each by its brush_operation. Brushes are see-through in the editor and not
	// part of the game; hidden ones are left out. Inverted: the world is solid and the brushes the other
	// way round (subtractive brushes make the rooms, seen from inside).
	bool csg_model = false;
	bool csg_inverted = false;
	int brush_operation = BOOLEAN_UNION;
	mutable std::vector<Ref<Material>> csg_materials; // the brushes' materials, after our own slots
	void _apply_brushes(hewn::Mesh &r_mesh) const;
	void _update_brush_display();
	std::vector<std::pair<ObjectID, std::pair<uint64_t, Transform3D>>> bool_signature; // operands at the last build
	bool bool_force = false;
	bool bool_dirty = false;
	std::vector<ObjectID> bool_watched;
	void _watch_operands();
	void _on_operand_changed();
	void _bool_refresh();
	void _apply_cutter_display();
	void _on_modifier_changed();
	template <typename T>
	T *_first_modifier() const;
	template <typename T>
	T *_ensure_modifier();
	void _remove_modifier(const Ref<HewnModifier> &p_mod);

	static Ref<StandardMaterial3D> default_material;

	// Non-destructive cutters reaching it (HewnCutter3D): what they were at the last build.
	std::vector<std::pair<uint64_t, Transform3D>> cut_signature;
	bool cuts_dirty = false;
	void _cuts_refresh();
	void _apply_cutters(hewn::Mesh &r_mesh) const;
	bool _has_cutters() const;

	// Lightmap UVs (UV2) of what is shown, saved in mesh_data with a hash of the geometry they were made
	// for: LightmapGI bakes the node as it is, and the scene loads with the same UV2.
	bool lightmap_uv2 = false;
	float lightmap_texel = 0.2f; // meters per texel
	std::vector<std::vector<Vector2>> uv2_faces;
	uint64_t uv2_hash = 0;
	Vector2i uv2_size;
	const std::vector<std::vector<Vector2>> *_lightmap_uvs(const hewn::Mesh &p_mesh);
	void _fill_array_mesh(const Ref<ArrayMesh> &p_mesh, const hewn::BuildOut &p_build, bool p_skip_tools) const;

	void _regenerate_shape();
	void center_auto_textures();
	Node3D *stairs_cutter = nullptr; // (internal: the headroom over the treads, cut out of the floors above)
	void _update_stairs_cutter();
	void _build_shape();
	void _update_collision();
	std::vector<Ref<Material>> period_materials; // (watched for their UV scale)
	void _update_uv_periods();
	void _on_period_material_changed();
	void _update_collision(const hewn::BuildOut *p_build, const hewn::Mesh *p_mesh);
	std::vector<float> _shape_values() const;

protected:
	static void _bind_methods();
	void _notification(int p_what);
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_world_uv_all(float p_scale); // every projected face from world space, this many tiles per meter
	void unfold_uvs(const PackedInt32Array &p_faces); // the image running on across these faces' edges (empty: all)
	static std::vector<HewnMesh3D *> all_meshes; // (in the tree: the cutters look through them)
	void _cuts_changed(); // a cutter moved / changed near it: rebuilt later in the frame if it matters
	AABB get_data_aabb() const;
	bool is_cut() const { return !cut_signature.empty(); }
	// Editor-side selection (not saved).
	hewn::Selection selection;

	static Ref<Material> get_default_material();
	static void free_static_resources();

	// C++ access for the editor.
	hewn::Mesh &get_data() { return data; }
	const hewn::Mesh &get_data() const { return data; }
	const hewn::BuildOut &get_build() const { return last_build; }
	const hewn::Mesh &get_shown_data() const { return has_eval ? eval_mesh : data; } // with the modifiers
	const hewn::BuildOut &get_shown_build() const { return has_eval ? eval_build : last_build; }
	void commit(bool p_geometry_edit = true); // rebuild after editing `data`; geometry edits turn the shape into Custom
	// Only vertices moved (same faces): positions and normals shown at once, the rest (UVs, collision, ...) made
	// again by finish_positions() when the move ends. Falls back to commit() where it can't.
	void commit_positions();
	bool dyntopo_refine(const Vector3 &p_center, float p_radius, float p_max_len, bool p_subdivide = true, bool p_collapse = true);
	void finish_positions();
	bool positions_pending = false;
	bool interactive_light = false; // (while a change goes on: the heavy modifiers left out)
	bool has_heavy_modifiers() const;
	void commit_interactive(); // faces changed during a stroke: shown at once, the heavy modifiers when it ends
	// UV lock (sticker mode): between begin and end, faces that move keep the UVs they had at
	// the start (the texture moves, turns and stretches with them).
	void uv_lock_begin();
	void uv_lock_end();

	void rebuild();
	uint64_t get_revision() const { return revision; } // counts rebuilds (any change of the mesh)
	bool has_modifiers() const;
	hewn::Mesh evaluated_mesh() const; // data with the modifiers applied
	void set_modifiers(const TypedArray<HewnModifier> &p_modifiers);
	TypedArray<HewnModifier> get_modifiers() const;
	void add_modifier(const Ref<HewnModifier> &p_modifier);
	// Mirror modifiers with Clipping: moved vertices that were on the plane stay on it, and no vertex
	// goes across it (p_before: their local positions before the move).
	void apply_mirror_clipping(const std::vector<int> &p_verts, const std::vector<Vector3> &p_before);
	// A node path of a modifier: relative to this node, or else to the scene root.
	Node *resolve_path(const NodePath &p_path) const;
	void apply_boolean(hewn::Mesh &r_mesh, HewnMesh3D *p_other, int p_op) const;

	void set_mirror_x(bool p_on);
	bool get_mirror_x() const;
	void set_mirror_y(bool p_on);
	bool get_mirror_y() const;
	void set_mirror_z(bool p_on);
	bool get_mirror_z() const;
	void set_mirror_merge(float p_dist);
	float get_mirror_merge() const;
	void set_solidify_thickness(float p_thickness);
	float get_solidify_thickness() const;
	void set_solidify_offset(float p_offset);
	float get_solidify_offset() const;
	void apply_modifiers();
	void set_boolean_targets(const TypedArray<NodePath> &p_targets);
	TypedArray<NodePath> get_boolean_targets() const;
	void set_boolean_ops(const PackedInt32Array &p_ops);
	PackedInt32Array get_boolean_ops() const;
	void set_boolean_cutter(bool p_on);
	bool is_boolean_cutter() const;
	void add_boolean(HewnMesh3D *p_other, int p_op); // non-destructive (boolean_with bakes)
	static bool boolean_step(hewn::Mesh &r_mesh, const hewn::Mesh &p_other, int p_op); // (bounds check first)
	void set_csg_model(bool p_on);
	bool is_csg_model() const;
	void set_csg_inverted(bool p_on);
	bool is_csg_inverted() const;
	void set_brush_operation(int p_op);
	int get_brush_operation() const;
	HewnMesh3D *get_csg_model() const; // the model this is a brush of (its parent), or null
	bool is_brush() const { return get_csg_model() != nullptr; }
	std::vector<HewnMesh3D *> get_brushes() const; // the brushes that take part, in order
	TypedArray<Node> get_brush_nodes() const;

	void set_shape(int p_shape);
	int get_shape() const;
	void set_shape_params(const Dictionary &p_params);
	Dictionary get_shape_params() const;
	void set_poly_points(const PackedVector3Array &p_points);
	void set_follow_curve(const NodePath &p_path);
	NodePath get_follow_curve() const { return follow_curve; }
	void follow_changed(); // (the curve it follows changed / moved)
	PackedVector3Array get_poly_points() const;
	void set_base_profile(const Ref<HewnProfile> &p_profile);
	Ref<HewnProfile> get_base_profile() const { return base_profile; }
	void set_shaft_profile(const Ref<HewnProfile> &p_profile);
	Ref<HewnProfile> get_shaft_profile() const { return shaft_profile; }
	void set_top_profile(const Ref<HewnProfile> &p_profile);
	Ref<HewnProfile> get_top_profile() const { return top_profile; }
	void set_shape_profile(const Ref<HewnProfile> &p_profile);
	Ref<HewnProfile> get_shape_profile() const { return shape_profile; }
	void set_sweep_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_sweep_mesh() const { return sweep_mesh; }
	PackedInt32Array get_inner_faces() const; // the faces no one can see (inside the solid)
	void set_accent_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_accent_mesh() const { return accent_mesh; }
	void set_path_gaps(const PackedInt32Array &p_gaps);
	PackedInt32Array get_path_gaps() const { return path_gaps; }
	static void mesh_to_hewn(const Ref<Mesh> &p_mesh, hewn::Mesh &r_out);
	void set_sweep_profile(const Ref<HewnProfile> &p_profile);
	Ref<HewnProfile> get_sweep_profile() const { return sweep_profile; }
	// The column's styles as editable profiles (then saved, reused, changed point by point).
	void make_profiles_editable();
	void set_shape_param(const String &p_name, float p_value);
	float get_shape_param(const String &p_name) const;

	void set_mesh_data(const Dictionary &p_data);
	Dictionary get_mesh_data() const;

	void set_materials(const TypedArray<Material> &p_materials);
	TypedArray<Material> get_materials() const;
	Ref<Material> get_material_for_slot(int p_slot) const;
	int add_material_slot(const Ref<Material> &p_material); // returns slot (reuses existing)

	void set_collision_mode(CollisionMode p_mode);
	void set_collision_ratio(float p_value);
	float get_collision_ratio() const { return collision_ratio; }
	void set_collision_max_pieces(int p_value);
	int get_collision_max_pieces() const { return collision_max_pieces; }
	void set_collision_axis(int p_value);
	int get_collision_axis() const { return collision_axis; }
	void set_collision_metadata(const Dictionary &p_value);
	Dictionary get_collision_metadata() const { return collision_metadata; }
	void set_collision_groups(const PackedStringArray &p_value);
	PackedStringArray get_collision_groups() const { return collision_groups; }
	void set_collision_script(const Ref<Script> &p_value) { collision_script = p_value; }
	Ref<Script> get_collision_script() const { return collision_script; }
	void set_game_script(const Ref<Script> &p_value) { set_script(p_value); } // (the node's own script, where it is seen with its metadata)
	Ref<Script> get_game_script() const { return get_script(); }
	void set_merge_at_bake(bool p_value) { merge_at_bake = p_value; }
	bool get_merge_at_bake() const { return merge_at_bake; }
	bool can_primitive_collision() const; // a parametric box / cylinder / sphere / capsule
	Array get_collision_shapes() const; // [{shape, transform}, ...] (the baked level makes a body of these)
	void apply_collision_tags(Node *p_body) const; // the collision's metadata and groups onto a body
	CollisionMode get_collision_mode() const;
	void _shape_lost(); // (a primitive collision: its triangles once the shape is edited)
	void set_lightmap_uv2(bool p_on);
	bool get_lightmap_uv2() const;
	void set_lightmap_texel_size(float p_size);
	float get_lightmap_texel_size() const;
	Vector2i get_lightmap_size() const { return uv2_size; }
	// What the game shows: tool faces left out, hidden faces in, with UV2 when it is on.
	Ref<ArrayMesh> get_game_mesh();
	Transform3D uv_transform() const; // where world UVs are projected from (the anchor, or where it is)
	void set_uv_anchor(const Transform3D &p_xf) { uv_anchor = p_xf; uv_anchored = true; }
	Transform3D get_uv_anchor() const { return uv_anchor; }
	void set_uv_anchored(bool p_on);
	bool is_uv_anchored() const { return uv_anchored; }
	void prepare_bake(); // the cutters' cut and the collision now (not on the next frame)
	// The collision shape the node uses in the game (null without collision), and its trigger shape.
	Ref<Shape3D> get_collision_shape() const;
	Ref<Shape3D> get_trigger_shape() const;

	Dictionary get_state() const;
	void set_state(const Dictionary &p_state);
	// only the selection
	Dictionary get_selection_state() const;
	void set_selection_state(const Dictionary &p_state);

	PackedInt32Array get_edge_loop(int p_a, int p_b) const;
	PackedInt32Array get_edge_ring(int p_a, int p_b) const;
	PackedInt32Array get_face_loop(int p_face, int p_a, int p_b) const;
	PackedInt32Array grow_faces(const PackedInt32Array &p_faces, float p_max_angle) const;
	PackedInt32Array get_linked_faces(const PackedInt32Array &p_faces) const;

	// Script API
	int get_vertex_count() const;
	int get_face_count() const;
	PackedVector3Array get_vertices() const;
	void set_vertices(const PackedVector3Array &p_vertices, bool p_positions_only = false);
	Vector3 get_vertex(int p_index) const;
	void set_vertex(int p_index, const Vector3 &p_pos);
	PackedInt32Array get_face(int p_face) const;
	int add_face(const PackedInt32Array &p_indices, int p_material);
	int add_vertex(const Vector3 &p_pos);
	void clear_mesh();
	int get_face_material(int p_face) const;
	int get_face_role(int p_face) const; // hewn::FaceRole
	// Tool materials: a material with the meta "hw_tool" (any value; "clip" now): collides, not drawn in the
	// game - an invisible wall. (Old "trigger" ones: no collision.)
	static String tool_of(const Ref<Material> &p_material);
	void apply_material(const Ref<Material> &p_material, const PackedInt32Array &p_faces);
	void compact_materials();
	void set_face_material(int p_face, int p_material);
	int get_face_smoothing(int p_face) const;
	void set_face_smoothing(int p_face, int p_group);
	void set_face_color(int p_face, const Color &p_color);
	Vector3 get_face_normal(int p_face) const;
	Vector3 get_face_center(int p_face) const;

	PackedInt32Array extrude_faces(const PackedInt32Array &p_faces, float p_distance, bool p_individual, bool p_dissolve_orthogonal = false);
	PackedInt32Array inset_faces(const PackedInt32Array &p_faces, float p_amount, bool p_individual);
	void delete_faces(const PackedInt32Array &p_faces);
	void flip_faces(const PackedInt32Array &p_faces);
	PackedInt32Array subdivide_faces(const PackedInt32Array &p_faces);
	PackedInt32Array subdivide_faces_ex(const PackedInt32Array &p_faces, int p_cuts, float p_smoothness, bool p_ngons, float p_fractal, float p_along_normal, int p_seed);
	PackedInt32Array triangulate_faces(const PackedInt32Array &p_faces);
	PackedInt32Array merge_faces(const PackedInt32Array &p_faces);
	PackedInt32Array detach_faces(const PackedInt32Array &p_faces);
	void conform_normals();
	void recalculate_normals(bool p_inside = false);
	int make_face(const PackedInt32Array &p_vertices);
	int limited_dissolve(float p_angle = 5.0f);
	PackedInt32Array poke_faces(const PackedInt32Array &p_faces);
	PackedInt32Array bridge_faces(const PackedInt32Array &p_faces);
	PackedInt32Array split_edges(const PackedInt32Array &p_edge_pairs);
	PackedInt32Array bevel_vertices(const PackedInt32Array &p_vertices, float p_amount);
	void set_edge_attributes(int p_a, int p_b, bool p_seam, bool p_sharp, float p_crease, float p_bevel_weight);
	Dictionary get_edge_attributes(int p_a, int p_b) const;
	PackedInt32Array bisect(const Vector3 &p_point, const Vector3 &p_normal, bool p_clear_inner = false, bool p_clear_outer = false, bool p_fill = false);
	bool move_face_plane(int p_face, float p_delta, bool p_collapse = false); // (a convex block: the faces as its planes then meet)
	// Level design (local space). Holes come back flattened: count, vertices..., count, vertices...
	PackedInt32Array cut_parts(const std::vector<std::vector<hewn::ops::CutPlane>> &p_parts, const PackedInt32Array &p_faces);
	PackedInt32Array cut_box(const AABB &p_box, const PackedInt32Array &p_faces);
	PackedInt32Array bridge_holes(const PackedInt32Array &p_a, const PackedInt32Array &p_b);
	PackedInt32Array make_alcove(const PackedInt32Array &p_loop, float p_depth);
	PackedInt32Array fill_hole(const PackedInt32Array &p_loop);
	bool wrap_texture(int p_from, int p_to, bool p_material);
	PackedVector2Array get_face_uvs(int p_face) const; // the final UVs of its corners
	bool uv_lock_on = false;
	std::vector<Vector3> uv_lock_pos;
	std::vector<std::vector<int>> uv_lock_faces;
	std::vector<std::vector<Vector2>> uv_lock_uvs;
	void _uv_lock_apply();
	// H in edit mode: hidden faces are left out of what is shown (and picked) while the editor
	// edits the mesh (set_hiding_faces); the game and object mode show them.
	void set_faces_hidden(const PackedInt32Array &p_faces, bool p_hidden);
	bool is_face_hidden(int p_face) const;
	void set_hiding_faces(bool p_on);
	void refresh_display() { _apply_cutter_display(); } // (see-through operands / brushes, after the editor's x-ray)
	bool hiding_faces = false;
	// Texture tools on faces (all when empty), in texture tiles (the material's
	// own tiling divided out). Ops: move (du, dv), rotate (deg), scale (su, sv), mirror_u, mirror_v, cycle,
	// edge (hu, hv: -1 / 1 to that edge of the tile, 0 = stay), justify (the same per face, 0 = center),
	// fit (u, v), inset (pixels; negative expands), transfer (u0, v0, u1, v1). Rotate / scale / mirror /
	// cycle work around the middle of all the faces (args 2, 3 / 1, 2: another center).
	bool edit_uvs(const PackedInt32Array &p_faces, const String &p_op, const Array &p_args);
	Rect2 get_uv_bounds(const PackedInt32Array &p_faces) const; // in tiles
	PackedInt32Array unwrap_quads(const PackedInt32Array &p_faces, int p_start); // fitted to one tile
	static std::vector<std::vector<int>> unpack_loops(const PackedInt32Array &p_packed);
	PackedInt32Array bevel_edges(const PackedInt32Array &p_edge_pairs, float p_amount, int p_segments = 1);
	PackedInt32Array loop_cut(int p_a, int p_b, int p_cuts = 1, float p_t = 0.5f);
	PackedInt32Array arch_edges(const PackedInt32Array &p_edge_pairs, int p_segments = 8, float p_rise = 1.0f);
	// Becomes a Sweep along the curve of a Path3D (its points where it is now, in this node's space).
	void sweep_along(Node3D *p_path);
	PackedInt32Array knife_cut(const PackedVector3Array &p_points, const PackedInt32Array &p_faces, const Vector3 &p_view_dir);
	bool boolean_with(HewnMesh3D *p_other, int p_op);
	PackedInt32Array extrude_edges(const PackedInt32Array &p_edge_pairs, float p_distance);
	PackedInt32Array connect_edges(const PackedInt32Array &p_edge_pairs);
	PackedInt32Array insert_edge_loops(const PackedInt32Array &p_edge_pairs);
	PackedInt32Array subdivide_edges(const PackedInt32Array &p_edge_pairs, int p_cuts, bool p_ngons = true);
	PackedInt32Array bridge_edges(const PackedInt32Array &p_edge_pairs);
	PackedInt32Array dissolve_edges(const PackedInt32Array &p_edge_pairs);
	void dissolve_vertices(const PackedInt32Array &p_vertices);
	int to_circle(const PackedInt32Array &p_vertices, const PackedInt32Array &p_faces, bool p_regular = true, float p_influence = 1.0f);
	PackedInt32Array get_face_triangles(int p_face) const; // vertex index triplets (counter-clockwise)
	Vector3 get_shape_anchor() const; // local point that stays when the shape parameters change (first step of stairs)
	PackedInt32Array weld_vertices(const PackedInt32Array &p_vertices, float p_distance);
	PackedInt32Array collapse_vertices(const PackedInt32Array &p_vertices);
	PackedInt32Array split_vertices(const PackedInt32Array &p_vertices);
	PackedInt32Array connect_vertices(const PackedInt32Array &p_vertices);
	PackedInt32Array fill_holes();
	void subdivide_smooth();
	void auto_smooth(float p_angle);
	int quadify(float p_angle);

	void freeze_transform();
	void set_pivot(const Vector3 &p_local_point);
	void center_pivot();
	void merge_from(HewnMesh3D *p_other);
	// The other mesh's geometry added (where it is now); faces lying on faces turned the other way are cut
	// from both unless p_keep_overlap. Returns the first new face.
	int join_mesh(HewnMesh3D *p_other, bool p_keep_overlap = false);

	Ref<ArrayMesh> to_array_mesh() const;
	String to_obj() const;
	Error export_obj(const String &p_path) const;
	Error export_gltf(const String &p_path) const;
	void create_from_mesh(const Ref<Mesh> &p_mesh, bool p_quadify);
	int custom_formats[4] = { -1, -1, -1, -1 }; // CUSTOM0..3 in the built mesh (Mesh::ArrayCustomFormat, -1: none)
	void set_custom_formats(const PackedInt32Array &p_formats);
	PackedInt32Array get_custom_formats() const;
	void set_custom_value(int p_channel, const PackedInt32Array &p_verts, const Color &p_value);
	String get_info() const;

	HewnMesh3D();
	~HewnMesh3D();
};

} // namespace godot

VARIANT_ENUM_CAST(HewnMesh3D::CollisionMode);
VARIANT_ENUM_CAST(HewnMesh3D::BooleanOp);
