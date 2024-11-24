.macro SAVE_ALL
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
    pushfq
.endm

.macro RESTORE_ALL
    popfq
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
.endm

.macro IRQ_PROLOGUE
    sub rsp, 8               # Simulate interrupt frame
    and rsp, -16             # Align stack to 16 bytes
    SAVE_ALL            # Save registers
.endm

.macro IRQ_EPILOGUE
    RESTORE_ALL             # Restore registers
    add rsp, 8               # Restore stack alignment
.endm
