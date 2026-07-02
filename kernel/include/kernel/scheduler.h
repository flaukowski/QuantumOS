/**
 * QuantumOS Preemptive Scheduler
 *
 * Round-robin preemptive scheduler driven by the PIT timer tick.
 * Context switching is done on the interrupt frame: on a scheduling
 * tick the saved cpu_state_t of the interrupted process is written to
 * its PCB and the next process's saved state is written back onto the
 * frame, so the iretq at the end of irq_common resumes the next
 * process directly.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <kernel/types.h>
#include <kernel/interrupts.h>

/* Scheduling quantum in timer ticks (ticks are 10 ms at TIMER_DEFAULT_HZ) */
#define SCHED_QUANTUM_TICKS 5

/* Attach the scheduler to the timer tick. Call after process_init(). */
status_t scheduler_init(void);

/* Create a kernel thread: own kmalloc'd stack, entry runs at the given
 * priority with interrupts enabled. The entry function must not return. */
status_t kernel_thread_create(const char *name, void (*entry)(void),
                              uint8_t priority, uint32_t *pid_out);

/* Number of context switches performed by the scheduler */
uint64_t scheduler_get_switches(void);

/* Timer callback (exposed for diagnostics/testing) */
void scheduler_tick(cpu_state_t *state);

#endif /* SCHEDULER_H */
