#pragma once

// C API exported by OP2LuaCore.dll (the shared Lua runtime) and called by each per-mission stub
// DLL. The stub owns the OP2 mission interface (DescBlock, InitProc, On*, ...) and forwards every
// call here; this library does the actual Lua work. Args are passed as void* (the Tethys arg
// structs) and cast back inside the core.

#define OP2LUA_VERSION "0.6.12"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef OP2LUACORE_EXPORTS
#define OP2LUACORE_API __declspec(dllexport)
#else
#define OP2LUACORE_API __declspec(dllimport)
#endif

// Load a mission. `missionDir` is the folder containing the scripts; `baseName` is the stub DLL's
// base filename (e.g. "StrikeTeam"). The runtime loads `<dir>/<baseName>.lua` (+ optional
// `<dir>/<baseName>.placement.lua`), falling back to `<dir>/mission.lua` (+ `placement.lua`) if the
// named files are absent. This lets several mission DLLs share one folder, each with its own
// scripts. Initializes the core on first use; recreates a fresh Lua state each call.
OP2LUACORE_API void OP2LuaCore_LoadMission(const char* missionDir, const char* baseName);

// Mission entry points (mirror the OP2 mission API). The stub forwards to these.
OP2LUACORE_API int  OP2LuaCore_InitProc(void);
OP2LUACORE_API void OP2LuaCore_AIProc(void);
OP2LUACORE_API void OP2LuaCore_GetSaveRegions(void* pSaveRegion);
OP2LUACORE_API int  OP2LuaCore_OnLoadMission(void* pArgs);
OP2LUACORE_API int  OP2LuaCore_OnUnloadMission(void* pArgs);
OP2LUACORE_API void OP2LuaCore_OnEndMission(void* pArgs);
OP2LUACORE_API int  OP2LuaCore_OnSaveGame(void* pArgs);
OP2LUACORE_API int  OP2LuaCore_OnLoadSavedGame(void* pArgs);
OP2LUACORE_API void OP2LuaCore_OnChat(void* pArgs);
OP2LUACORE_API void OP2LuaCore_OnCreateUnit(void* pArgs);
OP2LUACORE_API void OP2LuaCore_OnDestroyUnit(void* pArgs);
OP2LUACORE_API void OP2LuaCore_OnDamageUnit(void* pArgs);
OP2LUACORE_API void OP2LuaCore_OnTrigger(void* pArgs);

#ifdef __cplusplus
}
#endif
