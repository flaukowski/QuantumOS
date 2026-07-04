/**
 * QuantumOS VGA Text-Mode Console + Boot Splash Implementation
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/vga.h>

static volatile uint16_t *const VGA_MEM = (uint16_t *)0xB8000;

/* CP437 shade blocks, light -> full */
#define CH_SHADE_LIGHT 0xB0
#define CH_SHADE_MED 0xB1
#define CH_SHADE_DARK 0xB2
#define CH_BLOCK_FULL 0xDB

static int splash_frame = 0;

/* ---- port I/O (hardware cursor) ---- */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t cell(char ch, vga_color_t fg, vga_color_t bg) {
    return (uint16_t)((uint8_t)ch) | ((uint16_t)((bg << 4) | fg) << 8);
}

/* ---- integer sine, period 32, amplitude +/-100 ---- */
static const int8_t sine32[32] = {0,   20,  38,   56,  71,  83,  92,  98,  100, 98,  92,
                                  83,  71,  56,   38,  20,  0,   -20, -38, -56, -71, -83,
                                  -92, -98, -100, -98, -92, -83, -71, -56, -38, -20};

/* Coarse busy-wait: interrupts are not enabled during the splash, so
 * animation frames are paced by a calibrated volatile loop. Kept small
 * so total splash time stays well under the boot smoke-test budget. */
static void frame_delay(void) {
    for (volatile uint32_t i = 0; i < 400000; i++) {
        __asm__ volatile("" ::: "memory");
    }
}

/* ---- primitives ---- */
void vga_put(int x, int y, char ch, vga_color_t fg, vga_color_t bg) {
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) {
        return;
    }
    VGA_MEM[y * VGA_WIDTH + x] = cell(ch, fg, bg);
}

void vga_print(int x, int y, const char *s, vga_color_t fg, vga_color_t bg) {
    for (int i = 0; s[i]; i++) {
        vga_put(x + i, y, s[i], fg, bg);
    }
}

static int str_len(const char *s) {
    int n = 0;
    while (s[n])
        n++;
    return n;
}

void vga_print_center(int y, const char *s, vga_color_t fg, vga_color_t bg) {
    int x = (VGA_WIDTH - str_len(s)) / 2;
    vga_print(x, y, s, fg, bg);
}

void vga_clear(vga_color_t bg) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_MEM[i] = cell(' ', VGA_LIGHT_GREY, bg);
    }
}

static void hide_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20); /* bit 5 disables the cursor */
}

void vga_init(void) {
    hide_cursor();
    vga_clear(VGA_BLACK);
}

/* ---- boot splash ---- */

/* One frame of a two-source interference pattern across the wave band.
 * Two waves travel in opposite directions; their sum sets the shade
 * and colour of each column, producing a shimmering standing pattern. */
static void draw_wave(int frame) {
    const int top = 10; /* wave band rows 10..13 */
    const int rows = 4;
    const int mid = top + rows / 2;

    for (int x = 4; x < VGA_WIDTH - 4; x++) {
        int a = sine32[(x + frame) & 31];
        int b = sine32[(x * 2 - frame) & 31];
        int sum = (a + b) / 2;          /* -100..100 */
        int amp = sum < 0 ? -sum : sum; /* 0..100 */

        int level = amp / 26; /* 0..3 shade */
        char ch;
        vga_color_t fg;
        switch (level) {
        case 0:
            ch = CH_SHADE_LIGHT;
            fg = VGA_BLUE;
            break;
        case 1:
            ch = CH_SHADE_MED;
            fg = VGA_LIGHT_BLUE;
            break;
        case 2:
            ch = CH_SHADE_DARK;
            fg = VGA_LIGHT_CYAN;
            break;
        default:
            ch = CH_BLOCK_FULL;
            fg = VGA_WHITE;
            break;
        }

        /* Map the signed sum to a row within the band for a crest line */
        int row = mid - (sum * (rows / 2)) / 100;
        for (int y = top; y < top + rows; y++) {
            if (y == row) {
                vga_put(x, y, ch, fg, VGA_BLACK);
            } else {
                vga_put(x, y, CH_SHADE_LIGHT, (y < row) ? VGA_DARK_GREY : VGA_BLUE, VGA_BLACK);
            }
        }
    }
}

void vga_boot_splash(void) {
    vga_init();

    /* Frame border */
    for (int x = 0; x < VGA_WIDTH; x++) {
        vga_put(x, 0, CH_BLOCK_FULL, VGA_BLUE, VGA_BLACK);
        vga_put(x, VGA_HEIGHT - 1, CH_BLOCK_FULL, VGA_BLUE, VGA_BLACK);
    }

    /* Title */
    vga_print_center(3, "Q U A N T U M   O S", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_print_center(5, "wave-interference microkernel", VGA_WHITE, VGA_BLACK);
    vga_print_center(7, "// ring-3 isolation // capability security // qubits //", VGA_DARK_GREY,
                     VGA_BLACK);

    /* Progress bar frame at row 18 */
    vga_print(14, 18, "[", VGA_LIGHT_GREY, VGA_BLACK);
    vga_print(14 + 52 + 1, 18, "]", VGA_LIGHT_GREY, VGA_BLACK);

    vga_print_center(22, "seeding from the quantum lab  ~  qBraid boot target", VGA_DARK_GREY,
                     VGA_BLACK);
}

void vga_boot_stage(const char *label, int percent) {
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    /* Animate a couple of wave frames so the pattern travels between
     * stages */
    for (int f = 0; f < 2; f++) {
        draw_wave(splash_frame++);
        frame_delay();
    }

    /* Progress bar: 52 cells wide starting at column 15 */
    int filled = (percent * 52) / 100;
    for (int i = 0; i < 52; i++) {
        vga_put(15 + i, 18, (i < filled) ? CH_BLOCK_FULL : CH_SHADE_LIGHT,
                (i < filled) ? VGA_LIGHT_GREEN : VGA_DARK_GREY, VGA_BLACK);
    }

    /* Percent readout + current stage label (clear the line first) */
    char pct[5];
    pct[0] = (char)('0' + (percent / 100) % 10);
    pct[1] = (char)('0' + (percent / 10) % 10);
    pct[2] = (char)('0' + percent % 10);
    pct[3] = '%';
    pct[4] = '\0';
    const char *p = pct;
    if (percent < 100)
        p = pct + 1; /* drop leading zero */
    vga_print_center(19, "                                        ", VGA_BLACK, VGA_BLACK);
    for (int i = 0; i < VGA_WIDTH; i++)
        vga_put(i, 20, ' ', VGA_BLACK, VGA_BLACK);
    vga_print(15, 20, label, VGA_LIGHT_CYAN, VGA_BLACK);
    vga_print(15 + 52 - str_len(p), 20, p, VGA_YELLOW, VGA_BLACK);
}

void vga_boot_ready(void) {
    for (int f = 0; f < 6; f++) {
        draw_wave(splash_frame++);
        frame_delay();
    }
    for (int i = 0; i < 52; i++) {
        vga_put(15 + i, 18, CH_BLOCK_FULL, VGA_LIGHT_GREEN, VGA_BLACK);
    }
    for (int i = 0; i < VGA_WIDTH; i++)
        vga_put(i, 20, ' ', VGA_BLACK, VGA_BLACK);
    vga_print_center(20, ">>  Q U A N T U M   O S   R E A D Y  <<", VGA_YELLOW, VGA_BLACK);
}
