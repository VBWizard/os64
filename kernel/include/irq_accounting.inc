# The caller has saved RBX and its other C caller-saved registers. Offset
# names the CPU frame's CS from that saved RSP. Align for a ring-0 interrupt
# that may have interrupted C at any stack alignment, then restore the
# original stack before the handler touches its own frame.
.extern accounting_interrupt_enter
.extern accounting_interrupt_exit

.macro IRQ_ACCOUNT_ENTER cs_offset
    mov rbx, rsp
    mov rdi, [rbx + \cs_offset]
    and rsp, -16
    call accounting_interrupt_enter
    mov rsp, rbx
.endm

.macro IRQ_ACCOUNT_EXIT
    mov rbx, rsp
    and rsp, -16
    call accounting_interrupt_exit
    mov rsp, rbx
.endm
