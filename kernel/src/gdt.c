/**
 * QuantumOS GDT and TSS Implementation
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/gdt.h>
#include <kernel/boot.h>

/* 64-bit TSS (Intel SDM Vol.3 8.7) */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

/* GDT layout: null, kcode, kdata, udata, ucode, TSS (2 slots) */
static uint64_t gdt[7] __attribute__((aligned(8)));
static tss_t tss;

/* Kernel stack used when the CPU enters ring 0 from ring 3. A single
 * stack suffices on one CPU: the interrupt frame is consumed (copied
 * into the PCB or iretq'd) before the next ring-3 entry can occur. */
static uint8_t interrupt_stack[16384] __attribute__((aligned(16)));

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

static gdt_ptr_t gdt_ptr;

/* From interrupts.S */
extern void load_gdt(gdt_ptr_t *ptr);
extern void load_tss(uint16_t selector);

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}

void gdt_init(void) {
    /* Descriptors: same kernel entries as the boot GDT (so CS/DS stay
     * valid without reloading), plus DPL-3 user entries. In long mode
     * base/limit are ignored for code/data; the access bits matter. */
    gdt[0] = 0;                        /* null */
    gdt[1] = 0x00209A0000000000ULL;    /* 0x08 kernel code: P S X L */
    gdt[2] = 0x0000920000000000ULL;    /* 0x10 kernel data: P S RW */
    gdt[3] = 0x0000F20000000000ULL;    /* 0x18 user data:  P DPL3 S RW */
    gdt[4] = 0x0020FA0000000000ULL;    /* 0x20 user code:  P DPL3 S X L */

    /* TSS descriptor (16 bytes across two GDT slots) */
    uint64_t base = (uint64_t)&tss;
    uint32_t limit = sizeof(tss_t) - 1;
    gdt[5] = ((uint64_t)(limit & 0xFFFF)) |
             ((base & 0xFFFFFFULL) << 16) |
             (0x89ULL << 40) |                     /* type: available 64-bit TSS, P */
             (((uint64_t)(limit >> 16) & 0xF) << 48) |
             (((base >> 24) & 0xFFULL) << 56);
    gdt[6] = base >> 32;

    memset(&tss, 0, sizeof(tss));
    tss.rsp0 = (uint64_t)(interrupt_stack + sizeof(interrupt_stack));
    tss.iomap_base = sizeof(tss_t);   /* no I/O bitmap */

    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint64_t)gdt;
    load_gdt(&gdt_ptr);
    load_tss(GDT_TSS_SEL);

    boot_log("GDT + TSS installed (user-mode descriptors ready)");
}
