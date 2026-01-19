/**
 * QuantumOS Process Management System Implementation
 *
 * Process lifecycle management for the QuantumOS microkernel.
 * Implements process creation, scheduling, and destruction with
 * capability-based security integration.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/process.h>
#include <kernel/memory.h>
#include <kernel/ipc.h>
#include <kernel/boot.h>
#include <kernel/types.h>

/* ============================================================================
 * Internal Constants
 * ============================================================================ */

#define PROCESS_STACK_SIZE    8192    /* Default kernel stack size */
#define KERNEL_STACK_BASE     0xFFFF800000000000  /* High half kernel stack */

/* ============================================================================
 * Internal State
 * ============================================================================ */

/* Process table - all processes in the system */
static process_t process_table[MAX_PROCESSES];
static bool process_table_initialized = false;

/* Current running process */
static process_t *current_process = NULL;
static uint32_t current_pid = KERNEL_PROCESS_ID;

/* Next available PID */
static uint32_t next_pid = INIT_PROCESS_ID;

/* Scheduling queues */
static process_t *ready_queue[PRIORITY_KERNEL + 1];
static process_t *current_queue = NULL;

/* Process statistics */
static process_stats_t process_statistics = {0};

/* Kernel process stack */
static uint8_t kernel_stack[PROCESS_STACK_SIZE] ALIGNED(PAGE_SIZE);

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * Find an unused process slot
 */
static uint32_t find_free_pid(void) {
    for (uint32_t i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state == PROCESS_STATE_UNUSED) {
            return i;
        }
    }
    return MAX_PROCESSES; /* No free slots */
}

/**
 * Validate process parameters
 */
static status_t validate_process_params(const process_create_params_t *params) {
    if (!params) {
        return STATUS_INVALID_ARG;
    }
    
    if (!params->name || strlen(params->name) == 0) {
        return STATUS_INVALID_ARG;
    }
    
    if (strlen(params->name) >= PROCESS_NAME_MAX_LEN) {
        return STATUS_INVALID_ARG;
    }
    
    if (params->priority > PRIORITY_KERNEL) {
        return STATUS_INVALID_ARG;
    }
    
    if (params->stack_size == 0) {
        return STATUS_INVALID_ARG;
    }
    
    /* Validate parent process exists (unless it's the kernel) */
    if (params->parent_pid != KERNEL_PROCESS_ID) {
        if (!process_is_valid(params->parent_pid)) {
            return PROCESS_ERROR_INVALID_PARENT;
        }
    }
    
    return STATUS_SUCCESS;
}

/**
 * Initialize process control block
 */
static status_t init_process_pcb(process_t *process, const process_create_params_t *params, uint32_t pid) {
    if (!process || !params) {
        return STATUS_INVALID_ARG;
    }
    
    /* Clear the PCB */
    memset(process, 0, sizeof(process_t));
    
    /* Basic information */
    process->pid = pid;
    process->parent_pid = params->parent_pid;
    strncpy(process->name, params->name, PROCESS_NAME_MAX_LEN - 1);
    process->name[PROCESS_NAME_MAX_LEN - 1] = '\0';
    process->type = params->type;
    process->state = PROCESS_STATE_CREATED;
    process->priority = params->priority;
    
    /* Execution context */
    process->rip = (uint64_t)params->entry_point;
    process->rsp = (uint64_t)params->stack_address + params->stack_size - sizeof(uint64_t);
    process->rbp = process->rsp;
    
    /* Memory management */
    process->memory_size = params->stack_size;
    process->stack_top = params->stack_address;
    process->stack_bottom = process->stack_top;
    
    /* Timing */
    process->creation_time = 0; /* TODO: Get system time */
    process->runtime_total = 0;
    process->runtime_last = 0;
    process->last_scheduled = 0;
    
    /* Quantum awareness */
    process->quantum.is_quantum_aware = params->is_quantum_aware;
    process->quantum.qubit_allocation = 0;
    process->quantum.quantum_runtime = 0;
    
    /* Initialize relationships */
    process->child_count = 0;
    process->has_exited = false;
    process->exit_code = 0;
    
    /* Set magic number */
    process->magic = PROCESS_MAGIC;
    
    return STATUS_SUCCESS;
}

/**
 * Add process to ready queue
 */
static void add_to_ready_queue(process_t *process) {
    if (!process || process->priority > PRIORITY_KERNEL) {
        return;
    }
    
    process->next = ready_queue[process->priority];
    process->prev = NULL;
    
    if (ready_queue[process->priority]) {
        ready_queue[process->priority]->prev = process;
    }
    
    ready_queue[process->priority] = process;
}

/**
 * Remove process from ready queue
 */
static void remove_from_ready_queue(process_t *process) {
    if (!process) {
        return;
    }
    
    if (process->prev) {
        process->prev->next = process->next;
    } else {
        /* Process is at head of queue */
        for (int priority = 0; priority <= PRIORITY_KERNEL; priority++) {
            if (ready_queue[priority] == process) {
                ready_queue[priority] = process->next;
                break;
            }
        }
    }
    
    if (process->next) {
        process->next->prev = process->prev;
    }
    
    process->next = NULL;
    process->prev = NULL;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

/**
 * Initialize process management system
 */
status_t process_init(void) {
    if (process_table_initialized) {
        return STATUS_SUCCESS;
    }
    
    boot_log("Initializing process management system...");
    
    /* Clear process table */
    memset(&process_table, 0, sizeof(process_table));
    
    /* Initialize ready queues */
    for (int i = 0; i <= PRIORITY_KERNEL; i++) {
        ready_queue[i] = NULL;
    }
    
    /* Initialize statistics */
    memset(&process_statistics, 0, sizeof(process_statistics));
    
    /* Create kernel process */
    status_t result = process_init_kernel_process();
    if (result != STATUS_SUCCESS) {
        boot_panic("Failed to create kernel process");
        return result;
    }
    
    /* Create idle process */
    result = process_init_idle_process();
    if (result != STATUS_SUCCESS) {
        boot_panic("Failed to create idle process");
        return result;
    }
    
    current_process = &process_table[KERNEL_PROCESS_ID];
    current_pid = KERNEL_PROCESS_ID;
    
    process_table_initialized = true;
    
    boot_log("Process management system initialized");
    return STATUS_SUCCESS;
}

/**
 * Create a new process
 */
status_t process_create(const process_create_params_t *params, process_t **process) {
    if (!process_table_initialized) {
        return STATUS_ERROR;
    }
    
    status_t result = validate_process_params(params);
    if (result != STATUS_SUCCESS) {
        return result;
    }
    
    /* Find free PID */
    uint32_t pid = find_free_pid();
    if (pid >= MAX_PROCESSES) {
        return PROCESS_ERROR_TOO_MANY_PROCESSES;
    }
    
    /* Allocate memory for process */
    void *stack_memory = NULL;
    if (params->type != PROCESS_TYPE_KERNEL) {
        /* TODO: Allocate from user memory pool */
        stack_memory = (void*)0x400000 + (pid * PROCESS_STACK_SIZE);
    } else {
        stack_memory = &kernel_stack;
    }
    
    /* Initialize PCB */
    result = init_process_pcb(&process_table[pid], params, pid);
    if (result != STATUS_SUCCESS) {
        return result;
    }
    
    /* Set up memory management */
    if (params->type != PROCESS_TYPE_KERNEL) {
        /* TODO: Create page directory for user process */
        process_table[pid].cr3 = 0; /* Physical address of page directory */
    } else {
        process_table[pid].cr3 = 0; /* Use kernel page directory */
    }
    
    /* Add to parent's children list */
    if (params->parent_pid != KERNEL_PROCESS_ID) {
        process_add_child(params->parent_pid, pid);
    }
    
    /* Create IPC message queue */
    ipc_result_t ipc_result = ipc_process_init(pid);
    if (ipc_result != IPC_SUCCESS) {
        /* Clean up */
        process_table[pid].state = PROCESS_STATE_UNUSED;
        return STATUS_ERROR;
    }
    
    /* Update statistics */
    process_statistics.total_processes++;
    process_statistics.active_processes++;
    
    /* Set process state to ready */
    process_table[pid].state = PROCESS_STATE_READY;
    add_to_ready_queue(&process_table[pid]);
    
    *process = &process_table[pid];
    
    boot_log("Created process '%s' with PID %d", params->name, pid);
    return STATUS_SUCCESS;
}

/**
 * Destroy a process
 */
status_t process_destroy(uint32_t pid) {
    if (!process_table_initialized) {
        return STATUS_ERROR;
    }
    
    if (!process_is_valid(pid)) {
        return PROCESS_ERROR_INVALID_PID;
    }
    
    process_t *process = &process_table[pid];
    
    /* Cannot destroy running process */
    if (process == current_process) {
        return PROCESS_ERROR_INVALID_STATE;
    }
    
    /* Remove from ready queue */
    remove_from_ready_queue(process);
    
    /* Clean up IPC resources */
    ipc_process_cleanup(process->pid);
    
    /* Free memory */
    if (process->type != PROCESS_TYPE_KERNEL) {
        /* TODO: Free user memory */
    }
    
    /* Remove from parent's children list */
    if (process->parent_pid != KERNEL_PROCESS_ID) {
        process_remove_child(process->parent_pid, pid);
    }
    
    /* Update statistics */
    if (process->state != PROCESS_STATE_ZOMBIE) {
        process_statistics.active_processes--;
    }
    
    /* Mark process as unused */
    process->state = PROCESS_STATE_UNUSED;
    process->magic = 0;
    
    boot_log("Destroyed process with PID %d", pid);
    return STATUS_SUCCESS;
}

/**
 * Exit a process
 */
status_t process_exit(uint32_t pid, int32_t exit_code) {
    if (!process_table_initialized) {
        return STATUS_ERROR;
    }
    
    if (!process_is_valid(pid)) {
        return PROCESS_ERROR_INVALID_PID;
    }
    
    process_t *process = &process_table[pid];
    
    /* Set exit information */
    process->exit_code = exit_code;
    process->has_exited = true;
    
    /* Change state to zombie */
    process_set_state(pid, PROCESS_STATE_ZOMBIE);
    
    /* Remove from ready queue */
    remove_from_ready_queue(process);
    
    /* Update statistics */
    process_statistics.active_processes--;
    process_statistics.zombie_processes++;
    
    boot_log("Process %d exited with code %d", pid, exit_code);
    return STATUS_SUCCESS;
}

/**
 * Set process state
 */
status_t process_set_state(uint32_t pid, process_state_t new_state) {
    if (!process_is_valid(pid)) {
        return PROCESS_ERROR_INVALID_PID;
    }
    
    process_t *process = &process_table[pid];
    process_state_t old_state = process->state;
    
    /* Handle queue transitions */
    if (old_state == PROCESS_STATE_READY) {
        remove_from_ready_queue(process);
    }
    
    process->state = new_state;
    
    if (new_state == PROCESS_STATE_READY) {
        add_to_ready_queue(process);
    }
    
    return STATUS_SUCCESS;
}

/**
 * Get process state
 */
process_state_t process_get_state(uint32_t pid) {
    if (!process_is_valid(pid)) {
        return PROCESS_STATE_UNUSED;
    }
    
    return process_table[pid].state;
}

/**
 * Get process by PID
 */
process_t *process_get_by_pid(uint32_t pid) {
    if (!process_is_valid(pid)) {
        return NULL;
    }
    
    return &process_table[pid];
}

/**
 * Get current process
 */
process_t *process_get_current(void) {
    return current_process;
}

/**
 * Get next ready process for scheduling
 */
process_t *process_get_next_ready(void) {
    /* Find highest priority ready process */
    for (int priority = PRIORITY_KERNEL; priority >= 0; priority--) {
        if (ready_queue[priority]) {
            return ready_queue[priority];
        }
    }
    
    /* No ready processes, return idle process */
    return &process_table[KERNEL_PROCESS_ID + 1]; /* Assume PID 1 is idle */
}

/**
 * Schedule next process
 */
status_t process_schedule_next(void) {
    if (!process_table_initialized) {
        return STATUS_ERROR;
    }
    
    process_t *next = process_get_next_ready();
    if (!next) {
        return STATUS_ERROR;
    }
    
    if (next == current_process) {
        return STATUS_SUCCESS; /* Already running */
    }
    
    return process_switch_to(next);
}

/**
 * Switch to specific process
 */
status_t process_switch_to(process_t *process) {
    if (!process) {
        return STATUS_INVALID_ARG;
    }
    
    if (!process_is_valid(process->pid)) {
        return PROCESS_ERROR_INVALID_PID;
    }
    
    /* TODO: Implement actual context switch */
    /* This would involve:
     1. Save current process context
     2. Switch page tables (CR3)
     3. Restore new process context
     4. Update current_process pointer
    */
    
    process_t *old_process = current_process;
    current_process = process;
    current_pid = process->pid;
    
    /* Update statistics */
    process_statistics.context_switches++;
    
    /* Update timing */
    uint64_t now = 0; /* TODO: Get system time */
    if (old_process) {
        old_process->runtime_last = now - old_process->last_scheduled;
        old_process->runtime_total += old_process->runtime_last;
    }
    process->last_scheduled = now;
    
    return STATUS_SUCCESS;
}

/**
 * Check if process is valid
 */
bool process_is_valid(uint32_t pid) {
    if (pid >= MAX_PROCESSES) {
        return false;
    }
    
    process_t *process = &process_table[pid];
    return (process->magic == PROCESS_MAGIC && 
            process->state != PROCESS_STATE_UNUSED);
}

/**
 * Add child to parent
 */
status_t process_add_child(uint32_t parent_pid, uint32_t child_pid) {
    if (!process_is_valid(parent_pid) || !process_is_valid(child_pid)) {
        return PROCESS_ERROR_INVALID_PID;
    }
    
    process_t *parent = &process_table[parent_pid];
    
    if (parent->child_count >= MAX_PROCESSES) {
        return STATUS_ERROR;
    }
    
    parent->children[parent->child_count++] = child_pid;
    return STATUS_SUCCESS;
}

/**
 * Remove child from parent
 */
status_t process_remove_child(uint32_t parent_pid, uint32_t child_pid) {
    if (!process_is_valid(parent_pid) || !process_is_valid(child_pid)) {
        return PROCESS_ERROR_INVALID_PID;
    }
    
    process_t *parent = &process_table[parent_pid];
    
    /* Find and remove child */
    for (uint32_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child_pid) {
            /* Shift remaining children */
            for (uint32_t j = i; j < parent->child_count - 1; j++) {
                parent->children[j] = parent->children[j + 1];
            }
            parent->child_count--;
            return STATUS_SUCCESS;
        }
    }
    
    return PROCESS_ERROR_NOT_FOUND;
}

/**
 * Initialize kernel process
 */
status_t process_init_kernel_process(void) {
    process_create_params_t params = {
        .name = "kernel",
        .type = PROCESS_TYPE_KERNEL,
        .priority = PRIORITY_KERNEL,
        .parent_pid = KERNEL_PROCESS_ID,
        .entry_point = (void*)0xFFFFFFFF80000000UL, /* Kernel entry */
        .stack_address = &kernel_stack,
        .stack_size = PROCESS_STACK_SIZE,
        .is_quantum_aware = true
    };
    
    process_t *kernel_process;
    status_t result = process_create(&params, &kernel_process);
    if (result != STATUS_SUCCESS) {
        return result;
    }
    
    /* Set kernel process as running */
    kernel_process->state = PROCESS_STATE_RUNNING;
    
    return STATUS_SUCCESS;
}

/**
 * Initialize idle process
 */
status_t process_init_idle_process(void) {
    process_create_params_t params = {
        .name = "idle",
        .type = PROCESS_TYPE_KERNEL,
        .priority = PRIORITY_IDLE,
        .parent_pid = KERNEL_PROCESS_ID,
        .entry_point = (void*)process_idle_task,
        .stack_address = (void*)(KERNEL_STACK_BASE + PROCESS_STACK_SIZE),
        .stack_size = PROCESS_STACK_SIZE,
        .is_quantum_aware = false
    };
    
    process_t *idle_process;
    return process_create(&params, &idle_process);
}

/**
 * Idle task - runs when no other processes are ready
 */
void process_idle_task(void) {
    while (1) {
        __asm__ volatile("hlt");
    }
}

/**
 * Get process statistics
 */
status_t process_get_stats(process_stats_t *stats) {
    if (!stats) {
        return STATUS_INVALID_ARG;
    }
    
    *stats = process_statistics;
    return STATUS_SUCCESS;
}

/**
 * Dump process information for debugging
 */
void process_dump_info(uint32_t pid) {
    if (!process_is_valid(pid)) {
        boot_log("Invalid PID: %d", pid);
        return;
    }
    
    process_t *process = &process_table[pid];
    
    boot_log("=== Process %d ===", pid);
    boot_log("Name: %s", process->name);
    boot_log("Type: %d, State: %d, Priority: %d", 
              process->type, process->state, process->priority);
    boot_log("Parent: %d, Children: %d", process->parent_pid, process->child_count);
    boot_log("RIP: 0x%016lx, RSP: 0x%016lx", process->rip, process->rsp);
    boot_log("Runtime: %lld ns", process->runtime_total);
    boot_log("Quantum aware: %s", process->quantum.is_quantum_aware ? "yes" : "no");
}

/**
 * Dump all processes
 */
void process_dump_all(void) {
    boot_log("=== Process Table ===");
    boot_log("Total: %d, Active: %d, Zombies: %d", 
              process_statistics.total_processes,
              process_statistics.active_processes,
              process_statistics.zombie_processes);
    
    for (uint32_t i = 0; i < MAX_PROCESSES; i++) {
        if (process_is_valid(i)) {
            process_dump_info(i);
        }
    }
}
