#include <stdio.h>
#include <stdlib.h>
#include "../userland/libjs/tests/owner_ticket_cases.h"
static size_t checks, live;
static void require(bool ok, const char *what)
{
    checks++;
    if(!ok) { fprintf(stderr,"FAIL: %s\n",what); exit(1); }
}
static void *allocate(size_t bytes) { void *p=malloc(bytes); if(p)live++; return p; }
static void release(void *p) { if(p)live--; free(p); }
int main(void)
{
    OwnerTestHost h={require,allocate,release};
    owner_ticket_cases(&h);
    require(!live,"owners: caller-owned arenas balance");
    printf("owner tickets: %zu checks, 0 failed\n",checks);
    return 0;
}
