class_name UITheme
extends RefCounted
## Visual language of the interface. Everything is built here in code so the look stays
## consistent: dark translucent glass cards, warm off-white text, a gold accent for
## selection, and levistone-cyan for magic/information.

const BG := Color(0.055, 0.07, 0.11, 0.86)
const BG_SOLID := Color(0.07, 0.085, 0.13, 0.97)
const BG_SOFT := Color(1, 1, 1, 0.05)
const BORDER := Color(1, 1, 1, 0.09)
const TEXT := Color(0.93, 0.91, 0.86)
const TEXT_DIM := Color(0.62, 0.66, 0.74)
const TEXT_FAINT := Color(0.45, 0.49, 0.57)
const ACCENT := Color(0.91, 0.76, 0.44)       # gold
const ACCENT_SOFT := Color(0.91, 0.76, 0.44, 0.18)
const MAGIC := Color(0.50, 0.84, 0.82)        # levistone cyan
const GOOD := Color(0.52, 0.80, 0.52)
const WARN := Color(0.95, 0.70, 0.35)
const BAD := Color(0.93, 0.42, 0.40)

const RADIUS := 12
const FONT_SIZE := 15

static var _font_regular: Font
static var _font_bold: Font


static func font() -> Font:
	if _font_regular == null:
		_font_regular = load("res://assets/fonts/NotoSansSC-Regular.woff2")
	return _font_regular


static func font_bold() -> Font:
	if _font_bold == null:
		_font_bold = load("res://assets/fonts/NotoSansSC-Bold.woff2")
	return _font_bold


static func card_style(bg: Color = BG, radius: int = RADIUS, pad: int = 12) -> StyleBoxFlat:
	var sb := StyleBoxFlat.new()
	sb.bg_color = bg
	sb.set_corner_radius_all(radius)
	sb.set_border_width_all(1)
	sb.border_color = BORDER
	sb.shadow_color = Color(0, 0, 0, 0.35)
	sb.shadow_size = 14
	sb.shadow_offset = Vector2(0, 4)
	sb.set_content_margin_all(pad)
	sb.anti_aliasing = true
	return sb


static func flat(bg: Color, radius: int = 8, pad_h: int = 10, pad_v: int = 6) -> StyleBoxFlat:
	var sb := StyleBoxFlat.new()
	sb.bg_color = bg
	sb.set_corner_radius_all(radius)
	sb.content_margin_left = pad_h
	sb.content_margin_right = pad_h
	sb.content_margin_top = pad_v
	sb.content_margin_bottom = pad_v
	sb.anti_aliasing = true
	return sb


static func build() -> Theme:
	var t := Theme.new()
	t.default_font = font()
	t.default_font_size = FONT_SIZE

	t.set_stylebox("panel", "PanelContainer", card_style())
	t.set_stylebox("panel", "Panel", card_style())

	# Buttons: quiet by default, soft highlight on hover, gold when pressed/toggled.
	t.set_stylebox("normal", "Button", flat(Color(1, 1, 1, 0.0)))
	t.set_stylebox("hover", "Button", flat(Color(1, 1, 1, 0.08)))
	t.set_stylebox("pressed", "Button", flat(ACCENT_SOFT))
	t.set_stylebox("hover_pressed", "Button", flat(Color(0.91, 0.76, 0.44, 0.26)))
	t.set_stylebox("disabled", "Button", flat(Color(1, 1, 1, 0.0)))
	t.set_stylebox("focus", "Button", StyleBoxEmpty.new())
	t.set_color("font_color", "Button", TEXT)
	t.set_color("font_hover_color", "Button", Color.WHITE)
	t.set_color("font_pressed_color", "Button", ACCENT)
	t.set_color("font_hover_pressed_color", "Button", ACCENT)
	t.set_color("font_disabled_color", "Button", TEXT_FAINT)
	t.set_color("font_focus_color", "Button", TEXT)

	t.set_color("font_color", "Label", TEXT)

	# Sliders.
	var track := flat(Color(1, 1, 1, 0.10), 3, 0, 2)
	t.set_stylebox("slider", "HSlider", track)
	var fill := flat(ACCENT, 3, 0, 2)
	t.set_stylebox("grabber_area", "HSlider", fill)
	t.set_stylebox("grabber_area_highlight", "HSlider", fill)
	t.set_icon("grabber", "HSlider", _dot_texture(ACCENT, 7))
	t.set_icon("grabber_highlight", "HSlider", _dot_texture(Color.WHITE, 7))

	# Tooltips.
	t.set_stylebox("panel", "TooltipPanel", card_style(BG_SOLID, 8, 8))
	t.set_color("font_color", "TooltipLabel", TEXT)

	# Separators.
	var sep := StyleBoxLine.new()
	sep.color = Color(1, 1, 1, 0.10)
	sep.thickness = 1
	sep.vertical = true
	t.set_stylebox("separator", "VSeparator", sep)
	var hsep := StyleBoxLine.new()
	hsep.color = Color(1, 1, 1, 0.08)
	hsep.thickness = 1
	t.set_stylebox("separator", "HSeparator", hsep)

	# Scroll bars: thin and unobtrusive.
	t.set_stylebox("scroll", "VScrollBar", flat(Color(1, 1, 1, 0.03), 4, 2, 2))
	t.set_stylebox("grabber", "VScrollBar", flat(Color(1, 1, 1, 0.16), 4, 2, 2))
	t.set_stylebox("grabber_highlight", "VScrollBar", flat(Color(1, 1, 1, 0.28), 4, 2, 2))
	t.set_stylebox("grabber_pressed", "VScrollBar", flat(ACCENT, 4, 2, 2))

	# Text inputs.
	t.set_stylebox("normal", "LineEdit", flat(Color(1, 1, 1, 0.06), 8))
	t.set_stylebox("focus", "LineEdit", flat(Color(1, 1, 1, 0.10), 8))
	t.set_color("font_color", "LineEdit", TEXT)
	return t


static func _dot_texture(c: Color, r: int) -> ImageTexture:
	var s := r * 2 + 2
	var img := Image.create(s, s, false, Image.FORMAT_RGBA8)
	var center := Vector2(s / 2.0, s / 2.0)
	for y in s:
		for x in s:
			var d := Vector2(x + 0.5, y + 0.5).distance_to(center)
			var a := clampf(float(r) + 0.5 - d, 0.0, 1.0)
			img.set_pixel(x, y, Color(c.r, c.g, c.b, a))
	return ImageTexture.create_from_image(img)


static func label(text: String, size: int = FONT_SIZE, color: Color = TEXT, bold := false) -> Label:
	var l := Label.new()
	l.text = text
	l.add_theme_font_size_override("font_size", size)
	l.add_theme_color_override("font_color", color)
	if bold:
		l.add_theme_font_override("font", font_bold())
	return l
