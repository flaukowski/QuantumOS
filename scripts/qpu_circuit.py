#!/usr/bin/env python3
"""Host mirror of user/qpu_circuit.h — the OPAQUE circuit wire format the QPU
broker carries (epic #148/#149). One source of truth for BOTH host engines in
the cross-oracle gate: the exact integer engine (qsv_mirror) and the PennyLane
gateway (qsv_gateway) parse the SAME bytes, so "same circuit, two engines" is
literal, not approximate.

Circuit bytes (little-endian): see user/qpu_circuit.h. Header 8 bytes
(version, n_qubits, n_ops, probe u16, 3 reserved) then n_ops x {opcode, a, b}.
"""

QC_VERSION = 1
QC_HDR = 8

QC_OP_H = 1
QC_OP_X = 2
QC_OP_Z = 3
QC_OP_S = 4
QC_OP_CNOT = 5
QC_OP_CZ = 6
QC_OP_ORACLE = 7
QC_OP_DIFFUSION = 8


def build(n_qubits, ops, probe=0):
    """ops: list of (opcode, a, b). Returns the opaque circuit bytes."""
    assert 1 <= n_qubits <= 12
    assert 0 <= probe < (1 << n_qubits)
    assert len(ops) <= 255
    b = bytearray(QC_HDR)
    b[0] = QC_VERSION
    b[1] = n_qubits
    b[2] = len(ops)
    b[3] = probe & 0xFF
    b[4] = (probe >> 8) & 0xFF
    for opcode, a, aa in ops:
        b += bytes((opcode & 0xFF, a & 0xFF, aa & 0xFF))
    return bytes(b)


def parse(circuit):
    """Returns (n_qubits, probe, [(opcode, a, b), ...])."""
    if len(circuit) < QC_HDR or circuit[0] != QC_VERSION:
        raise ValueError("bad circuit header")
    n = circuit[1]
    n_ops = circuit[2]
    probe = circuit[3] | (circuit[4] << 8)
    if QC_HDR + n_ops * 3 > len(circuit):
        raise ValueError("truncated ops")
    ops = [(circuit[QC_HDR + i * 3], circuit[QC_HDR + i * 3 + 1], circuit[QC_HDR + i * 3 + 2])
           for i in range(n_ops)]
    return n, probe, ops


# --- canonical circuits (identical to what user/qpu_test.c builds) ---

def bell(probe=0):
    return build(2, [(QC_OP_H, 0, 0), (QC_OP_CNOT, 0, 1)], probe)


def ghz(n, probe=0):
    ops = [(QC_OP_H, 0, 0)] + [(QC_OP_CNOT, i, i + 1) for i in range(n - 1)]
    return build(n, ops, probe)


def grover(n, target, iters):
    ops = [(QC_OP_H, q, 0) for q in range(n)]
    for _ in range(iters):
        ops.append((QC_OP_ORACLE, target & 0xFF, (target >> 8) & 0xFF))
        ops.append((QC_OP_DIFFUSION, 0, 0))
    return build(n, ops, target)


def h_s_h():
    return build(1, [(QC_OP_H, 0, 0), (QC_OP_S, 0, 0), (QC_OP_H, 0, 0)], 0)
