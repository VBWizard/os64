// Real pool CAS and table preparation; host pointers stand in for HHDM aliases.
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define MEMCPY_H
#define _PRINTF_H
#undef __USE_MISC
#include "../kernel/src/memory/paging.c"
static unsigned checks;
static uint32_t seen[4096];
void printd(__uint128_t level,const char *fmt,...) {(void)level;(void)fmt;}
void panic(const char *fmt,...) {(void)fmt;abort();}
void *arena_alloc_aligned(struct arena *a,size_t size,size_t alignment)
{(void)a;(void)size;(void)alignment;abort();}
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"paging_try: line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
static void *draw(void *unused)
{
    (void)unused;uintptr_t p;
    while((p=try_paging_table_page())){
        size_t i=(p-kPagingPagesBaseAddressP)/4096;
        if(i>=4096 || __atomic_fetch_add(&seen[i],1,__ATOMIC_RELAXED))abort();
    }
    return NULL;
}
int main(void)
{
    void *pool=aligned_alloc(4096,4096*4096);CHECK(pool!=NULL);
    kPagingPagesBaseAddressV=kPagingPagesBaseAddressP=kPagingPagesCurrentPtr=(uintptr_t)pool;
    kPagingPagesCount=4096;kHHDMOffset=0;
    pthread_t threads[8];
    for(unsigned i=0;i<8;++i)CHECK(!pthread_create(&threads[i],NULL,draw,NULL));
    for(unsigned i=0;i<8;++i)CHECK(!pthread_join(threads[i],NULL));
    for(unsigned i=0;i<4096;++i)CHECK(seen[i]==1);
    CHECK(!try_paging_table_page());
    CHECK(kPagingPagesCurrentPtr==(uintptr_t)pool+4096*4096);
    pt_entry_t *root=aligned_alloc(4096,4096);CHECK(root!=NULL);memset(root,0,4096);
    kKernelPML4v=(uintptr_t)root;kHHDMMaintenanceEnabled=true;
    kPagingPagesCurrentPtr=(uintptr_t)pool;kPagingPagesCount=2;
    CHECK(!paging_hhdm_prepare_range(0x400000,4096));
    CHECK(kPagingPagesCurrentPtr==(uintptr_t)pool+8192);
    kPagingPagesCount=3;
    CHECK(paging_hhdm_prepare_range(0x400000,4096));
    CHECK(kPagingPagesCurrentPtr==(uintptr_t)pool+12288);
    pt_entry_t *pdpt=(void *)(root[0]&~0xfffUL),*pd=(void *)(pdpt[0]&~0xfffUL);
    pt_entry_t *pt=(void *)(pd[2]&~0xfffUL);
    for(unsigned i=0;i<512;++i)CHECK(pt[i]==0);
    CHECK(paging_hhdm_prepare_range(0x400000,4096));
    CHECK(!paging_hhdm_prepare_range(0x600000,4096));
    CHECK(!paging_hhdm_prepare_range(UINT64_MAX-8,32));
    CHECK(!paging_hhdm_prepare_range(0x400000,0));
    free(root);free(pool);printf("paging_try: %u checks passed\n",checks);return 0;
}
