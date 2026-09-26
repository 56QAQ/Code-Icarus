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

**Ecology** (`sim/ecology.cpp`): once a day, near trees still standing in places the
simulation has touched, saplings take root on open grass (not against buildings) and
grow into real trees after three days; berry bushes re-sprout at the edge of the woods.
Untouched parts of the island are left as generated and cost nothing. Its random
stream and state are saved (older saves load with an empty ecology).

## 5. Chronicle (core/sim/chronicle)

Append-only log of `Event{id, tick, type, severity, pos, actor, target, polity,
causes[4], text, data}`. Causes link events into chains, e.g.

```
AdminAction(meteor) → MeteorImpact → StructureDestroyed(bridge) → PathBlocked
  → LogisticsDisrupted(food) → Shortage(food) → DecisionRequested(ruler)
  → DecisionMade(ration) → SupportShift(-) → Protest → Secession
```

Each system that reacts to a change passes the triggering event id on as a cause.

## 6. Agents (core/agents)

Characters (residents and magical girls) with voxel bodies (6 parts, 1/8-cube voxels,
severable limbs, regrowth that costs food), needs, personality (8 traits), skills,
relationships, support for each magical girl, inventory, memories (linked to chronicle
events) and a decision trace (the scored options behind the current activity).

* **Utility AI with hysteresis** (`agents_ai.cpp`): eat, drink, sleep, talk, work,
  govern, flee, protest, steal, cast, escape, serve as a soldier, get wounds dressed,
  go over to another polity, wander. The current activity gets a
  commitment bonus; the top options are kept as the "why" shown in the UI.
* **Tasks** are small state machines (`agents_tasks.cpp`); every resource target is
  reserved with an expiry; unreachable targets are blacklisted for a while.
* **Jobs** (`agents_jobs.cpp`) appear where the world needs them: fields to till, sow
  and harvest; piles to store; construction/repair sites to supply and build; dig
  projects; kitchens; crafting on demand (recipes in `data/recipes.json`, produced at a
  public store and logged in the ledger); foraging under scarcity.
* **Navigation**: A* over standable cube positions (8-way, climb 1–2 with headroom,
  drop 3). A flood fill labels **walkable regions** around settlement anchors; the
  region map is rebuilt when event-driven terrain changes happen (or daily after
  settling matter) and is saved, so water search skips unreachable spots and saves stay
  deterministic. Someone outside every region (fallen into the ravine) plans and cuts a
  45° staircase out of the rock (`agents_escape.cpp`).
* **Magic on her own initiative** (`agents_magic.cpp`): healing the injured nearby,
  quenching fires. Strategic spells go through decisions (below).
* **Migration** (`agents_migrate.cpp`): grievance (resentment of the ruler, hunger, low
  mood) times the pull of another polity (better fed, a favoured ruler, friends there;
  less so an enemy), damped by conformity and caution. Migrants walk to the other seat
  with what they carry; the move is a `Migration` event caused by their latest bitter
  memory.
* **Medicine and carts** (`agents_medicine.cpp`): with herbalism, gatherers bring in
  herbs when the stock is short; the wounded fetch a bundle and have their wounds
  dressed (at a 药庐 if there is one): bleeding stops and regrowth runs faster, and the
  herbs are used up. Carts made in a workshop raise what a hauler carries; equipment is
  picked up when unloading at a store.

## 7. Economy (core/economy)

Material ledger: every item unit lives in exactly one store (stockpile, workshop, ground
pile, character inventory, construction site, home). Sources and sinks (harvest,
mining, crafting, cooking, eating, spoilage, construction, spells) are explicit and
counted by reason, so conservation is tested. Buildings are real cubes placed from
blueprints; integrity is derived from the world, so damage degrades function.
Bridges follow a span rule and fail as real debris. Farms are plots of farmland whose
growth depends on irrigation from real water nearby.

## 8. Society, politics, magic and decisions

* **Polity** (`society/`): identity and display name are separate from the ruler, so the
  title "X的文明，国名" follows the ruler while people, industry and history remain.
  Hourly statistics, support drift, crises (food, water/irrigation, logistics, unrest)
  with root causes from the chronicle, projects. Passive spells of the polity's girls
  are summed per effect and shape daily life (mood floor, meal joy, preservation...).
* **Jev pipeline** (`decision/`):
  1. *What she knows*: a situation text built from stats, crises with cause chains,
     the other girls and her past decisions.
  2. *Options*: programs build feasible options with computed facts and a value
     profile over 12 features (food security, welfare, order, harshness, cooperation,
     self-power, growth, frugality, military, risk, fairness, speed); infeasible ones
     carry a reason. Other girls add **proposals** (their own pick).
  3. *Provider*: the **local persona model** (drive values × personality, experience of
     past outcomes, loyalty/affinity bias, stateless noise), a **remote LLM** answered by
     the host (structured output restricted to feasible keys; validation, staleness
     check, deadline, daily budget, fallback to local), or a **replay** log (answers
     applied at the original tick).
  4. *Executor*: policies, repair/build/dig projects, farm expansion/founding,
     requisition, punishment, spells (real matter: feast from grain, growth draws water),
     petitions, withdrawal, secession (new polity, farm split, goods hauled away),
     coups.
  5. *Reaction and review*: residents judge the decision and the alternatives by their
     own personalities (support shifts); advisers whose proposal was ignored lose
     loyalty; outcomes are reviewed later and remembered as experience.
* **War** (`society/war.cpp`, `agents/agents_war.cpp`, `decision/decision_war.cpp`):
  wars are declared by rulers; armies are drafted able-bodied residents who arm
  themselves from the stores and follow one operation at a time (muster, march, raid or
  take the objective, return). Hits remove voxels from bodies, reduced by armour;
  raiders haul real food home; conquest annexes people, land and stores.
* **Trade** (`society/trade.cpp`, the `Trade` job in `agents_tasks.cpp`): each polity's
  *trade book* lists what it can spare (food beyond five days of eating, building
  materials beyond what construction still needs, tools and arms beyond one for everyone
  who would use them) and what it lacks. A ruler at peace may propose a pact when the
  books match; the other ruler accepts or refuses by her own values (a hungry polity is
  pulled towards it, uncooperative drives shun it). Under a pact, carriers take a load
  from their store to the partner's store and bring back goods of equal value — units
  move between stores, so the ledger holds. A caravan that cannot get there turns back;
  the cut trade road is an event caused by the latest destruction. War ends the pact;
  each day's exchanges are summed up in the chronicle; dealing slowly warms relations.
  A hungry neighbour cannot pay, so a ruler with food to spare may instead **send aid**:
  the same carriers take it over and ask nothing back (warm-hearted drives are drawn to
  it; others would rather raid the full granary or let the neighbour starve).
* **Miracles** (`sim/simulation.cpp`, `apply_admin`): besides matter (dig, place,
  meteor, fire, water) the god can bless food (items enter the ledger as
  `admin_bless`), inspire or terrify (fear, memories, a hazard people flee), heal (lost
  limbs regrow; the dead stay dead), smite, call rain (hours of it: water surfaces fill,
  fires under the open sky go out and hardly spread), grant research, empower a girl, or whisper
  to her: a value whisper shifts one feature weight for two days, is told to the remote
  model, and is recorded as a cause of the decisions it pushed.
* **Outcomes**: a round is won when one polity holds the whole island after there had
  been several (`EventType::Unification`, caused by the last annexation). The measures
  it is judged by — population, living standard, knowledge, ecology (share of the
  generator's trees still standing; `World::forest()` looks only at modified cells) and
  stability — are shown with it.
* The Godot autoload `Jev` sends awaiting decisions to the Claude Messages API (raw
  HTTP; `fallbacks: "default"`), and `tools/jev_mock_server.py` stands in for the API in
  tests.

## 9. Presentation (game/)

Godot renders cell meshes from the kernel, characters as voxel parts, debris and
meteors. The UI is built in code (`ui/ui_theme.gd`) as floating cards over the world —
time pill, civilisation card with polity switcher, tool dock, toasts, contextual
selection card with tabs — plus centred overlays: **议事录** (decisions), **编年史**
(history with a causal-chain graph), **科技** (the tech tree) and the round's ending
card. No permanent side panels.

## 10. Validation

`icarus_cli experiment` runs the same scenario and shocks on many seeds and reports how
each civilisation responded and ended (recovered / declined / split / coup / war /
reunified), with the forest left standing. Reports
live in `docs/experiments/`.

## 11. Determinism

* All randomness from `Rng` streams owned by systems (saved), or pure hashes.
* No iteration over unordered containers in simulation logic.
* `-ffp-contract=off`.
* Tests assert: same seed ⇒ same state hash; save→load→continue ⇒ same hash as an
  uninterrupted run; replaying a decision log reproduces a run exactly. Remote Jev
  responses are not reproducible by nature: they are recorded and replayed instead.
* Decision noise is a pure hash of (decision, girl, option), so local, remote and
  replayed decisions consume no shared random stream.
