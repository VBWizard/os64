#ifndef WAY_WAY_H
#define WAY_WAY_H

// libway — a browsing SESSION, apart from whatever draws it
// (docs/completed/06-navigator.md): where you have been, how a page is
// loaded, which addresses this browser follows, and when a person is asked
// before something is sent. The name is the idiom, to wend one's way;
// `wend` and `yonder` are two faces on it, and the library never paints and
// never reads a key.
//
// TWO HALVES. The I/O half (way_load) talks to libfetch. Everything else is
// pure over a page and a session, which is what lets a host harness drive
// every judgement with no network under it.
//
// SENTENCES. What the library has to say lands in `session->status`, the
// face's transient line, in the house's status-row voice (a leading space,
// no full stop). A face that shows it elsewhere trims what it likes.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fetch/fetch.h"
#include "html/html.h"
#include "page/page.h"

#pragma GCC visibility push(default)

#define WAY_SENTENCE_MAX 512
// Oldest dropped past this: somebody sixty-four pages in wants the
// sixty-fifth more than the first.
#define WAY_HISTORY_MAX 64
// A chain of immediate refreshes is capped as a chain of redirects is, and
// for the same reason: nobody reading can tell a slow one from a stuck one.
#define WAY_REFRESH_MAX 5
#define WAY_POSITION_MAX 16

// WHERE A PERSON WAS ON A PAGE, in the face's own terms — wend keeps a row
// and a selection, yonder a scroll offset in pixels. The library stores it
// and hands it back without reading it. It is a HINT: going back is a fetch,
// and the page that comes back need not be the page that was left, so a
// face checks it against what arrived before trusting it into an array.
typedef struct {
    uint8_t bytes[WAY_POSITION_MAX];
} way_position_t;

// A page, and everything a face needs to draw it again at another size.
typedef struct {
    char url[OS64_FETCH_URL_MAX];       // the address the body came from
    os64_html_document_t *doc;          // HTML: the tree
    char *text;                         // text/plain: the bytes
    size_t textlen;
    bool text_utf8;
    // What the page MEANS (LIBPAGE.md): where its links go, what its
    // controls hold. A person's edits live here, so they survive a relayout.
    // NULL when memory ran out building it: the page still reads, and its
    // links and boxes do not work.
    os64_page_t *model;
    bool refresh_handled;               // per loaded document, refused attempts included
    // The `details` the reader opened or closed: the reader's state, not
    // the page's, so it lives with the page and survives a relayout.
    const os64_html_node_t **flipped;
    int32_t nflipped, flippedcap;
    // The page's standing line: the reply's status and reason, and a
    // sentence for every way the page is incomplete.
    char note[WAY_SENTENCE_MAX];
} way_page_t;

// How the library reaches the person.
typedef struct {
    void *ctx;
    // Ask a yes/no question. `security` marks one where a stale or
    // unverifiable answer could send something in clear; `refused` is the
    // sentence for a no, NULL for none. The face owns how an answer is
    // shown to be FRESH.
    bool (*confirm)(void *ctx, const char *question, bool security, const char *refused);
    // Whether to stop the fetch in flight. Asked before every wait.
    bool (*cancelled)(void *ctx);
    // Show `session->status` NOW: a fetch takes as long as the network
    // takes, and a person deserves to see what is being fetched while it
    // happens. NULL shows nothing until the face next draws.
    void (*progress)(void *ctx, const char *sentence);
} way_face_t;

typedef struct {
    char url[OS64_FETCH_URL_MAX];
    way_position_t position;
} way_crumb_t;

typedef struct {
    const char *name;                   // how a sentence names this browser: "wend"
    const char *agent;                  // the User-Agent it sends
    const char *accept;                 // the Accept list, what it can render
    way_face_t face;
    char status[WAY_SENTENCE_MAX];      // the transient sentence
    way_crumb_t history[WAY_HISTORY_MAX];
    int32_t depth;
} way_session_t;

// ── The I/O half ────────────────────────────────────────────────────────

// Fetch and parse one address into `out`, which the caller zeroed.
// `request` is the form a page is sending, or NULL for a GET of `url`: a
// POST's body and content type go out with it, and must stay alive until
// this returns. False
// when there is nothing to show, with the reason in `session->status` and
// `out` empty. True leaves the page in `out` and its note written; the face
// lays it out and clears `session->status` or says what laying out found.
// `why` (may be NULL) is the fetch's own verdict, for a face that turns a
// failed first page into an exit code.
bool way_load(way_session_t *session, const char *url, const os64_page_request_t *request,
              way_page_t *out, os64_fetch_status_t *why);

// ── Pages ───────────────────────────────────────────────────────────────

// Frees what the page holds and leaves it empty.
void way_page_clear(way_page_t *page);

// Opens or closes a `details` for the reader. False on no memory.
bool way_details_flip(way_page_t *page, const os64_html_node_t *details);

// ── The history ─────────────────────────────────────────────────────────

// Remembers the page being left, and where on it the person was.
void way_remember(way_session_t *session, const char *url, const way_position_t *where);

// The crumb Back would return to. False, with the sentence, when there is
// none. The crumb stays until way_forget_last: Back is a fetch, and one
// that fails must leave the history as it was.
bool way_last(way_session_t *session, way_crumb_t *out);
void way_forget_last(way_session_t *session);

// ── Judgements: what to do about a request a page makes ────────────────

// WHO IS ASKED before an encrypted page's request goes out in the clear.
// Nobody for a link: a person pressed it, and where it goes is written on
// the page. A person for a form (what goes is what they typed) and for a
// refresh (the page chose where, and nobody pressed anything).
typedef enum {
    WAY_ASK_NEVER = 0,
    WAY_ASK_SEND,
    WAY_ASK_GO,
} way_ask_t;

typedef enum {
    WAY_REFUSE = 0,     // the reason is in session->status
    WAY_QUESTION,       // put `question` first; a no means `refused`
    WAY_FETCH,          // go to request->url
} way_judgement_kind_t;

typedef struct {
    way_judgement_kind_t kind;
    char question[WAY_SENTENCE_MAX];
    const char *refused;
    bool security;
} way_judgement_t;

// The judgement on a NAVIGATE request: this browser's own list of what it
// fetches, and the question owed before anything goes out in clear.
way_judgement_t way_judge(way_session_t *session, const os64_page_request_t *request,
                          way_ask_t ask);

// way_judge, with the question put through the face. True: fetch
// request->url.
bool way_may_go(way_session_t *session, const os64_page_request_t *request, way_ask_t ask);

// ── A page that asks to send you somewhere ──────────────────────────────

typedef enum {
    WAY_REFRESH_NONE = 0,   // nothing to do (a sentence may say why)
    WAY_REFRESH_JUMP,       // move to request->anchor, in this page
    WAY_REFRESH_GO,         // perform the request: no history entry, WAY_ASK_GO
} way_refresh_t;

// One hop of a declared refresh's chain, judged once per loaded page. The
// caller zero-starts `*chain` for each chain and asks again after a GO, so
// a redirector that lands on another redirector needs no keypress between
// them. After JUMP or GO the caller frees `request`
// (os64_page_request_free); after NONE there is nothing to free.
way_refresh_t way_refresh_step(way_session_t *session, way_page_t *page, int32_t *chain,
                               os64_page_request_t *request);

// The address a page asks to send the reader to after a delay, or NULL —
// what a face offers rather than follows.
const char *way_delayed_refresh(const way_page_t *page);

// ── What a person types ─────────────────────────────────────────────────

// A bare `host/path` means http. An address that does not fit is refused,
// never trimmed: a truncated URL is a different address, one nobody typed.
bool way_typed_address(const char *typed, char *out, size_t cap);

#pragma GCC visibility pop

#endif
