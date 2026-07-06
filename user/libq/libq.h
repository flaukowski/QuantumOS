/**
 * QuantumOS ring-3 runtime — libq.
 *
 * The freestanding C substrate every nontrivial user program links against:
 * a static-arena heap allocator, the libc-lite mem/string functions the
 * compiler itself emits calls to, and an integer-only printf family. This is
 * the only header a citizen needs to include; it also pulls in usys.h for the
 * raw syscall wrappers.
 *
 * NOT a POSIX libc. In particular printf routes through SYS_WRITE, which the
 * kernel frames as "[user pid=N] <text>\r\n" and truncates at 127 bytes per
 * call — so printf is line-oriented (one prefixed line, implicit newline per
 * call) and hard-capped near 127 characters, not byte-accurate stream output.
 *
 * v1 scope: numeric parse-in (atoi/strtoul) and a growable SYS_BRK heap are
 * intentionally deferred. The malloc/free/calloc/realloc API is stable across
 * a future arena-source swap, so it does not corner an eventual real-binary
 * port.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef LIBQ_H
#define LIBQ_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#include "../usys.h" /* raw syscall wrappers: write_str(), exit_(), ... */

/* Per-process heap arena size (bss-backed). Override with -DLIBQ_HEAP_SIZE.
 * The true total-image guard is the ASSERT in user/user.ld; this bound only
 * keeps the arena itself in range. */
#ifndef LIBQ_HEAP_SIZE
#define LIBQ_HEAP_SIZE (64u * 1024u)
#endif
_Static_assert(LIBQ_HEAP_SIZE % 16u == 0u, "LIBQ_HEAP_SIZE must be a multiple of 16");
_Static_assert(LIBQ_HEAP_SIZE >= 4096u, "LIBQ_HEAP_SIZE too small for the allocator");
_Static_assert(LIBQ_HEAP_SIZE <= 256u * 1024u, "LIBQ_HEAP_SIZE risks overflowing the user image");

/* Freestanding-required: gcc -O2 -ffreestanding emits external calls to these
 * exact names for struct/array copies and initializers, so they must be real
 * (non-inline) symbols. */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

/* Strings. */
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strchr(const char *s, int c);

/* Heap: first-fit static-arena allocator; every returned payload is 16-byte
 * aligned. malloc/calloc/realloc return NULL on exhaustion or size overflow;
 * free(NULL) is a no-op, realloc(NULL, n) == malloc(n), realloc(p, 0) frees p
 * and returns NULL. */
void *malloc(size_t n);
void free(void *p);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *p, size_t n);

/* Integer-only printf family: %d %i %u %x %X %s %c %p %%, the '0' flag,
 * numeric width, and the l/ll length modifiers. No floating point (the user
 * ABI is -mno-sse; a float conversion would drag in soft-float). snprintf /
 * vsnprintf are pure and follow C99 truncation: size 0 writes nothing, the
 * result is always NUL-terminated within size, buf[size] is never touched,
 * and the return value is the would-be length. */
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintf(const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#endif /* LIBQ_H */
