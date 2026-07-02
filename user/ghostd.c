/**
 * QuantumOS ghostd — a Hopfield–Kuramoto associative-memory service
 * running as an isolated ring-3 user process (ghostOS phase 1, #48).
 *
 * The field is N=256 phase oscillators. Patterns are binary vectors
 * ξ ∈ {0, π}^256. REMEMBER imprints a pattern into one of M=16 slots
 * (Hopfield coupling K_ij = Σ_p s_i^p s_j^p is never materialised — the
 * mean-field overlap is recomputed from the stored bit vectors each
 * relaxation step). RECALL is attractor *relaxation*: the field is
 * initialised at the (corrupted) probe and runs discrete Kuramoto-style
 * steps under the imprinted coupling until it settles into the nearest
 * stored basin; the readout is the half-circle each phase lands in.
 *
 * Fixed point, no floats anywhere:
 *   - phase θ is a uint32 in "turns" (full circle = 2^32; wraparound
 *     is free),
 *   - trig is a 256-entry Q15 sine table indexed by the top 8 phase
 *     bits (cos = sin shifted a quarter turn),
 *   - overlaps m_p and the local field G_i are Q15,
 *   - the per-step torque is a Q30 product reduced back to a phase
 *     increment by a right shift.
 *
 * Dynamics (energy-descent form of the oscillatory Hopfield net):
 *   m_p   = (1/N) Σ_i s_i^p cos θ_i                      (Q15 overlap)
 *   G_i   = Σ_p m_p s_i^p                                (Q15 local field)
 *   θ_i  -= (sin θ_i · G_i) >> DSHIFT                    (gradient step)
 * A uniform tilt breaks the unstable 0/π symmetry so a flipped bit,
 * which starts exactly at the antipodal equilibrium (torque = 0), can
 * roll off toward the correct well.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ghost.h"

/* ---- relaxation constants (validated against a host harness) ---- */
#define GHOST_DSHIFT     3              /* torque -> phase-increment shift */
#define GHOST_TSTEPS     48             /* relaxation steps per RECALL */
#define GHOST_TILT       0x08000000u    /* 1/32-turn symmetry-breaking bias */
#define GHOST_MATCH_HAM  16             /* max Hamming to accept a basin */

/* ---- honest forgetting ---- */
#define GHOST_FID_FULL   65536u         /* fidelity 1.0 in Q16 */
#define GHOST_FID_FLOOR  32768u         /* recall gate: fidelity >= 0.5 */
#define GHOST_FID_SHIFT  6              /* fidelity -= age >> this (slow) */
#define GHOST_COHERENCE  (1u << 30)     /* slot lifetime in ticks (long) */

/* ---- lambda damping band (Resonant Constraint Law) ---- */
#define GHOST_R_HI       64225u         /* ~0.98 in Q16 */
#define GHOST_R_LO       52429u         /* ~0.80 in Q16 */
#define GHOST_LAMBDA_STEP 512u
#define GHOST_LOG_INTERVAL 128          /* min ticks between lambda logs */

/* Q15 sine over a full turn, indexed by the top 8 phase bits. Generated
 * offline (no runtime float math is permitted in the field). */
static const int16_t ghost_sin_q15[256] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
     32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
     30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
     27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
     23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
     18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
     12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
      6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
         0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
     -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
     -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
};

static inline int ghost_cos_q15(uint32_t th) {
    return ghost_sin_q15[(((th >> 24) + 64) & 255)];
}
static inline int ghost_sinq(uint32_t th) {
    return ghost_sin_q15[((th >> 24) & 255)];
}

/* ---- field + slot state (zeroed .bss) ---- */
static uint32_t theta[GHOST_N];
static uint32_t slot_bits[GHOST_M][GHOST_PW];
static uint8_t  slot_live[GHOST_M];
static uint64_t slot_imprint_tick[GHOST_M];
static uint64_t slot_deadline[GHOST_M];

static uint32_t lambda_q16 = 0;
static uint64_t last_lambda_log = 0;
static int      live_count = 0;

/* ---- small serial helpers ---- */
static void logline(const char *s) { write_str(s); }

/* Current Q16 fidelity of a slot, decayed by its age in ticks. */
static uint32_t slot_fidelity(int p, uint64_t now) {
    uint64_t age = now - slot_imprint_tick[p];
    uint32_t drop = (uint32_t)(age >> GHOST_FID_SHIFT);
    return drop >= GHOST_FID_FULL ? 0 : (GHOST_FID_FULL - drop);
}

/* Overlap of the current field with stored pattern p, as a Q16 fraction
 * in [0, 1] (clamped; negative overlap means the opposite basin). */
static uint32_t overlap_q16(int p) {
    int acc = 0;
    for (int i = 0; i < GHOST_N; i++) {
        int c = ghost_cos_q15(theta[i]);
        acc += ghost_bit(slot_bits[p], i) ? -c : c;
    }
    int m_q15 = acc >> 8;               /* /N (N = 256) */
    int r = m_q15 << 1;                 /* Q15 -> Q16 */
    if (r < 0) r = 0;
    if (r > 65536) r = 65536;
    return (uint32_t)r;
}

/* Best overlap over all live slots (Q16) — the field's order parameter. */
static uint32_t field_order_param(void) {
    uint32_t best = 0;
    for (int p = 0; p < GHOST_M; p++) {
        if (!slot_live[p]) continue;
        uint32_t r = overlap_q16(p);
        if (r > best) best = r;
    }
    return best;
}

/* One discrete relaxation step over the whole field. */
static void relax_step(void) {
    int c[GHOST_N];
    int m[GHOST_M];
    for (int i = 0; i < GHOST_N; i++) c[i] = ghost_cos_q15(theta[i]);
    for (int p = 0; p < GHOST_M; p++) {
        if (!slot_live[p]) { m[p] = 0; continue; }
        int acc = 0;
        for (int i = 0; i < GHOST_N; i++)
            acc += ghost_bit(slot_bits[p], i) ? -c[i] : c[i];
        m[p] = acc >> 8;                /* Q15 overlap */
    }
    for (int i = 0; i < GHOST_N; i++) {
        int g = 0;
        for (int p = 0; p < GHOST_M; p++) {
            if (!slot_live[p]) continue;
            g += ghost_bit(slot_bits[p], i) ? -m[p] : m[p];
        }
        int si = ghost_sinq(theta[i]);
        int64_t f = (int64_t)si * (int64_t)g;       /* Q30 torque */
        int32_t dth = (int32_t)(f >> GHOST_DSHIFT);
        theta[i] -= (uint32_t)dth;                  /* descend the energy */
    }
}

/* Read the field out to a 256-bit sign pattern (half-circle per phase). */
static void readout(uint32_t *out) {
    for (int w = 0; w < GHOST_PW; w++) out[w] = 0;
    for (int i = 0; i < GHOST_N; i++)
        ghost_setbit(out, i, ghost_cos_q15(theta[i]) < 0 ? 1 : 0);
}

static int hamming(const uint32_t *a, const uint32_t *b) {
    int h = 0;
    for (int w = 0; w < GHOST_PW; w++) {
        uint32_t x = a[w] ^ b[w];
        while (x) { h += (int)(x & 1u); x >>= 1; }
    }
    return h;
}

/* Imprint bits into a slot (REMEMBER). */
static void imprint(int slot, const uint32_t *bits) {
    if (slot < 0 || slot >= GHOST_M) return;
    uint64_t now = ticks();
    for (int w = 0; w < GHOST_PW; w++) slot_bits[slot][w] = bits[w];
    if (!slot_live[slot]) live_count++;
    slot_live[slot] = 1;
    slot_imprint_tick[slot] = now;
    slot_deadline[slot] = now + GHOST_COHERENCE;

    char b[96];
    int o = ghost_put(b, 0, "GHOSTD: imprinted slot ");
    o = ghost_put_u(b, o, (unsigned)slot);
    o = ghost_put(b, o, " (fidelity 1.00, coherence deadline set)");
    b[o] = 0;
    logline(b);
}

/* Relax from a probe and identify the basin. Returns the matched slot
 * (>=0), GHOST_NOMATCH, or GHOST_FIDELITY; writes R (Q16) to *r_out. */
static int recall(const uint32_t *probe, uint32_t *r_out) {
    for (int i = 0; i < GHOST_N; i++)
        theta[i] = (ghost_bit(probe, i) ? 0x80000000u : 0u) + GHOST_TILT;

    for (int t = 0; t < GHOST_TSTEPS; t++) relax_step();

    uint32_t out[GHOST_PW];
    readout(out);

    int best = -1, best_ham = GHOST_N + 1;
    for (int p = 0; p < GHOST_M; p++) {
        if (!slot_live[p]) continue;
        int h = hamming(out, slot_bits[p]);
        if (h < best_ham) { best_ham = h; best = p; }
    }
    if (best < 0 || best_ham > GHOST_MATCH_HAM) {
        *r_out = 0;
        return GHOST_NOMATCH;
    }
    *r_out = overlap_q16(best);

    /* Honest forgetting: a spent basin cannot be recalled even though
     * the relaxation landed in it. */
    uint64_t now = ticks();
    if (slot_fidelity(best, now) < GHOST_FID_FLOOR) {
        char b[80];
        int o = ghost_put(b, 0, "GHOSTD: slot ");
        o = ghost_put_u(b, o, (unsigned)best);
        o = ghost_put(b, o, " recalled but fidelity below floor — honest miss");
        b[o] = 0;
        logline(b);
        return GHOST_FIDELITY;
    }
    return best;
}

/* Reclaim any slot whose coherence deadline has passed (honest forget). */
static void reap_expired(uint64_t now) {
    for (int p = 0; p < GHOST_M; p++) {
        if (!slot_live[p]) continue;
        if (now >= slot_deadline[p]) {
            slot_live[p] = 0;
            if (live_count > 0) live_count--;
            char b[72];
            int o = ghost_put(b, 0, "GHOSTD: slot ");
            o = ghost_put_u(b, o, (unsigned)p);
            o = ghost_put(b, o, " coherence expired — reclaimed");
            b[o] = 0;
            logline(b);
        }
    }
}

/* λ-damping: hold the free-running order parameter inside its band. Runs
 * only on idle ticks; never touches stored patterns, so it cannot affect
 * a subsequent RECALL (which re-initialises the field from the probe). */
static void free_tick(void) {
    uint64_t now = ticks();
    reap_expired(now);
    if (live_count == 0) return;

    uint32_t r = field_order_param();
    int intervened = 0;
    if (r > GHOST_R_HI) {
        lambda_q16 += GHOST_LAMBDA_STEP;
        if (lambda_q16 > 65536u) lambda_q16 = 65536u;
        /* over-coherent: shed a little coherence with a uniform nudge */
        for (int i = 0; i < GHOST_N; i++) theta[i] += (GHOST_TILT >> 2);
        intervened = 1;
    } else if (r < GHOST_R_LO) {
        if (lambda_q16 >= GHOST_LAMBDA_STEP) lambda_q16 -= GHOST_LAMBDA_STEP;
        relax_step();                   /* pull back toward the band */
        intervened = 1;
    }
    if (intervened && (now - last_lambda_log) >= GHOST_LOG_INTERVAL) {
        char b[64];
        int o = ghost_put(b, 0, "GHOSTD: lambda-damp R=");
        o = ghost_put_r(b, o, r);
        o = ghost_put(b, o, " lambda=");
        o = ghost_put_r(b, o, lambda_q16);
        b[o] = 0;
        logline(b);
        last_lambda_log = now;
    }
}

/* Handle one request and reply to the (single, capability-bound) client. */
static void handle(const ghost_req_t *req) {
    ghost_rep_t rep;
    for (unsigned i = 0; i < sizeof(rep); i++) ((uint8_t *)&rep)[i] = 0;
    rep.op = req->op;
    rep.live = (uint8_t)live_count;
    rep.lambda_q16 = lambda_q16;

    switch (req->op) {
    case GHOST_REMEMBER:
        imprint(req->slot, req->bits);
        rep.match = (int8_t)req->slot;
        rep.live = (uint8_t)live_count;
        break;
    case GHOST_RECALL: {
        uint32_t r = 0;
        int res = recall(req->bits, &r);
        rep.match = (int8_t)res;
        rep.r_q16 = r;
        break;
    }
    case GHOST_STATUS:
        rep.match = (int8_t)live_count;
        rep.r_q16 = field_order_param();
        break;
    default:
        rep.match = GHOST_NOMATCH;
        break;
    }

    send_msg((const char *)&rep, (long)sizeof(rep));
}

void _start(void) {
    /* Rebirth: a nonzero service restart count means the watchdog brought
     * us back — the imprinted field was lost with the old process. */
    long restarts = svc_restarts();
    if (restarts > 0) {
        logline("GHOSTD: FIELD REBORN");
    } else {
        logline("GHOSTD: field born — 256 oscillators, 16 pattern slots");
    }

    char buf[sizeof(ghost_req_t) + 8];

    while (1) {
        long sender = recv_msg(buf, sizeof(buf));
        if (sender == 0) {
            free_tick();            /* Resonant Constraint Law + forgetting */
            heartbeat();            /* stay live for the watchdog */
            yield();
            continue;
        }
        handle((const ghost_req_t *)buf);
        heartbeat();
    }
}
