"""The fake host list (tools/fake_hostlist.py): register → poll held → connect releases it.

In-process on 127.0.0.1: the server runs in a thread, the test makes HTTP requests with urllib.
The protocol matches lobby.js: the client brings its own identity token (the id is
sha256(token)[:16], hex), the listing is ids only, the connect replies with the host's endpoint
and MAC, a timed-out poll is a 200 with an empty list, and the admin routes are gated by a secret.
"""

from http.server import ThreadingHTTPServer
import json
import secrets
import threading
import time
import urllib.error
import urllib.request

# tools/ is on the path (conftest); a `tools.` import hides the toolkit's
from fake_hostlist import DEFAULT_TURN_URL, Lobby, derive_id, make_handler


def start_server(break_punch=False, secret=None, turn_user=None, turn_pass=None, turn_url=None):
    """Start the fake host list on a random port; return (port, server, lobby)."""
    lobby = Lobby(
        break_punch=break_punch,
        secret=secret,
        turn_user=turn_user,
        turn_pass=turn_pass,
        turn_url=turn_url or DEFAULT_TURN_URL,
    )
    server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(lobby))
    server.daemon_threads = True
    port = server.server_address[1]
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    return port, server, lobby


def http_json(url, method="GET", body=None, headers=None):
    """Make an HTTP request; return (status, parsed_json_or_None)."""
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    if data:
        req.add_header("content-type", "application/json")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = resp.read()
            return resp.status, json.loads(raw) if raw else None
    except urllib.error.HTTPError as e:
        raw = e.read()
        return e.code, json.loads(raw) if raw else None


def connect(base, host_id, gtoken=None, endpoint="127.0.0.1:50000", nonce=None, relay=None):
    """POST a guest connect; return (status, body)."""
    body = {
        "id": host_id,
        "endpoint": endpoint,
        "nonce": nonce or "0123456789abcdef0123456789abcdef",
        "mac": "02:46:43:aa:bb:cc",
        "token": gtoken or secrets.token_hex(32),
    }
    if relay is not None:
        body["relay"] = relay
    return http_json(f"{base}/v1/connect", method="POST", body=body)


def mint(base, host_id, gtoken, nonce):
    """POST a TURN mint; return (status, body)."""
    return http_json(
        f"{base}/v1/turn",
        method="POST",
        body={"id": host_id, "nonce": nonce, "token": gtoken},
    )


def register(base, title="evolution", version="3", endpoint="127.0.0.1:40000", token=None):
    token = token or secrets.token_hex(32)
    status, body = http_json(
        f"{base}/v1/register",
        method="POST",
        body={
            "proto": 2,
            "title": title,
            "version": version,
            "mac": "02:46:43:12:34:56",
            "endpoint": endpoint,
            "token": token,
        },
    )
    assert status == 200, body
    return body["id"], token


def test_register_poll_connect():
    """register → poll held → connect releases it with the connect data; the reply names the host."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"
        host_id, host_token = register(base)
        assert host_id == derive_id(host_token)
        assert len(host_id) == 32  # 16 bytes of sha256, hex

        # list: the host is there, as an id only
        status, body = http_json(f"{base}/v1/hosts?title=evolution&version=3")
        assert status == 200
        assert body["hosts"] == [{"id": host_id}]  # no endpoint, no MAC, no token

        # start a long-poll in a thread (it will be held)
        result = []

        def do_poll():
            s, b = http_json(f"{base}/v1/connects?id={host_id}&token={host_token}&wait=10")
            result.append((s, b))

        t = threading.Thread(target=do_poll, daemon=True)
        t.start()
        time.sleep(0.2)  # the poll is now held

        # connect: the reply carries the host's endpoint and MAC
        gtoken = secrets.token_hex(32)
        nonce = "0123456789abcdef0123456789abcdef"
        status, body = http_json(
            f"{base}/v1/connect",
            method="POST",
            body={
                "id": host_id,
                "endpoint": "127.0.0.1:50000",
                "nonce": nonce,
                "mac": "02:46:43:aa:bb:cc",
                "token": gtoken,
            },
        )
        assert status == 200, body
        assert body == {"endpoint": "127.0.0.1:40000", "mac": "02:46:43:12:34:56"}

        # the poll is released with the connect (the guest's id is not in it)
        t.join(timeout=5)
        assert len(result) == 1, "the poll was not released"
        status, body = result[0]
        assert status == 200
        assert body["connects"] == [{"endpoint": "127.0.0.1:50000", "nonce": nonce, "mac": "02:46:43:aa:bb:cc"}]
    finally:
        server.shutdown()


def test_register_wrong_proto_is_refused_and_hidden():
    """A registration with the old net protocol (1) is refused; a proto-1 host in the table is
    not listed to a proto-2 listing. The list hides a mismatch (the one rule, q21)."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"
        status, body = http_json(
            f"{base}/v1/register",
            method="POST",
            body={
                "proto": 1,
                "title": "evolution",
                "version": "3",
                "mac": "02:46:43:12:34:56",
                "endpoint": "127.0.0.1:40002",
                "token": secrets.token_hex(32),
            },
        )
        assert status == 400, body

        # and a proto-1 host already in the table is not listed to a proto-2 listing
        host_id, _ = register(base)
        lobby.hosts["deadbeef" * 4] = {
            "id": "deadbeef" * 4,
            "ip": "10.0.0.9",
            "proto": 1,
            "title": "evolution",
            "version": "3",
            "mac": "02:46:43:00:11:22",
            "endpoint": "1.2.3.4:3074",
            "local": None,
            "expires": time.time() + 60,
        }
        status, body = http_json(f"{base}/v1/hosts?title=evolution&version=3")
        assert status == 200
        assert body["proto"] == 2
        assert body["hosts"] == [{"id": host_id}]  # the proto-1 host is hidden
    finally:
        server.shutdown()


def test_poll_times_out():
    """A poll with no connect times out with a 200 and an empty list."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"
        host_id, host_token = register(base, title="instincts", version="1", endpoint="127.0.0.1:40001")
        start = time.time()
        status, body = http_json(f"{base}/v1/connects?id={host_id}&token={host_token}&wait=1")
        elapsed = time.time() - start
        assert status == 200
        assert body == {"connects": []}
        assert elapsed >= 0.9, f"wait=1 should hold for a second, got {elapsed:.2f}s"
    finally:
        server.shutdown()


def test_poll_wrong_token_is_404():
    """A poll whose token does not derive to the id is 404, like an unknown id."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"
        host_id, _ = register(base)
        status, _ = http_json(f"{base}/v1/connects?id={host_id}&token={secrets.token_hex(32)}&wait=1")
        assert status == 404
    finally:
        server.shutdown()


def test_connect_unknown_host_is_404():
    """A connect to an unknown host is 404."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"
        status, _ = http_json(
            f"{base}/v1/connect",
            method="POST",
            body={
                "id": "f" * 32,
                "endpoint": "127.0.0.1:50000",
                "nonce": "0123456789abcdef0123456789abcdef",
                "mac": "02:46:43:aa:bb:cc",
                "token": secrets.token_hex(32),
            },
        )
        assert status == 404
    finally:
        server.shutdown()


def test_register_bad_mac_is_rejected():
    """A MAC without the cards' prefix (or in uppercase) is rejected."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"
        for mac in ("02:46:43:AB:CD:EF", "aa:bb:cc:dd:ee:ff"):
            status, body = http_json(
                f"{base}/v1/register",
                method="POST",
                body={
                    "proto": 2,
                    "title": "evolution",
                    "version": "3",
                    "mac": mac,
                    "endpoint": "127.0.0.1:40000",
                    "token": secrets.token_hex(32),
                },
            )
            assert status == 400, body
    finally:
        server.shutdown()


def test_register_global_host_cap():
    """The global live-host cap (HOSTS_MAX): a flood of fake games from many addresses cannot grow
    the list without bound; a re-register of a live host never grows the map, so it is not capped."""
    import fake_hostlist

    port, server, lobby = start_server()
    old = fake_hostlist.HOSTS_MAX
    fake_hostlist.HOSTS_MAX = 3
    try:
        base = f"http://127.0.0.1:{port}"
        first_tok = None
        for i in range(3):
            _, tok = register(base, version=str(i + 1), endpoint=f"127.0.0.1:{40000 + i}")
            first_tok = first_tok or tok
        # a fourth, distinct host is refused
        status, body = http_json(
            f"{base}/v1/register",
            method="POST",
            body={
                "proto": 2,
                "title": "evolution",
                "version": "9",
                "mac": "02:46:43:12:34:56",
                "endpoint": "127.0.0.1:40003",
                "token": secrets.token_hex(32),
            },
        )
        assert status == 503, body
        assert body["error"] == "host list full"
        # a re-register of a live host (the same token derives the same id) still succeeds:
        # the map does not grow
        status, body = http_json(
            f"{base}/v1/register",
            method="POST",
            body={
                "proto": 2,
                "title": "evolution",
                "version": "1",
                "mac": "02:46:43:12:34:56",
                "endpoint": "127.0.0.1:40000",
                "token": tok,
            },
        )
        assert status == 200, body
    finally:
        fake_hostlist.HOSTS_MAX = old
        server.shutdown()


def test_ban_flow_and_admin_gate():
    """A banned id cannot host or join; the admin routes need the secret."""
    port, server, lobby = start_server(secret="topsecret")
    try:
        base = f"http://127.0.0.1:{port}"
        host_id, host_token = register(base)
        auth = {"authorization": "Bearer topsecret"}

        # the admin gate
        status, _ = http_json(f"{base}/admin/hosts")
        assert status == 401
        status, _ = http_json(f"{base}/admin/hosts", headers={"authorization": "Bearer wrong"})
        assert status == 401
        status, body = http_json(f"{base}/admin/hosts", headers=auth)
        assert status == 200
        assert body["hosts"][0]["id"] == host_id

        # ban the host's id
        status, body = http_json(
            f"{base}/admin/bans", method="POST", body={"id": host_id, "reason": "test"}, headers=auth
        )
        assert status == 200, body
        status, body = http_json(f"{base}/admin/bans", headers=auth)
        assert status == 200
        assert body["bans"][0]["id"] == host_id

        # the banned host is gone from the list and cannot re-register
        status, body = http_json(f"{base}/v1/hosts?title=evolution&version=3")
        assert body["hosts"] == []
        status, body = http_json(
            f"{base}/v1/register",
            method="POST",
            body={
                "proto": 2,
                "title": "evolution",
                "version": "3",
                "mac": "02:46:43:12:34:56",
                "endpoint": "127.0.0.1:40000",
                "token": host_token,
            },
        )
        assert status == 403, body
        assert body["error"] == "banned"

        # a banned guest cannot join
        host_id2, _ = register(base, endpoint="127.0.0.1:40002")
        banned_guest = secrets.token_hex(32)
        http_json(f"{base}/admin/bans", method="POST", body={"id": derive_id(banned_guest)}, headers=auth)
        status, body = http_json(
            f"{base}/v1/connect",
            method="POST",
            body={
                "id": host_id2,
                "endpoint": "127.0.0.1:50000",
                "nonce": "0123456789abcdef0123456789abcdef",
                "mac": "02:46:43:aa:bb:cc",
                "token": banned_guest,
            },
        )
        assert status == 403, body

        # unban
        status, body = http_json(f"{base}/admin/bans?id={host_id}", method="DELETE", headers=auth)
        assert status == 200
        assert body == {"removed": True}
        status, body = http_json(
            f"{base}/v1/register",
            method="POST",
            body={
                "proto": 2,
                "title": "evolution",
                "version": "3",
                "mac": "02:46:43:12:34:56",
                "endpoint": "127.0.0.1:40000",
                "token": host_token,
            },
        )
        assert status == 200, body

        # the abuse counters saw the refusals
        status, body = http_json(f"{base}/admin/abuse", headers=auth)
        assert status == 200
        assert body["ring"], "the ring should have refusals"
        assert any(e["kind"] == "banned" for e in body["ring"])
    finally:
        server.shutdown()


def test_break_punch_rewrites_connect_endpoint():
    """With --break-punch, the connect the host receives has the port + 1000 (a dead port)."""
    port, server, lobby = start_server(break_punch=True)
    try:
        base = f"http://127.0.0.1:{port}"
        host_id, host_token = register(base)

        result = []

        def do_poll():
            s, b = http_json(f"{base}/v1/connects?id={host_id}&token={host_token}&wait=10")
            result.append((s, b))

        t = threading.Thread(target=do_poll, daemon=True)
        t.start()
        time.sleep(0.2)  # the poll is now held

        nonce = "fedcba9876543210fedcba9876543210"
        status, _ = http_json(
            f"{base}/v1/connect",
            method="POST",
            body={
                "id": host_id,
                "endpoint": "127.0.0.1:50000",
                "nonce": nonce,
                "mac": "02:46:43:aa:bb:cc",
                "token": secrets.token_hex(32),
            },
        )
        assert status == 200

        t.join(timeout=5)
        assert len(result) == 1, "the poll was not released"
        status, body = result[0]
        assert status == 200
        assert len(body["connects"]) == 1
        # the port is shifted +1000: the host punches a dead port
        assert body["connects"][0]["endpoint"] == "127.0.0.1:51000", body["connects"][0]
        # the nonce and MAC are untouched (the relay room is named by the nonce)
        assert body["connects"][0]["nonce"] == nonce
        assert body["connects"][0]["mac"] == "02:46:43:aa:bb:cc"
    finally:
        server.shutdown()


def test_break_punch_leaves_the_relay_connect_untouched():
    """With --break-punch, a direct connect is rewritten (port + 1000) but the relay connect — the
    guest's real TURN address — is forwarded untouched: the host punches it for real."""
    port, server, lobby = start_server(break_punch=True, turn_user="user", turn_pass="pass")
    try:
        base = f"http://127.0.0.1:{port}"
        host_id, host_token = register(base)
        gtoken = secrets.token_hex(32)
        nonce = "0123456789abcdef0123456789abcdef"

        # a direct connect is rewritten (port + 1000): the host punches a dead port
        status, body = connect(base, host_id, gtoken, nonce=nonce, endpoint="127.0.0.1:50000")
        assert status == 200, body

        status, body = mint(base, host_id, gtoken, nonce)
        assert status == 200, body

        # the relay connect carries the guest's real TURN address: forwarded untouched
        status, body = connect(base, host_id, gtoken, endpoint="127.0.0.1:3478", nonce=nonce, relay=True)
        assert status == 200, body

        # the host's poll shows the relay address untouched (NOT 127.0.0.1:3478 + 1000)
        status, body = http_json(f"{base}/v1/connects?id={host_id}&token={host_token}&wait=1")
        assert status == 200
        relayed = [c for c in body["connects"] if c.get("relay")]
        assert len(relayed) == 1, body["connects"]
        assert relayed[0]["endpoint"] == "127.0.0.1:3478", relayed[0]
    finally:
        server.shutdown()


def test_connect_bad_mac_is_rejected():
    """A connect with a MAC without the cards' prefix is rejected."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"
        host_id, _ = register(base)
        status, body = http_json(
            f"{base}/v1/connect",
            method="POST",
            body={
                "id": host_id,
                "endpoint": "127.0.0.1:50000",
                "nonce": "0123456789abcdef0123456789abcdef",
                "mac": "02:46:43:AA:BB:CC",  # uppercase: rejected
                "token": secrets.token_hex(32),
            },
        )
        assert status == 400, body
    finally:
        server.shutdown()


def test_turn_unconfigured_is_503_no_relay():
    """Without --turn-user / --turn-pass, /v1/turn is 503 no relay (like unset Worker secrets)."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"
        host_id, _ = register(base)
        gtoken = secrets.token_hex(32)
        nonce = "0123456789abcdef0123456789abcdef"
        status, body = connect(base, host_id, gtoken, nonce=nonce)
        assert status == 200, body
        status, body = mint(base, host_id, gtoken, nonce)
        assert status == 503, body
        assert body == {"error": "no relay"}
    finally:
        server.shutdown()


def test_turn_requires_a_recent_connect():
    """A mint without a recent connect from this guest to this host is 403."""
    port, server, lobby = start_server(turn_user="user", turn_pass="pass")
    try:
        base = f"http://127.0.0.1:{port}"
        host_id, _ = register(base)
        gtoken = secrets.token_hex(32)
        nonce = "0123456789abcdef0123456789abcdef"
        status, body = mint(base, host_id, gtoken, nonce)
        assert status == 403, body
        assert body["error"] == "no connect"
        # a connect to another host does not open this one
        host_id2, _ = register(base, endpoint="127.0.0.1:40001")
        connect(base, host_id2, gtoken, nonce=nonce)
        status, body = mint(base, host_id, gtoken, nonce)
        assert status == 403, body
    finally:
        server.shutdown()


def test_turn_after_connect_and_relay_flow():
    """connect → mint → relay connect: the configured credential is returned, and the host's
    poll shows the relayed address with relay: true."""
    port, server, lobby = start_server(turn_user="user", turn_pass="pass")
    try:
        base = f"http://127.0.0.1:{port}"
        host_id, host_token = register(base)
        gtoken = secrets.token_hex(32)
        nonce = "0123456789abcdef0123456789abcdef"

        status, body = connect(base, host_id, gtoken, nonce=nonce)
        assert status == 200, body

        status, body = mint(base, host_id, gtoken, nonce)
        assert status == 200, body
        assert body == {
            "username": "user",
            "credential": "pass",
            "urls": ["turn:127.0.0.1:3478?transport=udp"],  # the default --turn-url
        }

        # the relay connect is accepted after the mint
        status, body = connect(base, host_id, gtoken, endpoint="127.0.0.1:3478", nonce=nonce, relay=True)
        assert status == 200, body

        # the host's poll shows the relayed address with relay: true
        status, body = http_json(f"{base}/v1/connects?id={host_id}&token={host_token}&wait=1")
        assert status == 200
        assert body["connects"] == [
            {"endpoint": "127.0.0.1:3478", "nonce": nonce, "mac": "02:46:43:aa:bb:cc", "relay": True}
        ]
    finally:
        server.shutdown()


def test_relay_requires_a_mint_and_is_capped_per_nonce():
    """A relay connect without a mint is 403; after a mint, at most 3 per (gid, nonce); the
    mint rate limit is 6 per guest in 10 minutes."""
    port, server, lobby = start_server(turn_user="user", turn_pass="pass")
    try:
        base = f"http://127.0.0.1:{port}"
        host_id, _ = register(base)
        gtoken = secrets.token_hex(32)
        nonce = "0123456789abcdef0123456789abcdef"

        status, body = connect(base, host_id, gtoken, nonce=nonce)
        assert status == 200, body

        # no mint yet: the relay connect is refused
        status, body = connect(base, host_id, gtoken, endpoint="127.0.0.1:3478", nonce=nonce, relay=True)
        assert status == 403, body
        assert body["error"] == "no relay"

        status, body = mint(base, host_id, gtoken, nonce)
        assert status == 200, body

        for i in range(3):
            status, body = connect(base, host_id, gtoken, endpoint=f"127.0.0.1:347{i}", nonce=nonce, relay=True)
            assert status == 200, body
        status, body = connect(base, host_id, gtoken, endpoint="127.0.0.1:3499", nonce=nonce, relay=True)
        assert status == 429, body
        assert body["error"] == "too many relays"

        # the mint rate limit: 6 per guest in 10 minutes (one more mint for a fresh nonce is the 7th)
        status, _ = connect(base, host_id, gtoken, nonce="f" * 32)
        assert status == 200
        for i in range(5):
            status, _ = mint(base, host_id, gtoken, nonce)
            assert status == 200, f"mint {i + 2} should be accepted"
        status, body = mint(base, host_id, gtoken, nonce)
        assert status == 429, body
        assert body["error"] == "slow down"
    finally:
        server.shutdown()


def test_one_mint_covers_every_host_the_guest_connected_to():
    """One mint (a TURN allocation) covers every host the guest connected to directly: relayed
    connects to two hosts are both 200; a host the guest never connected to is 403; a gid with no
    mint is 403; a mint older than the window is 403; the 4th relay for one nonce is 429."""
    port, server, lobby = start_server(turn_user="user", turn_pass="pass")
    try:
        base = f"http://127.0.0.1:{port}"
        host_id, _ = register(base)
        host_id2, _ = register(base, endpoint="127.0.0.1:40002")
        gtoken = secrets.token_hex(32)
        nonce = "0123456789abcdef0123456789abcdef"

        status, body = connect(base, host_id, gtoken, nonce=nonce)
        assert status == 200, body
        status, body = connect(base, host_id2, gtoken, nonce=nonce)
        assert status == 200, body

        # no mint yet: the relayed connect is refused
        status, body = connect(base, host_id, gtoken, endpoint="127.0.0.1:3478", nonce=nonce, relay=True)
        assert status == 403, body
        assert body["error"] == "no relay"

        status, body = mint(base, host_id, gtoken, nonce)
        assert status == 200, body

        # relayed connects to both hosts: both accepted, no second mint
        status, body = connect(base, host_id, gtoken, endpoint="127.0.0.1:3471", nonce=nonce, relay=True)
        assert status == 200, body
        status, body = connect(base, host_id2, gtoken, endpoint="127.0.0.1:3472", nonce=nonce, relay=True)
        assert status == 200, body
        assert len(lobby.mints) == 1
        assert len(lobby.gid_mints) == 1

        # a host the guest never connected to: 403
        host_id3, _ = register(base, endpoint="127.0.0.1:40003")
        status, body = connect(base, host_id3, gtoken, endpoint="127.0.0.1:3473", nonce=nonce, relay=True)
        assert status == 403, body
        assert body["error"] == "no relay"

        # the cap is per (gid, nonce) across hosts: the 4th relayed connect for the nonce is 429
        status, body = connect(base, host_id, gtoken, endpoint="127.0.0.1:3474", nonce=nonce, relay=True)
        assert status == 200, body
        status, body = connect(base, host_id, gtoken, endpoint="127.0.0.1:3475", nonce=nonce, relay=True)
        assert status == 429, body
        assert body["error"] == "too many relays"

        # a mint older than the window: 403 (age every record past the 10-minute window)
        old = time.time() - 601
        for m in (lobby.mints, lobby.gid_mints, lobby.pairs, lobby.relay_counts):
            for v in m.values():
                v["at"] = old
        nonce2 = "f" * 32
        status, body = connect(base, host_id, gtoken, nonce=nonce2)
        assert status == 200, body  # fresh direct connect (a fresh pair, no mint)
        status, body = connect(base, host_id, gtoken, endpoint="127.0.0.1:3476", nonce=nonce2, relay=True)
        assert status == 403, body
        assert body["error"] == "no relay"
        status, body = mint(base, host_id, gtoken, nonce2)
        assert status == 200, body
        for m in (lobby.mints, lobby.gid_mints, lobby.pairs, lobby.relay_counts):
            for v in m.values():
                if v["at"] > old:
                    v["at"] = old
        status, body = connect(base, host_id, gtoken, endpoint="127.0.0.1:3477", nonce=nonce2, relay=True)
        assert status == 403, body
        assert body["error"] == "no relay"
    finally:
        server.shutdown()


def test_post_without_json_content_type_is_415():
    """POSTs need content-type application/json, as the deployed Worker requires (a runtime bug hid here)."""
    port, server, lobby = start_server()
    try:
        req = urllib.request.Request(f"http://127.0.0.1:{port}/v1/register", data=b"{}", method="POST")
        req.add_header("content-type", "application")
        try:
            urllib.request.urlopen(req, timeout=10)
            status = 200
        except urllib.error.HTTPError as e:
            status = e.code
        assert status == 415
    finally:
        server.shutdown()


def test_unregister_removes_the_listing():
    """A leaving host removes its own listing; a second unregister is a no-op; a bad token is 403."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"
        host_id, host_token = register(base)

        # the host is listed before the unregister
        status, body = http_json(f"{base}/v1/hosts?title=evolution&version=3")
        assert body["hosts"] == [{"id": host_id}]

        # unregister with the host's own token: 200, the listing is gone
        status, body = http_json(f"{base}/v1/unregister", method="POST", body={"id": host_id, "token": host_token})
        assert status == 200, body
        assert body == {"ok": True}
        status, body = http_json(f"{base}/v1/hosts?title=evolution&version=3")
        assert body["hosts"] == []

        # a second unregister is a no-op (idempotent): 200
        status, body = http_json(f"{base}/v1/unregister", method="POST", body={"id": host_id, "token": host_token})
        assert status == 200, body
        assert body == {"ok": True}

        # a token that does not derive to the id is 403 (not 404: a wrong token is not "already gone")
        status, body = http_json(
            f"{base}/v1/unregister", method="POST", body={"id": host_id, "token": secrets.token_hex(32)}
        )
        assert status == 403, body
        assert body["error"] == "bad token"

        # a malformed token is 403
        status, body = http_json(f"{base}/v1/unregister", method="POST", body={"id": host_id, "token": "xyz"})
        assert status == 403, body
        assert body["error"] == "bad token"

        # an id that was never registered (a valid token that derives to it) is a 200: idempotent
        other_token = secrets.token_hex(32)
        status, body = http_json(
            f"{base}/v1/unregister", method="POST", body={"id": derive_id(other_token), "token": other_token}
        )
        assert status == 200, body
        assert body == {"ok": True}
    finally:
        server.shutdown()


def test_unregister_releases_a_held_poll_and_clears_the_queue():
    """A leaving host releases its held long-poll with an empty poll and clears its queued connects."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"
        host_id, host_token = register(base)

        # a guest connect is queued
        status, _ = connect(base, host_id)
        assert status == 200

        # start a long-poll in a thread. The queued connect is delivered to it (a poll drains the
        # queue), so start a second poll after the first returns: that one is held.
        first = []

        def do_first_poll():
            s, b = http_json(f"{base}/v1/connects?id={host_id}&token={host_token}&wait=10")
            first.append((s, b))

        t1 = threading.Thread(target=do_first_poll, daemon=True)
        t1.start()
        t1.join(timeout=5)  # it returns with the queued connect
        assert len(first) == 1, "the first poll was not released"
        assert first[0][1]["connects"] and len(first[0][1]["connects"]) == 1

        # a second poll is now held (the queue is empty)
        second = []

        def do_second_poll():
            s, b = http_json(f"{base}/v1/connects?id={host_id}&token={host_token}&wait=10")
            second.append((s, b))

        t2 = threading.Thread(target=do_second_poll, daemon=True)
        t2.start()
        time.sleep(0.2)  # the poll is now held

        # the host leaves: the held poll is released with an empty, successful poll
        status, body = http_json(f"{base}/v1/unregister", method="POST", body={"id": host_id, "token": host_token})
        assert status == 200, body
        t2.join(timeout=5)
        assert len(second) == 1, "the held poll was not released"
        assert second[0] == (200, {"connects": []})

        # the listing is gone, and a re-register (same token, same id) starts with an empty queue
        status, body = http_json(f"{base}/v1/hosts?title=evolution&version=3")
        assert body["hosts"] == []
        status, body = http_json(
            f"{base}/v1/register",
            method="POST",
            body={
                "proto": 2,
                "title": "evolution",
                "version": "3",
                "mac": "02:46:43:12:34:56",
                "endpoint": "127.0.0.1:40000",
                "token": host_token,
            },
        )
        assert status == 200, body
        status, body = http_json(f"{base}/v1/connects?id={host_id}&token={host_token}&wait=1")
        assert status == 200
        assert body["connects"] == []  # the old connect was cleared by the unregister
    finally:
        server.shutdown()


def test_local_stored_on_register():
    """A valid private IPv4 local is stored; a public one or a bad format is ignored."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"
        # a valid private address
        token = secrets.token_hex(32)
        status, body = http_json(
            f"{base}/v1/register",
            method="POST",
            body={
                "proto": 2,
                "title": "evolution",
                "version": "3",
                "mac": "02:46:43:12:34:56",
                "endpoint": "8.8.8.7:40000",
                "token": token,
                "local": "192.168.1.50:3074",
            },
        )
        assert status == 200, body
        hid = body["id"]
        h = lobby.hosts.get(hid)
        assert h["local"] == "192.168.1.50:3074"

        # a public address is silently ignored
        token2 = secrets.token_hex(32)
        status, body = http_json(
            f"{base}/v1/register",
            method="POST",
            body={
                "proto": 2,
                "title": "evolution",
                "version": "3",
                "mac": "02:46:43:12:34:57",
                "endpoint": "8.8.8.8:40001",
                "token": token2,
                "local": "8.8.8.8:3074",
            },
        )
        assert status == 200, body
        assert lobby.hosts.get(body["id"]).get("local") is None

        # a malformed local is silently ignored
        token3 = secrets.token_hex(32)
        status, body = http_json(
            f"{base}/v1/register",
            method="POST",
            body={
                "proto": 2,
                "title": "evolution",
                "version": "3",
                "mac": "02:46:43:12:34:58",
                "endpoint": "8.8.8.9:40002",
                "token": token3,
                "local": "not-an-address",
            },
        )
        assert status == 200, body
        assert lobby.hosts.get(body["id"]).get("local") is None
    finally:
        server.shutdown()


def test_local_forwarded_same_ip():
    """The host's local is in the connect reply (the Python fake uses the TCP client address,
    so all requests share the same IP: the same-IP check always passes here).
    The cross-IP case is tested in the Node tests (lobby.test.mjs)."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"
        htoken = secrets.token_hex(32)
        status, body = http_json(
            f"{base}/v1/register",
            method="POST",
            body={
                "proto": 2,
                "title": "evolution",
                "version": "3",
                "mac": "02:46:43:12:34:56",
                "endpoint": "93.184.100.9:40000",
                "token": htoken,
                "local": "192.168.1.50:3074",
            },
        )
        assert status == 200, body
        hid = body["id"]

        # a guest connect: the reply includes the host's local (same IP in the Python fake)
        gtoken = secrets.token_hex(32)
        status, body = http_json(
            f"{base}/v1/connect",
            method="POST",
            body={
                "id": hid,
                "endpoint": "93.184.100.9:41000",
                "nonce": "e" * 32,
                "mac": "02:46:43:aa:bb:cc",
                "token": gtoken,
                "local": "192.168.1.51:3074",
            },
        )
        assert status == 200, body
        assert body.get("local") == "192.168.1.50:3074"
    finally:
        server.shutdown()


def test_local_in_poll():
    """The guest's local is in the host's poll when both share the caller IP (same NAT)."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"
        htoken = secrets.token_hex(32)
        status, body = http_json(
            f"{base}/v1/register",
            method="POST",
            body={
                "proto": 2,
                "title": "evolution",
                "version": "3",
                "mac": "02:46:43:12:34:56",
                "endpoint": "93.184.100.9:40000",
                "token": htoken,
                "local": "192.168.1.50:3074",
            },
            headers={"cf-connecting-ip": "93.184.100.9"},
        )
        assert status == 200, body
        hid = body["id"]

        # a guest connect with a local
        gtoken = secrets.token_hex(32)
        status, body = http_json(
            f"{base}/v1/connect",
            method="POST",
            body={
                "id": hid,
                "endpoint": "93.184.100.9:41000",
                "nonce": "e" * 32,
                "mac": "02:46:43:aa:bb:cc",
                "token": gtoken,
                "local": "192.168.1.51:3074",
            },
            headers={"cf-connecting-ip": "93.184.100.9"},
        )
        assert status == 200, body

        # the host's poll: the guest's local is included
        status, body = http_json(
            f"{base}/v1/connects?id={hid}&token={htoken}&wait=1",
            headers={"cf-connecting-ip": "93.184.100.9"},
        )
        assert status == 200, body
        assert len(body["connects"]) == 1
        assert body["connects"][0].get("local") == "192.168.1.51:3074"
    finally:
        server.shutdown()


def test_local_cgnat_rejected():
    """100.64/10 (CGNAT) is not a LAN: two phones behind the same carrier share no network."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"
        htoken = secrets.token_hex(32)
        status, body = http_json(
            f"{base}/v1/register",
            method="POST",
            body={
                "proto": 2,
                "title": "evolution",
                "version": "3",
                "mac": "02:46:43:12:34:56",
                "endpoint": "93.184.100.9:40000",
                "token": htoken,
                "local": "100.64.1.2:3074",
            },
        )
        assert status == 200, body
        hid = body["id"]

        # a connect with a CGNAT local is also rejected
        gtoken = secrets.token_hex(32)
        status, body = http_json(
            f"{base}/v1/connect",
            method="POST",
            body={
                "id": hid,
                "endpoint": "93.184.100.9:41000",
                "nonce": "e" * 32,
                "mac": "02:46:43:aa:bb:cc",
                "token": gtoken,
                "local": "100.64.1.3:3074",
            },
        )
        assert status == 200, body
        assert body.get("local") is None
    finally:
        server.shutdown()
