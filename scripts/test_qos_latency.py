#!/usr/bin/env python3
"""
COM2 round-trip latency gate (ADR-0022, prerequisite 1).

ADR-0022 root-caused the agent-tool latency floor to the scheduler cadence
(100 Hz tick x SCHED_QUANTUM_TICKS=5 x ~9 runnable citizens ~= a 450 ms round-
robin cycle; each service hop costs one cycle). It called for a bounded latency
assertion so a future fix can be PROVEN and a scheduler change can't silently
regress the floor. This is that gate.

It measures the one-hop PING->PONG round-trip (swarm_svc echoes directly, no
downstream IPC hop — the purest transport+cadence signal) and asserts a median
under a gross-regression ceiling, and REPORTS the STATUS round-trip (adds the
swarm_svc<->ghostd hop, ~3x PING) as the recorded baseline. It is a gross-
regression guard + baseline, NOT a tight perf tracker (that is ADR-0022's
prerequisite 2 and needs historical tracking).

The ceiling is generous (CI runners are slower than a dev box) but non-vacuous:
REVERT-CONFIRM — bumping SCHED_QUANTUM_TICKS in kernel/include/kernel/scheduler.h
(e.g. 5 -> 80) multiplies the round-robin cycle and pushes the PING median well
past the ceiling, reddening this gate. That the real regression source trips it
proves it is load-bearing.

Run: python3 scripts/test_qos_latency.py   (boots one VM; needs qemu)
Gate: make ci-smoke-latency
"""
import atexit
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qos_bridge import QosVM, QosError  # noqa: E402

PING_CEILING_S = 4.0  # median one-hop round-trip; ~9x the observed ~0.45s, catches a gross regression


def _median(xs):
    s = sorted(xs)
    return s[len(s) // 2]


def _fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def main():
    vm = QosVM(kernel=os.environ.get("QOS_KERNEL") or None)
    atexit.register(vm.shutdown)
    vm.boot(timeout=45)

    # Warm-up: the FIRST status after a fresh boot can hit ghostd before its
    # field is ready (a transient slow reply), so absorb it before measuring.
    # Tolerant — a warm-up hiccup is not a latency regression.
    try:
        vm.status(deadline_s=10.0)
    except QosError:
        pass

    # PING->PONG: the pure transport + scheduler-cadence one-hop floor. Generous
    # per-ping deadline so even an inflated (regressed) round-trip is MEASURED
    # rather than timed out, letting the ceiling assertion catch it. This is the
    # ASSERTED metric (reliable — no downstream hop).
    pings = [vm.ping(deadline_s=10.0) for _ in range(5)]
    ping_med = _median(pings)

    # STATUS: adds the swarm_svc<->ghostd IPC hop (~2-3x PING). BEST-EFFORT
    # baseline only (not asserted), so a transient ghostd hiccup reports "n/a"
    # rather than flaking the gate — PING carries the pass/fail.
    stats = []
    for _ in range(3):
        try:
            t0 = time.time()
            vm.status(deadline_s=10.0)
            stats.append(time.time() - t0)
        except QosError:
            pass
    status_str = f"{_median(stats):.3f}s (n={len(stats)})" if stats else "n/a"

    print(f"COM2 latency baseline (ADR-0022): PING median {ping_med:.3f}s (n=5), "
          f"STATUS median {status_str}")

    if ping_med > PING_CEILING_S:
        _fail(f"PING round-trip median {ping_med:.2f}s exceeds {PING_CEILING_S}s ceiling — "
              "COM2 transport / scheduler-cadence latency has REGRESSED (see ADR-0022)")
    print(f"OK: PING round-trip within bound (median {ping_med:.2f}s < {PING_CEILING_S}s ceiling)")
    print("=== COM2 latency gate PASSED — one-hop round-trip bounded, baseline recorded ===")


if __name__ == "__main__":
    main()
