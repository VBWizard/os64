// Exercise the actual prompt builder and feed its captured writes through
// the production ANSI parser. No guest syscalls run in this fixture.
#define main husk_main
#include "../userland/apps/husk/husk.c"
#undef main
#include "ansi.h"

extern void abort(void);
extern int puts(const char *s);

static const char *test_prompt;
static char output[512];
static size_t output_len;

static void check(bool ok, const char *why)
{
    if (!ok) { puts(why); abort(); }
}

const char *os64_getenv(const char *name)
{ (void)name; return test_prompt; }
int64_t os64_getcwd(char *buf, size_t len)
{
    if (len < 4) return -1;
    buf[0] = '/'; buf[1] = 'a'; buf[2] = 'b'; buf[3] = 0;
    return 0;
}
int64_t os64_date_now(os64_date_t *out, os64_time_t *raw)
{ (void)out; (void)raw; return -1; }
int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    check(handle == 1 && output_len + len <= sizeof(output), "invalid prompt write");
    for (size_t i = 0; i < len; i++) output[output_len++] = ((const char *)buf)[i];
    return (int64_t)len;
}

static void render(const char *fmt, size_t expected_len, bool fallback)
{
    test_prompt = fmt; output_len = 0;
    prompt_render(0);
    check(output_len == expected_len, "unexpected prompt length");
    if (fallback) {
        const char *plain = "husk> ";
        for (size_t i = 0; i < expected_len; i++)
            check(output[i] == plain[i], "overlong prompt must use plain fallback");
    }
    ansi_parser_t parser = {0};
    for (size_t i = 0; i < output_len; i++) ansi_feed(&parser, output[i]);
    check(!ansi_in_sequence(&parser), "prompt left an unfinished escape");
    ansi_action_t a = ansi_feed(&parser, 'l');
    check(a.kind == ANSI_PRINT && a.byte == 'l', "prompt swallowed first echoed key");
}

int main(void)
{
    render(NULL, 6, true);
    render("", 6, true);
    render("\\e[32mOK\\e[0m ", 12, false);
    char fmt[600];
    for (unsigned i = 0; i < 255; i++) fmt[i] = 'x';
    fmt[255] = 0;
    render(fmt, 255, false);  // exactly the output byte limit still fits
    const char *tails[] = {"\\e[31m", "\\e]11;#123456\\e\\\\", "\\w", "\\?", "\\q"};
    const unsigned tail_lengths[] = {5, 14, 3, 1, 2};
    for (unsigned tail = 0; tail < sizeof(tails) / sizeof(tails[0]); tail++) {
        for (unsigned prefix = 240; prefix <= 256; prefix++) {
            for (unsigned i = 0; i < prefix; i++) fmt[i] = 'x';
            unsigned n = prefix;
            for (const char *s = tails[tail]; *s; s++) fmt[n++] = *s;
            fmt[n] = 0;
            size_t expanded = prefix + tail_lengths[tail];
            render(fmt, expanded > 255 ? 6 : expanded, expanded > 255);
        }
    }
    puts("test_husk_prompt_host: all checks passed");
    return 0;
}
