.intel_syntax noprefix
.section .text
.global _start

_start:
    // Page 1 of TEXT (0x400000) — faulted in when execution begins.
    // Prints "P1 " to serial to confirm page 1 ran, then calls into page 2.
    mov dx, 0x3f8
    mov al, 'P'
    out dx, al
    mov al, 'A'
    out dx, al
    mov al, 'G'
    out dx, al
    mov al, 'E'
    out dx, al
    mov al, '1'
    out dx, al
    mov al, ' '
    out dx, al

    call page2_func         // cross-page call — triggers a second demand-page fault

    // RAX = 0xE1F0CA11 on return; task_exit_with_retval captures it
    ret

    // Pad the rest of page 1 with NOPs so page2_func lands at exactly 0x401000.
    .balign 4096

page2_func:
    // Page 2 of TEXT (0x401000) — demand-paged on the cross-page call above.
    // If the vma->loaded-per-VMA bug were reintroduced, this page fault would
    // panic instead of mapping the page, so this function would never run.
    mov dx, 0x3f8
    mov al, 'P'
    out dx, al
    mov al, 'A'
    out dx, al
    mov al, 'G'
    out dx, al
    mov al, 'E'
    out dx, al
    mov al, '2'
    out dx, al
    mov al, '\n'
    out dx, al

    mov rax, 0xE1F0CA11     // return value captured by task_exit_with_retval
exit:
    ret
