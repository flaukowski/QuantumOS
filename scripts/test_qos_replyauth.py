#!/usr/bin/env python3
"""
ADR-0019 Extension — COM2 DATA-reply authentication gate.

Proves a keyed VM's STATUS reply carries a per-request nonce echo + HMAC-SHA256
tag the host verifies, so an agent's tool RESULT is provably FRESH and UNFORGED,
not just that the boot attested. Four legs:
  fail-open  — an UNKEYED status() still returns a plain reply (ci-smoke-mcp path)
  positive   — status_authenticated() verifies (fresh nonce + valid tag)
  replay     — the SAME reply against a DIFFERENT nonce is rejected on the echo
               (its tag is still HMAC-valid — only the nonce betrays the replay)
  forgery    — a tampered tag byte is rejected on the MAC

Revert-confirm: zeroing the guest's reply tag makes the positive leg fail
(too-short / MAC), reddening the gate.

Run: python3 scripts/test_qos_replyauth.py   (boots one VM; needs qemu)
Gate: make ci-smoke-replyauth
"""
import atexit
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qos_bridge import QosVM, QosRefused, frame, FRAME_DATA, SWARM_OP_STATUS  # noqa: E402


def _fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def main():
    vm = QosVM(kernel=os.environ.get("QOS_KERNEL") or None)
    atexit.register(vm.shutdown)
    vm.boot(timeout=45)

    # fail-open: before any key, plain status() must still work (the unkeyed
    # ci-smoke-mcp path — the guest replies legacy 6 bytes with no nonce).
    s = vm.status()
    if "r" not in s:
        _fail("unkeyed status() did not return a plain reply (fail-open broken)")
    print(f"OK: unkeyed status() returns a plain reply (fail-open intact) r={s['r']:.2f}")

    vm.admit_key(b"\x5A" * 32)
    time.sleep(1.5)  # let the SWARM_OP_KEY frame be consumed before the first authed STATUS

    # positive: authenticated STATUS verifies (fresh nonce + valid tag).
    a = vm.status_authenticated()
    if not a.get("authenticated"):
        _fail(f"status_authenticated did not verify: {a}")
    print(f"OK: authenticated STATUS verified (fresh nonce + HMAC) r={a['r']:.2f}")

    # Grab one raw authenticated reply for the negative legs.
    nonce_a = os.urandom(16)
    payload = vm._transact(frame(FRAME_DATA, bytes([SWARM_OP_STATUS]) + nonce_a),
                           SWARM_OP_STATUS, time.time() + 5)
    vm._verify_status_reply(payload, nonce_a)  # sanity: verifies for its own nonce

    # replay: the SAME reply verified against a DIFFERENT nonce must fail — its
    # tag is still HMAC-valid, only the nonce echo betrays the replay.
    try:
        vm._verify_status_reply(payload, os.urandom(16))
        _fail("replayed reply (stale nonce) was ACCEPTED — freshness not enforced")
    except QosRefused as e:
        if "nonce" not in str(e).lower():
            _fail(f"replay rejected for the wrong reason (should cite the nonce): {e}")
    print("OK: a replayed reply (stale nonce, still-valid tag) is REJECTED on the nonce echo")

    # forgery: flip one tag byte -> the MAC must fail.
    bad = bytearray(payload)
    bad[22] ^= 0xFF
    try:
        vm._verify_status_reply(bytes(bad), nonce_a)
        _fail("forged reply (tampered tag) was ACCEPTED — MAC not enforced")
    except QosRefused as e:
        if "mac" not in str(e).lower():
            _fail(f"forgery rejected for the wrong reason (should cite the MAC): {e}")
    print("OK: a forged reply (tampered tag) is REJECTED on the MAC")

    print("=== COM2 reply-auth gate PASSED — tool replies are nonce+HMAC attested ===")


if __name__ == "__main__":
    main()
