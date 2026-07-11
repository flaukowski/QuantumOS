# 9. Authority Audit Ledger

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

Capabilities (ADR-0001) and declarative grants (ADR-0002) decide who holds what;
nothing yet made that authority — and every attempt to exceed it — *observable*
to an external verifier. Ring-3 self-reporting is worthless: a citizen that
overstepped is exactly the citizen you cannot trust to say so, and a citizen the
kernel killed (the CPU-quota hog) is not around to report anything. The record
therefore has to be kernel-written, and it has to survive four attacks on its
own integrity: forgery/suppression by citizens, eviction of old evidence by a
denial flood, amnesia across reboot, and offline tampering with the disk it
persists to. A fifth, subtler attack is the read path itself — a text dump that
clips `perms=0x1f` to `perms=0x1` lies to the host parser with a syntactically
valid line (kernel/src/audit.c:219-221).

## Decision

The kernel keeps an append-only **authority ledger** — explicitly *not* a
syscall trace (kernel/include/kernel/audit.h:10-30). Ten 1-based record kinds
(a zeroed .bss slot is never a real entry, audit.h:56-71): GRANT at every mint
(kernel/src/capability.c:155, 204), DENY at every cap-gated refusal (the
`authorize()` choke point, kernel/src/syscall.c:122-128), SPAWN
(kernel/src/audit.c:88-91), MDENY for a held-but-undeclared cap
(kernel/src/manifest.c:144, ADR-0007), QUOTA (manifest.c:157, 193), CPUKILL
(kernel/src/scheduler.c:118), REVOKE vs REAP — explicit withdrawal never
conflated with owner-death garbage collection (capability.c:305, 435) — QSUBMIT
(kernel/src/qpu.c:123), and UNLINK, a spawn-channel cap freed under its
*surviving* owner because the target died (capability.c:475; audit.c:102-108).
Successful exercise of held authority is deliberately unrecorded, with two
quota-bounded, coalescible exceptions: SPAWN and QSUBMIT (audit.h:17-20).

The ring is 256 × 40-byte entries (ABI pinned by `_Static_assert`, audit.c:12;
raised from 128 when REVOKE/REAP symmetry pushed a boot to ~143 entries,
audit.h:46-51). `audit_record` is self-contained irqsave because it fires from
syscall context *and* the IF=1 health monitor (audit.c:29-40). **Coalescing**
makes it flood-proof: an identical consecutive record bumps the last entry's
`count` and refreshes its tick instead of appending, so a denial flood cannot
evict older evidence in O(flood), and `seq` (== `total`) only advances on
genuinely new records (audit.c:46-63).

**SYS_AUDIT** (syscall 32) is uncapped read-only introspection — reading the
ledger mints and denies nothing, so it never perturbs its own counters
(syscall.c:1484-1512). The dump is line-atomic: each entry commits
whole-or-nothing against a budget that pre-reserves an `AUDIT: truncated=1`
tail (audit.c:222-266), and an unknown kind prints `?`, visibly unknown, never
EMPTY (audit.c:181-183).

**Durability**: the whole ring plus an `AUD1` geometry header (magic, version,
entry count, entry size, 64-bit total — audit.h:126-131) is serialized into a
10272-byte / 21-sector blob whose disk home is **frozen** at
`ata_sector_count() - 27`, static-asserted against drift, precisely so the
ledger survives field-geometry upgrades (kernel/src/ramfs.c:209-221). The sync
write is best-effort — "durable authority is an addition, never a hostage" of
the fs/field commit (ramfs.c:470-486) — with the superblock written last as the
single commit point (ramfs.c:502-508). Restore treats the disk as
attacker-writable offline: the descriptor must name exactly the recomputed
home and exact size, the whole blob must checksum, and `audit_load` revalidates
header geometry; anything off cold-starts honestly (ramfs.c:617-650,
audit.c:328-348, including a `total > 2^48` plausibility guard). On success
`seq` **continues across boots** — a genuine append-history — ticks are zeroed
(boot-relative; seq is the sole durable ordering key, audit.c:355-364), and
`coalesce_floor` is sealed to `total` so the first post-restore append can
never fold into and mutate a persisted prior-boot tail entry (audit.c:22-27,
49-53, 356-358). Restore runs before `core_services_init`, so a restored GRANT
provably came from disk — this boot has emitted zero entries yet
(ramfs.c:652-663).

## Consequences

### Positive
- Enforcement proofs become un-echoable: the CPUKILL entry is the load-bearing
  evidence for the CPU-quota gate because the hog is dead and cannot
  self-report (Makefile:985-989), and a citizen cannot imprint a forged
  `AUDIT:` line (reserved marker, scripts/test_qos_mcp.py:478-485).
- The flood defense is structural, not rate-limited: coalescing bounds an
  attacker's eviction power regardless of flood volume (audit.c:46-63).
- Cross-boot seq continuity turns the ledger into an authority *history*; the
  restore-boundary seal keeps prior-boot records immutable (audit.c:53).
- Ring eviction is honest: `AUDIT: total= dropped= capacity=` states exactly
  how much history is gone (audit.c:268-286).

### Negative
- **The ledger records whose holdings changed, NOT who acted** (audit.h:25-28):
  GRANT/REVOKE/REAP carry the owner pid, so a forced ancestor revoke is
  indistinguishable from a self-revoke. Tolerable while agentd is the single
  auditable delegator (ADR-0008); an actor-bearing record is the named
  precondition for any ring-3 SYS_CAP_REVOKE and for multi-delegator expansion
  (ADR-0020).
- **Deny coverage is now complete across gated authority sites.** Capless
  `sys_send`/`sys_send_to` used to return EPERM with no DENY record (they use raw
  `cap_find`, not `authorize()`) — a citizen probing IPC caps it does not hold was
  invisible to the ledger. Both now record an IPC DENY (naming the exact target for
  `send_to`, or ANY for the first-match `send`), gated by paradox-test's capless send in
  `ci-smoke-mcp`. The net syscalls (`SYS_UDP`/`SYS_TCP`/`SYS_RESOLVE`) were **never** a
  gap: they go through `authorize()`, so a capless attempt already records a `DEV:NET`
  DENY (an earlier revision of this ADR mis-stated them as unaudited).
- No EXPIRE kind: expired caps are refused but never freed until owner death
  (audit.h:20-23), so absence of an entry does not mean the cap stayed live.
  TRANSFER is likewise unrecorded because `cap_transfer` has no caller
  (audit.h:24; capability.c:208-229) — a future caller that forgets the
  mandated REVOKE(from)+GRANT(to) pair would ship an invisible authority move.
- Durability is sync-granular, not per-record: everything since the last
  `sync` dies with a crash, and a failed audit write is only a boot_log line
  (ramfs.c:483-485, 514-519).
- Coalescing trades per-occurrence timestamps for flood-proofing: a coalesced
  run keeps only its latest tick (audit.c:59).

### Residual risks
- The CI gates read boot evidence through the 256-entry window (audit.h:49-51);
  entry-rate growth (more citizens, more REVOKE/REAP churn) can silently push
  MDENY/QUOTA/selftest evidence past the ring before the gate greps it —
  the ring size is a maintenance parameter, not a solved problem.
- The blob integrity check is an additive checksum plus geometry, not a MAC
  (ramfs.c:643-647): an offline attacker who preserves both restores as truth.
  The threat model already concedes the disk to the attacker; the guard is
  anti-corruption, not anti-forgery.
- The frozen home is load-bearing: any change to `audit_home_lba()` makes
  upgrades discard valid ledgers as "implausible" (ramfs.c:209-218). Two
  static asserts and the upgrade gate defend the invariant, but it constrains
  all future disk-layout work (ADR-0006's field reserve exists to protect it).
- SYS_AUDIT is uncapped by design (grants are already public in the model),
  so any citizen reads every pid's full authority history — a disclosure
  stance, acceptable single-tenant, that a multi-tenant future must revisit.

## Evidence
- Shipped in: PR #134 — the ledger, coalescing, and SYS_AUDIT (epic #133 Phase D)
- Shipped in: PR #136 — MDENY + QUOTA kinds (intent manifest, epic #135)
- Shipped in: PR #145 — CPUKILL kind (CPU-quota enforcement, epic #144)
- Shipped in: PR #146 — AUD1 durable blob, restore guards, coalesce_floor
- Shipped in: PR #147 — REVOKE vs REAP split; ring raised 128→256
- Shipped in: PR #152 — QSUBMIT kind (QPU job broker, epic #148)
- Shipped in: PR #159 — line-atomic dump + reserved truncated=1 tail (bug-hunt)
- Shipped in: PR #175 — UNLINK kind (spawn-channel teardown)
- Shipped in: PR #183 — frozen-home disk-upgrade CI gate (epic #177)
- Key code: kernel/include/kernel/audit.h:10-30 (model + honest scope), :56-71
  (kinds), :46-52 (ring sizing); kernel/src/audit.c:42-78 (coalescing append),
  :22-27 (coalesce_floor), :222-266 (line-atomic format), :303-367
  (serialize/load); kernel/src/syscall.c:122-128 (authorize DENY hook),
  :1489-1512 (SYS_AUDIT); kernel/src/ramfs.c:209-221 (frozen home), :470-486
  (best-effort sync), :624-664 (paranoid restore + content proof).
- CI gates: `ci-smoke` (MDENY self-test Makefile:965, `QUOTA ENFORCED`
  Makefile:974, `CPUKILL: pid=` Makefile:992, CAPUNLINK Makefile:481);
  `ci-smoke-disk` (boot-1 `AUDIT: synced ledger` Makefile:1243, boot-2
  `restored ledger` + prior-boot GRANT Makefile:1292-1302, corrupt-blob
  isolation Makefile:1303-1331); `ci-smoke-disk-upgrade` (descriptor frozen at
  LBA count-27 Makefile:1388-1397); `ci-smoke-mcp` (capacity=256, seq strictly
  increasing, QRNG DENY coverage, CPUKILL ledger entry, forged-AUDIT-line
  refusal — scripts/test_qos_mcp.py:348-371, 395-485).
