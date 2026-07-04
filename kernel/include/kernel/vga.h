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

#endif /* VGA_H */
