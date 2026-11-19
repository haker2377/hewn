# Hewn Manual

Hewn is a plugin for Godot 4.7 that lets you create and edit meshes and game levels right in the Godot editor.

## Installation

1. Download the [latest release](https://github.com/haker2377/hewn/releases) or [build the plugin from source](#building).
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

## Creating Objects

### Add Menu
**Key:** Shift+A

A menu of everything that can be added to the scene: meshes, curves, lights, a camera, sounds, pictures, decals,
cutters, physics (Area3D, a static body, a single collision shape: drawn as a box). The "+" button on the Hewn toolbar
opens the same menu. "Favorites": the scenes added to the favorites (RMB on a scene in the Scenes dock, or Godot's
FileSystem favorites); the item is there only when there are some.

With "Create at 3D Cursor" on in the menu, the object appears at the 3D cursor with its default size. With it off, the
shape is drawn with the mouse.

![](images/add_menu.png)

### Drawing a Shape

The first click places the first corner of the base, the second click the opposite corner, the third the height. You
can also drag. For round shapes the first click places the center.

- Shift on the first click - a base at an angle: the first side gives the direction.
- Ctrl before the first click - draw on the extension of the plane of the face under the mouse.
- Alt - draw on the plane of the previous shape.
- Q / E or R - turn a shape that has a direction (stairs, door, prism, wedge).
- The height of some shapes can be typed in.
- Enter instead of drawing - a shape of the default size at the 3D cursor.

After it is made, the shape's parameter panel opens at the bottom left. It opens again when the shape is selected, as
long as its geometry has not been changed in edit mode.

A shape drawn in edit mode is added to the mesh being edited.

### Shapes

- **Cube**
- **Plane**
- **Cylinder**
- **Cone**
- **Sphere**
- **Icosphere**
- **Torus**
- **Pipe**
- **Prism**
- **Stairs**
- **Arch**
- **Door**
- **Capsule**
- **Disc**
- **Wedge**
- **Column**

![](images/shapes.png)

#### Box
<img src="icons/shape_cube.svg" width="16" alt="">

**Key:** B

![](images/shape_box.png)

With "Room" on the box is hollow. Its floor and ceiling: none, solid (as thick as the walls) or flat (one face, facing in).

#### Plane
<img src="icons/shape_plane.svg" width="16" alt="">

A plane through two points.

![](images/shape_plane.png)

#### Cylinder
<img src="icons/shape_cylinder.svg" width="16" alt="">

![](images/shape_cylinder.png)

#### Cone
<img src="icons/shape_cone.svg" width="16" alt="">

![](images/shape_cone.png)

#### Sphere
<img src="icons/shape_sphere.svg" width="16" alt="">

![](images/shape_sphere.png)

#### Icosphere
<img src="icons/shape_icosphere.svg" width="16" alt="">

![](images/shape_icosphere.png)

#### Torus
<img src="icons/shape_torus.svg" width="16" alt="">

![](images/shape_torus.png)

#### Pipe
<img src="icons/shape_pipe.svg" width="16" alt="">

![](images/shape_pipe.png)

#### Prism
<img src="icons/shape_prism.svg" width="16" alt="">

![](images/shape_prism.png)

#### Stairs
<img src="icons/shape_stairs.svg" width="16" alt="">

![](images/shape_stairs.png)

"Step Shape": the tread's overhang and thickness. "Border Position": inside, outside or on the edge. "Ramp": a slope
beside the steps.

#### Arch
<img src="icons/shape_arch.svg" width="16" alt="">

![](images/shape_arch.png)

#### Capsule
<img src="icons/shape_capsule.svg" width="16" alt="">

![](images/shape_capsule.png)

#### Disc
<img src="icons/shape_disc.svg" width="16" alt="">

![](images/shape_disc.png)

#### Wedge
<img src="icons/shape_wedge.svg" width="16" alt="">

![](images/shape_wedge.png)

#### Wall
<img src="icons/shape_wall.svg" width="16" alt="">

A wall along a line. Clicks place the corners; Enter, RMB or a double click - done; Backspace removes the last corner.

![](images/shape_wall.png)

#### Column
<img src="icons/shape_column.svg" width="16" alt="">

![](images/shape_column.png)

#### Sweep
<img src="icons/shape_sweep.svg" width="16" alt="">

A profile along a line: railings, curbs, pipes, cornices. Drawn like a wall.

The "In View" button of the parameter panel shows the profile's points on the shape in the 3D view: drag them, a click
on the curve adds a point, Delete or X removes the selected one. The same for a column, a door, an arch, a wedge and a
roof.

![](images/shape_sweep.png)

#### Path Stairs

Stairs along a line: flights on the segments, landings at the turns. Drawn like a wall.

The mouse wheel while drawing sets the steps of each flight.

"Corner Radius" rounds the turns: the steps fan round, no landings. In "Edit Path Points" Ctrl+drag raises or lowers a
point. "UV Along" lays the texture the way the stairs go.

![](images/shape_path_stairs.png)

#### Poly Shape
<img src="icons/shape_poly_shape.svg" width="16" alt="">

An outline of points with a height. Clicks place the points, a click on the first point or Enter closes the outline,
then the mouse sets the height.

After it is made, its points can be changed with "Edit Base Points" in the parameter panel: drag a point to move it,
click a point to delete it, click an edge to add one.

![](images/shape_poly_shape.png)

#### Rock
<img src="icons/shape_rock.svg" width="16" alt="">

A rock: a ball pushed in and out by a noise (roughness, bump size, layers), cut flat here and there (facets), flat at the
bottom, tapering and leaning. Another seed, another rock.

![](images/shape_rock.png)

#### Text
<img src="icons/shape_text.svg" width="16" alt="">

Solid letters of a font. Placed with a click; the text, the font, the size and the depth are in the parameter panel.

![](images/shape_text.png)

#### Roof
<img src="icons/shape_roof.svg" width="16" alt="">

A roof over an outline: hip, gable, shed or flat. Drawn with three clicks like a box: two corners and the ridge's
height. The pitch, the
overhang, the thickness; for a gable roof which sides are gables and the gable walls. The "Curve" button sets the slopes'
shape from the eaves to the ridge (a break, a mansard). One roof over an L or a T outline joins its slopes itself.
- "Height By": the pitch or the ridge's height (the pitch made to fit it).
- "Flat Top": flat above this height.
- "UV Along Slope": the texture laid along each slope.
- "Gable Eave": a little roof along the foot of each gable.
- The gables' outer faces can get their own material.
- "Edit Base Points" puts the roof on a curve: Ctrl+click adds a point, V sets a point's type.

![](images/shape_roof.png)

## Modes

The modes are switched on the Hewn toolbar.

![](images/modes.png)

### Edit Mode
**Key:** Tab

Turns editing of the vertices, edges and faces of the selected meshes on and off.

### Vertices, Edges, Faces
<img src="icons/vertex.svg" width="16" alt=""> <img src="icons/edge.svg" width="16" alt=""> <img src="icons/face.svg" width="16" alt=""><br>
**Keys:** 1 / 2 / 3

What is selected in edit mode. Shift+1 / 2 / 3 or Shift+click on a mode button turns on several at once.

## Selection

### Select
**Keys:** LMB

Shift+click adds to the selection or takes away from it.

### Loop
**Keys:** Alt+click, double click

Selects a loop of edges or faces.

![](images/select_loop.png)

### Ring
**Key:** Ctrl+Alt+click

Selects a ring of edges or faces.

![](images/select_ring.png)

### Shortest Path
**Keys:** Ctrl+click, Ctrl+Shift+click

Ctrl+click selects the elements between the selected one and the one clicked.

![](images/select_shortest_path.png)

Ctrl+Shift+click selects all the shortest paths between them: on a grid, the whole rectangle.

![](images/select_shortest_path_fill.png)

### All / None / Invert
**Keys:** A / Alt+A / Ctrl+I

![](images/select_invert.png)

### Grow / Shrink
**Keys:** Ctrl+= / Ctrl+-

Adds the neighboring elements to the selection or takes away the outer ones.

![](images/select_grow.png)

### Linked
**Keys:** L / Ctrl+L

L selects the connected part under the mouse, Ctrl+L the part connected to the selection.

![](images/select_linked.png)

### Boundary

Selects the open edges of the mesh: edges with only one face.

![](images/select_border.png)

### Checker Deselect

Deselects every other element.

![](images/select_checker_deselect.png)

### Same Material

Selects the faces with the same material.

![](images/select_same_material.png)

### Same Normal

Selects the faces facing the same way. The largest angle and "Connected only" are in the Last Operation panel.

![](images/select_same_normal.png)

### Bad UVs

Selects the faces whose UVs have no area.

### Overlapping Faces

Selects faces lying on top of each other.

### Hide / Reveal
**Keys:** H / Shift+H / Alt+H

Hides the selected faces, hides the other faces, or shows everything again. Hidden faces are hidden only in edit mode.

![](images/hide.png)

### Visible Selection

The button with the eye on the toolbar. When it is on, clicks go through faces turned away from the camera, so a room
can be edited from outside.

On:

![](images/visible_selection_on.png)

Off:

![](images/visible_selection_off.png)

### X-Ray
**Key:** Alt+Z

The selection goes through the mesh.

![](images/x_ray.png)

## Move, Rotate, Scale

### Move / Rotate / Scale
**Keys:** G / R / S

Works on the selected elements in edit mode and on the selected objects in object mode.

- X / Y / Z - an axis, Shift+X / Y / Z - a plane, pressed again - the local axis.
- The amount can be typed in.
- Ctrl - inverts snapping.
- Shift - slows the change down for precise moves.
- LMB or Enter - apply, RMB or Esc - cancel.

### Slide
**Keys:** G G

The selected elements slide along the edges. C - keeps the elements on the line of the current edge.

![](images/edge_slide.png)

### Base Point
**Key:** B (during G / R / S and G G)

A click - the point from, a second click - the point to. Moves, rotates or scales by these points.

### Nudge with Arrows
**Keys:** arrows, Page Up / Page Down

Moves the selection by one grid step along the world axis closest to the view. With Ctrl in object mode a copy is
moved.

### Rotate with Arrows
**Keys:** Alt+arrows, Alt+Page Up / Page Down

Turns the selection by 90°.

### Gizmo
**Key:** W

Switches the gizmo: move, rotate, scale, none.

### Orientation
<img src="icons/global.svg" width="16" alt=""> <img src="icons/local.svg" width="16" alt=""> <img src="icons/normal.svg" width="16" alt=""> <img src="icons/gimbal.svg" width="16" alt=""> <img src="icons/view.svg" width="16" alt=""> <img src="icons/cursor.svg" width="16" alt=""> <img src="icons/parent.svg" width="16" alt="">

A menu on the toolbar: global, local, normal, gimbal, view, cursor, parent. "Create from Selection" adds an
orientation of your own from the selection.

![](images/orientation.png)

### Transform Pivot
<img src="icons/bbox.svg" width="16" alt=""> <img src="icons/cursor.svg" width="16" alt=""> <img src="icons/individual.svg" width="16" alt=""> <img src="icons/median.svg" width="16" alt=""> <img src="icons/active.svg" width="16" alt="">

A menu on the toolbar: bounding box center, 3D cursor, individual origins, median point, active element.

![](images/pivot.png)

### Proportional Editing
<img src="icons/proportional.svg" width="16" alt=""><br>
**Keys:** O / Alt+O

The neighboring elements move with the selection, less the farther they are. Alt+O - only the connected ones.

![](images/proportional_editing.png)

### Symmetry
<img src="icons/symmetry.svg" width="16" alt="">

A button on the toolbar, its axes in the menu next to it (X by default). When vertices move, their mirror twins on the
other side of the object's axis move mirrored.
- Works for move, rotate, scale, slide, and with proportional editing.
- Vertices on the plane of symmetry stay on it.
- New faces (extrude, duplicate) are not mirrored; HewnMirrorModifier does that.
- The operation panel turns symmetry on or off for each axis.

![](images/symmetry.png)

### Auto Merge
<img src="icons/automerge.svg" width="16" alt="">

A button on the toolbar. Vertices that end up next to each other after a move are merged.

## Snapping

### Snapping
**Keys:** Y, Shift+Tab

Turned on with Godot's magnet. The snapping menu on the toolbar chooses what to snap to: step, grid, vertex, edge, face,
edge center, face center.

### Grid Step
**Keys:** `[`, `]`

Makes the grid step half or twice as big. The current step is shown next to the snapping menu.

### 3D Cursor
<img src="icons/cursor.svg" width="16" alt=""><br>
**Keys:** Shift+RMB, Shift+S

Shift+RMB puts the 3D cursor on a surface. Shift+S - the cursor menu: to the selection, to the world origin, the
selection to the cursor.

## View

### Shading
<img src="icons/view_material.svg" width="16" alt=""> <img src="icons/view_channel.svg" width="16" alt=""> <img src="icons/view_normals.svg" width="16" alt=""> <img src="icons/view_normal_colors.svg" width="16" alt=""> <img src="icons/view_single.svg" width="16" alt=""> <img src="icons/view_random.svg" width="16" alt=""> <img src="icons/view_collision.svg" width="16" alt="">

The shader used in the 3D viewport.

![](images/shading.png)

### Overlays
<img src="icons/overlays.svg" width="16" alt="">

![](images/overlays.png)

## Vertices

Mesh -> Vertex menu.

![](images/vertex_menu.png)

### Extrude Vertices
**Key:** E

Extrudes the vertices into new edges.

![](images/extrude_vertices.png)

### Bevel Vertices
**Key:** Ctrl+Shift+B

Cuts the corners at the selected vertices.

![](images/bevel_vertices.png)

### Push / Pull

Moves the vertices toward the center of the selection or away from it.

![](images/push_pull.png)

### Shrink / Fatten
**Key:** Alt+S

Moves the vertices along their normals.

![](images/shrink_fatten.png)

### Shear

Shears the selection.

![](images/shear.png)

### Slide Vertices
**Key:** Shift+V

Moves a vertex along one of its edges.

![](images/slide_vertex.png)

### Randomize

Moves the vertices by a random distance.

![](images/randomize.png)

### Mirror

Mirrors the selection along an axis.

### To Circle

Puts the selected vertices on a circle.

![](images/to_circle.png)

### Connect
**Key:** J

Connects the selected vertices with edges across the faces.

![](images/connect_vertices.png)

### Rip
**Key:** V

Rips the selection off on the side of the mouse and moves it.

![](images/rip.png)

### Merge
**Key:** M

Merges the selected vertices: at the center, at the cursor, at the first, at the last, by distance.

![](images/merge.png)

### Drop / Raise to the Surface

Each selected vertex goes straight down (up) along Y to the nearest face under (over) it.

![](images/drop_to_surface.png)

### Merge by Distance

Merges vertices closer than the given distance.

### Vertex Crease

The vertex stays sharp when subdivided.

### Separate
**Key:** P

Moves the selection into a new mesh. It can also separate by material or by loose parts.

### Dissolve Vertices
**Key:** Ctrl+X

Removes the vertices; the faces around merge.

![](images/dissolve_vertices.png)

### Delete Vertices
**Keys:** X, Delete

Deletes the vertices and their faces.

![](images/delete_vertices.png)

## Edges

Mesh -> Edge menu.

![](images/edge_menu.png)

### Extrude Edges
**Key:** E

Extrudes the edges into new faces.

![](images/extrude_edges.png)

### Bevel Edges
**Key:** Ctrl+B

Cuts the edges off. The wheel or +/- changes the number of segments.

![](images/bevel_edges.png)

### Bridge Edge Loops

Joins two loops of edges with faces. In the operation panel: segments, smoothness, twist. With several segments handles
show at its ends: drag them. "Profile Factor" - how thick the bridge is in its middle, "Profile Shape" - how it changes.

![](images/bridge_edge_loops.png)

### Sweep Along Edges

A sweep (a cornice) along the selected edges, outside the faces next to them. In face mode: along the border of the
selected faces.

![](images/sweep_along_edges.png)

### Wall Along Edges

A wall on the selected edges: a parapet, a fence. Its height, thickness and side are set later in the shape panel.

![](images/wall_along_edges.png)

### New Face from Edges
**Key:** F

Makes a face from the selected edges or vertices. Two vertices - an edge between them.

![](images/new_face_from_edges.png)

### Subdivide

Divides the selected edges. Two opposite edges of a face: their new points are joined by an edge. With "Create N-gons"
off, the faces around become quads and triangles.

![](images/subdivide_edges.png)

### Loop Cut and Slide
<img src="icons/tool_loopcut.svg" width="16" alt=""><br>
**Key:** Ctrl+R

Cuts a loop of faces. The wheel - the number of cuts, a click - cut, then slide with the mouse, a click - apply.

![](images/loop_cut.png)

### Offset Edge Slide
**Key:** Ctrl+Shift+R

Adds edges on both sides of the selected ones and moves them away from the middle.

![](images/offset_edge_slide.png)

### Connect

Joins the middles of the selected edges.

![](images/connect_edges.png)

### Insert Loop

Inserts a loop through the selected edge.

![](images/insert_loop.png)

### Arch Edges

Bends the selected edges into an arc. Two opposite edges of a passage's ceiling make a vault. "Shape: Curve" takes the
arc from a curve; its points can be dragged in the 3D view.

![](images/arch_edges.png)

### Rotate Edge

Turns the edge between two faces.

![](images/rotate_edge.png)

### Bevel Weight / Crease
**Key:** Shift+E (crease)

A creased edge stays sharp when subdivided.

### Mark Seam / Clear Seam

A seam stops a UV unwrap and a continued texture.

### Mark Sharp / Clear Sharp / Sharpness by Angle

At a sharp edge the normals of smooth faces are split.

### Split

Splits the mesh along the selected edges.

### Dissolve Edges
**Key:** Ctrl+X

Removes the edges; the faces on both sides merge.

![](images/dissolve_edges.png)

### Delete Edges / Delete Edge Loops

![](images/delete_edges.png)

## Faces

Mesh -> Face menu.

![](images/face_menu.png)

### Extrude Faces
**Key:** E

Extrudes the faces together. Alt+E - a menu of the other kinds.

![](images/extrude_faces.png)

### Extrude Along Normals

Each vertex goes along its own normal.

![](images/extrude_along_normals.png)

### Extrude Individual Faces

Each face is extruded on its own.

![](images/extrude_individual.png)

### Extrude Repeat

Extrudes the selection several times along its normal. In the Last Operation panel: the repeats, the distance and the
scale per step.

![](images/extrude_repeat.png)

### Inset
**Key:** I

Makes smaller faces inside the faces.

![](images/inset.png)

### Spin
<img src="icons/tool_spin.svg" width="16" alt="">

Turns copies of the selection around the 3D cursor in steps.

![](images/spin.png)

### Poke

Splits a face into triangles with a vertex at the center.

![](images/poke.png)

### Bridge Faces

Joins two faces with a tunnel; the faces themselves are removed. Segments, smoothness, twist and handles as for Bridge
Edge Loops.

![](images/bridge_faces.png)

### To Circle

Puts the border of the selected faces on a circle.

### Subdivide

Divides the faces.

![](images/subdivide_faces.png)

### Triangulate

Splits the faces into triangles.

![](images/triangulate.png)

### Triangles to Quads

Merges pairs of triangles.

![](images/tris_to_quads.png)

### Combine Faces

Merges the selected faces into one, the edges between them go.

![](images/combine_faces.png)

### Thicken

The selected faces become a solid of their own: a back side and a rim. The thickness and the side are in the Last
Operation panel.

![](images/thicken.png)

### Fill Hole

Closes a hole with a face.

![](images/fill_hole.png)

### Shade Smooth / Shade Flat

![](images/shade_smooth.png)

### Flip Normals

Turns the faces around.

![](images/flip_normals.png)

### Double Sided

Makes the selected faces visible from both sides: a leaf, a fence, a thin wall made of one sheet. The back gets its own
turned normals, so it is lit right and no special material is needed. The same command again makes them
one-sided. The face inspector has the same switch.

![](images/double_sided.png)

### Recalculate Outside / Inside
**Keys:** Shift+N / Ctrl+Shift+N

Turns all the faces outward or inward. Alt+N - the normals menu.

![](images/recalculate_outside.png)

### Detach

Tears the selected faces off their neighbors; they stay in the same mesh.

![](images/detach.png)

### Duplicate
**Key:** Shift+D

Copies the selection and moves the copy.

![](images/duplicate.png)

### Dissolve Faces / Limited Dissolve

Merges faces. Limited Dissolve - only those meeting at a small angle.

![](images/dissolve_faces.png)

### Delete Faces

![](images/delete_faces.png)

### Face Drag
**Key:** Shift+drag

In object mode, on a selected object: moves a face along its normal; the faces around stretch; the face under the mouse is outlined in yellow. Ctrl+Shift+drag outward extrudes a new block from the face;
inward it cuts the object: the part dragged through becomes an object of its own.

### Face Cutter
<img src="icons/tool_face_cutter.svg" width="16" alt="">

Cuts a face with lines from one of its edges.

## Tools

The tool column on the left of the viewport.

![](images/tool_column.png)

### Box Select
<img src="icons/tool_select.svg" width="16" alt=""> <img src="icons/tool_select_circle.svg" width="16" alt=""> <img src="icons/tool_lasso.svg" width="16" alt="">

![](images/tool_box_select.png)

### Ruler
<img src="icons/tool_ruler.svg" width="16" alt=""><br>
**Key:** Shift+M

A drag - a new ruler. Drag its middle - an angle. X or Delete over a ruler - delete it.

![](images/ruler.png)

### Knife
<img src="icons/tool_knife.svg" width="16" alt=""><br>
**Key:** K

![](images/tool_knife.png)

Clicks place points, Enter cuts. RMB ends the current cut. A - 30° angle steps, Ctrl - the middle of an edge. A point
snaps to vertices, edges and the knife lines already drawn. All the cuts are made at once: where they cross a vertex is
placed, each face is split into pieces without overlaps.

### Loop Cut
<img src="icons/tool_loopcut.svg" width="16" alt=""><br>
**Key:** Ctrl+R

![](images/tool_loop_cut.png)

### Box Cut
<img src="icons/tool_cutter.svg" width="16" alt="">

![](images/tool_box_cut.png)

Draws a rectangle, a circle or a polygon on a face and cuts it out, cuts along it, or extrudes it.

### Bisect
<img src="icons/tool_bisect.svg" width="16" alt="">

![](images/tool_bisect.png)

In edit mode: draw a line across the mesh; the plane through it and the view cuts the selection. In the panel: "Cut
Only", "Cut and Fill" (two closed parts), "Remove Above", "Remove Below" ("Fill Hole"). The arrow on the plane moves it,
two rings turn it (Ctrl - snapping). "Cut and Fill": both parts are closed, the lower one is an object of its own
("Separate the Part Cut Off").

### Clip
<img src="icons/tool_clip.svg" width="16" alt=""><br>
**Key:** Alt+X

![](images/tool_clip.png)

A tool of its own (scissors in the tool column), in object and edit mode: clicks place 2 or 3 points, the plane
through them cuts, Ctrl+Enter - what to keep, Enter - apply.

### Spin
<img src="icons/tool_spin.svg" width="16" alt="">

A ring around the 3D cursor: drag it and the selection turns around the axis. X / Y / Z / V - the axis.

## Level Tools

### Box / Cylinder / Prism Cutter
**Key:** C (box)

Drawn like a shape and cuts its volume out of the meshes. Works on the mesh being edited, on the selected objects or on
the whole scene.

![](images/box_cutter.png)

## Objects

Object menu.

### Apply Transform

Moves the position, rotation and scale into the vertices.

### Origin to Geometry / to 3D Cursor / to Selection

Moves the object's origin; the geometry stays in place.

### Subdivide (Catmull-Clark)

Smooths the mesh by subdividing it.

![](images/catmull_clark.png)

### Remesh

The Object tab of the Hewn panel. Builds the selected meshes again, keeping their shape. The mesh must be closed.
- "Voxel": faces the size of a voxel. Adaptivity lowers the number of faces on flat areas, "Fix Poles" lines the
  vertices up along the surface. The volume and the attributes (vertex colors, smoothing) can be kept.

![](images/remesh_voxel.png)

- "Quad": quads of the density you want: by a number of faces, by a ratio to the current number of faces, or by edge
  length (the approximate number of faces is shown). Variation shifts the grid. The grid follows the axes, not the
  bends of the shape.

![](images/remesh_quad.png)

### Collision

The Collision Only check box: the game keeps only the object's collision (an invisible wall).

The Collision group in the Inspector of a Hewn mesh. Its kinds:

- Trimesh

![](images/collision_trimesh.png)

- Convex

![](images/collision_convex.png)

- Simplified Convex

![](images/collision_simplified_convex.png)

- Multiple Convex

![](images/collision_multiple_convex.png)

- Box

![](images/collision_box.png)

- Capsule

![](images/collision_capsule.png)

- Cylinder

![](images/collision_cylinder.png)

- Sphere

![](images/collision_sphere.png)

- Simplified Trimesh

![](images/collision_simplified_trimesh.png)

Metadata and Groups are put on the mesh and its collision body when baking. Collision Layer and Collision Mask in the
Collision section are the body's layers.

### Auto Smooth

Smooth shading with sharp edges by angle.

### To Quads

Merges the triangles of the whole mesh into quads.

![](images/to_quads.png)

### Conform Normals / Triangulate All / Flip All Normals

### Apply Modifiers

Moves the result of the modifiers into the mesh itself.

### Join Selected Objects

**Key:** Ctrl+J

### Remove Inner Faces

Removes the faces that cannot be seen at all: inside the body after joining objects that cross or touch. The inside
of a room (faces turned inward) stays. In edit mode "Select -> Inner Faces" only selects them. Limited Dissolve then
removes the extra edges on flat areas.

### Boolean Union / Difference / Intersection
<img src="icons/bool_union.svg" width="16" alt=""> <img src="icons/bool_subtract.svg" width="16" alt=""> <img src="icons/bool_intersect.svg" width="16" alt="">

Union:

![](images/boolean_union.png)

Difference:

![](images/boolean_difference.png)

Intersection:

![](images/boolean_intersection.png)

### MeshInstance3D -> Hewn

Turns a plain mesh (or a CSG shape) into a Hewn mesh.

### Bake to MeshInstance3D

Makes a plain MeshInstance3D of the selected Hewn meshes and curves.

### Export OBJ / Export glTF

Saves the selected mesh to a file (glTF or FBX).

### Object Menu
**Key:** RMB

In object mode: origin, cursor to selected, shading, normals, duplicate, delete, clear and apply transform.

![](images/object_menu.png)

### To Floor / To Ceiling

The selected objects go straight down (or up) until they touch: by their mesh or by their origin; "Along Surface
Normal" turns them to it. The panel at the bottom left.

![](images/to_floor.png)

### Physics Placement

The selected objects fall onto what is around them and settle. They can be dragged with the left button. "Apply"
(Enter) leaves them there as one undo step, "Cancel" (Esc) puts them back. Gravity is in the panel.

### Convert to Collision

The selected meshes as collision shapes of their form: convex, Trimesh, box, sphere, capsule, cylinder; in a static
body (the default), in an Area3D or on their own; "Keep Mesh" puts the body under the mesh.

### Delete
**Key:** X

In object mode deletes the selected nodes.

### Isolate / Frame Selected
<img src="icons/isolate.svg" width="16" alt=""><br>
**Keys:** / , .

## Modifiers

Modifiers are added in the Inspector, in the Modifiers array (Add Element). They work top to bottom; the order is
changed by dragging.

### HewnMirrorModifier

Mirrors the mesh along axes or along the planes of another node.

![](images/modifier_mirror.png)

### HewnSolidifyModifier

Gives the mesh a thickness.

![](images/modifier_solidify.png)

### HewnSubdivisionModifier

Smooths the mesh by subdividing it.

![](images/modifier_subdivision.png)

### HewnBooleanModifier

Joins, subtracts or intersects the mesh with other objects. The objects stay operands and can be moved.

![](images/modifier_boolean.png)

### HewnArrayModifier

Copies of the mesh along a line, in a circle or along a curve. On a Hewn curve with several splines "Per Curve" gives
the count to each. "Fit Curve Length": a line of as many copies as fit the length of the curve. "Caps": a mesh before
the first copy and after the last.

![](images/modifier_array.png)

### HewnCurveModifier

Bends the mesh along a curve: the "Deform Axis" goes along the first spline (from the curve's origin), the other two
across it, with the curve's tilt and radius.

![](images/modifier_curve.png)

### HewnRemeshModifier

Builds the mesh again on a voxel grid: Blocks (cubes), Smooth, Sharp (sharp corners kept, how much by Sharpness) or
Voxel (by the voxel size). For the first three, Octree Depth and Scale set the cell size. Remove Disconnected removes
small separate pieces, Smooth Shading turns on smooth shading. Quad and Triangles build a mesh of quads or triangles of
about Target Faces faces.

![](images/remesh_voxel.png)

### HewnFractureModifier

Breaks the object into pieces (Pieces, Seed); the cuts get the Inner Material, Gap moves the pieces apart. In the game
the pieces are one mesh, a mesh each, or frozen RigidBody3D nodes (group hewn_pieces).

Right click on an object -> "Apply Modifier" applies only that modifier.

![](images/modifier_fracture.png)

### HewnSimplifyModifier

Simplifies the mesh. Collapse keeps a share of the triangles (Ratio), Planar merges neighboring faces that meet at an
angle smaller than the limit (Angle Limit).

![](images/modifier_simplify.png)

## Textures

### Textures Dock

The pictures of the project. A click on a picture puts it on the selected faces, in object mode on the selected
objects. A picture can be dragged onto a face or an object.

The material for a picture is made once in the materials folder and reused after that.

![](images/textures_dock.png)

### Texture Transfer
**Keys:** Alt+click, Ctrl+Alt+click, Alt+RMB

With one face selected, Alt+click carries its texture onto other faces; the pattern continues across the edges. Dragged,
onto every face under the mouse.
Ctrl+Alt+click carries only the UVs. Alt+RMB takes the texture of the face clicked.

### Texture to Edge
**Key:** T

Puts the texture against the edge under the mouse. W / A / S / D - which side, Q - fit the height, E - fit the width,
R - scale 1. A click, Enter or T - apply, Esc - cancel.

![](images/texture_to_edge.png)

### Texture Transform
**Key:** Shift+T

Shows a tile of the texture on the chosen face with handles: corners and sides - scale, the middle - move, the circle -
turn. G / R / S - the same with the mouse, no handles. A click off the handles or Enter - apply, Esc - cancel.

![](images/texture_transform.png)

### Texture Tools
**Key:** U

The same tools as in the UV tab, in a menu at the mouse. With "Move / Rotate / Scale Texture" the texture follows the
mouse: X / Y - an axis, Ctrl - in steps.

- **Cycle** - a quarter turn.
- **Flip U / V**
- **Fit U / V** - one tile across the width or the height.
- **Center**, **Fit to Face**, **Random Offset**
- **Cube Projection**
- **Continue from Active** - the texture of the last selected face goes on to the others.
- **Unwrap Quads to Tile** - a strip of quads laid out into one tile.
- **Copy / Paste UV Rectangle**

### World UVs

The texture comes from the face's place in the world: neighboring objects join without seams. In the UV tab it is
turned on for new objects; "Apply World UVs" switches the selection over. The UV islands stay at the first tile, even
when the object is far from the center of the scene.

The texture is projected along the nearest world axis, so on slanted faces it is stretched. "Align to Face" projects
it onto the plane of the selected faces at its true size (a seam may appear with the neighbors), "Align to Axis" puts
it back.

### Hotspots

Rectangles of a material's image; each face gets the one that fits its shape and size. In the UV editor turn on
"Hotspots" and draw the rectangles with the mouse, then select faces: U -> "Hotspot".

### Strip Unwrap

U -> "Strip Unwrap": a strip of quads laid out as one band along U or V.

### Reshape UVs

U -> "Reshape UVs to the Faces": hand made UVs lie to the shape of the faces again after the vertices moved, without
stretching.

### UV Lock

Faces moved in edit mode take their texture with them.

### Material Tab

The mesh's materials and the texture settings of the selected faces: filtering, transparency, vertex colors,
roughness, metallic.

![](images/material_tab.png)

### Hide the Repeats

The "Hexagons", "Random Turn" and "Color Variation" check boxes make a shader material from the material, the texture's
repeats not lined up in it.

### Tool Materials

The Clip material is not drawn in the game but has collision. It is used for invisible walls.

### Lightmap UV2

Scene tab. Builds a second unwrap for light baking.

## UV Editor

The UV / UV2 switch in its header: the second channel is edited the same way.

The Hewn UV dock. Shows the UVs of the selected faces over the texture.

- 1 / 2 / 3 - UV vertices, edges, faces.
- Click, Shift+click, box - selection. L - the island under the mouse, Ctrl+L - the islands of the selection.
- G / R / S or drag - move, rotate, scale.
- Ctrl+P - pack the islands, Ctrl+A - make their scale even.
- The wheel - zoom, the middle button - pan.

![](images/uv_editor.png)

## Sculpting
<img src="icons/sculpt_mode.svg" width="16" alt="">

![](images/sculpt_header.png)

![](images/sculpt.png)

The mode is chosen in the menu at the start of the toolbar. Good for small terrain and soft shapes. LMB - a stroke; the
whole stroke is undone with one Ctrl+Z. Ctrl - the brush works the other way, Shift - smoothing with any brush.
Ctrl+wheel or [ / ] - radius, Shift+wheel - strength. If something is selected in edit mode, only the selection is
sculpted.

The tools in the column on the left:
- Raise / Lower.
- Smooth.
- Flatten - to the plane under the brush, with Ctrl - to the height where the stroke began.
- Grab - the area taken follows the mouse.
- Noise.
- Terrace - the heights are rounded to a step (the Step field on the toolbar).
- Pinch, Inflate.
- Sharp Edge - a groove, with Ctrl - a ridge.
- Density - only changes the detail (shown when dynamic topology is on).

On the toolbar: the radius in pixels, strength, direction ("Up" - straight up, slopes never fold over; "Along the
Surface" - out of the surface under the brush, the default), falloff (as in Vertex Paint, and "Custom" with a curve of
its own), pen pressure (on strength and on radius) and the cursor (on the screen or on the surface, its color).

### Dynamic Topology
<img src="icons/sc_density.svg" width="16" alt="">

A check box on the toolbar. Under the brush the surface is split into triangles of the right size: long edges are
split, short ones collapsed. The refine method: subdivide, collapse, or both. The detail: relative (in pixels on the
screen), constant (in meters) or brush detail (in percent of the radius).

## Vertex Paint
<img src="icons/tool_paint.svg" width="16" alt="">

![](images/vertex_paint.png)

![](images/vertex_paint_header.png)

The mode is chosen in the menu at the start of the toolbar. Vertices, edges or faces are painted (1 / 2 / 3), and if
something is selected, only the selection.

### Brush
<img src="icons/tool_paint.svg" width="16" alt="">

LMB paints. Ctrl+wheel - radius, Shift+wheel - strength, Ctrl+click - pick a color.

### Pencil
<img src="icons/tool_pencil.svg" width="16" alt="">

Paints one vertex.

### Fill
<img src="icons/tool_fill.svg" width="16" alt="">

Paints the selection or a connected part.

### Eyedropper
<img src="icons/tool_picker.svg" width="16" alt="">

Takes a color. With Shift - into the second color.

### Normals
<img src="icons/tool_normals.svg" width="16" alt="">

LMB makes the normals smooth, Ctrl+LMB flat.

### Colors
**Keys:** X, D

![](images/vertex_paint_colors.png)

X swaps the two colors, D resets them to black and white.

### Bake Light

The Colors menu or Object -> "Bake Light to Vertex Colors". Writes the light and shadows of the scene's lights into the
vertex colors.

### Blend Material

The Material tab. Blends up to four textures by the vertex colors. Check boxes in the window give each texture
metallic and roughness, a normal map, an ambient occlusion map, emission; their values are set in the Inspector. "Scrolling": the textures move
(scroll angle and speed), with a shader of its own.
- Noise mask: the borders between the textures are broken up by a noise; each layer has its own strength, the noise's
  scale and softness are shared.
- Lift by paint: a layer lifts the vertices up by its height where it is painted (snow). The collision does not change,
  and the lift shows only on a dense enough mesh.

### Scrolling Material

Material tab -> "Scrolling Material": the picture (of the selection's material, or of the texture picked last) moves
across the faces (water, a conveyor). Its direction and speed are in the Inspector; a shader of its own, the plain
materials stay as they are.

## Scenes and Objects

### Scenes Dock

A click on a scene - the scene follows the mouse, releasing places it. Q / E - turn by 90°, N - along the surface
normal, R - another random turn. "Placement..." - random rotation and size. RMB on a scene: add it to the favorites
(Shift+A -> Favorites) or show it in the FileSystem.

### Entities

The Create tab. A light, a spotlight, a sound and objects of your own from `res://hewn_entities.json`.

### Decals and Overlays

A picture on a surface. A decal is a Godot Decal node. An overlay lies on Hewn faces and is saved as geometry. - / = while
placing change the size.

The corner points of an overlay can be dragged with the mouse right in the viewport; the picture warps after them. In the
Inspector, in the Shape section:
- Control Points: 4 points or 9 (with them the sides can bend).
- Reset Shape: back to a rectangle.

![](images/overlay.png)

An overlay can be mirrored and cut with the Clip tool.

### Reference Image

Shift+A -> Image. A picture to trace over; not seen in the game.

### Scatter
<img src="icons/scatter_add.svg" width="16" alt=""> <img src="icons/scatter_erase.svg" width="16" alt=""> <img src="icons/scatter_thin.svg" width="16" alt="">

Shift+A -> Scatter. Copies of meshes or scenes painted on surfaces: grass, rocks, bushes. In the Inspector: the meshes and their
weights, density, brush radius, random rotation (also around X and Z), tilt and scale, "Base Rotation" (for a mesh made
lying down), "Align" (up or along the surface normal), offset. "Collisions": per mesh (convex, trimesh, box), made when
the game runs and when baking. While the node is selected, its brushes are in the tool column:
- Brush: adds copies. Ctrl - erase, Shift - thin out.
- Erase, Thin Out.
- Ctrl+wheel or [ / ] - radius.

While it is selected, its bottom panel opens: the meshes (Ctrl / Shift - several; the brush places the selected ones,
or all if none), "Add Mesh", "Remove", "All" / "None", "Erase Strength" (below 1 the eraser thins out) and the brush
settings (density, radius, scale, rotation, tilt, offset). Tab leaves the brush, like Esc.

The copies are kept in chunks ("Chunk Size"), each drawn by its own MultiMeshInstance3D with a "Visibility Range". A
stroke rebuilds only the chunks it touches. Baking turns them into plain MultiMeshInstance3D nodes.

![](images/scatter.png)

### Baking the Level

Bake Level opens its options first, then asks for the file. They are kept in the project and used when the game is
exported too.
- Occluders: for big opaque meshes. They work only with occlusion culling on in the Project Settings; the window warns
  when it is off.
- Merge static meshes: small objects with the same metadata, groups and render settings are joined in squares of the
  map.
- Split big meshes: detailed meshes bigger than the given size (128 m by default) are cut into squares of that size.
- Both leave alone objects with a script, under a moving parent, or with lightmap UV2. For one object: Merge At Bake in
  the Inspector.
- Also plain meshes: by default MeshInstance3D nodes not made by Hewn are neither joined nor cut.
- Merge collision, Detail levels (LOD), Visibility range by size, Remove hidden faces.

Run Baked: the level as it will be after export. F5 runs the editable nodes, unbaked.

Meshes, curves, scatters and things along curves have metadata, groups and scripts for their game nodes. The object's
script extends MeshInstance3D (a mesh) or Path3D (a curve), a collision script StaticBody3D.

### Roads and Bridges

Shift+A -> Curve -> Road (Overlay on the Ground), Road (Mesh on the Ground) or Bridge. Any curve can have one too: the
Road section of the Inspector. Pick its texture by clicking one with the curve selected. The picture lies once across
the road and repeats along it.
- Mode: off, mesh or overlay.
- Width: the width.
- Repeat: the length of one repeat of the picture along the road (0 - from its proportions).
- Reach: how far under the curve a surface is looked for.
- Step: how often along the road the height is checked.

An overlay lies on the highest surface under the curve and follows it when that is edited, adding no vertices to the
mesh. Softness is how soft its edges are, in meters; Order is which road is on top where they overlap. Edge Noise makes
the soft edge ragged: by a noise of Edge Noise Size and by the brightness of the road's texture (Edge Height) or a
height map of its own.

A mesh runs exactly along the curve, or with Snap on goes down onto the surface under it. Its settings: segments
across, thickness, curb height and width, lift off the ground. A bridge starts with curbs, collision and piers placed
along the curve. Side Material is a separate material for the sides, curbs and underside, Bottom turns the underside
off.

![](images/roads.png)

### Curves
<img src="icons/curve.svg" width="16" alt="">

Shift+A -> Curve: Bezier, Circle, NURBS Curve, NURBS Circle, Path3D, Rope. A Hewn curve is a Path3D (PathFollow3D works
with its first spline) made of several splines: poly, Bezier or NURBS, with a radius and a tilt at each point. In the
Inspector: "Resolution", "2D" (in the XZ plane, "Fill" fills closed ones), "Offset", "Extrude", "Taper Object", the
bevel ("Round" with a depth and resolution, "Object" - the shape of another curve, "Profile" - a quarter turned round),
"Fill Caps", material, collision. In the game it is a plain Path3D with its own mesh.

A curve is drawn point by point like a wall. Without "Bake Path" it is not a Path3D in the game.

Walls, sweeps and path stairs are made on a curve of their own and follow it. "Convert to Curve" (right click on the
object) gives one to a shape without it.

Tab - editing the points:
- Click / Shift / box: selection, A / Alt+A, Ctrl+I. A handle can be dragged with the mouse, or clicked to select it
  for G / R / S.
- G / R / S, X / Y / Z (again - local), Ctrl - snapping, Shift+Tab - snapping on / off, B - from point to point.
- Alt+S - radius, Ctrl+T - tilt, Alt+T - clear tilt.
- E - extrude: after an end a new point; a run of points in the middle is pulled out, with new points at its borders. F - make a segment (join two ends or
  close a spline). Ctrl+click - a point at the mouse. Shift+D - duplicate.
- V - handle type, Alt+C - cyclic, Shift+N - recalculate handles, P - separate, Ctrl+X - dissolve, X - delete.
- Ctrl+B - bevel: the selected points become arcs, the wheel sets the segments.
- GG - the points slide along their pieces. M - merge the selected points.
- RMB - a menu: subdivide, draw (a stroke on the plane of the 3D cursor or on surfaces), smooth (position, tilt,
  radius), simplify, set radius / goal weight, mirror, snap, spline type, direction, split.

RMB in object mode: to mesh, origin, duplicate, mirror, snap. The same is in the Curve menu on the toolbar while a curve
is selected. "Mesh -> Curve" (Convert) and "Create from Edges" (the edge menu) make curves from edges. A click
anywhere on a curve selects it. While its points are edited it is out of the selection (no gizmo); Esc leaves like Tab;
Ctrl+click on a spline selects all its points; during G / R / S a number can be typed (S Y 0), Shift+X / Y / Z - all
axes but that one; R twice - trackball; points snap to the points of curves. The Draw tool is in the tool column while
the curve is edited.

"Placements" (Inspector): meshes along it, each a MultiMesh: in the middle, on the left, on the right or on both sides
("Offset"), as many as fit ("Spacing" 0 - by the mesh's length) or at a given distance, at random ("Spacing Random",
"Chance", "Spread", scale, rotation), in stretches ("Patch Length", "Patch Amount"), dropped onto the ground under
them, with collision. "Reach Ground": each copy is stretched down to the surface (piers, posts).

"Vertex Paint" (Inspector): the meshes under it are painted (a color or CUSTOM0..3, width, falloff, strength, height):
a path with a blend material. It is shown and baked but not saved in the mesh: move the curve away and the colors come
back.

### Building

Shift+A -> Curve -> Building: a rectangle drawn with two clicks (its corners). The building (HewnBuilding3D) is made on
its outline curve: the floor, walls, windows and roof follow its points.

The floors are a list in the Inspector (bottom up), each with:
- its wall height, floor and wall thickness, whether it has a floor, walls, windows;
- windows: width, height, sill, spacing, the sides without them;
- an outline of its own (a curve under the building; empty: the one below's).

There too: the roof, a cornice, a plinth, windows in the gables and the materials.

Right click on the building -> "Building":
- "Add Floor" (a copy of the top one), "Remove Top Floor".
- "Own Outline for the Top Floor": a floor of another shape.
- "Add Courtyard": cut out of the building, with walls along it.

Tab on an outline, two neighboring points selected, right click -> "Building Side":
- "Roof: Gable / Slope / By Style".
- "No Windows" / "Windows" (the floors on this outline).
- "Add Door": an opening in the middle of the side.

Windows (HewnWindows3D) are set out along the outline's sides; each opening gets a cutter, a scene or a mesh.

![](images/building.png)

### Rope

Shift+A -> Curve -> Rope.

![](images/rope.png)

### Bake Navigation Mesh

Scene tab, or the Object menu: a plain NavigationMesh of the selected objects (nothing selected - of the whole
scene), its settings in a window as in Godot (by default from what collides), saved to a file; a NavigationRegion3D with
it can be added to the scene. After the level changes it is baked again the same way.

### TODO Note

Shift+A -> TODO Note: a note at a place in the level (what is planned there); its text is in the panel. It is not in
the game.

![](images/todo-note.png)

## Game and Export

Scene tab.

### Bake Level

Saves a copy of the scene for the game: plain MeshInstance3D nodes, collision, occluders. The edited scene stays
editable.

### Export Scene glTF

Saves the whole scene in one file (glTF or FBX).

### Check Scene

A list of the scene's problems: faces without area, unwelded vertices, faces that are not flat and others. Most of
them can be fixed from there.

### Scene Size

Shows what takes the room in the saved scene.

## Quad View

A tab next to 2D / 3D. The 3D view at the top left, the others from the top, the front and the side.

![](images/quad_view.png)

### Block
**Key:** Shift+B

Drag a box in a 2D view; Enter makes the shape.

![](images/quad-block.png)

### Clip
**Key:** Shift+X

Draw a line; Enter cuts the selected meshes.

### Maximize View
**Key:** Shift+Z

## Settings

Editor -> Editor Settings -> Hewn.

- **Interface -> Hotkeys** - Hewn's keys on / off.
- **Interface -> Last Operation Panel** - the Last Operation panel.
- **Interface -> Quad View** - the Quad view.
- **Interface -> Toggle On Color** - the background of the header and tool buttons that are on.
- **Interface -> Wrap Cursor At Viewport Edges** - when dragging, the mouse comes out at the other side of the viewport.
- **Warnings -> Subdivide Face Limit** - how many faces a subdivision may add without asking.

The keys are changed in Editor -> Editor Settings -> Shortcuts, the "hewn" section.
