OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS {
    . = 0x0000010000000000;

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        *(.lbss*)
        _bss_end = .;
    }

    /* Trailing PROGBITS marker so objcopy -O binary spans the .bss/.lbss
       range (zero-filled), letting the kernel map the full user image. */
    .userhdr : {
        BYTE(0)
    }
}
