// test_ansi_host.c — host harness for the terminal's escape reader.
//
// The parser is fed a script one byte at a time and every action it produces
// is printed as a line. The companion shell script checks individual
// sequences and a concatenated corpus to exercise state carried between
// calls, including recovery from incomplete sequences.

#include <stdio.h>
#include <string.h>

#include "ansi.h"

static const char *kind_name(ansi_action_kind_t k)
{
    switch (k) {
        case ANSI_NOTHING:        return "nothing";
        case ANSI_PRINT:          return "print";
        case ANSI_SGR:            return "sgr";
        case ANSI_CURSOR_POS:     return "cup";
        case ANSI_ERASE_DISPLAY:  return "ed";
        case ANSI_ERASE_LINE:     return "el";
        case ANSI_GLASS_BG:       return "bg";
    }
    return "?";
}

// The script's own tiny escaping, so a test case can be written on one line
// of shell: \e is ESC, \a is BEL, \n \r \t \\ as usual, \xHH for anything.
static size_t unescape(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = in; *p != '\0' && n + 1 < cap; p++) {
        if (*p != '\\') { out[n++] = *p; continue; }
        p++;
        switch (*p) {
            case 'e': out[n++] = 0x1B; break;
            case 'a': out[n++] = 0x07; break;
            case 'n': out[n++] = '\n'; break;
            case 'r': out[n++] = '\r'; break;
            case 't': out[n++] = '\t'; break;
            case '\\': out[n++] = '\\'; break;
            case 'x': {
                unsigned v = 0;
                for (int i = 0; i < 2 && p[1] != '\0'; i++) {
                    char c = *++p;
                    v = v * 16 + (unsigned)((c >= 'a') ? c - 'a' + 10 :
                                            (c >= 'A') ? c - 'A' + 10 : c - '0');
                }
                out[n++] = (char)v;
                break;
            }
            case '\0': return n;
            default: out[n++] = *p; break;
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_ansi <escaped-script>\n");
        return 2;
    }

    char script[4096];
    size_t len = unescape(argv[1], script, sizeof(script));

    ansi_parser_t parser;
    memset(&parser, 0, sizeof(parser));   // a zeroed parser is a valid one

    for (size_t i = 0; i < len; i++) {
        ansi_action_t a = ansi_feed(&parser, script[i]);
        switch (a.kind) {
            case ANSI_NOTHING:
                break;
            case ANSI_PRINT:
                printf("print %02x\n", (unsigned char)a.byte);
                break;
            case ANSI_GLASS_BG:
                printf("bg %06x\n", a.color);
                break;
            default:
                printf("%s", kind_name(a.kind));
                for (uint8_t j = 0; j < a.nparams; j++)
                    printf(" %u", (unsigned)a.params[j]);
                printf("\n");
                break;
        }
    }
    // A parser left mid-sequence is a fact worth printing: a test that ends
    // inside a sequence should say so rather than look like a clean run.
    if (ansi_in_sequence(&parser))
        printf("pending\n");
    return 0;
}
