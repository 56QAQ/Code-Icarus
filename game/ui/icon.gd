class_name UIIcon
extends Control
## Vector icons drawn with primitives, so the UI does not depend on glyph coverage of
## fonts and stays crisp at any scale.

@export var icon := "dot"
@export var color := Color(0.93, 0.91, 0.86)
@export var stroke := 1.8


func _init(name_: String = "dot", size_px: float = 22.0) -> void:
	icon = name_
	custom_minimum_size = Vector2(size_px, size_px)
	mouse_filter = Control.MOUSE_FILTER_IGNORE


func set_icon(name_: String) -> void:
	icon = name_
	queue_redraw()


func set_color(c: Color) -> void:
	color = c
	queue_redraw()


func _draw() -> void:
	var s := minf(size.x, size.y)
	var o := (size - Vector2(s, s)) * 0.5
	var P := func(x: float, y: float) -> Vector2: return o + Vector2(x, y) * s
	var w := stroke * s / 22.0
	match icon:
		"pause":
			draw_rect(Rect2(P.call(0.28, 0.22), Vector2(0.14, 0.56) * s), color)
			draw_rect(Rect2(P.call(0.58, 0.22), Vector2(0.14, 0.56) * s), color)
		"play":
			draw_colored_polygon(PackedVector2Array([P.call(0.3, 0.2), P.call(0.78, 0.5), P.call(0.3, 0.8)]), color)
		"fast":
			draw_colored_polygon(PackedVector2Array([P.call(0.14, 0.24), P.call(0.5, 0.5), P.call(0.14, 0.76)]), color)
			draw_colored_polygon(PackedVector2Array([P.call(0.5, 0.24), P.call(0.86, 0.5), P.call(0.5, 0.76)]), color)
		"inspect":
			draw_arc(P.call(0.42, 0.42), 0.24 * s, 0, TAU, 28, color, w, true)
			draw_line(P.call(0.6, 0.6), P.call(0.84, 0.84), color, w * 1.4, true)
		"dig":
			# pickaxe
			draw_arc(P.call(0.5, 0.62), 0.42 * s, deg_to_rad(215), deg_to_rad(325), 18, color, w * 1.2, true)
			draw_line(P.call(0.5, 0.22), P.call(0.5, 0.88), color, w * 1.2, true)
		"place":
			var top := PackedVector2Array([P.call(0.5, 0.16), P.call(0.84, 0.33), P.call(0.5, 0.5), P.call(0.16, 0.33)])
			draw_colored_polygon(top, Color(color, 0.95))
			draw_colored_polygon(PackedVector2Array([P.call(0.16, 0.33), P.call(0.5, 0.5), P.call(0.5, 0.86), P.call(0.16, 0.68)]), Color(color, 0.6))
			draw_colored_polygon(PackedVector2Array([P.call(0.84, 0.33), P.call(0.5, 0.5), P.call(0.5, 0.86), P.call(0.84, 0.68)]), Color(color, 0.35))
		"meteor":
			draw_circle(P.call(0.66, 0.66), 0.17 * s, color)
			draw_line(P.call(0.16, 0.16), P.call(0.52, 0.52), Color(color, 0.8), w * 1.3, true)
			draw_line(P.call(0.3, 0.12), P.call(0.58, 0.4), Color(color, 0.5), w, true)
			draw_line(P.call(0.12, 0.3), P.call(0.4, 0.58), Color(color, 0.5), w, true)
		"fire":
			var flame := PackedVector2Array([P.call(0.5, 0.1), P.call(0.64, 0.3), P.call(0.74, 0.48), P.call(0.76, 0.64),
				P.call(0.68, 0.8), P.call(0.5, 0.88), P.call(0.32, 0.8), P.call(0.24, 0.64), P.call(0.28, 0.46),
				P.call(0.38, 0.52), P.call(0.4, 0.34)])
			draw_colored_polygon(flame, color)
			draw_colored_polygon(PackedVector2Array([P.call(0.5, 0.5), P.call(0.6, 0.66), P.call(0.56, 0.8),
				P.call(0.44, 0.8), P.call(0.4, 0.66)]), Color(1, 1, 1, 0.35))
		"water":
			var d := PackedVector2Array()
			d.append(P.call(0.5, 0.14))
			# Round bottom: from the upper-left of the circle, down around the bottom, to the upper-right.
			for i in 17:
				var a := PI + 0.4 - float(i) / 16.0 * (PI + 0.8)
				d.append(P.call(0.5 + cos(a) * 0.24, 0.62 + sin(a) * 0.24))
			draw_colored_polygon(d, color)
		"save":
			draw_rect(Rect2(P.call(0.2, 0.2), Vector2(0.6, 0.6) * s), color, false, w)
			draw_rect(Rect2(P.call(0.32, 0.2), Vector2(0.36, 0.2) * s), color)
			draw_rect(Rect2(P.call(0.3, 0.56), Vector2(0.4, 0.24) * s), color, false, w)
		"load":
			draw_line(P.call(0.5, 0.2), P.call(0.5, 0.64), color, w, true)
			draw_line(P.call(0.32, 0.46), P.call(0.5, 0.64), color, w, true)
			draw_line(P.call(0.68, 0.46), P.call(0.5, 0.64), color, w, true)
			draw_line(P.call(0.2, 0.8), P.call(0.8, 0.8), color, w, true)
		"chronicle":
			for i in 4:
				var y := 0.24 + i * 0.17
				draw_circle(P.call(0.24, y), 0.045 * s, color)
				draw_line(P.call(0.36, y), P.call(0.8, y), Color(color, 0.85), w, true)
		"people":
			draw_circle(P.call(0.38, 0.34), 0.12 * s, color)
			draw_arc(P.call(0.38, 0.82), 0.24 * s, PI, TAU, 16, color, w * 1.2, true)
			draw_circle(P.call(0.68, 0.38), 0.09 * s, Color(color, 0.7))
			draw_arc(P.call(0.68, 0.8), 0.18 * s, PI, TAU, 16, Color(color, 0.7), w, true)
		"food":
			draw_arc(P.call(0.5, 0.5), 0.3 * s, 0, PI, 20, color, w * 1.2, true)
			draw_line(P.call(0.16, 0.5), P.call(0.84, 0.5), color, w * 1.2, true)
			draw_line(P.call(0.38, 0.38), P.call(0.44, 0.2), Color(color, 0.6), w, true)
			draw_line(P.call(0.56, 0.38), P.call(0.62, 0.2), Color(color, 0.6), w, true)
		"heart":
			var h := PackedVector2Array()
			for i in 32:
				var t := float(i) / 32.0 * TAU
				var x := 16.0 * pow(sin(t), 3.0)
				var y := -(13.0 * cos(t) - 5.0 * cos(2 * t) - 2.0 * cos(3 * t) - cos(4 * t))
				h.append(P.call(0.5 + x / 40.0, 0.48 + y / 40.0))
			draw_colored_polygon(h, color)
		"star":
			var st := PackedVector2Array()
			for i in 10:
				var a := -PI / 2 + float(i) * PI / 5.0
				var r := 0.36 if i % 2 == 0 else 0.15
				st.append(P.call(0.5 + cos(a) * r, 0.53 + sin(a) * r))
			draw_colored_polygon(st, color)
		"gear":
			draw_arc(P.call(0.5, 0.5), 0.2 * s, 0, TAU, 24, color, w * 1.3, true)
			for i in 8:
				var a := float(i) * TAU / 8.0
				draw_line(P.call(0.5 + cos(a) * 0.26, 0.5 + sin(a) * 0.26), P.call(0.5 + cos(a) * 0.36, 0.5 + sin(a) * 0.36), color, w * 1.6, true)
		"close":
			draw_line(P.call(0.25, 0.25), P.call(0.75, 0.75), color, w, true)
			draw_line(P.call(0.75, 0.25), P.call(0.25, 0.75), color, w, true)
		"sun":
			draw_circle(P.call(0.5, 0.5), 0.16 * s, color)
			for i in 8:
				var a := float(i) * TAU / 8.0
				draw_line(P.call(0.5 + cos(a) * 0.26, 0.5 + sin(a) * 0.26), P.call(0.5 + cos(a) * 0.36, 0.5 + sin(a) * 0.36), color, w, true)
		"moon":
			draw_circle(P.call(0.46, 0.5), 0.26 * s, color)
			draw_circle(P.call(0.6, 0.42), 0.22 * s, Color(0.07, 0.085, 0.13, 1.0))
		"crown":
			draw_colored_polygon(PackedVector2Array([P.call(0.16, 0.74), P.call(0.16, 0.34), P.call(0.34, 0.52), P.call(0.5, 0.26), P.call(0.66, 0.52), P.call(0.84, 0.34), P.call(0.84, 0.74)]), color)
		"shield":
			draw_colored_polygon(PackedVector2Array([P.call(0.5, 0.14), P.call(0.8, 0.26), P.call(0.74, 0.62), P.call(0.5, 0.86), P.call(0.26, 0.62), P.call(0.2, 0.26)]), color)
		"blessing":
			draw_arc(P.call(0.5, 0.5), 0.3 * s, 0, TAU, 28, Color(color, 0.5), w, true)
			draw_circle(P.call(0.5, 0.5), 0.12 * s, color)
			for i in 4:
				var a := float(i) * TAU / 4.0 + PI / 4.0
				draw_line(P.call(0.5 + cos(a) * 0.18, 0.5 + sin(a) * 0.18), P.call(0.5 + cos(a) * 0.42, 0.5 + sin(a) * 0.42), color, w, true)
		"scroll":
			draw_rect(Rect2(P.call(0.24, 0.18), Vector2(0.52, 0.64) * s), Color(color, 0.18))
			draw_rect(Rect2(P.call(0.24, 0.18), Vector2(0.52, 0.64) * s), color, false, w)
			for i in 3:
				var y := 0.34 + float(i) * 0.15
				draw_line(P.call(0.34, y), P.call(0.66 if i < 2 else 0.54, y), color, w, true)
		"flask":
			var fl := PackedVector2Array([P.call(0.4, 0.16), P.call(0.6, 0.16), P.call(0.6, 0.4), P.call(0.8, 0.78),
				P.call(0.74, 0.86), P.call(0.26, 0.86), P.call(0.2, 0.78), P.call(0.4, 0.4)])
			fl.append(fl[0])
			draw_colored_polygon(PackedVector2Array([P.call(0.3, 0.6), P.call(0.7, 0.6), P.call(0.8, 0.78),
				P.call(0.74, 0.86), P.call(0.26, 0.86), P.call(0.2, 0.78)]), Color(color, 0.55))
			draw_polyline(fl, color, w, true)
			draw_line(P.call(0.34, 0.16), P.call(0.66, 0.16), color, w, true)
		"swords":
			for sgn in [-1.0, 1.0]:
				var a: Vector2 = P.call(0.5 - 0.32 * sgn, 0.18)
				var b: Vector2 = P.call(0.5 + 0.26 * sgn, 0.76)
				draw_line(a, b, color, w * 1.3, true)
				var g: Vector2 = P.call(0.5 + 0.17 * sgn, 0.66)
				var nrm := (b - a).normalized().orthogonal() * 0.12 * s
				draw_line(g - nrm, g + nrm, color, w * 1.2, true)
				draw_circle(P.call(0.5 + 0.3 * sgn, 0.8), 0.045 * s, color)
		"trade":
			# Two arrows passing each other: goods out, goods back.
			draw_line(P.call(0.16, 0.36), P.call(0.72, 0.36), color, w * 1.2, true)
			draw_colored_polygon(PackedVector2Array([P.call(0.86, 0.36), P.call(0.68, 0.23), P.call(0.68, 0.49)]), color)
			draw_line(P.call(0.84, 0.66), P.call(0.28, 0.66), color, w * 1.2, true)
			draw_colored_polygon(PackedVector2Array([P.call(0.14, 0.66), P.call(0.32, 0.53), P.call(0.32, 0.79)]), color)
		"think":
			for i in 3:
				draw_circle(P.call(0.26 + float(i) * 0.24, 0.52), 0.07 * s, Color(color, 0.45 + 0.25 * float(i)))
		_:
			draw_circle(P.call(0.5, 0.5), 0.18 * s, color)
