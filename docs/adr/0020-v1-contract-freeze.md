# 20. Freeze the v1 Agent-Surface Contracts

Date: 2026-07-11
Status: Accepted (all three lanes complete 2026-07-15: guest syscall ABI frozen 2026-07-13 PRs #206–#211; MCP tool surface + COM2/attestation wire frozen 2026-07-15)

## Context

Once QuantumOS is packaged and released (ADR-0018), external consumers depend on
three contracts: the guest **syscall ABI**, the host **MCP tool schemas**, and the
**COM2 frame + attestation format**. Today none is version-pinned or diff-gated, so a
refactor can silently break an agent built against a prior release. But freezing too
early ossifies known mistakes into the published surface. This ADR decides *what* to
freeze, *when*, and *what to fix first* — and records why the freeze is sequenced
after ADR-0019 rather than bundled with the release.

## Decision

Freeze the v1 contracts as committed golden diffs, but only after the pre-freeze
fixes land and ADR-0019 finishes changing the wire.

- **Sequencing.** The freeze is **blocked by ADR-0019**: authenticating the swarm
  plane extends the COM2 frame set and likely the attestation string — exactly the
  contract this ADR pins. Packaging happens now at 0.x (ADR-0018); the `1.0` freeze
  waits.
- **What to freeze.** (a) The 35-syscall table + error-code table + twin-ABI struct
  sizes, as a **committed golden file** diffed in CI — never regenerated in the same
  step (the snapshot trap). #167 proved errno collisions are observable behavior, so
  the error table is part of the contract. (b) The MCP tool schemas as a snapshot
  test in a **new pip-provisioned CI lane** (the integration job is stdlib-only by
  contract, so `import mcp` cannot live there). (c) The COM2 frame set + attestation
  format (hence the ADR-0019 dependency).
- **Pre-freeze fixes (mandatory — do not freeze mistakes).**
  - Add the `qos_qpu_submit` MCP tool over the existing `QosVM.qsubmit` bridge method
    (ADR-0013) — do not freeze a surface that hides the quantum broker.
  - Fix the `qos_memory_import` energy docstring drift (ADR-0015).
  - Fix the `_err` identity misattribution and the Windows-only Kannaka default
    (ADR-0015).
  - **Delete `cap_transfer`** (kernel/src/capability.c:208-229): implemented, no
    caller, deliberately unaudited, its `transferred` stat can never move. Wiring it
    would move delegator authority wholesale, bypassing the one-hop rule (ADR-0010);
    deletion is ~22 lines under the #180/#181 dead-code precedent. Do not freeze an
    ABI with a dead ownership-move sibling in it.
  - Fix the browser-demo path filter that omits `user/**` and `rootfs/**` (a
    user-only merge silently leaves the flagship demo stale).
  - **Reserve the actor-bearing audit record shape** now (ADR-0009's named
    precondition) so the durable ledger format survives a future `SYS_CAP_REVOKE`
    without a geometry break.
- **Ship machine-readable agent docs in the package.** The surface's user is an
  agent; a tool-by-tool contract (args, error shapes, the *verified ≠ live* stance)
  lets an LLM operate the OS from docs alone.

## As-built (2026-07-13) — the guest syscall ABI is frozen and CI-enforced

The freeze shipped as six increments (PRs #206–#211), each through the recon →
panel → gate → merge pipeline:

- **Pre-freeze fixes (#206):** corrected the `field_info_` errno docstring, added
  five symmetric user-side twin `_Static_assert`s, and annotated the intentional
  `svc_restarts`/`qseed` errno-band overlaps as frozen-v1 decisions. (The other
  named pre-freeze items — deleting `cap_transfer`, adding `qos_qpu_submit`, the
  browser-demo path filter — had already landed with the ADR-0019 arc / #185.)
- **Struct refactor (#207):** the six kernel `_k_t` twins split into
  `kernel/include/kernel/syscall_abi.h` so the freeze probe can `#include` them.
- **The golden gate (#208):** `contracts/abi/v1.golden`, compiler-measured via two
  probe TUs (`user/abi_probe.c`, `kernel/src/abi_probe_kern.c`) that emit an
  `.abi_ents` section read back with `objcopy`, diffed by the new `abi-golden` CI
  job. Never regenerated in the same step (`make regen-abi-golden` is human-only);
  a `SYS_QPU` mutation teeth-check proves the gate reddens; a twin cross-check
  proves the user and kernel sides agree.
- **Coverage (#209/#210/#211):** ring-crossing struct **field offsets** (a
  size-preserving reorder reddens the gate), the **capability** resource-type
  namespace + permission bits, the **device-ID** namespace (the `resource_id` a
  `CAP_RESOURCE_DEVICE` cap names), and the arg-page field offsets — **237 golden
  entries** covering the whole security-observable guest contract.

Contract (a), the syscall ABI, is therefore **frozen and Accepted**. The rest is
scoped as follow-ups, not part of this closure:

- **MCP tool schemas (contract b):** need their own pip-provisioned CI lane (the
  integration job is stdlib-only by contract). Deferred.
- **COM2 frame + attestation (contract c):** ADR-0019 is now complete, so this is
  unblocked; deferred to a follow-up golden.
- **Durable audit/manifest record sizes:** intentionally *not* in the golden yet.
  Freezing `audit_entry_t` (40 B) ties into the ADR-0009 actor-bearing-record
  reservation — a future `SYS_CAP_REVOKE` may need an actor field, changing the
  durable on-disk geometry. Recommended resolution (freeze at 40 and let the golden
  make a future reservation a conscious red-CI change, rather than reserve
  durable-format space speculatively for an unbuilt syscall) is left open pending
  an explicit call.

## Update (2026-07-15) — lanes B and C frozen; all three contracts gated

The two deferred lanes shipped as one increment (branch `feat/adr-0020-freeze-lanes`):

- **Wire golden (contract c, `contracts/wire/v1.golden`).** A guest probe TU
  (`user/wire_probe.c`, compiler-measured under real `USER_CFLAGS` — the
  abi-golden pattern verbatim) plus a **live host ring** imported from
  `scripts/qos_bridge.py` by `scripts/extract-wire.py`. The literals both sides
  used are now shared named constants (`user/swarm.h` `SWARM_CRC8_*`/
  `SWARM_ATTEST_*`/reply-auth body lens; the FSYN/FSYP frames moved to
  `user/fsyn.h`; `qos_bridge` module constants), and the extractor
  **twin-cross-checks** guest vs host — plus a MUST-TWIN set so a one-sided
  deletion is a named failure, not a hole. Attestation-string KATs are packed
  from the SHARED macros on the guest and REBUILT from the parser's pieces on
  the host, so emitter and verifier cannot drift apart silently.
- **MCP tool-surface golden (contract b, `contracts/mcp/v1-tools.json`).**
  `scripts/extract-mcp-schema.py` freezes `name + inputSchema` per tool,
  **normalized** (docstring-derived `title`/`description` stripped — pydantic
  re-words those without wire consequence). The generators are **pinned**
  (`requirements-mcp-gate.txt`: mcp 1.28.1, pydantic 2.13.4, pydantic-core
  2.46.4) and recorded in the golden's `_meta`; the check self-verifies the
  environment against those pins first (mismatch = exit 2 `GENERATOR SKEW`,
  operational, never a fake diff). Runs in its own pinned-pip CI job
  (`mcp-schema`, the quantum-gateway shape) — the integration lane stays
  stdlib-only. Pre-freeze fix: `qos_society_boot_n(qseeds: list[str])` so the
  frozen schema has a typed items schema.
- **Shared gate discipline.** Both extractors: exit 1 = contract signal ONLY
  (diff/twin/must-twin), exit 2 = operational; selftests run FIRST in CI and
  assert rc == 1 **exactly** plus the specific mutated marker; teeth live in
  the extractors (never production code) and **emit refuses to run under
  teeth**; regen targets are human-only; goldens are LF-pinned
  (`.gitattributes contracts/**`).
- **Documented-not-gated:** tool descriptions and result-dict shapes are
  excluded from the freeze — they belong to the machine-readable agent-docs
  item (the Decision's last bullet), which remains the follow-up that
  documents them.
- **Follow-ups for the maintainer:** retrofit the same `#` header comment onto
  `contracts/abi/v1.golden` (its extractor does not strip comments yet), and
  add the two new gates to branch-protection required checks.

## Consequences

### Positive
- Golden-diff gates make an accidental ABI or schema change a red CI, not a silent
  break for a downstream agent.
- Fixing the known gaps first means v1 pins a correct surface, not a documented-wrong
  one.
- Reserving the actor-bearing audit shape now protects the durable ledger format
  against the delegation-expansion future (ADR-0009).

### Negative
- The MCP-schema freeze structurally needs its own pip lane — it cannot reuse the
  stdlib-only integration job, so the freeze is *two* CI lanes, not one.
- Golden files are maintenance: every intended ABI change now requires an explicit
  golden update, which is the point but also friction.
- Deleting `cap_transfer` removes optionality some future delegation design might
  have wanted — accepted, because unwired optionality is debt (ADR precedent).

### Residual risks
- A frozen v1 can still encode a subtler mistake the pre-freeze pass missed; semver
  makes that a v2 problem, but the golden diff makes it *visible* at least.
- The actor-bearing record is only *reserved* here, not implemented — a future
  `SYS_CAP_REVOKE` still has to fill it correctly.

## Evidence (baseline)
- Contracts today: syscall table verified byte-identical between user/usys.h and the
  kernel dispatch (ADR-0001 evidence); 21 MCP tools (ADR-0015); COM2/attestation
  (ADR-0014/0015)
- Pre-freeze targets: scripts/qos_bridge.py:811 (unexposed qsubmit),
  qos_mcp.py:211-212 (docstring drift), capability.c:208-229 (cap_transfer),
  audit.h:25-28 (actor-bearing precondition), browser-demo.yml path filter
- Gate patterns to extend: `check-api-consistency.sh` (golden ABI), a new pip lane
  like `quantum-gateway` (MCP schema snapshot)
- Cross-references: ADR-0018 (package/release), ADR-0019 (blocker — extends the wire),
  ADR-0009 (actor-bearing record), ADR-0010 (why cap_transfer must die), ADR-0013 (qsubmit)
