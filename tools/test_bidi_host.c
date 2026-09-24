// test_bidi_host.c — os64_bidi_strong/os64_bidi_first_strong against the
// Unicode standard, and the whole code space digested so the harness can
// compare it with an independent read of the pinned data file.
//
// The literal cases below are transcribed from the standard and NOT read out
// of the table under test, which is the only way a test can catch a table
// that is wrong. They include the cases where DerivedBidiClass.txt departs
// from its own "everything unlisted is left to right" default: an UNASSIGNED
// code point inside a right-to-left block is R or AL, and a browser that
// read it as L would point a line of Hebrew the wrong way.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "os64/str.h"

static int checks, failures;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
        }                                                                                          \
    } while (0)

static const char *name(os64_bidi_strong_t k)
{
    return k == OS64_BIDI_L ? "L" : k == OS64_BIDI_R ? "R" : k == OS64_BIDI_AL ? "AL" : "not-strong";
}

static void one(uint32_t cp, os64_bidi_strong_t want)
{
    os64_bidi_strong_t got = os64_bidi_strong(cp);
    checks++;
    if (got != want) {
        failures++;
        fprintf(stderr, "FAIL U+%04X is %s, want %s\n", cp, name(got), name(want));
    }
}

static void run(const char *what, const char *utf8, size_t len, os64_bidi_strong_t want)
{
    os64_bidi_strong_t got = os64_bidi_first_strong(utf8, len);
    checks++;
    if (got != want) {
        failures++;
        fprintf(stderr, "FAIL %s is %s, want %s\n", what, name(got), name(want));
    }
}
#define RUN(text, want) run(#text, text, strlen(text), want)

// The letters the cases are written with. Latin-1 would do for none of them.
#define ALEF     "\xd7\x90"          // U+05D0 HEBREW LETTER ALEF, class R
#define ARABIC   "\xd8\xa7"          // U+0627 ARABIC LETTER ALEF, class AL
#define HIRAGANA "\xe3\x81\x82"      // U+3042 HIRAGANA LETTER A, class L
#define ARABIC_0 "\xd9\xa0"          // U+0660 ARABIC-INDIC DIGIT ZERO, class AN

static void classes(void)
{
    // Left to right: a letter, the mark that exists to say so, and a script
    // that is neither Latin nor right to left.
    one(0x0041, OS64_BIDI_L);
    one(0x200E, OS64_BIDI_L);
    one(0x3042, OS64_BIDI_L);

    // Right to left: Hebrew's letters and its punctuation, N'Ko, the
    // presentation forms, a plane-1 script, and the mark.
    one(0x05BE, OS64_BIDI_R);
    one(0x05D0, OS64_BIDI_R);
    one(0x07C0, OS64_BIDI_R);
    one(0x07CA, OS64_BIDI_R);
    one(0x200F, OS64_BIDI_R);
    one(0xFB1D, OS64_BIDI_R);
    one(0x10800, OS64_BIDI_R);

    // Arabic letters, which are right to left and shaped besides.
    one(0x0627, OS64_BIDI_AL);
    one(0x0860, OS64_BIDI_AL);
    one(0xFB50, OS64_BIDI_AL);
    one(0x1EE00, OS64_BIDI_AL);

    // UNASSIGNED code points inside a right-to-left block, which the data
    // file gives R or AL through its @missing lines rather than leaving to
    // the global left-to-right default. The block edges are the interesting
    // part: 0590..05FF is R, 0600..07BF is AL, 07C0..085F is R.
    one(0x0590, OS64_BIDI_R);
    one(0x05FF, OS64_BIDI_R);
    one(0x07BF, OS64_BIDI_AL);
    one(0x085F, OS64_BIDI_R);

    one(0x086F, OS64_BIDI_AL);           // unassigned inside 0860..08FF

    // The code point either side of a block edge, which is where a table of
    // ranges goes wrong: 058F is outside the Hebrew default and ordinary,
    // 0870 is the first ASSIGNED letter of the second Arabic default, and
    // FDD0 is a noncharacter sitting inside an Arabic block.
    one(0x058F, OS64_BIDI_NOT_STRONG);   // ARMENIAN DRAM SIGN, class ET
    one(0x0870, OS64_BIDI_AL);
    one(0x08FF, OS64_BIDI_NOT_STRONG);   // an assigned combining mark, NSM
    one(0xFDD0, OS64_BIDI_NOT_STRONG);
    one(0xFDF0, OS64_BIDI_AL);

    // Nothing else decides a direction: digits of either kind, a space, a
    // separator, punctuation, a combining mark, an invisible, a symbol, a
    // noncharacter, and the replacement character a broken byte decodes to.
    one(0x0030, OS64_BIDI_NOT_STRONG);   // DIGIT ZERO, class EN
    one(0x0660, OS64_BIDI_NOT_STRONG);   // ARABIC-INDIC DIGIT ZERO, class AN
    one(0x0600, OS64_BIDI_NOT_STRONG);   // ARABIC NUMBER SIGN is AN, not AL
    one(0x0020, OS64_BIDI_NOT_STRONG);
    one(0x2028, OS64_BIDI_NOT_STRONG);
    one(0x002C, OS64_BIDI_NOT_STRONG);
    one(0x0300, OS64_BIDI_NOT_STRONG);
    one(0x200B, OS64_BIDI_NOT_STRONG);
    one(0x20AC, OS64_BIDI_NOT_STRONG);   // EURO SIGN, class ET
    one(0x1F600, OS64_BIDI_NOT_STRONG);
    one(0xFFFD, OS64_BIDI_NOT_STRONG);
    one(0x10FFFE, OS64_BIDI_NOT_STRONG);
    one(0x10FFFF, OS64_BIDI_NOT_STRONG);
}

static void first_strong(void)
{
    // Nothing strong at all is its own answer, and not "left to right": the
    // caller's own rule decides what an undecided string means.
    CHECK(os64_bidi_first_strong(NULL, 0) == OS64_BIDI_NOT_STRONG);
    CHECK(os64_bidi_first_strong(NULL, 8) == OS64_BIDI_NOT_STRONG);
    RUN("", OS64_BIDI_NOT_STRONG);
    RUN("123 .,-", OS64_BIDI_NOT_STRONG);
    RUN(ARABIC_0 " 45", OS64_BIDI_NOT_STRONG);

    RUN("Hello", OS64_BIDI_L);
    RUN(HIRAGANA, OS64_BIDI_L);
    RUN(ALEF, OS64_BIDI_R);
    RUN(ARABIC, OS64_BIDI_AL);

    // THE FIRST STRONG CHARACTER DECIDES AND THE REST DO NOT GET A VOTE,
    // which is the whole of the rule and the thing a naive scan for "is
    // there any Hebrew in here" gets wrong.
    RUN("A" ALEF, OS64_BIDI_L);
    RUN(ALEF "A", OS64_BIDI_R);
    RUN("A" ARABIC, OS64_BIDI_L);
    RUN(ARABIC "A", OS64_BIDI_AL);

    // What comes BEFORE the first strong character is skipped however much
    // of it there is: a search box holding a phone number and then a name.
    RUN("+1 (555) 0100 " ALEF, OS64_BIDI_R);
    RUN("\"'<>[]{} \t\n" ALEF, OS64_BIDI_R);

    // A byte that is not UTF-8 decodes to the replacement character, which
    // is not strong, so a corrupt string never decides a direction.
    RUN("\xff\xfe" ALEF, OS64_BIDI_R);
    RUN("\xff\xfe", OS64_BIDI_NOT_STRONG);
    // A sequence cut short by `len` is the same: one replacement, and the
    // walk ends where the bytes do.
    run("truncated alef", ALEF, 1, OS64_BIDI_NOT_STRONG);
    run("alef then a cut arabic", ALEF ARABIC, 3, OS64_BIDI_R);

    // A NUL is a byte like any other here: the length says where the text
    // ends, because a form value may hold one.
    run("nul then alef", "\0" ALEF, 3, OS64_BIDI_R);
}

// One byte per code point, folded into a hash the harness compares against
// its own reading of the pinned data file. The literal cases above prove the
// table says the right thing where the standard is quotable; this proves it
// says the right thing EVERYWHERE, including at every range edge the
// generator emitted.
static void digest(void)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    size_t counts[4] = {0};
    for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
        unsigned char k = (unsigned char)os64_bidi_strong(cp);
        counts[k & 3]++;
        hash ^= k;
        hash *= 0x100000001b3ull;
    }
    printf("bidi digest %016llx L=%zu R=%zu AL=%zu not-strong=%zu\n", (unsigned long long)hash,
           counts[OS64_BIDI_L], counts[OS64_BIDI_R], counts[OS64_BIDI_AL],
           counts[OS64_BIDI_NOT_STRONG]);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--digest") == 0) {
        digest();
        return 0;
    }
    classes();
    first_strong();
    printf("bidi checks: %d run, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
