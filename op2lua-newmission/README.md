# op2lua-newmission

A tiny **cross-platform** console tool that makes a new OP2Lua mission, or rebuilds an existing mission's DLL from
its `placement.lua`. No game compiler involved.

Outpost 2 reads a mission's name, map, tech tree, type, player count, maximum tech level, unit-only flag and a
multiplayer game's AI players from its DLL before any Lua runs. The tool copies the prebuilt mission stub
(`LuaMission.dll`) and writes each value at its export (`LevelDesc`, `MapName`, `TechtreeName`, `DescBlock`,
`DescBlockEx`), the same bytes OP2LuaEditor's Save writes. For a new mission it also writes starter
`<Mission>.lua` + `<Mission>.placement.lua`, whose header holds the same values.

It's the portable equivalent of the SDK's `tools/new-mission.ps1` (which stays for quick Windows use).

Pure C++17 (`<filesystem>` + `<fstream>`) - no OS-specific APIs. Builds on Windows, Linux, macOS.

## Build

```
cmake -B build
cmake --build build --config Release
```

The binary is `op2lua-newmission` (`.exe` on Windows). A prebuilt **Windows** binary
(`op2lua-newmission.exe`) ships in the **OP2LuaSDK** under `tools/`, so authors don't have to build it.
The SDK's **Linux** build (`op2lua-newmission`, x86-64 static) is older: it has `--map` only and names the
DLL without the `c`. On Linux, `chmod +x op2lua-newmission` first.

### Cross-compiling the Linux binary from Windows

No Linux box needed - [`zig`](https://ziglang.org) bundles a full cross toolchain:

```
zig c++ -target x86_64-linux-musl -std=c++17 -O2 -Wl,--strip-all main.cpp -o op2lua-newmission
```

The `musl` target produces a fully static ELF that runs on any Linux distro.

## Make a new mission

```
op2lua-newmission "My First Mission"
op2lua-newmission "Red Dawn" --map sgame01.map --out ./missions
op2lua-newmission "Relocation" --map Q01.MAP --tech Q01.tek --type 1 --max-tech 3
op2lua-newmission "Duel" --map on2_02.map --players 2 --type LastOneStanding
op2lua-newmission "Test" --stub ../bin/LuaMission.dll
```

| Option | Meaning |
|---|---|
| `--map <file>` | map the mission uses (default `eden01.map`) |
| `--tech <file>` | tech tree file (default `MULTITEK.TXT`) |
| `--type <type>` | `Colony` (default), `Tutorial`, `AutoDemo`, a campaign mission's number (`1` to `12`), or a multiplayer type: `LandRush`, `SpaceRace`, `ResourceRace`, `Midas`, `LastOneStanding` (or `MultiLandRush` etc., as the placement writes them); OP2 lists the mission under it |
| `--players <1-6>` | players in the mission (default 2) |
| `--max-tech <0-12>` | the highest tech level (default 12, every tech; a campaign mission uses its own, e.g. 3) |
| `--unit-only` | a unit-only mission (most reports off) |
| `--ai-players <0-5>` | computer players a multiplayer game adds (default 0) |
| `--out <dir>` | output directory (default `./out`) |
| `--stub <path>` | path to `LuaMission.dll` (default: auto-detected near the cwd / executable) |
| `-h`, `--help` | usage |

It writes three files (base name = the mission name with non-alphanumerics removed):

```
c<Base>.dll             the mission OP2 lists (name, map, tech tree, type, players, max tech level, ...)
<Base>.lua              starter logic (win/lose already wired)
<Base>.placement.lua    starter layout; its header holds the same values as the DLL
```

The placement's header gets `unit_only` and `ai_players` only when they are set.

The core finds `c<Base>.dll`'s scripts as `<Base>.*` by dropping a `c` that comes before a capital, so a
base that doesn't start with a capital (`convoyrun`) keeps the `c` on the scripts too (`cconvoyrun.lua`).
A multiplayer `--type` writes `<Base>.dll` with no `c` (the lobby lists it), a placement without a
`players` table (the lobby sets the colonies) and a mission that starts the type's victory.

Copy all three into your Outpost 2 `OPU/maps` folder and launch the game. See the SDK's
`INSTALL.md` and `docs/GETTING-STARTED.md`.

## Rebuild a mission's DLL

```
op2lua-newmission --dll-from maps/q01Lua.placement.lua
```

Builds the DLL beside the placement (`c<Base>.dll` or `<Base>.dll`, or a new one) from the placement's header:
`name`, `map`, `tech`, `type`, `max_tech`, `players_count`, `unit_only`, `ai_players`. A key the header lacks is
taken from the DLL already there. Use it after editing the header by hand, or to put right a DLL that disagrees
with its placement (one left at the stub's max tech level 12, say); it prints what changed. The old DLL is kept as
`.bak`, a DLL that isn't an OP2Lua stub is left alone, and nothing else is written.

## Finding the stub

`LuaMission.dll` is searched for (in order): `--stub`, `./LuaMission.dll`, `./bin/LuaMission.dll`,
then beside the executable and in `../bin/` relative to it (the SDK layout: `tools/op2lua-newmission`
+ `bin/LuaMission.dll`). Pass `--stub` if it lives somewhere else.
