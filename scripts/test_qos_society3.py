#!/usr/bin/env python3
"""QuantumOS N-way agent-society gate (epic #139). Stdlib-only: drives QosSociety
directly (the code path the qos_society_* tools delegate to) and never imports the
`mcp` package, so it runs in the integration CI job with no pip.

Boots THREE attested QuantumOS VMs into ONE mean-field society on a shared mcast
L2 and proves, anti-vacuously, what two VMs structurally CANNOT fake:
  1. all three members attest with their OWN distinct qseeds (three identities);
  2. each member receives phase frames from BOTH of its peers (the full 3-cycle,
     asserted per-IP inside boot_n — a 2-of-3 masquerade cannot satisfy it);
  3. every field synchronizes to MIN-pairwise R_x >= 0.80 (min, not mean, so a
     partial 2-of-3 lock cannot pass) AND each started DIVERGENT (a sub-0.50
     sample) — three divergent seeds mean-field converging is not a pairwise lock;
  4. shutdown reaps all three QEMU processes.

The 2-node society gate (test_qos_society.py) is unchanged and remains the
regression baseline for the P=1 coupling path.

Exit 0 on success. A SIGTERM (timeout) or any exception still reaps every VM via
the society's single atexit + signal reaper.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from qos_bridge import QosSociety, QosError  # noqa: E402

QSEEDS = ["1111111111111111", "8888888888888888", "cccccccccccccccc"]


def _fail(msg):
    print(f"FAIL: {msg}")
    raise SystemExit(1)


def _kernel():
    return os.environ.get("QOS_KERNEL") or None


def main():
    soc = QosSociety(kernel=_kernel())
    try:
        # 1-2. Boot 3 members; boot_n's mcast reachability precheck asserts the
        # full per-IP 3-cycle (every node heard both peers) or FAILS LOUD.
        st = soc.boot_n(QSEEDS, timeout=45)
        members = st["members"]
        if len(members) != 3:
            _fail(f"expected 3 members, got {len(members)}")
        seeds = [m["identity"]["qseed"].upper() for m in members]
        if not all(m["verified"] for m in members):
            _fail(f"all three members must attest: {st}")
        if len(set(seeds)) != 3:
            _fail(f"three DISTINCT attested identities expected: {seeds}")
        print(f"OK: three attested members booted, distinct qseeds {seeds}; "
              f"each received frames from both peers (full 3-cycle)")

        # 3. All three synchronize on MIN-pairwise R_x, each from a divergent start.
        final = soc.await_sync_n(threshold=0.80, timeout=140)
        for i, node in enumerate(final["members"]):
            if not node["synchronized"] or (node["r_x"] or 0) < 0.80:
                _fail(f"member {i} did not synchronize (min-pairwise): {node}")
            if node["r_x_min"] is None or node["r_x_min"] >= 0.50:
                _fail(f"member {i} never started divergent — gate vacuous: {node}")
        rxs = [(m["r_x"], m["r_x_min"]) for m in final["members"]]
        print(f"OK: all three fields mean-field synchronized — (r_x, min) {rxs} "
              f"— non-vacuous (each started < 0.50); a 2-VM society can only "
              f"pairwise-lock, so three divergent seeds locking proves mean-field")
    finally:
        soc.shutdown()
    if soc.members:
        _fail("society did not clear its member handles on shutdown")
    print("OK: society shutdown reaped all three members")

    if "mcp" in sys.modules:
        _fail("the `mcp` package leaked into the stdlib-only test")
    print("OK: no `mcp` import (integration CI stays pip-free)")
    print("=== SOCIETY3 gate PASSED — three attested kernels, one mean field ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
