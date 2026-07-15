# 14. Distributed Field Societies Over N VMs

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

The kernel field (ADR-0006) and ghostd's ring-3 oscillator field give one machine an
associative memory. The mission's scaling axis is not more cores (ADR-0005 rejects
SMP) but more *nodes*: a society of attested agents in separate VMs that share
resonant state. The question is how independent kernels — each seeded from its own
quantum entropy, so each starting in a different phase — come to a shared field
without a central coordinator, and how honest that coupling is about what it trusts.

## Decision

Kernels couple their ghostd oscillator fields by **Kuramoto phase synchronization
over a shared L2**. Two nodes use a directed QEMU UDP socket pair; three or four use
a multicast group (230.0.0.9) so every guest sees every frame with one NIC each and
ARP works (kernel/include/kernel/net.h:34-38). `fieldsyncd` exchanges a 260-byte
`FSYN` frame carrying all 256 phases about once per second (user/fieldsyncd.c:28).

ghostd aggregates peers as a **mean field**, not an overwrite: per-peer phase slots
are keyed by source IP (`remote_phase[GHOST_MAX_PEERS][256]`, user/ghostd.c:97-123),
and the coupling fold is `(K/P)·Σ_p sin(θ_p − θ_i)` — a sum of sines of *differences*,
which is wrap-safe and byte-identical to the single-peer path at P=1. The fold cadence
is capped (`MIN_FOLD_TICKS`) so the total gain is independent of peer count, and the
live-peer set is floored at 1 so P=0 can never divide (that divide is reachable at
P=1 — a panel blocker). The synchronization verdict is the **minimum** pairwise order
parameter R_x, not the mean: a mean hides one diverging node, a minimum does not.
`GHOST_MAX_PEERS` (4) must equal the kernel's `MAX_PEERS` — two separately compiled
constants kept in lockstep by hand (user/ghost.h:50, net.h:38).

A **society of societies** goes one level up: each VM runs its own sub-agent society
(ADR-0011) and exchanges a qseed-salted `FSYP` aggregate (a fixed 16-byte frame,
user/fieldsyncd.c:40-49) with the other VM. The v1 design that replicated results
*into* the field was rejected wholesale by a 29-finding panel — replicated mailboxes
fight the field's one-owner scrub/eviction. The shipped design stores **nothing** in
the field: FSYP values are printed and host-verified, never imprinted
(user/fieldsyncd.c:260-283). The host is the admission root — it attests each node
over COM2 and admits it to the L2.

## Consequences

### Positive
- Genuinely decentralized convergence: N kernels with distinct qseeds lock a shared
  field with no central clock, proven from a divergent start (R_x < 0.5 → ≥ 0.80).
- The min-pairwise verdict is non-vacuous — it cannot report SYNCHRONIZED while any
  node is out (user/ghostd.c:192-210).
- Gain is peer-count-independent, so adding a node does not destabilize the fold
  (the cadence cap, not the sum, controls energy).
- The society-of-societies trust boundary is explicit: cross-VM results are
  host-verified data, never trusted field content.

### Negative
- **Coupling and attestation are cryptographically UNLINKED** — the MCP server
  confesses this in every society status payload (scripts/qos_bridge.py:1402-1403).
  A node's *boot* is Lamport-attested (ADR-0015); its *field frames* are not. The
  only defense is a source-IP filter (`in_peer_set`, user/fieldsyncd.c:100-107),
  which is trivially spoofable on a shared L2. Authenticating the wire is ADR-0019.
- The two lockstep constants (`GHOST_MAX_PEERS` / `MAX_PEERS`) are a manual
  invariant across compilation units — a mismatch is a silent buffer bug, guarded
  only by a comment.
- The multicast L2 is a QEMU host-side construct; it proves the *protocol* at N, not
  a real multi-machine network with routing, loss, or reordering beyond what SLIRP
  models.
- FSYP frames resend continuously (idempotent) rather than acknowledging — a lost
  frame is covered by the next resend, not retransmit logic; convergence is
  eventual, not bounded-latency.

### Residual risks
- MAX_PEERS is 4: the society does not scale past four nodes without lifting a
  kernel constant that is lockstepped to a ring-3 array (a wall, not a solved
  problem).
- Watchdog-reborn `fieldsyncd`/peer IPC caps are not re-minted on restart
  (user/fieldsyncd.c:333-339) — a standing reliability gap that ADR-0019 turns into
  a key-distribution outage. **Closed (2026-07-15):** ADR-0023 re-mints peer IPC
  caps declaratively on every start, and swarm_svc re-forwards the cached group
  key on a `fieldsyncd` pid change — a rebirth is now a re-admission, not an outage.

> **Update (2026-07-15) — the N=4 configuration is now proven, and the ceiling
> stated precisely.** The societies epic adds `ci-smoke-society4-gate`
> (`scripts/test_qos_society4.py`): four attested kernels on a shared mcast L2,
> the full 4-cycle mesh (12 directed frame observations), four distinct
> identities, min-pairwise R_x ≥ 0.80 from divergent per-node starts, clean reap.
> The gate deliberately does **not** assert slot-full/eviction behavior: at N=4
> each node has N-1 = 3 peers against a 4-slot ghostd table (3/4 occupied), so
> the slot-full and eviction branches are structurally unreachable and have no
> console observable — an "assert no eviction" would be vacuous. The binding
> ceilings, precisely: the host bridge's `_NET`/`_MAC` arrays are 4 entries (the
> wall this gate sits at); the `GHOST_MAX_PEERS`/`MAX_PEERS` = 4 constants bound
> *peers per node* to 4 and would actually admit **N = 5** (a 5th node means 4
> peers each) — lifting past 5 is where the two lockstep constants plus the
> per-peer arrays must all grow together. The N-way authenticated wire (the group
> key gating field coupling at N > 2) landed alongside as `ci-smoke-keyauth-n`
> (ADR-0019 update).

## Evidence
- Shipped in: PR #116 — static IP + ARP responder (raw-L2 prerequisite, epic #97)
- Shipped in: PR #117 — two kernels couple oscillator fields over UDP (R_x 0.03→0.99)
- Shipped in: PR #140 — N-way mean-field society (epic #139)
- Shipped in: PR #141 — N-way society exposed to agents (qos_society_boot_n)
- Shipped in: PR #178 — society of societies (FSYP cross-VM aggregate exchange)
- Key code: user/ghostd.c:97-123 (per-peer mean field), :192-210 (min-pairwise
  verdict); user/fieldsyncd.c:28-49 (FSYN/FSYP frames), :100-107 (source-IP filter),
  :260-283 (print-never-imprint); kernel/include/kernel/net.h:34-38 (MAX_PEERS)
- CI gates: `ci-smoke-society` (2-VM), `ci-smoke-society3` (3 distinct qseeds →
  min-pairwise ≥ 0.80 from < 0.50; fails loud if host multicast is unavailable),
  `ci-smoke-society-agents` (society of societies)
