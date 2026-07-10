/**
 * QuantumOS qsv engine — EXACT integer quantum state vector (epic #148).
 *
 * Header-only (the sha256.h/ghost.h idiom: each includer gets its own copy,
 * including the ~32 KB of .bss state) so the one-shot proof citizen (qsv.c)
 * and the resident QPU executor (qpud.c) run the IDENTICAL engine. The
 * ci-smoke digest gate pins this: qsv's sha256 of the final Grover-4q state
 * must keep matching the independent host mirror after any refactor here.
 *
 * The model: amplitudes are exact Gaussian integers (int32 re, int32 im)
 * carrying ONE implicit global scale 2^(-qsv_h/2), where qsv_h counts
 * Hadamard applications. H on a pair is (a,b) -> (a+b, a-b) — pure integer,
 * ZERO rounding ever (no rounded binary fixed point can be exact: no
 * nontrivial 2x2 real unitary has dyadic entries). X/Z/CNOT/CZ are
 * permutations/sign flips; S multiplies the |1> half by i (exact in Z[i]);
 * T is excluded from v1 (irrational over Z[i]). Probabilities are exact
 * rationals |v|^2 / 2^qsv_h; unitarity is the integer identity
 * sum |v|^2 == 2^qsv_h, checked in unsigned __int128.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef QSV_ENGINE_H
#define QSV_ENGINE_H

#include "libq/libq.h"

#define QSV_MAX_QUBITS 12
#define QSV_DIM (1 << QSV_MAX_QUBITS)

static int32_t qsv_re[QSV_DIM];
static int32_t qsv_im[QSV_DIM];
static uint32_t qsv_h; /* running Hadamard count == -log2 of the scale^2 */
/* Sticky overflow flag: the H butterfly (a+b, a-b) can push an int32 amplitude
 * past +/-2^31 on a deep enough circuit (host-supplied circuits are opaque and
 * unbounded in gate count). Signed int32 overflow is UB, so the butterfly does
 * its arithmetic in int64 and sets this flag on any out-of-range result; the
 * caller (qpud) rejects the circuit rather than trusting a wrapped amplitude
 * (qsv_norm_ok runs AFTER the arithmetic and would miss the boundary). */
static int qsv_overflow;

static inline void qsv_reset(int n) {
    for (int k = 0; k < (1 << n); k++) {
        qsv_re[k] = 0;
        qsv_im[k] = 0;
    }
    qsv_re[0] = 1;
    qsv_h = 0;
    qsv_overflow = 0;
}

/* H on qubit q: butterfly (a,b) -> (a+b, a-b). The ONLY gate that grows
 * amplitudes; growth is bounded by the circuit family and the norm identity
 * below catches any overflow. */
static inline void qsv_gate_h(int n, int q) {
    int step = 1 << q;
    for (int base = 0; base < (1 << n); base += step << 1) {
        for (int k = base; k < base + step; k++) {
            /* int64 intermediates avoid the UB of a signed int32 overflow; the
             * truncating cast is defined, and a value that changes under it is
             * flagged so the caller rejects the circuit. */
            int64_t ar = qsv_re[k], ai = qsv_im[k];
            int64_t br = qsv_re[k + step], bi = qsv_im[k + step];
            int64_t sr = ar + br, si = ai + bi, dr = ar - br, di = ai - bi;
            if ((int64_t)(int32_t)sr != sr || (int64_t)(int32_t)si != si ||
                (int64_t)(int32_t)dr != dr || (int64_t)(int32_t)di != di) {
                qsv_overflow = 1;
            }
            qsv_re[k] = (int32_t)sr;
            qsv_im[k] = (int32_t)si;
            qsv_re[k + step] = (int32_t)dr;
            qsv_im[k + step] = (int32_t)di;
        }
    }
    qsv_h++;
}

static inline void qsv_gate_x(int n, int q) {
    int step = 1 << q;
    for (int base = 0; base < (1 << n); base += step << 1) {
        for (int k = base; k < base + step; k++) {
            int32_t t;
            t = qsv_re[k];
            qsv_re[k] = qsv_re[k + step];
            qsv_re[k + step] = t;
            t = qsv_im[k];
            qsv_im[k] = qsv_im[k + step];
            qsv_im[k + step] = t;
        }
    }
}

static inline void qsv_gate_z(int n, int q) {
    for (int k = 0; k < (1 << n); k++) {
        if ((k >> q) & 1) {
            qsv_re[k] = -qsv_re[k];
            qsv_im[k] = -qsv_im[k];
        }
    }
}

static inline void qsv_gate_cz(int n, int a, int b) {
    for (int k = 0; k < (1 << n); k++) {
        if (((k >> a) & 1) && ((k >> b) & 1)) {
            qsv_re[k] = -qsv_re[k];
            qsv_im[k] = -qsv_im[k];
        }
    }
}

static inline void qsv_gate_cnot(int n, int c, int t) {
    for (int k = 0; k < (1 << n); k++) {
        if (((k >> c) & 1) && !((k >> t) & 1)) {
            int j = k | (1 << t);
            int32_t tmp;
            tmp = qsv_re[k];
            qsv_re[k] = qsv_re[j];
            qsv_re[j] = tmp;
            tmp = qsv_im[k];
            qsv_im[k] = qsv_im[j];
            qsv_im[j] = tmp;
        }
    }
}

/* S: |1> half multiplied by i — (re,im) -> (-im,re). Exact in Z[i]. */
static inline void qsv_gate_s(int n, int q) {
    for (int k = 0; k < (1 << n); k++) {
        if ((k >> q) & 1) {
            int32_t t = qsv_re[k];
            qsv_re[k] = -qsv_im[k];
            qsv_im[k] = t;
        }
    }
}

/* Multi-controlled Z over ALL n qubits: phase-flip |11..1>. */
static inline void qsv_mcz_all(int n) {
    int k = (1 << n) - 1;
    qsv_re[k] = -qsv_re[k];
    qsv_im[k] = -qsv_im[k];
}

/* Phase-flip one basis state: X-conjugate the all-ones MCZ. */
static inline void qsv_oracle(int n, int target) {
    for (int q = 0; q < n; q++) {
        if (!((target >> q) & 1)) {
            qsv_gate_x(n, q);
        }
    }
    qsv_mcz_all(n);
    for (int q = 0; q < n; q++) {
        if (!((target >> q) & 1)) {
            qsv_gate_x(n, q);
        }
    }
}

/* Grover diffusion as H^n X^n MCZ X^n H^n — exactly, no mean inversion. */
static inline void qsv_diffusion(int n) {
    for (int q = 0; q < n; q++) {
        qsv_gate_h(n, q);
    }
    for (int q = 0; q < n; q++) {
        qsv_gate_x(n, q);
    }
    qsv_mcz_all(n);
    for (int q = 0; q < n; q++) {
        qsv_gate_x(n, q);
    }
    for (int q = 0; q < n; q++) {
        qsv_gate_h(n, q);
    }
}

/* The unitarity identity: sum |v|^2 must equal 2^qsv_h EXACTLY. __int128:
 * 4096 amplitudes x (2^31)^2 overflows int64's 2^63. */
static inline int qsv_norm_ok(int n) {
    unsigned __int128 sum = 0;
    for (int k = 0; k < (1 << n); k++) {
        int64_t r = qsv_re[k], i = qsv_im[k];
        sum += (unsigned __int128)(r * r) + (unsigned __int128)(i * i);
    }
    return sum == ((unsigned __int128)1 << qsv_h);
}

/* Reduce v^2 / 2^qsv_h by shifting out common twos (the denominator is a
 * power of two, so the gcd is exactly 2^(trailing zeros of v^2), and both
 * stay well inside uint32 for the boot circuits). */
static inline void qsv_reduced_prob(int32_t v, uint32_t *num_out, uint32_t *den_out) {
    uint32_t num = (uint32_t)(v < 0 ? -v : v);
    num = num * num;
    uint32_t hh = qsv_h;
    while (num != 0 && (num & 1) == 0 && hh > 0) {
        num >>= 1;
        hh--;
    }
    *num_out = num;
    *den_out = (uint32_t)1 << hh;
}

#endif /* QSV_ENGINE_H */
