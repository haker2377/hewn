// FBX export (binary FBX 7.4, the version other programs read): the MeshInstance3D nodes under a node, each a model
// with its triangles, normals, UVs and materials (their color and albedo texture). Written here from the format
// itself; Godot reads FBX but does not write it.
#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/string.hpp>

namespace hewn {

godot::Error write_fbx(godot::Node *p_root, const godot::String &p_path);

} // namespace hewn
