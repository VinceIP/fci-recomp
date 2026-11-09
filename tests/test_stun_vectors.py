"""Byte-exact RFC 5769 test vectors for the STUN wire format in ``fake_turn``.

These pin MESSAGE-INTEGRITY (RFC 8489 14.5), FINGERPRINT (14.7) and
XOR-MAPPED-ADDRESS (14.2) against the published vectors. A stand-in and its own
test client can share a protocol bug (e.g. a wrong attribute type, a hash computed
over the wrong byte range, or an address XORed with the wrong half of the magic
cookie) and still agree with each other; the published vectors catch that.
"""

import struct

import fake_turn as ft

MAGIC = ft.MAGIC_BYTES


def _msg(mtype, txn, body):
    return struct.pack("!HH", mtype, len(body)) + MAGIC + txn + body


# --- RFC 5769 2.1: Sample Request (short-term) -------------------------------
# Username "evtj:h6vY", password "VOkJxbRl1RmTxUk/WvJxBt".
V21_TXN = bytes.fromhex("b7e7a701bc34d686fa87dfae")
V21_BODY = bytes.fromhex(
    "802200105354554e207465737420636c69656e74"  # SOFTWARE "STUN test client"
    "002400046e0001ff"  # PRIORITY
    "80290008932ff9b151263b36"  # ICE-CONTROLLED
    "000600096576746a3a68367659202020"  # USERNAME "evtj:h6vY" + 3 pad
)
V21_FULL = bytes.fromhex(
    "00010058"
    "2112a442"
    + V21_TXN.hex()
    + V21_BODY.hex()
    + "000800149aeaa70cbfd8cb56781ef2b5b2d3f249c1b571a2"  # MESSAGE-INTEGRITY
    + "80280004e57a3bcf"  # FINGERPRINT
)
V21_PASSWORD = b"VOkJxbRl1RmTxUk/WvJxBt"


def test_rfc5769_2_1_short_term_roundtrip():
    m = ft.add_fingerprint(ft.add_integrity(_msg(0x0001, V21_TXN, V21_BODY), V21_PASSWORD))
    assert m == V21_FULL


def test_rfc5769_2_1_verify():
    ok, bad_after, has_fp = ft.verify_integrity(V21_PASSWORD, V21_FULL)
    assert (ok, bad_after, has_fp) == (True, False, True)
    assert ft.verify_fingerprint(V21_FULL) is True


# --- RFC 5769 2.2: Sample IPv4 Response --------------------------------------
# Software "test vector", mapped address 192.0.2.1 port 32853.
V22_TXN = bytes.fromhex("b7e7a701bc34d686fa87dfae")
V22_SOFTWARE = bytes.fromhex("8022000b7465737420766563746f7220")  # "test vector" + 1 pad
V22_XMA = bytes.fromhex("0001a147e112a643")  # XOR-MAPPED-ADDRESS 192.0.2.1:32853
V22_FULL = bytes.fromhex(
    "0101003c"
    "2112a442"
    + V22_TXN.hex()
    + V22_SOFTWARE.hex()
    + "00200008"
    + V22_XMA.hex()
    + "000800142b91f599fd9e90c38c7489f92af9ba53f06be7d7"  # MESSAGE-INTEGRITY
    + "80280004c07d4c96"  # FINGERPRINT
)


def test_rfc5769_2_2_ipv4_roundtrip():
    xma = ft.enc_addr(1, "192.0.2.1", 32853, V22_TXN, True)
    assert xma == V22_XMA
    body = V22_SOFTWARE + ft.enc_attr(0x0020, xma)
    m = ft.add_fingerprint(ft.add_integrity(_msg(0x0101, V22_TXN, body), V21_PASSWORD))
    assert m == V22_FULL


def test_rfc5769_2_2_xor_mapped_address():
    assert ft.dec_addr(V22_XMA, V22_TXN, True) == ("192.0.2.1", 32853)


# --- RFC 5769 2.3: Sample IPv6 Response --------------------------------------
# Mapped address 2001:db8:1234:5678:11:2233:4455:6677 port 32853.
V23_TXN = bytes.fromhex("b7e7a701bc34d686fa87dfae")
V23_XMA = bytes.fromhex("0002a1470113a9faa5d3f179bc25f4b5bed2b9d9")
V23_FULL = bytes.fromhex(
    "01010048"
    "2112a442"
    + V23_TXN.hex()
    + V22_SOFTWARE.hex()
    + "00200014"
    + V23_XMA.hex()
    + "00080014a382954e4be67bf11784c97c8292c275bfe3ed41"  # MESSAGE-INTEGRITY
    + "80280004c8fb0b4c"  # FINGERPRINT
)


def test_rfc5769_2_3_ipv6_roundtrip():
    xma = ft.enc_addr(2, "2001:db8:1234:5678:11:2233:4455:6677", 32853, V23_TXN, True)
    assert xma == V23_XMA
    body = V22_SOFTWARE + ft.enc_attr(0x0020, xma)
    m = ft.add_fingerprint(ft.add_integrity(_msg(0x0101, V23_TXN, body), V21_PASSWORD))
    assert m == V23_FULL


def test_rfc5769_2_3_xor_mapped_address():
    assert ft.dec_addr(V23_XMA, V23_TXN, True) == ("2001:db8:1234:5678:11:2233:4455:6677", 32853)


# --- RFC 5769 2.4: Sample Request with Long-Term Authentication --------------
# Username "マトリックス" (U+30DE U+30C8 U+30EA U+30C3 U+30AF U+30B9), realm
# "example.org". The password normalises under SASLprep to "TheMatrIX"; the
# long-term key is MD5(username:realm:password) over that normalised form. The
# message carries no FINGERPRINT.
V24_TXN = bytes.fromhex("78ad3433c6ad72c029da412e")
V24_BODY = bytes.fromhex(
    "00060012e3839ee38388e383aae38383e382afe382b90000"  # USERNAME (18 bytes) + 2 pad
    "0015001c662f2f3439396b39353464364f4c33346f4c39465354767936347341"  # NONCE
    "0014000b6578616d706c652e6f726700"  # REALM "example.org" + 1 pad
)
V24_FULL = bytes.fromhex(
    "00010060"
    "2112a442"
    + V24_TXN.hex()
    + V24_BODY.hex()
    + "00080014f67024656dd64a3e02b8e0712e85c9a28ca89666"  # MESSAGE-INTEGRITY
)
V24_KEY = ft.cred_key("マトリックス", "example.org", "TheMatrIX")


def test_rfc5769_2_4_longterm_roundtrip():
    m = ft.add_integrity(_msg(0x0001, V24_TXN, V24_BODY), V24_KEY)
    assert m == V24_FULL


def test_rfc5769_2_4_verify():
    ok, bad_after, has_fp = ft.verify_integrity(V24_KEY, V24_FULL)
    assert (ok, bad_after, has_fp) == (True, False, False)
