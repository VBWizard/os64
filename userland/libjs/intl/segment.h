#ifndef LIBJS_INTL_SEGMENT_H
#define LIBJS_INTL_SEGMENT_H
/* Text segmentation by UAX #29 (SEGMENTER.md): extended grapheme clusters,
 * default word boundaries and sentence boundaries, over UTF-16 as a JS
 * string holds it. Pure: no engine, no allocation, so the host can hold it
 * to Unicode's own break tests (tools/test_intl_segment_host.sh). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    INTL_SEGMENT_GRAPHEME,
    INTL_SEGMENT_WORD,
    INTL_SEGMENT_SENTENCE
} intl_segment_kind_t;

/* The end of the segment that starts at `at`, which must be a boundary (0,
 * or an end this returned); `length` when `at` is at or past the end. A
 * lone surrogate is a code point of its own. For the word kind, `*word_like`
 * says whether the segment holds a letter or a number (ECMA-402's
 * isWordLike); it is false for the other kinds. */
size_t intl_segment_next(intl_segment_kind_t kind, const uint16_t *text, size_t length, size_t at,
                         bool *word_like);

/* The Unicode version the table was made from. */
const char *intl_segment_unicode(void);

#endif
