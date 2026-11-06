# Hewn - Level and Mesh Editor for Godot

Hewn builds levels and models meshes right inside the Godot 4.7 editor: draw rooms and shapes, cut doors and
windows, edit vertices, edges and faces, set up textures and UVs, paint vertex colors, and export the result.

> **Version 0.1 - early release.** Things can still go wrong: save your scenes often and keep backups
> (or use version control). Bug reports are very welcome.

## Features

- **Polygon modeling** on `HewnMesh3D` nodes: vertex / edge / face modes, move / rotate / scale with
  snapping and axis constraints, extrude, inset, bevel, loop cut, knife, bridge, merge, dissolve, spin,
  proportional editing, X-ray, hide / reveal, mirror and other modifiers.
- **Level tools**: parametric shapes drawn on surfaces (boxes, stairs, arches, doors, columns, walls along
  a path, stairs along a path...), box / cylinder / prism cuts with corridors and alcoves, non-destructive
  cutters, a clip tool, Shift + drag to push faces, arrows to nudge and Alt + arrows to turn things on the grid.
- **Textures and UVs**: a texture browser, a texture per face, world-space UVs that work like classic brush
  editors, UV tools, a UV editor, lightmap UV2.
- **Vertex painting** with brushes, channels and blend materials.
- **Scenes and objects**: a scene browser to place prefabs, decals and overlays, entities, lights, a scene check.
- **Quad view**: top, front and side views next to a 3D view for blocking out
  (it can be turned off in **Editor Settings > Hewn > Interface > Quad View**).
- **Export**: glTF, OBJ, a baked copy of the level.

## Install

1. Download the archive from the **Releases** page and copy `addons/hewn` into your project.
   (The source code alone, "Code > Download ZIP", has no compiled library: build it first, see below.)
2. Open **Project > Project Settings > Plugins** and enable **Hewn**.
3. Add a `HewnMesh3D` node, or draw a shape: **Shift + A** in the 3D view.

Hewn is an **editor tool only**. When you export your game, its nodes are turned into plain Godot nodes
(`MeshInstance3D` with `StaticBody3D` collision, `Node3D` entities with groups and metadata), and the Hewn
library does not go into the game. While exporting, Godot prints *"No suitable library found for GDExtension"*
for Hewn: this is expected. To silence it, exclude `addons/hewn/*` in the export preset's **Resources** tab.

## Quick start

| Key | What it does |
|---|---|
| Tab | edit mode on / off |
| 1 / 2 / 3 | vertices / edges / faces |
| G / R / S | move / rotate / scale (X / Y / Z to constrain, type a number) |
| E, I, Ctrl+B | extrude, inset, bevel |
| K, Ctrl+R | knife, loop cut |
| C | box cut |
| Shift + A | add a shape or a node |
| Shift + drag on a face | push the face along its normal (Ctrl + Shift: a new block) |
| Arrows, Page Up / Down | nudge by the grid step (Alt: quarter turns) |
| Shift + Tab or Y | snapping on / off |

Texturing (edit mode, faces):

| Key | What it does |
|---|---|
| Alt + click | put the texture of the selected face on the clicked face, the pattern running on across edges |
| Ctrl + Alt + click | the same, UV only (the material stays) |
| Alt + right click | take the texture from the clicked face |
| T | the texture against the face edge under the mouse: W / A / S / D picks the side of the image, Q / E fit the height / width, R: scale 1, click applies, Esc cancels |
| U | the UV tools as a menu, with Move / Rotate / Scale Texture: the texture follows the mouse (X / Y: axis, Ctrl: steps) |

The interface is in English, or in Russian when the Godot editor runs in Russian. A detailed manual (in Russian)
is in [docs/manual_ru.md](docs/manual_ru.md).

## Build from source

Requirements: Python 3, SCons and a C++17 compiler.

```
git clone https://github.com/godotengine/godot-cpp
python -m pip install scons
scons platform=windows target=editor     # or platform=linux / platform=macos
```

The library is written to `project/addons/hewn/bin/`. The `project/` folder is a test project.

## Tests

```
godot --headless --path project --import
godot --headless --path project -s res://tests/run_tests.gd   # core: operations, saving
HW_SELFTEST=1 godot --headless --editor --path project        # editor: real input in the 3D view
```

## License

MIT, see [LICENSE](LICENSE). Third-party parts are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
