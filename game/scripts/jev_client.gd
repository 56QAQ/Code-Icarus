extends Node
## Jev: remote decision provider (autoload "Jev").
##
## The kernel never touches the network. When the decision mode is "remote", decisions
## wait in the kernel as "awaiting"; this node sends each one to the Claude API with the
## kernel-built prompt and a JSON schema that only admits the feasible option keys, then
## submits the answer back. The kernel validates it (option exists, feasible, situation
## not stale) and falls back to the local persona model on any failure or timeout.
##
## Configuration lives in user://jev.cfg (API key, model, effort, budget). The
## ANTHROPIC_API_KEY environment variable is used when no key is stored.

signal status_changed

const API_URL := "https://api.anthropic.com/v1/messages"
const DEFAULT_MODEL := "claude-opus-5"
const CONFIG_PATH := "user://jev.cfg"
const MAX_IN_FLIGHT := 2
const REQUEST_TIMEOUT_S := 90.0
## Real-time seconds the simulation may be held so a pending answer can arrive.
const MAX_HOLD_S := 25.0

var enabled := false
var api_key := ""
var model := DEFAULT_MODEL
var effort := "medium"          # low | medium | high
var budget_per_day := 24        # remote calls per in-game day
var deadline_ticks := 900       # in-game ticks a decision waits for an answer
var hold_for_answers := true    # briefly pause time while a girl is thinking remotely

var api_url := API_URL           # overridable (ICARUS_JEV_URL) for offline tests

var calls_total := 0
var calls_failed := 0
var last_error := ""
var last_model_used := ""

var _in_flight := {}            # decision id -> {"http": HTTPRequest, "started": msec}
var _poll := 0.0


func _ready() -> void:
	_load_config()
	var override := OS.get_environment("ICARUS_JEV_URL")
	if not override.is_empty():
		api_url = override
	Game.game_started.connect(_apply_mode)


func has_key() -> bool:
	return not effective_key().is_empty()


func effective_key() -> String:
	if not api_key.is_empty():
		return api_key
	return OS.get_environment("ANTHROPIC_API_KEY")


func in_flight_count() -> int:
	return _in_flight.size()


func is_thinking(decision_id: int) -> bool:
	return _in_flight.has(decision_id)


## True while the simulation should wait for an answer that is about to be needed.
func should_hold() -> bool:
	if not (enabled and hold_for_answers) or _in_flight.is_empty() or Game.sim == null:
		return false
	var now := Time.get_ticks_msec()
	var tick: int = Game.sim.get_tick()
	for id in _in_flight:
		var f: Dictionary = _in_flight[id]
		if float(now - int(f["started"])) / 1000.0 > MAX_HOLD_S:
			continue
		var dl: int = f.get("deadline", 0)
		if dl > 0 and tick >= dl - 40:
			return true
	return false


func configure(opts: Dictionary) -> void:
	for k in opts:
		if k in ["enabled", "api_key", "model", "effort", "budget_per_day", "deadline_ticks", "hold_for_answers"]:
			set(k, opts[k])
	if model.strip_edges().is_empty():
		model = DEFAULT_MODEL
	_save_config()
	_apply_mode()
	status_changed.emit()


func _apply_mode() -> void:
	if Game.sim == null or not Game.sim.has_game():
		return
	var remote := enabled and has_key()
	Game.sim.set_decision_mode("remote" if remote else "local", budget_per_day, deadline_ticks)


func _process(delta: float) -> void:
	if not enabled or Game.sim == null or not Game.sim.has_game():
		return
	_poll -= delta
	if _poll > 0.0:
		return
	_poll = 0.25
	var waiting: PackedInt32Array = Game.sim.awaiting_remote()
	for id in waiting:
		if _in_flight.size() >= MAX_IN_FLIGHT:
			break
		if not _in_flight.has(id):
			_send(id)


func _send(id: int) -> void:
	var req: Dictionary = Game.sim.remote_request(id)
	var schema = JSON.parse_string(req.get("schema", "{}"))
	if typeof(schema) != TYPE_DICTIONARY:
		Game.sim.remote_failed(id, "invalid schema")
		return
	var body := {
		"model": model,
		"max_tokens": 4000,
		"system": req.get("system", ""),
		"messages": [{"role": "user", "content": req.get("user", "")}],
		"thinking": {"type": "adaptive"},
		"output_config": {
			"effort": effort,
			"format": {"type": "json_schema", "schema": schema},
		},
		# On a policy decline the API re-runs the request on its recommended fallback model.
		"fallbacks": "default",
	}
	var headers := PackedStringArray([
		"content-type: application/json",
		"x-api-key: " + effective_key(),
		"anthropic-version: 2023-06-01",
		"anthropic-beta: server-side-fallback-2026-07-01",
	])
	var http := HTTPRequest.new()
	http.timeout = REQUEST_TIMEOUT_S
	add_child(http)
	var d: Dictionary = Game.sim.decision(id)
	_in_flight[id] = {"http": http, "started": Time.get_ticks_msec(), "deadline": Game.sim.get_tick() + deadline_ticks}
	if d.has("created"):
		_in_flight[id]["deadline"] = int(d["created"]) + deadline_ticks
	http.request_completed.connect(_on_completed.bind(id))
	var err := http.request(api_url, headers, HTTPClient.METHOD_POST, JSON.stringify(body))
	calls_total += 1
	if err != OK:
		_fail(id, "request error %d" % err)
	status_changed.emit()


func _on_completed(result: int, code: int, _headers: PackedStringArray, body: PackedByteArray, id: int) -> void:
	if result != HTTPRequest.RESULT_SUCCESS:
		_fail(id, "network error %d" % result)
		return
	var text := body.get_string_from_utf8()
	var resp = JSON.parse_string(text)
	if code != 200 or typeof(resp) != TYPE_DICTIONARY:
		var msg := "HTTP %d" % code
		if typeof(resp) == TYPE_DICTIONARY and resp.has("error"):
			msg += ": " + str(resp["error"].get("message", ""))
		_fail(id, msg)
		return
	# A declined request (after any fallback) carries no usable answer.
	var stop: String = str(resp.get("stop_reason", ""))
	if stop == "refusal":
		_fail(id, "refusal")
		return
	if stop == "max_tokens":
		_fail(id, "answer truncated")
		return
	var answer_text := ""
	for block in resp.get("content", []):
		if typeof(block) == TYPE_DICTIONARY and block.get("type", "") == "text":
			answer_text += str(block.get("text", ""))
	var answer = JSON.parse_string(answer_text)
	if typeof(answer) != TYPE_DICTIONARY or not answer.has("choice"):
		_fail(id, "unparseable answer")
		return
	last_model_used = str(resp.get("model", model))
	var res: Dictionary = Game.sim.submit_decision(id, str(answer["choice"]), str(answer.get("rationale", "")), "remote:" + last_model_used)
	if not res.get("ok", false):
		calls_failed += 1
		last_error = "kernel rejected answer: " + str(res.get("error", ""))
	_finish(id)


func _fail(id: int, why: String) -> void:
	calls_failed += 1
	last_error = why
	if Game.sim != null:
		Game.sim.remote_failed(id, why)
	_finish(id)


func _finish(id: int) -> void:
	if _in_flight.has(id):
		var http: HTTPRequest = _in_flight[id]["http"]
		http.queue_free()
		_in_flight.erase(id)
	status_changed.emit()


func _load_config() -> void:
	var cfg := ConfigFile.new()
	if cfg.load(CONFIG_PATH) != OK:
		return
	enabled = cfg.get_value("jev", "enabled", false)
	api_key = cfg.get_value("jev", "api_key", "")
	model = cfg.get_value("jev", "model", DEFAULT_MODEL)
	effort = cfg.get_value("jev", "effort", "medium")
	budget_per_day = cfg.get_value("jev", "budget_per_day", 24)
	deadline_ticks = cfg.get_value("jev", "deadline_ticks", 900)
	hold_for_answers = cfg.get_value("jev", "hold_for_answers", true)


func _save_config() -> void:
	var cfg := ConfigFile.new()
	cfg.set_value("jev", "enabled", enabled)
	cfg.set_value("jev", "api_key", api_key)
	cfg.set_value("jev", "model", model)
	cfg.set_value("jev", "effort", effort)
	cfg.set_value("jev", "budget_per_day", budget_per_day)
	cfg.set_value("jev", "deadline_ticks", deadline_ticks)
	cfg.set_value("jev", "hold_for_answers", hold_for_answers)
	cfg.save(CONFIG_PATH)


func export_log(path: String) -> bool:
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		return false
	f.store_string(Game.sim.export_decision_log())
	return true
