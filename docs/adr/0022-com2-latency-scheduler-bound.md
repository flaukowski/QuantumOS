# 22. COM2 Round-Trip Latency Is Scheduler-Cadence-Bound

Date: 2026-07-11
Status: Accepted (2026-07-13; the deferral decision + both prerequisites are complete — the latency fix itself remains a deliberately deferred future epic)

> **Update (2026-07-11).** Prerequisite 1 below — a bounded latency assertion — **shipped** as
> `ci-smoke-latency` (`scripts/test_qos_latency.py`, via the new `QosVM.ping()` one-hop primitive).
> It asserts a bounded PING/PONG median (the pure transport + scheduler-cadence floor) and records the
> STATUS baseline; it is revert-confirmed against the real regression source — bumping
> `SCHED_QUANTUM_TICKS` reddens it. This gives any future latency-reduction work a proven guard and a
> recorded baseline (~0.45 s PING / ~0.90 s STATUS on the dev box).
>
> **Update (2026-07-13) — Accepted, both prerequisites complete.** Prerequisite 2 (the scheduler
> perf/stability baseline) shipped as `ci-smoke-latency`'s sibling `ci-smoke-sched`
> (`scripts/test_qos_sched.py`, CI job `scheduler-baseline`). A design panel refuted the naive metric —
> aggregate context switches are voluntary-yield-dominated, so `switch_count/tick` is both quantum-
> insensitive and host-throughput-dependent. The kernel instead grew a **dedicated `preempt_count`**
> incremented only at timer-quantum expiry (never on `SYS_YIELD`), exposed via a `SYSINFO_SCHED` sub-op
> / qsh `sched` command / `QosVM.sched()`, alongside a made-live `last_scheduled` and a per-PCB
> `sched_picks` for the fairness/tail snapshot. Calibration (WSL) confirmed **preemptions-per-1000-
> guest-ticks ≈ 1000/quantum, host-invariant** (~200 at q=5 idle+load with <0.5 % variance, ~50 at
> q=20); the gate asserts that scalar in `[100, 600]` plus a liveness floor, revert-confirmed by a
> `SCHED_QUANTUM_TICKS` 5→20 bump (rate drops to ~50, reddens the floor). Both prerequisites the fix was
> gated on now exist. **The latency fix itself (the I/O-priority-boost epic, or a measured quantum
> reduction) stays deferred** — it is now unblocked, with a proven latency gate AND a proven scheduler
> baseline to measure it against, but remains a future epic per this ADR's decision, not an autonomous
> increment.

## Context

Every agent-facing MCP tool that talks to a live VM round-trips over the COM2 serial
bridge — `qos_status` (STATUS), `qos_qpu_submit` (QSUBMIT), and the attestation handshake
(`QosVM.attest`) all block on a request→reply over that wire. So the COM2 round-trip latency
is the single largest floor on agent tool responsiveness.

While debugging the #201 attestation gate (a keyed STATUS retry that offset its nonce by one
on a slow CI runner), the reply latency was measured directly rather than assumed. Two clean
numbers, both low-variance:

- **PING → PONG: ~0.45 s.** The simplest possible round-trip — `swarm_svc` echoes the frame
  itself, no downstream hop (`dispatch_frame` → `FRAME_PONG`).
- **STATUS: ~1.35 s ≈ 3 × 0.45 s.** STATUS adds a `swarm_svc → ghostd → swarm_svc` IPC hop
  (the `GHOST_STATUS` query), so the round-trip crosses ~3 service scheduling boundaries.

The ~0.45 s quantum is not the serial hardware or `ghost_query`; it is the **scheduler
cadence**. The math closes exactly:

- `TIMER_DEFAULT_HZ = 100` → a 10 ms tick (`kernel/include/kernel/interrupts.h:95`).
- `SCHED_QUANTUM_TICKS = 5` → a 50 ms scheduling quantum per process
  (`kernel/include/kernel/scheduler.h:21`; the early-return at `kernel/src/scheduler.c:132`).
- ~9 runnable ring-3 citizens at rest (of the ~18-name roster in `kernel/src/citizens.c`).

`swarm_svc`'s main loop (`poll_com2(); qsub_poll_step(); heartbeat(); yield();`,
`user/swarm_svc.c`) yields voluntarily each pass, but its NEXT turn to read COM2 only comes
around once per full round-robin cycle ≈ `50 ms × ~9 ≈ 450 ms`. A one-hop PING costs one
cycle; a three-hop STATUS costs ~three. That is the whole latency.

## Decision

**Defer** reducing the latency. Do NOT change the timer HZ, the scheduler quantum, or the
service model as an autonomous increment. Record the root cause and the option space here,
and gate any future fix on two prerequisites that do not yet exist:

1. **A COM2 latency regression gate** — a CI smoke test that asserts a bounded PING/PONG (and
   STATUS) round-trip, so a fix can be *proven* and a future change can't silently regress it.
   Today there is no latency assertion, only functional ones.
2. **A perf/stability baseline** for the resonant scheduler — every candidate fit below is
   system-wide and interacts with `SCHED_RESONANT` (which has its own honest-measurement gate).
   An unmeasured change to a core scheduler constant is exactly the kind of edit this project's
   anti-vacuous-gate discipline (ADR-0016) exists to forbid.

## Options (for when the prerequisites exist)

| Option | Effect | Tradeoff |
|---|---|---|
| Lower `SCHED_QUANTUM_TICKS` (5 → 1–2) | ~90–180 ms/cycle; ~2.5–5× faster round-trips | 2.5–5× more context switches system-wide; perturbs `SCHED_RESONANT` fairness/measurement |
| Raise `TIMER_DEFAULT_HZ` (100 → 250/1000) | Finer scheduling granularity | More timer-IRQ overhead for every process; touches every timing assumption (audit ticks, cpu-limit, TCP timers) |
| I/O-priority boost | Reschedule a service the moment its awaited I/O (COM2 byte / IPC reply) is ready | A real new scheduler feature (run-queue + wake path), not a constant tweak — the highest value but the most work |
| Collapse the STATUS hop | `swarm_svc` answers STATUS without the `ghostd` IPC round-trip | Removes ~2/3 of STATUS latency but couples `swarm_svc` to ghostd's field state; changes the service model |

The I/O-priority boost is the principled fix (it targets latency without penalizing throughput
the way a blanket smaller quantum does), but it is a genuine scheduler feature and should be
its own epic behind the latency gate.

## Consequences

**Positive.** The latency is now a *known, measured, root-caused* characteristic with a
number (~0.45 s/hop) and a cause (round-robin cadence), not a mystery. Any future fix has a
concrete target and a clear first task (build the latency gate). The measurement also explains
the #201 nonce-offset failure mode (a retry deadline below the reply latency), already fixed
by pinning `attest()`'s per-attempt deadline above it.

**Negative.** Agents pay ~1.3 s per STATUS tool call and ~0.45 s per PING/QSUBMIT-hop today,
and this ADR ships no improvement — only the diagnosis. Multi-call agent workflows feel
sluggish in proportion to their COM2 chattiness.

**Residual risk.** The ~9-runnable-citizen figure is a rest-state estimate; under load (busy
citizens consuming full quanta) the cycle — and thus every tool's latency — grows. A latency
gate would also surface that tail, which the functional gates do not.

## Evidence

- Measured with a PING/PONG-vs-STATUS harness over the live COM2 socket (PING steady ~0.45 s
  ×5; STATUS ~1.35 s), 2026-07-11, during the #201 (PR 201) attestation debugging.
- Anchors: `kernel/include/kernel/scheduler.h:21` (`SCHED_QUANTUM_TICKS 5`),
  `kernel/include/kernel/interrupts.h:95` (`TIMER_DEFAULT_HZ 100`),
  `kernel/src/scheduler.c:132` (quantum early-return), `user/swarm_svc.c` (`poll_com2` loop).
- Latency gate `ci-smoke-latency` (`scripts/test_qos_latency.py`) — asserts the bounded PING median
  and records the STATUS baseline; revert-confirmed by a `SCHED_QUANTUM_TICKS` bump. It is prerequisite
  (1) above, now shipped, and the first increment of any future latency-reduction epic.
