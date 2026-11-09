#ifndef WEND_RENDER_H
#define WEND_RENDER_H

// render.h — a parsed page in, the lines a terminal shows out.
//
// THIS HALF OF THE BROWSER TOUCHES NOTHING. A tree, its model and a width in,
// an array of lines and a table of spots out: no handle, no syscall, no byte
// written anywhere. That is what lets tools/test_wend_host.sh run it on the
// host under the sanitizers against the saved corpus, so a change in how a
// page READS arrives as a reviewable diff to that page rather than as "it
// looks different on my screen".
//
// IT DRAWS AND DECIDES NOTHING (LIBPAGE.md). Which form a control belongs to,
// what it holds, whether it is disabled, where a link goes: every one of
// those is asked of libpage's model, and a spot records only WHERE on the
// screen a link or control is and WHICH one it is. A rule decided here as
// well would be the same rule written twice, and the two copies drift.
//
// It is also deliberately throwaway (BROWSER.md § The face): it walks and it
// prints, it does not lay out. What the graphical browser inherits is the
// fetch, the parse, the model and the navigator; its layout engine starts
// from nothing, because a cell grid is a rough draft of the wrong thing.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "html/html.h"
#include "page/page.h"

// EVERY BYTE OF A RENDERED LINE IS ONE PRINTABLE CELL. The fold is what
// guarantees it: a code point the glass has no glyph for becomes `?`, and
// nothing below 0x20, nothing at 0x7F, and nothing in the C1 range 0x80-0x9F
// survives it. So a line's length in BYTES is its width in COLUMNS, which is
// what lets the painter stop and start colour at run boundaries by counting
// bytes, and what lets the wrapper know when a word will not fit.
#define WEND_ATTR_BOLD      0x01u
#define WEND_ATTR_UNDERLINE 0x02u

// Room for the longest replacement the fold spells — `(tm)`, for the
// trademark sign Latin-1 has no room for — and then some, because the table
// is the part of this design most likely to grow. The fold never writes past
// it whatever the table says.
#define WEND_FOLD_MAX 8

#define WEND_TITLE_MAX 256

// A stretch of one line that shares a pen. The painter walks these; the text
// between them is the same bytes, so a line with nothing but prose is one run
// and costs one SGR pair.
typedef struct {
    uint32_t start;      // byte offset into the line's text
    uint32_t len;
    uint8_t  attrs;      // WEND_ATTR_*
    int32_t  spot;       // 0 = ordinary prose, else the 1-based index into spots[]
} wend_run_t;

typedef struct {
    char         *text;  // NUL-terminated; every byte one printable cell
    wend_run_t *runs;
    int32_t       nruns;
    int32_t       len;   // bytes, and therefore cells
} wend_line_t;

// WHAT THE KEYBOARD CAN LAND ON. A page's links and its form controls are
// ONE ordered list, because to a person moving through a page they are one
// thing: the places where pressing Enter does something. Numbering them
// together is what makes "type 3, press Enter" reach a search box as readily
// as a link, and what lets the arrows walk both without knowing which is
// which.
typedef enum {
    WEND_SPOT_LINK = 0,   // an address to go to
    WEND_SPOT_TEXT,       // a box to type in
    WEND_SPOT_CHECK,      // a box to tick
    WEND_SPOT_RADIO,      // one of a group that share a name
    WEND_SPOT_CHOICE,     // a list to pick from
    WEND_SPOT_SUBMIT,     // the control that sends the form
    WEND_SPOT_TOGGLE,     // a `details` summary: opens or closes what it folds
} wend_spot_kind_t;

// A PLACE ON THE SCREEN AND WHAT IT IS IN THE MODEL — nothing else. What a
// link resolves to, what a control holds, which form it belongs to: all of
// it is read from the model at the moment it is wanted, so there is one
// answer to each question and it is libpage's.
typedef struct {
    wend_spot_kind_t kind;
    const os64_html_node_t *node; // the element the spot was drawn for (TOGGLE: the `details`)
    int32_t line;        // the first row it appears on
    int32_t link;        // LINK: index into the model's links
    int32_t control;     // every other kind: index into the model's controls
} wend_spot_t;

// Source-node geometry, rebuilt at each width. The document must outlive
// node lookups. Names and fragment precedence belong to libpage.
typedef struct {
    const os64_html_node_t *node;
    int32_t line;
} wend_node_row_t;

typedef struct {
    wend_line_t   *lines;
    int32_t        nlines;
    wend_spot_t   *spots;
    int32_t        nspots;
    wend_node_row_t *node_rows;
    int32_t        nnode_rows;
    int32_t        cols;              // the width these lines were wrapped at
    char           title[WEND_TITLE_MAX];  // folded; "" when the page has none
    // The renderer ran out of memory partway. What is here is real and is
    // worth showing — a page half-rendered says more than an empty screen —
    // so this is a fact about the lines rather than a failure to return.
    bool           incomplete;
} wend_page_t;

// Render a parsed document at `cols` columns, drawing each link and control
// as `model` says it stands — a person's edits included, since libpage keeps
// them. A link or control the model does not know (NULL model, or one that
// ran out of memory building) is drawn and is not a spot: there is nothing
// behind it to follow or fill in. NULL on no memory, which is different from
// `incomplete` — nothing at all came out.
//
// `flipped` lists the `details` elements the READER has opened or closed:
// each is drawn the other way from what its `open` attribute says. The
// reader's state, not the page's, which is why it is passed in and never
// written to the tree.
wend_page_t *wend_render_html(const os64_html_document_t *doc,
                              const os64_page_t *model, int32_t cols,
                              const os64_html_node_t *const *flipped, int32_t nflipped);

// Whether this `details` is drawn open, given the reader's flips.
bool wend_details_open(const os64_html_node_t *details,
                       const os64_html_node_t *const *flipped, int32_t nflipped);

// Render bytes that are not markup — a text/plain reply, or a message this
// program is writing to itself. Shown as it is, the way `pre` is: line
// breaks kept, tabs to the next stop, hard-wrapped at the margin.
//
// `utf8` says how to read a byte over 0x7F, and it is the reply's own
// answer: the old web's .txt files are not UTF-8, and decoding them as UTF-8
// turns every accented name into a question mark. FALSE MEANS
// WINDOWS-1252, not ISO-8859-1, which is the same answer libhtml gives the
// markup half for every label in that family — a browser whose two halves
// disagreed about one byte would draw a smart quote as `?` in a `.txt` file
// and as `'` in the page that linked to it.
wend_page_t *wend_render_text(const char *text, size_t len, bool utf8, int32_t cols);

void wend_page_free(wend_page_t *page);

// The spot drawn for this model control, or -1 when it has none (hidden,
// disabled, or not drawn at all).
int32_t wend_spot_for_control(const wend_page_t *page, int32_t control);

// The row for this source node, or -1 if absent or rendering was incomplete.
int32_t wend_node_line(const wend_page_t *page, const os64_html_node_t *node);

// ONE CODE POINT ONTO THE GLASS. Writes at most WEND_FOLD_MAX bytes and
// returns how many: 0 for a code point that is invisible by definition (a
// soft hyphen, a zero-width joiner), 1 for most things, more for the few
// that need a word (`...`, `(tm)`). Exposed because the table is the part of
// this file most likely to grow, and a table grows safely with cases.
size_t wend_fold(uint32_t cp, char *out);

#endif // WEND_RENDER_H
