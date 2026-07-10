#!/usr/bin/env python3
"""Independent host-side mirror of the qsv exact integer engine (epic #148).

user/qsv.c runs Bell/GHZ/Grover in ring 3 with exact Gaussian-integer
amplitudes (implicit scale 2^(-h/2)) and prints a sha256 of the final
Grover-4q state. This mirror re-derives that digest from a from-scratch
Python implementation of the same engine: CI asserts the two digests are
EQUAL (two independent implementations must agree bit-for-bit), and that
the --corrupt variant (an oracle aimed at the wrong basis state) does NOT
match (so the comparison can never pass vacuously).

Exact by construction: Python ints, zero rounding, no floats anywhere.
Serialization matches qsv.c exactly: 16 amplitudes x (re, im) as
little-endian two's-complement int32 = 128 bytes.

Usage:
  python3 scripts/qsv_mirror.py            -> digest of the correct grover4 state
  python3 scripts/qsv_mirror.py --corrupt  -> digest of a corrupted-circuit state
  python3 scripts/qsv_mirror.py --check    -> self-check the published constants
"""
import hashlib
import struct
import sys


class QSV:
    """Exact integer state vector: amplitudes (re, im) in Z[i], scale 2^(-h/2)."""

    def __init__(self, n):
        self.n = n
        self.re = [0] * (1 << n)
        self.im = [0] * (1 << n)
        self.re[0] = 1
        self.h = 0

    def H(self, q):
        step = 1 << q
        for base in range(0, 1 << self.n, step << 1):
            for k in range(base, base + step):
                ar, ai = self.re[k], self.im[k]
                br, bi = self.re[k + step], self.im[k + step]
                self.re[k], self.im[k] = ar + br, ai + bi
                self.re[k + step], self.im[k + step] = ar - br, ai - bi
        self.h += 1

    def X(self, q):
        step = 1 << q
        for base in range(0, 1 << self.n, step << 1):
            for k in range(base, base + step):
                self.re[k], self.re[k + step] = self.re[k + step], self.re[k]
                self.im[k], self.im[k + step] = self.im[k + step], self.im[k]

    def mcz_all(self):
        k = (1 << self.n) - 1
        self.re[k], self.im[k] = -self.re[k], -self.im[k]

    def oracle(self, target):
        for q in range(self.n):
            if not (target >> q) & 1:
                self.X(q)
        self.mcz_all()
        for q in range(self.n):
            if not (target >> q) & 1:
                self.X(q)

    def diffusion(self):
        for q in range(self.n):
            self.H(q)
        for q in range(self.n):
            self.X(q)
        self.mcz_all()
        for q in range(self.n):
            self.X(q)
        for q in range(self.n):
            self.H(q)

    def norm2(self):
        return sum(r * r + i * i for r, i in zip(self.re, self.im))

    def CNOT(self, c, t):
        for k in range(1 << self.n):
            if (k >> c) & 1 and not (k >> t) & 1:
                j = k | (1 << t)
                self.re[k], self.re[j] = self.re[j], self.re[k]
                self.im[k], self.im[j] = self.im[j], self.im[k]

    def CZ(self, a, b):
        for k in range(1 << self.n):
            if (k >> a) & 1 and (k >> b) & 1:
                self.re[k], self.im[k] = -self.re[k], -self.im[k]

    def Z(self, q):
        for k in range(1 << self.n):
            if (k >> q) & 1:
                self.re[k], self.im[k] = -self.re[k], -self.im[k]

    def S(self, q):
        for k in range(1 << self.n):
            if (k >> q) & 1:
                self.re[k], self.im[k] = -self.im[k], self.re[k]

    def digest(self):
        buf = b"".join(struct.pack("<ii", self.re[k], self.im[k]) for k in range(1 << self.n))
        return hashlib.sha256(buf).hexdigest()

    def probs(self):
        """Exact probability of every basis state as a Fraction |v|^2 / 2^h."""
        from fractions import Fraction
        scale = 1 << self.h
        return [Fraction(r * r + i * i, scale) for r, i in zip(self.re, self.im)]


def run_bytes(circuit):
    """Run an opaque qpu_circuit.h circuit on the exact integer engine and
    return (n_qubits, probe, exact-probability list as Fractions). The single
    source of truth the cross-oracle checks PennyLane against."""
    import qpu_circuit as qc
    n, probe, ops = qc.parse(circuit)
    s = QSV(n)
    for opcode, a, b in ops:
        if opcode == qc.QC_OP_H:
            s.H(a)
        elif opcode == qc.QC_OP_X:
            s.X(a)
        elif opcode == qc.QC_OP_Z:
            s.Z(a)
        elif opcode == qc.QC_OP_S:
            s.S(a)
        elif opcode == qc.QC_OP_CNOT:
            s.CNOT(a, b)
        elif opcode == qc.QC_OP_CZ:
            s.CZ(a, b)
        elif opcode == qc.QC_OP_ORACLE:
            s.oracle(a | (b << 8))
        elif opcode == qc.QC_OP_DIFFUSION:
            s.diffusion()
        else:
            raise ValueError(f"unknown opcode {opcode}")
    assert s.norm2() == 1 << s.h, "unitarity broken: norm != 2^h"
    return n, probe, s.probs()


def grover4(target):
    s = QSV(4)
    for q in range(4):
        s.H(q)
    for _ in range(3):
        s.oracle(target)
        s.diffusion()
    assert s.norm2() == 1 << s.h, "unitarity broken: norm != 2^h"
    return s


def main():
    corrupt = "--corrupt" in sys.argv
    if "--check" in sys.argv:
        s = grover4(11)
        assert s.h == 28, s.h
        assert (s.re[11], s.im[11]) == (-16064, 0), (s.re[11], s.im[11])
        num = s.re[11] * s.re[11]
        den = 1 << s.h
        while num % 2 == 0 and den > 1:
            num //= 2
            den //= 2
        assert (num, den) == (63001, 65536), (num, den)
        print("qsv_mirror self-check OK (amp=-16064 h=28 p=63001/65536)")
        return
    # --corrupt aims the oracle at |0110>=6 instead of |1011>=11: a genuinely
    # different circuit whose digest must NOT match qsv.c's.
    s = grover4(6 if corrupt else 11)
    print(s.digest())


if __name__ == "__main__":
    main()
