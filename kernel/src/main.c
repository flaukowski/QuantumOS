#include <kernel/types.h>
#include <kernel/boot.h>
#include <kernel/memory.h>
#include <kernel/interrupts.h>
#include <kernel/ipc.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/capability.h>
#include <kernel/quantum.h>
#include <kernel/service.h>
#include <kernel/gdt.h>
#include <kernel/syscall.h>
#include <kernel/com2_uart.h>
#include <kernel/console.h>
#include <kernel/initrd.h>
#include <kernel/ata.h>
#include <kernel/ramfs.h>
#include <kernel/rtl8139.h>
#include <kernel/net.h>
#include <kernel/vga.h>
#include <kernel/fb.h>
#ifdef SCHED_RESONANT
#include <kernel/resonant_fixed.h>
#endif

// Boot splash dispatch: framebuffer when available, else VGA text
static void splash_begin(void) {
    if (fb_available())
        fb_boot_splash();
    else
        vga_boot_splash();
}
static void splash_stage(const char *label, int percent) {
    if (fb_available())
        fb_boot_stage(label, percent);
    else
        vga_boot_stage(label, percent);
}
static void splash_ready(void) {
    if (fb_available())
        fb_boot_ready();
    else
        vga_boot_ready();
}

// External symbols from linker script
extern uint8_t __bss_start;
extern uint8_t __bss_end;
extern uint8_t __end;

// Boot state tracking
static boot_state_t current_boot_state = BOOT_STATE_FIRMWARE;
static boot_config_t boot_config;

// Forward declarations
static void kernel_init(void);
static void early_init(void);
static void hal_init(void);
static void memory_subsystem_init(void);
static void interrupts_subsystem_init(void);
static void core_services_init(void);
static void ipc_subsystem_init(void);
static void process_subsystem_init(void);
static void scheduler_subsystem_init(void);
static void parse_boot_cmdline(uint32_t info_addr);

// Kernel main entry point
void kernel_main(uint32_t magic, uint32_t info_addr) {
    current_boot_state = BOOT_STATE_KERNEL_ENTRY;

    // Validate multiboot
    if (!boot_validate_multiboot(magic, info_addr)) {
        boot_panic("Invalid multiboot information");
        return; // Never reached
    }

    // Parse boot configuration
    boot_config.magic = magic;
    boot_config.boot_flags = *(uint32_t *)(uintptr_t)info_addr;

    boot_log("QuantumOS v0.1 booting...");
    boot_log("Multiboot information validated");

    // Parse the boot command line for a quantum entropy seed
    // (qseed=<hex>) handed in by the qBraid boot harness
    parse_boot_cmdline(info_addr);

    // Use a linear framebuffer if the loader provided one (GRUB ISO /
    // real hardware); otherwise the VGA text splash. The QEMU -kernel
    // path — CI and the qBraid watch window — provides none and stays
    // in text mode.
    if (fb_init_from_multiboot(info_addr)) {
        boot_log("Linear framebuffer active — graphical boot splash");
    } else {
        boot_log("No framebuffer — VGA text boot splash");
    }

    // Early initialization
    early_init();

    // Main kernel initialization
    kernel_init();

    // Should never reach here
    boot_panic("Kernel initialization completed unexpectedly");
}

// Early initialization (before memory management)
static void early_init(void) {
    boot_log("Starting early initialization...");

    // Initialize early console
    early_console_init();

    // Basic CPU setup
    // TODO: CPU detection and setup

    boot_log("Early initialization complete");
}

// Main kernel initialization
static void kernel_init(void) {
    boot_log("Starting kernel initialization...");

    // Draw the boot splash; each subsystem advances the progress bar
    // and travels the interference wave one step further
    splash_begin();
    splash_stage("hardware abstraction layer", 5);

    // Initialize HAL
    hal_init();

    // Initialize memory management
    splash_stage("memory management", 20);
    memory_subsystem_init();

    // Initialize interrupt system
    splash_stage("interrupt controller", 35);
    interrupts_subsystem_init();

    // Install the full GDT + TSS (user-mode descriptors, ring-3 entry
    // stack) and the syscall gate
    splash_stage("descriptor tables + syscalls", 45);
    gdt_init();
    syscall_init();

    // Bring up the COM2 serial swarm bridge (0x2F8, polled, capability-guarded).
    // COM1/console is untouched; swarm_svc drives this port from ring 3.
    com2_init();

    // Console INPUT (epic #62): COM1 RX + PS/2 keyboard feed a kernel ring
    // buffer; ring 3 reads it via the capability-guarded SYS_CONS. Any byte
    // CI has already piped into the serial console is rescued here, before
    // interrupts are even on. IRQ1/IRQ4 unmask alongside the timer below.
    console_init();

    // Mount the embedded initrd (epic #62 phase 2): a read-only ustar
    // archive baked into the kernel image, served to ring 3 through the
    // SYS_OPEN/SYS_READ/SYS_CLOSE/SYS_READDIR VFS calls.
    initrd_init();

    // Probe the ATA disk (epic #71): the persistence substrate. A
    // diskless boot logs itself honestly and changes nothing else.
    // With a disk carrying a QDSK archive, the RAM filesystem overlay
    // is restored from it — files written and synced in a previous
    // boot come back.
    ata_init();
    persist_restore();

    // Probe the RTL8139 NIC (epic #73). A NIC-less boot (the default
    // -kernel path) logs "no rtl8139" and continues unchanged; the IRQ
    // is unmasked and the ARP self-test runs only when a NIC is present.
    rtl8139_init();

    // Initialize core services
    splash_stage("capabilities + quantum resources", 60);
    core_services_init();

    // Spawn demo kernel threads and attach the scheduler
    splash_stage("scheduler + services", 75);
    scheduler_subsystem_init();

    // Map the user memory window and spawn the user-mode processes
    splash_stage("user-space processes", 90);
    user_init();

    // Start the periodic timer and enable interrupts
    splash_stage("timer + interrupts", 98);
    pit_init(TIMER_DEFAULT_HZ);
    interrupt_enable(IRQ_BASE + IRQ_TIMER);
    interrupt_enable(IRQ_BASE + IRQ_KEYBOARD);
    // Only listen to a UART that exists — on serial-less hardware the
    // line floats and its handler would just poll a phantom port.
    if (console_com1_present()) {
        interrupt_enable(IRQ_BASE + IRQ_COM1);
    }
    // Unmask the NIC's (dynamically assigned) IRQ line if a NIC came up.
    // Its handler is routed from irq_handler's default case. On a PCI
    // line >= 8 the slave PIC's cascade (IRQ2) must also be live — it is
    // unmasked by pic_init at boot.
    if (rtl8139_present()) {
        interrupt_enable(IRQ_BASE + rtl8139_irq_line());
    }
    interrupt_enable_all();
    boot_log("Timer started, interrupts enabled");

    boot_log("Kernel initialization complete");
    splash_ready();

    // Text-mode boots hand the display from the splash to the scrolling
    // VGA console: on a machine with no serial port (epic #101) this is
    // the only way to SEE the shell. Framebuffer boots keep the live
    // field view; their text stays on COM1.
    if (!fb_available()) {
        vga_console_enable();
        boot_log("CONS: screen console active (VGA text 80x25)");
    }

    boot_log("QuantumOS ready");

    // Idle loop for the kernel process. Reaps terminated processes
    // (reclaiming their address-space frames) with interrupts disabled
    // so the pmm free path can't race a preemption, then sleeps until
    // the next tick.
    //
    // Under a GRUB/ISO framebuffer boot it also drains ghostd's latest
    // published field snapshot and redraws the live memory-field view, so the
    // picture ripples with REMEMBER/RECALL activity. The snapshot is copied out
    // under the same interrupts-disabled window (a bounded 256-byte copy) so a
    // concurrent SYS_FIELD_SNAPSHOT cannot tear it; the draw runs with
    // interrupts back on. On the -kernel/VGA path fb_available() is false and
    // this is skipped entirely, so the text boot is untouched.
    static int8_t field_buf[FIELD_SNAP_BYTES];
    while (1) {
        interrupt_disable_all();
        process_reap();
        int field_n = 0;
        bool have_field = fb_available() && field_snapshot_take(field_buf, &field_n);
        interrupt_enable_all();
        if (have_field) {
            fb_render_field(field_buf, field_n);
        }
        __asm__ volatile("hlt");
    }
}

// HAL initialization
static void hal_init(void) {
    current_boot_state = BOOT_STATE_HAL_INIT;
    boot_log("Initializing HAL...");

    // TODO: Initialize hardware abstraction layer
    // - CPU detection
    // - Feature detection
    // - Basic hardware setup

    boot_log("HAL initialization complete");
}

// Memory management initialization
static void memory_subsystem_init(void) {
    current_boot_state = BOOT_STATE_MEMORY_INIT;
    boot_log("Initializing memory management...");

    // Call the real memory_init from memory.h
    mem_result_t result = memory_init();
    if (result != MEM_SUCCESS) {
        boot_log("Warning: Memory init returned non-success");
    }

    boot_log("Memory management initialization complete");
}

// Interrupt system initialization
static void interrupts_subsystem_init(void) {
    current_boot_state = BOOT_STATE_INTERRUPTS_INIT;
    boot_log("Initializing interrupt system...");

    // Call the real interrupts_init from interrupts.h
    irq_result_t result = interrupts_init();
    if (result != IRQ_SUCCESS) {
        boot_log("Warning: Interrupts init returned non-success");
    }

    boot_log("Interrupt system initialization complete");
}

// Core services initialization
static void core_services_init(void) {
    current_boot_state = BOOT_STATE_CORE_SERVICES;
    boot_log("Initializing core services...");

    // Initialize capability system first — process creation grants
    // each process its root capability
    if (cap_init() != CAP_SUCCESS) {
        boot_panic("Failed to initialize capability system");
    }
    if (cap_selftest() != CAP_SUCCESS) {
        boot_panic("Capability self-test failed");
    }

    // Initialize quantum resource manager (before processes — quantum-
    // aware processes are granted pool capabilities at creation)
    if (quantum_init() != QUANTUM_SUCCESS) {
        boot_panic("Failed to initialize quantum resource manager");
    }
    if (quantum_selftest() != QUANTUM_SUCCESS) {
        boot_panic("Quantum self-test failed");
    }

    // Initialize process management system
    process_subsystem_init();

    // Initialize IPC system
    ipc_subsystem_init();

    // Initialize the service manager and bring up system services
    // (they run once the scheduler and timer start)
    if (service_manager_init() != SVC_SUCCESS) {
        boot_panic("Failed to initialize service manager");
    }
    if (service_selftest() != SVC_SUCCESS) {
        boot_panic("Service framework self-test failed");
    }

    boot_log("Core services initialization complete");
}

// IPC subsystem initialization
static void ipc_subsystem_init(void) {
    boot_log("Initializing IPC subsystem...");

    ipc_result_t result = ipc_init();
    if (result != IPC_SUCCESS) {
        boot_panic("Failed to initialize IPC subsystem");
        return;
    }

    boot_log("IPC subsystem initialized");
}

// Process subsystem initialization
static void process_subsystem_init(void) {
    boot_log("Initializing process subsystem...");

    status_t result = process_init();
    if (result != STATUS_SUCCESS) {
        boot_panic("Failed to initialize process subsystem");
        return;
    }

    boot_log("Process subsystem initialized");
}

// ============================================================================
// Demo kernel threads — visible proof that preemptive scheduling works
// ============================================================================

static volatile uint64_t alpha_wakeups = 0;
static volatile uint64_t beta_wakeups = 0;

static void demo_thread_alpha(void) {
    boot_log("kernel-thread alpha: online");
    while (1) {
        __asm__ volatile("hlt");
        if (++alpha_wakeups % 200 == 0) {
            boot_log_v("kernel-thread alpha: alive");
        }
    }
}

static void demo_thread_beta(void) {
    boot_log("kernel-thread beta: online");
    while (1) {
        __asm__ volatile("hlt");
        if (++beta_wakeups % 200 == 0) {
            boot_log_v("kernel-thread beta: alive");
        }
    }
}

// Network self-test thread (epic #73): runs once interrupts are live so
// the NIC RX IRQ can fire, ARP-resolves the user-net gateway, then idles.
// A no-op on the NIC-less default boot.
static void net_thread(void) {
    net_init();
    net_selftest();
    // Then serve ring-3 resolve requests forever (SYS_RESOLVE). Network
    // I/O must live here, in an IF=1 thread, because a syscall runs with
    // interrupts disabled and could never pump the NIC's RX interrupt.
    net_service_loop();
}

// Scheduler + demo thread initialization
static void scheduler_subsystem_init(void) {
    boot_log("Initializing scheduler...");

    if (kernel_thread_create("alpha", demo_thread_alpha, PRIORITY_NORMAL, NULL) != STATUS_SUCCESS) {
        boot_log("Warning: failed to create kernel thread alpha");
    }
    if (kernel_thread_create("beta", demo_thread_beta, PRIORITY_NORMAL, NULL) != STATUS_SUCCESS) {
        boot_log("Warning: failed to create kernel thread beta");
    }
    if (rtl8139_present()) {
        if (kernel_thread_create("net", net_thread, PRIORITY_NORMAL, NULL) != STATUS_SUCCESS) {
            boot_log("Warning: failed to create kernel thread net");
        }
    }

    scheduler_init();

#ifdef SCHED_RESONANT
    /* Honest, deterministic measurement of the #21 hypothesis: run one
     * identical canned workload under round-robin and under the resonant
     * policy and print both fairness numbers + the order parameter r, before
     * the live system starts. A loss ships in the log, unedited. */
    boot_log("Resonant scheduler ACTIVE (SCHED_RESONANT) — measuring vs round-robin");
    resonant_fixed_boot_report();
#endif
}

// ============================================================================
// Boot command-line parsing — quantum entropy handoff (qseed=<hex>)
// ============================================================================

static int hex_val(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Quiet-boot flag (cmdline token `quiet`) — see boot_is_quiet().
static int g_boot_quiet = 0;

int boot_is_quiet(void) {
    return g_boot_quiet;
}

// Is the whole-word token `word` present in `cmd` (space/tab/start bounded on
// the left, space/tab/NUL bounded on the right)? Avoids matching `quiet` inside
// a longer token like `disquiet=1`.
static int cmdline_has_word(const char *cmd, const char *word) {
    for (const char *p = cmd; *p; p++) {
        if (p != cmd && p[-1] != ' ' && p[-1] != '\t') {
            continue; // not at a token boundary
        }
        int k = 0;
        while (word[k] && p[k] == word[k]) {
            k++;
        }
        if (word[k] == '\0' && (p[k] == '\0' || p[k] == ' ' || p[k] == '\t')) {
            return 1;
        }
    }
    return 0;
}

// Match "qseed=" at p; on hit, decode up to 16 hex digits into a u64. Also
// scans for the bare `quiet` token (clean interactive console).
static void parse_boot_cmdline(uint32_t info_addr) {
    uint32_t *info = (uint32_t *)(uintptr_t)info_addr;
    // Multiboot v1: flags bit 2 => cmdline valid; cmdline ptr at word 4
    if (!(info[0] & 0x4)) {
        return;
    }
    const char *cmd = (const char *)(uintptr_t)info[4];
    if (!cmd) {
        return;
    }

    if (cmdline_has_word(cmd, "quiet")) {
        g_boot_quiet = 1;
    }

    static const char key[] = "qseed=";
    for (const char *p = cmd; *p; p++) {
        // match key
        int k = 0;
        while (key[k] && p[k] == key[k])
            k++;
        if (key[k] != '\0') {
            continue;
        }
        const char *h = p + k;
        uint64_t seed = 0;
        int digits = 0;
        while (digits < 16 && hex_val(*h) >= 0) {
            seed = (seed << 4) | (uint64_t)hex_val(*h);
            h++;
            digits++;
        }
        if (digits > 0) {
            quantum_set_boot_entropy(seed);
            boot_log("Boot entropy accepted from cmdline (qseed=)");
            boot_log("  qseed value: ");
            early_console_write_hex(seed);
        }
        return;
    }
}

// Boot validation
bool boot_validate_multiboot(uint32_t magic, uint32_t info_addr) {
    /* boot.S carries a Multiboot v1 header; accept v2 as well in case a
     * v2-capable loader is used later. */
    if (magic != MULTIBOOT1_MAGIC && magic != MULTIBOOT2_MAGIC) {
        return false;
    }

    if (info_addr == 0) {
        return false;
    }

    // TODO: More thorough validation
    return true;
}

// Boot logging
void boot_log(const char *message) {
    early_console_write("[BOOT] ");
    early_console_write(message);
    early_console_write("\r\n");
    // Tee to the screen console (no-op until enabled): on serial-less
    // real hardware this is the only place the kernel log is visible.
    if (vga_console_active()) {
        vga_console_puts("[BOOT] ");
        vga_console_puts(message);
        vga_console_putc('\n');
    }
}

// Steady-state / demo boot logging — silenced under a `quiet` boot. See boot.h.
void boot_log_v(const char *message) {
    if (!g_boot_quiet) {
        boot_log(message);
    }
}

// boot_panic is provided by boot.S (assembly implementation)

// Early console initialization
void early_console_init(void) {
    // Initialize COM1 serial port (0x3F8)
    // TODO: Proper serial port initialization
}

// Utility functions
void *memset(void *ptr, int value, size_t num) {
    uint8_t *p = (uint8_t *)ptr;
    while (num--) {
        *p++ = (uint8_t)value;
    }
    return ptr;
}

void *memcpy(void *dest, const void *src, size_t num) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (num--) {
        *d++ = *s++;
    }
    return dest;
}

size_t strlen(const char *str) {
    size_t len = 0;
    while (*str++) {
        len++;
    }
    return len;
}
