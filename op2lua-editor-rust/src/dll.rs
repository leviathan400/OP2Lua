//! Building a mission DLL: OP2 reads a mission's name, map, tech tree, type, player count, maximum tech level and
//! the rest of its description from its DLL before any Lua runs, so the editor writes them into a copy of the stub.

/// The bundled per-mission stub (OP2Lua's LuaMission.dll), cloned for each new mission.
pub const STUB: &[u8] = include_bytes!("../assets/LuaMission.dll");

/// The stub's LevelDesc, MapName and TechtreeName as built (the editor writes over them).
#[cfg(test)]
pub const STUB_NAME: &str = "OP2Lua mission (unpatched template)";
#[cfg(test)]
pub const STUB_MAP: &str = "eden01.map";
#[cfg(test)]
pub const STUB_TECH: &str = "MULTITEK.TXT";
/// The size of the LevelDesc, MapName and TechtreeName exports (`char[1024]`).
pub const TEXT_EXPORT_LEN: usize = 1024;
/// The stub's FileDescription placeholder (UTF-16, in the version resource). Must match stub/LuaMission.rc and the
/// command-line tools' OP2LUA_DESC_PLACEHOLDER byte for byte.
pub const STUB_DESC: &str = "OP2Lua mission name placeholder - set by the new-mission tool";

/// The mission types a placement may name, as the editor offers them.
pub const MISSION_TYPES: &[&str] =
    &["Colony", "Tutorial", "AutoDemo", "MultiLastOneStanding", "MultiLandRush", "MultiSpaceRace", "MultiResourceRace", "MultiMidas"];

/// A placement type name as the DescBlock's MissionType. Both the SDK's names (MultiLastOneStanding, as op2dll
/// writes them) and op2lua-newmission's --type names (LastOneStanding) are taken, and a campaign mission's number
/// (1 to 12). None for anything else.
pub fn mission_type_value(name: &str) -> Option<i32> {
    let name = name.trim();
    if let Ok(n) = name.parse::<i32>() {
        return (1..=12).contains(&n).then_some(n);
    }
    Some(match name.to_ascii_lowercase().as_str() {
        "colony" => -1,
        "autodemo" => -2,
        "tutorial" => -3,
        "multilandrush" | "landrush" => -4,
        "multispacerace" | "spacerace" => -5,
        "multiresourcerace" | "resourcerace" => -6,
        "multimidas" | "midas" => -7,
        "multilastonestanding" | "lastonestanding" => -8,
        _ => return None,
    })
}

/// The placement name of a DescBlock MissionType (a campaign mission's number as text). None for a value OP2 has no
/// type for.
pub fn mission_type_name(value: i32) -> Option<String> {
    if (1..=12).contains(&value) {
        return Some(value.to_string());
    }
    let name = match value {
        -1 => "Colony",
        -2 => "AutoDemo",
        -3 => "Tutorial",
        -4 => "MultiLandRush",
        -5 => "MultiSpaceRace",
        -6 => "MultiResourceRace",
        -7 => "MultiMidas",
        -8 => "MultiLastOneStanding",
        _ => return None,
    };
    Some(name.to_string())
}

/// Land Rush, Space Race, Resource Race, Midas or Last One Standing: listed in the multiplayer lobby.
pub fn is_multiplayer_type(name: &str) -> bool {
    mission_type_value(name).is_some_and(|v| v <= -4)
}

fn find(hay: &[u8], needle: &[u8], from: usize) -> Option<usize> {
    if needle.is_empty() || from >= hay.len() {
        return None;
    }
    hay[from..].windows(needle.len()).position(|w| w == needle).map(|p| p + from)
}

fn utf16(s: &str) -> Vec<u8> {
    s.encode_utf16().flat_map(u16::to_le_bytes).collect()
}

/// Overwrites a UTF-16 string in the version resource (FileDescription) within its existing room: `old` plus the
/// run of NUL words after it, one kept for the terminator. False when `old` isn't there or `new` doesn't fit.
pub fn patch_wide_field(buf: &mut [u8], old: &str, new: &str) -> bool {
    if old.is_empty() {
        return false;
    }
    let needle = utf16(old);
    let mut from = 0;
    let at = loop {
        let Some(hit) = find(buf, &needle, from) else { return false };
        let t = hit + needle.len();
        if buf.get(t) == Some(&0) && buf.get(t + 1) == Some(&0) {
            break hit;
        }
        from = hit + 1;
    };
    let mut cap_words = old.encode_utf16().count();
    let mut p = at + needle.len();
    while p + 1 < buf.len() && buf[p] == 0 && buf[p + 1] == 0 {
        cap_words += 1;
        p += 2;
    }
    let new_b = utf16(new);
    if new_b.len() / 2 > cap_words - 1 {
        return false;
    }
    for b in buf.iter_mut().skip(at).take(cap_words * 2) {
        *b = 0;
    }
    buf[at..at + new_b.len()].copy_from_slice(&new_b);
    true
}

fn u16_at(b: &[u8], i: usize) -> Option<u16> {
    Some(u16::from_le_bytes(b.get(i..i + 2)?.try_into().ok()?))
}
fn u32_at(b: &[u8], i: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(i..i + 4)?.try_into().ok()?))
}

/// The file offset of a named export's data in a PE image, and how many bytes of its section's file data follow it.
pub fn export_room(buf: &[u8], name: &str) -> Option<(usize, usize)> {
    let pe = u32_at(buf, 0x3C)? as usize;
    if u32_at(buf, pe)? != 0x4550 {
        return None; // "PE\0\0"
    }
    let sections_n = u16_at(buf, pe + 6)? as usize;
    let opt_size = u16_at(buf, pe + 20)? as usize;
    let opt = pe + 24;
    let data_dirs = opt + if u16_at(buf, opt)? == 0x20B { 112 } else { 96 };
    let sections = opt + opt_size;
    let to_offset = |rva: u32| -> Option<(usize, usize)> {
        (0..sections_n).find_map(|s| {
            let sec = sections + s * 40;
            let (vsize, va, raw_size, raw_ptr) =
                (u32_at(buf, sec + 8)?, u32_at(buf, sec + 12)?, u32_at(buf, sec + 16)?, u32_at(buf, sec + 20)?);
            (rva >= va && rva < va + vsize.max(raw_size) && rva - va < raw_size)
                .then(|| ((rva - va + raw_ptr) as usize, (raw_size - (rva - va)) as usize))
        })
    };
    let exp = to_offset(u32_at(buf, data_dirs)?)?.0;
    let names_n = u32_at(buf, exp + 24)? as usize;
    let funcs = to_offset(u32_at(buf, exp + 28)?)?.0;
    let names = to_offset(u32_at(buf, exp + 32)?)?.0;
    let ords = to_offset(u32_at(buf, exp + 36)?)?.0;
    (0..names_n).find_map(|i| {
        let p = to_offset(u32_at(buf, names + 4 * i)?)?.0;
        let end = p + buf.get(p..)?.iter().position(|&b| b == 0)?;
        if &buf[p..end] != name.as_bytes() {
            return None;
        }
        let ord = u16_at(buf, ords + 2 * i)? as usize;
        to_offset(u32_at(buf, funcs + 4 * ord)?)
    })
}

/// The file offset of a named export's data, when `len` bytes of it lie in the file.
fn export_bytes(buf: &[u8], name: &str, len: usize) -> Option<usize> {
    export_room(buf, name).filter(|&(at, room)| room >= len && at + len <= buf.len()).map(|(at, _)| at)
}

/// Writes `value` into a `char[1024]` export (LevelDesc, MapName, TechtreeName), clearing the rest. False when the
/// export isn't there whole or `value` isn't ASCII or doesn't fit with its NUL.
pub fn write_text_export(buf: &mut [u8], name: &str, value: &str) -> bool {
    if !value.is_ascii() || value.len() >= TEXT_EXPORT_LEN {
        return false;
    }
    let Some(at) = export_bytes(buf, name, TEXT_EXPORT_LEN) else { return false };
    buf[at..at + TEXT_EXPORT_LEN].fill(0);
    buf[at..at + value.len()].copy_from_slice(value.as_bytes());
    true
}

/// Writes little-endian ints at the start of a named export. False when the export isn't there whole.
pub fn write_int_export(buf: &mut [u8], name: &str, values: &[i32]) -> bool {
    let Some(at) = export_bytes(buf, name, 4 * values.len()) else { return false };
    for (k, v) in values.iter().enumerate() {
        buf[at + 4 * k..at + 4 * k + 4].copy_from_slice(&v.to_le_bytes());
    }
    true
}

/// A DLL's version resource value `key` (FileVersion, ...), read from its UTF-16 key and the value after it.
pub fn version_string(buf: &[u8], key: &str) -> Option<String> {
    let mut needle = utf16(key);
    needle.extend([0, 0]);
    let at = find(buf, &needle, 0)?;
    let mut p = at + needle.len();
    p = (p + 3) & !3; // the value is DWORD-aligned
    let mut units = Vec::new();
    while let Some(u) = u16_at(buf, p).filter(|&u| u != 0) {
        units.push(u);
        p += 2;
    }
    Some(String::from_utf16_lossy(&units))
}

/// The PE header's link time (seconds since 1970), which a copy keeps.
pub fn link_time(buf: &[u8]) -> Option<u32> {
    u32_at(buf, u32_at(buf, 0x3C)? as usize + 8)
}

#[cfg(test)]
mod tests {
    use super::*;
    use op2_formats::mission::{MissionDll, MissionType};

    #[test]
    fn stub_writes_read_back() {
        let mut dll = STUB.to_vec();
        let before = MissionDll::parse(&dll).unwrap();
        assert_eq!((before.level_desc.as_str(), before.map_name.as_str()), (STUB_NAME, STUB_MAP));
        assert_eq!(before.techtree_name, STUB_TECH);
        assert!(write_text_export(&mut dll, "LevelDesc", "Iron Front"));
        assert!(write_text_export(&mut dll, "MapName", "newworld.map"));
        assert!(write_text_export(&mut dll, "TechtreeName", "Q01.tek"));
        assert!(patch_wide_field(&mut dll, STUB_DESC, "Iron Front"));
        let t = mission_type_value("MultiLastOneStanding").unwrap();
        assert!(write_int_export(&mut dll, "DescBlock", &[t, 4, 10, 1]));
        assert!(write_int_export(&mut dll, "DescBlockEx", &[2]));
        let after = MissionDll::parse(&dll).unwrap();
        assert_eq!((after.level_desc.as_str(), after.map_name.as_str()), ("Iron Front", "newworld.map"));
        assert_eq!(after.techtree_name, "Q01.tek");
        assert_eq!((after.mission_type, after.players, after.max_tech_level), (MissionType::LastOneStanding, 4, 10));
        assert!(after.unit_only);
        assert_eq!(after.extra_ai_players, Some(2));
        assert_eq!(version_string(&dll, "FileDescription").as_deref(), Some("Iron Front"));
        assert_eq!(dll.len(), STUB.len());
        // A longer name, then a shorter one: nothing of the longer one is left.
        assert!(write_text_export(&mut dll, "LevelDesc", "A"));
        assert_eq!(MissionDll::parse(&dll).unwrap().level_desc, "A");
    }

    #[test]
    fn writes_refuse_what_doesnt_fit() {
        let mut dll = STUB.to_vec();
        assert!(!write_text_export(&mut dll, "NoSuchExport", "x"));
        assert!(!write_text_export(&mut dll, "LevelDesc", &"x".repeat(TEXT_EXPORT_LEN)));
        assert!(!write_text_export(&mut dll, "LevelDesc", "Café"));
        assert!(!patch_wide_field(&mut dll, STUB_DESC, &"x".repeat(500)));
        assert_eq!(dll, STUB);
    }

    #[test]
    fn mission_types() {
        assert_eq!(mission_type_value("colony"), Some(-1));
        assert_eq!(mission_type_value("LastOneStanding"), Some(-8));
        assert_eq!(mission_type_value("12"), Some(12));
        assert_eq!(mission_type_value("13"), None);
        assert!(is_multiplayer_type("MultiMidas") && !is_multiplayer_type("Tutorial"));
        for v in (-8..=-1).chain(1..=12) {
            assert_eq!(mission_type_name(v).and_then(|n| mission_type_value(&n)), Some(v));
        }
        assert_eq!(mission_type_name(0), None);
    }
}
