"""The LAN transport's portable logic, mirrored and pinned against real inputs.

The transport itself is C (src/fci_nic.c) and only runs on Windows/Proton, so the sandbox test
pins the *algorithm* the C must match, the way ``test_stun_vectors.py`` pins the STUN wire format.
A reviewer reads each test against the named C function; if the C and the mirror diverge, the one-PC
test (multiplayer.md) is the thing that catches it.

Mirrored:
- ``subnet_broadcast``      <- ``lan_adapters_refresh`` (the (ip & mask) | ~mask broadcast)
- ``adapter_broadcasts``    <- ``lan_adapters_refresh`` (skip loopback, link-local last-resort, dedup)
- ``triple`` / ``triples_match`` <- ``net_triple_fill`` / ``net_triple_ok`` (title, XBE version, net protocol)
- ``admit`` / ``frame_seen`` <- ``lan_admit`` / ``lan_frame_seen`` (the knock admits a MAC; a frame is
  dropped unless the MAC is admitted and its knocks run; the endpoint moves after 3 s of silence)
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


# --- the compatibility triple and the knock (q21) -------------------------------

PEERS_MAX = 32  # src/fci_nic.c: the table is 32 entries, twice the game's 16-player cap
LAN_MOVE_MS = 3000  # src/fci_nic.c: move to a new source only after 3 s of silence from the current one
LAN_KNOCK_MS = 1000  # src/fci_nic.c: the knock, every second
LAN_KNOCK_EXPIRE_MS = 5000  # src/fci_nic.c: a card quiet for 5 s is no longer admitted


def triple(title, version, proto):
    """The 6-byte triple every hello and knock carries (``net_triple_fill``): the title
    (0 = instincts, 1 = evolution), the disc's version (the XBE certificate's Version), and the net
    protocol (src/fci_net.h)."""
    return bytes([1 if title == "evolution" else 0]) + version.to_bytes(4, "little") + bytes([proto])


def triples_match(a, b):
    """``net_triple_ok``: a peer is admitted only when all three of title, version and net protocol
    are equal. Any one differing is a mismatch (not admitted, one rate-limited log line)."""
    return a == b


def test_triple_is_title_version_proto():
    assert triple("instincts", 3, 2) == bytes([0, 3, 0, 0, 0, 2])
    assert triple("evolution", 3, 2) == bytes([1, 3, 0, 0, 0, 2])
    assert triple("evolution", 0x01020304, 2) == bytes([1, 4, 3, 2, 1, 2])
    assert triples_match(triple("instincts", 3, 2), triple("instincts", 3, 2))
    # any one of the three differing is a mismatch
    assert not triples_match(triple("instincts", 3, 2), triple("evolution", 3, 2))
    assert not triples_match(triple("instincts", 3, 2), triple("instincts", 4, 2))
    assert not triples_match(triple("instincts", 3, 2), triple("instincts", 3, 1))


def room_scope(room, triple_, roomlen=33):
    """``room_scope`` in fci_nic.c: the room name the card sends to the hub — "<room>~<the triple in
    hex>" (12 hex chars). The hub keys its rooms by the name and never relays hellos, so a
    mismatched build lands in a different room and never sees this one. The user's room is cut so
    the whole name fits roomlen - 1 chars (32, the hub's limit). Returns the name and whether the
    room was cut."""
    hex_ = "".join("%02x" % c for c in triple_)
    keep = roomlen - 1 - 1 - len(hex_)
    cut = len(room) > keep
    return room[:keep] + "~" + hex_, cut


def test_room_name_is_scoped_by_the_triple():
    name, cut = room_scope("lobby", triple("instincts", 3, 2))
    assert name == "lobby~000300000002" and not cut
    # a mismatched build gets a different room name: it lands in a different room
    other, _ = room_scope("lobby", triple("instincts", 3, 1))
    assert name != other
    assert len(name) <= 32


def test_room_name_cuts_the_room_to_fit():
    # a 32-char room (the hub's limit) is cut to 19: the name is 19 + '~' + 12 = 32
    name, cut = room_scope("a" * 32, triple("instincts", 3, 2))
    assert cut and name == "a" * 19 + "~000300000002" and len(name) == 32
    # the relay room (the peer's 32-char nonce in hex) is cut the same way, identically on both sides
    nonce = "0123456789abcdef0123456789abcdef"
    a, _ = room_scope(nonce, triple("instincts", 3, 2))
    b, _ = room_scope(nonce, triple("instincts", 3, 2))
    assert a == b == nonce[:19] + "~000300000002"


# the table is a list of (mac, addr, last_heard, last_knock) or None, length PEERS_MAX


def admit(table, mac, addr, now):
    """One ``lan_admit`` step: a knock (its triple checked by the caller) admits the card's MAC and
    records its endpoint, de-duplicated by MAC. A knock from a known MAC refreshes its admission
    and, if it comes from a new address, moves the endpoint (the knock comes from the card's live
    adapter). Returns the entry index, or None when the table is full."""
    for i, e in enumerate(table):
        if e is not None and e[0] == mac:
            if e[1] != addr:
                table[i] = (mac, addr, now, now)  # the knock comes from the card's live adapter
            else:
                table[i] = (mac, e[1], e[2], now)  # the endpoint is alive: keep it, refresh the knock
            return i
    for i, e in enumerate(table):
        if e is None:
            table[i] = (mac, addr, now, now)
            return i
    return None


def frame_seen(table, mac, addr, now):
    """One ``lan_frame_seen`` step: a frame reaches the guest only from a MAC admitted by a knock,
    while its knocks run (LAN_KNOCK_EXPIRE_MS). The endpoint is kept while it is alive; it moves to
    a new source only after LAN_MOVE_MS of silence from the current one (an adapter change). Returns
    the entry index, or None (the frame is dropped)."""
    for i, e in enumerate(table):
        if e is not None and e[0] == mac:
            mac_, addr_, last_heard, last_knock = e
            if now - last_knock >= LAN_KNOCK_EXPIRE_MS:
                return None  # the card's knocks have stopped: no longer admitted
            if addr_ == addr:
                table[i] = (mac_, addr_, now, last_knock)  # the endpoint is alive: keep it
            elif now - last_heard >= LAN_MOVE_MS:
                table[i] = (mac_, addr, now, last_knock)  # quiet for 3 s: the card moved
            # else: alive, a frame from another adapter: keep it, no move
            return i
    return None


def test_admit_dedups_by_mac():
    t = [None] * PEERS_MAX
    assert admit(t, "AA", "1.1.1.1:1", 0) == 0
    # the same MAC knocking again from the same source: same entry, no new one
    assert admit(t, "AA", "1.1.1.1:1", 1000) == 0
    assert sum(e is not None for e in t) == 1


def test_admit_full_table():
    t = [("M%d" % i, "1.1.1.1:%d" % i, 0, 0) for i in range(PEERS_MAX)]
    assert admit(t, "NEW", "9.9.9.9:9", 0) is None


def test_frame_from_unknocked_mac_is_dropped():
    # a frame from a MAC that never knocked is not admitted (the old build learned it from its
    # first frame; now the knock is the admission)
    t = [None] * PEERS_MAX
    assert frame_seen(t, "AA", "1.1.1.1:1", 0) is None
    assert sum(e is not None for e in t) == 0
    # the knock admits it, and then its frames are seen
    assert admit(t, "AA", "1.1.1.1:1", 1000) == 0
    assert frame_seen(t, "AA", "1.1.1.1:1", 2000) == 0


def test_frame_dropped_after_knock_silence():
    # a card quiet for 5 s (four lost knocks) is no longer admitted, even if its frames arrive
    t = [None] * PEERS_MAX
    admit(t, "BB", "192.168.1.144:1", 0)
    frame_seen(t, "BB", "192.168.1.144:1", 4999)
    assert frame_seen(t, "BB", "192.168.1.144:1", 5000) is None
    # a fresh knock re-admits it
    assert admit(t, "BB", "192.168.1.144:1", 6000) == 0
    assert frame_seen(t, "BB", "192.168.1.144:1", 6100) == 0


def test_frame_keeps_endpoint_while_alive():
    # a sender broadcasts on every adapter: the same MAC arrives from several source addresses.
    # while the admitted endpoint is still sending, the entry does not move (no flapping).
    t = [None] * PEERS_MAX
    admit(t, "CC", "192.168.1.144:1", 0)
    assert frame_seen(t, "CC", "192.168.5.1:1", 1000) == 0  # a second adapter, 1 s in: no move
    assert t[0][1] == "192.168.1.144:1"
    assert frame_seen(t, "CC", "192.168.32.1:1", 2000) == 0  # a third, 2 s in: still no move
    assert t[0][1] == "192.168.1.144:1"


def test_frame_moves_after_silence():
    # the admitted endpoint stops sending; after 3 s of silence a new source takes over.
    t = [None] * PEERS_MAX
    admit(t, "DD", "192.168.1.144:1", 0)
    # 2.9 s of silence from the endpoint, then a frame from a new address: still alive, no move
    assert frame_seen(t, "DD", "192.168.5.1:1", 2900) == 0
    assert t[0][1] == "192.168.1.144:1"
    # 3.1 s of silence: the endpoint is gone, the new source wins
    assert frame_seen(t, "DD", "192.168.5.1:1", 3100) == 0
    assert t[0][1] == "192.168.5.1:1"
    assert t[0][2] == 3100


def test_knock_from_new_address_moves_endpoint():
    # the knock comes from the card's live adapter: a knock from a new address moves the endpoint
    t = [None] * PEERS_MAX
    admit(t, "EE", "192.168.1.144:1", 0)
    admit(t, "EE", "192.168.5.1:1", 1000)
    assert t[0][1] == "192.168.5.1:1"


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
