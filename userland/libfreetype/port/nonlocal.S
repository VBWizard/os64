// nonlocal.S — save and restore an execution point, for the character-map
// validator's escape from a malformed table.
//
// runtime.h carries the CONTRACT — what is saved, what is not, what the
// caller owes, and why this is private rather than a public setjmp. What is
// here is the mechanism.
//
// One body serves two toolchains: x86_64-elf-gcc for os64 and the host cc
// for tools/test_freetype_host, because both are System V x86-64 and the
// register set is the ABI's, not the platform's. That is the point — the
// host sanitizer run exercises the jump that ships, not a stand-in.
//
// Intel syntax, like every other piece of assembly in this tree.

.intel_syntax noprefix
.text

// ftport_jmp_buf layout, all eight slots written and read here and nowhere
// else:
//   +0x00 rbx   +0x08 rbp   +0x10 r12   +0x18 r13
//   +0x20 r14   +0x28 r15   +0x30 rsp (as it will be after we return)
//   +0x38 the return address

// int ftport_setjmp( ftport_jmp_buf buf )   -- buf in rdi
.globl ftport_setjmp
.hidden ftport_setjmp
.type ftport_setjmp, @function
ftport_setjmp:
    mov     [rdi + 0x00], rbx
    mov     [rdi + 0x08], rbp
    mov     [rdi + 0x10], r12
    mov     [rdi + 0x18], r13
    mov     [rdi + 0x20], r14
    mov     [rdi + 0x28], r15
    // The stack pointer worth saving is the caller's, not ours: rsp+8 is
    // where it lands once our own `ret` has consumed the return address.
    lea     rax, [rsp + 8]
    mov     [rdi + 0x30], rax
    mov     rax, [rsp]
    mov     [rdi + 0x38], rax
    xor     eax, eax                // a direct call answers 0
    ret
.size ftport_setjmp, . - ftport_setjmp

// void ftport_longjmp_raw( ftport_jmp_buf buf, int value )  -- buf rdi, value esi
//
// Not called directly: runtime.c's ftport_longjmp wraps it so an instrumented
// build can tell the sanitizer that these frames are being abandoned.
.globl ftport_longjmp_raw
.hidden ftport_longjmp_raw
.type ftport_longjmp_raw, @function
ftport_longjmp_raw:
    mov     eax, esi
    test    eax, eax
    jnz     1f
    mov     eax, 1                  // C's rule: 0 becomes 1, so the arrival
1:                                  // is always distinguishable from the call
    mov     rbx, [rdi + 0x00]
    mov     rbp, [rdi + 0x08]
    mov     r12, [rdi + 0x10]
    mov     r13, [rdi + 0x18]
    mov     r14, [rdi + 0x20]
    mov     r15, [rdi + 0x28]
    // Read the destination BEFORE moving rsp: after the move, this frame's
    // memory is no longer ours to read.
    mov     rdx, [rdi + 0x38]
    mov     rsp, [rdi + 0x30]
    jmp     rdx
.size ftport_longjmp_raw, . - ftport_longjmp_raw

// os64's loader derives page permissions from segment flags and has no use
// for this note, but the host link warns once per object without it.
.section .note.GNU-stack,"",@progbits
