# 13. QPU Job Broker Brokers Opaque Payloads

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

The quantum stack (ADR-0012) puts a real state-vector engine (`qsv`) in ring 3 and
real backends on the host. The kernel sits in the middle as the authority boundary:
an agent submits a circuit, the executor runs it, the result comes back. The naive
design has the kernel parse and validate circuits — which drags a quantum circuit
grammar, opcode bounds, and amplitude arithmetic into ring 0, exactly the FPU-using,
attacker-shaped code ADR-0004 keeps *out* of the kernel. It also conflates two
different rights: the right to *ask* for a computation and the right to *perform*
one. If both are the same capability, any submitter can impersonate the executor.

## Decision

`SYS_QPU` (syscall 35) is a **broker that never parses circuits**. Payloads are
opaque byte blobs (kernel/src/qpu.c); the kernel copies them in behind the
ADR-0003 user-pointer guard, queues a job, and hands the bytes to whichever process
holds the executor right — it never inspects an opcode. The circuit wire format
(`user/qpu_circuit.h`) is a ring-3/host contract; the kernel is format-agnostic.

Rights are **partitioned disjointly** over `DEVICE_ID_QPU` (0x5150). Submit is
`CAP_WRITE`-only; poll/fetch/complete require `CAP_READ|CAP_EXECUTE`. `grant_qpu_submit`
mints WRITE and never EXECUTE; `grant_qpu_execute` mints READ|EXECUTE and never WRITE
(kernel/src/service.c:269-289). A submitter therefore cannot masquerade as the
executor and a compromised executor cannot inject jobs. `qpud` is the sole
EXECUTE holder; a second live EXECUTE holder or a stale one is refused with an
audit_deny (kernel/src/qpu.c:160-164).

Submission is **quota-charged at accept**: `qsub_max` in the intent manifest
(ADR-0008) is checked and charged the moment a job enters the broker, before the
executor sees it — so a malformed circuit that the executor will reject still
spends quota (the submission *reached* the broker; charging only on success would
make the quota free to probe). Each accept records `AUDIT_QSUBMIT` under the
submitter (kernel/src/qpu.c:123). The executor is stamped with pid+generation so
that executor death frees the slot cleanly: `qpu_on_process_destroy` sweeps the
broker in the ADR-0002 destroy ordering, closing an executor-death slot-leak DoS
(EAGAIN starvation of every future submit).

## Consequences

### Positive
- Ring 0 carries zero quantum semantics: the circuit grammar can evolve entirely
  in ring 3 + host without a kernel change, and no amplitude math ever runs
  supervisor-side (ADR-0004).
- The disjoint submit/execute partition is the whole security story in two grant
  helpers — a submitter provably cannot execute, an executor provably cannot submit.
- Quota is honest under malformed input: a garbage circuit charges the submitter,
  so quota cannot be probed for free (kernel/src/qpu.c:75, 122-123).
- The best conscience-before-wallet demo the OS has: a capability-gated,
  quota-bounded, ledgered quantum job — authority you can watch in the audit ring.

### Negative
- The kernel cannot reject a malformed circuit early; it charges quota and defers
  the EINVAL to `qpud`, which fail-closes malformed payloads. This is deliberate
  (the kernel must not parse) but means a submitter learns validity only at
  completion.
- The quantum broker is now agent-reachable: `qos_qpu_submit` (the 22nd MCP tool)
  delegates to `QosVM.qpu_run` → `QosVM.qsubmit`, so an agent can run a named
  capability-gated, quota-metered, `AUDIT_QSUBMIT`-ledgered circuit and get the exact
  decoded result. Gated by `ci-smoke-qsubmit` (the `qpu_run('bell')` assertion). This
  closed the ADR-0015 "qsubmit unexposed" gap and an ADR-0020 pre-freeze target.
- One executor by construction: no parallel QPU service, no failover — an executor
  restart is a brief submit outage (bounded by the ADR-0002 watchdog).

### Residual risks
- Opacity means the kernel trusts the executor to honour the wire format; a buggy
  `qpud` cannot corrupt the kernel (payloads stay in ring 3) but can return wrong
  results — the host cross-oracle (ADR-0012) is what catches that, not the broker.
- `qsub_max` is charged but, like all manifest quotas, never refunded on rejection;
  a citizen that submits only malformed circuits exhausts its own quota — intended,
  but worth stating.

## Evidence
- Shipped in: PR #152 — SYS_QPU broker, disjoint submit/execute partition, qsub
  quota, AUDIT_QSUBMIT, executor-death sweep (Phase 1 A2+A3, epic #148)
- Shipped in: PR #154 — COM2 quantum-submit transport reaching the broker (Phase 2 B1)
- Key code: kernel/src/qpu.c:75 (quota precheck), :122-123 (charge + AUDIT_QSUBMIT),
  :160-164 (sole-executor guard); kernel/src/service.c:269-289 (disjoint grants);
  user/qpu_circuit.h (opaque wire format); scripts/qos_bridge.py:811 (host qsubmit)
- CI gates: `ci-smoke` QPU broker gate (job-id cross-reference + four authority
  denials, Makefile:1044-1079); `quantum-com2` / `ci-smoke-qsubmit` (wire path +
  quota, ci.yml:612-634)
