class_name HUD
extends Control
## Main overlay. Layout principle: the world stays visible. A compact time pill at the
## top, a status strip top-left, transient toasts top-right, one tool dock at the
## bottom, and contextual floating cards — no permanent side panels.

var _time_label: Label
var _day_icon: UIIcon
var _speed_buttons: Array[Button] = []
var _pause_button: Button
var _tool_buttons := {}
var _options_panel: PanelContainer
var _options_box: VBoxContainer
var _hover_card: PanelContainer
var _hover_label: RichTextLabel
var _toasts: VBoxContainer
var _status_box: HBoxContainer
var _status_labels := {}
var _top_left: VBoxContainer
var _top_center: CenterContainer
var _top_right: VBoxContainer
var _bottom_left: VBoxContainer
var _bottom_center: VBoxContainer
var _bottom_right: VBoxContainer
var _float: Control           # free-floating cards (the selection card)
var _sel_anchor := Vector2(-1, -1)
var civ_card: CivCard
var selection: SelectionCard
signal focus_requested(pos: Vector3)
signal character_requested(id: int)

var council: CouncilPanel
var web: GirlWebPanel
var chronicle: ChroniclePanel
var tech: TechPanel
var ending: EndingPanel
var menu: WorldMenu
var _council_wrap: CenterContainer
var _thinking: HBoxContainer

const TOOLS := [
	{"id": "inspect", "icon": "inspect", "label": "观察", "tip": "查看方块、地格与居民"},
	{"sep": true},
	{"id": "dig", "icon": "dig", "label": "挖除", "tip": "移除物质：留下真实的缺口，失去支撑的结构会坍塌"},
	{"id": "place", "icon": "place", "label": "创造", "tip": "凭空创造物质（会记录在编年史中）"},
	{"sep": true},
	{"id": "meteor", "icon": "meteor", "label": "陨石", "tip": "召唤陨石：撞出陨坑、点燃周边、留下陨铁"},
	{"id": "ignite", "icon": "fire", "label": "火焰", "tip": "点燃可燃物，火势会沿材料蔓延"},
	{"id": "flood", "icon": "water", "label": "洪水", "tip": "倾倒大量的水，水量守恒地流动"},
	{"sep": true},
	{"id": "miracle", "icon": "blessing", "label": "神迹", "tip": "作用于肉体与人心：赐粮、鼓舞、恐吓、治愈、天雷"},
]

## Miracles: [admin command, name, what it does].
const MIRACLES := [
	["bless_food", "赐粮", "降下真实的粮食，记入物资账本，由居民搬运入库"],
	["inspire", "鼓舞", "驱散范围内居民的恐惧与压力，留下被神眷顾的记忆"],
	["terrify", "恐吓", "范围内的人陷入恐惧并逃离此地"],
	["heal", "治愈", "伤口与断肢复原（不能起死回生）"],
	["smite", "天雷", "雷击范围内的人并引燃草木，周围的人会害怕"],
	["rain", "降雨", "乌云聚拢，全岛降雨约六小时：补满水面，浇熄露天的火"],
]

const PLACE_MATERIALS := [
	["stone", "岩石"], ["dirt", "泥土"], ["sand", "沙"], ["planks", "木板"], ["log", "原木"], ["levistone", "浮石"],
]


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	theme = UITheme.build()
	_build_layout()
	_build_time_pill()
	_build_status()
	_build_dock()
	_build_hover_card()
	_build_toasts()
	_build_council()
	Game.speed_changed.connect(_on_speed_changed)
	Game.tool_changed.connect(_on_tool_changed)
	Game.event_logged.connect(_on_event)
	_on_speed_changed(Game.speed, Game.paused)
	_on_tool_changed(Game.current_tool)


# ------------------------------------------------------------------ selection card

## Shows the selection card beside a screen point (what was clicked): to its right, or
## to its left near the right edge, vertically centred on it and kept clear of the top
## bar and the tool dock. Without a point it goes to the right side of the screen.
func place_selection_near(p: Vector2) -> void:
	_sel_anchor = p
	selection.moved_by_user = false
	_place_selection()


func _place_selection() -> void:
	if not selection.visible or selection.moved_by_user:
		return
	var view := get_viewport_rect().size
	selection.reset_size()
	var sz := selection.size
	var top := 70.0
	var bottom := view.y - 90.0
	var p := _sel_anchor
	if p.x < 0.0:
		p = Vector2(view.x - 40.0, view.y * 0.45)
	var x := p.x + 36.0
	if x + sz.x > view.x - 14.0:
		x = p.x - 36.0 - sz.x
	x = clampf(x, 14.0, view.x - sz.x - 14.0)
	var y := clampf(p.y - sz.y * 0.4, top, maxf(top, bottom - sz.y))
	selection.position = Vector2(x, y).round()


# ------------------------------------------------------------------ layout

func _build_layout() -> void:
	var margin := MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	margin.mouse_filter = Control.MOUSE_FILTER_IGNORE
	for side in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 14)
	add_child(margin)
	var col := VBoxContainer.new()
	col.mouse_filter = Control.MOUSE_FILTER_IGNORE
	margin.add_child(col)
	var top := HBoxContainer.new()
	top.mouse_filter = Control.MOUSE_FILTER_IGNORE
	col.add_child(top)
	_top_left = _column(top, true)
	_top_center = CenterContainer.new()
	_top_center.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_top_center.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	top.add_child(_top_center)
	_top_right = _column(top, true)
	_top_right.alignment = BoxContainer.ALIGNMENT_BEGIN
	var spacer := Control.new()
	spacer.size_flags_vertical = Control.SIZE_EXPAND_FILL
	spacer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	col.add_child(spacer)
	var bottom := HBoxContainer.new()
	bottom.mouse_filter = Control.MOUSE_FILTER_IGNORE
	col.add_child(bottom)
	_bottom_left = _column(bottom, true)
	_bottom_left.alignment = BoxContainer.ALIGNMENT_END
	_bottom_center = VBoxContainer.new()
	_bottom_center.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_bottom_center.alignment = BoxContainer.ALIGNMENT_END
	_bottom_center.add_theme_constant_override("separation", 8)
	bottom.add_child(_bottom_center)
	_bottom_right = _column(bottom, true)
	_bottom_right.alignment = BoxContainer.ALIGNMENT_END
	_float = Control.new()
	_float.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_float.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_float)


func _column(parent: Control, expand: bool) -> VBoxContainer:
	var v := VBoxContainer.new()
	v.mouse_filter = Control.MOUSE_FILTER_IGNORE
	if expand:
		v.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	v.size_flags_stretch_ratio = 1.0
	parent.add_child(v)
	return v


# ------------------------------------------------------------------ top: time pill

func _build_time_pill() -> void:
	var pill := PanelContainer.new()
	pill.add_theme_stylebox_override("panel", UITheme.card_style(UITheme.BG, 20, 6))
	_top_center.add_child(pill)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	pill.add_child(row)

	var left_pad := Control.new()
	left_pad.custom_minimum_size = Vector2(6, 0)
	row.add_child(left_pad)
	_day_icon = UIIcon.new("sun", 18)
	_day_icon.color = UITheme.ACCENT
	row.add_child(_day_icon)
	_time_label = UITheme.label("—", 15)
	_time_label.custom_minimum_size = Vector2(190, 0)
	_time_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	row.add_child(_time_label)
	_thinking = HBoxContainer.new()
	_thinking.add_theme_constant_override("separation", 2)
	var th_ic := UIIcon.new("think", 16)
	th_ic.color = UITheme.MAGIC
	_thinking.add_child(th_ic)
	_thinking.add_child(UITheme.label("思考中", 12, UITheme.MAGIC))
	_thinking.tooltip_text = "一位魔法少女正在等待 Claude 的答复，时间暂时停驻"
	_thinking.visible = false
	row.add_child(_thinking)
	row.add_child(VSeparator.new())

	_pause_button = _icon_button("pause", "暂停 / 继续（空格）")
	_pause_button.pressed.connect(Game.toggle_pause)
	row.add_child(_pause_button)
	var labels := ["1×", "2×", "5×", "20×"]
	for i in Game.SPEEDS.size():
		var b := Button.new()
		b.text = labels[i]
		b.toggle_mode = true
		b.focus_mode = Control.FOCUS_NONE
		b.custom_minimum_size = Vector2(40, 30)
		b.tooltip_text = "模拟速度 %s" % labels[i]
		var sp: float = Game.SPEEDS[i]
		b.pressed.connect(func() -> void: Game.set_speed(sp))
		row.add_child(b)
		_speed_buttons.append(b)
	row.add_child(VSeparator.new())
	var council_btn := _icon_button("scroll", "议事录：魔法少女的决策（J）")
	council_btn.pressed.connect(func() -> void: toggle_council())
	row.add_child(council_btn)
	var web_btn := _icon_button("bonds", "魔法少女的羁绊（B）")
	web_btn.pressed.connect(func() -> void: toggle_web())
	row.add_child(web_btn)
	var chron_btn := _icon_button("chronicle", "编年史与因果链（C）")
	chron_btn.pressed.connect(func() -> void: toggle_chronicle())
	row.add_child(chron_btn)
	var tech_btn := _icon_button("flask", "科技（T）")
	tech_btn.pressed.connect(func() -> void: toggle_tech())
	row.add_child(tech_btn)
	var menu_btn := _icon_button("gear", "世界：新空岛、存档与读档（Esc）")
	menu_btn.pressed.connect(func() -> void: open_menu())
	row.add_child(menu_btn)
	var right_pad := Control.new()
	right_pad.custom_minimum_size = Vector2(2, 0)
	row.add_child(right_pad)


func _icon_button(icon: String, tip: String, size_px := 30.0) -> Button:
	var b := Button.new()
	b.focus_mode = Control.FOCUS_NONE
	b.custom_minimum_size = Vector2(size_px + 4, size_px)
	b.tooltip_text = tip
	var ic := UIIcon.new(icon, size_px * 0.6)
	ic.set_anchors_preset(Control.PRESET_CENTER)
	ic.position = -ic.custom_minimum_size * 0.5
	b.add_child(ic)
	b.set_meta("icon", ic)
	return b


func _on_speed_changed(speed: float, paused: bool) -> void:
	for i in _speed_buttons.size():
		_speed_buttons[i].button_pressed = (not paused) and is_equal_approx(Game.SPEEDS[i], speed)
	var ic: UIIcon = _pause_button.get_meta("icon")
	ic.set_icon("play" if paused else "pause")
	ic.set_color(UITheme.ACCENT if paused else UITheme.TEXT)


# ------------------------------------------------------------------ top-left: status

func _build_status() -> void:
	civ_card = CivCard.new()
	civ_card.sim = Game.sim
	_top_left.add_child(civ_card)
	selection = SelectionCard.new()
	selection.sim = Game.sim
	_float.add_child(selection)
	selection.resized.connect(func() -> void: _place_selection())
	selection.visibility_changed.connect(func() -> void: _place_selection())
	selection.closed.connect(func() -> void:
		selection.visible = false
		Game.select({}))
	civ_card.council_requested.connect(func() -> void: toggle_council())
	civ_card.tech_requested.connect(func() -> void: toggle_tech())
	civ_card.event_requested.connect(func(id: int) -> void: toggle_chronicle(id))
	selection.decision_requested.connect(func(id: int) -> void: toggle_council(id))
	selection.event_requested.connect(func(id: int) -> void: toggle_chronicle(id))


func _process(_delta: float) -> void:
	if Game.sim == null or not Game.sim.has_game():
		return
	var ci: Dictionary = Game.sim.clock_info()
	_time_label.text = ci.get("text", "")
	var night: bool = ci.get("night", false)
	_day_icon.set_icon("moon" if night else "sun")
	_thinking.visible = Game.holding


# ------------------------------------------------------------------ bottom: tool dock

func _build_dock() -> void:
	var dock := PanelContainer.new()
	dock.add_theme_stylebox_override("panel", UITheme.card_style(UITheme.BG, 16, 6))
	dock.size_flags_horizontal = Control.SIZE_SHRINK_CENTER
	_bottom_center.add_child(dock)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 2)
	dock.add_child(row)
	for t in TOOLS:
		if t.has("sep"):
			var s := VSeparator.new()
			s.custom_minimum_size = Vector2(10, 0)
			row.add_child(s)
			continue
		var b := Button.new()
		b.toggle_mode = true
		b.focus_mode = Control.FOCUS_NONE
		b.custom_minimum_size = Vector2(62, 58)
		b.tooltip_text = t["tip"]
		var v := VBoxContainer.new()
		v.set_anchors_preset(Control.PRESET_FULL_RECT)
		v.alignment = BoxContainer.ALIGNMENT_CENTER
		v.mouse_filter = Control.MOUSE_FILTER_IGNORE
		v.add_theme_constant_override("separation", 2)
		var ic := UIIcon.new(t["icon"], 24)
		ic.size_flags_horizontal = Control.SIZE_SHRINK_CENTER
		v.add_child(ic)
		var l := UITheme.label(t["label"], 12, UITheme.TEXT_DIM)
		l.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		l.mouse_filter = Control.MOUSE_FILTER_IGNORE
		v.add_child(l)
		b.add_child(v)
		b.set_meta("icon", ic)
		b.set_meta("label", l)
		var id: String = t["id"]
		b.pressed.connect(func() -> void: Game.set_tool(id))
		row.add_child(b)
		_tool_buttons[id] = b

	# Options popover sits just above the dock.
	_options_panel = PanelContainer.new()
	_options_panel.add_theme_stylebox_override("panel", UITheme.card_style(UITheme.BG, 12, 10))
	_options_panel.size_flags_horizontal = Control.SIZE_SHRINK_CENTER
	_bottom_center.add_child(_options_panel)
	_bottom_center.move_child(_options_panel, 0)
	_options_box = VBoxContainer.new()
	_options_box.add_theme_constant_override("separation", 8)
	_options_panel.add_child(_options_box)


func _on_tool_changed(tool: String) -> void:
	for id in _tool_buttons:
		var b: Button = _tool_buttons[id]
		var on: bool = id == tool
		b.button_pressed = on
		(b.get_meta("icon") as UIIcon).set_color(UITheme.ACCENT if on else UITheme.TEXT)
		(b.get_meta("label") as Label).add_theme_color_override("font_color", UITheme.ACCENT if on else UITheme.TEXT_DIM)
	_rebuild_options(tool)


func _rebuild_options(tool: String) -> void:
	for c in _options_box.get_children():
		c.queue_free()
	var show := tool in ["dig", "place", "meteor", "ignite", "flood", "miracle"]
	_options_panel.visible = show
	if not show:
		return
	if tool == "miracle":
		_build_miracle_options()
		return
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 10)
	_options_box.add_child(row)
	row.add_child(UITheme.label("范围", 13, UITheme.TEXT_DIM))
	var slider := HSlider.new()
	slider.min_value = 0.5
	slider.max_value = 12.0 if tool != "meteor" else 14.0
	slider.step = 0.5
	slider.custom_minimum_size = Vector2(180, 18)
	slider.value = Game.tool_params.get("radius", 3.0)
	slider.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	row.add_child(slider)
	var val := UITheme.label("%.1f" % slider.value, 13, UITheme.ACCENT)
	val.custom_minimum_size = Vector2(32, 0)
	row.add_child(val)
	slider.value_changed.connect(func(v: float) -> void:
		Game.tool_params["radius"] = v
		val.text = "%.1f" % v)
	if tool == "place":
		var mats := HBoxContainer.new()
		mats.add_theme_constant_override("separation", 4)
		_options_box.add_child(mats)
		for m in PLACE_MATERIALS:
			var b := Button.new()
			b.text = m[1]
			b.toggle_mode = true
			b.focus_mode = Control.FOCUS_NONE
			b.button_pressed = Game.tool_params.get("material", "stone") == m[0]
			var key: String = m[0]
			b.pressed.connect(func() -> void:
				Game.tool_params["material"] = key
				for other in mats.get_children():
					(other as Button).button_pressed = other == b)
			mats.add_child(b)


func _build_miracle_options() -> void:
	var kind: String = Game.tool_params.get("miracle", "bless_food")
	var chips := HBoxContainer.new()
	chips.add_theme_constant_override("separation", 4)
	_options_box.add_child(chips)
	var desc := ""
	for m in MIRACLES:
		var b := Button.new()
		b.text = m[1]
		b.toggle_mode = true
		b.focus_mode = Control.FOCUS_NONE
		b.button_pressed = kind == m[0]
		b.tooltip_text = m[2]
		var key: String = m[0]
		b.pressed.connect(func() -> void:
			Game.tool_params["miracle"] = key
			_rebuild_options("miracle"))
		chips.add_child(b)
		if kind == m[0]:
			desc = m[2]
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 10)
	_options_box.add_child(row)
	var food := kind == "bless_food"
	row.add_child(UITheme.label("数量" if food else "范围", 13, UITheme.TEXT_DIM))
	var slider := HSlider.new()
	slider.min_value = 10.0 if food else 1.0
	slider.max_value = 200.0 if food else (3.0 if kind == "smite" else 20.0)
	slider.step = 10.0 if food else 0.5
	slider.custom_minimum_size = Vector2(180, 18)
	slider.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	var param := "amount" if food else "radius"
	slider.value = Game.tool_params.get(param, 40.0 if food else 3.0)
	row.add_child(slider)
	var val := UITheme.label(("%d 份" % int(slider.value)) if food else ("%.1f" % slider.value), 13, UITheme.ACCENT)
	val.custom_minimum_size = Vector2(44, 0)
	row.add_child(val)
	slider.value_changed.connect(func(v: float) -> void:
		Game.tool_params[param] = v
		val.text = ("%d 份" % int(v)) if food else ("%.1f" % v))
	_options_box.add_child(UITheme.label(desc, 12, UITheme.TEXT_FAINT))


# ------------------------------------------------------------------ hover card

func _build_hover_card() -> void:
	_hover_card = PanelContainer.new()
	_hover_card.add_theme_stylebox_override("panel", UITheme.card_style(UITheme.BG, 10, 10))
	_hover_card.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	_hover_card.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_bottom_left.add_child(_hover_card)
	_hover_label = RichTextLabel.new()
	_hover_label.bbcode_enabled = true
	_hover_label.fit_content = true
	_hover_label.scroll_active = false
	_hover_label.custom_minimum_size = Vector2(240, 0)
	_hover_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_hover_label.add_theme_font_size_override("normal_font_size", 13)
	_hover_label.add_theme_font_override("bold_font", UITheme.font_bold())
	_hover_label.add_theme_font_size_override("bold_font_size", 14)
	_hover_card.add_child(_hover_label)
	_hover_card.visible = false


func show_hover(info: Dictionary) -> void:
	if info.is_empty():
		_hover_card.visible = false
		return
	_hover_card.visible = true
	var cell: Dictionary = info.get("cell", {})
	var states := {"ungenerated": "未生成", "dormant": "休眠", "active": "活跃"}
	var t := "[b]%s[/b]" % info.get("material_name", "")
	if info.get("burning", false):
		t += "  [color=#f0a050]燃烧中[/color]"
	if info.get("material", "") == "water":
		t += "  [color=#8fb8d8]水量 %d/15[/color]" % info.get("level", 0)
	t += "\n[color=#9aa3b5]方块 %s[/color]" % str(info.get("cube", ""))
	if not cell.is_empty():
		t += "\n[color=#9aa3b5]地格 %s · %s · %s[/color]" % [str(cell.get("coord")), cell.get("biome", ""), states.get(cell.get("state", ""), "")]
	_hover_label.text = t


# ------------------------------------------------------------------ council overlay

func _build_council() -> void:
	_council_wrap = CenterContainer.new()
	_council_wrap.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_council_wrap.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_council_wrap)
	council = CouncilPanel.new()
	council.sim = Game.sim
	council.visible = false
	_council_wrap.add_child(council)
	chronicle = ChroniclePanel.new()
	chronicle.sim = Game.sim
	chronicle.visible = false
	_council_wrap.add_child(chronicle)
	tech = TechPanel.new()
	tech.sim = Game.sim
	tech.visible = false
	_council_wrap.add_child(tech)
	ending = EndingPanel.new()
	ending.sim = Game.sim
	ending.visible = false
	_council_wrap.add_child(ending)
	ending.trace_requested.connect(func(id: int) -> void: toggle_chronicle(id))
	menu = WorldMenu.new()
	menu.visible = false
	_council_wrap.add_child(menu)
	web = GirlWebPanel.new()
	web.sim = Game.sim
	web.visible = false
	_council_wrap.add_child(web)
	web.character_requested.connect(func(id: int) -> void:
		web.visible = false
		character_requested.emit(id))
	chronicle.focus_requested.connect(func(p: Vector3) -> void:
		chronicle.visible = false
		focus_requested.emit(p))
	chronicle.decision_requested.connect(func(id: int) -> void: toggle_council(id))
	council.event_requested.connect(func(id: int) -> void: toggle_chronicle(id))
	get_viewport().size_changed.connect(_size_council)
	_size_council()


func _size_council() -> void:
	var vp := get_viewport_rect().size
	var sz := Vector2(minf(1080.0, vp.x * 0.9), minf(660.0, vp.y * 0.82))
	council.custom_minimum_size = sz
	chronicle.custom_minimum_size = sz
	tech.custom_minimum_size = sz
	web.custom_minimum_size = sz


## Opens the decision ledger (at a decision if given); only one overlay at a time.
func toggle_council(id: int = 0) -> void:
	if council.visible and id == 0:
		council.visible = false
	else:
		chronicle.visible = false
		tech.visible = false
		web.visible = false
		council.open_at(id)


## Opens the web of ties between the magical girls.
func toggle_web() -> void:
	if web.visible:
		web.visible = false
	else:
		council.visible = false
		chronicle.visible = false
		tech.visible = false
		web.open()


## Opens the chronicle (at an event if given).
func toggle_chronicle(id: int = 0) -> void:
	if chronicle.visible and id == 0:
		chronicle.visible = false
	else:
		council.visible = false
		tech.visible = false
		web.visible = false
		chronicle.open_at(id)


## Opens the technology tree of the polity shown in the civilisation card.
func toggle_tech(key := "") -> void:
	if tech.visible:
		tech.visible = false
	else:
		council.visible = false
		chronicle.visible = false
		web.visible = false
		tech.open_for(civ_card.polity_id, key)


func overlay_open() -> bool:
	return council.visible or chronicle.visible or tech.visible or ending.visible or menu.visible or web.visible


func close_overlays() -> void:
	web.visible = false
	council.visible = false
	chronicle.visible = false
	tech.visible = false
	ending.visible = false
	if menu.visible:
		menu.close()


## The world menu: new island, saves, quit.
func open_menu() -> void:
	web.visible = false
	council.visible = false
	chronicle.visible = false
	tech.visible = false
	ending.visible = false
	menu.open()


## After a new or loaded world: forget what belonged to the old one.
func on_world_changed() -> void:
	web.visible = false
	council.visible = false
	chronicle.visible = false
	tech.visible = false
	ending.visible = false
	for c in _toasts.get_children():
		c.queue_free()
	selection.visible = false
	civ_card.polity_id = 1
	civ_card._switch_sig = ""
	civ_card._war_sig = ""


## The round's end card (when the island has been unified).
func show_ending() -> void:
	web.visible = false
	council.visible = false
	chronicle.visible = false
	tech.visible = false
	ending.open()


# ------------------------------------------------------------------ toasts

func _build_toasts() -> void:
	_toasts = VBoxContainer.new()
	_toasts.size_flags_horizontal = Control.SIZE_SHRINK_END
	_toasts.add_theme_constant_override("separation", 6)
	_toasts.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_top_right.add_child(_toasts)
	# Narrower toasts when the cards beside them grow or the window shrinks.
	_top_left.minimum_size_changed.connect(_fit_toasts)
	_top_center.minimum_size_changed.connect(_fit_toasts)
	get_viewport().size_changed.connect(_fit_toasts)


## Toast text width: 300, or what is left beside the time pill in a narrow layout.
func _toast_width() -> float:
	var free := get_viewport_rect().size.x - 28.0 - _top_left.get_combined_minimum_size().x - _top_center.get_combined_minimum_size().x
	return clampf(free - 32.0, 200.0, 300.0)


func _fit_toasts() -> void:
	var w := _toast_width()
	for card in _toasts.get_children():
		var text: Control = card.get_child(0).get_child(1)
		if not is_equal_approx(text.custom_minimum_size.x, w):
			text.custom_minimum_size.x = w


func _on_event(ev: Dictionary) -> void:
	if ev.get("type", "") == "unification":
		show_ending()
	var sev: int = ev.get("severity", 0)
	if sev < 3:
		return
	var card := PanelContainer.new()
	var accent := UITheme.BAD if sev >= 4 else UITheme.ACCENT
	var sb := UITheme.card_style(UITheme.BG, 10, 10)
	sb.border_width_left = 3
	sb.border_color = accent
	card.add_theme_stylebox_override("panel", sb)
	card.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var v := VBoxContainer.new()
	v.add_theme_constant_override("separation", 0)
	card.add_child(v)
	v.add_child(UITheme.label(ev.get("time", ""), 11, UITheme.TEXT_FAINT))
	var l := UITheme.label(ev.get("text", ""), 14)
	l.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	l.custom_minimum_size = Vector2(_toast_width(), 0)
	v.add_child(l)
	var data = ev.get("data", {})
	card.mouse_filter = Control.MOUSE_FILTER_STOP
	card.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
	var eid := int(ev.get("id", 0))
	if ev.get("type", "") == "decision_made" and typeof(data) == TYPE_DICTIONARY and data.has("decision"):
		# Decision toasts open the council at that decision.
		var did := int(data["decision"])
		v.add_child(UITheme.label("点击查看她的考量 →", 11, UITheme.MAGIC))
		card.gui_input.connect(func(e: InputEvent) -> void:
			if e is InputEventMouseButton and e.pressed and e.button_index == MOUSE_BUTTON_LEFT:
				toggle_council(did))
	else:
		card.tooltip_text = "点击追溯因果"
		card.gui_input.connect(func(e: InputEvent) -> void:
			if e is InputEventMouseButton and e.pressed and e.button_index == MOUSE_BUTTON_LEFT:
				toggle_chronicle(eid))
	_toasts.add_child(card)
	while _toasts.get_child_count() > 5:
		_toasts.get_child(0).queue_free()
		_toasts.remove_child(_toasts.get_child(0))
	var tw := create_tween()
	tw.tween_interval(6.0)
	tw.tween_property(card, "modulate:a", 0.0, 0.8)
	tw.tween_callback(card.queue_free)
