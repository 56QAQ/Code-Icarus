extends Node3D
## Scene root: builds the environment, renderer, camera and HUD, routes input to the
## active tool, and supports scripted screenshots for automated visual checks:
##   godot --path game -- --shot out.png [--seed N | --load FILE] [--layout classic|continent] [--scenario key]
##        [--ticks N] [--cam x,y,z,yaw,pitch,dist]
##        [--admin type:{json}|break_bridge] [--council [id]] [--tech] [--ending] [--menu] [--civ-detail] [--tool id] [--focus-soldiers [dist]] [--select id [--focus dist]]
##        [--focus-job job[,dist]] [--focus-animal species[,dist]]
##        [--hide-ui] [--frames N] [--late-admin type:{json} [--late-frames N] [--late-ticks N] [--late-run]]

var renderer: WorldRenderer
var fx: FxRenderer
var spells: SpellRenderer
var chars: CharacterRenderer
var animals: AnimalRenderer
var rig: CameraRig
var hud: HUD
var sun: DirectionalLight3D
var env: Environment
var sky_mat: ProceduralSkyMaterial
var cloud_mats: Array[ShaderMaterial] = []
var _rain := 0.0  # 0 clear .. 1 in the rain (eased)
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
	fx = FxRenderer.new()
	add_child(fx)
	spells = SpellRenderer.new()
	add_child(spells)
	chars = CharacterRenderer.new()
	add_child(chars)
	animals = AnimalRenderer.new()
	add_child(animals)
	_build_brush()
	var ui_layer := CanvasLayer.new()
	add_child(ui_layer)
	hud = HUD.new()
	# Name tags live on the same 2D layer, under the cards.
	var tags := Control.new()
	tags.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	tags.mouse_filter = Control.MOUSE_FILTER_IGNORE
	ui_layer.add_child(tags)
	chars.overlay = tags
	ui_layer.add_child(hud)
	if _cli.has("hide-ui"):
		ui_layer.visible = false

	Game.game_started.connect(_on_world_ready)
	# Launched plainly: a fresh island behind the world menu. Scripted runs (any
	# command-line arguments) start straight away and never autosave.
	var interactive := _cli.is_empty()
	Game.autosave = interactive
	var seed := int(_cli.get("seed", "1")) if not interactive else randi_range(1, 999999)
	var config := {"seed": seed}
	if _cli.has("layout"):
		config["layout"] = String(_cli["layout"])
	if _cli.has("scenario"):
		config["scenario"] = String(_cli["scenario"])
	if _cli.has("era"):
		config["era"] = String(_cli["era"])
	if _cli.has("civs"):
		config["civs"] = int(_cli["civs"])
	if not Game.start_new_game(config):
		push_error("could not start game")
		return
	# A saved game (e.g. written by `icarus_cli run --save`) to look at instead.
	if _cli.has("load") and not Game.load_game(String(_cli["load"])):
		push_error("could not load " + String(_cli["load"]))
	var info: Dictionary = Game.sim.world_info()
	if _cli.has("cam"):
		var p: PackedStringArray = _cli["cam"].split(",")
		if p.size() >= 6:
			rig.set_view(Vector3(float(p[0]), float(p[1]), float(p[2])), float(p[3]), float(p[4]), float(p[5]))
	if interactive:
		hud.open_menu()
	hud.selection.focus_requested.connect(func(p: Vector3) -> void: rig.focus(p, 40.0))
	hud.selection.character_requested.connect(func(id: int) -> void: _select_character(id))
	hud.focus_requested.connect(func(p: Vector3) -> void: rig.focus(p, 45.0))
	hud.civ_card.girl_selected.connect(func(id: int) -> void:
		# The camera turns to her (she ends up in the middle): the card goes beside that.
		_select_character(id, get_viewport().get_visible_rect().size * 0.5)
		rig.focus(chars.position_of(id), 40.0))
	if _cli.has("ticks"):
		Game.sim.step(int(_cli["ticks"]))
	for cmd in _cli.get("admin", []):
		var parts: PackedStringArray = String(cmd).split(":", true, 1)
		var params: Variant = JSON.parse_string(parts[1]) if parts.size() > 1 else {}
		if parts[0] == "kill_spring":
			# Scenario shortcut: bury the spring under stone.
			var sp: Vector3i = info["features"]["spring"]
			Game.sim.admin("place", {"pos": [sp.x, sp.y, sp.z], "radius": 2.5, "material": "stone"})
			continue
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
		if _cli.has("focus"):
			# Screenshot helper: frame the selected character from this far away.
			for c in Game.sim.characters():
				if int(c["id"]) == int(_cli["select"]):
					rig.focus(c["pos"] + Vector3(0, 1.3, 0), float(_cli["focus"]), true)
	if _cli.has("focus-job"):
		# Screenshot helper: look at someone doing this work (e.g. chop, till, build).
		var parts: PackedStringArray = String(_cli["focus-job"]).split(",")
		var d := float(parts[1]) if parts.size() > 1 else 7.0
		# Someone at it right now, else someone on the way to it (run the clock on).
		var seen_jobs := {}
		for attempt in 40:
			var found := {}
			for c in Game.sim.characters():
				seen_jobs[String(c.get("job", ""))] = true
				if String(c.get("job", "")) == parts[0] and (c.get("working", false) or attempt == 39):
					found = c
					break
			if not found.is_empty():
				rig.focus(found["pos"] + Vector3(0, 1.2, 0), d, true)
				_select_character(int(found["id"]))
				break
			Game.sim.step(20)
		print("focus-job: jobs seen ", seen_jobs.keys())
	if _cli.has("focus-animal"):
		# Screenshot helper: look at the nearest animal of a kind (deer, wolf...).
		var parts2: PackedStringArray = String(_cli["focus-animal"]).split(",")
		var dist2 := float(parts2[1]) if parts2.size() > 1 else 9.0
		var best := {}
		var bd := 1e18
		for a in Game.sim.animals(rig.target, 2000.0):
			if String(a["species"]) != parts2[0] or not a["alive"]:
				continue
			var d := (a["pos"] as Vector3).distance_squared_to(rig.target)
			if d < bd:
				bd = d
				best = a
		if not best.is_empty():
			rig.focus(best["pos"] + Vector3(0, 0.6, 0), dist2, true)
			animals.focus = best["pos"]
	if _cli.has("focus-soldiers"):
		# Screenshot helper: look at the soldiers (select the first one).
		var sum := Vector3.ZERO
		var n := 0
		for c in Game.sim.characters():
			if c.get("drafted", false) and c["alive"]:
				if n == 0:
					_select_character(int(c["id"]))
				sum += c["pos"]
				n += 1
		if n > 0:
			var d := float(_cli["focus-soldiers"]) if String(_cli["focus-soldiers"]).is_valid_float() else 30.0
			rig.focus(sum / n, d, true)
	if _cli.has("council"):
		var cid := int(_cli["council"]) if String(_cli["council"]).is_valid_int() else 0
		hud.toggle_council(cid)
	if _cli.has("chronicle-find"):
		var needle := String(_cli["chronicle-find"])
		for ev in Game.sim.recent_events("", 0, 5000, 0):
			if String(ev["text"]).contains(needle):
				hud.toggle_chronicle(int(ev["id"]))
				break
	if _cli.has("chronicle"):
		var eid := int(_cli["chronicle"]) if String(_cli["chronicle"]).is_valid_int() else 0
		hud.toggle_chronicle(eid)
	if _cli.has("tool"):
		Game.set_tool(_cli["tool"])
	if _cli.has("tech"):
		hud.toggle_tech()
	if _cli.has("ending"):
		hud.show_ending()
	if _cli.has("menu"):
		hud.open_menu()
	if _cli.has("civ-detail"):
		hud.civ_card.expanded = true
	if _cli.has("shot"):
		_shot_path = _cli["shot"]
		_shot_frames = int(_cli.get("frames", "20"))
		Game.paused = true


## Screenshot helper: casts of the named spells (comma-separated effects) in a row in
## front of the camera, from the girls of the world if there are any.
func _spell_demo(list: String) -> void:
	var effects := list.split(",")
	var girls: Array = []
	for c in Game.sim.characters():
		if c.get("girl", false) and c["alive"]:
			girls.append(c)
	var right := rig.camera.global_transform.basis.x
	right.y = 0
	right = right.normalized()
	var fwd := -rig.camera.global_transform.basis.z
	fwd.y = 0
	fwd = fwd.normalized()
	var base := rig.target
	var n := effects.size()
	for i in n:
		var off := (float(i) - (n - 1) * 0.5) * 9.0
		var at := base + right * off
		var ground := Game.sim.raycast(at + Vector3(0, 40, 0), Vector3.DOWN, 80.0)
		if ground.get("hit", false):
			at.y = float(Vector3i(ground["cube"]).y) + 2.0
		var girl: Dictionary = girls[i % girls.size()] if not girls.is_empty() else {}
		var accent: Color = girl.get("accent", Color(0.9, 0.6, 1.0)) if not girl.is_empty() else Color(0.9, 0.6, 1.0)
		spells._spawn({"effect": effects[i], "name": effects[i], "drive": "", "caster": 0, "target": 0,
			"from": at - fwd * 4.0 + Vector3(0, 1.3, 0), "to": at + fwd * 3.0, "radius": 6.0, "color": accent})


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
			if key == "admin" or key == "late-admin":
				if not out.has(key):
					out[key] = []
				out[key].append(val)
			else:
				out[key] = val
		i += 1
	return out


func _build_environment() -> void:
	var we := WorldEnvironment.new()
	env = Environment.new()
	env.background_mode = Environment.BG_SKY
	var sky := Sky.new()
	# Daylight and weather change this sky continuously. Automatic selects the
	# incremental GGX filter, which produces black radiance on some Vulkan GPUs
	# (reproduced on RTX 5090); both reflections and aerial fog sample that map.
	sky.process_mode = Sky.PROCESS_MODE_REALTIME
	sky.radiance_size = Sky.RADIANCE_SIZE_256  # Required by the realtime filter.
	var sm := ProceduralSkyMaterial.new()
	sky_mat = sm
	sm.sky_top_color = Color(0.22, 0.42, 0.72)
	sm.sky_horizon_color = Color(0.72, 0.80, 0.90)
	sm.ground_bottom_color = Color(0.62, 0.68, 0.80)
	sm.ground_horizon_color = Color(0.78, 0.84, 0.92)
	sm.sun_angle_max = 20.0
	sm.sky_curve = 0.12
	sky.sky_material = sm
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.62, 0.7, 0.85)
	env.ambient_light_energy = 0.9
	env.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.tonemap_exposure = 1.08
	env.ssao_enabled = true
	env.ssao_radius = 1.6
	env.ssao_intensity = 1.6
	env.glow_enabled = true
	env.glow_intensity = 0.6
	env.glow_bloom = 0.05
	env.glow_hdr_threshold = 1.6
	# Distant land fades into the colour of the sky behind it.
	env.fog_enabled = true
	env.fog_light_color = Color(0.72, 0.80, 0.92)
	env.fog_density = 0.0006
	env.fog_aerial_perspective = 0.45
	env.fog_sun_scatter = 0.12
	env.fog_sky_affect = 0.0
	env.adjustment_enabled = true
	env.adjustment_saturation = 1.16
	env.adjustment_contrast = 1.04
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
	cloud_mats.append(cmat)
	clouds.position = Vector3(512, 28, 512)
	clouds.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(clouds)
	var clouds2 := clouds.duplicate() as MeshInstance3D
	clouds2.position = Vector3(512, 8, 512)
	var cmat2 := cmat.duplicate() as ShaderMaterial
	cmat2.set_shader_parameter("scale", 0.0026)
	cmat2.set_shader_parameter("speed", 0.0025)
	clouds2.material_override = cmat2
	cloud_mats.append(cmat2)
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
	renderer.focus = rig.target
	animals.focus = rig.target
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
	var raining: bool = ci.get("raining", false)
	_rain = (1.0 if raining else 0.0) if _shot_path != "" else move_toward(_rain, 1.0 if raining else 0.0, get_process_delta_time() * 0.25)
	# sun_h > 0 by day; lit: 0 at night .. 1 by day; dusk peaks at sunrise and sunset.
	var sun_h := sin((h - 6.0) / 12.0 * PI)
	var lit := smoothstep(-0.15, 0.35, sun_h)
	var dusk := 1.0 - smoothstep(0.0, 0.42, absf(sun_h))
	var t := (h - 6.0) / 12.0  # 0 at sunrise, 1 at sunset
	var elev := sin(clampf(t, 0.0, 1.0) * PI)
	if sun_h > -0.05:
		# The sun crosses the sky; low and warm at dawn and dusk.
		sun.rotation_degrees = Vector3(-lerpf(6.0, 62.0, elev), 38.0 + (t - 0.5) * 70.0, 0)
		sun.light_color = Color(1.0, 0.96, 0.88).lerp(Color(1.0, 0.6, 0.36), dusk)
		sun.light_energy = lerpf(0.35, 1.25, lit)
	else:
		# Moonlight: high, cool and soft, so the island stays readable at night.
		sun.rotation_degrees = Vector3(-55.0, -30.0, 0)
		sun.light_color = Color(0.62, 0.72, 1.0)
		sun.light_energy = 0.42
	var amb_night := Color(0.34, 0.4, 0.64)
	var amb_day := Color(0.62, 0.7, 0.85)
	env.ambient_light_color = amb_night.lerp(amb_day, lit).lerp(Color(0.85, 0.66, 0.6), dusk * 0.4)
	env.ambient_light_energy = lerpf(0.62, 0.9, lit)
	env.background_energy_multiplier = lerpf(0.55, 1.0, lit)
	env.adjustment_saturation = lerpf(0.72, 1.16, lit)  # colours fade by moonlight
	# The sky through the day: deep blue at night, warm at the horizon at dawn and
	# dusk, clear blue by day; distant haze and the sea of clouds take its colours.
	var top := Color(0.04, 0.07, 0.17).lerp(Color(0.22, 0.42, 0.72), lit).lerp(Color(0.25, 0.28, 0.52), dusk * 0.5)
	var horizon := Color(0.12, 0.16, 0.3).lerp(Color(0.72, 0.8, 0.9), lit).lerp(Color(0.98, 0.62, 0.42), dusk * 0.75)
	sky_mat.sky_top_color = top
	sky_mat.sky_horizon_color = horizon
	sky_mat.ground_horizon_color = horizon
	sky_mat.ground_bottom_color = top.lerp(horizon, 0.5)
	env.fog_light_color = horizon
	# Rain: an overcast grey sky, flat light, wet ground.
	if _rain > 0.0:
		var grey := Color(0.46, 0.5, 0.56) * lerpf(0.35, 1.0, lit)
		top = top.lerp(grey * 0.85, _rain * 0.8)
		horizon = horizon.lerp(grey, _rain * 0.8)
		sky_mat.sky_top_color = top
		sky_mat.sky_horizon_color = horizon
		sky_mat.ground_horizon_color = horizon
		env.fog_light_color = horizon
		sun.light_energy *= lerpf(1.0, 0.35, _rain)
		env.fog_density = lerpf(0.0006, 0.0022, _rain)
	else:
		env.fog_density = 0.0006
	renderer.set_wetness(_rain)
	fx.set_rain(_rain, rig.target)
	var cloud := Color(0.28, 0.32, 0.46).lerp(Color(0.93, 0.95, 1.0), lit).lerp(Color(1.0, 0.78, 0.66), dusk * 0.6)
	cloud = cloud.lerp(Color(0.52, 0.55, 0.6) * lerpf(0.5, 1.0, lit), _rain * 0.65)
	for cm in cloud_mats:
		cm.set_shader_parameter("cloud_color", cloud)
		cm.set_shader_parameter("shadow_color", cloud * Color(0.62, 0.66, 0.8))
	renderer.set_night(1.0 - lit)


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
	if tool in ["dig", "place", "meteor", "ignite", "flood", "miracle"]:
		var r: float = Game.tool_params.get("radius", 3.0)
		var center := Vector3(cube) + Vector3(0.5, 0.5, 0.5)
		if tool in ["place", "flood", "miracle"]:
			center += Vector3(hit["normal"])
		if tool == "miracle":
			var kind: String = Game.tool_params.get("miracle", "bless_food")
			r = 1.0 if kind == "bless_food" else (minf(r, 3.0) if kind == "smite" else r)
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
				if hud.overlay_open():
					hud.close_overlays()
				elif Game.current_tool != "inspect":
					Game.set_tool("inspect")
				else:
					hud.open_menu()
			KEY_J:
				hud.toggle_council()
			KEY_C:
				hud.toggle_chronicle()
			KEY_T:
				hud.toggle_tech()
	if event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
		_apply_tool()


## A new or loaded world: rebuild what is drawn and look at the village.
func _on_world_ready() -> void:
	var info: Dictionary = Game.sim.world_info()
	var v: Vector3i = info["features"]["village"]
	rig.set_view(Vector3(v) + Vector3(0, 2, 0), 35.0, 48.0, 150.0)
	renderer.focus = Vector3(v)
	renderer.setup(Game.sim, rig.camera)
	fx.setup(Game.sim, rig.camera)
	spells.setup(Game.sim, rig.camera)
	chars.setup(Game.sim, rig.camera)
	animals.focus = Vector3(v)
	animals.setup(Game.sim, rig.camera)
	chars.selected_id = -1
	Game.select({})
	hud.on_world_changed()


func _select_character(id: int, near := Vector2(-1, -1)) -> void:
	chars.selected_id = id
	hud.selection.show_character(id)
	# Beside the clicked figure, or beside where the figure is on screen.
	if near.x < 0.0:
		var p := chars.position_of(id) + Vector3(0, 1.5, 0)
		if not rig.camera.is_position_behind(p):
			near = rig.camera.unproject_position(p)
	hud.place_selection_near(near)
	Game.select({"kind": "character", "id": id})


func _apply_tool() -> void:
	if Game.current_tool == "inspect":
		var mp := get_viewport().get_mouse_position()
		var cid := chars.pick(mp)
		if cid >= 0:
			_select_character(cid, mp)
			return
		var aid := animals.pick(mp)
		if aid >= 0:
			chars.selected_id = -1
			hud.selection.show_animal(aid)
			hud.place_selection_near(mp)
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
		"miracle":
			var kind: String = Game.tool_params.get("miracle", "bless_food")
			if kind == "bless_food":
				Game.admin(kind, {"pos": cube + normal, "amount": int(Game.tool_params.get("amount", 40.0))})
			else:
				Game.admin(kind, {"pos": cube + normal, "radius": minf(r, 3.0) if kind == "smite" else r})
		"inspect":
			var b: Dictionary = Game.sim.building_at(cube)
			if not b.is_empty():
				chars.selected_id = -1
				hud.selection.show_building(b["id"], cube)
				hud.place_selection_near(get_viewport().get_mouse_position())
			else:
				chars.selected_id = -1
				hud.selection.visible = false


func _screenshot_step() -> void:
	# Wait for the world to be meshed once; after that the countdown runs even while
	# late effects keep changing it.
	if _shot_wait == 0 and renderer.pending_count() > 0:
		return
	_shot_wait += 1
	# Effects to catch in the act: applied a few frames before the capture.
	if _shot_wait == maxi(1, _shot_frames - int(_cli.get("late-frames", "6"))):
		for cmd in _cli.get("late-admin", []):
			var parts: PackedStringArray = String(cmd).split(":", true, 1)
			var params: Variant = JSON.parse_string(parts[1]) if parts.size() > 1 else {}
			Game.sim.admin(parts[0], params if params is Dictionary else {})
		if _cli.has("late-admin"):
			Game.sim.step(int(_cli.get("late-ticks", "2")))
		if _cli.has("late-run"):
			Game.paused = false  # let the world move for the last frames
		if _cli.has("spell-demo"):
			_spell_demo(String(_cli["spell-demo"]))
	if _shot_wait < _shot_frames:
		return
	var img := get_viewport().get_texture().get_image()
	var err := img.save_png(_shot_path)
	print("screenshot saved: ", _shot_path, " err=", err)
	get_tree().quit(0 if err == OK else 1)
