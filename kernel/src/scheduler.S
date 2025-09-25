.code64
.intel_syntax noprefix
.extern mp_isrSavedCS, mp_isrSavedRIP, mp_isrSavedErrorCode, mp_isrSavedRAX, mp_isrSavedRBX, mp_isrSavedRCX, mp_isrSavedRDX
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
	sti
    push rbx                            # Save original RBX value

    mov rbx, rax                        # Put the original RAX value in RBX
	# APIC_ID is now in the RAX register
	mov rax, [gs:8]						# APIC_ID is the second field of the core_local_storage_t struct

    push rbx                            # Save original RAX value
    lea rbx, [rsp + 16]                 # Adjust rsp to reflect state *before* push rbx and before any changes
	mov [temp_rsp + rax * 8], rbx

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
    # Tag AP as 'in scheduler'
	mov byte ptr [mp_inScheduler + rax], 1

	# If the BSP is running the scheduler, do the signal check
	cmp rax, 0
	jne over_check_signals
	call _check_signals
over_check_signals:
	mov [mp_isrSavedRAX + rax * 8], rbx # Save original RAX value in the array

    # Restore RBX value from when we entered the routine
    pop rbx 
	
    mov [mp_isrSavedRBX + rax * 8], rbx # Save original EBX value in the array

    mov rbx, cr3
    mov [mp_isrSavedCR3 + rax * 8], rbx # Save CR3 in the array

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
    # Figure out if the CS was a kernel process or not
    # Interrupt frame layout: [0]=RIP, [8]=CS, [16]=RFLAGS, [+24]=RSP, [+32]=SS (ring3 only)
    mov rcx, [temp_rsp + rax * 8]
    mov rbx, [rcx + 8]
    and rbx, 3
    cmp rbx, 3
    jne same_priv_lvl_save_stack_and_flags
    
    # For ring 3 processes, we need to save the SS/ESP/Flags from the stack
    # TODO: Fix for ring 3!!!
	mov rbx, [rcx + 32]
    mov [mp_isrSavedSS + rax * 8], rbx
    # Get the RSP value from the interrupt stack frame (ring 3 pushes RIP/CS/RFLAGS/RSP/SS)
	mov rbx, [rcx + 24]
	mov [mp_isrSavedRSP + rax * 8], rbx
    mov rbx, [rcx + 16]
    mov [mp_isrSavedRFlags + rax * 8], rbx
    jmp over_save_samel_priv_lvl_stack_and_flags

same_priv_lvl_save_stack_and_flags:
    # For ring 0 processes, we need to save the SS/ESP/Flags from current values
    mov rbx, ss
    mov [mp_isrSavedSS + rax * 8], rbx
    # Get the RSP value from the interrupt stack frame (ring 0 pushes RIP/CS/RFLAGS)
	mov rbx, [temp_rsp + rax * 8]
	add rbx, 40
    mov [mp_isrSavedRSP + rax * 8], rbx
    pushfq
	pop rbx
    mov [mp_isrSavedRFlags + rax * 8], rbx

over_save_samel_priv_lvl_stack_and_flags:
	# Save CS/RIP for both rings (64-bit mode)
	mov rcx, [temp_rsp + rax * 8]
	mov rbx, [rcx + 8]
	mov [mp_isrSavedCS + rax * 8], rbx  # Save 64-bit CS value

	mov rbx, [rcx]
	mov [mp_isrSavedRIP + rax * 8], rbx # Save 64-bit RIP value

    
OverSaveRegisters:
    # Keep track of how many times we've entered the scheduler
    inc qword ptr [mp_timesEnteringScheduler + rax*8]
    
    # Load the scheduler stack pointer
    mov rsp, [mp_schedStack + rax * 8]
overSetESP:

    # push RAX which has the apic_id, so that it is available when we get back from scheduling
    push rax
    # Call the scheduler
    call scheduler_do
	mov rax, [gs:16]						# APIC_ID is the second field of the core_local_storage_t struct
    cmp rax, 0x0022
    jne overBreakInISR
breakInISRLabel: 
    nop

overBreakInISR:

    # Put the apic_id back in RAX
    pop rax                 			# apic_id is now in EAX
    
    # Identify whether the new CS is ring 3 for setting the threadIsKernel entry
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

    # If the new thread to be executed is ring 3, jump to a different return path
    pop rax
    mov bl, [threadIsKernel + rax * 1]
    cmp bl,1           
    jne ring3Return

# Ring 0 return (I expected to not have to do this for a same priv lvl return but I have to anyways)
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
	# Restore the last of the registers
	# Yes these would still be on the stack if I didn't release them when saving the RSP last time the thread ran,
	# but I'd rather keep the variables and stack in sync so my debug logging is never wrong
	mov rbx, [mp_isrSavedRFlags + rax * 8]
	push rbx
    mov rbx, [mp_isrSavedCS + rax * 8]
	push rbx
	mov rbx, [mp_isrSavedRIP + rax * 8]
	push rbx
notFirstTimeScheduled:
	# Test1
	push rax
	mov rax, 0xffffffff00000000
	cmp rbx, rax
	jge overScheduleTestRIP
invalidRIP:
	nop
overScheduleTestRIP:
	mov r15, rbx
    pop rax

	mov bl, [mp_inScheduler + rax]
	cmp bl,0
	je scheduler_halt_and_catch_a_coffee2
	jmp scheduler_continue_2
scheduler_halt_and_catch_a_coffee2:
	# If you got here then mp_inScheduler should have been true, BUT it was false!  Illogical.  Find it and fix it!
	jmp scheduler_halt_and_catch_a_coffee2
scheduler_continue_2:

	mov rbx, [mp_isrSavedRBX + rax * 8]
    call _write_eoi
	mov byte ptr [mp_inScheduler + rax], 0
    mov rax, [mp_isrSavedRAX + rax * 8]
    iretq



ring3Return:
    # NOTE: EAX still holds apic_id
    mov rbx, [mp_isrSavedRIP + rax * 8]
    mov [rsp], rbx
    mov rbx, [mp_isrSavedCS + rax * 8]
    mov [rsp + 8], rbx
    mov rbx, [mp_isrSavedRSP + rax * 8]
    mov [rsp + 16], rbx
    mov ebx, [mp_isrSavedSS + rax * 8]
    mov [rsp + 24], rbx
    mov bl,[mp_SchedulerTaskSwitched + rax]			# mp_SchedulerTaskSwitched is a byte array, 1 byte per core
    cmp bl,0
    jnz newTaskLoaded
    jmp doTheJump

newTaskLoaded:
    # Stack is already where it was when we started the ISR
    # reset the task switched indicator
    mov ebx,0
    mov [mp_SchedulerTaskSwitched + rax], bl

doTheJump:
    mov rbx, [mp_isrSavedRFlags + rax * 8]
    and rbx, 0xFFFFFFFFFFFFFDFF                         # Clear the IF flag
    push rbx
    popf
	mov rbx, [mp_isrSavedRBX + rax * 8]
	mov byte ptr [mp_inScheduler + rax], 0
    mov rax, [mp_isrSavedRAX + rax * 8]
    call _write_eoi
    retfq 0

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
