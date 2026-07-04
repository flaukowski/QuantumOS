/**
 * QuantumOS ELF64 Program Loader Implementation
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/elf.h>
#include <kernel/memory.h>
#include <kernel/boot.h>

static uint64_t align_down_page(uint64_t v) {
    return v & ~((uint64_t)PAGE_SIZE - 1);
}

/* Map + populate one PT_LOAD segment into `as`. Allocates a fresh
 * frame per page (segments are page-aligned by the user link script,
 * so no page is shared between segments), copies the file-backed
 * bytes, and leaves the remainder (bss) zeroed. */
static elf_result_t load_segment(address_space_t *as, const uint8_t *img, const elf64_phdr_t *ph) {
    uint64_t seg_start = ph->p_vaddr;
    uint64_t file_end = ph->p_vaddr + ph->p_filesz;
    uint64_t mem_end = ph->p_vaddr + ph->p_memsz;
    bool writable = (ph->p_flags & ELF_PF_W) != 0;

    for (uint64_t page = align_down_page(seg_start); page < mem_end; page += PAGE_SIZE) {
        void *frame = pmm_alloc_frame();
        if (!frame) {
            return ELF_ERR_NOMEM;
        }
        memset(frame, 0, PAGE_SIZE);

        uint8_t *dst = (uint8_t *)frame; /* identity-mapped kernel VA */
        for (uint64_t off = 0; off < PAGE_SIZE; off++) {
            uint64_t va = page + off;
            if (va < seg_start || va >= mem_end) {
                continue;
            }
            if (va < file_end) {
                dst[off] = img[ph->p_offset + (va - seg_start)];
            }
            /* else: bss, already zero */
        }

        if (!vmspace_map_page(as, page, (uint64_t)frame, writable)) {
            return ELF_ERR_NOMEM;
        }
    }
    return ELF_OK;
}

elf_result_t elf_load(address_space_t *as, const uint8_t *img, size_t size, uint64_t *entry_out) {
    if (size < sizeof(elf64_ehdr_t)) {
        return ELF_ERR_RANGE;
    }
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)img;

    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' ||
        eh->e_ident[3] != 'F') {
        return ELF_ERR_MAGIC;
    }
    if (eh->e_ident[4] != 2 /* ELFCLASS64 */) {
        return ELF_ERR_CLASS;
    }
    if (eh->e_type != 2 /* ET_EXEC */) {
        return ELF_ERR_TYPE;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) {
        return ELF_ERR_RANGE;
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph =
            (const elf64_phdr_t *)(img + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != ELF_PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (ph->p_offset + ph->p_filesz > size) {
            return ELF_ERR_RANGE;
        }
        elf_result_t r = load_segment(as, img, ph);
        if (r != ELF_OK) {
            return r;
        }
    }

    *entry_out = eh->e_entry;
    return ELF_OK;
}
