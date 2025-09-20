.code64
.intel_syntax noprefix
.section .text

.extern gLastFaultRbp
.extern gLastFaultRsp
.extern kTestingPageFaults

.global divide_by_zero_handler
divide_by_zero_handler:
    cli
    mov rdi, [rsp]  # Get faulting RIP from the stack
    call handle_divide_by_zero
    hlt

.global invalid_opcode_handler
invalid_opcode_handler:
    cli
    mov rdi, [rsp]  # Get RIP
    call handle_invalid_opcode
    hlt

.global double_fault_handler
double_fault_handler:
    cli
    mov rdi, [rsp]  # Get RIP
    call handle_double_fault
    hlt

.global general_protection_fault_handler
general_protection_fault_handler:
    cli
    pop rdi   # Get error code
    pop rsi   # Get RIP (next on stack)
    call handle_general_protection_fault
    hlt

.global page_fault_handler
page_fault_handler:
    cli
    mov rax, rbp
    mov qword ptr [rip + gLastFaultRbp], rax
    mov rax, rsp
    mov qword ptr [rip + gLastFaultRsp], rax
    mov rsi, [rsp]      # Get error code
    mov rdx, [rsp + 8]  # Get RIP (next on stack)
    mov rax, cr2
    mov rdi, rax
    call handle_page_fault
    cmp byte ptr [rip + kTestingPageFaults], 0
    jne .page_fault_resume
    hlt

.page_fault_resume:
    add rsp, 8          # Discard error code before iretq
    iretq

.global machine_check_handler
machine_check_handler:
    cli
    mov rdi, [rsp]  # Get RIP
    call handle_machine_check
    hlt
