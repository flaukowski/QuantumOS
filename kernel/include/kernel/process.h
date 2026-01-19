/**
 * QuantumOS Process Management System
 *
 * Process lifecycle management for the QuantumOS microkernel.
 * Implements process creation, scheduling, and destruction with
 * capability-based security integration.
 *
 * Features:
 * - Process creation and destruction
 * - Process state management
 * - Capability-based process isolation
 * - Integration with IPC system
 * - Support for quantum-aware processes
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PROCESS_H
#define PROCESS_H

#include <kernel/types.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define MAX_PROCESSES              256     /* Maximum concurrent processes */
#define MAX_THREADS_PER_PROCESS    16      /* Maximum threads per process */
#define PROCESS_NAME_MAX_LEN       64      /* Maximum process name length */
#define KERNEL_PROCESS_ID          0       /* Reserved for kernel process */
#define INIT_PROCESS_ID            1       /* First user process */

/* Process priorities */
#define PRIORITY_IDLE              0       /* Lowest priority */
#define PRIORITY_LOW               1       /* Low priority */
#define PRIORITY_NORMAL            2       /* Normal priority */
#define PRIORITY_HIGH              3       /* High priority */
#define PRIORITY_REALTIME          4       /* Real-time priority */
#define PRIORITY_KERNEL            5       /* Kernel priority (highest) */

/* Process states */
typedef enum {
    PROCESS_STATE_UNUSED = 0,      /* Process slot not used */
    PROCESS_STATE_CREATED,         /* Process created but not runnable */
    PROCESS_STATE_READY,           /* Process ready to run */
    PROCESS_STATE_RUNNING,         /* Process currently running */
    PROCESS_STATE_BLOCKED,         /* Process blocked (waiting for I/O, etc.) */
    PROCESS_STATE_TERMINATED,      /* Process terminated but not cleaned up */
    PROCESS_STATE_ZOMBIE           /* Process terminated, waiting for parent */
} process_state_t;

/* Process types */
typedef enum {
    PROCESS_TYPE_KERNEL = 0,       /* Kernel process */
    PROCESS_TYPE_USER,             /* Regular user process */
    PROCESS_TYPE_SERVICE,          /* System service */
    PROCESS_TYPE_QUANTUM           /* Quantum-aware process */
} process_type_t;

/* ============================================================================
 * Data Structures
 * ============================================================================ */

/**
 * Process Control Block (PCB)
 * 
 * Contains all information about a process. This is the core data structure
 * for process management in QuantumOS.
 */
typedef struct process {
    /* Basic process information */
    uint32_t pid;                  /* Process ID */
    uint32_t parent_pid;           /* Parent process ID */
    char name[PROCESS_NAME_MAX_LEN]; /* Process name */
    process_type_t type;           /* Process type */
    process_state_t state;         /* Current process state */
    uint8_t priority;              /* Process priority */
    
    /* Execution context */
    uint64_t rip;                  /* Instruction pointer */
    uint64_t rsp;                  /* Stack pointer */
    uint64_t rbp;                  /* Base pointer */
    uint64_t cr3;                  /* Page table physical address */
    
    /* Memory management */
    void *virtual_address_space;   /* Virtual memory root */
    size_t memory_size;            /* Total memory allocated */
    void *stack_top;               /* Top of kernel stack */
    void *stack_bottom;            /* Bottom of kernel stack */
    
    /* Process timing */
    uint64_t creation_time;        /* When process was created */
    uint64_t runtime_total;        /* Total runtime in nanoseconds */
    uint64_t runtime_last;         /* Runtime of last quantum */
    uint64_t last_scheduled;       /* Last time process was scheduled */
    
    /* IPC integration */
    uint32_t message_queue_id;     /* IPC message queue ID */
    uint32_t port_count;           /* Number of owned IPC ports */
    
    /* Capability security */
    uint32_t capability_root;      /* Root capability for this process */
    uint32_t capability_count;     /* Number of held capabilities */
    
    /* Process relationships */
    uint32_t children[MAX_PROCESSES]; /* Child process PIDs */
    uint32_t child_count;          /* Number of children */
    
    /* Exit information */
    int32_t exit_code;             /* Process exit code */
    bool has_exited;               /* True if process has exited */
    
    /* Quantum-specific fields (for quantum-aware processes) */
    struct {
        bool is_quantum_aware;     /* Process can use quantum resources */
        uint32_t qubit_allocation; /* Allocated qubits */
        uint64_t quantum_runtime; /* Time spent on quantum operations */
    } quantum;
    
    /* Internal kernel fields */
    uint32_t magic;                /* Magic number for validation */
    struct process *next;          /* Next process in scheduling queue */
    struct process *prev;          /* Previous process in scheduling queue */
} process_t;

/**
 * Process creation parameters
 */
typedef struct {
    const char *name;              /* Process name */
    process_type_t type;           /* Process type */
    uint8_t priority;              /* Initial priority */
    uint32_t parent_pid;           /* Parent process ID (0 for kernel) */
    void *entry_point;             /* Process entry point */
    void *stack_address;           /* Stack base address */
    size_t stack_size;             /* Stack size */
    bool is_quantum_aware;         /* Quantum-aware process flag */
} process_create_params_t;

/**
 * Process statistics
 */
typedef struct {
    uint32_t total_processes;      /* Total processes created */
    uint32_t active_processes;     /* Currently active processes */
    uint32_t zombie_processes;     /* Zombie processes */
    uint64_t total_runtime;        /* Total runtime of all processes */
    uint64_t context_switches;     /* Number of context switches */
} process_stats_t;

/* ============================================================================
 * Function Declarations
 * ============================================================================ */

/* Process management initialization */
status_t process_init(void);

/* Process creation and destruction */
status_t process_create(const process_create_params_t *params, process_t **process);
status_t process_destroy(uint32_t pid);
status_t process_exit(uint32_t pid, int32_t exit_code);
status_t process_kill(uint32_t pid, int32_t signal);

/* Process state management */
status_t process_set_state(uint32_t pid, process_state_t new_state);
process_state_t process_get_state(uint32_t pid);
status_t process_block(uint32_t pid);
status_t process_unblock(uint32_t pid);

/* Process information */
process_t *process_get_by_pid(uint32_t pid);
process_t *process_get_current(void);
uint32_t process_get_pid(process_t *process);
const char *process_get_name(uint32_t pid);
process_type_t process_get_type(uint32_t pid);

/* Process scheduling interface */
status_t process_schedule_next(void);
status_t process_switch_to(process_t *process);
process_t *process_get_next_ready(void);

/* Process relationships */
status_t process_add_child(uint32_t parent_pid, uint32_t child_pid);
status_t process_remove_child(uint32_t parent_pid, uint32_t child_pid);
uint32_t process_get_parent(uint32_t pid);

/* Process validation */
bool process_is_valid(uint32_t pid);
bool process_is_ready(uint32_t pid);
bool process_is_running(uint32_t pid);
bool process_is_terminated(uint32_t pid);

/* Process statistics */
status_t process_get_stats(process_stats_t *stats);
status_t process_reset_stats(void);

/* Quantum process support */
status_t process_set_quantum_aware(uint32_t pid, bool aware);
bool process_is_quantum_aware(uint32_t pid);
status_t process_allocate_qubits(uint32_t pid, uint32_t count);
status_t process_deallocate_qubits(uint32_t pid, uint32_t count);

/* Debug and diagnostics */
void process_dump_info(uint32_t pid);
void process_dump_all(void);
void process_dump_scheduler_queue(void);

/* Internal kernel functions */
void process_idle_task(void);
status_t process_init_kernel_process(void);
status_t process_init_idle_process(void);

/* ============================================================================
 * Constants and Macros
 * ============================================================================ */

/* Process magic number for validation */
#define PROCESS_MAGIC 0x50524F43  /* "PROC" */

/* Process flags */
#define PROCESS_FLAG_KERNEL        (1 << 0)  /* Kernel process */
#define PROCESS_FLAG_SYSTEM        (1 << 1)  /* System process */
#define PROCESS_FLAG_QUANTUM       (1 << 2)  /* Quantum-aware */
#define PROCESS_FLAG_PRIVILEGED    (1 << 3)  /* Privileged process */

/* Process error codes */
#define PROCESS_ERROR_INVALID_PID      -1001
#define PROCESS_ERROR_ALREADY_EXISTS   -1002
#define PROCESS_ERROR_NOT_FOUND        -1003
#define PROCESS_ERROR_INVALID_STATE   -1004
#define PROCESS_ERROR_PERMISSION_DENIED -1005
#define PROCESS_ERROR_NO_MEMORY        -1006
#define PROCESS_ERROR_TOO_MANY_PROCESSES -1007
#define PROCESS_ERROR_INVALID_PARENT   -1008

/* Convenience macros */
#define PROCESS_IS_KERNEL(p)     ((p)->type == PROCESS_TYPE_KERNEL)
#define PROCESS_IS_USER(p)       ((p)->type == PROCESS_TYPE_USER)
#define PROCESS_IS_SERVICE(p)    ((p)->type == PROCESS_TYPE_SERVICE)
#define PROCESS_IS_QUANTUM(p)     ((p)->type == PROCESS_TYPE_QUANTUM)
#define PROCESS_IS_ALIVE(p)      ((p)->state != PROCESS_STATE_UNUSED && \
                                 (p)->state != PROCESS_STATE_TERMINATED && \
                                 (p)->state != PROCESS_STATE_ZOMBIE)

#endif /* PROCESS_H */
