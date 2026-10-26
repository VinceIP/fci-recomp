//! The Settings tab: the options of fci.ini, read from the release's src/fci.ini (sections start with
//! ";;", each option is its comment lines and then ";key = default") with the values from the game's own
//! fci.ini. Saving changes only the lines of the options changed; an option set back to its default is
//! commented out again, as it came.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::Path;

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct Choice {
    pub value: String,
    pub label: String,
}

#[derive(Serialize, Clone, Debug)]
pub struct Option_ {
    pub key: String,
    pub section: String,
    pub help: Vec<String>,
    pub default: String,
    pub value: String,
    /// "toggle", "choice", "number", "keys" or "text"
    pub kind: String,
    pub choices: Vec<Choice>,
    /// what the template says after the value (a key binding's action)
    pub label: String,
    /// differs per game: not copied when the settings are synced
    pub per_game: bool,
}

fn option_line(line: &str) -> Option<(String, String, String)> {
    // ";key = value   ; comment" in the template, "key = value" when set
    let body = line.strip_prefix(';').unwrap_or(line);
    let first = body.chars().next()?;
    if !first.is_ascii_lowercase() {
        return None;
    }
    let (key, rest) = body.split_once('=')?;
    let key = key.trim();
    if !key.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-') {
        return None;
    }
    let (value, comment) = match rest.find(" ;").or_else(|| rest.find("\t;")) {
        Some(i) => (&rest[..i], rest[i..].trim().trim_start_matches(';').trim()),
        None => (rest, ""),
    };
    Some((key.to_string(), value.trim().to_string(), comment.to_string()))
}

pub fn parse_template(text: &str) -> Vec<Option_> {
    let mut found = Vec::new();
    let mut section = String::from("General");
    let mut help: Vec<String> = Vec::new();
    for raw in text.lines() {
        let line = raw.trim_end();
        if let Some(name) = line.strip_prefix(";;") {
            section = name.trim().to_string();
            help.clear();
        } else if let Some((key, default, label)) = option_line(line) {
            let choices: Vec<Choice> = help
                .iter()
                .filter_map(|h| {
                    let (v, l) = h.split_once(" - ")?;
                    let v = v.trim();
                    (!v.is_empty() && v.len() <= 6 && !v.contains(' ')).then(|| Choice { value: v.into(), label: l.trim().into() })
                })
                .collect();
            let numeric = default.parse::<f64>().is_ok();
            let kind = if key.starts_with("controls-bind-") {
                "keys"
            } else if choices.len() >= 2 {
                "choice"
            } else if default == "0" || default == "1" {
                // a range ("0 to 1", "between 2 and 16") makes it a number, "0 off, 1 on" a switch
                let text = help.join(" ").to_lowercase();
                let words: Vec<&str> = text.split_whitespace().collect();
                let range = words.windows(3).any(|w| w[1] == "to" && w[0].trim_start_matches('(').parse::<f64>().is_ok());
                if range || text.contains("between") { "number" } else { "toggle" }
            } else if numeric {
                "number"
            } else {
                "text"
            };
            let help_text: Vec<String> = if kind == "choice" {
                help.iter().filter(|h| !choices.iter().any(|c| h.starts_with(&format!("{} - ", c.value)))).cloned().collect()
            } else {
                help.clone()
            };
            found.push(Option_ {
                per_game: key.ends_with("-dir") || key == "shader-cache",
                value: default.clone(),
                key,
                section: section.clone(),
                help: help_text,
                default,
                kind: kind.into(),
                choices,
                label,
            });
        } else if let Some(text) = line.strip_prefix("; ").or_else(|| line.strip_prefix(";\t")) {
            help.push(text.trim().to_string());
        } else if line.trim().is_empty() || line == ";" {
            help.clear();
        }
    }
    found
}

/// the values set in a game's fci.ini (lines without the semicolon)
pub fn parse_values(text: &str) -> BTreeMap<String, String> {
    text.lines()
        .filter(|l| !l.trim_start().starts_with(';'))
        .filter_map(|l| option_line(l.trim()))
        .map(|(k, v, _)| (k, v))
        .collect()
}

pub fn read(template: &Path, ini: &Path) -> Result<Vec<Option_>, String> {
    let text = std::fs::read_to_string(template).map_err(|e| format!("{}: {e}", template.display()))?;
    let mut options = parse_template(&text);
    let values = std::fs::read_to_string(ini).map(|t| parse_values(&t)).unwrap_or_default();
    for o in &mut options {
        if let Some(v) = values.get(&o.key) {
            o.value = v.clone();
        }
    }
    Ok(options)
}

#[derive(Deserialize)]
pub struct Change {
    pub key: String,
    pub value: String,
}

/// the changes into text: an option set to its default goes back to its commented line; one the file
/// lacks (added by a newer release) is added at the end of its section, or of the file
pub fn apply(text: &str, template: &[Option_], changes: &[Change]) -> String {
    let newline = if text.contains("\r\n") { "\r\n" } else { "\n" };
    let mut lines: Vec<String> = text.lines().map(str::to_string).collect();
    for change in changes {
        let Some(opt) = template.iter().find(|o| o.key == change.key) else { continue };
        let value = change.value.trim();
        let comment = if opt.label.is_empty() { String::new() } else { format!("  ; {}", opt.label) };
        let line = if value == opt.default {
            format!(";{} = {}{comment}", opt.key, opt.default)
        } else {
            format!("{} = {value}{comment}", opt.key)
        };
        let found: Vec<usize> =
            lines.iter().enumerate().filter(|(_, l)| option_line(l.trim()).is_some_and(|(k, ..)| k == opt.key)).map(|(i, _)| i).collect();
        if let Some(&first) = found.first() {
            lines[first] = line;
            for &i in found.iter().skip(1).rev() {
                lines.remove(i);
            }
        } else if value != opt.default {
            let header = lines.iter().position(|l| l.strip_prefix(";;").is_some_and(|n| n.trim() == opt.section));
            let at = header
                .map(|h| lines.iter().skip(h + 1).position(|l| l.starts_with(";;")).map_or(lines.len(), |n| h + 1 + n))
                .unwrap_or(lines.len());
            lines.insert(at, line);
        }
    }
    let mut out = lines.join(newline);
    out.push_str(newline);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    const TEMPLATE: &str = "; fci-recomp options\n;\n\n;; Display\n\n; Borderless fullscreen.\n;display-borderless = 0\n\n; Aspect ratio.\n; 0 - 4:3 (original)\n; 1 - 16:9\n;display-aspect = 0\n\n; Anisotropic texture filtering. 0 to disable, or set between 2 and 16.\n;gfx-anisotropic-filtering = 16\n\n;; Key bindings\n\n;controls-bind-rt = MOUSE1                  ; fire weapon\n\n; Folder for log files.\n;log-dir = %APPDATA%\\fci-recomp\\logs\n";

    #[test]
    fn reads_the_options_and_their_kinds() {
        let o = parse_template(TEMPLATE);
        let kinds: Vec<(&str, &str)> = o.iter().map(|o| (o.key.as_str(), o.kind.as_str())).collect();
        assert_eq!(
            kinds,
            [
                ("display-borderless", "toggle"),
                ("display-aspect", "choice"),
                ("gfx-anisotropic-filtering", "number"),
                ("controls-bind-rt", "keys"),
                ("log-dir", "text")
            ]
        );
        assert_eq!(o[1].choices[1], Choice { value: "1".into(), label: "16:9".into() });
        assert_eq!(o[1].help, ["Aspect ratio."]);
        assert_eq!(o[3].label, "fire weapon");
        assert_eq!(o[3].section, "Key bindings");
        assert!(o[4].per_game && !o[0].per_game);
    }

    #[test]
    fn saves_only_what_changed() {
        let o = parse_template(TEMPLATE);
        let user = TEMPLATE.replace(";display-aspect = 0", "display-aspect = 1");
        assert_eq!(parse_values(&user).get("display-aspect").map(String::as_str), Some("1"));
        let out = apply(
            &user,
            &o,
            &[
                Change { key: "display-aspect".into(), value: "0".into() },
                Change { key: "display-borderless".into(), value: "1".into() },
                Change { key: "controls-bind-rt".into(), value: "MOUSE1,F".into() },
            ],
        );
        assert!(out.contains("\n;display-aspect = 0\n"));
        assert!(out.contains("\ndisplay-borderless = 1\n"));
        assert!(out.contains("\ncontrols-bind-rt = MOUSE1,F  ; fire weapon\n"));
        assert_eq!(out.lines().count(), TEMPLATE.lines().count());
    }

    #[test]
    fn adds_an_option_the_file_lacks_to_its_section() {
        let o = parse_template(TEMPLATE);
        let old = TEMPLATE.replace(";display-borderless = 0\n", "");
        let out = apply(&old, &o, &[Change { key: "display-borderless".into(), value: "1".into() }]);
        let display = out.find(";; Display").unwrap();
        let keys = out.find(";; Key bindings").unwrap();
        let at = out.find("display-borderless = 1").unwrap();
        assert!(display < at && at < keys, "{out}");
    }

    #[test]
    fn reads_the_real_template() {
        let o = parse_template(include_str!("../../../src/fci.ini"));
        let get = |k: &str| o.iter().find(|o| o.key == k).unwrap_or_else(|| panic!("{k}"));
        assert_eq!(get("display-borderless").kind, "toggle");
        assert_eq!(get("display-aspect").kind, "choice");
        assert_eq!(get("log-verbosity").kind, "choice");
        assert_eq!(get("engine-update-cap").kind, "choice");
        assert_eq!(get("display-frame-rate").kind, "number");
        assert_eq!(get("gfx-res-scale").kind, "text");
        assert_eq!(get("gfx-ssao").kind, "toggle");
        assert_eq!(get("controls-bind-a").label, "jump");
        assert!(get("save-dir").per_game);
        assert!(o.len() > 50, "{}", o.len());
    }
}
