/**
 * QuantumOS IPC System Implementation
 *
 * Message-passing IPC system for the QuantumOS microkernel.
 * Implements synchronous and asynchronous message passing with
 * zero-copy shared memory support.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/ipc.h>
#include <kernel/types.h>
#include <kernel/boot.h>

/* ============================================================================
 * Internal Constants
 * ============================================================================ */

#define MAX_PROCESSES       256
#define MAX_PORTS           128
#define MAX_SHARED_REGIONS  64
#define MAX_CHANNELS        64
#define MAX_GRANTS_PER_REGION 16

/* ============================================================================
 * Internal State
 * ============================================================================ */

/* Per-process message queues */
static ipc_queue_t process_queues[MAX_PROCESSES];
static uint8_t queue_initialized[MAX_PROCESSES];

/* Named ports */
static ipc_port_t ports[MAX_PORTS];
static uint32_t next_port_id = 1;

/* Shared memory regions */
static ipc_shared_region_t shared_regions[MAX_SHARED_REGIONS];
static uint32_t next_region_id = 1;

/* Region grants */
static ipc_region_grant_t region_grants[MAX_SHARED_REGIONS][MAX_GRANTS_PER_REGION];

/* Channels */
static ipc_channel_t channels[MAX_CHANNELS];
static uint32_t next_channel_id = 1;

/* Global message ID counter */
static uint32_t next_message_id = 1;

/* IPC statistics */
static struct {
    uint64_t total_sent;
    uint64_t total_received;
    uint64_t total_dropped;
} ipc_global_stats;

/* IPC subsystem initialized flag */
static uint8_t ipc_initialized = 0;

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static uint32_t get_current_pid(void);
static uint64_t get_timestamp_ns(void);
static ipc_queue_entry_t *queue_alloc_entry(void);
static void queue_free_entry(ipc_queue_entry_t *entry);
static ipc_result_t queue_enqueue(ipc_queue_t *queue, const ipc_message_t *msg);
static ipc_result_t queue_dequeue(ipc_queue_t *queue, ipc_message_t *msg, uint32_t *filter_sender);
static ipc_port_t *find_port_by_id(uint32_t port_id);
static ipc_port_t *find_port_by_name(const char *name);
static ipc_shared_region_t *find_region(uint32_t region_id);
static ipc_channel_t *find_channel(uint32_t channel_id);

/* ============================================================================
 * Utility Implementations
 * ============================================================================ */

/**
 * Get current process ID
 * TODO: Integrate with process management system
 */
static uint32_t get_current_pid(void) {
    /* Placeholder - return kernel PID for now */
    return IPC_PID_KERNEL;
}

/**
 * Get current timestamp in nanoseconds
 * TODO: Integrate with timer subsystem
 */
static uint64_t get_timestamp_ns(void) {
    /* Placeholder - return 0 for now */
    return 0;
}

/**
 * Simple string comparison
 */
static int str_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/**
 * String copy with limit
 */
static void str_copy(char *dest, const char *src, size_t max) {
    size_t i = 0;
    while (i < max - 1 && src[i]) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

/* ============================================================================
 * Queue Entry Management
 * ============================================================================ */

/* Simple entry pool for queue entries */
#define ENTRY_POOL_SIZE (MAX_PROCESSES * IPC_MAX_QUEUE_SIZE)
static ipc_queue_entry_t entry_pool[ENTRY_POOL_SIZE];
static uint8_t entry_in_use[ENTRY_POOL_SIZE];

static ipc_queue_entry_t *queue_alloc_entry(void) {
    for (uint32_t i = 0; i < ENTRY_POOL_SIZE; i++) {
        if (!entry_in_use[i]) {
            entry_in_use[i] = 1;
            entry_pool[i].next = NULL;
            entry_pool[i].prev = NULL;
            return &entry_pool[i];
        }
    }
    return NULL;
}

static void queue_free_entry(ipc_queue_entry_t *entry) {
    if (!entry) return;

    uint32_t idx = (uint32_t)(entry - entry_pool);
    if (idx < ENTRY_POOL_SIZE) {
        entry_in_use[idx] = 0;
    }
}

/* ============================================================================
 * Queue Operations
 * ============================================================================ */

static ipc_result_t queue_enqueue(ipc_queue_t *queue, const ipc_message_t *msg) {
    if (!queue || !msg) {
        return IPC_ERROR_INVALID_ARG;
    }

    if (queue->count >= queue->max_size) {
        queue->dropped++;
        ipc_global_stats.total_dropped++;
        return IPC_ERROR_BUFFER_FULL;
    }

    ipc_queue_entry_t *entry = queue_alloc_entry();
    if (!entry) {
        queue->dropped++;
        ipc_global_stats.total_dropped++;
        return IPC_ERROR_OUT_OF_MEMORY;
    }

    /* Copy message to entry */
    memcpy(&entry->message, msg, sizeof(ipc_message_t));

    /* Add to tail of queue */
    entry->next = NULL;
    entry->prev = queue->tail;

    if (queue->tail) {
        queue->tail->next = entry;
    } else {
        queue->head = entry;
    }
    queue->tail = entry;
    queue->count++;

    return IPC_SUCCESS;
}

static ipc_result_t queue_dequeue(ipc_queue_t *queue, ipc_message_t *msg, uint32_t *filter_sender) {
    if (!queue || !msg) {
        return IPC_ERROR_INVALID_ARG;
    }

    if (queue->count == 0 || !queue->head) {
        return IPC_ERROR_NO_MESSAGE;
    }

    ipc_queue_entry_t *entry = NULL;

    if (filter_sender && *filter_sender != IPC_PID_ANY) {
        /* Find message from specific sender */
        entry = queue->head;
        while (entry) {
            if (entry->message.sender_id == *filter_sender) {
                break;
            }
            entry = entry->next;
        }

        if (!entry) {
            return IPC_ERROR_NO_MESSAGE;
        }
    } else {
        /* Get first message */
        entry = queue->head;
    }

    /* Copy message out */
    memcpy(msg, &entry->message, sizeof(ipc_message_t));
    if (filter_sender) {
        *filter_sender = entry->message.sender_id;
    }

    /* Remove from queue */
    if (entry->prev) {
        entry->prev->next = entry->next;
    } else {
        queue->head = entry->next;
    }

    if (entry->next) {
        entry->next->prev = entry->prev;
    } else {
        queue->tail = entry->prev;
    }

    queue->count--;
    queue_free_entry(entry);

    return IPC_SUCCESS;
}

/* ============================================================================
 * Lookup Helpers
 * ============================================================================ */

static ipc_port_t *find_port_by_id(uint32_t port_id) {
    for (uint32_t i = 0; i < MAX_PORTS; i++) {
        if (ports[i].port_id == port_id && ports[i].state != IPC_PORT_CLOSED) {
            return &ports[i];
        }
    }
    return NULL;
}

static ipc_port_t *find_port_by_name(const char *name) {
    for (uint32_t i = 0; i < MAX_PORTS; i++) {
        if (ports[i].state != IPC_PORT_CLOSED && str_equal(ports[i].name, name)) {
            return &ports[i];
        }
    }
    return NULL;
}

static ipc_shared_region_t *find_region(uint32_t region_id) {
    for (uint32_t i = 0; i < MAX_SHARED_REGIONS; i++) {
        if (shared_regions[i].region_id == region_id && shared_regions[i].is_active) {
            return &shared_regions[i];
        }
    }
    return NULL;
}

static ipc_channel_t *find_channel(uint32_t channel_id) {
    for (uint32_t i = 0; i < MAX_CHANNELS; i++) {
        if (channels[i].channel_id == channel_id && channels[i].is_active) {
            return &channels[i];
        }
    }
    return NULL;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

ipc_result_t ipc_init(void) {
    if (ipc_initialized) {
        return IPC_SUCCESS;
    }

    /* Initialize all queues */
    for (uint32_t i = 0; i < MAX_PROCESSES; i++) {
        process_queues[i].head = NULL;
        process_queues[i].tail = NULL;
        process_queues[i].count = 0;
        process_queues[i].max_size = IPC_MAX_QUEUE_SIZE;
        process_queues[i].dropped = 0;
        process_queues[i].state = IPC_PORT_CLOSED;
        queue_initialized[i] = 0;
    }

    /* Initialize ports */
    for (uint32_t i = 0; i < MAX_PORTS; i++) {
        ports[i].port_id = 0;
        ports[i].owner_id = 0;
        ports[i].name[0] = '\0';
        ports[i].state = IPC_PORT_CLOSED;
    }

    /* Initialize shared regions */
    for (uint32_t i = 0; i < MAX_SHARED_REGIONS; i++) {
        shared_regions[i].region_id = 0;
        shared_regions[i].is_active = 0;
    }

    /* Initialize grants */
    for (uint32_t i = 0; i < MAX_SHARED_REGIONS; i++) {
        for (uint32_t j = 0; j < MAX_GRANTS_PER_REGION; j++) {
            region_grants[i][j].is_active = 0;
        }
    }

    /* Initialize channels */
    for (uint32_t i = 0; i < MAX_CHANNELS; i++) {
        channels[i].channel_id = 0;
        channels[i].is_active = 0;
    }

    /* Initialize entry pool */
    for (uint32_t i = 0; i < ENTRY_POOL_SIZE; i++) {
        entry_in_use[i] = 0;
    }

    /* Clear statistics */
    ipc_global_stats.total_sent = 0;
    ipc_global_stats.total_received = 0;
    ipc_global_stats.total_dropped = 0;

    /* Initialize kernel process queue */
    ipc_process_init(IPC_PID_KERNEL);

    ipc_initialized = 1;
    return IPC_SUCCESS;
}

ipc_result_t ipc_process_init(uint32_t pid) {
    if (pid >= MAX_PROCESSES) {
        return IPC_ERROR_INVALID_ARG;
    }

    if (queue_initialized[pid]) {
        return IPC_SUCCESS;
    }

    process_queues[pid].head = NULL;
    process_queues[pid].tail = NULL;
    process_queues[pid].count = 0;
    process_queues[pid].max_size = IPC_MAX_QUEUE_SIZE;
    process_queues[pid].dropped = 0;
    process_queues[pid].state = IPC_PORT_OPEN;
    queue_initialized[pid] = 1;

    return IPC_SUCCESS;
}

ipc_result_t ipc_process_cleanup(uint32_t pid) {
    if (pid >= MAX_PROCESSES) {
        return IPC_ERROR_INVALID_ARG;
    }

    if (!queue_initialized[pid]) {
        return IPC_SUCCESS;
    }

    /* Free all queued messages */
    ipc_queue_t *queue = &process_queues[pid];
    while (queue->head) {
        ipc_queue_entry_t *entry = queue->head;
        queue->head = entry->next;
        queue_free_entry(entry);
    }
    queue->tail = NULL;
    queue->count = 0;
    queue->state = IPC_PORT_CLOSED;
    queue_initialized[pid] = 0;

    /* Cleanup owned ports */
    for (uint32_t i = 0; i < MAX_PORTS; i++) {
        if (ports[i].owner_id == pid && ports[i].state != IPC_PORT_CLOSED) {
            ipc_port_destroy(ports[i].port_id);
        }
    }

    /* Cleanup owned shared regions */
    for (uint32_t i = 0; i < MAX_SHARED_REGIONS; i++) {
        if (shared_regions[i].owner_id == pid && shared_regions[i].is_active) {
            ipc_share_destroy(shared_regions[i].region_id);
        }
    }

    return IPC_SUCCESS;
}

/* ============================================================================
 * Message Passing
 * ============================================================================ */

ipc_result_t ipc_send(uint32_t receiver_id, const ipc_message_t *msg, uint64_t timeout_ns) {
    (void)timeout_ns; /* TODO: Implement timeout */

    if (!ipc_initialized) {
        return IPC_ERROR_NOT_SUPPORTED;
    }

    if (!msg) {
        return IPC_ERROR_INVALID_ARG;
    }

    if (receiver_id >= MAX_PROCESSES) {
        return IPC_ERROR_INVALID_RECEIVER;
    }

    if (!queue_initialized[receiver_id]) {
        return IPC_ERROR_INVALID_RECEIVER;
    }

    if (msg->length > IPC_MAX_MESSAGE_SIZE) {
        return IPC_ERROR_MESSAGE_TOO_LARGE;
    }

    /* Prepare message with sender info */
    ipc_message_t send_msg;
    memcpy(&send_msg, msg, sizeof(ipc_message_t));
    send_msg.sender_id = get_current_pid();
    send_msg.receiver_id = receiver_id;
    send_msg.message_id = next_message_id++;
    send_msg.timestamp = get_timestamp_ns();

    /* Enqueue to receiver */
    ipc_result_t result = queue_enqueue(&process_queues[receiver_id], &send_msg);

    if (result == IPC_SUCCESS) {
        ipc_global_stats.total_sent++;
    }

    return result;
}

ipc_result_t ipc_receive(uint32_t *sender_id, ipc_message_t *msg, uint64_t timeout_ns) {
    (void)timeout_ns; /* TODO: Implement timeout/blocking */

    if (!ipc_initialized) {
        return IPC_ERROR_NOT_SUPPORTED;
    }

    if (!msg) {
        return IPC_ERROR_INVALID_ARG;
    }

    uint32_t pid = get_current_pid();
    if (pid >= MAX_PROCESSES || !queue_initialized[pid]) {
        return IPC_ERROR_INVALID_RECEIVER;
    }

    ipc_result_t result = queue_dequeue(&process_queues[pid], msg, sender_id);

    if (result == IPC_SUCCESS) {
        ipc_global_stats.total_received++;
    }

    return result;
}

ipc_result_t ipc_reply(const ipc_message_t *original_msg, const ipc_message_t *reply) {
    if (!original_msg || !reply) {
        return IPC_ERROR_INVALID_ARG;
    }

    /* Build reply message */
    ipc_message_t reply_msg;
    memcpy(&reply_msg, reply, sizeof(ipc_message_t));
    reply_msg.message_type |= IPC_MSG_REPLY;
    reply_msg.reply_to = original_msg->message_id;

    return ipc_send(original_msg->sender_id, &reply_msg, IPC_NO_TIMEOUT);
}

ipc_result_t ipc_call(uint32_t receiver_id, const ipc_message_t *request,
                      ipc_message_t *reply, uint64_t timeout_ns) {
    if (!request || !reply) {
        return IPC_ERROR_INVALID_ARG;
    }

    /* Send request */
    ipc_result_t result = ipc_send(receiver_id, request, timeout_ns);
    if (result != IPC_SUCCESS) {
        return result;
    }

    /* Wait for reply */
    /* TODO: Implement proper blocking with reply matching */
    uint32_t sender = receiver_id;
    return ipc_receive(&sender, reply, timeout_ns);
}

/* ============================================================================
 * Port Operations
 * ============================================================================ */

ipc_result_t ipc_port_create(const char *name, uint32_t *port_id) {
    if (!ipc_initialized) {
        return IPC_ERROR_NOT_SUPPORTED;
    }

    if (!name || !port_id) {
        return IPC_ERROR_INVALID_ARG;
    }

    /* Check for duplicate name */
    if (find_port_by_name(name)) {
        return IPC_ERROR_ALREADY_EXISTS;
    }

    /* Find free slot */
    ipc_port_t *port = NULL;
    for (uint32_t i = 0; i < MAX_PORTS; i++) {
        if (ports[i].state == IPC_PORT_CLOSED) {
            port = &ports[i];
            break;
        }
    }

    if (!port) {
        return IPC_ERROR_OUT_OF_MEMORY;
    }

    /* Initialize port */
    port->port_id = next_port_id++;
    port->owner_id = get_current_pid();
    str_copy(port->name, name, sizeof(port->name));
    port->state = IPC_PORT_LISTENING;
    port->queue.head = NULL;
    port->queue.tail = NULL;
    port->queue.count = 0;
    port->queue.max_size = IPC_MAX_QUEUE_SIZE;
    port->queue.dropped = 0;
    port->queue.state = IPC_PORT_OPEN;

    *port_id = port->port_id;
    return IPC_SUCCESS;
}

ipc_result_t ipc_port_destroy(uint32_t port_id) {
    ipc_port_t *port = find_port_by_id(port_id);
    if (!port) {
        return IPC_ERROR_INVALID_PORT;
    }

    /* Check ownership */
    if (port->owner_id != get_current_pid() && get_current_pid() != IPC_PID_KERNEL) {
        return IPC_ERROR_PERMISSION_DENIED;
    }

    /* Free queued messages */
    while (port->queue.head) {
        ipc_queue_entry_t *entry = port->queue.head;
        port->queue.head = entry->next;
        queue_free_entry(entry);
    }

    port->state = IPC_PORT_CLOSED;
    port->port_id = 0;
    port->name[0] = '\0';

    return IPC_SUCCESS;
}

ipc_result_t ipc_port_lookup(const char *name, uint32_t *port_id) {
    if (!name || !port_id) {
        return IPC_ERROR_INVALID_ARG;
    }

    ipc_port_t *port = find_port_by_name(name);
    if (!port) {
        return IPC_ERROR_NOT_FOUND;
    }

    *port_id = port->port_id;
    return IPC_SUCCESS;
}

ipc_result_t ipc_port_send(uint32_t port_id, const ipc_message_t *msg) {
    ipc_port_t *port = find_port_by_id(port_id);
    if (!port) {
        return IPC_ERROR_INVALID_PORT;
    }

    if (port->state != IPC_PORT_LISTENING) {
        return IPC_ERROR_PORT_CLOSED;
    }

    /* Prepare message */
    ipc_message_t send_msg;
    memcpy(&send_msg, msg, sizeof(ipc_message_t));
    send_msg.sender_id = get_current_pid();
    send_msg.receiver_id = port->owner_id;
    send_msg.message_id = next_message_id++;
    send_msg.timestamp = get_timestamp_ns();

    ipc_result_t result = queue_enqueue(&port->queue, &send_msg);

    if (result == IPC_SUCCESS) {
        ipc_global_stats.total_sent++;
    }

    return result;
}

ipc_result_t ipc_port_receive(uint32_t port_id, ipc_message_t *msg, uint64_t timeout_ns) {
    (void)timeout_ns;

    ipc_port_t *port = find_port_by_id(port_id);
    if (!port) {
        return IPC_ERROR_INVALID_PORT;
    }

    /* Check ownership */
    if (port->owner_id != get_current_pid()) {
        return IPC_ERROR_PERMISSION_DENIED;
    }

    uint32_t any_sender = IPC_PID_ANY;
    ipc_result_t result = queue_dequeue(&port->queue, msg, &any_sender);

    if (result == IPC_SUCCESS) {
        ipc_global_stats.total_received++;
    }

    return result;
}

/* ============================================================================
 * Shared Memory Operations
 * ============================================================================ */

ipc_result_t ipc_share_create(size_t size, ipc_shared_region_t *region) {
    if (!ipc_initialized) {
        return IPC_ERROR_NOT_SUPPORTED;
    }

    if (!region || size == 0) {
        return IPC_ERROR_INVALID_ARG;
    }

    /* Find free slot */
    ipc_shared_region_t *reg = NULL;
    uint32_t slot = 0;
    for (uint32_t i = 0; i < MAX_SHARED_REGIONS; i++) {
        if (!shared_regions[i].is_active) {
            reg = &shared_regions[i];
            slot = i;
            break;
        }
    }

    if (!reg) {
        return IPC_ERROR_OUT_OF_MEMORY;
    }

    /* Allocate physical memory */
    /* TODO: Integrate with memory manager */
    void *phys = NULL; /* pmm_alloc_pages(size / PAGE_SIZE); */

    reg->region_id = next_region_id++;
    reg->owner_id = get_current_pid();
    reg->physical_addr = phys;
    reg->virtual_addr = phys; /* TODO: Map into owner's address space */
    reg->size = size;
    reg->permissions = IPC_SHARE_READ | IPC_SHARE_WRITE;
    reg->ref_count = 1;
    reg->is_active = 1;

    /* Clear grants for this slot */
    for (uint32_t i = 0; i < MAX_GRANTS_PER_REGION; i++) {
        region_grants[slot][i].is_active = 0;
    }

    memcpy(region, reg, sizeof(ipc_shared_region_t));
    return IPC_SUCCESS;
}

ipc_result_t ipc_share_destroy(uint32_t region_id) {
    ipc_shared_region_t *reg = find_region(region_id);
    if (!reg) {
        return IPC_ERROR_NOT_FOUND;
    }

    /* Check ownership */
    if (reg->owner_id != get_current_pid() && get_current_pid() != IPC_PID_KERNEL) {
        return IPC_ERROR_PERMISSION_DENIED;
    }

    /* Find slot for this region */
    uint32_t slot = (uint32_t)(reg - shared_regions);

    /* Revoke all grants */
    for (uint32_t i = 0; i < MAX_GRANTS_PER_REGION; i++) {
        region_grants[slot][i].is_active = 0;
    }

    /* Free physical memory */
    /* TODO: pmm_free_pages(reg->physical_addr, reg->size / PAGE_SIZE); */

    reg->is_active = 0;
    reg->region_id = 0;

    return IPC_SUCCESS;
}

ipc_result_t ipc_share_grant(uint32_t region_id, uint32_t grantee_id,
                             uint32_t permissions, ipc_region_grant_t *grant) {
    ipc_shared_region_t *reg = find_region(region_id);
    if (!reg) {
        return IPC_ERROR_NOT_FOUND;
    }

    /* Check ownership */
    if (reg->owner_id != get_current_pid()) {
        return IPC_ERROR_PERMISSION_DENIED;
    }

    /* Find slot */
    uint32_t slot = (uint32_t)(reg - shared_regions);

    /* Find free grant slot */
    ipc_region_grant_t *g = NULL;
    for (uint32_t i = 0; i < MAX_GRANTS_PER_REGION; i++) {
        if (!region_grants[slot][i].is_active) {
            g = &region_grants[slot][i];
            break;
        }
        /* Check for existing grant to same process */
        if (region_grants[slot][i].grantee_id == grantee_id) {
            return IPC_ERROR_ALREADY_EXISTS;
        }
    }

    if (!g) {
        return IPC_ERROR_OUT_OF_MEMORY;
    }

    g->region_id = region_id;
    g->grantee_id = grantee_id;
    g->permissions = permissions & reg->permissions; /* Can't exceed owner perms */
    g->mapped_addr = NULL; /* Mapped on ipc_share_map */
    g->is_active = 1;

    reg->ref_count++;

    if (grant) {
        memcpy(grant, g, sizeof(ipc_region_grant_t));
    }

    return IPC_SUCCESS;
}

ipc_result_t ipc_share_revoke(uint32_t region_id, uint32_t grantee_id) {
    ipc_shared_region_t *reg = find_region(region_id);
    if (!reg) {
        return IPC_ERROR_NOT_FOUND;
    }

    /* Check ownership */
    if (reg->owner_id != get_current_pid() && get_current_pid() != IPC_PID_KERNEL) {
        return IPC_ERROR_PERMISSION_DENIED;
    }

    uint32_t slot = (uint32_t)(reg - shared_regions);

    for (uint32_t i = 0; i < MAX_GRANTS_PER_REGION; i++) {
        if (region_grants[slot][i].is_active &&
            region_grants[slot][i].grantee_id == grantee_id) {

            /* Unmap from grantee's address space */
            /* TODO: vmm_unmap(grantee_id, region_grants[slot][i].mapped_addr, reg->size); */

            region_grants[slot][i].is_active = 0;
            reg->ref_count--;
            return IPC_SUCCESS;
        }
    }

    return IPC_ERROR_NOT_FOUND;
}

ipc_result_t ipc_share_map(uint32_t region_id, void **addr) {
    if (!addr) {
        return IPC_ERROR_INVALID_ARG;
    }

    ipc_shared_region_t *reg = find_region(region_id);
    if (!reg) {
        return IPC_ERROR_NOT_FOUND;
    }

    uint32_t pid = get_current_pid();

    /* Owner always has access */
    if (reg->owner_id == pid) {
        *addr = reg->virtual_addr;
        return IPC_SUCCESS;
    }

    /* Check for grant */
    uint32_t slot = (uint32_t)(reg - shared_regions);
    for (uint32_t i = 0; i < MAX_GRANTS_PER_REGION; i++) {
        if (region_grants[slot][i].is_active &&
            region_grants[slot][i].grantee_id == pid) {

            /* Map into grantee's address space */
            /* TODO: void *mapped = vmm_map(pid, reg->physical_addr, reg->size, perms); */
            void *mapped = reg->physical_addr; /* Placeholder */

            region_grants[slot][i].mapped_addr = mapped;
            *addr = mapped;
            return IPC_SUCCESS;
        }
    }

    return IPC_ERROR_PERMISSION_DENIED;
}

ipc_result_t ipc_share_unmap(uint32_t region_id) {
    ipc_shared_region_t *reg = find_region(region_id);
    if (!reg) {
        return IPC_ERROR_NOT_FOUND;
    }

    uint32_t pid = get_current_pid();

    /* Owner unmapping */
    if (reg->owner_id == pid) {
        /* TODO: vmm_unmap(pid, reg->virtual_addr, reg->size); */
        return IPC_SUCCESS;
    }

    /* Grantee unmapping */
    uint32_t slot = (uint32_t)(reg - shared_regions);
    for (uint32_t i = 0; i < MAX_GRANTS_PER_REGION; i++) {
        if (region_grants[slot][i].is_active &&
            region_grants[slot][i].grantee_id == pid) {

            /* TODO: vmm_unmap(pid, region_grants[slot][i].mapped_addr, reg->size); */
            region_grants[slot][i].mapped_addr = NULL;
            return IPC_SUCCESS;
        }
    }

    return IPC_ERROR_PERMISSION_DENIED;
}

/* ============================================================================
 * Channel Operations
 * ============================================================================ */

ipc_result_t ipc_channel_create(uint32_t endpoint_a, uint32_t endpoint_b,
                                uint32_t *channel_id) {
    if (!ipc_initialized) {
        return IPC_ERROR_NOT_SUPPORTED;
    }

    if (!channel_id) {
        return IPC_ERROR_INVALID_ARG;
    }

    if (endpoint_a >= MAX_PROCESSES || endpoint_b >= MAX_PROCESSES) {
        return IPC_ERROR_INVALID_ARG;
    }

    /* Find free slot */
    ipc_channel_t *ch = NULL;
    for (uint32_t i = 0; i < MAX_CHANNELS; i++) {
        if (!channels[i].is_active) {
            ch = &channels[i];
            break;
        }
    }

    if (!ch) {
        return IPC_ERROR_OUT_OF_MEMORY;
    }

    ch->channel_id = next_channel_id++;
    ch->endpoint_a = endpoint_a;
    ch->endpoint_b = endpoint_b;
    ch->is_active = 1;

    /* Initialize queues */
    ch->queue_a_to_b.head = NULL;
    ch->queue_a_to_b.tail = NULL;
    ch->queue_a_to_b.count = 0;
    ch->queue_a_to_b.max_size = IPC_MAX_QUEUE_SIZE;
    ch->queue_a_to_b.dropped = 0;
    ch->queue_a_to_b.state = IPC_PORT_OPEN;

    ch->queue_b_to_a.head = NULL;
    ch->queue_b_to_a.tail = NULL;
    ch->queue_b_to_a.count = 0;
    ch->queue_b_to_a.max_size = IPC_MAX_QUEUE_SIZE;
    ch->queue_b_to_a.dropped = 0;
    ch->queue_b_to_a.state = IPC_PORT_OPEN;

    *channel_id = ch->channel_id;
    return IPC_SUCCESS;
}

ipc_result_t ipc_channel_destroy(uint32_t channel_id) {
    ipc_channel_t *ch = find_channel(channel_id);
    if (!ch) {
        return IPC_ERROR_NOT_FOUND;
    }

    /* Free queued messages */
    while (ch->queue_a_to_b.head) {
        ipc_queue_entry_t *entry = ch->queue_a_to_b.head;
        ch->queue_a_to_b.head = entry->next;
        queue_free_entry(entry);
    }

    while (ch->queue_b_to_a.head) {
        ipc_queue_entry_t *entry = ch->queue_b_to_a.head;
        ch->queue_b_to_a.head = entry->next;
        queue_free_entry(entry);
    }

    ch->is_active = 0;
    ch->channel_id = 0;

    return IPC_SUCCESS;
}

ipc_result_t ipc_channel_send(uint32_t channel_id, const ipc_message_t *msg) {
    ipc_channel_t *ch = find_channel(channel_id);
    if (!ch) {
        return IPC_ERROR_NOT_FOUND;
    }

    uint32_t pid = get_current_pid();
    ipc_queue_t *queue;

    if (pid == ch->endpoint_a) {
        queue = &ch->queue_a_to_b;
    } else if (pid == ch->endpoint_b) {
        queue = &ch->queue_b_to_a;
    } else {
        return IPC_ERROR_PERMISSION_DENIED;
    }

    /* Prepare message */
    ipc_message_t send_msg;
    memcpy(&send_msg, msg, sizeof(ipc_message_t));
    send_msg.sender_id = pid;
    send_msg.receiver_id = (pid == ch->endpoint_a) ? ch->endpoint_b : ch->endpoint_a;
    send_msg.message_id = next_message_id++;
    send_msg.timestamp = get_timestamp_ns();

    ipc_result_t result = queue_enqueue(queue, &send_msg);

    if (result == IPC_SUCCESS) {
        ipc_global_stats.total_sent++;
    }

    return result;
}

ipc_result_t ipc_channel_receive(uint32_t channel_id, ipc_message_t *msg,
                                 uint64_t timeout_ns) {
    (void)timeout_ns;

    ipc_channel_t *ch = find_channel(channel_id);
    if (!ch) {
        return IPC_ERROR_NOT_FOUND;
    }

    uint32_t pid = get_current_pid();
    ipc_queue_t *queue;

    if (pid == ch->endpoint_a) {
        queue = &ch->queue_b_to_a;
    } else if (pid == ch->endpoint_b) {
        queue = &ch->queue_a_to_b;
    } else {
        return IPC_ERROR_PERMISSION_DENIED;
    }

    uint32_t sender = IPC_PID_ANY;
    ipc_result_t result = queue_dequeue(queue, msg, &sender);

    if (result == IPC_SUCCESS) {
        ipc_global_stats.total_received++;
    }

    return result;
}

/* ============================================================================
 * Quantum IPC Extensions
 * ============================================================================ */

ipc_result_t ipc_quantum_circuit_handoff(uint32_t receiver_id, uint32_t circuit_id,
                                         uint64_t coherence_deadline) {
    ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.message_type = IPC_MSG_QUANTUM | IPC_MSG_CIRCUIT_HANDOFF;
    msg.deadline = coherence_deadline;

    /* Pack circuit ID into message data */
    *(uint32_t*)msg.data = circuit_id;
    msg.length = sizeof(uint32_t);

    return ipc_send(receiver_id, &msg, IPC_NO_TIMEOUT);
}

ipc_result_t ipc_quantum_measurement_result(uint32_t receiver_id,
                                            uint32_t measurement_id,
                                            uint8_t result, double probability) {
    ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.message_type = IPC_MSG_QUANTUM;

    /* Pack measurement result into message data */
    struct {
        uint32_t measurement_id;
        uint8_t result;
        double probability;
    } PACKED payload;

    payload.measurement_id = measurement_id;
    payload.result = result;
    payload.probability = probability;

    memcpy(msg.data, &payload, sizeof(payload));
    msg.length = sizeof(payload);

    return ipc_send(receiver_id, &msg, IPC_NO_TIMEOUT);
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

uint32_t ipc_get_queue_depth(void) {
    uint32_t pid = get_current_pid();
    if (pid >= MAX_PROCESSES || !queue_initialized[pid]) {
        return 0;
    }
    return process_queues[pid].count;
}

int ipc_has_messages(void) {
    return ipc_get_queue_depth() > 0 ? 1 : 0;
}

void ipc_get_stats(uint32_t *sent, uint32_t *received, uint32_t *dropped) {
    if (sent) *sent = (uint32_t)ipc_global_stats.total_sent;
    if (received) *received = (uint32_t)ipc_global_stats.total_received;
    if (dropped) *dropped = (uint32_t)ipc_global_stats.total_dropped;
}

const char *ipc_result_string(ipc_result_t result) {
    switch (result) {
        case IPC_SUCCESS:               return "Success";
        case IPC_ERROR_INVALID_RECEIVER: return "Invalid receiver";
        case IPC_ERROR_INVALID_SENDER:   return "Invalid sender";
        case IPC_ERROR_MESSAGE_TOO_LARGE: return "Message too large";
        case IPC_ERROR_PERMISSION_DENIED: return "Permission denied";
        case IPC_ERROR_BUFFER_FULL:      return "Buffer full";
        case IPC_ERROR_TIMEOUT:          return "Timeout";
        case IPC_ERROR_NO_MESSAGE:       return "No message";
        case IPC_ERROR_INVALID_PORT:     return "Invalid port";
        case IPC_ERROR_PORT_CLOSED:      return "Port closed";
        case IPC_ERROR_OUT_OF_MEMORY:    return "Out of memory";
        case IPC_ERROR_INVALID_ARG:      return "Invalid argument";
        case IPC_ERROR_ALREADY_EXISTS:   return "Already exists";
        case IPC_ERROR_NOT_SUPPORTED:    return "Not supported";
        default:                         return "Unknown error";
    }
}
