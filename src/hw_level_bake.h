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
	static Node *bake_scene(Node *p_root, const Dictionary &p_options = Dictionary());
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
	// Turns lightmap UV2 on for every HewnMesh3D under the root; returns how many changed.
	static int enable_lightmap_uv2(Node *p_root, float p_texel_size = 0.2f);
};

} // namespace godot
