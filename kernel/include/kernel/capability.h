/**
 * QuantumOS Capability-Based Security System
 *
 * Fine-grained, unforgeable access control for all kernel resources
 * (issue #3). Capabilities live in a kernel-private table; user code
 * only ever holds opaque cap_id handles. IDs embed a generation
 * counter, so a revoked slot's stale handles fail validation instead
 * of aliasing a new capability.
 *
 * Model:
 * - No ambient authority: operations on guarded resources require an
 *   explicit capability check.
 * - Least privilege: derived capabilities can only narrow permissions.
 * - Revocation cascades to derived (child) capabilities.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CAPABILITY_H
#define CAPABILITY_H

#include <kernel/types.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define MAX_CAPABILITIES 1024
#define CAP_ID_INVALID 0

/* Permission bits (issue #3 spec) */
#define CAP_READ 0x01
#define CAP_WRITE 0x02
#define CAP_EXECUTE 0x04
#define CAP_GRANT 0x08  /* may derive/transfer this capability */
#define CAP_REVOKE 0x10 /* may revoke this capability's children */
#define CAP_QUANTUM 0x20
#define CAP_DEVICE 0x40
#define CAP_PROCESS 0x80

#define CAP_PERM_ALL 0xFF

/* Resource classes a capability can guard */
typedef enum {
    CAP_RESOURCE_MEMORY = 0, /* memory regions */
    CAP_RESOURCE_IPC,        /* IPC queues/ports/channels */
    CAP_RESOURCE_DEVICE,     /* hardware devices */
    CAP_RESOURCE_QUANTUM,    /* qubits / quantum contexts */
    CAP_RESOURCE_PROCESS,    /* control over other processes */
    CAP_RESOURCE_SERVICE,    /* user-space services */
    CAP_RESOURCE_FIELD,      /* holographic field regions (epic #95) */
    CAP_RESOURCE_TYPE_COUNT
} cap_resource_type_t;

/* Result codes */
typedef enum {
    CAP_SUCCESS = 0,
    CAP_ERROR_INVALID_ID = -1,
    CAP_ERROR_DENIED = -2,
    CAP_ERROR_EXPIRED = -3,
    CAP_ERROR_REVOKED = -4,
    CAP_ERROR_NO_SPACE = -5,
    CAP_ERROR_INVALID_ARG = -6,
    CAP_ERROR_NOT_OWNER = -7,
    CAP_ERROR_ESCALATION = -8, /* derived perms exceed parent perms */
    CAP_ERROR_NOT_REVOCABLE = -9
} cap_result_t;

/* ============================================================================
 * Structures
 * ============================================================================ */

/* Capability token (issue #3 spec, plus resource_type) */
typedef struct {
    uint32_t cap_id;                   /* Unique capability identifier */
    uint32_t owner_id;                 /* Owning process */
    cap_resource_type_t resource_type; /* Class of guarded resource */
    uint32_t resource_id;              /* Resource this cap controls */
    uint32_t permissions;              /* Permission bits */
    uint64_t expiration;               /* Expiration (timer ticks, 0 = never) */
    uint8_t is_revocable;              /* Can this be revoked? */
    uint8_t is_inherited;              /* Derived from a parent? */
    uint32_t parent_cap;               /* Parent capability ID (0 = root) */
} capability_t;

/* Audit statistics */
typedef struct {
    uint64_t created;
    uint64_t derived;
    uint64_t transferred;
    uint64_t revoked;
    uint64_t checks_passed;
    uint64_t checks_denied;
    uint32_t active;
} cap_stats_t;

/* ============================================================================
 * API
 * ============================================================================ */

/* Initialize the capability system (before process/IPC init) */
cap_result_t cap_init(void);

/* Create a root capability for a resource, owned by owner_pid */
cap_result_t cap_create(uint32_t owner_pid, cap_resource_type_t resource_type, uint32_t resource_id,
                        uint32_t permissions, uint64_t expiration, uint32_t *cap_id_out);

/* Derive a child capability with a (subset) permission mask for
 * new_owner_pid. Requires CAP_GRANT on the parent. Attempting to widen
 * permissions fails with CAP_ERROR_ESCALATION. */
cap_result_t cap_derive(uint32_t parent_cap_id, uint32_t requester_pid, uint32_t new_owner_pid,
                        uint32_t permissions, uint64_t expiration, uint32_t *cap_id_out);

/* Transfer ownership of a capability. Requires CAP_GRANT and ownership. */
cap_result_t cap_transfer(uint32_t cap_id, uint32_t from_pid, uint32_t to_pid);

/* Revoke a capability and (recursively) everything derived from it.
 * Requires ownership of the capability or of an ancestor with CAP_REVOKE. */
cap_result_t cap_revoke(uint32_t cap_id, uint32_t requester_pid);

/* Validate: does cap_id exist, belong to pid, guard (resource_type,
 * resource_id), carry all `required_perms`, and remain unexpired? */
cap_result_t cap_check(uint32_t cap_id, uint32_t pid, cap_resource_type_t resource_type,
                       uint32_t resource_id, uint32_t required_perms);

/* Look up a capability's current data (for diagnostics) */
cap_result_t cap_get(uint32_t cap_id, capability_t *out);

/* Find the first live capability owned by `pid` of the given resource
 * type carrying all `required_perms`, and return the resource it
 * guards. Used for capability-as-address routing (e.g. IPC send). */
cap_result_t cap_find(uint32_t pid, cap_resource_type_t resource_type, uint32_t required_perms,
                      uint32_t *resource_id_out);

/* Like cap_find, but for a *specific* target: does `pid` own a live
 * capability of the given resource type that guards exactly
 * `resource_id` and carries all `required_perms`? Used for targeted
 * capability-as-address routing (e.g. a service replying to the sender
 * pid recv handed it), where the destination is chosen rather than
 * taken first-match. */
cap_result_t cap_find_resource(uint32_t pid, cap_resource_type_t resource_type,
                               uint32_t required_perms, uint32_t resource_id);

/* Revoke everything owned by a process (called on process destroy) */
void cap_revoke_all_for_process(uint32_t pid);

/* Audit statistics */
void cap_get_stats(cap_stats_t *stats);

/* Boot-time self test; logs results, returns CAP_SUCCESS if all pass */
cap_result_t cap_selftest(void);

#endif /* CAPABILITY_H */
