class_name VillageRenderer
extends Node3D
## The look of a lived-in village. Goods lying on the ground are shown as what they are
## (a stack of logs, sacks of grain, a basket of fruit, a heap of ore, a rack of tools)
## and so is what a storehouse holds, in the yard by its door. Around the buildings
## stand the things of daily life: woodpiles and water jars by the huts, barrels by the
## storehouse, a hearth and pot in the kitchen, a workbench and tool rack by the
## workshop, banners in the people's colour at the hall, herbs drying at the herbalist.
## Pure presentation: it reads the kernel's buildings and piles, never changes them.

var sim: IcarusSim
var camera: Camera3D

const RANGE := 220.0         # nothing is built for places farther than this from the camera
const MAX_PER_PILE := 14     # pieces drawn for one heap

var _piles := {}             # key -> {node, sig}
var _decor := {}             # building id -> {node, sig}
var _timer := 0.0
var _mats := {}              # colour -> material
var _meshes := {}            # name -> mesh


func setup(s: IcarusSim, cam: Camera3D) -> void:
	sim = s
	camera = cam
	for d in [_piles, _decor]:
		for k in d.keys():
			(d[k]["node"] as Node3D).queue_free()
		d.clear()
	_timer = 0.0


func _process(delta: float) -> void:
	if sim == null or not sim.has_game():
		return
	_timer -= delta
	if _timer > 0.0:
		return
	_timer = 0.6
	var cam := camera.global_position if camera else Vector3.ZERO
	_update_piles(cam)
	_update_buildings(cam)


# ------------------------------------------------------------------ goods

## What a kind of goods looks like when heaped.
static func look_of(key: String) -> String:
	match key:
		"wood":
			return "logs"
		"planks":
			return "planks"
		"stone", "stone_block", "flint":
			return "stones"
		"brick":
			return "bricks"
		"copper_ore", "iron_ore", "meteoric_iron", "coal":
			return "ore"
		"copper", "iron":
			return "ingots"
		"grain", "dirt", "sand", "clay":
			return "sacks"
		"berries", "fruit", "mushroom", "herbs", "bread", "feast":
			return "basket"
		"meat", "cooked_meat":
			return "meat"
		"hide", "leaf_wrap", "fur_cloak", "linen_clothes", "hide_armor":
			return "hides"
		"fiber":
			return "bundle"
		"bone":
			return "bones"
		"pottery":
			return "jars"
		"levistone_shard":
			return "crystal"
	if key.ends_with("_tools") or key.ends_with("_axe") or key.ends_with("_pick") or key.ends_with("_hoe") \
			or key.ends_with("_hammer") or key.ends_with("_sickle") or key.ends_with("_knife") or key.ends_with("_sword") \
			or key.ends_with("spear") or key == "bow" or key == "digging_stick" or key.ends_with("_armor"):
		return "rack"
	return "crate"


const FOOD_COLORS := {"berries": Color(0.62, 0.12, 0.3), "fruit": Color(0.9, 0.45, 0.15), "mushroom": Color(0.85, 0.78, 0.66),
	"herbs": Color(0.35, 0.62, 0.3), "bread": Color(0.8, 0.58, 0.3), "feast": Color(0.95, 0.72, 0.35)}


func _update_piles(cam: Vector3) -> void:
	var seen := {}
	for p in sim.piles():
		var pos: Vector3i = p["pos"]
		if Vector3(pos).distance_to(cam) > RANGE:
			continue
		var key := "%d_%d_%d" % [pos.x, pos.y, pos.z]
		seen[key] = true
		var items: Array = p.get("items", [])
		var sig := _goods_sig(items)
		if _piles.has(key) and _piles[key]["sig"] == sig:
			continue
		if _piles.has(key):
			(_piles[key]["node"] as Node3D).queue_free()
		var root := Node3D.new()
		add_child(root)
		root.position = Vector3(pos) + Vector3(0.5, 0.0, 0.5)
		_heap(root, items, 0.9, hash(key))
		_piles[key] = {"node": root, "sig": sig}
	for key in _piles.keys():
		if not seen.has(key):
			(_piles[key]["node"] as Node3D).queue_free()
			_piles.erase(key)


func _goods_sig(items: Array) -> String:
	var s := ""
	for it in items:
		# Coarse amounts, so a heap is not rebuilt for every unit taken or added.
		s += "%s:%d," % [it["key"], mini(int(it["count"]), 40) / 5]
	return s


## Goods heaped on a patch of `span` cubes: up to three kinds side by side.
func _heap(root: Node3D, items: Array, span: float, seed: int) -> void:
	var rng := RandomNumberGenerator.new()
	rng.seed = seed
	var kinds := mini(items.size(), 3)
	for i in kinds:
		var it: Dictionary = items[i]
		var sub := Node3D.new()
		root.add_child(sub)
		var off := 0.0 if kinds == 1 else (float(i) - (kinds - 1) * 0.5) * span / float(kinds)
		sub.position = Vector3(off, 0, rng.randf_range(-0.1, 0.1))
		sub.rotation.y = rng.randf_range(-0.3, 0.3)
		var n := clampi(int(it["count"]), 1, 60)
		var scale := span / float(kinds)
		_goods(sub, String(it["key"]), n, scale, rng)


## One kind of goods: pieces in proportion to how much there is.
func _goods(root: Node3D, key: String, n: int, w: float, rng: RandomNumberGenerator) -> void:
	var look := look_of(key)
	var pieces := clampi(int(ceil(sqrt(float(n)) * 1.6)), 1, MAX_PER_PILE)
	match look:
		"logs":
			# A stack of logs, bark out, pale ends showing.
			var rows := clampi(pieces / 3 + 1, 1, 3)
			var k := 0
			for r in rows:
				for c in 3 - r:
					if k >= pieces:
						break
					var x := (float(c) - (2 - r) * 0.5) * 0.24
					_log(root, Vector3(x, 0.11 + r * 0.19, 0), w * 0.95, 0.1)
					k += 1
		"planks":
			for i in mini(pieces, 8):
				_box(root, Vector3(w * 0.9, 0.05, 0.18), Vector3(0, 0.03 + i * 0.055, (i % 2) * 0.05 - 0.1), Color(0.72, 0.55, 0.36), rng.randf_range(-0.08, 0.08))
		"stones", "bricks", "ore":
			var c: Color = {"stones": Color(0.55, 0.54, 0.52), "bricks": Color(0.66, 0.34, 0.24), "ore": Color(0.35, 0.3, 0.3)}[look]
			for i in pieces:
				var a := rng.randf() * TAU
				var r := rng.randf() * w * 0.35
				var y := 0.08 + (0.12 if i > pieces / 2 else 0.0)
				var sz := 0.16 if look == "bricks" else rng.randf_range(0.12, 0.2)
				var cc := c.lerp(Color(0.2, 0.2, 0.2), rng.randf() * 0.25)
				if key == "copper_ore":
					cc = cc.lerp(Color(0.2, 0.6, 0.5), 0.35)
				elif key == "iron_ore":
					cc = cc.lerp(Color(0.6, 0.35, 0.25), 0.35)
				_box(root, Vector3(sz * (1.6 if look == "bricks" else 1.0), sz * 0.8, sz), Vector3(cos(a) * r, y, sin(a) * r), cc, rng.randf() * PI)
		"ingots":
			for i in mini(pieces, 9):
				_box(root, Vector3(0.22, 0.07, 0.1), Vector3((i % 3 - 1) * 0.12, 0.04 + (i / 3) * 0.075, 0.0), Color(0.8, 0.5, 0.3) if key == "copper" else Color(0.6, 0.62, 0.66), 0.0)
		"sacks":
			var sc := Color(0.78, 0.68, 0.48) if key == "grain" else Color(0.55, 0.45, 0.35)
			for i in mini(pieces, 6):
				var x := (float(i % 3) - 1.0) * w * 0.3
				var y := 0.15 + (i / 3) * 0.24
				_sack(root, Vector3(x, y, (i % 2) * 0.06), sc.lerp(Color(0.6, 0.5, 0.35), rng.randf() * 0.2))
		"basket":
			var fc: Color = FOOD_COLORS.get(key, Color(0.8, 0.3, 0.2))
			for i in mini(maxi(1, pieces / 3), 3):
				_basket(root, Vector3((float(i) - 1.0) * 0.3 if pieces > 3 else 0.0, 0, 0), fc, rng)
		"meat":
			var mc := Color(0.7, 0.2, 0.2) if key == "meat" else Color(0.55, 0.3, 0.15)
			_box(root, Vector3(0.5, 0.06, 0.4), Vector3(0, 0.03, 0), Color(0.55, 0.42, 0.28), 0.0)  # a board
			for i in mini(pieces, 6):
				_box(root, Vector3(0.14, 0.09, 0.1), Vector3((i % 3 - 1) * 0.14, 0.1, (i / 3) * 0.14 - 0.07), mc, rng.randf() * 0.6)
		"hides":
			for i in mini(pieces, 5):
				var hc := Color(0.55, 0.4, 0.28) if key == "hide" else (Color(0.4, 0.6, 0.3) if key == "leaf_wrap" else Color(0.7, 0.66, 0.58))
				_box(root, Vector3(w * 0.8, 0.04, 0.45), Vector3(0, 0.02 + i * 0.045, 0), hc.lerp(Color.BLACK, rng.randf() * 0.15), rng.randf_range(-0.3, 0.3))
		"bundle":
			for i in mini(pieces, 5):
				_cyl(root, 0.08, w * 0.8, Vector3((i % 3 - 1) * 0.16, 0.09 + (i / 3) * 0.15, 0), Color(0.72, 0.68, 0.42), true)
		"bones":
			for i in mini(pieces, 8):
				_cyl(root, 0.03, 0.3, Vector3(rng.randf_range(-0.25, 0.25), 0.04, rng.randf_range(-0.2, 0.2)), Color(0.9, 0.87, 0.78), true, rng.randf() * PI)
		"jars":
			for i in mini(pieces, 6):
				_jar(root, Vector3((i % 3 - 1) * 0.24, 0, (i / 3) * 0.24 - 0.12), Color(0.66, 0.4, 0.26))
		"crystal":
			for i in mini(pieces, 5):
				var cr := _box(root, Vector3(0.1, 0.22, 0.1), Vector3(rng.randf_range(-0.2, 0.2), 0.12, rng.randf_range(-0.2, 0.2)), Color(0.5, 0.9, 0.95), rng.randf() * PI)
				cr.rotation.z = rng.randf_range(-0.4, 0.4)
		"rack":
			_rack(root, key)
		_:
			_box(root, Vector3(0.6, 0.4, 0.6), Vector3(0, 0.2, 0), Color(0.6, 0.45, 0.3), 0.0)


# ------------------------------------------------------------------ buildings

func _update_buildings(cam: Vector3) -> void:
	var seen := {}
	for b in sim.buildings():
		if b.get("bridge", false) or not b.has("min") or not b.get("complete", false):
			continue
		var id := int(b["id"])
		var door: Vector3i = b["pos"]
		if Vector3(door).distance_to(cam) > RANGE:
			continue
		seen[id] = true
		var sig := "%s|%s|%s" % [b["def"], b.get("functional", false), _goods_sig(b.get("goods", []))]
		if _decor.has(id) and _decor[id]["sig"] == sig:
			continue
		if _decor.has(id):
			(_decor[id]["node"] as Node3D).queue_free()
		var root := Node3D.new()
		add_child(root)
		_furnish(root, b)
		_decor[id] = {"node": root, "sig": sig}
	for id in _decor.keys():
		if not seen.has(id):
			(_decor[id]["node"] as Node3D).queue_free()
			_decor.erase(id)


## The building's door side: where things by the door and along the front wall go.
func _frame(b: Dictionary) -> Dictionary:
	var lo := Vector3(b["min"])
	var hi := Vector3(b["max"]) + Vector3.ONE
	var door := Vector3(b["pos"]) + Vector3(0.5, 0, 0.5)
	var c := (lo + hi) * 0.5
	var d := door - c
	var out := Vector3(signf(d.x), 0, 0) if absf(d.x) / (hi.x - lo.x) > absf(d.z) / (hi.z - lo.z) else Vector3(0, 0, signf(d.z))
	var side := Vector3(-out.z, 0, out.x)
	var half := (absf(side.x) * (hi.x - lo.x) + absf(side.z) * (hi.z - lo.z)) * 0.5
	var depth := (absf(out.x) * (hi.x - lo.x) + absf(out.z) * (hi.z - lo.z)) * 0.5
	var wall := Vector3(c.x, lo.y, c.z) + out * depth  # middle of the front wall, at the floor
	return {"lo": lo, "hi": hi, "c": c, "out": out, "side": side, "half": half, "depth": depth, "wall": wall, "door": door}


## A spot along the front wall (t along it, `away` out from it), on the ground there.
func _spot(f: Dictionary, t: float, away: float) -> Vector3:
	var p: Vector3 = f["wall"] + f["side"] * t + f["out"] * away
	p.y = _ground(p, float(f["lo"].y))
	return p


func _ground(p: Vector3, guess: float) -> float:
	var hit: Dictionary = sim.raycast(Vector3(p.x, guess + 4.0, p.z), Vector3.DOWN, 10.0)
	if hit.get("hit", false):
		return float((hit["cube"] as Vector3i).y) + 1.0
	return guess


func _furnish(root: Node3D, b: Dictionary) -> void:
	var f := _frame(b)
	var def := String(b["def"])
	var yaw := atan2((f["out"] as Vector3).x, (f["out"] as Vector3).z)
	var half: float = f["half"]
	var goods: Array = b.get("goods", [])
	var rng := RandomNumberGenerator.new()
	rng.seed = int(b["id"]) * 7919
	match def:
		"lean_to", "hut", "longhouse":
			# A woodpile against the wall, a water jar by the door, a bench.
			var wp := _node(root, _spot(f, -half + 0.7, 0.45), yaw)
			for r in 2:
				for c in 3 - r:
					_log(wp, Vector3((c - (2 - r) * 0.5) * 0.22, 0.1 + r * 0.18, 0), 0.9, 0.09, true)
			_jar(_node(root, _spot(f, 1.3, 0.45), yaw), Vector3.ZERO, Color(0.62, 0.38, 0.24), 1.3)
			if half > 2.0:
				var bench := _node(root, _spot(f, half - 1.0, 0.5), yaw)
				_box(bench, Vector3(0.9, 0.08, 0.28), Vector3(0, 0.4, 0), Color(0.6, 0.45, 0.3), 0.0)
				_box(bench, Vector3(0.08, 0.4, 0.24), Vector3(-0.36, 0.2, 0), Color(0.5, 0.38, 0.26), 0.0)
				_box(bench, Vector3(0.08, 0.4, 0.24), Vector3(0.36, 0.2, 0), Color(0.5, 0.38, 0.26), 0.0)
			if def == "longhouse":
				# A drying line with cloth between two poles.
				var line := _node(root, _spot(f, -1.5, 2.2), yaw)
				_cyl(line, 0.04, 1.6, Vector3(-1.1, 0.8, 0), Color(0.45, 0.33, 0.22))
				_cyl(line, 0.04, 1.6, Vector3(1.1, 0.8, 0), Color(0.45, 0.33, 0.22))
				_cyl(line, 0.015, 2.2, Vector3(0, 1.5, 0), Color(0.8, 0.78, 0.7), true)
				for i in 3:
					_box(line, Vector3(0.4, 0.5, 0.02), Vector3(-0.6 + i * 0.6, 1.22, 0), [Color(0.85, 0.8, 0.7), Color(0.6, 0.72, 0.85), Color(0.8, 0.55, 0.5)][i], 0.0)
		"storehouse", "granary":
			var bar := _node(root, _spot(f, -1.6, 0.5), yaw)
			_barrel(bar, Vector3(0, 0, 0))
			_barrel(bar, Vector3(0.5, 0, 0.05))
			_box(_node(root, _spot(f, 1.6, 0.5), yaw), Vector3(0.55, 0.45, 0.55), Vector3(0, 0.23, 0), Color(0.62, 0.47, 0.3), 0.1)
			# What it holds, set out in the yard beside the door.
			if not goods.is_empty():
				var yard := _node(root, _spot(f, half + 1.2, 1.4), yaw)
				_heap(yard, goods, 1.8, int(b["id"]))
		"kitchen":
			# The hearth in the middle, a pot over it, a table with bowls.
			var c := Vector3(f["c"])
			c.y = float(f["lo"].y)
			var hearth := _node(root, c, yaw)
			for i in 8:
				var a := TAU * i / 8.0
				_box(hearth, Vector3(0.22, 0.16, 0.22), Vector3(cos(a) * 0.42, 0.08, sin(a) * 0.42), Color(0.5, 0.49, 0.47), a)
			_cyl(hearth, 0.2, 0.26, Vector3(0, 0.45, 0), Color(0.22, 0.2, 0.2))
			_cyl(hearth, 0.025, 1.2, Vector3(-0.5, 0.6, 0), Color(0.3, 0.22, 0.15))
			_cyl(hearth, 0.025, 1.2, Vector3(0.5, 0.6, 0), Color(0.3, 0.22, 0.15))
			_cyl(hearth, 0.02, 1.0, Vector3(0, 1.15, 0), Color(0.3, 0.22, 0.15), true)
			var glow := OmniLight3D.new()
			glow.light_color = Color(1.0, 0.6, 0.3)
			glow.light_energy = 0.8
			glow.omni_range = 4.0
			glow.position = Vector3(0, 0.4, 0)
			hearth.add_child(glow)
			var table := _node(root, c + (f["side"] as Vector3) * 1.3, yaw)
			_table(table)
			for i in 3:
				_cyl(table, 0.08, 0.05, Vector3(-0.25 + i * 0.25, 0.78, 0), Color(0.72, 0.5, 0.32))
			for it in goods:
				if look_of(String(it["key"])) == "meat":
					var hang := _node(root, c - (f["side"] as Vector3) * 1.3, yaw)
					_cyl(hang, 0.02, 1.0, Vector3(0, 1.6, 0), Color(0.3, 0.22, 0.15), true)
					for i in 3:
						_box(hang, Vector3(0.1, 0.25, 0.08), Vector3(-0.3 + i * 0.3, 1.35, 0), Color(0.65, 0.22, 0.2), 0.0)
					break
		"workshop":
			var bench := _node(root, _spot(f, -1.2, 0.8), yaw)
			_table(bench, Color(0.55, 0.4, 0.26))
			_box(bench, Vector3(0.3, 0.06, 0.08), Vector3(0.2, 0.8, 0.1), Color(0.6, 0.6, 0.62), 0.3)  # a blade
			_cyl(bench, 0.05, 0.4, Vector3(-0.2, 0.82, -0.05), Color(0.5, 0.36, 0.22), true)
			_rack(_node(root, _spot(f, 1.5, 0.35), yaw), "stone_tools")
			var saw := _node(root, _spot(f, half + 0.8, 1.0), yaw + 0.4)
			_log(saw, Vector3(0, 0.45, 0), 1.2, 0.1, true)
			_box(saw, Vector3(0.06, 0.45, 0.4), Vector3(-0.4, 0.22, 0), Color(0.45, 0.33, 0.22), 0.0)
			_box(saw, Vector3(0.06, 0.45, 0.4), Vector3(0.4, 0.22, 0), Color(0.45, 0.33, 0.22), 0.0)
		"hall":
			# Banners of the people either side of the door, braziers before it.
			var pc: Color = b.get("color", Color(0.8, 0.2, 0.2))
			for s in [-1.0, 1.0]:
				var ban := _node(root, _spot(f, s * 1.6, 0.4), yaw)
				_cyl(ban, 0.05, 3.2, Vector3(0, 1.6, 0), Color(0.4, 0.3, 0.2))
				_box(ban, Vector3(0.6, 1.2, 0.03), Vector3(0, 2.4, 0.06), pc, 0.0)
				_box(ban, Vector3(0.6, 0.12, 0.035), Vector3(0, 1.84, 0.06), pc.lightened(0.35), 0.0)
				var br := _node(root, _spot(f, s * 2.6, 1.4), yaw)
				_cyl(br, 0.05, 0.9, Vector3(0, 0.45, 0), Color(0.3, 0.28, 0.28))
				_cyl(br, 0.25, 0.14, Vector3(0, 0.95, 0), Color(0.3, 0.28, 0.28))
				var fl := OmniLight3D.new()
				fl.light_color = Color(1.0, 0.62, 0.3)
				fl.light_energy = 0.9
				fl.omni_range = 5.0
				fl.position = Vector3(0, 1.3, 0)
				br.add_child(fl)
				_box(br, Vector3(0.2, 0.22, 0.2), Vector3(0, 1.1, 0), Color(1.0, 0.6, 0.2) * 1.6, 0.8, true)
		"study":
			var desk := _node(root, _spot(f, -1.3, 0.8), yaw)
			_table(desk)
			_cyl(desk, 0.05, 0.4, Vector3(-0.15, 0.8, 0), Color(0.92, 0.88, 0.76), true)
			_cyl(desk, 0.05, 0.4, Vector3(0.15, 0.8, 0.08), Color(0.92, 0.88, 0.76), true, 0.5)
		"herbalist":
			var rack := _node(root, _spot(f, 1.4, 0.5), yaw)
			_cyl(rack, 0.03, 1.4, Vector3(-0.5, 0.7, 0), Color(0.45, 0.33, 0.22))
			_cyl(rack, 0.03, 1.4, Vector3(0.5, 0.7, 0), Color(0.45, 0.33, 0.22))
			_cyl(rack, 0.02, 1.1, Vector3(0, 1.3, 0), Color(0.45, 0.33, 0.22), true)
			for i in 4:
				_box(rack, Vector3(0.1, 0.3, 0.1), Vector3(-0.36 + i * 0.24, 1.12, 0), Color(0.36, 0.6, 0.3).lerp(Color(0.6, 0.55, 0.3), i * 0.2), 0.0)
			_jar(_node(root, _spot(f, -1.2, 0.4), yaw), Vector3.ZERO, Color(0.55, 0.62, 0.5))
		"watchtower":
			var top := Vector3(f["c"])
			top.y = float(f["hi"].y)
			var flag := _node(root, top, 0.0)
			_cyl(flag, 0.04, 2.2, Vector3(0, 1.1, 0), Color(0.4, 0.3, 0.2))
			_box(flag, Vector3(0.7, 0.45, 0.03), Vector3(0.38, 1.9, 0), b.get("color", Color(0.8, 0.2, 0.2)), 0.0)
		"campfire":
			# Logs to sit on around the fire.
			var c2 := Vector3(b["inside"]) + Vector3(0.5, 0, 0.5)
			for i in 4:
				var a := TAU * i / 4.0 + 0.4
				var p := c2 + Vector3(cos(a), 0, sin(a)) * 2.0
				p.y = _ground(p, c2.y)
				var seat := _node(root, p, a)
				_log(seat, Vector3(0, 0.14, 0), 1.0, 0.13, true)
			for it in goods:
				if look_of(String(it["key"])) == "meat":
					var spit := _node(root, c2, 0.0)
					_cyl(spit, 0.025, 1.0, Vector3(-0.45, 0.5, 0), Color(0.35, 0.25, 0.16))
					_cyl(spit, 0.025, 1.0, Vector3(0.45, 0.5, 0), Color(0.35, 0.25, 0.16))
					_cyl(spit, 0.02, 1.1, Vector3(0, 0.95, 0), Color(0.35, 0.25, 0.16), true)
					_box(spit, Vector3(0.3, 0.16, 0.16), Vector3(0, 0.85, 0), Color(0.55, 0.28, 0.14), 0.0)
					break
			if not goods.is_empty():
				var heap := _node(root, c2 + Vector3(2.6, 0, -1.2), 0.3)
				heap.position.y = _ground(heap.position, c2.y)
				_heap(heap, goods, 1.4, int(b["id"]))


# ------------------------------------------------------------------ pieces

func _node(parent: Node3D, at: Vector3, yaw: float) -> Node3D:
	var n := Node3D.new()
	parent.add_child(n)
	n.position = at
	n.rotation.y = yaw
	return n


func _mat(c: Color, glow := false) -> StandardMaterial3D:
	var key := c.to_html() + ("g" if glow else "")
	if _mats.has(key):
		return _mats[key]
	var m := StandardMaterial3D.new()
	m.albedo_color = c
	m.roughness = 0.85
	if glow:
		m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_mats[key] = m
	return m


func _box(parent: Node3D, size: Vector3, at: Vector3, c: Color, yaw := 0.0, glow := false) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var key := "box%s" % size
	if not _meshes.has(key):
		var bm := BoxMesh.new()
		bm.size = size
		_meshes[key] = bm
	mi.mesh = _meshes[key]
	mi.material_override = _mat(c, glow)
	mi.position = at
	mi.rotation.y = yaw
	parent.add_child(mi)
	return mi


## A cylinder upright, or lying along the local x axis.
func _cyl(parent: Node3D, r: float, h: float, at: Vector3, c: Color, lying := false, yaw := 0.0) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var key := "cyl%.3f_%.3f" % [r, h]
	if not _meshes.has(key):
		var cm := CylinderMesh.new()
		cm.top_radius = r
		cm.bottom_radius = r
		cm.height = h
		cm.radial_segments = 8
		cm.rings = 1
		_meshes[key] = cm
	mi.mesh = _meshes[key]
	mi.material_override = _mat(c)
	mi.position = at
	if lying:
		mi.rotation = Vector3(0, yaw, PI / 2.0)
	else:
		mi.rotation.y = yaw
	parent.add_child(mi)
	return mi


func _log(parent: Node3D, at: Vector3, length: float, r: float, _along := true) -> void:
	_cyl(parent, r, length, at, Color(0.42, 0.3, 0.2), true)
	# Pale cut ends.
	_cyl(parent, r * 0.85, 0.02, at + Vector3(length * 0.5, 0, 0), Color(0.82, 0.68, 0.48), true)
	_cyl(parent, r * 0.85, 0.02, at - Vector3(length * 0.5, 0, 0), Color(0.82, 0.68, 0.48), true)


func _sack(parent: Node3D, at: Vector3, c: Color) -> void:
	var mi := MeshInstance3D.new()
	if not _meshes.has("sack"):
		var sm := SphereMesh.new()
		sm.radius = 0.16
		sm.height = 0.3
		sm.radial_segments = 10
		sm.rings = 6
		_meshes["sack"] = sm
	mi.mesh = _meshes["sack"]
	mi.material_override = _mat(c)
	mi.position = at
	mi.scale = Vector3(1.0, 1.0, 0.85)
	parent.add_child(mi)
	_cyl(parent, 0.05, 0.08, at + Vector3(0, 0.17, 0), c.darkened(0.2))


func _basket(parent: Node3D, at: Vector3, fruit: Color, rng: RandomNumberGenerator) -> void:
	_cyl(parent, 0.17, 0.14, at + Vector3(0, 0.07, 0), Color(0.62, 0.48, 0.28))
	for i in 5:
		var a := TAU * i / 5.0
		_box(parent, Vector3(0.07, 0.07, 0.07), at + Vector3(cos(a) * 0.08, 0.16 + rng.randf() * 0.03, sin(a) * 0.08), fruit.lerp(Color.WHITE, rng.randf() * 0.15), a)


func _jar(parent: Node3D, at: Vector3, c: Color, s := 1.0) -> void:
	_cyl(parent, 0.13 * s, 0.26 * s, at + Vector3(0, 0.13 * s, 0), c)
	_cyl(parent, 0.08 * s, 0.08 * s, at + Vector3(0, 0.3 * s, 0), c.darkened(0.1))


func _barrel(parent: Node3D, at: Vector3) -> void:
	_cyl(parent, 0.22, 0.55, at + Vector3(0, 0.28, 0), Color(0.55, 0.38, 0.22))
	_cyl(parent, 0.225, 0.04, at + Vector3(0, 0.12, 0), Color(0.3, 0.3, 0.3))
	_cyl(parent, 0.225, 0.04, at + Vector3(0, 0.44, 0), Color(0.3, 0.3, 0.3))


func _table(parent: Node3D, c := Color(0.6, 0.45, 0.3)) -> void:
	_box(parent, Vector3(0.9, 0.06, 0.5), Vector3(0, 0.74, 0), c, 0.0)
	for x in [-0.38, 0.38]:
		for z in [-0.18, 0.18]:
			_box(parent, Vector3(0.06, 0.72, 0.06), Vector3(x, 0.36, z), c.darkened(0.15), 0.0)


## A rack of tools or weapons leaning against a rail.
func _rack(parent: Node3D, key: String) -> void:
	_box(parent, Vector3(0.9, 0.05, 0.05), Vector3(0, 0.9, -0.12), Color(0.45, 0.33, 0.22), 0.0)
	_box(parent, Vector3(0.05, 0.9, 0.05), Vector3(-0.42, 0.45, -0.12), Color(0.45, 0.33, 0.22), 0.0)
	_box(parent, Vector3(0.05, 0.9, 0.05), Vector3(0.42, 0.45, -0.12), Color(0.45, 0.33, 0.22), 0.0)
	var head := Color(0.6, 0.6, 0.62)
	if key.begins_with("copper") or key == "bronze_armor":
		head = Color(0.8, 0.5, 0.3)
	elif key.begins_with("stone") or key.begins_with("flint"):
		head = Color(0.5, 0.49, 0.47)
	for i in 4:
		var x := -0.28 + i * 0.19
		var handle := _box(parent, Vector3(0.04, 1.0, 0.04), Vector3(x, 0.5, 0.0), Color(0.5, 0.36, 0.22), 0.0)
		handle.rotation.x = -0.18
		_box(parent, Vector3(0.14, 0.1, 0.05), Vector3(x, 0.98, -0.08), head, 0.0)
