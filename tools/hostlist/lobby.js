// The host list: a Cloudflare Worker with one Durable Object (lobby.js, research/docs/online-architecture.md, sections 3-5).
//
// The game's card model (src/fci_nic.c, RECOMP_FCI_NET_LIST mode) talks to this over plain HTTP(S):
//   GET  /v1/hosts?title=&version=[&max=]
//   POST /v1/register
//   GET  /v1/connects?id=&token=&wait=<1..60>   (long-poll; a host loops this)
//   POST /v1/connect
// and the operator's admin CLI (tools/hostlist/admin.py):
//   GET    /admin/hosts
//   GET    /admin/bans
//   POST   /admin/bans
//   DELETE /admin/bans?id=
//   GET    /admin/abuse
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
//   24 connects a minute from all addresses (flood budget, section 5).
// - Bans (a banned id cannot host, and cannot join anyone) persist in the object's SQLite, so they
//   survive an eviction; the refusal counters and a ring of recent refusals are memory-only.
//
// The state is tiny (a few hundred bytes per live host) and all of it is in this one object's
// memory: a long-poll keeps the object awake, so every poll is held here, never an object per
// host. An entry is a host that keeps polling; a host that stops stays listed until its TTL runs
// out (a poll is the renewal). Nothing important survives an eviction (a host re-registers),
// except the bans.
//
// Deploy: wrangler.toml (one Worker, one DO class, `Lobby`); RELAY is a plain variable
// (`wrangler variables put`) and ADMIN_SECRET a secret (`wrangler secret put`). Free tier.
// The how-to is research/docs/online-architecture.md, section 7.

export const PROTO = 1;
export const LIST_MAX = 64;
const TITLES = new Set(["instincts", "evolution"]);
const VERSION = /^[0-9]{1,5}$/;
const MAC = /^02:46:43(:[0-9a-f]{2}){3}$/;
const NONCE = /^[0-9a-f]{32}$/;
const ID = /^[0-9a-f]{32}$/; // 16 bytes of SHA-256(token), hex
const TOKEN = /^[0-9a-f]{64}$/; // 32 random bytes, hex
// Kept above the maximum long-poll wait (60 s) so a full-duration poll can never race the expiry
// and flap the host. A host that stops polling leaves after this (a poll is the renewal).
const TTL_MS = 90000;
const CONNECTS_MAX = 8; // the NIC's pending-peer cap (src/fci_nic.c)
const HOST_CONNECTS_PER_MINUTE = 24; // per-host flood budget (section 5)
const MESSAGE_MAX = 512; // register, connect and admin bodies
const BANS_RING = 1000; // recent refusals kept in memory
const LIMITS = {
  hostsPerIp: 4,
  hostsPerEndpointIp: 4, // live hosts that may list the same endpoint IP (a hostile endpoint is bounded)
  connectsPerEndpointIp: 8, // pending connects that may target the same endpoint IP
  listsPerMinute: 30,
  connectsPerMinute: 60,
  registersPerMinute: 10,
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

// The player's id from the identity token: the first 16 bytes of SHA-256(token), hex.
export async function deriveId(token) {
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
  constructor() {
    this.m = new Map();
  }
  take(key, max, now) {
    const minute = Math.floor(now / 60000);
    let e = this.m.get(key);
    if (!e || e.minute !== minute) {
      e = { minute, n: 0 };
      this.m.set(key, e);
    }
    e.n++;
    if (this.m.size > 4000) {
      // drop the oldest minutes so a flood of unique addresses can't grow this without bound
      for (const [k, v] of this.m) if (v.minute < minute) this.m.delete(k);
    }
    return e.n <= max;
  }
}

export class Lobby {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.ttl = (env && env.TTL_MS) || TTL_MS;
    this.relay = (env && env.RELAY) || "";
    this.hosts = new Map(); // id -> {id, ip, proto, title, version, mac, endpoint, expires, timer}
    this.connects = new Map(); // id -> [{endpoint, nonce, mac, gid}]
    this.waiters = new Map(); // id -> {resolve, timer}
    this.limits = new RateLimit();
    this.hostConn = new Map(); // id -> {minute, n} (the per-host connect budget)
    this.abuse = { byIp: new Map(), ring: [] };
    state.storage.sql(
      "CREATE TABLE IF NOT EXISTS bans (id TEXT PRIMARY KEY, reason TEXT NOT NULL DEFAULT '', date TEXT NOT NULL)"
    );
  }

  // Memory of the refusals this object has seen (the admin CLI's `abuse` command); the ban itself
  // lives in SQLite.
  note(kind, ip, id) {
    const c =
      this.abuse.byIp.get(ip) || { ip, rate: 0, bad: 0, nohost: 0, banned: 0 };
    c[kind]++;
    this.abuse.byIp.set(ip, c);
    this.abuse.ring.push({ ip, kind, id: id || null, at: Date.now() });
    if (this.abuse.ring.length > BANS_RING) this.abuse.ring.shift();
  }

  ban(id, reason, date) {
    this.state.storage.sql("INSERT OR REPLACE INTO bans (id, reason, date) VALUES (?, ?, ?)", id, reason, date);
  }

  bans() {
    return this.state.storage.sql("SELECT id, reason, date FROM bans ORDER BY date DESC").toArray();
  }

  unban(id) {
    const r = this.state.storage.sql("DELETE FROM bans WHERE id = ?", id);
    return (r.meta && r.meta.changes) || 0;
  }

  isBanned(id) {
    return this.state.storage.sql("SELECT 1 FROM bans WHERE id = ?", id).one() !== undefined;
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

  release(id) {
    // The host sees {endpoint, nonce, mac} per connect; the guest's id is the object's business.
    const q = (this.connects.get(id) || []).map(({ endpoint, nonce, mac }) => ({ endpoint, nonce, mac }));
    this.connects.delete(id);
    return json({ connects: q });
  }

  // The public routes.
  async fetch(request) {
    const url = new URL(request.url);
    // Cloudflare sets both request.cf.ip and the cf-connecting-ip header (the header is what the
    // Node tests stand in for, as request.cf is Workers-only).
    const ip = (request.cf && request.cf.ip) || request.headers.get("cf-connecting-ip") || "";
    try {
      if (url.pathname.startsWith("/admin/")) return await this.admin(request, url);
      if (url.pathname === "/v1/hosts" && request.method === "GET") return this.list(url, ip);
      if (url.pathname === "/v1/register" && request.method === "POST") return await this.register(request, ip);
      if (url.pathname === "/v1/connects" && request.method === "GET") return await this.poll(url, ip);
      if (url.pathname === "/v1/connect" && request.method === "POST") return await this.connect(request, ip);
      return json({ error: "not found" }, 404);
    } catch (e) {
      return json({ error: "internal" }, 500);
    }
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
    const old = this.hosts.get(id);
    if (old) clearTimeout(old.timer); // a re-register (the common case: the id is stable)
    this.hosts.set(id, {
      id,
      ip,
      proto: PROTO,
      title: msg.title,
      version: String(msg.version),
      mac: msg.mac,
      endpoint: msg.endpoint,
      expires: now + this.ttl,
      timer: null,
    });
    this.refresh(id);
    return json({ id }); // no token back: the player's runtime already has it
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
    const q = this.connects.get(id);
    if (q) return this.release(id);
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
      this.waiters.set(id, { resolve, timer });
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
    // The guest's endpoint IP is where the host will punch: cap how many pending connects target
    // one IP (a hostile guest endpoint is bounded), excluding this guest's own queued entry.
    const geip = endpointIp(msg.endpoint);
    let pendingAtIp = 0;
    for (const q of this.connects.values())
      for (const c of q) if (c.gid !== gid && endpointIp(c.endpoint) === geip) pendingAtIp++;
    if (pendingAtIp >= LIMITS.connectsPerEndpointIp) {
      this.note("rate", ip, gid);
      return json({ error: "too many connects at this endpoint" }, 429);
    }
    const q = this.connects.get(msg.id) || [];
    const i = q.findIndex((c) => c.gid === gid);
    if (i >= 0) q[i] = { endpoint: msg.endpoint, nonce: msg.nonce, mac: msg.mac, gid };
    else {
      if (q.length >= CONNECTS_MAX) q.shift(); // drop the oldest: the NIC can only hold 8 pending
      q.push({ endpoint: msg.endpoint, nonce: msg.nonce, mac: msg.mac, gid });
    }
    this.connects.set(msg.id, q);
    const w = this.waiters.get(msg.id);
    if (w) {
      this.waiters.delete(msg.id);
      clearTimeout(w.timer);
      w.resolve(this.release(msg.id));
    }
    // The host's endpoint and MAC are the reply: the listing shows ids only.
    return json({ endpoint: host.endpoint, mac: host.mac });
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
        const ta = a.rate + a.bad + a.nohost + a.banned;
        const tb = b.rate + b.bad + b.nohost + b.banned;
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
