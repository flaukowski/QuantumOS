#!/usr/bin/env python3
"""
ADR-0019 swarm-plane key channel gate (increment B, key distribution).

Proves the host-admitted group session key reaches fieldsyncd end to end:
  host  --SWARM_OP_KEY over attested COM2-->  swarm_svc
  swarm_svc  --"K"+32B over the boot-minted IPC send-cap-->  fieldsyncd

A single VM is booted with a peer= token so fieldsyncd runs its main loop (and
therefore drains the IPC where the key arrives; with no peer it idles). The peer
never answers — this gate is about the KEY path, not coupling. We assert both the
swarm-svc forward line and the fieldsyncd install line, and (anti-vacuity) that
the install marker is ABSENT before the admit.

Run: python3 scripts/test_qos_keyadmit.py   (boots its own QosVM; needs qemu)
Gate: make ci-smoke-keyadmit
"""
import atexit
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qos_bridge import QosVM  # noqa: E402

FWD = "SWARM: swarm-plane key forwarded to fieldsyncd"
INSTALL = "FSKEY: swarm-plane session key installed"


def _fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def main():
    vm = QosVM(kernel=os.environ.get("QOS_KERNEL") or None)
    atexit.register(vm.shutdown)
    # A static IP + a (never-answering) peer so fieldsyncd enters its loop and
    # binds UDP; the key path does not need the peer to exist.
    vm.boot(append_extra="ip=10.0.0.1 peer=10.0.0.99", timeout=45)

    # Anti-vacuity: the install marker must NOT be present before we admit.
    if INSTALL in vm._log_text():
        _fail("fieldsyncd reported a key installed BEFORE any admit (vacuous)")
    print("OK: no key installed pre-admit (gate is non-vacuous)")

    vm.admit_key(b"\xA5" * 32)

    # fieldsyncd processes the IPC on its next ~1 Hz loop pass; poll briefly.
    deadline = time.time() + 20
    got_fwd = got_install = False
    while time.time() < deadline and not (got_fwd and got_install):
        text = vm._log_text()
        got_fwd = got_fwd or (FWD in text)
        got_install = got_install or (INSTALL in text)
        time.sleep(0.5)

    if not got_fwd:
        _fail(f"swarm_svc did not forward the key (missing: {FWD!r})")
    print(f"OK: swarm_svc forwarded the admitted key over IPC ({FWD})")
    if not got_install:
        _fail(f"fieldsyncd did not install the key (missing: {INSTALL!r})")
    print(f"OK: fieldsyncd installed the host-admitted session key ({INSTALL})")

    print("=== swarm-plane key-admit gate PASSED — host key reaches fieldsyncd ===")


if __name__ == "__main__":
    main()
