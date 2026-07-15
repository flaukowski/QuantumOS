# 23. IPC Peer Re-Wiring on Watchdog Rebirth (and Dead-Target IPC Unlink)

Date: 2026-07-13
Status: Accepted (2026-07-14; Part 1 + Part 2 shipped together — generalized unlink,
declarative ipc_peers re-mint, swarm-svc key re-forward, anchored ci-smoke gate)

## Context

Declarative `grant_*` resource caps are re-minted by the service framework on every
start (ADR-0002; kernel/src/service.c `start_slot`), so a watchdog-reborn service
regains its console/quantum/COM2/field authority. But **pid-named IPC peer caps are
not**: they are hand-minted once at boot in kernel/src/citizens.c (fieldsyncd↔ghostd
at citizens.c:422-423, swarm-svc↔ghostd at 478-479, swarm-svc→fieldsyncd at 490,
qsh↔ghostd at 571-572, paradoxd↔ghostd at 343-344) and name a specific pid. Two
documented failure modes follow, both confirmed in the tree:

1. **The reborn service loses its wiring.** A watchdog rebirth of `fieldsyncd`
   permanently loses its ghostd IPC cap AND its ADR-0019 session-key delivery path
   (user/fieldsyncd.c:440-447 prints `FIELDSYNC: ghostd wiring lost (known restart
   limitation)`); a reborn `qsh` loses its `ghost` builtin (citizens.c:517-519
   documents it); a reborn `swarm_svc` cannot route DATA requests to the field.
   ADR-0019's increment-B scope note explicitly deferred "the generic IPC re-mint
   (fixing both the ghostd cap and the key cap on rebirth) to its own increment" —
   this is that increment.

2. **Stale caps outlive their target — a recycled-pid authority leak.** When a
   peer dies, the SURVIVING side's IPC cap still names the dead pid. Pids are
   recycled first-fit, so that cap silently re-attaches to *whatever process lands
   on the recycled pid* — the sender can then `SYS_SEND_TO` an unintended process,
   and `SYS_CAP_DERIVE`'s targeted-peer check (syscall.c:1635) would authorize
   delegating a capability into it. Epic #175 already closed exactly this hole for
   spawn-channel caps (`cap_revoke_spawn_channels`, capability.c:426-455) but
   deliberately scoped the unlink to the origin tag so "hand-minted pairs survive a
   peer's restart" (capability.c:428-431) — a survival that was only ever useful
   because a reborn service *sometimes* reclaims its old pid by first-fit accident.
   Accidental pid reuse is not an architecture.

The two defects are one decision: stale caps must die with their target, and the
wiring must come back *deterministically* — by declaration, not by pid-reuse luck.

## Decision

**Part 1 — IPC caps die with their target, tagged or not.** Generalize the epic
#175 unlink: on process death, revoke EVERY `CAP_RESOURCE_IPC` capability whose
`resource_id` names the dead pid — the spawn-channel origin-tag filter is dropped;
the type filter (a FIELD/PROC cap whose resource_id collides with a small pid must
survive) and the dead-target filter stay. Recorded as `AUDIT_UNLINK` under the
surviving owner, exactly like the tagged path (never REAP — the owner did not die).
The capability self-test's "untagged hand-minted pair survives" survivor assert
inverts to assert the new policy; the live-target and colliding-resource-id
survivor asserts keep guarding against an over-broad revoker.

**Part 2 — declarative IPC peer wiring, re-minted on every start.** Extend
`service_definition_t` with an `ipc_peers[]` table: peer *service name* + TWO
INDEPENDENT permission words, `to_peer` and `from_peer` — never a single
symmetric perms field. Mint my→peer only when `to_peer != 0` and peer→me only
when `from_peer != 0`: the ADR-0019 key cap (swarm-svc→fieldsyncd, from-only,
`to_peer == 0`) cannot be represented otherwise, and a stray reverse cap would
give fieldsyncd a SECOND outbound IPC cap, breaking its untargeted first-match
send to ghostd non-deterministically (the exact hazard citizens.c:483-485
documents). This is the mechanism ADR-0019 scoped:

- `start_slot` mints the declared pairs inside the SAME cli window that mints the
  `grant_*` caps and binds the manifest. Each PAIR is transactional: if either
  direction's `cap_create` fails (e.g. NO_SPACE), revoke the direction already
  minted and boot_log the service+peer — a pair is whole or absent, never
  one-way (an asymmetric qsh↔ghostd pair would send fine and time out on the
  reply, a flake worse than a hard fail). The cli window makes the commit atomic
  in time; the transactional mint is what makes it atomic in outcome.
- **Pass 1:** for each of MY declared peers that is currently RUNNING, mint
  my→peer and peer→me as declared (this covers first boot AND my own rebirth).
- **Pass 2:** for each RUNNING service whose declaration names ME, mint its
  declared pair against my fresh pid (this covers MY rebirth when the OTHER side
  owns the declaration, and boot-order gaps — e.g. fieldsyncd declares the
  swarm-svc→fieldsyncd key-delivery cap before swarm-svc exists; the pair mints
  when swarm-svc starts).
- **Both passes guard against a stale RUNNING slot.** `state == RUNNING` is a
  stale value: a peer can be TERMINATED, reaped, and its pid recycled first-fit
  up to ~2s before the health monitor marks the slot CRASHED (service.c:505-517)
  — minting against `other->info.pid` then hands an IPC cap to whatever process
  recycled the pid, reopening the exact leak Part 1 closes (cap_create validates
  neither owner nor resource liveness, capability.c:132). Before minting, require
  `process_is_valid(other->info.pid) && process_get_generation(other->info.pid)
  == other->info.pid_generation` — the identical guard slot_by_pid
  (service.c:97-99) and service_stop (service.c:519-520) already use. On failure
  skip the peer; its own Pass 1 re-wires it when it restarts.
- **Pass 1 MUST run entirely before Pass 2, with no cap frees in between.**
  swarm-svc holds two outbound IPC caps (ghostd RW + the fieldsyncd key cap) and
  its one-shot ghostd discovery relies on first-match, which is unambiguous only
  because its declared (Pass-1) ghostd cap lands at the lower slot
  (swarm_svc.c:249-256). The ordering is load-bearing; state it in the impl.
- **A pair is declared on exactly ONE side** (the later-starting service, matching
  today's citizens.c mint sites) — the two passes make single-sided declaration
  sufficient in both rebirth directions, and it structurally prevents double-mint,
  which would break qsh's singleton-IPC-cap first-match invariant (#176).
- The converted pairs: paradoxd{ghostd RW/RW}, fieldsyncd{ghostd RW/RW,
  swarm-svc from-peer W (the ADR-0019 key cap, `to_peer = 0`)}, swarm-svc{ghostd
  RW/RW}, qsh{ghostd RW/RW}. The hand mints in citizens.c are deleted, not
  duplicated.
- **Key re-forward on fieldsyncd rebirth.** Restoring the delivery PATH alone
  makes rebirth WORSE on a keyed swarm, not better: the session key lives only
  in fieldsyncd's BSS (fieldsyncd.c:108), swarm-svc's SWARM_OP_KEY forward fires
  only on host admission (swarm_svc.c:441-490), and with ghostd wiring restored
  the reborn fieldsyncd takes the snapshot success branch and emits seq=0
  zero-tag frames that keyed peers silently REJECT — a keyless-but-wired
  partition with no diagnostic. So: swarm-svc, which caches the admitted key
  (swarm_svc.c:45-46), re-forwards it when it observes fieldsyncd's pid change
  (it already resolves fs_pid via SYSINFO_PS at swarm_svc.c:457-475 — re-check
  on forward-failure/periodically and re-send if the pid differs from the last
  forward). The keyless-state diagnostic is KEPT (the wiring-lost print is
  deleted only because the wiring now genuinely returns).
- **Deliberately NOT converted:** the one-shot test citizens (ghost-test,
  paradox-test, delegation-test, the echo demo) — they are not monitored, run
  their proof once, and their capless/stale behavior IS the proof-by-attack in
  several gates; and the agentdemo agentd↔fieldsyncd pair (demo-only boots).
  Honestly stated: for agentd↔fieldsyncd Part 1 is a strict REGRESSION, not
  "same as today" — the tagged unlink currently preserves that untagged cap, so
  it sometimes re-attaches by pid-reuse luck; under Part 1 it is unlinked and
  never re-minted, so agentd is permanently capless after any fieldsyncd
  restart (citizens.c:857-858, sys_send_to → EPERM at syscall.c:278). Accepted:
  demo-only, no CI gate depends on it surviving a restart.

Part 1 and Part 2 must ship together: with Part 1 alone, a rebirth leaves the
survivor capless forever (strictly worse than the pid-reuse accident); with Part 2
alone, untargeted `send_msg` first-match can still route to the STALE cap ahead of
the freshly minted one and fail EIO — the unlink is what makes the re-mint the
first match.

## Consequences

### Positive
- A watchdog rebirth of any wired citizen — including qsh (`exit` is a feature)
  and fieldsyncd (whose rebirth is a key-re-admission *because of the explicit
  swarm-svc re-forward above, not automatically*) — restores full IPC wiring,
  deterministically.
- Closes the recycled-pid authority leak for ALL IPC caps, completing the epic
  #175 invariant: no capability ever outlives the process it names.
- Deletes the "known restart limitation" caveats from citizens.c, fieldsyncd.c,
  and ADR-0019's residual list.

### Negative
- `start_slot` gains a MAX_SERVICES × ipc_peers scan inside its cli window —
  bounded (32 × 2), but real added interrupt-latency on every service start.
- The unlink generalization changes behavior a self-test explicitly asserted
  (hand-minted survival); any out-of-tree code that relied on pid-reuse
  re-attachment breaks — that reliance was the bug.
- Declarations add a second place (after the def) where wiring is stated; the
  rule "declare on exactly one side" is enforced by convention + the #176
  singleton gate, not by the compiler.

### Residual risks
- IPC caps held by NON-service processes (agent-society sub-agents) targeting a
  reborn service are unlinked but not re-minted — unchanged from today's
  behavior, and spawn-channel pairs already handle the parent↔child case.
- `fieldsyncd`'s in-memory session key is still lost on rebirth; the swarm-svc
  cached-key re-forward (Decision, Part 2) is what re-delivers it. If swarm-svc
  itself also died and lost its cache, re-admission falls back to the host —
  that window is accepted.
- The cli window freezes services[] against concurrent mutation, but NOT
  against staleness — hence the mandatory liveness+generation guard in both
  passes (Decision, Part 2). An implementation that drops that guard reopens
  the recycled-pid leak this ADR exists to close.
- ci-smoke's qsh-rebirth gate exercises Part 2 fully but NOT Part 1's
  integration necessity (qsh's death REAPs its own outbound cap regardless of
  Part 1; the stale-first-match hazard only manifests when the TARGET restarts
  under a living sender). Part 1's mechanism is gated by the inverted
  capability self-test; the ghostd-restart-under-living-qsh leg (SHIPPED as
  the follow-up increment: `GHOST_EXIT` + `ghost exit` + the
  post-`GHOSTD: FIELD REBORN` slice gate) gates the integration — the living
  shell's answer after ghostd's rebirth requires the unlink AND the re-mint.

## Evidence

- Verified gaps: citizens.c:422-423/478-479/490/571-572/343-344 (hand mints),
  citizens.c:485-487 + 517-519 (documented non-re-mint), user/fieldsyncd.c:440-447
  (wiring-lost print), capability.c:426-455 (tag-scoped unlink + its
  "hand-minted pairs survive" comment), process.c:468-476 (unlink call site whose
  invariant comment already names pid-reuse as the enemy), syscall.c:1635
  (targeted derive check a stale cap would satisfy).
- Available machinery: ADR-0002's re-mint discipline in start_slot (the cli-window
  commit), the AUDIT_UNLINK record kind, slot_by_name.
- Un-fakeable gate (design): the existing ci-smoke scripted session already pipes
  `exit` and gates `QSH: reborn`; extend it to send `ghost` AFTER the rebirth and
  assert a `qsh: ghost R=` answer in the POST-reborn slice. Three traps the gate
  must dodge, each confirmed on the current Makefile:
  1. **Anchor the grep.** The session already emits `qsh: ghost R=` TWICE before
     `exit` (Makefile:447 runs `ghost` at positions 6 and 16; qsh.c:1271 prints
     the literal). A whole-log `grep -q` — the idiom every other gate uses —
     passes GREEN with the feature entirely broken. Slice first:
     `awk '/QSH: reborn/{f=1} f' /tmp/qemu-boot.log | grep -q 'qsh: ghost R='`.
     Single UART + single CPU makes serial ordering strict, so the anchored
     slice cannot capture a pre-rebirth answer.
  2. **Deliver `ghost` after the rebirth, not in the same burst.** qsh batches
     input via `cons_read(chunk, 32)` (qsh.c:1489-1490) and `exit` terminates
     mid-batch — a `ghost\n` appended to the single printf burst is latched into
     the dying shell's chunk and dies with it; the reborn shell reads EOF and
     the gate goes red on a CORRECT feature. Split the writes:
     `( printf '...exit\n'; sleep 4; printf 'ghost\n'; sleep 12 )` and raise the
     qemu timeout (14s today) to cover rebirth + a ghostd round-trip. Verify
     locally that the answer lands in the post-reborn slice before shipping.
  3. **Evaluate the revert-confirm on the SAME anchored slice.** Dropping qsh's
     ipc_peers declaration removes ghost wiring on EVERY incarnation (the hand
     mints are deleted), so ALL `R=` answers vanish, not only the post-rebirth
     one — the anchored post-reborn slice is the discriminator; the pre-`exit`
     answers going dark under revert is expected and carries no signal.
  The gate is non-vacuous once anchored: `qsh: ghost R=` is printed ONLY on
  cmd_ghost's success path (EPERM and timeout print distinct strings,
  qsh.c:1248/1266), and producing it post-rebirth requires BOTH directions of
  the re-minted pair (send + SYS_SEND_TO reply). What it proves is Part 2; the
  capability self-test's inverted survivor assert gates Part 1 the same boot —
  specifically the inverted `cap_check(hand) == CAP_ERROR_INVALID_ID` (the
  AUDIT_UNLINK ring check is satisfied by the tagged path and gates nothing
  new), placed ahead of the `CAPUNLINK:` boot_log so a failure keeps
  Makefile:560's existing grep red. Drop the now-dead `cap_revoke(hand, 900)`
  cleanup when inverting — the cap is already freed.
- Cross-references: ADR-0002 (declarative grants), ADR-0019 (the deferred
  re-mint + the key cap this restores), ADR-0009 (UNLINK audit semantics),
  ADR-0016 (gate discipline), #175/#176 (spawn-channel unlink + singleton
  invariant).
- Adversarial design review (2026-07-14, 4 lenses on the real tree): 3 blockers
  (stale-RUNNING generation guard, unanchored gate grep, same-burst `ghost`
  delivery) and 5 majors (asymmetric perms, keyless-but-wired fieldsyncd,
  revert-confirm scope, Part-1 gate separation) folded into this revision;
  26 load-bearing choices confirmed on code — notably the type/dead-target
  filters, the two-revoker division (REAP owner-death vs UNLINK survivors),
  single-sided declaration preventing double-mint in all rebirth orders, and
  the "must ship together" first-match rationale.
