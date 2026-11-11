// HewnLevel — getting a level built with Hewn ready for the game.
#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>

namespace godot {

class HewnLevel : public RefCounted {
	GDCLASS(HewnLevel, RefCounted)

protected:
	static void _bind_methods();

public:
	// A copy of the scene for the game: every HewnMesh3D becomes a plain MeshInstance3D (tool faces
	// left out, lightmap UV2 kept) with a StaticBody3D for its collision, an Area3D for its trigger faces
	// and (option "occluders") an OccluderInstance3D of its opaque faces. CSG brushes and boolean cutters
	// are left out (their model / target has them). Overlays become meshes too. Other nodes are copied;
	// placed scenes stay instances. Options: occluders (bool, true), collision (bool, true).
	// Also: occluder_min_size (meters: smaller things have none), and those of optimize_game_nodes.
	static Node *bake_scene(Node *p_root, const Dictionary &p_options = Dictionary());
	// The game's nodes made better (hw_bake_optimize.cpp). Options (all off unless set): merge (the meshes that can be, joined and
	// cut into squares of merge_cell meters), merge_plain (plain MeshInstance3D too), merge_collision, lod, vis_range, hidden_faces.
	static void optimize_game_nodes(Node *p_root, const Dictionary &p_options);
	// The bake options as first offered: for a level, or for a glTF file.
	static Dictionary bake_defaults(bool p_gltf = false);
	// As last chosen for this project (hewn/bake or hewn/bake_gltf in the Project Settings), else those.
	static Dictionary project_bake_options(bool p_gltf = false);
	// The game's copy of one object (a mesh, a curve, an overlay), not added anywhere; nullptr if it has none.
	static Node *game_copy(Node *p_node);
	// bake_scene saved as a scene file (.tscn / .scn).
	static Error save_baked_scene(Node *p_root, const String &p_path, const Dictionary &p_options = Dictionary());
	// The baked scene as glTF 2.0 (.glb / .gltf).
	static Error export_scene_gltf(Node *p_root, const String &p_path, const Dictionary &p_options = Dictionary());
	// The scene for the game, in place: every Hewn node becomes plain Godot nodes (a mesh: MeshInstance3D with a
	// StaticBody3D for its collision; an entity: Node3D in its class's group, its properties as metadata; an
	// overlay: a MeshInstance3D; cutters, brushes, boolean cutters, reference pictures go). The scene must be in the
	// tree (the cutters cut in world space). Returns the root (a new one when the root itself was replaced), sets
	// r_changed. The game then needs no Hewn at all.
	static Node *to_game_nodes(Node *p_root, bool &r_changed);
	static Node *_to_game_nodes_bound(Node *p_root) { bool c = false; return to_game_nodes(p_root, c); } // (tests)
	// Turns lightmap UV2 on for every HewnMesh3D under the root; returns how many changed.
	static int enable_lightmap_uv2(Node *p_root, float p_texel_size = 0.2f);
};

} // namespace godot
