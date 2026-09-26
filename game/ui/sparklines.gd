class_name Sparklines
extends Control
## Small stacked line charts: [[label, PackedFloat32Array, color, min, max], ...].

var series: Array = []


func _draw() -> void:
	if series.is_empty():
		return
	var font := UITheme.font()
	var n := series.size()
	var h := size.y / float(n)
	for i in n:
		var s: Array = series[i]
		var data: PackedFloat32Array = s[1]
		var col: Color = s[2]
		var lo: float = s[3]
		var hi: float = s[4]
		var top := h * float(i)
		var area := Rect2(70, top + 3, size.x - 110, h - 6)
		draw_string(font, Vector2(0, top + h * 0.5 + 4), s[0], HORIZONTAL_ALIGNMENT_LEFT, 68, 11, UITheme.TEXT_DIM)
		draw_rect(area, Color(1, 1, 1, 0.03), true)
		if lo < 0.0 and hi > 0.0:
			var zy := area.end.y - (0.0 - lo) / (hi - lo) * area.size.y
			draw_line(Vector2(area.position.x, zy), Vector2(area.end.x, zy), Color(1, 1, 1, 0.12), 1.0)
		if data.size() >= 2:
			var pts := PackedVector2Array()
			var count := mini(data.size(), 240)
			var start := data.size() - count
			for k in count:
				var v := clampf(data[start + k], lo, hi)
				var x := area.position.x + area.size.x * float(k) / float(maxi(1, count - 1))
				var y := area.end.y - (v - lo) / (hi - lo) * area.size.y
				pts.append(Vector2(x, y))
			draw_polyline(pts, col, 1.6, true)
			draw_string(font, Vector2(area.end.x + 6, top + h * 0.5 + 4), "%.2f" % data[data.size() - 1], HORIZONTAL_ALIGNMENT_LEFT, 40, 11, col)
