#ifndef TEST_FILE_DIALOG_SYSCALLS_H
#define TEST_FILE_DIALOG_SYSCALLS_H
/* Never execute Os64 syscall numbers on the Linux host. This header is forced
 * into every translation unit in the dialog harness, including font code. */
#define OS64_ABI_SYSCALL_H
#ifndef __ASSEMBLER__
#include <stdint.h>
#include "os64/syscall_numbers.h"
uint64_t file_test_syscall(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static inline uint64_t os64_syscall0(uint64_t n)
{ return file_test_syscall(n,0,0,0,0,0,0); }
static inline uint64_t os64_syscall1(uint64_t n,uint64_t a)
{ return file_test_syscall(n,a,0,0,0,0,0); }
static inline uint64_t os64_syscall2(uint64_t n,uint64_t a,uint64_t b)
{ return file_test_syscall(n,a,b,0,0,0,0); }
static inline uint64_t os64_syscall3(uint64_t n,uint64_t a,uint64_t b,uint64_t c)
{ return file_test_syscall(n,a,b,c,0,0,0); }
static inline uint64_t os64_syscall4(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{ return file_test_syscall(n,a,b,c,d,0,0); }
static inline uint64_t os64_syscall6(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
{ return file_test_syscall(n,a,b,c,d,e,f); }
#endif
#endif
