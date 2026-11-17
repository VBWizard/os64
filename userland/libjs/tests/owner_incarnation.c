#include <os64/os64.h>
#include "owner_incarnation_cases.h"
#define OWNER_INCARNATION_OK ((int32_t)0x4f490200)
static size_t checks;
static void require(bool ok, const char *what)
{
    checks++;
    if(!ok) { os64_serial_log(what); os64_exit(OWNER_INCARNATION_OK+1); }
}
int main(void)
{
    os64_heap_report_t before,after;
    os64_heap_report_copy(&before);
    owner_incarnation_cases(require);
    require(os64_heap_verify()==0,"owner incarnation: target heap integrity");
    os64_heap_report_copy(&after);
    require(after.bytes_live==before.bytes_live && after.blocks_live==before.blocks_live,
        "owner incarnation: caller-owned metadata arena returns to target heap baseline");
    require(after.bytes_mapped==after.bytes_live+after.bytes_free+after.bytes_overhead+after.bytes_virgin,
        "owner incarnation: target allocator accounting identity");
    char line[160];
    os64_snprintf(line,sizeof line,"owner incarnation: %lu checks, target heap PASS (issuer only; no runtime integration proof)\n",(unsigned long)checks);
    os64_serial_log(line);
    return OWNER_INCARNATION_OK;
}
