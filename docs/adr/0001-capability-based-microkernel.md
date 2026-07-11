# 1. Capability-Based Security as the Sole Kernel Authority Model

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

QuantumOS is a single-CPU x86-64 microkernel whose ring-3 population is not a set of trusted
utilities but a society of autonomous citizens — services, agents, and deliberately hostile test
processes — that spawn each other, exchange messages, and (later) delegate authority at runtime.
An ambient-authority model (UNIX-style uid checks, or "root can do anything") makes the
interesting questions unanswerable: *who* may touch the COM2 swarm bridge, *why* does this pid
get to spawn, and *what exactly* did a compromised citizen lose when it died? The kernel needed
an authority primitive that is explicit, attenuable, revocable, and cheap enough to check on
every syscall — and a way to prove at every boot that the checks are real rather than decorative.

## Decision

Capabilities are the **only** authority model in the kernel. There is no uid, no superuser bit,
and no ACL; a syscall that exercises authority either finds a covering capability or returns
`SYSCALL_EPERM`.

1. **Kernel-private table, opaque handles.** All capability state lives in a 1024-slot
   kernel-private table (`MAX_CAPABILITIES`, capability.h:28; `cap_table`, capability.c:27).
   Ring 3 only ever sees an opaque `cap_id` laid out as `(generation << 16) | (slot_index + 1)`
   (capability.c:62-64). `resolve()` rejects any handle whose embedded generation does not match
   the slot's (capability.c:67-85), and `free_slot` bumps the generation on every free
   (capability.c:111) — a stale handle to a revoked capability fails validation instead of
   aliasing whatever reuses the slot. `CAP_ID_INVALID` is 0, so a zeroed field is never a valid
   handle (capability.h:29).
2. **Seven resource types, eight permission bits.** MEMORY, IPC, DEVICE, QUANTUM, PROCESS,
   SERVICE, FIELD (capability.h:44-53) crossed with READ/WRITE/EXECUTE/GRANT/REVOKE/QUANTUM/
   DEVICE/PROCESS (capability.h:32-39). GRANT gates derivation; REVOKE gates revoking children.
3. **No ambient authority at birth.** `process_create` mints each process exactly one root
   capability: `CAP_PERM_ALL` over *itself* (`CAP_RESOURCE_PROCESS`, resource_id = own pid) —
   "Everything else must be derived or explicitly granted" (process.c:381-388). Service grants
   are minted per-definition by `start_slot` at service start; plain-spawned processes get
   nothing beyond the root cap.
4. **EPERM-by-default, gated per syscall.** The choke point is
   `authorize(pid, rtype, perm, rid)` (syscall.c:122-128): capability layer first
   (`cap_find_resource`; a miss records `AUDIT_DENY` and fails), then the intent-manifest layer
   (ADR-0008). Sixteen call sites gate every device, disk-write, net, spawn, field, and QPU
   syscall (e.g. SYS_COM2 write at syscall.c:402, SYS_SPAWN at syscall.c:1182); IPC sends use
   the capability itself as the address (`cap_find`/`cap_find_resource` over IPC caps,
   syscall.c:230-291), and SYS_QRAND/SYS_QSEED gate manually on the quantum-pool capability.
   The remaining uncapped syscalls (GETPID, YIELD, EXIT, RECV into one's own mailbox, SYS_AUDIT,
   SYS_MANIFEST, …) name no authority — they only report or act on self.
5. **Attenuation-only derivation.** `cap_derive` requires the requester to own a non-expired
   parent carrying `CAP_GRANT` (capability.c:169-177), refuses any child bit the parent lacks
   with `CAP_ERROR_ESCALATION` (capability.c:179-181), and clamps child expiration to the
   parent's (capability.c:183-185). A derived cap records `parent_cap` and can therefore be
   found and killed by cascade.
6. **Cascading revocation, three distinct ledger meanings.** `cap_revoke` authorizes the owner
   or any ancestor holding `CAP_REVOKE`, then recursively frees the derivation subtree
   (`revoke_children_of`, capability.c:242-263) before the root (capability.c:296-306). Owner
   death sweeps everything via `cap_revoke_all_for_process` under `AUDIT_REAP`, never
   `AUDIT_REVOKE` — garbage collection must not masquerade as an exercised revoke
   (capability.c:418-438); spawn-channel unlink records `AUDIT_UNLINK` (ADR-0009).
7. **Correct under the one real race.** Syscalls run cli'd, but the service health monitor
   revokes at IF=1; the two structural mutators are bracketed by self-contained
   irqsave/irqrestore (capability.c:40-54, 88-114), and cascade frees copy ledger fields out
   *before* `free_slot` so a first-fit reuse cannot misattribute the record (capability.c:247-254).
   Read lookups scan only `[0, cap_hwm)` — a monotone high-water mark that can only cause a
   false deny, never a false grant (capability.c:29-38).
8. **Proof by attack, not by assertion.** The boot roster permanently ships its own attackers:
   `user-rogue` reads supervisor-mapped kernel memory from ring 3 and must page-fault into
   containment (user_blob.S:103-128), and `ghost-test` holds only an IPC cap to ghostd and
   attempts capless SYS_QRAND, SYS_COM2, and SYS_CONS, expecting EPERM (-4) on all three
   (user/ghost_test.c:48-84). CI greps for the denial strings; a kernel that stops denying
   goes red.

## Consequences

### Positive

- Fail-closed by construction: forgetting to grant produces a visible EPERM plus an
  `AUDIT_DENY` ledger entry, not silent access.
- Generation tags make use-after-revoke a validation failure rather than a confused-deputy
  aliasing bug, at zero per-slot allocation cost.
- The model layered cleanly under three later systems without rework: intent manifests bound
  what a *held* cap may do (ADR-0008), the authority ledger observes mint/deny/revoke at the
  capability layer itself (ADR-0009), and SYS_CAP_DERIVE reuses `cap_derive`'s escalation and
  expiry guards verbatim for cross-process delegation (ADR-0010).
- Every boot re-proves the mechanism: `cap_selftest` panics the boot on failure (main.c:307-309),
  including denial legs for wrong pid/resource/perm (capability.c:534-539), the escalation
  refusal, and the anti-vacuous CAPHWM and CAPUNLINK gates.

### Negative

- All lookups are linear scans; the hwm bound (PR #170) amortizes the sparse-table case but a
  full table still costs O(1024) per gated syscall. Acceptable at 256 processes, not proven
  beyond.
- The 1024-slot table is a single shared budget: `CAP_ERROR_NO_SPACE` is a shared-fate failure
  one cap-hungry citizen can inflict on everyone. sys_spawn prechecks capacity before minting
  channel pairs (syscall.c:1193-1209), but nothing rate-limits table consumption generally.
- `cap_transfer` exists, is dead code, and is deliberately unaudited — the comment at
  capability.c:208-210 forbids any caller until it emits REVOKE(from)+GRANT(to);
  `stats.transferred` is permanently 0.
- Untargeted SYS_SEND routes to the *first-matching* IPC cap (capability-as-address,
  capability.c:347-369), so a citizen holding two send caps has table-order-dependent routing;
  this is why spawn channels are opt-in (ADR-0009 §UNLINK, service.h rationale).
- `CAP_RESOURCE_MEMORY` and `CAP_RESOURCE_SERVICE` are declared types with no authorize() site;
  MEMORY appears only in self-tests. The type space is wider than the enforced surface.

### Residual risks

- The 16-bit generation counter wraps after 65,536 revocations of a single slot
  (capability.c:23, 111); a hoarded stale handle could then alias. No shipped workload
  approaches this, and no gate watches for it.
- Expired capabilities are refused at check/find/derive (capability.c:116-118) but their slots
  are reclaimed only at owner death — a long-lived process minting short-lived caps leaks slots
  until it dies, and there is no EXPIRE ledger kind (audit.h:20-23).
- IPC and net *ownership* denials return EPERM without an audit record (sys_send,
  syscall.c:239-241; sys_send_to, syscall.c:273-275) — a probe against the IPC surface is
  invisible to the ledger (documented gap, audit.h:28-30; see ADR-0009).
- The ledger records whose holdings changed, not who acted (ADR-0009); a forced ancestor revoke
  and a self-revoke are indistinguishable today.

## Evidence

- Shipped in: PR #3 (original capability system, e69731b); capability-checked IPC syscalls
  (ac0b6db); authority ledger wiring, epic #133 Phase D (d25cf9e); PR #138 (SYS_CAP_DERIVE,
  epic #137); PR #147 (REVOKE vs REAP lifecycle); PR #159 (6 adversarial bug-hunt fixes,
  including capture-before-free); PR #170 (high-water-mark lookup bound); PR #175
  (spawn-channel origin tag + unlink).
- Key code: capability.c:62-85 (handle layout + resolve), capability.c:88-114 (irqsave
  alloc/free), capability.c:159-206 (attenuation-only derive), capability.c:242-308 (cascading
  revoke, capture-before-free), capability.c:310-336 (cap_check), syscall.c:122-128
  (authorize), process.c:381-388 (root capability, "no ambient authority"), capability.h:28-53
  (table size, perm bits, resource types).
- CI gates: `cap_selftest` boot-panic gate (main.c:307-309) with CAPHWM (capability.c:501-526)
  and CAPUNLINK (capability.c:572-621) legs — every CI boot runs them; integration Test 1c
  `QRAND: capless caller denied (EPERM)` (ci.yml:743); Test 1e `PARADOXD: capless send denied
  (EPERM)` (ci.yml:779); Test 1f `COM2: capless caller denied (EPERM)` (ci.yml:797); qsh
  interactive gate asserting `CONS: capless caller denied` (ci.yml:852).
