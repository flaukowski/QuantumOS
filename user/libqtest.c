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
 * It exercises the full runtime: the mem, str, heap and printf functions.
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

    /* ---- heap ---- */
    void *p = malloc(100);
    if (p == NULL || ((uintptr_t)p & 15u) != 0) {
        fail("LIBQ FAIL: malloc align");
    }
    memset(p, 0x5A, 100);
    if (((unsigned char *)p)[0] != 0x5A || ((unsigned char *)p)[99] != 0x5A) {
        fail("LIBQ FAIL: malloc readback");
    }
    void *q = malloc(200);
    if (q == NULL) {
        fail("LIBQ FAIL: malloc 200");
    }
    memset(q, 0x33, 200);
    void *r = realloc(q, 400); /* q is consumed; must preserve the first 200 */
    if (r == NULL) {
        fail("LIBQ FAIL: realloc");
    }
    for (int i = 0; i < 200; i++) {
        if (((unsigned char *)r)[i] != 0x33) {
            fail("LIBQ FAIL: realloc preserve");
        }
    }
    unsigned char *z = calloc(8, 16);
    if (z == NULL) {
        fail("LIBQ FAIL: calloc");
    }
    for (int i = 0; i < 128; i++) {
        if (z[i] != 0) {
            fail("LIBQ FAIL: calloc zero");
        }
    }
    free(p); /* free the arena-head block first: backward-coalesce guard */
    free(r);
    free(z);
    /* churn, then one large alloc must succeed — proving coalescing reclaims */
    for (int i = 0; i < 64; i++) {
        void *t = malloc(256);
        if (t == NULL) {
            fail("LIBQ FAIL: churn malloc");
        }
        free(t);
    }
    void *big = malloc(LIBQ_HEAP_SIZE / 2);
    if (big == NULL) {
        fail("LIBQ FAIL: large alloc after churn");
    }
    free(big);

    /* ---- printf / snprintf ---- */
    char sb[64];
    snprintf(sb, sizeof(sb), "%d,%u,%x,%X", -7, 42u, 0xBEEFu, 0xBEEFu);
    if (strcmp(sb, "-7,42,beef,BEEF") != 0) {
        fail("LIBQ FAIL: snprintf ints");
    }
    snprintf(sb, sizeof(sb), "%05u|%016llx|%02x", 42u, 0xDEADBEEFull, 5u);
    if (strcmp(sb, "00042|00000000deadbeef|05") != 0) {
        fail("LIBQ FAIL: snprintf width");
    }
    snprintf(sb, sizeof(sb), "%s %c %p", "hi", 'Q', (void *)(uintptr_t)0xDEADBEEFu);
    if (strcmp(sb, "hi Q 0xdeadbeef") != 0) {
        fail("LIBQ FAIL: snprintf s/c/p");
    }
    /* These deliberately overflow a too-small buffer to prove C99 truncation
     * semantics, so silence the (correct) compile-time truncation warning. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    char tb[4];
    if (snprintf(tb, sizeof(tb), "abcdef") != 6 || strcmp(tb, "abc") != 0) {
        fail("LIBQ FAIL: snprintf truncate");
    }
    char one[1];
    one[0] = 'X';
    if (snprintf(one, 1, "hello") != 5 || one[0] != '\0') {
        fail("LIBQ FAIL: snprintf size1");
    }
    char zb[8];
    memset(zb, 0x7F, sizeof(zb));
    if (snprintf(zb, 0, "hi") != 2 || (unsigned char)zb[0] != 0x7F) {
        fail("LIBQ FAIL: snprintf size0");
    }
#pragma GCC diagnostic pop

    /* Real printf -> vsnprintf -> write_str -> SYS_WRITE path (also gated). */
    printf("LIBQ printf d=%d u=%u x=%x s=%s\n", -7, 42u, 0xBEEFu, "ok");

    write_str("LIBQ: self-test OK");
    exit_(0);
    for (;;) {
    }
}
