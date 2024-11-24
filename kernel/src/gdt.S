.code64
.section .text
.global load_gdt_and_jump
.intel_syntax noprefix  # Use Intel syntax

load_gdt_and_jump:
	cli
    # Load the GDTR
    mov rax, rdi                # Load address of GDTR from the first argument (RDI)
    lgdt [rax]

    # Perform a far jump to reload CS
	mov rax, 0x28
	push rax
	lea rax, far_jump_target  # Use movabs to load full 64-bit address
	push rax
	retfq
far_jump_target:
    # Reload the data segment registers
    mov ax, 0x30                # 0x30 = GDT index 6 (Kernel Data Segment)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
	sti
    ret                         # Return to C

.section .data
    .align 16
    far_pointer: .space 10      # Reserve 10 bytes for segment + offset
