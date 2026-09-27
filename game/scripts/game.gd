extends Node
## Global game state (autoload "Game"): owns the simulation kernel and advances it in
## real time. Everything the player sees is read from `sim`; everything the player does
## goes through `sim.admin(...)` so it is logged in the chronicle.

signal game_started
signal ticked(tick: int)
signal event_logged(ev: Dictionary)
signal speed_changed(speed: float, paused: bool)
signal tool_changed(tool: String)
signal selection_changed(sel: Dictionary)

const TICKS_PER_SECOND := 20.0
const SPEEDS: Array[float] = [1.0, 2.0, 5.0, 20.0]

var sim: IcarusSim
var speed := 1.0
var paused := false
var frame_budget_ms := 12.0
var current_tool := "inspect"
var tool_params := {"radius": 3.0, "material": "stone"}
var selection := {}

var holding := false  # time held while a magical girl awaits a remote answer
var autosave := true   # off for scripted runs (screenshots, tests)

var _tick_accum := 0.0
var _last_event_id := 0
var _rules_loaded := false


func _ready() -> void:
	sim = IcarusSim.new()
	_rules_loaded = _load_rules()
	get_window().size_changed.connect(_fit_ui)
	apply_ui_scale(load_ui_scale())


# --- Interface size -----------------------------------------------------------------
# The interface is laid out for a 1600×900 window and grows with the window (project
# setting: stretch mode canvas_items; the 3D view always renders at full resolution).
# The player's own preference multiplies that, and is kept in user://settings.cfg.

const SETTINGS_PATH := "user://settings.cfg"
const UI_SCALES := [0.8, 0.9, 1.0, 1.1, 1.25]


func load_ui_scale() -> float:
	var cfg := ConfigFile.new()
	if cfg.load(SETTINGS_PATH) != OK:
		return 1.0
	return clampf(float(cfg.get_value("display", "ui_scale", 1.0)), 0.5, 2.0)


## The smallest size the interface is drawn at, relative to its 1600×900 design, so text
## stays legible in a small window.
const UI_MIN_EFFECTIVE := 0.85
## The layout needs at least 1280×720 logical pixels (1600×900 / 1.25); larger factors
## would make the cards overlap.
const UI_MAX_FACTOR := 1.25
var _ui_pref := 1.0


func apply_ui_scale(scale: float, remember := false) -> void:
	_ui_pref = scale
	_fit_ui()
	if remember:
		var cfg := ConfigFile.new()
		cfg.load(SETTINGS_PATH)
		cfg.set_value("display", "ui_scale", scale)
		cfg.save(SETTINGS_PATH)


## The player's chosen interface size (1.0 = fitted to the window).
func ui_scale() -> float:
	return _ui_pref


func _fit_ui() -> void:
	var win := get_window()
	var base := Vector2(ProjectSettings.get_setting("display/window/size/viewport_width", 1600),
		ProjectSettings.get_setting("display/window/size/viewport_height", 900))
	var fitted := minf(float(win.size.x) / base.x, float(win.size.y) / base.y)
	var f := _ui_pref
	if fitted > 0.0 and fitted * f < UI_MIN_EFFECTIVE:
		f = UI_MIN_EFFECTIVE / fitted
	f = minf(f, UI_MAX_FACTOR)
	if not is_equal_approx(win.content_scale_factor, f):
		win.content_scale_factor = f


func _load_rules() -> bool:
	var files := {}
	for f in DirAccess.get_files_at("res://data"):
		if f.ends_with(".json"):
			files[f.get_basename()] = FileAccess.get_file_as_string("res://data/" + f)
	if not sim.load_rules(files):
		push_error("Rule data failed to load: " + sim.last_error())
		return false
	return true


func start_new_game(config: Dictionary = {}) -> bool:
	if not _rules_loaded:
		return false
	var cfg := {"seed": 1}
	cfg.merge(config, true)
	if not sim.new_game(cfg):
		push_error("new_game failed: " + sim.last_error())
		return false
	_last_event_id = 0
	_tick_accum = 0.0
	_last_autosave_day = int(sim.clock_info().get("day", 0))
	game_started.emit()
	return true


func set_speed(s: float) -> void:
	speed = s
	paused = false
	speed_changed.emit(speed, paused)


func toggle_pause() -> void:
	paused = not paused
	speed_changed.emit(speed, paused)


func set_tool(t: String) -> void:
	current_tool = t
	tool_changed.emit(t)


func select(sel: Dictionary) -> void:
	selection = sel
	selection_changed.emit(sel)


func admin(type: String, params: Dictionary) -> void:
	sim.admin(type, params)
	# Apply promptly even while paused so the player sees the result.
	if paused:
		sim.step(1)


func _process(delta: float) -> void:
	if sim == null or not sim.has_game():
		return
	holding = Jev.should_hold()
	if not paused and not holding:
		_tick_accum += delta * TICKS_PER_SECOND * speed
		var want := int(_tick_accum)
		if want > 0:
			var t0 := Time.get_ticks_usec()
			var done := 0
			while done < want:
				var n: int = mini(4, want - done)
				sim.step(n)
				done += n
				if float(Time.get_ticks_usec() - t0) / 1000.0 > frame_budget_ms:
					break
			# Drop backlog we could not afford rather than spiralling.
			_tick_accum = clampf(_tick_accum - float(want), 0.0, 4.0) if done >= want else 0.0
			ticked.emit(sim.get_tick())
			if autosave:
				_autosave_check()
	_poll_events()


func _poll_events() -> void:
	var evs: Array = sim.events_since(_last_event_id, 256)
	for ev in evs:
		_last_event_id = ev["id"]
		event_logged.emit(ev)


func save_game(path: String) -> bool:
	var bytes := sim.save_bytes()
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		return false
	f.store_buffer(bytes)
	return true


func load_game(path: String) -> bool:
	var bytes := FileAccess.get_file_as_bytes(path)
	if bytes.is_empty() or not sim.load_bytes(bytes):
		push_error("load failed: " + sim.last_error())
		return false
	# History already happened: no toasts for it.
	_last_event_id = sim.last_event_id()
	_tick_accum = 0.0
	_last_autosave_day = int(sim.clock_info().get("day", 0))
	game_started.emit()
	return true


# ------------------------------------------------------------------ save slots

const SAVE_DIR := "user://saves"
const SLOTS := ["auto", "1", "2", "3"]


func slot_path(slot: String) -> String:
	return "%s/%s.icarus" % [SAVE_DIR, slot]


## What a slot holds, without loading it: {} when empty.
func slot_info(slot: String) -> Dictionary:
	var meta := "%s/%s.json" % [SAVE_DIR, slot]
	if not FileAccess.file_exists(slot_path(slot)) or not FileAccess.file_exists(meta):
		return {}
	var d: Variant = JSON.parse_string(FileAccess.get_file_as_string(meta))
	return d if d is Dictionary else {}


func save_slot(slot: String) -> bool:
	DirAccess.make_dir_recursive_absolute(SAVE_DIR)
	if not save_game(slot_path(slot)):
		return false
	var ps: Array = sim.polities()
	var pop := 0
	var names := PackedStringArray()
	for p in ps:
		pop += int(p["stats"]["population"])
		names.append(String(p["name"]))
	var meta := {
		"time": sim.clock_info().get("text", ""),
		"seed": sim.world_info().get("seed", 0),
		"title": String(ps[0]["title"]) if not ps.is_empty() else "无主之地",
		"polities": "、".join(names),
		"population": pop,
		"saved_at": Time.get_datetime_string_from_system(false, true),
	}
	var f := FileAccess.open("%s/%s.json" % [SAVE_DIR, slot], FileAccess.WRITE)
	if f == null:
		return false
	f.store_string(JSON.stringify(meta))
	return true


func load_slot(slot: String) -> bool:
	return load_game(slot_path(slot))


var _last_autosave_day := 0


## Once a day of game time the world is saved to the autosave slot.
func _autosave_check() -> void:
	var day := int(sim.clock_info().get("day", 0))
	if day > _last_autosave_day:
		_last_autosave_day = day
		save_slot("auto")
