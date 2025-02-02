.code64
.section .text
.intel_syntax noprefix  # Use Intel syntax

.extern syscall

# 64-bit SYSCALL
# NOTE: RCX is clobbered by SYSCALL for the return address to be used by SYSRET
.globl syscall_Enter
.type syscall_Enter, @function
syscall_Enter:                #WARNING: change libChrisOS linker file if you change this from 0x130000
    cli
    push rcx
    push rdx
    push rbp
    mov rbp,rsp
    call syscall_Return #NOTE: sysCallReturn only exists to put a return address on the stack to be put in the
                        #EDX register before the sysExit
    add rsp,8
    pop rbp
    pop rdx
    pop rcx
    sti
    ret
syscall_Return:
    mov ebp,esp         #ebp will be the ESP (ECX) point to the return address, on sysExit
    syscall

.globl _sysCall
.type _sysCall, @function
_sysCall:
    cli                         # Disable interrupts

    # Save registers, including RCX (return address)
    push rcx                     # **SAVE RCX (Return Address)** <--- IMPORTANT
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov rbp, rsp
    #    - System call number in RAX
    #    - Arguments in RDI, RSI, RDX, R10, R8, R9
	call _syscall
	#    - Call result is in rax
    mov rsp, rbp
    # Restore registers (in reverse order, including RCX)
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop rcx                     # **RESTORE RCX (Return Address)**
    sti                         # Re-enable interrupts
    sysretq                      # Return to user space
    ret
