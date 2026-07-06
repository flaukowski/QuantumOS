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
    _end = .;

    /* The kernel's ELF loader does no image/stack/args overlap check, so the
     * total user image (text+rodata+data+bss+libq heap arena) must be bounded
     * here. 0x40080000 leaves >=64 KiB below the read-only args page mapped at
     * USER_ARGS_VADDR (0x40090000); the 16 KiB stack lives above that from
     * 0x400FC000 to 0x40100000. Overflowing this collides a large arena with
     * the args page. */
    ASSERT(_end <= 0x40080000,
        "libq: user image overflows past 0x40080000 toward the args page (0x40090000)")

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note.*)
    }
}
