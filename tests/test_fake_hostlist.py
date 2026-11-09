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
from fake_hostlist import Lobby, derive_id, make_handler


def start_server(break_punch=False, secret=None):
    """Start the fake host list on a random port; return (port, server, lobby)."""
    lobby = Lobby(break_punch=break_punch, secret=secret)
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


def register(base, title="evolution", version="3", endpoint="127.0.0.1:40000", token=None):
    token = token or secrets.token_hex(32)
    status, body = http_json(
        f"{base}/v1/register",
        method="POST",
        body={
            "proto": 1,
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
                    "proto": 1,
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
                "proto": 1,
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
                "proto": 1,
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
