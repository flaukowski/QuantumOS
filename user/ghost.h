/**
 * QuantumOS ghostd — shared field/protocol definitions (ghostOS phase 1).
 *
 * A Hopfield–Kuramoto associative memory living in a ring-3 service.
 * The field is N=256 phase oscillators; patterns are binary vectors
 * ξ ∈ {0, π}^256 (a 256-bit word each). RECALL is attractor relaxation,
 * not a similarity lookup: the field is initialised at a corrupted probe
 * and relaxes under the imprinted coupling until it lands in the nearest
 * stored basin.
 *
 * ALL arithmetic here is integer / fixed-point — no floats, no doubles,
 * anywhere. Phase is a uint32 in "turns" (a full circle is 2^32, so
 * wraparound is free). Trig comes from a 256-entry Q15 table indexed by
 * the top 8 phase bits (see ghostd.c).
 *
 * This header carries only what both the service (ghostd) and the boot
 * self-test client (ghost_test) must agree on: the wire protocol and the
 * deterministic pattern generator used by the merge gate.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef GHOST_H
#define GHOST_H

#include <stdint.h>     /* freestanding: fixed-width types for the field */
#include "usys.h"

/* Field geometry */
#define GHOST_N        256              /* oscillators */
#define GHOST_M        16               /* pattern slots */
#define GHOST_PW       (GHOST_N / 32)   /* 8 words per 256-bit pattern */

/* IPC opcodes (request .op) */
#define GHOST_REMEMBER 1                /* imprint bits into slot */
#define GHOST_RECALL   2                /* relax from probe, identify basin */
#define GHOST_STATUS   3                /* report R, live count, lambda */

/* Reply .match sentinels (>= 0 is the matched slot index) */
#define GHOST_NOMATCH  (-1)            /* relaxation converged to nothing stored */
#define GHOST_FIDELITY (-2)            /* basin found but its fidelity is spent */

/* Wire messages. A pattern is 32 bytes, so a whole request is 36 bytes —
 * far below IPC_MAX_MESSAGE_SIZE (4096), hence no chunking is needed at
 * phase 1. Sent verbatim via send_msg()/recv_msg() (binary-safe: the
 * kernel copies exactly .length bytes). */
typedef struct {
    uint8_t  op;                        /* GHOST_REMEMBER/RECALL/STATUS */
    uint8_t  slot;                      /* REMEMBER: target slot */
    uint8_t  pad[2];
    uint32_t bits[GHOST_PW];            /* 256-bit pattern (REMEMBER/RECALL) */
} ghost_req_t;

typedef struct {
    uint8_t  op;                        /* echoes the request op */
    int8_t   match;                     /* slot index, or GHOST_NOMATCH/FIDELITY */
    uint8_t  live;                      /* live pattern count */
    uint8_t  pad;
    uint32_t r_q16;                     /* order parameter R, Q16 fraction */
    uint32_t lambda_q16;                /* current lambda (STATUS), Q16 */
} ghost_rep_t;

/* Bit i of a 256-bit pattern: 1 => phase π (spin -1), 0 => phase 0 (+1) */
static inline int ghost_bit(const uint32_t *pat, int i) {
    return (pat[i >> 5] >> (i & 31)) & 1;
}
static inline void ghost_setbit(uint32_t *pat, int i, int b) {
    if (b) pat[i >> 5] |= (1u << (i & 31));
    else   pat[i >> 5] &= ~(1u << (i & 31));
}

/* Deterministic pattern from a seed (numerical-recipes LCG). The gate
 * uses fixed seeds so the whole self-test path is randomness-free. */
static inline void ghost_gen_pattern(uint32_t *pat, uint32_t seed) {
    uint32_t x = seed;
    for (int w = 0; w < GHOST_PW; w++) {
        x = x * 1664525u + 1013904223u;
        pat[w] = x;
    }
}

/* The three well-separated seeds the merge gate imprints and recalls. */
#define GHOST_SEED_0   0xC0FFEE01u
#define GHOST_SEED_1   0xBADF00D2u
#define GHOST_SEED_2   0x51A7ED03u

/* ------------------------------------------------------------------ *
 * Tiny freestanding string builders (no libc). Both programs format
 * log/gate lines the same way — R is printed as an integer 0.xx.
 * ------------------------------------------------------------------ */

/* R (Q16 fraction) -> hundredths in [0,100] */
static inline int ghost_r_hundredths(uint32_t r_q16) {
    unsigned h = (unsigned)(((uint64_t)r_q16 * 100u) >> 16);
    return h > 100 ? 100 : (int)h;
}
static inline int ghost_put(char *b, int o, const char *s) {
    while (*s) b[o++] = *s++;
    return o;
}
static inline int ghost_put_u(char *b, int o, unsigned v) {
    char t[10];
    int n = 0;
    if (v == 0) { b[o++] = '0'; return o; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) b[o++] = t[--n];
    return o;
}
/* Append R as "0.xx" (or "1.00" at full coherence) */
static inline int ghost_put_r(char *b, int o, uint32_t r_q16) {
    int h = ghost_r_hundredths(r_q16);
    if (h >= 100) return ghost_put(b, o, "1.00");
    b[o++] = '0';
    b[o++] = '.';
    b[o++] = (char)('0' + h / 10);
    b[o++] = (char)('0' + h % 10);
    return o;
}

#endif /* GHOST_H */
