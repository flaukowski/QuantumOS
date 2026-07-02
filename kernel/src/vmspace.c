/**
 * QuantumOS Per-Process Address Spaces Implementation
 *
 * Physical frames are identity-mapped in the kernel half, so a frame's
 * physical address doubles as the kernel VA used to fill its entries.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/vmspace.h>
#include <kernel/memory.h>
#include <kernel/boot.h>

/* Boot page tables (kernel/src/boot.S) — the shared kernel half */
extern uint64_t boot_pml4[];
extern uint64_t boot_pd[];

/* CR3 helpers (kernel/src/interrupts.S) */
extern uint64_t get_cr3(void);
extern void set_cr3(uint64_t cr3);

/* Paging entry flags */
#define PG_PRESENT  0x001
#define PG_RW       0x002
#define PG_USER     0x004
#define PG_ADDR     0x000FFFFFFFFFF000ULL

static uint64_t *alloc_table(void) {
    void *frame = pmm_alloc_frame();
    if (!frame) {
        return NULL;
    }
    /* Frame is physical == identity-mapped kernel VA */
    memset(frame, 0, PAGE_SIZE);
    return (uint64_t *)frame;
}

uint64_t vmspace_kernel_cr3(void) {
    return (uint64_t)boot_pml4;
}

address_space_t vmspace_create(void) {
    address_space_t as = { NULL, 0 };

    uint64_t *pml4 = alloc_table();
    if (!pml4) {
        return as;
    }
    uint64_t *pdpt = alloc_table();
    if (!pdpt) {
        return as; /* frame leaks; kfree is a no-op anyway */
    }

    /* PML4[0] -> our PDPT (user bit on so the walker reaches user
     * pages; leaf pages enforce supervisor/user individually) */
    pml4[0] = ((uint64_t)pdpt & PG_ADDR) | PG_PRESENT | PG_RW | PG_USER;

    /* PDPT[0] -> the shared boot page directory: identity map of the
     * low 1 GB with supervisor-only 2 MB pages (the kernel half) */
    pdpt[0] = ((uint64_t)boot_pd & PG_ADDR) | PG_PRESENT | PG_RW | PG_USER;

    /* PDPT[1] (user half, 1–2 GB) created lazily in map_page */

    as.pml4 = pml4;
    as.cr3 = (uint64_t)pml4;
    return as;
}

bool vmspace_map_page(address_space_t *as, uint64_t uvaddr,
                      uint64_t paddr, bool writable) {
    if (!as || !as->pml4) {
        return false;
    }

    uint64_t pdpt_index = (uvaddr >> 30) & 0x1FF;
    uint64_t pd_index = (uvaddr >> 21) & 0x1FF;
    uint64_t pt_index = (uvaddr >> 12) & 0x1FF;

    uint64_t *pdpt = (uint64_t *)(as->pml4[0] & PG_ADDR);

    /* Page directory for this PDPT slot */
    if (!(pdpt[pdpt_index] & PG_PRESENT)) {
        uint64_t *pd = alloc_table();
        if (!pd) {
            return false;
        }
        pdpt[pdpt_index] = ((uint64_t)pd & PG_ADDR) | PG_PRESENT | PG_RW | PG_USER;
    }
    uint64_t *pd = (uint64_t *)(pdpt[pdpt_index] & PG_ADDR);

    /* Page table for this PD slot */
    if (!(pd[pd_index] & PG_PRESENT)) {
        uint64_t *pt = alloc_table();
        if (!pt) {
            return false;
        }
        pd[pd_index] = ((uint64_t)pt & PG_ADDR) | PG_PRESENT | PG_RW | PG_USER;
    }
    uint64_t *pt = (uint64_t *)(pd[pd_index] & PG_ADDR);

    uint64_t flags = PG_PRESENT | PG_USER | (writable ? PG_RW : 0);
    pt[pt_index] = (paddr & PG_ADDR) | flags;
    return true;
}

void vmspace_switch(uint64_t cr3) {
    if (cr3 && cr3 != get_cr3()) {
        set_cr3(cr3);
    }
}

void vmspace_destroy(uint64_t *pml4) {
    if (!pml4) {
        return;
    }

    uint64_t *pdpt = (uint64_t *)(pml4[0] & PG_ADDR);
    if (pdpt) {
        /* Walk the user half only. PDPT[0] points at the SHARED boot
         * page directory (the kernel half) — never free it. */
        for (int pi = 1; pi < 512; pi++) {
            if (!(pdpt[pi] & PG_PRESENT)) {
                continue;
            }
            uint64_t *pd = (uint64_t *)(pdpt[pi] & PG_ADDR);
            for (int di = 0; di < 512; di++) {
                if (!(pd[di] & PG_PRESENT)) {
                    continue;
                }
                uint64_t *pt = (uint64_t *)(pd[di] & PG_ADDR);
                for (int ti = 0; ti < 512; ti++) {
                    if (pt[ti] & PG_PRESENT) {
                        pmm_free_frame((void *)(pt[ti] & PG_ADDR)); /* user page */
                    }
                }
                pmm_free_frame(pt);   /* page table */
            }
            pmm_free_frame(pd);       /* page directory */
        }
        pmm_free_frame(pdpt);         /* PDPT */
    }
    pmm_free_frame(pml4);             /* PML4 */
}
