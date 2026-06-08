# OP2Lua Architecture

This document records the design and the reasoning behind it, so future contributors understand
*why* the project is shaped this way - not just what it does.

## The core idea: nouns vs. verbs

A mission is two files with a clean separation of concerns:

- **`placement.lua` - the nouns.** Where everything starts: players, their colony/AI/resources,
  pre-placed units and structures, beacons, walls, and named regions/markers. It is a **generated
  data table**, produced by OP2MissionEditor from a `.opm`. Authors never hand-edit it.
- **`mission.lua` - the verbs.** What *happens*: timers, attack waves, win/lose conditions,
  reactions to game events. Written by the author, by hand.

The bridge between them is **names**. Every object placed in the editor gets a name/ID; the logic
refers to it by that name: `units["enemy_cc"]`, `regions["spawn_point"]`. Place in the editor,
script by name.

### Why a generated Lua *data table* (and not JSON, and not generated code)

We considered three ways to get placement into the runtime:

1. **Runtime reads the `.opm` (JSON) directly.** Requires a JSON parser *and* native setup code
   for every field, in C++. Two parsing/marshalling paths to maintain.
2. **Converter emits imperative Lua** (`create_unit{...}` calls) mixed with author logic. Powerful,
   but regenerating placement risks clobbering hand-written logic - a classic generated-code trap.
3. **Converter emits a Lua *data table*; the runtime applies it.** **Chosen.**

Option 3 wins on every axis we care about:

- **No JSON parser in the C++ runtime** - Lua parses its own tables natively. The runtime has
  exactly **one** binding layer (sol2 → Tethys), not two.
- **Clean regeneration** - `placement.lua` is pure regenerated *data*, never hand-edited, so the
  editor can overwrite it freely. Author logic lives in a separate `mission.lua`.
- **Uniform mental model** - everything is "just Lua," fully inspectable.

The runtime flow is therefore:

```
load placement.lua  → apply it (create players/units/resources, register named handles)
load mission.lua    → resolve callbacks (on_init, on_tick, on_*),  then call on_init
run in lockstep     → callbacks fire on every client; state changes only via synced Tethys calls
```

This is structurally the same "data + logic" split either way - we just chose the data format to
be a Lua table so the runtime stays a single Lua host.

## Multiplayer is the hard master

Outpost 2 multiplayer is a **lockstep simulation**: every client runs the entire game locally and
only player *command packets* cross the network. **Mission logic runs on every client.** If it
isn't deterministic and identical across machines, the game desyncs ("players out of sync").

The governing rule:

> The mission script must be a deterministic function of *(game tick, synchronized game state)* -
> identical on every client - except for presentation actions that don't touch the simulation.

### Make the easy path the safe path

Rather than ask authors to understand lockstep, we remove the footguns and provide safe
equivalents, so the obvious code is automatically correct:

| Desync risk | What we do |
|---|---|
| Wall-clock time / frame timing | No clock access. Scheduling is **tick-based** (`after`/`every`/`when`). |
| Unsynced randomness | `math.random` is removed. The only randomness is `game.rand`, routed through the engine's **synchronized** RNG. |
| Hash-order iteration | Helpers iterate in a **deterministic order** (sorted by ID), never raw table/hash order. |
| File / OS / environment | `io`, `os`, and arbitrary `require` are stripped from the sandbox. |
| Per-client divergence | Simulation-changing actions go through Tethys calls that issue **synced commands**; presentation (messages, camera, music) may be local. |

An author writes `game.rand(...)` because it's the only random there is - and gets multiplayer
determinism for free.

## Two DLLs, no hooks

OP2 builds its mission menu by **statically reading every mission file's exported `DescBlock` /
`LevelDesc`** from the PE export table - no mission code runs at scan time. So *each mission must
be its own DLL with its own baked-in `DescBlock`*. There is no way to make N missions appear from
one binary, and (because the scan is static) no way to populate the desc by running code.

Earlier this project planned to be a resident op2ext module that hooks `MissionManager` to
impersonate a mission for `.lua` files. We dropped that entirely: hooking the loader is only
necessary when the script runtime *can't* be torn down and rebuilt per mission - an embedded
CPython, for instance, can't be cleanly re-initialized mid-process, so it must stay resident.
**Lua has no such constraint** - a `lua_State` is created and destroyed per mission - so OP2Lua can
just *be* ordinary mission DLLs. The result is split in two:

- **`OP2LuaCore.dll`** - the shared runtime. Embeds **Lua 5.4** (~35 `.c` files compiled in) and
  binds Tethys with **sol2** (header-only; bindings are `lua.set_function(...)`, no CLIF/LLVM).
  Exports a small C API (`OP2LuaCore_LoadMission`, `OP2LuaCore_InitProc`, `OP2LuaCore_On*`, …).
  Ships once.
- **`LuaMission.dll`** - a tiny per-mission stub. It exports the real OP2 mission interface
  (`DescBlock`, `LevelDesc`, `MapName`, `InitProc`, `AIProc`, `On*`, …), finds its own folder via
  `GetModuleFileName` at load, and forwards every call into `OP2LuaCore.dll`. The converter (or the
  `op2lua-newmission` tool) clones this prebuilt binary and **byte-patches** per mission - no
  compiler: the `LevelDesc` (menu name) and `MapName` (ASCII data exports), plus the UTF-16
  `FileDescription` in its version resource (so Windows Properties show the mission's name under
  `OP2Lua Mission` / `Outpost Universe`). The code is identical for every mission.

What this buys us versus the hook approach: **no op2ext module, no MissionManager hooks, no
hardcoded call-site offsets, no `Patcher`, no `capstone.dll` runtime dependency.** OP2 discovers
the stub like any other mission DLL.

### Per-mission lifecycle

```
OP2 scans maps\* → reads each stub's static DescBlock (name + numPlayers, patched by the converter)
                 → builds the menu
player picks a mission → OP2 LoadLibrary's the stub
  stub InitProc → OP2LuaCore_LoadMission(<own folder>, <own DLL base name>)
     → core pins itself (so freeing the stub later can't unload it mid-game)
     → fresh lua_State; load <base>.placement.lua then <base>.lua  (falls back to placement.lua /
       mission.lua); resolve on_* callbacks
  stub AIProc / On* → forwarded to OP2LuaCore_* → the Lua callbacks (each in a protected call)
mission unloads → OP2LuaCore_OnUnloadMission → tear down the lua_State (core stays pinned)
```

Two robustness details worth noting: the core **pins itself** at first load (OP2 frees the stub
when a mission ends, which would otherwise drop the core's refcount to zero and unload the whole
runtime mid-game - a crash in `FreeModuleDesc`); and the stub exports a no-op `OP2LuaTrigger` that
the runtime's internal victory/failure triggers reference by name, so OP2's trigger-function
resolution succeeds instead of failing on an empty name.

Because the `lua_State` is rebuilt per mission, every mission starts from a clean global
environment - no leakage between missions, and a script error is caught and logged, never crashing
the game.

### Diagnostics: two log channels

Every log line is tagged `kCore` or `kMission` and tee'd to up to two files:

- **`OP2Lua.log`** (master) - opened once, persists across the session, receives **core** lines
  only: lifecycle (load, version/build stamp, placement apply) and all errors/crashes. A mission's
  own output never lands here, so it stays a clean engine record (a 90-wave mission won't flood it
  with per-spawn lines).
- **`OP2Lua-<base>.log`** (per mission) - re-pointed on each load, receives **both** channels: the
  core context for that run *and* the mission's own output (author `print()`, `game.message`,
  `create_unit`, `mission.win/lose`). This is the mission author's working log.

Errors are `kCore`, so they appear in both - a failure is recorded centrally and shown to the
author. The split keeps a newbie's debug `print()` in their own log without polluting the shared one.

## `placement.lua` schema (draft)

The converter emits a single `return { ... }` table. Draft shape (see `op2luasdk/samples/hold-the-line`):

```lua
return {
  -- mission metadata for the editor / author reference. NOTE: name + map here do NOT drive the
  -- game - OP2 reads LevelDesc + MapName baked into the stub DLL before any Lua runs. The core
  -- warns if this map= disagrees with the DLL's baked MapName. Re-patch the DLL to change either.
  name = "Hold the Line",
  map  = "eden01.map",
  tech = "MULTITEK.TXT",
  type = "Colony",            -- Colony | MultiLastOneStanding | ... (see OPM_FORMAT)
  max_tech = 12,

  players = {
    [1] = { colony="Eden",     human=true,  color="Blue",
            resources = { food=1000, kids=10, workers=14, scientists=8,
                          common_ore=0, rare_ore=0, tech_level=0 } },
    [2] = { colony="Plymouth", human=false, color="Red", bot="Balanced" },
  },

  -- coordinates are in-game / status-bar coordinates (what OP2MissionEditor shows);
  -- the runtime translates them to OP2's internal grid via GameMap::At at load time.
  units = {
    { name="enemy_cc", type="CommandCenter", player=2, at={93,39} },
    { type="Tiger", player=2, at={95,40}, weapon="Microwave", health=1.0, lights=true },
  },

  beacons = { { type="MiningBeacon", ore="Common", yield="Random", at={88,42} } },
  walls   = { },                       -- { type="Tube"|"Wall"|..., at={x,y} }
  regions = { spawn_point={93,39}, your_base={40,55} },   -- name -> point or rect
  markers = { },                       -- name -> { type=..., at={x,y} }
}
```

Field meanings mirror the `.opm` model (see the OPM_FORMAT reference in the OP2OpmTools repo); the
converter translates enum names and applies `UnitData` defaults so the runtime can apply the table
verbatim. **Coordinates stay in editor / status-bar space** - the converter passes them through
unchanged and the runtime applies OP2's padding offset (via `GameMap::At`), so the same numbers
appear in the editor, the `.opm`, the `.lua`, and the in-game status bar.

## Component boundaries

```
┌──────────────────────────┐   .opm   ┌────────────────────────────────────────┐
│ OP2MissionEditor (C#)    │ ───────▶ │ OP2OpmTools / converter (C#)           │
│  visual placement        │          │  .opm → placement.lua                  │
└──────────────────────────┘          │  + clone LuaMission.dll, patch DescBlock│
                                       └───────────────┬────────────────────────┘
   author writes mission.lua                           │  maps\<mission>\:
                                                        │   <Mission>.dll + placement.lua + mission.lua + .map
                                                        ▼
   ┌──────────────────────────┐  forwards   ┌──────────────────────────────────┐
   │ LuaMission.dll (stub)    │ ──────────▶ │ OP2LuaCore.dll (shared runtime)  │
   │  OP2 mission exports     │  C API      │  Lua host + sol2 bindings        │
   │  DescBlock/InitProc/On*  │             │  ↔ Tethys (the game)             │
   └──────────────────────────┘             └──────────────────────────────────┘
```

- **C# side** (editor + converter): where most authoring value lives, and where the team is
  already strong. Owns placement, the `.opm` → `placement.lua` transpile, and the `DescBlock`
  byte-patch of the stub.
- **C++ side** (this repo): two DLLs - a tiny forwarding stub and a thin, deterministic Lua host.
- **Missions**: data + logic, both Lua, no per-mission compile.
