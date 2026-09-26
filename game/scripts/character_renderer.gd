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
var _mat: ShaderMaterial
var _dead_mat: ShaderMaterial
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
	_mat = ShaderMaterial.new()
	_mat.shader = load("res://shaders/character.gdshader")
	_dead_mat = _mat.duplicate()
	_dead_mat.set_shader_parameter("tint", Color(0.62, 0.58, 0.58))
	_dead_mat.set_shader_parameter("rim_strength", 0.0)
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
	var now := Time.get_ticks_usec() / 1e6
	var n := {"root": root, "body": body, "parts": [], "version": -1, "pos": c["pos"], "label": null, "crate": null,
		"aabbs": [], "gear_sig": "", "tool_sig": "", "prev": c["pos"], "cur": c["pos"], "t_cur": now, "dt": 0.05,
		"walk": 0.0, "phase": 0.0, "speed": 0.0, "lean": 0.0}
	root.position = c["pos"]
	if c.get("girl", false):
		n["look"] = {"hair": c.get("hair", Color(0.3, 0.2, 0.15)), "cloth": c.get("cloth", Color.WHITE),
			"accent": c.get("accent", Color(1, 0.8, 0.3))}
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
	n["tool_sig"] = ""
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
	if n.has("look"):
		_attach_look(n)


func _animate(n: Dictionary, c: Dictionary, delta: float) -> void:
	var root: Node3D = n["root"]
	# Characters walk the same cube paths; a small fixed offset per character keeps
	# two of them on one cube from rendering as one merged body.
	var a := float(c["id"]) * 2.39996
	var target: Vector3 = c["pos"] + Vector3(cos(a), 0.0, sin(a)) * 0.22
	# Interpolate between simulation snapshots: from where the figure is drawn now to
	# the newest position over one snapshot interval, so motion is even at any frame
	# rate and game speed.
	var now := Time.get_ticks_usec() / 1e6
	if target != n["cur"]:
		n["dt"] = clampf(now - float(n["t_cur"]), 0.016, 0.25)
		n["prev"] = root.position
		n["cur"] = target
		n["t_cur"] = now
	var prev_pos := root.position
	if (n["prev"] as Vector3).distance_to(target) > 4.0:
		root.position = target
		n["prev"] = target
	else:
		var f := clampf((now - float(n["t_cur"])) / float(n["dt"]), 0.0, 1.0)
		root.position = (n["prev"] as Vector3).lerp(target, f)
	var moved := Vector2(root.position.x - prev_pos.x, root.position.z - prev_pos.z).length()
	var spd := moved / maxf(delta, 0.001)
	n["speed"] = lerpf(float(n["speed"]), spd, 1.0 - exp(-delta * 8.0))
	root.rotation.y = lerp_angle(root.rotation.y, float(c["yaw"]), 1.0 - exp(-delta * 10.0))
	var parts: Array = n["parts"]
	if parts.size() < 6:
		return
	var body: Node3D = n["body"]
	var t := now + float(c["id"]) * 0.37
	var alive: bool = c["alive"]
	var sleeping: bool = c["sleeping"] and alive
	var lying := sleeping or not alive
	var k := 1.0 - exp(-delta * 12.0)
	var ks := 1.0 - exp(-delta * 5.0)
	# Lying down: asleep on the back, the dead fallen on their side.
	body.rotation.z = lerpf(body.rotation.z, PI / 2.0 if not alive else 0.0, ks)
	body.position.y = lerpf(body.position.y, 0.3 if lying else 0.0, ks)
	var legl: Node3D = parts[PART_LEG_L]
	var legr: Node3D = parts[PART_LEG_R]
	var arml: Node3D = parts[PART_ARM_L]
	var armr: Node3D = parts[PART_ARM_R]
	var head: Node3D = parts[PART_HEAD]
	# Walking: stride follows the ground actually covered (no sliding feet at any game
	# speed), blending in and out instead of snapping.
	var walking: bool = c["moving"] and not lying
	var w_target := clampf(float(n["speed"]) / 1.2, 0.0, 1.0) if walking else 0.0
	n["walk"] = lerpf(float(n["walk"]), w_target, 1.0 - exp(-delta * 6.0))
	var walk: float = n["walk"]
	n["phase"] = float(n["phase"]) + moved * 5.2
	var ph: float = n["phase"]
	var swing := sin(ph) * 0.7 * walk
	var ll := swing
	var lr := -swing
	var al := -swing * 0.75
	var ar := swing * 0.75
	var lean := 0.07 * walk
	var bob := absf(sin(ph)) * 0.07 * walk
	var head_x := 0.0
	var head_y := 0.0
	var arm_z := 0.0
	if walk < 0.3 and not lying:
		# Standing: breathing, and now and then a look around.
		bob += sin(t * 1.9) * 0.012
		al += sin(t * 1.9) * 0.03
		ar -= sin(t * 1.9) * 0.03
		head_y = sin(t * 0.31) * sin(t * 0.13 + 1.0) * 0.55
		head_x = sin(t * 0.7) * 0.05
	var job := String(c.get("job", ""))
	var working: bool = c.get("working", false) and not lying
	if working:
		var pose := _work_pose(job, t)
		al = pose[0]
		ar = pose[1]
		lean = pose[2]
		head_x = pose[3]
		arm_z = pose[4]
		head_y = 0.0
	if c.get("carrying", false) and not working:
		al = -1.15 + sin(ph) * 0.05 * walk
		ar = -1.15 - sin(ph) * 0.05 * walk
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
			var ph2 := t * 8.0
			ar = -1.25 + sin(ph2) * 0.55
			al = -0.5
			lean = 0.12
	elif c.get("drafted", false) and walk < 0.3 and weapon != "":
		ar = -0.35  # weapon at the ready
	if c.get("casting", false):
		al = -2.0 + sin(t * 5.0) * 0.15
		ar = -2.0 - sin(t * 5.0) * 0.15
		head_x = -0.2
	if lying:
		al = 0.1
		ar = 0.1
		ll = 0.0
		lr = 0.0
		lean = 0.0
		head_y = 0.0
		head_x = sin(t * 0.8) * 0.02 if sleeping else 0.0
	# A bend at the hips: the whole figure leans, the legs stay upright.
	var bx := -PI / 2.0 if sleeping else (0.0 if not alive else lean)
	body.rotation.x = lerpf(body.rotation.x, bx, ks if lying or absf(body.rotation.x) > 1.0 else k)
	var comp := body.rotation.x if not lying else 0.0
	legl.rotation.x = lerpf(legl.rotation.x, ll - comp, k)
	legr.rotation.x = lerpf(legr.rotation.x, lr - comp, k)
	arml.rotation.x = lerpf(arml.rotation.x, al, k)
	armr.rotation.x = lerpf(armr.rotation.x, ar, k)
	armr.rotation.z = lerpf(armr.rotation.z, arm_z, k)
	arml.rotation.z = lerpf(arml.rotation.z, -arm_z * 0.5, k)
	head.rotation.x = lerpf(head.rotation.x, head_x, k)
	head.rotation.y = lerpf(head.rotation.y, head_y, 1.0 - exp(-delta * 3.0))
	# Spears stay level and bows upright in the body's frame.
	var lvr: Node3D = n.get("level_r")
	if lvr != null and is_instance_valid(lvr):
		lvr.rotation.x = -armr.rotation.x - 0.1
	var lvl: Node3D = n.get("level_l")
	if lvl != null and is_instance_valid(lvl):
		lvl.rotation.x = -arml.rotation.x
	body.position.y += bob if not lying else 0.0
	# Twin tails swing with the stride and settle when standing; the emblem floats.
	for tp in n.get("tails", []):
		if is_instance_valid(tp):
			(tp as Node3D).rotation.x = lerpf((tp as Node3D).rotation.x, 0.35 * walk + sin(t * 2.2) * 0.05 + absf(sin(ph)) * 0.12 * walk, k)
	var em: MeshInstance3D = n.get("emblem")
	if em != null and is_instance_valid(em):
		em.visible = alive
		em.position = Vector3(0, (3.05 if not lying else 0.9) + sin(t * 1.6) * 0.08, 0)
		em.rotation = Vector3(0.785, t * 1.4, 0.615)
	# The tool for the job in hand while working.
	var tool_sig := "%s|%s" % [job, c.get("tool", "")] if working and weapon == "" else ""
	if tool_sig != n["tool_sig"]:
		n["tool_sig"] = tool_sig
		_attach_tool(n, job, String(c.get("tool", "")), working and weapon == "")
	var pulling: bool = c.get("cart", false) and c.get("carrying", false) and not lying and not c.get("drafted", false)
	(n["crate"] as MeshInstance3D).visible = c.get("carrying", false) and not lying and not c.get("drafted", false) and not pulling
	var cart: Node3D = n["cart"]
	cart.visible = pulling
	if pulling:
		for wn in ["wheel_l", "wheel_r"]:
			(cart.get_node(wn) as Node3D).rotate_x(moved / 0.28)
	var sig := "%s|%s|%s|%s" % [weapon, c.get("armor", ""), c.get("drafted", false), c.get("pcolor", Color.WHITE)]
	if sig != n["gear_sig"]:
		n["gear_sig"] = sig
		_attach_gear(n, c)
	if not alive and not n.get("dead_applied", false):
		n["dead_applied"] = true
		for p in parts:
			for ch in p.get_children():
				if ch is MeshInstance3D and not ch.has_meta("gear") and not ch.has_meta("look"):
					ch.material_override = _dead_mat


## A magical girl's silhouette: twin tails with bows, a flared skirt with a hem in her
## colour, and her emblem floating above her. Decoration only.
func _attach_look(n: Dictionary) -> void:
	var parts: Array = n["parts"]
	if parts.size() < 6:
		return
	var look: Dictionary = n["look"]
	var hair := StandardMaterial3D.new()
	hair.albedo_color = look["hair"]
	hair.roughness = 0.7
	var cloth := StandardMaterial3D.new()
	cloth.albedo_color = (look["cloth"] as Color).darkened(0.08)
	cloth.roughness = 0.85
	var accent := StandardMaterial3D.new()
	accent.albedo_color = look["accent"]
	accent.roughness = 0.5
	var aabbs: Array = n["aabbs"]
	var hb: AABB = aabbs[PART_HEAD]
	var tails: Array = []
	if hb.size.y > 0.0:
		var head: Node3D = parts[PART_HEAD]
		for side in [-1.0, 1.0]:
			var pivot := Node3D.new()
			pivot.set_meta("look", true)
			var x: float = hb.get_center().x + float(side) * (hb.size.x * 0.5 + 0.05)
			pivot.position = Vector3(x, hb.position.y + hb.size.y * 0.72, hb.position.z + hb.size.z * 0.3)
			head.add_child(pivot)
			var tail := _box(pivot, Vector3(0.15, 0.62, 0.15), Vector3(0, -0.28, -0.04), hair)
			tail.remove_meta("gear")
			var tip := _box(pivot, Vector3(0.11, 0.14, 0.11), Vector3(0, -0.64, -0.06), hair)
			tip.remove_meta("gear")
			var bow := _box(pivot, Vector3(0.2, 0.1, 0.08), Vector3(0, 0.02, 0.0), accent)
			bow.remove_meta("gear")
			tails.append(pivot)
	n["tails"] = tails
	var tb: AABB = aabbs[PART_TORSO]
	if tb.size.y > 0.0:
		var torso: Node3D = parts[PART_TORSO]
		var skirt := _box(torso, Vector3(tb.size.x * 1.28, 0.24, tb.size.z * 1.5), Vector3(tb.get_center().x, tb.position.y + 0.02, tb.get_center().z), cloth)
		skirt.remove_meta("gear")
		skirt.set_meta("look", true)
		var hem := _box(torso, Vector3(tb.size.x * 1.3, 0.05, tb.size.z * 1.52), Vector3(tb.get_center().x, tb.position.y - 0.1, tb.get_center().z), accent)
		hem.remove_meta("gear")
		hem.set_meta("look", true)
	if not n.has("emblem"):
		var em := MeshInstance3D.new()
		var bm := BoxMesh.new()
		bm.size = Vector3(0.14, 0.14, 0.14)
		em.mesh = bm
		var gm := StandardMaterial3D.new()
		gm.albedo_color = look["accent"]
		gm.emission_enabled = true
		gm.emission = look["accent"]
		gm.emission_energy_multiplier = 1.6
		em.material_override = gm
		em.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		(n["root"] as Node3D).add_child(em)
		n["emblem"] = em


## Arms, lean and head for a kind of work: [left arm, right arm, lean, head pitch,
## right arm sideways]. Rhythms are per character (t already carries an offset).
func _work_pose(job: String, t: float) -> Array:
	match job:
		"chop", "mine", "dig":
			# Wind up slowly overhead, strike fast, rest a moment.
			var period := 1.1 if job == "chop" else 0.95
			var p := fposmod(t / period, 1.0)
			var raised := -2.6 if job == "chop" else -2.3
			var hit := -0.45 if job == "chop" else -0.15
			var arm: float
			if p < 0.6:
				arm = lerpf(hit, raised, smoothstep(0.0, 1.0, p / 0.6))
			elif p < 0.72:
				arm = lerpf(raised, hit, (p - 0.6) / 0.12)
			else:
				arm = hit
			var lean := 0.12 + (0.18 if job != "chop" else 0.08) * (1.0 - absf(arm - hit) / absf(raised - hit))
			return [arm + 0.1, arm, lean, 0.15, 0.0]
		"till":
			var p := sin(t * TAU / 0.9)
			return [-0.9 + p * 0.35, -1.0 + p * 0.45, 0.32, 0.25, 0.0]
		"sow":
			var p := fposmod(t / 1.2, 1.0)
			var fling := -1.0 + 0.7 * smoothstep(0.0, 0.3, p) - 0.7 * smoothstep(0.5, 1.0, p)
			return [-0.55, fling, 0.28, 0.3, 0.35 * smoothstep(0.0, 0.3, p)]
		"harvest", "forage":
			var p := sin(t * TAU / 0.75)
			return [-0.5 - p * 0.3, -0.5 + p * 0.3, 0.55, 0.35, 0.1]
		"build", "craft":
			var p := fposmod(t / 0.45, 1.0)
			var tap := -1.35 + 0.65 * smoothstep(0.55, 0.75, p) - 0.65 * smoothstep(0.75, 1.0, p)
			return [-0.9, tap, 0.1, 0.2, 0.0]
		"cook":
			return [-0.85, -0.95 + sin(t * 4.0) * 0.12, 0.12, 0.3, sin(t * 4.0 + 1.3) * 0.18]
		"research":
			return [-1.05, -1.05, 0.05, 0.4, -0.15]
		"haul":
			return [-1.15, -1.15, 0.0, 0.0, 0.0]
	var p2 := sin(t * 9.0)
	return [-0.6 + sin(t * 9.0 + 1.0) * 0.3, -1.2 + p2 * 0.7, 0.1, 0.1, 0.0]


## A hand tool for the current work (axe, pick, hoe, hammer, sickle, ladle, book,
## basket), its head in the tool's metal.
func _attach_tool(n: Dictionary, job: String, tool_key: String, show: bool) -> void:
	var parts: Array = n["parts"]
	if parts.size() < 6:
		return
	for pi in [PART_ARM_R, PART_ARM_L]:
		for ch in (parts[pi] as Node3D).get_children():
			if ch.has_meta("tool"):
				ch.queue_free()
	if not show:
		return
	var aabbs: Array = n["aabbs"]
	var hand_r := Vector3(0, (aabbs[PART_ARM_R] as AABB).position.y + 0.06, 0)
	var hand_l := Vector3(0, (aabbs[PART_ARM_L] as AABB).position.y + 0.06, 0)
	var metal := "iron" if tool_key.begins_with("iron") else ("copper" if tool_key.begins_with("copper") else "stone")
	var wood := _gear_mat("wood")
	var head := _gear_mat(metal, metal != "stone")
	var g := Node3D.new()
	g.set_meta("tool", true)
	g.position = hand_r
	(parts[PART_ARM_R] as Node3D).add_child(g)
	match job:
		"chop":
			_box(g, Vector3(0.05, 0.05, 0.8), Vector3(0, 0, 0.3), wood)
			_box(g, Vector3(0.04, 0.24, 0.16), Vector3(0, 0.09, 0.62), head)
		"mine", "dig":
			_box(g, Vector3(0.05, 0.05, 0.8), Vector3(0, 0, 0.3), wood)
			_box(g, Vector3(0.05, 0.56, 0.06), Vector3(0, 0, 0.66), head)
			_box(g, Vector3(0.04, 0.08, 0.1), Vector3(0, 0.28, 0.63), head)
			_box(g, Vector3(0.04, 0.08, 0.1), Vector3(0, -0.28, 0.63), head)
		"till":
			_box(g, Vector3(0.05, 0.05, 1.05), Vector3(0, 0, 0.42), wood)
			_box(g, Vector3(0.16, 0.14, 0.03), Vector3(0, -0.07, 0.94), head)
		"build", "craft":
			_box(g, Vector3(0.05, 0.05, 0.45), Vector3(0, 0, 0.18), wood)
			_box(g, Vector3(0.09, 0.2, 0.09), Vector3(0, 0.04, 0.4), head)
		"harvest":
			_box(g, Vector3(0.05, 0.05, 0.22), Vector3(0, 0, 0.08), wood)
			_box(g, Vector3(0.03, 0.04, 0.2), Vector3(0, 0.03, 0.27), head)
			_box(g, Vector3(0.03, 0.14, 0.04), Vector3(0, 0.1, 0.37), head)
		"cook":
			_box(g, Vector3(0.03, 0.03, 0.42), Vector3(0, 0, 0.2), wood)
			_box(g, Vector3(0.1, 0.07, 0.1), Vector3(0, -0.03, 0.43), wood)
		"research":
			var cover := StandardMaterial3D.new()
			cover.albedo_color = Color(0.55, 0.22, 0.2)
			_box(g, Vector3(0.3, 0.05, 0.22), Vector3(-0.12, 0.02, 0.14), cover)
			_box(g, Vector3(0.27, 0.02, 0.2), Vector3(-0.12, 0.055, 0.14), _gear_mat("string"))
		"forage", "sow":
			var bag := Node3D.new()
			bag.set_meta("tool", true)
			bag.position = hand_l
			(parts[PART_ARM_L] as Node3D).add_child(bag)
			_box(bag, Vector3(0.28, 0.2, 0.26), Vector3(0, -0.08, 0.1), _gear_mat("hide"))
			_box(bag, Vector3(0.04, 0.14, 0.04), Vector3(0, 0.06, 0.1), _gear_mat("leather"))


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
