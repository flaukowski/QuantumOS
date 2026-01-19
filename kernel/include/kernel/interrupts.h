#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <kernel/types.h>

// Interrupt result codes
typedef enum {
    IRQ_SUCCESS = 0,
    IRQ_ERROR_INVALID_VECTOR = -1,
    IRQ_ERROR_ALREADY_REGISTERED = -2,
    IRQ_ERROR_PERMISSION_DENIED = -3,
    IRQ_ERROR_OUT_OF_HANDLERS = -4
} irq_result_t;

// x86_64 interrupt vectors
#define IRQ_BASE          32
#define IRQ_MAX           255
#define EXCEPTION_BASE    0
#define EXCEPTION_MAX     31

// Exception vectors
#define EXC_DIVIDE_ERROR          0
#define EXC_DEBUG                 1
#define EXC_NMI                   2
#define EXC_BREAKPOINT            3
#define EXC_OVERFLOW              4
#define EXC_BOUND_RANGE          5
#define EXC_INVALID_OPCODE        6
#define EXC_DEVICE_NOT_AVAILABLE  7
#define EXC_DOUBLE_FAULT          8
#define EXC_INVALID_TSS           10
#define EXC_SEGMENT_NOT_PRESENT   11
#define EXC_STACK_SEGMENT_FAULT   12
#define EXC_GENERAL_PROTECTION    13
#define EXC_PAGE_FAULT            14
#define EXC_X87_FPU_ERROR         16
#define EXC_ALIGNMENT_CHECK       17
#define EXC_MACHINE_CHECK         18
#define EXC_SIMD_FP_EXCEPTION     19
#define EXC_VIRTUALIZATION        20
#define EXC_SECURITY             30
#define EXC_RESERVED              31

// IRQ vectors (hardware interrupts)
#define IRQ_TIMER                0
#define IRQ_KEYBOARD             1
#define IRQ_CASCADE              2
#define IRQ_COM2                 3
#define IRQ_COM1                 4
#define IRQ_LPT2                 5
#define IRQ_FLOPPY               6
#define IRQ_LPT1                 7
#define IRQ_CMOS_RTC             8
#define IRQ_FREE1                9
#define IRQ_FREE2                10
#define IRQ_FREE3                11
#define IRQ_MOUSE                12
#define IRQ_MATH_COPROCESSOR     13
#define IRQ_PRIMARY_ATA          14
#define IRQ_SECONDARY_ATA        15

// Interrupt gate descriptor
typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;      // Interrupt stack table index
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

// IDT pointer
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr_t;

// CPU state saved on interrupt
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, eflags, rsp, ss;
} __attribute__((packed)) cpu_state_t;

// Interrupt handler function type
typedef void (*interrupt_handler_t)(cpu_state_t *state);

// Interrupt handler registration
typedef struct {
    interrupt_handler_t handler;
    void *context;
    uint32_t flags;
} interrupt_handler_info_t;

// Interrupt management functions
irq_result_t interrupts_init(void);
irq_result_t interrupt_register(uint8_t vector, interrupt_handler_t handler, void *context);
irq_result_t interrupt_unregister(uint8_t vector);
irq_result_t interrupt_enable(uint8_t vector);
irq_result_t interrupt_disable(uint8_t vector);
irq_result_t interrupt_enable_all(void);
irq_result_t interrupt_disable_all(void);

// Exception handling
void exception_handler(cpu_state_t *state);
void divide_error_handler(cpu_state_t *state);
void page_fault_handler(cpu_state_t *state);
void general_protection_fault_handler(cpu_state_t *state);
void double_fault_handler(cpu_state_t *state);

// IRQ handling
void irq_handler(cpu_state_t *state);
void timer_irq_handler(cpu_state_t *state);
void keyboard_irq_handler(cpu_state_t *state);

// Low-level interrupt handling
void idt_set_gate(uint8_t vector, uint64_t handler_addr, uint16_t selector, uint8_t type_attr);
void idt_install(void);
void load_idt(idt_ptr_t *idtp);

// Interrupt controller (PIC)
void pic_init(void);
void pic_send_eoi(uint8_t irq);
void pic_mask_irq(uint8_t irq);
void pic_unmask_irq(uint8_t irq);

// APIC (if available)
void apic_init(void);
void apic_timer_init(uint32_t frequency);
void apic_send_eoi(void);
void apic_timer_handler(cpu_state_t *state);

// Stack switching for interrupts
void interrupt_stack_init(void);
void set_ist_entry(uint8_t vector, uint8_t ist_index);

// Debugging
void dump_cpu_state(cpu_state_t *state);
void dump_idt(void);
void interrupt_stats(void);

// Error codes for exceptions
#define PF_PRESENT     0x01
#define PF_WRITE       0x02
#define PF_USER        0x04
#define PF_RESERVED    0x08
#define PF_INSTRUCTION 0x10

// Gate types
#define GATE_TYPE_INTERRUPT    0x0E
#define GATE_TYPE_TRAP         0x0F
#define GATE_TYPE_TASK         0x05

// Gate privileges
#define DPL_KERNEL    0x00
#define DPL_USER      0x03

// IST indices
#define IST_NONE      0
#define IST_DOUBLE_FAULT 1
#define IST_NMI       2
#define IST_MAX       7

// Constants
#define IDT_ENTRIES 256
#define IDT_SIZE (IDT_ENTRIES * sizeof(idt_entry_t))

#endif // INTERRUPTS_H
