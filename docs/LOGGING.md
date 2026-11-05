# OP2Lua Logging & Debug — the standard for every mission

This is the convention **every** OP2Lua mission should follow. It keeps the right information in the
right place so you can debug a mission, a scripted AI, or a crash without wading through noise.

OP2Lua writes up to **three log files** per run (in `Outpost2\OPU\logs\`) and can also echo a scripted
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
| `OP2Lua.log` | **Main** | The framework's own record across all missions — mission load/end, core version, map, and a **one-line crash summary**. You don't write to this from Lua. |
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
player and the deferred move/attack order flush (`issued N queued unit order(s)`) are written to the
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

Requires OP2LuaCore **0.6.7+** (the `log` table + AI log landed in 0.6.6; `log.ai_status` in 0.6.7). See
[`API.md`](API.md) for the rest of the scripting API.
