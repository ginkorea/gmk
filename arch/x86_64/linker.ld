/* GMK/cpu — Higher-half kernel linker script for x86_64 + Limine */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_kstart)

KERNEL_VIRT_BASE = 0xffffffff80000000;

PHDRS {
    text   PT_LOAD FLAGS(5); /* r-x */
    rodata PT_LOAD FLAGS(4); /* r-- */
    data   PT_LOAD FLAGS(6); /* rw- */
}

SECTIONS {
    . = KERNEL_VIRT_BASE;

    /* Limine request structs — must be in the first 4GB of the binary image */
    .limine_reqs : {
        KEEP(*(.limine_reqs))
    } :text

    .text : ALIGN(4K) {
        *(.text .text.*)
    } :text

    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
    } :rodata

    .data : ALIGN(4K) {
        *(.data .data.*)
    } :data

    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}
