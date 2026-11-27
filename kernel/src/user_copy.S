.intel_syntax noprefix

# The copies that touch user memory by its user address (USER_COPY.md;
# user_copy.h). Each is one `rep movsb`, the only instruction here that
# reaches user memory, listed in the resume table below beside the address
# its fault resumes at. A ring-0 page fault at that instruction on a user
# address the handler cannot resolve (a sibling took the page away after
# the check) resumes at the routine's failure exit instead of panicking
# (copy_fault.c, simple_exceptions.c): `rep movsb` leaves RCX holding the
# bytes it had not yet copied, which is the answer.
#
# The direction must be upward. A syscall arrives with DF clear (SFMASK
# clears bit 10 at entry, smp_core.c), and the kernel's own calling
# convention keeps it clear across calls and across a park (a thread
# resumes with the flags it parked with); the `cld` here makes the routine
# right whatever its caller left, for the cost of one instruction.

.section .text

# size_t user_copy_in(void *kernel_dst, const void *user_src, size_t n)
# RDI = kernel destination, RSI = user source, RDX = bytes.
# Returns 0 when every byte was copied, else the bytes not copied.
.global user_copy_in
user_copy_in:
    cld
    mov rcx, rdx
user_copy_in_move:
    rep movsb
    xor eax, eax
    ret
user_copy_in_fault:
    mov rax, rcx
    ret

# size_t user_copy_out(void *user_dst, const void *kernel_src, size_t n)
# RDI = user destination, RSI = kernel source, RDX = bytes.
# Returns 0 when every byte was copied, else the bytes not copied.
.global user_copy_out
user_copy_out:
    cld
    mov rcx, rdx
user_copy_out_move:
    rep movsb
    xor eax, eax
    ret
user_copy_out_fault:
    mov rax, rcx
    ret

# The resume table: each instruction that touches user memory, and where a
# fault there resumes (copy_fault.h's copy_resume_t). Read by the page-fault
# handler, never written.
.section .rodata
.balign 8
.global kCopyResumeTable
kCopyResumeTable:
    .quad user_copy_in_move, user_copy_in_fault
    .quad user_copy_out_move, user_copy_out_fault
.global kCopyResumeCount
kCopyResumeCount:
    .quad 2

.section .note.GNU-stack,"",@progbits
