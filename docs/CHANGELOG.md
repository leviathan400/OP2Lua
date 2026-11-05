# Changelog

All notable changes to OP2Lua (the engine: `OP2LuaCore.dll`, the mission stub, the editor and tools).
For the author-facing SDK, see the OP2LuaSDK changelog.

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
