# Headless smoke test: the GDExtension loads, a village game starts and advances.
# Run: godot --headless --path game --script res://tests/smoke.gd
extends SceneTree

func _init() -> void:
	var ok := true
	if not ClassDB.class_exists("IcarusSim"):
		push_error("IcarusSim class missing: GDExtension not loaded")
		quit(1)
		return
	var sim = ClassDB.instantiate("IcarusSim")
	print("kernel: ", sim.kernel_version())
	var files := {}
	var dir := DirAccess.open("res://data")
	for f in dir.get_files():
		if f.ends_with(".json"):
			files[f.get_basename()] = FileAccess.get_file_as_string("res://data/" + f)
	if not sim.load_rules(files):
		push_error("load_rules failed: " + sim.last_error())
		ok = false
	elif not sim.new_game({"seed": 1}):
		push_error("new_game failed: " + sim.last_error())
		ok = false
	else:
		sim.step(200)
		var p: Dictionary = sim.polity_info(1)
		print("polity: ", p.get("title", "?"), " pop ", p["stats"]["population"], " tick ", sim.get_tick())
		ok = int(p["stats"]["population"]) > 0 and sim.characters().size() > 0
	print("SMOKE ", "PASS" if ok else "FAIL")
	quit(0 if ok else 1)
