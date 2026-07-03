/**
 * QuantumOS Resonant Scheduler — fixed-point hot path (ghostd phase 5, #52)
 *
 * The dormant ghostOS resonant scheduler (kernel/src/resonance/) does all
 * of its oscillator math in `double` and is compiled with -msse. The live
 * scheduler runs in interrupt context (scheduler_tick -> reschedule) and the
 * kernel is otherwise built -mno-sse with NO fxsave/fxrstor anywhere: using
 * the FPU there would silently corrupt a ring-3 process's FPU state.
 *
 * This is the resolution: a Q16.16 / phase-in-turns reimplementation of just
 * the selection hot path (Kuramoto order parameter + per-process resonant
 * priority) using integer arithmetic only, so it is safe to call from the
 * timer ISR. It is the same hypothesis as PR #21 — priority emerging from
 * coupled-oscillator dynamics rather than static assignment — executed with
 * no floating point. The double-precision file is left intact but stays
 * unwired; nothing here calls into it.
 *
 * Phase is a uint32 in "turns" (a full circle is 2^32, so wraparound is
 * free); trig is a 256-entry Q15 table indexed by the top 8 phase bits.
 * Order parameter and priorities are Q16 fractions.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RESONANT_FIXED_H
#define RESONANT_FIXED_H

#include <kernel/types.h>
#include <kernel/process.h>

/* Pick the next process under the resonant policy. Advances the fixed-point
 * oscillator field one step, recomputes the Kuramoto order parameter, then
 * returns the ready, runnable, non-idle process with the highest resonant
 * priority (the idle process is the fallback when nothing else is ready) —
 * the same contract as the round-robin pick_next it replaces. Integer only;
 * safe in interrupt context. */
process_t *resonant_fixed_pick(uint32_t from_pid);

/* Current Kuramoto order parameter r of the live field, Q16 (0..65536). */
uint32_t resonant_fixed_order_q16(void);

/* Periodic live report: prints the running order parameter r and the
 * per-process run-count spread (a fairness signal) for the real workload.
 * Cheap; the caller throttles how often it runs. */
void resonant_fixed_live_report(uint64_t switches);

/* One-shot deterministic boot benchmark. Runs one identical canned workload
 * under round-robin and under the resonant policy and prints both fairness
 * numbers plus the resonant order parameter, so the #21 hypothesis is
 * measured honestly on the same input — a loss is printed, not hidden. */
void resonant_fixed_boot_report(void);

#endif /* RESONANT_FIXED_H */
