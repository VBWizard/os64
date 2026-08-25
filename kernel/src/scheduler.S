#include "asm-offsets.h"

.code64
.intel_syntax noprefix
.extern mp_isrSavedCS, mp_isrSavedRIP, mp_isrSavedErrorCode, mp_isrSavedRAX, mp_isrSavedRBX, mp_isrSavedRCX, mp_isrSavedRDX
.extern mp_lastIretqRIP
.extern mp_rflagsTripValue, mp_rflagsTripCount
.extern mp_isrSavedRSI, mp_isrSavedRDI, mp_isrSavedRBP, mp_isrSavedCR0, mp_isrSavedCR1, mp_isrSavedCR4, mp_isrSavedDS, mp_isrSavedES
.extern mp_isrSavedR8, mp_isrSavedR9, mp_isrSavedR10, mp_isrSavedR11, mp_isrSavedR12, mp_isrSavedR13, mp_isrSavedR14, mp_isrSavedR15
.extern mp_isrSavedFS, mp_isrSavedGS, mp_isrSavedSS, mp_isrSavedNumber, mp_isrSavedCR2, mp_isrSavedRSP, mp_isrSavedRFlags, mp_isrSavedStack
.extern mp_schedStack
.extern mp_CoreHasRunScheduledThread
.extern mp_SchedulerTaskSwitched
.extern kKernelPML4v
.extern kMPEOIOffset
.extern scheduler_do
.extern mp_timesEnteringScheduler
.extern mp_inScheduler
.extern panic_no_shutdown

.globl _write_eoi
.type _write_eoi, @function
_write_eoi:
	push rax
	mov rax, kMPEOIOffset
	mov dword ptr [rax], 0
	pop rax
	ret

.globl _check_signals
.type _check_signals, @function
_check_signals:
# Check if signals are enabled and if so see if it's time to process them
	push rax
	mov al, [kProcessSignals]
	cmp al, 1
	jne process_signals_disabled

	mov ax, [signals_update_count]
	inc ax
	cmp ax, signalProcTickFrequency # Check signals every X ticks
	jb over_process_signals
	push rbx
	push rcx
	push rdx
	push rsi
	push rdi
	push rbp
	push r8
	push r9
	push r10
	push r11
	push r12
	push r13
	push r14
	push r15
	pushf
	call processSignals
	popf
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
	mov ax, 0
over_process_signals:
	mov [signals_update_count], ax
process_signals_disabled:
	pop rax
	ret

    # ******************** NEW MP SCHEUDLER CODE ********************
.globl _schedule_ap
.type _schedule_ap, @function
_schedule_ap:
    # NOTE: On entry the CPU pushed:
    # 0x00: RIP
    # 0x08: CS
    # 0x10: RFLAGS
    # 0x18: RSP (only if returning to a different privilege level)
    # 0x20: SS  (only if returning to a different privilege level)
    # We then push RBX below that frame before doing any work.
    # NOTE: Throughout this routine we will use RAX (the apic_id) for the index into each mp_isrSaved array variable
    # and RBX for the data to be stored to each mp_isrSaved array variable
    #
    # INTERRUPTS STAY OFF until mp_inScheduler is set (below).  The old `sti`
    # here opened a re-entry window: a HARDWARE scheduler interrupt is shielded
    # during this prologue by the APIC (no EOI sent yet -> same-priority
    # delivery held), but the late scheduler_yield() entered via SOFTWARE
    # `int`, which never sets the APIC in-service bit — so a pending timer
    # interrupt (very likely pending: syscalls run with IF masked) could nest
    # into this handler between the old sti and the mp_inScheduler store,
    # clobbering temp_rsp and the mp_isrSaved arrays with this handler's own
    # half-saved context; the eventual "resume" then iretq'd into garbage
    # (first seen as a #GP at a non-canonical RIP on syscall_smoke's first
    # yield).  scheduler_yield and its software `int` are gone — every entry
    # is a real LAPIC interrupt now — but the guard-before-sti ordering stays:
    # it is correct for any entry path, present or future.
    #
    # WHAT THAT COMMENT DID NOT KNOW (learned 2026-08-10, SCHEDULER_REENTRANCY.md):
    # the APIC in-service bit shields this vector for the pass, but the shield
    # EXPIRES AT THE EOI — which used to sit three instructions before the
    # iretq, with IF=1 and the guard already dropped.  Every scheduler
    # interrupt that went pending during the pass delivered exactly there, as
    # a FULL nested entry that captured "mid-exit, on the scheduler stack" as
    # the outgoing thread's saved state; when another core later dispatched
    # that thread, two cores ran on one scheduler stack and scribbled each
    # other into pointers full of program text.  The exit is now cli-bracketed
    # from frame-build through iretq (see scheduler_continue_2).
    #
    # And the shield never covered a HIGHER-priority vector: the scheduling
    # IPI was 0x81 (priority class 8) against this timer's 0x7E (class 7), so
    # a nudge from another core walked straight in mid-pass.  Both scheduler
    # entry vectors now live in class 7, where the hardware holds the second
    # one pending instead of delivering it.  The software guard below is the
    # backstop, not the primary defense.
    cld                                 # DF clear before ANY C runs (Codex #29 rd27, a P1).
                                        # Both scheduler vectors are wired straight here,
                                        # not through handle_mp_isr.S — so rd10's seven
                                        # clds missed the two most-taken interrupt entries
                                        # in the kernel. A thread interrupted mid-`std`
                                        # in ring 3 would have run scheduler_do's
                                        # compiler-emitted string ops backwards. The
                                        # interrupted RFLAGS are read from the STACK frame
                                        # into mp_isrSavedRFlags, so clearing the live
                                        # flag changes nothing about what is saved.
    push rbx                            # Save original RBX value

    mov rbx, rax                        # Put the original RAX value in RBX
	# APIC_ID is now in the RAX register
	mov rax, [gs:CLS_APIC_ID_OFFSET]		# apic_id — offset auto-generated from CLS struct via asm-offsets.h

    push rbx                            # Save original RAX value

	# NOTHING ABOVE THE GUARD CHECK MAY WRITE A GLOBAL (2026-08-10, see
	# SCHEDULER_REENTRANCY.md).  The temp_rsp store used to live RIGHT HERE,
	# two instructions above the guard test — so a nested entry that the guard
	# was about to bounce "harmlessly" had already overwritten the OUTER
	# entry's saved frame pointer on its way through the door.  The outer pass
	# then copied the WRONG frame's RIP/CS/RFLAGS/RSP/SS into mp_isrSaved* and
	# handed that garbage to a thread as its resume state.  A frozen guest
	# caught it in the act: temp_rsp[1] provably held 0x40013f98 at the lea and
	# read back 0x1011bfa8 twenty instructions later.  The store now happens
	# below the guard (see not_in_scheduler_continue), where RSP is already the
	# exact frame base and no scratch register or lea is needed at all.
	#
	# The bounce path below is therefore DATA-CLEAN: it touches only its own
	# stack, _write_eoi, and iretq.

	# Verify we aren't in the scheduler ISR already.  If so then we can't continue.  Do this as early as possible
	mov bl, [mp_inScheduler + rax]
	cmp bl, 0
    pop rbx                             # Restore original RAX value to RBX
	je not_in_scheduler_continue
already_in_scheduler_exit:
    mov rax, rbx                        # Bugfix 08/18/2025: Restore rax from rbx before returning
    pop rbx                             # Restore original RBX value
    call _write_eoi
	iretq

not_in_scheduler_continue:
    # Tag AP as 'in scheduler'.  This is the FIRST global written on this path,
    # and it happens with interrupts still off (IDT gate type 0x8E clears IF on
    # entry), so the guard is up before anything can observe a half-built state.
	mov byte ptr [mp_inScheduler + rax], 1

	# Unwind the two entry pushes and record the frame base, ALL under the
	# guard.  Register state on arrival here: RAX = apic_id, RBX = the
	# interrupted thread's RAX, and the interrupted thread's RBX is one push
	# deep on the stack.  Saving RAX's copy first frees RBX; popping the
	# second leaves RSP pointing at the CPU-pushed interrupt frame EXACTLY —
	# which is what temp_rsp wants, with no lea and no scratch register.
	mov [mp_isrSavedRAX + rax * 8], rbx # Save original RAX value in the array

    # Restore RBX value from when we entered the routine
    pop rbx

    mov [mp_isrSavedRBX + rax * 8], rbx # Save original EBX value in the array

	# The frame pointer, written where a bounce can no longer reach it.
	mov [temp_rsp + rax * 8], rsp

	# Guard flag is up and every global this entry owns is written — a nested
	# scheduler entry now exits via the already_in_scheduler path WITHOUT
	# touching a byte of our state, so it is safe to take interrupts again.
	sti

	# If the BSP is running the scheduler, do the signal check.  This runs a
	# whole C function (processSignals) with IF=1 and the guard up; that is
	# only tolerable because the bounce path is data-clean as of 2026-08-10.
	cmp rax, 0
	jne over_check_signals
	call _check_signals              # preserves RAX (apic_id) and RBX — leave it that way
over_check_signals:

    mov rbx, cr3
    mov [mp_isrSavedCR3 + rax * 8], rbx # Save CR3 in the array

	# Save RIP and CS from the interrupt frame BEFORE switching CR3.
	# Kernel tasks with their own PML4 have their stack only mapped in that PML4.
	# After switching to kKernelPML4, dereferencing temp_rsp reads from the wrong
	# physical pages (e.g. ktask's stack instead of the interrupted task's stack).
	mov rbx, [temp_rsp + rax * 8]
	mov rbx, [rbx]
	mov [mp_isrSavedRIP + rax * 8], rbx
	mov rbx, [temp_rsp + rax * 8]
	mov rbx, [rbx + 8]
	mov [mp_isrSavedCS + rax * 8], rbx

	# RFLAGS/RSP/SS have the SAME constraint as RIP/CS: for a task with its own
	# PML4 (ring0 kernel task OR ring3 user task) the interrupt frame sits on a
	# stack that is NOT mapped in kKernelPML4. So read ALL FIVE frame fields
	# here, BEFORE the CR3 switch, rather than after (where the dereference
	# would hit the wrong physical page, or fault). Frame layout:
	# [temp_rsp+0]=RIP, +8=CS, +16=RFLAGS, +24=RSP, +32=SS — long mode ALWAYS
	# pushes all five, even ring0->ring0, so this is valid for every entry.
	mov rbx, [temp_rsp + rax * 8]
	mov rbx, [rbx + 16]
	mov [mp_isrSavedRFlags + rax * 8], rbx

	# RSP must come from the frame's +24 field — NOT from temp_rsp arithmetic.
	# The CPU aligns RSP down to a 16-byte boundary before pushing an interrupt
	# frame in 64-bit mode, so "frame base + 40" only recovers the ALIGNED
	# value: any interrupted RSP not on a 16-byte boundary loses its remainder
	# (up to 15 bytes) and the thread resumes reading byte-shifted qwords off
	# its own stack.  -O0 C code masked this for years (every leave/ret resyncs
	# RSP from RBP); the syscall path's hand-written asm finally exposed it as
	# a #GP at a byte-shifted non-canonical RIP.  The +24 field is the TRUE
	# pre-alignment RSP, pushed by the CPU itself.
	mov rbx, [temp_rsp + rax * 8]
	mov rbx, [rbx + 24]
	mov [mp_isrSavedRSP + rax * 8], rbx

	# SS from the frame's +32 field, same reasoning (and the frame value is
	# correct for BOTH rings: ring0 pushes the current kernel SS, ring3 pushes
	# the user SS).
	mov rbx, [temp_rsp + rax * 8]
	mov rbx, [rbx + 32]
	mov [mp_isrSavedSS + rax * 8], rbx

	# STACK FIRST, THEN ADDRESS SPACE (hoisted here 2026-08-09 — this closes a
	# reproducible #DF).
	#
	# RSP still points at the interrupted thread's RSP0 kernel stack, which
	# task_alloc_guarded_stack put at a TASK-LOCAL, lower-half VA (thread.c —
	# its own comment says the stack "is NOT mapped in kKernelPML4"). The
	# instant the CR3 below lands, that stack ceases to exist in the active
	# address space, and it stays gone until OverSaveRegisters reloaded RSP
	# some 45 instructions later.
	#
	# Interrupts are ON in that gap (the `sti` after the mp_inScheduler guard,
	# above). The guard makes a nested scheduler entry EXIT harmlessly — but it
	# guards SOFTWARE, and the CPU pushes a 5-QWORD frame before the handler's
	# first instruction runs. With RSP dead that push #PFs, #PF carries no IST
	# (only #DF does, idt.c), so delivering it faults on the same corpse and the
	# machine takes a #DF instead. mp_inScheduler never gets read.
	#
	# Proof it was this: two separate #DFs, both CR2=0x1023bf98 (the task-local
	# RSP0 stack) with CR3=kKernelPML4 live — and in the same captures, cores
	# whose frames happened to land on the per-core SCHEDULER stack
	# (0xffff800005f85f98) survived the identical situation. Same event, one
	# stack kernel-visible, one not.
	#
	# The scheduler stacks are HHDM/upper-half and therefore valid under EVERY
	# CR3 — which is exactly why this line can move ABOVE the switch and not the
	# other way around. Every temp_rsp dereference is already done (they must
	# happen under the task's CR3, see above), and nothing between here and
	# scheduler_do touches the stack, so the move is pure win: a nested
	# interrupt now pushes onto the scheduler stack, below the outer handler's
	# data, and iretq's straight back out.
	#
	# Frequency, not immunity, is why tickless never showed this: a compute-
	# bound AP that takes no timer ticks barely enters this window at all. The
	# planned AP preemption backstop WILL create that traffic — this had to
	# land first.
	mov rsp, [mp_schedStack + rax * 8]

    mov rbx, kKernelPML4
    mov cr3, rbx

	mov rbx, 0
	# For APs that are entering the scheduler for the first time, don't save registers to saved variables
    cmp [mp_CoreHasRunScheduledThread + rax], bl # NOTE: variable is a bool, 1 byte per element
    je OverSaveRegisters                # Skip saving of registers

dontSkipRegisters:
    # Store register values in the arrays based on the calculated index
    mov [mp_isrSavedRCX + rax * 8], rcx
    mov [mp_isrSavedRDX + rax * 8], rdx
    mov [mp_isrSavedRSI + rax * 8], rsi
    mov [mp_isrSavedRDI + rax * 8], rdi
    mov [mp_isrSavedRBP + rax * 8], rbp
    mov [mp_isrSavedR8 + rax * 8], r8
    mov [mp_isrSavedR9 + rax * 8], r9
    mov [mp_isrSavedR10 + rax * 8], r10
    mov [mp_isrSavedR11 + rax * 8], r11
    mov [mp_isrSavedR12 + rax * 8], r12
    mov [mp_isrSavedR13 + rax * 8], r13
    mov [mp_isrSavedR14 + rax * 8], r14
	mov [mp_isrSavedR15 + rax * 8], r15 # Save the value of the R15 register so we can use it

    mov rbx, ds
    mov [mp_isrSavedDS + rax * 8], rbx
    mov ebx, es
    mov [mp_isrSavedES + rax * 8], rbx
    mov ebx, fs
    mov [mp_isrSavedFS + rax * 8], rbx

    # RIP/CS/RFLAGS/RSP/SS were ALL saved from the interrupt frame before the
    # CR3 switch above — the 5-QWORD frame is identical in shape for ring0 and
    # ring3 entries (long mode always pushes all five), so no per-ring branch
    # is needed here anymore.  The old ring0 arm computed RSP as frame+40,
    # which loses the interrupted RSP's low bits to the CPU's 16-byte frame
    # alignment; the old ring3 arm dereferenced the frame AFTER the CR3 switch,
    # which reads the wrong physical page for a user task with its own PML4
    # (its RSP0 kernel stack is task-local, not HHDM).  Both are now handled
    # by the single pre-switch frame read.

OverSaveRegisters:
    # Keep track of how many times we've entered the scheduler
    inc qword ptr [mp_timesEnteringScheduler + rax*8]

    # (The scheduler stack was loaded BEFORE the CR3 switch above — see the
    # long note there. It used to happen here, which left ~45 instructions
    # running with interrupts on and RSP pointing at a stack the active address
    # space no longer contained.)
overSetESP:

    # push RAX which has the apic_id, so that it is available when we get back from scheduling
    push rax
    # Call the scheduler
	call scheduler_do

	# Put the apic_id back in RAX
	pop rax                 			# apic_id is now in EAX
    
    # Identify whether the new CS is ring 3 for setting the threadIsKernel entry.
    # (Informational/debug only since the ring0/ring3 return paths were unified —
    # iretq handles both privilege levels from the same 5-qword frame.)
    mov rbx, [mp_isrSavedCS + rax * 8]
    and rbx, 3
    cmp rbx, 3
    je nonKernelProcess
    mov rbx,1
    jmp kernelProcess
nonKernelProcess:
    mov rbx,0
kernelProcess:
    mov [threadIsKernel + rax], bl		# threadIsKernel is a byte array, one for each core

    # Restore all the segment registers except CS
    # CS and RIP will be handled later on before executing an IRET to start executing a process
    mov bx, [mp_isrSavedDS + rax * 8]
    mov ds, bx
    mov bx, [mp_isrSavedES + rax * 8]
    mov es, bx
    mov bx, [mp_isrSavedFS + rax * 8]
    mov fs, bx
    mov rcx, [mp_isrSavedRCX + rax * 8]
    mov rdx, [mp_isrSavedRDX + rax * 8]
    mov rsi, [mp_isrSavedRSI + rax * 8]
    mov rdi, [mp_isrSavedRDI + rax * 8]
    mov rbp, [mp_isrSavedRBP + rax * 8]
	mov r8, [mp_isrSavedR8 + rax * 8]
	mov r9, [mp_isrSavedR9 + rax * 8]
	mov r10, [mp_isrSavedR10 + rax * 8]
	mov r11, [mp_isrSavedR11 + rax * 8]
	mov r12, [mp_isrSavedR12 + rax * 8]
	mov r13, [mp_isrSavedR13 + rax * 8]
	mov r14, [mp_isrSavedR14 + rax * 8]
	mov r15, [mp_isrSavedR15 + rax * 8]
    
	# Unconditionally set the IOPL bits of flags (TODO: Fix this)
    mov rbx, [mp_isrSavedRFlags + rax * 8]
    or rbx, 0x3000
    mov [mp_isrSavedRFlags + rax * 8], rbx

    push rax
    mov rax,[mp_isrSavedCR3 + rax * 8]
    mov rbx,cr3
    cmp rax,rbx
    je overRestoreCR3
    mov CR3, rax
overRestoreCR3:

    pop rax

# Unified ring0/ring3 return.  iretq consumes the SAME full 5-qword frame
# (SS:RSP:RFLAGS:CS:RIP) for both cases — for a ring3 CS (RPL 3) it performs
# the privilege drop and stack switch natively, and unlike the old retfq-based
# ring3Return path it also restores RFLAGS from the frame, so user tasks start
# with IF=1 and stay preemptible (retfq left ring 3 running with interrupts
# off — whatever popf loaded stayed loaded, and we had cleared IF).
# (I expected to not have to push SS:RSP for a same-priv-level return but I
# have to anyways — long mode always pushes/pops all 5, so save/restore stays
# symmetric.)
    # ── THE EXIT IS ATOMIC FROM HERE TO THE IRETQ (2026-08-10) ──────────
    # Everything below builds the 5-QWORD frame the iretq consumes, then
    # releases BOTH shields: _write_eoi drops the APIC's in-service hold and
    # the store to mp_inScheduler drops the software guard.  With IF=1 that
    # left a three-instruction landing strip for every scheduler interrupt
    # that had gone pending during the pass — a full nested entry that saved
    # this half-built exit as the outgoing thread's state.  That was THE
    # magnet (SCHEDULER_REENTRANCY.md); the whole corruption zoo hung off it.
    #
    # cli here, and not one sti before the iretq: the iretq restores the
    # resumed thread's own IF from the frame, atomically, so a tick that
    # went pending during the pass now delivers AFTER the return — into a
    # fresh scheduler entry with a clean frame, which is the correct meaning
    # of "a tick was pending."
    #
    # WHY NOT cli FOR THE WHOLE PASS (asked and answered, do not resurrect):
    # the LAPIC's IRR holds exactly ONE pending instance per vector, so a
    # long IF=0 span collapses every tick that lands inside it into one.  A
    # full-pass CLI was tried historically and lost clock ticks in bulk.
    # This window is about a dozen instructions; the clock only notices if
    # the host stalls INSIDE it for more than two tick periods.
    cli

    mov rbx, [mp_isrSavedSS + rax * 8]
    push rbx
	mov rbx, [mp_isrSavedRSP + rax * 8]
	push rbx

	jmp overSignalReturn

#    mov ebx, sigProcAddress
#    cmp ebx,0
#    je overSignalReturn

    #For a signal return we need to change the IRET return address to that of the signal process
    #We need to maintain the program's EIP, CS and FLAGS on the stack so we can IRET from the _sigJumpPoint
    #CURRENT STACK: EIP(program), CS, FLAGS
    #NEW STACK: EIP(_sigJumpPoint), CS, FLAGS, CR3(real), EIP(program), CS, FLAGS
    #Also the CR3 has to be that of the program, can't be kernel CR3 (like if we suspended from a syscall)
    #So we have to load it and put the return CR3 on the stack so _sigJumpPoint can load it
#    mov ebx, cr3
#    push ebx
#    mov ebx, [mp_isrSavedEFlags + rax * 8]
#    push ebx

#    mov ebx, 0x88
#    push ebx

#    mov ebx, [mp_isrSavedEIP + rax * 8]
#    push ebx

#    mov ebx, sigProcCR3
#    mov cr3, ebx

overSignalReturn:
    cmp rax, 1
    jne notCoreOneExiting

coreOneExiting:
    nop
notCoreOneExiting:

	# Restore the last of the registers
	# Yes these would still be on the stack if I didn't release them when saving the RSP last time the thread ran,
	# but I'd rather keep the variables and stack in sync so my debug logging is never wrong
	mov rbx, [mp_isrSavedRFlags + rax * 8]

	# ── RFLAGS TRIPWIRE (SCHEDULER_STRAY_WRITE.md) ──────────────────────
	# THIS read is the value the iretq actually consumes, so the judgment
	# happens HERE — after this point the stray writer's window is two
	# instructions wide instead of a whole dispatch. The thread structs
	# were proven clean from a frozen guest; the corruption lands in the
	# per-core slot, so anything with TF set or a bit outside the legal
	# RFLAGS mask means the stray write JUST happened: impound the raw
	# qword (it IS the clue — the 8/2 sighting decoded to a pointer into
	# the PCI name table), bump the counter processSignals reports on,
	# and sanitize so the boot SURVIVES to print the evidence instead of
	# dying as a masked #DB/#GP/#UD. Legal mask 0x3D7ED7 = every defined
	# RFLAGS bit except TF (the trap that single-stepped /idle2) and VM
	# (impossible in long mode); 0x3000 = the IOPL bits OR'd in above.
	# rcx already holds task state — borrowed via the stack (pop does not
	# disturb the flags the AND just set).
	push rcx
	mov rcx, 0xFFFFFFFFFFC28128        # ~0x3D7ED7: TF, VM, reserved, 22+
	and rcx, rbx
	pop rcx
	jz rflagsCleanForIretq
	mov [mp_rflagsTripValue + rax * 8], rbx
	lock inc qword ptr [mp_rflagsTripCount]
	mov rbx, 0x3202                    # sane: IF + the IOPL bits, nothing else
rflagsCleanForIretq:
	push rbx
    mov rbx, [mp_isrSavedCS + rax * 8]
	push rbx
	mov rbx, [mp_isrSavedRIP + rax * 8]
	push rbx

	# Test1
	push rax
	mov rax, 0xffffffff00000000
	cmp rbx, rax
	jge overScheduleTestRIP
invalidRIP:
	nop
overScheduleTestRIP:
    pop rax
	# Debug breadcrumb: the RIP this core is about to iretq to, by core.
	# This was `mov r15, rbx` from the earliest days of the OS — fine while
	# nothing used R15, but it clobbered every resumed thread's real R15
	# (restored above from mp_isrSavedR15) and only -O0's register habits
	# kept that from ever mattering. Working storage now: same breadcrumb,
	# findable by name from gdb or the monitor, and R15 is a register again.
	mov [mp_lastIretqRIP + rax * 8], rbx

	mov bl, [mp_inScheduler + rax]
	cmp bl,0
	je scheduler_halt_and_catch_a_coffee2
	jmp scheduler_continue_2
scheduler_halt_and_catch_a_coffee2:
	# If you got here then mp_inScheduler should have been true, BUT it was false!  Illogical.  Find it and fix it!
	#
	# It used to spin here forever, silently — and after the cli above that
	# would be a mute hard hang, the worst possible ending for a tripwire.
	# Say so instead.  panic_no_shutdown, NOT panic: reaching this label means
	# the scheduler's own state is provably corrupt (somebody cleared a guard
	# that only we may clear), and the orderly-shutdown path would ask that
	# same broken machinery to walk itself to the door.  Print and halt.
	#
	# Calling C from raw asm, three requirements: RDI/RSI are the SysV
	# argument registers (apic_id becomes the %u), RSP must be 16-byte aligned
	# at the call — free to clobber, since nothing here returns — and AL must
	# carry the count of vector registers used by a varargs call, which is 0.
	mov rsi, rax                       # apic_id → the format's %u
	lea rdi, [rip + hcf2_panic_msg]
	and rsp, -16                       # SysV alignment; we never come back
	xor eax, eax                       # varargs: zero vector regs used
	call panic_no_shutdown
	jmp scheduler_halt_and_catch_a_coffee2   # unreachable; the old behavior if panic ever returns
scheduler_continue_2:

	mov rbx, [mp_isrSavedRBX + rax * 8]
    call _write_eoi
	mov byte ptr [mp_inScheduler + rax], 0
    mov rax, [mp_isrSavedRAX + rax * 8]
    iretq

.section .data
.align 8
threadIsKernel:
    .rept 128      # Room for 128 processors' values
    .byte 0
    .endr
signals_update_count:	
	.rept 8 
	.byte 0
	.endr
temp_rsp:
	.rept 24
	.quad 8
	.endr

# The coffee trap's dying words (scheduler_halt_and_catch_a_coffee2).  Written
# out here rather than inline so the exit path stays readable.
hcf2_panic_msg:
	.asciz "SCHEDULER: core %u reached the iretq with mp_inScheduler ALREADY CLEAR — the guard was dropped by someone who does not own it. Scheduler state is not trustworthy; halting. See SCHEDULER_REENTRANCY.md.\n"
