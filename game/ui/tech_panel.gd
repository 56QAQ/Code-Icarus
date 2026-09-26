class_name TechPanel
extends PanelContainer
## 科技: the polity's technology across the four eras. Techs are drawn as cards in era
## columns joined by their prerequisites; the ruler's current research glows gold. The
## god does not pick research — the magical girls do, in council — so this is a view of
## what the civilisation knows, what it is working on and what each step unlocks.

signal closed

var sim: IcarusSim
var polity_id := 1

var _view: TechTreeView
var _head_era: Label
var _head_meta: Label
var _detail_title: Label
var _detail_body: Label
var _detail_extra: Label
var _timer := 0.0


func _ready() -> void:
	add_theme_stylebox_override("panel", UITheme.card_style(UITheme.BG_SOLID, 16, 14))
	mouse_filter = Control.MOUSE_FILTER_STOP
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 10)
	add_child(col)

	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", 10)
	col.add_child(head)
	var ic := UIIcon.new("flask", 22)
	ic.color = UITheme.ACCENT
	head.add_child(ic)
	head.add_child(UITheme.label("科技", 18, UITheme.TEXT, true))
	_head_era = UITheme.label("", 13, UITheme.ACCENT)
	var era_pill := PanelContainer.new()
	era_pill.add_theme_stylebox_override("panel", UITheme.flat(UITheme.ACCENT_SOFT, 8, 8, 2))
	era_pill.add_child(_head_era)
	head.add_child(era_pill)
	_head_meta = UITheme.label("", 13, UITheme.TEXT_DIM)
	head.add_child(_head_meta)
	var grow := Control.new()
	grow.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(grow)
	head.add_child(UITheme.label("研究方向由魔法少女在议事中决定", 12, UITheme.TEXT_FAINT))
	# The god can still hand knowledge down; it goes to whatever they are studying.
	var reveal := Button.new()
	reveal.text = "  降下启示"
	reveal.focus_mode = Control.FOCUS_NONE
	reveal.tooltip_text = "神迹：为当前研究（无人研究时为最便宜的可研究科技）增加 40 研究点。会记入编年史。"
	reveal.add_theme_font_size_override("font_size", 13)
	reveal.add_theme_color_override("font_color", UITheme.MAGIC)
	var ric := UIIcon.new("blessing", 14)
	ric.color = UITheme.MAGIC
	ric.position = Vector2(6, 8)
	reveal.add_child(ric)
	reveal.pressed.connect(func() -> void:
		Game.admin("enlighten", {"polity": polity_id, "points": 40})
		_timer = 0.15)
	head.add_child(reveal)
	var close := Button.new()
	close.focus_mode = Control.FOCUS_NONE
	close.custom_minimum_size = Vector2(32, 30)
	close.tooltip_text = "关闭（T）"
	var cic := UIIcon.new("close", 16)
	cic.set_anchors_preset(Control.PRESET_CENTER)
	cic.position = Vector2(-8, -8)
	close.add_child(cic)
	close.pressed.connect(func() -> void:
		visible = false
		closed.emit())
	head.add_child(close)

	_view = TechTreeView.new()
	_view.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_view.selection_changed.connect(func(_k: String) -> void: _show_detail())
	col.add_child(_view)

	var detail := PanelContainer.new()
	detail.add_theme_stylebox_override("panel", UITheme.flat(UITheme.BG_SOFT, 10, 14, 10))
	detail.custom_minimum_size = Vector2(0, 92)
	col.add_child(detail)
	var dcol := VBoxContainer.new()
	dcol.add_theme_constant_override("separation", 3)
	detail.add_child(dcol)
	_detail_title = UITheme.label("", 15, UITheme.TEXT, true)
	dcol.add_child(_detail_title)
	_detail_body = UITheme.label("", 13, UITheme.TEXT_DIM)
	_detail_body.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	dcol.add_child(_detail_body)
	_detail_extra = UITheme.label("", 12, UITheme.TEXT_DIM)
	_detail_extra.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	dcol.add_child(_detail_extra)


func open_for(pid: int) -> void:
	polity_id = pid
	visible = true
	_view.selected = ""
	_refresh()


func _process(delta: float) -> void:
	if not visible:
		return
	_timer -= delta
	if _timer <= 0.0:
		_timer = 1.0
		_refresh()


func _refresh() -> void:
	if sim == null or not sim.has_game():
		return
	var info: Dictionary = sim.polity_info(polity_id)
	if info.is_empty():
		return
	var tree: Array = sim.tech_tree(polity_id)
	var eras: Array = sim.rules_doc("techs").get("eras", [])
	_view.set_data(tree, eras)
	_head_era.text = String(info.get("era_name", ""))
	var known := 0
	for t in tree:
		if t["state"] == "known":
			known += 1
	var res: Dictionary = info.get("research", {})
	var meta := "「%s」· 已掌握 %d / %d" % [info["name"], known, tree.size()]
	if not res.is_empty():
		meta += " · 研究中：%s %d%%" % [res["name"], int(100.0 * float(res["progress"]) / maxf(1.0, float(res["cost"])))]
	_head_meta.text = meta
	if _view.selected == "":
		_view.selected = String(res.get("key", ""))
	_show_detail()


func _show_detail() -> void:
	var t: Dictionary = _view.tech(_view.selected)
	if t.is_empty():
		_detail_title.text = "点选一项科技查看详情"
		_detail_body.text = "金色为正在研究的方向；绿色为已掌握；明亮的边框表示前置已满足、可以研究。"
		_detail_extra.text = ""
		return
	var state_names := {"known": "已掌握", "researching": "研究中", "available": "可研究", "locked": "未解锁"}
	var head := "%s · %s" % [t["name"], state_names.get(t["state"], "")]
	if float(t["cost"]) > 0.0 and t["state"] != "known":
		head += " · %d / %d 研究点" % [int(t["progress"]), int(t["cost"])]
	_detail_title.text = head
	_detail_body.text = String(t["desc"])
	var parts := PackedStringArray()
	var req: PackedStringArray = t["requires"]
	if not req.is_empty():
		var names := PackedStringArray()
		for k in req:
			var r: Dictionary = _view.tech(k)
			names.append(("✓" if r.get("state", "") == "known" else "✗") + String(r.get("name", k)))
		parts.append("前置：" + " ".join(names))
	var un: PackedStringArray = t["unlocks"]
	if not un.is_empty():
		parts.append("解锁：" + "、".join(un))
	if String(t["effects"]) != "":
		parts.append("效果：" + String(t["effects"]))
	_detail_extra.text = "　·　".join(parts)


## The drawn tree: era columns, cards and prerequisite curves.
class TechTreeView extends Control:
	signal selection_changed(key: String)

	var selected := ""
	var _techs: Array = []
	var _eras: Array = []
	var _by_key := {}
	var _rects := {}        # key -> Rect2
	var _hover := ""
	var _sig := ""

	const CARD_H := 50.0
	const GAP_Y := 12.0
	const HEAD_H := 30.0

	func _init() -> void:
		mouse_filter = Control.MOUSE_FILTER_STOP
		custom_minimum_size = Vector2(0, 5 * (CARD_H + GAP_Y) + HEAD_H + 8)
		resized.connect(_layout)

	func tech(key: String) -> Dictionary:
		return _by_key.get(key, {})

	func set_data(techs: Array, eras: Array) -> void:
		var sig := ""
		for t in techs:
			sig += "%s:%s:%d|" % [t["key"], t["state"], int(t["progress"])]
		if sig == _sig:
			return
		_sig = sig
		_techs = techs
		_eras = eras
		_by_key.clear()
		for t in techs:
			_by_key[String(t["key"])] = t
		_layout()

	func _layout() -> void:
		_rects.clear()
		var n_eras := maxi(_eras.size(), 1)
		for t in _techs:
			n_eras = maxi(n_eras, int(t["era"]) + 1)
		var col_w := size.x / float(n_eras)
		var card_w := minf(col_w - 36.0, 210.0)
		# Order each column by where its prerequisites sit, so curves rarely cross.
		var row_of := {}
		for e in n_eras:
			var col: Array = []
			for t in _techs:
				if int(t["era"]) == e:
					col.append(t)
			var keyed: Array = []
			for i in col.size():
				var t: Dictionary = col[i]
				var ys := 0.0
				var n := 0
				for r in t["requires"]:
					if row_of.has(r):
						ys += float(row_of[r])
						n += 1
				keyed.append([ys / n if n > 0 else float(i), i, t])
			keyed.sort_custom(func(a: Array, b: Array) -> bool: return a[0] < b[0] or (a[0] == b[0] and a[1] < b[1]))
			var total_h := keyed.size() * (CARD_H + GAP_Y) - GAP_Y
			var y0 := HEAD_H + maxf(0.0, (size.y - HEAD_H - total_h) * 0.5)
			for j in keyed.size():
				var t: Dictionary = keyed[j][2]
				row_of[String(t["key"])] = j
				var x := col_w * e + (col_w - card_w) * 0.5
				_rects[String(t["key"])] = Rect2(x, y0 + j * (CARD_H + GAP_Y), card_w, CARD_H)
		queue_redraw()

	func _gui_input(event: InputEvent) -> void:
		if event is InputEventMouseMotion:
			var h := _hit((event as InputEventMouseMotion).position)
			if h != _hover:
				_hover = h
				queue_redraw()
		elif event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
			var h := _hit((event as InputEventMouseButton).position)
			if h != "":
				selected = h
				selection_changed.emit(h)
				queue_redraw()
				accept_event()

	func _hit(p: Vector2) -> String:
		for k in _rects:
			if (_rects[k] as Rect2).has_point(p):
				return k
		return ""

	func _state_color(state: String) -> Color:
		match state:
			"known":
				return UITheme.GOOD
			"researching":
				return UITheme.ACCENT
			"available":
				return UITheme.TEXT
		return UITheme.TEXT_FAINT

	func _draw() -> void:
		var font := UITheme.font()
		var bold := UITheme.font_bold()
		var n_eras := maxi(_eras.size(), 1)
		var col_w := size.x / float(n_eras)
		# Era columns.
		for e in n_eras:
			var x := col_w * e
			if e % 2 == 1:
				draw_rect(Rect2(x, 0, col_w, size.y), Color(1, 1, 1, 0.022))
			var name: String = _eras[e] if e < _eras.size() else "第%d时代" % e
			draw_string(bold, Vector2(x, 20), name, HORIZONTAL_ALIGNMENT_CENTER, col_w, 14, UITheme.TEXT_DIM)
		# Prerequisite curves (under the cards). The focused tech's links are highlighted.
		var focus := _hover if _hover != "" else selected
		for t in _techs:
			var k := String(t["key"])
			if not _rects.has(k):
				continue
			var to: Rect2 = _rects[k]
			for r in t["requires"]:
				if not _rects.has(r):
					continue
				var from: Rect2 = _rects[r]
				var lit: bool = focus == k or focus == r
				var done: bool = (_by_key.get(r, {}) as Dictionary).get("state", "") == "known"
				var c := Color(UITheme.GOOD, 0.55) if done else Color(1, 1, 1, 0.16)
				if lit:
					c = Color(UITheme.ACCENT, 0.9)
				_curve(from, to, c, 2.4 if lit else 1.6)
		# Cards.
		for t in _techs:
			var k := String(t["key"])
			if not _rects.has(k):
				continue
			var r: Rect2 = _rects[k]
			var state := String(t["state"])
			var sc := _state_color(state)
			var bg := Color(1, 1, 1, 0.045)
			if state == "known":
				bg = Color(UITheme.GOOD, 0.12)
			elif state == "researching":
				bg = Color(UITheme.ACCENT, 0.16)
			var sb := UITheme.flat(bg, 9)
			sb.set_border_width_all(2 if (k == selected or state == "researching") else 1)
			sb.border_color = UITheme.ACCENT if k == selected else (Color(sc, 0.7) if state != "locked" else Color(1, 1, 1, 0.08))
			if k == _hover and k != selected:
				sb.bg_color = bg.lightened(0.08)
				sb.border_color = Color(UITheme.TEXT, 0.5)
			draw_style_box(sb, r)
			# State marker.
			var mc := r.position + Vector2(16, CARD_H * 0.5)
			match state:
				"known":
					draw_circle(mc, 7, UITheme.GOOD)
					draw_polyline(PackedVector2Array([mc + Vector2(-3.5, 0), mc + Vector2(-1, 2.8), mc + Vector2(3.8, -2.8)]), UITheme.BG_SOLID, 1.8, true)
				"researching":
					var frac := clampf(float(t["progress"]) / maxf(1.0, float(t["cost"])), 0.0, 1.0)
					draw_arc(mc, 6.5, 0, TAU, 24, Color(UITheme.ACCENT, 0.25), 2.4, true)
					draw_arc(mc, 6.5, -PI / 2, -PI / 2 + TAU * frac, 24, UITheme.ACCENT, 2.4, true)
				"available":
					draw_arc(mc, 6.5, 0, TAU, 24, Color(UITheme.TEXT, 0.8), 1.6, true)
				_:
					draw_arc(mc, 6.5, 0, TAU, 24, Color(1, 1, 1, 0.18), 1.4, true)
			var tx := r.position.x + 32
			var tw := r.size.x - 40
			draw_string(bold, Vector2(tx, r.position.y + 21), String(t["name"]), HORIZONTAL_ALIGNMENT_LEFT, tw, 14,
				UITheme.TEXT if state != "locked" else UITheme.TEXT_FAINT)
			var sub := ""
			var cost := float(t["cost"])
			match state:
				"known":
					sub = "已掌握"
				"researching":
					sub = "研究中 %d / %d" % [int(t["progress"]), int(cost)]
				_:
					sub = ("%d 研究点" % int(cost)) if cost > 0.0 else "基础"
					if float(t["progress"]) > 0.0:
						sub = "已积累 %d / %d" % [int(t["progress"]), int(cost)]
			var un: PackedStringArray = t["unlocks"]
			if not un.is_empty() and state != "researching":
				sub += " · " + un[0] + ("等" if un.size() > 1 else "")
			draw_string(font, Vector2(tx, r.position.y + 38), sub, HORIZONTAL_ALIGNMENT_LEFT, tw, 11, UITheme.TEXT_DIM)
			# Progress along the bottom edge.
			if cost > 0.0 and float(t["progress"]) > 0.0 and state != "known":
				var frac := clampf(float(t["progress"]) / cost, 0.0, 1.0)
				draw_rect(Rect2(r.position.x + 10, r.end.y - 5, (r.size.x - 20) * frac, 2.5), UITheme.ACCENT)

	func _curve(from: Rect2, to: Rect2, c: Color, w: float) -> void:
		var pts := PackedVector2Array()
		var a := Vector2(from.end.x, from.get_center().y)
		var b := Vector2(to.position.x, to.get_center().y)
		if absf(from.position.x - to.position.x) < 1.0:
			# Same era: loop around the right edge.
			a = Vector2(from.end.x, from.get_center().y)
			b = Vector2(to.end.x, to.get_center().y)
			var bulge := 22.0
			for i in 17:
				var s := float(i) / 16.0
				var p := a.lerp(b, s)
				p.x += sin(s * PI) * bulge
				pts.append(p)
		else:
			var dx := (b.x - a.x) * 0.5
			for i in 17:
				var s := float(i) / 16.0
				var p0 := a
				var p1 := a + Vector2(dx, 0)
				var p2 := b - Vector2(dx, 0)
				var p3 := b
				var u := 1.0 - s
				pts.append(p0 * u * u * u + p1 * 3.0 * u * u * s + p2 * 3.0 * u * s * s + p3 * s * s * s)
		draw_polyline(pts, c, w, true)
		# Arrowhead at the dependent card.
		var tip := pts[pts.size() - 1]
		var dir := (tip - pts[pts.size() - 3]).normalized()
		var nrm := Vector2(-dir.y, dir.x)
		draw_colored_polygon(PackedVector2Array([tip, tip - dir * 7 + nrm * 3.5, tip - dir * 7 - nrm * 3.5]), c)
