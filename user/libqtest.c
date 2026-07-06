/**
 * QuantumOS /bin/libqtest — the libq runtime self-test.
 *
 * Rides the initrd as /bin/libqtest and is started from the shell with
 * `run /bin/libqtest`. It exercises the libq runtime end-to-end at -O2 in
 * ring 3 and, on success, prints the sentinel "LIBQ: self-test OK" that the
 * ci-smoke / ci.yml boot gates grep for. On any failure it prints
 * "LIBQ FAIL: <what>" and omits the sentinel, failing the gate. Uses only
 * SYS_WRITE + SYS_EXIT (no capabilities).
 *
 * The self-test grows with the runtime: this commit covers the mem and str
 * functions; the heap and printf checks arrive with those modules.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"

static void fail(const char *what) {
    write_str(what);
    exit_(1);
}

void _start(void) {
    /* ---- mem ---- */
    unsigned char a[32];
    unsigned char b[32];
    memset(a, 0xAB, sizeof(a));
    for (size_t i = 0; i < sizeof(a); i++) {
        if (a[i] != 0xAB) {
            fail("LIBQ FAIL: memset");
        }
    }
    memcpy(b, a, sizeof(b));
    if (memcmp(a, b, sizeof(a)) != 0) {
        fail("LIBQ FAIL: memcpy/memcmp");
    }
    b[10] = 0xAC;
    if (!(memcmp(a, b, sizeof(a)) < 0) || !(memcmp(b, a, sizeof(a)) > 0)) {
        fail("LIBQ FAIL: memcmp order");
    }

    /* memmove overlapping, both directions */
    char ov[16] = "0123456789abcde"; /* 15 chars + NUL */
    memmove(ov + 2, ov, 8);          /* dst > src: must copy backward */
    if (memcmp(ov + 2, "01234567", 8) != 0) {
        fail("LIBQ FAIL: memmove fwd-overlap");
    }
    char ov2[16] = "0123456789abcde";
    memmove(ov2, ov2 + 2, 8); /* dst < src: must copy forward */
    if (memcmp(ov2, "23456789", 8) != 0) {
        fail("LIBQ FAIL: memmove back-overlap");
    }

    /* ---- str ---- */
    if (strlen("quantum") != 7) {
        fail("LIBQ FAIL: strlen");
    }
    if (strcmp("abc", "abc") != 0 || !(strcmp("abc", "abd") < 0) || !(strcmp("abd", "abc") > 0)) {
        fail("LIBQ FAIL: strcmp");
    }
    if (strncmp("abcXX", "abcYY", 3) != 0 || strncmp("abX", "abY", 3) == 0) {
        fail("LIBQ FAIL: strncmp");
    }
    char dst[8];
    strcpy(dst, "hi");
    if (strcmp(dst, "hi") != 0) {
        fail("LIBQ FAIL: strcpy");
    }
    char nd[6];
    memset(nd, 'Z', sizeof(nd));
    strncpy(nd, "ab", sizeof(nd)); /* n longer than src: zero-fill remainder */
    if (nd[0] != 'a' || nd[1] != 'b' || nd[2] != '\0' || nd[5] != '\0') {
        fail("LIBQ FAIL: strncpy zero-fill");
    }
    char nd2[3];
    strncpy(nd2, "abcdef", 3); /* n shorter than src: no NUL written */
    if (nd2[0] != 'a' || nd2[1] != 'b' || nd2[2] != 'c') {
        fail("LIBQ FAIL: strncpy no-nul");
    }
    const char *hay = "a.b.c";
    if (strchr(hay, 'b') != hay + 2) {
        fail("LIBQ FAIL: strchr hit");
    }
    if (strchr(hay, 'z') != NULL) {
        fail("LIBQ FAIL: strchr miss");
    }
    if (strchr(hay, '\0') != hay + 5) {
        fail("LIBQ FAIL: strchr nul");
    }

    write_str("LIBQ: self-test OK");
    exit_(0);
    for (;;) {
    }
}
