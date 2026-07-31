; NO_OS coro.s
; Minimal coroutine switch for kernel tasks (task 0 / REPL).
;
; coro_t layout (kernel/include/sched.h):
;   +0x00 rsp, +0x08 rbx, +0x10 rbp, +0x18 r12, +0x20 r13,
;   +0x28 r14, +0x30 r15

BITS 64

section .text

; int coro_save(coro_t *out)
; Saves the current kernel context and returns 0.
; When the saved context is restored via coro_restore, execution resumes
; right after the coro_save call with return value 1.
global coro_save
coro_save:
    mov [rdi + 0x00], rsp
    mov [rdi + 0x08], rbx
    mov [rdi + 0x10], rbp
    mov [rdi + 0x18], r12
    mov [rdi + 0x20], r13
    mov [rdi + 0x28], r14
    mov [rdi + 0x30], r15
    xor eax, eax
    ret

; void coro_restore(coro_t *in)
; Restores a previously saved context and returns (to the saved caller).
; Never returns to the current caller.
global coro_restore
coro_restore:
    mov rax, [rdi + 0x00]
    mov rsp, rax
    mov rax, [rdi + 0x08]
    mov rbx, rax
    mov rax, [rdi + 0x10]
    mov rbp, rax
    mov rax, [rdi + 0x18]
    mov r12, rax
    mov rax, [rdi + 0x20]
    mov r13, rax
    mov rax, [rdi + 0x28]
    mov r14, rax
    mov rax, [rdi + 0x30]
    mov r15, rax
    mov eax, 1
    ret

; void enter_user_mode(struct regs *frame)
; Switches to the stack given by frame (a struct regs, see isr.h) and
; iretq's it, entering ring 3. Never returns. Mirrors the isr_common_stub
; epilogue.
global enter_user_mode
enter_user_mode:
    mov rsp, rdi
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
    add rsp, 16
    iretq
