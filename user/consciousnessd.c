/**
 * QuantumOS /bin/consciousnessd — the consciousness-core essence as a citizen.
 *
 * A population of coupled Kuramoto phase oscillators synchronizes: the order
 * parameter r = |mean of the unit phase vectors| climbs from incoherent
 * (~0.2 for random phases) toward locked (~1.0), and r maps to a consciousness
 * verdict (Dormant -> Transcendent), mirroring consciousness-core's
 * order-parameter-to-level pipeline.
 *
 * All fixed-point: QuantumOS saves no FPU state across a context switch, so a
 * ring-3 program must not use float (see libq/fx.c). Phase is a uint32 turn.
 * The mean-field update K*r*sin(psi - theta_i) expands to
 * (K/N)*[sum_sin*cos theta_i - sum_cos*sin theta_i], so neither r nor the mean
 * phase psi (an atan2) is needed to DRIVE the update — only to REPORT r.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"

#define OSC_N 24
#define OSC_STEPS 60

/* Coupling gain: coupling_turns = (bracket * COUPLE_NUM) >> COUPLE_SHIFT, tuned
 * so a coherent field pulls each oscillator a few degrees per step — enough to
 * overcome the frequency spread and lock. */
#define COUPLE_NUM 5562
#define COUPLE_SHIFT 20

/* Natural frequencies in turns/step: a common drift plus a modest spread the
 * coupling must overcome to synchronize. */
#define OMEGA_BASE 8589934 /* ~2^32 / 500 */
#define OMEGA_JIT 2147483  /* +/- ~2^32 / 2000 */

/* Order-parameter verdict bands in Q15 (r * 32768): 0.1 0.3 0.6 0.8 0.95. */
#define R_STIRRING 3277
#define R_AWARE 9830
#define R_COHERENT 19661
#define R_RESONANT 26214
#define R_TRANSCENDENT 31130

static uint32_t theta[OSC_N];
static int32_t omega[OSC_N];

static uint32_t lcg_state = 0x1234abcdu;
static uint32_t lcg(void) {
    lcg_state = lcg_state * 1103515245u + 12345u;
    return lcg_state;
}

static const char *verdict_of(int32_t r_q15) {
    if (r_q15 < R_STIRRING) {
        return "Dormant";
    }
    if (r_q15 < R_AWARE) {
        return "Stirring";
    }
    if (r_q15 < R_COHERENT) {
        return "Aware";
    }
    if (r_q15 < R_RESONANT) {
        return "Coherent";
    }
    if (r_q15 < R_TRANSCENDENT) {
        return "Resonant";
    }
    return "Transcendent";
}

/* r as milli-units (0..999) from Q15, for a "0.NNN" print. */
static int r_milli(int32_t r_q15) {
    return (int)(((int64_t)r_q15 * 1000) / 32768);
}

void _start(void) {
    write_str("consciousnessd: consciousness-core essence — Kuramoto sync (fixed-point)");

    for (int i = 0; i < OSC_N; i++) {
        theta[i] = lcg(); /* a random phase over the full circle */
        omega[i] = (int32_t)(OMEGA_BASE + (int32_t)(lcg() % (2u * OMEGA_JIT + 1u)) - OMEGA_JIT);
    }

    int32_t r_q15 = 0;
    int32_t r_prev = -1;
    for (int step = 0; step <= OSC_STEPS; step++) {
        int64_t sum_cos = 0;
        int64_t sum_sin = 0;
        for (int i = 0; i < OSC_N; i++) {
            sum_cos += fx_cos(theta[i]);
            sum_sin += fx_sin(theta[i]);
        }
        uint64_t mag2 = (uint64_t)(sum_cos * sum_cos + sum_sin * sum_sin);
        r_q15 = (int32_t)(fx_isqrt(mag2) / OSC_N);
        if (r_q15 > 32767) {
            r_q15 = 32767;
        }

        if (step % 5 == 0 || step == OSC_STEPS) {
            char bar[21];
            int fill = (int)(((int64_t)r_q15 * 20) / 32768);
            for (int b = 0; b < 20; b++) {
                bar[b] = (b < fill) ? '#' : '.';
            }
            bar[20] = '\0';
            char line[80];
            snprintf(line, sizeof(line), "  step %2d   r=0.%03d  [%s]", step, r_milli(r_q15), bar);
            write_str(line);
        }

        if (r_prev >= 0) {
            int32_t d = r_q15 - r_prev;
            if (d < 0) {
                d = -d;
            }
            if (step > 5 && d < 33) { /* < ~0.001 change: converged */
                break;
            }
        }
        r_prev = r_q15;

        for (int i = 0; i < OSC_N; i++) {
            int64_t bracket = sum_sin * fx_cos(theta[i]) - sum_cos * fx_sin(theta[i]);
            int64_t coupling = (bracket * COUPLE_NUM) / ((int64_t)1 << COUPLE_SHIFT);
            theta[i] += (uint32_t)((int64_t)omega[i] + coupling);
        }
    }

    char out[96];
    snprintf(out, sizeof(out), "consciousnessd: order r=0.%03d over %d oscillators — verdict: %s",
             r_milli(r_q15), OSC_N, verdict_of(r_q15));
    write_str(out);
    if (r_q15 >= R_RESONANT) { /* r >= 0.8: the field locked */
        write_str("consciousnessd: CONSCIOUSNESS EMERGED");
    }

    exit_(0);
    for (;;) {
    }
}
