#ifndef WEND_RENDER_H
#define WEND_RENDER_H

// render.h — a parsed page in, the lines a terminal shows out.
//
// THIS HALF OF THE BROWSER TOUCHES NOTHING. A tree and a width in, an array
// of lines and a table of links out: no handle, no syscall, no byte written
// anywhere. That is what lets tools/test_wend_host.sh run it on the host
// under the sanitizers against the saved corpus, so a change in how a page
// READS arrives as a reviewable diff to that page rather than as "it looks
// different on my screen".
//
// It is also deliberately throwaway (BROWSER.md § The face): it walks and it
// prints, it does not lay out. What the graphical browser inherits is the
// fetch, the parse and the navigator; its layout engine starts from nothing,
// because a cell grid is a rough draft of the wrong thing.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "html/html.h"
#include "os64/url.h"

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
} wend_spot_kind_t;

// ONE ENTRY OF A LIST. A DISABLED option is still SHOWN when the page marks
// it — that is what a "choose one" placeholder is — but it is never cycled
// onto and never sent, because disabled means the page took it away.
typedef struct {
    char *shown;         // the words
    char *value;         // what picking it sends
    bool  off;
} wend_option_t;

typedef struct {
    wend_spot_kind_t kind;
    int32_t line;        // the first row it appears on
    int32_t form;        // 1-based into forms[]; 0 = in no form at all
    char   *url;         // LINK: resolved; "" when the href will not resolve
    // LINK: the `#name` the href asked for, without its `#`, or "". Kept
    // SEPARATELY because a fragment never crosses the wire — the resolver
    // drops it for that reason — and yet it is the whole meaning of a
    // table-of-contents link, which the navigator answers by moving rather
    // than by fetching.
    char   *fragment;
    // ...and whether the href ASKED for one at all. `#` with nothing after
    // it is the document's top, which is a move; no `#` is an address,
    // which is a fetch. The fragment text cannot tell those apart.
    bool    has_fragment;
    char   *name;        // a control's name; "" is a control nothing sends
    char   *value;       // what it holds NOW — typed text, or the value attribute
    char   *label;       // SUBMIT: the words on it
    // SUBMIT: what this particular button overrules about its form. The
    // standard lets the button carry its own action and method, and the
    // method matters to a browser that sends only one of them: a GET form
    // with a `formmethod=post` button is a POST, and sending it as a GET
    // would put a password in an address. Its action carries a `#name` of
    // its own for the same reason a link's does.
    char   *form_action; // "" = the form's own
    char   *form_fragment;
    bool    has_method;  // whether `post` below means anything
    bool    post;
    // SUBMIT: an image button, which does not send its value. What it sends
    // is where you clicked, and a keyboard's answer to that is the origin.
    bool    image;
    bool    on;          // CHECK / RADIO: ticked
    bool    secret;      // TEXT: a password — never drawn, never echoed
    wend_option_t *options;   // CHOICE: what it offers
    int32_t noptions, chosen, optioncap;
    int32_t width;       // TEXT: how many cells the box is drawn as
    // TEXT: the page keeps this value fixed. Unlike a disabled control it IS
    // sent — readonly is about who may change it, not about whether it
    // counts — so it stays a spot you can land on and cannot type into.
    bool    readonly;
} wend_spot_t;

// A FORM IS ITS DESTINATION AND HOW IT INSISTS ON GETTING THERE. `post` is
// not something this browser can do — libfetch sends no request body — and
// naming that beats sending the wrong thing to somebody's server.
typedef struct {
    char *action;        // resolved absolute address; "" = the page it is on
    char *fragment;      // the `#name` the action asked for, or ""
    bool  post;
    // The fields the form carries and never shows. They are the form's own
    // data rather than places a person can land, so they live here instead
    // of in the spot list, where they would take numbers nothing draws.
    char **hidden_names, **hidden_values;
    // ...and WHERE each one stood, as the number of spots that existed when
    // it was met. The form data set goes out in TREE ORDER, and a hidden
    // field is the one part of a form that is not a spot, so without this
    // the query would put every hidden field first whatever the page said.
    int32_t *hidden_after;
    int32_t nhidden, hiddencap, hiddenvalcap, hiddenaftercap;
} wend_form_t;

// WHERE A `#name` LANDS. Every element carrying an `id`, and every old-style
// `<a name>`, records the row it fell on — so a link into the same page is
// answered by scrolling to the row rather than by fetching the document
// again and showing its top, which is what a table of contents on a long
// article is entirely made of.
typedef struct {
    char   *name;
    int32_t line;
} wend_anchor_t;

typedef struct {
    wend_line_t   *lines;
    int32_t        nlines;
    wend_spot_t   *spots;
    int32_t        nspots;
    wend_anchor_t *anchors;
    int32_t        nanchors;
    wend_form_t   *forms;
    int32_t        nforms;
    int32_t        cols;              // the width these lines were wrapped at
    char           title[WEND_TITLE_MAX];  // folded; "" when the page has none
    // The renderer ran out of memory partway. What is here is real and is
    // worth showing — a page half-rendered says more than an empty screen —
    // so this is a fact about the lines rather than a failure to return.
    bool           incomplete;
} wend_page_t;

// WHAT A PERSON HAS FILLED IN, kept outside the page because the page is
// thrown away and rebuilt whenever the window changes width. The tree does
// not change under a re-wrap, so the walk numbers the spots identically and
// an edit belongs to the spot at the same index. Anything left at its
// "untouched" value takes what the page itself said.
typedef struct {
    char   *text;      // TEXT: the typed value; NULL = the page's own
    int8_t  on;        // CHECK / RADIO: -1 untouched, 0 clear, 1 ticked
    int32_t chosen;    // CHOICE: the option index; -1 untouched
} wend_edit_t;

// Render a parsed document at `cols` columns. `base` is the address every
// href resolves against: the page's own final URL, unless it carries a
// <base href>, which wins (wend_base_href finds it). `edits` may be NULL,
// and is read only for the spots it has room for. NULL on no memory, which
// is different from `incomplete` — nothing at all came out.
wend_page_t *wend_render_html(const os64_html_document_t *doc,
                              const os64_url_t *base, int32_t cols,
                              const wend_edit_t *edits, int32_t nedits);

// Render bytes that are not markup — a text/plain reply, or a message this
// program is writing to itself. Shown as it is, the way `pre` is: line
// breaks kept, tabs to the next stop, hard-wrapped at the margin. `utf8`
// says how to read a byte over 0x7F, and it is the reply's own answer: the
// old web's .txt files are Latin-1 and decoding them as UTF-8 turns every
// accented name into a question mark.
wend_page_t *wend_render_text(const char *text, size_t len, bool utf8, int32_t cols);

void wend_page_free(wend_page_t *page);

// ── Sending a form ──────────────────────────────────────────────────────

typedef enum {
    WEND_FORM_OK = 0,
    WEND_FORM_NONE,      // that control belongs to no form
    WEND_FORM_POST,      // it posts, and this browser asks only by address
    WEND_FORM_TOO_LONG,  // what it would send is longer than an address may be
} wend_form_result_t;

// The address that activating `spot` asks for: the form's action with a
// query built from every successful control in it, percent-encoded the way a
// form encodes. `page_url` is where the page came from, used when the form
// names no action of its own. `fragment` (optional, at least
// OS64_URL_PATH_MAX) takes the `#name` the action asked for, which the
// address itself cannot carry. Pure computation, so the harness can check
// what would go on the wire without a wire.
wend_form_result_t wend_form_url(const wend_page_t *page, int32_t spot,
                                 const char *page_url, char *out, size_t cap,
                                 char *fragment, size_t fragment_cap);

// The page's own idea of where it lives. False when it has no <base href>,
// or the element carries no usable href.
bool wend_base_href(const os64_html_document_t *doc, char *out, size_t cap);

// The row a `#name` names, or -1 when the page has no such anchor.
int32_t wend_anchor_line(const wend_page_t *page, const char *name);

// ONE CODE POINT ONTO THE GLASS. Writes at most WEND_FOLD_MAX bytes and
// returns how many: 0 for a code point that is invisible by definition (a
// soft hyphen, a zero-width joiner), 1 for most things, more for the few
// that need a word (`...`, `(tm)`). Exposed because the table is the part of
// this file most likely to grow, and a table grows safely with cases.
size_t wend_fold(uint32_t cp, char *out);

#endif // WEND_RENDER_H
