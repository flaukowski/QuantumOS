/**
 * QuantumOS /bin/qtop — the kannaka-tui essence as a native console dashboard.
 *
 * kannaka-tui is a full-screen ratatui dashboard (consciousness gauges, a
 * memory-field canvas, a process/agent table). A ring-3 /bin program is capless
 * — raw console control (cons_write) needs the console-device capability that
 * only qsh holds — so qtop is an honest SNAPSHOT dashboard built from the
 * uncapped SYS_SYSINFO surface and printed as framed lines: a memory gauge from
 * the real frame allocator, the wall clock, a decorative resonance-field wave
 * (fx_sin, phase-shifted by uptime so it differs run to run), and the live
 * process table. It renders once and exits — a `top` for QuantumOS.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"

/* Pointer just past the first occurrence of `needle` in `hay`, or NULL. */
static const char *after(const char *hay, const char *needle) {
    for (const char *h = hay; *h; h++) {
        const char *a = h;
        const char *b = needle;
        while (*b && *a == *b) {
            a++;
            b++;
        }
        if (*b == '\0') {
            return a;
        }
    }
    return NULL;
}

/* Parse a non-negative decimal at p (libq has no atoi by v1 design). */
static long parse_num(const char *p) {
    long v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    return v;
}

/* Emit a possibly multi-line buffer as one framed line per row (strip CR). */
static void write_lines(const char *buf) {
    char line[128];
    int o = 0;
    for (const char *p = buf; *p; p++) {
        if (*p == '\n' || o == (int)sizeof(line) - 1) {
            line[o] = '\0';
            if (o > 0) {
                write_str(line);
            }
            o = 0;
        } else if (*p != '\r') {
            line[o++] = *p;
        }
    }
    if (o > 0) {
        line[o] = '\0';
        write_str(line);
    }
}

static void bar20(char *out, long num, long den) {
    int fill = (den > 0) ? (int)(num * 20 / den) : 0;
    if (fill > 20) {
        fill = 20;
    }
    for (int b = 0; b < 20; b++) {
        out[b] = (b < fill) ? '#' : '.';
    }
    out[20] = '\0';
}

void _start(void) {
    char buf[512];
    char line[128];

    long up = ticks();
    snprintf(line, sizeof(line), "qtop: QuantumOS live dashboard — uptime %ld ticks", up);
    write_str(line);

    /* Memory gauge from the real frame allocator. */
    long n = sysinfo(SYSINFO_MEM, buf, sizeof(buf) - 1);
    buf[(n > 0) ? n : 0] = '\0';
    const char *fp = after(buf, "frames free=");
    if (fp) {
        long freef = parse_num(fp);
        long total = 0;
        const char *sp = after(fp, "/");
        if (sp) {
            total = parse_num(sp);
        }
        long used = (total > 0) ? (total - freef) : 0;
        long pct = (total > 0) ? (used * 100 / total) : 0;
        char bar[21];
        bar20(bar, used, total);
        snprintf(line, sizeof(line), "  memory  [%s] %ld%% used  (%ld/%ld frames free)", bar, pct,
                 freef, total);
        write_str(line);
    }

    /* Wall clock. */
    n = sysinfo(SYSINFO_TIME, buf, sizeof(buf) - 1);
    buf[(n > 0) ? n : 0] = '\0';
    if (buf[0]) {
        snprintf(line, sizeof(line), "  clock   ");
        int o = 10;
        for (const char *p = buf; *p && o < (int)sizeof(line) - 1; p++) {
            if (*p != '\r' && *p != '\n') {
                line[o++] = *p;
            }
        }
        line[o] = '\0';
        write_str(line);
    }

    /* Decorative resonance-field wave (fx_sin), phase-shifted by uptime so it
     * looks alive across runs. A 10-level ASCII ramp maps sin -1..1 -> ' '..'@'. */
    const char *ramp = " .:-=+*#%@";
    char wave[61];
    for (int x = 0; x < 60; x++) {
        uint32_t th = (uint32_t)x * 0x09000000u + (uint32_t)up * 0x03000000u;
        int32_t s = fx_sin(th);
        int level = (int)(((int64_t)(s + 32768) * 10) >> 16); /* 0..9 */
        if (level > 9) {
            level = 9;
        }
        wave[x] = ramp[level];
    }
    wave[60] = '\0';
    snprintf(line, sizeof(line), "  field   %s", wave);
    write_str(line);

    /* Live process table. */
    write_str("  processes:");
    n = sysinfo(SYSINFO_PS, buf, sizeof(buf) - 1);
    buf[(n > 0) ? n : 0] = '\0';
    write_lines(buf);

    write_str("qtop: DASHBOARD RENDERED");
    exit_(0);
    for (;;) {
    }
}
