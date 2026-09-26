# End-to-end check of the Jev remote client against a local mock of the Messages API.
# Run with tools/jev_mock_test.sh (starts the mock server and sets ICARUS_JEV_URL).
extends SceneTree

var _frames := 0
var _game: Node
var _jev: Node


func _setup() -> void:
	_game = root.get_node_or_null("Game")
	_jev = root.get_node_or_null("Jev")
	if _game == null or _jev == null:
		print("JEV_MOCK FAIL: autoloads missing")
		quit(1)
		return
	_game.start_new_game({"seed": 1})
	_jev.configure({"enabled": true, "api_key": "test-key", "model": "claude-opus-5", "deadline_ticks": 2000})
	_game.speed = 20.0
	# Break the bridge so the ruler must decide.
	var f: Dictionary = _game.sim.world_info()["features"]
	var mid: Vector3i = (Vector3i(f["bridge_a"]) + Vector3i(f["bridge_b"])) / 2
	_game.sim.admin("dig", {"pos": [mid.x, mid.y, mid.z], "radius": 4.5})


func _process(_delta: float) -> bool:
	_frames += 1
	if _frames == 1:
		_setup()  # autoloads are ready once the tree runs
		return false
	if _game == null:
		return true
	for d in _game.sim.decisions(0, 50):
		if String(d["source"]).begins_with("remote:"):
			print("decision %d by %s -> %s (%s): %s" % [d["id"], d["girl_name"], d["chosen"], d["source"], d["rationale"]])
			print("JEV_MOCK PASS")
			quit(0)
			return true
	if _frames > 1500:
		print("JEV_MOCK FAIL: no remote decision; calls=%d failed=%d last_error=%s" % [_jev.calls_total, _jev.calls_failed, _jev.last_error])
		for d in _game.sim.decisions(0, 50):
			print("  ", d)
		quit(1)
	return false
