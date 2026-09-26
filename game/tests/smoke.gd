# Headless smoke test: verifies the GDExtension loads and the kernel runs.
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
	elif not sim.new_world(1):
		push_error("new_world failed: " + sim.last_error())
		ok = false
	else:
		print("world: ", sim.world_stats())
	print("SMOKE ", "PASS" if ok else "FAIL")
	quit(0 if ok else 1)
