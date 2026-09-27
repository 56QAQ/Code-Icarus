class_name GirlWebPanel
extends PanelContainer
## 羁绊: a centred overlay card drawing the magical girls as a web. Each people is a
## cluster in its own colour; a girl's circle grows with her following among the people
## (the factions); lines are the ties between girls: friends, rivals, teacher and
## student, nemeses. Hovering shows a girl's story in the side column; clicking opens
## her card.

signal character_requested(id: int)
signal event_requested(id: int)

var sim: IcarusSim

var _canvas: Control
var _detail: VBoxContainer
var _nodes: Array = []        # dictionaries from the kernel, plus "p" (position on the canvas) and "r"
var _edges: Array = []
var _by_id := {}
var _hover := -1
var _picked := -1
var _timer := 0.0

const KIND_COLORS := {
	1: Color(0.52, 0.82, 0.55),   # friend
	2: Color(0.96, 0.66, 0.30),   # rival
	3: Color(0.50, 0.80, 0.95),   # mentor → student
	5: Color(0.93, 0.36, 0.36),   # nemesis
}
const KIND_NAMES := {1: "挚友", 2: "对手", 3: "师徒", 5: "宿敌"}


func _ready() -> void:
	add_theme_stylebox_override("panel", UITheme.card_style(UITheme.BG_SOLID, 16, 14))
	mouse_filter = Control.MOUSE_FILTER_STOP
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 10)
	add_child(col)
	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", 8)
	col.add_child(head)
	var ic := UIIcon.new("bonds", 22)
	ic.color = UITheme.MAGIC
	head.add_child(ic)
	var titles := VBoxContainer.new()
	titles.add_theme_constant_override("separation", 0)
	titles.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(titles)
	titles.add_child(UITheme.label("魔法少女的羁绊", 18, UITheme.TEXT, true))
	titles.add_child(UITheme.label("挚友、对手、师徒与宿敌 · 圆越大，追随她的民众越多", 12, UITheme.TEXT_DIM))
	var close := Button.new()
	close.focus_mode = Control.FOCUS_NONE
	close.tooltip_text = "关闭（B）"
	var x := UIIcon.new("close", 18)
	x.set_anchors_and_offsets_preset(Control.PRESET_CENTER)
	close.custom_minimum_size = Vector2(30, 30)
	close.add_child(x)
	close.pressed.connect(func() -> void: visible = false)
	head.add_child(close)

	var body := HBoxContainer.new()
	body.add_theme_constant_override("separation", 12)
	body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(body)
	_canvas = Control.new()
	_canvas.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_canvas.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_canvas.mouse_filter = Control.MOUSE_FILTER_STOP
	_canvas.clip_contents = true
	_canvas.draw.connect(_draw_web)
	_canvas.gui_input.connect(_on_canvas_input)
	_canvas.resized.connect(_layout)
	body.add_child(_canvas)
	body.add_child(VSeparator.new())
	var right := ScrollContainer.new()
	right.custom_minimum_size = Vector2(280, 0)
	right.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	body.add_child(right)
	_detail = VBoxContainer.new()
	_detail.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_detail.add_theme_constant_override("separation", 6)
	right.add_child(_detail)

	var legend := HBoxContainer.new()
	legend.add_theme_constant_override("separation", 16)
	col.add_child(legend)
	for k in [1, 2, 3, 5]:
		var item := HBoxContainer.new()
		item.add_theme_constant_override("separation", 6)
		var sw := ColorRect.new()
		sw.color = KIND_COLORS[k]
		sw.custom_minimum_size = Vector2(18, 4)
		sw.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		item.add_child(sw)
		item.add_child(UITheme.label(KIND_NAMES[k], 12, UITheme.TEXT_DIM))
		legend.add_child(item)
	var crown := UIIcon.new("crown", 14)
	crown.color = UITheme.ACCENT
	legend.add_child(crown)
	legend.add_child(UITheme.label("统治者", 12, UITheme.TEXT_FAINT))
	var swords := UIIcon.new("swords", 14)
	swords.color = Color(1.0, 0.72, 0.45)
	legend.add_child(swords)
	legend.add_child(UITheme.label("随军出征", 12, UITheme.TEXT_FAINT))
	legend.add_child(UITheme.label("灰色：已故", 12, UITheme.TEXT_FAINT))


func open() -> void:
	visible = true
	_picked = -1
	_refresh()


func _process(delta: float) -> void:
	if not visible or sim == null:
		return
	_timer -= delta
	if _timer <= 0.0:
		_timer = 1.0
		_refresh()


func _refresh() -> void:
	var web: Dictionary = sim.girl_web()
	_nodes = web.get("nodes", [])
	_edges = web.get("edges", [])
	_by_id.clear()
	for n in _nodes:
		_by_id[int(n["id"])] = n
	_layout()
	_show_detail(_picked if _by_id.has(_picked) else _hover)


## Peoples around a ring; each people's girls in a small ring of their own.
func _layout() -> void:
	if _canvas == null:
		return
	var groups := {}
	var order: Array = []
	for n in _nodes:
		var pid := int(n["polity"])
		if not groups.has(pid):
			groups[pid] = []
			order.append(pid)
		groups[pid].append(n)
	order.sort()
	var sz := _canvas.size
	var center := sz * 0.5 + Vector2(0, 8)
	var room := minf(sz.x, sz.y) * (0.34 if order.size() == 1 else 0.12)
	# The ring of peoples fits inside the canvas with the largest cluster.
	var widest := 0.0
	for pid in order:
		var m: int = (groups[pid] as Array).size()
		widest = maxf(widest, (0.0 if m == 1 else minf(room, 24.0 + 13.0 * m)) + 40.0)
	var big := 0.0 if order.size() < 2 else maxf(0.0, minf(sz.x, sz.y) * 0.5 - widest - 26.0)
	for gi in order.size():
		var members: Array = groups[order[gi]]
		var ang := -PI / 2.0 + TAU * float(gi) / float(maxi(1, order.size()))
		# Wider than tall: stretch the ring sideways.
		var gc := center + Vector2(cos(ang) * big * clampf(sz.x / maxf(1.0, sz.y), 1.0, 1.6), sin(ang) * big)
		var small := 0.0 if members.size() == 1 else minf(room, 24.0 + 13.0 * members.size())
		for mi in members.size():
			var a2 := ang + TAU * float(mi) / float(members.size()) + PI * 0.25
			var n: Dictionary = members[mi]
			n["p"] = gc + Vector2(cos(a2), sin(a2)) * small
			n["r"] = 13.0 + 2.8 * sqrt(float(n.get("followers", 0)))
			n["group"] = gc
			n["group_r"] = small + 40.0
	_canvas.queue_redraw()


func _draw_web() -> void:
	var font := UITheme.font()
	var bold := UITheme.font_bold()
	# People: a soft disc in the people's colour, its name above.
	var drawn := {}
	for n in _nodes:
		var pid := int(n["polity"])
		if drawn.has(pid):
			continue
		drawn[pid] = true
		var pc: Color = n.get("polity_color", Color(0.5, 0.5, 0.5))
		var gc: Vector2 = n["group"]
		var gr: float = n["group_r"]
		_canvas.draw_circle(gc, gr, Color(pc.r, pc.g, pc.b, 0.08))
		_canvas.draw_arc(gc, gr, 0.0, TAU, 64, Color(pc.r, pc.g, pc.b, 0.35), 1.5, true)
		var title := String(n.get("polity_name", ""))
		var tw := bold.get_string_size(title, HORIZONTAL_ALIGNMENT_LEFT, -1, 14).x
		_canvas.draw_string(bold, gc + Vector2(-tw * 0.5, -gr - 8.0), title, HORIZONTAL_ALIGNMENT_LEFT, -1, 14, pc.lightened(0.3))
	# Ties.
	var focus := _picked if _picked >= 0 else _hover
	for e in _edges:
		var a: Dictionary = _by_id.get(int(e["a"]), {})
		var b: Dictionary = _by_id.get(int(e["b"]), {})
		if a.is_empty() or b.is_empty():
			continue
		var k := int(e["kind"])
		var c: Color = KIND_COLORS.get(k, UITheme.TEXT_FAINT)
		var lit := focus < 0 or focus == int(e["a"]) or focus == int(e["b"])
		c.a = 0.95 if lit else 0.18
		var pa: Vector2 = a["p"]
		var pb: Vector2 = b["p"]
		# A gentle curve, so ties between the same peoples do not overlap.
		var mid := (pa + pb) * 0.5
		var nrm := (pb - pa).orthogonal().normalized()
		var bend := mid + nrm * minf(40.0, pa.distance_to(pb) * 0.15)
		var pts := PackedVector2Array()
		for i in 17:
			var t := float(i) / 16.0
			pts.append(pa.lerp(bend, t).lerp(bend.lerp(pb, t), t))
		var width := 3.0 if lit and focus >= 0 else 2.0
		if k == 5:
			# Nemeses: a broken, angry line.
			for i in range(0, 16, 2):
				_canvas.draw_line(pts[i], pts[i + 1], c, width + 0.5, true)
		else:
			_canvas.draw_polyline(pts, c, width, true)
		if k == 3:
			# Teacher → student.
			var tip: Vector2 = pts[12]
			var dir := (pts[13] - pts[11]).normalized()
			var side := dir.orthogonal() * 5.0
			_canvas.draw_colored_polygon(PackedVector2Array([tip + dir * 7.0, tip - dir * 4.0 + side, tip - dir * 4.0 - side]), c)
	# Girls.
	for n in _nodes:
		var p: Vector2 = n["p"]
		var r: float = n["r"]
		var alive: bool = n.get("alive", true)
		var cloth: Color = n.get("cloth", Color.WHITE)
		var accent: Color = n.get("accent", Color.WHITE)
		if not alive:
			cloth = Color(0.35, 0.36, 0.4)
			accent = Color(0.55, 0.56, 0.6)
		var lit := focus < 0 or focus == int(n["id"]) or _tied(focus, int(n["id"]))
		var fade := 1.0 if lit else 0.35
		if int(n["id"]) == focus:
			_canvas.draw_circle(p, r + 7.0, Color(accent.r, accent.g, accent.b, 0.25))
		_canvas.draw_circle(p, r, Color(cloth.r, cloth.g, cloth.b, fade))
		_canvas.draw_arc(p, r, 0.0, TAU, 40, Color(accent.r, accent.g, accent.b, fade), 3.0, true)
		# Hair: a band over the top of the circle.
		var hair: Color = n.get("hair", Color(0.3, 0.2, 0.15))
		_canvas.draw_arc(p, r * 0.62, PI * 1.15, PI * 1.85, 16, Color(hair.r, hair.g, hair.b, fade), r * 0.5, true)
		if n.get("ruler", false):
			_crown(p + Vector2(0, -r - 9.0), UITheme.ACCENT * Color(1, 1, 1, fade))
		if n.get("champion", false):
			_swords(p + Vector2(r * 0.8, -r * 0.8), Color(1.0, 0.72, 0.45, fade))
		var name := String(n["name"])
		var sub := String(n.get("drive", ""))
		if n.has("born_drive"):
			sub = "%s←%s" % [n["drive"], n["born_drive"]]
		var nw := bold.get_string_size(name, HORIZONTAL_ALIGNMENT_LEFT, -1, 13).x
		var sw := font.get_string_size(sub, HORIZONTAL_ALIGNMENT_LEFT, -1, 11).x
		_canvas.draw_string(bold, p + Vector2(-nw * 0.5, r + 15.0), name, HORIZONTAL_ALIGNMENT_LEFT, -1, 13, Color(UITheme.TEXT, fade))
		_canvas.draw_string(font, p + Vector2(-sw * 0.5, r + 29.0), sub, HORIZONTAL_ALIGNMENT_LEFT, -1, 11,
			Color(accent.lightened(0.2).r, accent.lightened(0.2).g, accent.lightened(0.2).b, 0.9 * fade))
	if _nodes.is_empty():
		var t := "这个世界还没有魔法少女"
		var w := font.get_string_size(t, HORIZONTAL_ALIGNMENT_LEFT, -1, 14).x
		_canvas.draw_string(font, _canvas.size * 0.5 - Vector2(w * 0.5, 0), t, HORIZONTAL_ALIGNMENT_LEFT, -1, 14, UITheme.TEXT_DIM)


func _swords(at: Vector2, c: Color) -> void:
	_canvas.draw_circle(at, 8.0, Color(0.08, 0.09, 0.13, 0.9))
	_canvas.draw_line(at + Vector2(-5, -5), at + Vector2(5, 5), c, 2.0, true)
	_canvas.draw_line(at + Vector2(5, -5), at + Vector2(-5, 5), c, 2.0, true)


func _crown(at: Vector2, c: Color) -> void:
	var s := 7.0
	_canvas.draw_colored_polygon(PackedVector2Array([
		at + Vector2(-s, 3), at + Vector2(-s, -3), at + Vector2(-s * 0.5, 0), at + Vector2(0, -5),
		at + Vector2(s * 0.5, 0), at + Vector2(s, -3), at + Vector2(s, 3)]), c)


func _tied(a: int, b: int) -> bool:
	if a < 0:
		return false
	for e in _edges:
		if (int(e["a"]) == a and int(e["b"]) == b) or (int(e["a"]) == b and int(e["b"]) == a):
			return true
	return false


func _node_at(p: Vector2) -> int:
	for n in _nodes:
		if (n["p"] as Vector2).distance_to(p) <= float(n["r"]) + 4.0:
			return int(n["id"])
	return -1


func _on_canvas_input(e: InputEvent) -> void:
	if e is InputEventMouseMotion:
		var h := _node_at(e.position)
		if h != _hover:
			_hover = h
			if _picked < 0:
				_show_detail(_hover)
			_canvas.queue_redraw()
	elif e is InputEventMouseButton and e.pressed and e.button_index == MOUSE_BUTTON_LEFT:
		var h := _node_at(e.position)
		if e.double_click and h >= 0:
			character_requested.emit(h)
			return
		_picked = h
		_show_detail(h)
		_canvas.queue_redraw()


## The side column: who she is, what she has been through, her ties.
func _show_detail(id: int) -> void:
	for c in _detail.get_children():
		c.queue_free()
	if id < 0 or not _by_id.has(id):
		_detail.add_child(UITheme.label("悬停或点击一位魔法少女，查看她的羁绊与经历。双击打开她的人物卡。", 13, UITheme.TEXT_DIM))
		var hint := UITheme.label("", 12, UITheme.TEXT_FAINT)
		hint.text = "民众追随谁，谁就有底气与统治者抗衡：圆的大小便是这股力量。"
		hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		_detail.add_child(hint)
		for c in _detail.get_children():
			if c is Label:
				c.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
				c.custom_minimum_size = Vector2(260, 0)
		return
	var n: Dictionary = _by_id[id]
	var info: Dictionary = sim.character_info(id)
	var girl: Dictionary = info.get("girl_data", {})
	var title := UITheme.label(String(n["name"]) + ("" if n.get("alive", true) else "（已故）"), 16, UITheme.TEXT, true)
	_detail.add_child(title)
	var roles := {"ruler": "统治者", "minister": "大臣", "governor": "总督", "general": "将军", "none": "无职务"}
	var line := "%s · %s · Lv%d" % [n.get("polity_name", ""), roles.get(String(n.get("role", "none")), ""), int(n.get("level", 1))]
	_detail.add_child(UITheme.label(line, 12, UITheme.TEXT_DIM))
	var drive := "源动力：%s" % n.get("drive", "")
	if n.has("born_drive"):
		drive += "（原为「%s」）" % n["born_drive"]
	_detail.add_child(_wrap(UITheme.label(drive, 13, n.get("accent", UITheme.MAGIC))))
	_detail.add_child(UITheme.label("追随者：%d 人" % int(n.get("followers", 0)), 13, UITheme.TEXT))
	if not girl.is_empty():
		var weight := UIBar.new("心之重负", false)
		weight.custom_minimum_size = Vector2(260, 18)
		weight.set_value(clampf(float(girl.get("trauma", 0.0)) / 1.2, 0.0, 1.0))
		weight.fixed_color = UITheme.BAD
		_detail.add_child(weight)
		var solace := UIBar.new("慰藉", false)
		solace.custom_minimum_size = Vector2(260, 18)
		solace.set_value(clampf(float(girl.get("solace", 0.0)) / 1.2, 0.0, 1.0))
		solace.fixed_color = UITheme.GOOD
		_detail.add_child(solace)
		if girl.has("duel"):
			_detail.add_child(UITheme.label("正与%s对决" % girl["duel"], 13, UITheme.BAD))
		var bonds: Array = girl.get("bonds", [])
		_detail.add_child(UITheme.label("羁绊", 12, UITheme.TEXT_FAINT))
		if bonds.is_empty():
			_detail.add_child(UITheme.label("尚无深交", 12, UITheme.TEXT_DIM))
		for b in bonds:
			var btn := Button.new()
			btn.focus_mode = Control.FOCUS_NONE
			btn.alignment = HORIZONTAL_ALIGNMENT_LEFT
			btn.add_theme_font_size_override("font_size", 13)
			btn.text = "%s  %s%s" % [b["kind_name"], b["name"], "" if b.get("alive", true) else "（已故）"]
			btn.add_theme_color_override("font_color", KIND_COLORS.get(_kind_group(int(b["kind"])), UITheme.TEXT))
			var oid := int(b["id"])
			btn.pressed.connect(func() -> void:
				_picked = oid
				_show_detail(oid)
				_canvas.queue_redraw())
			_detail.add_child(btn)
	var open := Button.new()
	open.focus_mode = Control.FOCUS_NONE
	open.text = "打开人物卡 ›"
	open.add_theme_color_override("font_color", UITheme.ACCENT)
	open.pressed.connect(func() -> void: character_requested.emit(id))
	_detail.add_child(open)


func _kind_group(k: int) -> int:
	return 3 if k == 4 else k  # student and teacher share a colour


func _wrap(l: Label) -> Label:
	l.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	l.custom_minimum_size = Vector2(260, 0)
	return l
