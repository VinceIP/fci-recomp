"""The fake host list (tools/fake_hostlist.py): register → poll held → connect releases it.

In-process on 127.0.0.1: the server runs in a thread, the test makes HTTP requests with urllib.
"""

from http.server import ThreadingHTTPServer
import json
import threading
import time
import urllib.error
import urllib.request

from fake_hostlist import Lobby, make_handler  # tools/ is on the path (conftest); a `tools.` import hides the toolkit's


def start_server(break_punch=False):
    """Start the fake host list on a random port; return (port, server, lobby)."""
    lobby = Lobby(break_punch=break_punch)
    server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(lobby))
    server.daemon_threads = True
    port = server.server_address[1]
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    return port, server, lobby


def http_json(url, method="GET", body=None):
    """Make an HTTP request; return (status, parsed_json_or_None)."""
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    if data:
        req.add_header("content-type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = resp.read()
            return resp.status, json.loads(raw) if raw else None
    except urllib.error.HTTPError as e:
        raw = e.read()
        return e.code, json.loads(raw) if raw else None


def test_register_poll_connect():
    """register → poll held → connect releases it with the connect data."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"

        # register a host
        status, body = http_json(
            f"{base}/v1/register",
            method="POST",
            body={
                "proto": 1,
                "title": "evolution",
                "version": "3",
                "mac": "02:46:43:12:34:56",
                "endpoint": "127.0.0.1:40000",
            },
        )
        assert status == 200, body
        host_id = body["id"]
        token = body["token"]
        assert len(host_id) == 24
        assert len(token) == 32

        # list: the host is there
        status, body = http_json(f"{base}/v1/hosts?title=evolution&version=3")
        assert status == 200
        assert len(body["hosts"]) == 1
        assert body["hosts"][0]["id"] == host_id
        assert body["hosts"][0]["endpoint"] == "127.0.0.1:40000"
        assert body["hosts"][0]["mac"] == "02:46:43:12:34:56"
        assert "token" not in body["hosts"][0]

        # start a long-poll in a thread (it will be held)
        result = []

        def do_poll():
            s, b = http_json(f"{base}/v1/connects?id={host_id}&token={token}&wait=10")
            result.append((s, b))

        t = threading.Thread(target=do_poll, daemon=True)
        t.start()
        time.sleep(0.2)  # the poll is now held

        # connect: releases the poll
        nonce = "0123456789abcdef0123456789abcdef"
        status, _ = http_json(
            f"{base}/v1/connect",
            method="POST",
            body={
                "id": host_id,
                "endpoint": "127.0.0.1:50000",
                "nonce": nonce,
                "mac": "02:46:43:aa:bb:cc",
            },
        )
        assert status == 204

        # the poll is released with the connect
        t.join(timeout=5)
        assert len(result) == 1, "the poll was not released"
        status, body = result[0]
        assert status == 200
        assert len(body["connects"]) == 1
        assert body["connects"][0]["endpoint"] == "127.0.0.1:50000"
        assert body["connects"][0]["nonce"] == nonce
        assert body["connects"][0]["mac"] == "02:46:43:aa:bb:cc"
    finally:
        server.shutdown()


def test_poll_times_out():
    """A poll with no connect times out with 204."""
    port, server, lobby = start_server()
    try:
        base = f"http://127.0.0.1:{port}"
        status, body = http_json(
            f"{base}/v1/register",
            method="POST",
            body={
                "proto": 1,
                "title": "instincts",
                "version": "1",
                "mac": "02:46:43:11:22:33",
                "endpoint": "127.0.0.1:40001",
            },
        )
        assert status == 200
        host_id = body["id"]
        token = body["token"]

        start = time.time()
        status, body = http_json(f"{base}/v1/connects?id={host_id}&token={token}&wait=1")
        elapsed = time.time() - start
        assert status == 204
        assert elapsed >= 0.9, f"wait=1 should hold for a second, got {elapsed:.2f}s"
    finally:
        server.shutdown()


def test_poll_wrong_token_is_404():
    """A poll with the wrong token is 404."""
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
                "endpoint": "127.0.0.1:40000",
            },
        )
        assert status == 200
        host_id = body["id"]

        status, _ = http_json(f"{base}/v1/connects?id={host_id}&token=wrong&wait=1")
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
                "id": "nonexistent",
                "endpoint": "127.0.0.1:50000",
                "nonce": "0123456789abcdef0123456789abcdef",
                "mac": "02:46:43:aa:bb:cc",
            },
        )
        assert status == 404
    finally:
        server.shutdown()


def test_register_uppercase_mac_is_rejected():
    """An uppercase MAC is rejected (the lobby wants lowercase)."""
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
                "mac": "02:46:43:AB:CD:EF",  # uppercase: rejected
                "endpoint": "127.0.0.1:40000",
            },
        )
        assert status == 400, body
        assert "MAC" in body["error"]
    finally:
        server.shutdown()


def test_break_punch_rewrites_connect_endpoint():
    """With --break-punch, the connect the host receives has the port + 1000 (a dead port)."""
    port, server, lobby = start_server(break_punch=True)
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
                "endpoint": "127.0.0.1:40000",
            },
        )
        assert status == 200, body
        host_id = body["id"]
        token = body["token"]

        result = []

        def do_poll():
            s, b = http_json(f"{base}/v1/connects?id={host_id}&token={token}&wait=10")
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
            },
        )
        assert status == 204

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


def test_connect_uppercase_mac_is_rejected():
    """A connect with an uppercase MAC is rejected."""
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
                "endpoint": "127.0.0.1:40000",
            },
        )
        assert status == 200
        host_id = body["id"]
        status, body = http_json(
            f"{base}/v1/connect",
            method="POST",
            body={
                "id": host_id,
                "endpoint": "127.0.0.1:50000",
                "nonce": "0123456789abcdef0123456789abcdef",
                "mac": "02:46:43:aa:bb:cc".upper(),  # uppercase: rejected
            },
        )
        assert status == 400, body
        assert "MAC" in body["error"]
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
