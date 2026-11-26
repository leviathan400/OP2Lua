// op2lua-newmission - make a new OP2Lua mission, or rebuild a mission's DLL from its placement (cross-platform console
// tool).
//
// OP2 reads a mission's name (LevelDesc), map (MapName), tech tree (TechtreeName), type, player count, maximum tech
// level and unit-only flag (DescBlock) and a multiplayer game's AI players (DescBlockEx) from its DLL before any Lua
// runs. The tool copies the prebuilt stub (LuaMission.dll) and writes each value at its export, as OP2LuaEditor's
// Save does (the same bytes), then writes starter <Mission>.lua + <Mission>.placement.lua next to it. With
// --dll-from it builds the DLL of an existing mission from its placement's header instead.
//
// Pure C++17 (<filesystem> + <fstream>); builds and runs on Windows, Linux, and macOS.
//
// Usage:
//   op2lua-newmission "My First Mission"
//   op2lua-newmission "Red Dawn" --map sgame01.map --tech MULTITEK.TXT --max-tech 8 --out ./missions
//   op2lua-newmission "Duel" --players 4 --type LastOneStanding --ai-players 1
//   op2lua-newmission --dll-from maps/q01Lua.placement.lua

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// The size of the LevelDesc, MapName and TechtreeName exports (char[1024] in stub/LuaMission.cpp).
constexpr std::size_t kTextExportLen = 1024;

// FileDescription placeholder in the stub's version resource (stored as UTF-16). Must match the text in
// stub/LuaMission.rc byte-for-byte. It is set to the mission name so the DLL's Windows Properties show it.
constexpr const char* kDescPlaceholder = "OP2Lua mission name placeholder - set by the new-mission tool";

// What a mission DLL tells the game, as the placement's header writes it. `type` is a placement type name
// (Colony, MultiLastOneStanding, ...) or a campaign mission's number ("1").
struct Props {
  std::string name;
  std::string map  = "eden01.map";
  std::string tech = "MULTITEK.TXT";
  std::string type = "Colony";
  int  maxTech   = 12;
  int  players   = 2;
  bool unitOnly  = false;
  int  aiPlayers = 0;
};

using Bytes = std::vector<std::uint8_t>;

Bytes readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool writeFile(const fs::path& p, const Bytes& bytes) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(f);
}

bool writeText(const fs::path& p, const std::string& text) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);  // binary => exact bytes, LF preserved
  if (!f) return false;
  f << text;
  return static_cast<bool>(f);
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// ---- mission types ---------------------------------------------------------------------------------------------

// A type name -> the DescBlock's MissionType: the placement's names (MultiLastOneStanding, as op2dll and the editors
// write them), the short multiplayer names (LastOneStanding), and a campaign mission's number (1 to 12). 0 = none.
int missionTypeFrom(const std::string& name) {
  const std::string n = lower(name);
  if (!n.empty() && std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isdigit(c); }) && n.size() <= 2) {
    const int v = std::stoi(n);
    return (v >= 1 && v <= 12) ? v : 0;
  }
  if (n == "colony") return -1;
  if (n == "autodemo") return -2;
  if (n == "tutorial") return -3;
  if (n == "multilandrush" || n == "landrush") return -4;
  if (n == "multispacerace" || n == "spacerace") return -5;
  if (n == "multiresourcerace" || n == "resourcerace") return -6;
  if (n == "multimidas" || n == "midas") return -7;
  if (n == "multilastonestanding" || n == "lastonestanding") return -8;
  return 0;
}

// The placement name of a DescBlock MissionType ("1" for a campaign mission), or "" for none.
std::string missionTypeName(int v) {
  if (v >= 1 && v <= 12) return std::to_string(v);
  switch (v) {
    case -1: return "Colony";
    case -2: return "AutoDemo";
    case -3: return "Tutorial";
    case -4: return "MultiLandRush";
    case -5: return "MultiSpaceRace";
    case -6: return "MultiResourceRace";
    case -7: return "MultiMidas";
    case -8: return "MultiLastOneStanding";
    default: return "";
  }
}

bool isMultiplayer(const std::string& type) { return missionTypeFrom(type) <= -4 && missionTypeFrom(type) != 0; }

// ---- the DLL's exports -------------------------------------------------------------------------------------------

std::optional<std::uint32_t> u32At(const Bytes& b, std::size_t i) {
  if (i + 4 > b.size()) return std::nullopt;
  return std::uint32_t(b[i]) | std::uint32_t(b[i + 1]) << 8 | std::uint32_t(b[i + 2]) << 16 | std::uint32_t(b[i + 3]) << 24;
}
std::optional<std::uint16_t> u16At(const Bytes& b, std::size_t i) {
  if (i + 2 > b.size()) return std::nullopt;
  return static_cast<std::uint16_t>(b[i] | b[i + 1] << 8);
}

// A named export's file offset and how many bytes of its section's file data follow it, if the DLL has it.
struct Span { std::size_t at, room; };
std::optional<Span> exportSpan(const Bytes& b, const std::string& name) {
  const auto pe = u32At(b, 0x3C);
  if (!pe || u32At(b, *pe) != 0x4550u) return std::nullopt;  // "PE\0\0"
  const auto nSec = u16At(b, *pe + 6), optSize = u16At(b, *pe + 20), magic = u16At(b, *pe + 24);
  if (!nSec || !optSize || !magic) return std::nullopt;
  const std::size_t dataDirs = *pe + 24 + (*magic == 0x20B ? 112 : 96);
  const std::size_t sections = *pe + 24 + *optSize;
  auto toOffset = [&](std::uint32_t rva) -> std::optional<Span> {
    for (std::size_t s = 0; s < *nSec; ++s) {
      const std::size_t sec = sections + s * 40;
      const auto vsize = u32At(b, sec + 8), va = u32At(b, sec + 12), rawSize = u32At(b, sec + 16), rawPtr = u32At(b, sec + 20);
      if (!vsize || !va || !rawSize || !rawPtr) return std::nullopt;
      if (rva >= *va && rva < *va + std::max(*vsize, *rawSize) && rva - *va < *rawSize) {
        const std::size_t at = rva - *va + *rawPtr;
        if (at >= b.size()) return std::nullopt;
        return Span{at, std::min<std::size_t>(*rawSize - (rva - *va), b.size() - at)};
      }
    }
    return std::nullopt;
  };
  const auto expRva = u32At(b, dataDirs);
  if (!expRva) return std::nullopt;
  const auto exp = toOffset(*expRva);
  if (!exp) return std::nullopt;
  const auto nNames = u32At(b, exp->at + 24);
  const auto funcsR = u32At(b, exp->at + 28), namesR = u32At(b, exp->at + 32), ordsR = u32At(b, exp->at + 36);
  if (!nNames || !funcsR || !namesR || !ordsR) return std::nullopt;
  const auto funcs = toOffset(*funcsR), names = toOffset(*namesR), ords = toOffset(*ordsR);
  if (!funcs || !names || !ords) return std::nullopt;
  for (std::uint32_t i = 0; i < *nNames; ++i) {
    const auto nameR = u32At(b, names->at + 4 * i);
    if (!nameR) return std::nullopt;
    const auto n = toOffset(*nameR);
    if (!n) continue;
    std::size_t e = n->at;
    while (e < b.size() && b[e] != 0) ++e;
    if (std::string(b.begin() + static_cast<std::ptrdiff_t>(n->at), b.begin() + static_cast<std::ptrdiff_t>(e)) != name) continue;
    const auto ord = u16At(b, ords->at + 2 * i);
    if (!ord) return std::nullopt;
    const auto fnR = u32At(b, funcs->at + 4 * *ord);
    if (!fnR) return std::nullopt;
    return toOffset(*fnR);
  }
  return std::nullopt;
}

// Writes `value` into a char[1024] export, clearing the rest.
bool writeTextExport(Bytes& b, const std::string& name, const std::string& value) {
  const auto s = exportSpan(b, name);
  if (!s || s->room < kTextExportLen || value.size() >= kTextExportLen) return false;
  std::fill(b.begin() + static_cast<std::ptrdiff_t>(s->at), b.begin() + static_cast<std::ptrdiff_t>(s->at + kTextExportLen), 0);
  std::copy(value.begin(), value.end(), b.begin() + static_cast<std::ptrdiff_t>(s->at));
  return true;
}

// Writes little-endian ints at the start of an export.
bool writeIntExport(Bytes& b, const std::string& name, const std::vector<int>& values) {
  const auto s = exportSpan(b, name);
  if (!s || s->room < 4 * values.size()) return false;
  for (std::size_t k = 0; k < values.size(); ++k)
    for (int j = 0; j < 4; ++j) b[s->at + 4 * k + j] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(values[k]) >> (8 * j));
  return true;
}

std::optional<std::string> readTextExport(const Bytes& b, const std::string& name) {
  const auto s = exportSpan(b, name);
  if (!s) return std::nullopt;
  std::size_t e = s->at;
  while (e < s->at + std::min(s->room, kTextExportLen) && b[e] != 0) ++e;
  return std::string(b.begin() + static_cast<std::ptrdiff_t>(s->at), b.begin() + static_cast<std::ptrdiff_t>(e));
}

std::optional<int> readIntExport(const Bytes& b, const std::string& name, std::size_t index) {
  const auto s = exportSpan(b, name);
  if (!s || s->room < 4 * (index + 1)) return std::nullopt;
  return static_cast<int>(*u32At(b, s->at + 4 * index));
}

// What a mission DLL describes, if it is one.
std::optional<Props> readDll(const Bytes& b) {
  const auto t = readIntExport(b, "DescBlock", 0);
  const auto level = readTextExport(b, "LevelDesc"), map = readTextExport(b, "MapName"), tech = readTextExport(b, "TechtreeName");
  if (!t || !level || !map || !tech) return std::nullopt;
  Props p;
  p.name = *level;
  p.map = *map;
  p.tech = *tech;
  if (!missionTypeName(*t).empty()) p.type = missionTypeName(*t);
  p.players = readIntExport(b, "DescBlock", 1).value_or(0);
  p.maxTech = readIntExport(b, "DescBlock", 2).value_or(0);
  p.unitOnly = readIntExport(b, "DescBlock", 3).value_or(0) != 0;
  p.aiPlayers = readIntExport(b, "DescBlockEx", 0).value_or(0);
  return p;
}

// Overwrites a UTF-16 string in the version resource within its existing room: `placeholder` plus the run of NUL
// words after it, one kept for the terminator. False when it isn't there or `value` doesn't fit.
bool patchWideField(Bytes& b, const std::string& placeholder, const std::string& value) {
  Bytes needle;
  for (char c : placeholder) { needle.push_back(static_cast<std::uint8_t>(c)); needle.push_back(0); }
  auto it = std::search(b.begin(), b.end(), needle.begin(), needle.end());
  if (it == b.end()) return false;
  const std::size_t at = static_cast<std::size_t>(it - b.begin());
  std::size_t capWords = placeholder.size(), p = at + needle.size();
  while (p + 1 < b.size() && b[p] == 0 && b[p + 1] == 0) { ++capWords; p += 2; }
  if (value.size() > capWords - 1) return false;
  std::fill(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + capWords * 2), 0);
  for (std::size_t k = 0; k < value.size(); ++k) b[at + 2 * k] = static_cast<std::uint8_t>(value[k]);
  return true;
}

// Why the properties can't go into a DLL, or "".
std::string problem(const Props& p) {
  if (p.name.find_first_not_of(" \t") == std::string::npos) return "the name can't be empty";
  for (const std::string* v : {&p.name, &p.map, &p.tech}) {
    if (std::any_of(v->begin(), v->end(), [](unsigned char c) { return c > 127; }))
      return "the name, map and tech tree are stored in the DLL as plain ASCII";
    if (v->size() >= kTextExportLen) return "the name, map and tech tree must be under 1024 characters";
  }
  if (p.map.empty()) return "no map";
  if (p.tech.empty()) return "no tech tree file";
  if (missionTypeFrom(p.type) == 0) return "unknown type " + p.type;
  if (p.players < 1 || p.players > 6) return "players must be 1-6";
  if (p.maxTech < 0 || p.maxTech > 12) return "the max tech level must be 0-12";
  if (p.aiPlayers < 0 || p.aiPlayers > 5) return "AI players must be 0-5";
  return "";
}

// The mission DLL the properties describe: the stub with each value written at its export. Empty on failure
// (the reason printed).
Bytes buildDll(const Bytes& stub, const Props& p) {
  if (const std::string why = problem(p); !why.empty()) { std::cerr << "error: " << why << ".\n"; return {}; }
  Bytes b = stub;
  const bool ok = writeTextExport(b, "LevelDesc", p.name) && writeTextExport(b, "MapName", p.map) &&
                  writeTextExport(b, "TechtreeName", p.tech) &&
                  writeIntExport(b, "DescBlock", {missionTypeFrom(p.type), p.players, p.maxTech, p.unitOnly ? 1 : 0}) &&
                  writeIntExport(b, "DescBlockEx", {p.aiPlayers});
  if (!ok) { std::cerr << "error: the stub DLL lacks one of the mission exports (is it LuaMission.dll?).\n"; return {}; }
  if (!patchWideField(b, kDescPlaceholder, p.name))
    std::cerr << "note: the DLL's Windows file description keeps the stub's text (name too long, or no placeholder).\n";
  return b;
}

// ---- the placement's header -------------------------------------------------------------------------------------

std::regex keyRegex(const std::string& key, const std::string& value) {
  return std::regex("(^|[^A-Za-z0-9_])" + key + "\\s*=\\s*" + value);
}
std::optional<std::string> headerString(const std::string& text, const std::string& key) {
  std::smatch m;
  if (std::regex_search(text, m, keyRegex(key, "\"([^\"]*)\""))) return m[2].str();
  return std::nullopt;
}
std::optional<int> headerInt(const std::string& text, const std::string& key) {
  std::smatch m;
  if (std::regex_search(text, m, keyRegex(key, "(-?[0-9]+)"))) return std::stoi(m[2].str());
  return std::nullopt;
}
std::optional<bool> headerBool(const std::string& text, const std::string& key) {
  std::smatch m;
  if (std::regex_search(text, m, keyRegex(key, "(true|false)([^A-Za-z0-9_]|$)"))) return m[2].str() == "true";
  return std::nullopt;
}

// The properties a placement's text declares; the ones it doesn't are `d`'s.
Props readHeader(const std::string& text, const Props& d) {
  Props p;
  p.name = headerString(text, "name").value_or(d.name);
  p.map = headerString(text, "map").value_or(d.map);
  p.tech = headerString(text, "tech").value_or(d.tech);
  p.type = headerString(text, "type").value_or(d.type);
  p.maxTech = headerInt(text, "max_tech").value_or(d.maxTech);
  p.players = headerInt(text, "players_count").value_or(d.players);
  p.unitOnly = headerBool(text, "unit_only").value_or(d.unitOnly);
  p.aiPlayers = headerInt(text, "ai_players").value_or(d.aiPlayers);
  return p;
}

// Where a DLL's values differ from the properties, as "max tech level 12 (here 3)".
std::string differences(const Props& p, const Props& dll) {
  std::string out;
  auto diff = [&](const char* what, const std::string& inDll, const std::string& here) {
    if (inDll != here) out += std::string(out.empty() ? "" : ", ") + what + " " + inDll + " (here " + here + ")";
  };
  const auto typeOf = [](const std::string& n) { const auto s = missionTypeName(missionTypeFrom(n)); return s.empty() ? n : s; };
  diff("name", "\"" + dll.name + "\"", "\"" + p.name + "\"");
  diff("map", lower(dll.map), lower(p.map));
  diff("tech tree", lower(dll.tech), lower(p.tech));
  diff("type", typeOf(dll.type), typeOf(p.type));
  diff("max tech level", std::to_string(dll.maxTech), std::to_string(p.maxTech));
  diff("players", std::to_string(dll.players), std::to_string(p.players));
  diff("unit-only", dll.unitOnly ? "true" : "false", p.unitOnly ? "true" : "false");
  diff("AI players", std::to_string(dll.aiPlayers), std::to_string(p.aiPlayers));
  return out;
}

// ---- files --------------------------------------------------------------------------------------------------------

// Mission name -> file base name (alphanumerics only). The DLL base name decides which scripts it
// loads (cMyMission.dll -> MyMission.lua + MyMission.placement.lua; see main()).
std::string baseNameFrom(const std::string& name) {
  std::string b;
  for (unsigned char c : name) {
    if (std::isalnum(c)) b += static_cast<char>(c);
  }
  return b;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
  for (std::size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size()) {
    s.replace(pos, from.size(), to);
  }
  return s;
}

// Locate the stub DLL: explicit --stub, else common spots relative to the cwd and the executable.
fs::path findStub(const std::string& explicitStub, const fs::path& exeDir) {
  std::vector<fs::path> candidates;
  if (!explicitStub.empty()) candidates.emplace_back(explicitStub);
  candidates.emplace_back("LuaMission.dll");
  candidates.emplace_back(fs::path("bin") / "LuaMission.dll");
  if (!exeDir.empty()) {
    candidates.push_back(exeDir / "LuaMission.dll");
    candidates.push_back(exeDir / "bin" / "LuaMission.dll");
    candidates.push_back(exeDir.parent_path() / "bin" / "LuaMission.dll");  // tools/ -> ../bin/
  }
  std::error_code ec;
  for (const auto& c : candidates) {
    if (fs::exists(c, ec)) return c;
  }
  return {};
}

const char* kHeader = R"LUA(-- __BASE__.placement.lua  -  __NAME__  -  starting layout.
-- Coordinates are the ones shown on the in-game status bar (hover a tile to read them).
return {
  name = "__NAME__", map = "__MAP__", tech = "__TECH__", type = "__TYPE__",
  max_tech = __MAXTECH__, players_count = __PLAYERS__,
__EXTRA__
)LUA";

const char* kPlacementSkeleton = R"LUA(  players = {
    [1] = { colony = "Eden", human = true, color = "Blue",
            resources = { common_ore = 3000, food = 3000, kids = 8, workers = 12, scientists = 6, tech_level = 9 },
            center_view = { 30, 55 } },
    [2] = { colony = "Plymouth", human = false, color = "Red",
            resources = { common_ore = 4000, food = 4000, kids = 15, workers = 20, scientists = 10, tech_level = 12 } },
  },

  units = {
    -- Your starting force (player 1).
    { type = "Lynx",          player = 1, at = { 30, 54 }, weapon = "Laser" },
    { type = "Lynx",          player = 1, at = { 31, 54 }, weapon = "RPG" },
    { type = "CommandCenter", player = 1, at = { 28, 56 } },
    { type = "Tokamak",       player = 1, at = { 31, 57 } },

    -- The enemy you fight (player 2). A powered Guard Post will fire at you.
    { type = "CommandCenter", player = 2, at = { 58, 44 } },
    { type = "Tokamak",       player = 2, at = { 60, 44 } },
    { type = "GuardPost",     player = 2, at = { 58, 46 }, weapon = "RPG" },
  },

  beacons = {}, walls = {},
  regions = { enemy_base = { 54, 40, 64, 48 } },   -- a rectangle: { x1, y1, x2, y2 }
  markers = {},
}
)LUA";

const char* kMissionSkeleton = R"LUA(-- __BASE__.lua  -  __NAME__  -  mission logic.
-- See docs/API.md for the full API. Edit freely; this is your mission.

function on_init()
  game.message("__NAME__: destroy the enemy Command Center!")
  game.sound("NewMissionObjective")
  game.morale_steady()   -- keep morale stable so it's about the fight, not colony micro

  -- WIN: the enemy has no Command Center left.
  when(function() return not players[2]:owns_any("CommandCenter") end, function()
    game.message("Enemy base destroyed - mission accomplished!")
    game.sound("StructureDestroyed")
    mission.win()
  end)

  -- LOSE: your colony's Command Center is gone.
  when(function() return not players[1]:owns_any("CommandCenter") end, function()
    game.message("Your Command Center is destroyed. Mission failed.")
    mission.lose()
  end)

  -- Example (uncomment to try): reinforcements at mark 10.
  -- at_mark(10, function()
  --   game.message("Reinforcements have arrived!")
  --   game.create_unit{ type = "Lynx", player = players[1], at = { 30, 54 }, count = 3, weapon = "Laser" }
  -- end)
end
)LUA";

// A multiplayer type: the lobby sets each player's colony, colour and human/AI, so the placement has no players
// table (a players entry would override the lobby), only what every game starts with.
const char* kPlacementSkeletonMP = R"LUA(  -- No players table: in a multiplayer game the lobby has already set each player's colony and colour.

  units = {
    -- What every game starts with (player = 1..__PLAYERS__). Bases that depend on the lobby's
    -- settings (game.initial_units()) are made in on_init instead.
__UNITS__  },

  beacons = {}, walls = {},
  regions = {},
  markers = {},
}
)LUA";

const char* kMissionSkeletonMP = R"LUA(-- __BASE__.lua  -  __NAME__  -  mission logic.
-- See docs/API.md for the full API. Edit freely; this is your mission.

function on_init()
  -- The lobby's settings: game.initial_units(), game.uses_day_night(), game.uses_morale(),
  -- game.disasters_enabled(), and each player's .difficulty (the resource level).
__WIN__
end
)LUA";

void printUsage() {
  std::cout <<
    "op2lua-newmission - make a new OP2Lua mission, or rebuild a mission's DLL from its placement\n\n"
    "Usage:\n"
    "  op2lua-newmission \"<Mission Name>\" [options] [--out <dir>] [--stub <path>]\n"
    "  op2lua-newmission --dll-from <Base.placement.lua> [--stub <path>]\n\n"
    "What the mission DLL tells the game (written into the DLL and the placement's header):\n"
    "  --map        <file>  map the mission uses (default: eden01.map)\n"
    "  --tech       <file>  tech tree file (default: MULTITEK.TXT)\n"
    "  --type       <type>  Colony (default), Tutorial, AutoDemo, a campaign mission's number (1-12), or a\n"
    "                       multiplayer type: LandRush, SpaceRace, ResourceRace, Midas, LastOneStanding\n"
    "                       (MultiLandRush etc. too); OP2 lists the mission under it\n"
    "  --players    <1-6>   players in the mission (default: 2)\n"
    "  --max-tech   <0-12>  highest tech level (default: 12)\n"
    "  --unit-only          a unit-only mission (most reports off)\n"
    "  --ai-players <0-5>   computer players a multiplayer game adds (default: 0)\n\n"
    "Other:\n"
    "  --out        <dir>   output directory (default: ./out)\n"
    "  --stub       <path>  path to LuaMission.dll (default: auto-detect near cwd/exe)\n"
    "  --dll-from   <file>  build the DLL beside an existing mission's placement.lua from its header (name, map,\n"
    "                       tech, type, max_tech, players_count, unit_only, ai_players); keys it lacks are taken\n"
    "                       from the DLL already there. The old DLL is kept as .bak. Nothing else is written.\n"
    "  -h, --help           show this help\n\n"
    "A new mission is c<Base>.dll + <Base>.lua + <Base>.placement.lua (Base = name, alphanumerics only); a\n"
    "multiplayer type's DLL has no leading c.\n";
}

int intArg(const std::string& opt, const std::string& v) {
  try {
    std::size_t used = 0;
    const int n = std::stoi(v, &used);
    if (used == v.size()) return n;
  } catch (const std::exception&) {
  }
  std::cerr << "error: " << opt << " needs a number, not \"" << v << "\".\n";
  std::exit(2);
}

// The DLL beside a placement: c<Base>.dll, else <Base>.dll, else "".
fs::path existingDll(const fs::path& placement, const std::string& base) {
  std::error_code ec;
  for (const auto& c : {placement.parent_path() / ("c" + base + ".dll"), placement.parent_path() / (base + ".dll")})
    if (fs::exists(c, ec)) return c;
  return {};
}

// --dll-from: builds the DLL of an existing mission from its placement's header.
int dllFrom(const fs::path& placement, const Bytes& stub) {
  std::ifstream f(placement, std::ios::binary);
  if (!f) { std::cerr << "error: can't read " << placement.string() << "\n"; return 1; }
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();

  std::string file = placement.filename().string();
  const std::string suffix = ".placement.lua";
  if (file.size() <= suffix.size() || lower(file.substr(file.size() - suffix.size())) != suffix) {
    std::cerr << "error: " << file << " isn't a <Base>.placement.lua.\n";
    return 2;
  }
  const std::string base = file.substr(0, file.size() - suffix.size());

  fs::path target = existingDll(placement, base);
  Bytes old;
  std::optional<Props> oldProps;
  if (!target.empty()) {
    old = readFile(target);
    oldProps = readDll(old);
  }
  Props defaults = oldProps.value_or(Props{});
  if (!oldProps) defaults.name = base;
  const Props p = readHeader(text, defaults);
  if (target.empty()) {
    const bool capital = !base.empty() && std::isupper(static_cast<unsigned char>(base[0]));
    target = placement.parent_path() / ((capital && !isMultiplayer(p.type) ? "c" : "") + base + ".dll");
  }

  const Bytes built = buildDll(stub, p);
  if (built.empty()) return 1;
  const std::string name = target.filename().string();
  if (old == built) { std::cout << name << " was up to date.\n"; return 0; }
  if (!old.empty()) {
    const std::string s(old.begin(), old.end());
    if (lower(s).find("op2luacore.dll") == std::string::npos) {
      std::cerr << "error: " << name << " isn't an OP2Lua mission DLL, so it was left as it was.\n";
      return 1;
    }
    if (!writeFile(target.string() + ".bak", old)) { std::cerr << "error: can't back up " << name << "\n"; return 1; }
  }
  if (!writeFile(target, built)) { std::cerr << "error: can't write " << target.string() << " (is the game running?)\n"; return 1; }
  if (old.empty()) std::cout << "Made " << name << ".\n";
  else if (oldProps && !differences(p, *oldProps).empty()) std::cout << "Rebuilt " << name << "; it had " << differences(p, *oldProps) << ".\n";
  else std::cout << "Rebuilt " << name << " from the stub.\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Props p;
  std::string outDir = "out", stub, dllFromPath;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* opt) -> std::string {
      if (i + 1 >= argc) { std::cerr << "error: " << opt << " needs a value.\n"; std::exit(2); }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") { printUsage(); return 0; }
    else if (a == "--map")        p.map       = next("--map");
    else if (a == "--tech")       p.tech      = next("--tech");
    else if (a == "--type")       p.type      = next("--type");
    else if (a == "--players")    p.players   = intArg(a, next("--players"));
    else if (a == "--max-tech")   p.maxTech   = intArg(a, next("--max-tech"));
    else if (a == "--unit-only")  p.unitOnly  = true;
    else if (a == "--ai-players") p.aiPlayers = intArg(a, next("--ai-players"));
    else if (a == "--out")        outDir      = next("--out");
    else if (a == "--stub")       stub        = next("--stub");
    else if (a == "--dll-from")   dllFromPath = next("--dll-from");
    else if (!a.empty() && a[0] == '-') { std::cerr << "error: unknown option " << a << "\n"; return 2; }
    else if (p.name.empty()) p.name = a;
    else { std::cerr << "error: unexpected argument \"" << a << "\"\n"; return 2; }
  }

  if (p.name.empty() && dllFromPath.empty()) { printUsage(); return 2; }

  std::error_code ec;
  fs::path exeDir;
  { fs::path e = fs::weakly_canonical(fs::path(argv[0]), ec); if (!ec) exeDir = e.parent_path(); }

  const fs::path stubPath = findStub(stub, exeDir);
  if (stubPath.empty()) {
    std::cerr << "error: LuaMission.dll not found. Pass --stub <path> (it ships in the SDK's bin/ folder).\n";
    return 1;
  }
  const Bytes stubBytes = readFile(stubPath);
  if (stubBytes.empty()) { std::cerr << "error: could not read stub: " << stubPath.string() << "\n"; return 1; }

  if (!dllFromPath.empty()) {
    if (!p.name.empty()) { std::cerr << "error: --dll-from takes no mission name (it reads the placement's).\n"; return 2; }
    return dllFrom(dllFromPath, stubBytes);
  }

  // The type as the placement names it (MultiLastOneStanding, "1"), however it was given.
  if (missionTypeFrom(p.type) != 0) p.type = missionTypeName(missionTypeFrom(p.type));
  const Bytes dll = buildDll(stubBytes, p);
  if (dll.empty()) return 2;

  const std::string base = baseNameFrom(p.name);
  if (base.empty()) { std::cerr << "error: name \"" << p.name << "\" has no letters/digits.\n"; return 2; }

  fs::create_directories(outDir, ec);
  const fs::path outPath = outDir;

  // Outpost 2 lists a DLL whose name starts with c under Colony Games. The core finds cMyMission.dll's scripts
  // as MyMission.*, dropping a c that comes before a capital; a base that doesn't start with one keeps the c in
  // the script names too. Multiplayer missions have no c.
  const bool multi = isMultiplayer(p.type);
  const std::string dllBase = multi ? base : "c" + base;
  const std::string scriptBase =
      (!multi && !std::isupper(static_cast<unsigned char>(base[0]))) ? dllBase : base;

  std::string extra;
  if (p.unitOnly || p.aiPlayers != 0)
    extra = std::string("  unit_only = ") + (p.unitOnly ? "true" : "false") + ", ai_players = " + std::to_string(p.aiPlayers) + ",\n";
  std::string placement = kHeader;
  for (const auto& [k, v] : std::vector<std::pair<std::string, std::string>>{
           {"__BASE__", scriptBase}, {"__NAME__", p.name}, {"__MAP__", p.map}, {"__TECH__", p.tech}, {"__TYPE__", p.type},
           {"__MAXTECH__", std::to_string(p.maxTech)}, {"__PLAYERS__", std::to_string(p.players)}, {"__EXTRA__", extra}})
    placement = replaceAll(placement, k, v);

  std::string mission;
  if (!multi) {
    placement += kPlacementSkeleton;
    mission = replaceAll(replaceAll(kMissionSkeleton, "__BASE__", scriptBase), "__NAME__", p.name);
  } else {
    std::string units;
    for (int k = 1; k <= p.players; ++k)
      units += "    { type = \"CommandCenter\", player = " + std::to_string(k) + ", at = { " +
               std::to_string(20 + 12 * (k - 1)) + ", 20 } },\n";
    placement += replaceAll(replaceAll(kPlacementSkeletonMP, "__PLAYERS__", std::to_string(p.players)), "__UNITS__", units);
    const std::string win = (missionTypeFrom(p.type) == -8)
        ? "  mission.last_one_standing()   -- the game's own Last One Standing victory, per player"
        : "  -- Win: this type's victory with trigger.* and mission.victory (docs/API.md, \"Multiplayer missions\").";
    mission = replaceAll(replaceAll(replaceAll(kMissionSkeletonMP, "__BASE__", scriptBase), "__NAME__", p.name),
                         "__WIN__", win);
  }

  bool ok = true;
  ok &= writeFile(outPath / (dllBase + ".dll"), dll);
  ok &= writeText(outPath / (scriptBase + ".placement.lua"), placement);
  ok &= writeText(outPath / (scriptBase + ".lua"), mission);
  if (!ok) { std::cerr << "error: failed to write output files into " << outPath.string() << "\n"; return 1; }

  std::cout << "Created mission \"" << p.name << "\" in " << outPath.string() << " :\n"
            << "  " << dllBase << ".dll    (" << (missionTypeFrom(p.type) > 0 ? "campaign mission " + p.type : p.type) << ", map " << p.map << ", tech " << p.tech << ", max tech level "
            << p.maxTech << ", " << p.players << " player(s)" << (p.unitOnly ? ", unit-only" : "")
            << (p.aiPlayers ? ", " + std::to_string(p.aiPlayers) + " AI player(s)" : "") << ")\n"
            << "  " << scriptBase << ".lua    (edit your logic here)\n"
            << "  " << scriptBase << ".placement.lua    (edit your layout here)\n\n"
            << "Next: copy all three files into your Outpost 2  OPU/maps  folder, then launch the game.\n"
            << "See the SDK's INSTALL.md for paths and docs/GETTING-STARTED.md for the walkthrough.\n";
  return 0;
}
