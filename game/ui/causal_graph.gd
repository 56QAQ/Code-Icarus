class_name CausalGraph
extends Control
## Causal chain of one chronicle event: its causes (and their causes) in columns to the
## left, the event in focus, and what it led to on the right. Nodes are small cards;
## edges are curves from cause to effect. Clicking a node refocuses the graph on it.

signal node_selected(id: int)

const NODE_W := 200.0
const NODE_H := 58.0
const COL_GAP := 64.0
const ROW_GAP := 12.0
const MAX_DEPTH := 6
const MAX_EFFECTS := 6

const CATEGORY_COLORS := {
	"politics": Color(0.91, 0.76, 0.44),
	"disaster": Color(0.93, 0.42, 0.40),
	"economy": Color(0.52, 0.80, 0.52),
	"life": Color(0.62, 0.72, 0.95),
	"admin": Color(0.50, 0.84, 0.82),
	"other": Color(0.62, 0.66, 0.74),
}

var sim: IcarusSim
var focus_id := 0

var _nodes := {}        # id -> {"ev": Dictionary, "col": int, "row": int, "rect": Rect2}
var _edges: Array = []  # [from_id, to_id]


func focus_rect() -> Rect2:
	if _nodes.has(focus_id):
		return _nodes[focus_id]["rect"]
	return Rect2()


static func color_for(category: String) -> Color:
	return CATEGORY_COLORS.get(category, CATEGORY_COLORS["other"])


func show_event(id: int) -> void:
	focus_id = id
	_rebuild()


func _rebuild() -> void:
	for c in get_children():
		c.queue_free()
	_nodes.clear()
	_edges.clear()
	if sim == null or focus_id <= 0:
		custom_minimum_size = Vector2.ZERO
		queue_redraw()
		return
	var focus: Dictionary = sim.event(focus_id)
	if focus.is_empty():
		queue_redraw()
		return
	# Ancestors by breadth-first search; column = -(shortest distance).
	var depth := {focus_id: 0}
	var events := {focus_id: focus}
	var frontier: Array = [focus_id]
	var d := 0
	while not frontier.is_empty() and d < MAX_DEPTH:
		d += 1
		var next: Array = []
		for id in frontier:
			var ev: Dictionary = events[id]
			for c in ev.get("causes", PackedInt32Array()):
				var cid := int(c)
				_edges.append([cid, int(id)])
				if depth.has(cid):
					continue
				var cev: Dictionary = sim.event(cid)
				if cev.is_empty():
					continue
				depth[cid] = d
				events[cid] = cev
				next.append(cid)
		frontier = next
	# Consequences (one step).
	var effects: PackedInt32Array = sim.event_effects(focus_id)
	for i in mini(effects.size(), MAX_EFFECTS):
		var eid := int(effects[i])
		if depth.has(eid):
			continue
		var eev: Dictionary = sim.event(eid)
		if eev.is_empty():
			continue
		depth[eid] = -1
		events[eid] = eev
		_edges.append([focus_id, eid])
	# Columns: deepest cause on the left.
	var max_d := 0
	for id in depth:
		max_d = maxi(max_d, int(depth[id]))
	var columns := {}
	for id in depth:
		var col: int = max_d - int(depth[id])
		if not columns.has(col):
			columns[col] = []
		columns[col].append(id)
	var tallest := 0
	for col in columns:
		columns[col].sort()
		tallest = maxi(tallest, columns[col].size())
	var height := tallest * (NODE_H + ROW_GAP) + 8.0
	var ncols: int = columns.size()
	for col in columns:
		var ids: Array = columns[col]
		var col_h := ids.size() * (NODE_H + ROW_GAP)
		for row in ids.size():
			var id: int = ids[row]
			var x := 4.0 + float(col) * (NODE_W + COL_GAP)
			var y := (height - col_h) * 0.5 + float(row) * (NODE_H + ROW_GAP)
			var rect := Rect2(Vector2(x, y), Vector2(NODE_W, NODE_H))
			_nodes[id] = {"ev": events[id], "rect": rect}
			add_child(_card(events[id], rect, id == focus_id))
	custom_minimum_size = Vector2(ncols * (NODE_W + COL_GAP) + 8.0, maxf(height, NODE_H + 16.0))
	queue_redraw()


func _card(ev: Dictionary, rect: Rect2, focused: bool) -> Control:
	var cat: String = ev.get("category", "other")
	var col := color_for(cat)
	var b := Button.new()
	b.focus_mode = Control.FOCUS_NONE
	b.position = rect.position
	b.size = rect.size
	b.clip_contents = true
	var sb := UITheme.flat(Color(col, 0.16) if focused else Color(0.10, 0.12, 0.17, 0.96), 8, 8, 5)
	sb.border_width_left = 3
	sb.border_color = col
	if focused:
		sb.set_border_width_all(1)
		sb.border_width_left = 3
	b.add_theme_stylebox_override("normal", sb)
	var hover := sb.duplicate() as StyleBoxFlat
	hover.bg_color = Color(col, 0.22)
	b.add_theme_stylebox_override("hover", hover)
	b.add_theme_stylebox_override("pressed", hover)
	b.tooltip_text = "%s\n%s" % [ev.get("time", ""), ev.get("text", "")]
	var v := VBoxContainer.new()
	v.mouse_filter = Control.MOUSE_FILTER_IGNORE
	v.position = Vector2(10, 4)
	v.size = rect.size - Vector2(16, 6)
	v.add_theme_constant_override("separation", 0)
	b.add_child(v)
	var t := UITheme.label(String(ev.get("time", "")).get_slice("·", 1).strip_edges(), 10, UITheme.TEXT_FAINT)
	t.mouse_filter = Control.MOUSE_FILTER_IGNORE
	v.add_child(t)
	var l := UITheme.label(String(ev.get("text", "")), 12, UITheme.TEXT if not focused else Color.WHITE)
	l.autowrap_mode = TextServer.AUTOWRAP_ARBITRARY
	l.custom_minimum_size = Vector2(rect.size.x - 18, 0)
	l.size = Vector2(rect.size.x - 18, 34)
	l.mouse_filter = Control.MOUSE_FILTER_IGNORE
	v.add_child(l)
	var id := int(ev.get("id", 0))
	b.pressed.connect(func() -> void: node_selected.emit(id))
	return b


func _draw() -> void:
	for e in _edges:
		if not _nodes.has(e[0]) or not _nodes.has(e[1]):
			continue
		var a: Rect2 = _nodes[e[0]]["rect"]
		var b: Rect2 = _nodes[e[1]]["rect"]
		var p0 := Vector2(a.end.x, a.get_center().y)
		var p1 := Vector2(b.position.x, b.get_center().y)
		if p1.x < p0.x:
			# Same column (rare): route around the right side.
			p0 = Vector2(a.end.x, a.get_center().y)
			p1 = Vector2(b.end.x, b.get_center().y)
		var cat: String = _nodes[e[1]]["ev"].get("category", "other")
		var col := Color(color_for(cat), 0.55)
		var pts := PackedVector2Array()
		var dx := maxf(24.0, absf(p1.x - p0.x) * 0.5)
		var c1 := p0 + Vector2(dx, 0)
		var c2 := p1 - Vector2(dx, 0)
		for i in 21:
			var t := float(i) / 20.0
			var u := 1.0 - t
			pts.append(p0 * (u * u * u) + c1 * (3.0 * u * u * t) + c2 * (3.0 * u * t * t) + p1 * (t * t * t))
		draw_polyline(pts, col, 1.6, true)
		# Arrow head.
		draw_colored_polygon(PackedVector2Array([p1, p1 + Vector2(-7, -4), p1 + Vector2(-7, 4)]), col)
