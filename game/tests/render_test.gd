# Real GPU regression for black sky radiance affecting reflections and aerial fog.
# Run with a display (NOT --headless):
# godot --path game --script res://tests/render_test.gd -- --seed 1 --hide-ui
# Optional: --render-test-out /absolute/output/directory to save each view as a PNG.
extends SceneTree

var _errors := 0
var _output := ""


func _initialize() -> void:
	call_deferred("_run")


func _expect(ok: bool, message: String) -> void:
	if not ok:
		_errors += 1
		push_error(message)


func _run() -> void:
	if DisplayServer.get_name() == "headless":
		push_error("RENDER test requires a real rendering device and a display")
		quit(1)
		return
	var args := OS.get_cmdline_user_args()
	# main.gd uses these to disable the random interactive world/menu and autosave.
	if not args.has("--seed") or not args.has("--hide-ui"):
		push_error("RENDER test requires -- --seed 1 --hide-ui")
		quit(1)
		return
	var oi := args.find("--render-test-out")
	if oi >= 0 and oi + 1 < args.size():
		_output = args[oi + 1]
		if DirAccess.make_dir_recursive_absolute(_output) != OK:
			push_error("Cannot create render-test output directory")
			quit(1)
			return
	var main = load("res://scenes/main.tscn").instantiate()
	root.add_child(main)
	var game: Node = root.get_node("Game")
	game.paused = true
	_expect(main.env.sky.process_mode == Sky.PROCESS_MODE_REALTIME, "Dynamic sky must use realtime radiance filtering")
	_expect(main.env.sky.radiance_size == Sky.RADIANCE_SIZE_256, "Realtime sky requires 256px radiance")
	# Bound the test if loading or meshing ever stops progressing.
	var deadline := Time.get_ticks_msec() + 120000
	while main.renderer.pending_count() > 0:
		if Time.get_ticks_msec() > deadline:
			push_error("RENDER timed out waiting for world meshes")
			quit(1)
			return
		await process_frame
	var target: Vector3 = main.rig.target
	# Include the reverse view and island underside: black back-facing surfaces
	# can be missed by a single sunny, top-down screenshot.
	for view in [
		["village", 35.0, 48.0, 150.0],
		["reverse", 215.0, 35.0, 140.0],
		["overhead", 35.0, 88.0, 220.0],
		["underside", 35.0, -25.0, 220.0],
	]:
		main.rig.set_view(target, view[1], view[2], view[3])
		# Change daylight every frame, as in normal play, while the map settles.
		for frame in 16:
			game.sim.step(1)
			await process_frame
		await RenderingServer.frame_post_draw
		var img := root.get_texture().get_image()
		_expect(img != null and not img.is_empty(), "Viewport image must not be empty")
		if img == null or img.is_empty():
			continue
		if not _output.is_empty():
			_expect(img.save_png(_output.path_join(view[0] + ".png")) == OK, "Could not save render screenshot")
		var black := 0
		for y in img.get_height():
			for x in img.get_width():
				var c := img.get_pixel(x, y)
				if maxf(c.r, maxf(c.g, c.b)) < 0.005:
					black += 1
		var ratio := float(black) / (img.get_width() * img.get_height())
		print("RENDER ", view[0], " black_fraction=", ratio)
		# The environment keeps even the underside visible. Allow isolated dark
		# details/edges, but reject the broad zero-radiance patches from this bug.
		_expect(ratio < 0.005, "Unexpected black surfaces in " + view[0])
	print("RENDER ", "PASS" if _errors == 0 else "FAIL")
	quit(0 if _errors == 0 else 1)
