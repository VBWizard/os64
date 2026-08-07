// test_fmt_host.c — HOST-side unit test for libos64's fmt.c and args.c.
//
// Those two files are pure computation (no syscalls, no allocation), so they
// compile with plain host gcc and can be checked the strongest possible way:
// os64_vsnprintf diffed against the host C library's snprintf on a battery
// of cases, and the arg parser walked through its whole grammar. Runs in
// seconds at build time on the machine you're sitting at — no QEMU needed to
// catch a %-8s regression.
//
// Build & run:
//   gcc -I userland/libos64/include -I abi/include userland/libos64/fmt.c
//       userland/libos64/args.c userland/libos64/env.c userland/libos64/str.c
//       tools/test_fmt_host.c
//       -o /tmp/os64_fmt_test     (one line)
//   /tmp/os64_fmt_test

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "os64/fmt.h"
#include "os64/args.h"
#include "os64/env.h"     // the ABI env block — os64_getenv walks it
#include "os64/proc.h"    // os64_getenv declaration

// env.c's global, normally stored by launch.S before main; the test sets it
// to a hand-built block.
extern const os64_env_block_t *__os64_env;

// fmt.c's printf veneers call os64_write; on the host, swallow it.
long os64_write(int handle, const void *buf, size_t len)
{
    (void)handle; (void)buf;
    return (long)len;
}

static int failures = 0;

#define CHECK_FMT(...) check_fmt(__LINE__, __VA_ARGS__)
static void check_fmt(int line, const char *fmt, ...)
{
    char ours[256], host[256];
    va_list a1, a2;

    va_start(a1, fmt);
    va_copy(a2, a1);
    int n_ours = os64_vsnprintf(ours, sizeof(ours), fmt, a1);
    int n_host = vsnprintf(host, sizeof(host), fmt, a2);
    va_end(a1);
    va_end(a2);

    if (n_ours != n_host || strcmp(ours, host) != 0) {
        printf("FAIL line %d: fmt=\"%s\"\n  ours: [%s] (%d)\n  host: [%s] (%d)\n",
               line, fmt, ours, n_ours, host, n_host);
        failures++;
    }
}

static void expect(int line, int cond, const char *what)
{
    if (!cond) {
        printf("FAIL line %d: %s\n", line, what);
        failures++;
    }
}
#define EXPECT(cond) expect(__LINE__, (cond), #cond)

int main(void)
{
    // ── fmt: the battery ────────────────────────────────────────────────────
    CHECK_FMT("plain text, no conversions");
    CHECK_FMT("%s and %s", "one", "two");
    CHECK_FMT("[%c]", 'x');
    CHECK_FMT("%d %d %d", 0, 42, -42);
    CHECK_FMT("%i", -2147483647);
    CHECK_FMT("%u", 4294967295u);
    CHECK_FMT("%x %X", 0xdeadbeef, 0xdeadbeef);
    CHECK_FMT("%ld %lu", (long)-1234567890123L, (unsigned long)987654321098UL);
    CHECK_FMT("%lx", (unsigned long)0xffffffff80000000UL);
    CHECK_FMT("100%% done");
    CHECK_FMT("%8d|", 42);
    CHECK_FMT("%-8d|", 42);
    CHECK_FMT("%08d|", 42);
    CHECK_FMT("%08d|", -42);        // zero pad after the sign: -0000042
    CHECK_FMT("%8s|", "ab");
    CHECK_FMT("%-8s|", "ab");
    CHECK_FMT("%12lu|", (unsigned long)1572864);   // the ls size column
    CHECK_FMT("%s", "");
    CHECK_FMT("%4c|", 'y');
    // Precision and star-width — the ls column vocabulary.
    CHECK_FMT("%.5s|", "abcdefghij");
    CHECK_FMT("%.0s|", "abcdefghij");
    CHECK_FMT("%.20s|", "short");
    CHECK_FMT("%-12.5s|", "abcdefghij");
    CHECK_FMT("%12.5s|", "abcdefghij");
    CHECK_FMT("%*d|", 8, 42);
    CHECK_FMT("%*d|", -8, 42);        // negative * width = left align (std)
    CHECK_FMT("%-*s|%6lu", 20, "hello.txt", (unsigned long)70);
    CHECK_FMT("%.*s|", 3, "abcdefghij");
    CHECK_FMT("%.*s|", -3, "abcdefghij");   // negative .* = as if omitted (std)
    CHECK_FMT("%.5d|", 42);           // int precision = min digits, zero-filled
    CHECK_FMT("%8.5d|", 42);
    CHECK_FMT("%-8.5d|", -42);
    CHECK_FMT("%.5x|", 0xab);
    // Truncation: full length reported, buffer NUL'd at capacity.
    {
        char small[8];
        int n = os64_snprintf(small, sizeof(small), "abcdefghij");
        EXPECT(n == 10);
        EXPECT(strcmp(small, "abcdefg") == 0);
    }
    // %p differs from host formatting (we pin "0x..."), so check shape only.
    {
        char p[64];
        os64_snprintf(p, sizeof(p), "%p", (void *)0x1234abcd);
        EXPECT(strcmp(p, "0x1234abcd") == 0);
    }

    // ── args: the grammar ───────────────────────────────────────────────────
    static const os64_optspec_t specs[] = {
        { 'l', "long", 0, "long listing" },
        { 'a', "all",  0, "include dotfiles" },
        { 'o', "out",  1, "output file" },
    };

    {   // ls -la /bin extra — bundle, then positionals
        char *argv[] = { "ls", "-la", "/bin", "extra", NULL };
        os64_args_t a;
        os64_args_init(&a, 4, argv, specs, 3);
        EXPECT(os64_args_next(&a) == 'l');
        EXPECT(os64_args_next(&a) == 'a');
        EXPECT(os64_args_next(&a) == OS64_ARG_POSITIONAL && strcmp(a.value, "/bin") == 0);
        EXPECT(os64_args_next(&a) == OS64_ARG_POSITIONAL && strcmp(a.value, "extra") == 0);
        EXPECT(os64_args_next(&a) == OS64_ARG_END);
        EXPECT(os64_args_next(&a) == OS64_ARG_END);   // END is sticky
    }
    {   // long forms and values, both spellings
        char *argv[] = { "prog", "--long", "-o", "file1", "--out=file2", NULL };
        os64_args_t a;
        os64_args_init(&a, 5, argv, specs, 3);
        EXPECT(os64_args_next(&a) == 'l');
        EXPECT(os64_args_next(&a) == 'o' && strcmp(a.value, "file1") == 0);
        EXPECT(os64_args_next(&a) == 'o' && strcmp(a.value, "file2") == 0);
        EXPECT(os64_args_next(&a) == OS64_ARG_END);
    }
    {   // "--" ends options; "-" is positional; argv untouched throughout
        char *argv[] = { "prog", "--", "-l", "-", NULL };
        os64_args_t a;
        os64_args_init(&a, 4, argv, specs, 3);
        EXPECT(os64_args_next(&a) == OS64_ARG_POSITIONAL && strcmp(a.value, "-l") == 0);
        EXPECT(os64_args_next(&a) == OS64_ARG_POSITIONAL && strcmp(a.value, "-") == 0);
        EXPECT(os64_args_next(&a) == OS64_ARG_END);
        EXPECT(strcmp(argv[1], "--") == 0 && strcmp(argv[2], "-l") == 0);
    }
    {   // errors: unknown option, missing value, value on a flag
        char *argv1[] = { "prog", "-z", NULL };
        char *argv2[] = { "prog", "-o", NULL };
        char *argv3[] = { "prog", "--all=nope", NULL };
        os64_args_t a;
        os64_args_init(&a, 2, argv1, specs, 3);
        EXPECT(os64_args_next(&a) == OS64_ARG_ERROR && strcmp(a.value, "-z") == 0);
        os64_args_init(&a, 2, argv2, specs, 3);
        EXPECT(os64_args_next(&a) == OS64_ARG_ERROR);
        os64_args_init(&a, 2, argv3, specs, 3);
        EXPECT(os64_args_next(&a) == OS64_ARG_ERROR);
    }
    {   // -h and --help plead for help when unclaimed
        char *argv[] = { "prog", "-h", NULL };
        char *argv2[] = { "prog", "--help", NULL };
        os64_args_t a;
        os64_args_init(&a, 2, argv, specs, 3);
        EXPECT(os64_args_next(&a) == OS64_ARG_HELP);
        os64_args_init(&a, 2, argv2, specs, 3);
        EXPECT(os64_args_next(&a) == OS64_ARG_HELP);
    }
    {   // zero options the RIGHT way (NULL, 0): -h still pleads, help
        // doesn't crash, positionals still flow; and the WRONG way ({{}} =
        // one blank spec) must at least not walk garbage.
        char *argv[] = { "pwd", "-h", NULL };
        os64_args_t a;
        os64_args_init(&a, 2, argv, NULL, 0);
        EXPECT(os64_args_next(&a) == OS64_ARG_HELP);
        a.about = "print the current working directory";
        os64_args_help(&a, "pwd");           // must not crash on NULL specs
        EXPECT(a.details == NULL);           // init must null it (stack garbage otherwise)
        a.details = "the one true path, canonicalized";
        os64_args_help(&a, "pwd");           // about + details + usage, no crash
        static const os64_optspec_t blank[] = { {0, NULL, 0, NULL} };
        os64_args_init(&a, 2, argv, blank, 1);
        os64_args_help(&a, "pwd");           // blank row: prints nothing
    }
    {   // ── env: os64_getenv over a hand-built ABI block ────────────────────
        // (raw buffer + cast, because a struct ending in a flexible array
        // member can't legally nest inside another struct)
        static unsigned char raw[sizeof(os64_env_block_t) + 64];
        os64_env_block_t *blk = (os64_env_block_t *)raw;
        const char pairs[] = "PATH\0/bin\0HOSTNAME\0yogi\0";
        memcpy(blk->data, pairs, sizeof(pairs));
        blk->page_count = 1;
        blk->count = 2;
        blk->data_end = sizeof(pairs) - 1;   // through the last pair's NUL

        __os64_env = blk;
        EXPECT(os64_getenv("PATH") != NULL && strcmp(os64_getenv("PATH"), "/bin") == 0);
        EXPECT(os64_getenv("HOSTNAME") != NULL && strcmp(os64_getenv("HOSTNAME"), "yogi") == 0);
        EXPECT(os64_getenv("NOPE") == NULL);
        EXPECT(os64_getenv("PAT") == NULL);     // prefix of a key is not the key
        __os64_env = NULL;
        EXPECT(os64_getenv("PATH") == NULL);    // no block = honest NULL
    }
    {   // value option may end a bundle, never sit inside one
        char *argv[] = { "prog", "-lo", "f", NULL };
        char *argv2[] = { "prog", "-ol", NULL };
        os64_args_t a;
        os64_args_init(&a, 3, argv, specs, 3);
        EXPECT(os64_args_next(&a) == 'l');
        EXPECT(os64_args_next(&a) == 'o' && strcmp(a.value, "f") == 0);
        EXPECT(os64_args_next(&a) == OS64_ARG_END);
        os64_args_init(&a, 2, argv2, specs, 3);
        EXPECT(os64_args_next(&a) == OS64_ARG_ERROR);
    }

    {   // ── os64_args_parse: the whole-loop convenience ─────────────────────
        bool longMode = false, allMode = false;
        const char *outName = NULL;
        // Not static: the destinations are stack addresses. The three shapes:
        // flag, flag, value option.
        const os64_optspec_t pspecs[] = {
            { 'l', "long", 0, "long listing",      .flag = &longMode },
            { 'a', "all",  0, "include dotfiles",  .flag = &allMode },
            { 'o', "out",  1, "output file",       .value_out = &outName },
        };
        const char *pos[2] = { NULL, NULL };
        os64_args_t a;

        // The happy path: bundle + long-form value + positional, one call.
        char *argv[] = { "ls", "-la", "--out=f.txt", "/bin", NULL };
        os64_args_init(&a, 4, argv, pspecs, 3);
        EXPECT(os64_args_parse(&a, "ls [-la] [-o file] [path]", pos, 2) == 1);
        EXPECT(longMode == true && allMode == true);
        EXPECT(outName != NULL && strcmp(outName, "f.txt") == 0);
        EXPECT(pos[0] != NULL && strcmp(pos[0], "/bin") == 0);

        // More positionals than the app declared → ERROR.
        char *argv2[] = { "ls", "p1", "p2", NULL };
        os64_args_init(&a, 3, argv2, pspecs, 3);
        EXPECT(os64_args_parse(&a, "ls", pos, 1) == OS64_ARG_ERROR);

        // Help and unknown option come back as their sentinels.
        char *argv3[] = { "ls", "-h", NULL };
        os64_args_init(&a, 2, argv3, pspecs, 3);
        EXPECT(os64_args_parse(&a, "ls", pos, 1) == OS64_ARG_HELP);
        char *argv4[] = { "ls", "-z", NULL };
        os64_args_init(&a, 2, argv4, pspecs, 3);
        EXPECT(os64_args_parse(&a, "ls", pos, 1) == OS64_ARG_ERROR);

        // A destination-less spec reached by parse is a programmer error.
        const os64_optspec_t bare[] = { { 'x', NULL, 0, "no home" } };
        char *argv5[] = { "prog", "-x", NULL };
        os64_args_init(&a, 2, argv5, bare, 1);
        EXPECT(os64_args_parse(&a, "prog", pos, 1) == OS64_ARG_ERROR);

        // pwd-shaped: zero positionals allowed (NULL, 0); flags still land.
        char *argv6[] = { "prog", "-l", NULL };
        longMode = false;
        os64_args_init(&a, 2, argv6, pspecs, 3);
        EXPECT(os64_args_parse(&a, "prog", NULL, 0) == 0 && longMode == true);
        char *argv7[] = { "prog", "oops", NULL };
        os64_args_init(&a, 2, argv7, pspecs, 3);
        EXPECT(os64_args_parse(&a, "prog", NULL, 0) == OS64_ARG_ERROR);

        // A long-only option has no invented short spelling. cp's
        // --progress is the first customer.
        bool progress = false;
        const os64_optspec_t longOnly[] = {
            { '\0', "progress", 0, "show progress", .flag = &progress }
        };
        char *argv8[] = { "prog", "--progress", NULL };
        os64_args_init(&a, 2, argv8, longOnly, 1);
        EXPECT(os64_args_parse(&a, "prog [--progress]", NULL, 0) == 0 &&
               progress == true);
    }

    if (failures == 0) {
        printf("fmt+args host tests: ALL PASS\n");
        return 0;
    }
    printf("fmt+args host tests: %d FAILURES\n", failures);
    return 1;
}
