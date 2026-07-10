/**
 * QuantumOS qsv — EXACT integer quantum state-vector proof citizen
 * (epic #148, A1).
 *
 * The engine lives in qsv_engine.h (shared with the resident QPU executor,
 * qpud); this citizen is the one-shot BOOT PROOF: quantum computation that
 * runs air-gapped in ring 3 and is CI-verifiable to INTEGER EQUALITY. See
 * the engine header for the exact-Gaussian-integer model (implicit scale
 * 2^(-h/2), zero rounding ever, unitarity as the integer identity
 * sum |v|^2 == 2^h).
 *
 * Every boot: Bell (2q), GHZ (3q), Grover-3q (2 iters, p = 121/128 exactly),
 * Grover-4q (3 iters, amp = -16064 at h=28, p = 63001/65536 exactly), then a
 * sha256 of the final Grover-4q state that CI re-derives with an independent
 * host-side Python mirror (scripts/qsv_mirror.py) — two implementations must
 * agree bit-for-bit, and a deliberately corrupted mirror circuit must NOT
 * match (anti-vacuous in both directions).
 *
 * qsv holds ZERO capabilities (its authority is the null set) and is not
 * monitored — a one-shot proof citizen like ghost_test.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"
#include "qsv_engine.h"
#include "sha256.h"

static int broken;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("QSV BROKEN: %s\n", what);
        broken = 1;
    }
}

void _start(void) {
    broken = 0;

    /* Bell: H(0), CNOT(0,1) -> (|00>+|11>)/sqrt2; p(00)=p(11)=1/2 exact. */
    qsv_reset(2);
    qsv_gate_h(2, 0);
    qsv_gate_cnot(2, 0, 1);
    check(qsv_norm_ok(2), "bell norm != 2^h");
    check(qsv_re[0] == 1 && qsv_im[0] == 0 && qsv_re[3] == 1 && qsv_im[3] == 0, "bell amplitudes");
    check(qsv_h == 1, "bell h");
    /* CZ sanity on the Bell state: flips ONLY the |11> sign, exactly. */
    qsv_gate_cz(2, 0, 1);
    check(qsv_re[0] == 1 && qsv_re[3] == -1 && qsv_norm_ok(2), "cz sign flip on |11>");
    printf("QSV: bell h=%u p00=1/2 p11=1/2 norm=OK\n", qsv_h);

    /* GHZ 3q: H(0), CNOT(0,1), CNOT(1,2); p(000)=p(111)=1/2 exact. */
    qsv_reset(3);
    qsv_gate_h(3, 0);
    qsv_gate_cnot(3, 0, 1);
    qsv_gate_cnot(3, 1, 2);
    check(qsv_norm_ok(3), "ghz norm != 2^h");
    check(qsv_re[0] == 1 && qsv_re[7] == 1 && qsv_h == 1, "ghz amplitudes");
    printf("QSV: ghz h=%u p000=1/2 p111=1/2 norm=OK\n", qsv_h);

    /* S-gate exactness: H(0) then S(0) on 1q -> amp(|1>) = i exactly. */
    qsv_reset(1);
    qsv_gate_h(1, 0);
    qsv_gate_s(1, 0);
    check(qsv_norm_ok(1) && qsv_re[1] == 0 && qsv_im[1] == 1, "s-gate |+> -> (1,i)/sqrt2");
    /* Z sanity on the same state: flips |1> sign exactly. */
    qsv_gate_z(1, 0);
    check(qsv_re[1] == 0 && qsv_im[1] == -1, "z-gate sign flip");

    /* Grover 3q: target |101>=5, 2 iterations -> p = 121/128 EXACTLY
     * (amp 176 at h=15). The textbook success probability, as an integer
     * identity rather than a float approximation. */
    qsv_reset(3);
    for (int q = 0; q < 3; q++) {
        qsv_gate_h(3, q);
    }
    for (int it = 0; it < 2; it++) {
        qsv_oracle(3, 5);
        qsv_diffusion(3);
    }
    check(qsv_norm_ok(3), "grover3 norm != 2^h");
    check(qsv_re[5] == 176 && qsv_im[5] == 0 && qsv_h == 15, "grover3 amp != 176 @ h=15");
    {
        uint32_t num, den;
        qsv_reduced_prob(qsv_re[5], &num, &den);
        check(num == 121 && den == 128, "grover3 p != 121/128");
        printf("QSV: grover3 h=%u amp=%d p=%u/%u norm=OK\n", qsv_h, qsv_re[5], num, den);
    }

    /* Grover 4q: target |1011>=11, 3 iterations -> amp = -16064 at h=28,
     * p = 63001/65536 exactly (~0.9613; the sign is the MCZ-diffusion
     * global phase — physical, and pinned by the digest cross-check). */
    qsv_reset(4);
    for (int q = 0; q < 4; q++) {
        qsv_gate_h(4, q);
    }
    for (int it = 0; it < 3; it++) {
        qsv_oracle(4, 11);
        qsv_diffusion(4);
    }
    check(qsv_norm_ok(4), "grover4 norm != 2^h");
    check(qsv_re[11] == -16064 && qsv_im[11] == 0 && qsv_h == 28, "grover4 amp != -16064 @ h=28");
    {
        uint32_t num, den;
        qsv_reduced_prob(qsv_re[11], &num, &den);
        check(num == 63001 && den == 65536, "grover4 p != 63001/65536");
        printf("QSV: grover4 h=%u amp=%d p=%u/%u norm=OK\n", qsv_h, qsv_re[11], num, den);
    }

    /* Digest the final Grover-4q state (16 x (re,im) as little-endian
     * int32 = 128 bytes) — CI re-derives this with the independent Python
     * mirror; equality proves two implementations of the exact engine
     * agree bit-for-bit, and the mirror's --corrupt digest must differ. */
    {
        uint8_t buf[16 * 8];
        for (int k = 0; k < 16; k++) {
            uint32_t r = (uint32_t)qsv_re[k], i = (uint32_t)qsv_im[k];
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
