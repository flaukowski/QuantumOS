#include <kernel/types.h>
#include <kernel/memory.h>
#include <kernel/boot.h>

// External symbols
extern uint8_t __end;

// Global memory managers
static physical_memory_t pmm;
static virtual_memory_t vmm;
static memory_allocator_t kernel_heap;

// Physical memory management
mem_result_t pmm_init(uint64_t total_memory) {
    boot_log("Initializing physical memory manager...");
    
    // Calculate total frames
    uint64_t total_frames = total_memory / PAGE_SIZE;
    pmm.total_frames = (uint32_t)total_frames;
    pmm.free_frames = (uint32_t)total_frames;
    pmm.used_frames = 0;
    pmm.highest_frame = total_frames - 1;
    
    // Allocate frame bitmap
    size_t bitmap_size = total_frames / 8;
    pmm.frame_bitmap = (uint8_t*)ALIGN_UP(&__end, PAGE_SIZE);
    
    // Initialize bitmap (all frames initially free)
    memset(pmm.frame_bitmap, 0, bitmap_size);
    
    // Reserve frames used by kernel
    uint64_t kernel_end = (uint64_t)pmm.frame_bitmap + bitmap_size;
    uint64_t kernel_frames = (kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;
    
    for (uint64_t i = 0; i < kernel_frames; i++) {
        uint32_t frame = i;
        uint32_t byte_index = frame / 8;
        uint8_t bit_index = frame % 8;
        pmm.frame_bitmap[byte_index] |= (1 << bit_index);
        pmm.free_frames--;
        pmm.used_frames++;
    }
    
    boot_log("Physical memory manager initialized");
    boot_log("Total frames: "); early_console_write_hex(pmm.total_frames);
    boot_log("Free frames: "); early_console_write_hex(pmm.free_frames);
    
    return MEM_SUCCESS;
}

void* pmm_alloc_frame(void) {
    // Find first free frame
    for (uint32_t i = 0; i < pmm.total_frames; i++) {
        uint32_t byte_index = i / 8;
        uint8_t bit_index = i % 8;
        
        if (!(pmm.frame_bitmap[byte_index] & (1 << bit_index))) {
            // Mark frame as used
            pmm.frame_bitmap[byte_index] |= (1 << bit_index);
            pmm.free_frames--;
            pmm.used_frames++;
            
            void *frame_addr = (void*)(i * PAGE_SIZE);
            return frame_addr;
        }
    }
    
    return NULL; // Out of memory
}

mem_result_t pmm_free_frame(void *frame_addr) {
    uint64_t frame_num = (uint64_t)frame_addr / PAGE_SIZE;
    
    if (frame_num >= pmm.total_frames) {
        return MEM_ERROR_INVALID_ADDRESS;
    }
    
    uint32_t byte_index = frame_num / 8;
    uint8_t bit_index = frame_num % 8;
    
    if (!(pmm.frame_bitmap[byte_index] & (1 << bit_index))) {
        return MEM_ERROR_INVALID_ADDRESS; // Already free
    }
    
    // Mark frame as free
    pmm.frame_bitmap[byte_index] &= ~(1 << bit_index);
    pmm.free_frames++;
    pmm.used_frames--;
    
    return MEM_SUCCESS;
}

uint32_t pmm_get_free_frames(void) {
    return pmm.free_frames;
}

uint32_t pmm_get_total_frames(void) {
    return pmm.total_frames;
}

// Virtual memory management
mem_result_t vmm_init(void) {
    boot_log("Initializing virtual memory manager...");
    
    // Allocate PML4 table
    vmm.pml4_table = (pml4e_t*)pmm_alloc_frame();
    if (!vmm.pml4_table) {
        return MEM_ERROR_OUT_OF_MEMORY;
    }
    
    // Clear PML4 table
    memset(vmm.pml4_table, 0, PAGE_SIZE);
    
    // Initialize kernel heap
    kernel_heap.start = (void*)KERNEL_HEAP_START;
    kernel_heap.end = (void*)(KERNEL_HEAP_START + KERNEL_HEAP_SIZE);
    kernel_heap.current = kernel_heap.start;
    kernel_heap.total_size = KERNEL_HEAP_SIZE;
    kernel_heap.used_size = 0;
    kernel_heap.free_size = KERNEL_HEAP_SIZE;
    
    // Map kernel space
    // TODO: Map kernel code, data, and heap
    
    boot_log("Virtual memory manager initialized");
    return MEM_SUCCESS;
}

mem_result_t memory_map_page(void *virt_addr, void *phys_addr, uint32_t permissions) {
    // Extract indices from virtual address
    uint64_t pml4_index = ((uint64_t)virt_addr >> 39) & 0x1FF;
    uint64_t pdp_index = ((uint64_t)virt_addr >> 30) & 0x1FF;
    uint64_t pd_index = ((uint64_t)virt_addr >> 21) & 0x1FF;
    uint64_t pt_index = ((uint64_t)virt_addr >> 12) & 0x1FF;
    
    // Get PML4 entry
    pml4e_t *pml4_entry = &vmm.pml4_table[pml4_index];
    
    // Allocate PDPT if not present
    if (!pml4_entry->present) {
        pdpe_t *pdpt = (pdpe_t*)pmm_alloc_frame();
        if (!pdpt) {
            return MEM_ERROR_OUT_OF_MEMORY;
        }
        
        memset(pdpt, 0, PAGE_SIZE);
        
        pml4_entry->present = 1;
        pml4_entry->read_write = 1;
        pml4_entry->user = 0;  // Kernel space
        pml4_entry->frame = (uint64_t)pdpt >> PAGE_SHIFT;
    }
    
    // Get PDPT entry
    pdpe_t *pdpt = (pdpe_t*)((uint64_t)pml4_entry->frame << PAGE_SHIFT);
    pdpe_t *pdp_entry = &pdpt[pdp_index];
    
    // Allocate PD if not present
    if (!pdp_entry->present) {
        pde_t *pd = (pde_t*)pmm_alloc_frame();
        if (!pd) {
            return MEM_ERROR_OUT_OF_MEMORY;
        }
        
        memset(pd, 0, PAGE_SIZE);
        
        pdp_entry->present = 1;
        pdp_entry->read_write = 1;
        pdp_entry->user = 0;
        pdp_entry->frame = (uint64_t)pd >> PAGE_SHIFT;
    }
    
    // Get PD entry
    pde_t *pd = (pde_t*)((uint64_t)pdp_entry->frame << PAGE_SHIFT);
    pde_t *pd_entry = &pd[pd_index];
    
    // Allocate PT if not present
    if (!pd_entry->present) {
        pte_t *pt = (pte_t*)pmm_alloc_frame();
        if (!pt) {
            return MEM_ERROR_OUT_OF_MEMORY;
        }
        
        memset(pt, 0, PAGE_SIZE);
        
        pd_entry->present = 1;
        pd_entry->read_write = 1;
        pd_entry->user = 0;
        pd_entry->frame = (uint64_t)pt >> PAGE_SHIFT;
    }
    
    // Get PT entry
    pte_t *pt = (pte_t*)((uint64_t)pd_entry->frame << PAGE_SHIFT);
    pte_t *pt_entry = &pt[pt_index];
    
    // Set page table entry
    pt_entry->present = 1;
    pt_entry->read_write = (permissions & MEM_WRITE) ? 1 : 0;
    pt_entry->user = (permissions & MEM_USER) ? 1 : 0;
    pt_entry->nx = (permissions & MEM_EXECUTE) ? 0 : 1;
    pt_entry->frame = (uint64_t)phys_addr >> PAGE_SHIFT;
    
    // Invalidate TLB
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    
    return MEM_SUCCESS;
}

mem_result_t memory_unmap_page(void *virt_addr) {
    // Extract indices from virtual address
    uint64_t pml4_index = ((uint64_t)virt_addr >> 39) & 0x1FF;
    uint64_t pdp_index = ((uint64_t)virt_addr >> 30) & 0x1FF;
    uint64_t pd_index = ((uint64_t)virt_addr >> 21) & 0x1FF;
    uint64_t pt_index = ((uint64_t)virt_addr >> 12) & 0x1FF;
    
    // Navigate to page table entry
    pml4e_t *pml4_entry = &vmm.pml4_table[pml4_index];
    if (!pml4_entry->present) {
        return MEM_ERROR_INVALID_ADDRESS;
    }
    
    pdpe_t *pdpt = (pdpe_t*)((uint64_t)pml4_entry->frame << PAGE_SHIFT);
    pdpe_t *pdp_entry = &pdpt[pdp_index];
    if (!pdp_entry->present) {
        return MEM_ERROR_INVALID_ADDRESS;
    }
    
    pde_t *pd = (pde_t*)((uint64_t)pdp_entry->frame << PAGE_SHIFT);
    pde_t *pd_entry = &pd[pd_index];
    if (!pd_entry->present) {
        return MEM_ERROR_INVALID_ADDRESS;
    }
    
    pte_t *pt = (pte_t*)((uint64_t)pd_entry->frame << PAGE_SHIFT);
    pte_t *pt_entry = &pt[pt_index];
    if (!pt_entry->present) {
        return MEM_ERROR_INVALID_ADDRESS;
    }
    
    // Clear page table entry
    pt_entry->present = 0;
    pt_entry->frame = 0;
    
    // Invalidate TLB
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    
    return MEM_SUCCESS;
}

// Kernel heap implementation
mem_result_t kheap_init(void) {
    boot_log("Initializing kernel heap...");
    
    kernel_heap.start = (void*)KERNEL_HEAP_START;
    kernel_heap.end = (void*)(KERNEL_HEAP_START + KERNEL_HEAP_SIZE);
    kernel_heap.current = kernel_heap.start;
    kernel_heap.total_size = KERNEL_HEAP_SIZE;
    kernel_heap.used_size = 0;
    kernel_heap.free_size = KERNEL_HEAP_SIZE;
    
    boot_log("Kernel heap initialized");
    return MEM_SUCCESS;
}

void* kmalloc(size_t size) {
    // Align size to 8 bytes
    size = ALIGN_UP(size, 8);
    
    // Check if we have enough space
    if (kernel_heap.used_size + size > kernel_heap.total_size) {
        return NULL;
    }
    
    // Allocate memory
    void *ptr = kernel_heap.current;
    kernel_heap.current += size;
    kernel_heap.used_size += size;
    kernel_heap.free_size -= size;
    
    return ptr;
}

void kfree(void *ptr) {
    // Simple heap - no free for now
    // TODO: Implement proper heap with free
    (void)ptr;
}

void* krealloc(void *ptr, size_t new_size) {
    // Simple implementation - allocate new and copy
    void *new_ptr = kmalloc(new_size);
    if (new_ptr && ptr) {
        // TODO: Copy old data (need size tracking)
        // For now, just allocate new
    }
    return new_ptr;
}

// Memory utilities
bool is_aligned(void *ptr, size_t alignment) {
    return ((uint64_t)ptr & (alignment - 1)) == 0;
}

bool is_user_address(void *addr) {
    return (uint64_t)addr < KERNEL_BASE_ADDR;
}

bool is_kernel_address(void *addr) {
    return (uint64_t)addr >= KERNEL_BASE_ADDR;
}

void* align_up(void *ptr, size_t alignment) {
    return (void*)ALIGN_UP((uint64_t)ptr, alignment);
}

void* align_down(void *ptr, size_t alignment) {
    return (void*)ALIGN_DOWN((uint64_t)ptr, alignment);
}

// Main memory initialization
mem_result_t memory_init(void) {
    boot_log("Initializing memory management...");
    
    // Initialize physical memory manager
    // TODO: Get actual memory size from multiboot
    uint64_t total_memory = 128 * 1024 * 1024; // 128MB for testing
    mem_result_t result = pmm_init(total_memory);
    if (result != MEM_SUCCESS) {
        return result;
    }
    
    // Initialize virtual memory manager
    result = vmm_init();
    if (result != MEM_SUCCESS) {
        return result;
    }
    
    // Initialize kernel heap
    result = kheap_init();
    if (result != MEM_SUCCESS) {
        return result;
    }
    
    boot_log("Memory management initialization complete");
    return MEM_SUCCESS;
}
