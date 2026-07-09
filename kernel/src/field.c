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
_Static_assert(sizeof(field_slot_info_k_t) == 40, "field slot info ABI drift");
_Static_assert(sizeof(field_info_out_k_t) == 332, "field info out ABI drift");

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
        target = 0;
        int32_t weakest = effective_energy(&r->slot[0], now_ticks);
        for (int i = 1; i < FIELD_SLOTS; i++) {
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

int64_t field_region_info(uint32_t region, field_info_out_k_t *out, uint64_t now_ticks) {
    if (!out) {
        return -1;
    }
    /* Zero the WHOLE struct FIRST. The syscall layer copies out every
     * FIELD_SLOTS entry from a process-shared static buffer, so any dead-slot
     * entry left un-rewritten would leak a prior call's (possibly other-region)
     * preview bytes across the exact-region isolation boundary. This mirrors
     * field_blob_write's "fully zeroed first — must never leak kernel memory". */
    uint8_t *raw = (uint8_t *)out;
    for (size_t i = 0; i < sizeof(*out); i++) {
        raw[i] = 0;
    }
    if (region >= FIELD_REGION_COUNT) {
        return -1;
    }
    /* const throughout: read-only is enforced structurally, not by convention
     * (the reinforcing recall is the ONLY writer — the dropped qos_field_status
     * fell through exactly this gap). */
    const field_region_t *r = &regions[region];
    out->region = region;
    out->capacity = FIELD_SLOTS;
    uint32_t live = 0;
    for (int i = 0; i < FIELD_SLOTS; i++) {
        const field_slot_t *s = &r->slot[i];
        if (!s->alive) {
            continue;
        }
        field_slot_info_k_t *si = &out->slots[live];
        /* The SAME guarded age math effective_energy uses — the two fields are
         * always mutually consistent and the subtraction can never underflow. */
        uint64_t age = now_ticks >= s->imprint_tick ? now_ticks - s->imprint_tick : 0;
        si->slot = (uint32_t)i;
        si->len = s->len;
        si->energy_q15 = s->energy_q15;
        si->eff_energy_q15 = effective_energy(s, now_ticks);
        si->retrievals = s->retrievals;
        si->age_ticks = age > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)age;
        uint32_t pv = s->len < FIELD_PREVIEW ? s->len : FIELD_PREVIEW;
        for (uint32_t b = 0; b < pv; b++) {
            si->preview[b] = s->bytes[b];
        }
        live++;
    }
    out->live = live;
    return (int64_t)live;
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

/* ============================================================================
 * Persistence (epic #96) — see field.h. All little-endian u32 fields,
 * written/read bytewise so the layout is compiler-independent.
 * ============================================================================ */

static uint8_t region_inheritable[FIELD_REGION_COUNT];

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t field_blob_write(uint8_t *buf) {
    /* Zero the WHOLE sector-rounded buffer first: the tail padding lands
     * on disk and must never carry kernel memory. */
    for (uint32_t i = 0; i < FIELD_BLOB_SECTORS * 512u; i++) {
        buf[i] = 0;
    }
    put_u32(buf + 0, FIELD_BLOB_MAGIC);
    put_u32(buf + 4, FIELD_BLOB_VERSION);
    put_u32(buf + 8, FIELD_REGION_COUNT);
    put_u32(buf + 12, FIELD_SLOTS);
    put_u32(buf + 16, FIELD_PAT_MAX);
    /* buf+20 reserved, zero */

    uint32_t live = 0;
    uint8_t *rec = buf + 24;
    for (uint32_t rg = 0; rg < FIELD_REGION_COUNT; rg++) {
        for (int sl = 0; sl < FIELD_SLOTS; sl++) {
            const field_slot_t *s = &regions[rg].slot[sl];
            rec[0] = s->alive ? 1 : 0;
            rec[1] = s->len;
            /* rec[2..3] reserved */
            put_u32(rec + 4, s->retrievals);
            put_u32(rec + 8, (uint32_t)s->energy_q15);
            for (uint32_t b = 0; b < FIELD_PAT_MAX; b++) {
                rec[12 + b] = s->bytes[b];
            }
            if (s->alive) {
                live++;
            }
            rec += FIELD_SLOT_REC_BYTES;
        }
    }
    return live;
}

int64_t field_blob_load(const uint8_t *buf, uint32_t len) {
    if (len < FIELD_BLOB_BYTES) {
        return -1;
    }
    /* The header must describe EXACTLY this build's geometry — any
     * mismatch (older layout, scribbled sector) means nothing here can
     * be trusted, and nothing has been touched yet. */
    if (get_u32(buf + 0) != FIELD_BLOB_MAGIC || get_u32(buf + 4) != FIELD_BLOB_VERSION ||
        get_u32(buf + 8) != FIELD_REGION_COUNT || get_u32(buf + 12) != FIELD_SLOTS ||
        get_u32(buf + 16) != FIELD_PAT_MAX) {
        return -1;
    }

    int64_t restored = 0;
    const uint8_t *rec = buf + 24;
    for (uint32_t rg = 0; rg < FIELD_REGION_COUNT; rg++) {
        field_region_scrub(rg);
        uint32_t region_live = 0;
        for (int sl = 0; sl < FIELD_SLOTS; sl++, rec += FIELD_SLOT_REC_BYTES) {
            /* The disk crossed a trust boundary: every field is
             * revalidated, and len is bounds-checked BEFORE the
             * wavefront recompute may index with it. */
            if (rec[0] != 1) {
                continue; /* dead (or scribbled alive-byte) slot */
            }
            uint32_t slen = rec[1];
            if (slen == 0 || slen > FIELD_PAT_MAX) {
                continue;
            }
            int16_t h[FIELD_PAT_MAX];
            if (wavefront(rec + 12, slen, h) != 0) {
                continue; /* degenerate content cannot resonate — drop */
            }
            int32_t energy = (int32_t)get_u32(rec + 8);
            if (energy < FIELD_ENERGY_MIN) {
                energy = FIELD_ENERGY_MIN;
            }
            if (energy > FIELD_ENERGY_MAX) {
                energy = FIELD_ENERGY_MAX;
            }
            field_slot_t *s = &regions[rg].slot[sl];
            s->alive = 1;
            s->len = (uint8_t)slen;
            s->retrievals = get_u32(rec + 4);
            s->energy_q15 = energy;
            s->imprint_tick = 0; /* boot-relative ticks reset on restore */
            for (uint32_t b = 0; b < FIELD_PAT_MAX; b++) {
                s->bytes[b] = b < slen ? rec[12 + b] : 0;
            }
            for (uint32_t b = 0; b < FIELD_PAT_MAX; b++) {
                s->h_q15[b] = b < slen ? h[b] : 0;
            }
            region_live++;
            restored++;
        }
        region_inheritable[rg] = region_live > 0 ? 1 : 0;
    }
    return restored;
}

int field_region_inherit_peek(uint32_t region) {
    return region < FIELD_REGION_COUNT && region_inheritable[region];
}

void field_region_inherit_consume(uint32_t region) {
    if (region < FIELD_REGION_COUNT) {
        region_inheritable[region] = 0;
    }
}
