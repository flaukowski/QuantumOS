# 11. Spawn channels: the society roster is the spawn returns

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

Before epic #175, every IPC pair in the system was hand-wired at boot by ring 0:
citizens.c mints per-pair `CAP_RESOURCE_IPC` caps for each known client/service
couple (e.g. qsh↔ghostd, citizens.c:558-560). A process created at runtime via
SYS_SPAWN was born with no channel to anyone — fine for `run /bin/hello`, fatal
for an orchestrator that needs to assemble a society of sub-agents and then
delegate narrowed field caps to each (SYS_CAP_DERIVE requires the delegator to
hold an IPC send-cap for exactly the target pid — the peer requirement,
ADR-0010). The alternative designs both fail: a name-registry the children
announce into reintroduces the roster race (who vouches the announcer?), and
blanket minting of channels on every spawn breaks a load-bearing invariant —
untargeted `send_msg` routes FIRST-MATCH over the sender's IPC caps, so a
second cap in a lower first-fit slot would silently redirect qsh's `ghost`
builtin to a dead child (service.h:165-173). Worse, pids are first-fit slot
indices: any channel cap left targeting a dead pid becomes a live channel into
whatever unrelated process recycles that slot (syscall.c:1245-1253).

## Decision

SYS_SPAWN mints a tagged, bidirectional parent↔child IPC cap pair — but only
for citizens that opt in.

- **Opt-in, declared, never ambient.** `grant_spawn_channel` is a service
  definition flag (service.h:165-173) copied into the manifest inside the same
  cli window as every other grant (`man.spawn_channel`, service.c:380).
  `manifest_spawn_channel()` reads it back with unbound = 0
  (manifest.c:176-182). Today exactly one citizen sets it: agentd
  (citizens.c:805-807). qsh deliberately does not — the blanket-minting
  rejection above is written into the flag's own doc block.
- **Capacity precheck before any side effect.** sys_spawn checks
  `cs.active + 2 > MAX_CAPABILITIES` before copy-in, parse, or ELF load; a
  refusal is audited (`audit_deny` on IPC/ANY) and returns EIO
  (syscall.c:1201-1209). The postcondition is all-or-nothing: a returned pid
  ALWAYS carries a fully wired channel, never a half-minted one whose mute
  child times the demo out minutes from the fault. Race-free because syscalls
  run cli'd on one CPU and the IF=1 reaper only frees slots.
- **Mint after `spawn_elf_args` succeeds:** two `cap_create` calls
  (parent→child and child→parent, R|W), each immediately
  `cap_mark_spawn_channel`'d (syscall.c:1254-1264; capability.c:440-447;
  origin tag field capability.h:83). **No `manifest_grant` on either side** —
  IPC caps are pair-wise runtime wiring, deliberately outside the manifest,
  and the derive peer check is cap-only (syscall.c:1250-1253).
- **Death-side teardown closes the pid-recycle window.**
  `cap_revoke_spawn_channels(dead_pid)` frees every cap matching THREE filters
  simultaneously: origin tag ∧ `CAP_RESOURCE_IPC` ∧ `resource_id == dead_pid`
  (capability.c:449-478). Each freed cap is recorded as **AUDIT_UNLINK under
  its SURVIVING owner** — never REAP, which must keep meaning "owner died"
  (audit.c:102-108, capability.c:475). The call site is pinned in
  process_destroy after `cap_revoke_all_for_process` and **before the
  state=UNUSED store** (process.c:466-475): every pid-reuse path funnels
  through process_destroy, so unlink-here is what keeps a recycled pid from
  inheriting a live inbound channel minted for its predecessor.

The agent-society arc self-assembles on this primitive. agentd spawns three
`/bin/agentsub` off the initrd (agentsub is deliberately NOT kernel-embedded,
citizens.c:53-54) and **the roster IS the spawn returns** — no registry, no
hand-wiring (agentd.c:199-219). Liveness "ready" and consensus acks are
deduped against that roster by kernel-vouched sender pid; an unknown or
duplicate sender never counts (agentd.c:221-242, 300-315). Both per-specialist
derives — region 3 READ-only shared knowledge, region 4+i READ|WRITE private
workspace (epic #177) — ride the spawn-minted pair (agentd.c:244-276).
Consensus is over content: each sub acks an FNV-1a digest of the phrase it
actually recalled, checked against the orchestrator's independent digest
(agentd.c:284-322). The society-of-societies aggregate (epic #178) is handed
to fieldsyncd and exchanged as FSYP frames that are **host-verified, never
imprinted** — the v1 field-replication design was rejected in review because
unauthenticated wire data must not become recallable field content
(fieldsyncd.c:38-49; ADR-0014).

## Consequences

### Positive

- Societies self-assemble with zero kernel hand-wiring and zero registry race:
  the kernel vouches every sender pid, and the spawn return is the only roster.
- The channel doubles as the delegation conduit — SYS_CAP_DERIVE's IPC-peer
  requirement is satisfied by construction for exactly the children the
  orchestrator spawned (ADR-0010).
- Teardown is honest and audited from both directions: the dead side's own
  caps REAP, the surviving peer's half UNLINKs, and the ledger distinguishes
  the two kinds so a verifier never mistakes garbage collection for revocation.

### Negative

- Opt-in means every non-agentd spawner's children are born mute: a `run`
  child under qsh has no channel to anything, by design. Until untargeted
  `send_msg` routing grows real addressing, opting qsh in is impossible, not
  merely unimplemented.
- The channel is invisible to SYS_MANIFEST — deliberate (IPC is outside the
  manifest), but it means declared-intent inspection cannot show society
  wiring; only the ledger's GRANT/UNLINK entries and `cap_stats` reflect it.
- The flag is per-service and static: a citizen cannot choose per-spawn
  whether a child gets a channel.

### Residual risks

- Channels exist only at spawn time. If a channel-holding spawner died and
  were watchdog-restarted, the rebirth would hold no channels to surviving
  children (its old caps REAPed, the children's halves UNLINKed) and there is
  no re-establishment path — moot today because agentd is unmonitored and
  one-shot (citizens.c roster), but load-bearing for any future monitored
  orchestrator.
- AUDIT_UNLINK records whose holding vanished and which pid died
  (resource_id), but per the ledger-wide rule the acting cause is not recorded
  (audit.h:25-28) — a kill and a natural exit are indistinguishable in the
  ledger.
- The triple filter assumes only sys_spawn ever tags caps, and only IPC ones.
  The CAPUNLINK self-test deliberately constructs a tagged FIELD cap to keep
  the type filter non-vacuous (capability.c:591-598), but the tag API itself
  is kernel-internal and unguarded — a future caller tagging non-channel caps
  would silently widen the sweep.

## Evidence

- Shipped in: PR #175 (spawn-time parent↔child IPC channels — the society
  self-assembles); PR #176 (CI gate for qsh's singleton-IPC-cap first-match
  invariant, `ghost` AFTER `run`); PR #171/#173/#174 (agent demo → society →
  content-digest consensus); PR #177 (division-of-labor workspaces); PR #178
  (society-of-societies FSYP aggregates).
- Key code: syscall.c:1181-1270 (sys_spawn: precheck + mint);
  capability.c:440-478 (tag + triple-filter unlink); capability.h:83 (origin
  tag); process.c:466-484 (before-UNUSED call-site invariant);
  service.h:165-173 (opt-in rationale); service.c:380 (manifest bind);
  manifest.c:176-182 (never-ambient read); citizens.c:805-807 (agentd, the
  sole holder); audit.c:102-108 (UNLINK under the surviving owner);
  agentd.c:199-322 (roster = spawn returns; vouched-pid dedup; consensus);
  agentsub.c:1-25 (channel as the child's only birth capability);
  fieldsyncd.c:38-49 (aggregates printed, never imprinted).
- CI gates: `ci-smoke` boot leg `CAPUNLINK: spawn-channel caps die with their
  target` (capability.c:581-621; Makefile:481-482); `ci-smoke` second-`ghost`
  leg (Makefile:429-435) proving no blanket minting regression; the agentdemo
  conjunction inside `ci-smoke` — `AGENTD: DEMO OK qpu+field+spawn+society`
  (Makefile:1109), `AGENT: society consensus 3/3` (Makefile:1115),
  `AGENT: division of labor 3/3` (Makefile:1123), `AGENT: aggregate a`
  (Makefile:1132); `ci-smoke-society-agents-gate` (Makefile:2064,
  .github/workflows/ci.yml:950-954) for the two-society FSYP exchange.
