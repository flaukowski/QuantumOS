# 10. One-Hop Cross-Ring Capability Delegation (SYS_CAP_DERIVE)

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

Static authority in QuantumOS is minted at service start from declarative grant flags
(service.c start_slot), which covers the boot roster but not runtime-created sub-agents:
an orchestrator that spawns a worker has no way to hand it a slice of its own authority.
The agent-native mission (epic #137 Phase D) required exactly that — and delegation is
the classic escalation surface of capability systems: transitive grant chains, handle
forgery across the ring boundary, and confused-deputy injection into unwilling recipients.

Three sibling invariants constrain any design here. Capability ids never cross the ring
boundary (ADR-0001) — ring 3 holds no forgeable handles. Declared intent (the manifest,
ADR-0008) is minted together with authority and checked at every gated syscall, so a
delegated cap that is not also reflected in the recipient's manifest would be held but
unusable (MDENY at authorize(), syscall.c:122-128). And the authority ledger (ADR-0011)
records the pid whose holdings changed, never the acting requester — which limits what
any delegation event can honestly claim about *who* delegated.

## Decision

SYS_CAP_DERIVE (syscall 34, dispatched at syscall.c:1906-1908) lets a citizen holding a
CAP_GRANT capability hand a strictly narrowed slice of it to one IPC peer — and the
recipient can never re-delegate. Delegation is one hop, by construction.

**Parents are named by (resource_type, resource_id), never by handle.** The 24-byte
packed request (`cap_derive_req_k_t`, syscall.c:1553-1560, `_Static_assert`ed against
the user-side twin) carries no cap_id; the kernel resolves the delegator's own grantable
parent via `cap_find_id` (syscall.c:1631-1640). This keeps cap_ids out of ring 3 and
closes the forgery surface (rationale at syscall.c:1547-1549).

**The guard ladder runs in this exact order** (sys_cap_derive, syscall.c:1573-1659):

1. Copy-in validation — `user_ok(req, 24, read)` else EFAULT (1575-1577).
2. `permissions == 0` → EINVAL; a no-rights derive never touches the cap table (1584-1587).
3. **One-hop rule** — `permissions & (CAP_GRANT|CAP_REVOKE)` → EPERM: the handed slice can
   never itself grant or revoke, so a sub-agent cannot re-delegate (1588-1592). This is
   also what bounds `revoke_children_of` recursion (capability.c:239-241).
4. Target sanity — self, kernel pid, or out-of-range pid → EPERM (1594-1599).
5. Target liveness/class — must exist, be PROCESS_TYPE_USER, RUNNING or READY (1600-1604).
6. Not a monitored service — a watchdog restart rebinds the target's manifest from grant
   flags and cascade-revokes the derived cap, silently evaporating the delegation, so it
   is refused up front (1605-1610; service.h:222-226).
7. **IPC-peer requirement** — the caller must hold an IPC send-cap for exactly
   `target_pid`; a miss records audit_deny + EPERM. This is what stops a CAP_GRANT holder
   from injecting a cap into an arbitrary victim (1611-1618).
8. **Transitive intent bound** — `manifest_check(pid, rtype, rid, perms)`: the delegator
   may only delegate a resource it is itself declared to touch; a miss records MDENY (1619-1623).
9. Idempotency — if the target already holds a covering cap, succeed without minting,
   bounding the shared 1024-slot table against a looping delegator (1624-1630).
10. Parent resolution — `cap_find_id(pid, rtype, perms|CAP_GRANT, rid)`: requiring
    CAP_GRANT in the match means a non-grantable same-resource cap is never selected, and
    a hit proves the caller actually holds the resource, not just a dangling manifest row;
    miss → audit_deny + EPERM (1631-1640).
11. Recipient-manifest room — `manifest_has_room(target)` else EIO, checked **before**
    minting so the manifest extension cannot fail after a cap exists (there is no undo
    path); cli'd single-CPU syscalls make the check-then-act atomic vs the IF=1 health
    monitor (1641-1647).
12. Mint — `cap_derive` re-enforces owner/expiry/CAP_GRANT/least-privilege/expiration-clamp
    at the capability layer (capability.c:169-185) and emits the child's GRANT ledger
    record (capability.c:204); child carries `is_inherited=1` and `parent_cap` (198-200).
13. Manifest extension — `manifest_grant(target, rtype, rid, perms)` makes the delegated
    cap usable; provably cannot fail after step 11 (1655-1658).

Errno mapping: `CAP_ERROR_NO_SPACE` → EIO, all other capability-layer failures → EPERM
(syscall.c:1562-1571).

**Delegable parents are deliberately scarce.** Only `grant_field_delegable` mints a field
cap with CAP_READ|CAP_WRITE|CAP_GRANT (service.c:344-352), and exactly two citizens set
it: delegation-test over region 2 (citizens.c:623-634) and agentd over its region 3-6
span (citizens.c:808-816). qsh deliberately does not — one auditable delegator, small
CAP_GRANT blast radius (service.h:140-147).

**Delegated-vs-static provenance is proven by death.** When the delegator dies, the
reaper's `cap_revoke_all_for_process` cascades `revoke_children_of(id, AUDIT_REAP)` over
every derived child before freeing the parent (capability.c:418-438), with capture-before-
free ordering because the reaper runs at IF=1 (capability.c:247-258). A static grant
survives an unrelated process's death; only a derived cap cascades — the un-fakeable
distinguisher the CI gate greps for.

**cap_transfer is implemented but unwired, and the verdict is DELETE, not wire.**
`cap_transfer` (capability.c:211-229) has no caller anywhere in the kernel and is
deliberately unaudited — the comment at capability.c:208-210 mandates REVOKE(from)+
GRANT(to) records before any caller ships. The adversarial panel rejected wiring it:
its own guard requires the moved cap to carry CAP_GRANT (capability.c:219), so a wired
transfer *moves delegator authority* — a structural bypass of the one-hop rule and the
one-auditable-delegator invariant; an honest transfer record needs actor identity, which
collides with the ledger's holdings-not-actor limitation (ADR-0011); and the source's
manifest keeps a stale allow-row (manifest.c has append and whole-clear but no single-row
removal). Deletion is ~22 lines plus the always-zero `stats.transferred` counter, matching
the #180/#181 dead-optionality precedent. Until that PR lands, this ADR records the
primitive as dead code, not as an available mechanism.

## Consequences

### Positive

- Delegation is provably single-hop: no derived cap can parent another, so revocation
  cascades are depth-bounded and the authority graph stays a two-level tree per delegator.
- No handles cross the ring; the syscall surface admits no forged or guessed cap_ids.
- Injection is impossible outside the caller's explicit IPC wiring (guard 7), and the
  delegator can never exceed its own declared intent (guard 8) — the manifest is a
  transitive bound, not just a per-process one.
- The delegated cap arrives usable (manifest row extended atomically with the mint) and
  observable: GRANT at mint, REAP at cascade, both in the durable ledger.
- Idempotency plus the room-before-mint ordering means the kernel never ends up in a
  half-committed state — no cap without a manifest row, no row without a cap.

### Negative

- One hop excludes legitimate multi-level orchestration: agentd's sub-agents cannot
  sub-delegate slices of their workspaces; any deeper society needs the root delegator
  in the loop for every grant.
- There is no voluntary revoke: SYS_CAP_REVOKE is not exposed to ring 3 (blocked on the
  ledger's actor anonymity, audit.h:25-28), so a delegator can only withdraw a delegation
  by dying. Expired delegations are refused but their slots are not freed until owner
  death (no EXPIRE ledger kind).
- The GRANT record names the recipient whose holdings grew, not the delegator who acted
  (ADR-0011) — the ledger shows that authority appeared, not who handed it over.
- MANIFEST_MAX_ENTRIES = 8 is a hard recipient-side wall (EIO at guard 11); agentd already
  runs at 7 of 8 rows (citizens.c:817-823), leaving one row of delegation headroom.
- Monitored services can never be delegation targets, ruling out the entire watchdog-
  supervised roster as recipients — correct (rebirth would evaporate the grant) but limiting.

### Residual risks

- `cap_transfer` still exists in the tree; a future caller wired in ignorance of this ADR
  would bypass one-hop silently. The deletion PR is the mitigation and has not shipped yet.
- The one-hop bound on `revoke_children_of` recursion holds only while no kernel-internal
  code derives from a derived cap; the comment at capability.c:239-241 flags this, but
  nothing mechanically prevents a future in-kernel deep-chain deriver.
- Guard 6 refuses monitored targets, but an *unmonitored* recipient that is later promoted
  to a monitored service would still lose delegations on restart; today no such promotion
  path exists, so the risk is latent.

## Evidence

- Shipped in: PR #137 — SYS_CAP_DERIVE cross-ring delegation, guard ladder, delegation-test/subagentd citizens (Phase D follow-up, 2026-07-09).
- Shipped in: PR #135 — intent manifest + `manifest_grant`/`manifest_has_room`, the bound and extension mechanism guards 8/11/13 depend on.
- Shipped in: PR #175 — spawn-channel IPC pairs, the wiring that satisfies guard 7 for spawned sub-agents.
- Shipped in: PR #177 — field-region spans + delegable span for agentd (regions 3-6, one delegator over its whole span).
- Key code: syscall.c:1573-1659 (guard ladder), syscall.c:1553-1560 (24-byte request ABI), capability.c:159-206 (cap_derive), capability.c:242-263 (bounded cascade, capture-before-free), capability.c:418-438 (reaper REAP cascade), capability.c:208-229 (dormant cap_transfer), service.c:344-352 + service.h:140-147 (delegable-parent minting), citizens.c:633 and 816 (the only two `grant_field_delegable` setters), service.h:222-226 (monitored-target refusal rationale).
- CI gates: `ci-smoke` delegation leg (Makefile:1087-1102) — requires `DELEG ENFORCED pid=` (subagentd proves recall-ok/imprint-EPERM narrowing, subagentd.c:98), `DELEG REVOKED pid=` (recall flips to EPERM after the delegator exits, subagentd.c:107-108 — the delegated-vs-static proof), and the absence of `DELEG BROKEN`; `ci-smoke` agentd demo gate (`AGENTD: DEMO OK qpu+field+spawn+society`, Makefile:1109) exercises span delegation to a live society; `ci-smoke-society-agents` (Makefile:2062-2066) proves the same across two coupled kernels.
