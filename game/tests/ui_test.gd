# Headless UI test: runs the real main scene (renderers, HUD and every overlay) for a
# few frames each, so script and layout errors in the interface are caught without a
# display. Run: HOME=$(mktemp -d) godot --headless --path game --script res://tests/ui_test.gd
extends SceneTree

var _frames := 0
var _main: Node
var _step := 0
var _errors := 0


func _process(_delta: float) -> bool:
	_frames += 1
	if _frames == 2:
		var game: Node = root.get_node("Game")
		game.autosave = false
		_main = load("res://scenes/main.tscn").instantiate()
		root.add_child(_main)
		return false
	if _frames < 6 or _frames % 4 != 0:
		return false
	var hud = _main.get("hud")
	var game: Node = root.get_node("Game")
	match _step:
		0:
			hud.open_menu()
			_expect(hud.menu.visible, "world menu opens")
		1:
			hud.close_overlays()
			_expect(not hud.menu.visible, "world menu closes")
			hud.toggle_council()
			_expect(hud.council.visible, "council opens")
		2:
			hud.toggle_chronicle()
			_expect(hud.chronicle.visible and not hud.council.visible, "chronicle replaces council")
		3:
			hud.toggle_tech()
			_expect(hud.tech.visible, "tech tree opens")
		4:
			hud.close_overlays()
			hud.civ_card.expanded = true
			game.set_tool("miracle")
			var girl := -1
			for c in game.sim.characters():
				if c.get("girl", false):
					girl = int(c["id"])
			_main.call("_select_character", girl)
			hud.selection._tab = "politics"
		5:
			# The magical girls' web, and a girl's life story.
			hud.toggle_web()
			_expect(hud.web.visible, "bonds web opens")
			_expect(not (hud.web.get("_nodes") as Array).is_empty(), "the web shows the girls")
			_expect(game.sim.debates(1000000) is Array, "council debates are readable")
			hud.selection._tab = "bio"
		6:
			hud.toggle_web()
			_expect(not hud.web.visible, "bonds web closes")
			_expect(hud.selection.visible, "girl card still open with her biography")
		7:
			game.set_tool("inspect")
			game.admin("bless_food", {"pos": game.sim.world_info()["features"]["village"], "amount": 10})
			game.sim.step(5)
			# An animal's card.
			var beasts: Array = game.sim.animals(Vector3(512, 160, 512), 2000.0)
			_expect(not beasts.is_empty(), "the island has animals")
			if not beasts.is_empty():
				hud.selection.show_animal(int(beasts[0]["id"]))
				_expect(hud.selection.visible, "animal card opens")
		8:
			# A new world through the same path the menu uses.
			game.start_new_game({"seed": 3})
			_expect(not hud.selection.visible, "selection cleared on a new world")
		9:
			print("UI ", "PASS" if _errors == 0 else "FAIL")
			quit(0 if _errors == 0 else 1)
			return true
	_step += 1
	return false


func _expect(cond: bool, what: String) -> void:
	if not cond:
		_errors += 1
		print("UI check failed: ", what)
