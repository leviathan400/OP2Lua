// LuaHost.cpp - owns the embedded Lua interpreter for the current mission.
//
// Creates a FRESH lua_State per mission (so every mission starts from a clean global environment),
// opens the safe standard libraries, installs the game bindings (Bindings.cpp), and loads the
// scheduler preamble (after/every/when + the tick pump). RunFile() runs a mission script and
// reports Lua errors without letting them reach the game's C code.

#include "LuaHost.h"

#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Log.h"
#include "Mission.h"

namespace OP2Lua {

LuaHost& LuaHost::Get() {
  static LuaHost* instance = new LuaHost();
  return *instance;
}

void LuaHost::Init() {
  // Recreate a fresh state on each call so every mission gets a clean global environment
  // (no leftover globals/callbacks from a previously played mission). Callers must release any
  // outstanding sol references (Mission::Reset) before calling this.
  lua_ = sol::state();

  // Open the standard libraries we consider safe.
  // Phase 6 (multiplayer hardening) will trim this further - in particular `io`, `os`,
  // `package`/`require` of arbitrary files, and `math.random*` must be removed so missions
  // cannot introduce desync sources. (See ROADMAP.md.)
  lua_.open_libraries(
      sol::lib::base,
      sol::lib::string,
      sol::lib::table,
      sol::lib::math);

  RegisterBindings(lua_);

  // Runtime preamble: the tick-based scheduler (after/every/when), the time helpers
  // (game.minutes/seconds), and the pump that AIProc drives. Tick-based => multiplayer-safe.
  static const char* kPreamble = R"LUA(
local _timers, _whens = {}, {}
-- Timers / conditions registered *during* a pump (i.e. from inside a firing callback - e.g. an
-- after() that schedules the next after()) are buffered here and merged in once the pump finishes.
-- Without this, the pump's wholesale list rebuild at the end would discard them, so a fired step
-- could never schedule the next step. _pumping selects which list new registrations land in.
local _addTimers, _addWhens = {}, {}
local _pumping = false
local function _pushTimer(e) local t = _pumping and _addTimers or _timers;  t[#t + 1] = e;  return e end
local function _pushWhen(w)  local t = _pumping and _addWhens  or _whens;   t[#t + 1] = w;  return w end

-- Logging. Three channels, each to its own file (all stamped with time + game mark):
--   print(...) / log.mission(...) -> the Mission log (OP2Lua-<name>.log): mission events.
--   log.ai(...)                   -> the AI log (OP2Lua-<name>-AI.log): a scripted AI's debug stream,
--                                    plus the in-game comms panel when log.show_ai_in_game(true).
--   log.show_ai_in_game(on)       -> toggle echoing log.ai lines to the comms panel (off by default).
-- (The framework's own Main log, OP2Lua.log, records mission load/end/crashes - not written from Lua.)
local function _concat(...)
  local p = {}
  for i = 1, select('#', ...) do p[i] = tostring(select(i, ...)) end
  return table.concat(p, '\t')
end
print = function(...) __op2lua_log(_concat(...)) end
log = {
  mission         = function(...) __op2lua_log(_concat(...)) end,
  ai              = function(...) __op2lua_log_ai(_concat(...)) end,
  show_ai_in_game = function(on)  __op2lua_ai_to_comms(on and true or false) end,
  -- log.ai_status(player): overwrite the AI status snapshot file (OP2Lua-<name>-AI-status.json) with
  -- the player's current resources + unit counts. Call from every(marks(1), ...) for a live feed.
  ai_status       = function(player) __op2lua_ai_status(player) end,
}

function after(ticks, fn)
  return _pushTimer({ at = game.tick() + ticks, fn = fn })
end

function every(ticks, fn)
  local e = { at = game.tick() + ticks, every = ticks, fn = fn, cancelled = false }
  _pushTimer(e)
  return { cancel = function() e.cancelled = true end }
end

function when(pred, fn)
  _pushWhen({ pred = pred, fn = fn })
end

-- Absolute scheduling: fire once when the game reaches a given tick / mark (vs after()'s relative).
function at(tick, fn) after(tick - game.tick(), fn) end
function at_mark(m, fn) at(m * 100, fn) end

-- Game time is measured in MARKS (the unit shown in-game; 1 mark = 100 ticks). Prefer marks for ALL
-- scheduling: at_mark(m, fn) fires at absolute mark m; marks(n) converts n marks to ticks for the
-- relative/repeating timers, e.g. after(marks(3), fn) or every(marks(120), fn).
function marks(n) return n * 100 end
function game.mark() return game.tick() // 100 end

-- Legacy time helpers (DEPRECATED - use marks()/at_mark instead; kept for backward compatibility).
function game.minutes(n) return n * game.ticks_per_minute end
function game.seconds(n) return n * game.ticks_per_second end

-- Run fn, reporting (never raising) any error. The log call is itself guarded so a broken
-- print() can never abort the scheduler.
local function safe(fn)
  local ok, err = pcall(fn)
  if not ok then pcall(function() print('[mission error] ' .. tostring(err)) end) end
end

-- Driven once per AIProc tick. Deterministic iteration order (arrays) => multiplayer-safe.
function __op2lua_pump()
  local now = game.tick()
  _pumping = true
  local keep = {}
  for _, e in ipairs(_timers) do
    if not e.cancelled then
      if now >= e.at then
        safe(e.fn)
        if e.every and not e.cancelled then e.at = e.at + e.every; keep[#keep + 1] = e end
      else
        keep[#keep + 1] = e
      end
    end
  end
  _timers = keep

  local wkeep = {}
  for _, w in ipairs(_whens) do
    local ok, fired = pcall(w.pred)
    if ok and fired then safe(w.fn) else wkeep[#wkeep + 1] = w end
  end
  _whens = wkeep

  -- Merge anything a callback registered during this pump (after both loops, so a when-callback can
  -- schedule a timer and have it live this same tick), then reset the buffers.
  for _, e in ipairs(_addTimers) do _timers[#_timers + 1] = e end
  for _, w in ipairs(_addWhens) do _whens[#_whens + 1] = w end
  _addTimers, _addWhens, _pumping = {}, {}, false
end
)LUA";
  sol::protected_function_result r = lua_.safe_script(kPreamble, &sol::script_pass_on_error);
  if (!r.valid()) {
    sol::error e = r;
    LOG(ERROR) << "Failed to load OP2Lua runtime preamble: " << e.what();
  }

  initialized_ = true;
  LOG(INFO) << "Lua host initialized (" << LUA_RELEASE << ")";
}

void LuaHost::Shutdown() {
  if (!initialized_) return;
  Mission::Reset();     // release mission callback refs while the state is still alive
  lua_ = sol::state();  // destroy and replace with a fresh empty state
  initialized_ = false;
  LOG(INFO) << "Lua host shut down";
}

bool LuaHost::RunFile(const std::string& path) {
  sol::protected_function_result result = lua_.safe_script_file(
      path, &sol::script_pass_on_error);

  if (!result.valid()) {
    sol::error err = result;
    std::string msg = "Error running ";
    msg += path;
    msg += ":\n";
    msg += err.what();
    LOG(ERROR) << msg;
    MessageBoxA(nullptr, msg.c_str(), "OP2Lua", MB_ICONEXCLAMATION | MB_SYSTEMMODAL);
    return false;
  }
  return true;
}

}  // namespace OP2Lua
