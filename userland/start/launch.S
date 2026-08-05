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
.extern __os64_env
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

    // RDI/RSI/RDX arrive holding argc/argv/envp (kernel-set — see above),
    // which ARE main's first three SysV arguments. Pass them through
    // untouched.

    // But first: publish the env block pointer for os64_getenv (libos64
    // env.c) — the environment is process-global state, and stashing it here
    // means no program ever has to thread envp through its own call tree
    // just so a leaf function can ask for PATH. RAX is free (no meaning at
    // entry) and RDI/RSI/RDX stay untouched for main.
    lea rax, [rip + __os64_env]
    mov [rax], rdx

    call main

    // main returned: RAX = its int return value. Convert to exit(code).
    mov edi, eax                // exit code = main's return value
    mov eax, 2                  // SYSCALL_EXIT (abi/include/os64/syscall_numbers.h)
    syscall

1:  jmp 1b                      // exit never returns; spin if it somehow does
.size _start, .-_start


// ── os64_thread_trampoline — where a thread's function RETURNS to ──────────
//
// The kernel seeds this address as the return address on a new thread's
// stack (SYSCALL_THREAD's third argument), so a thread function that
// simply returns lands here with its value in RAX — exactly as if it had
// been `call`ed, because from the CPU's point of view it was.
//
// It must be assembly for one reason: RAX. C has no way to say "whatever
// the previous function returned"; by the time a C function's prologue
// runs, RAX belongs to it. Six instructions of asm is cheaper than
// inventing a calling convention.
//
// It also must live in USERLAND — the kernel cannot seed one of its own
// addresses here, because ring 3 would fault the instant it tried to
// return into the kernel half of the address space.
.global os64_thread_trampoline
.type os64_thread_trampoline, @function
os64_thread_trampoline:
    mov rdi, rax                // the thread function's return value
    mov eax, 34                 // SYSCALL_THREAD_EXIT (os64/syscall_numbers.h)
    syscall

1:  jmp 1b                      // thread_exit never returns; spin if it does
.size os64_thread_trampoline, .-os64_thread_trampoline

// Mark the stack non-executable and silence the linker's default-exec-stack
// warning (os64 never runs code off the user stack).
.section .note.GNU-stack, "", @progbits
