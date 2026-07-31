ENTRY(_start)

SECTIONS
{
    /* Load the kernel at 1 MiB (conventional for multiboot). */
    . = 1M;
    _kernel_start = .;

    .multiboot : ALIGN(4) {
        KEEP(*(.multiboot))
    }

    .text : ALIGN(4K) {
        *(.text*)
    }

    .rodata : ALIGN(4K) {
        *(.rodata*)
    }

    .data : ALIGN(4K) {
        *(.data*)
    }

    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss*)
    }

    _kernel_end = .;
}
