/**
 * QuantumOS VGA Text-Mode Console + Boot Splash
 *
 * Drives the 80x25 colour text buffer at 0xB8000 (identity-mapped in
 * the kernel half). The boot splash is a quantum wave-interference
 * animation whose progress bar advances as real subsystems come up —
 * a nod to Kannaka's wave-interference lineage and the eventual
 * qBraid quantum-lab boot target.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VGA_H
#define VGA_H

#include <kernel/types.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

/* VGA colour attributes */
typedef enum {
    VGA_BLACK = 0,
    VGA_BLUE,
    VGA_GREEN,
    VGA_CYAN,
    VGA_RED,
    VGA_MAGENTA,
    VGA_BROWN,
    VGA_LIGHT_GREY,
    VGA_DARK_GREY,
    VGA_LIGHT_BLUE,
    VGA_LIGHT_GREEN,
    VGA_LIGHT_CYAN,
    VGA_LIGHT_RED,
    VGA_LIGHT_MAGENTA,
    VGA_YELLOW,
    VGA_WHITE
} vga_color_t;

/* Low-level buffer ops */
void vga_init(void);
void vga_clear(vga_color_t bg);
void vga_put(int x, int y, char ch, vga_color_t fg, vga_color_t bg);
void vga_print(int x, int y, const char *s, vga_color_t fg, vga_color_t bg);
void vga_print_center(int y, const char *s, vga_color_t fg, vga_color_t bg);

/* Boot splash */
void vga_boot_splash(void);                          /* draw the static frame + logo */
void vga_boot_stage(const char *label, int percent); /* advance bar, animate */
void vga_boot_ready(void);                           /* final "READY" flourish */

/* Scrolling text console (epic #101). On serial-less real hardware the
 * VGA text buffer is the machine's only display: once enabled (text-mode
 * boots only, after the splash), boot_log and SYS_CONS writes tee here
 * alongside COM1. Inactive until vga_console_enable() — the QEMU/CI
 * serial contract is unchanged. */
void vga_console_enable(void);
int vga_console_active(void);
void vga_console_putc(char c); /* does not move the HW cursor — batch via sync */
void vga_console_puts(const char *s);
void vga_console_sync(void); /* move the HW cursor to the pen position */

/* Panic banner: independent of console state so a failure at any boot
 * stage is visible on screen, not just COM1. Skips itself when a linear
 * framebuffer owns the display. Called from boot_panic (boot.S). */
void vga_panic_banner(const char *msg);

#endif /* VGA_H */
