/**
 * QuantumOS Embedded Initrd implementation (epic #62 phase 2, #64).
 *
 * A minimal ustar reader over the archive objcopy'd into the kernel
 * image. No allocation, no mutation: every lookup walks the archive's
 * 512-byte header blocks directly. The archive is produced by GNU tar
 * (--format=ustar) from rootfs/, so entry names carry a leading "./"
 * which is normalized away, directories carry typeflag '5', and sizes
 * are octal ASCII.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/initrd.h>
#include <kernel/tar.h>
#include <kernel/boot.h>

/* Embedded archive (build/x86_64/initrd_tar.o via objcopy). objcopy also
 * emits _binary_initrd_tar_size as an ABSOLUTE symbol whose address equals
 * the archive length; using it (a single symbol) avoids the end-minus-start
 * pointer subtraction that static analysis rejects as "different objects". */
extern const uint8_t _binary_initrd_tar_start[];
extern const uint8_t _binary_initrd_tar_size[];

#define TAR_BLOCK 512
#define TAR_NAME_OFF 0
#define TAR_NAME_LEN 100
#define TAR_SIZE_OFF 124
#define TAR_SIZE_LEN 12
#define TAR_TYPE_OFF 156
#define TAR_MAGIC_OFF 257

#define TAR_TYPE_FILE '0'
#define TAR_TYPE_FILE_ALT '\0' /* pre-POSIX archives use NUL for files */

static uint32_t initrd_files;
static uint32_t initrd_bytes;

/* Octal ASCII -> u32 (tar size fields; bounded, stops at NUL/space). */
static uint32_t tar_octal(const uint8_t *p, int len) {
    uint32_t v = 0;
    for (int i = 0; i < len && p[i] >= '0' && p[i] <= '7'; i++) {
        v = (v << 3) | (uint32_t)(p[i] - '0');
    }
    return v;
}

/* Strip the archive's "./" prefix and any leading '/'. */
static const char *normalize(const char *name) {
    if (name[0] == '.' && name[1] == '/') {
        name += 2;
    }
    while (name[0] == '/') {
        name++;
    }
    return name;
}

static int str_eq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

static int str_starts(const char *s, const char *prefix) {
    int i = 0;
    while (prefix[i] && s[i] == prefix[i]) {
        i++;
    }
    return prefix[i] == '\0';
}

/* Walk any in-memory ustar archive, invoking cb for each regular file
 * (cb may stop the walk by returning 1). Exported via kernel/tar.h:
 * the disk-restore path (epic #71) walks the persistence archive with
 * the same rules the embedded initrd gets. */
void tar_walk_mem(const uint8_t *base, size_t total, tar_visit_t cb, void *ctx) {
    size_t off = 0;

    while (off + TAR_BLOCK <= total) {
        const uint8_t *p = base + off;
        if (p[TAR_NAME_OFF] == '\0') {
            break; /* zero block: end of archive */
        }
        /* Header sanity: ustar magic (GNU writes "ustar " too) */
        if (p[TAR_MAGIC_OFF] != 'u') {
            break;
        }

        /* Header checksum: the sum of all 512 header bytes with the
         * 8-byte chksum field (148..155) counted as spaces must equal
         * the octal value stored there. This is what turns a torn sync
         * (old superblock over half-new data) from "restores garbage
         * under valid names" into "stops at the first bad block". */
        {
            uint32_t stored = tar_octal(p + 148, 8);
            uint32_t sum = 0;
            for (int i = 0; i < TAR_BLOCK; i++) {
                sum += (i >= 148 && i < 156) ? (uint32_t)' ' : (uint32_t)p[i];
            }
            if (sum != stored) {
                break; /* corrupt/torn header — end the walk honestly */
            }
        }

        /* Bounded copy of the name (the header field is not always
         * NUL-terminated at 100 chars). */
        char name[TAR_NAME_LEN + 1];
        for (int i = 0; i < TAR_NAME_LEN; i++) {
            name[i] = (char)p[TAR_NAME_OFF + i];
        }
        name[TAR_NAME_LEN] = '\0';

        uint32_t size = tar_octal(p + TAR_SIZE_OFF, TAR_SIZE_LEN);
        uint8_t type = p[TAR_TYPE_OFF];
        size_t data_off = off + TAR_BLOCK;

        /* Never let a corrupt size walk past the image (overflow-safe:
         * data_off <= total is guaranteed by the loop condition). */
        if (size > total - data_off) {
            break;
        }

        if (type == TAR_TYPE_FILE || type == TAR_TYPE_FILE_ALT) {
            if (cb(normalize(name), base + data_off, size, ctx)) {
                return;
            }
        }

        uint32_t blocks = (size + TAR_BLOCK - 1) / TAR_BLOCK;
        off = data_off + (size_t)blocks * TAR_BLOCK;
    }
}

/* The embedded archive, through the shared walker. Length comes from the
 * absolute _size symbol (a single symbol — no end-minus-start pointer
 * subtraction, which static analysis rejects as "different objects"). */
static void tar_walk(tar_visit_t cb, void *ctx) {
    tar_walk_mem(_binary_initrd_tar_start, (size_t)(uintptr_t)_binary_initrd_tar_size, cb, ctx);
}

/* ---- init: count the inventory ---- */

static int count_cb(const char *name, const uint8_t *data, uint32_t size, void *ctx) {
    (void)name;
    (void)data;
    (void)ctx;
    initrd_files++;
    initrd_bytes += size;
    return 0;
}

void initrd_init(void) {
    initrd_files = 0;
    initrd_bytes = 0;
    tar_walk(count_cb, NULL);
    boot_log("Initrd mounted (embedded ustar): files:");
    early_console_write_hex(initrd_files);
    boot_log("  bytes:");
    early_console_write_hex(initrd_bytes);
}

/* ---- lookup ---- */

typedef struct {
    const char *want;
    const uint8_t *data;
    uint32_t size;
    int found;
} lookup_ctx_t;

static int lookup_cb(const char *name, const uint8_t *data, uint32_t size, void *ctx) {
    lookup_ctx_t *l = (lookup_ctx_t *)ctx;
    if (str_eq(name, l->want)) {
        l->data = data;
        l->size = size;
        l->found = 1;
        return 1;
    }
    return 0;
}

int initrd_lookup(const char *path, const uint8_t **data_out, uint32_t *size_out) {
    if (!path || !data_out || !size_out) {
        return -1;
    }
    lookup_ctx_t l = {.want = normalize(path), .data = NULL, .size = 0, .found = 0};
    tar_walk(lookup_cb, &l);
    if (!l.found) {
        return -1;
    }
    *data_out = l.data;
    *size_out = l.size;
    return 0;
}

/* ---- listing ---- */

typedef struct {
    const char *prefix; /* normalized; "" lists everything */
    char *buf;
    size_t max;
    size_t o;
} list_ctx_t;

static int list_cb(const char *name, const uint8_t *data, uint32_t size, void *ctx) {
    (void)data;
    list_ctx_t *lc = (list_ctx_t *)ctx;

    if (lc->prefix[0] && !str_starts(name, lc->prefix)) {
        return 0;
    }

    /* Build "FS: <name> <size>\r\n" into a local row first so a
     * truncated row never lands in the output. */
    char row[TAR_NAME_LEN + 24];
    size_t r = 0;
    const char *pfx = "FS: ";
    while (*pfx) {
        row[r++] = *pfx++;
    }
    for (int i = 0; name[i] && r < sizeof(row) - 16; i++) {
        row[r++] = name[i];
    }
    row[r++] = ' ';
    char t[10];
    int n = 0;
    uint32_t v = size;
    if (v == 0) {
        t[n++] = '0';
    }
    while (v && n < (int)sizeof(t)) {
        t[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        row[r++] = t[--n];
    }
    row[r++] = '\r';
    row[r++] = '\n';

    if (lc->o + r + 1 > lc->max) {
        return 1; /* out of room — stop the walk at a whole row */
    }
    for (size_t i = 0; i < r; i++) {
        lc->buf[lc->o++] = row[i];
    }
    return 0;
}

size_t initrd_format_list(const char *path, char *buf, size_t max) {
    if (!buf || max == 0) {
        return 0;
    }
    list_ctx_t lc = {.prefix = normalize(path ? path : ""), .buf = buf, .max = max, .o = 0};
    tar_walk(list_cb, &lc);
    buf[lc.o] = '\0';
    return lc.o;
}
