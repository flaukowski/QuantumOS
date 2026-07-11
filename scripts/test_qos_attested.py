#!/usr/bin/env python3
"""
ADR-0019 Extension — attestation-by-default gate (the AGENT surface).

Proves QosVM.attest() turns the built-but-dormant COM2 reply-auth ON for a whole
session, so a tool RESULT is provably fresh + unforged — not merely that the boot
attested (ADR-0015 'verified != live'). This is what makes the MCP tools attested
by default: qos_boot calls attest(), qos_status uses status_authenticated, and
qos_qpu_run surfaces `attested`.

Legs:
  baseline — BEFORE attest, qpu_run() rides the plain path (attested False) and
             status_authenticated() refuses (no key): the dormant state
  attest   — attest() admits a fresh host key and confirms reply-auth is LIVE
  status   — status_authenticated() then verifies fresh (nonce + HMAC)
  qpu      — qpu_run() over the now-keyed session returns attested True + a real
             result: the live circuit path is attested end-to-end

Revert-confirm: making attest() skip its admit_key (a no-op) leaves the session
on the plain path, so the status leg's status_authenticated() raises (no key)
and the qpu leg's attested is False — reddening the gate.

Run: python3 scripts/test_qos_attested.py   (boots one VM; needs qemu)
Gate: make ci-smoke-attested
"""
import atexit
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qos_bridge import QosVM, QosError  # noqa: E402


def _fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def main():
    vm = QosVM(kernel=os.environ.get("QOS_KERNEL") or None)
    atexit.register(vm.shutdown)
    vm.boot(timeout=45)

    # baseline: the DORMANT state — before attest, no key, so qpu_run rides the
    # plain path (not attested) and an authenticated STATUS has nothing to verify.
    base = vm.qpu_run(kind="bell")
    if base.get("attested"):
        _fail(f"qpu_run before attest() reported attested — should be the plain path: {base}")
    try:
        vm.status_authenticated()
        _fail("status_authenticated() before attest() did not refuse (no key)")
    except QosError:
        pass
    print("OK: baseline dormant — qpu_run plain (attested False), authed STATUS refuses (no key)")

    # attest: admit a fresh host key and confirm reply-auth went LIVE.
    a = vm.attest()
    if not a.get("authenticated"):
        _fail(f"attest() did not confirm a live authenticated STATUS: {a}")
    print(f"OK: attest() went live — session reply-auth confirmed r={a['r']:.2f}")

    # status: an authenticated STATUS now verifies fresh.
    s = vm.status_authenticated()
    if not s.get("authenticated"):
        _fail(f"status_authenticated() did not verify after attest(): {s}")
    print(f"OK: STATUS attested fresh (nonce + HMAC) r={s['r']:.2f}")

    # qpu: the LIVE circuit path is attested end-to-end.
    q = vm.qpu_run(kind="bell")
    if not q.get("attested") or not q.get("ok"):
        _fail(f"qpu_run after attest() not attested/ok: {q}")
    print(f"OK: qpu_run attested end-to-end (bell ok, result verified) attested={q['attested']}")

    print("=== attestation-by-default gate PASSED — the agent surface is attested-fresh ===")


if __name__ == "__main__":
    main()
