class_name SpellRenderer
extends Node3D
## Magic made visible. Every cast in the kernel's spell feed becomes a short effect in the
## caster's colours: the spell's name rising over her head, light streaming, bolts flying,
## rings spreading over the ground, sparks, embers and motes. Pure presentation: reads the
## feed, never changes the world.

const MAX_LIVE := 40
const MAX_DISTANCE := 220.0
const GLOW := 1.9  # colour multiplier for rings, pillars and slashes (the scene's glow starts at 1.6)
const SPARK := 1.35  # ...and for particles (alpha-blended, so they keep their hue in daylight)

var sim: IcarusSim
var camera: Camera3D

var _soft: Texture2D
var _ring_tex: Texture2D
var _slash_tex: Texture2D
var _live: Array = []  # {node, age, life, tick: Callable}


func setup(s: IcarusSim, cam: Camera3D) -> void:
	sim = s
	camera = cam
	if _soft == null:
		_soft = _make_soft()
		_ring_tex = _make_ring()
		_slash_tex = _make_slash()
	for e in _live:
		e["node"].queue_free()
	_live.clear()
	sim.take_spells()  # nothing from before the world was shown


func _process(delta: float) -> void:
	if sim == null or not sim.has_game():
		return
	var cam := camera.global_position if camera else Vector3.ZERO
	for sp in sim.take_spells():
		var d: Dictionary = sp
		var near: Vector3 = d["to"]
		if near.distance_to(cam) > MAX_DISTANCE and Vector3(d["from"]).distance_to(cam) > MAX_DISTANCE:
			continue
		if _live.size() < MAX_LIVE:
			_spawn(d)
	var keep: Array = []
	for e in _live:
		e["age"] += delta
		var t: float = e["age"] / e["life"]
		if t >= 1.0:
			e["node"].queue_free()
			continue
		var cb: Callable = e["tick"]
		if cb.is_valid():
			cb.call(clampf(t, 0.0, 1.0), delta)
		keep.append(e)
	_live = keep


# ------------------------------------------------------------------ spells

func _spawn(d: Dictionary) -> void:
	var accent: Color = _vivid(d.get("color", Color(0.9, 0.85, 1.0)))
	var from: Vector3 = d["from"]
	var to: Vector3 = d["to"]
	var radius: float = d.get("radius", 0.0)
	var effect := String(d["effect"])
	_name_label(from + Vector3(0, 1.0, 0), String(d["name"]), accent)
	_flash(from, accent, 1.2, 5.0, 0.5)
	_burst(from, accent, 16, 1.2, 180.0, Vector3(0, 1.5, 0), 0.7, 0.18, 0.5)
	match effect:
		"heal":
			var gold := Color(1.0, 0.88, 0.45).lerp(accent, 0.35)
			_stream(from, to, gold, 26, 0.6)
			_pillar(to - Vector3(0, 1.0, 0), gold, 3.2, 1.6)
			_ring(to - Vector3(0, 0.95, 0), gold, 2.2, 1.2)
			_burst(to, gold.lightened(0.4), 36, 1.0, 40.0, Vector3(0, 2.2, 0), 1.4, 0.16, 0.8)
			_flash(to, gold, 1.8, 6.0, 1.0)
		"quench":
			var white := Color(0.92, 0.97, 1.0)
			_ring(to, white, maxf(radius, 7.0), 1.1)
			_burst(to, Color(0.85, 0.88, 0.92, 0.7), 40, 2.5, 60.0, Vector3(0, 1.8, 0), 2.0, 0.6, 3.0, false)
			_flash(to, white, 3.0, 14.0, 0.7)
		"strike":
			_slash(to, from, accent.lerp(Color(1, 0.95, 0.8), 0.4), 1.8)
			_burst(to, accent.lightened(0.2), 28, 5.0, 70.0, Vector3(0, -6, 0), 0.5, 0.12, 0.3)
			_flash(to, accent, 2.2, 6.0, 0.35)
		"ranged":
			var bolt := accent.lerp(Color(1, 1, 0.9), 0.5)
			_projectile(from, to, bolt, 38.0, 0.0, 0.22, func(p: Vector3) -> void:
				_burst(p, bolt, 22, 4.0, 180.0, Vector3(0, -4, 0), 0.4, 0.1, 0.2)
				_flash(p, bolt, 2.0, 6.0, 0.3))
		"firebomb":
			var fire := Color(1.0, 0.55, 0.15)
			_projectile(from, to, fire, 16.0, 2.5, 0.45, func(p: Vector3) -> void:
				_flash(p, fire, 5.0, 14.0, 0.6)
				_ring(p - Vector3(0, 0.8, 0), fire, maxf(radius, 3.0) + 1.0, 0.6)
				_burst(p, Color(1.0, 0.75, 0.3), 50, 7.0, 180.0, Vector3(0, -5, 0), 0.9, 0.3, 0.6)
				_burst(p, Color(0.25, 0.22, 0.2, 0.8), 24, 1.5, 60.0, Vector3(0, 1.5, 0), 2.2, 0.8, 1.0, false))
		"drain_strike":
			var blood := Color(0.75, 0.08, 0.12).lerp(accent, 0.25)
			_burst(to, blood, 26, 3.5, 180.0, Vector3(0, -3, 0), 0.5, 0.14, 0.3)
			_stream(to, from, blood.lightened(0.2), 30, 0.7)
			_flash(to, blood, 1.6, 5.0, 0.4)
		"drain_mana":
			var mana := Color(0.55, 0.95, 0.6).lerp(accent, 0.4)
			_stream(to, from, mana, 40, 0.9)
			_burst(from, mana.lightened(0.3), 20, 1.0, 180.0, Vector3(0, 0.5, 0), 1.0, 0.14, 0.6)
		"terrify":
			var dread := Color(0.35, 0.22, 0.55).lerp(accent, 0.3)
			_ring(to - Vector3(0, 0.8, 0), dread, maxf(radius, 8.0), 1.4)
			_burst(to, Color(0.12, 0.08, 0.18, 0.85), 48, 1.2, 180.0, Vector3(0, 1.2, 0), 2.4, 0.9, maxf(radius, 6.0) * 0.6, false)
			_flash(to, dread, 1.5, 12.0, 1.2)
		"rally":
			var gold := Color(1.0, 0.82, 0.35).lerp(accent, 0.3)
			_ring(from - Vector3(0, 1.2, 0), gold, maxf(radius, 10.0), 1.3)
			_burst(from, gold, 60, 3.0, 25.0, Vector3(0, 3.0, 0), 1.2, 0.16, 1.2)
			_flash(from, gold, 3.0, 16.0, 0.9)
		"berserk":
			var rage := Color(1.0, 0.18, 0.08).lerp(accent, 0.2)
			_aura(from - Vector3(0, 0.6, 0), rage, 2.5)
			_ring(from - Vector3(0, 1.2, 0), rage, 3.5, 0.7)
			_flash(from, rage, 2.4, 7.0, 1.5)
		"discord":
			var doubt := Color(0.62, 0.3, 0.75).lerp(accent, 0.3)
			_ring(to - Vector3(0, 0.8, 0), doubt, maxf(radius, 8.0) * 0.8, 1.6)
			_burst(to, doubt, 40, 1.6, 180.0, Vector3(0, 0.6, 0), 2.0, 0.2, maxf(radius, 6.0) * 0.5)
		"wither":
			var dust := Color(0.5, 0.42, 0.3)
			_ring(to, Color(0.35, 0.3, 0.25), maxf(radius, 6.0), 1.6)
			_burst(to + Vector3(0, 2.5, 0), dust, 60, 0.6, 180.0, Vector3(0, -1.2, 0), 2.4, 0.22, maxf(radius, 6.0) * 0.8, false)
		"devour":
			var maw := Color(0.55, 0.12, 0.45).lerp(accent, 0.3)
			_implode(to, maw, maxf(radius, 2.5) + 1.0, 1.2)
			_flash(to, maw, 2.0, 7.0, 1.0)
		"feast":
			var warm := Color(1.0, 0.78, 0.4)
			_burst(to, warm, 50, 1.4, 60.0, Vector3(0, 1.4, 0), 1.8, 0.18, 2.0)
			_flash(to, warm, 2.0, 10.0, 1.2)
		"grow":
			var green := Color(0.5, 0.95, 0.4).lerp(accent, 0.2)
			_ring(to - Vector3(0, 0.5, 0), green, maxf(radius, 8.0), 1.8)
			_burst(to, green, 90, 1.2, 30.0, Vector3(0, 1.6, 0), 1.8, 0.16, maxf(radius, 8.0) * 0.8)
		"inspire":
			var song := Color(0.85, 0.95, 1.0).lerp(accent, 0.4)
			_ring(to - Vector3(0, 0.9, 0), song, maxf(radius, 10.0), 2.0)
			_burst(to, song, 70, 1.0, 50.0, Vector3(0, 1.0, 0), 2.6, 0.2, maxf(radius, 8.0) * 0.6)
			_flash(to, song, 1.6, 14.0, 1.6)
		_:
			_burst(to, accent, 30, 2.0, 180.0, Vector3(0, 1, 0), 1.0, 0.16, 0.6)


# ------------------------------------------------------------------ building blocks

func _track(node: Node3D, life: float, tick: Callable = Callable()) -> void:
	_live.append({"node": node, "age": 0.0, "life": life, "tick": tick})


## The spell's name over the caster, rising and fading.
func _name_label(p: Vector3, text: String, col: Color) -> void:
	if text == "":
		return
	var l := Label3D.new()
	l.text = "「%s」" % text
	l.font = UITheme.font_bold()
	l.font_size = 30
	l.pixel_size = 0.0011
	l.outline_size = 9
	l.outline_modulate = Color(0.08, 0.06, 0.12, 0.85)
	l.modulate = col.lightened(0.35)
	l.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	l.no_depth_test = true
	l.fixed_size = true
	l.render_priority = 4
	add_child(l)
	l.global_position = p
	_track(l, 2.2, func(t: float, _dt: float) -> void:
		l.global_position = p + Vector3(0, t * 1.2, 0)
		l.modulate.a = clampf(1.6 - t * 1.6, 0.0, 1.0) if t > 0.1 else t * 10.0
		l.outline_modulate.a = l.modulate.a * 0.85)


func _flash(p: Vector3, col: Color, energy: float, rng: float, life: float) -> void:
	var o := OmniLight3D.new()
	o.light_color = col
	o.light_energy = energy
	o.omni_range = rng
	o.shadow_enabled = false
	add_child(o)
	o.global_position = p
	_track(o, life, func(t: float, _dt: float) -> void:
		o.light_energy = energy * (1.0 - t) * (1.0 - t))


## A one-shot spray of glowing (or smoky) particles.
func _burst(p: Vector3, col: Color, amount: int, speed: float, spread: float, gravity: Vector3, life: float,
		size: float, area: float, additive: bool = true) -> void:
	var e := GPUParticles3D.new()
	e.amount = amount
	e.lifetime = life
	e.one_shot = true
	e.explosiveness = 0.85
	e.local_coords = false
	e.visibility_aabb = AABB(Vector3(-20, -20, -20), Vector3(40, 40, 40))
	var pm := ParticleProcessMaterial.new()
	pm.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE
	pm.emission_sphere_radius = maxf(0.05, area)
	pm.direction = Vector3(0, 1, 0)
	pm.spread = spread
	pm.initial_velocity_min = speed * 0.5
	pm.initial_velocity_max = speed
	pm.gravity = gravity
	pm.damping_min = 0.5
	pm.damping_max = 1.5
	pm.scale_min = 0.6
	pm.scale_max = 1.3
	pm.scale_curve = _curve([Vector2(0, 0.4), Vector2(0.2, 1.0), Vector2(1, 0.0)])
	var c0 := _hdr(_vivid(col), SPARK) if additive else col  # `additive` false: smoke and dust
	var c1 := c0
	c1.a = 0.0
	pm.color_ramp = _ramp([c0, c0, c1], [0.0, 0.6, 1.0])
	e.process_material = pm
	e.draw_pass_1 = _billboard(size * 3.4, false)
	add_child(e)
	e.global_position = p
	e.emitting = true
	_track(e, life + 0.3)


## Light flowing from one point to another (healing, draining).
func _stream(a: Vector3, b: Vector3, col: Color, amount: int, life: float) -> void:
	var e := GPUParticles3D.new()
	var dist := a.distance_to(b)
	e.amount = amount
	e.lifetime = life
	e.one_shot = true
	e.explosiveness = 0.2
	e.local_coords = false
	e.visibility_aabb = AABB(Vector3(-30, -30, -30), Vector3(60, 60, 60))
	var pm := ParticleProcessMaterial.new()
	pm.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE
	pm.emission_sphere_radius = 0.25
	pm.direction = (b - a).normalized() if dist > 0.01 else Vector3.UP
	pm.spread = 6.0
	pm.initial_velocity_min = dist / life * 0.9
	pm.initial_velocity_max = dist / life * 1.05
	pm.gravity = Vector3.ZERO
	pm.scale_curve = _curve([Vector2(0, 0.5), Vector2(0.5, 1.0), Vector2(1, 0.3)])
	var c1 := _hdr(_vivid(col), SPARK)
	c1.a = 0.0
	pm.color_ramp = _ramp([_hdr(_vivid(col), SPARK), _hdr(_vivid(col), SPARK), c1], [0.0, 0.7, 1.0])
	e.process_material = pm
	e.draw_pass_1 = _billboard(0.45, false)
	add_child(e)
	e.global_position = a
	e.emitting = true
	_track(e, life * 2.0 + 0.2)


## A ring of light spreading over the ground.
func _ring(p: Vector3, col: Color, radius: float, life: float) -> void:
	var m := MeshInstance3D.new()
	var q := QuadMesh.new()
	q.size = Vector2(2, 2)
	q.orientation = PlaneMesh.FACE_Y
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	mat.albedo_texture = _ring_tex
	mat.albedo_color = col
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.no_depth_test = false
	q.material = mat
	m.mesh = q
	m.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(m)
	m.global_position = p + Vector3(0, 0.08, 0)
	_track(m, life, func(t: float, _dt: float) -> void:
		var r := lerpf(0.4, radius, 1.0 - pow(1.0 - t, 2.5))
		m.scale = Vector3(r, 1, r)
		mat.albedo_color = Color(col.r * GLOW, col.g * GLOW, col.b * GLOW, (1.0 - t) * 0.95))


## A column of light (healing).
func _pillar(p: Vector3, col: Color, height: float, life: float) -> void:
	var m := MeshInstance3D.new()
	var cyl := CylinderMesh.new()
	cyl.top_radius = 0.55
	cyl.bottom_radius = 0.7
	cyl.height = height
	cyl.cap_top = false
	cyl.cap_bottom = false
	cyl.radial_segments = 20
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.albedo_texture = _vertical_fade()
	mat.albedo_color = col
	cyl.material = mat
	m.mesh = cyl
	m.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(m)
	m.global_position = p + Vector3(0, height * 0.5, 0)
	_track(m, life, func(t: float, dt: float) -> void:
		m.rotate_y(dt * 1.5)
		var a := sin(t * PI)
		mat.albedo_color = Color(col.r * GLOW, col.g * GLOW, col.b * GLOW, a * 0.8)
		m.scale = Vector3(1.0 - 0.3 * t, 1, 1.0 - 0.3 * t))


## A crescent of light cutting across the target (sword magic).
func _slash(p: Vector3, from: Vector3, col: Color, size: float) -> void:
	var m := MeshInstance3D.new()
	var q := QuadMesh.new()
	q.size = Vector2(size * 1.6, size)
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.albedo_texture = _slash_tex
	mat.albedo_color = col
	q.material = mat
	m.mesh = q
	m.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(m)
	m.global_position = p
	var dir := (p - from)
	dir.y = 0
	if dir.length() > 0.01:
		m.look_at(p + dir.normalized(), Vector3.UP)
	m.rotate_object_local(Vector3(0, 0, 1), randf_range(-0.9, 0.9))
	_track(m, 0.35, func(t: float, _dt: float) -> void:
		m.scale = Vector3.ONE * (0.6 + t * 0.8)
		mat.albedo_color = Color(col.r * GLOW, col.g * GLOW, col.b * GLOW, 1.0 - t))


## A glowing bolt flying from caster to target, with a trail; `arc` lifts its path.
func _projectile(a: Vector3, b: Vector3, col: Color, speed: float, arc: float, size: float, on_hit: Callable) -> void:
	var holder := Node3D.new()
	add_child(holder)
	var ball := MeshInstance3D.new()
	var sph := SphereMesh.new()
	sph.radius = size
	sph.height = size * 2.0
	sph.radial_segments = 12
	sph.rings = 6
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = _hdr(col, 1.8)
	sph.material = mat
	ball.mesh = sph
	holder.add_child(ball)
	var light := OmniLight3D.new()
	light.light_color = col
	light.light_energy = 1.5
	light.omni_range = 4.0
	holder.add_child(light)
	var trail := GPUParticles3D.new()
	trail.amount = 40
	trail.lifetime = 0.35
	trail.local_coords = false
	trail.visibility_aabb = AABB(Vector3(-30, -30, -30), Vector3(60, 60, 60))
	var pm := ParticleProcessMaterial.new()
	pm.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE
	pm.emission_sphere_radius = size * 0.6
	pm.gravity = Vector3(0, 0.5, 0)
	pm.initial_velocity_min = 0.0
	pm.initial_velocity_max = 0.4
	pm.scale_curve = _curve([Vector2(0, 1.0), Vector2(1, 0.0)])
	var c1 := _hdr(col, SPARK)
	c1.a = 0.0
	pm.color_ramp = _ramp([_hdr(col.lightened(0.2), SPARK), c1], [0.0, 1.0])
	trail.process_material = pm
	trail.draw_pass_1 = _billboard(size * 3.0, false)
	holder.add_child(trail)
	holder.global_position = a
	var flight := maxf(0.08, a.distance_to(b) / speed)
	var state := {"hit": false}
	_track(holder, flight + 0.4, func(t: float, _dt: float) -> void:
		var age := t * (flight + 0.4)
		var k := clampf(age / flight, 0.0, 1.0)
		if k < 1.0:
			holder.global_position = a.lerp(b, k) + Vector3(0, arc * 4.0 * k * (1.0 - k), 0)
		elif not state["hit"]:
			state["hit"] = true
			holder.global_position = b
			ball.visible = false
			light.visible = false
			trail.emitting = false
			on_hit.call(b))


## Flames of rage around the caster.
func _aura(p: Vector3, col: Color, life: float) -> void:
	var e := GPUParticles3D.new()
	e.amount = 60
	e.lifetime = 0.6
	e.local_coords = false
	e.visibility_aabb = AABB(Vector3(-6, -3, -6), Vector3(12, 10, 12))
	var pm := ParticleProcessMaterial.new()
	pm.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_RING
	pm.emission_ring_axis = Vector3.UP
	pm.emission_ring_height = 0.3
	pm.emission_ring_radius = 0.7
	pm.emission_ring_inner_radius = 0.5
	pm.direction = Vector3(0, 1, 0)
	pm.spread = 10.0
	pm.initial_velocity_min = 1.8
	pm.initial_velocity_max = 3.2
	pm.gravity = Vector3(0, 1.5, 0)
	pm.scale_curve = _curve([Vector2(0, 1.0), Vector2(1, 0.1)])
	var c1 := _hdr(col, SPARK)
	c1.a = 0.0
	pm.color_ramp = _ramp([_hdr(col.lightened(0.4), SPARK), _hdr(col, SPARK), c1], [0.0, 0.4, 1.0])
	e.process_material = pm
	e.draw_pass_1 = _billboard(0.6, false)
	add_child(e)
	e.global_position = p
	e.emitting = true
	_track(e, life, func(t: float, _dt: float) -> void:
		if t > 0.75:
			e.emitting = false)


## Matter pulled in to a point (devouring).
func _implode(p: Vector3, col: Color, radius: float, life: float) -> void:
	var e := GPUParticles3D.new()
	e.amount = 70
	e.lifetime = 0.7
	e.local_coords = false
	e.visibility_aabb = AABB(Vector3(-8, -8, -8), Vector3(16, 16, 16))
	var pm := ParticleProcessMaterial.new()
	pm.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE_SURFACE
	pm.emission_sphere_radius = radius
	pm.radial_velocity_min = -radius / 0.7
	pm.radial_velocity_max = -radius / 0.8
	pm.gravity = Vector3.ZERO
	pm.scale_curve = _curve([Vector2(0, 1.0), Vector2(1, 0.2)])
	var c1 := _hdr(col, SPARK)
	c1.a = 0.0
	pm.color_ramp = _ramp([c1, _hdr(col, SPARK), _hdr(col.lightened(0.3), SPARK)], [0.0, 0.3, 1.0])
	e.process_material = pm
	e.draw_pass_1 = _billboard(0.3, false)
	add_child(e)
	e.global_position = p
	e.emitting = true
	_track(e, life + 0.7, func(t: float, _dt: float) -> void:
		if t * (life + 0.7) > life:
			e.emitting = false)


# ------------------------------------------------------------------ helpers

## The hue of a colour at full strength (pale costume accents would read as white).
func _vivid(c: Color) -> Color:
	var out := Color.from_hsv(c.h, maxf(c.s, 0.65), maxf(c.v, 0.9), c.a)
	return out


## Over-bright colour: additive light above the glow threshold blooms.
func _hdr(c: Color, k: float = GLOW) -> Color:
	return Color(c.r * k, c.g * k, c.b * k, c.a)


func _billboard(size: float, additive: bool) -> QuadMesh:
	var q := QuadMesh.new()
	q.size = Vector2(size, size)
	var m := StandardMaterial3D.new()
	m.billboard_mode = BaseMaterial3D.BILLBOARD_PARTICLES
	m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	m.vertex_color_use_as_albedo = true
	m.albedo_texture = _soft
	m.cull_mode = BaseMaterial3D.CULL_DISABLED
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	if additive:
		m.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
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
	# (an HDR gradient would drop the colour on some renderers; brightness comes from the colour values)
	return t


func _make_soft() -> ImageTexture:
	var s := 32
	var img := Image.create_empty(s, s, false, Image.FORMAT_RGBA8)
	for y in s:
		for x in s:
			var d := Vector2(x + 0.5 - s * 0.5, y + 0.5 - s * 0.5).length() / (s * 0.5)
			var a := clampf(1.0 - d, 0.0, 1.0)
			var core := clampf(1.0 - d * 2.2, 0.0, 1.0)
			var v := 0.75 + 0.25 * core
			img.set_pixel(x, y, Color(v, v, v, a * a * (3.0 - 2.0 * a)))
	return ImageTexture.create_from_image(img)


## A soft-edged ring (bright rim, faint inside).
func _make_ring() -> ImageTexture:
	var s := 128
	var img := Image.create_empty(s, s, false, Image.FORMAT_RGBA8)
	for y in s:
		for x in s:
			var d := Vector2(x + 0.5 - s * 0.5, y + 0.5 - s * 0.5).length() / (s * 0.5)
			var rim := exp(-pow((d - 0.9) / 0.05, 2.0))
			var inner := 0.12 * clampf(d / 0.9, 0.0, 1.0) if d < 0.9 else 0.0
			img.set_pixel(x, y, Color(1, 1, 1, clampf(rim + inner, 0.0, 1.0)))
	return ImageTexture.create_from_image(img)


## A crescent: bright along an arc, fading at both tips.
func _make_slash() -> ImageTexture:
	var w := 128
	var h := 64
	var img := Image.create_empty(w, h, false, Image.FORMAT_RGBA8)
	for y in h:
		for x in w:
			var u := (x + 0.5) / w * 2.0 - 1.0
			var v := (y + 0.5) / h
			var arc := 0.25 + 0.55 * (1.0 - u * u)
			var d := absf(v - arc)
			var a := exp(-pow(d / (0.06 + 0.05 * (1.0 - absf(u))), 2.0)) * (1.0 - pow(absf(u), 3.0))
			img.set_pixel(x, y, Color(1, 1, 1, clampf(a, 0.0, 1.0)))
	return ImageTexture.create_from_image(img)


func _vertical_fade() -> ImageTexture:
	var img := Image.create_empty(4, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		var v := y / 63.0
		var a := pow(v, 1.5) * (1.0 - pow(1.0 - v, 8.0))
		for x in 4:
			img.set_pixel(x, y, Color(1, 1, 1, a))
	return ImageTexture.create_from_image(img)
