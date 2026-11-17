#include <os64/os64.h>
#include "owner_runtime_cases.h"
#include "owner_admission_cases.h"
#include "owner_realm_cases.h"
#define OWNER_RUNTIME_OK ((int32_t)0x4f520400)
static size_t checks;
static void require(bool ok, const char *what)
{
    checks++;
    if(!ok) { os64_serial_log(what); os64_exit(OWNER_RUNTIME_OK+1); }
}
int main(void)
{
    os64_heap_report_t before,after;
    os64_heap_report_copy(&before);
    owner_runtime_cases(require);
    owner_admission_cases(require,NULL);
    owner_realm_cases(require);
    require(os64_heap_verify()==0,"owner runtime: target heap integrity");
    os64_heap_report_copy(&after);
    require(after.bytes_live==before.bytes_live && after.blocks_live==before.blocks_live,
        "owner runtime: caller-owned metadata arena returns to target heap baseline");
    require(after.bytes_mapped==after.bytes_live+after.bytes_free+after.bytes_overhead+after.bytes_virgin,
        "owner runtime: target allocator accounting identity");
    char line[160];
    os64_snprintf(line,sizeof line,"owner runtime: %lu checks, target heap PASS (actual realm owners and checked source; no public profile/reclamation)\n",(unsigned long)checks);
    os64_serial_log(line);
    return OWNER_RUNTIME_OK;
}
