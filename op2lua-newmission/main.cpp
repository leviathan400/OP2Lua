// op2lua-newmission - scaffold a new OP2Lua mission (cross-platform console tool).
//
// Clones the prebuilt mission stub (LuaMission.dll), byte-patches its menu name and map, and writes
// starter <Mission>.lua + <Mission>.placement.lua next to it. No compiler needed by the author -
// this is the portable equivalent of tools/new-mission.ps1, and the manual stand-in for the future
// OP2MissionEditor .opm -> Lua converter.
//
// Pure C++17 (<filesystem> + <fstream>); builds and runs on Windows, Linux, and macOS.
//
// Usage:
//   op2lua-newmission "My First Mission"
//   op2lua-newmission "Red Dawn" --map sgame01.map --out ./missions
//   op2lua-newmission "Test" --stub ../bin/LuaMission.dll

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// The placeholder strings baked into the stub template (see stub/LuaMission.cpp). Each is a fixed
// char[1024] export, so we can overwrite in place as long as the replacement fits.
constexpr const char* kLevelDescTemplate = "OP2Lua mission (unpatched template)";
constexpr const char* kMapNameTemplate   = "eden01.map";
constexpr std::size_t  kFieldMax          = 1024;

// FileDescription placeholder in the stub's version resource (stored as UTF-16). Must match the
// text in stub/LuaMission.rc byte-for-byte. We patch it to the mission name so the mission DLL's
// Windows Properties show "<Mission Name>" (alongside the fixed "OP2Lua Mission" / "Outpost Universe").
constexpr const char* kDescPlaceholder = "OP2Lua mission name placeholder - set by the new-mission tool";

std::vector<std::uint8_t> readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
}

bool writeFile(const fs::path& p, const std::vector<std::uint8_t>& bytes) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(f);
}

bool writeText(const fs::path& p, const std::string& text) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);  // binary => exact bytes, LF preserved
  if (!f) return false;
  f << text;
  return static_cast<bool>(f);
}

// Overwrite a fixed-size char[] export string in the DLL image: replace `oldStr` with `newStr`,
// clearing the old contents and leaving a null terminator. Returns false if not found / too long.
bool patchField(std::vector<std::uint8_t>& buf, const std::string& oldStr, const std::string& newStr) {
  if (newStr.size() + 1 >= kFieldMax) {
    std::cerr << "error: '" << newStr << "' is too long.\n";
    return false;
  }
  auto it = std::search(buf.begin(), buf.end(), oldStr.begin(), oldStr.end());
  if (it == buf.end()) {
    std::cerr << "error: template string \"" << oldStr << "\" not found in stub DLL.\n";
    return false;
  }
  const std::size_t idx   = static_cast<std::size_t>(it - buf.begin());
  const std::size_t clear = std::max(oldStr.size(), newStr.size() + 1);  // wipe old, keep a NUL
  for (std::size_t k = 0; k < clear; ++k)        buf[idx + k] = 0;
  for (std::size_t k = 0; k < newStr.size(); ++k) buf[idx + k] = static_cast<std::uint8_t>(newStr[k]);
  return true;
}

// Overwrite a UTF-16 string value in the version resource: find `placeholder` (as UTF-16LE) and
// replace it with `value` (UTF-16LE), zero-filling the rest of the field's bytes. Length-preserving,
// so the resource structure stays valid.
bool patchWideField(std::vector<std::uint8_t>& buf, const std::string& placeholder, const std::string& value) {
  std::vector<std::uint8_t> needle;
  needle.reserve(placeholder.size() * 2);
  for (char c : placeholder) { needle.push_back(static_cast<std::uint8_t>(c)); needle.push_back(0); }
  auto it = std::search(buf.begin(), buf.end(), needle.begin(), needle.end());
  if (it == buf.end()) {
    std::cerr << "error: description placeholder not found in stub DLL (resource missing?).\n";
    return false;
  }
  if (value.size() > placeholder.size()) {
    std::cerr << "error: name is too long for the description field.\n";
    return false;
  }
  const std::size_t idx       = static_cast<std::size_t>(it - buf.begin());
  const std::size_t fieldBytes = (placeholder.size() + 1) * 2;   // includes the UTF-16 null
  for (std::size_t k = 0; k < fieldBytes; ++k)   buf[idx + k] = 0;
  for (std::size_t k = 0; k < value.size(); ++k) buf[idx + k * 2] = static_cast<std::uint8_t>(value[k]);
  return true;
}

// Patch the DescBlock's mission type and numPlayers. The stub bakes
// ModDesc(MissionType::Colony=-1, numPlayers=2, maxTechLevel=12, unitMission=0), which lays out as
// bytes  FF FF FF FF | 02 00 00 00 | 0C 00 00 00 | 00 00 00 00 . We find that signature and overwrite
// the type (an int) and the numPlayers byte. OP2 needs both in the DLL: it lists a mission by its type
// (Colony, or a multiplayer type such as Last One Standing) and sizes the game by numPlayers;
// placement players_count alone is not enough - an extra player would be out of bounds and crash.
bool patchDesc(std::vector<std::uint8_t>& buf, int type, int n) {
  static const std::uint8_t pat[12] =
      { 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00 };
  auto it = std::search(buf.begin(), buf.end(), pat, pat + 12);
  if (it == buf.end()) {
    std::cerr << "error: DescBlock signature not found in stub DLL (can't set player count).\n";
    return false;
  }
  const std::size_t at = static_cast<std::size_t>(it - buf.begin());
  for (int k = 0; k < 4; ++k) buf[at + k] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(type) >> (8 * k));
  buf[at + 4] = static_cast<std::uint8_t>(n);  // numPlayers byte
  return true;
}

// Mission type names (--type) -> the DescBlock's MissionType values.
int missionTypeFrom(const std::string& name) {
  if (name == "Colony")          return -1;
  if (name == "LandRush")        return -4;
  if (name == "SpaceRace")       return -5;
  if (name == "ResourceRace")    return -6;
  if (name == "Midas")           return -7;
  if (name == "LastOneStanding") return -8;
  return 0;
}

// Mission name -> file base name (alphanumerics only). The DLL base name decides which scripts it
// loads (MyMission.dll -> MyMission.lua + MyMission.placement.lua).
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

const char* kPlacementSkeleton = R"LUA(-- __BASE__.placement.lua  -  __NAME__  -  starting layout.
-- Coordinates are the ones shown on the in-game status bar (hover a tile to read them).
return {
  name = "__NAME__", map = "__MAP__", tech = "MULTITEK.TXT", type = "Colony",
  max_tech = 12, players_count = __PLAYERS__,

  players = {
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

void printUsage() {
  std::cout <<
    "op2lua-newmission - scaffold a new OP2Lua mission\n\n"
    "Usage:\n"
    "  op2lua-newmission \"<Mission Name>\" [--map <file.map>] [--players <1-6>] [--type <type>] [--out <dir>] [--stub <path>]\n\n"
    "Options:\n"
    "  --map     <file>   map the mission uses (default: eden01.map)\n"
    "  --players <1-6>    number of players in the mission (default: 2) - patches the DLL's DescBlock\n"
    "  --type    <type>   Colony (default), or a multiplayer type: LandRush, SpaceRace, ResourceRace,\n"
    "                     Midas, LastOneStanding - patches the DLL's DescBlock; OP2 lists it there\n"
    "  --out     <dir>    output directory (default: ./out)\n"
    "  --stub    <path>   path to LuaMission.dll (default: auto-detect near cwd/exe)\n"
    "  -h, --help         show this help\n\n"
    "Creates <Base>.dll + <Base>.lua + <Base>.placement.lua (Base = name, alphanumerics only).\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string name, map = "eden01.map", outDir = "out", stub;
  int players = 2;
  std::string type = "Colony";

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* opt) -> std::string {
      if (i + 1 >= argc) { std::cerr << "error: " << opt << " needs a value.\n"; std::exit(2); }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") { printUsage(); return 0; }
    else if (a == "--map")     map     = next("--map");
    else if (a == "--out")     outDir  = next("--out");
    else if (a == "--stub")    stub    = next("--stub");
    else if (a == "--players") players = std::stoi(next("--players"));
    else if (a == "--type")    type    = next("--type");
    else if (!a.empty() && a[0] == '-') { std::cerr << "error: unknown option " << a << "\n"; return 2; }
    else if (name.empty()) name = a;
    else { std::cerr << "error: unexpected argument \"" << a << "\"\n"; return 2; }
  }

  if (name.empty()) { printUsage(); return 2; }
  if (players < 1 || players > 6) { std::cerr << "error: --players must be 1-6.\n"; return 2; }
  const int missionType = missionTypeFrom(type);
  if (missionType == 0) { std::cerr << "error: unknown --type " << type << ".\n"; return 2; }

  const std::string base = baseNameFrom(name);
  if (base.empty()) { std::cerr << "error: name \"" << name << "\" has no letters/digits.\n"; return 2; }

  std::error_code ec;
  fs::path exeDir;
  { fs::path p = fs::weakly_canonical(fs::path(argv[0]), ec); if (!ec) exeDir = p.parent_path(); }

  const fs::path stubPath = findStub(stub, exeDir);
  if (stubPath.empty()) {
    std::cerr << "error: LuaMission.dll not found. Pass --stub <path> "
                 "(it ships in the SDK's bin/ folder).\n";
    return 1;
  }

  std::vector<std::uint8_t> dll = readFile(stubPath);
  if (dll.empty()) { std::cerr << "error: could not read stub: " << stubPath << "\n"; return 1; }

  if (!patchField(dll, kLevelDescTemplate, name)) return 1;       // menu name (OP2 mission list)
  if (!patchField(dll, kMapNameTemplate, map))    return 1;       // map file
  if (!patchWideField(dll, kDescPlaceholder, name)) return 1;     // Windows file-description = name
  if ((players != 2 || missionType != -1) && !patchDesc(dll, missionType, players)) return 1;  // type + numPlayers

  fs::create_directories(outDir, ec);
  const fs::path outPath = outDir;

  const std::string placement =
      replaceAll(replaceAll(replaceAll(replaceAll(kPlacementSkeleton, "__BASE__", base), "__NAME__", name),
                            "__MAP__", map), "__PLAYERS__", std::to_string(players));
  const std::string mission =
      replaceAll(replaceAll(kMissionSkeleton, "__BASE__", base), "__NAME__", name);

  bool ok = true;
  ok &= writeFile(outPath / (base + ".dll"), dll);
  ok &= writeText(outPath / (base + ".placement.lua"), placement);
  ok &= writeText(outPath / (base + ".lua"), mission);
  if (!ok) { std::cerr << "error: failed to write output files into " << outPath << "\n"; return 1; }

  std::cout << "Created mission \"" << name << "\" in " << outPath.string() << " :\n"
            << "  " << base << ".dll              (menu name \"" << name << "\", map \"" << map << "\")\n"
            << "  " << base << ".lua              (edit your logic here)\n"
            << "  " << base << ".placement.lua    (edit your layout here)\n\n"
            << "Next: copy all three files into your Outpost 2  OPU/maps  folder, then launch the game.\n"
            << "See the SDK's INSTALL.md for paths and docs/GETTING-STARTED.md for the walkthrough.\n";
  return 0;
}
