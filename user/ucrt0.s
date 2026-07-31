; NO_OS user runtime crt0 (ring 3).
; Zeroes .bss, calls nocproc_main(), then exits via int 0x80.

BITS 64

section .text
global _start
extern nocproc_main
extern noc_os_exit
extern _bss_start
extern _bss_end

_start:
    ; Zero .bss
    mov rdi, _bss_start
    mov rcx, _bss_end
    sub rcx, rdi
    xor eax, eax
    rep stosb

    call nocproc_main

    ; process exit(code) -> int 0x80 SYS_EXIT
    mov edi, eax
    call noc_os_exit

.hang:
    hlt
    jmp .hang
