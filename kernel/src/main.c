#include <kernel/types.h>
#include <kernel/boot.h>
#include <kernel/memory.h>
#include <kernel/interrupts.h>
#include <kernel/ipc.h>
#include <kernel/process.h>

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
static void memory_init(void);
static void interrupts_init(void);
static void core_services_init(void);
static void ipc_subsystem_init(void);
static void process_subsystem_init(void);

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
    boot_config.boot_flags = *(uint32_t*)info_addr;
    
    boot_log("QuantumOS v0.1 booting...");
    boot_log("Multiboot information validated");
    
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
    
    // Initialize HAL
    hal_init();
    
    // Initialize memory management
    memory_init();
    
    // Initialize interrupt system
    interrupts_init();
    
    // Initialize core services
    core_services_init();
    
    boot_log("Kernel initialization complete");
    boot_log("QuantumOS ready");
    
    // Enter idle loop (for now)
    while (1) {
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
static void memory_init(void) {
    current_boot_state = BOOT_STATE_MEMORY_INIT;
    boot_log("Initializing memory management...");
    
    // TODO: Initialize memory management
    // - Page allocator
    // - Virtual memory
    // - Heap allocator
    
    boot_log("Memory management initialization complete");
}

// Interrupt system initialization
static void interrupts_init(void) {
    current_boot_state = BOOT_STATE_INTERRUPTS_INIT;
    boot_log("Initializing interrupt system...");
    
    // TODO: Initialize interrupt system
    // - IDT setup
    // - Exception handlers
    // - IRQ routing
    
    boot_log("Interrupt system initialization complete");
}

// Core services initialization
static void core_services_init(void) {
    current_boot_state = BOOT_STATE_CORE_SERVICES;
    boot_log("Initializing core services...");

    // Initialize process management system
    process_subsystem_init();

    // Initialize IPC system
    ipc_subsystem_init();

    // TODO: Initialize remaining core services
    // - Capability system
    // - Quantum subsystem

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

// Boot validation
bool boot_validate_multiboot(uint32_t magic, uint32_t info_addr) {
    if (magic != MULTIBOOT2_MAGIC) {
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
}

// Boot panic
void boot_panic(const char *message) {
    early_console_write("\r\n*** BOOT PANIC ***\r\n");
    early_console_write(message);
    early_console_write("\r\n");
    
    // Halt system
    __asm__ volatile("cli");
    while (1) {
        __asm__ volatile("hlt");
    }
}

// Early console initialization
void early_console_init(void) {
    // Initialize COM1 serial port (0x3F8)
    // TODO: Proper serial port initialization
}

// Utility functions
void *memset(void *ptr, int value, size_t num) {
    uint8_t *p = (uint8_t*)ptr;
    while (num--) {
        *p++ = (uint8_t)value;
    }
    return ptr;
}

void *memcpy(void *dest, const void *src, size_t num) {
    uint8_t *d = (uint8_t*)dest;
    const uint8_t *s = (const uint8_t*)src;
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
