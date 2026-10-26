//! One build at a time: building a game or its map editor, or updating. A task gets the release source
//! and the private Python ready, then runs fci-recomp.py with --events and turns its JSON lines into one
//! progress bar and two lines of status for the page. Everything it prints goes to the task's log.

use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::io::{BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tauri::window::{ProgressBarState, ProgressBarStatus};
use tauri::{AppHandle, Emitter, Manager};

use crate::paths::{self, Source};
use crate::{net, report, titles};

#[derive(Deserialize, Clone, Debug)]
pub struct Request {
    /// "game", "editor" or "update"
    pub kind: String,
    pub title: Option<String>,
    /// with "game": build the map editor too
    #[serde(default)]
    pub editor: bool,
    #[serde(default)]
    pub accept_license: bool,
}

#[derive(Serialize, Clone)]
#[serde(tag = "type", rename_all = "lowercase")]
pub enum Event {
    Started { kind: String, title: Option<String>, label: String },
    Progress { percent: f64, status: String, detail: String },
    Warning { text: String },
    Finished(Outcome),
}

#[derive(Serialize, Clone, Default)]
pub struct Outcome {
    pub ok: bool,
    pub cancelled: bool,
    /// Microsoft's terms must be accepted before the compiler is downloaded: the URL of the terms
    pub license: Option<String>,
    pub error: Option<String>,
    pub log: String,
    pub minutes: u64,
    pub kind: String,
    pub title: Option<String>,
}

#[derive(Default)]
pub struct Tasks {
    running: Mutex<Option<Running>>,
    /// the last task that failed, for the report
    pub last: Mutex<Option<Outcome>>,
    games: Mutex<HashMap<String, Child>>,
}

struct Running {
    cancel: Arc<AtomicBool>,
    process: Arc<Mutex<Option<Process>>>,
}

pub fn no_window(cmd: &mut Command) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    #[cfg(not(windows))]
    let _ = cmd;
}

/// a running fci-recomp.py and everything it starts (compilers, msiexec): on Windows in a job object, so
/// cancelling or closing the launcher stops them all
struct Process {
    child: Child,
    #[cfg(windows)]
    job: Option<job::Job>,
}

impl Process {
    fn kill(&mut self) {
        #[cfg(windows)]
        if let Some(j) = &self.job {
            j.kill();
        }
        let _ = self.child.kill();
    }
}

#[cfg(windows)]
mod job {
    use std::os::windows::io::AsRawHandle;
    use windows_sys::Win32::Foundation::{CloseHandle, HANDLE};
    use windows_sys::Win32::System::JobObjects::*;

    pub struct Job(HANDLE);
    unsafe impl Send for Job {}

    impl Job {
        pub fn for_child(child: &std::process::Child) -> Option<Job> {
            unsafe {
                let h = CreateJobObjectW(std::ptr::null(), std::ptr::null());
                if h.is_null() {
                    return None;
                }
                let mut info: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
                info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                let job = Job(h);
                // Python starts its first child well after this; a job that didn't take is no job (Stop then
                // ends Python alone, and the caller logs it)
                let limited = SetInformationJobObject(
                    h,
                    JobObjectExtendedLimitInformation,
                    &info as *const _ as *const core::ffi::c_void,
                    std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
                );
                (limited != 0 && AssignProcessToJobObject(h, child.as_raw_handle() as HANDLE) != 0).then_some(job)
            }
        }

        pub fn kill(&self) {
            unsafe {
                TerminateJobObject(self.0, 1);
            }
        }
    }

    impl Drop for Job {
        fn drop(&mut self) {
            unsafe {
                CloseHandle(self.0);
            }
        }
    }
}

impl Tasks {
    pub fn busy(&self) -> bool {
        self.running.lock().unwrap().is_some()
    }

    pub fn cancel(&self) {
        if let Some(r) = self.running.lock().unwrap().as_ref() {
            r.cancel.store(true, Ordering::SeqCst);
            if let Some(p) = r.process.lock().unwrap().as_mut() {
                p.kill();
            }
        }
    }

    pub fn running_games(&self) -> Vec<String> {
        let mut games = self.games.lock().unwrap();
        games.retain(|_, child| matches!(child.try_wait(), Ok(None)));
        games.keys().cloned().collect()
    }

    pub fn launch(&self, title: &titles::Title, editor: bool) -> Result<(), String> {
        let install = paths::install(title);
        let exe = install.join(if editor { title.editor_exe } else { title.game_exe });
        if !exe.exists() {
            return Err(format!("{} is not built yet", exe.display()));
        }
        let child = Command::new(&exe).current_dir(&install).spawn().map_err(|e| format!("starting {}: {e}", exe.display()))?;
        self.games.lock().unwrap().insert(format!("{}:{}", title.key, if editor { "editor" } else { "game" }), child);
        Ok(())
    }
}

pub fn start(app: AppHandle, tasks: Arc<Tasks>, request: Request) -> Result<(), String> {
    let mut slot = tasks.running.lock().unwrap();
    if slot.is_some() {
        return Err("a build is running already".into());
    }
    if let Some(key) = &request.title {
        let running = tasks.running_games();
        if running.iter().any(|g| g.starts_with(&format!("{key}:"))) {
            return Err("Close the game first: its files are replaced by the build.".into());
        }
    }
    let running = Running { cancel: Arc::new(AtomicBool::new(false)), process: Arc::new(Mutex::new(None)) };
    let (cancel, process) = (running.cancel.clone(), running.process.clone());
    *slot = Some(running);
    drop(slot);
    std::thread::spawn(move || {
        let started = Instant::now();
        let mut job = Job::new(app.clone(), request.clone(), cancel, process);
        let result = job.run();
        let mut outcome = Outcome {
            kind: request.kind.clone(),
            title: request.title.clone(),
            log: job.log_path.to_string_lossy().into(),
            minutes: started.elapsed().as_secs() / 60,
            ..Default::default()
        };
        match result {
            Ok(()) => outcome.ok = true,
            Err(Stop::Cancelled) => outcome.cancelled = true,
            Err(Stop::License(url)) => outcome.license = Some(url),
            Err(Stop::Failed(e)) => outcome.error = Some(e),
        }
        job.log(&format!(
            "== finished: {}",
            match (&outcome.error, outcome.ok) {
                (_, true) => "ok".to_string(),
                (Some(e), _) => e.clone(),
                (None, _) => "stopped".into(),
            }
        ));
        *tasks.last.lock().unwrap() = Some(outcome.clone());
        *tasks.running.lock().unwrap() = None;
        // the taskbar button: no more progress; it flashes when the window is in the background
        if let Some(w) = app.get_webview_window("main") {
            let _ = w.set_progress_bar(ProgressBarState { status: Some(ProgressBarStatus::None), progress: None });
            if !w.is_focused().unwrap_or(true) {
                let _ = w.request_user_attention(Some(tauri::UserAttentionType::Informational));
            }
        }
        let _ = app.emit("task", Event::Finished(outcome));
    });
    Ok(())
}

enum Stop {
    Cancelled,
    License(String),
    Failed(String),
}

impl From<String> for Stop {
    fn from(e: String) -> Self {
        Stop::Failed(e)
    }
}

/// one run of fci-recomp.py: its plan and how far each step is
struct Run {
    plan: Vec<(String, f64)>,
    current: usize,
    /// per step: per part (game, editor), percent
    parts: HashMap<String, HashMap<String, f64>>,
    expected_parts: usize,
}

impl Run {
    fn fraction(&self) -> f64 {
        let total: f64 = self.plan.iter().map(|(_, w)| w).sum();
        if total == 0.0 {
            return 0.0; // the checks before the plan
        }
        let before: f64 = self.plan.iter().take(self.current).map(|(_, w)| w).sum();
        let Some((key, weight)) = self.plan.get(self.current) else { return 1.0 };
        let now = self.parts.get(key).map_or(0.0, |p| {
            let n = if key == "lift" || key == "build" { self.expected_parts.max(p.len()) } else { p.len().max(1) };
            p.values().sum::<f64>() / n as f64
        });
        ((before + weight * now / 100.0) / total).clamp(0.0, 1.0)
    }
}

struct Job {
    app: AppHandle,
    request: Request,
    cancel: Arc<AtomicBool>,
    process: Arc<Mutex<Option<Process>>>,
    log_path: PathBuf,
    log_file: Option<std::fs::File>,
    /// the share of the bar before the current stage, and the current stage's share
    base: f64,
    span: f64,
    status: String,
    detail: String,
    last_emit: Instant,
    last_percent: f64,
}

impl Job {
    fn new(app: AppHandle, request: Request, cancel: Arc<AtomicBool>, process: Arc<Mutex<Option<Process>>>) -> Job {
        let stamp = chrono::Local::now().format("%Y-%m-%d_%H-%M-%S");
        let name = format!("{stamp}-{}-{}.log", request.title.as_deref().unwrap_or("all"), request.kind);
        let _ = std::fs::create_dir_all(paths::logs());
        let log_path = paths::logs().join(name);
        let log_file = std::fs::File::create(&log_path).ok();
        Job {
            app,
            request,
            cancel,
            process,
            log_path,
            log_file,
            base: 0.0,
            span: 0.0,
            status: String::new(),
            detail: String::new(),
            last_emit: Instant::now() - Duration::from_secs(1),
            last_percent: -1.0,
        }
    }

    fn log(&mut self, line: &str) {
        if let Some(f) = self.log_file.as_mut() {
            let _ = writeln!(f, "{line}");
        }
    }

    fn stage(&mut self, base: f64, span: f64, status: &str) {
        self.base = base;
        self.span = span;
        self.status = status.to_string();
        self.detail.clear();
        self.show(0.0, true);
    }

    /// fraction: of the current stage
    fn show(&mut self, fraction: f64, force: bool) {
        let percent = ((self.base + self.span * fraction.clamp(0.0, 1.0)) * 100.0).min(100.0);
        if !force && self.last_emit.elapsed() < Duration::from_millis(120) && (percent - self.last_percent).abs() < 1.0 {
            return;
        }
        self.last_emit = Instant::now();
        self.last_percent = percent;
        // the same on the taskbar button, for a build left running in the background
        if let Some(w) = self.app.get_webview_window("main") {
            let _ = w.set_progress_bar(ProgressBarState {
                status: Some(ProgressBarStatus::Normal),
                progress: Some(percent.round() as u64),
            });
        }
        let _ = self.app.emit(
            "task",
            Event::Progress { percent, status: self.status.clone(), detail: self.detail.clone() },
        );
    }

    fn check_cancel(&self) -> Result<(), Stop> {
        if self.cancel.load(Ordering::SeqCst) {
            Err(Stop::Cancelled)
        } else {
            Ok(())
        }
    }

    fn run(&mut self) -> Result<(), Stop> {
        let titles_here: Vec<&titles::Title> = match &self.request.title {
            Some(key) => vec![titles::by_key(key)?],
            None => titles::TITLES.iter().collect(),
        };
        let label = match (self.request.kind.as_str(), titles_here.as_slice()) {
            ("game", [t]) => format!("Building {}", t.name),
            ("editor", [t]) => format!("Building the {} map editor", t.name),
            ("update", [t]) => format!("Updating {}", t.name),
            _ => "Updating".to_string(),
        };
        let _ = self.app.emit(
            "task",
            Event::Started { kind: self.request.kind.clone(), title: self.request.title.clone(), label: label.clone() },
        );
        for line in report::header(&label) {
            self.log(&line);
        }
        let disc = match self.request.kind.as_str() {
            "game" => {
                let key = self.request.title.clone().unwrap_or_default();
                Some(paths::load_state().discs.get(&key).cloned().ok_or_else(|| "choose the game's disc first".to_string())?)
            }
            _ => None,
        };

        self.stage(0.0, 0.03, "Getting fci-recomp ready");
        let source = self.source()?;
        self.log(&format!("source: {} in {}", source.version, source.path.display()));
        self.check_cancel()?;

        self.stage(0.03, 0.03, "Getting Python ready");
        let python = {
            let (tx, rx) = mpsc::channel::<(String, u64, u64)>();
            let path = source.path.clone();
            let worker = std::thread::spawn(move || {
                net::ensure_python(&path, &mut |what, done, total| {
                    let _ = tx.send((what.to_string(), done, total));
                })
            });
            for (what, done, total) in rx {
                self.detail = format!("{what}: {} of {} MB", done >> 20, (total >> 20).max(1));
                self.show(done as f64 / total.max(1) as f64, false);
            }
            worker.join().map_err(|_| Stop::Failed("the Python download stopped".into()))??
        };
        self.check_cancel()?;

        let work = paths::work();
        let mut runs: Vec<(Vec<String>, usize, String)> = Vec::new();
        let common = |install: &Path| {
            vec![
                "--install".to_string(),
                install.to_string_lossy().to_string(),
                "--work".to_string(),
                work.to_string_lossy().to_string(),
                "--events".to_string(),
            ]
        };
        for t in &titles_here {
            let install = paths::install(t);
            let mut args = match self.request.kind.as_str() {
                "game" => {
                    let mut a = vec!["setup".to_string(), disc.clone().unwrap_or_default()];
                    if self.request.editor {
                        a.push("--editor".into());
                    }
                    a
                }
                "editor" => vec!["editor".to_string()],
                _ => {
                    if !install.join(t.game_exe).exists() {
                        continue; // not built here: nothing to update
                    }
                    vec!["update".to_string()]
                }
            };
            args.extend(common(&install));
            if self.request.accept_license {
                args.push("--accept-msvc-license".into());
            }
            let parts = match self.request.kind.as_str() {
                "game" => 1 + self.request.editor as usize,
                "editor" => 1,
                _ => 1 + install.join(t.editor_exe).exists() as usize,
            };
            let name = if titles_here.len() > 1 { format!("{}: ", t.name) } else { String::new() };
            runs.push((args, parts, name));
        }
        let share = 0.94 / runs.len().max(1) as f64;
        for (i, (args, parts, name)) in runs.into_iter().enumerate() {
            self.base = 0.06 + share * i as f64;
            self.span = share;
            self.fci_recomp(&python, &source.path, &args, parts, &name)?;
            self.check_cancel()?;
        }
        self.status = "Done".into();
        self.detail.clear();
        self.base = 1.0;
        self.span = 0.0;
        self.show(0.0, true);
        Ok(())
    }

    /// the source to build with. A map editor is built with the source here, which its game was built
    /// with (the page asks for the game's update first otherwise). A game or an update takes the latest
    /// release, when it is newer than the source here and can be downloaded. A release zip beside the
    /// launcher, newer than the source here, counts as downloaded.
    fn source(&mut self) -> Result<Source, Stop> {
        if let Some(zip) = net::bundled_source() {
            self.detail = format!("Unpacking {}", zip.file_name().unwrap_or_default().to_string_lossy());
            self.show(0.5, true);
            return Ok(net::install_source(&zip)?);
        }
        let here = paths::current_source();
        if self.request.kind == "editor" {
            if let Some(s) = here {
                return Ok(s);
            }
        }
        self.detail = "Checking for the latest release".into();
        self.show(0.0, true);
        let release = match net::latest_release() {
            Ok(r) => r,
            Err(e) => return here.ok_or(Stop::Failed(e)),
        };
        if let Some(s) = &here {
            if !paths::newer(&release.version, &s.version) {
                return Ok(s.clone());
            }
        }
        match self.download(&release) {
            Ok(s) => Ok(s),
            // a game can still be built with the source here; an update can't
            Err(e) => match here {
                Some(s) if self.request.kind != "update" => {
                    self.log(&format!("Warning: {e} Building with fci-recomp {} instead.", s.version));
                    Ok(s)
                }
                _ => Err(Stop::Failed(e)),
            },
        }
    }

    /// release's zip into source/<version>
    fn download(&mut self, release: &net::Release) -> Result<Source, String> {
        let (tx, rx) = mpsc::channel::<(u64, u64)>();
        let worker = {
            let release = release.clone();
            std::thread::spawn(move || {
                net::fetch_source(&release, &mut |d, t| {
                    let _ = tx.send((d, t));
                })
            })
        };
        for (done, total) in rx {
            self.detail = format!("Downloading fci-recomp {}: {} of {} MB", release.version, done >> 20, (total >> 20).max(1));
            self.show(done as f64 / total.max(1) as f64, false);
        }
        worker.join().map_err(|_| "the download stopped".to_string())?
    }

    fn fci_recomp(&mut self, python: &Path, source: &Path, args: &[String], parts: usize, name: &str) -> Result<(), Stop> {
        let mut cmd = Command::new(python);
        cmd.arg("-B")
            .arg(source.join("fci-recomp.py"))
            .args(args)
            .current_dir(source)
            .env("PYTHONUTF8", "1")
            .env("PYTHONUNBUFFERED", "1")
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        no_window(&mut cmd);
        self.log(&format!("== {} -B fci-recomp.py {}", python.display(), args.join(" ")));
        let mut child = cmd.spawn().map_err(|e| format!("starting Python ({}) failed: {e}", python.display()))?;
        let (tx, rx) = mpsc::channel::<(bool, String)>();
        for (is_err, stream) in [
            (false, Box::new(child.stdout.take().unwrap()) as Box<dyn Read + Send>),
            (true, Box::new(child.stderr.take().unwrap()) as Box<dyn Read + Send>),
        ] {
            let tx = tx.clone();
            std::thread::spawn(move || {
                // a line ends at \n, or at the \r a console progress line is redrawn with
                let mut reader = BufReader::new(stream);
                let mut buf = Vec::new();
                loop {
                    buf.clear();
                    let mut byte = [0u8; 1];
                    let mut got_any = false;
                    loop {
                        match reader.read(&mut byte) {
                            Ok(0) | Err(_) => break,
                            Ok(_) => {
                                got_any = true;
                                if byte[0] == b'\n' || byte[0] == b'\r' {
                                    break;
                                }
                                buf.push(byte[0]);
                            }
                        }
                    }
                    if !got_any {
                        return;
                    }
                    if !buf.is_empty() && tx.send((is_err, String::from_utf8_lossy(&buf).to_string())).is_err() {
                        return;
                    }
                }
            });
        }
        drop(tx);
        #[cfg(windows)]
        let job = job::Job::for_child(&child);
        #[cfg(windows)]
        if job.is_none() {
            self.log("Warning: the build couldn't be put in a job object; Stop ends Python but not what it started.");
        }
        *self.process.lock().unwrap() = Some(Process {
            #[cfg(windows)]
            job,
            child,
        });
        let mut run = Run { plan: Vec::new(), current: 0, parts: HashMap::new(), expected_parts: parts };
        let mut error: Option<String> = None;
        let mut license: Option<String> = None;
        let mut stderr_tail: Vec<String> = Vec::new();
        for (is_err, line) in rx {
            let Some(json) = line.strip_prefix("@fci ") else {
                self.log(&line);
                if is_err {
                    stderr_tail.push(line.clone());
                    if stderr_tail.len() > 30 {
                        stderr_tail.remove(0);
                    }
                } else if !line.trim().is_empty() {
                    self.detail = line.trim().to_string();
                    self.show(run.fraction(), false);
                }
                continue;
            };
            let Ok(e) = serde_json::from_str::<serde_json::Value>(json) else { continue };
            let s = |k: &str| e[k].as_str().unwrap_or_default().to_string();
            match e["event"].as_str().unwrap_or_default() {
                "plan" => {
                    run.plan = e["steps"]
                        .as_array()
                        .into_iter()
                        .flatten()
                        .map(|st| (st["key"].as_str().unwrap_or("").to_string(), st["weight"].as_f64().unwrap_or(1.0)))
                        .collect();
                }
                "step" => {
                    let key = s("key");
                    run.current = run.plan.iter().position(|(k, _)| *k == key).unwrap_or(run.current);
                    self.status = format!("{name}{}", s("text"));
                    self.detail.clear();
                    self.show(run.fraction(), true);
                }
                "progress" => {
                    let part = e["part"].as_str().unwrap_or("").to_string();
                    run.parts.entry(s("key")).or_default().insert(part, e["percent"].as_f64().unwrap_or(0.0));
                    if !s("text").is_empty() {
                        self.detail = s("text");
                    }
                    self.show(run.fraction(), false);
                }
                "warning" => {
                    self.log(&format!("Warning: {}", s("text")));
                    let _ = self.app.emit("task", Event::Warning { text: s("text") });
                }
                "error" => {
                    self.log(&format!("Error: {}", s("text")));
                    error = Some(s("text"));
                }
                "license" => license = Some(s("url")),
                "done" => {
                    run.current = run.plan.len();
                    self.show(1.0, true);
                }
                _ => {}
            }
        }
        let status = {
            let mut p = self.process.lock().unwrap();
            let status = p.as_mut().map(|p| p.child.wait());
            *p = None;
            status
        };
        self.check_cancel()?;
        let code = status.and_then(|s| s.ok()).and_then(|s| s.code()).unwrap_or(-1);
        if code == 0 {
            return Ok(());
        }
        if let Some(url) = license {
            return Err(Stop::License(url));
        }
        Err(Stop::Failed(error.unwrap_or_else(|| {
            let tail = stderr_tail.iter().rev().find(|l| !l.trim().is_empty()).cloned().unwrap_or_default();
            format!("the build stopped (exit code {code}). {tail}")
        })))
    }
}
