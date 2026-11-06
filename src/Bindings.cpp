// Bindings.cpp - the author-facing OP2Lua API, bound to the Tethys game API via sol2.
//
// game.*     message(text[,sound]), sound(name|id[,to]), rand, tick, create_unit, minutes/seconds
// mission.*  win, lose, last_one_standing, victory, failure
// trigger.*  count, resource, set, midas, one_player_left, operational, vehicle_count, building_count
// Unit       id/type/type_id/alive/owner/valid, move, attack_move, ==
// Player     is_eden/is_human, owns_any, unit_count
// Region     :units(player?), :contains(unit), :center()
// ChatArgs   text (get/set), player
//
// Determinism (multiplayer): the only randomness exposed is game.rand -> the engine's SYNCED RNG.
// State-changing calls go through Tethys (synced commands); no wall-clock, no math.random/os/io.

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "ApiTypes.h"
#include "LuaHost.h"
#include "Log.h"
#include "Names.h"

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
#include "Tethys/Game/MapObject.h"      // Tethys::MapObj::MiningBeacon (ore type for beacon perception)
#include "Tethys/Game/MineManager.h"    // OreYield, OreVariant (game.create_beacon)
#include "Tethys/Game/MapObjectType.h"  // Tethys::MapObjectType (building footprint for site finding)
#include "Tethys/Game/Research.h"       // Tethys::Research (validate tech ids before use - see ReportBadTech)
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

// Deferred unit orders (see ApiTypes.h). Issued one tick after they're requested.
enum class OrderKind { Move, AttackUnit, AttackGround };
struct DeferredOrder { Unit unit; OrderKind kind; int x; int y; Unit target; int attempts = 0; int readyTick = -1; };
std::vector<DeferredOrder> gOrders;

// Resolve a sound argument: a name ("Savant4"), a raw integer SoundID, or nil -> default.
Tethys::SoundID ResolveSound(const sol::object& s, Tethys::SoundID dflt) {
  if (s.is<int>()) return static_cast<Tethys::SoundID>(s.as<int>());
  if (s.is<std::string>()) {
    const int v = SoundIdFromName(s.as<std::string>());
    if (v >= 0) return static_cast<Tethys::SoundID>(v);
  }
  return dflt;
}

// Convert a Lua value to an internal Location. A Unit's and a Region's coords are already internal;
// a raw {x=,y=}/{x,y} table is author-facing (editor/status-bar) coords -> translate via GameMap::At.
Location LocFrom(const sol::object& o) {
  if (o.is<Unit>())   return o.as<Unit>().GetLocation();
  if (o.is<Region>()) { const Region& r = o.as<Region>(); return Location((r.x1 + r.x2) / 2, (r.y1 + r.y2) / 2); }
  if (o.is<sol::table>()) {
    sol::table t = o.as<sol::table>();
    sol::optional<int> x = t["x"], y = t["y"];
    if (x && y) return GameMap::At(*x, *y);
    return GameMap::At(t.get_or(1, 0), t.get_or(2, 0));
  }
  return Location();
}

// A MapRect (internal coords) from a Region, a Unit's footprint, a {x1,y1,x2,y2} rect, or a {x,y}
// single tile (author coords -> GameMap::At). Used by area orders (build_wall / doze / salvage).
MapRect RectFrom(const sol::object& o) {
  if (o.is<Region>()) { const Region& r = o.as<Region>(); return MapRect(r.x1, r.y1, r.x2, r.y2); }
  if (o.is<Unit>())   { return o.as<Unit>().GetRect(); }
  if (o.is<sol::table>()) {
    sol::table t = o.as<sol::table>();
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

// game.message(text [, sound]) - in-game message log (presentation; safe per-client).
void Game_Message(const std::string& text, sol::object sound) {
  Game::AddMessage(text, ResolveSound(sound, kDefaultMsgSound), Tethys::AllPlayers);
  LOG_MISSION(INFO) << "game.message: " << text;
}

// game.sound(name|id [, toPlayer]) - play a global game sound.
void Game_Sound(sol::object sound, sol::optional<int> toPlayer) {
  Game::AddGameSound(ResolveSound(sound, static_cast<Tethys::SoundID>(0)),
                     toPlayer.value_or(Tethys::AllPlayers));
}

int Game_Rand(int n) { return (n > 0) ? Game::GetRand(n) : 0; }  // synced RNG, [0, n)
int Game_Tick()      { return Game::Tick(); }

Tethys::MoraleLevel MoraleFromName(const std::string& n) {
  if (n == "Excellent" || n == "Great")  return Tethys::MoraleLevel::Excellent;
  if (n == "Fair" || n == "OK")          return Tethys::MoraleLevel::Fair;
  if (n == "Poor")                       return Tethys::MoraleLevel::Poor;
  if (n == "Terrible" || n == "Rotten")  return Tethys::MoraleLevel::Terrible;
  return Tethys::MoraleLevel::Good;
}

// game.morale_steady([level]) - lock all players' morale at a level (default "Good") so the colony
// doesn't fluctuate or collapse. This is the multiplayer "Morale Steady" rule: focus on fighting.
void Game_MoraleSteady(sol::object level) {
  const auto lvl = level.is<std::string>() ? MoraleFromName(level.as<std::string>())
                                           : Tethys::MoraleLevel::Good;
  Game::ForceMoraleLevel(lvl, Tethys::AllPlayers);
  LOG_MISSION(INFO) << "game.morale_steady";
}
// game.free_morale([player]) - release the lock (all players, or one: a Player or a 1-based number); morale
// fluctuates normally again.
void Game_FreeMorale(sol::object player) {
  const bool all = !player.valid() || player == sol::lua_nil;
  Game::FreeMoraleLevel(all ? static_cast<int>(Tethys::AllPlayers) : PlayerNumFrom(player));
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
  const sol::object atObj = opts["at"];
  const Location at = LocFrom(atObj);
  const int count = opts.get_or("count", 1);

  MapID weapon = MapID::None;
  sol::optional<std::string> w = opts["weapon"];
  if (w) weapon = MapIdFromName(*w);
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
  CreateVictoryCondition(CreateTimeTrigger(1, "OP2LuaTrigger", true, true), "Mission accomplished", true, true);
}
void Mission_Lose(sol::variadic_args) {
  using namespace Tethys::TethysAPI;
  LOG_MISSION(INFO) << "mission.lose()";
  CreateFailureCondition(CreateTimeTrigger(1, "OP2LuaTrigger", true, true), true);
}
// mission.last_one_standing([text]) - the multiplayer Last One Standing victory, as the stock ml*.dll
// missions set it up: the game's own one-player-left trigger wrapped in a victory condition. The game
// decides it per player (a player is out once they have no working Command Center), so unlike
// mission.win() it is fair in a multiplayer game. `text` is the objective shown in the game's list.
void Mission_LastOneStanding(sol::optional<std::string> text) {
  using namespace Tethys::TethysAPI;
  // The game keeps the text's pointer, not a copy (it shows it in Mission Objectives for the whole game),
  // so the text must outlive this call: kept here for the process's life (a few bytes per mission).
  static std::deque<std::string> kept;
  const std::string& objective = kept.emplace_back(text.value_or("Eliminate your opponents."));
  LOG_MISSION(INFO) << "mission.last_one_standing(\"" << objective << "\")";
  CreateVictoryCondition(CreateLastOneStandingTrigger("OP2LuaTrigger", true, true), objective, true, true);
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
// The functions live in this Lua state only, so a saved game would lose them (multiplayer games aren't saved).
using Tethys::TethysAPI::CompareMode;
using Tethys::TethysAPI::Trigger;
using Tethys::TethysAPI::TriggerResource;

std::map<int, sol::protected_function> gTriggerFns;

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
bool OneShot(const sol::table& o, bool dflt) { return o.get_or("one_shot", dflt); }
bool Enabled(const sol::table& o)            { return o.get_or("enabled", true); }

// trigger.count{ type=, cargo=, compare=">=", count=1, player= } - how many units of a type a player (or any
// player) has; `cargo` narrows it (a ConVec's kit, a weapon; any if left out). Space Race: type="IonDriveModule".
Trigger Trigger_Count(sol::table o) {
  const MapID type = MapIdFromName(o.get_or("type", std::string()));
  const std::string cargo = o.get_or("cargo", std::string());
  const MapID cw = cargo.empty() ? MapID::Any : MapIdFromName(cargo);   // no cargo given: any (as the stock maps)
  return Remember(Tethys::TethysAPI::CreateCountTrigger(type, cw, CompareFrom(o, ">="), o.get_or("count", 1),
                                                        TriggerFnName(o), PlayerOpt(o), OneShot(o, false), Enabled(o)),
                  o, "count");
}
// trigger.resource{ resource=, compare=">=", amount=, player= } - resource: "Food" / "CommonOre" / "RareOre" /
// "Kids" / "Workers" / "Scientists" / "Colonists".
Trigger Trigger_Resource(sol::table o) {
  static const std::map<std::string, TriggerResource> names = {
      {"Food", TriggerResource::Food}, {"CommonOre", TriggerResource::CommonOre}, {"RareOre", TriggerResource::RareOre},
      {"Kids", TriggerResource::Kids}, {"Workers", TriggerResource::Workers},
      {"Scientists", TriggerResource::Scientists}, {"Colonists", TriggerResource::Colonists}};
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
// trigger.one_player_left{} - the Last One Standing condition (mission.last_one_standing wraps it in a victory).
Trigger Trigger_OnePlayerLeft(sol::table o) {
  return Remember(Tethys::TethysAPI::CreateLastOneStandingTrigger(TriggerFnName(o), OneShot(o, true), Enabled(o)),
                  o, "one_player_left");
}
// trigger.operational{ type=, compare=">=", count=1, player= } - a player's working structures of a type (a
// player is needed: the game misjudges "any player"). Land Rush: a Command Center up.
Trigger Trigger_Operational(sol::table o) {
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
// trigger.vehicle_count{ compare=, count=, player= } / trigger.building_count{...} - all of a player's vehicles
// (or structures).
Trigger Trigger_VehicleCount(sol::table o) {
  return Remember(Tethys::TethysAPI::CreateVehicleCountTrigger(CompareFrom(o, ">="), o.get_or("count", 1),
                                                               TriggerFnName(o), PlayerOpt(o), OneShot(o, false), Enabled(o)),
                  o, "vehicle_count");
}
Trigger Trigger_BuildingCount(sol::table o) {
  return Remember(Tethys::TethysAPI::CreateBuildingCountTrigger(CompareFrom(o, ">="), o.get_or("count", 1),
                                                                TriggerFnName(o), PlayerOpt(o), OneShot(o, false), Enabled(o)),
                  o, "building_count");
}

// mission.victory(trigger, text[, {one_shot=false, enabled=true}]) - a victory condition: a player wins once
// `trigger` fires for them (all of a game's victory conditions, for one player). `text` is the objective in the
// game's Mission Objectives list; the game keeps its pointer, so it is kept here for the process's life.
Trigger Mission_Victory(Trigger condition, const std::string& text, sol::optional<sol::table> opts) {
  static std::deque<std::string> kept;
  const std::string& objective = kept.emplace_back(text);
  const bool once = opts ? opts->get_or("one_shot", false) : false;
  const bool on = opts ? opts->get_or("enabled", true) : true;
  LOG_MISSION(INFO) << "mission.victory(trigger " << condition.GetID() << ", \"" << objective << "\")";
  return Tethys::TethysAPI::CreateVictoryCondition(condition, objective, once, on);
}
// mission.failure(trigger[, {enabled=true}]) - a failure condition: a player loses once `trigger` fires for them.
Trigger Mission_Failure(Trigger condition, sol::optional<sol::table> opts) {
  const bool on = opts ? opts->get_or("enabled", true) : true;
  LOG_MISSION(INFO) << "mission.failure(trigger " << condition.GetID() << ")";
  return Tethys::TethysAPI::CreateFailureCondition(condition, on);
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

// game.find_build_site(type, from [, maxRadius]) -> {x=,y=} the DoBuild anchor (bottom-right, author
// coords) of the nearest spot to `from` where the WHOLE footprint of `type` is buildable, or nil.
// Rings outward and validates every footprint tile with the engine's CanPlaceStructure - so it skips
// existing buildings, cliffs, lava, etc. Feed the result straight to unit:build(). This is the proper
// "place a structure without colliding with what's already on the map".
sol::object Game_FindBuildSite(const std::string& typeName, sol::object from, sol::optional<int> maxRadius,
                               sol::this_state ts) {
  sol::state_view L(ts);
  Tethys::MapObjectType*const mot = Tethys::MapObjectType::GetInstance(static_cast<int>(MapIdFromName(typeName)));
  if (mot == nullptr) return sol::make_object(L, sol::lua_nil);
  const Location origin = LocFrom(from);
  const int maxR = maxRadius.value_or(30);
  // Valid internal-tile bounds. CanPlaceStructure does NOT range-check; off-map coords crash it, and the
  // base can sit in a corner so the ring runs off the edge - so we must clamp the footprint ourselves.
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
// including the 1-tile build-clearance border (tw x th), or nil. Lets the AI reason about how big each
// building is (spacing, base layout). E.g. Nursery = 2x2 (4x4 total), Vehicle Factory = 4x3 (6x5 total).
sol::object Game_Footprint(const std::string& typeName, sol::this_state ts) {
  sol::state_view L(ts);
  Tethys::MapObjectType*const mot = Tethys::MapObjectType::GetInstance(static_cast<int>(MapIdFromName(typeName)));
  if (mot == nullptr) return sol::make_object(L, sol::lua_nil);
  const Location c(100, 100);                         // any in-bounds center; we only want the size
  const MapRect core = mot->GetTileRect(c, false);
  const MapRect tot  = mot->GetTileRect(c, true);
  sol::table out = L.create_table();
  out["w"]  = core.x2 - core.x1 + 1;  out["h"]  = core.y2 - core.y1 + 1;
  out["tw"] = tot.x2  - tot.x1  + 1;  out["th"] = tot.y2  - tot.y1  + 1;
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
static bool GuardedTransferCargo(Unit u, int bay)      { __try { u.DoTransferCargo(bay); return true; } __except (1) { return false; } }

void FlushUnitOrders() {
  // A freshly-created unit is DRAWN and IsLive() within a tick, but its movement/command state isn't
  // fully initialized for several ticks. Executing a move/attack command on it before then crashes the
  // GAME in ProcessUnits -> ProcessCommands (write through an uninitialized field). CmdMove itself
  // succeeds, so we can't catch it - the only safe fix is to hold the command until the unit has aged.
  constexpr int kDelayTicks  = 25;          // ~aged a full game-second before we command it (invisible in play)
  constexpr int kMaxAttempts = 20;
  const int now = Game::Tick();
  std::vector<DeferredOrder> retry;
  int issued = 0;
  for (auto& o : gOrders) {
    if (!o.unit.IsLive()) continue;                        // unit gone before its order ripened -> drop it
    if (o.readyTick < 0) o.readyTick = now + kDelayTicks;  // first time seen live: start the safety clock
    if (now < o.readyTick) { retry.push_back(o); continue; }  // not aged enough yet -> hold it
    if (IssueOrderGuarded(o)) { ++issued; continue; }     // command issued cleanly
    // Still not command-ready (would have crashed) -> keep and retry next cycle.
    if (++o.attempts < kMaxAttempts) retry.push_back(o);
    else LOG_AI(WARNING) << "dropped a unit order after " << kMaxAttempts << " attempts";
  }
  // Issuing queued unit orders (move/attack) is scripted-unit activity -> the AI log.
  if (issued > 0) LOG_AI(INFO) << "issued " << issued << " queued unit order(s) at tick " << now;
  gOrders.swap(retry);
}
void ResetUnitOrders() { gOrders.clear(); }

void ResetTriggers() { gTriggerFns.clear(); }

void FireTrigger(void* pArgs) {
  const auto* a = static_cast<const Tethys::TethysAPI::OnTriggerArgs*>(pArgs);
  if (a == nullptr) return;
  const auto it = gTriggerFns.find(a->trigger.GetID());
  if (it == gTriggerFns.end()) return;
  sol::protected_function fn = it->second;   // a copy: the function may make more triggers
  try {
    sol::protected_function_result r = fn(a->trigger);
    if (!r.valid()) { sol::error e = r; LOG_MISSION(ERROR) << "trigger " << a->trigger.GetID() << ": " << e.what(); }
  } catch (const std::exception& e) {
    LOG_MISSION(ERROR) << "trigger " << a->trigger.GetID() << ": " << e.what();
  }
}

// When on, AI debug (log.ai) is also echoed to the in-game comms panel so a dev can watch the AI
// without leaving the game. Reset per mission (fresh Lua state) and toggled via log.show_ai_in_game().
static bool gAIToComms = false;

// Supplied to the logger so every line carries the game mark (100 ticks). Reads ~0 before a game is
// running. Fully qualified so it compiles regardless of which `using`s are in scope here.
int CurrentMarkForLog() { return Tethys::TethysAPI::Game::Tick() / 100; }

void RegisterBindings(sol::state& lua) {
  gAIToComms = false;   // each mission starts with AI events NOT shown in-game
  sol::table game = lua.create_named_table("game");
  game.set_function("message", &Game_Message);
  game.set_function("sound", &Game_Sound);
  game.set_function("rand", &Game_Rand);
  game.set_function("tick", &Game_Tick);
  game.set_function("morale_steady", &Game_MoraleSteady);
  game.set_function("free_morale", &Game_FreeMorale);
  game.set_function("create_unit", &Game_CreateUnit);

  // ---- world queries ----
  game.set_function("num_players", []() { return Game::NumPlayers(); });
  game.set_function("num_humans",  []() { return Game::NumHumans();  });
  // The multiplayer lobby's choices (a Colony game has no lobby: these read the game's defaults).
  game.set_function("initial_units",  []() { return Game::InitialUnits(); });  // starting vehicles, 0-12
  game.set_function("uses_day_night", []() { return Game::UsesDayNight(); });  // day and night on
  game.set_function("uses_morale",    []() { return Game::UsesMorale();   });  // morale free (not steady)
  game.set_function("beacons",     &Game_Beacons);     // discover mining beacons (perception)
  game.set_function("find_place",  &Game_FindPlace);   // find a clear spot to build/place a type
  game.set_function("find_build_site", &Game_FindBuildSite); // footprint-valid structure site (DoBuild anchor)
  game.set_function("footprint",   &Game_Footprint);   // a structure type's tile size (core + total)
  game.set_function("cell",        &Game_Cell);        // read a tile's terrain (passable/cliff/lava/...)
  game.set_function("can_place",   &Game_CanPlace);    // engine check: can a structure tile go here?
  game.set_function("map_size",    &Game_MapSize);     // playable map dimensions

  // ---- runtime map features (set-pieces created during the mission, not just at placement) ----
  game.set_function("create_wall", [](const std::string& type, sol::object area) {
    Game::CreateWall(MapIdFromName(type), RectFrom(area)); });                 // type: "Wall"/"Tube"/"LavaWall"/...
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
  lua.new_usertype<Trigger>(
      "Trigger", sol::no_constructor,
      "id",        sol::property([](const Trigger& t) { return t.GetID(); }),
      "valid",     sol::property([](const Trigger& t) { return t.IsValid(); }),
      "enabled",   sol::property([](const Trigger& t) { return t.IsValid() && t.IsEnabled() != 0; }),
      "enable",    [](Trigger& t) { if (t.IsValid()) t.Enable(); },
      "disable",   [](Trigger& t) { if (t.IsValid()) t.Disable(); },
      "has_fired", [](Trigger& t, sol::object player) { return t.IsValid() && t.HasFired(PlayerNumFrom(player)) != 0; },
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
                                                    gOrders.push_back({ u, OrderKind::Move, p.x, p.y, Unit() }); },
      // attack_move(target): if given a unit, hunt and attack it; if given a location, advance there
      // aggressively (DoAttack-to-ground, firing en route). NOTE: a plain DoMove (CmdMove) on a unit
      // freshly created this/last tick crashes OP2's pathfinder - DoAttack does not. This is the stable
      // r308 behaviour; r310 had changed the location case to Move, which is what broke spawned-defender
      // missions. Keep this as AttackGround.
      "attack_move", [](Unit& u, sol::object tgt) {
                       if (tgt.is<Unit>()) { Unit t = tgt.as<Unit>(); const Location p = t.GetLocation();
                                             gOrders.push_back({ u, OrderKind::AttackUnit, p.x, p.y, t }); }
                       else                { const Location p = LocFrom(tgt);
                                             gOrders.push_back({ u, OrderKind::AttackGround, p.x, p.y, Unit() }); } },
      // attack(target): attack a specific unit, or (with a location) deliberately attack the ground.
      "attack",      [](Unit& u, sol::object tgt) {
                       if (tgt.is<Unit>()) { Unit t = tgt.as<Unit>(); const Location p = t.GetLocation();
                                             gOrders.push_back({ u, OrderKind::AttackUnit, p.x, p.y, t }); }
                       else                { const Location p = LocFrom(tgt);
                                             gOrders.push_back({ u, OrderKind::AttackGround, p.x, p.y, Unit() }); } },
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
                        if (t != MapID::None) GuardedProduce(u, t, weapon ? MapIdFromName(*weapon) : MapID::None);
                        else LOG_MISSION(WARNING) << "produce: unknown type '" << type << "'"; },
      // factory:set_factory_cargo(bay, type [, cargoOrWeapon]) - preload a kit into a 1-based bay (1..6).
      "set_factory_cargo", [](Unit& u, int bay, const std::string& type, sol::optional<std::string> extra) {
                        if (u.IsLive()) u.SetFactoryCargo(bay - 1, MapIdFromName(type),
                                                          extra ? MapIdFromName(*extra) : MapID::None); },
      // factory:transfer_cargo(bay) - move the 1-based bay's cargo to the docked vehicle. ONLY call when
      // a vehicle is actually on the dock (see unit_on_dock) - calling it otherwise crashes OP2.
      "transfer_cargo", [](Unit& u, int bay) { if (u.IsLive()) GuardedTransferCargo(u, bay - 1); },
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
      "is_infected",        sol::property([](const Unit& u) { return u.IsInfected(); }),
      "disabled",           sol::property([](const Unit& u) { return u.IsDisabled(); }),
      "busy",               sol::property([](const Unit& u) { return u.IsBusy(); }),
      "is_emped",           sol::property([](const Unit& u) { return u.IsEMPed(); }),

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
      // factory_cargo(bay): the kit type name in a 1-based factory bay ("None" if empty) - "what's in my SF".
      "factory_cargo", [](const Unit& u, int bay) { return NameFromMapId(u.GetFactoryCargo(bay - 1).unitType); },

      // ---- health ----
      // hp / max_hp (hit points), health (0..1 fraction). damage(n) inflicts n; heal(n) repairs n.
      "max_hp",  sol::property([](const Unit& u) { return u.GetMaxHitpoints(); }),
      "hp",      sol::property([](const Unit& u) { return u.GetMaxHitpoints() - u.GetDamage(); }),
      "health",  sol::property([](const Unit& u) { const int m = u.GetMaxHitpoints();
                                                   return (m > 0) ? double(m - u.GetDamage()) / m : 0.0; }),
      "damage",  [](Unit& u, int n) { if (u.IsLive()) u.AddDamage(n); },                       // SetDamage kills if >= max
      "heal",    [](Unit& u, int n) { if (u.IsLive()) u.SetDamage(std::max(0, u.GetDamage() - n)); },

      // ---- lifecycle / ownership ----
      "stop",          [](Unit& u) { if (u.IsLive()) u.DoStop(); },
      "self_destruct", [](Unit& u) { if (u.IsLive()) u.DoSelfDestruct(); },
      "kill",          [](Unit& u) { if (u.IsLive()) u.DoDeath(); },                            // die with explosion
      "remove",        [](Unit& u) { if (u.IsLive()) u.DoPoof(); },                             // vanish, no animation
      "transfer",      [](Unit& u, sol::object player) { const int p = PlayerNumFrom(player);
                         if (u.IsLive() && p >= 0) GuardedTransfer(u, p); },

      // ---- combat positioning ----
      "guard",        [](Unit& u, Unit what) { if (u.IsLive() && what.IsValid()) GuardedGuard(u, what); },
      "stand_ground", [](Unit& u, sol::object loc) { if (u.IsLive()) GuardedStandGround(u, LocFrom(loc)); },

      // ---- weapon ----
      "weapon",     sol::property([](const Unit& u) { return NameFromMapId(u.GetWeapon()); }),
      "set_weapon", [](Unit& u, const std::string& w) { if (u.IsLive()) u.SetWeapon(MapIdFromName(w)); },

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
      "dock_at_garage", [](Unit& u, Unit garage) { if (u.IsLive() && garage.IsValid()) GuardedDockGarage(u, garage); },
      "set_lights",     [](Unit& u, bool on) { if (u.IsLive()) u.DoSetLights(on); },
      // [ConVec] structure kit it carries. The kit, a tank's weapon, and a truck's cargo all share one
      // union field in OP2 (a unit is only ever one of those), so set_kit / set_weapon / set_cargo are
      // really the same slot under different names - matching the unit type you call them on.
      "kit",            sol::property([](const Unit& u) { return NameFromMapId(u.GetConVecCargo().unitType); }),
      "set_kit",        [](Unit& u, const std::string& type, sol::optional<std::string> topping) {
                          if (u.IsLive()) u.SetCargo(MapIdFromName(type), topping ? MapIdFromName(*topping) : MapID::None); },

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
      "set_tech_level",  [](LuaPlayer& p, int level)  { _Player(p.num).SetTechLevel(level); },

      // ---- alliances ----
      "is_ally",            [](const LuaPlayer& p, sol::object o) { const int n = PlayerNumFrom(o);
                              return (n >= 0) && _Player(p.num).IsAlly(n); },
      "ally_with",          [](LuaPlayer& p, sol::object o) { const int n = PlayerNumFrom(o);
                              if (n >= 0) _Player(p.num).AllyWith(n); },
      "mutually_ally_with", [](LuaPlayer& p, sol::object o) { const int n = PlayerNumFrom(o);
                              if (n >= 0) _Player(p.num).MutuallyAllyWith(n); },

      // ---- faction / control ----
      "go_eden",     [](LuaPlayer& p) { _Player(p.num).GoEden(); },
      "go_plymouth", [](LuaPlayer& p) { _Player(p.num).GoPlymouth(); },
      "go_human",    [](LuaPlayer& p) { _Player(p.num).GoHuman(); },
      "go_ai",       [](LuaPlayer& p) { _Player(p.num).GoAI(); },
      "center_view", [](const LuaPlayer& p, sol::object loc) { _Player(p.num).CenterViewOn(LocFrom(loc)); });

  lua.new_usertype<Region>(
      "Region", sol::no_constructor,
      "units",    &Region_Units,
      "contains", &Region_Contains,
      "center",   [](const Region& r, sol::this_state ts) {
        sol::state_view L(ts);
        sol::table t = L.create_table();
        // Region coords are OP2 engine-internal (padded-grid) values, kept as-is on
        // purpose: the engine requires the internal offset (~x+31 on normal maps),
        // same convention as the C++ mission SDK. Placement in-game is correct.
        t["x"] = (r.x1 + r.x2) / 2;
        t["y"] = (r.y1 + r.y2) / 2;
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
