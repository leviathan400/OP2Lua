#pragma once

#include <string>

// Phase 2 - the placement loader.
//
// Reads the sibling `placement.lua` data table (generated from a .opm), fills the mission-list
// metadata (DescBlock etc.), and builds the level at mission start: players, starting units,
// resources, beacons, walls. It also registers the named Lua handles (units[name], regions[name],
// markers[name], players[id]) that mission.lua refers to.

namespace OP2Lua::Placement {

// Load a placement.lua file (full path) and store its data table.
// Returns false (and logs) if the file is missing or fails to parse. (Mission-list metadata is
// NOT set here - that's the stub DLL's DescBlock, baked by the converter.)
bool Load(const std::string& placementPath);

// Apply the stored placement to the running game (call from InitProc, before on_init):
// configure players, create units/beacons/walls, and register the named Lua handles.
// Safe to call repeatedly; only applies once per loaded placement.
void Apply();

// Drop the stored placement data (call when unloading a mission / shutting down).
void Reset();

// The map filename declared in placement.lua's `map=` field ("" if none / not loaded).
// This is author metadata only - it does NOT drive map loading (OP2 reads MapName from the stub
// DLL). Exposed so the loader can cross-check it against the baked DLL value and warn on drift.
std::string DeclaredMap();

}  // namespace OP2Lua::Placement
