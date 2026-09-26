class_name EndingPanel
extends PanelContainer
## 本轮终局: shown when one civilisation holds the whole island. Unification wins the
## round for the ruler and her drive — but the card also lays out what the island paid
## for it (people, living standard, knowledge, forest, stability), because unity is not
## the same as a happy or lasting civilisation. The world keeps running afterwards.

signal closed
signal trace_requested(event_id: int)

var sim: IcarusSim
var _event_id := 0
var _col: VBoxContainer


func _ready() -> void:
	var sb := UITheme.card_style(UITheme.BG_SOLID, 18, 22)
	sb.border_color = Color(UITheme.ACCENT, 0.55)
	sb.set_border_width_all(1)
	add_theme_stylebox_override("panel", sb)
	mouse_filter = Control.MOUSE_FILTER_STOP
	custom_minimum_size = Vector2(640, 0)
	_col = VBoxContainer.new()
	_col.add_theme_constant_override("separation", 12)
	add_child(_col)


func open() -> void:
	var rs: Dictionary = sim.round_state()
	if not rs.get("unified", false):
		return
	_event_id = int(rs["event"])
	for c in _col.get_children():
		c.queue_free()
	var info: Dictionary = sim.polity_info(int(rs["polity"]))
	# Header.
	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", 10)
	_col.add_child(head)
	var crown := UIIcon.new("crown", 30)
	crown.color = UITheme.ACCENT
	head.add_child(crown)
	var ht := VBoxContainer.new()
	ht.add_theme_constant_override("separation", 0)
	head.add_child(ht)
	ht.add_child(UITheme.label("本轮终局 · 空岛统一", 12, UITheme.ACCENT))
	ht.add_child(UITheme.label(String(info.get("title", "")), 22, UITheme.TEXT, true))
	var line := UITheme.label(String(rs.get("text", "")), 14, UITheme.TEXT_DIM)
	line.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_col.add_child(line)
	_col.add_child(UITheme.label("%s · %s" % [rs.get("time", ""), info.get("era_name", "")], 12, UITheme.TEXT_FAINT))
	# The cost of unity.
	var oc: Dictionary = info.get("outcomes", {})
	if not oc.is_empty():
		var tiles := HBoxContainer.new()
		tiles.add_theme_constant_override("separation", 8)
		_col.add_child(tiles)
		var ever := maxi(1, int(rs.get("people_ever", oc["population_peak"])))
		var pop_frac := float(oc["population"]) / float(ever)
		_tile(tiles, "人口", "%d" % oc["population"], pop_frac, "全岛曾有 %d 人" % ever)
		_tile(tiles, "生活水平", "%d%%" % int(float(oc["living"]) * 100), float(oc["living"]), _verdict(float(oc["living"]), ["民生凋敝", "勉强度日", "丰衣足食"]))
		_tile(tiles, "知识", "%d%%" % int(float(oc["knowledge"]) * 100), float(oc["knowledge"]), String(info.get("era_name", "")))
		_tile(tiles, "生态", "%d%%" % int(float(oc["ecology"]) * 100), float(oc["ecology"]), "林木 %d/%d" % [oc["trees"], oc["trees_initial"]])
		_tile(tiles, "稳定", "%d%%" % int(float(oc["stability"]) * 100), float(oc["stability"]), _verdict(float(oc["stability"]), ["人心离散", "暗流涌动", "安定"]))
		var note := UITheme.label(_reading(oc, rs), 13, UITheme.TEXT)
		note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		_col.add_child(note)
	# Reigns of the winner.
	var reigns: Array = info.get("reigns", [])
	if not reigns.is_empty():
		_col.add_child(UITheme.label("历代统治者", 12, UITheme.TEXT_FAINT))
		for r in reigns:
			_col.add_child(UITheme.label("%s　%s（%s）· %s" % [r["from"], r["ruler"], r["drive"], r["how"]], 12, UITheme.TEXT_DIM))
	# Actions.
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	row.alignment = BoxContainer.ALIGNMENT_END
	_col.add_child(row)
	var trace := Button.new()
	trace.text = "追溯统一的因果"
	trace.focus_mode = Control.FOCUS_NONE
	trace.pressed.connect(func() -> void:
		visible = false
		trace_requested.emit(_event_id))
	row.add_child(trace)
	var go := Button.new()
	go.text = "继续观察"
	go.focus_mode = Control.FOCUS_NONE
	go.add_theme_color_override("font_color", UITheme.ACCENT)
	go.pressed.connect(func() -> void:
		visible = false
		closed.emit())
	row.add_child(go)
	visible = true


func _verdict(v: float, words: Array) -> String:
	return words[0] if v < 0.4 else (words[1] if v < 0.7 else words[2])


## A plain reading of the measures: unity bought at what price.
func _reading(oc: Dictionary, rs: Dictionary) -> String:
	var bad := PackedStringArray()
	var dead := int(rs.get("dead", 0))
	if dead > 0:
		var gd := int(rs.get("girls_dead", 0))
		bad.append("一路上有 %d 人死去%s" % [dead, ("（其中 %d 位魔法少女）" % gd) if gd > 0 else ""])
	if int(rs.get("departed", 0)) > 0:
		bad.append("%d 人离开了这座岛" % int(rs["departed"]))
	if float(oc["living"]) < 0.5:
		bad.append("百姓生活困苦")
	if float(oc["ecology"]) < 0.7:
		bad.append("森林被砍伐焚毁了%d%%" % int(100.0 * (1.0 - float(oc["ecology"]))))
	if float(oc["stability"]) < 0.5:
		bad.append("统治并不稳固")
	if bad.is_empty():
		return "统一之后，这个文明依然人丁兴旺、生活安定——这是少见的好结局。世界会继续运转。"
	return "统一不等于幸福：" + "，".join(bad) + "。世界会继续运转，结局仍可能改变。"


func _tile(parent: HBoxContainer, label: String, value: String, frac: float, sub: String) -> void:
	var p := PanelContainer.new()
	p.add_theme_stylebox_override("panel", UITheme.flat(UITheme.BG_SOFT, 10, 10, 8))
	p.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(p)
	var v := VBoxContainer.new()
	v.add_theme_constant_override("separation", 2)
	p.add_child(v)
	v.add_child(UITheme.label(label, 11, UITheme.TEXT_FAINT))
	var c := UITheme.BAD if frac < 0.4 else (UITheme.WARN if frac < 0.7 else UITheme.GOOD)
	v.add_child(UITheme.label(value, 20, c, true))
	var bar := ColorRect.new()
	bar.color = Color(c, 0.8)
	bar.custom_minimum_size = Vector2(maxf(4.0, 96.0 * clampf(frac, 0.0, 1.0)), 3)
	bar.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	v.add_child(bar)
	v.add_child(UITheme.label(sub, 11, UITheme.TEXT_DIM))
