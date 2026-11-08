// node --test tools/hostlist/lobby.test.mjs   (Node 20+; no packages). The Durable Object is faked:
// the Lobby's state is in-memory, so the fake needs nothing. close() clears the live timers (the
// 60 s entry TTLs) so the Node process can exit; in Workers the object's own life owns them.
import assert from "node:assert/strict";
import { test } from "node:test";
import { LIST_MAX, Lobby, endpointOk, isPublic, parseEndpoint } from "./lobby.js";

const settle = (ms) => new Promise((r) => setTimeout(r, ms));

function setup(ttlMs) {
  const lobby = new Lobby({}, ttlMs ? { TTL_MS: ttlMs } : {});
  return {
    lobby,
    close() {
      for (const h of lobby.hosts.values()) clearTimeout(h.timer);
      for (const w of lobby.waiters.values()) clearTimeout(w.timer);
    },
  };
}

const req = (path, ip, init = {}) =>
  new Request("https://list.example" + path, { ...init, headers: { "cf-connecting-ip": ip, ...(init.headers || {}) } });

async function register(t, ip, over = {}) {
  const res = await t.lobby.fetch(req("/v1/register", ip, {
    method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify({ proto: 1, title: "evolution", version: "3", mac: "02:46:43:12:34:56", endpoint: `${ip}:40000`, ...over }),
  }));
  return { status: res.status, body: res.status === 200 ? await res.json() : null };
}

async function list(t, ip, query) {
  const res = await t.lobby.fetch(req("/v1/hosts?" + query, ip));
  return { status: res.status, body: await res.json() };
}

const poll = (t, e, wait, ip = "203.0.113.7") =>
  t.lobby.fetch(req(`/v1/connects?id=${e.id}&token=${e.token}&wait=${wait}`, ip));

const connect = (t, body, ip = "198.51.100.1") =>
  t.lobby.fetch(req("/v1/connect", ip, {
    method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(body),
  }));

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

test("register -> list -> connect: the waiting poll is answered with the connect", async () => {
  const t = setup();
  const reg = await register(t, "203.0.113.7");
  assert.equal(reg.status, 200);
  const { id, token } = reg.body;
  assert.match(id, /^[0-9a-f]{24}$/);
  assert.match(token, /^[0-9a-f]{32}$/);
  const found = await list(t, "198.51.100.1", "title=evolution&version=3");
  assert.deepEqual(found.body, { proto: 1, hosts: [{ id, endpoint: "203.0.113.7:40000", mac: "02:46:43:12:34:56" }] });

  const nonce = "0123456789abcdef0123456789abcdef";
  const p = poll(t, { id, token }, 30);
  await settle(20); // the poll is held
  const c = await connect(t, { id, endpoint: "198.51.100.1:50000", nonce, mac: "02:46:43:aa:bb:cc" });
  assert.equal(c.status, 204);
  const pr = await p;
  assert.equal(pr.status, 200);
  assert.deepEqual(await pr.json(), { connects: [{ endpoint: "198.51.100.1:50000", nonce, mac: "02:46:43:aa:bb:cc" }] });
  // the connects are one-shot: the next poll times out, and the host is still listed
  assert.equal((await poll(t, { id, token }, 1)).status, 204);
  assert.equal((await list(t, "198.51.100.1", "title=evolution&version=3")).body.hosts.length, 1);
  t.close();
});

test("connects queue per host: both guests are delivered, in order", async () => {
  const t = setup();
  const e = (await register(t, "203.0.113.7")).body;
  const mac = "02:46:43:aa:bb:cc";
  const n1 = "0123456789abcdef0123456789abcdef";
  const n2 = "fedcba9876543210fedcba9876543210";
  assert.equal((await connect(t, { id: e.id, endpoint: "198.51.100.1:50000", nonce: n1, mac })).status, 204);
  assert.equal((await connect(t, { id: e.id, endpoint: "198.51.100.2:50000", nonce: n2, mac }, "198.51.100.2")).status, 204);
  const pr = await poll(t, e, 30);
  assert.equal(pr.status, 200);
  assert.deepEqual(await pr.json(), {
    connects: [
      { endpoint: "198.51.100.1:50000", nonce: n1, mac },
      { endpoint: "198.51.100.2:50000", nonce: n2, mac },
    ],
  });
  t.close();
});

test("the queue caps at 8: the oldest guest is dropped", async () => {
  const t = setup();
  const e = (await register(t, "203.0.113.7")).body;
  const mac = "02:46:43:aa:bb:cc";
  for (let i = 0; i < 9; i++) {
    const ip = `198.51.100.${i + 1}`;
    assert.equal((await connect(t, { id: e.id, endpoint: `${ip}:50000`, nonce: `${i.toString(16).padStart(32, "0")}`, mac }, ip)).status, 204);
  }
  const pr = await poll(t, e, 30);
  const { connects } = await pr.json();
  assert.equal(connects.length, 8);
  assert.equal(connects[0].endpoint, "198.51.100.2:50000"); // the first guest was dropped
  assert.equal(connects[7].endpoint, "198.51.100.9:50000");
  t.close();
});

test("poll times out with 204 and refreshes the TTL", async () => {
  const t = setup();
  const e = (await register(t, "203.0.113.7")).body;
  const started = Date.now();
  assert.equal((await poll(t, e, 1)).status, 204);
  assert.ok(Date.now() - started >= 900, "wait=1 held for a second");
  assert.equal((await list(t, "198.51.100.1", "title=evolution&version=3")).body.hosts.length, 1);
  t.close();
});

test("a second poll by the host replaces the first (204) and gets the connect", async () => {
  const t = setup();
  const e = (await register(t, "203.0.113.7")).body;
  const p1 = poll(t, e, 30);
  await settle(20);
  const p2 = poll(t, e, 30);
  await settle(20);
  const nonce = "0123456789abcdef0123456789abcdef";
  await connect(t, { id: e.id, endpoint: "198.51.100.1:50000", nonce, mac: "02:46:43:aa:bb:cc" });
  const [r1, r2] = await Promise.all([p1, p2]);
  assert.equal(r1.status, 204); // replaced
  assert.equal(r2.status, 200);
  assert.equal((await r2.json()).connects[0].nonce, nonce);
  t.close();
});

test("polls are private: wrong token, other address, and strangers are all 404", async () => {
  const t = setup();
  const e = (await register(t, "203.0.113.7")).body;
  assert.equal((await poll(t, { id: e.id, token: "0".repeat(32) }, 1)).status, 404); // wrong token
  assert.equal((await poll(t, { id: e.id, token: e.token }, 1, "203.0.113.8")).status, 404); // other address
  assert.equal((await poll(t, { id: "nope", token: e.token }, 1)).status, 404); // unknown id
  assert.equal((await poll(t, { id: e.id, token: "" }, 1)).status, 404); // no token
  t.close();
});

test("a stranger's poll does not disturb the host's held poll", async () => {
  const t = setup();
  const e = (await register(t, "203.0.113.7")).body;
  const p = poll(t, e, 30);
  await settle(20);
  // a browser saw the id in the list; it cannot read the guests, replace the poll, or refresh the TTL
  assert.equal((await t.lobby.fetch(req(`/v1/connects?id=${e.id}&token=${"f".repeat(32)}&wait=30`, "198.51.100.9"))).status, 404);
  const nonce = "0123456789abcdef0123456789abcdef";
  await connect(t, { id: e.id, endpoint: "198.51.100.1:50000", nonce, mac: "02:46:43:aa:bb:cc" });
  const pr = await p;
  assert.equal(pr.status, 200);
  assert.equal((await pr.json()).connects[0].nonce, nonce); // the host's poll still got it
  t.close();
});

test("a host that stops polling expires: the held poll is released, the connects do not outlive it", async () => {
  const t = setup(80);
  const e = (await register(t, "203.0.113.7")).body;
  const started = Date.now();
  const res = await poll(t, e, 30); // held, then released by the expiry, not by the 30 s timeout
  assert.ok(Date.now() - started < 15000, "well under the 30 s wait");
  assert.equal(res.status, 204);
  assert.deepEqual((await list(t, "198.51.100.1", "title=evolution&version=3")).body.hosts, []);
  // a guest's connects are dropped with the entry
  const e2 = (await register(t, "203.0.113.7")).body;
  const nonce = "0123456789abcdef0123456789abcdef";
  assert.equal((await connect(t, { id: e2.id, endpoint: "198.51.100.1:50000", nonce, mac: "02:46:43:aa:bb:cc" })).status, 204);
  await settle(120);
  assert.equal((await poll(t, e2, 1)).status, 404);
  t.close();
});

test("a re-register from the same address is a fresh entry", async () => {
  const t = setup();
  const a = (await register(t, "203.0.113.7")).body;
  const b = (await register(t, "203.0.113.7")).body;
  assert.notEqual(a.id, b.id);
  assert.notEqual(a.token, b.token);
  const found = (await list(t, "198.51.100.1", "title=evolution&version=3")).body.hosts;
  assert.equal(found.length, 2);
  assert.ok(!found.some((h) => "token" in h)); // the token is never listed
  t.close();
});

test("listing: by title and version, capped", async () => {
  const t = setup();
  await register(t, "203.0.113.1");
  await register(t, "203.0.113.2", { title: "instincts", version: "1" });
  await register(t, "203.0.113.3", { version: "2" });
  const evo = (await list(t, "198.51.100.1", "title=evolution&version=3")).body.hosts;
  assert.deepEqual(evo.map((h) => h.endpoint), ["203.0.113.1:40000"]);
  const fci = (await list(t, "198.51.100.1", "title=instincts&version=1")).body.hosts;
  assert.deepEqual(fci.map((h) => h.endpoint), ["203.0.113.2:40000"]);
  t.close();

  const many = setup();
  for (let i = 0; i < LIST_MAX + 6; i++) await register(many, `203.0.${i >> 2}.${(i & 3) + 1}`);
  assert.equal((await list(many, "198.51.100.1", "title=evolution&version=3")).body.hosts.length, LIST_MAX);
  assert.equal((await list(many, "198.51.100.1", "title=evolution&version=3&max=5")).body.hosts.length, 5);
  many.close();
});

test("bad registrations are rejected and never listed", async () => {
  const bad = [
    { endpoint: "198.51.100.9:40000" }, // not the connecting address
    { endpoint: "192.168.0.2:40000" },
    { title: "predator" },
    { proto: 2 },
    { version: "v3" },
    { mac: "00:11:22:33:44:55" }, // not a card of ours
    { mac: "02:46:43:12:34" }, // too short
  ];
  for (const over of bad) {
    const t = setup();
    assert.equal((await register(t, "203.0.113.7", over)).status, 400, JSON.stringify(over));
    assert.deepEqual((await list(t, "198.51.100.1", "title=evolution&version=3")).body.hosts, []);
    t.close();
  }
});

test("connect: bad mac, nonce or endpoint are rejected; unknown host is 404", async () => {
  const t = setup();
  const e = (await register(t, "203.0.113.7")).body;
  const nonce = "0123456789abcdef0123456789abcdef";
  const mac = "02:46:43:aa:bb:cc";
  const ok = { id: e.id, endpoint: "198.51.100.1:50000", nonce, mac };
  assert.equal((await connect(t, ok)).status, 204);
  assert.equal((await connect(t, { ...ok, id: "nope" })).status, 404);
  assert.equal((await connect(t, { ...ok, endpoint: "198.51.100.2:50000" })).status, 400);
  assert.equal((await connect(t, { ...ok, mac: "00:11:22:33:44:55" })).status, 400);
  assert.equal((await connect(t, { ...ok, nonce: "xyz" })).status, 400);
  assert.equal((await connect(t, { ...ok, nonce: "g".repeat(32) })).status, 400);
  t.close();
});

test("connect: the body is JSON-capped like register's", async () => {
  const t = setup();
  const e = (await register(t, "203.0.113.7")).body;
  const res = (path, ip, init) => t.lobby.fetch(req(path, ip, init));
  assert.equal((await res("/v1/connect", "198.51.100.1", {
    method: "POST", body: JSON.stringify({ id: e.id, endpoint: "198.51.100.1:50000", nonce: "0".repeat(32), mac: "02:46:43:aa:bb:cc" }),
  })).status, 415); // no content-type
  assert.equal((await res("/v1/connect", "198.51.100.1", {
    method: "POST", headers: { "content-type": "application/json" }, body: "x".repeat(513),
  })).status, 413); // over MESSAGE_MAX
  t.close();
});

test("polling an unknown id is 404", async () => {
  const t = setup();
  assert.equal((await poll(t, { id: "nope", token: "0".repeat(32) }, 1)).status, 404);
  assert.equal((await poll(t, { id: "", token: "0".repeat(32) }, 1)).status, 404);
  t.close();
});

test("listing is rate limited per address (30 a minute)", async () => {
  const t = setup();
  for (let i = 0; i < 30; i++) assert.equal((await list(t, "198.51.100.1", "title=evolution&version=3")).status, 200);
  assert.equal((await list(t, "198.51.100.1", "title=evolution&version=3")).status, 429);
  assert.equal((await list(t, "198.51.100.2", "title=evolution&version=3")).status, 200);
  t.close();
});

test("live hosts are capped per address (4)", async () => {
  const t = setup();
  for (let i = 0; i < 4; i++) assert.equal((await register(t, "203.0.9.7")).status, 200);
  assert.equal((await register(t, "203.0.9.7")).status, 429);
  assert.equal((await register(t, "203.0.9.6")).status, 200); // a different address is fine
  t.close();
});

test("registering is rate limited per address (10 a minute)", async () => {
  const t = setup(1); // entries expire at once, so the live-host cap stays out of the way
  for (let i = 0; i < 10; i++) {
    assert.equal((await register(t, "203.0.9.9")).status, 200);
    await settle(3);
  }
  assert.equal((await register(t, "203.0.9.9")).status, 429);
  assert.equal((await register(t, "203.0.9.8")).status, 200); // a different address is fine
  t.close();
});

test("connecting is rate limited per address (60 a minute)", async () => {
  const t = setup();
  const e = (await register(t, "203.0.113.7")).body;
  const body = { id: e.id, endpoint: "198.51.100.1:50000", nonce: "0123456789abcdef0123456789abcdef", mac: "02:46:43:aa:bb:cc" };
  for (let i = 0; i < 60; i++) assert.equal((await connect(t, body)).status, 204);
  assert.equal((await connect(t, body)).status, 429);
  // a different address is fine, with its own endpoint (the endpoint is checked against it)
  assert.equal((await connect(t, { ...body, endpoint: "198.51.100.2:50000" }, "198.51.100.2")).status, 204);
  t.close();
});
