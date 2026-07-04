/**
 * QuantumOS init — a real compiled-C user program, ELF-loaded into
 * ring 3 (no libc, no CRT: _start is the entry point).
 *
 * Proves the ELF loader end-to-end and, now, that IPC is
 * capability-gated: init holds no IPC send-capability, so its send
 * attempt is denied at the syscall boundary (SYSCALL_EPERM = -4).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "usys.h"

void _start(void) {
    write_str("init: ELF-loaded C program running in ring 3");

    volatile long pid = getpid();
    (void)pid;
    write_str("init: obtained pid + tick count via syscalls");

    /* No IPC capability was granted to init, so this must be refused */
    long rc = send_msg("init has no capability for this", 30);
    if (rc == -4 /* SYSCALL_EPERM */) {
        write_str("init: IPC send denied without a capability — as expected");
    } else {
        write_str("init: IPC send NOT denied — capability check broken");
    }

    long t0 = ticks();
    for (int i = 0; i < 4; i++) {
        yield();
    }
    long t1 = ticks();
    if (t1 >= t0) {
        write_str("init: survived scheduler round-trips, exiting cleanly");
    }

    exit_(0);
    for (;;) {
    } /* unreachable */
}
