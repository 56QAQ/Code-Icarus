class_name CharacterRenderer
extends Node3D
## Draws characters from kernel snapshots. Bodies are the kernel's voxel parts (so
## injuries and lost limbs are visible); poses are procedural: walking, working,
## sleeping, protesting, carrying. Pure presentation.

signal character_clicked(id: int)

var sim: IcarusSim
var camera: Camera3D
var selected_id := -1

var _nodes := {}       # id -> Dictionary
var _mat: StandardMaterial3D
var _dead_mat: StandardMaterial3D
var _ring: MeshInstance3D
var _crate_mesh: BoxMesh
var _crate_mat: StandardMaterial3D
var _pile_nodes := {}  # key -> MeshInstance3D
var _pile_timer := 0.0

const PART_HEAD := 0
const PART_TORSO := 1
const PART_ARM_L := 2
const PART_ARM_R := 3
const PART_LEG_L := 4
const PART_LEG_R := 5


func _ready() -> void:
	_mat = StandardMaterial3D.new()
	_mat.vertex_color_use_as_albedo = true
	_mat.vertex_color_is_srgb = true
	_mat.roughness = 0.85
	_dead_mat = _mat.duplicate()
	_dead_mat.albedo_color = Color(0.55, 0.5, 0.5)
	_ring = MeshInstance3D.new()
	var tm := TorusMesh.new()
	tm.inner_radius = 0.75
	tm.outer_radius = 0.95
	tm.rings = 32
	_ring.mesh = tm
	var rm := StandardMaterial3D.new()
	rm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	rm.albedo_color = Color(0.91, 0.76, 0.44)
	rm.emission_enabled = true
	rm.emission = Color(0.91, 0.76, 0.44)
	rm.emission_energy_multiplier = 1.5
	_ring.material_override = rm
	_ring.scale = Vector3(1, 0.3, 1)
	_ring.visible = false
	_ring.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_ring)
	_crate_mesh = BoxMesh.new()
	_crate_mesh.size = Vector3(0.45, 0.35, 0.35)
	_crate_mat = StandardMaterial3D.new()
	_crate_mat.albedo_color = Color(0.72, 0.56, 0.34)


func setup(s: IcarusSim, cam: Camera3D) -> void:
	sim = s
	camera = cam
	for n in _nodes.values():
		n["root"].queue_free()
	_nodes.clear()


func _process(delta: float) -> void:
	if sim == null or not sim.has_game():
		return
	var seen := {}
	for c in sim.characters():
		var id: int = c["id"]
		seen[id] = true
		var n: Dictionary = _nodes.get(id, {})
		if n.is_empty():
			n = _create(c)
			_nodes[id] = n
		if n["version"] != c["body_version"]:
			_rebuild_body(n, id)
			n["version"] = c["body_version"]
		_animate(n, c, delta)
	for id in _nodes.keys():
		if not seen.has(id):
			_nodes[id]["root"].queue_free()
			_nodes.erase(id)
	# Selection ring.
	if selected_id >= 0 and _nodes.has(selected_id):
		_ring.visible = true
		var r: Node3D = _nodes[selected_id]["root"]
		_ring.global_position = r.global_position + Vector3(0, 0.08, 0)
	else:
		_ring.visible = false
	_pile_timer -= delta
	if _pile_timer <= 0.0:
		_pile_timer = 0.5
		_update_piles()


func _create(c: Dictionary) -> Dictionary:
	var root := Node3D.new()
	root.name = "char_%d" % c["id"]
	add_child(root)
	var body := Node3D.new()  # tilts when lying down
	root.add_child(body)
	var n := {"root": root, "body": body, "parts": [], "version": -1, "pos": c["pos"], "label": null, "crate": null}
	root.position = c["pos"]
	if c.get("girl", false):
		var l := Label3D.new()
		l.text = "◆ %s" % c["name"]
		l.font = UITheme.font_bold()
		l.font_size = 30
		l.pixel_size = 0.0011
		l.fixed_size = true
		l.outline_size = 10
		l.outline_modulate = Color(0.05, 0.06, 0.1, 0.85)
		l.modulate = Color(1.0, 0.92, 0.7)
		l.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		l.no_depth_test = false
		l.position = Vector3(0, 3.4, 0)
		l.visibility_range_end = 160.0
		root.add_child(l)
		n["label"] = l
	var crate := MeshInstance3D.new()
	crate.mesh = _crate_mesh
	crate.material_override = _crate_mat
	crate.position = Vector3(0, 1.45, 0.42)
	crate.visible = false
	body.add_child(crate)
	n["crate"] = crate
	return n


func _rebuild_body(n: Dictionary, id: int) -> void:
	for p in n["parts"]:
		p.queue_free()
	n["parts"] = []
	var parts: Array = sim.character_body(id)
	for i in parts.size():
		var d: Dictionary = parts[i]
		var pivot := Node3D.new()
		pivot.position = d["pivot"]
		n["body"].add_child(pivot)
		var arr: Array = d["mesh"]
		if not arr.is_empty():
			var mesh := ArrayMesh.new()
			mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr)
			var mi := MeshInstance3D.new()
			mi.mesh = mesh
			mi.material_override = _mat
			pivot.add_child(mi)
		n["parts"].append(pivot)


func _animate(n: Dictionary, c: Dictionary, delta: float) -> void:
	var root: Node3D = n["root"]
	var target: Vector3 = c["pos"]
	# Smooth between simulation ticks.
	if root.position.distance_to(target) > 4.0:
		root.position = target
	else:
		root.position = root.position.lerp(target, 1.0 - exp(-delta * 14.0))
	root.rotation.y = lerp_angle(root.rotation.y, float(c["yaw"]), 1.0 - exp(-delta * 10.0))
	var parts: Array = n["parts"]
	if parts.size() < 6:
		return
	var body: Node3D = n["body"]
	var t := Time.get_ticks_msec() / 1000.0
	var alive: bool = c["alive"]
	var lying: bool = c["sleeping"] or not alive
	body.rotation.x = lerpf(body.rotation.x, -PI / 2.0 if lying else 0.0, 1.0 - exp(-delta * 6.0))
	body.position.y = lerpf(body.position.y, 0.35 if lying else 0.0, 1.0 - exp(-delta * 6.0))
	var legl: Node3D = parts[PART_LEG_L]
	var legr: Node3D = parts[PART_LEG_R]
	var arml: Node3D = parts[PART_ARM_L]
	var armr: Node3D = parts[PART_ARM_R]
	var head: Node3D = parts[PART_HEAD]
	var swing := 0.0
	var bob := 0.0
	if c["moving"] and not lying:
		var ph: float = c["phase"]
		swing = sin(ph) * 0.65
		bob = abs(cos(ph)) * 0.06
	legl.rotation.x = swing
	legr.rotation.x = -swing
	var al := -swing * 0.8
	var ar := swing * 0.8
	if c.get("working", false):
		ar = -1.2 + sin(t * 9.0) * 0.7
		al = -0.6 + sin(t * 9.0 + 1.0) * 0.3
	if c.get("carrying", false):
		al = -1.2
		ar = -1.2
	if c.get("protest", false):
		ar = -2.8 + sin(t * 6.0) * 0.35
		al = -0.3
	arml.rotation.x = lerpf(arml.rotation.x, al, 1.0 - exp(-delta * 12.0))
	armr.rotation.x = lerpf(armr.rotation.x, ar, 1.0 - exp(-delta * 12.0))
	head.rotation.x = sin(t * 0.7 + float(c["id"])) * 0.06
	body.position.y += bob
	(n["crate"] as MeshInstance3D).visible = c.get("carrying", false) and not lying
	if not alive and not n.get("dead_applied", false):
		n["dead_applied"] = true
		for p in parts:
			for ch in p.get_children():
				if ch is MeshInstance3D:
					ch.material_override = _dead_mat


func pick(screen_pos: Vector2, max_px := 26.0) -> int:
	if camera == null:
		return -1
	var best := -1
	var bd := max_px
	for id in _nodes.keys():
		var root: Node3D = _nodes[id]["root"]
		var p := root.global_position + Vector3(0, 1.3, 0)
		if camera.is_position_behind(p):
			continue
		var sp := camera.unproject_position(p)
		var d := sp.distance_to(screen_pos)
		if d < bd:
			bd = d
			best = id
	return best


func position_of(id: int) -> Vector3:
	if _nodes.has(id):
		return (_nodes[id]["root"] as Node3D).global_position
	return Vector3.ZERO


func _update_piles() -> void:
	var seen := {}
	for p in sim.piles():
		var pos: Vector3i = p["pos"]
		var key := "%d_%d_%d" % [pos.x, pos.y, pos.z]
		seen[key] = true
		if not _pile_nodes.has(key):
			var mi := MeshInstance3D.new()
			var bm := BoxMesh.new()
			bm.size = Vector3(0.7, 0.45, 0.7)
			mi.mesh = bm
			mi.material_override = _crate_mat
			mi.position = Vector3(pos) + Vector3(0.5, 0.22, 0.5)
			add_child(mi)
			_pile_nodes[key] = mi
	for key in _pile_nodes.keys():
		if not seen.has(key):
			_pile_nodes[key].queue_free()
			_pile_nodes.erase(key)
