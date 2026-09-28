class_name WorldMenu
extends PanelContainer
## 世界: start a new island from a seed, save to and load from slots (with what each
## holds), or leave. Opened at launch, with Esc, or from the top bar; time stands still
## while it is open.

signal closed

var _col: VBoxContainer
var _scroll: ScrollContainer
var _seed_edit: LineEdit
## New-world options: [config value, label, tooltip].
const LAYOUTS := [
	["random", "随机空岛", "按种子随机生成的大空岛：山脉、河流、湖泊、多种生态群系与自然资源；可以选择周围是否有海、资源多少、大小、地形与气候。"],
	["continent", "广袤大陆", "第二版本的大陆：山脉、湖泊与多种生态群系，最多三个文明。"],
	["classic", "经典小岛", "第一版本的小岛：村落、田地、峡谷上的桥与山泉。"],
]
const ERAS := [
	["wild", "蛮荒", "一群衣不蔽体的人造人围着篝火：露宿、采集野果、赤手狩猎。科技与村落要从零摸索。"],
	["tribal", "部落", "篝火旁的窝棚、石器、兽皮与木矛：会打猎、会搭窝棚的部落。"],
	["village", "村落", "茅屋、仓库、灶房与水边的麦田：已经会耕种的村落。"],
]
const CIVS := [
	["1", "一", "空岛上只有一个文明。"],
	["2", "二", "两个文明各据一方。"],
	["3", "三", "三个文明各据一方（「经典小岛」只容得下一个）。"],
	["4", "四", "四个文明各据一方（需要「随机空岛」）。"],
]
# The random island's options (shown only for 随机空岛).
const SEAS := [
	["1", "环海", "岛的四周是一圈海：海水是咸的，不能喝也不能浇地；外海有鱼群，学会造木船才能去捕。海的外缘有看不见的空气墙，海平面永远不变。"],
	["0", "悬空", "没有海：空岛悬在云上，河流在岛边的湖里汇聚。"],
]
const RICHNESS := [
	["0", "贫瘠", "树木、野果、野麦、猎物、鱼群与矿脉都少：生存更难。"],
	["1", "普通", "寻常的丰饶程度。"],
	["2", "丰饶", "树木、野果、猎物、鱼群与矿脉都更多。"],
]
const SIZES := [
	["0", "中", "比「广袤大陆」略大。"],
	["1", "大", "再大一半：更长的河、更远的邻国。"],
]
const RELIEFS := [
	["0", "平缓", "丘陵低缓、山脉矮小。"],
	["1", "起伏", "丘陵与一两条山脉。"],
	["2", "险峻", "高山连绵、河谷深切。"],
]
const CLIMATES := [
	["0", "多样", "从雪原到荒漠：冷暖干湿随地而异，各种群系都有。"],
	["1", "温和", "大多是草原与森林。"],
	["2", "寒冷", "针叶林与雪原居多。"],
	["3", "炎热", "炎热干燥：稀树草原与荒漠居多。"],
	["4", "湿润", "森林与沼泽居多。"],
]
var _options := {"layout": "random", "era": "wild", "civs": "4", "sea": "1", "richness": "1", "size": "1", "relief": "1", "climate": "0"}
var _random_rows: Array[Control] = []
var _preview: TextureRect
var _preview_note: Label
var _preview_thread: Thread
var _preview_wanted := ""   # the config (as text) the picture should show
var _preview_shown := ""    # the config the picture shows (or is being drawn for)
var _preview_wait := 0.0
var _status: Label
var _slot_box: VBoxContainer
var _resume: Button
var _was_paused := false

const SLOT_NAMES := {"auto": "自动存档", "1": "存档一", "2": "存档二", "3": "存档三"}

const HELP := [
	["右键 / 中键拖动", "旋转 / 平移镜头；滚轮缩放；WASD、Q/E 也可移动与旋转"],
	["左键", "使用底部工具；「观察」时点击居民、魔法少女或建筑查看详情"],
	["空格 · 1–4", "暂停 · 1×、2×、5×、20× 速度"],
	["J 议事录", "魔法少女的每项决策：她知道的局势、可行选项、谁的主张、她的理由"],
	["C 编年史", "历史与因果链：点任一事件，看它从何而来、引出了什么"],
	["T 科技", "四个时代的科技树与当前研究"],
	["神迹", "赐粮、鼓舞、恐吓、治愈、天雷、降雨；魔法少女详情页里可以向她低语"],
	["你是谁", "你不统治任何国家。改变世界，然后看魔法少女与居民如何应对"],
]


func _ready() -> void:
	var sb := UITheme.card_style(UITheme.BG_SOLID, 18, 22)
	sb.border_color = Color(UITheme.ACCENT, 0.35)
	sb.set_border_width_all(1)
	add_theme_stylebox_override("panel", sb)
	mouse_filter = Control.MOUSE_FILTER_STOP
	custom_minimum_size = Vector2(660, 0)
	# The menu scrolls when it is taller than the screen (a small window, a large
	# interface size, or the help unfolded).
	_scroll = ScrollContainer.new()
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	add_child(_scroll)
	_col = VBoxContainer.new()
	_col.add_theme_constant_override("separation", 12)
	_col.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_scroll.add_child(_col)
	_col.minimum_size_changed.connect(_fit_height)
	get_viewport().size_changed.connect(_fit_height)

	# Title.
	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", 10)
	_col.add_child(head)
	var ic := UIIcon.new("blessing", 30)
	ic.color = UITheme.ACCENT
	head.add_child(ic)
	var ht := VBoxContainer.new()
	ht.add_theme_constant_override("separation", 0)
	head.add_child(ht)
	ht.add_child(UITheme.label("Code:Icarus", 22, UITheme.TEXT, true))
	ht.add_child(UITheme.label("空岛之上，魔法少女以各自的源动力塑造文明；你是俯瞰一切的管理员。", 12, UITheme.TEXT_DIM))
	var grow := Control.new()
	grow.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(grow)
	_resume = _button("继续观察", UITheme.ACCENT)
	_resume.pressed.connect(close)
	head.add_child(_resume)

	# New world.
	_col.add_child(_section("新的空岛"))
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	_col.add_child(row)
	row.add_child(UITheme.label("种子", 13, UITheme.TEXT_DIM))
	_seed_edit = LineEdit.new()
	_seed_edit.custom_minimum_size = Vector2(150, 30)
	_seed_edit.add_theme_font_size_override("font_size", 14)
	_seed_edit.placeholder_text = "任意整数"
	row.add_child(_seed_edit)
	var dice := _button("随机", UITheme.TEXT)
	dice.pressed.connect(func() -> void:
		_seed_edit.text = str(randi_range(1, 999999))
		_want_preview())
	row.add_child(dice)
	_seed_edit.text_changed.connect(func(_t: String) -> void: _want_preview())
	var g2 := Control.new()
	g2.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(g2)
	var start := _button("开辟这片空岛", UITheme.ACCENT)
	start.pressed.connect(_new_world)
	row.add_child(start)
	# The options beside a picture of the island they make.
	var body := HBoxContainer.new()
	body.add_theme_constant_override("separation", 14)
	_col.add_child(body)
	var frame := PanelContainer.new()
	var fsb := UITheme.flat(Color(0.03, 0.05, 0.09, 0.9), 12, 6, 6)
	fsb.border_color = Color(UITheme.ACCENT, 0.25)
	fsb.set_border_width_all(1)
	frame.add_theme_stylebox_override("panel", fsb)
	frame.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	body.add_child(frame)
	var fcol := VBoxContainer.new()
	fcol.add_theme_constant_override("separation", 4)
	frame.add_child(fcol)
	_preview = TextureRect.new()
	_preview.custom_minimum_size = Vector2(200, 200)
	_preview.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_preview.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	_preview.texture_filter = CanvasItem.TEXTURE_FILTER_LINEAR
	fcol.add_child(_preview)
	_preview_note = UITheme.label("", 11, UITheme.TEXT_FAINT)
	_preview_note.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_preview_note.custom_minimum_size = Vector2(200, 0)
	_preview_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	fcol.add_child(_preview_note)
	var opts := GridContainer.new()
	opts.columns = 2
	opts.add_theme_constant_override("h_separation", 10)
	opts.add_theme_constant_override("v_separation", 6)
	opts.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	body.add_child(opts)
	for entry in [["空岛", LAYOUTS, "layout", false], ["海洋", SEAS, "sea", true], ["资源", RICHNESS, "richness", true],
			["大小", SIZES, "size", true], ["地形", RELIEFS, "relief", true], ["气候", CLIMATES, "climate", true],
			["开局时代", ERAS, "era", false], ["文明", CIVS, "civs", false]]:
		var l := UITheme.label(entry[0], 13, UITheme.TEXT_DIM)
		l.custom_minimum_size = Vector2(64, 0)
		opts.add_child(l)
		var chips := _choice(entry[1], entry[2])
		opts.add_child(chips)
		if entry[3]:
			_random_rows.append(l)
			_random_rows.append(chips)
	_col.add_child(UITheme.label("同一个种子与同样的选项总会生成同一座岛与同样的开局；之后的历史由魔法少女与你的干预写成。", 11, UITheme.TEXT_FAINT))
	_show_random_rows()

	# Slots.
	_col.add_child(_section("存档"))
	_slot_box = VBoxContainer.new()
	_slot_box.add_theme_constant_override("separation", 6)
	_col.add_child(_slot_box)

	# How to play (folded by default).
	var help_toggle := _button("玩法与操作 ▾", UITheme.TEXT_DIM)
	help_toggle.alignment = HORIZONTAL_ALIGNMENT_LEFT
	_col.add_child(help_toggle)
	var help := GridContainer.new()
	help.columns = 2
	help.add_theme_constant_override("h_separation", 14)
	help.add_theme_constant_override("v_separation", 3)
	help.visible = false
	_col.add_child(help)
	for entry in HELP:
		var k := UITheme.label(entry[0], 12, UITheme.ACCENT)
		k.custom_minimum_size = Vector2(120, 0)
		help.add_child(k)
		var d := UITheme.label(entry[1], 12, UITheme.TEXT_DIM)
		d.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		d.custom_minimum_size = Vector2(390, 0)
		help.add_child(d)
	help_toggle.pressed.connect(func() -> void:
		help.visible = not help.visible
		help_toggle.text = "玩法与操作 ▴" if help.visible else "玩法与操作 ▾")

	# Rendering: the compatibility (OpenGL) renderer as a fallback for graphics drivers
	# that draw the standard (Vulkan / Direct3D) renderer wrongly.
	_col.add_child(_section("画面"))
	var gfx := HBoxContainer.new()
	gfx.add_theme_constant_override("separation", 8)
	_col.add_child(gfx)
	var compat := RenderingServer.get_current_rendering_method() == "gl_compatibility"
	var gl := UITheme.label("当前：%s" % ("兼容渲染（OpenGL）" if compat else "标准渲染"), 12, UITheme.TEXT_DIM)
	gl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	gfx.add_child(gl)
	var sw := _button("改用标准渲染" if compat else "改用兼容渲染", UITheme.TEXT)
	sw.tooltip_text = "画面出现大片黑色或花屏时，可改用兼容渲染。重启游戏后生效。"
	sw.pressed.connect(func() -> void:
		if _set_renderer(not compat):
			_status.text = "已设置，重启游戏后生效"
			_status.add_theme_color_override("font_color", UITheme.GOOD)
		else:
			_status.text = "无法写入设置文件（游戏目录可能只读）"
			_status.add_theme_color_override("font_color", UITheme.BAD))
	gfx.add_child(sw)
	# Interface size: fitted to the window, times the player's preference.
	var ui := HBoxContainer.new()
	ui.add_theme_constant_override("separation", 4)
	_col.add_child(ui)
	var ul := UITheme.label("界面大小", 12, UITheme.TEXT_DIM)
	ul.custom_minimum_size = Vector2(72, 0)
	ul.tooltip_text = "界面随窗口大小自动缩放；在此基础上可以再放大或缩小。立即生效。"
	ui.add_child(ul)
	var group := ButtonGroup.new()
	for f in Game.UI_SCALES:
		var b := _button("%d%%" % roundi(float(f) * 100.0), UITheme.TEXT_DIM)
		b.toggle_mode = true
		b.button_group = group
		b.add_theme_color_override("font_pressed_color", UITheme.ACCENT)
		b.add_theme_stylebox_override("pressed", UITheme.flat(Color(UITheme.ACCENT, 0.18), 8, 12, 5))
		b.button_pressed = is_equal_approx(float(f), Game.ui_scale())
		var value := float(f)
		b.toggled.connect(func(on: bool) -> void:
			if on:
				Game.apply_ui_scale(value, true))
		ui.add_child(b)

	var foot := HBoxContainer.new()
	_col.add_child(foot)
	_status = UITheme.label("", 12, UITheme.GOOD)
	_status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	foot.add_child(_status)
	var quit := _button("退出游戏", UITheme.TEXT_DIM)
	quit.pressed.connect(func() -> void: get_tree().quit())
	foot.add_child(quit)


## As tall as the content, but never taller than the screen.
func _fit_height() -> void:
	var margins := get_theme_stylebox("panel").get_minimum_size().y
	var room := get_viewport_rect().size.y - 32.0 - margins
	_scroll.custom_minimum_size.y = minf(_col.get_combined_minimum_size().y, maxf(200.0, room))
	reset_size()


func open() -> void:
	if not visible:
		_was_paused = Game.paused
	Game.paused = true
	Game.speed_changed.emit(Game.speed, true)
	var info: Dictionary = Game.sim.world_info() if Game.sim.has_game() else {}
	_seed_edit.text = str(info.get("seed", randi_range(1, 999999)))
	_resume.visible = Game.sim.has_game()
	_status.text = ""
	_refresh_slots()
	visible = true
	_want_preview()
	_preview_wait = 0.0


func close() -> void:
	visible = false
	Game.paused = _was_paused
	Game.speed_changed.emit(Game.speed, Game.paused)
	closed.emit()


func _new_world() -> void:
	var config := _config()
	if Game.start_new_game(config):
		_was_paused = false
		close()
	else:
		_status.text = "无法生成世界"
		_status.add_theme_color_override("font_color", UITheme.BAD)


func _refresh_slots() -> void:
	for c in _slot_box.get_children():
		c.queue_free()
	for slot in Game.SLOTS:
		var info: Dictionary = Game.slot_info(slot)
		var card := PanelContainer.new()
		card.add_theme_stylebox_override("panel", UITheme.flat(UITheme.BG_SOFT, 10, 12, 8))
		_slot_box.add_child(card)
		var r := HBoxContainer.new()
		r.add_theme_constant_override("separation", 10)
		card.add_child(r)
		var name_l := UITheme.label(SLOT_NAMES.get(slot, slot), 14, UITheme.TEXT, true)
		name_l.custom_minimum_size = Vector2(72, 0)
		r.add_child(name_l)
		var v := VBoxContainer.new()
		v.add_theme_constant_override("separation", 0)
		v.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		r.add_child(v)
		if info.is_empty():
			v.add_child(UITheme.label("空", 13, UITheme.TEXT_FAINT))
		else:
			var top := UITheme.label("%s · %s" % [info.get("time", ""), info.get("title", "")], 13)
			top.clip_text = true
			v.add_child(top)
			v.add_child(UITheme.label("种子 %s · %s · %d 人 · 保存于 %s" % [info.get("seed", "?"), info.get("polities", ""), int(info.get("population", 0)), String(info.get("saved_at", "")).replace("T", " ")], 11, UITheme.TEXT_DIM))
		if slot != "auto" and Game.sim.has_game():
			var sv := _button("保存", UITheme.TEXT)
			sv.pressed.connect(func() -> void:
				if Game.save_slot(slot):
					_status.text = "已保存到%s" % SLOT_NAMES[slot]
					_status.add_theme_color_override("font_color", UITheme.GOOD)
				_refresh_slots())
			r.add_child(sv)
		var ld := _button("读取", UITheme.ACCENT)
		ld.disabled = info.is_empty()
		ld.pressed.connect(func() -> void:
			if Game.load_slot(slot):
				_was_paused = true
				close()
			else:
				_status.text = "读取失败：存档损坏或版本不符"
				_status.add_theme_color_override("font_color", UITheme.BAD))
		r.add_child(ld)


## Writes (or removes) override.cfg next to the game, which Godot reads at start-up.
func _set_renderer(compat: bool) -> bool:
	var dir := ProjectSettings.globalize_path("res://") if OS.has_feature("editor") else OS.get_executable_path().get_base_dir()
	var path := dir.path_join("override.cfg")
	if not compat:
		if FileAccess.file_exists(path):
			return DirAccess.remove_absolute(path) == OK
		return true
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		return false
	f.store_string("[rendering]\n\nrenderer/rendering_method=\"gl_compatibility\"\nrenderer/rendering_method.mobile=\"gl_compatibility\"\n")
	return true


## A row of mutually exclusive chips that sets _options[key].
func _choice(options: Array, key: String) -> Control:
	var h := HBoxContainer.new()
	h.add_theme_constant_override("separation", 4)
	var group := ButtonGroup.new()
	for o in options:
		var b := _button(String(o[1]), UITheme.TEXT_DIM)
		b.toggle_mode = true
		b.button_group = group
		b.tooltip_text = String(o[2])
		b.add_theme_color_override("font_pressed_color", UITheme.ACCENT)
		b.add_theme_stylebox_override("pressed", UITheme.flat(Color(UITheme.ACCENT, 0.18), 8, 12, 5))
		b.button_pressed = _options.get(key, "") == o[0]
		var value: String = o[0]
		b.toggled.connect(func(on: bool) -> void:
			if on:
				_options[key] = value
				if key == "layout":
					_show_random_rows()
				_want_preview())
		h.add_child(b)
	return h


## The random island's own options only for 随机空岛.
func _show_random_rows() -> void:
	var random: bool = _options.get("layout", "") == "random"
	for c in _random_rows:
		c.visible = random


## The new-game config from the seed field and the chosen options.
func _config() -> Dictionary:
	var s := _seed_edit.text.strip_edges()
	var seed := int(s) if s.is_valid_int() else (hash(s) & 0x7fffffff)
	var config := {"seed": maxi(1, seed), "layout": _options["layout"], "era": _options["era"],
		"civs": int(_options.get("civs", "1"))}
	if _options["layout"] == "random":
		config["sea"] = _options["sea"] == "1"
		for k in ["richness", "size", "relief", "climate"]:
			config[k] = int(_options[k])
	return config


## The picture follows the options (drawn off the main thread, a moment after the last change).
func _want_preview() -> void:
	_preview_wanted = var_to_str(_config())
	_preview_wait = 0.3


func _process(delta: float) -> void:
	if not visible:
		return
	if _preview_thread != null and not _preview_thread.is_alive():
		var img: Image = _preview_thread.wait_to_finish()
		_preview_thread = null
		if img != null:
			_preview.texture = ImageTexture.create_from_image(img)
		_preview_note.text = ""
	if _preview_wanted == _preview_shown or _preview_thread != null:
		return
	_preview_wait -= delta
	if _preview_wait > 0.0:
		return
	_preview_shown = _preview_wanted
	var cfg: Dictionary = str_to_var(_preview_wanted)
	_preview_note.text = "正在绘制…"
	_preview_thread = Thread.new()
	_preview_thread.start(func() -> Image: return Game.sim.preview_island(cfg, 200))


func _exit_tree() -> void:
	if _preview_thread != null:
		_preview_thread.wait_to_finish()


func _section(text: String) -> Control:
	var h := HBoxContainer.new()
	h.add_theme_constant_override("separation", 8)
	h.add_child(UITheme.label(text, 12, UITheme.TEXT_FAINT))
	var line := HSeparator.new()
	line.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	line.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	h.add_child(line)
	return h


func _button(text: String, color: Color) -> Button:
	var b := Button.new()
	b.text = text
	b.focus_mode = Control.FOCUS_NONE
	b.add_theme_font_size_override("font_size", 13)
	b.add_theme_color_override("font_color", color)
	b.add_theme_stylebox_override("normal", UITheme.flat(Color(1, 1, 1, 0.06), 8, 12, 5))
	b.add_theme_stylebox_override("hover", UITheme.flat(Color(1, 1, 1, 0.12), 8, 12, 5))
	b.add_theme_stylebox_override("disabled", UITheme.flat(Color(1, 1, 1, 0.02), 8, 12, 5))
	return b
