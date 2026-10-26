//! Downloads: the release list from GitHub, release zips and the private Python, each checked against its
//! published digest before it is used.

use base64::Engine;
use serde::Serialize;
use sha2::{Digest, Sha256, Sha512};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::time::Duration;

use crate::paths::{self, Source};

pub const REPO: &str = "VinceIP/fci-recomp";

fn agent() -> ureq::Agent {
    ureq::AgentBuilder::new()
        .user_agent(concat!("fci-launcher/", env!("CARGO_PKG_VERSION")))
        .timeout_connect(Duration::from_secs(20))
        .timeout_read(Duration::from_secs(60))
        .try_proxy_from_env(true)
        .build()
}

pub const NETWORK_HINT: &str =
    "Check the internet connection; a proxy, firewall or antivirus can also block the download.";

#[derive(Serialize, Clone)]
pub struct Asset {
    pub name: String,
    pub url: String,
    pub size: u64,
    /// "sha256:<hex>", as GitHub lists it
    pub digest: Option<String>,
}

#[derive(Serialize, Clone)]
pub struct Release {
    pub version: String,
    pub name: String,
    pub notes: String,
    pub page: String,
    pub published: String,
    /// the zip players build from (tools/package.py's fci-recomp-<version>.zip); it carries the launcher of
    /// the same version too
    pub source: Option<Asset>,
}

pub fn latest_release() -> Result<Release, String> {
    let url = format!("https://api.github.com/repos/{REPO}/releases/latest");
    let v: serde_json::Value = agent()
        .get(&url)
        .set("Accept", "application/vnd.github+json")
        .call()
        .map_err(|e| format!("checking for updates failed ({e}). {NETWORK_HINT}"))?
        .into_json()
        .map_err(|e| e.to_string())?;
    let version = v["tag_name"].as_str().unwrap_or_default().trim_start_matches('v').to_string();
    let asset = |pred: &dyn Fn(&str) -> bool| {
        v["assets"].as_array().into_iter().flatten().find(|a| pred(a["name"].as_str().unwrap_or(""))).map(|a| Asset {
            name: a["name"].as_str().unwrap_or("").into(),
            url: a["browser_download_url"].as_str().unwrap_or("").into(),
            size: a["size"].as_u64().unwrap_or(0),
            digest: a["digest"].as_str().map(str::to_string),
        })
    };
    Ok(Release {
        source: asset(&|n| n.starts_with("fci-recomp-") && n.ends_with(".zip")),
        name: v["name"].as_str().unwrap_or(&version).into(),
        notes: v["body"].as_str().unwrap_or("").replace("\r\n", "\n"),
        page: v["html_url"].as_str().unwrap_or("").into(),
        published: v["published_at"].as_str().unwrap_or("").chars().take(10).collect(),
        version,
    })
}

pub enum Expect<'a> {
    Sha256Hex(&'a str),
    Sha512Base64(&'a str),
    /// GitHub's "sha256:<hex>"; a release asset without one is refused (every asset of ours has one)
    GitHub(Option<&'a str>),
}

/// url into file, checked; progress(bytes so far, bytes in all)
pub fn download(url: &str, file: &Path, expect: Expect, progress: &mut dyn FnMut(u64, u64)) -> Result<(), String> {
    if let Expect::GitHub(d) = &expect {
        if !d.is_some_and(|d| d.starts_with("sha256:")) {
            return Err(format!("{url} has no published SHA-256 to check it against; nothing was downloaded."));
        }
    }
    let response = agent().get(url).call().map_err(|e| format!("downloading {url} failed ({e}). {NETWORK_HINT}"))?;
    let total: u64 = response.header("Content-Length").and_then(|s| s.parse().ok()).unwrap_or(0);
    let mut reader = response.into_reader();
    let partial = file.with_extension("partial");
    let mut out = std::fs::File::create(&partial).map_err(|e| format!("{}: {e}", partial.display()))?;
    let (mut sha256, mut sha512) = (Sha256::new(), Sha512::new());
    let mut buf = vec![0u8; 1 << 16];
    let mut done = 0u64;
    loop {
        let n = reader.read(&mut buf).map_err(|e| format!("the download of {url} broke off ({e}). {NETWORK_HINT}"))?;
        if n == 0 {
            break;
        }
        sha256.update(&buf[..n]);
        sha512.update(&buf[..n]);
        out.write_all(&buf[..n]).map_err(|e| format!("{}: {e}", partial.display()))?;
        done += n as u64;
        progress(done, total.max(done));
    }
    drop(out);
    let ok = match expect {
        Expect::Sha256Hex(hex) => format!("{:x}", sha256.finalize()) == hex.to_lowercase(),
        Expect::Sha512Base64(b64) => base64::engine::general_purpose::STANDARD.encode(sha512.finalize()) == b64,
        Expect::GitHub(d) => d.and_then(|d| d.strip_prefix("sha256:")).is_some_and(|h| format!("{:x}", sha256.finalize()) == h),
    };
    if !ok {
        let _ = std::fs::remove_file(&partial);
        return Err(format!("{url} does not match its published digest; nothing was used. Try again; if it keeps failing, something between this PC and the server changes downloads."));
    }
    std::fs::rename(&partial, file).map_err(|e| format!("{}: {e}", file.display()))
}

pub fn unzip(zip: &Path, dst: &Path) -> Result<(), String> {
    let f = std::fs::File::open(zip).map_err(|e| e.to_string())?;
    let mut archive = zip::ZipArchive::new(f).map_err(|e| format!("{}: {e}", zip.display()))?;
    archive.extract(dst).map_err(|e| format!("unpacking {} failed: {e}", zip.display()))
}

/// release's zip, checked, into source/<version>; progress(bytes so far, bytes in all)
pub fn fetch_source(release: &Release, progress: &mut dyn FnMut(u64, u64)) -> Result<Source, String> {
    let asset = release
        .source
        .as_ref()
        .ok_or_else(|| format!("release {} has no fci-recomp zip to download; see {}", release.version, release.page))?;
    let downloads = paths::work().join("downloads");
    std::fs::create_dir_all(&downloads).map_err(|e| e.to_string())?;
    let zip = downloads.join(&asset.name);
    download(&asset.url, &zip, Expect::GitHub(asset.digest.as_deref()), progress)?;
    let installed = install_source(&zip);
    let _ = std::fs::remove_file(&zip);
    installed
}

/// a release zip into source/<version>, replacing a broken copy; the version comes from the zip itself
pub fn install_source(zip: &Path) -> Result<Source, String> {
    let partial = paths::sources().join("incoming.partial");
    if partial.exists() {
        std::fs::remove_dir_all(&partial).map_err(|e| e.to_string())?;
    }
    std::fs::create_dir_all(&partial).map_err(|e| e.to_string())?;
    unzip(zip, &partial)?;
    let found = paths::read_source(&partial).ok_or_else(|| {
        let version = std::fs::read_to_string(partial.join("fci-recomp.py"))
            .ok()
            .and_then(|t| t.lines().find_map(|l| l.strip_prefix("VERSION = ").map(|v| v.trim().trim_matches('"').to_string())));
        match version {
            Some(v) => format!(
                "fci-recomp {v} came out before this launcher and can't be built from it. Build it with the \
                 BuildGame.cmd that comes with it, or wait for the next release."
            ),
            None => format!("{} is not a release of fci-recomp", zip.display()),
        }
    })?;
    let dst = paths::sources().join(&found.version);
    if dst.exists() {
        std::fs::remove_dir_all(&dst).map_err(|e| format!("replacing {}: {e}", dst.display()))?;
    }
    std::fs::rename(&partial, &dst).map_err(|e| format!("{}: {e}", dst.display()))?;
    prune_sources(&found.version);
    Ok(Source { version: found.version, path: dst })
}

/// keep the newest two sources: the one in use and the one before it
fn prune_sources(keep: &str) {
    let mut found: Vec<(Vec<u64>, PathBuf)> = std::fs::read_dir(paths::sources())
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|e| paths::read_source(&e.path()).map(|s| (paths::version_key(&s.version), s.path)))
        .collect();
    found.sort();
    let keep = paths::version_key(keep);
    let n = found.len();
    for (i, (v, p)) in found.into_iter().enumerate() {
        if i + 2 < n && v != keep {
            let _ = std::fs::remove_dir_all(p);
        }
    }
}

/// a release zip that came beside the launcher (offline, or a test build) and is newer than what is here
pub fn bundled_source() -> Option<PathBuf> {
    let current = paths::current_source().map(|s| s.version).unwrap_or_default();
    std::fs::read_dir(paths::root())
        .ok()?
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            let n = p.file_name().unwrap_or_default().to_string_lossy().to_lowercase();
            n.starts_with("fci-recomp-") && n.ends_with(".zip")
        })
        .filter(|p| {
            let stem = p.file_stem().unwrap_or_default().to_string_lossy().to_string();
            current.is_empty() || paths::newer(stem.trim_start_matches("fci-recomp-"), &current)
        })
        .max()
}

/// the private Python from source's data/python.json into build/python, unless it is there already
pub fn ensure_python(source: &Path, progress: &mut dyn FnMut(&str, u64, u64)) -> Result<PathBuf, String> {
    let python = paths::python();
    if std::env::var_os("FCI_PYTHON").is_some() {
        return Ok(python);
    }
    let pins: serde_json::Value = serde_json::from_str(
        &std::fs::read_to_string(source.join("data").join("python.json")).map_err(|e| format!("data/python.json: {e}"))?,
    )
    .map_err(|e| e.to_string())?;
    let ready = format!(
        "python {}, capstone {}",
        pins["python"]["version"].as_str().unwrap_or("?"),
        pins["capstone"]["version"].as_str().unwrap_or("?")
    );
    let root = paths::work().join("python");
    let marker = root.join("ready");
    if python.exists() && std::fs::read_to_string(&marker).map(|t| t.trim() == ready).unwrap_or(false) {
        return Ok(python);
    }
    if root.exists() {
        std::fs::remove_dir_all(&root).map_err(|e| format!("removing the old Python in {}: {e}", root.display()))?;
    }
    std::fs::create_dir_all(&root).map_err(|e| e.to_string())?;
    let s = |v: &serde_json::Value| v.as_str().unwrap_or_default().to_string();
    let package = root.join("python.zip");
    download(&s(&pins["python"]["url"]), &package, Expect::Sha512Base64(&s(&pins["python"]["sha512"])), &mut |d, t| {
        progress("Downloading Python", d, t)
    })?;
    unzip(&package, &root)?;
    let _ = std::fs::remove_file(&package);
    let wheel = root.join("capstone.zip");
    download(&s(&pins["capstone"]["url"]), &wheel, Expect::Sha256Hex(&s(&pins["capstone"]["sha256"])), &mut |d, t| {
        progress("Downloading capstone", d, t)
    })?;
    unzip(&wheel, &root.join("tools").join("Lib").join("site-packages"))?;
    let _ = std::fs::remove_file(&wheel);
    let mut check = std::process::Command::new(&python);
    check.args(["-c", "import capstone, ssl, ctypes, winreg"]);
    crate::task::no_window(&mut check);
    let ok = check.status().map(|s| s.success()).unwrap_or(false);
    if !ok {
        return Err("the downloaded Python couldn't start. An antivirus may have blocked or removed it; allow the launcher's folder and try again.".into());
    }
    std::fs::write(&marker, format!("{ready}\r\n")).map_err(|e| e.to_string())?;
    Ok(python)
}
