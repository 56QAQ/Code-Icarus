extends Node3D
## Scene root: builds the environment, renderer, camera and HUD, routes input to the
## active tool, and supports scripted screenshots for automated visual checks:
##   godot --path game -- --shot out.png [--seed N] [--ticks N] [--cam x,y,z,yaw,pitch,dist]
##        [--admin type:{json}|break_bridge] [--council [id]] [--hide-ui] [--frames N]

var renderer: WorldRenderer
var chars: CharacterRenderer
var rig: CameraRig
var hud: HUD
var sun: DirectionalLight3D
var env: Environment
var brush: MeshInstance3D

var _shot_path := ""
var _shot_frames := 30
var _shot_wait := 0
var _hover_timer := 0.0
var _cli := {}


func _ready() -> void:
	_cli = _parse_cli()
	_build_environment()
	rig = CameraRig.new()
	add_child(rig)
	renderer = WorldRenderer.new()
	add_child(renderer)
	chars = CharacterRenderer.new()
	add_child(chars)
	_build_brush()
	var ui_layer := CanvasLayer.new()
	add_child(ui_layer)
	hud = HUD.new()
	ui_layer.add_child(hud)
	if _cli.has("hide-ui"):
		ui_layer.visible = false

	var seed := int(_cli.get("seed", "1"))
	if not Game.start_new_game({"seed": seed}):
		push_error("could not start game")
		return
	var info: Dictionary = Game.sim.world_info()
	var v: Vector3i = info["features"]["village"]
	rig.set_view(Vector3(v) + Vector3(0, 2, 0), 35.0, 48.0, 150.0)
	if _cli.has("cam"):
		var p: PackedStringArray = _cli["cam"].split(",")
		if p.size() >= 6:
			rig.set_view(Vector3(float(p[0]), float(p[1]), float(p[2])), float(p[3]), float(p[4]), float(p[5]))
	renderer.setup(Game.sim, rig.camera)
	chars.setup(Game.sim, rig.camera)
	hud.selection.focus_requested.connect(func(p: Vector3) -> void: rig.focus(p, 40.0))
	hud.civ_card.girl_selected.connect(func(id: int) -> void:
		_select_character(id)
		rig.focus(chars.position_of(id), 40.0))
	if _cli.has("ticks"):
		Game.sim.step(int(_cli["ticks"]))
	for cmd in _cli.get("admin", []):
		var parts: PackedStringArray = String(cmd).split(":", true, 1)
		var params: Variant = JSON.parse_string(parts[1]) if parts.size() > 1 else {}
		if parts[0] == "break_bridge":
			# Scenario shortcut: blow out the middle of the bridge deck.
			var f: Dictionary = info["features"]
			var mid: Vector3i = (Vector3i(f["bridge_a"]) + Vector3i(f["bridge_b"])) / 2
			Game.sim.admin("dig", {"pos": [mid.x, mid.y, mid.z], "radius": 4.5})
			continue
		Game.sim.admin(parts[0], params if params is Dictionary else {})
	if _cli.has("after-ticks"):
		Game.sim.step(int(_cli["after-ticks"]))
	if _cli.has("tab"):
		hud.selection._tab = _cli["tab"]
	if _cli.has("select"):
		_select_character(int(_cli["select"]))
	if _cli.has("council"):
		var cid := int(_cli["council"]) if String(_cli["council"]).is_valid_int() else 0
		hud.toggle_council(cid)
	if _cli.has("shot"):
		_shot_path = _cli["shot"]
		_shot_frames = int(_cli.get("frames", "20"))
		Game.paused = true


func _parse_cli() -> Dictionary:
	var out := {}
	var args := OS.get_cmdline_user_args()
	var i := 0
	while i < args.size():
		var a: String = args[i]
		if a.begins_with("--"):
			var key := a.substr(2)
			var val := "true"
			if i + 1 < args.size() and not args[i + 1].begins_with("--"):
				val = args[i + 1]
				i += 1
			if key == "admin":
				if not out.has("admin"):
					out["admin"] = []
				out["admin"].append(val)
			else:
				out[key] = val
		i += 1
	return out


func _build_environment() -> void:
	var we := WorldEnvironment.new()
	env = Environment.new()
	env.background_mode = Environment.BG_SKY
	var sky := Sky.new()
	var sm := ProceduralSkyMaterial.new()
	sm.sky_top_color = Color(0.22, 0.42, 0.72)
	sm.sky_horizon_color = Color(0.72, 0.80, 0.90)
	sm.ground_bottom_color = Color(0.62, 0.68, 0.80)
	sm.ground_horizon_color = Color(0.78, 0.84, 0.92)
	sm.sun_angle_max = 20.0
	sm.sky_curve = 0.12
	sky.sky_material = sm
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.ambient_light_energy = 0.9
	env.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.tonemap_exposure = 1.0
	env.tonemap_white = 6.0
	env.ssao_enabled = true
	env.ssao_radius = 1.6
	env.ssao_intensity = 1.6
	env.glow_enabled = true
	env.glow_intensity = 0.6
	env.glow_bloom = 0.05
	env.glow_hdr_threshold = 1.6
	env.fog_enabled = true
	env.fog_light_color = Color(0.72, 0.80, 0.92)
	env.fog_density = 0.00035
	env.fog_sky_affect = 0.0
	env.adjustment_enabled = true
	env.adjustment_saturation = 1.08
	we.environment = env
	add_child(we)

	sun = DirectionalLight3D.new()
	sun.light_energy = 1.25
	sun.light_color = Color(1.0, 0.96, 0.88)
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 420.0
	sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
	sun.shadow_bias = 0.04
	sun.shadow_normal_bias = 1.2
	sun.rotation_degrees = Vector3(-52, 38, 0)
	add_child(sun)

	# Sea of clouds under the floating islands.
	var clouds := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(6000, 6000)
	clouds.mesh = pm
	var cmat := ShaderMaterial.new()
	cmat.shader = load("res://shaders/cloudsea.gdshader")
	clouds.material_override = cmat
	clouds.position = Vector3(512, 28, 512)
	clouds.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(clouds)
	var clouds2 := clouds.duplicate() as MeshInstance3D
	clouds2.position = Vector3(512, 8, 512)
	var cmat2 := cmat.duplicate() as ShaderMaterial
	cmat2.set_shader_parameter("scale", 0.0026)
	cmat2.set_shader_parameter("speed", 0.0025)
	clouds2.material_override = cmat2
	add_child(clouds2)


func _build_brush() -> void:
	brush = MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 1.0
	sm.height = 2.0
	sm.radial_segments = 24
	sm.rings = 12
	brush.mesh = sm
	var mat := StandardMaterial3D.new()
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = Color(0.91, 0.76, 0.44, 0.18)
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.no_depth_test = false
	brush.material_override = mat
	brush.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	brush.visible = false
	add_child(brush)


func _process(delta: float) -> void:
	_update_daylight()
	if _shot_path != "":
		_screenshot_step()
		return
	_hover_timer -= delta
	if _hover_timer <= 0.0:
		_hover_timer = 0.05
		_update_hover()


func _update_daylight() -> void:
	if Game.sim == null or not Game.sim.has_game():
		return
	var ci: Dictionary = Game.sim.clock_info()
	var h: float = ci.get("hour", 12.0)
	# Sun elevation follows the hour; keep a little light at night (moonlight).
	var t := (h - 6.0) / 12.0  # 0 at sunrise, 1 at sunset
	var elev := sin(clampf(t, 0.0, 1.0) * PI)
	var day := clampf(elev * 1.6, 0.0, 1.0)
	sun.rotation_degrees = Vector3(-lerpf(8.0, 62.0, elev), 38.0 + (t - 0.5) * 70.0, 0)
	sun.light_energy = lerpf(0.18, 1.25, day)
	sun.light_color = Color(1.0, 0.96, 0.88).lerp(Color(1.0, 0.62, 0.38), 1.0 - clampf(elev * 2.5, 0.0, 1.0)) if day > 0.05 else Color(0.55, 0.65, 0.95)
	env.ambient_light_energy = lerpf(0.35, 0.9, day)
	env.background_energy_multiplier = lerpf(0.25, 1.0, day)


func _mouse_ray() -> Dictionary:
	var mp := get_viewport().get_mouse_position()
	var cam := rig.camera
	var from := cam.project_ray_origin(mp)
	var dir := cam.project_ray_normal(mp)
	return Game.sim.raycast(from, dir, 3000.0)


func _update_hover() -> void:
	if Game.sim == null or not Game.sim.has_game():
		return
	if get_viewport().gui_get_hovered_control() != null:
		brush.visible = false
		return
	var hit := _mouse_ray()
	if not hit.get("hit", false):
		hud.show_hover({})
		brush.visible = false
		return
	var cube: Vector3i = hit["cube"]
	var info: Dictionary = Game.sim.cube_info(cube)
	info["cube"] = cube
	hud.show_hover(info)
	var tool := Game.current_tool
	if tool in ["dig", "place", "meteor", "ignite", "flood"]:
		var r: float = Game.tool_params.get("radius", 3.0)
		var center := Vector3(cube) + Vector3(0.5, 0.5, 0.5)
		if tool == "place" or tool == "flood":
			center += Vector3(hit["normal"])
		brush.visible = true
		brush.position = center
		brush.scale = Vector3.ONE * maxf(r, 0.5)
	else:
		brush.visible = false


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		var k := event as InputEventKey
		match k.keycode:
			KEY_SPACE:
				Game.toggle_pause()
			KEY_1:
				Game.set_speed(1.0)
			KEY_2:
				Game.set_speed(2.0)
			KEY_3:
				Game.set_speed(5.0)
			KEY_4:
				Game.set_speed(20.0)
			KEY_ESCAPE:
				if hud.council.visible:
					hud.council.visible = false
				else:
					Game.set_tool("inspect")
			KEY_J:
				hud.toggle_council()
	if event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
		_apply_tool()


func _select_character(id: int) -> void:
	chars.selected_id = id
	hud.selection.show_character(id)
	Game.select({"kind": "character", "id": id})


func _apply_tool() -> void:
	if Game.current_tool == "inspect":
		var cid := chars.pick(get_viewport().get_mouse_position())
		if cid >= 0:
			_select_character(cid)
			return
	var hit := _mouse_ray()
	if not hit.get("hit", false):
		return
	var cube: Vector3i = hit["cube"]
	var normal: Vector3i = hit["normal"]
	var r: float = Game.tool_params.get("radius", 3.0)
	match Game.current_tool:
		"dig":
			Game.admin("dig", {"pos": cube, "radius": r})
		"place":
			Game.admin("place", {"pos": cube + normal, "radius": r, "material": Game.tool_params.get("material", "stone")})
		"meteor":
			Game.admin("meteor", {"pos": cube, "radius": maxf(2.0, r)})
		"ignite":
			Game.admin("ignite", {"pos": cube, "radius": minf(r, 3.0)})
		"flood":
			Game.admin("place", {"pos": cube + normal, "radius": r, "material": "water"})
		"inspect":
			var b: Dictionary = Game.sim.building_at(cube)
			if not b.is_empty():
				chars.selected_id = -1
				hud.selection.show_building(b["id"], cube)
			else:
				chars.selected_id = -1
				hud.selection.visible = false


func _screenshot_step() -> void:
	if renderer.pending_count() > 0:
		return
	_shot_wait += 1
	if _shot_wait < _shot_frames:
		return
	var img := get_viewport().get_texture().get_image()
	var err := img.save_png(_shot_path)
	print("screenshot saved: ", _shot_path, " err=", err)
	get_tree().quit(0 if err == OK else 1)
