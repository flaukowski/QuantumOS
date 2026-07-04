/**
 * QuantumOS IPC client — a ring-3 user process that sends a request to
 * the echo service over capability-checked IPC and prints the reply.
 * Its send is routed by the IPC send-capability the kernel granted it
 * (→ the echo service); it holds no other IPC capability, so it can
 * talk to nothing else.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "usys.h"

void _start(void) {
    write_str("ipc-client: sending request to echo service");

    long rc = send_msg("ping from client", str_len("ping from client"));
    if (rc != 0) {
        write_str("ipc-client: send FAILED (unexpected)");
        exit_(1);
    }

    char buf[120];
    long spins = 0;
    while (spins < 100000) {
        long sender = recv_msg(buf, sizeof(buf));
        if (sender != 0) {
            write_str("ipc-client: got reply ->");
            write_str(buf);
            break;
        }
        yield();
        spins++;
    }

    write_str("ipc-client: done");
    exit_(0);
    for (;;) {
    }
}
