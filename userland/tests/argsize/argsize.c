// A spawn carries an argument as long as OS64_SPAWN_ARG_MAX allows, whole,
// and answers OS64_SPAWN_TOO_LONG for more than it can carry — one argument
// too long, too many of them, or too much in all — rather than handing the
// child a truncated argv. The child half checks what arrived, byte for byte,
// in its argv and again in its own /proc/self/cmdline.
#include "os64/os64.h"

#define PASS 0xA2650000u
#define SELF "/tests/argsize"

static char pattern(size_t i)
{
    return (char)('a' + (i % 26));
}

static unsigned long parse(const char *s)
{
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (unsigned long)(*s++ - '0');
    return v;
}

static bool is_pattern(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (s[i] != pattern(i))
            return false;
    return s[len] == '\0' || s[len] == '\n';
}

// ── the child's half ────────────────────────────────────────────────────────
// argsize arg <len> <string>      — the string is <len> bytes of the pattern
// argsize count <n> ...           — argc is exactly <n>
// argsize cmdline <len> <string>  — and /proc/self/cmdline says so too
static int child(int argc, char **argv)
{
    if (argc >= 3 && os64_streq(argv[1], "count"))
        return argc == (int)parse(argv[2]) ? 0 : 21;

    if (argc != 4)
        return 22;
    unsigned long len = parse(argv[2]);
    if (os64_strlen(argv[3]) != len || !is_pattern(argv[3], len))
        return 23;
    if (!os64_streq(argv[1], "cmdline"))
        return 0;

    // One argument per line: the path, "cmdline", the length, the string.
    size_t cap = len + 256;
    char *text = os64_malloc(cap);
    int64_t h = os64_open("/proc/self/cmdline", "r");
    if (text == NULL || h < 0)
        return 24;
    size_t got = 0;
    for (;;)
    {
        int64_t n = os64_read((int32_t)h, text + got, cap - 1 - got);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (got == cap - 1)
            return 25;          // more than argv could have made
    }
    os64_close((int32_t)h);
    text[got] = '\0';
    char *line = text;
    for (int skip = 0; skip < 3; skip++)
    {
        while (*line != '\0' && *line != '\n')
            line++;
        if (*line != '\n')
            return 26;
        line++;
    }
    return (is_pattern(line, len) && line[len] == '\n' && line[len + 1] == '\0') ? 0 : 27;
}

// ── the parent's half ───────────────────────────────────────────────────────
static char *make_arg(size_t len)
{
    char *s = os64_malloc(len + 1);
    if (s != NULL)
    {
        for (size_t i = 0; i < len; i++)
            s[i] = pattern(i);
        s[len] = '\0';
    }
    return s;
}

// Spawn SELF <mode> <len> <string> and report the child's exit code, or the
// spawn's own negative answer.
static int64_t run(const char *mode, size_t len)
{
    char *s = make_arg(len);
    char num[24];
    if (s == NULL)
        return -1000;
    os64_snprintf(num, sizeof(num), "%lu", (unsigned long)len);
    char *argv[] = { SELF, (char *)mode, num, s, NULL };
    int64_t pid = os64_spawn(SELF, argv);
    int32_t code = -1;
    if (pid > 0 && os64_wait(pid, &code) != pid)
        code = -1001;
    os64_free(s);
    return pid > 0 ? code : pid;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return child(argc, argv);

    // Whole, at a size no command line reaches by accident, and at the cap.
    if (run("arg", 100 * 1024) != 0)
        return PASS | 1;
    if (run("arg", OS64_SPAWN_ARG_MAX - 1) != 0)
        return PASS | 2;
    if (run("cmdline", 100 * 1024) != 0)
        return PASS | 3;

    // One byte more than the cap leaves no room for the terminator.
    if (run("arg", OS64_SPAWN_ARG_MAX) != OS64_SPAWN_TOO_LONG)
        return PASS | 4;

    // Nine arguments, each allowed, are more than a child's block holds.
    char *big = make_arg(OS64_SPAWN_ARG_MAX - 1);
    if (big == NULL)
        return PASS | 5;
    char *nine[11] = { SELF };
    for (int i = 1; i <= 9; i++)
        nine[i] = big;
    nine[10] = NULL;
    if (os64_spawn(SELF, nine) != OS64_SPAWN_TOO_LONG)
        return PASS | 6;
    os64_free(big);

    // 512 arguments are carried; a 513th is refused, not quietly dropped.
    static char *many[515];
    many[0] = SELF;
    many[1] = "count";
    many[2] = "512";
    for (int i = 3; i < 514; i++)
        many[i] = "x";
    many[512] = NULL;
    int64_t pid = os64_spawn(SELF, many);
    int32_t code = -1;
    if (pid <= 0 || os64_wait(pid, &code) != pid || code != 0)
        return PASS | 7;
    many[512] = "x";
    many[513] = NULL;
    if (os64_spawn(SELF, many) != OS64_SPAWN_TOO_LONG)
        return PASS | 8;

    // An address the kernel cannot read is still that, not "too long".
    char *wild[] = { SELF, (char *)8, NULL };
    if (os64_spawn(SELF, wild) != -2)
        return PASS | 9;

    os64_printf("argsize: PASS 128 KiB arguments whole in argv and cmdline; "
                "too long, too many and too much refused by name\n");
    return PASS;
}
