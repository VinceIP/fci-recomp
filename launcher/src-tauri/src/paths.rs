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

use crate::titles::{Title, TITLES};

/// where the launcher offers to move itself when its folder is a poor place to build
pub const SUGGESTED_ROOT: &str = r"C:\Games\fci-recomp";

/// what a release zip holds beside the launcher (tests/test_package.py keeps this in step with the zip)
pub const RELEASE_ENTRIES: [&str; 13] = [
    "BuildEditor.cmd",
    "BuildGame.cmd",
    "CHANGELOG.md",
    "LICENSE",
    "README.md",
    "THIRD_PARTY.md",
    "data",
    "fci-recomp.py",
    "patches",
    "re",
    "requirements.txt",
    "src",
    "tools",
];

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

/// the folder to move to, when a move is worth offering: this folder has a warning, no game is built here
/// yet (the disc's files copied so far are copied again from the disc) and C:\Games\fci-recomp is free
pub fn move_target(warnings: &[String]) -> Option<PathBuf> {
    if warnings.is_empty() || TITLES.iter().any(|t| install(t).join(t.game_exe).exists()) {
        return None;
    }
    let target = PathBuf::from(SUGGESTED_ROOT);
    let free = std::fs::read_dir(&target).map(|mut d| d.next().is_none()).unwrap_or(!target.exists());
    (free && target != root()).then_some(target)
}

fn copy_all(from: &Path, to: &Path) -> std::io::Result<()> {
    if from.is_dir() {
        std::fs::create_dir_all(to)?;
        for e in std::fs::read_dir(from)? {
            let e = e?;
            copy_all(&e.path(), &to.join(e.file_name()))?;
        }
        Ok(())
    } else {
        std::fs::copy(from, to).map(|_| ())
    }
}

/// the launcher, the release and the disc choices copied into target, laid out as the launcher keeps them;
/// returns the copied launcher. This folder stays as it was. From an extracted zip only the zip's own files
/// are copied: anything else beside the launcher (a Desktop's) isn't ours.
pub fn copy_to(target: &Path, source: &Source) -> Result<PathBuf, String> {
    let fail = |what: &str, e: std::io::Error| format!("copying {what} to {}: {e}", target.display());
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let to_source = target.join("source").join(&source.version);
    std::fs::create_dir_all(&to_source).map_err(|e| fail("fci-recomp", e))?;
    if source.path == root() {
        for name in RELEASE_ENTRIES {
            let from = root().join(name);
            if from.exists() {
                copy_all(&from, &to_source.join(name)).map_err(|e| fail(name, e))?;
            }
        }
    } else {
        copy_all(&source.path, &to_source).map_err(|e| fail("fci-recomp", e))?;
    }
    if state_file().exists() {
        std::fs::copy(state_file(), target.join("launcher.json")).map_err(|e| fail("launcher.json", e))?;
    }
    let new_exe = target.join(exe.file_name().unwrap_or_default());
    std::fs::copy(&exe, &new_exe).map_err(|e| fail("the launcher", e))?;
    Ok(new_exe)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_move_copies_the_release_and_nothing_else_beside_it() {
        let base = std::env::temp_dir().join(format!("fci-move-{}", std::process::id()));
        let (here, target) = (base.join("OneDrive").join("Desktop"), base.join("Games"));
        std::fs::create_dir_all(here.join("tools")).unwrap();
        std::fs::write(here.join("fci-recomp.py"), "VERSION = \"9.9.9\"\n").unwrap();
        std::fs::write(here.join("tools").join("events.py"), format!("PROTOCOL = {PROTOCOL}\n")).unwrap();
        std::fs::write(here.join("launcher.json"), "{}").unwrap();
        std::fs::write(here.join("holiday.jpg"), "a Desktop's own file").unwrap();
        std::env::set_var("FCI_LAUNCHER_ROOT", &here);
        let source = current_source().unwrap();
        let exe = copy_to(&target, &source).unwrap();
        let copied = target.join("source").join("9.9.9");
        assert!(copied.join("fci-recomp.py").exists() && copied.join("tools").join("events.py").exists());
        assert!(!copied.join("holiday.jpg").exists() && !copied.join("launcher.json").exists());
        assert!(target.join("launcher.json").exists() && exe.exists());
        std::env::set_var("FCI_LAUNCHER_ROOT", &target);
        assert_eq!(current_source().unwrap().path, copied); // the moved launcher finds the release
        let _ = std::fs::remove_dir_all(&base);
    }
}
