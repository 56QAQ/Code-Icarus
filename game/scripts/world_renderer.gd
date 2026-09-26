class_name WorldRenderer
extends Node3D
## Draws the voxel world. Cells are meshed by the kernel (C++) nearest-first within a
## per-frame time budget; changed cells are re-meshed. Read-only with respect to the
## simulation: meshing uses renderer-safe access that never activates cells.

var sim: IcarusSim
var camera: Camera3D
var budget_ms := 7.0

var mat_terrain: ShaderMaterial
var mat_water: ShaderMaterial
var mat_foliage: ShaderMaterial
var mat_decor: ShaderMaterial

var _cells := {}      # Vector3i -> Node3D
var _pending := {}    # Vector3i -> true
var _order: Array[Vector3i] = []
var _order_dirty := false
var _debris := {}     # id -> MeshInstance3D
var _meteors := {}    # id -> Node3D

signal initial_meshing_done

var _initial_done := false


func _ready() -> void:
	mat_terrain = ShaderMaterial.new()
	mat_terrain.shader = load("res://shaders/terrain.gdshader")
	mat_water = ShaderMaterial.new()
	mat_water.shader = load("res://shaders/water.gdshader")
	mat_water.render_priority = 1
	mat_foliage = ShaderMaterial.new()
	mat_foliage.shader = load("res://shaders/foliage.gdshader")
	mat_decor = ShaderMaterial.new()
	mat_decor.shader = load("res://shaders/decor.gdshader")


func setup(s: IcarusSim, cam: Camera3D) -> void:
	sim = s
	camera = cam
	# Cube textures for every material in the rules.
	var tex := TextureForge.build(sim.material_table())
	for m in [mat_terrain, mat_foliage]:
		m.set_shader_parameter("atlas", tex["atlas"])
		m.set_shader_parameter("layer_map", tex["map"])
		m.set_shader_parameter("crack_base", tex["crack_base"])
	var grass := Color("5f9e3d")
	for m in sim.material_table():
		if m["key"] == "grass":
			grass = m["color"]
	mat_decor.set_shader_parameter("decor", TextureForge.build_decor(grass))
	for c in _cells.values():
		c.queue_free()
	_cells.clear()
	_pending.clear()
	_initial_done = false
	var list: PackedInt32Array = sim.render_cells()
	for i in range(0, list.size(), 3):
		_pending[Vector3i(list[i], list[i + 1], list[i + 2])] = true
	sim.take_dirty_cells()
	_order_dirty = true


## 0 by day .. 1 at night: lit windows.
func set_night(v: float) -> void:
	mat_terrain.set_shader_parameter("night", v)
	mat_foliage.set_shader_parameter("night", v)


## 0 dry .. 1 soaked by rain.
func set_wetness(v: float) -> void:
	mat_terrain.set_shader_parameter("wetness", v)
	mat_foliage.set_shader_parameter("wetness", v)


func pending_count() -> int:
	return _pending.size()


func _process(_delta: float) -> void:
	if sim == null or not sim.has_game():
		return
	var dirty: PackedInt32Array = sim.take_dirty_cells()
	for i in range(0, dirty.size(), 3):
		var c := Vector3i(dirty[i], dirty[i + 1], dirty[i + 2])
		if not _pending.has(c):
			_pending[c] = true
			_order_dirty = true
	if _pending.is_empty():
		if not _initial_done:
			_initial_done = true
			initial_meshing_done.emit()
	else:
		_mesh_some()
	_update_debris()


func _mesh_some() -> void:
	if _order_dirty:
		_order.clear()
		for c in _pending.keys():
			_order.append(c)
		var cam := camera.global_position if camera else Vector3.ZERO
		_order.sort_custom(func(a: Vector3i, b: Vector3i) -> bool:
			return _dist2(a, cam) > _dist2(b, cam))  # nearest at the back (pop_back)
		_order_dirty = false
	var t0 := Time.get_ticks_usec()
	var limit := budget_ms if _initial_done else 40.0
	while not _order.is_empty():
		var c: Vector3i = _order.pop_back()
		if not _pending.has(c):
			continue
		_pending.erase(c)
		_build(c)
		if float(Time.get_ticks_usec() - t0) / 1000.0 > limit:
			break


func _dist2(c: Vector3i, cam: Vector3) -> float:
	var center := Vector3(c) * 32.0 + Vector3(16, 16, 16)
	return center.distance_squared_to(cam)


func _build(c: Vector3i) -> void:
	var arrays: Array = sim.build_cell_mesh(c)
	var holder: Node3D = _cells.get(c)
	if holder:
		for ch in holder.get_children():
			ch.queue_free()
	var any := false
	for i in arrays.size():
		var arr: Array = arrays[i]
		if arr.is_empty():
			continue
		any = true
		if holder == null:
			holder = Node3D.new()
			holder.name = "cell_%d_%d_%d" % [c.x, c.y, c.z]
			add_child(holder)
			_cells[c] = holder
		var mesh := ArrayMesh.new()
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr)
		var mi := MeshInstance3D.new()
		mi.mesh = mesh
		match i:
			0:
				mi.material_override = mat_terrain
			1:
				mi.material_override = mat_water
				mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
			2:
				mi.material_override = mat_foliage
			3:
				# Tufts and flowers: only near the camera, fading out.
				mi.material_override = mat_decor
				mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
				mi.visibility_range_end = 110.0
				mi.visibility_range_end_margin = 20.0
				mi.visibility_range_fade_mode = GeometryInstance3D.VISIBILITY_RANGE_FADE_SELF
			4:
				mi.material_override = mat_decor  # crops
		holder.add_child(mi)
	if not any and holder:
		holder.queue_free()
		_cells.erase(c)


func _update_debris() -> void:
	var seen := {}
	var now := Time.get_ticks_usec() / 1e6
	for d in sim.debris_list():
		var id: int = d["id"]
		seen[id] = true
		var mi: MeshInstance3D = _debris.get(id)
		if mi == null:
			var arr: Array = sim.debris_mesh(id)
			if arr.is_empty():
				continue
			var mesh := ArrayMesh.new()
			mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr)
			mi = MeshInstance3D.new()
			mi.mesh = mesh
			mi.material_override = mat_terrain
			add_child(mi)
			_debris[id] = mi
			mi.position = d["pos"]
			mi.set_meta("prev", mi.position)
			mi.set_meta("cur", mi.position)
			mi.set_meta("t", now)
			mi.set_meta("dt", 0.05)
		# Interpolate between simulation snapshots, like characters.
		var target: Vector3 = d["pos"]
		if target != mi.get_meta("cur"):
			mi.set_meta("dt", clampf(now - float(mi.get_meta("t")), 0.016, 0.25))
			mi.set_meta("prev", mi.position)
			mi.set_meta("cur", target)
			mi.set_meta("t", now)
		var f := clampf((now - float(mi.get_meta("t"))) / float(mi.get_meta("dt")), 0.0, 1.0)
		mi.position = (mi.get_meta("prev") as Vector3).lerp(target, f)
		# A falling chunk wobbles as it goes (the world re-embeds it square on landing).
		var v: Vector3 = d.get("vel", Vector3.ZERO)
		var k := clampf(v.length() * 0.08, 0.0, 1.0)
		mi.rotation = Vector3(sin(now * 3.1 + id) * 0.07, sin(now * 1.7 + id * 0.5) * 0.05, cos(now * 2.6 + id) * 0.07) * k
	for id in _debris.keys():
		if not seen.has(id):
			_debris[id].queue_free()
			_debris.erase(id)
	# Meteors: glowing rock with a trail.
	var mseen := {}
	for m in sim.meteors():
		var id: int = m["id"]
		mseen[id] = true
		var node: Node3D = _meteors.get(id)
		if node == null:
			node = _make_meteor(m["radius"])
			add_child(node)
			_meteors[id] = node
		node.position = m["pos"]
		var v: Vector3 = m["vel"]
		if v.length() > 0.01:
			node.look_at(node.position + v, Vector3.UP)
	for id in _meteors.keys():
		if not mseen.has(id):
			_impact_flash((_meteors[id] as Node3D).position)
			_meteors[id].queue_free()
			_meteors.erase(id)


## A flash of light where a meteor strikes, fading over half a second.
func _impact_flash(p: Vector3) -> void:
	var light := OmniLight3D.new()
	light.light_color = Color(1.0, 0.7, 0.4)
	light.light_energy = 16.0
	light.omni_range = 40.0
	light.position = p
	add_child(light)
	var tw := create_tween()
	tw.tween_property(light, "light_energy", 0.0, 0.6).set_ease(Tween.EASE_OUT)
	tw.tween_callback(light.queue_free)


func _make_meteor(radius: float) -> Node3D:
	var root := Node3D.new()
	var rock := MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = maxf(1.0, radius * 0.35)
	sm.height = sm.radius * 2.0
	rock.mesh = sm
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.25, 0.2, 0.2)
	mat.emission_enabled = true
	mat.emission = Color(1.0, 0.45, 0.1)
	mat.emission_energy_multiplier = 3.0
	rock.material_override = mat
	root.add_child(rock)
	# Trail: fire streaming off the rock and smoke left hanging behind it.
	for smoke in [false, true]:
		var e := GPUParticles3D.new()
		e.amount = 60 if smoke else 50
		e.lifetime = 2.5 if smoke else 0.5
		e.local_coords = false
		e.visibility_aabb = AABB(Vector3(-40, -40, -40), Vector3(80, 80, 80))
		var pm := ParticleProcessMaterial.new()
		pm.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE
		pm.emission_sphere_radius = sm.radius * 0.8
		pm.spread = 180.0
		pm.initial_velocity_min = 0.2
		pm.initial_velocity_max = 1.5
		pm.gravity = Vector3(0, 0.6, 0) if smoke else Vector3.ZERO
		pm.scale_min = 1.0
		pm.scale_max = 2.0
		var curve := Curve.new()
		curve.max_value = 4.0
		curve.add_point(Vector2(0, 0.8 if smoke else 1.2))
		curve.add_point(Vector2(1, 3.5 if smoke else 0.2))
		var ct := CurveTexture.new()
		ct.curve = curve
		pm.scale_curve = ct
		var g := Gradient.new()
		if smoke:
			g.colors = PackedColorArray([Color(0.35, 0.3, 0.28, 0.0), Color(0.3, 0.27, 0.26, 0.55), Color(0.25, 0.24, 0.24, 0.0)])
			g.offsets = PackedFloat32Array([0.0, 0.1, 1.0])
		else:
			g.colors = PackedColorArray([Color(1.0, 0.95, 0.6, 1.0), Color(1.0, 0.5, 0.15, 0.8), Color(0.6, 0.1, 0.05, 0.0)])
			g.offsets = PackedFloat32Array([0.0, 0.4, 1.0])
		var gt := GradientTexture1D.new()
		gt.gradient = g
		pm.color_ramp = gt
		e.process_material = pm
		var q := QuadMesh.new()
		q.size = Vector2(sm.radius * 1.2, sm.radius * 1.2)
		var qm := StandardMaterial3D.new()
		qm.billboard_mode = BaseMaterial3D.BILLBOARD_PARTICLES
		qm.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		qm.vertex_color_use_as_albedo = true
		qm.albedo_texture = _soft_dot()
		if not smoke:
			qm.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
			qm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		q.material = qm
		e.draw_pass_1 = q
		root.add_child(e)
	var light := OmniLight3D.new()
	light.light_color = Color(1.0, 0.6, 0.3)
	light.light_energy = 6.0
	light.omni_range = radius * 8.0
	root.add_child(light)
	return root


var _soft_tex: ImageTexture


func _soft_dot() -> ImageTexture:
	if _soft_tex == null:
		var n := 32
		var img := Image.create_empty(n, n, false, Image.FORMAT_RGBA8)
		for y in n:
			for x in n:
				var d := Vector2(x + 0.5 - n * 0.5, y + 0.5 - n * 0.5).length() / (n * 0.5)
				var a := clampf(1.0 - d, 0.0, 1.0)
				img.set_pixel(x, y, Color(1, 1, 1, a * a * (3.0 - 2.0 * a)))
		_soft_tex = ImageTexture.create_from_image(img)
	return _soft_tex
