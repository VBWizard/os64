#ifndef SMP_OFFSETS_H
#define SMP_OFFSETS_H

#define CLS_KERNEL_RSP0_OFFSET 0x50

#ifndef __ASSEMBLER__
#include <stddef.h>
#include "smp.h"
_Static_assert(CLS_KERNEL_RSP0_OFFSET == offsetof(core_local_storage_t, kernel_rsp0),
               "CLS_KERNEL_RSP0_OFFSET mismatch");
#endif

#endif
