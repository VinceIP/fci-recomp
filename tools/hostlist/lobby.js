// The host list: a Cloudflare Worker with one Durable Object (lobby.js, research/docs/online-architecture.md, sections 3-5).
//
// The game's card model (src/fci_nic.c, RECOMP_FCI_NET_LIST mode) talks to this over plain HTTP(S):
//   GET  /v1/hosts?title=&version=[&max=]
//   POST /v1/register
//   GET  /v1/connects?id=&token=&wait=<1..60>   (long-poll; a host loops this)
//   POST /v1/connect
//   POST /v1/turn   (mints a Cloudflare TURN credential for a guest whose punch to `id` failed)
//   POST /v1/unregister   (a leaving host removes its own listing; without it it lingers until its TTL)
// and the operator's admin CLI (tools/hostlist/admin.py):
//   GET    /admin/hosts
//   GET    /admin/bans
//   POST   /admin/bans
//   DELETE /admin/bans?id=
//   GET    /admin/abuse
//   GET    /admin/relay
//
// What the object stores, and why it is safe to run for strangers:
// - A host registers with the identity token the player's runtime generated (32 random bytes, hex;
//   it lives in the game's data folder, see fci_nic.c). The host's id is the first 16 bytes of
//   SHA-256(token) (32 hex): a banned id is a stable id, and no endpoint, MAC or token is ever
//   listed. The token is checked on every poll and connect (it derives to the id) and is never
//   stored, logged, or returned.
// - The listing returns ids only. A guest posts a connect (its own token, public endpoint, nonce,
//   MAC); the reply carries the host's endpoint and MAC, and the host receives the guest's over
//   its long-poll (up to 8 queued, the NIC's pending cap). A connect from the same guest id while
//   one is still queued is not queued again (a replay).
// - An endpoint must be a public (not reserved) IPv4 with a valid port. It need not equal the
//   address the Worker sees the client connect from (a dual-stack home, or a VPN, sees a different
//   address): the harm of a hostile endpoint is bounded by per-endpoint-IP caps and the per-host
//   budget instead, and the residual is written in section 5 of the doc.
// - Rate limits per client address: 30 listings, 60 connects, 10 registers a minute. Per host:
//   24 connects a minute from all addresses (flood budget, section 5). Mints: 6 per guest and per
//   address in 10 minutes, and only for a (guest, nonce) that connected to that host within the
//   last 10 minutes (the connect queue is drained by the host's poll, so the recent pairs live in
//   their own small map). A guest that cannot hole-punch mints once, ALLOCATEs, and posts its
//   relayed address back as a second /v1/connect with the same nonce and "relay": true (capped at
//   3 per nonce: ICE restarts); the host receives it on its long-poll. One mint (a TURN
//   allocation) covers every host the guest reached directly: a relayed connect is accepted with
//   a mint for the guest within the window (any nonce, any host) and a non-relay connect of the
//   (guest, nonce) to that host within the window; a mint for the exact (guest, nonce, host)
//   still qualifies. The host never mints.
//   The TURN key is env.TURN_KEY_API_TOKEN (a wrangler secret); the key id is env.TURN_KEY_ID
//   (a wrangler secret). Neither is in the repo, and Cloudflare's answer is never echoed.
// - The operator's relay budget: RELAY_BUDGET_GB (a wrangler secret, GB), TURN_USAGE_API_TOKEN
//   (a wrangler secret, a least-privilege API token with the Account Analytics permission) and
//   TURN_USAGE_ACCOUNT_ID (a wrangler secret) together turn the quota on; with any of them
//   missing it is off (one log line on the first mint). With the quota on, the
//   month's TURN egress is checked before every mint: it is read from Cloudflare's GraphQL
//   analytics API at most every 5 minutes and cached in the object's SQLite for the UTC calendar
//   month (the provider's billing period); at or past the budget the mint is refused 503
//   `no relay`, the status the client already treats as relay unavailable. A cached value up to
//   1 h old is trusted when the read fails; with nothing that fresh the mint is refused (fail
//   closed: the budget must hold). RELAY_KILL (a wrangler secret, any non-empty value)
//   blocks mints regardless of quota configuration. The operator sets it: `wrangler secret put RELAY_KILL` sets it, `wrangler secret
//   delete RELAY_KILL` clears it; each creates a new Worker version, no code redeploy. The
//   refusal is the same `no relay` as when the TURN key is unset and is never explained to the
//   caller. The operator is told by state change, not per refusal: budget reached (the
//   month's usage and budget in GB), usage unreadable (the read's HTTP status or the
//   exception's name), relay killed and recovered are each one log line and, when
//   RELAY_ALERT_WEBHOOK is set (a wrangler secret, an https URL such as a Discord webhook),
//   one fire-and-forget POST of {"content": "..."} to it (at most 6 an hour; a failure never
//   delays or fails a mint; no webhook set: nothing sent). GET /admin/relay reports the
//   current alert state and when it last changed.
// - Bans (a banned id cannot host, and cannot join anyone) persist in the object's SQLite, so they
//   survive an eviction; the refusal counters and a ring of recent refusals are memory-only.
//
// The state is tiny (a few hundred bytes per live host) and all of it is in this one object's
// memory: a long-poll keeps the object awake, so every poll is held here, never an object per
// host. An entry is a host that keeps polling; a host that stops stays listed until its TTL runs
// out (a poll is the renewal). Nothing important survives an eviction (a host re-registers),
// except the bans.
//
// Deploy: wrangler.toml (one Worker, one DO class, `Lobby`). Every setting (RELAY, TURN_KEY_ID,
// TURN_KEY_API_TOKEN, RELAY_BUDGET_GB, TURN_USAGE_ACCOUNT_ID, TURN_USAGE_API_TOKEN, ADMIN_SECRET,
// RELAY_KILL, RELAY_ALERT_WEBHOOK, MIN_BUILD, MAINTENANCE) is a secret (`wrangler secret put`): secrets survive `wrangler deploy`, which replaces
// plain variables with wrangler.toml's (none), and no deployment's values live in the repo.
// Free tier.
// The how-to is research/docs/online-architecture.md, sections 7 and 7a (versions and updates).

const PROTO = 2; // the net protocol (src/fci_net.h): title + version + this must all match
const LIST_MAX = 64;
const TITLES = new Set(["instincts", "evolution"]);
const VERSION = /^[0-9]{1,5}$/;
const MAC = /^02:46:43(:[0-9a-f]{2}){3}$/;
const NONCE = /^[0-9a-f]{32}$/;
const REPORT_BUILD = /^[a-zA-Z0-9][a-zA-Z0-9._-]{0,39}$/;
const REPORT_LIMIT = 256;
const REPORT_TTL = 120000;
const ID = /^[0-9a-f]{32}$/; // 16 bytes of SHA-256(token), hex
const TOKEN = /^[0-9a-f]{64}$/; // 32 random bytes, hex
// Kept above the maximum long-poll wait (60 s) so a full-duration poll can never race the expiry
// and flap the host. A host that stops polling leaves after this (a poll is the renewal).
const TTL_MS = 90000;
const CONNECTS_MAX = 8; // the NIC's pending-peer cap (src/fci_nic.c)
const HOST_CONNECTS_PER_MINUTE = 24; // per-host flood budget (section 5)
const MESSAGE_MAX = 512; // register, connect and admin bodies
const BANS_RING = 1000; // recent refusals kept in memory
export const TURN_WINDOW_MS = 600000; // a mint (and a relayed connect) is valid for 10 minutes after the connect
const TURN_MAPS_MAX = 1000; // the recent-pairs and mints maps are tiny by construction; cap them anyway
const HOSTS_MAX = 1024; // the global live-host cap: the per-address caps bound one client, this bounds a flood of them
const MINTS_PER_WINDOW = 6; // mints per guest and per address, per 10-minute window
const RELAY_CONNECTS_PER_NONCE = 3; // re-POSTs of a relayed address (ICE restarts)
const TURN_TTL = 14400; // s; the credential must outlive the longest relayed session: a TURN
// refresh authenticates with the same long-term credential, so a short one would drop a running relay.
const USAGE_READ_EVERY_MS = 5 * 60 * 1000; // the month's usage, read at most every 5 minutes
const USAGE_STALE_MS = 60 * 60 * 1000; // a cached usage older than 1 h with a failed read: fail closed
// per address: generous enough for players sharing one public IPv4 (carrier NAT, a university, one VPN exit)
const LIMITS = {
  hostsPerIp: 16,
  hostsPerEndpointIp: 16, // live hosts that may list the same endpoint IP (a hostile endpoint is bounded)
  connectsPerEndpointIp: 8, // pending connects that may target the same endpoint IP
  listsPerMinute: 240, // a browsing game fetches every 10 s: 40 browsers on one address
  connectsPerMinute: 120,
  registersPerMinute: 40,
  unregistersPerMinute: 10, // a leaving host removes its own listing (one per session, in practice)
};

const json = (body, status = 200) =>
  new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json" },
  });

// 204 must have no body (Node's Response rejects a body with 204).
const noContent = () => new Response(null, { status: 204 });

// A public (not reserved, not a test or documentation range) IPv4.
const publicIp = (a, b, c) =>
  a > 0 &&
  !(a === 10) &&
  !(a === 172 && b >= 16 && b <= 31) &&
  !(a === 192 && b === 168) &&
  !(a === 127) &&
  !(a === 100 && b >= 64 && b <= 127) &&
  !(a === 169 && b === 254) &&
  !(a === 192 && b === 0 && c === 2) &&
  !(a === 198 && b === 51 && c === 100) &&
  !(a === 203 && b === 0 && c === 113) &&
  !(a === 224) &&
  !(a >= 240);

// The IP part of a validated "ip:port" endpoint.
const endpointIp = (endpoint) => String(endpoint).slice(0, String(endpoint).lastIndexOf(":"));

// The endpoint must be a public (not reserved) IPv4 with a valid port. It is NOT required to equal
// the address the Worker sees the client connect from: a dual-stack home reaches the Worker over
// IPv6 while the game registers its IPv4 STUN endpoint, and a VPN can route the game's UDP through
// a server while HTTP leaves from home. The harm of a hostile endpoint is bounded, not prevented:
// per-endpoint-IP caps (hostsPerEndpointIp, connectsPerEndpointIp) and the per-host budget, and the
// residual is written in section 5 of research/docs/online-architecture.md.
function endpointOk(endpoint) {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3}):(\d{1,5})$/.exec(String(endpoint));
  if (!m) return false;
  const a = Number(m[1]),
    b = Number(m[2]),
    c = Number(m[3]),
    d = Number(m[4]),
    port = Number(m[5]);
  return port > 0 && port <= 65535 && publicIp(a, b, c);
}

// Same NAT: the same caller IP, or the same IP in the STUN endpoints both sides reported. The
// endpoint test is needed because with IPv6 each device has its own caller IP (seen 2026-11-09).
function epHost(endpoint) {
  const s = String(endpoint || "");
  const i = s.lastIndexOf(":");
  return i > 0 ? s.slice(0, i) : "";
}
function sameNat(ipA, ipB, epA, epB) {
  if (ipA && ipA === ipB) return true;
  const a = epHost(epA);
  return a !== "" && a === epHost(epB);
}

// A private IPv4 (10/8, 172.16/12, 192.168/16) with a valid port.
// The card's LAN address: only forwarded when both sides are behind the same NAT
// (sameNat). 100.64/10 (CGNAT) is excluded:
// two phones behind the same carrier share no network.
function localOk(endpoint) {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3}):(\d{1,5})$/.exec(String(endpoint));
  if (!m) return false;
  const a = Number(m[1]), b = Number(m[2]), c = Number(m[3]), d = Number(m[4]), port = Number(m[5]);
  if (a < 1 || a > 255 || b > 255 || c > 255 || d > 255) return false;
  if (port <= 0 || port > 65535) return false;
  return (
    a === 10 ||
    (a === 172 && b >= 16 && b <= 31) ||
    (a === 192 && b === 168)
  );
}

// The player's id from the identity token: the first 16 bytes of SHA-256(token), hex.
async function deriveId(token) {
  const h = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(token));
  return [...new Uint8Array(h.slice(0, 16))].map((x) => x.toString(16).padStart(2, "0")).join("");
}

// A constant-time compare for the admin secret (the ids and tokens are compared with ===, which is
// fine: they are 128-bit values, not secrets with a timing surface worth defending here).
function secretOk(header, secret) {
  const got = String(header || "").slice("Bearer ".length);
  if (got.length !== secret.length) return false;
  let diff = 0;
  for (let i = 0; i < got.length; i++) diff |= got.charCodeAt(i) ^ secret.charCodeAt(i);
  return diff === 0;
}

function validRegistration(msg, ip) {
  return (
    msg.proto === PROTO &&
    TITLES.has(msg.title) &&
    VERSION.test(String(msg.version)) &&
    MAC.test(String(msg.mac)) &&
    endpointOk(msg.endpoint)
  );
}

class RateLimit {
  constructor(span = 60000) {
    this.span = span;
    this.m = new Map();
  }
  take(key, max, now) {
    const minute = Math.floor(now / this.span);
    let e = this.m.get(key);
    if (!e || e.minute !== minute) {
      e = { minute, n: 0 };
      this.m.set(key, e);
    }
    e.n++;
    if (this.m.size > 4000) {
      // drop the oldest windows so a flood of unique addresses can't grow this without bound
      for (const [k, v] of this.m) if (v.minute < minute) this.m.delete(k);
    }
    return e.n <= max;
  }
}

// Cloudflare's generate-ice-servers answer: iceServers is an array (a STUN-only entry, then the
// TURN entry with username and credential) or, in older shapes, a single object. We want the
// turn: UDP urls only: the cards relay over plain UDP, and the tcp/turns entries are for browsers.
function turnCredential(ice) {
  const list = Array.isArray(ice) ? ice : ice && typeof ice === "object" ? [ice] : [];
  for (const e of list) {
    if (!e || typeof e !== "object") continue;
    const username = String(e.username || "");
    const credential = String(e.credential || "");
    if (!username || !credential) continue;
    const urls = (Array.isArray(e.urls) ? e.urls : [])
      .map(String)
      .filter((u) => u.startsWith("turn:") && (u.includes("transport=udp") || !u.includes("transport=")));
    if (urls.length) return { username, credential, urls };
  }
  return null;
}

// Build versions (research/docs/online-architecture.md 7a): "0.3.0-beta.2" is the numbers, then a
// release above its pre-releases, then pre-releases by label and number. The launcher's paths.rs
// ranks the same way. A build that does not parse (0.3.0-beta.1 sends none) is older than any.
const BUILD = /^v?(\d{1,5})\.(\d{1,5})\.(\d{1,5})(?:-([a-z]{1,10})\.?(\d{1,5}))?(?:\+[0-9A-Za-z.-]{1,32})?$/;

export function compareBuilds(a, b) {
  const x = BUILD.exec(String(a ?? "")), y = BUILD.exec(String(b ?? ""));
  if (!x || !y) return (x ? 1 : 0) - (y ? 1 : 0);
  for (let i = 1; i <= 3; i++) if (+x[i] !== +y[i]) return +x[i] < +y[i] ? -1 : 1;
  if (!x[4] !== !y[4]) return x[4] ? -1 : 1; // the release above its pre-releases
  if (!x[4]) return 0;
  if (x[4] !== y[4]) return x[4] < y[4] ? -1 : 1; // alpha < beta < rc
  return Math.sign(+x[5] - +y[5]);
}

// Player routes the operator's switches apply to. Unregister is never refused: an old build still leaves.
const GATED = new Set([
  "GET /v1/hosts", "POST /v1/register", "GET /v1/connects", "POST /v1/connect", "POST /v1/turn",
  "POST /v1/probe-report",
]);

export class Lobby {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.ttl = (env && env.TTL_MS) || TTL_MS;
    this.relay = (env && env.RELAY) || "";
    this.hosts = new Map(); // id -> {id, ip, proto, title, version, mac, endpoint, expires, timer}
    this.connects = new Map(); // id -> [{endpoint, nonce, mac, gid, seq}]
    this.seq = 0; // connect numbers: one counter for every host, so a re-register never reuses one
    this.waiters = new Map(); // id -> {resolve, timer}
    this.limits = new RateLimit();
    this.hostConn = new Map(); // id -> {minute, n} (the per-host connect budget)
    this.pairs = new Map(); // gid+":"+nonce -> {at, hosts: {host: at}}: the guest's recent non-relay connects (the mint and relay gates)
    this.mints = new Map(); // gid+":"+nonce -> {host, at}: recent mints (the relay gate's exact-nonce path)
    this.gidMints = new Map(); // gid -> {at}: the guest's newest successful mint, any nonce (the relay gate)
    this.relayCounts = new Map(); // gid+":"+nonce -> {at, n}: accepted relay connects per nonce
    this.mintLimit = new RateLimit(TURN_WINDOW_MS); // mints per guest and per address
    this.abuse = { byIp: new Map(), ring: [] };
    this.reportReceipts = new Map();
    this.reportBuilds = new Map();
    this.metrics = {
      startedAt: Date.now(), routes: {},
      probes: { issued: 0, received: 0, results: {}, firstFrameMs: 0 },
      connects: { directQueued: 0, relayQueued: 0, directDelivered: 0, relayDelivered: 0 },
      relay: { issued: 0, unconfigured: 0, budget: 0, disabled: 0, usageUnavailable: 0, providerError: 0 },
    };
    // SqlStorage (Cloudflare): storage.sql.exec(query, ...bindings) returns a cursor
    state.storage.sql.exec(
      "CREATE TABLE IF NOT EXISTS bans (id TEXT PRIMARY KEY, reason TEXT NOT NULL DEFAULT '', date TEXT NOT NULL)"
    );
    state.storage.sql.exec(
      "CREATE TABLE IF NOT EXISTS relay_usage (k TEXT PRIMARY KEY, v TEXT NOT NULL)"
    );
    this.usage = null; // {month, bytes, at}: the month's egress, in memory (SQLite is the source)
    this.usageInFlight = null; // at most one in-flight usage read per object
    this.quotaOffLogged = false;
    this.alertState = "ok"; // the relay alert state: ok, budget, usage-unreadable, disabled
    this.alertChangedAt = null; // when the alert state last changed (GET /admin/relay)
    this.alertPosts = []; // webhook POST times (at most 6 an hour)
    this.usageReadError = null; // the last failed usage read's reason: an HTTP status, an exception name
  }

  // Memory of the refusals this object has seen (the admin CLI's `abuse` command); the ban itself
  // lives in SQLite.
  note(kind, ip, id) {
    const c =
      this.abuse.byIp.get(ip) || { ip, rate: 0, bad: 0, nohost: 0, banned: 0, norelay: 0 };
    c[kind]++;
    this.abuse.byIp.set(ip, c);
    this.abuse.ring.push({ ip, kind, id: id || null, at: Date.now() });
    if (this.abuse.ring.length > BANS_RING) this.abuse.ring.shift();
  }

  // The UTC calendar month a time falls in, as "YYYY-MM" (the provider's billing period is the
  // calendar month, so the budget resets there).
  monthKey(ms) {
    const d = new Date(ms);
    return d.getUTCFullYear() + "-" + String(d.getUTCMonth() + 1).padStart(2, "0");
  }

  // The operator's relay budget settings: on only when the budget, the usage token and the
  // account id are all set.
  quotaConfig() {
    const e = this.env || {};
    const gb = Number(e.RELAY_BUDGET_GB);
    if (!isFinite(gb) || gb <= 0) return { on: false, why: "RELAY_BUDGET_GB not set (or not a positive number)" };
    if (!e.TURN_USAGE_API_TOKEN) return { on: false, why: "TURN_USAGE_API_TOKEN not set" };
    if (!e.TURN_USAGE_ACCOUNT_ID) return { on: false, why: "TURN_USAGE_ACCOUNT_ID not set" };
    return { on: true, budgetBytes: gb * 1e9 };
  }

  // The operator's kill switch (a wrangler secret, any non-empty value): `wrangler secret put
  // RELAY_KILL` sets it, `wrangler secret delete RELAY_KILL` clears it; no code redeploy.
  relayKilled() {
    return String((this.env && this.env.RELAY_KILL) || "").length > 0;
  }

  // The operator's version and maintenance switches (secrets like RELAY_KILL): MAINTENANCE (any
  // non-empty value) pauses online play; MIN_BUILD turns away builds below it and those that send none.
  // The refusal for a gated route, or null.
  gate(route, build) {
    if (!GATED.has(route)) return null;
    const e = this.env || {};
    if (String(e.MAINTENANCE || "").length > 0) return json({ error: "maintenance" }, 503);
    const min = String(e.MIN_BUILD || "");
    if (!min) return null;
    if (!BUILD.test(min)) { // a typo must not lock everyone out: say so once and refuse nobody
      if (this.badMinBuild !== min) console.log(`MIN_BUILD "${min}" is not a version; no build is refused`);
      this.badMinBuild = min;
      return null;
    }
    if (compareBuilds(build, min) >= 0) return null;
    return json({ error: "update", minBuild: min }, 426);
  }

  cachedUsage() {
    if (this.usage) return this.usage;
    const rows = this.state.storage.sql.exec("SELECT v FROM relay_usage WHERE k = ?", "usage").toArray();
    if (rows.length === 1) {
      try {
        this.usage = JSON.parse(rows[0].v);
      } catch {
        this.usage = null;
      }
    }
    return this.usage;
  }

  storeUsage(u) {
    this.usage = u;
    this.state.storage.sql.exec(
      "INSERT OR REPLACE INTO relay_usage (k, v) VALUES (?, ?)", "usage", JSON.stringify(u)
    );
  }

  // The month's egress in bytes from Cloudflare's GraphQL analytics API (TURN usage is queryable
  // over the GraphQL API only; the TURN key itself has no usage endpoint). At most one in-flight
  // read per object; a failure returns null and keeps whatever is cached.
  readUsage(now) {
    if (!this.usageInFlight) {
      this.usageInFlight = this.queryUsage(now).finally(() => {
        this.usageInFlight = null;
      });
    }
    return this.usageInFlight;
  }

  async queryUsage(now) {
    const e = this.env || {};
    const d = new Date(now);
    // `date` filters take a Date (YYYY-MM-DD, UTC); a request must declare its variables
    const from = new Date(Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), 1)).toISOString().slice(0, 10);
    const query =
      'query TurnUsage($accountId: string!, $keyId: string!, $from: Date!, $to: Date!) { viewer { accounts(filter: { accountTag: $accountId }) { callsTurnUsageAdaptiveGroups(' +
      'limit: 1, filter: { keyId: $keyId, date_geq: $from, date_leq: $to }) { sum { egressBytes } } } } }';
    try {
      const resp = await fetch("https://api.cloudflare.com/client/v4/graphql", {
        method: "POST",
        headers: {
          authorization: "Bearer " + e.TURN_USAGE_API_TOKEN,
          "content-type": "application/json",
        },
        body: JSON.stringify({
          query,
          variables: {
            accountId: e.TURN_USAGE_ACCOUNT_ID,
            keyId: e.TURN_KEY_ID,
            from,
            to: d.toISOString().slice(0, 10),
          },
        }),
      });
      if (!resp.ok) {
        console.log("relay usage: Cloudflare answered " + resp.status);
        this.usageReadError = "HTTP " + resp.status;
        return null;
      }
      const body = await resp.json().catch(() => null);
      if (body && body.errors) {
        console.log("relay usage: GraphQL errors in Cloudflare's answer");
        this.usageReadError = "malformed usage answer";
      }
      const data = body && body.data;
      const groups =
        data &&
        data.viewer &&
        data.viewer.accounts &&
        data.viewer.accounts[0] &&
        data.viewer.accounts[0].callsTurnUsageAdaptiveGroups;
      // an empty list is a month with no relay traffic yet: 0 bytes, not a failed read
      const bytes = Array.isArray(groups) && groups.length === 0 ? 0
        : groups && groups[0] && groups[0].sum && Number(groups[0].sum.egressBytes);
      if (!isFinite(bytes)) {
        console.log("relay usage: no egressBytes in Cloudflare's answer");
        this.usageReadError = "malformed usage answer";
        return null;
      }
      this.usageReadError = null;
      const u = { month: this.monthKey(now), bytes, at: Date.now() };
      this.storeUsage(u);
      return u;
    } catch (err) {
      console.log("relay usage: the request failed (" + ((err && err.name) || "error") + ")");
      this.usageReadError = (err && err.name) || "error";
      return null;
    }
  }

  // The operator's relay budget: allow the mint while the month's egress is under
  // RELAY_BUDGET_GB. Off (any setting missing) => allow. Kill switch => refuse. The usage is
  // read at most every 5 minutes; a cached value up to 1 h old is trusted when the read fails,
  // and with nothing that fresh the mint is refused (fail closed: the budget must hold even
  // while the usage API is down).
  async quotaAllows(now) {
    return (await this.quotaCheck(now)).allow;
  }

  // The same decision with its reason and numbers, for the mint's log line and GET /admin/relay.
  async quotaCheck(now) {
    const q = this.quotaConfig();
    const month = this.monthKey(now);
    const out = { month, ...(q.on ? { budgetBytes: q.budgetBytes } : {}) };
    let result;
    if (this.relayKilled()) result = { ...out, allow: false, why: "kill switch (RELAY_KILL) set" };
    else if (!q.on) {
      if (!this.quotaOffLogged) {
        this.quotaOffLogged = true;
        console.log("relay quota off: " + q.why);
      }
      result = { allow: true, why: "quota off: " + q.why, month };
    } else {
      let u = this.cachedUsage();
      if (!u || u.month !== month) u = null;
      if (!u || now - u.at > USAGE_READ_EVERY_MS) {
        const fresh = await this.readUsage(now);
        if (fresh && fresh.month === month) u = fresh;
      }
      if (!u) result = { ...out, allow: false, why: "usage read failed, nothing cached" };
      else {
        out.usedBytes = u.bytes;
        out.readAt = new Date(u.at).toISOString();
        if (now - u.at > USAGE_STALE_MS) result = { ...out, allow: false, why: "usage read failed, cache over 1 h old" };
        else if (u.bytes >= q.budgetBytes) result = { ...out, allow: false, why: "budget reached" };
        else result = { ...out, allow: true, why: "under budget" };
      }
    }
    this.noteRelayAlert(result, now);
    return result;
  }

  // The operator's relay alert: one log line and one webhook POST per alert-state change (not
  // per refused mint): budget reached, usage unreadable, relay killed, recovered. The message
  // names the state and its numbers, never a token, an IP or an id.
  noteRelayAlert(q, now) {
    const state = !q.allow
      ? this.relayKilled() ? "disabled" : q.why === "budget reached" ? "budget" : "usage-unreadable"
      : "ok";
    if (state === this.alertState) return;
    this.alertState = state;
    this.alertChangedAt = new Date(now).toISOString();
    let msg;
    if (state === "budget") {
      msg = "relay budget reached: " + (q.usedBytes / 1e9).toFixed(2) + " GB of " +
        (q.budgetBytes / 1e9).toFixed(2) + " GB used in " + q.month;
    } else if (state === "usage-unreadable") {
      msg = "relay usage unreadable: " + (this.usageReadError || "unknown");
    } else if (state === "disabled") {
      msg = "relay disabled: RELAY_KILL set";
    } else {
      msg = "relay recovered: mints allowed again";
    }
    console.log(msg);
    this.postRelayAlert(msg, now);
  }

  // The alert webhook (env.RELAY_ALERT_WEBHOOK, a wrangler secret, an https URL such as a Discord
  // webhook): fire-and-forget, at most one POST per state change and 6 an hour in total; a
  // failure never delays or fails a mint. No webhook set: nothing is sent.
  postRelayAlert(msg, now) {
    const url = String((this.env && this.env.RELAY_ALERT_WEBHOOK) || "");
    if (!url.startsWith("https://")) return; // the secret is an https URL; nothing else is sent
    this.alertPosts = this.alertPosts.filter((t) => now - t < 3600000);
    if (this.alertPosts.length >= 6) return;
    this.alertPosts.push(now);
    const job = fetch(url, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ content: msg }),
    }).catch(() => {});
    if (this.state && this.state.waitUntil) this.state.waitUntil(job);
  }

  // The connect queue is drained by the host's poll, so the mint and relay-connect gates keep
  // their own recent (gid, nonce) records: pruned by age (10 minutes) and capped in size.
  pruneTurnState(now) {
    for (const [k, v] of this.pairs) if (now - v.at > TURN_WINDOW_MS) this.pairs.delete(k);
    for (const [k, v] of this.mints) if (now - v.at > TURN_WINDOW_MS) this.mints.delete(k);
    for (const [k, v] of this.gidMints) if (now - v.at > TURN_WINDOW_MS) this.gidMints.delete(k);
    for (const [k, v] of this.relayCounts) if (now - v.at > TURN_WINDOW_MS) this.relayCounts.delete(k);
    for (const m of [this.pairs, this.mints, this.gidMints, this.relayCounts])
      if (m.size > TURN_MAPS_MAX) for (const k of [...m.keys()].slice(0, m.size - TURN_MAPS_MAX)) m.delete(k);
  }

  ban(id, reason, date) {
    this.state.storage.sql.exec("INSERT OR REPLACE INTO bans (id, reason, date) VALUES (?, ?, ?)", id, reason, date);
  }

  bans() {
    return this.state.storage.sql.exec("SELECT id, reason, date FROM bans ORDER BY date DESC").toArray();
  }

  unban(id) {
    return this.state.storage.sql.exec("DELETE FROM bans WHERE id = ?", id).rowsWritten;
  }

  isBanned(id) {
    // a cursor's one() throws unless exactly one row came back, so count instead
    return this.state.storage.sql.exec("SELECT 1 FROM bans WHERE id = ?", id).toArray().length > 0;
  }

  refresh(id) {
    const h = this.hosts.get(id);
    if (!h) return;
    h.expires = Date.now() + this.ttl;
    clearTimeout(h.timer);
    h.timer = setTimeout(() => this.expire(id), this.ttl);
    if (typeof h.timer.unref === "function") h.timer.unref();
  }

  expire(id) {
    this.hosts.delete(id);
    this.connects.delete(id);
    this.hostConn.delete(id);
    const w = this.waiters.get(id);
    if (w) {
      this.waiters.delete(id);
      w.resolve(noContent());
    }
  }

  release(id, keep = false) {
    // The host sees {endpoint, nonce, mac} per connect (plus relay: true for a relayed address,
    // and local: the guest's LAN address when both sides are behind the same NAT);
    // the guest's id is the object's business. The host's IP is the caller IP that registered
    // the host; the guest's IP is the caller IP that posted the connect. The local is forwarded
    // only when the two match (same NAT). keep (a poll with ack): the entries carry their seq and
    // stay queued until a later poll acknowledges them, so a poll that dies on the way loses none.
    const host = this.hosts.get(id);
    const hostIp = host ? host.ip : "";
    const hostEp = host ? host.endpoint : "";
    const q = (this.connects.get(id) || []).map((c) => {
      const { endpoint, nonce, mac, relay, local, ip: gip } = c;
      if (!c.delivered) this.metrics.connects[relay ? "relayDelivered" : "directDelivered"]++;
      c.delivered = true;
      const e = relay ? { endpoint, nonce, mac, relay: true } : { endpoint, nonce, mac };
      if (local && host && sameNat(gip, hostIp, endpoint, hostEp)) e.local = local;
      if (keep) e.seq = c.seq;
      return e;
    });
    if (!keep) this.connects.delete(id);
    return json({ connects: q });
  }

  // A poll with ack=N: the host has handled every connect up to seq N.
  dropAcked(id, ack) {
    const q = (this.connects.get(id) || []).filter((c) => c.seq > ack);
    if (q.length) this.connects.set(id, q);
    else this.connects.delete(id);
  }

  // Only fixed route keys reach these counters. Long-poll latency includes the wait.
  async fetch(request) {
    const url = new URL(request.url);
    const ip = (request.cf && request.cf.ip) || request.headers.get("cf-connecting-ip") || "";
    if (url.pathname.startsWith("/admin/")) {
      try { return await this.admin(request, url); }
      catch { return json({ error: "internal" }, 500); }
    }
    const route = request.method + " " + url.pathname;
    const start = Date.now();
    let response = this.gate(route, request.headers.get("x-fci-build"));
    if (!response) try {
      switch (route) {
        case "GET /v1/hosts": response = this.list(url, ip); break;
        case "POST /v1/register": response = await this.register(request, ip); break;
        case "GET /v1/connects": response = await this.poll(url, ip); break;
        case "POST /v1/connect": response = await this.connect(request, ip); break;
        case "POST /v1/turn": response = await this.turn(request, ip); break;
        case "POST /v1/probe-report": response = await this.probeReport(request, ip); break;
        case "POST /v1/unregister": response = await this.unregister(request, ip); break;
        default: return json({ error: "not found" }, 404);
      }
    } catch {
      response = json({ error: "internal" }, 500);
    }
    const row = this.metrics.routes[route] ??= { requests: 0, status: {}, totalMs: 0, maxMs: 0 };
    const elapsed = Math.max(0, Date.now() - start);
    row.requests++;
    row.status[response.status] = (row.status[response.status] || 0) + 1;
    row.totalMs += elapsed;
    row.maxMs = Math.max(row.maxMs, elapsed);
    return response;
  }

  list(url, ip) {
    const now = Date.now();
    if (!this.limits.take("list:" + ip, LIMITS.listsPerMinute, now)) {
      this.note("rate", ip);
      return json({ error: "slow down" }, 429);
    }
    const title = url.searchParams.get("title");
    const version = url.searchParams.get("version");
    const max = Math.min(Number(url.searchParams.get("max")) || LIST_MAX, LIST_MAX);
    for (const h of [...this.hosts.values()]) if (h.expires <= now) this.expire(h.id);
    const found = [...this.hosts.values()]
      .filter((h) => h.title === title && h.version === version && h.proto === PROTO && !this.isBanned(h.id))
      .sort((a, b) => a.id.localeCompare(b.id))
      .slice(0, max)
      .map(({ id }) => ({ id })); // ids only: no endpoint, MAC or token in a listing
    const body = { proto: PROTO, hosts: found };
    if (this.relay) body.relay = this.relay; // the canonical hub, so both sides converge on it
    return json(body);
  }

  async register(request, ip) {
    const now = Date.now();
    if (!this.limits.take("register:" + ip, LIMITS.registersPerMinute, now)) {
      this.note("rate", ip);
      return json({ error: "slow down" }, 429);
    }
    const msg = await this.readJson(request, ip);
    if (msg === null) return this._refusal; // the body was refused (counted in the abuse counters)
    if (!TOKEN.test(String(msg.token))) {
      this.note("bad", ip);
      return json({ error: "bad token" }, 400);
    }
    const id = await deriveId(msg.token);
    if (this.isBanned(id)) {
      this.note("banned", ip, id);
      return json({ error: "banned" }, 403);
    }
    if (!validRegistration(msg, ip)) {
      this.note("bad", ip);
      return json({ error: "bad registration" }, 400);
    }
    if ([...this.hosts.values()].filter((h) => h.ip === ip).length >= LIMITS.hostsPerIp) {
      this.note("rate", ip);
      return json({ error: "too many hosts from this address" }, 429);
    }
    const eip = endpointIp(msg.endpoint);
    if ([...this.hosts.values()].filter((h) => endpointIp(h.endpoint) === eip).length >= LIMITS.hostsPerEndpointIp) {
      this.note("rate", ip, id);
      return json({ error: "too many hosts at this endpoint" }, 429);
    }
    if (!this.hosts.has(id) && this.hosts.size >= HOSTS_MAX) {
      this.note("rate", ip, id);
      return json({ error: "host list full" }, 503); // a re-register of a live host never grows the map
    }
    const old = this.hosts.get(id);
    if (old) clearTimeout(old.timer); // a re-register (the common case: the id is stable)
    const local = localOk(msg.local) ? String(msg.local) : undefined;
    this.hosts.set(id, {
      id,
      ip,
      proto: PROTO,
      title: msg.title,
      version: String(msg.version),
      mac: msg.mac,
      endpoint: msg.endpoint,
      local,
      expires: now + this.ttl,
      timer: null,
    });
    this.refresh(id);
    // no token back: the player's runtime already has it. seq: connects queued before this
    // registration predate the listing, so an acknowledging host starts from here.
    return json({ id, seq: this.seq });
  }

  // A host that is leaving (its process is shutting down) removes its own listing here, so the
  // next instance does not connect to a dead host; without it the listing stays joinable until its
  // 90-second TTL (a poll is the renewal). The token must derive to the id: a 403, not a 404, so a
  // wrong token is not mistaken for "already gone". An unknown id is a 200, so a second unregister,
  // or one after a TTL expiry, is a no-op. The long-poll waiter, if any, is released with an empty,
  // successful poll (the host is leaving, not merely idle).
  async unregister(request, ip) {
    const now = Date.now();
    if (!this.limits.take("unregister:" + ip, LIMITS.unregistersPerMinute, now)) {
      this.note("rate", ip);
      return json({ error: "slow down" }, 429);
    }
    const msg = await this.readJson(request, ip);
    if (msg === null) return this._refusal;
    const id = String(msg.id || "");
    const token = String(msg.token || "");
    if (!ID.test(id) || !TOKEN.test(token) || (await deriveId(token)) !== id) {
      this.note("bad", ip, id);
      return json({ error: "bad token" }, 403);
    }
    const h = this.hosts.get(id);
    if (!h) return json({ ok: true }); // already gone (TTL, a second call): idempotent
    clearTimeout(h.timer);
    this.hosts.delete(id);
    this.connects.delete(id);
    this.hostConn.delete(id);
    const w = this.waiters.get(id);
    if (w) {
      this.waiters.delete(id);
      clearTimeout(w.timer);
      w.resolve(json({ connects: [] })); // the host is leaving: an empty, successful poll
    }
    return json({ ok: true });
  }

  async poll(url, ip) {
    const id = url.searchParams.get("id") || "";
    const token = url.searchParams.get("token") || "";
    if (!ID.test(id) || !TOKEN.test(token) || (await deriveId(token)) !== id)
      return json({ error: "no such host" }, 404);
    const h = this.hosts.get(id);
    // A wrong token, a token of another player, and another address all 404 like an unknown id.
    if (!h || h.ip !== ip) return json({ error: "no such host" }, 404);
    if (this.isBanned(id)) {
      this.expire(id);
      return json({ error: "no such host" }, 404);
    }
    this.refresh(id);
    const ack = url.searchParams.get("ack");
    const keep = ack !== null && /^\d{1,15}$/.test(ack);
    if (keep) this.dropAcked(id, Number(ack));
    const q = this.connects.get(id);
    if (q) return this.release(id, keep);
    return new Promise((resolve) => {
      const old = this.waiters.get(id);
      if (old) {
        clearTimeout(old.timer);
        this.waiters.delete(id);
        old.resolve(noContent()); // the later poll replaces the earlier one
      }
      const timer = setTimeout(() => {
        this.waiters.delete(id);
        resolve(json({ connects: [] })); // a timed-out poll is an empty, successful poll
      }, Math.min(60, Math.max(1, Number(url.searchParams.get("wait")) || 20)) * 1000);
      if (typeof timer.unref === "function") timer.unref();
      this.waiters.set(id, { resolve, timer, keep });
    });
  }

  async connect(request, ip) {
    const now = Date.now();
    if (!this.limits.take("connect:" + ip, LIMITS.connectsPerMinute, now)) {
      this.note("rate", ip);
      return json({ error: "slow down" }, 429);
    }
    const msg = await this.readJson(request, ip);
    if (msg === null) return this._refusal;
    const host = ID.test(String(msg.id)) ? this.hosts.get(msg.id) : null;
    if (!host || this.isBanned(String(msg.id))) {
      this.note("nohost", ip, msg.id);
      return json({ error: "no such host" }, 404);
    }
    if (!TOKEN.test(String(msg.token))) {
      this.note("bad", ip);
      return json({ error: "bad token" }, 400);
    }
    const gid = await deriveId(msg.token);
    if (this.isBanned(gid)) {
      this.note("banned", ip, gid);
      return json({ error: "banned" }, 403);
    }
    if (!endpointOk(msg.endpoint) || !MAC.test(String(msg.mac)) || !NONCE.test(String(msg.nonce))) {
      this.note("bad", ip);
      return json({ error: "bad request" }, 400);
    }
    // The per-host budget: one host can be connect-targeted by at most 24 connects a minute from
    // all addresses, so a flood of guests (or one address) cannot hammer one host's poll.
    const minute = Math.floor(now / 60000);
    let hc = this.hostConn.get(msg.id);
    if (!hc || hc.minute !== minute) {
      hc = { minute, n: 0 };
      this.hostConn.set(msg.id, hc);
    }
    hc.n++;
    if (hc.n > HOST_CONNECTS_PER_MINUTE) {
      this.note("rate", ip, msg.id);
      return json({ error: "host busy" }, 429);
    }
    // A relayed address: the guest's punch failed, it ALLOCATEd through Cloudflare TURN, and this
    // is its second connect with the same nonce. One mint (a TURN allocation) covers every host
    // the guest reached directly: it is accepted with a mint for this gid within the window (any
    // nonce, any host) and a non-relay connect of this (gid, nonce) to this host within the
    // window; a mint for this exact (gid, nonce, host) still qualifies. At most
    // RELAY_CONNECTS_PER_NONCE times per nonce (ICE restarts re-POST a new address). It skips the
    // per-endpoint-IP cap (many guests' relayed addresses share Cloudflare's IPs) and keeps every
    // other check and budget.
    const relay = msg.relay === true;
    const key = gid + ":" + msg.nonce;
    if (relay) {
      const pair = this.pairs.get(key);
      const gidMint = this.gidMints.get(gid);
      const exact = this.mints.get(key);
      const direct = pair && pair.hosts[msg.id] != null && now - pair.hosts[msg.id] <= TURN_WINDOW_MS;
      const gated =
        (gidMint && now - gidMint.at <= TURN_WINDOW_MS && direct) ||
        (exact && exact.host === msg.id && now - exact.at <= TURN_WINDOW_MS);
      if (!gated) {
        this.note("bad", ip, gid);
        return json({ error: "no relay" }, 403);
      }
      const relays = this.relayCounts.get(key) || { at: now, n: 0 };
      if (relays.n >= RELAY_CONNECTS_PER_NONCE) {
        this.note("rate", ip, gid);
        return json({ error: "too many relays" }, 429);
      }
      relays.n++;
      relays.at = now;
      this.relayCounts.set(key, relays);
    }
    // The guest's endpoint IP is where the host will punch: cap how many pending connects target
    // one IP (a hostile guest endpoint is bounded), excluding this guest's own queued entry. A
    // relayed connect skips the cap (its endpoint is Cloudflare's, shared by every relayed guest).
    const geip = endpointIp(msg.endpoint);
    let pendingAtIp = 0;
    for (const q of this.connects.values())
      for (const c of q) if (c.gid !== gid && endpointIp(c.endpoint) === geip) pendingAtIp++;
    if (!relay && pendingAtIp >= LIMITS.connectsPerEndpointIp) {
      this.note("rate", ip, gid);
      return json({ error: "too many connects at this endpoint" }, 429);
    }
    if (!relay) {
      // the mint and relay gates: this guest reached this host directly
      const pair = this.pairs.get(key) || { at: now, hosts: {} };
      pair.hosts[msg.id] = now;
      pair.at = now;
      this.pairs.set(key, pair);
    }
    this.pruneTurnState(now);
    const glocal = localOk(msg.local) ? String(msg.local) : undefined;
    const q = this.connects.get(msg.id) || [];
    const i = q.findIndex((c) => c.gid === gid);
    const entry = { endpoint: msg.endpoint, nonce: msg.nonce, mac: msg.mac, gid, relay, local: glocal, ip, seq: ++this.seq };
    if (i >= 0) q[i] = entry;
    else {
      if (q.length >= CONNECTS_MAX) q.shift(); // drop the oldest: the NIC can only hold 8 pending
      q.push(entry);
    }
    this.connects.set(msg.id, q);
    this.metrics.connects[relay ? "relayQueued" : "directQueued"]++;
    const w = this.waiters.get(msg.id);
    if (w) {
      this.waiters.delete(msg.id);
      clearTimeout(w.timer);
      w.resolve(this.release(msg.id, w.keep));
    }
    // The host's endpoint and MAC are the reply: the listing shows ids only.
    // The host's LAN address is forwarded only when the guest and the host are behind the same
    // NAT (the Worker sees the same caller IP on both sides).
    const reply = { endpoint: host.endpoint, mac: host.mac };
    if (!relay && this.env?.BETA_REPORTS === "1" && typeof msg.reportBuild === "string"
        && REPORT_BUILD.test(msg.reportBuild)) {
      for (const [receipt, entry] of this.reportReceipts) {
        if (now - entry.at > REPORT_TTL) this.reportReceipts.delete(receipt);
      }
      if (this.reportReceipts.size < REPORT_LIMIT) {
        const receipt = crypto.randomUUID().replaceAll("-", "");
        this.reportReceipts.set(receipt, { at: now, title: host.title, build: msg.reportBuild });
        reply.reportReceipt = receipt;
        this.metrics.probes.issued++;
      }
    }
    if (host.local && sameNat(host.ip, ip, host.endpoint, msg.endpoint)) reply.local = host.local;
    return json(reply);
  }

  async probeReport(request, ip) {
    if (this.env?.BETA_REPORTS !== "1") return json({ error: "not found" }, 404);
    const msg = await this.readJson(request, ip);
    if (msg === null) return this._refusal;
    const paths = ["direct", "turn", "hub", "unknown"];
    const stages = ["first_frame", "punch", "turn", "no_frame", "peer_setup"];
    if (!NONCE.test(String(msg.receipt)) || !paths.includes(msg.path) || !stages.includes(msg.stage) ||
        !Number.isInteger(msg.ms) || msg.ms < 0 || msg.ms > REPORT_TTL ||
        (msg.stage === "first_frame" && msg.path === "unknown"))
      return json({ error: "bad probe report" }, 400);
    const entry = this.reportReceipts.get(msg.receipt);
    if (!entry) return json({ error: "unknown probe receipt" }, 404);
    this.reportReceipts.delete(msg.receipt);
    if (Date.now() - entry.at > REPORT_TTL) return json({ error: "unknown probe receipt" }, 404);
    const metrics = this.metrics.probes;
    const key = entry.title + "/" + msg.path + "/" + msg.stage;
    metrics.results[key] = (metrics.results[key] || 0) + 1;
    metrics.received++;
    if (msg.stage === "first_frame") metrics.firstFrameMs += msg.ms;
    const build = this.reportBuilds.size < 16 || this.reportBuilds.has(entry.build) ? entry.build : "(other)";
    this.reportBuilds.set(build, (this.reportBuilds.get(build) || 0) + 1);
    return noContent();
  }

  // Mint a Cloudflare TURN credential for a guest. The guest must have connected to `id` with
  // this `nonce` within the last 10 minutes (the pair map, kept because the connect queue is
  // drained by the host's poll). The TURN key never leaves the Worker: it signs the call to
  // Cloudflare's credential API, and Cloudflare's body is never echoed to the caller.
  async turn(request, ip) {
    const now = Date.now();
    const msg = await this.readJson(request, ip);
    if (msg === null) return this._refusal;
    if (!ID.test(String(msg.id)) || !NONCE.test(String(msg.nonce))) {
      this.note("bad", ip);
      return json({ error: "bad request" }, 400);
    }
    if (!TOKEN.test(String(msg.token))) {
      this.note("bad", ip);
      return json({ error: "bad token" }, 400);
    }
    const gid = await deriveId(msg.token);
    if (this.isBanned(gid)) {
      this.note("banned", ip, gid);
      return json({ error: "banned" }, 403);
    }
    const pair = this.pairs.get(gid + ":" + msg.nonce);
    const host = this.hosts.get(String(msg.id));
    if (!host || this.isBanned(String(msg.id))) {
      this.note("nohost", ip, msg.id);
      return json({ error: "no such host" }, 404);
    }
    const direct = pair && pair.hosts[String(msg.id)] != null && now - pair.hosts[String(msg.id)] <= TURN_WINDOW_MS;
    if (!direct) {
      this.note("bad", ip, gid);
      return json({ error: "no connect" }, 403);
    }
    if (
      !this.mintLimit.take("gid:" + gid, MINTS_PER_WINDOW, now) ||
      !this.mintLimit.take("ip:" + ip, MINTS_PER_WINDOW, now)
    ) {
      this.note("rate", ip, gid);
      return json({ error: "slow down" }, 429);
    }
    const keyId = String((this.env && this.env.TURN_KEY_ID) || "");
    const apiToken = String((this.env && this.env.TURN_KEY_API_TOKEN) || "");
    if (!keyId || !apiToken) {
      this.metrics.relay.unconfigured++;
      this.note("norelay", ip, gid);
      return json({ error: "no relay" }, 503);
    }
    const quota = await this.quotaCheck(now); // logs the alert line on a state change (not per mint)
    if (!quota.allow) {
      const reason = quota.why === "budget reached" ? "budget"
        : this.relayKilled() ? "disabled" : "usageUnavailable";
      this.metrics.relay[reason]++;
      this.note("norelay", ip, gid);
      return json({ error: "no relay" }, 503);
    }
    let data = null;
    try {
      const resp = await fetch(
        "https://rtc.live.cloudflare.com/v1/turn/keys/" + keyId + "/credentials/generate-ice-servers",
        {
          method: "POST",
          headers: { authorization: "Bearer " + apiToken, "content-type": "application/json" },
          body: JSON.stringify({ ttl: TURN_TTL }),
        }
      );
      if (resp.ok) data = await resp.json().catch(() => null);
      // status only, never the body: an operator needs to tell a bad key id (404) from a bad token (401/403)
      else console.log("turn mint: Cloudflare answered " + resp.status);
    } catch (e) {
      console.log("turn mint: request failed (" + (e && e.name) + ")");
      data = null;
    }
    const cred = turnCredential(data && data.iceServers);
    if (!cred) {
      this.metrics.relay.providerError++;
      if (data) console.log("turn mint: no usable TURN entry in Cloudflare's answer");
      this.note("norelay", ip, gid);
      return json({ error: "no relay" }, 503);
    }
    this.mints.set(gid + ":" + msg.nonce, { host: String(msg.id), at: now });
    this.gidMints.set(gid, { at: now }); // the relay gate: this guest holds a fresh TURN allocation
    this.pruneTurnState(now);
    this.metrics.relay.issued++;
    return json({ username: cred.username, credential: cred.credential, urls: cred.urls });
  }

  // The shared body check: JSON, at most MESSAGE_MAX bytes. Refusals are counted.
  async readJson(request, ip) {
    if (!String(request.headers.get("content-type") || "").includes("application/json")) {
      this.note("bad", ip);
      this._refusal = json({ error: "JSON only" }, 415);
      return null;
    }
    const text = await request.text();
    if (text.length > MESSAGE_MAX) {
      this.note("bad", ip);
      this._refusal = json({ error: "too long" }, 413);
      return null;
    }
    let msg;
    try {
      msg = JSON.parse(text);
    } catch {
      msg = null;
    }
    if (!msg || typeof msg !== "object" || Array.isArray(msg)) {
      this.note("bad", ip);
      this._refusal = json({ error: "not JSON" }, 400);
      return null;
    }
    this._refusal = null;
    return msg;
  }

  // The admin routes: the operator's secret (env.ADMIN_SECRET) is the only gate.
  async admin(request, url) {
    const secret = this.env && this.env.ADMIN_SECRET;
    if (!secret) return json({ error: "admin not configured" }, 503);
    if (!secretOk(request.headers.get("authorization"), secret)) {
      return json({ error: "unauthorized" }, 401);
    }
    if (url.pathname === "/admin/hosts" && request.method === "GET") {
      return json({
        hosts: [...this.hosts.values()].map((h) => ({
          id: h.id,
          ip: h.ip,
          title: h.title,
          version: h.version,
          mac: h.mac,
          endpoint: h.endpoint,
          pending: (this.connects.get(h.id) || []).length,
          expires: h.expires,
        })),
      });
    }
    if (url.pathname === "/admin/bans" && request.method === "GET") {
      return json({ bans: this.bans() });
    }
    if (url.pathname === "/admin/metrics" && request.method === "GET") {
      const now = Date.now();
      const active = {};
      for (const h of this.hosts.values()) {
        if (h.expires <= now) continue;
        const key = h.title + "/" + h.proto;
        active[key] = (active[key] || 0) + 1;
      }
      return json({
        ...this.metrics, sampledAt: now, active,
        probes: { ...this.metrics.probes, builds: Object.fromEntries(this.reportBuilds) },
      });
    }
    if (url.pathname === "/admin/relay" && request.method === "GET") {
      // the month's relay usage against the budget, whether a mint would be allowed now, and
      // the current alert state with when it last changed
      const q = await this.quotaCheck(Date.now());
      return json({ ...q, alertState: this.alertState, alertChangedAt: this.alertChangedAt });
    }
    if (url.pathname === "/admin/bans" && request.method === "POST") {
      const msg = await this.adminJson(request);
      if (msg === null) return this._refusal;
      if (!ID.test(String(msg.id))) {
        this._refusal = json({ error: "bad id" }, 400);
        return this._refusal;
      }
      const reason = String(msg.reason || "").slice(0, 200);
      this.ban(msg.id, reason, new Date().toISOString());
      return json({ ok: true });
    }
    if (url.pathname === "/admin/bans" && request.method === "DELETE") {
      const id = url.searchParams.get("id") || "";
      if (!ID.test(id)) return json({ error: "bad id" }, 400);
      return json({ removed: this.unban(id) > 0 });
    }
    if (url.pathname === "/admin/abuse" && request.method === "GET") {
      const byIp = [...this.abuse.byIp.values()].sort((a, b) => {
        const ta = a.rate + a.bad + a.nohost + a.banned + a.norelay;
        const tb = b.rate + b.bad + b.nohost + b.banned + b.norelay;
        return tb - ta;
      });
      return json({ byIp: byIp.slice(0, 100), ring: this.abuse.ring });
    }
    return json({ error: "not found" }, 404);
  }

  // The admin bodies use the same size limit; the refusals are not abuse (the operator is here).
  async adminJson(request) {
    if (!String(request.headers.get("content-type") || "").includes("application/json")) {
      this._refusal = json({ error: "JSON only" }, 415);
      return null;
    }
    const text = await request.text();
    if (text.length > MESSAGE_MAX) {
      this._refusal = json({ error: "too long" }, 413);
      return null;
    }
    let msg;
    try {
      msg = JSON.parse(text);
    } catch {
      msg = null;
    }
    if (!msg || typeof msg !== "object" || Array.isArray(msg)) {
      this._refusal = json({ error: "not JSON" }, 400);
      return null;
    }
    this._refusal = null;
    return msg;
  }
}

// Cloudflare's entry point: route every request to the one lobby object. Lobby is the Durable
// Object itself (wrangler.toml's class): its constructor runs once per incarnation and its fetch
// is called per request, so the in-memory state is the object's own.
export default {
  async fetch(request, env) {
    const lobby = env.LOBBY.get(env.LOBBY.idFromName("lobby"));
    return lobby.fetch(request);
  },
};
