class_name AnimalRenderer
extends Node3D
## Draws the wild animals near the camera from kernel snapshots: blocky voxel-style
## bodies built per species from its size, colours and features (ears, antlers, tusks,
## horns, tails), legs that swing with the gait, heads that dip to graze, young ones
## smaller, the dead lying on their side. Pure presentation.

var sim: IcarusSim
var camera: Camera3D
var focus := Vector3.ZERO  # set by the scene (the camera rig's target)
var radius := 150.0

var _species := {}   # key -> Dictionary (from the kernel)
var _nodes := {}     # id -> Dictionary
var _mats := {}      # Color -> StandardMaterial3D
var _poll := 0.0
var _snap: Array = []


func setup(s: IcarusSim, cam: Camera3D) -> void:
	sim = s
	camera = cam
	for n in _nodes.values():
		(n["root"] as Node3D).queue_free()
	_nodes.clear()
	_species.clear()
	for sp in sim.animal_species():
		_species[String(sp["key"])] = sp


func _process(delta: float) -> void:
	if sim == null or not sim.has_game():
		return
	# Snapshots a few times a second is plenty; motion is interpolated between them.
	_poll -= delta
	if _poll <= 0.0:
		_poll = 0.1
		_snap = sim.animals(focus, radius)
		var seen := {}
		for a in _snap:
			var id: int = a["id"]
			seen[id] = true
			var n: Dictionary = _nodes.get(id, {})
			if n.is_empty():
				n = _create(a)
				if n.is_empty():
					continue
				_nodes[id] = n
			_update_target(n, a)
		for id in _nodes.keys():
			if not seen.has(id):
				(_nodes[id]["root"] as Node3D).queue_free()
				_nodes.erase(id)
	for n in _nodes.values():
		_animate(n, delta)


func _mat(c: Color) -> StandardMaterial3D:
	if not _mats.has(c):
		var m := StandardMaterial3D.new()
		m.albedo_color = c
		m.roughness = 0.92
		_mats[c] = m
	return _mats[c]


func _box(parent: Node3D, size: Vector3, pos: Vector3, c: Color, rot := Vector3.ZERO) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = size
	mi.mesh = bm
	mi.material_override = _mat(c)
	mi.position = pos
	mi.rotation = rot
	parent.add_child(mi)
	return mi


func _create(a: Dictionary) -> Dictionary:
	var sp: Dictionary = _species.get(String(a["species"]), {})
	if sp.is_empty():
		return {}
	var size: Vector3 = sp["size"]  # length, height, width
	var cols: Array = sp["colors"]
	var body_c: Color = cols[0] if cols.size() > 0 else Color(0.5, 0.4, 0.3)
	var light_c: Color = cols[1] if cols.size() > 1 else body_c.lightened(0.3)
	var dark_c: Color = cols[2] if cols.size() > 2 else body_c.darkened(0.4)
	var look: Array = sp["look"]
	var L := size.x
	var H := size.y
	var W := size.z
	var root := Node3D.new()
	add_child(root)
	var body := Node3D.new()  # rolls over when dead
	root.add_child(body)
	var leg_h := H * 0.42
	var torso_h := H * 0.4
	var torso_y := leg_h + torso_h * 0.5
	# Body: back and a lighter belly.
	_box(body, Vector3(W, torso_h, L * 0.72), Vector3(0, torso_y, 0), body_c)
	_box(body, Vector3(W * 0.82, torso_h * 0.3, L * 0.6), Vector3(0, leg_h + torso_h * 0.1, 0), light_c)
	# Legs: four pivots at the hips and shoulders, swinging with the gait.
	var legs: Array = []
	var leg_w := maxf(0.08, W * 0.2)
	for i in 4:
		var side := -1.0 if i % 2 == 0 else 1.0
		var front := 1.0 if i < 2 else -1.0
		var pivot := Node3D.new()
		pivot.position = Vector3(side * W * 0.3, leg_h, front * L * 0.26)
		body.add_child(pivot)
		_box(pivot, Vector3(leg_w, leg_h, leg_w), Vector3(0, -leg_h * 0.5, 0), body_c.darkened(0.08))
		_box(pivot, Vector3(leg_w * 1.05, leg_h * 0.14, leg_w * 1.1), Vector3(0, -leg_h * 0.93, 0.01), dark_c)
		legs.append(pivot)
	# Neck and head on a pivot at the front of the body (it dips to graze).
	var tall := H > 1.1
	var head_pivot := Node3D.new()
	head_pivot.position = Vector3(0, torso_y + torso_h * 0.3, L * 0.34)
	body.add_child(head_pivot)
	var head_size := Vector3(W * 0.72, H * 0.3, L * 0.26)
	var head_pos := Vector3(0, H * 0.16, L * 0.14)
	if tall:
		_box(head_pivot, Vector3(W * 0.42, H * 0.36, W * 0.42), Vector3(0, H * 0.14, L * 0.06), body_c, Vector3(-0.5, 0, 0))
		head_pos = Vector3(0, H * 0.34, L * 0.16)
	var head := _box(head_pivot, head_size, head_pos, body_c)
	var face_z := head_pos.z + head_size.z * 0.5
	if "snout" in look or tall or "tusks" in look:
		_box(head_pivot, Vector3(head_size.x * 0.6, head_size.y * 0.55, head_size.z * 0.55), Vector3(0, head_pos.y - head_size.y * 0.15, face_z + head_size.z * 0.2), light_c if not "snout" in look else body_c.lightened(0.1))
		face_z += head_size.z * 0.45
	_box(head_pivot, Vector3(head_size.x * 0.25, head_size.y * 0.22, 0.04), Vector3(0, head_pos.y - head_size.y * 0.08, face_z + 0.02), dark_c)  # nose
	for side in [-1.0, 1.0]:
		_box(head_pivot, Vector3(0.05, 0.06, 0.04), Vector3(side * head_size.x * 0.32, head_pos.y + head_size.y * 0.15, head_pos.z + head_size.z * 0.5 + 0.01), Color(0.08, 0.07, 0.06))  # eyes
	var top := head_pos.y + head_size.y * 0.5
	for f in look:
		match String(f):
			"long_ears":
				for side in [-1.0, 1.0]:
					_box(head_pivot, Vector3(0.08, H * 0.45, 0.06), Vector3(side * head_size.x * 0.22, top + H * 0.2, head_pos.z - 0.04), body_c, Vector3(-0.2, 0, side * 0.15))
			"pointed_ears":
				for side in [-1.0, 1.0]:
					_box(head_pivot, Vector3(0.1, 0.16, 0.06), Vector3(side * head_size.x * 0.3, top + 0.07, head_pos.z - head_size.z * 0.2), body_c.darkened(0.1))
			"round_ears":
				for side in [-1.0, 1.0]:
					_box(head_pivot, Vector3(0.14, 0.12, 0.08), Vector3(side * head_size.x * 0.38, top + 0.04, head_pos.z - head_size.z * 0.25), body_c.darkened(0.15))
			"antlers":
				# Stags only.
				if not bool(a.get("female", false)):
					var ant := Color(0.78, 0.7, 0.55)
					for side in [-1.0, 1.0]:
						_box(head_pivot, Vector3(0.05, 0.45, 0.05), Vector3(side * 0.16, top + 0.22, head_pos.z - 0.05), ant, Vector3(-0.3, 0, side * 0.35))
						_box(head_pivot, Vector3(0.04, 0.24, 0.04), Vector3(side * 0.3, top + 0.4, head_pos.z + 0.04), ant, Vector3(0.3, 0, side * 0.6))
						_box(head_pivot, Vector3(0.04, 0.2, 0.04), Vector3(side * 0.24, top + 0.52, head_pos.z - 0.12), ant, Vector3(-0.6, 0, side * 0.1))
			"horns":
				for side in [-1.0, 1.0]:
					_box(head_pivot, Vector3(0.07, 0.24, 0.07), Vector3(side * 0.1, top + 0.1, head_pos.z - 0.06), Color(0.55, 0.5, 0.42), Vector3(-0.8, 0, side * 0.2))
					_box(head_pivot, Vector3(0.06, 0.18, 0.06), Vector3(side * 0.12, top + 0.13, head_pos.z - 0.25), Color(0.5, 0.45, 0.38), Vector3(-2.0, 0, side * 0.2))
			"tusks":
				for side in [-1.0, 1.0]:
					_box(head_pivot, Vector3(0.04, 0.12, 0.04), Vector3(side * head_size.x * 0.28, head_pos.y - head_size.y * 0.1, face_z - 0.02), Color(0.93, 0.9, 0.82), Vector3(-0.4, 0, 0))
			"beard":
				_box(head_pivot, Vector3(0.1, 0.16, 0.06), Vector3(0, head_pos.y - head_size.y * 0.6, face_z - 0.06), light_c)
			"mane":
				_box(body, Vector3(W * 0.3, 0.1, L * 0.6), Vector3(0, torso_y + torso_h * 0.55, 0.02), dark_c)
	# Tail.
	var tail_z := -L * 0.36
	if "puff_tail" in look:
		_box(body, Vector3(0.14, 0.14, 0.12), Vector3(0, torso_y + torso_h * 0.2, tail_z - 0.04), Color(0.95, 0.93, 0.88))
	elif "bushy_tail" in look:
		_box(body, Vector3(0.14, 0.14, L * 0.4), Vector3(0, torso_y - 0.05, tail_z - L * 0.16), body_c, Vector3(0.55, 0, 0))
		_box(body, Vector3(0.1, 0.1, 0.1), Vector3(0, torso_y - 0.2, tail_z - L * 0.33), light_c)
	elif "thin_tail" in look:
		_box(body, Vector3(0.04, 0.04, 0.3), Vector3(0, torso_y, tail_z - 0.12), dark_c, Vector3(0.9, 0, 0))
	elif "short_tail" in look or "stub_tail" in look:
		_box(body, Vector3(0.1, 0.12, 0.1), Vector3(0, torso_y + torso_h * 0.25, tail_z - 0.03), light_c if "short_tail" in look else body_c)
	if "spots" in look:
		for i in 4:
			_box(body, Vector3(0.07, 0.02, 0.07), Vector3((float(i % 2) - 0.5) * W * 0.4, torso_y + torso_h * 0.5 + 0.005, (float(i / 2) - 0.5) * L * 0.3), light_c)
	var now := Time.get_ticks_usec() / 1e6
	root.position = a["pos"]
	return {"root": root, "body": body, "legs": legs, "head": head_pivot, "prev": a["pos"], "cur": a["pos"], "t_cur": now,
		"dt": 0.1, "phase": 0.0, "moving": false, "running": false, "grazing": false, "alive": true, "yaw": float(a["yaw"]),
		"grown": float(a.get("grown", 1.0)), "leg_h": leg_h, "id": int(a["id"])}


func _update_target(n: Dictionary, a: Dictionary) -> void:
	var now := Time.get_ticks_usec() / 1e6
	var target: Vector3 = a["pos"]
	if target != n["cur"]:
		n["dt"] = clampf(now - float(n["t_cur"]), 0.05, 0.5)
		n["prev"] = (n["root"] as Node3D).position
		n["cur"] = target
		n["t_cur"] = now
	n["moving"] = a["moving"]
	n["running"] = a["running"]
	n["grazing"] = a["grazing"]
	n["alive"] = a["alive"]
	n["yaw"] = float(a["yaw"])
	n["grown"] = float(a.get("grown", 1.0))
	(n["root"] as Node3D).visible = not bool(a.get("butchered", false))


func _animate(n: Dictionary, delta: float) -> void:
	var root: Node3D = n["root"]
	var now := Time.get_ticks_usec() / 1e6
	if (n["prev"] as Vector3).distance_to(n["cur"]) > 8.0:
		root.position = n["cur"]
	else:
		var f := clampf((now - float(n["t_cur"])) / float(n["dt"]), 0.0, 1.0)
		root.position = (n["prev"] as Vector3).lerp(n["cur"], f)
	var k := 1.0 - exp(-delta * 10.0)
	root.rotation.y = lerp_angle(root.rotation.y, float(n["yaw"]), k)
	var g := float(n["grown"])
	root.scale = Vector3.ONE * g
	var body: Node3D = n["body"]
	var alive: bool = n["alive"]
	body.rotation.z = lerpf(body.rotation.z, 0.0 if alive else PI / 2.0, 1.0 - exp(-delta * 4.0))
	body.position.y = lerpf(body.position.y, 0.0 if alive else float(n["leg_h"]) * 0.6, 1.0 - exp(-delta * 4.0))
	var moving: bool = n["moving"] and alive
	var running: bool = n["running"] and moving
	var rate := (11.0 if running else 6.0) if moving else 0.0
	n["phase"] = float(n["phase"]) + delta * rate
	var amp := (0.85 if running else 0.5) if moving else 0.0
	var ph: float = n["phase"]
	var legs: Array = n["legs"]
	for i in legs.size():
		# Diagonal pairs move together (a trot); a bound when running.
		var off := 0.0 if i == 0 or i == 3 else PI
		if running:
			off = 0.0 if i < 2 else PI * 0.6
		(legs[i] as Node3D).rotation.x = lerpf((legs[i] as Node3D).rotation.x, sin(ph + off) * amp, 1.0 - exp(-delta * 20.0))
	if moving:
		body.position.y = absf(sin(ph)) * (0.06 if running else 0.025)
	var head: Node3D = n["head"]
	var dip := 0.75 if (n["grazing"] and alive and not moving) else 0.0
	if dip > 0.0:
		dip += sin(now * 1.7 + float(n["id"])) * 0.1
	head.rotation.x = lerpf(head.rotation.x, dip, 1.0 - exp(-delta * 3.0))


## The animal under the cursor (-1 if none): a segment from its feet to its back.
func pick(screen_pos: Vector2) -> int:
	if camera == null:
		return -1
	var best := -1
	var bd := INF
	for id in _nodes.keys():
		var root: Node3D = _nodes[id]["root"]
		var feet := root.global_position
		var top := feet + Vector3(0, 1.2, 0)
		if camera.is_position_behind(top) or camera.is_position_behind(feet):
			continue
		var a := camera.unproject_position(feet)
		var b := camera.unproject_position(top)
		var d := Geometry2D.get_closest_point_to_segment(screen_pos, a, b).distance_to(screen_pos)
		var tol := maxf(16.0, a.distance_to(b) * 0.5 + 6.0)
		if d > tol:
			continue
		var score := d / tol + camera.global_position.distance_to(feet) * 0.001
		if score < bd:
			bd = score
			best = id
	return best


func position_of(id: int) -> Vector3:
	var n: Dictionary = _nodes.get(id, {})
	return (n["root"] as Node3D).global_position if not n.is_empty() else Vector3.ZERO
