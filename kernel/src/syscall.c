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
#include <kernel/boot.h>

/* Embedded user programs (kernel/src/user_blob.S) */
extern const uint8_t user_hello_start[], user_hello_end[];
extern const uint8_t user_rogue_start[], user_rogue_end[];

/* Boot page tables (kernel/src/boot.S) */
extern uint64_t boot_pml4[], boot_pdpt[], boot_pd[];

/* int 0x80 stub (kernel/src/interrupts.S) */
extern void isr128(void);

/* CR3 helpers (kernel/src/interrupts.S) */
extern uint64_t get_cr3(void);
extern void set_cr3(uint64_t cr3);

static uint32_t user_regions_used = 0;

/* ============================================================================
 * Helpers
 * ============================================================================ */

static int in_user_window(uint64_t addr) {
    return addr >= USER_REGION_BASE &&
           addr < USER_REGION_BASE + (uint64_t)USER_MAX_PROCESSES * USER_REGION_SIZE;
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
    if (!in_user_window(user_ptr)) {
        return SYSCALL_EFAULT;
    }

    /* Copy out of user memory, bounded by buffer and window end */
    char buf[128];
    uint64_t window_end = USER_REGION_BASE +
        (uint64_t)USER_MAX_PROCESSES * USER_REGION_SIZE;
    size_t i = 0;
    const char *src = (const char *)user_ptr;
    while (i < sizeof(buf) - 1 && user_ptr + i < window_end && src[i]) {
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

status_t user_process_spawn(const char *name, const void *blob_start,
                            const void *blob_end, uint32_t *pid_out) {
    if (user_regions_used >= USER_MAX_PROCESSES) {
        return STATUS_NO_MEMORY;
    }

    size_t blob_size = (size_t)((const uint8_t *)blob_end -
                                (const uint8_t *)blob_start);
    if (blob_size == 0 || blob_size > USER_REGION_SIZE / 2) {
        return STATUS_INVALID_ARG;
    }

    uint8_t *region = (uint8_t *)(USER_REGION_BASE +
        (uint64_t)user_regions_used * USER_REGION_SIZE);
    user_regions_used++;

    memcpy(region, blob_start, blob_size);

    process_create_params_t params = {
        .name = name,
        .type = PROCESS_TYPE_USER,
        .priority = PRIORITY_NORMAL,
        .parent_pid = KERNEL_PROCESS_ID,
        .entry_point = region,
        .stack_address = region,           /* stack_top = region end */
        .stack_size = USER_REGION_SIZE,
        .is_quantum_aware = false
    };

    process_t *proc = NULL;
    status_t result = process_create(&params, &proc);
    if (result != STATUS_SUCCESS) {
        return result;
    }
    if (pid_out) {
        *pid_out = proc->pid;
    }
    return STATUS_SUCCESS;
}

void user_init(void) {
    /* Open the user window: the user bit must be set at every paging
     * level. PML4[0]/PDPT[0] become user-reachable, but individual
     * kernel 2 MB pages remain supervisor-only — only the PD entries
     * covering the user window get the user bit. */
    boot_pml4[0] |= 0x4;
    boot_pdpt[0] |= 0x4;
    for (uint32_t i = 0; i < USER_MAX_PROCESSES; i++) {
        uint64_t addr = USER_REGION_BASE + (uint64_t)i * USER_REGION_SIZE;
        boot_pd[addr >> 21] |= 0x4;
    }
    set_cr3(get_cr3()); /* flush TLB */
    boot_log("User memory window mapped (ring-3 pages above heap)");

    uint32_t pid = 0;
    if (user_process_spawn("user-hello-1", user_hello_start, user_hello_end,
                           &pid) != STATUS_SUCCESS ||
        user_process_spawn("user-hello-2", user_hello_start, user_hello_end,
                           &pid) != STATUS_SUCCESS ||
        user_process_spawn("user-rogue", user_rogue_start, user_rogue_end,
                           &pid) != STATUS_SUCCESS) {
        boot_log("Warning: failed to spawn user processes");
        return;
    }
    boot_log("User processes spawned (2x hello, 1x rogue)");
}
