// Mission.cpp - the OP2LuaCore mission runtime.
//
// Implements the OP2LuaCore_* C API (OP2LuaCore.h): load a mission folder's Lua, resolve its
// on_* callbacks, and run them when the per-mission stub DLL forwards the game's mission calls.
// All callbacks run inside protected calls so a script bug can never crash or desync the game.

#include "OP2LuaCore.h"  // OP2LUACORE_EXPORTS is defined by the build for this target

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <string>
#include <system_error>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>

#include "ApiTypes.h"
#include "Log.h"
#include "LuaHost.h"
#include "Mission.h"
#include "Placement.h"
#include "ErrorHandling.h"
#include "Guard.h"
#include "Hackaray.h"
#include "Persist.h"
#include "SaveGame.h"

#include "Tethys/API/Mission.h"
#include "Tethys/Game/GameStartInfo.h"   // MissionResults (OnEndMission)
#include "Tethys/Game/GameImpl.h"        // the game's end reason
#include "Tethys/Game/MissionManager.h"  // the mission DLL the game loaded
#include <algorithm>
using std::max;   // Tethys's headers use unqualified min()/max() (NOMINMAX build)
using std::min;
#include "Tethys/API/Game.h"             // Game::AddMessage (a saved game's notice)
#include "Tethys/API/Unit.h"

namespace fs = std::filesystem;
using namespace Tethys::TethysAPI;
using Tethys::ibool;

namespace OP2Lua::Mission {
namespace {

struct MissionState {
  bool active = false;
  sol::protected_function onInit, onTick, onChat, onCreateUnit, onDestroyUnit, onDamageUnit,
                          onLoad, onUnload, onEnd, onSave, onLoadSave;
  sol::protected_function pump;  // __op2lua_pump (scheduler), from the runtime preamble
};
MissionState g;
std::string gDir;   // the running mission's folder (Dir())
std::string gBase;  // the running mission DLL's name without .dll (its log and result file names)
bool gPrepared = false;       // the logs point at the mission DLL the game loaded last (GetSaveRegions)
bool gStarted = false;        // the script has run for this mission DLL (InitProc, or a saved game's restore)
bool gLoadedSave = false;     // a saved game was put back; its first tick calls on_load_save
std::string gLoadNotice;      // why a loaded game's script is stopped, for the player (NoticeAfterLoad)
int gLoadNoticeRepeat = -1;   // the tick to post gLoadNotice again, or -1
constexpr const char* kLoadFailTitle = "GAME LOAD MISSION FAIL";
constexpr int kLoadNoticeRepeatTicks = 75;

void ClearLoadNotice() {
  gLoadNotice.clear();
  gLoadNoticeRepeat = -1;
}

void LogCbError(const char* cb, const std::string& what) {
  // A script error is a mission event - send it to the mission log, not the framework Main log.
  LOG_MISSION(ERROR) << "Unhandled Lua error in mission " << cb << "(): " << what;
}

// Case-insensitive string equality (map filenames may differ only in case, e.g. Eden01 vs eden01).
bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

sol::protected_function Resolve(sol::state& L, const char* name) {
  sol::object o = L[name];
  return o.is<sol::protected_function>() ? o.as<sol::protected_function>() : sol::protected_function();
}

template <typename... Args>
void DispatchVoid(sol::protected_function& fn, const char* name, Args&&... args) {
  if (!g.active || !fn.valid()) return;
  Guard::Scope guard;   // a fault in a game call ends this callback as a Lua error
  Budget::Call budget;  // so does running past the instruction limit
  try {
    sol::protected_function_result r = fn(std::forward<Args>(args)...);
    if (!r.valid()) { sol::error e = r; LogCbError(name, e.what()); }
  } catch (const std::exception& e) { LogCbError(name, e.what()); }
    catch (...) { LogCbError(name, "unknown C++ exception"); }
}

template <typename... Args>
bool DispatchBool(sol::protected_function& fn, const char* name, bool dflt, Args&&... args) {
  if (!g.active || !fn.valid()) return dflt;
  Guard::Scope guard;
  Budget::Call budget;
  try {
    sol::protected_function_result r = fn(std::forward<Args>(args)...);
    if (!r.valid()) { sol::error e = r; LogCbError(name, e.what()); return dflt; }
    if (r.return_count() < 1) return dflt;
    sol::object ret = r.get<sol::object>(0);
    if (!ret.valid() || ret == sol::lua_nil) return false;
    if (ret.is<bool>()) return ret.as<bool>();
    return true;
  } catch (const std::exception& e) { LogCbError(name, e.what()); return dflt; }
    catch (...) { LogCbError(name, "unknown C++ exception"); return dflt; }
}

// One-time core initialization (master log + crash reporter + pin). Called on first mission load.
void EnsureCoreInit() {
  static bool inited = false;
  if (inited) return;
  inited = true;
  std::error_code ec;
  fs::create_directories("logs", ec);          // cwd is Outpost2\OPU at runtime
  InitMainLog("logs/OP2Lua.log", false);       // the persistent framework record
  ErrorHandling::Enable();
  SetMarkProvider(&CurrentMarkForLog);          // stamp every log line with the game mark

  // Pin this DLL for the process lifetime. When OP2 ends a mission it frees the stub DLL, which
  // would otherwise drop this shared core's refcount to zero and unload it mid-game (crashing in
  // FreeModuleDesc). Pinning keeps it resident across missions.
  HMODULE pinned = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                     reinterpret_cast<LPCWSTR>(&EnsureCoreInit), &pinned);
}

// Log the running core's version + when the DLL was built - the ground truth for "which build is this".
// Logged once per mission load.
void LogBuildStamp() {
  LOG(INFO) << "OP2LuaCore " OP2LUA_VERSION " (DLL built " << BuildTime() << ")";
}

// Log which .map the mission actually loads. OP2 reads the map filename from the stub DLL's exported
// MapName[] *before* any Lua runs, so THIS - not placement.lua's `map=` field, which is author
// metadata only and never drives loading - is the ground truth. Logging it makes a stale bake
// obvious (e.g. placement.lua says eden01.map but the DLL still forces newworld.map: re-patch the
// DLL to change the map). `base` is the stub's own filename, so <base>.dll is loaded in-process.
void LogMissionMap(const std::string& base) {
  if (base.empty()) return;
  const std::string dll = base + ".dll";
  const HMODULE h = GetModuleHandleA(dll.c_str());
  if (h == nullptr) {
    LOG(INFO) << "Mission map: <could not locate stub '" << dll << "' in process>";
    return;
  }
  const auto* mapName   = reinterpret_cast<const char*>(GetProcAddress(h, "MapName"));
  const auto* levelDesc = reinterpret_cast<const char*>(GetProcAddress(h, "LevelDesc"));
  const std::string baked = (mapName != nullptr) ? mapName : "";
  LOG(INFO) << "Mission map: " << (baked.empty() ? "<MapName export missing>" : baked)
            << " (baked into " << dll << "; LevelDesc \"" << (levelDesc ? levelDesc : "?") << "\")";

  // Cross-check placement.lua's `map=` field. That field is author metadata only and never drives
  // loading (OP2 reads MapName from the DLL above), so if the two disagree the DLL silently wins -
  // warn loudly, because editing only placement.lua looks like it should change the map but doesn't.
  const std::string declared = Placement::DeclaredMap();
  if (!declared.empty() && !baked.empty() && !EqualsIgnoreCase(declared, baked)) {
    LOG(WARNING) << "Map mismatch: placement.lua declares \"" << declared << "\" but " << dll
                 << " bakes \"" << baked << "\". The DLL wins - re-patch the DLL to change the map.";
  }
}

// Filename-safe form of the stub base name (keep alnum / '-' / '_').
std::string CleanBase(const std::string& base) {
  std::string clean;
  for (char c : base) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') clean += c;
  }
  return clean;
}

// Per-mission log file names: logs/OP2Lua-<base>.log (events) and logs/OP2Lua-<base>-AI.log (AI debug),
// so each mission DLL gets its own logs. Empty base -> no per-mission file (everything goes to Main).
std::string MissionLogPath(const std::string& base) {
  const std::string clean = CleanBase(base);
  return clean.empty() ? std::string() : ("logs/OP2Lua-" + clean + ".log");
}
std::string AILogPath(const std::string& base) {
  const std::string clean = CleanBase(base);
  return clean.empty() ? std::string() : ("logs/OP2Lua-" + clean + "-AI.log");
}
std::string AIStatusPath(const std::string& base) {
  const std::string clean = CleanBase(base);
  return clean.empty() ? std::string() : ("logs/OP2Lua-" + clean + "-AI-status.json");
}
std::string ResultPath(const std::string& base) {
  const std::string clean = CleanBase(base);
  return clean.empty() ? std::string() : ("logs/OP2Lua-" + clean + "-result.json");
}

// The game's GameTermReasons as a word: single player won/lost/aborted, multiplayer victory/defeat/terminated.
const char* ResultName(int reason) {
  switch (reason) {
    case 3: return "won";
    case 4: return "lost";
    case 6: return "aborted";
    case 7: return "victory";
    case 8: return "defeat";
    case 9: return "terminated";
    default: return "other";
  }
}

// The mission's end, recorded once, from whichever comes first: the game's own end reason as a tick sees it, the
// MissionResults OPU passes OnEndMission, or the end reason when the game unloads the mission. Written to
// OPU\logs\OP2Lua-<dll>-result.json (overwritten each end, so a launcher can read how the mission ended) and given to
// on_end as a table.
struct EndResult { int reason = 0, tick = 0, localPlayer = 0; };
bool gEnded = false;

void FinishMission(const EndResult& e, const char* source, bool callLua = true) {
  if (gEnded) return;
  gEnded = true;
  const char* result = ResultName(e.reason);
  const std::string path = ResultPath(gBase);
  if (!path.empty()) {
    char when[32] = "";
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (localtime_s(&local, &now) == 0) std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S", &local);
    if (FILE* f = std::fopen(path.c_str(), "w")) {
      std::fprintf(f,
                   "{\n  \"mission\": \"%s.dll\",\n  \"result\": \"%s\",\n  \"reason\": %d,\n  \"tick\": %d,\n"
                   "  \"mark\": %d,\n  \"local_player\": %d,\n  \"time\": \"%s\"\n}\n",
                   CleanBase(gBase).c_str(), result, e.reason, e.tick, e.tick / 100, e.localPlayer, when);
      std::fclose(f);
    } else {
      LOG(WARNING) << "could not write " << path;
    }
  }
  LOG_MISSION(INFO) << "mission ended: " << result << " (reason " << e.reason << ", tick " << e.tick << ", seen by "
                    << source << ")";
  if (!callLua || !g.active || !g.onEnd.valid()) return;
  sol::table t = LuaHost::Get().lua().create_table();
  t["result"] = result;
  t["reason"] = e.reason;
  t["tick"] = e.tick;
  t["mark"] = e.tick / 100;
  t["local_player"] = e.localPlayer;
  DispatchVoid(g.onEnd, "on_end", t);
}

// The game's end reason now (GameImpl::gameTermReasons_); 0 while the game runs.
EndResult GameEnd() {
  EndResult e;
  if (auto* impl = Tethys::GameImpl::GetInstance()) {
    e.reason = static_cast<int>(impl->gameTermReasons_);
    e.tick = impl->tick_;
    e.localPlayer = impl->localPlayer_;
  }
  return e;
}

// When the game exits without telling the mission it ended (a game started by op2launcher closes from the end
// screen: no OnEndMission, no OnUnloadMission), the core's shutdown records the end. No Lua that late.
void FinishAtExit() {
  if (!gEnded && !gBase.empty()) FinishMission(GameEnd(), "the game's exit", false);
}

// Logs a mission event the game sent, with the end reason the game had then.
void NoteEndEvent(const char* name) {
  const EndResult e = GameEnd();
  LOG_MISSION(INFO) << name << " (end reason " << e.reason << ", tick " << e.tick << ")";
}

// The .map name baked into the stub DLL's MapName export (the ground truth - what OP2 actually loads).
std::string BakedMapName(const std::string& base) {
  if (base.empty()) return "";
  const HMODULE h = GetModuleHandleA((base + ".dll").c_str());
  if (h == nullptr) return "";
  const auto* mapName = reinterpret_cast<const char*>(GetProcAddress(h, "MapName"));
  return (mapName != nullptr) ? mapName : "";
}

// Returns `preferred` if it exists on disk, else `fallback`.
std::string PickExisting(const std::string& preferred, const std::string& fallback) {
  std::error_code ec;
  return fs::exists(preferred, ec) ? preferred : fallback;
}

// First of the candidates that exists on disk, else the last (the generic fallback).
std::string PickExisting(const std::string& a, const std::string& b, const std::string& fallback) {
  std::error_code ec;
  if (fs::exists(a, ec)) return a;
  if (fs::exists(b, ec)) return b;
  return fallback;
}

// OP2 lists a mission under "colony games" via a leading 'c' on the DLL filename - but the .lua scripts
// don't need that prefix. So for a stub named e.g. "cStrikeTeam" we also look for the unprefixed
// "StrikeTeam.{lua,placement.lua}". Only strips a 'c' followed by an uppercase letter (the CamelCase
// colony-naming convention), so an actual name like "convoy" is left alone.
std::string StripColonyPrefix(const std::string& base) {
  if ((base.size() >= 2) && ((base[0] == 'c') || (base[0] == 'C')) && (base[1] >= 'A') && (base[1] <= 'Z'))
    return base.substr(1);
  return base;
}

// Points the logs at a mission and forgets the previous one. The script runs later: at InitProc for a new game
// (StartScript), or not at all for a saved game, whose script state is put back (RestoreSave).
void PrepareMission(const std::string& dir, const std::string& base) {
  EnsureCoreInit();                      // one-time: main log + crash reporter + mark stamp + pin
  if (gPrepared && dir == gDir && base == gBase) return;   // the same DLL again (the stub's InitProc after GetSaveRegions)
  gPrepared = true;
  gDir = dir;
  gBase = base;
  gEnded = false;
  gStarted = false;
  gLoadedSave = false;
  ClearLoadNotice();
  static const bool atExitSet = (std::atexit(FinishAtExit), true);   // after the crash reporter's: runs before it
  (void)atExitSet;

  // Point the per-mission Mission + AI logs at this mission, with a self-identifying header on each
  // (name + baked map + core version) so neither file needs the framework lines duplicated into it.
  const std::string ver   = "OP2LuaCore " OP2LUA_VERSION;
  const std::string baked = BakedMapName(base);
  const std::string label = base.empty() ? std::string("mission") : base;
  std::string missionHeader = label + (baked.empty() ? "" : ("  map " + baked)) + "  (" + ver + ")";
  SetMissionLog(MissionLogPath(base), missionHeader);
  SetAILog(AILogPath(base), label + " scripted AI  (" + ver + ")");
  SetAIStatusPath(AIStatusPath(base));   // log.ai_status overwrites this snapshot file each call
  LogBuildStamp();                       // stamp version + DLL build time (-> Main log)
  Reset();                               // release any previous callbacks/placement (old state alive)
}

// The mission's scripts: <dir>/<base>.lua (+ <base>.placement.lua), else mission.lua (+ placement.lua).
std::string PlacementPath() {
  const std::string alt = StripColonyPrefix(gBase);   // cStrikeTeam -> StrikeTeam (scripts need no 'c')
  return PickExisting(gDir + "\\" + gBase + ".placement.lua", gDir + "\\" + alt + ".placement.lua",
                      gDir + "\\placement.lua");
}
std::string MissionLuaPath() {
  const std::string alt = StripColonyPrefix(gBase);
  return PickExisting(gDir + "\\" + gBase + ".lua", gDir + "\\" + alt + ".lua", gDir + "\\mission.lua");
}

// A fresh Lua state with the mission's placement read.
void NewState() {
  Reset();
  LuaHost::Get().Init();      // fresh Lua state + bindings for this mission
  Placement::Load(PlacementPath());
  LogMissionMap(gBase);   // baked .map (ground truth) + warn if placement.lua's map= disagrees
}

void ResolveCallbacks() {
  sol::state& L = LuaHost::Get().lua();
  g.onInit        = Resolve(L, "on_init");
  g.onTick        = Resolve(L, "on_tick");
  g.onChat        = Resolve(L, "on_chat");
  g.onCreateUnit  = Resolve(L, "on_create_unit");
  g.onDestroyUnit = Resolve(L, "on_destroy_unit");
  g.onDamageUnit  = Resolve(L, "on_damage_unit");
  g.onLoad        = Resolve(L, "on_load");
  g.onUnload      = Resolve(L, "on_unload");
  g.onEnd         = Resolve(L, "on_end");
  g.onSave        = Resolve(L, "on_save");
  g.onLoadSave    = Resolve(L, "on_load_save");
  g.pump          = Resolve(L, "__op2lua_pump");  // scheduler pump from the preamble
  g.active = true;
}

// A new game: run the mission's script (its top level), then on_load.
void StartScript() {
  if (gStarted) return;
  gStarted = true;
  ClearLoadNotice();
  NewState();
  const std::string missionLua = MissionLuaPath();
  if (!LuaHost::Get().RunFile(missionLua)) return;  // RunFile reports load errors
  ResolveCallbacks();
  LOG(INFO) << "Mission loaded: " << missionLua;
  DispatchBool(g.onLoad, "on_load", true);
}

// The mission DLL the game has loaded (MissionManager's module): its folder and name.
bool LoadedMissionDll(std::string& dir, std::string& base) {
  const auto* mm = Tethys::MissionManager::GetInstance();
  if (mm == nullptr || mm->hModule_ == nullptr) return false;
  wchar_t pathW[MAX_PATH] = {};
  if (GetModuleFileNameW(mm->hModule_, pathW, MAX_PATH) == 0) return false;
  const std::string full = fs::path(pathW).u8string();
  const size_t slash = full.find_last_of("\\/");
  dir = slash != std::string::npos ? full.substr(0, slash) : ".";
  base = slash != std::string::npos ? full.substr(slash + 1) : full;
  const size_t dot = base.find_last_of('.');
  if (dot != std::string::npos) base.resize(dot);
  return true;
}

// ---- saved games (SaveGame.cpp hooks the game's save and load) ----
// A save's region: "OP2LuaSv", the format, the core's version, the mission's name, whether a script was running,
// then the Lua values (Persist), the bindings' state and hackaray's.
constexpr char kSaveMagic[8] = { 'O', 'P', '2', 'L', 'u', 'a', 'S', 'v' };
constexpr uint32_t kSaveFormat = 1;

// Runs `body` in a protected call, so a Lua error inside it (out of memory) comes back as a message.
template <typename F>
bool Protected(F& body, std::string& err) {
  lua_State* L = LuaHost::Get().lua().lua_state();
  struct Ctx { F* body; std::string* err; bool ok; } ctx{ &body, &err, false };
  lua_pushcfunction(L, [](lua_State* S) -> int {
    auto* c = static_cast<Ctx*>(lua_touserdata(S, 1));
    c->ok = (*c->body)(S);
    return 0;
  });
  lua_pushlightuserdata(L, &ctx);
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    err = lua_isstring(L, -1) ? lua_tostring(L, -1) : "Lua error";
    lua_pop(L, 1);
    return false;
  }
  return ctx.ok;
}

// Tells the player a loaded game's script is stopped, on the first tick after the load (the game is running again)
// and once more 75 ticks later: kLoadFailTitle, then the reason. The second title plays a chat line's sound (message2,
// as the game posts an incoming chat line, 0x40FE14) so the player notices it. The game's message pane cuts a message
// off after about 90 characters, so the reasons are short; the Mission log has the whole one.
void NoticeAfterLoad(const std::string& text) {
  gLoadNotice = text;
  gLoadNoticeRepeat = -1;
}
}  // namespace

// What the state byte after the header says.
enum SavedScript : uint8_t { kNoScript = 0, kScriptState = 1, kScriptNotKept = 2 };

void WriteHeader(Persist::Writer& w, SavedScript what) {
  w.Bytes(kSaveMagic, sizeof kSaveMagic);
  w.U32(kSaveFormat);
  w.Str(OP2LUA_VERSION);
  w.Str(CleanBase(gBase));
  w.U8(what);
}

bool BuildSave(std::vector<char>& out, std::string& err) {
  Persist::Writer w;
  WriteHeader(w, g.active ? kScriptState : kNoScript);
  bool kept = g.active;
  if (g.active) {
    DispatchVoid(g.onSave, "on_save");
    auto body = [&](lua_State* L) {
      lua_createtable(L, 0, 2);
      lua_pushglobaltable(L);
      lua_setfield(L, -2, "globals");
      PushTriggerFunctions(L);
      lua_setfield(L, -2, "triggers");
      const bool ok = Persist::Save(L, -1, w, err);
      lua_pop(L, 1);
      return ok;
    };
    if (!Protected(body, err)) {
      // The game is half way through writing the file, so the save goes on, without the script's state: a load then
      // says why the script can't carry on.
      LOG_MISSION(ERROR) << "saved game: the script's state can't be saved: " << err
                         << ". The game is saved without it; loading it stops the mission's script.";
      Game::AddMessage("OP2Lua: the script couldn't be saved: a load stops it. See the OP2Lua log.",
                       static_cast<Tethys::SoundID>(0));
      w = Persist::Writer();
      WriteHeader(w, kScriptNotKept);
      kept = false;
      w.Str(err);
    } else {
      SaveBindingsState(w);
      Hackaray::SaveState(w);
    }
  }
  out.assign(w.out.begin(), w.out.end());
  if (kept)
    LOG_MISSION(INFO) << "saved game: the script's state saved (" << out.size() << " bytes, tick " << Game::Tick() << ")";
  return true;
}

void RestoreSave(const char* data, size_t size) {
  std::string dir, base;
  if (LoadedMissionDll(dir, base)) PrepareMission(dir, base);
  gStarted = true;   // the script's top level doesn't run again: its state comes from the save
  Persist::Reader r(data, size);
  char magic[sizeof kSaveMagic] = {};
  r.Bytes(magic, sizeof magic);
  const uint32_t format = r.U32();
  const std::string version = r.Str(), savedBase = r.Str();
  const uint8_t saved = r.U8();
  const auto fail = [&](const std::string& why) {
    LOG_MISSION(ERROR) << "saved game: the script's state couldn't be put back: " << why
                       << ". The mission's script doesn't run in this game.";
    NoticeAfterLoad(why.find("version of the mission script") != std::string::npos
                        ? "OP2Lua: saved with another version of this mission: its script is stopped."
                    : why.find("version of the OP2Lua runtime") != std::string::npos
                        ? "OP2Lua: saved with another OP2Lua version: the mission's script is stopped."
                        : "OP2Lua: this save's mission script couldn't carry on. See the OP2Lua log.");
    Reset();
    LoseObjectiveTexts();
  };
  if (!r.Ok() || std::memcmp(magic, kSaveMagic, sizeof magic) != 0) { fail("its data isn't OP2Lua's"); return; }
  if (format != kSaveFormat) { fail("it was saved by OP2Lua " + version + ", in a format this OP2Lua doesn't read"); return; }
  if (savedBase != CleanBase(gBase)) { fail("it belongs to mission " + savedBase); return; }
  LOG_MISSION(INFO) << "saved game: loading at tick " << Game::Tick() << " (mark " << Game::Tick() / 100
                    << "), saved by OP2Lua " << version;
  if (saved == kScriptNotKept) { fail("its state couldn't be saved: " + r.Str()); return; }
  if (saved != kScriptState) {
    // Saved after the script stopped (a failed load, say): the conditions' texts point where they were in the game
    // that saved, which may be nowhere now.
    Reset();
    LoseObjectiveTexts();
    LOG_MISSION(WARNING) << "saved game: no script was running when it was saved";
    return;
  }
  NewState();
  std::string err;
  if (!LuaHost::Get().LoadChunk(MissionLuaPath(), "mission script", err)) { fail(err); return; }
  lua_pop(LuaHost::Get().lua().lua_state(), 1);   // compiled for its functions' prototypes, not run
  auto body = [&](lua_State* L) {
    if (!Persist::Restore(L, r, err)) return false;
    lua_getfield(L, -1, "triggers");
    if (lua_istable(L, -1)) SetTriggerFunctions(L, -1);
    lua_pop(L, 2);
    return true;
  };
  if (!Protected(body, err)) { fail(err); return; }
  if (!RestoreBindingsState(r, err)) { fail(err); return; }
  Hackaray::RestoreState(r);
  ResolveCallbacks();
  gLoadedSave = true;
  LOG_MISSION(INFO) << "saved game: the script carries on (" << size << " bytes)";
}

void LoadedWithoutState() {
  std::string dir, base;
  if (LoadedMissionDll(dir, base)) PrepareMission(dir, base);
  gStarted = true;
  Reset();
  LoseObjectiveTexts();
  LOG_MISSION(WARNING) << "saved game: saved before OP2Lua kept a mission's script state in the save (0.8.8); the "
                          "mission's script doesn't run in this game";
  NoticeAfterLoad("OP2Lua: saved before OP2Lua 0.8.8: the mission's script is stopped.");
}

namespace {

}  // namespace

std::string Dir() { return gDir; }

std::string BuildTime() {
  HMODULE hSelf = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&BuildTime), &hSelf)) {
    return "unknown";
  }
  // The link time in the loaded image's PE header (the COFF TimeDateStamp, seconds since 1970 UTC): unlike the
  // file's modified time it stays the same when the DLL is copied, and it's what OP2LuaEditor shows.
  const auto* base = reinterpret_cast<const unsigned char*>(hSelf);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const __time64_t stamp = nt->FileHeader.TimeDateStamp;
  tm local = {};
  if (_localtime64_s(&local, &stamp) != 0) return "unknown";
  char text[32] = {};
  std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
  return text;
}

std::string GameVersion() {
  // TApp::Version, called on the game's TApp: the version the running game goes by, the one its lobby compares with a
  // joining player's ("wrong game version"). 0x01020007 is 1.2.0.7 (the GOG exe alone); OPU 1.4.x makes it 1.4.0.1.
  // Both are Outpost2.exe exports, found by name. TApp::Version is __thiscall with no arguments: a __fastcall
  // pointer passes `this` in ECX the same way (EDX is ignored).
  HMODULE exe = GetModuleHandleW(nullptr);
  using VersionFn = unsigned long(__fastcall*)(void* self, void* unused);
  const auto version = reinterpret_cast<VersionFn>(GetProcAddress(exe, "?Version@TApp@@QAEKXZ"));
  void* const app = reinterpret_cast<void*>(GetProcAddress(exe, "?gTApp@@3VTApp@@A"));
  if (version == nullptr || app == nullptr) return std::string();
  const unsigned long v = version(app, nullptr);
  char text[32] = {};
  std::snprintf(text, sizeof(text), "%lu.%lu.%lu.%lu", (v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
  return text;
}

std::string GameFileVersion() {
  // The FileVersion of the running Outpost2.exe, from the version resource in its loaded image (resource 1 of type
  // RT_VERSION). The VS_FIXEDFILEINFO block sits a few bytes in, after the resource's header and key, on a 4-byte
  // boundary; it starts with the signature 0xFEEF04BD.
  HMODULE exe = GetModuleHandleW(nullptr);
  HRSRC res = FindResourceW(exe, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));   // RT_VERSION
  if (res == nullptr) return std::string();
  const auto* data = static_cast<const unsigned char*>(LockResource(LoadResource(exe, res)));
  const DWORD size = SizeofResource(exe, res);
  if (data == nullptr) return std::string();
  for (DWORD i = 0; i + sizeof(VS_FIXEDFILEINFO) <= size; i += 4) {
    const auto* ffi = reinterpret_cast<const VS_FIXEDFILEINFO*>(data + i);
    if (ffi->dwSignature != 0xFEEF04BD) continue;
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                  HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    return text;
  }
  return std::string();
}

void Reset() {
  g = MissionState();
  Placement::Reset();
  OP2Lua::ResetUnitOrders();
  OP2Lua::StopMissionWav();
  OP2Lua::ResetTriggers();
  OP2Lua::ResetObjectives();
  OP2Lua::Hackaray::Reset();
}

}  // namespace OP2Lua::Mission


// ---- Exported C API (OP2LuaCore.h) -----------------------------------------------------------
using namespace OP2Lua::Mission;

extern "C" {

OP2LUACORE_API void OP2LuaCore_LoadMission(const char* missionDir, const char* baseName) {
  PrepareMission(missionDir != nullptr ? missionDir : "", baseName != nullptr ? baseName : "");
}

OP2LUACORE_API int OP2LuaCore_InitProc(void) {
  StartScript();   // a new game: the script's top level, then on_load
  {
    OP2Lua::Guard::Scope guard;
    try {
      OP2Lua::Placement::Apply();  // build the level before on_init runs
    } catch (const std::exception& e) {
      LOG_MISSION(ERROR) << "placement: " << e.what() << " - the rest of the layout wasn't placed";
    }
  }
  return DispatchBool(g.onInit, "on_init", true) ? 1 : 0;
}

OP2LUACORE_API void OP2LuaCore_AIProc(void) {
  if (!gLoadNotice.empty() && (gLoadNoticeRepeat < 0 || Game::Tick() >= gLoadNoticeRepeat)) {
    const bool second = (gLoadNoticeRepeat >= 0);
    Game::AddMessage(kLoadFailTitle, second ? Tethys::SoundID::Message2 : static_cast<Tethys::SoundID>(0));
    Game::AddMessage(gLoadNotice, static_cast<Tethys::SoundID>(0));
    if (gLoadNoticeRepeat < 0) gLoadNoticeRepeat = Game::Tick() + kLoadNoticeRepeatTicks;
    else ClearLoadNotice();
  }
  if (gLoadedSave) {
    gLoadedSave = false;
    DispatchBool(g.onLoadSave, "on_load_save", true);
  }
  {
    OP2Lua::Guard::Scope guard;
    try {
      OP2Lua::FlushUnitOrders();      // issue orders queued last tick (units now safe to command)
    } catch (const std::exception& e) {
      LOG_MISSION(ERROR) << "queued unit orders: " << e.what();
    }
  }
  DispatchVoid(g.pump, "scheduler");  // fire due after()/every()/when() (may queue new orders)
  DispatchVoid(g.onTick, "on_tick");
  if (!gEnded) {
    const EndResult e = GameEnd();
    if (e.reason != 0) FinishMission(e, "the game's end reason");
  }
}

// The game has just loaded a mission DLL (MissionManager::LoadScript): a new game, or a saved game about to be read.
// The previous mission is let go here (OPU 1.4.1 doesn't tell a mission DLL it is unloaded), and the DLL gets the
// save region its script state goes in (SaveGame.h).
OP2LUACORE_API void OP2LuaCore_GetSaveRegions(void* pSaveRegion) {
  std::string dir, base;
  if (LoadedMissionDll(dir, base)) {
    gPrepared = false;   // a new DLL, even with the same name
    PrepareMission(dir, base);
  }
  OP2Lua::SaveGame::UseRegion(pSaveRegion);
}

// OPU 1.4.2's mission events. The core runs the script at InitProc and keeps saved games through its own hooks, so
// these do nothing (on_save and on_load_save come from the hooks, on_load from StartScript).
OP2LUACORE_API int  OP2LuaCore_OnLoadMission(void*)     { return 1; }
OP2LUACORE_API void OP2LuaCore_OnEndMission(void* pArgs) {
  NoteEndEvent("OnEndMission");
  EndResult e = GameEnd();
  const auto* a = static_cast<const OnEndMissionArgs*>(pArgs);
  if (a != nullptr && a->structSize >= sizeof(OnEndMissionArgs) && a->pMissionResults != nullptr) {
    e.reason = static_cast<int>(a->pMissionResults->gameTermReason);
    e.tick = a->pMissionResults->tick;
    e.localPlayer = a->pMissionResults->localPlayerID;
  }
  FinishMission(e, "OnEndMission");
}
OP2LUACORE_API int  OP2LuaCore_OnSaveGame(void*)        { return 1; }
OP2LUACORE_API int  OP2LuaCore_OnLoadSavedGame(void*)   { return 1; }

OP2LUACORE_API int OP2LuaCore_OnUnloadMission(void*) {
  NoteEndEvent("OnUnloadMission");
  if (!gEnded && !gBase.empty()) FinishMission(GameEnd(), "OnUnloadMission");
  const int r = DispatchBool(g.onUnload, "on_unload", true) ? 1 : 0;
  Reset();  // tear the mission down when the game unloads it
  return r;
}

OP2LUACORE_API void OP2LuaCore_OnChat(void* pArgs) {
  OP2Lua::ChatArgs chat{ static_cast<OnChatArgs*>(pArgs) };
  DispatchVoid(g.onChat, "on_chat", &chat);
}

OP2LUACORE_API void OP2LuaCore_OnCreateUnit(void* pArgs) {
  auto* a = static_cast<OnCreateUnitArgs*>(pArgs);
  if (a == nullptr) return;
  DispatchVoid(g.onCreateUnit, "on_create_unit", a->unit);
}

OP2LUACORE_API void OP2LuaCore_OnDestroyUnit(void* pArgs) {
  auto* a = static_cast<OnDestroyUnitArgs*>(pArgs);
  if (a != nullptr) DispatchVoid(g.onDestroyUnit, "on_destroy_unit", a->unit);
}

OP2LUACORE_API void OP2LuaCore_OnDamageUnit(void* pArgs) {
  auto* a = static_cast<OnDamageUnitArgs*>(pArgs);
  if (a != nullptr) DispatchVoid(g.onDamageUnit, "on_damage_unit", a->targetUnit, a->sourceUnit, a->damage);
}

OP2LUACORE_API void OP2LuaCore_OnTrigger(void* pArgs) {
  if (g.active) OP2Lua::FireTrigger(pArgs);   // the `on=` function of the trigger that fired
}

}  // extern "C"
