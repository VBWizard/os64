.code64
.intel_syntax noprefix
.section .text

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
    pop rdi   # Get error code
    pop rsi   # Get RIP (next on stack)
    call handle_page_fault
    hlt

.global machine_check_handler
machine_check_handler:
    cli
    mov rdi, [rsp]  # Get RIP
    call handle_machine_check
    hlt

