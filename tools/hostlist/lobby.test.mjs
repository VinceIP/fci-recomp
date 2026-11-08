// node --test tools/hostlist/lobby.test.mjs   (Node 20+; no packages). The Durable Object API is faked below.
import assert from "node:assert/strict";
import { test } from "node:test";
import { LIST_MAX, Lobby, endpointOk, isPublic, parseEndpoint } from "./lobby.js";

class FakeSocket {
  constructor(state) { this.state = state; this.sent = []; this.closed = null; this.att = null; }
  serializeAttachment(v) { this.att = structuredClone(v); }
  deserializeAttachment() { return this.att && structuredClone(this.att); }
  send(text) { this.sent.push(JSON.parse(text)); }
  close(code, reason) { this.closed = { code, reason }; this.state.drop(this); }
}

class FakeState {
  constructor() { this.sockets = []; }
  acceptWebSocket(ws, tags) { this.sockets.push({ ws, tags }); }
  getWebSockets(tag) { return this.sockets.filter((s) => tag === undefined || s.tags.includes(tag)).map((s) => s.ws); }
  drop(ws) { this.sockets = this.sockets.filter((s) => s.ws !== ws); }
}

function setup() {
  const state = new FakeState();
  globalThis.WebSocketPair = function () { return [{ client: true }, new FakeSocket(state)]; };
  Lobby.upgrade = (client) => ({ status: 101, webSocket: client });
  return { state, lobby: new Lobby(state, {}) };
}

const req = (path, ip, init = {}) =>
  new Request("https://list.example" + path, { ...init, headers: { "cf-connecting-ip": ip, ...(init.headers || {}) } });

const hostReq = (ip) => req("/v1/host", ip, { headers: { upgrade: "websocket" } });

function registration(ip, over = {}) {
  return {
    t: "register", proto: 1, title: "evolution", version: "3", mac: "02:46:43:12:34:56",
    endpoint: `${ip}:40000`, region: "us", ...over,
  };
}

// opens a host socket from ip and sends msg; returns the server-side socket
async function host(t, ip, msg = registration(ip)) {
  const before = new Set(t.state.getWebSockets());
  const res = await t.lobby.fetch(hostReq(ip));
  assert.equal(res.status, 101);
  const ws = t.state.getWebSockets().find((w) => !before.has(w));
  await t.lobby.webSocketMessage(ws, JSON.stringify(msg));
  return ws;
}

async function list(t, ip, query) {
  const res = await t.lobby.fetch(req("/v1/hosts?" + query, ip));
  return { status: res.status, body: await res.json() };
}

test("endpoints: parsed, public only, and on the connecting address", () => {
  const cases = [
    ["203.0.113.7:3074", "203.0.113.7", true],
    ["203.0.113.7:3074", "203.0.113.8", false], // someone else's address
    ["10.1.2.3:3074", "10.1.2.3", false],
    ["192.168.1.146:3074", "192.168.1.146", false],
    ["172.20.0.1:3074", "172.20.0.1", false],
    ["100.64.0.9:3074", "100.64.0.9", false], // carrier-grade NAT
    ["127.0.0.1:3074", "127.0.0.1", false],
    ["224.0.0.1:3074", "224.0.0.1", false],
    ["8.8.8.8:0", "8.8.8.8", false],
    ["8.8.8.8:65536", "8.8.8.8", false],
    ["256.1.1.1:1", "256.1.1.1", false],
    ["8.8.8.8:1", "2001:db8::1", false], // an IPv6 connection can't vouch for an IPv4 endpoint
  ];
  for (const [ep, ip, ok] of cases) assert.equal(endpointOk(ep, ip), ok, `${ep} from ${ip}`);
  assert.deepEqual(parseEndpoint("198.51.100.4:47901"), { ip: "198.51.100.4", port: 47901 });
  assert.equal(isPublic("172.15.255.255"), true);
  assert.equal(isPublic("172.32.0.0"), true);
});

test("a host registers, is listed without its address field, and leaves with its socket", async () => {
  const t = setup();
  const ws = await host(t, "203.0.113.7");
  assert.equal(ws.sent[0].t, "ok");
  const id = ws.sent[0].id;
  const { body } = await list(t, "198.51.100.1", "title=evolution&version=3");
  assert.deepEqual(body.hosts, [{ id, endpoint: "203.0.113.7:40000", mac: "02:46:43:12:34:56", region: "us" }]);
  ws.close(1000, "done");
  assert.deepEqual((await list(t, "198.51.100.1", "title=evolution&version=3")).body.hosts, []);
});

test("bad registrations close the socket and are never listed", async () => {
  const bad = [
    { endpoint: "198.51.100.9:40000" }, // not the connecting address
    { endpoint: "192.168.0.2:40000" },
    { title: "predator" },
    { proto: 2 },
    { version: "v3" },
    { mac: "00:11:22:33:44:55" }, // not a card of ours
    { region: "usa" },
    { t: "hello" },
  ];
  for (const over of bad) {
    const t = setup();
    const ws = await host(t, "203.0.113.7", registration("203.0.113.7", over));
    assert.equal(ws.closed.code, 1008, JSON.stringify(over));
    assert.deepEqual((await list(t, "198.51.100.1", "title=evolution&version=3")).body.hosts, []);
  }
});

test("a second message from a registered host closes it", async () => {
  const t = setup();
  const ws = await host(t, "203.0.113.7");
  await t.lobby.webSocketMessage(ws, JSON.stringify(registration("203.0.113.7", { version: "4" })));
  assert.equal(ws.closed.code, 1008);
});

test("listing: by title and version, the asker's region first, capped", async () => {
  const t = setup();
  await host(t, "203.0.113.1", registration("203.0.113.1", { region: "eu" }));
  await host(t, "203.0.113.2", registration("203.0.113.2", { region: "us" }));
  await host(t, "203.0.113.3", registration("203.0.113.3", { title: "instincts", version: "1" }));
  await host(t, "203.0.113.4", registration("203.0.113.4", { version: "2" }));
  const evo = (await list(t, "198.51.100.1", "title=evolution&version=3&region=us")).body.hosts;
  assert.deepEqual(evo.map((h) => h.endpoint), ["203.0.113.2:40000", "203.0.113.1:40000"]);
  const fci = (await list(t, "198.51.100.1", "title=instincts&version=1")).body.hosts;
  assert.deepEqual(fci.map((h) => h.endpoint), ["203.0.113.3:40000"]);

  const many = setup();
  for (let i = 0; i < LIST_MAX + 6; i++) await host(many, `198.18.${i >> 2}.${(i & 3) + 1}`.replace(/^198\.18/, "203.0"));
  assert.equal((await list(many, "198.51.100.1", "title=evolution&version=3")).body.hosts.length, LIST_MAX);
  assert.equal((await list(many, "198.51.100.1", "title=evolution&version=3&max=5")).body.hosts.length, 5);
});

test("at most four hosts per address", async () => {
  const t = setup();
  for (let i = 0; i < 4; i++) await host(t, "203.0.113.7");
  const res = await t.lobby.fetch(hostReq("203.0.113.7"));
  assert.equal(res.status, 429);
  assert.equal(t.state.getWebSockets().length, 4);
});

test("connect passes the asker's endpoint and nonce to that host only", async () => {
  const t = setup();
  const a = await host(t, "203.0.113.7");
  const b = await host(t, "203.0.113.8");
  const nonce = "0123456789abcdef0123456789abcdef";
  const post = (body, ip = "198.51.100.1") =>
    t.lobby.fetch(req("/v1/connect", ip, { method: "POST", body: JSON.stringify(body) }));
  const ok = await post({ id: a.sent[0].id, endpoint: "198.51.100.1:50000", nonce });
  assert.equal(ok.status, 204);
  assert.deepEqual(a.sent[1], { t: "connect", endpoint: "198.51.100.1:50000", nonce });
  assert.equal(b.sent.length, 1); // only its "ok"
  assert.equal((await post({ id: "nope", endpoint: "198.51.100.1:50000", nonce })).status, 404);
  assert.equal((await post({ id: a.sent[0].id, endpoint: "198.51.100.2:50000", nonce })).status, 400);
  assert.equal((await post({ id: a.sent[0].id, endpoint: "198.51.100.1:50000", nonce: "xyz" })).status, 400);
});

test("connect never reaches a host that hasn't registered yet", async () => {
  const t = setup();
  await t.lobby.fetch(hostReq("203.0.113.7"));
  const pending = t.state.getWebSockets()[0];
  const id = pending.deserializeAttachment().pending;
  const res = await t.lobby.fetch(req("/v1/connect", "198.51.100.1", {
    method: "POST", body: JSON.stringify({ id, endpoint: "198.51.100.1:50000", nonce: "0".repeat(32) }),
  }));
  assert.equal(res.status, 404);
  assert.equal(pending.sent.length, 0);
});

test("listing is rate limited per address", async () => {
  const t = setup();
  for (let i = 0; i < 30; i++) assert.equal((await list(t, "198.51.100.1", "title=evolution&version=3")).status, 200);
  assert.equal((await list(t, "198.51.100.1", "title=evolution&version=3")).status, 429);
  assert.equal((await list(t, "198.51.100.2", "title=evolution&version=3")).status, 200);
});
