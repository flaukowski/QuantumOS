/**
 * QuantumOS CMOS Real-Time Clock (epic follow-up)
 *
 * Reads the wall-clock time from the PC's CMOS RTC (ports 0x70/0x71).
 * Under QEMU this reflects the host time. Gives the OS real-world time
 * awareness — the `date` shell command reads it via SYS_SYSINFO.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RTC_H
#define RTC_H

#include <kernel/types.h>

typedef struct {
    uint16_t year; /* full year, e.g. 2026 */
    uint8_t month; /* 1-12 */
    uint8_t day;   /* 1-31 */
    uint8_t hour;  /* 0-23 */
    uint8_t minute;
    uint8_t second;
} rtc_time_t;

/* Read the current time from the CMOS RTC (handles the update-in-
 * progress flag and BCD encoding). */
void rtc_read(rtc_time_t *out);

/* Format the current time as "TIME: YYYY-MM-DD HH:MM:SS\r\n" into buf
 * (bounded, NUL-terminated). Returns bytes written. */
size_t rtc_format(char *buf, size_t max);

#endif /* RTC_H */
