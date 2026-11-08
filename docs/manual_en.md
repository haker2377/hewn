# Hewn Manual

Hewn is a plugin for Godot 4.7 for making and editing meshes and game levels in the Godot editor.

## Installation

1. Download the [latest build](https://github.com/haker2377/hewn/releases) or [build the plugin from source](#building).
2. Copy the `addons` folder into the root folder of your project.
3. Enable the plugin in Project -> Project Settings -> Plugins.

## Building

```
git clone https://github.com/godotengine/godot-cpp
python -m pip install scons
scons platform=windows target=editor     # or linux / macos
```

The built library goes to `project/addons/hewn/bin/`.

# Usage

## Creating an Object

### Add Menu
**Shortcut:** Shift+A

A menu of everything that can be added to the scene: meshes, a Path3D curve, lights, a camera, sound, images, decals,
cuts, Area3D. The "+" button on the Hewn header opens the same menu.

With "Create at 3D Cursor" on in the menu, the object appears at the 3D cursor with its default size. With it off, the
shape is drawn with the mouse.

### Drawing a Shape

The first click places one corner of the base, the second click the opposite corner, the third the height. Dragging
works too. For round shapes the first click places the center.

- Shift on the first click - a base at an angle: the first side sets the direction.
- Ctrl before the first click - draw on the plane of the face under the mouse, extended.
- Alt - draw on the plane of the previous shape.
- Q / E or R - turn a shape that has a direction (stairs, door, prism).
- The height can be typed as a number.
- Enter instead of drawing - a shape of the default size at the 3D cursor.

After the shape is made, its parameter panel opens at the bottom left. It opens again when the shape is selected, as
long as its geometry has not been changed in edit mode.

A shape drawn in edit mode is added to the edited mesh.

### Box
**Shortcut:** B

Draws a cube.

### Flip
**Shortcut:** W (right after a box)

Turns the faces of the new box inward, making a room.

### Shapes

- **Cube**
- **Plane** - a single face.
- **Cylinder**
- **Cone**
- **Sphere**
- **Icosphere**
- **Torus**
- **Pipe** - a cylinder with a hole.
- **Prism** - a gable roof.
- **Stairs** - straight, curved or spiral.
- **Arch**
- **Door** - a wall with an opening.
- **Capsule**
- **Disc** - a circle or a ring.
- **Wedge**
- **Polygon** - a regular polygon, flat or with a height.
- **Column** - base, shaft and top, each from its own profile.

### Poly Shape

An outline from points, with a height. Clicks place the points, a click on the first point or Enter closes the
outline, then the mouse sets the height.

The points can be changed with "Edit Base Points" in the parameter panel: drag a point to move it, click a point to
delete it, click an edge to add one.

### Wall

A wall along a line. Clicks place the corners; Enter, RMB or a double click finishes; Backspace removes the last
corner.

### Sweep

A profile along a line: railings, curbs, pipes, cornices. Drawn the same way as a wall.

### Path Stairs

Stairs along a line: flights on the segments, landings at the turns. Drawn the same way as a wall.

### Walls Around Floor

Places walls along the outline of the selected floor or of the selected faces. With edges selected, the walls go only
along them.

### Sweep Along Path3D

A sweep along the curve of the selected Path3D.

## Modes

### Edit Mode
**Shortcut:** Tab

Turns editing of the vertices, edges and faces of the selected mesh on and off. With several meshes selected, all of
them are edited. Picking another node in the scene tree turns the mode off.

Object mode, edit mode and Vertex Paint can also be picked in the menu at the start of the Hewn header.

### Vertices, Edges, Faces
**Shortcuts:** 1 / 2 / 3

What gets selected in edit mode. Shift+1 / 2 / 3 or Shift+click on a mode button turns on several at once.

## Selection

### Select
**Shortcuts:** click, box

Shift+click adds to the selection or removes from it. The element clicked last is the active one: a vertex or an
edge is white, a face has white edges and a lighter fill.

### Loop
**Shortcuts:** Alt+click, double click

Selects a loop of edges or faces.

### Ring
**Shortcut:** Ctrl+Alt+click

Selects a ring of edges or faces.

### Shortest Path
**Shortcuts:** Ctrl+click, Ctrl+Shift+click

Ctrl+click selects the elements between the selected one and the clicked one. Ctrl+Shift+click selects every
shortest path between them: on a grid, the whole rectangle.

### All / None / Invert
**Shortcuts:** A / Alt+A / Ctrl+I

### Grow / Shrink
**Shortcuts:** Ctrl+= / Ctrl+-

Adds the neighboring elements to the selection or removes the outer ones.

### Linked
**Shortcuts:** L / Ctrl+L

L selects the connected part under the mouse, Ctrl+L the part connected to the selection.

### Border

Selects the open edges of the mesh: edges with only one face.

### Checker Deselect

Deselects every other element.

### Same Material / Same Normal

Selects faces with the same material or facing the same way. Same Normal has a maximum angle and "Connected only"
in the Last Operation panel.

### Invalid UVs

Selects faces whose UVs have no area.

### Overlapping Faces

Selects faces lying on top of each other.

### Hide / Reveal
**Shortcuts:** H / Shift+H / Alt+H

Hides the selected faces or the other faces, or shows everything again. Hidden faces are hidden only in edit mode.

### Visible Selection

The eye button on the header. With it on, clicks go through faces whose back is toward the camera, so a room can be
edited from outside.

### X-Ray
**Shortcut:** Alt+Z

Selection goes through the mesh.

## Move, Rotate, Scale

### Move / Rotate / Scale
**Shortcuts:** G / R / S

Works on the selected elements in edit mode and on the selected objects in object mode.

- X / Y / Z - an axis, Shift+X / Y / Z - a plane, pressed again - the local axis.
- A number or an expression can be typed.
- Ctrl - snapping the other way, Shift - slower.
- LMB or Enter - apply, RMB or Esc - cancel.

### Slide
**Shortcuts:** G G

Vertices slide along their edges. In edge mode the edges slide. C - the vertices go past the ends of the edges, along
their lines. Snapping works: to the grid in grid steps along the edge, to vertices and edges at the point of the edge
nearest to the target.

### Base Point
**Shortcut:** B (during G / R / S)

A click picks the point to move from, a second click the point to move to. Moves, rotates or scales by these points.

### Arrow Nudge
**Shortcuts:** arrows, Page Up / Page Down

Moves the selection by one grid step along the world axis closest to the view. With Ctrl, object mode moves a copy and
face mode extrudes the faces by one step.

### Arrow Rotate
**Shortcuts:** Alt+arrows, Alt+Page Up / Page Down

Rotates the selection by 90°.

### Gizmo
**Shortcut:** W

Switches the gizmo: move, rotate, scale, no gizmo. Shift+drag of a gizmo handle extrudes. The gizmo size is set in
the Godot editor settings (Editors -> 3D -> Manipulator Gizmo Size).

### Orientation

A menu on the header: global, local, normal, gimbal, view, cursor, parent. "Create from Selection" adds an
orientation of your own from the selection.

### Pivot Point

A menu on the header: bounding box center, 3D cursor, individual origins, median point, active element.

### Proportional Editing
**Shortcuts:** O / Alt+O

The neighboring elements move along with the selection, less the farther away they are. Alt+O - connected ones only.
The wheel during G / R / S changes the size of the area. The Custom Curve falloff is set by a curve of its own: x the
distance from the middle (0) to the edge (1), y the strength.

### Auto Merge

A button on the header. Vertices that end up next to each other after a move are merged.

## Snapping

### Snapping
**Shortcuts:** Y, Shift+Tab

Turned on with Godot's magnet. The snapping menu on the header picks what to snap to: increment, grid, vertex, edge,
face, edge center, face center.

### Grid Step
**Shortcuts:** [ / ]

Makes the grid step half or twice as big. The current step is shown next to the snapping menu.

### 3D Cursor
**Shortcuts:** Shift+RMB, Shift+S

Shift+RMB places the 3D cursor on the surface. Shift+S - the cursor menu: to the selection, to the world origin, the
selection to the cursor.

## View

### Shading

A menu on the header: materials, attribute color, smooth / flat faces, normals, single color, random color. For
single color it also has world space lighting (with its turn) and cavity (ridges lighter, valleys darker; Forward+ or
Mobile renderer only). The color itself is set in the Editor Settings.

### Overlays

A menu on the header: wireframe, 3D cursor, normals, measurements of the selection. "Collision" shows the collision
shapes of the objects. "Dimensions" writes the size of the selection along X, Y and Z next to the outer edges of its
bounds.

## Vertices

Mesh -> Vertex menu.

### Extrude Vertices
**Shortcut:** E

Extrudes the vertices into new edges.

### Bevel Vertices
**Shortcut:** Ctrl+Shift+B

Cuts off the corners at the selected vertices.

### Push/Pull

Moves the vertices toward the center of the selection or away from it.

### Shrink/Fatten
**Shortcut:** Alt+S

Moves the vertices along their normals.

### Shear

Shears the selection.

### Slide Vertices
**Shortcut:** Shift+V

Moves a vertex along one of its edges.

### Randomize

Moves the vertices by a random distance.

### Mirror

Mirrors the selection along an axis.

### To Circle

Puts the selected vertices on a circle.

### Connect
**Shortcut:** J

Connects the selected vertices with edges across the faces.

### Rip
**Shortcut:** V

Tears the selection off on the side of the mouse and moves it.

### Split

Gives each face around a vertex its own copy of the vertex.

### Merge
**Shortcut:** M

Merges the selected vertices: at the center, at the cursor, at the first, at the last, by distance.

### Weld (by distance)

Merges vertices closer than the given distance.

### Vertex Crease

The vertex stays sharp when subdivided.

### Separate
**Shortcut:** P

Moves the selection into a new mesh. It can also separate by material or by loose parts.

### Dissolve Vertices
**Shortcut:** Ctrl+X

Removes the vertices, the faces around them merge.

### Delete Vertices
**Shortcuts:** X, Delete

Deletes the vertices and their faces. X opens the delete menu.

## Edges

Mesh -> Edge menu.

### Extrude Edges
**Shortcut:** E

Extrudes the edges into new faces.

### Bevel Edges
**Shortcut:** Ctrl+B

Bevels the edges. The wheel or +/- changes the number of segments.

### Bridge Edge Loops

Connects two edge loops with faces.

### Sweep Along Edges

A sweep (a cornice) along the selected edges, outside the faces next to them. In face mode: along the border of the
selected faces. Its profile and size are then in the shape panel.

### New Face from Edges
**Shortcut:** F

Makes a face from the selected edges or vertices. Two vertices - an edge between them.

### Subdivide

Splits the selected edges.

### Loop Cut and Slide
**Shortcut:** Ctrl+R

Cuts a loop of faces. The wheel - the number of cuts, a click - cut, then the mouse slides it, a click - apply.

### Offset Edge Slide
**Shortcut:** Ctrl+Shift+R

Adds edges on both sides of the selected ones and moves them away from the middle.

### Connect

Connects the middles of the selected edges.

### Insert Loop

Inserts a loop through the selected edge.

### Arch Edges

Bends the selected edges into an arc. Two opposite ceiling edges of a passage make a vault.

### Rotate Edge

Turns the edge between two triangles.

### Bevel Weight / Crease
**Shortcut:** Shift+E (crease)

An edge with a crease stays sharp when subdivided.

### Mark Seam / Clear Seam

A seam stops UV unwrapping and texture continuation.

### Mark Sharp / Clear Sharp / Sharpness by Angle

At a sharp edge the normals of smooth faces split.

### Split

Tears the mesh apart along the selected edges.

### Dissolve Edges
**Shortcut:** Ctrl+X

Removes the edges, the faces on both sides merge.

### Delete Edges / Delete Edge Loops

## Faces

Mesh -> Face menu.

### Extrude Faces
**Shortcut:** E

Extrudes the faces together. Alt+E - a menu of the other kinds.

### Extrude Along Normals

Each vertex goes along its own normal.

### Extrude Individual

Each face is extruded on its own.

### Extrude Repeat

Extrudes the selection several times in a row along its normal. In the Last Operation panel: repeats, distance and
scale per step.

### Inset
**Shortcut:** I

Makes smaller faces inside the faces.

### Spin

Rotates copies of the selection in steps around an axis. The center is the 3D cursor, the axis the view direction.

### Poke

Splits a face into triangles meeting at its center.

### Bridge Faces

Connects two faces with a tunnel; the faces themselves are removed.

### To Circle

Puts the outline of the selected faces on a circle.

### Subdivide

Splits the faces.

### Triangulate

Splits the faces into triangles.

### Tris to Quads

Merges pairs of triangles.

### Merge

Merges the selected faces into one.

### Fill Hole

Closes a hole with a face.

### Shade Smooth / Shade Flat

### Flip Normals

Turns the faces around.

### Recalculate Outside / Inside
**Shortcuts:** Shift+N / Ctrl+Shift+N

Turns all faces outward or inward. Alt+N - the normals menu.

### Detach

Tears the selected faces off their neighbors; they stay in the same mesh.

### Duplicate
**Shortcut:** Shift+D

Copies the selection and moves the copy.

### Dissolve Faces / Limited Dissolve

Merges faces. Limited - only those meeting at a small angle.

### Delete Faces

### Face Drag
**Shortcut:** Shift+drag

Moves a face along its normal, the neighboring faces stretch. Ctrl+Shift+drag outward extrudes a new block from the
face, inward it cuts the object: the part dragged over becomes an object of its own.

### Face Cutter

Cuts a face with lines from one of its edges.

## Tools

The tool column on the left of the viewport.

### Box Select

### Ruler
**Shortcut:** Shift+M

A drag makes a new ruler. Dragging the middle makes an angle. X or Delete over a ruler deletes it.

### Knife
**Shortcut:** K

Clicks place points, Enter cuts. RMB ends the current cut. A - 30° angle steps, Ctrl - the middle of an edge.

### Knife Project

Cuts the active mesh along the outlines of the other edited meshes, as they are seen in the view. Select the cutter
and the mesh to cut, Tab, click the mesh to cut, then Knife Project. The outline is the cutter's open borders and
loose edges; for a closed mesh, its silhouette.

### Loop Cut
**Shortcut:** Ctrl+R

### Box Cutter

Draws a rectangle, a circle or a polygon on a face and cuts it out, cuts along it or extrudes it.

### Bisect
**Shortcut:** Alt+X

Cuts the mesh with a plane. Clicks place 2 or 3 points, Ctrl+Enter - which side to keep, Enter - apply.

### Spin

A ring around the 3D cursor: dragging it rotates the selection around the axis. X / Y / Z / V - the axis.

## Level Tools

### Box Cut / Cylinder Cut / Prism Cut
**Shortcut:** C (box)

Drawn like a shape; cuts its volume out of the meshes. Works on the edited mesh, on the selected objects or on the
whole scene.

### Corridor / Alcove
**Shortcut:** W (right after a cut)

After a cut with two openings - a corridor between them. With one opening - an alcove.

### Complete
**Shortcut:** W

If the cut went through a corner, the cut follows the faces of its volume.

### Non-destructive Cut

A checkbox in the cut panel. Instead of a hole, a cutter node is placed that keeps cutting the meshes. It can be moved
and deleted.

### Add Cutter / Apply Cutter

Places a cutter as an object. "Apply" makes its cut permanent.

### Keep Overlap Faces

A checkbox in the Create tab. When a shape is added to a mesh, the faces lying on the mesh's faces stay.

## Objects

Object menu.

### Freeze Transform

Moves the position, rotation and scale into the vertices.

### Center Pivot / Origin to 3D Cursor / Pivot to Selection

Moves the object's origin; the geometry stays in place.

### Subdivide (Catmull-Clark)

Smooths the mesh by subdivision.

### Remesh

The Object tab of the Hewn panel. Makes the selected meshes again, their shape kept. The mesh has to be closed.
- Voxel: faces as big as the voxel. Adaptivity: fewer faces on flat parts; Fix Poles evens the vertices out along the
  surface. The volume and the attributes (vertex colors, smoothing) can be kept.
- Quad: quads of the density asked for: by a face count, by a ratio of the faces there are now, or by an edge length
  (about how many faces is shown). Variation moves the grid. The grid goes along the axes, not along the curves.

### Trims

The Object tab. "Belt at Height" puts a sweep (a cornice) around the walls of the selected objects at a height
measured from their bottom. Floors and Floor Height add more belts above it. The sweep's Align: Outside keeps the
profile against the wall whatever its width.

### Collision

The Collision group in the inspector of a Hewn mesh. Types:
- Trimesh, Convex: from the faces.
- Simplified Convex, Simplified Trimesh: fewer faces (Ratio).
- Multiple Convex: the shape split into convex pieces (Max Pieces).
- Box, Capsule, Cylinder, Sphere: around the mesh. Capsule and cylinder have an Axis.
- Primitive: the exact shape of a cube, cylinder, sphere or capsule made by Hewn.

Metadata and Groups are put on the collision body when baking (for example `surface = wood` for footstep sounds). The
mesh's own metadata and groups go to the baked MeshInstance3D.

### Add Subdivision Modifier

### Auto Smooth

Smooth shading with sharp edges by angle.

### Quadify

Merges the triangles of the whole mesh into quads.

### Conform Normals / Triangulate All / Flip All Normals

### Apply Modifiers

Moves the result of the modifiers into the mesh itself.

### Merge Selected Objects

### Boolean Union / Subtract / Intersect

### MeshInstance3D -> Hewn

Turns a plain mesh into a Hewn mesh.

### Bake to MeshInstance3D

Makes a plain MeshInstance3D from a Hewn mesh.

### Export OBJ / Export glTF

Saves the selected mesh to a file.

### Object Menu
**Shortcut:** RMB

In object mode: origin, cursor to selected, shading, normals, duplicate, delete, clear and apply transforms.

### Delete
**Shortcut:** X

In object mode deletes the selected nodes.

### Isolate / Frame
**Shortcuts:** / , .

## Modifiers

Modifiers are added in the Inspector, in the Modifiers array (Add Element). They run from top to bottom; drag them to
change the order.

### HewnMirrorModifier

Mirrors the mesh along axes or along the planes of another node.

### HewnSolidifyModifier

Gives the mesh a thickness.

### HewnSubdivisionModifier

Smooths the mesh by subdivision.

### HewnBooleanModifier

Unites, subtracts or intersects the mesh with other objects. The objects stay operands and can be moved.

### HewnArrayModifier

Copies of the mesh along a line, around a circle or along a Path3D.

### HewnRemeshModifier

Makes the mesh again on a voxel grid: Blocks (cubes), Smooth, Sharp (the corners kept, how much: Sharpness) or Voxel
(by the voxel size). For the first three Octree Depth and Scale set the cell. Remove Disconnected takes small separate
pieces away, Smooth Shading turns smooth shading on.

### HewnSimplifyModifier

Simplifies the mesh. Collapse keeps a part of the triangles (Ratio), Planar joins neighboring faces that meet at less
than an angle (Angle Limit).

## Textures

### Textures Dock

The images of the project. A click on an image puts it on the selected faces, in object mode on the selected objects.
An image can be dragged onto a face or an object.

The material for an image is made once in the materials folder and reused after that.

### Texture for New Objects

A click on an image makes it the current one. New shapes are made with it.

### Texture Transfer
**Shortcuts:** Alt+click, Ctrl+Alt+click, Alt+RMB

With one face selected, Alt+click carries its texture onto other faces; the pattern continues across the edges.
Ctrl+Alt+click carries only the UVs. Alt+RMB takes the texture from the clicked face.

### Texture to Edge
**Shortcut:** T

Aligns the texture to the edge under the mouse. W / A / S / D - which side, Q - fit the height, E - fit the width, R -
scale 1. A click, Enter or T - apply, Esc - cancel.

### UV Transform
**Shortcut:** Shift+T

Shows the texture tile on the selected face with handles: corners and sides - scale, middle - move, circle - rotate.
G / R / S - the same with the mouse, without handles. A click away from the handles or Enter - apply, Esc - cancel.

### Texture Tools
**Shortcut:** U

The same tools as in the UV tab, as a menu under the mouse. In "Move / Rotate / Scale Texture" the texture follows the
mouse: X / Y - an axis, Ctrl - in steps.

- **Cycle** - a quarter turn.
- **Mirror U / V**
- **Fit U / V** - one tile across the width or the height.
- **Center**, **Fit to Face**, **Randomize Offset**
- **Box Projection**
- **Extend from Active** - the texture of the last selected face goes onto the others.
- **Square Fit Unwrap** - a strip of quads is laid out in one tile.
- **Copy / Paste UV Rect**

### World UV

The texture follows where the face is in the world: neighboring objects meet without seams. The UV tab turns it on for
new objects; "Apply World UV" converts the selection. The UV islands stay near the first tile even when the object is
far from the center of the scene.

The texture is projected along the nearest world axis, so it is stretched on slopes. "Align to Face" projects it onto
the plane of the selected faces, true to size (a seam may show with the faces around); "Align to Axis" puts it back.

### UV Lock

Faces moved in edit mode take their texture with them.

### Material Tab

The materials of the mesh and the texture settings of the selected faces: filtering, transparency, vertex colors,
roughness, metallic.

### Tool Materials

The "Clip" material is not drawn in the game but collides. It makes invisible walls.

### Lightmap UV2

Object tab -> Game. Makes a second unwrap for baking light.

## UV Editor

The "Hewn UV" dock. Shows the UVs of the selected faces over the texture.

- 1 / 2 / 3 - UV vertices, edges, faces.
- Click, Shift+click, box - selection. L - the island under the mouse, Ctrl+L - the islands of the selection.
- G / R / S or a drag - move, rotate, scale.
- Ctrl+P - pack the islands, Ctrl+A - average their scale.
- The wheel zooms the view, the middle button pans it.

## Sculpt

The mode is picked in the menu at the start of the header. Good for a small terrain and soft shapes. LMB - a stroke,
the whole stroke is undone by one Ctrl+Z. Ctrl - the brush the other way, Shift - smoothing in any brush. Ctrl+wheel or
[ / ] - the radius, Shift+wheel - the strength. When something is selected in edit mode, only that is sculpted.

The tools in the column on the left:
- Raise / Lower.
- Smooth.
- Flatten - to the plane under the brush, with Ctrl to the height where the stroke began.
- Grab - the area taken follows the mouse.
- Noise.
- Terrace - the heights rounded to the step ("Step" on the header).
- Pinch, Inflate.
- Sharp Edge - a groove, with Ctrl a ridge.
- Density - only changes the detail (shown when the dynamic topology is on).

On the header: the radius in pixels, the strength, the direction ("Up" - straight up, slopes never fold over; "Along
the Surface" - out of the surface under the brush, the default), the falloff (as in Vertex Paint, plus Custom Curve),
the pen pressure (on the strength and on the radius) and the cursor (on the screen or on the surface, its color).

### Dynamic Topology

A check box on the header. Under the brush the surface is split into triangles of the size wanted: long edges split,
short ones collapsed. Refine method: subdivide, collapse or both. Detailing: relative (pixels on the screen), constant
(meters) or brush detail (percent of the radius).

## Vertex Paint

The mode is picked in the menu at the start of the header. Vertices, edges or faces are painted (1 / 2 / 3), and when
something is selected, only the selection.

### Brush

LMB paints. Ctrl+wheel - radius, Shift+wheel - strength, Ctrl+click - pick a color.

### Pencil

Paints one vertex.

### Fill

Paints the selection or a connected part.

### Eyedropper

Picks a color. With Shift - as the second color.

### Normals

LMB makes the normals smooth, Ctrl+LMB flat.

### Colors
**Shortcuts:** X, D

X swaps the two colors, D resets them to black and white.

### Bake Light

The Colors menu or Object -> "Bake Light to Vertex Colors". Writes the light and shadows of the scene's lights into
the vertex colors.

### Blend Material

The Material tab. Blends up to four textures by the vertex colors. Check boxes in the window give each texture
metallic and roughness, a normal map, emission. Their values are set in the Inspector.

## Scenes and Objects

### Prefabs Dock

A click on a scene makes it follow the mouse, releasing places it. Q / E - turn by 90°, N - along the surface normal,
R - another random turn. "Placement..." - random rotation and size.

### Place Another
**Shortcut:** W

Places another copy of the same scene.

### Entities

The Create tab. Lights, a spot light, sound and your own objects from `res://hewn_entities.json`.

### Decals and Overlays

An image on a surface. A decal is a Godot Decal node. An overlay lies on Hewn faces and is saved as geometry. - / =
during placement change the size.

### Reference Image

Shift+A -> Image. An image to trace over, not shown in the game.

### Scatter

Shift+A -> Scatter. Copies of meshes painted on surfaces: grass, stones, bushes. In the inspector: the meshes and their
weights, density, brush radius, random rotation, tilt and scale, Align (up or along the surface's normal), offset.
While the node is selected, its brushes are in the tool column:
- Brush: adds copies. Ctrl - erase, Shift - thin.
- Erase, Thin.
- Ctrl+wheel or [ / ] - the radius.

The copies are kept in chunks (Chunk Size), each drawn by its own MultiMeshInstance3D with a Visibility Range. A
stroke rebuilds only the chunks it touched. Baking makes plain MultiMeshInstance3D nodes.

### Curve Points

Select a Path3D and press Tab. E - a new point after the end, Ctrl+click - a point under the mouse, X - delete, V -
smooth or straight handles.

## Game and Export

Object tab -> Game.

### Bake Level

Saves a copy of the scene for the game: plain MeshInstance3D nodes, collision, occluders. The scene itself stays
editable.

### Export Scene glTF

Saves the whole scene as one file.

### Check Scene

A list of the scene's problems: faces without area, unwelded vertices, non-planar faces and others. Most can be fixed
from there.

### Scene Size

Shows what takes up space in the saved scene.

## Quad Screen

A tab next to 2D / 3D. The top left is a 3D view, the others are top, front and side.

### Select
**Shortcut:** Shift+S

A click or a box selects, a drag moves along the grid, Shift+drag makes a copy. The handles change the size.

### Block
**Shortcut:** Shift+B

Drag a box in a 2D view; Enter makes the shape.

### Vertex
**Shortcut:** Shift+V

Drags the vertices of the selected meshes.

### Clip
**Shortcut:** Shift+X

Draw a line; Enter cuts the selected meshes.

### Maximize View
**Shortcut:** Shift+Z

## Settings

Editor -> Editor Settings -> Hewn.

- **Interface -> Hotkeys** - Hewn's keys on / off.
- **Interface -> Last Operation Panel** - the last operation panel.
- **Interface -> Quad View** - the Quad screen.
- **Interface -> Wrap Cursor At Viewport Edges** - while dragging, the cursor comes out on the other side of the
  viewport.
- **Warnings -> Subdivide Face Limit** - how many faces a subdivision may add without asking.

The keys are changed in Editor -> Editor Settings -> Shortcuts, in the "hewn" section.

## Last Operation Panel

Opens at the bottom left after an operation. Its parameters can be changed, and the operation is redone. The fields can
be dragged with the mouse: Shift - finer, Ctrl - in steps. The wheel over a field changes the value by a step (Shift:
a small one); the view does not zoom.
