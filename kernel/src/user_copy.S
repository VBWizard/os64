.intel_syntax noprefix

# The copies that touch user memory by its user address (USER_COPY.md;
# user_copy.h). A long copy moves its bytes the way the kernel's own memcpy
# moves one (shared/copy_fill.c, copy_rep: QEMU without KVM emulates a
# string instruction one step at a time, so eight bytes a step, not one):
# single bytes up to the destination's first 8-byte boundary, then eight
# bytes a step, then single bytes for the tail. A copy of COPY_SHORT_MOST
# bytes or fewer is one `rep movsb` of its own: three string instructions'
# start-up would cost more than the copy, and memcpy's own short path ends
# at the same size.
#
# Those string moves are the only instructions here that reach user memory,
# and each is listed in the resume table below beside the address its fault
# resumes at. A ring-0 page fault at one, on a user address the handler
# cannot resolve (a sibling took the page away after the check), resumes at
# its failure exit instead of panicking (copy_fault.c, simple_exceptions.c).
# A string move that faults leaves RCX holding the steps it had not done, so
# each exit adds what was still to come after its move and returns the bytes
# not copied (an 8-byte step the fault interrupted counts whole; the callers
# ask only for 0).
#
# Registers in a long copy: RCX the steps of the move under way; R8 the
# 8-byte steps still to come; RDX the tail's bytes still to come. A long
# copy's head is at most 7 bytes, so it never outruns the copy. All three
# are the caller's to lose (the System V convention).
#
# The direction must be upward. A syscall arrives with DF clear (SFMASK
# clears bit 10 at entry, smp_core.c), and the kernel's own calling
# convention keeps it clear across calls and across a park (a thread
# resumes with the flags it parked with); the `cld` here makes the routine
# right whatever its caller left, for the cost of one instruction.

.set COPY_SHORT_MOST, 64

.section .text

# size_t user_copy_in(void *kernel_dst, const void *user_src, size_t n)
# RDI = kernel destination, RSI = user source, RDX = bytes.
# Returns 0 when every byte was copied, else the bytes not copied.
.global user_copy_in
user_copy_in:
    cld
    mov rcx, rdx
    cmp rdx, COPY_SHORT_MOST
    jbe user_copy_in_short
    mov rcx, rdi
    neg rcx
    and rcx, 7                  # the head: bytes to the destination's 8-byte boundary
    sub rdx, rcx
    mov r8, rdx
    shr r8, 3                   # the 8-byte steps after the head
    and rdx, 7                  # the tail after them
user_copy_in_head:
    rep movsb
    mov rcx, r8
user_copy_in_quads:
    rep movsq
    mov rcx, rdx
user_copy_in_tail:
    rep movsb
    xor eax, eax
    ret
user_copy_in_short:
    rep movsb
    xor eax, eax
    ret
user_copy_in_head_fault:
    lea rax, [rcx + r8 * 8]
    add rax, rdx
    ret
user_copy_in_quads_fault:
    lea rax, [rdx + rcx * 8]
    ret
user_copy_in_tail_fault:
user_copy_in_short_fault:
    mov rax, rcx
    ret

# size_t user_copy_out(void *user_dst, const void *kernel_src, size_t n)
# RDI = user destination, RSI = kernel source, RDX = bytes.
# Returns 0 when every byte was copied, else the bytes not copied.
.global user_copy_out
user_copy_out:
    cld
    mov rcx, rdx
    cmp rdx, COPY_SHORT_MOST
    jbe user_copy_out_short
    mov rcx, rdi
    neg rcx
    and rcx, 7                  # the head: bytes to the destination's 8-byte boundary
    sub rdx, rcx
    mov r8, rdx
    shr r8, 3                   # the 8-byte steps after the head
    and rdx, 7                  # the tail after them
user_copy_out_head:
    rep movsb
    mov rcx, r8
user_copy_out_quads:
    rep movsq
    mov rcx, rdx
user_copy_out_tail:
    rep movsb
    xor eax, eax
    ret
user_copy_out_short:
    rep movsb
    xor eax, eax
    ret
user_copy_out_head_fault:
    lea rax, [rcx + r8 * 8]
    add rax, rdx
    ret
user_copy_out_quads_fault:
    lea rax, [rdx + rcx * 8]
    ret
user_copy_out_tail_fault:
user_copy_out_short_fault:
    mov rax, rcx
    ret

# The resume table: each instruction that touches user memory, and where a
# fault there resumes (copy_fault.h's copy_resume_t). Read by the page-fault
# handler, never written.
.section .rodata
.balign 8
.global kCopyResumeTable
kCopyResumeTable:
    .quad user_copy_in_head, user_copy_in_head_fault
    .quad user_copy_in_quads, user_copy_in_quads_fault
    .quad user_copy_in_tail, user_copy_in_tail_fault
    .quad user_copy_in_short, user_copy_in_short_fault
    .quad user_copy_out_head, user_copy_out_head_fault
    .quad user_copy_out_quads, user_copy_out_quads_fault
    .quad user_copy_out_tail, user_copy_out_tail_fault
    .quad user_copy_out_short, user_copy_out_short_fault
.global kCopyResumeCount
kCopyResumeCount:
    .quad (. - kCopyResumeTable) / 16

.section .note.GNU-stack,"",@progbits
