#!/usr/bin/env python3
"""GPU-scale proof (epic #150 C1): PennyLane lightning.gpu (NVIDIA cuQuantum
cuStateVec) vs lightning.qubit (CPU) on GHZ-N circuits BEYOND qsv's 12-qubit
exact range. Two independent PennyLane backends must agree to 1e-9, and GHZ has
an analytic anchor p(0..0)=p(1..1)=1/2 — so a lightning.gpu that silently fell
back to CPU, or gave a wrong answer, cannot pass. Because n > 12 exceeds the
exact integer engine, qsv cannot "fake" this — only real large-statevector
execution produces it.

MANUAL / GPU-only: needs pennylane-lightning-gpu + custatevec-cu12 + a
compute-capability >= 7.0 NVIDIA GPU (Volta+). Not a GitHub-runner CI gate;
run locally or on a self-hosted GPU runner. Exits 0 on success, 2 if the GPU
backend is unavailable (skip, not fail).
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import qpu_circuit as qc
import qsv_gateway

# 2^N complex64 amplitudes: 22q = 4.2M states x 8 B = 32 MB (fits a 4 GB card).
QUBITS = [int(x) for x in os.environ.get("QSV_GPU_QUBITS", "18,22").split(",")]
TOL = 1e-9


def main():
    try:
        import pennylane as qml
        qml.device("lightning.gpu", wires=2)  # probe availability
    except Exception as e:  # noqa: BLE001 — any import/init failure = skip
        print(f"SKIP: lightning.gpu unavailable ({type(e).__name__}: {e}) — needs cuQuantum + a "
              "CC>=7.0 GPU")
        sys.exit(2)

    ok = True
    for n in QUBITS:
        circ = qc.ghz(n, max_qubits=64)
        t0 = time.time()
        pg = qsv_gateway.probs(circ, "lightning.gpu")
        tg = time.time() - t0
        pc = qsv_gateway.probs(circ, "lightning.qubit")
        worst = max(abs(a - b) for a, b in zip(pg, pc))
        p0, plast = pg[0], pg[-1]
        agree = worst <= TOL and abs(p0 - 0.5) <= TOL and abs(plast - 0.5) <= TOL
        ok = ok and agree
        print(f"{'OK  ' if agree else 'FAIL'}: GHZ-{n:<2d} "
              f"gpu-vs-cpu worst={worst:.2e}  p(0..0)={p0:.6f} p(1..1)={plast:.6f}  "
              f"gpu={tg:.2f}s  (2^{n} amplitudes, beyond qsv 12q)")

    print("GPU-SCALE " + ("PASSED — real cuQuantum offload proven" if ok else "FAILED"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
