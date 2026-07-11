# 19. Authenticate the Swarm Plane

Date: 2026-07-11
Status: Proposed

## Context

Guest authority is structural (ADR-0001/0008) and boot identity is Lamport-attested
(ADR-0015), but the field-coupling wire between kernels is authenticated by **source
IP alone**. `fieldsyncd` accepts an `FSYN` phase frame or `FSYP` society aggregate if
its source address is in the configured peer set (`in_peer_set`,
user/fieldsyncd.c:100-107) — trivially spoofable on a shared L2. The MCP server
confesses the gap in every society status payload: *coupling and attestation are
cryptographically UNLINKED* (scripts/qos_bridge.py:1402-1403). A forged frame
carrying a configured peer's IP overwrites that peer's phase slot
(user/ghostd.c:736-743), forcing a false `SYNCHRONIZED` or denying convergence, with
**zero replay protection** (no sequence number or nonce). This is the same class as
the real Kannaka swarm-injection incident. It is the next phase because it is the
one place the mission's structural-safety claim currently has a hole.

## Decision (proposed)

Authenticate every society wire frame. The design constraints below are not
negotiable — the adversarial panel killed the naive version on each:

- **Keys must NOT derive from the qseed.** Each node knows only its own qseed, the
  society *requires* distinct qseeds by construction, and the qseed is **public** —
  printed in the attestation, echoed on the console, and grepped by CI. Deriving a
  shared MAC key from it is cryptographically vacuous.
- **Use host-admitted session keys over the attested COM2 channel.** The host
  (already the admission root, ADR-0014) distributes a group/pairwise session key to
  each node over COM2 after attestation. This needs a **new swarm-svc ↔ fieldsyncd
  IPC pair** that does not exist today (swarm-svc is the sole COM2 holder,
  citizens.c:437-483; fieldsyncd sends the frames).
- **MAC with the existing integer SHA-256.** `user/sha256.h` is integer-only and
  already shipped — HMAC-SHA256 over each frame plus a **monotonic sequence number**
  for replay protection. No hand-rolled MAC (no SipHash).
- **Reject accounting stays ring-3/host-side.** Ring 3 cannot append to the kernel
  authority ledger without destroying its unforgeability (ADR-0009), so rejected
  frames are gate-greppable console lines backed by a **saturating counter**, never
  per-frame log spam and never a kernel-ledger write.
- **Fix watchdog-rebirth IPC re-mint in-phase.** Boot-minted peer/key IPC caps are
  not re-minted on service restart (user/fieldsyncd.c:333-339) — under this phase
  that becomes a key-distribution outage, so it must be fixed here.
- **Extension (same machinery): authenticate COM2 DATA replies.** The Lamport
  signature covers only the boot string; STATUS/RECALL/QSUBMIT replies are unsigned
  and replayable. A host nonce + per-reply HMAC lets every MCP tool result actually
  *be* attested — higher agent-native value than the FSYN wire alone.
- **Ride-along: audit IPC/net ownership denials** (ADR-0009's named gap).

## Consequences

### Positive
- Closes the structural-safety hole in the swarm plane: a spoofed frame cannot move a
  peer's phase, and a replayed frame is rejected by sequence.
- Extends attestation from "this booted honestly" to "this reply is fresh and from
  the attested node" — the honest completion of ADR-0015's *verified ≠ live* caveat.
- Also unlocks the deferred FSYP-aggregate-as-field-content feature that the ADR-0014
  panel rejected *because* the wire was untrusted.

### Negative
- Adds a key-distribution mechanism and a new IPC pair to a system that deliberately
  had neither — real new surface, justified only by the closed hole.
- Per-frame HMAC adds CPU to the ~1 Hz fieldsync path and boot time to key admission;
  the society gates run under hard timeouts (ADR-0016), so the added work must fit the
  CI wall-clock budget.
- Extends the COM2 frame set and likely the attestation string — which is exactly why
  the v1 contract freeze (ADR-0020) is sequenced *after* this phase.

### Residual risks
- The host is the key root: compromise of the host admission path compromises the
  group key. The threat model already trusts the host (ADR-0015), so this is
  consistent, but it concentrates trust.
- MAX_PEERS 4 still bounds the society; authentication does not lift the scaling wall.

## Implementation plan (2026-07-11, panel-reconciled)

**Increment A shipped** (the crypto primitive, verified in isolation before the wiring):
`hmac_sha256` + a constant-time `hmac_sha256_equal` in `user/sha256.h`, self-tested at boot in
`swarm_svc` against the RFC 4231 test-case-2 vector so the guest MAC agrees byte-for-byte with the
host `hmac.new(key, msg, sha256)`. Gated by the `HMAC256: self-test OK (RFC4231 tc2)` marker in
`ci-smoke` (revert-confirmed: a corrupted vector prints `SELF-TEST FAILED` and the gate reddens).
Increment B (below) wires it onto the frame.

**Increment B scope refinement (2026-07-11):** the key cap is **boot-minted** `swarm_svc→fieldsyncd`
in `citizens.c` (the existing `agentd→fieldsyncd` precedent, `cap_create(swarm_pid,
CAP_RESOURCE_IPC, fs_pid, CAP_WRITE, ...)` — no kernel change), rather than the generic
`service_definition_t.ipc_peer` + `start_slot` re-mint the panel proposed. Rationale: a
`fieldsyncd` watchdog rebirth **already** loses its ghostd IPC cap and stops coupling today (a
documented existing limitation), so a key outage on rebirth is the *same* pre-existing failure
class, not a new regression the auth introduces. The generic IPC re-mint (fixing both the ghostd
cap and the key cap on rebirth) is deferred to its own increment. This keeps B off the load-bearing
`start_slot` cli path.


A 3-lens implementation-design panel resolved the wiring against the real code. Key finding:
`fieldsyncd.c`'s `peer_pk[]` is a **packed IP** array (a review trap — "pk" = packed, not public
key), and there is **zero** pre-existing key infrastructure. It was renamed `peer_ip` and the
FSYN/FSYP frame ABIs were locked with static asserts as a prep step (this increment).

**First increment — authenticated FSYN only:**
- **Frame** grows to **296 B**: `magic(4) | seq(4) | phase[256] | tag[32]`, `_Static_assert(==296)`;
  receiver tightens the accept from `n >= sizeof` to exact `n == 296`.
- **Key store**: `peer_key[GHOST_MAX_PEERS][32]`, `peer_key_ok[]`, `peer_last_seq[]` in lockstep
  with `peer_ip[]`.
- **MAC**: add RFC-2104 **HMAC-SHA256** on the shipped integer `user/sha256.h` (NOT bare
  `sha256(key||msg)`, NOT SipHash); `tag = HMAC(peer_key[i], magic||seq||phase)` over the leading
  264 B; **constant-time** compare on ingress.
- **Seq (blockers)**: a per-sender **monotonic transmit counter** (bumped once per send cycle,
  NOT content-derived — FSYP is an idempotent resend that reuses its value, so a content-tied seq
  would be rejected by the strictly-greater check). Seed `tx_seq = (uint32_t)ticks()` at `_start`
  **and on watchdog rebirth** (SYS_TICKS survives rebirth and climbs 100×/s, so it is always above
  the peer's last-seen seq — otherwise a reborn node's seq-from-0 is rejected forever). Receiver
  requires strictly-greater; a key (re)install resets that peer's watermark.
- **Key distribution**: host-admitted **group key over the attested COM2 channel**. `swarm_svc`
  (sole COM2 holder) gets `SWARM_OP_KEY`; it discovers `fieldsyncd`'s pid via the uncapped
  `SYSINFO_PS` scan (agentd precedent) and pushes the key with a **one-way TARGETED `send_to`**
  (a bidirectional IPC pair would break `fieldsyncd`'s untargeted first-match send to ghostd).
  A new `service_definition_t.ipc_peer` makes `start_slot` **re-mint** that cap on watchdog
  rebirth (declarative `grant_*` caps name fixed ids and are re-minted; pid-named IPC caps are
  not — the ADR-0014 gap this fixes). `fieldsyncd`'s IPC drain buffer widens (16→≥40 B) and
  `note_key` is wired into **both** recv sites (main-loop drain + snapshot reply-wait) since the
  key can race the snapshot reply.
- **Host**: new `QosVM.admit_key` (model on `status()`) + `QosSociety` admits the group key to
  every member **before** awaiting sync (fail-closed: no key → no `FIELDSYNC: frame from`).
- **Reject accounting**: ring-3 **saturating counter** + one gate-greppable console line — never a
  per-frame print, never a ring-3 write to the kernel authority ledger.
- **Untouched**: `ghostd.c` and the kernel net layer (the MAC is stripped at the `fieldsyncd`
  boundary before the `GHOST_COUPLE` IPC).

**Gate**: an attacker VM on the society mcast L2 forges an FSYN and **spoofs a configured peer's
source IP** (mandatory — `in_peer_set` already drops non-configured sources, so an un-spoofed
injector is vacuous); lacking the key its tag fails the constant-time compare, so it never reaches
a ghostd slot / never prints. Positive path still synchronizes. Revert-confirm: stub the MAC check
to always-pass → the same spoofed frame now couples.

**Deferred to later increments**: FSYP society-aggregate auth (print-only today, so a spoof only
prints — reuse `reserved0` as seq + 32 B tag → 48 B); COM2 DATA-reply auth (host nonce + per-reply
HMAC); insider hardening (pairwise keys + sender-IP binding); a sliding replay window; the IPC/net
deny-audit ride-alongs.

## Evidence (baseline the phase builds on)
- Verified threat: user/fieldsyncd.c:100-107 (source-IP-only filter), ghostd.c:736-743
  (peer slot overwrite), scripts/qos_bridge.py:1402-1403 (unlinked confession)
- Available machinery: user/sha256.h (integer HMAC-SHA256), the attested COM2 channel
  (ADR-0015), the host admission root (ADR-0014)
- Un-fakeable gate (design): an attacker VM on the existing society3 multicast L2 that
  **spoofs a configured peer's IP** (mandatory — `in_peer_set` already drops
  non-configured sources, so an un-spoofed injector makes the gate vacuous) must fail
  to couple; the positive path still synchronizes
- Cross-references: ADR-0009 (ledger + deny-coverage gap), ADR-0014 (the wire),
  ADR-0015 (attestation + DATA-reply extension), ADR-0020 (freeze waits on this)
