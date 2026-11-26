# OP2Lua Mission File Format

The files an OP2Lua mission is made of, what OP2LuaCore reads from each, and in what order. This is the reference
for `<Mission>.placement.lua`; the functions a mission script calls are in [API.md](API.md). As of v0.8.14.

## The files

A mission is three files side by side in the game's `OPU\maps` folder (or a folder under it, such as `maps\LUA`),
plus the map it plays on:

| File | What it is | Who reads it |
|---|---|---|
| `<Mission>.dll` | A copy of the mission stub (`LuaMission.dll`) with the mission's name, map, type and player count patched in. The code is the same in every mission: it hands every call on to `OP2LuaCore.dll`. | Outpost 2 (the mission list, then the game) |
| `<Mission>.placement.lua` | Data: players, starting units, beacons, walls, regions and markers. | OP2LuaCore, when the mission starts |
| `<Mission>.lua` | Logic: the callbacks and scheduled events. | OP2LuaCore, when the mission starts |
| `<map>.map` | The map named in the DLL (a stock map, or one shipped with the mission). | Outpost 2 |

`OP2LuaCore.dll` itself sits in the `OPU` folder, shared by every mission.

`op2lua-newmission` and OP2LuaEditor's New Mission write all three files; OP2LuaEditor edits the placement and
the DLL's settings afterwards.

### How the scripts are found

The stub passes its own folder and file name (without `.dll`) to the core. For a DLL named `<base>.dll` the core
takes the first file that exists of:

| Placement | Script |
|---|---|
| `<base>.placement.lua` | `<base>.lua` |
| `<alt>.placement.lua` | `<alt>.lua` |
| `placement.lua` | `mission.lua` |

`<alt>` is `<base>` without a leading `c` that comes before a capital letter: Outpost 2 lists a DLL whose name
starts with `c` under colony games, and the scripts need no such prefix (`cStrikeTeam.dll` finds
`StrikeTeam.lua`). A name like `convoy` keeps its `c`.

With no placement file the mission runs script-only (the log notes it), and `players[]` still holds every player
in the game. A missing script, or one with an error, is reported in a message box and the log; the placement is still applied,
but no callback runs.

## The mission DLL

Outpost 2 reads these exports from the DLL before any Lua runs, so they, not the placement's header, decide what
the mission list shows and which map loads:

| Export | Type | Meaning |
|---|---|---|
| `LevelDesc` | `char[1024]` | The name in the mission list. |
| `MapName` | `char[1024]` | The `.map` file the game loads. |
| `TechtreeName` | `char[1024]` | The tech tree file (`MULTITEK.TXT` for the standard tree). |
| `DescBlock` | 4 ints | `missionType`, `numPlayers`, `maxTechLevel`, `unitMission`. |
| `DescBlockEx` | 8 ints | `numMultiplayerAIs`, the computer players a multiplayer game adds, then 7 reserved; all zero in the stub. |

`missionType` is the list the mission appears under:

| Value | Placement `type` | Listed as |
|---|---|---|
| 1 .. 12 | `"1"` .. `"12"` (as op2dll writes them) | Campaign mission 1 .. 12 |
| -1 | `Colony` | Colony game |
| -2 | `AutoDemo` | Demo |
| -3 | `Tutorial` | Tutorial |
| -4 | `MultiLandRush` (`LandRush`) | Multiplayer: Land Rush |
| -5 | `MultiSpaceRace` (`SpaceRace`) | Multiplayer: Space Race |
| -6 | `MultiResourceRace` (`ResourceRace`) | Multiplayer: Resource Race |
| -7 | `MultiMidas` (`Midas`) | Multiplayer: Midas |
| -8 | `MultiLastOneStanding` (`LastOneStanding`) | Multiplayer: Last One Standing |

`numPlayers` is 1 to 6. On a multiplayer type it is the number of human players and the lobby sets up each one;
on the other types it counts every player, computer players included. `maxTechLevel` is 12 for the whole tree.
`unitMission` 1 turns off most reports.

The stub also exports the usual mission entry points (`InitProc`, `AIProc`, `GetSaveRegions`, the `On*` hooks)
and a do-nothing `OP2LuaTrigger`, which the core's own triggers name.

The tools write each of these values at its export in a copy of the stub, so they never search the DLL for an old
value: OP2LuaEditor's Save Mission and Mission Properties build the DLL from the placement's header (below), and
`op2lua-newmission` takes them as options for a new mission (`--map`, `--tech`, `--type`, `--players`,
`--max-tech`, `--unit-only`, `--ai-players`; the SDK's older Linux build has `--map` only) or builds an existing
mission's DLL from its header (`--dll-from <Base>.placement.lua`). Both editors and the tool write the same bytes.

## Coordinates

A coordinate in a placement file is a **status-bar coordinate**: the numbers Outpost 2 shows for the tile
under the mouse, 1-based, the ones OP2MissionEditor and OP2LuaEditor show too. The core converts them to the
game's own grid (which has a hidden 32-tile margin on maps narrower than 512) when it applies the placement.

| Form | Meaning |
|---|---|
| `{ x, y }` or `{ x =, y = }` | A tile. |
| `{ x =, y =, raw = true }` | The game's own tile (0-based, with the hidden margin), used as it is: a stock mission's tile off the map, say. Not for regions. |
| `{ x1, y1, x2, y2 }` | A rectangle, both corners included (regions only). The corners may come in either order. |

The conversion of a status-bar tile wraps: a coordinate past the edge lands on the other side of the map (x by
the map's width mask, y by its height). A raw tile isn't converted, so it isn't wrapped either. (Script calls
don't wrap status-bar tiles; see API.md, Coordinates.)

## `<Mission>.placement.lua`

An ordinary Lua chunk that **returns one table**. Anything else (a syntax error, or no table returned) is
reported in a message box and the log, and the mission runs with no placement.

```lua
return {
  -- header: for the tools and the author (see below)
  name = "Hold the Line", map = "eden01.map", tech = "MULTITEK.TXT",
  type = "Colony", max_tech = 12, players_count = 2,

  players = {
    [1] = { colony = "Eden", human = true, color = "Blue",
            resources = { common_ore = 2000, food = 1000, kids = 10, workers = 14, scientists = 8 },
            center_view = { 9, 56 } },
    [2] = { colony = "Plymouth", human = false, color = "Red" },
  },

  units = {
    { name = "enemy_cc", type = "CommandCenter", player = 2, at = { 93, 39 } },
    { type = "Tiger", player = 2, at = { 95, 40 }, weapon = "Microwave", direction = "West" },
    { type = "CargoTruck", player = 1, at = { 11, 55 }, cargo = "Food", amount = 500 },
    { type = "ConVec", player = 1, at = { 12, 55 }, kit = "GuardPost", kit_topping = "Laser" },
  },

  beacons = {
    { type = "MiningBeacon", ore = "Common", yield = "Bar2", variant = "Random", at = { 88, 42 } },
  },

  walls   = { { type = "Tube", at = { 92, 41 } }, { type = "Wall", at = { 80, 30 } } },
  regions = { your_base = { 30, 50, 45, 62 }, spawn_point = { 93, 39 } },
  markers = { drop_zone = { type = "Circle", at = { 50, 50 } } },
}
```

Every section is optional. Keys the core doesn't know are ignored, so a file can carry notes for tools. Since the
file is Lua it may compute its tables (a loop laying out a row of beacons, say), but it runs before the game is
set up, so it should build data and not call the game.

### Header

| Key | Used by |
|---|---|
| `name` | The tools (OP2LuaEditor, the converters). The mission list shows the DLL's `LevelDesc`. |
| `map` | The tools, and a check: the core logs a warning when it differs (ignoring case) from the DLL's `MapName`. The DLL's map is the one that loads. |
| `tech` | The tools. The game uses the DLL's `TechtreeName`. |
| `type` | The tools (names in the table above). The game uses the DLL's `DescBlock`. |
| `max_tech` | The tools. The game uses the DLL's `DescBlock`. |
| `players_count` | The tools. The game uses the DLL's `DescBlock`. |
| `unit_only` | The tools (`true` or `false`; left out is `false`). The game uses the DLL's `DescBlock`. |
| `ai_players` | The tools (left out is 0). The game uses the DLL's `DescBlockEx`. |

The header holds what the mission DLL says, and the tools keep the two in step: OP2LuaEditor's Save Mission and
Mission Properties, and `op2lua-newmission --dll-from`, build the DLL from the header (a key the header lacks is
taken from the DLL already there), keep the old DLL as `.bak` and leave alone a DLL that isn't an OP2Lua stub.
They write `unit_only` and `ai_players` only when they are set.

### `players`

A table keyed by player number, **1-based** (`[1]` is the first player). Each entry:

| Key | Default | Meaning |
|---|---|---|
| `colony` | `"Eden"` | `"Eden"` or `"Plymouth"`; anything else is Eden. |
| `human` | `true` | `false` makes a computer player. |
| `color` | the game's | `Blue`, `Red`, `Green`, `Yellow`, `Cyan`, `Magenta`, `Black`, `White`; an unknown name is Blue. Left out, the colour stays as the game set it. |
| `resources` | | See below. |
| `center_view` | | `{ x, y }`: the tile the view starts on, for the player at this computer. |
| `allies` | | A list of player numbers (1-based) this player is mutually allied with. |

`resources`, all optional:

| Key | Default | Meaning |
|---|---|---|
| `common_ore`, `rare_ore` | 0 | Stored ore. The game caps it at the colony's storage at the colony's first update, so ore beyond what the starting structures hold is lost (see API.md, `player.ore`). |
| `food` | 0 | Stored food. |
| `kids`, `workers`, `scientists` | 0 | Population. |
| `solar_satellites` | | Solar Power Satellites in orbit (applied when above 0). |
| `edward_satellites` | | EDWARD Satellites in orbit (applied when above 0): with one up every mining beacon counts as surveyed for the player. |
| `tech_level` | | Gives every topic whose tech ID is at most 1000 times this, and the free topics after them (applied when above 0). |
| `completed_research` | | A list of tech IDs (from the tech tree file) marked researched. |

Some files carry `bot` on a player or `morale` in `resources`; the core reads neither (the script can hold morale
with `game.morale_steady`).

When `resources` is present every number in it is set, so a left-out `common_ore` means 0 ore, not the game's
starting amount.

**Multiplayer missions leave `players` out.** The lobby has already chosen each player's colony, colour and
human or computer, and a `players` entry would override them. Without it `players[]` still gets every player in
the game; set starting resources from the script (API.md, Multiplayer missions).

### `units`

A list, made in order. Each entry:

| Key | Default | Meaning |
|---|---|---|
| `type` | (required) | A vehicle or structure name (table below). An unknown type is skipped with a warning in the log, and still counts for beacons' `after`. |
| `player` | 1 | The owner, 1-based. |
| `at` | | `{ x, y }`. |
| `name` | | Publishes the unit as `units["name"]` for the script. |
| `weapon` | none | The turret of a combat vehicle or Guard Post (weapon names below). An unknown name gives no weapon. On a Guard Post `"Starflare"` and `"Supernova"` make the Guard Post versions of those weapons. |
| `direction` | `"East"` | `East`, `SouthEast`, `South`, `SouthWest`, `West`, `NorthWest`, `North`, `NorthEast`; an unknown name is East. |
| `lights` | `true` | A vehicle's headlights. |
| `health` | 1.0 | The share of hit points left: `0.5` starts the unit half damaged. |
| `cargo` | | A Cargo Truck's load (cargo names below). An unknown name is ignored with a warning. |
| `amount` | 1000 | How much `cargo` the truck carries. |
| `kit` | | A ConVec's structure kit: a structure name. An unknown name is ignored with a warning. |
| `kit_topping` | | The kit's weapon, for a Guard Post kit (Starflare and Supernova as for `weapon`). |

A mine (`CommonOreMine`, `RareOreMine`) must stand on a beacon that already exists: give the beacon no `after`
(it is then made before every unit) or an `after` no later than the mine's position in the list. A mine with no
beacon under it crashes the game.

**Unit types**

- Vehicles: `CargoTruck`, `ConVec`, `Spider`, `Scorpion`, `Lynx`, `Panther`, `Tiger`, `RoboSurveyor`,
  `RoboMiner`, `GeoCon`, `Scout`, `RoboDozer`, `EvacuationTransport`, `RepairVehicle`, `Earthworker`,
  `SmallCapacityAirTransport`.
- Structures: `CommandCenter`, `StructureFactory`, `VehicleFactory`, `ArachnidFactory`, `ConsumerFactory`,
  `CommonOreMine`, `RareOreMine`, `CommonOreSmelter`, `RareOreSmelter`, `CommonStorage`, `RareStorage`,
  `Agridome`, `Residence`, `ReinforcedResidence`, `AdvancedResidence`, `Nursery`, `University`, `MedicalCenter`,
  `RecreationFacility`, `Forum`, `DIRT`, `Garage`, `RobotCommand`, `TradeCenter`, `BasicLab`, `StandardLab`,
  `AdvancedLab`, `Observatory`, `Spaceport`, `MeteorDefense`, `GORF`, `GuardPost`, `LightTower`, `Tokamak`,
  `MHDGenerator`, `SolarPowerArray`, `GeothermalPlant`, `MagmaWell`.
- Also accepted: `Tube`, `Wall`, `LavaWall`, `MicrobeWall` (better in `walls`) and `Wreckage`.

**Weapons**: `Laser`, `Microwave`, `RailGun`, `RPG`, `EMP`, `ESG`, `Stickyfoam`, `AcidCloud`, `ThorsHammer`,
`Starflare`, `Supernova`, `EnergyCannon` (the Scorpion's).

**Cargo**: `Food`, `CommonOre`, `RareOre`, `CommonMetal`, `RareMetal`, `CommonRubble`, `RareRubble`,
`Spacecraft`, `Wreckage`, `GeneBank`, `Empty`; also `Ore` (CommonOre), `Metal` (CommonMetal) and `Rubble`
(CommonRubble).

### `beacons`

A list. Each entry:

| Key | Default | Meaning |
|---|---|---|
| `type` | `"MiningBeacon"` | `MiningBeacon`, `MagmaVent` or `Fumarole`. |
| `ore` | `"Random"` | A mining beacon's ore: `Common`, `Rare` or `Random`. |
| `yield` | `"Random"` | `Bar3`, `Bar2`, `Bar1` or `Random`. |
| `variant` | `"Random"` | `High`, `Mid`, `Low` or `Random` (also `Variant1`, `Variant2`, `Variant3`). |
| `at` | | `{ x, y }`. |
| `name` | | Publishes the beacon as `units["name"]`. |
| `after` | 0 | When to make it: 0 (or left out) before the first unit; `n` right after the n-th entry of `units`; a number past the last entry after all of them. |

`after` exists so a converted mission makes its objects in the original's order, which gives every unit the same
object id as in the original. Unknown `yield` and `variant` names are Random.

### `walls`

A list of `{ type =, at = { x, y } }`, one tile each. `type` is `Wall` (the default), `LavaWall`, `MicrobeWall`
or `Tube`.

### `regions`

A table of name to point (`{ x, y }`) or rectangle (`{ x1, y1, x2, y2 }`). Each becomes `regions["name"]` in the
script, a Region in status-bar coordinates (API.md, Region).

### `markers`

A table of name to `{ type =, at = { x, y } }`. `type` is `Circle` (the default), `DNA` or `Beaker`; the marker is
drawn on the map. Each becomes `markers["name"]` = `{ x =, y = }` in the script, holding the **game's own tile**
(0-based, with the hidden margin), not the status-bar coordinate.

### What the core makes, in order

When the mission starts (the game's `InitProc`), before `on_init`:

1. `players`: each listed player's colony, human or computer, colour, resources, view and allies.
2. Beacons with no `after` (or `after = 0`).
3. `units`, in list order; after the n-th entry, the beacons with `after = n`. Then the beacons whose `after` is
   past the last entry.
4. `walls`.
5. `regions`, then `markers`.
6. The script's globals `players`, `units`, `regions` and `markers` are set.

These globals don't exist while the script file itself is being run (at load); use them from `on_init` on.

## `<Mission>.lua`

An ordinary Lua chunk run once when a new game of the mission starts, after the placement file has been read (not
when a saved game loads: see *Saved games*). It defines globals; the core then looks up these callbacks by name, any
of which may be left out:

| Callback | When |
|---|---|
| `on_init()` | Once at the start, after the placement is applied. Return `false` (or `nil`) to abort the mission; returning nothing carries on. |
| `on_tick()` | Every game tick, after the scheduled events of that tick. |
| `on_chat(chat)` | A chat message. |
| `on_create_unit(unit)`, `on_destroy_unit(unit)` | A unit made or destroyed (OPU 1.4.0 and later). |
| `on_damage_unit(target, source, damage)` | A unit damaged (OPU 1.4.2 and later). |
| `on_load()` | Once, right after the script's top level has run, before the placement is applied and `on_init`. |
| `on_unload()`, `on_end(result)` | The game unloads the mission (OPU 1.4.2 and later), or ends it (`result`: how; API.md). |
| `on_save()`, `on_load_save()` | The game is about to save; the first tick after a saved game loaded (*Saved games*). |

Everything a script can call is in [API.md](API.md).

**The Lua environment**: Lua 5.4 with the `base`, `string`, `table` and `math` libraries; no `io`, `os`,
`package` or `require`, no `dofile`, `loadfile` or `load`, and no `math.random` or `math.randomseed` (use
`game.rand`, which every player in a multiplayer game draws the same). `print` writes to the mission's log,
`OPU\logs\OP2Lua-<base>.log`. A fresh Lua state is made
for each mission, so nothing carries over from the last one. The placement file and the script share that state.

## Saved games

A saved game carries the mission's script on where it was (OP2Lua 0.8.8 and later). The game saves the units, the
map, the triggers and the AI groups; OP2Lua adds the script's state to the same file, and puts it back when the save
is loaded. The script's top level, `on_load`, the placement and `on_init` don't run again: the script goes on from
the tick it was saved at, its timers due when they were due, and calls `on_load_save()` on the first tick after the
load.

**What is kept**: everything the script can reach from its globals and its triggers' `on` functions:

- tables (with their metatables), strings, numbers and booleans, a table that refers to itself included;
- the script's functions with their upvalues: a file-level `local` that several functions use stays one variable;
- the scheduler's pending `after`, `every` and `when` functions, and the triggers' `on` functions;
- the game's handles - `Unit`, `Trigger`, `Group`, `Player`, `Region`, `UnitBlock` - as the ids the game's own save
  keeps, so `units.base_cc` is still the Command Center after the load;
- what the script changed in the core's own tables (`game`, `players`, `units`, `regions`, `markers`, ...), and the
  library functions it keeps (`local fmt = string.format`);
- the core's own state: unit orders still waiting to go out, the objective texts, `log.show_ai_in_game`, and the
  `hackaray` patches (set again after the load).

**What a script must do**: keep its state in Lua variables, as any script does; nothing has to be written in
`on_save` or read back in `on_load_save`. Those two are for what lives outside the script:

- Game settings a saved game doesn't hold. The game saves most of what a script changes in it, but not all:

  | Kept by the game's save | Not kept: set it again in `on_load_save()` |
  |---|---|
  | unit stats (`set_unit_stat`; measured), required techs (`set_required_tech`), the map and its cells, research done, morale locks (`morale_steady`), `force_robot_command`, daylight | research costs (`set_tech_cost`): the game reads the tech file again when it loads a save |
  | | the music playlist (`set_music`; not measured, so set it to be safe) |

  The left column comes from what the save holds (the unit-type table, the game's own state block, the day/night
  section); only unit stats were checked in a game.
- Files: `campaign.save` writes to disk at once, and a load doesn't take it back. A WAV `game.play_wav` was playing
  doesn't play on.
- `on_save()` runs while the game writes the file: it may change the script's own variables but mustn't change the
  game.

**What a script mustn't keep** in a variable when the game is saved: an iterator in the middle of a walk
(`string.gmatch`'s), or a C function from somewhere other than the libraries and the core. The game is then saved
without the script's state: the Mission log names the value (`G.words.iterator` is the global `words`' field
`iterator`), the player gets a message, and loading that save stops the script. A `pairs` or `ipairs` walk inside a
function is fine: nothing of it is left once the function returns.

**A save resumes with the same script**: the save names each of the script's functions by its place in the
`.lua` file (it holds no code), so loading it needs that mission's `.lua` exactly as it was. After the script changes
(an update of the mission), its older saves load with the script stopped, and the Mission log and the player are
told why. A change to the placement file doesn't matter. A later OP2Lua loads the save while its own runtime (the
scheduler) is unchanged; otherwise the same (*Which OP2Lua updates keep saves loading*).

**Releasing an update of a mission**: change a `.lua` only when it needs fixing, since every byte counts (a comment,
a version string, line endings). Keep things that change with each release out of the script: a version number
belongs in the placement file, the DLL's description or a message the launcher shows. A tool can tell before loading
whether a save will carry on (*The state's header*).

**Checking a mission**: save a game, play on a little, load it, and read the Mission log: `saved game: the script
carries on` and the script's own lines going on from the saved tick mean it worked; anything else says why
([LOGGING.md](LOGGING.md), *Saved games*). A game that wasn't saved and one that was loaded should log the same lines
after the save's tick.

**Older saves**: a game saved before OP2Lua 0.8.8 holds no script state. It loads (units, map, triggers as the game
saved them) with the mission's script stopped, and says so; to play the mission with its script, start it again.

**A load that stops the script** (a changed script, an older save, a state that couldn't be saved) tells the player:
the message pane shows `GAME LOAD MISSION FAIL` and a line saying why, on the first tick and again 75 ticks later,
with the sound of an incoming chat line.
The mission's objectives then read *Objective unknown: the mission's script is stopped*: their texts were the
script's. A game saved again after that, and loaded later, reads the same. A mission that shows its own message on a good load (from `on_load_save`) needs nothing for this.

**Tables in a loaded game** are new tables with the same contents. A loop over a table with `pairs` may visit its
keys in another order after a load than in the game that saved; a script that needs an order uses an array
(`ipairs`, `#`) or sorts.

How it is stored: the bytes are the mission DLL's save region (`GetSaveRegions`), in the save's mission block with
their size first. Saving and loading go through three hooks the core puts into Outpost2.exe 1.2.0.7's save and load
code (src/SaveGame.h); with another Outpost2.exe the hooks aren't made, the Main log says so, and saved games behave
as before 0.8.8. The state of a large campaign mission takes 10-150 KB.

### Which OP2Lua updates keep saves loading

A save made with one OP2Lua loads with a later one when all of these still hold; a release that breaks one says so
in its changelog ("old saves stop the script"), and OP2Lua avoids it where it can:

- **The runtime is the same.** The core's own Lua (the scheduler behind `after`/`every`/`when`, `print`, `log`) is a
  chunk of its own, `OP2Lua runtime`, hashed like the mission's script. Any change to it stops the script in every
  mission's older saves.
- **The format number is the same** (below).
- **What the save names is still there.** Library and binding functions and tables the script kept are saved by name
  (`game.tick`, `string.format`); removing or renaming one that a save refers to stops that save's script.

Adding functions, fixing what a binding does in the game, and any change to OP2Lua's C++ alone keep saves loading.

### The state's header (for tools)

Tools may read the start of the state from a `.op2` file; this part stays as it is while the format number stays 1,
and changes only with a new format number. In the save's mission block (magic `0xAEDE0034`, u32 name length, the
mission DLL's file name) comes the u32 size of the region, then the region. All integers are little-endian; a
*string* is a u32 length and that many bytes, no terminating zero.

| Field | Contents |
|---|---|
| 8 bytes | `OP2LuaSv` |
| u32 | format: 1 |
| string | the OP2Lua version that saved (e.g. `0.8.14`) |
| string | the mission: its DLL's name without `.dll` (letters, digits, `-`, `_`) |
| u8 | 0: no script was running; 1: the script's state follows; 2: the state couldn't be saved, and a string says why |

With 1, the scripts the state needs follow:

| Field | Contents |
|---|---|
| u32 | how many scripts |
| each: u32 | the script's number in the saving game (used inside the state) |
| each: string | its name: `OP2Lua runtime`, `mission script`, or `placement` (only when the state uses a function from it) |
| each: u64 | the script's hash: FNV-1a, 64 bits, over every byte of its file as it is on disk (for `OP2Lua runtime`, the core's built-in text), with prime 1099511628211 and starting value **1469598103934665603** (0x14650FB0739D0383; not FNV's usual 14695981039346656037, and kept so for older saves) |
| each: u32 | how many functions the compiled script has |

So a tool can tell whether a save will carry on with the installed mission: FNV-1a of `<Mission>.lua`'s bytes must
equal the `mission script` hash (and the running core's runtime must be the one that saved, which only its version
tells a tool). What follows (a u32 count, then the values) is OP2Lua's own and may change within a format.
