// The internet System Link host list (research: docs/multiplayer.md, Internet System Link).
// A host keeps a WebSocket open while it hosts; its entry is that socket's attachment, so an idle host costs
// nothing (hibernation) and a host that vanishes leaves the list with its socket. Browsers list hosts over
// HTTPS and ask one to start a hole punch. Nothing here relays game traffic.

export const PROTO = 1;
export const LIST_MAX = 64; // cap on one listing; the game's browser shows fewer (backlog 19)
const TITLES = new Set(["instincts", "evolution"]);
const VERSION = /^[0-9]{1,5}$/; // the disc's version number (data/disc.json)
const MAC = /^02:46:43(:[0-9a-f]{2}){3}$/; // the cards' own locally administered prefix (fci_nic.c)
const REGION = /^[a-z]{2}$/;
const NONCE = /^[0-9a-f]{32}$/;
const MESSAGE_MAX = 512;
const LIMITS = { hostsPerIp: 4, listsPerMinute: 30, connectsPerMinute: 60 };

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
    msg && msg.t === "register" && msg.proto === PROTO && TITLES.has(msg.title) &&
    VERSION.test(String(msg.version)) && MAC.test(String(msg.mac)) &&
    (msg.region === undefined || REGION.test(String(msg.region))) &&
    endpointOk(msg.endpoint, connectingIp)
  );
}

// per-address counters for one minute; best effort (they reset when the object hibernates)
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

function randomId() {
  const b = new Uint8Array(12);
  crypto.getRandomValues(b);
  return [...b].map((x) => x.toString(16).padStart(2, "0")).join("");
}

// One Durable Object holds every host (idFromName("lobby")); each host is one hibernatable WebSocket.
export class Lobby {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.limits = new RateLimit();
  }

  hosts() {
    return this.state.getWebSockets().map((ws) => ws.deserializeAttachment()).filter((a) => a && a.id);
  }

  async fetch(request) {
    const url = new URL(request.url);
    const ip = request.headers.get("cf-connecting-ip") || "";
    const now = Date.now();
    if (url.pathname === "/v1/host") return this.host(request, ip);
    if (url.pathname === "/v1/hosts" && request.method === "GET") {
      if (!this.limits.take("list:" + ip, LIMITS.listsPerMinute, now)) return json({ error: "slow down" }, 429);
      const title = url.searchParams.get("title"), version = url.searchParams.get("version");
      const region = url.searchParams.get("region");
      const max = Math.min(Number(url.searchParams.get("max")) || LIST_MAX, LIST_MAX);
      let found = this.hosts().filter((h) => h.title === title && h.version === version && h.proto === PROTO);
      // the asker's own region first; otherwise the order is left to the client, which probes and measures
      if (region) found.sort((a, b) => (b.region === region) - (a.region === region));
      found = found.slice(0, max).map(({ id, endpoint, mac, region }) => ({ id, endpoint, mac, region }));
      return json({ proto: PROTO, hosts: found });
    }
    if (url.pathname === "/v1/connect" && request.method === "POST") {
      if (!this.limits.take("connect:" + ip, LIMITS.connectsPerMinute, now)) return json({ error: "slow down" }, 429);
      const text = await request.text();
      if (text.length > MESSAGE_MAX) return json({ error: "too long" }, 413);
      let msg;
      try { msg = JSON.parse(text); } catch { return json({ error: "not JSON" }, 400); }
      if (!msg || typeof msg.id !== "string" || !NONCE.test(String(msg.nonce)) || !endpointOk(msg.endpoint, ip))
        return json({ error: "bad request" }, 400);
      const ws = this.state.getWebSockets(msg.id)[0];
      if (!ws || !(ws.deserializeAttachment() || {}).id) return json({ error: "no such host" }, 404);
      ws.send(JSON.stringify({ t: "connect", endpoint: msg.endpoint, nonce: msg.nonce }));
      return new Response(null, { status: 204 });
    }
    return json({ error: "not found" }, 404);
  }

  host(request, ip) {
    if (request.headers.get("upgrade") !== "websocket") return json({ error: "WebSocket only" }, 426);
    const mine = this.state.getWebSockets("ip:" + ip).length;
    if (mine >= LIMITS.hostsPerIp) return json({ error: "too many hosts from this address" }, 429);
    const pair = new WebSocketPair();
    const id = randomId();
    // tagged by id (connect requests) and by address (the per-address cap); registered on its first message
    this.state.acceptWebSocket(pair[1], [id, "ip:" + ip]);
    pair[1].serializeAttachment({ pending: id, ip });
    return Lobby.upgrade(pair[0]);
  }

  // the 101 response the Workers runtime turns into a WebSocket (a seam: Node's Response refuses 101)
  static upgrade(client) {
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws, data) {
    const a = ws.deserializeAttachment() || {};
    if (typeof data !== "string" || data.length > MESSAGE_MAX) return ws.close(1009, "too long");
    let msg;
    try { msg = JSON.parse(data); } catch { return ws.close(1007, "not JSON"); }
    if (a.id || !a.pending) return ws.close(1008, "already registered");
    if (!validRegistration(msg, a.ip)) return ws.close(1008, "bad registration");
    const entry = {
      id: a.pending, ip: a.ip, proto: PROTO, title: msg.title, version: String(msg.version),
      mac: msg.mac, endpoint: msg.endpoint, region: msg.region,
    };
    ws.serializeAttachment(entry);
    ws.send(JSON.stringify({ t: "ok", id: entry.id }));
  }

  webSocketClose(ws, code) {
    ws.close(code === 1005 ? 1000 : code, "bye");
  }

  webSocketError(ws) {
    ws.close(1011, "error");
  }
}

export default {
  async fetch(request, env) {
    const lobby = env.LOBBY.get(env.LOBBY.idFromName("lobby"));
    return lobby.fetch(request);
  },
};
