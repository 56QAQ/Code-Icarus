# Code:Icarus — working notes for Claude Code

3D voxel god-simulation (Godot 4.5 + C++ kernel). Magical girls with different
源动力 (drives) and personalities shape an emergent civilisation on a floating island;
the player is an administrator who observes and intervenes. Full vision:
`docs/VISION.md` (the original brief). Architecture: `docs/ARCHITECTURE.md`.

## Layout
- `core/` — engine-independent simulation kernel (C++20). **Never include Godot here.**
  World state must exist and run without any scene/renderer.
- `tests/` — kernel tests (tiny framework in `test_framework.h`), run headless.
- `tools/cli/` — `icarus_cli`: headless runs, map dumps, scenario runs, replays.
- `gdext/` — thin GDExtension binding (`IcarusSim`), built with SCons against
  `thirdparty/godot-cpp` (branch 4.5, exceptions enabled).
- `game/` — Godot project (presentation, UI, input). Data files in `game/data/*.json`
  are the rules/content, loaded by the kernel (CLI reads them from disk; Godot passes
  file contents through `load_rules`).

## Commands
- `tools/setup_env.sh` — fresh container setup (Godot, scons, lavapipe, submodule).
- `tools/build.sh` — CMake kernel/tests/CLI + GDExtension (debug).
- `tools/test.sh [filter]` — kernel tests + Godot headless smoke test.
- `./build/tools/cli/icarus_cli map --seed N --out out/map.png` — top-down map.
- Screenshots: `tools/screenshot.sh` (Xvfb + lavapipe, Forward+).

## Rules of the road
- Determinism: all randomness through `icarus::Rng` streams owned by the simulation, or
  pure hashes (`hash3`) for generation. No unordered_map iteration in simulation logic.
  Keep `-ffp-contract=off`. Save → load → continue must equal an uninterrupted run
  (tested). Camera/renderer access uses `World::peek*` and must never change sim state.
- Material ledger: items are conserved; every unit lives in exactly one inventory/pile.
  Sources/sinks (harvest, eating, spoilage, admin creation) are explicit and logged.
- Causality: significant state changes emit chronicle events with cause links, so the
  UI can explain *why* (player action → physical change → logistics → decisions → politics).
- Small, verifiable steps: implement, test headless, check real rendering, commit.
- UI (user requirement): design a polished custom look from the start — custom theme,
  consistent palette & typography (Noto Sans SC bundled), floating cards/overlays, a
  compact top HUD and bottom tool dock, sensible information density. Do **not** ship
  plain default Godot controls and do **not** pile up side panels.
- Commit messages: imperative summary + short body. Don't put model names in commits.
