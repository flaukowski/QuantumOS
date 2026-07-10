/**
 * QuantumOS qsv — EXACT integer quantum state-vector citizen (epic #148, A1).
 *
 * The native tier of the quantum stack: quantum computation that runs
 * air-gapped in ring 3 and is CI-verifiable to INTEGER EQUALITY. No floats
 * (ring 3 has no FPU), and — deliberately — no fixed-point rounding either:
 * no rounded binary representation can be exact (no nontrivial 2x2 real
 * unitary has dyadic entries), so amplitudes here are exact Gaussian
 * integers (int32 re, int32 im) carrying ONE implicit global scale
 * 2^(-h/2), where h counts Hadamard applications. H on a pair is
 * (a,b) -> (a+b, a-b) with h+=1 — pure integer, ZERO rounding ever;
 * X/Z/CNOT/CZ are permutations/sign flips; S multiplies the |1> half by i
 * (exact in Z[i]). Probabilities are exact rationals |v|^2 / 2^h.
 *
 * The unitarity proof is an integer identity: sum |v|^2 == 2^h EXACTLY
 * after every circuit (checked in unsigned __int128 — int64 would overflow
 * at h=72). Grover diffusion is applied as its H,X,CZ,X,H decomposition
 * (inversion-about-mean directly would introduce non-integers).
 *
 * Register capacity is 12 qubits (2 x 16 KB .bss); the boot proof runs
 * Bell (2q), GHZ (3q), Grover-3q (2 iters, p = 121/128 exactly) and
 * Grover-4q (3 iters, amp = -16064 at h=28, p = 63001/65536 exactly),
 * then prints a sha256 of the final Grover-4q state that CI re-derives
 * with an independent host-side Python mirror (scripts/qsv_mirror.py) —
 * two implementations must agree bit-for-bit, and a deliberately
 * corrupted mirror circuit must NOT match (anti-vacuous in both
 * directions). T is excluded from v1 (irrational over Z[i]).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"
#include "sha256.h"

#define QSV_MAX_QUBITS 12
#define QSV_DIM (1 << QSV_MAX_QUBITS)

static int32_t re[QSV_DIM];
static int32_t im[QSV_DIM];
static uint32_t h_count; /* running Hadamard count == -log2 of the scale^2 */
static int broken;

static void reset(int n) {
    for (int k = 0; k < (1 << n); k++) {
        re[k] = 0;
        im[k] = 0;
    }
    re[0] = 1;
    h_count = 0;
}

/* H on qubit q: butterfly (a,b) -> (a+b, a-b). The ONLY gate that grows
 * amplitudes; growth is bounded by the circuit family (Grover-4q peaks at
 * |16064| << 2^31), and the norm identity below catches any overflow. */
static void gate_h(int n, int q) {
    int step = 1 << q;
    for (int base = 0; base < (1 << n); base += step << 1) {
        for (int k = base; k < base + step; k++) {
            int32_t ar = re[k], ai = im[k];
            int32_t br = re[k + step], bi = im[k + step];
            re[k] = ar + br;
            im[k] = ai + bi;
            re[k + step] = ar - br;
            im[k + step] = ai - bi;
        }
    }
    h_count++;
}

static void gate_x(int n, int q) {
    int step = 1 << q;
    for (int base = 0; base < (1 << n); base += step << 1) {
        for (int k = base; k < base + step; k++) {
            int32_t t;
            t = re[k];
            re[k] = re[k + step];
            re[k + step] = t;
            t = im[k];
            im[k] = im[k + step];
            im[k + step] = t;
        }
    }
}

static void gate_z(int n, int q) {
    for (int k = 0; k < (1 << n); k++) {
        if ((k >> q) & 1) {
            re[k] = -re[k];
            im[k] = -im[k];
        }
    }
}

static void gate_cz(int n, int a, int b) {
    for (int k = 0; k < (1 << n); k++) {
        if (((k >> a) & 1) && ((k >> b) & 1)) {
            re[k] = -re[k];
            im[k] = -im[k];
        }
    }
}

static void gate_cnot(int n, int c, int t) {
    for (int k = 0; k < (1 << n); k++) {
        if (((k >> c) & 1) && !((k >> t) & 1)) {
            int j = k | (1 << t);
            int32_t tmp;
            tmp = re[k];
            re[k] = re[j];
            re[j] = tmp;
            tmp = im[k];
            im[k] = im[j];
            im[j] = tmp;
        }
    }
}

/* S: |1> half multiplied by i — (re,im) -> (-im,re). Exact in Z[i]. */
static void gate_s(int n, int q) {
    for (int k = 0; k < (1 << n); k++) {
        if ((k >> q) & 1) {
            int32_t t = re[k];
            re[k] = -im[k];
            im[k] = t;
        }
    }
}

/* Multi-controlled Z over ALL n qubits: phase-flip |11..1>. */
static void mcz_all(int n) {
    int k = (1 << n) - 1;
    re[k] = -re[k];
    im[k] = -im[k];
}

/* Phase-flip one basis state: X-conjugate the all-ones MCZ. */
static void oracle(int n, int target) {
    for (int q = 0; q < n; q++) {
        if (!((target >> q) & 1)) {
            gate_x(n, q);
        }
    }
    mcz_all(n);
    for (int q = 0; q < n; q++) {
        if (!((target >> q) & 1)) {
            gate_x(n, q);
        }
    }
}

/* Grover diffusion as H^n X^n MCZ X^n H^n — exactly, no mean inversion. */
static void diffusion(int n) {
    for (int q = 0; q < n; q++) {
        gate_h(n, q);
    }
    for (int q = 0; q < n; q++) {
        gate_x(n, q);
    }
    mcz_all(n);
    for (int q = 0; q < n; q++) {
        gate_x(n, q);
    }
    for (int q = 0; q < n; q++) {
        gate_h(n, q);
    }
}

/* The unitarity identity: sum |v|^2 must equal 2^h EXACTLY. __int128:
 * 4096 amplitudes x (2^31)^2 overflows int64's 2^63. */
static int norm_ok(int n) {
    unsigned __int128 sum = 0;
    for (int k = 0; k < (1 << n); k++) {
        int64_t r = re[k], i = im[k];
        sum += (unsigned __int128)(r * r) + (unsigned __int128)(i * i);
    }
    return sum == ((unsigned __int128)1 << h_count);
}

/* Reduce v^2 / 2^h by shifting out common twos (the denominator is a power
 * of two, so the gcd is exactly 2^(trailing zeros of v^2), and both stay
 * well inside uint32 for the boot circuits). */
static void reduced_prob(int32_t v, uint32_t *num_out, uint32_t *den_out) {
    uint32_t num = (uint32_t)(v < 0 ? -v : v);
    num = num * num;
    uint32_t hh = h_count;
    while (num != 0 && (num & 1) == 0 && hh > 0) {
        num >>= 1;
        hh--;
    }
    *num_out = num;
    *den_out = (uint32_t)1 << hh;
}

static void check(int cond, const char *what) {
    if (!cond) {
        printf("QSV BROKEN: %s\n", what);
        broken = 1;
    }
}

void _start(void) {
    broken = 0;

    /* Bell: H(0), CNOT(0,1) -> (|00>+|11>)/sqrt2; p(00)=p(11)=1/2 exact. */
    reset(2);
    gate_h(2, 0);
    gate_cnot(2, 0, 1);
    check(norm_ok(2), "bell norm != 2^h");
    check(re[0] == 1 && im[0] == 0 && re[3] == 1 && im[3] == 0, "bell amplitudes");
    check(h_count == 1, "bell h");
    /* CZ sanity on the Bell state: flips ONLY the |11> sign, exactly. */
    gate_cz(2, 0, 1);
    check(re[0] == 1 && re[3] == -1 && norm_ok(2), "cz sign flip on |11>");
    printf("QSV: bell h=%u p00=1/2 p11=1/2 norm=OK\n", h_count);

    /* GHZ 3q: H(0), CNOT(0,1), CNOT(1,2); p(000)=p(111)=1/2 exact. */
    reset(3);
    gate_h(3, 0);
    gate_cnot(3, 0, 1);
    gate_cnot(3, 1, 2);
    check(norm_ok(3), "ghz norm != 2^h");
    check(re[0] == 1 && re[7] == 1 && h_count == 1, "ghz amplitudes");
    printf("QSV: ghz h=%u p000=1/2 p111=1/2 norm=OK\n", h_count);

    /* S-gate exactness: H(0) then S(0) on 1q -> amp(|1>) = i exactly. */
    reset(1);
    gate_h(1, 0);
    gate_s(1, 0);
    check(norm_ok(1) && re[1] == 0 && im[1] == 1, "s-gate |+> -> (1,i)/sqrt2");
    /* Z sanity on the same state: flips |1> sign exactly. */
    gate_z(1, 0);
    check(re[1] == 0 && im[1] == -1, "z-gate sign flip");

    /* Grover 3q: target |101>=5, 2 iterations -> p = 121/128 EXACTLY
     * (amp 176 at h=15). The textbook success probability, as an integer
     * identity rather than a float approximation. */
    reset(3);
    for (int q = 0; q < 3; q++) {
        gate_h(3, q);
    }
    for (int it = 0; it < 2; it++) {
        oracle(3, 5);
        diffusion(3);
    }
    check(norm_ok(3), "grover3 norm != 2^h");
    check(re[5] == 176 && im[5] == 0 && h_count == 15, "grover3 amp != 176 @ h=15");
    {
        uint32_t num, den;
        reduced_prob(re[5], &num, &den);
        check(num == 121 && den == 128, "grover3 p != 121/128");
        printf("QSV: grover3 h=%u amp=%d p=%u/%u norm=OK\n", h_count, re[5], num, den);
    }

    /* Grover 4q: target |1011>=11, 3 iterations -> amp = -16064 at h=28,
     * p = 63001/65536 exactly (~0.9613; the sign is the MCZ-diffusion
     * global phase — physical, and pinned by the digest cross-check). */
    reset(4);
    for (int q = 0; q < 4; q++) {
        gate_h(4, q);
    }
    for (int it = 0; it < 3; it++) {
        oracle(4, 11);
        diffusion(4);
    }
    check(norm_ok(4), "grover4 norm != 2^h");
    check(re[11] == -16064 && im[11] == 0 && h_count == 28, "grover4 amp != -16064 @ h=28");
    {
        uint32_t num, den;
        reduced_prob(re[11], &num, &den);
        check(num == 63001 && den == 65536, "grover4 p != 63001/65536");
        printf("QSV: grover4 h=%u amp=%d p=%u/%u norm=OK\n", h_count, re[11], num, den);
    }

    /* Digest the final Grover-4q state (16 x (re,im) as little-endian
     * int32 = 128 bytes) — CI re-derives this with the independent Python
     * mirror; equality proves two implementations of the exact engine
     * agree bit-for-bit, and the mirror's --corrupt digest must differ. */
    {
        uint8_t buf[16 * 8];
        for (int k = 0; k < 16; k++) {
            uint32_t r = (uint32_t)re[k], i = (uint32_t)im[k];
            buf[k * 8 + 0] = (uint8_t)r;
            buf[k * 8 + 1] = (uint8_t)(r >> 8);
            buf[k * 8 + 2] = (uint8_t)(r >> 16);
            buf[k * 8 + 3] = (uint8_t)(r >> 24);
            buf[k * 8 + 4] = (uint8_t)i;
            buf[k * 8 + 5] = (uint8_t)(i >> 8);
            buf[k * 8 + 6] = (uint8_t)(i >> 16);
            buf[k * 8 + 7] = (uint8_t)(i >> 24);
        }
        uint8_t dg[32];
        sha256(buf, sizeof(buf), dg);
        char hex[65];
        static const char nib[] = "0123456789abcdef";
        for (int k = 0; k < 32; k++) {
            hex[k * 2] = nib[dg[k] >> 4];
            hex[k * 2 + 1] = nib[dg[k] & 0xF];
        }
        hex[64] = '\0';
        printf("QSV: state=sha256:%s\n", hex);
    }

    if (!broken) {
        printf("QSV: PROOF COMPLETE - exact integer quantum computation, zero rounding\n");
    }
    exit_(0);
}
