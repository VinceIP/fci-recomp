//! What a build log starts with, and the report a player can paste into an issue: versions, the PC, the
//! error and the end of the log. The user's name is taken out of paths.

use std::path::Path;

use crate::{net, paths};

pub fn system() -> Vec<(String, String)> {
    let mut found = vec![
        ("Launcher".to_string(), env!("CARGO_PKG_VERSION").to_string()),
        ("fci-recomp".to_string(), paths::current_source().map(|s| s.version).unwrap_or_else(|| "not downloaded yet".into())),
    ];
    #[cfg(windows)]
    {
        use windows_sys::Win32::Globalization::{GetOEMCP, GetUserDefaultLocaleName};
        use windows_sys::Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX};
        let mut cmd = std::process::Command::new("cmd");
        cmd.args(["/c", "ver"]);
        crate::task::no_window(&mut cmd);
        if let Ok(out) = cmd.output() {
            found.push(("Windows".into(), String::from_utf8_lossy(&out.stdout).trim().to_string()));
        }
        let mut name = [0u16; 85];
        let n = unsafe { GetUserDefaultLocaleName(name.as_mut_ptr(), name.len() as i32) };
        let locale = String::from_utf16_lossy(&name[..(n.max(1) - 1) as usize]);
        found.push(("Language".into(), format!("{locale}, code page {}", unsafe { GetOEMCP() })));
        let mut mem: MEMORYSTATUSEX = unsafe { std::mem::zeroed() };
        mem.dwLength = std::mem::size_of::<MEMORYSTATUSEX>() as u32;
        if unsafe { GlobalMemoryStatusEx(&mut mem) } != 0 {
            found.push(("Memory".into(), format!("{} GB", (mem.ullTotalPhys as f64 / (1u64 << 30) as f64).round())));
        }
    }
    #[cfg(not(windows))]
    found.push(("System".into(), format!("{} {}", std::env::consts::OS, std::env::consts::ARCH)));
    found.push((
        "Processor threads".into(),
        std::thread::available_parallelism().map(|n| n.to_string()).unwrap_or_else(|_| "?".into()),
    ));
    found.push(("Folder".into(), mask(&paths::root().to_string_lossy())));
    found
}

pub fn header(label: &str) -> Vec<String> {
    let mut lines = vec![format!("== {label}, {}", chrono::Local::now().format("%Y-%m-%d %H:%M"))];
    lines.extend(system().into_iter().map(|(k, v)| format!("{k}: {v}")));
    lines.extend(paths::root_warnings().into_iter().map(|w| format!("Warning: {w}")));
    lines
}

/// the user's name out of a path or a line of a log: the profile folder where a path starts with it, and
/// the name where it is a whole folder name
pub fn mask(text: &str) -> String {
    let mut out = text.to_string();
    if let Some(home) = std::env::var_os("USERPROFILE").or_else(|| std::env::var_os("HOME")) {
        let home = home.to_string_lossy().trim_end_matches(['\\', '/']).to_string();
        if home.len() > 3 {
            out = replace_folder(&out, &home, "%USERPROFILE%", true);
        }
    }
    if let Some(user) = std::env::var_os("USERNAME").or_else(|| std::env::var_os("USER")) {
        let user = user.to_string_lossy().to_string();
        if user.len() > 1 {
            for sep in ['\\', '/'] {
                out = replace_folder(&out, &format!("{sep}{user}"), &format!("{sep}<user>"), false);
            }
        }
    }
    out
}

/// what in text, where the next character ends a folder name (a separator, a quote, a space or the end);
/// rooted: only where a path starts (not inside another path)
fn replace_folder(text: &str, what: &str, with: &str, rooted: bool) -> String {
    let lower = text.to_lowercase();
    let find = what.to_lowercase();
    let mut out = String::new();
    let mut at = 0;
    while let Some(i) = lower[at..].find(&find).map(|i| i + at) {
        let end = i + find.len();
        let next = text[end..].chars().next();
        let prev = text[..i].chars().next_back();
        let starts = !rooted || matches!(prev, None | Some('"' | '\'' | ' ' | '(' | '=' | ':' | '\t'));
        if starts && matches!(next, None | Some('\\' | '/' | '"' | '\'' | ' ' | ')' | ';' | ',')) {
            out.push_str(&text[at..i]);
            out.push_str(with);
        } else {
            out.push_str(&text[at..end]);
        }
        at = end;
    }
    out.push_str(&text[at..]);
    out
}

#[cfg(test)]
mod tests {
    #[test]
    fn whole_folder_names_only() {
        let r = |t: &str, w: &str| super::replace_folder(t, w, "%P%", true);
        assert_eq!(r("C:\\Users\\Bob\\x", "C:\\Users\\Bob"), "%P%\\x");
        assert_eq!(r("C:\\Users\\Bobby\\x", "C:\\Users\\Bob"), "C:\\Users\\Bobby\\x");
        assert_eq!(r("see c:\\users\\bob", "C:\\Users\\Bob"), "see %P%");
        assert_eq!(r("/demo/root", "/root"), "/demo/root");
        assert_eq!(r("in /root/a", "/root"), "in %P%/a");
        assert_eq!(super::replace_folder("D:\\x\\bob\\y", "\\bob", "\\<user>", false), "D:\\x\\<user>\\y");
    }
}

pub struct Report {
    pub text: String,
    pub url: String,
}

/// for a failed task: its log's end, the error and the PC
pub fn build(log: &Path, label: &str, error: &str) -> Report {
    let text = std::fs::read_to_string(log).unwrap_or_default();
    let lines: Vec<&str> = text.lines().collect();
    let tail: Vec<String> = lines.iter().rev().take(40).rev().map(|l| mask(l)).collect();
    let compiler = lines.iter().find(|l| l.starts_with("Compiler:")).map(|l| l.to_string());
    let warnings: Vec<String> = lines.iter().filter_map(|l| l.strip_prefix("Warning: ")).map(mask).collect();
    let system = system();
    let field = |k: &str| system.iter().find(|(n, _)| n == k).map(|(_, v)| v.clone()).unwrap_or_default();
    let version = format!("fci-recomp {}, launcher {}", field("fci-recomp"), field("Launcher"));
    let mut sys_line: Vec<String> =
        system.iter().filter(|(k, _)| !["Launcher", "fci-recomp", "Folder"].contains(&k.as_str())).map(|(k, v)| format!("{k}: {v}")).collect();
    sys_line.push(compiler.clone().unwrap_or_else(|| "Compiler: not reached".into()));
    let error = mask(error);
    let mut out = String::new();
    out += &format!("**{label} failed**\n\n```text\n{error}\n```\n\n");
    out += &format!("**Version:** {version}\n\n**System:** {}\n\n", sys_line.join(" · "));
    if !warnings.is_empty() {
        out += &format!("**Warnings before the build:**\n{}\n\n", warnings.iter().map(|w| format!("- {w}")).collect::<Vec<_>>().join("\n"));
    }
    out += &format!("<details><summary>End of the log</summary>\n\n```text\n{}\n```\n</details>\n", tail.join("\n"));
    let enc = |s: &str| {
        s.bytes()
            .map(|b| match b {
                b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => (b as char).to_string(),
                _ => format!("%{b:02X}"),
            })
            .collect::<String>()
    };
    let title: String = format!("Build: {}", error.lines().next().unwrap_or(""));
    let title: String = title.chars().take(90).collect();
    let mut url = format!(
        "https://github.com/{}/issues/new?template=bug_report.yml&title={}&stop={}&version={}&system={}&what={}",
        net::REPO,
        enc(&title),
        enc(&error.chars().take(1500).collect::<String>()),
        enc(&version),
        enc(&sys_line.join(" · ")),
        enc(&format!("{label} failed in the launcher. (Attach the log file from the launcher's logs folder.)"))
    );
    url.truncate(7500);
    Report { text: out, url }
}
