# Changelog

All notable changes to OP2Lua (the engine: `OP2LuaCore.dll`, the mission stub, the editor and tools).
For the author-facing SDK, see the OP2LuaSDK changelog.

## [0.8.14] - 2026-11-26

### Added

- `unit:launch_at_pixel(px, py, force)`: with `force` true the Spaceport launches although it is EMPed, the game's
  own forced launch, as a stock mission captures a rocket.
- The type names `SULV` and `RLV`, the Spaceport's two rockets: for a count trigger's `type` and what
  `unit.object_on_pad` names.
- `player.rlv_count` and `player:capture_rlv(from)`: the RLVs a player has, and taking one from another player, the
  game's own calls.

### Changed

- `unit:launch_at_pixel` warns when the game faults on the launch instead of saying nothing.

### Fixed

- `trigger.count{type = "RLV", ...}` counted nothing and never fired: the name wasn't known, so the trigger counted
  no type at all, without a warning.
- `unit:set_factory_cargo` on a Spaceport now makes the game's own call, which loads the launch pad too: a rocket
  set so (`set_factory_cargo(1, "RLV")`) was in the bay but not on the pad, so there was nothing to launch.

Saves made with 0.8.13 load and carry on.

## [0.8.13] - 2026-11-26

### Added

- `player.morale` and `player.morale_level`, read and set: the colony's morale (0-99) and its level.
- `player:morale_state()` / `player:set_morale_state(t)`: the player's whole morale record, the block the game itself
  carries from one campaign mission to the next, so a campaign can save it with `campaign.save` and give it back.
- `player:techs()`: the topics the player has researched, to carry over and give back with `mark_research`.
- OP2LuaEditor (both editors): Mission Properties sets every value the game reads from the mission DLL, adding the
  tech tree file, a unit-only mission and a multiplayer game's AI players. The placement's header gains `unit_only`
  and `ai_players` for them.
- `op2lua-newmission`: `--tech`, `--max-tech`, `--unit-only` and `--ai-players`; `--type` also takes `Tutorial`,
  `AutoDemo`, a campaign mission's number and the placement's `Multi...` names. `--dll-from <Base>.placement.lua`
  builds an existing mission's DLL from its header.

### Changed

- OP2LuaEditor's Save Mission (both editors) also writes the mission DLL: a copy of the stub with every value from
  the placement's header written at its export (name, map, tech tree, type, players, max tech level, unit-only, AI
  players). A key the header lacks is taken from the DLL already there; the old DLL is kept as `.bak`, and a DLL
  that isn't an OP2Lua stub is left alone. Save As brings the mission's `<Base>.lua` along.
- `op2lua-newmission` writes the same bytes as the editors, and the placement it writes carries the tech tree and
  max tech level it was given.

### Fixed

- A mission DLL could disagree with its placement and nothing put it right: the editors patched only the fields
  that changed, by searching the DLL for their old text, and `op2lua-newmission` always left the max tech level at
  12 and the tech tree at `MULTITEK.TXT`. A DLL with the stub's max tech level 12 lets the player research past the
  mission's limit from the start. Saving the mission in either editor, or `op2lua-newmission --dll-from`, now
  rewrites it from the placement.

Saves made with 0.8.12 load and carry on.

## [0.8.12] - 2026-11-25

### Changed

- After a load that stops the mission's script, the second `GAME LOAD MISSION FAIL` comes 75 ticks after the first
  (was 100) and plays the sound of an incoming chat line, so the player notices it.

### Fixed

- A game saved again after a load that stopped its script, and loaded later, also reads "Objective unknown" for its
  objectives. Their texts pointed at the previous game's copy of OP2Lua, which may sit elsewhere after a reboot.

### Docs

- API.md: what a `player` argument takes (a Player or 1-7; anything else warns and does nothing), how triggers take
  `player`, and what the player sees after a failed load.

Saves made with 0.8.11 load and carry on.

## [0.8.11] - 2026-11-25

### Changed

- A load that stops the mission's script says so more clearly: the message pane shows `GAME LOAD MISSION FAIL`,
  then the line saying why (e.g. "OP2Lua: saved with another version of this mission: its script is stopped."),
  on the first tick and again a mark (100 ticks) later. A save made with no script running isn't a failure and
  shows neither.

### Fixed

- After a load that stops the script, the mission's objectives read "Objective unknown: the mission's script is
  stopped". Their texts belonged to the script that saved, and the game still pointed at them, so reading them
  (the Mission Objectives list) could show garbage or crash the game.

Saves made with 0.8.10 load and carry on.

## [0.8.10] - 2026-11-25

### Fixed

- Calls that take a player check it is one (1-7 or a Player) before it reaches the game's player table (PR #4, by
  bufke). Given a number past the seven players, or Lua's -1, `player:ally_with`, `mutually_ally_with` and
  `is_ally`, `unit:transfer` and `do_transfer`, `block:create_units`, the `ai.*_group` calls and the triggers that
  take a `player` could crash the game or change memory past the table. They now warn in the Mission log (the `ai`
  calls in the AI log) and do nothing: `create_units` returns 0, no group or trigger is made, and `is_ally` returns
  false. A trigger with no `player`, or given 0 or a non-number, still counts every player.
- `trigger.kit` needs a player: made for every player, it crashed the game at its first check. It now warns
  (`trigger.kit: needs a player`) and makes no trigger, as `trigger.operational` does.

Saves made with 0.8.9 load and carry on.

## [0.8.9] - 2026-11-23

### Changed

- The in-game messages about saved games are short enough for the message pane (it cut them off after about 90
  characters), e.g. "OP2Lua: saved with another version of this mission: its script is stopped." The Mission log
  keeps the whole reason. Saves made with 0.8.8 load and carry on.
- FILE-FORMAT.md, "Saved games": which OP2Lua updates keep saves loading, and the saved state's header as something
  tools may read (to tell before loading whether a save will carry on with the installed mission).

## [0.8.8] - 2026-11-22

### Added

- Saved games keep the mission's script: its variables and tables, its pending `after`/`every`/`when` timers, its
  triggers' `on` functions and the game handles it holds. A loaded game carries on where it was saved, without
  running the script's start-up code again, and calls `on_load_save()` on its first tick. A save resumes only with the
  same `.lua` file; a value that can't be saved, a changed script, or a save made before 0.8.8 loads with the script
  stopped and a message saying why. FILE-FORMAT.md, "Saved games", lists what a script must and mustn't do.

### Changed

- The script's top level runs when a new game starts (InitProc) and `on_load()` right after it, before `on_init()`.
  `on_save()` is called just before the game writes a save; its return value is no longer used.
- Loading a saved game, or starting another mission, lets go of the previous mission's script: its timers and
  triggers' functions no longer run on in the next game.

## [0.8.7] - 2026-11-22

### Added

- `unit.kit_weapon`: a ConVec kit's weapon (a Guard Post kit's turret, the name `set_kit` takes as its topping), or
  `nil`. On a ConVec `unit.weapon` reads the kit itself, so the turret couldn't be read before.
- A limit for runaway Lua: a call into the script (a callback, a trigger's function, each `after`/`every`/`when`
  function) that runs more than 25 million Lua instructions, an endless loop, is stopped with an error naming the
  line, logged in the Mission log and the Main log, and dropped; the game carries on. A dropped `every` or `when` is
  cancelled. `pcall` can't catch it and carry on.

## [0.8.6] - 2026-11-21

### Changed

- A `move`, `attack_move` or `attack` to a unit made this tick goes out at once, as a mission DLL's `CreateUnit`
  then `DoMove` does. Through 0.8.5 such an order was held about 25 ticks, so a mission's reinforcements set off
  late. An order a unit can't take is still queued and retried each tick, up to 20 times.

## [0.8.5] - 2026-11-21

### Fixed

- `game.morale_steady()` and `game.free_morale()` with no player (every player) did nothing in 0.8.4.

## [0.8.4] - 2026-11-20

### Added

- OP2LuaEditor rewritten in Rust (`op2lua-editor-rust/`, GPL-3.0-or-later) for Windows, Linux and macOS: everything
  the VB.NET editor does, drawing the map from GPU textures, with a Select tool, side-panel editing and a save prompt.
  It reads and rewrites `placement.lua` with op2-formats' `placement` module (0.5). Version 0.8.4, by Outpost Universe.
- Both editors draw the two tube tiles the game lays beside each structure with tube connections (east of it and south
  of it), and the dozed ring around every structure, power plants and mines included.

### Changed

- A fault inside a game call a script makes no longer closes the game: it comes back as a Lua error with the script's
  file and line, is logged, and the mission carries on. The crash reporter no longer logs a crash for faults that are
  caught.
- Functions check their arguments before calling the game and refuse a bad one with a warning: player numbers
  (`create_unit`, morale, EMP missiles), tiles off the map (`set_tile`, `initial_set_tile`, `set_cell_type`), wall
  types (`create_wall`), and `transfer_cargo`'s bay, factory and docked vehicle.

## [0.8.3] - 2026-11-19

### Added

- `campaign.save(name, tbl)` / `campaign.load(name)` / `campaign.delete(name)`: plain data a campaign mission saves
  for the next one, in a `.carry` file beside the mission DLL (data only, never run as code).

### Changed

- `unit:factory_cargo(bay)` also returns the bay's weapon or cargo, and works on a Spaceport's bays.

## [0.8.2] - 2026-11-19

### Added

- `OPU\logs\OP2Lua-<dll>-result.json`: how the mission ended (won, lost, aborted, victory, defeat, terminated), for
  launchers; `on_end(result)` gets the same when the game reports the end while the mission runs.

### OP2LuaEditor 0.8.2 (build 0027)

- The version only.

## [0.8.1] - 2026-11-18

### Added

- `game.units_closest`, `game.force_robot_command`, `game.clear_cheated`, `player:set_launched`.
- `game.play_wav(file)`: a WAV shipped with the mission, at the game's sound volume; `game.message(text, false)`: a
  message with no sound.

### Changed

- `move` / `attack` / `attack_move` go out at once; only a unit made this tick or the last has its order held
  until it can take it. A move to a tile off the map's grid is ignored, as the game does.

### Fixed

- Area arguments (`build_wall`, `doze`, `units_in_rect`, `rand_point`, `set_rect`, ...) take a `{ x =, y = }` point
  and a raw tile; a named point became tile 1,1.
- `markers[name]` is in status-bar coordinates, as documented (it was the game's internal tile).
- `math.random`, `math.randomseed`, `dofile`, `loadfile` and `load` are removed from the sandbox, as documented.
- An empty weapon, kit, pad or factory bay reads `"None"`; `set_factory_cargo` gives a Guard Post kit its turret.
- `op2lua-newmission` and OP2LuaEditor's New Mission name a Colony mission's DLL `c<Base>.dll`; a multiplayer
  `--type` gets the lobby skeleton.
- docs: API.md checked against the code (defaults, returns, every callback, a *Names* section); other docs updated.

## [0.8.0] - 2026-11-18

### Added

- `game.morale_steady(level, player)` locks one player's morale; `unit_stat`, `set_unit_stat` and `set_required_tech`
  take a raw MapID number as well as a name.
- `game.set_music(songs, repeat_from)`, `game.player(n)` for any slot 1-7, `player:unit_stat` / `set_unit_stat` (per-player unit stats),
  `game.set_required_tech`, `game.set_tech_cost`, `player:unmark_research`, `unit.rotation`, `unit.is_stickyfoamed`, `unit.dirt_prevention`, `unit.double_fire_rate`, `unit.eden`:
  what a mission that changes the rules as it goes needs.
- `player.edward_satellites` and `player.solar_satellites`, read and set, and placement's `edward_satellites`:
  with an EDWARD Satellite every beacon counts as surveyed for the player.
- `hackaray.*`, opt-in: a mission that calls `hackaray.load()` can switch on Hackaray.dll's nuke patch and set
  the end-of-game videos; without that line OP2Lua never looks for the DLL.
- `unit.lights`: whether a vehicle's headlights are on.
- `unit:set_offensive(on)`: whether a unit acts as a combat unit.
- `unit:set_emp(ticks)`: EMP a unit for a time (`-1`: as long as the game can).

### Changed

- OP2LuaEditor 0.8.0 (build 0025): the version only.

## [0.7.9] - 2026-11-17

### Fixed

- Placement takes `at = { x =, y = }` and the game's own tile, `{ x =, y =, raw = true }`, as script calls do.
- `group:first_of_type(type)` without a weapon found nothing, so a converted computer player's Structure Factory
  got no kits (Peacekeeper, Eden 11).
- OP2LuaEditor 0.7.9 (build 0025): Mission Properties takes a campaign mission's type (`"1"` to `"12"`); a sound
  when a map has loaded.
- docs/FILE-FORMAT.md: the mission file reference.

## [0.7.6] - 2026-11-17

### Added

- `game.version()`: the running OP2LuaCore's version and DLL build time; `game.op2_version()`: the version the running
  Outpost 2 goes by (`TApp::Version`, `"1.4.0.1"` under OPU 1.4.x) and Outpost2.exe's file version.
- OP2LuaEditor 0.7.6: the open mission's `.lua` in the status bar.

### Fixed

- A Guard Post armed with `"Starflare"` or `"Supernova"` no longer crashes the game when it is drawn: it gets the Guard
  Post's turret version of the weapon (StarflareGP / SupernovaGP), as the stock missions make it.

## [0.7.5] - 2026-11-17

### Added

- **Multiplayer missions:** `op2lua-newmission --type` (Land Rush, Space Race, Resource Race, Midas, Last One
  Standing), the lobby's settings, the game's own triggers and victory conditions, `players[]` for every player.
- **Bindings for the stock missions' setups:** the game's AI groups (`ai.*`), time and research triggers, map edits
  (`game.tile`/`set_tile`, `initial_set_tile`, `set_cell_type`), campaign triggers and unit calls, placement beacons
  with `after = n`, raw locations, `player.ore_capacity` / `rare_ore_capacity`, and read-only AI queries.
- **OP2LuaEditor 0.7.5:** a lossless placement save, the mission DLL's type and player count, beacons, walls and
  markers on the map, dozed borders, Test in OP2 (F5) through op2launcher, the selected tile, and the game's
  OP2LuaCore.dll version in the status bar.

### Fixed

- `region:center()` in author coordinates; `unit.progress` nil when nothing is made; `game.footprint` /
  `find_build_site` nil for a non-structure; `player:default_group()` as the game exports it; `game.rand(0)` draws
  as the game does; points off the map aren't wrapped onto it.

## [0.7.0] - 2026-11-05

### Changed

- Released under the **MIT License** (`LICENSE`). Vendored libraries keep their own licenses
  (Tethys BSD 3-Clause, Lua MIT, sol2 MIT).
- The engine source (`src/`) and the `op2lua-newmission` scaffolder are now published in this repo.
- Version set to **0.7.0** across the core, the editor and the SDK.

## [0.6.12] - 2026-06-22

### Changed

- Updated the vendored TethysAPI (`fix-unit-commands` branch, `8078c14`). OP2LuaCore compiled with no
  source changes. The vendored copy is again a clean mirror of upstream - no local Tethys edits remain.

## [0.6.11] - 2026-06-22

### Fixed

- `player:has_tech`, `player:mark_research` and `unit:research` no longer crash on an invalid tech id.
  They now validate the id, log an error and show an in-game message, then do nothing (`has_tech`
  returns false).

## [0.6.10] - 2026-06-19

### Changed

- Updated the vendored TethysAPI to the upstream `fix-unit-commands` branch, which includes the
  unit-command fixes OP2Lua had been carrying locally (`DoMove`, `DoDock`, `DoMiningRoute`,
  `DoDumpCargo`, `DoTransferCargo`). Tethys `GFX*` headers moved to a new `GFX/` folder.

## [0.6.9] - 2026-06-19

### Added

- Placement: a ConVec entry can preload a structure kit with `kit = "<Structure>"` (and optional
  `kit_topping`), the ConVec counterpart of a Cargo Truck's `cargo`.

## [0.6.8] - 2026-06-17

### Fixed

- `unit:dump_cargo()` and `factory:transfer_cargo(bay)` no longer crash the game (Tethys
  `DoDumpCargo` / `DoTransferCargo` now issue proper command packets).

## [0.6.7] - 2026-06-10

### Added

- `log.ai_status(player)` - writes a live JSON snapshot of a player's resources and unit counts to
  `logs/OP2Lua-<mission>-AI-status.json`, overwritten on each call, for external monitoring tools.

## [0.6.6] - 2026-06-10

### Added

- Logging split into three files: the main `OP2Lua.log`, the per-mission `OP2Lua-<mission>.log`, and
  an AI log `OP2Lua-<mission>-AI.log` written via `log.ai(...)`. Also `log.mission(...)` and
  `log.show_ai_in_game(on)`.

### Changed

- Every log line carries the game mark. Framework lines are no longer duplicated into the mission log;
  a crash writes a one-line summary to the main log and the full dump to the mission log.

## [0.6.5] - 2026-06-10

### Added

- `game.disasters_enabled()` - whether disasters are enabled in the game setup.

## [0.6.4] - 2026-06-10

### Added

- `player:satellite_count(type)` and the starship / Spaceport type names, enabling a scripted Starship
  colony victory.

## [0.6.3] - 2026-06-10

### Added

- `player.difficulty` - `"Easy"`, `"Normal"` or `"Hard"`, so one mission can tune itself per difficulty.

## [0.6.2] - 2026-06-09

### Added

- `game.set_lava_possible(area [, on])` - mark tiles lava-possible so eruptions can flow across them.

## [0.6.1] - 2026-06-08

### Fixed

- Placement creates beacons before units, so a mine placed on a beacon no longer crashes on load.

## [0.6.0] - 2026-06-07

First shared release of the SDK.

### Added

- The log records the `.map` the mission DLL actually loads, and warns when `placement.lua`'s `map=`
  disagrees with it (the DLL wins).

## [0.5.x] - 2026-06-05 to 2026-06-07

### Added

- In-game (status-bar) coordinates for placement and scripts.
- Large author API expansion: unit health, orders and engineering; player resources, research and
  diplomacy; walls, tubes, markers and every disaster type; cargo and kits; perception (`game.cell`,
  `game.map_size`, `game.beacons`, `unit.x` / `unit.y`); build-site helpers (`game.find_build_site`,
  `game.footprint`); dock queries.
- Base-building orders (survey, deploy, build, mining routes, factory production, research), enough
  for a scripted economy AI.
- `marks(n)` scheduling in game marks; `game.seconds` / `game.minutes` deprecated.
- `op2lua-newmission`, a cross-platform mission scaffolder.
- Version resources on `OP2LuaCore.dll` and the mission stub.
- Colony `c` prefix on mission DLLs (`cStrikeTeam.dll` loads `StrikeTeam.lua`).

### Fixed

- Several Tethys command-packet bugs (`DoMove`, `DoMiningRoute`, `DoDock`) that crashed or silently
  dropped orders.
- Timers registered from inside a firing callback are no longer discarded.
- `attack_move` no longer attacks the ground; commanding freshly spawned units no longer crashes.
- Build-site search no longer crashes off-map or overlaps existing structures.

## [0.4.0] - 2026-06-05

First playable release: the two-DLL design (`OP2LuaCore.dll` + per-mission `LuaMission.dll` stub),
`placement.lua` loader, scheduler (`after` / `every` / `when` / `at_mark`), core unit / player /
region / game API, protected callbacks, crash reporting, and multiplayer-safe-by-construction
scripting.
