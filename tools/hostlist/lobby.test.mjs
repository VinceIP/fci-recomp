// node --test tools/hostlist/lobby.test.mjs   (Node 20+; no packages). The Durable Object is faked:
// the Lobby's state is in-memory, and its SQLite (the ban table) is faked with the exact statements
// the Worker uses, so a ban made here behaves like a ban on Cloudflare.

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { test } from "node:test";
import { Lobby, TURN_WINDOW_MS, compareBuilds } from "./lobby.js";

const env = { TTL_MS: 200, RELAY: "hub.example:4000" };

// The ban table in the fake, shaped like Cloudflare's SqlStorage: storage.sql.exec(query, ...bindings)
// returns a cursor with toArray(), one() (throws unless exactly one row) and rowsWritten.
function cursor(rows, written = 0) {
  return {
    toArray: () => rows,
    one: () => {
      if (rows.length !== 1) throw new Error("Expected exactly one result from SQL query, but got " + rows.length);
      return rows[0];
    },
    rowsWritten: written,
  };
}

function fakeStorage() {
  const bans = new Map();
  const kv = new Map(); // the relay_usage table (the cached month's usage)
  return {
    sql: {
      exec(query, ...params) {
        const q = query.trim().toUpperCase();
        if (q.startsWith("CREATE TABLE")) return cursor([]);
        if (q.startsWith("INSERT OR REPLACE INTO BANS")) {
          bans.set(params[0], { id: params[0], reason: params[1], date: params[2] });
          return cursor([], 1);
        }
        if (q.startsWith("SELECT 1 FROM BANS WHERE ID")) return cursor(bans.has(params[0]) ? [{ 1: 1 }] : []);
        if (q.startsWith("SELECT ID, REASON, DATE FROM BANS")) return cursor([...bans.values()]);
        if (q.startsWith("DELETE FROM BANS WHERE ID")) return cursor([], bans.delete(params[0]) ? 1 : 0);
        if (q.startsWith("INSERT OR REPLACE INTO RELAY_USAGE")) {
          kv.set(params[0], params[1]);
          return cursor([], 1);
        }
        if (q.startsWith("SELECT V FROM RELAY_USAGE WHERE K"))
          return cursor(kv.has(params[0]) ? [{ v: kv.get(params[0]) }] : []);
        throw new Error("fake storage: unknown query: " + query);
      },
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
      proto: 2,
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

// The Worker mints through the global fetch; the tests stub it the way they fake the storage
// (a test-local stand-in, no package). The shape mirrors Cloudflare's 201 (generate-ice-servers):
// an array with a STUN-only entry and a TURN entry (the urls mix udp, tcp and turns).
const STUN_ENTRY = { urls: ["stun:stun.cloudflare.com:3478"] };
const TURN_ENTRY = {
  urls: [
    "turn:turn.cloudflare.com:3478?transport=udp",
    "turn:turn.cloudflare.com:443?transport=udp",
    "turn:turn.cloudflare.com:3478?transport=tcp",
    "turn:turn.cloudflare.com:80?transport=tcp",
    "turns:turn.cloudflare.com:5349?transport=tcp",
  ],
  username: "u1".repeat(32),
  credential: "c2".repeat(32),
};
const TURN_ENV = { TURN_KEY_ID: "keyid", TURN_KEY_API_TOKEN: "apitoken" };
// The operator's relay budget settings (the quota is on only when all three are set).
const QUOTA_ENV = {
  ...TURN_ENV,
  RELAY_BUDGET_GB: "100",
  TURN_USAGE_API_TOKEN: "usagetoken",
  TURN_USAGE_ACCOUNT_ID: "acct",
};
const cfOk = (ice) => new Response(JSON.stringify({ iceServers: ice }), { status: 201 });
const cfBad = (status, text) => new Response(text, { status });

// The GraphQL analytics endpoint and its answer shape (callsTurnUsageAdaptiveGroups, sum of
// egressBytes for the month, the fields the Worker reads).
const USAGE_URL = "https://api.cloudflare.com/client/v4/graphql";
const usageOk = (bytes) =>
  new Response(
    JSON.stringify({
      data: { viewer: { accounts: [{ callsTurnUsageAdaptiveGroups: [{ sum: { egressBytes: bytes } }] }] } },
    }),
    { status: 200 }
  );

// A fetch stub that routes the two Cloudflare calls the Worker makes: the usage read (GraphQL)
// and the mint (generate-ice-servers). `usage` and `mint` are called per request; record both.
function stubTurnUsage({ usage = () => usageOk(0), mint = () => cfOk([STUN_ENTRY, TURN_ENTRY]) } = {}) {
  const calls = { usage: [], mint: [] };
  const restore = stubFetch(async (url, init) => {
    const u = String(url);
    if (u.startsWith(USAGE_URL)) {
      calls.usage.push([u, init]);
      return usage();
    }
    if (u.includes("generate-ice-servers")) {
      calls.mint.push([u, init]);
      return mint();
    }
    throw new Error("unexpected fetch: " + u);
  });
  return { restore, calls };
}

// Rewrite the cached usage row (and drop the in-memory copy) so a test can age the cache.
function setCache(lobby, { bytes, at, month }) {
  lobby.usage = null;
  lobby.state.storage.sql.exec(
    "INSERT OR REPLACE INTO relay_usage (k, v) VALUES (?, ?)",
    "usage",
    JSON.stringify({ month: month || lobby.monthKey(Date.now()), bytes, at })
  );
}

function stubFetch(impl) {
  const old = globalThis.fetch;
  globalThis.fetch = impl;
  return () => {
    globalThis.fetch = old;
  };
}

// A host with a token, a guest token, and one connect by the guest to the host with `nonce`.
async function hostAndConn(lobby, { nonce, gToken, ip = "93.184.100.9", endpoint } = {}) {
  const hToken = "c".repeat(64);
  const [, regBody] = await r(lobby, reg({ token: hToken }));
  const hid = regBody.id;
  const [s] = await r(lobby, conn(hid, ip, gToken || "d".repeat(64), endpoint));
  if (s !== 200) throw new Error("connect refused: " + s);
  return { hToken, hid, nonce: nonce || "e".repeat(32) };
}

async function r(lobby, request) {
  const response = await lobby.fetch(request);
  return [response.status, await response.json().catch(() => null)];
}

test("net protocol: a registration with the old proto is refused, and a proto-1 host is hidden", async () => {
  const lobby = mk();
  // a registration with the old net protocol (1) is refused
  let [status, body] = await r(lobby, reg({ proto: 1 }));
  assert.equal(status, 400, JSON.stringify(body));

  // a host that got into the table with proto 1 (an older build) is not listed to a proto-2 fetch
  const hToken = "c".repeat(64);
  [status, body] = await r(lobby, reg({ token: hToken }));
  assert.equal(status, 200);
  const hid = body.id;
  lobby.hosts.set("f".repeat(32), {
    id: "f".repeat(32),
    ip: "93.184.100.9",
    proto: 1, // an older build
    title: "instincts",
    version: "1",
    mac: "02:46:43:44:55:66",
    endpoint: "93.184.100.9:40000",
    local: null,
    expires: Date.now() + 60000,
  });
  [status, body] = await r(lobby, req("/v1/hosts?title=instincts&version=1"));
  assert.equal(status, 200);
  assert.equal(body.proto, 2);
  assert.deepEqual(body.hosts, [{ id: hid }]); // the proto-1 host is hidden
});

test("endpoints: the five v1 routes and nothing else", async () => {
  const lobby = mk();
  let [status] = await r(lobby, req("/"));
  assert.equal(status, 404);
  [status] = await r(lobby, req("/v1/hosts"));
  assert.equal(status, 200);
  [status] = await r(lobby, req("/v1/nope"));
  assert.equal(status, 404);
  [status] = await r(lobby, req("/v1/register")); // the wrong method
  assert.equal(status, 404);
  [status] = await r(lobby, req("/v1/unregister")); // the wrong method (GET)
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
  assert.equal(body.proto, 2);
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
    proto: 2, title: "instincts", version: "1", mac: "02:46:43:11:22:33",
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

test("per-address rate limits: 240 listings, 40 registers, 120 connects a minute", async () => {
  const lobby = mk();
  let status;
  for (let i = 0; i < 240; i++) [status] = await r(lobby, req("/v1/hosts"));
  assert.equal(status, 200);
  [status] = await r(lobby, req("/v1/hosts"));
  assert.equal(status, 429);

  // re-registers of the same id count toward the register limit (the live-host cap is 16)
  for (let i = 0; i < 40; i++) [status] = await r(lobby, reg({ token: "b".repeat(64) }));
  assert.equal(status, 200);
  [status] = await r(lobby, reg({ token: "b".repeat(64) }));
  assert.equal(status, 429);

  // spread over six hosts so the per-host budget (24 a minute) does not trip first
  const hosts = [];
  for (let i = 0; i < 6; i++) {
    const [s, b] = await r(lobby, reg({ token: ("c" + i).repeat(32) }, `8.8.8.${10 + i}`));
    assert.equal(s, 200);
    hosts.push(b.id);
  }
  for (let i = 0; i < 120; i++) {
    [status] = await r(lobby, conn(hosts[i % 6], "93.184.100.200", "d".repeat(64)));
    assert.equal(status, 200, `connect ${i + 1} accepted`);
  }
  [status] = await r(lobby, conn(hosts[0], "93.184.100.200", "d".repeat(64)));
  assert.equal(status, 429);
});

test("the hosts-per-address cap is 16", async () => {
  const lobby = mk();
  let status;
  for (let i = 0; i < 16; i++) {
    [status] = await r(lobby, reg({ token: ("a" + i.toString(16)).repeat(32) }));
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
        proto: 2,
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

test("the live-hosts-per-endpoint-IP cap is 16", async () => {
  const lobby = mk();
  let status, body;
  // sixteen different players (tokens, all from one address) listing the same endpoint IP on different ports
  for (let i = 0; i < 16; i++) {
    [status] = await r(lobby, reg({ token: ("a" + i.toString(16)).repeat(32), endpoint: "8.8.4.1:" + (40000 + i) }));
    assert.equal(status, 200);
  }
  // a seventeenth host at the same endpoint IP is refused, from a fresh address (so the per-address cap
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

test("mint without a prior connect is 403", async () => {
  const lobby = mk(TURN_ENV);
  const [, regBody] = await r(lobby, reg({ token: "c".repeat(64) }));
  const hid = regBody.id;
  const restore = stubFetch(async () => cfOk([STUN_ENTRY, TURN_ENTRY]));
  try {
    let [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce: "e".repeat(32), token: "d".repeat(64) } }));
    assert.equal(status, 403);
    assert.equal(body.error, "no connect");
    // a connect for another host does not open this one
    await r(lobby, conn(hid, "93.184.100.9", "d".repeat(64)));
    const other = (await r(lobby, reg({ token: "a".repeat(64) }, "8.8.8.11")))[1].id;
    [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: other, nonce: "e".repeat(32), token: "d".repeat(64) } }));
    assert.equal(status, 403);
  } finally {
    restore();
  }
});

test("mint after connect: 200, the turn: UDP urls only, and a usable reply", async () => {
  const lobby = mk(TURN_ENV);
  let calls = [];
  const restore = stubFetch(async (url, init) => {
    calls.push([String(url), init && init.headers && init.headers.authorization]);
    return cfOk([STUN_ENTRY, TURN_ENTRY]);
  });
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 200);
    assert.equal(body.username, TURN_ENTRY.username);
    assert.equal(body.credential, TURN_ENTRY.credential);
    assert.deepEqual(body.urls, [
      "turn:turn.cloudflare.com:3478?transport=udp",
      "turn:turn.cloudflare.com:443?transport=udp",
    ]); // no stun, no tcp, no turns
    // the Worker called Cloudflare with the key id and the bearer token (the key never reaches the caller)
    assert.equal(calls.length, 1);
    assert.ok(calls[0][0].includes("/v1/turn/keys/keyid/credentials/generate-ice-servers"));
    assert.equal(calls[0][1], "Bearer apitoken");
    const text = JSON.stringify(body);
    assert.ok(!text.includes("apitoken") && !text.includes("keyid"), "no key material in the reply");
  } finally {
    restore();
  }
});

test("unset TURN env: 503 no relay, and Cloudflare is never called", async () => {
  const lobby = mk(); // no TURN_KEY_ID / TURN_KEY_API_TOKEN
  let called = false;
  const restore = stubFetch(async () => {
    called = true;
    return cfOk([STUN_ENTRY, TURN_ENTRY]);
  });
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 503);
    assert.deepEqual(body, { error: "no relay" });
    assert.ok(!called);
  } finally {
    restore();
  }
});

test("Cloudflare 500: 503 no relay, the body is not echoed", async () => {
  const lobby = mk(TURN_ENV);
  const secret = "CF-BODY-MUST-NOT-LEAK";
  const restore = stubFetch(async () => cfBad(500, JSON.stringify({ error: secret, detail: "boom" })));
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 503);
    assert.equal(JSON.stringify(body), JSON.stringify({ error: "no relay" }));
    assert.ok(!JSON.stringify(body).includes(secret));
    // a malformed 201 is refused the same way (no credential to hand over)
    const restore2 = stubFetch(async () => cfOk("not-an-object"));
    try {
      const [s2, b2] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
      assert.equal(s2, 503);
      assert.deepEqual(b2, { error: "no relay" });
    } finally {
      restore2();
    }
  } finally {
    restore();
  }
});

test("relay budget: under budget mints; the usage read goes to the GraphQL endpoint with the month", async () => {
  const lobby = mk(QUOTA_ENV);
  const { restore, calls } = stubTurnUsage({ usage: () => usageOk(1e9) }); // 1 GB of a 100 GB budget
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 200);
    assert.equal(body.username, TURN_ENTRY.username);
    assert.equal(calls.usage.length, 1);
    assert.ok(calls.usage[0][0].startsWith(USAGE_URL));
    assert.equal(calls.usage[0][1].headers.authorization, "Bearer usagetoken");
    const sent = JSON.parse(calls.usage[0][1].body);
    const d = new Date();
    const monthStart = new Date(Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), 1)).toISOString().slice(0, 10);
    assert.equal(sent.variables.keyId, "keyid");
    assert.equal(sent.variables.accountId, "acct");
    assert.equal(sent.variables.from, monthStart); // the UTC calendar month the budget is in
    assert.ok(sent.query.includes("callsTurnUsageAdaptiveGroups"));
    assert.ok(sent.query.includes("egressBytes"));
    assert.match(sent.query, /\$from: Date!/); // declared, a plain date
    assert.match(sent.variables.to, /^\d{4}-\d{2}-\d{2}$/);
    assert.equal(calls.mint.length, 1);
  } finally {
    restore();
  }
});

test("relay budget: at or past the budget the mint is 503 no relay, and Cloudflare is never called", async () => {
  const lobby = mk(QUOTA_ENV); // budget 100 GB
  const { restore, calls } = stubTurnUsage({ usage: () => usageOk(101e9) }); // 101 GB
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 503);
    assert.deepEqual(body, { error: "no relay" });
    assert.equal(calls.mint.length, 0); // a refused mint never burns a Cloudflare mint call
    // exactly at the budget is refused too (the budget is a cap, not a target)
    const lobby2 = mk(QUOTA_ENV);
    const t2 = stubTurnUsage({ usage: () => usageOk(100e9) });
    try {
      const c2 = await hostAndConn(lobby2, {});
      const [s2, b2] = await r(lobby2, req("/v1/turn", { method: "POST", body: { id: c2.hid, nonce: c2.nonce, token: "d".repeat(64) } }));
      assert.equal(s2, 503);
      assert.deepEqual(b2, { error: "no relay" });
    } finally {
      t2.restore();
    }
  } finally {
    restore();
  }
});

test("relay budget: the usage read fails with no cache: 503 (fail closed)", async () => {
  const lobby = mk(QUOTA_ENV);
  const { restore, calls } = stubTurnUsage({ usage: () => cfBad(500, "nope") });
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 503);
    assert.deepEqual(body, { error: "no relay" });
    assert.equal(calls.usage.length, 1);
    assert.equal(calls.mint.length, 0);
  } finally {
    restore();
  }
});

test("relay budget: the usage read fails with a recent cache: the cache is trusted (fail open under 1 h)", async () => {
  const lobby = mk(QUOTA_ENV);
  // first mint: the read succeeds and the cache is stored
  let t = stubTurnUsage({ usage: () => usageOk(1e9) });
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 200);
  } finally {
    t.restore();
  }
  // age the cache past the 5-minute read interval, then break the usage endpoint
  setCache(lobby, { bytes: 1e9, at: Date.now() - 10 * 60 * 1000 });
  t = stubTurnUsage({ usage: () => cfBad(500, "nope") });
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 200, "a cache under 1 h old is trusted when the read fails");
    assert.equal(body.username, TURN_ENTRY.username);
    assert.equal(t.calls.usage.length, 1); // it did try the read
    assert.equal(t.calls.mint.length, 1);
  } finally {
    t.restore();
  }
});

test("relay budget: the usage read fails and the cache is over 1 h old: 503 (fail closed)", async () => {
  const lobby = mk(QUOTA_ENV);
  let t = stubTurnUsage({ usage: () => usageOk(1e9) });
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 200);
  } finally {
    t.restore();
  }
  setCache(lobby, { bytes: 1e9, at: Date.now() - 2 * 60 * 60 * 1000 }); // 2 h old
  t = stubTurnUsage({ usage: () => cfBad(500, "nope") });
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 503);
    assert.deepEqual(body, { error: "no relay" });
    assert.equal(t.calls.mint.length, 0);
  } finally {
    t.restore();
  }
});

test("relay budget: a month rollover resets the budget (the old month's cache does not carry over)", async () => {
  const lobby = mk(QUOTA_ENV);
  const d = new Date();
  const last = new Date(Date.UTC(d.getUTCFullYear(), d.getUTCMonth() - 1, 15));
  const lastMonth = last.getUTCFullYear() + "-" + String(last.getUTCMonth() + 1).padStart(2, "0");
  // last month's usage, over the budget, cached
  setCache(lobby, { bytes: 999e9, at: Date.now(), month: lastMonth });
  const { restore, calls } = stubTurnUsage({ usage: () => usageOk(1e9) }); // this month: 1 GB
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 200, "the new month's usage is under the budget");
    assert.equal(body.username, TURN_ENTRY.username);
    assert.equal(calls.usage.length, 1); // a fresh read for the new month
    assert.equal(lobby.usage.month, lobby.monthKey(Date.now()));
  } finally {
    restore();
  }
});

test("relay budget: quota off (any setting missing) mints without a usage read", async () => {
  for (const over of [{}, { RELAY_BUDGET_GB: "100" }, { RELAY_BUDGET_GB: "100", TURN_USAGE_API_TOKEN: "usagetoken" }, { RELAY_BUDGET_GB: "junk", TURN_USAGE_API_TOKEN: "usagetoken", TURN_USAGE_ACCOUNT_ID: "acct" }]) {
    const lobby = mk({ ...TURN_ENV, ...over });
    let usageCalls = 0;
    const { restore, calls } = stubTurnUsage();
    try {
      const { hid, nonce } = await hostAndConn(lobby, {});
      const [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
      assert.equal(status, 200);
      assert.equal(body.username, TURN_ENTRY.username);
      usageCalls = calls.usage.length;
    } finally {
      restore();
    }
    assert.equal(usageCalls, 0, "no usage read while the quota is off");
  }
});

test("relay budget: the kill switch refuses the mint with or without a quota", async () => {
  for (const env of [TURN_ENV, QUOTA_ENV]) {
    const lobby = mk({ ...env, RELAY_KILL: "1" });
    const { restore, calls } = stubTurnUsage();
    try {
      const { hid, nonce } = await hostAndConn(lobby, {});
      const [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
      assert.equal(status, 503);
      assert.deepEqual(body, { error: "no relay" });
      assert.equal(calls.usage.length, 0);
      assert.equal(calls.mint.length, 0);
    } finally {
      restore();
    }
  }
});

test("mint rate limits: 6 per guest in 10 minutes, 6 per address in 10 minutes", async () => {
  // per guest: one connect, seven mints for the same (gid, nonce) -> the 7th is 429
  const lobby = mk(TURN_ENV);
  const restore = stubFetch(async () => cfOk([STUN_ENTRY, TURN_ENTRY]));
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    let status;
    for (let i = 0; i < 6; i++) {
      [status] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
      assert.equal(status, 200, `mint ${i + 1} accepted`);
    }
    [status] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 429);
  } finally {
    restore();
  }
  // per address: seven guests (each with its own connect) from one address -> the 7th mint is 429
  const lobby2 = mk(TURN_ENV);
  const restore2 = stubFetch(async () => cfOk([STUN_ENTRY, TURN_ENTRY]));
  try {
    const { hid } = await hostAndConn(lobby2, {});
    let status2;
    for (let i = 0; i < 7; i++) {
      const g = ("d" + i).repeat(32); // 'd' + 0-6 are all hex
      const nonce = ("e" + i).repeat(16); // a nonce is 32 hex chars
      const [cs] = await r(lobby2, conn(hid, "93.184.100.9", g));
      assert.equal(cs, 200);
      // the conn() helper's nonce is fixed; re-post with the guest's own nonce
      [status2] = await r(lobby2, req("/v1/connect", {
        method: "POST",
        ip: "93.184.100.9",
        body: { id: hid, endpoint: "93.184.100.9:41000", nonce, mac: "02:46:43:aa:bb:cc", token: g },
      }));
      assert.equal(status2, 200);
      [status2] = await r(lobby2, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: g } }));
      assert.equal(status2, i < 6 ? 200 : 429, `mint ${i + 1} for guest ${i}`);
    }
  } finally {
    restore2();
  }
});

test("relay connect without a mint is 403", async () => {
  const lobby = mk(TURN_ENV);
  const { hid, nonce } = await hostAndConn(lobby, {});
  const [status, body] = await r(lobby, req("/v1/connect", {
    method: "POST",
    ip: "93.184.100.9",
    body: { id: hid, endpoint: "198.41.200.1:30000", nonce, mac: "02:46:43:aa:bb:cc", token: "d".repeat(64), relay: true },
  }));
  assert.equal(status, 403);
  assert.equal(body.error, "no relay");
});

test("relay connect after a mint: 200, skips the endpoint-IP cap, the host's poll shows relay:true", async () => {
  const lobby = mk(TURN_ENV);
  const restore = stubFetch(async () => cfOk([STUN_ENTRY, TURN_ENTRY]));
  try {
    // fill the pending-connects-per-endpoint-IP cap (8) with eight other guests at a Cloudflare IP
    const CF_IP = "198.41.200.1";
    const hids = [];
    for (let i = 0; i < 9; i++) {
      const t = ("a" + i).repeat(32);
      const [s, b] = await r(lobby, reg({ token: t, endpoint: "8.8.4." + (10 + i) + ":40000" }, "8.8.4." + (10 + i)));
      assert.equal(s, 200);
      hids.push(b.id);
    }
    for (let i = 0; i < 8; i++) {
      const [s] = await r(lobby, conn(hids[i], "93.184.100." + (20 + i), ("b" + i).repeat(32), CF_IP + ":" + (50000 + i)));
      assert.equal(s, 200, `guest ${i + 1} accepted`);
    }
    // a ninth non-relay connect at that IP is refused (the cap is live)
    let [status, body] = await r(lobby, conn(hids[8], "93.184.100.99", ("b" + 8).repeat(32), CF_IP + ":59999"));
    assert.equal(status, 429);
    assert.equal(body.error, "too many connects at this endpoint");
    // our guest: connect, mint, relay at the same (saturated) IP -> accepted
    const gToken = "d".repeat(64);
    const nonce = "e".repeat(32);
    [status] = await r(lobby, conn(hids[8], "93.184.100.9", gToken));
    assert.equal(status, 200);
    [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hids[8], nonce, token: gToken } }));
    assert.equal(status, 200);
    [status, body] = await r(lobby, req("/v1/connect", {
      method: "POST",
      ip: "93.184.100.9",
      body: { id: hids[8], endpoint: CF_IP + ":30000", nonce, mac: "02:46:43:aa:bb:cc", token: gToken, relay: true },
    }));
    assert.equal(status, 200, "the relayed connect skips the endpoint-IP cap");
    // the host's long-poll shows the relayed address with relay:true (polled from the host's
    // own address: the poll is the id and token bound to the registration address)
    const hToken = ("a" + 8).repeat(32);
    [status, body] = await r(lobby, req(`/v1/connects?id=${hids[8]}&token=${hToken}&wait=1`, { ip: "8.8.4.18" }));
    assert.equal(status, 200);
    assert.equal(body.connects.length, 1);
    assert.deepEqual(body.connects[0], {
      endpoint: CF_IP + ":30000",
      nonce,
      mac: "02:46:43:aa:bb:cc",
      relay: true,
    });
  } finally {
    restore();
  }
});

test("relay connects are capped at 3 per (gid, nonce)", async () => {
  const lobby = mk(TURN_ENV);
  const restore = stubFetch(async () => cfOk([STUN_ENTRY, TURN_ENTRY]));
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [ms] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(ms, 200);
    let status, body;
    for (let i = 0; i < 3; i++) {
      [status] = await r(lobby, req("/v1/connect", {
        method: "POST",
        ip: "93.184.100.9",
        body: { id: hid, endpoint: "198.41.200.1:" + (30000 + i), nonce, mac: "02:46:43:aa:bb:cc", token: "d".repeat(64), relay: true },
      }));
      assert.equal(status, 200, `relay ${i + 1} accepted`);
    }
    [status, body] = await r(lobby, req("/v1/connect", {
      method: "POST",
      ip: "93.184.100.9",
      body: { id: hid, endpoint: "198.41.200.1:30003", nonce, mac: "02:46:43:aa:bb:cc", token: "d".repeat(64), relay: true },
    }));
    assert.equal(status, 429);
    assert.equal(body.error, "too many relays");
    // a fresh nonce mints fresh and relays again (the cap is per nonce, not per guest)
    const nonce2 = "f".repeat(32);
    [status] = await r(lobby, req("/v1/connect", {
      method: "POST",
      ip: "93.184.100.9",
      body: { id: hid, endpoint: "93.184.100.9:41000", nonce: nonce2, mac: "02:46:43:aa:bb:cc", token: "d".repeat(64) },
    }));
    assert.equal(status, 200);
    [status] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce: nonce2, token: "d".repeat(64) } }));
    assert.equal(status, 200);
    [status] = await r(lobby, req("/v1/connect", {
      method: "POST",
      ip: "93.184.100.9",
      body: { id: hid, endpoint: "198.41.200.1:30004", nonce: nonce2, mac: "02:46:43:aa:bb:cc", token: "d".repeat(64), relay: true },
    }));
    assert.equal(status, 200);
  } finally {
    restore();
  }
});

test("one mint covers every host the guest reached: relayed connects to two hosts are both 200", async () => {
  const lobby = mk(TURN_ENV);
  const gToken = "d".repeat(64);
  const nonce = "e".repeat(32);
  const relay = (id, port) =>
    r(lobby, req("/v1/connect", {
      method: "POST",
      ip: "93.184.100.9",
      body: { id, endpoint: "198.41.200.1:" + port, nonce, mac: "02:46:43:aa:bb:cc", token: gToken, relay: true },
    }));
  const restore = stubFetch(async (url) => {
    if (String(url).includes("generate-ice-servers")) return cfOk([STUN_ENTRY, TURN_ENTRY]);
    throw new Error("unexpected fetch: " + url);
  });
  try {
    // two hosts; the guest connects to both (the one TURN allocation covers both)
    const h1 = (await r(lobby, reg({ token: "c".repeat(64) }, "8.8.8.13")))[1].id;
    const h2 = (await r(lobby, reg({ token: "a".repeat(64) }, "8.8.8.14")))[1].id;
    let [status, body] = await r(lobby, conn(h1, "93.184.100.9", gToken));
    assert.equal(status, 200);
    [status] = await r(lobby, conn(h2, "93.184.100.9", gToken));
    assert.equal(status, 200);
    // one mint (for the first host)
    [status] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: h1, nonce, token: gToken } }));
    assert.equal(status, 200);
    // relayed connects to both hosts: both accepted, and no second mint is made
    [status] = await relay(h1, 30001);
    assert.equal(status, 200, "relay to the minted host");
    [status] = await relay(h2, 30002);
    assert.equal(status, 200, "relay to the second host, no second mint");
    assert.equal(lobby.gidMints.size, 1);
    assert.equal(lobby.mints.size, 1);
    // the cap is per (gid, nonce) across hosts: the 4th relayed connect for the nonce is 429
    [status] = await relay(h1, 30003);
    assert.equal(status, 200, "3rd relay for the nonce");
    [status, body] = await relay(h1, 30004);
    assert.equal(status, 429, "4th relay for the nonce");
    assert.equal(body.error, "too many relays");
  } finally {
    restore();
  }
});

test("a relayed connect to a host the guest never connected to is 403, and a mint for the exact nonce still qualifies", async () => {
  const lobby = mk(TURN_ENV);
  const gToken = "d".repeat(64);
  const nonce = "e".repeat(32);
  const relay = (id, port) =>
    r(lobby, req("/v1/connect", {
      method: "POST",
      ip: "93.184.100.9",
      body: { id, endpoint: "198.41.200.1:" + port, nonce, mac: "02:46:43:aa:bb:cc", token: gToken, relay: true },
    }));
  const restore = stubFetch(async (url) => {
    if (String(url).includes("generate-ice-servers")) return cfOk([STUN_ENTRY, TURN_ENTRY]);
    throw new Error("unexpected fetch: " + url);
  });
  try {
    const h1 = (await r(lobby, reg({ token: "c".repeat(64) }, "8.8.8.13")))[1].id;
    const h2 = (await r(lobby, reg({ token: "a".repeat(64) }, "8.8.8.14")))[1].id;
    let [status, body] = await r(lobby, conn(h1, "93.184.100.9", gToken));
    assert.equal(status, 200);
    [status] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: h1, nonce, token: gToken } }));
    assert.equal(status, 200);
    // the guest never connected to h2: its relayed connect is refused
    [status, body] = await relay(h2, 30001);
    assert.equal(status, 403);
    assert.equal(body.error, "no relay");
    // after a direct connect to h2 the same relayed connect is accepted (the one mint covers it)
    [status] = await r(lobby, conn(h2, "93.184.100.9", gToken));
    assert.equal(status, 200);
    [status] = await relay(h2, 30001);
    assert.equal(status, 200);
    // the old path: a mint for the exact (gid, nonce, host) qualifies on its own
    const nonce2 = "f".repeat(32);
    [status] = await r(lobby, req("/v1/connect", {
      method: "POST", ip: "93.184.100.9",
      body: { id: h1, endpoint: "93.184.100.9:41000", nonce: nonce2, mac: "02:46:43:aa:bb:cc", token: gToken },
    }));
    assert.equal(status, 200);
    [status] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: h1, nonce: nonce2, token: gToken } }));
    assert.equal(status, 200);
    [status] = await relay(h1, 30002);
    assert.equal(status, 200, "a mint for the exact nonce still qualifies");
  } finally {
    restore();
  }
});

test("a relayed connect with no mint for the gid is 403", async () => {
  const lobby = mk(TURN_ENV);
  const { hid, nonce } = await hostAndConn(lobby, {});
  const [status, body] = await r(lobby, req("/v1/connect", {
    method: "POST",
    ip: "93.184.100.9",
    body: { id: hid, endpoint: "198.41.200.1:30000", nonce, mac: "02:46:43:aa:bb:cc", token: "d".repeat(64), relay: true },
  }));
  assert.equal(status, 403);
  assert.equal(body.error, "no relay");
});

test("a relayed connect after a mint older than the window is 403", async () => {
  const lobby = mk(TURN_ENV);
  const gToken = "d".repeat(64);
  const nonce = "e".repeat(32);
  const restore = stubFetch(async (url) => {
    if (String(url).includes("generate-ice-servers")) return cfOk([STUN_ENTRY, TURN_ENTRY]);
    throw new Error("unexpected fetch: " + url);
  });
  try {
    const { hid } = await hostAndConn(lobby, {});
    let [status] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: gToken } }));
    assert.equal(status, 200);
    // age the records past the 10-minute window (the fresh pair is replaced by the stale mint's)
    const old = Date.now() - TURN_WINDOW_MS - 1000;
    lobby.mints.forEach((v) => (v.at = old));
    lobby.gidMints.forEach((v) => (v.at = old));
    lobby.pairs.forEach((v) => (v.at = old));
    const [s, body] = await r(lobby, req("/v1/connect", {
      method: "POST",
      ip: "93.184.100.9",
      body: { id: hid, endpoint: "198.41.200.1:30000", nonce, mac: "02:46:43:aa:bb:cc", token: gToken, relay: true },
    }));
    assert.equal(s, 403);
    assert.equal(body.error, "no relay");
  } finally {
    restore();
  }
});

test("unregister: a leaving host removes its listing; idempotent; a bad token is 403", async () => {
  const lobby = mk({ TTL_MS: 5000 });
  const hToken = "c".repeat(64);
  let [status, body] = await r(lobby, reg({ token: hToken }));
  assert.equal(status, 200);
  const hid = body.id;

  // the host is listed before the unregister
  [status, body] = await r(lobby, req("/v1/hosts?title=instincts&version=1"));
  assert.deepEqual(body.hosts, [{ id: hid }]);

  // unregister with the host's own token: 200, the listing is gone
  [status, body] = await r(lobby, req("/v1/unregister", { method: "POST", body: { id: hid, token: hToken } }));
  assert.equal(status, 200);
  assert.deepEqual(body, { ok: true });
  [status, body] = await r(lobby, req("/v1/hosts?title=instincts&version=1"));
  assert.deepEqual(body.hosts, []);

  // a second unregister is a no-op (idempotent): 200
  [status, body] = await r(lobby, req("/v1/unregister", { method: "POST", body: { id: hid, token: hToken } }));
  assert.equal(status, 200);
  assert.deepEqual(body, { ok: true });

  // a token that does not derive to the id is 403 (not 404: a wrong token is not "already gone")
  [status, body] = await r(lobby, req("/v1/unregister", { method: "POST", body: { id: hid, token: "d".repeat(64) } }));
  assert.equal(status, 403);
  assert.equal(body.error, "bad token");

  // a malformed token is 403
  [status, body] = await r(lobby, req("/v1/unregister", { method: "POST", body: { id: hid, token: "xyz" } }));
  assert.equal(status, 403);
  assert.equal(body.error, "bad token");

  // an id that was never registered (a valid token that derives to it) is a 200: idempotent
  const otherToken = "f".repeat(64);
  [status, body] = await r(lobby, req("/v1/unregister", { method: "POST", body: { id: sha16(otherToken), token: otherToken } }));
  assert.equal(status, 200);
  assert.deepEqual(body, { ok: true });
});

test("unregister releases a held long-poll with an empty poll", async () => {
  const lobby = mk({ TTL_MS: 5000 });
  const hToken = "c".repeat(64);
  let [, regBody] = await r(lobby, reg({ token: hToken }));
  const hid = regBody.id;

  // the host's long-poll is held (no connect is queued, so it has nothing to deliver)
  const pollPromise = r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=10`));
  await new Promise((res) => setTimeout(res, 50)); // the poll is now held

  // the host leaves: the held poll is released with an empty, successful poll
  let [status] = await r(lobby, req("/v1/unregister", { method: "POST", body: { id: hid, token: hToken } }));
  assert.equal(status, 200);
  const [pStatus, pBody] = await pollPromise;
  assert.equal(pStatus, 200);
  assert.deepEqual(pBody, { connects: [] });

  // the listing is gone
  let [lStatus, lBody] = await r(lobby, req("/v1/hosts?title=instincts&version=1"));
  assert.equal(lStatus, 200);
  assert.deepEqual(lBody.hosts, []);
});

test("unregister clears the host's queued connects", async () => {
  const lobby = mk({ TTL_MS: 5000 });
  const hToken = "c".repeat(64);
  let [, regBody] = await r(lobby, reg({ token: hToken }));
  const hid = regBody.id;

  // a guest connect is queued
  let [status] = await r(lobby, conn(hid, "93.184.100.9", "d".repeat(64)));
  assert.equal(status, 200);

  // the host leaves: the queued connect is removed with the listing
  [status] = await r(lobby, req("/v1/unregister", { method: "POST", body: { id: hid, token: hToken } }));
  assert.equal(status, 200);

  // the listing is gone, and a re-register starts with an empty queue (the connect was not kept)
  let [, lBody] = await r(lobby, req("/v1/hosts?title=instincts&version=1"));
  assert.deepEqual(lBody.hosts, []);
  let [, rBody] = await r(lobby, reg({ token: hToken }));
  assert.equal(rBody.id, hid);
  let [pStatus, pBody] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1`));
  assert.equal(pStatus, 200);
  assert.deepEqual(pBody.connects, []); // the old connect was cleared by the unregister
});

// --- the local field (same-NAT forwarding) ---

test("local: a private IPv4 is stored on register; a public one is ignored", async () => {
  const lobby = mk();
  const hToken = "c".repeat(64);
  // a valid private address
  let [status, body] = await r(lobby, reg({ token: hToken, local: "192.168.1.50:3074" }));
  assert.equal(status, 200);
  const hid = body.id;
  // the host record has the local field
  const h = lobby.hosts.get(hid);
  assert.equal(h.local, "192.168.1.50:3074");

  // a public address is silently ignored
  const hToken2 = "e".repeat(64);
  [status, body] = await r(lobby, req("/v1/register", {
    method: "POST", ip: "8.8.8.8",
    body: { proto: 2, title: "instincts", version: "1", mac: "02:46:43:44:55:66",
            endpoint: "8.8.8.8:40001", token: hToken2, local: "8.8.8.8:3074" },
  }));
  assert.equal(status, 200);
  assert.equal(lobby.hosts.get(body.id).local, undefined);

  // a malformed local is silently ignored
  const hToken3 = "f".repeat(64);
  [status, body] = await r(lobby, req("/v1/register", {
    method: "POST", ip: "8.8.8.9",
    body: { proto: 2, title: "instincts", version: "1", mac: "02:46:43:77:88:99",
            endpoint: "8.8.8.9:40002", token: hToken3, local: "not-an-address" },
  }));
  assert.equal(status, 200);
  assert.equal(lobby.hosts.get(body.id).local, undefined);
});

test("local: forwarded in the connect reply when both sides share the caller IP", async () => {
  const lobby = mk();
  const hToken = "c".repeat(64);
  const ip = "93.184.100.9";
  // register with a local address
  let [status] = await r(lobby, req("/v1/register", {
    method: "POST", ip,
    body: { proto: 2, title: "instincts", version: "1", mac: "02:46:43:11:22:33",
            endpoint: ip + ":40000", token: hToken, local: "192.168.1.50:3074" },
  }));
  assert.equal(status, 200);
  const hid = sha16(hToken);

  // a guest from the same IP: the reply includes the host's local
  const gToken = "d".repeat(64);
  let [cStatus, cBody] = await r(lobby, req("/v1/connect", {
    method: "POST", ip, // same IP as the host
    body: { id: hid, endpoint: ip + ":41000", nonce: "e".repeat(32),
            mac: "02:46:43:aa:bb:cc", token: gToken, local: "192.168.1.51:3074" },
  }));
  assert.equal(cStatus, 200);
  assert.equal(cBody.local, "192.168.1.50:3074"); // the host's LAN address

  // a guest from a different IP: the reply does NOT include the host's local
  const gToken2 = "f".repeat(64);
  [cStatus, cBody] = await r(lobby, req("/v1/connect", {
    method: "POST", ip: "1.2.3.4", // different IP
    body: { id: hid, endpoint: "1.2.3.4:41000", nonce: "a".repeat(32),
            mac: "02:46:43:dd:ee:ff", token: gToken2 },
  }));
  assert.equal(cStatus, 200);
  assert.equal(cBody.local, undefined); // no local: different NAT
});

test("local: forwarded when the caller IPs differ (IPv6) but the STUN endpoint IPs match", async () => {
  const lobby = mk();
  const hToken = "1".repeat(64);
  const pub = "93.184.100.10";
  await r(lobby, req("/v1/register", {
    method: "POST", ip: "2600:1700::1", // the host reaches the Worker over IPv6
    body: { proto: 2, title: "evolution", version: "1", mac: "02:46:43:11:22:44",
            endpoint: pub + ":40000", token: hToken, local: "192.168.1.144:60827" },
  }));
  const hid = sha16(hToken);
  // the guest reaches it from another address (IPv4), but its STUN endpoint has the same public IP
  let [cStatus, cBody] = await r(lobby, req("/v1/connect", {
    method: "POST", ip: "69.235.55.220",
    body: { id: hid, endpoint: pub + ":42455", nonce: "2".repeat(32),
            mac: "02:46:43:aa:bb:dd", token: "3".repeat(64), local: "192.168.1.146:42455" },
  }));
  assert.equal(cStatus, 200);
  assert.equal(cBody.local, "192.168.1.144:60827");
  // the host's poll carries the guest's LAN address too
  const [, pBody] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1`, { ip: "2600:1700::1" }));
  assert.equal(pBody.connects[0].local, "192.168.1.146:42455");
  // a guest on another network (different caller IP and endpoint IP) gets no local
  [cStatus, cBody] = await r(lobby, req("/v1/connect", {
    method: "POST", ip: "1.2.3.5",
    body: { id: hid, endpoint: "1.2.3.5:41000", nonce: "4".repeat(32),
            mac: "02:46:43:aa:bb:ee", token: "5".repeat(64) },
  }));
  assert.equal(cStatus, 200);
  assert.equal(cBody.local, undefined);
});

test("local: the guest's local is in the host's poll when IPs match; absent when they differ", async () => {
  const lobby = mk({ TTL_MS: 5000 });
  const hToken = "c".repeat(64);
  const ip = "93.184.100.9";
  let [status] = await r(lobby, req("/v1/register", {
    method: "POST", ip,
    body: { proto: 2, title: "instincts", version: "1", mac: "02:46:43:11:22:33",
            endpoint: ip + ":40000", token: hToken, local: "192.168.1.50:3074" },
  }));
  assert.equal(status, 200);
  const hid = sha16(hToken);

  // a guest from the same IP, with its own local
  const gToken = "d".repeat(64);
  let [cStatus] = await r(lobby, req("/v1/connect", {
    method: "POST", ip, // same IP
    body: { id: hid, endpoint: ip + ":41000", nonce: "e".repeat(32),
            mac: "02:46:43:aa:bb:cc", token: gToken, local: "192.168.1.51:3074" },
  }));
  assert.equal(cStatus, 200);

  // the host's poll (from the same IP as the registration): the guest's local is included
  let [pStatus, pBody] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1`, { ip }));
  assert.equal(pStatus, 200);
  assert.equal(pBody.connects.length, 1);
  assert.equal(pBody.connects[0].local, "192.168.1.51:3074");

  // a guest from a different IP, with a local: the poll does NOT include the guest's local
  // (the rule is symmetric: a side's LAN address is forwarded only when both share the caller IP)
  lobby.connects.set(hid, []); // clear the queue
  const gToken2 = "f".repeat(64);
  [cStatus] = await r(lobby, req("/v1/connect", {
    method: "POST", ip: "1.2.3.4", // different IP
    body: { id: hid, endpoint: "1.2.3.4:41000", nonce: "a".repeat(32),
            mac: "02:46:43:dd:ee:ff", token: gToken2, local: "10.0.0.5:3074" },
  }));
  assert.equal(cStatus, 200);

  // the host's poll: the guest's local is absent (different caller IP)
  [pStatus, pBody] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1`, { ip }));
  assert.equal(pStatus, 200);
  assert.equal(pBody.connects.length, 1);
  assert.equal(pBody.connects[0].local, undefined);
});

test("local: 100.64/10 (CGNAT) is rejected", async () => {
  const lobby = mk();
  // a CGNAT address is not a LAN: two phones behind the same carrier share no network
  const hToken = "c".repeat(64);
  let [status, body] = await r(lobby, req("/v1/register", {
    method: "POST", ip: "8.8.8.7",
    body: { proto: 2, title: "instincts", version: "1", mac: "02:46:43:11:22:33",
            endpoint: "8.8.8.7:40000", token: hToken, local: "100.64.1.2:3074" },
  }));
  assert.equal(status, 200);
  assert.equal(lobby.hosts.get(body.id).local, undefined);

  // a connect with a CGNAT local is also rejected
  const gToken = "d".repeat(64);
  [status, body] = await r(lobby, req("/v1/connect", {
    method: "POST", ip: "8.8.8.7",
    body: { id: body.id, endpoint: "8.8.8.7:41000", nonce: "e".repeat(32),
            mac: "02:46:43:aa:bb:cc", token: gToken, local: "100.64.1.3:3074" },
  }));
  assert.equal(status, 200);
  assert.equal(body.local, undefined);
});

test("relay budget: a month with no relay traffic yet (empty usage list) mints", async () => {
  const lobby = mk(QUOTA_ENV);
  const empty = () =>
    new Response(JSON.stringify({ data: { viewer: { accounts: [{ callsTurnUsageAdaptiveGroups: [] }] } } }), { status: 200 });
  const { restore, calls } = stubTurnUsage({ usage: empty });
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 200);
    assert.equal(calls.mint.length, 1);
  } finally {
    restore();
  }
});

test("relay budget: /admin/relay reports the usage, the budget, the reason and the alert state", async () => {
  const lobby = mk({ ...QUOTA_ENV, ADMIN_SECRET: "s3cret" }); // budget 100 GB
  const auth = { headers: { authorization: "Bearer s3cret" } };
  const lines = [];
  const log = console.log;
  console.log = (m) => lines.push(String(m));
  let t = stubTurnUsage({ usage: () => usageOk(101e9) });
  try {
    let [status, body] = await r(lobby, req("/admin/relay"));
    assert.equal(status, 401);
    [status, body] = await r(lobby, req("/admin/relay", auth));
    assert.equal(status, 200);
    assert.equal(body.allow, false);
    assert.equal(body.why, "budget reached");
    assert.equal(body.usedBytes, 101e9);
    assert.equal(body.budgetBytes, 100e9);
    assert.equal(body.alertState, "budget");
    assert.ok(body.alertChangedAt, "the last change time is reported");
    assert.equal(new Date(body.alertChangedAt).getTime() > 0, true);
    // the alert line is logged once for the state change (seen on the admin call), not per mint
    const { hid, nonce } = await hostAndConn(lobby, {});
    [status] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 503);
    const alertLines = lines.filter((l) => l.startsWith("relay budget reached"));
    assert.equal(alertLines.length, 1, lines.join("\n"));
    assert.equal(alertLines[0], "relay budget reached: 101.00 GB of 100.00 GB used in " + lobby.monthKey(Date.now()));
    [status, body] = await r(lobby, req("/admin/relay", auth));
    assert.equal(body.alertState, "budget");
    t.restore();
    // a failed read with nothing cached is a different reason and a different alert line
    const lobby2 = mk({ ...QUOTA_ENV, ADMIN_SECRET: "s3cret" });
    t = stubTurnUsage({ usage: () => new Response("no", { status: 500 }) });
    [status, body] = await r(lobby2, req("/admin/relay", auth));
    assert.equal(body.allow, false);
    assert.equal(body.why, "usage read failed, nothing cached");
    assert.equal(body.usedBytes, undefined);
    assert.equal(body.alertState, "usage-unreadable");
    assert.ok(lines.includes("relay usage unreadable: HTTP 500"), lines.join("\n"));
  } finally {
    console.log = log;
    t.restore();
  }
});

// --- the relay alert (backlog 40): one line and one webhook POST per state change ---

const WEBHOOK_URL = "https://discord.example/webhooks/1/abc";

// Routes the three outbound calls the Worker makes: the usage read (GraphQL), the mint
// (generate-ice-servers) and the alert webhook.
function stubTurnUsageWebhook({ usage = () => usageOk(0), mint = () => cfOk([STUN_ENTRY, TURN_ENTRY]), webhook = () => new Response("ok", { status: 204 }) } = {}) {
  const calls = { usage: [], mint: [], webhook: [] };
  const restore = stubFetch(async (url, init) => {
    const u = String(url);
    if (u.startsWith(USAGE_URL)) {
      calls.usage.push([u, init]);
      return usage(u, init);
    }
    if (u.includes("generate-ice-servers")) {
      calls.mint.push([u, init]);
      return mint(u, init);
    }
    if (u.includes("/webhooks/")) {
      calls.webhook.push([u, init]);
      return webhook(u, init);
    }
    throw new Error("unexpected fetch: " + u);
  });
  return { restore, calls };
}

test("relay alert: the budget transition logs and posts once across many refused mints", async () => {
  const lobby = mk({ ...QUOTA_ENV, RELAY_ALERT_WEBHOOK: WEBHOOK_URL }); // budget 100 GB
  const lines = [];
  const log = console.log;
  console.log = (m) => lines.push(String(m));
  const { restore, calls } = stubTurnUsageWebhook({ usage: () => usageOk(101e9) });
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    let status;
    for (let i = 0; i < 3; i++) {
      [status] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
      assert.equal(status, 503, `mint ${i + 1} refused`);
    }
    const alertLines = lines.filter((l) => l.startsWith("relay budget reached"));
    assert.equal(alertLines.length, 1, "one line for the run of refusals: " + lines.join("\n"));
    assert.equal(alertLines[0], "relay budget reached: 101.00 GB of 100.00 GB used in " + lobby.monthKey(Date.now()));
    assert.equal(calls.webhook.length, 1, "one webhook POST for the run of refusals");
    assert.equal(calls.webhook[0][0], WEBHOOK_URL);
    assert.deepEqual(JSON.parse(calls.webhook[0][1].body), { content: alertLines[0] });
    // the message carries no token, no IP and no id
    const sent = JSON.stringify(JSON.parse(calls.webhook[0][1].body));
    assert.ok(!sent.includes("d".repeat(64)) && !sent.includes("93.184") && !sent.includes("keyid") && !sent.includes("usagetoken"), sent);
  } finally {
    console.log = log;
    restore();
  }
});

test("relay alert: recovery to an under-budget month logs and posts once", async () => {
  const lobby = mk({ ...QUOTA_ENV, RELAY_ALERT_WEBHOOK: WEBHOOK_URL });
  const lines = [];
  const log = console.log;
  console.log = (m) => lines.push(String(m));
  let t = stubTurnUsageWebhook({ usage: () => usageOk(101e9) });
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 503);
    assert.equal(lines.filter((l) => l.startsWith("relay budget reached")).length, 1);
  } finally {
    t.restore();
  }
  // a new month with a fresh, under-budget read: the state changes back
  setCache(lobby, { bytes: 1e9, at: Date.now() - 10 * 60 * 1000, month: "1999-01" });
  t = stubTurnUsageWebhook({ usage: () => usageOk(1e9) });
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    let [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 200, "the new month's mint is allowed");
    assert.ok(body.username);
    const rec = lines.filter((l) => l.startsWith("relay recovered"));
    assert.equal(rec.length, 1, lines.join("\n"));
    assert.equal(rec[0], "relay recovered: mints allowed again");
    assert.equal(t.calls.webhook.length, 1);
    assert.deepEqual(JSON.parse(t.calls.webhook[0][1].body), { content: "relay recovered: mints allowed again" });
    assert.equal(lobby.alertState, "ok");
    // a second under-budget mint does not log or post again
    const nonce2 = "f".repeat(32);
    await r(lobby, req("/v1/connect", { method: "POST", ip: "93.184.100.9", body: { id: hid, endpoint: "93.184.100.9:41000", nonce: nonce2, mac: "02:46:43:aa:bb:cc", token: "d".repeat(64) } }));
    [status] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce: nonce2, token: "d".repeat(64) } }));
    assert.equal(status, 200);
    assert.equal(lines.filter((l) => l.startsWith("relay recovered")).length, 1);
    assert.equal(t.calls.webhook.length, 1);
  } finally {
    console.log = log;
    t.restore();
  }
});

test("relay alert: a webhook that fails (throws, 500) does not change the mint's answer", async () => {
  // the webhook fetch throws: the mint is still refused 503 no relay
  let lobby = mk({ ...QUOTA_ENV, RELAY_ALERT_WEBHOOK: WEBHOOK_URL });
  const lines = [];
  const log = console.log;
  console.log = (m) => lines.push(String(m));
  let t = stubTurnUsageWebhook({ usage: () => usageOk(101e9), webhook: () => { throw new Error("webhook down"); } });
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 503);
    assert.deepEqual(body, { error: "no relay" });
    assert.equal(lines.filter((l) => l.startsWith("relay budget reached")).length, 1);
  } finally {
    t.restore();
  }
  // the webhook answers 500: the mint is refused the same way
  lobby = mk({ ...QUOTA_ENV, RELAY_ALERT_WEBHOOK: WEBHOOK_URL });
  t = stubTurnUsageWebhook({ usage: () => usageOk(101e9), webhook: () => new Response("err", { status: 500 }) });
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status, body] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 503);
    assert.deepEqual(body, { error: "no relay" });
  } finally {
    console.log = log;
    t.restore();
  }
});

test("relay alert: no webhook set sends nothing (and a bad URL sends nothing)", async () => {
  // no RELAY_ALERT_WEBHOOK: a budget transition logs but fetch is never called for a webhook
  const lobby = mk(QUOTA_ENV);
  const { restore, calls } = stubTurnUsageWebhook({ usage: () => usageOk(101e9) });
  try {
    const { hid, nonce } = await hostAndConn(lobby, {});
    const [status] = await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 503);
    assert.equal(calls.webhook.length, 0);
    assert.equal(lobby.alertState, "budget");
  } finally {
    restore();
  }
  // a non-https URL is not sent (the secret is meant to be an https URL)
  const lobby2 = mk({ ...QUOTA_ENV, RELAY_ALERT_WEBHOOK: "http://insecure.example/hook" });
  const t2 = stubTurnUsageWebhook({ usage: () => usageOk(101e9) });
  try {
    const { hid, nonce } = await hostAndConn(lobby2, {});
    const [status] = await r(lobby2, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
    assert.equal(status, 503);
    assert.equal(t2.calls.webhook.length, 0);
  } finally {
    t2.restore();
  }
});

test("relay alert: at most 6 webhook POSTs an hour, across state changes", async () => {
  const lobby = mk({ ...QUOTA_ENV, RELAY_ALERT_WEBHOOK: WEBHOOK_URL });
  let usageBytes = 101e9; // the stubbed month's egress, flipped per phase
  const t = stubTurnUsageWebhook({ usage: () => usageOk(usageBytes) });
  // every mint is a fresh guest from a fresh address (the mint caps are per guest and per address)
  let n = 0;
  try {
    const { hid } = await hostAndConn(lobby, {});
    const mint = async (expect) => {
      n++;
      const gt = ("d" + (n % 10)).repeat(32);
      const ip = "93.184.100." + (20 + n);
      const nonceN = ("e" + (n % 10)).repeat(16);
      const [cs] = await r(lobby, req("/v1/connect", {
        method: "POST", ip,
        body: { id: hid, endpoint: ip + ":41000", nonce: nonceN, mac: "02:46:43:aa:bb:cc", token: gt },
      }));
      assert.equal(cs, 200);
      const [status] = await r(lobby, req("/v1/turn", { method: "POST", ip, body: { id: hid, nonce: nonceN, token: gt } }));
      assert.equal(status, expect);
    };
    // six state changes (budget -> ok three times): six POSTs
    for (let i = 0; i < 3; i++) {
      usageBytes = 101e9;
      setCache(lobby, { bytes: 101e9, at: Date.now() - 10 * 60 * 1000 }); // forces the fresh over-budget read
      await mint(503);
      usageBytes = 1e9;
      setCache(lobby, { bytes: 1e9, at: Date.now() - 10 * 60 * 1000, month: "1999-01" }); // the read replaces it
      await mint(200, `recovery ${i + 1}`);
    }
    assert.equal(t.calls.webhook.length, 6);
    // a seventh change in the same hour is logged but not posted
    usageBytes = 101e9;
    setCache(lobby, { bytes: 101e9, at: Date.now() - 10 * 60 * 1000 });
    await mint(503);
    assert.equal(lobby.alertState, "budget");
    assert.equal(t.calls.webhook.length, 6, "the 7th change of the hour is not posted");
    // once the hour has passed the cap lifts
    lobby.alertPosts = lobby.alertPosts.map((x) => x - 3600000 - 1000);
    usageBytes = 1e9;
    setCache(lobby, { bytes: 1e9, at: Date.now() - 10 * 60 * 1000, month: "1999-01" });
    await mint(200);
    assert.equal(t.calls.webhook.length, 7, "after the hour, the next change is posted again");
  } finally {
    t.restore();
  }
});

test("operator metrics aggregate fixed routes, delivery and refusals without player details", async () => {
  const lobby = mk({ ADMIN_SECRET: "s3cret" });
  const path = "/admin/metrics";
  assert.equal((await r(lobby, req(path)))[0], 401);
  const { hToken, hid, nonce } = await hostAndConn(lobby);
  await r(lobby, req("/v1/hosts?title=instincts&version=1"));
  await r(lobby, req("/v1/connects?id=" + hid + "&token=" + hToken));
  await r(lobby, req("/v1/turn", { method: "POST", body: { id: hid, nonce, token: "d".repeat(64) } }));
  await r(lobby, req("/unrecognized-player-path"));
  const [status, data] = await r(lobby, req(path, { headers: { authorization: "Bearer s3cret" } }));
  assert.equal(status, 200);
  assert.equal(data.active["instincts/2"], 1);
  assert.equal(data.connects.directQueued, 1);
  assert.equal(data.connects.directDelivered, 1);
  assert.equal(data.relay.unconfigured, 1);
  assert.equal(data.routes["GET /v1/hosts"].status[200], 1);
  assert.equal(data.routes["POST /v1/turn"].status[503], 1);
  assert.ok(data.routes["GET /v1/hosts"].totalMs >= 0);
  assert.ok(data.startedAt <= data.sampledAt);
  const serialized = JSON.stringify(data);
  for (const value of [hToken, hid, nonce, "s3cret", "8.8.8.7", "unrecognized-player-path", "version="]) {
    assert.equal(serialized.includes(value), false);
  }
});

test("beta probe reports require opt-in and consume a private, single-use receipt", async () => {
  const lobby = mk({ BETA_REPORTS: "1", ADMIN_SECRET: "test-only" });
  const [, { id }] = await r(lobby, reg());
  const optIn = conn(id, "93.184.100.9", "d".repeat(64));
  const body = await optIn.json();
  const [status, reply] = await r(lobby, req("/v1/connect", {
    method: "POST", ip: "93.184.100.9", body: { ...body, reportBuild: "beta-1" },
  }));
  assert.equal(status, 200);
  assert.match(reply.reportReceipt, /^[0-9a-f]{32}$/);
  const report = (extra = {}) => req("/v1/probe-report", {
    method: "POST", body: { receipt: reply.reportReceipt, path: "direct", stage: "first_frame", ms: 125, ...extra },
  });
  assert.equal((await r(lobby, report({ stage: "unexpected" })))[0], 400);
  assert.equal((await r(lobby, report()))[0], 204);
  assert.equal((await r(lobby, report()))[0], 404);
  const [, metrics] = await r(lobby, req("/admin/metrics", { headers: { authorization: "Bearer test-only" } }));
  assert.deepEqual(metrics.probes, {
    issued: 1, received: 1, results: { "instincts/direct/first_frame": 1 }, firstFrameMs: 125, builds: { "beta-1": 1 },
  });
  assert.equal(JSON.stringify(metrics).includes(reply.reportReceipt), false);
  assert.equal((await r(mk(), report()))[0], 404);
  const [, ordinary] = await r(lobby, conn(id, "93.184.100.10", "f".repeat(64)));
  assert.equal(ordinary.reportReceipt, undefined);
});

// --- acknowledged delivery (backlog 51): a poll with ack keeps connects until the host has them ---

test("ack: a delivered connect stays until acknowledged, so a poll lost on the way loses nothing", async () => {
  const lobby = mk({ TTL_MS: 5000 });
  const hToken = "c".repeat(64);
  const [, regBody] = await r(lobby, reg({ token: hToken }));
  const hid = regBody.id;
  assert.equal((await r(lobby, conn(hid, "93.184.100.9", "d".repeat(64))))[0], 200);

  // delivered with its seq; the host never saw it (the poll died): the next ack=0 poll repeats it
  let [status, body] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1&ack=0`));
  assert.equal(status, 200);
  assert.equal(body.connects.length, 1);
  assert.equal(body.connects[0].seq, 1);
  [status, body] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1&ack=0`));
  assert.deepEqual(body.connects.map((c) => c.seq), [1]);

  // acknowledged: gone, and the next poll waits and returns empty
  [status, body] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1&ack=1`));
  assert.equal(status, 200);
  assert.deepEqual(body.connects, []);
  // the operator counts it as delivered once
  assert.equal(lobby.metrics.connects.directDelivered, 1);
});

test("ack: a held poll answered by a connect keeps it; later connects get higher seqs", async () => {
  const lobby = mk({ TTL_MS: 5000 });
  const hToken = "c".repeat(64);
  const [, regBody] = await r(lobby, reg({ token: hToken }));
  const hid = regBody.id;
  const held = r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=10&ack=0`));
  await new Promise((res) => setTimeout(res, 50));
  assert.equal((await r(lobby, conn(hid, "93.184.100.9", "d".repeat(64))))[0], 200);
  const [, first] = await held;
  assert.deepEqual(first.connects.map((c) => c.seq), [1]);
  assert.equal((await r(lobby, conn(hid, "93.184.100.10", "f".repeat(64))))[0], 200);
  // the first poll's reply was lost: both come back until acknowledged
  const [, both] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1&ack=0`));
  assert.deepEqual(both.connects.map((c) => c.seq), [1, 2]);
  const [, second] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1&ack=1`));
  assert.deepEqual(second.connects.map((c) => c.seq), [2]);
});

test("ack: a poll without ack is unchanged (delivered once, no seq field)", async () => {
  const lobby = mk({ TTL_MS: 5000 });
  const hToken = "c".repeat(64);
  const [, regBody] = await r(lobby, reg({ token: hToken }));
  const hid = regBody.id;
  assert.equal((await r(lobby, conn(hid, "93.184.100.9", "d".repeat(64))))[0], 200);
  let [, body] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1`));
  assert.equal(body.connects.length, 1);
  assert.equal("seq" in body.connects[0], false);
  [, body] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1`));
  assert.deepEqual(body.connects, []);
});

test("ack: a malformed ack is treated as no ack", async () => {
  const lobby = mk({ TTL_MS: 5000 });
  const hToken = "c".repeat(64);
  const [, regBody] = await r(lobby, reg({ token: hToken }));
  const hid = regBody.id;
  assert.equal((await r(lobby, conn(hid, "93.184.100.9", "d".repeat(64))))[0], 200);
  let [status, body] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1&ack=-1`));
  assert.equal(status, 200);
  assert.equal("seq" in body.connects[0], false);
  [, body] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1&ack=0`));
  assert.deepEqual(body.connects, []); // the malformed poll delivered it the old way
});

test("ack: register reports the connect counter, and a re-register never reuses a seq", async () => {
  const lobby = mk({ TTL_MS: 5000 });
  const hToken = "c".repeat(64);
  let [, regBody] = await r(lobby, reg({ token: hToken }));
  assert.equal(regBody.seq, 0);
  const hid = regBody.id;
  assert.equal((await r(lobby, conn(hid, "93.184.100.9", "d".repeat(64))))[0], 200);
  [, regBody] = await r(lobby, reg({ token: hToken }));
  assert.equal(regBody.seq, 1);
  assert.equal((await r(lobby, conn(hid, "93.184.100.10", "f".repeat(64))))[0], 200);
  const [, body] = await r(lobby, req(`/v1/connects?id=${hid}&token=${hToken}&wait=1&ack=1`));
  assert.deepEqual(body.connects.map((c) => c.seq), [2]);
});

// ---- versions and updates (research/docs/online-architecture.md 7a) ----

test("builds: numbers, then a release above its pre-releases, then label and number", () => {
  const order = ["", "garbage", "1.0", "0.2.4", "0.3.0-alpha.3", "0.3.0-beta.1", "0.3.0-beta.2", "0.3.0-beta.10",
    "0.3.0-rc.1", "0.3.0", "0.3.1-beta.1", "0.10.0"];
  for (let i = 0; i < order.length; i++)
    for (let j = 0; j < order.length; j++) {
      const want = i < 3 && j < 3 ? 0 : Math.sign(i - j); // the three unparsable strings are equal
      assert.equal(compareBuilds(order[i], order[j]), want, `${order[i]} vs ${order[j]}`);
    }
  assert.equal(compareBuilds("v0.3.0-beta.2", "0.3.0-beta2"), 0, "a leading v and a missing dot are the same build");
  assert.equal(compareBuilds("0.3.0+abc123", "0.3.0"), 0, "build metadata is ignored");
  assert.equal(compareBuilds(undefined, "0.3.0"), -1);
});

const withBuild = (build, extra = {}) => ({ ...extra, headers: { ...(extra.headers || {}), ...(build ? { "x-fci-build": build } : {}) } });

test("MIN_BUILD: a build below it, or none, is told to update on every player route", async () => {
  const lobby = mk({ ...env, MIN_BUILD: "0.3.0-beta.2" });
  for (const build of [null, "0.3.0-beta.1", "nonsense"]) {
    for (const request of [
      req("/v1/hosts?title=instincts&version=1", withBuild(build)),
      req("/v1/register", withBuild(build, { method: "POST", body: {} })),
      req("/v1/connects?id=" + "a".repeat(32), withBuild(build)),
      req("/v1/connect", withBuild(build, { method: "POST", body: {} })),
      req("/v1/turn", withBuild(build, { method: "POST", body: {} })),
    ]) {
      const [status, body] = await r(lobby, request);
      assert.equal(status, 426, `${build} ${request.url}`);
      assert.deepEqual(body, { error: "update", minBuild: "0.3.0-beta.2" });
    }
  }
  for (const build of ["0.3.0-beta.2", "0.3.0-beta.3", "0.3.0"]) {
    const [status] = await r(lobby, req("/v1/hosts?title=instincts&version=1", withBuild(build)));
    assert.equal(status, 200, build);
  }
});

test("MIN_BUILD: unregister is never refused, and admin routes are not gated", async () => {
  const lobby = mk({ ...env, MIN_BUILD: "9.9.9", MAINTENANCE: "1", ADMIN_SECRET: "s".repeat(32) });
  const [status] = await r(lobby, req("/v1/unregister", { method: "POST", body: {} }));
  assert.notEqual(status, 426);
  assert.notEqual(status, 503);
  const [admin] = await r(lobby, req("/admin/hosts", { headers: { authorization: "Bearer " + "s".repeat(32) } }));
  assert.equal(admin, 200);
});

test("MIN_BUILD: unset or not a version refuses nobody", async () => {
  for (const MIN_BUILD of [undefined, "", "latest"]) {
    const lobby = mk({ ...env, MIN_BUILD });
    const [status] = await r(lobby, req("/v1/hosts?title=instincts&version=1"));
    assert.equal(status, 200, String(MIN_BUILD));
  }
});

test("MAINTENANCE: every gated route answers 503 maintenance, whatever the build", async () => {
  const lobby = mk({ ...env, MAINTENANCE: "back at 20:00 UTC" });
  for (const request of [
    req("/v1/hosts?title=instincts&version=1", withBuild("0.3.0-beta.2")),
    req("/v1/register", withBuild("0.3.0-beta.2", { method: "POST", body: {} })),
    req("/v1/connect", { method: "POST", body: {} }),
  ]) {
    const [status, body] = await r(lobby, request);
    assert.equal(status, 503);
    assert.deepEqual(body, { error: "maintenance" });
  }
});
