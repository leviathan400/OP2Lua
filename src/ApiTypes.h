#pragma once

// Lua-facing wrapper types for data passed into mission callbacks.
// These are thin views over the Tethys callback arg structs; they are constructed on the stack
// for the duration of a single callback and handed to Lua by reference.

#include <string>

#include "Tethys/API/Mission.h"  // Tethys::TethysAPI::OnChatArgs

namespace OP2Lua {

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

// Unit orders (move/attack_move) are queued and issued one tick LATER. A freshly-created unit
// cannot be commanded the same tick it's created (OP2 hasn't finished registering it -> crash).
// FlushUnitOrders() is called at the start of each scheduler pump; ResetUnitOrders() on load.
void FlushUnitOrders();
void ResetUnitOrders();

// Triggers made from Lua (trigger.* with an `on=` function) name the stub's OnTrigger export; OPU passes
// it the trigger that fired, and FireTrigger runs that trigger's Lua function. ResetTriggers() on load
// (while the old Lua state is still alive: the functions belong to it).
void FireTrigger(void* pArgs);
void ResetTriggers();

}  // namespace OP2Lua
