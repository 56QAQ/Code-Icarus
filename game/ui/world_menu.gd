class_name WorldMenu
extends PanelContainer
## 世界: start a new island from a seed, save to and load from slots (with what each
## holds), or leave. Opened at launch, with Esc, or from the top bar; time stands still
## while it is open.

signal closed

var _col: VBoxContainer
var _seed_edit: LineEdit
var _status: Label
var _slot_box: VBoxContainer
var _resume: Button
var _was_paused := false

const SLOT_NAMES := {"auto": "自动存档", "1": "存档一", "2": "存档二", "3": "存档三"}


func _ready() -> void:
	var sb := UITheme.card_style(UITheme.BG_SOLID, 18, 22)
	sb.border_color = Color(UITheme.ACCENT, 0.35)
	sb.set_border_width_all(1)
	add_theme_stylebox_override("panel", sb)
	mouse_filter = Control.MOUSE_FILTER_STOP
	custom_minimum_size = Vector2(580, 0)
	_col = VBoxContainer.new()
	_col.add_theme_constant_override("separation", 12)
	add_child(_col)

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
	dice.pressed.connect(func() -> void: _seed_edit.text = str(randi_range(1, 999999)))
	row.add_child(dice)
	var g2 := Control.new()
	g2.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(g2)
	var start := _button("开辟这片空岛", UITheme.ACCENT)
	start.pressed.connect(_new_world)
	row.add_child(start)
	_col.add_child(UITheme.label("同一个种子总会生成同一座岛与同样的开局；之后的历史由魔法少女与你的干预写成。", 11, UITheme.TEXT_FAINT))

	# Slots.
	_col.add_child(_section("存档"))
	_slot_box = VBoxContainer.new()
	_slot_box.add_theme_constant_override("separation", 6)
	_col.add_child(_slot_box)

	var foot := HBoxContainer.new()
	_col.add_child(foot)
	_status = UITheme.label("", 12, UITheme.GOOD)
	_status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	foot.add_child(_status)
	var quit := _button("退出游戏", UITheme.TEXT_DIM)
	quit.pressed.connect(func() -> void: get_tree().quit())
	foot.add_child(quit)


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


func close() -> void:
	visible = false
	Game.paused = _was_paused
	Game.speed_changed.emit(Game.speed, Game.paused)
	closed.emit()


func _new_world() -> void:
	var s := _seed_edit.text.strip_edges()
	var seed := int(s) if s.is_valid_int() else (hash(s) & 0x7fffffff)
	if Game.start_new_game({"seed": maxi(1, seed)}):
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
