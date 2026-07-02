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
#include <kernel/elf.h>
#include <kernel/capability.h>
#include <kernel/ipc.h>
#include <kernel/service.h>
#include <kernel/boot.h>

/* Embedded user programs (kernel/src/user_blob.S) */
extern const uint8_t user_hello_start[], user_hello_end[];
extern const uint8_t user_canary_start[], user_canary_end[];
extern const uint8_t user_rogue_start[], user_rogue_end[];

/* Embedded ELF user programs (build/x86_64/user ELFs via objcopy) */
extern const uint8_t _binary_init_elf_start[], _binary_init_elf_end[];
extern const uint8_t _binary_echo_elf_start[], _binary_echo_elf_end[];
extern const uint8_t _binary_client_elf_start[], _binary_client_elf_end[];

static status_t finalize_user_process(address_space_t *as, const char *name,
                                      uint64_t entry, uint32_t *pid_out);
void user_ipc_demo_init(void);

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

/* Capability-routed send: the caller may only transmit to whatever
 * destination it holds an IPC send-capability for (capability-as-
 * address). No such capability -> EPERM. */
static uint64_t sys_send(uint32_t pid, uint64_t user_ptr, uint64_t len) {
    if (!in_user_range(user_ptr)) {
        return SYSCALL_EFAULT;
    }
    if (len == 0 || len > IPC_MAX_MESSAGE_SIZE) {
        return SYSCALL_EINVAL;
    }

    uint32_t dest = 0;
    if (cap_find(pid, CAP_RESOURCE_IPC, CAP_WRITE, &dest) != CAP_SUCCESS) {
        return SYSCALL_EPERM;
    }

    ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    const uint8_t *src = (const uint8_t *)user_ptr;
    uint32_t n = 0;
    while (n < len && n < IPC_MAX_MESSAGE_SIZE && in_user_range(user_ptr + n)) {
        msg.data[n] = src[n];
        n++;
    }
    msg.length = n;

    if (ipc_send(dest, &msg, 0) != IPC_SUCCESS) {
        return SYSCALL_EIO;
    }
    return 0;
}

/* Receive from the caller's own queue (no capability needed to drain
 * your own mailbox). Returns the sender pid, or 0 if empty. */
static uint64_t sys_recv(uint32_t pid, uint64_t user_ptr, uint64_t len) {
    (void)pid;
    if (!in_user_range(user_ptr)) {
        return SYSCALL_EFAULT;
    }

    ipc_message_t msg;
    uint32_t sender = IPC_PID_ANY;
    if (ipc_receive(&sender, &msg, 0) != IPC_SUCCESS) {
        return 0; /* nothing queued */
    }

    uint8_t *dst = (uint8_t *)user_ptr;
    uint32_t n = 0;
    while (n < len && n < msg.length && in_user_range(user_ptr + n)) {
        dst[n] = msg.data[n];
        n++;
    }
    /* NUL-terminate if room (messages in the demo are strings) */
    if (n < len && in_user_range(user_ptr + n)) {
        dst[n] = 0;
    }
    return msg.sender_id;
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
    case SYS_SEND:
        state->rax = sys_send(pid, state->rdi, state->rsi);
        break;
    case SYS_RECV:
        state->rax = sys_recv(pid, state->rdi, state->rsi);
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

    /* Stack + process creation shared with the ELF path */
    return finalize_user_process(&as, name, USER_VBASE, pid_out);
}

/* Map the user stack and create the ring-3 process bound to `as`, with
 * the given entry point. Shared by the flat-blob and ELF spawn paths. */
static status_t finalize_user_process(address_space_t *as, const char *name,
                                      uint64_t entry, uint32_t *pid_out) {
    /* User stack, mapped just below USER_STACK_TOP */
    for (uint32_t p = 1; p <= USER_STACK_PAGES; p++) {
        uint64_t uvaddr = USER_STACK_TOP - (uint64_t)p * PAGE_SIZE;
        if (!map_fresh_page(as, uvaddr, true)) {
            return STATUS_NO_MEMORY;
        }
    }

    process_create_params_t params = {
        .name = name,
        .type = PROCESS_TYPE_USER,
        .priority = PRIORITY_NORMAL,
        .parent_pid = KERNEL_PROCESS_ID,
        .entry_point = (void *)entry,
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
    proc->cr3 = as->cr3;
    proc->virtual_address_space = as->pml4;

    if (pid_out) {
        *pid_out = proc->pid;
    }
    return STATUS_SUCCESS;
}

/* Spawn a user process from an embedded ELF64 image: the loader maps
 * and populates the program's PT_LOAD segments into a private address
 * space, then the process starts at the ELF entry point. */
status_t user_process_spawn_elf(const char *name, const void *elf_start,
                                const void *elf_end, uint32_t *pid_out) {
    size_t elf_size = (size_t)((const uint8_t *)elf_end -
                               (const uint8_t *)elf_start);

    address_space_t as = vmspace_create();
    if (!as.pml4) {
        return STATUS_NO_MEMORY;
    }

    uint64_t entry = 0;
    if (elf_load(&as, (const uint8_t *)elf_start, elf_size, &entry) != ELF_OK) {
        return STATUS_ERROR;
    }

    return finalize_user_process(&as, name, entry, pid_out);
}

void user_init(void) {
    boot_log("Per-process address spaces enabled (private CR3 per user proc)");

    uint32_t pid = 0;

    /* A real compiled-C program, loaded from its embedded ELF image */
    if (user_process_spawn_elf("init", _binary_init_elf_start,
                               _binary_init_elf_end, &pid) != STATUS_SUCCESS) {
        boot_log("Warning: failed to load init ELF");
    }

    /* Hand-written blobs for the isolation + fault-containment demos */
    if (user_process_spawn("user-canary-1", user_canary_start, user_canary_end,
                           &pid) != STATUS_SUCCESS ||
        user_process_spawn("user-canary-2", user_canary_start, user_canary_end,
                           &pid) != STATUS_SUCCESS ||
        user_process_spawn("user-rogue", user_rogue_start, user_rogue_end,
                           &pid) != STATUS_SUCCESS) {
        boot_log("Warning: failed to spawn user processes");
        return;
    }
    boot_log("User processes spawned (init ELF, 2x canary, 1x rogue)");

    user_ipc_demo_init();
}

/* Bring up a user-space service (echo) via the service framework and a
 * client that talks to it over capability-checked IPC. Demonstrates the
 * microkernel model: a system service running as an isolated ring-3
 * process, reachable only by a process holding the right capability. */
void user_ipc_demo_init(void) {
    service_definition_t echo_def = {
        .name = "echo",
        .entry = NULL,                        /* user-process service */
        .user_elf_start = _binary_echo_elf_start,
        .user_elf_end = _binary_echo_elf_end,
        .dependencies = { NULL },
        .max_restarts = 1,
    };

    uint32_t sid = 0;
    uint32_t echo_pid = 0, client_pid = 0;

    if (service_register(&echo_def, &sid) == SVC_SUCCESS &&
        service_start("echo", NULL) == SVC_SUCCESS) {
        service_info_t info;
        if (service_status(sid, &info) == SVC_SUCCESS) {
            echo_pid = info.pid;
        }
    }
    if (echo_pid == 0) {
        boot_log("Warning: echo user-service failed to start");
        return;
    }

    if (user_process_spawn_elf("ipc-client", _binary_client_elf_start,
                               _binary_client_elf_end, &client_pid) != STATUS_SUCCESS) {
        boot_log("Warning: ipc-client failed to spawn");
        return;
    }

    /* Capability-as-address: grant each side a single IPC send-cap to
     * the other. These are the only IPC capabilities either holds, so
     * they can talk to each other and nothing else. */
    uint32_t cap = CAP_ID_INVALID;
    cap_create(client_pid, CAP_RESOURCE_IPC, echo_pid, CAP_READ | CAP_WRITE, 0, &cap);
    cap_create(echo_pid, CAP_RESOURCE_IPC, client_pid, CAP_READ | CAP_WRITE, 0, &cap);

    boot_log("IPC demo: echo service (ring 3) + client wired via capabilities");
}
