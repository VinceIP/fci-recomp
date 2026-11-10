#!/usr/bin/env python3
"""net_fuzz.py: send hostile datagrams at a running card's UDP port and judge the card's stderr.

The card's network gate (q24, src/fci_frame.c, the rx_enqueue gate in src/fci_nic.c) must drop
hostile datagrams, count them, log them, and keep the game running. This tool is the external half
of that proof: it sends the hostile traffic, and --log points it at the card's stderr file (the
launcher's stderr.log, as in tools/net_check.py) so the drops can be judged.

Judging is from the gate's counters line, not the detailed drop lines: the detailed lines are
rate-limited (the first eight, then one a 10 s), so a burst hides most of the reasons. The counters
line ("[FCI] network: gate drops: envelope N, rate cap N, bad frame N (short N, ...)" in
src/fci_nic.c) carries every counter by name, cumulative, at most once a 10 s. The fuzzer waits
past the 10 s window, causes one drop to force a fresh line, and judges each counter from the last
one. The slow valid frames must raise no counter.

Modes:
  lan   the card is in LAN mode (bound 0.0.0.0:<port>). The fuzzer knocks with --triple
        (title:versionhex:proto, the triple the card checks; the version is the XBE certificate's
        4-byte Version field, little-endian hex), is admitted, and then the gate itself is
        exercised: bad frames, a rate burst, and valid frames that must pass.
  list  the card is on the host list (its public port). The fuzzer is not an admitted peer, so
        only the envelope layer (magic/kind/size) is reachable from outside; the gate is exercised
        by the game's own traffic (tools/net_check.py) and the unit tests (tests/test_frame_sanity.py).

Usage (from the host running the card):
  python tools/net_fuzz.py --target 192.168.1.50:50000 --mode lan --triple 1:00000003:2 \
      --log <run-folder>/stderr.log
  python tools/net_fuzz.py --target 203.0.113.9:40000 --mode list --send-only

Exit codes: 0 = the card survived and dropped what it should, 1 = a check failed, 2 = the fuzzer
could not admit itself in LAN mode (wrong --triple), 3 = usage/network error.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import socket
import struct
import sys
import threading
import time

MAGIC = b"FCN1"
ETHER_MAX = 1514
# the fuzzer's card MAC: the cards' prefix, locally administered, and not a real card's (02:46:43
# is the cards' OUI-like prefix; FA:xx:xx is unused by the card's 02:46:43:xx:xx:xx allocation)
FUZZ_MAC = bytes.fromhex("024643FA1122")
# the card logs its MAC in upper case (src/fci_nic.c); the admit line is matched on that
ADMIT_RE = re.compile(re.escape("LAN admit " + FUZZ_MAC.hex(":").upper()))
SUMMARY_RE = re.compile(r"\[FCI\] network: gate drops: (.*)", re.MULTILINE)
# the hostile gate vectors, by fci_frame_reason() name: how many of each the fuzzer sends
# (src/fci_frame.c classifies them; tests/test_net_fuzz.py cross-checks this against the C)
GATE_EXPECT = {
    "ip-total-length": 4,  # too short x2, IP total length oversize, IP total length short
    "ethertype": 2,  # ARP, IPv6 EtherType
    "ip-version": 1,  # IP version 6
    "ip-header": 1,  # IHL too big
    "fragment": 2,  # MF set, fragment offset set
    "not-udp": 1,  # ICMP
    "udp-length": 2,  # UDP length oversize, UDP length short
    "padding": 1,  # excessive Ethernet padding
}
SUMMARY_TRIGGER = b"\x00"  # 1-byte garbage: no FCN1 magic, always an envelope drop


def build_frame(
    mac: bytes,
    src_ip: bytes,
    dst_ip: bytes,
    eth_type: bytes = b"\x08\x00",
    ip_payload: bytes = b"\x00" * 24,
    proto: int = 17,
    totlen: int | None = None,
    flags_frag: int = 0,
    udp_len: int | None = None,
    version: int = 4,
    ihl: int = 5,
    eth_len: int | None = None,
) -> bytes:
    """An Ethernet/IPv4/UDP frame built field by field, so each hostile vector is exact.

    The 66-byte shape is the recorded game frame: 14 Ethernet + 20 IP + 8 UDP + 24 payload; a
    hostile vector breaks exactly one field of it.
    """
    if udp_len is None:
        udp_len = 8 + len(ip_payload)
    if totlen is None:
        totlen = 20 + udp_len
    # the 16-bit flags/offset field: bit 14 DF (0x4000), bit 13 MF (0x2000), low 13 bits the offset;
    # the game's frames set DF, as the recorded run's IP headers do
    ip = struct.pack(
        "!BBHHHBBH4s4s",
        (version << 4) | ihl,
        0,  # DSCP/ECN
        totlen,
        0,  # identification: arbitrary
        flags_frag | 0x4000,  # DF plus the caller's flags (MF 0x2000) / offset
        64,  # TTL
        proto,
        0,  # checksum (a zero is a valid wire value; the guest's stack recomputes)
        src_ip,
        dst_ip,
    )
    udp = struct.pack("!HHHH", 64000, 64000, udp_len, 0) + ip_payload
    eth = b"\x02\x46\x43\xaa\xbb\xcc" + mac + eth_type
    f = eth + ip + udp
    if eth_len is not None and eth_len != len(f):
        pad = eth_len - len(f)
        if pad >= 0:
            f = f + b"\x00" * pad
        else:
            f = f[:eth_len]
    return f


def valid_frame(src_ip: bytes, dst_ip: bytes) -> bytes:
    """The 66-byte game frame: what a card on the segment sends (recorded: 66 bytes, 329 in q23)."""
    return build_frame(FUZZ_MAC, src_ip, dst_ip)


def knock(triple: bytes) -> bytes:
    """The LAN knock: magic, 'H', the fuzzer's MAC, the 6-byte triple (17 bytes total)."""
    return MAGIC + b"H" + FUZZ_MAC + triple


def envelope(frame: bytes) -> bytes:
    """A frame datagram: the FCN1 magic, the 'F' kind, the Ethernet frame (src/fci_nic.c: the card
    drops a datagram to the envelope unless it is FCN1 'F' + a 14..1514-byte frame)."""
    return MAGIC + b"F" + frame


class Fuzzer:
    def __init__(self, target: tuple[str, int], mode: str, triple: bytes, log: Path | None):
        self.target = target
        self.mode = mode
        self.triple = triple
        self.log = log
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(5.0)
        self.sent = 0
        self.log_start = 0
        self.failures: list[str] = []
        self.notes: list[str] = []
        self._stop_knock = threading.Event()
        self._knock_thread: threading.Thread | None = None

    def send(self, data: bytes) -> None:
        self.sock.sendto(data, self.target)
        self.sent += 1

    def send_many(self, data: bytes, n: int, gap: float = 0.0) -> None:
        for i in range(n):
            self.send(data)
            if gap and i < n - 1:
                time.sleep(gap)

    def tail_log(self) -> str:
        """The card's stderr since the fuzzer started (rate-limited logs: read the window)."""
        if not self.log or not self.log.exists():
            return ""
        text = self.log.read_text(errors="replace")
        return text[self.log_start :] if self.log_start < len(text) else text

    def admit(self) -> bool:
        """Knock until the card admits the fuzzer's MAC (its log line), up to 10 knocks."""
        for _ in range(10):
            self.send(knock(self.triple))
            time.sleep(1.1)  # the card's knock period is 1 s; its reply is the admit line
            if self.log and ADMIT_RE.search(self.tail_log()):
                self.notes.append("admitted by knock")
                return True
        return False

    # the knock thread (q24): the card expires a MAC whose knocks stop for 5 s
    # (LAN_KNOCK_EXPIRE_MS), and the fuzzer's waits for the 10 s counters line outlast that. A real
    # card knocks every second for the whole match, so the fuzzer does too, in a thread

    def start_knocking(self) -> None:
        self._stop_knock.clear()
        self._knock_thread = threading.Thread(target=self._knock_loop, daemon=True)
        self._knock_thread.start()

    def stop_knocking(self) -> None:
        self._stop_knock.set()
        if self._knock_thread:
            self._knock_thread.join(timeout=2.0)
            self._knock_thread = None

    def _knock_loop(self) -> None:
        while not self._stop_knock.wait(1.0):
            self.send(knock(self.triple))

    # the counters line (src/fci_nic.c gate_summary), read and judged

    @staticmethod
    def parse_summary(line: str) -> dict[str, int]:
        """Parse a "gate drops:" counters line into {counter: count}.

        The line is "envelope N, rate cap N, bad frame N (short N, ..., padding N), stranger N,
        passed N" (src/fci_nic.c); every counter by its fci_frame_reason() name, cumulative.
        """
        d: dict[str, int] = {}

        def add(part: str) -> None:
            name, _, val = part.rpartition(" ")
            if val.isdigit():
                d[name] = int(val)

        head, _, rest = line.partition(" (")
        for part in head.split(", "):
            add(part)
        reasons, _, tail = rest.partition(")")  # the reasons are inside the parentheses
        for part in reasons.split(", "):
            add(part)
        for part in tail.split(", "):
            if part.strip():
                add(part)
        return d

    def last_summary(self) -> dict[str, int] | None:
        lines = SUMMARY_RE.findall(self.tail_log())
        if not lines:
            return None
        return self.parse_summary(lines[-1])

    def fresh_summary(self) -> dict[str, int]:
        """Wait past the card's 10 s summary window, cause one drop, and read the counters line.

        The line is written only when a counter changes and the 10 s window has elapsed, so this
        waits 11 s, sends one envelope-garbage datagram (always dropped, so it always changes a
        counter and forces the line), and reads the last one. The trigger adds 1 to the envelope
        count; the caller accounts for it.
        """
        time.sleep(11.0)
        self.send(SUMMARY_TRIGGER)
        time.sleep(1.5)
        s = self.last_summary()
        if s is None:
            self.failures.append("no 'gate drops:' counters line in the card's log")
            return {}
        return s

    # the hostile vectors, phase by phase (send only; the counters line is judged after)

    def phase_gate(self, src_ip: bytes, dst_ip: bytes) -> None:
        """Frames with a valid envelope the gate must reject (the fuzzer is an admitted MAC)."""
        bad = [
            build_frame(FUZZ_MAC, src_ip, dst_ip, eth_len=14),  # too short (14: no IP)
            build_frame(FUZZ_MAC, src_ip, dst_ip, eth_len=20),  # too short (20: no UDP)
            build_frame(FUZZ_MAC, src_ip, dst_ip, eth_type=b"\x08\x06"),  # not IPv4 (ARP)
            build_frame(FUZZ_MAC, src_ip, dst_ip, eth_type=b"\x86\xdd"),  # not IPv4 (IPv6 type)
            build_frame(FUZZ_MAC, src_ip, dst_ip, version=6),  # IP version 6
            build_frame(FUZZ_MAC, src_ip, dst_ip, ihl=15),  # IP header too big (IHL 15)
            build_frame(FUZZ_MAC, src_ip, dst_ip, totlen=500),  # IP total length oversize
            build_frame(FUZZ_MAC, src_ip, dst_ip, totlen=10),  # IP total length short
            build_frame(FUZZ_MAC, src_ip, dst_ip, flags_frag=0x2000),  # fragment (MF set)
            build_frame(FUZZ_MAC, src_ip, dst_ip, flags_frag=100),  # fragment (offset set)
            build_frame(FUZZ_MAC, src_ip, dst_ip, proto=1),  # not UDP (ICMP)
            build_frame(FUZZ_MAC, src_ip, dst_ip, totlen=52, udp_len=100),  # UDP length oversize
            build_frame(FUZZ_MAC, src_ip, dst_ip, totlen=52, udp_len=4),  # UDP length short
            build_frame(FUZZ_MAC, src_ip, dst_ip, eth_len=120),  # excessive padding
        ]
        for data in bad:
            self.send(envelope(data))
            time.sleep(0.05)
        self.notes.append(f"sent {len(bad)} hostile frames (the gate must drop each, by reason)")

    def phase_rate(self, src_ip: bytes, dst_ip: bytes, burst: int) -> None:
        """A burst of valid frames from one MAC: the rate cap must engage (200/s is the cap)."""
        self.send_many(envelope(valid_frame(src_ip, dst_ip)), burst)
        self.notes.append(f"sent a {burst}-frame burst (the rate cap must drop the excess)")

    def phase_envelope(self) -> None:
        """Datagrams the envelope must reject (no FCN1 magic, an unknown kind, out-of-size)."""
        vectors = [
            b"\x00",  # 1-byte garbage
            b"\x01\x02\x03\x04",  # 4-byte garbage
            b"\x00" * 19,  # 19-byte garbage (no magic)
            b"\x00" * 100,  # 100-byte garbage (no magic)
            b"\xff" * 65000,  # 65000-byte garbage (no magic)
            MAGIC + b"X" + b"\x00" * 100,  # unknown kind 'X'
            MAGIC + b"F" + b"\x00" * 13,  # frame too small (13 < 14)
            MAGIC + b"F" + b"\x00" * (ETHER_MAX + 1),  # frame too large (1515 > 1514)
        ]
        for data in vectors:
            self.send(data)
        self.notes.append(f"sent {len(vectors)} hostile datagrams (the envelope must drop each)")

    def phase_valid(self, src_ip: bytes, dst_ip: bytes, n: int) -> None:
        """Slow valid frames: the gate must let them through (well under the 200/s cap)."""
        self.send_many(envelope(valid_frame(src_ip, dst_ip)), n, gap=0.6)
        self.notes.append(f"sent {n} slow valid frames (the gate must pass each)")

    # the judging, from the counters line

    def judge_drops(self, s: dict[str, int]) -> None:
        """The hostile traffic's drops, by counter (cumulative; the baseline's trigger is +1
        envelope). The rate cap is judged on the counter alone: 'passed' also counts any
        concurrent game traffic, so the burst's share of it cannot be isolated.
        """
        if s.get("envelope", 0) < 8:
            self.failures.append(f"envelope counter {s.get('envelope', 0)} < the 8 hostile datagrams sent")
        else:
            self.notes.append(f"envelope drops counted: {s['envelope']} (8 sent + the trigger)")
        if self.mode == "lan":
            if s.get("rate cap", 0) < 1:
                self.failures.append("rate cap counter is 0 after the burst (the cap did not engage)")
            else:
                self.notes.append(
                    f"rate cap engaged: {s['rate cap']} frame(s) dropped (passed so far: {s.get('passed', 0)})"
                )
            for reason, want in GATE_EXPECT.items():
                got = s.get(reason, 0)
                if got < want:
                    self.failures.append(f"bad frame '{reason}': counter {got} < the {want} sent")
            self.notes.append(
                f"bad frame drops counted: {s.get('bad frame', 0)} "
                f"({sum(GATE_EXPECT.values())} sent, {len(GATE_EXPECT)} reasons)"
            )

    def judge_valid(self, before: dict[str, int], after: dict[str, int], n: int) -> None:
        """The slow valid frames must pass: 'passed' rises by at least n, and no other counter
        rises ("after"'s envelope is 1 higher than "before"'s, the second trigger). A valid frame
        the gate dropped would raise the rate cap, a bad-frame reason, or stranger.
        """
        dp = after.get("passed", 0) - before.get("passed", 0)
        if dp < n:
            self.failures.append(f"'passed' rose by {dp} < the {n} valid frames sent (the gate dropped some)")
        for k in before:
            if k == "passed":
                continue
            want = before[k] + (1 if k == "envelope" else 0)
            if after.get(k, 0) != want:
                self.failures.append(
                    f"counter '{k}' changed by the valid frames: {before[k]} -> {after.get(k, 0)} (want {want})"
                )
        if not self.failures:
            self.notes.append(f"{n} slow valid frames passed ('passed' +{dp}, no other counter raised)")

    def finish(self, send_only: bool) -> int:
        # the summary
        print(f"net_fuzz: sent {self.sent} datagram(s) at {self.target[0]}:{self.target[1]}")
        for note in self.notes:
            print(f"  {note}")
        if self.failures:
            for f in self.failures:
                print(f"  FAIL: {f}")
            return 1
        if send_only:
            print("  sent only (no log judged)")
        else:
            print("  ok: the card survived and dropped what it should")
        return 0

    def run(self, burst: int, valid_n: int, send_only: bool) -> int:
        self.log_start = len(self.log.read_text(errors="replace")) if self.log else 0
        src_ip = socket.inet_aton(socket.gethostbyname(socket.gethostname()) or "127.0.0.1")
        dst_ip = socket.inet_aton(self.target[0])
        self.notes.append(f"target {self.target[0]}:{self.target[1]}, mode {self.mode}")
        try:
            if self.mode == "lan":
                if not self.admit():
                    self.notes.append("NOT admitted: check --triple (title:versionhex:proto)")
                    return 2
                self.start_knocking()  # keep the MAC admitted for the whole run (the 5 s expiry)
                self.phase_gate(src_ip, dst_ip)
                self.phase_rate(src_ip, dst_ip, burst)
                time.sleep(1.0)  # let the card drain the burst: a 65000-byte datagram needs the 64 KB socket buffer free
            self.phase_envelope()
            if not self.log or send_only:
                return self.finish(send_only)
            baseline = self.fresh_summary()
            self.judge_drops(baseline)
            if self.mode == "lan":
                self.phase_valid(src_ip, dst_ip, valid_n)
                after = self.fresh_summary()
                self.judge_valid(baseline, after, valid_n)
            tail = self.tail_log()
            if re.search(r"\[FCI\] (STOP|crash|assert)", tail, re.IGNORECASE):
                self.failures.append("the card logged a STOP/crash line during the fuzz")
            return self.finish(send_only)
        finally:
            self.stop_knocking()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", required=True, help="the card's ip:port (its UDP port)")
    ap.add_argument("--mode", choices=["lan", "list"], required=True)
    ap.add_argument(
        "--triple",
        default="1:00000000:2",
        help="title:versionhex:proto for the LAN knock (default 1:00000000:2; the version is "
        "the XBE certificate's 4-byte Version field, little-endian hex)",
    )
    ap.add_argument("--log", type=Path, default=None, help="the card's stderr file (for judging)")
    ap.add_argument("--burst", type=int, default=300, help="rate-burst size (default 300)")
    ap.add_argument("--valid", type=int, default=10, help="slow valid frames (default 10)")
    ap.add_argument("--send-only", action="store_true", help="send without judging a log")
    args = ap.parse_args()

    host, _, port = args.target.rpartition(":")
    if not host or not port.isdigit():
        print("net_fuzz: --target must be ip:port", file=sys.stderr)
        return 3
    title, _, rest = args.triple.partition(":")
    version, _, proto = rest.partition(":")
    try:
        triple = bytes([int(title, 16)]) + int(version, 16).to_bytes(4, "little") + bytes([int(proto, 16)])
    except ValueError:
        print("net_fuzz: --triple must be title:versionhex:proto (hex bytes)", file=sys.stderr)
        return 3
    fuzz = Fuzzer((host, int(port)), args.mode, triple, args.log)
    try:
        return fuzz.run(args.burst, args.valid, args.send_only)
    except OSError as e:
        print(f"net_fuzz: network error: {e}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
