// Execute production allocator transactions with host memory and a fallible
// HHDM preparation stand-in. Only privileged irq masking is replaced.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MEMCPY_H
#define _PRINTF_H
#undef __USE_MISC
#define SPINLOCK_H
 typedef volatile uint32_t spinlock_t;
static uint64_t spinlock_acquire_irqsave(spinlock_t *p) { while(__sync_lock_test_and_set(p,1)){} return 0; }
static void spinlock_release_irqrestore(spinlock_t *p,uint64_t f) { (void)f;__sync_lock_release(p); }
#include "../kernel/src/memory/allocator.c"
#include "../kernel/src/memory/kmalloc.c"
static bool allow_prepare;
static unsigned prepares,maps,checks;
uint64_t kHHDMOffset;
volatile bool kHHDMMaintenanceEnabled=true;
void printd(__uint128_t level,const char *fmt,...) {(void)level;(void)fmt;}
void panic(const char *fmt,...) {(void)fmt;abort();}
bool paging_hhdm_prepare_range(uintptr_t start,size_t length)
{
    ++prepares;
    if(kMemoryStatus[0].startAddress!=start || kMemoryStatus[0].in_use ||
       kMemoryStatus[0].length<length)abort();
    return allow_prepare;
}
void paging_hhdm_map_range(uintptr_t start,size_t length){(void)start;(void)length;++maps;}
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"memory_try: line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
int main(void)
{
    static memory_status_t ledger[INITIAL_MEMORY_STATUS_COUNT];
    unsigned char *ram=aligned_alloc(4096,8192);CHECK(ram!=NULL);
    memset(ram,0xa5,8192);kMemoryStatus=ledger;kMemoryStatusCurrentPtr=1;
    ledger[0]=(memory_status_t){(uintptr_t)ram,8192,false};
    CHECK(kmalloc_try(0)==NULL && kmalloc_try(UINT64_MAX)==NULL);
    CHECK(kmalloc_try(8193)==NULL && prepares==0 && maps==0);
    memory_status_t before=ledger[0];
    CHECK(kmalloc_try(4096)==NULL && prepares==1 && maps==0);
    CHECK(!memcmp(&before,&ledger[0],sizeof(before)) && kMemoryStatusCurrentPtr==1);
    CHECK(ram[0]==0xa5 && ram[8191]==0xa5);
    allow_prepare=true;
    CHECK(kmalloc_try(4093)==ram && prepares==2 && maps==1);
    CHECK(kMemoryStatusCurrentPtr==2 && ledger[1].in_use && ledger[1].length==4096);
    for(unsigned i=0;i<4096;++i)CHECK(ram[i]==0);
    CHECK(ram[4096]==0xa5);
    // Metadata refusal must precede a carve even with sufficient free RAM.
    kMemoryStatusCurrentPtr=INITIAL_MEMORY_STATUS_COUNT-2;
    for(size_t i=1;i<kMemoryStatusCurrentPtr;++i)ledger[i]=(memory_status_t){1,1,true};
    before=ledger[0];unsigned was=prepares;
    CHECK(kmalloc_try(8)==NULL && prepares==was && maps==1);
    CHECK(!memcmp(&before,&ledger[0],sizeof(before)));
    free(ram);printf("memory_try: %u checks passed\n",checks);return 0;
}
