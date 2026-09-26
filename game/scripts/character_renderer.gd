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
var _gear_mats := {}   # name -> StandardMaterial3D

# Materials of held gear, by the item's key.
const GEAR_COLORS := {
	"wood": Color(0.52, 0.36, 0.2), "stone": Color(0.55, 0.55, 0.56), "copper": Color(0.78, 0.5, 0.28),
	"iron": Color(0.62, 0.65, 0.7), "hide": Color(0.62, 0.52, 0.33), "leather": Color(0.42, 0.3, 0.18),
	"string": Color(0.9, 0.88, 0.8),
}
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
	var n := {"root": root, "body": body, "parts": [], "version": -1, "pos": c["pos"], "label": null, "crate": null,
		"aabbs": [], "gear_sig": ""}
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
		# Girls often stand together (at the hall): stagger their names.
		l.position = Vector3(0, 3.4 + 0.55 * float(int(c["id"]) % 3), 0)
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
	n["cart"] = _make_cart(root)
	return n


## A little hand cart, pulled behind a hauler who owns one.
func _make_cart(root: Node3D) -> Node3D:
	var cart := Node3D.new()
	cart.position = Vector3(0, 0, -1.05)
	cart.visible = false
	root.add_child(cart)
	var wood := _gear_mat("wood")
	_box(cart, Vector3(0.8, 0.32, 0.9), Vector3(0, 0.55, 0), wood)
	_box(cart, Vector3(0.06, 0.06, 0.7), Vector3(-0.3, 0.62, 0.75), wood)
	_box(cart, Vector3(0.06, 0.06, 0.7), Vector3(0.3, 0.62, 0.75), wood)
	var load := _box(cart, Vector3(0.6, 0.22, 0.7), Vector3(0, 0.8, 0), _crate_mat)
	load.name = "load"
	for side in [-1.0, 1.0]:
		var wheel := MeshInstance3D.new()
		var cm := CylinderMesh.new()
		cm.top_radius = 0.28
		cm.bottom_radius = 0.28
		cm.height = 0.08
		cm.radial_segments = 10
		wheel.mesh = cm
		wheel.material_override = _gear_mat("leather")
		wheel.rotation.z = PI / 2.0
		wheel.position = Vector3(0.46 * side, 0.28, 0)
		wheel.name = "wheel_l" if side < 0 else "wheel_r"
		cart.add_child(wheel)
	return cart


func _rebuild_body(n: Dictionary, id: int) -> void:
	for p in n["parts"]:
		p.queue_free()
	n["parts"] = []
	n["aabbs"] = []
	n["gear_sig"] = ""
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
			n["aabbs"].append(mesh.get_aabb())
		else:
			n["aabbs"].append(AABB())
		n["parts"].append(pivot)


func _animate(n: Dictionary, c: Dictionary, delta: float) -> void:
	var root: Node3D = n["root"]
	# Characters walk the same cube paths; a small fixed offset per character keeps
	# two of them on one cube from rendering as one merged body.
	var a := float(c["id"]) * 2.39996
	var target: Vector3 = c["pos"] + Vector3(cos(a), 0.0, sin(a)) * 0.22
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
	var weapon := String(c.get("weapon", ""))
	if c.get("fighting", false):
		if weapon == "bow":
			al = -1.55
			ar = -1.35 + sin(t * 3.0) * 0.12
		else:
			# Thrust / swing, a little out of step for each fighter.
			var ph2 := t * 8.0 + float(c["id"])
			ar = -1.25 + sin(ph2) * 0.55
			al = -0.5
	elif c.get("drafted", false) and not c["moving"] and weapon != "":
		ar = -0.35  # weapon at the ready
	if c.get("casting", false):
		al = -2.0 + sin(t * 5.0) * 0.15
		ar = -2.0 - sin(t * 5.0) * 0.15
	arml.rotation.x = lerpf(arml.rotation.x, al, 1.0 - exp(-delta * 12.0))
	armr.rotation.x = lerpf(armr.rotation.x, ar, 1.0 - exp(-delta * 12.0))
	# Spears stay level and bows upright in the body's frame.
	var lr: Node3D = n.get("level_r")
	if lr != null and is_instance_valid(lr):
		lr.rotation.x = -armr.rotation.x - 0.1
	var ll: Node3D = n.get("level_l")
	if ll != null and is_instance_valid(ll):
		ll.rotation.x = -arml.rotation.x
	head.rotation.x = sin(t * 0.7 + float(c["id"])) * 0.06
	body.position.y += bob
	var pulling: bool = c.get("cart", false) and c.get("carrying", false) and not lying and not c.get("drafted", false)
	(n["crate"] as MeshInstance3D).visible = c.get("carrying", false) and not lying and not c.get("drafted", false) and not pulling
	var cart: Node3D = n["cart"]
	cart.visible = pulling
	if pulling and c["moving"]:
		for wn in ["wheel_l", "wheel_r"]:
			(cart.get_node(wn) as Node3D).rotate_x(delta * 6.0)
	var sig := "%s|%s|%s|%s" % [weapon, c.get("armor", ""), c.get("drafted", false), c.get("pcolor", Color.WHITE)]
	if sig != n["gear_sig"]:
		n["gear_sig"] = sig
		_attach_gear(n, c)
	if not alive and not n.get("dead_applied", false):
		n["dead_applied"] = true
		for p in parts:
			for ch in p.get_children():
				if ch is MeshInstance3D:
					ch.material_override = _dead_mat


func _gear_mat(key: String, metal := false) -> StandardMaterial3D:
	var k := key + ("_m" if metal else "")
	if not _gear_mats.has(k):
		var m := StandardMaterial3D.new()
		m.albedo_color = GEAR_COLORS.get(key, Color(0.6, 0.6, 0.6))
		m.roughness = 0.45 if metal else 0.85
		m.metallic = 0.55 if metal else 0.0
		_gear_mats[k] = m
	return _gear_mats[k]


func _box(parent: Node3D, size: Vector3, pos: Vector3, mat: Material, rot := Vector3.ZERO) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = size
	mi.mesh = bm
	mi.material_override = mat
	mi.position = pos
	mi.rotation = rot
	mi.set_meta("gear", true)
	parent.add_child(mi)
	return mi


## Weapons, armour and the soldier's helmet, hung on the voxel body's part pivots. Pure
## decoration derived from the kernel's equipment slots.
func _attach_gear(n: Dictionary, c: Dictionary) -> void:
	var parts: Array = n["parts"]
	if parts.size() < 6:
		return
	for p in parts:
		for ch in (p as Node3D).get_children():
			if ch.has_meta("gear"):
				ch.queue_free()
	n.erase("level_r")
	n.erase("level_l")
	var aabbs: Array = n["aabbs"]
	var weapon := String(c.get("weapon", ""))
	var armor := String(c.get("armor", ""))
	var arm_r: Node3D = parts[PART_ARM_R]
	var arm_l: Node3D = parts[PART_ARM_L]
	var hand_r := Vector3(0, (aabbs[PART_ARM_R] as AABB).position.y + 0.05, 0)
	var hand_l := Vector3(0, (aabbs[PART_ARM_L] as AABB).position.y + 0.05, 0)
	var metal := "iron" if weapon.begins_with("iron") else ("copper" if weapon.begins_with("copper") else "stone")
	match weapon:
		"spear":
			# Held level in the fist whatever the arm does (see _animate), so a raised
			# arm thrusts it forward.
			var g := Node3D.new()
			g.set_meta("gear", true)
			g.position = hand_r
			arm_r.add_child(g)
			_box(g, Vector3(0.05, 0.05, 1.9), Vector3(0, 0, 0.45), _gear_mat("wood"))
			_box(g, Vector3(0.09, 0.03, 0.22), Vector3(0, 0, 1.48), _gear_mat("stone"))
			n["level_r"] = g
		"copper_sword", "iron_sword":
			var g := Node3D.new()
			g.set_meta("gear", true)
			g.position = hand_r
			g.rotation.x = -0.2
			arm_r.add_child(g)
			_box(g, Vector3(0.05, 0.1, 0.78), Vector3(0, 0, 0.46), _gear_mat(metal, true))
			_box(g, Vector3(0.26, 0.05, 0.05), Vector3(0, 0, 0.06), _gear_mat("leather"))
		"bow":
			# Upright in the left hand whatever the arm does: grip, two limbs curving
			# back towards the archer, and the string.
			var g := Node3D.new()
			g.set_meta("gear", true)
			g.position = hand_l
			arm_l.add_child(g)
			_box(g, Vector3(0.05, 0.5, 0.05), Vector3(0, 0, 0.08), _gear_mat("wood"))
			_box(g, Vector3(0.05, 0.45, 0.05), Vector3(0, 0.42, -0.01), _gear_mat("wood"), Vector3(-0.45, 0, 0))
			_box(g, Vector3(0.05, 0.45, 0.05), Vector3(0, -0.42, -0.01), _gear_mat("wood"), Vector3(0.45, 0, 0))
			_box(g, Vector3(0.015, 1.24, 0.015), Vector3(0, 0, -0.11), _gear_mat("string"))
			n["level_l"] = g
	var torso: Node3D = parts[PART_TORSO]
	var tb: AABB = aabbs[PART_TORSO]
	if armor != "" and tb.size.y > 0.0:
		var am := "hide" if armor == "hide_armor" else ("copper" if armor == "bronze_armor" else "iron")
		var vest := _box(torso, Vector3(tb.size.x * 1.1, tb.size.y * 0.62, tb.size.z * 1.18),
			tb.get_center() + Vector3(0, tb.size.y * 0.12, 0), _gear_mat(am, am != "hide"))
		vest.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	if c.get("drafted", false):
		var head: Node3D = parts[PART_HEAD]
		var hb: AABB = aabbs[PART_HEAD]
		if hb.size.y > 0.0:
			var hm := "leather" if armor == "" or armor == "hide_armor" else ("copper" if armor == "bronze_armor" else "iron")
			_box(head, Vector3(hb.size.x * 1.12, hb.size.y * 0.3, hb.size.z * 1.12),
				Vector3(hb.get_center().x, hb.end.y - hb.size.y * 0.1, hb.get_center().z), _gear_mat(hm, hm != "leather"))
			# A pennant in the polity's colour tells the sides apart.
			var pm := StandardMaterial3D.new()
			pm.albedo_color = c.get("pcolor", Color.WHITE)
			pm.emission_enabled = true
			pm.emission = pm.albedo_color
			pm.emission_energy_multiplier = 0.35
			_box(head, Vector3(0.045, 0.55, 0.045), Vector3(hb.get_center().x, hb.end.y + 0.2, hb.position.z - 0.02), _gear_mat("leather"))
			_box(head, Vector3(0.025, 0.2, 0.3), Vector3(hb.get_center().x, hb.end.y + 0.37, hb.position.z - 0.17), pm)


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
