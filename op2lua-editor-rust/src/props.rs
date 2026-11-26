//! A mission's properties (the top of its placement.lua: name, map, tech, type, max_tech, players_count, unit_only,
//! ai_players), the mission DLL built from them, and New Mission's files.

use std::path::{Path, PathBuf};

use op2_formats::mission::MissionDll;
use op2_formats::placement::{new_line_of, replace_scalar_int, replace_scalar_string, scalar_int, scalar_string};

use crate::dll;

#[derive(Debug, Clone, PartialEq, Eq)]
/// The mission's properties as placement.lua writes them, which are what its DLL holds: `type` is a name such as
/// Colony or MultiLastOneStanding (or a campaign mission's number), `max_tech` the highest tech level, `players` the
/// player count, `unit_only` the DescBlock's unit-only flag (most reports off) and `ai_players` the computer players
/// a multiplayer game adds (DescBlockEx).
pub struct Props {
    pub name: String,
    pub map: String,
    pub tech: String,
    pub mission_type: String,
    pub max_tech: i32,
    pub players: i32,
    pub unit_only: bool,
    pub ai_players: i32,
}

impl Default for Props {
    fn default() -> Self {
        Self {
            name: String::new(),
            map: "eden01.map".into(),
            tech: "MULTITEK.TXT".into(),
            mission_type: "Colony".into(),
            max_tech: 12,
            players: 2,
            unit_only: false,
            ai_players: 0,
        }
    }
}

fn is_word_byte(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_'
}

/// The first `key = true` / `key = false` in the text and where its value is (the header's keys are unique in a
/// placement).
fn scalar_bool(text: &str, key: &str) -> Option<(bool, std::ops::Range<usize>)> {
    let b = text.as_bytes();
    let mut from = 0;
    while let Some(hit) = text[from..].find(key).map(|p| p + from) {
        from = hit + key.len();
        if hit > 0 && is_word_byte(b[hit - 1]) || b.get(from).is_some_and(|&c| is_word_byte(c)) {
            continue;
        }
        let Some(value) = text[from..].trim_start().strip_prefix('=').map(str::trim_start) else { continue };
        let at = text.len() - value.len();
        for (word, v) in [("true", true), ("false", false)] {
            let end = at + word.len();
            if value.starts_with(word) && !b.get(end).is_some_and(|&c| is_word_byte(c)) {
                return Some((v, at..end));
            }
        }
    }
    None
}

/// The text with a header line `line` added after the line holding `return {` (unchanged when there is none).
fn insert_header_line(text: &str, line: &str) -> String {
    let Some(ret) = text.find("return {") else { return text.to_string() };
    let nl = new_line_of(text);
    match text[ret..].find('\n').map(|p| p + ret + 1) {
        Some(at) => format!("{}  {line}{nl}{}", &text[..at], &text[at..]),
        None => format!("{text}{nl}  {line}{nl}"),
    }
}

impl Props {
    /// The properties a placement's text declares (defaults for the ones it doesn't).
    #[cfg(test)]
    pub fn read(text: &str) -> Self {
        Self::read_with_dll(text, None)
    }

    /// The properties a placement's text declares; the ones it doesn't declare are the DLL's, else the defaults.
    pub fn read_with_dll(text: &str, dll: Option<&MissionDll>) -> Self {
        let d = dll.map(Self::from_dll).unwrap_or_default();
        let s = |k: &str, def: &str| scalar_string(text, k).filter(|v| !v.is_empty()).unwrap_or_else(|| def.to_string());
        Self {
            name: scalar_string(text, "name").unwrap_or_else(|| if dll.is_some() { d.name.clone() } else { String::new() }),
            map: scalar_string(text, "map").unwrap_or_else(|| if dll.is_some() { d.map.clone() } else { String::new() }),
            tech: s("tech", &d.tech),
            mission_type: s("type", &d.mission_type),
            max_tech: scalar_int(text, "max_tech").unwrap_or(d.max_tech),
            players: scalar_int(text, "players_count").unwrap_or(d.players),
            unit_only: scalar_bool(text, "unit_only").map_or(d.unit_only, |(v, _)| v),
            ai_players: scalar_int(text, "ai_players").unwrap_or(d.ai_players),
        }
    }

    /// The properties a mission DLL holds.
    pub fn from_dll(d: &MissionDll) -> Self {
        Self {
            name: d.level_desc.clone(),
            map: d.map_name.clone(),
            tech: d.techtree_name.clone(),
            mission_type: dll::mission_type_name(d.mission_type.value()).unwrap_or_else(|| Self::default().mission_type),
            max_tech: d.max_tech_level,
            players: d.players,
            unit_only: d.unit_only,
            ai_players: d.extra_ai_players.unwrap_or(0),
        }
    }

    /// Where a DLL's description differs from these properties, as "max tech level 12 (here 3)".
    pub fn differences(&self, d: &MissionDll) -> Vec<String> {
        let o = Self::from_dll(d);
        let mut out = Vec::new();
        let mut diff = |what: &str, dll: String, here: String| {
            if dll != here {
                out.push(format!("{what} {dll} (here {here})"));
            }
        };
        diff("name", format!("\"{}\"", o.name), format!("\"{}\"", self.name));
        diff("map", o.map.to_ascii_lowercase(), self.map.to_ascii_lowercase());
        diff("tech tree", o.tech.to_ascii_lowercase(), self.tech.to_ascii_lowercase());
        let t = |n: &str| dll::mission_type_value(n).and_then(dll::mission_type_name).unwrap_or_else(|| n.to_string());
        diff("type", t(&o.mission_type), t(&self.mission_type));
        diff("max tech level", o.max_tech.to_string(), self.max_tech.to_string());
        diff("players", o.players.to_string(), self.players.to_string());
        diff("unit-only", o.unit_only.to_string(), self.unit_only.to_string());
        if d.extra_ai_players.is_some() {
            diff("AI players", o.ai_players.to_string(), self.ai_players.to_string());
        }
        out
    }

    /// `text` with these properties written over the ones it declares, and added after `return {` where it doesn't
    /// (unit_only and ai_players only when they aren't off and 0).
    pub fn write_into(&self, text: &str) -> String {
        let mut t = text.to_string();
        for (key, value) in [("name", &self.name), ("map", &self.map), ("tech", &self.tech), ("type", &self.mission_type)] {
            t = if scalar_string(&t, key).is_some() {
                replace_scalar_string(&t, key, value)
            } else {
                insert_header_line(&t, &format!("{key} = \"{value}\","))
            };
        }
        for (key, value, add) in [
            ("max_tech", self.max_tech, true),
            ("players_count", self.players, true),
            ("ai_players", self.ai_players, self.ai_players != 0),
        ] {
            if scalar_int(&t, key).is_some() {
                t = replace_scalar_int(&t, key, value);
            } else if add {
                t = insert_header_line(&t, &format!("{key} = {value},"));
            }
        }
        match scalar_bool(&t, "unit_only") {
            Some((_, at)) => t.replace_range(at, if self.unit_only { "true" } else { "false" }),
            None if self.unit_only => t = insert_header_line(&t, "unit_only = true,"),
            None => {}
        }
        t
    }

    /// Why these properties can't be used, if they can't.
    pub fn problem(&self) -> Option<String> {
        if self.name.trim().is_empty() {
            return Some("The name can't be empty.".into());
        }
        if !self.name.is_ascii() || !self.map.is_ascii() || !self.tech.is_ascii() {
            return Some("The name, map and tech tree are stored in the mission DLL as plain ASCII.".into());
        }
        if [&self.name, &self.map, &self.tech].iter().any(|v| v.len() >= dll::TEXT_EXPORT_LEN) {
            return Some(format!("The name, map and tech tree must be under {} characters.", dll::TEXT_EXPORT_LEN));
        }
        if self.map.trim().is_empty() {
            return Some("Choose a map.".into());
        }
        if self.tech.trim().is_empty() {
            return Some("Name a tech tree file (MULTITEK.TXT, say).".into());
        }
        if dll::mission_type_value(&self.mission_type).is_none() {
            return Some(format!(
                "The type must be one of {}, or a campaign mission's number (1 to 12).",
                dll::MISSION_TYPES.join(", ")
            ));
        }
        if !(1..=6).contains(&self.players) {
            return Some("Players must be 1 to 6.".into());
        }
        if !(0..=12).contains(&self.max_tech) {
            return Some("The max tech level must be 0 to 12.".into());
        }
        if !(0..=5).contains(&self.ai_players) {
            return Some("AI players must be 0 to 5.".into());
        }
        None
    }

    /// The mission DLL these properties describe: a copy of the bundled stub with each field written at its export.
    pub fn build_dll(&self) -> Result<Vec<u8>, String> {
        if let Some(p) = self.problem() {
            return Err(p);
        }
        let mut b = dll::STUB.to_vec();
        let t = dll::mission_type_value(&self.mission_type).ok_or("Unknown mission type.")?;
        let ok = dll::write_text_export(&mut b, "LevelDesc", &self.name)
            && dll::write_text_export(&mut b, "MapName", &self.map)
            && dll::write_text_export(&mut b, "TechtreeName", &self.tech)
            && dll::write_int_export(&mut b, "DescBlock", &[t, self.players, self.max_tech, i32::from(self.unit_only)])
            && dll::write_int_export(&mut b, "DescBlockEx", &[self.ai_players]);
        if !ok {
            return Err("The bundled stub DLL lacks one of the mission exports.".into());
        }
        dll::patch_wide_field(&mut b, dll::STUB_DESC, &self.name); // FileDescription (Properties > Details), best effort
        Ok(b)
    }
}

/// The mission DLL beside a placement file: `c<Base>.dll`, else `<Base>.dll`.
pub fn mission_dll(placement: &Path) -> Option<PathBuf> {
    let base = crate::mission::base_name(placement);
    let dir = placement.parent()?;
    [dir.join(format!("c{base}.dll")), dir.join(format!("{base}.dll"))].into_iter().find(|p| p.is_file())
}

/// The DLL a mission's placement file gets when it has none: `c<Base>.dll` for a single-player mission whose base
/// starts with a capital (Outpost 2 lists it under Colony Games and OP2LuaCore drops the c), else `<Base>.dll`.
pub fn dll_for(placement: &Path, props: &Props) -> PathBuf {
    let base = crate::mission::base_name(placement);
    let capital = base.chars().next().is_some_and(|c| c.is_ascii_uppercase());
    let name = if capital && !dll::is_multiplayer_type(&props.mission_type) { format!("c{base}") } else { base };
    placement.with_file_name(format!("{name}.dll"))
}

/// Whether a file is an OP2Lua stub (it imports OP2LuaCore.dll), which the editor may replace.
pub fn is_op2lua_stub(bytes: &[u8]) -> bool {
    bytes.windows(14).any(|w| w.eq_ignore_ascii_case(b"OP2LuaCore.dll"))
}

/// Writes the mission DLL for a placement file's text: the DLL beside it (or a new one), built from the properties
/// the text declares (the old DLL's for any it doesn't). The previous DLL is kept as `.bak`; a DLL that isn't an
/// OP2Lua stub is left alone. Returns what was done.
pub fn save_dll(placement: &Path, text: &str) -> Result<String, String> {
    let existing = mission_dll(placement);
    let old = match &existing {
        Some(p) => Some(std::fs::read(p).map_err(|e| format!("Can't read {}: {e}", p.display()))?),
        None => None,
    };
    let old_desc = old.as_deref().and_then(|b| MissionDll::parse(b).ok());
    let mut props = Props::read_with_dll(text, old_desc.as_ref());
    if props.name.trim().is_empty() {
        props.name = crate::mission::base_name(placement);
    }
    let path = existing.unwrap_or_else(|| dll_for(placement, &props));
    let file = path.file_name().unwrap_or_default().to_string_lossy().into_owned();
    let built = props.build_dll().map_err(|e| format!("{file} not written: {e}"))?;
    let Some(old) = old else {
        std::fs::write(&path, &built).map_err(|e| format!("Can't write {}: {e}", path.display()))?;
        return Ok(format!("Made {file}."));
    };
    if old == built {
        return Ok(format!("{file} was up to date."));
    }
    if !is_op2lua_stub(&old) {
        return Err(format!("{file} isn't an OP2Lua mission DLL, so it was left as it was."));
    }
    let changes = old_desc.map(|d| props.differences(&d)).unwrap_or_default();
    let bak = PathBuf::from(format!("{}.bak", path.display()));
    std::fs::copy(&path, &bak).map_err(|e| format!("Can't back up {}: {e}", path.display()))?;
    std::fs::write(&path, &built).map_err(|e| format!("Can't write {} (is the game running?): {e}", path.display()))?;
    Ok(if changes.is_empty() {
        format!("Rebuilt {file} from the editor's stub.")
    } else {
        format!("Rebuilt {file}; it had {}.", changes.join(", "))
    })
}

/// What Save would change in the mission's DLL, for a note: the DLL's values that differ from the placement's text,
/// or that there is no DLL yet. None when the DLL already matches.
pub fn dll_note(placement: &Path, text: &str) -> Option<String> {
    let Some(path) = mission_dll(placement) else {
        return Some("There is no mission DLL beside it yet: Save makes one.".into());
    };
    let file = path.file_name().unwrap_or_default().to_string_lossy().into_owned();
    let Ok(d) = MissionDll::open(&path) else { return Some(format!("{file} can't be read as a mission DLL.")) };
    let changes = Props::read_with_dll(text, Some(&d)).differences(&d);
    (!changes.is_empty()).then(|| format!("{file} has {}: Save writes the placement's values into it.", changes.join(", ")))
}

/// A mission's base name: its name's letters and digits (as op2lua-newmission makes it).
pub fn base_name_from(name: &str) -> String {
    name.chars().filter(|c| c.is_ascii_alphanumeric()).collect()
}

/// The three files New Mission writes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NewFiles {
    /// The scripts' base name (`<base>.lua`, `<base>.placement.lua`) and the three paths.
    pub base: String,
    pub dll: PathBuf,
    pub script: PathBuf,
    pub placement: PathBuf,
}

/// Where New Mission puts a mission named by `props` in `dir`. Outpost 2 lists a DLL whose name starts with c under
/// Colony Games, and OP2LuaCore finds cMyMission.dll's scripts as MyMission.* (it drops a c before a capital), so a
/// base without a leading capital keeps the c on the scripts too. Multiplayer missions have no c.
pub fn new_files(dir: &Path, props: &Props) -> Option<NewFiles> {
    let mut base = base_name_from(&props.name);
    let first = base.chars().next()?;
    let colony = !dll::is_multiplayer_type(&props.mission_type);
    let dll_base = if colony { format!("c{base}") } else { base.clone() };
    if colony && !first.is_ascii_uppercase() {
        base = dll_base.clone();
    }
    Some(NewFiles {
        dll: dir.join(format!("{dll_base}.dll")),
        script: dir.join(format!("{base}.lua")),
        placement: dir.join(format!("{base}.placement.lua")),
        base,
    })
}

/// Writes a new mission: the DLL built from its properties, and starter scripts.
pub fn create(files: &NewFiles, props: &Props) -> Result<(), String> {
    let bytes = props.build_dll()?;
    if let Some(dir) = files.dll.parent() {
        std::fs::create_dir_all(dir).map_err(|e| format!("Can't create {}: {e}", dir.display()))?;
    }
    let write = |p: &Path, data: &[u8]| std::fs::write(p, data).map_err(|e| format!("Can't write {}: {e}", p.display()));
    write(&files.dll, &bytes)?;
    write(&files.placement, placement_skeleton(&files.base, props).as_bytes())?;
    write(&files.script, script_skeleton(&files.base, props).as_bytes())?;
    Ok(())
}

/// The starter placement.lua: a small two-player fight, or for a multiplayer type a Command Center per player and no
/// players table (the lobby sets each player's colony and colour).
pub fn placement_skeleton(base: &str, p: &Props) -> String {
    let q = |s: &str| format!("\"{s}\"");
    let mut s = format!(
        "-- {base}.placement.lua  -  {}  -  starting layout.\n\
         -- Coordinates are the ones shown on the in-game status bar (hover a tile to read them).\n\
         return {{\n  name = {}, map = {}, tech = {}, type = {},\n  max_tech = {}, players_count = {},\n",
        p.name,
        q(&p.name),
        q(&p.map),
        q(&p.tech),
        q(&p.mission_type),
        p.max_tech,
        p.players
    );
    if p.unit_only || p.ai_players != 0 {
        s += &format!("  unit_only = {}, ai_players = {},\n", p.unit_only, p.ai_players);
    }
    s += "\n";
    if dll::is_multiplayer_type(&p.mission_type) {
        s += "  -- No players table: in a multiplayer game the lobby has already set each player's colony and colour.\n\n";
        s += "  units = {\n";
        s += &format!("    -- What every game starts with (player = 1..{}). Bases that depend on the lobby's\n", p.players);
        s += "    -- settings (game.initial_units()) are made in on_init instead.\n";
        for k in 1..=p.players {
            s += &format!("    {{ type = \"CommandCenter\", player = {k}, at = {{ {}, 20 }} }},\n", 20 + 12 * (k - 1));
        }
        s += "  },\n\n  beacons = {}, walls = {},\n  regions = {},\n  markers = {},\n}\n";
        return s;
    }
    s += r#"  players = {
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
"#;
    s
}

/// The starter `<Base>.lua`: win or lose on the Command Centers, or for a multiplayer type the lobby's settings and
/// the type's victory.
pub fn script_skeleton(base: &str, p: &Props) -> String {
    let mut s = format!(
        "-- {base}.lua  -  {}  -  mission logic.\n-- See docs/API.md for the full API. Edit freely; this is your mission.\n\n",
        p.name
    );
    if dll::is_multiplayer_type(&p.mission_type) {
        s += "function on_init()\n";
        s += "  -- The lobby's settings: game.initial_units(), game.uses_day_night(), game.uses_morale(),\n";
        s += "  -- game.disasters_enabled(), and each player's .difficulty (the resource level).\n";
        if dll::mission_type_value(&p.mission_type) == Some(-8) {
            s += "  mission.last_one_standing()   -- the game's own Last One Standing victory, per player\n";
        } else {
            s += "  -- Win: this type's victory with trigger.* and mission.victory (docs/API.md, \"Multiplayer missions\").\n";
        }
        s += "end\n";
        return s;
    }
    s += &format!("function on_init()\n  game.message(\"{}: destroy the enemy Command Center!\")\n", p.name.replace('"', "\\\""));
    s += r#"  game.sound("NewMissionObjective")
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
end
"#;
    s
}

#[cfg(test)]
mod tests {
    use super::*;
    use op2_formats::mission::{MissionDll, MissionType};

    fn temp_dir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("op2luaeditor-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn colony_names_get_the_c() {
        let p = Props { name: "Iron Front!".into(), ..Default::default() };
        let f = new_files(Path::new("LUA"), &p).unwrap();
        assert_eq!((f.dll.file_name().unwrap().to_str().unwrap(), f.base.as_str()), ("cIronFront.dll", "IronFront"));
        let p = Props { name: "rescue".into(), ..Default::default() };
        let f = new_files(Path::new("LUA"), &p).unwrap();
        assert_eq!((f.dll.file_name().unwrap().to_str().unwrap(), f.base.as_str()), ("crescue.dll", "crescue"));
        let p = Props { name: "Duel".into(), mission_type: "MultiLastOneStanding".into(), ..Default::default() };
        assert_eq!(new_files(Path::new("LUA"), &p).unwrap().dll, Path::new("LUA").join("Duel.dll"));
        assert!(new_files(Path::new("LUA"), &Props { name: "!!".into(), ..Default::default() }).is_none());
        assert_eq!(dll_for(Path::new("LUA/IronFront.placement.lua"), &p), Path::new("LUA").join("IronFront.dll"));
        assert_eq!(dll_for(Path::new("LUA/IronFront.placement.lua"), &Props::default()), Path::new("LUA").join("cIronFront.dll"));
        assert_eq!(dll_for(Path::new("LUA/q01Lua.placement.lua"), &Props::default()), Path::new("LUA").join("q01Lua.dll"));
    }

    #[test]
    fn every_field_reaches_the_dll() {
        let p = Props {
            name: "Relocation (Lua)".into(),
            map: "Q01.MAP".into(),
            tech: "Q01.tek".into(),
            mission_type: "1".into(),
            max_tech: 3,
            players: 2,
            unit_only: true,
            ai_players: 1,
        };
        let d = MissionDll::parse(&p.build_dll().unwrap()).unwrap();
        assert_eq!(Props::from_dll(&d), p);
        assert!(p.differences(&d).is_empty());
        let mut q = p.clone();
        q.max_tech = 12;
        assert_eq!(q.differences(&d), ["max tech level 3 (here 12)"]);
    }

    #[test]
    fn header_keys_round_trip_and_are_added() {
        let text = "-- x\nreturn {\n  name = \"A\", map = \"eden01.map\",\n  units = { { type = \"Lynx\", player = 1, at = { 1, 1 } } },\n}\n";
        let p = Props {
            name: "B".into(),
            tech: "Q01.tek".into(),
            mission_type: "MultiMidas".into(),
            max_tech: 5,
            players: 3,
            unit_only: true,
            ai_players: 2,
            ..Default::default()
        };
        let out = p.write_into(text);
        assert_eq!(Props::read(&out), p);
        assert_eq!(op2_formats::placement::parse_units(&out).units.len(), 1);
        // Written again: the keys are replaced, not added twice.
        let q = Props { unit_only: false, ai_players: 0, ..p.clone() };
        let again = q.write_into(&out);
        assert_eq!(Props::read(&again), q);
        assert_eq!(again.matches("unit_only").count(), 1);
        assert_eq!(again.matches("ai_players").count(), 1);
        // Off and 0 aren't added to a file that lacks them.
        assert!(!Props { name: "B".into(), ..Default::default() }.write_into(text).contains("unit_only"));
    }

    #[test]
    fn keys_a_placement_lacks_come_from_the_dll() {
        let p = Props { name: "X".into(), tech: "Q02.tek".into(), max_tech: 3, ai_players: 1, ..Default::default() };
        let d = MissionDll::parse(&p.build_dll().unwrap()).unwrap();
        let text = "return {\n  name = \"Y\", max_tech = 5,\n}\n";
        let r = Props::read_with_dll(text, Some(&d));
        assert_eq!((r.name.as_str(), r.max_tech, r.tech.as_str(), r.ai_players), ("Y", 5, "Q02.tek", 1));
    }

    #[test]
    fn save_makes_fixes_and_keeps_foreign_dlls() {
        let dir = temp_dir("savedll");
        let placement = dir.join("Rescue.placement.lua");
        let text = "return {\n  name = \"Rescue\", map = \"eden02.map\", tech = \"Q01.tek\", type = \"Colony\",\n  max_tech = 3, players_count = 1,\n}\n";
        assert_eq!(save_dll(&placement, text).unwrap(), "Made cRescue.dll.");
        let dll = dir.join("cRescue.dll");
        assert_eq!(save_dll(&placement, text).unwrap(), "cRescue.dll was up to date.");
        // A DLL left at the template's max tech level 12, as a hand-made one was: Save writes the placement's 3.
        let wrong = Props { max_tech: 12, ..Props::read(text) };
        std::fs::write(&dll, wrong.build_dll().unwrap()).unwrap();
        let summary = save_dll(&placement, text).unwrap();
        assert!(summary.contains("max tech level 12 (here 3)"), "{summary}");
        let d = MissionDll::open(&dll).unwrap();
        assert_eq!((d.max_tech_level, d.techtree_name.as_str(), d.players), (3, "Q01.tek", 1));
        assert!(dir.join("cRescue.dll.bak").is_file());
        // Not a stub: left alone.
        std::fs::write(&dll, b"MZ not a stub").unwrap();
        assert!(save_dll(&placement, text).is_err());
        assert_eq!(std::fs::read(&dll).unwrap(), b"MZ not a stub");
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn create_then_save() {
        let dir = temp_dir("props");
        let p = Props { name: "Iron Front".into(), map: "newworld.map".into(), players: 3, ..Default::default() };
        let files = new_files(&dir, &p).unwrap();
        create(&files, &p).unwrap();
        let d = MissionDll::open(&files.dll).unwrap();
        assert_eq!((d.level_desc.as_str(), d.map_name.as_str(), d.players), ("Iron Front", "newworld.map", 3));
        assert_eq!(d.mission_type, MissionType::Colony);
        let text = std::fs::read_to_string(&files.placement).unwrap();
        assert_eq!(Props::read(&text), p);
        assert_eq!(op2_formats::placement::parse_units(&text).units.len(), 7);
        assert_eq!(mission_dll(&files.placement).as_deref(), Some(files.dll.as_path()));

        let q = Props { name: "Steel Front".into(), map: "eden04.map".into(), mission_type: "Tutorial".into(), max_tech: 8, ..p.clone() };
        let rewritten = q.write_into(&text);
        let summary = save_dll(&files.placement, &rewritten).unwrap();
        assert!(summary.starts_with("Rebuilt cIronFront.dll"), "{summary}");
        let d = MissionDll::open(&files.dll).unwrap();
        assert_eq!((d.level_desc.as_str(), d.map_name.as_str(), d.max_tech_level), ("Steel Front", "eden04.map", 8));
        assert_eq!(d.mission_type, MissionType::Tutorial);
        assert_eq!(Props::read(&rewritten), q);
        assert_eq!(op2_formats::placement::parse_units(&rewritten).units.len(), 7);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn multiplayer_skeleton() {
        let p = Props { name: "Duel".into(), mission_type: "MultiLastOneStanding".into(), players: 4, ..Default::default() };
        let text = placement_skeleton("Duel", &p);
        assert_eq!(op2_formats::placement::parse_units(&text).units.len(), 4);
        assert!(!text.contains("players = {"));
        assert!(script_skeleton("Duel", &p).contains("mission.last_one_standing()"));
    }
}
