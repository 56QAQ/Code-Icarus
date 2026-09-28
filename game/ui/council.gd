class_name CouncilPanel
extends PanelContainer
## 议事录: a centred overlay card listing the magical girls' decisions. The left column
## is the ledger of decisions; the right column shows one decision in full — what she
## knew, the options the program offered (with computed facts), what the other girls
## proposed, what she chose and why, and how it turned out. The header holds the Jev
## provider settings (local persona model or Claude).

signal closed
signal event_requested(id: int)

var sim: IcarusSim
var selected_id := 0

var _list: VBoxContainer
var _detail: VBoxContainer
var _provider_btn: Button
var _provider_dot: UIIcon
var _settings: VBoxContainer
var _status: Label
var _timer := 0.0
var _last_count := -1
var _last_selected_status := ""
var _situation_open := false

const KIND_NAMES := {"crisis": "危机应对", "governance": "日常施政", "stance": "立场", "petition": "进谏", "research": "研究方向",
	"diplomacy": "外交", "war": "战事", "peace_offer": "议和", "trade_offer": "通商"}
const SOURCE_NAMES := {"local": "本地人格", "fallback": "本地接管", "replay": "回放"}


func _ready() -> void:
	add_theme_stylebox_override("panel", UITheme.card_style(UITheme.BG_SOLID, 16, 14))
	mouse_filter = Control.MOUSE_FILTER_STOP
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 10)
	add_child(col)
	_build_header(col)
	_build_settings(col)
	var body := HBoxContainer.new()
	body.add_theme_constant_override("separation", 12)
	body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(body)

	var left := ScrollContainer.new()
	left.custom_minimum_size = Vector2(300, 0)
	left.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	body.add_child(left)
	_list = VBoxContainer.new()
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_list.add_theme_constant_override("separation", 4)
	left.add_child(_list)

	body.add_child(VSeparator.new())
	var right := ScrollContainer.new()
	right.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	right.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	body.add_child(right)
	_detail = VBoxContainer.new()
	_detail.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_detail.add_theme_constant_override("separation", 8)
	right.add_child(_detail)
	Jev.status_changed.connect(_refresh_provider)
	_refresh_provider()


func open_at(id: int = 0) -> void:
	visible = true
	if id > 0:
		selected_id = id
	_last_count = -1
	_refresh()


# ------------------------------------------------------------------ header & settings

func _build_header(col: VBoxContainer) -> void:
	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", 8)
	col.add_child(head)
	var ic := UIIcon.new("scroll", 22)
	ic.color = UITheme.ACCENT
	head.add_child(ic)
	head.add_child(UITheme.label("议事录", 18, UITheme.TEXT, true))
	var sub := UITheme.label("魔法少女的决策 · 程序给出可行选项，由她依性格与源动力抉择", 12, UITheme.TEXT_FAINT)
	sub.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	sub.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	head.add_child(sub)
	_provider_btn = Button.new()
	_provider_btn.focus_mode = Control.FOCUS_NONE
	_provider_btn.toggle_mode = true
	_provider_btn.add_theme_font_size_override("font_size", 13)
	_provider_btn.tooltip_text = "决策提供者设置"
	_provider_btn.add_theme_stylebox_override("normal", UITheme.flat(Color(1, 1, 1, 0.06), 14, 12, 5))
	_provider_btn.toggled.connect(func(on: bool) -> void: _settings.visible = on)
	head.add_child(_provider_btn)
	_provider_dot = UIIcon.new("dot", 12)
	_provider_dot.position = Vector2(8, 11)
	_provider_btn.add_child(_provider_dot)
	var close := Button.new()
	close.focus_mode = Control.FOCUS_NONE
	close.custom_minimum_size = Vector2(32, 30)
	close.tooltip_text = "关闭（J）"
	var cic := UIIcon.new("close", 16)
	cic.position = Vector2(8, 7)
	close.add_child(cic)
	close.pressed.connect(func() -> void:
		visible = false
		closed.emit())
	head.add_child(close)


func _build_settings(col: VBoxContainer) -> void:
	_settings = VBoxContainer.new()
	_settings.visible = false
	_settings.add_theme_constant_override("separation", 8)
	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel", UITheme.flat(Color(1, 1, 1, 0.04), 10, 12, 10))
	panel.add_child(_settings)
	col.add_child(panel)
	# Hide the whole panel together with the settings box.
	_settings.visibility_changed.connect(func() -> void: panel.visible = _settings.visible)
	panel.visible = false

	var r1 := HBoxContainer.new()
	r1.add_theme_constant_override("separation", 12)
	_settings.add_child(r1)
	var enable := CheckButton.new()
	enable.text = "由 Claude 远程决策"
	enable.focus_mode = Control.FOCUS_NONE
	enable.button_pressed = Jev.enabled
	r1.add_child(enable)
	_status = UITheme.label("", 12, UITheme.TEXT_DIM)
	_status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	r1.add_child(_status)

	var r2 := HBoxContainer.new()
	r2.add_theme_constant_override("separation", 8)
	_settings.add_child(r2)
	r2.add_child(UITheme.label("API Key", 13, UITheme.TEXT_DIM))
	var key := LineEdit.new()
	key.secret = true
	key.text = Jev.api_key
	key.placeholder_text = "sk-ant-…（留空则使用环境变量 ANTHROPIC_API_KEY）"
	key.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	r2.add_child(key)

	var r3 := HBoxContainer.new()
	r3.add_theme_constant_override("separation", 8)
	_settings.add_child(r3)
	r3.add_child(UITheme.label("模型", 13, UITheme.TEXT_DIM))
	var mdl := LineEdit.new()
	mdl.text = Jev.model
	mdl.custom_minimum_size = Vector2(170, 0)
	r3.add_child(mdl)
	r3.add_child(UITheme.label("思考强度", 13, UITheme.TEXT_DIM))
	var eff := OptionButton.new()
	eff.focus_mode = Control.FOCUS_NONE
	var efforts := ["low", "medium", "high"]
	for e in ["低", "中", "高"]:
		eff.add_item(e)
	eff.selected = maxi(0, efforts.find(Jev.effort))
	r3.add_child(eff)
	r3.add_child(UITheme.label("每日调用上限", 13, UITheme.TEXT_DIM))
	var budget := SpinBox.new()
	budget.min_value = 1
	budget.max_value = 500
	budget.value = Jev.budget_per_day
	r3.add_child(budget)
	var hold := CheckButton.new()
	hold.text = "思考时暂停时间"
	hold.focus_mode = Control.FOCUS_NONE
	hold.button_pressed = Jev.hold_for_answers
	r3.add_child(hold)

	var note := UITheme.label("已启用服务器端 fallback：若请求被安全策略拒绝，将自动由推荐的备用模型作答。超时、网络错误、无效或过期的答复都会由本地人格模型接管，游戏不会因此停滞。", 12, UITheme.TEXT_FAINT)
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.custom_minimum_size = Vector2(600, 0)
	_settings.add_child(note)

	var r4 := HBoxContainer.new()
	r4.add_theme_constant_override("separation", 8)
	_settings.add_child(r4)
	var apply := Button.new()
	apply.text = "应用"
	apply.focus_mode = Control.FOCUS_NONE
	apply.add_theme_stylebox_override("normal", UITheme.flat(UITheme.ACCENT_SOFT, 8))
	r4.add_child(apply)
	var export := Button.new()
	export.text = "导出决策日志"
	export.focus_mode = Control.FOCUS_NONE
	r4.add_child(export)
	var export_note := UITheme.label("", 12, UITheme.TEXT_FAINT)
	r4.add_child(export_note)
	apply.pressed.connect(func() -> void:
		Jev.configure({
			"enabled": enable.button_pressed,
			"api_key": key.text.strip_edges(),
			"model": mdl.text.strip_edges(),
			"effort": efforts[eff.selected],
			"budget_per_day": int(budget.value),
			"hold_for_answers": hold.button_pressed,
		}))
	export.pressed.connect(func() -> void:
		var path := "user://decision_log_%d.json" % Game.sim.get_tick()
		if Jev.export_log(path):
			export_note.text = "已保存到 " + ProjectSettings.globalize_path(path)
		else:
			export_note.text = "保存失败")


func _refresh_provider() -> void:
	if _provider_btn == null:
		return
	var remote := Jev.enabled and Jev.has_key()
	var label := ("Claude · " + Jev.model) if remote else "本地人格模型"
	if Jev.enabled and not Jev.has_key():
		label = "本地人格模型（缺少 API Key）"
	_provider_btn.text = "    " + label
	_provider_dot.set_color(UITheme.MAGIC if remote else UITheme.TEXT_DIM)
	if _status != null:
		var t := "远程调用 %d 次" % Jev.calls_total
		if Jev.calls_failed > 0:
			t += " · 失败 %d" % Jev.calls_failed
		if Jev.in_flight_count() > 0:
			t += " · 思考中 %d" % Jev.in_flight_count()
		if not Jev.last_error.is_empty():
			t += " · 最近错误：" + Jev.last_error
		_status.text = t


# ------------------------------------------------------------------ refresh

func _process(delta: float) -> void:
	if not visible or sim == null or not sim.has_game():
		return
	_timer -= delta
	if _timer <= 0.0:
		_timer = 0.8
		_refresh()


func _refresh() -> void:
	var items: Array = sim.decisions(0, 80)
	var sig := items.size()
	for d in items:
		sig += 7 * int(d["id"]) * (1 + ["pending", "awaiting", "decided", "executed", "cancelled"].find(d["status"]))
		if str(d["outcome"]) != "":
			sig += 3
	if sig != _last_count:
		_last_count = sig
		_rebuild_list(items)
	if selected_id == 0 and not items.is_empty():
		selected_id = int(items.back()["id"])
	var d: Dictionary = sim.decision(selected_id)
	var st := "%s|%s|%s" % [d.get("status", ""), d.get("outcome", ""), selected_id]
	if st != _last_selected_status:
		_last_selected_status = st
		_rebuild_detail(d)
	_refresh_provider()


func _source_badge(src: String) -> PanelContainer:
	var name: String = SOURCE_NAMES.get(src, "")
	var color := UITheme.TEXT_DIM
	if src.begins_with("remote"):
		name = "Claude"
		color = UITheme.MAGIC
	elif src == "fallback":
		color = UITheme.WARN
	elif src == "replay":
		color = UITheme.ACCENT
	if name == "":
		name = src
	var p := PanelContainer.new()
	var sb := UITheme.flat(Color(color, 0.14), 6, 6, 0)
	p.add_theme_stylebox_override("panel", sb)
	p.add_child(UITheme.label(name, 11, color))
	p.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	return p


func _rebuild_list(items: Array) -> void:
	for c in _list.get_children():
		c.queue_free()
	if items.is_empty():
		_list.add_child(UITheme.label("还没有任何决策。", 13, UITheme.TEXT_FAINT))
		return
	for i in range(items.size() - 1, -1, -1):
		var d: Dictionary = items[i]
		var id: int = d["id"]
		var b := Button.new()
		b.focus_mode = Control.FOCUS_NONE
		b.toggle_mode = true
		b.button_pressed = id == selected_id
		b.custom_minimum_size = Vector2(0, 64)
		b.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		var v := VBoxContainer.new()
		v.mouse_filter = Control.MOUSE_FILTER_IGNORE
		v.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
		v.offset_left = 10
		v.offset_right = -8
		v.offset_top = 5
		v.add_theme_constant_override("separation", 0)
		b.add_child(v)
		var top := HBoxContainer.new()
		top.mouse_filter = Control.MOUSE_FILTER_IGNORE
		var t := UITheme.label(str(d["time"]), 11, UITheme.TEXT_FAINT)
		t.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		t.mouse_filter = Control.MOUSE_FILTER_IGNORE
		top.add_child(t)
		var status: String = d["status"]
		if status == "awaiting" or status == "pending":
			var th := UITheme.label("思考中…", 11, UITheme.MAGIC)
			th.mouse_filter = Control.MOUSE_FILTER_IGNORE
			top.add_child(th)
		elif str(d["source"]) != "":
			var badge := _source_badge(str(d["source"]))
			badge.mouse_filter = Control.MOUSE_FILTER_IGNORE
			top.add_child(badge)
		v.add_child(top)
		var who := UITheme.label("%s · %s" % [d.get("girl_name", "?"), d["topic"]], 13)
		who.clip_text = true
		who.mouse_filter = Control.MOUSE_FILTER_IGNORE
		v.add_child(who)
		var ch := UITheme.label("→ " + str(d["chosen"]) if str(d["chosen"]) != "" else "", 12, UITheme.ACCENT)
		ch.clip_text = true
		ch.mouse_filter = Control.MOUSE_FILTER_IGNORE
		v.add_child(ch)
		b.pressed.connect(func() -> void:
			selected_id = id
			for other in _list.get_children():
				if other is Button:
					(other as Button).button_pressed = other == b
			_last_selected_status = ""
			_refresh())
		_list.add_child(b)


func _section(text: String) -> void:
	_detail.add_child(UITheme.label(text, 12, UITheme.TEXT_FAINT))


func _rebuild_detail(d: Dictionary) -> void:
	for c in _detail.get_children():
		c.queue_free()
	if d.is_empty():
		_detail.add_child(UITheme.label("选择左侧的一项决策查看详情。", 14, UITheme.TEXT_FAINT))
		return
	var title := UITheme.label(str(d["topic"]), 19, UITheme.TEXT, true)
	title.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_detail.add_child(title)
	var meta := HBoxContainer.new()
	meta.add_theme_constant_override("separation", 8)
	meta.add_child(UITheme.label("◆ " + str(d.get("girl_name", "?")), 13, UITheme.ACCENT))
	meta.add_child(UITheme.label(KIND_NAMES.get(d["kind"], d["kind"]), 13, UITheme.TEXT_DIM))
	meta.add_child(UITheme.label(str(d["time"]), 13, UITheme.TEXT_FAINT))
	if str(d["source"]) != "":
		meta.add_child(_source_badge(str(d["source"])))
	var ev_id := int(d.get("event", 0))
	if ev_id > 0:
		var why := Button.new()
		why.text = "追溯因果"
		why.focus_mode = Control.FOCUS_NONE
		why.add_theme_font_size_override("font_size", 12)
		why.add_theme_stylebox_override("normal", UITheme.flat(Color(1, 1, 1, 0.06), 8, 10, 3))
		why.pressed.connect(func() -> void: event_requested.emit(ev_id))
		meta.add_child(why)
	_detail.add_child(meta)

	# What she knew (collapsed to a few lines by default).
	var sit := RichTextLabel.new()
	sit.fit_content = true
	sit.scroll_active = false
	sit.selection_enabled = true
	sit.add_theme_font_size_override("normal_font_size", 13)
	sit.add_theme_color_override("default_color", UITheme.TEXT_DIM)
	var sit_text: String = str(d.get("situation", ""))
	var lines := sit_text.split("\n")
	if not _situation_open and lines.size() > 5:
		sit.text = "\n".join(lines.slice(0, 5)) + "\n…"
	else:
		sit.text = sit_text
	var sit_head := HBoxContainer.new()
	var sh := UITheme.label("她所知道的局势", 12, UITheme.TEXT_FAINT)
	sh.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	sit_head.add_child(sh)
	if lines.size() > 5:
		var more := Button.new()
		more.text = "收起" if _situation_open else "展开全部"
		more.focus_mode = Control.FOCUS_NONE
		more.add_theme_font_size_override("font_size", 12)
		more.pressed.connect(func() -> void:
			_situation_open = not _situation_open
			_rebuild_detail(sim.decision(selected_id)))
		sit_head.add_child(more)
	_detail.add_child(sit_head)
	var sit_panel := PanelContainer.new()
	sit_panel.add_theme_stylebox_override("panel", UITheme.flat(Color(1, 1, 1, 0.03), 8, 10, 8))
	sit_panel.add_child(sit)
	_detail.add_child(sit_panel)

	# Options with computed facts and value profile.
	_section("可行选项（由程序计算后提供）")
	var opts: Array = d.get("option_list", [])
	var lo := 1e9
	var hi := -1e9
	for o in opts:
		if o.get("score") != null:
			lo = minf(lo, float(o["score"]))
			hi = maxf(hi, float(o["score"]))
	for o in opts:
		_detail.add_child(_option_row(o, lo, hi))

	var props: Array = d.get("proposals", [])
	if not props.is_empty():
		_section("内政官与其他魔法少女的建议")
		var flow := HFlowContainer.new()
		flow.add_theme_constant_override("h_separation", 6)
		flow.add_theme_constant_override("v_separation", 6)
		for p in props:
			var adopted: bool = p.get("adopted", false)
			var chip := PanelContainer.new()
			var c := UITheme.GOOD if adopted else UITheme.TEXT_DIM
			var sb := UITheme.flat(Color(c, 0.12), 8, 9, 3)
			sb.border_color = Color(c, 0.45)
			sb.set_border_width_all(1)
			chip.add_theme_stylebox_override("panel", sb)
			var txt := "%s：%s%s" % [p.get("name", "?"), p.get("option", ""), "（采纳）" if adopted else ""]
			var l := UITheme.label(txt, 12, UITheme.TEXT)
			l.tooltip_text = str(p.get("reason", ""))
			l.mouse_filter = Control.MOUSE_FILTER_PASS
			chip.add_child(l)
			flow.add_child(chip)
		_detail.add_child(flow)

	var status: String = d["status"]
	if status == "awaiting" or status == "pending":
		var th := HBoxContainer.new()
		var ic := UIIcon.new("think", 20)
		ic.color = UITheme.MAGIC
		th.add_child(ic)
		th.add_child(UITheme.label("她正在考虑…" + ("（Claude 思考中）" if Jev.is_thinking(selected_id) else ""), 14, UITheme.MAGIC))
		_detail.add_child(th)
	if str(d.get("rationale", "")) != "":
		_section("她的理由")
		var q := PanelContainer.new()
		var sb := UITheme.flat(Color(UITheme.ACCENT, 0.06), 6, 12, 8)
		sb.border_width_left = 3
		sb.border_color = UITheme.ACCENT
		q.add_theme_stylebox_override("panel", sb)
		var r := UITheme.label(str(d["rationale"]), 14)
		r.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		q.add_child(r)
		_detail.add_child(q)
	if str(d.get("note", "")) != "":
		var n := UITheme.label("※ " + str(d["note"]), 12, UITheme.WARN)
		n.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		_detail.add_child(n)
	if str(d.get("outcome", "")) != "":
		_section("事后回顾")
		var oc: String = d["outcome"]
		var col := UITheme.GOOD if oc.contains("好转") else (UITheme.BAD if oc.contains("恶化") else UITheme.TEXT_DIM)
		_detail.add_child(UITheme.label(oc, 14, col))


func _option_row(o: Dictionary, lo: float, hi: float) -> Control:
	var chosen: bool = o.get("chosen", false)
	var feasible: bool = o.get("feasible", true)
	var p := PanelContainer.new()
	var sb := UITheme.flat(Color(UITheme.ACCENT, 0.10) if chosen else Color(1, 1, 1, 0.03), 8, 10, 7)
	if chosen:
		sb.border_color = Color(UITheme.ACCENT, 0.7)
		sb.set_border_width_all(1)
	p.add_theme_stylebox_override("panel", sb)
	if not feasible:
		p.modulate = Color(1, 1, 1, 0.5)
	var v := VBoxContainer.new()
	v.add_theme_constant_override("separation", 3)
	p.add_child(v)
	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", 8)
	v.add_child(head)
	var t := UITheme.label(str(o["title"]), 14, UITheme.ACCENT if chosen else UITheme.TEXT, chosen)
	t.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	t.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	head.add_child(t)
	if o.get("score") != null and hi > lo:
		var bar := UIBar.new("倾向", false)
		bar.label_width = 30
		bar.show_value = false
		bar.custom_minimum_size = Vector2(110, 16)
		bar.fixed_color = UITheme.ACCENT if chosen else UITheme.TEXT_FAINT
		bar.set_value(clampf((float(o["score"]) - lo) / (hi - lo), 0.04, 1.0))
		bar.tooltip_text = "本地人格模型的倾向分 %.2f" % float(o["score"])
		head.add_child(bar)
	if not feasible:
		head.add_child(UITheme.label("不可行：" + str(o.get("why_not", "")), 12, UITheme.BAD))
	var desc := UITheme.label(str(o.get("desc", "")), 12, UITheme.TEXT_DIM)
	desc.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	v.add_child(desc)
	var feats: Dictionary = o.get("features", {})
	if not feats.is_empty():
		var flow := HFlowContainer.new()
		flow.add_theme_constant_override("h_separation", 4)
		flow.add_theme_constant_override("v_separation", 3)
		var keys := feats.keys()
		keys.sort_custom(func(a, b) -> bool: return absf(float(feats[a])) > absf(float(feats[b])))
		for k in keys:
			var val: float = feats[k]
			var c := UITheme.GOOD if val > 0 else UITheme.BAD
			var chip := PanelContainer.new()
			chip.add_theme_stylebox_override("panel", UITheme.flat(Color(c, 0.10), 5, 5, 0))
			chip.add_child(UITheme.label("%s%s" % ["+" if val > 0 else "−", k], 11, Color(c, 0.9)))
			flow.add_child(chip)
		v.add_child(flow)
	return p
