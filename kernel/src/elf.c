/**
 * QuantumOS ELF64 Program Loader Implementation
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/elf.h>
#include <kernel/memory.h>
#include <kernel/boot.h>
#include <kernel/vmspace.h> /* USER_VBASE — confine segments to the user half */

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
            pmm_free_frame(frame); /* not mapped -> vmspace_destroy can't reclaim it */
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
    /* Program-header table bounds, overflow-safe (e_phoff is attacker-shaped):
     * a large e_phoff must not wrap the sum below `size` and pass. */
    if (eh->e_phoff > size || (uint64_t)eh->e_phnum * eh->e_phentsize > size - eh->e_phoff) {
        return ELF_ERR_RANGE;
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph =
            (const elf64_phdr_t *)(img + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != ELF_PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        /* File-extent bounds, overflow-safe: reject before load_segment forms
         * img + p_offset. The old `p_offset + p_filesz > size` add wrapped for a
         * huge p_offset, passing the check, then read kernel memory far outside
         * the image into a user-mapped page (info leak) or faulted the cli'd
         * syscall (panic). */
        if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset) {
            return ELF_ERR_RANGE;
        }
        /* Confine every segment to the private user half [USER_VBASE,0x80000000)
         * (PDPT[1], 4 KB pages). A p_vaddr below USER_VBASE would map through the
         * SHARED boot page directory's 2 MB PS pages: vmspace_map_page misreads a
         * 2 MB frame as a page table and corrupts arbitrary physical memory. The
         * upper-bound + wrap check also kills a p_vaddr+p_memsz overflow, and
         * p_filesz<=p_memsz keeps the file copy inside the segment. */
        uint64_t seg_end = ph->p_vaddr + ph->p_memsz; /* p_memsz != 0 (checked above) */
        if (ph->p_filesz > ph->p_memsz || ph->p_vaddr < USER_VBASE || seg_end > 0x80000000UL ||
            seg_end <= ph->p_vaddr) {
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
