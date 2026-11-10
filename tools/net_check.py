#!/usr/bin/env python3
"""Repeatable two-instance LAN network check (Q23).

Runs the two-instance LAN match recorded in build/qwen-in/q23/run and gives a pass/fail verdict:

  1. both sides logged "LAN admit" (src/fci_nic.c:548 lan_admit) - each card admitted the other;
  2. no side logged "not admitted" (src/fci_nic.c:636: a title / disc version / net protocol
     mismatch);
  3. frames flowed both ways on each side ("first frame sent" + "first frame received",
     src/fci_nic.c:1344/:1381; the "nic: tx" / "nic: rx" counts, :1341/:1375, come from the
     frame trace, RECOMP_FCI_NET_TRACE=1 in the args);
  4. tools/sync_check.py passes on the two telemetry files (steady-state player sync).

The [FCI] network lines go to each instance's stderr, which tools/game_driver.py captures in the
run folder's stderr.log (<FCI_REGRESS>/drive/evo-sync-<side>-<HHMMSS>/stderr.log); that is what
this check reads, not the driver's stdout.

The menu steps are the Evolution flow (tools/net_check/evolution-<side>.args, verified on the
recorded run); for Instincts they are unverified, and the telemetry mapping (which the sync part
needs) exists for Evolution only.

Usage:
    python -B tools/net_check.py                 # run the match, then check
    python -B tools/net_check.py --check-only    # re-judge the newest run folders + telemetry
    python -B tools/net_check.py --check-only --host-run <dir> --join-run <dir>

Environment (as tools/game_driver.py): FCI_TITLE (default: evolution), FCI_INSTALL, FCI_REGRESS
(default: build/regress). The telemetry files are written under <FCI_REGRESS>/net-check-<HHMMSS>/,
so a fresh checkout runs with only those three set.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[1]
SYNC_CHECK = REPO_ROOT / "tools" / "sync_check.py"
ARGS_DIR = REPO_ROOT / "tools" / "net_check"
HOST_PREFIX = "evo-sync-host"
JOIN_PREFIX = "evo-sync-join"
JOIN_WAIT_S = 30
RUN_TIMEOUT_S = 600


def parse_side_log(text: str) -> dict[str, Any]:
    """The per-side network facts from one instance's stderr.log. The line formats are the ones
    src/fci_nic.c writes: "LAN admit" (:548 lan_admit), "... not admitted" (:636), "first frame
    sent" / "first frame received" (:1344/:1381), and the "nic: tx" / "nic: rx" frame trace lines
    (:1341/:1375, RECOMP_FCI_NET_TRACE=1)."""
    lines = text.splitlines()
    return {
        "admit": [line for line in lines if "LAN admit" in line],
        "not_admitted": [line for line in lines if "not admitted" in line],
        "first_frame_sent": any("first frame sent" in line for line in lines),
        "first_frame_received": any("first frame received" in line for line in lines),
        "tx_frames": sum(1 for line in lines if " nic: tx " in line),
        "rx_frames": sum(1 for line in lines if " nic: rx " in line),
    }


def side_reasons(facts: dict[str, Any]) -> list[str]:
    """Why this side fails the log checks (empty when it passes)."""
    reasons: list[str] = []
    if not facts["admit"]:
        reasons.append("no 'LAN admit' line (the other card was never admitted)")
    if facts["not_admitted"]:
        reasons.append(f"{len(facts['not_admitted'])} 'not admitted' line(s) (triple mismatch)")
    if not facts["first_frame_sent"]:
        reasons.append("no 'first frame sent' line (no frames out)")
    if not facts["first_frame_received"]:
        reasons.append("no 'first frame received' line (no frames in)")
    return reasons


def _drive_dirs(drive: Path, prefix: str) -> list[Path]:
    """The run folders for one side, oldest to newest by mtime."""
    return sorted(
        (p for p in drive.glob(prefix + "-*") if p.is_dir()),
        key=lambda p: p.stat().st_mtime,
    )


def newest_runs(drive: Path, host_prefix: str, join_prefix: str, since: float | None = None) -> tuple[Path, Path]:
    """The newest host and joiner run folders (by mtime); when since is given, only folders
    created at/after it count (a clock-skew allowance), as run-evo-sync.sh found the joiner's."""
    host = [p for p in _drive_dirs(drive, host_prefix) if since is None or p.stat().st_mtime >= since - 2]
    join = [p for p in _drive_dirs(drive, join_prefix) if since is None or p.stat().st_mtime >= since - 2]
    if not host or not join:
        raise FileNotFoundError(f"no {host_prefix}-* / {join_prefix}-* run folders under {drive}")
    return host[-1], join[-1]


def newest_tele(outdir_parent: Path, name: str) -> Path | None:
    """The newest <outdir_parent>/net-check-*/<name> telemetry file, or None."""
    for d in sorted(outdir_parent.glob("net-check-*"), reverse=True):
        p = d / name
        if p.is_file():
            return p
    return None


def run_sync_check(host_tele: str, join_tele: str, min_players: int) -> dict[str, Any]:
    """Run tools/sync_check.py on the two telemetry files; its JSON report plus the exit code."""
    proc = subprocess.run(
        [
            sys.executable,
            "-B",
            str(SYNC_CHECK),
            host_tele,
            join_tele,
            "--min-players",
            str(min_players),
        ],
        capture_output=True,
        text=True,
    )
    report: dict[str, Any] | None = None
    try:
        report = json.loads(proc.stdout)
    except json.JSONDecodeError:
        pass
    stderr_tail = proc.stderr.strip().splitlines()[-1] if proc.stderr.strip() else None
    ok = proc.returncode == 0 and bool(report and report.get("ok"))
    return {"rc": proc.returncode, "ok": ok, "report": report, "stderr": stderr_tail}


def _side_summary(facts: dict[str, Any]) -> dict[str, Any]:
    """The per-side verdict fields from the parsed log facts."""
    reasons = side_reasons(facts)
    return {
        "n_admit": len(facts["admit"]),
        "n_not_admitted": len(facts["not_admitted"]),
        "first_frame_sent": facts["first_frame_sent"],
        "first_frame_received": facts["first_frame_received"],
        "tx_frames": facts["tx_frames"],
        "rx_frames": facts["rx_frames"],
        "ok": not reasons,
        "reasons": reasons,
    }


def verdict(host_facts: dict[str, Any], join_facts: dict[str, Any], sync: dict[str, Any]) -> dict[str, Any]:
    """The pass/fail verdict from the per-side facts and the sync_check result."""
    host = _side_summary(host_facts)
    join = _side_summary(join_facts)
    sync_reasons: list[str] = []
    if not sync["ok"]:
        if sync.get("report") is None:
            sync_reasons.append("sync_check produced no report (missing telemetry file?)")
        else:
            sync_reasons.append(f"sync_check failed ({sync.get('stderr') or 'see report'})")
    ok = host["ok"] and join["ok"] and not sync_reasons
    reasons = [f"host: {r}" for r in host["reasons"]] + [f"join: {r}" for r in join["reasons"]] + sync_reasons
    return {"ok": ok, "host": host, "join": join, "sync": sync, "reasons": reasons}


def _append(path: Path, line: str) -> None:
    with open(path, "a") as f:
        f.write(line + "\n")


def run_match(
    wt: Path,
    py: str,
    regress: Path,
    title: str,
    outdir: Path,
    timeout: float,
) -> tuple[Path, Path, Path, Path]:
    """The two-instance LAN run, the same sequence as the recorded run (build/qwen-in/q23/run):
    host, then the joiner 30 s in, the joiner's captures, then the paired dumps around distinct
    moves. The telemetry paths are injected at run time under outdir. Returns (host run folder,
    joiner run folder, host telemetry, joiner telemetry)."""
    host_args = ARGS_DIR / f"{title}-host.args"
    join_args = ARGS_DIR / f"{title}-join.args"
    for a in (host_args, join_args):
        if not a.is_file():
            raise FileNotFoundError(
                f"no {a} (the menu steps are the Evolution flow; for other titles they are unverified)"
            )
    outdir.mkdir(parents=True, exist_ok=True)
    host_tele = outdir / "tele-host.jsonl"
    join_tele = outdir / "tele-join.jsonl"
    drive = regress / "drive"
    start = time.time()
    t_end = time.monotonic() + timeout
    env = dict(os.environ, FCI_TITLE=title)

    def left() -> float:
        return max(t_end - time.monotonic(), 0.0)

    n0 = len(_drive_dirs(drive, JOIN_PREFIX))
    with open(outdir / "host-driver.out", "wb") as ho, open(outdir / "join-driver.out", "wb") as jo:
        host = subprocess.Popen(
            [
                py,
                "-B",
                "tools/game_driver.py",
                HOST_PREFIX,
                f"@{host_args}",
                "--opt",
                f"RECOMP_FCI_TELEMETRY={host_tele}",
            ],
            cwd=wt,
            stdout=ho,
            stderr=subprocess.STDOUT,
            env=env,
        )
        time.sleep(min(JOIN_WAIT_S, left()))
        join = subprocess.Popen(
            [
                py,
                "-B",
                "tools/game_driver.py",
                JOIN_PREFIX,
                f"@{join_args}",
                "--opt",
                f"RECOMP_FCI_TELEMETRY={join_tele}",
            ],
            cwd=wt,
            stdout=jo,
            stderr=subprocess.STDOUT,
            env=env,
        )
        # the joiner's run folder appears once its driver starts
        while len(_drive_dirs(drive, JOIN_PREFIX)) <= n0:
            if left() < 1:
                raise TimeoutError("no joiner run folder appeared (the joiner driver failed to start)")
            time.sleep(1)
        host_dir, join_dir = newest_runs(drive, HOST_PREFIX, JOIN_PREFIX, since=start)
        capture = join_dir / "capture"
        if not capture.is_dir():
            raise RuntimeError(f"no capture folder in {join_dir}")
        while len(list(capture.iterdir())) < 3:
            if left() < 1:
                raise TimeoutError("the joiner never captured (3 captures expected)")
            time.sleep(1)
        time.sleep(min(3, left()))

        def both(name: str) -> None:
            _append(host_dir / "control.txt", f"dump {name}")
            _append(join_dir / "control.txt", f"dump {name}")
            time.sleep(min(4, left()))

        both("s0")
        both("s1")
        _append(host_dir / "pad.txt", "lup:1500")
        _append(join_dir / "pad.txt", "lleft:1500")
        time.sleep(min(3, left()))
        both("s2")
        both("s3")
        _append(host_dir / "pad.txt", "rright:400")
        time.sleep(min(2, left()))
        for p in (host, join):
            p.wait(timeout=left() or 1)
    return host_dir, join_dir, host_tele, join_tele


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Two-instance LAN network check (Q23)")
    ap.add_argument(
        "--check-only", action="store_true", help="check existing run folders and telemetry, do not run the match"
    )
    ap.add_argument("--title", default=None, help="the game (default: the FCI_TITLE env var, else evolution)")
    ap.add_argument(
        "--regress",
        type=Path,
        default=None,
        help="the FCI_REGRESS dir (default: the FCI_REGRESS env var, else build/regress)",
    )
    ap.add_argument("--wt", type=Path, default=REPO_ROOT, help="the worktree with tools/ (default: this checkout)")
    ap.add_argument(
        "--py", default=sys.executable, help="the python that runs tools/game_driver.py (default: this python)"
    )
    ap.add_argument(
        "--host-run",
        type=Path,
        default=None,
        help="check-only: the host run folder (default: the newest evo-sync-host-*)",
    )
    ap.add_argument(
        "--join-run",
        type=Path,
        default=None,
        help="check-only: the joiner run folder (default: the newest evo-sync-join-*)",
    )
    ap.add_argument(
        "--host-tele",
        type=Path,
        default=None,
        help="check-only: the host telemetry file (default: the newest net-check-*/tele-host.jsonl)",
    )
    ap.add_argument(
        "--join-tele",
        type=Path,
        default=None,
        help="check-only: the joiner telemetry file (default: the newest net-check-*/tele-join.jsonl)",
    )
    ap.add_argument("--min-players", type=int, default=2, help="players sync_check needs to pass (default 2)")
    ap.add_argument("--run-timeout", type=float, default=RUN_TIMEOUT_S, help="max run time (s)")
    args = ap.parse_args(argv)

    title = args.title or os.environ.get("FCI_TITLE", "evolution")
    regress = args.regress or Path(os.environ.get("FCI_REGRESS", REPO_ROOT / "build" / "regress"))
    drive = regress / "drive"

    if args.check_only:
        try:
            if args.host_run and args.join_run:
                host_dir, join_dir = args.host_run, args.join_run
            else:
                auto_host, auto_join = newest_runs(drive, HOST_PREFIX, JOIN_PREFIX)
                host_dir = args.host_run or auto_host
                join_dir = args.join_run or auto_join
        except FileNotFoundError as e:
            print(f"net_check: {e}", file=sys.stderr)
            return 2
        host_tele = args.host_tele or newest_tele(regress, "tele-host.jsonl")
        join_tele = args.join_tele or newest_tele(regress, "tele-join.jsonl")
    else:
        outdir = regress / f"net-check-{time.strftime('%H%M%S')}"
        try:
            host_dir, join_dir, host_tele, join_tele = run_match(
                args.wt, args.py, regress, title, outdir, args.run_timeout
            )
        except (OSError, TimeoutError, RuntimeError, FileNotFoundError) as e:
            print(f"net_check: run failed: {e}", file=sys.stderr)
            return 2

    host_log = host_dir / "stderr.log"
    join_log = join_dir / "stderr.log"
    try:
        host_text = host_log.read_text(errors="replace")
        join_text = join_log.read_text(errors="replace")
    except OSError as e:
        print(f"net_check: {e}", file=sys.stderr)
        return 2

    if host_tele is None or join_tele is None:
        sync = {
            "rc": 2,
            "ok": False,
            "report": None,
            "stderr": "no telemetry files (the sync part needs the telemetry mapping, Evolution only)",
        }
    else:
        sync = run_sync_check(str(host_tele), str(join_tele), args.min_players)

    v = verdict(parse_side_log(host_text), parse_side_log(join_text), sync)
    print(json.dumps(v, indent=2))
    for r in v["reasons"]:
        print(f"net_check: {r}", file=sys.stderr)
    print(f"net_check: {'PASS' if v['ok'] else 'FAIL'}", file=sys.stderr)
    return 0 if v["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
