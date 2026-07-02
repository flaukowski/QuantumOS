/**
 * QuantumOS echo service — a system service running as an isolated
 * ring-3 user process (not a kernel thread). It receives messages over
 * capability-checked IPC and replies to its client. Its ability to
 * reply is itself a capability: send_msg() is routed by the IPC
 * send-capability the kernel granted it (→ its client).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "usys.h"

void _start(void) {
    write_str("echo-service: online in ring 3, awaiting IPC");

    char buf[120];
    int handled = 0;
    long spins = 0;

    while (handled < 2 && spins < 100000) {
        long sender = recv_msg(buf, sizeof(buf));
        if (sender == 0) {
            yield();          /* mailbox empty — let the client run */
            spins++;
            continue;
        }

        write_str("echo-service: received a request, replying");

        /* Build "echo: <request>" */
        char reply[120];
        const char *pfx = "echo: ";
        int i = 0;
        for (const char *p = pfx; *p && i < (int)sizeof(reply) - 1; p++) {
            reply[i++] = *p;
        }
        for (int j = 0; buf[j] && i < (int)sizeof(reply) - 1; j++) {
            reply[i++] = buf[j];
        }
        reply[i] = 0;

        send_msg(reply, i);   /* routed to the client by capability */
        handled++;
    }

    write_str("echo-service: served requests, shutting down");
    exit_(0);
    for (;;) { }
}
