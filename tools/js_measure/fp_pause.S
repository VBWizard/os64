/* Keep register sentinels live across an existing yield/sleep syscall.
 * Save and restore the caller's floating-point state around the probe. */
#include "os64/syscall_numbers.h"
.intel_syntax noprefix
.text
.global js_measure_fp_pause
.type js_measure_fp_pause, @function
js_measure_fp_pause:
    push rbp
    mov rbp, rsp
    sub rsp, 1552
    fxsave64 [rsp]
    mov [rsp + 1544], esi
    fninit
    mov ax, [rsp]
    and ax, 0xf3ff
    mov ecx, edi
    inc ecx
    shl ecx, 10
    or ax, cx
    mov [rsp + 1536], ax
    fldcw [rsp + 1536]
    fld1
    fldpi
    mov eax, [rsp + 24]
    and eax, 0xffff9fff
    mov ecx, edi
    inc ecx
    shl ecx, 13
    or eax, ecx
    mov [rsp + 1540], eax
    ldmxcsr [rsp + 1540]
    lea r10, [rip + fp_patterns]
    shl edi, 8
    add r10, rdi
    movdqa xmm0, [r10 + 0]
    movdqa xmm1, [r10 + 16]
    movdqa xmm2, [r10 + 32]
    movdqa xmm3, [r10 + 48]
    movdqa xmm4, [r10 + 64]
    movdqa xmm5, [r10 + 80]
    movdqa xmm6, [r10 + 96]
    movdqa xmm7, [r10 + 112]
    movdqa xmm8, [r10 + 128]
    movdqa xmm9, [r10 + 144]
    movdqa xmm10, [r10 + 160]
    movdqa xmm11, [r10 + 176]
    movdqa xmm12, [r10 + 192]
    movdqa xmm13, [r10 + 208]
    movdqa xmm14, [r10 + 224]
    movdqa xmm15, [r10 + 240]
    fxsave64 [rsp + 512]
    mov eax, SYSCALL_YIELD
    test dword ptr [rsp + 1544], 1
    jz .Lpause
    mov eax, SYSCALL_SLEEP
    mov edi, 1
.Lpause:
    syscall
    test dword ptr [rsp + 1544], 2
    jz .Lcapture
    /* Disturb each compared state class to prove the check rejects it. */
    fchs
    pcmpeqd xmm15, xmm15
    xor dword ptr [rsp + 1540], 0x2000
    ldmxcsr [rsp + 1540]
.Lcapture:
    fxsave64 [rsp + 1024]
    xor r9d, r9d
    cmp rax, 0
    jge .Lcontrols
    or r9d, 8
.Lcontrols:
    mov eax, [rsp + 512]
    cmp eax, [rsp + 1024]
    jne .Lcontrol_bad
    mov al, [rsp + 516]
    cmp al, [rsp + 1028]
    jne .Lcontrol_bad
    mov eax, [rsp + 536]
    cmp eax, [rsp + 1048]
    je .Lst
.Lcontrol_bad:
    or r9d, 1
.Lst:
    xor ecx, ecx
.Lst_loop:
    mov rax, [rsp + rcx + 544]
    cmp rax, [rsp + rcx + 1056]
    jne .Lst_bad
    mov ax, [rsp + rcx + 552]
    cmp ax, [rsp + rcx + 1064]
    je .Lst_next
.Lst_bad:
    or r9d, 2
.Lst_next:
    add ecx, 16
    cmp ecx, 32
    jne .Lst_loop
    xor ecx, ecx
.Lxmm_loop:
    mov rax, [rsp + rcx + 672]
    cmp rax, [rsp + rcx + 1184]
    je .Lxmm_next
    or r9d, 4
.Lxmm_next:
    add ecx, 8
    cmp ecx, 256
    jne .Lxmm_loop
    fxrstor64 [rsp]
    mov eax, r9d
    leave
    ret
.size js_measure_fp_pause, .-js_measure_fp_pause
.section .rodata
.balign 16
fp_patterns:
    .quad 0x3ff0000000000000, 0x4000000000000000
    .quad 0x3ff0000000000001, 0x4000000000000001
    .quad 0x3ff0000000000002, 0x4000000000000002
    .quad 0x3ff0000000000003, 0x4000000000000003
    .quad 0x3ff0000000000004, 0x4000000000000004
    .quad 0x3ff0000000000005, 0x4000000000000005
    .quad 0x3ff0000000000006, 0x4000000000000006
    .quad 0x3ff0000000000007, 0x4000000000000007
    .quad 0x3ff0000000000008, 0x4000000000000008
    .quad 0x3ff0000000000009, 0x4000000000000009
    .quad 0x3ff000000000000a, 0x400000000000000a
    .quad 0x3ff000000000000b, 0x400000000000000b
    .quad 0x3ff000000000000c, 0x400000000000000c
    .quad 0x3ff000000000000d, 0x400000000000000d
    .quad 0x3ff000000000000e, 0x400000000000000e
    .quad 0x3ff000000000000f, 0x400000000000000f
    .quad 0x4000000000000000, 0x4010000000000000
    .quad 0x4000000000000001, 0x4010000000000001
    .quad 0x4000000000000002, 0x4010000000000002
    .quad 0x4000000000000003, 0x4010000000000003
    .quad 0x4000000000000004, 0x4010000000000004
    .quad 0x4000000000000005, 0x4010000000000005
    .quad 0x4000000000000006, 0x4010000000000006
    .quad 0x4000000000000007, 0x4010000000000007
    .quad 0x4000000000000008, 0x4010000000000008
    .quad 0x4000000000000009, 0x4010000000000009
    .quad 0x400000000000000a, 0x401000000000000a
    .quad 0x400000000000000b, 0x401000000000000b
    .quad 0x400000000000000c, 0x401000000000000c
    .quad 0x400000000000000d, 0x401000000000000d
    .quad 0x400000000000000e, 0x401000000000000e
    .quad 0x400000000000000f, 0x401000000000000f
.section .note.GNU-stack, "", @progbits
