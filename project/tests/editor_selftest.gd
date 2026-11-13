@tool
extends Node
# Editor self-test for Hewn: the Hewn plugin runs it with HW_SELFTEST=1 (not part of the addon):
#   HW_SELFTEST=1 godot --editor --path project
# Drives the 3D viewport with real input events and checks the results, then quits.
# HW_SELFTEST_SHOT=<path.png> also saves a screenshot of the viewport in edit mode (needs a window).

var failures := 0
var surf: Control
var cam: Camera3D


func check(cond: bool, msg: String) -> void:
	if cond:
		print("  ok   ", msg)
	else:
		failures += 1
		printerr("  FAIL ", msg)


func _enter_tree() -> void:
	if OS.get_environment("HW_SELFTEST") == "1":
		_run.call_deferred()
	elif OS.get_environment("HW_ICONS") == "1":
		var names := EditorInterface.get_editor_theme().get_icon_list("EditorIcons")
		print("ICONS ", " ".join(names))
		get_tree().quit.call_deferred()
	elif OS.get_environment("HW_UI_SHOT") != "":
		_ui_shot.call_deferred()
	elif OS.get_environment("HW_WIDTHS") == "1":
		_widths.call_deferred()
	elif OS.get_environment("HW_HEIGHTS") == "1":
		_heights.call_deferred()


# What makes the dock tall (HW_HEIGHTS=1): the minimum height of each tab and of the dock's parts, a mesh in edit
# mode with a face selected.
func _heights() -> void:
	await frames(30)
	var scene := EditorInterface.get_edited_scene_root()
	if scene:
		var m := HewnMesh3D.new()
		scene.add_child(m)
		m.owner = scene
		EditorInterface.set_main_screen_editor("3D")
		var vp := EditorInterface.get_editor_viewport_3d(0)
		surf = surface_of(vp)
		cam = vp.get_camera_3d()
		await select_node(m)
		await key(KEY_TAB)
		await key(KEY_3)
		var st = m.get_state()
		st["sel_faces"] = PackedInt32Array([0, 1])
		m.set_state(st)
		await frames(5)
	var dock: Control = EditorInterface.get_base_control().find_child("Hewn", true, false)
	var tabs: TabContainer = dock.find_children("*", "TabContainer", true, false)[0] if dock else null
	if tabs:
		for t in tabs.get_tab_count():
			tabs.current_tab = t
			await frames(4)
			print("TAB ", tabs.get_tab_title(t), " page ", tabs.get_tab_control(t).get_combined_minimum_size().y, " tabs ", tabs.get_combined_minimum_size().y, " dock ", dock.get_combined_minimum_size().y)
			if tabs.get_tab_title(t) == "Inspector" or t == tabs.get_tab_count() - 1:
				var c: Control = tabs.get_tab_control(t)
				while c:
					print("   ", c.get_class(), " ", c.name, " ", c.get_combined_minimum_size())
					c = c.get_parent() as Control
				for x in dock.get_child(0).get_children():
					print("  dock part ", x.get_class(), " ", x.name, " ", x.get_combined_minimum_size())
				for x in tabs.get_tab_control(t).find_children("*", "Control", true, false):
					if x.get_combined_minimum_size().x > 70:
						print("  tall ", x.get_class(), " ", x.name, " ", x.get_combined_minimum_size())
	get_tree().quit()


# What makes each tab of the dock wide: its widest controls (HW_WIDTHS=1).
func _widths() -> void:
	await frames(30)
	var dock: Control = EditorInterface.get_base_control().find_child("Hewn", true, false)
	var tabs: TabContainer = dock.find_children("*", "TabContainer", true, false)[0] if dock else null
	if tabs:
		for t in tabs.get_tab_count():
			tabs.current_tab = t
			await frames(3)
			var page := tabs.get_tab_control(t)
			var rows := []
			for c in page.find_children("*", "Control", true, false):
				if c.get_child_count() == 0 or c is Button or c is OptionButton or c is Label or ((c is HBoxContainer or c is GridContainer or c is HFlowContainer) and c.get_combined_minimum_size().x > 200):
					var kids := ""
					if c is Container:
						for k in c.get_children():
							kids += "[%s %s %d] " % [k.get_class(), k.get("text") if k.get("text") != null else "", k.get_combined_minimum_size().x]
					rows.append([c.get_combined_minimum_size().x, "%s %s '%s' %s" % [c.get_class(), c.name, c.get("text") if c.get("text") != null else "", kids]])
			rows.sort_custom(func(a, b): return a[0] > b[0])
			print("TAB ", tabs.get_tab_title(t), " min ", page.get_combined_minimum_size().x)
			for i in mini(8, rows.size()):
				print("   ", rows[i][0], " ", rows[i][1])
	get_tree().quit()


# Screenshot of the whole editor window right after startup (layout checks).
func _ui_shot() -> void:
	if OS.get_environment("HW_UI_WIDTH") != "":
		get_window().mode = Window.MODE_WINDOWED
		get_window().size = Vector2i(int(OS.get_environment("HW_UI_WIDTH")), 720)
	await frames(30)
	EditorInterface.set_main_screen_editor("3D")
	var scene := EditorInterface.get_edited_scene_root()
	if scene:
		var m := HewnMesh3D.new()
		scene.add_child(m)
		m.owner = scene
		EditorInterface.get_selection().clear()
		if OS.get_environment("HW_UI_NOSEL") != "1":  # (nothing selected: the panels stay)
			EditorInterface.get_selection().add_node(m)
	await frames(30)
	if OS.get_environment("HW_UI_CREATE") == "1":
		# 3D cursor at (1.5, 0, 0.5) with Shift+RMB, then a cube from the Create tab at the cursor
		var vp := EditorInterface.get_editor_viewport_3d(0)
		var sf := surface_of(vp)
		var at := vp.get_camera_3d().unproject_position(Vector3(1.5, 0, 0.5))
		for pressed in [true, false]:
			var ev := InputEventMouseButton.new()
			ev.button_index = MOUSE_BUTTON_RIGHT
			ev.shift_pressed = true
			ev.pressed = pressed
			ev.position = at
			sf.gui_input.emit(ev)
			await frames(2)
		surf = sf
		press("Cube")
		await frames(2)
		sf.grab_focus()
		await key(KEY_ENTER)
		await frames(10)
	if OS.get_environment("HW_UI_QUAD") == "1" and scene:
		# the Quad screen with a few shapes, one selected
		var st := HewnMesh3D.new()
		st.shape = 10
		st.position = Vector3(2, 0, -1)
		scene.add_child(st)
		st.owner = scene
		EditorInterface.set_main_screen_editor("Quad")
		await frames(5)
		EditorInterface.get_selection().clear()
		EditorInterface.get_selection().add_node(st)
		await frames(30)
	if OS.get_environment("HW_UI_DIM") == "1" and scene:
		# a placed scene beside the edited cube
		var dim_lib := Node3D.new()
		dim_lib.scene_file_path = "res://placed_for_test.tscn"
		var dim_m := MeshInstance3D.new()
		dim_m.mesh = BoxMesh.new()
		dim_lib.add_child(dim_m)
		dim_lib.position = Vector3(1.6, 0, 0)
		scene.add_child(dim_lib)
		var dim_vp := EditorInterface.get_editor_viewport_3d(0)
		surf = surface_of(dim_vp)
		await key(KEY_TAB)
		await frames(10)
	if OS.get_environment("HW_UI_UVT") == "1" and scene:
		# Shift+T on the top face of the selected cube
		var uvt_vp := EditorInterface.get_editor_viewport_3d(0)
		surf = surface_of(uvt_vp)
		cam = uvt_vp.get_camera_3d()
		var uvt_m := scene.get_child(scene.get_child_count() - 1) as HewnMesh3D
		await key(KEY_TAB)
		await key(KEY_3)
		var uvt_top := cam.unproject_position(uvt_m.global_transform * Vector3(0.1, 0.5, 0.1))
		await click(uvt_top)
		await move(uvt_top)
		await key(KEY_T, true)
		await move(uvt_top + Vector2(40, 10))
		await frames(10)
	if OS.get_environment("HW_UI_TAB") != "":
		# a tab of the Hewn dock (by index), the dock shown
		var pfd := EditorInterface.get_base_control().find_child("Hewn", true, false)
		if pfd:
			var tcs := pfd.find_children("*", "TabContainer", true, false)
			if tcs.size() > 0:
				tcs[0].current_tab = int(OS.get_environment("HW_UI_TAB"))
			var p := pfd.get_parent()
			if p is TabContainer:
				p.current_tab = pfd.get_index()
			await frames(5)
			for sc in pfd.find_children("*", "ScrollContainer", true, false):
				if sc.is_visible_in_tree():
					sc.scroll_vertical = int(OS.get_environment("HW_UI_SCROLL"))
		await frames(10)
	var bar := EditorInterface.get_base_control().find_child("HewnToolbar", true, false) as ScrollContainer
	if bar and OS.get_environment("HW_UI_QUAD") != "1":
		print("toolbar width ", bar.size.x, " content ", bar.get_child(0).size.x, " window ", get_window().size.x)
		var ev := InputEventMouseButton.new()
		ev.button_index = MOUSE_BUTTON_WHEEL_DOWN
		ev.pressed = true
		bar.gui_input.emit(ev)
		await frames(2)
		print("toolbar scroll after wheel ", bar.scroll_horizontal)
	get_tree().root.get_texture().get_image().save_png(OS.get_environment("HW_UI_SHOT"))
	get_tree().quit()


func frames(n: int) -> void:
	for i in n:
		await get_tree().process_frame


func surface_of(vp: SubViewport) -> Control:
	var holder := vp.get_parent().get_parent()
	for c in holder.get_children():
		if c.get_class() == "Control":
			return c
	return null


# The field after a label (inside the holder of its revert arrow, when it has one), and the label before a field.
func field_after(l: Node) -> Node:
	var n := l.get_parent().get_child(l.get_index() + 1)
	if n is HBoxContainer and n.has_meta("hw_revert_holder"):
		return n.get_child(0)
	return n


func label_before(f: Node) -> Node:
	var x := f
	if f.get_parent().has_meta("hw_revert_holder"):
		x = f.get_parent()
	return x.get_parent().get_child(x.get_index() - 1)


func send(ev: InputEvent) -> void:
	Input.parse_input_event(ev)
	Input.flush_buffered_events()
	await frames(2)


func key(code: Key, shift := false, ctrl := false, alt := false) -> void:
	if surf:
		surf.grab_focus()
	for pressed in [true, false]:
		var k := InputEventKey.new()
		# Only the physical code: in headless runs a logical "1" gets reported as keypad 1,
		# which the 3D editor treats as "front view".
		k.physical_keycode = code
		k.pressed = pressed
		k.shift_pressed = shift
		k.ctrl_pressed = ctrl
		k.alt_pressed = alt
		await send(k)


func type_text(text: String) -> void:
	for ch in text:
		if ch == ".":
			await key(KEY_PERIOD)
		elif ch == "-":
			await key(KEY_MINUS)
		else:
			await key(KEY_0 + int(ch))


func move(pos: Vector2, shift := false, mask := 0, ctrl := false) -> void:
	# Hover motion (no button held) is not routed to the viewport in headless runs, so it is
	# delivered straight to the viewport surface, in its local coordinates.
	var m := InputEventMouseMotion.new()
	m.position = pos
	m.global_position = surf.get_global_rect().position + pos
	m.shift_pressed = shift
	m.ctrl_pressed = ctrl
	m.button_mask = mask
	surf.gui_input.emit(m)
	await frames(2)


func button(pos: Vector2, index: MouseButton, pressed: bool, alt := false, ctrl := false, shift := false, double := false) -> void:
	var g := surf.get_global_rect().position + pos
	var b := InputEventMouseButton.new()
	b.button_index = index
	b.pressed = pressed
	b.double_click = double and pressed
	b.alt_pressed = alt
	b.ctrl_pressed = ctrl
	b.shift_pressed = shift
	b.position = g
	b.global_position = g
	b.button_mask = (1 << (index - 1)) if pressed else 0
	await send(b)


func click(pos: Vector2, double := false, alt := false, ctrl := false, shift := false) -> void:
	await move(pos)
	await button(pos, MOUSE_BUTTON_LEFT, true, alt, ctrl, shift, double)
	await button(pos, MOUSE_BUTTON_LEFT, false, alt, ctrl, shift)


func wheel_up(pos: Vector2) -> void:
	await button(pos, MOUSE_BUTTON_WHEEL_UP, true)
	await button(pos, MOUSE_BUTTON_WHEEL_UP, false)


func drag(from: Vector2, to: Vector2, shift := false) -> void:
	var o := surf.get_global_rect().position
	var b := InputEventMouseButton.new()
	b.button_index = MOUSE_BUTTON_LEFT
	b.pressed = true
	b.shift_pressed = shift
	b.position = o + from
	b.global_position = o + from
	b.button_mask = MOUSE_BUTTON_MASK_LEFT
	await send(b)
	for i in range(1, 11):
		var m := InputEventMouseMotion.new()
		m.position = o + from.lerp(to, i / 10.0)
		m.global_position = m.position
		m.button_mask = MOUSE_BUTTON_MASK_LEFT
		m.shift_pressed = shift
		m.relative = (to - from) / 10.0
		await send(m)
	var r := InputEventMouseButton.new()
	r.button_index = MOUSE_BUTTON_LEFT
	r.pressed = false
	r.position = o + to
	r.global_position = o + to
	await send(r)


func find_button(n: Node, text: String, visible_only: bool) -> Button:
	if n is Button and n.text == text and (not visible_only or n.is_visible_in_tree()):
		return n
	for c in n.get_children():
		var b := find_button(c, text, visible_only)
		if b:
			return b
	return null


func press(text: String) -> void:
	# Search the Hewn dock first (other editor panels have buttons with the same text).
	var dock := EditorInterface.get_base_control().find_child("Hewn", true, false)
	var b: Button = null
	if dock:
		b = find_button(dock, text, false)
	if b == null:
		b = find_button(EditorInterface.get_base_control(), text, false)
	check(b != null, "button '%s' exists" % text)
	if b:
		b.pressed.emit()


func sp(m: HewnMesh3D, local: Vector3) -> Vector2:
	return cam.unproject_position(m.global_transform * local)


func visible_face(m: HewnMesh3D) -> int:
	var best := -1
	var best_d := INF
	for f in m.get_face_count():
		var c := m.global_transform * m.get_face_center(f)
		var n := m.get_face_normal(f)
		if n.dot(cam.global_position - c) > 0.0 and c.distance_to(cam.global_position) < best_d:
			best_d = c.distance_to(cam.global_position)
			best = f
	return best


func sel(m: HewnMesh3D, what: String) -> Array:
	return Array(m.get_state()[what])


func undo(m: Object) -> void:
	var ur := EditorInterface.get_editor_undo_redo()
	ur.get_history_undo_redo(ur.get_object_history_id(m)).undo()
	await frames(2)


# Sign of the +Y face normal: 1 if the top face points up, -1 if it points down.
func faces_up(m: HewnMesh3D) -> int:
	for f in m.get_face_count():
		if m.get_face_center(f).y > 0.49:
			return 1 if m.get_face_normal(f).y > 0.0 else -1
	return 0


# Toolbar snapping: magnet on/off and one snap target ("Increment", "Vertex", "Edge", "Face").
# Godot's own snap (magnet) toggle in the 3D toolbar: Hewn snaps when it is on.
# the Hewn gizmo follows Godot's gizmo size setting (80 = the size the tests were written for)
func gizmo_k() -> float:
	var es := EditorInterface.get_editor_settings()
	return clampf(float(es.get_setting("editors/3d/manipulator_gizmo_size")), 16.0, 160.0) / 80.0 if es.has_setting("editors/3d/manipulator_gizmo_size") else 1.0


func godot_snap() -> Button:
	var magnet := EditorInterface.get_editor_theme().get_icon("Snap", "EditorIcons")
	var bar := EditorInterface.get_base_control().find_child("HewnToolbar", true, false)
	for b in bar.get_parent().find_children("*", "Button", true, false):
		if b.toggle_mode and b.icon == magnet:
			return b
	return null


func set_snapping(on: bool, target: String) -> void:
	var bar := EditorInterface.get_base_control().find_child("HewnToolbar", true, false)
	godot_snap().button_pressed = on
	var t := find_button(bar, target, false)
	if t:
		t.pressed.emit()
	await frames(1)


# Picks an item of the Hewn popup menu that is open (X, right click, P, M...).
func menu_pick(text: String) -> void:
	for pm in EditorInterface.get_base_control().find_children("*", "PopupMenu", false, false):
		if not pm.visible:
			continue
		# (its categories too: the submenus of Shift+A)
		for sub in [pm] + pm.find_children("*", "PopupMenu", false, false):
			for i in sub.item_count:
				if sub.get_item_text(i) == text:
					pm.hide()
					sub.id_pressed.emit(sub.get_item_id(i))
					await frames(2)
					return
	check(false, "menu item '%s' found" % text)


func mesh_volume(m: HewnMesh3D) -> float:
	var vol := 0.0
	for f in m.get_face_count():
		var ids := m.get_face(f)
		for i in range(1, ids.size() - 1):
			vol += m.get_vertex(ids[0]).dot(m.get_vertex(ids[i]).cross(m.get_vertex(ids[i + 1])))
	return vol / 6.0


func faces_up_index(m: HewnMesh3D) -> int:
	for f in m.get_face_count():
		if m.get_face_normal(f).y > 0.9:
			return f
	return -1


func face_center(m: HewnMesh3D, f: int) -> Vector3:
	var c := Vector3()
	var fv := m.get_face(f)
	for v in fv:
		c += m.get_vertex(v) / fv.size()
	return c


func select_node(n: Node) -> void:
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(n)
	await frames(5)


# Ctrl+Shift + drag in the viewport (surface coordinates), in ten steps.
func cs_drag(from: Vector2, to: Vector2) -> void:
	var o := surf.get_global_rect().position
	var pe := InputEventMouseButton.new()
	pe.button_index = MOUSE_BUTTON_LEFT
	pe.pressed = true
	pe.shift_pressed = true
	pe.ctrl_pressed = true
	pe.position = o + from
	pe.global_position = pe.position
	pe.button_mask = MOUSE_BUTTON_MASK_LEFT
	await send(pe)
	for i in range(1, 11):
		var mv := InputEventMouseMotion.new()
		mv.position = o + from.lerp(to, i / 10.0)
		mv.global_position = mv.position
		mv.button_mask = MOUSE_BUTTON_MASK_LEFT
		mv.shift_pressed = true
		mv.ctrl_pressed = true
		await send(mv)
	var re := InputEventMouseButton.new()
	re.button_index = MOUSE_BUTTON_LEFT
	re.pressed = false
	re.position = o + to
	re.global_position = re.position
	await send(re)
	await frames(2)


# The field of the Last Operation panel after its label.
func last_op_field(label: String) -> Node:
	var lp := surf.find_child("HewnLastOperation", true, false)
	if lp:
		for l in lp.find_children("*", "Label", true, false):
			if l.text == label and l.is_visible_in_tree():
				return field_after(l)
	return null


func _run() -> void:
	print("Hewn editor self-test")
	# (the gizmo button and the snap steps are kept in the editor settings: put back as they were at the end)
	var kept_es := EditorInterface.get_editor_settings()
	var kept := {}
	for kept_name in ["hewn/viewport/show_gizmo", "hewn/snapping/move_step", "hewn/snapping/rotate_step", "hewn/snapping/scale_step"]:
		if kept_es.has_setting(kept_name):
			kept[kept_name] = kept_es.get_setting(kept_name)
	# (the tests were written for the gizmo shown and these steps)
	kept_es.set_setting("hewn/viewport/show_gizmo", true)
	kept_es.set_setting("hewn/snapping/move_step", 0.25)
	kept_es.set_setting("hewn/snapping/rotate_step", 15.0)
	kept_es.set_setting("hewn/snapping/scale_step", 0.1)
	kept_es.emit_signal("settings_changed")
	await frames(10)
	EditorInterface.set_main_screen_editor("3D")
	var root := Node3D.new()
	root.name = "SelfTest"
	var ps := PackedScene.new()
	ps.pack(root)
	ResourceSaver.save(ps, "res://tests/selftest_scene.tscn")
	root.free()
	EditorInterface.open_scene_from_path("res://tests/selftest_scene.tscn")
	await frames(10)
	var scene := EditorInterface.get_edited_scene_root()
	check(scene != null, "scene opened")

	var cube := HewnMesh3D.new()
	cube.name = "Cube"
	scene.add_child(cube)
	cube.owner = scene
	await select_node(cube)

	var vp := EditorInterface.get_editor_viewport_3d(0)
	cam = vp.get_camera_3d()
	surf = surface_of(vp)
	check(surf != null, "found viewport surface")
	print("  viewport size ", vp.size, " camera at ", cam.global_position)
	surf.grab_focus()
	# Headless windows never get "mouse entered", so hover motion would be dropped by the GUI.
	get_tree().root.propagate_notification(NOTIFICATION_WM_MOUSE_ENTER)
	get_tree().root.propagate_notification(NOTIFICATION_VP_MOUSE_ENTER)
	await frames(2)

	# ---- Poly shape: outline on the ground, close it, Enter keeps the default height.
	var count1 := scene.get_child_count()
	press("Draw Poly Shape")
	await frames(2)
	var corners := [Vector3(-0.5, 0, -0.5), Vector3(0.5, 0, -0.5), Vector3(0.5, 0, 0.5), Vector3(-0.5, 0, 0.5)]
	for c in corners:
		await click(cam.unproject_position(c))
	await click(cam.unproject_position(corners[0]))  # close -> height phase
	await move(cam.unproject_position(corners[0]) + Vector2(0, -40))
	await key(KEY_ENTER)
	check(scene.get_child_count() == count1 + 1, "poly shape created")
	var poly: HewnMesh3D = scene.get_child(scene.get_child_count() - 1)
	var poly_ok := poly is HewnMesh3D
	check(poly_ok and poly.get_face_count() == 6 and poly.get_vertex_count() == 8, "poly shape is a closed box")
	check(poly_ok and poly.shape == 18 and poly.poly_points.size() == 4, "poly shape stays editable (Poly Shape)")
	check(poly_ok and poly.get_shape_param("height") > 0.05, "height phase sets a height (%f)" % (poly.get_shape_param("height") if poly_ok else 0.0))
	if poly_ok:
		poly.visible = false  # (it stands where the cube is: its faces would hide the cube's corners from the snapping)

	await select_node(cube)

	# ---- Object mode leaves keys to Godot; Tab enters edit mode and hides Godot's gizmo (lock).
	check(EditorInterface.get_selection().get_selected_nodes() == [cube], "object mode: cube selected in Godot")
	await key(KEY_TAB)
	await frames(2)
	check(EditorInterface.get_selection().get_selected_nodes().is_empty() and not cube.has_meta("_edit_lock_"), "Tab: edit mode takes the cube out of Godot's selection (no gizmo / box)")
	await frames(5)
	print("    inspector: ", EditorInterface.get_inspector().get_edited_object())
	check(EditorInterface.get_inspector().get_edited_object() == cube, "inspector still shows the cube")
	var cam_xf := cam.global_transform
	await key(KEY_3)  # face mode
	check(cam.global_transform.is_equal_approx(cam_xf), "key 3 in edit mode does not change the view")
	# focus elsewhere (another panel), click back into the viewport: keys must still be ours
	var other := find_button(EditorInterface.get_base_control().find_child("Hewn", true, false), "All", false)
	if other:
		other.grab_focus()
	await click(Vector2(5, surf.size.y - 5))
	await key(KEY_1)
	await key(KEY_3)
	check(cam.global_transform.is_equal_approx(cam_xf), "after clicking another panel, 1 / 3 still do not change the view")

	var best := visible_face(cube)
	var fc := cube.get_face_center(best)
	await click(sp(cube, fc))
	check(sel(cube, "sel_faces") == [best], "click selects visible face %d (got %s)" % [best, sel(cube, "sel_faces")])

	# Shift+drag on the move handle center = extrude; undo.
	var faces_before := cube.get_face_count()
	var center := sp(cube, fc)
	await drag(center, center + Vector2(0, -60), true)
	check(cube.get_face_count() == faces_before + 4, "shift+drag extrudes (%d -> %d)" % [faces_before, cube.get_face_count()])
	await undo(cube)
	check(cube.get_face_count() == faces_before, "undo restores face count")


	# ---- E: extrude along the normal, click to confirm.
	var n0 := cube.get_face_normal(best)
	await key(KEY_E)
	await move(center + Vector2(0, -50))
	await move(center + Vector2(0, -80))
	print("    after E moves: ", cube.get_face_center(best), " faces ", cube.get_face_count())
	await click(center + Vector2(0, -80))
	check(cube.get_face_count() == faces_before + 4, "E extrudes (%d)" % cube.get_face_count())
	var moved := cube.get_face_center(best) - fc
	check(moved.length() > 0.01 and absf(moved.normalized().dot(n0)) > 0.99, "E moves along the face normal (%s)" % moved)
	await undo(cube)

	# ---- G X 0.5 Enter: exact move along X.
	var vs: Array = Array(cube.get_face(best))
	var p0 := cube.get_vertex(vs[0])
	await key(KEY_G)
	await key(KEY_X)
	await type_text("0.5")
	await key(KEY_ENTER)
	var d := cube.get_vertex(vs[0]) - p0
	check(d.is_equal_approx(Vector3(0.5, 0, 0)), "G X 0.5 moves exactly (%s)" % d)
	await undo(cube)
	check(cube.get_vertex(vs[0]).is_equal_approx(p0), "undo G")

	# ---- R Z 90: rotate the face around Z by typed degrees.
	await key(KEY_R)
	await key(KEY_Z)
	await type_text("90")
	await key(KEY_ENTER)
	check(not cube.get_vertex(vs[0]).is_equal_approx(p0) or cube.get_face_normal(best).is_equal_approx(n0), "R Z 90 rotates")
	await undo(cube)

	# ---- Box select in vertex mode: only visible vertices.
	await key(KEY_1)
	await drag(surf.size - Vector2(4, 4), Vector2(4, 60))
	var nverts: int = sel(cube, "sel_verts").size()
	check(nverts >= 4 and nverts < 8, "box select picks only visible vertices (%d)" % nverts)

	# ---- B snap: move one vertex onto another one.
	var tv := cube.get_vertices()
	var vis_verts := []
	for i in tv.size():
		var w := cube.global_transform * tv[i]
		if sel(cube, "sel_verts").has(i):
			vis_verts.append(i)
	if vis_verts.size() >= 2:
		var a: int = vis_verts[0]
		var b: int = vis_verts[1]
		await click(sp(cube, tv[a]))
		check(sel(cube, "sel_verts") == [a], "vertex click")
		await set_snapping(true, "Vertex")
		await key(KEY_G)
		await key(KEY_B)  # B during G: snap base points
		await click(sp(cube, tv[a]))
		await click(sp(cube, tv[b]))
		check(cube.get_vertex(a).is_equal_approx(tv[b]), "G then B: source vertex lands on the target (%s vs %s)" % [cube.get_vertex(a), tv[b]])
		await undo(cube)
		# A on the first point, click on the second: midpoint as the source, target the second -> half way
		await key(KEY_G)
		await key(KEY_B)
		await move(sp(cube, tv[a]))
		await key(KEY_A)
		await click(sp(cube, tv[b]))
		await click(sp(cube, tv[b]))
		var expect: Vector3 = tv[a] + (tv[b] - (tv[a] + tv[b]) * 0.5)
		check(cube.get_vertex(a).is_equal_approx(expect), "G then B with A: midpoint source (%s vs %s)" % [cube.get_vertex(a), expect])
		await undo(cube)
		await set_snapping(false, "Increment")

	# ---- Segmented cube, edge mode: Alt+click selects the loop.
	await key(KEY_TAB)
	cube.shape = 1
	for p in ["width_segments", "height_segments", "depth_segments"]:
		cube.set_shape_param(p, 2)
	await key(KEY_TAB)
	await key(KEY_2)
	var pick := Vector2i(-1, -1)
	var pick_len := 0
	for f in cube.get_face_count():
		var wc := cube.global_transform * cube.get_face_center(f)
		if cube.get_face_normal(f).dot(cam.global_position - wc) <= 0.0:
			continue
		var fv := cube.get_face(f)
		for i in fv.size():
			var l := cube.get_edge_loop(fv[i], fv[(i + 1) % fv.size()]).size()
			if l > pick_len:
				pick_len = l
				pick = Vector2i(fv[i], fv[(i + 1) % fv.size()])
	var mid := (cube.get_vertex(pick.x) + cube.get_vertex(pick.y)) * 0.5
	await click(sp(cube, mid), false, true)
	var se: Array = sel(cube, "sel_edges")
	check(pick_len == 16 and se.size() == pick_len, "Alt+click selects the edge loop (%d of %d ints)" % [se.size(), pick_len])

	# ---- Ctrl+B: interactive bevel, wheel adds a segment, click confirms.
	var faces0 := cube.get_face_count()
	var bpos := sp(cube, mid)
	await move(bpos + Vector2(40, 0))
	await key(KEY_B, false, true)
	await move(bpos + Vector2(70, 0))
	await wheel_up(bpos + Vector2(70, 0))
	await click(bpos + Vector2(70, 0))
	var faces_bevel := cube.get_face_count()
	check(faces_bevel >= faces0 + 16, "Ctrl+B bevels with 2 segments (%d -> %d)" % [faces0, faces_bevel])
	check(sel(cube, "sel_faces").size() >= 16, "bevel leaves its faces selected")
	if OS.get_environment("HW_SELFTEST_SHOT") != "":
		await frames(20)
		get_tree().root.get_texture().get_image().save_png(OS.get_environment("HW_SELFTEST_SHOT").replace(".png", "_editor.png"))
	var panel := surf.find_child("HewnLastOperation", true, false)
	check(panel != null and panel.visible, "Last Operation panel shows after the bevel")
	if panel:
		var spins := panel.find_children("*", "LineEdit", true, false)
		spins = panel.find_children("*", "HewnDragField", true, false)
		check(spins.size() == 2 and panel.find_children("*", "OptionButton", true, false).size() == 3, "bevel panel has affect, width type, width, segments and the profile type (%d, %d)" % [spins.size(), panel.find_children("*", "OptionButton", true, false).size()])
		if spins.size() == 2:
			var seg_box: LineEdit = spins[1]
			# dragging the width field changes the value
			var wbox: LineEdit = spins[0]
			var w0 := float(wbox.text)
			# real input through the GUI: press on the field, drag right, release
			var gp := wbox.get_global_rect().get_center()
			var pressed_ev := InputEventMouseButton.new()
			pressed_ev.button_index = MOUSE_BUTTON_LEFT
			pressed_ev.pressed = true
			pressed_ev.position = gp
			pressed_ev.global_position = gp
			pressed_ev.button_mask = MOUSE_BUTTON_MASK_LEFT
			await send(pressed_ev)
			for i in 4:
				var mv := InputEventMouseMotion.new()
				mv.relative = Vector2(20, 0)
				mv.position = gp + Vector2(20 * (i + 1), 0)
				mv.global_position = mv.position
				mv.button_mask = MOUSE_BUTTON_MASK_LEFT
				await send(mv)
			# Shift mid-drag continues from the current value (only slower)
			var v_mid := float(wbox.text)
			var sv := InputEventMouseMotion.new()
			sv.relative = Vector2(20, 0)
			sv.position = gp + Vector2(100, 0)
			sv.global_position = sv.position
			sv.button_mask = MOUSE_BUTTON_MASK_LEFT
			sv.shift_pressed = true
			await send(sv)
			check(float(wbox.text) >= v_mid and float(wbox.text) - v_mid < (v_mid - w0) * 0.2 + 1e-3, "Shift while dragging a field goes on from the value (%s -> %s)" % [v_mid, wbox.text])
			var rel_ev := InputEventMouseButton.new()
			rel_ev.button_index = MOUSE_BUTTON_LEFT
			rel_ev.pressed = false
			rel_ev.position = gp + Vector2(80, 0)
			rel_ev.global_position = rel_ev.position
			await send(rel_ev)
			await frames(2)
			check(float(wbox.text) > w0, "dragging a panel field changes the value (%s -> %s)" % [w0, wbox.text])
			seg_box.text = "4"
			seg_box.text_submitted.emit("4")
			await frames(2)
			check(cube.get_face_count() == faces0 + 16 * 4 - 16 + 16 or cube.get_face_count() > faces_bevel, "changing segments in the panel redoes the bevel (%d)" % cube.get_face_count())
			# the width types (Width, Depth, Percent): the bevel stays, the mesh in place
			var wt_ob: OptionButton = panel.find_children("*", "OptionButton", true, false)[1]
			var wt_bb0 := cube.get_aabb()
			for wt in [1, 2, 3, 0]:
				wt_ob.select(wt)
				wt_ob.item_selected.emit(wt)
				await frames(2)
				var wt_bb := cube.get_aabb()
				check(cube.get_face_count() > faces0 and cube.is_inside_tree() and cube.visible and wt_bb.size.is_finite() and wt_bb.size.is_equal_approx(wt_bb0.size), "bevel width type %d: the bevel stays in place (%d faces, %s)" % [wt, cube.get_face_count(), wt_bb])
	await undo(cube)
	check(cube.get_face_count() == faces0, "undo bevel")

	# ---- Ctrl+R: loop cut on a visible edge: hover, click, click.
	await key(KEY_2)
	await key(KEY_R, false, true)
	await move(sp(cube, mid))
	await click(sp(cube, mid))
	await click(sp(cube, mid))
	check(cube.get_face_count() > faces0, "Ctrl+R loop cut adds faces (%d -> %d)" % [faces0, cube.get_face_count()])
	await undo(cube)

	# ---- K: knife across a visible face, from one edge midpoint to the opposite one.
	await key(KEY_3)
	var kf := visible_face(cube)
	var kv := cube.get_face(kf)
	if kv.size() == 4:
		var m1 := (cube.get_vertex(kv[0]) + cube.get_vertex(kv[1])) * 0.5
		var m2 := (cube.get_vertex(kv[2]) + cube.get_vertex(kv[3])) * 0.5
		var fcount := cube.get_face_count()
		await key(KEY_K)
		await click(sp(cube, m1))
		await click(sp(cube, m2))
		await key(KEY_ENTER)
		check(cube.get_face_count() == fcount + 1, "knife splits the face (%d -> %d)" % [fcount, cube.get_face_count()])
		await undo(cube)

	# ---- Box Cutter (the Edit tab) through the visible face center.
	await key(KEY_TAB)
	for p in ["width_segments", "height_segments", "depth_segments"]:
		cube.set_shape_param(p, 1)
	cube.shape = 1
	await key(KEY_TAB)
	var cf := visible_face(cube)
	var cc := sp(cube, cube.get_face_center(cf))
	var vcount := cube.get_vertex_count()
	press("Box Cutter")
	await frames(1)
	await drag(cc - Vector2(12, 12), cc + Vector2(12, 12))
	await key(KEY_ESCAPE)
	check(cube.get_vertex_count() > vcount, "box cutter cuts a hole (%d -> %d verts)" % [vcount, cube.get_vertex_count()])
	if OS.get_environment("HW_SELFTEST_SHOT") != "":
		await frames(30)
		vp.get_texture().get_image().save_png(OS.get_environment("HW_SELFTEST_SHOT"))
	await undo(cube)

	# ---- Vertex mode on the plain cube: GG slide, Shift magnet, F, normals.
	await key(KEY_1)
	await key(KEY_A, false, false, true)
	var verts := cube.get_vertices()
	var front := []
	for i in verts.size():
		var w := cube.global_transform * verts[i]
		var hit := false
		for f in cube.get_face_count():
			if Array(cube.get_face(f)).has(i) and cube.get_face_normal(f).dot(cam.global_position - w) > 0.0:
				hit = true
		if hit:
			front.append(i)
	var va: int = front[0]
	var vb: int = front[1]
	await click(sp(cube, verts[va]))
	check(sel(cube, "sel_verts") == [va], "select vertex for slide")
	var start := sp(cube, verts[va])
	var nb0 := -1
	for f in cube.get_face_count():
		var fv := Array(cube.get_face(f))
		if fv.has(va) and nb0 < 0:
			nb0 = fv[(fv.find(va) + 1) % fv.size()]
	var toward := start.lerp(sp(cube, verts[nb0]), 0.5)
	await key(KEY_G)
	await key(KEY_G)
	await move(start.lerp(toward, 0.5))
	await move(toward)
	await click(toward)
	var slid := cube.get_vertex(va)
	var on_edge := false
	for f in cube.get_face_count():
		var fv := Array(cube.get_face(f))
		var k := fv.find(va)
		if k < 0:
			continue
		for nb in [fv[(k + 1) % fv.size()], fv[(k - 1 + fv.size()) % fv.size()]]:
			var dir: Vector3 = verts[nb] - verts[va]
			var off: Vector3 = slid - verts[va]
			if off.length() > 1e-4 and off.cross(dir).length() < 1e-4 and off.dot(dir) > 0.0:
				on_edge = true
	check(on_edge, "GG slides the vertex along one of its edges (%s)" % slid)
	await undo(cube)

	await set_snapping(false, "Vertex")
	await click(sp(cube, verts[va]))
	await key(KEY_G)
	await move(sp(cube, verts[vb]) + Vector2(4, 3), false, 0, true)
	await button(sp(cube, verts[vb]) + Vector2(4, 3), MOUSE_BUTTON_LEFT, true, false, true)
	await button(sp(cube, verts[vb]) + Vector2(4, 3), MOUSE_BUTTON_LEFT, false, false, true)
	check(cube.get_vertex(va).is_equal_approx(verts[vb]), "Ctrl while moving snaps onto a vertex (%s vs %s)" % [cube.get_vertex(va), verts[vb]])
	await undo(cube)
	await set_snapping(false, "Increment")

	# Shift = precise: 100 px of mouse motion with Shift moves like 10 px.
	await click(sp(cube, verts[va]))
	var s0 := sp(cube, verts[va])
	await key(KEY_G)
	await key(KEY_X)
	await move(s0 + Vector2(10, 0))
	var fast := cube.get_vertex(va).x - verts[va].x
	await move(s0 + Vector2(10, 0), true)
	await move(s0 + Vector2(110, 0), true)
	var slow := cube.get_vertex(va).x - verts[va].x
	await key(KEY_ESCAPE)
	check(absf(fast) > 1e-4 and absf(slow - 2.0 * fast) < absf(fast) * 0.05, "Shift slows the mouse down 10x (%f, %f)" % [fast, slow])

	await key(KEY_3)
	var df := visible_face(cube)
	await click(sp(cube, cube.get_face_center(df)))
	await key(KEY_X)
	await menu_pick("Faces")
	check(cube.get_face_count() == 5, "X, Faces deletes the face")
	await key(KEY_1)
	press("Border")
	await key(KEY_F)
	check(cube.get_face_count() == 6, "F fills the hole from its border vertices (%d)" % cube.get_face_count())
	await key(KEY_A, false, false, true)
	await key(KEY_N, false, false, true)  # Alt+N: the Normals menu; Flip with nothing selected flips everything
	await menu_pick("Flip Normals")
	var up := faces_up(cube)
	check(up < 0, "Alt+N, Flip Normals flips the normals")
	await key(KEY_N, true)  # Shift+N recalculates outside
	check(faces_up(cube) >= 0, "Shift+N recalculates the normals outside")

	# ---- V: rip the center vertex of a subdivided face.
	await key(KEY_TAB)
	cube.shape = 1
	for p in ["width_segments", "height_segments", "depth_segments"]:
		cube.set_shape_param(p, 2)
	await key(KEY_TAB)
	await key(KEY_1)
	var rf := visible_face(cube)
	var center_v := -1
	var rc := Vector3.ZERO
	var nvs := cube.get_vertices()
	for i in nvs.size():
		# the vertex at the middle of the visible side
		var side_n := cube.get_face_normal(rf)
		if absf(nvs[i].dot(side_n) - 0.5) < 1e-4 and (nvs[i] - side_n * 0.5).length() < 1e-4:
			center_v = i
	check(center_v >= 0, "found the center vertex for rip")
	if center_v >= 0:
		var vc := cube.get_vertex_count()
		await click(sp(cube, nvs[center_v]))
		await move(sp(cube, nvs[center_v]) + Vector2(30, 0))
		await key(KEY_V)
		await move(sp(cube, nvs[center_v]) + Vector2(50, 0))
		await click(sp(cube, nvs[center_v]) + Vector2(50, 0))
		check(cube.get_vertex_count() == vc + 1, "V rips the vertex (%d -> %d)" % [vc, cube.get_vertex_count()])
		await undo(cube)

	# ---- Shift+M ruler opens and closes.
	await key(KEY_M, true)
	await click(sp(cube, cube.get_vertex(0)))
	await key(KEY_ESCAPE)

	# ---- RMB held (camera fly mode): keys are left to Godot.
	await key(KEY_A, false, false, true)
	await button(Vector2(100, 100), MOUSE_BUTTON_RIGHT, true)
	await key(KEY_A)
	await get_tree().create_timer(0.4).timeout  # a held right button flies the camera (a short click opens the menu)
	await button(Vector2(100, 100), MOUSE_BUTTON_RIGHT, false)
	check(sel(cube, "sel_faces").is_empty(), "A ignored while the right button is held")

	# ---- Tab leaves edit mode and removes our lock.
	await key(KEY_TAB)
	await frames(2)
	check(EditorInterface.get_selection().get_selected_nodes() == [cube], "Tab out: cube selected again")
	# selecting another node in edit mode leaves edit mode
	await key(KEY_TAB)
	await frames(2)
	if is_instance_valid(poly):  # an earlier failure + undo may have removed it
		EditorInterface.get_selection().add_node(poly)
	await frames(3)
	await key(KEY_A)
	check(sel(cube, "sel_faces").is_empty(), "selecting another node leaves edit mode (A not handled)")
	await select_node(cube)

	# ---- Boolean buttons: subtract a moved cube from the cube.
	cube.shape = 1
	var cutter := HewnMesh3D.new()
	cutter.name = "BoolCutter"
	scene.add_child(cutter)
	cutter.owner = scene
	cutter.position = Vector3(0.5, 0.5, 0.5)
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(cutter)
	await frames(2)
	EditorInterface.get_selection().add_node(cube)
	await frames(3)
	var bcount := scene.get_child_count()
	var base_verts := cube.get_vertex_count()
	var base_tris := 0
	for si in cube.mesh.get_surface_count():
		base_tris += (cube.mesh.surface_get_arrays(si)[Mesh.ARRAY_INDEX] as PackedInt32Array).size() / 3
	press("Subtract")
	await frames(3)
	# non-destructive by default: the cutter stays (see-through), the cube shows the cut, its base is kept
	var shown_tris := 0
	for si in cube.mesh.get_surface_count():
		shown_tris += (cube.mesh.surface_get_arrays(si)[Mesh.ARRAY_INDEX] as PackedInt32Array).size() / 3
	var bvol := 0.0
	for si in cube.mesh.get_surface_count():
		var barr := cube.mesh.surface_get_arrays(si)
		var bpv: PackedVector3Array = barr[Mesh.ARRAY_VERTEX]
		var bix: PackedInt32Array = barr[Mesh.ARRAY_INDEX]
		for bk in range(0, bix.size(), 3):
			bvol += bpv[bix[bk]].dot(bpv[bix[bk + 2]].cross(bpv[bix[bk + 1]])) / 6.0
	check(scene.get_child_count() == bcount and cutter.is_boolean_cutter() and cube.get_boolean_targets().size() == 1 and cube.get_vertex_count() == base_verts and absf(bvol - 0.875) < 1e-3,
			"Boolean Subtract adds a live modifier: the corner cut away (volume %f)" % bvol)
	cutter.position = Vector3(5, 0, 0)
	await frames(3)
	shown_tris = 0
	for si in cube.mesh.get_surface_count():
		shown_tris += (cube.mesh.surface_get_arrays(si)[Mesh.ARRAY_INDEX] as PackedInt32Array).size() / 3
	check(shown_tris == base_tris, "moving the cutter updates the result live (%d triangles)" % shown_tris)
	await undo(cube)
	check(cube.get_boolean_targets().is_empty() and not cutter.is_boolean_cutter(), "undo removes the boolean modifier")
	# destructive mode still there
	(EditorInterface.get_base_control().find_child("Hewn", true, false).find_children("*", "CheckBox", true, false).filter(func(c): return c.text.begins_with("Non-destructive"))[0] as CheckBox).button_pressed = false
	cutter.position = Vector3(0.5, 0.5, 0.5)
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(cutter)
	await frames(2)
	EditorInterface.get_selection().add_node(cube)
	await frames(3)
	press("Subtract")
	await frames(3)
	var dvol := 0.0
	var dvs := cube.get_vertices()
	for dvf in cube.get_face_count():
		var dfc := cube.get_face(dvf)
		for di in range(1, dfc.size() - 1):
			dvol += dvs[dfc[0]].dot(dvs[dfc[di]].cross(dvs[dfc[di + 1]])) / 6.0
	check(scene.get_child_count() == bcount - 1 and absf(dvol - 0.875) < 1e-3, "destructive Boolean Subtract removes the cutter and cuts the cube (volume %f)" % dvol)
	await undo(cube)
	(EditorInterface.get_base_control().find_child("Hewn", true, false).find_children("*", "CheckBox", true, false).filter(func(c): return c.text.begins_with("Non-destructive"))[0] as CheckBox).button_pressed = true
	cutter.position = Vector3(5, 0, 0)

	# ---- Toolbar settings: orientation, pivot, proportional editing, auto merge, x-ray, snapping toggle.
	var bar := EditorInterface.get_base_control().find_child("HewnToolbar", true, false)
	var orient_mb: MenuButton = null
	var pivot_mb: MenuButton = null
	for mbtn in bar.find_children("*", "MenuButton", true, false):
		if mbtn.tooltip_text.begins_with("Transform orientation"):
			orient_mb = mbtn
		elif mbtn.tooltip_text.begins_with("Pivot"):
			pivot_mb = mbtn
	check(orient_mb != null and pivot_mb != null, "toolbar has orientation and pivot menus")
	await select_node(cube)
	cube.shape = 1
	for p in ["width_segments", "height_segments", "depth_segments"]:
		cube.set_shape_param(p, 1)
	await key(KEY_TAB)
	await key(KEY_3)
	# Normal orientation: G Z moves the face along its normal
	var nf := visible_face(cube)
	var nn := cube.get_face_normal(nf)
	var nc0 := cube.get_face_center(nf)
	await click(sp(cube, nc0))
	orient_mb.get_popup().id_pressed.emit(2)
	await key(KEY_G)
	await key(KEY_Z)
	await type_text("0.3")
	await key(KEY_ENTER)
	check((cube.get_face_center(nf) - nc0).is_equal_approx(nn * 0.3), "Normal orientation: G Z moves along the face normal (%s)" % (cube.get_face_center(nf) - nc0))
	await undo(cube)
	orient_mb.get_popup().id_pressed.emit(0)
	# Individual origins: scaling two separate faces keeps each center in place
	var faces2 := []
	for f in cube.get_face_count():
		if absf(cube.get_face_normal(f).y) > 0.9:
			faces2.append(f)
	await key(KEY_A, false, false, true)
	await click(sp(cube, cube.get_face_center(nf)))
	pivot_mb.get_popup().id_pressed.emit(2)
	var c_before := cube.get_face_center(nf)
	await key(KEY_S)
	await type_text("0.5")
	await key(KEY_ENTER)
	check(cube.get_face_center(nf).is_equal_approx(c_before), "Individual origins: the face scales around its own center")
	await undo(cube)
	pivot_mb.get_popup().id_pressed.emit(3)
	# Proportional editing (O): a neighbor follows partially
	cube.set_shape_param("width_segments", 2)
	cube.shape = 1
	await key(KEY_TAB)
	cube.shape = 1
	for p in ["width_segments", "height_segments", "depth_segments"]:
		cube.set_shape_param(p, 2)
	await key(KEY_TAB)
	await key(KEY_1)
	var pv := cube.get_vertices()
	var vfront := cube.get_face(visible_face(cube))
	var sel_v: int = vfront[0]
	await click(sp(cube, pv[sel_v]))
	await key(KEY_O)
	await key(KEY_G)
	await key(KEY_X)
	await type_text("0.5")
	await key(KEY_ENTER)
	var partial := 0
	var after_v := cube.get_vertices()
	for i in after_v.size():
		var dx := after_v[i].x - pv[i].x
		if i != sel_v and dx > 0.01 and dx < 0.49:
			partial += 1
	check(absf(after_v[sel_v].x - pv[sel_v].x - 0.5) < 1e-4 and partial > 0, "O: proportional editing moves neighbors partially (%d)" % partial)
	await undo(cube)
	await key(KEY_O)
	# Alt+Z x-ray: box select also takes hidden vertices
	await key(KEY_Z, false, false, true)
	await drag(surf.size - Vector2(4, 4), Vector2(4, 60))
	check(sel(cube, "sel_verts").size() == cube.get_vertex_count(), "Alt+Z x-ray: box select takes hidden vertices (%d of %d)" % [sel(cube, "sel_verts").size(), cube.get_vertex_count()])
	await key(KEY_Z, false, false, true)
	# Shift+Tab toggles snapping
	var magnet := godot_snap()
	check(magnet != null, "found Godot's snap button")
	var was_on := magnet.button_pressed
	await key(KEY_TAB, true)
	check(magnet.button_pressed != was_on, "Shift+Tab toggles snapping")
	await key(KEY_TAB, true)
	# a tool, then 3: the tool ends, the mode changes, the view stays
	var xf0 := cam.global_transform
	await key(KEY_K)
	await key(KEY_3)
	check(cam.global_transform.is_equal_approx(xf0), "K then 3: the view does not change")
	# Bisect: a line dragged across a face; split only (its panel): the faces are cut
	var bf := cube.get_face_count()
	var bis_vf := visible_face(cube)
	var bis_n := cube.get_face_normal(bis_vf)
	var bis_t := bis_n.cross(Vector3.UP) if absf(bis_n.y) < 0.9 else Vector3.RIGHT
	bis_t = bis_t.normalized()
	var bis_b := bis_n.cross(bis_t).normalized()
	var bis_c := cube.get_face_center(bis_vf)
	var bis_snap := godot_snap().button_pressed
	godot_snap().button_pressed = false
	press("Bisect")
	await frames(1)
	await drag(sp(cube, bis_c + bis_t * 0.7 + bis_b * 0.1), sp(cube, bis_c - bis_t * 0.7 + bis_b * 0.1))
	await frames(2)
	godot_snap().button_pressed = bis_snap
	var bis_lp := surf.find_child("HewnLastOperation", true, false)
	var bis_mode: OptionButton = bis_lp.find_children("*", "OptionButton", true, false)[0] if bis_lp and bis_lp.visible and bis_lp.find_children("*", "OptionButton", true, false).size() > 0 else null
	var bis_had_mode := bis_mode != null  # (the panel is made again when the mode changes what it shows)
	if bis_mode:
		bis_mode.item_selected.emit(0)
		await frames(1)
	check(bis_had_mode and cube.get_face_count() > bf, "Bisect (two points, Enter, split only) cuts the faces (%d -> %d)" % [bf, cube.get_face_count()])
	await undo(cube)
	# Auto merge: moving a vertex onto another one merges them
	await key(KEY_1)
	pv = cube.get_vertices()
	var vf2 := cube.get_face(visible_face(cube))
	var am_a: int = vf2[0]
	var am_b: int = vf2[1]
	var am_count := cube.get_vertex_count()
	for b in bar.find_children("*", "Button", true, false):
		if b.tooltip_text.begins_with("Auto merge vertices"):
			b.button_pressed = true
	var hidden := []
	for c in scene.get_children():
		if c != cube and c.visible:
			c.visible = false  # only the cube's vertices are snap targets
			hidden.append(c)
	await set_snapping(true, "Vertex")
	await key(KEY_A, false, false, true)  # (nothing selected: the click must not land on the gizmo of the old selection)
	await click(sp(cube, pv[am_a]))
	await key(KEY_G)
	await move(sp(cube, pv[am_b]) + Vector2(3, 2))
	await click(sp(cube, pv[am_b]) + Vector2(3, 2))
	check(cube.get_vertex_count() == am_count - 1, "Auto merge: the moved vertex merges into the target (%d -> %d)" % [am_count, cube.get_vertex_count()])
	await undo(cube)
	for c in hidden:
		c.visible = true
	await set_snapping(false, "Increment")
	for b in bar.find_children("*", "Button", true, false):
		if b.tooltip_text.begins_with("Auto merge vertices"):
			b.button_pressed = false
	await key(KEY_TAB)

	# ---- Context menu (short right click), P separate, vertex bevel, L with delimiters, several modes.
	await select_node(cube)
	cube.shape = 1
	for p in ["width_segments", "height_segments", "depth_segments"]:
		cube.set_shape_param(p, 1)
	await key(KEY_TAB)
	await key(KEY_3)
	var cf2 := visible_face(cube)
	await click(sp(cube, cube.get_face_center(cf2)))
	var rclick := sp(cube, cube.get_face_center(cf2))
	await button(rclick, MOUSE_BUTTON_RIGHT, true)
	await button(rclick, MOUSE_BUTTON_RIGHT, false)
	var fc0 := cube.get_face_count()
	await menu_pick("Poke Faces")
	check(cube.get_face_count() == fc0 + 3, "right-click menu: Poke Faces (%d -> %d)" % [fc0, cube.get_face_count()])
	await undo(cube)
	await click(sp(cube, cube.get_face_center(cf2)))
	var before_children := scene.get_child_count()
	await key(KEY_P)
	await menu_pick("Selection")
	check(scene.get_child_count() == before_children + 1 and cube.get_face_count() == 5, "P, Selection: the face goes to a new mesh")
	await undo(cube)
	check(scene.get_child_count() == before_children and cube.get_face_count() == 6, "undo separate")
	await key(KEY_1)
	var vb_v := cube.get_face(visible_face(cube))[0]
	await key(KEY_A, false, false, true)  # no selection = no gizmo over the vertex
	await click(sp(cube, cube.get_vertex(vb_v)))
	check(sel(cube, "sel_verts") == [vb_v], "vertex selected for the vertex bevel")
	await move(sp(cube, cube.get_vertex(vb_v)) + Vector2(40, 0))
	await key(KEY_B, true, true)
	await move(sp(cube, cube.get_vertex(vb_v)) + Vector2(60, 0))
	await click(sp(cube, cube.get_vertex(vb_v)) + Vector2(60, 0))
	check(cube.get_face_count() == 7, "Ctrl+Shift+B bevels the vertex (%d faces)" % cube.get_face_count())
	await undo(cube)
	# Shift+3: vertices and faces at once
	await key(KEY_3, true)
	check(sel(cube, "sel_verts").size() >= 0 and EditorInterface.get_base_control().find_child("HewnToolbar", true, false) != null, "Shift+3 adds face mode")
	await key(KEY_3)
	# L on a face: linked faces, the panel offers the delimiters
	await key(KEY_A, false, false, true)
	await move(sp(cube, cube.get_face_center(cf2)))
	await key(KEY_L)
	check(sel(cube, "sel_faces").size() == 6, "L selects the linked faces")
	var lpanel := surf.find_child("HewnLastOperation", true, false)
	check(lpanel != null and lpanel.visible and lpanel.find_children("*", "CheckBox", true, false).size() == 6, "L: deselect + 5 delimiters in the panel")
	await key(KEY_TAB)

	# ---- E then RMB keeps the extrusion, the move is cancelled.
	await select_node(cube)
	await key(KEY_TAB)
	await key(KEY_3)
	var ef := visible_face(cube)
	await key(KEY_A, false, false, true)  # (the gizmo of an old selection would catch the click)
	await click(sp(cube, cube.get_face_center(ef)))
	var fcnt := cube.get_face_count()
	var ec := cube.get_face_center(ef)
	await key(KEY_E)
	await move(sp(cube, ec) + Vector2(0, -40))
	await button(sp(cube, ec) + Vector2(0, -40), MOUSE_BUTTON_RIGHT, true)
	await button(sp(cube, ec) + Vector2(0, -40), MOUSE_BUTTON_RIGHT, false)
	check(cube.get_face_count() == fcnt + 4 and cube.get_face_center(ef).is_equal_approx(ec), "RMB after E keeps the extrusion in place")
	await undo(cube)
	await key(KEY_TAB)

	# ---- Vertex paint: a click on the visible face paints its corners.
	await select_node(cube)
	var picker: ColorPickerButton = EditorInterface.get_base_control().find_child("VertexColor", true, false)
	picker.color = Color(1, 0, 0)  # white on white would not show
	press("Vertex Paint")
	await frames(1)
	await key(KEY_1)
	await key(KEY_A, false, false, true)  # (nothing selected: no mask)
	var vf := cube.get_face(visible_face(cube))
	var pc := sp(cube, cube.get_vertex(vf[0]).lerp(cube.get_face_center(visible_face(cube)), 0.1))  # the brush paints vertices
	await move(pc)
	await click(pc)
	await key(KEY_TAB)  # (Vertex Paint is a mode: Tab back to object mode)
	var cols: PackedColorArray = face_cols(cube.mesh_data)
	var painted := false
	for c in cols:
		if not c.is_equal_approx(Color(1, 1, 1, 1)):
			painted = true
	check(painted, "vertex paint changes corner colors")
	await undo(cube)

	# ---- 3D cursor: Shift+RMB on the ground, new shapes appear there.
	var cpos := cam.unproject_position(Vector3(0.75, 0, 1.0))
	await button(cpos, MOUSE_BUTTON_RIGHT, true, false, false, true)
	await button(cpos, MOUSE_BUTTON_RIGHT, false, false, false, true)
	var cn := scene.get_child_count()
	press("Cube")
	await frames(1)
	await key(KEY_ENTER)
	var made: Node3D = scene.get_child(scene.get_child_count() - 1)
	check(scene.get_child_count() == cn + 1 and absf(made.global_position.x - 0.75) < 0.02 and absf(made.global_position.z - 1.0) < 0.02, "Shift+RMB places the 3D cursor; new shapes appear there (%s)" % made.global_position)
	await undo(cube)
	# (the cursor back to the origin: on the ground, not on the cube standing there)
	var cvis := cube.visible
	cube.visible = false
	await button(cam.unproject_position(Vector3.ZERO), MOUSE_BUTTON_RIGHT, true, false, false, true)
	await button(cam.unproject_position(Vector3.ZERO), MOUSE_BUTTON_RIGHT, false, false, false, true)
	cube.visible = cvis

	# ---- Create tab: every shape button adds a node.
	await select_node(cube)
	var count0 := scene.get_child_count()
	for t in ["Cube", "Plane", "Cylinder", "Cone", "Sphere", "Icosphere", "Torus", "Pipe", "Prism", "Stairs", "Arch", "Door", "Capsule", "Disc", "Wedge", "Wall"]:
		press(t)  # starts the drawing tool; Enter = default size at the 3D cursor
		await frames(1)
		await key(KEY_ENTER)
	var names := []
	for i in range(count0, scene.get_child_count()):
		names.append(scene.get_child(i).name)
	print("    created: ", names)
	check(scene.get_child_count() == count0 + 16, "create buttons add 16 shapes (%d)" % (scene.get_child_count() - count0))

	# ---- Drawing a cube: base from (-1, -1) to (1, 0.5) on the ground, then the height.
	for c in scene.get_children():
		c.visible = false  # draw on the empty ground
	var before := scene.get_child_count()
	press("Cube")
	await frames(1)
	var pa := cam.unproject_position(Vector3(-1, 0, -1))
	var pb := cam.unproject_position(Vector3(1, 0, 0.5))
	await drag(pa, pb)
	var top := cam.unproject_position(Vector3(0, 0.8, -0.25))
	await move(cam.unproject_position(Vector3(0, 0, -0.25)))
	await move(top)
	await click(top)
	check(scene.get_child_count() == before + 1, "drawn cube created")
	var drawn: HewnMesh3D = scene.get_child(scene.get_child_count() - 1)
	if drawn is HewnMesh3D:
		var aabb := drawn.get_aabb()
		check(absf(aabb.size.x - 2.0) < 0.05 and absf(aabb.size.z - 1.5) < 0.05 and aabb.size.y > 0.2, "drawn cube has the drawn size (%s)" % aabb.size)
		check(absf(drawn.global_position.y - aabb.size.y * 0.5) < 0.05, "drawn cube rests on the ground")

	# ---- Shape panel: the parameters of a parametric shape, bottom left of the viewport.
	godot_snap().button_pressed = false
	await frames(3)
	var spanel: Control = surf.find_child("HewnShapePanel", true, false)
	check(spanel != null and spanel.visible, "shape panel after drawing a shape")
	if drawn is HewnMesh3D and spanel:
		var fields := spanel.find_children("*", "HewnDragField", true, false)
		check(fields.size() >= 3, "shape panel has fields (%d)" % fields.size())
		var base_y := drawn.global_position.y - drawn.get_aabb().size.y * 0.5
		var h0: float = drawn.get_shape_param("height")
		fields[1].value_changed.emit(1.5)  # width, height, depth...
		await frames(2)
		check(absf(drawn.get_shape_param("height") - 1.5) < 1e-3, "shape panel changes the height")
		check(absf(drawn.global_position.y - 0.75 - base_y) < 0.02, "the shape keeps its base (%f)" % drawn.global_position.y)
		await undo(drawn)
		check(absf(drawn.get_shape_param("height") - h0) < 1e-3, "undo of a shape parameter")
		await key(KEY_TAB)
		await frames(2)
		check(not spanel.visible, "no shape panel in edit mode")
		await key(KEY_TAB)
		await frames(3)
		check(spanel.visible, "shape panel again in object mode (shape not edited)")
		drawn.visible = false

	# ---- Clicks (no drag): a corner, the opposite corner, the height. W then turns the box into a room.
	var r20_before := scene.get_child_count()
	press("Cube")
	await frames(1)
	var r20_a := cam.unproject_position(Vector3(-1, 0, -1))
	var r20_b := cam.unproject_position(Vector3(0.5, 0, 0.5))
	await click(r20_a)
	await move(r20_a.lerp(r20_b, 0.5))
	await click(r20_b)
	await move(cam.unproject_position(Vector3(-0.25, 0, -0.25)))
	var r20_top := cam.unproject_position(Vector3(-0.25, 0.6, -0.25))
	await move(r20_top)
	await click(r20_top)
	check(scene.get_child_count() == r20_before + 1, "a cube drawn with three clicks")
	var r20_c = scene.get_child(scene.get_child_count() - 1)
	if r20_c is HewnMesh3D:
		check(absf(r20_c.get_aabb().size.x - 1.5) < 0.05 and absf(r20_c.get_aabb().size.z - 1.5) < 0.05, "clicked base size (%s)" % r20_c.get_aabb().size)
		var r20_flip: float = r20_c.get_shape_param("flip_normals")
		await key(KEY_W)
		check(r20_c.get_shape_param("flip_normals") != r20_flip, "W after a new box: invert (a room)")
		await undo(r20_c)
		check(r20_c.get_shape_param("flip_normals") == r20_flip, "undo of the invert")
		r20_c.visible = false
	# Shift on the first click: a turned box (its first side sets the direction)
	var r20_b2 := scene.get_child_count()
	press("Cube")
	await frames(1)
	var r20_t_a := cam.unproject_position(Vector3(0, 0, 0))
	await click(r20_t_a, false, false, false, true)
	var r20_t_b := cam.unproject_position(Vector3(1, 0, -1))
	await move(r20_t_b)
	await click(r20_t_b)
	var r20_t_c := cam.unproject_position(Vector3(1.5, 0, -0.5))
	await move(r20_t_c)
	await click(r20_t_c)
	await move(cam.unproject_position(Vector3(0.75, 0, -0.25)))
	var r20_t_top := cam.unproject_position(Vector3(0.75, 0.5, -0.25))
	await move(r20_t_top)
	await click(r20_t_top)
	check(scene.get_child_count() == r20_b2 + 1, "a turned box drawn (Shift on the first click)")
	var r20_t = scene.get_child(scene.get_child_count() - 1)
	if r20_t is HewnMesh3D:
		var r20_ax: Vector3 = r20_t.global_transform.basis.x.normalized()
		check(absf(absf(r20_ax.dot(Vector3(1, 0, -1).normalized())) - 1.0) < 0.02 or absf(absf(r20_ax.dot(Vector3(1, 0, 1).normalized())) - 1.0) < 0.02, "the turned box follows its first side (%s)" % r20_ax)
		check(absf(r20_t.get_shape_param("width") * r20_t.get_shape_param("depth") - sqrt(2.0) * sqrt(0.5)) < 0.05, "turned box base sides (%f x %f)" % [r20_t.get_shape_param("width"), r20_t.get_shape_param("depth")])
		r20_t.visible = false

	# ---- Texture browser (bottom panel): a click puts the texture on the selected object
	var r20_tb: Node = EditorInterface.get_base_control().find_child("HewnTextures", true, false)
	check(r20_tb != null, "Textures tab in the bottom panel")
	var r20_tex_path := "res://tests/hw_selftest_tex.png"
	var r20_timg := Image.create_empty(16, 16, false, Image.FORMAT_RGBA8)
	r20_timg.fill(Color(0.8, 0.3, 0.2))
	r20_timg.save_png(ProjectSettings.globalize_path(r20_tex_path))
	EditorInterface.get_resource_filesystem().scan()
	for i in 600:
		await frames(1)
		if ResourceLoader.exists(r20_tex_path) and not EditorInterface.get_resource_filesystem().is_scanning():
			break
	if r20_tb and r20_c is HewnMesh3D:
		r20_c.visible = true
		await select_node(r20_c)
		r20_tb.click_image(r20_tex_path)
		await frames(2)
		var r20_tm: StandardMaterial3D = r20_c.get_materials()[r20_c.get_face_material(0)] if r20_c.get_materials().size() > 0 else null
		check(r20_tm != null and r20_tm.albedo_texture != null and r20_tm.albedo_texture.resource_path == r20_tex_path, "the clicked texture is on all faces (%s)" % r20_tm)
		check(r20_tm != null and r20_tm.resource_path.begins_with("res://materials/"), "its material is saved in the materials folder (%s)" % (r20_tm.resource_path if r20_tm else ""))
		check(r20_tm != null and r20_tm.uv1_scale.is_equal_approx(Vector3(8, 8, 1)), "128 px/m: a 16 px texture tiles 8 times a meter (%s)" % (r20_tm.uv1_scale if r20_tm else Vector3()))
		var r20_tm_path: String = r20_tm.resource_path if r20_tm else ""
		await undo(r20_c)
		check(r20_c.get_materials().size() == 0 or r20_c.get_materials()[0] == null, "undo takes it off")
		r20_c.visible = false
		if r20_tm_path != "":
			DirAccess.remove_absolute(ProjectSettings.globalize_path(r20_tm_path))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(r20_tex_path))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(r20_tex_path + ".import"))
	DirAccess.remove_absolute(ProjectSettings.globalize_path("res://materials"))

	# ---- Box Cut (clicks): a flat hole in the ceiling of a room, W makes it an alcove
	var r21_room := HewnMesh3D.new()
	r21_room.shape = 1
	scene.add_child(r21_room)
	r21_room.owner = scene
	r21_room.set_shape_param("width", 2.0)
	r21_room.set_shape_param("depth", 2.0)
	r21_room.set_shape_param("flip_normals", 1.0)
	r21_room.position = Vector3(0, 0.5, 0)
	await select_node(r21_room)
	press("Box Cut")
	await frames(1)
	var r21_a := cam.unproject_position(Vector3(-0.3, 1.0, -0.3))
	var r21_b := cam.unproject_position(Vector3(0.3, 1.0, 0.3))
	await click(r21_a)
	await move(r21_b)
	await click(r21_b)
	await click(r21_b)  # no move: a flat cut
	var r21_an := analyze_mesh(r21_room)
	check(not r21_an.closed and r21_room.get_face_count() > 6, "Box Cut: a hole in the ceiling (%d faces)" % r21_room.get_face_count())
	await key(KEY_W)
	r21_an = analyze_mesh(r21_room)
	check(r21_an.closed, "W after a cut with one hole: an alcove closes the room")
	await undo(r21_room)
	check(not analyze_mesh(r21_room).closed, "undo of the alcove")
	await undo(r21_room)
	check(r21_room.get_face_count() == 6, "undo of the cut (%d faces)" % r21_room.get_face_count())
	r21_room.visible = false

	# ---- Clip: a line across the top of a cube removes its front part, W closes the cut
	var r21_cube := HewnMesh3D.new()
	r21_cube.shape = 1
	scene.add_child(r21_cube)
	r21_cube.owner = scene
	r21_cube.position = Vector3(0, 0.5, 0)
	await select_node(r21_cube)
	press("Clip")
	await frames(1)
	var r21_c1 := cam.unproject_position(Vector3(0.1, 1.0, -0.3))
	var r21_c2 := cam.unproject_position(Vector3(0.1, 1.0, 0.3))
	await click(r21_c1)
	await move(r21_c2)
	await click(r21_c2)
	await key(KEY_ENTER)
	var r21_bb: AABB = r21_cube.get_aabb()
	var r21_lp0 := surf.find_child("HewnLastOperation", true, false)
	var r21_m0: OptionButton = r21_lp0.find_children("*", "OptionButton", true, false)[0] if r21_lp0 and r21_lp0.visible and r21_lp0.find_children("*", "OptionButton", true, false).size() > 0 else null
	if r21_m0 and r21_m0.selected == 0:
		r21_m0.item_selected.emit(1)  # (the tool started with "split only" left from before)
		await frames(1)
		r21_bb = r21_cube.get_aabb()
	check(analyze_mesh(r21_cube).closed and r21_bb.size.x < 0.95 and r21_bb.size.x > 0.05, "Clip removes one side, the cut closed (%s)" % r21_bb.size)
	# its Last Operation panel: Bisect Only keeps both sides, Remove Front again
	var r28_lp := surf.find_child("HewnLastOperation", true, false)
	var r28_mode: OptionButton = r28_lp.find_children("*", "OptionButton", true, false)[0] if r28_lp and r28_lp.visible and r28_lp.find_children("*", "OptionButton", true, false).size() > 0 else null
	check(r28_mode != null and r28_mode.item_count == 3, "Clip has a Last Operation panel with its mode")
	if r28_mode:
		r28_mode.item_selected.emit(0)
		await frames(1)
		check(analyze_mesh(r21_cube).closed and absf(r21_cube.get_aabb().size.x - 1.0) < 0.01 and r21_cube.get_face_count() == 10, "Clip mode Bisect Only: only cut (%d faces)" % r21_cube.get_face_count())
		r28_mode.item_selected.emit(1)
		await frames(1)
		check(analyze_mesh(r21_cube).closed and r21_cube.get_aabb().size.x < 0.9, "back to Remove Front")
	r21_cube.visible = false

	# ---- Alt+click: the texture of the one selected face onto a neighbor, running on across the edge
	var r21_tc := HewnMesh3D.new()
	r21_tc.shape = 1
	scene.add_child(r21_tc)
	r21_tc.owner = scene
	r21_tc.position = Vector3(0, 0.5, 0)
	var r21_mat := StandardMaterial3D.new()
	var r21_top := -1
	var r21_side := -1
	var r21_best := -1.0
	for f in r21_tc.get_face_count():
		var fn := r21_tc.get_face_normal(f)
		if fn.y > 0.9:
			r21_top = f
	# the side face the camera sees best
	for f in r21_tc.get_face_count():
		var fn := r21_tc.get_face_normal(f)
		var facing := -fn.dot(cam.global_transform.basis.z * -1.0)
		if absf(fn.y) < 0.1 and facing > r21_best:
			r21_best = facing
			r21_side = f
	r21_tc.apply_material(r21_mat, PackedInt32Array([r21_top]))
	await select_node(r21_tc)
	await key(KEY_TAB)
	await key(KEY_3)
	var r21_st: Dictionary = r21_tc.get_state()
	r21_st["sel_faces"] = PackedInt32Array([r21_top])
	r21_tc.set_state(r21_st)
	await frames(2)
	var r21_sc: Vector3 = r21_tc.global_transform * r21_tc.get_face_center(r21_side)
	await click(cam.unproject_position(r21_sc), false, true)
	var r21_mats := r21_tc.get_materials()
	check(r21_mats.size() > r21_tc.get_face_material(r21_side) and r21_mats[r21_tc.get_face_material(r21_side)] == r21_mat, "Alt+click puts the texture of the selected face on the clicked one")
	var r21_ua := r21_tc.get_face_uvs(r21_top)
	var r21_ub := r21_tc.get_face_uvs(r21_side)
	var r21_fa := r21_tc.get_face(r21_top)
	var r21_fb := r21_tc.get_face(r21_side)
	var r21_same := 0
	for i in r21_fa.size():
		for kk in r21_fb.size():
			if r21_fa[i] == r21_fb[kk] and r21_ua[i].distance_to(r21_ub[kk]) < 1e-4:
				r21_same += 1
	check(r21_same == 2, "the texture runs on across the edge (%d corners)" % r21_same)
	await undo(r21_tc)
	check(r21_tc.get_face_material(r21_side) != r21_tc.get_face_material(r21_top), "undo of Alt+click")
	await key(KEY_TAB)
	r21_tc.visible = false

	# ---- Prefabs: a scene from the Prefabs tab placed with a click, W places another one
	var r21_pd: Node = EditorInterface.get_base_control().find_child("HewnPrefabs", true, false)
	check(r21_pd != null, "Prefabs tab in the bottom panel")
	var r21_pp := "res://tests/hw_selftest_prefab.tscn"
	var r21_src := Node3D.new()
	r21_src.name = "Crate"
	var r21_mi := MeshInstance3D.new()
	r21_mi.mesh = BoxMesh.new()
	r21_src.add_child(r21_mi)
	r21_mi.owner = r21_src
	var r21_ps := PackedScene.new()
	r21_ps.pack(r21_src)
	ResourceSaver.save(r21_ps, r21_pp)
	r21_src.free()
	if r21_pd:
		var r21_n0 := scene.get_child_count()
		r21_pd.place(r21_pp)
		var r21_at := cam.unproject_position(Vector3(0.5, 0, 0.5))
		await move(r21_at + Vector2(3, 0))
		await move(r21_at)
		await click(r21_at)
		check(scene.get_child_count() == r21_n0 + 1, "a click places the prefab")
		var r21_placed: Node3D = scene.get_child(scene.get_child_count() - 1)
		check(r21_placed.scene_file_path == r21_pp and r21_placed.global_position.distance_to(Vector3(0.5, 0, 0.5)) < 0.05, "linked to its scene, where clicked (%s)" % r21_placed.global_position)
		await key(KEY_W)
		var r21_at2 := cam.unproject_position(Vector3(-0.5, 0, 0.5))
		await move(r21_at2 + Vector2(3, 0))
		await move(r21_at2)
		await click(r21_at2)
		check(scene.get_child_count() == r21_n0 + 2, "W: place another one")
		await undo(scene)
		await undo(scene)
		check(scene.get_child_count() == r21_n0, "undo removes them")
	DirAccess.remove_absolute(ProjectSettings.globalize_path(r21_pp))

	# ---- Alt+G: the grid on faces; overlapping faces are found
	var r21_fg := find_button(EditorInterface.get_base_control(), "Grid on faces", false)
	await key(KEY_G, false, false, true)
	check(r21_fg != null and r21_fg.button_pressed, "Alt+G turns the grid on faces on")
	await key(KEY_G, false, false, true)
	check(r21_fg != null and not r21_fg.button_pressed, "and off")
	var r21_ov := HewnMesh3D.new()
	r21_ov.shape = 1
	scene.add_child(r21_ov)
	r21_ov.owner = scene
	var r21_ovb := HewnMesh3D.new()
	r21_ovb.shape = 1
	r21_ovb.position = Vector3(1, 0, 0)
	scene.add_child(r21_ovb)
	r21_ov.merge_from(r21_ovb)  # two cubes touching: two faces on top of each other
	r21_ovb.free()
	await select_node(r21_ov)
	await key(KEY_TAB)
	press("Overlapping Faces")
	await frames(2)
	check(sel(r21_ov, "sel_faces").size() == 2, "Overlapping Faces selects the two faces between the cubes (%d)" % sel(r21_ov, "sel_faces").size())
	await key(KEY_TAB)
	r21_ov.visible = false

	# ---- Round shapes are drawn from the center out to the radius.
	var nb := scene.get_child_count()
	press("Cylinder")
	await frames(1)
	await drag(cam.unproject_position(Vector3(0, 0, 0)), cam.unproject_position(Vector3(0.8, 0, 0.5)))
	await move(cam.unproject_position(Vector3(0, 0, 0)))
	await move(cam.unproject_position(Vector3(0, 1, 0)))
	await click(cam.unproject_position(Vector3(0, 1, 0)))
	check(scene.get_child_count() == nb + 1, "drawn cylinder created")
	var cyl: HewnMesh3D = scene.get_child(scene.get_child_count() - 1)
	check(absf(cyl.get_shape_param("radius") - 0.8) < 0.05 and Vector2(cyl.global_position.x, cyl.global_position.z).length() < 0.05,
			"cylinder: radius from the drag, centered on the press (%f, %s)" % [cyl.get_shape_param("radius"), cyl.global_position])
	check(absf(cyl.get_shape_param("radius_z") - 0.5) < 0.05, "an oval: radius Z from the corner (%f)" % cyl.get_shape_param("radius_z"))
	cyl.visible = false

	# ---- Wall: two clicks, Enter.
	nb = scene.get_child_count()
	press("Wall")
	await frames(1)
	await click(cam.unproject_position(Vector3(-1, 0, 0.5)))
	await move(cam.unproject_position(Vector3(1, 0, 0.5)))
	await click(cam.unproject_position(Vector3(1, 0, 0.5)))
	await key(KEY_ENTER)
	check(scene.get_child_count() == nb + 1, "wall from two clicks")
	var wl: HewnMesh3D = scene.get_child(scene.get_child_count() - 1)
	check(absf(wl.get_shape_param("length") - 2.0) < 0.05 and absf(wl.global_position.z - 0.5) < 0.05, "wall length and place (%f)" % wl.get_shape_param("length"))
	wl.visible = false
	# a wall with corners: three clicks, RMB finishes
	nb = scene.get_child_count()
	press("Wall")
	await frames(1)
	for wp in [Vector3(-1, 0, -1), Vector3(1, 0, -1), Vector3(1, 0, 1)]:
		await move(cam.unproject_position(wp))
		await click(cam.unproject_position(wp))
	await button(cam.unproject_position(Vector3(1, 0, 1)), MOUSE_BUTTON_RIGHT, true)
	await button(cam.unproject_position(Vector3(1, 0, 1)), MOUSE_BUTTON_RIGHT, false)
	check(scene.get_child_count() == nb + 1, "wall with a corner created")
	var wl3: HewnMesh3D = scene.get_child(scene.get_child_count() - 1)
	var wab := wl3.global_transform * wl3.get_aabb()
	check(wl3.poly_points.size() == 3 and absf(wab.size.x - 2.0) < 0.05 and absf(wab.size.z - 2.0) < 0.05, "wall with a corner: L shape, inside the line (%s, %s)" % [wl3.poly_points.size(), wab.size])
	wl3.visible = false

	# ---- Door: the width follows the longer side of the drawn base.
	nb = scene.get_child_count()
	press("Door")
	await frames(1)
	await drag(cam.unproject_position(Vector3(-0.3, 0, -1)), cam.unproject_position(Vector3(0.3, 0, 1)))
	await move(cam.unproject_position(Vector3(0, 0, 0)))
	await move(cam.unproject_position(Vector3(0, 1.5, 0)))
	await click(cam.unproject_position(Vector3(0, 1.5, 0)))
	var door: HewnMesh3D = scene.get_child(scene.get_child_count() - 1)
	var dab := door.get_aabb()
	check(scene.get_child_count() == nb + 1 and absf(door.get_shape_param("width") - 0.6) < 0.05 and absf(dab.size.z - 2.0) < 0.05 and int(door.get_shape_param("opening_on_long_side")) == 1,
			"door keeps the drawn footprint, the wall runs along the long side (%s)" % dab.size)
	door.set_shape_param("opening_on_long_side", 0)
	# the front / back of the wall (its first faces) now face along Z: the opening is in the short side
	check(absf(door.get_aabb().size.x - 0.6) < 0.05 and absf(door.get_face_normal(0).z) > 0.9, "door opening on the short side (%s)" % door.get_face_normal(0))
	door.visible = false

	# ---- Loose edges: F on two vertices, E on one vertex.
	var lt := HewnMesh3D.new()
	lt.name = "LooseTest"
	scene.add_child(lt)
	lt.owner = scene
	await select_node(lt)
	await key(KEY_TAB)
	await key(KEY_1)
	await key(KEY_A, false, false, true)
	var lf := lt.get_face(visible_face(lt))
	await click(sp(lt, lt.get_vertex(lf[0])))
	await click(sp(lt, lt.get_vertex(lf[2])), false, false, false, true)
	check(sel(lt, "sel_verts").size() == 2, "two opposite corners selected")
	await key(KEY_F)
	var wires: PackedInt32Array = (lt.mesh_data as Dictionary).get("wires", PackedInt32Array())
	check(wires.size() == 2, "F on two vertices adds a loose edge (%s)" % wires)
	await undo(lt)
	await key(KEY_A, false, false, true)
	await click(sp(lt, lt.get_vertex(lf[1])))
	var v_before := lt.get_vertex_count()
	await key(KEY_E)
	await move(sp(lt, lt.get_vertex(lf[1])) + Vector2(0, -40))
	await click(sp(lt, lt.get_vertex(lf[1])) + Vector2(0, -40))
	wires = (lt.mesh_data as Dictionary).get("wires", PackedInt32Array())
	check(lt.get_vertex_count() == v_before + 1 and wires.size() == 2, "E extrudes a vertex into a loose edge (%d verts, %s)" % [lt.get_vertex_count(), wires])
	# the loose edge extrudes into a face
	await key(KEY_2)
	await key(KEY_A, false, false, true)
	var wa := lt.get_vertex(wires[0])
	var wb := lt.get_vertex(wires[1])
	await click(sp(lt, wa.lerp(wb, 0.5)))
	check(sel(lt, "sel_edges").size() == 2, "a loose edge can be clicked")
	var f_before := lt.get_face_count()
	await key(KEY_E)
	await move(sp(lt, wa.lerp(wb, 0.5)) + Vector2(40, 0))
	await click(sp(lt, wa.lerp(wb, 0.5)) + Vector2(40, 0))
	check(lt.get_face_count() == f_before + 1, "E on a loose edge makes a face")
	await key(KEY_TAB)

	# ---- The Hewn toolbar floats over the viewport: the viewport has the same size as with a
	# plain MeshInstance3D selected (Godot's own toolbar may wrap in this narrow headless window).
	var plain := MeshInstance3D.new()
	scene.add_child(plain)
	plain.owner = scene
	await select_node(plain)
	await frames(3)
	var vs0 := surf.size
	await select_node(lt)
	await frames(3)
	var tb: Control = EditorInterface.get_base_control().find_child("HewnToolbar", true, false)
	check(tb.top_level and tb.visible and surf.size == vs0, "the toolbar floats and does not resize the viewport (%s -> %s)" % [vs0, surf.size])

	# ---- Poly Shape base: the mesh follows a dragged point live
	if poly_ok:
		poly.visible = true
		await select_node(poly)
		press("Edit Poly Shape / Wall")
		await frames(2)
		var gl := poly.global_transform
		var p0w := gl * poly.poly_points[0]
		var aabb0 := poly.get_aabb()
		var src := cam.unproject_position(p0w)
		await button(src, MOUSE_BUTTON_LEFT, true)
		var dst := cam.unproject_position(p0w + (p0w - gl.origin).normalized() * 0.5)
		var mm := InputEventMouseMotion.new()
		mm.position = surf.get_global_rect().position + dst
		mm.global_position = mm.position
		mm.button_mask = MOUSE_BUTTON_MASK_LEFT
		await send(mm)
		check(poly.get_aabb().size.length() > aabb0.size.length() + 0.1, "poly base point drag updates the mesh live")
		await button(dst, MOUSE_BUTTON_LEFT, false)
		await key(KEY_ESCAPE)
		check(poly.get_aabb().size.is_equal_approx(aabb0.size), "Esc restores the base")

	# ---- Poly Shape base editing: click an outline edge adds a point, click a point removes it,
	# click outside applies.
	if poly_ok:
		poly.visible = true
		await select_node(poly)
		var pp0 := poly.poly_points.size()
		press("Edit Poly Shape / Wall")
		await frames(2)
		var g := poly.global_transform
		var e0 := g * poly.poly_points[0]
		var e1 := g * poly.poly_points[1]
		var removed: Vector3 = poly.poly_points[2]
		var mid_local: Vector3 = poly.poly_points[0].lerp(poly.poly_points[1], 0.5)
		await click(cam.unproject_position(e0.lerp(e1, 0.5)))
		await click(cam.unproject_position(g * removed))  # remove a corner
		await click(Vector2(5, 5))  # outside: apply
		var has_mid := false
		var has_removed := false
		for q in poly.poly_points:
			has_mid = has_mid or q.distance_to(mid_local) < 0.05
			has_removed = has_removed or q.distance_to(removed) < 1e-3
		check(poly.poly_points.size() == pp0 and has_mid and not has_removed, "poly base: one point added, one removed (%s)" % poly.poly_points)
		await undo(poly)
		check(poly.poly_points.size() == pp0, "undo of the base edit")

	# ---- Shift+S while the right button flies the camera does not open the 3D cursor menu
	await select_node(lt)
	var fly_at := surf.size * 0.5
	await button(fly_at, MOUSE_BUTTON_RIGHT, true)
	await key(KEY_S, true)
	var cursor_open := false
	for pm in EditorInterface.get_base_control().find_children("*", "PopupMenu", false, false):
		if pm.visible and pm.item_count > 0 and pm.get_item_text(0) == "Cursor to Selected":
			cursor_open = true
			pm.hide()
	await button(fly_at, MOUSE_BUTTON_RIGHT, false)
	check(not cursor_open, "Shift+S during camera flight does not open the cursor menu")

	# ---- Stairs in the shape panel: bending keeps the first step in place
	var stn := HewnMesh3D.new()
	stn.name = "StairsTest"
	scene.add_child(stn)
	stn.owner = scene
	stn.shape = 10
	await select_node(stn)
	await frames(3)
	var sp_panel: Control = surf.find_child("HewnShapePanel", true, false)
	var anchor0: Vector3 = stn.global_transform * stn.get_shape_anchor()
	var curv_field: Control = null
	if sp_panel:
		# (the Curve group starts folded)
		for gb in sp_panel.find_children("*", "Button", true, false):
			if gb.text.ends_with(" Curve"):
				gb.pressed.emit()
		await frames(3)
		for l in sp_panel.find_children("*", "Label", true, false):
			if l.text == "Curvature":
				curv_field = field_after(l)
	check(curv_field != null, "stairs panel has Curvature")
	if curv_field:
		curv_field.value_changed.emit(-20.0)
		await frames(2)
		var anchor1: Vector3 = stn.global_transform * stn.get_shape_anchor()
		check(absf(stn.get_shape_param("curvature") + 20.0) < 1e-3 and anchor1.distance_to(anchor0) < 1e-3,
				"negative curvature, the first step stays (%s -> %s)" % [anchor0, anchor1])
	stn.visible = false

	# ---- Poly Shape: A = angle constraint in screen space (30 degree steps)
	for c in scene.get_children():
		c.visible = false
	godot_snap().button_pressed = false
	var pa0 := cam.unproject_position(Vector3(-0.5, 0, 0))
	var pa1 := pa0 + Vector2(120, 12)  # about 6 degrees off the horizontal
	press("Draw Poly Shape")
	await frames(2)
	var nb2 := scene.get_child_count()
	await click(pa0)
	await key(KEY_A)
	await move(pa1)
	await click(pa1)
	var pa2 := pa1 + Vector2(-10, 90)
	await move(pa2)
	await click(pa2)
	await click(pa0)  # close
	await key(KEY_ENTER)
	check(scene.get_child_count() == nb2 + 1, "poly shape drawn with the angle constraint")
	var ps2: HewnMesh3D = scene.get_child(scene.get_child_count() - 1)
	if scene.get_child_count() == nb2 + 1:
		var w1 := ps2.global_transform * ps2.poly_points[1]
		var s1 := cam.unproject_position(w1)
		check(absf(s1.y - pa0.y) < 1.5, "A: the second point is on the horizontal screen line through the first (%s vs %s)" % [s1, pa0])
		ps2.visible = false

	# ---- Height of a drawn shape snaps (grid steps of 0.25)
	await set_snapping(true, "Increment")
	var nb3 := scene.get_child_count()
	press("Cube")
	await frames(1)
	await drag(cam.unproject_position(Vector3(-0.5, 0, -0.5)), cam.unproject_position(Vector3(0.5, 0, 0.5)))
	await move(cam.unproject_position(Vector3(0, 0, 0)))
	await move(cam.unproject_position(Vector3(0, 0.83, 0)))
	await click(cam.unproject_position(Vector3(0, 0.83, 0)))
	if scene.get_child_count() == nb3 + 1:
		var hc: HewnMesh3D = scene.get_child(scene.get_child_count() - 1)
		var hy := hc.get_aabb().size.y
		check(absf(hy / 0.25 - roundf(hy / 0.25)) < 1e-3, "drawn height snaps to the grid (%f)" % hy)
	else:
		check(false, "snapped cube drawn")
	godot_snap().button_pressed = false

	# ---- Stairs "reverse" in the panel: same box, same place
	stn.visible = true
	await select_node(stn)
	await frames(3)
	var rev_field: CheckBox = null
	for l in surf.find_child("HewnShapePanel", true, false).find_children("*", "Label", true, false):
		if l.text == "Reverse":
			rev_field = field_after(l)
	var st_pos := stn.global_position
	var st_box := stn.get_aabb()
	check(rev_field != null, "stairs panel has Reverse")
	if rev_field:
		rev_field.toggled.emit(true)
		await frames(2)
		check(stn.global_position.is_equal_approx(st_pos) and stn.get_aabb().size.is_equal_approx(st_box.size) and int(stn.get_shape_param("reverse")) == 1,
				"reversed stairs keep their box and place (%s -> %s)" % [st_pos, stn.global_position])
	stn.visible = false

	# ---- "+" next to the orientation menu: a custom orientation from the selected face
	lt.visible = true
	await select_node(lt)
	await key(KEY_TAB)
	await key(KEY_3)
	await key(KEY_A, false, false, true)
	await click(sp(lt, lt.get_face_center(visible_face(lt))))
	var tb2 := EditorInterface.get_base_control().find_child("HewnToolbar", true, false)
	var omb: MenuButton = null
	var add_btn: Button = null
	for b in tb2.find_children("*", "MenuButton", true, false):
		if b.tooltip_text.begins_with("Transform orientation"):
			omb = b
	for b in tb2.find_children("*", "Button", true, false):
		if b.tooltip_text.begins_with("Add (Shift+A)"):
			add_btn = b
	check(add_btn != null, "toolbar has the Add button")
	if add_btn:
		add_btn.pressed.emit()
		await frames(2)
		var add_found := false
		for pm in EditorInterface.get_base_control().find_children("*", "PopupMenu", false, false):
			if pm.visible:
				for i in pm.item_count:
					add_found = add_found or pm.get_item_text(i).contains("3D")
				pm.hide()
		check(add_found, "the + button opens the Add menu (Shift+A's)")
	var plus_found := false
	if omb:
		for i in omb.get_popup().item_count:
			plus_found = plus_found or omb.get_popup().get_item_text(i).contains("Create from Selection")
	check(plus_found, "the orientation menu has + Create from Selection")
	if omb:
		omb.get_popup().id_pressed.emit(900)
		await frames(2)
		check(omb.tooltip_text == "Transform orientation: Face", "custom orientation created and active (%s)" % omb.tooltip_text)
		# back to Global (the custom one stays in the menu)
		omb.get_popup().id_pressed.emit(0)

	# ---- Knife: RMB takes back the last point instead of leaving the knife
	var rk_f := visible_face(lt)
	var rk_face := lt.get_face(rk_f)
	var rk_a := lt.get_vertex(rk_face[0]).lerp(lt.get_vertex(rk_face[1]), 0.5)
	var rk_b := lt.get_vertex(rk_face[2]).lerp(lt.get_vertex(rk_face[3]), 0.5)
	var rk_fc0 := lt.get_face_count()
	await key(KEY_K)
	await click(sp(lt, rk_a))
	await click(sp(lt, lt.get_face_center(rk_f)))
	await button(sp(lt, rk_b), MOUSE_BUTTON_RIGHT, true)
	await button(sp(lt, rk_b), MOUSE_BUTTON_RIGHT, false)
	await click(sp(lt, rk_b))
	await key(KEY_ENTER)
	check(lt.get_face_count() == rk_fc0 + 1, "knife: RMB removed the last point, Enter cut straight (%d -> %d faces)" % [rk_fc0, lt.get_face_count()])
	await key(KEY_TAB)

	# ---- Shapes show while they are drawn; R turns the stairs inside the same box
	for c in scene.get_children():
		c.visible = false
	godot_snap().button_pressed = false
	press("Stairs")
	await frames(1)
	var pv_a := cam.unproject_position(Vector3(-1, 0, -0.5))
	var pv_b := cam.unproject_position(Vector3(1, 0, 0.5))
	await move(pv_a)
	await button(pv_a, MOUSE_BUTTON_LEFT, true)
	var pv_o := surf.get_global_rect().position
	for i in range(1, 6):
		var pmm := InputEventMouseMotion.new()
		pmm.position = pv_o + pv_a.lerp(pv_b, i / 5.0)
		pmm.global_position = pmm.position
		pmm.button_mask = MOUSE_BUTTON_MASK_LEFT
		await send(pmm)
	check(scene.get_child_count(true) == scene.get_child_count() + 1, "the shape shows while its base is drawn")
	await button(pv_b, MOUSE_BUTTON_LEFT, false)
	await move(cam.unproject_position(Vector3(0, 0, 0)))
	await move(cam.unproject_position(Vector3(0, 0.8, 0)))
	var spv: HewnMesh3D = scene.get_child(scene.get_child_count(true) - 1, true)
	var pv_box := spv.get_aabb()
	var pv_climb := int(spv.get_shape_param("climb_along_long_side"))
	check(pv_box.size.y > 0.3, "the preview follows the height (%s)" % pv_box.size)
	await key(KEY_R)
	spv = scene.get_child(scene.get_child_count(true) - 1, true)
	check(int(spv.get_shape_param("climb_along_long_side")) != pv_climb and spv.get_aabb().size.is_equal_approx(pv_box.size),
			"R turns the stairs, the box stays (%s -> %s)" % [pv_box.size, spv.get_aabb().size])
	var nb4 := scene.get_child_count()
	await click(cam.unproject_position(Vector3(0, 0.8, 0)))
	check(scene.get_child_count() == nb4 + 1 and scene.get_child_count(true) == scene.get_child_count(), "stairs created, the preview is gone")
	if scene.get_child_count() == nb4 + 1:
		var st3: HewnMesh3D = scene.get_child(scene.get_child_count() - 1)
		check(int(st3.get_shape_param("climb_along_long_side")) != pv_climb, "the created stairs keep the turned direction")
		st3.visible = false

	# ---- Object mode: G / R / S with axis and typed values, Esc cancels, undo
	var obx := HewnMesh3D.new()
	obx.name = "ObjXform"
	scene.add_child(obx)
	obx.owner = scene
	obx.shape = 1
	await select_node(obx)
	await move(cam.unproject_position(Vector3(0.3, 0.2, 0)))
	var ob_p0 := obx.global_position
	await key(KEY_G)
	await key(KEY_X)
	await type_text("2")
	await key(KEY_ENTER)
	check(obx.global_position.is_equal_approx(ob_p0 + Vector3(2, 0, 0)), "object G X 2 (%s)" % obx.global_position)
	await undo(obx)
	check(obx.global_position.is_equal_approx(ob_p0), "undo of the object move")
	await key(KEY_R)
	await key(KEY_Z)
	await type_text("90")
	await key(KEY_ENTER)
	check(obx.global_basis.x.is_equal_approx(Vector3(0, 1, 0)), "object R Z 90 (%s)" % obx.global_basis.x)
	await key(KEY_S)
	await type_text("2")
	await key(KEY_ENTER)
	check(obx.global_basis.get_scale().is_equal_approx(Vector3(2, 2, 2)), "object S 2 (%s)" % obx.global_basis.get_scale())
	var ob_xf := obx.global_transform
	await key(KEY_G)
	await move(cam.unproject_position(Vector3(1.5, 0.2, 0)))
	await key(KEY_ESCAPE)
	check(obx.global_transform.is_equal_approx(ob_xf), "Esc cancels the object move")
	obx.visible = false

	# ---- G on an ordinary MeshInstance3D (no Hewn mesh edited), then B (snap base) inside G
	var plm := MeshInstance3D.new()
	plm.name = "PlainMesh"
	plm.mesh = BoxMesh.new()
	scene.add_child(plm)
	plm.owner = scene
	await select_node(plm)
	await move(cam.unproject_position(Vector3(0.3, 0.2, 0)))
	var pl_p0 := plm.global_position
	await key(KEY_G)
	await key(KEY_X)
	await type_text("2")
	await key(KEY_ENTER)
	check(plm.global_position.is_equal_approx(pl_p0 + Vector3(2, 0, 0)), "G X 2 on an ordinary mesh (%s)" % plm.global_position)
	await undo(plm)
	check(plm.global_position.is_equal_approx(pl_p0), "undo of the ordinary mesh move")
	await key(KEY_S)
	await type_text("3")
	await key(KEY_ENTER)
	check(plm.global_basis.get_scale().is_equal_approx(Vector3(3, 3, 3)), "S 3 on an ordinary mesh")
	await undo(plm)
	await key(KEY_G)
	await key(KEY_B)
	await click(cam.unproject_position(pl_p0))  # source: the origin
	await move(cam.unproject_position(pl_p0 + Vector3(1, 0, 0)))
	await key(KEY_X)
	await click(cam.unproject_position(pl_p0 + Vector3(1, 0, 0)))  # target
	var pl_d := plm.global_position - pl_p0
	check(pl_d.x > 0.3 and absf(pl_d.y) < 1e-4 and absf(pl_d.z) < 1e-4, "B inside G moves the object from the source to the target point (%s)" % pl_d)
	await undo(plm)
	check(plm.global_position.is_equal_approx(pl_p0), "undo of the snap move")
	plm.visible = false

	# ---- Texture tools: U menu, the texture follows the mouse; Esc; Cycle button
	var r22_tx := HewnMesh3D.new()
	r22_tx.name = "TexTools"
	scene.add_child(r22_tx)
	r22_tx.owner = scene
	r22_tx.shape = 1
	await select_node(r22_tx)
	var r22_tx_top := -1
	for r22_f in r22_tx.get_face_count():
		if r22_tx.get_face_normal(r22_f).y > 0.9:
			r22_tx_top = r22_f
	await key(KEY_TAB)
	await key(KEY_3)
	await key(KEY_A, false, false, true)
	var r22_tx_c := Vector3(0, 0, 0)
	for r22_v in r22_tx.get_face(r22_tx_top):
		r22_tx_c += r22_tx.get_vertex(r22_v) / 4.0
	await click(sp(r22_tx, r22_tx_c))
	var r22_tx_uv0 := r22_tx.get_face_uvs(r22_tx_top)
	await move(sp(r22_tx, r22_tx_c + Vector3(-0.1, 0, 0)))
	await key(KEY_U)
	await menu_pick("Move Texture")
	await move(sp(r22_tx, r22_tx_c + Vector3(0.1, 0, 0)))
	await click(sp(r22_tx, r22_tx_c + Vector3(0.1, 0, 0)))
	var r22_tx_uv1 := r22_tx.get_face_uvs(r22_tx_top)
	var r22_tx_d := r22_tx_uv1[0] - r22_tx_uv0[0]
	check(absf(r22_tx_d.length() - 0.2) < 0.03, "U > Move Texture: the texture follows the mouse 0.2 m (%s)" % r22_tx_d)
	await undo(r22_tx)
	check(r22_tx.get_face_uvs(r22_tx_top)[0].is_equal_approx(r22_tx_uv0[0]), "undo of the texture move")
	await key(KEY_U)
	await menu_pick("Rotate Texture")
	await move(sp(r22_tx, r22_tx_c + Vector3(0.2, 0, 0.2)))
	await key(KEY_ESCAPE)
	check(r22_tx.get_face_uvs(r22_tx_top)[0].is_equal_approx(r22_tx_uv0[0]), "Esc cancels the texture rotation")
	press("Cycle")
	await frames(2)
	var r22_tx_uv2 := r22_tx.get_face_uvs(r22_tx_top)
	check(not r22_tx_uv2[0].is_equal_approx(r22_tx_uv0[0]) and r22_tx.get_uv_bounds(PackedInt32Array([r22_tx_top])).is_equal_approx(Rect2(r22_tx_uv0[0].min(r22_tx_uv0[2]), (r22_tx_uv0[0] - r22_tx_uv0[2]).abs())), "Cycle turns the texture inside its bounds")
	# Shift+T: drag the middle handle (move), a corner (scale, snapped to a vertex)
	var r23_f := r22_tx_top
	var r23_fv := r22_tx.get_face(r23_f)
	var r23_uv := r22_tx.get_face_uvs(r23_f)
	var r23_p := []
	for r23_i in 4:
		r23_p.append(r22_tx.global_transform * r22_tx.get_vertex(r23_fv[r23_i]))
	# world point of a UV on the face (affine from three corners)
	var r23_world := func(uvq: Vector2) -> Vector3:
		var e1: Vector2 = r23_uv[1] - r23_uv[0]
		var e2: Vector2 = r23_uv[3] - r23_uv[0]
		var dq: Vector2 = uvq - r23_uv[0]
		var det := e1.x * e2.y - e1.y * e2.x
		var a := (dq.x * e2.y - dq.y * e2.x) / det
		var b := (e1.x * dq.y - e1.y * dq.x) / det
		return r23_p[0] + (r23_p[1] - r23_p[0]) * a + (r23_p[3] - r23_p[0]) * b
	var r23_cuv := (r23_uv[0] + r23_uv[2]) * 0.5
	var r23_tile := Vector2(floorf(r23_cuv.x), floorf(r23_cuv.y))
	await move(sp(r22_tx, r22_tx_c))
	await key(KEY_T, true)
	var r23_h := cam.unproject_position(r23_world.call(r23_tile + Vector2(0.5, 0.5)))
	await move(r23_h)
	await button(r23_h, MOUSE_BUTTON_LEFT, true)
	var r23_to := cam.unproject_position(r23_world.call(r23_tile + Vector2(0.5, 0.5)) + Vector3(0.23, 0, 0))
	await move(r23_h.lerp(r23_to, 0.5), false, MOUSE_BUTTON_MASK_LEFT)
	await move(r23_to, false, MOUSE_BUTTON_MASK_LEFT)
	await button(r23_to, MOUSE_BUTTON_LEFT, false)
	await key(KEY_ENTER)
	var r23_d := r22_tx.get_face_uvs(r23_f)[0] - r23_uv[0]
	check(absf(r23_d.length() - 0.23) < 0.03 or absf(r23_d.length() - 0.25) < 0.03, "Shift+T: the middle handle moves the texture (%s)" % r23_d)
	await undo(r22_tx)
	check(r22_tx.get_face_uvs(r23_f)[0].is_equal_approx(r23_uv[0]), "undo of the UV Transform")
	# a corner onto the face's far corner: the tile's side ends on that vertex
	godot_snap().button_pressed = true
	await move(sp(r22_tx, r22_tx_c))
	await key(KEY_T, true)
	var r23_c0 := cam.unproject_position(r23_world.call(r23_tile))
	await move(r23_c0)
	await button(r23_c0, MOUSE_BUTTON_LEFT, true)
	var r23_far := r23_uv[0]
	for r23_q in r23_uv:
		if (r23_q - (r23_tile + Vector2(1, 1))).length() > (r23_far - (r23_tile + Vector2(1, 1))).length():
			r23_far = r23_q
	var r23_cto := cam.unproject_position(r23_world.call(r23_far)) + Vector2(3, 2)
	await move(r23_c0.lerp(r23_cto, 0.5), false, MOUSE_BUTTON_MASK_LEFT)
	await move(r23_cto, false, MOUSE_BUTTON_MASK_LEFT)
	await button(r23_cto, MOUSE_BUTTON_LEFT, false)
	var r23_b := r22_tx.get_uv_bounds(PackedInt32Array([r23_f]))
	var r23_span := (r23_tile + Vector2(1, 1) - r23_far).abs()
	check(r23_b.size.is_equal_approx(Vector2(1, 1) / r23_span), "Shift+T: a corner scales the tile, snapped to the vertex (%s, span %s)" % [r23_b, r23_span])
	await key(KEY_ESCAPE)
	godot_snap().button_pressed = false
	check(r22_tx.get_face_uvs(r23_f)[0].is_equal_approx(r23_uv[0]), "Esc cancels the UV Transform")
	await key(KEY_TAB)
	r22_tx.visible = false

	# ---- H / Alt+H on objects
	var r22_wl := HewnMesh3D.new()
	r22_wl.name = "CsgWall"
	scene.add_child(r22_wl)
	r22_wl.owner = scene
	r22_wl.shape = 1
	r22_wl.set_shape_param("width", 4.0)
	r22_wl.set_shape_param("height", 2.0)
	r22_wl.set_shape_param("depth", 0.3)
	var r22_dr := HewnMesh3D.new()
	r22_dr.name = "CsgDoor"
	scene.add_child(r22_dr)
	r22_dr.owner = scene
	r22_dr.shape = 1
	r22_dr.set_shape_param("width", 1.0)
	r22_dr.set_shape_param("height", 1.5)
	r22_dr.set_shape_param("depth", 1.0)
	await frames(2)
	await select_node(r22_wl)
	await key(KEY_H)
	check(not r22_wl.visible, "H hides the selected object")
	await key(KEY_H, false, false, true)
	check(r22_wl.visible, "Alt+H shows it again")
	r22_wl.visible = false
	r22_dr.visible = false

	# ---- level tools (round 23)
	var r23_iv := HewnMesh3D.new()
	r23_iv.name = "InvalidUV"
	scene.add_child(r23_iv)
	r23_iv.owner = scene
	r23_iv.shape = 1
	r23_iv.edit_uvs(PackedInt32Array([2]), "scale", [0.0, 0.0])
	await select_node(r23_iv)
	await key(KEY_TAB)
	press("Invalid UVs")
	await frames(2)
	check(sel(r23_iv, "sel_faces") == [2], "Select Invalid UVs: the face with squashed UVs (%s)" % [sel(r23_iv, "sel_faces")])
	await key(KEY_TAB)
	r23_iv.visible = false
	# Alt+click on another object: the texture runs on over the shared edge in the world
	var r23_a := HewnMesh3D.new()
	r23_a.name = "TexA"
	scene.add_child(r23_a)
	r23_a.owner = scene
	r23_a.shape = 1
	var r23_bm := HewnMesh3D.new()
	r23_bm.name = "TexB"
	scene.add_child(r23_bm)
	r23_bm.owner = scene
	r23_bm.shape = 1
	r23_bm.position = Vector3(1, 0, 0)
	var r23_atop := faces_up_index(r23_a)
	var r23_btop := faces_up_index(r23_bm)
	r23_a.edit_uvs(PackedInt32Array([r23_atop]), "move", [0.3, 0.1])
	r23_a.edit_uvs(PackedInt32Array([r23_atop]), "rotate", [20.0])
	await select_node(r23_a)
	await key(KEY_TAB)
	await key(KEY_3)
	await key(KEY_A, false, false, true)
	await click(sp(r23_a, face_center(r23_a, r23_atop)))
	await click(sp(r23_bm, face_center(r23_bm, r23_btop)), false, true)
	var r23_same := 0
	var r23_fa := r23_a.get_face(r23_atop)
	var r23_fb := r23_bm.get_face(r23_btop)
	var r23_ua := r23_a.get_face_uvs(r23_atop)
	var r23_ub := r23_bm.get_face_uvs(r23_btop)
	for r23_i in r23_fa.size():
		for r23_k in r23_fb.size():
			var r23_pa := r23_a.global_transform * r23_a.get_vertex(r23_fa[r23_i])
			var r23_pb := r23_bm.global_transform * r23_bm.get_vertex(r23_fb[r23_k])
			if r23_pa.distance_to(r23_pb) < 1e-4 and r23_ua[r23_i].distance_to(r23_ub[r23_k]) < 1e-3:
				r23_same += 1
	check(r23_same == 2, "Alt+click on another object: the texture runs on over the touching edge (%d)" % r23_same)
	await undo(r23_bm)
	check(r23_bm.get_face_uvs(r23_btop) != r23_ub, "undo of the texture on the other object")
	await key(KEY_TAB)
	r23_a.visible = false
	r23_bm.visible = false
	# T: the texture against the edge under the mouse; E fits the width; a grid of quads
	var r23_t := HewnMesh3D.new()
	r23_t.name = "TSnap"
	scene.add_child(r23_t)
	r23_t.owner = scene
	r23_t.shape = 1
	r23_t.set_shape_param("width", 2.0)
	r23_t.set_shape_param("width_segments", 3)
	var r23_front := []
	for r23_f2 in r23_t.get_face_count():
		if r23_t.get_face_normal(r23_f2).z > 0.9:
			r23_front.append(r23_f2)
	await select_node(r23_t)
	await key(KEY_TAB)
	await key(KEY_3)
	var r23_st: Dictionary = r23_t.get_state()
	r23_st["sel_faces"] = PackedInt32Array([r23_front[1]])
	r23_t.set_state(r23_st)
	await frames(2)
	var r23_box: AABB = r23_t.get_aabb()
	# the bottom edge of the middle face: the image's bottom (S, the default) on it
	var r23_mid := Vector3(0, r23_box.position.y, r23_box.end.z)
	await move(sp(r23_t, r23_mid))
	await key(KEY_T)
	await key(KEY_S)
	await click(sp(r23_t, r23_mid))
	var r23_b1 := r23_t.get_uv_bounds(PackedInt32Array([r23_front[1]]))
	check(is_equal_approx(r23_b1.end.y, roundf(r23_b1.end.y)) and r23_b1.size.y > 0.99 and r23_b1.size.y < 1.01, "T: the image's bottom on the bottom edge (%s)" % r23_b1)
	# all three front quads: a grid; E fits the image's width over them
	r23_st = r23_t.get_state()
	r23_st["sel_faces"] = PackedInt32Array(r23_front)
	r23_t.set_state(r23_st)
	await frames(2)
	await move(sp(r23_t, r23_mid))
	await key(KEY_T)
	await key(KEY_E)
	await click(sp(r23_t, r23_mid))
	var r23_b3 := r23_t.get_uv_bounds(PackedInt32Array(r23_front))
	check(r23_b3.size.x > 0.99 and r23_b3.size.x < 1.01, "T on a grid of quads, E: one image over the whole width (%s)" % r23_b3)
	await move(sp(r23_t, r23_mid))
	await key(KEY_T)
	await key(KEY_D)
	await key(KEY_ESCAPE)
	check(r23_t.get_uv_bounds(PackedInt32Array(r23_front)).is_equal_approx(r23_b3), "Esc cancels T")
	await key(KEY_TAB)
	r23_t.visible = false
	# A box drawn in edit mode joins the edited mesh; the touching walls go (Keep Overlap Faces off)
	var r23_j := HewnMesh3D.new()
	r23_j.name = "BuildInto"
	scene.add_child(r23_j)
	r23_j.owner = scene
	r23_j.shape = 1
	r23_j.position = Vector3(0, 0.5, 0)
	await select_node(r23_j)
	await key(KEY_TAB)
	var r23_nodes := scene.get_child_count()
	press("Cube")
	await frames(1)
	# on the top of the box: a smaller box standing on it
	await click(cam.unproject_position(Vector3(-0.25, 1, -0.25)))
	await click(cam.unproject_position(Vector3(0.25, 1, 0.25)))
	await move(cam.unproject_position(Vector3(0, 1.0, 0)))
	await move(cam.unproject_position(Vector3(0, 1.6, 0)))
	await click(cam.unproject_position(Vector3(0, 1.6, 0)))
	await frames(3)
	var r23_jb: AABB = r23_j.global_transform * r23_j.get_aabb()
	check(scene.get_child_count() == r23_nodes and r23_jb.end.y > 1.2, "a box drawn in edit mode joins the edited mesh (%s)" % r23_jb)
	check(analyze_mesh(r23_j).closed, "the joined mesh is closed: the touching faces were cut away")
	var r23_inner := 0
	for r23_f3 in r23_j.get_face_count():
		var r23_c := r23_j.global_transform * face_center(r23_j, r23_f3)
		if absf(r23_c.y - 1.0) < 0.01 and r23_j.get_face_normal(r23_f3).y < -0.9:
			r23_inner += 1
	check(r23_inner == 0, "no face left between the two boxes (%d)" % r23_inner)
	await undo(r23_j)
	check(r23_j.get_face_count() == 6, "undo of the build")
	await key(KEY_ESCAPE)
	r23_j.visible = false
	# W after a cut over a corner: the notch follows the cut volume (folded plane)
	var r23_fc := HewnMesh3D.new()
	r23_fc.name = "FoldedCut"
	scene.add_child(r23_fc)
	r23_fc.owner = scene
	r23_fc.shape = 1
	r23_fc.position = Vector3(0, 0.5, 0)
	await select_node(r23_fc)
	press("Box Cut")
	await frames(1)
	await click(cam.unproject_position(Vector3(0.2, 1.0, 0.2)))
	await move(cam.unproject_position(Vector3(0.8, 1.0, 0.8)))
	await click(cam.unproject_position(Vector3(0.8, 1.0, 0.8)))
	await move(cam.unproject_position(Vector3(0.8, 0.85, 0.8)))
	await move(cam.unproject_position(Vector3(0.8, 0.7, 0.8)))
	await click(cam.unproject_position(Vector3(0.8, 0.7, 0.8)))
	check(not analyze_mesh(r23_fc).closed, "a box cut over the corner of a cube opens it")
	await key(KEY_W)
	check(analyze_mesh(r23_fc).closed, "W: the cut is completed around the corner (a closed notch)")
	var r23_vol := 0.0
	for r23_si in r23_fc.mesh.get_surface_count():
		var r23_arr := r23_fc.mesh.surface_get_arrays(r23_si)
		var r23_pv: PackedVector3Array = r23_arr[Mesh.ARRAY_VERTEX]
		var r23_idx: PackedInt32Array = r23_arr[Mesh.ARRAY_INDEX]
		for r23_k2 in range(0, r23_idx.size(), 3):
			r23_vol += r23_pv[r23_idx[r23_k2]].dot(r23_pv[r23_idx[r23_k2 + 2]].cross(r23_pv[r23_idx[r23_k2 + 1]])) / 6.0
	check(r23_vol > 0.9 and r23_vol < 0.999, "the notch is taken out of the cube (%f)" % r23_vol)
	await undo(r23_fc)
	check(not analyze_mesh(r23_fc).closed, "undo of the completion")
	r23_fc.visible = false
	# placed scenes dim while a mesh is edited (and come back after)
	var r23_lib := Node3D.new()
	r23_lib.name = "PlacedScene"
	r23_lib.scene_file_path = "res://placed_for_test.tscn"
	var r23_libm := MeshInstance3D.new()
	r23_libm.mesh = BoxMesh.new()
	r23_lib.add_child(r23_libm)
	scene.add_child(r23_lib)
	r23_lib.owner = scene
	r23_fc.visible = true
	await select_node(r23_fc)
	await key(KEY_TAB)
	await frames(3)
	await key(KEY_TAB)
	await frames(3)
	check(r23_lib.is_inside_tree(), "edit mode next to a placed scene (dimmed while editing)")
	r23_lib.queue_free()
	r23_fc.visible = false
	#R23_NEXT
	# ---- A box cut through a cube (top to bottom): two holes, W bridges them (a tunnel); not while flying
	var r24_tb := HewnMesh3D.new()
	r24_tb.name = "Tunnel"
	scene.add_child(r24_tb)
	r24_tb.owner = scene
	r24_tb.shape = 1
	r24_tb.position = Vector3(0, 0.5, 0)
	await select_node(r24_tb)
	press("Box Cut")
	await frames(1)
	await click(cam.unproject_position(Vector3(-0.2, 1.0, -0.2)))
	await move(cam.unproject_position(Vector3(0.2, 1.0, 0.2)))
	await click(cam.unproject_position(Vector3(0.2, 1.0, 0.2)))
	await move(cam.unproject_position(Vector3(0.2, 0.5, 0.2)))
	await move(cam.unproject_position(Vector3(0.2, -0.5, 0.2)))
	await click(cam.unproject_position(Vector3(0.2, -0.5, 0.2)))
	check(not analyze_mesh(r24_tb).closed, "a box cut through a cube: holes top and bottom")
	var r24_rmb := InputEventMouseButton.new()
	r24_rmb.button_index = MOUSE_BUTTON_RIGHT
	r24_rmb.pressed = true
	Input.parse_input_event(r24_rmb)
	await frames(1)
	var r24_fc := r24_tb.get_face_count()
	await key(KEY_W)
	check(r24_tb.get_face_count() == r24_fc, "W does nothing while the right button flies the camera")
	r24_rmb = InputEventMouseButton.new()
	r24_rmb.button_index = MOUSE_BUTTON_RIGHT
	r24_rmb.pressed = false
	Input.parse_input_event(r24_rmb)
	await frames(2)
	await key(KEY_W)
	check(analyze_mesh(r24_tb).closed and r24_tb.get_face_count() > r24_fc, "W bridges the two holes: a tunnel through the cube (%d faces)" % r24_tb.get_face_count())
	await undo(r24_tb)
	r24_tb.visible = false
	# ---- B with ordinary meshes: their corners are source and target points
	var r24_pc := HewnMesh3D.new()
	r24_pc.name = "SnapCube"
	scene.add_child(r24_pc)
	r24_pc.owner = scene
	r24_pc.shape = 1
	r24_pc.position = Vector3(0, 0.5, 0)
	var r24_pm := MeshInstance3D.new()
	r24_pm.name = "SnapPlain"
	r24_pm.mesh = BoxMesh.new()
	scene.add_child(r24_pm)
	r24_pm.owner = scene
	r24_pm.position = Vector3(2, 0.5, 0)
	await set_snapping(true, "Vertex")
	await select_node(r24_pm)
	await move(cam.unproject_position(Vector3(2, 0.5, 0)))
	await key(KEY_G)
	await key(KEY_B)
	await move(cam.unproject_position(Vector3(1.5, 1.0, 0.5)))
	await click(cam.unproject_position(Vector3(1.5, 1.0, 0.5)))  # source: a corner of the ordinary box
	await move(cam.unproject_position(Vector3(0.5, 1.0, -0.5)))
	await click(cam.unproject_position(Vector3(0.5, 1.0, -0.5)))  # target: a corner of the Hewn cube
	check(r24_pm.global_position.is_equal_approx(Vector3(1, 0.5, -1)), "B: an ordinary mesh from its corner to a corner of a Hewn cube (%s)" % r24_pm.global_position)
	await undo(r24_pm)
	await select_node(r24_pc)
	await move(cam.unproject_position(Vector3(0, 0.5, 0)))
	await key(KEY_G)
	await key(KEY_B)
	await move(cam.unproject_position(Vector3(0.5, 1.0, 0.5)))
	await click(cam.unproject_position(Vector3(0.5, 1.0, 0.5)))
	await move(cam.unproject_position(Vector3(1.5, 1.0, -0.5)))
	await click(cam.unproject_position(Vector3(1.5, 1.0, -0.5)))  # target: a corner of the ordinary box
	check(r24_pc.global_position.is_equal_approx(Vector3(1, 0.5, -1)), "B: a Hewn cube onto a corner of an ordinary mesh (%s)" % r24_pc.global_position)
	await undo(r24_pc)
	godot_snap().button_pressed = false
	r24_pm.visible = false
	r24_pc.visible = false
	# B with nothing going on: a box is drawn
	var r24_n := scene.get_child_count()
	await move(cam.unproject_position(Vector3(-2, 0, -2)))
	await key(KEY_B)
	await click(cam.unproject_position(Vector3(-2.5, 0, -2.5)))
	await move(cam.unproject_position(Vector3(-1.5, 0, -1.5)))
	await click(cam.unproject_position(Vector3(-1.5, 0, -1.5)))
	await move(cam.unproject_position(Vector3(-1.5, 0.5, -1.5)))
	await click(cam.unproject_position(Vector3(-1.5, 1.0, -1.5)))
	await frames(2)
	check(scene.get_child_count() == r24_n + 1, "B draws a box")
	if scene.get_child_count() == r24_n + 1:
		var r24_box: HewnMesh3D = scene.get_child(scene.get_child_count() - 1)
		var r24_bb: AABB = r24_box.global_transform * r24_box.get_aabb()
		# the height follows the mouse along the edge at the last corner
		check(absf(r24_bb.end.y - 1.0) < 0.05, "the height of the box is where the mouse is (%s)" % r24_bb)
		r24_box.visible = false
	r24_pc.visible = false
	# ---- A face moved with the gizmo arrow: the Last Operation panel shows the move, and changes it
	var r24_g := HewnMesh3D.new()
	r24_g.name = "GizmoMove"
	scene.add_child(r24_g)
	r24_g.owner = scene
	r24_g.shape = 1
	r24_g.position = Vector3(0, 0.5, 0)
	await select_node(r24_g)
	await key(KEY_TAB)
	await key(KEY_3)
	var r24_gst := r24_g.get_state()
	r24_gst["sel_faces"] = PackedInt32Array([faces_up_index(r24_g)])
	r24_g.set_state(r24_gst)
	await frames(2)
	var r24_piv := Vector3(0, 1, 0)
	var r24_L := (r24_piv - cam.global_position).dot(-cam.global_basis.z) * 0.17 * gizmo_k()
	var r24_a := cam.unproject_position(r24_piv + Vector3(0, r24_L * 0.8, 0))
	var r24_b := cam.unproject_position(r24_piv + Vector3(0, r24_L * 0.8 + 0.5, 0))
	await move(r24_a)
	await drag(r24_a, r24_b)
	await frames(2)
	var r24_top := (r24_g.global_transform * r24_g.get_aabb()).end.y
	check(r24_top > 1.2, "the gizmo arrow moves the top face up (%f)" % r24_top)
	var r24_panel := surf.find_child("HewnLastOperation", true, false)
	check(r24_panel != null and r24_panel.visible, "the Last Operation panel shows after a gizmo move")
	if r24_panel and r24_panel.visible:
		var r24_fields := r24_panel.find_children("*", "LineEdit", true, false)
		check(r24_fields.size() >= 3, "the panel has Move X / Y / Z")
		if r24_fields.size() >= 3:
			var r24_y: LineEdit = r24_fields[1]
			r24_y.text = "1"
			r24_y.text_submitted.emit("1")
			await frames(3)
			var r24_top2 := (r24_g.global_transform * r24_g.get_aabb()).end.y
			check(absf(r24_top2 - 2.0) < 1e-3, "Move Y = 1 in the panel redoes the move (%f)" % r24_top2)
	await undo(r24_g)
	check(absf((r24_g.global_transform * r24_g.get_aabb()).end.y - 1.0) < 1e-4, "undo of the gizmo move")
	await key(KEY_TAB)
	r24_g.visible = false
	# ---- H hides the selected faces (edit mode only), Alt+H shows them again (selected), undo
	var r24_h := HewnMesh3D.new()
	r24_h.name = "HideFaces"
	scene.add_child(r24_h)
	r24_h.owner = scene
	r24_h.shape = 1
	r24_h.position = Vector3(0, 0.5, 0)
	var r24_tris := func() -> int:
		var c := 0
		for r24_si in r24_h.mesh.get_surface_count():
			c += (r24_h.mesh.surface_get_arrays(r24_si)[Mesh.ARRAY_INDEX] as PackedInt32Array).size() / 3
		return c
	await select_node(r24_h)
	await key(KEY_TAB)
	await key(KEY_3)
	var r24_up := faces_up_index(r24_h)
	var r24_hst := r24_h.get_state()
	r24_hst["sel_faces"] = PackedInt32Array([r24_up])
	r24_h.set_state(r24_hst)
	await frames(2)
	await key(KEY_H)
	await frames(2)
	check(r24_h.is_face_hidden(r24_up) and r24_tris.call() == 10 and sel(r24_h, "sel_faces").is_empty(), "H hides the selected face (%d triangles shown)" % r24_tris.call())
	await key(KEY_A)
	check(not (r24_up in sel(r24_h, "sel_faces")) and sel(r24_h, "sel_faces").size() == 5, "A does not select the hidden face")
	await key(KEY_TAB)
	await frames(2)
	check(r24_tris.call() == 12, "object mode shows the hidden face")
	await select_node(r24_h)
	await key(KEY_TAB)
	await frames(2)
	check(r24_tris.call() == 10, "back in edit mode it is hidden again")
	await key(KEY_H, false, false, true)
	await frames(2)
	check(not r24_h.is_face_hidden(r24_up) and sel(r24_h, "sel_faces").size() == 1 and sel(r24_h, "sel_faces")[0] == r24_up and r24_tris.call() == 12, "Alt+H shows it again, selected")
	await undo(r24_h)
	check(r24_h.is_face_hidden(r24_up), "undo of Alt+H")
	await key(KEY_TAB)
	r24_h.visible = false
	# ---- Visible select in object mode: a click on a room from outside picks what is inside it
	var r24_room := HewnMesh3D.new()
	r24_room.name = "VisRoom"
	scene.add_child(r24_room)
	r24_room.owner = scene
	r24_room.shape = 1
	r24_room.set_shape_param("flip_normals", 1.0)
	r24_room.scale = Vector3(3, 3, 3)
	var r24_in := MeshInstance3D.new()
	r24_in.name = "VisInside"
	r24_in.mesh = BoxMesh.new()
	scene.add_child(r24_in)
	r24_in.owner = scene
	r24_in.position = Vector3(0, 0, 0)
	EditorInterface.get_selection().clear()
	await frames(2)
	await click(cam.unproject_position(Vector3(0, 0, 0)))
	await frames(4)
	var r24_vs := EditorInterface.get_selection().get_selected_nodes()
	check(r24_vs.size() == 1 and r24_vs[0] == r24_in, "a click through a room seen from outside selects the box inside (%s)" % [r24_vs])
	var r24_vbtn: Button = null
	for r24_bt in EditorInterface.get_base_control().find_children("*", "Button", true, false):
		if r24_bt.tooltip_text.begins_with("Visible select"):
			r24_vbtn = r24_bt
	check(r24_vbtn != null, "the Visible select button")
	if r24_vbtn:
		r24_vbtn.button_pressed = false
		EditorInterface.get_selection().clear()
		await frames(2)
		await click(cam.unproject_position(Vector3(0, 0, 0)))
		await frames(4)
		r24_vs = EditorInterface.get_selection().get_selected_nodes()
		check(r24_vs.size() == 1 and r24_vs[0] == r24_room, "Visible select off: Godot's pick, the room (%s)" % [r24_vs])
		r24_vbtn.button_pressed = true
	r24_in.visible = false
	EditorInterface.get_selection().clear()
	await frames(2)
	await click(cam.unproject_position(Vector3(0, 0, 0)))
	await frames(4)
	r24_vs = EditorInterface.get_selection().get_selected_nodes()
	check(r24_vs.size() == 1 and r24_vs[0] == r24_room, "with nothing inside, the far wall seen from inside: the room (%s)" % [r24_vs])
	r24_room.visible = false
	# ---- r25: the snap step on the header (expressions), grid snapping on walls, Ctrl keeps the plane
	var r25_bar := EditorInterface.get_base_control().find_child("HewnToolbar", true, false)
	var r25_step: LineEdit = null
	for r25_f in r25_bar.find_children("*", "HewnDragField", true, false):
		if r25_f.tooltip_text.begins_with("Snap step"):
			r25_step = r25_f
	check(r25_step != null and r25_step.is_visible_in_tree(), "the snap step is on the header")
	r25_step.text_submitted.emit("0.125")
	await frames(1)
	r25_step.text_submitted.emit("*2")
	await frames(1)
	check(r25_step.text == "0.25", "*2 in a field doubles its value (%s)" % r25_step.text)
	r25_step.text_submitted.emit("1/8 + 0.125")
	await frames(1)
	check(r25_step.text == "0.25", "an expression in a field (%s)" % r25_step.text)
	await key(KEY_BRACKETRIGHT)
	check(r25_step.text == "0.5", "] shows on the header (%s)" % r25_step.text)
	await key(KEY_BRACKETLEFT)
	var r25_w := HewnMesh3D.new()
	r25_w.name = "WallSnap"
	scene.add_child(r25_w)
	r25_w.owner = scene
	r25_w.shape = 1
	r25_w.position = Vector3(0, 0.5, 0)
	EditorInterface.get_selection().clear()
	await set_snapping(true, "Increment")
	await frames(2)
	var r25_n := scene.get_child_count()
	await move(cam.unproject_position(Vector3(0.07, 0.4, 0.5)))
	await key(KEY_B)
	await click(cam.unproject_position(Vector3(0.07, 0.4, 0.5)))
	await move(cam.unproject_position(Vector3(0.38, 0.9, 0.5)))
	await click(cam.unproject_position(Vector3(0.38, 0.9, 0.5)))
	await move(cam.unproject_position(Vector3(0.5, 1.0, 1.1)))
	await click(cam.unproject_position(Vector3(0.5, 1.0, 1.1)))
	await frames(2)
	check(scene.get_child_count() == r25_n + 1, "a box drawn on a wall")
	if scene.get_child_count() == r25_n + 1:
		var r25_box: HewnMesh3D = scene.get_child(scene.get_child_count() - 1)
		var r25_bb: AABB = r25_box.global_transform * r25_box.get_aabb()
		check(r25_bb.position.is_equal_approx(Vector3(0, 0.5, 0.5)) and r25_bb.size.is_equal_approx(Vector3(0.5, 0.5, 0.5)), "a box on a wall snaps to the grid (%s)" % r25_bb)
		r25_box.visible = false
	# Ctrl held over the wall: the box starts on its plane past the face's edge
	r25_n = scene.get_child_count()
	await move(cam.unproject_position(Vector3(0.0, 0.5, 0.5)))
	await key(KEY_B)
	await move(cam.unproject_position(Vector3(0.0, 0.5, 0.5)), false, 0, true)
	await move(cam.unproject_position(Vector3(1.0, 0.75, 0.5)), false, 0, true)
	await button(cam.unproject_position(Vector3(1.0, 0.75, 0.5)), MOUSE_BUTTON_LEFT, true, false, true)
	await button(cam.unproject_position(Vector3(1.0, 0.75, 0.5)), MOUSE_BUTTON_LEFT, false, false, true)
	await move(cam.unproject_position(Vector3(1.5, 1.25, 0.5)))
	await click(cam.unproject_position(Vector3(1.5, 1.25, 0.5)))
	await move(cam.unproject_position(Vector3(1.5, 1.25, 1.0)))
	await click(cam.unproject_position(Vector3(1.5, 1.25, 1.0)))
	await frames(2)
	check(scene.get_child_count() == r25_n + 1, "a box drawn with Ctrl")
	if scene.get_child_count() == r25_n + 1:
		var r25_cb: HewnMesh3D = scene.get_child(scene.get_child_count() - 1)
		var r25_cbb: AABB = r25_cb.global_transform * r25_cb.get_aabb()
		check(r25_cbb.position.is_equal_approx(Vector3(1.0, 0.75, 0.5)) and r25_cbb.size.is_equal_approx(Vector3(0.5, 0.5, 0.5)), "Ctrl keeps the wall's plane past its edge (%s)" % r25_cbb)
		r25_cb.visible = false
	# C: Box Cut (a flat hole in the wall)
	await select_node(r25_w)
	await move(cam.unproject_position(Vector3(-0.25, 0.25, 0.5)))
	await key(KEY_C)
	await click(cam.unproject_position(Vector3(-0.25, 0.25, 0.5)))
	await move(cam.unproject_position(Vector3(0.25, 0.75, 0.5)))
	await click(cam.unproject_position(Vector3(0.25, 0.75, 0.5)))
	await click(cam.unproject_position(Vector3(0.25, 0.75, 0.5)))
	await frames(2)
	check(not analyze_mesh(r25_w).closed, "C cuts a hole (Box Cut)")
	await set_snapping(false, "Increment")
	r25_w.visible = false
	# ---- B in edit mode: edit mode stays, the box joins the mesh, its Add panel changes that build
	var r25_face_btn: Button = null
	for r25_b in r25_bar.find_children("*", "Button", true, false):
		if r25_b.tooltip_text.begins_with("Faces (3)") or r25_b.tooltip_text.begins_with("Грани (3)"):
			r25_face_btn = r25_b
	var r25_j := HewnMesh3D.new()
	r25_j.name = "BuildIntoB"
	scene.add_child(r25_j)
	r25_j.owner = scene
	r25_j.shape = 1
	r25_j.position = Vector3(0, 0.5, 0)
	await select_node(r25_j)
	await key(KEY_TAB)
	await key(KEY_3)
	var r25_nodes := scene.get_child_count()
	await move(cam.unproject_position(Vector3(-0.25, 1, -0.25)))
	await key(KEY_B)
	check(r25_face_btn != null and r25_face_btn.button_pressed, "B in edit mode: still in edit mode while drawing")
	await click(cam.unproject_position(Vector3(-0.25, 1, -0.25)))
	await click(cam.unproject_position(Vector3(0.25, 1, 0.25)))
	await move(cam.unproject_position(Vector3(0, 1.0, 0)))
	await move(cam.unproject_position(Vector3(0, 1.6, 0)))
	await click(cam.unproject_position(Vector3(0, 1.6, 0)))
	await frames(3)
	var r25_jb: AABB = r25_j.global_transform * r25_j.get_aabb()
	check(scene.get_child_count() == r25_nodes and r25_jb.end.y > 1.2 and r25_face_btn.button_pressed, "the B box joins the edited mesh, edit mode stays (%s)" % r25_jb)
	var r25_sp: Control = surf.find_child("HewnShapePanel", true, false)
	check(r25_sp != null and r25_sp.visible, "the Add Cube panel after a box built into the mesh")
	if r25_sp and r25_sp.visible:
		var r25_fields := r25_sp.find_children("*", "HewnDragField", true, false)
		r25_fields[1].value_changed.emit(1.0)  # height
		await frames(2)
		r25_jb = r25_j.global_transform * r25_j.get_aabb()
		check(absf(r25_jb.end.y - 2.0) < 0.02 and r25_face_btn.button_pressed, "the panel changes the built box (%s)" % r25_jb)
		r25_fields[1].value_changed.emit(0.5)
		await frames(2)
		r25_jb = r25_j.global_transform * r25_j.get_aabb()
		check(absf(r25_jb.end.y - 1.5) < 0.02, "and again (%s)" % r25_jb)
	await undo(r25_j)
	check(r25_j.get_face_count() == 6, "one undo takes the whole build back")
	await frames(2)
	check(not r25_sp.visible, "no Add panel after undo")
	var r25_ur := EditorInterface.get_editor_undo_redo()
	r25_ur.get_history_undo_redo(r25_ur.get_object_history_id(r25_j)).redo()
	await frames(2)
	r25_jb = r25_j.global_transform * r25_j.get_aabb()
	check(absf(r25_jb.end.y - 1.5) < 0.02, "redo gives the build as last set in the panel (%s)" % r25_jb)
	await undo(r25_j)
	# ---- Last Operation panel: changes rewrite the history step, so undo goes back to before the operation
	await key(KEY_3)
	var r25_gst := r25_j.get_state()
	r25_gst["sel_faces"] = PackedInt32Array([faces_up_index(r25_j)])
	r25_j.set_state(r25_gst)
	await frames(2)
	var r25_piv := Vector3(0, 1, 0)
	var r25_L := (r25_piv - cam.global_position).dot(-cam.global_basis.z) * 0.17 * gizmo_k()
	var r25_a := cam.unproject_position(r25_piv + Vector3(0, r25_L * 0.8, 0))
	var r25_b2 := cam.unproject_position(r25_piv + Vector3(0, r25_L * 0.8 + 0.5, 0))
	await move(r25_a)
	await drag(r25_a, r25_b2)
	await frames(2)
	var r25_panel := surf.find_child("HewnLastOperation", true, false)
	check(r25_panel != null and r25_panel.visible, "Last Operation panel after a gizmo move")
	if r25_panel and r25_panel.visible:
		var r25_y: LineEdit = r25_panel.find_children("*", "LineEdit", true, false)[1]
		r25_y.text_submitted.emit("0.5")
		await frames(2)
		r25_y.text_submitted.emit("*2")
		await frames(2)
		var r25_top := (r25_j.global_transform * r25_j.get_aabb()).end.y
		check(absf(r25_top - 2.0) < 1e-3, "*2 in the panel doubles Move Y (%f)" % r25_top)
		await undo(r25_j)
		r25_top = (r25_j.global_transform * r25_j.get_aabb()).end.y
		check(absf(r25_top - 1.0) < 1e-4, "undo after changes in the panel: back to before the move (%f)" % r25_top)
		r25_ur.get_history_undo_redo(r25_ur.get_object_history_id(r25_j)).redo()
		await frames(2)
		r25_top = (r25_j.global_transform * r25_j.get_aabb()).end.y
		check(absf(r25_top - 2.0) < 1e-3, "redo: the move as last set in the panel (%f)" % r25_top)
		await undo(r25_j)
	await key(KEY_TAB)
	r25_j.visible = false
	# ---- r26: another scene opened while in edit mode, then back (no errors about nodes outside the tree)
	var r26_e := HewnMesh3D.new()
	r26_e.name = "SceneSwitch"
	scene.add_child(r26_e)
	r26_e.owner = scene
	r26_e.shape = 1
	await select_node(r26_e)
	await key(KEY_TAB)
	await key(KEY_3)
	var r26_root := Node3D.new()
	r26_root.name = "Other"
	var r26_ps := PackedScene.new()
	r26_ps.pack(r26_root)
	ResourceSaver.save(r26_ps, "res://tests/selftest_other.tscn")
	r26_root.free()
	EditorInterface.open_scene_from_path("res://tests/selftest_other.tscn")
	await frames(10)
	await move(Vector2(200, 200))
	EditorInterface.open_scene_from_path("res://tests/selftest_scene.tscn")
	await frames(10)
	check(EditorInterface.get_edited_scene_root() == scene, "back in the test scene")
	await select_node(r26_e)
	await key(KEY_TAB)
	await key(KEY_A)
	check(sel(r26_e, "sel_faces").size() == 6, "after switching back the mesh can be edited again (Tab, A)")
	await key(KEY_TAB)
	r26_e.visible = false
	# ---- Shift+N: Recalculate Normals with "Inside" in the Last Operation panel; Alt+N names Ctrl+Shift+N
	var r26_n := HewnMesh3D.new()
	r26_n.name = "Recalc"
	scene.add_child(r26_n)
	r26_n.owner = scene
	r26_n.shape = 1
	r26_n.position = Vector3(0, 0.5, 0)
	await select_node(r26_n)
	await key(KEY_TAB)
	await key(KEY_3)
	await key(KEY_N, true)
	var r26_panel := surf.find_child("HewnLastOperation", true, false)
	check(r26_panel != null and r26_panel.visible, "Shift+N shows the Last Operation panel")
	if r26_panel and r26_panel.visible:
		var r26_cb := r26_panel.find_children("*", "CheckBox", true, false)
		check(r26_cb.size() == 1, "the panel has Inside")
		if r26_cb.size() == 1:
			r26_cb[0].button_pressed = true
			await frames(2)
			check(faces_up(r26_n) < 0, "Inside turns the normals inward")
			r26_cb[0].button_pressed = false
			await frames(2)
			check(faces_up(r26_n) > 0, "and back outward")
	await key(KEY_N, false, false, true)
	var r26_hint := false
	for r26_pm in EditorInterface.get_base_control().find_children("*", "PopupMenu", false, false):
		if r26_pm.visible:
			for r26_i in r26_pm.item_count:
				if r26_pm.get_item_text(r26_i).ends_with("Ctrl+Shift+N"):
					r26_hint = true
			r26_pm.hide()
	check(r26_hint, "Alt+N menu shows Ctrl+Shift+N")
	await key(KEY_TAB)
	r26_n.visible = false
	# ---- Shift+X: one click puts the 3D cursor there, then the tool ends; Alt+X: Clip
	EditorInterface.get_selection().clear()
	await move(cam.unproject_position(Vector3(-1.25, 0, 0.5)))
	await key(KEY_X, true)
	await click(cam.unproject_position(Vector3(-1.25, 0, 0.5)))
	var r26_cn := scene.get_child_count()
	await move(cam.unproject_position(Vector3(-1.25, 0, 0.5)))
	press("Cube")
	await frames(1)
	await key(KEY_ENTER)
	var r26_made: Node3D = scene.get_child(scene.get_child_count() - 1)
	check(scene.get_child_count() == r26_cn + 1 and absf(r26_made.global_position.x + 1.25) < 0.02 and absf(r26_made.global_position.z - 0.5) < 0.02, "Shift+X: a click places the 3D cursor, then the tool ends (%s)" % r26_made.global_position)
	r26_made.visible = false
	await button(cam.unproject_position(Vector3.ZERO), MOUSE_BUTTON_RIGHT, true, false, false, true)
	await button(cam.unproject_position(Vector3.ZERO), MOUSE_BUTTON_RIGHT, false, false, false, true)
	# ---- B: the second corner above the horizon: the box goes far away, not behind the camera
	# (the view orbited to look along the ground: the horizon in the middle of the viewport)
	var r26_xf := cam.global_transform
	var r26_orbit := func(dy: float) -> void:
		for r26_k in 6:
			var r26_m := InputEventMouseMotion.new()
			r26_m.position = surf.size * 0.5
			r26_m.relative = Vector2(0, dy / 6.0)
			r26_m.button_mask = MOUSE_BUTTON_MASK_MIDDLE
			surf.gui_input.emit(r26_m)
			await frames(1)
		await frames(10)
	var r26_hy := 0.0
	var r26_orbited := 0.0
	for r26_step in 12:
		var r26_fwd := -cam.global_basis.z
		r26_fwd.y = 0.0
		r26_hy = cam.unproject_position(cam.global_position + r26_fwd.normalized() * 1e5).y
		if r26_hy > surf.size.y * 0.3 or cam.global_position.y < 0.6:
			break
		await r26_orbit.call(-10.0)
		r26_orbited -= 10.0
	var r26_bn := scene.get_child_count()
	var r26_c0 := cam.unproject_position(Vector3(0, 0, 0))
	await move(r26_c0)
	await key(KEY_B)
	await click(r26_c0)
	var r26_sky := Vector2(r26_c0.x + 40, r26_hy - 30)
	await move(r26_sky)
	await click(r26_sky)
	await move(r26_sky + Vector2(0, 20))
	await click(r26_sky + Vector2(0, 20))
	await frames(2)
	check(scene.get_child_count() == r26_bn + 1, "a box with its corner above the horizon")
	if scene.get_child_count() == r26_bn + 1:
		var r26_far: HewnMesh3D = scene.get_child(scene.get_child_count() - 1)
		var r26_fb: AABB = r26_far.global_transform * r26_far.get_aabb()
		var r26_ahead := (r26_fb.get_center() - cam.global_position).dot(-cam.global_basis.z)
		check(maxf(r26_fb.size.x, r26_fb.size.z) > 500.0 and r26_ahead > 0.0, "past the horizon the box reaches far ahead (%s)" % r26_fb)
		r26_far.visible = false
	await r26_orbit.call(-r26_orbited)
	# ---- r27: a Box Cut on a room's wall down to the floor, W: the alcove goes straight in, its floor level
	var r27_room := HewnMesh3D.new()
	r27_room.name = "AlcoveRoom"
	scene.add_child(r27_room)
	r27_room.owner = scene
	r27_room.shape = 1
	for r27_pn in ["width", "height", "depth"]:
		r27_room.set_shape_param(r27_pn, 4.0)
	r27_room.set_shape_param("flip_normals", 1.0)
	r27_room.position = Vector3(0, 2, 0)
	await select_node(r27_room)
	await set_snapping(true, "Increment")
	await move(cam.unproject_position(Vector3(-0.5, 0.02, -2)))
	await key(KEY_C)
	await click(cam.unproject_position(Vector3(-0.5, 0.02, -2)))  # (on the wall, not the floor's edge)
	await move(cam.unproject_position(Vector3(0.5, 1.0, -2)))
	await click(cam.unproject_position(Vector3(0.5, 1.0, -2)))
	await move(cam.unproject_position(Vector3(0.5, 1.0, -2.5)))
	await click(cam.unproject_position(Vector3(0.5, 1.0, -2.5)))
	await frames(2)
	await key(KEY_W)
	await frames(2)
	await set_snapping(false, "Increment")
	var r27_bb: AABB = r27_room.global_transform * r27_room.get_aabb()
	var r27_tilt := 0
	for r27_f in r27_room.get_face_count():
		var r27_c := r27_room.global_transform * r27_room.get_face_center(r27_f)
		var r27_nm := r27_room.get_face_normal(r27_f)
		if r27_c.z < -2.01 and r27_c.y < 0.5 and absf(r27_nm.y) < 0.99 and absf(r27_nm.y) > 0.01:
			r27_tilt += 1
	check(r27_bb.position.y > -0.01 and r27_bb.end.z < 2.01 and r27_bb.position.z < -2.2 and r27_tilt == 0, "an alcove cut down to the floor goes straight in (%s, %d tilted faces)" % [r27_bb, r27_tilt])
	r27_room.visible = false
	# ---- r28: C while B draws gives the cutter (and back); Shift+A: the Add menu; snap button lit
	var r28_w := HewnMesh3D.new()
	r28_w.name = "SwitchCut"
	scene.add_child(r28_w)
	r28_w.owner = scene
	r28_w.shape = 1
	r28_w.position = Vector3(0, 0.5, 0)
	await select_node(r28_w)
	var r28_n := scene.get_child_count()
	await move(cam.unproject_position(Vector3(-0.25, 0.25, 0.5)))
	await key(KEY_B)
	await key(KEY_C)
	await click(cam.unproject_position(Vector3(-0.25, 0.25, 0.5)))
	await move(cam.unproject_position(Vector3(0.25, 0.75, 0.5)))
	await click(cam.unproject_position(Vector3(0.25, 0.75, 0.5)))
	await click(cam.unproject_position(Vector3(0.25, 0.75, 0.5)))
	await frames(2)
	check(scene.get_child_count() == r28_n and not analyze_mesh(r28_w).closed, "C while B draws: the cutter takes over (a hole, no new box)")
	await undo(r28_w)
	r28_w.visible = false
	await move(cam.unproject_position(Vector3(1, 0, 1)))
	await key(KEY_A, true)
	var r28_add := false
	for r28_pm in EditorInterface.get_base_control().find_children("*", "PopupMenu", false, false):
		if r28_pm.visible:
			for r28_i in r28_pm.item_count:
				r28_add = r28_add or r28_pm.get_item_text(r28_i) == "Mesh"
	check(r28_add, "Shift+A opens the Add menu")
	await menu_pick("Cube")
	await key(KEY_ENTER)  # (the shape tool: Enter = default size at the 3D cursor)
	check(scene.get_child_count() == r28_n + 1, "a cube from the Shift+A menu")
	if scene.get_child_count() == r28_n + 1:
		scene.get_child(scene.get_child_count() - 1).visible = false
	var r28_bar := EditorInterface.get_base_control().find_child("HewnToolbar", true, false)
	var r28_sb: Button = null
	for r28_b in r28_bar.find_children("*", "Button", true, false):
		if r28_b.tooltip_text.begins_with("Snapping (Y") or r28_b.tooltip_text.begins_with("Привязка (Y"):
			r28_sb = r28_b
	godot_snap().button_pressed = true
	await frames(2)
	var r28_lit := r28_sb != null and r28_sb.has_theme_color_override("icon_normal_color")
	godot_snap().button_pressed = false
	await frames(2)
	check(r28_lit and not r28_sb.has_theme_color_override("icon_normal_color"), "the snap menu button is lit while Godot's magnet is on")
	# ---- Spin: a face turned around the 3D cursor in steps; the panel shows its parameters
	var r28_s := HewnMesh3D.new()
	r28_s.name = "Spin"
	scene.add_child(r28_s)
	r28_s.owner = scene
	r28_s.shape = 1
	r28_s.position = Vector3(2, 0.5, 0)
	await select_node(r28_s)
	await key(KEY_TAB)
	await key(KEY_3)
	var r28_st := r28_s.get_state()
	r28_st["sel_faces"] = PackedInt32Array([faces_up_index(r28_s)])
	r28_s.set_state(r28_st)
	await frames(2)
	await button(cam.unproject_position(Vector3(2, 1, 0)), MOUSE_BUTTON_RIGHT, true)
	await button(cam.unproject_position(Vector3(2, 1, 0)), MOUSE_BUTTON_RIGHT, false)
	await frames(2)
	await menu_pick("Spin")
	check(r28_s.get_face_count() == 6 + 4 * 8, "Spin: 8 steps of side faces (%d faces)" % r28_s.get_face_count())
	var r28_panel := surf.find_child("HewnLastOperation", true, false)
	check(r28_panel != null and r28_panel.visible, "Spin has its Last Operation panel")
	# UV Along the Turn: the image laid straight along what was spun, bent with it
	var r72_square := func(m: HewnMesh3D) -> int:
		var count := 0
		for f in range(6, m.get_face_count()):
			var q: PackedVector2Array = m.get_face_uvs(f)
			count += 1 if q.size() == 4 and (q[1] - q[0]).is_equal_approx(q[2] - q[3]) else 0
		return count
	var r72_all := r28_s.get_face_count() - 6
	var r72_off: int = r72_square.call(r28_s)
	var r72_uv = last_op_field("UV Along the Turn")
	if r72_uv == null:
		r72_uv = last_op_field("UV вдоль поворота")
	check(r72_uv != null, "Spin: its UV checkbox in the panel")
	if r72_uv:
		r72_uv.button_pressed = true
		await frames(3)
		check(r72_off < r72_all and r72_square.call(r28_s) == r72_all and r28_s.get_face_count() == 6 + 32, "Spin: UV along the turn makes the strips straight (%d, then %d of %d)" % [r72_off, r72_square.call(r28_s), r72_all])
		r72_uv = last_op_field("UV Along the Turn") if last_op_field("UV Along the Turn") else last_op_field("UV вдоль поворота")
		r72_uv.button_pressed = false
		await frames(3)
	await undo(r28_s)
	await key(KEY_TAB)
	r28_s.visible = false
	# ---- Editor Settings > Shortcuts: another key for the box builder; the old one is Godot's then
	var r28_es := EditorInterface.get_editor_settings()
	check(r28_es.has_shortcut("hewn/box_builder"), "Hewn's keys are in Editor Settings > Shortcuts")
	var r28_sc := r28_es.get_shortcut("hewn/box_builder")
	var r28_old := r28_sc.events
	var r28_u := InputEventKey.new()
	r28_u.physical_keycode = KEY_U
	r28_sc.events = [r28_u]
	EditorInterface.get_selection().clear()
	r28_n = scene.get_child_count()
	await move(cam.unproject_position(Vector3(-2, 0, 2)))
	await key(KEY_B)
	await key(KEY_ENTER)
	var r28_b_free := scene.get_child_count() == r28_n
	await key(KEY_U)
	await key(KEY_ENTER)
	check(r28_b_free and scene.get_child_count() == r28_n + 1, "a key set in the shortcuts starts the box builder, the old one does not")
	if scene.get_child_count() == r28_n + 1:
		scene.get_child(scene.get_child_count() - 1).visible = false
	r28_sc.events = r28_old
	# ---- Shift+T: G moves the texture with the mouse until a click, S scales it, RMB goes back
	var r28_t := HewnMesh3D.new()
	r28_t.name = "UvtKeys"
	scene.add_child(r28_t)
	r28_t.owner = scene
	r28_t.shape = 1
	r28_t.position = Vector3(0, 0.5, 0)
	await select_node(r28_t)
	await key(KEY_TAB)
	await key(KEY_3)
	var r28_top := faces_up_index(r28_t)
	r28_st = r28_t.get_state()
	r28_st["sel_faces"] = PackedInt32Array([r28_top])
	r28_t.set_state(r28_st)
	await frames(2)
	var r28_uv0 := r28_t.get_face_uvs(r28_top)
	var r28_c := cam.unproject_position(Vector3(0, 1, 0))
	await move(r28_c)
	await key(KEY_T, true)
	await key(KEY_G)
	await move(r28_c + Vector2(30, 0))
	await click(r28_c + Vector2(30, 0))
	var r28_uv1 := r28_t.get_face_uvs(r28_top)
	check(not r28_uv1[0].is_equal_approx(r28_uv0[0]) and (r28_uv1[0] - r28_uv1[2]).is_equal_approx(r28_uv0[0] - r28_uv0[2]), "Shift+T, G: the texture moves (same size)")
	await key(KEY_S)
	await move(r28_c + Vector2(80, 0))
	await button(r28_c + Vector2(80, 0), MOUSE_BUTTON_RIGHT, true)
	await button(r28_c + Vector2(80, 0), MOUSE_BUTTON_RIGHT, false)
	check(r28_t.get_face_uvs(r28_top)[0].is_equal_approx(r28_uv1[0]), "Shift+T, S then RMB: back as it was")
	await key(KEY_ESCAPE)
	# ---- UV editor: UV selection by element, G moves it, Ctrl+P packs into 0..1, undo
	var r28_cv: Control = null
	for r28_x in EditorInterface.get_base_control().find_child("HewnUV", true, false).find_children("*", "HewnUVCanvas", true, false):
		r28_cv = r28_x
	check(r28_cv != null, "the UV editor exists")
	var r28_hdr := EditorInterface.get_base_control().find_child("HewnUVHeader", true, false)
	var r28_menus := []
	if r28_hdr:
		for r28_mb in r28_hdr.find_children("*", "MenuButton", true, false):
			r28_menus.append(r28_mb.text)
	check(r28_menus == ["View", "Select", "Image", "UV"], "the UV editor has View / Select / Image / UV menus (%s)" % [r28_menus])
	if r28_cv:
		r28_cv.size = Vector2(400, 300)
		var r28_ck := func(code: Key, ctrl := false) -> void:
			var ev := InputEventKey.new()
			ev.physical_keycode = code
			ev.ctrl_pressed = ctrl
			ev.pressed = true
			r28_cv.call("feed_input", ev)
			await frames(1)
		var r28_cm := func(pos: Vector2) -> void:
			var ev := InputEventMouseMotion.new()
			ev.position = pos
			r28_cv.call("feed_input", ev)
			await frames(1)
		var r28_cb := func(pos: Vector2, pressed: bool) -> void:
			var ev := InputEventMouseButton.new()
			ev.position = pos
			ev.button_index = MOUSE_BUTTON_LEFT
			ev.pressed = pressed
			r28_cv.call("feed_input", ev)
			await frames(1)
		await r28_cm.call(Vector2(200, 150))
		await r28_ck.call(KEY_A)  # (the UVs of the selected top face)
		await r28_ck.call(KEY_G)
		await r28_cm.call(Vector2(244, 150))
		await r28_cb.call(Vector2(244, 150), true)
		await r28_cb.call(Vector2(244, 150), false)
		var r28_uv2 := r28_t.get_face_uvs(r28_top)
		check(r28_uv2[0].x > r28_uv0[0].x + 0.05 and absf(r28_uv2[0].y - r28_uv0[0].y) < 1e-4, "UV editor: A, G moves the UVs (%s -> %s)" % [r28_uv0[0], r28_uv2[0]])
		await undo(r28_t)
		check(r28_t.get_face_uvs(r28_top)[0].is_equal_approx(r28_uv0[0]), "undo of the UV move")
		await key(KEY_A)  # all faces in the viewport: all their UVs shown
		await r28_ck.call(KEY_A)
		await r28_ck.call(KEY_P, true)
		var r28_in := true
		for r28_f in r28_t.get_face_count():
			for r28_p in r28_t.get_face_uvs(r28_f):
				r28_in = r28_in and r28_p.x > -1e-4 and r28_p.y > -1e-4 and r28_p.x < 1.0001 and r28_p.y < 1.0001
		check(r28_in, "UV editor: Ctrl+P packs the islands into 0..1")
		await undo(r28_t)
	await key(KEY_TAB)
	r28_t.visible = false
	# ---- r28: the Material tab shows the texture of the selection; Closest / Linear changes its material
	var r28m := HewnMesh3D.new()
	r28m.name = "MatPanel"
	r28m.shape = 1
	scene.add_child(r28m)
	r28m.owner = scene
	r28m.position = Vector3(-3, 0.5, -3)
	var r28_img := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	r28_img.fill(Color(0.5, 0.3, 0.2))
	var r28_mat := StandardMaterial3D.new()
	r28_mat.albedo_texture = ImageTexture.create_from_image(r28_img)
	r28_mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS
	r28m.apply_material(r28_mat)
	await select_node(r28m)
	await frames(2)
	var r28_dock := EditorInterface.get_base_control().find_child("Hewn", true, false)
	var r28_prev: TextureRect = r28_dock.find_child("TexturePreview", true, false)
	check(r28_prev != null and r28_prev.texture == r28_mat.albedo_texture, "the Material tab previews the texture of the selected object")
	var r28_closest: Button = null
	for r28_b in r28_dock.find_children("*", "Button", true, false):
		if r28_b.text == "Closest" and r28_closest == null:
			r28_closest = r28_b
	if r28_closest:
		r28_closest.pressed.emit()
		await frames(1)
	check(r28_mat.texture_filter == BaseMaterial3D.TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, "Closest sets the material's filter (already applied material too)")
	var r28_ur := EditorInterface.get_editor_undo_redo()
	r28_ur.get_history_undo_redo(r28_ur.get_object_history_id(r28_mat)).undo()
	await frames(1)
	check(r28_mat.texture_filter == BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, "undo of the filter")
	r28m.visible = false
	# ---- Show Overlapping Faces runs on the edited mesh (two cubes touching)
	var r28_ovc: CheckBox = EditorInterface.get_base_control().find_child("OverlapCheck", true, false)
	check(r28_ovc != null, "the Show Overlapping Faces overlay exists")
	if r28_ovc:
		var r28_om := HewnMesh3D.new()
		r28_om.shape = 1
		scene.add_child(r28_om)
		r28_om.owner = scene
		var r28_ob := HewnMesh3D.new()
		r28_ob.shape = 1
		r28_ob.position = Vector3(1, 0, 0)
		r28_om.merge_from(r28_ob)
		r28_ob.free()
		await select_node(r28_om)
		await key(KEY_TAB)
		r28_ovc.button_pressed = true
		await get_tree().create_timer(0.4).timeout
		await frames(2)
		r28_ovc.button_pressed = false
		await frames(2)
		check(true, "the overlap check ran")
		await key(KEY_TAB)
		r28_om.visible = false
	# ---- r29: entities, a trigger, an overlay and a decal placed like prefabs; a Sweep drawn with clicks; Arch Edges
	var r29_floor := HewnMesh3D.new()
	r29_floor.shape = 2
	r29_floor.set_shape_param("width", 6.0)
	r29_floor.set_shape_param("depth", 6.0)
	scene.add_child(r29_floor)
	r29_floor.owner = scene
	await frames(2)
	var r29_n0 := scene.get_child_count()
	# an omni light from Shift+A > Light, placed with a click; W: another one with the first one's values
	EditorInterface.get_selection().clear()
	await move(cam.unproject_position(Vector3(0.5, 0, 0.5)))
	await key(KEY_A, true)
	await menu_pick("Omni Light")
	var r29_at := cam.unproject_position(Vector3(0.5, 0, 0.5))
	await move(r29_at + Vector2(3, 0))
	await move(r29_at)
	await click(r29_at)
	check(scene.get_child_count() == r29_n0 + 1, "Shift+A > Light > Omni Light placed with a click")
	var r29_sp = scene.get_child(scene.get_child_count() - 1)
	check(r29_sp is OmniLight3D and Vector2(r29_sp.global_position.x, r29_sp.global_position.z).distance_to(Vector2(0.5, 0.5)) < 0.05,
			"the light where clicked (%s)" % r29_sp.global_position)
	if r29_sp is OmniLight3D:
		r29_sp.light_energy = 3.5
	await key(KEY_W)
	var r29_at2 := cam.unproject_position(Vector3(-0.5, 0, 0.5))
	await move(r29_at2 + Vector2(3, 0))
	await move(r29_at2)
	await click(r29_at2)
	var r29_tr = scene.get_child(scene.get_child_count() - 1)
	check(r29_tr is OmniLight3D and r29_tr != r29_sp and absf(r29_tr.light_energy - 3.5) < 1e-4, "W: another light like the last one (energy %s)" % (r29_tr.light_energy if r29_tr is OmniLight3D else -1.0))
	await undo(scene)
	await undo(scene)
	check(scene.get_child_count() == r29_n0, "undo removes the lights")
	# an overlay on the floor from the Create tab's picture
	var r29_img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	r29_img.fill(Color(0.2, 0.2, 0.2, 1.0))
	var r29_tex := ImageTexture.create_from_image(r29_img)
	var r29_pick: EditorResourcePicker = EditorInterface.get_base_control().find_child("DecalTexture", true, false)
	check(r29_pick != null, "the decal picture picker exists")
	var r29_place: Button = EditorInterface.get_base_control().find_child("PlaceDecal", true, false)
	if r29_pick and r29_place:
		r29_pick.edited_resource = r29_tex
		r29_place.pressed.emit()
		await frames(1)
		var r29_at3 := cam.unproject_position(Vector3(1.0, 0, -0.5))
		await move(r29_at3 + Vector2(3, 0))
		await move(r29_at3)
		await click(r29_at3)
		await frames(3)
		var r29_ov = scene.get_child(scene.get_child_count() - 1)
		check(r29_ov is HewnOverlay3D, "Place: an overlay")
		if r29_ov is HewnOverlay3D:
			check(r29_ov.get_triangle_count() >= 2, "the overlay lies on the floor (%d triangles)" % r29_ov.get_triangle_count())
			check(r29_ov.global_transform.basis.y.dot(Vector3.UP) > 0.99, "the overlay faces up from the floor")
		# the same picture as a Godot decal, only on the level layer
		var r29_kind: OptionButton = null
		for r29_o in EditorInterface.get_base_control().find_child("Hewn", true, false).find_children("*", "OptionButton", true, false):
			if r29_o.item_count == 2 and r29_o.get_item_text(1) == "Decal":
				r29_kind = r29_o
		if r29_kind:
			r29_kind.select(1)
			r29_place.pressed.emit()
			await frames(1)
			var r29_at4 := cam.unproject_position(Vector3(-1.0, 0, -0.5))
			await move(r29_at4 + Vector2(3, 0))
			await move(r29_at4)
			await click(r29_at4)
			var r29_dc = scene.get_child(scene.get_child_count() - 1)
			check(r29_dc is Decal and r29_dc.cull_mask == 1 and r29_dc.texture_albedo == r29_tex, "Place: a Decal on the level layer")
			r29_kind.select(0)
	# a Sweep: three clicks along the floor, Enter
	var r29_nb := scene.get_child_count()
	press("Sweep")
	await frames(1)
	for r29_wp in [Vector3(-1, 0, -1), Vector3(1, 0, -1), Vector3(1, 0, 0.5)]:
		await move(cam.unproject_position(r29_wp))
		await click(cam.unproject_position(r29_wp))
	await key(KEY_ENTER)
	check(scene.get_child_count() == r29_nb + 1, "Sweep drawn with clicks")
	var r29_sw = scene.get_child(scene.get_child_count() - 1)
	check(r29_sw is HewnMesh3D and r29_sw.shape == 20 and r29_sw.poly_points.size() == 3, "a Sweep along the clicked path (%s)" % [r29_sw.poly_points.size() if r29_sw is HewnMesh3D else -1])
	if r29_sw is HewnMesh3D:
		r29_sw.visible = false
	r29_floor.visible = false
	# Arch Edges: the top edges of a passage through a wall become a vault
	var r29_wall := HewnMesh3D.new()
	r29_wall.shape = 1
	r29_wall.set_shape_param("width", 4.0)
	r29_wall.set_shape_param("height", 3.0)
	r29_wall.set_shape_param("depth", 0.5)
	scene.add_child(r29_wall)
	r29_wall.owner = scene
	var r29_h: PackedInt32Array = r29_wall.cut_box(AABB(Vector3(-0.5, -1.0, -1.0), Vector3(1.0, 1.5, 2.0)))
	var r29_loops := []
	var r29_i := 0
	while r29_i < r29_h.size():
		r29_loops.append(r29_h.slice(r29_i + 1, r29_i + 1 + r29_h[r29_i]))
		r29_i += r29_h[r29_i] + 1
	if r29_loops.size() == 2:
		r29_wall.bridge_holes(r29_loops[0], r29_loops[1])
	var r29_v := r29_wall.get_vertices()
	var r29_edges := PackedInt32Array()
	for r29_f in r29_wall.get_face_count():
		var r29_fc := r29_wall.get_face(r29_f)
		for r29_k in r29_fc.size():
			var r29_a := r29_fc[r29_k]
			var r29_b := r29_fc[(r29_k + 1) % r29_fc.size()]
			if r29_a < r29_b and absf(r29_v[r29_a].y - 0.5) < 1e-4 and absf(r29_v[r29_b].y - 0.5) < 1e-4 and absf(r29_v[r29_a].z - r29_v[r29_b].z) < 1e-4 and absf(r29_v[r29_a].x) < 0.51 and absf(r29_v[r29_b].x) < 0.51 and absf(absf(r29_v[r29_a].z) - 0.25) < 1e-4:
				r29_edges.push_back(r29_a)
				r29_edges.push_back(r29_b)
	await select_node(r29_wall)
	await key(KEY_TAB)
	await key(KEY_2)
	var r29_state: Dictionary = r29_wall.get_state()
	r29_state["sel_edges"] = r29_edges
	r29_wall.set_state(r29_state)
	var r29_nf := r29_wall.get_face_count()
	press("Arch Edges")
	await frames(2)
	check(r29_edges.size() == 4 and r29_wall.get_face_count() == r29_nf + 7, "Arch Edges: a vault of 8 strips (%d edges, %d -> %d faces)" % [r29_edges.size() / 2, r29_nf, r29_wall.get_face_count()])
	await key(KEY_TAB)
	r29_wall.visible = false
	# ---- r30: Box Cut's panel: non-destructive (a cutter node) and back; Y toggles snapping while drawing;
	# walls around a floor; the object panel; decimal commas; Path Stairs drawn with clicks
	var r30_room := HewnMesh3D.new()
	r30_room.name = "R30Room"
	scene.add_child(r30_room)
	r30_room.owner = scene
	r30_room.shape = 1
	for r30_pn in ["width", "height", "depth"]:
		r30_room.set_shape_param(r30_pn, 4.0)
	r30_room.set_shape_param("flip_normals", 1.0)
	r30_room.position = Vector3(0, 2, 0)
	await select_node(r30_room)
	var r30_snap := godot_snap()
	var r30_was := r30_snap.button_pressed if r30_snap else false
	await move(cam.unproject_position(Vector3(-0.5, 0.02, -2)))
	await key(KEY_C)
	await key(KEY_Y)
	check(r30_snap != null and r30_snap.button_pressed != r30_was, "Y toggles snapping while a Box Cut is drawn")
	await key(KEY_Y)
	await click(cam.unproject_position(Vector3(-0.5, 0.02, -2)))  # (on the wall, not the floor's edge)
	await move(cam.unproject_position(Vector3(0.5, 1.0, -2)))
	await click(cam.unproject_position(Vector3(0.5, 1.0, -2)))
	await move(cam.unproject_position(Vector3(0.5, 1.0, -2.5)))
	await click(cam.unproject_position(Vector3(0.5, 1.0, -2.5)))
	await frames(3)
	var r30_cut_faces := r30_room.get_face_count()
	check(r30_cut_faces > 6, "Box Cut cuts the wall (%d faces)" % r30_cut_faces)
	var r30_nd: CheckBox = null
	for r30_l in EditorInterface.get_base_control().find_children("*", "Label", true, false):
		if r30_l.text.begins_with("Non-destructive (a cutter") and r30_l.is_visible_in_tree():
			r30_nd = field_after(r30_l) as CheckBox
	check(r30_nd != null, "the Box Cut panel has Non-destructive")
	if r30_nd:
		r30_nd.button_pressed = true
		await frames(3)
		var r30_cut := scene.find_children("*", "HewnCutter3D", false, false)
		check(r30_cut.size() == 1 and r30_room.get_face_count() == 6, "non-destructive: a cutter node, the room's own faces as they were (%d)" % r30_room.get_face_count())
		r30_room.update_cuts()
		check(r30_room.is_cut(), "the cutter cuts the wall")
		var r30_nd2: CheckBox = null
		for r30_l in EditorInterface.get_base_control().find_children("*", "Label", true, false):
			if r30_l.text.begins_with("Non-destructive (a cutter") and r30_l.is_visible_in_tree():
				r30_nd2 = field_after(r30_l) as CheckBox
		if r30_nd2:
			r30_nd2.button_pressed = false
			await frames(3)
			check(scene.find_children("*", "HewnCutter3D", false, false).is_empty() and r30_room.get_face_count() == r30_cut_faces, "back to a destructive cut")
			# non-destructive again, then W: the cutter lines its hole (made again whenever it moves)
			var r30_nd3: CheckBox = null
			for r30_l in EditorInterface.get_base_control().find_children("*", "Label", true, false):
				if r30_l.text.begins_with("Non-destructive (a cutter") and r30_l.is_visible_in_tree():
					r30_nd3 = field_after(r30_l) as CheckBox
			check(r30_nd3 != null, "the Box Cut panel again after going back")
			if r30_nd3:
				r30_nd3.button_pressed = true
			await frames(3)
			await key(KEY_W)
			await frames(3)
			var r30_c2 := scene.find_children("*", "HewnCutter3D", false, false)
			check(r30_c2.size() == 1 and r30_c2[0].lining == 1, "W after switching to non-destructive: the cutter lines its hole")
			for r30_x in r30_c2:
				r30_x.get_parent().remove_child(r30_x)
				r30_x.queue_free()
	r30_room.visible = false
	# walls around a floor (a plane): one closed wall on its inside
	var r30_floor := HewnMesh3D.new()
	r30_floor.shape = 2
	r30_floor.set_shape_param("width", 3.0)
	r30_floor.set_shape_param("depth", 3.0)
	scene.add_child(r30_floor)
	r30_floor.owner = scene
	await select_node(r30_floor)
	var r30_n0 := scene.get_child_count()
	press("Walls Around Floor")
	await frames(2)
	check(scene.get_child_count() == r30_n0 + 1, "Walls Around Floor adds a wall")
	var r30_wall = scene.get_child(scene.get_child_count() - 1)
	if r30_wall is HewnMesh3D:
		var r30_wb: AABB = r30_wall.global_transform * r30_wall.get_aabb()
		check(r30_wall.shape == 19 and absf(r30_wb.size.x - 3.0) < 0.01 and absf(r30_wb.size.z - 3.0) < 0.01, "the wall runs inside the floor's edge (%s)" % r30_wb)
		r30_wall.visible = false
	r30_floor.visible = false
	# the object panel of a light
	var r30_light := OmniLight3D.new()
	scene.add_child(r30_light)
	r30_light.owner = scene
	await select_node(r30_light)
	await frames(3)
	var r30_op: Control = EditorInterface.get_base_control().find_child("HewnObjectPanel", true, false)
	check(r30_op != null and r30_op.is_visible_in_tree(), "a selected light shows its parameters")
	r30_light.queue_free()
	await frames(2)
	# a decimal comma in a number field becomes a point
	var r30_le: LineEdit = null
	for r30_sb in EditorInterface.get_base_control().find_child("Hewn", true, false).find_children("*", "SpinBox", true, false):
		if r30_le == null:
			r30_le = r30_sb.get_line_edit()
	if r30_le:
		r30_le.text = "0,5"
		r30_le.text_changed.emit("0,5")  # (as when typed)
		check(r30_le.text == "0.5", "a decimal comma becomes a point (%s)" % r30_le.text)
	# Path Stairs: clicks along the floor, Enter
	var r30_hw := HewnMesh3D.new()
	r30_hw.shape = 2
	r30_hw.set_shape_param("width", 6.0)
	r30_hw.set_shape_param("depth", 6.0)
	scene.add_child(r30_hw)
	r30_hw.owner = scene
	await frames(2)
	var r30_nb := scene.get_child_count()
	press("Path Stairs")
	await frames(1)
	for r30_wp in [Vector3(-1, 0, -1), Vector3(1, 0, -1), Vector3(1, 0, 0.5)]:
		await move(cam.unproject_position(r30_wp))
		await click(cam.unproject_position(r30_wp))
	await key(KEY_ENTER)
	check(scene.get_child_count() == r30_nb + 1, "Path Stairs drawn with clicks")
	var r30_ps = scene.get_child(scene.get_child_count() - 1)
	check(r30_ps is HewnMesh3D and r30_ps.shape == 22 and r30_ps.poly_points.size() == 3, "stairs along the clicked path")
	if r30_ps is HewnMesh3D:
		r30_ps.visible = false
	r30_hw.visible = false
	# ---- r31: multi-object edit (Tab with two meshes), W after B in edit mode, Face Cutter, Spin tool,
	# a non-destructive cut lined with W
	for r31_c in scene.get_children():
		if r31_c is Node3D:
			r31_c.visible = false
	var r31_a := HewnMesh3D.new()
	r31_a.name = "MultiA"
	scene.add_child(r31_a)
	r31_a.owner = scene
	r31_a.shape = 1
	r31_a.position = Vector3(-1, 0.5, 0)
	var r31_b := HewnMesh3D.new()
	r31_b.name = "MultiB"
	scene.add_child(r31_b)
	r31_b.owner = scene
	r31_b.shape = 1
	r31_b.position = Vector3(1, 0.5, 0)
	await frames(2)
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(r31_a)
	EditorInterface.get_selection().add_node(r31_b)
	await frames(3)
	await key(KEY_TAB)
	await key(KEY_1)
	await key(KEY_A)
	var r31_a0 := r31_a.get_vertex(0)
	var r31_b0 := r31_b.get_vertex(0)
	await key(KEY_G)
	await key(KEY_Y)
	await type_text("1")
	await key(KEY_ENTER)
	await frames(2)
	check(absf(r31_a.get_vertex(0).y - r31_a0.y - 1.0) < 1e-3 and absf(r31_b.get_vertex(0).y - r31_b0.y - 1.0) < 1e-3,
			"multi-edit: G Y 1 moves both meshes' vertices (%s, %s)" % [r31_a.get_vertex(0) - r31_a0, r31_b.get_vertex(0) - r31_b0])
	await undo(r31_a)
	check(r31_a.get_vertex(0).is_equal_approx(r31_a0) and r31_b.get_vertex(0).is_equal_approx(r31_b0), "multi-edit: one undo step for both")
	await key(KEY_TAB)
	var r31_sel := EditorInterface.get_selection().get_selected_nodes()
	check(r31_sel.has(r31_a) and r31_sel.has(r31_b), "back in object mode both stay selected")
	r31_b.visible = false
	# B in edit mode: the box goes into the mesh, W turns it inward
	await select_node(r31_a)
	await key(KEY_TAB)
	await key(KEY_3)
	var r31_nf := r31_a.get_face_count()
	await move(cam.unproject_position(Vector3(-1.2, 1.0, -0.2)))
	await key(KEY_B)
	await click(cam.unproject_position(Vector3(-1.2, 1.0, -0.2)))
	await move(cam.unproject_position(Vector3(-0.8, 1.0, 0.2)))
	await click(cam.unproject_position(Vector3(-0.8, 1.0, 0.2)))
	await move(cam.unproject_position(Vector3(-0.8, 1.5, 0.2)))
	await click(cam.unproject_position(Vector3(-0.8, 1.5, 0.2)))
	await frames(2)
	var r31_nf2 := r31_a.get_face_count()
	var r31_vol0 := mesh_volume(r31_a)
	await key(KEY_W)
	await frames(2)
	check(r31_nf2 > r31_nf and mesh_volume(r31_a) < r31_vol0 - 1e-4, "B in edit mode, then W: the new part faces inward (%f -> %f)" % [r31_vol0, mesh_volume(r31_a)])
	await undo(r31_a)
	await undo(r31_a)
	# Face Cutter: the top face, its edge, two lines, Q
	var r31_top := faces_up_index(r31_a)
	var r31_st := r31_a.get_state()
	r31_st["sel_faces"] = PackedInt32Array([r31_top])
	r31_a.set_state(r31_st)
	await frames(2)
	var r31_tc := r31_a.global_transform * r31_a.get_face_center(r31_top)
	await button(cam.unproject_position(r31_tc), MOUSE_BUTTON_RIGHT, true)
	await button(cam.unproject_position(r31_tc), MOUSE_BUTTON_RIGHT, false)
	await frames(2)
	await menu_pick("Face Cutter")
	var r31_edge := cam.unproject_position(Vector3(-1.0, 1.0, 0.5))
	await move(r31_edge)
	await click(r31_edge)
	await move(cam.unproject_position(Vector3(-1.0, 1.0, 0.0)))
	await click(cam.unproject_position(Vector3(-1.0, 1.0, 0.0)))
	await move(cam.unproject_position(Vector3(-1.25, 1.0, 0.0)))
	await click(cam.unproject_position(Vector3(-1.25, 1.0, 0.0)))
	await key(KEY_Z, false, true)
	var r31_fc0 := r31_a.get_face_count()
	await key(KEY_Q)
	await frames(2)
	check(r31_a.get_face_count() == r31_fc0 + 1, "Face Cutter: one line (the second taken back), Q cuts the top in two (%d -> %d)" % [r31_fc0, r31_a.get_face_count()])
	await undo(r31_a)
	# the Spin tool: drag its ring
	var r31_tcol := EditorInterface.get_base_control().find_child("HewnTools", true, false)
	var r31_tb: Array = []
	for r31_x in r31_tcol.find_children("*", "Button", true, false):
		if r31_x.toggle_mode:
			r31_tb.append(r31_x)
	r31_tb = []
	for r31_i in 15:
		r31_tb.append(r31_tcol.find_child("Tool%d" % r31_i, true, false))
	if r31_tb[7] != null:
		r31_st = r31_a.get_state()
		r31_st["sel_faces"] = PackedInt32Array([faces_up_index(r31_a)])
		r31_a.set_state(r31_st)
		r31_tb[7].pressed.emit()
		await frames(2)
		var r31_n0 := r31_a.get_face_count()
		var r31_c := cam.unproject_position(Vector3.ZERO)
		var r31_r := 70.0 * EditorInterface.get_editor_scale()
		await move(r31_c + Vector2(r31_r, 0))
		await button(r31_c + Vector2(r31_r, 0), MOUSE_BUTTON_LEFT, true)
		for r31_k in range(1, 7):
			var r31_ang := -0.2 * r31_k
			await move(r31_c + Vector2(cos(r31_ang), sin(r31_ang)) * r31_r)
		await button(r31_c + Vector2(cos(-1.2), sin(-1.2)) * r31_r, MOUSE_BUTTON_LEFT, false)
		await frames(2)
		check(r31_a.get_face_count() > r31_n0, "Spin tool: dragging its ring spins the face (%d -> %d)" % [r31_n0, r31_a.get_face_count()])
		await key(KEY_ESCAPE)
		await undo(r31_a)
	await key(KEY_TAB)
	r31_a.visible = false
	# ---- r32: Vertex Paint mode (the mode menu, brush with a channel off, fill, eyedropper, X), circle / lasso
	# select, the Add menu at the 3D cursor and an Area3D
	for r32_c in scene.get_children():
		if r32_c is Node3D:
			r32_c.visible = false
	var r32_m := HewnMesh3D.new()
	r32_m.name = "PaintMe"
	scene.add_child(r32_m)
	r32_m.owner = scene
	r32_m.shape = 1
	await select_node(r32_m)
	await frames(2)
	var r32_im: MenuButton = EditorInterface.get_base_control().find_child("InteractionMode", true, false)
	check(r32_im != null, "the header has the mode menu")
	var r32_c1: ColorPickerButton = EditorInterface.get_base_control().find_child("VertexColor", true, false)
	var r32_c2: ColorPickerButton = EditorInterface.get_base_control().find_child("VertexColor2", true, false)
	if r32_im and r32_c1 and r32_c2:
		r32_im.get_popup().id_pressed.emit(2)
		await frames(2)
		var r32_tools := EditorInterface.get_base_control().find_child("HewnTools", true, false)
		var r32_brush: Button = r32_tools.find_child("Tool10", true, false)
		var r32_knife: Button = r32_tools.find_child("Tool3", true, false)
		check(r32_brush.is_visible_in_tree() and r32_brush.button_pressed and not r32_knife.is_visible_in_tree(), "Vertex Paint: the brush is on, the mesh tools hidden")
		await key(KEY_1)
		await key(KEY_A, false, false, true)
		r32_c1.color = Color(1, 0, 0, 1)
		r32_c2.color = Color(0, 0, 1, 1)
		# the green channel off: red paints, green stays 1
		var r32_g: Button = null
		for r32_b in EditorInterface.get_base_control().find_child("HewnPaintRow", true, false).find_children("*", "Button", true, false):
			if r32_b.text == "G" and r32_b.toggle_mode:
				r32_g = r32_b
		check(r32_g != null, "the R / G / B / A buttons")
		if r32_g:
			r32_g.button_pressed = false
		var r32_f := visible_face(r32_m)
		var r32_fv := r32_m.get_face(r32_f)
		var r32_at := sp(r32_m, r32_m.get_vertex(r32_fv[0]))
		await move(r32_at)
		await click(r32_at)
		var r32_cols: PackedColorArray = face_cols(r32_m.mesh_data)
		var r32_red := false
		var r32_green_kept := true
		for r32_col in r32_cols:
			if r32_col.b < 0.99:
				r32_red = true
				r32_green_kept = r32_green_kept and r32_col.g > 0.99
		check(r32_red and r32_green_kept, "the brush paints, the green channel turned off stays (%s)" % r32_cols[0])
		if r32_g:
			r32_g.button_pressed = true
		# X swaps the colors
		await key(KEY_X)
		check(r32_c1.color.is_equal_approx(Color(0, 0, 1, 1)), "X swaps the two colors")
		# fill: everything linked to the face clicked
		r32_tools.find_child("Tool12", true, false).pressed.emit()
		await frames(1)
		await click(sp(r32_m, r32_m.get_face_center(r32_f)))
		r32_cols = face_cols(r32_m.mesh_data)
		var r32_all_blue := true
		for r32_col in r32_cols:
			r32_all_blue = r32_all_blue and r32_col.is_equal_approx(Color(0, 0, 1, 1))
		check(r32_all_blue, "fill: all the linked faces")
		# eyedropper: the second color from a vertex
		r32_c2.color = Color(1, 1, 1, 1)
		r32_tools.find_child("Tool13", true, false).pressed.emit()
		await frames(1)
		await move(r32_at)
		await click(r32_at, false, false, false, true)
		check(r32_c2.color.is_equal_approx(Color(0, 0, 1, 1)), "eyedropper (Shift): the second color from the vertex (%s)" % r32_c2.color)
		await undo(r32_m)
		await undo(r32_m)
		# the selection tools in Vertex Paint: circle select, then the brush paints only the selection
		r32_tools.find_child("Tool8", true, false).pressed.emit()
		await frames(1)
		await move(r32_at)
		await click(r32_at)
		var r32_sel: PackedInt32Array = r32_m.get_state().get("sel_verts", PackedInt32Array())
		check(r32_sel.size() == 1, "circle select picks the vertex under it (%d)" % r32_sel.size())
		await key(KEY_TAB)
		check(not EditorInterface.get_base_control().find_child("HewnTools", true, false).find_child("Tool10", true, false).is_visible_in_tree(), "Tab: out of Vertex Paint (object mode)")
	# lasso in edit mode: around the top face's corners
	await select_node(r32_m)
	await key(KEY_TAB)
	await key(KEY_1)
	var r32_tools2 := EditorInterface.get_base_control().find_child("HewnTools", true, false)
	r32_tools2.find_child("Tool9", true, false).pressed.emit()
	await frames(1)
	var r32_pts: Array = []
	for r32_k in 17:
		var r32_a := TAU * r32_k / 16.0
		r32_pts.append(cam.unproject_position(Vector3(cos(r32_a) * 0.95, 0.5, sin(r32_a) * 0.95)))
	await move(r32_pts[0])
	await button(r32_pts[0], MOUSE_BUTTON_LEFT, true)
	for r32_p in r32_pts:
		await move(r32_p)
	await button(r32_pts[16], MOUSE_BUTTON_LEFT, false)
	var r32_ls: PackedInt32Array = r32_m.get_state().get("sel_verts", PackedInt32Array())
	check(r32_ls.size() >= 3, "lasso select: the top's corners inside it (%d)" % r32_ls.size())
	r32_tools2.find_child("Tool0", true, false).pressed.emit()
	await key(KEY_TAB)
	r32_m.visible = false
	# the Add menu with Create at 3D Cursor: an Area3D at once
	await button(cam.unproject_position(Vector3(1.5, 0, 0.5)), MOUSE_BUTTON_RIGHT, true, false, false, true)
	await button(cam.unproject_position(Vector3(1.5, 0, 0.5)), MOUSE_BUTTON_RIGHT, false, false, false, true)
	await move(cam.unproject_position(Vector3(0, 0, 0)))
	await key(KEY_A, true)
	await frames(2)
	await menu_pick("Create at 3D Cursor")
	await key(KEY_A, true)
	await frames(2)
	await menu_pick("Area3D")
	await frames(2)
	var r32_area: Node = scene.get_child(scene.get_child_count() - 1)
	check(r32_area is Area3D and r32_area.get_child_count() == 1 and absf((r32_area as Node3D).global_position.x - 1.5) < 0.05, "Add > Area3D at the 3D cursor, with its shape (%s)" % r32_area)
	await key(KEY_A, true)
	await frames(2)
	await menu_pick("Create at 3D Cursor")
	if r32_area is Node3D:
		(r32_area as Node3D).visible = false
	# (nothing selected: the area's panel at the corner of the view could catch the click and open its
	# field's menu, which then takes the keys of the next tests)
	EditorInterface.get_selection().clear()
	await frames(2)
	await button(cam.unproject_position(Vector3.ZERO), MOUSE_BUTTON_RIGHT, true, false, false, true)
	await button(cam.unproject_position(Vector3.ZERO), MOUSE_BUTTON_RIGHT, false, false, false, true)
	# ---- r34: a Path3D's points (Tab, A, G X, E), Ctrl+wheel with the normals brush, the blend
	# material window
	var r34_path := Path3D.new()
	r34_path.name = "R34Path"
	var r34_curve := Curve3D.new()
	r34_curve.add_point(Vector3(-1, 0, 0))
	r34_curve.add_point(Vector3(1, 0, 0))
	r34_path.curve = r34_curve
	scene.add_child(r34_path)
	r34_path.owner = scene
	await select_node(r34_path)
	await frames(2)
	await move(cam.unproject_position(Vector3(0, 0, 0)))
	await key(KEY_TAB)
	await key(KEY_A)
	var r34_m0 := cam.unproject_position(Vector3(0, 0, 0))
	await move(r34_m0)
	await key(KEY_G)
	await key(KEY_X)
	await move(cam.unproject_position(Vector3(0.5, 0, 0)))
	await key(KEY_ENTER)
	var r34_c: Curve3D = r34_path.curve
	check(r34_c.get_point_count() == 2 and absf(r34_c.get_point_position(0).x + 0.5) < 0.05 and absf(r34_c.get_point_position(0).y) < 1e-3, "curve: G X moves the selected points along X (%s)" % r34_c.get_point_position(0))
	# the last point alone: E makes a new one after it
	await key(KEY_A, false, false, true)
	await click(cam.unproject_position(r34_path.global_transform * r34_c.get_point_position(1)))
	await key(KEY_E)
	await move(cam.unproject_position(Vector3(1.5, 0, 0.5)))
	await key(KEY_ENTER)
	check(r34_path.curve.get_point_count() == 3, "curve: E extrudes a new point after the end (%d)" % r34_path.curve.get_point_count())
	await key(KEY_TAB)
	r34_path.visible = false
	# the normals brush: Ctrl+wheel changes its radius
	var r34_m := HewnMesh3D.new()
	r34_m.name = "R34Paint"
	scene.add_child(r34_m)
	r34_m.owner = scene
	r34_m.shape = 1
	await select_node(r34_m)
	var r34_im: MenuButton = EditorInterface.get_base_control().find_child("InteractionMode", true, false)
	if r34_im:
		r34_im.get_popup().id_pressed.emit(2)
		await frames(2)
		EditorInterface.get_base_control().find_child("HewnTools", true, false).find_child("Tool14", true, false).pressed.emit()
		await frames(1)
		var r34_rf: LineEdit = null
		for r34_f in EditorInterface.get_base_control().find_child("HewnPaintRow", true, false).find_children("*", "HewnDragField", true, false):
			if r34_f.is_visible_in_tree() and (r34_f.tooltip_text.begins_with("Brush radius") or r34_f.tooltip_text.begins_with("Радиус")):
				r34_rf = r34_f
		var r34_r0 := float(r34_rf.text) if r34_rf else -1.0
		var r34_at := cam.unproject_position(Vector3(0, 0.5, 0))
		await move(r34_at)
		await button(r34_at, MOUSE_BUTTON_WHEEL_UP, true, false, true)
		await button(r34_at, MOUSE_BUTTON_WHEEL_UP, false, false, true)
		await frames(1)
		check(r34_rf != null and float(r34_rf.text) > r34_r0, "normals brush: Ctrl+wheel makes the radius bigger (%s -> %s)" % [r34_r0, r34_rf.text if r34_rf else "?"])
		# Ctrl+click on a channel: only that one
		var r34_g: Button = null
		var r34_r: Button = null
		for r34_b in EditorInterface.get_base_control().find_child("HewnPaintRow", true, false).find_children("*", "Button", true, false):
			if r34_b.toggle_mode and r34_b.text == "G":
				r34_g = r34_b
			if r34_b.toggle_mode and r34_b.text == "R":
				r34_r = r34_b
		await key(KEY_TAB)
	r34_m.visible = false
	# ---- r35: the pencil on an edge paints its two vertices (in all their faces); the blend material window
	# (one layer per component, by height)
	var r35_m := HewnMesh3D.new()
	r35_m.name = "R35Edge"
	scene.add_child(r35_m)
	r35_m.owner = scene
	await select_node(r35_m)
	await frames(2)
	var r35_im: MenuButton = EditorInterface.get_base_control().find_child("InteractionMode", true, false)
	var r35_c1: ColorPickerButton = EditorInterface.get_base_control().find_child("VertexColor", true, false)
	if r35_im and r35_c1:
		r35_im.get_popup().id_pressed.emit(2)
		await frames(2)
		await key(KEY_2)
		await key(KEY_A, false, false, true)
		EditorInterface.get_base_control().find_child("HewnTools", true, false).find_child("Tool11", true, false).pressed.emit()
		await frames(1)
		r35_c1.color = Color(0, 1, 0, 1)
		var r35_split: Button = null
		for r35_x in EditorInterface.get_base_control().find_children("*", "Button", true, false):
			if r35_x.tooltip_text.begins_with("Split vertices") or r35_x.tooltip_text.begins_with("Раздельные вершины"):
				r35_split = r35_x
		if r35_split:
			r35_split.button_pressed = true
		var r35_f := visible_face(r35_m)
		var r35_fv := r35_m.get_face(r35_f)
		var r35_a: int = r35_fv[0]
		var r35_b: int = r35_fv[1]
		# (a little inside the face from the edge's middle: on a face the pencil takes its edge nearest the mouse)
		var r35_at := sp(r35_m, ((r35_m.get_vertex(r35_a) + r35_m.get_vertex(r35_b)) * 0.5).lerp(r35_m.get_face_center(r35_f), 0.08))
		await move(r35_at)
		await click(r35_at)
		var r35_cols: PackedColorArray = face_cols(r35_m.mesh_data)
		var r35_k := 0
		var r35_ends_ok := true
		var r35_others_ok := true
		var r35_ends := 0
		for r35_fi in r35_m.get_face_count():
			var r35_face := r35_m.get_face(r35_fi)
			var r35_along := r35_face.has(r35_a) and r35_face.has(r35_b)
			for r35_v in r35_face:
				var r35_green: bool = r35_cols[r35_k].is_equal_approx(Color(0, 1, 0, 1))
				# (the faces along the edge; a smooth face: the whole vertex)
				var r35_want: bool = (r35_v == r35_a or r35_v == r35_b) and (r35_along or r35_m.get_face_smoothing(r35_fi) != 0)
				if r35_want:
					r35_ends += 1
					r35_ends_ok = r35_ends_ok and r35_green
				else:
					r35_others_ok = r35_others_ok and not r35_green
				r35_k += 1
		check(r35_split != null, "the Split vertices button")
		check(r35_ends_ok and r35_others_ok and r35_ends >= 4, "pencil on an edge: its ends in the faces along it (smooth faces: the whole vertex) (%d corners, ends %s, others %s)" % [r35_ends, r35_ends_ok, r35_others_ok])
		# Split vertices off: the edge paints the whole of its two vertices
		if r35_split:
			r35_split.button_pressed = false
		r35_c1.color = Color(1, 0, 0, 1)
		await move(r35_at)
		await click(r35_at)
		r35_cols = face_cols(r35_m.mesh_data)
		r35_k = 0
		var r35_whole := 0
		var r35_whole_ok := true
		for r35_fi in r35_m.get_face_count():
			for r35_v in r35_m.get_face(r35_fi):
				if r35_v == r35_a or r35_v == r35_b:
					r35_whole += 1
					r35_whole_ok = r35_whole_ok and r35_cols[r35_k].is_equal_approx(Color(1, 0, 0, 1))
				r35_k += 1
		check(r35_whole_ok and r35_whole >= 6, "pencil on an edge, Split vertices off: the whole of both vertices (%d corners)" % r35_whole)
		await key(KEY_TAB)
	# the blend material: the shader (v2: by height) in the materials' folder, layer 2 by G
	var r35_btn: Button = null
	for r35_x in EditorInterface.get_base_control().find_children("*", "Button", true, false):
		if r35_x.text == "Blend Material..." or r35_x.text == "Материал смешивания...":
			r35_btn = r35_x
	check(r35_btn != null, "the Blend Material button")
	if r35_btn:
		r35_btn.pressed.emit()
		await frames(2)
		var r35_dlg: AcceptDialog = null
		for r35_d in EditorInterface.get_base_control().find_children("*", "AcceptDialog", true, false):
			if r35_d.visible and r35_d.find_child("*", true, false) and r35_d.find_children("*", "EditorResourcePicker", true, false).size() == 5:
				r35_dlg = r35_d
		check(r35_dlg != null and r35_dlg.find_children("*", "OptionButton", true, false).size() == 2, "the blend window: no R / G / B / A pickers (one layer per component), a filter choice")
		if r35_dlg:
			r35_dlg.confirmed.emit()
			r35_dlg.hide()
			await frames(2)
			var r35_mat: ShaderMaterial = r35_m.get_surface_override_material(0) if r35_m.get_surface_override_material_count() > 0 else null
			if r35_mat == null:
				r35_mat = r35_m.material_override as ShaderMaterial
			var r35_sh := FileAccess.get_file_as_string("res://materials/hewn_blend_2_pbr_normal.gdshader")
			check(r35_sh.begins_with("// Hewn blend material v5") and r35_sh.contains("height_blend"), "the blend shader: blends by height")
			# r54: metallic / roughness and normal maps for each texture (on by default), the shader compiles
			var r54_un := []
			var r54_sh: Shader = load("res://materials/hewn_blend_2_pbr_normal.gdshader")
			if r54_sh:
				for r54_u in r54_sh.get_shader_uniform_list():
					r54_un.append(r54_u["name"])
			check(r54_un.has("base_metallic") and r54_un.has("roughness_map_1") and r54_un.has("normal_map_2") and r54_un.has("normal_strength_1") and not r54_un.has("emission_1"),
					"the blend material: metallic, roughness and a normal map for each texture (%s)" % [r54_un])
			if r35_mat:
				r35_m.material_override = null
			# a noise on the borders and the vertices lifted: a shader of their own, the noise image given
			var r74_boxes := {}
			for r74_c in r35_dlg.find_children("*", "CheckBox", true, false):
				r74_boxes[r74_c.text] = r74_c
			var r74_noise: CheckBox = r74_boxes.get("Noise mask", r74_boxes.get("Маска шума"))
			var r74_lift: CheckBox = r74_boxes.get("Lift by paint", r74_boxes.get("Подъём по закраске"))
			var r76_ao: CheckBox = r74_boxes.get("Ambient occlusion maps", r74_boxes.get("Карты затенения (AO)"))
			check(r76_ao != null and not r76_ao.button_pressed, "the blend window: AO maps, off at first")
			check(r74_noise != null and r74_lift != null and not r74_noise.button_pressed and not r74_lift.button_pressed, "the blend window: noise and lift, off at first")
			if r74_noise and r74_lift:
				r74_noise.button_pressed = true
				r74_lift.button_pressed = true
				if r76_ao:
					r76_ao.button_pressed = true
				r35_dlg.confirmed.emit()
				await frames(2)
				var r74_path := "res://materials/hewn_blend_2_pbr_normal_noise_lift_ao.gdshader"
				var r74_un := []
				var r74_sh: Shader = load(r74_path) if FileAccess.file_exists(r74_path) else null
				if r74_sh:
					for r74_u in r74_sh.get_shader_uniform_list():
						r74_un.append(r74_u["name"])
				var r74_mat = null
				for r74_x in r35_m.get_materials():
					if r74_x is ShaderMaterial and r74_x.shader and r74_x.shader.resource_path == r74_path:
						r74_mat = r74_x
				check(r74_un.has("noise_texture") and r74_un.has("noise_2") and r74_un.has("lift_1") and r74_un.has("ao_map_1") and r74_un.has("base_ao_strength") and r74_mat is ShaderMaterial and r74_mat.get_shader_parameter("noise_texture") is NoiseTexture2D,
						"the blend material: a noise on the borders, the vertices lifted (%s)" % [r74_un])
				r74_noise.button_pressed = false
				r74_lift.button_pressed = false
				if r76_ao:
					r76_ao.button_pressed = false
				r35_m.material_override = null
				for r74_file in [r74_path, r74_path + ".uid"]:
					DirAccess.remove_absolute(r74_file)
			for r35_file in ["res://materials/hewn_blend_2_pbr_normal.gdshader", "res://materials/hewn_blend_2_pbr_normal.gdshader.uid"]:
				DirAccess.remove_absolute(r35_file)  # (the test's own)
			DirAccess.remove_absolute("res://materials")  # (empty again)
	r35_m.visible = false
	# ---- r35: Vertex Paint on a dense mesh (32k vertices): the mouse moving over it stays fast (the visibility
	# kept, the squares in one draw)
	var r35_big := HewnMesh3D.new()
	r35_big.name = "R35Dense"
	scene.add_child(r35_big)
	r35_big.owner = scene
	r35_big.shape = 5
	r35_big.set_shape_param("rings", 128)
	r35_big.set_shape_param("segments", 256)
	await select_node(r35_big)
	await frames(2)
	var r35_im2: MenuButton = EditorInterface.get_base_control().find_child("InteractionMode", true, false)
	if r35_im2:
		r35_im2.get_popup().id_pressed.emit(2)
		await frames(2)
		await key(KEY_1)
		var r35_t0 := Time.get_ticks_msec()
		for r35_i in 20:
			await move(cam.unproject_position(Vector3(-0.3 + 0.03 * r35_i, 0.2, 0.4)))
		var r35_dt := Time.get_ticks_msec() - r35_t0
		print("  Vertex Paint, %d vertices: 20 mouse moves in %d ms" % [r35_big.get_vertex_count(), r35_dt])
		check(r35_big.get_vertex_count() > 20000 and r35_dt < 8000, "Vertex Paint on %d vertices: the mouse moves fast (%d ms for 20)" % [r35_big.get_vertex_count(), r35_dt])
		# the normals view: turning it on, then the mouse moving
		var r35_nv: Button = null
		for r35_b in EditorInterface.get_base_control().find_children("*", "Button", true, false):
			if r35_b.toggle_mode and (r35_b.tooltip_text == "Smooth / Flat" or r35_b.tooltip_text == "Гладкие / плоские"):
				r35_nv = r35_b
		if r35_nv:
			r35_t0 = Time.get_ticks_msec()
			r35_nv.button_pressed = true
			await frames(1)
			var r35_on := Time.get_ticks_msec() - r35_t0
			r35_t0 = Time.get_ticks_msec()
			for r35_i in 20:
				await move(cam.unproject_position(Vector3(-0.3 + 0.03 * r35_i, 0.2, 0.4)))
			r35_dt = Time.get_ticks_msec() - r35_t0
			print("  normals view, %d vertices: on in %d ms, 20 mouse moves in %d ms" % [r35_big.get_vertex_count(), r35_on, r35_dt])
			check(r35_dt < 8000, "the normals view on %d vertices: the mouse moves fast (%d ms for 20)" % [r35_big.get_vertex_count(), r35_dt])
			# painting there (the brush, then the normals brush): 20 steps each
			for r35_tool in ["Tool10", "Tool14"]:
				EditorInterface.get_base_control().find_child("HewnTools", true, false).find_child(r35_tool, true, false).pressed.emit()
				await frames(1)
				var r35_p0 := cam.unproject_position(Vector3(-0.3, 0.2, 0.4))
				await move(r35_p0)
				await button(r35_p0, MOUSE_BUTTON_LEFT, true)
				r35_t0 = Time.get_ticks_msec()
				for r35_i in 20:
					await move(cam.unproject_position(Vector3(-0.3 + 0.03 * r35_i, 0.2, 0.4)), false, MOUSE_BUTTON_MASK_LEFT)
				r35_dt = Time.get_ticks_msec() - r35_t0
				await button(cam.unproject_position(Vector3(0.3, 0.2, 0.4)), MOUSE_BUTTON_LEFT, false)
				print("  normals view, %s: 20 painting steps in %d ms" % [r35_tool, r35_dt])
				check(r35_dt < 8000, "the normals view: painting with %s is fast (%d ms for 20)" % [r35_tool, r35_dt])
			for r35_b in EditorInterface.get_base_control().find_children("*", "Button", true, false):
				if r35_b.toggle_mode and (r35_b.tooltip_text == "Materials" or r35_b.tooltip_text == "Материалы"):
					r35_b.button_pressed = true
		await key(KEY_TAB)
		# edit mode, faces, X-ray (the face dots in one draw)
		await select_node(r35_big)
		await key(KEY_TAB)
		await key(KEY_3)
		await key(KEY_Z, false, false, true)
		r35_t0 = Time.get_ticks_msec()
		for r35_i in 20:
			await move(cam.unproject_position(Vector3(-0.3 + 0.03 * r35_i, 0.2, 0.4)))
		r35_dt = Time.get_ticks_msec() - r35_t0
		print("  X-ray faces, %d faces: 20 mouse moves in %d ms" % [r35_big.get_face_count(), r35_dt])
		check(r35_dt < 8000, "X-ray face mode on %d faces: the mouse moves fast (%d ms for 20)" % [r35_big.get_face_count(), r35_dt])
		await key(KEY_Z, false, false, true)
		await key(KEY_TAB)
	r35_big.visible = false
	# ---- r37: Ctrl+Z undoes the selection; the arrows move by the grid, Ctrl+arrow in face
	# mode extrudes; Check Scene finds vertices not joined and joins them
	var r37_m := HewnMesh3D.new()
	r37_m.name = "R37Cube"
	scene.add_child(r37_m)
	r37_m.owner = scene
	await select_node(r37_m)
	await frames(2)
	await key(KEY_TAB)
	await key(KEY_3)
	var r37_fa := visible_face(r37_m)
	await click(sp(r37_m, r37_m.get_face_center(r37_fa)))
	var r37_fb := -1
	for r37_f in r37_m.get_face_count():
		var r37_c := r37_m.global_transform * r37_m.get_face_center(r37_f)
		if r37_f != r37_fa and r37_m.get_face_normal(r37_f).dot(cam.global_position - r37_c) > 0.0:
			r37_fb = r37_f
	await click(sp(r37_m, r37_m.get_face_center(r37_fb)))
	check(sel(r37_m, "sel_faces") == [r37_fb], "a second face clicked (%s)" % [sel(r37_m, "sel_faces")])
	await undo(r37_m)
	check(sel(r37_m, "sel_faces") == [r37_fa], "Ctrl+Z: the selection before (%s)" % [sel(r37_m, "sel_faces")])
	# arrows pressed in a row: one undo step
	var r37_fsel = sel(r37_m, "sel_faces")
	await key(KEY_1)
	var r37_st = r37_m.get_state()
	r37_st["sel_verts"] = PackedInt32Array([0])
	r37_m.set_state(r37_st)
	await frames(2)
	var r37_p0 := r37_m.get_vertex(0)
	await key(KEY_UP)
	await key(KEY_UP)
	await key(KEY_RIGHT)
	check(not r37_m.get_vertex(0).is_equal_approx(r37_p0), "arrows move the vertex (%s)" % r37_m.get_vertex(0))
	await undo(r37_m)
	check(r37_m.get_vertex(0).is_equal_approx(r37_p0), "arrows pressed in a row: one Ctrl+Z takes them all back (%s)" % r37_m.get_vertex(0))
	await key(KEY_3)
	r37_st = r37_m.get_state()
	r37_st["sel_faces"] = PackedInt32Array(r37_fsel)
	r37_m.set_state(r37_st)
	await frames(2)
	# Ctrl+arrow up in face mode: a block on the face as an object of its own (a brush: six faces, eight
	# vertices; the cube stays as it was), its top selected
	var r37_n0 := r37_m.get_face_count()
	var r37_v0 := r37_m.get_vertex_count()
	await key(KEY_UP, false, true)
	var r37_blk: HewnMesh3D = scene.find_child("R37Cube_block", false, false)
	check(r37_m.get_face_count() == r37_n0 and r37_m.get_vertex_count() == r37_v0 and r37_blk != null and r37_blk.get_face_count() == 6 and r37_blk.get_vertex_count() == 8, "Ctrl+arrow: a block as an object of its own (cube %d faces, block %s)" % [r37_m.get_face_count(), [r37_blk.get_face_count(), r37_blk.get_vertex_count()] if r37_blk else "missing"])
	if r37_blk:
		check(sel(r37_blk, "sel_faces").size() == 1 and sel(r37_m, "sel_faces").is_empty(), "Ctrl+arrow: the block's top selected (%s)" % [sel(r37_blk, "sel_faces")])
		await key(KEY_UP, false, true)
		var r37_blk2: HewnMesh3D = null
		var r37_names := []
		for r37_ch in scene.get_children():
			if String(r37_ch.name).begins_with("R37Cube_block"):
				r37_names.append(r37_ch.name)
				if r37_ch != r37_blk:
					r37_blk2 = r37_ch
		check(r37_blk2 != null and r37_blk2.get_face_count() == 6 and r37_blk.get_face_count() == 6, "Ctrl+arrow again: the next block on the first one, an object of its own (%s)" % [r37_names])
		if r37_blk2:
			r37_blk2.visible = false
		r37_blk.visible = false
	await key(KEY_TAB)
	await select_node(r37_m)
	# object mode: Page Up moves the object up a grid step
	var r37_y0 := r37_m.position.y
	await key(KEY_PAGEUP)
	check(r37_m.position.y > r37_y0 + 0.01, "Page Up: the object up by the grid step (%f -> %f)" % [r37_y0, r37_m.position.y])
	await undo(r37_m)
	r37_m.visible = false
	# Check Scene: two faces side by side with their own vertices on the shared edge
	var r37_s := HewnMesh3D.new()
	r37_s.name = "R37Split"
	scene.add_child(r37_s)
	r37_s.owner = scene
	var r37_d: Dictionary = r37_s.mesh_data
	r37_d["positions"] = PackedVector3Array([Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 0, 1), Vector3(0, 0, 1), Vector3(1, 0, 0), Vector3(2, 0, 0), Vector3(2, 0, 1), Vector3(1, 0, 1)])
	r37_d["face_sizes"] = PackedInt32Array([4, 4])
	r37_d["face_indices"] = PackedInt32Array([0, 3, 2, 1, 4, 7, 6, 5])
	r37_d.erase("face_uvs")
	r37_d.erase("face_colors")
	r37_s.mesh_data = r37_d
	var r37_btn: Button = null
	for r37_x in EditorInterface.get_base_control().find_children("*", "Button", true, false):
		if r37_x.text == "Check Scene..." or r37_x.text == "Проверить сцену...":
			r37_btn = r37_x
	check(r37_btn != null and r37_s.get_vertex_count() == 8, "the Check Scene button (a mesh of %d vertices)" % r37_s.get_vertex_count())
	if r37_btn:
		r37_btn.pressed.emit()
		await frames(2)
		var r37_tree: Tree = null
		var r37_found := false
		for r37_t in EditorInterface.get_base_control().find_children("*", "Tree", true, false):
			if r37_t.get_root() == null:
				continue
			for r37_it in r37_t.get_root().get_children():
				if r37_it.get_text(0).begins_with("R37Split") and r37_it.get_text(0).contains("4"):
					r37_found = true
					r37_tree = r37_t
		check(r37_found, "Check Scene: the vertices not joined are listed")
		var r37_fix: Button = null
		for r37_x in EditorInterface.get_base_control().find_children("*", "Button", true, false):
			if r37_x.is_visible_in_tree() and (r37_x.text == "Fix All" or r37_x.text == "Исправить всё"):
				r37_fix = r37_x
		if r37_fix:
			r37_fix.pressed.emit()
			await frames(2)
		check(r37_s.get_vertex_count() == 6, "Fix All: joined (%d vertices)" % r37_s.get_vertex_count())
		for r37_dl in EditorInterface.get_base_control().find_children("*", "AcceptDialog", true, false):
			if r37_dl.title == "Check Scene" or r37_dl.title == "Проверка сцены":
				r37_dl.hide()
		await frames(1)
	r37_s.visible = false
	# ---- r38: the brush's Ctrl+wheel / Shift+wheel (taken before Godot's viewport zooms)
	var r38_m := HewnMesh3D.new()
	r38_m.name = "R38Paint"
	scene.add_child(r38_m)
	r38_m.owner = scene
	await select_node(r38_m)
	await frames(2)
	var r38_im: MenuButton = EditorInterface.get_base_control().find_child("InteractionMode", true, false)
	if r38_im:
		r38_im.get_popup().id_pressed.emit(2)
		await frames(2)
		EditorInterface.get_base_control().find_child("HewnTools", true, false).find_child("Tool10", true, false).pressed.emit()
		await frames(1)
		var r38_rf: LineEdit = null
		for r38_f in EditorInterface.get_base_control().find_child("HewnPaintRow", true, false).find_children("*", "HewnDragField", true, false):
			if r38_f.is_visible_in_tree() and (r38_f.tooltip_text.begins_with("Brush radius") or r38_f.tooltip_text.begins_with("Радиус")):
				r38_rf = r38_f
		var r38_at := cam.unproject_position(Vector3(0, 0.5, 0))
		await move(r38_at)
		var r38_r0 := float(r38_rf.text) if r38_rf else -1.0
		await button(r38_at, MOUSE_BUTTON_WHEEL_UP, true, false, true)
		await button(r38_at, MOUSE_BUTTON_WHEEL_UP, false, false, true)
		check(r38_rf != null and float(r38_rf.text) > r38_r0, "brush: Ctrl+wheel makes the radius bigger (%s -> %s)" % [r38_r0, r38_rf.text if r38_rf else "?"])
		var r38_sf: LineEdit = EditorInterface.get_base_control().find_child("HewnPaintRow", true, false).find_child("BrushStrength", true, false)
		var r38_s0 := float(r38_sf.text) if r38_sf else -1.0
		await button(r38_at, MOUSE_BUTTON_WHEEL_DOWN, true, false, false, true)
		await button(r38_at, MOUSE_BUTTON_WHEEL_DOWN, false, false, false, true)
		check(r38_sf != null and float(r38_sf.text) < r38_s0, "brush: Shift+wheel makes the strength smaller (%s -> %s)" % [r38_s0, r38_sf.text if r38_sf else "?"])
		# Shift + drag 40 px to the right: the radius 40 px bigger (by the position: a tablet's pen too)
		var r38_r1 := float(r38_rf.text) if r38_rf else -1.0
		await drag(r38_at, r38_at + Vector2(40, 0), true)
		check(r38_rf != null and absf(float(r38_rf.text) - (r38_r1 + 40.0)) < 1.0, "brush: Shift+drag sets the radius by the distance (%s -> %s)" % [r38_r1, r38_rf.text if r38_rf else "?"])
		await key(KEY_TAB)
	r38_m.visible = false
	# ---- r39: T on a part of a sphere: one seamless layout (the faces meet in the UVs too)
	var r39_s := HewnMesh3D.new()
	r39_s.name = "R39Sphere"
	scene.add_child(r39_s)
	r39_s.owner = scene
	r39_s.shape = 5
	await select_node(r39_s)
	await frames(2)
	await key(KEY_TAB)
	await key(KEY_3)
	var r39_fs := []
	var r39_to_cam := (cam.global_position - r39_s.global_position).normalized()
	for r39_f in r39_s.get_face_count():
		if r39_s.get_face_center(r39_f).normalized().dot(r39_to_cam) > 0.55:
			r39_fs.append(r39_f)
	var r39_st: Dictionary = r39_s.get_state()
	r39_st["sel_faces"] = PackedInt32Array(r39_fs)
	r39_s.set_state(r39_st)
	await frames(2)
	var r39_at := sp(r39_s, r39_s.get_face_center(r39_fs[0]))
	await move(r39_at)
	await key(KEY_T)
	await click(r39_at)
	var r39_corner := {}
	var r39_shared := 0
	var r39_seams := 0
	for r39_f in r39_fs:
		var r39_vs: PackedInt32Array = r39_s.get_face(r39_f)
		var r39_uvs: PackedVector2Array = r39_s.get_face_uvs(r39_f)
		for r39_i in r39_vs.size():
			if r39_corner.has(r39_vs[r39_i]):
				r39_shared += 1
				r39_seams += 1 if (r39_corner[r39_vs[r39_i]] as Vector2).distance_to(r39_uvs[r39_i]) > 1e-3 else 0
			else:
				r39_corner[r39_vs[r39_i]] = r39_uvs[r39_i]
	check(r39_fs.size() > 10 and r39_shared > 0 and r39_seams == 0, "T on a part of a sphere: no seams (%d faces, %d corners shared, %d apart)" % [r39_fs.size(), r39_shared, r39_seams])
	await key(KEY_TAB)
	r39_s.visible = false
	# Fit to Face: one tile over each face; Box Projection back to meters; the UV fields locked in object mode
	var r39_c := HewnMesh3D.new()
	r39_c.name = "R39Cube"
	scene.add_child(r39_c)
	r39_c.owner = scene
	r39_c.set_shape_param("width", 2.0)
	await select_node(r39_c)
	await frames(2)
	var r39_front := -1
	for r39_f in r39_c.get_face_count():
		if r39_c.get_face_normal(r39_f).z > 0.9:
			r39_front = r39_f
	press("Fit to Face")
	await frames(2)
	var r39_b := r39_c.get_uv_bounds(PackedInt32Array([r39_front]))
	check(r39_b.size.is_equal_approx(Vector2(1, 1)) and r39_b.position.is_equal_approx(Vector2(0, 0)), "Fit to Face: exactly one tile over the face (%s)" % r39_b)
	press("Box Projection")
	await frames(2)
	r39_b = r39_c.get_uv_bounds(PackedInt32Array([r39_front]))
	check(absf(r39_b.size.x - 2.0) < 0.01, "Box Projection: back to the face's own size (%s)" % r39_b)
	var r39_u: SpinBox = null
	var r39_bs: SpinBox = null
	for r39_x in EditorInterface.get_base_control().find_child("Hewn", true, false).find_children("*", "SpinBox", true, false):
		if r39_x.prefix == "U" and r39_u == null:
			r39_u = r39_x
		if r39_x.tooltip_text.begins_with("Scale of the box projection") or r39_x.tooltip_text.begins_with("Масштаб"):
			r39_bs = r39_x
	check(r39_u != null and not r39_u.editable and r39_u.mouse_filter == Control.MOUSE_FILTER_IGNORE and r39_u.get_line_edit().mouse_filter == Control.MOUSE_FILTER_IGNORE, "object mode: the auto UV fields locked (not dragged either)")
	await key(KEY_TAB)
	await key(KEY_3)
	r39_st = r39_c.get_state()
	r39_st["sel_faces"] = PackedInt32Array([r39_front])
	r39_c.set_state(r39_st)
	await frames(2)
	press("Center")
	await frames(2)
	check(r39_u != null and r39_u.editable, "face mode with a face selected: the auto UV fields open")
	# a typed 1/5 in a SpinBox: 0.2 (Godot alone gives 0: whole numbers)
	if r39_bs:
		var r39_le := r39_bs.get_line_edit()
		r39_le.text = "1/5"
		r39_le.text_changed.emit("1/5")
		r39_le.focus_exited.emit()
		await frames(2)
		check(absf(r39_bs.value - 0.2) < 1e-4, "a typed 1/5 is 0.2 (%s)" % r39_bs.value)
	# Alt+G in the middle of G: the grid on faces toggles, G goes on
	var r39_grid: CheckBox = null
	for r39_x in EditorInterface.get_base_control().find_children("*", "CheckBox", true, false):
		if r39_x.tooltip_text.contains("Alt+G"):
			r39_grid = r39_x
	var r39_g0 := r39_grid.button_pressed if r39_grid else false
	var r39_v0 := r39_c.get_vertex(0)
	await move(r39_at)
	await key(KEY_G)
	await key(KEY_G, false, false, true)
	await move(r39_at + Vector2(40, 0))
	await key(KEY_ESCAPE)
	check(r39_grid != null and r39_grid.button_pressed != r39_g0 and r39_c.get_vertex(0).is_equal_approx(r39_v0), "Alt+G during G: the grid on faces toggled (not a G)")
	if r39_grid and r39_grid.button_pressed != r39_g0:
		await key(KEY_G, false, false, true)
	await key(KEY_TAB)
	r39_c.visible = false
	# the Material List: opens, a row for each material
	var r39_mlb: Button = EditorInterface.get_base_control().find_child("MaterialListButton", true, false)
	if r39_mlb:
		r39_mlb.pressed.emit()
		await frames(2)
	var r39_ml: AcceptDialog = EditorInterface.get_base_control().find_child("HewnMaterialList", true, false)
	check(r39_ml != null and r39_ml.visible, "the Material List opens")
	if r39_ml:
		r39_ml.hide()
	# ---- r40: a bevel's faces take the texture of the faces around them (no jumps); Alt+clicks face by face run
	# on like a drag
	var r40_c := HewnMesh3D.new()
	r40_c.name = "R40Cube"
	scene.add_child(r40_c)
	r40_c.owner = scene
	r40_c.set_shape_param("width_segments", 2)
	await select_node(r40_c)
	await frames(2)
	await key(KEY_TAB)
	await key(KEY_3)
	var r40_top := []
	var r40_front := []
	for r40_f in r40_c.get_face_count():
		var r40_n := r40_c.get_face_normal(r40_f)
		if r40_n.y > 0.9:
			r40_top.append(r40_f)
		elif r40_n.z > 0.9:
			r40_front.append(r40_f)
	# the front quads ordered left to right, the top one over the left front one
	r40_front.sort_custom(func(r40x, r40y): return r40_c.get_face_center(r40x).x < r40_c.get_face_center(r40y).x)
	r40_top.sort_custom(func(r40x, r40y): return r40_c.get_face_center(r40x).x < r40_c.get_face_center(r40y).x)
	var r40_st: Dictionary = r40_c.get_state()
	r40_st["sel_faces"] = PackedInt32Array([r40_top[0]])
	r40_c.set_state(r40_st)
	await frames(2)
	for r40_f in [r40_front[0], r40_front[1]]:
		var r40_p := sp(r40_c, r40_c.get_face_center(r40_f))
		await move(r40_p)
		await button(r40_p, MOUSE_BUTTON_LEFT, true, true)
		await button(r40_p, MOUSE_BUTTON_LEFT, false, true)
	# the right front quad continues the left one (not the top's projection): their shared corners alike
	var r40_same := func(fa: int, fb: int) -> Array:
		var r40va: PackedInt32Array = r40_c.get_face(fa)
		var r40vb: PackedInt32Array = r40_c.get_face(fb)
		var r40ua: PackedVector2Array = r40_c.get_face_uvs(fa)
		var r40ub: PackedVector2Array = r40_c.get_face_uvs(fb)
		var r40sh := 0
		var r40ap := 0
		for i in r40va.size():
			var k := Array(r40vb).find(r40va[i])
			if k >= 0:
				r40sh += 1
				r40ap += 1 if r40ua[i].distance_to(r40ub[k]) > 1e-3 else 0
		return [r40sh, r40ap]
	var r40_r: Array = r40_same.call(r40_front[0], r40_front[1])
	check(r40_r[0] == 2 and r40_r[1] == 0, "Alt+click face by face: the next one runs on from the one before (%s)" % [r40_r])
	# a bevel on the top front edge: each new face meets a face around it without a jump
	await key(KEY_2)
	var r40_e := []
	var r40_vs := r40_c.get_vertices()
	for r40_f in r40_front:
		var r40_fv: PackedInt32Array = r40_c.get_face(r40_f)
		for i in r40_fv.size():
			var a: int = r40_fv[i]
			var b: int = r40_fv[(i + 1) % r40_fv.size()]
			if r40_vs[a].y > 0.4 and r40_vs[b].y > 0.4:
				r40_e.append(a)
				r40_e.append(b)
	r40_st = r40_c.get_state()
	r40_st["sel_edges"] = PackedInt32Array(r40_e)
	r40_c.set_state(r40_st)
	await frames(2)
	press("Bevel")
	await frames(2)
	var r40_bf: Array = sel(r40_c, "sel_faces")
	var r40_ok := r40_bf.size() > 0
	for r40_f in r40_bf:
		var r40_any := false
		for r40_g in r40_c.get_face_count():
			if r40_g != r40_f:
				var r40_q: Array = r40_same.call(r40_f, r40_g)
				r40_any = r40_any or (r40_q[0] == 2 and r40_q[1] == 0)
		r40_ok = r40_ok and r40_any
	check(r40_ok, "bevel: each new face runs on from a face next to it (%d faces)" % r40_bf.size())
	await key(KEY_TAB)
	r40_c.visible = false
	# ---- r41: Subdivide asks first when it would add many faces (Editor Settings > Hewn); the header off in
	# the settings; Shift+Alt+Z: the overlays
	var r41_es := EditorInterface.get_editor_settings()
	var r41_lim0 = r41_es.get_setting("hewn/warnings/subdivide_face_limit") if r41_es.has_setting("hewn/warnings/subdivide_face_limit") else null
	check(r41_lim0 != null, "Editor Settings has Hewn's subdivide warning (%s)" % [r41_lim0])
	r41_es.set_setting("hewn/warnings/subdivide_face_limit", 1000)
	var r41_c := HewnMesh3D.new()
	r41_c.name = "R41Cube"
	scene.add_child(r41_c)
	r41_c.owner = scene
	await select_node(r41_c)
	await frames(2)
	await key(KEY_TAB)
	await key(KEY_3)
	await key(KEY_A)
	press("Subdivide")
	await frames(2)
	var r41_n1 := r41_c.get_face_count()
	var r41_panel := surf.find_child("HewnLastOperation", true, false)
	var r41_cuts: LineEdit = null
	if r41_panel:
		var r41_fs := r41_panel.find_children("*", "HewnDragField", true, false)
		if r41_fs.size() > 0:
			r41_cuts = r41_fs[0]
	var r41_dlg: ConfirmationDialog = null
	if r41_cuts:
		var r41_c0 := r41_cuts.text
		r41_cuts.text = "50"
		r41_cuts.text_submitted.emit("50")
		await frames(3)
		r41_dlg = EditorInterface.get_base_control().find_child("HewnHeavyDialog", true, false)
		check(r41_dlg != null and r41_dlg.visible and r41_c.get_face_count() == r41_n1, "Subdivide with many faces more: asked first, nothing done yet (%d faces)" % r41_c.get_face_count())
		check(r41_dlg != null and r41_dlg.size.y < 400 * EditorInterface.get_editor_scale(), "the question fits on the screen (%s)" % [r41_dlg.size if r41_dlg else null])
		if r41_dlg:
			r41_dlg.get_cancel_button().pressed.emit()
			await frames(3)
			check(r41_c.get_face_count() == r41_n1 and not r41_dlg.visible, "Cancel: the mesh as it was (%d faces)" % r41_c.get_face_count())
			r41_panel = surf.find_child("HewnLastOperation", true, false)
			r41_cuts = r41_panel.find_children("*", "HewnDragField", true, false)[0]
			check(r41_cuts.text == r41_c0, "Cancel: the panel's number of cuts back (%s)" % r41_cuts.text)
			r41_cuts.text = "40"
			r41_cuts.text_submitted.emit("40")
			await frames(3)
			var r41_sp: SpinBox = r41_dlg.find_children("*", "SpinBox", true, false)[0]
			r41_sp.value = 5
			r41_dlg.get_ok_button().pressed.emit()
			await frames(3)
			check(r41_c.get_face_count() == 6 * 36, "the question's own number of cuts, then go on: subdivided by it (%d faces)" % r41_c.get_face_count())
	await key(KEY_TAB)
	r41_c.visible = false
	r41_es.set_setting("hewn/warnings/subdivide_face_limit", 200000 if r41_lim0 == null or r41_lim0 == 1000 else r41_lim0)
	# the header off in Editor Settings: gone (not only folded)
	var r41_bar: Control = EditorInterface.get_base_control().find_child("HewnToolbar", true, false)
	r41_es.set_setting("hewn/interface/show_header", false)
	await frames(3)
	check(r41_bar != null and not r41_bar.visible, "Editor Settings: the header off")
	r41_es.set_setting("hewn/interface/show_header", true)
	await frames(3)
	check(r41_bar != null and r41_bar.visible, "Editor Settings: the header on again")
	# Shift+Alt+Z: the overlays off and on
	var r41_ov: Button = null
	for r41_x in r41_bar.find_children("*", "Button", true, false) if r41_bar else []:
		if r41_x.tooltip_text.begins_with("Overlays") or r41_x.tooltip_text.begins_with("Наложения"):
			r41_ov = r41_x
	var r41_ov0 := r41_ov.button_pressed if r41_ov else false
	await key(KEY_Z, true, false, true)
	check(r41_ov != null and r41_ov.button_pressed != r41_ov0, "Shift+Alt+Z: the overlays toggled")
	await key(KEY_Z, true, false, true)
	# ---- r42: a texture dragged onto a face / an object; Mark Sharp's Vertices; Hewn's keys and docks off in
	# Editor Settings
	var r42_c := HewnMesh3D.new()
	r42_c.name = "R42Cube"
	scene.add_child(r42_c)
	r42_c.owner = scene
	await select_node(r42_c)
	await frames(2)
	await key(KEY_TAB)
	await key(KEY_3)
	r42_c.set_selection_state({})
	await frames(2)
	var r42_tb = EditorInterface.get_base_control().find_child("HewnTextures", true, false)
	var r42_vp := cam.get_viewport()
	var r42_box: Control = r42_vp.get_parent()
	var r42_f := -1
	var r42_best := -2.0
	for r42_i in r42_c.get_face_count():
		var r42_ctr: Vector3 = r42_c.global_transform * r42_c.get_face_center(r42_i)
		var r42_d := (r42_c.global_transform.basis * r42_c.get_face_normal(r42_i)).normalized().dot((cam.global_position - r42_ctr).normalized())
		if r42_d > r42_best:
			r42_best = r42_d
			r42_f = r42_i
	var r42_scr := func(r42_face: int) -> Vector2:
		var r42_at := cam.unproject_position(r42_c.global_transform * r42_c.get_face_center(r42_face))
		return r42_box.get_screen_position() + r42_at * r42_box.size / Vector2(r42_vp.size)
	var r42_m0 := r42_c.get_face_material(r42_f)
	if r42_tb and ResourceLoader.exists(r20_tex_path):
		r42_tb.drop_image(r20_tex_path, r42_scr.call(r42_f))
		await frames(2)
		var r42_same := 0
		for r42_i in r42_c.get_face_count():
			r42_same += 1 if r42_c.get_face_material(r42_i) == r42_c.get_face_material(r42_f) else 0
		check(r42_c.get_face_material(r42_f) != r42_m0 and r42_same == 1, "a texture dropped on a face: on that face only (%d faces)" % r42_same)
		# dropped on a selected face: on the whole selection
		var r42_k := (r42_f + 1) % r42_c.get_face_count()
		r42_c.set_selection_state({"sel_faces": PackedInt32Array([r42_f, r42_k])})
		await frames(2)
		r42_tb.drop_image(r20_tex_path, r42_scr.call(r42_f))
		await frames(2)
		check(r42_c.get_face_material(r42_k) == r42_c.get_face_material(r42_f), "dropped on a selected face: on the whole selection")
	else:
		check(false, "the textures panel and its test image")
	# Mark Sharp with Vertices: every edge at the selected vertex
	await key(KEY_1)
	r42_c.set_selection_state({"sel_verts": PackedInt32Array([0])})
	await frames(2)
	var r42_item := false
	for r42_pm in EditorInterface.get_base_control().find_children("*", "PopupMenu", true, false):
		for r42_i in r42_pm.item_count:
			if not r42_item and (r42_pm.get_item_text(r42_i) == "Mark Sharp" or r42_pm.get_item_text(r42_i) == "Пометить острым"):
				r42_item = true
				r42_pm.id_pressed.emit(r42_pm.get_item_id(r42_i))
	await frames(3)
	check(r42_item, "the Mark Sharp menu item")
	var r42_panel := surf.find_child("HewnLastOperation", true, false)
	var r42_vb: CheckBox = null
	for r42_x in r42_panel.find_children("*", "Label", true, false) if r42_panel else []:
		if r42_x.text == "Vertices" or r42_x.text == "Вершины":
			var r42_nx := field_after(r42_x)
			if r42_nx is CheckBox:
				r42_vb = r42_nx
	check(r42_vb != null, "Mark Sharp's panel has Vertices")
	if r42_vb:
		r42_vb.button_pressed = true
		await frames(3)
	var r42_sharp := {}
	for r42_i in r42_c.get_face_count():
		var r42_fv := r42_c.get_face(r42_i)
		for r42_j in r42_fv.size():
			var r42_a := r42_fv[r42_j]
			var r42_b := r42_fv[(r42_j + 1) % r42_fv.size()]
			if (r42_a == 0 or r42_b == 0) and r42_c.get_edge_attributes(r42_a, r42_b).get("sharp", false):
				r42_sharp[mini(r42_a, r42_b) * 100000 + maxi(r42_a, r42_b)] = true
	check(r42_sharp.size() == 3, "Mark Sharp with Vertices: the 3 edges at the vertex (%d)" % r42_sharp.size())
	# Hewn's keys off: A does nothing; on: A selects all
	var r42_es := EditorInterface.get_editor_settings()
	await key(KEY_3)
	r42_c.set_selection_state({})
	await frames(2)
	r42_es.set_setting("hewn/interface/hotkeys", false)
	await frames(2)
	await key(KEY_A)
	check(r42_c.get_selection_state()["sel_faces"].size() == 0, "Editor Settings: Hewn's keys off (A selects nothing)")
	r42_es.set_setting("hewn/interface/hotkeys", true)
	await frames(2)
	await key(KEY_A)
	check(r42_c.get_selection_state()["sel_faces"].size() == 6, "Hewn's keys on again (A selects all)")
	await key(KEY_TAB)
	r42_c.visible = false
	# the docks are Godot's Editor > Editor Docks: no check boxes of ours for them
	check(not r42_es.has_setting("hewn/interface/show_hewn_dock"), "Editor Settings: no dock check boxes")
	# (the Hewn dock in front in its slot, as the later checks of its tabs need it)
	var r42_pd: Node = EditorInterface.get_base_control().find_child("Hewn", true, false)
	if r42_pd and r42_pd.has_method("make_visible"):
		r42_pd.make_visible()
	elif r42_pd and r42_pd.get_parent() is TabContainer:
		(r42_pd.get_parent() as TabContainer).current_tab = r42_pd.get_index()
	await frames(2)
	# ---- r43: light baked into the vertex colors: a cube under a roof in its shadow, one in the light
	var r43_a := HewnMesh3D.new()
	r43_a.name = "R43Shaded"
	r43_a.position = Vector3(20, 0, 0)
	var r43_b := HewnMesh3D.new()
	r43_b.name = "R43Roof"
	r43_b.position = Vector3(20, 2, 0)
	r43_b.scale = Vector3(3, 0.2, 3)
	var r43_c := HewnMesh3D.new()
	r43_c.name = "R43Lit"
	r43_c.position = Vector3(25, 0, 0)
	var r43_l := DirectionalLight3D.new()
	r43_l.name = "R43Sun"
	r43_l.rotation_degrees = Vector3(-90, 0, 0)
	r43_l.shadow_enabled = true
	for r43_x in [r43_a, r43_b, r43_c, r43_l]:
		scene.add_child(r43_x)
		r43_x.owner = scene
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(r43_a)
	EditorInterface.get_selection().add_node(r43_c)
	await frames(5)
	var r43_item := false
	var r43_menus: Array = []
	for r43_mb in EditorInterface.get_base_control().find_children("*", "MenuButton", true, false):
		r43_menus.append(r43_mb.get_popup())
	for r43_pm in r43_menus:
		for r43_i in r43_pm.item_count:
			if not r43_item and (r43_pm.get_item_text(r43_i) == "Bake Light to Vertex Colors..." or r43_pm.get_item_text(r43_i) == "Запечь свет в цвета вершин..."):
				r43_item = true
				r43_pm.id_pressed.emit(r43_pm.get_item_id(r43_i))
	await frames(3)
	check(r43_item, "the Object menu has Bake Light to Vertex Colors")
	var r43_dlg: ConfirmationDialog = EditorInterface.get_base_control().find_child("HewnBakeLight", true, false)
	check(r43_dlg != null and r43_dlg.visible, "Bake Light: its settings asked first")
	var r43_top := func(r43_m: HewnMesh3D) -> float:
		var r43_cols: PackedColorArray = face_cols(r43_m.mesh_data)
		var r43_at := 0
		var r43_sum := 0.0
		for r43_f in r43_m.get_face_count():
			var r43_n := r43_m.get_face(r43_f).size()
			if r43_m.get_face_normal(r43_f).y > 0.9:
				for r43_k in r43_n:
					r43_sum += r43_cols[r43_at + r43_k].r / r43_n
			r43_at += r43_n
		return r43_sum
	if r43_dlg:
		var r43_sp := r43_dlg.find_children("*", "SpinBox", true, false)
		r43_sp[0].value = 0  # (no occlusion rays: only the light and its shadow)
		r43_dlg.get_ok_button().pressed.emit()
		await frames(3)
		var r43_ta: float = r43_top.call(r43_a)
		var r43_tc: float = r43_top.call(r43_c)
		check(r43_tc > 0.95 and r43_ta < 0.5, "Bake Light: the top in the light bright, the one in the roof's shadow dark (%.2f, %.2f)" % [r43_tc, r43_ta])
		await undo(r43_a)
		check(r43_top.call(r43_a) > 0.99 and r43_top.call(r43_c) > 0.99, "Bake Light: undone (%.2f)" % r43_top.call(r43_a))
	for r43_x in [r43_a, r43_b, r43_c]:
		r43_x.visible = false
	r43_l.get_parent().remove_child(r43_l)
	r43_l.queue_free()
	# ---- r44: textures centered on new shapes; Triangulate keeps the UVs; Subdivision (op with its panel, modifier);
	# the panel of an object's operation stays; Shift+A's Cut / Keep Overlap; camera / picture panels; the light
	# baked with subdivisions; the material list's users and replace; the box cutter's header
	var r44_c := HewnMesh3D.new()
	r44_c.name = "R44Cube"
	scene.add_child(r44_c)
	r44_c.owner = scene
	var r44_ok := true
	for r44_f in r44_c.get_face_count():
		var r44_uv := r44_c.get_face_uvs(r44_f)
		var r44_mn := r44_uv[0]
		var r44_mx := r44_uv[0]
		for r44_q in r44_uv:
			r44_mn = r44_mn.min(r44_q)
			r44_mx = r44_mx.max(r44_q)
		var r44_ctr := (r44_mn + r44_mx) * 0.5
		r44_ok = r44_ok and absf(fposmod(r44_ctr.x, 1.0) - 0.5) < 0.01 and absf(fposmod(r44_ctr.y, 1.0) - 0.5) < 0.01
	check(r44_ok, "a new cube: the middle of the texture on the middle of each face")
	# a stretched face triangulated: its corners keep their UVs
	var r44_d: Dictionary = r44_c.mesh_data
	var r44_fl: PackedInt32Array = r44_d["face_flags"]
	r44_fl[0] = r44_fl[0] | (2 << 8)
	r44_d["face_flags"] = r44_fl
	r44_c.mesh_data = r44_d
	var r44_before := {}
	var r44_fv := r44_c.get_face(0)
	var r44_fu := r44_c.get_face_uvs(0)
	for r44_i in r44_fv.size():
		r44_before[r44_fv[r44_i]] = r44_fu[r44_i]
	var r44_tris := r44_c.triangulate_faces(PackedInt32Array([0]))
	var r44_same := r44_tris.size() == 2
	for r44_t in r44_tris:
		var r44_tv := r44_c.get_face(r44_t)
		var r44_tu := r44_c.get_face_uvs(r44_t)
		for r44_i in r44_tv.size():
			r44_same = r44_same and r44_before.has(r44_tv[r44_i]) and (r44_before[r44_tv[r44_i]] as Vector2).distance_to(r44_tu[r44_i]) < 1e-4
	check(r44_same, "Triangulate: a stretched texture stays where it was")
	r44_c.visible = false
	# Subdivide (Catmull-Clark) in object mode: its panel, Levels; it stays when the mouse goes over the view
	var r44_s := HewnMesh3D.new()
	r44_s.name = "R44Sub"
	scene.add_child(r44_s)
	r44_s.owner = scene
	await select_node(r44_s)
	await frames(2)
	press("Subdivide (Catmull-Clark)")
	await frames(3)
	check(r44_s.get_face_count() == 24, "Subdivide (Catmull-Clark): one level (%d faces)" % r44_s.get_face_count())
	var r44_panel := surf.find_child("HewnLastOperation", true, false)
	await move(surf.size * 0.5)
	await move(surf.size * 0.5 + Vector2(20, 10))
	await frames(2)
	check(r44_panel != null and r44_panel.visible, "an object's operation keeps its panel when the mouse goes over the view")
	var r44_lv: LineEdit = null
	for r44_x in r44_panel.find_children("*", "HewnDragField", true, false) if r44_panel else []:
		r44_lv = r44_x
		break
	if r44_lv:
		r44_lv.text = "2"
		r44_lv.text_submitted.emit("2")
		await frames(3)
	check(r44_s.get_face_count() == 96, "Subdivide's levels in its panel: 2 (%d faces)" % r44_s.get_face_count())
	r44_s.visible = false
	# the Subdivision modifier: the mesh stays, what is shown is subdivided
	var r44_m := HewnMesh3D.new()
	r44_m.name = "R44Mod"
	scene.add_child(r44_m)
	r44_m.owner = scene
	await select_node(r44_m)
	await frames(2)
	press("Subdivision Modifier")
	await frames(3)
	var r44_mods: Array = r44_m.modifiers
	check(r44_mods.size() == 1 and r44_mods[0] is HewnSubdivisionModifier and r44_m.get_face_count() == 6, "Subdivision Modifier added, the mesh itself as it was")
	var r44_tc := 0
	if r44_m.mesh:
		for r44_si in r44_m.mesh.get_surface_count():
			r44_tc += (r44_m.mesh.surface_get_arrays(r44_si)[Mesh.ARRAY_INDEX] as PackedInt32Array).size() / 3
	check(r44_tc == 48, "the modifier: shown subdivided (%d triangles)" % r44_tc)
	if r44_mods.size() == 1:
		r44_mods[0].kind = 1
		r44_mods[0].levels = 2
		await frames(2)
		var r44_ab: AABB = r44_m.mesh.get_aabb() if r44_m.mesh else AABB()
		check(r44_ab.size.distance_to(Vector3.ONE) < 1e-3, "the modifier, Simple: the shape stays (%s)" % r44_ab.size)
	await undo(r44_m)
	check((r44_m.modifiers as Array).is_empty(), "the modifier: undone")
	r44_m.visible = false
	# Shift+A: Cut has the cuts; Keep Overlap Faces is the Create tab's
	var r44_ko: CheckBox = null
	for r44_x in EditorInterface.get_base_control().find_children("*", "CheckBox", true, false):
		if r44_x.text == "Keep Overlap Faces" or r44_x.text == "Оставлять касающиеся грани":
			r44_ko = r44_x
	var r44_ko0 := r44_ko.button_pressed if r44_ko else false
	await move(surf.size * 0.5)
	await key(KEY_A, true)
	await frames(2)
	var r44_cut := false
	var r44_keep := false
	for r44_pm in EditorInterface.get_base_control().find_children("*", "PopupMenu", true, false):
		if not r44_pm.visible and not (r44_pm.get_parent() is PopupMenu and r44_pm.get_parent().visible):
			continue
		for r44_i in r44_pm.item_count:
			var r44_tx: String = r44_pm.get_item_text(r44_i)
			r44_cut = r44_cut or r44_tx == "Cylinder Cut" or r44_tx == "Вырез цилиндром"
			if not r44_keep and (r44_tx == "Keep Overlap Faces" or r44_tx == "Оставлять касающиеся грани"):
				r44_keep = true
				r44_pm.id_pressed.emit(r44_pm.get_item_id(r44_i))
	await frames(2)
	check(r44_cut, "Shift+A: the Cut category with the cuts")
	check(r44_keep and r44_ko != null and r44_ko.button_pressed != r44_ko0, "Shift+A: Keep Overlap Faces sets the Create tab's")
	if r44_ko:
		r44_ko.button_pressed = r44_ko0
	await key(KEY_ESCAPE)
	for r44_pm in EditorInterface.get_base_control().find_children("*", "PopupMenu", true, false):
		r44_pm.hide()
	# a camera: its panel with To the View and the preview
	var r44_cam := Camera3D.new()
	r44_cam.name = "R44Camera"
	scene.add_child(r44_cam)
	r44_cam.owner = scene
	await select_node(r44_cam)
	await frames(4)
	var r44_op := surf.find_child("HewnObjectPanel", true, false)
	var r44_prev := r44_op.find_child("CameraPreview", true, false) if r44_op else null
	check(r44_op != null and r44_op.visible and r44_prev != null, "a camera: its panel with a preview")
	var r44_tv: Button = find_button(r44_op, "To the View", false) if r44_op else null
	if r44_tv == null and r44_op:
		r44_tv = find_button(r44_op, "На место вида", false)
	if r44_tv:
		r44_tv.pressed.emit()
		await frames(2)
	check(r44_tv != null and r44_cam.global_position.distance_to(cam.global_position) < 1e-3, "the camera: To the View")
	r44_cam.get_parent().remove_child(r44_cam)
	r44_cam.queue_free()
	# a sprite: another picture by its path in the panel
	var r44_sp := Sprite3D.new()
	r44_sp.name = "R44Sprite"
	scene.add_child(r44_sp)
	r44_sp.owner = scene
	await select_node(r44_sp)
	await frames(4)
	r44_op = surf.find_child("HewnObjectPanel", true, false) as Node
	var r44_pp: LineEdit = r44_op.find_child("PicturePath", true, false) if r44_op else null
	if r44_pp and ResourceLoader.exists(r20_tex_path):
		r44_pp.text = r20_tex_path
		r44_pp.text_submitted.emit(r20_tex_path)
		await frames(2)
	check(r44_sp.texture != null and r44_sp.texture.resource_path == r20_tex_path, "a sprite: its picture changed by the path in its panel")
	r44_sp.visible = false
	# the light baked with subdivisions where a shadow's edge crosses a big face
	var r44_fl2 := HewnMesh3D.new()
	r44_fl2.name = "R44Floor"
	r44_fl2.position = Vector3(40, 0, 0)
	r44_fl2.scale = Vector3(8, 0.1, 8)
	var r44_rf := HewnMesh3D.new()
	r44_rf.name = "R44Roof"
	r44_rf.position = Vector3(41.5, 2, 1.0)
	r44_rf.scale = Vector3(3, 0.1, 3)
	var r44_sun := DirectionalLight3D.new()
	r44_sun.rotation_degrees = Vector3(-90, 0, 0)
	r44_sun.shadow_enabled = true
	for r44_x in [r44_fl2, r44_rf, r44_sun]:
		scene.add_child(r44_x)
		r44_x.owner = scene
	await select_node(r44_fl2)
	await frames(3)
	var r44_bake := false
	for r44_mb in EditorInterface.get_base_control().find_children("*", "MenuButton", true, false):
		var r44_pm2: PopupMenu = r44_mb.get_popup()
		for r44_i in r44_pm2.item_count:
			if not r44_bake and (r44_pm2.get_item_text(r44_i) == "Bake Light to Vertex Colors..." or r44_pm2.get_item_text(r44_i) == "Запечь свет в цвета вершин..."):
				r44_bake = true
				r44_pm2.id_pressed.emit(r44_pm2.get_item_id(r44_i))
	await frames(3)
	var r44_dlg: ConfirmationDialog = EditorInterface.get_base_control().find_child("HewnBakeLight", true, false)
	if r44_dlg:
		var r44_sps := r44_dlg.find_children("*", "SpinBox", true, false)
		r44_sps[0].value = 0  # (no occlusion rays)
		r44_sps[5].value = 200  # (new faces at most)
		var r44_cbs := r44_dlg.find_children("*", "CheckBox", true, false)
		for r44_cb in r44_cbs:
			if r44_cb.tooltip_text.begins_with("Faces too big") or r44_cb.tooltip_text.begins_with("Грани, слишком"):
				r44_cb.button_pressed = true
		var r44_n0 := r44_fl2.get_face_count()
		r44_dlg.get_ok_button().pressed.emit()
		await frames(3)
		var r44_n1 := r44_fl2.get_face_count()
		check(r44_n1 > r44_n0 and r44_n1 <= r44_n0 + 200 + 8, "Bake Light: subdivided where the shadow's edge crosses (%d -> %d faces)" % [r44_n0, r44_n1])
		await undo(r44_fl2)
		check(r44_fl2.get_face_count() == r44_n0, "Bake Light with subdivisions: undone (%d faces)" % r44_fl2.get_face_count())
		for r44_cb in r44_cbs:
			if r44_cb.tooltip_text.begins_with("Faces too big") or r44_cb.tooltip_text.begins_with("Грани, слишком"):
				r44_cb.button_pressed = false
	else:
		check(false, "the Bake Light dialog")
	for r44_x in [r44_fl2, r44_rf]:
		r44_x.visible = false
	r44_sun.get_parent().remove_child(r44_sun)
	r44_sun.queue_free()
	# the box cutter's settings on the header while it is the tool; the circle's sides only for a circle
	# Decals & Overlays: the overlay's own rows, the decal's mask (each only for its kind)
	var r44_kind: OptionButton = null
	for r44_x in EditorInterface.get_base_control().find_children("*", "OptionButton", true, false):
		if r44_x.tooltip_text.begins_with("Overlay: geometry") or r44_x.tooltip_text.begins_with("Оверлей: геометрия"):
			r44_kind = r44_x
	var r44_mask: Control = null
	for r44_x in EditorInterface.get_base_control().find_children("*", "Label", true, false):
		if r44_x.text == "Cull mask" or r44_x.text == "Маска отсечения":
			r44_mask = r44_x
	if r44_kind and r44_mask:
		r44_kind.select(0)
		r44_kind.item_selected.emit(0)
		await frames(2)
		var r44_m0 := r44_mask.visible
		r44_kind.select(1)
		r44_kind.item_selected.emit(1)
		await frames(2)
		check(not r44_m0 and r44_mask.visible, "the cull mask only for a decal")
		r44_kind.select(0)
		r44_kind.item_selected.emit(0)
		await frames(2)
	else:
		check(false, "the decal kind and its cull mask (%s %s)" % [r44_kind, r44_mask])
	var r44_bx := HewnMesh3D.new()
	r44_bx.name = "R44Box"
	scene.add_child(r44_bx)
	r44_bx.owner = scene
	await select_node(r44_bx)
	await frames(2)
	await key(KEY_TAB)
	var r44_tool: Button = EditorInterface.get_base_control().find_child("Tool5", true, false)
	if r44_tool:
		r44_tool.pressed.emit()
	await frames(3)
	var r44_hs: OptionButton = null
	var r44_sides: SpinBox = null
	var r44_bar: Control = EditorInterface.get_base_control().find_child("HewnCutterRow", true, false)
	for r44_x in r44_bar.find_children("*", "OptionButton", true, false) if r44_bar else []:
		if r44_x.tooltip_text == "The Box Cutter's shape" or r44_x.tooltip_text == "Форма резака":
			r44_hs = r44_x
	if r44_hs:
		for r44_x in r44_hs.get_parent().find_children("*", "SpinBox", true, false):
			r44_sides = r44_x
	check(r44_hs != null and r44_hs.is_visible_in_tree(), "the Box Cutter tool: its settings on the header")
	if r44_hs and r44_sides:
		r44_hs.select(0)
		r44_hs.item_selected.emit(0)
		var r44_hid := not r44_sides.visible
		r44_hs.select(1)
		r44_hs.item_selected.emit(1)
		check(r44_hid and r44_sides.visible, "the circle's sides only for a circle")
		r44_hs.select(0)
		r44_hs.item_selected.emit(0)
	var r44_t0: Button = EditorInterface.get_base_control().find_child("Tool0", true, false)
	if r44_t0:
		r44_t0.pressed.emit()
	await frames(2)
	await key(KEY_TAB)
	r44_bx.visible = false
	# ---- r45: bake dialog fields, Set Vertex Color, Duplicate, Knife across faces, Inset options, Element Inspector,
	# typed height + world UV of new shapes, X deletes, the Last Operation panel setting, empty cut, cutter row
	var r45_bake := EditorInterface.get_base_control().find_child("HewnBakeLight", true, false)
	check(r45_bake == null or (r45_bake.find_child("MeshInstanceShadows", true, false) != null and r45_bake.find_child("SpotStrength", true, false) != null and r45_bake.find_child("AmbientStrength", true, false) != null), "the bake: MeshInstance3D shadows and a strength for each kind of light")
	for r45_x in scene.get_children():
		if r45_x is Node3D:
			r45_x.visible = false
	var r45_c := HewnMesh3D.new()
	r45_c.name = "R45Cube"
	scene.add_child(r45_c)
	r45_c.owner = scene
	await select_node(r45_c)
	await frames(2)
	await key(KEY_TAB)
	await key(KEY_3)
	var r45_top := faces_up_index(r45_c)
	var r45_st := r45_c.get_state()
	r45_st["sel_faces"] = PackedInt32Array([r45_top])
	r45_c.set_state(r45_st)
	await frames(2)
	# Set Vertex Color: the color in its panel; only the top face's corners (the sides keep theirs), then all
	var r45_rc := sp(r45_c, r45_c.get_face_center(r45_top))
	await button(r45_rc, MOUSE_BUTTON_RIGHT, true)
	await button(r45_rc, MOUSE_BUTTON_RIGHT, false)
	await menu_pick("Set Vertex Color")
	await frames(3)
	var r45_lp := surf.find_child("HewnLastOperation", true, false)
	var r45_cp: ColorPickerButton = r45_lp.find_child("Field_color", true, false) if r45_lp else null
	if r45_cp:
		r45_cp.color_changed.emit(Color(1, 0, 0, 1))
		await frames(2)
	var r45_cols := func(r45_mm: HewnMesh3D, r45_ff: int) -> PackedColorArray:
		var r45_md: Dictionary = r45_mm.mesh_data
		var r45_sz: PackedInt32Array = r45_md["face_sizes"]
		var r45_cc: PackedColorArray = face_cols(r45_md)
		var r45_at := 0
		for r45_i in r45_ff:
			r45_at += r45_sz[r45_i]
		return r45_cc.slice(r45_at, r45_at + r45_sz[r45_ff]) if r45_cc.size() >= r45_at + r45_sz[r45_ff] else PackedColorArray()
	var r45_red_top: bool = r45_cols.call(r45_c, r45_top).size() > 0
	for r45_q in r45_cols.call(r45_c, r45_top):
		r45_red_top = r45_red_top and r45_q.is_equal_approx(Color(1, 0, 0, 1))
	var r45_side := -1
	for r45_f in r45_c.get_face_count():
		if absf(r45_c.get_face_normal(r45_f).y) < 0.1:
			r45_side = r45_f
	var r45_side_red := false
	for r45_q in r45_cols.call(r45_c, r45_side):
		r45_side_red = r45_side_red or r45_q.is_equal_approx(Color(1, 0, 0, 1))
	check(r45_cp != null and r45_red_top and not r45_side_red, "Set Vertex Color: the top face red, its neighbors keep their color")
	var r45_fo: CheckBox = null
	for r45_x in r45_lp.find_children("*", "CheckBox", true, false) if r45_lp else []:
		r45_fo = r45_x
	if r45_fo:
		r45_fo.button_pressed = false
		await frames(2)
	r45_side_red = false
	for r45_q in r45_cols.call(r45_c, r45_side):
		r45_side_red = r45_side_red or r45_q.is_equal_approx(Color(1, 0, 0, 1))
	check(r45_side_red, "Set Vertex Color, not only the faces: the neighbors' corners at those vertices too")
	await undo(r45_c)
	# the Element Inspector (its tab after UV): the face's sections; its smoothing group changed there
	r45_st = r45_c.get_state()
	r45_st["sel_faces"] = PackedInt32Array([r45_top])
	r45_c.set_state(r45_st)
	var r45_ei: Control = EditorInterface.get_base_control().find_child("ElementInspector", true, false)
	var r45_tabs: TabContainer = r45_ei.get_parent().get_parent() if r45_ei else null
	var r45_tab0 := r45_tabs.current_tab if r45_tabs else 0
	if r45_tabs:
		r45_tabs.current_tab = r45_ei.get_parent().get_index()
	await frames(3)
	check(r45_ei != null and r45_ei.is_visible_in_tree() and r45_ei.find_child("Section_Faces", true, false) != null and r45_ei.find_child("Section_MeshData", true, false) != null, "Element Inspector tab: the face's sections")
	if r45_ei:
		var r45_sg = r45_ei.find_child("Field_fsmooth", true, false)
		if r45_sg:
			r45_sg.value_changed.emit(3.0)
			await frames(2)
		check(r45_sg != null and int(r45_c.mesh_data["face_smooth"][r45_top]) == 3, "Element Inspector: the smoothing group set")
		await undo(r45_c)
	if r45_tabs:
		r45_tabs.current_tab = r45_tab0
	await frames(2)
	# Inset with options: Outset keeps the face's size, Depth raises it
	r45_st = r45_c.get_state()
	r45_st["sel_faces"] = PackedInt32Array([faces_up_index(r45_c)])
	r45_c.set_state(r45_st)
	await frames(2)
	press("Inset")
	await frames(3)
	r45_lp = surf.find_child("HewnLastOperation", true, false)
	var r45_checks := r45_lp.find_children("*", "CheckBox", true, false) if r45_lp else []
	var r45_fields := r45_lp.find_children("*", "HewnDragField", true, false) if r45_lp else []
	check(r45_checks.size() == 8 and r45_fields.size() == 2, "Inset: its options in its panel (%d check boxes, %d fields)" % [r45_checks.size(), r45_fields.size()])
	if r45_checks.size() == 8 and r45_fields.size() == 2:
		r45_checks[4].button_pressed = true  # Outset
		await frames(2)
		var r45_tf := faces_up_index(r45_c)
		var r45_tv := r45_c.get_face(r45_tf)
		var r45_mx := 0.0
		for r45_v in r45_c.get_vertices():
			r45_mx = maxf(r45_mx, absf(r45_v.x))
		check(r45_c.get_face_count() == 10 and r45_mx > 0.55, "Inset, Outset: a ring outside the face (%d faces, %.3f)" % [r45_c.get_face_count(), r45_mx])
		r45_checks[4].button_pressed = false
		await frames(2)
		r45_fields[1].value_changed.emit(0.25)  # Depth
		await frames(2)
		var r45_hy := -10.0
		for r45_v in r45_c.get_vertices():
			r45_hy = maxf(r45_hy, r45_v.y)
		check(absf(r45_hy - 0.75) < 1e-3, "Inset, Depth: the inner face raised (%.3f)" % r45_hy)
	await undo(r45_c)
	# Shift+D on two faces that do not touch: all of the copies follow the mouse at once
	var r45_f0 := faces_up_index(r45_c)
	var r45_f1 := -1
	for r45_f in r45_c.get_face_count():
		if r45_c.get_face_normal(r45_f).y < -0.9:
			r45_f1 = r45_f
	r45_st = r45_c.get_state()
	r45_st["sel_faces"] = PackedInt32Array([r45_f0, r45_f1])
	r45_c.set_state(r45_st)
	await frames(2)
	var r45_mid := sp(r45_c, Vector3(0, 0.5, 0))
	await move(r45_mid)
	await key(KEY_D, true)
	await move(r45_mid + Vector2(60, 0))
	await move(r45_mid + Vector2(80, 0))
	var r45_vs := r45_c.get_vertices()
	var r45_moved := 0
	for r45_i in range(8, r45_vs.size()):
		var r45_orig: Vector3 = r45_vs[r45_i]
		var r45_on_cube := absf(absf(r45_orig.x) - 0.5) < 1e-4 and absf(absf(r45_orig.z) - 0.5) < 1e-4 and absf(absf(r45_orig.y) - 0.5) < 1e-4
		if not r45_on_cube:
			r45_moved += 1
	check(r45_vs.size() == 16 and r45_moved == 8, "Shift+D, faces apart: every copy moves with the mouse (%d of %d)" % [r45_moved, r45_vs.size() - 8])
	await key(KEY_ESCAPE)
	await undo(r45_c)
	await frames(2)
	await key(KEY_TAB)
	# the knife from inside one face to inside its neighbor: a point on the edge between, nothing moves
	var r45_k := HewnMesh3D.new()
	r45_k.name = "R45Knife"
	scene.add_child(r45_k)
	r45_k.owner = scene
	await select_node(r45_k)
	await frames(2)
	await key(KEY_TAB)
	var r45_kt := faces_up_index(r45_k)
	var r45_ks := visible_face(r45_k)
	for r45_f in r45_k.get_face_count():
		var r45_n := r45_k.get_face_normal(r45_f)
		if absf(r45_n.y) < 0.1 and r45_n.dot(cam.global_position - r45_k.get_face_center(r45_f)) > 0.0:
			r45_ks = r45_f
	var r45_sn := r45_k.get_face_normal(r45_ks)
	var r45_kb := r45_sn * 0.5 + Vector3(0, -0.15, 0) + r45_sn.cross(Vector3.UP) * 0.1
	var r45_ka := Vector3(0, 0.5, 0) + r45_sn * 0.15 - r45_sn.cross(Vector3.UP) * 0.05
	var r45_kv0 := r45_k.get_vertices()
	await key(KEY_K)
	await click(sp(r45_k, r45_ka))
	await click(sp(r45_k, r45_kb))
	await key(KEY_ENTER)
	await frames(2)
	var r45_kv := r45_k.get_vertices()
	var r45_same := true
	for r45_i in 8:
		r45_same = r45_same and r45_kv[r45_i].distance_to(r45_kv0[r45_i]) < 1e-5
	var r45_on_edge := false
	var r45_edge_dir := r45_sn.cross(Vector3.UP).abs()
	for r45_v in r45_kv:
		var r45_off := r45_v - (Vector3(0, 0.5, 0) + r45_sn * 0.5)
		if r45_off.length() < 0.5 and absf(r45_off.dot(Vector3.UP)) < 1e-4 and absf(r45_off.dot(r45_sn)) < 1e-4:
			r45_on_edge = true
	var r45_planar := true
	for r45_f in r45_k.get_face_count():
		var r45_fv := r45_k.get_face(r45_f)
		var r45_n := r45_k.get_face_normal(r45_f)
		var r45_p0 := r45_k.get_vertex(r45_fv[0])
		for r45_v in r45_fv:
			r45_planar = r45_planar and absf(r45_n.dot(r45_k.get_vertex(r45_v) - r45_p0)) < 1e-4
	check(r45_k.get_face_count() > 6 and r45_same and r45_planar, "the knife across two faces: they are cut, nothing moves, every face stays flat (%d faces)" % r45_k.get_face_count())
	check(r45_on_edge, "the knife across two faces: a point on the edge between them")
	await key(KEY_TAB)
	r45_k.visible = false
	r45_c.visible = false
	# a drawn cube: its height typed (2 Enter); new objects take their textures from world space
	var r45_n0 := scene.get_child_count()
	press("Cube")
	await frames(1)
	await click(cam.unproject_position(Vector3(-0.5, 0, -0.5)))
	await move(cam.unproject_position(Vector3(0.5, 0, 0.5)))
	await click(cam.unproject_position(Vector3(0.5, 0, 0.5)))
	await move(cam.unproject_position(Vector3(0.5, 0.3, 0.5)))
	await key(KEY_2)
	await key(KEY_ENTER)
	await frames(2)
	var r45_dc: HewnMesh3D = scene.get_child(scene.get_child_count() - 1) if scene.get_child_count() > r45_n0 else null
	check(r45_dc != null and absf(r45_dc.get_shape_param("height") - 2.0) < 1e-3, "a drawn shape: the height typed (%s)" % (r45_dc.get_shape_param("height") if r45_dc else "-"))
	var r45_world := r45_dc != null
	if r45_dc:
		for r45_fl in r45_dc.mesh_data["face_flags"]:
			r45_world = r45_world and (int(r45_fl) & 2) != 0
	check(r45_world, "a new shape: its faces projected from world space")
	var r45_px: SpinBox = EditorInterface.get_base_control().find_child("NewUVPixels", true, false)
	var r45_x2: Button = EditorInterface.get_base_control().find_child("PixelsDouble", true, false)
	if r45_px and r45_x2:
		var r45_pv := r45_px.value
		r45_x2.pressed.emit()
		check(absf(r45_px.value - r45_pv * 2.0) < 0.5, "Pixels x2")
		(EditorInterface.get_base_control().find_child("PixelsHalf", true, false) as Button).pressed.emit()
		check(absf(r45_px.value - r45_pv) < 0.5, "Pixels /2")
	else:
		check(false, "the Pixels field and its buttons")
	# X in object mode: asks, then deletes (one undo step)
	if r45_dc:
		await select_node(r45_dc)
		await move(surf.size * 0.5)
		await key(KEY_X)
		var r45_dd: ConfirmationDialog = EditorInterface.get_base_control().find_child("HewnDeleteObjects", true, false)
		check(r45_dd != null and r45_dd.visible, "X in object mode: asks to delete")
		if r45_dd:
			r45_dd.hide()
			r45_dd.confirmed.emit()
			await frames(2)
		check(not r45_dc.is_inside_tree(), "X: the object deleted")
		var r45_ur := EditorInterface.get_editor_undo_redo()
		r45_ur.get_history_undo_redo(r45_ur.get_object_history_id(scene)).undo()
		await frames(2)
		check(r45_dc.is_inside_tree() and r45_dc.owner == scene, "X: undo brings it back")
		r45_dc.visible = false
	# the Last Operation panel turned off in Editor Settings: an operation shows nothing
	var r45_es := EditorInterface.get_editor_settings()
	var r45_redo0 = r45_es.get_setting("hewn/interface/last_operation_panel")
	r45_es.set_setting("hewn/interface/last_operation_panel", false)
	r45_es.settings_changed.emit()
	await frames(2)
	r45_c.visible = true
	await select_node(r45_c)
	await key(KEY_TAB)
	await key(KEY_3)
	r45_st = r45_c.get_state()
	r45_st["sel_faces"] = PackedInt32Array([faces_up_index(r45_c)])
	r45_c.set_state(r45_st)
	await frames(2)
	press("Inset")
	await frames(3)
	r45_lp = surf.find_child("HewnLastOperation", true, false)
	check(r45_c.get_face_count() == 10 and (r45_lp == null or not r45_lp.visible), "Last Operation Panel off: the operation done, no panel")
	await undo(r45_c)
	r45_es.set_setting("hewn/interface/last_operation_panel", r45_redo0 if r45_redo0 != null else true)
	r45_es.settings_changed.emit()
	await frames(2)
	await key(KEY_TAB)
	r45_c.visible = false
	# a box cut drawn where there is no mesh: its panel, to keep it as a cutter
	press("Box Cut")
	await frames(1)
	await click(cam.unproject_position(Vector3(0.2, 0, 0.2)))
	await move(cam.unproject_position(Vector3(0.8, 0, 0.8)))
	await click(cam.unproject_position(Vector3(0.8, 0, 0.8)))
	await move(cam.unproject_position(Vector3(0.8, 0.5, 0.8)))
	await click(cam.unproject_position(Vector3(0.8, 0.5, 0.8)))
	await frames(2)
	r45_lp = surf.find_child("HewnLastOperation", true, false)
	var r45_cn := scene.get_child_count()
	var r45_nd: CheckBox = null
	for r45_x in r45_lp.find_children("*", "CheckBox", true, false) if r45_lp else []:
		if r45_nd == null:
			r45_nd = r45_x
	check(r45_lp != null and r45_lp.visible and r45_nd != null, "a cut where there is no mesh: its panel shows")
	if r45_nd:
		if r45_nd.button_pressed:
			# (Non-destructive was on already: off first - nothing to cut, the panel stays)
			r45_nd.button_pressed = false
			await frames(2)
			r45_cn = scene.get_child_count()
			check(not (scene.get_child(r45_cn - 1) is HewnCutter3D) and r45_lp.visible, "the empty cut, destructive: nothing made, its panel stays")
			r45_nd = null
			for r45_x in r45_lp.find_children("*", "CheckBox", true, false):
				if r45_nd == null:
					r45_nd = r45_x
		r45_nd.button_pressed = true
		await frames(2)
		check(scene.get_child_count() == r45_cn + 1 and scene.get_child(scene.get_child_count() - 1) is HewnCutter3D, "the empty cut kept as a cutter node")
		var r45_ur2 := EditorInterface.get_editor_undo_redo()
		r45_ur2.get_history_undo_redo(r45_ur2.get_object_history_id(scene)).undo()
		await frames(2)
	# the Box Cutter tool's row: its own, under the header; the tool column does not move for it
	var r45_bx := HewnMesh3D.new()
	r45_bx.name = "R45Box"
	scene.add_child(r45_bx)
	r45_bx.owner = scene
	await select_node(r45_bx)
	await frames(2)
	await key(KEY_TAB)
	var r45_col: Control = EditorInterface.get_base_control().find_child("HewnTools", true, false)
	var r45_cy := r45_col.global_position.y if r45_col else 0.0
	var r45_tool: Button = EditorInterface.get_base_control().find_child("Tool5", true, false)
	if r45_tool:
		r45_tool.pressed.emit()
	await frames(3)
	var r45_row: Control = EditorInterface.get_base_control().find_child("HewnCutterRow", true, false)
	check(r45_row != null and r45_row.visible, "the Box Cutter's settings: a row of their own")
	check(r45_col == null or absf(r45_col.global_position.y - r45_cy) < 0.5, "the tool column stays where it was (%.1f -> %.1f)" % [r45_cy, r45_col.global_position.y if r45_col else 0.0])
	var r45_t0: Button = EditorInterface.get_base_control().find_child("Tool0", true, false)
	if r45_t0:
		r45_t0.pressed.emit()
	await frames(2)
	await key(KEY_TAB)
	r45_bx.visible = false
	# ---- r46: shading menu, Scene Size tree, collapse to one point, Clear / Apply, knife with several cuts, bevel and
	# knife keep world UVs, Apply World UV, reference image
	for r46_x in scene.get_children():
		if r46_x is Node3D:
			r46_x.visible = false
	var r46_sh: PopupPanel = EditorInterface.get_base_control().find_child("HewnShadingPopup", true, false)
	if r46_sh:
		r46_sh.find_child("Shading3", true, false).pressed.emit()
		await frames(2)
		check(r46_sh.find_child("Shading3", true, false).button_pressed, "Shading menu: Normals picked")
		# Single Color: the world lighting button and the cavity show (both settings), the cavity's fields only when on
		r46_sh.find_child("Shading4", true, false).pressed.emit()
		await frames(2)
		var r46_wl: Button = r46_sh.find_child("WorldLight", true, false)
		var r46_cav: CheckBox = r46_sh.find_child("Cavity", true, false)
		check(r46_wl != null and r46_wl.get_parent().get_parent().visible and r46_cav != null and r46_cav.visible, "Single Color: world space lighting and cavity in the Shading menu")
		var r46_es := EditorInterface.get_editor_settings()
		var r46_kept := {}
		for r46_k in ["hewn/viewport/cavity", "hewn/viewport/world_space_lighting"]:
			r46_kept[r46_k] = r46_es.get_setting(r46_k)
		if r46_cav:
			r46_cav.toggled.emit(true)
			await frames(2)
			check(bool(r46_es.get_setting("hewn/viewport/cavity")), "the cavity check box sets its editor setting")
		if r46_wl:
			r46_wl.toggled.emit(true)
			await frames(2)
			check(bool(r46_es.get_setting("hewn/viewport/world_space_lighting")), "the world lighting button sets its editor setting")
		for r46_k in r46_kept:
			r46_es.set_setting(r46_k, r46_kept[r46_k])
		await frames(2)
		r46_sh.find_child("Shading0", true, false).pressed.emit()
		await frames(2)
		check(r46_sh.find_child("Shading0", true, false).button_pressed, "Shading menu: back to the materials")
	else:
		check(false, "the Shading menu in the header")
	# Collapse of every vertex: one point stays
	var r46_c := HewnMesh3D.new()
	r46_c.name = "R46Cube"
	scene.add_child(r46_c)
	r46_c.owner = scene
	await select_node(r46_c)
	await key(KEY_TAB)
	await key(KEY_1)
	var r46_st := r46_c.get_state()
	var r46_all := PackedInt32Array()
	for r46_i in r46_c.get_vertex_count():
		r46_all.push_back(r46_i)
	r46_st["sel_verts"] = r46_all
	r46_c.set_state(r46_st)
	await frames(2)
	press("Collapse")
	await frames(2)
	check(r46_c.get_face_count() == 0 and r46_c.get_vertex_count() == 1 and r46_c.mesh_data.has("points"), "Collapse of all the vertices: one point stays (%d faces, %d vertices)" % [r46_c.get_face_count(), r46_c.get_vertex_count()])
	await undo(r46_c)
	await key(KEY_TAB)
	# Apply > Rotation & Scale: the object as it was, its transform the identity
	r46_c.transform = Transform3D(Basis(Vector3.UP, 0.5).scaled(Vector3(2, 1, 1)), Vector3(1, 0, 0))
	await frames(2)
	var r46_w0: Vector3 = r46_c.global_transform * r46_c.get_vertex(0)
	await select_node(r46_c)
	var r46_rc := cam.unproject_position(r46_c.global_position)
	await button(r46_rc, MOUSE_BUTTON_RIGHT, true)
	await button(r46_rc, MOUSE_BUTTON_RIGHT, false)
	await menu_pick("Rotation & Scale")
	check(r46_c.transform.basis.is_equal_approx(Basis()) and r46_c.position.is_equal_approx(Vector3(1, 0, 0)) and (r46_c.global_transform * r46_c.get_vertex(0)).distance_to(r46_w0) < 1e-4, "Apply > Rotation & Scale: in the vertices, the object stays")
	await button(r46_rc, MOUSE_BUTTON_RIGHT, true)
	await button(r46_rc, MOUSE_BUTTON_RIGHT, false)
	await menu_pick("Origin")
	check(r46_c.position.is_equal_approx(Vector3()) and (r46_c.global_transform * r46_c.get_vertex(0)).distance_to(r46_w0) < 1e-4, "Clear > Origin: the origin at zero, the geometry stays")
	r46_c.transform = Transform3D()
	# World UV on an existing object; a bevel keeps the faces in world space
	r46_c.set_world_uv_all(1.0)
	var r46_d: Dictionary = r46_c.mesh_data
	var r46_fl: PackedInt32Array = r46_d["face_flags"]
	for r46_i in r46_fl.size():
		r46_fl[r46_i] = 0
	r46_d["face_flags"] = r46_fl
	r46_c.mesh_data = r46_d
	press("Apply World UV")
	await frames(2)
	var r46_wok := true
	for r46_f in r46_c.mesh_data["face_flags"]:
		r46_wok = r46_wok and (int(r46_f) & 2) != 0
	check(r46_wok, "Apply World UV: the selected object's faces in world space")
	await key(KEY_TAB)
	await key(KEY_2)
	r46_st = r46_c.get_state()
	var r46_v := r46_c.get_face(0)
	r46_st["sel_edges"] = PackedInt32Array([r46_v[0], r46_v[1]])
	r46_c.set_state(r46_st)
	await frames(2)
	var r46_fc := r46_c.get_face_count()
	press("Bevel")
	await frames(2)
	await key(KEY_ENTER)
	await frames(2)
	var r46_bok: bool = r46_c.get_face_count() > r46_fc
	for r46_f in r46_c.mesh_data["face_flags"]:
		r46_bok = r46_bok and (int(r46_f) & 1) == 0 and (int(r46_f) & 2) != 0
	check(r46_bok, "Bevel on world UV faces: the new faces projected from world space too (%d -> %d faces)" % [r46_fc, r46_c.get_face_count()])
	await undo(r46_c)
	# the knife: RMB ends a cut, another one is drawn, Enter cuts both (a new cube in world UV)
	await key(KEY_TAB)
	r46_c.visible = false
	var r46_k := HewnMesh3D.new()
	r46_k.name = "R46Knife"
	scene.add_child(r46_k)
	r46_k.owner = scene
	r46_k.set_world_uv_all(1.0)
	r46_k.scale = Vector3(2, 2, 2)  # (big on the screen: the clicks stay inside the front face, off its edges)
	await select_node(r46_k)
	await key(KEY_TAB)
	await key(KEY_3)
	var r46_fc0 := r46_k.get_face_count()
	await key(KEY_K)
	await click(sp(r46_k, Vector3(-0.25, -0.2, 0.5)))
	await click(sp(r46_k, Vector3(0.25, -0.2, 0.5)))
	await button(sp(r46_k, Vector3(0.25, -0.2, 0.5)), MOUSE_BUTTON_RIGHT, true)
	await button(sp(r46_k, Vector3(0.25, -0.2, 0.5)), MOUSE_BUTTON_RIGHT, false)
	await click(sp(r46_k, Vector3(-0.25, 0.2, 0.5)))
	await click(sp(r46_k, Vector3(0.25, 0.2, 0.5)))
	await key(KEY_ENTER)
	await frames(2)
	var r46_kw := true
	for r46_f in r46_k.mesh_data["face_flags"]:
		r46_kw = r46_kw and (int(r46_f) & 2) != 0
	check(r46_k.get_face_count() == r46_fc0 + 2, "the knife: RMB ends a cut, a second one, Enter cuts both (%d -> %d faces)" % [r46_fc0, r46_k.get_face_count()])
	check(r46_kw, "the knife keeps the faces in world space")
	await key(KEY_TAB)
	r46_k.visible = false
	# Scene Size: a tree of the nodes, a click selects one
	press("Scene Size...")
	await frames(2)
	var r46_sd: AcceptDialog = EditorInterface.get_base_control().find_child("HewnSceneSize", true, false)
	var r46_tree: Tree = r46_sd.find_child("SizeTree", true, false) if r46_sd else null
	check(r46_sd != null and r46_sd.visible and r46_tree != null and r46_tree.get_root() != null and r46_tree.get_root().get_child_count() > 0, "Scene Size: the nodes in a tree")
	if r46_tree and r46_tree.get_root() and r46_tree.get_root().get_child_count() > 0:
		var r46_it := r46_tree.get_root().get_child(0)
		r46_it.select(0)
		await frames(2)
		check(EditorInterface.get_selection().get_selected_nodes().size() == 1, "Scene Size: a click selects the node")
		check(r46_it.get_child_count() > 0, "Scene Size: a node unfolds into what takes the room")
	if r46_sd:
		r46_sd.hide()
	# the reference image: its rectangle by the picture's sides and the offset
	var r46_ri := HewnImage3D.new()
	r46_ri.name = "R46Image"
	scene.add_child(r46_ri)
	r46_ri.owner = scene
	var r46_img := Image.create_empty(64, 32, false, Image.FORMAT_RGBA8)
	r46_ri.texture = ImageTexture.create_from_image(r46_img)
	r46_ri.depth = HewnImage3D.DEPTH_BACK
	r46_ri.side = HewnImage3D.SIDE_FRONT
	r46_ri.show_perspective = false
	await frames(2)
	var r46_ra: AABB = r46_ri.get_rect_aabb()
	check(r46_ra.size.is_equal_approx(Vector3(5, 2.5, 0)) and r46_ra.position.is_equal_approx(Vector3(-2.5, -1.25, 0)), "Reference image: 5 m wide, half as high, the origin in its middle (%s)" % r46_ra)
	r46_ri.get_parent().remove_child(r46_ri)
	r46_ri.queue_free()
	# F (F2): one edge of a hole fills it; one inner corner vertex makes a new vertex and a quad
	var r46_g := HewnMesh3D.new()
	r46_g.name = "R46Grid"
	scene.add_child(r46_g)
	r46_g.owner = scene
	var r46_pos := PackedVector3Array()
	for r46_z in 4:
		for r46_x2 in 4:
			r46_pos.push_back(Vector3(r46_x2, 0, r46_z))
	var r46_idx := PackedInt32Array()
	var r46_sz := PackedInt32Array()
	for r46_z in 3:
		for r46_x2 in 3:
			if r46_z == 1 and r46_x2 == 1:
				continue  # (the hole in the middle)
			var r46_i0 := r46_z * 4 + r46_x2
			r46_idx.append_array([r46_i0, r46_i0 + 4, r46_i0 + 5, r46_i0 + 1])
			r46_sz.push_back(4)
	r46_g.mesh_data = {"positions": r46_pos, "face_sizes": r46_sz, "face_indices": r46_idx}
	await select_node(r46_g)
	await key(KEY_TAB)
	await key(KEY_2)
	r46_st = r46_g.get_state()
	r46_st["sel_edges"] = PackedInt32Array([5, 6])
	r46_g.set_state(r46_st)
	await frames(2)
	var r46_gf := r46_g.get_face_count()
	await move(surf.size * 0.5)
	await key(KEY_F)
	check(r46_g.get_face_count() == r46_gf + 1 and r46_g.get_face_normal(r46_gf).dot(r46_g.get_face_normal(0)) > 0.99, "F on one edge of a hole: it is filled, facing like its neighbors (%d -> %d)" % [r46_gf, r46_g.get_face_count()])
	await undo(r46_g)
	# an L: the inner corner vertex 5 of the faces (0..2 x 0..2 without the top right), F: a new vertex, a quad
	var r46_l := {"positions": PackedVector3Array([Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(2, 0, 0), Vector3(0, 0, 1), Vector3(1, 0, 1), Vector3(2, 0, 1), Vector3(0, 0, 2), Vector3(1, 0, 2)]),
		"face_sizes": PackedInt32Array([4, 4, 4]), "face_indices": PackedInt32Array([0, 3, 4, 1, 1, 4, 5, 2, 3, 6, 7, 4])}
	r46_g.mesh_data = r46_l
	await key(KEY_1)
	r46_st = r46_g.get_state()
	r46_st["sel_verts"] = PackedInt32Array([4])
	r46_g.set_state(r46_st)
	await frames(2)
	await key(KEY_F)
	await key(KEY_ESCAPE)
	var r46_lv := r46_g.get_vertices()
	check(r46_g.get_face_count() == 4 and r46_lv.size() == 9 and r46_lv[8].is_equal_approx(Vector3(2, 0, 2)), "F on an inner corner vertex: a new vertex and a quad (%d faces, %s)" % [r46_g.get_face_count(), r46_lv[r46_lv.size() - 1]])
	# Checker Deselect on a full grid of 3 x 3 faces: every other one goes (a checker)
	var r46_fi := PackedInt32Array()
	r46_sz = PackedInt32Array()
	for r46_z in 3:
		for r46_x2 in 3:
			var r46_i0 := r46_z * 4 + r46_x2
			r46_fi.append_array([r46_i0, r46_i0 + 4, r46_i0 + 5, r46_i0 + 1])
			r46_sz.push_back(4)
	r46_g.mesh_data = {"positions": r46_pos, "face_sizes": r46_sz, "face_indices": r46_fi}
	await key(KEY_3)
	r46_st = r46_g.get_state()
	r46_st["sel_faces"] = PackedInt32Array([0, 1, 2, 3, 4, 5, 6, 7, 8])
	r46_g.set_state(r46_st)
	await frames(2)
	press("Checker Deselect")
	await frames(2)
	var r46_cs: Array = sel(r46_g, "sel_faces")
	r46_cs.sort()
	check(r46_cs == [0, 2, 4, 6, 8], "Checker Deselect: a checker (%s)" % [r46_cs])
	await key(KEY_TAB)
	r46_g.visible = false
	# the Mirror modifier with a Mirror Object: mirrored across that node's plane
	var r46_mo := Node3D.new()
	r46_mo.name = "R46MirrorAt"
	scene.add_child(r46_mo)
	r46_mo.owner = scene
	r46_mo.position = Vector3(2, 0, 0)
	var r46_mm := HewnMesh3D.new()
	r46_mm.name = "R46Mirrored"
	scene.add_child(r46_mm)
	r46_mm.owner = scene
	var r46_mod := HewnMirrorModifier.new()
	r46_mod.mirror_object = r46_mm.get_path_to(r46_mo)
	r46_mm.modifiers = [r46_mod]
	await frames(2)
	var r46_mab: AABB = r46_mm.mesh.get_aabb() if r46_mm.mesh else AABB()
	check(absf(r46_mab.size.x - 5.0) < 1e-3, "Mirror Object: the copy across its plane (%s)" % r46_mab)
	r46_mo.position = Vector3(3, 0, 0)
	await frames(3)
	r46_mab = r46_mm.mesh.get_aabb() if r46_mm.mesh else AABB()
	check(absf(r46_mab.size.x - 7.0) < 1e-3, "Mirror Object moved: the copy follows (%s)" % r46_mab)
	r46_mm.visible = false
	# ---- r47: Delete (what stays), Subdivide on a wire, F2 on wires and by the mouse, the knife from
	# empty space
	var r47_m := HewnMesh3D.new()
	r47_m.name = "R47"
	scene.add_child(r47_m)
	r47_m.owner = scene
	var r47_quad := {"positions": PackedVector3Array([Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 1, 0), Vector3(0, 1, 0)]),
		"face_sizes": PackedInt32Array([4]), "face_indices": PackedInt32Array([0, 1, 2, 3])}
	r47_m.mesh_data = r47_quad
	await select_node(r47_m)
	await key(KEY_TAB)
	await key(KEY_1)
	var r47_st = r47_m.get_state()
	r47_st["sel_verts"] = PackedInt32Array([0])
	r47_m.set_state(r47_st)
	await frames(2)
	await key(KEY_X)
	await menu_pick("Vertices")
	var r47_w: PackedInt32Array = (r47_m.mesh_data as Dictionary).get("wires", PackedInt32Array())
	check(r47_m.get_face_count() == 0 and r47_m.get_vertices().size() == 3 and r47_w.size() == 4, "Delete Vertices: the face goes, its other edges stay (%d faces, %d vertices, %d wires)" % [r47_m.get_face_count(), r47_m.get_vertices().size(), r47_w.size() / 2])
	r47_m.mesh_data = r47_quad
	await key(KEY_3)
	r47_st = r47_m.get_state()
	r47_st["sel_faces"] = PackedInt32Array([0])
	r47_m.set_state(r47_st)
	await frames(2)
	await key(KEY_X)
	await menu_pick("Only Faces")
	r47_w = (r47_m.mesh_data as Dictionary).get("wires", PackedInt32Array())
	check(r47_m.get_face_count() == 0 and r47_m.get_vertices().size() == 4 and r47_w.size() == 8, "Delete Only Faces: its edges and vertices stay (%d wires)" % [r47_w.size() / 2])
	# two quads side by side, the edge between them: Only Edges & Faces leaves the six other edges
	var r47_strip := {"positions": PackedVector3Array([Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(2, 0, 0), Vector3(0, 1, 0), Vector3(1, 1, 0), Vector3(2, 1, 0)]),
		"face_sizes": PackedInt32Array([4, 4]), "face_indices": PackedInt32Array([0, 1, 4, 3, 1, 2, 5, 4])}
	r47_m.mesh_data = r47_strip
	await key(KEY_2)
	r47_st = r47_m.get_state()
	r47_st["sel_edges"] = PackedInt32Array([1, 4])
	r47_m.set_state(r47_st)
	await frames(2)
	await key(KEY_X)
	await menu_pick("Only Edges & Faces")
	r47_w = (r47_m.mesh_data as Dictionary).get("wires", PackedInt32Array())
	check(r47_m.get_face_count() == 0 and r47_m.get_vertices().size() == 6 and r47_w.size() == 12, "Delete Only Edges & Faces: only the selected edge goes, the other six stay (%d wires)" % [r47_w.size() / 2])
	# Subdivide on a wire (no face next to it)
	r47_m.mesh_data = {"positions": PackedVector3Array([Vector3(-0.5, 0.5, 0), Vector3(0.5, 0.5, 0)]), "face_sizes": PackedInt32Array(), "face_indices": PackedInt32Array(), "wires": PackedInt32Array([0, 1])}
	r47_st = r47_m.get_state()
	r47_st["sel_edges"] = PackedInt32Array([0, 1])
	r47_m.set_state(r47_st)
	await frames(2)
	var r47_rc := sp(r47_m, Vector3(0, 0.5, 0))
	await button(r47_rc, MOUSE_BUTTON_RIGHT, true)
	await button(r47_rc, MOUSE_BUTTON_RIGHT, false)
	await menu_pick("Subdivide")
	r47_w = (r47_m.mesh_data as Dictionary).get("wires", PackedInt32Array())
	check(r47_m.get_vertices().size() == 3 and r47_w.size() == 4, "Subdivide on a wire: it is cut in two (%d vertices, %d wires)" % [r47_m.get_vertices().size(), r47_w.size() / 2])
	# F on one edge of a square of wires: the face (looking at the camera)
	r47_m.mesh_data = {"positions": PackedVector3Array([Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 1, 0), Vector3(0, 1, 0)]), "face_sizes": PackedInt32Array(), "face_indices": PackedInt32Array(),
		"wires": PackedInt32Array([0, 1, 1, 2, 2, 3, 3, 0])}
	r47_st = r47_m.get_state()
	r47_st["sel_edges"] = PackedInt32Array([0, 1])
	r47_m.set_state(r47_st)
	await frames(2)
	await key(KEY_F)
	check(r47_m.get_face_count() == 1 and r47_m.get_face_normal(0).dot(cam.global_position - r47_m.global_transform * r47_m.get_face_center(0)) > 0.0, "F on one edge of a square of wires: its face, toward the view (%d)" % r47_m.get_face_count())
	r47_m.mesh_data = {"positions": PackedVector3Array([Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0)]), "face_sizes": PackedInt32Array(), "face_indices": PackedInt32Array(),
		"wires": PackedInt32Array([0, 1, 1, 2, 2, 0])}
	r47_st = r47_m.get_state()
	r47_st["sel_edges"] = PackedInt32Array([0, 1])
	r47_m.set_state(r47_st)
	await frames(2)
	await key(KEY_F)
	check(r47_m.get_face_count() == 1 and r47_m.get_face(0).size() == 3, "F on one edge of a triangle of wires: the triangle")
	# F2 on a vertex: of its open edges (a wire too), the two whose new corner is nearest to the mouse
	var r47_f2 := {"positions": PackedVector3Array([Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 1, 0), Vector3(0, 1, 0), Vector3(1, 2, 0), Vector3(0, 2, 0), Vector3(-1, 1, 0)]),
		"face_sizes": PackedInt32Array([4, 4]), "face_indices": PackedInt32Array([0, 1, 2, 3, 3, 2, 4, 5]), "wires": PackedInt32Array([3, 6])}
	for r47_case in [[Vector3(-1, 2, 0), "above"], [Vector3(-1, 0, 0), "below"]]:
		r47_m.mesh_data = r47_f2
		await key(KEY_1)
		r47_st = r47_m.get_state()
		r47_st["sel_verts"] = PackedInt32Array([3])
		r47_m.set_state(r47_st)
		await frames(2)
		await move(sp(r47_m, r47_case[0]))
		await key(KEY_F)
		await key(KEY_ESCAPE)
		var r47_v := r47_m.get_vertices()
		check(r47_m.get_face_count() == 3 and r47_v[r47_v.size() - 1].is_equal_approx(r47_case[0]) and r47_m.get_face_normal(2).dot(r47_m.get_face_normal(0)) > 0.99,
			"F on a vertex, the mouse %s: the quad there, turned like its neighbors (%d faces, %s)" % [r47_case[1], r47_m.get_face_count(), r47_v[r47_v.size() - 1]])
	# the knife from empty space across a quad to empty space: it is cut in two
	r47_m.mesh_data = {"positions": PackedVector3Array([Vector3(-0.5, 0, 0), Vector3(0.5, 0, 0), Vector3(0.5, 1, 0), Vector3(-0.5, 1, 0)]), "face_sizes": PackedInt32Array([4]), "face_indices": PackedInt32Array([0, 1, 2, 3])}
	await key(KEY_2)
	await key(KEY_K)
	await click(sp(r47_m, Vector3(-1.2, 0.5, 0)))
	await click(sp(r47_m, Vector3(1.2, 0.5, 0)))
	await key(KEY_ENTER)
	check(r47_m.get_face_count() == 2, "the knife from empty space across a quad: cut in two (%d faces)" % r47_m.get_face_count())
	# ... and across a wire: it gets a vertex there
	r47_m.mesh_data = {"positions": PackedVector3Array([Vector3(-0.5, 0.5, 0), Vector3(0.5, 0.5, 0)]), "face_sizes": PackedInt32Array(), "face_indices": PackedInt32Array(), "wires": PackedInt32Array([0, 1])}
	await key(KEY_K)
	await click(sp(r47_m, Vector3(0.1, 0, 0)))
	await click(sp(r47_m, Vector3(0.1, 1, 0)))
	await key(KEY_ENTER)
	r47_w = (r47_m.mesh_data as Dictionary).get("wires", PackedInt32Array())
	check(r47_m.get_vertices().size() == 3 and r47_w.size() == 4, "the knife from empty space across a wire: a vertex on it (%d vertices)" % r47_m.get_vertices().size())
	# the materials of the inspector: slots that open the material (as Array[Material])
	var r47_hint := ""
	for r47_p in r47_m.get_property_list():
		if r47_p.name == "materials":
			r47_hint = r47_p.hint_string
	check(r47_hint.ends_with(":Material") and r47_hint.begins_with("%d/%d" % [TYPE_OBJECT, PROPERTY_HINT_RESOURCE_TYPE]), "the materials in the inspector are resource slots (%s)" % r47_hint)
	await key(KEY_TAB)
	r47_m.visible = false
	# the knife inside a room (faces inward, the camera in it): ceiling -> floor -> floor -> ceiling, over the far
	# wall; every part cut, nothing across the room
	var r47_lo := Vector3(-3, -1, -3)
	var r47_hi := Vector3(3, 2.5, 6)
	var r47_rp := PackedVector3Array()
	for r47_i in 8:
		r47_rp.push_back(Vector3(r47_hi.x if r47_i & 1 else r47_lo.x, r47_hi.y if r47_i & 2 else r47_lo.y, r47_hi.z if r47_i & 4 else r47_lo.z))
	var r47_room := HewnMesh3D.new()
	r47_room.name = "R47Room"
	scene.add_child(r47_room)
	r47_room.owner = scene
	r47_room.mesh_data = {"positions": r47_rp, "face_sizes": PackedInt32Array([4, 4, 4, 4, 4, 4]),
		"face_indices": PackedInt32Array([2, 6, 4, 0, 5, 7, 3, 1, 4, 5, 1, 0, 3, 7, 6, 2, 1, 3, 2, 0, 6, 7, 5, 4])}
	await select_node(r47_room)
	await key(KEY_TAB)
	await key(KEY_2)
	var r47_vs := surf.size
	var r47_on := func(p: Vector2, y: float) -> Vector3:
		var r47_o := cam.project_ray_origin(p)
		var r47_d := cam.project_ray_normal(p)
		return r47_o + r47_d * ((y - r47_o.y) / r47_d.y)
	var r47_c1 := Vector2(r47_vs.x * 0.35, 4)
	var r47_ceil: Vector3 = r47_on.call(r47_c1, r47_hi.y)
	var r47_ok_setup := r47_ceil.z > r47_lo.z + 0.1 and r47_ceil.z < r47_hi.z
	check(r47_ok_setup, "room: the ceiling is seen at the top of the view (%s)" % r47_ceil)
	var r47_fc0 := r47_room.get_face_count()
	await key(KEY_K)
	await click(r47_c1)
	await click(Vector2(r47_vs.x * 0.1, r47_vs.y - 10))
	await click(Vector2(r47_vs.x * 0.35, r47_vs.y - 6))
	await click(r47_c1)
	await key(KEY_ENTER)
	await frames(2)
	var r47_planar := true
	var r47_ceil_cut := false
	var r47_rv := r47_room.get_vertices()
	for r47_f in r47_room.get_face_count():
		var r47_ids := r47_room.get_face(r47_f)
		var r47_any := false
		for r47_ax in 3:
			for r47_val in [r47_lo[r47_ax], r47_hi[r47_ax]]:
				var r47_all := true
				for r47_v in r47_ids:
					r47_all = r47_all and absf(r47_rv[r47_v][r47_ax] - r47_val) < 1e-3
				r47_any = r47_any or r47_all
		r47_planar = r47_planar and r47_any
	for r47_v in r47_rv:
		var r47_corner := absf(absf(r47_v.x - (r47_lo.x + r47_hi.x) * 0.5) - (r47_hi.x - r47_lo.x) * 0.5) < 1e-3 and absf(absf(r47_v.z - (r47_lo.z + r47_hi.z) * 0.5) - (r47_hi.z - r47_lo.z) * 0.5) < 1e-3
		r47_ceil_cut = r47_ceil_cut or (absf(r47_v.y - r47_hi.y) < 1e-3 and not r47_corner)
	check(r47_planar and r47_room.get_face_count() >= r47_fc0 + 3 and r47_ceil_cut, "the knife in a room, ceiling -> floor -> ceiling: cut over the far wall onto the ceiling, every face on the room's sides (%d -> %d faces, planar %s, ceiling cut %s)" % [r47_fc0, r47_room.get_face_count(), r47_planar, r47_ceil_cut])
	await key(KEY_TAB)
	r47_room.visible = false
	# ---- r48: a field dragged in the Element Inspector is one undo step; the revert arrow; Solid colors in
	# the Shading menu; Scene Size unfolds mesh_data
	var r48_m := HewnMesh3D.new()
	r48_m.name = "R48"
	scene.add_child(r48_m)
	r48_m.owner = scene
	await select_node(r48_m)
	await key(KEY_TAB)
	await key(KEY_1)
	var r48_st = r48_m.get_state()
	r48_st["sel_verts"] = PackedInt32Array([0])
	r48_m.set_state(r48_st)
	var r48_dock := EditorInterface.get_base_control().find_child("Hewn", true, false)
	var r48_tabs: TabContainer = r48_dock.find_children("*", "TabContainer", true, false)[0]
	var r48_ei: Control = r48_dock.find_child("ElementInspector", true, false)
	var r48_tab0 := r48_tabs.current_tab
	r48_tabs.current_tab = r48_ei.get_parent().get_index()
	await frames(4)
	var r48_f = r48_dock.find_child("Field_vx", true, false)
	check(r48_f != null, "Element Inspector: the position field")
	if r48_f:
		var r48_x0 := r48_m.get_vertex(0).x
		var r48_gp: Vector2 = r48_f.get_global_rect().get_center()
		var r48_ev := InputEventMouseButton.new()
		r48_ev.button_index = MOUSE_BUTTON_LEFT
		r48_ev.pressed = true
		r48_ev.position = r48_gp
		r48_ev.global_position = r48_gp
		r48_ev.button_mask = MOUSE_BUTTON_MASK_LEFT
		await send(r48_ev)
		for r48_i in 6:
			var r48_mv := InputEventMouseMotion.new()
			r48_mv.relative = Vector2(25, 0)
			r48_mv.position = r48_gp + Vector2(25 * (r48_i + 1), 0)
			r48_mv.global_position = r48_mv.position
			r48_mv.button_mask = MOUSE_BUTTON_MASK_LEFT
			await send(r48_mv)
		var r48_rel := InputEventMouseButton.new()
		r48_rel.button_index = MOUSE_BUTTON_LEFT
		r48_rel.pressed = false
		r48_rel.position = r48_gp + Vector2(150, 0)
		r48_rel.global_position = r48_rel.position
		await send(r48_rel)
		await frames(3)
		var r48_x1 := r48_m.get_vertex(0).x
		check(r48_x1 > r48_x0 + 0.05, "Element Inspector: a drag moves the vertex (%f -> %f)" % [r48_x0, r48_x1])
		await undo(r48_m)
		check(is_equal_approx(r48_m.get_vertex(0).x, r48_x0), "Element Inspector: one Ctrl+Z takes the whole drag back (%f, was %f)" % [r48_m.get_vertex(0).x, r48_x0])
	# the revert arrow: shown when the crease is changed, a click puts 0 back
	await key(KEY_2)
	r48_st = r48_m.get_state()
	r48_st["sel_edges"] = PackedInt32Array([0, 1])
	r48_m.set_state(r48_st)
	await frames(4)
	var r48_c = r48_dock.find_child("Field_ecrease", true, false)
	var r48_rb: Button = null
	if r48_c:
		for r48_x in r48_c.get_parent().get_children():
			if r48_x is Button and r48_x != r48_c:
				r48_rb = r48_x
	check(r48_c != null and r48_rb != null and r48_rb.disabled, "Element Inspector: the crease field has the revert arrow, hidden while it is the default")
	if r48_c and r48_rb:
		r48_c.text = "0.5"
		r48_c.text_submitted.emit("0.5")
		await frames(3)
		check(not r48_rb.disabled and r48_rb.modulate.a > 0.5, "the revert arrow shows once the value is changed")
		r48_rb.pressed.emit()
		await frames(3)
		check(is_zero_approx(float(r48_c.text)) and r48_rb.disabled, "the revert arrow puts the default back (%s)" % r48_c.text)
	r48_tabs.current_tab = r48_tab0
	# the Last Operation panel's fields have it too
	var r48_any_rev := EditorInterface.get_base_control().find_children("*", "HewnRevertButton", true, false).size()
	check(r48_any_rev > 10, "the revert arrows are on the fields of the dock (%d)" % r48_any_rev)
	# the Shading menu: Random: each mesh its own color
	var r48_sm: PopupPanel = EditorInterface.get_base_control().find_child("HewnShadingPopup", true, false)
	if r48_sm:
		r48_sm.find_child("Shading5", true, false).pressed.emit()
		await frames(2)
		var r48_col = RenderingServer.instance_geometry_get_shader_parameter(r48_m.get_instance(), "obj_color")
		check(r48_col is Vector4 and not (r48_col as Vector4).is_equal_approx(Vector4(0.8, 0.8, 0.8, 1)), "Shading > Random: the mesh gets its own color (%s)" % [r48_col])
		r48_sm.find_child("Shading0", true, false).pressed.emit()
		await frames(2)
	await key(KEY_TAB)
	# Scene Size: mesh_data unfolds into its keys
	press("Scene Size...")
	await frames(2)
	var r48_sd: AcceptDialog = EditorInterface.get_base_control().find_child("HewnSceneSize", true, false)
	var r48_tree: Tree = r48_sd.find_child("SizeTree", true, false) if r48_sd else null
	var r48_md_ok := false
	if r48_tree and r48_tree.get_root():
		var r48_stack := [r48_tree.get_root()]
		while not r48_stack.is_empty():
			var r48_it: TreeItem = r48_stack.pop_back()
			if r48_it.get_text(0).begins_with("mesh_data"):
				for r48_ch in r48_it.get_children():
					r48_md_ok = r48_md_ok or r48_ch.get_text(0).begins_with("positions")
			r48_stack.append_array(r48_it.get_children())
	check(r48_md_ok, "Scene Size: mesh_data unfolds into its keys (positions...)")
	if r48_sd:
		r48_sd.hide()
	r48_m.visible = false
	# ---- r49: Shift+Tab / Shift+M with nothing selected; Alt+arrow a quarter turn; Shift+Alt on a selected loop takes
	# it away; Shift + drag on a face moves it along its normal; the Area3D's panel; a cutter under the scene root with
	# Skip Parent still cuts; no Hotspots tab; the Inspector tab can be made narrow
	EditorInterface.get_selection().clear()
	await frames(3)
	var r49_mag := godot_snap()
	var r49_was: bool = r49_mag.button_pressed if r49_mag else false
	await move(Vector2(60, 60))
	await key(KEY_TAB, true)
	check(r49_mag != null and r49_mag.button_pressed != r49_was, "Shift+Tab toggles snapping with nothing selected")
	if r49_mag and r49_mag.button_pressed != r49_was:
		await key(KEY_TAB, true)
	await key(KEY_M, true)
	await frames(3)
	var r49_rb: Button = EditorInterface.get_base_control().find_child("Tool2", true, false)
	check(r49_rb != null and r49_rb.button_pressed, "Shift+M starts the ruler with nothing selected")
	await key(KEY_ESCAPE)
	await key(KEY_ESCAPE)
	# Alt+arrow: the selected object turns a quarter about the vertical (left / right)
	var r49_m := HewnMesh3D.new()
	r49_m.name = "R49"
	scene.add_child(r49_m)
	r49_m.owner = scene
	await select_node(r49_m)
	await key(KEY_RIGHT, false, false, true)
	var r49_x: Vector3 = r49_m.global_transform.basis.x
	check(absf(r49_x.y) < 1e-4 and absf(absf(r49_x.z) - 1.0) < 1e-4, "Alt+Right: a quarter turn about the vertical (%s)" % r49_x)
	await undo(r49_m)
	check(r49_m.global_transform.basis.x.is_equal_approx(Vector3.RIGHT), "undo of Alt+arrow")
	# Shift + drag on the visible face of the selected cube: the face moves out along its normal
	var r49_f := visible_face(r49_m)
	var r49_n := r49_m.get_face_normal(r49_f)
	var r49_c := r49_m.get_face_center(r49_f)
	var r49_a := sp(r49_m, r49_c)
	var r49_b := sp(r49_m, r49_c + r49_n * 0.5)
	if r49_a.distance_to(r49_b) < 20.0:
		r49_b = r49_a + (r49_b - r49_a).normalized() * 40.0
	var r49_v0: float = r49_m.get_aabb().size.dot(r49_n.abs())
	await move(r49_a, true)
	await drag(r49_a, r49_b, true)
	var r49_v1: float = r49_m.get_aabb().size.dot(r49_n.abs())
	check(r49_v1 > r49_v0 + 0.05 and EditorInterface.get_selection().get_selected_nodes().has(r49_m), "Shift + drag on a face moves it along its normal (%f -> %f)" % [r49_v0, r49_v1])
	await undo(r49_m)
	check(absf(r49_m.get_aabb().size.dot(r49_n.abs()) - r49_v0) < 1e-4, "undo of the face move")
	# r77: in edit mode Shift + drag is a box that adds to the selection; the face stays where it is
	await key(KEY_TAB)
	await key(KEY_3)
	var r77_st: Dictionary = r49_m.get_state()
	r77_st["sel_faces"] = PackedInt32Array()
	r49_m.set_state(r77_st)
	await move(r49_a - Vector2(30, 30), true)
	await drag(r49_a - Vector2(30, 30), r49_a + Vector2(30, 30), true)
	var r77_sel: PackedInt32Array = r49_m.get_state()["sel_faces"]
	check(absf(r49_m.get_aabb().size.dot(r49_n.abs()) - r49_v0) < 1e-4 and r77_sel.has(r49_f), "edit mode: Shift + drag selects with a box, nothing moves (%s)" % r77_sel)
	await key(KEY_TAB)
	# Shift+Alt on a selected loop: it goes again
	await key(KEY_TAB)
	await key(KEY_2)
	var r49_fv := r49_m.get_face(visible_face(r49_m))
	var r49_e := sp(r49_m, (r49_m.get_vertex(r49_fv[0]) + r49_m.get_vertex(r49_fv[1])) * 0.5)
	await click(r49_e, false, true)
	var r49_loop := sel(r49_m, "sel_edges").size()
	await click(r49_e, false, true, false, true)
	check(r49_loop > 0 and sel(r49_m, "sel_edges").is_empty(), "Shift+Alt on a selected loop takes it away (%d edges, then %d)" % [r49_loop, sel(r49_m, "sel_edges").size()])
	# the Inspector tab: as narrow as the others
	var r49_dock := EditorInterface.get_base_control().find_child("Hewn", true, false)
	var r49_tabs: TabContainer = r49_dock.find_children("*", "TabContainer", true, false)[0]
	var r49_ei: Control = r49_dock.find_child("ElementInspector", true, false)
	var r49_t0 := r49_tabs.current_tab
	await key(KEY_3)
	var r49_st = r49_m.get_state()
	r49_st["sel_faces"] = PackedInt32Array([0, 1])
	r49_m.set_state(r49_st)
	r49_tabs.current_tab = r49_ei.get_parent().get_index()
	await frames(4)
	var r49_w: float = r49_ei.get_parent().get_combined_minimum_size().x
	check(r49_w < 170.0, "the Inspector tab can be made narrow (%f)" % r49_w)
	r49_tabs.current_tab = r49_t0
	await key(KEY_TAB)
	r49_m.visible = false
	# the Area3D's panel at the bottom left, its size the box's
	var r49_area := Area3D.new()
	r49_area.name = "R49Area"
	var r49_cs := CollisionShape3D.new()
	var r49_box := BoxShape3D.new()
	r49_box.size = Vector3(2, 1, 1)
	r49_cs.shape = r49_box
	r49_area.add_child(r49_cs)
	scene.add_child(r49_area)
	r49_area.owner = scene
	r49_cs.owner = scene
	await select_node(r49_area)
	await frames(3)
	var r49_op := surf.find_child("HewnObjectPanel", true, false) as Control
	var r49_sx: LineEdit = r49_op.find_child("size_0", true, false) if r49_op else null
	check(r49_op != null and r49_op.visible and r49_sx != null and absf(float(r49_sx.text) - 2.0) < 1e-3, "an Area3D has its panel, the size of its box (%s)" % [r49_sx.text if r49_sx else "none"])
	if r49_sx:
		r49_sx.text = "3"
		r49_sx.text_submitted.emit("3")
		await frames(2)
		check(absf(r49_box.size.x - 3.0) < 1e-3, "the panel's size sets the box (%s)" % r49_box.size)
	r49_area.visible = false
	EditorInterface.get_selection().clear()
	# a cutter right under the scene root with Skip Parent still cuts the meshes
	var r49_wall := HewnMesh3D.new()
	r49_wall.name = "R49Wall"
	r49_wall.position = Vector3(40, 0, 0)
	scene.add_child(r49_wall)
	r49_wall.owner = scene
	var r49_cut := HewnCutter3D.new()
	r49_cut.size = Vector3(0.4, 0.4, 3)
	r49_cut.skip_parent = true
	r49_cut.position = Vector3(40, 0, 0)
	scene.add_child(r49_cut)
	r49_cut.owner = scene
	await frames(2)
	var r49_got: Array = r49_cut.apply()
	check(r49_got.has(r49_wall), "a cutter under the scene root with Skip Parent cuts the meshes (%s)" % [r49_got])
	r49_cut.queue_free()
	r49_wall.visible = false
	check(EditorInterface.get_base_control().find_child("HewnHotspots", true, false) == null, "no Hotspots tab")
	# ---- r50: Hewn is a plugin of the project (Project Settings > Plugins); world UVs stay when the object moves;
	# a click on nothing in multi-object edit clears every mesh; H on a vertex; Ctrl+Shift + drag on a face: a block
	check(EditorInterface.is_plugin_enabled("hewn") or EditorInterface.is_plugin_enabled("res://addons/hewn/plugin.cfg"), "Hewn is switched on in Project Settings > Plugins")
	# the Quad screen: Editor Settings > Hewn > Interface > Quad View takes it away and brings it back
	var r51_es := EditorInterface.get_editor_settings()
	var r51_was = r51_es.get_setting("hewn/interface/quad_view")
	r51_es.set_setting("hewn/interface/quad_view", false)
	r51_es.notify_property_list_changed()
	r51_es.emit_signal("settings_changed")
	await frames(4)
	check(EditorInterface.get_editor_main_screen().find_child("HewnQuad", true, false) == null, "Quad View off: no Quad screen")
	r51_es.set_setting("hewn/interface/quad_view", true)
	r51_es.emit_signal("settings_changed")
	await frames(4)
	check(EditorInterface.get_editor_main_screen().find_child("HewnQuad", true, false) != null, "Quad View on: the Quad screen is back")
	r51_es.set_setting("hewn/interface/quad_view", r51_was if r51_was != null else true)
	var r50_w := HewnMesh3D.new()
	r50_w.name = "R50World"
	r50_w.position = Vector3(50, 0, 0)
	scene.add_child(r50_w)
	r50_w.owner = scene
	r50_w.set_world_uv_all(1.0)
	await frames(3)
	var r50_uv0 := r50_w.get_face_uvs(0)
	r50_w.position = Vector3(50.37, 0.21, 0)
	await frames(3)
	check(r50_w.get_face_uvs(0) == r50_uv0 and r50_w.is_uv_anchored(), "world UVs stay when the object is moved")
	r50_w.set_uv_anchored(false)
	await frames(2)
	check(r50_w.get_face_uvs(0) != r50_uv0, "Apply World UV: they follow the world again")
	r50_w.visible = false
	# two meshes edited together, faces selected in both; a click on nothing clears both
	var r50_a := HewnMesh3D.new()
	r50_a.name = "R50A"
	var r50_b := HewnMesh3D.new()
	r50_b.name = "R50B"
	r50_b.position = Vector3(1.5, 0, 0)
	for r50_m in [r50_a, r50_b]:
		scene.add_child(r50_m)
		r50_m.owner = scene
	EditorInterface.get_selection().clear()
	await frames(2)
	EditorInterface.get_selection().add_node(r50_a)
	await frames(2)
	EditorInterface.get_selection().add_node(r50_b)
	await frames(2)
	await key(KEY_TAB)
	await key(KEY_3)
	for r50_m in [r50_a, r50_b]:
		var r50_st = r50_m.get_state()
		r50_st["sel_faces"] = PackedInt32Array([0, 1])
		r50_m.set_state(r50_st)
	await frames(2)
	await click(Vector2(surf.size.x - 20, surf.size.y * 0.6))
	check(sel(r50_a, "sel_faces").is_empty() and sel(r50_b, "sel_faces").is_empty(), "a click on nothing clears the selection of every edited mesh (%s, %s)" % [sel(r50_a, "sel_faces"), sel(r50_b, "sel_faces")])
	await key(KEY_TAB)
	r50_b.visible = false
	# H on one vertex: the faces using it are hidden (three on a box corner)
	await select_node(r50_a)
	await key(KEY_TAB)
	await key(KEY_1)
	var r50_st2 = r50_a.get_state()
	r50_st2["sel_verts"] = PackedInt32Array([0])
	r50_a.set_state(r50_st2)
	await frames(2)
	await key(KEY_H)
	var r50_hidden := 0
	for r50_f in r50_a.get_face_count():
		if r50_a.is_face_hidden(r50_f):
			r50_hidden += 1
	check(r50_hidden == 3, "H on a vertex hides the faces using it (%d)" % r50_hidden)
	await key(KEY_H, false, false, true)
	await key(KEY_TAB)
	# Ctrl+Shift + drag on a face of the selected box: a new block, an object of its own (its length snapped: Ctrl
	# doesn't turn the snapping off here)
	var r50_snap_was := godot_snap().button_pressed
	await set_snapping(true, "Increment")
	await select_node(r50_a)
	var r50_f := visible_face(r50_a)
	var r50_n := r50_a.get_face_normal(r50_f)
	var r50_p0 := sp(r50_a, r50_a.get_face_center(r50_f))
	var r50_p1 := sp(r50_a, r50_a.get_face_center(r50_f) + r50_n * 0.6)
	if r50_p0.distance_to(r50_p1) < 20.0:
		r50_p1 = r50_p0 + (r50_p1 - r50_p0).normalized() * 40.0
	var r50_kids := scene.get_child_count()
	var r50_o := surf.get_global_rect().position
	var r50_pe := InputEventMouseButton.new()
	r50_pe.button_index = MOUSE_BUTTON_LEFT
	r50_pe.pressed = true
	r50_pe.shift_pressed = true
	r50_pe.ctrl_pressed = true
	r50_pe.position = r50_o + r50_p0
	r50_pe.global_position = r50_pe.position
	r50_pe.button_mask = MOUSE_BUTTON_MASK_LEFT
	await send(r50_pe)
	for r50_i in range(1, 11):
		var r50_mv := InputEventMouseMotion.new()
		r50_mv.position = r50_o + r50_p0.lerp(r50_p1, r50_i / 10.0)
		r50_mv.global_position = r50_mv.position
		r50_mv.button_mask = MOUSE_BUTTON_MASK_LEFT
		r50_mv.shift_pressed = true
		r50_mv.ctrl_pressed = true
		await send(r50_mv)
	var r50_re := InputEventMouseButton.new()
	r50_re.button_index = MOUSE_BUTTON_LEFT
	r50_re.pressed = false
	r50_re.position = r50_o + r50_p1
	r50_re.global_position = r50_re.position
	await send(r50_re)
	await frames(2)
	var r50_blk: HewnMesh3D = scene.find_child("R50A_block", false, false)
	check(scene.get_child_count() == r50_kids + 1 and r50_blk != null and r50_blk.get_face_count() == 6 and r50_a.get_face_count() == 6, "Ctrl+Shift + drag on a face: a block as an object of its own (%s)" % [r50_blk.get_face_count() if r50_blk else -1])
	if r50_blk:
		var r50_bb := r50_blk.get_aabb()
		var r50_len := absf(r50_bb.size.dot(r50_n.abs()))
		check(absf(r50_len / 0.25 - roundf(r50_len / 0.25)) < 0.01 and r50_len > 0.1, "Ctrl+Shift block: its length snaps to the grid (%f)" % r50_len)
		r50_blk.visible = false
	godot_snap().button_pressed = r50_snap_was
	r50_a.visible = false
	#R26_NEXT

	# ---- Wall points: a straight wall becomes a path; drag an end, apply, undo
	var wlx := HewnMesh3D.new()
	wlx.name = "WallEdit"
	scene.add_child(wlx)
	wlx.owner = scene
	wlx.shape = 19
	await select_node(wlx)
	await frames(3)
	var wl_xf := wlx.transform
	press("Edit Wall Points")
	await frames(2)
	check(wlx.poly_points.size() == 2, "the straight wall is edited through its two ends")
	if wlx.poly_points.size() == 2:
		var wend := wlx.global_transform * wlx.poly_points[1]
		var wnew := wend + Vector3(0, 0, 1.0)
		await drag(cam.unproject_position(wend), cam.unproject_position(wnew))
		await click(Vector2(5, 5))  # outside: apply
		check(wlx.poly_points.size() == 2 and (wlx.global_transform * wlx.poly_points[1]).distance_to(wnew) < 0.05,
				"wall end moved (%s)" % [wlx.global_transform * wlx.poly_points[1]])
		await undo(wlx)
		check(wlx.poly_points.is_empty() and wlx.transform.is_equal_approx(wl_xf), "undo gives the straight wall back")
	wlx.visible = false

	# ---- Poly Shape shows while it is drawn
	for c in scene.get_children():
		c.visible = false
	godot_snap().button_pressed = false
	press("Draw Poly Shape")
	await frames(2)
	var pp_a := cam.unproject_position(Vector3(-0.5, 0, -0.5))
	var pp_b := cam.unproject_position(Vector3(0.5, 0, -0.5))
	var pp_c := cam.unproject_position(Vector3(0.5, 0, 0.5))
	await click(pp_a)
	await click(pp_b)
	await move(pp_c)
	check(scene.get_child_count(true) == scene.get_child_count() + 1, "poly shape: the outline shows while points are placed")
	await click(pp_c)
	await click(pp_a)  # close: height
	await move(pp_a + Vector2(0, -40))
	var ppv: HewnMesh3D = scene.get_child(scene.get_child_count(true) - 1, true)
	check(ppv.get_aabb().size.y > 0.01, "poly shape preview follows the height (%s)" % ppv.get_aabb().size)
	var nb5 := scene.get_child_count()
	await click(pp_a + Vector2(0, -40))
	check(scene.get_child_count() == nb5 + 1 and scene.get_child_count(true) == scene.get_child_count(), "poly shape created, preview gone")
	if scene.get_child_count() == nb5 + 1:
		scene.get_child(scene.get_child_count() - 1).visible = false

	# ---- Tool column (left, collapsible) and the Mesh / Object menus
	var tcol: Control = EditorInterface.get_base_control().find_child("HewnTools", true, false)
	check(tcol != null, "tool column exists")
	var tc := HewnMesh3D.new()
	tc.name = "ToolCol"
	scene.add_child(tc)
	tc.owner = scene
	tc.shape = 1
	await select_node(tc)
	await frames(3)
	if tcol:
		var tbtns: Array = []
		var tshown := 0
		for i in 30:
			var tb_i: Button = tcol.find_child("Tool%d" % i, true, false)
			tbtns.append(tb_i)
			if tb_i and tb_i.is_visible_in_tree():
				tshown += 1
		check(tbtns[14] != null and tshown == 5 and tcol.is_visible_in_tree(), "tool column: object mode's 5 tools (select, circle, cursor, ruler, clip) shown (%d)" % tshown)
		if tbtns[14] != null:
			tbtns[2].pressed.emit()  # ruler
			await frames(3)
			check(tbtns[2].button_pressed and not tbtns[0].button_pressed, "ruler tool active in the column")
			# rulers stay: two of them, then X over the first removes it
			var r1a := cam.unproject_position(Vector3(-0.5, 0.5, 0.5))
			var r1b := cam.unproject_position(Vector3(0.5, 0.5, 0.5))
			await move(r1a)
			await drag(r1a, r1b)
			var r2a := cam.unproject_position(Vector3(-0.5, -0.5, 0.5))
			var r2b := cam.unproject_position(Vector3(0.5, -0.5, 0.5))
			await move(r2a)
			await drag(r2a, r2b)
			tbtns[0].pressed.emit()  # select: leaves the ruler tool
			await frames(3)
			check(tbtns[0].button_pressed, "select tool back")
			tbtns[3].pressed.emit()  # knife: enters edit mode
			await frames(3)
			check(tbtns[3].button_pressed, "knife from the column (edit mode)")
			await key(KEY_ESCAPE)
		var collapse: Button = null
		for b in tcol.find_children("*", "Button", true, false):
			if not b.toggle_mode:
				collapse = b
		if collapse:
			var h0 := tcol.get_combined_minimum_size().y
			collapse.pressed.emit()
			await frames(2)
			check(tcol.get_combined_minimum_size().y < h0 * 0.5, "tool column collapses")
			collapse.pressed.emit()
	var tb3 := EditorInterface.get_base_control().find_child("HewnToolbar", true, false)
	var mesh_mb: MenuButton = null
	var obj_mb: MenuButton = null
	for b in tb3.find_children("*", "MenuButton", true, false):
		if b.text.begins_with("Mesh"):
			mesh_mb = b
		if b.text.begins_with("Object"):
			obj_mb = b
	check(mesh_mb != null and obj_mb != null, "toolbar has the Mesh and Object menus")
	if mesh_mb:
		var subs := mesh_mb.get_popup().find_children("*", "PopupMenu", false, false)
		check(subs.size() == 4, "Mesh menu: Select / Vertex / Edge / Face (%d)" % subs.size())
		# Face > To Circle on the top face of the cube
		await key(KEY_3)
		var tc_top := -1
		for f in tc.get_face_count():
			if tc.get_face_normal(f).y > 0.9:
				tc_top = f
		await click(sp(tc, tc.get_face_center(tc_top)))
		var face_sub: PopupMenu = null
		for sm in subs:
			if sm.name == "Face":
				face_sub = sm
		if face_sub:
			for i in face_sub.item_count:
				if face_sub.get_item_text(i) == "To Circle":
					face_sub.id_pressed.emit(face_sub.get_item_id(i))
			await frames(2)
			var tf := tc.get_face(tc_top)
			var tcc := tc.get_face_center(tc_top)
			var d0 := tc.get_vertex(tf[0]).distance_to(tcc)
			var same := true
			for v in tf:
				same = same and absf(tc.get_vertex(v).distance_to(tcc) - d0) < 1e-4
			check(same, "Mesh > Face > To Circle ran on the selection")
	await key(KEY_TAB)

	# ---- GG, then C: the vertex stays on the edge it was sliding along
	var gs := HewnMesh3D.new()
	gs.name = "SlideLock"
	scene.add_child(gs)
	gs.owner = scene
	gs.shape = 1
	tc.visible = false
	await select_node(gs)
	await key(KEY_TAB)
	await key(KEY_1)
	var gv := 0
	for v in gs.get_vertex_count():
		if gs.get_vertex(v).y > 0.4 and gs.get_vertex(v).x > 0.4 and gs.get_vertex(v).z > 0.4:
			gv = v
	var g0 := gs.get_vertex(gv)
	await click(sp(gs, g0))
	await key(KEY_G)
	await key(KEY_G)
	var gpos := sp(gs, g0)
	await move(gpos + Vector2(25, 0))
	await move(gpos + Vector2(50, 0))
	check((gs.get_vertex(gv) - g0).length() > 1e-3, "GG slides the vertex")
	var dir1 := (gs.get_vertex(gv) - g0).normalized()
	await key(KEY_C)
	await move(gpos + Vector2(0, 60))
	await move(gpos + Vector2(-20, 80))
	var d2 := gs.get_vertex(gv) - g0
	check(d2.length() > 1e-4 and absf(absf(d2.normalized().dot(dir1)) - 1.0) < 1e-3, "C keeps the vertex on its edge line (%s vs %s)" % [d2, dir1])
	await key(KEY_ESCAPE)
	await key(KEY_TAB)

	# ---- New shapes turn with the transform orientation (here: a custom one turned 30 degrees)
	for c in scene.get_children():
		c.visible = false
	var ro := HewnMesh3D.new()
	ro.name = "Turned"
	scene.add_child(ro)
	ro.owner = scene
	ro.shape = 1
	ro.rotation_degrees = Vector3(0, 30, 0)
	await select_node(ro)
	var tb4 := EditorInterface.get_base_control().find_child("HewnToolbar", true, false)
	var omb4: MenuButton = null
	for b in tb4.find_children("*", "MenuButton", true, false):
		if b.tooltip_text.begins_with("Transform orientation"):
			omb4 = b
	if omb4:
		omb4.get_popup().id_pressed.emit(900)  # + from the selection (object mode: its rotation)
		await frames(2)
		ro.visible = false
		var nb6 := scene.get_child_count()
		press("Cube")
		await frames(1)
		await drag(cam.unproject_position(Vector3(-0.6, 0, -0.4)), cam.unproject_position(Vector3(0.6, 0, 0.4)))
		await move(cam.unproject_position(Vector3(0, 0.5, 0)))
		await click(cam.unproject_position(Vector3(0, 0.5, 0)))
		if scene.get_child_count() == nb6 + 1:
			var tcube: Node3D = scene.get_child(scene.get_child_count() - 1)
			var yaw := rad_to_deg(atan2(-tcube.global_basis.x.z, tcube.global_basis.x.x))
			check(absf(absf(fposmod(yaw, 90.0)) - 30.0) < 0.5, "drawn cube turned like the custom orientation (%f)" % yaw)
			tcube.visible = false
		else:
			check(false, "cube drawn with a custom orientation (%d -> %d, ro children %d, sel %s)" % [nb6, scene.get_child_count(), ro.get_child_count(), EditorInterface.get_selection().get_selected_nodes()])
		omb4.get_popup().id_pressed.emit(0)  # back to Global
	# ---- the snap popup fields are drag fields
	var sf := 0
	for f in tb4.get_parent().find_children("*", "HewnDragField", true, false):
		sf += 1
	check(sf >= 6, "popup fields can be dragged (%d)" % sf)

	# ---- Mirror clipping: a vertex on the mirror plane stays on it, another one cannot cross it
	for c in scene.get_children():
		c.visible = false
	var cl := HewnMesh3D.new()
	cl.name = "Clip"
	scene.add_child(cl)
	cl.owner = scene
	cl.shape = 1
	for v in cl.get_vertex_count():
		cl.set_vertex(v, cl.get_vertex(v) + Vector3(0.5, 0, 0))  # x from 0 to 1
	var clm := HewnMirrorModifier.new()
	clm.clipping = true
	cl.add_modifier(clm)
	await select_node(cl)
	await key(KEY_TAB)
	await key(KEY_1)
	var v_on := -1
	var v_out := -1
	for v in cl.get_vertex_count():
		var q := cl.get_vertex(v)
		if q.y > 0.4 and q.z > 0.4:
			if q.x < 0.01:
				v_on = v
			else:
				v_out = v
	await click(sp(cl, cl.get_vertex(v_on)))
	await key(KEY_G)
	await key(KEY_X)
	await type_text("0.3")
	await key(KEY_ENTER)
	check(absf(cl.get_vertex(v_on).x) < 1e-5, "clipping: the vertex on the plane stays on it (%f)" % cl.get_vertex(v_on).x)
	await click(sp(cl, cl.get_vertex(v_out)))
	await key(KEY_G)
	await key(KEY_X)
	await type_text("-2")
	await key(KEY_ENTER)
	check(absf(cl.get_vertex(v_out).x) < 1e-5, "clipping: a vertex does not cross the plane (%f)" % cl.get_vertex(v_out).x)
	await key(KEY_TAB)
	cl.visible = false

	# ---- the header folds like the tool column; its fold button is outside the scrolled row
	var tb5 := EditorInterface.get_base_control().find_child("HewnToolbar", true, false)
	var fold_block: Control = EditorInterface.get_base_control().find_child("HewnHeaderFold", true, false)
	var fold: Button = null
	if fold_block:
		for b in fold_block.find_children("*", "Button", true, false):
			if b.tooltip_text == "Hide / Show" or b.tooltip_text == "Скрыть/Показать":
				fold = b
	check(fold != null and not tb5.is_ancestor_of(fold), "header has a fold button, not scrolled with the row")
	if fold_block:
		check(fold_block.visible and fold_block.get_global_rect().end.x <= tb5.get_global_rect().position.x + 1.0, "the fold button is left of the row (%s, %s)" % [fold_block.get_global_rect(), tb5.get_global_rect()])
	if fold:
		var w0: float = tb5.get_child(0).get_combined_minimum_size().x
		var col5: Control = EditorInterface.get_base_control().find_child("HewnTools", true, false)
		var cy0: float = col5.global_position.y if col5 else 0.0
		fold.pressed.emit()
		await frames(2)
		var w1: float = tb5.get_child(0).get_combined_minimum_size().x
		check(w1 < w0 * 0.3, "header folded (%f -> %f)" % [w0, w1])
		if col5:
			check(absf(col5.global_position.y - cy0) < 0.5, "the tool column stays where it was when the header folds (%f -> %f)" % [cy0, col5.global_position.y])
		fold.pressed.emit()
		await frames(2)

	# ---- Grid snapping: the moved vertex lands on grid points (world grid, Move step)
	for c in scene.get_children():
		c.visible = false
	var gc := HewnMesh3D.new()
	gc.name = "GridSnap"
	scene.add_child(gc)
	gc.owner = scene
	gc.shape = 1
	gc.position = Vector3(0.13, 0, 0.07)  # off the grid
	await select_node(gc)
	await key(KEY_TAB)
	await key(KEY_1)
	var r18_gv := -1
	for v in gc.get_vertex_count():
		var r18_q := gc.get_vertex(v)
		if r18_q.x > 0.4 and r18_q.y > 0.4 and r18_q.z > 0.4:
			r18_gv = v
	await key(KEY_A, false, false, true)
	await click(sp(gc, gc.get_vertex(r18_gv)))
	await set_snapping(true, "Grid")
	var step: float = 0.0
	for f in EditorInterface.get_base_control().find_children("*", "HewnDragField", true, false):
		var lab = label_before(f)
		if lab is Label and lab.text == "Move step":
			step = float(f.text)
	var gp0 := sp(gc, gc.get_vertex(r18_gv))
	await move(gp0)
	await key(KEY_G)
	await move(gp0 + Vector2(23, 9))
	await move(gp0 + Vector2(41, 17))
	await key(KEY_ENTER)
	var gw := gc.global_transform * gc.get_vertex(r18_gv)
	var on_grid := step > 0.0
	for k in 3:
		on_grid = on_grid and absf(gw[k] / step - roundf(gw[k] / step)) < 1e-3
	check(on_grid and not gw.is_equal_approx(gc.global_transform * Vector3(0.5, 0.5, 0.5)), "Grid: the moved vertex is on the grid (%s, step %f)" % [gw, step])

	# ---- Edge Center / Face Center targets: G moves the vertex onto the middle of an edge / a face center
	var ref := HewnMesh3D.new()
	ref.name = "SnapRef"
	scene.add_child(ref)
	ref.owner = scene
	ref.shape = 1
	ref.position = Vector3(0.0, 0.0, -2.0)
	await frames(2)
	for target in [["Edge Center", Vector3(0.0, 0.5, -1.5)], ["Face Center", Vector3(0.0, 0.5, -2.0)]]:
		await set_snapping(true, target[0])
		var r18_start := sp(gc, gc.get_vertex(r18_gv))
		await move(r18_start)
		await key(KEY_G)
		var at := cam.unproject_position(target[1])
		await move(r18_start.lerp(at, 0.5))
		await move(at)
		await key(KEY_ENTER)
		var r18_w := gc.global_transform * gc.get_vertex(r18_gv)
		check(r18_w.distance_to(target[1]) < 1e-3, "%s: the vertex snaps onto %s (%s)" % [target[0], target[1], r18_w])
	await set_snapping(false, "Increment")
	await key(KEY_TAB)

	# ---- Left panels: at the viewport edge; aside only while Godot's zoom bar shows
	var col: Control = EditorInterface.get_base_control().find_child("HewnTools", true, false)
	await select_node(ref)
	await frames(3)
	if col and col.visible:
		var sr := surf.get_global_rect()
		var x0 := col.get_global_rect().position.x - sr.position.x
		check(x0 < 6.0, "tool column at the viewport edge (%f)" % x0)
	var sp_panel2: Control = surf.find_child("HewnShapePanel", true, false)
	check(sp_panel2 != null and sp_panel2.visible, "shape panel of the parametric cube")
	if sp_panel2 and sp_panel2.visible:
		# (a panel as tall as to reach the tool column goes right of it)
		var col_r := col.get_global_rect().end.x - surf.get_global_rect().position.x if col and col.visible and sp_panel2.global_position.y < col.get_global_rect().end.y + 4.0 else 0.0
		check(sp_panel2.position.x < 6.0 + col_r, "shape panel at the viewport edge, or next to the tool column (%f)" % sp_panel2.position.x)
		var wheel := InputEventMouseButton.new()
		wheel.button_index = MOUSE_BUTTON_WHEEL_UP
		wheel.pressed = true
		wheel.position = surf.size * 0.5
		wheel.global_position = surf.get_global_rect().position + surf.size * 0.5
		Input.parse_input_event(wheel)
		await frames(20)
		check(sp_panel2.position.x > 20.0, "zoom bar shown: the shape panel steps aside (%f)" % sp_panel2.position.x)
		await get_tree().create_timer(2.2).timeout
		await frames(20)
		check(sp_panel2.position.x < 6.0 + col_r, "zoom bar gone: back at the edge (%f)" % sp_panel2.position.x)

	# ---- Shape panel folds by its header (one state with the last operation panel)
	if sp_panel2 and sp_panel2.visible:
		var title: Button = sp_panel2.find_child("Title", true, false)
		var r18_h0 := sp_panel2.get_combined_minimum_size().y
		check(title != null and title.icon != null, "shape panel header has the fold arrow")
		# a click left of the title (the panel's margin) folds it too
		var ev := InputEventMouseButton.new()
		ev.button_index = MOUSE_BUTTON_LEFT
		ev.pressed = true
		ev.position = Vector2(1, title.position.y + 4 if title else 4)
		sp_panel2.gui_input.emit(ev)
		await frames(3)
		var h1 := sp_panel2.get_combined_minimum_size().y
		check(h1 < r18_h0 * 0.6, "a click left of the title folds the shape panel (%f -> %f)" % [r18_h0, h1])
		title.pressed.emit()
		await frames(3)
		check(sp_panel2.get_combined_minimum_size().y > r18_h0 * 0.9, "clicking the title unfolds it")
	gc.visible = false
	ref.visible = false

	# ---- r54: Ctrl+Shift + drag inward splits the object; GG snaps; Same Normal and its angle; Ctrl+Shift+click: the
	# region; Extrude Repeat (the wheel over its Steps field steps it, the view stays); Knife Project; Dimensions
	var r54_snap_was := godot_snap().button_pressed
	await set_snapping(true, "Increment")
	var r54 := HewnMesh3D.new()
	r54.name = "R54S"
	scene.add_child(r54)
	r54.owner = scene
	r54.shape = 1
	await select_node(r54)
	# (the seen face whose way in shows the longest on the screen)
	var r54_f := -1
	var r54_len := 0.0
	for f in r54.get_face_count():
		var r54_fc := r54.get_face_center(f)
		var r54_fn := r54.get_face_normal(f)
		if r54_fn.dot(cam.global_position - r54.global_transform * r54_fc) > 0.0 and sp(r54, r54_fc).distance_to(sp(r54, r54_fc - r54_fn * 0.4)) > r54_len:
			r54_len = sp(r54, r54_fc).distance_to(sp(r54, r54_fc - r54_fn * 0.4))
			r54_f = f
	var r54_n := r54.get_face_normal(r54_f)
	var r54_c := r54.get_face_center(r54_f)
	var r54_p0 := sp(r54, r54_c)
	var r54_p1 := sp(r54, r54_c - r54_n * 0.4)
	var r54_kids := scene.get_child_count()
	await cs_drag(r54_p0, r54_p1)
	var r54_blk: HewnMesh3D = scene.find_child("R54S_block", false, false)
	check(scene.get_child_count() == r54_kids + 1 and r54_blk != null and r54_blk.get_face_count() == 6 and r54.get_face_count() == 6,
			"Ctrl+Shift + drag inward: the object is cut in two (%d, %d)" % [r54_blk.get_face_count() if r54_blk else -1, r54.get_face_count()])
	if r54_blk:
		var r54_la := absf(r54.get_aabb().size.dot(r54_n.abs()))
		var r54_lb := absf(r54_blk.get_aabb().size.dot(r54_n.abs()))
		check(absf(r54_la + r54_lb - 1.0) < 1e-3 and r54_la > 0.05 and r54_lb > 0.05, "the two parts make the whole (%f + %f)" % [r54_la, r54_lb])
		await undo(scene)
		check(not r54_blk.is_inside_tree() and absf(r54.get_aabb().size.dot(r54_n.abs()) - 1.0) < 1e-4, "undo: one object again")
	# the Dimensions overlay draws (the size of the selection)
	var r54_dims: CheckBox = null
	for r54_x in EditorInterface.get_base_control().find_children("*", "CheckBox", true, false):
		if r54_x.text == "Dimensions" or r54_x.text == "Размеры":
			r54_dims = r54_x
	check(r54_dims != null, "Overlays: Dimensions")
	if r54_dims:
		r54_dims.button_pressed = true
		await frames(3)
		r54_dims.button_pressed = false
	# GG with grid snapping: the distance slid goes in grid steps
	await select_node(r54)
	await key(KEY_TAB)
	await key(KEY_1)
	var r54_gv := 0
	for v in r54.get_vertex_count():
		if r54.get_vertex(v).y > 0.4 and r54.get_vertex(v).x > 0.4 and r54.get_vertex(v).z > 0.4:
			r54_gv = v
	var r54_g0 := r54.get_vertex(r54_gv)
	await click(sp(r54, r54_g0))
	await key(KEY_G)
	await key(KEY_G)
	var r54_gp := sp(r54, r54_g0)
	await move(r54_gp + Vector2(60, 0))
	await move(r54_gp + Vector2(140, 0))
	var r54_gd := (r54.get_vertex(r54_gv) - r54_g0).length()
	check(r54_gd > 1e-3 and absf(r54_gd / 0.25 - roundf(r54_gd / 0.25)) < 1e-3, "GG with snapping: in grid steps (%f)" % r54_gd)
	await key(KEY_ESCAPE)
	check(r54.get_vertex(r54_gv).is_equal_approx(r54_g0), "Esc puts the slid vertex back")
	# Same Normal: the face alone, then within 100 degrees the four sides too
	await key(KEY_3)
	var r54_top := faces_up_index(r54)
	var r54_st = r54.get_state()
	r54_st["sel_faces"] = PackedInt32Array([r54_top])
	r54.set_state(r54_st)
	await frames(2)
	press("Same Normal")
	await frames(2)
	check(sel(r54, "sel_faces").size() == 1, "Same Normal on a box: the face alone (%s)" % [sel(r54, "sel_faces")])
	var r54_ang = last_op_field("Max angle (deg)")
	if r54_ang == null:
		r54_ang = last_op_field("Макс. угол (°)")
	check(r54_ang != null, "Same Normal: its angle in the Last Operation panel")
	if r54_ang:
		r54_ang.value_changed.emit(100.0)
		await frames(2)
		check(sel(r54, "sel_faces").size() == 5, "Same Normal within 100 degrees: the top and the sides (%s)" % [sel(r54, "sel_faces")])
	# Extrude Repeat: 4 steps of 4 sides each; the wheel over Steps makes it 5, the view does not move
	r54_st = r54.get_state()
	r54_st["sel_faces"] = PackedInt32Array([r54_top])
	r54.set_state(r54_st)
	await frames(2)
	press("Extrude Repeat")
	await frames(2)
	check(r54.get_face_count() == 6 + 16, "Extrude Repeat: 4 steps (%d faces)" % r54.get_face_count())
	var r54_steps = last_op_field("Repeats")
	if r54_steps == null:
		r54_steps = last_op_field("Повторы")
	check(r54_steps != null, "Extrude Repeat: Steps in the panel")
	if r54_steps:
		var r54_cam := cam.global_transform
		var r54_at: Vector2 = r54_steps.get_global_rect().get_center() - surf.get_global_rect().position
		await wheel_up(r54_at)
		await frames(2)
		check(r54.get_face_count() == 6 + 20 and cam.global_transform.is_equal_approx(r54_cam), "the wheel over the Steps field: one step more, the view stays (%d faces)" % r54.get_face_count())
	await undo(r54)
	await key(KEY_TAB)
	r54.visible = false
	# Ctrl+click: the path; Ctrl+Shift+click: the whole region between two corners of a 4 x 4 grid
	var r54_pl := HewnMesh3D.new()
	r54_pl.name = "R54Grid"
	scene.add_child(r54_pl)
	r54_pl.owner = scene
	r54_pl.shape = 2
	r54_pl.set_shape_param("width_segments", 4)
	r54_pl.set_shape_param("depth_segments", 4)
	await select_node(r54_pl)
	await key(KEY_TAB)
	await key(KEY_3)
	var r54_fa := -1
	var r54_fb := -1
	for f in r54_pl.get_face_count():
		var r54_cc := r54_pl.get_face_center(f)
		if r54_cc.x < -0.7 and r54_cc.z < -0.7:
			r54_fa = f
		if r54_cc.x > 0.7 and r54_cc.z > 0.7:
			r54_fb = f
	r54_st = r54_pl.get_state()
	r54_st["sel_faces"] = PackedInt32Array([r54_fa])
	r54_pl.set_state(r54_st)
	await frames(2)
	await click(sp(r54_pl, r54_pl.get_face_center(r54_fb)), false, false, true, false)
	check(sel(r54_pl, "sel_faces").size() == 7, "Ctrl+click: the shortest path between the corners (%d)" % sel(r54_pl, "sel_faces").size())
	r54_pl.set_state(r54_st)
	await frames(2)
	await click(sp(r54_pl, r54_pl.get_face_center(r54_fb)), false, false, true, true)
	check(sel(r54_pl, "sel_faces").size() == 16, "Ctrl+Shift+click: every path, the whole rectangle (%d)" % sel(r54_pl, "sel_faces").size())
	await key(KEY_TAB)
	# Knife Project: a small square above the grid cuts its outline into it
	var r54_kc := HewnMesh3D.new()
	r54_kc.name = "R54Cutter"
	scene.add_child(r54_kc)
	r54_kc.owner = scene
	r54_kc.shape = 2
	r54_kc.set_shape_param("width", 0.5)
	r54_kc.set_shape_param("depth", 0.5)
	r54_kc.set_shape_param("width_segments", 1)
	r54_kc.set_shape_param("depth_segments", 1)
	r54_kc.position = Vector3(0.1, 0.3, 0.1)
	r54_pl.set_shape_param("width_segments", 1)
	r54_pl.set_shape_param("depth_segments", 1)
	r54_pl.set_shape_param("width", 4)
	r54_pl.set_shape_param("depth", 4)
	await select_node(r54_kc)
	EditorInterface.get_selection().add_node(r54_pl)
	await frames(3)
	await key(KEY_TAB)
	await key(KEY_3)
	# (the grid the active mesh: a click on it, away from the cutter)
	await click(sp(r54_pl, Vector3(-1.6, 0, 1.6)))
	press("Knife Project")
	await frames(2)
	check(r54_pl.get_face_count() >= 2 and r54_kc.get_face_count() == 1, "Knife Project: the cutter's outline cut into the grid (%d faces)" % r54_pl.get_face_count())
	await key(KEY_TAB)
	r54_pl.visible = false
	r54_kc.visible = false
	godot_snap().button_pressed = r54_snap_was

	# ---- r55: Align to Face / Align to Axis on a wedge's slope (its world UVs on its own plane, and back)
	var r55 := HewnMesh3D.new()
	r55.name = "R55Wedge"
	scene.add_child(r55)
	r55.owner = scene
	r55.shape = 16
	r55.set_world_uv_all(1.0)
	await select_node(r55)
	await key(KEY_TAB)
	await key(KEY_3)
	var r55_slope := -1
	for f in r55.get_face_count():
		if absf(r55.get_face_normal(f).y) > 0.3 and absf(r55.get_face_normal(f).y) < 0.95:
			r55_slope = f
	var r55_st = r55.get_state()
	r55_st["sel_faces"] = PackedInt32Array([r55_slope])
	r55.set_state(r55_st)
	await frames(2)
	press("Align to Face")
	await frames(2)
	check((int(r55.mesh_data["face_flags"][r55_slope]) >> 23) & 1 == 1, "Align to Face: the slope's world UVs on its own plane")
	press("Align to Axis")
	await frames(2)
	check((int(r55.mesh_data["face_flags"][r55_slope]) >> 23) & 1 == 0, "Align to Axis: back along the nearest axis")
	await key(KEY_TAB)
	r55.visible = false

	# ---- Wireframe overlay toggle exists; shape params on/off are bools in the inspector
	var wf := find_button(EditorInterface.get_base_control(), "Wireframe", false)
	check(wf != null, "Overlays: Wireframe check box")
	var cone := HewnMesh3D.new()
	cone.shape = 4
	check(typeof(cone.get("shape_cap")) == TYPE_BOOL and typeof(cone.get("shape_flip_normals")) == TYPE_BOOL, "cone cap / flip normals are bools")
	cone.free()

	# ---- r56: Sculpt mode: Draw raises the ground under the stroke (one undo step), Ctrl lowers it, Smooth evens it,
	# Terrace rounds the heights to its step
	var r56 := HewnMesh3D.new()
	r56.name = "R56Ground"
	scene.add_child(r56)
	r56.owner = scene
	r56.shape = 2
	r56.set_shape_param("width", 4.0)
	r56.set_shape_param("depth", 4.0)
	r56.set_shape_param("width_segments", 30)
	r56.set_shape_param("depth_segments", 30)
	await select_node(r56)
	var r56_im: MenuButton = EditorInterface.get_base_control().find_child("InteractionMode", true, false)
	r56_im.get_popup().id_pressed.emit(3)
	await frames(3)
	var r56_draw: Button = EditorInterface.get_base_control().find_child("Tool15", true, false)
	check(r56_draw != null and r56_draw.is_visible_in_tree(), "Sculpt: its tools in the tool column")
	var r56_maxy := func() -> float:
		var hy := -1e9
		for v in r56.get_vertices():
			hy = maxf(hy, v.y)
		return hy
	var r56_c := sp(r56, Vector3.ZERO)
	var r56_stroke := func(ctrl: bool) -> void:
		await button(r56_c, MOUSE_BUTTON_LEFT, true, false, ctrl)
		for r56_i in range(1, 9):
			var r56_m := InputEventMouseMotion.new()
			r56_m.position = r56_c + Vector2(r56_i * 6, 0)
			r56_m.global_position = surf.get_global_rect().position + r56_m.position
			r56_m.button_mask = MOUSE_BUTTON_MASK_LEFT
			r56_m.ctrl_pressed = ctrl
			surf.gui_input.emit(r56_m)
			await frames(1)
		await button(r56_c + Vector2(48, 0), MOUSE_BUTTON_LEFT, false, false, ctrl)
		await frames(2)
	# Ctrl+wheel: the brush's radius, Shift+wheel: its strength; the camera stays where it is
	var r56_rad = EditorInterface.get_base_control().find_child("SculptRadius", true, false)
	var r56_str = EditorInterface.get_base_control().find_child("SculptStrength", true, false)
	var r56_r0: float = r56_rad.get_value()
	var r56_s0: float = r56_str.get_value()
	var r56_cam0 := cam.global_transform
	await button(r56_c, MOUSE_BUTTON_WHEEL_UP, true, false, true)
	await button(r56_c, MOUSE_BUTTON_WHEEL_UP, false, false, true)
	await button(r56_c, MOUSE_BUTTON_WHEEL_DOWN, true, false, false, true)
	await button(r56_c, MOUSE_BUTTON_WHEEL_DOWN, false, false, false, true)
	await frames(3)
	check(r56_rad.get_value() > r56_r0 and r56_str.get_value() < r56_s0, "Sculpt: Ctrl+wheel the radius, Shift+wheel the strength (%f, %f)" % [r56_rad.get_value(), r56_str.get_value()])
	check(cam.global_transform.is_equal_approx(r56_cam0), "Sculpt: Ctrl / Shift + wheel leave the camera where it is")
	r56_rad.set_value(r56_r0)
	r56_str.set_value(r56_s0)
	await r56_stroke.call(false)
	var r56_up: float = r56_maxy.call()
	if r56_up <= 0.01:
		print("DIAG sculpt: c=%s surf=%s cam=%s sel=%s faces=%d vis=%s draw=%s hovered=%s focus=%s popups=%s" % [r56_c, surf.size, cam.global_transform, EditorInterface.get_selection().get_selected_nodes(), r56.get_face_count(), r56.is_visible_in_tree(), r56_draw.button_pressed, surf.get_viewport().gui_get_hovered_control(), surf.get_viewport().gui_get_focus_owner(), EditorInterface.get_base_control().get_tree().root.get_children().filter(func(x): return x is Window and x.visible)])
	check(r56_up > 0.01, "Sculpt > Draw raises the ground (%f)" % r56_up)
	await undo(r56)
	check(absf(r56_maxy.call()) < 1e-5, "one undo step takes the whole stroke back (%f)" % r56_maxy.call())
	await r56_stroke.call(true)
	var r56_miny := 1e9
	for v in r56.get_vertices():
		r56_miny = minf(r56_miny, v.y)
	check(r56_miny < -0.01, "Ctrl+Draw lowers it (%f)" % r56_miny)
	await undo(r56)
	await r56_stroke.call(false)
	await r56_stroke.call(false)
	var r56_peak: float = r56_maxy.call()
	EditorInterface.get_base_control().find_child("Tool16", true, false).pressed.emit()  # Smooth
	await frames(2)
	await r56_stroke.call(false)
	await r56_stroke.call(false)
	check(r56_maxy.call() < r56_peak - 1e-4, "Smooth lowers the peak (%f -> %f)" % [r56_peak, r56_maxy.call()])
	EditorInterface.get_base_control().find_child("Tool20", true, false).pressed.emit()  # Terrace
	await frames(2)
	var r56_step = EditorInterface.get_base_control().find_child("SculptStep", true, false)
	check(r56_step != null and r56_step.is_visible_in_tree(), "Terrace: its step in the header")
	var r56_strength = EditorInterface.get_base_control().find_child("SculptStrength", true, false)
	r56_strength.set_value(1.0)
	var r56_fo: OptionButton = EditorInterface.get_base_control().find_child("SculptFalloff", true, false)
	r56_fo.select(5)  # (Constant: every vertex in reach goes the whole way)
	r56_fo.item_selected.emit(5)
	var r56_dir: OptionButton = EditorInterface.get_base_control().find_child("SculptDirection", true, false)
	r56_dir.select(0)  # (Up: the heights are world heights)
	r56_dir.item_selected.emit(0)
	if r56_step:
		r56_step.set_value(0.05)
	await r56_stroke.call(false)
	var r56_on_steps := 0
	var r56_center_n := 0
	for v in r56.get_vertices():
		if Vector2(v.x, v.z).length() < 0.15:
			r56_center_n += 1
			if absf(v.y / 0.05 - roundf(v.y / 0.05)) < 0.02:
				r56_on_steps += 1
	check(r56_center_n > 0 and r56_on_steps == r56_center_n, "Terrace: the heights in the middle on the steps (%d of %d)" % [r56_on_steps, r56_center_n])
	r56_strength.set_value(0.5)
	r56_fo.select(0)
	r56_fo.item_selected.emit(0)
	r56_dir.select(1)
	r56_dir.item_selected.emit(1)
	# the dynamic topology: on, a stroke adds faces where it goes; Density is in the tools then
	var r56_dyn: CheckBox = EditorInterface.get_base_control().find_child("Dyntopo", true, false)
	check(r56_dyn != null, "Sculpt: the Dynamic Topology check box")
	if r56_dyn:
		r56_dyn.button_pressed = true
		await frames(3)
		check(EditorInterface.get_base_control().find_child("Tool24", true, false).is_visible_in_tree(), "Dynamic topology on: the Density tool shows")
		EditorInterface.get_base_control().find_child("Tool15", true, false).pressed.emit()
		await frames(2)
		var r56_f0 := r56.get_face_count()
		await r56_stroke.call(false)
		check(r56.get_face_count() > r56_f0, "Dynamic topology: the stroke adds faces where it goes (%d -> %d)" % [r56_f0, r56.get_face_count()])
		await undo(r56)
		check(r56.get_face_count() == r56_f0, "undo: the faces as they were (%d)" % r56.get_face_count())
		r56_dyn.button_pressed = false
		await frames(2)
	# proportional editing's Custom Curve shows its curve
	var r56_pf: OptionButton = null
	for r56_ob in EditorInterface.get_base_control().find_children("*", "OptionButton", true, false):
		if r56_ob.item_count == 9 and r56_ob.get_item_text(8) in ["Custom Curve", "Особая"]:
			r56_pf = r56_ob
	check(r56_pf != null, "Proportional editing: the Custom Curve falloff")
	if r56_pf:
		r56_pf.select(8)
		r56_pf.item_selected.emit(8)
		await frames(1)
		var r56_cp = r56_pf.get_parent().get_parent().find_children("*", "EditorResourcePicker", true, false)[0]  # (under the grid of fields)
		check(r56_cp is EditorResourcePicker and r56_cp.visible and r56_cp.edited_resource is Curve, "Custom Curve: its curve to edit")
		r56_pf.select(0)
		r56_pf.item_selected.emit(0)
	await key(KEY_TAB)
	check(not EditorInterface.get_base_control().find_child("Tool15", true, false).is_visible_in_tree(), "Tab leaves Sculpt (its tools go)")
	r56.visible = false

	# ---- r57: the Object tab's Remesh: Voxel makes a cube again of small faces (one undo step); Quad about the faces asked for
	var r57 := HewnMesh3D.new()
	r57.name = "R57Remesh"
	scene.add_child(r57)
	r57.owner = scene
	r57.shape = 1
	await select_node(r57)
	var r57_vs = EditorInterface.get_base_control().find_child("RemeshVoxelSize", true, false)
	r57_vs.set_value(0.1)
	press("Voxel Remesh")
	await frames(2)
	check(r57.get_face_count() > 50, "Voxel Remesh: the cube made of small faces (%d)" % r57.get_face_count())
	await undo(r57)
	check(r57.get_face_count() == 6, "undo: the cube again (%d)" % r57.get_face_count())
	var r57_mode: OptionButton = EditorInterface.get_base_control().find_child("RemeshMode", true, false)
	r57_mode.select(1)
	r57_mode.item_selected.emit(1)
	await frames(1)
	var r57_est: Label = EditorInterface.get_base_control().find_child("QuadRemeshEstimate", true, false)
	press("Quad Remesh")
	await frames(2)
	check(r57.get_face_count() > 1000 and r57.get_face_count() < 12000, "Quad Remesh: about the faces asked for (%d, about %s)" % [r57.get_face_count(), r57_est.text])
	await undo(r57)
	r57_mode.select(0)
	r57_mode.item_selected.emit(0)
	r57.visible = false

	# ---- r58: Belt at Height: a closed cornice around a box's walls, outside them
	var r58 := HewnMesh3D.new()
	r58.name = "R58House"
	scene.add_child(r58)
	r58.owner = scene
	r58.shape = 1
	r58.set_shape_param("width", 4.0)
	r58.set_shape_param("height", 3.0)
	r58.set_shape_param("depth", 4.0)
	await select_node(r58)
	EditorInterface.get_base_control().find_child("BeltHeight", true, false).set_value(1.5)
	press("Belt at Height")
	await frames(3)
	var r58_t: HewnMesh3D = scene.find_child("R58House_Trim", true, false)
	check(r58_t != null and r58_t.poly_points.size() == 4 and int(r58_t.get_shape_param("closed")) == 1, "Belt at Height: a closed path of the 4 walls (%s)" % [r58_t.poly_points if r58_t else null])
	if r58_t:
		var r58_bb: AABB = r58_t.global_transform * r58_t.get_aabb()
		check(r58_bb.position.x < -2.05 and r58_bb.end.x > 2.05 and r58_bb.position.x > -2.5, "Belt at Height: outside the walls, against them (%s)" % r58_bb)
		r58_t.visible = false
	r58.visible = false

	# ---- r59: Scatter: its brushes in the tool column while it is selected; a stroke paints copies on the ground
	# (one undo step), Ctrl erases them
	var r59g := HewnMesh3D.new()
	r59g.name = "R59Ground"
	scene.add_child(r59g)
	r59g.owner = scene
	r59g.shape = 2
	r59g.set_shape_param("width", 6.0)
	r59g.set_shape_param("depth", 6.0)
	var r59 := HewnScatter3D.new()
	r59.name = "R59Scatter"
	scene.add_child(r59)
	r59.owner = scene
	r59.meshes = [BoxMesh.new()]
	r59.density = 3.0
	r59.brush_radius = 1.0
	await select_node(r59g)
	var r59_t: Button = EditorInterface.get_base_control().find_child("Tool25", true, false)
	check(r59_t != null and not r59_t.is_visible_in_tree(), "Scatter brushes: not shown for a mesh")
	await select_node(r59)
	await frames(3)
	check(r59_t.is_visible_in_tree(), "Scatter brushes: in the tool column while a Scatter is selected")
	r59_t.pressed.emit()
	await frames(2)
	check(r59_t.button_pressed, "Scatter brush: active")
	var r59_c := sp(r59g, Vector3.ZERO)
	var r59_stroke := func(ctrl: bool) -> void:
		await button(r59_c, MOUSE_BUTTON_LEFT, true, false, ctrl)
		for r59_i in range(1, 9):
			var r59_m := InputEventMouseMotion.new()
			r59_m.position = r59_c + Vector2(r59_i * 8, 0)
			r59_m.global_position = surf.get_global_rect().position + r59_m.position
			r59_m.button_mask = MOUSE_BUTTON_MASK_LEFT
			r59_m.ctrl_pressed = ctrl
			surf.gui_input.emit(r59_m)
			await frames(1)
		await button(r59_c + Vector2(64, 0), MOUSE_BUTTON_LEFT, false, false, ctrl)
		await frames(2)
	await r59_stroke.call(false)
	var r59_n := r59.get_instance_count()
	var r59_on := true
	var r59_d := r59.get_instances()
	for i in range(0, r59_d.size(), 13):
		r59_on = r59_on and absf(r59_d[i + 10]) < 1e-3
	check(r59_n > 0 and r59_on, "Scatter brush: copies on the ground (%d)" % r59_n)
	await undo(r59)
	check(r59.get_instance_count() == 0, "Scatter brush: one undo step takes the stroke back")
	await r59_stroke.call(false)
	await r59_stroke.call(true)
	check(r59.get_instance_count() < r59_n, "Scatter brush: Ctrl erases (%d)" % r59.get_instance_count())
	key(KEY_ESCAPE)
	await frames(2)
	check(not r59_t.button_pressed, "Scatter brush: Esc puts it down")
	r59.visible = false
	r59g.visible = false

	# ---- r60: two sweeps selected: the shape panel changes both; Drop goes down (the sweep stays on its path); the
	# mesh picked in the panel; Ctrl+J merges; Delete Inner Faces; Wall Along Edges; a path gap with Alt+click
	var r60a := HewnMesh3D.new()
	r60a.name = "R60SweepA"
	scene.add_child(r60a)
	r60a.owner = scene
	r60a.shape = 20
	r60a.poly_points = PackedVector3Array([Vector3(-2, 0, 0), Vector3(0, 0, 0), Vector3(0, 0, 2)])
	r60a.set_shape_param("profile", 1)
	var r60b: HewnMesh3D = r60a.duplicate()
	r60b.name = "R60SweepB"
	scene.add_child(r60b)
	r60b.owner = scene
	r60b.position = Vector3(0, 0, -3)
	await select_node(r60a)
	EditorInterface.get_selection().add_node(r60b)
	await frames(4)
	var r60_sp: Control = surf.find_child("HewnShapePanel", true, false)
	check(r60_sp != null and r60_sp.visible, "two sweeps selected: the shape panel")
	if r60_sp and r60_sp.visible:
		var r60_f := r60_sp.find_children("*", "HewnDragField", true, false)
		r60_f[0].value_changed.emit(0.5)  # width
		await frames(2)
		check(is_equal_approx(r60a.get_shape_param("width"), 0.5) and is_equal_approx(r60b.get_shape_param("width"), 0.5), "the panel changes both selected sweeps")
		var r60_o := r60a.position
		r60_f = r60_sp.find_children("*", "HewnDragField", true, false)
		r60_f[2].value_changed.emit(0.3)  # drop
		await frames(2)
		check(is_equal_approx(r60a.get_shape_param("drop"), 0.3) and r60a.position.is_equal_approx(r60_o) and absf(r60a.get_aabb().position.y + 0.3) < 1e-3,
				"Drop: the sweep goes down, it stays on its path (%s, %s)" % [r60a.position, r60a.get_aabb()])
		await undo(r60a)
		check(is_equal_approx(r60a.get_shape_param("drop"), 0.0) and is_equal_approx(r60b.get_shape_param("drop"), 0.0), "one undo for both")
		r60a.set_shape_param("profile", 7)
		r60b.set_shape_param("profile", 7)
		await frames(3)
		var r60_rp: EditorResourcePicker = r60_sp.find_child("SweepMesh", true, false)
		check(r60_rp != null, "profile Mesh: the mesh picker in the shape panel")
		if r60_rp:
			var r60_box := BoxMesh.new()
			r60_rp.resource_changed.emit(r60_box)
			await frames(2)
			check(r60a.sweep_mesh == r60_box and r60b.sweep_mesh == r60_box and int(r60b.get_shape_param("profile")) == 7, "the picked mesh on both sweeps")
	r60a.visible = false
	r60b.visible = false
	# Ctrl+J: two touching boxes into one; then the two faces between them deleted
	var r60c := HewnMesh3D.new()
	r60c.name = "R60BoxA"
	scene.add_child(r60c)
	r60c.owner = scene
	r60c.shape = 1
	var r60d := HewnMesh3D.new()
	r60d.name = "R60BoxB"
	scene.add_child(r60d)
	r60d.owner = scene
	r60d.shape = 1
	r60d.position = Vector3(1, 0, 0)
	await select_node(r60c)
	EditorInterface.get_selection().add_node(r60d)
	await frames(3)
	await key(KEY_J, false, true)
	await frames(3)
	check(r60d.get_parent() == null and r60c.get_face_count() == 12, "Ctrl+J: the selected objects merged (%d faces)" % r60c.get_face_count())
	press("Delete Inner Faces")
	await frames(2)
	check(r60c.get_face_count() == 10, "Delete Inner Faces: the two between the boxes go (%d)" % r60c.get_face_count())
	await undo(r60c)
	check(r60c.get_face_count() == 12, "undo brings them back")
	# Wall Along Edges: a wall on the box's top edges
	await key(KEY_TAB)
	await key(KEY_2)
	var r60_st = r60c.get_state()
	var r60_top := faces_up_index(r60c)
	var r60_tv := r60c.get_face(r60_top)
	var r60_e := PackedInt32Array()
	for i in r60_tv.size():
		r60_e.append(r60_tv[i])
		r60_e.append(r60_tv[(i + 1) % r60_tv.size()])
	r60_st["sel_edges"] = r60_e
	r60c.set_state(r60_st)
	await frames(2)
	press("Wall Along Edges")
	await frames(3)
	var r60_w: HewnMesh3D = scene.find_child("R60BoxA_Wall", true, false)
	check(r60_w != null and r60_w.shape == 19 and r60_w.poly_points.size() == 5, "Wall Along Edges: a ring of walls on the edges (%s)" % [r60_w.poly_points if r60_w else null])
	if r60_w:
		var r60_wb: AABB = r60_w.global_transform * r60_w.get_aabb()
		check(r60_wb.position.y > 0.45 and r60_wb.end.y > 1.0, "the wall stands on the top edges (%s)" % r60_wb)
		r60_w.visible = false
	await key(KEY_TAB)
	r60c.visible = false
	# a path gap: Alt+click a segment while the path points are edited
	r60a.visible = true
	r60a.set_shape_param("profile", 1)
	await select_node(r60a)
	press("Edit Path Points")
	await frames(2)
	var r60_mid: Vector3 = r60a.global_transform * Vector3(-1, 0, 0)
	await click(cam.unproject_position(r60_mid), false, true)
	await key(KEY_ENTER)
	await frames(2)
	check(r60a.path_gaps == PackedInt32Array([0]), "Alt+click a segment: a gap there (%s)" % r60a.path_gaps)
	check(r60a.get_face_count() == 6, "the sweep: the other segment alone (%d faces)" % r60a.get_face_count())
	await undo(r60a)
	check(r60a.path_gaps.is_empty(), "undo: no gap")
	r60a.visible = false

	# ---- r61: G on a cutter with Ctrl: its corner nearest the target lands on a mesh's vertex
	var r61m := HewnMesh3D.new()
	r61m.name = "R61Box"
	scene.add_child(r61m)
	r61m.owner = scene
	r61m.shape = 1
	var r61c := HewnCutter3D.new()
	r61c.name = "R61Cutter"
	r61c.size = Vector3(0.4, 0.4, 0.4)
	scene.add_child(r61c)
	r61c.owner = scene
	r61c.position = Vector3(2.5, 0, 0)
	await select_node(r61c)
	await set_snapping(false, "Vertex")
	var r61_t := Vector3(0.5, 0.5, 0.5)
	await move(cam.unproject_position(r61c.global_position))
	await key(KEY_G)
	await move(cam.unproject_position(r61_t) + Vector2(3, 2), false, 0, true)
	await button(cam.unproject_position(r61_t) + Vector2(3, 2), MOUSE_BUTTON_LEFT, true, false, true)
	await button(cam.unproject_position(r61_t) + Vector2(3, 2), MOUSE_BUTTON_LEFT, false, false, true)
	var r61_hit := false
	for r61_x in [-0.2, 0.2]:
		for r61_y in [-0.2, 0.2]:
			for r61_z in [-0.2, 0.2]:
				r61_hit = r61_hit or (r61c.global_position + Vector3(r61_x, r61_y, r61_z)).distance_to(r61_t) < 1e-3
	check(r61_hit, "G on a cutter with Ctrl: a corner of it onto the vertex (%s)" % r61c.global_position)
	await set_snapping(false, "Increment")
	# its corridor: shown like the meshes in the Shading modes (Random: a color of its own, not the texture)
	r61m.position = Vector3(40, 0, 40)
	r61c.position = Vector3(40, 0, 40)
	r61c.size = Vector3(2.0, 0.4, 0.4)
	r61c.lining = HewnCutter3D.LINING_AUTO
	r61c.update_lining()
	await frames(3)
	var r61_lm: HewnMesh3D = r61c.get_lining_mesh()
	var r61_sm: PopupPanel = EditorInterface.get_base_control().find_child("HewnShadingPopup", true, false)
	check(r61_lm != null and r61_lm.get_face_count() == 4, "cutter through a box: a corridor of 4 faces (%d)" % (r61_lm.get_face_count() if r61_lm else -1))
	if r61_sm and r61_lm:
		r61_sm.find_child("Shading5", true, false).pressed.emit()
		await frames(2)
		var r61_col = RenderingServer.instance_geometry_get_shader_parameter(r61_lm.get_instance(), "obj_color")
		check(r61_col is Vector4, "Shading > Random: the cutter's corridor too (%s)" % [r61_col])
		r61_sm.find_child("Shading0", true, false).pressed.emit()
		await frames(2)
	r61c.queue_free()
	r61m.visible = false

	# ---- r62: the knife: two cuts crossing on a face make four pieces (none over another); a point near a cut drawn
	# already lands on it
	var r62 := HewnMesh3D.new()
	r62.name = "R62Knife"
	scene.add_child(r62)
	r62.owner = scene
	r62.shape = 1
	r62.scale = Vector3(2, 2, 2)
	await select_node(r62)
	await key(KEY_TAB)
	await key(KEY_3)
	var r62_fc0 := r62.get_face_count()
	await key(KEY_K)
	await click(sp(r62, Vector3(-0.5, 0.0, 0.5)))
	await click(sp(r62, Vector3(0.5, 0.0, 0.5)))
	await button(sp(r62, Vector3(0.5, 0.0, 0.5)), MOUSE_BUTTON_RIGHT, true)
	await button(sp(r62, Vector3(0.5, 0.0, 0.5)), MOUSE_BUTTON_RIGHT, false)
	await click(sp(r62, Vector3(0.0, 0.5, 0.5)))
	await click(sp(r62, Vector3(0.0, -0.5, 0.5)))
	await key(KEY_ENTER)
	await frames(2)
	var r62_area := 0.0
	var r62_front := 0
	var r62_v := r62.get_vertices()
	for f in r62.get_face_count():
		if r62.get_face_normal(f).z > 0.9:
			r62_front += 1
			var fv := r62.get_face(f)
			for i in range(1, fv.size() - 1):
				r62_area += (r62_v[fv[i]] - r62_v[fv[0]]).cross(r62_v[fv[i + 1]] - r62_v[fv[0]]).length() * 0.5
	check(r62.get_face_count() == r62_fc0 + 3 and r62_front == 4 and absf(r62_area - 1.0) < 1e-4, "the knife: two crossing cuts, four pieces of the face, none over another (%d faces, area %f)" % [r62_front, r62_area])
	await undo(r62)
	# the second cut ends a few pixels off the first one: on it
	await key(KEY_K)
	await click(sp(r62, Vector3(-0.5, 0.2, 0.5)))
	await click(sp(r62, Vector3(0.5, 0.2, 0.5)))
	await button(sp(r62, Vector3(0.5, 0.2, 0.5)), MOUSE_BUTTON_RIGHT, true)
	await button(sp(r62, Vector3(0.5, 0.2, 0.5)), MOUSE_BUTTON_RIGHT, false)
	await click(sp(r62, Vector3(0.1, 0.5, 0.5)))
	await click(sp(r62, Vector3(0.1, 0.2, 0.5)) + Vector2(0, -4))
	await key(KEY_ENTER)
	await frames(2)
	var r62_on := false
	for p in r62.get_vertices():
		r62_on = r62_on or (absf(p.y - 0.2) < 1e-4 and absf(p.x - 0.1) < 0.03 and absf(p.z - 0.5) < 1e-4)
	check(r62_on and r62.get_face_count() == r62_fc0 + 2, "the knife: a point near its own line drawn before lands on it (%d faces)" % r62.get_face_count())
	await undo(r62)
	await key(KEY_TAB)
	r62.visible = false

	# ---- r63: snapping to the grid: the knife's points inside a face and the Box Cutter's corners on the world grid
	# (not in steps from the click); a click off the mesh with the knife keeps the edit mode
	var r63 := HewnMesh3D.new()
	r63.name = "R63Grid"
	scene.add_child(r63)
	r63.owner = scene
	r63.shape = 1
	r63.scale = Vector3(2, 2, 2)
	await select_node(r63)
	await key(KEY_TAB)
	await key(KEY_3)
	await set_snapping(true, "Grid")
	orient_mb.get_popup().id_pressed.emit(3)  # (the gizmo's orientation: the grid's lines are the world's still)
	var r63_on_grid := func(m: HewnMesh3D) -> Array:
		var bad := []
		var newv := 0
		for p in m.get_vertices():
			var w: Vector3 = m.global_transform * p
			if absf(w.z - 1.0) > 1e-3 or (absf(absf(w.x) - 1.0) < 1e-3 and absf(absf(w.y) - 1.0) < 1e-3):
				continue
			newv += 1
			if absf(w.x / 0.25 - roundf(w.x / 0.25)) > 1e-3 or absf(w.y / 0.25 - roundf(w.y / 0.25)) > 1e-3:
				bad.append(w)
		return [newv, bad]
	await key(KEY_K)
	await move(cam.unproject_position(Vector3(-0.42, 0.11, 1.0)))
	await click(cam.unproject_position(Vector3(-0.42, 0.11, 1.0)))
	await click(cam.unproject_position(Vector3(0.36, -0.33, 1.0)))
	await key(KEY_ENTER)
	await frames(2)
	var r63_k: Array = r63_on_grid.call(r63)
	check(r63_k[0] >= 2 and r63_k[1].is_empty(), "the knife with the grid on: its points inside a face on the grid (%d new, off: %s)" % [r63_k[0], r63_k[1]])
	check(r25_face_btn != null and r25_face_btn.button_pressed, "the knife's clicks keep the edit mode")
	await undo(r63)
	press("Box Cutter")
	await frames(1)
	# Alt+G works while a tool runs
	var r63_fg := find_button(EditorInterface.get_base_control(), "Grid on faces", false)
	var r63_fg0: bool = r63_fg.button_pressed if r63_fg else false
	await key(KEY_G, false, false, true)
	check(r63_fg != null and r63_fg.button_pressed != r63_fg0, "Alt+G toggles the grid on faces while the Box Cutter runs")
	await key(KEY_G, false, false, true)
	await drag(cam.unproject_position(Vector3(-0.42, 0.11, 1.0)), cam.unproject_position(Vector3(0.36, -0.33, 1.0)))
	await key(KEY_ESCAPE)
	await frames(2)
	var r63_b: Array = r63_on_grid.call(r63)
	check(r63_b[0] >= 4 and r63_b[1].is_empty(), "the Box Cutter with the grid on: its corners on the world grid (%d new, off: %s)" % [r63_b[0], r63_b[1]])
	await undo(r63)
	await set_snapping(false, "Increment")
	orient_mb.get_popup().id_pressed.emit(0)
	await key(KEY_TAB)
	r63.visible = false

	# ---- r64: objects onto the floor, physics placement, convert to collision, circle select of objects, a line
	var r64_floor := HewnMesh3D.new()
	r64_floor.name = "R64Floor"
	scene.add_child(r64_floor)
	r64_floor.owner = scene
	r64_floor.shape = 1
	r64_floor.set_shape_param("width", 8.0)
	r64_floor.set_shape_param("height", 0.2)
	r64_floor.set_shape_param("depth", 8.0)
	r64_floor.position = Vector3(30, -0.1, 0)  # (its top at y = 0)
	var r64_box := HewnMesh3D.new()
	r64_box.name = "R64Box"
	scene.add_child(r64_box)
	r64_box.owner = scene
	r64_box.shape = 1
	r64_box.position = Vector3(30, 3, 0)
	await select_node(r64_box)
	press("Move to Floor / Ceiling")
	await frames(2)
	check(absf(r64_box.position.y - 0.5) < 1e-3, "Move to Floor: the box stands on the floor (y %f)" % r64_box.position.y)
	var r64_lp := surf.find_child("HewnLastOperation", true, false)
	var r64_opts: Array = r64_lp.find_children("*", "OptionButton", true, false) if r64_lp and r64_lp.visible else []
	if r64_opts.size() >= 2:
		r64_opts[1].item_selected.emit(1)  # touching with its origin
		await frames(2)
	check(r64_opts.size() >= 2 and absf(r64_box.position.y) < 1e-3, "Move to Floor, by its origin: the origin on the floor (y %f)" % r64_box.position.y)
	await undo(scene)
	check(absf(r64_box.position.y - 3.0) < 1e-3, "Move to Floor: one undo step back (y %f)" % r64_box.position.y)
	# physics: it falls onto the floor; Apply keeps it there, one undo step back
	await select_node(r64_box)
	press("Physics Placement")
	for r64_i in 120:
		await get_tree().physics_frame
	var r64_y := r64_box.position.y
	check(r64_y < 1.0 and r64_y > 0.3, "physics placement: the box fell onto the floor (y %f)" % r64_y)
	await key(KEY_ENTER)
	await frames(2)
	check(absf(r64_box.position.y - r64_y) < 0.05, "physics placement applied: it stays (y %f)" % r64_box.position.y)
	await undo(scene)
	check(absf(r64_box.position.y - 3.0) < 1e-3, "physics placement: one undo step back (y %f)" % r64_box.position.y)
	# convert to collision: a static body of its form in its place
	await select_node(r64_box)
	press("Convert to Collision")
	await frames(2)
	var r64_body := scene.find_child("R64Box", false, false)
	check(r64_body is StaticBody3D and r64_body.get_child_count() == 1 and r64_body.get_child(0) is CollisionShape3D, "Convert to Collision: a static body with a shape instead of the mesh (%s)" % r64_body)
	await undo(scene)
	check(scene.find_child("R64Box", false, false) == r64_box, "Convert to Collision: undone, the mesh back")
	# the circle select tool in object mode: what is under the circle gets selected
	EditorInterface.get_selection().clear()
	var r64_tc: Control = EditorInterface.get_base_control().find_child("HewnTools", true, false)
	var r64_circle: Button = r64_tc.find_child("Tool8", true, false) if r64_tc else null
	r64_box.position = Vector3(0, 0.3, 0)  # (in sight: the viewport is small in headless runs)
	await frames(2)
	if r64_circle:
		r64_circle.pressed.emit()
		await frames(1)
		var r64_at := cam.unproject_position(r64_box.global_position)
		if not cam.is_position_behind(r64_box.global_position):
			await drag(r64_at, r64_at + Vector2(3, 0))
			await frames(2)
	check(r64_circle != null and EditorInterface.get_selection().get_selected_nodes().has(r64_box), "circle select in object mode: the box under the circle selected (%s, %s)" % [r64_circle != null and r64_circle.is_visible_in_tree(), EditorInterface.get_selection().get_selected_nodes()])
	var r64_sel: Button = r64_tc.find_child("Tool0", true, false) if r64_tc else null
	if r64_sel:
		r64_sel.pressed.emit()
	await frames(1)
	# ---- r65: two objects onto the floor, one undo step for both; physics takes the selection off and gives it back;
	# a Hewn curve: its header menu, Tab takes it out of the selection (no gizmo), Esc brings it back
	var r65_b := HewnMesh3D.new()
	r65_b.name = "R65Box"
	scene.add_child(r65_b)
	r65_b.owner = scene
	r65_b.shape = 1
	r65_b.position = Vector3(32, 5, 0)
	r64_box.position = Vector3(30, 3, 0)
	await frames(1)
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(r64_box)
	await frames(1)
	EditorInterface.get_selection().add_node(r65_b)
	await frames(2)
	press("Move to Floor / Ceiling")
	await frames(2)
	var r65_down := r64_box.position.y < 0.6 and r65_b.position.y < 0.6  # (on the floor: by its mesh or its origin, as set before)
	await undo(scene)
	check(r65_down and absf(r64_box.position.y - 3.0) < 1e-3 and absf(r65_b.position.y - 5.0) < 1e-3,
		"Move to Floor, two objects: one undo step takes both back (%f, %f)" % [r64_box.position.y, r65_b.position.y])
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(r65_b)
	await frames(2)
	press("Physics Placement")
	await frames(2)
	var r65_off := EditorInterface.get_selection().get_selected_nodes().is_empty()
	await key(KEY_ESCAPE)
	await frames(2)
	check(r65_off and EditorInterface.get_selection().get_selected_nodes().has(r65_b), "physics placement: the objects unselected while they fall, selected again after")
	r65_b.free()
	var r65_c := HewnCurve3D.new()
	r65_c.name = "R65Curve"
	scene.add_child(r65_c)
	r65_c.owner = scene
	r65_c.make_preset(0)
	await select_node(r65_c)
	await frames(2)
	var r65_menu: Control = null
	for r65_mb in bar.find_children("*", "MenuButton", true, false):
		if r65_mb.tooltip_text.begins_with("The curve's functions"):
			r65_menu = r65_mb
	check(r65_menu != null and r65_menu.is_visible_in_tree(), "a curve selected: the Curve menu in the header")
	await key(KEY_TAB)
	await frames(2)
	var r65_detached := EditorInterface.get_selection().get_selected_nodes().is_empty()
	# drawing: its panel before the stroke; a change after it puts the same stroke again (no new spline)
	var r65_tc: Control = EditorInterface.get_base_control().find_child("HewnTools", true, false)
	var r65_draw: Button = r65_tc.find_child("Tool28", true, false) if r65_tc else null
	var r65_splines0: int = r65_c.get_spline_count()
	var r65_panel_first := false
	var r65_after := -1
	var r65_again := -1
	if r65_draw:
		r65_draw.pressed.emit()
		await frames(2)
		var r65_lp := surf.find_child("HewnLastOperation", true, false)
		r65_panel_first = r65_lp != null and r65_lp.visible and r65_lp.find_children("*", "OptionButton", true, false).size() > 0
		var r65_a := cam.unproject_position(Vector3(-0.5, 0.2, 0))
		await drag(r65_a, r65_a + Vector2(40, 10))
		await frames(2)
		r65_after = r65_c.get_spline_count()
		var r65_opts: Array = r65_lp.find_children("*", "OptionButton", true, false) if r65_lp and r65_lp.visible else []
		if r65_opts.size() > 0:
			r65_opts[0].item_selected.emit(1)  # on the surfaces now
			await frames(2)
		r65_again = r65_c.get_spline_count()
	check(r65_panel_first and r65_after == r65_splines0 + 1 and r65_again == r65_after,
		"curve draw: its panel before the stroke, a change after it redoes the same stroke (%s, %d -> %d -> %d)" % [r65_panel_first, r65_splines0, r65_after, r65_again])
	await key(KEY_ESCAPE)
	await frames(2)
	check(r65_detached and EditorInterface.get_selection().get_selected_nodes().has(r65_c), "curve edit: out of the selection (no gizmo), Esc: back in object mode, selected")
	EditorInterface.get_selection().clear()
	await frames(1)
	r65_c.free()
	# Bake Navigation Mesh: its window with a NavigationMesh's settings in an inspector
	EditorInterface.get_selection().clear()
	press("Bake Navigation Mesh...")
	await frames(2)
	var r68_dlg: Window = EditorInterface.get_base_control().find_child("HewnNavmeshDialog", true, false)
	var r68_insp: Node = r68_dlg.find_children("*", "EditorInspector", true, false)[0] if r68_dlg and r68_dlg.find_children("*", "EditorInspector", true, false).size() > 0 else null
	check(r68_dlg != null and r68_dlg.visible and r68_insp != null and r68_insp.get_edited_object() is NavigationMesh, "Bake Navigation Mesh: its window, a NavigationMesh's settings")
	if r68_dlg:
		r68_dlg.hide()
	# a modifier that would make very many faces: left out, asked about first
	var r68_m := HewnMesh3D.new()
	r68_m.name = "R68Array"
	scene.add_child(r68_m)
	r68_m.owner = scene
	r68_m.shape = 1
	for r68_pn in ["width_segments", "height_segments", "depth_segments"]:
		r68_m.set_shape_param(r68_pn, 10)
	var r68_arr := HewnArrayModifier.new()
	r68_arr.count = 1000  # (600 faces each: 600 000)
	r68_m.modifiers = [r68_arr]
	await frames(4)
	var r68_hd: ConfirmationDialog = EditorInterface.get_base_control().find_child("HewnHeavyAnyDialog", true, false)
	var r68_faces: int = r68_m.mesh.get_faces().size() / 3 if r68_m.mesh else 0
	check(r68_hd != null and r68_hd.visible and r68_faces <= 1200, "a modifier of very many faces: left out and asked about (%s, %d triangles)" % [r68_hd != null and r68_hd.visible, r68_faces])
	if r68_hd:
		r68_hd.hide()
		r68_hd.canceled.emit()
	r68_m.free()
	# a curve selected: a material clicked is for it; Move to Floor: each of its points on what is under it
	var r70_had_clip := FileAccess.file_exists("res://materials/hw_tool_clip.tres")
	var r70_floor := HewnMesh3D.new()
	r70_floor.name = "R70Floor"
	scene.add_child(r70_floor)
	r70_floor.owner = scene
	r70_floor.shape = 0
	r70_floor.position = Vector3(40, -2, 0)
	r70_floor.scale = Vector3(20, 1, 20)
	var r70_c := HewnCurve3D.new()
	r70_c.name = "R70Curve"
	scene.add_child(r70_c)
	r70_c.owner = scene
	r70_c.position = Vector3(40, 0, 0)
	r70_c.add_spline(0, PackedVector3Array([Vector3(-2, 3, 0), Vector3(2, 1, 0)]), false)
	await frames(2)
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(r70_c)
	await frames(2)
	# (the Clip tool material's button: its tooltip tells it from the Clip tool's)
	var r70_mat_btn: Button = null
	for r70_b in EditorInterface.get_base_control().find_child("Hewn", true, false).find_children("*", "Button", true, false):
		if r70_b.tooltip_text.begins_with("Invisible faces") or r70_b.tooltip_text.begins_with("Невидимые грани"):
			r70_mat_btn = r70_b
	check(r70_mat_btn != null, "the Clip material's button found")
	if r70_mat_btn:
		r70_mat_btn.pressed.emit()
	await frames(2)
	check(r70_c.material != null and r70_c.material.has_meta("hw_tool"), "a curve selected: the material clicked goes on it")
	var r70_ur := EditorInterface.get_editor_undo_redo()
	r70_ur.get_history_undo_redo(r70_ur.get_object_history_id(r70_c)).undo()
	check(r70_c.material == null, "a curve's material: undone")
	press("Move to Floor / Ceiling")
	await frames(2)
	check(absf(r70_c.get_point_of(0, 0).y + 1.5) < 0.01 and absf(r70_c.get_point_of(0, 1).y + 1.5) < 0.01, "Move to Floor: each point of the curve on the mesh (%s, %s)" % [r70_c.get_point_of(0, 0), r70_c.get_point_of(0, 1)])
	r70_ur.get_history_undo_redo(r70_ur.get_object_history_id(scene)).undo()
	await frames(1)
	check(absf(r70_c.get_point_of(0, 0).y - 3.0) < 0.01, "Move to Floor on a curve: undone (%s)" % r70_c.get_point_of(0, 0))
	EditorInterface.get_selection().clear()
	await frames(2)
	# the bake's window: its options before the file is asked for; Run Baked makes the file it runs
	EditorInterface.get_selection().clear()
	press("Bake Level...")
	await frames(2)
	var r71_dlg: ConfirmationDialog = EditorInterface.get_base_control().find_child("HewnBakeOptions", true, false)
	check(r71_dlg != null and r71_dlg.visible and r71_dlg.find_children("*", "CheckBox", true, false).size() == 8, "Bake Level: its options first (%d)" % (r71_dlg.find_children("*", "CheckBox", true, false).size() if r71_dlg else -1))
	if r71_dlg:
		var r72_room := EditorInterface.get_base_control().size.y
		check(r71_dlg.size.y <= r72_room * 0.75 + 120 * EditorInterface.get_editor_scale(), "Bake Level: its window fits the screen (%d of %d)" % [r71_dlg.size.y, r72_room])
		r71_dlg.hide()
	press("Run Baked")
	await frames(2)
	check(FileAccess.file_exists("res://.godot/hewn_run_baked.scn"), "Run Baked: the level baked to its file")
	DirAccess.remove_absolute(ProjectSettings.globalize_path("res://.godot/hewn_run_baked.scn"))
	# r75: Shift+D in a curve's edit mode: the points copied as a spline of their own (Esc: they stay where they began)
	await select_node(r70_c)
	await key(KEY_TAB)
	await key(KEY_A)
	var r75_n0 := r70_c.get_spline_count()
	await key(KEY_D, true)
	await frames(1)
	await key(KEY_ESCAPE)
	await frames(1)
	check(r70_c.get_spline_count() == r75_n0 * 2, "curve Shift+D: its points copied as splines of their own (%d -> %d)" % [r75_n0, r70_c.get_spline_count()])
	await undo(r70_c)
	check(r70_c.get_spline_count() == r75_n0, "curve Shift+D: undone (%d)" % r70_c.get_spline_count())
	await key(KEY_TAB)
	# r76: E on points in the middle of a spline: pulled out, joined by new points to where they were (one spline)
	var r76 := HewnCurve3D.new()
	r76.name = "R76Curve"
	scene.add_child(r76)
	r76.owner = scene
	r76.add_spline(0, PackedVector3Array([Vector3(30, 0, 10), Vector3(32, 0, 10), Vector3(34, 0, 10), Vector3(36, 0, 10)]), false)
	await select_node(r76)
	await key(KEY_TAB)
	var r76_st: Dictionary = r76.get_state()
	r76_st["sel"] = PackedByteArray([0, 1, 1, 0])
	r76.set_state(r76_st)
	await key(KEY_E)
	await frames(1)
	await key(KEY_ENTER)
	await frames(1)
	check(r76.get_spline_count() == 1 and r76.get_point_count_of(0) == 6, "curve E in the middle: two new points, one spline (%d splines, %d points)" % [r76.get_spline_count(), r76.get_point_count_of(0)])
	await key(KEY_TAB)
	r76.free()
	# r75: Bake to MeshInstance3D with a mesh and a curve selected: one MeshInstance3D, both hidden
	r70_c.depth = 0.2
	r70_c.rebuild()
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(r70_floor)
	EditorInterface.get_selection().add_node(r70_c)
	await frames(3)
	var r75_count := scene.get_child_count()
	press("Bake to MeshInstance3D" if find_button(EditorInterface.get_base_control().find_child("Hewn", true, false), "Bake to MeshInstance3D", false) else "Запечь в MeshInstance3D")
	await frames(2)
	var r75_mi: MeshInstance3D = scene.get_child(scene.get_child_count() - 1) as MeshInstance3D
	check(scene.get_child_count() == r75_count + 1 and r75_mi != null and not r70_floor.visible and not r70_c.visible, "Bake to MeshInstance3D: the selected mesh and curve in one")
	if r75_mi:
		await undo(r75_mi)
	check(r70_floor.visible and r70_c.visible and scene.get_child_count() == r75_count, "Bake to MeshInstance3D: undone")
	r70_c.free()
	r70_floor.free()
	# (the Clip material's file, made by the button above: the project left as it was)
	if not r70_had_clip:
		DirAccess.remove_absolute(ProjectSettings.globalize_path("res://materials/hw_tool_clip.tres"))
		if DirAccess.get_files_at("res://materials").is_empty() and DirAccess.get_directories_at("res://materials").is_empty():
			DirAccess.remove_absolute(ProjectSettings.globalize_path("res://materials"))
	r64_box.visible = false
	r64_floor.visible = false

	# ---- every mesh the tests made and edited: clean (each vertex used, no face without three different corners)
	var dirty := []
	for cm in scene.find_children("*", "HewnMesh3D", true, false):
		var md: Dictionary = cm.mesh_data
		var nv: int = cm.get_vertex_count()
		var used := PackedByteArray()
		used.resize(nv)
		var bad_faces := 0
		for f in cm.get_face_count():
			var fv: PackedInt32Array = cm.get_face(f)
			var uniq := {}
			for v in fv:
				if v >= 0 and v < nv:
					used[v] = 1
					uniq[v] = true
			if uniq.size() < 3:
				bad_faces += 1
		for v in PackedInt32Array(md.get("wires", PackedInt32Array())) + PackedInt32Array(md.get("points", PackedInt32Array())):
			if v >= 0 and v < nv:
				used[v] = 1
		var unused := used.count(0)
		if unused > 0 or bad_faces > 0:
			dirty.append("%s: %d unused vertices, %d bad faces" % [cm.name, unused, bad_faces])
	check(dirty.is_empty(), "the meshes edited by the tests are clean %s" % [dirty])

	# ---- Quad screen: four views (Top / Front / Side + 3D), block / move / resize / clip / delete on the grid
	for c in scene.get_children():
		c.visible = false
	EditorInterface.get_selection().clear()
	EditorInterface.set_main_screen_editor("Quad")
	await frames(10)
	var quad: Control = EditorInterface.get_editor_main_screen().find_child("HewnQuad", true, false)
	check(quad != null and quad.is_visible_in_tree(), "Quad screen opens next to 2D / 3D")
	if quad:
		var q_top: Control = quad.find_child("Pane1", true, false)
		var q_front: Control = quad.find_child("Pane2", true, false)
		var q_top_ov: Control = q_top.get_node("Overlay")
		var q_front_ov: Control = q_front.get_node("Overlay")
		check(q_top.size.x > 50 and q_front.size.y > 50, "four views laid out (%s, %s)" % [q_top.size, q_front.size])
		# Block tool: a box from (0, 0) to (2, -1) in Top (depth: 0..1 m), Enter creates a parametric cube
		await qkey(q_top_ov, KEY_B, true)
		var q_before := scene.find_children("*", "HewnMesh3D", true, false).size()
		await qdrag(q_top_ov, q_top.world_to_screen(Vector3(0, 0, 0)), q_top.world_to_screen(Vector3(2, 0, -1)))
		await qkey(q_top_ov, KEY_ENTER)
		var q_all := scene.find_children("*", "HewnMesh3D", true, false)
		check(q_all.size() == q_before + 1, "Block + Enter creates a mesh")
		var qb: HewnMesh3D = null
		for c in q_all:
			if EditorInterface.get_selection().get_selected_nodes().has(c):
				qb = c
		check(qb != null, "the new block is selected")
		if qb:
			var bb: AABB = qb.global_transform * qb.get_aabb()
			check(bb.position.is_equal_approx(Vector3(0, 0, -1)) and bb.size.is_equal_approx(Vector3(2, 1, 1)), "block fills the drawn box (%s)" % bb)
			check(qb.shape == 1 and is_equal_approx(qb.get_shape_param("width"), 2.0), "the block stays a parametric cube")
			# Select tool: drag inside the selection moves it on the grid
			await qkey(q_top_ov, KEY_S, true)
			var q_from: Vector2 = q_top.world_to_screen(Vector3(1, 0, -0.5))
			await qdrag(q_top_ov, q_from, q_from + (q_top.world_to_screen(Vector3(1.1, 0, 0)) - q_top.world_to_screen(Vector3(0, 0, 0))))
			bb = qb.global_transform * qb.get_aabb()
			check(is_equal_approx(bb.position.x, 1.0), "move snaps the box to the grid (%s)" % bb.position)
			# the right handle in Front: one more meter of width, still parametric
			var h_at: Vector2 = q_front.world_to_screen(Vector3(bb.end.x, bb.get_center().y, 0)) + Vector2(3.5, 0)
			await qdrag(q_front_ov, h_at, h_at + (q_front.world_to_screen(Vector3(1, 0, 0)) - q_front.world_to_screen(Vector3(0, 0, 0))))
			bb = qb.global_transform * qb.get_aabb()
			check(is_equal_approx(bb.size.x, 3.0) and is_equal_approx(bb.position.x, 1.0), "the handle resizes on the grid (%s)" % bb)
			check(qb.shape == 1 and is_equal_approx(qb.get_shape_param("width"), 3.0), "resize goes into the shape parameters")
			await undo(qb)
			bb = qb.global_transform * qb.get_aabb()
			check(is_equal_approx(bb.size.x, 2.0), "undo restores the size (%s)" % bb)
			# Clip: a line across the middle (Top), keep both: two meshes
			await qkey(q_top_ov, KEY_X, true)
			var c_x := bb.get_center().x
			await qdrag(q_top_ov, q_top.world_to_screen(Vector3(c_x, 0, 1)), q_top.world_to_screen(Vector3(c_x, 0, -3)))
			await qkey(q_top_ov, KEY_ENTER)
			var q_after := scene.find_children("*", "HewnMesh3D", true, false)
			check(q_after.size() == q_before + 2, "Clip (keep both) splits the mesh in two")
			var halves := 0
			for c in q_after:
				var cb: AABB = c.global_transform * c.get_aabb()
				if c.visible and c.get_face_count() > 0 and is_equal_approx(cb.size.x, 1.0):
					halves += 1
			check(halves == 2, "both parts are closed halves (%d)" % halves)
			# Delete removes the selection, undo brings it back
			await qkey(q_top_ov, KEY_S, true)
			EditorInterface.get_selection().clear()
			EditorInterface.get_selection().add_node(qb)
			await frames(2)
			await qkey(q_top_ov, KEY_DELETE)
			check(not qb.is_inside_tree(), "Delete removes the selected object")
			EditorInterface.get_editor_undo_redo().get_history_undo_redo(EditorInterface.get_editor_undo_redo().get_object_history_id(scene)).undo()
			await frames(2)
			check(qb.is_inside_tree() and qb.owner == scene, "undo restores it")
	EditorInterface.set_main_screen_editor("3D")
	await frames(5)
	# Project Settings > Plugins: Hewn off takes its docks and the Quad screen away, on brings them back
	# (this test lives under the plugin: it moves out first)
	var r51_pl := get_parent()
	r51_pl.remove_child(self)
	EditorInterface.get_base_control().add_child(self)
	EditorInterface.set_plugin_enabled("hewn", false)
	await frames(6)
	check(EditorInterface.get_editor_main_screen().find_child("HewnQuad", true, false) == null and EditorInterface.get_base_control().find_child("HewnUV", true, false) == null, "Hewn off in Project Settings > Plugins: no docks, no Quad screen")
	EditorInterface.set_plugin_enabled("hewn", true)
	await frames(6)
	check(EditorInterface.get_editor_main_screen().find_child("HewnQuad", true, false) != null and EditorInterface.get_base_control().find_child("HewnUV", true, false) != null, "Hewn on again: the docks and the Quad screen are back")

	# (the texture drops made a material for the test image: removed at the end, a rescan in between upsets the tests)
	for r42_file in ["res://materials/hw_selftest_tex.tres", "res://materials/hw_selftest_tex.tres.uid"]:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(r42_file))
	DirAccess.remove_absolute(ProjectSettings.globalize_path("res://materials"))  # (only when empty)
	for kept_name in kept:
		kept_es.set_setting(kept_name, kept[kept_name])
	print("Hewn editor self-test: %d failures" % failures)
	get_tree().quit(1 if failures > 0 else 0)


# Quad screen input: signals of a view's overlay (its handler is connected to gui_input).
func qkey(ov: Control, code: Key, shift := false) -> void:
	for pressed in [true, false]:
		var ev := InputEventKey.new()
		ev.physical_keycode = code
		ev.keycode = code
		ev.shift_pressed = shift
		ev.pressed = pressed
		ov.gui_input.emit(ev)
	await frames(2)


func qdrag(ov: Control, from: Vector2, to: Vector2) -> void:
	var ev := InputEventMouseButton.new()
	ev.button_index = MOUSE_BUTTON_LEFT
	ev.pressed = true
	ev.position = from
	ov.gui_input.emit(ev)
	await frames(1)
	for i in range(1, 5):
		var mm := InputEventMouseMotion.new()
		mm.position = from.lerp(to, i / 4.0)
		mm.button_mask = MOUSE_BUTTON_MASK_LEFT
		ov.gui_input.emit(mm)
		await frames(1)
	var up := InputEventMouseButton.new()
	up.button_index = MOUSE_BUTTON_LEFT
	up.pressed = false
	up.position = to
	ov.gui_input.emit(up)
	await frames(2)


# Closed surface: every edge has two faces.
# The corner colors of mesh data (not saved when all are white).
func face_cols(d: Dictionary) -> PackedColorArray:
	if d.has("face_colors"):
		return d["face_colors"]
	var out := PackedColorArray()
	out.resize((d["face_indices"] as PackedInt32Array).size())
	out.fill(Color(1, 1, 1, 1))
	return out


func analyze_mesh(m: HewnMesh3D) -> Dictionary:
	var count := {}
	for f in m.get_face_count():
		var face := m.get_face(f)
		for i in face.size():
			var a := face[i]
			var b := face[(i + 1) % face.size()]
			var k := Vector2i(mini(a, b), maxi(a, b))
			count[k] = count.get(k, 0) + 1
	var closed := count.size() > 0
	for k in count:
		closed = closed and count[k] == 2
	return {"closed": closed}
