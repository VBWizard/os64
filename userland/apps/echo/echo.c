// echo.c — say what you were told, then get out of the way.
//
// echo is famously the simplest program in Unix AND its longest-running
// portability war: BSD added -n (suppress newline), System V answered with
// the \c escape instead, and the two camps diverged hard enough that POSIX
// eventually declared echo's behavior with ANY option or backslash
// implementation-defined and invented printf(1) as the portable escape
// hatch. A one-line program, unportable for forty years. os64 has exactly
// one echo and owns it, so it simply takes both: -n AND \c.
//
// Shape (deliberately NOT os64_args): echo's arguments ARE its text. After
// the leading flags, every token is literal — `echo hello -n` prints
// "hello -n", because -n after text is text. That "flags stop at the first
// non-flag" scan is the one thing the os32 echo got structurally right,
// and it's kept here. The args parser would keep hunting for options all
// the way down the line (and reject them); echo is the canonical odd app
// the manual path exists for.
//
// Escapes (-e): decoded in ONE pass, in place of os32's five-malloc
// strreplace relay. \f is in the set — on os64 that byte clears the
// console (see BasicRenderer.c), so `echo -e \f` is clear's party-trick
// cousin. \c is the System V fossil: stop printing HERE, newline included.

#include "os64/os64.h"

#define OUT_MAX 256   // one decoded token; matches husk's whole-line cap

// Did any write fail? echo used to ignore every return value and exit 0
// unconditionally — which meant `echo kill > /proc/32/ctl` reported success
// for a command the kernel had refused, and `$?` said 0. A program that could
// not deliver its output has not succeeded, and must not claim it did.
// (Found the day /proc's ctl file arrived, which is the first thing in os64
// that ever answers a write with "no".)
static bool gWriteFailed = false;

// Every byte echo emits goes through here. os64_write returns the count, or a
// negative sentinel — os64 has no errno by design, the return value IS the
// answer (LIBOS64.md).
static void emit(const char *s, size_t n)
{
    if (os64_write(1, s, n) < 0)
        gWriteFailed = true;
}

static bool str_is(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// Decode C-style escapes from s into out (cap bytes, NUL-terminated).
// Returns the byte count. Sets *stop when \c is seen: print what we have,
// then say nothing more — not even the newline.
static int32_t decode_escapes(const char *s, char *out, int32_t cap, bool *stop)
{
    int32_t n = 0;
    while (*s != '\0' && n < cap - 1)
    {
        if (*s == '\\' && s[1] != '\0')
        {
            s++;
            switch (*s)
            {
                case 'n':  out[n++] = '\n';  break;
                case 't':  out[n++] = '\t';  break;
                case 'r':  out[n++] = '\r';  break;
                case 'b':  out[n++] = '\b';  break;
                case 'f':  out[n++] = '\f';  break;   // fresh page (1963)
                case 'e':  out[n++] = 0x1b;  break;   // ESC, for someday
                case '\\': out[n++] = '\\';  break;
                case 'c':                             // SysV: full stop, no newline
                    *stop = true;
                    out[n] = '\0';
                    return n;
                default:                              // unknown escape: keep both
                    out[n++] = '\\';
                    if (n < cap - 1)
                        out[n++] = *s;
                    break;
            }
            s++;
        }
        else
        {
            out[n++] = *s++;
        }
    }
    out[n] = '\0';
    return n;
}

int main(int argc, char **argv)
{
    (void)argc;

    bool noNewline = false;
    bool doEscapes = false;
    bool stop = false;
    int32_t i = 1;

    // --help only as the FIRST token — anywhere later it's text, honestly.
    if (argv[1] != NULL && str_is(argv[1], "--help"))
    {
        os64_printf("usage: echo [-n] [-e] [text ...]\n"
                    "Print its arguments, space-separated, newline-terminated.\n"
                    "  -n  no trailing newline\n"
                    "  -e  decode escapes: \\n \\t \\r \\b \\f \\e \\\\ \\c\n");
        return 0;
    }

    // Leading flags only; the first token that isn't one ends the scan and
    // everything from there on is text as typed.
    for (; argv[i] != NULL; i++)
    {
        if (str_is(argv[i], "-n"))
            noNewline = true;
        else if (str_is(argv[i], "-e"))
            doEscapes = true;
        else
            break;
    }

    for (bool first = true; argv[i] != NULL && !stop; i++, first = false)
    {
        if (!first)
            emit(" ", 1);

        if (doEscapes)
        {
            char out[OUT_MAX];
            int32_t n = decode_escapes(argv[i], out, sizeof(out), &stop);
            emit(out, (size_t)n);
        }
        else
        {
            size_t n = 0;
            while (argv[i][n] != '\0')
                n++;
            emit(argv[i], n);
        }
    }

    if (!noNewline && !stop)
        emit("\n", 1);

    return gWriteFailed ? 1 : 0;
}
