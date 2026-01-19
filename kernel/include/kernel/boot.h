#ifndef BOOT_H
#define BOOT_H

#include <stdint.h>

// Boot magic numbers
#define MULTIBOOT2_MAGIC 0x36d76289

// Kernel entry point signature
typedef void (*kernel_entry_t)(uint32_t magic, uint32_t info_addr);

// Boot information structure
typedef struct {
    uint32_t magic;
    uint32_t flags;
    uint32_t checksum;
    uint32_t header_addr;
    uint32_t load_addr;
    uint32_t load_end_addr;
    uint32_t bss_end_addr;
    uint32_t entry_addr;
} multiboot_header_t;

// Boot flags
#define MULTIBOOT_FLAG_PAGE_ALIGN    0x00000001
#define MULTIBOOT_FLAG_MEMORY_INFO   0x00000002
#define MULTIBOOT_FLAG_VGA_MODE      0x00000004
#define MULTIBOOT_FLAG_REQUESTED     0x00010000

// Memory information
typedef struct {
    uint32_t lower_memory;
    uint32_t upper_memory;
    uint32_t memory_type;
    uint32_t memory_length;
} memory_info_t;

// Boot states
typedef enum {
    BOOT_STATE_FIRMWARE = 0,
    BOOT_STATE_BOOTLOADER = 1,
    BOOT_STATE_KERNEL_ENTRY = 2,
    BOOT_STATE_HAL_INIT = 3,
    BOOT_STATE_MEMORY_INIT = 4,
    BOOT_STATE_INTERRUPTS_INIT = 5,
    BOOT_STATE_CORE_SERVICES = 6,
    BOOT_STATE_USERSPACE = 7,
    BOOT_STATE_COMPLETE = 8
} boot_state_t;

// Boot configuration
typedef struct {
    uint32_t magic;
    uint32_t boot_flags;
    uint32_t memory_size;
    uint32_t kernel_size;
    uint32_t initrd_start;
    uint32_t initrd_size;
    char cmdline[256];
} boot_config_t;

// Boot functions
void kernel_main(uint32_t magic, uint32_t info_addr);
void boot_panic(const char *message);
void boot_log(const char *message);
bool boot_validate_multiboot(uint32_t magic, uint32_t info_addr);

// Early boot utilities
void early_console_init(void);
void early_console_write(const char *str);
void early_console_write_hex(uint64_t value);

// Memory utilities
void *memset(void *ptr, int value, size_t num);
void *memcpy(void *dest, const void *src, size_t num);
size_t strlen(const char *str);

// Stack configuration
#define BOOT_STACK_SIZE 8192
extern char boot_stack[BOOT_STACK_SIZE];

#endif // BOOT_H
