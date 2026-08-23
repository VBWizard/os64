// thread_trampoline.S — where a thread's function RETURNS to.
//
// MOVED HERE FROM start/launch.S ON 2026-08-22, the day libos64 became a
// shared object. It was never really launch's business: launch is the
// EXECUTABLE's entry stub, while this trampoline is the other end of
// libos64's own os64_thread_create() — thread.c passes its address to the
// kernel, so the library depended on a symbol defined in the program that
// linked it. Statically linked that was merely untidy; dynamically linked it
// is impossible, because a shared object's relocations resolve against ITSELF
// AND ITS OWN DEPENDENCIES (kernel/src/shared_object.c's scoped resolver) and
// deliberately never against the executable that loaded it. The library must
// carry everything the library needs. Moving one function fixed it; the
// alternative would have been teaching the loader to search backwards into
// the executable, which is exactly the fault-order-dependent scoping the
// kernel's resolver was designed to refuse.
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

.code64
.intel_syntax noprefix

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
