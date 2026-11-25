// Bindings.cpp - the author-facing OP2Lua API, bound to the Tethys game API via sol2.
//
// game.*     messages and sounds, rand, tick, create_unit, map queries, map features, disasters, environment
// mission.*  win, lose, last_one_standing, victory, failure
// trigger.*  the game's triggers (count, resource, set, time, research, the campaign's escape/kit/point/...)
// Unit       state, orders, read-back of what was ordered, health, ownership
// Player     colony, resources, research, the game's own checks, alliances, faction/control
// Region     :units(player?), :contains(unit), :center()
// ChatArgs   text (get/set), player
// ai.*       building_group / fight_group / mining_group, unit_block; Group (the game's AI groups)
// docs/API.md has the full list.
//
// Determinism (multiplayer): the only randomness exposed is game.rand -> the engine's SYNCED RNG.
// State-changing calls go through Tethys (synced commands); no wall-clock, no math.random/os/io.

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <array>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "ApiTypes.h"
#include "Campaign.h"
#include "Hackaray.h"
#include "LuaHost.h"
#include "Log.h"
#include "Names.h"
#include "Mission.h"
#include "OP2LuaCore.h"   // OP2LUA_VERSION
#include "Persist.h"

// Tethys headers use unqualified min()/max() (they expect the <windows.h> macros). We build with
// NOMINMAX (sol2/STL require it), so pull std::min/std::max into the global namespace instead.
using std::max;
using std::min;

#include "Tethys/API/Game.h"          // Game::AddMessage / AddGameSound / CreateUnit / GetRand / Tick
#include "Tethys/API/Unit.h"          // Unit
#include "Tethys/API/Player.h"        // _Player (+ Enumerators)
#include "Tethys/API/Location.h"      // Location, MapRect
#include "Tethys/API/Trigger.h"       // CreateTimeTrigger / CreateVictoryCondition / CreateFailureCondition
#include "Tethys/API/Enumerators.h"   // PlayerBuildingEnum / PlayerVehicleEnum / InRectEnumerator
#include "Tethys/API/GameMap.h"       // GameMap::At (status-bar/editor coords -> internal)
#include "Tethys/API/ScGroup.h"       // BuildingGroup / FightGroup / MiningGroup (ai.*)
#include "Tethys/API/UnitBlock.h"     // UnitBlock / UnitRecord (ai.unit_block)
#include "Tethys/Game/MapObject.h"      // Tethys::MapObj::MiningBeacon (ore type for beacon perception)
#include "Tethys/Game/MineManager.h"    // OreYield, OreVariant (game.create_beacon)
#include "Tethys/Game/MapObjectType.h"  // Tethys::MapObjectType (building footprint for site finding)
#include "Tethys/Game/Research.h"       // Tethys::Research (validate tech ids before use - see ReportBadTech)
#include "Tethys/Game/TubeConnection.h"  // TubeConnection (a building's DIRT damage prevention)
#include "Tethys/Game/ScBase.h"         // ScBase (a loaded condition's objective text)
#include "Tethys/Resource/SoundID.h"  // SoundID

namespace OP2Lua {

// --- ChatArgs (declared in ApiTypes.h) -----------------------------------------------------
std::string ChatArgs::GetText() const {
  return (p != nullptr && p->pText != nullptr) ? std::string(p->pText) : std::string();
}
void ChatArgs::SetText(const std::string& text) {
  if (p == nullptr || p->pText == nullptr || p->bufferSize == 0) return;
  const size_t maxLen = p->bufferSize - 1;
  const size_t n = (text.size() < maxLen) ? text.size() : maxLen;
  std::memcpy(p->pText, text.data(), n);
  p->pText[n] = '\0';
}
int ChatArgs::GetPlayer() const { return (p != nullptr) ? p->playerNum : -1; }

namespace {

using Tethys::MapID;
using Tethys::TethysAPI::Game;
using Tethys::TethysAPI::GameMap;
using Tethys::TethysAPI::InRectEnumerator;
using Tethys::TethysAPI::Location;
using Tethys::TethysAPI::MapRect;
using Tethys::TethysAPI::MineType;
using Tethys::TethysAPI::PlayerBuildingEnum;
using Tethys::TethysAPI::PlayerVehicleEnum;
using Tethys::TethysAPI::Unit;
using Tethys::TethysAPI::UnitDirection;
using Tethys::TethysAPI::_Player;

// Default message sound. OP2's AddMessage always plays a sound by index; there is no true "silent"
// value, and an out-of-range one (e.g. -1) crashes. 0 is valid. Authors can override per-message.
constexpr Tethys::SoundID kDefaultMsgSound = static_cast<Tethys::SoundID>(0);

constexpr int kTicksPerSecond = 100;             // ~1 mark; "game time", deterministic. Tunable.
constexpr int kTicksPerMinute = 60 * kTicksPerSecond;
constexpr int kGaiaPlayer     = 6;               // the "native"/Gaia player that owns mining beacons

// Unit orders (see ApiTypes.h). Given at once; one the unit can't take yet waits in gOrders and is retried.
enum class OrderKind { Move, AttackUnit, AttackGround };
struct DeferredOrder { Unit unit; OrderKind kind; int x; int y; Unit target; int attempts = 0; };
std::vector<DeferredOrder> gOrders;

// The objective texts of the mission's victory and failure conditions, by the condition's stub id. The game keeps a
// condition's text as a pointer (+0x30, VictoryCondition/FailureCondition init 0x495870/0x495AC0), not a copy, and a
// saved game keeps the pointer: a load re-points each one at its text (RestoreBindingsState). The texts live as long as
// the process.
std::deque<std::string> gTexts;
std::map<int, std::string> gObjectives;

const std::string& KeepText(const std::string& text) { return gTexts.emplace_back(text); }
Tethys::TethysAPI::Trigger NoteObjective(Tethys::TethysAPI::Trigger t, const std::string& text) {
  if (t.IsValid()) gObjectives[t.GetID()] = text;
  return t;
}

// Resolve a sound argument: a name ("Savant4"), a raw integer SoundID, or nil -> default.
Tethys::SoundID ResolveSound(const sol::object& s, Tethys::SoundID dflt) {
  if (s.is<int>()) return static_cast<Tethys::SoundID>(s.as<int>());
  if (s.is<std::string>()) {
    const int v = SoundIdFromName(s.as<std::string>());
    if (v >= 0) return static_cast<Tethys::SoundID>(v);
  }
  return dflt;
}

// Convert a Lua value to an internal Location. A Unit's location and a Region's centre are already internal;
// a {x=,y=}/{x,y} table is author-facing (editor/status-bar) coords, translated below.
Location LocFrom(const sol::object& o) {
  if (o.is<Unit>())   return o.as<Unit>().GetLocation();
  if (o.is<Region>()) { const Region& r = o.as<Region>(); return Location((r.x1 + r.x2) / 2, (r.y1 + r.y2) / 2); }
  if (o.is<sol::table>()) {
    sol::table t = o.as<sol::table>();
    sol::optional<int> x = t["x"], y = t["y"];
    // { x =, y =, raw = true }: the game's own tile, unconverted, as a stock mission passes one off the map (CPS2's
    // truck at x 1262, which the game puts at the east edge; GameMap::At would wrap it).
    if (x && y && t.get_or("raw", false)) return Location(*x, *y);
    // 1-based author coords to the padded internal grid, without GameMap::At's wrap: a tile off the map stays off
    // it, as a stock mission passes one (Plymouth 3's storms end at the row below the map, which At would put at
    // the top).
    const int px = x ? *x : t.get_or(1, 0), py = y ? *y : t.get_or(2, 0);
    return Location(px - 1 + GameMap::GetPaddingWidth(), py - 1);
  }
  return Location();
}

// A MapRect (internal coords) from a Region, a Unit's footprint, a {x1,y1,x2,y2} rect, a {x,y} single tile (author
// coords -> GameMap::At, which wraps), or a { x =, y = [, raw = true] } single tile read as LocFrom reads it (what
// region:center() and find_place() return). Used by area orders (build_wall / doze / salvage) and the rectangle
// queries (units_in_rect, rand_point, find_emp_missile_target).
MapRect RectFrom(const sol::object& o) {
  if (o.is<Region>()) { const Region& r = o.as<Region>(); return MapRect(r.x1, r.y1, r.x2, r.y2); }
  if (o.is<Unit>())   { return o.as<Unit>().GetRect(); }
  if (o.is<sol::table>()) {
    sol::table t = o.as<sol::table>();
    if (t["x"].valid() && t["y"].valid()) {
      const Location p = LocFrom(o);
      return MapRect(p.x, p.y, p.x, p.y);
    }
    if (t.size() >= 4) {
      const Location a = GameMap::At(t.get_or(3, 0), t.get_or(4, 0)), b = GameMap::At(t.get_or(1, 0), t.get_or(2, 0));
      return MapRect(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y));
    }
    const Location p = GameMap::At(t.get_or(1, 0), t.get_or(2, 0));
    return MapRect(p.x, p.y, p.x, p.y);
  }
  return MapRect(0, 0, 0, 0);
}

// A 0-based OP2 player number from a Player handle (players[i]) or a 1-based Lua number.
int PlayerNumFrom(const sol::object& o) {
  if (o.is<LuaPlayer>()) return o.as<LuaPlayer>().num;
  if (o.is<int>())       return o.as<int>() - 1;
  return -1;
}

bool ValidPlayerNum(int num);

// PlayerNumFrom for a call that indexes the game's player table with it: -1, and a warning naming `fn`, unless it
// is a player (1-7, or a Player).
int PlayerArg(const sol::object& o, const char* fn) {
  const int num = PlayerNumFrom(o);
  if (ValidPlayerNum(num)) return num;
  LOG_MISSION(WARNING) << fn << ": needs a player (1-7 or a Player)";
  return -1;
}

// Whether a tile (the game's coordinates, as LocFrom gives them) is on the game's grid: x within the padded width
// (0x54F808), y within the height (0x54F810). The map's tile array is indexed with it unchecked.
bool OnGrid(const Location& l) {
  return l.x >= 0 && l.x < *reinterpret_cast<const int*>(0x54F808) &&
         l.y >= 0 && l.y < *reinterpret_cast<const int*>(0x54F810);
}

// OnGrid for a call that writes the tile: false, and a warning naming `fn`, when it is off the map.
bool OnGridArg(const Location& l, const char* fn) {
  if (OnGrid(l)) return true;
  LOG_MISSION(WARNING) << fn << ": the tile is off the map";
  return false;
}

// The sound manager's "sound is on" flag (+0x920: AddMapSound, 0x47EDB0, plays nothing while it is 0) and its
// volume step (+0x914, OPU\outpost2.ini's SoundVol: a sound is played at -50 * step hundredths of a dB, 0x47E32D).
int& GameSoundOn()  { return *reinterpret_cast<int*>(reinterpret_cast<char*>(Tethys::SoundManager::GetInstance()) + 0x920); }
int  GameSoundVol() { return *reinterpret_cast<int*>(reinterpret_cast<char*>(Tethys::SoundManager::GetInstance()) + 0x914); }

// A message with the sound `sound` names, or none for `false`. MessageLog::AddMessage (0x439070) always plays one
// (sound 0 is Beep8), through AddGameSound -> AddMapSound, so the sound flag is off for that call.
void AddMessageWith(const std::string& text, const sol::object& sound, int to, int px, int py) {
  if (sound.is<bool>() && !sound.as<bool>()) {
    int& on = GameSoundOn();
    const int was = on;
    on = 0;
    Game::AddMessage(text, kDefaultMsgSound, to, px, py);
    on = was;
    return;
  }
  Game::AddMessage(text, ResolveSound(sound, kDefaultMsgSound), to, px, py);
}

// game.message(text [, sound]) - in-game message log (presentation; safe per-client). `sound` false: no sound.
void Game_Message(const std::string& text, sol::object sound) {
  AddMessageWith(text, sound, Tethys::AllPlayers, 0, 0);
  LOG_MISSION(INFO) << "game.message: " << text;
}

// game.play_wav(file [, toPlayer]) - a WAV shipped with the mission, by its file name in the mission's own folder,
// played through Windows (PlaySound) rather than the game's mixer, at the game's sound volume (8- or 16-bit PCM
// samples scaled as the game attenuates its own sounds), and not at all while the game's sound is off. One at a
// time: a new one stops the last. True when the file is there and is a WAV, whether or not this machine plays it;
// false (and a log line) otherwise.
std::vector<char> gWav;   // the playing WAV's bytes (PlaySound plays from them)

// Scales a RIFF WAVE's PCM samples in place by the game's volume step (-50 * step hundredths of a dB).
void ScaleWav(std::vector<char>& w, int step) {
  if (step <= 0) return;
  const double gain = std::pow(10.0, -50.0 * step / 2000.0);
  int bits = 0;
  for (size_t at = 12; at + 8 <= w.size();) {
    uint32_t len; std::memcpy(&len, w.data() + at + 4, 4);
    const size_t body = at + 8;
    if (body + len > w.size()) break;
    if (std::memcmp(w.data() + at, "fmt ", 4) == 0 && len >= 16) {
      uint16_t fmt, b; std::memcpy(&fmt, w.data() + body, 2); std::memcpy(&b, w.data() + body + 14, 2);
      if (fmt != 1) return;   // not PCM: played as it is
      bits = b;
    } else if (std::memcmp(w.data() + at, "data", 4) == 0) {
      if (bits == 8) {
        for (size_t i = 0; i < len; ++i) {
          auto& v = reinterpret_cast<unsigned char&>(w[body + i]);
          v = static_cast<unsigned char>(128 + std::lround((v - 128) * gain));
        }
      } else if (bits == 16) {
        for (size_t i = 0; i + 1 < len; i += 2) {
          int16_t v; std::memcpy(&v, w.data() + body + i, 2);
          v = static_cast<int16_t>(std::lround(v * gain));
          std::memcpy(w.data() + body + i, &v, 2);
        }
      }
      return;
    }
    at = body + len + (len & 1);
  }
}
void StopWav() { PlaySoundA(nullptr, nullptr, 0); gWav.clear(); }
bool Game_PlayWav(const std::string& file, sol::optional<int> toPlayer) {
  std::string lower = file;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  if (file.empty() || file.find_first_of("/\\:") != std::string::npos || file.find("..") != std::string::npos ||
      lower.size() < 5 || lower.compare(lower.size() - 4, 4, ".wav") != 0) {
    LOG_MISSION(WARNING) << "game.play_wav: '" << file << "' is not a .wav file name in the mission's folder";
    return false;
  }
  const std::string path = ::OP2Lua::Mission::Dir() + "\\" + file;
  std::ifstream in(path, std::ios::binary);
  std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0) {
    LOG_MISSION(WARNING) << "game.play_wav: " << path << (data.empty() ? " not found" : " is not a WAV file");
    return false;
  }
  const int to = toPlayer.value_or(Tethys::AllPlayers);
  if (to != Tethys::AllPlayers && to != Game::LocalPlayer()) return true;
  const bool on = GameSoundOn() != 0;
  LOG_MISSION(INFO) << "game.play_wav: " << file << (on ? "" : " not played: the game's sound is off") << " (Sound "
                    << GameSoundOn() << ", SoundVol " << GameSoundVol() << ")";
  if (!on) return true;
  ScaleWav(data, GameSoundVol());
  PlaySoundA(nullptr, nullptr, 0);   // stop the last one before its bytes go
  gWav.swap(data);
  PlaySoundA(gWav.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
  return true;
}

// game.sound(name|id [, toPlayer]) - play a global game sound.
void Game_Sound(sol::object sound, sol::optional<int> toPlayer) {
  Game::AddGameSound(ResolveSound(sound, static_cast<Tethys::SoundID>(0)),
                     toPlayer.value_or(Tethys::AllPlayers));
}

// game.rand(n) - the synced RNG, [0, n). n = 0 still draws (and gives 0), as the game's GetRand (0x46F060) does
// and a stock mission relies on (Eden 7's RepeatWaves); a negative n gives 0 without drawing.
int Game_Rand(int n) { return (n >= 0) ? Game::GetRand(n) : 0; }
int Game_Tick()      { return Game::Tick(); }

// game.version() -> "0.8.10", "2026-11-17 11:57:19": the running OP2LuaCore's version and when its DLL was built.
std::tuple<std::string, std::string> Game_Version() { return { OP2LUA_VERSION, ::OP2Lua::Mission::BuildTime() }; }

// game.op2_version() -> the version the running Outpost 2 goes by (TApp::Version: "1.4.0.1" under OPU 1.4.x), and
// Outpost2.exe's own file version ("1.2.0.7"); nil for either one not found.
std::tuple<sol::object, sol::object> Game_Op2Version(sol::this_state ts) {
  sol::state_view L(ts);
  const std::string v = ::OP2Lua::Mission::GameVersion(), f = ::OP2Lua::Mission::GameFileVersion();
  return { v.empty() ? sol::make_object(L, sol::lua_nil) : sol::make_object(L, v),
           f.empty() ? sol::make_object(L, sol::lua_nil) : sol::make_object(L, f) };
}

Tethys::MoraleLevel MoraleFromName(const std::string& n) {
  if (n == "Excellent" || n == "Great")  return Tethys::MoraleLevel::Excellent;
  if (n == "Fair" || n == "OK")          return Tethys::MoraleLevel::Fair;
  if (n == "Poor")                       return Tethys::MoraleLevel::Poor;
  if (n == "Terrible" || n == "Rotten")  return Tethys::MoraleLevel::Terrible;
  return Tethys::MoraleLevel::Good;
}

// game.morale_steady([level]) - lock all players' morale at a level (default "Good") so the colony
// doesn't fluctuate or collapse. This is the multiplayer "Morale Steady" rule: focus on fighting.
// game.morale_steady([level [, player]]) - lock morale at `level` for every player, or one (a Player or 1-based number;
// a mission can lock it for one player, as a reward for something that player did).
void Game_MoraleSteady(sol::object level, sol::object player) {
  const auto lvl = level.is<std::string>() ? MoraleFromName(level.as<std::string>())
                                           : Tethys::MoraleLevel::Good;
  const bool all = !player.valid() || player == sol::lua_nil;
  const int num = all ? static_cast<int>(Tethys::AllPlayers) : PlayerArg(player, "morale_steady");
  if (!all && num < 0) return;
  Game::ForceMoraleLevel(lvl, num);
  LOG_MISSION(INFO) << "game.morale_steady";
}
// game.free_morale([player]) - release the lock (all players, or one: a Player or a 1-based number); morale
// fluctuates normally again.
void Game_FreeMorale(sol::object player) {
  const bool all = !player.valid() || player == sol::lua_nil;
  const int num = all ? static_cast<int>(Tethys::AllPlayers) : PlayerArg(player, "free_morale");
  if (all || num >= 0) Game::FreeMoraleLevel(num);
}

// game.create_unit{ type=, player=, at=, count=, weapon=, direction=, lights= } -> { Unit, ... }
sol::table Game_CreateUnit(sol::table opts, sol::this_state ts) {
  sol::state_view L(ts);
  sol::table out = L.create_table();

  const std::string typeName = opts.get_or("type", std::string());
  const MapID type = MapIdFromName(typeName);
  if (type == MapID::None) {
    LOG_MISSION(WARNING) << "create_unit: unknown type '" << typeName << "'";
    return out;
  }
  // `player` accepts a Player handle (players[i], 0-based) or a 1-based number.
  int owner = 0;
  {
    sol::object pObj = opts["player"];
    if (pObj.is<LuaPlayer>())   owner = pObj.as<LuaPlayer>().num;
    else if (pObj.is<int>())    owner = pObj.as<int>() - 1;
  }
  if (!ValidPlayerNum(owner)) {
    LOG_MISSION(WARNING) << "create_unit: player " << (owner + 1) << " isn't a player (1-7)";
    return out;
  }
  const sol::object atObj = opts["at"];
  const Location at = LocFrom(atObj);
  const int count = opts.get_or("count", 1);

  MapID weapon = MapID::None;
  sol::optional<std::string> w = opts["weapon"];
  if (w) weapon = WeaponFor(type, MapIdFromName(*w));   // a Guard Post's Starflare is its turret's
  const auto dir = static_cast<UnitDirection>(DirFromName(opts.get_or("direction", std::string("East"))));
  const bool lights = opts.get_or("lights", true);

  // Place each unit on a CLEAR tile near the target. Stacking onto a tile that already holds a unit
  // corrupts OP2's unit grid and crashes the game a frame later - e.g. spawning enemies on top of the
  // player's units that have just advanced into the spawn area. FindUnitPlacementLocation is OP2's own
  // deploy-site finder: a passable, unoccupied tile near 'at', respecting the type's footprint. Each
  // CreateUnit marks its tile occupied, so successive calls scatter the group.
  // `exact = true` skips the finder and places on `at` itself, as a mission's setup places its bases
  // (structures beside tubes the finder would step away from). Only for setup-time placement on a
  // tile known to be free.
  const bool exact = opts.get_or("exact", false);
  int created = 0;
  const int width = (count > 1) ? 4 : 1;
  for (int i = 0; i < count; ++i) {
    Location spot = exact ? Location(at.x + (i % width), at.y + (i / width))
                          : GameMap::FindUnitPlacementLocation(at, type);
    if (spot.x < 0) spot = Location(at.x + (i % width), at.y + (i / width));   // finder failed -> fallback
    Unit u = Game::CreateUnit(type, spot, owner, weapon, dir, lights);
    if (u.IsValid()) ++created;
    out[i + 1] = u;
  }
  // AI spawns -> the AI log; human-player reinforcements -> the mission log.
  const bool aiOwner = !_Player(owner).IsHuman();
  if (created < count) {
    const std::string msg = "create_unit: requested " + std::to_string(count) + "x " + typeName +
                            " but only created " + std::to_string(created) + " (OP2 unit limit reached?)";
    if (aiOwner) LOG_AI(WARNING) << msg; else LOG_MISSION(WARNING) << msg;
  } else {
    const std::string msg = "create_unit: " + std::to_string(count) + "x " + typeName + " for player " +
                            std::to_string(owner) + " around (" + std::to_string(at.x) + "," +
                            std::to_string(at.y) + ")";
    if (aiOwner) LOG_AI(INFO) << msg; else LOG_MISSION(INFO) << msg;
  }
  return out;
}

// game.create_beacon{ at=, type=, ore=, yield=, variant= } -> Unit. The runtime twin of a placement.lua
// beacon, for a mission that decides its beacons when it starts (a stock multiplayer map rolls some
// yields with game.rand). Same names as placement: type "MiningBeacon" (default) / "MagmaVent" /
// "Fumarole"; ore "Common" / "Rare" / "Random"; yield "Bar3" / "Bar2" / "Bar1" / "Random";
// variant "High" / "Mid" / "Low" / "Random".
Unit Game_CreateBeacon(sol::table opts) {
  const std::string type = opts.get_or("type", std::string("MiningBeacon"));
  const std::string ore = opts.get_or("ore", std::string("Random"));
  MineType mt = MineType::RandomOre;
  if (type == "Fumarole")       mt = MineType::Fumarole;
  else if (type == "MagmaVent") mt = MineType::MagmaVent;
  else if (ore == "Common")     mt = MineType::CommonOre;
  else if (ore == "Rare")       mt = MineType::RareOre;
  const std::string y = opts.get_or("yield", std::string("Random"));
  const Tethys::OreYield yield = (y == "Bar3") ? Tethys::OreYield::Bar3 : (y == "Bar2") ? Tethys::OreYield::Bar2
                               : (y == "Bar1") ? Tethys::OreYield::Bar1 : Tethys::OreYield::Random;
  const std::string v = opts.get_or("variant", std::string("Random"));
  const Tethys::OreVariant variant = (v == "High") ? Tethys::OreVariant::High : (v == "Mid") ? Tethys::OreVariant::Mid
                                   : (v == "Low") ? Tethys::OreVariant::Low : Tethys::OreVariant::Random;
  return Game::CreateMine(LocFrom(opts["at"]), mt, yield, variant);
}

// The internal time trigger references "OP2LuaTrigger" - a no-op function the stub DLL exports -
// so OP2 can resolve the trigger function (an empty name fails: "couldn't resolve data reference").
void Mission_Win(sol::variadic_args) {
  using namespace Tethys::TethysAPI;
  LOG_MISSION(INFO) << "mission.win()";
  const std::string& text = KeepText("Mission accomplished");
  NoteObjective(CreateVictoryCondition(CreateTimeTrigger(1, "OP2LuaTrigger", true, true), text, true, true), text);
}
void Mission_Lose(sol::variadic_args) {
  using namespace Tethys::TethysAPI;
  LOG_MISSION(INFO) << "mission.lose()";
  NoteObjective(CreateFailureCondition(CreateTimeTrigger(1, "OP2LuaTrigger", true, true), true), "");
}
// mission.last_one_standing([text]) - the multiplayer Last One Standing victory, as the stock ml*.dll
// missions set it up: the game's own one-player-left trigger wrapped in a victory condition. The game
// decides it per player (a player is out once they have no working Command Center), so unlike
// mission.win() it is fair in a multiplayer game. `text` is the objective shown in the game's list.
void Mission_LastOneStanding(sol::optional<std::string> text) {
  using namespace Tethys::TethysAPI;
  // The game keeps the text's pointer, not a copy (it shows it in Mission Objectives for the whole game),
  // so the text must outlive this call: kept here for the process's life (a few bytes per mission).
  const std::string& objective = KeepText(text.value_or("Eliminate your opponents."));
  LOG_MISSION(INFO) << "mission.last_one_standing(\"" << objective << "\")";
  NoteObjective(CreateVictoryCondition(CreateLastOneStandingTrigger("OP2LuaTrigger", true, true), objective, true, true),
                objective);
}

// ---- triggers (trigger.* / mission.victory / mission.failure) -----------------------------
// The game's own triggers, as the stock missions make them: a Space Race victory is a count trigger for each
// launched module, a Resource Race one a set of two resource triggers, Midas a Midas trigger, Land Rush per-player
// operational/colonist/vehicle triggers that switch a disabled last-one-standing victory on. The game checks them
// every 4 ticks, on every machine alike, so they are safe in a multiplayer game. Each constructor takes an options
// table: the trigger's own fields plus `on` (a function called with the Trigger when it fires), `one_shot` and
// `enabled`; it returns the Trigger, a handle with :enable() / :disable() / .enabled / :has_fired(player).
//
// A trigger with `on` names the stub's OnTrigger export, which OPU (1.4.0+) calls with the trigger that fired:
// FireTrigger looks its function up here by trigger id. Without `on` it names the stub's no-op OP2LuaTrigger.
// A saved game keeps the functions with the rest of the script (SaveGame.cpp: PushTriggerFunctions / SetTriggerFunctions).
using Tethys::TethysAPI::CompareMode;
using Tethys::TethysAPI::Trigger;
using Tethys::TethysAPI::TriggerResource;

std::map<int, sol::protected_function> gTriggerFns;
bool IsValidTechId(int techId);                 // below, with the Player bindings
void ReportBadTech(const char* fn, int techId);

CompareMode CompareFrom(const sol::table& o, const char* dflt) {
  const std::string c = o.get_or("compare", std::string(dflt));
  if (c == "==" || c == "=") return CompareMode::Equal;
  if (c == "<=")             return CompareMode::LowerEqual;
  if (c == "<")              return CompareMode::Lower;
  if (c == ">")              return CompareMode::Greater;
  if (c != ">=") LOG_MISSION(WARNING) << "trigger: unknown compare \"" << c << "\", using >=";
  return CompareMode::GreaterEqual;
}

// The trigger function name for a new trigger: the stub's OnTrigger when it has a Lua function to call.
const char* TriggerFnName(const sol::table& o) {
  return o.get<sol::object>("on").is<sol::protected_function>() ? "OnTrigger" : "OP2LuaTrigger";
}
// Keeps a new trigger's `on` function for FireTrigger (by trigger id) and warns when the game made no trigger.
Trigger Remember(Trigger t, const sol::table& o, const char* what) {
  sol::object fn = o.get<sol::object>("on");
  if (fn.is<sol::protected_function>() && t.IsValid()) gTriggerFns[t.GetID()] = fn.as<sol::protected_function>();
  if (!t.IsValid()) LOG_MISSION(WARNING) << "trigger." << what << ": the game made no trigger";
  return t;
}
int PlayerOpt(const sol::table& o) {
  sol::object p = o.get<sol::object>("player");
  return (p.valid() && p != sol::lua_nil) ? PlayerNumFrom(p) : static_cast<int>(Tethys::AllPlayers);
}
// Whether a trigger's `player` is a number the game can't take: past its players, or below 0 but not every player.
// The game's checks index its player records with it, so the caller makes no trigger; a warning names it.
bool BadTriggerPlayer(const sol::table& o, const char* what) {
  const int num = PlayerOpt(o);
  if ((num == static_cast<int>(Tethys::AllPlayers)) || ValidPlayerNum(num)) return false;
  LOG_MISSION(WARNING) << "trigger." << what << ": needs a player (1-7 or a Player)";
  return true;
}
bool OneShot(const sol::table& o, bool dflt) { return o.get_or("one_shot", dflt); }
bool Enabled(const sol::table& o)            { return o.get_or("enabled", true); }

// trigger.count{ type=, cargo=, compare=">=", count=1, player= } - how many units of a type a player (or any
// player) has; `cargo` narrows it (a ConVec's kit, a weapon; any if left out). Space Race: type="IonDriveModule".
Trigger Trigger_Count(sol::table o) {
  if (BadTriggerPlayer(o, "count")) return Trigger();
  const MapID type = MapIdFromName(o.get_or("type", std::string()));
  // `cargo` may be a number, passed to the game as it is: a stock mission's raw value (Eden 1 counts Cargo Trucks
  // carrying truck cargo 1, food, and 4, Common Metal).
  const sol::object cargoArg = o.get<sol::object>("cargo");
  const std::string cargo = cargoArg.is<std::string>() ? cargoArg.as<std::string>() : std::string();
  const MapID cw = cargoArg.is<int>() ? MapID(cargoArg.as<int>())
                 : cargo.empty()      ? MapID::Any : MapIdFromName(cargo);   // no cargo given: any (as the stock maps)
  return Remember(Tethys::TethysAPI::CreateCountTrigger(type, cw, CompareFrom(o, ">="), o.get_or("count", 1),
                                                        TriggerFnName(o), PlayerOpt(o), OneShot(o, false), Enabled(o)),
                  o, "count");
}
// trigger.resource{ resource=, compare=">=", amount=, player= } - resource: "Food" / "CommonOre" / "RareOre" /
// "Kids" / "Workers" / "Scientists" / "Colonists" / "Ore".
Trigger Trigger_Resource(sol::table o) {
  if (BadTriggerPlayer(o, "resource")) return Trigger();
  static const std::map<std::string, TriggerResource> names = {
      {"Food", TriggerResource::Food}, {"CommonOre", TriggerResource::CommonOre}, {"RareOre", TriggerResource::RareOre},
      {"Kids", TriggerResource::Kids}, {"Workers", TriggerResource::Workers},
      {"Scientists", TriggerResource::Scientists}, {"Colonists", TriggerResource::Colonists},
      // 7: past Tethys's list, but the game takes it (the stock Labyrinth, ml2_11, waits on a player's ore with it).
      {"Ore", static_cast<TriggerResource>(7)}};
  const std::string r = o.get_or("resource", std::string("CommonOre"));
  const auto it = names.find(r);
  if (it == names.end()) LOG_MISSION(WARNING) << "trigger.resource: unknown resource \"" << r << "\"";
  const TriggerResource res = (it != names.end()) ? it->second : TriggerResource::CommonOre;
  return Remember(Tethys::TethysAPI::CreateResourceTrigger(res, CompareFrom(o, ">="), o.get_or("amount", 0),
                                                           TriggerFnName(o), PlayerOpt(o), OneShot(o, false), Enabled(o)),
                  o, "resource");
}
// trigger.set{ a, b, ..., needed=#list } - fires once `needed` of the listed triggers (1 to 8) have; each player
// is judged on their own. Resource Race: both resource triggers.
Trigger Trigger_Set(sol::table o) {
  std::vector<Trigger> ts;
  for (size_t i = 1; i <= o.size(); ++i) {
    sol::object t = o[i];
    if (t.is<Trigger>()) ts.push_back(t.as<Trigger>());
  }
  if (ts.empty() || ts.size() > 8) {
    LOG_MISSION(WARNING) << "trigger.set: needs 1 to 8 triggers, got " << ts.size();
    return Trigger();
  }
  const int needed = o.get_or("needed", static_cast<int>(ts.size()));
  const char* fn = TriggerFnName(o);
  const bool once = OneShot(o, false), on = Enabled(o);
  using Tethys::TethysAPI::CreateSetTrigger;
  Trigger t;
  switch (ts.size()) {
    case 1: t = CreateSetTrigger(fn, needed, once, on, ts[0]); break;
    case 2: t = CreateSetTrigger(fn, needed, once, on, ts[0], ts[1]); break;
    case 3: t = CreateSetTrigger(fn, needed, once, on, ts[0], ts[1], ts[2]); break;
    case 4: t = CreateSetTrigger(fn, needed, once, on, ts[0], ts[1], ts[2], ts[3]); break;
    case 5: t = CreateSetTrigger(fn, needed, once, on, ts[0], ts[1], ts[2], ts[3], ts[4]); break;
    case 6: t = CreateSetTrigger(fn, needed, once, on, ts[0], ts[1], ts[2], ts[3], ts[4], ts[5]); break;
    case 7: t = CreateSetTrigger(fn, needed, once, on, ts[0], ts[1], ts[2], ts[3], ts[4], ts[5], ts[6]); break;
    default: t = CreateSetTrigger(fn, needed, once, on, ts[0], ts[1], ts[2], ts[3], ts[4], ts[5], ts[6], ts[7]); break;
  }
  return Remember(t, o, "set");
}
// trigger.midas{ time= } - Midas: fires at tick `time` for the player with the most ore in storage.
Trigger Trigger_Midas(sol::table o) {
  return Remember(Tethys::TethysAPI::CreateMidasTrigger(o.get_or("time", 25000), TriggerFnName(o), OneShot(o, true),
                                                        Enabled(o)),
                  o, "midas");
}
// trigger.time{ time= } or { min=, max= } - fires after `time` ticks, or after a time the game draws once between
// `min` and `max` when the trigger is made (one_shot: true by default; with one_shot = false, every that long). The
// stock missions' disasters: a time trigger whose function makes the next one.
Trigger Trigger_Time(sol::table o) {
  sol::optional<int> time = o["time"], min = o["min"], max = o["max"];
  if (time) return Remember(Tethys::TethysAPI::CreateTimeTrigger(*time, TriggerFnName(o), OneShot(o, true), Enabled(o)), o, "time");
  if (!min || !max) {
    LOG_MISSION(WARNING) << "trigger.time: needs time, or min and max";
    return Trigger();
  }
  return Remember(Tethys::TethysAPI::CreateTimeTrigger(*min, *max, TriggerFnName(o), OneShot(o, true), Enabled(o)), o, "time");
}
// trigger.research{ tech=, player= } - a player (or any player) has researched the topic `tech` (its id).
Trigger Trigger_Research(sol::table o) {
  if (BadTriggerPlayer(o, "research")) return Trigger();
  const int tech = o.get_or("tech", 0);
  if (!IsValidTechId(tech)) {
    ReportBadTech("trigger.research", tech);
    return Trigger();
  }
  return Remember(Tethys::TethysAPI::CreateResearchTrigger(tech, TriggerFnName(o), PlayerOpt(o), OneShot(o, false), Enabled(o)),
                  o, "research");
}
// trigger.one_player_left{} - the Last One Standing condition (mission.last_one_standing wraps it in a victory).
Trigger Trigger_OnePlayerLeft(sol::table o) {
  return Remember(Tethys::TethysAPI::CreateLastOneStandingTrigger(TriggerFnName(o), OneShot(o, true), Enabled(o)),
                  o, "one_player_left");
}
// trigger.operational{ type=, compare=">=", count=1, player= } - a player's working structures of a type (a
// player is needed: the game misjudges "any player"). Land Rush: a Command Center up.
Trigger Trigger_Operational(sol::table o) {
  if (BadTriggerPlayer(o, "operational")) return Trigger();
  const int player = PlayerOpt(o);
  if (player == static_cast<int>(Tethys::AllPlayers)) {
    LOG_MISSION(WARNING) << "trigger.operational: needs a player";
    return Trigger();
  }
  const MapID type = MapIdFromName(o.get_or("type", std::string("CommandCenter")));
  return Remember(Tethys::TethysAPI::CreateOperationalTrigger(type, CompareFrom(o, ">="), o.get_or("count", 1), player,
                                                              TriggerFnName(o), OneShot(o, true), Enabled(o)),
                  o, "operational");
}
// trigger.vehicle_count{ compare=, count=, player= } - all of a player's vehicles, every type.
Trigger Trigger_VehicleCount(sol::table o) {
  if (BadTriggerPlayer(o, "vehicle_count")) return Trigger();
  return Remember(Tethys::TethysAPI::CreateVehicleCountTrigger(CompareFrom(o, ">="), o.get_or("count", 1),
                                                               TriggerFnName(o), PlayerOpt(o), OneShot(o, false), Enabled(o)),
                  o, "vehicle_count");
}
// The campaign's triggers, made by calling the game's own factories at their addresses with the arguments a stock
// mission passes, not through Tethys's wrappers (those take a MapRect and pass its width through the game's
// MapRect::Width, or x1 and x2 for a rect trigger's x and y). `at` is a tile as anywhere else; a stock mission's
// own tile is { x =, y =, raw = true }.
//
// trigger.escape{ at=, width=, height=, type=, count=1, cargo=-1, amount=-1, player= } - at least `count` units of a
// type (with truck cargo and amount) in the rectangle: the campaign's evacuations (Eden 1's trucks of food at 50,45).
Trigger Trigger_Escape(sol::table o) {
  if (BadTriggerPlayer(o, "escape")) return Trigger();
  const Location at = LocFrom(o.get<sol::object>("at"));
  const MapID type = MapIdFromName(o.get_or("type", std::string()));
  using Fn = Trigger FASTCALL(Tethys::ibool, Tethys::ibool, int, int, int, int, int, int, MapID, int, int, const char*);
  return Remember(Tethys::OP2Thunk<0x4796E0, Fn>(Enabled(o), OneShot(o, false), PlayerOpt(o), at.x, at.y,
                                                 o.get_or("width", 1), o.get_or("height", 1), o.get_or("count", 1), type,
                                                 o.get_or("cargo", -1), o.get_or("amount", -1), TriggerFnName(o)),
                  o, "escape");
}
// trigger.kit{ type=, count=1, player= } - a player's ConVecs carrying at least `count` kits of a structure
// (Eden 1's six kits). `player` is needed.
Trigger Trigger_Kit(sol::table o) {
  if (BadTriggerPlayer(o, "kit")) return Trigger();
  // For all players the game faults at its next check of the trigger (PlayerUnitEnum::GetNext, 0x47A5EE).
  const int player = PlayerOpt(o);
  if (player == static_cast<int>(Tethys::AllPlayers)) {
    LOG_MISSION(WARNING) << "trigger.kit: needs a player";
    return Trigger();
  }
  const MapID type = MapIdFromName(o.get_or("type", std::string()));
  using Fn = Trigger FASTCALL(Tethys::ibool, Tethys::ibool, int, MapID, int, const char*);
  return Remember(Tethys::OP2Thunk<0x4791C0, Fn>(Enabled(o), OneShot(o, false), player, type, o.get_or("count", 1),
                                                 TriggerFnName(o)),
                  o, "kit");
}
// trigger.evac{ player= } - the game's evacuation trigger (CreateEvacTrigger, Eden 2's evacuation objective).
Trigger Trigger_Evac(sol::table o) {
  if (BadTriggerPlayer(o, "evac")) return Trigger();
  using Fn = Trigger FASTCALL(Tethys::ibool, Tethys::ibool, int, const char*);
  return Remember(Tethys::OP2Thunk<0x479260, Fn>(Enabled(o), OneShot(o, false), PlayerOpt(o), TriggerFnName(o)), o, "evac");
}
// trigger.special_target{ target=unit, type="Scout", one_shot=true } - a unit of `type` next to `target` (the
// campaign's lab visits by a Scout; CreateSpecialTarget, 0x4797A0). It takes no `player`.
Trigger Trigger_SpecialTarget(sol::table o) {
  const sol::object t = o.get<sol::object>("target");
  if (!t.is<Unit>()) {
    LOG_MISSION(WARNING) << "trigger.special_target: needs a target unit";
    return Trigger();
  }
  return Remember(Tethys::TethysAPI::CreateSpecialTarget(t.as<Unit>(), MapIdFromName(o.get_or("type", std::string("Scout"))),
                                                         TriggerFnName(o), OneShot(o, true), Enabled(o)),
                  o, "special_target");
}
// trigger.point{ at=, player= } - a vehicle of the player on the tile.
Trigger Trigger_Point(sol::table o) {
  if (BadTriggerPlayer(o, "point")) return Trigger();
  const Location at = LocFrom(o.get<sol::object>("at"));
  using Fn = Trigger FASTCALL(Tethys::ibool, Tethys::ibool, int, int, int, const char*);
  return Remember(Tethys::OP2Thunk<0x479070, Fn>(Enabled(o), OneShot(o, false), PlayerOpt(o), at.x, at.y, TriggerFnName(o)),
                  o, "point");
}
// trigger.rect{ at=, width=, height=, player= } - a vehicle of the player in the rectangle from `at`.
Trigger Trigger_Rect(sol::table o) {
  if (BadTriggerPlayer(o, "rect")) return Trigger();
  const Location at = LocFrom(o.get<sol::object>("at"));
  using Fn = Trigger FASTCALL(Tethys::ibool, Tethys::ibool, int, int, int, int, int, const char*);
  return Remember(Tethys::OP2Thunk<0x478FC0, Fn>(Enabled(o), OneShot(o, false), PlayerOpt(o), at.x, at.y,
                                                 o.get_or("width", 1), o.get_or("height", 1), TriggerFnName(o)),
                  o, "rect");
}
// trigger.building_count{ compare=, count=, player= } - all of a player's structures, every type.
Trigger Trigger_BuildingCount(sol::table o) {
  if (BadTriggerPlayer(o, "building_count")) return Trigger();
  return Remember(Tethys::TethysAPI::CreateBuildingCountTrigger(CompareFrom(o, ">="), o.get_or("count", 1),
                                                                TriggerFnName(o), PlayerOpt(o), OneShot(o, false), Enabled(o)),
                  o, "building_count");
}

// mission.victory(trigger, text[, {one_shot=false, enabled=true}]) - a victory condition: a player wins once
// `trigger` fires for them (all of a game's victory conditions, for one player). `text` is the objective in the
// game's Mission Objectives list; the game keeps its pointer, so it is kept here for the process's life.
Trigger Mission_Victory(Trigger condition, const std::string& text, sol::optional<sol::table> opts) {
  const std::string& objective = KeepText(text);
  const bool once = opts ? opts->get_or("one_shot", false) : false;
  const bool on = opts ? opts->get_or("enabled", true) : true;
  LOG_MISSION(INFO) << "mission.victory(trigger " << condition.GetID() << ", \"" << objective << "\")";
  return NoteObjective(Tethys::TethysAPI::CreateVictoryCondition(condition, objective, once, on), objective);
}
// mission.failure(trigger[, {enabled=true}]) - a failure condition: a player loses once `trigger` fires for them.
Trigger Mission_Failure(Trigger condition, sol::optional<sol::table> opts) {
  const bool on = opts ? opts->get_or("enabled", true) : true;
  LOG_MISSION(INFO) << "mission.failure(trigger " << condition.GetID() << ")";
  return NoteObjective(Tethys::TethysAPI::CreateFailureCondition(condition, on), "");
}

bool Player_OwnsAny(int num, const std::string& typeName) {
  const MapID t = MapIdFromName(typeName);
  if (t == MapID::None) return false;
  for (Unit u : PlayerBuildingEnum(num, t)) { (void)u; return true; }
  for (Unit u : PlayerVehicleEnum(num, t)) { (void)u; return true; }
  return false;
}
int Player_UnitCount(int num, const std::string& typeName) {
  const MapID t = MapIdFromName(typeName);
  if (t == MapID::None) return 0;
  int c = 0;
  for (Unit u : PlayerBuildingEnum(num, t)) { (void)u; ++c; }
  for (Unit u : PlayerVehicleEnum(num, t)) { (void)u; ++c; }
  return c;
}

// player:units([type]) -> list of this player's units (vehicles + buildings), optionally one type.
sol::table Player_Units(int num, sol::object typeArg, sol::this_state ts) {
  sol::state_view L(ts);
  sol::table out = L.create_table();
  MapID t = MapID::Any;
  if (typeArg.is<std::string>()) {
    t = MapIdFromName(typeArg.as<std::string>());
    if (t == MapID::None) return out;  // unknown type -> empty list
  }
  int i = 1;
  for (Unit u : PlayerVehicleEnum(num, t))  out[i++] = u;
  for (Unit u : PlayerBuildingEnum(num, t)) out[i++] = u;
  return out;
}

// Region:units(player?) -> list of units in the region (optionally filtered to a player).
sol::table Region_Units(const Region& r, sol::object playerArg, sol::this_state ts) {
  sol::state_view L(ts);
  sol::table out = L.create_table();
  int filterOwner = -1;  // -1 = any player
  if (playerArg.is<LuaPlayer>())   filterOwner = playerArg.as<LuaPlayer>().num;
  else if (playerArg.is<int>())    filterOwner = playerArg.as<int>() - 1;
  int i = 1;
  for (Unit u : InRectEnumerator(MapRect(r.x1, r.y1, r.x2, r.y2))) {
    if (filterOwner >= 0 && u.GetOwner() != filterOwner) continue;
    out[i++] = u;
  }
  return out;
}
// game.units_in_range(at, range) -> the units of every player the game's InRangeEnumerator finds within `range`
// tiles of `at`, in its order (CPP1's Rand_Meteor rolls again while one of them is a player 1 building).
sol::table Game_UnitsInRange(sol::object at, int range, sol::this_state ts) {
  sol::state_view L(ts);
  sol::table out = L.create_table();
  int i = 1;
  for (Unit u : Tethys::TethysAPI::InRangeEnumerator(LocFrom(at), range)) out[i++] = u;
  return out;
}

// game.units_closest(at [, count]) -> the units of every player in the order the game's ClosestEnumerator gives them,
// nearest `at` first by its own measure (0x47A7B0, 0x47A7F0), ties in its order; at most `count` of them. For a
// mission that walks the units nearest a spot as a mission DLL does with ClosestEnumerator.
sol::table Game_UnitsClosest(sol::object at, sol::optional<int> count, sol::this_state ts) {
  sol::state_view L(ts);
  sol::table out = L.create_table();
  const int most = count.value_or(INT_MAX);
  int i = 0;
  for (const auto& r : Tethys::TethysAPI::ClosestEnumerator(LocFrom(at))) {
    if (i >= most) break;
    out[++i] = r.first;
  }
  return out;
}

// game.units_in_rect(rect) -> the units of every player the game's InRectEnumerator finds in the rectangle
// {x1, y1, x2, y2} (CPS2's Base1BSmelterCheck looks there for a smelter).
sol::table Game_UnitsInRect(sol::object rect, sol::this_state ts) {
  sol::state_view L(ts);
  sol::table out = L.create_table();
  int i = 1;
  for (Unit u : InRectEnumerator(RectFrom(rect))) out[i++] = u;
  return out;
}

// game.find_emp_missile_target(rect, player) -> { x =, y =, raw = true }: the tile the game picks for an EMP missile
// aimed at `player` in the rectangle (TethysGame::FindEMPMissleTarget, 0x478480, called as the stock missions call it:
// the result through a pointer in ecx, x1 in edx, then y1, x2, y2 and the player; Tethys's wrapper passes them in
// another order). The result is the game's own tile, unconverted (LocFrom takes it back as is); a launch at it aims
// at pixel x * 32, y * 32 (unit:launch_at_pixel).
sol::table Game_FindEmpMissileTarget(sol::object rect, sol::object player, sol::this_state ts) {
  sol::state_view L(ts);
  const MapRect r = RectFrom(rect);
  Location out;
  const int num = PlayerArg(player, "find_emp_missile_target");
  if (num < 0) return L.create_table();
  using FindFn = Location*(__fastcall*)(Location*, int, int, int, int, int);
  reinterpret_cast<FindFn>(0x478480)(&out, r.x1, r.y1, r.x2, r.y2, num);
  sol::table t = L.create_table();
  t["x"] = out.x;
  t["y"] = out.y;
  t["raw"] = true;
  return t;
}

// game.rand_point(rect) -> { x, y } (author coords): a tile drawn in the rectangle {x1, y1, x2, y2} by the game's own
// MAP_RECT::RandPt (0x475CC0), so the game's random numbers go as a mission's own draws do (CPP1's storms). It writes
// its result through a pointer passed on the stack (`ret 4`); Tethys's MapRect::RandomPoint thunk reads it back wrong.
sol::table Game_RandPoint(sol::object rect, sol::this_state ts) {
  sol::state_view L(ts);
  const MapRect r = RectFrom(rect);
  Location p;
  using RandPt = Location*(__thiscall*)(const MapRect*, Location*);
  reinterpret_cast<RandPt>(0x475CC0)(&r, &p);
  sol::table t = L.create_table();
  t[1] = p.x - GameMap::GetPaddingWidth() + 1;
  t[2] = p.y + 1;
  return t;
}

bool Region_Contains(const Region& r, const Unit& u) {
  const Location loc = u.GetLocation();
  return loc.x >= r.x1 && loc.x <= r.x2 && loc.y >= r.y1 && loc.y <= r.y2;
}

// game.beacons([ore]) -> list of unmined mining beacons (owned by the Gaia player). Optional ore
// filter "Common"/"Rare". Beacons aren't in any player's unit list, so this is how a mission/AI
// DISCOVERS where the mines are. Sort by distance with unit.x/unit.y on the Lua side.
sol::table Game_Beacons(sol::object oreArg, sol::this_state ts) {
  sol::state_view L(ts);
  sol::table out = L.create_table();
  int wantOre = -2;  // -2 = any; 0 = Common; 1 = Rare
  if (oreArg.is<std::string>()) {
    const std::string s = oreArg.as<std::string>();
    wantOre = (s == "Rare") ? 1 : (s == "Common") ? 0 : -2;
  }
  int i = 1;
  for (auto it = _Player(kGaiaPlayer).GetBeacons(); it; ++it) {
    Unit b = *it;
    if (b.GetType() != MapID::MiningBeacon) continue;   // skip wreckage / fumaroles / magma vents
    if (wantOre != -2) {
      auto*const mo = b.GetMapObject<Tethys::MapObj::MiningBeacon>();
      if ((mo == nullptr) || (static_cast<int>(mo->GetOreType()) != wantOre)) continue;
    }
    out[i++] = b;
  }
  return out;
}

// game.find_place(near, type) -> {x=,y=} (author coords) of a clear spot near `near` to place a
// structure/unit of `type`, or nil if none. Wraps OP2's own deploy-site finder (passable, unoccupied,
// footprint-aware). The returned point round-trips: pass it straight to unit:build / create_unit.
sol::object Game_FindPlace(sol::object from, const std::string& type, sol::this_state ts) {
  sol::state_view L(ts);
  const MapID t = MapIdFromName(type);
  const Location spot = GameMap::FindUnitPlacementLocation(LocFrom(from), t);
  if (spot.x < 0) return sol::make_object(L, sol::lua_nil);
  sol::table out = L.create_table();
  out["x"] = spot.x - GameMap::GetPaddingWidth() + 1;
  out["y"] = spot.y + 1;
  return out;
}

// Cell passability: cliffs, walls, and the two "impassible" types block; everything else is traversable.
static bool CellPassable(int ct) {
  switch (ct) {
    case 1: case 6:             // Impassible2 / Impassible1
    case 8: case 9: case 10:    // NorthCliffs / CliffsHighSide / CliffsLowSide
    case 23: case 24: case 25:  // NormalWall / MicrobeWall / LavaWall
      return false;
    default: return true;
  }
}

// A tile is buildable if its terrain is passable and nothing already occupies it (wall/building) and it
// isn't lava. Reads the tile flags directly (safe on in-bounds coords) - we deliberately do NOT use the
// engine's CanPlaceStructure thunk: it dereferences global state that isn't valid when called from a
// script and crashes (eip inside CanPlaceStructure, read of null+0x10), even for a valid in-bounds tile.
static bool TileBuildable(Location loc) {
  return CellPassable(static_cast<int>(GameMap::GetCellType(loc)))
         && (GameMap::GetWallOrBuilding(loc) == false)
         && (GameMap::GetLavaPresent(loc)    == false);
}

// game.cell(x, y) -> { passable, cliff, lava, blocked, type } for the tile at author coords (x, y).
// This is how the AI "sees" terrain - for avoiding cliffs/lava, zoning, and finding choke points.
sol::table Game_Cell(int x, int y, sol::this_state ts) {
  sol::state_view L(ts);
  const Location loc = GameMap::At(x, y);            // At() masks/wraps, so any (x,y) is in-bounds
  const int ct = static_cast<int>(GameMap::GetCellType(loc));
  sol::table out = L.create_table();
  out["type"]     = ct;
  out["passable"] = CellPassable(ct);
  out["cliff"]    = (ct == 8) || (ct == 9) || (ct == 10);
  out["lava"]     = GameMap::GetLavaPresent(loc);
  out["blocked"]  = GameMap::GetWallOrBuilding(loc);  // a wall or building already occupies the tile
  return out;
}

// A structure type's MapObjectType by name, or nullptr for an unknown name or a type that isn't a structure (only
// a structure's stats have a building size; MapIDs CommonOreMine..Tokamak are the structures).
static Tethys::MapObjectType* StructureType(const std::string& typeName) {
  const MapID id = MapIdFromName(typeName);
  if ((id < MapID::CommonOreMine) || (id > MapID::Tokamak)) return nullptr;
  return Tethys::MapObjectType::GetInstance(static_cast<int>(id));
}

// game.find_build_site(type, from [, maxRadius]) -> {x=,y=} the DoBuild anchor (bottom-right, author
// coords) of the nearest spot to `from` where the WHOLE footprint of `type` is buildable, or nil (nil too for a
// type that isn't a structure).
// Rings outward and checks every footprint tile with TileBuildable (the tile flags, not the engine's
// CanPlaceStructure) - so it skips existing buildings, walls, cliffs, lava, etc. Feed the result straight
// to unit:build(). This is the proper "place a structure without colliding with what's already on the map".
sol::object Game_FindBuildSite(const std::string& typeName, sol::object from, sol::optional<int> maxRadius,
                               sol::this_state ts) {
  sol::state_view L(ts);
  Tethys::MapObjectType*const mot = StructureType(typeName);
  if (mot == nullptr) return sol::make_object(L, sol::lua_nil);
  const Location origin = LocFrom(from);
  const int maxR = maxRadius.value_or(30);
  // Valid internal-tile bounds. The tile-flag reads don't range-check (they are safe only on in-bounds
  // coords), and the base can sit in a corner so the ring runs off the edge - so skip sites past them.
  const int pad  = GameMap::GetPaddingWidth();
  const int minX = pad, maxX = pad + GameMap::GetWidth() - 1;
  const int minY = 0,   maxY = GameMap::GetHeight() - 1;
  for (int r = 0; r <= maxR; ++r) {
    for (int dy = -r; dy <= r; ++dy) {
      const int ay = (dy < 0) ? -dy : dy;
      for (int dx = -r; dx <= r; ++dx) {
        const int ax = (dx < 0) ? -dx : dx;
        if (std::max(ax, ay) != r) continue;          // only the ring shell at distance r
        const Location center(origin.x + dx, origin.y + dy);
        // Validate the TOTAL footprint (building + 1-tile build clearance) so structures aren't jammed
        // together; OP2 requires that border clear. includeBorder=true gives the total rect.
        const MapRect area = mot->GetTileRect(center, true);
        if ((area.x1 < minX) || (area.x2 > maxX) || (area.y1 < minY) || (area.y2 > maxY)) continue;  // off-map
        // GetTileRect clips to the map, so a site running off an edge comes back smaller and passes the check
        // above; skip one whose rect isn't the full footprint.
        if (((area.x2 - area.x1 + 1) != mot->stats_.building.width + 2) ||
            ((area.y2 - area.y1 + 1) != mot->stats_.building.height + 2)) continue;
        bool ok = true;
        for (int ty = area.y1; ok && (ty <= area.y2); ++ty)
          for (int tx = area.x1; tx <= area.x2; ++tx)
            if (TileBuildable(Location(tx, ty)) == false) { ok = false; break; }
        if (ok) {
          const MapRect core = mot->GetTileRect(center, false);  // the building's own tiles
          sol::table out = L.create_table();
          out["x"] = core.x2 - GameMap::GetPaddingWidth() + 1;   // DoBuild anchor = building bottom-right
          out["y"] = core.y2 + 1;
          return out;
        }
      }
    }
  }
  return sol::make_object(L, sol::lua_nil);
}

// game.footprint(type) -> { w, h, tw, th }: a structure's core size (w x h tiles) and its total size
// including the 1-tile build-clearance border (tw x th), or nil for an unknown type or one that isn't a structure.
// Lets the AI reason about how big each building is (spacing, base layout). E.g. Nursery = 2x2 (4x4 total), Vehicle
// Factory = 4x3 (6x5 total).
sol::object Game_Footprint(const std::string& typeName, sol::this_state ts) {
  sol::state_view L(ts);
  Tethys::MapObjectType*const mot = StructureType(typeName);
  if (mot == nullptr) return sol::make_object(L, sol::lua_nil);
  // The size comes from the type's stats, not GetTileRect round a sample tile: that rect is clipped to the
  // map, so on a small map (64x64, where a tile like (100, 100) is off it) it would come back 1x1.
  const int w = mot->stats_.building.width, h = mot->stats_.building.height;
  sol::table out = L.create_table();
  out["w"]  = w;      out["h"]  = h;
  out["tw"] = w + 2;  out["th"] = h + 2;              // plus the 1-tile build-clearance border
  return out;
}

// game.can_place(x, y) -> is a structure tile buildable here? Reads the tile's terrain + occupancy
// flags (passable, no wall/building, no lava). Check every tile of a footprint to validate a whole
// site. (Answers "is something already here?" from the live tile flags - no external base-layout file.)
bool Game_CanPlace(int x, int y) {
  return TileBuildable(GameMap::At(x, y));
}

// game.map_size() -> { w, h } playable tile dimensions; author coords run 1..w / 1..h.
sol::table Game_MapSize(sol::this_state ts) {
  sol::state_view L(ts);
  sol::table out = L.create_table();
  out["w"] = GameMap::GetWidth();
  out["h"] = GameMap::GetHeight();
  return out;
}

// True if `techId` is a real tech in the loaded tech tree. Research::GetTechNum returns -1 for an
// unknown id (it binary-searches the sorted tech table), so this is a safe pre-check for ANY int.
// We need it because HasTechnology / MarkResearchComplete / DoResearch do NOT bounds-check the id and
// CRASH the game on a bad one (they index off the -1), unlike GetTechInfo which does guard it.
bool IsValidTechId(int techId) {
  Tethys::Research*const r = Tethys::Research::GetInstance();
  return (r != nullptr) && (r->GetTechNum(techId) != -1);
}

// Report (don't crash on) a bad tech id: full detail to the mission log, a SHORT line in-game.
// OP2's comms panel truncates long messages, so the in-game text is kept terse.
void ReportBadTech(const char* fn, int techId) {
  LOG_MISSION(ERROR) << "OP2Lua: invalid tech id " << techId << " passed to " << fn
                     << "() - ignored (an unknown tech id would crash the game).";
  Game::AddMessage("OP2Lua: bad tech id " + std::to_string(techId) + " (" + fn + ")",
                   kDefaultMsgSound, Tethys::AllPlayers);
}

// ---- read-only queries for computer-player scripts (costs, balances, research, survey, what was ordered) ----
// Each reads the game's own state and changes nothing, so they are safe in a multiplayer game: every machine reads
// the same values.

// A 0-based player number in range for the game's per-player arrays.
bool ValidPlayerNum(int num) { return (num >= 0) && (num < static_cast<int>(Tethys::MaxPlayers)); }

// player:cost(type [, weapon]) -> { common=, rare= }: what `type` costs this player's colony (the type's stats for
// the player, as research left them), plus a turret weapon's for a vehicle or Guard Post (an unknown or missing
// weapon adds nothing); nil for an unknown type.
sol::object Player_Cost(int num, const std::string& typeName, sol::object weaponArg, sol::this_state ts) {
  sol::state_view L(ts);
  const MapID type = MapIdFromName(typeName);
  Tethys::MapObjectType*const mot = (type != MapID::None) ? Tethys::MapObjectType::GetInstance(static_cast<int>(type)) : nullptr;
  if ((mot == nullptr) || !ValidPlayerNum(num)) return sol::make_object(L, sol::lua_nil);
  int common = mot->playerStats_[num].commonCost, rare = mot->playerStats_[num].rareCost;
  const MapID weapon = weaponArg.is<std::string>() ? WeaponFor(type, MapIdFromName(weaponArg.as<std::string>())) : MapID::None;
  Tethys::MapObjectType*const w = (weapon != MapID::None) ? Tethys::MapObjectType::GetInstance(static_cast<int>(weapon)) : nullptr;
  if (w != nullptr) {
    common += w->playerStats_[num].commonCost;
    rare   += w->playerStats_[num].rareCost;
  }
  sol::table out = L.create_table();
  out["common"] = common;
  out["rare"]   = rare;
  return out;
}

// game.tech(id) -> { id, lab, eden_cost, plymouth_cost, max_scientists, requires = { ids } } from the loaded tech
// tree, or nil for a topic the tree hasn't. lab: 1 Basic, 2 Standard, 3 Advanced, 0 none (a free topic). A cost of
// -1: not available to that colony. `requires` lists the tree file's REQUIRES: the loader makes the tree's root (topic
// 0, which every player has) the prerequisite of a topic without any, and that is left out.
sol::object Game_Tech(int techId, sol::this_state ts) {
  sol::state_view L(ts);
  Tethys::Research*const r = Tethys::Research::GetInstance();
  Tethys::TechInfo*const t = (r != nullptr) ? r->GetTechInfo(techId) : nullptr;
  if (t == nullptr) return sol::make_object(L, sol::lua_nil);
  sol::table out = L.create_table();
  out["id"]             = t->techID;
  out["lab"]            = static_cast<int>(t->lab);
  out["eden_cost"]      = t->edenCost;
  out["plymouth_cost"]  = t->plymouthCost;
  out["max_scientists"] = t->maxScientists;
  sol::table requires = L.create_table();
  int n = 0;
  for (int i = 0; i < t->numRequiredTechs; ++i) {
    const int num = t->pRequiredTechNum[i];
    if ((num >= 0) && (num < r->numTechs_) && (r->ppTechInfos_[num]->techID != 0)) requires[++n] = r->ppTechInfos_[num]->techID;
  }
  out["requires"] = requires;
  return out;
}

// player:can_research(id) - whether the player may start the topic now (Research::CanResearchTech 0x4733C0, what a
// lab's topic list offers): not researched yet, available to its colony, every prerequisite researched, within the
// mission's tech level. False for a topic the tree hasn't.
bool Player_CanResearch(int num, int techId) {
  Tethys::Research*const r = Tethys::Research::GetInstance();
  if ((r == nullptr) || !ValidPlayerNum(num)) return false;
  const int techNum = r->GetTechNum(techId);
  return (techNum != -1) && (r->CanResearchTech(num, techNum) != 0);
}

// beacon:surveyed_by(player) - whether a mining beacon counts as surveyed for the player (MiningBeacon::IsSurveyed
// 0x4055F0: the player or an ally surveyed it, or the player has an EDWARD Satellite up). False for anything else.
bool Unit_SurveyedBy(Unit& u, sol::object player) {
  const int num = PlayerNumFrom(player);
  if (!ValidPlayerNum(num) || (u.GetType() != MapID::MiningBeacon)) return false;
  auto*const mo = u.GetMapObject<Tethys::MapObj::MiningBeacon>();
  return (mo != nullptr) && (mo->IsSurveyed(num) != 0);
}

// A dword of a unit's map object at `offset`, for fields Tethys leaves unnamed.
int MapObjField(const Unit& u, size_t offset) {
  const auto*const mo = u.GetMapObject();
  return (mo != nullptr) ? *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(mo) + offset) : 0;
}

// lab.research_topic -> the tech id a lab is researching, nil when it isn't. While its command is Research, the lab
// keeps the topic's index in the tree at +0x60.
sol::object Unit_ResearchTopic(const Unit& u, sol::this_state ts) {
  sol::state_view L(ts);
  Tethys::Research*const r = Tethys::Research::GetInstance();
  if (!u.IsBuilding() || (u.GetCommand() != Tethys::CommandType::Research) || (r == nullptr))
    return sol::make_object(L, sol::lua_nil);
  const int num = MapObjField(u, 0x60);
  if ((num < 0) || (num >= r->numTechs_)) return sol::make_object(L, sol::lua_nil);
  return sol::make_object(L, r->ppTechInfos_[num]->techID);
}

// factory.producing -> the type name a factory is making (a vehicle, or a kit at the Structure Factory), nil when it
// makes nothing; factory.progress -> how far along, 0..1: nil when it makes nothing, 0 until the build timer runs. The product's MapID is at +0x60 (itemToProduce_), its build time at +0x68, and the time left in the action
// timer while the factory works (action OperationalWait).
// What a factory is making: MapID::None when it isn't a factory, has no Produce command, or makes nothing.
static MapID FactoryProduct(const Unit& u) {
  if (!u.IsFactory() || (u.GetCommand() != Tethys::CommandType::Produce)) return MapID::None;
  const auto* f = u.GetMapObject<Tethys::FactoryBuilding>();
  return (f == nullptr) ? MapID::None : static_cast<MapID>(f->itemToProduce_);
}
sol::object Unit_Producing(const Unit& u, sol::this_state ts) {
  sol::state_view L(ts);
  const MapID t = FactoryProduct(u);
  if (t == MapID::None) return sol::make_object(L, sol::lua_nil);
  return sol::make_object(L, NameFromMapId(t));
}
sol::object Unit_Progress(const Unit& u, sol::this_state ts) {
  sol::state_view L(ts);
  // nil whenever `producing` is: the factory makes nothing.
  if (FactoryProduct(u) == MapID::None) return sol::make_object(L, sol::lua_nil);
  const int total = MapObjField(u, 0x68);
  if ((u.GetAction() != Tethys::ActionType::OperationalWait) || (total <= 0)) return sol::make_object(L, 0.0);
  const int left = std::clamp(u.GetMapObject()->actionTimer_, 0, total);
  return sol::make_object(L, double(total - left) / total);
}

// unit.target -> the unit a vehicle's Attack order is aimed at, nil otherwise (no Attack order, a ground target, or
// the target gone). The order's target is at +0x48: an object index with the top bit set, else a tile (0x423AB0).
sol::object Unit_Target(const Unit& u, sol::this_state ts) {
  sol::state_view L(ts);
  if (!u.IsVehicle() || (u.GetCommand() != Tethys::CommandType::Attack)) return sol::make_object(L, sol::lua_nil);
  const uint32_t t = static_cast<uint32_t>(MapObjField(u, 0x48));
  if ((t & 0x80000000u) == 0) return sol::make_object(L, sol::lua_nil);
  const Unit target(static_cast<int>(t & 0xFFFFu));
  return target.IsLive() ? sol::make_object(L, target) : sol::make_object(L, sol::lua_nil);
}

}  // namespace

// Issue ONE queued order's game command, shielded with SEH. Commanding a unit OP2 hasn't finished
// registering can fault deep inside CmdMove/CmdAttack (reads 0xffffffff) - IsLive() flips true before
// the MapObject is fully command-ready, so the pre-check isn't enough. Rather than let that take down
// the whole game, we catch the fault and report failure so the caller can retry next cycle. (Unit and
// Location are trivially destructible, so __try is legal in this function - no C++ stack unwinding.)
static bool IssueOrderGuarded(DeferredOrder& o) {
  __try {
    const Location loc(o.x, o.y);
    switch (o.kind) {
      // Attack a specific unit. If the target died before the order flushed, just advance
      // to where it last was (units auto-fire on whatever is in range).
      case OrderKind::AttackUnit:   if (o.target.IsLive()) o.unit.DoAttack(o.target);
                                    else                   o.unit.DoMove(loc);          break;
      // Attack the ground at a tile (siege a spot). Rarely what you want.
      case OrderKind::AttackGround: o.unit.DoAttack(loc);                                break;
      // Move there; armed units still fire on enemies that come into range en route.
      default:                      o.unit.DoMove(loc);                                  break;
    }
    return true;
  } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
    return false;
  }
}

// AI base-building commands, issued immediately. The AI's starting ConVecs / trucks / RoboMiner /
// RoboSurveyor are long-aged by the time it acts, so the deferred-order safety (which protects
// freshly-spawned units, see FlushUnitOrders) isn't needed here. We still SEH-guard the calls that
// reach OP2's Cmd* path so a bad unit state logs+skips instead of crashing the game. (Unit / Location
// / MapID are trivially destructible, so __try is legal - no C++ unwinding.) NOTE: a unit the AI
// *produces* later is fresh - command it after a short delay (the brain schedules that), don't call
// these on a unit created the same tick.
static bool GuardedDeploy(Unit u, Location c)          { __try { u.DoDeploy(c);         return true; } __except (1) { return false; } }
static bool GuardedBuild(Unit u, Location br)          { __try { u.DoBuild(br);         return true; } __except (1) { return false; } }
static bool GuardedMiningRoute(Unit u, Unit m, Unit s) { __try { u.DoMiningRoute(m, s); return true; } __except (1) { return false; } }
static bool GuardedDock(Unit u, Unit at)               { __try { u.DoDock(at);          return true; } __except (1) { return false; } }
static bool GuardedProduce(Unit u, MapID t, MapID w)   { __try { u.DoProduce(t, w);     return true; } __except (1) { return false; } }
// More order-issuing commands, SEH-guarded the same way (they touch OP2's Cmd*/pathfinder path).
static bool GuardedGuard(Unit u, Unit t)               { __try { u.DoGuard(t);          return true; } __except (1) { return false; } }
static bool GuardedStandGround(Unit u, Location l)     { __try { u.DoStandGround(l);    return true; } __except (1) { return false; } }
static bool GuardedRepair(Unit u, Unit t)              { __try { u.DoRepair(t);         return true; } __except (1) { return false; } }
static bool GuardedDismantle(Unit u, Unit t)           { __try { u.DoDismantle(t);      return true; } __except (1) { return false; } }
static bool GuardedReprogram(Unit u, Unit t)           { __try { u.DoReprogram(t);      return true; } __except (1) { return false; } }
static bool GuardedDockGarage(Unit u, Unit g)          { __try { u.DoDockAtGarage(g);   return true; } __except (1) { return false; } }
static bool GuardedTransfer(Unit u, int p)             { __try { u.DoTransfer(p);       return true; } __except (1) { return false; } }
static bool GuardedBuildWall(Unit u, MapID t, MapRect a){ __try { u.DoBuildWall(t, a);  return true; } __except (1) { return false; } }
static bool GuardedRemoveWall(Unit u, MapRect a)       { __try { u.DoRemoveWall(a);     return true; } __except (1) { return false; } }
static bool GuardedDoze(Unit u, MapRect a)             { __try { u.DoDoze(a);           return true; } __except (1) { return false; } }
static bool GuardedSalvage(Unit u, MapRect a, Unit g)  { __try { u.DoSalvage(a, g);     return true; } __except (1) { return false; } }
static bool GuardedLaunch(Unit u, Location t)          { __try { u.DoLaunch(t);         return true; } __except (1) { return false; } }
// Unit::DoInfect (0x476B90), Outpost2.exe's export, called as the stock missions call it (Eden 1 infects its
// buildings as the volcano comes).
static void GameDoInfect(Unit& u) {
  using Fn = void(__thiscall*)(Unit*);
  reinterpret_cast<Fn>(0x476B90)(&u);
}
// Unit::SetDamage (0x476200) and Unit::SetOppFiredUpon (0x475F60), Outpost2.exe's exports, as the stock missions
// call them on the units their setups make (Eden 11's computer guards fire back; Eden 4's damaged buildings).
static void GameSetDamage(Unit& u, int damage) {
  using Fn = void(__thiscall*)(Unit*, int);
  reinterpret_cast<Fn>(0x476200)(&u, damage);
}
static void GameSetOppFiredUpon(Unit& u, int on) {
  using Fn = void(__thiscall*)(Unit*, int);
  reinterpret_cast<Fn>(0x475F60)(&u, on);
}
static bool GameIsDiscovered(const Unit& u) {
  using Fn = int(__thiscall*)(const Unit*);
  return reinterpret_cast<Fn>(0x476120)(&u) != 0;   // Unit::isDiscovered, exported (a wreck the player has found)
}
// Unit::HasOccupiedBay (0x476080), Outpost2.exe's export: whether a Garage, Structure Factory or Spaceport has
// something in a bay.
static bool GameHasOccupiedBay(const Unit& u) {
  using Fn = int(__thiscall*)(const Unit*);
  return reinterpret_cast<Fn>(0x476080)(&u) != 0;
}
// A Spaceport's launch at a pixel, as the stock missions' Unit::DoLaunch(px, py, 0) asks (CES1's StageDone fires at
// FindEMPMissleTarget's tile times 32); Tethys's DoLaunch takes a tile and aims at its centre. Sent as a Launch
// command packet through the owner's ProcessCommandPacket.
static void LaunchAtPixel(Unit u, int px, int py) {
  Tethys::CommandPacket packet = { Tethys::CommandType::Launch, sizeof(Tethys::LaunchCommand) };
  packet.data.launch.unitID       = u.GetID();
  packet.data.launch.targetPixelX = px;
  packet.data.launch.targetPixelY = py;
  auto*const owner = Tethys::GameImpl::GetInstance()->GetPlayer(u.GetOwner());
  if (owner) owner->ProcessCommandPacket(packet);
}
static bool GuardedLaunchAtPixel(Unit u, int px, int py) { __try { LaunchAtPixel(u, px, py); return true; } __except (1) { return false; } }
static bool GuardedTransferCargo(Unit u, int bay)      { __try { u.DoTransferCargo(bay); return true; } __except (1) { return false; } }
// Unit::IsBusy as Outpost2.exe exports it (0x475E60), the call the stock missions make: whether the unit has a
// command (MapObject +0x21, Tethys's command_), e.g. a Spaceport until its rocket is built. Tethys's IsBusy reads the
// byte before it (+0x20, isBusy_), which goes false while a Spaceport still builds.
static bool GameIsBusy(const Unit& u) {
  if (!u.IsValid()) return false;
  using IsBusyFn = int(__thiscall*)(const Unit*);
  return reinterpret_cast<IsBusyFn>(0x475E60)(&u) != 0;
}
// Unit::DoDevelop(map_id), exported by Outpost2.exe at 0x4764F0 (Tethys has no wrapper): a Spaceport builds a
// rocket or its cargo, as the stock missions ask (AutoDemo 3's EMP missile). DoProduce doesn't start one there.
static bool GuardedDevelop(Unit u, MapID t) {
  using DoDevelopFn = void(__thiscall*)(Unit*, MapID);
  __try { reinterpret_cast<DoDevelopFn>(0x4764F0)(&u, t); return true; } __except (1) { return false; }
}

void FlushUnitOrders() {
  // Orders a unit couldn't take when they were given (the guarded call faulted), retried once per pump. Orders to a
  // unit made this tick go out at once, as a mission DLL's DoMove does: the old crash on them (0.5.1) was Tethys's raw
  // CmdMove thunk, not the unit's age, and DoMove has sent a command packet since 0.5.2.
  constexpr int kMaxAttempts = 20;
  const int now = Game::Tick();
  std::vector<DeferredOrder> retry;
  int issued = 0;
  for (auto& o : gOrders) {
    if (!o.unit.IsLive()) continue;                        // unit gone before it could take the order -> drop it
    if (IssueOrderGuarded(o)) { ++issued; continue; }     // command issued cleanly
    if (++o.attempts < kMaxAttempts) retry.push_back(o);
    else LOG_AI(WARNING) << "dropped a unit order after " << kMaxAttempts << " attempts";
  }
  // Issuing queued unit orders (move/attack) is scripted-unit activity -> the AI log.
  if (issued > 0) LOG_AI(INFO) << "issued " << issued << " queued unit order(s) at tick " << now;
  gOrders.swap(retry);
}
void ResetUnitOrders() { gOrders.clear(); }
void StopMissionWav() { StopWav(); }

// Gives a unit an order at once, as a mission DLL's DoMove / DoAttack do, a unit made this tick included. One it can't
// take (not live, or the guarded call faulted) is queued and retried (FlushUnitOrders). A newer order replaces one
// still queued for the unit.
static void Order(DeferredOrder o) {
  // The game's Unit::DoMove (0x476240) ignores a move to a tile off its grid (x outside the padded width at 0x54F808,
  // y outside the height at 0x54F810), so the unit keeps the order it had; a mission's off-map move does the same here.
  if (o.kind == OrderKind::Move && !OnGrid(Location(o.x, o.y))) return;
  const int id = o.unit.GetID();
  gOrders.erase(std::remove_if(gOrders.begin(), gOrders.end(), [id](const DeferredOrder& q) { return q.unit.GetID() == id; }),
                gOrders.end());
  if (o.unit.IsLive() && IssueOrderGuarded(o)) return;
  gOrders.push_back(o);
}

void ResetTriggers() { gTriggerFns.clear(); }

void FireTrigger(void* pArgs) {
  const auto* a = static_cast<const Tethys::TethysAPI::OnTriggerArgs*>(pArgs);
  if (a == nullptr) return;
  const auto it = gTriggerFns.find(a->trigger.GetID());
  if (it == gTriggerFns.end()) return;
  sol::protected_function fn = it->second;   // a copy: the function may make more triggers
  Budget::Call budget;
  try {
    sol::protected_function_result r = fn(a->trigger);
    if (!r.valid()) { sol::error e = r; LOG_MISSION(ERROR) << "trigger " << a->trigger.GetID() << ": " << e.what(); }
  } catch (const std::exception& e) {
    LOG_MISSION(ERROR) << "trigger " << a->trigger.GetID() << ": " << e.what();
  }
}

// --- AI groups (ai.*) ------------------------------------------------------------------------
// The game's own computer-player machinery, as the stock missions drive it: a BuildingGroup rebuilds the
// structures, tubes and walls it has recorded and produces the vehicles other groups ask for; a MiningGroup runs
// its trucks between a mine and a smelter; a FightGroup guards (or patrols, or attacks). The game runs them; a
// mission only sets them up. A Group handle holds the game's stub id and which kind it is.
enum class GroupKind { Plain, Building, Fight, Mining };
struct LuaGroup {
  int       id   = -1;
  GroupKind kind = GroupKind::Plain;
};
// A UnitBlock: a table of unit records (the stock missions' UnitRecord arrays). The game keeps pointers to the
// records, so blocks live as long as the process.
struct LuaUnitBlock {
  Tethys::TethysAPI::UnitBlock* block = nullptr;
};

using Tethys::TethysAPI::BuildingGroup;
using Tethys::TethysAPI::FightGroup;
using Tethys::TethysAPI::MiningGroup;
using Tethys::TethysAPI::ScGroup;
using Tethys::TethysAPI::UnitBlock;
using Tethys::TethysAPI::UnitRecord;

const char* GroupKindName(GroupKind k) {
  switch (k) {
    case GroupKind::Building: return "Building";
    case GroupKind::Fight:    return "Fight";
    case GroupKind::Mining:   return "Mining";
    default:                  return "Plain";
  }
}
bool GroupIs(const LuaGroup& g, GroupKind k, const char* fn) {
  if (g.id >= 0 && g.kind == k) return true;
  LOG_AI(WARNING) << "group:" << fn << ": needs a " << GroupKindName(k) << " group (this is "
                  << GroupKindName(g.kind) << " #" << g.id << ")";
  return false;
}
// An optional weapon (or kit) name as `carrier` mounts it (WeaponFor); None when absent.
MapID WeaponOpt(MapID carrier, const sol::object& o) {
  return o.is<std::string>() ? WeaponFor(carrier, MapIdFromName(o.as<std::string>())) : MapID::None;
}

// ai.unit_block{ { type=, at=, direction=|rotation=, weapon=, cargo=, amount=, classification= }, ... } - `at` in
// author coords; `rotation` is the game's heading (0-255, East 0, South 64), `direction` a name as create_unit takes.
// Unknown types are skipped with a warning.
// A block of `recs` (the end record is added here), kept with its records for the process's life.
std::map<const UnitBlock*, const std::vector<UnitRecord>*> gBlockRecords;
LuaUnitBlock MakeUnitBlock(std::vector<UnitRecord> recs) {
  static std::deque<std::vector<UnitRecord>> tables;
  static std::deque<UnitBlock>               blocks;
  std::vector<UnitRecord>& kept = tables.emplace_back(std::move(recs));
  kept.emplace_back();   // the table ends with a record of no type
  UnitBlock* b = &blocks.emplace_back(kept.data());
  gBlockRecords[b] = &kept;
  return LuaUnitBlock{ b };
}

LuaUnitBlock Ai_UnitBlock(sol::table list) {
  std::vector<UnitRecord> recs;
  for (size_t i = 1; i <= list.size(); ++i) {
    sol::optional<sol::table> r = list[i];
    if (!r) continue;
    const std::string typeName = r->get_or("type", std::string());
    const MapID type = MapIdFromName(typeName);
    if (type == MapID::None) { LOG_AI(WARNING) << "ai.unit_block: unknown type '" << typeName << "'"; continue; }
    const Location at = LocFrom(r->get<sol::object>("at"));
    sol::optional<int> rot = (*r)["rotation"];
    sol::optional<std::string> dir = (*r)["direction"];
    const int rotation = rot ? *rot : (dir ? DirFromName(*dir) * 32 : 0);
    const std::string cargo = r->get_or("cargo", std::string());
    const int cargoType = cargo.empty() ? 0 : std::max(0, CargoTypeFromName(cargo));
    // Classification NotSet (0x10) unless given, as most records of the stock missions' tables have it (AutoDemo 2's,
    // La Corrida's); CEP1's Scouts have 9 (Colony).
    const int classification = r->get_or("classification", static_cast<int>(Tethys::UnitClassification::NotSet));
    recs.emplace_back(type, at.x, at.y, 0, rotation, WeaponOpt(type, r->get<sol::object>("weapon")),
                      static_cast<Tethys::UnitClassification>(classification), static_cast<uint16_t>(cargoType),
                      static_cast<uint16_t>(r->get_or("amount", 0)));
  }
  return MakeUnitBlock(std::move(recs));
}

LuaGroup Ai_Create(const sol::object& player, GroupKind kind) {
  const int num = PlayerNumFrom(player);
  if (!ValidPlayerNum(num)) { LOG_AI(WARNING) << "ai: a group needs a player (1-7 or a Player)"; return LuaGroup{}; }
  int id = -1;
  switch (kind) {
    case GroupKind::Building: id = Tethys::TethysAPI::CreateBuildingGroup(_Player(num)).GetID(); break;
    case GroupKind::Fight:    id = Tethys::TethysAPI::CreateFightGroup(_Player(num)).GetID();    break;
    case GroupKind::Mining:   id = Tethys::TethysAPI::CreateMiningGroup(_Player(num)).GetID();   break;
    default: break;
  }
  if (id < 0) LOG_AI(WARNING) << "ai: the game made no " << GroupKindName(kind) << " group for player " << num;
  else LOG_AI(INFO) << "ai: " << GroupKindName(kind) << " group #" << id << " for player " << num;
  return LuaGroup{ id, kind };
}

sol::table Group_Units(const LuaGroup& g, sol::this_state ts) {
  sol::state_view L(ts);
  sol::table out = L.create_table();
  if (g.id < 0) return out;
  ScGroup sg(g.id);
  if (sg.GetImpl() == nullptr) return out;
  int i = 0;
  for (Unit u : sg) out[++i] = u;
  return out;
}

// trigger.damaged{ group=, damage=1 } - a share of a fight group destroyed: 1 all of it, 2 three quarters, 3 half
// (Plymouth 6's convoy escort).
Trigger Trigger_Damaged(sol::table o) {
  const sol::object g = o.get<sol::object>("group");
  if (!g.is<LuaGroup>() || g.as<LuaGroup>().id < 0) {
    LOG_MISSION(WARNING) << "trigger.damaged: needs a group";
    return Trigger();
  }
  return Remember(Tethys::TethysAPI::CreateDamagedTrigger(ScGroup(g.as<LuaGroup>().id),
                                                          Tethys::TethysAPI::TriggerDamage(o.get_or("damage", 1)),
                                                          TriggerFnName(o), OneShot(o, false), Enabled(o)),
                  o, "damaged");
}
void RegisterGroupBindings(sol::state& lua) {
  // trigger.damaged is registered here, where LuaGroup is known; RegisterBindings has made the trigger table.
  if (sol::optional<sol::table> trig = lua["trigger"]) trig->set_function("damaged", &Trigger_Damaged);
  sol::table ai = lua.create_named_table("ai");
  ai.set_function("building_group", [](sol::object p) { return Ai_Create(p, GroupKind::Building); });
  ai.set_function("fight_group",    [](sol::object p) { return Ai_Create(p, GroupKind::Fight); });
  ai.set_function("mining_group",   [](sol::object p) { return Ai_Create(p, GroupKind::Mining); });
  ai.set_function("unit_block",     &Ai_UnitBlock);
  // block:create_units(player [, lights]): the block's units for `player`, in no group, as the game's
  // TethysGame::CreateUnitBlock makes a mission's exported table (CES1's computer base); returns how many were
  // made (0 without a player). Lights default to off.
  lua.new_usertype<LuaUnitBlock>("UnitBlock", sol::no_constructor,
      "create_units", [](const LuaUnitBlock& b, sol::object player, sol::optional<bool> lights) {
        const int num = PlayerArg(player, "create_units");
        return (b.block && num >= 0) ? b.block->CreateUnits(num, lights.value_or(false)) : 0; });

  lua.new_usertype<LuaGroup>(
      "Group", sol::no_constructor,
      "id",    sol::property([](const LuaGroup& g) { return g.id; }),
      "kind",  sol::property([](const LuaGroup& g) { return std::string(GroupKindName(g.kind)); }),
      "valid", sol::property([](const LuaGroup& g) { return g.id >= 0 && ScGroup(g.id).GetImpl() != nullptr; }),
      "units", &Group_Units,
      "unit_count", [](const LuaGroup& g) { return g.id >= 0 ? ScGroup(g.id).TotalUnitCount() : 0; },
      // The first unit in the group of a type (and weapon or kit), or nil. Without a weapon it walks the group's
      // units in order, as a mission's GroupEnumerator does: the game's own lookup by type and cargo takes MapID::Any
      // as a cargo to match, so it found no Structure Factory (Peacekeeper's computer base got no kits).
      "first_of_type", [](const LuaGroup& g, const std::string& type, sol::object weapon, sol::this_state ts) -> sol::object {
        sol::state_view L(ts);
        if (g.id < 0) return sol::make_object(L, sol::lua_nil);
        const MapID t = MapIdFromName(type);
        ScGroup sg(g.id);
        if (sg.GetImpl() == nullptr) return sol::make_object(L, sol::lua_nil);
        if (!weapon.is<std::string>()) {
          for (Unit u : sg) if (u.IsLive() && u.GetType() == t) return sol::make_object(L, u);
          return sol::make_object(L, sol::lua_nil);
        }
        const Unit u = sg.GetFirstOfType(t, WeaponFor(t, MapIdFromName(weapon.as<std::string>())));
        return u.IsLive() ? sol::make_object(L, u) : sol::make_object(L, sol::lua_nil); },
      "take_unit",   [](LuaGroup& g, Unit u) { if (g.id >= 0 && u.IsValid()) ScGroup(g.id).TakeUnit(u); },
      "remove_unit", [](LuaGroup& g, Unit u) { if (g.id >= 0 && u.IsValid()) ScGroup(g.id).RemoveUnit(u); },
      // Every unit of `other` moves to this group (CES1's sixth wave takes fight group 3 whole).
      "take_all_units", [](LuaGroup& g, LuaGroup& other) {
        if (g.id >= 0 && other.id >= 0) { ScGroup src(other.id); ScGroup(g.id).TakeAllUnits(&src); } },
      // Makes the block's units for the group's player, into the group (while it has fewer than 32).
      "add_units",   [](LuaGroup& g, const LuaUnitBlock& b) { if (g.id >= 0 && b.block) ScGroup(g.id).AddUnits(*b.block); },
      "set_lights",  [](LuaGroup& g, bool on) { if (g.id >= 0) ScGroup(g.id).SetLights(on); },
      "set_delete_when_empty", [](LuaGroup& g, bool on) { if (g.id >= 0) ScGroup(g.id).SetDeleteWhenEmpty(on); },
      // How many of a unit (and weapon, nil for none) the group wants: its building group makes the rest.
      "set_targ_count", [](LuaGroup& g, const std::string& type, sol::object weapon, int count) {
        if (g.id >= 0) ScGroup(g.id).SetTargCount(MapIdFromName(type), WeaponOpt(MapIdFromName(type), weapon), count); },
      "clear_targ_count", [](LuaGroup& g) { if (g.id >= 0) ScGroup(g.id).ClearTargCount(); },
      // set_targ_counts(block): as many of each unit (and weapon) as the block has (ScGroup::SetTargCount(UnitBlock&),
      // Eden 11's guards).
      "set_targ_counts", [](LuaGroup& g, const LuaUnitBlock& b) { if (g.id >= 0 && b.block) ScGroup(g.id).SetTargCount(*b.block); },
      "enable",  [](LuaGroup& g) { if (g.id >= 0) ScGroup(g.id).Enable(); },
      "disable", [](LuaGroup& g) { if (g.id >= 0) ScGroup(g.id).Disable(); },
      "destroy", [](LuaGroup& g) { if (g.id >= 0) { ScGroup(g.id).Destroy(); g.id = -1; } },

      // ---- BuildingGroup: what it keeps standing, and whom it builds for ----
      // set_rect: Building - where its ConVecs wait; Fight - where its units wait.
      "set_rect", [](LuaGroup& g, sol::object area) {
        if (g.id < 0) return;
        if (g.kind == GroupKind::Building) BuildingGroup(g.id).SetRect(RectFrom(area));
        else if (g.kind == GroupKind::Fight) FightGroup(g.id).SetRect(RectFrom(area));
        else LOG_AI(WARNING) << "group:set_rect: needs a Building or Fight group"; },
      // record_building(at, type [, kit or weapon] [, group]): a structure to keep standing; a factory's output
      // goes to `group`.
      "record_building", [](LuaGroup& g, sol::object at, const std::string& type, sol::object weapon, sol::object group) {
        if (!GroupIs(g, GroupKind::Building, "record_building")) return;
        BuildingGroup bg(g.id);
        const MapID t = MapIdFromName(type);
        if (group.is<LuaGroup>()) bg.RecordBuilding(LocFrom(at), t, WeaponOpt(t, weapon), ScGroup(group.as<LuaGroup>().id));
        else bg.RecordBuilding(LocFrom(at), t, WeaponOpt(t, weapon)); },
      // record_unit_block(block [, group]): every structure of the block.
      "record_unit_block", [](LuaGroup& g, const LuaUnitBlock& b, sol::object group) {
        if (!GroupIs(g, GroupKind::Building, "record_unit_block") || !b.block) return;
        BuildingGroup bg(g.id);
        if (group.is<LuaGroup>()) bg.RecordUnitBlock(*b.block, ScGroup(group.as<LuaGroup>().id));
        else bg.RecordUnitBlock(*b.block); },
      "record_tube", [](LuaGroup& g, sol::object at) {
        if (GroupIs(g, GroupKind::Building, "record_tube")) BuildingGroup(g.id).RecordTube(LocFrom(at)); },
      // record_tubes_touching(at): the tubes joined to the one at `at`.
      "record_tubes_touching", [](LuaGroup& g, sol::object at) {
        if (GroupIs(g, GroupKind::Building, "record_tubes_touching")) BuildingGroup(g.id).RecordConnectTubes(LocFrom(at)); },
      "record_wall", [](LuaGroup& g, sol::object at, sol::optional<std::string> type) {
        if (GroupIs(g, GroupKind::Building, "record_wall"))
          BuildingGroup(g.id).RecordWall(LocFrom(at), type ? MapIdFromName(*type) : MapID::Wall); },
      // record_veh_reinforce_group(group, priority): build the vehicles `group` wants (higher priority first).
      "record_veh_reinforce_group", [](LuaGroup& g, const LuaGroup& other, int priority) {
        if (GroupIs(g, GroupKind::Building, "record_veh_reinforce_group") && other.id >= 0)
          BuildingGroup(g.id).RecordVehReinforceGroup(ScGroup(other.id), priority); },
      "unrecord_veh_group", [](LuaGroup& g, const LuaGroup& other) {
        if (GroupIs(g, GroupKind::Building, "unrecord_veh_group") && other.id >= 0)
          BuildingGroup(g.id).UnRecordVehGroup(ScGroup(other.id)); },

      // ---- MiningGroup ----
      // setup(mine, smelter, area) or setup(mine, smelter, mine_type, smelter_type, area): trucks between the
      // structures at those tiles (or ones of those types to come), waiting in `area`.
      "setup", [](LuaGroup& g, sol::object mine, sol::object smelter, sol::object a, sol::object b, sol::object c) {
        if (!GroupIs(g, GroupKind::Mining, "setup")) return;
        // A mine or smelter not found (nil) would hand the game an empty unit or tile, which it faults on.
        if (mine == sol::lua_nil || smelter == sol::lua_nil) { LOG_MISSION(WARNING) << "setup: no mine or smelter"; return; }
        MiningGroup mg(g.id);
        if (a.is<std::string>()) mg.Setup(LocFrom(mine), LocFrom(smelter), MapIdFromName(a.as<std::string>()),
                                          MapIdFromName(b.as<std::string>()), RectFrom(c));
        else if (mine.is<Unit>() && smelter.is<Unit>()) mg.Setup(mine.as<Unit>(), smelter.as<Unit>(), RectFrom(a));
        else mg.Setup(LocFrom(mine), LocFrom(smelter), RectFrom(a)); },

      // ---- FightGroup ----
      "add_guarded_rect",    [](LuaGroup& g, sol::object area) {
        if (GroupIs(g, GroupKind::Fight, "add_guarded_rect")) FightGroup(g.id).AddGuardedRect(RectFrom(area)); },
      "clear_guarded_rects", [](LuaGroup& g) {
        if (GroupIs(g, GroupKind::Fight, "clear_guarded_rects")) FightGroup(g.id).ClearGuardedRects(); },
      "do_guard_rect",    [](LuaGroup& g) { if (GroupIs(g, GroupKind::Fight, "do_guard_rect")) FightGroup(g.id).DoGuardRect(); },
      "do_patrol_only",   [](LuaGroup& g) { if (GroupIs(g, GroupKind::Fight, "do_patrol_only")) FightGroup(g.id).DoPatrolOnly(); },
      // group:set_patrol({ {x, y}, ... } [, first]) - the route a patrolling group follows (FightGroup::SetPatrolMode):
      // up to 8 waypoints (more are ignored; unused slots are (-1, -1)); `first` is the route's first field as the
      // mission gives it, 0 if left out (CEP1's scouts: 2). The game keeps pointers to the route and its waypoints, so
      // both are kept for the process's life.
      "set_patrol",       [](LuaGroup& g, sol::table points, sol::optional<int> first) {
        if (!GroupIs(g, GroupKind::Fight, "set_patrol")) return;
        static std::deque<std::array<Location, 8>> routes;
        std::array<Location, 8>& w = routes.emplace_back();
        w.fill(Location(-1, -1));
        for (size_t i = 1; i <= points.size() && i <= w.size(); ++i) w[i - 1] = LocFrom(points[i]);
        static std::deque<Tethys::PatrolRoute> held;
        Tethys::PatrolRoute& route = held.emplace_back();
        route.field_00 = first.value_or(0);
        route.pWaypoints = w.data();
        FightGroup(g.id).SetPatrolMode(route); },
      "do_guard_group",   [](LuaGroup& g) { if (GroupIs(g, GroupKind::Fight, "do_guard_group")) FightGroup(g.id).DoGuardGroup(); },
      "do_guard_unit",    [](LuaGroup& g) { if (GroupIs(g, GroupKind::Fight, "do_guard_unit")) FightGroup(g.id).DoGuardUnit(); },
      "do_attack_enemy",  [](LuaGroup& g) { if (GroupIs(g, GroupKind::Fight, "do_attack_enemy")) FightGroup(g.id).DoAttackEnemy(); },
      "do_attack_unit",   [](LuaGroup& g) { if (GroupIs(g, GroupKind::Fight, "do_attack_unit")) FightGroup(g.id).DoAttackUnit(); },
      "do_exit_map",      [](LuaGroup& g) { if (GroupIs(g, GroupKind::Fight, "do_exit_map")) FightGroup(g.id).DoExitMap(); },
      "set_attack_type",  [](LuaGroup& g, const std::string& type) {
        if (GroupIs(g, GroupKind::Fight, "set_attack_type")) FightGroup(g.id).SetAttackType(MapIdFromName(type)); },
      "set_target_group", [](LuaGroup& g, const LuaGroup& other) {
        if (GroupIs(g, GroupKind::Fight, "set_target_group") && other.id >= 0) FightGroup(g.id).SetTargetGroup(ScGroup(other.id)); },
      "set_target_unit",  [](LuaGroup& g, Unit u) {
        if (GroupIs(g, GroupKind::Fight, "set_target_unit")) FightGroup(g.id).SetTargetUnit(u); },
      "set_follow_mode",  [](LuaGroup& g, int mode) {
        if (GroupIs(g, GroupKind::Fight, "set_follow_mode")) FightGroup(g.id).SetFollowMode(mode); },
      "set_combine_fire", [](LuaGroup& g, sol::optional<bool> on) {
        if (!GroupIs(g, GroupKind::Fight, "set_combine_fire")) return;
        if (on.value_or(true)) FightGroup(g.id).SetCombineFire(); else FightGroup(g.id).ClearCombineFire(); },
      sol::meta_function::equal_to, [](const LuaGroup& a, const LuaGroup& b) { return a.id == b.id; });
}

// When on, AI debug (log.ai) is also echoed to the in-game comms panel so a dev can watch the AI
// without leaving the game. Reset per mission (fresh Lua state) and toggled via log.show_ai_in_game().
static bool gAIToComms = false;

// ---- saved games (SaveGame.cpp) ----
// A game handle in a save: its kind, then its fields. Each is a value the game's own save keeps meaning: a unit's or
// stub's id, a player's number, tiles, a unit block's records.
enum HandleKind : uint8_t { kUnitHandle = 1, kTriggerHandle, kGroupHandle, kPlayerHandle, kRegionHandle, kBlockHandle };

bool WriteHandle(lua_State* L, int idx, Persist::Writer& w) {
  if (sol::stack::check<Unit>(L, idx, sol::no_panic)) {
    w.U8(kUnitHandle);
    w.I32(sol::stack::get<Unit&>(L, idx).GetID());
  } else if (sol::stack::check<Trigger>(L, idx, sol::no_panic)) {
    w.U8(kTriggerHandle);
    w.I32(sol::stack::get<Trigger&>(L, idx).GetID());
  } else if (sol::stack::check<LuaGroup>(L, idx, sol::no_panic)) {
    const LuaGroup& g = sol::stack::get<LuaGroup&>(L, idx);
    w.U8(kGroupHandle);
    w.I32(g.id);
    w.U8(static_cast<uint8_t>(g.kind));
  } else if (sol::stack::check<LuaPlayer>(L, idx, sol::no_panic)) {
    w.U8(kPlayerHandle);
    w.I32(sol::stack::get<LuaPlayer&>(L, idx).num);
  } else if (sol::stack::check<Region>(L, idx, sol::no_panic)) {
    const Region& r = sol::stack::get<Region&>(L, idx);
    w.U8(kRegionHandle);
    w.I32(r.x1); w.I32(r.y1); w.I32(r.x2); w.I32(r.y2);
  } else if (sol::stack::check<LuaUnitBlock>(L, idx, sol::no_panic)) {
    const LuaUnitBlock& b = sol::stack::get<LuaUnitBlock&>(L, idx);
    const auto it = gBlockRecords.find(b.block);
    if (it == gBlockRecords.end()) return false;
    const std::vector<UnitRecord>& recs = *it->second;
    w.U8(kBlockHandle);
    w.U32(static_cast<uint32_t>(recs.size() - 1));   // without the end record
    w.U32(static_cast<uint32_t>(sizeof(UnitRecord)));
    w.Bytes(recs.data(), (recs.size() - 1) * sizeof(UnitRecord));
  } else {
    return false;
  }
  return true;
}

bool PushHandle(lua_State* L, uint8_t kind, Persist::Reader& r) {
  switch (kind) {
    case kUnitHandle: { const int id = r.I32(); if (!r.Ok()) return false; sol::stack::push(L, Unit(id)); return true; }
    case kTriggerHandle: { const int id = r.I32(); if (!r.Ok()) return false; sol::stack::push(L, Trigger(id)); return true; }
    case kGroupHandle: {
      LuaGroup g;
      g.id = r.I32();
      g.kind = static_cast<GroupKind>(r.U8());
      if (!r.Ok() || static_cast<int>(g.kind) > static_cast<int>(GroupKind::Mining)) return false;
      sol::stack::push(L, g);
      return true;
    }
    case kPlayerHandle: { LuaPlayer p; p.num = r.I32(); if (!r.Ok()) return false; sol::stack::push(L, p); return true; }
    case kRegionHandle: {
      Region g;
      g.x1 = r.I32(); g.y1 = r.I32(); g.x2 = r.I32(); g.y2 = r.I32();
      if (!r.Ok()) return false;
      sol::stack::push(L, g);
      return true;
    }
    case kBlockHandle: {
      const uint32_t n = r.U32(), size = r.U32();
      if (!r.Ok() || size != sizeof(UnitRecord) || n > 4096) return false;
      std::vector<UnitRecord> recs(n);
      if (!r.Bytes(recs.data(), n * sizeof(UnitRecord))) return false;
      sol::stack::push(L, MakeUnitBlock(std::move(recs)));
      return true;
    }
    default: return false;
  }
}

void SaveBindingsState(Persist::Writer& w) {
  w.U8(gAIToComms ? 1 : 0);
  w.U32(static_cast<uint32_t>(gObjectives.size()));
  for (const auto& [id, text] : gObjectives) { w.I32(id); w.Str(text); }
  w.U32(static_cast<uint32_t>(gOrders.size()));
  for (const DeferredOrder& o : gOrders) {
    w.I32(o.unit.GetID());
    w.U8(static_cast<uint8_t>(o.kind));
    w.I32(o.x);
    w.I32(o.y);
    w.I32(o.target.GetID());
    w.I32(o.attempts);
  }
}

bool RestoreBindingsState(Persist::Reader& r, std::string& err) {
  gAIToComms = r.U8() != 0;
  gObjectives.clear();
  const uint32_t texts = r.U32();
  for (uint32_t i = 0; r.Ok() && i < texts; ++i) {
    const int id = r.I32();
    const std::string text = r.Str();
    if (!r.Ok()) break;
    gObjectives[id] = text;
    // The loaded condition still points where its text was in the game that saved: point it at the text again.
    Tethys::ScBase* stub = Tethys::ScBase::GetInstance(id);
    if (stub == nullptr) {
      LOG_MISSION(WARNING) << "saved game: condition " << id << " (\"" << text << "\") isn't in the loaded game";
      continue;
    }
    *reinterpret_cast<const char**>(reinterpret_cast<char*>(stub) + 0x30) = KeepText(text).c_str();
  }
  gOrders.clear();
  const uint32_t orders = r.U32();
  for (uint32_t i = 0; r.Ok() && i < orders; ++i) {
    DeferredOrder o{ Unit(r.I32()), OrderKind::Move, 0, 0, Unit() };
    o.kind = static_cast<OrderKind>(r.U8());
    o.x = r.I32();
    o.y = r.I32();
    o.target = Unit(r.I32());
    o.attempts = r.I32();
    if (r.Ok()) gOrders.push_back(o);
  }
  if (!r.Ok()) { err = "the bindings' part of the save is broken"; return false; }
  return true;
}

void ResetObjectives() { gObjectives.clear(); }

void PushTriggerFunctions(lua_State* L) {
  lua_createtable(L, 0, static_cast<int>(gTriggerFns.size()));
  for (const auto& [id, fn] : gTriggerFns) {
    fn.push(L);
    lua_rawseti(L, -2, id);
  }
}

void SetTriggerFunctions(lua_State* L, int idx) {
  gTriggerFns.clear();
  idx = lua_absindex(L, idx);
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    if (lua_isinteger(L, -2) && lua_isfunction(L, -1))
      gTriggerFns[static_cast<int>(lua_tointeger(L, -2))] = sol::protected_function(L, -1);
    lua_pop(L, 1);
  }
}

// Supplied to the logger so every line carries the game mark (100 ticks). Reads ~0 before a game is
// running. Fully qualified so it compiles regardless of which `using`s are in scope here.
int CurrentMarkForLog() { return Tethys::TethysAPI::Game::Tick() / 100; }

// ---- unit stats, tech gates and costs (missions that change the rules as they go) ----

// A per-player unit stat's offset in PerPlayerUnitStats (Tethys/Game/MapObjectType.h), by name, or -1. The record's
// last part means different things for vehicles, weapons and buildings; each name is its field's offset, whatever the
// type ("move_speed" and "power" are the same field).
int UnitStatOffset(const std::string& field) {
  static const std::map<std::string, int> offsets = {
    { "hp", 0x00 }, { "repair", 0x04 }, { "armor", 0x08 }, { "common_cost", 0x0C }, { "rare_cost", 0x10 },
    { "build_time", 0x14 }, { "sight_range", 0x18 },
    { "move_speed", 0x24 }, { "power", 0x24 },
    { "turn_rate", 0x28 }, { "concussion", 0x28 }, { "workers", 0x28 },
    { "production_rate", 0x2C }, { "penetration", 0x2C }, { "scientists", 0x2C },
    { "reload_time", 0x30 }, { "storage", 0x30 },
    { "weapon_range", 0x34 }, { "production_capacity", 0x34 },
    { "cargo_capacity", 0x38 }, { "storage_bays", 0x38 },
  };
  const auto it = offsets.find(field);
  return (it != offsets.end()) ? it->second : -1;
}

// The stat `field` of unit `type` (a name, or a raw MapID number as a mission's loops over the types use) for player
// `num` (0-6), or nullptr (logged) for an unknown type, field or player.
int* UnitStatPtr(int num, const sol::object& typeArg, const std::string& field, const char* fn) {
  const MapID t = typeArg.is<int>() ? static_cast<MapID>(typeArg.as<int>())
                : typeArg.is<std::string>() ? MapIdFromName(typeArg.as<std::string>()) : MapID::None;
  const std::string type = typeArg.is<std::string>() ? typeArg.as<std::string>() : std::to_string(static_cast<int>(t));
  const int off = UnitStatOffset(field);
  const int id = static_cast<int>(t);
  Tethys::MapObjectType*const mot = (id > 0 && id < static_cast<int>(MapID::MaxObject)) ? Tethys::MapObjectType::GetInstance(id) : nullptr;
  if ((mot == nullptr) || (off < 0) || (num < 0) || (num > 6)) {
    LOG_MISSION(ERROR) << "OP2Lua: " << fn << "(" << type << ", " << field << ") for player " << (num + 1)
                       << ": unknown unit type, stat or player - ignored";
    return nullptr;
  }
  return reinterpret_cast<int*>(reinterpret_cast<char*>(&mot->playerStats_[num]) + off);
}

void RegisterBindings(sol::state& lua) {
  gAIToComms = false;   // each mission starts with AI events NOT shown in-game
#ifdef OP2LUA_GUARD_TEST
  // Test builds only (cmake -DOP2LUA_GUARD_TEST=ON): a game call that faults, Unit::isDiscovered (0x476120) on a unit
  // id far past the unit table, for checking that the fault comes back as a Lua error (Guard.h).
  lua.set_function("__op2lua_fault", [] {
    Unit u(0x0FFFFFFF);
    using Fn = int(__thiscall*)(Unit*);
    return reinterpret_cast<Fn>(0x476120)(&u); });
#endif
  sol::table game = lua.create_named_table("game");
  game.set_function("message", &Game_Message);
  // game.message_at(text, sound, player, pixel_x, pixel_y): a message for one player (or all, with nil), pointing at a
  // spot of the map in pixels, as the stock missions post them (CPS2's "Microbe growth detected!").
  game.set_function("message_at", [](const std::string& text, sol::object sound, sol::object player, int px, int py) {
    const int to = player.is<sol::lua_nil_t>() ? Tethys::AllPlayers : PlayerNumFrom(player);
    AddMessageWith(text, sound, to, px, py);
    LOG_MISSION(INFO) << "game.message_at: " << text; });
  game.set_function("sound", &Game_Sound);
  game.set_function("play_wav", &Game_PlayWav);
  game.set_function("rand", &Game_Rand);
  game.set_function("tick", &Game_Tick);
  game.set_function("version", &Game_Version);
  game.set_function("op2_version", &Game_Op2Version);
  game.set_function("morale_steady", &Game_MoraleSteady);
  game.set_function("free_morale", &Game_FreeMorale);
  game.set_function("create_unit", &Game_CreateUnit);

  // ---- world queries ----
  game.set_function("num_players", []() { return Game::NumPlayers(); });
  game.set_function("num_humans",  []() { return Game::NumHumans();  });
  // The multiplayer lobby's choices (a Colony game has no lobby: these read the game's defaults).
  game.set_function("initial_units",  []() { return Game::InitialUnits(); });  // starting vehicles, 0-12
  game.set_function("uses_day_night", []() { return Game::UsesDayNight(); });  // day and night on
  game.set_function("uses_morale",    []() { return Game::UsesMorale();   });  // the lobby's Morale Steady is off
  game.set_function("beacons",     &Game_Beacons);     // discover mining beacons (perception)
  game.set_function("find_place",  &Game_FindPlace);   // find a clear spot to build/place a type
  game.set_function("find_build_site", &Game_FindBuildSite); // footprint-valid structure site (DoBuild anchor)
  game.set_function("footprint",   &Game_Footprint);   // a structure type's tile size (core + total)
  game.set_function("cell",        &Game_Cell);        // read a tile's terrain (passable/cliff/lava/...)
  game.set_function("can_place",   &Game_CanPlace);    // tile-flag check: can a structure tile go here?
  game.set_function("map_size",    &Game_MapSize);     // playable map dimensions
  game.set_function("tech",        &Game_Tech);        // a research topic from the loaded tech tree

  // ---- runtime map features (set-pieces created during the mission, not just at placement) ----
  game.set_function("create_wall", [](const std::string& type, sol::object area) {   // "Wall"/"Tube"/"LavaWall"/...
    const MapID t = MapIdFromName(type);
    if (t != MapID::Wall && t != MapID::Tube && t != MapID::LavaWall && t != MapID::MicrobeWall) {
      LOG_MISSION(WARNING) << "create_wall: '" << type << "' isn't a wall type (Wall, Tube, LavaWall, MicrobeWall)";
      return;
    }
    Game::CreateWall(t, RectFrom(area)); });
  game.set_function("create_tube", [](sol::object area) { Game::CreateTube(RectFrom(area)); });
  game.set_function("create_marker", [](sol::object at, sol::optional<std::string> type) {
    using Tethys::TethysAPI::MarkerType;
    MarkerType mt = MarkerType::Circle;
    if (type) { if (*type == "DNA") mt = MarkerType::DNA; else if (*type == "Beaker") mt = MarkerType::Beaker; }
    return Game::CreateMarker(LocFrom(at), mt); });
  game.set_function("create_wreckage", [](sol::object at, int techId, sol::optional<bool> discovered) {
    return Game::CreateWreckage(LocFrom(at), techId, discovered.value_or(false)); });  // techId 8000-12095
  game.set_function("create_beacon", &Game_CreateBeacon);   // a mining beacon / vent / fumarole at runtime

  // ---- disasters (the trailing `now` forces it to trigger immediately instead of after a warning) ----
  game.set_function("create_meteor", [](sol::object at, sol::optional<int> size, sol::optional<bool> now) {
    return Game::CreateMeteor(LocFrom(at), static_cast<Tethys::TethysAPI::MeteorSize>(size.value_or(-1)),
                              now.value_or(false)); });                          // size: 0 large,1 med,2 small,-1 random
  game.set_function("create_earthquake", [](sol::object at, int magnitude, sol::optional<bool> now) {
    return Game::CreateEarthquake(LocFrom(at), magnitude, now.value_or(false)); });
  game.set_function("create_eruption", [](sol::object at, int spreadSpeed, sol::optional<bool> now) {
    return Game::CreateEruption(LocFrom(at), spreadSpeed, now.value_or(false)); });
  game.set_function("create_lightning", [](sol::object from, sol::object to, int duration, sol::optional<bool> now) {
    return Game::CreateLightning(LocFrom(from), LocFrom(to), duration, now.value_or(false)); });
  game.set_function("create_tornado", [](sol::object from, sol::object to, int duration, sol::optional<bool> now) {
    return Game::CreateTornado(LocFrom(from), LocFrom(to), duration, now.value_or(false)); });
  game.set_function("create_blight", [](sol::object at, sol::optional<int> spreadSpeed) {
    Game::CreateBlight(LocFrom(at), spreadSpeed.value_or(-1)); });
  game.set_function("unset_blight", [](sol::object at) { Game::UnsetBlight(LocFrom(at)); });
  // game.disasters_enabled() - whether disasters are enabled for this game (the Colony Games menu
  // "disasters" setting). Let a mission's random-disaster director respect the player's choice.
  game.set_function("disasters_enabled", []() { return Game::CanHaveDisasters(); });

  // ---- environment ----
  game.set_function("set_daylight_everywhere", [](bool on) { Game::SetDaylightEverywhere(on); });
  game.set_function("set_daylight_moves",      [](bool on) { Game::SetDaylightMoves(on); });
  // Where the day/night band starts (a tile column, as the stock missions pass it: e.g. -32). Call in
  // on_init; with set_daylight_moves(false) the band stays there.
  game.set_function("set_initial_light_level", [](int position) { GameMap::SetInitialLightLevel(position); });
  game.set_function("set_lava_speed",          [](int n)  { Game::SetLavaSpeed(n);   });
  game.set_function("set_blight_speed",        [](int n)  { Game::SetBlightSpeed(n); });
  // Mark an area's tiles lava-possible (or not), so an eruption's lava can spread there. Lets you
  // carve a flow channel from a volcano toward a base on maps that have no native lava terrain.
  game.set_function("set_lava_possible", [](sol::object area, sol::optional<bool> on) {
    const MapRect r = RectFrom(area);
    const int     v = on.value_or(true) ? 1 : 0;
    for (int y = r.y1; y <= r.y2; ++y)
      for (int x = r.x1; x <= r.x2; ++x)
        GameMap::SetLavaPossible(Location(x, y), v);
  });
  game.set_function("units_in_range", &Game_UnitsInRange);
  game.set_function("units_in_rect",  &Game_UnitsInRect);
  game.set_function("units_closest",  &Game_UnitsClosest);
  // game.force_robot_command(on) - every player's robot units work without a Robot Command Center (the game's
  // forceEnableRCC flag, game state +0x3C), for a mission that takes the RCC away.
  game.set_function("force_robot_command", [](bool on) { Tethys::GameImpl::GetInstance()->forceEnableRCC_ = on; });
  // game.clear_cheated() - clears the tick of the last game-option change (game state +0x88), which a mission's
  // forced morale sets, so the game doesn't mark the game "CHEATED GAME!".
  game.set_function("clear_cheated", [] { Tethys::GameImpl::GetInstance()->tickOfLastSetGameOpt_ = 0; });
  game.set_function("rand_point", &Game_RandPoint);
  game.set_function("find_emp_missile_target", &Game_FindEmpMissileTarget);
  // game.create_emp_missile(target, from, player): an EMP missile of `player` flying from `from` to `target`
  // (TethysGame::SetEMPMissile, 0x478420, with the stock missions' arguments: from x, y, the player, target x, y;
  // Eden 12's FireEMP1).
  game.set_function("create_emp_missile", [](sol::object target, sol::object from, sol::object player) {
    const Location t = LocFrom(target), f = LocFrom(from);
    const int num = PlayerArg(player, "create_emp_missile");
    if (num < 0) return;
    using Fn = void(__fastcall*)(int, int, int, int, int);
    reinterpret_cast<Fn>(0x478420)(f.x, f.y, num, t.x, t.y); });
  // game.wreckage() - the wreckage on the map (type 0x54, the gaia player's, 6), as a stock mission walks it
  // (Eden 6).
  // game.set_music(songs [, repeat_from]) - the music playlist: the game's song numbers in order, and the position
  // (1-based) the list starts again from once it has played through (default 1), as a mission's
  // TethysGame::SetMusicPlayList sets it.
  game.set_function("set_music", [](sol::table songs, sol::optional<int> repeatFrom) {
    std::vector<Tethys::SongID> ids;
    for (auto& kv : songs) if (kv.second.is<int>()) ids.push_back(static_cast<Tethys::SongID>(kv.second.as<int>()));
    if (ids.empty()) return;
    const int from = std::clamp(repeatFrom.value_or(1), 1, static_cast<int>(ids.size())) - 1;
    Game::SetMusicPlayList(static_cast<int>(ids.size()), from, ids.data()); });
  // game.player(n) - the Player of slot n (1-7), whether or not the lobby has it: `players` holds the lobby's, and a
  // mission's own computer player beyond them (the game's player 6, say) is game.player(7). Nil
  // outside 1-7.
  game.set_function("player", [](int n, sol::this_state ts) -> sol::object {
    sol::state_view L(ts);
    if (n < 1 || n > 7) return sol::make_object(L, sol::lua_nil);
    return sol::make_object(L, LuaPlayer{ n - 1 }); });
  // game.set_required_tech(type, tech) - the topic a unit type (a name or raw MapID) needs before it can be built (MapObjectType's
  // requiredTechNum_, which holds the topic's place in the tech tree, not its id), so a mission can gate units behind
  // its own topics. Every player is affected.
  game.set_function("set_required_tech", [](sol::object type, int techId) {
    const MapID t = type.is<int>() ? static_cast<MapID>(type.as<int>())
                  : type.is<std::string>() ? MapIdFromName(type.as<std::string>()) : MapID::None;
    const int id = static_cast<int>(t);
    Tethys::MapObjectType*const mot = (id > 0 && id < static_cast<int>(MapID::MaxObject)) ? Tethys::MapObjectType::GetInstance(id) : nullptr;
    if (mot == nullptr) { LOG_MISSION(ERROR) << "OP2Lua: set_required_tech: unknown unit type " << id; return; }
    if (!IsValidTechId(techId)) { ReportBadTech("set_required_tech", techId); return; }
    mot->requiredTechNum_ = Tethys::Research::GetInstance()->GetTechNum(techId); });
  // game.set_tech_cost(tech, cost) - the research a topic takes, for Eden and Plymouth alike; -1 makes it impossible to
  // research.
  game.set_function("set_tech_cost", [](int techId, int cost) {
    if (!IsValidTechId(techId)) { ReportBadTech("set_tech_cost", techId); return; }
    Tethys::TechInfo*const info = Tethys::Research::GetInstance()->GetTechInfo(techId);
    if (info != nullptr) { info->edenCost = cost; info->plymouthCost = cost; } });
  game.set_function("wreckage", [](sol::this_state ts) {
    sol::state_view L(ts);
    sol::table out = L.create_table();
    int i = 1;
    for (Unit u : PlayerVehicleEnum(6, MapID(0x54))) out[i++] = u;
    return out; });
  // A tile's graphic: its index in the map's tile mappings (as a map editor shows it). The stock
  // lava maps swap in volcano tiles before an eruption: game.set_tile({ 52, 4 }, 1147).
  game.set_function("tile",     [](sol::object at) { return GameMap::GetTile(LocFrom(at)); });
  game.set_function("set_tile", [](sol::object at, int index) {
    const Location l = LocFrom(at);
    if (OnGridArg(l, "set_tile")) GameMap::SetTile(l, index); });
  // A setup's own map edits (Tutorial 5's): a tile set as the map loads, and a tile's cell type (the game's
  // CellType number: what moves over it and what can be built on it).
  game.set_function("initial_set_tile", [](sol::object at, int index) {
    const Location l = LocFrom(at);
    if (OnGridArg(l, "initial_set_tile")) GameMap::InitialSetTile(l, index); });
  game.set_function("set_cell_type", [](sol::object at, int type) {
    const Location l = LocFrom(at);
    if (OnGridArg(l, "set_cell_type")) GameMap::SetCellType(l, static_cast<Tethys::CellType>(type)); });
  game.set_function("map_sound", [](sol::object sound, sol::object at) {
    Game::AddMapSound(ResolveSound(sound, static_cast<Tethys::SoundID>(0)), LocFrom(at)); });

  game["ticks_per_second"] = kTicksPerSecond;
  game["ticks_per_minute"] = kTicksPerMinute;

  sol::table mission = lua.create_named_table("mission");
  mission.set_function("win", &Mission_Win);
  mission.set_function("lose", &Mission_Lose);
  mission.set_function("last_one_standing", &Mission_LastOneStanding);
  mission.set_function("victory", &Mission_Victory);
  mission.set_function("failure", &Mission_Failure);

  sol::table trigger = lua.create_named_table("trigger");
  trigger.set_function("count",           &Trigger_Count);
  trigger.set_function("resource",        &Trigger_Resource);
  trigger.set_function("set",             &Trigger_Set);
  trigger.set_function("midas",           &Trigger_Midas);
  trigger.set_function("one_player_left", &Trigger_OnePlayerLeft);
  trigger.set_function("operational",     &Trigger_Operational);
  trigger.set_function("vehicle_count",   &Trigger_VehicleCount);
  trigger.set_function("building_count",  &Trigger_BuildingCount);
  trigger.set_function("escape",          &Trigger_Escape);
  trigger.set_function("kit",             &Trigger_Kit);
  trigger.set_function("evac",            &Trigger_Evac);
  trigger.set_function("special_target",  &Trigger_SpecialTarget);
  trigger.set_function("point",           &Trigger_Point);
  trigger.set_function("rect",            &Trigger_Rect);
  trigger.set_function("time",            &Trigger_Time);
  trigger.set_function("research",        &Trigger_Research);
  lua.new_usertype<Trigger>(
      "Trigger", sol::no_constructor,
      "id",        sol::property([](const Trigger& t) { return t.GetID(); }),
      "valid",     sol::property([](const Trigger& t) { return t.IsValid(); }),
      "enabled",   sol::property([](const Trigger& t) { return t.IsValid() && t.IsEnabled() != 0; }),
      "enable",    [](Trigger& t) { if (t.IsValid()) t.Enable(); },
      "disable",   [](Trigger& t) { if (t.IsValid()) t.Disable(); },
      // Ends the trigger (and forgets its function): a disaster's function ends the time trigger that fired.
      "destroy",   [](Trigger& t) { if (t.IsValid()) { gTriggerFns.erase(t.GetID()); t.Destroy(); } },
      "has_fired", [](Trigger& t, sol::object player) { return t.IsValid() && t.HasFired(PlayerNumFrom(player)) != 0; },
      // A special-target trigger's unit that reached the target (GetSpecialTargetData, 0x479860).
      "special_target_unit", [](Trigger& t) { return t.IsValid() ? Tethys::TethysAPI::GetSpecialTargetData(t) : Unit(); },
      sol::meta_function::equal_to, [](const Trigger& a, const Trigger& b) { return a.GetID() == b.GetID(); });

  // A mission's print() is redirected here by the runtime preamble. This is author/debug output,
  // so it goes to the mission log ONLY (LOG_MISSION) - never the shared Main OP2Lua.log.
  lua.set_function("__op2lua_log", [](const std::string& s) { LOG_MISSION(INFO) << "[lua] " << s; });

  // log.ai(...) routes here: a scripted AI's debug stream -> its own AI log file (LOG_AI), and (when
  // the dev has enabled it) also the in-game comms panel so the AI can be watched without leaving OP2.
  lua.set_function("__op2lua_log_ai", [](const std::string& s) {
    LOG_AI(INFO) << s;
    if (gAIToComms) Game::AddMessage(s, kDefaultMsgSound, Tethys::AllPlayers);
  });
  // log.show_ai_in_game(on): toggle the comms-panel echo above.
  lua.set_function("__op2lua_ai_to_comms", [](bool on) { gAIToComms = on; });

  // log.ai_status(player): (over)write a current-state JSON snapshot of a player (resources + unit
  // counts by type) to OP2Lua-<base>-AI-status.json, for an external monitor to poll. Overwrites each
  // call so the file is always "now". Call it from every(marks(1), ...).
  lua.set_function("__op2lua_ai_status", [](const LuaPlayer& lp) {
    using namespace Tethys::TethysAPI;
    const int num = lp.num;
    auto p = _Player(num);
    std::map<std::string, int> byType;
    int buildings = 0, vehicles = 0;
    for (Unit u : PlayerBuildingEnum(num, MapID::Any)) { ++buildings; byType[NameFromMapId(u.GetType())]++; }
    for (Unit u : PlayerVehicleEnum(num,  MapID::Any)) { ++vehicles;  byType[NameFromMapId(u.GetType())]++; }

    std::ostringstream js;
    js << "{\n";
    js << "  \"mark\": "       << CurrentMarkForLog() << ",\n";
    js << "  \"player\": "     << (num + 1) << ",\n";
    js << "  \"resources\": {\n";
    js << "    \"common_ore\": " << p.GetCommonOre()  << ",\n";
    js << "    \"rare_ore\": "   << p.GetRareOre()    << ",\n";
    js << "    \"food\": "       << p.GetFoodStored() << ",\n";
    js << "    \"population\": " << p.GetPopulation() << ",\n";
    js << "    \"kids\": "       << p.GetKids()       << ",\n";
    js << "    \"workers\": "    << p.GetWorkers()    << ",\n";
    js << "    \"scientists\": " << p.GetScientists() << "\n";
    js << "  },\n";
    js << "  \"units\": {\n";
    js << "    \"total\": "     << (buildings + vehicles) << ",\n";
    js << "    \"buildings\": " << buildings << ",\n";
    js << "    \"vehicles\": "  << vehicles  << ",\n";
    js << "    \"by_type\": {";
    bool first = true;
    for (const auto& [name, cnt] : byType) {
      if (name.empty()) continue;
      js << (first ? "\n" : ",\n") << "      \"" << name << "\": " << cnt;
      first = false;
    }
    js << (first ? "}" : "\n    }") << "\n";
    js << "  }\n";
    js << "}\n";
    WriteAIStatusSnapshot(js.str());
  });

  lua.new_usertype<Unit>(
      "Unit", sol::no_constructor,
      "id",          sol::property([](const Unit& u) { return u.GetID(); }),
      "type",        sol::property([](const Unit& u) { return NameFromMapId(u.GetType()); }),
      "type_id",     sol::property([](const Unit& u) { return static_cast<int>(u.GetType()); }),
      "alive",       sol::property([](const Unit& u) { return u.IsLive(); }),
      "owner",       sol::property([](const Unit& u) { return u.GetOwner(); }),
      "valid",       sol::property([](const Unit& u) { return u.IsValid(); }),
      "move",        [](Unit& u, sol::object loc) { const Location p = LocFrom(loc);
                                                    Order({ u, OrderKind::Move, p.x, p.y, Unit() }); },
      // attack_move(target): if given a unit, hunt and attack it; if given a location, advance there
      // aggressively (DoAttack-to-ground, firing en route). NOTE: a plain DoMove (CmdMove) on a unit
      // freshly created this/last tick crashes OP2's pathfinder - DoAttack does not - so the location
      // case stays AttackGround (as Move it breaks missions that spawn defenders).
      "attack_move", [](Unit& u, sol::object tgt) {
                       if (tgt.is<Unit>()) { Unit t = tgt.as<Unit>(); const Location p = t.GetLocation();
                                             Order({ u, OrderKind::AttackUnit, p.x, p.y, t }); }
                       else                { const Location p = LocFrom(tgt);
                                             Order({ u, OrderKind::AttackGround, p.x, p.y, Unit() }); } },
      // attack(target): attack a specific unit, or (with a location) deliberately attack the ground.
      "attack",      [](Unit& u, sol::object tgt) {
                       if (tgt.is<Unit>()) { Unit t = tgt.as<Unit>(); const Location p = t.GetLocation();
                                             Order({ u, OrderKind::AttackUnit, p.x, p.y, t }); }
                       else                { const Location p = LocFrom(tgt);
                                             Order({ u, OrderKind::AttackGround, p.x, p.y, Unit() }); } },
      // Cargo Truck contents.  unit:set_cargo("RareMetal", 1000)  /  unit.cargo  /  unit.cargo_amount
      "set_cargo",    [](Unit& u, const std::string& cargo, sol::optional<int> amount) {
                        const int ct = CargoTypeFromName(cargo);
                        if (ct >= 0) u.SetCargo(static_cast<Tethys::CargoType>(ct), amount.value_or(1000)); },
      "cargo",        sol::property([](Unit& u) { return NameFromCargoType(static_cast<int>(u.GetTruckCargo().cargoType)); }),
      "cargo_amount", sol::property([](Unit& u) { return u.GetTruckCargo().amount; }),

      // ---- base-building orders (RoboSurveyor / RoboMiner / ConVec / Cargo Truck / factories) ----
      // For a computer player to bring up a base FOR REAL (survey -> mine -> truck routes -> produce
      // + build structures) instead of cheat-spawning finished units. SEH-guarded where they touch
      // OP2's Cmd* path. See docs/AI-DESIGN.md.

      // surveyor:survey(beacon) - mark a mining beacon surveyed for this unit's owner, so a RoboMiner
      // can deploy on it. (OP2 has no scriptable "drive over and survey" order; this is its effect.)
      "survey",       [](Unit& u, Unit beacon) { if (u.IsLive() && beacon.IsValid()) beacon.SetSurveyed(u.GetOwner()); },
      // miner:deploy(beacon|loc) - RoboMiner/GeoCon drives to the spot and deploys into a building
      // (a mine, on a surveyed beacon).
      "deploy",       [](Unit& u, sol::object tgt) { if (!u.IsLive()) return;
                        const Location c = tgt.is<Unit>() ? tgt.as<Unit>().GetLocation() : LocFrom(tgt);
                        if (!GuardedDeploy(u, c)) LOG_MISSION(WARNING) << "deploy: command faulted"; },
      // convec:build(loc) - ConVec builds the structure kit it is carrying, anchored at loc.
      "build",        [](Unit& u, sol::object loc) { if (u.IsLive() && !GuardedBuild(u, LocFrom(loc)))
                        LOG_MISSION(WARNING) << "build: command faulted"; },
      // truck:mining_route(mine, smelter) - run a Cargo Truck on a mine<->smelter ore route.
      "mining_route", [](Unit& u, Unit mine, Unit smelter) { if (u.IsLive() && mine.IsValid() && smelter.IsValid())
                        GuardedMiningRoute(u, mine, smelter); },
      // unit:dock(structure) - dock a vehicle at a structure (e.g. a ConVec at a Structure Factory).
      "dock",         [](Unit& u, Unit at) { if (u.IsLive() && at.IsValid()) GuardedDock(u, at); },
      // structure:unit_on_dock() - the vehicle currently on this structure's dock, or nil. Use to know
      // a ConVec has finished docking BEFORE transferring a kit (transferring early crashes OP2).
      "unit_on_dock", [](Unit& u, sol::this_state ts) -> sol::object {
                        sol::state_view L(ts);
                        const Unit d = u.GetUnitOnDock();
                        return d.IsLive() ? sol::make_object(L, d) : sol::make_object(L, sol::lua_nil); },
      // structure:dock_location() - the tile a vehicle docks on (e.g. a ConVec at a Structure Factory),
      // as author coords {x,y}, or nil. Move a ConVec here to collect a kit on the dock. (Read-only and
      // safe; the actual DoDock command is not - see docs.)
      "dock_location", [](Unit& u, sol::this_state ts) -> sol::object {
                        sol::state_view L(ts);
                        const Location d = u.IsBuilding() ? u.GetDockLocation() : Location();
                        if ((d.x == 0) && (d.y == 0)) return sol::make_object(L, sol::lua_nil);
                        sol::table out = L.create_table();
                        out["x"] = d.x - GameMap::GetPaddingWidth() + 1;
                        out["y"] = d.y + 1;
                        return out; },
      // factory:produce(type [, weapon]) - queue a unit/kit at a Vehicle/Structure Factory.
      "produce",      [](Unit& u, const std::string& type, sol::optional<std::string> weapon) {
                        if (!u.IsLive()) return;
                        const MapID t = MapIdFromName(type);
                        if (t != MapID::None) GuardedProduce(u, t, weapon ? WeaponFor(t, MapIdFromName(*weapon)) : MapID::None);
                        else LOG_MISSION(WARNING) << "produce: unknown type '" << type << "'"; },
      // spaceport:develop(type) - build a rocket or its cargo at a Spaceport (e.g. "EMPMissile", "SolarSatellite").
      "develop",      [](Unit& u, const std::string& type) {
                        if (!u.IsLive()) return;
                        const MapID t = MapIdFromName(type);
                        if (t != MapID::None) GuardedDevelop(u, t);
                        else LOG_MISSION(WARNING) << "develop: unknown type '" << type << "'"; },
      // factory:set_factory_cargo(bay, type [, cargoOrWeapon]) - preload a kit into a 1-based bay (1..6).
      "set_factory_cargo", [](Unit& u, int bay, const std::string& type, sol::optional<std::string> extra) {
                        const MapID t = MapIdFromName(type);   // a Guard Post kit's weapon is its turret version
                        if (u.IsLive()) u.SetFactoryCargo(bay - 1, t, extra ? WeaponFor(t, MapIdFromName(*extra)) : MapID::None); },
      // factory:transfer_cargo(bay) - move the 1-based bay's cargo to the docked vehicle. ONLY call when
      // a vehicle is actually on the dock (see unit_on_dock) - calling it otherwise crashes OP2.
      "transfer_cargo", [](Unit& u, int bay) {
                        if (!u.IsLive()) return;
                        if (!u.IsFactory() || bay < 1 || bay > 6) {
                          LOG_MISSION(WARNING) << "transfer_cargo: needs a factory and a bay 1-6";
                          return;
                        }
                        if (u.GetFactoryCargo(bay - 1).unitType == MapID::None || !u.GetUnitOnDock().IsLive()) {
                          LOG_MISSION(WARNING) << "transfer_cargo: bay " << bay
                                               << " is empty or no vehicle is on the dock";
                          return;
                        }
                        if (!GuardedTransferCargo(u, bay - 1))
                          LOG_MISSION(WARNING) << "transfer_cargo: the game faulted moving bay " << bay << "; skipped"; },
      // lab:research(techId [, scientists]) - begin researching a technology. Validates the id first
      // (a bad id crashes DoResearch); on a bad id it logs an error + in-game message and no-ops.
      "research",     [](Unit& u, int techId, sol::optional<int> sci) {
                        if (!u.IsLive()) return;
                        if (!IsValidTechId(techId)) { ReportBadTech("research", techId); return; }
                        u.DoResearch(techId, sci.value_or(1)); },
      // university:train_scientists(n) - train n new scientists.
      "train_scientists", [](Unit& u, int n) { if (u.IsLive()) u.DoTrainScientists(n); },
      "idle",         [](Unit& u) { if (u.IsLive()) u.DoIdle();   },
      "unidle",       [](Unit& u) { if (u.IsLive()) u.DoUnidle(); },

      // ---- state queries (for sequencing a build order) ----
      "command",            sol::property([](const Unit& u) { return static_cast<int>(u.GetCommand()); }),
      "is_building",        sol::property([](const Unit& u) { return u.IsBuilding(); }),
      "is_vehicle",         sol::property([](const Unit& u) { return u.IsVehicle(); }),
      "enabled",            sol::property([](const Unit& u) { return u.IsEnabled(); }),
      "idled",              sol::property([](const Unit& u) { return u.IsIdled(); }),
      "under_construction", sol::property([](const Unit& u) { return u.IsUnderConstruction(); }),
      "is_factory",         sol::property([](const Unit& u) { return u.IsFactory(); }),
      "is_offensive",       sol::property([](const Unit& u) { return u.IsOffensive(); }),
      // A vehicle's headlights are on (MoFlagVecHeadlights, bit 0 of the flags at +0x44): what op2extra's
      // IUnit::GetLights reads, so a mission can wait for the player to switch them on.
      "lights",             sol::property([](Unit& u) { return u.GetLights(); }),
      "is_infected",        sol::property([](const Unit& u) { return u.IsInfected(); }),
      "disabled",           sol::property([](const Unit& u) { return u.IsDisabled(); }),
      // The game's exported Unit::IsBusy, not Tethys's (see GameIsBusy): a Spaceport stays busy until its rocket
      // is built.
      "busy",               sol::property([](const Unit& u) { return GameIsBusy(u); }),
      // [Garage, StructureFactory, Spaceport] a bay holds something.
      "occupied_bay",       sol::property([](const Unit& u) { return u.IsLive() && GameHasOccupiedBay(u); }),
      // [Wreckage] the player has found it (Unit::isDiscovered).
      "is_discovered",      sol::property([](const Unit& u) { return u.IsValid() && GameIsDiscovered(u); }),
      // A Spaceport's rocket on its pad ("EMPMissile", ...), or "None".
      "object_on_pad",      sol::property([](const Unit& u) { return NameFromMapId(u.IsLive() ? u.GetRocketOnPad() : MapID::None); }),
      "is_emped",           sol::property([](const Unit& u) { return u.IsEMPed(); }),
      // The way a vehicle faces, 0-255 (0 East, clockwise: 64 South, 128 West, 192 North); a facing is rotation / 32.
      "rotation",           sol::property([](const Unit& u) { return u.IsValid() ? int(u.GetMapObject()->rotation_) : 0; }),
      // A vehicle is stickyfoamed (MoFlagVecStickyfoamed, bit 18 of the flags at +0x44).
      "is_stickyfoamed",    sol::property([](const Unit& u) { return u.IsLive() && u.IsVehicle() && (u.GetMapObject()->flags_ & Tethys::MoFlagVecStickyfoamed) != 0; }),
      // The unit fires twice as often (MoFlagDoubleFireRate, bit 5 of the flags at +0x44: the Tiger's), read and set.
      "double_fire_rate",   sol::property([](const Unit& u) { return u.IsValid() && (u.GetMapObject()->flags_ & Tethys::MoFlagDoubleFireRate) != 0; },
                                          [](Unit& u, bool on) { if (u.IsLive()) u.SetFlag(Tethys::MoFlagDoubleFireRate, on); }),
      // A building's DIRT damage prevention, in hundredths of a percent (2500 = 25%), through its colony tube
      // connection (TubeConnection::CalculateDIRTDamagePrevention 0x430050); 0 for a building with no connection or a
      // vehicle.
      "dirt_prevention",    sol::property([](const Unit& u) -> int {
        if (!u.IsLive() || !u.IsBuilding()) return 0;
        Tethys::TubeConnection*const tc = u.GetMapObject()->pTubeConnection_;
        return (tc != nullptr) ? tc->CalculateDIRTDamagePrevention() : 0; }),
      // The unit counts as made by an Eden player (MoFlagEden, bit 12), which picks Eden's art for it, read and set.
      "eden",               sol::property([](const Unit& u) { return u.IsValid() && (u.GetMapObject()->flags_ & Tethys::MoFlagEden) != 0; },
                                          [](Unit& u, bool on) { if (u.IsLive()) u.SetFlag(Tethys::MoFlagEden, on); }),

      // ---- position & perception ----
      // x / y are the unit's tile in author / status-bar coords (round-trip safe: feed {x,y} back to
      // move/attack/build). Use for "what's nearest", expansion, build offsets.
      "x",   sol::property([](const Unit& u) { return u.GetLocation().x - GameMap::GetPaddingWidth() + 1; }),
      "y",   sol::property([](const Unit& u) { return u.GetLocation().y + 1; }),
      // ore: a mining beacon's ore type ("Common"/"Rare"); "" for non-beacons.
      "ore", sol::property([](Unit& u) -> std::string {
               if (u.GetType() != MapID::MiningBeacon) return "";
               auto*const mo = u.GetMapObject<Tethys::MapObj::MiningBeacon>();
               return mo ? (static_cast<int>(mo->GetOreType()) == 1 ? "Rare" : "Common") : ""; }),
      // factory_cargo(bay) -> the kit type name in a 1-based factory bay ("None" if empty), and its weapon or cargo
      // (a Guard Post kit's turret, read back under the name set_factory_cargo takes), nil when it has none.
      "factory_cargo", [](const Unit& u, int bay, sol::this_state ts) {
        sol::state_view L(ts);
        const Tethys::CargoKit k = u.GetFactoryCargo(bay - 1);
        const std::string extra = (k.cargoOrWeaponType == MapID::None) ? std::string() : NameFromMapId(k.cargoOrWeaponType);
        return std::make_tuple(NameFromMapId(k.unitType), extra.empty() ? sol::make_object(L, sol::lua_nil)
                                                                        : sol::make_object(L, extra)); },
      "surveyed_by",   &Unit_SurveyedBy,

      // ---- what was ordered, read back (nil when the unit isn't doing it) ----
      "research_topic", sol::property(&Unit_ResearchTopic),
      "producing",      sol::property(&Unit_Producing),
      "progress",       sol::property(&Unit_Progress),
      "target",         sol::property(&Unit_Target),

      // ---- health ----
      // hp / max_hp (hit points), health (0..1 fraction). damage(n) inflicts n; heal(n) repairs n.
      "max_hp",  sol::property([](const Unit& u) { return u.GetMaxHitpoints(); }),
      "hp",      sol::property([](const Unit& u) { return u.GetMaxHitpoints() - u.GetDamage(); }),
      "health",  sol::property([](const Unit& u) { const int m = u.GetMaxHitpoints();
                                                   return (m > 0) ? double(m - u.GetDamage()) / m : 0.0; }),
      "damage",  [](Unit& u, int n) { if (u.IsLive()) u.AddDamage(n); },                       // past max_hp kills it
      "heal",    [](Unit& u, int n) { if (u.IsLive()) u.SetDamage(std::max(0, u.GetDamage() - n)); },

      // ---- lifecycle / ownership ----
      "stop",          [](Unit& u) { if (u.IsLive()) u.DoStop(); },
      "self_destruct", [](Unit& u) { if (u.IsLive()) u.DoSelfDestruct(); },
      "kill",          [](Unit& u) { if (u.IsLive()) u.DoDeath(); },                            // die with explosion
      "remove",        [](Unit& u) { if (u.IsLive()) u.DoPoof(); },                             // vanish, no animation
      // do_transfer(player): the game's own Unit::DoTransfer (0x476AF0), as a stock mission calls it (Eden 4's tanks
      // and Scouts going over): the unit changes hands at once. transfer(player) goes through Tethys's DoTransfer,
      // SEH-guarded.
      "do_transfer",   [](Unit& u, sol::object player) { const int p = PlayerArg(player, "do_transfer");
                         if (u.IsLive() && p >= 0) { using Fn = void(__thiscall*)(Unit*, int);
                                                     reinterpret_cast<Fn>(0x476AF0)(&u, p); } },
      "transfer",      [](Unit& u, sol::object player) { const int p = PlayerArg(player, "transfer");
                         if (u.IsLive() && p >= 0) GuardedTransfer(u, p); },

      // ---- combat positioning ----
      "guard",        [](Unit& u, Unit what) { if (u.IsLive() && what.IsValid()) GuardedGuard(u, what); },
      "stand_ground", [](Unit& u, sol::object loc) { if (u.IsLive()) GuardedStandGround(u, LocFrom(loc)); },

      // ---- weapon ----
      "weapon",     sol::property([](const Unit& u) { return NameFromMapId(u.GetWeapon()); }),
      "set_weapon", [](Unit& u, const std::string& w) { if (u.IsLive()) u.SetWeapon(WeaponFor(u.GetType(), MapIdFromName(w))); },

      // ---- engineering (ConVec / Spider / Earthworker / Dozer) ----
      "repair",      [](Unit& u, Unit what) { if (u.IsLive() && what.IsValid()) GuardedRepair(u, what); },
      "dismantle",   [](Unit& u, Unit what) { if (u.IsLive() && what.IsValid()) GuardedDismantle(u, what); },
      "reprogram",   [](Unit& u, Unit what) { if (u.IsLive() && what.IsValid()) GuardedReprogram(u, what); },
      "build_wall",  [](Unit& u, const std::string& type, sol::object area) {
                       if (u.IsLive()) GuardedBuildWall(u, MapIdFromName(type), RectFrom(area)); },
      "remove_wall", [](Unit& u, sol::object area) { if (u.IsLive()) GuardedRemoveWall(u, RectFrom(area)); },
      "doze",        [](Unit& u, sol::object area) { if (u.IsLive()) GuardedDoze(u, RectFrom(area)); },
      "salvage",     [](Unit& u, sol::object area, Unit gorf) {
                       if (u.IsLive() && gorf.IsValid()) GuardedSalvage(u, RectFrom(area), gorf); },

      // ---- cargo truck load/unload/dump ----
      "load_cargo",   [](Unit& u) { if (u.IsLive()) u.DoLoadCargo(); },
      "unload_cargo", [](Unit& u) { if (u.IsLive()) u.DoUnloadCargo(); },
      "dump_cargo",   [](Unit& u) { if (u.IsLive()) u.DoDumpCargo(); },

      // ---- spaceport / garage / lights ----
      "launch",         [](Unit& u, sol::object target) { if (u.IsLive())
                          GuardedLaunch(u, target.valid() ? LocFrom(target) : Location()); },
      // launch_at_pixel(px, py): launch at a map pixel (see LaunchAtPixel); launch(target) aims at a tile's centre.
      "launch_at_pixel", [](Unit& u, int px, int py) { if (u.IsLive()) GuardedLaunchAtPixel(u, px, py); },
      // infect(): the Blight takes the unit (the game's Unit::DoInfect).
      "infect",         [](Unit& u) { if (u.IsLive()) GameDoInfect(u); },
      // No longer a special target (Unit::ClearSpecialTarget, 0x475F90, the game's own).
      "clear_special_target", [](Unit& u) { if (u.IsValid()) { using Fn = void(__thiscall*)(Unit*);
                                                                  reinterpret_cast<Fn>(0x475F90)(&u); } },
      // set_damage(n): the unit's damage becomes n (hp = max_hp - n), through the game's own Unit::SetDamage;
      // damage(n) adds to it. set_opp_fired_upon(on): whether the unit fires back when fired upon.
      "set_damage",     [](Unit& u, int n) { if (u.IsLive()) GameSetDamage(u, n); },
      "set_opp_fired_upon", [](Unit& u, bool on) { if (u.IsLive()) GameSetOppFiredUpon(u, on ? 1 : 0); },
      "dock_at_garage", [](Unit& u, Unit garage) { if (u.IsLive() && garage.IsValid()) GuardedDockGarage(u, garage); },
      // put_in_garage(bay, at): parked at once in bay 1-6 of the Garage whose dock tile is `at`, as a mission's setup
      // parks a vehicle (Unit::PutInGarage, 0x476160, the game's own; it takes a 0-based bay). Any other bay: nothing.
      "put_in_garage",  [](Unit& u, int bay, sol::object at) { if (u.IsLive() && bay >= 1 && bay <= 6) {
                          const Location d = LocFrom(at);
                          using Fn = void(__thiscall*)(Unit*, int, int, int);
                          reinterpret_cast<Fn>(0x476160)(&u, bay - 1, d.x, d.y); } },
      "set_lights",     [](Unit& u, bool on) { if (u.IsLive()) u.DoSetLights(on); },
      // Whether the unit counts as a combat unit (MoFlagOffensive, bit 4 of the flags at +0x44), as op2extra's
      // IUnit::SetOffensive sets it: false keeps an escort from opening fire, true lets it fight again.
      "set_offensive",  [](Unit& u, bool on) { if (u.IsLive()) u.SetFlag(Tethys::MoFlagOffensive, on); },
      // EMPs the unit for `ticks` (the game's own MapObject::SetEMPed, 0x401880); -1 or more than 65535 gives the
      // longest it can (the timer is 16 bits), as op2extra's SetEMPticks(-1) does. 0 ends it.
      "set_emp",        [](Unit& u, int ticks) {
                          if (!u.IsLive()) return;
                          u.SetEMPed((ticks < 0 || ticks > 0xFFFF) ? 0xFFFF : ticks); },
      // [ConVec] structure kit it carries. The kit, a tank's weapon, and a truck's cargo all share one
      // union field in OP2 (a unit is only ever one of those), so set_kit / set_weapon / set_cargo are
      // really the same slot under different names - matching the unit type you call them on.
      "kit",            sol::property([](const Unit& u) { return NameFromMapId(u.GetConVecCargo().unitType); }),
      // [ConVec] the kit's weapon (a Guard Post kit's turret, under the name set_kit takes as its topping), nil
      // without one. unit.weapon reads the kit's own slot on a ConVec, not this.
      "kit_weapon",     sol::property([](const Unit& u, sol::this_state ts) {
                          // nil unless a ConVec carries a kit with a named weapon: on other units, and on a
                          // ConVec without a kit, the slot holds something else.
                          sol::state_view L(ts);
                          if (u.GetType() != MapID::ConVec) return sol::make_object(L, sol::lua_nil);
                          const auto cargo = u.GetConVecCargo();
                          const std::string name = (cargo.unitType == MapID::None || cargo.cargoOrWeaponType == MapID::None)
                                                       ? std::string() : std::string(NameFromMapId(cargo.cargoOrWeaponType));
                          return name.empty() ? sol::make_object(L, sol::lua_nil) : sol::make_object(L, name); }),
      "set_kit",        [](Unit& u, const std::string& type, sol::optional<std::string> topping) {
                          const MapID kit = MapIdFromName(type);
                          if (u.IsLive()) u.SetCargo(kit, topping ? WeaponFor(kit, MapIdFromName(*topping)) : MapID::None); },

      sol::meta_function::equal_to, [](const Unit& a, const Unit& b) { return a == b; });

  lua.new_usertype<ChatArgs>(
      "ChatArgs", sol::no_constructor,
      "text",   sol::property(&ChatArgs::GetText, &ChatArgs::SetText),
      "player", sol::property(&ChatArgs::GetPlayer));

  lua.new_usertype<LuaPlayer>(
      "Player", sol::no_constructor,
      "is_eden",     sol::property([](const LuaPlayer& p) { return _Player(p.num).IsEden(); }),
      "is_plymouth", sol::property([](const LuaPlayer& p) { return _Player(p.num).IsPlymouth(); }),
      "is_human",    sol::property([](const LuaPlayer& p) { return _Player(p.num).IsHuman(); }),
      "is_ai",       sol::property([](const LuaPlayer& p) { return _Player(p.num).IsAI(); }),
      "owns_any",    [](const LuaPlayer& p, const std::string& t) { return Player_OwnsAny(p.num, t); },
      "unit_count",  [](const LuaPlayer& p, const std::string& t) { return Player_UnitCount(p.num, t); },
      "units",       [](const LuaPlayer& p, sol::object t, sol::this_state ts) { return Player_Units(p.num, t, ts); },
      // buildings([type]): the player's buildings (all, or of one type; an unknown type gives an empty list), in the
      // game's PlayerBuildingEnum order (CES1's SpawnAttack counts them by area).
      "buildings",   [](const LuaPlayer& p, sol::object typeArg, sol::this_state ts) {
        sol::state_view L(ts);
        sol::table out = L.create_table();
        const MapID t = typeArg.is<std::string>() ? MapIdFromName(typeArg.as<std::string>()) : MapID::Any;
        int i = 1;
        if (t != MapID::None)
          for (Unit u : PlayerBuildingEnum(p.num, t)) out[i++] = u;
        return out; },

      // ---- resources / population (read-write properties; the runtime equivalents of placement) ----
      "ore",        sol::property([](const LuaPlayer& p) { return _Player(p.num).GetCommonOre(); },
                                  [](LuaPlayer& p, int v) { _Player(p.num).SetCommonOre(v); }),
      "rare_ore",   sol::property([](const LuaPlayer& p) { return _Player(p.num).GetRareOre(); },
                                  [](LuaPlayer& p, int v) { _Player(p.num).SetRareOre(v); }),
      "food",       sol::property([](const LuaPlayer& p) { return _Player(p.num).GetFoodStored(); },
                                  [](LuaPlayer& p, int v) { _Player(p.num).SetFoodStored(v); }),
      "kids",       sol::property([](const LuaPlayer& p) { return _Player(p.num).GetKids(); },
                                  [](LuaPlayer& p, int v) { _Player(p.num).SetKids(v); }),
      "workers",    sol::property([](const LuaPlayer& p) { return _Player(p.num).GetWorkers(); },
                                  [](LuaPlayer& p, int v) { _Player(p.num).SetWorkers(v); }),
      "scientists", sol::property([](const LuaPlayer& p) { return _Player(p.num).GetScientists(); },
                                  [](LuaPlayer& p, int v) { _Player(p.num).SetScientists(v); }),
      "population", sol::property([](const LuaPlayer& p) { return _Player(p.num).GetPopulation(); }),
      // The colony's balances as its last update left them (read-only): spare power (generated less what its
      // working structures use), and the food stored's change over the last food update.
      "power",      sol::property([](const LuaPlayer& p) { const auto* i = _Player(p.num).GetImpl(); return i ? i->amountPowerAvailable_ : 0; }),
      "food_net",   sol::property([](const LuaPlayer& p) { const auto* i = _Player(p.num).GetImpl(); return i ? i->netFoodProduction_ : 0; }),
      // How much common and rare ore the colony can hold (read-only): the game caps `ore` / `rare_ore` at these, as
      // its last update worked them out from the colony's storage.
      "ore_capacity",      sol::property([](const LuaPlayer& p) { const auto* i = _Player(p.num).GetImpl(); return i ? i->maxCommonOre_ : 0; }),
      "rare_ore_capacity", sol::property([](const LuaPlayer& p) { const auto* i = _Player(p.num).GetImpl(); return i ? i->maxRareOre_ : 0; }),
      // cost(type [, weapon]) -> { common=, rare= } (see Player_Cost).
      "cost",       [](const LuaPlayer& p, const std::string& t, sol::object w, sol::this_state ts) { return Player_Cost(p.num, t, w, ts); },
      // The difficulty level chosen in the Colony Games menu, as "Easy"/"Normal"/"Hard". Lets a mission
      // tweak its settings per difficulty (e.g. AI resources, disaster timing).
      "difficulty", sol::property([](const LuaPlayer& p) -> std::string {
        switch (static_cast<int>(_Player(p.num).GetDifficulty())) {
          case 0:  return "Easy";
          case 1:  return "Normal";
          case 2:  return "Hard";
          default: return "Unknown";
        } }),
      // player:satellite_count("EDWARDSatellite" / "RareMetalsCargo" / ...) - how many of a satellite or
      // launched Spaceport cargo rocket the player has. Drives Starship-colony evacuation victories.
      "satellite_count", [](const LuaPlayer& p, const std::string& type) {
        return _Player(p.num).GetSatelliteCount(MapIdFromName(type)); },
      // The EDWARD and Solar Power Satellites the player has in orbit (0-15 each), read and written. With an EDWARD
      // Satellite up every mining beacon counts as surveyed for the player (MiningBeacon::IsSurveyed 0x4055F0), so its
      // Robo-Miners need no Robo-Surveyor.
      "edward_satellites", sol::property(
        [](const LuaPlayer& p) { const auto* i = _Player(p.num).GetImpl(); return i ? int(i->satellites_.numEDWARDSatllites) : 0; },
        [](LuaPlayer& p, int n) { if (auto* i = _Player(p.num).GetImpl()) i->satellites_.numEDWARDSatllites = static_cast<uint32_t>(std::clamp(n, 0, 15)); }),
      "solar_satellites", sol::property(
        [](const LuaPlayer& p) { const auto* i = _Player(p.num).GetImpl(); return i ? int(i->satellites_.numSolarSatellites) : 0; },
        [](LuaPlayer& p, int n) { if (auto* i = _Player(p.num).GetImpl()) i->satellites_.numSolarSatellites = static_cast<uint32_t>(std::clamp(n, 0, 15)); }),
      // player:set_launched(type, on): marks a starship module or cargo (IonDriveModule ... ChildrenModule, the
      // Skydock among them) launched or not, for a mission that starts with the starship in orbit. Each is one bit,
      // 13 + (type - IonDriveModule), of the word the EDWARD and Solar counts share; satellite_count reads it back.
      "set_launched", [](LuaPlayer& p, const std::string& type, bool on) {
        const int t = static_cast<int>(MapIdFromName(type));
        const int first = static_cast<int>(MapID::IonDriveModule), last = static_cast<int>(MapID::ChildrenModule);
        if (t < first || t > last) throw sol::error("set_launched: not a starship module or cargo: " + type);
        if (auto* i = _Player(p.num).GetImpl()) {
          auto& word = reinterpret_cast<uint32_t&>(i->satellites_);
          const uint32_t bit = 1u << (13 + t - first);
          word = on ? (word | bit) : (word & ~bit);
        } },

      // ---- technology ----
      // has_tech / mark_research validate the tech id first - a bad id crashes the engine's lookup
      // (HasTechnology/MarkResearchComplete don't guard it). On a bad id they log an error + show an
      // in-game message and no-op (has_tech returns false). See IsValidTechId / ReportBadTech.
      "has_tech",        [](const LuaPlayer& p, int techId) {
                           if (!IsValidTechId(techId)) { ReportBadTech("has_tech", techId); return false; }
                           return bool(_Player(p.num).HasTechnology(techId)); },
      "mark_research",   [](LuaPlayer& p, int techId) {
                           if (!IsValidTechId(techId)) { ReportBadTech("mark_research", techId); return; }
                           _Player(p.num).MarkResearchComplete(techId); },
      // unmark_research(tech) - the player no longer has the topic: the opposite of mark_research, so a mission can
      // take a topic back (and with it what it opens up).
      "unmark_research", [](LuaPlayer& p, int techId) {
                           if (!IsValidTechId(techId)) { ReportBadTech("unmark_research", techId); return; }
                           Tethys::TechInfo*const info = Tethys::Research::GetInstance()->GetTechInfo(techId);
                           if (info != nullptr) info->playerHasTechMask.Set(p.num, false); },
      // unit_stat(type, field) / set_unit_stat(type, field, value) - the player's stats for a unit type (a name, or a
      // raw MapID number; the per-player
      // record the tech tree's UNIT_PROP upgrades change): "hp", "repair", "armor" (the raw armor value: 256, 180,
      // 130, 90, 60, 40 for classes 0-5), "common_cost", "rare_cost", "build_time", "sight_range", and, by kind of
      // unit, "move_speed" / "turn_rate" / "production_rate" / "reload_time" / "cargo_capacity" (vehicles),
      // "concussion" / "penetration" / "reload_time" / "weapon_range" (weapons), "power" / "workers" / "scientists" /
      // "storage" / "production_capacity" / "storage_bays" (buildings).
      "unit_stat",       [](const LuaPlayer& p, sol::object type, const std::string& field) -> int {
                           const int*const v = UnitStatPtr(p.num, type, field, "unit_stat");
                           return v ? *v : 0; },
      "set_unit_stat",   [](LuaPlayer& p, sol::object type, const std::string& field, int value) {
                           if (int*const v = UnitStatPtr(p.num, type, field, "set_unit_stat")) *v = value; },
      "set_tech_level",  [](LuaPlayer& p, int level)  { _Player(p.num).SetTechLevel(level); },
      "can_research",    [](const LuaPlayer& p, int techId) { return Player_CanResearch(p.num, techId); },

      // ---- alliances ----
      "is_ally",            [](const LuaPlayer& p, sol::object o) { const int n = PlayerNumFrom(o);
                              return ValidPlayerNum(n) && _Player(p.num).IsAlly(n); },
      "ally_with",          [](LuaPlayer& p, sol::object o) { const int n = PlayerArg(o, "ally_with");
                              if (n >= 0) _Player(p.num).AllyWith(n); },
      "mutually_ally_with", [](LuaPlayer& p, sol::object o) { const int n = PlayerArg(o, "mutually_ally_with");
                              if (n >= 0) _Player(p.num).MutuallyAllyWith(n); },

      // ---- the game's own checks of what the colony can still do. The game caches the answers in its own player
      //      record (Player[], 0x56C280), so these call through Player[n] rather than a fresh _Player, whose cache
      //      reads "no". Call reset_checks first, as the stock missions do ----
      "reset_checks",            [](LuaPlayer& p) { Tethys::TethysAPI::Player[p.num].ResetChecks(); },
      "can_build_building",      [](LuaPlayer& p) { return bool(Tethys::TethysAPI::Player[p.num].CanBuildBuilding()); },
      // can_build_vehicle([arg]): called at 0x4777F0 with `arg` (default 0, what Tethys passes); the stock missions
      // pass 1 (Eden 3's CheckForFail).
      "can_build_vehicle",       [](LuaPlayer& p, sol::optional<int> arg) {
                                   using Fn = int(__thiscall*)(void*, int);
                                   return reinterpret_cast<Fn>(0x4777F0)(&Tethys::TethysAPI::Player[p.num], arg.value_or(0)) != 0; },
      "can_build_space",         [](LuaPlayer& p) { return bool(Tethys::TethysAPI::Player[p.num].CanBuildSpace()); },
      "can_accumulate_ore",      [](LuaPlayer& p) { return bool(Tethys::TethysAPI::Player[p.num].CanAccumulateOre()); },
      "can_accumulate_rare_ore", [](LuaPlayer& p) { return bool(Tethys::TethysAPI::Player[p.num].CanAccumulateRareOre()); },
      // can_do_research(tech): the game's _Player::canDoResearch (Eden 2's CheckForFail asks it of each database it
      // must rebuild); false for an id the tree hasn't, which isn't reported. Compare can_research, which asks the
      // research tree as a lab's topic list does.
      "can_do_research",         [](LuaPlayer& p, int techId) {
                                   return IsValidTechId(techId) && bool(Tethys::TethysAPI::Player[p.num].CanDoResearch(techId)); },
      // player:total_strength(): the game's measure of the player's military (GetTotalPlayerStrength, 0x477DA0), which
      // CPP1 sizes its attack waves by.
      "total_strength",          [](LuaPlayer& p) { return Tethys::TethysAPI::Player[p.num].GetTotalPlayerStrength(); },

      // ---- faction / control ----
      "go_eden",     [](LuaPlayer& p) { _Player(p.num).GoEden(); },
      "go_plymouth", [](LuaPlayer& p) { _Player(p.num).GoPlymouth(); },
      "go_human",    [](LuaPlayer& p) { _Player(p.num).GoHuman(); },
      "go_ai",       [](LuaPlayer& p) { _Player(p.num).GoAI(); },
      // set_default_group(group): the group a computer player's new units join when no other takes them (set on the
      // game's own player record, as the checks above).
      "set_default_group", [](LuaPlayer& p, const LuaGroup& g) { if (g.id >= 0) Tethys::TethysAPI::Player[p.num].SetDefaultGroup(ScGroup(g.id)); },
      // default_group(): the player's default group as the game keeps it: the fight group it makes for each player at
      // the start, unless a mission set another (GetDefaultGroup 0x4775C0); an invalid Group if there is none. Handed
      // out as a Fight group (AutoDemo 2 sets its area and lights).
      "default_group", [](const LuaPlayer& p) {
                         // _Player::GetDefaultGroup (0x4775C0) returns its ScGroup through a hidden pointer (thiscall: the
                         // player in ecx, the pointer on the stack); Tethys's thunk passes them otherwise and writes the
                         // group's id over the player's (Player[n].id_).
                         int ret[2] = { -1, -1 };
                         using Fn = int*(__thiscall*)(void*, int*);
                         reinterpret_cast<Fn>(0x4775C0)(&Tethys::TethysAPI::Player[p.num], ret);
                         const int id = ret[0];
                         return id >= 0 ? LuaGroup{ id, GroupKind::Fight } : LuaGroup{}; },
      "center_view", [](const LuaPlayer& p, sol::object loc) { _Player(p.num).CenterViewOn(LocFrom(loc)); });

  RegisterGroupBindings(lua);
  Hackaray::Register(lua);
  Campaign::Register(lua);

  lua.new_usertype<Region>(
      "Region", sol::no_constructor,
      "units",    &Region_Units,
      "contains", &Region_Contains,
      "center",   [](const Region& r, sol::this_state ts) {
        sol::state_view L(ts);
        sol::table t = L.create_table();
        // The region holds internal (padded-grid) tiles; its centre comes back in author coords like unit.x/y, so
        // the table can go straight to `at =` (LocFrom pads a table again). Passing the region itself also works.
        t["x"] = (r.x1 + r.x2) / 2 - GameMap::GetPaddingWidth() + 1;
        t["y"] = (r.y1 + r.y2) / 2 + 1;
        return t;
      });

  // Default empty handles; replaced by the placement loader at mission start.
  lua["players"] = lua.create_table();
  lua["units"]   = lua.create_table();
  lua["regions"] = lua.create_table();
  lua["markers"] = lua.create_table();

  LOG(INFO) << "Registered OP2Lua bindings (game, mission, Unit, Player, Region, ChatArgs)";
}

}  // namespace OP2Lua
