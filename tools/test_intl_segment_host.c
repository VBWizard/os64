/* libjs Intl's segmentation (userland/libjs/intl/segment.c) held to
 * Unicode's own tests: every line of GraphemeBreakTest.txt, WordBreakTest.txt
 * and SentenceBreakTest.txt (tools/unicode/segmentation/, pinned), each
 * line's text made UTF-16 and segmented from its start, the boundaries
 * compared with the line's ÷ marks. Then isWordLike on a few words, lone
 * surrogates, and a long run after a full stop (SB8 looks ahead through it). Run by tools/test_intl_segment_host.sh, under ASan. */
#include "../userland/libjs/intl/segment.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned checks, failures;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* One test line: "÷ 0020 × 0308 ÷", into UTF-16 and the code-unit offsets
 * the ÷ marks stand at. False for a line with no test on it. */
static int parse(const char *line, uint16_t *text, size_t *length, size_t *breaks, size_t *nbreaks)
{
    *length = 0;
    *nbreaks = 0;
    const char *p = line;
    int any = 0;
    while (*p != '\0' && *p != '#') {
        if ((unsigned char)p[0] == 0xC3 && (unsigned char)p[1] == 0xB7) {          /* ÷ */
            breaks[(*nbreaks)++] = *length;
            p += 2;
            any = 1;
        } else if ((unsigned char)p[0] == 0xC3 && (unsigned char)p[1] == 0x97) {   /* × */
            p += 2;
        } else if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F')) {
            unsigned long cp = strtoul(p, (char **)&p, 16);
            if (cp >= 0x10000) {
                cp -= 0x10000;
                text[(*length)++] = (uint16_t)(0xD800 + (cp >> 10));
                text[(*length)++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
            } else {
                text[(*length)++] = (uint16_t)cp;
            }
        } else {
            p++;
        }
    }
    return any;
}

static void unicode_file(const char *path, intl_segment_kind_t kind, const char *name)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        check(0, path);
        return;
    }
    char line[4096];
    unsigned lines = 0, wrong = 0, number = 0;
    uint16_t text[512];
    size_t breaks[512], got[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        number++;
        size_t length, nbreaks;
        if (!parse(line, text, &length, breaks, &nbreaks)) continue;
        lines++;
        size_t ngot = 0, at = 0;
        got[ngot++] = 0;
        while (at < length) {
            bool like;
            size_t end = intl_segment_next(kind, text, length, at, &like);
            if (end <= at || end > length) break;
            got[ngot++] = end;
            at = end;
        }
        int same = ngot == nbreaks;
        for (size_t i = 0; same && i < ngot; i++) same = got[i] == breaks[i];
        if (!same) {
            if (wrong < 8) printf("%s line %u: %s", name, number, line);
            wrong++;
        }
    }
    fclose(f);
    char what[160];
    snprintf(what, sizeof(what), "%s: every line of Unicode's test (%u lines, %u wrong)", name, lines, wrong);
    check(lines > 0 && wrong == 0, what);
    printf("%s: %u lines\n", name, lines);
}

/* The segments of `ascii` by word, each followed by '+' when word-like and
 * '-' when not, joined by '|'. */
static void words(const char *ascii, char *out, size_t cap)
{
    uint16_t text[128];
    size_t length = strlen(ascii), at = 0, w = 0;
    for (size_t i = 0; i < length; i++) text[i] = (unsigned char)ascii[i];
    out[0] = '\0';
    while (at < length) {
        bool like;
        size_t end = intl_segment_next(INTL_SEGMENT_WORD, text, length, at, &like);
        w += (size_t)snprintf(out + w, cap - w, "%s%.*s%c", at ? "|" : "", (int)(end - at), ascii + at, like ? '+' : '-');
        at = end;
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tools/unicode/segmentation";
    char path[1024];
    snprintf(path, sizeof(path), "%s/GraphemeBreakTest.txt", dir);
    unicode_file(path, INTL_SEGMENT_GRAPHEME, "grapheme");
    snprintf(path, sizeof(path), "%s/WordBreakTest.txt", dir);
    unicode_file(path, INTL_SEGMENT_WORD, "word");
    snprintf(path, sizeof(path), "%s/SentenceBreakTest.txt", dir);
    unicode_file(path, INTL_SEGMENT_SENTENCE, "sentence");

    char got[512];
    words("Hello, world! It's 3.14 o'clock_now.", got, sizeof(got));
    check(!strcmp(got, "Hello+|,-| -|world+|!-| -|It's+| -|3.14+| -|o'clock_now+|.-"),
          "word-likeness: letters and numbers are word-like, spaces and punctuation are not");
    if (strcmp(got, "Hello+|,-| -|world+|!-| -|It's+| -|3.14+| -|o'clock_now+|.-") != 0) printf("  got %s\n", got);

    /* A lone surrogate is a code point of its own, a grapheme by itself
     * (its Grapheme_Cluster_Break is Control). */
    const uint16_t lone[] = {'a', 0xD800, 'b', 0xDC00, 0xD83D, 0xDE00};
    size_t ends[8], n = 0, at = 0;
    while (at < 6 && n < 8) at = ends[n++] = intl_segment_next(INTL_SEGMENT_GRAPHEME, lone, 6, at, NULL);
    check(n == 5 && ends[0] == 1 && ends[1] == 2 && ends[2] == 3 && ends[3] == 4 && ends[4] == 6,
          "lone surrogates are graphemes of their own; a pair is one");
    check(intl_segment_next(INTL_SEGMENT_SENTENCE, lone, 6, 6, NULL) == 6 &&
          intl_segment_next(INTL_SEGMENT_WORD, lone, 0, 0, NULL) == 0, "at or past the end, the end");
    check(!strcmp(intl_segment_unicode(), "17.0.0"), "the table is Unicode 17's");

    /* "a." then 50,000 spaces, or closing parentheses, then "X": SB8 asks
     * at each of them whether a Lower lies ahead, and asking afresh each
     * time reads the run again at every character, many seconds. Read
     * once, it is milliseconds even here, so a second of CPU is the line. */
    enum { RUN = 50000 };
    uint16_t *run = malloc((RUN + 3) * sizeof(*run));
    for (int pass = 0; pass < 2 && run != NULL; pass++) {
        run[0] = 'a';
        run[1] = '.';
        for (size_t i = 0; i < RUN; i++) run[2 + i] = pass ? ')' : ' ';
        run[RUN + 2] = 'X';
        clock_t began = clock();
        size_t first = intl_segment_next(INTL_SEGMENT_SENTENCE, run, RUN + 3, 0, NULL);
        double spent = (double)(clock() - began) / CLOCKS_PER_SEC;
        check(first == RUN + 2 && spent < 1.0, pass ? "a long run of Close after a full stop is read once"
                                                    : "a long run of spaces after a full stop is read once");
    }
    free(run);

    printf("intl segment: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
