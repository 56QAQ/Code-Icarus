class_name SelectionCard
extends PanelContainer
## One contextual card for whatever is selected (character, building, cube). Tabs keep
## the information dense but organised; the card floats bottom-right and closes with ×.

signal closed
signal focus_requested(pos: Vector3)
signal event_requested(id: int)
signal decision_requested(id: int)

var sim: IcarusSim
var kind := ""        # "character" | "building" | "cube"
var target_id := -1
var cube := Vector3i.ZERO

var _title: Label
var _subtitle: Label
var _tabs: HBoxContainer
var _body: VBoxContainer
var _scroll: ScrollContainer
var _tab := "status"
var _timer := 0.0
var _whisper_dir := 1

## Values a god can whisper about: [feature key, short name].
const WHISPER_VALUES := [
	["food_security", "粮食"], ["welfare", "民生"], ["fairness", "公平"], ["cooperation", "合作"],
	["harshness", "严惩"], ["military", "武力"], ["self_power", "权力"], ["growth", "发展"],
]
var _tab_buttons := {}
## Dragged by its header: stays where the player put it until the next selection.
var moved_by_user := false
var _dragging := false


func _ready() -> void:
	add_theme_stylebox_override("panel", UITheme.card_style(UITheme.BG, 14, 12))
	custom_minimum_size = Vector2(380, 0)
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 6)
	add_child(col)
	var head := HBoxContainer.new()
	head.mouse_filter = Control.MOUSE_FILTER_STOP
	head.mouse_default_cursor_shape = Control.CURSOR_MOVE
	head.gui_input.connect(_on_head_input)
	col.add_child(head)
	var titles := VBoxContainer.new()
	titles.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	titles.add_theme_constant_override("separation", 0)
	head.add_child(titles)
	_title = UITheme.label("", 18, UITheme.TEXT, true)
	titles.add_child(_title)
	_subtitle = UITheme.label("", 12, UITheme.ACCENT)
	_subtitle.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_subtitle.custom_minimum_size = Vector2(290, 0)  # a width to wrap at (the card floats)
	titles.add_child(_subtitle)
	var focus := Button.new()
	focus.focus_mode = Control.FOCUS_NONE
	focus.tooltip_text = "镜头对准"
	focus.custom_minimum_size = Vector2(30, 30)
	var fi := UIIcon.new("inspect", 16)
	fi.set_anchors_preset(Control.PRESET_CENTER)
	fi.position = Vector2(-8, -8)
	focus.add_child(fi)
	focus.pressed.connect(_on_focus)
	head.add_child(focus)
	var close := Button.new()
	close.focus_mode = Control.FOCUS_NONE
	close.custom_minimum_size = Vector2(30, 30)
	var ci := UIIcon.new("close", 16)
	ci.set_anchors_preset(Control.PRESET_CENTER)
	ci.position = Vector2(-8, -8)
	close.add_child(ci)
	close.pressed.connect(func() -> void: closed.emit())
	head.add_child(close)
	_tabs = HBoxContainer.new()
	_tabs.add_theme_constant_override("separation", 2)
	col.add_child(_tabs)
	col.add_child(HSeparator.new())
	_scroll = ScrollContainer.new()
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_scroll.custom_minimum_size = Vector2(356, 250)
	col.add_child(_scroll)
	_body = VBoxContainer.new()
	_body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_body.add_theme_constant_override("separation", 4)
	_scroll.add_child(_body)
	visible = false


func _on_head_input(e: InputEvent) -> void:
	if e is InputEventMouseButton and e.button_index == MOUSE_BUTTON_LEFT:
		_dragging = e.pressed
		accept_event()
	elif e is InputEventMouseMotion and _dragging:
		var view := get_viewport_rect().size
		position = (position + e.relative).clamp(Vector2(0, 0), view - size)
		moved_by_user = true
		accept_event()


func show_character(id: int) -> void:
	kind = "character"
	target_id = id
	var info: Dictionary = sim.character_info(id)
	var tabs := [["status", "状态"], ["why", "缘由"], ["traits", "性格"], ["social", "关系"], ["memory", "记忆"]]
	if info.get("girl", false):
		tabs.append(["politics", "政见"])
		tabs.append(["magic", "魔法"])
	_set_tabs(tabs)
	visible = true
	_refresh()


func show_building(id: int, at_cube: Vector3i) -> void:
	kind = "building"
	target_id = id
	cube = at_cube
	_set_tabs([])
	visible = true
	_refresh()


func _set_tabs(tabs: Array) -> void:
	for c in _tabs.get_children():
		c.queue_free()
	_tab_buttons.clear()
	var valid := false
	for t in tabs:
		if t[0] == _tab:
			valid = true
	if not valid:
		_tab = tabs[0][0] if not tabs.is_empty() else ""
	for t in tabs:
		var b := Button.new()
		b.text = t[1]
		b.toggle_mode = true
		b.focus_mode = Control.FOCUS_NONE
		b.button_pressed = t[0] == _tab
		b.add_theme_font_size_override("font_size", 13)
		var key: String = t[0]
		b.pressed.connect(func() -> void:
			_tab = key
			for k in _tab_buttons:
				_tab_buttons[k].button_pressed = k == key
			_refresh())
		_tabs.add_child(b)
		_tab_buttons[key] = b
	_tabs.visible = not tabs.is_empty()


func _process(delta: float) -> void:
	if not visible:
		return
	_timer -= delta
	# Rebuilding the body under a pressed mouse would swallow the click; while the
	# pointer rests on the card, refresh more slowly so hover states stay put.
	var over := get_global_rect().has_point(get_global_mouse_position())
	if over and Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT):
		return
	if _timer <= 0.0:
		_timer = 1.2 if over else 0.35
		_refresh()


func _on_focus() -> void:
	if kind == "character":
		var info: Dictionary = sim.character_info(target_id)
		focus_requested.emit(info.get("pos", Vector3.ZERO))
	elif kind == "building":
		focus_requested.emit(Vector3(cube))


func _clear() -> void:
	for c in _body.get_children():
		c.queue_free()


func _section(text: String) -> void:
	var l := UITheme.label(text, 12, UITheme.TEXT_FAINT)
	_body.add_child(l)


func _line(text: String, size_ := 13, color := UITheme.TEXT) -> Label:
	var l := UITheme.label(text, size_, color)
	l.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	l.custom_minimum_size = Vector2(340, 0)
	_body.add_child(l)
	return l


func _bar(label: String, v: float, bipolar := false, text := "") -> UIBar:
	var b := UIBar.new(label, bipolar)
	b.custom_minimum_size = Vector2(340, 20)
	b.set_value(v, text)
	_body.add_child(b)
	return b


## The god's hand on a magical girl: a whisper shifting what she values for two days,
## or strength (a level and full mana). Both are recorded and traceable.
func _god_hand(d: Dictionary, girl: Dictionary) -> void:
	_section("神之手")
	var w: Dictionary = girl.get("whisper", {})
	if not w.is_empty():
		_line("低语仍在回响：她%s「%s」（还剩 %d 小时）" % ["更在意" if int(w["dir"]) > 0 else "不再在意", w["name"], int(w["hours_left"])], 12, UITheme.MAGIC)
	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", 4)
	_body.add_child(head)
	head.add_child(UITheme.label("低语：让她", 12, UITheme.TEXT_DIM))
	for opt in [[1, "更在意"], [-1, "不再在意"]]:
		var b := Button.new()
		b.text = opt[1]
		b.toggle_mode = true
		b.focus_mode = Control.FOCUS_NONE
		b.action_mode = BaseButton.ACTION_MODE_BUTTON_PRESS
		b.add_theme_font_size_override("font_size", 12)
		b.button_pressed = _whisper_dir == opt[0]
		var dir: int = opt[0]
		b.pressed.connect(func() -> void:
			_whisper_dir = dir
			_refresh())
		head.add_child(b)
	var flow := HFlowContainer.new()
	flow.add_theme_constant_override("h_separation", 4)
	flow.add_theme_constant_override("v_separation", 4)
	flow.custom_minimum_size = Vector2(340, 0)
	_body.add_child(flow)
	var gid := int(d["id"])
	for v in WHISPER_VALUES:
		var b := Button.new()
		b.text = v[1]
		b.focus_mode = Control.FOCUS_NONE
		b.action_mode = BaseButton.ACTION_MODE_BUTTON_PRESS
		b.add_theme_font_size_override("font_size", 12)
		b.add_theme_stylebox_override("normal", UITheme.flat(Color(UITheme.MAGIC, 0.12), 7, 9, 3))
		b.add_theme_stylebox_override("hover", UITheme.flat(Color(UITheme.MAGIC, 0.26), 7, 9, 3))
		b.add_theme_color_override("font_color", UITheme.MAGIC.lightened(0.2))
		b.tooltip_text = "在她耳边低语，两天内她做决定时会%s%s" % ["更看重" if _whisper_dir > 0 else "更轻视", v[1]]
		var key: String = v[0]
		var dir := _whisper_dir
		b.pressed.connect(func() -> void:
			Game.admin("whisper_value", {"girl": gid, "feature": key, "dir": dir})
			_timer = 0.1)
		flow.add_child(b)
	var emp := Button.new()
	emp.text = "赐予力量（升一级，魔力回满）"
	emp.focus_mode = Control.FOCUS_NONE
	emp.action_mode = BaseButton.ACTION_MODE_BUTTON_PRESS
	emp.alignment = HORIZONTAL_ALIGNMENT_LEFT
	emp.add_theme_font_size_override("font_size", 12)
	emp.add_theme_color_override("font_color", UITheme.ACCENT)
	emp.pressed.connect(func() -> void:
		Game.admin("empower", {"girl": gid})
		_timer = 0.1)
	_body.add_child(emp)


func _refresh() -> void:
	if sim == null:
		return
	_clear()
	match kind:
		"character":
			_refresh_character()
		"building":
			_refresh_building()


func _refresh_character() -> void:
	var d: Dictionary = sim.character_info(target_id)
	if d.is_empty():
		visible = false
		return
	var girl: Dictionary = d.get("girl_data", {})
	_title.text = ("◆ " if not girl.is_empty() else "") + String(d["name"]) + ("" if d["alive"] else "（已故）")
	if not girl.is_empty():
		var roles := {"ruler": "统治者", "minister": "大臣", "governor": "总督", "general": "将军", "none": "无职务"}
		_subtitle.text = "%s · %s · Lv%d" % [girl["title"], roles.get(girl["role"], girl["role"]), girl["level"]]
	else:
		var occ := {"food": "农夫", "build": "工匠", "gather": "劳工"}
		var job: String = "士兵" if d.get("drafted", false) else occ.get(d.get("occupation", ""), "居民")
		_subtitle.text = "人造人 · %s · 住在%s" % [job, d.get("home", "野外")]
	match _tab:
		"status":
			if not d["alive"]:
				_line("死因：%s" % d.get("death_cause", ""), 14, UITheme.BAD)
				return
			_line("正在%s：%s" % [d["task"], d["status"]], 14)
			_section("需求")
			var needs: Dictionary = d["needs"]
			_bar("饱腹", needs["food"])
			_bar("饮水", needs["water"])
			_bar("精力", needs["rest"])
			_bar("社交", needs["social"])
			_bar("安全", needs["safety"])
			_section("状态")
			_bar("心情", d["mood"])
			_bar("生命力", d["vitality"])
			var fear := _bar("畏惧", d["fear"])
			fear.fixed_color = UITheme.MAGIC
			_section("身体")
			var parts := ""
			for p in d["body"]:
				if p["severed"]:
					parts += "%s 断失  " % p["name"]
				elif p["integrity"] < 0.99:
					parts += "%s %d%%  " % [p["name"], int(p["integrity"] * 100)]
			_line(parts if parts != "" else "完好无损", 13, UITheme.TEXT_DIM if parts == "" else UITheme.WARN)
			if d.has("treated_hours"):
				_line("伤口已包扎，正在加速愈合（还剩 %d 小时）" % int(d["treated_hours"]), 12, UITheme.GOOD)
			var eq: Array = d.get("equipment", [])
			if not eq.is_empty():
				var es := PackedStringArray()
				for e in eq:
					es.append("%s %s" % [e["slot"], e["name"]])
				_section("装备")
				_line("  ·  ".join(es), 13, UITheme.TEXT_DIM)
			var inv: Array = d["inventory"]
			if not inv.is_empty():
				var s := ""
				for it in inv:
					s += "%s×%d  " % [it["item"], it["count"]]
				_section("携带")
				_line(s, 13, UITheme.TEXT_DIM)
		"why":
			_section("此刻的考量（分数越高越倾向）")
			var trace: Array = d["trace"]
			var top := 0.001
			for o in trace:
				top = maxf(top, float(o["score"]))
			for o in trace:
				var b := _bar(o["label"], clampf(float(o["score"]) / top, 0.0, 1.0), false, "%.2f" % float(o["score"]))
				b.fixed_color = UITheme.ACCENT if o == trace[0] else UITheme.TEXT_FAINT
				_line("　" + String(o["why"]), 12, UITheme.TEXT_DIM)
		"traits":
			_section("性格")
			for t in d["personality"]:
				var b := _bar(t["name"], t["value"])
				b.fixed_color = UITheme.MAGIC
			_section("技能")
			for s in d["skills"]:
				var b := _bar(s["name"], s["value"])
				b.fixed_color = UITheme.ACCENT
		"social":
			_section("对魔法少女的支持")
			for s in d["support"]:
				_bar("%s" % s["name"], s["value"], true)
			_section("亲疏")
			for r in d["relations"]:
				_bar(r["name"], r["value"], true)
		"memory":
			_section("近来的经历")
			var mems: Array = d["memories"]
			if mems.is_empty():
				_line("平静的日子", 13, UITheme.TEXT_DIM)
			for m in mems:
				var who := ("（%s）" % m["subject"]) if m.has("subject") else ""
				var c := UITheme.GOOD if float(m["valence"]) > 0.0 else UITheme.BAD
				var text := "%s  %s%s" % [String(m["time"]).substr(String(m["time"]).find("·") + 1), m["text"], who]
				var ev_id := int(m.get("event", 0))
				if ev_id > 0:
					# Memories tied to a recorded event can be traced in the chronicle.
					var b := Button.new()
					b.text = text + "  ›"
					b.focus_mode = Control.FOCUS_NONE
					b.alignment = HORIZONTAL_ALIGNMENT_LEFT
					b.clip_text = true
					b.add_theme_font_size_override("font_size", 12)
					b.add_theme_color_override("font_color", c)
					b.tooltip_text = "追溯这段经历的起因"
					b.pressed.connect(func() -> void: event_requested.emit(ev_id))
					_body.add_child(b)
				else:
					_line(text, 12, c)
		"politics":
			if girl.is_empty():
				return
			var stances := {"loyal": ["忠诚", UITheme.GOOD], "critical": ["有所不满", UITheme.WARN], "defiant": ["离心", UITheme.WARN], "rebel": ["反叛", UITheme.BAD]}
			var st: Array = stances.get(girl.get("stance", ""), [girl.get("stance", ""), UITheme.TEXT])
			if girl.get("role", "") == "ruler":
				_line("立场：一国之主", 14, UITheme.ACCENT)
			else:
				_line("立场：%s" % st[0], 14, st[1])
				_bar("忠诚", girl["loyalty"], true)
			_bar("民望", girl.get("popular_support", 0.0), true)
			_bar("经验", clampf(float(girl["xp"]) / maxf(1.0, float(girl.get("xp_next", 40.0))), 0.0, 1.0), false, "%d/%d" % [int(girl["xp"]), int(girl.get("xp_next", 40.0))])
			if girl.has("grudge"):
				_line("心怀芥蒂：%s" % girl["grudge"], 13, UITheme.BAD)
			_section("她的决策")
			var ids: PackedInt32Array = girl.get("decisions", PackedInt32Array())
			if ids.is_empty():
				_line("尚未做出重大决定", 13, UITheme.TEXT_DIM)
			for i in range(ids.size() - 1, maxi(-1, ids.size() - 7), -1):
				var dd: Dictionary = sim.decision(ids[i])
				if dd.is_empty():
					continue
				var b := Button.new()
				b.focus_mode = Control.FOCUS_NONE
				b.alignment = HORIZONTAL_ALIGNMENT_LEFT
				b.clip_text = true
				b.custom_minimum_size = Vector2(0, 26)
				b.add_theme_font_size_override("font_size", 12)
				b.text = "%s  %s → %s" % [String(dd["time"]).get_slice("·", 1).strip_edges(), dd["topic"], dd["chosen"]]
				b.tooltip_text = String(dd.get("rationale", ""))
				var did := int(ids[i])
				b.pressed.connect(func() -> void: decision_requested.emit(did))
				_body.add_child(b)
			if d["alive"]:
				_god_hand(d, girl)
		"magic":
			if girl.is_empty():
				return
			_line("源动力：%s（%s）" % [girl["drive"], "正面" if int(girl.get("valence", 1)) > 0 else "负面"], 14, UITheme.MAGIC)
			_line("性情：%s" % girl.get("temperament", ""), 13, UITheme.TEXT_DIM)
			_bar("魔力", girl["mana"])
			_bar("忠诚", girl["loyalty"], true)
			_section("魔法（Lv%d）" % girl["level"])
			for sp in girl.get("spells", []):
				var tag := "主动" if sp["type"] == "active" else "被动"
				var col := UITheme.TEXT if sp["unlocked"] else UITheme.TEXT_FAINT
				_line("%s【%s·Lv%d】%s" % ["◆" if sp["unlocked"] else "◇", tag, sp["level"], sp["name"]], 13, col)
				_line("　" + String(sp["desc"]), 12, UITheme.TEXT_FAINT)


func _refresh_building() -> void:
	var d: Dictionary = sim.building_at(cube)
	if d.is_empty():
		visible = false
		return
	_title.text = d["name"]
	var state := "完好" if d["functional"] else "损毁"
	if not d["complete"]:
		state = "施工中"
	_subtitle.text = "建筑 · %s" % state
	_bar("完好度", d["integrity"])
	if int(d.get("beds", 0)) > 0:
		_line("床位 %d · 住户 %d" % [d["beds"], d["residents"]], 13, UITheme.TEXT_DIM)
	var items: Array = d.get("items", [])
	if not items.is_empty():
		_section("库存")
		var s := ""
		for it in items:
			s += "%s×%d  " % [it["item"], it["count"]]
		_line(s, 13)
