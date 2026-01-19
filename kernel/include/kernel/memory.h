#ifndef MEMORY_H
#define MEMORY_H

#include <kernel/types.h>

// Memory management result codes
typedef enum {
    MEM_SUCCESS = 0,
    MEM_ERROR_OUT_OF_MEMORY = -1,
    MEM_ERROR_INVALID_ADDRESS = -2,
    MEM_ERROR_ALIGNMENT = -3,
    MEM_ERROR_PERMISSION = -4,
    MEM_ERROR_ALREADY_MAPPED = -5
} mem_result_t;

// Memory permissions
#define MEM_READ    0x01
#define MEM_WRITE   0x02
#define MEM_EXECUTE 0x04
#define MEM_USER    0x08
#define MEM_KERNEL  0x10

// Page table entry (x86_64)
typedef struct {
    uint64_t present    : 1;
    uint64_t read_write : 1;
    uint64_t user       : 1;
    uint64_t pwt        : 1;  // Page-level write-through
    uint64_t pcd        : 1;  // Page-level cache disable
    uint64_t accessed   : 1;
    uint64_t dirty      : 1;
    uint64_t pat        : 1;  // Page-attribute table
    uint64_t global     : 1;
    uint64_t available  : 3;
    uint64_t frame      : 40; // Physical frame address
    uint64_t reserved   : 11;
    uint64_t nx         : 1;  // No-execute bit
} pte_t;

// Page table levels
typedef pte_t pml4e_t;   // PML4 (Level 4)
typedef pte_t pdpe_t;    // Page Directory Pointer (Level 3)
typedef pte_t pde_t;     // Page Directory (Level 2)
typedef pte_t pte_t;     // Page Table Entry (Level 1)

// Memory region
typedef struct {
    void *virtual_addr;
    void *physical_addr;
    size_t size;
    uint32_t permissions;
    uint8_t is_mapped;
    uint8_t is_allocated;
} memory_region_t;

// Memory allocator
typedef struct {
    void *start;
    void *end;
    void *current;
    size_t total_size;
    size_t used_size;
    size_t free_size;
} memory_allocator_t;

// Physical memory manager
typedef struct {
    uint32_t total_frames;
    uint32_t free_frames;
    uint32_t used_frames;
    uint8_t *frame_bitmap;
    uint64_t highest_frame;
} physical_memory_t;

// Virtual memory manager
typedef struct {
    pml4e_t *pml4_table;
    memory_region_t *regions;
    size_t region_count;
    memory_allocator_t kernel_heap;
} virtual_memory_t;

// Memory management functions
mem_result_t memory_init(void);
mem_result_t memory_map_page(void *virt_addr, void *phys_addr, uint32_t permissions);
mem_result_t memory_unmap_page(void *virt_addr);
mem_result_t memory_map_region(void *virt_addr, void *phys_addr, size_t size, uint32_t permissions);
mem_result_t memory_unmap_region(void *virt_addr, size_t size);

// Physical memory management
mem_result_t pmm_init(uint64_t total_memory);
void* pmm_alloc_frame(void);
mem_result_t pmm_free_frame(void *frame_addr);
uint32_t pmm_get_free_frames(void);
uint32_t pmm_get_total_frames(void);

// Virtual memory management
mem_result_t vmm_init(void);
void* vmm_alloc_page(uint32_t permissions);
mem_result_t vmm_free_page(void *virt_addr);
mem_result_t vmm_switch_context(pml4e_t *new_pml4);

// Kernel heap
mem_result_t kheap_init(void);
void* kmalloc(size_t size);
void kfree(void *ptr);
void* krealloc(void *ptr, size_t new_size);

// Memory utilities
bool is_aligned(void *ptr, size_t alignment);
bool is_user_address(void *addr);
bool is_kernel_address(void *addr);
void* align_up(void *ptr, size_t alignment);
void* align_down(void *ptr, size_t alignment);

// Memory debugging
void memory_dump_regions(void);
void memory_dump_page_tables(void);
void memory_dump_physical_map(void);

// Constants
#define PAGE_SIZE 4096
#define PAGE_SHIFT 12
#define PAGE_MASK (~(PAGE_SIZE - 1))

#define KERNEL_BASE_ADDR 0xFFFF800000000000
#define KERNEL_HEAP_START 0xFFFF800000000000
#define KERNEL_HEAP_SIZE 0x100000000  // 4GB

#define USER_BASE_ADDR 0x0000000000400000
#define USER_HEAP_START 0x0000000000800000

// Alignment macros
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)

#endif // MEMORY_H
