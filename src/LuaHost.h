#pragma once

#include <string>

// sol2 - header-only C++ <-> Lua bindings. See third_party/README.md for how to add it.
#include <sol/sol.hpp>

namespace OP2Lua {

// Owns the embedded Lua interpreter and the bindings to the game.
//
// There is a single shared state for the running mission. (OP2 runs one mission at a time.)
// Phase 6 will sandbox this state for multiplayer determinism; for now it opens the standard
// safe libraries only.
class LuaHost {
 public:
  static LuaHost& Get();

  // Create the Lua state, open libraries, install the game bindings. Idempotent.
  void Init();
  // Destroy the Lua state.
  void Shutdown();

  sol::state& lua() { return lua_; }

  // Run a Lua script file in the main state.
  // Returns true on success. On a Lua error it logs the traceback and shows a message box,
  // and returns false - it never lets the error escape into the game's C/C++ code.
  bool RunFile(const std::string& path);

 private:
  sol::state lua_;
  bool initialized_ = false;
};

// Installs the author-facing API (game.*, players, units, scheduler, ...). Defined in Bindings.cpp.
void RegisterBindings(sol::state& lua);

// The current game mark (100 ticks), for the logger's per-line stamp. Defined in Bindings.cpp (which
// can include Game.h cleanly); installed via Log's SetMarkProvider during core init.
int CurrentMarkForLog();

}  // namespace OP2Lua
