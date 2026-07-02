/**
 * QuantumOS Preemptive Scheduler Implementation
 *
 * Round-robin over the process table, driven by the PIT timer tick.
 * The context switch happens on the interrupt frame: irq_common saves
 * the full cpu_state_t on the stack and passes it to the C handler;
 * writing a different process's saved state onto that frame makes the
 * final iretq resume that process.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/scheduler.h>
#include <kernel/process.h>
#include <kernel/memory.h>
#include <kernel/boot.h>

static uint64_t switch_count = 0;
static uint32_t quantum_counter = 0;

/* PID of the idle process (created second in process_init) */
#define IDLE_PROCESS_ID (KERNEL_PROCESS_ID + 1)

/**
 * Round-robin pick: scan the table starting after the current PID for a
 * READY process with a runnable context. The idle process is only
 * chosen when nothing else is ready.
 */
static process_t *pick_next(uint32_t from_pid) {
    process_t *idle = NULL;

    for (uint32_t off = 1; off <= MAX_PROCESSES; off++) {
        uint32_t pid = (from_pid + off) % MAX_PROCESSES;
        process_t *p = process_get_by_pid(pid);
        if (!p || p->state != PROCESS_STATE_READY) {
            continue;
        }
        if (!p->context_valid) {
            continue; /* nothing runnable saved yet */
        }
        if (pid == IDLE_PROCESS_ID) {
            idle = p;
            continue;
        }
        return p;
    }

    return idle;
}

void scheduler_tick(cpu_state_t *state) {
    if (++quantum_counter < SCHED_QUANTUM_TICKS) {
        return;
    }
    quantum_counter = 0;

    process_t *cur = process_get_current();
    if (!cur) {
        return;
    }

    process_t *next = pick_next(cur->pid);
    if (!next || next == cur) {
        return;
    }

    /* Save the interrupted context into the current PCB */
    cur->context = *state;
    cur->context_valid = true;
    cur->rip = state->rip;
    cur->rsp = state->rsp;
    if (cur->state == PROCESS_STATE_RUNNING) {
        process_set_state(cur->pid, PROCESS_STATE_READY);
    }

    /* Bookkeeping: current_process pointer, statistics */
    process_switch_to(next);
    process_set_state(next->pid, PROCESS_STATE_RUNNING);

    /* Restore the next context onto the interrupt frame; the iretq at
     * the end of irq_common resumes it */
    *state = next->context;

    switch_count++;
    if (switch_count == 1) {
        boot_log("scheduler: first context switch");
    }
}

uint64_t scheduler_get_switches(void) {
    return switch_count;
}

status_t kernel_thread_create(const char *name, void (*entry)(void),
                              uint8_t priority, uint32_t *pid_out) {
    process_create_params_t params = {
        .name = name,
        .type = PROCESS_TYPE_KERNEL,
        .priority = priority,
        .parent_pid = KERNEL_PROCESS_ID,
        .entry_point = (void *)entry,
        .stack_address = NULL, /* process_create kmallocs a stack */
        .stack_size = PROCESS_STACK_SIZE,
        .is_quantum_aware = false
    };

    process_t *thread = NULL;
    status_t result = process_create(&params, &thread);
    if (result != STATUS_SUCCESS) {
        return result;
    }

    if (pid_out) {
        *pid_out = thread->pid;
    }
    return STATUS_SUCCESS;
}

status_t scheduler_init(void) {
    timer_set_callback(scheduler_tick);
    boot_log("Scheduler attached to timer tick");
    return STATUS_SUCCESS;
}
