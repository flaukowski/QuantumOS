/**
 * QuantumOS cpu_hog — the un-echoable proof that the CPU quota is ENFORCED
 * (epic #144).
 *
 * A ring-3 service granted a finite manifest CPU budget (cpu_limit in its
 * service_definition_t). It busy-spins FOREVER — no yield(), no exit_() — so
 * its manifest cpu_ticks climb every timer tick it is scheduled. Once
 * cpu_ticks >= cpu_limit, the kernel's CPU-quota enforcer (scheduler_tick)
 * TERMINATES it: a runaway that ignores cooperative scheduling cannot hog the
 * machine. Because it spins entirely in ring 3, it is ALWAYS interrupted in
 * ring 3, so the kernel's (cs&3) kill guard always lets the termination land.
 *
 * It cannot report its own death — it is dead. The proof is EXTERNAL and
 * un-forgeable: the kernel writes a CPUKILL entry to the authority ledger for
 * this pid, and the pid vanishes from the process and manifest tables. (Same
 * "the proof is external" shape as ghost_test's capless denials.)
 *
 * Registered but NOT monitored (the kernel also refuses to monitor a cpu_limit
 * service): a watchdog respawn would reset cpu_ticks and re-kill it forever.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"

void _start(void) {
    write_str("CPUHOG: burning CPU with a finite budget — expect termination");

    /* Busy-spin with real work the compiler cannot elide. No yield, no exit:
     * we are meant to be killed by the CPU-quota enforcer. */
    volatile unsigned x = 0;
    for (;;) {
        x = x + 1;
    }
}
