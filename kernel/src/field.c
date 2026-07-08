/**
 * QuantumOS Kernel Holographic Field implementation (epic #95).
 *
 * See kernel/include/kernel/field.h for the model and the syscall-only
 * concurrency invariant. The math is ported from user/kannakad.c's
 * validated Q15 routines (centered byte wavefront, integer isqrt,
 * cosine-by-dot-product), reshaped for caller-supplied byte patterns.
 *
 * Adversarial-review commitments implemented here:
 *  - zero-magnitude guard: a degenerate (all-equal-bytes) pattern is
 *    rejected at imprint (-1 -> EINVAL) and a degenerate probe recalls
 *    n=0 successfully — the integer divide can never see mag == 0;
 *  - lazy decay in u64 with clamped subtraction (no s32 inversion at
 *    long uptimes);
 *  - region ids bounds-checked here as defense in depth even though the
 *    syscall layer already matched the capability.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/field.h>

_Static_assert(sizeof(field_imprint_req_k_t) == 76, "field imprint req ABI drift");
_Static_assert(sizeof(field_recall_req_k_t) == 76, "field recall req ABI drift");
_Static_assert(sizeof(field_recall_out_k_t) == 136, "field recall out ABI drift");

typedef struct {
    uint8_t alive;
    uint8_t len;
    uint32_t retrievals;
    int32_t energy_q15;
    uint64_t imprint_tick;
    uint8_t bytes[FIELD_PAT_MAX];
    int16_t h_q15[FIELD_PAT_MAX]; /* unit wavefront of `bytes` */
} field_slot_t;

typedef struct {
    field_slot_t slot[FIELD_SLOTS];
} field_region_t;

/* ~7 KB of .bss total — no kernel-heap dependency, no init order issue. */
static field_region_t regions[FIELD_REGION_COUNT];

/* ---- integer helpers (freestanding; no libc, no floats) ---- */

/* Integer sqrt of a u64, bit-by-bit — bounded 32 iterations. */
static uint32_t isqrt_u64(uint64_t x) {
    uint64_t root = 0;
    uint64_t bit = 1ULL << 62;
    while (bit > x) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (x >= root + bit) {
            x -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

/* Derive the Q15 unit wavefront of a byte pattern: subtract the mean
 * (raw ASCII is all-positive and would crowd every cosine toward 1 —
 * centering restores discriminating sign structure), then normalize.
 * Returns 0 on success, -1 if the pattern is degenerate (all bytes
 * equal -> zero vector -> nothing to resonate along). */
static int wavefront(const uint8_t *bytes, uint32_t len, int16_t *h_out) {
    int32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum += bytes[i];
    }
    int32_t mean_x256 = (sum * 256) / (int32_t)len; /* mean in 1/256 units */

    int32_t c[FIELD_PAT_MAX];
    uint64_t mag2 = 0;
    for (uint32_t i = 0; i < len; i++) {
        c[i] = (int32_t)bytes[i] * 256 - mean_x256; /* centered, x256 */
        mag2 += (uint64_t)((int64_t)c[i] * c[i]);
    }
    if (mag2 == 0) {
        return -1; /* degenerate: the divide below must never see this */
    }
    uint32_t mag = isqrt_u64(mag2);
    if (mag == 0) {
        return -1;
    }
    for (uint32_t i = 0; i < len; i++) {
        int64_t v = ((int64_t)c[i] * 32767) / (int32_t)mag;
        if (v > 32767) {
            v = 32767;
        }
        if (v < -32767) {
            v = -32767;
        }
        h_out[i] = (int16_t)v;
    }
    return 0;
}

/* Q15 cosine of two unit wavefronts. Different lengths compare over the
 * common prefix — equivalent to zero-padding the shorter one, which
 * preserves both unit norms. */
static int32_t cosine_q15(const int16_t *a, uint32_t alen, const int16_t *b, uint32_t blen) {
    uint32_t n = alen < blen ? alen : blen;
    int64_t acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc += (int64_t)a[i] * b[i];
    }
    acc >>= 15;
    if (acc > 32767) {
        acc = 32767;
    }
    if (acc < -32767) {
        acc = -32767;
    }
    return (int32_t)acc;
}

/* Effective energy after lazy age decay: u64 delta, clamped subtraction
 * — monotone toward the floor, never inverted, no periodic bookkeeping. */
static int32_t effective_energy(const field_slot_t *s, uint64_t now_ticks) {
    uint64_t age = now_ticks >= s->imprint_tick ? now_ticks - s->imprint_tick : 0;
    uint64_t decay = age >> FIELD_DECAY_SHIFT;
    int32_t energy = s->energy_q15;
    if (decay >= (uint64_t)(energy - FIELD_ENERGY_MIN)) {
        return FIELD_ENERGY_MIN;
    }
    return energy - (int32_t)decay;
}

/* ---- public API (syscall context only) ---- */

int64_t field_imprint(uint32_t region, const uint8_t *pattern, uint32_t len, int32_t energy_q15,
                      uint64_t now_ticks) {
    if (region >= FIELD_REGION_COUNT || !pattern || len == 0 || len > FIELD_PAT_MAX) {
        return -1;
    }

    int16_t h[FIELD_PAT_MAX];
    if (wavefront(pattern, len, h) != 0) {
        return -1;
    }

    if (energy_q15 == 0) {
        energy_q15 = FIELD_ENERGY_DEFAULT;
    }
    if (energy_q15 < FIELD_ENERGY_MIN) {
        energy_q15 = FIELD_ENERGY_MIN;
    }
    if (energy_q15 > FIELD_ENERGY_MAX) {
        energy_q15 = FIELD_ENERGY_MAX;
    }

    field_region_t *r = &regions[region];

    /* First free slot, else evict the lowest effective energy. */
    int target = -1;
    for (int i = 0; i < FIELD_SLOTS; i++) {
        if (!r->slot[i].alive) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        int32_t weakest = 0x7FFFFFFF;
        for (int i = 0; i < FIELD_SLOTS; i++) {
            int32_t eff = effective_energy(&r->slot[i], now_ticks);
            if (eff < weakest) {
                weakest = eff;
                target = i;
            }
        }
    }

    field_slot_t *s = &r->slot[target];
    s->alive = 1;
    s->len = (uint8_t)len;
    s->retrievals = 0;
    s->energy_q15 = energy_q15;
    s->imprint_tick = now_ticks;
    for (uint32_t i = 0; i < len; i++) {
        s->bytes[i] = pattern[i];
        s->h_q15[i] = h[i];
    }
    for (uint32_t i = len; i < FIELD_PAT_MAX; i++) {
        s->bytes[i] = 0;
        s->h_q15[i] = 0;
    }
    return target;
}

int64_t field_recall(uint32_t region, const uint8_t *probe, uint32_t len, uint32_t k, int reinforce,
                     field_recall_out_k_t *out, uint64_t now_ticks) {
    if (region >= FIELD_REGION_COUNT || !probe || !out || len == 0 || len > FIELD_PAT_MAX ||
        k == 0 || k > FIELD_RANK_MAX) {
        return -1;
    }

    out->n = 0;
    out->winner_len = 0;
    for (int i = 0; i < FIELD_RANK_MAX; i++) {
        out->rank[i].slot = 0;
        out->rank[i].score_q15 = 0;
    }
    for (int i = 0; i < FIELD_PAT_MAX; i++) {
        out->winner[i] = 0;
    }

    int16_t hp[FIELD_PAT_MAX];
    if (wavefront(probe, len, hp) != 0) {
        return 0; /* degenerate probe resonates with nothing: n = 0 */
    }

    field_region_t *r = &regions[region];

    int32_t score[FIELD_SLOTS];
    int considered[FIELD_SLOTS];
    uint32_t live = 0;
    for (int i = 0; i < FIELD_SLOTS; i++) {
        if (!r->slot[i].alive) {
            continue;
        }
        int32_t sim = cosine_q15(hp, len, r->slot[i].h_q15, r->slot[i].len);
        int32_t eff = effective_energy(&r->slot[i], now_ticks);
        score[live] = (int32_t)(((int64_t)sim * eff) >> 15);
        considered[live] = i;
        live++;
    }
    if (live == 0) {
        return 0;
    }

    /* Selection-sort the top k (kannakad's rule; live <= 8). */
    uint32_t want = k < live ? k : live;
    for (uint32_t a = 0; a < want; a++) {
        uint32_t best = a;
        for (uint32_t b = a + 1; b < live; b++) {
            if (score[b] > score[best]) {
                best = b;
            }
        }
        int32_t ts = score[a];
        score[a] = score[best];
        score[best] = ts;
        int ti = considered[a];
        considered[a] = considered[best];
        considered[best] = ti;
        out->rank[a].slot = (uint32_t)considered[a];
        out->rank[a].score_q15 = score[a];
    }
    out->n = want;

    field_slot_t *w = &r->slot[considered[0]];
    out->winner_len = w->len;
    for (uint32_t i = 0; i < w->len; i++) {
        out->winner[i] = w->bytes[i];
    }

    /* Retrieval reinforcement is a WRITE: only applied when the syscall
     * layer verified the caller holds CAP_WRITE too — a READ-only
     * capability must never mutate the energy landscape. */
    if (reinforce) {
        w->retrievals++;
        w->energy_q15 += (FIELD_ENERGY_MAX - w->energy_q15) >> 4;
        w->imprint_tick = now_ticks; /* reinforcement also refreshes decay */
    }
    return 0;
}

void field_region_scrub(uint32_t region) {
    if (region >= FIELD_REGION_COUNT) {
        return;
    }
    field_region_t *r = &regions[region];
    for (int i = 0; i < FIELD_SLOTS; i++) {
        r->slot[i].alive = 0;
        r->slot[i].len = 0;
        r->slot[i].retrievals = 0;
        r->slot[i].energy_q15 = 0;
        r->slot[i].imprint_tick = 0;
        for (int b = 0; b < FIELD_PAT_MAX; b++) {
            r->slot[i].bytes[b] = 0;
            r->slot[i].h_q15[b] = 0;
        }
    }
}
