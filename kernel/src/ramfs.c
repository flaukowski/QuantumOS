/**
 * QuantumOS Writable RAM Filesystem Overlay implementation (epic #71
 * phase 2).
 *
 * A fixed table of kmalloc-backed files. Single CPU and every caller is
 * a cli'd syscall or the boot path, so no locking. Storage is allocated
 * at full RAMFS_FILE_MAX capacity on create (16 x 64 KiB = 1 MiB worst
 * case against a ~67 MB heap) and freed on unlink, keeping the append
 * path allocation-free and the data pointer stable for open fds.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/ramfs.h>
#include <kernel/memory.h>
#include <kernel/ata.h>
#include <kernel/tar.h>
#include <kernel/boot.h>

typedef struct {
    char name[RAMFS_NAME_MAX + 1];
    uint8_t *data;
    uint32_t size;
    bool used;
} ramfile_t;

static ramfile_t files[RAMFS_MAX_FILES];

/* Strip "./" and leading '/' — the same normalization the initrd uses,
 * so /data/note and data/note are one path across both layers. */
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

static int find_by_name(const char *norm) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (files[i].used && str_eq(files[i].name, norm)) {
            return i;
        }
    }
    return -1;
}

int ramfs_create(const char *path) {
    if (!path) {
        return -1;
    }
    const char *norm = normalize(path);
    if (!norm[0]) {
        return -1;
    }

    size_t len = 0;
    while (norm[len]) {
        len++;
    }
    if (len > RAMFS_NAME_MAX) {
        return -1;
    }

    /* Existing file: truncate in place (data pointer stays valid for
     * any open fd; its recorded size snapshot just goes stale, exactly
     * like a POSIX file truncated behind a reader). */
    int idx = find_by_name(norm);
    if (idx >= 0) {
        files[idx].size = 0;
        return idx;
    }

    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].used) {
            uint8_t *buf = kmalloc(RAMFS_FILE_MAX);
            if (!buf) {
                return -1;
            }
            for (size_t c = 0; c <= len; c++) {
                files[i].name[c] = norm[c];
            }
            files[i].data = buf;
            files[i].size = 0;
            files[i].used = true;
            return i;
        }
    }
    return -1; /* table full */
}

int ramfs_append(int idx, const uint8_t *data, uint32_t len) {
    if (idx < 0 || idx >= RAMFS_MAX_FILES || !files[idx].used || !data) {
        return -1;
    }
    uint32_t room = RAMFS_FILE_MAX - files[idx].size;
    if (len > room) {
        len = room;
    }
    for (uint32_t i = 0; i < len; i++) {
        files[idx].data[files[idx].size + i] = data[i];
    }
    files[idx].size += len;
    return (int)len;
}

int ramfs_lookup(const char *path, const uint8_t **data_out, uint32_t *size_out) {
    if (!path || !data_out || !size_out) {
        return -1;
    }
    int idx = find_by_name(normalize(path));
    if (idx < 0) {
        return -1;
    }
    *data_out = files[idx].data;
    *size_out = files[idx].size;
    return idx;
}

int ramfs_unlink(const char *path) {
    if (!path) {
        return -1;
    }
    int idx = find_by_name(normalize(path));
    if (idx < 0) {
        return -1;
    }
    kfree(files[idx].data);
    files[idx].data = NULL;
    files[idx].size = 0;
    files[idx].used = false;
    files[idx].name[0] = '\0';
    return 0;
}

uint32_t ramfs_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (files[i].used) {
            n++;
        }
    }
    return n;
}

int ramfs_get(int idx, const char **name_out, const uint8_t **data_out, uint32_t *size_out) {
    if (idx < 0 || idx >= RAMFS_MAX_FILES || !files[idx].used) {
        return -1;
    }
    if (name_out) {
        *name_out = files[idx].name;
    }
    if (data_out) {
        *data_out = files[idx].data;
    }
    if (size_out) {
        *size_out = files[idx].size;
    }
    return 0;
}

/* ============================================================================
 * Persistence (epic #71 phase 3): overlay <-> ATA disk as ustar
 * ============================================================================ */

#define QDSK_MAGIC "QDSK1"
#define QDSK_MAGIC_LEN 5
#define QDSK_TARBYTES_OFF 8   /* u32: archive byte count */
#define QDSK_FILECOUNT_OFF 12 /* u32: files in the archive (informational) */
#define QDSK_CHECKSUM_OFF 16  /* u32: additive checksum over the archive */
#define TAR_BLOCK_SZ 512

/* Simple additive checksum over the archive. Written into the
 * superblock only after the archive itself is on disk, so a torn sync
 * (new superblock never reached) leaves the OLD superblock+archive
 * intact, and a torn archive write is caught by the mismatch at restore
 * rather than half-restored as truth. */
static uint32_t archive_checksum(const uint8_t *buf, uint32_t len) {
    uint32_t sum = 0x517E7A11u;
    for (uint32_t i = 0; i < len; i++) {
        sum = (sum << 1 | sum >> 31) ^ buf[i];
    }
    return sum;
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Write an 11-digit octal field (POSIX size/mtime format), NUL-closed. */
static void tar_put_octal(uint8_t *field, uint32_t v) {
    for (int i = 10; i >= 0; i--) {
        field[i] = (uint8_t)('0' + (v & 7));
        v >>= 3;
    }
    field[11] = '\0';
}

/* Compose one POSIX ustar header (valid checksum) for a regular file. */
static void tar_put_header(uint8_t *h, const char *name, uint32_t size) {
    for (int i = 0; i < TAR_BLOCK_SZ; i++) {
        h[i] = 0;
    }
    /* name (bounded by RAMFS_NAME_MAX <= 100) */
    for (int i = 0; i < 100 && name[i]; i++) {
        h[i] = (uint8_t)name[i];
    }
    /* mode/uid/gid: octal ASCII */
    const char *mode = "0000644";
    const char *zero7 = "0000000";
    for (int i = 0; i < 7; i++) {
        h[100 + i] = (uint8_t)mode[i];  /* mode */
        h[108 + i] = (uint8_t)zero7[i]; /* uid */
        h[116 + i] = (uint8_t)zero7[i]; /* gid */
    }
    tar_put_octal(h + 124, size); /* size */
    tar_put_octal(h + 136, 0);    /* mtime */
    h[156] = '0';                 /* typeflag: regular file */
    const char *magic = "ustar";
    for (int i = 0; i < 5; i++) {
        h[257 + i] = (uint8_t)magic[i];
    }
    h[262] = '\0';
    h[263] = '0'; /* version "00" */
    h[264] = '0';

    /* Checksum: sum of all header bytes with the chksum field (148..155)
     * read as spaces; stored as 6 octal digits + NUL + space. */
    for (int i = 0; i < 8; i++) {
        h[148 + i] = ' ';
    }
    uint32_t sum = 0;
    for (int i = 0; i < TAR_BLOCK_SZ; i++) {
        sum += h[i];
    }
    for (int i = 5; i >= 0; i--) {
        h[148 + i] = (uint8_t)('0' + (sum & 7));
        sum >>= 3;
    }
    h[154] = '\0';
    h[155] = ' ';
}

static uint32_t align_block(uint32_t v) {
    return (v + TAR_BLOCK_SZ - 1) & ~(uint32_t)(TAR_BLOCK_SZ - 1);
}

int persist_sync(void) {
    if (!ata_present()) {
        return -1;
    }

    /* Size the archive: header + block-aligned data per file, plus the
     * two terminating zero blocks. */
    uint32_t tar_bytes = 2 * TAR_BLOCK_SZ;
    uint32_t nfiles = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        const uint8_t *d;
        uint32_t sz;
        const char *nm;
        if (ramfs_get(i, &nm, &d, &sz) == 0) {
            tar_bytes += TAR_BLOCK_SZ + align_block(sz);
            nfiles++;
        }
    }

    uint32_t tar_sectors = tar_bytes / ATA_SECTOR_SIZE;
    /* Fit check: superblock + archive, leaving the last sector free for
     * the boot-time RW self-test scratch. */
    if (1 + tar_sectors + 1 > ata_sector_count()) {
        boot_log("SYNC: archive does not fit on disk — not flushed");
        return -1;
    }

    uint8_t *buf = kmalloc(tar_bytes);
    if (!buf) {
        boot_log("SYNC: out of kernel heap — not flushed");
        return -1;
    }
    for (uint32_t i = 0; i < tar_bytes; i++) {
        buf[i] = 0;
    }

    uint32_t off = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        const uint8_t *d;
        uint32_t sz;
        const char *nm;
        if (ramfs_get(i, &nm, &d, &sz) != 0) {
            continue;
        }
        tar_put_header(buf + off, nm, sz);
        off += TAR_BLOCK_SZ;
        for (uint32_t b = 0; b < sz; b++) {
            buf[off + b] = d[b];
        }
        off += align_block(sz);
    }

    uint32_t csum = archive_checksum(buf, tar_bytes);

    /* Archive first (flushed to disk by ata_write), superblock last: a
     * crash before the superblock leaves the OLD superblock pointing at
     * the OLD intact archive; a torn archive write is caught by the
     * checksum mismatch at restore. */
    if (ata_write(1, buf, tar_sectors) != 0) {
        kfree(buf);
        boot_log("SYNC: disk write failed — not flushed");
        return -1;
    }
    kfree(buf);

    uint8_t sb[ATA_SECTOR_SIZE];
    for (int i = 0; i < ATA_SECTOR_SIZE; i++) {
        sb[i] = 0;
    }
    const char *magic = QDSK_MAGIC;
    for (int i = 0; i < QDSK_MAGIC_LEN; i++) {
        sb[i] = (uint8_t)magic[i];
    }
    put_u32(sb + QDSK_TARBYTES_OFF, tar_bytes);
    put_u32(sb + QDSK_FILECOUNT_OFF, nfiles);
    put_u32(sb + QDSK_CHECKSUM_OFF, csum);
    if (ata_write(0, sb, 1) != 0) {
        boot_log("SYNC: superblock write failed — not flushed");
        return -1;
    }

    boot_log("SYNC: overlay flushed to disk, files:");
    early_console_write_hex(nfiles);
    return (int)nfiles;
}

static int restore_cb(const char *name, const uint8_t *data, uint32_t size, void *ctx) {
    uint32_t *count = (uint32_t *)ctx;
    int idx = ramfs_create(name);
    if (idx >= 0) {
        ramfs_append(idx, data, size);
        (*count)++;
    }
    return 0;
}

void persist_restore(void) {
    if (!ata_present()) {
        return;
    }

    uint8_t sb[ATA_SECTOR_SIZE];
    if (ata_read(0, sb, 1) != 0) {
        boot_log("FS: superblock read failed — starting empty");
        return;
    }

    const char *magic = QDSK_MAGIC;
    for (int i = 0; i < QDSK_MAGIC_LEN; i++) {
        if (sb[i] != (uint8_t)magic[i]) {
            boot_log("FS: no persisted archive on disk (fresh volume)");
            return;
        }
    }

    uint32_t tar_bytes = get_u32(sb + QDSK_TARBYTES_OFF);
    uint32_t want_csum = get_u32(sb + QDSK_CHECKSUM_OFF);

    /* Sanity BEFORE the kmalloc: the archive can never legitimately
     * exceed the overlay's worst case nor the disk that holds it (a
     * scribbled superblock could otherwise demand a ~4 GB allocation). */
    uint32_t worst = (uint32_t)RAMFS_MAX_FILES * (TAR_BLOCK_SZ + RAMFS_FILE_MAX) + 2 * TAR_BLOCK_SZ;
    if (tar_bytes < 2 * TAR_BLOCK_SZ || tar_bytes % TAR_BLOCK_SZ != 0 || tar_bytes > worst ||
        tar_bytes / ATA_SECTOR_SIZE > ata_sector_count() - 1) {
        boot_log("FS: superblock names an implausible archive — starting empty");
        return;
    }

    uint8_t *buf = kmalloc(tar_bytes);
    if (!buf) {
        boot_log("FS: out of kernel heap for restore — starting empty");
        return;
    }
    if (ata_read(1, buf, tar_bytes / ATA_SECTOR_SIZE) != 0) {
        kfree(buf);
        boot_log("FS: archive read failed — starting empty");
        return;
    }

    /* Whole-archive checksum: catches a torn sync (superblock committed
     * but archive write interrupted) that the per-header check might not.
     * Mismatch -> start empty rather than restore corruption as truth. */
    if (archive_checksum(buf, tar_bytes) != want_csum) {
        kfree(buf);
        boot_log("FS: persisted archive checksum mismatch (torn sync?) — starting empty");
        return;
    }

    /* restore_cb routes every entry through ramfs_create/append, so the
     * 100-char name cap, 64 KB size cap, 16-file cap, and normalize()
     * all apply — a hostile archive can create no file the syscall path
     * could not. tar_walk_mem also verifies each header checksum. */
    uint32_t restored = 0;
    tar_walk_mem(buf, tar_bytes, restore_cb, &restored);
    kfree(buf);

    boot_log("FS: restored persisted files from disk:");
    early_console_write_hex(restored);
}

size_t ramfs_format_list(const char *prefix, char *buf, size_t max, size_t o) {
    const char *pref = normalize(prefix ? prefix : "");

    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].used) {
            continue;
        }
        if (pref[0] && !str_starts(files[i].name, pref)) {
            continue;
        }

        /* Build "FS: <name> <size> [ram]\r\n" as a whole row. */
        char row[RAMFS_NAME_MAX + 32];
        size_t r = 0;
        const char *pfx = "FS: ";
        while (*pfx) {
            row[r++] = *pfx++;
        }
        for (int c = 0; files[i].name[c] && r < sizeof(row) - 24; c++) {
            row[r++] = files[i].name[c];
        }
        row[r++] = ' ';
        char t[10];
        int n = 0;
        uint32_t v = files[i].size;
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
        const char *tag = " [ram]\r\n";
        while (*tag) {
            row[r++] = *tag++;
        }

        if (o + r + 1 > max) {
            break;
        }
        for (size_t c = 0; c < r; c++) {
            buf[o++] = row[c];
        }
    }
    buf[o] = '\0';
    return o;
}
