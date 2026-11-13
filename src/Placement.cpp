// Placement.cpp - applies a mission's placement.lua to the running game.
//
// placement.lua is a pure data table (generated from a .opm) describing players, starting units,
// resources, beacons, walls, and named regions. Load() parses and stores it; Apply() (called from
// InitProc, before on_init) builds the level via the Tethys API and publishes the named handles
// (players[], units[], regions[], markers[]) that mission.lua refers to.

#include "Placement.h"

#include <algorithm>
#include <climits>
#include <filesystem>
#include <string>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ApiTypes.h"
#include "Log.h"
#include "LuaHost.h"
#include "Names.h"

// Tethys headers use unqualified min()/max() (they expect the <windows.h> macros). We build with
// NOMINMAX (sol2/STL require it), so pull std::min/std::max into the global namespace instead.
using std::max;
using std::min;

#include "Tethys/API/Game.h"            // Game::CreateUnit / CreateMine / CreateWall / CreateMarker
#include "Tethys/API/GameMap.h"         // GameMap::At (status-bar/editor coords -> internal)
#include "Tethys/API/Player.h"          // _Player
#include "Tethys/API/Location.h"        // Location
#include "Tethys/Game/MapObjectType.h"  // MapID
#include "Tethys/Game/MineManager.h"    // OreYield, OreVariant
#include "Tethys/Game/PlayerImpl.h"     // PlayerColor

namespace fs = std::filesystem;

using Tethys::MapID;
using Tethys::OreVariant;
using Tethys::OreYield;
using Tethys::PlayerColor;
using namespace Tethys::TethysAPI;  // Game, _Player, Location, Unit, MineType, UnitDirection, MarkerType

namespace OP2Lua::Placement {
namespace {

// Parsed placement table (valid while the Lua state lives). Applied once per load.
sol::table gPlacement;
bool gApplied = false;

// ------- name -> enum helpers ----------------------------------------------------------------

MapID MapIdFromName(const std::string& name) {
  static const std::unordered_map<std::string, MapID> kMap = {
      { "CargoTruck", MapID::CargoTruck },
      { "ConVec", MapID::ConVec },
      { "Spider", MapID::Spider },
      { "Scorpion", MapID::Scorpion },
      { "Lynx", MapID::Lynx },
      { "Panther", MapID::Panther },
      { "Tiger", MapID::Tiger },
      { "RoboSurveyor", MapID::RoboSurveyor },
      { "RoboMiner", MapID::RoboMiner },
      { "GeoCon", MapID::GeoCon },
      { "Scout", MapID::Scout },
      { "RoboDozer", MapID::RoboDozer },
      { "EvacuationTransport", MapID::EvacuationTransport },
      { "RepairVehicle", MapID::RepairVehicle },
      { "Earthworker", MapID::Earthworker },
      { "SmallCapacityAirTransport", MapID::SmallCapacityAirTransport },
      { "Tube", MapID::Tube },
      { "Wall", MapID::Wall },
      { "LavaWall", MapID::LavaWall },
      { "MicrobeWall", MapID::MicrobeWall },
      { "CommonOreMine", MapID::CommonOreMine },
      { "RareOreMine", MapID::RareOreMine },
      { "GuardPost", MapID::GuardPost },
      { "LightTower", MapID::LightTower },
      { "CommonStorage", MapID::CommonStorage },
      { "RareStorage", MapID::RareStorage },
      { "Forum", MapID::Forum },
      { "CommandCenter", MapID::CommandCenter },
      { "MHDGenerator", MapID::MHDGenerator },
      { "Residence", MapID::Residence },
      { "RobotCommand", MapID::RobotCommand },
      { "TradeCenter", MapID::TradeCenter },
      { "BasicLab", MapID::BasicLab },
      { "MedicalCenter", MapID::MedicalCenter },
      { "Nursery", MapID::Nursery },
      { "SolarPowerArray", MapID::SolarPowerArray },
      { "RecreationFacility", MapID::RecreationFacility },
      { "University", MapID::University },
      { "Agridome", MapID::Agridome },
      { "DIRT", MapID::DIRT },
      { "Garage", MapID::Garage },
      { "MagmaWell", MapID::MagmaWell },
      { "MeteorDefense", MapID::MeteorDefense },
      { "GeothermalPlant", MapID::GeothermalPlant },
      { "ArachnidFactory", MapID::ArachnidFactory },
      { "ConsumerFactory", MapID::ConsumerFactory },
      { "StructureFactory", MapID::StructureFactory },
      { "VehicleFactory", MapID::VehicleFactory },
      { "StandardLab", MapID::StandardLab },
      { "AdvancedLab", MapID::AdvancedLab },
      { "Observatory", MapID::Observatory },
      { "ReinforcedResidence", MapID::ReinforcedResidence },
      { "AdvancedResidence", MapID::AdvancedResidence },
      { "CommonOreSmelter", MapID::CommonOreSmelter },
      { "Spaceport", MapID::Spaceport },
      { "RareOreSmelter", MapID::RareOreSmelter },
      { "GORF", MapID::GORF },
      { "Tokamak", MapID::Tokamak },
      { "AcidCloud", MapID::AcidCloud },
      { "EMP", MapID::EMP },
      { "Laser", MapID::Laser },
      { "Microwave", MapID::Microwave },
      { "RailGun", MapID::RailGun },
      { "RPG", MapID::RPG },
      { "Starflare", MapID::Starflare },
      { "Supernova", MapID::Supernova },
      { "ESG", MapID::ESG },
      { "Stickyfoam", MapID::Stickyfoam },
      { "ThorsHammer", MapID::ThorsHammer },
      { "EnergyCannon", MapID::EnergyCannon },
      { "Wreckage", MapID::Wreckage },
  };
  const auto it = kMap.find(name);
  return (it != kMap.end()) ? it->second : MapID::None;
}

PlayerColor ColorFromName(const std::string& name) {
  static const std::unordered_map<std::string, int> kMap = {
      { "Blue", 0 }, { "Red", 1 }, { "Green", 2 }, { "Yellow", 3 },
      { "Cyan", 4 }, { "Magenta", 5 }, { "Black", 6 }, { "White", 7 },
  };
  const auto it = kMap.find(name);
  return static_cast<PlayerColor>((it != kMap.end()) ? it->second : 0);
}

OreYield YieldFromName(const std::string& name) {
  if (name == "Bar3") return OreYield::Bar3;
  if (name == "Bar2") return OreYield::Bar2;
  if (name == "Bar1") return OreYield::Bar1;
  return OreYield::Random;
}

OreVariant VariantFromName(const std::string& name) {
  if (name == "Variant1" || name == "High") return OreVariant::High;
  if (name == "Variant2" || name == "Mid")  return OreVariant::Mid;
  if (name == "Variant3" || name == "Low")  return OreVariant::Low;
  return OreVariant::Random;
}

// Beacon kind + ore type -> MineType used by Game::CreateMine.
MineType MineTypeFor(const std::string& beaconType, const std::string& oreType) {
  if (beaconType == "Fumarole")  return MineType::Fumarole;
  if (beaconType == "MagmaVent") return MineType::MagmaVent;
  if (oreType == "Common") return MineType::CommonOre;
  if (oreType == "Rare")   return MineType::RareOre;
  return MineType::RandomOre;
}

UnitDirection DirFromName(const std::string& name) {
  static const std::unordered_map<std::string, int> kMap = {
      { "East", 0 }, { "SouthEast", 1 }, { "South", 2 }, { "SouthWest", 3 },
      { "West", 4 }, { "NorthWest", 5 }, { "North", 6 }, { "NorthEast", 7 },
  };
  const auto it = kMap.find(name);
  return static_cast<UnitDirection>((it != kMap.end()) ? it->second : 0);
}

MarkerType MarkerTypeFromName(const std::string& name) {
  if (name == "DNA")    return MarkerType::DNA;
  if (name == "Beaker") return MarkerType::Beaker;
  return MarkerType::Circle;
}

// ------- table helpers -----------------------------------------------------------------------

template <typename T>
T Field(const sol::table& t, const char* key, T dflt) {
  sol::optional<T> v = t[key];
  return v.value_or(dflt);
}

// {x, y} (array) -> Location. Coords are author-facing (editor / status-bar) coordinates,
// translated to OP2's internal padded grid by GameMap::At (see GameMap.h).
Location LocFromTable(const sol::table& t) {
  return GameMap::At(t.get_or(1, 0), t.get_or(2, 0));
}

// Build a Region from a placement region array ({x,y} point or {x1,y1,x2,y2} rect). Corners are
// converted from editor coords to internal, then normalized so x1<=x2 / y1<=y2 for range tests.
Region RegionFromTable(const sol::table& r) {
  int ex1, ey1, ex2, ey2;
  if (r.size() >= 4) {
    ex1 = r.get_or(1, 0); ey1 = r.get_or(2, 0); ex2 = r.get_or(3, 0); ey2 = r.get_or(4, 0);
  } else {
    ex1 = ex2 = r.get_or(1, 0); ey1 = ey2 = r.get_or(2, 0);
  }
  const Location a = GameMap::At(ex1, ey1), b = GameMap::At(ex2, ey2);
  Region reg;
  reg.x1 = std::min(a.x, b.x); reg.x2 = std::max(a.x, b.x);
  reg.y1 = std::min(a.y, b.y); reg.y2 = std::max(a.y, b.y);
  return reg;
}

// ------- application -------------------------------------------------------------------------
//
// Note: the mission-list metadata (name/map/players/type) is NOT applied here - it lives in the
// per-mission stub DLL's DescBlock, baked from the .opm by the converter. placement.lua may still
// carry those fields for the converter's use; at runtime we only build the level from them.

void ApplyPlayers(sol::state& L, sol::table& luaPlayers) {
  sol::optional<sol::table> players = gPlacement["players"];
  if (!players) return;
  for (auto& kv : *players) {
    const int luaId = kv.first.as<int>();   // 1-based in the Lua API
    sol::table pd = kv.second.as<sol::table>();
    _Player p(luaId - 1);                    // OP2 players are 0-based

    if (Field<std::string>(pd, "colony", "Eden") == "Plymouth") p.GoPlymouth(); else p.GoEden();
    if (Field<bool>(pd, "human", true)) p.GoHuman(); else p.GoAI();

    sol::optional<std::string> color = pd["color"];
    if (color) p.SetColor(ColorFromName(*color));

    sol::optional<sol::table> res = pd["resources"];
    if (res) {
      p.SetCommonOre (Field<int>(*res, "common_ore", 0));
      p.SetRareOre   (Field<int>(*res, "rare_ore", 0));
      p.SetFoodStored(Field<int>(*res, "food", 0));
      p.SetKids      (Field<int>(*res, "kids", 0));
      p.SetWorkers   (Field<int>(*res, "workers", 0));
      p.SetScientists(Field<int>(*res, "scientists", 0));
      const int sat = Field<int>(*res, "solar_satellites", 0);
      if (sat > 0) p.SetSolarSat(sat);
      const int techLevel = Field<int>(*res, "tech_level", 0);
      if (techLevel > 0) p.SetTechLevel(techLevel);
      sol::optional<sol::table> research = (*res)["completed_research"];
      if (research) for (auto& r : *research) p.MarkResearchComplete(r.second.as<int>());
    }

    sol::optional<sol::table> centerView = pd["center_view"];
    if (centerView) p.CenterViewOn(LocFromTable(*centerView));

    sol::optional<sol::table> allies = pd["allies"];
    if (allies) for (auto& a : *allies) p.MutuallyAllyWith(a.second.as<int>() - 1);

    luaPlayers[luaId] = LuaPlayer{ luaId - 1 };  // register the player handle for mission.lua
  }
}

// Every player in the game gets a handle, listed in placement or not. A multiplayer mission leaves
// `players` out of placement.lua: the lobby has already chosen each player's colony, colour and
// human/AI, and ApplyPlayers would override them.
void RegisterAllPlayers(sol::table& luaPlayers) {
  for (int num = 0; num < Game::NumPlayers(); ++num) {
    if (!luaPlayers[num + 1].valid()) luaPlayers[num + 1] = LuaPlayer{ num };
  }
}

void ApplyBeacons(sol::table& luaUnits, int fromAfter, int toAfter);

// Each unit in order; after the n-th, the beacons with `after = n`.
void ApplyUnits(sol::table& luaUnits) {
  sol::optional<sol::table> units = gPlacement["units"];
  int made = 0;
  if (units) for (auto& kv : *units) {
    if (made > 0) ApplyBeacons(luaUnits, made, made);
    ++made;
    sol::table u = kv.second.as<sol::table>();
    const std::string typeName = Field<std::string>(u, "type", "");
    const MapID type = MapIdFromName(typeName);
    if (type == MapID::None) {
      LOG(WARNING) << "placement: unknown unit type '" << typeName << "' (skipped)";
      continue;
    }
    const int owner = Field<int>(u, "player", 1) - 1;
    sol::optional<sol::table> at = u["at"];
    const Location loc = at ? LocFromTable(*at) : Location();

    MapID weapon = MapID::None;
    sol::optional<std::string> weaponName = u["weapon"];
    if (weaponName) weapon = MapIdFromName(*weaponName);

    const UnitDirection dir = DirFromName(Field<std::string>(u, "direction", "East"));
    const bool lights = Field<bool>(u, "lights", true);

    Unit unit = Game::CreateUnit(type, loc, owner, weapon, dir, lights);

    const double health = Field<double>(u, "health", 1.0);
    if (health < 1.0 && unit.IsLive()) {
      const int maxHp = unit.GetMaxHitpoints();
      unit.SetDamage(static_cast<int>((1.0 - health) * maxHp));
    }

    // Cargo Truck contents:  cargo = "RareMetal" (or Food/CommonOre/...), amount = 1000 (default).
    sol::optional<std::string> cargoName = u["cargo"];
    if (cargoName && unit.IsLive()) {
      const int ct = CargoTypeFromName(*cargoName);
      if (ct > 0) unit.SetCargo(static_cast<Tethys::CargoType>(ct), Field<int>(u, "amount", 1000));
      else        LOG(WARNING) << "placement: unknown cargo '" << *cargoName << "' (ignored)";
    }

    // ConVec structure kit:  kit = "Agridome" (a structure map_id), plus an optional kit_topping for
    // the kit's payload (e.g. a Guard Post kit's weapon). This is the ConVec equivalent of `cargo`:
    // a ConVec carries a kit, a Cargo Truck carries cargo. Mirrors unit:set_kit() / unit.kit.
    sol::optional<std::string> kitName = u["kit"];
    if (kitName && unit.IsLive()) {
      const MapID kit = MapIdFromName(*kitName);
      if (kit != MapID::None) {
        sol::optional<std::string> topping = u["kit_topping"];
        unit.SetCargo(kit, topping ? MapIdFromName(*topping) : MapID::None);
      } else {
        LOG(WARNING) << "placement: unknown kit '" << *kitName << "' (ignored)";
      }
    }

    sol::optional<std::string> name = u["name"];
    if (name) luaUnits[*name] = unit;
  }
  ApplyBeacons(luaUnits, std::max(made, 1), INT_MAX);
}

// The beacons whose `after` (how many of the placement's units are made before it; 0 when left out) is in
// fromAfter..toAfter. A setup that makes a beacon between its units (Tutorial 3, between its Tokamak and the mine on
// it) gets the same object ids so.
void ApplyBeacons(sol::table& luaUnits, int fromAfter, int toAfter) {
  sol::optional<sol::table> beacons = gPlacement["beacons"];
  if (!beacons) return;
  for (auto& kv : *beacons) {
    sol::table b = kv.second.as<sol::table>();
    const int after = Field<int>(b, "after", 0);
    if (after < fromAfter || after > toAfter) continue;
    const MineType   mt = MineTypeFor(Field<std::string>(b, "type", "MiningBeacon"),
                                      Field<std::string>(b, "ore", "Random"));
    const OreYield   y  = YieldFromName(Field<std::string>(b, "yield", "Random"));
    const OreVariant v  = VariantFromName(Field<std::string>(b, "variant", "Random"));
    sol::optional<sol::table> at = b["at"];
    const Location loc = at ? LocFromTable(*at) : Location();
    Unit u = Game::CreateMine(loc, mt, y, v);
    sol::optional<std::string> name = b["name"];
    if (name) luaUnits[*name] = u;
  }
}

void ApplyWalls() {
  sol::optional<sol::table> walls = gPlacement["walls"];
  if (!walls) return;
  for (auto& kv : *walls) {
    sol::table w = kv.second.as<sol::table>();
    const MapID type = MapIdFromName(Field<std::string>(w, "type", "Wall"));
    sol::optional<sol::table> at = w["at"];
    if (at && type != MapID::None) Game::CreateWall(type, LocFromTable(*at));
  }
}

void ApplyRegions(sol::state& L, sol::table& luaRegions) {
  sol::optional<sol::table> regions = gPlacement["regions"];
  if (!regions) return;
  for (auto& kv : *regions) {
    luaRegions[kv.first.as<std::string>()] = RegionFromTable(kv.second.as<sol::table>());
  }
}

void ApplyMarkers(sol::state& L, sol::table& luaMarkers) {
  sol::optional<sol::table> markers = gPlacement["markers"];
  if (!markers) return;
  for (auto& kv : *markers) {
    sol::table m = kv.second.as<sol::table>();
    sol::optional<sol::table> at = m["at"];
    const Location loc = at ? LocFromTable(*at) : Location();
    Game::CreateMarker(loc, MarkerTypeFromName(Field<std::string>(m, "type", "Circle")));
    sol::table out = L.create_table();
    // loc is in OP2 engine-internal (padded-grid) coordinates, published as-is on
    // purpose: the engine requires the internal offset, same convention as the C++
    // mission SDK. Placement in-game is correct.
    out["x"] = loc.x; out["y"] = loc.y;
    luaMarkers[kv.first.as<std::string>()] = out;
  }
}

}  // namespace

bool Load(const std::string& placementPath) {
  Reset();

  std::error_code ec;
  if (!fs::exists(placementPath, ec)) {
    LOG(WARNING) << "No placement file: " << placementPath << " (script-only mission)";
    return false;
  }

  sol::state& L = LuaHost::Get().lua();
  sol::protected_function_result r =
      L.safe_script_file(placementPath, &sol::script_pass_on_error);
  if (!r.valid()) {
    sol::error e = r;
    const std::string msg = std::string("Error in placement.lua:\n") + e.what();
    LOG(ERROR) << msg;
    MessageBoxA(nullptr, msg.c_str(), "OP2Lua", MB_ICONEXCLAMATION | MB_SYSTEMMODAL);
    return false;
  }

  sol::object result = r.get<sol::object>(0);
  if (!result.is<sol::table>()) {
    LOG(ERROR) << "placement.lua must `return { ... }` a table";
    return false;
  }

  gPlacement = result.as<sol::table>();
  LOG(INFO) << "Loaded placement: " << placementPath;
  return true;
}

std::string DeclaredMap() {
  if (!gPlacement.valid()) return std::string();
  sol::optional<std::string> m = gPlacement["map"];
  return m.value_or(std::string());
}

void Apply() {
  if (gApplied) return;
  if (!gPlacement.valid()) {
    // A script-only mission: no layout to build, but mission.lua still needs players[].
    sol::state& L = LuaHost::Get().lua();
    sol::table luaPlayers = L.create_table();
    RegisterAllPlayers(luaPlayers);
    L["players"] = luaPlayers;
    gApplied = true;
    return;
  }
  sol::state& L = LuaHost::Get().lua();

  sol::table luaPlayers = L.create_table();
  sol::table luaUnits   = L.create_table();
  sol::table luaRegions = L.create_table();
  sol::table luaMarkers = L.create_table();

  ApplyPlayers(L, luaPlayers);
  RegisterAllPlayers(luaPlayers);
  ApplyBeacons(luaUnits, 0, 0);   // beacons are terrain - create them BEFORE units, so a mine building
  ApplyUnits(luaUnits);           // placed on a beacon finds it (else CreateUnit faults on the missing beacon)
  ApplyWalls();
  ApplyRegions(L, luaRegions);
  ApplyMarkers(L, luaMarkers);

  // Publish the named handles mission.lua refers to.
  L["players"] = luaPlayers;
  L["units"]   = luaUnits;
  L["regions"] = luaRegions;
  L["markers"] = luaMarkers;

  gApplied = true;
  LOG(INFO) << "Placement applied to the running game";
}

void Reset() {
  gPlacement = sol::table();
  gApplied = false;
}

}  // namespace OP2Lua::Placement
