/**
 * QuantumOS libq — freestanding mem* functions.
 *
 * gcc -O2 -ffreestanding emits external calls to memcpy/memmove/memset/memcmp
 * for struct/array copies, aggregate initializers and struct compares, so
 * these must exist as real (non-inline) symbols with the exact C signatures.
 *
 * This TU is compiled with -fno-tree-loop-distribute-patterns -fno-builtin
 * (see the Makefile) so -O2 cannot rewrite a naive copy/fill loop into a call
 * to the very function being defined (a silent infinite recursion). Each
 * routine is self-contained — memmove does NOT delegate to memcpy — so the
 * pre-boot objdump gate can assert this object makes no call to any libc name.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq.h"

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char v = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        d[i] = v;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}
