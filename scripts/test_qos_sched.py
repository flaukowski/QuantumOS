#!/usr/bin/env python3
"""
Scheduler perf/stability baseline recorder (ADR-0022, prerequisite 2) — REPORT-ONLY.

ADR-0022 root-caused the agent-tool latency floor to the scheduler cadence and DEFERS
the fix behind two prerequisites. Prereq 1 (the COM2 latency gate, test_qos_latency.py)
shipped. This is prereq 2: a perf/stability baseline so a future scheduler-constant
change (the deferred fix) is MEASURED, not blind (ADR-0016 anti-vacuous discipline).

WHY REPORT-ONLY (PR-1): the design panel's red-team refuted the naive metric. Aggregate
context switches (`switches`) are dominated by VOLUNTARY yields — every long-lived citizen
busy-yields, resetting the quantum counter thousands of times per 10 ms tick, so the timer-
preemption path is nearly dead at rest. switches/tick is therefore BOTH quantum-insensitive
(a SCHED_QUANTUM_TICKS change does not move it -> a gate on it is vacuous) AND host-CPU-
throughput dependent (yield-loop iterations per tick scale with guest speed -> flaky on slow
CI). The kernel now exposes a DEDICATED `preempt` counter incremented ONLY on timer-quantum
expiry (never on SYS_YIELD), which is 1/quantum-paced by construction. But whether `preempt`
is non-zero enough UNDER LOAD to separate q=5 from q=80 is an EMPIRICAL question. So PR-1
does NOT assert any perf band. It RECORDS every candidate (preempt-per-1000-ticks, switch-
per-1000-ticks, max reschedule gap, run-count spread) under idle and under COM2 PING load,
across the SCHED_QUANTUM_TICKS values, to CALIBRATE which scalar PR-2 will gate. The only
assertion here is a non-vacuous LIVENESS floor (the scheduler is actually multiplexing a
real roster) so this job still fails on a dead/wedged/frozen scheduler.

Env:
  QOS_KERNEL       path to kernel.elf32 (else the bridge default)
  QOS_SCHED_LOAD   "1" to drive continuous COM2 PING load while sampling (default idle)
  QOS_SCHED_TICKS  target guest-tick window to accumulate before reporting (default 3000)

Run:  python3 scripts/test_qos_sched.py            # idle baseline
      QOS_SCHED_LOAD=1 python3 scripts/test_qos_sched.py   # under-load baseline
Gate: make ci-smoke-sched
"""
import atexit
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qos_bridge import QosVM, QosError  # noqa: E402

# The scheduler must genuinely be multiplexing a real roster — a non-vacuous liveness
# floor that reddens on a dead/single-runnable/wedged scheduler but not on any healthy
# cadence. Observed steady-state runnable (READY|RUNNING minus idle/kernel, includes the
# demo kernel threads) is ~20-22 at rest; the floor is set generously below that.
LIVENESS_MIN_SWITCHES = 100   # context switches over the measured window
LIVENESS_MIN_RUNNABLE = 12    # settled runnable count (observed ~20 at rest)

# State-gated warmup by GUEST TICKS: the transient boot self-tests (echo, watched-svc,
# quota-test, delegation-test, cpu-hog, qpu-test) run and exit inside the first few
# hundred ticks; cpu-hog in particular holds whole quanta and pollutes the preemption
# regime. Waiting for a tick floor clears them without depending on wall clock.
WARMUP_MAX_S = 25.0
WARMUP_MIN_TICKS = 500        # start measuring only after the boot-transient window
WINDOW_TICKS = int(os.environ.get("QOS_SCHED_TICKS", "3000"))
LOAD = os.environ.get("QOS_SCHED_LOAD", "") == "1"


def _fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def main():
    vm = QosVM(kernel=os.environ.get("QOS_KERNEL") or None)
    atexit.register(vm.shutdown)
    vm.boot(timeout=45)

    # Warm-up / state gate: wait past the boot-transient window (WARMUP_MIN_TICKS
    # guest ticks) so the transient self-test citizens — cpu-hog especially, which
    # holds whole quanta and pollutes the preemption regime — have exited before the
    # measured window opens.
    settled = None
    t_end = time.time() + WARMUP_MAX_S
    while time.time() < t_end:
        try:
            s = vm.sched(deadline_s=12.0)
        except QosError:
            time.sleep(0.3)
            continue
        if s["ticks"] >= WARMUP_MIN_TICKS:
            settled = s
            break
        time.sleep(0.4)
    if settled is None:
        # Not fatal to REPORT, but record that warmup never reached the tick floor.
        try:
            settled = vm.sched(deadline_s=12.0)
        except QosError:
            _fail("scheduler never produced a SCHED line (dead scheduler / no qsh)")
    print(f"warmup: reached tick={settled['ticks']} runnable={settled['runnable']} "
          f"(load={'on' if LOAD else 'off'})")

    # Sampling window: from S0, optionally drive continuous PING load (the only load
    # that makes the COM2 service chain consume whole quanta), sampling the scheduler
    # counters until >= WINDOW_TICKS guest ticks have elapsed. Everything asserted or
    # reported is a DELTA over guest ticks -> host-CPU-invariant (a slow runner just
    # takes longer wall-clock to fill the window).
    s0 = settled
    last = s0
    max_gap = s0["maxgap"]
    max_spread = s0["spread"]
    min_runnable = s0["runnable"]
    wall_deadline = time.time() + 90.0  # absolute safety bound on wall time
    while (last["ticks"] - s0["ticks"]) < WINDOW_TICKS and time.time() < wall_deadline:
        if LOAD:
            try:
                vm.ping(deadline_s=10.0)
            except QosError:
                pass  # a transient ping hiccup is not a scheduler fault; keep sampling
        else:
            time.sleep(0.05)
        try:
            last = vm.sched(deadline_s=12.0)
        except QosError:
            continue
        max_gap = max(max_gap, last["maxgap"])
        max_spread = max(max_spread, last["spread"])
        min_runnable = min(min_runnable, last["runnable"])

    d_ticks = last["ticks"] - s0["ticks"]
    d_switch = last["switches"] - s0["switches"]
    d_preempt = last["preempt"] - s0["preempt"]
    if d_ticks < WINDOW_TICKS // 2:
        _fail(f"window too short (dticks={d_ticks} < {WINDOW_TICKS // 2}) — "
              "guest clock not advancing (scheduler/timer wedged?)")

    # Guest-tick-normalized candidate metrics (the calibration currency).
    p_preempt = 1000.0 * d_preempt / d_ticks
    p_switch = 1000.0 * d_switch / d_ticks

    # THE recorded baseline line — machine-parseable, aggregated across the 10-run
    # q=5/q=80 idle/load calibration table that SELECTS the PR-2 gated scalar.
    print(f"SCHEDBASE: load={1 if LOAD else 0} dticks={d_ticks} dswitch={d_switch} "
          f"dpreempt={d_preempt} preempt_per_1000t={p_preempt:.2f} "
          f"switch_per_1000t={p_switch:.2f} maxgap={max_gap} spread={max_spread} "
          f"runnable_min={min_runnable}")

    # NON-VACUOUS LIVENESS FLOOR (the only assertion in PR-1): the scheduler must be
    # multiplexing a real roster. Reddens on a dead scheduler (no switches), a single-
    # runnable degenerate, or a frozen guest clock — but NOT on any healthy cadence, so
    # it is not a perf band (that is PR-2, armed on the calibration-selected scalar).
    if d_switch < LIVENESS_MIN_SWITCHES:
        _fail(f"scheduler not multiplexing: {d_switch} switches over {d_ticks} ticks "
              f"(< {LIVENESS_MIN_SWITCHES}) — dead/wedged scheduler")
    if min_runnable < LIVENESS_MIN_RUNNABLE:
        _fail(f"runnable roster collapsed to {min_runnable} (< {LIVENESS_MIN_RUNNABLE}) "
              "— citizens not staying schedulable")
    print(f"OK: scheduler live ({d_switch} switches, {min_runnable} runnable over "
          f"{d_ticks} ticks). Baseline recorded — no perf band asserted (PR-1 report-only).")
    print("=== scheduler baseline recorder PASSED (liveness floor; calibration data emitted) ===")


if __name__ == "__main__":
    main()
