#include "recovery_cases.h"
#define RECOVERY_OK ((int32_t)0x446f0200)
static void require(bool pass, const char *what)
{
    if (pass) return;
    os64_serial_log(what);
    os64_exit(RECOVERY_OK+1);
}
static void real_clock(bool stepping)
{ (void)stepping; }
int main(void)
{
    os64_heap_report_t baseline;
    os64_heap_report_copy(&baseline);
    for (unsigned policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (unsigned mode = 0; mode < 6; ++mode) {
        recovery_consumer_case(policy, mode, require, real_clock);
        require(os64_heap_verify() == 0, "recoverytest: heap after root teardown");
        os64_heap_report_t after;
        os64_heap_report_copy(&after);
        require(after.bytes_live == baseline.bytes_live && after.blocks_live == baseline.blocks_live,
                "recoverytest: native and engine heap return to the process baseline");
        require(after.bytes_mapped == after.bytes_live + after.bytes_free + after.bytes_overhead + after.bytes_virgin,
                "recoverytest: target allocator accounting identity");
    }
    os64_serial_log("recoverytest: PASS (host routing, immutable diagnostics, siblings, native wait, global fallback, both policies)");
    return RECOVERY_OK;
}
