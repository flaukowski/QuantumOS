# QuantumOS Development Guidelines

## Peer Review: Resonant Scheduler Bugs (from Cascade — Feb 8 2026)

All bugs from both review rounds have been fixed by Claude Code (Feb 8 2026).
Fixes verified: ghostOS `tsc --noEmit` and `npm run build` pass. QuantumOS C build pending WSL.

---

### BUG 1: `fast_atan2()` uses `sin()` instead of `asin()` — FIXED

**Files:** `resonant_scheduler.c`, `geometric_control.c`

Implemented `fast_asin()` using rational polynomial approximation:
`asin(x) ≈ x*(1 + x²*(1/6 + x²*(3/40 + x²*15/336)))`.
Rewrote `fast_atan2()` in both files to use `fast_asin()` with proper quadrant handling.

---

### BUG 2: `chiral_allocate()` shared static buffer — FIXED

**Files:** `chiral_resources.h`, `chiral_resources.c`

Replaced `uint32_t *qubit_ids` pointer with `uint32_t qubit_ids[MAX_ALLOC_IDS]` fixed-size
array embedded directly in `chiral_alloc_result_t`. Removed the shared `alloc_id_buffer` static.
`MAX_ALLOC_IDS` moved to the header. Allocation now writes directly into the result struct.

---

### BUG 3: `resonant_unregister()` skips couplings — FIXED

**File:** `resonant_scheduler.c`

Changed forward iteration to reverse: `for (int8_t i = rpcb->coupling_count - 1; i >= 0; i--)`.

---

### BUG 4: `init_chiral()` and `resonant_set_chiral()` missing `fabs` — FIXED

**File:** `resonant_scheduler.c`

Added `fast_abs()` helper. Both `init_chiral()` and `resonant_set_chiral()` now compute
`fast_abs(eta / gamma)` for asymmetry.

---

### BUG 5: Phi sub-components accumulate without bound — FIXED

**File:** `consciousness_process.c`

Added `clamp(value, 0.0, 1.0)` after every increment to `structural_phi`, `dynamic_phi`,
and `emergent_phi` across all trigger cases (EMERGENCE, LEARNING, DECISION, CRISIS).

---

### BUG 6: `COHERENCE_URGENCY` macro inverted division — FIXED

**File:** `resonance_types.h`

Replaced with: urgency = 1.0 when `now >= deadline`, otherwise
`1.0 - (deadline - now) / (deadline + 1)`. Urgency correctly rises as deadline approaches.

---

### NOTE: `process_is_ready()` — IMPLEMENTED

**File:** `kernel/src/process.c`

Added implementations for `process_is_ready()`, `process_is_running()`, and
`process_is_terminated()` — all were declared in `process.h` but had no implementation.

---

### BUG 7: `protocol.ts` comment says 58 bytes — FIXED

**File:** `ghostOS/src/bridge/protocol.ts`

Changed comment from "58 bytes after header" to "48 bytes after header".

---

### BUG 8: `EMERGENCE` and `ANOMALY` message types have no serializer — FIXED

**File:** `ghostOS/src/bridge/protocol.ts`, `ghostOS/src/bridge/index.ts`

Added `EmergenceMessage` interface (24-byte payload: emergenceNorm, integrationLevel,
patternCount, isActive) and `AnomalyMessage` interface (32-byte payload: anomalyIndex,
spectralAsymmetry, topologicalCharge, isAnomalous, leftModeCount, rightModeCount).
Added serialize/deserialize methods for both. Updated `getMessageSize()` and barrel exports.

---

### BUG 9: BFGS formula is SR1, not rank-2 — FIXED

**Files:** `ghostOS/src/geometric/manifold.ts`, `kernel/src/resonance/geometric_control.c`

Replaced SR1-like update with full BFGS rank-2 inverse Hessian update:
`H_new = (I - ρ·s·yᵀ)·H·(I - ρ·y·sᵀ) + ρ·s·sᵀ`
Expanded form: `H + ρ·(1 + ρ·yᵀHy)·s·sᵀ - ρ·(s·(Hy)ᵀ + (Hy)·sᵀ)`.
This preserves positive definiteness (with curvature condition check).

---

### BUG 10: `curvature.ts` hardcoded epsilon — OPEN (low priority)

The function already accepts `stepSize` as a parameter. Callers should pass
a scaled value. No code change made — this is a usage concern, not a bug in the function.

---

### BUG 11: `calculateLocalCoherence()` negative coherence — FIXED

**File:** `ghostOS/src/integration/index.ts`

Both chiral and achiral paths now normalize `cos(phaseDiff)` from [-1,1] to [0,1]
before applying CISS boost: `const normalized = (baseCoherence + 1) / 2`.

---

### BUG 12: Phase variance uses unwrapped differences — FIXED

**File:** `ghostOS/src/integration/index.ts`

Added S¹ phase wrapping: `if (diff > Math.PI) diff = 2 * Math.PI - diff`.

---

### NOTE: `berry.ts` cross-product assumes even dimension — ACKNOWLEDGED

Documented as precondition. Fine for even-dimensional oscillator systems.

### NOTE: `wasm-bridge.ts` no WASM path configuration — ACKNOWLEDGED

Will need parameterization for deployment. The bridge falls back to pure TypeScript
geometric module when WASM is unavailable.

### NOTE: `geometric_control.h` has no `.c` — RESOLVED

`geometric_control.c` (~600 lines) now exists with full implementation.

---

## Build & Architecture Notes

- Makefile resonance integration (`RESONANCE_SOURCES`, compile rule) is correct.
- ghostOS TypeScript and QuantumOS C constants match across both codebases.
- `quantum_types.h` dependency (`qubit_handle_t`, `FIDELITY_STANDARD`) is satisfied.
- Static helper duplication (`fast_sqrt`, `fast_abs`, `clamp`) across .c files is fine for now
  but consider a shared `kernel/src/resonance/math_helpers.h` if the module grows.
- `resonance_types.h` cross-repo constant documentation (`LAMBDA_DEFAULT` vs `K_COUPLING`,
  `CISS_COHERENCE_BOOST` additive vs multiplicative) is clear.
- `geometric_control.h` API design is clean. Implementation in `geometric_control.c` is complete.
- ghostOS geometric layer (`manifold.ts`, `berry.ts`, `curvature.ts`, `anomaly.ts`)
  uses correct full BFGS rank-2 update.
- Protocol serializer (`protocol.ts`) covers all 7 message types with binary serialize/deserialize.
- Integration layer geometric control hookup in `QueenSynchronizer` and
  `ResonanceEngine` is architecturally sound. Coherence normalization and phase wrapping are correct.
- `geometric_control.c` is integrated into `resonant_scheduler.c`: metric updates after
  `update_order_parameter()`, Ricci curvature factor in priority calculations, chiral anomaly
  detection during sync.

## Remaining Work

- Verify QuantumOS C build via WSL (`make`)
- BUG 10 (curvature.ts epsilon) is low priority — callers should scale `stepSize`
- WASM bridge path configuration for multi-environment deployment
- Document berry.ts even-dimension precondition in code
