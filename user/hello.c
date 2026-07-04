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

#include "usys.h"

void _start(void) {
    write_str("HELLO: greetings from /bin/hello — an ELF loaded off the initrd");

    char b[64];
    long o = 0;
    const char *s = "HELLO: my pid is ";
    while (*s) {
        b[o++] = *s++;
    }
    unsigned v = (unsigned)getpid();
    char t[10];
    int n = 0;
    if (v == 0) {
        t[n++] = '0';
    }
    while (v) {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n) {
        b[o++] = t[--n];
    }
    b[o] = 0;
    write_str(b);

    exit_(42); /* the shell reports this code via SYS_WAITPID */
    for (;;) {
    }
}
