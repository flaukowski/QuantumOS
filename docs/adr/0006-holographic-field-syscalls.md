# 6. Holographic Field Syscalls — Associative Memory as a Kernel Primitive

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

QuantumOS carries two "wave memory" lineages that predate the kernel field. kannakad's userland
Q15 ranked recall (centered wavefronts, cosine × energy) and ghostd's Hopfield–Kuramoto attractor
field (256 phase oscillators relaxed over 48 steps per recall). Epic #95 asked which of these
deserves to be a kernel primitive. The answer was deliberate: only the RANKED holographic kind is
syscall-shaped — one bounded pass of ~512 integer MACs, no relaxation loop — so it moved into the
kernel, while ghostd's ATTRACTOR memory "is a different, living memory kind and deliberately stays
in ring 3" (kernel/include/kernel/field.h:9-15). The result is a two-layer memory design: a
capability-gated kernel associative store any citizen can be granted, plus an autonomous ring-3
oscillator field that holds no kernel-field capability at all (kernel/src/citizens.c:237-249 —
ghostd's only declared grant is the quantum pool).

Kernel constraints shaped everything: the kernel is `-mno-sse` with no fxsave, so the math is
integer-only Q15 (field.h:17-18); syscalls run cli'd on a single CPU, so the field is touched only
from syscall context with lazy tick-stamp decay instead of periodic bookkeeping — deliberately no
locking (field.h:20-25).

## Decision

**Geometry.** 8 regions × 8 slots × 64-byte patterns, top-k up to 8 (field.h:40-44), stored as
~7 KB of static `.bss` — no kernel-heap dependency, no init-order issue (kernel/src/field.c:43-44).
Region assignments are fixed by convention (field.h:35-39): 0 = qsh (the operator's
`imprint`/`recall` builtins, the only `field_inherit` region, citizens.c:530-538), 1 = kannakad
(citizens.c:154-155), 2 = delegation-test with a delegable CAP_GRANT cap (citizens.c:630-633),
3 = agentd's shared knowledge and 4–6 the division-of-labor specialists' private workspaces
(agentd holds span 4 over regions 3–6, citizens.c:813-816), 7 spare.

**Scoring.** `wavefront()` derives a Q15 unit vector from a pattern by subtracting the mean in
×256 fixed point (raw ASCII is all-positive and would crowd every cosine toward 1; centering
restores sign structure), normalizing by a bit-by-bit `isqrt_u64`, and refusing degenerate
all-equal-byte patterns whose magnitude is zero (field.c:72-103). `cosine_q15()` dot-products two
unit wavefronts over their common prefix — equivalent to zero-padding the shorter (field.c:105-122).
Recall scores every live slot as `(cosine × effective_energy) >> 15` in one pass, then
selection-sorts the top k (field.c:225-257). A degenerate probe returns n=0 as success — the
integer divide can never see mag == 0 (field.c:215-218); a degenerate imprint is −1 → EINVAL
(field.c:144-147, syscall.c:1396-1398).

**Energy landscape.** Importance defaults to 0x4000 and clamps to [0x0800, 0x7FFF] — the floor
means "no zero-energy squatting" (field.h:46-48, field.c:149-157). Effective energy decays lazily,
1 Q15 unit per 256 ticks at 100 Hz, computed on demand in u64 with clamped subtraction that never
inverts (field.h:50-54, field.c:126-134). A full region evicts the lowest *effective*-energy slot
(field.c:160-179).

**Syscall surface.** SYS_IMPRINT = 29, SYS_RECALL = 30, SYS_FIELD_INFO = 31
(kernel/include/kernel/syscall.h:192-214; dispatch syscall.c:1891-1898). Every entry is
capless-first: a caller holding no CAP_RESOURCE_FIELD cap at all is denied with exactly EPERM and
an audit_deny record *before any user memory is read* (syscall.c:1369-1373, 1403-1407, 1455-1458).
After whole-struct copy-in, `authorize()` must match the capability against EXACTLY `req.region` —
"this comparison IS the isolation boundary" — through two layers since epic #135: capability, then
intent manifest (syscall.c:1383-1390, 113-128). Recall's retrieval reinforcement (retrievals++,
energy += gap/16, decay stamp refreshed) is a write to the energy landscape and is applied only
when the caller *also* holds CAP_WRITE on that region (syscall.c:1427-1428; field.c:265-273).
SYS_FIELD_INFO is the honest non-mutating counterpart: `const` throughout, and it zeroes the whole
332-byte out struct first so the shared static copy-out buffer never leaks a prior call's
other-region preview bytes across the isolation boundary (field.c:280-295). ABI structs are
`_Static_assert`ed on both sides — kernel (field.c:23-27) and the user twins in user/usys.h:334-391
(drift asserts usys.h:386-387).

**Grant lifecycle.** The service layer scrubs every region in a grant's span BEFORE minting its
cap — a reborn or successor service must never inherit its predecessor's memories — with one
exception: a `field_inherit` service may claim disk-restored content exactly once per boot
(kernel/src/service.c:329-342, 358-364; restore mechanics in ADR-0007). Out-of-range spans fail
the WHOLE grant closed, because `cap_create` never validates FIELD ids and `field_region_scrub`
silently no-ops out of range (service.c:320-327). Delegable grants add CAP_GRANT so exactly one
citizen per region may SYS_CAP_DERIVE narrowed slices to sub-agents (service.c:348-351) — the
mechanism the agent societies of ADR-0014 are built on.

## Consequences

### Positive

- Recall is syscall-shaped and bounded: no loops whose iteration count depends on convergence, no
  allocation, no floats. The worst case is fixed at 8 slots × 64 MACs plus a selection sort of 8.
- Isolation is enforced at three layers (capless-first, exact-region capability, intent manifest)
  and every denial is an audit record — the deny path is as observable as the grant path.
- The read/write split is structural, not conventional: reinforcement lives behind CAP_WRITE, and
  the info path cannot mutate because the region pointer is `const` (field.c:292-295).

### Negative

- The store is tiny by design: 64-byte patterns, 8 slots per region, no per-slot delete — the only
  erase is whole-region scrub at re-grant time (field.c:324-340). A busy region silently forgets
  its weakest memory on the 9th imprint.
- Cosine over the common prefix means pattern length is not part of similarity: a short probe can
  resonate strongly with a much longer pattern whose prefix wavefront matches.
- Reinforcement also refreshes `imprint_tick` (field.c:271), so a frequently-recalled slot climbs
  toward ENERGY_MAX and becomes effectively unevictable — a write-capable caller can pin slots and
  starve its own region.
- The region-assignment comment at field.h:35 still reads "0=ghostd/qsh"; as-built, ghostd holds
  no kernel-field capability and region 0 belongs to qsh alone (citizens.c:530-538). The comment is
  a fossil of the original plan.
- `age_ticks` in the info struct is u32 and saturates at ~497 days of uptime — an acknowledged
  display-only limit (field.h:96-98).

### Residual risks

- The no-locking invariant holds only while the field is touched exclusively from cli'd syscall
  context (field.h:20-25). Nothing enforces this mechanically; a future timer/IRQ-context caller
  would introduce silent corruption.
- The static kernel-side out buffers in sys_recall/sys_field_info (syscall.c:1433, 1470) are
  justified by the same single-CPU cli'd argument; SMP would turn them into a data race.
- The exact-region check happens after copy-in by necessity (the region id lives inside the
  request), so a capless-but-any-region-holding caller can probe EFAULT/EINVAL behavior for
  regions it cannot touch; content isolation is unaffected.

## Evidence

- Shipped in: PR #112 (SYS_IMPRINT/SYS_RECALL, epic #95); PR #113 (kannakad rehosted onto the
  kernel field); PR #114 (field persistence, epic #96 — see ADR-0007); PR #130 (SYS_FIELD_INFO +
  imprint --energy, epic #127 B1); PR #136 (intent-manifest second authority layer, epic #135);
  PR #138 (SYS_CAP_DERIVE narrowed delegation, epic #137); PR #177 (field_region_span multi-region
  grants for the division-of-labor society — see ADR-0014).
- Key code: kernel/src/field.c:72-134 (wavefront/cosine/decay), field.c:160-179 (eviction),
  field.c:265-273 (reinforcement); kernel/src/syscall.c:1364-1482 (three syscalls),
  syscall.c:113-128 (two-layer authorize); kernel/include/kernel/field.h:35-54 (regions, energy,
  decay); kernel/src/service.c:314-368 (span grants, scrub-before-mint, fail-closed);
  user/usys.h:334-391 (ABI twins); kernel/src/citizens.c:530-538, 154-155, 630-633, 813-816
  (region owners).
- CI gates: `ci-smoke` field block (Makefile:836-872) — "FIELD: imprinted slot 2", the exact
  noisy-probe winner text, "FIELD: cross-region denied (EPERM)", "FIELD: empty-probe ok (n=0)",
  and capless imprint/recall EPERM; kannakad's write-side contract gate
  "kannakad: RESONANCE VERIFIED" (Makefile:797-807); the society demo consumes regions 3–6 end to
  end via "AGENTD: DEMO OK qpu+field+spawn+society" (Makefile:1109-1110).
