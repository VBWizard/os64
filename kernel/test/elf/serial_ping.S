.intel_syntax noprefix
.section .text
.global _start

_start:
    mov dx, 0x3f8
    mov al, 'E'
    out dx, al
    mov al, 'L'
    out dx, al
    mov al, 'F'
    out dx, al
    mov al, ' '
    out dx, al
    mov al, 'O'
    out dx, al
    mov al, 'K'
    out dx, al
    mov al, '\n'
    out dx, al

    mov rax, 0xE1F0CA11     // return value captured by task_exit_with_retval
    ret
