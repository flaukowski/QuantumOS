/**
 * QuantumOS libq — freestanding string functions.
 *
 * Like mem.c, this TU is compiled with -fno-tree-loop-distribute-patterns
 * -fno-builtin: gcc idiom recognition can otherwise lower a strlen-shaped or
 * strcpy-shaped loop into a call to the standard function being defined here.
 * Each routine is self-contained and covered by the same objdump self-call
 * gate as mem.c.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq.h"

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
        if (ca == 0) {
            break;
        }
    }
    return 0;
}

char *strcpy(char *d, const char *s) {
    char *r = d;
    while (*s) {
        *d++ = *s++;
    }
    *d = '\0';
    return r;
}

char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) {
        d[i] = s[i];
    }
    for (; i < n; i++) {
        d[i] = '\0';
    }
    return d;
}

char *strchr(const char *s, int c) {
    char ch = (char)c;
    while (*s) {
        if (*s == ch) {
            return (char *)s;
        }
        s++;
    }
    return (ch == '\0') ? (char *)s : NULL;
}
