#pragma once

// Lua-facing wrapper types for data passed into mission callbacks.
// These are thin views over the Tethys callback arg structs; they are constructed on the stack
// for the duration of a single callback and handed to Lua by reference.

#include <string>

#include "Tethys/API/Mission.h"  // Tethys::TethysAPI::OnChatArgs

struct lua_State;

namespace OP2Lua {

namespace Persist { class Writer; class Reader; }

// Wraps OnChatArgs so a mission can read and rewrite the chat text:  function on_chat(chat) ...
//   chat.text   - get/set (writing back into the game's buffer, length-clamped)
//   chat.player  - source player number (read-only)
struct ChatArgs {
  Tethys::TethysAPI::OnChatArgs* p = nullptr;

  std::string GetText() const;
  void        SetText(const std::string& text);
  int         GetPlayer() const;
};

// The Lua handle for players[id]. Holds the OP2 (0-based) player number so the bound methods can
// construct _Player(num) and the per-player enumerators. (_Player itself doesn't expose its id.)
struct LuaPlayer {
  int num = 0;  // OP2 0-based player number
};

// The Lua handle for regions["name"] - a tile rectangle (a point is just x1==x2, y1==y2).
// Methods (bound in Bindings): :units(player?), :contains(unit), .center
struct Region {
  int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

// Unit orders (move/attack_move/attack) are given at once; one a unit can't take yet is queued and retried.
// FlushUnitOrders() is called at the start of each scheduler pump; ResetUnitOrders() when a mission is let go.
void FlushUnitOrders();
void ResetUnitOrders();
// Stops a WAV that game.play_wav started (the mission is unloading).
void StopMissionWav();

// Triggers made from Lua (trigger.* with an `on=` function) name the stub's OnTrigger export; OPU passes
// it the trigger that fired, and FireTrigger runs that trigger's Lua function. ResetTriggers() when a mission is let go
// (while the old Lua state is still alive: the functions belong to it).
void FireTrigger(void* pArgs);
void ResetTriggers();

// Saved games (SaveGame.cpp): the bindings' own state - the condition texts, the queued unit orders, the AI comms
// switch - and the triggers' functions as a Lua table (trigger id -> function). RestoreBindingsState runs once the game
// has loaded its triggers: it points each loaded condition at its text again.
void SaveBindingsState(Persist::Writer& w);
bool RestoreBindingsState(Persist::Reader& r, std::string& err);
void ResetObjectives();
// A load that stops the script: every victory and failure condition still points where its text was in the game
// that saved; LoseObjectiveTexts points them all at one text that says so.
void LoseObjectiveTexts();
void PushTriggerFunctions(lua_State* L);
void SetTriggerFunctions(lua_State* L, int idx);

}  // namespace OP2Lua
