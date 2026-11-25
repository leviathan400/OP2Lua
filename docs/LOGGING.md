# OP2Lua Logging & Debug — the standard for every mission

This is the convention **every** OP2Lua mission should follow. It keeps the right information in the
right place so you can debug a mission, a scripted AI, or a crash without wading through noise.

OP2Lua writes up to **three log files** per run (in `Outpost2\OPU\logs\`), plus the AI status snapshot
`OP2Lua-<dll>-AI-status.json` when a mission asks for one (`log.ai_status`) and the mission's end result
`OP2Lua-<dll>-result.json` (*Mission result* below), and can also echo a scripted
AI's events to the **in-game comms panel**. Every log line is stamped with the wall-clock time and the
current game **mark** (100 ticks), and nothing is duplicated between files.

```
02:45:05 m260 [INFO] [lava] mark 260: ERUPTION at (21,26) speed 45
         └ mark (game time)        └ your message
└ HH:MM:SS (wall clock)
```

---

## The three files (and what belongs in each)

| File | Channel | Put here |
|---|---|---|
| `OP2Lua.log` | **Main** | The framework's own record across all missions — mission load/end, core version, map, a **one-line crash summary**, and a warning for each call the instruction limit drops. You don't write to this from Lua. |
| `OP2Lua-<MissionDll>.log` | **Mission** | *This mission's* events — `game.message` calls, mission/script errors, your `print()` debug, and the **full crash dump**. |
| `OP2Lua-<MissionDll>-AI.log` | **AI** | A scripted AI's debug stream, written with **`log.ai(...)`**. Lets you monitor/debug just the AI in isolation. Created lazily — a mission that never calls `log.ai` makes no AI file. |

`<MissionDll>` is the mission's DLL base name, e.g. `OP2Lua-cColdFront.log` and
`OP2Lua-cColdFront-AI.log`.

---

## The Lua API

```lua
print(...)                 -- Mission log. Your general debug. (alias: log.mission(...))
log.mission(...)           -- Mission log. Same as print(); use it when you want to be explicit.
log.ai(...)                -- AI log. A scripted AI's actions/decisions.
log.show_ai_in_game(on)    -- also echo log.ai lines to the in-game comms panel (default: off)
game.message(text [,sound])-- a real in-game message (comms panel) AND the Mission log
```

All of `print` / `log.mission` / `log.ai` take any number of values and join them with tabs, exactly
like Lua's `print`. Use `string.format` for structured lines.

---

## The convention — which channel for which output

Decide by **who the line is about**, not where in the code it lives:

| The line is about… | Use | Lands in |
|---|---|---|
| A **scripted AI's** behaviour (its base building, mining, production, unit orders, attacks, raids, repairs) | `log.ai(...)` | AI log |
| A **mission/environment** event (disasters, objectives/victory progress, timers, your own debug toggles, generic loggers) | `print(...)` | Mission log |
| Something the **player** should see in-game (objectives, warnings, story beats) | `game.message(...)` | comms panel + Mission log |

Rule of thumb: **if it describes what the enemy/computer player is doing, it's `log.ai`.** Everything
else a mission prints is `print`. This is what makes the AI log a clean, AI-only stream.

The framework follows the same rule for its own unit instrumentation: `create_unit` for a **computer**
player and the retried move/attack orders (`issued N queued unit order(s)`) are written to the
**AI** log automatically; `create_unit` for the **human** player goes to the Mission log. So spawning
and commanding a scripted AI's forces all shows up in one place without you doing anything.

> **Cold Front** is the reference mission: the Plymouth AI's ~40 debug lines use `log.ai`, while the
> lava disaster, the starship victory tracker, the building-completion logger, and the debug toggles
> stay on `print`.

---

## Tagging — make lines greppable

Prefix each line with a short `[tag]` for the subsystem, so you can filter a log fast:

```lua
log.ai(string.format("[trucks4] mark %d: truck %d/2 -> 4th smelter (%d,%d)", game.mark(), n, x, y))
print(string.format("[lava] mark %d: ERUPTION at (%d,%d) speed %d", game.mark(), x, y, spd))
```

Then in the AI log: `grep "\[trucks4\]"`. Suggested tags: a subsystem name in brackets
(`[mine2]`, `[patrol]`, `[micrun]`, `[starship]`, `[lava]`), or `AI:` / `AI[<stream>]:` for build
pipelines. Including `mark %d` in the body is optional (every line is already mark-stamped) but handy
when a line is also shown in the comms panel, which has no mark column.

---

## Watching the AI live (in-game)

To see a scripted AI's events without leaving the game, gate the comms echo behind a debug flag:

```lua
local AI_EVENTS_IN_GAME = false        -- flip to true to watch the AI in the comms panel
log.show_ai_in_game(AI_EVENTS_IN_GAME)
```

When on, every `log.ai(...)` line also appears in the comms panel (quiet sound). It **always** goes to
the AI log file regardless. Keep it **off by default** — a per-tick `log.ai` (e.g. a unit's position
each tick) will flood the panel. Reserve high-frequency spam for the file; the toggle is best for a
quick live look.

---

## AI status snapshot (for external tools)

`log.ai_status(player)` writes a **current-state snapshot** of a player to
`OP2Lua-<MissionDll>-AI-status.json`. Unlike the logs, this file is **overwritten** each call, so it
always holds "now" — point an external monitor (a dashboard, a script) at it and poll. Drive it once
per mark:

```lua
every(marks(1), function() log.ai_status(players[2]) end)   -- live feed of the AI player
```

---

## Mission result (for launchers and tools)

When a mission ends, the core writes how it ended to `OP2Lua-<MissionDll>-result.json`, overwritten at each end,
so a launcher that started the mission can read the outcome:

```json
{
  "mission": "cMyMission.dll",
  "result": "won",
  "reason": 3,
  "tick": 383,
  "mark": 3,
  "local_player": 0,
  "time": "2026-11-19T01:25:15"
}
```

`result` is the game's end reason as a word: `won` (3), `lost` (4), `aborted` (6) in a single-player game,
`victory` (7), `defeat` (8), `terminated` (9) in a multiplayer one, `other` for anything else (0: the game was
closed while the mission was still running). `tick` is the game's tick when it recorded the end; `local_player` is
0-based.

The file is written as soon as the core learns the mission ended: on a tick that sees the game's end reason, when
the game sends the mission its end (`OnEndMission`) or unloads it, or at the latest when the game exits. Outpost 2
started by `op2launcher` sends neither event and runs no more ticks once the mission ends, so there the file is
written when the player leaves the end screen and the game closes (measured: a win, a loss and a quit from the
menu, OPU 1.4.1). The mission log notes it: `mission ended: won (reason 3, tick 383, seen by the game's exit)`.

The JSON:

```json
{
  "mark": 432,
  "player": 2,
  "resources": { "common_ore": 4200, "rare_ore": 0, "food": 980,
                 "population": 34, "kids": 11, "workers": 15, "scientists": 8 },
  "units": {
    "total": 47, "buildings": 21, "vehicles": 26,
    "by_type": { "Agridome": 4, "CargoTruck": 6, "CommonOreSmelter": 4, "Lynx": 10, "Tokamak": 3 }
  }
}
```

Use it to watch a scripted AI's economy and force composition over a run without reading the log. It's
a snapshot, not history — if you want a time series, have your tool sample the file each mark and record
the `mark` field.

---

## Crashes

A native crash is split so the summary and the detail don't both clutter the same file:

- **Main log** gets one line: `Mission CRASHED: EXCEPTION_ACCESS_VIOLATION at 0047000A … - full dump in the mission log`.
- **Mission log** gets the full dump: exception, faulting address, CPU registers, and the symbolized
  native stack trace.

So when something dies, glance at `OP2Lua.log` to see *that* it crashed, then open the mission's own
log for the registers/stack. Script errors (a Lua error in `on_init`, a callback, etc.) also go to the
Mission log.

A fault inside a game call a script makes isn't a crash: it is a script error,
`GuardTest.lua:29: game call faulted: access violation at 0x0047613C reading 0x83C07FD8`, in the Mission log, with
no dump, and the game carries on.

Nor is a call that never returns: past 25 million Lua instructions in one call (API.md, *Callbacks*) the call is
dropped and the game carries on. The Mission log names it as an error, a scheduled function with where it was
defined:

```
16:46:29 m1     [ERROR] every() function defined at RunawayTest.lua:20 dropped: RunawayTest.lua:7: the mission's script ran more than 25 million instructions in one call (an endless loop?); the call was dropped
```

and the Main log has a warning with the line it was running:
`[WARN] a mission call ran more than 25 million Lua instructions (at RunawayTest.lua:7); dropping it`.

---

## Saved games

A saved game keeps the mission's script (0.8.8; FILE-FORMAT.md, *Saved games*). Each save and load leaves a
`saved game:` line in the Mission log, so `grep "saved game"` shows what happened to the script:

```
10:09:55 m50    [INFO] saved game: the script's state saved (33177 bytes, tick 5000)
10:17:53 m80    [INFO] saved game: loading at tick 8000 (mark 80), saved by OP2Lua 0.8.8
10:17:53 m80    [INFO] saved game: the script carries on (81256 bytes)
```

After `the script carries on`, the script's own lines go on from the saved tick, and `on_load_save` runs on the first
tick. A load starts a new section of the Mission log, with the usual header line.

When the script can't carry on, the line says why. The player gets a message too: at the save, one line; at a load
that stops the script (the second and third rows), `GAME LOAD MISSION FAIL` and a line saying why, on the first
tick and again 75 ticks later (with the chat line sound).

| Mission log | What happened |
|---|---|
| `[ERROR] saved game: the script's state can't be saved: <why> (at G.words.iterator). The game is saved without it; loading it stops the mission's script.` | At the save: the script held a value a save can't keep. `at` names it from the globals (`G`): here the global `words`, its field `iterator`. The game is still saved. |
| `[ERROR] saved game: the script's state couldn't be put back: <why>. The mission's script doesn't run in this game.` | At the load. `<why>` is e.g. *the save was made with another version of the mission script* (the `.lua` changed since the save), or the reason the save gave when it couldn't keep the state. |
| `[WARN] saved game: saved before OP2Lua kept a mission's script state in the save (0.8.8); the mission's script doesn't run in this game` | A save made with OP2Lua 0.8.7 or older. |
| `[WARN] saved game: no script was running when it was saved` | The script had failed to load in the game that saved. No message for the player. |
| `[INFO] saved game: 2 victory/failure conditions lost their text` | After a load that stops the script, or of a game saved with none running: the objectives' texts were the script's, so they now read *Objective unknown: the mission's script is stopped*. |

The Main log notes once per game session whether saved games keep the script:
`saved games: hooks installed (a Lua mission's script state goes in the save)`, or a warning that this Outpost2.exe
isn't the 1.2.0.7 the core knows, in which case a saved game doesn't keep the script's state.

---

## Quick reference

```lua
-- AI behaviour  -> OP2Lua-<dll>-AI.log
log.ai("AI: 2-bar mine online - trucks routed")
log.ai(string.format("[mk550] mark %d: %d VFs building RPG Lynx", game.mark(), n))

-- Mission/debug -> OP2Lua-<dll>.log
print(string.format("[starship] %s: %d/%d evacuated", name, have, target))

-- Player-facing -> comms panel + OP2Lua-<dll>.log
game.message("The volcano ERUPTS! Relocate your colony!")

-- Watch the AI in-game (off by default)
log.show_ai_in_game(true)
```

| You want… | Open |
|---|---|
| What the framework did / that it crashed | `OP2Lua.log` |
| What your mission did + the full crash dump | `OP2Lua-<dll>.log` |
| Only the scripted AI | `OP2Lua-<dll>-AI.log` |
| What a save or load did to the script | `OP2Lua-<dll>.log`, the `saved game:` lines |

Requires OP2LuaCore **0.6.7+** (the `log` table + AI log landed in 0.6.6; `log.ai_status` in 0.6.7). See
[`API.md`](API.md) for the rest of the scripting API.
