/**
 * QuantumOS GDT and TSS
 *
 * Replaces the minimal boot GDT (null + kernel code/data) with a full
 * table including ring-3 code/data descriptors and a TSS, so the CPU
 * can take interrupts from user mode (RSP0 stack switch).
 *
 * Selector layout (kernel selectors unchanged from boot.S so no
 * segment reloading gymnastics are needed):
 *   0x08 kernel code   0x10 kernel data
 *   0x18 user data     0x20 user code     0x28 TSS
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef GDT_H
#define GDT_H

#include <kernel/types.h>

#define GDT_KERNEL_CS  0x08
#define GDT_KERNEL_DS  0x10
#define GDT_USER_DS    0x18
#define GDT_USER_CS    0x20
#define GDT_TSS_SEL    0x28

/* Ring-3 selectors as loaded into segment registers (RPL = 3) */
#define USER_CS        (GDT_USER_CS | 3)   /* 0x23 */
#define USER_DS        (GDT_USER_DS | 3)   /* 0x1B */

/* Install the full GDT + TSS and load TR. Call once during kernel
 * init, before any ring-3 code can run. */
void gdt_init(void);

/* Set the kernel stack the CPU switches to on ring3 -> ring0 entry */
void tss_set_rsp0(uint64_t rsp0);

#endif /* GDT_H */
