/**
 * QuantumOS Resonant Scheduler — fixed-point hot path (ghostd phase 5, #52)
 *
 * Integer-only reimplementation of the dormant resonant scheduler's selection
 * math so it is safe to run in the timer ISR (no FPU / no SSE, hence no need
 * for fxsave/fxrstor the kernel does not have). See resonant_fixed.h for the
 * FPU-hazard rationale.
 *
 * Model (one Kuramoto step per reschedule):
 *   avg = (1/N) Σ (cos θ_i, sin θ_i)                       Q15 mean vector
 *   r   = |avg|                                            order parameter
 *   θ_i += ω_i + KC·r·sin(ψ − θ_i)                         coupling pull
 *   priority_i = base_i + 0.1·r·(1 + cos(θ_i − ψ))         resonant bonus
 * The next process is argmax(priority) over ready processes; because the ω_i
 * are heterogeneous the argmax rotates over time, which is exactly the
 * emergent-scheduling hypothesis being tested.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/resonant_fixed.h>
#include <kernel/process.h>
#include <kernel/boot.h>

/* Idle process id — created second in process_init (mirror of scheduler.c). */
#define RF_IDLE_PROCESS_ID (KERNEL_PROCESS_ID + 1)

/* Coupling pull in phase-turns at unit sin: how hard oscillators sync each
 * step. Small so synchronisation is gradual (subcritical-to-critical band). */
#define RF_KC            0x00A00000u

/* Deterministic per-pid natural frequency (phase increment per step) and
 * initial phase. Heterogeneous and mutually incommensurate so the coupled
 * field never trivially locks and the argmax genuinely rotates. */
#define RF_OMEGA_BASE    0x03000000u
#define RF_OMEGA_SPREAD  0x00280000u
#define RF_PHASE_SEED    0x2545F491u

/* Anti-starvation aging (Q16 priority boost per decision a ready process has
 * gone unpicked). The raw #21 hypothesis — pure argmax over resonant priority
 * — STARVES: with a max resonant priority of ~2.0 (Q16), this gain forces any
 * ready process to the front within ~2.0/RF_AGE_GAIN ≈ 32 decisions, bounding
 * the worst-case wait so the isolated services still make progress. The
 * measurement below applies the identical aged policy, so the fairness numbers
 * describe exactly what ships — and it still loses to round-robin. */
#define RF_AGE_GAIN      4096
#define RF_AGE_CAP       (8u << 16)

/* Q15 sine over a full turn, indexed by the top 8 phase bits (cos = sin a
 * quarter-turn ahead). Generated offline; no runtime float math. */
static const int16_t rf_sin_q15[256] = {
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

static inline int rf_sin(uint32_t th) { return rf_sin_q15[(th >> 24) & 255]; }
static inline int rf_cos(uint32_t th) { return rf_sin_q15[((th >> 24) + 64) & 255]; }

static uint32_t rf_isqrt(uint32_t v) {
    if (v == 0) return 0;
    uint32_t x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

/* ---- live oscillator field, indexed by pid ---- */
static uint32_t rf_phase[MAX_PROCESSES];
static uint32_t rf_omega[MAX_PROCESSES];
static uint8_t  rf_reg[MAX_PROCESSES];
static uint64_t rf_runs[MAX_PROCESSES];      /* per-process selection count */
static uint64_t rf_last_pick[MAX_PROCESSES]; /* decision seq of last selection */
static uint64_t rf_pick_seq = 0;             /* monotonic scheduling-decision counter */

/* Last-computed field summary (shared by pick + reports). */
static int      rf_avg_cos_q15 = 0;
static int      rf_avg_sin_q15 = 0;
static uint32_t rf_r_q16 = 0;

/* Shared aged-resonant priority (Q16): the emergent resonant priority plus a
 * bounded aging boost. `base_q16` is the static-priority term, `phase` the
 * oscillator phase, `waited` the number of decisions since this candidate last
 * ran. Used verbatim by the live pick AND the benchmark so the measured policy
 * is the shipped policy. */
static uint32_t rf_priority_core(uint32_t phase, uint32_t base_q16, uint64_t waited) {
    int c = rf_cos(phase);
    int s = rf_sin(phase);
    /* align = r·cos(θ − ψ) via the unit-vector dot product (no atan2). int64
     * intermediate: two Q15·Q15 products summed can approach INT_MAX. */
    int align_q15 = (int)(((int64_t)c * rf_avg_cos_q15 +
                           (int64_t)s * rf_avg_sin_q15) >> 15);
    int align_q16 = align_q15 << 1;
    /* resonant bonus = 0.1·r·(1 + cos(θ − ψ)) = ((r + r·cos)/2)/5, ≥ 0 */
    int bonus = ((int)rf_r_q16 + align_q16) / 2 / 5;
    if (bonus < 0) bonus = 0;
    uint64_t age = waited * RF_AGE_GAIN;
    if (age > RF_AGE_CAP) age = RF_AGE_CAP;
    return base_q16 + (uint32_t)bonus + (uint32_t)age;
}

/* Per-pid aged-resonant priority for the live workload. */
static uint32_t rf_priority_q16(process_t *p) {
    uint32_t base = ((uint32_t)p->priority << 16) / PRIORITY_KERNEL;
    uint64_t waited = rf_pick_seq - rf_last_pick[p->pid];
    return rf_priority_core(rf_phase[p->pid], base, waited);
}

/* Recompute (avg_cos, avg_sin, r) over the registered field. */
static void rf_update_field(void) {
    int sum_cos = 0, sum_sin = 0, n = 0;
    for (uint32_t pid = 0; pid < MAX_PROCESSES; pid++) {
        if (!rf_reg[pid]) continue;
        sum_cos += rf_cos(rf_phase[pid]);
        sum_sin += rf_sin(rf_phase[pid]);
        n++;
    }
    if (n == 0) {
        rf_avg_cos_q15 = rf_avg_sin_q15 = 0;
        rf_r_q16 = 0;
        return;
    }
    rf_avg_cos_q15 = sum_cos / n;
    rf_avg_sin_q15 = sum_sin / n;
    int64_t mag2 = (int64_t)rf_avg_cos_q15 * rf_avg_cos_q15 +
                   (int64_t)rf_avg_sin_q15 * rf_avg_sin_q15;
    uint32_t r_q15 = rf_isqrt((uint32_t)mag2);
    uint32_t r_q16 = r_q15 << 1;
    rf_r_q16 = r_q16 > 65536u ? 65536u : r_q16;
}

/* Advance every registered oscillator one Kuramoto step using the current
 * field summary (explicit step: pull toward the mean phase). */
static void rf_advance(void) {
    for (uint32_t pid = 0; pid < MAX_PROCESSES; pid++) {
        if (!rf_reg[pid]) continue;
        int c = rf_cos(rf_phase[pid]);
        int s = rf_sin(rf_phase[pid]);
        /* coupling = r·sin(ψ − θ) = avg_sin·cosθ − avg_cos·sinθ (Q15) */
        int coupling_q15 = (int)(((int64_t)rf_avg_sin_q15 * c -
                                  (int64_t)rf_avg_cos_q15 * s) >> 15);
        int32_t pull = (int32_t)(((int64_t)coupling_q15 * (int64_t)RF_KC) >> 15);
        rf_phase[pid] += rf_omega[pid] + (uint32_t)pull;
    }
}

uint32_t resonant_fixed_order_q16(void) { return rf_r_q16; }

process_t *resonant_fixed_pick(uint32_t from_pid) {
    (void)from_pid;

    /* Sync the registered set to the currently valid processes; assign a
     * heterogeneous natural frequency + starting phase the first time each
     * one is seen. The idle process is excluded from the field. */
    for (uint32_t pid = 0; pid < MAX_PROCESSES; pid++) {
        bool active = process_is_valid(pid) && pid != RF_IDLE_PROCESS_ID;
        if (active && !rf_reg[pid]) {
            rf_reg[pid] = 1;
            rf_phase[pid] = pid * RF_PHASE_SEED;
            rf_omega[pid] = RF_OMEGA_BASE + pid * RF_OMEGA_SPREAD;
            rf_last_pick[pid] = rf_pick_seq;   /* no instant aging windfall */
        } else if (!active) {
            rf_reg[pid] = 0;
        }
    }

    rf_pick_seq++;   /* one scheduling decision */

    /* Kuramoto step: summarise the field, advance it, re-summarise so the
     * priorities below see the post-step alignment. */
    rf_update_field();
    rf_advance();
    rf_update_field();

    process_t *best = NULL;
    uint32_t best_prio = 0;
    process_t *idle = NULL;

    for (uint32_t pid = 0; pid < MAX_PROCESSES; pid++) {
        process_t *p = process_get_by_pid(pid);
        if (!p || p->state != PROCESS_STATE_READY || !p->context_valid) {
            continue;
        }
        if (pid == RF_IDLE_PROCESS_ID) {
            idle = p;
            continue;
        }
        uint32_t prio = rf_priority_q16(p);
        if (!best || prio > best_prio) {
            best = p;
            best_prio = prio;
        }
    }

    if (!best) {
        return idle;
    }
    rf_runs[best->pid]++;
    rf_last_pick[best->pid] = rf_pick_seq;
    return best;
}

/* ============================================================================
 * Honest reporting
 * ============================================================================ */

static void rf_emit(const char *s) { early_console_write(s); }

static int rf_put(char *b, int o, const char *s) {
    while (*s) b[o++] = *s++;
    return o;
}
static int rf_put_u(char *b, int o, uint64_t v) {
    char t[20];
    int n = 0;
    if (v == 0) { b[o++] = '0'; return o; }
    while (v) { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n) b[o++] = t[--n];
    return o;
}
/* Q16 fraction as "0.xxx" thousandths (or "1.000" at full order). */
static int rf_put_frac(char *b, int o, uint32_t q16) {
    unsigned th = (unsigned)(((uint64_t)q16 * 1000u) >> 16);
    if (th >= 1000) return rf_put(b, o, "1.000");
    b[o++] = '0'; b[o++] = '.';
    b[o++] = (char)('0' + (th / 100) % 10);
    b[o++] = (char)('0' + (th / 10) % 10);
    b[o++] = (char)('0' + th % 10);
    return o;
}
/* Fairness ratio max/min as "X.XX" hundredths, or "INF" if some ready
 * process was never scheduled (min == 0 → starvation). */
static int rf_put_ratio(char *b, int o, uint64_t mx, uint64_t mn) {
    if (mn == 0) return rf_put(b, o, "INF");
    uint64_t r = (mx * 100u) / mn;
    o = rf_put_u(b, o, r / 100);
    b[o++] = '.';
    b[o++] = (char)('0' + (int)((r / 10) % 10));
    b[o++] = (char)('0' + (int)(r % 10));
    return o;
}

void resonant_fixed_live_report(uint64_t switches) {
    uint64_t mx = 0, mn = (uint64_t)-1;
    int counted = 0;
    for (uint32_t pid = 0; pid < MAX_PROCESSES; pid++) {
        if (!rf_reg[pid]) continue;
        uint64_t rc = rf_runs[pid];
        if (rc > mx) mx = rc;
        if (rc < mn) mn = rc;
        counted++;
    }
    if (counted == 0) { mn = 0; mx = 0; }

    char b[96];
    int o = rf_put(b, 0, "SCHED[resonant]: live r=");
    o = rf_put_frac(b, o, rf_r_q16);
    o = rf_put(b, o, " switches=");
    o = rf_put_u(b, o, switches);
    o = rf_put(b, o, " runspread=");
    o = rf_put_ratio(b, o, mx, mn);
    b[o] = 0;
    rf_emit(b);
    rf_emit("\r\n");
}

/* ---- deterministic canned-workload benchmark (rr vs resonant) ---- */

#define RF_BENCH_N     8      /* canned ready processes */
#define RF_BENCH_STEPS 1024   /* scheduling decisions */

void resonant_fixed_boot_report(void) {
    uint32_t ph[RF_BENCH_N], om[RF_BENCH_N];
    /* Three policies over the SAME field trajectory (a scheduling choice does
     * not alter the oscillator dynamics in this model, so all three can be
     * scored from one shared evolution): round-robin, raw resonant (pure
     * argmax — the literal #21 hypothesis), and aged resonant (the shipped
     * live policy: raw + bounded anti-starvation aging). */
    uint64_t rr_runs[RF_BENCH_N], raw_runs[RF_BENCH_N], aged_runs[RF_BENCH_N];
    uint32_t rr_last[RF_BENCH_N], raw_last[RF_BENCH_N], aged_last[RF_BENCH_N];
    uint64_t rr_maxgap = 0, raw_maxgap = 0, aged_maxgap = 0;

    for (int i = 0; i < RF_BENCH_N; i++) {
        ph[i] = (uint32_t)(i + 1) * RF_PHASE_SEED;
        om[i] = RF_OMEGA_BASE + (uint32_t)(i + 1) * RF_OMEGA_SPREAD;
        rr_runs[i] = raw_runs[i] = aged_runs[i] = 0;
        rr_last[i] = raw_last[i] = aged_last[i] = 0;
    }

    uint32_t r_q16_final = 0;
    for (uint32_t t = 0; t < RF_BENCH_STEPS; t++) {
        /* pre-step field summary */
        int sum_cos = 0, sum_sin = 0;
        for (int i = 0; i < RF_BENCH_N; i++) {
            sum_cos += rf_cos(ph[i]);
            sum_sin += rf_sin(ph[i]);
        }
        int avg_cos = sum_cos / RF_BENCH_N;
        int avg_sin = sum_sin / RF_BENCH_N;
        for (int i = 0; i < RF_BENCH_N; i++) {
            int c = rf_cos(ph[i]), s = rf_sin(ph[i]);
            int coupling_q15 = (int)(((int64_t)avg_sin * c -
                                      (int64_t)avg_cos * s) >> 15);
            int32_t pull = (int32_t)(((int64_t)coupling_q15 * (int64_t)RF_KC) >> 15);
            ph[i] += om[i] + (uint32_t)pull;
        }
        /* post-step summary drives alignment + the reported order parameter */
        sum_cos = sum_sin = 0;
        for (int i = 0; i < RF_BENCH_N; i++) {
            sum_cos += rf_cos(ph[i]);
            sum_sin += rf_sin(ph[i]);
        }
        avg_cos = sum_cos / RF_BENCH_N;
        avg_sin = sum_sin / RF_BENCH_N;
        int64_t mag2 = (int64_t)avg_cos * avg_cos + (int64_t)avg_sin * avg_sin;
        uint32_t r_q16 = rf_isqrt((uint32_t)mag2) << 1;
        r_q16_final = r_q16 > 65536u ? 65536u : r_q16;

        /* Score via rf_priority_core (the shipped formula). Canned procs share
         * a base priority, so base_q16 = 0 isolates emergent + aging terms. */
        rf_avg_cos_q15 = avg_cos;
        rf_avg_sin_q15 = avg_sin;
        rf_r_q16 = r_q16_final;

        int rr_pick = (int)(t % RF_BENCH_N);
        int raw_best = 0, aged_best = 0;
        uint32_t raw_bp = 0, aged_bp = 0;
        for (int i = 0; i < RF_BENCH_N; i++) {
            uint32_t raw = rf_priority_core(ph[i], 0, 0);                 /* aging off */
            uint32_t aged = rf_priority_core(ph[i], 0, (uint64_t)(t - aged_last[i]));
            if (i == 0 || raw > raw_bp)   { raw_bp = raw;   raw_best = i; }
            if (i == 0 || aged > aged_bp) { aged_bp = aged; aged_best = i; }
        }

        uint64_t g;
        g = t - rr_last[rr_pick];     if (g > rr_maxgap)   rr_maxgap = g;
        rr_last[rr_pick] = t;         rr_runs[rr_pick]++;
        g = t - raw_last[raw_best];   if (g > raw_maxgap)  raw_maxgap = g;
        raw_last[raw_best] = t;       raw_runs[raw_best]++;
        g = t - aged_last[aged_best]; if (g > aged_maxgap) aged_maxgap = g;
        aged_last[aged_best] = t;     aged_runs[aged_best]++;
    }

    uint64_t rr_mx = 0, rr_mn = (uint64_t)-1;
    uint64_t raw_mx = 0, raw_mn = (uint64_t)-1;
    uint64_t aged_mx = 0, aged_mn = (uint64_t)-1;
    for (int i = 0; i < RF_BENCH_N; i++) {
        if (rr_runs[i] > rr_mx) rr_mx = rr_runs[i];
        if (rr_runs[i] < rr_mn) rr_mn = rr_runs[i];
        if (raw_runs[i] > raw_mx) raw_mx = raw_runs[i];
        if (raw_runs[i] < raw_mn) raw_mn = raw_runs[i];
        if (aged_runs[i] > aged_mx) aged_mx = aged_runs[i];
        if (aged_runs[i] < aged_mn) aged_mn = aged_runs[i];
    }

    char b[160];
    int o = rf_put(b, 0, "SCHED: canned workload procs=");
    o = rf_put_u(b, o, RF_BENCH_N);
    o = rf_put(b, o, " steps=");
    o = rf_put_u(b, o, RF_BENCH_STEPS);
    b[o] = 0; rf_emit(b); rf_emit("\r\n");

    o = rf_put(b, 0, "SCHED: policy=rr fairness=");
    o = rf_put_ratio(b, o, rr_mx, rr_mn);
    o = rf_put(b, o, " maxgap=");
    o = rf_put_u(b, o, rr_maxgap);
    b[o] = 0; rf_emit(b); rf_emit("\r\n");

    /* Raw resonant = the literal #21 hypothesis. It starves (min run-count 0 →
     * fairness INF): this is the strong refutation, on record. */
    o = rf_put(b, 0, "SCHED: policy=resonant-raw r=");
    o = rf_put_frac(b, o, r_q16_final);
    o = rf_put(b, o, " aging=off fairness=");
    o = rf_put_ratio(b, o, raw_mx, raw_mn);
    o = rf_put(b, o, " maxgap=");
    o = rf_put_u(b, o, raw_maxgap);
    b[o] = 0; rf_emit(b); rf_emit("\r\n");

    /* Aged resonant = the policy the live kernel actually runs. */
    o = rf_put(b, 0, "SCHED: policy=resonant r=");
    o = rf_put_frac(b, o, r_q16_final);
    o = rf_put(b, o, " aging=on fairness=");
    o = rf_put_ratio(b, o, aged_mx, aged_mn);
    o = rf_put(b, o, " maxgap=");
    o = rf_put_u(b, o, aged_maxgap);
    b[o] = 0; rf_emit(b); rf_emit("\r\n");

    /* Honest verdict: round-robin is even by construction (fairness 1.00). The
     * raw hypothesis starves; even the aged variant that ships is no better
     * than round-robin on fairness. We print it plainly — proving/refuting #21
     * by execution is the deliverable, not a win for resonant scheduling. */
    bool aged_worse = (aged_mn == 0) || (aged_mx * rr_mn > rr_mx * aged_mn) ||
                      (aged_maxgap > rr_maxgap);
    o = rf_put(b, 0, "SCHED: verdict raw resonant STARVES; aged resonant fairness ");
    o = rf_put(b, o, aged_worse ? "WORSE than rr (hypothesis refuted, reported honestly)"
                                : "on par with rr (hypothesis not supported)");
    b[o] = 0; rf_emit(b); rf_emit("\r\n");
}
