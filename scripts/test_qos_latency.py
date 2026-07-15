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

Since the ADR-0022 I/O boost (the latency fix) this gate is ARMED TIGHT and
non-vacuous twice over: the PING median must sit under the boosted ceiling AND
the kernel's honored-boost counter must advance across the batch (binding the
number to the mechanism — a bare median is gameable by an unrelated quantum
change, which ci-smoke-sched deliberately tolerates).
REVERT-CONFIRM — primary: SCHED_IO_BOOST 0 in kernel/include/kernel/scheduler.h
reddens BOTH assertions (median back to the phase-locked ~0.45s rotation floor;
boost delta 0). Secondary: bumping SCHED_QUANTUM_TICKS (e.g. 5 -> 80) multiplies
the boost-off cycle and reddens the gross ceiling as before.

Run: python3 scripts/test_qos_latency.py   (boots one VM; needs qemu)
Gate: make ci-smoke-latency
"""
import atexit
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qos_bridge import QosVM, QosError  # noqa: E402

PING_CEILING_S = 4.0  # median one-hop round-trip; gross-regression guard (kept — see below)

# ADR-0022 I/O boost (the latency fix this gate was built to prove). With the
# boost, a hop costs RX-detect (<=10ms tick) + one quantum remainder (<=50ms)
# instead of a full round-robin rotation — expected median <=0.1s. The bound is
# safe on BOTH sides for structural reasons: boost-OFF pings are PHASE-LOCKED
# to a full rotation (the host re-sends within ~1ms of each PONG and swarm_svc
# yields right after its pass), and the rotation is guest PIT wall-clock time,
# host-invariant — so a reverted/rotted boost medians >=~0.45s on ANY host
# (1.5x fail-side margin); boost-ON is roster-size-invariant (the boost jumps
# the queue), so CI roster growth cannot erode the pass side (~3x margin).
PING_BOOSTED_CEILING_S = 0.30
N_PINGS = 9  # median-of-9 tolerates 4 outliers (median-of-5 only 2)

# STATUS (3 boosted hops: request-IPC boosts ghostd, reply-IPC boosts
# swarm_svc, COM2-RX boosts swarm_svc). Boost-off floor is >=2 phase-locked
# rotations (~0.9s); boost-on ~0.2-0.3s. Armed with a success floor: with the
# boost live, repeated ghostd-chain timeouts ARE an IPC-delivery defect, not
# weather.
STATUS_CEILING_S = 0.6
STATUS_MIN_OK = 3  # of 5


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

    # Boost non-vacuity bracket, part 1 (ADR-0022): snapshot the HONORED-boost
    # counter before the measured batch. The counter is incremented only at a
    # committed boosted dispatch (never on flag-sets), so the delta assertion
    # below binds the latency number to the boost MECHANISM — a median alone
    # is gameable by an unrelated quantum change, and a set-side counter would
    # read nonzero with the pick path dead.
    boosts_before = vm.sched(deadline_s=10.0)["boost"]

    # PING->PONG: the pure transport + scheduler-cadence one-hop floor. Generous
    # per-ping deadline so even an inflated (regressed) round-trip is MEASURED
    # rather than timed out, letting the ceiling assertion catch it. Back-to-back
    # sends are deliberate: they phase-lock a boost-OFF ping to a FULL rotation,
    # which is what makes the tightened bound's fail side deterministic — do NOT
    # add inter-ping sleeps.
    pings = [vm.ping(deadline_s=10.0) for _ in range(N_PINGS)]
    ping_med = _median(pings)

    # STATUS: adds the swarm_svc<->ghostd IPC hop. Armed since the boost
    # (previously recorded-only: at a ~1s boost-off round-trip any meaningful
    # bound would have flaked on warm-up hiccups; boosted, the margin exists
    # on both sides).
    stats = []
    for _ in range(5):
        try:
            t0 = time.time()
            vm.status(deadline_s=10.0)
            stats.append(time.time() - t0)
        except QosError:
            pass
    status_str = f"{_median(stats):.3f}s (n={len(stats)})" if stats else "n/a"

    boosts_after = vm.sched(deadline_s=10.0)["boost"]
    boost_delta = boosts_after - boosts_before

    print(f"COM2 latency (ADR-0022): PING median {ping_med:.3f}s (n={N_PINGS}), "
          f"STATUS median {status_str}, boost picks +{boost_delta}")

    if ping_med > PING_CEILING_S:
        _fail(f"PING round-trip median {ping_med:.2f}s exceeds {PING_CEILING_S}s ceiling — "
              "COM2 transport / scheduler-cadence latency has REGRESSED (see ADR-0022)")
    if ping_med > PING_BOOSTED_CEILING_S:
        _fail(f"PING round-trip median {ping_med:.2f}s exceeds the BOOSTED ceiling "
              f"{PING_BOOSTED_CEILING_S}s — the ADR-0022 I/O boost is not delivering "
              "(reverted, rotted, or mis-picked); boost-off phase-locks to a full "
              "rotation >=~0.45s on any host")
    if boost_delta <= 0:
        _fail(f"boost-pick counter did not advance across the measured batch "
              f"(+{boost_delta}) — the latency number is not coming from the I/O boost "
              "(unrelated cadence change, or the boost path is dead); see ADR-0022")
    if len(stats) < STATUS_MIN_OK:
        _fail(f"only {len(stats)}/5 STATUS round-trips completed — with the IPC boost "
              "live, repeated ghostd-chain timeouts are an IPC-delivery defect, not weather")
    if _median(stats) > STATUS_CEILING_S:
        _fail(f"STATUS round-trip median {_median(stats):.2f}s exceeds {STATUS_CEILING_S}s — "
              "the 3-hop chain is not riding the IPC boost (see ADR-0022)")
    print(f"OK: PING median {ping_med:.2f}s < {PING_BOOSTED_CEILING_S}s, "
          f"STATUS median {_median(stats):.2f}s < {STATUS_CEILING_S}s, "
          f"boost picks +{boost_delta} (mechanism live)")
    print("=== COM2 latency gate PASSED — boosted round-trips bounded, boost mechanism proven ===")


if __name__ == "__main__":
    main()
