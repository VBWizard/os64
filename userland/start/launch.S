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
    call main

    // main returned: RAX = its int return value. Convert to exit(code).
    mov edi, eax                // exit code = main's return value
    mov eax, 2                  // SYSCALL_EXIT (abi/include/os64/syscall_numbers.h)
    syscall

1:  jmp 1b                      // exit never returns; spin if it somehow does
.size _start, .-_start

// Mark the stack non-executable and silence the linker's default-exec-stack
// warning (os64 never runs code off the user stack).
.section .note.GNU-stack, "", @progbits
