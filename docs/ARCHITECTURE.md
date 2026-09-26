# Code:Icarus — Architecture

## 1. Layers

```
 game/ (Godot 4.5, GDScript)      presentation: rendering, camera, UI, input, audio,
        │                          remote Jev calls (HTTP), screenshots
        ▼
 gdext/ (IcarusSim, C++)          thin binding: Variant <-> kernel types, mesh arrays
        ▼
 core/  (C++20, no Godot)         the simulation kernel: world, physics, agents, economy,
                                   society, magic, decisions, chronicle, save/load
        ▲
 tools/cli (icarus_cli)            headless runs, scenario tests, map dumps, replays
 tests/                             kernel tests
```

The world exists only in the kernel. Godot reads it (meshes, entity snapshots,
chronicle) and sends commands (admin actions, decision responses). Nothing in the
simulation depends on the camera, frame rate or scene tree.

## 2. Scale and units (prototype values, tunable)

| quantity | value | notes |
|---|---|---|
| cube | 1 world unit ≈ 0.5 m | terrain/building resolution |
| cell (地格) | 32×32×32 cubes | generation/storage/scheduling unit |
| world | 32×8×32 cells = 1024×256×1024 cubes | main island r≈112 + 4 islets |
| body voxel | 1/8 cube | characters are ~3 cubes (1.5 m) tall |
| tick | 1/20 s at 1x | 6000 ticks per day (5 min at 1x) |

## 3. World (core/world)

* `Voxel` = u16: material (8 bits) | level/stage (4) | damage (3) | burning (1).
* `Cell` lifecycle **Ungenerated → Active ↔ Dormant**:
  * *Ungenerated*: only seed + macro info (biome from `WorldGen::cell_biome`).
  * *Active*: decompressed; simulation may touch it. Only changed cubes are processed.
  * *Dormant*: RLE-compressed (uniform cells need no array). Keeps `last_sim_tick`,
    owner, environment summary and `pending` events. Waking calls `World::on_wake`
    with the elapsed interval so slow processes catch up.
* `World::get/set` = simulation access (activates, touches). `World::peek*` =
  renderer/analysis access (generates pristine data if needed, never changes
  simulation-relevant fields). This is what guarantees the camera cannot alter history.
* Cells idle for `idle_ticks` with no holds are demoted to Dormant.
* Every `set` is journaled as a `VoxelChange{pos, before, after, cause}`; the simulation
  drains the journal each tick and dispatches it to physics, buildings, navigation.
* Save files store only non-pristine cells (the world's *changes*) plus cell metadata;
  pristine cells are regenerated from the seed. Generation is a pure function.

## 4. Matter (core/sim/physics)

* **Water**: integer units (15 per cube), conserved. Falls, then spreads to lower
  neighbours, prefers edges with drops (waterfalls). Leaves the world below y=0 (the
  abyss under the floating island). Only shallow puddles evaporate. Springs are explicit
  sources (1 unit / N ticks); destroying the spring cube emits `WaterSourceLost`.
* **Fire**: burning flag on flammable cubes; spreads by material flammability (upwards
  faster); water quenches (consuming water); burnt cubes become `burn_to` material.
* **Granular** (sand, gravel, rubble, ash): falls and slides with an angle of repose.
* **Support**: removing a rigid cube triggers bounded BFS from its neighbours. A
  component is supported if it reaches a levistone anchor or any cube of a pristine
  cell (unmodified island mass). Unsupported components become **debris bodies**
  (aggregated rigid fragments) that fall, crush characters, and re-embed on landing
  (brittle parts shatter, stone may break into rubble). Fragments falling off the
  island are lost to the abyss.
* **Meteors**: fall along a trajectory, carve a jagged crater, scorch/ignite the rim,
  leave meteoric iron + basalt, and queue area damage.
* Physics never touches characters directly; it queues `AreaDamage` for the agents.

## 5. Chronicle (core/sim/chronicle)

Append-only log of `Event{id, tick, type, severity, pos, actor, target, polity,
causes[4], text, data}`. Causes link events into chains, e.g.

```
AdminAction(meteor) → MeteorImpact → StructureDestroyed(bridge) → PathBlocked
  → LogisticsDisrupted(food) → Shortage(food) → DecisionRequested(ruler)
  → DecisionMade(ration) → SupportShift(-) → Protest → Secession
```

Each system that reacts to a change passes the triggering event id on as a cause.

## 6. Agents (core/agents) — see docs/AGENTS.md when written

Characters (residents and magical girls) with voxel bodies, needs, personality, skills,
relationships, support for each magical girl, inventory, memory and a decision trace.
Rule-based utility AI with hysteresis; tasks are small state machines; all resource
targets are reserved with expiry; A* over standable cube positions with repath on
invalidation and temporary blacklisting of unreachable targets.

## 7. Economy

Material ledger: every item unit lives in exactly one inventory (stockpile, character,
ground pile). Sources (harvest, mining, admin gifts) and sinks (eating, spoilage,
construction, crafting) are explicit and counted, so conservation is testable.

## 8. Society, magic and decisions

Polity (identity, display name, culture, ruler, policies, projects, territory),
support dynamics, crises. Magical girls: drive (源动力) + separate personality, levels,
2 active + 2 passive spells from data. Strategic choices go through the **Jev pipeline**:
program builds feasible options with computed costs → a decision provider (local persona
model or remote LLM via the Godot host) picks one with a rationale → local executor
applies it → outcome is checked later. Requests have deadlines, budgets, staleness
checks, fallbacks, and are logged for replay.

## 9. Determinism

* All randomness from `Rng` streams owned by systems (saved), or pure hashes.
* No iteration over unordered containers in simulation logic.
* `-ffp-contract=off`.
* Tests assert: same seed ⇒ same state hash; save→load→continue ⇒ same hash as an
  uninterrupted run. Remote Jev responses are not reproducible by nature: they are
  recorded and replayed from the decision log instead.
