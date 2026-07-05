/**
 * QuantumOS CMOS Real-Time Clock implementation.
 *
 * The RTC is read through the CMOS index/data ports (0x70/0x71). Two
 * standard hazards are handled: the Update-In-Progress flag (register A
 * bit 7 — never read the time fields while the chip is mid-update), and
 * BCD encoding (register B bit 2 clear => the fields are BCD, which is
 * QEMU's default). We read twice and accept the value only when two
 * consecutive reads agree, the belt-and-braces approach that avoids a
 * torn read across a tick.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/rtc.h>

#define CMOS_INDEX 0x70
#define CMOS_DATA 0x71

#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS 0x04
#define RTC_DAY 0x07
#define RTC_MONTH 0x08
#define RTC_YEAR 0x09
#define RTC_CENTURY 0x32
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

#define STATUS_A_UIP 0x80
#define STATUS_B_BINARY 0x04 /* set => fields already binary, not BCD */
#define STATUS_B_24H 0x02    /* set => 24-hour mode */

static inline void outb_(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint8_t inb_(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static uint8_t cmos_read(uint8_t reg) {
    outb_(CMOS_INDEX, reg);
    return inb_(CMOS_DATA);
}

static int update_in_progress(void) {
    return cmos_read(RTC_STATUS_A) & STATUS_A_UIP;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

/* Read the raw register set once (after the UIP flag clears). */
static void read_raw(rtc_time_t *t, uint8_t *century) {
    /* Bounded wait for any in-progress update to finish. */
    for (int i = 0; i < 1000000 && update_in_progress(); i++) {
        /* spin */
    }
    t->second = cmos_read(RTC_SECONDS);
    t->minute = cmos_read(RTC_MINUTES);
    t->hour = cmos_read(RTC_HOURS);
    t->day = cmos_read(RTC_DAY);
    t->month = cmos_read(RTC_MONTH);
    uint8_t y = cmos_read(RTC_YEAR);
    t->year = y;
    *century = cmos_read(RTC_CENTURY);
}

void rtc_read(rtc_time_t *out) {
    rtc_time_t a, b;
    uint8_t ca, cb;

    /* Read until two consecutive reads agree (no tick crossed between). */
    read_raw(&a, &ca);
    for (int i = 0; i < 10; i++) {
        read_raw(&b, &cb);
        if (a.second == b.second && a.minute == b.minute && a.hour == b.hour && a.day == b.day &&
            a.month == b.month && a.year == b.year && ca == cb) {
            break;
        }
        a = b;
        ca = cb;
    }

    uint8_t regb = cmos_read(RTC_STATUS_B);
    uint16_t year = a.year;
    uint8_t century = ca;

    if (!(regb & STATUS_B_BINARY)) {
        a.second = bcd_to_bin(a.second);
        a.minute = bcd_to_bin(a.minute);
        /* The hour's high bit is the PM flag in 12-hour mode; mask it
         * off before BCD-decoding the low bits. */
        a.hour = (uint8_t)(((a.hour & 0x0F) + (((a.hour & 0x70) >> 4) * 10)) | (a.hour & 0x80));
        a.day = bcd_to_bin(a.day);
        a.month = bcd_to_bin(a.month);
        year = bcd_to_bin((uint8_t)year);
        century = bcd_to_bin(century);
    }

    /* 12-hour mode: convert, honoring the PM bit. */
    if (!(regb & STATUS_B_24H) && (a.hour & 0x80)) {
        a.hour = (uint8_t)(((a.hour & 0x7F) + 12) % 24);
    }

    /* Full year: prefer the century register when present, else assume
     * the 2000s (this OS did not exist before then). */
    if (century >= 19 && century <= 21) {
        out->year = (uint16_t)(century * 100 + year);
    } else {
        out->year = (uint16_t)(2000 + year);
    }
    out->month = a.month;
    out->day = a.day;
    out->hour = a.hour;
    out->minute = a.minute;
    out->second = a.second;
}

static size_t put2(char *b, size_t o, size_t max, uint8_t v) {
    if (o + 2 < max) {
        b[o++] = (char)('0' + (v / 10) % 10);
        b[o++] = (char)('0' + v % 10);
    }
    return o;
}

size_t rtc_format(char *buf, size_t max) {
    if (!buf || max < 24) {
        return 0;
    }
    rtc_time_t t;
    rtc_read(&t);

    size_t o = 0;
    const char *pfx = "TIME: ";
    while (*pfx) {
        buf[o++] = *pfx++;
    }
    /* YYYY */
    buf[o++] = (char)('0' + (t.year / 1000) % 10);
    buf[o++] = (char)('0' + (t.year / 100) % 10);
    buf[o++] = (char)('0' + (t.year / 10) % 10);
    buf[o++] = (char)('0' + t.year % 10);
    buf[o++] = '-';
    o = put2(buf, o, max, t.month);
    buf[o++] = '-';
    o = put2(buf, o, max, t.day);
    buf[o++] = ' ';
    o = put2(buf, o, max, t.hour);
    buf[o++] = ':';
    o = put2(buf, o, max, t.minute);
    buf[o++] = ':';
    o = put2(buf, o, max, t.second);
    buf[o++] = '\r';
    buf[o++] = '\n';
    buf[o] = '\0';
    return o;
}
