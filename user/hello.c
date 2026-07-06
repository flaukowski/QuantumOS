/**
 * QuantumOS /bin/hello — the first program that lives on the filesystem
 * (epic #62 phase 3, #65). It is NOT embedded in the kernel like the
 * service ELFs: it rides the initrd as /bin/hello, and the shell starts
 * it with `run /bin/hello` through SYS_SPAWN. Boot → shell → exec off
 * the filesystem → exit code back to the shell: the loop that makes an
 * OS an OS.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"

void _start(void) {
    write_str("HELLO: greetings from /bin/hello — an ELF loaded off the initrd");

    /* The first citizen to adopt libq: the pid line now goes through the
     * runtime's snprintf instead of a hand-rolled decimal loop. Output is
     * byte-identical, proving a real program links libq.a and behaves the same. */
    char b[64];
    snprintf(b, sizeof(b), "HELLO: my pid is %u", (unsigned)getpid());
    write_str(b);

    exit_(42); /* the shell reports this code via SYS_WAITPID */
    for (;;) {
    }
}
