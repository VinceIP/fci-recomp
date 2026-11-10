"""tools/sync_check.py on synthetic telemetry files (no game, no dumps)."""

from __future__ import annotations

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import sync_check as sc  # noqa: E402

Q = 1.0 / 512.0  # the network quantum (m)


def write_samples(path: Path, samples: list[dict]) -> Path:
    with open(path, "w") as f:
        for s in samples:
            f.write(json.dumps(s) + "\n")
    return path


def sample(wall: int, tick: int, players: list[dict]) -> dict:
    return {"ms": wall, "wall": wall, "tick": tick, "players": players}


def player(pid: int, pos, yaw: float, local: bool = False) -> dict:
    """A player with ang=[0, 0, yaw] (the yaw is the third component)."""
    return {"id": pid, "pos": list(pos), "ang": [0.0, 0.0, yaw], "local": local, "health": None}


def quant(x: float) -> float:
    """Snap to the 1/512 m grid, as the network does."""
    return round(x / Q) * Q


class Run:
    """Run sc.main on two in-memory sample lists; return (exit_code, report)."""

    def __call__(self, host: list[dict], join: list[dict], **kw) -> tuple[int, dict]:
        with tempfile.TemporaryDirectory() as d:
            hp = write_samples(Path(d) / "h.jsonl", host)
            jp = write_samples(Path(d) / "j.jsonl", join)
            argv = [str(hp), str(jp)]
            for k, v in kw.items():
                argv.append("--" + k.replace("_", "-"))
                argv.append(str(v))
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = sc.main(argv)
            return rc, json.loads(buf.getvalue())


class LoadTest(unittest.TestCase):
    def test_load_sorted_by_wall(self):
        with tempfile.TemporaryDirectory() as d:
            p = write_samples(
                Path(d) / "t.jsonl",
                [
                    sample(200, 2, [player(2, [1, 2, 3], 10.0)]),
                    sample(100, 1, [player(2, [1, 2, 3], 10.0)]),
                ],
            )
            s = sc.load(str(p))
            self.assertEqual([x[0] for x in s], [100, 200])
            self.assertEqual(list(s[0][1].keys()), [2])


class YawTest(unittest.TestCase):
    def test_wraparound(self):
        self.assertAlmostEqual(sc.yaw_delta(350.0, 10.0), 20.0)
        self.assertAlmostEqual(sc.yaw_delta(10.0, 350.0), 20.0)
        self.assertAlmostEqual(sc.yaw_delta(0.0, 0.0), 0.0)
        self.assertAlmostEqual(sc.yaw_delta(0.0, 180.0), 180.0)

    def test_yaw_is_third_component(self):
        # ang is [pitch, roll, yaw]: a yaw difference is read from index 2, not index 0
        self.assertEqual(sc.YAW_INDEX, 2)
        track = sc.player_track([(100, {2: player(2, [0, 0, 0], 0.0)}), (200, {2: player(2, [1, 0, 0], 45.0)})], 2)
        self.assertEqual(list(track[2]), [0.0, 45.0])  # the yaw column, from ang[2]


class LagFitTest(unittest.TestCase):
    def test_moving_5mps_lag100_passes(self):
        # the owner walks at 5 m/s; the remote is the owner 100 ms behind, quantised to 1/512 m.
        # linear interpolation is exact at constant velocity, so the residual is just the quantum.
        run = Run()
        host = []
        join = []
        for i in range(40):
            t = i * 100  # ms
            host.append(sample(t, i, [player(2, [5.0 * t / 1000.0, 0, 0], 0.0, local=True)]))
            rx = quant(5.0 * (t - 100) / 1000.0) if t >= 100 else 0.0
            join.append(sample(t, i, [player(2, [rx, 0, 0], 0.0)]))
        rc, rep = run(host, join, min_players=1)
        self.assertEqual(rc, 0)
        p = rep["players"][0]
        self.assertAlmostEqual(p["lag_ms"], 100.0, delta=10.0)  # the fitted lag is ~100 ms
        self.assertLessEqual(p["pos_p95"], 0.05)  # within the quantum, well under the tolerance

    def test_three_second_shift_fails(self):
        # the joiner's wall is +3000 ms: far beyond the 500 ms lag range, so the in-span samples
        # are wildly off (the owner moved metres during the uncorrected shift)
        run = Run()
        walls = list(range(0, 4000, 100))
        host = [sample(t, i, [player(2, [5.0 * t / 1000.0, 0, 0], 0.0, local=True)]) for i, t in enumerate(walls)]
        join = [sample(t + 3000, i, [player(2, [5.0 * t / 1000.0, 0, 0], 0.0)]) for i, t in enumerate(walls)]
        rc, rep = run(host, join, min_players=1)
        self.assertEqual(rc, 1)
        self.assertGreater(rep["players"][0]["pos_p95"], 0.05)

    def test_stalled_remote_fails(self):
        # the owner walks but the remote is frozen: the residual grows as the owner moves
        run = Run()
        walls = list(range(0, 4000, 100))
        host = [sample(t, i, [player(2, [5.0 * t / 1000.0, 0, 0], 0.0, local=True)]) for i, t in enumerate(walls)]
        join = [sample(t, i, [player(2, [0.0, 0, 0], 0.0)]) for i, t in enumerate(walls)]
        rc, rep = run(host, join, min_players=1)
        self.assertEqual(rc, 1)
        self.assertGreater(rep["players"][0]["pos_p95"], 0.05)

    def test_in_sync_two_players_passes(self):
        # both players in sync, each owned by a different file (the two-instance layout); the
        # remote in each file is the other owner delayed by a positive lag
        run = Run()
        host = []
        join = []
        for i in range(20):
            t = i * 100
            host.append(
                sample(
                    t,
                    i,
                    [
                        player(2, [2.0 * t / 1000.0, 0, 0], 0.0, local=True),
                        player(3, [10.0, 3.0 * (t - 50) / 1000.0, 0], 90.0),  # join's id 3, 50 ms behind
                    ],
                )
            )
            join.append(
                sample(
                    t,
                    i,
                    [
                        player(2, [2.0 * (t - 50) / 1000.0, 0, 0], 0.0),  # host's id 2, 50 ms behind
                        player(3, [10.0, 3.0 * t / 1000.0, 0], 90.0, local=True),
                    ],
                )
            )
        rc, rep = run(host, join)
        self.assertEqual(rc, 0)
        self.assertEqual(len(rep["players"]), 2)
        self.assertTrue(all(p["ok"] for p in rep["players"]))
        for p in rep["players"]:
            self.assertAlmostEqual(p["lag_ms"], 50.0, delta=10.0)

    def test_teleport_is_skipped_not_failed(self):
        # a spawn: both owners jump 50 m at t=1000 and the copies follow one sample late; the samples
        # next to the jump are skipped, so the pair still passes
        def track(t, base):
            return [base + (50.0 if t >= 1000 else 0.0) + 2.0 * t / 1000.0, 0, 0]

        host, join = [], []
        for i in range(30):
            t = i * 100
            late = max(t - 100, 0)
            host.append(sample(t, i, [player(2, track(t, 0), 0.0, local=True), player(3, track(late, 5), 0.0)]))
            join.append(sample(t, i, [player(2, track(late, 0), 0.0), player(3, track(t, 5), 0.0, local=True)]))
        rc, rep = Run()(host, join)
        self.assertEqual(rc, 0)

    def test_yaw_interpolates_across_180(self):
        walls = np.array([0.0, 100.0])
        self.assertAlmostEqual(abs(sc.interp_yaw(walls, np.array([170.0, -170.0]), 50.0)), 180.0, places=6)

    def test_one_shared_player_fails_by_default(self):
        # min_players defaults to 2: one shared player is not enough
        run = Run()
        host = [sample(t, i, [player(2, [0, 0, 0], 0.0, local=True)]) for i, t in enumerate(range(0, 4000, 100))]
        join = [sample(t, i, [player(2, [0, 0, 0], 0.0)]) for i, t in enumerate(range(0, 4000, 100))]
        rc, _ = run(host, join)
        self.assertEqual(rc, 1)

    def test_nothing_shared_fails(self):
        run = Run()
        host = [sample(t, i, [player(2, [0, 0, 0], 0.0, local=True)]) for i, t in enumerate(range(0, 4000, 100))]
        join = [sample(t, i, [player(9, [0, 0, 0], 0.0, local=True)]) for i, t in enumerate(range(0, 4000, 100))]
        rc, rep = run(host, join)
        self.assertEqual(rc, 1)
        self.assertEqual(rep["players"], [])

    def test_spawn_transient_passes(self):
        # the q23 run: the copy starts from a stale spawn state (0.79 m off in z) and converges
        # within 4 samples while the owner walks; the short excess run is a transient, not a desync
        run = Run()
        host = []
        join = []
        z_off = {0: 0.79, 1: 0.4, 2: 0.15, 3: 0.03}  # the spawn-state mismatch, converging
        for i in range(60):
            t = i * 100
            host.append(sample(t, i, [player(2, [2.0 * t / 1000.0, 0, 0], 0.0, local=True)]))
            x = quant(2.0 * (t - 100) / 1000.0) if t >= 100 else 0.0
            join.append(sample(t, i, [player(2, [x, 0, z_off.get(i, 0.0)], 0.0)]))
        rc, rep = run(host, join, min_players=1)
        self.assertEqual(rc, 0)
        p = rep["players"][0]
        self.assertLessEqual(p["pos_excess_run_ms"], 5 * p["interval_ms"])  # a short run

    def test_sustained_excess_burst_fails(self):
        # 9 consecutive samples (9 intervals) 3.5 m off, 191 in sync: the p95 stays within tolerance
        # (the bad samples are 4.5% of the run), but the excess run is too long to be a transient
        run = Run()
        host = []
        join = []
        for i in range(200):
            t = i * 100
            host.append(sample(t, i, [player(2, [5.0 * t / 1000.0, 0, 0], 0.0, local=True)]))
            behind = 800 if 100 <= i <= 108 else 100  # a 700 ms dip for 9 samples
            x = quant(5.0 * max(t - behind, 0) / 1000.0)
            join.append(sample(t, i, [player(2, [x, 0, 0], 0.0)]))
        rc, rep = run(host, join, min_players=1)
        self.assertEqual(rc, 1)
        p = rep["players"][0]
        self.assertLessEqual(p["pos_p95"], 0.05)  # the steady state is in sync
        self.assertGreater(p["pos_excess_run_ms"], 5 * p["interval_ms"])  # but the run is too long

    def test_moving_still_split(self):
        # the owner walks for the first 2 s (moving) then stops (still); the remote follows exactly
        run = Run()
        host = []
        join = []
        for i in range(40):
            t = i * 100
            x = 5.0 * min(t, 2000) / 1000.0  # walks to x=10 by 2 s, then stops
            host.append(sample(t, i, [player(2, [x, 0, 0], 0.0, local=True)]))
            join.append(sample(t, i, [player(2, [x, 0, 0], 0.0)]))
        rc, rep = run(host, join, min_players=1)
        self.assertEqual(rc, 0)
        p = rep["players"][0]
        self.assertGreater(p["moving"], 0)  # the walking part
        self.assertGreater(p["still"], 0)  # the stopped part


if __name__ == "__main__":
    unittest.main()
