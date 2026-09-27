class_name ArmyBanners
extends Node
## Armies in the field, seen from afar: a banner card over each army (which people, raid
## or campaign, how many, what it is doing right now) and a marker over its objective.
## Clicking a banner brings the camera to the army. Pure presentation.

signal focus_requested(pos: Vector3)

var sim: IcarusSim
var camera: Camera3D
var overlay: Control

var _cards := {}    # polity id -> Control
var _marks := {}    # polity id -> Control (objective marker)
var _armies: Array = []
var _poll := 0.0


func _process(delta: float) -> void:
	if sim == null or overlay == null or camera == null or not sim.has_game():
		_clear()
		return
	_poll -= delta
	if _poll <= 0.0:
		_poll = 0.25
		_armies = sim.armies()
		_sync()
	for a in _armies:
		var id := int(a["polity"])
		_place(_cards.get(id), Vector3(a["pos"]) + Vector3(0, 5.6, 0), 260.0, true)
		# (Once the army is there, its own banner says where it is.)
		var aiming := int(a["phase"]) in [1, 2] and String(a["aim"]) != "defend" \
			and Vector3(a["pos"]).distance_to(Vector3(a["objective"])) > 15.0
		_place(_marks.get(id), Vector3(a["objective"]) + Vector3(0, 3.0, 0), 320.0, aiming)


func _clear() -> void:
	for c in _cards.values():
		c.queue_free()
	for c in _marks.values():
		c.queue_free()
	_cards.clear()
	_marks.clear()
	_armies = []


func _sync() -> void:
	var seen := {}
	for a in _armies:
		var id := int(a["polity"])
		seen[id] = true
		if not _cards.has(id):
			_cards[id] = _make_card(id)
			_marks[id] = _make_mark(a)
		var card: Control = _cards[id]
		var col: Color = a["color"]
		(card.get_meta("title") as Label).text = "%s%s · %d 人" % [a["name"], a["kind"], int(a["count"])]
		(card.get_meta("status") as Label).text = String(a["status"]) + ("  （折损 %d）" % int(a["lost"]) if int(a["lost"]) > 0 else "")
		(card.get_meta("icon") as UIIcon).color = col.lightened(0.25)
		var sb: StyleBoxFlat = card.get_theme_stylebox("panel")
		sb.border_color = col.lightened(0.1)
		card.set_meta("pos", a["pos"])
		var mk: Control = _marks[id]
		(mk.get_meta("icon") as UIIcon).color = col.lightened(0.25)
	for id in _cards.keys():
		if not seen.has(id):
			_cards[id].queue_free()
			_marks[id].queue_free()
			_cards.erase(id)
			_marks.erase(id)


func _make_card(id: int) -> Control:
	var card := PanelContainer.new()
	card.mouse_filter = Control.MOUSE_FILTER_STOP
	card.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
	card.tooltip_text = "点击把镜头移到这支军队"
	var sb := UITheme.flat(Color(UITheme.BG_SOLID, 0.82), 10, 9, 4)
	sb.border_width_left = 4
	sb.border_width_bottom = 1
	sb.border_width_top = 1
	sb.border_width_right = 1
	card.add_theme_stylebox_override("panel", sb)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 7)
	row.mouse_filter = Control.MOUSE_FILTER_IGNORE
	card.add_child(row)
	var ic := UIIcon.new("swords", 20)
	ic.mouse_filter = Control.MOUSE_FILTER_IGNORE
	row.add_child(ic)
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", -2)
	col.mouse_filter = Control.MOUSE_FILTER_IGNORE
	row.add_child(col)
	var t := UITheme.label("", 13, Color(1.0, 0.95, 0.84), true)
	t.mouse_filter = Control.MOUSE_FILTER_IGNORE
	col.add_child(t)
	var st := UITheme.label("", 11, UITheme.TEXT_DIM)
	st.mouse_filter = Control.MOUSE_FILTER_IGNORE
	col.add_child(st)
	card.set_meta("title", t)
	card.set_meta("status", st)
	card.set_meta("icon", ic)
	card.set_meta("fade", 0.0)
	card.gui_input.connect(func(e: InputEvent) -> void:
		if e is InputEventMouseButton and e.pressed and e.button_index == MOUSE_BUTTON_LEFT:
			focus_requested.emit(Vector3(card.get_meta("pos", Vector3.ZERO))))
	card.visible = false
	overlay.add_child(card)
	return card


## The objective: a small crossed-swords pin in the army's colour.
func _make_mark(a: Dictionary) -> Control:
	var pin := PanelContainer.new()
	pin.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var sb := UITheme.flat(Color(UITheme.BG_SOLID, 0.6), 14, 5, 3)
	pin.add_theme_stylebox_override("panel", sb)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	row.mouse_filter = Control.MOUSE_FILTER_IGNORE
	pin.add_child(row)
	var ic := UIIcon.new("swords", 14)
	ic.mouse_filter = Control.MOUSE_FILTER_IGNORE
	row.add_child(ic)
	var l := UITheme.label("%s的目标" % a["name"], 11, UITheme.TEXT_DIM)
	l.mouse_filter = Control.MOUSE_FILTER_IGNORE
	row.add_child(l)
	pin.set_meta("icon", ic)
	pin.set_meta("fade", 0.0)
	pin.visible = false
	overlay.add_child(pin)
	return pin


## Over a point in the world, faded out when far away or behind the camera.
func _place(c: Control, world: Vector3, far: float, shown: bool) -> void:
	if c == null:
		return
	var dist := camera.global_position.distance_to(world)
	var target := 0.0 if not shown or camera.is_position_behind(world) else 1.0 - smoothstep(far * 0.75, far, dist)
	var fade := move_toward(float(c.get_meta("fade")), target, get_process_delta_time() * 4.0)
	c.set_meta("fade", fade)
	c.modulate.a = fade
	c.visible = fade > 0.01
	if not c.visible:
		return
	c.reset_size()
	var sp := camera.unproject_position(world)
	c.position = (sp - Vector2(c.size.x * 0.5, c.size.y)).round()
