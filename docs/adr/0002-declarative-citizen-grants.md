# 2. Declarative Citizen Grants

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

Every nontrivial ring-3 citizen needs authority — a console, the quantum pool, a
field region, the spawn right. The capability model (ADR-0001) gives processes no
ambient authority: `process_create` mints exactly one root capability, full perms
over the process itself. Something has to wire the rest, and two failure modes
constrain how. First, the watchdog: a monitored service that crashes is respawned
as a *fresh pid*, so any authority minted once at boot is silently lost on rebirth —
ghostd would degrade from qseed-derived noise to a plain PRNG forever after its
first restart. Second, scheduling: `process_create` enqueues the child READY
immediately, and the health monitor runs as an IF=1 kernel thread, so a reborn
service could be preempted-in and *scheduled before its caps exist* — a console
EPERM at the first prompt write is shell suicide — and if the reaper freed the
half-built PCB first, caps would be minted for a dead, recyclable pid and never
revoked (kernel/src/service.c:156-163). The intent-manifest layer (epic #135)
added a third constraint: the declared-intent rows must never diverge from, or be
half-committed relative to, the minted capabilities.

## Decision

Authority is **declared, not scripted**. A citizen's grants live as `grant_*`
flags and quota fields in its static `service_definition_t`
(kernel/include/kernel/service.h:90-188): `grant_quantum_pool`, `grant_com2`,
`grant_console`, `grant_spawn`, `grant_fswrite`, `grant_net`, `grant_field` (+
`field_region`, `field_region_span`, `field_inherit`, `grant_field_delegable`),
`grant_spawn_channel`, `grant_qpu_submit`/`grant_qpu_execute`, and the quotas
`spawn_max`/`cpu_limit`/`qsub_max`.

`start_slot` resolves the declaration on **every** start — first spawn and every
watchdog rebirth alike (kernel/src/service.c:241-245) — and commits the whole
sequence **spawn → cap mints → manifest bind → pid/generation record atomically
under cli** (kernel/src/service.c:198-202, 235-457, 459-468). The intent manifest
is built *from the same grant flags, inside the same cli window*, and
`manifest_bind` last-write-wins over the restrictive default that
`finalize_user_process` bound at spawn (kernel/src/service.c:370-378, 456). Field
grants use one normalized span that drives both the cap-mint loop and the
manifest-row loop (kernel/src/service.c:314-318); an out-of-range span fails the
whole grant closed — no caps, no rows (kernel/src/service.c:320-327) — and row
appends past `MANIFEST_MAX_ENTRIES` drop loudly instead of overrunning `entries[]`
(kernel/src/service.c:425-455). Every field region is scrubbed before mint, with
`field_inherit` as the single per-boot, per-region exception, consumed only after
a successful mint (kernel/src/service.c:329-365). QPU submit and execute masks
are disjoint by construction (kernel/src/service.c:269-289).

Supervision is split **monitored vs proof citizen**. Monitored services heartbeat;
`health_monitor_scan` (1 s interval, 2 s timeout — service.h:36-38) marks a silent
RUNNING service CRASHED and restarts it (kernel/src/service.c:629-658). Proof
citizens are deliberately *not* monitored, because a respawn would re-run or
destroy their proof: quota-test's per-incarnation spawn quota would reset
(kernel/src/citizens.c:575-581), subagentd's rebirth would rebind its manifest and
drop the delegated row, and delegation-test must be free to EXIT so the reaper
cascade-revokes its derived cap (kernel/src/citizens.c:607-613). The kernel
enforces one such split itself: `service_monitor` **refuses** a `cpu_limit`
service — the CPU-quota kill plus watchdog respawn would be a restart-bounded
kill/respawn churn (kernel/src/service.c:583-591). When a monitored service
exhausts its restart budget, `service_restart` **reclaims** the hung process via
`service_stop` instead of leaking it RUNNING-forever (kernel/src/service.c:537-548);
`service_stop`'s guard→retire sequence is generation-checked and cli'd against the
reaper/recycle TOCTOU (kernel/src/service.c:498-525).

Finally, the roster itself was split out: `citizens.c` owns the boot **wiring**
(who declares which grants — one `user_*_init` per citizen, `user_init()` the
single dispatcher), while syscall.c keeps the enforcing **mechanism**; citizens
reach it only through `user_process_spawn_elf`/`service_*`
(kernel/src/citizens.c:1-14, 73-114; PR #179).

## Consequences

### Positive
- The authority map reads in one place. Who may delegate (only delegation-test
  and agentd set `grant_field_delegable`, service.h:140-147), who may spawn, who
  holds a device — all reviewable as static struct literals in citizens.c, e.g.
  qsh's full set (kernel/src/citizens.c:512-539) or agentd's budgeted 7-of-8
  manifest rows (kernel/src/citizens.c:817-822).
- Rebirth restores identical authority: a watchdog-reborn qsh keeps its console,
  ghostd honestly recovers its entropy source (kernel/src/service.c:241-245).
- Intent and authority cannot diverge or half-commit — same flags, same
  normalized span, same cli window (kernel/src/service.c:370-374, 466-468).
- The monitored/proof split makes enforcement gates un-echoable: quota-test and
  cpu-hog print their tokens only on the exact expected outcome, and no respawn
  can launder a second attempt.

### Negative
- The roster and the grant vocabulary are **static and closed**: no runtime
  service registration from ring 3, and every new right is a kernel change (a new
  `grant_*` flag plus mint/manifest arms in start_slot). Runtime authority growth
  exists only via SYS_CAP_DERIVE, one hop, delegator-bounded (ADR-0010).
- Boot-time hand-wired IPC *peer* caps (qsh↔ghostd, fieldsyncd↔ghostd, …) are
  **not** part of the declaration and are not re-minted on watchdog restart — a
  documented service.c limitation (kernel/src/citizens.c:505-507); a reborn qsh
  keeps its console but loses its `ghost` builtin.
- Individual cap-mint failures inside start_slot log-and-continue
  (kernel/src/service.c:246-313): a service can come up with *partial* authority
  and only a boot_log line to show for it. Only the field-span bounds check fails
  the grant closed.

### Residual risks
- The manifest row budget is 8; agentd already declares 7 (quantum 1 + spawn 1 +
  FIELD 3-6 + QPU 1, kernel/src/citizens.c:817-822). The guard drops overflow
  rows fail-closed, but a dropped row means a held-but-undeclared cap that the
  manifest layer will then deny — grant additions must budget rows first.
- Quotas are per-incarnation by design (`spawn_max` resets on rebirth,
  service.h:158-164) — a soft bound, acceptable only because the quota-proof
  citizens are unmonitored.
- Liveness is heartbeat-only: a busy-but-alive monitored service that misses its
  2 s heartbeat is indistinguishable from a hung one and gets restarted.
- The cli window covers spawn plus every mint plus the manifest bind; its length
  grows with the grant count. Fine at 32 services (service.h:30) on one CPU, but
  it is interrupt latency spent per (re)start.

## Evidence
- Shipped in: PR #136 (intent manifest bound in start_slot + first enforced quota, epic #135)
- Shipped in: PR #138 (`grant_field_delegable` → SYS_CAP_DERIVE, epic #137)
- Shipped in: PR #145 (`cpu_limit` quota + the monitor refusal, epic #144)
- Shipped in: PR #166 (watchdog give-up leak — budget exhaustion now reclaims)
- Shipped in: PR #175 (`grant_spawn_channel`, opt-in spawn-time IPC pairs)
- Shipped in: PR #177 (`field_region_span`, normalized span + fail-closed rows)
- Shipped in: PR #179 (roster split: citizens.c wiring vs syscall.c mechanism)
- Key code: kernel/src/service.c:156-171 (atomicity rationale), :198-202/:466-468
  (the cli commit), :241-245 (re-mint on every start), :314-368 (span, scrub,
  inherit, delegable), :370-457 (manifest from the same flags), :498-548 (stop
  guard + give-up reclaim), :583-591 (cpu_limit monitor refusal), :629-658
  (health monitor); kernel/include/kernel/service.h:90-188 (the declaration
  vocabulary); kernel/src/citizens.c:73-114 (roster order), :512-565 (qsh),
  :567-601 (quota-test), :614-661 (delegation pair), :674-693 (cpu-hog),
  :781-851 (agentd).
- CI gates: `ci-smoke` (Makefile:423) greps `QSH: reborn` (watchdog rebirth of
  the operator surface, Makefile:1145), `QUOTA ENFORCED pid=` (Makefile:974),
  `CPUKILL: pid=` (Makefile:992), and `SVCGIVEUP: restart budget exhausted
  reclaims the process` (Makefile:537; emitted by service_selftest,
  kernel/src/service.c:791-808).
