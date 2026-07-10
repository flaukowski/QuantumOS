#!/usr/bin/env python3
"""QuantumOS host QPU gateway (epic #149 B2, quantum-stack Phase 2).

The host half of the quantum stack: it terminates the same OPAQUE circuit wire
format the in-OS broker uses (qpu_circuit.h / qpu_circuit.py) and dispatches it
to a PennyLane device — `lightning.qubit` (CPU) by default, and the same call
extends to `lightning.gpu` (cuQuantum) or a real QPU (qBraid/Braket) in Phase 3
by swapping the device string. This is the "run PennyLane Lightning" answer:
the OS never learns which backend ran the circuit (an opaque selector), so the
kernel is future-proof against vendor/API churn.

The native tier (qsv, exact integers) and this tier (PennyLane, C++ floats) are
utterly independent implementations; the cross-oracle CI gate
(test_qsv_oracle.py) asserts they AGREE to 1e-9 on identical circuits — a stub
gateway cannot pass, and neither engine can fake the other.

Pinned deps (hard requirements): pennylane==0.45.1, pennylane-lightning==0.45.0,
NumPy>=2.0.
"""
import qpu_circuit as qc


def probs(circuit, device="lightning.qubit"):
    """Dispatch an opaque circuit to a PennyLane device; return the probability
    of every basis state (little-endian, qubit 0 = LSB — matching the exact
    engine's convention, verified by the cross-oracle)."""
    import pennylane as qml

    n, _probe, ops = qc.parse(circuit)
    dev = qml.device(device, wires=n)
    wires = list(range(n))

    def build():
        for opcode, a, b in ops:
            if opcode == qc.QC_OP_H:
                qml.Hadamard(a)
            elif opcode == qc.QC_OP_X:
                qml.PauliX(a)
            elif opcode == qc.QC_OP_Z:
                qml.PauliZ(a)
            elif opcode == qc.QC_OP_S:
                qml.S(a)
            elif opcode == qc.QC_OP_CNOT:
                qml.CNOT([a, b])
            elif opcode == qc.QC_OP_CZ:
                qml.CZ([a, b])
            elif opcode == qc.QC_OP_ORACLE:
                qml.FlipSign(a | (b << 8), wires=wires)
            elif opcode == qc.QC_OP_DIFFUSION:
                qml.GroverOperator(wires=wires)
            else:
                raise ValueError(f"unknown opcode {opcode}")

    @qml.qnode(dev)
    def circuit_fn():
        build()
        # Convention: FlipSign/GroverOperator/probs all take the SAME `wires`
        # order, which makes PennyLane self-consistent — a target integer T
        # lands at probs index T, exactly as the exact engine indexes basis T.
        # (Verified against non-palindromic Grover-4q target 11 -> index 11;
        # reversing the wires mis-maps it to 13.)
        return qml.probs(wires=wires)

    return list(circuit_fn())
