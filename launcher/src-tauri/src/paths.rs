//! Where everything is. The launcher is portable: it keeps everything in the folder it is in.
//!
//!   <root>/FCI Launcher.exe
//!   <root>/source/<version>/      a release of fci-recomp, as its zip holds it
//!   <root>/build/                 the work folder all versions share: Python, compiler, toolkit, lift, build
//!   <root>/FarCryInstincts/  <root>/FarCryEvolution/    the games (as BuildGame.cmd puts them)
//!   <root>/logs/                  one log per build
//!   <root>/launcher.json          the launcher's own state: the discs chosen, settings sync
//!
//! Save data stays where the games keep it, in %APPDATA%\fci-recomp. A folder that BuildGame.cmd built
//! in (a release extracted with FarCryInstincts\ beside it) works the same: the games there are found.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use crate::titles::Title;

pub fn root() -> PathBuf {
    if let Some(dir) = std::env::var_os("FCI_LAUNCHER_ROOT") {
        return PathBuf::from(dir);
    }
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(Path::to_path_buf))
        .unwrap_or_else(|| PathBuf::from("."))
}

pub fn work() -> PathBuf {
    root().join("build")
}

pub fn logs() -> PathBuf {
    root().join("logs")
}

pub fn install(title: &Title) -> PathBuf {
    root().join(title.folder)
}

pub fn sources() -> PathBuf {
    root().join("source")
}

/// Python, as the work folder holds it (tools/bootstrap.ps1 makes the same one)
pub fn python() -> PathBuf {
    if let Some(p) = std::env::var_os("FCI_PYTHON") {
        return PathBuf::from(p);
    }
    work().join("python").join("tools").join("python.exe")
}

#[derive(Default, Serialize, Deserialize)]
#[serde(default)]
pub struct State {
    /// per title key: the disc image or folder last chosen
    pub discs: BTreeMap<String, String>,
    /// settings saved for one game are saved for the other too
    pub sync_settings: bool,
}

fn state_file() -> PathBuf {
    root().join("launcher.json")
}

pub fn load_state() -> State {
    std::fs::read_to_string(state_file()).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
}

pub fn save_state(state: &State) -> Result<(), String> {
    let text = serde_json::to_string_pretty(state).map_err(|e| e.to_string())?;
    std::fs::write(state_file(), text).map_err(|e| format!("saving {}: {e}", state_file().display()))
}

/// "0.2.1" as numbers, for comparing; anything after the numbers ("-rc1") is ignored
pub fn version_key(v: &str) -> Vec<u64> {
    v.trim_start_matches('v')
        .split(|c: char| !c.is_ascii_digit())
        .take_while(|s| !s.is_empty())
        .map(|s| s.parse().unwrap_or(0))
        .collect()
}

pub fn newer(a: &str, b: &str) -> bool {
    version_key(a) > version_key(b)
}

/// a release source: a folder with fci-recomp.py that speaks the launcher's protocol
#[derive(Clone, Serialize)]
pub struct Source {
    pub version: String,
    pub path: PathBuf,
}

pub const PROTOCOL: u64 = 1;

/// the version of the fci-recomp.py in dir, when it is one the launcher can drive
pub fn read_source(dir: &Path) -> Option<Source> {
    let script = std::fs::read_to_string(dir.join("fci-recomp.py")).ok()?;
    let events = std::fs::read_to_string(dir.join("tools").join("events.py")).ok()?;
    let protocol = events.lines().find_map(|l| l.strip_prefix("PROTOCOL = "))?.trim().parse::<u64>().ok()?;
    if protocol != PROTOCOL {
        return None;
    }
    let version = script.lines().find_map(|l| l.strip_prefix("VERSION = "))?.trim().trim_matches('"').to_string();
    Some(Source { version, path: dir.to_path_buf() })
}

/// the newest source here: one under source/, or the root itself when it is an extracted release
pub fn current_source() -> Option<Source> {
    let mut found: Vec<Source> = std::fs::read_dir(sources())
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| !e.file_name().to_string_lossy().ends_with(".partial"))
        .filter_map(|e| read_source(&e.path()))
        .collect();
    found.extend(read_source(&root()));
    found.into_iter().max_by(|a, b| version_key(&a.version).cmp(&version_key(&b.version)))
}

/// what may go wrong with the folder the launcher is in, said before anything is built there
pub fn root_warnings() -> Vec<String> {
    let root = root();
    let text = root.to_string_lossy().to_lowercase();
    let mut found = Vec::new();
    let temp = std::env::temp_dir().to_string_lossy().to_lowercase();
    if !temp.is_empty() && text.starts_with(&temp) {
        found.push(
            "The launcher is running from a temporary folder (inside a zip?). Move FCI Launcher.exe to a \
             folder of its own, such as C:\\Games\\fci-recomp, and start it there."
                .into(),
        );
    }
    if text.contains("onedrive") {
        found.push(
            "This folder is in OneDrive, which can lock or upload the build's files while it runs. A folder \
             outside OneDrive, such as C:\\Games\\fci-recomp, works best."
                .into(),
        );
    }
    if text.contains("program files") {
        found.push("Folders in Program Files are not writable without administrator rights. Choose another folder.".into());
    }
    if let Some(home) = std::env::var_os("USERPROFILE").map(PathBuf::from) {
        for name in ["Desktop", "Downloads", "Documents"] {
            if root == home.join(name) {
                found.push(format!(
                    "The launcher puts several folders beside itself. Give it a folder of its own instead of {name}, \
                     such as C:\\Games\\fci-recomp."
                ));
            }
        }
    }
    if root.to_string_lossy().chars().count() > 100 {
        found.push("This folder's path is long; the build's deepest files may not fit. A shorter folder, such as C:\\Games\\fci-recomp, is safer.".into());
    }
    let probe = root.join(".write-test");
    if std::fs::write(&probe, b"").is_err() {
        found.push("This folder is not writable. Move the launcher to a folder you can write to.".into());
    }
    let _ = std::fs::remove_file(probe);
    found
}
