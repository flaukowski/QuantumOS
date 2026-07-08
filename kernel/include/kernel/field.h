/**
 * QuantumOS Kernel Holographic Field (epic #95)
 *
 * Associative memory as a first-class kernel primitive: capability-
 * scoped field REGIONS a ring-3 process can IMPRINT patterns into and
 * RECALL associatively against — ranked by wave-resonance similarity,
 * noise-tolerant, single-pass.
 *
 * This is the RANKED HOLOGRAPHIC memory kind (kannaka-memory's essence,
 * ported from user/kannakad.c): each pattern is stored with a Q15 unit
 * "wavefront" derived from its bytes, and recall scores every live slot
 * by cosine-similarity x energy in one bounded pass — syscall-shaped
 * compute (~512 integer MACs), no relaxation loop. ghostd's ATTRACTOR
 * memory (iterative Kuramoto basin dynamics) is a different, living
 * memory kind and deliberately stays in ring 3.
 *
 * Math is integer-only Q15 (the resonant_fixed.c precedent): the kernel
 * is -mno-sse with no fxsave anywhere, so no float may execute here.
 *
 * CONCURRENCY INVARIANT: the field is touched ONLY from syscall context
 * (cli'd, single CPU, non-reentrant). No timer/IRQ path may call into
 * field.c — decay is computed lazily from tick stamps at imprint/recall
 * time, so there is deliberately no periodic bookkeeping and therefore
 * no locking. Adding an IRQ-context caller breaks this contract.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef KERNEL_FIELD_H
#define KERNEL_FIELD_H

#include <kernel/types.h>

#define FIELD_REGION_COUNT 4 /* fixed regions, statically allocated */
#define FIELD_SLOTS 8        /* pattern slots per region */
#define FIELD_PAT_MAX 64     /* max pattern/probe bytes */
#define FIELD_RANK_MAX 8     /* max rankings a recall may request */

#define FIELD_ENERGY_MAX 0x7FFF     /* Q15 1.0 */
#define FIELD_ENERGY_MIN 0x0800     /* floor: no zero-energy squatting */
#define FIELD_ENERGY_DEFAULT 0x4000 /* Q15 0.5 when the caller passes 0 */

/* Lazy age decay: effective energy loses 1 Q15 unit per 2^FIELD_DECAY_SHIFT
 * ticks (100 Hz), computed on demand from the imprint tick stamp — old
 * imprints fade for EVICTION ordering and recall scoring without any
 * IRQ-context bookkeeping. u64 math, clamped: never inverts. */
#define FIELD_DECAY_SHIFT 8

/* ---- syscall ABI structs ----
 * Layouts MUST stay byte-identical to the user-side twins in
 * user/usys.h (field_imprint_req_t / field_recall_req_t /
 * field_recall_out_t). All members are 4-byte or byte arrays: no
 * padding on either compile. Sizes are _Static_asserted in field.c. */

typedef struct {
    uint32_t region;    /* explicit target region; the caller's
                                    * capability must name exactly this id */
    uint32_t len;       /* 1..FIELD_PAT_MAX pattern bytes */
    int32_t energy_q15; /* importance; 0 => FIELD_ENERGY_DEFAULT,
                                    * else clamped to [MIN, MAX] */
    uint8_t pattern[FIELD_PAT_MAX];
} field_imprint_req_k_t; /* 76 bytes */

typedef struct {
    uint32_t region;
    uint32_t len; /* 1..FIELD_PAT_MAX probe bytes */
    uint32_t k;   /* 1..FIELD_RANK_MAX rankings wanted */
    uint8_t probe[FIELD_PAT_MAX];
} field_recall_req_k_t; /* 76 bytes */

typedef struct {
    uint32_t slot;
    int32_t score_q15; /* cosine(sim) x effective-energy, Q15 */
} field_rank_k_t;

typedef struct {
    uint32_t n; /* rankings returned (<= k and <= live slots; 0 is a
                 * successful "nothing resonates", incl. degenerate probe) */
    field_rank_k_t rank[FIELD_RANK_MAX];
    uint32_t winner_len; /* stored content of rank[0] — the
                                     * "completion" a noisy probe recovers */
    uint8_t winner[FIELD_PAT_MAX];
} field_recall_out_k_t; /* 136 bytes */

/* ---- kernel API (syscall context ONLY — see invariant above) ----
 * Region ids are validated here as defense in depth; the syscall layer
 * has already matched the caller's capability against the region. */

/* Store a pattern. Returns the slot index (>= 0), or -1 on invalid
 * arguments (bad region/len, or a degenerate all-equal pattern that has
 * no direction to resonate along). Evicts the lowest effective-energy
 * slot when the region is full. */
int64_t field_imprint(uint32_t region, const uint8_t *pattern, uint32_t len, int32_t energy_q15,
                      uint64_t now_ticks);

/* Ranked associative recall into *out (kernel memory; the syscall layer
 * copies it to the caller afterwards). Returns 0 on success (including
 * out->n == 0), -1 on invalid arguments. When `reinforce` is nonzero
 * (caller holds CAP_WRITE as well as CAP_READ) the winning slot gets
 * retrievals++ and an energy boost of 1/16 of its gap to MAX. */
int64_t field_recall(uint32_t region, const uint8_t *probe, uint32_t len, uint32_t k, int reinforce,
                     field_recall_out_k_t *out, uint64_t now_ticks);

/* Clear every slot in a region. Called when a service is (re)granted
 * the region's capability: a reborn or successor service must never
 * inherit its predecessor's memories. */
void field_region_scrub(uint32_t region);

#endif /* KERNEL_FIELD_H */
