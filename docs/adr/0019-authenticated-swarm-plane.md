# 19. Authenticate the Swarm Plane

Date: 2026-07-11
Status: Accepted (core FSYN authentication + the full COM2 reply-auth extension shipped — STATUS #199,
QSUBMIT #200, attestation-by-default #201, consolidation #202; FSYP and insider hardening still deferred)

> **Shipped 2026-07-11.** The core is live: the FSYN field-coupling frame now carries an
> HMAC-SHA256 tag over `magic||seq||phase` under a host-admitted group session key, with a
> ticks-seeded monotonic transmit sequence for replay protection. The model is
> **enforce-when-keyed**: a keyed node stamps and verifies the tag and rejects anything without a
> valid one; an unkeyed node behaves exactly as before (the wire grew 260→296 B but coupling is
> unchanged until the host admits a key), so the low-level `ci-smoke-fieldsync`/`society3` gates are
> unaffected. Proven by `ci-smoke-keyauth`: two members with the SAME key couple under the MAC;
> two with DIFFERENT keys each reject the peer's frames and do NOT synchronize (revert-confirmed —
> neutering the MAC check makes the different-key pair couple). Still deferred (see below): FSYP
> society-aggregate auth, COM2 DATA-reply auth, insider hardening (pairwise keys + sender-IP
> binding), a sliding replay window, and the IPC/net deny-audit ride-alongs.

> **Update 2026-07-15 — N-way authenticated swarm plane (epic #139 + increment 0 observability).**
> The FSYN key auth now extends to N>2 societies, gated by `ci-smoke-keyauth-n` (three kernels on a
> shared mcast L2). An adversarial panel established the load-bearing correction below.
>
> - **One-way-lock semantics (why an R_x / SYNCHRONIZED assertion cannot detect a keyless node).**
>   The model is enforce-*when-keyed*, so an UNKEYED receiver skips the MAC guard and accepts every
>   frame, INCLUDING a keyed peer's authenticated frames. A keyless node C in a 2-keyed+1-keyless
>   mesh therefore becomes a **one-way Kuramoto follower**: it phase-locks onto A/B's frames and
>   prints `R_x >= 0.80` + `SYNCHRONIZED`, while A/B reject C's zero-tag frames and (after
>   `PEER_STALE_TICKS`) drop C's frozen slot so their own min-pairwise R_x recovers. All three
>   print SYNCHRONIZED with the key withheld — **R_x discriminates nothing about admission.** The
>   real discriminator is frame-ADMISSION, offset-anchored at each member's `FSKEY` log position:
>   the `FSAUTH … (bad MAC)` line on the keyed members, and per-source `FIELDSYNC: frame from <ip>`
>   growth that STOPS for the excluded source and RESUMES once it is keyed. This one-way lock is
>   expected, documented behaviour — the gate asserts it positively (C keeps receiving from A/B),
>   never treats it as a failure.
> - **Increment 0 observability split.** `note_reject` now latches ONCE PER REASON (two independent
>   flags: bad-MAC, replay) rather than once per boot, so a distinct replayed frame surfaces its own
>   `… (replay)` line instead of being swallowed behind an earlier bad-MAC line; `note_key` now also
>   emits `FSKEY: rekeyed (replay watermarks reset)` on a re-install (previously console-silent).
>   These are console lines only — **`SWARM_STATUS_BODY_LEN` stays 6 (frozen ABI, contracts/wire/
>   v1.golden is untouched).** The host status helpers gained an optional `since=` log offset so a
>   multi-phase gate reads only evidence produced after a phase boundary (the whole-log grep is
>   sticky and would otherwise report a stale pre-key SYNCHRONIZED forever).
> - **FSYP remains UNAUTHENTICATED — explicit scope.** Only the FSYN phase frame is MAC-gated. The
>   FSYP society-aggregate frame (epic #178) carries no tag: it is source-IP filtered (`in_peer_set`)
>   and **print-never-imprint** (received aggregates are only printed for host-side value-recompute,
>   never folded into field content), so a forged aggregate can at most redden a host gate, never
>   poison a field. Authenticating FSYP (reuse `reserved0` as seq + a 32 B tag → 48 B) stays
>   deferred; the integrity check for FSYP is the host value-recompute, stated here so it is not
>   mistaken for an oversight.
>
> Revert-confirmed: skipping the leg-(b) admit reddens the frame-from-C resume + post-key sync;
> collapsing the per-reason reject flag back to one shared latch reddens the leg-(c) replay marker.

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

**Extension shipped 2026-07-11 — COM2 reply authentication (STATUS).** The same HMAC + session key
now attest the COM2 DATA *replies*, not just the FSYN wire: `status_authenticated()` sends a fresh
16-byte host nonce, and swarm_svc (which now also STORES the admitted key, not just forwards it)
echoes the nonce and appends `HMAC(key, op||nonce||body)` to the STATUS reply. The host verifies
the echo (freshness — a replayed genuine reply carries an old nonce whose tag is still valid, so
only the echo betrays it) then the tag (unforgeability), failing CLOSED (a keyed caller rejects a
stripped-tag reply). Fail-OPEN when no key / no nonce, so the unkeyed `ci-smoke-mcp` path is
untouched. Gated by `ci-smoke-replyauth` (positive + replay-reject + forgery-reject + fail-open,
revert-confirmed). This turns "verified ≠ live" (ADR-0015) into an attested-fresh tool result for
STATUS. DEFERRED: RECALL (same synchronous shape, trivial follow-up) and QSUBMIT (needs the nonce
threaded through the async `qsub_jid` deferred-reply state).

**Extension shipped 2026-07-11 — COM2 reply authentication (QSUBMIT).** The async, LIVE path:
the host submits opaque circuits to the in-OS QPU broker over COM2 and consumes the result, so an
unauthenticated reply was a forgeable *job result*. Unlike STATUS (fixed-length body → an optional
nonce), the QSUBMIT circuit is VARIABLE-length, so the nonce cannot be optionally detected — the
design is **enforce-when-keyed**: a keyed guest always expects `op|nonce(16)|circuit` and a keyed
host's `qsubmit()` (the SOLE COM2 QSUBMIT emitter — `qpu_run`/the MCP tool delegate to it) always
prepends the nonce; unkeyed keeps the legacy `op|circuit` wire byte-for-byte. Because the reply is
deferred to `qsub_poll_step`, the request's nonce is STASHED atomically with `qsub_jid` at accept
and echoed when the job completes; the emit decision keys off the accept-time `qsub_authed` snapshot
(not live `have_key`, which a mid-flight key admit could flip), and the stash is scrubbed on
completion. All five reply sites are authenticated — the three synchronous errors (malformed / busy
/ broker-EPERM) use the in-hand nonce, the async DONE and poll-error use the stashed one — via one
`emit_qsubmit_reply(body, len, nonce)` helper whose MAC preimage `op||nonce||body[1..]` is identical
in shape to STATUS. The host `_verify_qsubmit_reply` fails CLOSED on a `<50`-byte (stripped) reply
and on `body_len ∉ {2,22}`, checks the echo (freshness) then the tag (unforgeability). A too-short
keyed request (`len<17`, no extractable nonce) is dropped silently — no OOB read, no plaintext
downgrade oracle; the host fail-closes on the timeout. COM2 RX is polled in swarm_svc's single
main loop (no ISR), so the stash needs no interrupt guard — a future move to interrupt-driven RX
must re-trigger this review. Gated by `ci-smoke-qsubmit-replyauth` (positive DONE + synchronous-error
+ replay-reject + forgery-reject + fail-open, revert-confirmed: forcing the guest emit plain reddens
the DONE leg on the host's `<50` floor). An adversarial design panel caught four ship-blockers before
code (host MAC omitting the nonce → dead-on-arrival + echo-rewrite replay; the `len<17` underflow;
the missing host length floor; a would-be-vacuous gate). This attests the live circuit-submit path,
completing reply-auth for both COM2 ops with a real host consumer.

**RECALL reply-auth NOT built — no consumer (2026-07-11).** `SWARM_OP_RECALL` is defined and the
guest handles it, but NO caller emits it: the host recalls via qsh (`recall …`), not COM2, and no
peer/society issues it (field coupling rides FSYN, not COM2 RECALL). Authenticating a reply nothing
requests would be speculative crypto for a dead path — deferred until a COM2 RECALL consumer exists,
at which point it is the trivial synchronous STATUS mirror. This corrects the earlier "trivial
follow-up" note above: the triviality was never in doubt; the absence of a consumer is the reason.

**Extension shipped 2026-07-11 — attestation by DEFAULT (the agent surface).** The two reply-auth
increments above built the mechanism but left it DORMANT: the agent-facing MCP session admitted no
key, so `qos_status`/`qos_qpu_run` rode the plain path and no agent ever saw the freshness guarantee.
This turns it ON. `QosVM.attest()` admits a fresh HOST-generated 32-byte session key (host generates
AND holds it — control of the serial channel == control of the VM, so a session-scoped key binds every
reply to the VM this host booted) then confirms reply-auth is LIVE by retrying `status_authenticated`
until the guest has consumed the key frame (it must span a short guest settle — retries on both
`QosRefused` (plain reply, not yet keyed) and `QosTimeout` (not yet replying), fail-CLOSED if it
can't go live). `qos_boot` now attests by default (degraded-not-fatal), `qos_status` is adaptive
(authenticated when keyed, uniform `{r, live, attested, identity}`), and `qos_qpu_run` surfaces
`attested` (its `qpu_run`→`qsubmit` path authenticates once keyed). Purely ADDITIVE — STATUS keeps
its optional nonce so a plain `status()` still works, and only an `attested` field is added; no
breaking change. This makes "faithfulness = observable" concrete: an agent now sees, per result,
whether it is cryptographically fresh, not merely that the boot attested (ADR-0015). Gated by
`ci-smoke-attested` (baseline-dormant → attest → attested STATUS + attested `qpu_run`,
revert-confirmed: a no-op attest reddens the status/qpu legs).

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
  receiver accepts `n >= sizeof(fsyn_frame_t)` (NOT exact `== 296`). *Doc/code drift resolved
  2026-07-15: the shipped code always used `>=` (user/fieldsyncd.c) and `>=` is kept.* Rationale:
  the MAC covers exactly `FSYN_MAC_COVERED` (264) leading bytes and the tag is read at its fixed
  compile-time offset, so trailing bytes beyond the struct can neither move the tag nor alter the
  MAC preimage — accepting a padded transport datagram tolerates a benign encapsulation without
  weakening authentication. Runts (`n < sizeof`) still fall through and drop.
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
