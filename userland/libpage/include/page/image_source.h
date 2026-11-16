#ifndef OS64_PAGE_IMAGE_SOURCE_H
#define OS64_PAGE_IMAGE_SOURCE_H
#include <stddef.h>
#include <stdbool.h>

#define OS64_PAGE_SRCSET_BYTES_MAX 65536u
#define OS64_PAGE_SRCSET_CANDIDATES_MAX 128u
typedef enum { OS64_PAGE_SOURCE_EMPTY, OS64_PAGE_SOURCE_OK, OS64_PAGE_SOURCE_LIMIT } os64_page_source_status_t;
typedef struct {
    const char *url;                 // borrowed span in srcset or src; not NUL terminated
    size_t length;
    double density;                  // zero is permitted by the descriptor grammar
} os64_page_source_t;
#pragma GCC visibility push(default)
// Parse HTML srcset descriptors and choose the lowest density >= ratio, or
// the largest available. Equal densities retain source order. source_size
// is a CSS pixel length evaluated by the caller; zero normalizes w to infinity.
os64_page_source_status_t os64_page_source_select(const char *srcset, const char *src,
    double source_size, double ratio, os64_page_source_t *out);
#pragma GCC visibility pop
#endif
