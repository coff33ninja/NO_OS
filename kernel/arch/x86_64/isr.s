; NO_OS isr.s
; Interrupt service routine stubs for vectors 0..47.
; Each stub pushes (error code, vector) then jumps to isr_common_stub, which
; saves the GPRs and calls the C dispatcher isr_dispatch(vector, error, regs).

BITS 64

section .text

extern isr_dispatch

; ------------------------------------------------------------------
; Stubs: vectors 0..47 (0..31 exceptions, 32..47 hardware IRQs).
; Exceptions with a CPU-pushed error code: 8, 10, 11, 12, 13, 14, 17, 21.
; ------------------------------------------------------------------
%macro ISR_STUB 1
    %if %1 == 8 || %1 == 10 || %1 == 11 || %1 == 12 || %1 == 13 || %1 == 14 || %1 == 17 || %1 == 21
        push qword %1
    %else
        push qword 0
        push qword %1
    %endif
    jmp isr_common_stub
%endmacro

%assign i 0
%rep 48
global isr%+i
isr%+i:
    ISR_STUB i
%assign i i+1
%endrep

; ------------------------------------------------------------------
; Syscall gate (int 0x80), callable from ring 3.
; The CPU pushes ss, rsp, rflags, cs, rip (ring change), then we push the
; dummy error code and vector like every other stub.
; ------------------------------------------------------------------
global isr80
isr80:
    push qword 0
    push qword 0x80
    jmp isr_common_stub

; ------------------------------------------------------------------
; Stub address table for idt_init.
; ------------------------------------------------------------------
section .data
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 48
    dq isr%+i
%assign i i+1
%endrep

; ------------------------------------------------------------------
; Common handler.
;
; Stack layout at entry (top = lowest address):
;   [rsp+0..112]   r15..rax (15 saved GPRs)
;   [rsp+120]      vector
;   [rsp+128]      error
;   [rsp+136]      rip, cs, rflags [, rsp, ss]
; ------------------------------------------------------------------
section .text
isr_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rbp, rsp
    and rsp, -16            ; realign for the C ABI
    mov rdi, [rbp + 120]    ; vector
    mov rsi, [rbp + 128]    ; error
    mov rdx, rbp            ; regs
    call isr_dispatch
    mov rsp, rbp

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16             ; discard vector + error
    iretq
