# 16. Anti-Vacuous CI Gate Discipline

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

An OS kernel cannot be unit-tested like a library — it links `-nostdlib`, so gcov
cannot instrument it (.github/workflows/ci.yml documents this explicitly), and a
"builds and boots" check proves almost nothing about behavior. A green CI that only
proves the image does not crash is worse than no CI: it manufactures false
confidence. Every feature in this codebase is proven by asserting a *runtime signal
the code could not emit unless the feature actually worked* — and the harder problem
is keeping those assertions honest as the code evolves.

## Decision

Gate on un-echoable runtime signals, and treat gate honesty as an architectural
property, not a testing detail. The recurring techniques:

- **Proof by attack.** Capless canary citizens (`ghost-test`, `user-rogue`) are
  booted specifically to be denied; CI greps their deterministic EPERM/isolation
  strings. A feature's *refusal* is proven by a process that provably lacks the
  authority (ADR-0001).
- **Un-echoable evidence.** The CPU-quota kill is proven by the `AUDIT_CPUKILL`
  ledger entry, because the killed hog is dead and cannot self-report (ADR-0009,
  Makefile CPUKILL gate). A ring-3 citizen cannot forge a kernel-anchored line —
  reserved markers are refused (ADR-0015).
- **Vacuity self-checks.** A gate asserts its expected values *differ* before boot,
  so a constant-true predicate cannot ship green: the qsv digest gate checks the OS
  digest equals the host mirror *and* differs from the `--corrupt` mirror
  (Makefile:998-1043); the society gate checks the pre-sync R_x is genuinely low.
- **Revert-and-confirm-fail.** Every boundary/enforcement gate is validated by
  reverting the fix and confirming the gate reddens — the discipline that caught a
  palindromic-seed xor-fold that would have silently zeroed a salt.
- **Verify the fixture, not just the target.** A gate that patches a byte must be
  confirmed to patch the *right* byte (make recipes run under dash — `printf '\xNN'`
  writes a literal, so byte-patching uses python3).
- **Single-sourced timeouts.** Gate timeouts live once in the Makefile
  (`MCP_GATE_TIMEOUT`, `SOCIETY_GATE_TIMEOUT`, …); ci.yml calls `make <target>-gate`
  and never duplicates a `timeout` literal (the #93 desync lesson).
- **Negative gates.** Absence assertions (`QUOTA BROKEN`, `DELEG BROKEN` must *not*
  appear) prove the enforcement path, not just the happy path.

## Consequences

### Positive
- CI failures are meaningful: a red gate names a real behavioral regression, and the
  vacuity self-checks mean a green gate is not silently constant-true.
- The bug-hunt campaign (ADR-0017) leaned on this — ~31 bugs, 0 false positives,
  because every claimed fix landed with a gate that reddened on revert.
- New durable/boundary features inherit a template: assert the un-echoable signal,
  self-check for vacuity, revert-and-confirm.

### Negative
- **`make ci-smoke` (the ~60-gate contributor command) is not invoked by ci.yml** —
  the integration job carries its own inline artifact-driven copy of the gates
  (ci.yml). The two can drift; a contributor's local `ci-smoke` is not byte-identical
  to what CI runs.
- **The flagship `build` job's boot check is decorative**: `timeout 30 make run ||
  echo …` cannot fail the job (ci.yml:53). It reads as a boot gate but is not one;
  the real boot proof is in the integration/iso jobs.
- **No mutation lane.** The anti-vacuity teeth are per-gate and manual; nothing
  continuously reverts a known fix to prove the matching gate still fails. The
  discipline is a practice, not an enforced invariant.
- **Unpinned toolchain**: the kernel builds with the runner's system gcc, so
  ABI/layout-sensitive gates validate against a moving compiler.

### Residual risks
- The audit-ring window (256 entries, ADR-0009) can push evidence past the ring
  before a gate greps it as citizen count grows — a gate can go silently vacuous
  from entry-rate pressure, not code change.
- CI wall-clock is an unlisted scaling wall: four society gates run serially with
  hard timeouts; a phase adding per-boot work risks flaky reds that read as
  failures.

## Evidence
- Shipped in: PR #59 — publish the resonant-scheduler experiment's honest verdict
- Shipped in: PR #93 — single-source gate timeouts (desync fix)
- Shipped in: PR #94 — run PR CI regardless of base branch (stacked-PR zero-CI trap)
- Shipped in: PRs #156-#168 — the bug-hunt campaign's revert-and-confirm gates
- Shipped in: PR #183 — disk-upgrade gate with a mutate-and-fail anti-vacuity check
- Key code: Makefile (ci-smoke targets, qsv digest self-check :998-1043, gate
  timeouts); .github/workflows/ci.yml (integration inline gates, gcov-impossible
  note); scripts/test_qos_mcp.py (reserved-marker forgery refusal)
- CI gates: this ADR is *about* the gate suite — `ci-smoke`, `ci-smoke-disk`,
  `ci-smoke-disk-upgrade`, `ci-smoke-mcp`, `ci-smoke-society*`, `quantum-gateway`
