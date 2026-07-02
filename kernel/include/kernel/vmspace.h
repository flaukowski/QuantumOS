/**
 * QuantumOS Per-Process Address Spaces
 *
 * Each user process gets its own PML4. The kernel half (identity map
 * of the low 1 GB, supervisor-only 2 MB pages) is *shared* by pointing
 * every process's PDPT[0] at the boot page directory, so kernel code,
 * stacks, and data stay mapped and valid across a CR3 switch — the
 * kernel can run in any process's address space. The user half
 * (1–2 GB, PDPT[1]) is private: the same user virtual address is
 * backed by different physical frames in every process, so one
 * process literally cannot name another's memory.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VMSPACE_H
#define VMSPACE_H

#include <kernel/types.h>

/* User virtual layout (per process, in the 1–2 GB PDPT slot) */
#define USER_VBASE        0x40000000UL   /* code copied here; entry point */
#define USER_DATA_VADDR   0x40080000UL   /* zeroed writable page (isolation canary) */
#define USER_STACK_TOP    0x40100000UL   /* stack grows down from here */
#define USER_STACK_PAGES  4              /* 16 KB user stack */
#define USER_CODE_PAGES   8              /* 32 KB code window */

typedef struct {
    uint64_t *pml4;   /* kernel-VA (identity-mapped) of the PML4 */
    uint64_t cr3;     /* physical address to load into CR3 */
} address_space_t;

/* Create a fresh address space: private PML4 + PDPT, kernel half
 * shared. Returns pml4 == NULL on allocation failure. */
address_space_t vmspace_create(void);

/* Map one 4 KB user page (present|user, writable optional) at uvaddr
 * to physical frame paddr, allocating intermediate tables as needed.
 * Returns false on allocation failure. */
bool vmspace_map_page(address_space_t *as, uint64_t uvaddr,
                      uint64_t paddr, bool writable);

/* Load an address space into CR3 (no-op if already active) */
void vmspace_switch(uint64_t cr3);

/* Free every frame in a process's private user half — its page tables
 * and the pages they map — back to the pmm. The shared kernel half
 * (PDPT[0] -> boot page directory) is left untouched. Must NOT be
 * called on the address space currently loaded in CR3. */
void vmspace_destroy(uint64_t *pml4);

/* CR3 for the plain kernel address space (boot page tables) */
uint64_t vmspace_kernel_cr3(void);

#endif /* VMSPACE_H */
