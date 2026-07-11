# 17. Honest-Slice Engineering

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

QuantumOS makes large claims — a quantum-aware OS, associative kernel memory,
attested agent societies. The failure mode for a project like this is the demo that
implies more than it does: a "TCP stack" that is one hard-coded connection, a
"scheduler research result" that quietly buries a negative outcome, "quantum" entropy
that is actually a PRNG. The decision here is a norm, not a feature: ship the narrow
real slice, document its boundary in the same breath, and report negative results
plainly. It is what makes the large claims trustworthy.

## Decision

Every subsystem ships as an explicitly bounded slice with its limits in-tree:

- **TCP is a client-focused stop-and-wait slice**: two static TCBs (one client, one
  server), ≤ MSS single-segment, in-order-only receive, ~1s TIME_WAIT — and the
  boundaries are documented in `docs/NETWORKING.md`, not discovered by a user
  (kernel/src/net_tcp.c). A server slice (listen/accept + httpd) was added later and
  labelled as its own slice, not retrofitted into the "client-only" prose.
- **Negative results are published, not hidden**: the resonant scheduler is wired,
  run at boot against round-robin on an identical workload, and its verdict — *it
  loses to round-robin* — is printed unedited and asserted in CI (ADR-0016). The #21
  pure-argmax hypothesis is refuted by execution.
- **Provenance honesty**: ghostd prints `noise source = qseed-derived quantum pool`
  only with a real seed, and `prng (no qseed)` otherwise — the OS never claims
  quantum provenance it does not have (README quantum section, gated by
  `ci-smoke-qseed`).
- **Documented hardware limits**: RAM beyond 128 MB is ignored, BIOS/CSM boot only
  (no UEFI), keyboard via BIOS PS/2 emulation — all stated in the README, not
  papered over.
- **Attestation honesty**: the host surface states *verified ≠ live* (ADR-0015) —
  the Lamport signature covers the boot string, and the code says so rather than
  implying end-to-end liveness.

## Consequences

### Positive
- The big claims are credible precisely because the small print is in the repo: a
  reader can find the boundary of every slice without running it.
- Negative results compound into knowledge — the resonant-scheduler refutation is a
  cited result, not a silent dead end.
- Honest provenance protects the quantum claim: "quantum-seeded" means it, because
  the seedless path says PRNG out loud.

### Negative
- Recorded dishonesty that predates the norm and must be corrected: the Multiboot2
  magic is *accepted* by `boot_validate_multiboot` while every consumer parses
  Multiboot v1 offsets — a latent trap for a future MB2 loader (verified dead code
  today because `boot.S` carries only an MB1 header). Rejecting the MB2 magic is a
  now-hardening fix (ADR-0021).
- The doc set drifted badly from the code before this ADR batch: several design docs
  described systems that were never built (a HAL, a 3-architecture matrix, user-space
  service daemons) — the opposite of honest-slice, corrected by the doc refresh and
  the retirement of fiction docs alongside these ADRs.
- Honest slices invite "why not the whole thing?" — the discipline requires
  defending narrowness (a client-only TCP is *enough* for the mission) against a pull
  toward completeness for its own sake.

### Residual risks
- The norm is cultural, enforced by review and the ADR record, not by a mechanism.
  A future PR can quietly overclaim; the defense is that the ADR set now names the
  norm so overclaiming is a reviewable violation.
- "Documented limit" only helps a reader who reads the doc — the README limits list
  must stay current or an honest slice becomes a stale slice (the reason the doc
  refresh is part of this session's work).

## Evidence
- Shipped in: PR #58 / #59 — resonant scheduler measured honestly, verdict published
- Shipped in: PR #54 — qseed-traced noise provenance (prng vs quantum)
- Shipped in: PR #83 — TCP client slice with documented boundaries; #118 server slice
- Shipped in: PRs #103-#111 — real-hardware boot with its BIOS/PS2/128MB limits stated
- Key code: kernel/src/net_tcp.c (2 TCBs, stop-and-wait); kernel/src/scheduler.c
  (opt-in resonant pick, verdict at boot); README.md "What runs today" (limits);
  kernel/src/main.c:706-719 (the MB2-magic honesty debt)
- CI gates: `ci-smoke-resonant` (verdict printed, still boots), `ci-smoke-qseed`
  (provenance line), `ci-smoke-httpd` (server slice)
