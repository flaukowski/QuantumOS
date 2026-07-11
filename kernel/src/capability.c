/**
 * QuantumOS Capability-Based Security System Implementation
 *
 * Kernel-private capability table with generation-tagged handles.
 * cap_id layout: (generation << 16) | (slot_index + 1). A revoked
 * slot bumps its generation, so stale handles fail validation rather
 * than aliasing whatever reuses the slot.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/audit.h>
#include <kernel/capability.h>
#include <kernel/interrupts.h>
#include <kernel/boot.h>

/* ============================================================================
 * Internal State
 * ============================================================================ */

typedef struct {
    capability_t cap;
    uint16_t generation;
    uint8_t in_use;
} cap_slot_t;

static cap_slot_t cap_table[MAX_CAPABILITIES];

/* High-water mark: (highest ever-used slot index) + 1. Every gated syscall runs
 * authorize() -> cap_find_resource, which linear-scanned all MAX_CAPABILITIES
 * (1024) slots — mostly !in_use skips — on each call. Caps allocate low and the
 * table is sparse, so the read lookups scan only [0, cap_hwm) instead. It grows
 * in alloc_slot and NEVER shrinks, so it is always a valid upper bound on every
 * in-use slot: a lookup can therefore only ever miss a cap (a false deny caught
 * by the boot's capability gates), never match the wrong one. The revoke/reap
 * scans keep the full range (strictly safe, and cold). Guarded by the same
 * cap_irq_save bracket as the table it bounds. */
static uint32_t cap_hwm;

/* Self-contained IRQ save/restore (the qpu.c/audit.c/manifest.c rule). Syscalls
 * run cli'd (int 0x80 is an interrupt gate), so cap_create/derive/revoke from
 * ring 3 are atomic w.r.t. IRQs — but cap_revoke_all_for_process runs at IF=1
 * from the service health-monitor thread's process_destroy. A timer preemption
 * between free_slot's stores (in_use / generation / stats.active) could let a
 * concurrent cli'd allocator claim a half-freed slot, leaking it and skewing
 * stats. Bracket the two structural mutators so the table is atomic. */
static inline uint64_t cap_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
static inline void cap_irq_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
}
static cap_stats_t stats;
static uint8_t cap_initialized = 0;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static uint32_t make_cap_id(uint32_t index, uint16_t generation) {
    return ((uint32_t)generation << 16) | (index + 1);
}

/* Resolve a cap_id to its slot; NULL if stale, revoked, or malformed */
static cap_slot_t *resolve(uint32_t cap_id) {
    if (cap_id == CAP_ID_INVALID) {
        return NULL;
    }

    uint32_t index = (cap_id & 0xFFFF) - 1;
    uint16_t generation = (uint16_t)(cap_id >> 16);

    if (index >= MAX_CAPABILITIES) {
        return NULL;
    }

    cap_slot_t *slot = &cap_table[index];
    if (!slot->in_use || slot->generation != generation) {
        return NULL;
    }

    return slot;
}

static cap_slot_t *alloc_slot(uint32_t *cap_id_out) {
    uint64_t flags = cap_irq_save();
    cap_slot_t *found = NULL;
    for (uint32_t i = 0; i < MAX_CAPABILITIES; i++) {
        if (!cap_table[i].in_use) {
            cap_table[i].in_use = 1;
            *cap_id_out = make_cap_id(i, cap_table[i].generation);
            cap_table[i].cap.cap_id = *cap_id_out;
            found = &cap_table[i];
            if (i + 1 > cap_hwm) {
                cap_hwm = i + 1; /* keep the read-lookup bound covering this slot */
            }
            break;
        }
    }
    cap_irq_restore(flags);
    return found;
}

static void free_slot(cap_slot_t *slot) {
    /* Atomic multi-store: a torn free racing a preempting allocator (IF=1
     * reaper vs cli'd syscall) would leak the slot and skew stats.active. */
    uint64_t flags = cap_irq_save();
    slot->in_use = 0;
    slot->generation++; /* invalidate outstanding handles */
    stats.active--;
    cap_irq_restore(flags);
}

static int is_expired(const capability_t *cap) {
    return cap->expiration != 0 && timer_get_ticks() >= cap->expiration;
}

/* ============================================================================
 * API
 * ============================================================================ */

cap_result_t cap_init(void) {
    memset(cap_table, 0, sizeof(cap_table));
    memset(&stats, 0, sizeof(stats));
    cap_initialized = 1;
    boot_log("Capability system initialized");
    return CAP_SUCCESS;
}

cap_result_t cap_create(uint32_t owner_pid, cap_resource_type_t resource_type, uint32_t resource_id,
                        uint32_t permissions, uint64_t expiration, uint32_t *cap_id_out) {
    if (!cap_initialized || !cap_id_out || resource_type >= CAP_RESOURCE_TYPE_COUNT) {
        return CAP_ERROR_INVALID_ARG;
    }

    cap_slot_t *slot = alloc_slot(cap_id_out);
    if (!slot) {
        return CAP_ERROR_NO_SPACE;
    }

    slot->cap.owner_id = owner_pid;
    slot->cap.resource_type = resource_type;
    slot->cap.resource_id = resource_id;
    slot->cap.permissions = permissions;
    slot->cap.expiration = expiration;
    slot->cap.is_revocable = 1;
    slot->cap.is_inherited = 0;
    slot->cap.is_spawn_channel = 0; /* only sys_spawn tags, via cap_mark_spawn_channel */
    slot->cap.parent_cap = CAP_ID_INVALID;

    stats.created++;
    stats.active++;
    audit_grant(owner_pid, resource_type, resource_id, permissions);
    return CAP_SUCCESS;
}

cap_result_t cap_derive(uint32_t parent_cap_id, uint32_t requester_pid, uint32_t new_owner_pid,
                        uint32_t permissions, uint64_t expiration, uint32_t *cap_id_out) {
    if (!cap_id_out) {
        return CAP_ERROR_INVALID_ARG;
    }

    cap_slot_t *parent = resolve(parent_cap_id);
    if (!parent) {
        return CAP_ERROR_INVALID_ID;
    }
    if (parent->cap.owner_id != requester_pid) {
        return CAP_ERROR_NOT_OWNER;
    }
    if (is_expired(&parent->cap)) {
        return CAP_ERROR_EXPIRED;
    }
    if (!(parent->cap.permissions & CAP_GRANT)) {
        return CAP_ERROR_DENIED;
    }
    /* Least privilege: a child may never carry bits its parent lacks */
    if (permissions & ~parent->cap.permissions) {
        return CAP_ERROR_ESCALATION;
    }
    /* A child may never outlive its parent's expiration */
    if (parent->cap.expiration != 0 && (expiration == 0 || expiration > parent->cap.expiration)) {
        expiration = parent->cap.expiration;
    }

    cap_slot_t *child = alloc_slot(cap_id_out);
    if (!child) {
        return CAP_ERROR_NO_SPACE;
    }

    child->cap.owner_id = new_owner_pid;
    child->cap.resource_type = parent->cap.resource_type;
    child->cap.resource_id = parent->cap.resource_id;
    child->cap.permissions = permissions;
    child->cap.expiration = expiration;
    child->cap.is_revocable = 1;
    child->cap.is_inherited = 1;
    child->cap.is_spawn_channel = 0; /* derivation never inherits the spawn-channel tag */
    child->cap.parent_cap = parent_cap_id;

    stats.derived++;
    stats.active++;
    audit_grant(new_owner_pid, child->cap.resource_type, child->cap.resource_id, permissions);
    return CAP_SUCCESS;
}

/* Recursively revoke every capability derived from parent_id. `kind` is the
 * ledger kind recorded for each freed child (AUDIT_REVOKE when reached from
 * an explicit cap_revoke, AUDIT_REAP from the reaper's
 * cap_revoke_all_for_process) — this helper is shared by BOTH paths and
 * cannot otherwise know which semantic event it is recording. Each child
 * records its OWN owner (a cascade-freed delegated cap belongs to the
 * recipient, not the revoker).
 *
 * Recursion depth is bounded by the one-hop rule (SYS_CAP_DERIVE refuses
 * CAP_GRANT/CAP_REVOKE in derived perms, so a derived cap cannot parent
 * another); revisit if a kernel-internal deep-chain deriver ever appears. */
static void revoke_children_of(uint32_t parent_id, uint16_t kind) {
    for (uint32_t i = 0; i < MAX_CAPABILITIES; i++) {
        if (cap_table[i].in_use && cap_table[i].cap.parent_cap == parent_id) {
            uint32_t child_id = cap_table[i].cap.cap_id;
            revoke_children_of(child_id, kind);
            /* Capture the record fields BEFORE free_slot. The reaper and the
             * IF=1 health monitor run with interrupts ON, free_slot leaves
             * the cap fields intact, and alloc_slot is first-fit — so a
             * post-free read raced by a timer IRQ + another process's
             * cap_create would record the NEXT capability's owner/resource/
             * perms: a misattributed ledger entry no cli'd-syscall test can
             * ever catch. The order here is load-bearing. */
            uint32_t owner = cap_table[i].cap.owner_id;
            uint32_t rtype = cap_table[i].cap.resource_type;
            uint32_t rid = cap_table[i].cap.resource_id;
            uint32_t perms = cap_table[i].cap.permissions;
            free_slot(&cap_table[i]);
            stats.revoked++;
            audit_record(kind, owner, AUDIT_V_OK, rtype, rid, perms);
        }
    }
}

cap_result_t cap_revoke(uint32_t cap_id, uint32_t requester_pid) {
    cap_slot_t *slot = resolve(cap_id);
    if (!slot) {
        return CAP_ERROR_INVALID_ID;
    }
    if (!slot->cap.is_revocable) {
        return CAP_ERROR_NOT_REVOCABLE;
    }

    /* The owner may revoke; so may the owner of an ancestor that
     * carries CAP_REVOKE */
    if (slot->cap.owner_id != requester_pid) {
        uint8_t authorized = 0;
        uint32_t ancestor_id = slot->cap.parent_cap;
        while (ancestor_id != CAP_ID_INVALID) {
            cap_slot_t *ancestor = resolve(ancestor_id);
            if (!ancestor) {
                break;
            }
            if (ancestor->cap.owner_id == requester_pid &&
                (ancestor->cap.permissions & CAP_REVOKE)) {
                authorized = 1;
                break;
            }
            ancestor_id = ancestor->cap.parent_cap;
        }
        if (!authorized) {
            return CAP_ERROR_NOT_OWNER;
        }
    }

    revoke_children_of(cap_id, AUDIT_REVOKE);
    /* Capture-before-free: same load-bearing ordering as revoke_children_of. */
    {
        uint32_t owner = slot->cap.owner_id;
        uint32_t rtype = slot->cap.resource_type;
        uint32_t rid = slot->cap.resource_id;
        uint32_t perms = slot->cap.permissions;
        free_slot(slot);
        stats.revoked++;
        audit_revoke(owner, rtype, rid, perms);
    }
    return CAP_SUCCESS;
}

cap_result_t cap_check(uint32_t cap_id, uint32_t pid, cap_resource_type_t resource_type,
                       uint32_t resource_id, uint32_t required_perms) {
    cap_slot_t *slot = resolve(cap_id);
    if (!slot) {
        stats.checks_denied++;
        return CAP_ERROR_INVALID_ID;
    }

    if (slot->cap.owner_id != pid || slot->cap.resource_type != resource_type ||
        slot->cap.resource_id != resource_id) {
        stats.checks_denied++;
        return CAP_ERROR_DENIED;
    }

    if (is_expired(&slot->cap)) {
        stats.checks_denied++;
        return CAP_ERROR_EXPIRED;
    }

    if ((slot->cap.permissions & required_perms) != required_perms) {
        stats.checks_denied++;
        return CAP_ERROR_DENIED;
    }

    stats.checks_passed++;
    return CAP_SUCCESS;
}

cap_result_t cap_get(uint32_t cap_id, capability_t *out) {
    cap_slot_t *slot = resolve(cap_id);
    if (!slot || !out) {
        return CAP_ERROR_INVALID_ID;
    }
    *out = slot->cap;
    return CAP_SUCCESS;
}

cap_result_t cap_find(uint32_t pid, cap_resource_type_t resource_type, uint32_t required_perms,
                      uint32_t *resource_id_out) {
    for (uint32_t i = 0; i < cap_hwm; i++) {
        if (!cap_table[i].in_use) {
            continue;
        }
        capability_t *c = &cap_table[i].cap;
        if (c->owner_id != pid || c->resource_type != resource_type) {
            continue;
        }
        if ((c->permissions & required_perms) != required_perms) {
            continue;
        }
        if (is_expired(c)) {
            continue;
        }
        if (resource_id_out) {
            *resource_id_out = c->resource_id;
        }
        return CAP_SUCCESS;
    }
    return CAP_ERROR_DENIED;
}

cap_result_t cap_find_resource(uint32_t pid, cap_resource_type_t resource_type,
                               uint32_t required_perms, uint32_t resource_id) {
    for (uint32_t i = 0; i < cap_hwm; i++) {
        if (!cap_table[i].in_use) {
            continue;
        }
        capability_t *c = &cap_table[i].cap;
        if (c->owner_id != pid || c->resource_type != resource_type ||
            c->resource_id != resource_id) {
            continue;
        }
        if ((c->permissions & required_perms) != required_perms) {
            continue;
        }
        if (is_expired(c)) {
            continue;
        }
        return CAP_SUCCESS;
    }
    return CAP_ERROR_DENIED;
}

cap_result_t cap_find_id(uint32_t pid, cap_resource_type_t resource_type, uint32_t required_perms,
                         uint32_t resource_id, uint32_t *cap_id_out) {
    for (uint32_t i = 0; i < cap_hwm; i++) {
        if (!cap_table[i].in_use) {
            continue;
        }
        capability_t *c = &cap_table[i].cap;
        if (c->owner_id != pid || c->resource_type != resource_type ||
            c->resource_id != resource_id) {
            continue;
        }
        if ((c->permissions & required_perms) != required_perms) {
            continue;
        }
        if (is_expired(c)) {
            continue;
        }
        if (cap_id_out) {
            *cap_id_out = c->cap_id;
        }
        return CAP_SUCCESS;
    }
    return CAP_ERROR_DENIED;
}

void cap_revoke_all_for_process(uint32_t pid) {
    for (uint32_t i = 0; i < MAX_CAPABILITIES; i++) {
        if (cap_table[i].in_use && cap_table[i].cap.owner_id == pid) {
            uint32_t id = cap_table[i].cap.cap_id;
            /* Death cleanup, not authority revocation: recorded as REAP so a
             * verifier reading the durable ledger never mistakes garbage
             * collection for an exercised revoke. The cascade children are
             * REAPed too — they died WITH their owner's death, transitively. */
            revoke_children_of(id, AUDIT_REAP);
            /* Capture-before-free: this runs from the IF=1 idle-loop reaper
             * and health monitor — see revoke_children_of for why the order
             * is load-bearing. */
            uint32_t rtype = cap_table[i].cap.resource_type;
            uint32_t rid = cap_table[i].cap.resource_id;
            uint32_t perms = cap_table[i].cap.permissions;
            free_slot(&cap_table[i]);
            stats.revoked++;
            audit_reap(pid, rtype, rid, perms);
        }
    }
}

cap_result_t cap_mark_spawn_channel(uint32_t cap_id) {
    cap_slot_t *slot = resolve(cap_id);
    if (!slot) {
        return CAP_ERROR_INVALID_ID;
    }
    slot->cap.is_spawn_channel = 1;
    return CAP_SUCCESS;
}

void cap_revoke_spawn_channels(uint32_t dead_pid) {
    for (uint32_t i = 0; i < MAX_CAPABILITIES; i++) {
        /* Scoped THREE ways: origin tag (hand-minted pairs survive a peer's
         * restart), resource type (a FIELD/PROC cap whose resource_id happens
         * to equal a small pid survives — pids are slot indices and collide
         * with region/device ids), and the dead target itself. */
        if (cap_table[i].in_use && cap_table[i].cap.is_spawn_channel &&
            cap_table[i].cap.resource_type == CAP_RESOURCE_IPC &&
            cap_table[i].cap.resource_id == dead_pid) {
            uint32_t id = cap_table[i].cap.cap_id;
            /* Defensive: channel caps carry no CAP_GRANT so nothing derives
             * from them today; if a future deriver appears, its children die
             * with the channel rather than dangling. */
            revoke_children_of(id, AUDIT_UNLINK);
            /* Capture-before-free (the capability.c:245 discipline): this runs
             * at IF=1 from process_destroy's callers, and the audit must be
             * emitted from the captured copy, never the freed slot. The record
             * lands under the SURVIVING owner — the peer whose channel half
             * became meaningless — with kind UNLINK, never REAP: the owner did
             * NOT die, and REAP must keep meaning "owner died" for verifiers. */
            uint32_t owner = cap_table[i].cap.owner_id;
            uint32_t rtype = cap_table[i].cap.resource_type;
            uint32_t rid = cap_table[i].cap.resource_id;
            uint32_t perms = cap_table[i].cap.permissions;
            free_slot(&cap_table[i]);
            stats.revoked++;
            audit_unlink(owner, rtype, rid, perms);
        }
    }
}

void cap_get_stats(cap_stats_t *out) {
    if (out) {
        *out = stats;
    }
}

/* ============================================================================
 * Boot self-test
 * ============================================================================ */

#define ST_ASSERT(cond, msg)                                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            boot_log("cap-selftest FAIL: " msg);                                                   \
            return CAP_ERROR_DENIED;                                                               \
        }                                                                                          \
    } while (0)

cap_result_t cap_selftest(void) {
    uint32_t root = 0, child = 0, grandchild = 0;

    /* High-water-mark lookup gate (hot-path optimization). Runs FIRST, while the
     * table is still packed from the bottom (no revoked holes yet), so the cap
     * created here deterministically EXTENDS the hwm — the case the optimization
     * must get right. Proves the read lookups both (a) find a cap the hwm covers
     * (so alloc_slot advanced hwm past the allocated slot — an off-by-one would
     * leave hwm == slot and this find would DENY), and (b) actually honor the
     * bound: shrinking hwm below the cap's slot must make the same lookup miss it
     * (if the scan still ran to MAX_CAPABILITIES it would wrongly succeed). */
    {
        uint32_t hc = 0;
        uint32_t before = cap_hwm;
        ST_ASSERT(cap_create(777, CAP_RESOURCE_MEMORY, 0xABC, CAP_READ, 0, &hc) == CAP_SUCCESS,
                  "hwm-gate create");
        uint32_t slot = (hc & 0xFFFF) - 1;
        ST_ASSERT(slot >= before, "hwm-gate cap extends the mark"); /* packed table -> new top */
        ST_ASSERT(cap_hwm == slot + 1, "hwm advanced exactly past the allocated slot");
        ST_ASSERT(cap_find_resource(777, CAP_RESOURCE_MEMORY, CAP_READ, 0xABC) == CAP_SUCCESS,
                  "cap found within hwm");
        uint32_t saved = cap_hwm;
        cap_hwm = slot; /* exclude the cap's slot from the scan */
        ST_ASSERT(cap_find_resource(777, CAP_RESOURCE_MEMORY, CAP_READ, 0xABC) != CAP_SUCCESS,
                  "lookup honors the hwm bound");
        cap_hwm = saved;
        cap_revoke(hc, 777);
        boot_log("CAPHWM: capability lookups bounded by high-water-mark");
    }

    /* Create + check */
    ST_ASSERT(cap_create(1, CAP_RESOURCE_MEMORY, 42, CAP_READ | CAP_WRITE | CAP_GRANT | CAP_REVOKE,
                         0, &root) == CAP_SUCCESS,
              "create");
    ST_ASSERT(cap_check(root, 1, CAP_RESOURCE_MEMORY, 42, CAP_READ) == CAP_SUCCESS, "check pass");

    /* Wrong pid / resource / perms are denied */
    ST_ASSERT(cap_check(root, 2, CAP_RESOURCE_MEMORY, 42, CAP_READ) != CAP_SUCCESS,
              "check wrong pid");
    ST_ASSERT(cap_check(root, 1, CAP_RESOURCE_MEMORY, 43, CAP_READ) != CAP_SUCCESS,
              "check wrong resource");
    ST_ASSERT(cap_check(root, 1, CAP_RESOURCE_MEMORY, 42, CAP_EXECUTE) != CAP_SUCCESS,
              "check missing perm");

    /* Derivation narrows; escalation is refused */
    ST_ASSERT(cap_derive(root, 1, 2, CAP_READ | CAP_GRANT, 0, &child) == CAP_SUCCESS, "derive");
    ST_ASSERT(cap_check(child, 2, CAP_RESOURCE_MEMORY, 42, CAP_READ) == CAP_SUCCESS,
              "derived check");
    ST_ASSERT(cap_derive(root, 1, 2, CAP_EXECUTE, 0, &grandchild) == CAP_ERROR_ESCALATION,
              "escalation refused");
    ST_ASSERT(cap_derive(child, 2, 3, CAP_READ, 0, &grandchild) == CAP_SUCCESS,
              "grandchild derive");

    /* Revocation cascades and stale handles die */
    ST_ASSERT(cap_revoke(root, 1) == CAP_SUCCESS, "revoke root");
    ST_ASSERT(cap_check(root, 1, CAP_RESOURCE_MEMORY, 42, CAP_READ) == CAP_ERROR_INVALID_ID,
              "root stale after revoke");
    ST_ASSERT(cap_check(child, 2, CAP_RESOURCE_MEMORY, 42, CAP_READ) == CAP_ERROR_INVALID_ID,
              "child cascade-revoked");
    ST_ASSERT(cap_check(grandchild, 3, CAP_RESOURCE_MEMORY, 42, CAP_READ) == CAP_ERROR_INVALID_ID,
              "grandchild cascade-revoked");

    /* Expiration */
    uint32_t ephemeral = 0;
    ST_ASSERT(cap_create(1, CAP_RESOURCE_IPC, 7, CAP_READ, 1, &ephemeral) == CAP_SUCCESS,
              "ephemeral create");
    /* expiration tick 1 is already in the past once the timer runs; at
     * boot (ticks==0) it must still be valid */
    if (timer_get_ticks() == 0) {
        ST_ASSERT(cap_check(ephemeral, 1, CAP_RESOURCE_IPC, 7, CAP_READ) == CAP_SUCCESS,
                  "ephemeral valid pre-tick");
    }
    cap_revoke(ephemeral, 1);

    /* Spawn-channel unlink gate (epic #175). ORDERING CONSTRAINT: this leg
     * creates-and-frees caps, so it must stay AFTER the CAPHWM block above —
     * that gate requires a packed hole-free table, and a hole punched before
     * it would first-fit its probe cap below the mark and panic the boot.
     * Anti-vacuous by survivor asserts: the revoker must be scoped by origin
     * tag AND resource type AND target — each survivor below defeats one
     * over-broad implementation that a lone "target cap died" assert would
     * let ship green. Fake pids: 900 = surviving peer, 901 = dead child,
     * 902 = unrelated. */
    {
        uint32_t ch = 0, other = 0, field = 0, hand = 0;
        ST_ASSERT(cap_create(900, CAP_RESOURCE_IPC, 901, CAP_READ | CAP_WRITE, 0, &ch) ==
                      CAP_SUCCESS,
                  "unlink-gate channel create");
        ST_ASSERT(cap_mark_spawn_channel(ch) == CAP_SUCCESS, "unlink-gate tag");
        ST_ASSERT(cap_create(900, CAP_RESOURCE_IPC, 902, CAP_READ | CAP_WRITE, 0, &other) ==
                      CAP_SUCCESS,
                  "unlink-gate other-target create");
        ST_ASSERT(cap_mark_spawn_channel(other) == CAP_SUCCESS, "unlink-gate other tag");
        ST_ASSERT(cap_create(900, CAP_RESOURCE_FIELD, 901, CAP_READ, 0, &field) == CAP_SUCCESS,
                  "unlink-gate field create"); /* resource_id COLLIDES with the dead pid */
        /* TAG the field cap too: no tagged non-IPC cap exists in practice (only
         * sys_spawn tags, and only on IPC caps), so this deliberately constructs
         * the adversarial case where ONLY the type filter protects the survivor —
         * without it, dropping the filter would pass vacuously (the untagged
         * field cap would survive on the origin filter alone). */
        ST_ASSERT(cap_mark_spawn_channel(field) == CAP_SUCCESS, "unlink-gate field tag");
        ST_ASSERT(cap_create(900, CAP_RESOURCE_IPC, 901, CAP_READ | CAP_WRITE, 0, &hand) ==
                      CAP_SUCCESS,
                  "unlink-gate hand-minted create"); /* same target, NO tag */

        cap_revoke_spawn_channels(901);

        ST_ASSERT(cap_check(ch, 900, CAP_RESOURCE_IPC, 901, CAP_READ) == CAP_ERROR_INVALID_ID,
                  "tagged channel to dead pid freed");
        ST_ASSERT(cap_check(other, 900, CAP_RESOURCE_IPC, 902, CAP_READ) == CAP_SUCCESS,
                  "tagged channel to a LIVE pid survives");
        ST_ASSERT(cap_check(field, 900, CAP_RESOURCE_FIELD, 901, CAP_READ) == CAP_SUCCESS,
                  "non-IPC cap with colliding resource_id survives (type filter)");
        ST_ASSERT(cap_check(hand, 900, CAP_RESOURCE_IPC, 901, CAP_READ) == CAP_SUCCESS,
                  "untagged hand-minted pair survives (origin filter)");
        ST_ASSERT(audit_ring_has_kind(AUDIT_UNLINK), "UNLINK recorded in the ledger");

        /* Leave the table as found (later self-tests and the packed-boot
         * mints must see no unexpected live caps). */
        cap_revoke(other, 900);
        cap_revoke(field, 900);
        cap_revoke(hand, 900);
        boot_log("CAPUNLINK: spawn-channel caps die with their target (scoped by tag+type)");
    }

    boot_log("capability self-test: PASS");
    return CAP_SUCCESS;
}
