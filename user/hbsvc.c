/**
 * QuantumOS watched-svc — a ring-3 user-process service that reports
 * liveness to the kernel's service watchdog via SYS_HEARTBEAT. To
 * demonstrate crash detection + automatic restart of a *user* service,
 * it heartbeats for a while, then deliberately goes silent (simulating
 * a hang). The health monitor notices the stale heartbeat and restarts
 * it as a fresh ring-3 process, up to its restart limit.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "usys.h"

void _start(void) {
    /* Under a `quiet` boot, stay off the shared console — the watchdog
     * demo reprints "online"/"going silent" on every restart, which would
     * bury the interactive shell. The behaviour (heartbeat then hang) is
     * unchanged; only the narration is silenced. */
    int quiet = (int)sysinfo_quiet();
    if (!quiet) {
        write_str("watched-svc: online in ring 3, heartbeating to the watchdog");
    }

    long start = ticks();
    int went_silent = 0;

    while (1) {
        long now = ticks();
        if (now - start < 120) { /* ~1.2s of healthy heartbeats */
            heartbeat();
        } else if (!went_silent) {
            if (!quiet) {
                write_str("watched-svc: going silent (simulated hang)");
            }
            went_silent = 1; /* stop heartbeating; keep running */
        }
        yield();
    }
}
