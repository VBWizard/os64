#include <os64/os64.h>
#include "owner_ticket_cases.h"
#define OWNER_TICKETS_OK ((int32_t)0x4f570200)
static size_t checks;
static void require(bool ok, const char *what)
{
    checks++;
    if(!ok) { os64_serial_log(what); os64_exit(OWNER_TICKETS_OK+1); }
}
int main(void)
{
    os64_heap_report_t before,after;
    os64_heap_report_copy(&before);
    OwnerTestHost h={require,os64_malloc,os64_free};
    owner_ticket_cases(&h);
    require(os64_heap_verify()==0,"owner tickets: target heap integrity");
    os64_heap_report_copy(&after);
    require(after.bytes_live==before.bytes_live && after.blocks_live==before.blocks_live,
        "owner tickets: caller-owned metadata arena returns to target heap baseline");
    require(after.bytes_mapped==after.bytes_live+after.bytes_free+after.bytes_overhead+after.bytes_virgin,
        "owner tickets: target allocator accounting identity");
    char line[160];
    os64_snprintf(line,sizeof line,"owner tickets: %lu checks, target heap PASS (metadata only; no runtime issuer/integration proof)\n",(unsigned long)checks);
    os64_serial_log(line);
    return OWNER_TICKETS_OK;
}
