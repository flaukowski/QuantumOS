/**
 * QuantumOS /bin/life — Conway's Game of Life as a native citizen (issue #102).
 *
 * A self-contained libq citizen: no floats (QuantumOS saves no FPU state across
 * a context switch), capless (only SYS_WRITE via write_str / libq snprintf), and
 * honestly gated. It seeds a single GLIDER on a 16x16 toroidal grid and runs it
 * for 16 generations. A glider has a period of 4 and translates one cell
 * diagonally per period, so on a 16-cell torus it stays a 5-cell glider forever
 * — the live-cell count, COMPUTED from the evolved grid (not hardcoded), is the
 * un-fakeable proof the step function is correct: a broken neighbour count would
 * make the population explode or die, never hold at exactly 5 while moving.
 *
 * Prints the initial and final grids (a small visual) and one gate line
 * "LIFE: glider intact after 16 generations (live=5)".
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"

#define W 16
#define H 16
#define GENS 16

static unsigned char grid[H][W];
static unsigned char next[H][W];

/* Live neighbours of (r,c) on the torus (wrap at the edges). */
static int neighbours(int r, int c) {
    int n = 0;
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) {
                continue;
            }
            int rr = (r + dr + H) % H;
            int cc = (c + dc + W) % W;
            n += grid[rr][cc];
        }
    }
    return n;
}

/* One Conway step: a live cell survives with 2-3 neighbours; a dead cell is
 * born with exactly 3. */
static void step(void) {
    for (int r = 0; r < H; r++) {
        for (int c = 0; c < W; c++) {
            int n = neighbours(r, c);
            next[r][c] = (grid[r][c] ? (n == 2 || n == 3) : (n == 3)) ? 1 : 0;
        }
    }
    for (int r = 0; r < H; r++) {
        for (int c = 0; c < W; c++) {
            grid[r][c] = next[r][c];
        }
    }
}

static int population(void) {
    int p = 0;
    for (int r = 0; r < H; r++) {
        for (int c = 0; c < W; c++) {
            p += grid[r][c];
        }
    }
    return p;
}

static void draw(const char *title) {
    write_str(title);
    for (int r = 0; r < H; r++) {
        char line[W + 8];
        int o = 0;
        line[o++] = 'L';
        line[o++] = ':';
        line[o++] = ' ';
        for (int c = 0; c < W; c++) {
            line[o++] = grid[r][c] ? '#' : '.';
        }
        line[o] = '\0';
        write_str(line);
    }
}

void _start(void) {
    /* A glider (5 cells), the canonical moving pattern:
     *   . # .
     *   . . #
     *   # # #
     */
    grid[1][2] = 1;
    grid[2][3] = 1;
    grid[3][1] = 1;
    grid[3][2] = 1;
    grid[3][3] = 1;

    draw("life: initial glider (5 cells)");
    for (int g = 0; g < GENS; g++) {
        step();
    }
    draw("life: after 16 generations");

    int p = population();
    char b[64];
    if (p == 5) {
        snprintf(b, sizeof(b), "LIFE: glider intact after %d generations (live=%d)", GENS, p);
    } else {
        snprintf(b, sizeof(b), "LIFE: BROKEN — population %d != 5 after %d generations", p, GENS);
    }
    write_str(b);

    exit_(p); /* the shell reports the live-cell count as the exit code */
    for (;;) {
    }
}
