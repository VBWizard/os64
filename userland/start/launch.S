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
// argc/argv/envp handoff is NOT wired yet (the initial-stack-layout ABI is
// unspecified — see ABI.md's open "startup handoff" item). main() is called
// with zeroed args for now; hello doesn't need them. When the layout is
// designed, this stub reads it and nothing else changes.

.code64
.intel_syntax noprefix

.extern main
.global _start
.type _start, @function

_start:
    xor rbp, rbp                // terminate the frame-pointer chain (ABI: the
                                // outermost frame has rbp = 0)

    // main(argc=0, argv=NULL, envp=NULL) until the handoff ABI exists.
    xor edi, edi
    xor esi, esi
    xor edx, edx
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
