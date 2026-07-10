#!/usr/bin/env python3
"""Cross-oracle CI gate (epic #149 B2): the EXACT integer engine (qsv_mirror)
vs the PennyLane gateway (qsv_gateway, lightning.qubit) on IDENTICAL opaque
circuits. Two utterly independent implementations — freestanding exact Gaussian
integers vs Xanadu's C++ float state vector — must agree on every basis-state
probability to 1e-9.

Anti-vacuous in BOTH directions:
  - AGREEMENT: a stub gateway that echoes its input, or a broken translation,
    cannot match the exact engine to 1e-9.
  - DISAGREEMENT: a deliberately DIFFERENT circuit (oracle aimed at the wrong
    state) MUST fail the 1e-9 check — proving the comparison has teeth and is
    not passing vacuously (e.g. both returning uniform distributions).

Hermetic: pinned pennylane==0.45.1 / pennylane-lightning==0.45.0 / numpy>=2.
Run from scripts/ (or with scripts/ on sys.path).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import qpu_circuit as qc
import qsv_mirror
import qsv_gateway

TOL = 1e-9


def _worst(circuit, device="lightning.qubit"):
    _n, _probe, exact = qsv_mirror.run_bytes(circuit)
    exact_f = [float(p) for p in exact]
    pl = qsv_gateway.probs(circuit, device)
    if len(pl) != len(exact_f):
        raise AssertionError(f"length mismatch: exact {len(exact_f)} vs gateway {len(pl)}")
    return max(abs(a - b) for a, b in zip(exact_f, pl))


def main():
    device = sys.argv[1] if len(sys.argv) > 1 else "lightning.qubit"
    cases = [
        ("bell", qc.bell()),
        ("ghz3", qc.ghz(3)),
        ("ghz6", qc.ghz(6)),
        ("h_s_h", qc.h_s_h()),
        ("grover3(t=5,2it)", qc.grover(3, 5, 2)),
        ("grover4(t=11,3it)", qc.grover(4, 11, 3)),
        # ASYMMETRIC circuits — REQUIRED to catch qubit-ordering/endianness bugs
        # that symmetric Bell/GHZ and Grover's near-uniform tail hide (a big-
        # endian gateway readout put H(0)-3q's mass at index 4 instead of 1).
        ("H(0)-3q asym", qc.build(3, [(qc.QC_OP_H, 0, 0)], 0)),
        ("H(1)-3q asym", qc.build(3, [(qc.QC_OP_H, 1, 0)], 0)),
        ("H0-X2-3q asym", qc.build(3, [(qc.QC_OP_H, 0, 0), (qc.QC_OP_X, 2, 0)], 0)),
    ]
    ok = True
    for name, circ in cases:
        worst = _worst(circ, device)
        status = "OK  " if worst <= TOL else "FAIL"
        if worst > TOL:
            ok = False
        print(f"{status}: {name:20s} worst|delta|={worst:.2e}  (exact vs {device})")

    # Teeth check: the exact engine's grover4(target=11) vs the GATEWAY running
    # grover4(target=6) must DISAGREE grossly, or the comparison is vacuous.
    _n, _p, exact11 = qsv_mirror.run_bytes(qc.grover(4, 11, 3))
    exact11_f = [float(p) for p in exact11]
    pl6 = qsv_gateway.probs(qc.grover(4, 6, 3), device)
    mismatch = max(abs(a - b) for a, b in zip(exact11_f, pl6))
    if mismatch <= 0.1:
        print(f"FAIL: teeth check — wrong-circuit worst={mismatch:.2e} (comparison is vacuous)")
        ok = False
    else:
        print(f"OK  : teeth check — wrong circuit disagrees (worst={mismatch:.2e} > 0.1)")

    print("CROSS-ORACLE " + ("PASSED" if ok else "FAILED"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
