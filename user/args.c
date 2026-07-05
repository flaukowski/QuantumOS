/**
 * QuantumOS /bin/args — echo the argument vector (epic #62 follow-up, #69).
 *
 * Proves the argv path end to end: the shell's `run /bin/args a b c` passes
 * the command line to SYS_SPAWN, the kernel splits it into argv and lays it
 * into this process's read-only args page, and get_args() reads it back.
 * Prints one line per argument, then exits with argc as the code.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "usys.h"

void _start(void) {
    char *argv[USER_ARGS_MAX];
    int argc = get_args(argv, USER_ARGS_MAX);

    char b[64];
    int o = 0;
    const char *h = "ARGS: argc=";
    while (*h) {
        b[o++] = *h++;
    }
    /* argc as decimal */
    {
        unsigned v = (unsigned)(argc < 0 ? 0 : argc);
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
    }
    b[o] = 0;
    write_str(b);

    int shown = argc < USER_ARGS_MAX ? argc : USER_ARGS_MAX;
    for (int i = 0; i < shown; i++) {
        char line[128];
        int p = 0;
        const char *pfx = "ARGS: argv[";
        while (*pfx) {
            line[p++] = *pfx++;
        }
        /* index */
        {
            unsigned v = (unsigned)i;
            char t[4];
            int n = 0;
            if (v == 0) {
                t[n++] = '0';
            }
            while (v) {
                t[n++] = (char)('0' + v % 10);
                v /= 10;
            }
            while (n) {
                line[p++] = t[--n];
            }
        }
        line[p++] = ']';
        line[p++] = '=';
        for (const char *s = argv[i]; *s && p < (int)sizeof(line) - 1; s++) {
            line[p++] = *s;
        }
        line[p] = 0;
        write_str(line);
    }

    exit_(argc); /* the shell reports argc as the exit code */
    for (;;) {
    }
}
