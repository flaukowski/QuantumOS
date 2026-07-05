/**
 * QuantumOS Writable RAM Filesystem Overlay (epic #71 phase 2)
 *
 * The writable half of the VFS: a bounded table of kmalloc-backed files
 * layered over the read-only initrd. Lookups hit the overlay first, so
 * a written file shadows an initrd file of the same path; readdir
 * merges both. On SYS_SYNC the whole overlay serializes to the ATA disk
 * as a ustar archive and is restored from it at the next boot — this
 * table IS the OS's persistent state between syncs.
 *
 * Bounded on purpose: RAMFS_MAX_FILES files of RAMFS_FILE_MAX bytes,
 * allocated on create and freed on unlink. No directories — paths are
 * flat normalized strings, exactly like the initrd.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RAMFS_H
#define RAMFS_H

#include <kernel/types.h>

#define RAMFS_MAX_FILES 16
#define RAMFS_FILE_MAX (64 * 1024)
#define RAMFS_NAME_MAX 100 /* ustar name field — sync round-trips it */

/* Create (or truncate) a file. Returns the file index, or -1 (table
 * full, name too long, or out of memory). */
int ramfs_create(const char *path);

/* Append `len` bytes to file `idx`. Returns bytes appended (may be
 * short if the file cap is hit), or -1 on a bad index. */
int ramfs_append(int idx, const uint8_t *data, uint32_t len);

/* Find a file by path. On hit returns the index and points *data_out /
 * *size_out at the live bytes. Returns -1 if absent. The pointer stays
 * valid until the file is unlinked or truncated. */
int ramfs_lookup(const char *path, const uint8_t **data_out, uint32_t *size_out);

/* Remove a file and free its storage. Returns 0, or -1 if absent. The
 * caller (syscall layer) is responsible for refusing the unlink while
 * any live process holds the file open. */
int ramfs_unlink(const char *path);

/* Live file count. */
uint32_t ramfs_count(void);

/* Iterate for sync/list: fetch file `idx` (0..RAMFS_MAX_FILES-1).
 * Returns 0 and fills the out-params if the slot is live, -1 if not. */
int ramfs_get(int idx, const char **name_out, const uint8_t **data_out, uint32_t *size_out);

/* Append "FS: <name> <size> [ram]" console rows for overlay files under
 * `prefix` ("" lists all) to buf at offset o. Returns the new offset. */
size_t ramfs_format_list(const char *prefix, char *buf, size_t max, size_t o);

/* ---- Persistence (epic #71 phase 3) ----
 *
 * On-disk layout: LBA 0 is a superblock ("QDSK1" magic + the archive
 * byte count), LBA 1.. is the overlay serialized as a POSIX ustar
 * archive (valid checksums — the same format the initrd parser walks).
 * Sync writes the archive FIRST and the superblock LAST, so a crash
 * mid-sync leaves the previous superblock pointing at the previous
 * (intact) archive rather than at torn data. */

/* Serialize the overlay to the ATA disk. Returns the number of files
 * flushed, or -1 (no disk, archive would not fit, or I/O error). */
int persist_sync(void);

/* Restore the overlay from the disk archive at boot (no-op without a
 * disk or a valid superblock). Logs the outcome. */
void persist_restore(void);

#endif /* RAMFS_H */
