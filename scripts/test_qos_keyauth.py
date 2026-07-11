#!/usr/bin/env python3
"""
ADR-0019 authenticated-FSYN gate: prove the per-frame HMAC-SHA256 tag gates the
field-coupling wire (increment B-frame-auth).

Positive: two society members with the SAME host-admitted key couple under the
  MAC (R_x rises) — the keyed happy path (tags verify, seqs advance).
Negative: two members with DIFFERENT keys each REJECT the peer's frames (the
  FSAUTH marker appears on both) and do NOT synchronize — the security property.
  This is the un-fakeable, revert-confirm anchor: remove the MAC check and the
  different-key pair couples (no rejects), reddening this gate.

The frame the attacker cannot forge is the HMAC; source IP alone (which a spoof
passes) is not enough. A wrong key stands in for "no key / a spoofed source":
it is exactly the frame a non-member on the shared L2 could put on the wire.

Run: python3 scripts/test_qos_keyauth.py   (boots two 2-VM societies; needs qemu)
Gate: make ci-smoke-keyauth
"""
import atexit
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qos_bridge import QosSociety  # noqa: E402

KEY1 = bytes([0x11]) * 32
KEY2 = bytes([0x22]) * 32
KEY3 = bytes([0x33]) * 32
FSKEY = "FSKEY: swarm-plane session key installed"
REJECT = "FSAUTH: forged FSYN frame rejected (bad MAC)"


def _fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def _wait_both(soc, marker, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if marker in soc.a._log_text() and marker in soc.b._log_text():
            return True
        time.sleep(0.5)
    return False


def main():
    # --- Positive: same key => authenticated coupling ---
    soc = QosSociety()
    atexit.register(soc.shutdown)
    soc.boot("A1", "B2", timeout=45)
    # Admit the SAME key to both AS SOON AS possible after boot.
    soc.a.admit_key(KEY1)
    soc.b.admit_key(KEY1)
    if not _wait_both(soc, FSKEY, 20):
        _fail("same-key: both members did not install the key")
    print("OK: same key installed on both members")
    st = soc.await_sync(threshold=0.80, timeout=90)
    if not st:
        _fail("same-key: members did NOT synchronize (authenticated coupling broken)")
    print(f"OK: same-key members synchronized under HMAC auth "
          f"(R_x a={st['a']['r_x']:.2f} b={st['b']['r_x']:.2f})")
    soc.shutdown()

    # --- Negative: different keys => each rejects the peer, no coupling ---
    soc2 = QosSociety()
    atexit.register(soc2.shutdown)
    soc2.boot("C3", "D4", timeout=45)
    soc2.a.admit_key(KEY2)
    soc2.b.admit_key(KEY3)
    if not _wait_both(soc2, FSKEY, 20):
        _fail("diff-key: both members did not install their (distinct) key")
    print("OK: distinct keys installed on the two members")
    # Each member must reject the OTHER's wrong-key frames — the security property.
    if not _wait_both(soc2, REJECT, 45):
        _fail(f"diff-key: expected BOTH members to reject wrong-key frames ({REJECT!r})")
    print("OK: different-key members each REJECTED the peer's forged-MAC frames")
    # And neither may report synchronized (rejection prevents coupling).
    st2 = soc2.status()
    if st2["a"]["synchronized"] and st2["b"]["synchronized"]:
        _fail("diff-key: members synchronized despite mismatched keys (MAC not enforced)")
    print(f"OK: different-key members did NOT synchronize "
          f"(R_x a={st2['a'].get('r_x')} b={st2['b'].get('r_x')})")

    print("=== authenticated-FSYN gate PASSED — the frame MAC gates coupling ===")


if __name__ == "__main__":
    main()
