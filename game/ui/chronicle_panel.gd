class_name ChroniclePanel
extends PanelContainer
## 编年史: the island's history, filterable by kind, with the causal chain of any event.
## Everything that happens is recorded with its causes, so any event can be traced back
## to where it started — a god's meteor, a buried spring, a ruler's decree.

signal closed
signal focus_requested(pos: Vector3)
signal decision_requested(id: int)

var sim: IcarusSim
var selected_id := 0
var category := ""
var major_only := true

var _list: VBoxContainer
var _graph: CausalGraph
var _graph_scroll: ScrollContainer
var _head_title: Label
var _head_meta: Label
var _summary: Label
var _actions: HBoxContainer
var _filter_buttons := {}
var _timer := 0.0
var _last_top := -1

const FILTERS := [["", "全部"], ["politics", "政治"], ["disaster", "灾变"], ["economy", "生计"], ["life", "生死"], ["admin", "神迹"]]


func _ready() -> void:
	add_theme_stylebox_override("panel", UITheme.card_style(UITheme.BG_SOLID, 16, 14))
	mouse_filter = Control.MOUSE_FILTER_STOP
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 10)
	add_child(col)

	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", 8)
	col.add_child(head)
	var ic := UIIcon.new("chronicle", 22)
	ic.color = UITheme.ACCENT
	head.add_child(ic)
	head.add_child(UITheme.label("编年史", 18, UITheme.TEXT, true))
	var spacer := Control.new()
	spacer.custom_minimum_size = Vector2(12, 0)
	head.add_child(spacer)
	for f in FILTERS:
		var b := Button.new()
		b.text = f[1]
		b.toggle_mode = true
		b.focus_mode = Control.FOCUS_NONE
		b.add_theme_font_size_override("font_size", 13)
		b.button_pressed = f[0] == category
		var key: String = f[0]
		b.pressed.connect(func() -> void:
			category = key
			for k in _filter_buttons:
				_filter_buttons[k].button_pressed = k == key
			_last_top = -1
			_refresh())
		head.add_child(b)
		_filter_buttons[key] = b
	var grow := Control.new()
	grow.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(grow)
	var major := CheckButton.new()
	major.text = "只看要事"
	major.focus_mode = Control.FOCUS_NONE
	major.button_pressed = major_only
	major.add_theme_font_size_override("font_size", 13)
	major.toggled.connect(func(on: bool) -> void:
		major_only = on
		_last_top = -1
		_refresh())
	head.add_child(major)
	var close := Button.new()
	close.focus_mode = Control.FOCUS_NONE
	close.custom_minimum_size = Vector2(32, 30)
	close.tooltip_text = "关闭（C）"
	var cic := UIIcon.new("close", 16)
	cic.position = Vector2(8, 7)
	close.add_child(cic)
	close.pressed.connect(func() -> void:
		visible = false
		closed.emit())
	head.add_child(close)

	var body := HBoxContainer.new()
	body.add_theme_constant_override("separation", 12)
	body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(body)
	var left := ScrollContainer.new()
	left.custom_minimum_size = Vector2(330, 0)
	left.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	body.add_child(left)
	_list = VBoxContainer.new()
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_list.add_theme_constant_override("separation", 2)
	left.add_child(_list)
	body.add_child(VSeparator.new())

	var right := VBoxContainer.new()
	right.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	right.add_theme_constant_override("separation", 6)
	body.add_child(right)
	_head_title = UITheme.label("选择一条记录", 17, UITheme.TEXT, true)
	_head_title.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	right.add_child(_head_title)
	_head_meta = UITheme.label("", 12, UITheme.TEXT_FAINT)
	right.add_child(_head_meta)
	_actions = HBoxContainer.new()
	_actions.add_theme_constant_override("separation", 6)
	right.add_child(_actions)
	right.add_child(HSeparator.new())
	right.add_child(UITheme.label("因果链 · 左边是起因，右边是后果；点击任一事件继续追溯", 12, UITheme.TEXT_FAINT))
	_graph_scroll = ScrollContainer.new()
	_graph_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	right.add_child(_graph_scroll)
	_graph = CausalGraph.new()
	_graph.sim = sim
	_graph.node_selected.connect(select_event)
	_graph_scroll.add_child(_graph)
	_summary = UITheme.label("", 13, UITheme.TEXT_DIM)
	_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	right.add_child(_summary)


func open_at(id: int = 0) -> void:
	visible = true
	_graph.sim = sim
	_last_top = -1
	_refresh()
	if id > 0:
		select_event(id)


func select_event(id: int) -> void:
	selected_id = id
	var ev: Dictionary = sim.event(id)
	if ev.is_empty():
		return
	_head_title.text = String(ev.get("text", ""))
	var cat := String(ev.get("category", "other"))
	_head_meta.text = "%s · %s · 重要度 %d" % [ev.get("time", ""), _category_name(cat), int(ev.get("severity", 0))]
	for c in _actions.get_children():
		c.queue_free()
	var pos: Vector3i = ev.get("pos", Vector3i.ZERO)
	if pos != Vector3i.ZERO:
		var b := _small_button("在世界中定位")
		b.pressed.connect(func() -> void: focus_requested.emit(Vector3(pos) + Vector3(0.5, 0.5, 0.5)))
		_actions.add_child(b)
	var data = ev.get("data", {})
	if typeof(data) == TYPE_DICTIONARY and data.has("decision"):
		var did := int(data["decision"])
		var b2 := _small_button("查看她的考量")
		b2.pressed.connect(func() -> void: decision_requested.emit(did))
		_actions.add_child(b2)
	_graph.show_event(id)
	# Keep the focused event in view once the graph has been laid out.
	_scroll_to_focus.call_deferred()
	# A one-line account of where it all began.
	var ancestors: PackedInt32Array = sim.event_causes(id)
	if ancestors.is_empty():
		_summary.text = "这件事没有记录在案的前因。"
	else:
		var root: Dictionary = sim.event(int(ancestors[ancestors.size() - 1]))
		_summary.text = "可追溯到 %d 个前因，最早的是：%s（%s）" % [ancestors.size(), root.get("text", "?"), root.get("time", "")]
	for row in _list.get_children():
		if row is Button:
			(row as Button).button_pressed = int(row.get_meta("id", 0)) == id


func _scroll_to_focus() -> void:
	await get_tree().process_frame
	var r: Rect2 = _graph.focus_rect()
	_graph_scroll.scroll_horizontal = int(maxf(0.0, r.get_center().x - _graph_scroll.size.x * 0.6))


func _small_button(text: String) -> Button:
	var b := Button.new()
	b.text = text
	b.focus_mode = Control.FOCUS_NONE
	b.add_theme_font_size_override("font_size", 12)
	b.add_theme_stylebox_override("normal", UITheme.flat(Color(1, 1, 1, 0.06), 8, 10, 4))
	return b


func _category_name(cat: String) -> String:
	for f in FILTERS:
		if f[0] == cat:
			return f[1]
	return "其他"


func _process(delta: float) -> void:
	if not visible or sim == null or not sim.has_game():
		return
	_timer -= delta
	if _timer <= 0.0:
		_timer = 1.0
		_refresh()


func _refresh() -> void:
	var evs: Array = sim.recent_events(category, 3 if major_only else 1, 150, 0)
	var top := int(evs[0]["id"]) if not evs.is_empty() else 0
	if top == _last_top:
		return
	_last_top = top
	for c in _list.get_children():
		c.queue_free()
	if evs.is_empty():
		_list.add_child(UITheme.label("暂无记录", 13, UITheme.TEXT_FAINT))
		return
	var last_day := ""
	for ev in evs:
		var t: String = ev.get("time", "")
		var day := t.substr(0, t.rfind(" ")) if t.contains(" ") else t
		if day != last_day:
			last_day = day
			var dl := UITheme.label(day, 11, UITheme.TEXT_FAINT)
			_list.add_child(dl)
		_list.add_child(_row(ev))


func _row(ev: Dictionary) -> Button:
	var id := int(ev["id"])
	var b := Button.new()
	b.toggle_mode = true
	b.focus_mode = Control.FOCUS_NONE
	b.button_pressed = id == selected_id
	b.set_meta("id", id)
	b.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	b.custom_minimum_size = Vector2(0, 32)
	var row := HBoxContainer.new()
	row.mouse_filter = Control.MOUSE_FILTER_IGNORE
	row.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	row.offset_left = 8
	row.offset_right = -6
	row.add_theme_constant_override("separation", 8)
	b.add_child(row)
	var dot := UIIcon.new("dot", 12)
	dot.color = CausalGraph.color_for(String(ev.get("category", "other")))
	dot.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	row.add_child(dot)
	var ts := String(ev.get("time", ""))
	var time := UITheme.label(ts.substr(ts.rfind(" ") + 1), 11, UITheme.TEXT_FAINT)
	time.custom_minimum_size = Vector2(40, 0)
	time.mouse_filter = Control.MOUSE_FILTER_IGNORE
	time.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	row.add_child(time)
	var sev := int(ev.get("severity", 0))
	var l := UITheme.label(String(ev.get("text", "")), 13, UITheme.TEXT if sev >= 3 else UITheme.TEXT_DIM, sev >= 5)
	l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	l.clip_text = true
	l.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	l.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	l.mouse_filter = Control.MOUSE_FILTER_IGNORE
	row.add_child(l)
	b.tooltip_text = String(ev.get("text", ""))
	b.pressed.connect(func() -> void: select_event(id))
	return b
