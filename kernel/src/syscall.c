/**
 * QuantumOS System Call Implementation
 *
 * int 0x80 gate (DPL 3) dispatched through the common interrupt path;
 * the handler reads arguments from — and writes results into — the
 * saved cpu_state_t frame. Yield and exit reuse the scheduler's
 * frame-swap machinery, so returning from the syscall may resume a
 * different process.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/syscall.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/gdt.h>
#include <kernel/vmspace.h>
#include <kernel/memory.h>
#include <kernel/boot.h>

/* Embedded user programs (kernel/src/user_blob.S) */
extern const uint8_t user_hello_start[], user_hello_end[];
extern const uint8_t user_canary_start[], user_canary_end[];
extern const uint8_t user_rogue_start[], user_rogue_end[];

/* int 0x80 stub (kernel/src/interrupts.S) */
extern void isr128(void);

/* ============================================================================
 * Helpers
 * ============================================================================ */

/* Is addr inside the calling process's mapped user half (1–2 GB)? */
static int in_user_range(uint64_t addr) {
    return addr >= USER_VBASE && addr < 0x80000000UL;
}

/* Print "[user pid=N] msg" atomically enough for the boot console */
static void user_console_write(uint32_t pid, const char *msg) {
    early_console_write("[user pid=");
    early_console_write_hex(pid);
    early_console_write("] ");
    early_console_write(msg);
    early_console_write("\r\n");
}

/* ============================================================================
 * Syscall implementations
 * ============================================================================ */

static uint64_t sys_write(uint32_t pid, uint64_t user_ptr) {
    if (!in_user_range(user_ptr)) {
        return SYSCALL_EFAULT;
    }

    /* Copy out of the caller's user memory (current CR3 is the caller's
     * address space, so the pointer resolves to its private frame),
     * bounded by buffer and the user half's end. */
    char buf[128];
    size_t i = 0;
    const char *src = (const char *)user_ptr;
    while (i < sizeof(buf) - 1 && in_user_range(user_ptr + i) && src[i]) {
        buf[i] = src[i];
        i++;
    }
    buf[i] = '\0';

    user_console_write(pid, buf);
    return i;
}

static void sys_exit(uint32_t pid, uint64_t code, cpu_state_t *state) {
    (void)code;
    boot_log("syscall: user process exited");
    process_set_state(pid, PROCESS_STATE_TERMINATED);
    scheduler_kill_current(state);
}

/* ============================================================================
 * Dispatch
 * ============================================================================ */

static void syscall_dispatch(cpu_state_t *state) {
    process_t *cur = process_get_current();
    uint32_t pid = cur ? cur->pid : 0;

    switch (state->rax) {
    case SYS_WRITE:
        state->rax = sys_write(pid, state->rdi);
        break;
    case SYS_GETPID:
        state->rax = pid;
        break;
    case SYS_YIELD:
        state->rax = 0;
        scheduler_reschedule(state);
        break;
    case SYS_EXIT:
        sys_exit(pid, state->rdi, state);
        break;
    case SYS_TICKS:
        state->rax = timer_get_ticks();
        break;
    default:
        state->rax = SYSCALL_ENOSYS;
        break;
    }
}

void syscall_init(void) {
    /* DPL 3 gate: ring 3 may execute int 0x80; everything else in the
     * IDT stays kernel-only */
    idt_set_gate(SYSCALL_VECTOR, (uint64_t)isr128, GDT_KERNEL_CS,
                 GATE_TYPE_INTERRUPT | DPL_USER);
    interrupt_register(SYSCALL_VECTOR, syscall_dispatch, NULL);
    boot_log("Syscall interface installed (int 0x80)");
}

/* ============================================================================
 * User process setup
 * ============================================================================ */

/* Allocate a physical frame and map it at uvaddr in `as`. Returns the
 * frame's kernel-VA (== physical, identity-mapped) for population, or
 * NULL on failure. */
static uint8_t *map_fresh_page(address_space_t *as, uint64_t uvaddr,
                               bool writable) {
    void *frame = pmm_alloc_frame();
    if (!frame) {
        return NULL;
    }
    memset(frame, 0, PAGE_SIZE);
    if (!vmspace_map_page(as, uvaddr, (uint64_t)frame, writable)) {
        return NULL;
    }
    return (uint8_t *)frame;
}

status_t user_process_spawn(const char *name, const void *blob_start,
                            const void *blob_end, uint32_t *pid_out) {
    size_t blob_size = (size_t)((const uint8_t *)blob_end -
                                (const uint8_t *)blob_start);
    if (blob_size == 0 || blob_size > USER_CODE_PAGES * PAGE_SIZE) {
        return STATUS_INVALID_ARG;
    }

    /* Private address space for this process */
    address_space_t as = vmspace_create();
    if (!as.pml4) {
        return STATUS_NO_MEMORY;
    }

    /* Map + populate the code window (executable via absent NX; the
     * blob is copied through the frame's identity VA) */
    const uint8_t *src = (const uint8_t *)blob_start;
    for (uint32_t p = 0; p < USER_CODE_PAGES; p++) {
        uint64_t uvaddr = USER_VBASE + (uint64_t)p * PAGE_SIZE;
        uint8_t *page = map_fresh_page(&as, uvaddr, true);
        if (!page) {
            return STATUS_NO_MEMORY;
        }
        size_t off = (size_t)p * PAGE_SIZE;
        for (size_t b = 0; b < PAGE_SIZE && off + b < blob_size; b++) {
            page[b] = src[off + b];
        }
    }

    /* Zeroed writable data page (isolation canary target) */
    if (!map_fresh_page(&as, USER_DATA_VADDR, true)) {
        return STATUS_NO_MEMORY;
    }

    /* User stack, mapped just below USER_STACK_TOP */
    for (uint32_t p = 1; p <= USER_STACK_PAGES; p++) {
        uint64_t uvaddr = USER_STACK_TOP - (uint64_t)p * PAGE_SIZE;
        if (!map_fresh_page(&as, uvaddr, true)) {
            return STATUS_NO_MEMORY;
        }
    }

    process_create_params_t params = {
        .name = name,
        .type = PROCESS_TYPE_USER,
        .priority = PRIORITY_NORMAL,
        .parent_pid = KERNEL_PROCESS_ID,
        .entry_point = (void *)USER_VBASE,
        .stack_address = (void *)(USER_STACK_TOP - USER_REGION_SIZE),
        .stack_size = USER_REGION_SIZE,   /* so stack_top == USER_STACK_TOP */
        .is_quantum_aware = false
    };

    process_t *proc = NULL;
    status_t result = process_create(&params, &proc);
    if (result != STATUS_SUCCESS) {
        return result;
    }

    /* Bind the process to its private address space */
    proc->cr3 = as.cr3;
    proc->virtual_address_space = as.pml4;

    if (pid_out) {
        *pid_out = proc->pid;
    }
    return STATUS_SUCCESS;
}

void user_init(void) {
    boot_log("Per-process address spaces enabled (private CR3 per user proc)");

    uint32_t pid = 0;
    if (user_process_spawn("user-canary-1", user_canary_start, user_canary_end,
                           &pid) != STATUS_SUCCESS ||
        user_process_spawn("user-canary-2", user_canary_start, user_canary_end,
                           &pid) != STATUS_SUCCESS ||
        user_process_spawn("user-hello", user_hello_start, user_hello_end,
                           &pid) != STATUS_SUCCESS ||
        user_process_spawn("user-rogue", user_rogue_start, user_rogue_end,
                           &pid) != STATUS_SUCCESS) {
        boot_log("Warning: failed to spawn user processes");
        return;
    }
    boot_log("User processes spawned (2x canary, 1x hello, 1x rogue)");
}
