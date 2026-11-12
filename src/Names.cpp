// Names.cpp - lookup tables between author-facing names and engine ids:
// type names <-> MapID, direction names -> UnitDirection, and sound names -> SoundID.

#include "Names.h"

#include <unordered_map>
#include <utility>
#include <vector>

using Tethys::MapID;

namespace OP2Lua {
namespace {

// Single source of truth for type names (used for both name->MapID and MapID->name).
const std::vector<std::pair<const char*, MapID>>& MapIdTable() {
  static const std::vector<std::pair<const char*, MapID>> kTable = {
      { "CargoTruck", MapID::CargoTruck }, { "ConVec", MapID::ConVec },
      { "Spider", MapID::Spider }, { "Scorpion", MapID::Scorpion },
      { "Lynx", MapID::Lynx }, { "Panther", MapID::Panther }, { "Tiger", MapID::Tiger },
      { "RoboSurveyor", MapID::RoboSurveyor }, { "RoboMiner", MapID::RoboMiner },
      { "GeoCon", MapID::GeoCon }, { "Scout", MapID::Scout }, { "RoboDozer", MapID::RoboDozer },
      { "EvacuationTransport", MapID::EvacuationTransport }, { "RepairVehicle", MapID::RepairVehicle },
      { "Earthworker", MapID::Earthworker },
      { "SmallCapacityAirTransport", MapID::SmallCapacityAirTransport },
      { "Tube", MapID::Tube }, { "Wall", MapID::Wall }, { "LavaWall", MapID::LavaWall },
      { "MicrobeWall", MapID::MicrobeWall },
      { "CommonOreMine", MapID::CommonOreMine }, { "RareOreMine", MapID::RareOreMine },
      { "GuardPost", MapID::GuardPost }, { "LightTower", MapID::LightTower },
      { "CommonStorage", MapID::CommonStorage }, { "RareStorage", MapID::RareStorage },
      { "Forum", MapID::Forum }, { "CommandCenter", MapID::CommandCenter },
      { "MHDGenerator", MapID::MHDGenerator }, { "Residence", MapID::Residence },
      { "RobotCommand", MapID::RobotCommand }, { "TradeCenter", MapID::TradeCenter },
      { "BasicLab", MapID::BasicLab }, { "MedicalCenter", MapID::MedicalCenter },
      { "Nursery", MapID::Nursery }, { "SolarPowerArray", MapID::SolarPowerArray },
      { "RecreationFacility", MapID::RecreationFacility }, { "University", MapID::University },
      { "Agridome", MapID::Agridome }, { "DIRT", MapID::DIRT }, { "Garage", MapID::Garage },
      { "MagmaWell", MapID::MagmaWell }, { "MeteorDefense", MapID::MeteorDefense },
      { "MiningBeacon", MapID::MiningBeacon }, { "MagmaVent", MapID::MagmaVent }, { "Fumarole", MapID::Fumarole },
      { "GeothermalPlant", MapID::GeothermalPlant }, { "ArachnidFactory", MapID::ArachnidFactory },
      { "ConsumerFactory", MapID::ConsumerFactory }, { "StructureFactory", MapID::StructureFactory },
      { "VehicleFactory", MapID::VehicleFactory }, { "StandardLab", MapID::StandardLab },
      { "AdvancedLab", MapID::AdvancedLab }, { "Observatory", MapID::Observatory },
      { "ReinforcedResidence", MapID::ReinforcedResidence },
      { "AdvancedResidence", MapID::AdvancedResidence },
      { "CommonOreSmelter", MapID::CommonOreSmelter }, { "Spaceport", MapID::Spaceport },
      { "RareOreSmelter", MapID::RareOreSmelter }, { "GORF", MapID::GORF }, { "Tokamak", MapID::Tokamak },
      { "AcidCloud", MapID::AcidCloud }, { "EMP", MapID::EMP }, { "Laser", MapID::Laser },
      { "Microwave", MapID::Microwave }, { "RailGun", MapID::RailGun }, { "RPG", MapID::RPG },
      { "Starflare", MapID::Starflare }, { "Supernova", MapID::Supernova },
      { "ESG", MapID::ESG }, { "Stickyfoam", MapID::Stickyfoam }, { "ThorsHammer", MapID::ThorsHammer },
      { "Wreckage", MapID::Wreckage },
      // A Spaceport's EMP missile ('EMP_MISSILE'), for unit:produce on a Spaceport and unit:launch.
      { "EMPMissile", MapID::EMPMissile },
      // Starship / satellites + Spaceport cargo rockets (for the Starship colony victory).
      { "EDWARDSatellite", MapID::EDWARDSatellite }, { "SolarSatellite", MapID::SolarSatellite },
      { "IonDriveModule", MapID::IonDriveModule }, { "FusionDriveModule", MapID::FusionDriveModule },
      { "CommandModule", MapID::CommandModule }, { "FuelingSystems", MapID::FuelingSystems },
      { "HabitatRing", MapID::HabitatRing }, { "SensorPackage", MapID::SensorPackage },
      { "Skydock", MapID::Skydock }, { "StasisSystems", MapID::StasisSystems },
      { "OrbitalPackage", MapID::OrbitalPackage }, { "PhoenixModule", MapID::PhoenixModule },
      { "RareMetalsCargo", MapID::RareMetalsCargo }, { "CommonMetalsCargo", MapID::CommonMetalsCargo },
      { "FoodCargo", MapID::FoodCargo }, { "EvacuationModule", MapID::EvacuationModule },
      { "ChildrenModule", MapID::ChildrenModule },
  };
  return kTable;
}

}  // namespace

MapID MapIdFromName(const std::string& name) {
  static const std::unordered_map<std::string, MapID> kByName = [] {
    std::unordered_map<std::string, MapID> m;
    for (const auto& [n, id] : MapIdTable()) m.emplace(n, id);
    return m;
  }();
  const auto it = kByName.find(name);
  return (it != kByName.end()) ? it->second : MapID::None;
}

std::string NameFromMapId(MapID id) {
  static const std::unordered_map<int, std::string> kById = [] {
    std::unordered_map<int, std::string> m;
    for (const auto& [n, mid] : MapIdTable()) m.emplace(static_cast<int>(mid), n);
    return m;
  }();
  const auto it = kById.find(static_cast<int>(id));
  return (it != kById.end()) ? it->second : std::string();
}

int DirFromName(const std::string& name) {
  static const std::unordered_map<std::string, int> kMap = {
      { "East", 0 }, { "SouthEast", 1 }, { "South", 2 }, { "SouthWest", 3 },
      { "West", 4 }, { "NorthWest", 5 }, { "North", 6 }, { "NorthEast", 7 },
  };
  const auto it = kMap.find(name);
  return (it != kMap.end()) ? it->second : 0;
}

int SoundIdFromName(const std::string& name) {
  // Curated set: the Savant voices (most useful for missions) + UI beeps + a couple of effects.
  // Authors can also pass a raw integer SoundID for anything not listed here.
  static const std::unordered_map<std::string, int> kMap = {
      { "Beep2", 85 }, { "Beep5", 87 }, { "Beep6", 89 }, { "Beep8", 91 }, { "Beep9", 90 },
      { "Message2", 93 }, { "BuildingExplosion", 57 },
      // Savant voice lines (value -> spoken line):
      { "StructureCompleted", 94 }, { "ResearchCompleted", 95 }, { "ColonyUnderAttack", 96 },
      { "WeAreUnderAttack", 97 }, { "VehicleReady", 98 }, { "ConsumerItemsCompleted", 99 },
      { "StructureKitManufactured", 100 }, { "SpiderReady", 101 }, { "ScorpionReady", 102 },
      { "StructureDestroyed", 103 }, { "VehicleDestroyed", 104 }, { "DatabaseReconstructed", 105 },
      { "EnemyUnitSighted", 106 }, { "VehicleRepaired", 107 }, { "StructureRepaired", 108 },
      { "MissileLaunchDetected", 109 }, { "IncomingMissile", 110 },
      { "MoraleExcellent", 111 }, { "MoraleGood", 112 }, { "MoraleFair", 113 },
      { "MoralePoor", 114 }, { "MoraleTerrible", 115 },
      { "FoodSurplus", 116 }, { "FoodDiminishing", 117 }, { "PeopleStarving", 118 },
      { "PowerOptimal", 119 }, { "PowerMarginal", 120 }, { "PowerShortage", 121 },
      { "NewMissionObjective", 122 }, { "EdwardDeployed", 123 }, { "SolarSatelliteDeployed", 124 },
      { "RocketLaunched", 125 }, { "RLVLaunched", 126 },
      // Raw Savant ids by their enum name too (so "Savant4" etc. work):
      { "Savant2", 95 }, { "Savant3", 96 }, { "Savant4", 97 }, { "Savant6", 98 }, { "Savant7", 99 },
      { "Savant8", 100 }, { "Savant9", 101 }, { "Savant10", 102 }, { "Savant11", 103 },
      { "Savant13", 105 }, { "Savant14", 106 }, { "Savant15", 107 }, { "Savant17", 109 },
      { "Savant18", 110 },
  };
  const auto it = kMap.find(name);
  return (it != kMap.end()) ? it->second : -1;
}

// CargoType enum (Tethys/Game/MapObject.h): Empty=0, Food=1, CommonOre=2, RareOre=3,
// CommonMetal=4, RareMetal=5, CommonRubble=6, RareRubble=7, Spacecraft=8, Wreckage=9, GeneBank=10.
static const std::vector<std::pair<std::string, int>> kCargoTable = {
    { "Empty", 0 }, { "Food", 1 }, { "CommonOre", 2 }, { "RareOre", 3 },
    { "CommonMetal", 4 }, { "RareMetal", 5 }, { "CommonRubble", 6 }, { "RareRubble", 7 },
    { "Spacecraft", 8 }, { "Wreckage", 9 }, { "GeneBank", 10 },
    // Friendly aliases:
    { "Metal", 4 }, { "Ore", 2 }, { "Rubble", 6 },
};

int CargoTypeFromName(const std::string& name) {
  for (const auto& [n, v] : kCargoTable) if (n == name) return v;
  return -1;
}

std::string NameFromCargoType(int cargoType) {
  for (const auto& [n, v] : kCargoTable) if (v == cargoType) return n;
  return (cargoType == 0) ? "Empty" : "";
}

}  // namespace OP2Lua
