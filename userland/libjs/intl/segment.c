#include "segment.h"

/* UAX #29's three rule sets, each a forward scan from a boundary that keeps
 * the little state its rules look back at, and SB8's look ahead keeps its
 * answer for as far as it holds, so a scan is linear in what it reads.
 * Every rule is a line below with its number; the standard is the
 * specification, and Unicode's *BreakTest.txt files are the proof. */

#include "segment_data.inc"

/* The property values, in tools/intl_unicode.py's order. */
enum { G_OTHER, G_CR, G_LF, G_CONTROL, G_EXTEND, G_ZWJ, G_RI, G_PREPEND, G_SPACINGMARK, G_L, G_V, G_T, G_LV,
       G_LVT };
enum { W_OTHER, W_CR, W_LF, W_NEWLINE, W_EXTEND, W_ZWJ, W_RI, W_FORMAT, W_KATAKANA, W_HEBREW, W_ALETTER,
       W_SQUOTE, W_DQUOTE, W_MIDNUMLET, W_MIDLETTER, W_MIDNUM, W_NUMERIC, W_EXTENDNUMLET, W_WSEGSPACE,
       W_NONE /* no character: before the first, after the last */ };
enum { S_OTHER, S_CR, S_LF, S_EXTEND, S_SEP, S_FORMAT, S_SP, S_LOWER, S_UPPER, S_OLETTER, S_NUMERIC, S_ATERM,
       S_SCONTINUE, S_STERM, S_CLOSE };
enum { I_NONE, I_LINKER, I_CONSONANT, I_EXTEND };

/* The packing, tools/intl_unicode.py's. */
static unsigned gcb(uint32_t p) { return p & 0xF; }
static unsigned wbreak(uint32_t p) { return (p >> 4) & 0x1F; }
static unsigned sbreak(uint32_t p) { return (p >> 9) & 0xF; }
static unsigned incb(uint32_t p) { return (p >> 13) & 0x3; }
static bool pictographic(uint32_t p) { return (p >> 15) & 1; }
static bool wordy(uint32_t p) { return (p >> 16) & 1; }

const char *intl_segment_unicode(void)
{
    return INTL_SEGMENT_UNICODE;
}

/* A code point's properties: the last range starting at or below it. */
static uint32_t props(uint32_t cp)
{
    size_t low = 0, high = INTL_SEGMENT_RANGES;
    while (high - low > 1) {
        size_t mid = low + (high - low) / 2;
        if (kSegmentStarts[mid] <= cp) low = mid;
        else high = mid;
    }
    return kSegmentProps[low];
}

/* The code point at `i` (< length), and where the next one starts. */
static uint32_t decode(const uint16_t *text, size_t length, size_t i, size_t *next)
{
    uint32_t c = text[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < length && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000) {
        *next = i + 2;
        return 0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00u);
    }
    *next = i + 1;
    return c;
}

/* ── Extended grapheme clusters (UAX #29 § 3.1.1) ────────────────────── */
static size_t grapheme_next(const uint16_t *text, size_t length, size_t at)
{
    size_t i;
    uint32_t prev = props(decode(text, length, at, &i));
    unsigned ri = gcb(prev) == G_RI;                  /* RIs ending at prev */
    /* GB11's: 1 after ExtPict Extend*, 2 after ExtPict Extend* ZWJ. */
    unsigned pict = pictographic(prev) ? 1 : 0;
    /* GB9c's: 1 after a Consonant and InCB Extend or Linker, 2 once a Linker
     * is among them. */
    unsigned conjunct = incb(prev) == I_CONSONANT ? 1 : 0;
    while (i < length) {
        size_t after;
        uint32_t cur = props(decode(text, length, i, &after));
        unsigned a = gcb(prev), b = gcb(cur);
        bool joined;
        if (a == G_CR && b == G_LF) joined = true;                                       /* GB3 */
        else if (a == G_CR || a == G_LF || a == G_CONTROL) joined = false;              /* GB4 */
        else if (b == G_CR || b == G_LF || b == G_CONTROL) joined = false;              /* GB5 */
        else if (a == G_L && (b == G_L || b == G_V || b == G_LV || b == G_LVT)) joined = true;   /* GB6 */
        else if ((a == G_LV || a == G_V) && (b == G_V || b == G_T)) joined = true;      /* GB7 */
        else if ((a == G_LVT || a == G_T) && b == G_T) joined = true;                   /* GB8 */
        else if (b == G_EXTEND || b == G_ZWJ) joined = true;                            /* GB9 */
        else if (b == G_SPACINGMARK) joined = true;                                     /* GB9a */
        else if (a == G_PREPEND) joined = true;                                         /* GB9b */
        else if (conjunct == 2 && incb(cur) == I_CONSONANT) joined = true;              /* GB9c */
        else if (pict == 2 && pictographic(cur)) joined = true;                         /* GB11 */
        else if (a == G_RI && b == G_RI && ri % 2 == 1) joined = true;                  /* GB12, GB13 */
        else joined = false;                                                            /* GB999 */
        if (!joined) return i;
        ri = b == G_RI ? ri + 1 : 0;
        if (pictographic(cur)) pict = 1;
        else if (pict == 1 && b == G_EXTEND) pict = 1;
        else if (pict == 1 && b == G_ZWJ) pict = 2;
        else pict = 0;
        unsigned c = incb(cur);
        if (c == I_CONSONANT) conjunct = 1;
        else if (conjunct != 0 && c == I_LINKER) conjunct = 2;
        else if (conjunct == 0 || c != I_EXTEND) conjunct = 0;
        prev = cur;
        i = after;
    }
    return length;
}

/* ── Word boundaries (UAX #29 § 4.1.1) ───────────────────────────────── */
static bool word_ignored(unsigned w) { return w == W_EXTEND || w == W_FORMAT || w == W_ZWJ; }
static bool ahletter(unsigned w) { return w == W_ALETTER || w == W_HEBREW; }
static bool midnumletq(unsigned w) { return w == W_MIDNUMLET || w == W_SQUOTE; }

/* The Word_Break of the first character at or after `i` that WB4 does not
 * absorb, W_NONE at the end. */
static unsigned word_ahead(const uint16_t *text, size_t length, size_t i)
{
    while (i < length) {
        size_t next;
        unsigned w = wbreak(props(decode(text, length, i, &next)));
        if (!word_ignored(w)) return w;
        i = next;
    }
    return W_NONE;
}

static size_t word_next(const uint16_t *text, size_t length, size_t at, bool *word_like)
{
    size_t i;
    uint32_t first = props(decode(text, length, at, &i));
    unsigned a = wbreak(first);                /* the previous character's, as it is */
    unsigned last = a, before = W_NONE;        /* the two before, as WB4 leaves them */
    unsigned ri = a == W_RI;
    *word_like = wordy(first);
    while (i < length) {
        size_t after;
        uint32_t cur = props(decode(text, length, i, &after));
        unsigned b = wbreak(cur);
        bool joined, absorbed = false;
        if (a == W_CR && b == W_LF) joined = true;                                            /* WB3 */
        else if (a == W_CR || a == W_LF || a == W_NEWLINE) joined = false;                   /* WB3a */
        else if (b == W_CR || b == W_LF || b == W_NEWLINE) joined = false;                   /* WB3b */
        else if (a == W_ZWJ && pictographic(cur)) joined = true;                             /* WB3c */
        else if (a == W_WSEGSPACE && b == W_WSEGSPACE) joined = true;                        /* WB3d */
        else if (word_ignored(b)) joined = absorbed = true;                                  /* WB4 */
        else if (ahletter(last) && ahletter(b)) joined = true;                               /* WB5 */
        else if (ahletter(last) && (b == W_MIDLETTER || midnumletq(b)) &&
                 ahletter(word_ahead(text, length, after))) joined = true;                   /* WB6 */
        else if (ahletter(before) && (last == W_MIDLETTER || midnumletq(last)) && ahletter(b))
            joined = true;                                                                   /* WB7 */
        else if (last == W_HEBREW && b == W_SQUOTE) joined = true;                           /* WB7a */
        else if (last == W_HEBREW && b == W_DQUOTE && word_ahead(text, length, after) == W_HEBREW)
            joined = true;                                                                   /* WB7b */
        else if (before == W_HEBREW && last == W_DQUOTE && b == W_HEBREW) joined = true;     /* WB7c */
        else if (last == W_NUMERIC && b == W_NUMERIC) joined = true;                         /* WB8 */
        else if (ahletter(last) && b == W_NUMERIC) joined = true;                            /* WB9 */
        else if (last == W_NUMERIC && ahletter(b)) joined = true;                            /* WB10 */
        else if (before == W_NUMERIC && (last == W_MIDNUM || midnumletq(last)) && b == W_NUMERIC)
            joined = true;                                                                   /* WB11 */
        else if (last == W_NUMERIC && (b == W_MIDNUM || midnumletq(b)) &&
                 word_ahead(text, length, after) == W_NUMERIC) joined = true;                /* WB12 */
        else if (last == W_KATAKANA && b == W_KATAKANA) joined = true;                       /* WB13 */
        else if ((ahletter(last) || last == W_NUMERIC || last == W_KATAKANA || last == W_EXTENDNUMLET) &&
                 b == W_EXTENDNUMLET) joined = true;                                         /* WB13a */
        else if (last == W_EXTENDNUMLET && (ahletter(b) || b == W_NUMERIC || b == W_KATAKANA))
            joined = true;                                                                   /* WB13b */
        else if (last == W_RI && b == W_RI && ri % 2 == 1) joined = true;                    /* WB15, WB16 */
        else joined = false;                                                                 /* WB999 */
        if (!joined) return i;
        *word_like |= wordy(cur);
        if (!absorbed) {
            ri = b == W_RI ? ri + 1 : 0;
            before = last;
            last = b;
        }
        a = b;
        i = after;
    }
    return length;
}

/* ── Sentence boundaries (UAX #29 § 5.1.1) ───────────────────────────── */
static bool sentence_ignored(unsigned s) { return s == S_EXTEND || s == S_FORMAT; }
static bool parasep(unsigned s) { return s == S_SEP || s == S_CR || s == S_LF; }
static bool saterm(unsigned s) { return s == S_STERM || s == S_ATERM; }

/* SB8's look ahead from `i`: past every character that is not OLetter,
 * Upper, Lower, ParaSep or SATerm (SB5's ignored ones too), whether the
 * first that is, is Lower. `*decided` is where that first one starts
 * (`length` for none): the answer is the same from anywhere up to there,
 * which is what keeps a long run of spaces after a full stop from being
 * read again at every space. */
static bool sentence_lower_ahead(const uint16_t *text, size_t length, size_t i, size_t *decided)
{
    while (i < length) {
        size_t next;
        unsigned s = sbreak(props(decode(text, length, i, &next)));
        *decided = i;
        if (s == S_LOWER) return true;
        if (s == S_OLETTER || s == S_UPPER || parasep(s) || saterm(s)) return false;
        i = next;
    }
    *decided = length;
    return false;
}

static size_t sentence_next(const uint16_t *text, size_t length, size_t at)
{
    size_t i;
    unsigned a = sbreak(props(decode(text, length, at, &i)));   /* the previous character's, as it is */
    unsigned last = a;                                           /* as SB5 leaves it */
    /* The SATerm that opens a run of SATerm Close* Sp* ending at `last`, or
     * S_OTHER for none; `spaced` once an Sp is in that run; `ahead_of_term`,
     * the character before an ATerm (SB7). */
    unsigned term = saterm(a) ? a : S_OTHER, ahead_of_term = S_OTHER;
    bool spaced = false;
    /* SB8's last look ahead: its answer holds for every position up to
     * `lower_decided`, and SIZE_MAX means none was made yet. */
    bool lower_ahead = false;
    size_t lower_decided = SIZE_MAX;
    while (i < length) {
        size_t after;
        unsigned b = sbreak(props(decode(text, length, i, &after)));
        bool joined, absorbed = false;
        if (a == S_CR && b == S_LF) joined = true;                                                /* SB3 */
        else if (parasep(a)) joined = false;                                                     /* SB4 */
        else if (sentence_ignored(b)) joined = absorbed = true;                                  /* SB5 */
        else if (last == S_ATERM && b == S_NUMERIC) joined = true;                               /* SB6 */
        else if (last == S_ATERM && (ahead_of_term == S_UPPER || ahead_of_term == S_LOWER) && b == S_UPPER)
            joined = true;                                                                       /* SB7 */
        else if (term == S_ATERM && (lower_decided != SIZE_MAX && i <= lower_decided
                                         ? lower_ahead
                                         : (lower_ahead = sentence_lower_ahead(text, length, i, &lower_decided))))
            joined = true;                                                                       /* SB8 */
        else if (term != S_OTHER && (b == S_SCONTINUE || saterm(b))) joined = true;              /* SB8a */
        else if (term != S_OTHER && !spaced && (b == S_CLOSE || b == S_SP || parasep(b))) joined = true;   /* SB9 */
        else if (term != S_OTHER && (b == S_SP || parasep(b))) joined = true;                    /* SB10 */
        else if (term != S_OTHER) joined = false;                                                /* SB11 */
        else joined = true;                                                                      /* SB998 */
        if (!joined) return i;
        if (!absorbed) {
            if (saterm(b)) {
                ahead_of_term = last;
                term = b;
                spaced = false;
            } else if (term != S_OTHER && b == S_CLOSE && !spaced) {
                /* still SATerm Close* */
            } else if (term != S_OTHER && b == S_SP) {
                spaced = true;
            } else {
                term = S_OTHER;
            }
            last = b;
        }
        a = b;
        i = after;
    }
    return length;
}

size_t intl_segment_next(intl_segment_kind_t kind, const uint16_t *text, size_t length, size_t at,
                         bool *word_like)
{
    bool like = false;
    size_t end = length;
    if (at < length) {
        if (kind == INTL_SEGMENT_WORD) end = word_next(text, length, at, &like);
        else if (kind == INTL_SEGMENT_SENTENCE) end = sentence_next(text, length, at);
        else end = grapheme_next(text, length, at);
    }
    if (word_like != NULL) *word_like = like;
    return end;
}
