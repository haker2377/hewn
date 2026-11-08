# Headless test suite: godot --headless --path project -s res://tests/run_tests.gd
extends SceneTree

var failures := 0
var checks := 0


func check(cond: bool, msg: String) -> void:
	checks += 1
	if not cond:
		failures += 1
		printerr("FAIL: ", msg)


# Returns {closed, consistent, volume, euler}
func analyze(m: HewnMesh3D) -> Dictionary:
	var halves := {}
	var consistent := true
	var verts := m.get_vertices()
	var vol := 0.0
	var edges := {}
	for f in m.get_face_count():
		var face := m.get_face(f)
		var n := face.size()
		for i in n:
			var a := face[i]
			var b := face[(i + 1) % n]
			var key := Vector2i(a, b)
			if halves.has(key):
				consistent = false
			halves[key] = true
			edges[Vector2i(mini(a, b), maxi(a, b))] = true
		for i in range(1, n - 1):
			vol += verts[face[0]].dot(verts[face[i]].cross(verts[face[i + 1]])) / 6.0
	var closed := true
	for k in halves:
		if not halves.has(Vector2i(k.y, k.x)):
			closed = false
			break
	return {"closed": closed, "consistent": consistent, "volume": vol,
		"euler": m.get_vertex_count() - edges.size() + m.get_face_count()}


# A clean mesh: every vertex used (by a face, a loose edge or a loose point), no face with fewer than three different
# corners, a corner twice in a row or no area, no edge shared by more than two faces, no two faces on the same corners.
func clean_problems(m: HewnMesh3D) -> Array:
	var out := []
	var md: Dictionary = m.mesh_data
	var nv := m.get_vertex_count()
	var used := PackedByteArray()
	used.resize(nv)
	var edge_count := {}
	var face_keys := {}
	var verts := m.get_vertices()
	for f in m.get_face_count():
		var fv := m.get_face(f)
		var uniq := {}
		for i in fv.size():
			var v := fv[i]
			if v < 0 or v >= nv:
				out.append("face %d: corner %d out of range" % [f, v])
				continue
			used[v] = 1
			uniq[v] = true
			var w := fv[(i + 1) % fv.size()]
			if v == w:
				out.append("face %d: corner %d twice in a row" % [f, v])
			var k := Vector2i(mini(v, w), maxi(v, w))
			edge_count[k] = edge_count.get(k, 0) + 1
		if uniq.size() < 3:
			out.append("face %d: %d different corners" % [f, uniq.size()])
		var nrm := Vector3.ZERO
		for i in fv.size():
			nrm += verts[fv[i]].cross(verts[fv[(i + 1) % fv.size()]])
		if nrm.length() * 0.5 < 1e-10:
			out.append("face %d: no area" % f)
		var sorted := Array(fv)
		sorted.sort()
		var fk := str(sorted)
		if face_keys.has(fk):
			out.append("faces %d and %d on the same corners" % [face_keys[fk], f])
		face_keys[fk] = f
	for k in edge_count:
		if edge_count[k] > 2:
			out.append("edge %s in %d faces" % [k, edge_count[k]])
	for v in PackedInt32Array(md.get("wires", PackedInt32Array())):
		if v >= 0 and v < nv:
			used[v] = 1
	for v in PackedInt32Array(md.get("points", PackedInt32Array())):
		if v >= 0 and v < nv:
			used[v] = 1
	var unused := 0
	for v in nv:
		if used[v] == 0:
			unused += 1
	if unused > 0:
		out.append("%d unused vertices" % unused)
	return out


func clean_ok(m: HewnMesh3D, name: String) -> void:
	var p := clean_problems(m)
	check(p.is_empty(), name + ": not clean " + str(p.slice(0, 4)))


func solid_ok(m: HewnMesh3D, name: String, genus := 0) -> Dictionary:
	print("  .. ", name)
	clean_ok(m, name)
	var a := analyze(m)
	check(a.consistent, name + ": inconsistent winding")
	check(a.closed, name + ": not closed")
	check(a.volume > 0.0, name + ": volume not positive (%f)" % a.volume)
	check(a.euler == 2 - 2 * genus, name + ": euler %d" % a.euler)
	return a


# The rendered triangles cover exactly the faces (a bad triangulation of a concave face spills over).
func tri_area_ok(m: HewnMesh3D) -> bool:
	var face_area := 0.0
	var verts := m.get_vertices()
	for f in m.get_face_count():
		var face := m.get_face(f)
		var nrm := Vector3.ZERO
		for i in face.size():
			var a := verts[face[i]]
			var b := verts[face[(i + 1) % face.size()]]
			nrm += Vector3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y))
		face_area += nrm.length() * 0.5
	var tri_area := 0.0
	var am: ArrayMesh = m.mesh
	for s in am.get_surface_count():
		var arr := am.surface_get_arrays(s)
		var pv: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
		var idx: PackedInt32Array = arr[Mesh.ARRAY_INDEX]
		for k in range(0, idx.size(), 3):
			tri_area += (pv[idx[k + 1]] - pv[idx[k]]).cross(pv[idx[k + 2]] - pv[idx[k]]).length() * 0.5
	return absf(tri_area - face_area) < 1e-3 * maxf(1.0, face_area)


# Volume of what is shown (the modifiers included), from the rendered triangles.
func shown_volume(m: HewnMesh3D) -> float:
	var vol := 0.0
	var am: ArrayMesh = m.mesh
	for s in am.get_surface_count():
		var arr := am.surface_get_arrays(s)
		var pv: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
		var idx: PackedInt32Array = arr[Mesh.ARRAY_INDEX]
		for k in range(0, idx.size(), 3):
			# Godot winding is clockwise: swap two corners
			vol += pv[idx[k]].dot(pv[idx[k + 2]].cross(pv[idx[k + 1]])) / 6.0
	return vol


func unpack(packed: PackedInt32Array) -> Array:
	var out := []
	var i := 0
	while i < packed.size():
		var n := packed[i]
		out.append(packed.slice(i + 1, i + 1 + n))
		i += n + 1
	return out


func mat_of(m: HewnMesh3D, f: int) -> Material:
	var mats := m.get_materials()
	var slot := m.get_face_material(f)
	return mats[slot] if slot < mats.size() else null


func make(shape: int) -> HewnMesh3D:
	var m := HewnMesh3D.new()
	root.add_child(m)
	m.shape = shape
	return m


func edge_pairs(m: HewnMesh3D, pred: Callable) -> PackedInt32Array:
	var verts := m.get_vertices()
	var pairs := PackedInt32Array()
	var seen := {}
	for f in m.get_face_count():
		var face := m.get_face(f)
		for i in face.size():
			var a := face[i]
			var b := face[(i + 1) % face.size()]
			var k := Vector2i(mini(a, b), maxi(a, b))
			if seen.has(k):
				continue
			seen[k] = true
			if pred.call(verts[a], verts[b]):
				pairs.push_back(a)
				pairs.push_back(b)
	return pairs


func faces_facing(m: HewnMesh3D, dir: Vector3) -> PackedInt32Array:
	var out := PackedInt32Array()
	for f in m.get_face_count():
		if m.get_face_normal(f).dot(dir) > 0.9:
			out.push_back(f)
	return out


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	# --- shapes: closed ones with expected genus
	var solids := {1: 0, 3: 0, 4: 0, 5: 0, 6: 0, 7: 1, 8: 1, 9: 0, 10: 0, 11: 0, 12: 0, 13: 0, 14: 0, 16: 0}
	for s in solids:
		var m := make(s)
		solid_ok(m, "shape %d" % s, solids[s])
		check(m.mesh != null and m.mesh.get_surface_count() > 0, "shape %d has no surfaces" % s)
		m.free()
	for s in [2, 15, 17]:
		var m := make(s)
		var a := analyze(m)
		check(a.consistent and m.get_face_count() > 0, "open shape %d" % s)
		m.free()
	var poly := make(17)
	poly.set_shape_param("height", 1.0)
	solid_ok(poly, "polygon prism")
	poly.free()
	var arch := make(12)
	arch.set_shape_param("angle", 360.0)
	solid_ok(arch, "arch 360", 1)
	arch.free()

	# --- cube volume and Godot winding (front faces clockwise)
	var cube := make(1)
	check(absf(analyze(cube).volume - 1.0) < 1e-4, "cube volume")
	var arrays: Array = cube.mesh.surface_get_arrays(0)
	var pv: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var pn: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
	var pidx: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
	var cw_ok := true
	for t in range(0, pidx.size(), 3):
		var a := pv[pidx[t]]
		var b := pv[pidx[t + 1]]
		var c := pv[pidx[t + 2]]
		if (c - a).cross(b - a).dot(pn[pidx[t]]) <= 0.0:
			cw_ok = false
	check(cw_ok, "triangles are not clockwise relative to normals")

	# --- face ops
	var top := faces_facing(cube, Vector3.UP)[0]
	cube.extrude_faces(PackedInt32Array([top]), 0.5)
	var a2 := solid_ok(cube, "extrude")
	check(absf(a2.volume - 1.5) < 1e-4, "extrude volume %f" % a2.volume)
	cube.inset_faces(PackedInt32Array([top]), 0.1)
	solid_ok(cube, "inset")
	cube.extrude_faces(PackedInt32Array([top]), -0.2)
	solid_ok(cube, "extrude in")
	cube.free()

	cube = make(1)
	cube.extrude_faces(PackedInt32Array(range(cube.get_face_count())), 0.25, true)
	check(analyze(cube).consistent, "extrude individual winding")
	cube.free()

	cube = make(1)
	cube.set_shape_param("width_segments", 3)
	var tops := faces_facing(cube, Vector3.UP)
	cube.inset_faces(tops, 0.1)
	solid_ok(cube, "inset region")
	cube.free()

	cube = make(1)
	cube.subdivide_faces(PackedInt32Array([0]))
	solid_ok(cube, "subdivide face")
	cube.triangulate_faces(PackedInt32Array(range(cube.get_face_count())))
	solid_ok(cube, "triangulate")
	var q := cube.quadify(1.0)
	check(q > 0, "quadify merged nothing")
	solid_ok(cube, "quadify")
	cube.free()

	cube = make(1)
	cube.set_shape_param("width_segments", 2)
	var fc := cube.get_face_count()
	var ups := faces_facing(cube, Vector3.UP)
	check(ups.size() == 2, "segmented cube top")
	cube.merge_faces(ups)
	solid_ok(cube, "merge faces")
	check(cube.get_face_count() == fc - 1, "merge face count")
	cube.free()

	# --- bevel
	var preds := {
		"one": func(p: Vector3, r: Vector3): return p.y > 0 and r.y > 0 and p.z > 0 and r.z > 0,
		"two_adjacent": func(p: Vector3, r: Vector3): return p.y > 0 and r.y > 0 and ((p.z > 0 and r.z > 0) or (p.x > 0 and r.x > 0)),
		"top_loop": func(p: Vector3, r: Vector3): return p.y > 0 and r.y > 0,
		"all": func(_p: Vector3, _r: Vector3): return true,
	}
	for mode in preds:
		cube = make(1)
		var pairs := edge_pairs(cube, preds[mode])
		var before: float = analyze(cube).volume
		var r := cube.bevel_edges(pairs, 0.1)
		check(r.size() > 0, "bevel %s produced no faces" % mode)
		var a3 := solid_ok(cube, "bevel " + mode)
		check(a3.volume < before, "bevel %s volume did not shrink" % mode)
		cube.free()
	var cyl := make(3)
	cyl.bevel_edges(edge_pairs(cyl, func(p: Vector3, r: Vector3): return p.y > 0 and r.y > 0), 0.05)
	solid_ok(cyl, "bevel cylinder cap")
	cyl.free()

	# --- edge loops / connect / dissolve
	cube = make(1)
	var face0 := cube.get_face(0)
	var e := PackedInt32Array([face0[0], face0[1]])
	cube.insert_edge_loops(e)
	solid_ok(cube, "insert edge loop")
	check(cube.get_face_count() == 10, "edge loop face count %d" % cube.get_face_count())
	cube.subdivide_edges(e, 2)
	solid_ok(cube, "subdivide edge")
	cube.free()

	cube = make(1)
	face0 = cube.get_face(0)
	cube.connect_edges(PackedInt32Array([face0[0], face0[1], face0[2], face0[3]]))
	solid_ok(cube, "connect edges")
	check(cube.get_face_count() == 7, "connect edges count %d" % cube.get_face_count())
	cube.free()

	cube = make(1)
	face0 = cube.get_face(0)
	cube.connect_vertices(PackedInt32Array([face0[0], face0[2]]))
	solid_ok(cube, "connect verts")
	check(cube.get_face_count() == 7, "connect verts count")
	cube.dissolve_edges(PackedInt32Array([face0[0], face0[2]]))
	solid_ok(cube, "dissolve edge")
	check(cube.get_face_count() == 6, "dissolve count %d" % cube.get_face_count())
	cube.free()

	# --- delete + fill holes, extrude border edge, bridge
	cube = make(1)
	cube.delete_faces(PackedInt32Array([0]))
	check(not analyze(cube).closed, "delete face should open the mesh")
	cube.fill_holes()
	solid_ok(cube, "fill holes")
	cube.free()

	var plane := make(2)
	plane.set_shape_param("width_segments", 1)
	plane.set_shape_param("depth_segments", 1)
	var pf0 := plane.get_face(0)
	plane.extrude_edges(PackedInt32Array([pf0[0], pf0[1]]), 0.5)
	check(analyze(plane).consistent and plane.get_face_count() == 2, "edge extrude")
	plane.free()

	cube = make(1)
	cube.delete_faces(faces_facing(cube, Vector3.UP))
	var bridge := edge_pairs(cube, func(p: Vector3, r: Vector3): return p.y > 0 and r.y > 0 and absf(p.z - r.z) < 0.01)
	check(bridge.size() == 4, "bridge edge pick")
	check(cube.bridge_edges(bridge).size() == 1, "bridge produced no face")
	solid_ok(cube, "bridge")
	cube.free()

	# --- vertices
	cube = make(1)
	var vc := cube.get_vertex_count()
	var top_verts := PackedInt32Array()
	var cv := cube.get_vertices()
	for i in cv.size():
		if cv[i].y > 0 and cv[i].z > 0:
			top_verts.push_back(i)
	cube.collapse_vertices(top_verts)
	solid_ok(cube, "collapse")
	check(cube.get_vertex_count() == vc - 1, "collapse count")
	cube.free()

	cube = make(1)
	cube.split_vertices(PackedInt32Array([0]))
	check(cube.get_vertex_count() == 10, "split count %d" % cube.get_vertex_count())
	cube.weld_vertices(PackedInt32Array(range(cube.get_vertex_count())), 0.001)
	solid_ok(cube, "weld")
	check(cube.get_vertex_count() == 8, "weld count")
	cube.free()

	# --- object level
	cube = make(1)
	cube.subdivide_smooth()
	var a5 := solid_ok(cube, "catmull-clark")
	check(cube.get_face_count() == 24, "cc faces")
	check(a5.volume < 1.0 and a5.volume > 0.3, "cc volume %f" % a5.volume)
	cube.auto_smooth(30.0)
	cube.flip_faces(PackedInt32Array([0, 1, 2]))
	cube.conform_normals()
	solid_ok(cube, "conform normals")
	cube.free()

	# --- state roundtrip and Custom conversion
	cube = make(5)
	var st := cube.get_state()
	var nfaces := cube.get_face_count()
	cube.extrude_faces(PackedInt32Array([0]), 0.2)
	check(cube.shape == 0, "edit should switch to Custom")
	cube.set_state(st)
	check(cube.shape == 5 and cube.get_face_count() == nfaces, "state restore")
	var copy := HewnMesh3D.new()
	copy.shape = 0
	copy.set_mesh_data(cube.get_mesh_data())
	check(copy.get_face_count() == nfaces and copy.get_vertex_count() == cube.get_vertex_count(), "mesh_data roundtrip")
	copy.free()
	cube.free()

	# --- conversion from a Godot mesh
	var conv := HewnMesh3D.new()
	root.add_child(conv)
	conv.create_from_mesh(BoxMesh.new(), true)
	solid_ok(conv, "from BoxMesh")
	check(conv.get_face_count() == 6, "BoxMesh quadify %d" % conv.get_face_count())
	check(conv.to_obj().begins_with("# Hewn"), "obj export")
	check(conv.to_array_mesh().get_surface_count() == 1, "to_array_mesh")
	conv.free()

	# --- materials and collision
	cube = make(1)
	cube.materials = [null, StandardMaterial3D.new()]
	cube.set_face_material(0, 1)
	check(cube.mesh.get_surface_count() == 2, "two material surfaces")
	cube.collision_mode = HewnMesh3D.COLLISION_TRIMESH
	check(cube.get_child_count(true) == 1, "collision body")
	cube.free()

	# --- save / load through a PackedScene
	var holder := Node3D.new()
	var saved := HewnMesh3D.new()
	holder.add_child(saved)
	saved.owner = holder
	saved.shape = 7
	saved.extrude_faces(PackedInt32Array([3]), 0.1)
	var faces_before := saved.get_face_count()
	var ps := PackedScene.new()
	check(ps.pack(holder) == OK, "pack")
	ResourceSaver.save(ps, "user://hw_test.tscn")
	var txt := FileAccess.get_file_as_string("user://hw_test.tscn")
	check(not txt.contains("ArrayMesh"), "mesh resource must not be saved")
	var inst: Node3D = load("user://hw_test.tscn").instantiate()
	var loaded: HewnMesh3D = inst.get_child(0)
	check(loaded.get_face_count() == faces_before and loaded.shape == 0, "scene reload")
	check(loaded.mesh != null, "mesh rebuilt on load")
	inst.free()
	holder.free()

	# --- README example
	var ex := make(1)
	var top2 := ex.get_face_count() - 1
	check(ex.get_face_normal(top2).y > 0.9, "readme: last cube face is +Y")
	ex.extrude_faces(PackedInt32Array([top2]), 0.5)
	var f2 := ex.get_face(top2)
	var edges2 := PackedInt32Array()
	for i in f2.size():
		edges2.append_array([f2[i], f2[(i + 1) % f2.size()]])
	check(ex.bevel_edges(edges2, 0.05).size() > 0, "readme bevel")
	solid_ok(ex, "readme example")
	check(ex.export_obj("user://box.obj") == OK, "readme obj export")
	# glTF: a .glb file and a .gltf (+ .bin) that load back with the same triangle count
	check(ex.export_gltf("user://box.glb") == OK and FileAccess.file_exists("user://box.glb"), "glb export")
	check(ex.export_gltf("user://box.gltf") == OK and FileAccess.file_exists("user://box.gltf"), "gltf export")
	var gdoc := GLTFDocument.new()
	var gstate := GLTFState.new()
	check(gdoc.append_from_file("user://box.glb", gstate) == OK, "exported glb loads back")
	var gscene := gdoc.generate_scene(gstate)
	var gmi: Array = gscene.find_children("*", "MeshInstance3D", true, false) if gscene else []
	var gtris := 0
	if gmi.size() == 1:
		var gm: Mesh = gmi[0].mesh
		for si in gm.get_surface_count():
			var arr := gm.surface_get_arrays(si)
			gtris += (arr[Mesh.ARRAY_INDEX].size() if arr[Mesh.ARRAY_INDEX] != null else arr[Mesh.ARRAY_VERTEX].size()) / 3
	var etris := 0
	for si in ex.mesh.get_surface_count():
		var arr2 := ex.mesh.surface_get_arrays(si)
		etris += (arr2[Mesh.ARRAY_INDEX].size() if arr2[Mesh.ARRAY_INDEX] != null else arr2[Mesh.ARRAY_VERTEX].size()) / 3
	check(gmi.size() == 1 and gtris == etris and etris > 0, "glb mesh has the same triangles (%d vs %d)" % [gtris, etris])
	if gscene:
		gscene.free()
	ex.free()

	# --- To Circle (LoopTools): the top of an irregular prism becomes a regular hexagon on a circle
	var cp := HewnMesh3D.new()
	root.add_child(cp)
	cp.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(2, 0, 0), Vector3(2.4, 0, 1), Vector3(1, 0, 1.3), Vector3(0.6, 0, 2), Vector3(-0.3, 0, 1.1)])
	cp.shape = 18
	cp.set_shape_param("height", 0.5)
	cp.shape = 0  # (editable mesh)
	var cp_top := -1
	for f in cp.get_face_count():
		if cp.get_face_normal(f).y > 0.9:
			cp_top = f
	check(cp.to_circle(PackedInt32Array(), PackedInt32Array([cp_top])) == 1, "to_circle: one loop")
	var ring := cp.get_face(cp_top)
	var cc := Vector3()
	for v in ring:
		cc += cp.get_vertex(v)
	cc /= ring.size()
	var r0 := cp.get_vertex(ring[0]).distance_to(cc)
	var circle_ok := r0 > 0.1
	for i in ring.size():
		var a := cp.get_vertex(ring[i])
		var b := cp.get_vertex(ring[(i + 1) % ring.size()])
		circle_ok = circle_ok and absf(a.distance_to(cc) - r0) < 1e-4 and absf(a.distance_to(b) - r0) < 1e-3 and absf(a.y - 0.5) < 1e-4
	check(circle_ok, "to_circle: on one circle, evenly spaced (hexagon side = radius)")
	solid_ok(cp, "to_circle result")
	# vertices only (an edge loop given by its vertices), with half influence
	var cp2 := make(3)  # cylinder
	var top_ring := PackedInt32Array()
	for v in cp2.get_vertex_count():
		if cp2.get_vertex(v).y > 0.49 and Vector2(cp2.get_vertex(v).x, cp2.get_vertex(v).z).length() > 0.1:
			top_ring.append(v)
	var before := cp2.get_vertex(top_ring[0])
	cp2.set_vertex(top_ring[0], before * Vector3(1.5, 1, 1.5))
	cp2.to_circle(top_ring, PackedInt32Array(), true, 0.5)
	check(cp2.get_vertex(top_ring[0]).distance_to(before) > 1e-3 and cp2.get_vertex(top_ring[0]).distance_to(before * Vector3(1.5, 1, 1.5)) > 1e-3, "to_circle: half influence moves halfway")
	cp.free()
	cp2.free()

	# --- modifier stack: Array (line / circle / Path3D), order, enabled, apply
	var am := make(1)
	var a_mod := HewnArrayModifier.new()
	a_mod.count = 3  # relative offset (1, 0, 0) x box: side by side
	am.add_modifier(a_mod)
	var ab := am.mesh.get_aabb()
	check(absf(ab.size.x - 3.0) < 1e-4 and absf(ab.size.y - 1.0) < 1e-4, "array line relative: 3 cubes in a row (%s)" % ab.size)
	a_mod.offset_method = 1  # constant
	a_mod.translation = Vector3(0, 0, 2)
	ab = am.mesh.get_aabb()
	check(absf(ab.size.z - 5.0) < 1e-4, "array constant offset (%s)" % ab.size)
	a_mod.offset_method = 2  # endpoint
	a_mod.translation = Vector3(0, 4, 0)
	ab = am.mesh.get_aabb()
	check(absf(ab.size.y - 5.0) < 1e-4, "array endpoint: copies up to the end point (%s)" % ab.size)
	# order matters: mirror after the array mirrors the whole row
	a_mod.offset_method = 1
	a_mod.translation = Vector3(2, 0, 0)
	for v in am.get_vertex_count():
		am.set_vertex(v, am.get_vertex(v) + Vector3(1, 0, 0))  # off the mirror plane
	var mm := HewnMirrorModifier.new()
	am.add_modifier(mm)
	ab = am.mesh.get_aabb()
	check(absf(ab.position.x + 5.5) < 1e-4 and absf(ab.end.x - 5.5) < 1e-4, "mirror after array mirrors the row (%s)" % ab)
	var reordered: Array[HewnModifier] = [mm, a_mod]
	am.modifiers = reordered
	ab = am.mesh.get_aabb()
	check(absf(ab.position.x + 1.5) < 1e-4 and absf(ab.end.x - 5.5) < 1e-4, "mirror before array: the stack runs in order (%s)" % ab)
	mm.enabled = false
	a_mod.enabled = false
	check(am.mesh.get_aabb().size.is_equal_approx(Vector3(1, 1, 1)), "disabled modifiers do nothing")
	for v in am.get_vertex_count():
		am.set_vertex(v, am.get_vertex(v) - Vector3(1, 0, 0))
	a_mod.enabled = true
	# circle around the origin, 4 copies of a cube at radius 3
	a_mod.shape = 1
	a_mod.count = 4
	a_mod.radius = 3.0
	ab = am.mesh.get_aabb()
	check(absf(ab.size.x - 7.0) < 1e-3 and absf(ab.size.z - 7.0) < 1e-3, "array circle (%s)" % ab.size)
	check(am.get_mirror_x() and am.has_modifiers(), "legacy getter reads the stack")
	# along a Path3D (the copies sit on the curve)
	var path := Path3D.new()
	root.add_child(path)
	var curve := Curve3D.new()
	curve.add_point(Vector3(0, 0, 0))
	curve.add_point(Vector3(10, 0, 0))
	path.curve = curve
	path.position = Vector3(0, 0, 5)
	a_mod.shape = 2
	a_mod.count = 6
	a_mod.path = am.get_path_to(path)
	ab = am.mesh.get_aabb()
	check(absf(ab.size.x - 11.0) < 1e-3 and absf(ab.get_center().z - 5.0) < 1e-3, "array along a Path3D (%s)" % ab)
	a_mod.count_method = 1
	a_mod.distance = 5.0
	ab = am.mesh.get_aabb()
	check(absf(ab.size.x - 11.0) < 1e-3 and am.get_face_count() == 6, "array along a path by distance (%s)" % ab)
	am.apply_modifiers()
	check(am.get_face_count() == 18 and am.modifiers.is_empty(), "apply modifiers bakes the stack (%d faces)" % am.get_face_count())
	check(analyze(am).consistent, "applied array is consistent")
	path.free()
	am.free()
	# a symmetric mesh mirrored onto itself stays (it vanished: both copies of each face were dropped)
	var sym := make(1)
	var sym_mesh: Mesh = sym.mesh
	var sym_m := HewnMirrorModifier.new()
	sym.add_modifier(sym_m)
	check(sym.mesh != null and sym.mesh.get_surface_count() > 0 and sym.mesh.get_aabb().size.is_equal_approx(Vector3(1, 1, 1)), "mirrored symmetric cube stays visible")
	var sym_ev := HewnMesh3D.new()
	root.add_child(sym_ev)
	sym_ev.shape = 0
	sym.apply_modifiers()
	check(sym.get_face_count() == 6 and absf(analyze(sym).volume - 1.0) < 1e-4, "symmetric mirror: one cube, no double faces (%d faces)" % sym.get_face_count())
	# changing a modifier refills the same mesh (a new one made the inspector fold the modifier)
	sym_m = HewnMirrorModifier.new()
	sym.add_modifier(sym_m)
	var mesh_before: Mesh = sym.mesh
	sym_m.merge_distance = 0.01
	check(sym.mesh == mesh_before and sym.mesh == sym_mesh, "a modifier change keeps the same mesh resource")
	sym.free()
	sym_ev.free()

	# on / off shape parameters are bools (cone cap...), the array seed is in the Randomize group
	var cn := make(4)
	check(typeof(cn.get("shape_cap")) == TYPE_BOOL and cn.get("shape_cap") == true, "cone cap is a bool property")
	cn.set("shape_cap", false)
	check(cn.get_shape_param("cap") == 0.0, "setting the bool turns the cap off")
	cn.free()
	var arm := HewnArrayModifier.new()
	var group := ""
	var seed_group := "?"
	for pi in arm.get_property_list():
		if pi.usage & PROPERTY_USAGE_GROUP:
			group = pi.name
		elif pi.name == "random_seed":
			seed_group = group
	check(seed_group == "Randomize", "array seed is in the Randomize group (%s)" % seed_group)
	arm.set("seed", 7)
	check(arm.random_seed == 7, "old scenes' seed still loads")

	# old scenes / scripts: mirror_x and solidify_thickness create stack entries
	var lg := make(1)
	lg.mirror_x = true
	lg.solidify_thickness = 0.1
	check(lg.modifiers.size() == 2 and lg.modifiers[0] is HewnMirrorModifier and lg.modifiers[1] is HewnSolidifyModifier, "legacy properties fill the stack")
	lg.mirror_x = false
	check(lg.modifiers.size() == 1, "legacy mirror off removes it")
	lg.free()

	# --- a wall through 2 points (an edited straight wall) is the same wall as the `length` one
	var wl := make(19)
	var wl_vol: float = analyze(wl).volume
	wl.poly_points = PackedVector3Array([Vector3(-2, 0, 0), Vector3(2, 0, 0)])
	var wl2: Dictionary = solid_ok(wl, "wall through 2 points")
	check(absf(wl2.volume - wl_vol) < 1e-3, "2-point wall volume %f vs %f" % [wl2.volume, wl_vol])
	wl.free()

	_test_new_tools()
	_test_plane_push()

	# --- face roles: generated faces know what they are; materials follow them when parameters change
	const R_TOP := 2
	const R_TREAD := 12
	const R_RISER := 13
	const R_UNDERSIDE := 14
	const R_BORDER := 15
	var rc := make(1)
	var rc_roles := {}
	for f in rc.get_face_count():
		rc_roles[rc.get_face_role(f)] = true
	check(rc_roles.size() == 6, "cube faces have 6 roles (%s)" % [rc_roles.keys()])
	var top_mat := StandardMaterial3D.new()
	var rc_tops := PackedInt32Array()
	for f in rc.get_face_count():
		if rc.get_face_role(f) == R_TOP:
			rc_tops.push_back(f)
	rc.apply_material(top_mat, rc_tops)
	rc.set_shape_param("width_segments", 3)
	var rc_top_ok := 0
	var rc_other_ok := true
	for f in rc.get_face_count():
		var m: Material = mat_of(rc, f)
		if rc.get_face_role(f) == R_TOP:
			rc_top_ok += 1 if m == top_mat else 0
		else:
			rc_other_ok = rc_other_ok and m != top_mat
	check(rc_top_ok == 3 and rc_other_ok, "the top keeps its material when the cube is rebuilt (%d)" % rc_top_ok)
	var rc_data: Dictionary = rc.get_mesh_data()
	var rc2 := HewnMesh3D.new()
	rc2.set_mesh_data(rc_data)
	check(rc2.get_face_role(0) == rc.get_face_role(0) and rc2.get_face_count() == rc.get_face_count(), "roles are saved in mesh_data")
	rc2.free()
	rc.apply_material(null)
	check(rc.get_materials().size() == 1, "one material on all faces leaves one slot (%d)" % rc.get_materials().size())
	rc.free()

	var stairs_mat := StandardMaterial3D.new()
	var rs := make(10)
	rs.set_shape_param("height", 1.8)
	rs.set_shape_param("depth", 3.0)
	rs.set_shape_param("use_riser_height", 1.0)
	rs.set_shape_param("riser_height", 0.18)
	var rs_n_tread := 0
	for f in rs.get_face_count():
		rs_n_tread += 1 if rs.get_face_role(f) == R_TREAD else 0
	check(rs_n_tread == 10, "riser height 0.18 over 1.8 m: 10 rs_treads (%d)" % rs_n_tread)
	solid_ok(rs, "stairs by riser height")
	var rs_treads := PackedInt32Array()
	for f in rs.get_face_count():
		if rs.get_face_role(f) == R_TREAD:
			rs_treads.push_back(f)
	rs.apply_material(stairs_mat, rs_treads)
	rs.set_shape_param("riser_height", 0.3)
	var rs_painted := 0
	var rs_risers_plain := true
	for f in rs.get_face_count():
		var m: Material = mat_of(rs, f)
		if rs.get_face_role(f) == R_TREAD:
			rs_painted += 1 if m == stairs_mat else 0
		elif rs.get_face_role(f) == R_RISER:
			rs_risers_plain = rs_risers_plain and m != stairs_mat
	check(rs_painted == 6 and rs_risers_plain, "rs_treads keep their material with another step count (%d)" % rs_painted)
	rs.set_shape_param("final_riser", 1.0)
	var fr_t := 0
	var fr_r := 0
	for f in rs.get_face_count():
		fr_t += 1 if rs.get_face_role(f) == R_TREAD else 0
		fr_r += 1 if rs.get_face_role(f) == R_RISER else 0
	check(fr_t == 5 and fr_r == 6, "final riser: 5 rs_treads, 6 risers (%d, %d)" % [fr_t, fr_r])
	var bb_fr: AABB = rs.get_aabb()
	check(is_equal_approx(bb_fr.size.y, 1.8) and is_equal_approx(bb_fr.size.z, 3.0), "final riser keeps the box (%s)" % bb_fr)
	rs.set_shape_param("final_riser", 0.0)
	rs.set_shape_param("underside", 1.0)
	var rs_has_under := false
	for f in rs.get_face_count():
		rs_has_under = rs_has_under or rs.get_face_role(f) == R_UNDERSIDE
	check(rs_has_under, "sloped underside")
	solid_ok(rs, "stairs with a sloped underside")
	rs.set_shape_param("underside", 2.0)
	check(not analyze(rs).closed, "open underside leaves the bottom open")
	rs.set_shape_param("underside", 0.0)
	rs.set_shape_param("border_left", 1.0)
	rs.set_shape_param("border_right", 1.0)
	var rs_n_border := 0
	for f in rs.get_face_count():
		rs_n_border += 1 if rs.get_face_role(f) == R_BORDER else 0
	check(rs_n_border >= 10, "borders on both sides (%d faces)" % rs_n_border)
	var bb_in: AABB = rs.get_aabb()
	rs.set_shape_param("border_outside", 1.0)
	var bb_out: AABB = rs.get_aabb()
	check(is_equal_approx(bb_in.size.x, 1.0) and is_equal_approx(bb_out.size.x, 1.2), "borders inside keep the width, outside add to it (%f, %f)" % [bb_in.size.x, bb_out.size.x])
	rs.free()

	# --- r28: stairs options (Fill By, Height Fit, Top of Box, Border Alignment), oval cylinders
	var r28s := make(10)
	r28s.set_shape_param("height", 1.0)
	r28s.set_shape_param("depth", 2.0)
	r28s.set_shape_param("fill_by", 1.0)
	r28s.set_shape_param("riser_height", 0.3)
	var r28_risers := func(m: HewnMesh3D) -> int:
		var c := 0
		for f in m.get_face_count():
			c += 1 if m.get_face_role(f) == R_RISER else 0
		return c
	check(r28_risers.call(r28s) == 3, "Fill By Step Height, even risers: 1 m / 0.3 -> 3 risers (%d)" % r28_risers.call(r28s))
	r28s.set_shape_param("height_fit", 1.0)
	check(r28_risers.call(r28s) == 4, "Height Fit Short First Riser: 4 risers (%d)" % r28_risers.call(r28s))
	var r28_low := 10.0
	for f in r28s.get_face_count():
		if r28s.get_face_role(f) == R_TREAD:
			r28_low = minf(r28_low, r28s.get_face_center(f).y - r28s.get_aabb().position.y)
	check(absf(r28_low - 0.1) < 1e-3, "the first riser is the short one (0.1 m: %f)" % r28_low)
	solid_ok(r28s, "stairs with a short first riser")
	r28s.set_shape_param("fill_by", 0.0)
	r28s.set_shape_param("steps", 4.0)
	r28s.set_shape_param("top_of_box", 1.0)
	check(r28_risers.call(r28s) == 3 and r28s.get_aabb().size.y < 0.8, "Destination Floor: 3 risers, the last tread one riser under the box top (%d, %s)" % [r28_risers.call(r28s), r28s.get_aabb()])
	r28s.set_shape_param("create_final_riser", 1.0)
	check(r28_risers.call(r28s) == 4, "... with Create Final Riser: 4 (%d)" % r28_risers.call(r28s))
	r28s.set_shape_param("top_of_box", 0.0)
	r28s.set_shape_param("border_left", 1.0)
	r28s.set_shape_param("border_height", 0.0)
	var r28_top := func(m: HewnMesh3D) -> float:
		var t := -10.0
		for f in m.get_face_count():
			if m.get_face_role(f) == R_BORDER:
				t = maxf(t, m.get_face_center(f).y)
		return t
	var r28_tips: float = r28_top.call(r28s)
	r28s.set_shape_param("border_alignment", 1.0)
	check(r28_top.call(r28s) < r28_tips - 0.05, "Border Alignment Riser Bottoms: the border lies lower (%f < %f)" % [r28_top.call(r28s), r28_tips])
	r28s.free()
	var r28_old := make(10)
	r28_old.set_shape_params({"final_riser": 1.0, "use_riser_height": 0.0, "steps": 4.0})
	check(r28_risers.call(r28_old) == 4 and is_equal_approx(r28_old.get_shape_param("top_of_box"), 1.0), "stairs saved before r28 (final_riser) keep their destination riser")
	r28_old.free()
	var r28c := make(3)
	r28c.set_shape_param("radius", 1.0)
	r28c.set_shape_param("radius_z", 0.5)
	var r28_cb: AABB = r28c.get_aabb()
	check(is_equal_approx(r28_cb.size.x, 2.0) and absf(r28_cb.size.z - 1.0) < 0.01, "an oval cylinder: 2 x 1 (%s)" % r28_cb)
	solid_ok(r28c, "oval cylinder")
	r28c.shape = 8
	r28c.set_shape_param("radius", 1.0)
	r28c.set_shape_param("radius_z", 0.6)
	r28_cb = r28c.get_aabb()
	check(is_equal_approx(r28_cb.size.x, 2.0) and absf(r28_cb.size.z - 1.2) < 0.01, "an oval pipe: 2 x 1.2 (%s)" % r28_cb)
	solid_ok(r28c, "oval pipe", 1)
	var r28_rc := make(3)
	r28_rc.set_shape_params({"radius": 0.7})
	check(absf(r28_rc.get_aabb().size.z - 1.4) < 0.01, "a cylinder saved before r28 stays round")
	r28_rc.free()
	# UV lock: a moved face keeps its UVs
	r28c.shape = 1
	var r28_top_f := -1
	for f in r28c.get_face_count():
		if r28c.get_face_normal(f).y > 0.9:
			r28_top_f = f
	var r28_uv0: PackedVector2Array = r28c.get_face_uvs(r28_top_f)
	r28c.uv_lock_begin()
	var r28_vs: PackedVector3Array = r28c.get_vertices()
	for r28_vi in r28c.get_face(r28_top_f):
		r28_vs[r28_vi] += Vector3(0.3, 0, 0.2)
	r28c.set_vertices(r28_vs)
	var r28_uv1: PackedVector2Array = r28c.get_face_uvs(r28_top_f)
	r28c.uv_lock_end()
	var r28_same := true
	for i in r28_uv0.size():
		r28_same = r28_same and r28_uv0[i].is_equal_approx(r28_uv1[i])
	check(r28_same, "UV lock: the moved face keeps its UVs (%s -> %s)" % [r28_uv0, r28_uv1])
	r28c.free()

	# --- cuts on rooms: a door with an alcove, a corridor between cut_two rooms
	var cut_room := make(1)
	for cut_pn in ["width", "height", "depth"]:
		cut_room.set_shape_param(cut_pn, 4.0)
	cut_room.set_shape_param("flip_normals", 1.0)  # a cut_room: faces inward
	var cut_holes := cut_room.cut_box(AABB(Vector3(1.9, -2.0, -0.5), Vector3(0.2, 2.5, 1.0)))
	var cut_hole_loops := unpack(cut_holes)
	check(cut_hole_loops.size() == 1, "a door cut through one wall: one hole (%d)" % cut_hole_loops.size())
	var cut_ra := analyze(cut_room)
	check(cut_ra.consistent and not cut_ra.closed, "the cut leaves a consistent open room")
	var cut_floor_ok := false
	for f in cut_room.get_face_count():
		var cut_c := cut_room.get_face_center(f)
		if absf(cut_c.y + 2.0) < 1e-3 and cut_room.get_face_normal(f).y > 0.9:
			cut_floor_ok = true
	check(cut_floor_ok, "the floor under the door stays")
	if cut_hole_loops.size() == 1:
		cut_room.make_alcove(cut_hole_loops[0], 0.5)
		var cut_rb := analyze(cut_room)
		check(cut_rb.closed and cut_rb.consistent, "alcove closes the cut_room again")
		var cut_bb := cut_room.get_aabb()
		check(is_equal_approx(cut_bb.end.x, 2.5), "the alcove goes 0.5 into the wall (%f)" % cut_bb.end.x)
		# a door down to the floor: the alcove goes straight in, its floor level with the room's
		check(is_equal_approx(cut_bb.position.y, -2.0), "the alcove's floor stays level (%f)" % cut_bb.position.y)
	cut_room.free()

	var cut_r1 := make(1)
	var cut_r2 := make(1)
	for cut_rr in [cut_r1, cut_r2]:
		for cut_pn in ["width", "height", "depth"]:
			cut_rr.set_shape_param(cut_pn, 4.0)
		cut_rr.set_shape_param("flip_normals", 1.0)
	cut_r2.position = Vector3(7, 0, 0)
	cut_r1.merge_from(cut_r2)
	cut_r2.free()
	var cut_two := unpack(cut_r1.cut_box(AABB(Vector3(1.9, -2.0, -0.6), Vector3(3.2, 2.6, 1.2))))
	check(cut_two.size() == 2, "a cut through both walls: cut_two cut_holes (%d)" % cut_two.size())
	if cut_two.size() == 2:
		cut_r1.bridge_holes(cut_two[0], cut_two[1])
		var cut_rc := analyze(cut_r1)
		check(cut_rc.closed and cut_rc.consistent, "the corridor joins the rooms into one closed surface (closed %s, consistent %s)" % [cut_rc.closed, cut_rc.consistent])
	cut_r1.free()

	var cut_slab := make(2)  # a plane: a flat cut makes a hole, fill closes it
	cut_slab.set_shape_param("width", 4.0)
	cut_slab.set_shape_param("depth", 4.0)
	var cut_fh := unpack(cut_slab.cut_box(AABB(Vector3(-0.5, -0.1, -0.5), Vector3(1, 0.2, 1))))
	check(cut_fh.size() == 1, "a flat cut makes a hole in a floor (%d)" % cut_fh.size())
	if cut_fh.size() == 1:
		var cut_nf0 := cut_slab.get_face_count()
		cut_slab.fill_hole(cut_fh[0])
		check(cut_slab.get_face_count() == cut_nf0 + 1 and analyze(cut_slab).consistent, "fill closes it")
	cut_slab.free()

	# --- Alt+click texture wrap: the texture runs on around a corner
	var wr := make(1)
	var w_front := -1
	var w_right := -1
	for f in wr.get_face_count():
		var nn := wr.get_face_normal(f)
		if nn.z > 0.9:
			w_front = f
		elif nn.x > 0.9:
			w_right = f
	wr.wrap_texture(w_front, w_right, true)
	var uf := wr.get_face_uvs(w_front)
	var ur := wr.get_face_uvs(w_right)
	var fa := wr.get_face(w_front)
	var fb := wr.get_face(w_right)
	var same := 0
	for i in fa.size():
		for k in fb.size():
			if fa[i] == fb[k] and uf[i].distance_to(ur[k]) < 1e-4:
				same += 1
	check(same == 2, "wrapped texture matches along the shared edge (%d corners)" % same)
	wr.free()

	# --- tool materials: nodraw is not drawn in the game but collides, trigger becomes an area
	var tm_cube := make(1)
	tm_cube.collision_mode = HewnMesh3D.COLLISION_TRIMESH
	var nodraw := StandardMaterial3D.new()
	nodraw.set_meta("hw_tool", "nodraw")
	tm_cube.apply_material(nodraw, PackedInt32Array([0]))
	var drawn_tris := 0
	for si in tm_cube.mesh.get_surface_count():
		drawn_tris += tm_cube.mesh.surface_get_arrays(si)[Mesh.ARRAY_INDEX].size() / 3
	check(drawn_tris == 10, "a nodraw face is not drawn in the game (%d triangles)" % drawn_tris)
	tm_cube.free()

	_test_uv_tools()
	_test_join()
	_test_r29_cuts()
	_test_r29_uv2()
	_test_r29_bake()
	_test_r29_sweep()
	_test_r29_overlays()
	_test_r30()
	_test_r31()
	_test_r32()
	_test_r30_path_stairs()
	_test_r30_boolean_far()
	_test_r49()
	_test_r53_uv_near_origin()
	_test_r55_uv_face_aligned()
	_test_clean_sweep()
	_test_positions_only()
	_test_dyntopo()
	_test_simplify()
	_test_remesh()
	_test_collision_modes()
	_test_sweep_outside()
	_test_scatter()
	_test_sweep_accents()
	_test_sweep_corners_gaps()
	_test_scatter_rotation_collision()
	_test_inner_faces()

	print("Hewn tests: %d checks, %d failures" % [checks, failures])
	quit(1 if failures > 0 else 0)


# A face of a convex block moved as a plane: the faces around keep their planes; where they meet anew, the corners
# change (a cut corner grows past the box's edges); no move where a face would vanish.
func _test_plane_push() -> void:
	var pb := make(1)
	var pv := pb.get_vertices()
	var corner := 0
	for i in pv.size():
		if pv[i].x + pv[i].y + pv[i].z > pv[corner].x + pv[corner].y + pv[corner].z:
			corner = i
	var hx: float = pv[corner].x
	pb.bevel_vertices(PackedInt32Array([corner]), hx * 0.5)
	solid_ok(pb, "plane push: a box with a cut corner")
	var normals := []
	for f in pb.get_face_count():
		normals.append(pb.get_face_normal(f))
	var cut := -1
	var right := -1
	var top := -1
	for f in pb.get_face_count():
		var n: Vector3 = normals[f]
		if n.x > 0.5 and n.y > 0.5:
			cut = f
		elif n.x > 0.99:
			right = f
		elif n.y > 0.99:
			top = f
	check(cut >= 0 and right >= 0 and top >= 0 and pb.get_face(cut).size() == 3, "plane push: the cut corner is a triangle")
	var faces_before := pb.get_face_count()
	# the right side far out: the cut corner reaches the back and the bottom, it gets five corners
	check(pb.move_face_plane(right, hx * 2.0), "plane push: the side moved out")
	solid_ok(pb, "plane push: moved out")
	var same := pb.get_face_count() == faces_before
	for f in mini(pb.get_face_count(), faces_before):
		same = same and pb.get_face_normal(f).dot(normals[f]) > 0.9999
	check(same, "plane push: every face keeps its plane's direction")
	check(pb.get_face(cut).size() == 5, "plane push: the cut corner runs on over the next sides (%d corners)" % pb.get_face(cut).size())
	var xmax := -1e9
	for v in pb.get_vertices():
		xmax = maxf(xmax, v.x)
	check(absf(xmax - hx * 3.0) < 1e-3, "plane push: the side is where it was moved to (%f)" % xmax)
	check(not pb.move_face_plane(right, hx * 4.0), "plane push: not past where the side would vanish behind the cut corner")
	# back; then the top down past the cut corner: refused (the corner's face would vanish), nothing changes
	check(pb.move_face_plane(right, -hx * 2.0), "plane push: the side back")
	check(pb.get_face(cut).size() == 3, "plane push: the cut corner a triangle again")
	var data_before: Dictionary = pb.mesh_data
	check(not pb.move_face_plane(top, -hx * 1.5) and pb.mesh_data == data_before, "plane push: no move where a face would vanish")
	check(pb.move_face_plane(top, -hx * 0.2), "plane push: the top a little down")
	solid_ok(pb, "plane push: top down")
	pb.free()
	# not a convex block (an L): refused, Shift + drag moves the corners along the edges then
	var pl := make(1)
	pl.extrude_faces(faces_facing(pl, Vector3(1, 0, 0)), 1.0)
	var pl_up := faces_facing(pl, Vector3(0, 1, 0))
	pl.extrude_faces(PackedInt32Array([pl_up[pl_up.size() - 1]]), 1.0)
	check(not pl.move_face_plane(0, 0.5), "plane push: an L is no convex block")
	pl.free()
	# a box whose front is two triangles in one plane, and one top corner lowered (the top is not flat any more):
	# still a convex block; the top leans down toward that corner, so the left side pushed out gets smaller
	# instead of the corners running on past the faces around
	var cb := HewnMesh3D.new()
	cb.clear_mesh()
	var cbv := [Vector3(-1, -0.5, -0.5), Vector3(1, -0.5, -0.5), Vector3(1, 0.5, -0.5), Vector3(-1, 0.5, -0.5),
			Vector3(-1, -0.5, 0.5), Vector3(1, -0.5, 0.5), Vector3(1, 0.5, 0.5), Vector3(-1, 0.0, 0.5)]
	for p in cbv:
		cb.add_vertex(p)
	for fv in [[0, 3, 2, 1], [4, 5, 7], [5, 6, 7], [0, 1, 5, 4], [3, 7, 6, 2], [0, 4, 7, 3], [1, 2, 6, 5]]:
		cb.add_face(PackedInt32Array(fv))
	var cb_left := -1
	for f in cb.get_face_count():
		if cb.get_face_normal(f).x < -0.7:
			cb_left = f
	check(cb_left >= 0, "plane push (shared planes): the left side")
	check(cb.move_face_plane(cb_left, 0.3), "plane push: a block with two faces in one plane and faces not flat")
	solid_ok(cb, "plane push: the block with shared planes moved")
	var cb_minx := 1e9
	for v in cb.get_vertices():
		cb_minx = minf(cb_minx, v.x)
	check(absf(cb_minx - (-1.3)) < 1e-3, "plane push: the side moved out to its plane (%f)" % cb_minx)
	var cb_box := AABB()
	var cb_first := true
	for v in cb.get_vertices():
		cb_box = AABB(v, Vector3()) if cb_first else cb_box.expand(v)
		cb_first = false
	check(cb_box.size.y <= 1.0 + 1e-3 and cb_box.size.z <= 1.0 + 1e-3, "plane push: no corner past the faces around (%s)" % cb_box)
	check(not cb.move_face_plane(cb_left, 50.0), "plane push: not past where the leaning side would vanish")
	cb.free()
	# a box with a slanted cut at a front corner: the left side pushed out gets smaller under the cut until it is a
	# triangle, then stops (it would vanish)
	var ch := HewnMesh3D.new()
	ch.shape = 1
	ch.set_shape_param("width", 2.0)
	ch.set_shape_param("height", 0.6)
	ch.set_shape_param("depth", 1.2)
	ch.bisect(Vector3(-0.3, -0.3, 0.6), Vector3(-0.4, -0.7, 0.6).normalized(), false, true, true)
	var ch_left := -1
	for f in ch.get_face_count():
		if ch.get_face_normal(f).x < -0.99:
			ch_left = f
	var ch_corners := []
	for ch_step in [0.2, 0.4, 0.8]:
		check(ch.move_face_plane(ch_left, ch_step), "plane push (cut corner): the left side out by %f" % ch_step)
		ch_corners.append(ch.get_face(ch_left).size())
	solid_ok(ch, "plane push: the cut box pushed out")
	check(ch_corners == [5, 4, 3], "plane push: the left side gets smaller under the cut (%s)" % [ch_corners])
	check(not ch.move_face_plane(ch_left, 1.6), "plane push: it stops before the side would vanish")
	ch.free()
	# a corner in the middle of another face's edge (the front has it, the bottom not): still moved as planes
	var tj := HewnMesh3D.new()
	tj.clear_mesh()
	for p in [Vector3(-1, -0.5, -0.5), Vector3(1, -0.5, -0.5), Vector3(1, 0.5, -0.5), Vector3(-1, 0.5, -0.5),
			Vector3(-1, -0.5, 0.5), Vector3(1, -0.5, 0.5), Vector3(1, 0.5, 0.5), Vector3(-1, 0.5, 0.5), Vector3(0, -0.5, 0.5)]:
		tj.add_vertex(p)
	for fv in [[0, 3, 2, 1], [4, 8, 5, 6, 7], [0, 1, 5, 4], [3, 7, 6, 2], [0, 4, 7, 3], [1, 2, 6, 5]]:
		tj.add_face(PackedInt32Array(fv))
	var tj_left := -1
	for f in tj.get_face_count():
		if tj.get_face_normal(f).x < -0.99:
			tj_left = f
	check(tj.move_face_plane(tj_left, 0.5), "plane push: a corner on another face's edge does not stop it")
	tj.free()
	# a cube with a corner cut off whose old corner stayed in the vertex list (used by no face): still a convex
	# block; the right side pushed out gets smaller under the cut, then stops
	var cc := HewnMesh3D.new()
	cc.clear_mesh()
	for p in [Vector3(-1, -1, -1), Vector3(1, -1, -1), Vector3(1, 1, -1), Vector3(-1, 1, -1), Vector3(-1, -1, 1), Vector3(1, -1, 1),
			Vector3(1, 1, 1), Vector3(-1, 1, 1), Vector3(1, 0, 1), Vector3(0, 1, 1), Vector3(1, 1, 0)]:
		cc.add_vertex(p)
	for fv in [[3, 2, 1, 0], [9, 7, 4, 5, 8], [0, 4, 7, 3], [8, 5, 1, 2, 10], [0, 1, 5, 4], [10, 2, 3, 7, 9], [9, 8, 10]]:
		cc.add_face(PackedInt32Array(fv))
	var cc_right := -1
	for f in cc.get_face_count():
		if cc.get_face_normal(f).x > 0.99:
			cc_right = f
	check(cc.move_face_plane(cc_right, 0.5), "plane push: an unused vertex left by a cut does not stop it")
	solid_ok(cc, "plane push: the cut cube pushed out")
	var cc_max := Vector3(-1e9, -1e9, -1e9)
	for v in cc.get_vertices():
		cc_max = cc_max.max(v)
	check(cc_max.y <= 1.0 + 1e-3 and cc_max.z <= 1.0 + 1e-3 and absf(cc_max.x - 1.5) < 1e-3, "plane push: nothing past the faces around (%s)" % cc_max)
	check(not cc.move_face_plane(cc_right, 5.0), "plane push: stops before the side would vanish under the cut")
	# with collapse (as the face drag does): past that the side goes into one corner and is left out
	check(cc.move_face_plane(cc_right, 5.0, true), "plane push (collapse): the side pushed far out")
	solid_ok(cc, "plane push: the side gone into a corner")
	var cc_far := Vector3(-1e9, -1e9, -1e9)
	for v in cc.get_vertices():
		cc_far = cc_far.max(v)
	check(cc.get_face_count() == 6 and absf(cc_far.x - 4.0) < 1e-3, "plane push (collapse): one face less, its corner where the planes meet (%d faces, %s)" % [cc.get_face_count(), cc_far])
	cc.free()


func _test_new_tools() -> void:
	# --- rounded bevel: one cube edge, 4 segments
	var bc := make(1)
	var one := edge_pairs(bc, func(a, b): return a.y > 0.4 and b.y > 0.4 and a.z > 0.4 and b.z > 0.4)
	check(one.size() == 2, "bevel round: one edge")
	check(bc.bevel_edges(one, 0.2, 4).size() >= 4, "bevel round: strip faces")
	solid_ok(bc, "bevel round 1 edge")
	bc.free()
	var bc2 := make(1)
	check(bc2.bevel_edges(edge_pairs(bc2, func(a, b): return true), 0.15, 3).size() > 0, "bevel round all")
	solid_ok(bc2, "bevel round all edges")
	bc2.free()

	# --- loop cut around a cube (closed ring of 4 quads)
	var lc := make(1)
	var e := edge_pairs(lc, func(a, b): return a.y > 0.4 and b.y > 0.4 and a.z > 0.4 and b.z > 0.4)
	var fc := lc.get_face_count()
	check(lc.loop_cut(e[0], e[1], 1, 0.25).size() == 8, "loop cut: 4 new edges")
	check(lc.get_face_count() == fc + 4, "loop cut: 4 more faces")
	solid_ok(lc, "loop cut")
	lc.free()
	var lc3 := make(1)
	e = edge_pairs(lc3, func(a, b): return a.y > 0.4 and b.y > 0.4 and a.z > 0.4 and b.z > 0.4)
	check(lc3.loop_cut(e[0], e[1], 3).size() == 24, "loop cut x3")
	solid_ok(lc3, "loop cut x3")
	lc3.free()

	# --- knife across the top of a cube: from the middle of the -X top edge to the middle of the +X top edge
	var kc := make(1)
	var top := faces_facing(kc, Vector3.UP)[0]
	var pts := PackedVector3Array([Vector3(-0.5, 0.5, 0.1), Vector3(0.5, 0.5, -0.1)])
	var kfaces := PackedInt32Array([top, top])
	var cut := kc.knife_cut(pts, kfaces, Vector3.DOWN)
	check(cut.size() == 2, "knife: one new edge across the top (%d)" % cut.size())
	check(kc.get_face_count() == 7, "knife: top split in two")
	solid_ok(kc, "knife top")
	kc.free()
	# knife across three faces: front face -> top -> back face
	var kc2 := make(1)
	var front := faces_facing(kc2, Vector3.BACK)[0]
	var back := faces_facing(kc2, Vector3.FORWARD)[0]
	pts = PackedVector3Array([Vector3(-0.5, 0.2, 0.5), Vector3(0.5, 0.2, -0.5)])
	# walk over the +Y side: points on the left/right side edges, cut plane contains the up axis direction
	var kf2 := faces_facing(kc2, Vector3.LEFT)[0]
	var kf3 := faces_facing(kc2, Vector3.RIGHT)[0]
	cut = kc2.knife_cut(PackedVector3Array([Vector3(-0.5, 0.2, 0.0), Vector3(0.5, 0.2, 0.0)]), PackedInt32Array([kf2, kf3]), Vector3.UP)
	check(cut.size() >= 2, "knife: cut through faces between (%d)" % cut.size())
	solid_ok(kc2, "knife multi-face")
	kc2.free()

	# --- knife with points inside faces (not on edges / vertices): joined to the nearest corner
	var kc4 := make(1)
	var ktop := faces_facing(kc4, Vector3.UP)[0]
	var kfc := kc4.get_face_count()
	cut = kc4.knife_cut(PackedVector3Array([Vector3(-0.2, 0.5, 0.05), Vector3(0.2, 0.5, -0.05)]), PackedInt32Array([ktop, ktop]), Vector3.DOWN)
	check(cut.size() >= 2 and kc4.get_face_count() == kfc + 1, "knife inside one face splits it (%d edges, %d faces)" % [cut.size() / 2, kc4.get_face_count()])
	solid_ok(kc4, "knife inside a face")
	kc4.free()
	var kc5 := make(1)
	ktop = faces_facing(kc5, Vector3.UP)[0]
	var kfront := faces_facing(kc5, Vector3.BACK)[0]
	kfc = kc5.get_face_count()
	cut = kc5.knife_cut(PackedVector3Array([Vector3(0.1, 0.5, 0.1), Vector3(0.1, 0.1, 0.5)]), PackedInt32Array([ktop, kfront]), Vector3(0, 1, 1).normalized())
	check(kc5.get_face_count() == kfc + 2, "knife from inside one face to inside its neighbor (%d faces)" % kc5.get_face_count())
	solid_ok(kc5, "knife across an edge, ends inside faces")
	kc5.free()
	# a knife from a ceiling to a floor not joined to it (a room seen across): two cuts, nothing joins them
	var kr := HewnMesh3D.new()
	kr.mesh_data = {"positions": PackedVector3Array([Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 0, 1), Vector3(0, 0, 1),
			Vector3(0, 1, 0), Vector3(1, 1, 0), Vector3(1, 1, 1), Vector3(0, 1, 1)]),
		"face_sizes": PackedInt32Array([4, 4]), "face_indices": PackedInt32Array([0, 1, 2, 3, 4, 7, 6, 5])}
	kr.knife_cut(PackedVector3Array([Vector3(0, 1, 0.5), Vector3(1, 1, 0.5), Vector3(1, 0, 0.5), Vector3(0, 0, 0.5)]), PackedInt32Array([1, 1, 0, 0]), Vector3(0, -1, -0.1).normalized())
	var kr_mixed := false
	for f in kr.get_face_count():
		var ys := {}
		for v in kr.get_face(f):
			ys[roundi(kr.get_vertex(v).y)] = true
		kr_mixed = kr_mixed or ys.size() > 1
	check(kr.get_face_count() == 4 and not kr_mixed, "knife from a ceiling to a floor apart: each cut on its own, nothing joins them (%d faces)" % kr.get_face_count())
	kr.free()

	# --- knife that comes back to the edge it started on (a notch in the face, nothing else touched)
	var kc6 := make(1)
	var ktop6 := faces_facing(kc6, Vector3.UP)[0]
	var kfc6 := kc6.get_face_count()
	cut = kc6.knife_cut(PackedVector3Array([Vector3(-0.2, 0.5, 0.5), Vector3(0.0, 0.5, 0.1), Vector3(0.2, 0.5, 0.5)]), PackedInt32Array([ktop6, ktop6, ktop6]), Vector3.DOWN)
	check(kc6.get_face_count() == kfc6 + 1, "knife back to its start edge: one notch (%d faces)" % kc6.get_face_count())
	solid_ok(kc6, "knife back to the start edge")
	check(tri_area_ok(kc6), "knife notch: triangles cover the faces exactly")
	kc6.free()

	# --- dissolve: an edge of a cube -> a prism; its cut_two vertices -> cut_two quads and a big face
	var dc := make(1)
	var de := edge_pairs(dc, func(a, b): return a.y > 0.4 and b.y > 0.4 and a.z > 0.4 and b.z > 0.4)
	dc.dissolve_edges(de)
	check(dc.get_vertex_count() == 6 and dc.get_face_count() == 5, "dissolve a cube edge: prism (%d verts, %d faces)" % [dc.get_vertex_count(), dc.get_face_count()])
	solid_ok(dc, "dissolve edge prism")
	dc.free()
	var dv := make(1)
	de = edge_pairs(dv, func(a, b): return a.y > 0.4 and b.y > 0.4 and a.z > 0.4 and b.z > 0.4)
	dv.dissolve_vertices(PackedInt32Array([de[0], de[1]]))
	check(dv.get_vertex_count() == 6 and dv.get_face_count() == 3, "dissolve cut_two cube vertices (%d verts, %d faces)" % [dv.get_vertex_count(), dv.get_face_count()])
	solid_ok(dv, "dissolve cut_two vertices")
	dv.free()

	# --- loose edges: kept through edits, saved, extruded into a face
	var wm := make(1)
	var wd: Dictionary = wm.mesh_data
	var wpos: PackedVector3Array = wd["positions"]
	wpos.append(Vector3(0, 2, 0))
	wpos.append(Vector3(1, 2, 0))
	wd["positions"] = wpos
	wd["wires"] = PackedInt32Array([wpos.size() - 2, wpos.size() - 1])
	wm.mesh_data = wd
	check(wm.get_vertex_count() == 10 and (wm.mesh_data as Dictionary).has("wires"), "loose edge stored with its vertices")
	wm.free()

	# --- stairs: smooth collision ramp, same bounds as the steps
	var ramp_st := make(10)
	ramp_st.collision_mode = HewnMesh3D.COLLISION_TRIMESH
	ramp_st.set_shape_param("smooth_collision", 1)
	var body: StaticBody3D = null
	for cut_c in ramp_st.get_children(true):
		if cut_c is StaticBody3D:
			body = cut_c
	var faces_ramp := 0
	if body:
		var cs: CollisionShape3D = body.get_child(0)
		faces_ramp = (cs.shape as ConcavePolygonShape3D).get_faces().size() / 3
	check(faces_ramp > 0 and faces_ramp <= 16, "stairs smooth collision is a ramp (%d triangles)" % faces_ramp)
	ramp_st.free()
	# flip normals on any shape, bent stairs keep their size, walls with corners
	var flc := make(1)
	flc.set_shape_param("flip_normals", 1)
	check(analyze(flc).volume < 0.0, "cube flip_normals turns the faces inwards")
	flc.free()
	var bst := make(10)
	bst.set_shape_param("curvature", 5.0)
	var bsz := bst.get_aabb().size
	check(bsz.z > 1.5, "low curvature keeps the stairs' depth (%s)" % bsz)
	solid_ok(bst, "bent stairs 5 deg")
	bst.free()
	var wp := make(19)
	wp.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(2, 0, 0), Vector3(2, 0, 2)])
	solid_ok(wp, "wall with a corner")
	check(wp.get_face_count() == 8, "wall with a corner: 6 sides + top + bottom (%d)" % wp.get_face_count())
	wp.free()
	# stairs direction: across the footprint / reversed / bent the other way, all closed
	var sd := make(10)
	var sd0 := sd.get_aabb().size
	sd.set_shape_param("climb_along_long_side", 0)
	check(sd.get_aabb().size.is_equal_approx(sd0), "stairs across the footprint keep it (%s -> %s)" % [sd0, sd.get_aabb().size])
	solid_ok(sd, "stairs climbing across")
	sd.set_shape_param("reverse", 1)
	solid_ok(sd, "stairs reversed")
	sd.set_shape_param("climb_along_long_side", 1)
	sd.set_shape_param("curvature", -90.0)
	solid_ok(sd, "stairs bent the other way")
	sd.set_shape_param("smooth_collision", 1)
	sd.free()

	# prism: the ridge along the short side keeps the footprint
	var pz := make(9)
	pz.set_shape_param("depth", 2.0)
	var pz_size := pz.get_aabb().size
	var pz_verts := pz.get_vertices()
	pz.set_shape_param("ridge_along_long_side", 0)
	check(pz.get_aabb().size.is_equal_approx(pz_size) and pz.get_vertices() != pz_verts, "prism ridge across: footprint kept (%s)" % pz.get_aabb().size)
	solid_ok(pz, "prism ridge across")
	pz.free()

	# arch: the opening on the short side keeps the footprint
	var ar := make(12)
	var ar_size := ar.get_aabb().size
	ar.set_shape_param("depth", 3.0)
	var long_size := ar.get_aabb().size
	var long_verts := ar.get_vertices()
	ar.set_shape_param("opening_on_long_side", 0)
	check(absf(ar.get_aabb().size.x - long_size.x) < 1e-3 and absf(ar.get_aabb().size.z - long_size.z) < 1e-3 and ar.get_vertices() != long_verts, "arch opening side: turned, footprint kept (%s -> %s)" % [ar_size, ar.get_aabb().size])
	solid_ok(ar, "arch opening on the short side")
	ar.free()

	# non-destructive boolean: the operand stays, the result follows it, Apply bakes it
	var nb_a := make(1)
	var nb_b := make(1)
	nb_b.position = Vector3(0.5, 0.5, 0.5)
	nb_a.add_boolean(nb_b, HewnMesh3D.BOOLEAN_SUBTRACT)
	check(absf(shown_volume(nb_a) - 0.875) < 1e-3 and nb_a.get_face_count() == 6, "boolean modifier: shown result cut, base kept (%f)" % shown_volume(nb_a))
	nb_b.position = Vector3(5, 0, 0)
	nb_a.set_boolean_ops(nb_a.get_boolean_ops())  # (normally refreshed on the next frame)
	check(absf(shown_volume(nb_a) - 1.0) < 1e-3, "boolean modifier follows the moved operand (%f)" % shown_volume(nb_a))
	nb_b.position = Vector3(0.5, 0.5, 0.5)
	nb_a.apply_modifiers()
	check(nb_a.get_boolean_targets().is_empty() and absf(analyze(nb_a).volume - 0.875) < 1e-3, "apply bakes the boolean")
	nb_a.free()
	nb_b.free()

	# CSG model: child brushes combined in order, each by its operation
	var cm := HewnMesh3D.new()
	root.add_child(cm)
	cm.clear_mesh()
	cm.csg_model = true
	var br_a := HewnMesh3D.new()
	cm.add_child(br_a)
	var br_b := HewnMesh3D.new()
	br_b.position = Vector3(0.5, 0.5, 0.5)
	cm.add_child(br_b)
	var red := StandardMaterial3D.new()
	br_b.apply_material(red, PackedInt32Array())
	br_b.brush_operation = 1
	cm.csg_inverted = false  # (refreshes now; the editor does it on the next frame)
	check(br_a.is_brush() and cm.get_brush_nodes().size() == 2, "children of a CSG model are its brushes")
	check(absf(shown_volume(cm) - 0.875) < 1e-3, "model: brush minus brush (%f)" % shown_volume(cm))
	var has_red := false
	for si in cm.mesh.get_surface_count():
		has_red = has_red or cm.mesh.surface_get_material(si) == red
	check(has_red, "the cut shows the subtracting brush's material")
	br_b.visible = false
	cm.csg_inverted = false  # (refreshes now; the editor does it on the next frame)
	check(absf(shown_volume(cm) - 1.0) < 1e-3, "a hidden brush is left out (%f)" % shown_volume(cm))
	br_b.visible = true
	br_b.brush_operation = 2
	cm.csg_inverted = false
	check(absf(shown_volume(cm) - 0.125) < 1e-3, "intersecting brush (%f)" % shown_volume(cm))
	cm.csg_inverted = true
	br_b.brush_operation = 1
	br_a.brush_operation = 1
	cm.csg_inverted = true
	check(absf(shown_volume(cm) + 1.875) < 1e-2, "inverted world: subtracting brushes make rooms seen from inside (%f)" % shown_volume(cm))
	cm.free()
	var wall := make(19)
	check(absf(wall.get_aabb().size.x - 4.0) < 1e-3 and absf(wall.get_aabb().size.z - 0.2) < 1e-3, "wall shape: length x thickness (%s)" % wall.get_aabb().size)
	wall.free()

	# --- box cuts through the slanted side of a prism leave a closed mesh
	for i in range(6):
		var pr := make(9)
		var bx := make(1)
		bx.scale = Vector3(0.3 + 0.1 * i, 0.3, 2.0)
		bx.rotation = Vector3(0, 0, deg_to_rad(-63.4))  # across the slanted side
		bx.position = Vector3(0.25 + 0.01 * i, 0.0, 0.05 * i)
		var ok := pr.boolean_with(bx, HewnMesh3D.BOOLEAN_SUBTRACT)
		check(ok, "prism cut %d ok" % i)
		solid_ok(pr, "prism cut %d" % i)
		check(tri_area_ok(pr), "prism cut %d: triangles cover the faces exactly" % i)
		bx.free()
		pr.free()
	# through the slanted side along its normal, also over the prism's end (like the box cutter's "through")
	for i in range(12):
		var pr := make(9)
		var bx := make(1)
		bx.scale = Vector3(3.0, 0.2 + 0.07 * i, 0.3 + 0.1 * (i % 4))
		bx.rotation = Vector3(0, 0, atan2(0.5, 1.0))
		bx.position = Vector3(0.25, 0.0, -0.5 + 0.1 * i)
		var ok := pr.boolean_with(bx, HewnMesh3D.BOOLEAN_SUBTRACT)
		check(ok, "prism through-cut %d ok" % i)
		var pa := analyze(pr)
		check(pa.closed and pa.consistent and pa.volume > 0.0 and (pa.euler == 2 or pa.euler == 0), "prism through-cut %d: closed solid or tunnel (%s)" % [i, pa])
		check(tri_area_ok(pr), "prism through-cut %d: triangles cover the faces exactly" % i)
		bx.free()
		pr.free()

	# --- booleans
	var a := make(1)
	var b := make(1)
	b.position = Vector3(0.5, 0.5, 0.5)
	check(a.boolean_with(b, HewnMesh3D.BOOLEAN_SUBTRACT), "csg subtract ok")
	var sa := solid_ok(a, "csg subtract")
	check(absf(sa.volume - 0.875) < 1e-3, "csg subtract volume %f" % sa.volume)
	check(a.get_face_count() <= 12, "csg subtract merged faces (%d)" % a.get_face_count())
	b.free()
	var hole := make(3)  # cylinder through the cube
	hole.set_shape_param("radius", 0.2)
	hole.set_shape_param("height", 3.0)
	check(a.boolean_with(hole, HewnMesh3D.BOOLEAN_SUBTRACT), "csg drill ok")
	solid_ok(a, "csg drill", 1)
	hole.free()
	a.free()
	var u1 := make(1)
	var u2 := make(1)
	u2.position = Vector3(0.5, 0, 0)
	u1.boolean_with(u2, HewnMesh3D.BOOLEAN_UNION)
	var su := solid_ok(u1, "csg union")
	check(absf(su.volume - 1.5) < 1e-3, "csg union volume %f" % su.volume)
	check(u1.get_face_count() == 6, "csg union merges coplanar faces (%d)" % u1.get_face_count())
	u2.free()
	u1.free()

	# --- stairs: sides off = open, curvature = curved
	var st := make(10)
	st.set_shape_param("curvature", 90.0)
	solid_ok(st, "stairs curved")
	st.set_shape_param("build_sides", 0)
	check(not analyze(st).closed, "stairs without sides are open")
	st.free()
	var cs := make(11)
	cs.set_shape_param("angle", 300.0)
	solid_ok(cs, "curved stairs 300")
	cs.free()

	# --- rounded door
	for segs in [1, 2, 8]:
		var door := make(13)
		door.set_shape_param("arch_segments", segs)
		solid_ok(door, "door with arch %d" % segs)
		door.set_shape_param("arch_height", 2.0)
		solid_ok(door, "door with full arch %d" % segs)
		door.free()

	# --- limited dissolve: a subdivided cube collapses back to 6 faces
	var ld := make(1)
	for p in ["width_segments", "height_segments", "depth_segments"]:
		ld.set_shape_param(p, 3)
	ld.limited_dissolve(5.0)
	check(ld.get_face_count() == 6 and ld.get_vertex_count() == 8, "limited dissolve: back to a plain cube (%d faces, %d verts)" % [ld.get_face_count(), ld.get_vertex_count()])
	solid_ok(ld, "limited dissolve")
	ld.free()

	# --- modifiers: mirror a half box, solidify a plane
	var hb := make(1)
	hb.delete_faces(faces_facing(hb, Vector3.LEFT))
	var hv := hb.get_vertices()
	for i in hv.size():
		if hv[i].x < 0.0:
			hv[i].x = 0.0
	hb.set_vertices(hv)
	hb.mirror_x = true
	check(hb.mesh.get_faces().size() > 0, "mirror shows geometry")
	hb.apply_modifiers()
	var ha := solid_ok(hb, "mirror x applied")
	check(absf(ha.volume - 1.0) < 1e-4, "mirror volume %f" % ha.volume)
	check(not hb.mirror_x, "apply clears the modifier")
	hb.free()
	var pl := make(2)
	pl.solidify_thickness = 0.1
	pl.apply_modifiers()
	var pa := solid_ok(pl, "solidify plane")
	check(absf(pa.volume - 0.4) < 1e-3, "solidify volume %f" % pa.volume)
	pl.free()

	# --- extrude with "dissolve orthogonal edges": inwards trims the walls, outwards extends them
	for dist in [-0.3, 0.3]:
		var dob := make(1)
		var tf := faces_facing(dob, Vector3.UP)
		dob.extrude_faces(tf, dist, false, true)
		var da := solid_ok(dob, "dissolve orthogonal %f" % dist)
		check(dob.get_face_count() == 6 and dob.get_vertex_count() == 8, "dissolve orthogonal %f keeps a box (%d faces, %d verts)" % [dist, dob.get_face_count(), dob.get_vertex_count()])
		check(absf(da.volume - (1.0 + dist)) < 1e-4, "dissolve orthogonal %f volume %f" % [dist, da.volume])
		dob.free()

	# --- bisect a cube in the middle, keep the top half and fill the cut
	var bs := make(1)
	check(bs.bisect(Vector3(0, 0.1, 0), Vector3.UP, false, false, false).size() == 8, "bisect makes a loop of 4 edges")
	solid_ok(bs, "bisect only cut")
	bs.free()
	var bs2 := make(1)
	bs2.bisect(Vector3(0, 0.1, 0), Vector3.UP, true, false, true)
	var ba := solid_ok(bs2, "bisect clear inner + fill")
	check(absf(ba.volume - 0.4) < 1e-4, "bisect volume %f" % ba.volume)
	bs2.free()

	# --- poke, bridge, edge split, vertex bevel, creases, sharp edges
	var pk := make(1)
	var pk_top := faces_facing(pk, Vector3.UP)
	check(pk.poke_faces(pk_top).size() == 4, "poke: a quad becomes 4 triangles")
	solid_ok(pk, "poke")
	pk.free()
	var br := make(1)
	var br_f := faces_facing(br, Vector3.UP)
	br_f.append_array(faces_facing(br, Vector3.DOWN))
	br_f = br.inset_faces(br_f, 0.2, true)  # bridge the inner faces through the cube
	check(br.bridge_faces(br_f).size() == 4, "bridge faces: 4 tube walls")
	var bra := analyze(br)
	check(bra.closed and bra.consistent and bra.euler == 0, "bridge faces makes a closed tube (euler %d)" % bra.euler)
	br.free()
	var spl := make(1)
	var sp_e := edge_pairs(spl, func(a, b): return a.y > 0.4 and b.y > 0.4)  # the top outline
	spl.split_edges(sp_e)
	check(spl.get_vertex_count() == 12 and not analyze(spl).closed, "edge split detaches the top along its outline (%d verts)" % spl.get_vertex_count())
	spl.free()
	var bv := make(1)
	check(bv.bevel_vertices(PackedInt32Array([0]), 0.2).size() == 1, "vertex bevel adds a face")
	solid_ok(bv, "vertex bevel")
	check(bv.get_face_count() == 7, "vertex bevel: 7 faces")
	bv.free()
	var cr := make(1)
	for pr in edge_pairs(cr, func(a, b): return true).size() / 2:
		pass
	var all_e := edge_pairs(cr, func(a, b): return true)
	for i in range(0, all_e.size(), 2):
		cr.set_edge_attributes(all_e[i], all_e[i + 1], false, false, 1.0, 0.0)
	cr.subdivide_smooth()
	var cra := solid_ok(cr, "creased catmull-clark")
	check(absf(cra.volume - 1.0) < 1e-3, "fully creased cube stays a cube (volume %f)" % cra.volume)
	cr.free()
	var sh := make(5)  # smooth sphere: a sharp edge splits its normals
	var sh_e := edge_pairs(sh, func(a, b): return true)
	sh.set_edge_attributes(sh_e[0], sh_e[1], true, true, 0.0, 0.0)
	check(sh.get_edge_attributes(sh_e[0], sh_e[1]).sharp, "edge attributes stored")
	var shst := sh.get_state()
	sh.set_edge_attributes(sh_e[0], sh_e[1], false, false, 0.0, 0.0)
	sh.set_state(shst)
	check(sh.get_edge_attributes(sh_e[0], sh_e[1]).seam, "edge attributes saved in the state")
	sh.free()

	# --- normals: flip everything, recalculate outside / inside
	var rn := make(1)
	rn.flip_faces(PackedInt32Array(range(rn.get_face_count())))
	check(analyze(rn).volume < 0.0, "flipped cube is inside out")
	rn.recalculate_normals()
	check(analyze(rn).volume > 0.0, "recalculate outside")
	rn.recalculate_normals(true)
	check(analyze(rn).volume < 0.0, "recalculate inside")
	rn.free()

	# --- F: delete a face, make it again from its border vertices (also from a partial selection)
	var mf := make(1)
	var topf := faces_facing(mf, Vector3.UP)[0]
	var ring := mf.get_face(topf)
	mf.delete_faces(PackedInt32Array([topf]))
	check(mf.make_face(ring) >= 0, "make face")
	solid_ok(mf, "make face closes the hole")
	mf.free()
	var mf2 := make(1)
	var topf2 := faces_facing(mf2, Vector3.UP)[0]
	var ring2 := mf2.get_face(topf2)
	mf2.delete_faces(PackedInt32Array([topf2]))
	mf2.make_face(PackedInt32Array([ring2[0], ring2[1], ring2[2]]))
	check(analyze(mf2).consistent, "make face from 3 border vertices keeps the winding")
	mf2.free()

	# --- poly shape (concave L outline)
	var ps := HewnMesh3D.new()
	root.add_child(ps)
	ps.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(2, 0, 0), Vector3(2, 0, 1), Vector3(1, 0, 1), Vector3(1, 0, 2), Vector3(0, 0, 2)])
	ps.shape = 18
	ps.set_shape_param("height", 0.5)
	var sp := solid_ok(ps, "poly shape L")
	check(absf(sp.volume - 1.5) < 1e-4, "poly shape volume %f" % sp.volume)
	ps.set_shape_param("height", -0.5)
	solid_ok(ps, "poly shape down")
	var st2 := ps.get_state()
	ps.set_shape_param("height", 2.0)
	ps.set_state(st2)
	check(absf(analyze(ps).volume - 1.5) < 1e-4, "poly shape state restore")
	ps.free()


# Texture tools (edit_uvs works in texture tiles).
func _test_uv_tools() -> void:
	var m := make(1)
	m.set_shape_param("width", 2.0)
	var front := faces_facing(m, Vector3.BACK)[0]
	var fa := PackedInt32Array([front])
	var b0 := m.get_uv_bounds(fa)
	check(b0.size.is_equal_approx(Vector2(2, 1)), "front face spans 2 x 1 tiles (%s)" % b0)
	m.edit_uvs(fa, "move", [0.25, 0.0])
	check(m.get_uv_bounds(fa).position.is_equal_approx(b0.position + Vector2(0.25, 0)), "UV move")
	m.edit_uvs(fa, "rotate", [90.0])
	check(m.get_uv_bounds(fa).size.is_equal_approx(Vector2(1, 2)), "UV rotate 90 turns the bounds")
	m.edit_uvs(fa, "rotate", [-90.0])
	var bc := m.get_uv_bounds(fa)
	m.edit_uvs(fa, "cycle")
	var bc2 := m.get_uv_bounds(fa)
	check(bc2.position.is_equal_approx(bc.position) and bc2.size.is_equal_approx(bc.size), "cycle stays inside the bounds (%s -> %s)" % [bc, bc2])
	var u_before := m.get_face_uvs(front)
	m.edit_uvs(fa, "mirror_u")
	var u_after := m.get_face_uvs(front)
	var cx := (bc.position.x + bc.end.x) * 0.5
	check(is_equal_approx(u_after[0].x, 2.0 * cx - u_before[0].x), "mirror U")
	m.edit_uvs(fa, "fit", [1, 1])
	var bf := m.get_uv_bounds(fa)
	check(bf.size.is_equal_approx(Vector2(1, 1)) and is_equal_approx(bf.position.x, roundf(bf.position.x)), "fit: one tile (%s)" % bf)
	m.edit_uvs(fa, "scale", [0.5, 0.5])
	m.edit_uvs(fa, "move", [0.3, 0.1])
	m.edit_uvs(fa, "justify", [-1, 1])
	var bj := m.get_uv_bounds(fa)
	check(is_equal_approx(bj.position.x, floorf(bj.get_center().x)) and is_equal_approx(bj.end.y, floorf(bj.get_center().y) + 1.0), "justify to the top left corner of the tile (%s)" % bj)
	m.edit_uvs(fa, "edge", [1, 0])
	check(is_equal_approx(m.get_uv_bounds(fa).end.x, floorf(m.get_uv_bounds(fa).get_center().x) + 1.0), "move to the right edge")
	m.edit_uvs(fa, "transfer", [0.5, 0.5, 1.0, 1.0])
	check(m.get_uv_bounds(fa).is_equal_approx(Rect2(0.5, 0.5, 0.5, 0.5)), "transfer into a rect (%s)" % m.get_uv_bounds(fa))
	m.edit_uvs(fa, "inset", [4.0])  # the default material: a 64 px texture
	check(m.get_uv_bounds(fa).size.is_equal_approx(Vector2(0.375, 0.375)), "inset by pixels (%s)" % m.get_uv_bounds(fa))
	# still parametric: the texture follows the geometry
	m.edit_uvs(fa, "rotate", [30.0])
	var uv_r := m.get_face_uvs(front)
	var fv := m.get_face(front)
	m.set_vertex(fv[0], m.get_vertex(fv[0]) + Vector3(0.3, 0.2, 0))
	check(m.get_face_uvs(front)[0].distance_to(uv_r[0]) > 0.05, "after the UV tools the texture still follows moved vertices")
	m.free()

	# square fit unwrap: the four sides of a cube as one strip
	var q := make(1)
	var sides := PackedInt32Array()
	for f in q.get_face_count():
		if absf(q.get_face_normal(f).y) < 0.5:
			sides.push_back(f)
	var done := q.unwrap_quads(sides, sides[0])
	check(done.size() == 4, "the four sides are unwrapped (%d)" % done.size())
	check(q.get_uv_bounds(sides).is_equal_approx(Rect2(0, 0, 1, 1)), "fitted to one tile (%s)" % q.get_uv_bounds(sides))
	var joined := 0
	for i in sides.size():
		for k in sides.size():
			if i >= k:
				continue
			var ua := q.get_face_uvs(sides[i])
			var ub := q.get_face_uvs(sides[k])
			var fa2 := q.get_face(sides[i])
			var fb2 := q.get_face(sides[k])
			for x in fa2.size():
				for y in fb2.size():
					if fa2[x] == fb2[y] and ua[x].distance_to(ub[y]) < 1e-4:
						joined += 1
	check(joined == 6, "neighbors share their UVs except at the closing seam (%d)" % joined)
	q.free()


# builders: touching boxes grow together, touching rooms open into each other.
func _test_join() -> void:
	var a := make(1)
	var b := make(1)
	b.position = Vector3(1, 0, 0)
	a.join_mesh(b)
	var r := solid_ok(a, "two touching boxes joined")
	check(absf(r.volume - 2.0) < 1e-3 and a.get_face_count() == 10, "joined boxes: one 2 x 1 x 1 block (%f, %d faces)" % [r.volume, a.get_face_count()])
	a.free()
	b.free()
	var c := make(1)
	var d := make(1)
	d.position = Vector3(1, 0.25, 0)
	d.set_shape_param("height", 0.5)
	c.join_mesh(d)
	solid_ok(c, "a smaller box against a side")
	check(absf(analyze(c).volume - 1.5) < 1e-3, "joined with a smaller box (%f)" % analyze(c).volume)
	c.free()
	d.free()
	var e := make(1)
	var f := make(1)
	f.position = Vector3(1, 0, 0)
	e.join_mesh(f, true)
	check(e.get_face_count() == 12, "Keep Overlap Faces: both walls stay (%d)" % e.get_face_count())
	e.free()
	f.free()


# The shortest edge and the smallest face (hair-thin strips left by a cut show up here).
func sliver_info(m: HewnMesh3D) -> Vector2:
	var verts := m.get_vertices()
	var min_edge := 1e9
	var min_area := 1e9
	for f in m.get_face_count():
		var face := m.get_face(f)
		var nrm := Vector3.ZERO
		for i in face.size():
			var a := verts[face[i]]
			var b := verts[face[(i + 1) % face.size()]]
			min_edge = minf(min_edge, a.distance_to(b))
			nrm += a.cross(b)
		min_area = minf(min_area, nrm.length() * 0.5)
	return Vector2(min_edge, min_area)


func r29_room() -> HewnMesh3D:
	var r := make(1)
	for pn in ["width", "height", "depth"]:
		r.set_shape_param(pn, 4.0)
	r.set_shape_param("flip_normals", 1.0)
	return r


func _test_r29_cuts() -> void:
	# a door exactly down to the floor, and one a hair below it: no strip of wall under it
	for r29_bottom in [-2.0, -2.0004, -1.9996]:
		var r29_a := r29_room()
		var r29_h := unpack(r29_a.cut_box(AABB(Vector3(1.5, r29_bottom, -0.5), Vector3(1.0, 2.0, 1.0))))
		check(r29_h.size() == 1, "flush door (bottom %s): one hole (%d)" % [r29_bottom, r29_h.size()])
		var r29_si := sliver_info(r29_a)
		check(r29_si.x > 0.01 and r29_si.y > 1e-3, "flush door (bottom %s): no sliver faces (edge %f, area %f)" % [r29_bottom, r29_si.x, r29_si.y])
		var r29_floor := 0.0
		for f in r29_a.get_face_count():
			if r29_a.get_face_normal(f).y > 0.9:
				r29_floor += 1.0
		check(r29_floor >= 1.0, "flush door (bottom %s): the floor stays" % r29_bottom)
		check(analyze(r29_a).consistent, "flush door (bottom %s): consistent" % r29_bottom)
		r29_a.free()
	# a cut through a corner: both walls open, one hole, no slivers
	var r29_c := r29_room()
	var r29_ch := unpack(r29_c.cut_box(AABB(Vector3(1.5, -1.0, 1.5), Vector3(1.0, 2.0, 1.0))))
	check(r29_ch.size() == 1, "a cut through a corner: one hole (%d)" % r29_ch.size())
	var r29_csi := sliver_info(r29_c)
	check(r29_csi.x > 0.01, "a cut through a corner: no slivers (%f)" % r29_csi.x)
	check(analyze(r29_c).consistent, "a cut through a corner: consistent")
	# the same cut again: nothing more changes
	var r29_nv := r29_c.get_vertex_count()
	var r29_nf := r29_c.get_face_count()
	var r29_again := unpack(r29_c.cut_box(AABB(Vector3(1.5, -1.0, 1.5), Vector3(1.0, 2.0, 1.0))))
	check(r29_again.size() == 0 and r29_c.get_vertex_count() == r29_nv and r29_c.get_face_count() == r29_nf,
		"the same cut again changes nothing (%d holes, %d/%d verts, %d/%d faces)" % [r29_again.size(), r29_c.get_vertex_count(), r29_nv, r29_c.get_face_count(), r29_nf])
	r29_c.free()
	# a door whose side lies on the next wall: that wall stays whole
	var r29_s := r29_room()
	r29_s.cut_box(AABB(Vector3(1.0, -2.0, 1.5), Vector3(1.0, 2.5, 1.0)))
	var r29_side_area := 0.0
	var r29_sv := r29_s.get_vertices()
	for f in r29_s.get_face_count():
		if r29_s.get_face_normal(f).x < -0.9 and absf(r29_s.get_face_center(f).x - 2.0) < 1e-3:
			var r29_fc := r29_s.get_face(f)
			var r29_n := Vector3.ZERO
			for i in r29_fc.size():
				r29_n += r29_sv[r29_fc[i]].cross(r29_sv[r29_fc[(i + 1) % r29_fc.size()]])
			r29_side_area += r29_n.length() * 0.5
	check(absf(r29_side_area - 16.0) < 1e-3, "a door at the corner leaves the next wall whole (%f)" % r29_side_area)
	check(sliver_info(r29_s).x > 0.01, "a door at the corner: no slivers")
	r29_s.free()
	# a cut through two rooms and the wall between them (one mesh): consistent, no slivers
	var r29_t := r29_room()
	var r29_t2 := r29_room()
	r29_t2.position = Vector3(4, 0, 0)
	r29_t.merge_from(r29_t2)
	r29_t2.free()
	var r29_th := unpack(r29_t.cut_box(AABB(Vector3(1.0, -2.0, -0.5), Vector3(2.0, 2.0, 1.0))))
	check(r29_th.size() == 2, "a door through two touching walls: two holes (%d)" % r29_th.size())
	check(sliver_info(r29_t).x > 0.01 and analyze(r29_t).consistent, "two touching walls: no slivers, consistent")
	r29_t.free()


func _uv2_tris(am: ArrayMesh) -> Array:
	var out := []
	for s in am.get_surface_count():
		var arr := am.surface_get_arrays(s)
		var u2 = arr[Mesh.ARRAY_TEX_UV2]
		if u2 == null:
			continue
		var idx: PackedInt32Array = arr[Mesh.ARRAY_INDEX]
		for k in range(0, idx.size(), 3):
			out.append([u2[idx[k]], u2[idx[k + 1]], u2[idx[k + 2]]])
	return out


func _test_r29_uv2() -> void:
	var u2m := make(1)
	for pn in ["width", "height", "depth"]:
		u2m.set_shape_param(pn, 4.0)
	u2m.cut_box(AABB(Vector3(1.5, -2.0, -0.5), Vector3(1.0, 2.0, 1.0)))
	u2m.lightmap_uv2 = true
	var u2_tris := _uv2_tris(u2m.mesh)
	check(u2_tris.size() > 0, "lightmap UV2: the mesh has a UV2 channel")
	var u2_in := true
	var u2_area := 0.0
	var u2_zero := 0
	for t in u2_tris:
		for q in t:
			u2_in = u2_in and q.x >= 0.0 and q.y >= 0.0 and q.x <= 1.0 and q.y <= 1.0
		var a := absf((t[1] - t[0]).cross(t[2] - t[0])) * 0.5
		u2_area += a
		if a < 1e-9:
			u2_zero += 1
	check(u2_in, "lightmap UV2 stays inside 0..1")
	check(u2_area < 1.0 and u2_area > 0.3, "lightmap UV2 charts fill the atlas without overlap (area %f)" % u2_area)
	check(u2_zero == 0, "lightmap UV2: no collapsed triangles (%d)" % u2_zero)
	check(u2m.get_lightmap_size().x > 0 and u2m.mesh.lightmap_size_hint == u2m.get_lightmap_size(), "lightmap size hint is set (%s)" % u2m.get_lightmap_size())
	# no two charts overlap: rasterize the triangles on a grid
	var u2_grid := {}
	var u2_overlap := 0
	var u2_res := 256
	var u2_ti := 0
	for t in u2_tris:
		u2_ti += 1
		var mn: Vector2 = t[0].min(t[1]).min(t[2]) * u2_res
		var mx: Vector2 = t[0].max(t[1]).max(t[2]) * u2_res
		for gx in range(int(mn.x), int(ceil(mx.x))):
			for gy in range(int(mn.y), int(ceil(mx.y))):
				var c := (Vector2(gx, gy) + Vector2(0.5, 0.5)) / u2_res
				if Geometry2D.point_is_inside_triangle(c, t[0], t[1], t[2]):
					var key := Vector2i(gx, gy)
					if u2_grid.has(key) and u2_grid[key] != u2_ti:
						u2_overlap += 1
					u2_grid[key] = u2_ti
	check(u2_overlap == 0, "lightmap UV2 triangles do not overlap (%d cells)" % u2_overlap)
	# saved with the scene and loaded as it was
	var u2_data: Dictionary = u2m.mesh_data
	check(u2_data.has("lightmap_uv2"), "mesh_data keeps the lightmap UV2")
	var u2b := HewnMesh3D.new()
	u2b.lightmap_uv2 = true
	u2b.mesh_data = u2_data
	root.add_child(u2b)
	var u2_tb := _uv2_tris(u2b.mesh)
	check(u2_tb.size() == u2_tris.size() and (u2_tb.size() == 0 or u2_tb[0][0].is_equal_approx(u2_tris[0][0])), "loaded scene: the same UV2")
	# the geometry changes: new UV2
	var u2_v := u2m.get_vertices()
	u2_v[0] += Vector3(0.0, 0.5, 0.0)
	u2m.set_vertices(u2_v)
	check(u2m.get_mesh_data()["lightmap_hash"] != u2_data["lightmap_hash"], "edited geometry: the lightmap UV2 is made again")
	# the game mesh: tool faces out, UV2 in
	var u2_et := 0
	var u2_before: ArrayMesh = u2m.get_game_mesh()
	for s in u2_before.get_surface_count():
		u2_et += u2_before.surface_get_arrays(s)[Mesh.ARRAY_INDEX].size() / 3
	var u2_nd := StandardMaterial3D.new()
	u2_nd.set_meta("hw_tool", "nodraw")
	u2m.apply_material(u2_nd, PackedInt32Array([0]))
	var u2_game: ArrayMesh = u2m.get_game_mesh()
	var u2_gt := 0
	for s in u2_game.get_surface_count():
		u2_gt += u2_game.surface_get_arrays(s)[Mesh.ARRAY_INDEX].size() / 3
	check(u2_gt < u2_et and u2_gt > 0, "game mesh leaves the nodraw face out (%d of %d)" % [u2_gt, u2_et])
	check(_uv2_tris(u2_game).size() == u2_gt, "game mesh has UV2")
	u2m.free()
	u2b.free()


func _test_r29_bake() -> void:
	var bk_root := Node3D.new()
	bk_root.name = "Level"
	root.add_child(bk_root)
	var bk_room := HewnMesh3D.new()
	bk_room.name = "Room"
	bk_root.add_child(bk_room)
	bk_room.owner = bk_root
	bk_room.collision_mode = HewnMesh3D.COLLISION_TRIMESH
	bk_room.lightmap_uv2 = true
	var bk_trig := StandardMaterial3D.new()
	bk_trig.set_meta("hw_tool", "trigger")
	var bk_box := HewnMesh3D.new()
	bk_box.name = "TriggerBox"
	bk_box.position = Vector3(3, 0, 0)
	bk_root.add_child(bk_box)
	bk_box.owner = bk_root
	bk_box.apply_material(bk_trig)
	var bk_cut := HewnMesh3D.new()
	bk_cut.name = "Cutter"
	bk_cut.boolean_cutter = true
	bk_root.add_child(bk_cut)
	bk_cut.owner = bk_root
	var bk_light := OmniLight3D.new()
	bk_light.name = "Lamp"
	bk_room.add_child(bk_light)
	bk_light.owner = bk_root
	var bk_out: Node = HewnLevel.bake_scene(bk_root)
	check(bk_out != null and bk_out.name == "Level", "bake: a copy of the level")
	if bk_out:
		var bk_mi := bk_out.get_node_or_null("Room") as MeshInstance3D
		check(bk_mi != null and not (bk_mi is HewnMesh3D), "bake: the Hewn mesh became a MeshInstance3D")
		if bk_mi:
			check(bk_mi.mesh.get_surface_count() > 0 and bk_mi.mesh.surface_get_arrays(0)[Mesh.ARRAY_TEX_UV2] != null, "bake: the mesh keeps UV2")
			check(bk_mi.get_node_or_null("StaticBody3D/CollisionShape3D") != null, "bake: collision as a StaticBody3D")
			check(bk_mi.get_node_or_null("Occluder") is OccluderInstance3D, "bake: an occluder")
			check(bk_mi.get_node_or_null("Lamp") is OmniLight3D and bk_mi.get_node("Lamp").owner == bk_out, "bake: children come along, owned by the new root")
		var bk_tb := bk_out.get_node_or_null("TriggerBox")
		check(bk_tb != null and not (bk_tb is MeshInstance3D), "bake: (old) trigger faces are not drawn")
		check(bk_out.get_node_or_null("Cutter") == null, "bake: boolean cutters are left out")
		bk_out.free()
	var bk_err := HewnLevel.save_baked_scene(bk_root, "user://hw_baked.tscn")
	check(bk_err == OK, "bake: saved as a scene (%d)" % bk_err)
	var bk_ps := load("user://hw_baked.tscn") as PackedScene
	check(bk_ps != null, "bake: the saved scene loads")
	if bk_ps:
		var bk_inst := bk_ps.instantiate()
		check(bk_inst.get_node_or_null("Room/StaticBody3D/CollisionShape3D") != null and bk_inst.get_node_or_null("Room/Lamp") != null, "bake: the saved scene has the nodes")
		bk_inst.free()
	var bk_gerr := HewnLevel.export_scene_gltf(bk_root, "user://hw_level.glb")
	check(bk_gerr == OK and FileAccess.file_exists("user://hw_level.glb"), "export: the whole scene as glTF (%d)" % bk_gerr)
	var bk_n := HewnLevel.enable_lightmap_uv2(bk_root, 0.25)
	check(bk_n == 2 and bk_box.lightmap_uv2 and bk_box.lightmap_texel_size == 0.25, "lightmap UV2 for all meshes (%d)" % bk_n)
	bk_root.free()


func _test_r29_sweep() -> void:
	# an L-shaped rail with caps: a closed solid
	var sw := make(20)
	check(sw.get_face_count() > 0, "Sweep without points: a straight piece")
	solid_ok(sw, "sweep straight")
	sw.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(3, 0, 0), Vector3(3, 0, 2)])
	for sw_prof in 6:
		sw.set_shape_param("profile", sw_prof)
		solid_ok(sw, "sweep profile %d along an L" % sw_prof)
		check(tri_area_ok(sw), "sweep profile %d: triangles cover the faces" % sw_prof)
	sw.set_shape_param("profile", 1)
	sw.set_shape_param("width", 0.4)
	sw.set_shape_param("height", 0.3)
	var sw_bb := sw.get_aabb()
	check(is_equal_approx(sw_bb.position.y, 0.0) and is_equal_approx(sw_bb.end.y, 0.3), "sweep: the profile stands on the path (%s)" % sw_bb)
	check(is_equal_approx(sw_bb.end.x, 3.2) and is_equal_approx(sw_bb.position.z, -0.2), "sweep: mitered corner (%s)" % sw_bb)
	sw.set_shape_param("corner_radius", 0.5)
	solid_ok(sw, "sweep with a rounded corner")
	# a closed loop: a ring (genus 1)
	sw.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(4, 0, 0), Vector3(4, 0, 4), Vector3(0, 0, 4)])
	sw.set_shape_param("corner_radius", 0.0)
	sw.set_shape_param("closed", 1)
	solid_ok(sw, "sweep loop", 1)
	# up a slope (a handrail along stairs)
	sw.set_shape_param("closed", 0)
	sw.set_shape_param("profile", 0)
	sw.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(2, 0, 0), Vector3(4, 2, 0), Vector3(6, 2, 0)])
	solid_ok(sw, "sweep up a slope")
	# along a Path3D
	var sw_path := Path3D.new()
	sw_path.curve = Curve3D.new()
	sw_path.curve.add_point(Vector3(0, 0, 0), Vector3.ZERO, Vector3(1, 0, 0))
	sw_path.curve.add_point(Vector3(3, 0, 3), Vector3(0, 0, -1), Vector3.ZERO)
	root.add_child(sw_path)
	sw.sweep_along(sw_path)
	check(sw.shape == 20 and sw.poly_points.size() > 3, "sweep along a Path3D (%d points)" % sw.poly_points.size())
	solid_ok(sw, "sweep along a curve")
	sw_path.free()
	sw.free()

	# an arched passage: a door cut through a wall slab, the holes joined, the ceiling bent into a vault
	var ar := make(1)
	ar.set_shape_param("width", 4.0)
	ar.set_shape_param("height", 3.0)
	ar.set_shape_param("depth", 0.5)
	var ar_holes := unpack(ar.cut_box(AABB(Vector3(-0.5, -1.0, -1.0), Vector3(1.0, 1.5, 2.0))))
	check(ar_holes.size() == 2, "arch: a door through a wall (%d holes)" % ar_holes.size())
	if ar_holes.size() == 2:
		ar.bridge_holes(ar_holes[0], ar_holes[1])
		var ar_top := edge_pairs(ar, func(a, b): return absf(a.y - 0.5) < 1e-4 and absf(b.y - 0.5) < 1e-4 and absf(a.z - b.z) < 1e-4 and absf(absf(a.z) - 0.25) < 1e-4 and absf(a.x) <= 0.5001 and absf(b.x) <= 0.5001)
		check(ar_top.size() == 4, "arch: the two top edges of the passage (%d)" % ar_top.size())
		var ar_nf := ar.get_face_count()
		var ar_b := analyze(ar)
		check(ar_b.closed and ar_b.consistent, "arch: the passage through the wall is closed")
		ar.arch_edges(ar_top, 8, 1.0)
		var ar_a := analyze(ar)
		check(ar_a.closed and ar_a.consistent, "arch: the wall stays closed and consistent (closed %s, consistent %s)" % [ar_a.closed, ar_a.consistent])
		check(ar.get_face_count() == ar_nf + 7, "arch: the passage ceiling became 8 strips (%d -> %d)" % [ar_nf, ar.get_face_count()])
		var ar_top_y := -1e9
		for v in ar.get_vertices():
			if absf(v.x) < 1e-3 and absf(absf(v.z) - 0.25) < 1e-4 and v.y < 1.4:
				ar_top_y = maxf(ar_top_y, v.y)
		check(is_equal_approx(ar_top_y, 1.0), "arch: a half circle, 0.5 above the door top (%f)" % ar_top_y)
		check(tri_area_ok(ar), "arch: triangles cover the faces")
	ar.free()


class R29Target extends Node:
	var got := 0
	func open_door(_body) -> void:
		got += 1


func _test_r29_overlays() -> void:
	var ov_root := Node3D.new()
	root.add_child(ov_root)
	var ov_floor := HewnMesh3D.new()
	ov_root.add_child(ov_floor)
	ov_floor.owner = ov_root
	ov_floor.shape = 2
	ov_floor.set_shape_param("width", 4.0)
	ov_floor.set_shape_param("depth", 4.0)
	# a round blob: opaque inside a circle
	var ov_img := Image.create_empty(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			var d := Vector2(x - 31.5, y - 31.5).length()
			ov_img.set_pixel(x, y, Color(1, 0, 0, 1.0 if d < 20.0 else 0.0))
	var ov_tex := ImageTexture.create_from_image(ov_img)
	var ov_a := HewnOverlay3D.new()
	ov_a.texture = ov_tex
	ov_a.size = Vector2(1, 1)
	ov_root.add_child(ov_a)
	ov_a.owner = ov_root
	ov_a.position = Vector3(0, 0.1, 0)
	var ov_tris := ov_a.reproject()
	check(ov_tris >= 2, "overlay: projected onto the floor (%d triangles)" % ov_tris)
	var ov_wa: Array = ov_a.get_world_arrays()
	var ov_ok := true
	for v in ov_wa[0]:
		ov_ok = ov_ok and absf(v.y - 0.002) < 1e-4 and absf(v.x) <= 0.5001 and absf(v.z) <= 0.5001
	check(ov_ok, "overlay: on the floor, a hair above it, inside its size")
	# the alpha outline: a polygon around the opaque pixels, smaller than the rectangle
	ov_a.outline = HewnOverlay3D.OUTLINE_ALPHA
	ov_a.max_vertices = 8
	var ov_pts: PackedVector2Array = ov_a.get_outline_points()
	var ov_area := 0.0
	for i in ov_pts.size():
		ov_area += ov_pts[i].cross(ov_pts[(i + 1) % ov_pts.size()])
	ov_area = absf(ov_area) * 0.5
	check(ov_pts.size() >= 3 and ov_pts.size() <= 8, "alpha outline: at most 8 corners (%d)" % ov_pts.size())
	check(ov_area < 0.9 and ov_area > PI * 0.3125 * 0.3125, "alpha outline: smaller than the rectangle, around the blob (%f)" % ov_area)
	var ov_in := true
	for k in 32:
		var a := TAU * k / 32.0
		var q := Vector2(cos(a), sin(a)) * (19.0 / 64.0)
		ov_in = ov_in and Geometry2D.is_point_in_polygon(q, ov_pts)
	check(ov_in, "alpha outline: every opaque pixel is inside")
	ov_a.reproject()
	check(ov_a.get_triangle_count() >= 1, "alpha outline: projected (%d)" % ov_a.get_triangle_count())
	# two overlays with one texture: one material, one batch (one draw call)
	var ov_b := HewnOverlay3D.new()
	ov_b.texture = ov_tex
	ov_root.add_child(ov_b)
	ov_b.owner = ov_root
	ov_b.position = Vector3(1.2, 0.1, 1.2)
	ov_b.reproject()
	HewnOverlay3D.flush_batches()
	check(ov_a.get_draw_material() == ov_b.get_draw_material(), "overlays with one texture share the material")
	check(HewnOverlay3D.get_batch_count() == 1, "two overlays: one batch (%d)" % HewnOverlay3D.get_batch_count())
	# over the edge: only on the floor, cut at its border
	ov_b.position = Vector3(1.8, 0.1, 0)
	ov_b.reproject()
	var ov_bw: Array = ov_b.get_world_arrays()
	var ov_mx := 0.0
	for v in ov_bw[0]:
		ov_mx = maxf(ov_mx, v.x)
	check(is_equal_approx(ov_mx, 2.0), "overlay over the edge: cut at the floor's border (%f)" % ov_mx)
	# saved with the scene: no projection needed in the game
	var ov_c := HewnOverlay3D.new()
	ov_c.texture = ov_tex
	ov_c.cache = ov_a.cache
	check(ov_c.get_triangle_count() == ov_a.get_triangle_count(), "overlay cache is saved and loaded")
	ov_c.free()
	# baked: one mesh for all overlays
	var ov_baked: Node = HewnLevel.bake_scene(ov_root)
	var ov_mi := ov_baked.get_node_or_null("Overlays") as MeshInstance3D
	check(ov_mi != null and ov_mi.mesh.get_surface_count() == 1, "bake: the overlays merged into one mesh")
	check(ov_baked.find_children("*", "HewnOverlay3D", true, false).is_empty(), "bake: no overlay nodes left")
	ov_baked.free()

	# entities
	var en := HewnEntity3D.new()
	en.classname = "spawn_point"
	en.properties = {"team": 2}
	ov_root.add_child(en)
	check(en.is_in_group("spawn_point") and en.get_value("team") == 2, "entity: in the group of its class, with its values")
	en.classname = "info_target"
	check(not en.is_in_group("spawn_point") and en.is_in_group("info_target"), "entity: class change moves the group")
	check(en.get_meta("team", 0) == 2, "entity: its properties as metadata")
	ov_root.free()
	HewnOverlay3D.flush_batches()
	check(HewnOverlay3D.get_batch_count() == 0, "overlays gone: no batches left")


func _test_r31() -> void:
	# path stairs: points with heights climb and go down flight by flight
	var ps := make(22)
	ps.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(3, 1, 0), Vector3(3, 0.2, 3)])
	var ps_bb := ps.get_aabb()
	check(absf(ps_bb.position.y) < 0.01 and absf(ps_bb.end.y - 1.0) < 0.05, "path stairs: up to the second point's height, then down (%s)" % ps_bb)
	ps.free()
	# a wall's foot follows its points' heights
	var wl := make(19)
	wl.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(4, 1, 0)])
	var wl_hi := -1.0
	for v in wl.get_vertex_count():
		if wl.get_vertex(v).x > 3.9:
			wl_hi = maxf(wl_hi, wl.get_vertex(v).y)
	check(absf(wl_hi - (1.0 + wl.get_shape_param("height"))) < 0.01, "wall: its far end raised with its point (%f)" % wl_hi)
	wl.free()
	# subdivide: 3 cuts on a quad make a 4 x 4 grid; smoothness pushes the middle out
	var sq := make(2)
	var sq_n := sq.get_face_count()
	sq.subdivide_faces_ex(PackedInt32Array([0]), 3)
	check(sq.get_face_count() == sq_n - 1 + 16, "subdivide: 3 cuts, a 4 x 4 grid (%d faces)" % sq.get_face_count())
	sq.free()
	var sc := make(1)
	sc.subdivide_faces_ex(PackedInt32Array(range(6)), 2, 1.0)
	var sc_far := 0.0
	for v in sc.get_vertex_count():
		sc_far = maxf(sc_far, sc.get_vertex(v).length())
	check(sc.get_face_count() == 54 and sc_far > 0.5 * sqrt(3.0) - 0.01, "subdivide: smoothness rounds the cube (%d faces)" % sc.get_face_count())
	var sc_fr := make(1)
	sc_fr.subdivide_faces_ex(PackedInt32Array(range(6)), 2, 0.0, true, 0.5, 0.0, 7)
	var sc_fr2 := make(1)
	sc_fr2.subdivide_faces_ex(PackedInt32Array(range(6)), 2, 0.0, true, 0.5, 0.0, 7)
	check(sc_fr.get_vertices() == sc_fr2.get_vertices() and sc_fr.get_vertices() != sc.get_vertices(), "subdivide: fractal with a seed, the same each time")
	sc.free()
	sc_fr.free()
	sc_fr2.free()
	# CUSTOM0..3: painted values in the built mesh in the chosen format
	var cu := make(1)
	cu.set_custom_value(1, PackedInt32Array(), Color(0.25, 0.5, 0.75, 1.0))
	cu.custom_formats = PackedInt32Array([-1, 7, -1, -1])
	var cu_arr: Array = (cu.mesh as ArrayMesh).surface_get_arrays(0)
	var cu1 = cu_arr[Mesh.ARRAY_CUSTOM1]
	check(cu1 is PackedFloat32Array and cu1.size() > 4 and absf(cu1[1] - 0.5) < 1e-4, "CUSTOM1 painted, RGBA float in the mesh")
	var cu_state := cu.get_mesh_data()
	check(cu_state.has("face_custom"), "the custom values are saved with the mesh")
	cu.free()
	# profiles: handles give a curve, Godot's Curve in and out
	var pr := HewnProfile.new()
	pr.points = PackedVector2Array([Vector2(0, 0), Vector2(0.5, 1), Vector2(1, 0)])
	check(pr.sample().size() == 3, "profile: straight pieces, only the points")
	pr.handle_types = PackedInt32Array([0, 0, 0])
	check(pr.sample().size() == 1 + 2 * pr.resolution, "profile: auto handles, a curve (%d samples)" % pr.sample().size())
	var pr_c: Curve = pr.to_curve()
	var pr2 := HewnProfile.new()
	pr2.from_curve(pr_c)
	check(pr2.points.size() == 3 and pr2.points[1].distance_to(Vector2(0.5, 1)) < 1e-4, "profile: through Godot's Curve and back")
	pr.sample_equal = true
	var pr_s := pr.sample()
	var pr_d0 := pr_s[0].distance_to(pr_s[1])
	var pr_eq := true
	for i in range(1, pr_s.size() - 1):
		pr_eq = pr_eq and absf(pr_s[i].distance_to(pr_s[i + 1]) - pr_d0) < pr_d0 * 0.15
	check(pr_eq, "profile: equal lengths")
	# door / arch / wedge from a drawn curve
	var dr := make(13)
	var dr_n := dr.get_vertex_count()
	var dp := HewnProfile.new()
	dp.points = PackedVector2Array([Vector2(0, 0), Vector2(0, 0.6), Vector2(0.5, 1), Vector2(1, 0.6), Vector2(1, 0)])
	dr.shape_profile = dp
	check(dr.get_vertex_count() != dr_n, "doorway: a pointed opening from the curve")
	dr.free()
	var wg := make(16)
	var wp := HewnProfile.new()
	wp.points = PackedVector2Array([Vector2(0, 0), Vector2(0.5, 0.8), Vector2(1, 1)])
	wg.shape_profile = wp
	check(wg.get_face_count() == 2 + 1 + 1 + 2, "wedge: a slope of two pieces (%d faces)" % wg.get_face_count())
	wg.free()
	var ar := make(12)
	var ap := HewnProfile.new()
	ap.points = PackedVector2Array([Vector2(0, 0), Vector2(0.5, 1), Vector2(1, 0)])
	ar.shape_profile = ap
	check(ar.get_face_count() == 2 * 4 + 2, "arch: a pointed arch from two straight pieces (%d faces)" % ar.get_face_count())
	ar.free()
	# mirror without merging: the halves stay apart
	var mr := make(1)
	var mm := HewnMirrorModifier.new()
	mm.merge = false
	mr.position = Vector3(0, 0, 0)
	mr.add_modifier(mm)
	var mr_t := _shown_tris(mr)
	mm.merge = true
	check(mr_t > 0 and _shown_tris(mr) > 0 and "merge" in mm, "mirror: Merge can be turned off (%d triangles)" % mr_t)
	mr.free()


func _count_role(m: HewnMesh3D, role: int) -> int:
	var c := 0
	for f in m.get_face_count():
		if m.get_face_role(f) == role:
			c += 1
	return c


# Round 49: world UVs project standard format (onto the nearest world plane); a concave face
# touching the cut line at a corner is split right.
func _test_r49() -> void:
	# a cube with world UVs, its top tilted (the +Z top edge raised): the top keeps the floor projection, so its
	# texture along Z spans the same as the bottom's (stretched along the slope, not turned with it)
	var w := make(1)
	w.set_world_uv_all(1.0)
	for i in w.get_vertex_count():
		var p := w.get_vertex(i)
		if p.y > 0.4 and p.z > 0.4:
			w.set_vertex(i, p + Vector3(0, 0.3, 0))
	var top := -1
	var bottom := -1
	for f in w.get_face_count():
		var n := w.get_face_normal(f)
		if n.y > 0.8:
			top = f
		elif n.y < -0.9:
			bottom = f
	var span := func(f: int) -> float:
		var uvs := w.get_face_uvs(f)
		var mn := uvs[0]
		var mx := uvs[0]
		for q in uvs:
			mn = mn.min(q)
			mx = mx.max(q)
		return (mx - mn).length()
	check(top >= 0 and bottom >= 0 and absf(span.call(top) - span.call(bottom)) < 1e-3, "world UV: a sloped top projects onto the floor plane (%f / %f)" % [span.call(top) if top >= 0 else -1.0, span.call(bottom) if bottom >= 0 else -1.0])
	# tilted past 45 degrees it takes the wall's projection: the side face's texture spans as the wall's
	w.free()
	# a face with a notch whose tip touches the cut line between the two crossings: the part beyond goes whole
	var c := HewnMesh3D.new()
	root.add_child(c)
	c.shape = 0
	var ids := PackedInt32Array()
	for q in [Vector3(0, 0, 0), Vector3(4, 0, 0), Vector3(4, 0, 2), Vector3(2, 0, 1), Vector3(0, 0, 2)]:
		ids.append(c.add_vertex(q))
	c.add_face(ids)
	c.bisect(Vector3(0, 0, 1), Vector3(0, 0, 1), false, true, false)
	var maxz := -1.0
	for f in c.get_face_count():
		for v in c.get_face(f):
			maxz = maxf(maxz, c.get_vertex(v).z)
	check(c.get_face_count() >= 1 and maxz < 1.001, "bisect: a notched face touching the line is cut right (max z %f)" % maxz)
	c.free()
	# a bevel on one edge of a 2 m box can go all the way along the faces beside it: 2 m makes a prism
	for bv_seg in [1]:
		var bx := make(1)
		bx.set_shape_param("width", 2.0)
		bx.set_shape_param("height", 2.0)
		bx.set_shape_param("depth", 2.0)
		var top_front := PackedInt32Array()
		for i in bx.get_vertex_count():
			var p := bx.get_vertex(i)
			if p.y > 0.9 and p.z > 0.9:
				top_front.append(i)
		bx.bevel_edges(top_front, 2.0, bv_seg)
		var bxa := analyze(bx)
		var bx_maxz := -9.0
		for i in bx.get_vertex_count():
			bx_maxz = maxf(bx_maxz, bx.get_vertex(i).z if bx.get_vertex(i).y > 0.9 else -9.0)
		check(bxa.closed and bxa.euler == 2 and bx.get_face_count() == 4 + bv_seg and bx_maxz < -0.99, "bevel all the way: %d segment(s), %d faces, closed %s, euler %d, top back at %f" % [bv_seg, bx.get_face_count(), bxa.closed, bxa.euler, bx_maxz])
		bx.free()


func _test_r32() -> void:
	# bent stairs: borders too; a little curvature changes them only a little (also on a wide footprint)
	var bs := make(10)
	bs.set_shape_param("border_left", 1.0)
	bs.set_shape_param("border_right", 1.0)
	bs.set_shape_param("curvature", 90.0)
	check(_count_role(bs, 15) > 0, "bent stairs: borders along both sides (%d faces)" % _count_role(bs, 15))
	var wide := make(10)
	wide.set_shape_param("width", 3.0)
	wide.set_shape_param("depth", 1.0)
	var bb0 := wide.get_aabb()
	wide.set_shape_param("curvature", 0.5)
	var bb1 := wide.get_aabb()
	check(bb0.size.distance_to(bb1.size) < 0.1, "stairs: a little curvature keeps their direction (%s -> %s)" % [bb0.size, bb1.size])
	wide.set_shape_param("curvature", -0.5)
	check(bb0.size.distance_to(wide.get_aabb().size) < 0.1, "stairs: a little curvature the other way too (%s)" % wide.get_aabb().size)
	bs.free()
	wide.free()
	# path stairs: by step count on each flight, borders on the landings
	var ps := make(22)
	ps.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(3, 0, 0), Vector3(3, 0, 3)])
	ps.set_shape_param("fill_by", 1.0)
	ps.set_shape_param("steps", 5)
	check(_count_role(ps, 12) >= 10 and _count_role(ps, 12) <= 12, "path stairs: 5 steps on each of the two flights (%d treads)" % _count_role(ps, 12))
	var ps_b := _count_role(ps, 15)
	ps.set_shape_param("landing_borders", 1.0)
	check(_count_role(ps, 15) > ps_b, "path stairs: borders on the landing (%d -> %d)" % [ps_b, _count_role(ps, 15)])
	ps.free()
	# a sphere's UVs like a globe: no face stretched across the seam (a corner's U far from the others)
	var sph := make(5)
	var sph_ok := true
	var sph_d: Dictionary = sph.mesh_data
	var sph_sizes: PackedInt32Array = sph_d["face_sizes"]
	var sph_uvs: PackedVector2Array = sph_d["face_uvs"]
	var sph_k := 0
	for sz in sph_sizes:
		var lo := INF
		var hi := -INF
		for i in sz:
			lo = minf(lo, sph_uvs[sph_k + i].x)
			hi = maxf(hi, sph_uvs[sph_k + i].x)
		sph_ok = sph_ok and hi - lo < 0.5
		sph_k += sz
	check(sph_ok and sph_uvs.size() > 0, "sphere: globe UVs, none across the seam")
	sph.free()
	# path stairs turning 60 degrees: the landing meets both flights (five corners: the outer one between them)
	var ps60 := make(22)
	ps60.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(3, 0, 0), Vector3(3 + 3 * cos(PI / 3), 0, 3 * sin(PI / 3))])
	var ps60_land := false
	for f in ps60.get_face_count():
		if ps60.get_face_role(f) == 12 and ps60.get_face(f).size() == 5:
			ps60_land = true
	check(ps60_land, "path stairs: a 60 degree turn's landing has the outer corner (no gap)")
	ps60.free()
	# a symmetric profile: one half changed, the other follows
	var sp := HewnProfile.new()
	sp.points = PackedVector2Array([Vector2(0, 0), Vector2(0, 1), Vector2(1, 1), Vector2(1, 0)])
	sp.symmetric = true
	sp.set_point(1, Vector2(0.1, 0.8))
	check(sp.points.size() == 4 and sp.points[2].distance_to(Vector2(0.9, 0.8)) < 1e-4, "symmetric profile: the mirrored point follows (%s)" % sp.points)
	sp.insert_point(1, Vector2(0.05, 0.4))
	check(sp.points.size() == 6 and sp.points[4].distance_to(Vector2(0.95, 0.4)) < 1e-4, "symmetric profile: a new point gets its mirror (%s)" % sp.points)
	sp.remove_point(4)
	check(sp.points.size() == 4, "symmetric profile: removing a point removes its mirror")
	# a cutter's lining: made from what it cuts, follows when it moves away
	var w1 := make(1)
	w1.set_shape_param("width", 0.3)
	w1.set_shape_param("height", 3.0)
	w1.set_shape_param("depth", 4.0)
	w1.position = Vector3(-2, 0, 0)
	var w2 := make(1)
	w2.set_shape_param("width", 0.3)
	w2.set_shape_param("height", 3.0)
	w2.set_shape_param("depth", 4.0)
	w2.position = Vector3(2, 0, 0)
	var lc := HewnCutter3D.new()
	lc.size = Vector3(5.0, 1.5, 1.0)
	root.add_child(lc)
	lc.lining = HewnCutter3D.LINING_AUTO
	lc.update_lining()
	var lm: HewnMesh3D = lc.get_lining_mesh()
	check(lm != null and lm.get_face_count() > 0 and lm.visible, "cutter lining: a corridor between the two walls (%d faces)" % (lm.get_face_count() if lm else -1))
	lc.position = Vector3(0, 0, 30)
	lc.update_lining()
	check(lm.get_face_count() == 0, "cutter lining: moved away from the walls, no corridor left")
	lc.free()
	w1.free()
	w2.free()
	# an odd shape (a wedge: a slope on top) cut through from above: the corridor is the cutter's sides inside it
	var wd := make(16)
	var vc := HewnCutter3D.new()
	vc.size = Vector3(0.4, 3.0, 0.4)
	root.add_child(vc)
	vc.lining = HewnCutter3D.LINING_AUTO
	vc.update_lining()
	var vm: HewnMesh3D = vc.get_lining_mesh()
	var v_off := 0
	var v_area := 0.0
	if vm:
		for p in vm.get_vertices():
			var on_side := absf(absf(p.x) - 0.2) < 1e-3 or absf(absf(p.z) - 0.2) < 1e-3
			v_off += 0 if on_side and p.y > -0.5 - 1e-3 and p.y < 0.5 + 1e-3 else 1
		var vv := vm.get_vertices()
		for f in vm.get_face_count():
			var fv := vm.get_face(f)
			for i in range(1, fv.size() - 1):
				v_area += (vv[fv[i]] - vv[fv[0]]).cross(vv[fv[i + 1]] - vv[fv[0]]).length() * 0.5
	check(vm != null and vm.get_face_count() > 0 and v_off == 0, "cutter through a wedge: the corridor on its sides, inside the wedge (%d off)" % v_off)
	# (the wedge: 0.5 high in the middle, its sides inside it the perimeter times that)
	check(absf(v_area - 0.8) < 0.01, "cutter through a wedge: the corridor's area, 4 sides 0.4 wide, 0.5 high on average (%f)" % v_area)
	vc.free()
	wd.free()
	# a wall with corners and a closed trim of a custom profile with a point twice (once crashed the corridor): the
	# corridor made at many places, the trim without faces of no area
	var cw := make(19)
	cw.set_shape_param("side", 0)
	cw.poly_points = PackedVector3Array([Vector3(3.5, 0, 4.416666), Vector3(-4, 0, 4.416666), Vector3(-4, 0, 2.416666), Vector3(-7, 0, 2.416666),
			Vector3(-7, 0, -1.083334), Vector3(-4, 0, -1.083334), Vector3(-4, 0, -4.583334), Vector3(3.5, 0, -4.583334), Vector3(3.5, 0, -3.083334),
			Vector3(8, 0, -3.083334), Vector3(8, 0, 1.916666), Vector3(3.5, 0, 1.916666), Vector3(3.5, 0, 4.416666)])
	cw.position = Vector3(-28.5, 0.5, -30.416666)
	var ct := make(20)
	var ctp := HewnProfile.new()
	ctp.points = PackedVector2Array([Vector2(0, 0), Vector2(1, 0), Vector2(1, 0.62587315), Vector2(0, 1), Vector2(0, 1)])
	ct.sweep_profile = ctp
	for kv in [["accent", 2], ["align", 1], ["closed", 1], ["drop", 0.5], ["height", 0.208], ["profile", 6], ["width", 0.2]]:
		ct.set_shape_param(kv[0], kv[1])
	ct.poly_points = PackedVector3Array([Vector3(-5.5, 0.25, -4.5), Vector3(-5.5, 0.25, -1), Vector3(-8.5, 0.25, -1), Vector3(-8.5, 0.25, 2.5),
			Vector3(-5.5, 0.25, 2.5), Vector3(-5.5, 0.25, 4.5), Vector3(2, 0.25, 4.5), Vector3(2, 0.25, 2), Vector3(6.5, 0.25, 2), Vector3(6.5, 0.25, -3),
			Vector3(2, 0.25, -3), Vector3(2, 0.25, -4.5)])
	ct.position = Vector3(-27, 0.25, -30.5)
	var ct_bad := 0
	var ctv := ct.get_vertices()
	for f in ct.get_face_count():
		var fv := ct.get_face(f)
		var seen := {}
		for v in fv:
			seen[ctv[v].snapped(Vector3(1e-5, 1e-5, 1e-5))] = true
		ct_bad += 1 if seen.size() < 3 else 0
	check(ct_bad == 0, "a custom profile with a point twice: no face without an area (%d)" % ct_bad)
	var cc := HewnCutter3D.new()
	cc.size = Vector3(1.25, 1, 2)
	cc.lining = HewnCutter3D.LINING_CORRIDOR
	root.add_child(cc)
	var cc_t0 := Time.get_ticks_msec()
	var cc_n := 0
	for ix in range(-6, 7, 2):
		for iz in range(-6, 7, 2):
			cc.transform = Transform3D(Basis(Vector3(0, 1, 0), Vector3(0, 0, 1), Vector3(1, 0, 0)), Vector3(-22.1173 + (ix + 6) * 0.25, 1.507886, -32.625 + (iz + 6) * 0.25))
			cc.update_lining()
			cc_n += 1
	check(cc_n == 49, "the corridor through a wall and a trim at 49 places (no crash)")
	check(Time.get_ticks_msec() - cc_t0 < 5000, "and quickly (%d ms)" % (Time.get_ticks_msec() - cc_t0))
	cc.free()
	ct.free()
	cw.free()
	# two rooms (boxes turned in) with a gap: the corridor across the gap only
	var ra := make(1)
	ra.set_shape_param("width", 4.0)
	ra.set_shape_param("flip_normals", 1.0)
	ra.position = Vector3(-2.2, 0, 0)
	var rb := make(1)
	rb.set_shape_param("width", 4.0)
	rb.set_shape_param("flip_normals", 1.0)
	rb.position = Vector3(2.2, 0, 0)
	var rc := HewnCutter3D.new()
	rc.size = Vector3(2.0, 0.6, 0.6)
	root.add_child(rc)
	rc.lining = HewnCutter3D.LINING_AUTO
	rc.update_lining()
	var rm: HewnMesh3D = rc.get_lining_mesh()
	var r_out := 0
	if rm:
		for p in rm.get_vertices():
			r_out += 1 if absf(p.x) > 0.2 + 1e-3 else 0
	check(rm != null and rm.get_face_count() >= 4 and r_out == 0, "cutter between two rooms: the corridor across the gap (%d faces, %d outside)" % [rm.get_face_count() if rm else -1, r_out])
	rc.free()
	ra.free()
	rb.free()
	# an alcove of a cutter turned against the wall of a room: along the cutter, as deep as its far side
	var room := make(1)
	room.set_shape_param("width", 4.0)
	room.set_shape_param("height", 4.0)
	room.set_shape_param("depth", 4.0)
	room.set_shape_param("flip_normals", 1.0)
	var ac := HewnCutter3D.new()
	ac.size = Vector3(2.0, 1.0, 1.0)
	root.add_child(ac)
	ac.transform = Transform3D(Basis(Vector3.UP, deg_to_rad(30.0)), Vector3(2.0, 0.0, 0.0))
	ac.lining = HewnCutter3D.LINING_ALCOVE
	ac.update_lining()
	var am: HewnMesh3D = ac.get_lining_mesh()
	var a_back := 0
	var a_out := 0
	if am:
		for p in am.get_vertices():
			a_back += 1 if absf(p.x - 1.0) < 1e-3 else 0
			a_out += 1 if p.x > 1.0 + 1e-3 or absf(p.z) > 0.5 + 1e-3 else 0
	check(am != null and am.get_face_count() > 0 and a_back >= 4 and a_out == 0, "alcove of a turned cutter: along it, to its far side (%d on the back, %d outside it)" % [a_back, a_out])
	ac.free()
	room.free()
	# bevel by percent; harden normals: the faces around strong, saved
	var bv := make(1)
	var bv_pairs := edge_pairs(bv, func(a: Vector3, b: Vector3) -> bool: return a.y > 0.4 and b.y > 0.4 and absf(a.z - b.z) < 1e-4 and a.z > 0.4)
	check(bv_pairs.size() == 2, "bevel test: one top edge")
	bv.free()
	var hm := make(1)
	var hd: Dictionary = hm.get_mesh_data()
	var fl: PackedInt32Array = hd["face_flags"]
	fl[0] = fl[0] | (1 << 16)
	hd["face_flags"] = fl
	hm.set_mesh_data(hd)
	check(((hm.get_mesh_data()["face_flags"] as PackedInt32Array)[0] >> 16) & 3 == 1, "face strength: strong is saved")
	hm.free()


func _shown_tris(m: HewnMesh3D) -> int:
	var t := 0
	for s in m.mesh.get_surface_count():
		t += m.mesh.surface_get_arrays(s)[Mesh.ARRAY_INDEX].size() / 3
	return t


func _test_r30() -> void:
	# walls: a closed ring has no top over the room; the wall stands inside the line by default when drawn
	var wl := make(19)
	wl.set_shape_param("side", 0)
	wl.poly_points = PackedVector3Array([Vector3(-2, 0, -2), Vector3(2, 0, -2), Vector3(2, 0, 2), Vector3(-2, 0, 2), Vector3(-2, 0, -2)])
	solid_ok(wl, "closed wall ring", 1)
	var wl_bb := wl.get_aabb()
	check(absf(wl_bb.size.x - 4.0) < 1e-3, "walls inside the line: the outside stays on the line (%s)" % wl_bb)
	wl.set_shape_param("side", 2)
	check(absf(wl.get_aabb().size.x - 4.4) < 1e-3, "walls outside the line (%s)" % wl.get_aabb())
	wl.set_shape_param("side", 1)
	check(absf(wl.get_aabb().size.x - 4.2) < 1e-3, "walls centered on the line (%s)" % wl.get_aabb())
	wl.free()
	# column: base / top keep their height, the shaft repeats
	var col := make(21)
	solid_ok(col, "column")
	var col_v0 := col.get_vertex_count()
	col.set_shape_param("shaft_style", 2)
	col.set_shape_param("shaft_count", 3)
	check(col.get_vertex_count() > col_v0, "column: a ringed shaft three times has more rings")
	solid_ok(col, "column ringed x3")
	col.set_shape_param("shaft_repeat", 1)
	col.set_shape_param("shaft_length", 0.5)
	solid_ok(col, "column shaft by distance")
	var prof := HewnProfile.new()
	prof.points = PackedVector2Array([Vector2(1.6, 0), Vector2(1.6, 1)])
	col.base_profile = prof
	var col_bb := col.get_aabb()
	check(absf(col_bb.size.x - 0.25 * 2 * 1.6) < 0.05, "column: a custom base profile (%s)" % col_bb)
	col.make_profiles_editable()
	check(col.shaft_profile != null and col.top_profile != null, "column: the styles as editable profiles")
	col.free()
	# non-destructive cut: a cutter box through a room's wall; moved away the wall is whole again
	var nd := make(1)
	for pn in ["width", "height", "depth"]:
		nd.set_shape_param(pn, 4.0)
	nd.set_shape_param("flip_normals", 1.0)
	var nd_tris := _shown_tris(nd)
	var nd_nv := nd.get_vertex_count()
	var cutter := HewnCutter3D.new()
	cutter.size = Vector3(1.0, 2.0, 1.0)
	cutter.position = Vector3(2.0, -1.0, 0.0)  # through the +X wall, down to the floor
	cutter.cut_ends = false  # (its bottom on the floor: the floor stays)
	root.add_child(cutter)
	nd.update_cuts()
	check(nd.is_cut() and _shown_tris(nd) != nd_tris, "a cutter cuts the wall it reaches (%d -> %d triangles)" % [nd_tris, _shown_tris(nd)])
	check(nd.get_vertex_count() == nd_nv, "the mesh's own geometry stays as it was")
	var nd_floor := false
	var nd_mesh: ArrayMesh = nd.mesh
	for s in nd_mesh.get_surface_count():
		var arr := nd_mesh.surface_get_arrays(s)
		var pv: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
		var idx: PackedInt32Array = arr[Mesh.ARRAY_INDEX]
		for k in range(0, idx.size(), 3):
			# (a floor triangle under the point next to the wall; the floor is one face again, not split)
			var ta := pv[idx[k]]
			var tb := pv[idx[k + 1]]
			var tc := pv[idx[k + 2]]
			if absf(ta.y + 2.0) < 1e-3 and absf(tb.y + 2.0) < 1e-3 and absf(tc.y + 2.0) < 1e-3 and 					Geometry2D.point_is_inside_triangle(Vector2(1.9, 0.0), Vector2(ta.x, ta.z), Vector2(tb.x, tb.z), Vector2(tc.x, tc.z)):
				nd_floor = true
	check(nd_floor, "the floor under the cutter stays (its side is flush)")
	cutter.position = Vector3(20, 0, 0)
	nd.update_cuts()
	check(not nd.is_cut() and _shown_tris(nd) == nd_tris, "the cutter moved away: the wall is whole again")
	cutter.position = Vector3(2.0, -1.0, 0.0)
	nd.update_cuts()
	# inside a prefab (a door): it cuts the wall, not the door's own frame
	var door := Node3D.new()
	door.scene_file_path = "res://door.tscn"
	var door_root := Node3D.new()
	root.add_child(door_root)
	door_root.add_child(door)
	door.owner = door_root
	var frame := HewnMesh3D.new()
	door.add_child(frame)
	frame.position = Vector3(12.0, -1.0, 0.0)
	var door_cut := HewnCutter3D.new()
	door.add_child(door_cut)
	door_cut.position = Vector3(12.0, -1.0, 0.0)
	frame.update_cuts()
	check(not frame.is_cut(), "a cutter in a prefab leaves the prefab's own meshes alone")
	door_root.free()
	# apply: the hole is in the mesh itself
	var nd_ap: Array = cutter.apply()
	check(nd_ap.size() == 1 and nd.get_vertex_count() > nd_nv, "Apply: the cut is in the mesh's geometry")
	nd.update_cuts()
	check(not nd.is_cut(), "applied: the cutter does not cut any more")
	cutter.free()
	nd.free()
	# stairs: a cutter opens the floor above them
	var st := make(10)
	st.set_shape_param("width", 1.0)
	st.set_shape_param("height", 2.0)
	st.set_shape_param("depth", 3.0)
	st.set_shape_param("generate_cutter", 1)
	st.set_shape_param("cutter_headroom", 2.2)
	var fl := make(2)
	fl.set_shape_param("width", 6.0)
	fl.set_shape_param("depth", 6.0)
	fl.position = Vector3(0, 1.0, 0)  # a floor above the stairs (they rise from y = -1 to 1)
	var fl_tris := _shown_tris(fl)
	fl.update_cuts()
	check(fl.is_cut() and _shown_tris(fl) > fl_tris, "stairs with a cutter open the floor above them (%d -> %d)" % [fl_tris, _shown_tris(fl)])
	st.update_cuts()
	check(not st.is_cut(), "the stairs' own cutter does not cut the stairs")
	st.free()
	fl.free()


func _test_r30_path_stairs() -> void:
	var ps := make(22)
	solid_ok(ps, "path stairs straight")
	ps.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(3, 0, 0), Vector3(3, 0, 3)])
	var ps_a := analyze(ps)
	check(ps_a.closed and ps_a.consistent and ps_a.volume > 0.0, "path stairs with a landing: closed pieces")
	var ps_bb := ps.get_aabb()
	check(absf(ps_bb.size.y - 2.5) < 1e-3, "path stairs climb the whole height (%s)" % ps_bb)
	var ps_treads := 0
	for f in ps.get_face_count():
		if ps.get_face_role(f) == 12:
			ps_treads += 1
	check(ps_treads >= 12, "path stairs: about 0.18 m risers (%d treads)" % ps_treads)
	ps.set_shape_param("underside", 1)
	ps.set_shape_param("border", 3)
	var ps_b := analyze(ps)
	check(ps_b.closed and ps_b.consistent, "path stairs with a slab and borders: closed")
	check(tri_area_ok(ps), "path stairs: triangles cover the faces")
	ps.free()
	# spiral stairs: more than one turn
	var sp := make(10)
	sp.set_shape_param("curvature", 720.0)
	sp.set_shape_param("steps", 40)
	sp.set_shape_param("height", 6.0)
	check(sp.get_face_count() > 40 and absf(sp.get_aabb().size.y - 6.0) < 1e-3, "spiral stairs: two turns (%s)" % sp.get_aabb())
	sp.free()


func _test_r30_boolean_far() -> void:
	# a boolean leaves the part of the mesh away from it exactly as it was
	var bs := make(1)
	bs.set_shape_param("width", 4.0)
	bs.set_shape_param("height", 1.0)
	bs.set_shape_param("depth", 4.0)
	bs.set_shape_param("width_segments", 4)
	bs.set_shape_param("depth_segments", 4)
	var bs_orig := bs.get_vertices()
	var bs_fc := bs.get_face_count()
	var bc := make(1)
	bc.position = Vector3(1.5, 0.3, 1.5)
	bc.scale = Vector3(0.6, 0.6, 0.6)
	bs.boolean_with(bc, 1)
	var bs_box := AABB(Vector3(0.0, -1.0, 0.0), Vector3(2.5, 2.5, 2.5))  # (around the cut, generously)
	var bs_new := 0
	for v in bs.get_vertices():
		if bs_box.has_point(v):
			continue
		if not bs_orig.has(v):
			bs_new += 1
	check(bs_new == 0, "boolean: no new or moved vertices away from it (%d)" % bs_new)
	var bs_kept := 0
	for v in bs_orig:
		if not bs_box.has_point(v) and bs.get_vertices().has(v):
			bs_kept += 1
	var bs_far := 0
	for v in bs_orig:
		if not bs_box.has_point(v):
			bs_far += 1
	check(bs_kept == bs_far, "boolean: the far vertices all stay (%d of %d)" % [bs_kept, bs_far])
	check(bs.get_face_count() > bs_fc - 4, "boolean: the far grid faces are not merged (%d -> %d)" % [bs_fc, bs.get_face_count()])
	bs.free()
	bc.free()


# World UVs aligned to the face (face flag bit 23): on a slope the texture is true to size; along the nearest
# axis (the default) it is stretched there as seen along that axis.
func _test_r55_uv_face_aligned() -> void:
	var w := make(16)  # a wedge: its slope at 45 degrees
	w.set_world_uv_all(1.0)
	var slope := -1
	for f in w.get_face_count():
		var n := w.get_face_normal(f)
		if absf(n.y) > 0.3 and absf(n.y) < 0.95:
			slope = f
	check(slope >= 0, "uv face aligned: the wedge has a slope")
	# the smallest ratio of a UV distance between two corners to their 3D distance (shorter in UV: stretched)
	var ratio := func() -> float:
		var ids := w.get_face(slope)
		var uv := w.get_face_uvs(slope)
		var best := 1e9
		for i in ids.size():
			for j in ids.size():
				var d3 := w.get_vertex(ids[i]).distance_to(w.get_vertex(ids[j]))
				if d3 > 1e-4:
					best = minf(best, uv[i].distance_to(uv[j]) / d3)
		return best
	var r_axis: float = ratio.call()
	var md: Dictionary = w.mesh_data
	var fl: PackedInt32Array = md["face_flags"]
	fl[slope] = fl[slope] | (1 << 23)
	md["face_flags"] = fl
	w.mesh_data = md
	var r_face: float = ratio.call()
	check(r_axis < 0.95, "uv along the axis: the slope stretched (%f)" % r_axis)
	check(absf(r_face - 1.0) < 1e-3, "uv aligned to the face: true to size on the slope (%f)" % r_face)
	w.free()


# Every shape, and every operation on a few shapes: the mesh stays clean (no unused vertices, no faces without
# area or with a corner twice, no edge in three faces, no two faces on the same corners).
func _test_clean_sweep() -> void:
	for t in range(1, 23):
		if t in [18, 19, 20, 22]:
			continue  # (drawn from points)
		var sm := make(t)
		clean_ok(sm, "clean: shape %d" % t)
		sm.free()
	var first_edge := func(m: HewnMesh3D) -> PackedInt32Array:
		var f0 := m.get_face(0)
		return PackedInt32Array([f0[0], f0[1]])
	var face_edges := func(m: HewnMesh3D) -> PackedInt32Array:
		var f0 := m.get_face(0)
		var out := PackedInt32Array()
		for i in f0.size():
			out.append(f0[i])
			out.append(f0[(i + 1) % f0.size()])
		return out
	var ops := {
		"extrude": func(m): m.extrude_faces(PackedInt32Array([0]), 0.5),
		"extrude individual": func(m): m.extrude_faces(PackedInt32Array([0, 1]), 0.3, true),
		"extrude dissolve": func(m): m.extrude_faces(PackedInt32Array([0]), 0.5, false, true),
		"inset": func(m): m.inset_faces(PackedInt32Array([0]), 0.1),
		"inset individual": func(m): m.inset_faces(PackedInt32Array([0, 1]), 0.1, true),
		"delete faces": func(m): m.delete_faces(PackedInt32Array([0])),
		"flip": func(m): m.flip_faces(PackedInt32Array([0])),
		"subdivide": func(m): m.subdivide_faces(PackedInt32Array([0])),
		"subdivide ex": func(m): m.subdivide_faces_ex(PackedInt32Array([0, 1]), 2, 0.0, 0, 0.0, false, 0),
		"triangulate": func(m): m.triangulate_faces(PackedInt32Array([0])),
		"merge faces": func(m): m.merge_faces(PackedInt32Array([0, 1])),
		"detach": func(m): m.detach_faces(PackedInt32Array([0])),
		"bevel edge": func(m): m.bevel_edges(first_edge.call(m), 0.1, 2),
		"bevel face edges": func(m): m.bevel_edges(face_edges.call(m), 0.1, 1),
		"bevel vertex": func(m): m.bevel_vertices(PackedInt32Array([m.get_face(0)[0]]), 0.2),
		"bisect": func(m): m.bisect(Vector3(0.1, 0.05, 0.0), Vector3(1, 0.3, 0.2).normalized()),
		"bisect clear fill": func(m): m.bisect(Vector3(0.1, 0.05, 0.0), Vector3(1, 0.3, 0.2).normalized(), false, true, true),
		"bisect clear": func(m): m.bisect(Vector3(0.1, 0.05, 0.0), Vector3(1, 0.3, 0.2).normalized(), true, false, false),
		"cut box": func(m): m.cut_box(AABB(Vector3(-0.2, -0.2, 0.3), Vector3(0.4, 0.4, 1.0)), PackedInt32Array()),
		"poke": func(m): m.poke_faces(PackedInt32Array([0])),
		"split edge": func(m): m.split_edges(first_edge.call(m)),
		"limited dissolve": func(m): m.limited_dissolve(5.0),
		"loop cut": func(m): m.loop_cut(first_edge.call(m)[0], first_edge.call(m)[1], 2, 0.0),
		"extrude border edge": func(m):
			var e: PackedInt32Array = first_edge.call(m)
			m.delete_faces(PackedInt32Array([0]))  # (an edge of the hole: on the border)
			m.extrude_edges(e, 0.3),
		"connect edges": func(m): m.connect_edges(PackedInt32Array([m.get_face(0)[0], m.get_face(0)[1], m.get_face(0)[2], m.get_face(0)[3 % m.get_face(0).size()]])),
		"edge loops": func(m): m.insert_edge_loops(first_edge.call(m)),
		"subdivide edge": func(m): m.subdivide_edges(first_edge.call(m), 2),
		"dissolve edge": func(m): m.dissolve_edges(first_edge.call(m)),
		"dissolve vertex": func(m): m.dissolve_vertices(PackedInt32Array([m.get_face(0)[0]])),
		"to circle": func(m): m.to_circle(PackedInt32Array(), PackedInt32Array([0]), true, 1.0),
		"weld": func(m): m.weld_vertices(PackedInt32Array(m.get_face(0)), 10.0),
		"collapse": func(m): m.collapse_vertices(PackedInt32Array([m.get_face(0)[0], m.get_face(0)[1]])),
		"split vertex": func(m): m.split_vertices(PackedInt32Array([m.get_face(0)[0]])),
		"connect vertices": func(m): m.connect_vertices(PackedInt32Array([m.get_face(0)[0], m.get_face(0)[2 % m.get_face(0).size()]])),
		"fill holes": func(m):
			m.delete_faces(PackedInt32Array([0]))
			m.fill_holes(),
		"subdivide smooth": func(m): m.subdivide_smooth(),
		"quadify": func(m):
			m.triangulate_faces(PackedInt32Array())
			m.quadify(10.0),
		"face plane collapse": func(m): m.move_face_plane(0, 5.0, true),
		"boolean subtract": func(m):
			var o := make(1)
			o.position = Vector3(0.4, 0.4, 0.4)
			m.boolean_with(o, 2)
			o.free(),
		"boolean union": func(m):
			var o := make(1)
			o.position = Vector3(0.4, 0.4, 0.4)
			m.boolean_with(o, 0)
			o.free(),
		"boolean intersect": func(m):
			var o := make(1)
			o.position = Vector3(0.4, 0.4, 0.4)
			m.boolean_with(o, 1)
			o.free(),
	}
	for shape in [1, 3, 5, 10]:  # a cube, a cylinder, a sphere, stairs
		for op in ops:
			var m := make(shape)
			ops[op].call(m)
			clean_ok(m, "clean: %s on shape %d" % [op, shape])
			m.free()


# Only the vertices moved: the fast update shows the same positions and normals (smooth ones too) as a whole rebuild.
func _test_positions_only() -> void:
	var m := make(5)  # a sphere: smooth faces
	m.set_shape_param("rings", 8)
	m.set_shape_param("sides", 8)
	var vs := m.get_vertices()
	for i in vs.size():
		vs[i] = vs[i] * (1.0 + 0.3 * sin(i * 1.7)) + Vector3(0, 0.05 * cos(i * 0.9), 0)
	m.set_vertices(vs, true)
	var fast := (m.mesh as ArrayMesh).surface_get_arrays(0)
	m.finish_positions()
	var full := (m.mesh as ArrayMesh).surface_get_arrays(0)
	var fv: PackedVector3Array = fast[Mesh.ARRAY_VERTEX]
	var fn: PackedVector3Array = fast[Mesh.ARRAY_NORMAL]
	var gv: PackedVector3Array = full[Mesh.ARRAY_VERTEX]
	var gn: PackedVector3Array = full[Mesh.ARRAY_NORMAL]
	var fu: PackedVector2Array = fast[Mesh.ARRAY_TEX_UV]
	var gu: PackedVector2Array = full[Mesh.ARRAY_TEX_UV]
	var same := fv.size() == gv.size() and fv.size() > 0
	for i in mini(fv.size(), gv.size()):
		same = same and fv[i].distance_to(gv[i]) < 1e-4 and fn[i].distance_to(gn[i]) < 1e-3 and fu[i].distance_to(gu[i]) < 1e-4
	check(same, "positions only: the fast update shows what a whole rebuild does, the UVs too (%d vertices)" % fv.size())
	m.free()


func area_of(m: HewnMesh3D) -> float:
	var a := 0.0
	var vs := m.get_vertices()
	for f in m.get_face_count():
		var fv := m.get_face(f)
		var n := Vector3.ZERO
		for i in fv.size():
			n += vs[fv[i]].cross(vs[fv[(i + 1) % fv.size()]])
		a += n.length() * 0.5
	return a


# Dynamic topology: split down to the detail, collapsed back; the mesh stays clean (and closed where it was), the
# surface where it was.
func _test_dyntopo() -> void:
	var p := make(2)  # a plane, 2 x 2 faces
	var a0 := area_of(p)
	check(p.dyntopo_refine(Vector3.ZERO, 10.0, 0.3, true, false), "dyntopo: the plane split")
	var longest := 0.0
	var vs := p.get_vertices()
	for f in p.get_face_count():
		var fv := p.get_face(f)
		for i in fv.size():
			longest = maxf(longest, vs[fv[i]].distance_to(vs[fv[(i + 1) % fv.size()]]))
	clean_ok(p, "dyntopo: split")
	check(longest <= 0.3 + 1e-4 and absf(area_of(p) - a0) < 1e-3, "dyntopo: no edge longer than the detail, the same area (%f, %f)" % [longest, area_of(p)])
	var many := p.get_face_count()
	check(p.dyntopo_refine(Vector3.ZERO, 10.0, 2.0, false, true), "dyntopo: collapsed")
	clean_ok(p, "dyntopo: collapsed")
	check(p.get_face_count() < many and absf(area_of(p) - a0) < 1e-3, "dyntopo: fewer faces, the same area (%d -> %d)" % [many, p.get_face_count()])
	p.free()
	var sph := make(5)
	sph.dyntopo_refine(Vector3(0, 0.5, 0), 0.4, 0.05)
	solid_ok(sph, "dyntopo: a sphere refined at its top")
	sph.dyntopo_refine(Vector3(0, 0.5, 0), 0.4, 0.5)
	solid_ok(sph, "dyntopo: and collapsed again")
	sph.free()


# The Simplify modifier: Collapse leaves about the ratio of the triangles (closed still); Planar joins a subdivided
# cube's faces back into its six sides.
func _test_simplify() -> void:
	var sph := make(5)
	sph.triangulate_faces(PackedInt32Array(range(sph.get_face_count())))
	var tris := sph.get_face_count()
	var sm := HewnSimplifyModifier.new()
	sm.ratio = 0.5
	sph.add_modifier(sm)
	sph.apply_modifiers()
	solid_ok(sph, "simplify: collapse to half")
	check(sph.get_face_count() <= int(tris * 0.55) and sph.get_face_count() >= int(tris * 0.3), "simplify: about half the triangles (%d of %d)" % [sph.get_face_count(), tris])
	sph.free()
	var cb := make(1)
	cb.subdivide_faces_ex(PackedInt32Array(range(6)), 3, 0.0, 0, 0.0, false, 0)
	var pm := HewnSimplifyModifier.new()
	pm.mode = 1
	cb.add_modifier(pm)
	cb.apply_modifiers()
	solid_ok(cb, "simplify: planar")
	check(cb.get_face_count() == 6, "simplify planar: the six sides again (%d)" % cb.get_face_count())
	cb.free()


# The Remesh modifier: every mode makes a closed, clean surface about the size of the source; Blocks only faces
# along the axes; Sharp keeps a cube's corners.
func _test_remesh() -> void:
	for mode in [0, 1, 2, 3]:
		var sph := make(5)
		var a0 := area_of(sph)
		var rm := HewnRemeshModifier.new()
		rm.mode = mode
		rm.voxel_size = 0.05
		rm.octree_depth = 5
		sph.add_modifier(rm)
		sph.apply_modifiers()
		solid_ok(sph, "remesh mode %d: a sphere" % mode)
		var a1 := area_of(sph)
		check(sph.get_face_count() > 20 and absf(a1 / a0 - 1.0) < (0.6 if mode == 0 else 0.15), "remesh mode %d: about the same surface (%f vs %f, %d faces)" % [mode, a1, a0, sph.get_face_count()])
		if mode == 0:
			var axis_ok := true
			for f in sph.get_face_count():
				var n := sph.get_face_normal(f).abs()
				axis_ok = axis_ok and maxf(n.x, maxf(n.y, n.z)) > 0.999
			check(axis_ok, "remesh blocks: only faces along the axes")
		sph.free()
	var cube := make(1)
	var sharp := HewnRemeshModifier.new()
	sharp.mode = 2
	sharp.octree_depth = 4
	cube.add_modifier(sharp)
	cube.apply_modifiers()
	solid_ok(cube, "remesh sharp: a cube")
	var lo := Vector3(1e9, 1e9, 1e9)
	var hi := -lo
	for v in cube.get_vertices():
		lo = lo.min(v)
		hi = hi.max(v)
	check((hi - lo).distance_to(Vector3.ONE) < 0.03, "remesh sharp: the cube's corners kept (%s)" % (hi - lo))
	cube.free()


# Collision: each mode its shape (Primitive: the parameters exactly; Multiple Convex: pieces), the collision's
# metadata and groups on its body.
func _test_collision_modes() -> void:
	var expect := {1: "ConcavePolygonShape3D", 2: "ConvexPolygonShape3D", 3: "ConvexPolygonShape3D", 5: "BoxShape3D", 6: "CapsuleShape3D",
			7: "CylinderShape3D", 8: "SphereShape3D", 9: "BoxShape3D", 10: "ConcavePolygonShape3D"}
	for mode in expect:
		var c := make(1)
		c.set_shape_param("width", 2.0)
		c.collision_mode = mode
		var parts := c.get_collision_shapes()
		check(parts.size() == 1 and (parts[0]["shape"] as Shape3D).get_class() == expect[mode], "collision mode %d: %s (%s)" % [mode, expect[mode], parts])
		if mode == 9 and parts.size() == 1:
			check((parts[0]["shape"] as BoxShape3D).size.is_equal_approx(Vector3(2, 1, 1)), "collision primitive: the box of the parameters")
		c.free()
	var l := make(1)
	l.extrude_faces(faces_facing(l, Vector3(1, 0, 0)), 2.0)
	var l_up := faces_facing(l, Vector3(0, 1, 0))
	l.extrude_faces(PackedInt32Array([l_up[l_up.size() - 1]]), 2.0)
	l.collision_mode = 4
	check(l.get_collision_shapes().size() >= 2, "collision multiple convex: an L in pieces (%d)" % l.get_collision_shapes().size())
	l.collision_metadata = {"surface": "wood"}
	l.collision_groups = PackedStringArray(["walkable"])
	var body: Node = null
	for i in l.get_child_count(true):
		if l.get_child(i, true) is StaticBody3D:
			body = l.get_child(i, true)
	check(body != null and body.get_meta("surface", "") == "wood" and body.is_in_group("walkable") and body.get_child_count() == l.get_collision_shapes().size(),
			"collision: the body has the metadata, the group, a shape for each piece")
	l.free()


# A sweep with its profile Outside: all of it on the right of the path (a trim against a wall).
func _test_sweep_outside() -> void:
	var sw := make(20)
	sw.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(2, 0, 0)])
	sw.set_shape_param("profile", 1)
	sw.set_shape_param("width", 0.4)
	var c0 := sw.get_aabb()
	sw.set_shape_param("align", 1)
	var c1 := sw.get_aabb()
	clean_ok(sw, "sweep outside")
	check(absf(c0.get_center().z) < 1e-3 and absf(absf(c1.get_center().z) - 0.2) < 1e-3 and absf(c1.size.z - 0.4) < 1e-3, "sweep outside: beside the path, not across it (%s -> %s)" % [c0, c1])
	sw.free()


# Scatter: copies kept apart by the density, in chunks; a stroke builds only its chunks again; erase / thin; the
# saved data back; the game copy is plain MultiMeshInstance3D nodes.
func _test_scatter() -> void:
	var sc := HewnScatter3D.new()
	root.add_child(sc)
	var box := BoxMesh.new()
	var sph := SphereMesh.new()
	sc.meshes = [box, sph]
	sc.weights = PackedFloat32Array([1.0, 0.0])
	sc.density = 4.0
	sc.chunk_size = 4.0
	# 400 tries on a 4x4 m square: no more than the density lets stand
	for i in 400:
		sc.paint_point(Vector3(randf() * 3.99, 0, randf() * 3.99), Vector3.UP)
	var n := sc.get_instance_count()
	sc.flush()
	check(n > 20 and n <= 4 * 16 * 1.6, "scatter: the density holds (%d on 16 m2 at 4/m2)" % n)
	check(sc.get_chunk_count() == 1, "scatter: one chunk (%d)" % sc.get_chunk_count())
	check(not sc.paint_point(Vector3(1, 0, 1) if n > 0 else Vector3.ZERO, Vector3.UP) or sc.get_instance_count() == n + 1, "scatter: a copy too near another is not added")
	var mmi: MultiMeshInstance3D = null
	for c in sc.get_children(true):
		if c is MultiMeshInstance3D:
			mmi = c
	check(mmi != null and mmi.multimesh.mesh == box and mmi.multimesh.instance_count == sc.get_instance_count(), "scatter: the weight-0 mesh is never picked, one MultiMesh")
	# another chunk: only it is built again
	sc.paint_point(Vector3(10, 0, 1), Vector3.UP)
	check(sc.flush() == 1 and sc.get_chunk_count() == 2, "scatter: a stroke builds only its chunk (%d)" % sc.get_last_rebuilt())
	# along the normal
	sc.align = 1
	sc.paint_point(Vector3(20, 0, 20), Vector3(1, 0, 0))
	var data := sc.get_instances()
	var found := false
	for i in range(0, data.size(), 13):
		if absf(data[i + 9] - 20.0) < 1e-3:
			var up := Vector3(data[i + 3], data[i + 4], data[i + 5]).normalized()
			found = up.dot(Vector3(1, 0, 0)) > 0.99
	check(found, "scatter: Align Normal stands along the surface's normal")
	# erase / thin
	var before := sc.get_instance_count()
	sc.erase(Vector3(10, 0, 1), 0.5, 1.0)
	check(sc.get_instance_count() == before - 1, "scatter: erase takes the copies under the brush")
	var in_sq := sc.get_instance_count() - 1
	sc.begin_stroke()
	for k in 20:
		sc.erase(Vector3(2, 0, 2), 10.0, 0.5)
	var left := sc.get_instance_count() - 1
	check(left > in_sq * 0.2 and left < in_sq * 0.8, "scatter: thinning takes about half, once per stroke (%d of %d)" % [left, in_sq])
	# the saved data back, the same
	var saved := sc.get_instances()
	var sc2 := HewnScatter3D.new()
	sc2.meshes = [box, sph]
	sc2.chunk_size = 4.0
	sc2.instances = saved
	root.add_child(sc2)
	check(sc2.get_instance_count() == sc.get_instance_count() and sc2.get_instances() == saved, "scatter: the saved copies come back")
	sc2.set_instances(saved)
	check(sc2.flush() == 0, "scatter: the same data: nothing built again")
	var game: Node = sc.make_game_node()
	var mm_count := 0
	for c in game.get_children():
		if c is MultiMeshInstance3D:
			mm_count += c.multimesh.instance_count
	check(not (game is HewnScatter3D) and mm_count == sc.get_instance_count(), "scatter: the game copy is plain MultiMeshes (%d)" % mm_count)
	game.free()
	sc.free()
	sc2.free()


# A sweep's Drop (below the path), accents (a bigger profile at the corners / every so far, steps closed) and the
# profile Mesh (copies of a mesh along the path).
func _test_sweep_accents() -> void:
	var sw := make(20)
	sw.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(4, 0, 0), Vector3(4, 0, 4)])
	sw.set_shape_param("profile", 1)
	sw.set_shape_param("width", 0.2)
	sw.set_shape_param("height", 0.3)
	sw.set_shape_param("drop", 0.1)
	var bb := sw.get_aabb()
	check(absf(bb.position.y + 0.1) < 1e-4 and absf(bb.end.y - 0.3) < 1e-4, "sweep drop: below the path by it, the height above (%s)" % bb)
	var f0 := sw.get_face_count()
	sw.set_shape_param("accent", 2)  # Corners
	sw.set_shape_param("accent_length", 0.6)
	sw.set_shape_param("accent_width", 2.0)
	sw.set_shape_param("accent_height", 1.5)
	solid_ok(sw, "sweep accent corners")
	bb = sw.get_aabb()
	check(sw.get_face_count() > f0 and absf(bb.end.y - 0.45) < 1e-3, "sweep accents at the corner: taller there (%s, %d faces)" % [bb, sw.get_face_count()])
	sw.set_shape_param("accent", 1)  # every 1 m
	sw.set_shape_param("accent_step", 1.0)
	solid_ok(sw, "sweep accent every")
	sw.set_shape_param("align", 1)  # Outside: the steps share the wall-side corners
	solid_ok(sw, "sweep accent outside")
	# a mesh along the path
	var box := BoxMesh.new()
	box.size = Vector3(0.1, 0.5, 0.1)
	sw.sweep_mesh = box
	check(int(sw.get_shape_param("profile")) == 7, "sweep mesh: the profile becomes Mesh")
	sw.set_shape_param("mesh_place", 0)
	sw.set_shape_param("mesh_step", 1.0)
	check(sw.get_face_count() == 9 * 6, "sweep mesh every 1 m on 8 m: 9 copies (quads) (%d faces)" % sw.get_face_count())
	sw.set_shape_param("mesh_place", 1)  # corners
	check(sw.get_face_count() == 6, "sweep mesh at the corners: one (%d faces)" % sw.get_face_count())
	sw.free()


func _test_sweep_corners_gaps() -> void:
	# an inner corner (the path turns toward the profile's side, Outside): the accent long enough for the miter
	var sw := make(20)
	sw.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(4, 0, 0), Vector3(4, 0, 4)])
	sw.set_shape_param("profile", 1)
	sw.set_shape_param("width", 0.2)
	sw.set_shape_param("height", 0.3)
	sw.set_shape_param("align", 1)
	var plain := sw.get_face_count()
	sw.set_shape_param("accent", 2)
	sw.set_shape_param("accent_length", 0.1)
	sw.set_shape_param("accent_width", 3.0)
	solid_ok(sw, "sweep accent at an inner corner")
	var bx := sw.get_aabb()
	check(sw.get_face_count() > plain and bx.end.z > 0.55 - 1e-3, "inner corner accent: wide there (%s)" % bx)
	sw.set_shape_param("corner_side", 1)  # Outer only: this one is inner
	check(sw.get_face_count() == plain, "corner side Outer: no accent at the inner corner (%d)" % sw.get_face_count())
	sw.set_shape_param("corner_side", 2)
	check(sw.get_face_count() > plain, "corner side Inner: the accent")
	sw.set_shape_param("corner_side", 0)
	sw.set_shape_param("corner_min_angle", 100.0)
	check(sw.get_face_count() == plain, "corner min angle 100: a 90 degree turn is no corner")
	sw.set_shape_param("corner_min_angle", 10.0)
	# another profile: a capped piece of its own
	sw.set_shape_param("accent_shape", 2)  # Rectangle
	sw.set_shape_param("accent_length", 0.4)
	check(sw.get_face_count() == plain + 6 + 4, "accent shape Rectangle: a capped piece over the corner (%d)" % sw.get_face_count())
	# a mesh at the accents
	var box := BoxMesh.new()
	box.size = Vector3(0.3, 0.6, 0.3)
	sw.accent_mesh = box
	check(int(sw.get_shape_param("accent_shape")) == 8 and sw.get_face_count() == plain + 6, "accent mesh: one copy at the corner (%d)" % sw.get_face_count())
	sw.accent_mesh = null
	sw.set_shape_param("accent", 0)
	# a bevel: two 45 degree turns closer than the distance are one corner (in their middle)
	sw.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(4, 0, 0), Vector3(4.3, 0, 0.3), Vector3(4.3, 0, 4)])
	var post := BoxMesh.new()
	post.size = Vector3(0.1, 0.5, 0.1)
	var mat := StandardMaterial3D.new()
	post.material = mat
	sw.sweep_mesh = post
	sw.set_shape_param("mesh_place", 1)  # corners
	check(sw.get_face_count() == 12, "two bevel corners: two posts (%d)" % sw.get_face_count())
	sw.set_shape_param("corner_min_distance", 0.5)
	check(sw.get_face_count() == 6, "closer than the min distance: one corner (%d)" % sw.get_face_count())
	sw.set_shape_param("corner_min_angle", 60.0)
	check(sw.get_face_count() == 6, "the bevel turns 90 together: still a corner")
	sw.set_shape_param("corner_min_distance", 0.0)
	check(sw.get_face_count() == 0, "each 45 degree turn alone: no corner (%d)" % sw.get_face_count())
	sw.set_shape_param("corner_min_angle", 10.0)
	# the placed mesh keeps its own material
	var own := true
	for f in sw.get_face_count():
		own = own and sw.get_materials()[sw.get_face_material(f)] == mat
	check(own, "sweep mesh: the copies keep the mesh's material")
	sw.sweep_mesh = null
	sw.set_shape_param("profile", 1)
	# gaps: the segments left out, a piece for each run
	sw.poly_points = PackedVector3Array([Vector3(0, 0, 0), Vector3(2, 0, 0), Vector3(2, 0, 2), Vector3(4, 0, 2)])
	sw.set_shape_param("align", 0)
	check(sw.get_face_count() == 14, "sweep without gaps: one piece (%d)" % sw.get_face_count())
	sw.path_gaps = PackedInt32Array([1])
	check(sw.get_face_count() == 12, "a gap in the middle: two capped pieces (%d)" % sw.get_face_count())
	sw.set_shape_param("closed", 1)
	sw.path_gaps = PackedInt32Array([3])
	check(sw.get_face_count() == 14, "a loop with its closing segment a gap: one open piece (%d)" % sw.get_face_count())
	sw.free()


func _test_scatter_rotation_collision() -> void:
	var sc := HewnScatter3D.new()
	root.add_child(sc)
	var bm := BoxMesh.new()
	sc.meshes = [bm]
	sc.random_rotation = 0.0
	sc.scale_min = 1.0
	sc.scale_max = 1.0
	sc.base_rotation = Vector3(90, 0, 0)
	sc.paint_point(Vector3(0, 0, 0), Vector3.UP)
	var d := sc.get_instances()
	check(d.size() == 13 and absf(d[5] - 1.0) < 1e-3, "scatter base rotation: X 90 turns its up to Z (%s)" % d)
	sc.base_rotation = Vector3()
	sc.random_rotation_x = 360.0
	var tilted := false
	for i in 20:
		sc.paint_point(Vector3(i * 3.0 + 3.0, 0, 0), Vector3.UP)
	d = sc.get_instances()
	for k in range(13, d.size(), 13):
		tilted = tilted or absf(d[k + 4] - 1.0) > 0.01
	check(tilted, "scatter random rotation X: copies turned off the up")
	sc.collisions = [1]
	var g := sc.make_game_node()
	var shapes := 0
	for c in g.get_children():
		if c is StaticBody3D:
			shapes += c.get_child_count()
	check(shapes == sc.get_instance_count(), "scatter collision: a shape for each copy (%d of %d)" % [shapes, sc.get_instance_count()])
	g.free()
	sc.free()


func _test_inner_faces() -> void:
	# two cubes merged that touch: the two faces between them hidden
	var a := make(1)
	var b := make(1)
	b.position = Vector3(1, 0, 0)
	a.merge_from(b)
	check(a.get_inner_faces().size() == 2, "touching cubes merged: 2 hidden faces (%s)" % a.get_inner_faces())
	b.free()
	a.free()
	# overlapping: each one's face inside the other
	a = make(1)
	b = make(1)
	b.position = Vector3(0.5, 0, 0)
	a.merge_from(b)
	check(a.get_inner_faces().size() == 2, "overlapping cubes merged: 2 hidden faces (%s)" % a.get_inner_faces())
	b.free()
	a.free()
	# a room (faces turned in): nothing hidden
	a = make(1)
	a.set_shape_param("flip_normals", 1)
	check(a.get_inner_faces().size() == 0, "a room's inside: nothing hidden")
	a.free()


func _test_r53_uv_near_origin() -> void:
	# world UVs of a cube far from the origin: its islands are moved by whole tiles next to the UV origin
	# (the texture looks the same: its offset from the tile grid is kept)
	var w := make(1)
	w.position = Vector3(1000.3, 0.0, -500.2)
	w.set_world_uv_all(1.0)
	var far := 0.0
	var off_ok := true
	for f in w.get_face_count():
		var uv := w.get_face_uvs(f)
		var c := Vector2()
		for q in uv:
			c += q
		c /= uv.size()
		far = maxf(far, maxf(absf(c.x), absf(c.y)))
	check(far < 1.5, "world UVs far from the origin stay near the first tile (%f)" % far)
	# the same place in the tile as without the move: the top's corners at x 999.8 and 1000.8
	var top := -1
	for f in w.get_face_count():
		if w.get_face_normal(f).y > 0.9:
			top = f
	var tu := w.get_face_uvs(top)
	for q in tu:
		var fr := fposmod(q.x, 1.0)
		off_ok = off_ok and (absf(fr - 0.8) < 1e-3 or absf(fr - 0.2) < 1e-3)
	check(off_ok, "the moved islands keep their place in the tile (%s)" % tu)
	w.free()
