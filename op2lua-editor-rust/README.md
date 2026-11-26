# OP2LuaEditor (Rust)

A visual editor for **OP2Lua** mission layouts for *Outpost 2: Divided Destiny*, for Windows, Linux and macOS.

Open a mission, see its units, regions, beacons, walls and markers over its map, place and move units, draw regions,
set the mission's properties, and save it all back to `<Name>.placement.lua`. It also makes new missions and starts
Outpost 2 straight into the one you're editing. You write the mission's logic in `<Name>.lua`; OP2Lua's
`OP2LuaCore.dll` runs both in the game.

This is the Rust rewrite of the VB.NET editor in [`../op2lua-editor`](../op2lua-editor), and does everything that
one does. Built with [egui/eframe](https://github.com/emilk/egui) and
[op2-formats](https://crates.io/crates/op2-formats) (the game's maps, tilesets, `.vol` archives and mission DLLs, and OP2Lua's `placement.lua`: its `placement` module
reads and rewrites the file as text, without disturbing what the editor doesn't own).

## Requirements

- An **Outpost 2** install with **OPU 1.4.1**: the editor reads the map graphics from the game's files. It looks for
  the install itself; if it can't find it, File > Settings asks for the folder that holds `OPU`.
- Test in OP2 needs `op2launcher.exe` (and `Launcher.dll`) in the `OPU` folder; on Linux and macOS it runs through
  Wine.

## Using it

**Opening.** File > Open mission takes a mission's `<Name>.lua` or `<Name>.placement.lua` and opens its map with it
(a `.map` beside the mission first, else the game's). You can also drop either file, or a `.map`, on the window, or
name one on the command line (`OP2LuaEditor MyMission.lua`, `OP2LuaEditor eden01.map`). The last mission or map
reopens at start. A map can be opened on its own too (File > Open map file / Open map from the game).

**Moving around.** Drag with the right or middle mouse button to pan; the wheel zooms about the cursor (5% to 400%).
The status bar shows the tile under the cursor in status-bar coordinates, the ones `placement.lua` and OP2Lua use and
the in-game status bar shows. Click a tile to see its cell type, tile graphic and flags, and what the mission has on
it.

**Views** (View menu): the game map; a passability overlay (green fast, yellow medium, orange slow, red blocked, cyan
tubes), plain or with each tile's cell type named; flat cell-type colours, plain or named. Names show once tiles are
big enough to read. A grid, and switches for units and for beacons, walls and markers.

**Tools** (the bar under the menu):

| Tool | Left mouse |
|---|---|
| Select | Click a unit or region to select it. Delete removes it. |
| Units | Click empty ground to place the chosen type, player and weapon; drag a unit to move it. |
| Regions | Drag on empty ground for a rectangle, click for a point, then name it; drag a region to move it, drag the selected one's handles to resize it. |

The side panel lists every unit and region and edits the selected one: a unit's type, player, weapon and position, a
region's name and corners. A Lynx, Panther or Tiger always carries a weapon (Laser when none is chosen). Clear all
(beside each list, or in the Mission menu) removes every unit or region, after asking.

**Saving.** File > Save mission (Ctrl+S) writes the units and regions back into `placement.lua` and changes nothing
else: every other field of every unit (direction, health, cargo, ...), every other table, every comment and every
blank line stays as it was. Deleting a unit renumbers the beacons' `after = n` so each still follows the same unit.
The previous file is kept as `<file>.bak`. Save also writes the mission DLL from the placement's header (see
*Properties*), so the DLL always holds what the editor shows. If the file changed on disk while it was open, the
editor reloads it (no edits here) or asks which to keep. Closing the window, or opening something else, with unsaved edits asks first.

**Mission menu.**

- *Properties* edits everything the mission DLL tells the game, kept in `placement.lua`'s header: name, map, tech tree
  file, type, max tech level, players, unit-only and a multiplayer game's AI players (`unit_only` and `ai_players`
  only when set). Outpost 2 reads them from the DLL before any Lua runs, so OK and Save build the DLL beside the
  mission (`c<Base>.dll` or `<Base>.dll`, or a new one) from them: a copy of the bundled stub with each value
  written at its export. Both files keep a `.bak`. Where the DLL holds other values (a DLL left at the stub's max
  tech level 12, say), opening the mission and the dialog say so, and the next save replaces them. A DLL that isn't
  an OP2Lua stub is left alone.
- *New mission* writes a mission into the game's `OPU\maps\LUA`: the DLL built from its properties, and starter
  scripts (a two-player fight, or a lobby skeleton for a multiplayer type).
  A Colony mission's DLL is `c<Base>.dll`, so Outpost 2 lists it under Colony Games.
- *Import regions from...* takes another placement file's regions; *Save regions into...* and *Save units into...*
  write this mission's table into another placement file (the rest of that file kept, with a `.bak`).
- *Test in OP2* (F5) copies the mission into `OPU\maps\LUA` (asking before it replaces files there that it didn't put
  there) and starts Outpost 2 straight into it.

**Structures.** Every structure is drawn over its footprint with the ring of dozed ground the game clears around it.
Those that connect by tubes (`building.txt`'s Border column: not power plants, mines, the magma well or light towers)
show the two tube tiles the game lays when it places them: one east of the footprint on the location's row, one south
of it on the location's column. Footprints and tube flags come from the game's own `building.txt`.

**Images.** File > Export map image saves the map as a PNG at full size (32 px per tile) or 1/8, with the open
mission drawn on it as the View menu shows it. File > Save map as writes the `.map` itself.

The status bar also shows which `OP2LuaCore.dll` the game has (version and build time), and notes when a map's tile
group names hold stray bytes (the map still works).

## Building

Rust 1.99 (pinned in `rust-toolchain.toml`, as op2-formats is):

```
cargo build --release
```

The executable is `target/release/OP2LuaEditor` (`.exe` on Windows, with the editor's icon). On Linux, eframe needs
the usual X11 or Wayland and OpenGL development packages, and rodio the ALSA ones (`libasound2-dev`).

## Tests

```
cargo test
```

The tests cover reading and rewriting `placement.lua` (round trips, edits, deletes, beacons' `after`), the tools
driven by synthetic mouse input through egui, saving, the DLL built from the properties (read back with
op2-formats' mission-DLL reader), New Mission's files, Test in OP2's copy plan and the exported image. Two need data and are skipped without it:

- `OP2LUA_PLACEMENT_DIRS` (folders separated by `;`): every `.placement.lua` under them must read and write back
  byte for byte.
- `OP2_DIR` (the game folder): the exported image is drawn on eden01; `OP2LUAEDITOR_EXPORT_PNG` saves it for a look.

## Source

| File | What it holds |
|---|---|
| `src/main.rs` | Start-up: the window, its icon. |
| `src/app.rs` | The window: menus, panels, status bar, dialogs and questions, opening and saving. |
| `src/view.rs` | The map canvas: pan, zoom, drawing the visible tiles from the tileset textures. |
| `src/mapdoc.rs` | A loaded map: tilesets packed into GPU textures, the overview and cell-type textures. |
| `src/celltype.rs` | The views and their cell-type colours and names. |
| `src/mission.rs` | An open mission: its files, units, regions and map objects; save, import, export of tables. |
| `src/editor.rs` | The tools: drawing the mission over the map, mouse handling, the side panel. |
| `src/units.rs` | Unit types, footprints, dozed borders, labels, player and region colours. |
| `src/props.rs` | Mission properties, the mission DLL built from them, New Mission's files. |
| `src/dll.rs` | Writing a mission DLL's exports (LevelDesc, MapName, TechtreeName, DescBlock, DescBlockEx) and version resource. |
| `src/launch.rs` | Test in OP2, and the game's OP2LuaCore version. |
| `src/export.rs` | The exported image with the mission drawn on it. |
| `src/sound.rs` | The beep when a map loads. |
| `build.rs` | The executable's icon; the library versions the About box lists. |
| `assets/` | The stub `LuaMission.dll` (OP2Lua's, as the SDK ships it), the beep, the icon and the About banner. |

When OP2Lua's stub `LuaMission.dll` changes, copy the new one into `assets/` (New Mission clones it).

## License

GPL-3.0-or-later ([LICENSE](LICENSE)), because op2-formats is GPL-3.0. The rest of OP2Lua (the runtime, the SDK, the
VB.NET editor) is MIT.
