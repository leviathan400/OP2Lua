# OP2LuaEditor

> A Rust rewrite that runs on Windows, Linux and macOS and does everything this editor does is in
> [`../op2lua-editor-rust`](../op2lua-editor-rust). This VB.NET editor is what the SDK ships until that one replaces it.

A visual editor for building **OP2Lua** mission layouts for *Outpost 2: Divided Destiny*.

## What is it?

OP2LuaEditor lets you author a mission's starting layout without hand-writing Lua. Load an
Outpost 2 map as the backdrop, place units, draw named regions and markers, set the player and
mission properties, and save it straight to a `placement.lua`. It also scaffolds new missions -
cloning and byte-patching the stub DLL and naming it - so you can go from blank to a playable
mission shell without touching a compiler. You then write the mission logic in `<Name>.lua`.

It pairs with the **OP2Lua** runtime: the editor produces the `placement.lua` (the layout), you
write the `<Name>.lua` (the logic), and `OP2LuaCore.dll` runs them in-game.

## Features

- **Open / load a map** as the editing backdrop (reads OP2 1.4.1 tile graphics from the well BMPs).
- **Place units** for each player (1-6) and **draw regions**, round-tripped to/from `placement.lua`. Saving
  rewrites only what you changed: every other field, comment and line of the file stays as it was. Save Mission also
  writes the mission DLL from the placement's header (below), so the DLL always holds what the editor shows.
- **Beacons, walls, tubes and markers** from `placement.lua` are drawn on the map for context, with the ring of dozed
  ground around every structure and the two tubes the game lays beside each structure with tube connections (east of
  it on its location's row, south of it on its location's column).
- **Mission properties** - everything the mission DLL tells the game: name, map, tech tree file, mission type
  (Colony, Tutorial, AutoDemo, a multiplayer type or a campaign number), players, max tech level, unit-only, and a
  multiplayer game's AI players. They are kept in `placement.lua`'s header (`unit_only` and `ai_players` only when
  set), and OK and Save Mission build the DLL from them: a copy of the bundled stub with each value written at its
  export, the old DLL kept as `.bak`. Where the DLL beside the mission holds other values (a DLL left at the stub's
  max tech level 12, say), the dialog says so and the next save replaces them. A DLL that isn't an OP2Lua stub is
  left alone.
- **New Mission** scaffolding - builds the mission DLL from its properties, writes
  skeleton scripts (a multiplayer type gets a lobby-driven skeleton), straight into the game's `OPU\maps\LUA`. A
  Colony mission's DLL is `c<Base>.dll`, so Outpost 2 lists it under Colony Games.
- **Test in OP2 (F5)** - copies the open mission into `OPU\maps\LUA` and starts Outpost 2 straight into it
  (needs `op2launcher.exe` in the `OPU` folder).
- **Open a mission** from File > Open Mission, the command line, or by dropping its `.lua` on the map.
- Coordinates match the in-game status bar (what you place is where it lands).

## Requirements

- An **Outpost 2 (OPU 1.4.1)** install - the editor loads tile graphics from the game's well files.
- **.NET Framework 4.8**.

Built in VB.NET (WinForms); uses **OP2UtilityDotNet** to read map files.
