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
#include <kernel/vmspace.h>
#include <kernel/boot.h>
#ifdef SCHED_LOTTERY
#include <kernel/quantum.h>
#endif
#ifdef SCHED_RESONANT
#include <kernel/resonant_fixed.h>
#endif

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
#if defined(SCHED_RESONANT)
    /* Resonant pick (opt-in, SCHED_RESONANT): the next process emerges from
     * the fixed-point coupled-oscillator field (Kuramoto order parameter +
     * per-process resonant priority) rather than table order. Integer-only,
     * so it is safe in this interrupt context — the dormant double-precision
     * scheduler is NOT called here (see resonant_fixed.h). */
    return resonant_fixed_pick(from_pid);
#elif defined(SCHED_LOTTERY)
    /* Lottery pick (opt-in, SCHED_LOTTERY): gather every ready, runnable,
     * non-idle process and draw one uniformly using the kernel's qseed-mixed
     * generator (integer only). The idle process remains the fallback when
     * nothing else is ready, exactly as in round-robin. */
    process_t *cand[MAX_PROCESSES];
    uint32_t n = 0;
    process_t *idle = NULL;

    for (uint32_t pid = 0; pid < MAX_PROCESSES; pid++) {
        process_t *p = process_get_by_pid(pid);
        if (!p || p->state != PROCESS_STATE_READY || !p->context_valid) {
            continue;
        }
        if (pid == IDLE_PROCESS_ID) {
            idle = p;
            continue;
        }
        cand[n++] = p;
    }

    (void)from_pid;
    if (n == 0) {
        return idle;
    }
    return cand[quantum_kernel_rand() % n];
#else
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
#endif
}

void scheduler_tick(cpu_state_t *state) {
    if (++quantum_counter < SCHED_QUANTUM_TICKS) {
        return;
    }
    scheduler_reschedule(state);
}

void scheduler_reschedule(cpu_state_t *state) {
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
     * the end of irq_common resumes it. Switch to the next process's
     * address space first — safe because the kernel half (this code,
     * the interrupt stack, the frame) is mapped identically in every
     * space. */
    vmspace_switch(next->cr3 ? next->cr3 : vmspace_kernel_cr3());
    *state = next->context;

    switch_count++;
    if (switch_count == 1) {
        boot_log("scheduler: first context switch");
    }
#ifdef SCHED_LOTTERY
    /* Periodically report the running total of quantum-derived bits the
     * lottery has consumed (the "reap/idle report"). */
    if ((switch_count & 0xFFu) == 0) {
        early_console_write("scheduler[lottery]: quantum bits consumed = ");
        early_console_write_hex(quantum_bits_consumed());
        early_console_write("\r\n");
    }
#endif
#ifdef SCHED_RESONANT
    /* Periodic live honesty report: the running order parameter r and the
     * per-process run-count spread (fairness) for the REAL workload — printed
     * whether resonant scheduling is winning or losing. */
    if ((switch_count & 0xFFu) == 0) {
        resonant_fixed_live_report(switch_count);
    }
#endif
}

void scheduler_kill_current(cpu_state_t *state) {
    process_t *cur = process_get_current();
    process_t *next = pick_next(cur ? cur->pid : 0);
    if (!next) {
        boot_panic("scheduler: nothing runnable after kill");
        return;
    }

    /* Do not save the dead process's context */
    process_switch_to(next);
    process_set_state(next->pid, PROCESS_STATE_RUNNING);
    vmspace_switch(next->cr3 ? next->cr3 : vmspace_kernel_cr3());
    *state = next->context;
    switch_count++;
}

uint64_t scheduler_get_switches(void) {
    return switch_count;
}

status_t kernel_thread_create(const char *name, void (*entry)(void), uint8_t priority,
                              uint32_t *pid_out) {
    process_create_params_t params = {.name = name,
                                      .type = PROCESS_TYPE_KERNEL,
                                      .priority = priority,
                                      .parent_pid = KERNEL_PROCESS_ID,
                                      .entry_point = (void *)entry,
                                      .stack_address = NULL, /* process_create kmallocs a stack */
                                      .stack_size = PROCESS_STACK_SIZE,
                                      .is_quantum_aware = false};

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
