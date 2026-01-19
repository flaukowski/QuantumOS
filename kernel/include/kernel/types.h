#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stddef.h>

// Basic types
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

// Boolean type
typedef u8 bool;
#define true 1
#define false 0

// NULL pointer
#ifndef NULL
#define NULL ((void*)0)
#endif

// Status codes
typedef enum {
    STATUS_SUCCESS = 0,
    STATUS_ERROR = -1,
    STATUS_INVALID_ARG = -2,
    STATUS_NO_MEMORY = -3,
    STATUS_NOT_FOUND = -4,
    STATUS_PERMISSION_DENIED = -5,
    STATUS_TIMEOUT = -6,
    STATUS_BUSY = -7,
    STATUS_NOT_IMPLEMENTED = -8
} status_t;

// Memory attributes
#define MEM_READ    0x01
#define MEM_WRITE   0x02
#define MEM_EXECUTE 0x04
#define MEM_USER    0x08
#define MEM_KERNEL  0x10
#define MEM_SHARED  0x20

// Page size (4KB standard)
#define PAGE_SIZE 4096
#define PAGE_SHIFT 12

// Alignment helpers
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)

// Array size macro
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

// Min/Max macros
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// Bit manipulation
#define BIT(x) (1U << (x))
#define BIT_SET(x, y) ((x) |= (y))
#define BIT_CLR(x, y) ((x) &= ~(y))
#define BIT_TST(x, y) ((x) & (y))

// Endianness (assuming little endian)
#define LITTLE_ENDIAN 1
#define BIG_ENDIAN 0

// Compiler attributes
#define PACKED __attribute__((packed))
#define ALIGNED(n) __attribute__((aligned(n)))
#define NORETURN __attribute__((noreturn))
#define WEAK __attribute__((weak))

// Function calling convention
#define CALLCONV

// Debug macros
#ifdef DEBUG
#define ASSERT(x) \
    do { \
        if (!(x)) { \
            panic("Assertion failed: %s at %s:%d", #x, __FILE__, __LINE__); \
        } \
    } while(0)
#else
#define ASSERT(x) ((void)0)
#endif

// Panic function
NORETURN void panic(const char *fmt, ...);

#endif // TYPES_H
