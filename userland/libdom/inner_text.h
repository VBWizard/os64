#ifndef LIBDOM_INNER_TEXT_H
#define LIBDOM_INNER_TEXT_H

/* innerText's walk (HTML § 3.2.7, the rendered text collection steps),
 * pure: libhtml's tree, a provider's four facts per element, and memory
 * from the caller, with no engine. WHEEL_FILELIST_INNERTEXT.md § 3 is the
 * design: the first cut takes each element's facts from its style and
 * handles white space itself, where the full algorithm would take the text
 * layout made. tools/test_inner_text_host.sh drives it. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <html/html.h>
#include <dom/text_style.h>

/* The caller's memory: `grow` returns a block of `size` bytes holding the
 * first `old_size` bytes of `old` (NULL: a new block), or NULL with `old`
 * untouched; `release` frees a block grow returned (NULL: nothing). */
typedef struct {
    void *(*grow)(void *ctx, void *old, size_t old_size, size_t size);
    void (*release)(void *ctx, void *block);
    void *ctx;
} d_inner_text_memory_t;

typedef enum {
    D_INNER_TEXT_OK = 0,
    /* The root is not being rendered, or no facts are to be had: the
     * answer is its textContent (the standard's, for a root not rendered
     * and for a browser without CSS). */
    D_INNER_TEXT_NOT_RENDERED,
    D_INNER_TEXT_NO_MEMORY,
} d_inner_text_status_t;

/* `root`'s innerText, in `doc`, as UTF-8 in a block from `memory` (`*out`,
 * `*length` bytes and a NUL after them), on OK only. The walk climbs the
 * tree by its parent links and keeps no stack, so a tree of any depth costs
 * the same memory. `facts` is asked once with no element, then about each
 * element (dom/text_style.h); a false from it is NOT_RENDERED for the whole
 * read. */
d_inner_text_status_t d_inner_text(const os64_html_document_t *doc, const os64_html_node_t *root,
                                   os64_dom_text_style_provider_t facts, void *opaque,
                                   const d_inner_text_memory_t *memory, char **out, size_t *length);

#endif
