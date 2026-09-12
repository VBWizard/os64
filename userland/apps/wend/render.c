// render.c — the tree walked, the words wrapped, the page turned into lines.
//
// THE SHAPE OF THE WALK. One pass, depth first, with a PEN (bold, underline,
// which link or form control we are inside) that elements push and pop, and
// a WORD held back until whitespace ends it. Holding the word back is what
// makes wrapping work: a row is committed only when the next word will not
// fit, so no word is ever split except one that could not have fitted a row.
//
// Recursion is bounded by libhtml's max_depth (512 by default, a refusal by
// name past that) and a frame here is small, so a page cannot spend this
// program's megabyte of stack by nesting divs.
//
// WHAT THIS FILE MAY NOT DO: touch a handle, read the clock, print. Its only
// outside world is the allocator, the UTF-8 decoder and the URL resolver,
// which is what lets the host harness run every corpus page through it under
// the sanitizers.

#include "render.h"

#include "html/tags.h"
#include "os64/mem.h"
#include "os64/str.h"

// ── The fold: a code point onto a Latin-1 glass ─────────────────────────
//
// The tree is UTF-8 and the terminal draws Latin-1, so something has to
// decide what a curly quote becomes. The rule: a code point below 256 is its
// own byte, the punctuation the web is full of gets a byte that READS right,
// and everything else is `?`.
//
// `?` and never a blank. A blank says nothing was there; the mark says
// something was there and this program could not draw it, which is both the
// truth and the thing a person needs in order to know whether they are
// missing anything.
//
// The exceptions to "below 256 is its byte" are the code points that are not
// characters in the drawing sense. The C0 and C1 control ranges are a
// stranger steering the terminal rather than writing on it. The invisibles —
// a soft hyphen, a zero-width joiner — mark where a word MAY break, and
// drawing the soft hyphen as the dash Latin-1 keeps at 0xAD would put a
// hyphen in the middle of a word that has none.
static const struct { uint32_t cp; const char *as; } s_fold[] = {
    // The quotation marks a word processor produces
    { 0x2018, "'" }, { 0x2019, "'" }, { 0x201A, "'" }, { 0x201B, "'" },
    { 0x201C, "\"" }, { 0x201D, "\"" }, { 0x201E, "\"" }, { 0x201F, "\"" },
    { 0x2039, "<" }, { 0x203A, ">" },
    { 0x2032, "'" }, { 0x2033, "\"" },
    // Every dash that is not the hyphen-minus
    { 0x2010, "-" }, { 0x2011, "-" }, { 0x2012, "-" }, { 0x2013, "-" },
    { 0x2014, "-" }, { 0x2015, "-" }, { 0x2212, "-" },
    { 0x2026, "..." },
    // A space that must neither collapse nor break a line. It folds AFTER
    // the collapser has had its say, so it survives where an ordinary space
    // would have been eaten.
    { 0x00A0, " " },
    // Bullets, for the lists a page draws by hand
    { 0x2022, "*" }, { 0x2023, "*" }, { 0x25AA, "*" }, { 0x25CF, "*" },
    { 0x25E6, "*" }, { 0x2605, "*" }, { 0x2606, "*" },
    // Arrows point the way they point
    { 0x2190, "<" }, { 0x2192, ">" }, { 0x2191, "^" }, { 0x2193, "v" },
    { 0x21D0, "<" }, { 0x21D2, ">" },
    { 0x2122, "(tm)" },        // Latin-1 keeps (c) and (r); this one it lacks
    { 0x2044, "/" },
    // Invisible by definition: a break opportunity, not a character
    { 0x00AD, "" }, { 0x200B, "" }, { 0x200C, "" }, { 0x200D, "" },
    { 0x2060, "" }, { 0xFEFF, "" },
    // "New line" as a document with no markup spells it
    { 0x2028, " " }, { 0x2029, " " },
};

size_t wend_fold(uint32_t cp, char *out)
{
    for (size_t i = 0; i < sizeof(s_fold) / sizeof(s_fold[0]); i++)
        if (s_fold[i].cp == cp) {
            size_t n = os64_strlen(s_fold[i].as);
            if (n > WEND_FOLD_MAX)
                n = WEND_FOLD_MAX;
            os64_memcpy(out, s_fold[i].as, n);
            return n;
        }
    // The printable halves of Latin-1 and nothing else: the gaps are the two
    // control ranges, 0x00-0x1F and 0x7F-0x9F.
    if ((cp >= 0x20 && cp < 0x7F) || (cp >= 0xA0 && cp < 0x100)) {
        out[0] = (char)cp;
        return 1;
    }
    out[0] = '?';
    return 1;
}

// A number as its digits, for the `[n]` a link wears and the counter an
// ordered list keeps. Local because this file has no business pulling in a
// formatter for two call sites, and because the host harness links it alone.
static size_t num_text(int32_t v, char *out)
{
    if (v <= 0) {
        out[0] = '0';
        return 1;
    }
    char tmp[12];
    size_t n = 0;
    while (v > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (size_t i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

// A PAGE THAT RAN OUT OF MEMORY PARTWAY CAN BE MISSING A STRING. Its
// `incomplete` flag says so and what is there is still worth showing, so
// every reader below treats an absent string as an empty one rather than
// trusting a pointer the renderer never got to fill.
static const char *some(const char *s) { return s ? s : ""; }

// ── Growing arrays ──────────────────────────────────────────────────────

static bool reserve(void **items, int32_t *cap, int32_t want, size_t elem)
{
    if (want <= *cap)
        return true;
    int32_t next = *cap ? *cap : 16;
    while (next < want) {
        if (next > (int32_t)0x3FFFFFFF)
            return false;
        next *= 2;
    }
    void *grown = os64_realloc(*items, (size_t)next * elem);
    if (!grown)
        return false;
    *items = grown;
    *cap = next;
    return true;
}

// ── A buffer of folded bytes, and the pens over it ──────────────────────
//
// Both the row being built and the word being held are this: bytes, plus the
// runs that say how to paint them. A run EXTENDS rather than repeating
// itself, so a row of prose costs one run and one SGR pair.

typedef struct {
    char         *text;
    int32_t       len, cap;
    wend_run_t *runs;
    int32_t       nruns, runcap;
} buf_t;

static void buf_reset(buf_t *b) { b->len = 0; b->nruns = 0; }

static void buf_free(buf_t *b)
{
    os64_free(b->text);
    os64_free(b->runs);
    b->text = NULL;
    b->runs = NULL;
    b->len = b->cap = b->nruns = b->runcap = 0;
}

static bool buf_append(buf_t *b, const char *s, size_t n, uint8_t attrs, int32_t spot)
{
    if (n == 0)
        return true;
    if (!reserve((void **)&b->text, &b->cap, b->len + (int32_t)n + 1, 1))
        return false;
    // TERMINATE BEFORE ANYTHING ELSE CAN FAIL. Fresh storage holds whatever
    // the allocator last had there, and the run reservation below can refuse
    // — which would leave a row whose bytes nobody wrote and whose end
    // nothing marks, for the painter to walk off.
    b->text[b->len] = '\0';
    wend_run_t *last = b->nruns ? &b->runs[b->nruns - 1] : NULL;
    if (!last || last->attrs != attrs || last->spot != spot) {
        if (!reserve((void **)&b->runs, &b->runcap, b->nruns + 1, sizeof(*b->runs)))
            return false;
        b->runs[b->nruns++] = (wend_run_t){ (uint32_t)b->len, 0, attrs, spot };
        last = &b->runs[b->nruns - 1];
    }
    os64_memcpy(b->text + b->len, s, n);
    b->len += (int32_t)n;
    b->text[b->len] = '\0';
    last->len += (uint32_t)n;
    return true;
}

// ── The renderer's state ────────────────────────────────────────────────

typedef struct {
    wend_page_t *page;
    int32_t        linecap, spotcap, formcap, anchorcap;
    int32_t        cols;
    const os64_url_t *base;

    buf_t   line;           // what is on the row so far, its indent included
    buf_t   word;           // held back until whitespace or a block ends it

    uint8_t attrs;          // the pen
    int32_t spot;

    // THE PEN THE OWED SPACE WEARS, kept from where the whitespace WAS. A
    // space between two words of one link belongs to that link, a space
    // inside a heading is part of the heading — and by the time the word
    // after it is laid down the walk may have left the element that owned
    // them both, taking the pen with it.
    uint8_t space_attrs;
    int32_t space_spot;

    int32_t form;           // 1-based into the page's forms; 0 = outside any
    // >0 inside a fieldset the page disabled. A fieldset disables every
    // control under it, which is how a page greys out a whole section, and
    // the controls themselves carry no attribute saying so.
    int32_t disabled;
    const wend_edit_t *edits;   // what a person has filled in, by spot index
    int32_t nedits;

    int32_t indent;         // spaces a fresh row opens with
    int32_t pre;            // >0 inside pre/listing: the author's own columns
    bool    pending_space;  // whitespace was seen; one space is owed
    bool    content;        // this row holds something besides its indent
    bool    want_blank;     // a blank row is owed before the next content
    bool    oom;
} render_t;

// Where the row under construction will land. A spot records it the moment
// its first byte reaches a row, which is what lets the selection scroll to
// one without searching the page for it.
static int32_t line_index(const render_t *r) { return r->page->nlines; }

// AN INDENT MAY NEVER EAT THE ROW. Comment threads and quoted mail nest far
// deeper than anyone designing a margin imagines, and an indent that reached
// the right edge would leave no room for a word and no way forward. Half the
// screen is the most any nesting gets.
static int32_t indent_now(const render_t *r)
{
    int32_t ind = r->indent;
    if (ind > r->cols / 2)
        ind = r->cols / 2;
    return ind < 0 ? 0 : ind;
}

// Spaces at the end of a flowed row are the renderer's own leavings — a cell
// separator that turned out to end the row, an indent under nothing. They
// paint as nothing and they widen a selection over nothing, so they go.
// Inside `pre` they are the author's and they stay.
static void line_trim(buf_t *b)
{
    while (b->len > 0 && b->text[b->len - 1] == ' ') {
        b->text[--b->len] = '\0';
        wend_run_t *last = &b->runs[b->nruns - 1];
        if (--last->len == 0)
            b->nruns--;
    }
}

static void anchors_bind(render_t *r, int32_t line);

// Commit the row under construction. `force` is the preformatted path's:
// inside `pre` an empty row is a blank line the author typed, while in
// flowing text it is the artefact of a block that turned out to hold nothing.
static void line_end(render_t *r, bool force)
{
    if (!r->content && !force) {
        buf_reset(&r->line);
        r->pending_space = false;
        return;
    }
    if (r->pre == 0)
        line_trim(&r->line);
    wend_page_t *p = r->page;
    if (!reserve((void **)&p->lines, &r->linecap, p->nlines + 1, sizeof(*p->lines))) {
        r->oom = true;
        buf_reset(&r->line);
        r->content = false;
        r->pending_space = false;
        return;
    }
    char *text = r->line.text;
    if (!text) {
        text = os64_malloc(1);          // a blank row still owes a string
        if (!text) {
            r->oom = true;
            buf_reset(&r->line);
            r->content = false;
            r->pending_space = false;
            return;
        }
        text[0] = '\0';
    }
    p->lines[p->nlines].text = text;
    p->lines[p->nlines].runs = r->line.runs;
    p->lines[p->nlines].nruns = r->line.nruns;
    p->lines[p->nlines].len = r->line.len;
    p->nlines++;
    // The row's storage belongs to the page now; the builder starts empty.
    r->line.text = NULL;
    r->line.runs = NULL;
    r->line.len = r->line.cap = r->line.nruns = r->line.runcap = 0;
    r->content = false;
    r->pending_space = false;
}

// Open a row if none is open: first the blank line anything owes, then the
// indent. Called from the content path rather than from the block verbs, so
// an owed blank never materialises at the foot of a page.
static void line_open(render_t *r)
{
    if (r->line.len > 0 || r->content)
        return;
    if (r->want_blank) {
        r->want_blank = false;
        if (r->page->nlines > 0)
            line_end(r, true);          // an empty row, deliberately
    }
    anchors_bind(r, line_index(r));
    int32_t ind = indent_now(r);
    for (int32_t i = 0; i < ind; i++)
        if (!buf_append(&r->line, " ", 1, 0, 0)) {
            r->oom = true;
            return;
        }
}

// Put a slice of the held word onto the row, with the pens it was written
// under. A word that outgrew the screen is cut here, so the cut has to carry
// its runs across.
static void word_slice_to_line(render_t *r, int32_t from, int32_t take)
{
    for (int32_t i = 0; i < r->word.nruns && !r->oom; i++) {
        wend_run_t *run = &r->word.runs[i];
        int32_t start = (int32_t)run->start, end = start + (int32_t)run->len;
        int32_t piece_from = start > from ? start : from;
        int32_t piece_to = end < from + take ? end : from + take;
        if (piece_to <= piece_from)
            continue;
        if (run->spot > 0 && r->page->spots[run->spot - 1].line < 0)
            r->page->spots[run->spot - 1].line = line_index(r);
        if (!buf_append(&r->line, r->word.text + piece_from,
                        (size_t)(piece_to - piece_from), run->attrs, run->spot))
            r->oom = true;
    }
}

// End the held word: wrap first if it will not fit where the row now stands,
// then lay it down. A word wider than the whole row is cut at the margin,
// which is the only place this program ever splits one.
static void word_flush(render_t *r)
{
    if (r->word.len == 0)
        return;
    line_open(r);
    if (r->oom) {
        buf_reset(&r->word);
        return;
    }
    int32_t owed = (r->pending_space && r->content) ? 1 : 0;
    if (r->content && r->line.len + owed + r->word.len > r->cols) {
        line_end(r, false);
        line_open(r);
        owed = 0;
    }
    if (owed) {
        if (r->space_spot > 0 && r->page->spots[r->space_spot - 1].line < 0)
            r->page->spots[r->space_spot - 1].line = line_index(r);
        if (!buf_append(&r->line, " ", 1, r->space_attrs, r->space_spot))
            r->oom = true;
    }
    r->pending_space = false;

    int32_t at = 0;
    while (at < r->word.len && !r->oom) {
        line_open(r);
        int32_t room = r->cols - r->line.len;
        if (room <= 0) {                 // an indent this wide cannot happen,
            line_end(r, true);           // but a row with no room must still
            continue;                    // make progress
        }
        int32_t take = r->word.len - at;
        if (take > room)
            take = room;
        word_slice_to_line(r, at, take);
        r->content = true;
        at += take;
        if (at < r->word.len)
            line_end(r, false);
    }
    buf_reset(&r->word);
}

// ── What the walk puts on the page ──────────────────────────────────────

// Text that is part of a word: a marker, a widget, the fold's output. It
// GLUES, so `[3]` and the first word of the link stay on one row together.
static void word_text(render_t *r, const char *s, size_t n)
{
    if (!buf_append(&r->word, s, n, r->attrs, r->spot))
        r->oom = true;
}

// Text that opens a row: a list bullet, a rule. Ends whatever word was held
// and lands directly, so a wrap indents under it rather than beside it.
static void line_text(render_t *r, const char *s, size_t n)
{
    word_flush(r);
    line_open(r);
    if (r->oom)
        return;
    if (!buf_append(&r->line, s, n, r->attrs, r->spot))
        r->oom = true;
    else
        r->content = true;
}

static void block_break(render_t *r)
{
    word_flush(r);
    line_end(r, false);
}

static void blank_break(render_t *r)
{
    block_break(r);
    r->want_blank = true;
}

// A row of the author's own text, verbatim. Wraps hard at the margin because
// there is nothing here that may be re-flowed: inside `pre` the columns ARE
// the meaning.
static void pre_put(render_t *r, const char *s, size_t n)
{
    line_open(r);
    if (r->oom)
        return;
    if (r->content && r->line.len + (int32_t)n > r->cols) {
        line_end(r, false);
        line_open(r);
        if (r->oom)
            return;
    }
    if (r->spot > 0 && r->page->spots[r->spot - 1].line < 0)
        r->page->spots[r->spot - 1].line = line_index(r);
    if (!buf_append(&r->line, s, n, r->attrs, r->spot))
        r->oom = true;
    else
        r->content = true;
}

// ── Text nodes ──────────────────────────────────────────────────────────

static bool is_space(uint32_t cp)
{
    return cp == 0x20 || cp == 0x09 || cp == 0x0A || cp == 0x0C || cp == 0x0D;
}

// Flowing text: runs of whitespace become one owed space, and the words
// between them are held for the wrapper.
static void flow_text(render_t *r, const char *s, size_t n)
{
    size_t at = 0;
    while (at < n && !r->oom) {
        uint32_t cp = 0;
        size_t took = os64_utf8_decode(s + at, n - at, &cp);
        at += took ? took : 1;
        if (is_space(cp)) {
            word_flush(r);
            r->pending_space = true;
            r->space_attrs = r->attrs;
            r->space_spot = r->spot;
            continue;
        }
        char folded[WEND_FOLD_MAX];
        size_t got = wend_fold(cp, folded);
        if (got)
            word_text(r, folded, got);
    }
}

// Preformatted text: every byte where the author put it. A newline is a row
// even when the row is empty, and a tab goes to the next eight-column stop —
// the width every author who typed one was imagining.
//
// `utf8` says how to read a byte over 0x7F. False means the byte IS the code
// point, which is what the machines that wrote the old web's .txt files
// meant; decoding those as UTF-8 turns every accented name into a question
// mark.
static void pre_bytes(render_t *r, const char *s, size_t n, bool utf8)
{
    size_t at = 0;
    while (at < n && !r->oom) {
        uint32_t cp = 0;
        if (utf8) {
            size_t took = os64_utf8_decode(s + at, n - at, &cp);
            at += took ? took : 1;
        } else {
            cp = (unsigned char)s[at];
            at++;
        }
        if (cp == 0x0A) {
            line_end(r, true);
            continue;
        }
        // A CARRIAGE RETURN ENDS A ROW TOO, because a text file written on a
        // machine that ended its lines that way is still a text file with
        // lines. A CRLF pair is ONE ending: the newline that follows is
        // swallowed rather than counted again. (libhtml normalises both
        // before the tree, so this is the raw-text path's business.)
        if (cp == 0x0D) {
            line_end(r, true);
            if (at < n && s[at] == '\n')
                at++;                   // CRLF is one ending, not two
            continue;
        }
        if (cp == 0x09) {
            line_open(r);
            if (r->oom)
                return;
            int32_t stop = (r->line.len / 8 + 1) * 8;
            if (stop > r->cols)
                stop = r->cols;
            while (r->line.len < stop && !r->oom)
                pre_put(r, " ", 1);
            continue;
        }
        char folded[WEND_FOLD_MAX];
        size_t got = wend_fold(cp, folded);
        if (got)
            pre_put(r, folded, got);
    }
}

static void text_node(render_t *r, const os64_html_node_t *n)
{
    if (!n->text || n->text_len == 0)
        return;
    if (r->pre > 0)
        pre_bytes(r, n->text, n->text_len, true);
    else
        flow_text(r, n->text, n->text_len);
}

// ── Spots: the places a person can land ─────────────────────────────────

// A string the page owns a copy of. NULL and absent both become "", because
// every consumer of these fields wants a string and none of them wants to
// ask first.
static char *dup_text(render_t *r, const char *s)
{
    size_t n = s ? os64_strlen(s) : 0;
    char *copy = os64_malloc(n + 1);
    if (!copy) {
        r->oom = true;
        return NULL;
    }
    if (n)
        os64_memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

// Claim the next spot. Everything in it starts empty, so a caller fills only
// what its kind uses and a free walks the same fields for every kind.
static wend_spot_t *spot_new(render_t *r, wend_spot_kind_t kind)
{
    wend_page_t *p = r->page;
    if (!reserve((void **)&p->spots, &r->spotcap, p->nspots + 1, sizeof(*p->spots))) {
        r->oom = true;
        return NULL;
    }
    wend_spot_t *spot = &p->spots[p->nspots];
    *spot = (wend_spot_t){ 0 };
    spot->kind = kind;
    spot->line = -1;
    spot->chosen = -1;
    spot->form = r->form;
    p->nspots++;
    return spot;
}

// The edit a person made to the spot being built, if they made one. Indexed
// by spot number, which is why the walk must be deterministic for one tree.
static const wend_edit_t *edit_for(const render_t *r)
{
    int32_t index = r->page->nspots - 1;
    if (!r->edits || index < 0 || index >= r->nedits)
        return NULL;
    return &r->edits[index];
}

// Number a link and resolve where it goes. An href that will not resolve
// still gets a number and an EMPTY address: the page said this was a link,
// and hiding that would be editing the page. Following it is what fails, and
// it fails with a sentence.
//
// THE FRAGMENT IS KEPT ASIDE. `os64_url_absolute` drops it, correctly — a
// `#name` is never sent to a server — but dropping it here as well would
// turn every entry in a table of contents into "fetch this page again and
// show me the top of it", which on a long article is worse than doing
// nothing.
// The `#name` a reference asks for, or "" — the half `os64_url_absolute`
// drops, because it is the half no server is ever told about.
static const char *reference_fragment(const char *href)
{
    if (!href)
        return NULL;
    for (const char *p = href; *p != '\0'; p++)
        if (*p == '#')
            return p + 1;            // may be "" — `#` alone is the top
    return NULL;
}

static int32_t link_add(render_t *r, const char *href)
{
    wend_spot_t *spot = spot_new(r, WEND_SPOT_LINK);
    if (!spot)
        return 0;
    char resolved[OS64_URL_REF_MAX];
    resolved[0] = '\0';
    // AN EMPTY href NAMES THE PAGE IT IS ON — the standard's rule, and what
    // every "reload" and "back to the top" link on the old web is made of.
    // The resolver refuses an empty reference by contract, so ask it the
    // question it does answer: a fragment-only reference names the same
    // page.
    const char *reference = (href && href[0]) ? href : "#";
    if (r->base)
        (void)os64_url_absolute(r->base, reference, resolved, sizeof(resolved));
    const char *hash = reference_fragment(href);
    spot->url = dup_text(r, resolved);
    spot->fragment = dup_text(r, hash ? hash : "");
    spot->has_fragment = hash != NULL;
    return (spot->url && spot->fragment) ? r->page->nspots : 0;
}

// Remember a `#name`, for anything carrying an `id` and for the old-style
// `<a name>`. The ROW is not known yet: an element is met before its first
// word is placed, and between the two lies a block break and possibly a
// blank line. So the anchor is left UNBOUND and takes the row that opens
// next — the row its content actually lands on, which is what a reader
// jumping to a section wants at the top of the screen.
static void anchor_add(render_t *r, const char *name)
{
    if (!name || name[0] == '\0')
        return;
    wend_page_t *p = r->page;
    if (!reserve((void **)&p->anchors, &r->anchorcap, p->nanchors + 1,
                 sizeof(*p->anchors))) {
        r->oom = true;
        return;
    }
    p->anchors[p->nanchors].name = dup_text(r, name);
    p->anchors[p->nanchors].line = -1;
    if (p->anchors[p->nanchors].name)
        p->nanchors++;
}

// Give every anchor still waiting the row that is opening. They were added
// in document order, so the unbound ones are the tail.
static void anchors_bind(render_t *r, int32_t line)
{
    for (int32_t i = r->page->nanchors - 1; i >= 0; i--) {
        if (r->page->anchors[i].line >= 0)
            return;
        r->page->anchors[i].line = line;
    }
}

int32_t wend_anchor_line(const wend_page_t *page, const char *name)
{
    if (!page || !name || name[0] == '\0')
        return -1;
    for (int32_t i = 0; i < page->nanchors; i++)
        if (os64_streq(some(page->anchors[i].name), name))
            return page->anchors[i].line;
    return -1;
}

static const char *attr_value(const os64_html_node_t *el, const char *name)
{
    const os64_html_attr_t *a = os64_html_attr(el, name);
    return a && a->value ? a->value : NULL;
}

// ── The walk ────────────────────────────────────────────────────────────

// A list in progress: which bullet its items wear, and how far an ordered
// one has counted. It lives on the walk's own stack frame, which is where
// the nesting already is — no separate stack to keep in step with the tree.
typedef struct {
    bool    ordered;
    int32_t counter;
} list_t;

static void walk(render_t *r, const os64_html_node_t *n, list_t *list);

static void walk_children(render_t *r, const os64_html_node_t *n, list_t *list)
{
    for (const os64_html_node_t *c = n->first_child; c && !r->oom; c = c->next)
        walk(r, c, list);
}

// The bullet an item opens with, and the indent its wrapped rows line up
// under. An ordered list counts; an unordered one does not.
static void list_marker(render_t *r, list_t *list)
{
    char mark[16];
    size_t n = 0;
    if (list && list->ordered) {
        n = num_text(++list->counter, mark);
        mark[n++] = '.';
        mark[n++] = ' ';
    } else {
        mark[n++] = '*';
        mark[n++] = ' ';
    }
    line_text(r, mark, n);
    r->indent += (int32_t)n;            // a wrap aligns under the text
}

// The number a spot wears, glued to whatever it labels. It is what "type 3
// and press Enter" reaches, and on a form control it is the only thing that
// says the control is a place you can go.
static void spot_mark(render_t *r, int32_t index)
{
    char mark[16];
    size_t n = 0;
    mark[n++] = '[';
    n += num_text(index, mark + n);
    mark[n++] = ']';
    word_text(r, mark, n);
}

// Every descendant text node of an element, run together, as UTF-8 — an
// option's words, a button's words, a textarea's contents. Owned by the
// page; folded to Latin-1 only when it is drawn, because what is SENT must
// stay the bytes the page wrote.
//
// `verbatim` is the difference between WORDS and a VALUE. An option's words
// are prose: the runs of whitespace in the markup are indentation and are
// collapsed, the way they would be anywhere else on the page. A textarea's
// contents are the field's value, and collapsing them would send the server
// something other than what the page put in the box.
static char *gather(render_t *r, const os64_html_node_t *el, bool verbatim)
{
    buf_t got = { 0 };
    bool space_owed = false;
    for (const os64_html_node_t *c = el->first_child; c && !r->oom; c = c->next) {
        if (c->kind == OS64_HTML_ELEMENT) {
            char *inner = gather(r, c, verbatim);
            if (inner && inner[0]) {
                if (space_owed && got.len)
                    buf_append(&got, " ", 1, 0, 0);
                space_owed = false;
                buf_append(&got, inner, os64_strlen(inner), 0, 0);
            }
            os64_free(inner);
            continue;
        }
        if (c->kind != OS64_HTML_TEXT || !c->text)
            continue;
        for (size_t i = 0; i < c->text_len; i++) {
            unsigned char b = (unsigned char)c->text[i];
            if (!verbatim && (b == ' ' || b == '\t' || b == '\n' || b == '\f'
                              || b == '\r')) {
                space_owed = got.len > 0;
                continue;
            }
            if (space_owed) {
                if (!buf_append(&got, " ", 1, 0, 0))
                    r->oom = true;
                space_owed = false;
            }
            if (!buf_append(&got, c->text + i, 1, 0, 0))
                r->oom = true;
        }
    }
    char *out = dup_text(r, got.text ? got.text : "");
    buf_free(&got);
    return out;
}

static char *gather_text(render_t *r, const os64_html_node_t *el)
{
    return gather(r, el, false);
}

// UTF-8 in, cells out, no collapsing: a value is what somebody typed or what
// the page put there, spaces and all.
static void word_folded(render_t *r, const char *s)
{
    size_t at = 0, len = os64_strlen(s);
    while (at < len && !r->oom) {
        uint32_t cp = 0;
        size_t took = os64_utf8_decode(s + at, len - at, &cp);
        at += took ? took : 1;
        char folded[WEND_FOLD_MAX];
        size_t n = wend_fold(cp, folded);
        if (n)
            word_text(r, folded, n);
    }
}

// HOW WIDE A BOX IS DRAWN. The page's `size` when it gives a sane one,
// something typeable when it does not, and never more than half the row —
// a box wider than the screen is a box you cannot see the end of.
static int32_t box_width(render_t *r, const os64_html_node_t *n)
{
    const char *size = attr_value(n, "size");
    int32_t width = 16;
    if (size) {
        int32_t asked = 0;
        for (const char *p = size; *p >= '0' && *p <= '9' && asked < 1000; p++)
            asked = asked * 10 + (*p - '0');
        if (asked >= 3)
            width = asked;
    }
    if (width > r->cols / 2)
        width = r->cols / 2;
    if (width < 3)
        width = 3;
    return width;
}

// A text box with what is in it. The TAIL is what shows when the value
// outgrows the box, because the tail is what you just typed.
//
// A SECRET IS DRAWN AS ITS LENGTH AND NOTHING ELSE. A password field's
// value is on the glass in front of whoever is standing behind you, and a
// page that prefills one is not a reason to publish it.
//
// A line break inside a value — a textarea's, mostly — is shown as a space
// rather than as the `?` an unprintable code point earns. The VALUE keeps
// its bytes; this is one row's account of text that has more than one.
static void draw_box(render_t *r, const char *value, int32_t width, bool secret)
{
    char cells[128];
    if (width > (int32_t)sizeof(cells))
        width = (int32_t)sizeof(cells);
    int32_t have = 0;
    size_t at = 0, len = os64_strlen(value);
    while (at < len) {
        uint32_t cp = 0;
        size_t took = os64_utf8_decode(value + at, len - at, &cp);
        at += took ? took : 1;
        char folded[WEND_FOLD_MAX];
        size_t n;
        if (secret) {
            folded[0] = '*';
            n = 1;
        } else if (cp == 0x0A || cp == 0x0D || cp == 0x09) {
            folded[0] = ' ';
            n = 1;
        } else {
            n = wend_fold(cp, folded);
        }
        for (size_t i = 0; i < n; i++) {
            if (have == width) {
                for (int32_t j = 1; j < width; j++)
                    cells[j - 1] = cells[j];
                have--;
            }
            cells[have++] = folded[i];
        }
    }
    word_text(r, "[", 1);
    if (have)
        word_text(r, cells, (size_t)have);
    for (int32_t i = have; i < width; i++)
        word_text(r, "_", 1);
    word_text(r, "]", 1);
}

// A control that is drawn and cannot be operated — a reset, a plain button.
// It stays visible because a page with a gap where its button was reads as
// broken, and it is not a spot because landing on it would promise
// something.
static void draw_dead_control(render_t *r, const char *label, const char *fallback)
{
    word_text(r, "[", 1);
    if (label && label[0])
        word_folded(r, label);
    else
        word_text(r, fallback, os64_strlen(fallback));
    word_text(r, "]", 1);
}

// A DISABLED CONTROL IS DRAWN AND IS NOT A SPOT. The standard says it is not
// a successful control — it is not sent — and a page disables a control to
// take it away from you. Letting the keyboard land on one would offer
// something the page refused, and putting its value in the query would ask
// the server for what the page was built to prevent.
static bool control_disabled(const render_t *r, const os64_html_node_t *n)
{
    return r->disabled > 0 || os64_html_attr(n, "disabled") != NULL;
}

// A hidden field is the FORM's data and no part of what the form shows, so
// it is remembered against the form rather than made a place you can land.
static void hidden_add(render_t *r, const os64_html_node_t *n)
{
    if (r->form <= 0 || control_disabled(r, n))
        return;                          // nothing to carry it, or not sent at all
    wend_form_t *form = &r->page->forms[r->form - 1];
    if (!reserve((void **)&form->hidden_names, &form->hiddencap, form->nhidden + 1,
                 sizeof(*form->hidden_names))
        || !reserve((void **)&form->hidden_values, &form->hiddenvalcap, form->nhidden + 1,
                    sizeof(*form->hidden_values))) {
        r->oom = true;
        return;
    }
    if (!reserve((void **)&form->hidden_after, &form->hiddenaftercap,
                 form->nhidden + 1, sizeof(*form->hidden_after))) {
        r->oom = true;
        return;
    }
    form->hidden_names[form->nhidden] = dup_text(r, attr_value(n, "name"));
    form->hidden_values[form->nhidden] = dup_text(r, attr_value(n, "value"));
    form->hidden_after[form->nhidden] = r->page->nspots;
    form->nhidden++;
}

static bool type_is_nocase(const char *type, const char *want)
{
    return type && os64_streq_nocase(type, want);
}


// WHAT A SUBMIT BUTTON OVERRULES ABOUT ITS FORM. The standard lets the
// button carry its own action and its own method, and BOTH element types
// that can be one — `input type=submit` and `button` — must record them or
// the form is sent somewhere, or in a way, the page did not ask for.
static void submit_overrides(render_t *r, const os64_html_node_t *n,
                             wend_spot_t *spot)
{
    const char *formaction = attr_value(n, "formaction");
    const char *formmethod = attr_value(n, "formmethod");
    char over[OS64_URL_REF_MAX];
    over[0] = '\0';
    if (formaction && formaction[0] && r->base)
        (void)os64_url_absolute(r->base, formaction, over, sizeof(over));
    spot->form_action = dup_text(r, over);
    spot->form_fragment = dup_text(r, some(reference_fragment(formaction)));
    spot->has_method = formmethod != NULL && formmethod[0] != '\0';
    spot->post = spot->has_method && os64_streq_nocase(formmethod, "post");
}

// One `input`, which is six different controls wearing one tag name.
static void field_input(render_t *r, const os64_html_node_t *n)
{
    const char *type = attr_value(n, "type");
    const char *value = attr_value(n, "value");

    if (type_is_nocase(type, "hidden")) {
        hidden_add(r, n);
        return;
    }
    if (type_is_nocase(type, "reset") || type_is_nocase(type, "button")) {
        draw_dead_control(r, value, type);
        return;
    }

    bool readonly = os64_html_attr(n, "readonly") != NULL;
    wend_spot_kind_t kind = WEND_SPOT_TEXT;
    bool image = type_is_nocase(type, "image");
    if (type_is_nocase(type, "submit") || image)
        kind = WEND_SPOT_SUBMIT;
    else if (type_is_nocase(type, "checkbox"))
        kind = WEND_SPOT_CHECK;
    else if (type_is_nocase(type, "radio"))
        kind = WEND_SPOT_RADIO;
    bool secret = type_is_nocase(type, "password");

    if (control_disabled(r, n)) {
        bool checked = os64_html_attr(n, "checked") != NULL;
        switch (kind) {
            case WEND_SPOT_SUBMIT: draw_dead_control(r, value, "Submit"); break;
            case WEND_SPOT_CHECK:  word_text(r, checked ? "[x]" : "[ ]", 3); break;
            case WEND_SPOT_RADIO:  word_text(r, checked ? "(*)" : "( )", 3); break;
            default: draw_box(r, value ? value : "", box_width(r, n), secret); break;
        }
        return;
    }

    wend_spot_t *spot = spot_new(r, kind);
    if (!spot)
        return;
    int32_t index = r->page->nspots;
    spot->name = dup_text(r, attr_value(n, "name"));
    spot->value = dup_text(r, value);
    spot->secret = secret;
    spot->readonly = readonly;
    spot->on = os64_html_attr(n, "checked") != NULL;
    const wend_edit_t *edit = edit_for(r);
    if (edit && edit->on >= 0)
        spot->on = edit->on != 0;
    if (edit && edit->text && kind == WEND_SPOT_TEXT && !readonly) {
        os64_free(spot->value);
        spot->value = dup_text(r, edit->text);
    }
    if (r->oom)
        return;

    r->spot = index;
    spot_mark(r, index);
    switch (kind) {
        case WEND_SPOT_SUBMIT: {
            submit_overrides(r, n, spot);
            spot->image = image;
            const char *alt = image ? attr_value(n, "alt") : NULL;
            spot->label = dup_text(r, alt && alt[0] ? alt
                                      : (value && value[0] ? value : "Submit"));
            word_text(r, "[", 1);
            word_folded(r, spot->label);
            word_text(r, "]", 1);
            break;
        }
        case WEND_SPOT_CHECK:
            word_text(r, spot->on ? "[x]" : "[ ]", 3);
            break;
        case WEND_SPOT_RADIO:
            word_text(r, spot->on ? "(*)" : "( )", 3);
            break;
        default:
            spot->width = box_width(r, n);
            draw_box(r, spot->value, spot->width, spot->secret);
            break;
    }
    r->spot = 0;
}

// A `select` is its chosen option. The list is collected so the choice can
// be cycled without going back to the tree, and so what is SENT is the
// option's value rather than the words shown for it.
static void collect_options(render_t *r, const os64_html_node_t *el,
                            wend_spot_t *spot, bool group_off)
{
    for (const os64_html_node_t *c = el->first_child; c && !r->oom; c = c->next) {
        if (c->kind != OS64_HTML_ELEMENT)
            continue;
        bool off = group_off || os64_html_attr(c, "disabled") != NULL;
        if (c->tag == OS64_HTML_TAG_OPTGROUP) {
            // A disabled group disables every option under it.
            collect_options(r, c, spot, off);
            continue;
        }
        if (c->tag != OS64_HTML_TAG_OPTION)
            continue;
        if (!reserve((void **)&spot->options, &spot->optioncap, spot->noptions + 1,
                     sizeof(*spot->options))) {
            r->oom = true;
            return;
        }
        wend_option_t *option = &spot->options[spot->noptions];
        option->shown = gather_text(r, c);
        const char *value = attr_value(c, "value");
        option->value = dup_text(r, value ? value : some(option->shown));
        option->off = off;
        if (os64_html_attr(c, "selected") != NULL)
            spot->chosen = spot->noptions;
        spot->noptions++;
    }
}

static void field_select(render_t *r, const os64_html_node_t *n)
{
    if (control_disabled(r, n)) {
        // Its options are the page's business and it is not yours to open,
        // so what is drawn is that a list is there.
        word_text(r, "[v]", 3);
        return;
    }
    wend_spot_t *spot = spot_new(r, WEND_SPOT_CHOICE);
    if (!spot)
        return;
    int32_t index = r->page->nspots;
    spot->name = dup_text(r, attr_value(n, "name"));
    collect_options(r, n, spot, false);
    // A list with no marked option shows its first — even a disabled one,
    // because a disabled first option is how a page writes "choose one", and
    // showing it is the honest answer to what the list currently holds. It
    // still sends nothing.
    if (spot->chosen < 0 && spot->noptions > 0)
        spot->chosen = 0;
    const wend_edit_t *edit = edit_for(r);
    if (edit && edit->chosen >= 0 && edit->chosen < spot->noptions
        && !spot->options[edit->chosen].off)
        spot->chosen = edit->chosen;
    if (r->oom)
        return;

    r->spot = index;
    spot_mark(r, index);
    word_text(r, "[v ", 3);
    if (spot->chosen >= 0)
        word_folded(r, some(spot->options[spot->chosen].shown));
    word_text(r, "]", 1);
    r->spot = 0;
}

static void walk(render_t *r, const os64_html_node_t *n, list_t *list)
{
    if (r->oom)
        return;
    switch (n->kind) {
        case OS64_HTML_TEXT:
            text_node(r, n);
            return;
        case OS64_HTML_DOCUMENT:
        case OS64_HTML_FRAGMENT:
            walk_children(r, n, list);
            return;
        case OS64_HTML_ELEMENT:
            break;
        default:
            return;                      // a doctype and a comment draw nothing
    }

    // SVG AND MATHML DRAW NOTHING HERE. Their shapes are the graphical
    // browser's job, and their text is not prose — printing a formula's
    // tokens in reading order would say something the page does not.
    if (n->ns != OS64_HTML_NS_HTML)
        return;

    uint8_t saved_attrs = r->attrs;
    int32_t saved_spot = r->spot;
    int32_t saved_indent = r->indent;
    int32_t saved_form = r->form;

    // WHERE A `#name` LANDS is decided before the element draws anything:
    // the row it is ABOUT to fall on is the row a reader jumping here wants
    // at the top of the screen. `id` is every element's; `name` is only the
    // old anchor's, where it meant the same thing.
    const os64_html_attr_t *id = os64_html_attr(n, "id");
    if (id && id->value)
        anchor_add(r, id->value);
    if (n->tag == OS64_HTML_TAG_A && !os64_html_attr(n, "href")) {
        const os64_html_attr_t *anchor = os64_html_attr(n, "name");
        if (anchor && anchor->value)
            anchor_add(r, anchor->value);
    }

    switch (n->tag) {
        // ── Never shown ──
        //
        // `head` carries no prose (its title is read separately), a script
        // and a style are a program and a stylesheet, a template's contents
        // are a fragment the page has not used, and an iframe is a different
        // document that this program would have to fetch to show.
        case OS64_HTML_TAG_HEAD:
        case OS64_HTML_TAG_SCRIPT:
        case OS64_HTML_TAG_STYLE:
        case OS64_HTML_TAG_TEMPLATE:
        case OS64_HTML_TAG_IFRAME:
            return;

        // A FRAMESET PAGE IS ITS FRAMES. There is no body to show and no
        // prose anywhere in the document, so a face that skipped these would
        // paint an empty screen for a whole era of the web. Each frame
        // becomes a link to the document it names, which is what the page
        // was going to show you anyway.
        case OS64_HTML_TAG_FRAME: {
            const char *src = attr_value(n, "src");
            if (!src || !src[0])
                return;
            block_break(r);
            int32_t idx = link_add(r, src);
            if (idx > 0) {
                r->spot = idx;
                spot_mark(r, idx);
                const char *name = attr_value(n, "name");
                word_text(r, "frame: ", 7);
                flow_text(r, name && name[0] ? name : src,
                          os64_strlen(name && name[0] ? name : src));
                r->spot = saved_spot;
            }
            block_break(r);
            return;
        }

        // `noscript` is SHOWN, and that is the point of parsing with a
        // standard parser: this browser runs no script, so the standard says
        // its contents are markup, and they are usually the page's own
        // apology for needing one.
        case OS64_HTML_TAG_NOSCRIPT:
            break;

        // ── The paragraph family: a blank line either side ──
        case OS64_HTML_TAG_P:
            blank_break(r);
            walk_children(r, n, list);
            blank_break(r);
            goto done;

        case OS64_HTML_TAG_H1: case OS64_HTML_TAG_H2: case OS64_HTML_TAG_H3:
        case OS64_HTML_TAG_H4: case OS64_HTML_TAG_H5: case OS64_HTML_TAG_H6:
            blank_break(r);
            r->attrs |= WEND_ATTR_BOLD;
            walk_children(r, n, list);
            r->attrs = saved_attrs;
            blank_break(r);
            goto done;

        // WHATEVER THE WALK CHANGED IS PUT BACK ONLY AFTER THE ROWS IT
        // GOVERNS ARE DOWN, because a word and a row are both still held
        // when an element's children are done. Give the margin back first
        // and the last word of a list item wraps at the OUTER indent, under
        // its own bullet; leave `pre` first and the author's own trailing
        // spaces are trimmed off its last row.
        case OS64_HTML_TAG_BLOCKQUOTE:
            blank_break(r);
            r->indent += 4;
            walk_children(r, n, list);
            block_break(r);
            r->indent = saved_indent;
            blank_break(r);
            goto done;

        // A FORM IS A DESTINATION AND A METHOD, remembered while its controls
        // are walked so each one knows which form it belongs to. An action
        // that will not resolve, or none at all, leaves "" — which the
        // sender reads as "the page this form is on", the standard's answer.
        case OS64_HTML_TAG_FORM: {
            block_break(r);
            wend_page_t *p = r->page;
            if (!reserve((void **)&p->forms, &r->formcap, p->nforms + 1, sizeof(*p->forms))) {
                r->oom = true;
                goto done;
            }
            wend_form_t *form = &p->forms[p->nforms];
            *form = (wend_form_t){ 0 };
            const char *action = attr_value(n, "action");
            const char *method = attr_value(n, "method");
            char resolved[OS64_URL_REF_MAX];
            resolved[0] = '\0';
            if (action && action[0] && r->base)
                (void)os64_url_absolute(r->base, action, resolved, sizeof(resolved));
            form->action = dup_text(r, resolved);
            form->fragment = dup_text(r, some(reference_fragment(action)));
            form->post = method && os64_streq_nocase(method, "post");
            p->nforms++;
            r->form = p->nforms;
            walk_children(r, n, list);
            block_break(r);
            r->form = saved_form;
            goto done;
        }

        // ── Blocks that only want their own row ──
        // A DISABLED FIELDSET DISABLES WHAT IS INSIDE IT — except the words
        // in its first `legend`, which are the section's title and were
        // never a control. That exception is the standard's, and it is the
        // reason this is a case of its own rather than a plain block.
        case OS64_HTML_TAG_FIELDSET: {
            bool off = os64_html_attr(n, "disabled") != NULL;
            bool legend_seen = false;
            block_break(r);
            if (off)
                r->disabled++;
            for (const os64_html_node_t *c = n->first_child; c && !r->oom; c = c->next) {
                bool legend = !legend_seen && c->kind == OS64_HTML_ELEMENT
                              && c->ns == OS64_HTML_NS_HTML
                              && c->tag == OS64_HTML_TAG_LEGEND;
                if (legend) {
                    legend_seen = true;
                    if (off)
                        r->disabled--;
                }
                walk(r, c, list);
                if (legend && off)
                    r->disabled++;
            }
            if (off)
                r->disabled--;
            block_break(r);
            goto done;
        }

        case OS64_HTML_TAG_DIV: case OS64_HTML_TAG_DL: case OS64_HTML_TAG_DT:
        case OS64_HTML_TAG_ADDRESS: case OS64_HTML_TAG_CENTER:
        case OS64_HTML_TAG_SECTION: case OS64_HTML_TAG_ARTICLE:
        case OS64_HTML_TAG_NAV: case OS64_HTML_TAG_ASIDE:
        case OS64_HTML_TAG_HEADER: case OS64_HTML_TAG_FOOTER:
        case OS64_HTML_TAG_MAIN: case OS64_HTML_TAG_FIGURE:
        case OS64_HTML_TAG_FIGCAPTION: case OS64_HTML_TAG_CAPTION:
        case OS64_HTML_TAG_SUMMARY: case OS64_HTML_TAG_DETAILS:
        case OS64_HTML_TAG_TABLE: case OS64_HTML_TAG_THEAD:
        case OS64_HTML_TAG_TBODY: case OS64_HTML_TAG_TFOOT:
        case OS64_HTML_TAG_TR:
            block_break(r);
            walk_children(r, n, list);
            block_break(r);
            goto done;

        // A CELL IS TWO SPACES FROM ITS NEIGHBOUR AND NOTHING MORE. The old
        // web's tables are LAYOUT — a page built as one big table reads
        // acceptably as a sequence of rows, which is what lynx shows — and
        // aligning columns is the graphical browser's boss, not this one's.
        case OS64_HTML_TAG_TD: case OS64_HTML_TAG_TH:
            // The word the PREVIOUS cell ended with is still held, so ask
            // whether the row has anything on it only after laying it down —
            // otherwise the first two cells of every row run together.
            word_flush(r);
            if (r->content)
                line_text(r, "  ", 2);
            walk_children(r, n, list);
            goto done;

        case OS64_HTML_TAG_UL: case OS64_HTML_TAG_OL:
        case OS64_HTML_TAG_MENU: case OS64_HTML_TAG_DIR: {
            list_t inner = { n->tag == OS64_HTML_TAG_OL, 0 };
            block_break(r);
            r->indent += 2;
            walk_children(r, n, &inner);
            block_break(r);
            r->indent = saved_indent;
            goto done;
        }

        case OS64_HTML_TAG_LI:
            block_break(r);
            list_marker(r, list);
            walk_children(r, n, list);
            block_break(r);
            r->indent = saved_indent;
            goto done;

        case OS64_HTML_TAG_DD:
            block_break(r);
            r->indent += 4;
            walk_children(r, n, list);
            block_break(r);
            r->indent = saved_indent;
            goto done;

        case OS64_HTML_TAG_PRE: case OS64_HTML_TAG_LISTING:
            blank_break(r);
            r->pre++;
            walk_children(r, n, list);
            block_break(r);              // the last row, while it is still the
            r->pre--;                    // author's own spacing
            r->want_blank = true;
            goto done;

        // A textarea's text is its VALUE, not the page's prose — showing it
        // as prose would put the contents of a comment box into the article —
        // so it becomes the box's starting contents and the box is a place
        // you can type. One row's worth: a line-mode browser has no second
        // row to give it.
        case OS64_HTML_TAG_TEXTAREA: {
            if (control_disabled(r, n)) {
                char *shown = gather(r, n, true);
                if (shown)
                    draw_box(r, shown, box_width(r, n), false);
                os64_free(shown);
                goto done;
            }
            wend_spot_t *spot = spot_new(r, WEND_SPOT_TEXT);
            if (!spot)
                goto done;
            int32_t index = r->page->nspots;
            spot->name = dup_text(r, attr_value(n, "name"));
            // VERBATIM: this is the field's VALUE, not the page's prose, so
            // the newlines and runs of spaces in it are the author's and go
            // back to the server as they came.
            spot->value = gather(r, n, true);
            const wend_edit_t *edit = edit_for(r);
            if (edit && edit->text && os64_html_attr(n, "readonly") == NULL) {
                os64_free(spot->value);
                spot->value = dup_text(r, edit->text);
            }
            spot->readonly = os64_html_attr(n, "readonly") != NULL;
            spot->width = box_width(r, n);
            if (r->oom)
                goto done;
            r->spot = index;
            spot_mark(r, index);
            draw_box(r, spot->value, spot->width, spot->secret);
            r->spot = 0;
            goto done;
        }

        case OS64_HTML_TAG_SELECT:
            field_select(r, n);
            goto done;

        case OS64_HTML_TAG_INPUT:
            field_input(r, n);
            goto done;

        // A button's own words are its label, so they are drawn INSIDE it
        // rather than gathered: the pen it sets makes the whole thing one
        // highlight. A button that does not submit is drawn and not landed
        // on, like a reset.
        case OS64_HTML_TAG_BUTTON: {
            const char *type = attr_value(n, "type");
            if (control_disabled(r, n) || type_is_nocase(type, "reset")
                || type_is_nocase(type, "button")) {
                word_text(r, "[", 1);
                walk_children(r, n, list);
                word_text(r, "]", 1);
                goto done;
            }
            wend_spot_t *spot = spot_new(r, WEND_SPOT_SUBMIT);
            if (!spot)
                goto done;
            int32_t index = r->page->nspots;
            spot->name = dup_text(r, attr_value(n, "name"));
            spot->value = dup_text(r, attr_value(n, "value"));
            submit_overrides(r, n, spot);
            spot->label = gather_text(r, n);
            if (r->oom)
                goto done;
            r->spot = index;
            spot_mark(r, index);
            word_text(r, "[", 1);
            walk_children(r, n, list);
            word_text(r, "]", 1);
            r->spot = saved_spot;
            goto done;
        }

        case OS64_HTML_TAG_BR:
            // A break the author asked for is a break even on an empty row —
            // two of them in a row is how a page with no `p` writes a blank
            // line. What it may not do is open the page with one.
            word_flush(r);
            if (r->content)
                line_end(r, false);
            else if (r->page->nlines > 0)
                line_end(r, true);
            goto done;

        case OS64_HTML_TAG_HR: {
            block_break(r);
            char rule[256];
            int32_t width = r->cols - indent_now(r);
            if (width > (int32_t)sizeof(rule))
                width = (int32_t)sizeof(rule);
            for (int32_t i = 0; i < width; i++)
                rule[i] = '-';
            line_text(r, rule, (size_t)(width > 0 ? width : 0));
            block_break(r);
            goto done;
        }

        // ── The pen ──
        case OS64_HTML_TAG_B: case OS64_HTML_TAG_STRONG:
            r->attrs |= WEND_ATTR_BOLD;
            break;

        // Italic as UNDERLINE, which is lynx's answer and the only one a
        // terminal with one font weight can give.
        case OS64_HTML_TAG_I: case OS64_HTML_TAG_EM: case OS64_HTML_TAG_U:
            r->attrs |= WEND_ATTR_UNDERLINE;
            break;

        case OS64_HTML_TAG_A: {
            const char *href = attr_value(n, "href");
            if (!href)
                break;                   // an anchor with no address is a name
            int32_t idx = link_add(r, href);
            if (idx > 0) {
                r->spot = idx;
                spot_mark(r, idx);       // glued to the link's first word
            }
            break;
        }

        // An image is its alt text, which is what alt text is FOR. Without
        // one, the word `[image]` — because a page that is one picture and
        // no words should say so rather than look empty. Inside a link, both
        // are the link's text, which is how a masthead stays followable.
        case OS64_HTML_TAG_IMG: {
            // AN EMPTY `alt` IS AN ANSWER, not a missing one: it is the page
            // saying this picture is decoration and carries no words. Drawing
            // `[image]` for it fills a page with prose its author deliberately
            // did not write — a modern article's logos, icons and tracking
            // pixels all say `alt=""`.
            const os64_html_attr_t *alt = os64_html_attr(n, "alt");
            if (alt && alt->value && alt->value[0]) {
                word_text(r, "[", 1);
                flow_text(r, alt->value, os64_strlen(alt->value));
                word_text(r, "]", 1);
            } else if (!alt) {
                word_text(r, "[image]", 7);
            }
            goto done;
        }

        default:
            break;                       // every other element is its contents
    }

    walk_children(r, n, list);

done:
    r->attrs = saved_attrs;
    r->spot = saved_spot;
    r->indent = saved_indent;
}

// ── The page's own facts ────────────────────────────────────────────────

// The title, folded and collapsed: it goes on one row of chrome, so the
// newlines and runs of spaces a page puts in its `title` element are noise.
static void title_text(wend_page_t *page, const os64_html_node_t *title)
{
    size_t at = 0;
    bool space_owed = false;
    for (const os64_html_node_t *c = title->first_child; c; c = c->next) {
        if (c->kind != OS64_HTML_TEXT || !c->text)
            continue;
        size_t i = 0;
        while (i < c->text_len) {
            uint32_t cp = 0;
            size_t took = os64_utf8_decode(c->text + i, c->text_len - i, &cp);
            i += took ? took : 1;
            if (is_space(cp)) {
                space_owed = at > 0;
                continue;
            }
            char folded[WEND_FOLD_MAX];
            size_t got = wend_fold(cp, folded);
            if (!got)
                continue;
            if (space_owed && at + 1 < sizeof(page->title)) {
                page->title[at++] = ' ';
                space_owed = false;
            }
            if (at + got >= sizeof(page->title))
                return;                  // a title this long is already said
            os64_memcpy(page->title + at, folded, got);
            at += got;
            page->title[at] = '\0';
        }
    }
}

bool wend_base_href(const os64_html_document_t *doc, char *out, size_t cap)
{
    if (!doc || !doc->head || cap == 0)
        return false;
    // THE FIRST `base` WITH AN href WINS, which is the standard's rule and
    // the one that matters: a page with two of them is telling you it was
    // assembled by machines, and the later one is the accident.
    for (const os64_html_node_t *c = doc->head->first_child; c; c = c->next) {
        if (c->kind != OS64_HTML_ELEMENT || c->tag != OS64_HTML_TAG_BASE)
            continue;
        const os64_html_attr_t *href = os64_html_attr(c, "href");
        if (!href || !href->value || !href->value[0])
            continue;
        return os64_strcopy(out, cap, href->value) < cap;
    }
    return false;
}

// ── Entry points ────────────────────────────────────────────────────────

// The narrowest page this will lay out, which is a floor against absurdity
// rather than a policy: an indent is capped at half the row, so every loop
// here makes progress at any width, and a real terminal is far above this.
#define WEND_COLS_MIN 4

static wend_page_t *page_new(int32_t cols)
{
    wend_page_t *page = os64_calloc(1, sizeof(*page));
    if (!page)
        return NULL;
    page->cols = cols < WEND_COLS_MIN ? WEND_COLS_MIN : cols;
    return page;
}

wend_page_t *wend_render_html(const os64_html_document_t *doc,
                              const os64_url_t *base, int32_t cols,
                              const wend_edit_t *edits, int32_t nedits)
{
    wend_page_t *page = page_new(cols);
    if (!page)
        return NULL;
    render_t r = { 0 };
    r.page = page;
    r.cols = page->cols;
    r.base = base;
    r.edits = edits;
    r.nedits = edits ? nedits : 0;
    if (doc) {
        if (doc->head)
            for (const os64_html_node_t *c = doc->head->first_child; c; c = c->next)
                if (c->kind == OS64_HTML_ELEMENT && c->tag == OS64_HTML_TAG_TITLE) {
                    title_text(page, c);
                    break;
                }
        const os64_html_node_t *root = doc->document ? doc->document : doc->html;
        if (root)
            walk(&r, root, NULL);
    }
    block_break(&r);
    // An anchor at the very foot of a document has no row opening after it;
    // it belongs to the last one there is.
    anchors_bind(&r, page->nlines > 0 ? page->nlines - 1 : 0);
    buf_free(&r.line);
    buf_free(&r.word);
    page->incomplete = r.oom;
    return page;
}

wend_page_t *wend_render_text(const char *text, size_t len, bool utf8, int32_t cols)
{
    wend_page_t *page = page_new(cols);
    if (!page)
        return NULL;
    render_t r = { 0 };
    r.page = page;
    r.cols = page->cols;
    r.pre = 1;
    if (text && len)
        pre_bytes(&r, text, len, utf8);
    // A file that ends without a newline still ends with a line.
    if (r.content)
        line_end(&r, false);
    buf_free(&r.line);
    buf_free(&r.word);
    page->incomplete = r.oom;
    return page;
}

// ── Sending a form ──────────────────────────────────────────────────────
//
// PERCENT-ENCODING, in the dialect a form uses: a space is `+`, the
// unreserved characters go as they are, and everything else is `%` and two
// hex digits. The bytes being encoded are UTF-8, which is what the page's
// own values are and what a server that sent a UTF-8 page expects back.

static bool append_raw(char *out, size_t cap, size_t *at, const char *s)
{
    size_t n = os64_strlen(s);
    if (*at + n + 1 > cap)
        return false;
    os64_memcpy(out + *at, s, n + 1);
    *at += n;
    return true;
}

static bool append_encoded(char *out, size_t cap, size_t *at, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        char piece[4];
        size_t n = 0;
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')
            || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.'
            || *p == '~') {
            piece[n++] = (char)*p;
        } else if (*p == ' ') {
            piece[n++] = '+';
        } else {
            piece[n++] = '%';
            piece[n++] = hex[*p >> 4];
            piece[n++] = hex[*p & 0x0F];
        }
        if (*at + n + 1 > cap)
            return false;
        for (size_t i = 0; i < n; i++)
            out[(*at)++] = piece[i];
        out[*at] = '\0';
    }
    return true;
}

// One name and value. A control with no name sends NOTHING — the standard's
// rule, and what keeps a decorative box out of the query.
static bool pair_append(char *out, size_t cap, size_t *at, bool *first,
                        const char *name, const char *value)
{
    if (!name || name[0] == '\0')
        return true;
    if (!append_raw(out, cap, at, *first ? "?" : "&"))
        return false;
    *first = false;
    return append_encoded(out, cap, at, name)
           && append_raw(out, cap, at, "=")
           && append_encoded(out, cap, at, value ? value : "");
}

wend_form_result_t wend_form_url(const wend_page_t *page, int32_t index,
                                 const char *page_url, char *out, size_t cap,
                                 char *fragment, size_t fragment_cap)
{
    if (fragment && fragment_cap)
        fragment[0] = '\0';
    if (!page || !out || cap == 0 || index < 0 || index >= page->nspots)
        return WEND_FORM_NONE;
    const wend_spot_t *from = &page->spots[index];
    if (from->form <= 0 || from->form > page->nforms)
        return WEND_FORM_NONE;
    const wend_form_t *form = &page->forms[from->form - 1];
    // THE BUTTON'S OWN METHOD WINS WHERE IT NAMES ONE. A GET form with a
    // `formmethod=post` button is a POST, and sending it as a GET would put
    // whatever it collected — a password, most of the time — into an address
    // that servers and proxies write down.
    bool post = (from->kind == WEND_SPOT_SUBMIT && from->has_method) ? from->post
                                                                    : form->post;
    if (post)
        return WEND_FORM_POST;

    // The form's own destination, the button's if it names one, or the page
    // it all sits on. A GET form REPLACES whatever query that address
    // already carries.
    const char *action = some(form->action)[0] ? form->action : some(page_url);
    const char *want = some(form->fragment);
    if (from->kind == WEND_SPOT_SUBMIT && some(from->form_action)[0]) {
        // The button's action brings its own `#name`: it is a different
        // destination, so the form's section does not travel to it.
        action = from->form_action;
        want = some(from->form_fragment);
    }
    if (fragment && fragment_cap)
        (void)os64_strcopy(fragment, fragment_cap, want);
    size_t at = 0;
    for (const char *q = action; *q != '\0' && *q != '?' && *q != '#'; q++) {
        if (at + 2 > cap)
            return WEND_FORM_TOO_LONG;
        out[at++] = *q;
    }
    out[at] = '\0';

    // IN TREE ORDER, which means merging the two lists a form's controls are
    // split across: a hidden field remembers how many spots stood before it,
    // so each one goes out where the page wrote it rather than all of them
    // in front. Order is only visible to a server when two controls share a
    // name — which is exactly when it is load-bearing.
    bool first = true;
    int32_t hidden_at = 0;
    for (int32_t i = 0; i <= page->nspots; i++) {
        while (hidden_at < form->nhidden && form->hidden_after[hidden_at] <= i) {
            if (!pair_append(out, cap, &at, &first,
                             some(form->hidden_names[hidden_at]),
                             some(form->hidden_values[hidden_at])))
                return WEND_FORM_TOO_LONG;
            hidden_at++;
        }
        if (i == page->nspots)
            break;
        const wend_spot_t *spot = &page->spots[i];
        bool room = true;
        if (spot->form != from->form)
            continue;
        switch (spot->kind) {
            case WEND_SPOT_TEXT:
                room = pair_append(out, cap, &at, &first, some(spot->name),
                                   some(spot->value));
                break;
            case WEND_SPOT_CHECK:
            case WEND_SPOT_RADIO:
                // A box that is not ticked is not sent at all, and one that is
                // with no value of its own sends `on` — both the standard's,
                // and both what a server is written against.
                if (spot->on)
                    room = pair_append(out, cap, &at, &first, some(spot->name),
                                       some(spot->value)[0] ? spot->value : "on");
                break;
            case WEND_SPOT_CHOICE:
                // A disabled option — a "choose one" placeholder, usually —
                // is not a successful control and sends nothing at all.
                if (spot->chosen >= 0 && spot->chosen < spot->noptions
                    && !spot->options[spot->chosen].off)
                    room = pair_append(out, cap, &at, &first, some(spot->name),
                                       some(spot->options[spot->chosen].value));
                break;
            case WEND_SPOT_SUBMIT:
                // Only the button that was pressed says so — and an IMAGE
                // button says it differently: the standard sends where the
                // pointer was, which for a keyboard is the origin, and
                // never the button's value. A server written for one is
                // looking for `name.x` and would not see a `name=`.
                if (i != index)
                    break;
                if (spot->image) {
                    char field[OS64_URL_PATH_MAX];
                    const char *base_name = some(spot->name);
                    if (base_name[0] == '\0')
                        base_name = "";
                    for (int32_t axis = 0; axis < 2 && room; axis++) {
                        size_t at_name = 0;
                        field[0] = '\0';
                        if (!append_raw(field, sizeof(field), &at_name, base_name)
                            || !append_raw(field, sizeof(field), &at_name,
                                           axis == 0 ? ".x" : ".y")) {
                            room = false;
                            break;
                        }
                        room = pair_append(out, cap, &at, &first,
                                           base_name[0] ? field : "", "0");
                    }
                    break;
                }
                room = pair_append(out, cap, &at, &first, some(spot->name),
                                   some(spot->value));
                break;
            default:
                break;
        }
        if (!room)
            return WEND_FORM_TOO_LONG;
    }
    return WEND_FORM_OK;
}

void wend_page_free(wend_page_t *page)
{
    if (!page)
        return;
    for (int32_t i = 0; i < page->nlines; i++) {
        os64_free(page->lines[i].text);
        os64_free(page->lines[i].runs);
    }
    // Every spot frees the same fields whatever its kind: the ones its kind
    // does not use were never filled, and a free that asks about the kind is
    // a free that leaks the day a kind grows a field.
    for (int32_t i = 0; i < page->nspots; i++) {
        wend_spot_t *spot = &page->spots[i];
        os64_free(spot->url);
        os64_free(spot->fragment);
        os64_free(spot->name);
        os64_free(spot->value);
        os64_free(spot->label);
        os64_free(spot->form_action);
        os64_free(spot->form_fragment);
        for (int32_t j = 0; j < spot->noptions; j++) {
            os64_free(spot->options[j].shown);
            os64_free(spot->options[j].value);
        }
        os64_free(spot->options);
    }
    for (int32_t i = 0; i < page->nforms; i++) {
        wend_form_t *form = &page->forms[i];
        os64_free(form->action);
        os64_free(form->fragment);
        for (int32_t j = 0; j < form->nhidden; j++) {
            os64_free(form->hidden_names[j]);
            os64_free(form->hidden_values[j]);
        }
        os64_free(form->hidden_names);
        os64_free(form->hidden_values);
        os64_free(form->hidden_after);
    }
    for (int32_t i = 0; i < page->nanchors; i++)
        os64_free(page->anchors[i].name);
    os64_free(page->lines);
    os64_free(page->anchors);
    os64_free(page->spots);
    os64_free(page->forms);
    os64_free(page);
}
