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
		_rng.seed = hash(key) & 0x7fffffff
		var faces := _paint(key, col, m)
		var top := _add(faces[0])
		var side := _add(faces[1]) if faces[1] != faces[0] else top
		var bottom := top
		if faces[2] == faces[1]:
			bottom = side
		elif faces[2] != faces[0]:
			bottom = _add(faces[2])
		map.set_pixel(int(m["id"]), 0, Color8(top, side, bottom, _flags(key, m)))
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
	if key in ["grass", "dirt", "sand", "stone", "gravel", "leaves", "clay", "path", "farmland", "basalt"]:
		f |= FLAG_NATURAL
	if key in ["levistone", "magma", "spring"]:
		f |= FLAG_GLOW
	if key in ["copper_ore", "iron_ore", "coal", "meteorite", "glass", "levistone", "spring"]:
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


## Decoration sprites: three grass tufts (in the grass colour) and four flowers.
static func build_decor(grass: Color) -> Texture2DArray:
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
	for img in imgs:
		img.generate_mipmaps()
	var arr := Texture2DArray.new()
	arr.create_from_images(imgs)
	return arr


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
