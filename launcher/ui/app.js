// The launcher's page: the two games as banners, what each can do now, the build's progress, the settings
// of fci.ini and the build logs. Everything it does goes through the commands in src-tauri/src/main.rs.
"use strict";

const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

// banners: the colour art, and a monochrome version when the artwork comes with one (otherwise the
// colour art is greyed out until the game is built)
const ART = {
  instincts: { color: "art/instincts.jpg", mono: null },
  evolution: { color: "art/evolution.jpg", mono: null },
};
const SHORT = { instincts: "Instincts", evolution: "Evolution" };
// brand marks from Simple Icons (CC0), and a folder
const ICONS = {
  discord: '<svg viewBox="0 0 24 24" width="18" height="18" aria-hidden="true"><path d="M20.317 4.3698a19.7913 19.7913 0 00-4.8851-1.5152.0741.0741 0 00-.0785.0371c-.211.3753-.4447.8648-.6083 1.2495-1.8447-.2762-3.68-.2762-5.4868 0-.1636-.3933-.4058-.8742-.6177-1.2495a.077.077 0 00-.0785-.037 19.7363 19.7363 0 00-4.8852 1.515.0699.0699 0 00-.0321.0277C.5334 9.0458-.319 13.5799.0992 18.0578a.0824.0824 0 00.0312.0561c2.0528 1.5076 4.0413 2.4228 5.9929 3.0294a.0777.0777 0 00.0842-.0276c.4616-.6304.8731-1.2952 1.226-1.9942a.076.076 0 00-.0416-.1057c-.6528-.2476-1.2743-.5495-1.8722-.8923a.077.077 0 01-.0076-.1277c.1258-.0943.2517-.1923.3718-.2914a.0743.0743 0 01.0776-.0105c3.9278 1.7933 8.18 1.7933 12.0614 0a.0739.0739 0 01.0785.0095c.1202.099.246.1981.3728.2924a.077.077 0 01-.0066.1276 12.2986 12.2986 0 01-1.873.8914.0766.0766 0 00-.0407.1067c.3604.698.7719 1.3628 1.225 1.9932a.076.076 0 00.0842.0286c1.961-.6067 3.9495-1.5219 6.0023-3.0294a.077.077 0 00.0313-.0552c.5004-5.177-.8382-9.6739-3.5485-13.6604a.061.061 0 00-.0312-.0286zM8.02 15.3312c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9555-2.4189 2.157-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.9555 2.4189-2.1569 2.4189zm7.9748 0c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9554-2.4189 2.1569-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.946 2.4189-2.1568 2.4189Z"/></svg>',
  github: '<svg viewBox="0 0 24 24" width="16" height="16" aria-hidden="true"><path d="M12 .297c-6.63 0-12 5.373-12 12 0 5.303 3.438 9.8 8.205 11.385.6.113.82-.258.82-.577 0-.285-.01-1.04-.015-2.04-3.338.724-4.042-1.61-4.042-1.61C4.422 18.07 3.633 17.7 3.633 17.7c-1.087-.744.084-.729.084-.729 1.205.084 1.838 1.236 1.838 1.236 1.07 1.835 2.809 1.305 3.495.998.108-.776.417-1.305.76-1.605-2.665-.3-5.466-1.332-5.466-5.93 0-1.31.465-2.38 1.235-3.22-.135-.303-.54-1.523.105-3.176 0 0 1.005-.322 3.3 1.23.96-.267 1.98-.399 3-.405 1.02.006 2.04.138 3 .405 2.28-1.552 3.285-1.23 3.285-1.23.645 1.653.24 2.873.12 3.176.765.84 1.23 1.91 1.23 3.22 0 4.61-2.805 5.625-5.475 5.92.42.36.81 1.096.81 2.22 0 1.606-.015 2.896-.015 3.286 0 .315.21.69.825.57C20.565 22.092 24 17.592 24 12.297c0-6.627-5.373-12-12-12"/></svg>',
  folder: '<svg viewBox="0 0 24 24" width="16" height="16" aria-hidden="true"><path d="M3 6.5A1.5 1.5 0 0 1 4.5 5h4.6l2 2h8.4A1.5 1.5 0 0 1 21 8.5v9a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 17.5z" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"/></svg>',
};
const DISCORD = "https://discord.gg/UhRnapmH2j";
const GITHUB = "https://github.com/VinceIP/fci-recomp";

const S = {
  ov: null, // overview()
  updates: null, // check_updates()
  task: null, // the running build
  result: null, // how the last one ended, until dismissed
  lastRequest: null,
  discFail: {}, // per title: a disc that was not supported
  editorToo: {}, // per title: "also build the map editor"
  tab: "games",
  settings: { title: "instincts", options: [], edits: {}, available: false, reason: "", section: null },
};

// ---------------------------------------------------------------- helpers

function h(tag, props, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v == null || v === false) continue;
    if (k === "class") el.className = v;
    else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
    else if (k === "html") el.innerHTML = v;
    else if (k === "style") el.style.cssText = v;
    else if (k === "checked" || k === "value") el[k] = v;
    else el.setAttribute(k, v === true ? "" : v);
  }
  for (const kid of kids.flat()) {
    if (kid == null || kid === false) continue;
    el.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
  }
  return el;
}
const $ = (id) => document.getElementById(id);
const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);

let toastTimer;
function toast(text, ms = 3800) {
  const t = $("toast");
  t.textContent = text;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.hidden = true), ms);
}

function modal(...content) {
  const wrap = $("modal");
  wrap.replaceChildren(h("div", { class: "modal", role: "dialog" }, ...content));
  wrap.hidden = false;
  return () => (wrap.hidden = true);
}
function closeModal() {
  $("modal").hidden = true;
}

function popover(anchor, items) {
  const p = $("popover");
  p.replaceChildren(
    ...items.map((it) =>
      it === "-"
        ? h("hr")
        : h("button", { onclick: () => { p.hidden = true; it.run(); } }, it.label),
    ),
  );
  p.hidden = false;
  const r = anchor.getBoundingClientRect();
  const w = p.offsetWidth;
  p.style.left = Math.max(8, Math.min(window.innerWidth - w - 8, r.right - w)) + "px";
  const top = r.bottom + 6;
  p.style.top = (top + p.offsetHeight > window.innerHeight ? r.top - p.offsetHeight - 6 : top) + "px";
}
document.addEventListener("keydown", (e) => {
  if (e.key !== "Escape") return;
  $("popover").hidden = true;
  closeModal();
});
document.addEventListener("mousedown", (e) => {
  const p = $("popover");
  if (!p.hidden && !p.contains(e.target)) p.hidden = true;
});

async function call(cmd, args) {
  try {
    return await invoke(cmd, args);
  } catch (e) {
    toast(String(e), 6000);
    throw e;
  }
}

const open = (what) => call("open", { what }).catch(() => {});

function fileName(path) {
  return String(path).split(/[\\/]/).filter(Boolean).pop() || path;
}

// release notes (GitHub markdown, hard-wrapped) as simple HTML: headings, lists, paragraphs
function notesHtml(text) {
  const blocks = []; // [kind, text]
  for (const raw of esc(text || "").split("\n")) {
    const line = raw.trim();
    const last = blocks[blocks.length - 1];
    if (!line) blocks.push(["gap", ""]);
    else if (line.startsWith("#")) blocks.push(["h4", line.replace(/^#+\s*/, "")]);
    else if (/^[-*] /.test(line)) blocks.push(["li", line.slice(2)]);
    else if (last && (last[0] === "li" || last[0] === "p") && raw.startsWith(" ") === (last[0] === "li")) last[1] += " " + line;
    else if (last && last[0] === "p") last[1] += " " + line;
    else blocks.push(["p", line]);
  }
  const inline = (s) => s
    .replace(/`([^`]+)`/g, "<code>$1</code>")
    .replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>")
    .replace(/\[([^\]]+)\]\([^)]+\)/g, "$1");
  let html = "", list = false;
  for (const [kind, body] of blocks) {
    if (kind === "li" && !list) html += "<ul>";
    if (kind !== "li" && list) html += "</ul>";
    list = kind === "li";
    if (kind !== "gap") html += `<${kind}>${inline(body)}</${kind}>`;
  }
  return html + (list ? "</ul>" : "");
}

// ---------------------------------------------------------------- data

let lastOverview = "";
async function refresh(force = true) {
  let ov;
  try {
    ov = await invoke("overview");
  } catch (e) {
    toast(String(e), 6000);
    return;
  }
  const text = JSON.stringify(ov);
  if (!force && text === lastOverview) return; // nothing changed: keep the page (and its scrolling) as it is
  lastOverview = text;
  S.ov = ov;
  $("version").textContent = S.ov.source ? `v${S.ov.source.version}` : "";
  render();
}

async function checkUpdates(manual = false) {
  try {
    S.updates = await invoke("check_updates");
    if (manual) toast(S.updates.source || S.updates.launcher ? `fci-recomp ${S.updates.release.version} is out.` : "Everything is up to date.");
  } catch (e) {
    if (manual) toast(String(e), 6000);
  }
  renderTop();
  renderSide();
}

const anyBuilt = () => S.ov && S.ov.titles.some((t) => t.game);
const titleOf = (key) => S.ov && S.ov.titles.find((t) => t.key === key);

// ---------------------------------------------------------------- tasks

async function start(request) {
  // a build in a poor folder (OneDrive, a long path) is the most common way to fail: offer the move first
  if (S.ov && S.ov.move_to && !S.buildHere && (request.kind === "game" || request.kind === "editor")) return moveDialog(request);
  S.lastRequest = request;
  S.result = null;
  try {
    await invoke("start", { request });
  } catch (e) {
    modal(h("h3", {}, "Can't start"), h("p", {}, String(e)), h("div", { class: "buttons" }, h("button", { class: "btn", onclick: closeModal }, "OK")));
  }
}

listen("task", ({ payload: e }) => {
  if (e.type === "started") {
    S.task = { kind: e.kind, title: e.title, label: e.label, percent: 0, status: "Starting", detail: "", warnings: [], started: Date.now() };
    render();
  } else if (e.type === "progress" && S.task) {
    Object.assign(S.task, { percent: e.percent, status: e.status, detail: e.detail });
    updateProgress();
  } else if (e.type === "warning" && S.task) {
    S.task.warnings.push(e.text);
    renderSide();
  } else if (e.type === "finished") {
    const label = S.task ? S.task.label : "The build";
    S.task = null;
    S.result = { ...e, label, request: S.lastRequest };
    if (e.license) {
      S.result = null;
      licenseDialog(e.license);
    } else if (e.cancelled) {
      S.result = null;
      toast("Stopped. Starting it again resumes where it left off.");
    }
    refresh().then(() => checkUpdates());
    if (S.tab === "settings") loadCache();
  }
});

function licenseDialog(url) {
  modal(
    h("h3", {}, "Microsoft's compiler is needed"),
    h("p", {}, "Visual Studio with the C++ tools wasn't found on this PC, so the build downloads Microsoft's C++ compiler and the Windows SDK into the launcher's build folder (about 2 GB, only once). Nothing is installed on the system."),
    h("p", {}, "They are downloaded from Microsoft under the Visual Studio Build Tools license terms. ",
      h("button", { class: "link", onclick: () => open("url:" + url) }, "Read the license terms")),
    h("div", { class: "buttons" },
      h("button", { class: "btn", onclick: closeModal }, "Cancel"),
      h("button", { class: "btn primary", onclick: () => { closeModal(); start({ ...S.lastRequest, accept_license: true }); } }, "Accept and build"),
    ),
  );
}

function updateProgress() {
  const t = S.task;
  if (!t) return;
  const pct = Math.floor(t.percent);
  for (const el of document.querySelectorAll("[data-pct]")) el.textContent = pct;
  for (const el of document.querySelectorAll("[data-bar]")) el.style.width = t.percent + "%";
  for (const el of document.querySelectorAll("[data-status]")) el.textContent = t.status;
  for (const el of document.querySelectorAll("[data-detail]")) el.textContent = t.detail || " ";
}

setInterval(() => {
  const el = document.querySelector("[data-elapsed]");
  if (el && S.task) {
    const s = Math.floor((Date.now() - S.task.started) / 1000);
    el.textContent = `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")} elapsed`;
  }
}, 1000);

// ---------------------------------------------------------------- discs

async function pickDisc(key, folder = false) {
  let check;
  try {
    check = await invoke("pick_disc", { folder });
  } catch (e) {
    toast(String(e), 6000);
    return;
  }
  if (check) showDisc(check, key);
}

function showDisc(check, key) {
  if (check.ok) {
    delete S.discFail[check.title];
    if (key && check.title !== key) toast(`That's ${titleOf(check.title).name}: it's ready on its own banner.`);
    else toast(check.headline);
  } else if (check.title) {
    S.discFail[check.title] = check;
  } else {
    modal(
      h("h3", {}, check.headline || "Not a supported disc"),
      h("p", {}, check.message),
      check.detail && h("p", { class: "muted" }, check.detail),
      h("p", { class: "muted" }, "fci-recomp builds Far Cry Instincts (title 55530008, version 1) and Far Cry Instincts Evolution (title 55530060, version 3), from an ISO of the disc or the folder with its files."),
      h("div", { class: "buttons" }, h("button", { class: "btn primary", onclick: closeModal }, "OK")),
    );
  }
  refresh();
}

listen("drag", ({ payload }) => {
  $("drop").hidden = !(payload && S.tab === "games" && !S.task);
});
listen("dropped", async ({ payload }) => {
  $("drop").hidden = true;
  if (S.task || S.tab !== "games") return;
  toast("Checking the disc…");
  try {
    showDisc(await invoke("check_disc", { path: payload }));
  } catch (e) {
    toast(String(e), 6000);
  }
});

listen("close-requested", () => {
  modal(
    h("h3", {}, "A build is running"),
    h("p", {}, "Quitting stops it. Starting it again later resumes where it left off."),
    h("div", { class: "buttons" },
      h("button", { class: "btn", onclick: closeModal }, "Keep building"),
      h("button", { class: "btn danger", onclick: () => invoke("quit") }, "Stop and quit"),
    ),
  );
});

// ---------------------------------------------------------------- rendering: top

function render() {
  renderTop();
  renderWarnings();
  renderCards();
  renderSide();
  if (S.tab === "logs") renderLogs();
}

function renderTop() {
  const slot = $("update-slot");
  slot.replaceChildren();
  const u = S.updates;
  if (!u) return;
  if (u.source && anyBuilt()) {
    slot.append(h("button", { class: "btn update", disabled: !!S.task, onclick: updateDialog }, `Update to ${u.release.version}`));
  }
  if (u.launcher) {
    slot.append(h("button", { class: "btn small ghost", onclick: launcherDialog, title: "A newer launcher is available" }, "New launcher"));
  }
}

function updateDialog() {
  const u = S.updates;
  const built = S.ov.titles.filter((t) => t.game).map((t) => t.name);
  modal(
    h("h3", {}, u.release.name || `fci-recomp ${u.release.version}`),
    h("p", {}, `Updating downloads this release and rebuilds ${built.join(" and ")} with it, from the game files already here: no disc needed. Only what the release changed is rebuilt.`),
    h("div", { class: "notes", html: notesHtml(u.release.notes) }),
    h("div", { class: "buttons" },
      h("button", { class: "btn ghost", onclick: () => open("url:" + u.release.page) }, "Release page"),
      h("button", { class: "btn", onclick: closeModal }, "Later"),
      h("button", { class: "btn primary", onclick: () => { closeModal(); start({ kind: "update" }); } }, "Update now"),
    ),
  );
}

function launcherDialog() {
  modal(
    h("h3", {}, "A newer launcher is available"),
    h("p", {}, "It replaces this one in the same folder and starts again. Your games, settings and saves stay as they are."),
    h("div", { class: "buttons" },
      h("button", { class: "btn", onclick: closeModal }, "Later"),
      h("button", { class: "btn primary", disabled: !!S.task, onclick: async () => {
        closeModal();
        toast("Downloading the new launcher…", 20000);
        await call("update_launcher").catch(() => {});
      } }, "Update the launcher"),
    ),
  );
}

$("menu-button").addEventListener("click", (e) =>
  popover(e.currentTarget, [
    { label: "Check for updates", run: () => checkUpdates(true) },
    { label: "Open the launcher's folder", run: () => open("root") },
    "-",
    { label: "Discord", run: () => open("url:" + DISCORD) },
    { label: "fci-recomp on GitHub", run: () => open("url:" + GITHUB) },
    "-",
    { label: `Launcher ${S.ov ? S.ov.launcher_version : ""}`, run: () => {} },
  ]),
);

$("home").addEventListener("click", () => switchTab("games"));
for (const b of document.querySelectorAll(".tab")) {
  b.addEventListener("click", () => switchTab(b.dataset.tab));
}
function switchTab(tab) {
  if (S.tab === "settings" && tab !== "settings" && Object.keys(S.settings.edits).length) {
    if (!confirm("Discard the settings you haven't saved?")) return;
    S.settings.edits = {};
  }
  S.tab = tab;
  for (const b of document.querySelectorAll(".tab")) b.classList.toggle("active", b.dataset.tab === tab);
  for (const p of document.querySelectorAll(".page")) p.classList.toggle("active", p.id === "page-" + tab);
  if (tab === "settings") loadSettings(S.settings.title);
  if (tab === "logs") renderLogs();
}

function renderWarnings() {
  const ov = S.ov;
  const notes = ov ? ov.warnings.map((w) => h("div", { class: "notice" }, w)) : [];
  if (ov && ov.move_to) {
    notes.push(h("div", { class: "notice" }, "The launcher can move itself and fci-recomp there for you. ",
      h("button", { class: "btn small", onclick: () => moveDialog() }, `Move to ${ov.move_to}`)));
  }
  if (ov && ov.moved_from) notes.push(h("div", { class: "notice" }, `fci-recomp moved here from ${ov.moved_from}. That folder isn't needed anymore; you can delete it.`));
  $("warnings").replaceChildren(...notes);
}

// request: the build that is waiting, when the move is offered before it starts
function moveDialog(request) {
  const ov = S.ov;
  modal(
    h("h3", {}, `Move to ${ov.move_to}?`),
    request && h("p", {}, "This folder may make the build fail:"),
    request && h("div", { class: "warn-list" }, ov.warnings.map((w) => h("div", { class: "notice" }, w))),
    h("p", {}, "The launcher copies itself, fci-recomp and your disc choices there and starts again from there. Anything half-built here is built again. This folder stays as it is; you can delete it afterwards."),
    h("div", { class: "buttons" },
      request
        ? h("button", { class: "btn", onclick: () => { closeModal(); S.buildHere = true; start(request); } }, "Build here anyway")
        : h("button", { class: "btn", onclick: closeModal }, "Cancel"),
      h("button", { class: "btn primary", onclick: moveNow }, "Move"),
    ),
  );
}

async function moveNow() {
  closeModal();
  toast("Moving fci-recomp…", 30000);
  try {
    await invoke("move_launcher");
  } catch (e) {
    toast(String(e), 8000);
  }
}

// ---------------------------------------------------------------- rendering: cards

function renderCards() {
  if (!S.ov) return;
  $("cards").replaceChildren(...S.ov.titles.map(card));
}

function card(t) {
  const task = S.task;
  const busyHere = task && (task.title === t.key || (task.kind === "update" && !task.title && t.game));
  const disc = t.disc && t.disc.ok ? t.disc : S.discFail[t.key] || null;
  const art = ART[t.key];
  const running = t.running.includes("game");
  const chips = [];
  if (busyHere) chips.push(h("span", { class: "chip accent" }, task.kind === "update" ? "Updating" : "Building"));
  else if (t.game && t.outdated) chips.push(h("span", { class: "chip warn" }, "Update ready"));
  else if (t.game) chips.push(h("span", { class: "chip ok" }, "Ready to play"));
  else if (disc && disc.ok) chips.push(h("span", { class: "chip ok" }, "Disc ready"));
  else if (disc) chips.push(h("span", { class: "chip bad" }, "Disc not supported"));
  else if (t.installed) chips.push(h("span", { class: "chip warn" }, "Build not finished"));
  else chips.push(h("span", { class: "chip" }, "Not built"));
  if (t.game && t.built_version) chips.push(h("span", { class: "chip plain" }, "v" + t.built_version));
  if (t.running.length) chips.push(h("span", { class: "chip accent" }, "Running"));

  let body;
  if (busyHere) {
    body = h("div", { class: "card-progress" },
      h("div", { class: "row" }, h("span", { "data-status": "" }, task.status), h("strong", {}, h("span", { "data-pct": "" }, Math.floor(task.percent)), "%")),
      h("div", { class: "bar" }, h("span", { "data-bar": "", style: `width:${task.percent}%` })),
    );
  } else if (!t.game) {
    const discBlock = disc && h("div", { class: "disc" + (disc.ok ? "" : " bad") },
      h("span", { class: "name", title: disc.path }, fileName(disc.path)),
      h("span", { class: "sub" }, disc.message),
    );
    if (disc && disc.ok) {
      const editorToo = S.editorToo[t.key] ?? true;
      body = h("div", { class: "actions" },
        discBlock,
        h("button", { class: "btn primary big", disabled: !!task, onclick: () => start({ kind: "game", title: t.key, editor: S.editorToo[t.key] ?? true }) },
          t.installed ? "Finish building" : "Build game"),
        h("label", { class: "check" },
          h("input", { type: "checkbox", checked: editorToo, onchange: (e) => (S.editorToo[t.key] = e.target.checked) }),
          "Also build the map editor"),
        h("div", { class: "hint" }, h("button", { class: "link", disabled: !!task, onclick: () => pickDisc(t.key) }, "Use another disc")),
      );
    } else {
      body = h("div", { class: "actions" },
        discBlock,
        h("button", { class: "btn primary big", disabled: !!task, onclick: () => pickDisc(t.key) }, disc ? "Choose another disc" : "Choose disc"),
        h("div", { class: "hint" }, "or drop the disc image here · ",
          h("button", { class: "link", disabled: !!task, onclick: () => pickDisc(t.key, true) }, "choose a folder")),
      );
    }
  } else {
    const editorBtn = t.editor
      ? h("button", { class: "btn", disabled: t.running.includes("editor"), onclick: () => launch(t.key, true) }, "Map Editor")
      : h("button", {
          class: "btn", disabled: !!task || t.outdated || !t.editor_available,
          title: t.outdated ? "Update the game first" : null,
          onclick: () => start({ kind: "editor", title: t.key }),
        }, "Build Map Editor");
    body = h("div", { class: "actions" },
      t.outdated && h("button", { class: "btn update", disabled: !!task, onclick: () => start({ kind: "update", title: t.key }) },
        `Update to ${S.ov.source.version}`),
      h("button", { class: "btn primary big", disabled: running, onclick: () => launch(t.key, false) }, running ? "Running" : "Play"),
      h("div", { class: "row" },
        editorBtn,
        h("button", { class: "btn icon-only", title: "More", "aria-label": "More", html: '<svg viewBox="0 0 24 24" width="18" height="18"><circle cx="5" cy="12" r="1.8"/><circle cx="12" cy="12" r="1.8"/><circle cx="19" cy="12" r="1.8"/></svg>',
          onclick: (e) => cardMenu(e.currentTarget, t) }),
      ),
    );
  }
  return h("article", { class: `card ${t.game ? "ready" : "mono"}${busyHere ? " busy" : ""}`, "data-key": t.key },
    h("div", { class: "art" + (!t.game && art.mono ? " has-mono" : ""), style: `background-image:url(${!t.game && art.mono ? art.mono : art.color})` }),
    h("div", { class: "scrim" }),
    h("div", { class: "card-body" }, h("div", { class: "chips" }, chips), body),
  );
}

function cardMenu(anchor, t) {
  const disc = t.disc && t.disc.ok ? t.disc : null;
  popover(anchor, [
    { label: "Open the game's folder", run: () => open("title:" + t.key) },
    { label: "Open crash logs", run: () => open("gamelogs:" + t.key) },
    { label: "Settings", run: () => { S.settings.title = t.key; switchTab("settings"); } },
    "-",
    { label: "Rebuild the game", run: () => (disc ? confirmRebuild(t) : pickDisc(t.key)) },
    t.editor && { label: "Rebuild the map editor", run: () => start({ kind: "editor", title: t.key }) },
    { label: disc ? "Use another disc" : "Choose the disc", run: () => pickDisc(t.key) },
  ].filter(Boolean));
}

function confirmRebuild(t) {
  modal(
    h("h3", {}, `Rebuild ${t.name}?`),
    h("p", {}, "Only what changed since the last build is redone, so this is quick unless something is broken. Settings and saves are kept."),
    h("div", { class: "buttons" },
      h("button", { class: "btn", onclick: closeModal }, "Cancel"),
      h("button", { class: "btn primary", onclick: () => { closeModal(); start({ kind: "game", title: t.key, editor: t.editor }); } }, "Rebuild"),
    ),
  );
}

async function launch(key, editor) {
  await call("launch", { title: key, editor }).catch(() => {});
  setTimeout(refresh, 600);
}

// ---------------------------------------------------------------- rendering: side panel

function renderSide() {
  const side = $("side");
  if (!S.ov) return;
  const t = S.task;
  if (t) {
    side.replaceChildren(
      h("h2", {}, t.label),
      h("div", { class: "big-progress" },
        h("div", { class: "percent" }, h("span", { "data-pct": "" }, Math.floor(t.percent)), h("small", {}, "%")),
        h("div", { class: "bar" }, h("span", { "data-bar": "", style: `width:${t.percent}%` })),
        h("div", { class: "status-line", "data-status": "" }, t.status),
        h("div", { class: "detail-line", "data-detail": "" }, t.detail || " "),
      ),
      h("div", { class: "meta" }, h("span", { "data-elapsed": "" }, "0:00 elapsed"), h("span", {}, "A first build takes 15 to 25 minutes")),
      t.warnings.length && h("div", { class: "warn-list" }, t.warnings.map((w) => h("div", { class: "notice" }, w))),
      h("p", { class: "muted", style: "margin:0;font-size:13px" }, "Your build is in progress. You can stop and resume later at any time."),
      h("div", { class: "side-foot" }, h("button", { class: "btn danger", onclick: () => invoke("cancel") }, "Stop"), h("button", { class: "btn ghost", onclick: () => open("logs") }, "Open logs folder")),
    );
    return;
  }
  const r = S.result;
  if (r && !r.ok) {
    side.replaceChildren(
      h("h2", {}, "Build stopped"),
      h("div", { class: "error-box" }, h("div", { class: "title" }, `${r.label} failed`), h("div", { class: "msg" }, r.error || "Unknown error")),
      h("p", { class: "muted", style: "margin:0;font-size:13px" }, "Building again resumes where it stopped. If it fails again, send the report: it says what went wrong and on what kind of PC, without your name or any game files."),
      h("div", { class: "btn-row" },
        h("button", { class: "btn primary", onclick: () => start(r.request) }, "Try again"),
        h("button", { class: "btn", onclick: () => reportDialog(r.log) }, "Report the problem"),
        h("button", { class: "btn ghost", onclick: () => open("log:" + r.log) }, "Show the log"),
      ),
      h("div", { class: "side-foot" }, h("button", { class: "btn ghost", onclick: () => { S.result = null; renderSide(); } }, "Dismiss"),
        h("button", { class: "btn ghost", onclick: () => open("url:" + DISCORD) }, "Ask on Discord")),
    );
    return;
  }
  const parts = [];
  if (r && r.ok) {
    const name = r.title && titleOf(r.title) ? titleOf(r.title).name : "The games";
    const text = r.kind === "update" ? `${name} ${r.title ? "is" : "are"} up to date.`
      : r.kind === "editor" ? `The ${name} map editor is ready. Start it here, or from the game's Map Editor menu.`
      : `${name} is ready. Press Play.`;
    parts.push(h("div", { class: "done-box" },
      h("span", {}, text, r.minutes ? ` Built in ${r.minutes} min.` : ""),
      h("button", { class: "btn ghost small", onclick: () => { S.result = null; renderSide(); } }, "OK")));
  }
  const discReady = S.ov.titles.some((t) => t.disc && t.disc.ok);
  if (!anyBuilt()) {
    parts.push(
      h("h2", {}, "Build your games"),
      h("ol", { class: "steps" },
        h("li", { class: discReady ? "done" : "" }, h("div", {}, h("strong", {}, "Choose your disc"), h("span", {}, "Provide an ISO of your own disc, or a directory of extracted game data."))),
        h("li", {}, h("div", {}, h("strong", {}, "Build"), h("span", {}, "Build the game. See the requirements section of the README or drop by the Discord if something goes wrong. Building the map editor for either title is optional."))),
        h("li", {}, h("div", {}, h("strong", {}, "Play"), h("span", {}, "Adjust settings to your liking from the Settings tab above, then launch a title."))),
      ),
    );
  } else {
    const rel = S.updates && S.updates.release;
    if (rel) {
      parts.push(h("h2", {}, rel.name), h("div", { class: "date" }, `Released ${rel.published}`),
        h("div", { class: "notes", html: notesHtml(rel.notes) }));
    } else {
      parts.push(h("p", { class: "muted" }, "Release notes appear here when the launcher is online."));
    }
  }
  parts.push(h("div", { class: "side-foot" },
    h("button", { class: "btn ghost small", title: "fci-recomp on Discord", "aria-label": "Discord", html: ICONS.discord, onclick: () => open("url:" + DISCORD) }),
    h("button", { class: "btn ghost small", title: GITHUB.replace("https://", ""), html: ICONS.github + "<span>GitHub</span>", onclick: () => open("url:" + GITHUB) }),
    h("button", { class: "btn ghost small", html: ICONS.folder + "<span>Open Directory…</span>", onclick: (e) => directoryMenu(e.currentTarget) }),
  ));
  side.replaceChildren(...parts);
}

// the folders a player looks for: where each game is, where it keeps its saves
function directoryMenu(anchor) {
  const items = [];
  for (const t of S.ov.titles) {
    if (items.length) items.push("-");
    items.push(
      { label: `${SHORT[t.key]}: game directory`, run: () => (t.installed ? open("title:" + t.key) : toast(`${t.name} isn't built yet.`)) },
      { label: `${SHORT[t.key]}: save directory`, run: () => open("saves:" + t.key) },
    );
  }
  items.push("-", { label: "Launcher directory", run: () => open("root") });
  popover(anchor, items);
}

// ---------------------------------------------------------------- reports and logs

async function reportDialog(path) {
  let r;
  try {
    r = await invoke("report", { path: path || null });
  } catch (e) {
    toast(String(e), 6000);
    return;
  }
  modal(
    h("h3", {}, "Report the problem"),
    h("p", {}, "This is what the report says. GitHub opens with it filled in; attach the log file there too (the folder with it opens as well). Or paste the report on Discord."),
    h("pre", {}, r.text),
    h("div", { class: "buttons" },
      h("button", { class: "btn", onclick: closeModal }, "Close"),
      h("button", { class: "btn", onclick: async () => { await navigator.clipboard.writeText(r.text); toast("Report copied."); } }, "Copy report"),
      h("button", { class: "btn primary", onclick: async () => {
        await navigator.clipboard.writeText(r.text).catch(() => {});
        open("url:" + r.url);
        open("log:" + r.log);
      } }, "Open a GitHub issue"),
    ),
  );
}

async function renderLogs() {
  let logs = [];
  try {
    logs = await invoke("logs_list");
  } catch (e) {
    toast(String(e));
  }
  const mark = { ok: "✓", failed: "!", stopped: "–", running: "…" };
  $("logs-list").replaceChildren(
    ...(logs.length
      ? logs.map((l) => h("div", { class: "log-row" },
          h("span", { class: "icon " + l.result }, mark[l.result]),
          h("div", {}, h("div", { class: "what" }, l.label, h("span", { class: "when" }, l.when)), l.error && h("div", { class: "err", title: l.error }, l.error)),
          h("div", { class: "btn-row" },
            l.result === "failed" && h("button", { class: "btn small", onclick: () => reportDialog(l.path) }, "Report"),
            h("button", { class: "btn small ghost", onclick: () => open("log:" + l.path) }, "Show file"),
          ),
        ))
      : [h("p", { class: "muted" }, "No builds yet.")]),
  );
}
for (const b of document.querySelectorAll("[data-open]")) b.addEventListener("click", () => open(b.dataset.open));
const LINKS = { discord: DISCORD, issues: GITHUB + "/issues" };
for (const b of document.querySelectorAll("[data-link]")) b.addEventListener("click", () => open("url:" + LINKS[b.dataset.link]));

// ---------------------------------------------------------------- settings

const LABELS = {
  "save-dir": "Save folder", "shader-cache": "Shader cache folder", "log-verbosity": "Logging", "log-limit": "Log files kept",
  "log-dir": "Log folder", "log-input": "Log controller input", "display-borderless": "Borderless fullscreen",
  "display-dimensions": "Window size", "display-aspect": "Aspect ratio", "display-frame-rate": "Frame rate",
  "engine-update-cap": "Engine update cap", "skip-intro-movies": "Skip intro movies", "gfx-res-scale": "Resolution scale",
  "gfx-anisotropic-filtering": "Anisotropic filtering", "gfx-ssao": "Ambient occlusion", "gfx-ssao-strength": "Ambient occlusion strength",
  "gfx-ssao-radius": "Ambient occlusion radius", "gfx-aa": "Anti-aliasing", "gfx-shadow-scale": "Shadow detail",
  "gfx-shadow-filter": "Soft shadows", "gfx-object-draw-distance": "Object draw distance", "gfx-object-lod-distance": "Object detail distance",
  "gfx-foliage-lod": "Foliage detail distance", "controls-mouse-keyboard": "Mouse and keyboard", "controls-mouse-aim": "Direct mouse aim",
  "controls-mouse-y-scale": "Vertical sensitivity", "controls-mouse-invert-y": "Invert vertical look", "controls-mouse-capture": "Capture the mouse",
};
// shorter names for choices the ini explains at length
const CHOICES = {
  "engine-update-cap": { 1: "On (as on the Xbox)", 0: "Off (smoother, may break AI)" },
};
const BUTTONS = { rt: "RT", lt: "LT", rs: "Right stick click", ls: "Left stick click", a: "A", b: "B", x: "X", y: "Y", black: "Black", white: "White", start: "Start", back: "Back" };

function humanize(o) {
  if (LABELS[o.key]) return LABELS[o.key];
  if (o.kind === "keys" && o.label) return o.label[0].toUpperCase() + o.label.slice(1);
  const words = o.key.replace(/^(controls-bind-|controls-mouse-stick-|controls-mouse-|controls-|gfx-|display-|log-)/, "").split("-");
  const text = words.join(" ");
  return text[0].toUpperCase() + text.slice(1);
}
function bindName(o) {
  const b = o.key.replace("controls-bind-", "");
  if (BUTTONS[b]) return "Xbox " + BUTTONS[b];
  return b.startsWith("dpad-") ? "D-pad " + b.slice(5) : null;
}

// the build cache: its size on the button, and clearing it after a warning
function formatSize(bytes) {
  return bytes >= 1e9 ? `${(bytes / 1e9).toFixed(1)} GB` : `${Math.max(1, Math.round(bytes / 1e6))} MB`;
}

async function loadCache() {
  const btn = $("clear-cache");
  try {
    S.cache = await invoke("build_cache");
  } catch (e) {
    S.cache = null;
  }
  const bytes = S.cache ? S.cache.bytes : 0;
  btn.textContent = bytes ? `Clear build cache (${formatSize(bytes)})` : "Build cache is empty";
  btn.disabled = !bytes || !!S.task;
}

$("clear-cache").addEventListener("click", () => {
  if (!S.cache) return;
  const size = formatSize(S.cache.bytes);
  modal(
    h("h3", {}, "Clear build cache?"),
    h("p", {}, `This will free ${size} from ${S.cache.drive}, but require a full rebuild on the next update.`),
    h("div", { class: "buttons" },
      h("button", { class: "btn", onclick: closeModal }, "Cancel"),
      h("button", { class: "btn danger", onclick: async () => {
        closeModal();
        $("clear-cache").disabled = true;
        $("clear-cache").textContent = "Clearing…";
        await call("clear_build_cache").then(() => toast(`Freed ${size}.`)).catch(() => {});
        loadCache();
      } }, `Clear ${size}`),
    ),
  );
});

async function loadSettings(key) {
  S.settings.title = key;
  loadCache();
  try {
    const r = await invoke("settings_get", { title: key });
    Object.assign(S.settings, { options: r.options, available: r.available, reason: r.reason, edits: {} });
  } catch (e) {
    Object.assign(S.settings, { options: [], available: false, reason: String(e), edits: {} });
  }
  renderSettings();
}

function valueOf(o) {
  return o.key in S.settings.edits ? S.settings.edits[o.key] : o.value;
}

function setValue(o, v) {
  if (String(v) === String(o.value)) delete S.settings.edits[o.key];
  else S.settings.edits[o.key] = String(v);
  renderSaveBar();
  const row = document.querySelector(`[data-opt="${o.key}"]`);
  if (row) row.replaceWith(optionRow(o));
}

function control(o) {
  const v = valueOf(o);
  if (o.kind === "toggle") {
    return h("span", { class: "switch" },
      h("input", { type: "checkbox", checked: v === "1", onchange: (e) => setValue(o, e.target.checked ? "1" : "0") }), h("span"));
  }
  if (o.kind === "choice") {
    const choices = o.choices.some((c) => c.value === v) ? o.choices : [...o.choices, { value: v, label: v }];
    return h("select", { class: "field", onchange: (e) => setValue(o, e.target.value) },
      choices.map((c) => {
        const text = (CHOICES[o.key] && CHOICES[o.key][c.value]) || c.label.split(/[.;]\s|:\s/)[0].trim();
        return h("option", { value: c.value, selected: c.value === v }, text[0].toUpperCase() + text.slice(1));
      }));
  }
  return h("input", {
    class: "field", type: "text", value: v, spellcheck: "false", inputmode: o.kind === "number" ? "decimal" : null,
    style: o.kind === "keys" ? "text-transform:uppercase;font-family:ui-monospace,Consolas,monospace" : null,
    onchange: (e) => setValue(o, o.kind === "keys" ? e.target.value.toUpperCase().replace(/\s+/g, "") : e.target.value.trim()),
  });
}

function optionRow(o) {
  const v = valueOf(o);
  const changed = v !== o.default;
  const help = o.kind === "keys" ? [] : o.help;
  return h("div", { class: "opt", "data-opt": o.key },
    h("div", {},
      h("div", { class: "label" }, humanize(o), changed && h("span", { class: "dot", title: "Changed from the default" })),
      help.length ? h("div", { class: "help" + (help.join(" ").includes("EXPERIMENTAL") ? " warn" : "") }, help.join(" ")) : null,
      h("div", { class: "key" }, o.kind === "keys" && bindName(o) ? `${bindName(o)} · ${o.key}` : o.key),
    ),
    h("div", { class: "control" },
      control(o),
      h("button", { class: "reset" + (changed ? "" : " invisible"), title: `Default: ${o.default}`, onclick: () => setValue(o, o.default) }, "↺"),
    ),
  );
}

function renderSettings() {
  const st = S.settings;
  const other = st.title === "instincts" ? "evolution" : "instincts";
  $("settings-title").replaceChildren(
    ...["instincts", "evolution"].map((k) => h("button", { class: st.title === k ? "active" : "", onclick: () => {
      if (k === st.title) return;
      if (Object.keys(st.edits).length && !confirm("Discard the settings you haven't saved?")) return;
      loadSettings(k);
    } }, SHORT[k])),
  );
  $("sync").checked = !!(S.ov && S.ov.sync_settings);
  const copy = $("settings-copy");
  copy.textContent = `Copy these to ${SHORT[other]}`;
  copy.disabled = !st.available || !(titleOf(other) && titleOf(other).game);
  const list = $("settings-list");
  const nav = $("settings-sections");
  if (!st.available) {
    nav.replaceChildren();
    list.replaceChildren(h("div", { class: "empty" }, h("strong", {}, "No settings yet"), h("span", {}, st.reason),
      h("button", { class: "btn", onclick: () => switchTab("games") }, "Go to Games")));
    renderSaveBar();
    return;
  }
  // what players change most first; the folders last
  const order = ["Display", "Graphics", "Controls", "Mouse", "Keyboard", "Key bindings", "Mouse as right stick", "System"];
  const rank = (s) => (order.includes(s) ? order.indexOf(s) : order.length - 1);
  const sections = [...new Set(st.options.map((o) => o.section))].sort((a, b) => rank(a) - rank(b));
  nav.replaceChildren(...sections.map((s) => h("button", { onclick: () => $("sec-" + s.replace(/\W+/g, "-")).scrollIntoView() }, s)));
  const kids = [];
  for (const s of sections) {
    kids.push(h("h3", { id: "sec-" + s.replace(/\W+/g, "-") }, s));
    if (s === "Key bindings") {
      kids.push(h("p", { class: "muted", style: "margin:0 0 4px;font-size:12.5px" },
        "Keys and mouse buttons replace an Xbox controller. Separate several inputs with commas; NONE disables one. Inputs: A–Z, 0–9, F1–F24, SPACE, ENTER, ESCAPE, TAB, LSHIFT, LCTRL, LALT, UP, DOWN, LEFT, RIGHT, NUMPAD0–9, MOUSE1–MOUSE5, WHEELUP, WHEELDOWN and more (see fci.ini)."));
    }
    kids.push(...st.options.filter((o) => o.section === s).map(optionRow));
  }
  list.replaceChildren(...kids);
  renderSaveBar();
}

function renderSaveBar() {
  const n = Object.keys(S.settings.edits).length;
  $("savebar").hidden = n === 0;
  const sync = $("sync").checked;
  $("savebar-text").textContent = `${n} unsaved change${n === 1 ? "" : "s"}` + (sync ? " · saved for both games" : "");
}

$("settings-save").addEventListener("click", async () => {
  const st = S.settings;
  const changes = Object.entries(st.edits).map(([key, value]) => ({ key, value }));
  const saved = await call("settings_save", { title: st.title, changes, sync: $("sync").checked }).catch(() => null);
  if (!saved) return;
  toast(`Saved for ${saved.map((k) => SHORT[k]).join(" and ")}. It applies the next time the game starts.`);
  await refresh();
  loadSettings(st.title);
});
$("settings-discard").addEventListener("click", () => {
  S.settings.edits = {};
  renderSettings();
});
$("sync").addEventListener("change", async (e) => {
  await call("settings_save", { title: S.settings.title, changes: [], sync: e.target.checked }).catch(() => {});
  if (S.ov) S.ov.sync_settings = e.target.checked;
  renderSaveBar();
  toast(e.target.checked ? "Settings you save now apply to both games (each keeps its own folders)." : "Each game keeps its own settings.");
});
$("settings-copy").addEventListener("click", () => {
  const from = S.settings.title;
  const to = from === "instincts" ? "evolution" : "instincts";
  modal(
    h("h3", {}, `Copy ${SHORT[from]}'s settings to ${SHORT[to]}?`),
    h("p", {}, `Every option of ${titleOf(to).name} is set the same as ${titleOf(from).name}'s, except the folders each game keeps for itself (saves, logs, shader cache).`),
    h("div", { class: "buttons" },
      h("button", { class: "btn", onclick: closeModal }, "Cancel"),
      h("button", { class: "btn primary", onclick: async () => {
        closeModal();
        await call("settings_copy", { from, to }).then(() => toast(`Copied to ${SHORT[to]}.`)).catch(() => {});
      } }, "Copy"),
    ),
  );
});

// ---------------------------------------------------------------- start

window.addEventListener("focus", () => { if (!S.task) refresh(false); });
setInterval(() => { if (!S.task && document.hasFocus()) refresh(false); }, 10000);
setInterval(() => checkUpdates(), 30 * 60 * 1000);
refresh().then(() => checkUpdates());
