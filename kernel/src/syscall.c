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
#include <kernel/quantum.h>
#include <kernel/ipc.h>
#include <kernel/service.h>
#include <kernel/com2_uart.h>
#include <kernel/console.h>
#include <kernel/initrd.h>
#include <kernel/ramfs.h>
#include <kernel/ata.h>
#include <kernel/net.h>
#include <kernel/rtc.h>
#include <kernel/boot.h>

/* Embedded user programs (kernel/src/user_blob.S) */
extern const uint8_t user_hello_start[], user_hello_end[];
extern const uint8_t user_canary_start[], user_canary_end[];
extern const uint8_t user_rogue_start[], user_rogue_end[];

/* Embedded ELF user programs (build/x86_64/user ELFs via objcopy) */
extern const uint8_t _binary_init_elf_start[], _binary_init_elf_end[];
extern const uint8_t _binary_echo_elf_start[], _binary_echo_elf_end[];
extern const uint8_t _binary_client_elf_start[], _binary_client_elf_end[];
extern const uint8_t _binary_hbsvc_elf_start[], _binary_hbsvc_elf_end[];
extern const uint8_t _binary_ghostd_elf_start[], _binary_ghostd_elf_end[];
extern const uint8_t _binary_ghost_test_elf_start[], _binary_ghost_test_elf_end[];
extern const uint8_t _binary_paradoxd_elf_start[], _binary_paradoxd_elf_end[];
extern const uint8_t _binary_paradox_test_elf_start[], _binary_paradox_test_elf_end[];
extern const uint8_t _binary_swarm_svc_elf_start[], _binary_swarm_svc_elf_end[];
extern const uint8_t _binary_qsh_elf_start[], _binary_qsh_elf_end[];
extern const uint8_t _binary_quantumd_elf_start[], _binary_quantumd_elf_end[];

/* Argument vector ABI (epic #62). MUST stay byte-identical to user_args_t
 * in user/usys.h — there is no shared header across the ring boundary. The
 * kernel fills a copy of this and maps it read-only at USER_ARGS_VADDR into
 * the new process; get_args() on the user side reads it. */
#define KARGS_MAX 8
#define KARGS_STRBYTES 480
typedef struct {
    int argc;
    unsigned argv_off[KARGS_MAX];
    char strings[KARGS_STRBYTES];
} kuser_args_t;

static status_t finalize_user_process(address_space_t *as, const char *name, uint64_t entry,
                                      const kuser_args_t *kargs, uint32_t *pid_out);
static status_t spawn_elf_args(const char *name, const void *elf_start, const void *elf_end,
                               const kuser_args_t *kargs, uint32_t *pid_out);
void user_ipc_demo_init(void);
void user_ghost_demo_init(void);
void user_paradox_demo_init(uint32_t ghostd_pid);
void user_swarm_demo_init(uint32_t ghostd_pid);
void user_shell_init(uint32_t ghostd_pid);
void user_quantum_demo_init(void);

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

/* Exit ledger (epic #62 phase 3): the idle-loop reaper memsets a
 * terminated PCB long before a shell polls for it, so exit codes are
 * also recorded in this small ring. SYS_WAITPID consults the ledger
 * first (newest wins, so a recycled pid still resolves to the exit the
 * waiter is after) and the live table second. Single CPU, syscalls run
 * cli'd — no locking needed. */
#define EXIT_LEDGER_SIZE 16
static struct {
    uint32_t pid;
    int32_t code;
    bool valid;
} exit_ledger[EXIT_LEDGER_SIZE];
static uint32_t exit_ledger_next;

static void exit_ledger_record(uint32_t pid, int32_t code) {
    exit_ledger[exit_ledger_next].pid = pid;
    exit_ledger[exit_ledger_next].code = code;
    exit_ledger[exit_ledger_next].valid = true;
    exit_ledger_next = (exit_ledger_next + 1) % EXIT_LEDGER_SIZE;
}

static void sys_exit(uint32_t pid, uint64_t code, cpu_state_t *state) {
    process_t *cur = process_get_by_pid(pid);
    if (cur) {
        cur->exit_code = (int32_t)code;
        cur->has_exited = true;
    }
    exit_ledger_record(pid, (int32_t)code);
    boot_log_v("syscall: user process exited");
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

/* Targeted capability-routed send: like sys_send, but the destination is
 * named explicitly (dest pid) instead of taken first-match. The caller must
 * hold an IPC send-capability whose resource_id is exactly `dest` — the same
 * capability-as-address rule, just choosing which held address to use. This
 * lets a service reply to whichever client sent it a request (the sender pid
 * recv returns) while still only being able to reach peers it was granted. */
static uint64_t sys_send_to(uint32_t pid, uint64_t dest, uint64_t user_ptr, uint64_t len) {
    if (!in_user_range(user_ptr)) {
        return SYSCALL_EFAULT;
    }
    if (len == 0 || len > IPC_MAX_MESSAGE_SIZE) {
        return SYSCALL_EINVAL;
    }

    if (cap_find_resource(pid, CAP_RESOURCE_IPC, CAP_WRITE, (uint32_t)dest) != CAP_SUCCESS) {
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

    if (ipc_send((uint32_t)dest, &msg, 0) != IPC_SUCCESS) {
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

/* Capability-gated quantum randomness. Draws up to QRAND_MAX_BYTES bytes
 * from the kernel's qseed-mixed generator into the caller's buffer; the
 * caller must hold a CAP_RESOURCE_QUANTUM read capability or the draw is
 * denied (EPERM) before any byte is produced. len == 0 is a provenance
 * query returning whether a boot qseed was mixed in (still cap-gated). */
static uint64_t sys_qrand(uint32_t pid, uint64_t user_ptr, uint64_t len) {
    uint8_t seed_present = 0;

    if (len == 0) {
        /* Provenance query: no buffer touched. Still requires the cap so a
         * capless caller cannot even learn the entropy provenance. */
        if (quantum_user_random(pid, NULL, 0, &seed_present) != QUANTUM_SUCCESS) {
            return SYSCALL_EPERM;
        }
        return seed_present ? 1 : 0;
    }

    if (!in_user_range(user_ptr)) {
        return SYSCALL_EFAULT;
    }
    if (len > QRAND_MAX_BYTES) {
        len = QRAND_MAX_BYTES;
    }

    /* Draw into a kernel buffer under the capability check, then copy out
     * only as far as the caller's mapped user half extends. */
    uint8_t tmp[QRAND_MAX_BYTES];
    if (quantum_user_random(pid, tmp, (uint32_t)len, &seed_present) != QUANTUM_SUCCESS) {
        return SYSCALL_EPERM;
    }

    uint8_t *dst = (uint8_t *)user_ptr;
    uint32_t n = 0;
    while (n < len && in_user_range(user_ptr + n)) {
        dst[n] = tmp[n];
        n++;
    }
    return n;
}

/* Capability-gated COM2 serial pipe (backs SYS_COM2). The COM2 swarm-bridge
 * UART is a guarded device resource: the caller must hold a CAP_RESOURCE_DEVICE
 * capability over DEVICE_ID_COM2 carrying CAP_WRITE (to transmit) or CAP_READ
 * (to receive). Bytes are bounced through a bounded kernel buffer so the driver
 * never touches user pointers directly, and the copy is clamped to the caller's
 * mapped user half exactly like the other syscalls. Read is non-blocking: it
 * returns however many bytes the UART had waiting (0 if none). */
static uint64_t sys_com2(uint32_t pid, uint64_t op, uint64_t user_ptr, uint64_t len) {
    if (op != SYS_COM2_READ && op != SYS_COM2_WRITE) {
        return SYSCALL_EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (!in_user_range(user_ptr)) {
        return SYSCALL_EFAULT;
    }
    if (len > COM2_MAX_BYTES) {
        len = COM2_MAX_BYTES;
    }

    uint8_t tmp[COM2_MAX_BYTES];

    if (op == SYS_COM2_WRITE) {
        if (cap_find_resource(pid, CAP_RESOURCE_DEVICE, CAP_WRITE, DEVICE_ID_COM2) != CAP_SUCCESS) {
            return SYSCALL_EPERM;
        }
        const uint8_t *src = (const uint8_t *)user_ptr;
        uint32_t n = 0;
        while (n < len && in_user_range(user_ptr + n)) {
            tmp[n] = src[n];
            n++;
        }
        return com2_write(tmp, n);
    }

    /* SYS_COM2_READ */
    if (cap_find_resource(pid, CAP_RESOURCE_DEVICE, CAP_READ, DEVICE_ID_COM2) != CAP_SUCCESS) {
        return SYSCALL_EPERM;
    }
    uint32_t got = com2_read(tmp, (uint32_t)len);
    uint8_t *dst = (uint8_t *)user_ptr;
    uint32_t n = 0;
    while (n < got && in_user_range(user_ptr + n)) {
        dst[n] = tmp[n];
        n++;
    }
    return n;
}

/* Capability-gated interactive console (backs SYS_CONS, epic #62 phase 1).
 * The console is a guarded device resource exactly like the COM2 bridge: the
 * caller must hold a CAP_RESOURCE_DEVICE capability over DEVICE_ID_CONSOLE
 * carrying CAP_READ (to drain the input ring) or CAP_WRITE (to emit raw
 * bytes). Reads are non-blocking; writes are raw — no "[user pid]" prefix —
 * so the shell owns its own line discipline. Bytes bounce through a bounded
 * kernel buffer, clamped to the caller's mapped user half as usual. */
static uint64_t sys_cons(uint32_t pid, uint64_t op, uint64_t user_ptr, uint64_t len) {
    if (op != SYS_CONS_READ && op != SYS_CONS_WRITE) {
        return SYSCALL_EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (!in_user_range(user_ptr)) {
        return SYSCALL_EFAULT;
    }
    if (len > CONS_MAX_BYTES) {
        len = CONS_MAX_BYTES;
    }

    uint8_t tmp[CONS_MAX_BYTES];

    if (op == SYS_CONS_WRITE) {
        if (cap_find_resource(pid, CAP_RESOURCE_DEVICE, CAP_WRITE, DEVICE_ID_CONSOLE) !=
            CAP_SUCCESS) {
            return SYSCALL_EPERM;
        }
        const uint8_t *src = (const uint8_t *)user_ptr;
        uint32_t n = 0;
        while (n < len && in_user_range(user_ptr + n)) {
            tmp[n] = src[n];
            n++;
        }
        return console_write(tmp, n);
    }

    /* SYS_CONS_READ */
    if (cap_find_resource(pid, CAP_RESOURCE_DEVICE, CAP_READ, DEVICE_ID_CONSOLE) != CAP_SUCCESS) {
        return SYSCALL_EPERM;
    }
    uint32_t got = console_read(tmp, (uint32_t)len);
    uint8_t *dst = (uint8_t *)user_ptr;
    uint32_t n = 0;
    while (n < got && in_user_range(user_ptr + n)) {
        dst[n] = tmp[n];
        n++;
    }
    return n;
}

/* Append a decimal u64 to buf at offset o (bounded by max). */
static size_t fmt_dec(char *buf, size_t o, size_t max, uint64_t v) {
    char t[20];
    int n = 0;
    if (v == 0) {
        t[n++] = '0';
    }
    while (v && n < (int)sizeof(t)) {
        t[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && o + 1 < max) {
        buf[o++] = t[--n];
    }
    return o;
}

static size_t fmt_str(char *buf, size_t o, size_t max, const char *s) {
    while (*s && o + 1 < max) {
        buf[o++] = *s++;
    }
    return o;
}

/* Live introspection text (backs SYS_SYSINFO). Uncapped read-only reporting,
 * like SYS_GETPID/SYS_TICKS: it names no authority. The kernel formats the
 * text so the user side needs no struct ABI — SYSINFO_PS is one "PS: pid
 * name STATE" line per live process (formatted by process.c, which owns the
 * table), SYSINFO_MEM is a single "MEM: ..." stats line. */
static uint64_t sys_sysinfo(uint32_t pid, uint64_t op, uint64_t user_ptr, uint64_t len) {
    (void)pid;
    /* SYSINFO_QUIET is a bare boolean query (no buffer): returns 1 if the
     * kernel booted `quiet`, so a chatty service can silence its steady-state
     * console logging. Answered before the buffer checks. */
    if (op == SYSINFO_QUIET) {
        return (uint64_t)boot_is_quiet();
    }
    if (len == 0) {
        return 0;
    }
    if (!in_user_range(user_ptr)) {
        return SYSCALL_EFAULT;
    }

    /* Static bounce buffer: syscalls run cli'd on one CPU, so this cannot
     * be re-entered while in use. */
    static char tmp[SYSINFO_MAX_BYTES];
    size_t produced = 0;

    if (op == SYSINFO_PS) {
        produced = process_format_ps(tmp, sizeof(tmp));
    } else if (op == SYSINFO_TIME) {
        produced = rtc_format(tmp, sizeof(tmp));
    } else if (op == SYSINFO_MEM) {
        size_t o = 0;
        o = fmt_str(tmp, o, sizeof(tmp), "MEM: heap free=");
        o = fmt_dec(tmp, o, sizeof(tmp), kheap_free_bytes());
        o = fmt_str(tmp, o, sizeof(tmp), " bytes, frames free=");
        o = fmt_dec(tmp, o, sizeof(tmp), pmm_get_free_frames());
        o = fmt_str(tmp, o, sizeof(tmp), "/");
        o = fmt_dec(tmp, o, sizeof(tmp), pmm_get_total_frames());
        o = fmt_str(tmp, o, sizeof(tmp), "\r\n");
        produced = o;
    } else {
        return SYSCALL_EINVAL;
    }

    if (len > produced) {
        len = produced;
    }
    char *dst = (char *)user_ptr;
    uint32_t n = 0;
    while (n < len && in_user_range(user_ptr + n)) {
        dst[n] = tmp[n];
        n++;
    }
    return n;
}

/* ---- Read-only VFS over the embedded initrd (epic #62 phase 2, #64) ----
 *
 * Four calls: open/read/close/readdir. The backing store is the ustar
 * archive baked into the kernel image — public, read-only data — so the
 * calls are uncapped like SYS_SYSINFO (per-file capabilities are future
 * work). File state lives in the caller's PCB fd table and dies with it. */

/* Copy a NUL-terminated path out of user memory, bounded. Returns 0 on
 * success, -1 if the pointer or termination is bad. */
static int copy_user_path(uint64_t user_ptr, char *dst, size_t max) {
    if (!in_user_range(user_ptr)) {
        return -1;
    }
    const char *src = (const char *)user_ptr;
    size_t i = 0;
    while (i < max - 1 && in_user_range(user_ptr + i) && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return 0;
}

/* Grab a free fd slot in `cur` and fill it. Returns the fd or EIO. */
static uint64_t install_fd(process_t *cur, const uint8_t *data, uint32_t size, bool writable,
                           int16_t ram_idx) {
    for (uint32_t fd = 0; fd < PROCESS_MAX_FDS; fd++) {
        if (!cur->fds[fd].used) {
            cur->fds[fd].data = data;
            cur->fds[fd].size = size;
            cur->fds[fd].offset = 0;
            cur->fds[fd].used = true;
            cur->fds[fd].writable = writable;
            cur->fds[fd].ram_idx = ram_idx;
            return fd;
        }
    }
    return SYSCALL_EIO; /* fd table full */
}

/* Does the caller hold the volume-level filesystem-write capability?
 * (CAP_RESOURCE_DEVICE over DEVICE_ID_DISK — writing files is writing
 * the persistence volume; per-file capabilities remain future work.) */
static int has_fs_write_cap(uint32_t pid) {
    return cap_find_resource(pid, CAP_RESOURCE_DEVICE, CAP_WRITE, DEVICE_ID_DISK) == CAP_SUCCESS;
}

static uint64_t sys_open(uint32_t pid, uint64_t path_ptr, uint64_t flags) {
    process_t *cur = process_get_by_pid(pid);
    if (!cur) {
        return SYSCALL_EINVAL;
    }
    if (flags & ~(uint64_t)(O_WRONLY | O_CREAT | O_TRUNC)) {
        return SYSCALL_EINVAL;
    }

    char path[VFS_PATH_MAX];
    if (copy_user_path(path_ptr, path, sizeof(path)) != 0) {
        return SYSCALL_EFAULT;
    }

    /* ---- Write path (epic #71): create-or-append on the RAM overlay,
     * gated on the filesystem-write capability BEFORE any effect. ---- */
    if (flags & (O_WRONLY | O_CREAT | O_TRUNC)) {
        if (!(flags & O_WRONLY) || !(flags & O_CREAT)) {
            return SYSCALL_EINVAL; /* write combos require O_WRONLY|O_CREAT */
        }
        if (!has_fs_write_cap(pid)) {
            return SYSCALL_EPERM;
        }

        int idx;
        const uint8_t *rdata = NULL;
        uint32_t rsize = 0;
        if (!(flags & O_TRUNC) && ramfs_lookup(path, &rdata, &rsize) >= 0) {
            /* Existing overlay file, appending. */
            idx = ramfs_lookup(path, &rdata, &rsize);
        } else {
            idx = ramfs_create(path); /* creates, or truncates existing */
            if (idx < 0) {
                return SYSCALL_EIO; /* table full / name too long / OOM */
            }
        }

        const uint8_t *d = NULL;
        uint32_t sz = 0;
        ramfs_get(idx, NULL, &d, &sz);
        return install_fd(cur, d, sz, true, (int16_t)idx);
    }

    /* ---- Read path: the overlay shadows the initrd. ---- */
    const uint8_t *data = NULL;
    uint32_t size = 0;
    int ridx = ramfs_lookup(path, &data, &size);
    if (ridx >= 0) {
        return install_fd(cur, data, size, false, (int16_t)ridx);
    }
    if (initrd_lookup(path, &data, &size) != 0) {
        return SYSCALL_ENOENT;
    }
    return install_fd(cur, data, size, false, -1);
}

/* Append through a write-opened fd (epic #71). The write authority was
 * capability-checked at open; the fd carries it. */
static uint64_t sys_fwrite(uint32_t pid, uint64_t fd, uint64_t user_ptr, uint64_t len) {
    process_t *cur = process_get_by_pid(pid);
    if (!cur || fd >= PROCESS_MAX_FDS || !cur->fds[fd].used || !cur->fds[fd].writable ||
        cur->fds[fd].ram_idx < 0) {
        return SYSCALL_EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (!in_user_range(user_ptr)) {
        return SYSCALL_EFAULT;
    }

    /* Bounce through a bounded kernel buffer like every other copy-in. */
    uint8_t tmp[CONS_MAX_BYTES];
    if (len > sizeof(tmp)) {
        len = sizeof(tmp);
    }
    const uint8_t *src = (const uint8_t *)user_ptr;
    uint32_t n = 0;
    while (n < len && in_user_range(user_ptr + n)) {
        tmp[n] = src[n];
        n++;
    }

    int wrote = ramfs_append(cur->fds[fd].ram_idx, tmp, n);
    if (wrote < 0) {
        return SYSCALL_EINVAL;
    }
    return (uint64_t)wrote;
}

/* Remove an overlay file (epic #71). Refused while any live process
 * holds it open — the fd's data snapshot would dangle. */
static uint64_t sys_unlink(uint32_t pid, uint64_t path_ptr) {
    if (!has_fs_write_cap(pid)) {
        return SYSCALL_EPERM;
    }

    char path[VFS_PATH_MAX];
    if (copy_user_path(path_ptr, path, sizeof(path)) != 0) {
        return SYSCALL_EFAULT;
    }

    const uint8_t *data = NULL;
    uint32_t size = 0;
    if (ramfs_lookup(path, &data, &size) < 0) {
        return SYSCALL_ENOENT;
    }
    if (process_any_fd_references(data)) {
        return SYSCALL_EIO; /* still open somewhere — refuse */
    }
    if (ramfs_unlink(path) != 0) {
        return SYSCALL_ENOENT;
    }
    return 0;
}

/* Flush the overlay to disk (epic #71 phase 3). */
static uint64_t sys_sync_fs(uint32_t pid) {
    if (!has_fs_write_cap(pid)) {
        return SYSCALL_EPERM;
    }
    int n = persist_sync();
    if (n < 0) {
        return SYSCALL_EIO;
    }
    return (uint64_t)n;
}

/* Resolve a hostname to an IPv4 address (epic #73 follow-up). Real
 * authority — using the shared NIC — so it is capability-gated on a
 * CAP_RESOURCE_DEVICE over DEVICE_ID_NET, held only by qsh. The lookup
 * itself runs in the IF=1 net thread (a cli'd syscall can't pump the RX
 * IRQ); this call posts the request on the first invocation and polls on
 * the rest, so the caller loops with SYS_YIELD. */
static uint64_t sys_resolve(uint32_t pid, uint64_t host_ptr, uint64_t out_ptr) {
    if (cap_find_resource(pid, CAP_RESOURCE_DEVICE, CAP_READ, DEVICE_ID_NET) != CAP_SUCCESS) {
        return SYSCALL_EPERM;
    }
    if (!in_user_range(out_ptr)) {
        return SYSCALL_EFAULT;
    }

    char host[64];
    if (copy_user_path(host_ptr, host, sizeof(host)) != 0) {
        return SYSCALL_EFAULT;
    }

    /* Poll first: if a result for the in-flight request is ready, deliver
     * it. A fresh (idle) state returns 0 here, so we then post below. */
    uint8_t ip[4];
    int r = net_poll_resolve(ip);
    if (r == 1) {
        uint8_t *dst = (uint8_t *)out_ptr;
        for (int i = 0; i < 4; i++) {
            if (in_user_range(out_ptr + i)) {
                dst[i] = ip[i];
            }
        }
        return 0;
    }
    if (r < 0) {
        return SYSCALL_EIO; /* the lookup failed */
    }

    /* No NIC at all is a hard failure; a NIC that is still bringing its
     * DHCP lease up is transient — keep polling until the net thread is
     * ready and services the request. */
    if (!net_nic_present()) {
        return SYSCALL_EIO;
    }

    /* Pending or idle: (re)post the request. net_request_resolve returns
     * busy while one is already in flight or the lease isn't up yet, so
     * re-posting the same host each poll is harmless. */
    net_request_resolve(host);
    return RESOLVE_WOULDBLOCK;
}

/* SYS_UDP (epic #80): ring-3 UDP sockets, op-multiplexed over a request
 * struct (usys wrappers cap out at 3 registers — the socketcall
 * pattern). The layout must match user/usys.h's udp_req_t exactly:
 * 24 bytes, buf at offset 16, no padding either side. */
typedef struct __attribute__((packed)) {
    int64_t sock;
    uint8_t ip[4];
    uint16_t port;
    uint16_t len;
    uint64_t buf;
} udp_req_k_t;
_Static_assert(sizeof(udp_req_k_t) == 24, "udp_req_t ABI drift");

/* Map a net_udp_* result onto the syscall ABI. */
static uint64_t udp_map_err(long r) {
    switch (r) {
    case NET_UDP_EINVAL:
        return SYSCALL_EINVAL;
    case NET_UDP_EPERM:
        return SYSCALL_EPERM;
    case NET_UDP_EAGAIN:
        return RESOLVE_WOULDBLOCK;
    case NET_UDP_ENONET:
        return SYSCALL_EIO;
    default:
        return (uint64_t)r;
    }
}

static uint64_t sys_udp(uint32_t pid, uint64_t op, uint64_t req_ptr) {
    /* Capability FIRST — before any argument or network check — so the
     * capless-denial gate fires even in the NIC-less default boot (the
     * sys_resolve precedent). */
    if (cap_find_resource(pid, CAP_RESOURCE_DEVICE, CAP_READ, DEVICE_ID_NET) != CAP_SUCCESS) {
        return SYSCALL_EPERM;
    }
    /* The WHOLE struct must sit inside the user window — a struct whose
     * first byte is in range but whose tail straddles the 2 GB boundary
     * would have the kernel read/write past the user half. */
    if (!in_user_range(req_ptr) || !in_user_range(req_ptr + sizeof(udp_req_k_t) - 1)) {
        return SYSCALL_EFAULT;
    }
    udp_req_k_t req;
    const uint8_t *usrc = (const uint8_t *)req_ptr;
    for (size_t i = 0; i < sizeof(req); i++) {
        ((uint8_t *)&req)[i] = usrc[i];
    }

    switch (op) {
    case UDP_OP_BIND:
        return udp_map_err(net_udp_bind(pid, req.port));

    case UDP_OP_SENDTO: {
        if (req.len > UDP_PAYLOAD_MAX) {
            return SYSCALL_EINVAL;
        }
        if (!net_udp_dst_ok(req.ip)) {
            return SYSCALL_EINVAL; /* fail fast — never ARP the unARPable */
        }
        /* req.buf is a second untrusted pointer; the user window is one
         * contiguous range, so endpoint checks cover every byte. */
        if (req.len > 0 && (!in_user_range(req.buf) || !in_user_range(req.buf + req.len - 1))) {
            return SYSCALL_EFAULT;
        }
        /* Bounce to kernel memory: syscalls run cli'd on one CPU (the
         * sys_readdir static-buffer justification), and the net thread
         * must never see a user pointer — it runs under its own CR3. */
        static uint8_t bounce[UDP_PAYLOAD_MAX];
        const uint8_t *ubuf = (const uint8_t *)req.buf;
        for (uint16_t i = 0; i < req.len; i++) {
            bounce[i] = ubuf[i];
        }
        return udp_map_err(net_udp_sendto(pid, (long)req.sock, req.ip, req.port, bounce, req.len));
    }

    case UDP_OP_RECVFROM: {
        uint16_t want = req.len > UDP_PAYLOAD_MAX ? UDP_PAYLOAD_MAX : req.len;
        if (want > 0 && (!in_user_range(req.buf) || !in_user_range(req.buf + want - 1))) {
            return SYSCALL_EFAULT;
        }
        static uint8_t bounce[UDP_PAYLOAD_MAX];
        uint8_t sip[4];
        uint16_t sport = 0;
        long r = net_udp_recvfrom(pid, (long)req.sock, bounce, want, sip, &sport);
        if (r < 0) {
            return udp_map_err(r);
        }
        uint8_t *ubuf = (uint8_t *)req.buf;
        for (long i = 0; i < r; i++) {
            ubuf[i] = bounce[i];
        }
        /* Report the sender back through the (already validated) struct. */
        udp_req_k_t *ureq = (udp_req_k_t *)req_ptr;
        for (int i = 0; i < 4; i++) {
            ureq->ip[i] = sip[i];
        }
        ureq->port = sport;
        ureq->len = (uint16_t)r;
        return (uint64_t)r;
    }

    case UDP_OP_CLOSE:
        return udp_map_err(net_udp_close(pid, (long)req.sock));

    default:
        return SYSCALL_EINVAL;
    }
}

/* SYS_TCP (epic #82): ring-3 TCP client. Same request-struct ABI shape as
 * SYS_UDP (the `sock`/`ip`/`port`/`len`/`buf` layout); the single
 * connection means `sock` is unused. */
#define TCP_SEND_MAX 1460 /* == TCP_MSS */

static uint64_t tcp_map_err(long r) {
    switch (r) {
    case NET_TCP_EINVAL:
        return SYSCALL_EINVAL;
    case NET_TCP_EIO:
        return SYSCALL_EIO;
    case NET_TCP_WOULDBLOCK:
        return RESOLVE_WOULDBLOCK;
    case NET_TCP_ENONET:
        return SYSCALL_EIO;
    default:
        return (uint64_t)r;
    }
}

static uint64_t sys_tcp(uint32_t pid, uint64_t op, uint64_t req_ptr) {
    /* Capability FIRST (the sys_udp/sys_resolve precedent) so the capless
     * gate fires even in the NIC-less default boot. */
    if (cap_find_resource(pid, CAP_RESOURCE_DEVICE, CAP_READ, DEVICE_ID_NET) != CAP_SUCCESS) {
        return SYSCALL_EPERM;
    }
    if (!in_user_range(req_ptr) || !in_user_range(req_ptr + sizeof(udp_req_k_t) - 1)) {
        return SYSCALL_EFAULT;
    }
    udp_req_k_t req;
    const uint8_t *usrc = (const uint8_t *)req_ptr;
    for (size_t i = 0; i < sizeof(req); i++) {
        ((uint8_t *)&req)[i] = usrc[i];
    }

    switch (op) {
    case TCP_OP_CONNECT:
        if (!net_udp_dst_ok(req.ip)) {
            return SYSCALL_EINVAL; /* same unARPable-destination guard as UDP */
        }
        return tcp_map_err(net_tcp_connect(pid, req.ip, req.port));

    case TCP_OP_SEND: {
        if (req.len > TCP_SEND_MAX) {
            return SYSCALL_EINVAL;
        }
        if (req.len > 0 && (!in_user_range(req.buf) || !in_user_range(req.buf + req.len - 1))) {
            return SYSCALL_EFAULT;
        }
        static uint8_t bounce[TCP_SEND_MAX];
        const uint8_t *ubuf = (const uint8_t *)req.buf;
        for (uint16_t i = 0; i < req.len; i++) {
            bounce[i] = ubuf[i];
        }
        return tcp_map_err(net_tcp_send(pid, bounce, req.len));
    }

    case TCP_OP_RECV: {
        uint16_t want = req.len > TCP_SEND_MAX ? TCP_SEND_MAX : req.len;
        if (want > 0 && (!in_user_range(req.buf) || !in_user_range(req.buf + want - 1))) {
            return SYSCALL_EFAULT;
        }
        static uint8_t bounce[TCP_SEND_MAX];
        long r = net_tcp_recv(pid, bounce, want);
        if (r < 0) {
            return tcp_map_err(r);
        }
        uint8_t *ubuf = (uint8_t *)req.buf;
        for (long i = 0; i < r; i++) {
            ubuf[i] = bounce[i];
        }
        return (uint64_t)r; /* 0 = EOF */
    }

    case TCP_OP_CLOSE:
        return tcp_map_err(net_tcp_close(pid));

    case TCP_OP_STATUS:
        return tcp_map_err(net_tcp_status(pid));

    default:
        return SYSCALL_EINVAL;
    }
}

static uint64_t sys_read(uint32_t pid, uint64_t fd, uint64_t user_ptr, uint64_t len) {
    process_t *cur = process_get_by_pid(pid);
    if (!cur || fd >= PROCESS_MAX_FDS || !cur->fds[fd].used) {
        return SYSCALL_EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (!in_user_range(user_ptr)) {
        return SYSCALL_EFAULT;
    }

    uint32_t remaining = cur->fds[fd].size - cur->fds[fd].offset;
    if (len > remaining) {
        len = remaining;
    }

    const uint8_t *src = cur->fds[fd].data + cur->fds[fd].offset;
    uint8_t *dst = (uint8_t *)user_ptr;
    uint32_t n = 0;
    while (n < len && in_user_range(user_ptr + n)) {
        dst[n] = src[n];
        n++;
    }
    cur->fds[fd].offset += n;
    return n;
}

static uint64_t sys_close(uint32_t pid, uint64_t fd) {
    process_t *cur = process_get_by_pid(pid);
    if (!cur || fd >= PROCESS_MAX_FDS || !cur->fds[fd].used) {
        return SYSCALL_EINVAL;
    }
    /* Reset EVERY field: a later open into this slot on the RO path sets
     * only data/size/offset — a lingering writable/ram_idx would make a
     * read-only reopen wrongly appear writable and target a stale file. */
    cur->fds[fd].used = false;
    cur->fds[fd].data = NULL;
    cur->fds[fd].size = 0;
    cur->fds[fd].offset = 0;
    cur->fds[fd].writable = false;
    cur->fds[fd].ram_idx = -1;
    return 0;
}

static uint64_t sys_readdir(uint32_t pid, uint64_t path_ptr, uint64_t user_ptr, uint64_t len) {
    (void)pid;
    if (len == 0) {
        return 0;
    }
    if (!in_user_range(user_ptr)) {
        return SYSCALL_EFAULT;
    }

    char path[VFS_PATH_MAX];
    if (copy_user_path(path_ptr, path, sizeof(path)) != 0) {
        return SYSCALL_EFAULT;
    }

    /* Static bounce buffer: syscalls run cli'd on one CPU (same
     * justification as sys_sysinfo). Initrd rows first, then the RAM
     * overlay's (tagged [ram]) appended after them. */
    static char tmp[SYSINFO_MAX_BYTES];
    size_t produced = initrd_format_list(path, tmp, sizeof(tmp));
    produced = ramfs_format_list(path, tmp, sizeof(tmp), produced);

    if (len > produced) {
        len = produced;
    }
    char *dst = (char *)user_ptr;
    uint32_t n = 0;
    while (n < len && in_user_range(user_ptr + n)) {
        dst[n] = tmp[n];
        n++;
    }
    return n;
}

/* ---- Program execution off the filesystem (epic #62 phase 3, #65) ----
 *
 * SYS_SPAWN loads a named initrd ELF into a fresh address space through
 * the same proven loader the boot-time services use. Unlike the VFS
 * reads, spawning is REAL authority — a process that can start programs
 * can multiply — so it is capability-gated: CAP_RESOURCE_PROCESS with
 * CAP_EXECUTE over SPAWN_RESOURCE_ID, declaratively granted to qsh
 * alone. The capless ghost-test proves the denial by attack every boot. */

/* Longest command line SYS_SPAWN copies from user memory (path + args). */
#define SPAWN_CMDLINE_MAX 256

/* Split `cmd` on runs of spaces/tabs into argv, packing the tokens into
 * `ka`. Returns the token count (>= 0). argv[0] is the first token — the
 * initrd path to load; it is also copied to `path0` (bounded by pmax). */
static int parse_cmdline(const char *cmd, kuser_args_t *ka, char *path0, size_t pmax) {
    ka->argc = 0;
    size_t soff = 0;
    const char *p = cmd;
    while (*p && ka->argc < KARGS_MAX) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        /* Record this token's start offset, copy until the next space. */
        unsigned start = (unsigned)soff;
        while (*p && *p != ' ' && *p != '\t' && soff + 1 < KARGS_STRBYTES) {
            char c = *p++;
            if (ka->argc == 0 && (size_t)(soff - start) < pmax - 1) {
                path0[soff - start] = c;
            }
            ka->strings[soff++] = c;
        }
        ka->strings[soff++] = '\0';
        if (ka->argc == 0) {
            size_t plen = (soff - 1) - start;
            path0[plen < pmax ? plen : pmax - 1] = '\0';
        }
        ka->argv_off[ka->argc] = start;
        ka->argc++;
        /* Skip any overflow of an over-long token. */
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
    }
    return ka->argc;
}

static uint64_t sys_spawn(uint32_t pid, uint64_t cmd_ptr) {
    if (cap_find_resource(pid, CAP_RESOURCE_PROCESS, CAP_EXECUTE, SPAWN_RESOURCE_ID) !=
        CAP_SUCCESS) {
        return SYSCALL_EPERM;
    }

    /* Copy the whole command line (path + args) out of user memory. */
    char cmd[SPAWN_CMDLINE_MAX];
    if (!in_user_range(cmd_ptr)) {
        return SYSCALL_EFAULT;
    }
    {
        const char *src = (const char *)cmd_ptr;
        size_t i = 0;
        while (i < sizeof(cmd) - 1 && in_user_range(cmd_ptr + i) && src[i]) {
            cmd[i] = src[i];
            i++;
        }
        cmd[i] = '\0';
    }

    /* Parse into an argv (argv[0] = the initrd path to load). */
    static kuser_args_t ka; /* syscalls run cli'd on one CPU — not re-entered */
    char path[VFS_PATH_MAX];
    if (parse_cmdline(cmd, &ka, path, sizeof(path)) < 1) {
        return SYSCALL_EINVAL;
    }

    const uint8_t *data = NULL;
    uint32_t size = 0;
    if (initrd_lookup(path, &data, &size) != 0) {
        return SYSCALL_ENOENT;
    }

    /* Process name = the path's basename. */
    const char *name = path;
    for (const char *q = path; *q; q++) {
        if (*q == '/' && q[1]) {
            name = q + 1;
        }
    }

    uint32_t new_pid = 0;
    if (spawn_elf_args(name, data, data + size, &ka, &new_pid) != STATUS_SUCCESS) {
        return SYSCALL_EIO;
    }
    return new_pid;
}

static uint64_t sys_waitpid(uint32_t pid, uint64_t target) {
    (void)pid;
    if (target >= MAX_PROCESSES) {
        return SYSCALL_EINVAL;
    }

    /* Ledger first, newest to oldest: a reaped (or recycled) pid still
     * resolves to the exit the waiter is polling for. */
    for (uint32_t i = 0; i < EXIT_LEDGER_SIZE; i++) {
        uint32_t idx = (exit_ledger_next + EXIT_LEDGER_SIZE - 1 - i) % EXIT_LEDGER_SIZE;
        if (exit_ledger[idx].valid && exit_ledger[idx].pid == (uint32_t)target) {
            return (uint64_t)(uint8_t)exit_ledger[idx].code;
        }
    }

    process_t *p = process_get_by_pid((uint32_t)target);
    if (p) {
        return WAITPID_RUNNING;
    }
    return SYSCALL_ENOENT;
}

/* Report the boot qseed (SYS_QSEED). Gated on the same quantum-pool read
 * capability as SYS_QRAND — a capless caller cannot even learn the seed. The
 * value lets a ring-3 attestation service bind its boot record to the exact
 * entropy the kernel accepted on the cmdline (0 = no qseed handoff). */
static uint64_t sys_qseed(uint32_t pid) {
    uint32_t rid = 0;
    if (cap_find(pid, CAP_RESOURCE_QUANTUM, CAP_READ, &rid) != CAP_SUCCESS ||
        rid != QUANTUM_POOL_RESOURCE_ID) {
        return SYSCALL_EPERM;
    }
    return quantum_boot_seed();
}

/* Live memory-field visualization buffer. A ring-3 producer (ghostd) publishes
 * a downsampled snapshot of its field here via SYS_FIELD_SNAPSHOT; the
 * framebuffer renderer drains it in the idle loop. This is a display sink only
 * — it carries no capability and confers no authority, so it needs no cap
 * check (the worst a rogue writer can do is draw noise on a screen that only
 * exists under a GRUB/ISO boot). */
static int8_t g_field_snap[FIELD_SNAP_BYTES];
static int g_field_n = 0;
static volatile bool g_field_dirty = false;

static uint64_t sys_field_snapshot(uint32_t pid, uint64_t user_ptr, uint64_t len) {
    (void)pid;
    if (len == 0) {
        return 0;
    }
    if (!in_user_range(user_ptr)) {
        return SYSCALL_EFAULT;
    }
    if (len > FIELD_SNAP_BYTES) {
        len = FIELD_SNAP_BYTES;
    }
    const int8_t *src = (const int8_t *)user_ptr;
    uint32_t n = 0;
    while (n < len && in_user_range(user_ptr + n)) {
        g_field_snap[n] = src[n];
        n++;
    }
    g_field_n = (int)n;
    g_field_dirty = true;
    return n;
}

bool field_snapshot_take(int8_t *dst, int *out_n) {
    if (!g_field_dirty) {
        return false;
    }
    int n = g_field_n;
    for (int i = 0; i < n; i++) {
        dst[i] = g_field_snap[i];
    }
    *out_n = n;
    g_field_dirty = false;
    return true;
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
    case SYS_HEARTBEAT:
        service_heartbeat(); /* uses the current process to find its slot */
        state->rax = 0;
        break;
    case SYS_SVC_RESTARTS:
        state->rax = (uint64_t)(int64_t)service_current_restart_count();
        break;
    case SYS_QRAND:
        state->rax = sys_qrand(pid, state->rdi, state->rsi);
        break;
    case SYS_SEND_TO:
        state->rax = sys_send_to(pid, state->rdi, state->rsi, state->rdx);
        break;
    case SYS_COM2:
        state->rax = sys_com2(pid, state->rdi, state->rsi, state->rdx);
        break;
    case SYS_QSEED:
        state->rax = sys_qseed(pid);
        break;
    case SYS_FIELD_SNAPSHOT:
        state->rax = sys_field_snapshot(pid, state->rdi, state->rsi);
        break;
    case SYS_CONS:
        state->rax = sys_cons(pid, state->rdi, state->rsi, state->rdx);
        break;
    case SYS_SYSINFO:
        state->rax = sys_sysinfo(pid, state->rdi, state->rsi, state->rdx);
        break;
    case SYS_OPEN:
        state->rax = sys_open(pid, state->rdi, state->rsi);
        break;
    case SYS_READ:
        state->rax = sys_read(pid, state->rdi, state->rsi, state->rdx);
        break;
    case SYS_CLOSE:
        state->rax = sys_close(pid, state->rdi);
        break;
    case SYS_READDIR:
        state->rax = sys_readdir(pid, state->rdi, state->rsi, state->rdx);
        break;
    case SYS_SPAWN:
        state->rax = sys_spawn(pid, state->rdi);
        break;
    case SYS_WAITPID:
        state->rax = sys_waitpid(pid, state->rdi);
        break;
    case SYS_FWRITE:
        state->rax = sys_fwrite(pid, state->rdi, state->rsi, state->rdx);
        break;
    case SYS_UNLINK:
        state->rax = sys_unlink(pid, state->rdi);
        break;
    case SYS_SYNC:
        state->rax = sys_sync_fs(pid);
        break;
    case SYS_RESOLVE:
        state->rax = sys_resolve(pid, state->rdi, state->rsi);
        break;
    case SYS_UDP:
        state->rax = sys_udp(pid, state->rdi, state->rsi);
        break;
    case SYS_TCP:
        state->rax = sys_tcp(pid, state->rdi, state->rsi);
        break;
    default:
        state->rax = SYSCALL_ENOSYS;
        break;
    }
}

void syscall_init(void) {
    /* DPL 3 gate: ring 3 may execute int 0x80; everything else in the
     * IDT stays kernel-only */
    idt_set_gate(SYSCALL_VECTOR, (uint64_t)isr128, GDT_KERNEL_CS, GATE_TYPE_INTERRUPT | DPL_USER);
    interrupt_register(SYSCALL_VECTOR, syscall_dispatch, NULL);
    boot_log("Syscall interface installed (int 0x80)");
}

/* ============================================================================
 * User process setup
 * ============================================================================ */

/* Allocate a physical frame and map it at uvaddr in `as`. Returns the
 * frame's kernel-VA (== physical, identity-mapped) for population, or
 * NULL on failure. */
static uint8_t *map_fresh_page(address_space_t *as, uint64_t uvaddr, bool writable) {
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

status_t user_process_spawn(const char *name, const void *blob_start, const void *blob_end,
                            uint32_t *pid_out) {
    size_t blob_size = (size_t)((const uint8_t *)blob_end - (const uint8_t *)blob_start);
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

    /* Stack + process creation shared with the ELF path (no args) */
    return finalize_user_process(&as, name, USER_VBASE, NULL, pid_out);
}

/* Map the user stack and create the ring-3 process bound to `as`, with
 * the given entry point. Shared by the flat-blob and ELF spawn paths.
 * `kargs` (may be NULL) is the argument vector to expose read-only at
 * USER_ARGS_VADDR; NULL leaves the process with argc == 0. */
static status_t finalize_user_process(address_space_t *as, const char *name, uint64_t entry,
                                      const kuser_args_t *kargs, uint32_t *pid_out) {
    /* User stack, mapped just below USER_STACK_TOP */
    for (uint32_t p = 1; p <= USER_STACK_PAGES; p++) {
        uint64_t uvaddr = USER_STACK_TOP - (uint64_t)p * PAGE_SIZE;
        if (!map_fresh_page(as, uvaddr, true)) {
            return STATUS_NO_MEMORY;
        }
    }

    /* Argument-vector page: mapped read-only to the process (populated here
     * through the frame's supervisor identity VA). Always present so
     * get_args() reads a valid page; a spawn without args leaves argc == 0. */
    uint8_t *argpage = map_fresh_page(as, USER_ARGS_VADDR, false);
    if (!argpage) {
        return STATUS_NO_MEMORY;
    }
    if (kargs) {
        const uint8_t *s = (const uint8_t *)kargs;
        for (size_t b = 0; b < sizeof(kuser_args_t) && b < PAGE_SIZE; b++) {
            argpage[b] = s[b];
        }
    } /* else the page is already zeroed by map_fresh_page -> argc == 0 */

    process_create_params_t params = {.name = name,
                                      .type = PROCESS_TYPE_USER,
                                      .priority = PRIORITY_NORMAL,
                                      .parent_pid = KERNEL_PROCESS_ID,
                                      .entry_point = (void *)entry,
                                      .stack_address = (void *)(USER_STACK_TOP - USER_REGION_SIZE),
                                      .stack_size =
                                          USER_REGION_SIZE, /* so stack_top == USER_STACK_TOP */
                                      .is_quantum_aware = false};

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
/* Full ELF spawn with an optional argument vector. `kargs` may be NULL. */
static status_t spawn_elf_args(const char *name, const void *elf_start, const void *elf_end,
                               const kuser_args_t *kargs, uint32_t *pid_out) {
    size_t elf_size = (size_t)((const uint8_t *)elf_end - (const uint8_t *)elf_start);

    address_space_t as = vmspace_create();
    if (!as.pml4) {
        return STATUS_NO_MEMORY;
    }

    uint64_t entry = 0;
    if (elf_load(&as, (const uint8_t *)elf_start, elf_size, &entry) != ELF_OK) {
        return STATUS_ERROR;
    }

    return finalize_user_process(&as, name, entry, kargs, pid_out);
}

status_t user_process_spawn_elf(const char *name, const void *elf_start, const void *elf_end,
                                uint32_t *pid_out) {
    return spawn_elf_args(name, elf_start, elf_end, NULL, pid_out);
}

void user_init(void) {
    boot_log("Per-process address spaces enabled (private CR3 per user proc)");

    uint32_t pid = 0;

    /* A real compiled-C program, loaded from its embedded ELF image */
    if (user_process_spawn_elf("init", _binary_init_elf_start, _binary_init_elf_end, &pid) !=
        STATUS_SUCCESS) {
        boot_log("Warning: failed to load init ELF");
    }

    /* Hand-written blobs for the isolation + fault-containment demos */
    if (user_process_spawn("user-canary-1", user_canary_start, user_canary_end, &pid) !=
            STATUS_SUCCESS ||
        user_process_spawn("user-canary-2", user_canary_start, user_canary_end, &pid) !=
            STATUS_SUCCESS ||
        user_process_spawn("user-rogue", user_rogue_start, user_rogue_end, &pid) !=
            STATUS_SUCCESS) {
        boot_log("Warning: failed to spawn user processes");
        return;
    }
    boot_log("User processes spawned (init ELF, 2x canary, 1x rogue)");

    user_ipc_demo_init();
    user_ghost_demo_init();
    user_quantum_demo_init();
}

/* Bring up quantumd — a quantum-pool service (kannaka-quantum, a fourth
 * ghostOS citizen). grant_quantum_pool=1 mints its SYS_QRAND/SYS_QSEED read
 * cap on every start, so it draws real collapse-derived entropy and refuses to
 * fall back to a PRNG. Monitored, so the watchdog keeps it resident after it
 * prints its one-shot quantum demo. */
void user_quantum_demo_init(void) {
    service_definition_t quantumd_def = {
        .name = "quantumd",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_quantumd_elf_start,
        .user_elf_end = _binary_quantumd_elf_end,
        .dependencies = {NULL},
        .max_restarts = 2,
        .grant_quantum_pool = 1,
    };
    uint32_t sid = 0;
    if (service_register(&quantumd_def, &sid) == SVC_SUCCESS &&
        service_start("quantumd", NULL) == SVC_SUCCESS) {
        service_monitor(sid, true);
        boot_log("kannaka-quantum: quantumd (ring 3) drawing real quantum entropy");
    } else {
        boot_log("Warning: quantumd service failed to start");
    }
}

/* Bring up a user-space service (echo) via the service framework and a
 * client that talks to it over capability-checked IPC. Demonstrates the
 * microkernel model: a system service running as an isolated ring-3
 * process, reachable only by a process holding the right capability. */
void user_ipc_demo_init(void) {
    service_definition_t echo_def = {
        .name = "echo",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_echo_elf_start,
        .user_elf_end = _binary_echo_elf_end,
        .dependencies = {NULL},
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

    if (user_process_spawn_elf("ipc-client", _binary_client_elf_start, _binary_client_elf_end,
                               &client_pid) != STATUS_SUCCESS) {
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

    /* A monitored user-process service: it heartbeats via SYS_HEARTBEAT
     * and the watchdog restarts it when it goes silent — demonstrating
     * that ring-3 services are first-class in the health monitor. */
    service_definition_t watched_def = {
        .name = "watched-svc",
        .entry = NULL,
        .user_elf_start = _binary_hbsvc_elf_start,
        .user_elf_end = _binary_hbsvc_elf_end,
        .dependencies = {NULL},
        .max_restarts = 2,
    };
    uint32_t wsid = 0;
    if (service_register(&watched_def, &wsid) == SVC_SUCCESS &&
        service_start("watched-svc", NULL) == SVC_SUCCESS) {
        service_monitor(wsid, true);
        boot_log("Watchdog now monitoring a ring-3 user service (watched-svc)");
    }
}

/* Bring up ghostd — a Hopfield–Kuramoto associative-memory service
 * (ghostOS phase 1) — as a monitored ring-3 user-process service, and a
 * ghost_test client that drives the boot self-test / merge gate over
 * capability-checked IPC. Same shape as the echo demo: register + start
 * the service, spawn the client, then grant each side exactly one IPC
 * send-capability to the other (capability-as-address). */
void user_ghost_demo_init(void) {
    service_definition_t ghostd_def = {
        .name = "ghostd",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_ghostd_elf_start,
        .user_elf_end = _binary_ghostd_elf_end,
        .dependencies = {NULL},
        .max_restarts = 2,
        /* ghostd's perturbation noise reads the quantum pool; declaring the
         * cap here means the service framework re-mints it on every start,
         * so a watchdog-reborn ghostd regains qseed-derived noise instead of
         * degrading permanently to a PRNG. */
        .grant_quantum_pool = 1,
    };

    uint32_t sid = 0;
    uint32_t ghostd_pid = 0, test_pid = 0;

    if (service_register(&ghostd_def, &sid) == SVC_SUCCESS &&
        service_start("ghostd", NULL) == SVC_SUCCESS) {
        service_info_t info;
        if (service_status(sid, &info) == SVC_SUCCESS) {
            ghostd_pid = info.pid;
        }
    }
    if (ghostd_pid == 0) {
        boot_log("Warning: ghostd service failed to start");
        return;
    }

    if (user_process_spawn_elf("ghost-test", _binary_ghost_test_elf_start,
                               _binary_ghost_test_elf_end, &test_pid) != STATUS_SUCCESS) {
        boot_log("Warning: ghost-test failed to spawn");
        return;
    }

    uint32_t cap = CAP_ID_INVALID;
    cap_create(test_pid, CAP_RESOURCE_IPC, ghostd_pid, CAP_READ | CAP_WRITE, 0, &cap);
    cap_create(ghostd_pid, CAP_RESOURCE_IPC, test_pid, CAP_READ | CAP_WRITE, 0, &cap);

    /* ghostd's quantum-pool read cap is (re-)minted by the service framework
     * from ghostd_def.grant_quantum_pool on every start, so it survives a
     * watchdog restart. ghost-test is deliberately left without one: its
     * capless SYS_QRAND attempt is the proof-by-attack the gate denies
     * (EPERM). */

    /* Let the watchdog keep ghostd alive; it heartbeats via SYS_HEARTBEAT
     * and reprints "GHOSTD: FIELD REBORN" if it is ever restarted. */
    service_monitor(sid, true);

    boot_log("ghostOS: ghostd (ring 3) + ghost-test wired via capabilities");

    /* ghostOS phase 3: bring up paradoxd, coupled to ghostd's field. */
    user_paradox_demo_init(ghostd_pid);
}

/* Bring up paradoxd — a fixed-point paradox-resolution service (ghostOS
 * phase 3, #50) — as a monitored ring-3 user-process service coupled to
 * ghostd's field, plus a paradox-test client that hands it a canned
 * contradiction over capability-checked IPC.
 *
 * Wiring (all capability-as-address):
 *   - paradox-test -> paradoxd  : one IPC send-cap, to hand over the gate's
 *                                 canned contradiction.
 *   - paradoxd     -> ghostd    : one IPC send-cap, to poll ghostd STATUS.
 *   - ghostd       -> paradoxd  : one IPC send-cap, so ghostd can reply to
 *                                 paradoxd's STATUS query (ghostd replies to
 *                                 the sender via SYS_SEND_TO).
 * paradoxd is given NO cap back to paradox-test: the merge gate is paradoxd's
 * own printed RESOLVED line, so it never needs to answer the client. */
void user_paradox_demo_init(uint32_t ghostd_pid) {
    service_definition_t paradoxd_def = {
        .name = "paradoxd",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_paradoxd_elf_start,
        .user_elf_end = _binary_paradoxd_elf_end,
        .dependencies = {NULL},
        .max_restarts = 2,
    };

    uint32_t sid = 0;
    uint32_t paradoxd_pid = 0, test_pid = 0;

    if (service_register(&paradoxd_def, &sid) == SVC_SUCCESS &&
        service_start("paradoxd", NULL) == SVC_SUCCESS) {
        service_info_t info;
        if (service_status(sid, &info) == SVC_SUCCESS) {
            paradoxd_pid = info.pid;
        }
    }
    if (paradoxd_pid == 0) {
        boot_log("Warning: paradoxd service failed to start");
        return;
    }

    if (user_process_spawn_elf("paradox-test", _binary_paradox_test_elf_start,
                               _binary_paradox_test_elf_end, &test_pid) != STATUS_SUCCESS) {
        boot_log("Warning: paradox-test failed to spawn");
        return;
    }

    uint32_t cap = CAP_ID_INVALID;
    /* client -> paradoxd (hand over the contradiction) */
    cap_create(test_pid, CAP_RESOURCE_IPC, paradoxd_pid, CAP_READ | CAP_WRITE, 0, &cap);
    /* paradoxd -> ghostd (poll STATUS), and ghostd -> paradoxd (reply). The
     * two services are now coupled through ghostd's field over cap-checked
     * IPC — the point of phase 3. */
    cap_create(paradoxd_pid, CAP_RESOURCE_IPC, ghostd_pid, CAP_READ | CAP_WRITE, 0, &cap);
    cap_create(ghostd_pid, CAP_RESOURCE_IPC, paradoxd_pid, CAP_READ | CAP_WRITE, 0, &cap);

    service_monitor(sid, true);

    boot_log("ghostOS: paradoxd (ring 3) coupled to ghostd via capabilities");

    /* ghostd phase 4: bring up swarm_svc, the COM2 serial swarm bridge. */
    user_swarm_demo_init(ghostd_pid);
}

/* Bring up swarm_svc — the COM2 serial swarm bridge (ghostd phase 4, #51) —
 * as a monitored ring-3 user-process service. It is declaratively granted the
 * COM2 device cap and a quantum-pool read cap (both re-minted on every start),
 * plus a single IPC send-cap to ghostd so a remote DATA request can be routed
 * to the field over capability-checked IPC; ghostd gets a send-cap back so it
 * can reply to swarm_svc (SYS_SEND_TO to the sender pid). swarm_svc holds no
 * other authority: it can drive COM2, draw quantum bytes, and talk to ghostd —
 * nothing else. */
void user_swarm_demo_init(uint32_t ghostd_pid) {
    service_definition_t swarm_def = {
        .name = "swarm-svc",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_swarm_svc_elf_start,
        .user_elf_end = _binary_swarm_svc_elf_end,
        .dependencies = {NULL},
        .max_restarts = 2,
        /* Lamport key material is drawn from the qseed-mixed quantum pool. */
        .grant_quantum_pool = 1,
        /* Sole ring-3 holder of the COM2 device capability. */
        .grant_com2 = 1,
    };

    uint32_t sid = 0;
    uint32_t swarm_pid = 0;

    if (service_register(&swarm_def, &sid) == SVC_SUCCESS &&
        service_start("swarm-svc", NULL) == SVC_SUCCESS) {
        service_info_t info;
        if (service_status(sid, &info) == SVC_SUCCESS) {
            swarm_pid = info.pid;
        }
    }
    if (swarm_pid == 0) {
        boot_log("Warning: swarm-svc service failed to start");
        return;
    }

    /* swarm_svc <-> ghostd: one IPC send-cap each way, so a DATA request can be
     * routed to ghostd's field and ghostd can reply to the sender. */
    uint32_t cap = CAP_ID_INVALID;
    cap_create(swarm_pid, CAP_RESOURCE_IPC, ghostd_pid, CAP_READ | CAP_WRITE, 0, &cap);
    cap_create(ghostd_pid, CAP_RESOURCE_IPC, swarm_pid, CAP_READ | CAP_WRITE, 0, &cap);

    service_monitor(sid, true);

    boot_log("ghostOS: swarm-svc (ring 3) bridging COM2, wired to ghostd");

    /* Epic #62: the interactive shell is the last citizen up, once the
     * services it can talk to already exist. */
    user_shell_init(ghostd_pid);
}

/* Bring up qsh — the interactive shell (epic #62 phase 1, #63) — as a
 * monitored ring-3 user-process service. Its authority is declarative and
 * minimal: the console device capability (its whole reason to exist) and a
 * quantum-pool read cap (the qrand/qseed builtins) are re-minted by the
 * service framework on every start, so a watchdog-reborn shell keeps its
 * console. One IPC send-cap each way wires it to ghostd for the `ghost`
 * builtin (peer caps are NOT re-minted on restart — a known service.c
 * limitation shared with the other demo peers).
 *
 * `exit` is a feature, not a crash: the shell terminates, its heartbeat
 * goes silent, and the watchdog restarts it — the reborn banner is the
 * boot-log proof that an OS operator surface can die and come back. */
void user_shell_init(uint32_t ghostd_pid) {
    service_definition_t qsh_def = {
        .name = "qsh",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_qsh_elf_start,
        .user_elf_end = _binary_qsh_elf_end,
        .dependencies = {NULL},
        .max_restarts = 3,
        .grant_quantum_pool = 1,
        .grant_console = 1,
        /* The shell is the operator surface: it alone may start programs
         * off the initrd (SYS_SPAWN) and write the filesystem (create/
         * unlink/sync — epic #71). Re-minted on every start like the
         * other declared grants. */
        .grant_spawn = 1,
        .grant_fswrite = 1,
        /* Network access: the shell alone may resolve hostnames (SYS_RESOLVE). */
        .grant_net = 1,
    };

    uint32_t sid = 0;
    uint32_t qsh_pid = 0;

    if (service_register(&qsh_def, &sid) == SVC_SUCCESS &&
        service_start("qsh", NULL) == SVC_SUCCESS) {
        service_info_t info;
        if (service_status(sid, &info) == SVC_SUCCESS) {
            qsh_pid = info.pid;
        }
    }
    if (qsh_pid == 0) {
        boot_log("Warning: qsh shell service failed to start");
        return;
    }

    /* qsh <-> ghostd: the `ghost` builtin queries the field over
     * capability-checked IPC; ghostd replies to the sender via SYS_SEND_TO. */
    uint32_t cap = CAP_ID_INVALID;
    cap_create(qsh_pid, CAP_RESOURCE_IPC, ghostd_pid, CAP_READ | CAP_WRITE, 0, &cap);
    cap_create(ghostd_pid, CAP_RESOURCE_IPC, qsh_pid, CAP_READ | CAP_WRITE, 0, &cap);

    service_monitor(sid, true);

    boot_log("qsh: interactive shell online (ring 3, console capability)");
}
