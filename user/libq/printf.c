/**
 * QuantumOS libq — integer-only printf family.
 *
 * vsnprintf is the pure, iterative core (no recursion, no floating point);
 * snprintf/vprintf/printf build on it. Supported conversions: d i u x X s c p
 * and a literal percent, with the '0' flag, a numeric field width, and the
 * l / ll length modifiers. No floating point: the user ABI is -mno-sse, so a
 * float conversion would drag in soft-float and break the -nostdlib link.
 *
 * snprintf / vsnprintf follow C99 truncation: a size of 0 writes nothing, the
 * result is always NUL-terminated within size, buf[size] is never touched, and
 * the return value is the would-be length. printf / vprintf format into a
 * 128-byte stack buffer and hand the NUL-terminated line to write_str
 * (SYS_WRITE), which the kernel frames as one prefixed line and caps at 127
 * bytes — so printf is line-oriented, not byte-accurate stream output.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq.h"

typedef struct {
    char *buf;
    size_t size;
    size_t count; /* running would-be length; may exceed size - 1 */
} outbuf_t;

static void out_char(outbuf_t *o, char c) {
    if (o->count + 1u < o->size) { /* leave the last slot for the NUL */
        o->buf[o->count] = c;
    }
    o->count++;
}

static void out_str(outbuf_t *o, const char *s) {
    while (*s) {
        out_char(o, *s++);
    }
}

/* Emit `mag` in `base` with an optional leading '-', right-justified to
 * `width` and padded with '0' (when zero) or ' '. The sign is placed before
 * the zero padding but after the space padding, matching C. */
static void emit_number(outbuf_t *o, unsigned long long mag, int negative, unsigned base, int upper,
                        int width, int zero) {
    char tmp[24];
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    if (mag == 0) {
        tmp[n++] = '0';
    }
    while (mag) {
        tmp[n++] = digs[mag % base];
        mag /= base;
    }
    int total = n + (negative ? 1 : 0);
    if (zero) {
        if (negative) {
            out_char(o, '-');
        }
        for (int i = total; i < width; i++) {
            out_char(o, '0');
        }
    } else {
        for (int i = total; i < width; i++) {
            out_char(o, ' ');
        }
        if (negative) {
            out_char(o, '-');
        }
    }
    while (n) {
        out_char(o, tmp[--n]);
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    outbuf_t o = {buf, size, 0};
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') {
            out_char(&o, *f);
            continue;
        }
        f++;
        if (*f == '%') {
            out_char(&o, '%');
            continue;
        }
        int zero = 0;
        while (*f == '0') {
            zero = 1;
            f++;
        }
        int width = 0;
        while (*f >= '0' && *f <= '9') {
            width = width * 10 + (*f - '0');
            f++;
        }
        int lng = 0;
        while (*f == 'l') {
            lng++;
            f++;
        }
        char conv = *f;
        switch (conv) {
        case 'd':
        case 'i': {
            long long v = (lng >= 2)   ? va_arg(ap, long long)
                          : (lng == 1) ? va_arg(ap, long)
                                       : (long long)va_arg(ap, int);
            int neg = v < 0;
            unsigned long long mag =
                neg ? (unsigned long long)(-(v + 1)) + 1ull : (unsigned long long)v;
            emit_number(&o, mag, neg, 10u, 0, width, zero);
            break;
        }
        case 'u': {
            unsigned long long v = (lng >= 2)   ? va_arg(ap, unsigned long long)
                                   : (lng == 1) ? va_arg(ap, unsigned long)
                                                : (unsigned long long)va_arg(ap, unsigned int);
            emit_number(&o, v, 0, 10u, 0, width, zero);
            break;
        }
        case 'x':
        case 'X': {
            unsigned long long v = (lng >= 2)   ? va_arg(ap, unsigned long long)
                                   : (lng == 1) ? va_arg(ap, unsigned long)
                                                : (unsigned long long)va_arg(ap, unsigned int);
            emit_number(&o, v, 0, 16u, conv == 'X', width, zero);
            break;
        }
        case 'p': {
            void *pv = va_arg(ap, void *);
            out_char(&o, '0');
            out_char(&o, 'x');
            emit_number(&o, (unsigned long long)(uintptr_t)pv, 0, 16u, 0, 0, 0);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            int len = 0;
            for (const char *t = s; *t; t++) {
                len++;
            }
            for (int i = len; i < width; i++) {
                out_char(&o, zero ? '0' : ' ');
            }
            out_str(&o, s);
            break;
        }
        case 'c': {
            out_char(&o, (char)va_arg(ap, int));
            break;
        }
        case '\0':
            out_char(&o, '%');
            f--; /* the outer loop's f++ lands on the NUL and terminates */
            break;
        default:
            out_char(&o, '%');
            out_char(&o, conv);
            break;
        }
    }
    if (o.size > 0) {
        size_t term = (o.count < o.size) ? o.count : o.size - 1;
        o.buf[term] = '\0';
    }
    return (int)o.count;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char *fmt, va_list ap) {
    char line[128];
    int r = vsnprintf(line, sizeof(line), fmt, ap);
    write_str(line);
    return r;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}
