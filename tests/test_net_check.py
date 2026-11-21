"""tools/net_check.py on recorded inputs (no game): the stderr.log excerpts recorded from the real
two-instance run (tests/fixtures/net-check/, lines from build/qwen-in/q23/run/host-stderr.log and
join-stderr.log) plus small in-sync / shifted telemetry pairs for the check-only verdict."""

from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import net_check as nc  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures" / "net-check"

Q = 1.0 / 512.0  # the network quantum (m)


def write_samples(path: Path, samples: list[dict]) -> Path:
    with open(path, "w") as f:
        for s in samples:
            f.write(json.dumps(s) + "\n")
    return path


def sample(wall: int, tick: int, players: list[dict]) -> dict:
    return {"ms": wall, "wall": wall, "tick": tick, "players": players}


def player(pid: int, pos, yaw: float, local: bool = False) -> dict:
    return {"id": pid, "pos": list(pos), "ang": [0.0, 0.0, yaw], "local": local, "health": None}


def quant(x: float) -> float:
    return round(x / Q) * Q


def in_sync_pair(n: int = 40, lag_ms: int = 100, shift_ms: int = 0) -> tuple[list[dict], list[dict]]:
    """A small two-player in-sync pair (the owner walks, the copies follow, quantised)."""
    host: list[dict] = []
    join: list[dict] = []
    for i in range(n):
        t = i * 100
        host.append(
            sample(
                t,
                i,
                [
                    player(2, [2.0 * t / 1000.0, 0, 0], 0.0, local=True),
                    player(3, [10.0, 3.0 * max(t - lag_ms, 0) / 1000.0], 90.0),
                ],
            )
        )
        join.append(
            sample(
                t + shift_ms,
                i,
                [
                    player(2, [quant(2.0 * max(t - lag_ms, 0) / 1000.0), 0, 0], 0.0),
                    player(3, [10.0, 3.0 * t / 1000.0], 90.0, local=True),
                ],
            )
        )
    return host, join


class ParseSideLogTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.host = nc.parse_side_log((FIXTURES / "host-stderr.log").read_text())
        cls.join = nc.parse_side_log((FIXTURES / "join-stderr.log").read_text())

    def test_host_fixture(self):
        # recorded: the host admitted the joiner's card, frames both ways, 2 tx and 2 rx trace lines
        f = self.host
        self.assertEqual(len(f["admit"]), 1)
        self.assertIn("LAN admit 02:46:43:32:FC:03", f["admit"][0])
        self.assertEqual(f["not_admitted"], [])
        self.assertTrue(f["first_frame_sent"])
        self.assertTrue(f["first_frame_received"])
        self.assertEqual((f["tx_frames"], f["rx_frames"]), (2, 2))
        self.assertEqual(nc.side_reasons(f), [])

    def test_join_fixture(self):
        # recorded: the joiner admitted the host's card, frames both ways, 1 tx and 1 rx trace line
        f = self.join
        self.assertEqual(len(f["admit"]), 1)
        self.assertIn("LAN admit 02:46:43:BB:49:60", f["admit"][0])
        self.assertEqual(f["not_admitted"], [])
        self.assertTrue(f["first_frame_sent"])
        self.assertTrue(f["first_frame_received"])
        self.assertEqual((f["tx_frames"], f["rx_frames"]), (1, 1))
        self.assertEqual(nc.side_reasons(f), [])

    def test_missing_admit(self):
        # the LAN-up line alone (recorded format): nothing admitted, no frames
        f = nc.parse_side_log("[FCI] network: LAN up, 02:46:43:BB:49:60, bound 0.0.0.0:47920\n")
        reasons = nc.side_reasons(f)
        self.assertTrue(any("LAN admit" in r for r in reasons))
        self.assertTrue(any("first frame sent" in r for r in reasons))
        self.assertTrue(any("first frame received" in r for r in reasons))

    def test_not_admitted(self):
        # the mismatch line in the format src/fci_nic.c:635-637 writes it: it names the peer, so it
        # must count as "not admitted" and not as an admit
        line = (
            "[FCI] network: peer 02:46:43:BB:49:60 is evolution version 1234 net protocol 5; "
            "we are evolution version 1235 net protocol 5; not admitted"
        )
        f = nc.parse_side_log(line + "\n")
        self.assertEqual(f["not_admitted"], [line])
        self.assertEqual(f["admit"], [])
        self.assertTrue(any("not admitted" in r for r in nc.side_reasons(f)))


class VerdictTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.host = nc.parse_side_log((FIXTURES / "host-stderr.log").read_text())
        cls.join = nc.parse_side_log((FIXTURES / "join-stderr.log").read_text())

    def good_sync(self) -> dict:
        return {"rc": 0, "ok": True, "report": {"ok": True}, "stderr": None}

    def fail_sync(self) -> dict:
        return {"rc": 1, "ok": False, "report": {"ok": False}, "stderr": "sync_check: FAIL (0/2 players ok)"}

    def test_pass(self):
        v = nc.verdict(self.host, self.join, self.good_sync())
        self.assertTrue(v["ok"])
        self.assertEqual(v["reasons"], [])
        self.assertEqual(v["host"]["n_admit"], 1)
        self.assertEqual(v["join"]["n_admit"], 1)

    def test_fail_side(self):
        # a side with no admit line (the LAN-up line alone)
        bad = nc.parse_side_log("[FCI] network: LAN up, 02:46:43:BB:49:60, bound 0.0.0.0:47920\n")
        v = nc.verdict(bad, self.join, self.good_sync())
        self.assertFalse(v["ok"])
        self.assertTrue(any(r.startswith("host: ") for r in v["reasons"]))
        self.assertFalse(any(r.startswith("join: ") for r in v["reasons"]))

    def test_fail_sync(self):
        v = nc.verdict(self.host, self.join, self.fail_sync())
        self.assertFalse(v["ok"])
        self.assertTrue(any("sync_check failed" in r for r in v["reasons"]))


class CheckOnlyTest(unittest.TestCase):
    """--check-only end to end: run folders with the recorded stderr.log excerpts, small telemetry
    pairs for the sync part."""

    def make_run(self, d: Path, host_tele: Path | None = None, join_tele: Path | None = None) -> None:
        drive = d / "drive"
        for side, fix in (
            ("evo-sync-host-224055", "host-stderr.log"),
            ("evo-sync-join-224125", "join-stderr.log"),
        ):
            (drive / side).mkdir(parents=True)
            (drive / side / "stderr.log").write_bytes((FIXTURES / fix).read_bytes())
        if host_tele is not None or join_tele is not None:
            out = d / "net-check-224055"
            out.mkdir(parents=True)
            if host_tele is not None:
                write_samples(out / "tele-host.jsonl", host_tele)
            if join_tele is not None:
                write_samples(out / "tele-join.jsonl", join_tele)

    def run_check(self, d: Path, argv: list[str]) -> tuple[int, dict]:
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = nc.main(["--check-only", "--regress", str(d)] + argv)
        return rc, json.loads(buf.getvalue())

    def test_check_only_pass(self):
        host, join = in_sync_pair()
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            self.make_run(d, host, join)  # telemetry under net-check-224055/ (the default)
            rc, v = self.run_check(d, [])
        self.assertEqual(rc, 0)
        self.assertTrue(v["ok"])
        self.assertTrue(v["sync"]["ok"])

    def test_check_only_shifted_fails(self):
        # the same pair with the joiner's wall shifted 3 s: the log parts pass, the sync fails
        host, join = in_sync_pair(shift_ms=3000)
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            self.make_run(d, host, join)
            rc, v = self.run_check(d, [])
        self.assertEqual(rc, 1)
        self.assertTrue(v["host"]["ok"] and v["join"]["ok"])
        self.assertFalse(v["sync"]["ok"])

    def test_check_only_explicit_run_folders(self):
        host, join = in_sync_pair()
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            self.make_run(d, host, join)
            hp, jp = write_samples(d / "h.jsonl", host), write_samples(d / "j.jsonl", join)
            rc, v = self.run_check(
                d,
                [
                    "--host-run",
                    str(d / "drive" / "evo-sync-host-224055"),
                    "--join-run",
                    str(d / "drive" / "evo-sync-join-224125"),
                    "--host-tele",
                    str(hp),
                    "--join-tele",
                    str(jp),
                ],
            )
        self.assertEqual(rc, 0)
        self.assertTrue(v["ok"])

    def test_check_only_newest_run_folders(self):
        # an older pair with no admit line, a newer pair with it: the newest wins
        host, join = in_sync_pair()
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            self.make_run(d, host, join)
            for side in ("evo-sync-host-224055", "evo-sync-join-224125"):
                kind = side.split("-")[2]  # host / join
                old = d / "drive" / f"evo-sync-{kind}-090000"
                old.mkdir(parents=True)
                (old / "stderr.log").write_text("[FCI] network: LAN up, 02:46:43:BB:49:60, bound 0.0.0.0:47920\n")
                old_time = time.time() - 3600
                os.utime(old / "stderr.log", (old_time, old_time))
                os.utime(old, (old_time, old_time))
            rc, v = self.run_check(d, [])
        self.assertEqual(rc, 0)
        self.assertTrue(v["ok"])

    def test_check_only_no_run_folders(self):
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            (d / "drive").mkdir()
            buf = io.StringIO()
            err = io.StringIO()
            with redirect_stdout(buf), redirect_stderr(err):
                rc = nc.main(["--check-only", "--regress", str(d)])
        self.assertEqual(rc, 2)
        self.assertIn("no evo-sync-host-*", err.getvalue())


if __name__ == "__main__":
    unittest.main()
