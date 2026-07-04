/**
 * QuantumOS Embedded Initrd (epic #62 phase 2, #64)
 *
 * A read-only ustar archive built from rootfs/ at compile time and
 * embedded in the kernel image (objcopy, like the user ELFs). This is
 * the OS's first filesystem: enough VFS to open, read, and list — the
 * substrate the shell's ls/cat and (phase 3) SYS_SPAWN stand on.
 *
 * Paths are normalized: a leading '/' or './' is ignored, so
 * "/etc/motd", "etc/motd" and "./etc/motd" name the same entry.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef INITRD_H
#define INITRD_H

#include <kernel/types.h>

/* Parse the embedded archive and log its inventory. Safe to call once
 * at boot, before user processes exist. */
void initrd_init(void);

/* Find a regular file by path. On hit returns 0 and points *data_out at
 * the file's bytes inside the embedded image (read-only, lives forever)
 * with *size_out its length. Returns -1 if the path names no file. */
int initrd_lookup(const char *path, const uint8_t **data_out, uint32_t *size_out);

/* Format a directory listing as console text — one
 * "FS: <path> <size>" line per regular file under `path` ("" or "/"
 * lists everything). Bounded by max, NUL-terminated; returns bytes
 * written (excluding the NUL). */
size_t initrd_format_list(const char *path, char *buf, size_t max);

#endif /* INITRD_H */
