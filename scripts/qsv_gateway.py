#!/usr/bin/env python3
"""QuantumOS host QPU gateway (epic #149 B2 + #150 Phase 3).

The host half of the quantum stack: it terminates the same OPAQUE circuit wire
format the in-OS broker uses (qpu_circuit.h / qpu_circuit.py) and dispatches it
to a backend selected by an opaque device string — the OS never learns which
vendor ran a circuit, so the kernel is future-proof against API churn. Backends:

  - PennyLane (Phase 2/3): "lightning.qubit" (CPU), "lightning.gpu" (NVIDIA
    cuQuantum cuStateVec), "lightning.kokkos", "default.qubit" — statevector.
  - CUDA-Q (Phase 3 C3): "cudaq" / "cudaq:<target>" — NVIDIA's CUDA-Q; the
    local CPU target ("qpp-cpu"/default) runs free, "nvidia" uses a GPU, and
    "nvqc"/qBraid targets reach hosted H100s or real QPUs (API key).
  - qBraid (Phase 3 C2): "qbraid:<machine>" — submits to a real QPU
    (Rigetti/IonQ/IQM/QuEra) via the qBraid runtime. CREDENTIALED + metered:
    a maintainer-invoked path, never an autonomous CI gate.

The native tier (qsv, exact integers) and the PennyLane/CUDA-Q tiers (floats)
are utterly independent implementations; the cross-oracle gates assert they
AGREE to 1e-9 on identical circuits — neither can fake the other.

Pinned deps: pennylane==0.45.1, pennylane-lightning==0.45.0, NumPy>=2.0;
optional cudaq==0.15.x (C3), qbraid (C2).
"""
import qpu_circuit as qc


def probs(circuit, device="lightning.qubit"):
    """Dispatch an opaque circuit to a backend; return the probability of every
    basis state (little-endian, qubit 0 = LSB — the exact engine's convention).
    Dispatch is by device prefix so one opaque selector reaches every tier."""
    if device.startswith("cudaq"):
        return _probs_cudaq(circuit, device)
    if device.startswith("qbraid:"):
        return _probs_qbraid(circuit, device)
    return _probs_pennylane(circuit, device)


def _probs_pennylane(circuit, device):
    import pennylane as qml

    n, _probe, ops = qc.parse(circuit)
    dev = qml.device(device, wires=n)
    wires = list(range(n))
    # PennyLane indexes a basis integer/probs vector BIG-endian (wire 0 = MSB);
    # the exact engine (and the OS) is LITTLE-endian (qubit 0 = LSB). So the
    # index/integer-mapping calls — probs() and FlipSign() — take the REVERSED
    # wire order to read out little-endian. The elementary gates act on physical
    # wire labels and are unchanged. (A previous version passed `wires` to probs;
    # that was masked by symmetric Bell/GHZ and Grover's near-uniform tail but
    # gave WRONG probabilities for asymmetric circuits — e.g. H on qubit 0 of a
    # 3q register put mass at index 4 instead of 1.)
    rw = wires[::-1]

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
                qml.FlipSign(a | (b << 8), wires=rw)
            elif opcode == qc.QC_OP_DIFFUSION:
                qml.GroverOperator(wires=rw)
            else:
                raise ValueError(f"unknown opcode {opcode}")

    @qml.qnode(dev)
    def circuit_fn():
        build()
        return qml.probs(wires=rw)

    return list(circuit_fn())


def _probs_cudaq(circuit, device):
    """CUDA-Q backend (Phase 3 C3). Builds a kernel from the wire format and
    returns exact probabilities via get_state. The primitive gate set
    (H/X/Z/S/CNOT/CZ) covers Bell/GHZ; ORACLE/DIFFUSION (Grover composites) are
    not yet decomposed for CUDA-Q — a documented follow-up. device may name a
    target after a colon (e.g. "cudaq:nvidia", "cudaq:qpp-cpu")."""
    import cudaq

    target = device.split(":", 1)[1] if ":" in device else ""
    if target:
        cudaq.set_target(target)

    n, _probe, ops = qc.parse(circuit)
    kernel = cudaq.make_kernel()
    q = kernel.qalloc(n)
    for opcode, a, b in ops:
        if opcode == qc.QC_OP_H:
            kernel.h(q[a])
        elif opcode == qc.QC_OP_X:
            kernel.x(q[a])
        elif opcode == qc.QC_OP_Z:
            kernel.z(q[a])
        elif opcode == qc.QC_OP_S:
            kernel.s(q[a])
        elif opcode == qc.QC_OP_CNOT:
            kernel.cx(q[a], q[b])
        elif opcode == qc.QC_OP_CZ:
            kernel.cz(q[a], q[b])
        else:
            raise NotImplementedError(
                f"opcode {opcode} (ORACLE/DIFFUSION) not yet mapped for CUDA-Q")

    state = cudaq.get_state(kernel)
    # CUDA-Q indexes basis states big-endian (qubit 0 = MSB); reverse the bits
    # to the exact engine's little-endian so the vectors are index-comparable.
    dim = 1 << n
    out = [0.0] * dim
    for i in range(dim):
        rev = int(f"{i:0{n}b}"[::-1], 2)
        amp = state[i]
        out[rev] = float((amp.conjugate() * amp).real)
    return out


def _probs_qbraid(circuit, device):
    """qBraid backend (Phase 3 C2): submit to a REAL QPU via the qBraid runtime.
    CREDENTIALED (needs ~/.qbraid/qbraidrc) and METERED (each submission costs
    credits and queues), so this is a maintainer-invoked path — never called by
    an autonomous CI gate. Returns sampled probabilities; the caller reconciles
    the provider job id into the OS authority ledger. Left as an explicit,
    guarded stub so importing the gateway never requires qbraid to be present."""
    raise NotImplementedError(
        "qBraid real-QPU submission is a credentialed, metered, manual-trigger "
        "path — invoke scripts/qsv_qbraid_submit.py directly with credentials, "
        "not through the autonomous cross-oracle gate. device=" + device)
