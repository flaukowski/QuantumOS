/**
 * QuantumOS ustar walking (shared) — epic #71.
 *
 * The initrd (epic #62) and the persistence archive on the ATA disk
 * (epic #71) are the same format: ustar. The walker lives in initrd.c
 * (which owns the parsing rules: "./" normalization, corrupt-size
 * guards, offset arithmetic) and is exported here so the disk-restore
 * path reuses it over an arbitrary in-memory archive.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef TAR_H
#define TAR_H

#include <kernel/types.h>

/* Visitor: called once per regular file with the normalized name and a
 * pointer into the archive. Return 1 to stop the walk, 0 to continue. */
typedef int (*tar_visit_t)(const char *name, const uint8_t *data, uint32_t size, void *ctx);

/* Walk a ustar archive of `total` bytes at `base`. Bounded and
 * corruption-tolerant: a bad header or size ends the walk. */
void tar_walk_mem(const uint8_t *base, size_t total, tar_visit_t cb, void *ctx);

#endif /* TAR_H */
