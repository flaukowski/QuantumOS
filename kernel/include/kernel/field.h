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
 * field.c — decay is computed lazily from tick stamps at imprint/recall/
 * region_info time, so there is deliberately no periodic bookkeeping and
 * therefore no locking. field_region_info reads the same tick stamps but
 * writes nothing (read-only). Adding an IRQ-context caller breaks this.
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
#define FIELD_PREVIEW 16     /* leading pattern bytes exposed by field_region_info */

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

/* Read-only slot introspection (epic #127 B1). Per ALIVE slot: its index, the
 * stored length, the raw stored energy (stable importance) AND the effective
 * energy after lazy decay (time-varying), the retrieval count, the saturating
 * age in ticks, and a bounded preview of the stored bytes. All members are
 * 4-byte or byte arrays: NO padding on either compile. age_ticks is u32 (never
 * u64 — that would force 8-byte alignment and diverge from the user twin); it
 * saturates at ~497 days of 100 Hz uptime, acceptable for a display field. */
typedef struct {
    uint32_t slot;
    uint32_t len;
    int32_t energy_q15;     /* stored importance, stable */
    int32_t eff_energy_q15; /* after age decay, time-varying */
    uint32_t retrievals;
    uint32_t age_ticks; /* saturates at UINT32_MAX */
    uint8_t preview[FIELD_PREVIEW];
} field_slot_info_k_t; /* 40 bytes */

typedef struct {
    uint32_t region;
    uint32_t live;     /* alive slots (== filled entries in slots[]) */
    uint32_t capacity; /* FIELD_SLOTS */
    field_slot_info_k_t slots[FIELD_SLOTS];
} field_info_out_k_t; /* 332 bytes */

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

/* Read-only enumeration of a region's live slots into *out (kernel memory; the
 * syscall layer copies it out). Returns the live-slot count (>= 0), or -1 on a
 * bad region. Computes decay lazily like recall (same guarded age math) but
 * writes NOTHING — the honest, non-mutating counterpart to the reinforcing
 * recall, safe under a READ-only capability. Zeroes the WHOLE out struct first,
 * so a shared static copy-out buffer never leaks a prior call's (possibly
 * other-region) dead-slot preview bytes. */
int64_t field_region_info(uint32_t region, field_info_out_k_t *out, uint64_t now_ticks);

/* Clear every slot in a region. Called when a service is (re)granted
 * the region's capability: a reborn or successor service must never
 * inherit its predecessor's memories. */
void field_region_scrub(uint32_t region);

/* ---- persistence (epic #96) ----
 * The field rides the QDSK volume: persist_sync serializes every region
 * into a fixed-size blob at a FIXED home at the top of the disk (away
 * from the growing archive), and persist_restore validates + reloads it
 * before any service starts. Restored content survives only the FIRST
 * grant of a region whose service opted in (field_inherit); every other
 * grant scrubs as epic #95 requires. Slot records persist bytes +
 * metadata only — the wavefront is recomputed (and thereby revalidated)
 * on load, and imprint ticks are boot-relative so they reset to 0. */

#define FIELD_BLOB_MAGIC 0x314C4651u /* 'QFL1' little-endian */
#define FIELD_BLOB_VERSION 1u
#define FIELD_SLOT_REC_BYTES 76u
#define FIELD_BLOB_BYTES (24u + FIELD_REGION_COUNT * FIELD_SLOTS * FIELD_SLOT_REC_BYTES) /* 2456 */
#define FIELD_BLOB_SECTORS ((FIELD_BLOB_BYTES + 511u) / 512u)                            /* 5 */

/* Serialize every region into buf (FIELD_BLOB_SECTORS * 512 bytes; fully
 * zeroed first — sector padding must never leak kernel memory). Returns
 * the number of live slots written. */
uint32_t field_blob_write(uint8_t *buf);

/* Validate + load a blob (header, then per-slot: alive/len bounds BEFORE
 * the wavefront recompute, energy clamped). Returns restored live-slot
 * count, or -1 if the header does not describe exactly this build's
 * geometry (then nothing was touched). Invalid slots are dropped, not
 * trusted. Marks populated regions as inheritable-once. */
int64_t field_blob_load(const uint8_t *buf, uint32_t len);

/* Inheritance handshake for the grant path (service.c): peek asks
 * whether a region still holds unclaimed restored content; consume
 * clears the mark once a cap was successfully minted for an opted-in
 * service. A region whose mark is never consumed keeps its content but
 * nothing can reach it without a capability. */
int field_region_inherit_peek(uint32_t region);
void field_region_inherit_consume(uint32_t region);

#endif /* KERNEL_FIELD_H */
