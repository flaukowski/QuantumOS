/**
 * QuantumOS ELF64 Program Loader
 *
 * Loads a statically-linked ET_EXEC ELF64 image into a per-process
 * address space by mapping and populating its PT_LOAD segments. Used
 * to run real compiled user programs (embedded in the kernel image)
 * instead of hand-written position-independent blobs.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef ELF_H
#define ELF_H

#include <kernel/types.h>
#include <kernel/vmspace.h>

/* Minimal ELF64 definitions (little-endian, x86-64) */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

#define ELF_PT_LOAD   1
#define ELF_PF_X      0x1
#define ELF_PF_W      0x2
#define ELF_PF_R      0x4

typedef enum {
    ELF_OK = 0,
    ELF_ERR_MAGIC = -1,
    ELF_ERR_CLASS = -2,
    ELF_ERR_TYPE = -3,
    ELF_ERR_RANGE = -4,
    ELF_ERR_NOMEM = -5
} elf_result_t;

/* Parse and load `img` (size bytes, readable in kernel memory) into the
 * address space `as`, mapping/populating every PT_LOAD segment. Writes
 * the entry point to *entry_out. Segments are mapped user-accessible;
 * writable per PF_W. */
elf_result_t elf_load(address_space_t *as, const uint8_t *img, size_t size,
                      uint64_t *entry_out);

#endif /* ELF_H */
