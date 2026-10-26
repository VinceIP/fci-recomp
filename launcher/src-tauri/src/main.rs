//! fci-recomp's launcher: builds the games from the player's disc with fci-recomp.py, keeps them up to date
//! with the releases on GitHub, and starts them. The page (../ui) calls the commands below.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod disc;
mod net;
mod paths;
mod report;
mod settings;
mod task;
mod titles;

use serde::Serialize;
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use tauri::{AppHandle, Emitter, Manager, State};
use tauri_plugin_dialog::DialogExt;
use tauri_plugin_opener::OpenerExt;

use task::Tasks;

type Discs = Mutex<HashMap<String, disc::DiscCheck>>;

#[derive(Serialize)]
struct TitleStatus {
    key: &'static str,
    name: &'static str,
    /// the disc's files are in the game's folder
    installed: bool,
    game: bool,
    editor: bool,
    /// the disc had the map editor (ige.xbe is among the installed files)
    editor_available: bool,
    built_version: Option<String>,
    /// built with an older version than the source here
    outdated: bool,
    disc: Option<disc::DiscCheck>,
    running: Vec<String>,
    folder: String,
}

#[derive(Serialize)]
struct Overview {
    launcher_version: &'static str,
    root: String,
    warnings: Vec<String>,
    /// C:\Games\fci-recomp, when the launcher can move itself there (see move_launcher)
    move_to: Option<String>,
    /// the folder this launcher was moved from, when it was started by a move
    moved_from: Option<String>,
    source: Option<paths::Source>,
    busy: bool,
    titles: Vec<TitleStatus>,
    sync_settings: bool,
}

fn moved_from() -> Option<String> {
    let args: Vec<String> = std::env::args().collect();
    args.iter().position(|a| a == "--moved-from").and_then(|i| args.get(i + 1)).cloned()
}

fn built_version(install: &std::path::Path) -> Option<String> {
    let text = std::fs::read_to_string(install.join("build.json")).ok()?;
    serde_json::from_str::<serde_json::Value>(&text).ok()?["version"].as_str().map(str::to_string)
}

#[tauri::command]
async fn overview(tasks: State<'_, Arc<Tasks>>, discs: State<'_, Discs>) -> Result<Overview, String> {
    let source = paths::current_source();
    let state = paths::load_state();
    let running = tasks.running_games();
    let mut titles_out = Vec::new();
    for t in titles::TITLES.iter() {
        let install = paths::install(t);
        let built = built_version(&install);
        let game = install.join(t.game_exe).exists();
        let cached = discs.lock().unwrap().get(t.key).cloned(); // not in the match: its lock would last the match
        let disc = match (cached, state.discs.get(t.key)) {
            (Some(d), _) => Some(d),
            (None, Some(p)) if PathBuf::from(p).exists() => {
                let d = disc::check(&PathBuf::from(p), source.as_ref().map(|s| s.path.as_path()));
                discs.lock().unwrap().insert(t.key.into(), d.clone());
                Some(d)
            }
            _ => None,
        };
        titles_out.push(TitleStatus {
            key: t.key,
            name: t.name,
            installed: install.join("game").join("default.xbe").exists(),
            game,
            editor: install.join(t.editor_exe).exists(),
            editor_available: install.join("game").join("ige.xbe").exists(),
            outdated: game
                && match (&built, &source) {
                    (Some(b), Some(s)) => paths::newer(&s.version, b),
                    _ => false,
                },
            built_version: built,
            disc,
            running: running.iter().filter_map(|r| r.strip_prefix(&format!("{}:", t.key)).map(str::to_string)).collect(),
            folder: install.to_string_lossy().into(),
        });
    }
    let warnings = paths::root_warnings();
    Ok(Overview {
        launcher_version: env!("CARGO_PKG_VERSION"),
        root: paths::root().to_string_lossy().into(),
        move_to: paths::move_target(&warnings).map(|p| p.to_string_lossy().into()),
        moved_from: moved_from(),
        warnings,
        source,
        busy: tasks.busy(),
        titles: titles_out,
        sync_settings: state.sync_settings,
    })
}

fn remember_disc(discs: &Discs, check: &disc::DiscCheck) -> Result<(), String> {
    if let (true, Some(key)) = (check.ok, &check.title) {
        let mut state = paths::load_state();
        state.discs.insert(key.clone(), check.path.clone());
        paths::save_state(&state)?;
        discs.lock().unwrap().insert(key.clone(), check.clone());
    }
    Ok(())
}

#[tauri::command]
async fn check_disc(path: String, discs: State<'_, Discs>) -> Result<disc::DiscCheck, String> {
    let source = paths::current_source();
    let p = PathBuf::from(path);
    let check = tauri::async_runtime::spawn_blocking(move || disc::check(&p, source.as_ref().map(|s| s.path.as_path())))
        .await
        .map_err(|e| e.to_string())?;
    remember_disc(&discs, &check)?;
    Ok(check)
}

#[tauri::command]
async fn pick_disc(app: AppHandle, folder: bool, discs: State<'_, Discs>) -> Result<Option<disc::DiscCheck>, String> {
    let picked = if folder {
        app.dialog().file().set_title("The folder with the disc's files").blocking_pick_folder()
    } else {
        app.dialog()
            .file()
            .set_title("Your Far Cry Instincts or Evolution disc image")
            .add_filter("Xbox disc image", &["iso", "xiso", "img", "bin"])
            .add_filter("All files", &["*"])
            .blocking_pick_file()
    };
    let Some(path) = picked.and_then(|p| p.into_path().ok()) else { return Ok(None) };
    check_disc(path.to_string_lossy().into(), discs).await.map(Some)
}

#[derive(Serialize)]
struct Updates {
    release: net::Release,
    /// the release is newer than the source here (or there is none yet)
    source: bool,
    /// a newer launcher: the launcher has the version of the release it comes in, so any newer release
    launcher: bool,
}

#[tauri::command]
async fn check_updates() -> Result<Updates, String> {
    tauri::async_runtime::spawn_blocking(|| {
        let release = net::latest_release()?;
        let here = paths::current_source().map(|s| s.version);
        Ok(Updates {
            source: here.as_deref().map_or(true, |h| paths::newer(&release.version, h)),
            launcher: paths::newer(&release.version, env!("CARGO_PKG_VERSION")),
            release,
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
fn start(app: AppHandle, tasks: State<'_, Arc<Tasks>>, request: task::Request) -> Result<(), String> {
    task::start(app, tasks.inner().clone(), request)
}

#[tauri::command]
fn cancel(tasks: State<'_, Arc<Tasks>>) {
    tasks.cancel();
}

#[tauri::command]
fn launch(tasks: State<'_, Arc<Tasks>>, title: String, editor: bool) -> Result<(), String> {
    tasks.launch(titles::by_key(&title)?, editor)
}

#[tauri::command]
fn open(app: AppHandle, what: String) -> Result<(), String> {
    let opener = app.opener();
    let path = match what.split_once(':') {
        Some(("url", url)) => return opener.open_url(url, None::<&str>).map_err(|e| e.to_string()),
        Some(("log", file)) => return opener.reveal_item_in_dir(file).map_err(|e| e.to_string()),
        Some(("title", key)) => paths::install(titles::by_key(key)?),
        // what the games themselves write: crash logs and saves
        Some(("gamelogs", key)) => game_folder(titles::by_key(key)?, "log-dir", "logs")?,
        Some(("saves", key)) => game_folder(titles::by_key(key)?, "save-dir", "profile")?,
        _ if what == "logs" => paths::logs(),
        _ => paths::root(),
    };
    let _ = std::fs::create_dir_all(&path);
    opener.open_path(path.to_string_lossy(), None::<&str>).map_err(|e| e.to_string())
}

/// a folder a game keeps under %APPDATA% (fci.ini's option, when set there, or its default)
fn game_folder(title: &titles::Title, option: &str, default: &str) -> Result<PathBuf, String> {
    let ini = std::fs::read_to_string(paths::install(title).join("fci.ini")).unwrap_or_default();
    if let Some(set) = settings::parse_values(&ini).get(option).filter(|v| !v.is_empty()) {
        return Ok(PathBuf::from(expand_env(set)));
    }
    let appdata = std::env::var_os("APPDATA").map(PathBuf::from).ok_or("no %APPDATA% folder")?;
    let base = if title.key == "evolution" { appdata.join("fci-recomp").join("evolution") } else { appdata.join("fci-recomp") };
    Ok(base.join(default))
}

/// %NAME% replaced by the environment variable's value, as fci.ini's paths are written
fn expand_env(text: &str) -> String {
    let mut out = String::new();
    let mut rest = text;
    while let Some(start) = rest.find('%') {
        let Some(len) = rest[start + 1..].find('%') else { break };
        let name = &rest[start + 1..start + 1 + len];
        out.push_str(&rest[..start]);
        match std::env::var(name) {
            Ok(v) if !name.is_empty() => out.push_str(&v),
            _ => out.push_str(&rest[start..start + len + 2]),
        }
        rest = &rest[start + len + 2..];
    }
    out + rest
}

#[derive(Serialize)]
struct ReportOut {
    text: String,
    url: String,
    log: String,
}

#[derive(Serialize)]
struct LogEntry {
    path: String,
    label: String,
    when: String,
    /// "ok", "failed", "stopped" or "running"
    result: String,
    error: Option<String>,
}

/// a build log's label and how it ended, from its first and last lines (task.rs writes both)
fn read_log(path: &std::path::Path) -> LogEntry {
    let text = std::fs::read_to_string(path).unwrap_or_default();
    let first = text.lines().next().unwrap_or("").trim_start_matches("== ");
    let (label, when) = first.rsplit_once(", ").unwrap_or((first, ""));
    let end = text.lines().rev().find_map(|l| l.strip_prefix("== finished: ")).map(str::to_string);
    let error = text.lines().rev().find_map(|l| l.strip_prefix("Error: ")).map(str::to_string);
    let result = match end.as_deref() {
        Some("ok") => "ok",
        Some("stopped") => "stopped",
        Some(_) => "failed",
        None => "running",
    };
    LogEntry {
        path: path.to_string_lossy().into(),
        label: label.into(),
        when: when.into(),
        result: result.into(),
        error: error.or_else(|| end.filter(|e| e != "ok" && e != "stopped")),
    }
}

#[tauri::command]
fn logs_list() -> Vec<LogEntry> {
    let mut files: Vec<PathBuf> = std::fs::read_dir(paths::logs())
        .into_iter()
        .flatten()
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|x| x == "log"))
        .collect();
    files.sort();
    files.iter().rev().take(30).map(|p| read_log(p)).collect()
}

/// the report for a build log (by default the last task's)
#[tauri::command]
fn report(tasks: State<'_, Arc<Tasks>>, path: Option<String>) -> Result<ReportOut, String> {
    let log = match path {
        Some(p) => p,
        None => tasks.last.lock().unwrap().as_ref().map(|o| o.log.clone()).ok_or("no build has run yet")?,
    };
    let entry = read_log(std::path::Path::new(&log));
    let r = report::build(std::path::Path::new(&log), &entry.label, entry.error.as_deref().unwrap_or("stopped"));
    Ok(ReportOut { text: r.text, url: r.url, log })
}

#[derive(Serialize)]
struct SettingsOut {
    available: bool,
    reason: String,
    options: Vec<settings::Option_>,
}

fn template() -> Result<PathBuf, String> {
    let source = paths::current_source().ok_or("fci-recomp is not downloaded yet")?;
    Ok(source.path.join("src").join("fci.ini"))
}

#[tauri::command]
fn settings_get(title: String) -> Result<SettingsOut, String> {
    let t = titles::by_key(&title)?;
    let ini = paths::install(t).join("fci.ini");
    if !ini.exists() {
        return Ok(SettingsOut {
            available: false,
            reason: format!("Build {} first: its settings file is made with it.", t.name),
            options: vec![],
        });
    }
    let template = template().unwrap_or_else(|_| ini.clone());
    Ok(SettingsOut { available: true, reason: String::new(), options: settings::read(&template, &ini)? })
}

#[tauri::command]
fn settings_save(title: String, changes: Vec<settings::Change>, sync: bool) -> Result<Vec<String>, String> {
    let template_path = template()?;
    let options = settings::parse_template(&std::fs::read_to_string(&template_path).map_err(|e| e.to_string())?);
    let mut saved = Vec::new();
    for t in titles::TITLES.iter() {
        let ini = paths::install(t).join("fci.ini");
        let ours = t.key == title;
        if !(ours || sync) || !ini.exists() {
            continue;
        }
        let changes: Vec<settings::Change> = changes
            .iter()
            .filter(|c| ours || !options.iter().any(|o| o.key == c.key && o.per_game))
            .map(|c| settings::Change { key: c.key.clone(), value: c.value.clone() })
            .collect();
        let text = std::fs::read_to_string(&ini).map_err(|e| format!("{}: {e}", ini.display()))?;
        std::fs::write(&ini, settings::apply(&text, &options, &changes)).map_err(|e| format!("{}: {e}", ini.display()))?;
        saved.push(t.key.to_string());
    }
    let mut state = paths::load_state();
    if state.sync_settings != sync {
        state.sync_settings = sync;
        paths::save_state(&state)?;
    }
    Ok(saved)
}

/// one game's settings onto the other's (the folders each game keeps for itself stay)
#[tauri::command]
fn settings_copy(from: String, to: String) -> Result<(), String> {
    let (a, b) = (titles::by_key(&from)?, titles::by_key(&to)?);
    let template_path = template()?;
    let options = settings::read(&template_path, &paths::install(a).join("fci.ini"))?;
    let changes: Vec<settings::Change> =
        options.iter().filter(|o| !o.per_game).map(|o| settings::Change { key: o.key.clone(), value: o.value.clone() }).collect();
    let ini = paths::install(b).join("fci.ini");
    let text = std::fs::read_to_string(&ini).map_err(|e| format!("Build {} first ({e})", b.name))?;
    std::fs::write(&ini, settings::apply(&text, &options, &changes)).map_err(|e| e.to_string())
}

/// the launcher in the release zip (tools/package.py puts it there)
const LAUNCHER_IN_SOURCE: &str = "FCI Launcher.exe";

/// the launcher of a newer release in place of this one, then started: from the source here when a games
/// update brought it already, else from the latest release's zip, downloaded and checked like any other
#[tauri::command]
async fn update_launcher(app: AppHandle, tasks: State<'_, Arc<Tasks>>) -> Result<(), String> {
    if tasks.busy() {
        return Err("Wait for the build to end first.".into());
    }
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    tauri::async_runtime::spawn_blocking(move || {
        let newer = |s: &paths::Source| paths::newer(&s.version, env!("CARGO_PKG_VERSION"));
        let source = match paths::current_source().filter(newer) {
            Some(s) => s,
            None => net::fetch_source(&net::latest_release()?, &mut |_, _| {})?,
        };
        let from = source.path.join(LAUNCHER_IN_SOURCE);
        if !newer(&source) || !from.exists() {
            return Err(format!("fci-recomp {} has no newer launcher in it", source.version));
        }
        let new = exe.with_extension("exe.new");
        std::fs::copy(&from, &new).map_err(|e| format!("copying the new launcher: {e}"))?;
        let old = exe.with_extension("exe.old");
        let _ = std::fs::remove_file(&old);
        std::fs::rename(&exe, &old).map_err(|e| format!("replacing the launcher: {e}"))?;
        if let Err(e) = std::fs::rename(&new, &exe) {
            let _ = std::fs::rename(&old, &exe);
            return Err(format!("replacing the launcher: {e}"));
        }
        std::process::Command::new(&exe).spawn().map_err(|e| e.to_string())?;
        app.exit(0);
        Ok(())
    })
    .await
    .map_err(|e| e.to_string())?
}

/// fci-recomp copied to C:\Games\fci-recomp and started from there, for a folder that is a poor place to
/// build (OneDrive, a long path, Program Files); this folder stays for the player to delete
#[tauri::command]
async fn move_launcher(app: AppHandle, tasks: State<'_, Arc<Tasks>>) -> Result<(), String> {
    if tasks.busy() {
        return Err("Wait for the build to end first.".into());
    }
    let old = paths::root();
    tauri::async_runtime::spawn_blocking(move || {
        let target = paths::move_target(&paths::root_warnings())
            .ok_or_else(|| format!("{} isn't empty, or a game is built here already.", paths::SUGGESTED_ROOT))?;
        let source = paths::current_source().ok_or("There is no fci-recomp release here to move.")?;
        let exe = paths::copy_to(&target, &source).inspect_err(|_| {
            let _ = std::fs::remove_dir_all(&target); // it was empty: nothing of the player's is lost
        })?;
        std::process::Command::new(&exe).arg("--moved-from").arg(&old).spawn().map_err(|e| e.to_string())?;
        app.exit(0);
        Ok(())
    })
    .await
    .map_err(|e| e.to_string())?
}

/// what in build\ a build makes again by itself: the recompiled C, the builds, the object cache, the
/// toolkit, downloads left over. Only these names: anything else in the folder isn't ours to remove.
/// Python and the downloaded compiler stay: they are tools, not a cache.
const CACHE_IN_WORK: [&str; 6] = ["lift", "build", "objects", "deps", "downloads", "toolchain/downloads"];

fn cache_entries() -> Vec<PathBuf> {
    CACHE_IN_WORK.iter().map(|name| paths::work().join(name)).filter(|p| p.exists()).collect()
}

fn size_of(path: &std::path::Path) -> u64 {
    match std::fs::symlink_metadata(path) {
        Ok(m) if m.is_dir() => std::fs::read_dir(path).into_iter().flatten().flatten().map(|e| size_of(&e.path())).sum(),
        Ok(m) => m.len(),
        Err(_) => 0,
    }
}

/// a file or a folder and everything in it; Git's read-only files too (tools/deps.py's remove_tree)
fn remove_all(path: &std::path::Path) -> std::io::Result<()> {
    fn writable(path: &std::path::Path) {
        if let Ok(m) = std::fs::symlink_metadata(path) {
            let mut perms = m.permissions();
            #[allow(clippy::permissions_set_readonly_false)]
            perms.set_readonly(false);
            let _ = std::fs::set_permissions(path, perms);
            if m.is_dir() {
                for e in std::fs::read_dir(path).into_iter().flatten().flatten() {
                    writable(&e.path());
                }
            }
        }
    }
    let remove = |p: &std::path::Path| if p.is_dir() { std::fs::remove_dir_all(p) } else { std::fs::remove_file(p) };
    remove(path).or_else(|_| {
        writable(path);
        remove(path)
    })
}

#[derive(Serialize)]
struct CacheSize {
    bytes: u64,
    /// the drive it is on ("C:"), or the folder elsewhere
    drive: String,
}

#[tauri::command]
async fn build_cache() -> Result<CacheSize, String> {
    tauri::async_runtime::spawn_blocking(|| {
        let work = paths::work();
        let drive = match work.components().next() {
            Some(std::path::Component::Prefix(p)) => p.as_os_str().to_string_lossy().to_string(),
            _ => work.to_string_lossy().to_string(),
        };
        CacheSize { bytes: cache_entries().iter().map(|p| size_of(p)).sum(), drive }
    })
    .await
    .map_err(|e| e.to_string())
}

#[tauri::command]
async fn clear_build_cache(tasks: State<'_, Arc<Tasks>>) -> Result<(), String> {
    if tasks.busy() {
        return Err("Wait for the build to end first.".into());
    }
    tauri::async_runtime::spawn_blocking(|| {
        for p in cache_entries() {
            remove_all(&p).map_err(|e| format!("{} couldn't be removed ({e}). A program may be using it; close it and try again.", p.display()))?;
        }
        Ok(())
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
fn quit(app: AppHandle, tasks: State<'_, Arc<Tasks>>) {
    tasks.cancel();
    app.exit(0);
}

fn main() {
    // what an update of the launcher left behind
    if let Ok(exe) = std::env::current_exe() {
        let _ = std::fs::remove_file(exe.with_extension("exe.old"));
    }
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .manage(Arc::new(Tasks::default()))
        .manage(Discs::default())
        .on_window_event(|window, event| match event {
            tauri::WindowEvent::CloseRequested { api, .. } => {
                if window.state::<Arc<Tasks>>().busy() {
                    api.prevent_close();
                    let _ = window.emit("close-requested", ());
                }
            }
            tauri::WindowEvent::DragDrop(tauri::DragDropEvent::Enter { .. }) => {
                let _ = window.emit("drag", true);
            }
            tauri::WindowEvent::DragDrop(tauri::DragDropEvent::Leave) => {
                let _ = window.emit("drag", false);
            }
            tauri::WindowEvent::DragDrop(tauri::DragDropEvent::Drop { paths, .. }) => {
                let _ = window.emit("drag", false);
                if let Some(p) = paths.first() {
                    let _ = window.emit("dropped", p.to_string_lossy().to_string());
                }
            }
            _ => {}
        })
        .invoke_handler(tauri::generate_handler![
            overview,
            check_disc,
            pick_disc,
            check_updates,
            start,
            cancel,
            launch,
            open,
            logs_list,
            report,
            settings_get,
            settings_save,
            settings_copy,
            update_launcher,
            move_launcher,
            build_cache,
            clear_build_cache,
            quit
        ])
        .run(tauri::generate_context!())
        .expect("error while running the launcher");
}
