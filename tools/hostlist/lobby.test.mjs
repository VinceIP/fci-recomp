// node --test tools/hostlist/lobby.test.mjs   (Node 20+; no packages). The Durable Object is faked:
// the Lobby's state is in-memory, and its SQLite (the ban table) is faked with the exact statements
// the Worker uses, so a ban made here behaves like a ban on Cloudflare.

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { test } from "node:test";
import { Lobby } from "./lobby.js";

const env = { TTL_MS: 200, RELAY: "hub.example:4000" };

// The ban table in the fake: the Worker uses exactly these five statements.
function fakeStorage() {
  const bans = new Map();
  const empty = { one: () => undefined, toArray: () => [], meta: { changes: 0 } };
  return {
    sql(query, ...params) {
      const q = query.trim().toUpperCase();
      if (q.startsWith("CREATE TABLE")) return empty;
      if (q.startsWith("INSERT OR REPLACE INTO BANS")) {
        bans.set(params[0], { id: params[0], reason: params[1], date: params[2] });
        return { ...empty, meta: { changes: 1 } };
      }
      if (q.startsWith("SELECT 1 FROM BANS WHERE ID"))
        return { one: () => (bans.has(params[0]) ? { 1: 1 } : undefined), toArray: () => [], meta: { changes: 0 } };
      if (q.startsWith("SELECT ID, REASON, DATE FROM BANS"))
        return { one: () => undefined, toArray: () => [...bans.values()], meta: { changes: 0 } };
      if (q.startsWith("DELETE FROM BANS WHERE ID"))
        return { ...empty, meta: { changes: bans.delete(params[0]) ? 1 : 0 } };
      throw new Error("fake storage: unknown query: " + query);
    },
  };
}

const mk = (over = {}) => new Lobby({ storage: fakeStorage() }, { ...env, ...over });

// cf-connecting-ip is how the Worker sees the client's address (Cloudflare sets the header;
// the tests set it here, as request.cf is Workers-only).
function req(path, { method = "GET", body, ip = "8.8.8.7", headers = {} } = {}) {
  const h = { "cf-connecting-ip": ip, ...(body === undefined ? {} : { "content-type": "application/json" }), ...headers };
  return new Request("https://list.test" + path, { method, body: body === undefined ? undefined : JSON.stringify(body), headers: h });
}

function reg(over = {}, ip) {
  return req("/v1/register", {
    method: "POST",
    ip,
    body: {
      proto: 1,
      title: "instincts",
      version: "1",
      mac: "02:46:43:11:22:33",
      endpoint: (ip || "8.8.8.7") + ":40000",
      token: "a".repeat(64),
      ...over,
    },
  });
}

function conn(id, ip, token, endpoint) {
  return req("/v1/connect", {
    method: "POST",
    ip,
    body: { id, endpoint: endpoint || ip + ":41000", nonce: "e".repeat(32), mac: "02:46:43:aa:bb:cc", token },
  });
}

const sha16 = (s) => createHash("sha256").update(s).digest("hex").slice(0, 32);

async function r(lobby, request) {
  const response = await lobby.fetch(request);
  return [response.status, await response.json().catch(() => null)];
}

test("endpoints: the four v1 routes and nothing else", async () => {
  const lobby = mk();
  let [status] = await r(lobby, req("/"));
  assert.equal(status, 404);
  [status] = await r(lobby, req("/v1/hosts"));
  assert.equal(status, 200);
  [status] = await r(lobby, req("/v1/nope"));
  assert.equal(status, 404);
  [status] = await r(lobby, req("/v1/register")); // the wrong method
  assert.equal(status, 404);
});

test("the whole flow: register, id-only listing, connect, long-poll delivery", async () => {
  const lobby = mk();
  const hToken = "c".repeat(64);
  let [status, body] = await r(lobby, reg({ token: hToken }));
  assert.equal(status, 200);
  const hid = body.id;
  assert.equal(hid, sha16(hToken)); // the id is derived from the token
  assert.equal(body.token, undefined); // the token is never returned

  [status, body] = await r(lobby, req("/v1/hosts?title=instincts&version=1"));
  assert.equal(status, 200);
  assert.equal(body.proto, 1);
  assert.equal(body.relay, "hub.example:4000"); // the canonical hub in the listing
  assert.deepEqual(body.hosts, [{ id: hid }]); // ids only

  // the guest connects with its own token; the reply is the host's endpoint and MAC
  [status, body] = await r(lobby, conn(hid, "93.184.100.9", "d".repeat(64)));
  assert.equal(status, 200);
  assert.equal(body.endpoint, "8.8.8.7:40000");
  assert.equal(body.mac, "02:46:43:11:22:33");

  // the host's long-poll (its own token, its own address) delivers the connect
  [status, body] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1`));
  assert.equal(status, 200);
  assert.deepEqual(body.connects, [
    { endpoint: "93.184.100.9:41000", nonce: "e".repeat(32), mac: "02:46:43:aa:bb:cc" },
  ]);
});

test("a listing has no endpoints, no MACs and no tokens", async () => {
  const lobby = mk();
  const t1 = "1".repeat(64);
  const t2 = "2".repeat(64);
  await r(lobby, reg({ token: t1 }));
  await r(lobby, reg({ token: t2, endpoint: "8.8.8.8:40001" }, "8.8.8.8"));
  const [status, body] = await r(lobby, req("/v1/hosts?title=instincts&version=1"));
  assert.equal(status, 200);
  assert.equal(body.hosts.length, 2);
  const text = JSON.stringify(body);
  assert.ok(!text.includes("40000") && !text.includes("40001"), "no endpoint in the listing");
  assert.ok(!text.includes("02:46:43"), "no MAC in the listing");
  assert.ok(!text.includes(t1) && !text.includes(t2), "no token in the listing");
  assert.deepEqual(body.hosts.map((h) => Object.keys(h)), [["id"], ["id"]]);
});

test("poll auth: a token that does not derive to the id, another address, an unknown id: 404", async () => {
  const lobby = mk({ TTL_MS: 5000 });
  const hToken = "3".repeat(64);
  const [, regBody] = await r(lobby, reg({ token: hToken }));
  const hid = regBody.id;
  let [status] = await r(lobby, req(`/v1/connects?id=${hid}&token=${"4".repeat(64)}&wait=1`));
  assert.equal(status, 404);
  [status] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1`, { ip: "93.184.100.9" }));
  assert.equal(status, 404);
  [status] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken.slice(1) + "0"}&wait=1`));
  assert.equal(status, 404);
  [status] = await r(lobby, req(`/v1/connects?id=${"9".repeat(32)}&token=${"9".repeat(64)}&wait=1`));
  assert.equal(status, 404);
  [status] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1`));
  assert.equal(status, 200);
});

test("a banned id cannot host, cannot join, and a banned host is gone", async () => {
  const lobby = mk();
  const t = "5".repeat(64);
  const id = sha16(t);
  const auth = { headers: { authorization: "Bearer s3cret" } };

  // admin is off without a secret: no ban can be made
  let [status] = await r(lobby, req("/admin/bans", { method: "POST", body: { id, reason: "cheating" } }));
  assert.equal(status, 503);
  [status] = await r(lobby, reg({ token: t }));
  assert.equal(status, 200); // not banned

  lobby.env.ADMIN_SECRET = "s3cret";
  // a banned id cannot register (host)
  [status] = await r(lobby, req("/admin/bans", { ...auth, method: "POST", body: { id, reason: "cheating" } }));
  assert.equal(status, 200);
  [status] = await r(lobby, reg({ token: t, endpoint: "93.184.100.2:40000" }, "93.184.100.2"));
  assert.equal(status, 403);

  // a banned id cannot connect (join anyone)
  const hToken = "6".repeat(64);
  const [, regBody] = await r(lobby, reg({ token: hToken, endpoint: "93.184.100.3:40002" }, "93.184.100.3"));
  const hid = regBody.id;
  [status] = await r(lobby, conn(hid, "93.184.100.4", t));
  assert.equal(status, 403);

  // a banned host is a no-such-host to connectors
  [status] = await r(lobby, conn(id, "93.184.100.5", "7".repeat(64)));
  assert.equal(status, 404);

  // the ban is listed; unban lifts it
  let body;
  [status, body] = await r(lobby, req("/admin/bans", auth));
  assert.equal(body.bans.length, 1);
  assert.equal(body.bans[0].id, id);
  assert.equal(body.bans[0].reason, "cheating");
  [status, body] = await r(lobby, req(`/admin/bans?id=${id}`, { ...auth, method: "DELETE" }));
  assert.equal(status, 200);
  assert.equal(body.removed, true);
  [status] = await r(lobby, reg({ token: t, endpoint: "93.184.100.6:40003" }, "93.184.100.6"));
  assert.equal(status, 200);
});

test("a banned host leaves the list on its next poll", async () => {
  const lobby = mk();
  lobby.env.ADMIN_SECRET = "s3cret";
  const hToken = "e".repeat(64);
  const hid = sha16(hToken);
  const [, regBody] = await r(lobby, reg({ token: hToken }));
  assert.equal(regBody.id, hid);
  await r(lobby, req("/admin/bans", { method: "POST", headers: { authorization: "Bearer s3cret" }, body: { id: hid } }));
  let [status] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1`));
  assert.equal(status, 404);
  const [, listBody] = await r(lobby, req("/v1/hosts?title=instincts&version=1"));
  assert.equal(listBody.hosts.length, 0);
});

test("a replayed connect is not queued twice", async () => {
  const lobby = mk();
  const hToken = "8".repeat(64);
  const [, regBody] = await r(lobby, reg({ token: hToken }));
  const hid = regBody.id;
  let [status] = await r(lobby, conn(hid, "93.184.100.9", "9".repeat(64)));
  assert.equal(status, 200);
  [status] = await r(lobby, conn(hid, "93.184.100.9", "9".repeat(64))); // replay
  assert.equal(status, 200);
  let body;
  [status, body] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1`));
  assert.equal(status, 200);
  assert.equal(body.connects.length, 1);
});

test("hostile bodies: oversized, bad JSON, the wrong content type, a bad token", async () => {
  const lobby = mk();
  const bigReg = {
    proto: 1, title: "instincts", version: "1", mac: "02:46:43:11:22:33",
    endpoint: "8.8.8.7:40000", token: "0".repeat(64), padding: "x".repeat(600),
  };
  let [status] = await r(lobby, req("/v1/register", { method: "POST", body: bigReg }));
  assert.equal(status, 413);
  const hToken = "1".repeat(64);
  const [, regBody] = await r(lobby, reg({ token: hToken }));
  const hid = regBody.id;
  const bigConn = {
    id: hid, endpoint: "93.184.100.9:41000", nonce: "e".repeat(32), mac: "02:46:43:aa:bb:cc",
    token: "2".repeat(64), padding: "x".repeat(600),
  };
  [status] = await r(lobby, req("/v1/connect", { method: "POST", ip: "93.184.100.9", body: bigConn }));
  assert.equal(status, 413);
  [status] = await r(lobby, req("/v1/register", { method: "POST", body: "{nope", headers: { "content-type": "application/json" } }));
  assert.equal(status, 400);
  [status] = await r(lobby, req("/v1/register", { method: "POST", body: { proto: 1 }, headers: { "content-type": "text/plain" } }));
  assert.equal(status, 415);
  [status] = await r(lobby, req("/v1/connect", { method: "POST", ip: "93.184.100.9", body: [1, 2], headers: { "content-type": "application/json" } }));
  assert.equal(status, 400);
  [status] = await r(lobby, reg({ token: "xyz" }));
  assert.equal(status, 400);
});

test("flooding one host: 24 connects a minute, then busy; the queue keeps the 8 newest", async () => {
  const lobby = mk();
  const hToken = "3".repeat(64);
  const [, regBody] = await r(lobby, reg({ token: hToken }));
  const hid = regBody.id;
  let status, body;
  for (let i = 0; i < 24; i++) {
    [status] = await r(lobby, conn(hid, `93.184.100.${i + 1}`, (4096 + 7 * i).toString(16).padStart(4, "0").repeat(16)));
    assert.equal(status, 200, `connect ${i + 1} accepted`);
  }
  [status, body] = await r(lobby, conn(hid, "93.184.100.250", "5".repeat(64)));
  assert.equal(status, 429);
  assert.equal(body.error, "host busy");
  [status, body] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1`));
  assert.equal(status, 200);
  assert.equal(body.connects.length, 8);
  assert.equal(body.connects[0].endpoint, "93.184.100.17:41000"); // the oldest 16 were dropped
});

test("per-address rate limits: 30 listings, 10 registers, 60 connects a minute", async () => {
  const lobby = mk();
  let status;
  for (let i = 0; i < 30; i++) [status] = await r(lobby, req("/v1/hosts"));
  assert.equal(status, 200);
  [status] = await r(lobby, req("/v1/hosts"));
  assert.equal(status, 429);

  // re-registers of the same id count toward the register limit (the live-host cap is 4)
  for (let i = 0; i < 10; i++) [status] = await r(lobby, reg({ token: "b".repeat(64) }));
  assert.equal(status, 200);
  [status] = await r(lobby, reg({ token: "b".repeat(64) }));
  assert.equal(status, 429);

  // spread over three hosts so the per-host budget (24 a minute) does not trip first
  const hosts = [];
  for (let i = 0; i < 3; i++) {
    const [s, b] = await r(lobby, reg({ token: ("c" + i).repeat(32) }, `8.8.8.${10 + i}`));
    assert.equal(s, 200);
    hosts.push(b.id);
  }
  for (let i = 0; i < 60; i++) {
    [status] = await r(lobby, conn(hosts[i % 3], "93.184.100.200", "d".repeat(64)));
    assert.equal(status, 200, `connect ${i + 1} accepted`);
  }
  [status] = await r(lobby, conn(hosts[0], "93.184.100.200", "d".repeat(64)));
  assert.equal(status, 429);
});

test("the hosts-per-address cap is 4", async () => {
  const lobby = mk();
  let status;
  for (let i = 0; i < 4; i++) {
    [status] = await r(lobby, reg({ token: ("a" + i).repeat(32) }));
    assert.equal(status, 200);
  }
  [status] = await r(lobby, reg({ token: "f".repeat(64) }));
  assert.equal(status, 429);
});

test("a private endpoint is refused", async () => {
  const lobby = mk();
  const [status, body] = await r(lobby, reg({ token: "a".repeat(64), endpoint: "192.168.1.5:40000" }));
  assert.equal(status, 400);
  assert.equal(body.error, "bad registration");
});

test("an IPv6 caller may register an IPv4 endpoint (the endpoint need not equal the caller)", async () => {
  const lobby = mk();
  // A dual-stack home: the Worker sees the caller over IPv6, the game registers its IPv4 STUN endpoint.
  const [status, body] = await r(
    lobby,
    req("/v1/register", {
      method: "POST",
      ip: "2600:1f18:90e:6e01::1",
      body: {
        proto: 1,
        title: "instincts",
        version: "1",
        mac: "02:46:43:11:22:33",
        endpoint: "8.8.8.7:40000",
        token: "a".repeat(64),
      },
    })
  );
  assert.equal(status, 200);
  assert.equal(body.id, sha16("a".repeat(64)));
});

test("the live-hosts-per-endpoint-IP cap is 4", async () => {
  const lobby = mk();
  let status, body;
  // four different players (tokens, all from one address) listing the same endpoint IP on different ports
  for (let i = 0; i < 4; i++) {
    [status] = await r(lobby, reg({ token: ("a" + i).repeat(32), endpoint: "8.8.4.1:" + (40000 + i) }));
    assert.equal(status, 200);
  }
  // a fifth host at the same endpoint IP is refused, from a fresh address (so the per-address cap
  // does not fire first and the endpoint-IP cap is the one under test)
  [status, body] = await r(lobby, reg({ token: "f".repeat(64), endpoint: "8.8.4.1:40999" }, "93.184.100.9"));
  assert.equal(status, 429);
  assert.equal(body.error, "too many hosts at this endpoint");
});

test("the pending-connects-per-endpoint-IP cap is 8", async () => {
  const lobby = mk();
  // nine hosts (fresh addresses) so the per-host queue (8) never fills; the cap under test is per endpoint IP
  const hids = [];
  for (let i = 0; i < 9; i++) {
    const t = ("a" + i).repeat(32); // 'a' + 0-8 are all hex
    const [s, b] = await r(lobby, reg({ token: t, endpoint: "8.8.4." + (10 + i) + ":40000" }, "8.8.4." + (10 + i)));
    assert.equal(s, 200);
    hids.push(b.id);
  }
  let status, body;
  // eight guests (fresh addresses) all listing the same guest endpoint IP (different ports), to eight hosts
  for (let i = 0; i < 8; i++) {
    [status] = await r(lobby, conn(hids[i], "93.184.100." + (20 + i), ("b" + i).repeat(32), "8.8.4.2:" + (50000 + i)));
    assert.equal(status, 200, `guest ${i + 1} accepted`);
  }
  // a ninth pending connect at the same guest endpoint IP is refused
  [status, body] = await r(lobby, conn(hids[8], "93.184.100.99", ("b" + 8).repeat(32), "8.8.4.2:59999"));
  assert.equal(status, 429);
  assert.equal(body.error, "too many connects at this endpoint");
});

test("a long-poll holds for the wait; a host that stops polling expires (TTL)", async () => {
  // The poll is the renewal: with a TTL longer than the wait, the poll holds and returns empty.
  const lobby = mk({ TTL_MS: 5000 });
  const hToken = "c".repeat(64);
  const [, regBody] = await r(lobby, reg({ token: hToken }));
  const hid = regBody.id;
  const t0 = Date.now();
  let [status, body] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1`));
  assert.ok(Date.now() - t0 >= 900); // it held for the wait
  assert.equal(status, 200);
  assert.deepEqual(body, { connects: [] });
  let listBody;
  [, listBody] = await r(lobby, req("/v1/hosts?title=instincts&version=1"));
  assert.equal(listBody.hosts.length, 1);

  // A host that stops polling leaves the list when its TTL runs out.
  const short = mk({ TTL_MS: 60 });
  await r(short, reg({ token: hToken }));
  [, listBody] = await r(short, req("/v1/hosts?title=instincts&version=1"));
  assert.equal(listBody.hosts.length, 1);
  await new Promise((res) => setTimeout(res, 100));
  [, listBody] = await r(short, req("/v1/hosts?title=instincts&version=1"));
  assert.equal(listBody.hosts.length, 0);
});

test("the long-poll wait is clamped to [1, 60] s; the TTL is above the ceiling", async () => {
  // The ceiling is 60 s and the default TTL is 90 s: a full-length poll can never race the
  // expiry and flap the host. The floor is 1 s (wait=0 holds for a second, not instant).
  const lobby = mk({ TTL_MS: 100000 }); // the expiry cannot cut in here
  const hToken = "b".repeat(64);
  const [, regBody] = await r(lobby, reg({ token: hToken }));
  const hid = regBody.id;

  let t0 = Date.now();
  let [status, body] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=0`));
  assert.ok(Date.now() - t0 >= 900, "wait=0 clamps to 1 s");
  assert.equal(status, 200);
  assert.deepEqual(body, { connects: [] });

  // wait=99999 clamps to 60 s: the poll is still pending a moment later (no short fallback),
  // the host survives it, and a second poll replaces the first without losing the entry.
  const pending = r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=99999`));
  await new Promise((res) => setTimeout(res, 1200));
  let listBody;
  [, listBody] = await r(lobby, req("/v1/hosts?title=instincts&version=1"));
  assert.equal(listBody.hosts.length, 1);
  [status, body] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1`));
  assert.equal(status, 200); // the new poll times out normally
  assert.deepEqual(body, { connects: [] });
  [status] = await pending; // the replaced poll ended in a 204
  assert.equal(status, 204);
  [, listBody] = await r(lobby, req("/v1/hosts?title=instincts&version=1"));
  assert.equal(listBody.hosts.length, 1); // the host is still there
});

test("the admin routes: the secret gates everything; bans and the abuse counters", async () => {
  const lobby = mk();
  lobby.env.ADMIN_SECRET = "s3cret";
  const hToken = "c".repeat(64);
  const [, regBody] = await r(lobby, reg({ token: hToken }));
  const hid = regBody.id;
  const auth = { headers: { authorization: "Bearer s3cret" } };

  let [status] = await r(lobby, req("/admin/hosts"));
  assert.equal(status, 401); // no secret header
  [status] = await r(lobby, req("/admin/hosts", { headers: { authorization: "Bearer wrong" } }));
  assert.equal(status, 401);
  let body;
  [status, body] = await r(lobby, req("/admin/hosts", auth));
  assert.equal(status, 200);
  assert.equal(body.hosts.length, 1);
  assert.equal(body.hosts[0].id, hid);
  assert.equal(body.hosts[0].ip, "8.8.8.7");

  [status] = await r(lobby, req("/admin/bans", { ...auth, method: "POST", body: { id: "zzz", reason: "x" } }));
  assert.equal(status, 400); // a bad id
  [status] = await r(lobby, req("/admin/bans", { ...auth, method: "POST", body: { id: hid, reason: "x".repeat(500) } }));
  assert.equal(status, 413); // an oversized body
  [status, body] = await r(lobby, req("/admin/bans", { ...auth, method: "POST", body: { id: hid, reason: "flood" } }));
  assert.equal(status, 200);
  assert.equal(body.ok, true);
  [status, body] = await r(lobby, req("/admin/bans", auth));
  assert.equal(body.bans.length, 1);
  [status] = await r(lobby, req("/admin/bans?id=zzz", { ...auth, method: "DELETE" }));
  assert.equal(status, 400);
  [status, body] = await r(lobby, req(`/admin/bans?id=${hid}`, { ...auth, method: "DELETE" }));
  assert.equal(status, 200);
  assert.equal(body.removed, true);

  // the abuse counters see the public refusals (the admin's own refusals are not counted)
  await r(lobby, req("/v1/connect", { method: "POST", ip: "93.184.100.9", body: [1, 2], headers: { "content-type": "application/json" } }));
  [status, body] = await r(lobby, req("/admin/abuse", auth));
  assert.equal(status, 200);
  assert.ok(body.ring.some((e) => e.kind === "bad"));
  assert.ok(body.byIp.length >= 1);
});
