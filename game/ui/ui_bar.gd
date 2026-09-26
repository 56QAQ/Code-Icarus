class_name UIBar
extends Control
## Compact labelled meter: "label ▮▮▮▮▯▯ value". Colour follows the value unless fixed.

var label := ""
var value := 0.0          # 0..1 (or -1..1 when bipolar)
var bipolar := false
var fixed_color := Color(0, 0, 0, 0)
var show_value := true
var value_text := ""
var label_width := 56.0


func _init(label_: String = "", bipolar_ := false) -> void:
	label = label_
	bipolar = bipolar_
	custom_minimum_size = Vector2(200, 20)
	mouse_filter = Control.MOUSE_FILTER_PASS


func set_value(v: float, text := "") -> void:
	value = v
	value_text = text
	queue_redraw()


func _color() -> Color:
	if fixed_color.a > 0.0:
		return fixed_color
	var v := value
	if bipolar:
		return UITheme.GOOD if v >= 0.15 else (UITheme.BAD if v <= -0.15 else UITheme.TEXT_DIM)
	if v < 0.25:
		return UITheme.BAD
	if v < 0.5:
		return UITheme.WARN
	return UITheme.GOOD


func _draw() -> void:
	var font := UITheme.font()
	var fs := 12
	var h := size.y
	var ty := h * 0.5 + fs * 0.36
	draw_string(font, Vector2(0, ty), label, HORIZONTAL_ALIGNMENT_LEFT, label_width, fs, UITheme.TEXT_DIM)
	var vx := size.x - (38.0 if show_value else 0.0)
	var bar := Rect2(label_width, h * 0.5 - 3.0, maxf(10.0, vx - label_width - 6.0), 6.0)
	draw_rect(bar, Color(1, 1, 1, 0.08), true)
	var c := _color()
	if bipolar:
		var mid := bar.position.x + bar.size.x * 0.5
		var w := bar.size.x * 0.5 * clampf(absf(value), 0.0, 1.0)
		var r := Rect2(mid if value >= 0.0 else mid - w, bar.position.y, w, bar.size.y)
		draw_rect(r, c, true)
		draw_line(Vector2(mid, bar.position.y - 2), Vector2(mid, bar.end.y + 2), Color(1, 1, 1, 0.25), 1.0)
	else:
		var r := Rect2(bar.position, Vector2(bar.size.x * clampf(value, 0.0, 1.0), bar.size.y))
		draw_rect(r, c, true)
	if show_value:
		var t := value_text if value_text != "" else ("%+.2f" % value if bipolar else "%d%%" % int(round(value * 100.0)))
		draw_string(font, Vector2(vx, ty), t, HORIZONTAL_ALIGNMENT_RIGHT, 38.0, fs, UITheme.TEXT)
