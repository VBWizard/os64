#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "../userland/libjs/tests/owner_incarnation_cases.h"
enum { THREADS=8, PER_THREAD=2048 };
typedef struct {
    JSOwnerIncarnationIssuer *local;
    uint64_t values[PER_THREAD];
    JSOwnerStatus status[PER_THREAD];
    size_t count;
} Worker;
static size_t checks;
static void require(bool ok, const char *what)
{
    checks++;
    if(!ok) { fprintf(stderr,"FAIL: %s\n",what); exit(1); }
}
static void *worker(void *opaque)
{
    Worker *w=opaque;
    for(size_t i=0;i<w->count;i++)
        w->status[i]=w->local?js_owner_incarnation_reserve(w->local,&w->values[i]):
            js_owner_incarnation_next(&w->values[i]);
    return NULL;
}
static int compare(const void *a,const void *b)
{ uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b; return (x>y)-(x<y); }
int main(void)
{
    owner_incarnation_cases(require);
    pthread_t threads[THREADS]; Worker workers[THREADS]={0};
    uint64_t values[THREADS*PER_THREAD];
    for(size_t i=0;i<THREADS;i++) {
        workers[i].count=PER_THREAD;
        require(pthread_create(&threads[i],NULL,worker,&workers[i])==0,"incarnation: start concurrent issuer");
    }
    for(size_t i=0;i<THREADS;i++) {
        require(pthread_join(threads[i],NULL)==0,"incarnation: join concurrent issuer");
        for(size_t j=0;j<PER_THREAD;j++) {
            require(workers[i].status[j]==JS_OWNER_OK,"incarnation: concurrent reservation succeeds");
            values[i*PER_THREAD+j]=workers[i].values[j];
        }
    }
    qsort(values,THREADS*PER_THREAD,sizeof(*values),compare);
    for(size_t i=1;i<THREADS*PER_THREAD;i++)
        require(values[i]==values[i-1]+1,"incarnation: concurrent actual issuer is unique and contiguous");
    /* Exactly one thread can claim the last identity from this fresh test
     * namespace. Losers preserve their sentinel and the terminal counter. */
    JSOwnerIncarnationIssuer terminal={UINT64_MAX-1};
    for(size_t i=0;i<THREADS;i++) {
        workers[i].local=&terminal; workers[i].count=1; workers[i].values[0]=77;
        require(pthread_create(&threads[i],NULL,worker,&workers[i])==0,"incarnation: start terminal race");
    }
    size_t won=0;
    for(size_t i=0;i<THREADS;i++) {
        require(pthread_join(threads[i],NULL)==0,"incarnation: join terminal race");
        if(workers[i].status[0]==JS_OWNER_OK) {
            won++;
            require(workers[i].values[0]==UINT64_MAX,"incarnation: terminal winner gets final identity");
        } else require(workers[i].status[0]==JS_OWNER_EXHAUSTED && workers[i].values[0]==77,
            "incarnation: terminal losers preserve outputs");
    }
    require(won==1 && __atomic_load_n(&terminal._last,__ATOMIC_RELAXED)==UINT64_MAX,
        "incarnation: one winner without namespace wrap");
    printf("owner incarnation: %zu checks, 0 failed\n",checks);
    return 0;
}
