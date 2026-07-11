# 8. Intent Manifests

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

Capabilities (ADR-0001) answer "may this process touch this resource right now" —
they say nothing about what a process was *registered* to touch. On the shipped
system that distinction is invisible: every cap a citizen holds was minted from its
declarative grant flags (ADR-0002). But two developments made the gap load-bearing.
First, delegation (ADR-0010): once SYS_CAP_DERIVE makes capabilities transferable at
runtime, holding a cap no longer proves the holder was ever supposed to have it, and
the system needs an outer bound that delegation is checked against. Second, budgets:
capability grants are binary — nothing stopped a spawn-capable citizen from forking
until the process table filled, or a QPU client from submitting circuits forever.
Epic #135 (Phase D increment 2) added the missing layer: an explicit, kernel-held,
inspectable record of declared intent, plus the machinery to hang enforced quotas on.

## Decision

Give every ring-3 process a kernel-held `manifest_t`: a 128-byte record
(`_Static_assert`, manifest.h:88) holding an allow-set of up to `MANIFEST_MAX_ENTRIES`
= 8 rows of `{resource_type, resource_id, permissions}` (manifest.h:54, 65-86) plus
quota counters (`spawn_max/used`, `cpu_limit/cpu_ticks`, `qsub_max/used`). The table
lives in .bss keyed by pid (manifest.c:17-19); ring 3 never sees or edits it.

**Unbound = allow, made safe by a choke point.** A pid with no bound manifest passes
every check (manifest.c:130-132). This is safe only because `finalize_user_process` —
the single point all three spawn paths (flat blob, ELF, SYS_SPAWN) funnel through —
binds a restrictive default (`bound=1`, zero rows, zero quotas) to every ring-3
process (syscall.c:2046-2061). "Unbound" is therefore provably ring 0, which never
traverses int 0x80. Services then overwrite the default from the *same* grant flags
that mint their caps, inside the *same* cli window (service.c:370-457), so intent and
authority are never half-committed and a watchdog rebirth re-binds exactly as it
re-mints. `manifest_bind` is deliberately last-write-wins (manifest.c:44-49): a
bind-once "hardening" would leave a watchdog-reborn qsh holding a child's manifest.

**Two-layer authorization.** `authorize()` (syscall.c:122-128) checks the capability
layer first (a missing cap records AUDIT_DENY), then `manifest_check` — a *held* cap
on a bound manifest with no matching `(resource_type, resource_id)` row records
AUDIT_MDENY and is refused (manifest.c:124-146). Matching is membership-only:
permission bits are recorded for inspection but never matched (manifest.h:68), so the
two layers cannot diverge on perm semantics. IPC is deliberately outside the manifest
— IPC caps are pair-wise runtime wiring, not declarative intent (manifest.h:20-23;
spawn-channel mints skip `manifest_grant` for exactly this reason, syscall.c:1250-1253).

**Three enforced quotas**, each with a distinct charge discipline:
- **spawn_max** (epic #135): `manifest_spawn_precheck` runs before *any* spawn side
  effect and records AUDIT_QUOTA on refusal (syscall.c:1185-1192, manifest.c:148-161);
  the charge lands only after `spawn_elf_args` succeeds (syscall.c:1265-1268) — a
  typo'd path costs no quota. Per-incarnation: rebirth rebind resets the counter.
- **cpu_limit** (epic #144): `manifest_tick` charges the interrupted pid in
  `scheduler_tick` before the quantum early-return (scheduler.c:98-104). Enforcement
  is a latch: if the interrupted context is ring 3 (`(state->cs & 3) != 0` — never
  kills mid-syscall in ring 0) and a runnable successor exists, the process is
  terminated with AUDIT_CPUKILL and its manifest cleared synchronously
  (scheduler.c:117-128); a skipped tick retries on the next ring-3 interrupt.
- **qsub_max** (epic #148): precheck first in `qpu_submit` (qpu.c:75-77), charge as
  the terminal accept step followed by AUDIT_QSUBMIT (qpu.c:122-123). Charged at
  submit-accept; EAGAIN/EINVAL never consume, later execution failure never refunds.

**Inspectability.** SYS_MANIFEST (syscall 33) is uncapped read-only introspection in
the SYS_AUDIT class (syscall.c:1524-1540): bound pids only, line-atomic emission with
a reserved `MANIFEST: truncated=1` tail, and the qsub field appended last because host
parsers are prefix-anchored (manifest.c:286-370, 317-319).

**Non-vacuity by self-test.** Because shipped citizens' caps and manifests are minted
from the same grants, no citizen ever trips MDENY in normal operation — so
`manifest_selftest` (manifest.c:382-432) drives the *real* helpers to a real deny and
a real quota refusal at boot, recording the AUDIT_MDENY and AUDIT_QUOTA entries the CI
gate parses. Without it, a constant-true `manifest_check` would ship green.

## Consequences

### Positive

- Delegation has an outer bound: SYS_CAP_DERIVE refuses to delegate a resource the
  delegator's own manifest does not declare (syscall.c:1619-1623), pre-checks
  recipient room before minting so the extension can never fail after a cap exists
  (syscall.c:1641-1647), and extends the recipient's manifest so the delegated cap is
  usable (syscall.c:1655-1657). See ADR-0010.
- Quotas are real bounds, not accounting: the second over-quota spawn is EPERM, the
  cpu_hog citizen is terminated, the third over-quota QPU submit is refused — all
  three proven by CI gates against boot output.
- Recycled-pid hygiene: `manifest_clear` whole-entry memsets from `process_destroy`
  and at CPUKILL, so a reused pid never inherits predecessor rows or counters
  (manifest.c:57-66, process.c:484, scheduler.c:126).
- Row appends fail closed: span-derived field rows and QPU rows that would exceed the
  8-row bound are dropped with a boot log, not written past `entries[]`
  (service.c:425-455).

### Negative

- **v1 intent largely mirrors the cap set.** For every shipped citizen the manifest
  is derived from the same grant flags as the caps, so the intent check refuses
  nothing today (manifest.h:14-25). Its present value is explicitness (SYS_MANIFEST),
  the quota substrate, and the delegation bound — the deny path stays honest only via
  the boot self-test.
- Permission bits in rows are decorative: recorded, reported, never matched. An
  operator reading SYS_MANIFEST output may reasonably over-read them as enforced.
- The 8-row bound became def-dependent once field spans landed (epic #177): a def
  combining a span with many grants silently loses rows (fail-closed), narrowing its
  usable field span with only a boot log as witness. agentd sits at 6/8.
- `cpu_limit` is 0 (unlimited) for every shipped citizen (manifest.h:119-125);
  CPUKILL is exercised only by the cpu_hog test citizen, so the enforcement path has
  no production traffic.
- qsub quota is a lifetime counter charged at accept with no refund on execution
  failure — a flaky backend burns a citizen's declared budget.

### Residual risks

- Any future spawn path that bypasses `finalize_user_process` silently reopens the
  unbound=allow hole for ring 3 (warning at manifest.h:31-32). The invariant is
  structural, not checked at runtime.
- IPC being outside the manifest inherits the audit ledger's documented IPC deny gap
  (audit.h:28-30): IPC ownership refusals are neither manifest-bounded nor audited.
- `cpu_ticks` in SYS_MANIFEST output is a documented, deliberate timing side channel
  (syscall.c:1517-1519).
- `manifest_check` returns allow for pids >= MAX_PROCESSES (manifest.c:126-128) on
  the grounds the cap layer already vouched for the pid; the manifest layer is not an
  independent defense there.

## Evidence

- Shipped in: PR #136 — intent manifest + spawn quota + SYS_MANIFEST (epic #135, Phase D inc. 2)
- Shipped in: PR #138 — SYS_CAP_DERIVE checks/extends manifests (epic #137; ADR-0010)
- Shipped in: PR #145 — cpu_limit becomes a real bound: CPUKILL (epic #144)
- Shipped in: PR #152 — QPU broker + qsub quota (epic #148)
- Shipped in: PR #159 — trust-core bug-hunt fixes across capability/ipc/manifest/audit
- Key code: kernel/include/kernel/manifest.h:54-88 (model + 128-byte layout);
  kernel/src/manifest.c:124-146 (check/MDENY), 148-172 (spawn quota), 184-208 (qsub
  quota), 210-218 (tick), 286-370 (format), 382-432 (self-test);
  kernel/src/syscall.c:122-128 (authorize), 1181-1270 (spawn precheck/charge),
  1524-1540 (SYS_MANIFEST), 2046-2061 (default-bind choke point);
  kernel/src/service.c:370-457 (grant-derived bind); kernel/src/scheduler.c:98-128
  (tick accounting + CPUKILL); kernel/src/qpu.c:72-125 (qsub precheck/charge)
- CI gates: `ci-smoke` legs — manifest self-test "PASS (MDENY recorded)"
  (Makefile:960-970), spawn quota "QUOTA ENFORCED"/no "QUOTA BROKEN"
  (Makefile:971-984), "CPUKILL: pid=" (Makefile:985-997); `ci-smoke-qsubmit` —
  "QPU: quota ENFORCED (third submit refused)" (Makefile:1069); integration shell
  drive exercises the `manifest` command and re-checks QUOTA BROKEN absence
  (.github/workflows/ci.yml:849, 867-874)
