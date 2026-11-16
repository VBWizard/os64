// Execute production allocator transactions with host memory and a fallible
// HHDM preparation stand-in. Only privileged irq masking and the TSC are
// replaced. Physical addresses are small frame numbers' bytes; kHHDMOffset is
// the host buffer, aligned past the fake physical range so phys|offset is the
// host address of that byte.
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
static uint64_t spinlock_acquire_irqsave_measured(spinlock_t *p,uint64_t (*clock)(void),uint64_t *waited)
{ (void)clock;*waited=0;return spinlock_acquire_irqsave(p); }
static void spinlock_release_irqrestore(spinlock_t *p,uint64_t f) { (void)f;__sync_lock_release(p); }
#include "../kernel/src/memory/frames.c"
#include "../kernel/src/memory/allocator.c"
#include "../kernel/src/memory/kmalloc.c"
#define RAM_FRAMES 64
#define RAM_BYTES (RAM_FRAMES*PAGE_SIZE)
static bool allow_prepare;
static unsigned prepares,maps,checks;
uint64_t kHHDMOffset;
volatile bool kHHDMMaintenanceEnabled=true;
uint64_t rdtsc(void) {return 0;}
void printd(__uint128_t level,const char *fmt,...) {(void)level;(void)fmt;}
void panic(const char *fmt,...) {(void)fmt;abort();}
bool paging_hhdm_prepare_range(uintptr_t start,size_t length)
{
    ++prepares;
    if(start==0 || length==0 || start+length>RAM_BYTES)abort();
    return allow_prepare;
}
void paging_hhdm_map_range(uintptr_t start,size_t length)
{
    if(start==0 || length==0 || start+length>RAM_BYTES)abort();
    ++maps;
}
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"memory_try: line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
// Bytes of the fake RAM that no longer hold the fill: everything an
// allocation zeroed, and nothing else may have been written.
static size_t touched(const unsigned char *ram)
{
    size_t n=0;
    for(size_t i=0;i<RAM_BYTES;++i)n+=ram[i]!=0xa5;
    return n;
}
static void books_unchanged(uint64_t free_frames,uint64_t largest)
{
    uint64_t where;
    CHECK(frames_audit(&kFrames,&where)==NULL);
    CHECK(kFrames.free_frames==free_frames && frames_largest_free(&kFrames)==largest);
}
int main(void)
{
    static memory_status_t ledger[INITIAL_MEMORY_STATUS_COUNT];
    unsigned char *ram=aligned_alloc(RAM_BYTES,RAM_BYTES);CHECK(ram!=NULL);
    memset(ram,0xa5,RAM_BYTES);kHHDMOffset=(uintptr_t)ram;
    frame_t *table=calloc(1,frames_table_bytes(RAM_FRAMES));CHECK(table!=NULL);
    CHECK(frames_init(&kFrames,table,RAM_FRAMES)==FRAMES_OK);
    CHECK(frames_add_region(&kFrames,1,RAM_FRAMES-1)==FRAMES_OK);
    kMemoryStatus=ledger;kMemoryStatusCurrentPtr=0;
    uint64_t free0=kFrames.free_frames,largest0=frames_largest_free(&kFrames);

    // Unservable lengths, and a run longer than all free memory, refuse
    // before any HHDM preparation.
    CHECK(kmalloc_try(0)==NULL && kmalloc_try(UINT64_MAX)==NULL);
    CHECK(kmalloc_try(RAM_BYTES)==NULL && prepares==0 && maps==0);
    books_unchanged(free0,largest0);

    // A frame run whose preparation is refused goes back: books and RAM as
    // they were, nothing mapped.
    CHECK(kmalloc_try(PAGE_SIZE)==NULL && prepares==1 && maps==0);
    books_unchanged(free0,largest0);
    CHECK(touched(ram)==0);

    allow_prepare=true;
    unsigned char *run=kmalloc_try(PAGE_SIZE+1);
    CHECK(run!=NULL && prepares==2 && maps==1);
    uint64_t run_frame=((uintptr_t)run-kHHDMOffset)/PAGE_SIZE;
    CHECK(frames_live(&kFrames,run_frame) && frames_kind(&kFrames,run_frame)==FRAMES_RUN);
    CHECK(kFrames.free_frames==free0-2);
    for(unsigned i=0;i<2*PAGE_SIZE;++i)CHECK(run[i]==0);
    CHECK(touched(ram)==2*PAGE_SIZE);

    // A ledger request with no hole borrows a chunk; a refused preparation
    // hands the chunk straight back and records no row.
    allow_prepare=false;
    uint64_t free1=kFrames.free_frames,largest1=frames_largest_free(&kFrames);
    CHECK(kmalloc_try(100)==NULL && prepares==3 && maps==1 && kMemoryStatusCurrentPtr==0);
    books_unchanged(free1,largest1);
    CHECK(touched(ram)==2*PAGE_SIZE);

    allow_prepare=true;
    // Prepared twice: at the chunk's base before it is committed, then for
    // the hole the carve takes, which a fresh chunk's preparation covers.
    unsigned char *small=kmalloc_try(4093);
    CHECK(small!=NULL && prepares==5 && maps==2);
    CHECK(kMemoryStatusCurrentPtr==2 && ledger[1].in_use && ledger[1].length==4096);
    CHECK(ledger[1].startAddress==(uintptr_t)small-kHHDMOffset);
    CHECK(!ledger[0].in_use && ledger[0].startAddress==ledger[1].startAddress+4096);
    uint64_t chunk_frame=ledger[1].startAddress/PAGE_SIZE;
    CHECK(frames_kind(&kFrames,chunk_frame)==FRAMES_LEDGER && frames_live(&kFrames,chunk_frame));
    CHECK(kFrames.free_frames==free1-LEDGER_CHUNK_FRAMES);
    for(unsigned i=0;i<4096;++i)CHECK(small[i]==0);
    CHECK(touched(ram)==2*PAGE_SIZE+4096);   // the rest of the chunk is lent, not written

    // Metadata refusal must precede a carve even with a free hole that fits.
    kMemoryStatusCurrentPtr=INITIAL_MEMORY_STATUS_COUNT-2;
    for(size_t i=2;i<kMemoryStatusCurrentPtr;++i)ledger[i]=(memory_status_t){1,1,true};
    memory_status_t before=ledger[0];unsigned was=prepares;
    uint64_t free2=kFrames.free_frames,largest2=frames_largest_free(&kFrames);
    CHECK(kmalloc_try(8)==NULL && prepares==was && maps==2);
    CHECK(!memcmp(&before,&ledger[0],sizeof(before)));
    books_unchanged(free2,largest2);
    free(table);free(ram);printf("memory_try: %u checks passed\n",checks);return 0;
}
