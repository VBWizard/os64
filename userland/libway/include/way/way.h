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
// SENTENCES. What the library has to say lands in `session->status` (or a
// leg's, for a load), the face's transient line, in the house's status-row
// voice (a leading space, no full stop). A face that shows it elsewhere
// trims what it likes.
//
// THREADS. A session belongs to the thread that made it. A load may run on
// another, through a way_leg_t: see way_load.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fetch/fetch.h"
#include "html/html.h"
#include "page/page.h"
#include "way/cache.h"
#include "way/jar.h"

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
    // The reply to a POST, by the FINAL method: its address alone cannot
    // fetch it again, so a reload is the form sent again (a 303 turned a
    // POST into a GET, and that page is an address like any other).
    bool posted;
} way_page_t;

// How the library reaches the person.
typedef struct {
    void *ctx;
    // Ask a yes/no question. The question says nothing about HOW to answer
    // — a terminal's "(y/n)" is not a window's pair of buttons — so the face
    // adds its own. `security` marks one where a stale or unverifiable
    // answer could send something in clear; `refused` is the sentence for a
    // no, NULL for none. The face owns how an answer is shown to be FRESH.
    // NULL answers every question no: nothing goes out in clear that nobody
    // was asked about.
    bool (*confirm)(void *ctx, const char *question, bool security, const char *refused);
    // Whether to stop the fetch in flight. Asked before every wait.
    bool (*cancelled)(void *ctx);
    // Show the load's sentence NOW: a fetch takes as long as the network
    // takes, and a person deserves to see what is being fetched while it
    // happens. NULL shows nothing until the face next draws.
    void (*progress)(void *ctx, const char *sentence);
} way_face_t;

typedef struct {
    char url[OS64_FETCH_URL_MAX];
    way_position_t position;
} way_crumb_t;

// A session is LARGE — its history is WAY_HISTORY_MAX addresses of up to
// OS64_FETCH_URL_MAX bytes each — so a face keeps one in static storage, as
// wend does, and never on a thread's stack.
typedef struct {
    const char *name;                   // how a sentence names this browser: "wend"
    const char *agent;                  // the User-Agent it sends
    const char *accept;                 // the Accept list, what it can render
    // What the face offers for a page that asks to send the reader on after
    // a delay, appended to the sentence saying so: wend's " - press g to
    // go". NULL or "" offers nothing.
    const char *delayed_hint;
    way_face_t face;
    char status[WAY_SENTENCE_MAX];      // the transient sentence
    way_crumb_t history[WAY_HISTORY_MAX];
    int32_t depth;
    // Where Forward goes: the pages Back left, newest last. Following
    // anything new empties it.
    way_crumb_t forward[WAY_HISTORY_MAX];
    int32_t ahead;
    // The browser's cookies (way/jar.h), shared by every fetch it makes on
    // any thread; the one thing a load writes that is not its own. NULL
    // keeps none and sends none.
    way_jar_t *jar;
} way_session_t;

// ONE LOAD'S WORTH OF THE SESSION. A load reads the browser's name and
// accept list (set before the first load and never changed while one
// runs), sends the agent its leg carries, writes only its own sentence,
// and reaches the person only through its own face. So a face may run
// way_load on a thread other than the session's: the leg is the whole of
// what the two share.
typedef struct {
    const way_session_t *session;
    // The User-Agent this load sends: the session's when way_leg made the
    // leg, or the one handed to way_leg_as. A face that lets the agent
    // change while loads run elsewhere (yonder's Settings) reads it on the
    // session's thread when it asks for the load and makes the leg with
    // way_leg_as, so nothing off that thread reads the session's.
    const char *agent;
    way_face_t face;
    // Parser noscript policy for this load, captured by the requesting face.
    // The finished-document loader resumes script stops without executing.
    bool scripting;
    // The pictures the face shows, as an Accept list of their types (yonder's
    // is libimage's OS64_IMAGE_ACCEPT), or NULL for none: a picture asked
    // for by itself, of a type it names, is a page, WAY_BODY_IMAGE, whose
    // body the face does not read — it writes the page that shows the
    // picture. Any other picture is not a page.
    const char *pictures;
    char status[WAY_SENTENCE_MAX];
    // The page this load was asked for from — a link followed, a form sent,
    // a refresh — which its Referer names (way_referrer). Empty for an
    // address typed, Back, Forward and Reload.
    char referrer[OS64_FETCH_URL_MAX];
} way_leg_t;

// A leg for `session`, with the session's own face and agent and an empty
// sentence. way_leg reads the session's agent; way_leg_as is handed one
// read earlier, on the session's thread, and reads only what never
// changes while a load runs.
way_leg_t way_leg(const way_session_t *session);
way_leg_t way_leg_as(const way_session_t *session, const char *agent);

// A PAGE'S SCRIPT's cookies (document.cookie), for the page at `page_url`,
// at the system's clock: what it may read, at most `cap` bytes with the
// NUL, answering the length written and whether that is all of it (false:
// ask again with more room), and one cookie it sets ("name=value;
// attributes", as a Set-Cookie value). The jar's script rules apply
// (jar.h); a page that is not http or https reads "" and sets nothing.
size_t way_script_cookies(way_jar_t *jar, const char *page_url, char *out, size_t cap,
                          bool *whole);
void way_script_cookie(way_jar_t *jar, const char *page_url, const char *text, size_t len);

// ── The I/O half ────────────────────────────────────────────────────────
//
// A LOAD IN PIECES (docs/design/pending/DOM_D4.md). way_load is the four
// pieces below in a row, on one thread, which is how wend loads. yonder
// runs the first two on a worker and the last two on its window: the
// parser lives on the thread that will run the page's scripts (DOM.md
// ruling 5), and the worker carries only bytes. Each judgement is written
// once, here, so the two faces cannot drift.

// Whether an Accept-style list ("a/b, c/d;q=0.5") names `type` exactly,
// parameters aside.
bool way_accept_names(const char *list, const char *type);

// What a body is, judged from its head. A head that is none of these is not
// a page: way_open says so and refuses. IMAGE only for a leg whose face shows
// pictures, and never for the reply to a POST.
typedef enum { WAY_BODY_HTML = 0, WAY_BODY_TEXT, WAY_BODY_IMAGE } way_body_t;

// The head, COPIED out of the fetch: os64_fetch_head_t is storage inside
// the fetch and dies with it, and the note is written from the head after
// the fetch is closed, on another thread.
typedef struct {
    int32_t status;
    char reason[HTTP_REASON_MAX];
    char content_type[HTTP_TYPE_MAX];
    char charset[HTTP_CHARSET_MAX];
    char url[OS64_FETCH_URL_MAX];       // where the body came from, after redirects
    bool posted;                        // the FINAL method was POST
    uint16_t tls_version;
    bool tls_fallback;
    way_body_t body;
} way_head_t;

// A fetch open with its head read and judged a page. The fetch is the
// caller's to read (way_read) and to close (os64_fetch_close).
typedef struct {
    os64_fetch_t *fetch;
    way_head_t head;
    char asked[OS64_FETCH_URL_MAX];     // the address asked for, which the sentences name
    uint64_t shown;                     // bytes read when the sentence was last said
} way_opening_t;

// Opens `url` with the leg's own options (agent, accept, the parser's byte
// cap, the downgrade question, cookies and Referer, a POST's body when
// `request` is a form), reads the head and judges it. True leaves the fetch
// open in `out`. False closes it: a head that never came (`why` says what
// stopped it), or a reply that is not a page, with the sentence saying what
// it is and how to keep it in `leg->status`.
bool way_open(way_leg_t *leg, const char *url, const os64_page_request_t *request,
              way_opening_t *out, os64_fetch_status_t *why);

// One read of the body, os64_fetch_read's answer, and the "reading N KB"
// sentence through the leg's face every 64 KB.
int64_t way_read(way_leg_t *leg, way_opening_t *opening, void *buf, size_t cap);

// Whether a text/plain body is UTF-8: by its label, else by JSON's own
// rule, else by a byte order mark in its first bytes.
bool way_text_utf8(const way_head_t *head, const void *first, size_t n);
// Whether that answer waits on the body: nothing in the head decides, so
// the first three bytes do, and a reader must have them whole before asking.
bool way_text_sniffs(const way_head_t *head);

// The page's address, whether it was posted, and its standing line: the
// status and reason, then a sentence for every way it is incomplete — our
// memory (`short_of_memory`), the wire (`fetch` and its `reason` sentence,
// os64_fetch_reason's, read only when `fetch` is not OK; a NULL there says
// only that the fetch did not finish), and the parser (`page->doc->refusal`,
// when there is a doc).
void way_note(way_page_t *page, const way_head_t *head, bool short_of_memory,
              os64_fetch_status_t fetch, const char *reason);

// Fetch and parse one address into `out`, which the caller zeroed.
// `request` is the form a page is sending, or NULL for a GET of `url`: a
// POST's body and content type go out with it, and must stay alive until
// this returns. False when there is nothing to show, with the reason in
// `leg->status` and `out` empty. True leaves the page in `out` and its note
// written; the face lays it out and says what laying out found. `why` (may
// be NULL) is the fetch's own verdict, for a face that turns a failed first
// page into an exit code.
bool way_load(way_leg_t *leg, const char *url, const os64_page_request_t *request,
              way_page_t *out, os64_fetch_status_t *why);

// For a fetch a face makes itself — a picture a page names — the same
// cookies and Referer a page's own fetch carries, and the browser's cache.
// way_fetch_hooks takes over `opt`'s headers_for, on_set_cookie, cancelled
// and ctx: the caller's own cancellation goes in the hooks instead, and the
// hooks must live until the fetch is closed.
typedef struct {
    way_jar_t *jar;
    way_cache_t *cache;                     // NULL keeps nothing
    char referrer[OS64_FETCH_URL_MAX];      // "" for none
    bool (*cancelled)(void *ctx);
    void *cancel_ctx;
    // way_fetch_whole's own: a redirect on the way that was not permanent
    // (301, 308), which makes the reply one not to keep under the address
    // asked for; and what the permanent ones said about keeping them.
    bool passing_hop;
    way_chain_t chain;
} way_hooks_t;

void way_fetch_hooks(way_hooks_t *hooks, os64_fetch_options_t *opt);

// Where a whole body came from.
typedef enum {
    WAY_SERVED_NETWORK = 0,
    WAY_SERVED_KEPT,            // the cache, fresh: nothing went out
    WAY_SERVED_CONFIRMED,       // the cache, after the server answered 304
    WAY_SERVED_STALE,           // the cache, stale, because the server could not be reached
} way_served_t;

// A body read whole, and what came with it. `bytes` is os64_malloc'd and
// the caller's to free.
typedef struct {
    uint8_t *bytes;
    size_t len;
    int32_t status;
    char content_type[HTTP_TYPE_MAX];
    char charset[HTTP_CHARSET_MAX];
    char url[OS64_FETCH_URL_MAX];           // where it came from: a base for what it names
    way_served_t served;
    os64_fetch_status_t fetch;              // the fetch's verdict, when there was one
} way_whole_t;

// A GET of `url` read whole, of at most `cap` bytes, through the hooks'
// cache when it has one (CACHE.md): served from it while fresh, asked
// again with its validator when stale (fetched whole, when it came through
// redirects), kept when the reply allows. `opt`
// carries the rest of the request — agent, accept, extra headers — and is
// the caller's; the hooks are set on it here. True when there is a body
// whole, whatever its status: a 404's page is a body. False with
// `out->fetch` saying why when there is none, or when it was longer than
// `cap`; `out` holds no bytes then.
bool way_fetch_whole(way_hooks_t *hooks, const char *url, os64_fetch_options_t *opt,
                     size_t cap, way_whole_t *out);

// ── Pages ───────────────────────────────────────────────────────────────

// Frees what the page holds and leaves it empty.
void way_page_clear(way_page_t *page);

// Opens or closes a `details` for the reader. False on no memory.
bool way_details_flip(way_page_t *page, const os64_html_node_t *details);

// ── The history ─────────────────────────────────────────────────────────

// Remembers the page being left, and where on it the person was. This is
// a NEW way, so Forward has nowhere left to go.
void way_remember(way_session_t *session, const char *url, const way_position_t *where);

// The crumb Back would return to. False, with the sentence, when there is
// none. The crumb stays until the face says it went: Back is a fetch, and
// one that fails must leave the history as it was.
bool way_last(way_session_t *session, way_crumb_t *out);
// Back arrived and nothing is kept for Forward (wend has no Forward).
void way_forget_last(way_session_t *session);
// Back arrived: the crumb is spent, and the page left is where Forward goes.
void way_went_back(way_session_t *session, const char *url, const way_position_t *where);

// The crumb Forward would go to, and its arrival, the mirror of Back's:
// the page left joins the history without emptying what is ahead.
bool way_next(way_session_t *session, way_crumb_t *out);
void way_went_forward(way_session_t *session, const char *url, const way_position_t *where);

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
