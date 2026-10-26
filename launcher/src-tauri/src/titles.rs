//! The two games, as tools/titles.py names them. What a supported disc is comes from each title's
//! disc.json (the release source's copy when there is one, else the copy built into the launcher).

use serde::Serialize;

#[derive(Clone, Copy, Debug, Serialize)]
pub struct Title {
    pub key: &'static str,
    pub name: &'static str,
    pub folder: &'static str,
    pub title_id: &'static str,
    pub game_exe: &'static str,
    pub editor_exe: &'static str,
    /// the folder of disc.json in a release source
    pub data: &'static str,
    #[serde(skip)]
    pub builtin_disc_json: &'static str,
}

pub const TITLES: [Title; 2] = [
    Title {
        key: "instincts",
        name: "Far Cry Instincts",
        folder: "FarCryInstincts",
        title_id: "55530008",
        game_exe: "FarCryInstincts.exe",
        editor_exe: "FarCryInstinctsEditor.exe",
        data: "data",
        builtin_disc_json: include_str!("../../../data/disc.json"),
    },
    Title {
        key: "evolution",
        name: "Far Cry Instincts Evolution",
        folder: "FarCryEvolution",
        title_id: "55530060",
        game_exe: "FarCryEvolution.exe",
        editor_exe: "FarCryEvolutionEditor.exe",
        data: "data/evolution",
        builtin_disc_json: include_str!("../../../data/evolution/disc.json"),
    },
];

pub fn by_key(key: &str) -> Result<&'static Title, String> {
    TITLES.iter().find(|t| t.key == key).ok_or_else(|| format!("unknown game {key}"))
}

/// what a supported disc of this title is: its version and its default.xbe's SHA-256
pub struct Release {
    pub version: u64,
    pub default_xbe: String,
}

impl Title {
    pub fn release(&self, source: Option<&std::path::Path>) -> Release {
        let text = source
            .and_then(|s| std::fs::read_to_string(s.join(self.data).join("disc.json")).ok())
            .unwrap_or_else(|| self.builtin_disc_json.to_string());
        let v: serde_json::Value = serde_json::from_str(&text).unwrap_or_default();
        let default_xbe = v["files"]["default.xbe"]["sha256"].as_str().unwrap_or_default().to_ascii_lowercase();
        Release { version: v["version"].as_u64().unwrap_or(0), default_xbe }
    }
}
