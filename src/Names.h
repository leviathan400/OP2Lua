#pragma once

#include <string>

#include "Tethys/Game/MapObjectType.h"  // Tethys::MapID

namespace OP2Lua {

// Map a type name (e.g. "CommandCenter", "Tiger", "Microwave") to its MapID; MapID::None if unknown.
Tethys::MapID MapIdFromName(const std::string& name);

// Reverse: MapID -> type name (e.g. for unit.type). Empty string if unknown.
std::string NameFromMapId(Tethys::MapID id);

// Direction name ("East".."NorthEast") -> UnitDirection integer 0..7 (default 0/East).
int DirFromName(const std::string& name);

// Sound name (e.g. "Savant4", "Beep2") -> SoundID integer; -1 if unknown.
int SoundIdFromName(const std::string& name);

// Cargo type name ("Food", "CommonOre", "RareMetal"...) -> CargoType integer; -1 if unknown.
int CargoTypeFromName(const std::string& name);

// Reverse: CargoType integer -> name (e.g. for unit.cargo). "Empty" / "" if unknown.
std::string NameFromCargoType(int cargoType);

}  // namespace OP2Lua
