#!/usr/bin/env python3
"""QuantumOS N=4 society gate — the documented configuration ceiling (societies
epic, increment 2). Stdlib-only: drives QosSociety directly (the path the
qos_society_* MCP tools delegate to) and never imports `mcp`, so it runs in CI
with no pip.

Boots FOUR attested QuantumOS VMs into ONE mean-field society on a shared mcast
L2 — the maximum the host bridge addresses (`_NET`/`_MAC` are 4 entries,
qos_bridge.py) and the same count `GHOST_MAX_PEERS`/`MAX_PEERS` bound each node's
peer set to (N-1 = 3 peers). Proves, anti-vacuously:
  1. all four members attest with their OWN distinct qseeds (four identities);
  2. each member receives phase frames from ALL THREE of its peers — 12 directed
     frame observations, asserted per-IP inside boot_n; a 3-of-4 masquerade
     cannot satisfy the full mesh;
  3. every field synchronizes to MIN-pairwise R_x >= 0.80 (min, not mean, so a
     partial 3-of-4 lock cannot pass) AND each started DIVERGENT (a sub-0.50
     sample) — four divergent seeds mean-field converging is not a pairwise lock;
  4. shutdown reaps all four QEMU processes.

Scope, stated honestly (per the epic's design review): N=4 puts THREE peers in
each node's four-slot ghostd table, so the slot table is at 3/4 — the slot-full
and eviction paths are structurally unreachable here and are NOT asserted (there
is no such thing as "no eviction" to observe). What this gate proves is the
DOCUMENTED N=4 configuration: full-mesh coupling, distinct identities, divergent
convergence, clean teardown. The true ceilings are the host `_NET`/`_MAC` arrays
(this N=4) and, one past them, the `GHOST_MAX_PEERS`/`MAX_PEERS`=4 constants
(which bound peers-per-node and would admit N=5) — see ADR-0014.

Exit 0 on success. A SIGTERM (timeout) or any exception still reaps every VM via
the society's single atexit + signal reaper.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from qos_bridge import QosSociety, QosError  # noqa: E402

# Four well-separated qseeds — distinct so each field STARTS divergent (the
# non-vacuous R_x-climb premise) and each attests a distinct identity.
QSEEDS = ["1111111111111111", "5555555555555555",
          "aaaaaaaaaaaaaaaa", "ffffffffffffffff"]


def _fail(msg):
    print(f"FAIL: {msg}")
    raise SystemExit(1)


def _kernel():
    return os.environ.get("QOS_KERNEL") or None


def main():
    soc = QosSociety(kernel=_kernel())
    try:
        # 1-2. Boot 4 members; boot_n's mcast reachability precheck asserts the
        # full per-IP 4-cycle (each of the 4 nodes heard all 3 peers = 12
        # directed observations) or FAILS LOUD. Precheck deadline scales with N.
        st = soc.boot_n(QSEEDS, timeout=60)
        members = st["members"]
        if len(members) != 4:
            _fail(f"expected 4 members, got {len(members)}")
        seeds = [m["identity"]["qseed"].upper() for m in members]
        if not all(m["verified"] for m in members):
            _fail(f"all four members must attest: {st}")
        if len(set(seeds)) != 4:
            _fail(f"four DISTINCT attested identities expected: {seeds}")
        print(f"OK: four attested members booted, distinct qseeds {seeds}; "
              f"each received frames from all three peers (12 directed observations)")

        # 3. All four synchronize on MIN-pairwise R_x, each from a divergent
        # start. r_x_min is per-NODE (min over that node's live pairs, over time),
        # so the divergence assert is per-node, not per-pair (the log carries no
        # per-slot R_x). Timeout is generous: 4 QEMU-TCG VMs oversubscribe the
        # runner's cores, dilating guest-tick (hence convergence) wall-clock.
        final = soc.await_sync_n(threshold=0.80, timeout=240)
        for i, node in enumerate(final["members"]):
            if not node["synchronized"] or (node["r_x"] or 0) < 0.80:
                _fail(f"member {i} did not synchronize (min-pairwise): {node}")
            if node["r_x_min"] is None or node["r_x_min"] >= 0.50:
                _fail(f"member {i} never started divergent — gate vacuous: {node}")
        rxs = [(m["r_x"], m["r_x_min"]) for m in final["members"]]
        print(f"OK: all four fields mean-field synchronized — (r_x, min) {rxs} "
              f"— non-vacuous (each started < 0.50); four divergent seeds locking "
              f"on min-pairwise proves a real 4-node mean field, not a lucky pair")
    finally:
        soc.shutdown()
    if soc.members:
        _fail("society did not clear its member handles on shutdown")
    print("OK: society shutdown reaped all four members")

    if "mcp" in sys.modules:
        _fail("the `mcp` package leaked into the stdlib-only test")
    print("OK: no `mcp` import (integration CI stays pip-free)")
    print("=== SOCIETY4 gate PASSED — four attested kernels, one mean field "
          "(the documented ceiling) ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
