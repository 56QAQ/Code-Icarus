# Save slots end to end through the Game autoload: save a world to a slot, start a
# different world, load the slot back, and check it continues exactly as before.
# Run: HOME=$(mktemp -d) godot --headless --path game --script res://tests/save_test.gd
extends SceneTree

var _frames := 0


func _process(_delta: float) -> bool:
	_frames += 1
	if _frames < 2:
		return false  # autoloads are ready once the tree runs
	var game: Node = root.get_node_or_null("Game")
	if game == null:
		print("SAVE FAIL: Game autoload missing")
		quit(1)
		return true
	var ok := true
	game.autosave = false
	game.start_new_game({"seed": 7})
	game.sim.step(900)
	if not game.save_slot("2"):
		print("SAVE FAIL: could not save")
		quit(1)
		return true
	var info: Dictionary = game.slot_info("2")
	ok = ok and int(info.get("seed", 0)) == 7 and int(info.get("population", 0)) > 0
	var tick_saved: int = game.sim.get_tick()
	game.sim.step(300)
	var hash_continued: int = game.sim.state_hash()
	# Somewhere else entirely, then back.
	game.start_new_game({"seed": 8})
	game.sim.step(50)
	if not game.load_slot("2"):
		print("SAVE FAIL: could not load")
		quit(1)
		return true
	ok = ok and game.sim.get_tick() == tick_saved and int(game.sim.world_info()["seed"]) == 7
	game.sim.step(300)
	ok = ok and game.sim.state_hash() == hash_continued
	print("SAVE ", "PASS" if ok else "FAIL", " (slot: ", info, ")")
	quit(0 if ok else 1)
	return true
