#define YONDER_HOST_FAKES_ONLY
#include "test_yonder_scripts_host.c"
#include <os64/js_failure.h>
int main(void)
{
    os64_text_options_t options = {.memory={.alloc=probe_alloc,.free=probe_free},
        .backend=flow_test_backend(),.memory_cap=8*1024*1024};
    check(os64_text_create(&options, &probe_text) == OS64_FONT_OK, "recovery policy: text context");
    check(os64_text_font_bitmap(probe_text, &probe_font) == OS64_FONT_OK, "recovery policy: bitmap font");
    loop_page("<script>globalThis.ready=1</script>");
    check(loop_settle() && yonder_scripts_alive(g.page.scripts), "recovery policy: ordinary browser script arrives");
    os64_js_recovery_info_t info;
    check(os64_js_recovery_inspect(teardown_fixture_runtime, OS64_JS_ABI_ID, &info) == OS64_JS_ADMISSION_REFUSED,
          "recovery policy: primary-only browser construction remains opted out");
    loop_drop();
    join_click_overrun();
    os64_text_font_release(probe_font);
    check(os64_text_destroy(probe_text) == OS64_FONT_OK, "recovery policy: text released");
    check(live == 0, "recovery policy: browser failure/teardown releases the heap");
    printf("Yonder recovery policy: %u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
