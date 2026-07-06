/**
 * QuantumOS libq — heap allocator (static-arena, first-fit).
 *
 * One per-process bss arena (LIBQ_HEAP_SIZE, 16-byte aligned, zero at exec)
 * carved by an explicit free list with boundary tags and immediate
 * bidirectional coalescing. No SYS_BRK: v1 is arena-only, which keeps the
 * ring-3 ABI frozen; the malloc/free/calloc/realloc contract is stable across
 * a future growable-heap swap.
 *
 * Block layout (all sizes are multiples of 16):
 *   [ header 16B: { size_t size; size_t used } ][ payload ... ][ footer 8B ]
 *   - `used` is a SEPARATE field, never packed into size, so
 *     next = (uint8_t *)hdr + hdr->size is always exact.
 *   - the footer mirrors `size` on EVERY block, for backward coalescing.
 *   - a FREE block additionally stores { next, prev } free-list links in the
 *     first 16 payload bytes.
 * Used-block overhead is 24 bytes; a request of n bytes needs a block of at
 * least align16(n + 24), floored to MIN_BLOCK.
 *
 * heap.o is NOT under the mem.o/str.o self-recursion guard: it legitimately
 * emits external memset/memcpy calls, which mem.o satisfies.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq.h"

typedef struct block {
    size_t size; /* total block bytes incl header+footer, multiple of 16 */
    size_t used; /* 0 = free, 1 = in use */
} block_t;

#define HDR 16u                   /* header size; 16-aligned payload follows */
#define FTR 8u                    /* footer: size mirror */
#define USED_OVERHEAD (HDR + FTR) /* 24 */
#define MIN_BLOCK 48u             /* 16 hdr + 16 free-list links + 8 footer */

_Static_assert(sizeof(block_t) == HDR, "block header must be 16 bytes");
_Static_assert(LIBQ_HEAP_SIZE >= 4u * MIN_BLOCK, "heap too small for the allocator");

static uint8_t libq_heap[LIBQ_HEAP_SIZE] __attribute__((aligned(16)));
static size_t heap_usable; /* LIBQ_HEAP_SIZE floored to a multiple of 16 */
static block_t *free_head;
static int inited;

static size_t align16(size_t n) {
    return (n + 15u) & ~(size_t)15u;
}
static size_t *footer_of(block_t *b) {
    return (size_t *)((uint8_t *)b + b->size - FTR);
}
static block_t **fl_next(block_t *b) {
    return (block_t **)((uint8_t *)b + HDR);
}
static block_t **fl_prev(block_t *b) {
    return (block_t **)((uint8_t *)b + HDR + sizeof(block_t *));
}

static void set_block(block_t *b, size_t size, size_t used) {
    b->size = size;
    b->used = used;
    *footer_of(b) = size;
}

static void fl_insert(block_t *b) {
    *fl_next(b) = free_head;
    *fl_prev(b) = NULL;
    if (free_head) {
        *fl_prev(free_head) = b;
    }
    free_head = b;
}
static void fl_remove(block_t *b) {
    block_t *n = *fl_next(b);
    block_t *p = *fl_prev(b);
    if (p) {
        *fl_next(p) = n;
    } else {
        free_head = n;
    }
    if (n) {
        *fl_prev(n) = p;
    }
}

static void heap_init(void) {
    heap_usable = LIBQ_HEAP_SIZE & ~(size_t)15u;
    free_head = NULL;
    block_t *b = (block_t *)libq_heap;
    set_block(b, heap_usable, 0);
    fl_insert(b);
    inited = 1;
}

/* Reject a free()/realloc() pointer whose header fails any consistency check,
 * so a stray or double-freed pointer fails safe instead of corrupting the
 * arena. Agreeing size/footer tags almost never match reused garbage. */
static int valid_used_block(block_t *b) {
    uint8_t *p = (uint8_t *)b;
    if (p < libq_heap || p > libq_heap + heap_usable - MIN_BLOCK) {
        return 0;
    }
    if ((size_t)(p - libq_heap) % 16u != 0) {
        return 0;
    }
    if (b->used != 1) {
        return 0;
    }
    if (b->size % 16u != 0 || b->size < MIN_BLOCK || b->size > heap_usable) {
        return 0;
    }
    if (p + b->size > libq_heap + heap_usable) {
        return 0;
    }
    if (*footer_of(b) != b->size) {
        return 0;
    }
    return 1;
}

void *malloc(size_t n) {
    if (n == 0) {
        return NULL;
    }
    if (!inited) {
        heap_init();
    }
    if (n > heap_usable) { /* also blocks the n + USED_OVERHEAD overflow */
        return NULL;
    }
    size_t need = align16(n + USED_OVERHEAD);
    if (need < MIN_BLOCK) {
        need = MIN_BLOCK;
    }
    for (block_t *b = free_head; b != NULL; b = *fl_next(b)) {
        if (b->size < need) {
            continue;
        }
        fl_remove(b);
        if (b->size - need >= MIN_BLOCK) {
            size_t rem = b->size - need;
            set_block(b, need, 1);
            block_t *r = (block_t *)((uint8_t *)b + need);
            set_block(r, rem, 0);
            fl_insert(r);
        } else {
            b->used = 1; /* take the whole block; footer already correct */
        }
        return (uint8_t *)b + HDR;
    }
    return NULL; /* out of memory: no free-list mutation */
}

void free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    block_t *b = (block_t *)((uint8_t *)ptr - HDR);
    if (!inited || !valid_used_block(b)) {
        return; /* fail safe on a detectably-corrupt pointer / double free */
    }
    b->used = 0;

    /* coalesce forward (strict < : at the tail next == arena end, never read) */
    uint8_t *next = (uint8_t *)b + b->size;
    if (next < libq_heap + heap_usable) {
        block_t *nb = (block_t *)next;
        if (nb->used == 0) {
            fl_remove(nb);
            set_block(b, b->size + nb->size, 0);
        }
    }

    /* coalesce backward (guard b > heap : the front block never reads [-8]) */
    if ((uint8_t *)b > libq_heap) {
        size_t ps = *(size_t *)((uint8_t *)b - FTR);
        block_t *pb = (block_t *)((uint8_t *)b - ps);
        if ((uint8_t *)pb >= libq_heap && pb->used == 0) {
            fl_remove(pb);
            set_block(pb, pb->size + b->size, 0);
            b = pb;
        }
    }

    set_block(b, b->size, 0); /* ensure the merged block's footer is coherent */
    fl_insert(b);
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        return NULL;
    }
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p != NULL) {
        memset(p, 0, total); /* never assume bss-zero on a reused block */
    }
    return p;
}

void *realloc(void *ptr, size_t n) {
    if (ptr == NULL) {
        return malloc(n);
    }
    if (n == 0) {
        free(ptr);
        return NULL;
    }
    block_t *b = (block_t *)((uint8_t *)ptr - HDR);
    if (!inited || !valid_used_block(b)) {
        return NULL;
    }
    size_t need = align16(n + USED_OVERHEAD);
    if (need < MIN_BLOCK) {
        need = MIN_BLOCK;
    }
    if (need <= b->size) {
        return ptr; /* v1 keeps the slack rather than splitting on shrink */
    }
    /* try to grow in place into a free forward neighbour */
    uint8_t *next = (uint8_t *)b + b->size;
    if (next < libq_heap + heap_usable) {
        block_t *nb = (block_t *)next;
        if (nb->used == 0 && b->size + nb->size >= need) {
            fl_remove(nb);
            size_t merged = b->size + nb->size;
            if (merged - need >= MIN_BLOCK) {
                set_block(b, need, 1);
                block_t *r = (block_t *)((uint8_t *)b + need);
                set_block(r, merged - need, 0);
                fl_insert(r);
            } else {
                set_block(b, merged, 1);
            }
            return ptr;
        }
    }
    /* relocate; the original is preserved if the new allocation fails */
    void *q = malloc(n);
    if (q == NULL) {
        return NULL;
    }
    size_t old_payload = b->size - USED_OVERHEAD;
    size_t copy = (old_payload < n) ? old_payload : n;
    memcpy(q, ptr, copy);
    free(ptr);
    return q;
}
