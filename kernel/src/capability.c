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
    for (uint32_t i = 0; i < MAX_CAPABILITIES; i++) {
        if (!cap_table[i].in_use) {
            cap_table[i].in_use = 1;
            *cap_id_out = make_cap_id(i, cap_table[i].generation);
            cap_table[i].cap.cap_id = *cap_id_out;
            return &cap_table[i];
        }
    }
    return NULL;
}

static void free_slot(cap_slot_t *slot) {
    slot->in_use = 0;
    slot->generation++; /* invalidate outstanding handles */
    stats.active--;
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

cap_result_t cap_create(uint32_t owner_pid, cap_resource_type_t resource_type,
                        uint32_t resource_id, uint32_t permissions,
                        uint64_t expiration, uint32_t *cap_id_out) {
    if (!cap_initialized || !cap_id_out ||
        resource_type >= CAP_RESOURCE_TYPE_COUNT) {
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
    slot->cap.parent_cap = CAP_ID_INVALID;

    stats.created++;
    stats.active++;
    return CAP_SUCCESS;
}

cap_result_t cap_derive(uint32_t parent_cap_id, uint32_t requester_pid,
                        uint32_t new_owner_pid, uint32_t permissions,
                        uint64_t expiration, uint32_t *cap_id_out) {
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
    if (parent->cap.expiration != 0 &&
        (expiration == 0 || expiration > parent->cap.expiration)) {
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
    child->cap.parent_cap = parent_cap_id;

    stats.derived++;
    stats.active++;
    return CAP_SUCCESS;
}

cap_result_t cap_transfer(uint32_t cap_id, uint32_t from_pid, uint32_t to_pid) {
    cap_slot_t *slot = resolve(cap_id);
    if (!slot) {
        return CAP_ERROR_INVALID_ID;
    }
    if (slot->cap.owner_id != from_pid) {
        return CAP_ERROR_NOT_OWNER;
    }
    if (!(slot->cap.permissions & CAP_GRANT)) {
        return CAP_ERROR_DENIED;
    }
    if (is_expired(&slot->cap)) {
        return CAP_ERROR_EXPIRED;
    }

    slot->cap.owner_id = to_pid;
    stats.transferred++;
    return CAP_SUCCESS;
}

/* Recursively revoke every capability derived from parent_id */
static void revoke_children_of(uint32_t parent_id) {
    for (uint32_t i = 0; i < MAX_CAPABILITIES; i++) {
        if (cap_table[i].in_use && cap_table[i].cap.parent_cap == parent_id) {
            uint32_t child_id = cap_table[i].cap.cap_id;
            revoke_children_of(child_id);
            free_slot(&cap_table[i]);
            stats.revoked++;
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

    revoke_children_of(cap_id);
    free_slot(slot);
    stats.revoked++;
    return CAP_SUCCESS;
}

cap_result_t cap_check(uint32_t cap_id, uint32_t pid,
                       cap_resource_type_t resource_type,
                       uint32_t resource_id, uint32_t required_perms) {
    cap_slot_t *slot = resolve(cap_id);
    if (!slot) {
        stats.checks_denied++;
        return CAP_ERROR_INVALID_ID;
    }

    if (slot->cap.owner_id != pid ||
        slot->cap.resource_type != resource_type ||
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

cap_result_t cap_find(uint32_t pid, cap_resource_type_t resource_type,
                      uint32_t required_perms, uint32_t *resource_id_out) {
    for (uint32_t i = 0; i < MAX_CAPABILITIES; i++) {
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
    for (uint32_t i = 0; i < MAX_CAPABILITIES; i++) {
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

void cap_revoke_all_for_process(uint32_t pid) {
    for (uint32_t i = 0; i < MAX_CAPABILITIES; i++) {
        if (cap_table[i].in_use && cap_table[i].cap.owner_id == pid) {
            uint32_t id = cap_table[i].cap.cap_id;
            revoke_children_of(id);
            free_slot(&cap_table[i]);
            stats.revoked++;
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

#define ST_ASSERT(cond, msg)                        \
    do {                                            \
        if (!(cond)) {                              \
            boot_log("cap-selftest FAIL: " msg);    \
            return CAP_ERROR_DENIED;                \
        }                                           \
    } while (0)

cap_result_t cap_selftest(void) {
    uint32_t root = 0, child = 0, grandchild = 0;

    /* Create + check */
    ST_ASSERT(cap_create(1, CAP_RESOURCE_MEMORY, 42,
                         CAP_READ | CAP_WRITE | CAP_GRANT | CAP_REVOKE,
                         0, &root) == CAP_SUCCESS, "create");
    ST_ASSERT(cap_check(root, 1, CAP_RESOURCE_MEMORY, 42, CAP_READ) == CAP_SUCCESS,
              "check pass");

    /* Wrong pid / resource / perms are denied */
    ST_ASSERT(cap_check(root, 2, CAP_RESOURCE_MEMORY, 42, CAP_READ) != CAP_SUCCESS,
              "check wrong pid");
    ST_ASSERT(cap_check(root, 1, CAP_RESOURCE_MEMORY, 43, CAP_READ) != CAP_SUCCESS,
              "check wrong resource");
    ST_ASSERT(cap_check(root, 1, CAP_RESOURCE_MEMORY, 42, CAP_EXECUTE) != CAP_SUCCESS,
              "check missing perm");

    /* Derivation narrows; escalation is refused */
    ST_ASSERT(cap_derive(root, 1, 2, CAP_READ | CAP_GRANT, 0, &child) == CAP_SUCCESS,
              "derive");
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

    boot_log("capability self-test: PASS");
    return CAP_SUCCESS;
}
