//! The editor window: menus, the map canvas, the side panel and the status bar.
//!
//! `EditorApp` owns everything open: the game's files, the map (`MapDoc`), the mission (`Mission`), the view and
//! the tools (`Editor`). Each frame `ui` draws the panels, then the windows (messages, questions, dialogs).
//! Anything that would drop unsaved edits goes through `guarded`, which asks first (`Ask::Unsaved`) and then
//! `run`s the `Action`. Settings that outlive a run are in `Settings`, kept in eframe's storage.

use std::collections::HashSet;
use std::path::{Path, PathBuf};

use eframe::egui::{self, RichText};
use op2_formats::GameFiles;
use op2_formats::mission::MissionDll;

use crate::celltype::{self, RenderMode};
use crate::editor::{Editor, Tool};
use crate::launch::{self, Deploy};
use crate::mapdoc::MapDoc;
use crate::mission::Mission;
use crate::props::{self, Props};
use crate::sound::Sounds;
use crate::view::View;

/// Kept between runs (eframe's storage).
#[derive(serde::Serialize, serde::Deserialize, Default)]
#[serde(default)]
struct Settings {
    /// The Outpost 2 folder (the one holding `OPU`).
    game_dir: Option<PathBuf>,
    /// The last map opened without a mission, reopened at start.
    last_map: Option<String>,
    /// The last mission opened (its placement file), reopened at start.
    last_mission: Option<PathBuf>,
    mode: Option<RenderMode>,
    show_grid: bool,
    tool: Option<Tool>,
}

const SETTINGS_KEY: &str = "op2luaeditor-settings";

/// An Outpost 2 folder holds OPU (OPU 1.4) or the game's .vol archives.
fn is_game_dir(dir: &Path) -> bool {
    dir.join("OPU").is_dir()
        || std::fs::read_dir(dir).is_ok_and(|entries| {
            entries.flatten().any(|e| e.path().extension().is_some_and(|x| x.eq_ignore_ascii_case("vol")))
        })
}

/// Something that would drop unsaved edits, held until the user answers.
#[derive(Clone)]
enum Action {
    OpenMission(PathBuf),
    OpenMap(String),
    CloseMission,
    TestInOp2,
    Exit,
}

/// A question waiting for an answer.
enum Ask {
    /// The mission has unsaved edits: save them before `Action`?
    Unsaved(Action),
    /// The placement file changed on disk and there are edits here too (the disk's text).
    DiskChanged(String),
    /// Test in OP2 would replace files in the game that aren't the editor's: go ahead?
    Replace(Deploy, String),
}

/// The Mission Properties / New Mission window.
struct PropsDialog {
    props: Props,
    /// The open mission's properties when editing them; None for New Mission.
    original: Option<Props>,
    overwrite: bool,
    error: Option<String>,
    /// Where the mission's DLL differs from these properties (OK writes them into it).
    note: Option<String>,
}

/// The whole application's state.
pub struct EditorApp {
    settings: Settings,
    files: Option<GameFiles>,
    doc: Option<MapDoc>,
    mission: Option<Mission>,
    editor: Editor,
    view: View,
    hovered: Option<(u32, u32)>,
    selected: Option<(u32, u32)>,
    /// A message shown in a window until dismissed.
    message: Option<(String, String)>,
    /// A line in the status bar (the last save, ...).
    status: String,
    ask: Option<Ask>,
    /// Set once the user agreed to close with unsaved edits (or saved them).
    may_close: bool,
    settings_open: bool,
    settings_dir_text: String,
    map_list: Option<MapList>,
    about_open: bool,
    props_dialog: Option<PropsDialog>,
    sounds: Sounds,
    /// Files Test in OP2 has copied into the game this session.
    deployed: HashSet<PathBuf>,
    /// The game's OP2LuaCore.dll for the status bar, and whether there is one.
    core: (String, bool),
    /// The About box's banner, made the first time it shows.
    banner: Option<egui::TextureHandle>,
}

const BANNER: &[u8] = include_bytes!("../assets/banner.png");

/// The "Open map from game" window.
struct MapList {
    names: Vec<String>,
    filter: String,
}

fn is_lua(p: &Path) -> bool {
    p.extension().is_some_and(|e| e.eq_ignore_ascii_case("lua"))
}

impl EditorApp {
    /// Restores the settings, finds the game, and opens the mission or map named on the command line (else the
    /// last one open).
    pub fn new(cc: &eframe::CreationContext<'_>) -> Self {
        let settings: Settings = cc.storage.and_then(|s| eframe::get_value(s, SETTINGS_KEY)).unwrap_or_default();
        let mut view = View::default();
        view.mode = settings.mode.unwrap_or(RenderMode::GameMap);
        view.show_grid = settings.show_grid;
        let mut editor = Editor::default();
        editor.tool = settings.tool.unwrap_or(Tool::Select);
        let mut app = Self {
            settings_dir_text: String::new(),
            settings,
            files: None,
            doc: None,
            mission: None,
            editor,
            view,
            hovered: None,
            selected: None,
            message: None,
            status: String::new(),
            ask: None,
            may_close: false,
            settings_open: false,
            map_list: None,
            about_open: false,
            props_dialog: None,
            sounds: Sounds::default(),
            deployed: HashSet::new(),
            core: (String::new(), false),
            banner: None,
        };
        app.open_game_files();
        // A mission or map named on the command line, else the last one opened.
        let ctx = &cc.egui_ctx;
        match std::env::args().nth(1) {
            Some(arg) if is_lua(Path::new(&arg)) => app.open_mission(ctx, Path::new(&arg), true),
            Some(arg) => app.open_map(ctx, &arg, true),
            None => {
                if let Some(p) = app.settings.last_mission.clone() {
                    app.open_mission(ctx, &p, false);
                } else if let Some(name) = app.settings.last_map.clone() {
                    app.open_map(ctx, &name, false);
                }
            }
        }
        app
    }

    /// Opens the game folder from the settings, or finds an install.
    fn open_game_files(&mut self) {
        let dir = self.settings.game_dir.clone().unwrap_or_else(op2_formats::default_game_dir);
        let result = if is_game_dir(&dir) {
            GameFiles::open(&dir).map_err(|e| e.to_string())
        } else {
            Err("it has no OPU folder and no .vol files".to_string())
        };
        match result {
            Ok(files) => {
                // Footprints and which structures have borders and tubes, from the game's own building.txt.
                if let Ok((data, _)) = files.read("building.txt") {
                    crate::units::load_building_sheet(&String::from_utf8_lossy(&data));
                }
                self.core = launch::core_info(&dir);
                self.settings.game_dir = Some(dir);
                self.files = Some(files);
            }
            Err(e) => {
                self.files = None;
                self.message = Some((
                    "Outpost 2 folder".into(),
                    format!("{} isn't an Outpost 2 folder: {e}.\n\nChoose the folder that holds OPU.", dir.display()),
                ));
                self.show_settings();
            }
        }
    }

    fn show_settings(&mut self) {
        self.settings_dir_text = self.settings.game_dir.as_ref().map(|p| p.display().to_string()).unwrap_or_default();
        self.settings_open = true;
    }

    fn load_map(&mut self, ctx: &egui::Context, name: &str) -> Result<(), String> {
        let files = self.files.as_ref().ok_or("Set the Outpost 2 folder first (File > Settings).")?;
        let doc = MapDoc::load(ctx, files, name)?;
        if !doc.tileset_errors.is_empty() {
            self.message = Some((
                "Missing tilesets".into(),
                format!("Some of the map's tilesets couldn't be read:\n{}", doc.tileset_errors.join("\n")),
            ));
        }
        if doc.corrupt_group_names > 0 {
            self.status = format!(
                "Note: {} of {}'s tile group names hold stray bytes (the map still works).",
                doc.corrupt_group_names, doc.name
            );
        }
        self.doc = Some(doc);
        self.selected = None;
        self.view.center_pending = true;
        self.sounds.map_loaded();
        Ok(())
    }

    /// Opens a map on its own (closing any mission).
    fn open_map(&mut self, ctx: &egui::Context, name: &str, report: bool) {
        match self.load_map(ctx, name) {
            Ok(()) => {
                self.mission = None;
                self.editor.reset();
                self.settings.last_map = Some(name.to_string());
                self.settings.last_mission = None;
            }
            Err(e) if report => self.message = Some(("Open map".into(), format!("Can't open {name}:\n{e}"))),
            Err(_) => {}
        }
    }

    /// Opens a mission from either of its files, and its map.
    fn open_mission(&mut self, ctx: &egui::Context, picked: &Path, report: bool) {
        let result = Mission::open(picked).and_then(|m| {
            let map = m.map_to_open();
            self.load_map(ctx, &map).map_err(|e| format!("Can't open the mission's map {}:\n{e}", m.map_name))?;
            Ok(m)
        });
        match result {
            Ok(m) => {
                self.status = format!(
                    "Opened {}: {} unit(s), {} region(s), map {}.",
                    m.name,
                    m.units.units.len(),
                    m.regions.len(),
                    m.map_name
                );
                if let Some(note) = props::dll_note(&m.placement_path, &m.loaded_text) {
                    self.status += &format!(" {note}");
                }
                self.settings.last_mission = Some(m.placement_path.clone());
                self.settings.last_map = None;
                self.mission = Some(m);
                self.editor.reset();
            }
            Err(e) if report => self.message = Some(("Open mission".into(), e)),
            Err(_) => {}
        }
    }

    fn unsaved(&self) -> bool {
        self.mission.as_ref().is_some_and(|m| m.has_unsaved_edits())
    }

    /// Runs `action`, first asking about unsaved edits.
    fn guarded(&mut self, ctx: &egui::Context, action: Action) {
        if self.unsaved() {
            self.ask = Some(Ask::Unsaved(action));
        } else {
            self.run(ctx, action);
        }
    }

    fn run(&mut self, ctx: &egui::Context, action: Action) {
        match action {
            Action::OpenMission(p) => self.open_mission(ctx, &p, true),
            Action::OpenMap(name) => self.open_map(ctx, &name, true),
            Action::CloseMission => {
                self.mission = None;
                self.editor.reset();
                self.settings.last_mission = None;
                self.settings.last_map = self.doc.as_ref().map(|d| d.source.clone());
            }
            Action::TestInOp2 => self.test_in_op2(),
            Action::Exit => {
                self.may_close = true;
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            }
        }
    }

    /// Saves the open mission, first checking the file wasn't changed on disk under the editor.
    fn save(&mut self) -> bool {
        let Some(m) = &mut self.mission else { return false };
        if let Some(disk) = m.changed_on_disk() {
            if m.has_unsaved_edits() {
                self.ask = Some(Ask::DiskChanged(disk));
                return false;
            }
            m.load_text(disk);
            self.editor.reset();
            self.status = format!("{} changed on disk; reloaded it (there were no edits here).", m.name);
            return true;
        }
        self.write()
    }

    fn write(&mut self) -> bool {
        let Some(m) = &mut self.mission else { return false };
        match m.save() {
            Ok(summary) => {
                // The DLL is built from the placement's header each time, so it holds what the editor shows.
                match props::save_dll(&m.placement_path, &m.loaded_text) {
                    Ok(dll) => self.status = format!("{summary} {dll}"),
                    Err(e) => {
                        self.status = summary;
                        self.message = Some(("Save mission".into(), format!("The placement was saved, but: {e}")));
                    }
                }
                true
            }
            Err(e) => {
                self.message = Some(("Save mission".into(), e));
                false
            }
        }
    }

    fn save_as(&mut self) {
        let Some(m) = &mut self.mission else { return };
        let dir = m.placement_path.parent().map(Path::to_path_buf);
        let mut dialog = rfd::FileDialog::new()
            .add_filter("OP2Lua placement", &["lua"])
            .set_file_name(m.placement_path.file_name().unwrap_or_default().to_string_lossy());
        if let Some(d) = dir {
            dialog = dialog.set_directory(d);
        }
        if let Some(path) = dialog.save_file() {
            m.placement_path = crate::mission::placement_for(&path);
            // The mission's logic comes along under the new name, so the copy is a whole mission with its DLL.
            let script = m.placement_path.with_file_name(format!("{}.lua", crate::mission::base_name(&m.placement_path)));
            if let Some(old) = m.script_path.clone().filter(|old| *old != script && !script.exists())
                && let Err(e) = std::fs::copy(&old, &script)
            {
                self.message = Some(("Save mission".into(), format!("Can't write {}: {e}", script.display())));
                return;
            }
            m.script_path = script.is_file().then_some(script);
            if !m.placement_path.exists() {
                // A new file starts from the open one's text, so its other tables come along.
                if let Err(e) = std::fs::write(&m.placement_path, &m.loaded_text) {
                    self.message = Some(("Save mission".into(), format!("Can't write {}: {e}", m.placement_path.display())));
                    return;
                }
            } else if let Ok(text) = std::fs::read_to_string(&m.placement_path) {
                m.loaded_text = text;
            }
            self.settings.last_mission = Some(m.placement_path.clone());
            self.write();
        }
    }

    /// Test in OP2: the mission's DLL, script and placement into the game, then Outpost 2 straight into it.
    fn test_in_op2(&mut self) {
        let (Some(m), Some(game)) = (&self.mission, self.settings.game_dir.clone()) else { return };
        let Some(dll_path) = props::mission_dll(&m.placement_path) else {
            let base = crate::mission::base_name(&m.placement_path);
            self.message = Some((
                "Test in OP2".into(),
                format!(
                    "There is no c{base}.dll or {base}.dll beside the placement file. Make the mission with New mission so it has its DLL."
                ),
            ));
            return;
        };
        let dll_name = dll_path.file_name().unwrap_or_default().to_string_lossy().into_owned();
        let plan = launch::plan_deploy(&game, &dll_path, &crate::mission::base_name(&m.placement_path), &self.deployed);
        if plan.replaces.is_empty() {
            self.deploy_and_launch(&plan, &dll_name);
        } else {
            self.ask = Some(Ask::Replace(plan, dll_name));
        }
    }

    fn deploy_and_launch(&mut self, plan: &Deploy, dll_name: &str) {
        let Some(game) = self.settings.game_dir.clone() else { return };
        let result = launch::deploy(plan, &mut self.deployed).and_then(|()| launch::launch(&game, dll_name));
        match result {
            Ok(()) => self.status = format!("Started Outpost 2 with {dll_name}."),
            Err(e) => self.message = Some(("Test in OP2".into(), e)),
        }
    }

    fn edit_properties(&mut self) {
        let Some(m) = &self.mission else { return };
        let text = std::fs::read_to_string(&m.placement_path).unwrap_or_else(|_| m.loaded_text.clone());
        let dll = props::mission_dll(&m.placement_path).and_then(|p| MissionDll::open(p).ok());
        let p = Props::read_with_dll(&text, dll.as_ref());
        let note = props::dll_note(&m.placement_path, &text);
        self.props_dialog = Some(PropsDialog { props: p.clone(), original: Some(p), overwrite: false, error: None, note });
    }

    /// Writes changed properties into the open mission's placement file and DLL; reopens the map when it changed.
    fn apply_properties(&mut self, ctx: &egui::Context, old: &Props, new: &Props) -> Result<(), String> {
        let m = self.mission.as_mut().ok_or("No mission is open.")?;
        let path = m.placement_path.clone();
        let disk = std::fs::read_to_string(&path).map_err(|e| format!("Can't read {}: {e}", path.display()))?;
        let updated = new.write_into(&disk);
        std::fs::copy(&path, format!("{}.bak", path.display()))
            .map_err(|e| format!("Can't back up {}: {e}", path.display()))?;
        std::fs::write(&path, &updated).map_err(|e| format!("Can't write {}: {e}", path.display()))?;
        if m.loaded_text == disk {
            m.loaded_text = updated.clone(); // the edits here stay edits against the new text
        }
        m.name = new.name.clone();
        m.map_name = new.map.clone();
        let mut summary = format!("Saved the properties to {}.", path.file_name().unwrap_or_default().to_string_lossy());
        summary += &format!(" {}", props::save_dll(&path, &updated)?);
        if new.map != old.map {
            let map = self.mission.as_ref().map(|m| m.map_to_open()).unwrap_or_default();
            if let Err(e) = self.load_map(ctx, &map) {
                summary += &format!(" (Can't show {}: {e})", new.map);
            }
        }
        self.status = summary;
        Ok(())
    }

    fn props_window(&mut self, ctx: &egui::Context) {
        let game_dir = self.settings.game_dir.clone();
        let Some(d) = &mut self.props_dialog else { return };
        let title = if d.original.is_some() { "Mission properties" } else { "New mission" };
        let mut answer = None;
        let new_files = game_dir.as_ref().and_then(|g| props::new_files(&launch::deploy_dir(g), &d.props));
        egui::Window::new(title).collapsible(false).resizable(false).show(ctx, |ui| {
            egui::Grid::new("props").num_columns(2).show(ui, |ui| {
                ui.label("Name");
                ui.text_edit_singleline(&mut d.props.name);
                ui.end_row();
                ui.label("Map");
                ui.horizontal(|ui| {
                    ui.text_edit_singleline(&mut d.props.map);
                    if ui.button("Choose...").clicked() {
                        let mut dialog = rfd::FileDialog::new().add_filter("Outpost 2 map", &["map"]);
                        if let Some(dir) = game_dir.as_ref().map(|g| g.join("OPU")).filter(|p| p.is_dir()) {
                            dialog = dialog.set_directory(dir);
                        }
                        if let Some(p) = dialog.pick_file() {
                            d.props.map = p.file_name().unwrap_or_default().to_string_lossy().into_owned();
                        }
                    }
                });
                ui.end_row();
                ui.label("Tech tree file");
                ui.text_edit_singleline(&mut d.props.tech);
                ui.end_row();
                ui.label("Type");
                egui::ComboBox::from_id_salt("props-type").selected_text(d.props.mission_type.as_str()).show_ui(ui, |ui| {
                    for t in crate::dll::MISSION_TYPES {
                        ui.selectable_value(&mut d.props.mission_type, t.to_string(), *t);
                    }
                });
                ui.end_row();
                ui.label("Max tech level");
                ui.add(egui::DragValue::new(&mut d.props.max_tech).range(0..=12));
                ui.end_row();
                ui.label("Players");
                ui.add(egui::DragValue::new(&mut d.props.players).range(1..=6));
                ui.end_row();
                ui.label("Unit-only mission");
                ui.checkbox(&mut d.props.unit_only, "most reports off (no colony to manage)");
                ui.end_row();
                let multiplayer = crate::dll::is_multiplayer_type(&d.props.mission_type);
                ui.label("AI players");
                ui.add_enabled(multiplayer, egui::DragValue::new(&mut d.props.ai_players).range(0..=5))
                    .on_disabled_hover_text("Computer players a multiplayer game adds (DescBlockEx); for multiplayer types.");
                ui.end_row();
            });
            ui.small("These go into the mission DLL, which Save mission writes with the placement.");
            if let Some(note) = &d.note {
                ui.colored_label(egui::Color32::from_rgb(200, 140, 0), note);
            }
            if d.original.is_none() {
                match &new_files {
                    Some(f) => {
                        let name = |p: &PathBuf| p.file_name().unwrap_or_default().to_string_lossy().into_owned();
                        ui.small(format!(
                            "Writes {}, {} and {} into {}.",
                            name(&f.dll),
                            name(&f.script),
                            name(&f.placement),
                            f.dll.parent().map(|p| p.display().to_string()).unwrap_or_default()
                        ));
                        if [&f.dll, &f.script, &f.placement].iter().any(|p| p.exists()) {
                            ui.checkbox(&mut d.overwrite, "A mission of that name exists there: overwrite its files");
                        }
                    }
                    None => {
                        ui.small("The name needs a letter or digit, and the Outpost 2 folder must be set.");
                    }
                }
            }
            if let Some(e) = &d.error {
                ui.colored_label(egui::Color32::RED, e);
            }
            ui.horizontal(|ui| {
                if ui.button(if d.original.is_some() { "OK" } else { "Create" }).clicked() {
                    answer = Some(true);
                }
                if ui.button("Cancel").clicked() {
                    answer = Some(false);
                }
            });
        });
        match answer {
            Some(false) => self.props_dialog = None,
            Some(true) => {
                let d = self.props_dialog.as_mut().unwrap();
                d.props.name = d.props.name.trim().to_string();
                d.props.map = d.props.map.trim().to_string();
                if let Some(problem) = d.props.problem() {
                    d.error = Some(problem);
                    return;
                }
                let (props, original, overwrite, dll_differs) = (d.props.clone(), d.original.clone(), d.overwrite, d.note.is_some());
                let result = match original {
                    Some(old) if old == props && !dll_differs => Ok(()),
                    Some(old) => self.apply_properties(ctx, &old, &props),
                    None => match new_files {
                        None => Err("The name needs a letter or digit, and the Outpost 2 folder must be set.".to_string()),
                        Some(f) if !overwrite && [&f.dll, &f.script, &f.placement].iter().any(|p| p.exists()) => {
                            Err("A mission of that name exists there: tick overwrite, or choose another name.".to_string())
                        }
                        Some(f) => props::create(&f, &props).map(|()| {
                            self.open_mission(ctx, &f.placement, true);
                            self.status = format!(
                                "Created {} in {}.",
                                props.name,
                                f.dll.parent().map(|p| p.display().to_string()).unwrap_or_default()
                            );
                        }),
                    },
                };
                match result {
                    Ok(()) => self.props_dialog = None,
                    Err(e) => {
                        if let Some(d) = &mut self.props_dialog {
                            d.error = Some(e);
                        }
                    }
                }
            }
            None => {}
        }
    }

    fn missions_dir(&self) -> Option<PathBuf> {
        let root = self.settings.game_dir.as_ref()?;
        [root.join("OPU").join("maps").join("LUA"), root.clone()].into_iter().find(|p| p.is_dir())
    }

    fn maps_dir(&self) -> Option<PathBuf> {
        let root = self.settings.game_dir.as_ref()?;
        [root.join("OPU").join("base").join("maps"), root.join("OPU").join("maps"), root.clone()]
            .into_iter()
            .find(|p| p.is_dir())
    }

    fn pick_mission(&mut self, ctx: &egui::Context) {
        let mut dialog = rfd::FileDialog::new()
            .add_filter("OP2Lua mission", &["lua"])
            .set_title("Open mission (its .lua or .placement.lua)");
        if let Some(dir) = self.missions_dir() {
            dialog = dialog.set_directory(dir);
        }
        if let Some(path) = dialog.pick_file() {
            self.guarded(ctx, Action::OpenMission(path));
        }
    }

    fn pick_map_file(&mut self, ctx: &egui::Context) {
        let mut dialog = rfd::FileDialog::new().add_filter("Outpost 2 map", &["map"]).set_title("Open map");
        if let Some(dir) = self.maps_dir() {
            dialog = dialog.set_directory(dir);
        }
        if let Some(path) = dialog.pick_file() {
            self.guarded(ctx, Action::OpenMap(path.to_string_lossy().into_owned()));
        }
    }

    fn export_image(&mut self, px: u32) {
        let Some(doc) = &self.doc else { return };
        let stem = Path::new(&doc.name).file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
        let Some(path) =
            rfd::FileDialog::new().add_filter("PNG image", &["png"]).set_file_name(format!("{stem}.png")).save_file()
        else {
            return;
        };
        let layers = crate::export::Layers { units: self.editor.show_units, objects: self.editor.show_objects };
        let img = crate::export::map_image(&doc.map, &doc.tilesets, px, self.mission.as_ref(), layers);
        if let Err(e) = image::save_buffer(&path, &img.rgba, img.width, img.height, image::ColorType::Rgba8) {
            self.message = Some(("Export image".into(), format!("Can't write {}:\n{e}", path.display())));
        }
    }

    fn save_map_as(&mut self) {
        let Some(doc) = &self.doc else { return };
        let Some(path) = rfd::FileDialog::new()
            .add_filter("Outpost 2 map", &["map"])
            .set_file_name(doc.name.clone())
            .set_title("Save map as")
            .save_file()
        else {
            return;
        };
        match std::fs::write(&path, doc.map.write()) {
            Ok(()) => self.status = format!("Saved the map as {}.", path.display()),
            Err(e) => self.message = Some(("Save map".into(), format!("Can't write {}:\n{e}", path.display()))),
        }
    }

    fn pick_placement(&self, title: &str) -> Option<PathBuf> {
        let mut dialog = rfd::FileDialog::new().add_filter("OP2Lua placement", &["lua"]).set_title(title);
        if let Some(dir) = self.mission.as_ref().and_then(|m| m.placement_path.parent()).map(Path::to_path_buf) {
            dialog = dialog.set_directory(dir);
        }
        dialog.pick_file().map(|p| crate::mission::placement_for(&p))
    }

    fn import_regions(&mut self) {
        let Some(path) = self.pick_placement("Import regions from a placement.lua") else { return };
        let Some(m) = &mut self.mission else { return };
        let before = m.regions.len();
        match m.import_regions(&path) {
            Ok(n) => {
                self.editor.selected_region = None;
                self.status = format!(
                    "Imported {n} region(s) from {} (replacing {before}); save to keep them.",
                    path.file_name().unwrap_or_default().to_string_lossy()
                );
            }
            Err(e) => self.message = Some(("Import regions".into(), e)),
        }
    }

    fn write_table_into(&mut self, units: bool) {
        let title = if units { "Save the units into a placement.lua" } else { "Save the regions into a placement.lua" };
        let Some(path) = self.pick_placement(title) else { return };
        let Some(m) = &self.mission else { return };
        if path == m.placement_path {
            self.message = Some(("Save into".into(), "That is the open mission's own file: use Save mission.".into()));
            return;
        }
        match m.write_table_into(&path, units) {
            Ok(s) => self.status = s,
            Err(e) => self.message = Some(("Save into".into(), e)),
        }
    }

    fn menu_bar(&mut self, ui: &mut egui::Ui) {
        let ctx = ui.ctx().clone();
        let has_mission = self.mission.is_some();
        egui::MenuBar::new().ui(ui, |ui| {
            ui.menu_button("File", |ui| {
                if ui.button("Open mission...").clicked() {
                    self.pick_mission(&ctx);
                }
                if ui.add_enabled(has_mission, egui::Button::new("Save mission").shortcut_text("Ctrl+S")).clicked() {
                    self.save();
                }
                if ui.add_enabled(has_mission, egui::Button::new("Save mission as...")).clicked() {
                    self.save_as();
                }
                if ui.add_enabled(has_mission, egui::Button::new("Close mission")).clicked() {
                    self.guarded(&ctx, Action::CloseMission);
                }
                ui.separator();
                if ui.button("Open map file...").clicked() {
                    self.pick_map_file(&ctx);
                }
                if ui.button("Open map from the game...").clicked() {
                    let names = self.files.as_ref().map(|f| f.map_names()).unwrap_or_default();
                    self.map_list = Some(MapList { names, filter: String::new() });
                }
                if ui.add_enabled(self.doc.is_some(), egui::Button::new("Save map as...")).clicked() {
                    self.save_map_as();
                }
                ui.add_enabled_ui(self.doc.is_some(), |ui| {
                    let title = if has_mission { "Export map image (with the mission)" } else { "Export map image" };
                    ui.menu_button(title, |ui| {
                        if ui.button("Full size (32 px per tile)").clicked() {
                            self.export_image(32);
                        }
                        if ui.button("1/8 size (4 px per tile)").clicked() {
                            self.export_image(4);
                        }
                    });
                });
                ui.separator();
                if ui.button("Settings...").clicked() {
                    self.show_settings();
                }
                ui.separator();
                if ui.button("Exit").clicked() {
                    ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                }
            });
            ui.menu_button("Mission", |ui| {
                if ui.add_enabled(has_mission, egui::Button::new("Properties...")).clicked() {
                    self.edit_properties();
                }
                if ui.button("New mission...").clicked() {
                    self.props_dialog = Some(PropsDialog { props: Props::default(), original: None, overwrite: false, error: None, note: None });
                }
                ui.separator();
                if ui.add_enabled(has_mission, egui::Button::new("Import regions from...")).clicked() {
                    self.import_regions();
                }
                if ui.add_enabled(has_mission, egui::Button::new("Save regions into...")).clicked() {
                    self.write_table_into(false);
                }
                if ui.add_enabled(has_mission, egui::Button::new("Save units into...")).clicked() {
                    self.write_table_into(true);
                }
                if ui.add_enabled(has_mission, egui::Button::new("Clear all units...")).clicked() {
                    self.editor.confirm_clear = Some(crate::editor::Clear::Units);
                }
                if ui.add_enabled(has_mission, egui::Button::new("Clear all regions...")).clicked() {
                    self.editor.confirm_clear = Some(crate::editor::Clear::Regions);
                }
                ui.separator();
                if ui.add_enabled(has_mission, egui::Button::new("Test in OP2").shortcut_text("F5")).clicked() {
                    self.guarded(&ctx, Action::TestInOp2);
                }
            });
            ui.menu_button("View", |ui| {
                for mode in RenderMode::ALL {
                    if ui.radio(self.view.mode == mode, mode.label()).clicked() {
                        self.view.mode = mode;
                        self.settings.mode = Some(mode);
                    }
                }
                ui.separator();
                if ui.checkbox(&mut self.view.show_grid, "Grid").changed() {
                    self.settings.show_grid = self.view.show_grid;
                }
                ui.checkbox(&mut self.editor.show_units, "Units");
                ui.checkbox(&mut self.editor.show_objects, "Beacons, walls and markers");
                ui.separator();
                ui.add_enabled_ui(self.doc.is_some(), |ui| {
                    if let Some(doc) = &self.doc {
                        if ui.button("Zoom to fit").clicked() {
                            self.view.fit(doc);
                        }
                        if ui.button("Zoom 100%").clicked() {
                            self.view.reset_zoom(doc);
                        }
                        if ui.button("Centre map").clicked() {
                            self.view.center(doc);
                        }
                    }
                });
            });
            ui.menu_button("Help", |ui| {
                if ui.button("About OP2LuaEditor").clicked() {
                    self.about_open = true;
                }
            });
        });
    }

    fn side_panel(&mut self, ui: &mut egui::Ui) {
        let Some(doc) = &self.doc else {
            ui.label("No map open.");
            return;
        };
        if let Some(m) = &mut self.mission {
            self.editor.panel(ui, m);
            ui.separator();
        }
        egui::CollapsingHeader::new(RichText::new(format!("Map: {}", doc.name)).strong())
            .default_open(self.mission.is_none())
            .show(ui, |ui| {
                ui.label(format!("{} x {} tiles{}", doc.width(), doc.height(), if doc.map.wraps() { ", wraps" } else { "" }));
                ui.small(&doc.source);
                for (i, ts) in doc.map.tilesets.iter().enumerate() {
                    if let Some(ts) = ts {
                        let ok = doc.atlases.get(i).is_some_and(|a| a.is_some());
                        let text = format!("{i}: {} ({} tiles)", ts.name, ts.tile_count);
                        ui.label(if ok { RichText::new(text) } else { RichText::new(text + " - missing").color(egui::Color32::RED) });
                    }
                }
            });
        ui.separator();
        ui.label(RichText::new("Selected tile").strong());
        let Some((x, y)) = self.selected else {
            ui.label("Click a tile.");
            return;
        };
        let tile = doc.tile(x, y);
        let pad = doc.map.x_padding();
        egui::Grid::new("tile").num_columns(2).show(ui, |ui| {
            ui.label("Position");
            ui.label(format!("{}, {}", x + 1, y + 1));
            ui.end_row();
            ui.label("Game tile");
            ui.label(format!("{}, {}", x as i32 + pad, y));
            ui.end_row();
            ui.label("Cell type");
            ui.label(celltype::name(tile.cell_type));
            ui.end_row();
            ui.label("Mapping");
            ui.label(tile.mapping_index.to_string());
            ui.end_row();
            if let Some(m) = doc.map.mapping(tile) {
                ui.label("Tileset");
                ui.label(doc.map.tileset_name(m.tileset).unwrap_or("?"));
                ui.end_row();
                ui.label("Image");
                ui.label(m.graphic.to_string());
                ui.end_row();
                if m.animation_count > 0 {
                    ui.label("Animation");
                    ui.label(format!("{} frames, delay {}", m.animation_count, m.animation_delay));
                    ui.end_row();
                }
            }
            let flags: Vec<&str> = [
                (tile.lava_possible, "lava possible"),
                (tile.lava, "lava"),
                (tile.microbe, "Blight"),
                (tile.wall_or_building, "wall or building"),
            ]
            .into_iter()
            .filter_map(|(on, name)| on.then_some(name))
            .collect();
            if !flags.is_empty() {
                ui.label("Flags");
                ui.label(flags.join(", "));
                ui.end_row();
            }
        });
        if let Some(m) = &self.mission {
            for line in m.tile_contents(x as i32 + 1, y as i32 + 1) {
                ui.label(line);
            }
        }
    }

    fn status_bar(&self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            let pos = |t: Option<(u32, u32)>| t.map_or("-".to_string(), |(x, y)| format!("{}, {}", x + 1, y + 1));
            ui.label(format!("Cursor: {}", pos(self.hovered)));
            ui.separator();
            ui.label(format!("Selected: {}", pos(self.selected)));
            ui.separator();
            ui.label(format!("Zoom: {:.0}%", self.view.zoom * 100.0));
            ui.separator();
            ui.label(self.view.mode.label());
            if !self.status.is_empty() {
                ui.separator();
                ui.label(&self.status);
            }
            if let Some(dir) = &self.settings.game_dir {
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    let (text, ok) = &self.core;
                    let label = if *ok { RichText::new(text) } else { RichText::new(text).color(egui::Color32::RED) };
                    ui.label(label).on_hover_text(dir.join("OPU").join("OP2LuaCore.dll").display().to_string());
                });
            }
        });
    }

    fn windows(&mut self, ctx: &egui::Context) {
        if let Some((title, text)) = &self.message {
            let mut open = true;
            let mut ok = false;
            egui::Window::new(title.as_str()).collapsible(false).open(&mut open).show(ctx, |ui| {
                ui.label(text.as_str());
                ok = ui.button("OK").clicked();
            });
            if !open || ok {
                self.message = None;
            }
        }

        match self.ask.take() {
            Some(Ask::Unsaved(action)) => {
                let name = self.mission.as_ref().map(|m| m.name.clone()).unwrap_or_default();
                let mut answer = None;
                egui::Window::new("Unsaved changes").collapsible(false).resizable(false).show(ctx, |ui| {
                    ui.label(format!("{name} has unsaved changes. Save them first?"));
                    ui.horizontal(|ui| {
                        if ui.button("Save").clicked() {
                            answer = Some(1);
                        }
                        if ui.button("Don't save").clicked() {
                            answer = Some(2);
                        }
                        if ui.button("Cancel").clicked() {
                            answer = Some(0);
                        }
                    });
                });
                match answer {
                    Some(1) => {
                        if self.save() {
                            self.run(ctx, action);
                        }
                    }
                    Some(2) => self.run(ctx, action),
                    Some(_) => {}
                    None => self.ask = Some(Ask::Unsaved(action)),
                }
            }
            Some(Ask::DiskChanged(disk)) => {
                let mut answer = None;
                egui::Window::new("File changed").collapsible(false).resizable(false).show(ctx, |ui| {
                    ui.label("The placement file changed on disk since it was opened, and it has edits here too.");
                    ui.horizontal(|ui| {
                        if ui.button("Reload the file (drop the edits here)").clicked() {
                            answer = Some(1);
                        }
                        if ui.button("Keep the edits here (overwrite)").clicked() {
                            answer = Some(2);
                        }
                        if ui.button("Cancel").clicked() {
                            answer = Some(0);
                        }
                    });
                });
                match answer {
                    Some(1) => {
                        if let Some(m) = &mut self.mission {
                            m.load_text(disk);
                            self.editor.reset();
                        }
                    }
                    Some(2) => {
                        if let Some(m) = &mut self.mission {
                            m.loaded_text = disk;
                        }
                        self.write();
                    }
                    Some(_) => {}
                    None => self.ask = Some(Ask::DiskChanged(disk)),
                }
            }
            Some(Ask::Replace(plan, dll_name)) => {
                let mut answer = None;
                egui::Window::new("Test in OP2").collapsible(false).resizable(false).show(ctx, |ui| {
                    ui.label("Test in OP2 copies the mission into the game's OPU\\maps\\LUA. These files there are different and will be replaced:");
                    for n in &plan.replaces {
                        ui.monospace(format!("  {n}"));
                    }
                    ui.horizontal(|ui| {
                        if ui.button("Replace them").clicked() {
                            answer = Some(true);
                        }
                        if ui.button("Cancel").clicked() {
                            answer = Some(false);
                        }
                    });
                });
                match answer {
                    Some(true) => self.deploy_and_launch(&plan, &dll_name),
                    Some(false) => {}
                    None => self.ask = Some(Ask::Replace(plan, dll_name)),
                }
            }
            None => {}
        }
        self.props_window(ctx);

        if let Some(m) = &mut self.mission {
            self.editor.naming_window(ctx, m);
            self.editor.clear_window(ctx, m);
        }

        if self.settings_open {
            let mut open = true;
            let mut apply = false;
            egui::Window::new("Settings").collapsible(false).open(&mut open).show(ctx, |ui| {
                ui.label("Outpost 2 folder (the one holding OPU):");
                ui.horizontal(|ui| {
                    ui.add(egui::TextEdit::singleline(&mut self.settings_dir_text).desired_width(360.0));
                    if ui.button("Browse...").clicked()
                        && let Some(dir) = rfd::FileDialog::new().pick_folder()
                    {
                        self.settings_dir_text = dir.display().to_string();
                    }
                });
                apply = ui.button("Apply").clicked();
            });
            if apply {
                self.settings.game_dir =
                    (!self.settings_dir_text.trim().is_empty()).then(|| PathBuf::from(self.settings_dir_text.trim()));
                self.open_game_files();
                open = self.files.is_none();
            }
            self.settings_open = open;
        }

        let mut chosen = None;
        if let Some(list) = &mut self.map_list {
            let mut open = true;
            egui::Window::new("Open map from the game").open(&mut open).default_height(420.0).show(ctx, |ui| {
                ui.horizontal(|ui| {
                    ui.label("Filter:");
                    ui.text_edit_singleline(&mut list.filter);
                });
                let filter = list.filter.to_ascii_lowercase();
                egui::ScrollArea::vertical().show(ui, |ui| {
                    for name in list.names.iter().filter(|n| n.to_ascii_lowercase().contains(&filter)) {
                        if ui.selectable_label(false, name).double_clicked() {
                            chosen = Some(name.clone());
                        }
                    }
                });
            });
            if !open {
                self.map_list = None;
            }
        }
        if let Some(name) = chosen {
            self.map_list = None;
            self.guarded(ctx, Action::OpenMap(name));
        }

        if self.about_open {
            let banner = self.banner.get_or_insert_with(|| {
                let img = image::load_from_memory(BANNER).expect("the embedded banner").to_rgba8();
                let size = [img.width() as usize, img.height() as usize];
                ctx.load_texture("banner", egui::ColorImage::from_rgba_unmultiplied(size, img.as_raw()), Default::default())
            });
            let banner = egui::load::SizedTexture::from_handle(banner);
            egui::Window::new("About OP2LuaEditor").collapsible(false).resizable(false).open(&mut self.about_open).show(ctx, |ui| {
                ui.add(egui::Image::new(banner).max_width(620.0));
                ui.heading("OP2LuaEditor");
                ui.label(format!("Version {}", env!("CARGO_PKG_VERSION")));
                ui.label("A mission layout editor for OP2Lua missions: units, regions and mission properties.");
                ui.label("GPL-3.0-or-later. Reads the game's files with op2-formats.");
                ui.hyperlink("https://github.com/leviathan400/OP2LuaSDK");
                ui.separator();
                ui.label(RichText::new("Built with").strong());
                for lib in env!("OP2LUAEDITOR_LIBRARIES").split(';').filter(|l| !l.is_empty()) {
                    ui.small(lib);
                }
            });
        }
    }
}

impl eframe::App for EditorApp {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();

        // Closing the window with unsaved edits asks first.
        if ctx.input(|i| i.viewport().close_requested()) && !self.may_close && self.unsaved() {
            ctx.send_viewport_cmd(egui::ViewportCommand::CancelClose);
            if self.ask.is_none() {
                self.ask = Some(Ask::Unsaved(Action::Exit));
            }
        }

        // A mission's .lua or a .map dropped on the window opens.
        let dropped: Vec<PathBuf> = ctx.input(|i| i.raw.dropped_files.iter().map(|f| f.path().to_path_buf()).collect());
        if let Some(path) = dropped.into_iter().next() {
            if is_lua(&path) {
                self.guarded(&ctx, Action::OpenMission(path));
            } else if path.extension().is_some_and(|e| e.eq_ignore_ascii_case("map")) {
                self.guarded(&ctx, Action::OpenMap(path.to_string_lossy().into_owned()));
            }
        }

        // Ctrl+S saves; Delete removes the selected unit or region (unless a text field has the keyboard).
        if ctx.input_mut(|i| i.consume_key(egui::Modifiers::COMMAND, egui::Key::S)) {
            self.save();
        }
        if self.mission.is_some() && ctx.input_mut(|i| i.consume_key(egui::Modifiers::NONE, egui::Key::F5)) {
            self.guarded(&ctx, Action::TestInOp2);
        }
        let typing = ctx.memory(|m| m.focused().is_some());
        if !typing
            && ctx.input(|i| i.key_pressed(egui::Key::Delete))
            && let Some(m) = &mut self.mission
        {
            self.editor.delete_selected(m);
        }

        let title = match (&self.mission, &self.doc) {
            (Some(m), _) => format!("OP2LuaEditor - {}{}", m.name, if m.has_unsaved_edits() { " *" } else { "" }),
            (None, Some(doc)) => format!("OP2LuaEditor - {}", doc.name),
            _ => "OP2LuaEditor".to_string(),
        };
        ctx.send_viewport_cmd(egui::ViewportCommand::Title(title));

        egui::Panel::top("menu").show(ui, |ui| self.menu_bar(ui));
        if self.mission.is_some() {
            egui::Panel::top("tools").show(ui, |ui| {
                self.editor.toolbar(ui);
                self.settings.tool = Some(self.editor.tool);
            });
        }
        egui::Panel::bottom("status").show(ui, |ui| self.status_bar(ui));
        egui::Panel::right("side").resizable(true).default_size(300.0).min_size(240.0).show(ui, |ui| {
            egui::ScrollArea::vertical().show(ui, |ui| self.side_panel(ui));
        });
        egui::CentralPanel::no_frame().show(ui, |ui| match &mut self.doc {
            Some(doc) => {
                let (events, response) = self.view.show(ui, doc, self.selected);
                self.hovered = events.hovered;
                if events.clicked.is_some() {
                    self.selected = events.clicked;
                }
                if let Some(m) = &mut self.mission {
                    let (w, h) = (doc.width() as i32, doc.height() as i32);
                    self.editor.handle_input(ui, &response, &self.view, m, w, h);
                    let painter = ui.painter_at(self.view.canvas());
                    self.editor.draw(&painter, &self.view, m);
                }
            }
            None => {
                self.hovered = None;
                ui.centered_and_justified(|ui| {
                    ui.label("Open a mission (File > Open mission) or a map, or drop a mission's .lua or a .map here.");
                });
            }
        });

        self.windows(&ctx);
    }

    fn save(&mut self, storage: &mut dyn eframe::Storage) {
        eframe::set_value(storage, SETTINGS_KEY, &self.settings);
    }
}
