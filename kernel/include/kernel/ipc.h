/**
 * QuantumOS IPC System Header
 *
 * Inter-Process Communication using message passing.
 * Implements the microkernel IPC architecture defined in MICROKERNEL_DESIGN.md
 *
 * Features:
 * - Synchronous and asynchronous message passing
 * - Zero-copy shared memory regions for large data
 * - Time-bounded delivery guarantees
 * - Quantum-safe channels (no side-channel leakage)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef IPC_H
#define IPC_H

#include <kernel/types.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define IPC_MAX_MESSAGE_SIZE    4096    /* Maximum message payload size */
#define IPC_MAX_QUEUE_SIZE      64      /* Maximum messages in queue */
#define IPC_DEFAULT_TIMEOUT_NS  1000000000ULL  /* 1 second default timeout */
#define IPC_NO_TIMEOUT          0       /* Blocking wait forever */
#define IPC_NO_WAIT             1       /* Non-blocking */

/* Message types */
#define IPC_MSG_NORMAL          0x0000  /* Standard message */
#define IPC_MSG_URGENT          0x0001  /* High priority */
#define IPC_MSG_REPLY           0x0002  /* Reply to previous message */
#define IPC_MSG_NOTIFICATION    0x0004  /* Async notification */
#define IPC_MSG_QUANTUM         0x0008  /* Quantum-related message */
#define IPC_MSG_CIRCUIT_HANDOFF 0x0010  /* Quantum circuit transfer */

/* Shared region permissions */
#define IPC_SHARE_READ          0x01
#define IPC_SHARE_WRITE         0x02
#define IPC_SHARE_EXEC          0x04

/* Port states */
#define IPC_PORT_CLOSED         0
#define IPC_PORT_OPEN           1
#define IPC_PORT_LISTENING      2

/* Special process IDs */
#define IPC_PID_KERNEL          0
#define IPC_PID_ANY             0xFFFFFFFF
#define IPC_PID_INVALID         0xFFFFFFFE

/* ============================================================================
 * Result Codes
 * ============================================================================ */

typedef enum {
    IPC_SUCCESS = 0,
    IPC_ERROR_INVALID_RECEIVER = -1,
    IPC_ERROR_INVALID_SENDER = -2,
    IPC_ERROR_MESSAGE_TOO_LARGE = -3,
    IPC_ERROR_PERMISSION_DENIED = -4,
    IPC_ERROR_BUFFER_FULL = -5,
    IPC_ERROR_TIMEOUT = -6,
    IPC_ERROR_NO_MESSAGE = -7,
    IPC_ERROR_INVALID_PORT = -8,
    IPC_ERROR_PORT_CLOSED = -9,
    IPC_ERROR_OUT_OF_MEMORY = -10,
    IPC_ERROR_INVALID_ARG = -11,
    IPC_ERROR_ALREADY_EXISTS = -12,
    IPC_ERROR_NOT_SUPPORTED = -13,
    IPC_ERROR_NOT_FOUND = -14
} ipc_result_t;

/* ============================================================================
 * Core Data Structures
 * ============================================================================ */

/**
 * IPC Message Structure
 *
 * Fixed-size header with variable payload.
 * Maximum payload is IPC_MAX_MESSAGE_SIZE bytes.
 */
typedef struct {
    uint32_t sender_id;         /* Process ID of sender */
    uint32_t receiver_id;       /* Process ID of receiver */
    uint32_t message_type;      /* Message type flags */
    uint32_t message_id;        /* Unique message identifier */
    uint32_t reply_to;          /* Message ID this replies to (0 if none) */
    uint32_t length;            /* Payload length in bytes */
    uint64_t timestamp;         /* Send timestamp (ns since boot) */
    uint64_t deadline;          /* Delivery deadline (0 = no deadline) */
    uint8_t  data[IPC_MAX_MESSAGE_SIZE];  /* Message payload */
} PACKED ipc_message_t;

/**
 * Message Queue Entry
 *
 * Linked list node for the per-process message queue.
 */
typedef struct ipc_queue_entry {
    ipc_message_t message;
    struct ipc_queue_entry *next;
    struct ipc_queue_entry *prev;
} ipc_queue_entry_t;

/**
 * Process Message Queue
 *
 * Per-process queue for incoming messages.
 */
typedef struct {
    ipc_queue_entry_t *head;    /* First message in queue */
    ipc_queue_entry_t *tail;    /* Last message in queue */
    uint32_t count;             /* Number of messages in queue */
    uint32_t max_size;          /* Maximum queue size */
    uint32_t dropped;           /* Count of dropped messages */
    uint8_t state;              /* Queue state */
} ipc_queue_t;

/**
 * IPC Port
 *
 * Named communication endpoint for services.
 */
typedef struct {
    uint32_t port_id;           /* Unique port identifier */
    uint32_t owner_id;          /* Owning process ID */
    char name[64];              /* Human-readable port name */
    uint8_t state;              /* Port state */
    ipc_queue_t queue;          /* Message queue for this port */
} ipc_port_t;

/**
 * Shared Memory Region
 *
 * Zero-copy memory region for large data transfers.
 */
typedef struct {
    uint32_t region_id;         /* Unique region identifier */
    uint32_t owner_id;          /* Creating process */
    void *physical_addr;        /* Physical memory address */
    void *virtual_addr;         /* Virtual address in owner's space */
    size_t size;                /* Region size in bytes */
    uint32_t permissions;       /* Access permissions */
    uint32_t ref_count;         /* Number of processes with access */
    uint8_t is_active;          /* Region is active */
} ipc_shared_region_t;

/**
 * Shared Region Grant
 *
 * Records a grant of access to a shared region.
 */
typedef struct {
    uint32_t region_id;         /* Shared region ID */
    uint32_t grantee_id;        /* Process granted access */
    void *mapped_addr;          /* Address in grantee's space */
    uint32_t permissions;       /* Granted permissions */
    uint8_t is_active;          /* Grant is active */
} ipc_region_grant_t;

/**
 * IPC Channel
 *
 * Bidirectional communication channel between two processes.
 */
typedef struct {
    uint32_t channel_id;        /* Unique channel identifier */
    uint32_t endpoint_a;        /* First process ID */
    uint32_t endpoint_b;        /* Second process ID */
    ipc_queue_t queue_a_to_b;   /* Messages from A to B */
    ipc_queue_t queue_b_to_a;   /* Messages from B to A */
    uint8_t is_active;          /* Channel is active */
} ipc_channel_t;

/* ============================================================================
 * IPC System Initialization
 * ============================================================================ */

/**
 * Initialize the IPC subsystem
 *
 * Must be called during kernel initialization before any IPC operations.
 *
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_init(void);

/**
 * Initialize IPC for a process
 *
 * Called when a new process is created to set up its message queue.
 *
 * @param pid Process ID
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_process_init(uint32_t pid);

/**
 * Cleanup IPC for a process
 *
 * Called when a process terminates to free IPC resources.
 *
 * @param pid Process ID
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_process_cleanup(uint32_t pid);

/* ============================================================================
 * Message Passing Operations
 * ============================================================================ */

/**
 * Send a message to a process
 *
 * Sends a message to the specified receiver. Can be blocking or non-blocking
 * depending on the timeout value.
 *
 * @param receiver_id Target process ID
 * @param msg Message to send
 * @param timeout_ns Timeout in nanoseconds (0 = block, 1 = no wait)
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_send(uint32_t receiver_id, const ipc_message_t *msg, uint64_t timeout_ns);

/**
 * Receive a message
 *
 * Receives a message from any sender or a specific sender.
 *
 * @param sender_id Pointer to store sender ID (or filter if not IPC_PID_ANY)
 * @param msg Buffer to receive message
 * @param timeout_ns Timeout in nanoseconds
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_receive(uint32_t *sender_id, ipc_message_t *msg, uint64_t timeout_ns);

/**
 * Send a reply to a message
 *
 * Sends a reply to a previously received message.
 *
 * @param original_msg The message being replied to
 * @param reply The reply message
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_reply(const ipc_message_t *original_msg, const ipc_message_t *reply);

/**
 * Send and wait for reply (call semantics)
 *
 * Synchronously sends a message and waits for a reply.
 *
 * @param receiver_id Target process ID
 * @param request Request message
 * @param reply Buffer for reply message
 * @param timeout_ns Timeout in nanoseconds
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_call(uint32_t receiver_id, const ipc_message_t *request,
                      ipc_message_t *reply, uint64_t timeout_ns);

/* ============================================================================
 * Port Operations
 * ============================================================================ */

/**
 * Create a named port
 *
 * Creates a named port for receiving messages.
 *
 * @param name Port name (max 63 characters)
 * @param port_id Pointer to store created port ID
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_port_create(const char *name, uint32_t *port_id);

/**
 * Destroy a port
 *
 * @param port_id Port to destroy
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_port_destroy(uint32_t port_id);

/**
 * Look up a port by name
 *
 * @param name Port name to find
 * @param port_id Pointer to store found port ID
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_port_lookup(const char *name, uint32_t *port_id);

/**
 * Send a message to a port
 *
 * @param port_id Target port
 * @param msg Message to send
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_port_send(uint32_t port_id, const ipc_message_t *msg);

/**
 * Receive from a port
 *
 * @param port_id Port to receive from
 * @param msg Buffer for message
 * @param timeout_ns Timeout in nanoseconds
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_port_receive(uint32_t port_id, ipc_message_t *msg, uint64_t timeout_ns);

/* ============================================================================
 * Zero-Copy Shared Memory Operations
 * ============================================================================ */

/**
 * Create a shared memory region
 *
 * Creates a new shared memory region for zero-copy transfers.
 *
 * @param size Size of region in bytes
 * @param region Pointer to store region info
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_share_create(size_t size, ipc_shared_region_t *region);

/**
 * Destroy a shared memory region
 *
 * Frees a shared region. All grants must be revoked first.
 *
 * @param region_id Region to destroy
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_share_destroy(uint32_t region_id);

/**
 * Grant access to a shared region
 *
 * Grants another process access to a shared memory region.
 *
 * @param region_id Region to share
 * @param grantee_id Process to grant access to
 * @param permissions Access permissions
 * @param grant Pointer to store grant info
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_share_grant(uint32_t region_id, uint32_t grantee_id,
                             uint32_t permissions, ipc_region_grant_t *grant);

/**
 * Revoke access to a shared region
 *
 * Revokes a previously granted access.
 *
 * @param region_id Region ID
 * @param grantee_id Process to revoke from
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_share_revoke(uint32_t region_id, uint32_t grantee_id);

/**
 * Map a shared region into current process
 *
 * Maps a granted shared region into the calling process's address space.
 *
 * @param region_id Region to map
 * @param addr Pointer to store mapped address
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_share_map(uint32_t region_id, void **addr);

/**
 * Unmap a shared region
 *
 * @param region_id Region to unmap
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_share_unmap(uint32_t region_id);

/* ============================================================================
 * Channel Operations
 * ============================================================================ */

/**
 * Create a bidirectional channel
 *
 * Creates a dedicated channel between two processes.
 *
 * @param endpoint_a First process ID
 * @param endpoint_b Second process ID
 * @param channel_id Pointer to store channel ID
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_channel_create(uint32_t endpoint_a, uint32_t endpoint_b,
                                uint32_t *channel_id);

/**
 * Destroy a channel
 *
 * @param channel_id Channel to destroy
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_channel_destroy(uint32_t channel_id);

/**
 * Send on a channel
 *
 * @param channel_id Channel to send on
 * @param msg Message to send
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_channel_send(uint32_t channel_id, const ipc_message_t *msg);

/**
 * Receive from a channel
 *
 * @param channel_id Channel to receive from
 * @param msg Buffer for message
 * @param timeout_ns Timeout in nanoseconds
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_channel_receive(uint32_t channel_id, ipc_message_t *msg,
                                 uint64_t timeout_ns);

/* ============================================================================
 * Quantum IPC Extensions
 * ============================================================================ */

/**
 * Send quantum circuit handoff
 *
 * Transfers a quantum circuit to another process for execution.
 *
 * @param receiver_id Target process
 * @param circuit_id Circuit to transfer
 * @param coherence_deadline Deadline for execution
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_quantum_circuit_handoff(uint32_t receiver_id, uint32_t circuit_id,
                                         uint64_t coherence_deadline);

/**
 * Send measurement result
 *
 * Propagates quantum measurement results to interested processes.
 *
 * @param receiver_id Target process
 * @param measurement_id Measurement event ID
 * @param result Measurement result (0 or 1)
 * @param probability Result probability
 * @return IPC_SUCCESS on success, error code otherwise
 */
ipc_result_t ipc_quantum_measurement_result(uint32_t receiver_id,
                                            uint32_t measurement_id,
                                            uint8_t result, double probability);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * Get current process's message queue depth
 *
 * @return Number of pending messages
 */
uint32_t ipc_get_queue_depth(void);

/**
 * Check if messages are waiting
 *
 * @return 1 if messages waiting, 0 otherwise
 */
int ipc_has_messages(void);

/**
 * Get IPC statistics for current process
 *
 * @param sent Pointer to store sent count
 * @param received Pointer to store received count
 * @param dropped Pointer to store dropped count
 */
void ipc_get_stats(uint32_t *sent, uint32_t *received, uint32_t *dropped);

/**
 * Get string description of IPC result code
 *
 * @param result Result code
 * @return String description
 */
const char *ipc_result_string(ipc_result_t result);

#endif /* IPC_H */
