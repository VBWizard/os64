#ifndef DOM_TEXT_STYLE_H
#define DOM_TEXT_STYLE_H

/* What innerText asks of an element's style (HTML § 3.2.7), four facts and
 * no more: dom.h's os64_dom_set_text_style installs the host's answer. Its
 * own header, with no engine in it, so libdom's walk (inner_text.c) can be
 * driven on the host by a provider of the test's own. */

#include <stdbool.h>
#include <stdint.h>

#include <html/html.h>

/* An element's `display`, as innerText reads it. BLOCK is any block-level
 * box that is not a table's: block, flow-root, list-item, flex and grid.
 * TABLE is a table's box. TABLE_PART is a row group, a column or a column
 * group. INLINE is every other inline-level box: inline, and the atomic
 * inline-block, inline-flex and inline-grid. */
typedef enum {
    OS64_DOM_TEXT_INLINE = 0,
    OS64_DOM_TEXT_BLOCK,
    OS64_DOM_TEXT_TABLE,
    OS64_DOM_TEXT_TABLE_PART,
    OS64_DOM_TEXT_TABLE_ROW,
    OS64_DOM_TEXT_TABLE_CELL,
    OS64_DOM_TEXT_TABLE_CAPTION,
    OS64_DOM_TEXT_CONTENTS,
} os64_dom_text_display_t;

/* How its text's white space is handled: COLLAPSE is `normal` and
 * `nowrap`; PRESERVE is `pre`, `pre-wrap` and `break-spaces`. */
typedef enum {
    OS64_DOM_SPACE_COLLAPSE = 0,
    OS64_DOM_SPACE_PRESERVE,
    OS64_DOM_SPACE_PRE_LINE,
} os64_dom_text_space_t;

/* `rendered` is HTML's "being rendered": the element is styled, and neither
 * it nor an ancestor is `display: none` (`display: contents` is rendered:
 * its children have boxes). `visible` is `visibility: visible`. The other
 * two are read only when it is rendered. */
typedef struct {
    bool rendered;
    bool visible;
    uint8_t display;            /* os64_dom_text_display_t */
    uint8_t white_space;        /* os64_dom_text_space_t */
} os64_dom_text_style_t;

/* A read of innerText asks first with `element` NULL: the host makes its
 * style current (a layout, when it is stale) and answers whether it could;
 * `out` is not read. Then it asks about each element it passes, with no
 * script run between, so the host may answer from what that first call
 * made current: it fills `out` and answers true. Any false makes innerText
 * answer textContent. Must not enter JS or pump events. Borrowed nodes
 * expire on return. */
typedef bool (*os64_dom_text_style_provider_t)(void *opaque, const os64_html_node_t *element,
                                               os64_dom_text_style_t *out);

#endif
