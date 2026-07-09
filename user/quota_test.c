/**
 * QuantumOS quota-test — the un-echoable proof that the spawn QUOTA is
 * ENFORCED (epic #135 Phase D increment 2).
 *
 * A ring-3 service granted a spawn capability AND a manifest spawn quota of
 * exactly 1 (grant_spawn + spawn_max=1 in its service_definition_t). It
 * spawns /bin/qprobe twice:
 *   - the FIRST succeeds (returns a pid > 0),
 *   - the SECOND is refused by the kernel's manifest quota with EXACTLY
 *     EPERM (-4) — a denial the capability layer alone would never produce,
 *     because quota-test holds a perfectly valid spawn cap. The kernel also
 *     records that denial in the authority ledger as kind=QUOTA, which no
 *     citizen can forge or suppress.
 *
 * It prints "QUOTA ENFORCED pid=<self>" ONLY on the exact expected outcome,
 * and "QUOTA BROKEN first=<r1> second=<r2>" on anything else, so a boot where
 * the quota silently never ran cannot pass by accident (e.g. a first-spawn
 * failure would show BROKEN, not a missing line the gate could misread).
 *
 * It is registered but NOT monitored (see user_quota_test_init in
 * kernel/src/syscall.c): after the one-shot proof it loops idle rather than
 * exiting, so it neither triggers a watchdog respawn nor leaves a stale
 * service slot.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"

void _start(void) {
    unsigned self = (unsigned)getpid();

    long r1 = spawn_("/bin/qprobe"); /* under quota — must succeed */
    long r2 = spawn_("/bin/qprobe"); /* over quota — must be EPERM (-4) */

    char line[96];
    if (r1 > 0 && r2 == -4) {
        snprintf(line, sizeof(line), "QUOTA ENFORCED pid=%u", self);
    } else {
        snprintf(line, sizeof(line), "QUOTA BROKEN first=%d second=%d", (int)r1, (int)r2);
    }
    write_str(line);

    /* Proof done. Loop idle (unmonitored → no heartbeat needed) rather than
     * exiting, so the service slot never goes stale. */
    for (;;) {
        yield();
    }
}
