@tool
extends Node
# Editor timing for Hewn on a heavy scene: the Hewn plugin runs it with HW_PERF=1 (not part of the addon):
#   HW_PERF=1 godot --headless --editor --path project
# Builds a scene of many objects and a few dense meshes, times what the editor does with them, prints the times
# and quits. Nothing is saved (the scene file is the test's own).

var surf: Control
var cam: Camera3D
var results := []


func _enter_tree() -> void:
	_run.call_deferred()


func frames(n: int) -> void:
	for i in n:
		await get_tree().process_frame


func surface_of(vp: SubViewport) -> Control:
	var holder := vp.get_parent().get_parent()
	for c in holder.get_children():
		if c.get_class() == "Control":
			return c
	return null


func send(ev: InputEvent) -> void:
	Input.parse_input_event(ev)
	Input.flush_buffered_events()
	await frames(1)


func key(code: Key) -> void:
	surf.grab_focus()
	for pressed in [true, false]:
		var k := InputEventKey.new()
		k.physical_keycode = code
		k.pressed = pressed
		await send(k)


func move(pos: Vector2) -> void:
	var m := InputEventMouseMotion.new()
	m.position = pos
	m.global_position = surf.get_global_rect().position + pos
	surf.gui_input.emit(m)
	await frames(1)


# The time of n frames, ms per frame.
func frame_ms(n: int) -> float:
	var t := Time.get_ticks_usec()
	await frames(n)
	return (Time.get_ticks_usec() - t) / 1000.0 / n


func report(what: String, ms: float) -> void:
	results.append([what, ms])
	print("PERF %-58s %9.2f ms" % [what, ms])


func _run() -> void:
	# (no sleeping between frames: a frame's time is the work done in it)
	OS.low_processor_usage_mode = false
	OS.low_processor_usage_mode_sleep_usec = 0
	Engine.max_fps = 0
	await frames(10)
	EditorInterface.set_main_screen_editor("3D")
	var root := Node3D.new()
	root.name = "PerfTest"
	var ps := PackedScene.new()
	ps.pack(root)
	ResourceSaver.save(ps, "res://tests/perf_scene.tscn")
	root.free()
	EditorInterface.open_scene_from_path("res://tests/perf_scene.tscn")
	await frames(10)
	var scene := EditorInterface.get_edited_scene_root()
	var vp := EditorInterface.get_editor_viewport_3d(0)
	cam = vp.get_camera_3d()
	surf = surface_of(vp)
	get_tree().root.propagate_notification(NOTIFICATION_WM_MOUSE_ENTER)
	get_tree().root.propagate_notification(NOTIFICATION_VP_MOUSE_ENTER)
	report("idle frame, empty scene", await frame_ms(60))

	# ---- the heavy scene: a grid of 600 objects (boxes, cylinders, stairs, spheres), and two dense meshes
	var t := Time.get_ticks_usec()
	var shapes := [1, 3, 10, 5]
	var all := []
	for i in 600:
		var m := HewnMesh3D.new()
		m.name = "P%d" % i
		m.shape = shapes[i % shapes.size()]
		m.position = Vector3((i % 25) * 3.0, 0, (i / 25) * 3.0)
		scene.add_child(m)
		m.owner = scene
		all.append(m)
	report("add 600 objects", (Time.get_ticks_usec() - t) / 1000.0)
	await frames(5)
	var dense := HewnMesh3D.new()
	dense.name = "Dense"
	dense.position = Vector3(-10, 0, 0)
	scene.add_child(dense)
	dense.owner = scene
	dense.shape = 1
	dense.subdivide_faces_ex(PackedInt32Array([0, 1, 2, 3, 4, 5]), int(OS.get_environment("HW_PERF_CUTS")) if OS.get_environment("HW_PERF_CUTS") != "" else 80, 0.0, 0, 0.0, false, 0)
	print("PERF dense mesh: %d faces" % dense.get_face_count())
	await frames(5)
	report("idle frame, 600 objects + dense, object mode", await frame_ms(120))

	# ---- selection
	t = Time.get_ticks_usec()
	EditorInterface.get_selection().clear()
	for m in all:
		EditorInterface.get_selection().add_node(m)
	await frames(1)
	report("select 600 objects (one by one) + a frame", (Time.get_ticks_usec() - t) / 1000.0)
	report("idle frame, 600 selected", await frame_ms(60))
	EditorInterface.get_selection().clear()
	await frames(2)

	# ---- shading views over the whole scene
	var sh: PopupPanel = EditorInterface.get_base_control().find_child("HewnShadingPopup", true, false)
	for v in [4, 5, 3, 0]:
		t = Time.get_ticks_usec()
		sh.find_child("Shading%d" % v, true, false).pressed.emit()
		await frames(1)
		report("shading view %d on 601 objects + a frame" % v, (Time.get_ticks_usec() - t) / 1000.0)
		if v == 4:
			report("idle frame, Single Color", await frame_ms(60))

	# ---- the dense mesh in edit mode
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(dense)
	await frames(3)
	t = Time.get_ticks_usec()
	await key(KEY_TAB)
	report("Tab into edit mode (dense) + a frame", (Time.get_ticks_usec() - t) / 1000.0)
	report("idle frame, edit mode dense", await frame_ms(60))
	var c := cam.unproject_position(dense.global_position)
	for mode_key in [KEY_1, KEY_2, KEY_3]:
		await key(mode_key)
		t = Time.get_ticks_usec()
		for i in 30:
			await move(c + Vector2(i * 3 - 45, (i % 7) * 4 - 12))
		report("hover move, mode %d (per move + frame)" % (mode_key - KEY_0), (Time.get_ticks_usec() - t) / 1000.0 / 30)
	# select all, G and mouse moves (each one moves every vertex)
	await key(KEY_A)
	await frames(2)
	await key(KEY_G)
	t = Time.get_ticks_usec()
	for i in 20:
		await move(c + Vector2(i * 4, 0))
	report("G with all selected: per mouse move + frame", (Time.get_ticks_usec() - t) / 1000.0 / 20)
	t = Time.get_ticks_usec()
	await key(KEY_ESCAPE)
	report("Esc (cancel G)", (Time.get_ticks_usec() - t) / 1000.0)
	await key(KEY_TAB)
	await frames(3)

	# ---- one simple object in edit mode among the 600
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(all[0])
	await frames(3)
	await key(KEY_TAB)
	report("idle frame, edit mode on a cube among 600", await frame_ms(60))
	var c0 := cam.unproject_position(all[0].global_position)
	t = Time.get_ticks_usec()
	for i in 30:
		await move(c0 + Vector2(i * 3 - 45, 0))
	report("hover move on the cube (per move + frame)", (Time.get_ticks_usec() - t) / 1000.0 / 30)
	await key(KEY_TAB)
	await frames(2)

	# ---- object mode: hover over the scene (Shift: the face outline)
	t = Time.get_ticks_usec()
	for i in 30:
		await move(Vector2(surf.size.x * 0.5 + i * 5, surf.size.y * 0.5))
	report("hover move, object mode (per move + frame)", (Time.get_ticks_usec() - t) / 1000.0 / 30)

	# ---- Sculpt on the dense mesh: a stroke, then the same with the dynamic topology
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(dense)
	await frames(3)
	var im: MenuButton = EditorInterface.get_base_control().find_child("InteractionMode", true, false)
	im.get_popup().id_pressed.emit(3)
	await frames(3)
	# (in front of the camera, so the brush is on it)
	dense.global_position = cam.global_position - cam.global_transform.basis.z * 3.0
	await frames(2)
	var dc := cam.unproject_position(dense.global_position)
	print("PERF dense on the screen at %s (viewport %s)" % [dc, surf.size])
	for dyn in [false, true]:
		var dcb: CheckBox = EditorInterface.get_base_control().find_child("Dyntopo", true, false)
		dcb.button_pressed = dyn
		await frames(2)
		var o := surf.get_global_rect().position
		var pb := InputEventMouseButton.new()
		pb.button_index = MOUSE_BUTTON_LEFT
		pb.pressed = true
		pb.position = o + dc
		pb.global_position = pb.position
		await send(pb)
		t = Time.get_ticks_usec()
		for i in 20:
			var mv := InputEventMouseMotion.new()
			mv.position = dc + Vector2(i * 5, 0)
			mv.global_position = o + mv.position
			mv.button_mask = MOUSE_BUTTON_MASK_LEFT
			surf.gui_input.emit(mv)
			await frames(1)
		report("sculpt stroke%s: per mouse move + frame" % (" (dynamic topology)" if dyn else ""), (Time.get_ticks_usec() - t) / 1000.0 / 20)
		var rb := InputEventMouseButton.new()
		rb.button_index = MOUSE_BUTTON_LEFT
		rb.pressed = false
		rb.position = o + dc + Vector2(100, 0)
		rb.global_position = rb.position
		t = Time.get_ticks_usec()
		await send(rb)
		report("sculpt stroke end (the whole mesh made again)", (Time.get_ticks_usec() - t) / 1000.0)
		print("PERF faces now %d" % dense.get_face_count())
	await key(KEY_TAB)

	print("PERF done")
	get_tree().quit()
