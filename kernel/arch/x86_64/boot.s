; NO_OS boot.s
; Multiboot entry -> 64-bit long mode -> kmain(mb_info)
;
; The Multiboot loader (QEMU -kernel, GRUB, etc.) starts us in 32-bit
; protected mode with:
;   EAX = 0x2BADB002 (multiboot magic)
;   EBX = physical address of the multiboot info structure

BITS 32

; ------------------------------------------------------------------
; Multiboot header (must be in the first 8 KiB of the image)
; ------------------------------------------------------------------
section .multiboot
align 4
    dd 0x1BADB002           ; magic
    dd 0x00000000           ; flags
    dd -(0x1BADB002)        ; checksum (magic + flags + checksum == 0)

; ------------------------------------------------------------------
; .bss -- boot stack and initial page tables
; ------------------------------------------------------------------
section .bss
align 16
stack_bottom:
    resb 16384              ; 16 KiB boot stack
stack_top:

align 4096
pml4:
    resb 4096
pdpt:
    resb 4096
pd0:
    resb 4096

saved_mb_info:
    resd 1                  ; multiboot info pointer, passed to kmain

; ------------------------------------------------------------------
; Entry point (32-bit protected mode)
; ------------------------------------------------------------------
section .text
global _start
extern kmain
_start:
    mov esp, stack_top
    mov [saved_mb_info], ebx

    ; --- Identity-map the first 1 GiB with 2 MiB pages ---
    ; PML4[0] -> PDPT
    mov eax, pdpt
    or  eax, 0x3            ; present | writable
    mov [pml4], eax

    ; PDPT[0] -> PD0
    mov eax, pd0
    or  eax, 0x3
    mov [pdpt], eax

    ; PD0[0..511] -> 512 x 2 MiB pages (present | writable | huge)
    xor ecx, ecx
    xor eax, eax
.fill_pd:
    mov edx, eax
    or  edx, 0x83
    mov [pd0 + ecx*8], edx
    add eax, 0x200000
    inc ecx
    cmp ecx, 512
    jne .fill_pd

    ; --- Enable PAE (CR4.PAE = bit 5) ---
    mov eax, cr4
    or  eax, 1 << 5
    mov cr4, eax

    ; --- Load page table root ---
    mov eax, pml4
    mov cr3, eax

    ; --- Enable long mode: EFER.LME (MSR 0xC0000080, bit 8) ---
    mov ecx, 0xC0000080
    rdmsr
    or  eax, 1 << 8
    wrmsr

    ; --- Enable paging (CR0.PG = bit 31) ---
    mov eax, cr0
    or  eax, 1 << 31
    mov cr0, eax

    ; --- Load a 64-bit GDT and far-jump into long mode ---
    lgdt [gdt_ptr]
    jmp 0x08:long_mode_start

; ------------------------------------------------------------------
; 64-bit GDT (built before the far jump)
; ------------------------------------------------------------------
section .data
align 8
gdt:
    dq 0x0000000000000000   ; null descriptor
    dq 0x00AF9A000000FFFF   ; 64-bit code, DPL 0
    dq 0x00CF92000000FFFF   ; data, DPL 0
gdt_ptr:
    dw $ - gdt - 1          ; limit
    dd gdt                  ; 32-bit base (kernel is linked below 4 GiB)

; ------------------------------------------------------------------
; Long mode entry point
; ------------------------------------------------------------------
BITS 64
section .text
long_mode_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; First argument to kmain: multiboot info pointer (zero-extended)
    mov edi, [saved_mb_info]
    call kmain

.hang:
    cli
    hlt
    jmp .hang
