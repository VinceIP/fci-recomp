"""The LAN transport's portable logic, mirrored and pinned against real inputs.

The transport itself is C (src/fci_nic.c) and only runs on Windows/Proton, so the sandbox test
pins the *algorithm* the C must match, the way ``test_stun_vectors.py`` pins the STUN wire format.
A reviewer reads each test against the named C function; if the C and the mirror diverge, the one-PC
test (multiplayer.md) is the thing that catches it.

Mirrored:
- ``subnet_broadcast``      <- ``lan_adapters_refresh`` (the (ip & mask) | ~mask broadcast)
- ``adapter_broadcasts``    <- ``lan_adapters_refresh`` (skip loopback, link-local last-resort, dedup)
- ``learn``                 <- ``lan_learn`` (de-duplicate by MAC, the latest source wins, full table)
- ``bcports``               <- ``fci_nic_start_lan`` (RECOMP_FCI_NET_LAN_BCPORTS, default the bind port)
"""

import ipaddress

# --- the subnet broadcast ------------------------------------------------------


def subnet_broadcast(ip, mask):
    """The subnet broadcast for one (ip, mask) pair, as a string (the C: bc = (h & m) | ~m)."""
    h = int(ipaddress.IPv4Address(ip))
    m = int(ipaddress.IPv4Address(mask))
    return str(ipaddress.IPv4Address((h & m) | (~m & 0xFFFFFFFF)))


def test_subnet_broadcast_real_subnets():
    assert subnet_broadcast("192.168.1.5", "255.255.255.0") == "192.168.1.255"
    assert subnet_broadcast("10.0.0.1", "255.0.0.0") == "10.255.255.255"
    assert subnet_broadcast("172.16.4.9", "255.255.0.0") == "172.16.255.255"
    # a /31 point-to-point: (ip & mask) | ~mask is the top of the two-address pair, from either end
    assert subnet_broadcast("192.168.5.1", "255.255.255.254") == "192.168.5.1"
    assert subnet_broadcast("192.168.5.0", "255.255.255.254") == "192.168.5.1"
    # a /32: the address itself
    assert subnet_broadcast("192.168.9.7", "255.255.255.255") == "192.168.9.7"


# --- the adapter filter --------------------------------------------------------


def _link_local(ip):
    """169.254/16 (the C: (h & 0xFFFF0000) == 0xA9FE0000)."""
    a = int(ipaddress.IPv4Address(ip))
    return (a & 0xFFFF0000) == 0xA9FE0000


def adapter_broadcasts(adapters):
    """The send list for a list of adapters.

    ``adapters`` is a list of ``(name, ip, mask, is_loopback)`` in the order ``GetAdaptersInfo``
    returns them. Returns the de-duplicated broadcast addresses, in first-seen order, applying the
    C's rules: drop loopback; drop a down adapter (0.0.0.0 address or 0 mask, which would give the
    255.255.255.255 broadcast); a 169.254/16 address is a last resort (only the first one) used when
    nothing else is up; otherwise the subnet broadcast is added, de-duplicated.
    """
    out = []
    llbc = None
    for _name, ip, mask, loop in adapters:
        if loop:
            continue
        h = int(ipaddress.IPv4Address(ip))
        m = int(ipaddress.IPv4Address(mask))
        if not h or not m:  # a down adapter reports 0.0.0.0/0: no broadcast to send to
            continue
        bc = subnet_broadcast(ip, mask)
        if _link_local(ip):
            if llbc is None:
                llbc = bc
            continue
        if bc not in out:
            out.append(bc)
    if not out and llbc is not None:
        out.append(llbc)
    return out


def test_adapter_drops_loopback():
    ads = [
        ("Loopback", "127.0.0.1", "255.0.0.0", True),
        ("Ethernet", "192.168.1.5", "255.255.255.0", False),
    ]
    assert adapter_broadcasts(ads) == ["192.168.1.255"]


def test_adapter_skips_down():
    # a down/disconnected adapter reports 0.0.0.0 with a 0 mask: it would give the 255.255.255.255
    # broadcast, so it is skipped (the 5-of-8 adapters in the run log)
    ads = [
        ("Down 1", "0.0.0.0", "0.0.0.0", False),
        ("Down 2", "0.0.0.0", "0.0.0.0", False),
        ("Ethernet", "192.168.1.5", "255.255.255.0", False),
    ]
    assert adapter_broadcasts(ads) == ["192.168.1.255"]


def test_adapter_only_down_is_empty():
    # nothing up, only down adapters: no broadcast addresses at all
    assert adapter_broadcasts([("Down", "0.0.0.0", "0.0.0.0", False)]) == []


def test_adapter_dedups_shared_subnet():
    # two adapters on the same subnet (a bridge): one broadcast address, not two
    ads = [
        ("Ethernet", "192.168.1.5", "255.255.255.0", False),
        ("Wi-Fi", "192.168.1.9", "255.255.255.0", False),
    ]
    assert adapter_broadcasts(ads) == ["192.168.1.255"]


def test_adapter_link_local_is_last_resort():
    # a real up adapter wins over a link-local one
    ads = [
        ("Wi-Fi (APTELLINK)", "169.254.3.4", "255.255.0.0", False),
        ("Ethernet", "10.0.0.2", "255.0.0.0", False),
    ]
    assert adapter_broadcasts(ads) == ["10.255.255.255"]


def test_adapter_link_local_only():
    # no other adapter: the link-local broadcast is used (the C keeps the first one)
    ads = [
        ("Wi-Fi (APTELLINK)", "169.254.3.4", "255.255.0.0", False),
        ("Wi-Fi 2 (APTELLINK)", "169.254.8.9", "255.255.0.0", False),
    ]
    assert adapter_broadcasts(ads) == ["169.254.255.255"]


def test_adapter_none_up():
    assert adapter_broadcasts([]) == []
    assert adapter_broadcasts([("Loopback", "127.0.0.1", "255.0.0.0", True)]) == []


# --- MAC learning --------------------------------------------------------------

PEERS_MAX = 32  # src/fci_nic.c: the table is 32 entries, twice the game's 16-player cap
LAN_MOVE_MS = 3000  # src/fci_nic.c: move to a new source only after 3 s of silence from the current one


def learn(table, mac, addr, now):
    """One ``lan_learn`` step. ``table`` is a list of (mac, addr, last_heard) or None, length PEERS_MAX.

    ``now`` is the current time in ms. Returns the entry index, or None when the table is full.
    De-duplicates by MAC. A frame from the learned endpoint keeps it alive (updates last_heard). A
    frame from a *different* source does not move the entry while the endpoint is alive (last frame
    within LAN_MOVE_MS); it moves only after LAN_MOVE_MS of silence from the current endpoint (an
    adapter change). A new MAC takes the first free entry.
    """
    for i, e in enumerate(table):
        if e is not None and e[0] == mac:
            if e[1] == addr:
                table[i] = (mac, addr, now)  # the endpoint is alive: keep it
            elif now - e[2] >= LAN_MOVE_MS:
                table[i] = (mac, addr, now)  # quiet for 3 s: the card moved, the new source wins
            # else: alive, a frame from another adapter: keep it, no move
            return i
    for i, e in enumerate(table):
        if e is None:
            table[i] = (mac, addr, now)
            return i
    return None


def test_learn_dedups_by_mac():
    t = [None] * PEERS_MAX
    assert learn(t, "AA", "1.1.1.1:1", 0) == 0
    # the same MAC from the same source: same entry, no new one
    assert learn(t, "AA", "1.1.1.1:1", 1000) == 0
    assert sum(e is not None for e in t) == 1


def test_learn_keeps_endpoint_while_alive():
    # a sender broadcasts on every adapter: the same MAC arrives from several source addresses.
    # while the learned endpoint is still sending, the entry does not move (no flapping).
    t = [None] * PEERS_MAX
    learn(t, "BB", "192.168.1.144:1", 0)
    assert learn(t, "BB", "192.168.5.1:1", 1000) == 0  # a second adapter, 1 s in: no move
    assert t[0][1] == "192.168.1.144:1"
    assert learn(t, "BB", "192.168.32.1:1", 2000) == 0  # a third, 2 s in: still no move
    assert t[0][1] == "192.168.1.144:1"


def test_learn_moves_after_silence():
    # the learned endpoint stops sending; after 3 s of silence a new source takes over.
    t = [None] * PEERS_MAX
    learn(t, "CC", "192.168.1.144:1", 0)
    # 2.9 s of silence from the endpoint, then a frame from a new address: still alive, no move
    assert learn(t, "CC", "192.168.5.1:1", 2900) == 0
    assert t[0][1] == "192.168.1.144:1"
    # 3.1 s of silence: the endpoint is gone, the new source wins
    assert learn(t, "CC", "192.168.5.1:1", 3100) == 0
    assert t[0][1] == "192.168.5.1:1"
    assert t[0][2] == 3100


def test_learn_full_table():
    t = [("M%d" % i, "1.1.1.1:%d" % i, 0) for i in range(PEERS_MAX)]
    assert learn(t, "NEW", "9.9.9.9:9", 0) is None


# --- the broadcast destination ports ------------------------------------------


def bcports(spec, bind_port):
    """RECOMP_FCI_NET_LAN_BCPORTS -> the destination ports (the C: default {bind_port}, max 8,
    an out-of-range token is skipped)."""
    out = []
    if spec:
        for tok in spec.split(","):
            try:  # the C uses atoi: a non-number token is 0 and skipped
                p = int(tok)
            except ValueError:
                p = 0
            if 0 < p <= 65535:
                out.append(p)
    if not out:
        out = [bind_port]
    return out[:8]


def test_bcports_defaults_to_bind_port():
    assert bcports(None, 47920) == [47920]
    assert bcports("", 47920) == [47920]


def test_bcports_explicit_list():
    # the one-PC test: two instances, each broadcasts to both ports so they find each other
    assert bcports("47920,47921", 47920) == [47920, 47921]
    assert bcports("47920,47921", 47921) == [47920, 47921]


def test_bcports_skips_bad_tokens():
    assert bcports("47920,0,70000,47921", 47920) == [47920, 47921]


def test_bcports_caps_at_eight():
    assert bcports(",".join(str(47900 + i) for i in range(12)), 47920) == [47900 + i for i in range(8)]
