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


func setup(s: IcarusSim, cam: Camera3D) -> void:
	sim = s
	camera = cam
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
	for i in 3:
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
		holder.add_child(mi)
	if not any and holder:
		holder.queue_free()
		_cells.erase(c)


func _update_debris() -> void:
	var seen := {}
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
			_meteors[id].queue_free()
			_meteors.erase(id)


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
	var trail := MeshInstance3D.new()
	var cm := CylinderMesh.new()
	cm.top_radius = sm.radius * 0.9
	cm.bottom_radius = 0.05
	cm.height = radius * 6.0
	trail.mesh = cm
	var tmat := StandardMaterial3D.new()
	tmat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	tmat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	tmat.albedo_color = Color(1.0, 0.6, 0.25, 0.55)
	tmat.emission_enabled = true
	tmat.emission = Color(1.0, 0.5, 0.2)
	trail.material_override = tmat
	trail.rotation_degrees = Vector3(90, 0, 0)
	trail.position = Vector3(0, 0, cm.height * 0.5)
	root.add_child(trail)
	var light := OmniLight3D.new()
	light.light_color = Color(1.0, 0.6, 0.3)
	light.light_energy = 6.0
	light.omni_range = radius * 8.0
	root.add_child(light)
	return root
