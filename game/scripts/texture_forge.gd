class_name TextureForge
extends RefCounted
## Paints the cube textures at start-up: a 16×16 pixel-art face for every material
## (top / side / bottom where they differ), in the material's own colour from the rules,
## plus crack overlays for damaged cubes. Everything is generated, so a new material in
## `materials.json` gets a sensible texture without any art files.
##
## Output: a Texture2DArray (the layers) and a 1-pixel-high map, one texel per material
## id: R/G/B = layer of the top / side / bottom face, A = flags (see FLAG_*).

const SIZE := 16
const FLAG_TINTED := 1    # multiply by the vertex colour's hue (crops ripen)
const FLAG_NATURAL := 2   # large-scale colour drift across the landscape
const FLAG_GLOW := 4      # bright texels glow (levistone veins, magma)
const FLAG_SHINY := 8     # texture alpha < 1 marks glossy texels (ore, glass)
const FLAG_WINDOW := 16   # panes glow warm at night
# Bits 5-6 of the flags: extra variants of each face (consecutive layers), picked per
# cube so natural ground and rock never show the same stamp twice in a row.
const VARIANT_SHIFT := 5
const VARIED := ["grass", "dirt", "stone", "sand", "gravel", "clay", "leaves", "basalt", "path", "farmland", "rubble",
	"ash", "copper_ore", "iron_ore", "coal", "log", "snow", "mud", "red_sand", "sandstone", "granite", "limestone",
	"dry_grass", "pine_leaves", "birch_leaves", "fruit_leaves", "pine_log", "birch_log", "flint"]
const CRACK_STAGES := 4

var _layers: Array[Image] = []
var _rng := RandomNumberGenerator.new()


## materials: IcarusSim.material_table(). Returns {atlas, map, crack_base}.
static func build(materials: Array) -> Dictionary:
	var f := TextureForge.new()
	return f._build(materials)


func _build(materials: Array) -> Dictionary:
	var map := Image.create_empty(maxi(1, materials.size()), 1, false, Image.FORMAT_RGBA8)
	for m in materials:
		var key: String = m["key"]
		var col: Color = m["color"]
		var nvar := 3 if key in VARIED else 1
		var sets: Array = []
		for vi in nvar:
			_rng.seed = hash("%s#%d" % [key, vi]) & 0x7fffffff
			sets.append(_paint(key, col, m))
		# Each face kind gets its run of variants; faces that look alike share one.
		var top := _layers.size()
		for vi in nvar:
			_add(sets[vi][0])
		var side := top
		if sets[0][1] != sets[0][0]:
			side = _layers.size()
			for vi in nvar:
				_add(sets[vi][1])
		var bottom := top
		if sets[0][2] == sets[0][1]:
			bottom = side
		elif sets[0][2] != sets[0][0]:
			bottom = _layers.size()
			for vi in nvar:
				_add(sets[vi][2])
		map.set_pixel(int(m["id"]), 0, Color8(top, side, bottom, _flags(key, m) | ((nvar - 1) << VARIANT_SHIFT)))
	var crack_base := _layers.size()
	for s in CRACK_STAGES:
		_rng.seed = 7001 + s
		_add(_cracks(s))
	for img in _layers:
		img.generate_mipmaps()
	var atlas := Texture2DArray.new()
	atlas.create_from_images(_layers)
	return {"atlas": atlas, "map": ImageTexture.create_from_image(map), "crack_base": crack_base}


func _add(img: Image) -> int:
	_layers.append(img)
	return _layers.size() - 1


func _flags(key: String, m: Dictionary) -> int:
	var f := 0
	if key == "crop":
		f |= FLAG_TINTED
	if key in ["grass", "dirt", "sand", "stone", "gravel", "leaves", "clay", "path", "farmland", "basalt", "mud",
			"red_sand", "sandstone", "granite", "limestone", "dry_grass", "pine_leaves", "birch_leaves", "fruit_leaves"]:
		f |= FLAG_NATURAL
	if key in ["levistone", "magma", "spring"]:
		f |= FLAG_GLOW
	if key in ["copper_ore", "iron_ore", "coal", "meteorite", "glass", "levistone", "spring", "ice", "mud", "flint"]:
		f |= FLAG_SHINY
	if key == "glass":
		f |= FLAG_WINDOW
	return f


## A wooden crate face (carried goods, piles on the ground).
static func crate_texture() -> ImageTexture:
	var f := TextureForge.new()
	f._rng.seed = 4242
	var img := f._planks(Color("b08850"), false)
	var frame := Color(0.36, 0.25, 0.15)
	for i in SIZE:
		for e in [0, 1, SIZE - 2, SIZE - 1]:
			img.set_pixel(i, e, frame)
			img.set_pixel(e, i, frame)
		# A diagonal brace.
		img.set_pixel(i, i, frame.lightened(0.1))
		if i + 1 < SIZE:
			img.set_pixel(i + 1, i, frame)
	img.generate_mipmaps()
	return ImageTexture.create_from_image(img)


## Decoration sprites: three grass tufts (in the grass colour), four flowers, wheat in
## three stages, two dry tufts (layers 10-11), then one layer per plant material with a
## "sprite" in the rules, in material order (the kernel numbers them the same way).
static func build_decor(grass: Color, materials: Array = []) -> Texture2DArray:
	var f := TextureForge.new()
	var imgs: Array[Image] = []
	grass.s *= 0.84
	grass.v *= 0.94
	for i in 3:
		f._rng.seed = 900 + i
		imgs.append(f._tuft(grass, i))
	var petals := [Color(0.92, 0.28, 0.3), Color(0.98, 0.84, 0.3), Color(0.95, 0.95, 0.92), Color(0.52, 0.58, 0.95)]
	for i in 4:
		f._rng.seed = 950 + i
		imgs.append(f._flower(grass, petals[i]))
	# Wheat: sprouts, green stalks, ripe gold ears (layers 7-9).
	for i in 3:
		f._rng.seed = 980 + i
		imgs.append(f._wheat(i))
	for i in 2:
		f._rng.seed = 990 + i
		imgs.append(f._tuft(Color(0.78, 0.70, 0.36), i + 1))
	for m in materials:
		var layer: int = int(m.get("sprite_layer", -1))
		if layer < 0:
			continue
		while imgs.size() < layer:
			imgs.append(imgs[0])
		f._rng.seed = hash(String(m["key"])) & 0x7fffffff
		imgs.append(f._plant(String(m["sprite"]), m["color"]))
	for img in imgs:
		_bleed(img)
		img.generate_mipmaps()
	var arr := Texture2DArray.new()
	arr.create_from_images(imgs)
	return arr


## Transparent texels take the average colour of the opaque ones (alpha stays 0), so
## mipmaps of cut-out sprites do not darken towards black at a distance.
static func _bleed(img: Image) -> void:
	var sum := Color(0, 0, 0, 0)
	var n := 0
	for y in img.get_height():
		for x in img.get_width():
			var c := img.get_pixel(x, y)
			if c.a > 0.5:
				sum += Color(c.r, c.g, c.b, 0)
				n += 1
	if n == 0:
		return
	var avg := Color(sum.r / n, sum.g / n, sum.b / n, 0.0)
	for y in img.get_height():
		for x in img.get_width():
			if img.get_pixel(x, y).a <= 0.5:
				img.set_pixel(x, y, avg)


func _wheat(stage: int) -> Image:
	var img := _img()
	img.fill(Color(0, 0, 0, 0))
	var green := _ramp(Color(0.42, 0.66, 0.26), 4, 0.2)
	var gold := _ramp(Color(0.86, 0.72, 0.32), 4, 0.2)
	for x in range(1, SIZE, 2):
		var top := _rng.randi_range(0, 3) if stage > 0 else _rng.randi_range(6, 10)
		for y in range(top, SIZE):
			var col: Color = green[_rng.randi_range(1, 3)]
			if stage == 2:
				col = gold[_rng.randi_range(0, 2)] if y > top + 4 else gold[3]
			elif stage == 1 and y < top + 3:
				col = green[3].lerp(gold[2], 0.35)
			img.set_pixel(x, y, col)
			# Ears on ripe wheat: a wider head.
			if stage == 2 and y < top + 4 and x + 1 < SIZE:
				img.set_pixel(x + 1, y, gold[2] if (y + x) % 2 == 0 else gold[1])
		if stage == 0 and x + 1 < SIZE:
			img.set_pixel(x + 1, top + 1, green[2])
	return img


func _tuft(c: Color, variant: int) -> Image:
	var img := _img()
	img.fill(Color(0, 0, 0, 0))
	var pal := _ramp(c, 4, 0.24)
	var blades := 10 + variant * 3
	for b in blades:
		var x := float(_rng.randi_range(1, SIZE - 2))
		var lean := _rng.randf_range(-0.35, 0.35)
		var h := _rng.randi_range(5, SIZE - 1 - variant * 2)
		for s in h:
			var y := SIZE - 1 - s
			var px := clampi(int(round(x + lean * float(s))), 0, SIZE - 1)
			img.set_pixel(px, y, pal[clampi(1 + int(float(s) / float(h) * 3.0), 0, 3)])
	return img


func _flower(c: Color, petal: Color) -> Image:
	var img := _img()
	img.fill(Color(0, 0, 0, 0))
	var stem := _shade(c, 0.85)
	for y in range(7, SIZE):
		img.set_pixel(7, y, stem)
	img.set_pixel(6, 11, stem)
	img.set_pixel(5, 10, stem)
	img.set_pixel(8, 12, stem)
	img.set_pixel(9, 11, stem)
	for p in [[7, 3], [6, 4], [8, 4], [7, 5], [5, 4], [9, 4], [7, 2], [6, 5], [8, 5], [6, 3], [8, 3]]:
		img.set_pixel(p[0], p[1], petal if (p[0] + p[1]) % 3 != 0 else petal.lightened(0.2))
	img.set_pixel(7, 4, Color(0.95, 0.8, 0.25) if petal.b < 0.5 else Color(0.98, 0.9, 0.4))
	return img


## Plant sprites for materials drawn as crossed planes.
func _plant(kind: String, c: Color) -> Image:
	var img := _img()
	img.fill(Color(0, 0, 0, 0))
	match kind:
		"reeds":
			# Tall stems with brown cattail heads.
			var pal := _ramp(c, 4, 0.2)
			for x in range(1, SIZE, 3):
				var xx := x + _rng.randi_range(0, 1)
				var top := _rng.randi_range(0, 4)
				for y in range(top, SIZE):
					img.set_pixel(clampi(xx, 0, SIZE - 1), y, pal[_rng.randi_range(1, 3)])
				if _rng.randf() < 0.6:
					for y in range(top + 1, top + 4):
						img.set_pixel(clampi(xx, 0, SIZE - 1), y, Color(0.42, 0.27, 0.16))
				# A leaf blade leaning off the stem.
				var lx := xx
				for y in range(SIZE - 2, top + 6, -1):
					lx += 1 if _rng.randf() < 0.3 else 0
					if lx < SIZE:
						img.set_pixel(lx, y, pal[2])
		"wild_grain":
			# Thin stalks with nodding golden-green ears.
			var green := _ramp(Color(0.55, 0.62, 0.3), 4, 0.2)
			var gold := _ramp(c, 4, 0.2)
			for x in range(1, SIZE, 2):
				var top := _rng.randi_range(2, 6)
				for y in range(top, SIZE):
					img.set_pixel(x, y, green[_rng.randi_range(1, 3)] if y > top + 3 else gold[_rng.randi_range(1, 3)])
				if x + 1 < SIZE:
					img.set_pixel(x + 1, top + 1, gold[3])
					img.set_pixel(x + 1, top + 3, gold[2])
		"mushroom":
			# Two or three capped mushrooms of different sizes.
			var cap := _ramp(c, 4, 0.22)
			var stem := Color(0.9, 0.86, 0.76)
			for k in 3:
				var cx := 3 + k * 5 + _rng.randi_range(-1, 1)
				var h := _rng.randi_range(4, 8) if k != 1 else _rng.randi_range(7, 10)
				var r := 2 if k != 1 else 3
				for y in range(SIZE - h, SIZE):
					img.set_pixel(clampi(cx, 0, SIZE - 1), y, stem)
				var cy := SIZE - h
				for dy in range(-2, 1):
					for dx in range(-r, r + 1):
						if abs(dx) + max(0, -dy) * 1 > r + 1:
							continue
						var px := clampi(cx + dx, 0, SIZE - 1)
						var col: Color = cap[3] if dy == -2 else cap[2 if dy == -1 else 1]
						img.set_pixel(px, clampi(cy + dy, 0, SIZE - 1), col)
				# White spots on the cap.
				img.set_pixel(clampi(cx - 1, 0, SIZE - 1), clampi(cy - 1, 0, SIZE - 1), Color(0.96, 0.94, 0.9))
		"herb_plant":
			# A leafy herb with pale flower clusters.
			var pal := _ramp(c, 4, 0.24)
			for b in 7:
				var x := float(_rng.randi_range(3, 12))
				var lean := _rng.randf_range(-0.5, 0.5)
				var h := _rng.randi_range(5, 11)
				for s in h:
					var px := clampi(int(round(x + lean * float(s))), 0, SIZE - 1)
					img.set_pixel(px, SIZE - 1 - s, pal[clampi(1 + s * 3 / h, 0, 3)])
					if s % 3 == 1 and px + 1 < SIZE:
						img.set_pixel(px + 1, SIZE - 1 - s, pal[2])
				var tx := clampi(int(round(x + lean * float(h))), 0, SIZE - 2)
				img.set_pixel(tx, SIZE - h, Color(0.95, 0.92, 0.98))
				img.set_pixel(tx + 1, SIZE - h, Color(0.86, 0.8, 0.95))
		_:
			img = _tuft(c, 1)
	return img


# ------------------------------------------------------------------ painting helpers

func _img() -> Image:
	return Image.create_empty(SIZE, SIZE, false, Image.FORMAT_RGBA8)


## Tileable value noise on a coarse grid (cells of `step` pixels), 0..1.
func _noise(step: int) -> PackedFloat32Array:
	var g := SIZE / step
	var grid := PackedFloat32Array()
	grid.resize(g * g)
	for i in grid.size():
		grid[i] = _rng.randf()
	var out := PackedFloat32Array()
	out.resize(SIZE * SIZE)
	for y in SIZE:
		for x in SIZE:
			var fx := float(x) / float(step)
			var fy := float(y) / float(step)
			var x0 := int(floor(fx)) % g
			var y0 := int(floor(fy)) % g
			var x1 := (x0 + 1) % g
			var y1 := (y0 + 1) % g
			var tx := smoothstep(0.0, 1.0, fx - floor(fx))
			var ty := smoothstep(0.0, 1.0, fy - floor(fy))
			var a := lerpf(grid[y0 * g + x0], grid[y0 * g + x1], tx)
			var b := lerpf(grid[y1 * g + x0], grid[y1 * g + x1], tx)
			out[y * SIZE + x] = lerpf(a, b, ty)
	return out


## Two octaves plus a little per-pixel grain.
func _fbm(fine := 0.25) -> PackedFloat32Array:
	var a := _noise(8)
	var b := _noise(4)
	var c := _noise(2)
	var out := PackedFloat32Array()
	out.resize(SIZE * SIZE)
	for i in out.size():
		out[i] = clampf(a[i] * 0.45 + b[i] * 0.35 + c[i] * 0.2 + (_rng.randf() - 0.5) * fine, 0.0, 1.0)
	return out


func _shade(c: Color, k: float) -> Color:
	return Color(clampf(c.r * k, 0, 1), clampf(c.g * k, 0, 1), clampf(c.b * k, 0, 1), c.a)


## A palette from dark to light around a base colour, hue drifting a little warmer in
## the light and cooler in the shadow (reads better than plain brightness steps).
func _ramp(c: Color, n := 5, spread := 0.32) -> Array[Color]:
	var out: Array[Color] = []
	for i in n:
		var t := float(i) / float(n - 1) * 2.0 - 1.0  # -1 .. 1
		var k := 1.0 + t * spread
		var h := c
		h.h = fposmod(c.h + t * 0.012, 1.0)
		h.s = clampf(c.s * (1.0 - t * 0.12), 0.0, 1.0)
		out.append(_shade(h, k))
	return out


func _quantize(img: Image, field: PackedFloat32Array, pal: Array[Color]) -> void:
	for y in SIZE:
		for x in SIZE:
			var v := field[y * SIZE + x]
			var i := clampi(int(v * float(pal.size())), 0, pal.size() - 1)
			img.set_pixel(x, y, pal[i])


func _noisy(c: Color, spread := 0.28, fine := 0.25) -> Image:
	var img := _img()
	_quantize(img, _fbm(fine), _ramp(c, 5, spread))
	return img


func _speckle(img: Image, c: Color, count: int, size := 1) -> void:
	for i in count:
		var x := _rng.randi_range(0, SIZE - 1)
		var y := _rng.randi_range(0, SIZE - 1)
		for dy in size:
			for dx in size:
				img.set_pixel((x + dx) % SIZE, (y + dy) % SIZE, c)


func _pebbles(img: Image, c: Color, count: int) -> void:
	for i in count:
		var x := _rng.randi_range(0, SIZE - 1)
		var y := _rng.randi_range(0, SIZE - 1)
		var w := _rng.randi_range(1, 2)
		for dy in 2:
			for dx in w:
				img.set_pixel((x + dx) % SIZE, (y + dy) % SIZE, _shade(c, 1.12 if dy == 0 else 0.86))


# ------------------------------------------------------------------ materials

## Returns [top, side, bottom] images (the same object where faces look alike).
func _paint(key: String, c: Color, m: Dictionary) -> Array:
	match key:
		"grass":
			var dirt := _dirt(Color("7a5534"))
			# A touch less saturated than the rules' colour: reads as grass, not neon.
			var g := c
			g.s *= 0.84
			g.v *= 0.94
			return [_grass_top(g), _grass_side(g, dirt), dirt]
		"dirt":
			var d := _dirt(c)
			return [d, d, d]
		"path":
			var p := _path(c)
			return [p, _dirt(Color("7a5534")), _dirt(Color("7a5534"))]
		"farmland":
			var d := _dirt(Color("7a5534"))
			return [_farmland(c), d, d]
		"stone", "spring":
			var s := _stone(c, key == "spring")
			return [s, s, s]
		"rubble":
			var r := _cobble(c, 6)
			return [r, r, r]
		"gravel":
			var g := _gravel(c)
			return [g, g, g]
		"sand":
			var s := _sand(c)
			return [s, s, s]
		"clay":
			var s := _layered(c)
			return [s, s, s]
		"log":
			var bark := _bark(c)
			var rings := _rings(c)
			return [rings, bark, rings]
		"leaves", "berry_bush", "sapling":
			var l := _leaves(c)
			return [l, l, l]
		"planks":
			var p := _planks(c, false)
			return [p, p, p]
		"door":
			var d := _planks(c, true)
			return [d, d, d]
		"brick":
			var b := _bricks(c, Color(0.78, 0.74, 0.68))
			return [b, b, b]
		"stone_brick":
			var b := _blocks(c)
			return [b, b, b]
		"thatch":
			return [_thatch(c, true), _thatch(c, false), _thatch(c, false)]
		"glass":
			var g := _glass(c)
			return [g, g, g]
		"copper_ore", "iron_ore", "coal":
			var o := _ore(Color("8a8a86"), c, key == "coal")
			return [o, o, o]
		"meteorite":
			var o := _ore(c, Color(0.72, 0.72, 0.78), false)
			return [o, o, o]
		"basalt":
			var b := _basalt(c)
			return [_noisy(c, 0.22), b, _noisy(c, 0.22)]
		"ash":
			var a := _noisy(c, 0.3, 0.4)
			_speckle(a, _shade(c, 1.5), 6)
			return [a, a, a]
		"levistone":
			var l := _crystal(c)
			return [l, l, l]
		"magma":
			var g := _magma(c)
			return [g, g, g]
		"crop":
			var s := _stalks()
			return [s, s, s]
		"snow":
			var dirt := _dirt(Color("7a5534"))
			return [_snow_top(c), _snow_side(c, dirt), dirt]
		"ice":
			var i := _ice(c)
			return [i, i, i]
		"mud":
			var md := _mud(c)
			return [md, md, md]
		"red_sand":
			var s := _sand(c)
			return [s, s, s]
		"sandstone":
			var s := _sandstone(c)
			return [_noisy(c, 0.14, 0.25), s, _noisy(c, 0.14, 0.25)]
		"granite":
			var g := _stone(c, false)
			_speckle(g, Color(0.86, 0.72, 0.68), 9)
			_speckle(g, Color(0.2, 0.19, 0.2), 8)
			return [g, g, g]
		"limestone":
			var l := _stone(c, false)
			_speckle(l, _shade(c, 0.78), 10)
			_speckle(l, _shade(c, 1.1), 6, 2)
			return [l, l, l]
		"flint":
			var o := _ore(Color("c9c3ae"), c, false)
			return [o, o, o]
		"dry_grass":
			var dirt := _dirt(Color("7a5534"))
			var top := _grass_top(c)
			_speckle(top, Color(0.62, 0.5, 0.3), 8)
			return [top, _grass_side(c, dirt), dirt]
		"pine_log":
			var bark := _bark(c)
			for i in 10:
				bark.set_pixel(_rng.randi_range(0, 15), _rng.randi_range(0, 15), _shade(c, 0.6))
			return [_rings(c), bark, _rings(c)]
		"birch_log":
			return [_rings(Color(0.6, 0.5, 0.36)), _birch(c), _rings(Color(0.6, 0.5, 0.36))]
		"pine_leaves":
			var l := _needles(c)
			return [l, l, l]
		"birch_leaves":
			var l := _leaves(c)
			return [l, l, l]
		"fruit_leaves":
			var l := _leaves(c)
			for i in 5:
				var x := _rng.randi_range(0, SIZE - 2)
				var y := _rng.randi_range(0, SIZE - 2)
				var fruit := Color(0.86, 0.22, 0.16) if i % 2 == 0 else Color(0.95, 0.58, 0.16)
				img_blob(l, x, y, fruit)
			return [l, l, l]
		"cactus":
			var s := _cactus(c)
			return [_noisy(c, 0.2), s, _noisy(c, 0.2)]
		_:
			var n := _noisy(c)
			return [n, n, n]


func _grass_top(c: Color) -> Image:
	var img := _img()
	var pal := _ramp(c, 5, 0.26)
	_quantize(img, _fbm(0.45), pal)
	# Blades: short bright strokes and darker roots.
	for i in 22:
		var x := _rng.randi_range(0, SIZE - 1)
		var y := _rng.randi_range(0, SIZE - 1)
		img.set_pixel(x, y, _shade(pal[4], 1.06))
		img.set_pixel(x, (y + 1) % SIZE, pal[3])
	_speckle(img, pal[0], 10)
	# A few flowers and dry tips.
	if _rng.randf() < 0.9:
		img.set_pixel(_rng.randi_range(0, 15), _rng.randi_range(0, 15), Color(0.93, 0.86, 0.45))
	return img


func _grass_side(c: Color, dirt: Image) -> Image:
	var img := dirt.duplicate() as Image
	var pal := _ramp(c, 4, 0.22)
	for x in SIZE:
		var depth := 3 + (1 if _rng.randf() < 0.45 else 0) + (1 if _rng.randf() < 0.18 else 0)
		for y in depth:
			img.set_pixel(x, y, pal[clampi(3 - y + _rng.randi_range(-1, 0), 0, 3)])
		# Dark shadow line where the turf meets the soil.
		img.set_pixel(x, depth, _shade(img.get_pixel(x, depth), 0.78))
	return img


func _dirt(c: Color) -> Image:
	var img := _noisy(c, 0.26, 0.35)
	_pebbles(img, Color(0.55, 0.5, 0.45), 3)
	_speckle(img, _shade(c, 0.7), 7)
	return img


func _path(c: Color) -> Image:
	var img := _img()
	var pal := _ramp(c, 4, 0.16)
	var f := _fbm(0.2)
	for y in SIZE:
		for x in SIZE:
			# Trodden streaks along the way.
			var v := f[y * SIZE + x] * 0.8 + 0.2 * (0.5 + 0.5 * sin(float(y) * 1.3 + f[x] * 3.0))
			img.set_pixel(x, y, pal[clampi(int(v * 4.0), 0, 3)])
	_pebbles(img, Color(0.62, 0.6, 0.56), 4)
	return img


func _farmland(c: Color) -> Image:
	var img := _noisy(c, 0.2, 0.3)
	for y in SIZE:
		var ridge := y % 4
		for x in SIZE:
			var p := img.get_pixel(x, y)
			if ridge == 0:
				img.set_pixel(x, y, _shade(p, 1.28))
			elif ridge == 3:
				img.set_pixel(x, y, _shade(p, 0.68))
	return img


func _stone(c: Color, wet: bool) -> Image:
	var img := _noisy(c, 0.2, 0.3)
	var dark := _shade(c, 0.66)
	# Cracks: short random walks.
	for k in 3:
		var x := _rng.randi_range(0, SIZE - 1)
		var y := _rng.randi_range(0, SIZE - 1)
		for s in _rng.randi_range(3, 6):
			img.set_pixel(x % SIZE, y % SIZE, dark)
			x += _rng.randi_range(0, 1)
			y += _rng.randi_range(-1, 1) if _rng.randf() < 0.5 else 1
			x = posmod(x, SIZE)
			y = posmod(y, SIZE)
	_speckle(img, _shade(c, 1.22), 5)
	if wet:
		# The spring: glossy wet stone with blue seeping through.
		for y in SIZE:
			for x in SIZE:
				var p := img.get_pixel(x, y)
				var blue := Color(0.42, 0.72, 0.92)
				img.set_pixel(x, y, Color(p.lerp(blue, 0.45 if (x + y * 3) % 7 < 3 else 0.2), 0.55))
	return img


## Voronoi cobbles: `n` stones, each its own shade, with dark joints.
func _cobble(c: Color, n: int) -> Image:
	var img := _img()
	var pts: Array[Vector2] = []
	var shades: Array[float] = []
	for i in n:
		pts.append(Vector2(_rng.randf() * SIZE, _rng.randf() * SIZE))
		shades.append(_rng.randf_range(0.82, 1.18))
	for y in SIZE:
		for x in SIZE:
			var best := 1e9
			var second := 1e9
			var bi := 0
			for i in n:
				for oy in [-SIZE, 0, SIZE]:
					for ox in [-SIZE, 0, SIZE]:
						var d := Vector2(x + 0.5 + ox, y + 0.5 + oy).distance_to(pts[i])
						if d < best:
							second = best
							best = d
							bi = i
						elif d < second:
							second = d
			var k: float = shades[bi]
			if second - best < 1.1:
				k = 0.58
			elif best < 1.6:
				k *= 1.1
			img.set_pixel(x, y, _shade(c, k * (1.0 + (_rng.randf() - 0.5) * 0.08)))
	return img


func _gravel(c: Color) -> Image:
	var img := _noisy(c, 0.18, 0.2)
	for i in 26:
		var x := _rng.randi_range(0, SIZE - 1)
		var y := _rng.randi_range(0, SIZE - 1)
		var k := _rng.randf_range(0.75, 1.3)
		img.set_pixel(x, y, _shade(c, k * 1.08))
		img.set_pixel((x + 1) % SIZE, y, _shade(c, k))
		img.set_pixel(x, (y + 1) % SIZE, _shade(c, k * 0.75))
	return img


func _sand(c: Color) -> Image:
	var img := _img()
	var pal := _ramp(c, 4, 0.12)
	var f := _fbm(0.5)
	for y in SIZE:
		for x in SIZE:
			# Wind ripples.
			var v := f[y * SIZE + x] * 0.7 + 0.3 * (0.5 + 0.5 * sin(float(x + y * 2) * 0.9))
			img.set_pixel(x, y, pal[clampi(int(v * 4.0), 0, 3)])
	_speckle(img, _shade(c, 0.72), 6)
	_speckle(img, _shade(c, 1.12), 6)
	return img


func _layered(c: Color) -> Image:
	var img := _noisy(c, 0.12, 0.2)
	for y in SIZE:
		if y % 5 == 2:
			for x in SIZE:
				img.set_pixel(x, y, _shade(img.get_pixel(x, y), 0.86))
	return img


func _bark(c: Color) -> Image:
	var img := _img()
	var pal := _ramp(c, 5, 0.3)
	var cols := PackedFloat32Array()
	for x in SIZE:
		cols.append(_rng.randf())
	var f := _noise(4)
	for y in SIZE:
		for x in SIZE:
			# Vertical furrows, wavering a little.
			var v := cols[x] * 0.65 + f[y * SIZE + x] * 0.35
			if x % 4 == int(y / 5) % 4:
				v *= 0.5
			img.set_pixel(x, y, pal[clampi(int(v * 5.0), 0, 4)])
	return img


func _rings(c: Color) -> Image:
	var img := _img()
	var wood := Color(0.74, 0.58, 0.38).lerp(c, 0.25)
	var pal := _ramp(wood, 4, 0.14)
	var bark := _ramp(c, 3, 0.2)
	var cx := 7.5 + _rng.randf_range(-0.8, 0.8)
	var cy := 7.5 + _rng.randf_range(-0.8, 0.8)
	for y in SIZE:
		for x in SIZE:
			if x == 0 or y == 0 or x == SIZE - 1 or y == SIZE - 1:
				img.set_pixel(x, y, bark[_rng.randi_range(0, 2)])
				continue
			var d := Vector2(x + 0.5 - cx, y + 0.5 - cy).length()
			var ring := int(d * 1.15) % 2
			img.set_pixel(x, y, pal[1 + ring] if d > 1.2 else pal[3])
	return img


func _leaves(c: Color) -> Image:
	var img := _img()
	var pal := _ramp(c, 5, 0.34)
	var f := _fbm(0.55)
	_quantize(img, f, pal)
	# Gaps into the dark crown and light catching leaf edges.
	for i in 16:
		var x := _rng.randi_range(0, SIZE - 1)
		var y := _rng.randi_range(0, SIZE - 1)
		img.set_pixel(x, y, _shade(pal[0], 0.62))
	for i in 12:
		var x := _rng.randi_range(0, SIZE - 1)
		var y := _rng.randi_range(0, SIZE - 1)
		img.set_pixel(x, y, _shade(pal[4], 1.08))
	return img


func _planks(c: Color, door: bool) -> Image:
	var img := _img()
	var pal := _ramp(c, 5, 0.18)
	for y in SIZE:
		var board := int(y / 4)
		for x in SIZE:
			var bx := x if not door else y
			var by := y if not door else x
			var v := 0.5 + 0.18 * sin(float(bx) * 0.7 + float(board) * 2.1) + (_rng.randf() - 0.5) * 0.18
			var col := pal[clampi(int(v * 5.0), 1, 4)]
			if by % 4 == 3:
				col = pal[0]  # seam
			elif by % 4 == 0:
				col = _shade(col, 1.06)
			img.set_pixel(x, y, col)
	if door:
		# A frame and an iron handle.
		for i in SIZE:
			img.set_pixel(0, i, pal[0])
			img.set_pixel(SIZE - 1, i, pal[0])
			img.set_pixel(i, 0, pal[0])
		img.set_pixel(11, 8, Color(0.28, 0.28, 0.3))
		img.set_pixel(11, 9, Color(0.45, 0.45, 0.48))
	return img


func _bricks(c: Color, mortar: Color) -> Image:
	var img := _img()
	for y in SIZE:
		var row := int(y / 4)
		var off := 4 if row % 2 == 1 else 0
		for x in SIZE:
			var mx := (x + off) % 8
			var col: Color
			if y % 4 == 3 or mx == 7:
				col = _shade(mortar, 0.92 + _rng.randf() * 0.1)
			else:
				var brick := int((x + off) / 8) + row * 3
				var k := 0.86 + fposmod(float(brick) * 0.37, 0.28)
				col = _shade(c, k * (1.0 + (_rng.randf() - 0.5) * 0.1))
				if y % 4 == 0:
					col = _shade(col, 1.08)
			img.set_pixel(x, y, col)
	return img


func _blocks(c: Color) -> Image:
	var img := _noisy(c, 0.12, 0.25)
	for y in SIZE:
		for x in SIZE:
			var bx := x % 8
			var by := (y + (4 if x >= 8 else 0)) % 8
			var p := img.get_pixel(x, y)
			if bx == 7 or by == 7:
				img.set_pixel(x, y, _shade(c, 0.62))
			elif bx == 0 or by == 0:
				img.set_pixel(x, y, _shade(p, 1.14))
	return img


func _thatch(c: Color, top: bool) -> Image:
	var img := _img()
	var pal := _ramp(c, 5, 0.26)
	for x in SIZE:
		var phase := _rng.randi_range(0, 7)
		for y in SIZE:
			var v := 0.55 + 0.35 * sin(float(y + phase) * (0.9 if top else 0.45)) + (_rng.randf() - 0.5) * 0.3
			var col := pal[clampi(int(v * 5.0), 0, 4)]
			if not top and y % 5 == 4:
				col = _shade(pal[0], 0.9)  # the edges of the bundles
			img.set_pixel(x if not top else (x + y) % SIZE, y, col)
	return img


func _glass(c: Color) -> Image:
	var img := _img()
	var frame := Color(0.42, 0.33, 0.24)
	for y in SIZE:
		for x in SIZE:
			var edge := x == 0 or y == 0 or x == SIZE - 1 or y == SIZE - 1 or x == 7 or y == 7
			if edge:
				img.set_pixel(x, y, frame)
			else:
				var hl := (x + y) % 11 < 2 and x < 7 and y < 7
				var col := c.lightened(0.35) if hl else _shade(c, 0.92 + 0.08 * float(y) / 16.0)
				img.set_pixel(x, y, Color(col, 0.25))
	return img


func _ore(stone: Color, nugget: Color, dull: bool) -> Image:
	var img := _stone(stone, false)
	var lit := nugget.lightened(0.35)
	var dark := _shade(nugget, 0.6)
	for i in 5:
		var x := _rng.randi_range(1, SIZE - 3)
		var y := _rng.randi_range(1, SIZE - 3)
		var shape := [[0, 0], [1, 0], [0, 1], [1, 1], [2, 1]]
		for p in shape:
			if _rng.randf() < 0.85:
				img.set_pixel(x + p[0], y + p[1], Color(nugget, 1.0 if dull else 0.45))
		img.set_pixel(x, y, Color(lit, 1.0 if dull else 0.3))
		img.set_pixel(x + 1, y + 1, Color(dark, 1.0 if dull else 0.6))
	return img


func _basalt(c: Color) -> Image:
	var img := _noisy(c, 0.18, 0.25)
	for y in SIZE:
		for x in [0, 5, 11]:
			var xx: int = (x + (1 if (y / 4) % 2 == 1 else 0)) % SIZE
			img.set_pixel(xx, y, _shade(c, 0.6))
			img.set_pixel((xx + 1) % SIZE, y, _shade(img.get_pixel((xx + 1) % SIZE, y), 1.15))
	return img


func _crystal(c: Color) -> Image:
	var img := _img()
	var pal := _ramp(c, 5, 0.3)
	var f := _noise(4)
	for y in SIZE:
		for x in SIZE:
			# Facets: diagonal bands.
			var v := fposmod(float(x) * 0.31 + float(y) * 0.17 + f[y * SIZE + x] * 0.8, 1.0)
			img.set_pixel(x, y, Color(pal[clampi(int(v * 4.0), 0, 3)], 0.5))
	# Bright veins (these glow).
	for k in 2:
		var x := _rng.randi_range(0, SIZE - 1)
		for y in SIZE:
			img.set_pixel(x, y, Color(c.lightened(0.6), 0.3))
			x = posmod(x + _rng.randi_range(-1, 1), SIZE)
	return img


func _magma(c: Color) -> Image:
	var img := _img()
	var f := _fbm(0.2)
	for y in SIZE:
		for x in SIZE:
			var v := f[y * SIZE + x]
			var col: Color
			if v < 0.38:
				col = Color(0.18, 0.08, 0.06)  # cooled crust
			elif v < 0.55:
				col = _shade(c, 0.75)
			else:
				col = c.lerp(Color(1.0, 0.85, 0.35), (v - 0.55) * 1.8)
			img.set_pixel(x, y, col)
	return img


## Neutral stalks: the crop's colour (green → gold as it ripens) comes from the mesh.
func _stalks() -> Image:
	var img := _img()
	for x in SIZE:
		var k := 0.8 + 0.25 * float((x * 7) % 3) / 2.0
		for y in SIZE:
			var v := k * (0.85 + 0.15 * float(y) / 16.0)
			if x % 3 == 2:
				v *= 0.7
			if y < 4 and x % 3 != 2:
				v *= 1.12  # ears
			img.set_pixel(x, y, Color(v, v, v))
	return img


## Crack overlays for damaged cubes (alpha = crack).
func _cracks(stage: int) -> Image:
	var img := _img()
	img.fill(Color(0, 0, 0, 0))
	var walks := 2 + stage * 2
	for k in walks:
		var x := _rng.randi_range(2, SIZE - 3)
		var y := _rng.randi_range(2, SIZE - 3)
		var dx := _rng.randi_range(-1, 1)
		var dy := 1 if dx == 0 else _rng.randi_range(-1, 1)
		for s in 4 + stage * 2:
			img.set_pixel(posmod(x, SIZE), posmod(y, SIZE), Color(0, 0, 0, 0.85))
			x += dx if _rng.randf() < 0.7 else _rng.randi_range(-1, 1)
			y += dy if _rng.randf() < 0.7 else _rng.randi_range(-1, 1)
	return img


func img_blob(img: Image, x: int, y: int, c: Color) -> void:
	img.set_pixel(x, y, c.lightened(0.25))
	img.set_pixel(x + 1, y, c)
	img.set_pixel(x, y + 1, c)
	img.set_pixel(x + 1, y + 1, _shade(c, 0.7))


func _snow_top(c: Color) -> Image:
	var img := _noisy(c, 0.06, 0.12)
	# Faint blue hollows and glints.
	_speckle(img, Color(0.8, 0.86, 0.95), 10, 2)
	_speckle(img, Color(1, 1, 1), 8)
	return img


func _snow_side(c: Color, dirt: Image) -> Image:
	var img := dirt.duplicate() as Image
	var pal := _ramp(c, 3, 0.06)
	for x in SIZE:
		var depth := 3 + (1 if _rng.randf() < 0.5 else 0) + (1 if _rng.randf() < 0.2 else 0)
		for y in depth:
			img.set_pixel(x, y, pal[_rng.randi_range(0, 2)])
		img.set_pixel(x, depth, _shade(img.get_pixel(x, depth), 0.8))
	return img


func _ice(c: Color) -> Image:
	var img := _img()
	var pal := _ramp(c, 4, 0.12)
	var f := _fbm(0.2)
	for y in SIZE:
		for x in SIZE:
			img.set_pixel(x, y, Color(pal[clampi(int(f[y * SIZE + x] * 4.0), 0, 3)], 0.4))
	# White fracture lines.
	for k in 3:
		var x := _rng.randi_range(0, SIZE - 1)
		var y := _rng.randi_range(0, SIZE - 1)
		for s in _rng.randi_range(4, 8):
			img.set_pixel(posmod(x, SIZE), posmod(y, SIZE), Color(0.95, 0.98, 1.0, 0.3))
			x += _rng.randi_range(-1, 1)
			y += 1
	return img


func _mud(c: Color) -> Image:
	var img := _noisy(c, 0.22, 0.3)
	# Wet, glossy puddles (alpha < 1 marks glossy texels).
	var f := _noise(4)
	for y in SIZE:
		for x in SIZE:
			if f[y * SIZE + x] > 0.68:
				var p := img.get_pixel(x, y)
				img.set_pixel(x, y, Color(_shade(p, 0.85).lerp(Color(0.45, 0.43, 0.4), 0.2), 0.7))
	_pebbles(img, Color(0.45, 0.4, 0.34), 2)
	return img


func _sandstone(c: Color) -> Image:
	var img := _noisy(c, 0.12, 0.2)
	# Horizontal strata in warm bands.
	for y in SIZE:
		var k := 1.0 + 0.1 * sin(float(y) * 1.1)
		if y % 5 == 4:
			k = 0.8
		for x in SIZE:
			img.set_pixel(x, y, _shade(img.get_pixel(x, y), k))
	return img


func _birch(c: Color) -> Image:
	var img := _noisy(c, 0.08, 0.15)
	# Black lenticels in short horizontal dashes.
	for i in 9:
		var x := _rng.randi_range(0, SIZE - 4)
		var y := _rng.randi_range(0, SIZE - 1)
		for dx in _rng.randi_range(2, 4):
			img.set_pixel(x + dx, y, Color(0.16, 0.15, 0.14))
	return img


func _needles(c: Color) -> Image:
	var img := _img()
	var pal := _ramp(c, 5, 0.36)
	_quantize(img, _fbm(0.6), pal)
	# Short diagonal needle strokes.
	for i in 18:
		var x := _rng.randi_range(0, SIZE - 1)
		var y := _rng.randi_range(0, SIZE - 1)
		var d := 1 if i % 2 == 0 else -1
		for k in 3:
			img.set_pixel(posmod(x + k * d, SIZE), posmod(y + k, SIZE), pal[4] if k == 0 else pal[3])
	for i in 14:
		img.set_pixel(_rng.randi_range(0, SIZE - 1), _rng.randi_range(0, SIZE - 1), _shade(pal[0], 0.6))
	return img


func _cactus(c: Color) -> Image:
	var img := _img()
	var pal := _ramp(c, 4, 0.22)
	for y in SIZE:
		for x in SIZE:
			var rib := x % 4
			var col: Color = pal[3] if rib == 1 else (pal[0] if rib == 3 else pal[2])
			img.set_pixel(x, y, col)
	# Pale spines along the ribs.
	for y in range(1, SIZE, 3):
		for x in range(1, SIZE, 4):
			img.set_pixel(x, y, Color(0.93, 0.9, 0.78))
	return img
