class_name CivCard
extends PanelContainer
## Top-left civilisation card. Compact by default (title + key numbers + crisis badges);
## click to expand into an overview of magical girls, policies and trends.

signal girl_selected(id: int)
signal council_requested
signal tech_requested
signal event_requested(id: int)

var sim: IcarusSim
var polity_id := 1
var expanded := false

var _title: Label
var _era: Label
var _era_pill: PanelContainer
var _research: Button
var _research_bar: UIBar
var _war: VBoxContainer
var _war_sig := ""
var _switcher: HBoxContainer
var _switch_sig := ""
var _chips: HBoxContainer
var _badges: HBoxContainer
var _detail: VBoxContainer
var _timer := 0.0
var _world_line: Label


func _ready() -> void:
	add_theme_stylebox_override("panel", UITheme.card_style(UITheme.BG, 14, 10))
	size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	mouse_filter = Control.MOUSE_FILTER_STOP
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 4)
	add_child(col)
	var head := HBoxContainer.new()
	col.add_child(head)
	var crown := UIIcon.new("crown", 18)
	crown.color = UITheme.ACCENT
	head.add_child(crown)
	_title = UITheme.label("—", 16, UITheme.TEXT, true)
	head.add_child(_title)
	_era_pill = PanelContainer.new()
	_era_pill.add_theme_stylebox_override("panel", UITheme.flat(UITheme.ACCENT_SOFT, 7, 7, 1))
	_era_pill.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	_era = UITheme.label("", 11, UITheme.ACCENT)
	_era_pill.add_child(_era)
	head.add_child(_era_pill)
	var toggle := Button.new()
	toggle.text = "详情"
	toggle.focus_mode = Control.FOCUS_NONE
	toggle.add_theme_font_size_override("font_size", 12)
	toggle.pressed.connect(func() -> void:
		expanded = not expanded
		toggle.text = "收起" if expanded else "详情"
		_refresh())
	head.add_child(toggle)
	# Polity switcher: appears once the island holds more than one civilisation.
	_switcher = HBoxContainer.new()
	_switcher.add_theme_constant_override("separation", 4)
	_switcher.visible = false
	col.add_child(_switcher)
	_chips = HBoxContainer.new()
	_chips.add_theme_constant_override("separation", 12)
	col.add_child(_chips)
	# Research: what the polity is working on; opens the tech tree.
	_research = Button.new()
	_research.focus_mode = Control.FOCUS_NONE
	_research.tooltip_text = "科技树（T）"
	_research.custom_minimum_size = Vector2(0, 24)
	_research.pressed.connect(func() -> void: tech_requested.emit())
	var rrow := HBoxContainer.new()
	rrow.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	rrow.offset_left = 2
	rrow.add_theme_constant_override("separation", 5)
	rrow.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var fic := UIIcon.new("flask", 14)
	fic.color = UITheme.ACCENT
	fic.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	rrow.add_child(fic)
	_research_bar = UIBar.new("", false)
	_research_bar.label_width = 92
	_research_bar.fixed_color = UITheme.ACCENT
	_research_bar.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_research_bar.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rrow.add_child(_research_bar)
	_research.add_child(rrow)
	col.add_child(_research)
	# Wars and the army's current operation.
	_war = VBoxContainer.new()
	_war.add_theme_constant_override("separation", 3)
	col.add_child(_war)
	_badges = HBoxContainer.new()
	_badges.add_theme_constant_override("separation", 6)
	col.add_child(_badges)
	_detail = VBoxContainer.new()
	_detail.add_theme_constant_override("separation", 4)
	col.add_child(_detail)
	_world_line = UITheme.label("", 11, UITheme.TEXT_FAINT)
	col.add_child(_world_line)


func _process(delta: float) -> void:
	_timer -= delta
	if _timer <= 0.0:
		_timer = 0.5
		_refresh()


func _chip(icon: String, text: String, color := UITheme.TEXT) -> void:
	var h := HBoxContainer.new()
	h.add_theme_constant_override("separation", 3)
	var ic := UIIcon.new(icon, 14)
	ic.color = color
	h.add_child(ic)
	h.add_child(UITheme.label(text, 13, color))
	_chips.add_child(h)


func _badge(text: String, color: Color) -> void:
	var p := PanelContainer.new()
	var sb := UITheme.flat(Color(color, 0.18), 8, 7, 1)
	sb.border_color = Color(color, 0.6)
	sb.set_border_width_all(1)
	p.add_theme_stylebox_override("panel", sb)
	p.add_child(UITheme.label(text, 12, color))
	_badges.add_child(p)


func _refresh_switcher() -> void:
	var ps: Array = sim.polities()
	var sig := ""
	for p in ps:
		sig += "%d:%s|" % [p["id"], p["name"]]
	sig += str(polity_id)
	if sig == _switch_sig:
		return
	_switch_sig = sig
	for c in _switcher.get_children():
		c.queue_free()
	_switcher.visible = ps.size() > 1
	if ps.size() <= 1:
		return
	for p in ps:
		var pid: int = p["id"]
		var b := Button.new()
		b.focus_mode = Control.FOCUS_NONE
		b.toggle_mode = true
		b.button_pressed = pid == polity_id
		b.text = "    " + String(p["name"])
		b.add_theme_font_size_override("font_size", 12)
		b.tooltip_text = String(p["title"])
		var dot := UIIcon.new("dot", 12)
		dot.color = p["color"]
		dot.position = Vector2(6, 7)
		b.add_child(dot)
		b.pressed.connect(func() -> void:
			polity_id = pid
			_switch_sig = ""
			_refresh())
		_switcher.add_child(b)


func _refresh() -> void:
	if sim == null or not sim.has_game():
		return
	_refresh_switcher()
	var d: Dictionary = sim.polity_info(polity_id)
	if d.is_empty():
		_title.text = "无主之地"
		return
	_title.text = d["title"]
	for c in _chips.get_children():
		c.queue_free()
	for c in _badges.get_children():
		c.queue_free()
	for c in _detail.get_children():
		c.queue_free()
	var st: Dictionary = d["stats"]
	var fd: float = st["food_days"]
	_chip("people", "%d" % st["population"])
	_chip("food", "存粮 %.1f天" % fd, UITheme.BAD if fd < 0.8 else (UITheme.WARN if fd < 1.5 else UITheme.TEXT))
	_chip("heart", "心情 %d%%" % int(float(st["mood"]) * 100), UITheme.BAD if float(st["mood"]) < 0.35 else UITheme.TEXT)
	var sup: float = st["ruler_support"]
	_chip("crown", "支持 %+.2f" % sup, UITheme.BAD if sup < -0.1 else (UITheme.GOOD if sup > 0.3 else UITheme.TEXT))
	if int(d.get("soldiers", 0)) > 0:
		_chip("swords", "士兵 %d" % d["soldiers"], UITheme.WARN)
	_era.text = String(d.get("era_name", ""))
	_era_pill.visible = _era.text != ""
	var res: Dictionary = d.get("research", {})
	if res.is_empty():
		_research_bar.label = "科技 %d 项" % int(d.get("techs_known", 0))
		_research_bar.show_value = false
		_research_bar.set_value(0.0)
		_research_bar.label_width = 200
	else:
		var frac := float(res["progress"]) / maxf(1.0, float(res["cost"]))
		_research_bar.label = "研究 · %s" % res["name"]
		_research_bar.label_width = 92
		_research_bar.show_value = true
		_research_bar.set_value(frac)
	_refresh_war(d)
	for cr in d["crises"]:
		_badge("%s %d%%" % [cr["kind"], int(float(cr["severity"]) * 100)], UITheme.BAD)
	if int(st.get("protesters", 0)) > 0:
		_badge("抗议 %d人" % st["protesters"], UITheme.WARN)
	_badges.visible = _badges.get_child_count() > 0
	_detail.visible = expanded
	if expanded:
		_detail.add_child(HSeparator.new())
		_detail.add_child(UITheme.label("魔法少女", 12, UITheme.TEXT_FAINT))
		for g in d["girls"]:
			var row := HBoxContainer.new()
			var b := Button.new()
			var roles := {"ruler": "统治者", "minister": "大臣", "none": ""}
			b.text = "◆ %s · %s %s" % [g["name"], g["drive"], roles.get(g["role"], g["role"])]
			b.focus_mode = Control.FOCUS_NONE
			b.alignment = HORIZONTAL_ALIGNMENT_LEFT
			b.custom_minimum_size = Vector2(170, 0)
			b.add_theme_font_size_override("font_size", 13)
			var gid: int = g["id"]
			b.pressed.connect(func() -> void: girl_selected.emit(gid))
			row.add_child(b)
			var bar := UIBar.new("民心", true)
			bar.label_width = 30
			bar.custom_minimum_size = Vector2(150, 20)
			bar.set_value(g["support"])
			row.add_child(bar)
			_detail.add_child(row)
		_detail.add_child(UITheme.label("政策", 12, UITheme.TEXT_FAINT))
		var pol: Dictionary = d["policies"]
		var dist := ["平均分配", "按劳分配", "精英优先"]
		_detail.add_child(UITheme.label("配给 %d%% · 惩罚 %d%% · 工时 %d小时 · %s" % [int(float(pol["ration"]) * 100), int(float(pol["punishment"]) * 100), int(pol["work_hours"]), dist[clampi(int(pol["distribution"]), 0, 2)]], 13))
		_detail.add_child(UITheme.label("趋势（近 %d 小时）" % (d["history"]["food_days"] as PackedFloat32Array).size(), 12, UITheme.TEXT_FAINT))
		var spark := Sparklines.new()
		spark.series = [
			["存粮(天)", d["history"]["food_days"], UITheme.ACCENT, 0.0, 4.0],
			["心情", d["history"]["mood"], UITheme.GOOD, 0.0, 1.0],
			["统治者支持", d["history"]["ruler_support"], UITheme.MAGIC, -1.0, 1.0],
		]
		spark.custom_minimum_size = Vector2(330, 84)
		_detail.add_child(spark)
	var ws: Dictionary = sim.world_stats()
	_world_line.text = "地格 活跃%d · 休眠%d · 未生成%d · 流水%d · 火%d" % [ws.get("active", 0), ws.get("dormant", 0), ws.get("ungenerated", 0), ws.get("water_active", 0), ws.get("fire_active", 0)]


func _refresh_war(d: Dictionary) -> void:
	var wars: Array = d.get("wars", [])
	var op: Dictionary = d.get("op", {})
	var sig := var_to_str(wars) + var_to_str(op)
	if sig == _war_sig:
		return
	_war_sig = sig
	for c in _war.get_children():
		c.queue_free()
	_war.visible = not wars.is_empty() or op.get("active", false)
	var aims := {"raid": "劫掠", "conquest": "征服", "defend": "防御"}
	for w in wars:
		var b := Button.new()
		b.focus_mode = Control.FOCUS_NONE
		b.alignment = HORIZONTAL_ALIGNMENT_LEFT
		b.add_theme_font_size_override("font_size", 12)
		var sb := UITheme.flat(Color(UITheme.BAD, 0.14), 8, 8, 3)
		sb.border_color = Color(UITheme.BAD, 0.5)
		sb.set_border_width_all(1)
		b.add_theme_stylebox_override("normal", sb)
		var hov := sb.duplicate() as StyleBoxFlat
		hov.bg_color = Color(UITheme.BAD, 0.24)
		b.add_theme_stylebox_override("hover", hov)
		b.add_theme_color_override("font_color", UITheme.BAD.lightened(0.25))
		var what := ("向「%s」发动%s" % [w["enemy_name"], aims.get(w["aim"], "战争")]) if w["attacker"] else ("抵御「%s」的进攻" % w["enemy_name"])
		b.text = "      %s · 歼%d 损%d" % [what, w["kills"], w["losses"]]
		b.tooltip_text = "自 %s 起。点击查看宣战的因果链" % w["since"]
		var ic := UIIcon.new("swords", 14)
		ic.color = UITheme.BAD.lightened(0.2)
		ic.position = Vector2(8, 5)
		b.add_child(ic)
		var dot := UIIcon.new("dot", 10)
		dot.color = w["enemy_color"]
		dot.position = Vector2(24, 7)
		b.add_child(dot)
		var ev: int = w["event"]
		b.pressed.connect(func() -> void: event_requested.emit(ev))
		_war.add_child(b)
	if op.get("active", false):
		var verb := {"raid": "劫掠", "conquest": "进攻", "defend": "迎击"}
		var line := "军队 · %s「%s」 · %s · %d人" % [verb.get(op["aim"], ""), op["enemy_name"], op["phase_name"], op["party"]]
		if int(op["lost"]) > 0:
			line += "（折损 %d）" % op["lost"]
		_war.add_child(UITheme.label(line, 12, UITheme.WARN))
