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

var _tick_accum := 0.0
var _last_event_id := 0
var _rules_loaded := false


func _ready() -> void:
	sim = IcarusSim.new()
	_rules_loaded = _load_rules()


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
	_last_event_id = 0
	game_started.emit()
	return true
