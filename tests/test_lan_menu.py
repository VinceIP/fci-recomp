"""The online menu gate and relabel, mirrored and pinned against the C (src/fci_lan_menu.c, src/fci_nic.c).

The gate and the relabel are C (Windows/Proton only), so the sandbox test pins the *algorithm* the C
must match, the way test_lan.py pins the LAN transport's logic. A reviewer reads each test against the
named C function; if the C and the mirror diverge, the one-PC test (multiplayer.md) catches it.

Mirrored:
- ``action_crc``           <- the CRC-32 of the action name (sub_0026ACB2, poly 0xEDB88320)
- ``gate_pick``            <- ``fci_gate_000FB0A2`` (the B_Live/B_SystemLink pick, the hash rewrite)
- ``pick_state_machine``   <- ``fci_nic_start_menu_transport`` (first pick wins until restart)
- ``relabel_entry``        <- ``relabel_entry`` (the "XBOX LIVE" -> "INTERNET" UTF-16 replacement
                               in the string table entry, sub_0026A628 posthook)
"""

import zlib

# --- the action CRC-32 (sub_0026ACB2, poly 0xEDB88320) ------------------------


def action_crc(name: str) -> int:
    """The CRC-32 of the ASCII action name, as the game computes it (sub_0026ACB2)."""
    return zlib.crc32(name.encode("ascii")) & 0xFFFFFFFF


def test_action_crc_known_values():
    # The dispatch table (recomp_0023.c:27569): the B_Live/B_SystemLink branches.
    assert action_crc("B_Live") == 0x9B680E53
    assert action_crc("B_SystemLink") == 0xF65128AA
    # The other items (for reference; the gate only acts on B_Live/B_SystemLink).
    assert action_crc("B_Multiplayer") == 0x344E8201
    assert action_crc("B_SplitScreen") == 0x8FA9611F
    assert action_crc("B_StoryMode") == 0x17130B2F


# --- the gate's pick logic (fci_gate_000FB0A2) --------------------------------

# The redirect: B_Live -> B_SystemLink (the System Link lobby branch, the redirect target).
REDIRECT_TO = 0xF65128AA  # B_SystemLink


def gate_pick(hash_: int, menu_mode: bool, transport_up: bool) -> dict:
    """Mirror of fci_gate_000FB0A2: the pick logic.

    Returns a dict with:
      - "transport": the transport started ("lan", "list", or None)
      - "new_hash": the hash after the gate (rewritten for B_Live, unchanged otherwise)
      - "skip": 1 if the gate skips the body, 0 if the body runs (always 0 here)
    """
    if not menu_mode:
        return {"transport": None, "new_hash": hash_, "skip": 0}
    if transport_up:
        # "First choice wins until restart": a second pick logs and no-ops.
        return {"transport": None, "new_hash": hash_, "skip": 0}
    if hash_ == 0xF65128AA:  # B_SystemLink -> LAN
        return {"transport": "lan", "new_hash": hash_, "skip": 0}
    if hash_ == 0x9B680E53:  # B_Live -> LIST, rewrite the hash to B_SystemLink
        return {"transport": "list", "new_hash": REDIRECT_TO, "skip": 0}
    return {"transport": None, "new_hash": hash_, "skip": 0}


def test_gate_system_link():
    # B_SystemLink in menu mode: starts the LAN transport, the hash is unchanged, the body runs.
    r = gate_pick(0xF65128AA, menu_mode=True, transport_up=False)
    assert r["transport"] == "lan"
    assert r["new_hash"] == 0xF65128AA
    assert r["skip"] == 0


def test_gate_live_redirect():
    # B_Live in menu mode: starts the LIST transport, the hash is rewritten to B_SystemLink, the body
    # runs (the dispatcher re-reads the hash and takes the System Link lobby branch).
    r = gate_pick(0x9B680E53, menu_mode=True, transport_up=False)
    assert r["transport"] == "list"
    assert r["new_hash"] == 0xF65128AA  # rewritten to B_SystemLink
    assert r["skip"] == 0


def test_gate_not_menu_mode():
    # Not menu mode: the gate is inert, the original Xbox Live / System Link runs.
    r = gate_pick(0x9B680E53, menu_mode=False, transport_up=False)
    assert r["transport"] is None
    assert r["new_hash"] == 0x9B680E53
    assert r["skip"] == 0


def test_gate_other_item():
    # Other items (e.g. B_Multiplayer): the gate is inert, the body runs.
    r = gate_pick(0x344E8201, menu_mode=True, transport_up=False)
    assert r["transport"] is None
    assert r["new_hash"] == 0x344E8201
    assert r["skip"] == 0


# --- the pick state machine (fci_nic_start_menu_transport) --------------------


def pick_state_machine(picks: list, url_set: bool) -> list:
    """Mirror of fci_nic_start_menu_transport: the "first pick wins until restart" logic.

    picks: a list of mode ints (0 = LAN, 1 = LIST).
    url_set: 1 when the LIST URL is set (the INTERNET transport can start).
    Returns a list of results: "started", "already-up", "no-url", or "inert".
    """
    transport_up = False
    results = []
    for mode in picks:
        if transport_up:
            results.append("already-up")
            continue
        if mode == 0:  # LAN
            transport_up = True
            results.append("started")
        else:  # LIST
            if not url_set:
                # No URL: the Q4 log line, no socket (s_up stays 1, the browser is empty).
                results.append("no-url")
            else:
                transport_up = True
                results.append("started")
    return results


def test_first_pick_wins_lan():
    # LAN first, then LIST: the LAN transport starts, the LIST pick is ignored.
    assert pick_state_machine([0, 1], url_set=True) == ["started", "already-up"]


def test_first_pick_wins_list():
    # LIST first, then LAN: the LIST transport starts, the LAN pick is ignored.
    assert pick_state_machine([1, 0], url_set=True) == ["started", "already-up"]


def test_list_no_url():
    # LIST with no URL: no transport starts (the Q4 log line), a later LAN pick starts.
    assert pick_state_machine([1, 0], url_set=False) == ["no-url", "started"]


def test_list_no_url_then_list():
    # LIST with no URL, then LIST again: both are no-ops (no transport), a later LAN pick starts.
    assert pick_state_machine([1, 1, 0], url_set=False) == ["no-url", "no-url", "started"]


# --- the relabel (relabel_entry, sub_0026A628 posthook) -----------------------

# The UTF-16LE byte sequences (the C: s_from_utf16, s_to_utf16 in fci_lan_menu.c).
FROM_UTF16 = "XBOX LIVE"
TO_UTF16 = "INTERNET"
SCAN_UNITS = 64  # 128 bytes / 2 (the C: scan_units in relabel_entry)


def relabel_entry(buf: bytes) -> tuple[bytes, bool]:
    """Mirror of relabel_entry: scan a UTF-16LE buffer for "XBOX LIVE" and replace with "INTERNET".

    buf: the entry structure (up to 128 bytes = 64 uint16_t).
    Returns (new_buf, found): the buffer with the first match replaced (null-padded),
    and whether a match was found.
    """
    from_bytes = FROM_UTF16.encode("utf-16-le")
    to_bytes = TO_UTF16.encode("utf-16-le")
    # Null-pad the replacement to the same length as the original
    padded_to = to_bytes + b"\x00\x00" * ((len(from_bytes) - len(to_bytes)) // 2)

    data = buf[: SCAN_UNITS * 2]  # scan at most 128 bytes
    idx = data.find(from_bytes)
    if idx < 0:
        return buf, False
    # Replace at idx
    new_data = data[:idx] + padded_to + data[idx + len(from_bytes) :]
    return new_data + buf[SCAN_UNITS * 2 :], True


def test_relabel_entry_xbox_live():
    # "XBOX LIVE" at the start of the entry: replaced with "INTERNET" (null-padded).
    entry = "XBOX LIVE\0".encode("utf-16-le") + b"\x00" * 120
    result, found = relabel_entry(entry)
    assert found
    assert result.startswith("INTERNET\0\0".encode("utf-16-le"))


def test_relabel_entry_offset():
    # "XBOX LIVE" at an offset within the entry (e.g. after the CRC-32 at +4): replaced.
    # Simulate: 2 dwords of header (8 bytes), then the text.
    header = b"\x53\x0e\x68\x9b\x00\x00\x00\x00"  # CRC-32 + padding
    entry = header + "XBOX LIVE\0".encode("utf-16-le") + b"\x00" * 100
    result, found = relabel_entry(entry)
    assert found
    assert "INTERNET".encode("utf-16-le") in result
    assert "XBOX LIVE".encode("utf-16-le") not in result


def test_relabel_entry_idempotent():
    # A second relabel finds nothing (the text is already "INTERNET").
    entry = "XBOX LIVE\0".encode("utf-16-le") + b"\x00" * 120
    once, found1 = relabel_entry(entry)
    assert found1
    twice, found2 = relabel_entry(once)
    assert not found2
    assert twice == once


def test_relabel_entry_no_match():
    # A string that doesn't match: unchanged.
    entry = "SYSTEM LINK\0".encode("utf-16-le") + b"\x00" * 120
    result, found = relabel_entry(entry)
    assert not found
    assert result == entry


def test_relabel_entry_not_in_scan_range():
    # "XBOX LIVE" beyond the 128-byte scan range: not found.
    entry = b"\x00" * 128 + "XBOX LIVE\0".encode("utf-16-le")
    result, found = relabel_entry(entry)
    assert not found
    assert result == entry
