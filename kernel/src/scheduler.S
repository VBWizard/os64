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

.extern write_eoi
.extern scheduler_do
.extern mp_timesEnteringScheduler

    # ******************** NEW MP SCHEUDLER CODE ********************
.globl _schedule_ap
.type _schedule_ap, @function
_schedule_ap:
    # NOTE: Stack is:
    # 0x00: Vector number
    # 0x04: Error code (if applicable)
    # 0x08: EIP
    # 0x0C: CS
    # 0x10: EFLAGS
    # 0x14: ESP - only if we're returning to a different privilege level
    # 0x18: SS - only if we're returning to a different privilege level
    # NOTE: Throughout this routine we will use EAX (the apic_id) for the index into each mp_isrSaved array variable
    # and EBX for the data to be stored to each mp_isrSaved array variable
#    cli                                 # Disable interrupts
sti
    push rax                            # Save original EAX value
    call write_eoi                      # Acknowledge the interrupt
    pop rax                             # Restore original EAX value
    add rsp, 8                          # get rid of vector158 return address
    push rbx                            # Save original EBX value
    mov rbx, rax                        # Put the original EAX value in EBX
	mov rax, [gs:8]						# APIC_ID is the first field of the core_local_storage_t struct
	# APIC_ID is now in the RAX register
    mov [mp_isrSavedRAX + rax * 8], rbx # Save original EAX value in the array
    pop rbx                             # Restore original EBX value
    mov [mp_isrSavedRBX + rax * 8], rbx # Save original EBX value in the array
	mov [mp_isrSavedR15 + rax * 8], r15 # Save the value of the R15 register so we can use it

	mov r15, [gs:0]						# R15 now points at the core_local_storage_t struct



    mov rbx, cr3
    mov [mp_isrSavedCR3 + rax * 8], rbx # Save CR3 in the array

    mov rbx, kKernelPML4
    mov cr3, rbx

	mov rbx, 0
    cmp [mp_CoreHasRunScheduledThread + rax * 8], rbx # For APs that are entering the scheduler for the first time
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
	# r15 was saved earlier

    mov rbx, ds
    mov [mp_isrSavedDS + rax * 8], rbx
    mov ebx, es
    mov [mp_isrSavedES + rax * 8], rbx
    mov ebx, fs
    mov [mp_isrSavedFS + rax * 8], rbx
    # Figure out if the CS was a kernel process or not
    mov rbx, [rsp+8]
    and rbx, 3
    cmp rbx, 3
    jne same_priv_lvl_save_stack_and_flags
    
    # For ring 3 processes, we need to save the SS/ESP/Flags from the stack
    mov rbx, [rsp+24]
    mov [mp_isrSavedSS + rax * 8], rbx
    mov rbx, [rsp+32]
    mov [mp_isrSavedRSP + rax * 8], rbx
    mov rbx, [rsp+16]
    mov [mp_isrSavedRFlags + rax * 8], rbx
    jmp over_save_samel_priv_lvl_stack_and_flags

same_priv_lvl_save_stack_and_flags:
    # For ring 0 processes, we need to save the SS/ESP/Flags from current values
    mov rbx, ss
    mov [mp_isrSavedSS + rax * 8], rbx
    mov [mp_isrSavedRSP + rax * 8], rsp
    pushfq
	pop rbx
    mov [mp_isrSavedRFlags + rax * 8], rbx

over_save_samel_priv_lvl_stack_and_flags:
	# Save CS/RIP for both rings (64-bit mode)
	mov rbx, [rsp + 8]
	mov [mp_isrSavedCS + rax * 8], rbx  # Save 64-bit CS value

	mov rbx, [rsp]
	mov [mp_isrSavedRIP + rax * 8], rbx # Save 64-bit RIP value

    
OverSaveRegisters:
    # Keep track of how many times we've entered the scheduler
    inc qword ptr [mp_timesEnteringScheduler + rax*8]
    mov rsp, [mp_schedStack + rax * 8]
overSetESP:

    # push RAX which has the apic_id, so that it is available when we get back from scheduling
    push rax
	# push R15 which has the core_local_storage_t base address, so that it is available when we get back from scheduling
	push r15
    # Call the scheduler
    call scheduler_do
#    cli
	pop r15								# core_local_storage_t base address is now in r15
    mov rax, [r15 + 16]					# Get the new threadID
    shr rax, 3
    cmp rax, 0x0022
    jne overBreakInISR
breakInISRLabel: 
    nop

overBreakInISR:

    pop rax                 			# apic_id is now in EAX
    mov rbx, [mp_isrSavedCS + rax * 8]
    and rbx, 3
    cmp rbx, 3
    je nonKernelProcess

    mov rbx,1
    jmp continue1
nonKernelProcess:
    mov rbx,0
continue1:
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
	# We'll restore r15 later, before returning to ring 3
    
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

    mov bl, [threadIsKernel + rax * 8]
    cmp bl,1           
    jne overKernelReturn

    mov rbx, [mp_isrSavedSS + rax * 8]
    mov ss, rbx
    mov rsp, [mp_isrSavedRSP + rax * 8]

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
    mov r15, [mp_isrSavedR15 + rax * 8]
	mov rbx, [mp_isrSavedRBX + rax * 8]
    mov rax, [mp_isrSavedRAX + rax * 8]
    iretq



overKernelReturn:
    # NOTE: EAX still holds apic_id
    mov rbx, [mp_isrSavedRIP + rax * 8]
    mov [rsp], rbx
    mov rbx, [mp_isrSavedCS + rax * 8]
    mov [esp + 8], rbx
    mov rbx, [mp_isrSavedRSP + rax * 8]
    mov [esp + 16], rbx
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
    mov r15, [mp_isrSavedR15 + rax * 8]
	mov rbx, [mp_isrSavedRBX + rax * 8]
    mov rax, [mp_isrSavedRAX + rax * 8]
    sti                                         # Only enable interrupts immediately before IRET
    retfq 0

.section .data
.align 8
threadIsKernel:
    .rept 128      # Room for 128 processors' values
    .byte 0
    .endr
