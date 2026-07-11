#!/usr/bin/env python3
"""
ADR-0019 Extension — COM2 QSUBMIT reply-auth gate.

The QSUBMIT path is LIVE (the host submits opaque circuits to the in-OS QPU
broker and consumes the result), so an unauthenticated reply is a forgeable job
result. This gate proves a keyed VM's QSUBMIT reply carries a per-request nonce
echo + HMAC-SHA256 the host verifies, across BOTH reply sources — the ASYNC DONE
(nonce STASHED at accept, echoed when the job completes) and a SYNCHRONOUS error
(in-hand nonce) — so neither the deferred nor the immediate path is forgeable.

Five legs:
  fail-open  — an UNKEYED qsubmit() still returns a plain reply (ci-smoke path)
  positive   — a keyed qsubmit() DONE verifies (fresh nonce + valid tag), 20B result
  sync-error — a keyed malformed submit (empty circuit) verifies too: the
               synchronous error uses the IN-HAND nonce, not the stash
  replay     — the SAME reply against a DIFFERENT nonce is rejected on the echo
               (its tag is still HMAC-valid — only the nonce betrays the replay)
  forgery    — a tampered tag byte is rejected on the MAC

Revert-confirm: making the guest's emit_qsubmit_reply ignore its nonce (always
plain) fails the positive DONE leg (host rejects the <50-byte nonce-less reply).

Run: python3 scripts/test_qos_qsubmit_replyauth.py   (boots one VM; needs qemu)
Gate: make ci-smoke-qsubmit-replyauth
"""
import atexit
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qos_bridge import QosVM, QosRefused, frame, FRAME_DATA, SWARM_OP_QSUBMIT  # noqa: E402
import qpu_circuit as qc  # noqa: E402


def _fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def main():
    vm = QosVM(kernel=os.environ.get("QOS_KERNEL") or None)
    atexit.register(vm.shutdown)
    vm.boot(timeout=45)

    # fail-open: before any key, a plain qsubmit() must still work (the unkeyed
    # ci-smoke path — the guest replies legacy op|status|result with no nonce).
    r = vm.qsubmit(qc.bell())
    if r.get("status") != 0 or len(r.get("result", b"")) != qc.QC_RESULT_LEN:
        _fail(f"unkeyed qsubmit() did not return a plain DONE (fail-open broken): {r}")
    if r.get("authenticated"):
        _fail("unkeyed qsubmit() reported authenticated — should be the plain path")
    print(f"OK: unkeyed qsubmit() returns a plain DONE (fail-open intact) status={r['status']}")

    vm.admit_key(b"\x5A" * 32)
    time.sleep(1.5)  # let the SWARM_OP_KEY frame be consumed before the first authed submit

    # positive DONE: a keyed submit's ASYNC DONE verifies (stashed nonce + tag).
    a = vm.qsubmit(qc.bell())
    if not a.get("authenticated") or a.get("status") != 0 or len(a.get("result", b"")) != qc.QC_RESULT_LEN:
        _fail(f"keyed qsubmit DONE did not verify: {a}")
    print(f"OK: keyed QSUBMIT DONE verified (stashed nonce + HMAC) status={a['status']} "
          f"result={len(a['result'])}B")

    # sync-error: a keyed malformed submit (empty circuit -> clen==0) verifies on
    # the IN-HAND nonce path (distinct source from the stashed DONE above).
    e = vm.qsubmit(b"")
    if not e.get("authenticated") or e.get("status") != 2:
        _fail(f"keyed malformed submit did not verify as an authenticated error: {e}")
    print(f"OK: keyed QSUBMIT sync-error verified (in-hand nonce) status={e['status']}")

    # Grab one raw authenticated DONE reply for the negative legs.
    nonce = os.urandom(16)
    payload = vm._transact(
        frame(FRAME_DATA, bytes([SWARM_OP_QSUBMIT]) + nonce + bytes(qc.bell())),
        SWARM_OP_QSUBMIT, time.time() + 10)
    vm._verify_qsubmit_reply(payload, nonce)  # sanity: verifies for its own nonce

    # replay: the SAME reply verified against a DIFFERENT nonce must fail — its
    # tag is still HMAC-valid, only the nonce echo betrays the replay.
    try:
        vm._verify_qsubmit_reply(payload, os.urandom(16))
        _fail("replayed QSUBMIT reply (stale nonce) was ACCEPTED — freshness not enforced")
    except QosRefused as ex:
        if "nonce" not in str(ex).lower():
            _fail(f"replay rejected for the wrong reason (should cite the nonce): {ex}")
    print("OK: a replayed QSUBMIT reply (stale nonce, still-valid tag) is REJECTED on the nonce echo")

    # forgery: flip one tag byte -> the MAC must fail. The tag is the last 32 B.
    bad = bytearray(payload)
    bad[-1] ^= 0xFF
    try:
        vm._verify_qsubmit_reply(bytes(bad), nonce)
        _fail("forged QSUBMIT reply (tampered tag) was ACCEPTED — MAC not enforced")
    except QosRefused as ex:
        if "mac" not in str(ex).lower():
            _fail(f"forgery rejected for the wrong reason (should cite the MAC): {ex}")
    print("OK: a forged QSUBMIT reply (tampered tag) is REJECTED on the MAC")

    print("=== COM2 QSUBMIT reply-auth gate PASSED — job results are nonce+HMAC attested ===")


if __name__ == "__main__":
    main()
