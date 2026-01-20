# QuantumOS Kernel Linker Script

ENTRY(_start)

/* Define memory regions */
MEMORY
{
    kernel (rwx) : ORIGIN = 0x00100000, LENGTH = 96M   /* Kernel loaded at 1MB, includes BSS */
    ram (rwx)    : ORIGIN = 0x06100000, LENGTH = 32M   /* Rest of low memory */
}

/* Sections */
SECTIONS
{
    /* Kernel starts at 1MB */
    . = 0x00100000;
    
    /* Multiboot header must be in first 8KB */
    .multiboot :
    {
        KEEP(*(.multiboot_header))
        . = ALIGN(4096);
    } > kernel
    
    /* Code section */
    .text :
    {
        *(.text .text.*)
        *(.gnu.linkonce.t.*)
        . = ALIGN(4096);
    } > kernel
    
    /* Read-only data */
    .rodata :
    {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
        . = ALIGN(4096);
    } > kernel
    
    /* Data section */
    .data :
    {
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(4096);
    } > kernel
    
    /* BSS section */
    .bss :
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        __bss_end = .;
        . = ALIGN(4096);
    } > kernel
    
    /* Symbol for end of kernel */
    __end = .;
    
    /* Stack section */
    .stack (NOLOAD) :
    {
        . = . + 8192;  /* 8KB stack */
        __stack_top = .;
    } > ram
    
    /* Heap section */
    .heap (NOLOAD) :
    {
        __heap_start = .;
        . = . + 64M;   /* 64MB heap */
        __heap_end = .;
    } > ram
    
    /* Discard sections we don't need */
    /DISCARD/ :
    {
        *(.eh_frame)
        *(.comment)
        *(.note.gnu.build-id)
        *(.note.GNU-stack)
        *(.gnu_debuglink)
        *(.gnu_debugdata)
    }
}

/* Symbols for C code */
__kernel_start = 0x00100000;
__kernel_size = __end - __kernel_start;
__kernel_end = __end;

/* Stack symbols */
__stack_bottom = __stack_top - 8192;
__stack_pointer = __stack_top;

/* Heap symbols */
__heap_pointer = __heap_start;

/* Page table symbols */
__page_tables_start = __heap_end;
__page_tables_end = __page_tables_start + 1M;  /* 1MB for page tables */

/* Frame buffer symbols (if needed) */
__framebuffer_start = 0xB8000;
__framebuffer_end = 0xB8FA0;
