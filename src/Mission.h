#pragma once

// Mission runtime (OP2LuaCore). The exported entry points are the OP2LuaCore_* C API in
// OP2LuaCore.h; this header just exposes the teardown used by the Lua host.

namespace OP2Lua::Mission {

// Clear the active mission and release its callback + placement references. Must run while the
// Lua state is still alive (callbacks hold refs into it).
void Reset();

}  // namespace OP2Lua::Mission
