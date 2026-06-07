// LuaMission.cpp - the per-mission stub DLL (template).
//
// This is the tiny binary OP2 actually discovers and loads as a "mission". It exports the
// standard OP2 mission interface and forwards every call into OP2LuaCore.dll, which runs the
// mission's Lua. It carries NO Lua/sol2/Tethys runtime itself.
//
// The DescBlock / LevelDesc / MapName / TechtreeName below are PLACEHOLDERS. The converter
// (OP2OpmTools) clones this DLL and byte-patches those bytes from the .opm metadata so OP2's
// static mission-list scan shows the right name/map/players. The code is identical for every
// mission; only that data differs.
//
// On load, the stub locates its own folder (where its mission.lua / placement.lua live) and
// passes it to the core.

#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Tethys/API/Mission.h"   // mission interface signatures (header-only)
#include "Tethys/API/Unit.h"
#include "OP2LuaCore.h"

using namespace Tethys::TethysAPI;
using Tethys::ibool;

// ---- Mission metadata exports (PATCHED PER-MISSION by the converter) ------------------------
MISSION_API char      LevelDesc[1024]    = "OP2Lua mission (unpatched template)";
MISSION_API char      MapName[1024]      = "eden01.map";
MISSION_API char      TechtreeName[1024] = "MULTITEK.TXT";
MISSION_API ModDesc   DescBlock          = ModDesc(MissionType::Colony, 2, 12, 0);  // numPlayers patched per-mission by the converter
MISSION_API ModDescEx DescBlockEx        = {};

// ---- Stub plumbing --------------------------------------------------------------------------
namespace {

HINSTANCE ghInstance = nullptr;
bool      gLoaded    = false;

std::string ToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

// Ensure the core has loaded this mission. Derives the folder AND this DLL's base name from its own
// path (GetModuleFileName), so e.g. StrikeTeam.dll loads StrikeTeam.lua sitting next to it.
void EnsureLoaded() {
  if (gLoaded) return;
  wchar_t pathW[MAX_PATH] = {};
  GetModuleFileNameW(ghInstance, pathW, MAX_PATH);
  std::wstring full(pathW);

  const size_t slash = full.find_last_of(L"\\/");
  const std::wstring dirW  = (slash != std::wstring::npos) ? full.substr(0, slash) : L".";
  std::wstring       fileW = (slash != std::wstring::npos) ? full.substr(slash + 1) : full;
  const size_t dot = fileW.find_last_of(L'.');
  if (dot != std::wstring::npos) fileW.resize(dot);  // strip ".dll" -> base name

  OP2LuaCore_LoadMission(ToUtf8(dirW).c_str(), ToUtf8(fileW).c_str());
  gLoaded = true;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, void*) {
  if (reason == DLL_PROCESS_ATTACH) {
    ghInstance = hInst;
    DisableThreadLibraryCalls(hInst);
  }
  return TRUE;
}

// ---- Mission entry points: forward to OP2LuaCore.dll -----------------------------------------
MISSION_API ibool InitProc()                          { EnsureLoaded(); return OP2LuaCore_InitProc(); }
MISSION_API void  AIProc()                            { OP2LuaCore_AIProc(); }
MISSION_API void  GetSaveRegions(SaveRegion* p)       { OP2LuaCore_GetSaveRegions(p); }
MISSION_API ibool OnLoadMission(OnLoadMissionArgs* a) { EnsureLoaded(); return OP2LuaCore_OnLoadMission(a); }
MISSION_API ibool OnUnloadMission(OnUnloadMissionArgs* a) { return OP2LuaCore_OnUnloadMission(a); }
MISSION_API void  OnEndMission(OnEndMissionArgs* a)   { OP2LuaCore_OnEndMission(a); }
MISSION_API ibool OnSaveGame(OnSaveGameArgs* a)       { return OP2LuaCore_OnSaveGame(a); }
MISSION_API ibool OnLoadSavedGame(OnLoadSavedGameArgs* a) { return OP2LuaCore_OnLoadSavedGame(a); }
MISSION_API void  OnChat(OnChatArgs* a)               { OP2LuaCore_OnChat(a); }
MISSION_API void  OnCreateUnit(OnCreateUnitArgs* a)   { OP2LuaCore_OnCreateUnit(a); }
MISSION_API void  OnDestroyUnit(OnDestroyUnitArgs* a) { OP2LuaCore_OnDestroyUnit(a); }
MISSION_API void  OnDamageUnit(OnDamageUnitArgs* a)   { OP2LuaCore_OnDamageUnit(a); }
MISSION_API void  OnTrigger(OnTriggerArgs* a)         { OP2LuaCore_OnTrigger(a); }

// A no-op trigger function that the runtime's internal victory/failure triggers reference by name,
// so OP2's trigger-function resolution succeeds. Takes no args -> safe however OP2 calls it.
MISSION_API void  OP2LuaTrigger()                     { }
