#!/usr/bin/env python3
"""CUDA-Q cross-oracle (epic #150 C3): NVIDIA CUDA-Q vs the exact integer engine
(qsv_mirror) on IDENTICAL circuits. A third utterly independent implementation
(CUDA-Q's C++ statevector) must agree with the exact Gaussian-integer engine —
proving the SAME opaque circuit runs correctly through the CUDA-Q dispatch path,
so adding CUDA-Q as a backend is a ~1-file branch, not a second architecture.

Gate set: the primitive Bell/GHZ circuits (H/X/CNOT); Grover's ORACLE/DIFFUSION
composites are not yet decomposed for CUDA-Q (a documented follow-up), so this
proves the dispatch + primitive translation, not Grover. Tolerance 1e-6 —
CUDA-Q's default target is single precision (float32), unlike PennyLane's
float64 path (1e-9); this is a deliberately looser but honest bound.

Autonomous: CUDA-Q's local CPU target ("qpp-cpu") runs free with no GPU and no
credentials (Apache-2.0 wheel). Exits 0 on agreement, 2 if cudaq is absent.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import qpu_circuit as qc
import qsv_mirror
import qsv_gateway

TOL = 1e-6  # CUDA-Q default target is float32; PennyLane's fp64 path uses 1e-9


def main():
    device = sys.argv[1] if len(sys.argv) > 1 else "cudaq"
    try:
        import cudaq  # noqa: F401
    except ImportError:
        print("SKIP: cudaq not installed (Apache-2.0; pip install cudaq) — the "
              "PennyLane cross-oracle covers the gateway hermetically")
        sys.exit(2)

    cases = [
        ("bell", qc.bell()),
        ("ghz3", qc.ghz(3)),
        ("ghz6", qc.ghz(6)),
        ("h_s_h", qc.h_s_h()),
        # asymmetric — catches any qubit-ordering bug the symmetric cases hide.
        ("H0-3q", qc.build(3, [(qc.QC_OP_H, 0, 0)], 0)),
        ("H0-X2-3q", qc.build(3, [(qc.QC_OP_H, 0, 0), (qc.QC_OP_X, 2, 0)], 0)),
    ]
    ok = True
    for name, circ in cases:
        _n, _probe, exact = qsv_mirror.run_bytes(circ)
        exact_f = [float(p) for p in exact]
        cq = qsv_gateway.probs(circ, device)
        worst = max(abs(a - b) for a, b in zip(exact_f, cq))
        if worst > TOL:
            ok = False
        print(f"{'OK  ' if worst <= TOL else 'FAIL'}: {name:6s} exact-vs-{device} worst={worst:.2e}")

    # Teeth: the exact GHZ-3 vs CUDA-Q on a DIFFERENT circuit (Bell) must differ.
    _n, _p, exg = qsv_mirror.run_bytes(qc.ghz(3))
    exg_f = [float(p) for p in exg]
    cq_bell = qsv_gateway.probs(qc.bell(), device)
    # pad/truncate for comparison length (bell is 2q=4 states, ghz3 is 3q=8)
    m = min(len(exg_f), len(cq_bell))
    mismatch = max(abs(exg_f[i] - cq_bell[i]) for i in range(m))
    if mismatch <= 0.1:
        print(f"FAIL: teeth check — different circuits agree (worst={mismatch:.2e})")
        ok = False
    else:
        print(f"OK  : teeth check — different circuits disagree (worst={mismatch:.2e} > 0.1)")

    print("CUDA-Q CROSS-ORACLE " + ("PASSED" if ok else "FAILED"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
