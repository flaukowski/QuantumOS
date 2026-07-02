/* QuantumOS user program linker script.
 *
 * Fixed load address at USER_VBASE (0x40000000); the kernel's ELF
 * loader maps each PT_LOAD segment at its p_vaddr into a private
 * address space. Segments are page-aligned so the loader never has to
 * split a page between two segments.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text : {
        *(.text._start)
        *(.text .text.*)
    }
    . = ALIGN(4096);

    .rodata : {
        *(.rodata .rodata.*)
    }
    . = ALIGN(4096);

    .data : {
        *(.data .data.*)
    }
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(4096);

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note.*)
    }
}
