class_name FxRenderer
extends Node3D
## Short-lived effects of the physics: chips flying from broken cubes (in the cube's
## colour), dust where something lands or is built, flames and smoke over burning
## cubes. Pure presentation: reads the kernel's effect feed and fire list, never
## changes the world.

const CHIP_POOL := 28
const DUST_POOL := 24
const FIRE_POOL := 12
const MAX_BURSTS_PER_FRAME := 10
const MAX_DISTANCE := 170.0

var sim: IcarusSim
var camera: Camera3D

var _colors := PackedColorArray()
var _chips: Array[GPUParticles3D] = []
var _dust: Array[GPUParticles3D] = []
var _flames: Array[GPUParticles3D] = []
var _smoke: Array[GPUParticles3D] = []
var _next_chip := 0
var _next_dust := 0
var _fire_timer := 0.0
var _soft: Texture2D
var _collider: GPUParticlesCollisionHeightField3D
var _rain: GPUParticles3D


func setup(s: IcarusSim, cam: Camera3D) -> void:
	sim = s
	camera = cam
	_colors.clear()
	for m in sim.material_table():
		_colors.append(m["color"])
	sim.take_fx()  # nothing from before the world was shown
	if _chips.is_empty():
		_build_pools()


func _build_pools() -> void:
	_soft = _soft_dot()
	# Chips and dust bounce off the ground near the camera instead of sinking into it.
	_collider = GPUParticlesCollisionHeightField3D.new()
	_collider.size = Vector3(120, 80, 120)
	_collider.resolution = GPUParticlesCollisionHeightField3D.RESOLUTION_256
	_collider.update_mode = GPUParticlesCollisionHeightField3D.UPDATE_MODE_WHEN_MOVED
	_collider.follow_camera_enabled = true
	add_child(_collider)
	for i in CHIP_POOL:
		_chips.append(_make_chips())
	for i in DUST_POOL:
		_dust.append(_make_dust())
	for i in FIRE_POOL:
		_flames.append(_make_flames())
		_smoke.append(_make_smoke())
	_rain = _make_rain()


func _process(delta: float) -> void:
	if sim == null or not sim.has_game():
		return
	_spawn_bursts()
	_fire_timer -= delta
	if _fire_timer <= 0.0:
		_fire_timer = 0.25
		_place_fires()


## Rain around where the camera looks, heavier as the shower builds.
func set_rain(amount: float, focus: Vector3) -> void:
	if _rain == null:
		return
	_rain.emitting = amount > 0.05
	_rain.amount_ratio = clampf(amount, 0.0, 1.0)
	_rain.global_position = focus + Vector3(0, 30, 0)


# ------------------------------------------------------------------ bursts

func _spawn_bursts() -> void:
	var fx: PackedInt32Array = sim.take_fx()
	if fx.is_empty():
		return
	var cam := camera.global_position if camera else Vector3.ZERO
	# Group by 3-cube buckets, so a meteor crater makes a few big bursts rather than
	# hundreds of small ones.
	var buckets := {}
	for i in range(0, fx.size(), 5):
		var p := Vector3(fx[i + 1], fx[i + 2], fx[i + 3]) + Vector3(0.5, 0.5, 0.5)
		if p.distance_to(cam) > MAX_DISTANCE:
			continue
		var key := Vector3i(floori(p.x / 3.0), floori(p.y / 3.0), floori(p.z / 3.0) * 2 + fx[i])
		if not buckets.has(key):
			buckets[key] = [fx[i], p, fx[i + 4], 0]
		buckets[key][3] += 1
	var n := 0
	for b in buckets.values():
		if n >= MAX_BURSTS_PER_FRAME:
			break
		var col: Color = _colors[b[2]] if b[2] < _colors.size() else Color(0.5, 0.5, 0.5)
		if b[0] == 0:
			_burst_chips(b[1], col, b[3])
			if b[3] > 2:
				_burst_dust(b[1], col, b[3])
		else:
			_burst_dust(b[1], col, b[3])
		n += 1


func _burst_chips(p: Vector3, col: Color, count: int) -> void:
	var e := _chips[_next_chip]
	_next_chip = (_next_chip + 1) % _chips.size()
	e.global_position = p
	e.amount = clampi(6 + count * 3, 6, 40)
	(e.process_material as ParticleProcessMaterial).color = col
	e.restart()


func _burst_dust(p: Vector3, col: Color, count: int) -> void:
	var e := _dust[_next_dust]
	_next_dust = (_next_dust + 1) % _dust.size()
	e.global_position = p - Vector3(0, 0.3, 0)
	e.amount = clampi(4 + count * 2, 4, 32)
	var pm := e.process_material as ParticleProcessMaterial
	pm.color = col.lerp(Color(0.72, 0.68, 0.62), 0.55)
	pm.emission_sphere_radius = clampf(0.4 + float(count) * 0.12, 0.4, 2.5)
	e.restart()


# ------------------------------------------------------------------ fire

func _place_fires() -> void:
	var spots: PackedVector3Array = sim.fire_spots(400)
	var cam := camera.global_position if camera else Vector3.ZERO
	var chosen: Array[Vector3] = []
	var list := Array(spots)
	list.sort_custom(func(a: Vector3, b: Vector3) -> bool: return a.distance_squared_to(cam) < b.distance_squared_to(cam))
	for p in list:
		if chosen.size() >= FIRE_POOL or p.distance_to(cam) > MAX_DISTANCE:
			break
		var near := false
		for q in chosen:
			if q.distance_to(p) < 1.8:
				near = true
				break
		if not near:
			chosen.append(p)
	for i in FIRE_POOL:
		var on := i < chosen.size()
		if on:
			_flames[i].global_position = chosen[i]
			_smoke[i].global_position = chosen[i] + Vector3(0, 0.6, 0)
		_flames[i].emitting = on
		_smoke[i].emitting = on


# ------------------------------------------------------------------ emitters

func _make_chips() -> GPUParticles3D:
	var e := GPUParticles3D.new()
	e.one_shot = true
	e.emitting = false
	e.lifetime = 1.4
	e.explosiveness = 0.95
	e.local_coords = false
	e.collision_base_size = 0.08
	e.visibility_aabb = AABB(Vector3(-6, -6, -6), Vector3(12, 12, 12))
	var pm := ParticleProcessMaterial.new()
	pm.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_BOX
	pm.emission_box_extents = Vector3(0.45, 0.45, 0.45)
	pm.direction = Vector3(0, 1, 0)
	pm.spread = 65.0
	pm.initial_velocity_min = 1.8
	pm.initial_velocity_max = 4.2
	pm.gravity = Vector3(0, -15, 0)
	pm.scale_min = 0.55
	pm.scale_max = 1.25
	pm.scale_curve = _curve([Vector2(0, 1), Vector2(0.75, 0.9), Vector2(1, 0)])
	pm.collision_mode = ParticleProcessMaterial.COLLISION_RIGID
	pm.collision_bounce = 0.25
	pm.collision_friction = 0.7
	e.process_material = pm
	var mesh := BoxMesh.new()
	mesh.size = Vector3(0.13, 0.13, 0.13)
	var mat := StandardMaterial3D.new()
	mat.vertex_color_use_as_albedo = true
	mat.roughness = 0.9
	mesh.material = mat
	e.draw_pass_1 = mesh
	add_child(e)
	return e


func _make_dust() -> GPUParticles3D:
	var e := GPUParticles3D.new()
	e.one_shot = true
	e.emitting = false
	e.lifetime = 1.8
	e.explosiveness = 0.9
	e.local_coords = false
	e.visibility_aabb = AABB(Vector3(-6, -3, -6), Vector3(12, 10, 12))
	var pm := ParticleProcessMaterial.new()
	pm.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE
	pm.emission_sphere_radius = 0.5
	pm.direction = Vector3(0, 1, 0)
	pm.spread = 180.0
	pm.initial_velocity_min = 0.4
	pm.initial_velocity_max = 1.4
	pm.gravity = Vector3(0, 0.25, 0)
	pm.damping_min = 1.2
	pm.damping_max = 2.0
	pm.scale_min = 1.0
	pm.scale_max = 2.0
	pm.scale_curve = _curve([Vector2(0, 0.45), Vector2(1, 1)])
	pm.color_ramp = _ramp([Color(1, 1, 1, 0.0), Color(1, 1, 1, 0.55), Color(1, 1, 1, 0.0)], [0.0, 0.12, 1.0])
	e.process_material = pm
	e.draw_pass_1 = _billboard(0.9, false)
	add_child(e)
	return e


func _make_flames() -> GPUParticles3D:
	var e := GPUParticles3D.new()
	e.amount = 26
	e.lifetime = 0.7
	e.local_coords = false
	e.emitting = false
	var pm := ParticleProcessMaterial.new()
	pm.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE
	pm.emission_sphere_radius = 0.55
	pm.direction = Vector3(0, 1, 0)
	pm.spread = 14.0
	pm.initial_velocity_min = 1.0
	pm.initial_velocity_max = 2.2
	pm.gravity = Vector3(0, 3.0, 0)
	pm.scale_min = 0.7
	pm.scale_max = 1.3
	pm.scale_curve = _curve([Vector2(0, 1), Vector2(1, 0.1)])
	pm.color_ramp = _ramp([Color(1.0, 0.92, 0.5, 0.9), Color(1.0, 0.5, 0.12, 0.8), Color(0.7, 0.12, 0.04, 0.0)], [0.0, 0.45, 1.0])
	e.process_material = pm
	e.draw_pass_1 = _billboard(0.95, true)
	add_child(e)
	var light := OmniLight3D.new()
	light.light_color = Color(1.0, 0.55, 0.2)
	light.light_energy = 1.6
	light.omni_range = 6.0
	light.shadow_enabled = false
	e.add_child(light)
	return e


func _make_smoke() -> GPUParticles3D:
	var e := GPUParticles3D.new()
	e.amount = 22
	e.lifetime = 4.0
	e.local_coords = false
	e.emitting = false
	e.visibility_aabb = AABB(Vector3(-6, -2, -6), Vector3(12, 16, 12))
	var pm := ParticleProcessMaterial.new()
	pm.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE
	pm.emission_sphere_radius = 0.4
	pm.direction = Vector3(0, 1, 0)
	pm.spread = 18.0
	pm.initial_velocity_min = 0.9
	pm.initial_velocity_max = 1.6
	pm.gravity = Vector3(0.35, 0.4, 0.1)  # a little drift with the wind
	pm.damping_min = 0.2
	pm.damping_max = 0.5
	pm.scale_min = 0.9
	pm.scale_max = 1.5
	pm.scale_curve = _curve([Vector2(0, 0.7), Vector2(1, 3.8)])
	pm.color = Color(0.2, 0.19, 0.19)
	pm.color_ramp = _ramp([Color(1, 1, 1, 0.0), Color(1, 1, 1, 0.72), Color(1, 1, 1, 0.0)], [0.0, 0.12, 1.0])
	e.process_material = pm
	e.draw_pass_1 = _billboard(1.5, false)
	add_child(e)
	return e


func _make_rain() -> GPUParticles3D:
	var e := GPUParticles3D.new()
	e.amount = 4000
	e.lifetime = 1.4
	e.local_coords = false
	e.emitting = false
	e.visibility_aabb = AABB(Vector3(-60, -60, -60), Vector3(120, 90, 120))
	var pm := ParticleProcessMaterial.new()
	pm.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_BOX
	pm.emission_box_extents = Vector3(55, 1, 55)
	pm.direction = Vector3(0.08, -1, 0.04)
	pm.spread = 2.0
	pm.initial_velocity_min = 26.0
	pm.initial_velocity_max = 32.0
	pm.gravity = Vector3(0, -10, 0)
	pm.collision_mode = ParticleProcessMaterial.COLLISION_HIDE_ON_CONTACT
	pm.color = Color(0.75, 0.82, 0.95, 0.5)
	e.process_material = pm
	var q := QuadMesh.new()
	q.size = Vector2(0.045, 1.0)
	var m := StandardMaterial3D.new()
	m.billboard_mode = BaseMaterial3D.BILLBOARD_FIXED_Y
	m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.vertex_color_use_as_albedo = true
	m.albedo_color = Color(1, 1, 1, 0.55)
	q.material = m
	e.draw_pass_1 = q
	add_child(e)
	return e


# ------------------------------------------------------------------ helpers

func _billboard(size: float, additive: bool) -> QuadMesh:
	var q := QuadMesh.new()
	q.size = Vector2(size, size)
	var m := StandardMaterial3D.new()
	m.billboard_mode = BaseMaterial3D.BILLBOARD_PARTICLES
	m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	m.vertex_color_use_as_albedo = true
	m.albedo_texture = _soft
	m.cull_mode = BaseMaterial3D.CULL_DISABLED
	if additive:
		m.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
		m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	else:
		m.shading_mode = BaseMaterial3D.SHADING_MODE_PER_VERTEX
	q.material = m
	return q


func _curve(points: Array) -> CurveTexture:
	var c := Curve.new()
	c.max_value = 4.0
	for p in points:
		c.add_point(p)
	var t := CurveTexture.new()
	t.curve = c
	return t


func _ramp(colors: Array, offsets: Array) -> GradientTexture1D:
	var g := Gradient.new()
	g.colors = PackedColorArray(colors)
	g.offsets = PackedFloat32Array(offsets)
	var t := GradientTexture1D.new()
	t.gradient = g
	return t


func _soft_dot() -> ImageTexture:
	var s := 32
	var img := Image.create_empty(s, s, false, Image.FORMAT_RGBA8)
	for y in s:
		for x in s:
			var d := Vector2(x + 0.5 - s * 0.5, y + 0.5 - s * 0.5).length() / (s * 0.5)
			var a := clampf(1.0 - d, 0.0, 1.0)
			img.set_pixel(x, y, Color(1, 1, 1, a * a * (3.0 - 2.0 * a)))
	return ImageTexture.create_from_image(img)
