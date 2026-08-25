#include "asm-offsets.h"
#include "syscall_numbers.h"

.code64
.intel_syntax noprefix

.extern task_exit
.extern kKernelPML4

// Called when a task's thread returns via 'ret'.  The x86-64 ABI leaves the
// return value in RAX.  We walk CLS -> currentThread -> ownerTask and store
// RAX directly into task->retVal before any C function call can clobber it,
// then fall through into the normal task_exit path.
.globl task_exit_with_retval
.type  task_exit_with_retval, @function
task_exit_with_retval:
    cli
    mov  rcx, rax                                   // rcx = ELF's return value
    mov  rax, gs:[0]                                // rax = CLS self-pointer
    mov  rax, [rax + CLS_CURRENTTHREAD_OFFSET]      // rax = currentThread
    mov  rax, [rax + THREAD_OWNERTASK_OFFSET]       // rax = ownerTask (task_t*)
    mov  [rax + TASK_RETVAL_OFFSET], rcx            // task->retVal = return value
    sti
    jmp  task_exit

// void call_in_kernel_context(void (*func)(void*), void *arg)
//
// Run func(arg) in kernel context — on the core's kernel interrupt stack with
// kKernelPML4 loaded — then restore the caller's (task) CR3/RSP and return.
//
// This is a NAKED trampoline on purpose. The equivalent C ran the switch in a
// normal frame and then touched C locals whose rbp was still the (now-unmapped)
// task stack — UB masked only by the -O0 layout (see the CLAUDE.md
// "Context Switching" notes). Here nothing is stored to a stack local across a
// switch: task CR3/RSP and func/arg live in callee-saved registers (which func
// preserves per the SysV ABI), and the ONE piece of per-core state we need,
// kernel_interrupt_stack_top, is read from CLS via GS (valid under any CR3/RSP).
//
// IT IS ALSO IRQSAVE, AND THAT IS LOAD-BEARING (2026-08-20, PR #27 round 9).
// The stack we borrow is BOLTED TO THE CORE: the same per-core scratch stack
// task_exit's last breath stands on, re-issued to that core's next customer.
// So a context must never be PARKED while standing on it — park two contexts
// on one stack and they stomp each other's frames at deterministic offsets.
// That was the stack poisoner of 2026-08-11/12, and scheduler_load_thread's
// BORROWED-STACK TRIPWIRE (via kernel_scratch_stack_owner) panics rather than
// resume such a thread — the tripwire's own comment names "the cikc-hosted
// disk closes" as a suspected open door. This `cli` is that door closing.
//
// Most callers were already safe BY ACCIDENT, not by design: syscall bodies
// enter with IF=0 (SFMASK clears it, smp_core.c), and the shared-object page
// read runs from a #PF whose 0x8E gate did the same. But task_destroy's burial
// close (task.c) is reached from KWORKER — an ordinary kernel thread,
// dispatched with RFLAGS=0x202 — so it borrowed this stack with interrupts
// ON. One scheduling IPI (vector IPI_TIMER_SCHEDULE_VECTOR, an 0x8E gate with
// no IST, so it pushes onto whatever stack is current) during fops->close and
// kworker is parked on the scratch stack; the next core to resume it panics.
// Timing-dependent, and likeliest exactly when close does real disk I/O —
// which is precisely what ext2's orphan reaping made it do.
//
// Masking here rather than at the call site is deliberate: the hazard belongs
// to the BORROWING, so the borrower owns the guard and no future caller has to
// remember. It costs nothing where IF was already 0, and it is safe for the
// callees: NVMe's completion wait POLLS against an rdtsc deadline under an
// irqsave ioLock (nvme_wait_for_completion says so in writing — it must not
// depend on ticks precisely because it already runs with IF=0), and nothing on
// the filesystem/block close path sleeps, yields, or does a bare `sti`.
// RFLAGS is saved on the CALLER's stack, so the caller's IF — whatever it was
// — is what comes back.
.globl call_in_kernel_context
.type  call_in_kernel_context, @function
call_in_kernel_context:
    // Preserve the callee-saved regs we borrow (the caller expects them intact).
    push r12
    push r13
    push r14
    push r15
    pushfq                                          // caller's RFLAGS (IF included)
    cli                                             // nothing may park us on the borrowed stack
    mov  r12, rdi                                   // r12 = func
    mov  r13, rsi                                   // r13 = arg
    mov  r14, cr3                                   // r14 = task CR3   (survives call)
    mov  r15, rsp                                   // r15 = task RSP   (survives call)

    // kernel_rsp = (cls->kernel_interrupt_stack_top - 16) & ~0xF  (16-byte aligned
    // so the `call` below lands the callee at rsp%16 == 8, per the SysV ABI).
    mov  rax, [gs:CLS_KERNEL_INTERRUPT_STACK_TOP_OFFSET]
    sub  rax, 16
    and  rax, -16

    // Switch to the kernel interrupt stack, then to kKernelPML4.
    mov  rsp, rax
    mov  rax, kKernelPML4                           // value load, per scheduler.S idiom
    mov  cr3, rax

    // func(arg): SysV first argument in RDI.
    mov  rdi, r13
    call r12

    // Restore task CR3 and task RSP, then unwind and return to the caller (whose
    // return address is on the task stack we just restored). r15 was captured
    // AFTER the pushfq, so the restored RSP points straight at the saved RFLAGS
    // — popfq hands the caller back its own IF, and preemption resumes there.
    mov  cr3, r14
    mov  rsp, r15
    popfq                                           // caller's IF back, exactly as found
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    ret

// ── Ring-3 exit trampoline TEMPLATE ──────────────────────────────────────────
//
// These bytes are never executed here in the kernel image.  task_create()
// copies them into a user-mapped, read-only+executable page at
// TASK_EXIT_TRAMPOLINE_VIRT, and seeds that address as the initial return
// address on every ring-3 thread's user stack.  A user _start that plainly
// `ret`s therefore lands HERE at CPL 3 (an ordinary ring3→ring3 return — the
// ring-0 task_exit_with_retval seeding trick can't work for user threads
// because returning from CPL 3 into kernel text faults), and we convert the
// C return value into a proper exit syscall:
//   RAX (SysV return value) → RDI (exit code arg) → SYSCALL_EXIT.
//
// Must stay position-independent: RIP-relative jump only, no absolute
// references (the copy runs at a completely different VA than it was
// assembled at).  Kept in .rodata — it is DATA to the kernel, and mapping the
// user page without PAGE_WRITE means the program can't scribble on its own
// exit path.
.section .rodata
.globl user_exit_trampoline_template
.globl user_exit_trampoline_template_end
user_exit_trampoline_template:
    mov  rdi, rax                   // exit code = _start's return value
    mov  eax, SYSCALL_EXIT
    syscall
1:  jmp  1b                         // exit never returns; spin if it somehow does
user_exit_trampoline_template_end:

// ── The SIGNAL RETURN stub (2026-08-23, SIGNALS.md §5) ──────────────────────
//
// Copied into the SAME page as the exit trampoline, at TASK_SIGRETURN_OFFSET,
// and for the same reason it exists at all: a signal handler is an ordinary C
// function, so it ends with `ret`, and something has to be at that return
// address to hand control back to the kernel.
//
// THE PAGE IS WHY THIS IS EASY, and it is a debt the NX arc paid forward.
// Every historic Unix pushed this stub onto the STACK and pointed the return
// address at it — which needs an executable stack, the exact thing nx_test
// asserts is fatal here. Linux carried a stack trampoline for years before
// moving it into the vDSO. os64 never has to: this page is already mapped
// PAGE_USER without PAGE_WRITE, already executable, already per-task. The
// program can run its own return path and cannot scribble on it.
//
// The handler's frame pointer is irrelevant to us: sigreturn does not return
// here, it rebuilds the syscall's return context and drops to ring 3 at the
// interrupted address. The `jmp` after it is the same belt-and-braces as
// above — if sigreturn somehow comes back, spin rather than run whatever
// happens to sit in the next bytes of the page.
//
// Position-independent, like its neighbour: this code executes at a VA it was
// never assembled at.
// On entry RSP points AT the signal frame the kernel just wrote, so the
// handler's argument and address are read straight off it. Offsets are the
// ones signal_frame_t declares (signals.h) and signals.c static-asserts.
//
// The stub CALLS the handler rather than returning into it, which is why
// there is no pushed return address to arrange: the handler's own `ret` comes
// back to the next instruction here, and we fall through into sigreturn. RSP
// is 16-byte aligned on entry, so the `call` leaves the handler seeing
// RSP%16==8 — what SysV promises a called function.
.globl user_sigreturn_trampoline_template
.globl user_sigreturn_trampoline_template_end
user_sigreturn_trampoline_template:
    mov  rdi, [rsp + 40]            // frame.signo -> the handler's int argument
    mov  rax, [rsp + 48]            // frame.handler
    call rax
    mov  rdi, rsp                   // the frame, for sigreturn to validate
    mov  eax, SYSCALL_SIGRETURN
    syscall
1:  jmp  1b                         // sigreturn does not come back
user_sigreturn_trampoline_template_end:
.section .text
