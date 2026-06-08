# OP2Lua

Write Outpost 2 missions in Lua - no compiler, no DLL authoring.

OP2Lua lets you build a mission for *Outpost 2* from two plain
text files instead of a hand-compiled C++ mission DLL:

| File | What it is |
|---|---|
| `cMyMission.placement.lua` | Players, starting units, resources, beacons, named regions. The starting layout. |
| `cMyMission.lua` | What happens during the mission (timers, attack waves, win/lose, reactions). |

The author edits Lua and never touches a compiler. (A tiny prebuilt stub DLL ships *with* the
mission - see the architecture below - but no one compiles it per mission.)

The goal is simple: *make missions easy for non-C++ developers*. OP2Lua is built around **Lua**
and the existing **`.opm`** placement format, reusing the reverse-engineered `Tethys` API while
keeping the toolchain tiny - no LLVM, no CLIF, no embedded interpreter to build, and **no Patcher,
no capstone, no op2ext module**.

---

## Status

**Working in-game (v0.6.0).** A **5-mission demo pack** runs in real Outpost 2 - **Hold the Line**
(defense), **Strike Team** (offense), **The Convoy** (escort), **Hold the Beacon** (control), and
**Seek and Destroy** (hunt): placement, escalating waves, scripted/reactive enemies, timed
reinforcements, cargo trucks, synced RNG, combat orders, messages, Savant voices, the multiplayer
"Morale Steady" rule, and clean win/lose. Coordinates match the in-game status bar (type what you
see); each mission gets its own debug log.

**Download the 5-mission pack:** https://github.com/leviathan400/OP2LuaSDK/releases

See [`docs/CHANGES.md`](docs/CHANGES.md) for the full feature list and
[`docs/ROADMAP.md`](docs/ROADMAP.md) for what's next.

---

## How it works - two DLLs

OP2 builds its mission menu by **statically reading every mission DLL's exported `DescBlock`/
`LevelDesc`** - so each mission needs its own DLL. Rather than bake the ~800 KB Lua runtime into
every mission, OP2Lua splits it:

- **`OP2LuaCore.dll`** - the shared runtime: the Lua interpreter, the sol2 ↔ Tethys bindings, the
  mission/placement logic, the scheduler, the crash reporter. Ships once, lives in the game folder.
- **`LuaMission.dll`** - a tiny (~100 KB) per-mission stub. It exports the standard OP2 mission
  interface (`DescBlock`, `InitProc`, `On*`, …), finds its own folder at load, and forwards every
  call into `OP2LuaCore.dll`. The converter clones it and **byte-patches** its `DescBlock` per
  mission - no compiler involved. The code is identical for every mission.

This is **not** an op2ext module and needs no `outpost2.ini` entry - OP2 discovers the stub like
any other mission DLL. It works this cleanly *because* we chose Lua: a `lua_State` is created and
destroyed per mission, so the runtime needn't be resident the way an embedded Python interpreter
would force.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full design and [`docs/API.md`](docs/API.md)
for the mission scripting API.

---

## Repository layout

```
OP2Lua/
  README.md                   - this file (the only doc in the root)
  CMakeLists.txt              - 32-bit build of OP2LuaCore.dll + LuaMission.dll
  src/                        - the shared runtime (OP2LuaCore.dll)
    Mission.cpp               - OP2LuaCore_* C API; callback dispatch; per-mission load
    Placement.cpp             - placement.lua -> players/units/beacons + named handles
    LuaHost.cpp               - embeds Lua, runs scripts, scheduler preamble
    Bindings.cpp              - the author-facing API, bound to Tethys via sol2
    Names.cpp                 - name <-> MapID / SoundID tables
    ErrorHandling.cpp         - crash reporter / native stack traces
    Log.cpp                   - dependency-free logger + reliable build stamp
    OP2LuaCore.h / *.h        - the C API header + internal headers
  stub/
    LuaMission.cpp            - the per-mission stub (template; DescBlock byte-patched per mission)
  op2lua-newmission/          - cross-platform C++ console tool that scaffolds a new mission
                                (clone+patch the stub + skeleton scripts); compiled exe ships in the SDK
  docs/                       - API.md, ARCHITECTURE.md, DEPLOY.md, CHANGES.md, ROADMAP.md (all docs except README)
  dist/                       - packaged demo pack (core + stub DLLs + scripts + README, zipped)
  third_party/                - Tethys (vendored), Lua 5.4.7, sol2  (see third_party/README.md)
```

---

## Building

All build dependencies are **vendored** under `third_party/` (Tethys, Lua 5.4.7, sol2). You only
need a **32-bit MSVC toolchain** (OP2 is 32-bit). Ninja recommended.

```
cmake -B build -G Ninja
cmake --build build
```

Outputs `build/OP2LuaCore.dll` and `build/LuaMission.dll`. Use `-DOP2_GAME_DIR=<path>` to auto-copy
the core into your game folder on each build. (From a 64-bit host, configure inside the
`x64_x86` cross-tools environment, e.g. `vcvarsall.bat x64_x86`.)

> No LLVM, no CLIF, no CPython, no Patcher, no capstone - those are all gone in this design.

---

## Installing a mission (Outpost 2 1.4.1)

The 1.4.1 installer unpacks the game's content into an **`OPU`** folder under the install root.

1. Put **`OP2LuaCore.dll`** in the `OPU` folder (e.g. `D:\Outpost 2\OPU\`). Ships once, runs every mission.
2. Put each mission's **three files** - its stub DLL (e.g. `MyMission.dll`), `MyMission.lua`, and
   `MyMission.placement.lua` - into `OPU\maps\` (alongside any custom `.map`). The `.lua` files
   must sit next to the DLL (same base name).
3. Launch the game - the mission appears in the list like any other.

> On older slim / 1.3.x installs (no `OPU` folder), the core goes next to `Outpost2.exe` and mission
> files sit with the other mission DLLs. The rule is constant: **runtime with the game, a mission's
> `.lua` next to its `.dll`.**

The complete sample missions (and the `feature-test` API-coverage mission) live in the **OP2LuaSDK**
under [`OP2LuaSDK/samples/`](https://github.com/leviathan400/OP2LuaSDK/tree/main/samples) - kept there as the single source of truth so the
demos can't drift out of sync. For making missions without this repo, see the **OP2LuaSDK** (prebuilt
runtime + docs + samples + a `new-mission` scaffolder) - authors never build anything here.

Logs are written to `Outpost2\OPU\logs\` (each session stamped with the OP2Lua version and the
DLL's build time): a shared **`OP2Lua.log`** (core/engine record across all missions) plus a
per-mission **`OP2Lua-<MissionDll>.log`** that carries that mission's own `print()` debug and
activity. Errors logged to both.

---

## Related tooling

- **`op2lua-newmission/`** - a cross-platform C++ console tool that scaffolds a new mission (clones +
  byte-patches the stub, writes skeleton scripts). It's the manual stand-in for the `.opm` converter;
  prebuilt Windows + Linux binaries ship in the OP2LuaSDK (with `new-mission.ps1` for Windows).
- **OP2MissionEditor / OP2OpmTools** (the `.opm` format) - OP2Lua consumes a `placement.lua`
  *generated from* a `.opm`. The converter belongs on the editor side (it already transpiles
  `.opm` → C++; emitting Lua + byte-patching the stub's `DescBlock`/strings is the same kind of tool).
