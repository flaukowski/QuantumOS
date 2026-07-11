# 4. Integer-Only Math

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

QuantumOS context-switches by swapping the general-register frame that `irq_common` saves on
the interrupt stack; there is no `fxsave`/`fxrstor` anywhere in the kernel, so x87/XMM state is
simply not part of a process's saved context. Any floating-point or SSE instruction executed in
ring 0 — a timer ISR, a syscall handler — would silently clobber whichever ring-3 process's FP
registers happen to be live, a corruption with no crash and no trace (README.md:289,
kernel/include/kernel/resonant_fixed.h:5-8). The same reasoning bans FP in ring 3: with no FP
save on switch, two FP-using citizens would corrupt each other. Yet the system's whole identity
is wave math — Kuramoto oscillator fields, resonance scoring, quantum state vectors — the
canonical floating-point workloads.

There is a complicating inherited fact: `boot.S` *enables* SSE at boot (clears CR0.EM, sets
CR4.OSFXSR|OSXMMEXCPT, kernel/src/boot.S:128-137) because the dormant ghostOS resonance
subsystem (`kernel/src/resonance/`) does its math in `double`, is compiled `-msse`
(RESONANCE_CFLAGS, Makefile:235), and stays linked into the image (Makefile:83,104). FP use in
the kernel therefore does not fault — nothing at the hardware level enforces the ban.

## Decision

All *live* code paths, ring 0 and ring 3, use integer arithmetic only, enforced at the build
level: kernel CFLAGS carry `-mno-mmx -mno-sse -mno-sse2` (Makefile:36), the user ABI carries
the same (Makefile:138), and libq's printf deliberately omits `%f` because a float conversion
would drag soft-float into the `-nostdlib` link (user/libq/printf.c:7-8). Each subsystem then
picks the integer representation its math actually needs:

1. **Q16.16 + phase-in-turns Kuramoto** for the resonant-scheduler experiment
   (`kernel/src/resonant_fixed.c`, opt-in `SCHED_RESONANT=1`): phase is a u32 in "turns" so a
   full circle is 2^32 and wraparound is free; trig is a 256-entry Q15 sine table indexed by
   the top 8 phase bits (resonant_fixed.c:49-78); order parameter and priorities are Q16
   fractions (resonant_fixed.h:18-20). This makes the Kuramoto pick safe to run inside the
   timer ISR (scheduler.c:41-47); the double-precision original stays compiled but unwired,
   and the default build filters `resonant_fixed.c` out entirely (Makefile:79-80).
2. **Q15 wave-resonance scoring** for the kernel holographic field (`kernel/src/field.c`):
   patterns are mean-centered at ×256 scale, normalized to Q15 unit wavefronts via a bit-by-bit
   `isqrt_u64` (field.c:49-103), and recall scores each live slot as
   `(cosine_q15 × effective_energy) >> 15` in one bounded pass (field.c:108-122, 229-231).
   The header states the rule as an invariant: "the kernel is -mno-sse with no fxsave anywhere,
   so no float may execute here" (field.h:17-18). ghostd's ring-3 oscillator field and
   quantumd's amplitude-amplification demo follow the same discipline on libq's fixed-point
   helpers (user/quantumd.c:18-20).
3. **Integer SHA-256** for attestation (`user/sha256.h`): a freestanding FIPS 180-4
   implementation in pure uint32 arithmetic — swarm_svc hashes the boot-attestation message and
   expands Lamport one-time-signature keys with it, and a host verifier using Python's hashlib
   computes byte-identical digests (sha256.h:2-12).
4. **The extreme case — exact Gaussian-integer quantum state vectors** (`user/qsv_engine.h`,
   run by both the qsv boot-proof citizen and the qpud executor behind SYS_QPU; ADR-0012).
   This is *not* fixed-point approximation: amplitudes are exact Gaussian integers
   (int32 re, int32 im) carrying one implicit global scale 2^(−h/2) where h counts Hadamards.
   H is the integer butterfly (a,b) → (a+b, a−b) with **zero rounding ever** — no rounded
   binary fixed point can be exact, because no nontrivial 2×2 real unitary has dyadic entries
   (qsv_engine.h:10-18). S multiplies the |1⟩ half by i, exact in Z[i] (qsv_engine.h:126-135);
   T is excluded from v1 as irrational over Z[i] (qsv_engine.h:16). Probabilities are exact
   rationals |v|²/2^h (qsv_engine.h:190-200), and unitarity is the *integer identity*
   Σ|v|² == 2^h, summed in `unsigned __int128` because 4096 × (2^31)² overflows int64
   (qsv_engine.h:176-185). The H butterfly computes in int64 and sets a sticky `qsv_overflow`
   flag on any value that changes under the truncating int32 cast, so deep host circuits are
   rejected rather than wrapped (qsv_engine.h:34-40, 55-76).

## Consequences

### Positive

- ISR- and syscall-safe wave math: the resonant pick, field recall, and every audit/attestation
  hash run in interrupt-disabled kernel context with no possibility of corrupting ring-3 FP
  state, because they never touch it.
- Bit-exact reproducibility turns CI from tolerance checks into **equality gates**: Grover-3q
  must hit p = 121/128 and Grover-4q p = 63001/65536 *exactly* (user/qsv.c:71-109), and the
  in-OS SHA-256 of the final state must equal an independent from-scratch Python mirror's
  digest bit-for-bit while differing from a deliberately corrupted circuit's digest
  (user/qsv.c:111-138, Makefile:998-1043).
- Determinism across compilers and hosts: no FP flags, rounding modes, or x87-vs-SSE precision
  divergence can perturb a result; the same wire bytes produce the same integers everywhere.
- The exact engine makes floating-point *oracles* meaningful rather than circular: PennyLane
  agrees with the integer engine to 1e-9 on identical circuit bytes precisely because the
  integer side has no error of its own to hide in the tolerance (scripts/test_qsv_oracle.py).

### Negative

- The exact engine is deliberately non-universal: T is excluded (irrational over Z[i]), so only
  the H/X/Z/S/CNOT/CZ family plus Grover composites runs; capacity is 12 qubits and int32
  amplitudes, and deep circuits are *refused* (qsv_overflow → EINVAL, user/qpud.c:104-109)
  rather than executed approximately.
- Q15 quantization is real precision loss: cosine truncates at `>>15`, wavefront components
  clamp at ±32767 (field.c:92-101), and the 256-entry sine table quantizes phase to 8 bits —
  fine for ranking and synchronization verdicts, unusable for anything needing more than ~4
  significant digits.
- Developer tax: every new subsystem must bring its own integer trig/sqrt (three separate Q15
  sine tables exist: resonant_fixed.c, ghostd, libq/fx), and printf cannot format a float at
  all, so all diagnostics are integers or rationals by construction.
- Measured honestly under its integer port, the resonant scheduler still loses to round-robin
  on fairness — the shipped anti-starvation aging bounds the damage and the loss is printed at
  boot, not hidden (resonant_fixed.c:39-47).

### Residual risks

- The ban is held by build flags and review, not hardware: SSE is architecturally enabled at
  boot for the dormant `-msse` resonance objects that remain linked (boot.S:128-137,
  Makefile:235). A future call from a live path into `kernel/src/resonance/`, or a stray
  `double` in a new kernel file that dodges `-Werror`, would corrupt ring-3 FP state silently —
  there is no #NM trap and no CI objdump scan for XMM instructions in live objects.
- The sticky-overflow pattern depends on caller ordering: `qsv_overflow` must be checked
  *before* `qsv_norm_ok`, which runs after the wrap and can miss it. qpud does this correctly
  (qpud.c:104-114), but the engine cannot force a future includer to.
- `qsv_reduced_prob` assumes the squared amplitude fits uint32 and the reduced denominator
  exponent stays ≤ 31; qpud guards the latter (hh > 31 → EINVAL) but the invariant lives in
  the caller, not the header.

## Evidence

- Shipped in: PR #57 (integer SHA-256 + Lamport-attested boot, ghostd phase 4)
- Shipped in: PR #58 (fixed-point resonant scheduler + honest rr-vs-resonant benchmark, ghostd phase 5)
- Shipped in: PR #112 (Q15 kernel holographic field, SYS_IMPRINT/SYS_RECALL, epic #95)
- Shipped in: PR #151 (qsv — exact Gaussian-integer state-vector citizen, epic #148 A1)
- Shipped in: PR #152 (SYS_QPU broker; qpud executes the same header-only exact engine)
- Key code: Makefile:36,138 (`-mno-sse` both rings); kernel/src/boot.S:128-137 (SSE enabled for
  the dormant subsystem); kernel/include/kernel/resonant_fixed.h:5-8 (FPU-hazard rationale);
  kernel/src/resonant_fixed.c:49-78 (Q15 table, phase-in-turns); kernel/src/field.c:72-122,
  229-231 (Q15 wavefront/cosine/score); kernel/include/kernel/field.h:17-18 (no-float
  invariant); user/sha256.h:2-12 (uint32 FIPS 180-4); user/qsv_engine.h:10-18,34-76,176-200
  (exact model, sticky overflow, __int128 unitarity); user/qsv.c:71-138 (exact Grover
  assertions + state digest); user/qpud.c:104-114 (overflow-before-norm rejection ordering).
- CI gates: `ci-smoke` qsv digest gate (integer-equality Grover probabilities + digest ==
  independent mirror, != corrupt mirror; Makefile:998-1043); `ci-smoke-resonant` (rebuild with
  SCHED_RESONANT=1, boot to ready, ghostd gate under the alternate policy, honest rr-vs-resonant
  report; Makefile:1844-1880, .github/workflows/ci.yml:500); kannakad "RESONANCE VERIFIED"
  Q15 field recall gate (Makefile:797-807); `quantum-gateway` cross-oracle of the exact engine
  vs PennyLane at 1e-9 on identical bytes (scripts/test_qsv_oracle.py,
  .github/workflows/ci.yml:591-610).
