// The internet System Link host list (research: docs/multiplayer.md, Internet System Link).
// One Durable Object holds every host in memory (the single always-awake list object; a long-poll
// keeps its object awake, so never one object per host). A host POSTs /v1/register and then loops
// the long-poll GET /v1/connects, which is both its connect-notice channel and its keepalive (each
// poll refreshes the 60 s TTL). Browsers list hosts over HTTPS and ask one to start a hole punch.
// Nothing here relays game traffic.

export const PROTO = 1;
export const LIST_MAX = 64; // cap on one listing; the game's browser shows fewer (backlog 19)
const TITLES = new Set(["instincts", "evolution"]);
const VERSION = /^[0-9]{1,5}$/; // the disc's version number (data/disc.json)
const MAC = /^02:46:43(:[0-9a-f]{2}){3}$/; // the cards' own locally administered prefix (fci_nic.c)
const NONCE = /^[0-9a-f]{32}$/;
const ID = /^[0-9a-f]{24}$/;
const TOKEN = /^[0-9a-f]{32}$/;
const TTL_MS = 60000; // a host that misses ~3 long-polls (wait=20) expires
const CONNECTS_MAX = 8; // the NIC's pending-peer cap: beyond it the oldest guest is dropped
const MESSAGE_MAX = 512;
const LIMITS = { hostsPerIp: 4, listsPerMinute: 30, connectsPerMinute: 60, registersPerMinute: 10 };

// "1.2.3.4:5678" -> {ip, port}, or null
export function parseEndpoint(text) {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3}):(\d{1,5})$/.exec(String(text));
  if (!m) return null;
  const parts = m.slice(1, 5).map(Number);
  const port = Number(m[5]);
  if (parts.some((p) => p > 255) || port < 1 || port > 65535) return null;
  return { ip: parts.join("."), port };
}

// a public IPv4 address only: the endpoint is where strangers will send datagrams
export function isPublic(ip) {
  const [a, b] = ip.split(".").map(Number);
  return !(
    a === 0 || a === 10 || a === 127 || a >= 224 ||
    (a === 100 && b >= 64 && b < 128) || (a === 169 && b === 254) ||
    (a === 172 && b >= 16 && b < 32) || (a === 192 && b === 168)
  );
}

// the endpoint a client reports (from STUN) must be on the address the Worker sees it connect from, so
// nobody can list or punch toward someone else's address. An IPv6 connection can't be compared: refused.
export function endpointOk(text, connectingIp) {
  const ep = parseEndpoint(text);
  return !!ep && isPublic(ep.ip) && ep.ip === connectingIp;
}

export function validRegistration(msg, connectingIp) {
  return (
    msg && msg.proto === PROTO && TITLES.has(msg.title) &&
    VERSION.test(String(msg.version)) && MAC.test(String(msg.mac)) &&
    endpointOk(msg.endpoint, connectingIp)
  );
}

// per-address counters for one minute; best effort (they reset when the object is evicted)
export class RateLimit {
  constructor() { this.minute = 0; this.counts = new Map(); }
  take(key, limit, now) {
    const minute = Math.floor(now / 60000);
    if (minute !== this.minute) { this.minute = minute; this.counts.clear(); }
    const n = (this.counts.get(key) || 0) + 1;
    this.counts.set(key, n);
    return n <= limit;
  }
}

function json(body, status = 200) {
  return new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json" } });
}

const noContent = () => new Response(null, { status: 204 });

function randomHex(bytes) {
  const b = new Uint8Array(bytes);
  crypto.getRandomValues(b);
  return [...b].map((x) => x.toString(16).padStart(2, "0")).join("");
}

// One Durable Object holds the whole list in memory: entries, pending connects and the long-poll
// waiters. The object stays awake (all polls live here, never an object per host), so no state is
// stored and nothing survives an eviction: a host re-registers.
export class Lobby {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.limits = new RateLimit();
    this.ttl = Number(env && env.TTL_MS) > 0 ? Number(env.TTL_MS) : TTL_MS;
    this.hosts = new Map(); // id -> {id, token, ip, proto, title, version, mac, endpoint, expires, timer}
    this.connects = new Map(); // id -> [{endpoint, nonce, mac}] queued guest connects, awaiting the host's poll
    this.waiters = new Map(); // id -> {resolve, timer} the host's held long-poll
  }

  // a register or a poll refreshes the entry's TTL and its expiry timer
  refresh(id) {
    const h = this.hosts.get(id);
    if (!h) return;
    h.expires = Date.now() + this.ttl;
    clearTimeout(h.timer);
    h.timer = setTimeout(() => this.expire(id), this.ttl);
  }

  // an entry outlived its TTL: drop it, its pending connect and any held poll (the host re-registers)
  expire(id) {
    this.hosts.delete(id);
    this.connects.delete(id);
    const w = this.waiters.get(id);
    if (w) { this.waiters.delete(id); clearTimeout(w.timer); w.resolve(noContent()); }
  }

  async fetch(request) {
    const url = new URL(request.url);
    const ip = request.headers.get("cf-connecting-ip") || "";
    const now = Date.now();
    for (const id of [...this.hosts.keys()]) if (this.hosts.get(id).expires <= now) this.expire(id);
    if (url.pathname === "/v1/register" && request.method === "POST") return this.register(request, ip, now);
    if (url.pathname === "/v1/connects" && request.method === "GET") return this.poll(url, ip, now);
    if (url.pathname === "/v1/hosts" && request.method === "GET") {
      if (!this.limits.take("list:" + ip, LIMITS.listsPerMinute, now)) return json({ error: "slow down" }, 429);
      const title = url.searchParams.get("title"), version = url.searchParams.get("version");
      const max = Math.min(Number(url.searchParams.get("max")) || LIST_MAX, LIST_MAX);
      const found = [...this.hosts.values()]
        .filter((h) => h.title === title && h.version === version && h.proto === PROTO)
        .sort((a, b) => a.id.localeCompare(b.id)) // deterministic: the client probes and measures the order
        .slice(0, max)
        .map(({ id, endpoint, mac }) => ({ id, endpoint, mac }));
      return json({ proto: PROTO, hosts: found });
    }
    if (url.pathname === "/v1/connect" && request.method === "POST") {
      if (!this.limits.take("connect:" + ip, LIMITS.connectsPerMinute, now)) return json({ error: "slow down" }, 429);
      if (!request.headers.get("content-type")?.includes("application/json")) return json({ error: "JSON only" }, 415);
      const text = await request.text();
      if (text.length > MESSAGE_MAX) return json({ error: "too long" }, 413);
      let msg;
      try { msg = JSON.parse(text); } catch { return json({ error: "not JSON" }, 400); }
      if (!msg || !ID.test(String(msg.id)) || !this.hosts.has(msg.id)) return json({ error: "no such host" }, 404);
      if (!endpointOk(msg.endpoint, ip) || !MAC.test(String(msg.mac)) || !NONCE.test(String(msg.nonce)))
        return json({ error: "bad request" }, 400);
      const q = this.connects.get(msg.id) || [];
      if (q.length >= CONNECTS_MAX) q.shift(); // the NIC caps pending peers at 8; drop the oldest guest
      q.push({ endpoint: msg.endpoint, nonce: msg.nonce, mac: msg.mac });
      this.connects.set(msg.id, q);
      const w = this.waiters.get(msg.id); // the host's long-poll is the connect-notice channel
      if (w) { this.waiters.delete(msg.id); clearTimeout(w.timer); w.resolve(this.release(msg.id)); }
      return noContent();
    }
    return json({ error: "not found" }, 404);
  }

  async register(request, ip, now) {
    if (!this.limits.take("register:" + ip, LIMITS.registersPerMinute, now)) return json({ error: "slow down" }, 429);
    if (!request.headers.get("content-type")?.includes("application/json")) return json({ error: "JSON only" }, 415);
    const text = await request.text();
    if (text.length > MESSAGE_MAX) return json({ error: "too long" }, 413);
    let msg;
    try { msg = JSON.parse(text); } catch { return json({ error: "not JSON" }, 400); }
    if (!validRegistration(msg, ip)) return json({ error: "bad registration" }, 400);
    // live entries per address are capped, or polling could keep any number alive
    if ([...this.hosts.values()].filter((h) => h.ip === ip).length >= LIMITS.hostsPerIp)
      return json({ error: "too many hosts from this address" }, 429);
    const id = randomHex(12), token = randomHex(16); // the token is the poll's key; it is never listed
    this.hosts.set(id, {
      id, token, ip, proto: PROTO, title: msg.title, version: String(msg.version),
      mac: msg.mac, endpoint: msg.endpoint, expires: now + this.ttl, timer: null,
    });
    this.refresh(id);
    return json({ id, token });
  }

  // the queued connects, handed to the host by its next (or held) poll; a fresh nonce per connect
  release(id) {
    const q = this.connects.get(id) || [];
    this.connects.delete(id);
    return json({ connects: q });
  }

  // GET /v1/connects?id=&token=&wait=<1..30>: the ids are public (the list) but the polls are not:
  // the token and the registered address both have to match, else 404 like an unknown id (so a
  // stranger can neither read the host's guests nor knock out its held poll). Held until a connect
  // arrives (then answered with the queue), wait seconds elapse (204) or the entry expires (204).
  // Each poll refreshes the TTL; a second poll by the same host replaces the first (204).
  async poll(url, ip, now) {
    const id = url.searchParams.get("id") || "";
    const h = ID.test(id) ? this.hosts.get(id) : null;
    if (!h || h.token !== url.searchParams.get("token") || h.ip !== ip) return json({ error: "no such host" }, 404);
    this.refresh(id);
    if (this.connects.has(id)) return this.release(id);
    const n = Math.floor(Number(url.searchParams.get("wait") ?? 20));
    const wait = Math.min(30, Math.max(1, Number.isFinite(n) ? n : 20)) * 1000; // clamped 1..30 s
    const old = this.waiters.get(id);
    if (old) { this.waiters.delete(id); clearTimeout(old.timer); old.resolve(noContent()); }
    return new Promise((resolve) => {
      const w = { resolve, timer: setTimeout(() => { this.waiters.delete(id); resolve(noContent()); }, wait) };
      this.waiters.set(id, w);
    });
  }
}

export default {
  async fetch(request, env) {
    const lobby = env.LOBBY.get(env.LOBBY.idFromName("lobby"));
    return lobby.fetch(request);
  },
};
