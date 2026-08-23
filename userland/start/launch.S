// launch.S — the os64 userland startup stub (the crt0-equivalent, named
// `launch` per the userland roadmap — NOT crt0; we don't inherit MS/Unix
// jargon where a clearer name exists).
//
// The kernel enters a ring-3 ELF at its entry point on a fresh user stack.
// launch is that entry point: it hands control to main(), then converts
// main's return value into an explicit exit() syscall so a plain
// `return 0;` from main terminates the task cleanly. (The kernel's ring-3
// exit trampoline is a pure-ABI backstop for binaries with no launch; a
// libos64 program always comes through here.)
//
// argc/argv/envp handoff: the KERNEL already sets it up in registers before
// entering us — task_create latches RDI=argc, RSI=argv (@TASK_ARGV_VIRT),
// RDX=envp (@TASK_ENV_VIRT) into the new thread's saved regs, which is
// exactly the SysV register order for main(argc, argv, envp). So launch does
// NOTHING but preserve them and call main. (An earlier version zeroed
// RDI/RSI/RDX here and threw the whole handoff away — hence hello's fake
// empty args. The contract is register-based, not stack-based; the initial
// user stack carries only the return address to the exit trampoline.)

.code64
.intel_syntax noprefix

.extern main
.extern os64_runtime_init
.global _start
.type _start, @function

// Put _start in its own section so the app link script (*(.text._start)
// first) places it at the load base — the ELF entry sits at 0x400000, the
// conventional and debugger-friendly layout. Without this, _start lands in
// plain .text and whatever object links first (the app's main) takes the
// base instead. Correct either way (ENTRY(_start) sets e_entry), but this
// is the one you want when you attach a debugger.
.section .text._start, "ax", @progbits
_start:
    xor rbp, rbp                // terminate the frame-pointer chain (ABI: the
                                // outermost frame has rbp = 0)

    // THE ALIGNMENT DEBT, PAID (PR #26 round four — Codex, reading the ABI
    // more carefully than the ABI's own comment below did). The kernel
    // enters _start with rsp%16 == 8 (thread.c: stack top minus the
    // traditional 6-qword margin minus the seeded return slot) — a
    // deliberate POST-CALL shape, so the exit-trampoline address at [rsp]
    // behaves like a caller's return address. But _start then CALLS into C,
    // and SysV wants each call SITE at rsp%16 == 0 so the callee lands at
    // %16 == 8. From a %8 start, every call out of here entered its callee
    // one qword out of phase — since the first app ever ran, masked only by
    // -mno-sse (no movaps to fault). One qword restores the phase; the
    // seeded slot stays untouched above us, and launch never `ret`s — main's
    // return becomes os64_exit below.
    sub rsp, 8

    // RDI/RSI/RDX arrive holding argc/argv/envp (kernel-set — see above),
    // which ARE main's first three SysV arguments. Pass them through
    // untouched.

    // Stand the LIBRARY up before the PROGRAM runs (os64/runtime.h): publish
    // the environment block for os64_getenv, and let the heap register its
    // report with the kernel so /proc/<pid>/heap is honest from this
    // instruction onward.
    //
    // THE ENVIRONMENT IS PASSED, NOT POKED. This used to be `lea rax, [rip +
    // __os64_env]; mov [rax], rdx` — launch reaching directly into a libos64
    // global. That worked only while libos64 was statically linked into the
    // same binary as launch; with libos64 a shared object (2026-08-22), the
    // variable lives in the LIBRARY and this code lives in the EXECUTABLE, and
    // an executable that references a library's VARIABLE drags in an
    // R_X86_64_COPY relocation — the one genuinely nasty corner of non-PIE
    // dynamic linking. Handing the pointer to os64_runtime_init as an ordinary
    // argument avoids the entire problem, and libos64 now exports functions
    // and no variables at all (see env.c, LIBOS64.md).
    //
    // main's three arguments have to survive the call, and RDI/RSI/RDX are
    // caller-saved — so park them in RBX/R12/R13, which the SysV ABI makes
    // the CALLEE's job to preserve. (Registers rather than pushes so the
    // alignment established above stays established — three pushes would
    // flip the phase and hand main a misaligned frame all over again.)
    mov rbx, rdi
    mov r12, rsi
    mov r13, rdx
    mov rdi, rdx                // envp — os64_runtime_init's only argument
    call os64_runtime_init
    mov rdi, rbx
    mov rsi, r12
    mov rdx, r13

    call main

    // main returned: RAX = its int return value. Convert to exit(code).
    mov edi, eax                // exit code = main's return value
    mov eax, 2                  // SYSCALL_EXIT (abi/include/os64/syscall_numbers.h)
    syscall

1:  jmp 1b                      // exit never returns; spin if it somehow does
.size _start, .-_start


// (os64_thread_trampoline used to live here. It moved to
// libos64/thread_trampoline.S on 2026-08-22 — it is libos64's own thread
// machinery, and a shared library cannot depend on a symbol defined in the
// executable that loaded it. That file explains the whole reason.)

// Mark the stack non-executable and silence the linker's default-exec-stack
// warning (os64 never runs code off the user stack).
.section .note.GNU-stack, "", @progbits
