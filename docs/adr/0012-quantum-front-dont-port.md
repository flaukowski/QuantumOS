# 12. Quantum: Front, Don't Port

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

QuantumOS wants real quantum computation as an OS service, but the kernel is freestanding,
integer-only (`-mno-sse` both rings, ADR-0004), and single-core (ADR-0005). Porting a vendor
quantum stack in-tree is not just impractical — the pre-implementation survey (decision workflow
wf_a23ec855, which produced epics #148–150) found it actively dangerous, and those findings are
recorded here because they live nowhere else in-tree: the `pennylane-rigetti` device plugin is
frozen/unmaintained (any Rigetti path must go through the qBraid runtime instead); NVIDIA's
remote `cudaq-qpud` daemon is deprecated *and ships authless* — fronting it directly would graft
an unauthenticated network service onto a capability kernel. Vendor APIs churn on quarters; a
kernel ABI cannot.

The question the workflow settled: what must the OS *own* about quantum execution? Answer:
authority (who may submit a circuit, under what quota) and verifiability (is a returned result
honest) — never the physics. So the stack FRONTS real backends behind one opaque wire format and
proves every foreign result against an engine the OS does own.

## Decision

Three tiers, one wire format, every tier cross-checked against tier 1.

1. **Tier 1 — an exact engine the OS owns.** `user/qsv_engine.h`: amplitudes are exact Gaussian
   integers (int32 re/im) under one implicit scale 2^(−h/2); probabilities are exact rationals;
   unitarity is the integer identity Σ|v|² == 2^h; T is excluded as irrational over Z[i]; deep
   circuits set a sticky `qsv_overflow` flag and are refused, not wrapped
   (qsv_engine.h:10-18,28-40; the full model is ADR-0004 §4). It is header-only so the one-shot
   boot-proof citizen `qsv` (zero capabilities — "its authority is the null set", user/qsv.c;
   roster kernel/src/citizens.c:702-718) and the resident executor `qpud` run the *identical*
   engine (qsv_engine.h:4-8). Air-gapped and boot-CI-verifiable: qsv asserts Grover-3q
   p = 121/128 and Grover-4q p = 63001/65536 *exactly*, then prints a sha256 of the final state
   that must equal an independent from-scratch Python mirror (`scripts/qsv_mirror.py`) and must
   NOT equal the mirror's `--corrupt` digest — anti-vacuous in both directions
   (Makefile:998-1043).
2. **Tier 2 — an opaque wire format through a kernel that never parses it.**
   `user/qpu_circuit.h` defines the little-endian circuit bytes (version, n_qubits 1..12, ops as
   {opcode,a,b} triples; opcodes H/X/Z/S/CNOT/CZ plus the Grover composites ORACLE/DIFFUSION)
   and the 20-byte exact-rational result (qpu_circuit.h:7-22,36-46). The kernel is a broker,
   never a parser (kernel/include/kernel/qpu.h:4-9): SYS_QPU enforces WHO (capability + intent
   manifest, with SUBMIT/POLL and FETCH/COMPLETE as *disjoint* rights that must never co-occur —
   a submitter that could COMPLETE could forge results, qpu.h:11-17), HOW MUCH (manifest qsub
   quota + per-owner in-flight cap), and slot lifecycle, recording AUDIT_QSUBMIT in the ledger on
   accept (kernel/src/qpu.c:119-124). Host agents reach the same broker over the Lamport-attested
   COM2 bridge via `SWARM_OP_QSUBMIT` (user/swarm.h:70-73); swarm_svc submits non-blocking and
   polls one step per main-loop pass so a slow circuit can never starve the watchdog and kill the
   sole COM2 bridge (user/swarm_svc.c:42-49).
3. **Tier 3 — a host gateway that fronts vendors behind an opaque device string.**
   `scripts/qsv_gateway.py` terminates the same wire bytes and dispatches by prefix — PennyLane
   `lightning.qubit`/`lightning.gpu` (cuQuantum)/`lightning.kokkos`/`default.qubit`, `cudaq[:target]`,
   `qbraid:<machine>` — "the OS never learns which vendor ran a circuit" (qsv_gateway.py:4-7,28-36).
   Real QPUs (Rigetti/IonQ/IQM/QuEra via the qBraid runtime) are maintainer-invoked only:
   credentialed, metered, and gated on an explicit `--confirm` flag, without which the script is a
   dry-run that contacts nothing (scripts/qsv_qbraid_submit.py:2-19,33-45); the gateway's qbraid
   branch is a deliberate guarded stub so no autonomous gate can spend credits
   (qsv_gateway.py:128-138). Every float backend is cross-oracled against the exact engine on
   identical bytes: PennyLane to 1e-9 (scripts/test_qsv_oracle.py:27), CUDA-Q to 1e-6 because its
   default target is float32 — "deliberately looser but honest" (scripts/test_qsv_cudaq.py:10-12,27).

Two lessons from PR #156's adversarial bug-hunt are baked into the tests as policy: PennyLane and
CUDA-Q index basis states big-endian while the exact engine is little-endian, and the original
gateway's big-endian readout PASSED every symmetric oracle (Bell/GHZ, Grover's near-uniform
tail) — so ASYMMETRIC circuits are mandatory in every cross-oracle (qsv_gateway.py:45-53,
test_qsv_oracle.py:48-53), and every comparison carries a teeth check: a deliberately different
circuit must disagree grossly or the gate fails as vacuous (test_qsv_oracle.py:63-73).

## Consequences

### Positive

- The kernel ABI is future-proof against vendor churn: adding CUDA-Q was a ~1-file gateway branch
  behind the same opaque bytes (test_qsv_cudaq.py:5-6), and no vendor SDK name appears anywhere in
  ring 0 or ring 3.
- Foreign results are checkable against a reference with zero error of its own: the 1e-9 agreement
  is meaningful precisely because the integer side cannot hide error in the tolerance (ADR-0004).
- Real-QPU spend reconciles into OS authority: the AUDIT_QSUBMIT ledger entry vs the upstream
  qBraid job id closes "who spent this quantum credit, under what grant"
  (qsv_qbraid_submit.py:10-14,74-76; ledger semantics ADR-0009).
- CI splits honestly along the money/hardware line: autonomous hermetic gates (digest, broker,
  gateway, COM2) run on every push; GPU/credentialed legs are `workflow_dispatch` only because
  "GitHub-hosted runners have no GPU and no QPU credentials" (.github/workflows/quantum-phase3.yml:3-8).
- Scale beyond the exact engine is still proven un-fakeably: lightning.gpu vs lightning.qubit on
  GHZ-18/22 to 1e-9 plus the analytic p(0..0)=p(1..1)=1/2 anchor — n > 12 exceeds qsv, so only
  real large-statevector execution passes (scripts/test_qsv_gpu_scale.py:1-13).

### Negative

- Tier 1 is deliberately non-universal: no T gate, 12 qubits, int32 amplitudes, deep circuits
  refused (qsv_overflow → EINVAL before the norm check, user/qpud.c:104-114). It is a reference,
  not a simulator product.
- The CUDA-Q branch maps primitives only; ORACLE/DIFFUSION are not decomposed and raise
  NotImplementedError (qsv_gateway.py:112-114), so CUDA-Q is cross-oracled on Bell/GHZ, never
  Grover (test_qsv_cudaq.py:8-10).
- Real-QPU results are sampled counts under shot noise — they can be *reconciled* (ledger vs job
  id) but never oracled to 1e-9; and the qBraid path maps only H/X/CNOT
  (qsv_qbraid_submit.py:60-67).
- Beyond 12 qubits the exact anchor is gone by construction; the GPU-scale gate degrades to
  backend-vs-backend agreement plus one analytic point.
- The qsub quota charges on broker acceptance even when the executor later rejects the circuit —
  swarm-svc's qsub_max=5 is sized to exactly this accounting (kernel/src/citizens.c:449-457).

### Residual risks

- The cross-oracles compare probability vectors only; a backend with correct magnitudes but wrong
  relative phases passes every gate (the exact result carries amp_re/amp_im, qpu_circuit.h:21-22,
  but no gateway backend returns amplitudes to compare against).
- Hermeticity is bought with pins (`pennylane==0.45.1`, `pennylane-lightning==0.45.0`,
  `cudaq==0.15.0`; qsv_gateway.py:22-23) — the pins themselves will age into the same
  frozen-plugin problem the survey flagged, and nothing automatic revisits them.
- The asymmetric-circuit discipline is enforced by the shared case list in test_qsv_oracle.py, not
  by construction; a future backend tested outside that list can reintroduce the PR #156 bug class.
- Phase-3 legs run only when a maintainer dispatches them with the right runner/secret; the GPU and
  real-QPU paths can silently rot between dispatches (exit-2 skip semantics mean an absent backend
  is a skip, not a failure).
- The COM2 wire collapses failure detail to refused(1)/error(2) (swarm.h:71-73); a host cannot
  distinguish quota exhaustion from EPERM without the OS-side ledger.

The other quantum-hardware touchpoint — boot entropy from a real QPU via `qseed=` into
SYS_QRAND/SYS_QSEED — is a separate decision with the same fronting philosophy (ADR-0013).

## Evidence

- Shipped in: PR #151 — qsv exact Gaussian-integer state-vector citizen + boot proof (epic #148 A1)
- Shipped in: PR #152 — SYS_QPU job broker + qpud executor + qpu_test quota/partition proofs (epic #148 A2+A3)
- Shipped in: PR #153 — host QPU gateway → PennyLane lightning.qubit + hermetic cross-oracle gate (epic #149 B2)
- Shipped in: PR #154 — COM2 SWARM_OP_QSUBMIT transport, host → broker over the attested bridge (epic #149 B1)
- Shipped in: PR #155 — Phase 3 backends: lightning.gpu (cuQuantum), CUDA-Q, qBraid real-QPU path (epic #150)
- Shipped in: PR #156 — adversarial bug-hunt: big-endian gateway readout fix (masked by symmetric
  oracles → asymmetric circuits mandatory), broker death-sweep IF=1 race, and 5 more
- Key code: user/qsv_engine.h:4-18,28-40 (exact model, header-only idiom, sticky overflow);
  user/qsv.c + kernel/src/citizens.c:702-718 (zero-cap boot proof); user/qpu_circuit.h:7-22,36-46
  (opaque wire format); kernel/include/kernel/qpu.h:4-31 (broker-never-parser, disjoint
  SUBMIT/EXECUTE rights, fail-closed death sweep); kernel/src/qpu.c:119-124 (charge-last +
  AUDIT_QSUBMIT); user/qpud.c:104-114 (overflow-before-norm rejection); user/swarm.h:70-73 +
  user/swarm_svc.c:42-49 (non-blocking wire submit); scripts/qsv_gateway.py:28-36,45-53,128-138
  (opaque dispatch, endianness reversal, guarded qBraid stub); scripts/qsv_mirror.py:1-21
  (independent digest oracle); scripts/qsv_qbraid_submit.py:33-45,74-76 (--confirm + ledger
  reconciliation)
- CI gates: `ci-smoke` qsv digest gate (exact Grover probabilities; OS digest == mirror,
  != --corrupt; Makefile:998-1043) and QPU broker gate (job-id cross-reference + four authority
  denials; Makefile:1044-1079); `quantum-gateway` hermetic cross-oracle with teeth check
  (.github/workflows/ci.yml:591-610, scripts/test_qsv_oracle.py); `quantum-com2` /
  `ci-smoke-qsubmit` (wire path + B1↔B2 cross-oracle + wire quota; ci.yml:612-634,
  Makefile:2036-2038, scripts/test_qos_qsubmit.py); manual `quantum-phase3.yml` legs cudaq /
  gpu-scale / qbraid (workflow_dispatch only)
