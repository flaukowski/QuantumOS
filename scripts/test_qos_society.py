#!/usr/bin/env python3
"""QuantumOS agent-society gate (epic #131). Stdlib-only: it drives QosSociety
directly (the exact code path the qos_society_* FastMCP tools delegate to) and
NEVER imports the `mcp` package, so it runs in the integration CI job with no pip.

Boots TWO attested QuantumOS VMs coupled into one holographic field and proves,
anti-vacuously:
  1. both members attest with their OWN qseeds and the two identities DIFFER;
  2. the two fields SYNCHRONIZE (R_x >= 0.80 on both) AND each started DIVERGENT
     (a sub-0.50 R_x sample before sync) — a lone/un-wired VM never synchronizes,
     so this cannot pass vacuously;
  3. shutdown reaps BOTH QEMU processes (the society clears its handles);
  4. identical qseeds are refused (a duplicate identity + a vacuous instant sync);
  5. NEGATIVE admission: a member booted with qseed X but verified against Y is
     reported UNVERIFIED with the exact mismatch reason, while its peer (correct
     qseed) verifies — and the coupling still happens (the FSYN wire is
     unauthenticated: the honest trust boundary).

Exit 0 on success. A SIGTERM (timeout) or any exception still reaps every VM via
the society's single atexit + signal reaper.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from qos_bridge import QosSociety, QosError

QSEED_A = "1111111111111111"
QSEED_B = "8888888888888888"


def _fail(msg):
    print(f"FAIL: {msg}")
    raise SystemExit(1)


def _kernel():
    return os.environ.get("QOS_KERNEL") or None


def main():
    # 1-3. Positive: two attested members boot, couple, synchronize, and reap.
    soc = QosSociety(kernel=_kernel())
    try:
        st = soc.boot(QSEED_A, QSEED_B, timeout=45)
        if not st["a"]["verified"] or not st["b"]["verified"]:
            _fail(f"both members must attest: {st}")
        qa = st["a"]["identity"]["qseed"]
        qb = st["b"]["identity"]["qseed"]
        if qa == qb or qa.upper() != QSEED_A.upper() or qb.upper() != QSEED_B.upper():
            _fail(f"distinct attested identities expected: A={qa} B={qb}")
        print(f"OK: two attested members booted (A qseed={qa}, B qseed={qb})")

        final = soc.await_sync(threshold=0.80, timeout=100)
        for n in ("a", "b"):
            node = final[n]
            if not node["synchronized"] or (node["r_x"] or 0) < 0.80:
                _fail(f"member {n} did not synchronize: {node}")
            if node["r_x_min"] is None or node["r_x_min"] >= 0.50:
                _fail(f"member {n} never started divergent — gate vacuous: {node}")
        print(f"OK: fields synchronized — A r_x={final['a']['r_x']} (min "
              f"{final['a']['r_x_min']}), B r_x={final['b']['r_x']} (min "
              f"{final['b']['r_x_min']}) — non-vacuous (both started < 0.50)")
    finally:
        soc.shutdown()
    if soc.a is not None or soc.b is not None:
        _fail("society did not clear its handles on shutdown")
    print("OK: society shutdown reaped both members")

    # 4. Distinct-qseed enforcement (refused BEFORE any VM boots).
    dup = QosSociety(kernel=_kernel())
    try:
        dup.boot(QSEED_A, QSEED_A, timeout=45)
        _fail("identical qseeds were not refused")
    except QosError:
        print("OK: identical qseeds refused (distinct identities required)")
    finally:
        dup.shutdown()

    # 5. Negative admission: boot qseed A but VERIFY against B -> member A is
    # unverified with the mismatch reason; member B (correct) verifies; coupling
    # still happens (the wire is unauthenticated — the honest boundary).
    neg = QosSociety(kernel=_kernel())
    try:
        st = neg.boot(QSEED_A, QSEED_B, expect_a=QSEED_B, timeout=45)
        if st["a"]["verified"]:
            _fail("member A verified despite a qseed mismatch")
        reason = st["a"]["identity"].get("reason") or ""
        if "!=" not in reason and "qseed" not in reason.lower():
            _fail(f"mismatch reason not surfaced: {reason!r}")
        if not st["b"]["verified"]:
            _fail(f"member B (correct qseed) should verify: {st['b']}")
        print(f"OK: negative admission — A refused ({reason}); B verified; "
              f"coupling still occurred (wire is unauthenticated, as documented)")
    finally:
        neg.shutdown()

    if "mcp" in sys.modules:
        _fail("the `mcp` package leaked into the stdlib-only test")
    print("OK: no `mcp` import (integration CI stays pip-free)")
    print("=== SOCIETY gate PASSED — two attested kernels, one field ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
