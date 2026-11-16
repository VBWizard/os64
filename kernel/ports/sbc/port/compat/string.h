// string.h for the SBC encoder's upstream files, which include it for one
// call: SbcAnalysisInit's memset. The kernel has no C library; this
// declares the kernel's own memset (kernel/include/memory/memset.h) and
// nothing else, so any other C library name upstream reached for would be
// undeclared (an error under the kernel's GCC 14) and an undefined symbol
// tools/test_sbc_audit.py refuses.

#ifndef SBC_COMPAT_STRING_H
#define SBC_COMPAT_STRING_H

#include <stddef.h>

void *memset(void *dst, int c, size_t n);

#endif
