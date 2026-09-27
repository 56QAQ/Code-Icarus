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
| world | 32×8×32 cells = 1024×256×1024 cubes | classic: main island r≈112 + 4 islets; continent: r≈330 with biomes |
| body voxel | 1/8 cube | characters are ~3 cubes (1.5 m) tall |
| tick | 1/20 s at 1x | 6000 ticks per day (5 min at 1x) |
| season / year | 8 days / 4 seasons | a life runs at 0.75 years per day (`data/life.json`) |

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
* **Layouts**: `Classic` (the first version's island) and `Continent`
  (`worldgen_continent.cpp`): an irregular coast, a mountain range, lakes, rivers from
  springs, ravines and islets; biomes from temperature × moisture × height (grassland,
  broadleaf and conifer forest, snowy highland, red desert, marsh, lakeshore, rock);
  wild resources (fruit trees, wild grain, mushrooms, reeds, herbs, flint nodules, peat,
  ores) and a *site* per civilisation (a plateau by a lake, in different biomes).

## 4. Matter (core/sim/physics)

* **Water**: integer units (15 per cube), conserved. Falls, then spreads to lower
  neighbours, prefers edges with drops (waterfalls). Leaves the world below y=0 (the
  abyss under the floating island). Evaporation only takes water standing above the
  generated water table (floods, puddles); lakes keep to their shores, rain tops them
  up. Springs are explicit
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

**Fauna** (`fauna/fauna.cpp`, `data/animals.json`): deterministic, saved animals by
biome (rabbits, deer, boar, goats, wolves, bears, fish, birds) that graze, flock, flee,
hunt, breed within a cap and grow old. Far from people they move in large steps (LOD).
Hunters stalk and strike them; carcasses are butchered into meat, hide and bone in the
ledger. Wolves only prey on lone people at night when starving and far from buildings;
bears and boars strike once at whoever comes too close or struck them, then break off.

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
* **Work choice**: each resident takes the best-scoring open job (priority × the
  polity's category weight × motivation × skill − distance). Food work weighs up to
  1.8× more as the larder empties; while people are fed, construction (building,
  carrying to sites) and gathering (wood, stone) keep about 15% of the hands; nobody
  sets out for far work late in the day.
* **Tasks** are small state machines (`agents_tasks.cpp`); every resource target is
  reserved with an expiry; targets out of reach are blacklisted for a while (other
  failures only rest the job, so a busy store is never shunned).
* **Jobs** (`agents_jobs.cpp`) appear where the world needs them: fields to till, sow
  and harvest; piles to store; construction/repair sites to supply and build; dig
  projects; kitchens; crafting on demand (recipes in `data/recipes.json`, produced at a
  public store and logged in the ledger); foraging under scarcity. Materials go to the
  site begun first, crafting leaves alone what sites wait for, and builders lay roofs
  from a ladder (up to seven cubes up and two to the side). Rulers begin no new
  building while two sites stand unfinished, nor one whose materials cannot be made
  yet. Sites are on dry, flat ground, two cubes clear of other buildings (a campfire's
  ring included), with the doorstep and two cubes beyond it open.
* **Navigation**: A* over standable cube positions (8-way, climb 1–2 with headroom,
  drop 3). A flood fill labels **walkable regions** around settlement anchors (a flat
  open-addressing table, `region_map.h`); the survey is redone after building work at
  most every six hours (daily after settling matter), not when a new anchor lies in
  ground already surveyed. It is built a few thousand positions per tick into a
  staging table and swapped in when done; the table and the staging state are saved,
  so water search skips unreachable spots and saves stay deterministic. Regions flag whether their flood was cut short (`open`):
  another survey region is out of reach only when both floods ran their course. Long
  searches share a per-tick budget of expanded nodes; beyond it they wait a tick. Someone outside every region (fallen into the ravine) plans and cuts a
  45° staircase out of the rock (`agents_escape.cpp`).
* **Magic on her own initiative** (`agents_magic.cpp`): healing the injured nearby,
  quenching fires, battle magic (enemy magical girls first: a told **duel**), and the
  rituals — a war cry (the fighters around her fearless and stronger for two hours),
  frenzy, discord (in the enemy's ranks, or at home against the ruler), withering the
  enemy's crops, devouring their walls for mana. Long rituals keep cooldowns. Strategic
  spells go through decisions (below). Every cast enters a presentation-only **spell
  feed** (effect, caster, target, colours) that Godot turns into visible magic.
* **Life** (`agents_life.cpp`, `data/life.json`): ages, partnerships between fond
  housemates, births when fed, housed and content, children who play and grow up to
  work, elders who slow down and die of old age, mourning. A people with too few
  magical girls sees one awaken among its women (her drive answers what the people have
  been through), taken in hand by the most experienced girl; a people that loses its
  last girl sees one awaken at once.
* **Drama** (`agents_drama.cpp`): ties between girls (`Bond`: friend, rival, mentor /
  student, nemesis) grow from shared drives, campaigns, a girl turned against her
  ruler, a newly awakened student, a friend felled by an enemy girl. What a girl lives
  through adds to her *trauma* (a friend lost, famine, her people cut down, an army
  broken, a people forced to bow, punishment) or her *solace* (kindness, levels,
  triumphs, friendship); past a threshold a bright drive falls (hope→despair,
  courage→wrath, light→envy, gourmet→gluttony) or a dark one rises, recolouring her
  costume, with the experiences behind it as the event's causes.
* **Equipment** (`agents_equipment.cpp`): typed tools (axe, pick, hoe, hammer, sickle,
  knife) in flint/stone, copper and iron with wear; the right tool speeds the work (bare
  hands are slow), workers swap tools at stores; clothes (leaf wrap, fur cloak, linen)
  keep off cold and wet; weapons and armour for hunters and soldiers.
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
* **Technology** (`data/techs.json`): knowledge comes from research at the seat (or the
  campfire) and from practice in everyday work. A tech of a new era can be studied or
  stumbled on only once half of the previous era's techs are known, so a wild band
  climbs through the eras rather than leaping ahead.
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
  besiege the objective, return), with up to two magical girls with battle magic as
  champions. Hits remove voxels from bodies, reduced by armour; the badly wounded fall
  back and an army breaks at a third lost; raiders haul real food home. A hall held
  for hours without a defender falls: a people that can still stand capitulates as a
  tribute-paying vassal, a broken one is annexed.
* **Strategy and diplomacy** (`society/strategy.cpp`): fighting strength, and an
  *assessment* of each neighbour (motive: grievance, hostility, hunger against their
  plenty, the ruler's drive — war appetite, envy of the stronger, gluttony for a full
  granary; opportunity: their famine, unrest, other wars; the strength ratio with
  allies; the first season of building up; truces). Grievances pile up from trespass
  and borders within reach, fade slowly, and harden attitudes. Rulers decide: raids,
  conquest, demands for submission, alliances against a stronger third, envoys with
  real gifts, peace with reparations or vassalage; wars that drag on, a starving or
  bled people, weigh toward peace. Vassals pay tribute every few days and may throw off
  the yoke. Growing villages found new ones by distant water (outposts).
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
meteors.

* **Cubes** are textured from a texture array painted at start-up from the rules'
  colours (`scripts/texture_forge.gd`): every material gets 16x16 pixel-art faces (top,
  side, bottom where they differ), natural ones in three variants that each cube picks by
  a hash and turns or mirrors. The mesher writes material, face kind and damage into the
  vertex UV; the shared shader include (`shaders/voxel_common.gdshaderinc`) looks up the
  layer, draws cracks on damaged cubes, lets meadows drift between lush and dry, makes
  ores and glass glint, levistone and magma glow, windows light up at night and surfaces
  darken when wet. Grass tufts, flowers and wheat are alpha-cut sprites.
* **Water** surfaces share heights at cube corners (smooth lakes); the shader colours by
  the thickness of water in view, refracts the bed, foams at shores and streaks where
  water falls.
* **Figures** are interpolated between simulation snapshots; the walk cycle is driven by
  distance covered, each job has its motion and tool, magical girls carry twin tails, a
  flared skirt and a floating emblem.
* **Effects** (`scripts/fx_renderer.gd`) read the kernel's presentation-only feed of
  broken and landed cubes (chips, dust), its fire list (flames, smoke) and the weather:
  rain falls as streaks that splash where they land, rings water surfaces with ripples
  and gathers in mirror-dark puddles; the sky, clouds and light follow the hour.
* **Spells** (`scripts/spell_renderer.gd`) turn the spell feed into short effects in
  the caster's colours: the spell's name over her, light streaming, bolts and fireballs
  in flight, slashes, rings over the ground, pillars of light, embers and motes.
* **Villages** (`scripts/village_renderer.gd`): goods on the ground and in storehouse
  yards are drawn by kind (logs, sacks, baskets, ore, jars, tool racks...), and the
  things of daily life stand around each building (woodpiles, jars, barrels, hearth and
  pot, workbench, banners and braziers at the hall). Presentation only. The UI is built in code (`ui/ui_theme.gd`) as floating cards over the world —
time pill, civilisation card with polity switcher, tool dock, toasts, contextual
selection card with tabs (a girl's card adds her ties, drive history and **传记**, her
life's great moments) — plus centred overlays: **议事录** (decisions), **编年史**
(history with a causal-chain graph), **科技** (the tech tree), **羁绊** (the girls as a
web: peoples as clusters, followings as circle size, ties as lines) and the round's
ending card. For a few hours after a council decision the girls' name tags say what
each argued for. No permanent side panels.

## 10. Validation

`icarus_cli experiment` runs the same scenario and shocks on many seeds and reports how
each civilisation responded and ended (recovered / declined / split / coup / war /
reunified), with the forest left standing. Reports
live in `docs/experiments/`. `icarus_cli run --layout continent --scenario three_realms`
(or `--scenario wild`) with `--events -v` streams a whole year; `--load` continues from
a save, and slow ticks are reported with the path searching and flooding they did. The
second version's acceptance runs are summarised in `docs/V2_PLAN.md`.

## 11. Determinism

* All randomness from `Rng` streams owned by systems (saved), or pure hashes.
* No iteration over unordered containers in simulation logic.
* `-ffp-contract=off`.
* Tests assert: same seed ⇒ same state hash; save→load→continue ⇒ same hash as an
  uninterrupted run; replaying a decision log reproduces a run exactly. Remote Jev
  responses are not reproducible by nature: they are recorded and replayed instead.
* Decision noise is a pure hash of (decision, girl, option), so local, remote and
  replayed decisions consume no shared random stream.
