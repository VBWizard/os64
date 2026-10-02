#ifndef HTML_INTERNAL_H
#define HTML_INTERNAL_H
#include "html/html.h"
#include "os64/mem.h"
#include <stddef.h>
#include "os64/str.h"

#define H_EOF 0xffffffffu
#define H_ARRAY(a) (sizeof(a) / sizeof((a)[0]))
typedef os64_html_node_t HNode;
typedef os64_html_attr_t HAttr;
typedef struct {
    char *s;
    size_t len, cap;
} HBuf;
typedef struct HBlock HBlock;
/* Snapshots that borrow from the tree, each by the version it was built at. */
#define H_PINS 16
typedef struct {
    os64_html_document_t pub;
    HBlock *blocks;
    size_t budget;
    unsigned char *permanent;
    size_t permanent_left, permanent_chunk;
    /* A node is followed by one private word (h_word). The two nodes that
     * live here and not in a chunk carry theirs beside them. */
    HNode root;
    size_t root_word;
    HNode html;
    size_t html_word;
    /* What each node of this document carries in `document_id`, and no other
     * document's does (h_document_serial). A node's memory and its strings
     * go when its document is freed, and its replaced strings are retired on
     * its document's ledger, so a verb has to tell its own document's nodes
     * from another's. Never zero, which is what a node nobody stamped
     * carries. */
    uint32_t id;
    /* Set by a verb that moves a node. A parser still building the document
     * reads it: the tree is then no longer what its stack of open elements
     * says it is, so it stops relying on the stack for what a link needs to
     * be true (tree.c, attach). */
    bool disturbed;
    /* How many of `pins` are held, so that the parser's every character need
     * not look through them when none is. */
    uint8_t pinned;
    /* DOM.md, the mutation core. `version` moves whenever a verb changes
     * what a reader can see, and once in each call of a parser that parses
     * anything. It is never zero, so a zero pin is a free slot.
     * `retired` holds replaced blocks a pinned snapshot may still point at. */
    uint64_t version;
    uint64_t pins[H_PINS];
    os64_html_pin_t pin_handles[H_PINS];    /* what os64_html_pin answered for each held slot */
    HBlock *retired;
    size_t retired_bytes;
    size_t records;     /* live form_owner records: a move with none skips its walk */
    size_t max_depth;   /* the parse's limit on open elements: see h_depth_limit */
} HDoc;
_Static_assert(offsetof(HDoc, root_word) == offsetof(HDoc, root) + sizeof(HNode) &&
               offsetof(HDoc, html_word) == offsetof(HDoc, html) + sizeof(HNode),
               "an embedded node's private word must sit where h_word looks for it");

/* The private word after a node: a TEXT or COMMENT's buffer capacity (zero
 * when its bytes are a literal or lie in a permanent chunk, so are never
 * freed alone); an ELEMENT's flags; a template-contents FRAGMENT's host. */
static inline size_t *h_word(const HNode *n)
{
    return (size_t *)(n + 1);
}
#define H_ATTRS_PRIVATE ((size_t)1)   /* ELEMENT: its attribute records are its own blocks */

/* How deep a node may be, the document at 0 and a template's contents
 * counted as its children. `max_depth` bounds the parser's stack of OPEN
 * elements, and a tree can be deeper than its stack ever was: `</form>`
 * takes the form off the stack while what it contains stays open, so
 * `<form><div></form>` over and over nests two levels for each entry the
 * stack keeps. A removed form is never the parent of another, which makes
 * twice the stack the most a parse can build; the verbs hold the tree to
 * the same number, so no walker meets a tree a parse could not have made. */
static inline size_t h_depth_limit(const HDoc *d)
{
    return d->max_depth > (SIZE_MAX - 2) / 2 ? SIZE_MAX : 2 * d->max_depth + 2;
}
typedef enum { H_CHAR, H_START, H_END, H_COMMENT, H_DOCTYPE, H_END_INPUT } HType;
typedef struct {
    HType type;
    HBuf name, data, public_id, system_id;
    HAttr *attrs, *last_attr;
    uint32_t ch;
    bool self_closing, acknowledged, force_quirks, has_public, has_system;
} HToken;
typedef enum {
    T_DATA,
    T_RCDATA,
    T_RAWTEXT,
    T_SCRIPT,
    T_PLAIN,
    T_TAG_OPEN,
    T_END_OPEN,
    T_TAG_NAME,
    T_RC_LT,
    T_RC_END_OPEN,
    T_RC_END_NAME,
    T_RAW_LT,
    T_RAW_END_OPEN,
    T_RAW_END_NAME,
    T_SCRIPT_LT,
    T_SCRIPT_END_OPEN,
    T_SCRIPT_END_NAME,
    T_ESCAPE_START,
    T_ESCAPE_START_DASH,
    T_ESCAPED,
    T_ESCAPED_DASH,
    T_ESCAPED_DASH_DASH,
    T_ESCAPED_LT,
    T_ESCAPED_END_OPEN,
    T_ESCAPED_END_NAME,
    T_DOUBLE_START,
    T_DOUBLE,
    T_DOUBLE_DASH,
    T_DOUBLE_DASH_DASH,
    T_DOUBLE_LT,
    T_DOUBLE_END,
    T_BEFORE_ATTR,
    T_ATTR_NAME,
    T_AFTER_ATTR,
    T_BEFORE_VALUE,
    T_VALUE_DQ,
    T_VALUE_SQ,
    T_VALUE_UQ,
    T_AFTER_VALUE,
    T_SELF_CLOSE,
    T_BOGUS_COMMENT,
    T_DECLARATION,
    T_COMMENT_START,
    T_COMMENT_START_DASH,
    T_COMMENT,
    T_COMMENT_LT,
    T_COMMENT_LT_BANG,
    T_COMMENT_LT_BANG_DASH,
    T_COMMENT_LT_BANG_DASH_DASH,
    T_COMMENT_END_DASH,
    T_COMMENT_END,
    T_COMMENT_END_BANG,
    T_DOCTYPE,
    T_BEFORE_DOCTYPE_NAME,
    T_DOCTYPE_NAME,
    T_AFTER_DOCTYPE_NAME,
    T_DOCTYPE_KEYWORD,
    T_AFTER_PUBLIC,
    T_BEFORE_PUBLIC,
    T_PUBLIC_DQ,
    T_PUBLIC_SQ,
    T_AFTER_PUBLIC_ID,
    T_BETWEEN_IDS,
    T_AFTER_SYSTEM,
    T_BEFORE_SYSTEM,
    T_SYSTEM_DQ,
    T_SYSTEM_SQ,
    T_AFTER_SYSTEM_ID,
    T_BOGUS_DOCTYPE,
    T_CDATA,
    T_CDATA_BRACKET,
    T_CDATA_END,
    T_REFERENCE,
    T_NAMED_REFERENCE,
    T_AMBIGUOUS,
    T_NUMERIC,
    T_HEX_START,
    T_DEC_START,
    T_HEX,
    T_DEC
} HState;
typedef enum {
    M_INITIAL,
    M_BEFORE_HTML,
    M_BEFORE_HEAD,
    M_HEAD,
    M_HEAD_NOSCRIPT,
    M_AFTER_HEAD,
    M_BODY,
    M_TEXT,
    M_TABLE,
    M_TABLE_TEXT,
    M_CAPTION,
    M_COLGROUP,
    M_TABLE_BODY,
    M_ROW,
    M_CELL,
    M_SELECT,
    M_SELECT_TABLE,
    M_TEMPLATE,
    M_AFTER_BODY,
    M_FRAMESET,
    M_AFTER_FRAMESET,
    M_AFTER_AFTER_BODY,
    M_AFTER_AFTER_FRAMESET
} HMode;
typedef struct {
    HNode **v;
    size_t n, cap;
} HNodes;
struct os64_html_parser {
    HDoc *d;
    os64_html_options_t opt;
    size_t offset;
    HState state, return_state;
    HMode mode, original_mode;
    HToken token;
    HBuf attr_name, attr_value, temporary, last_start, table_text;
    bool attr_active, duplicate_attr, foster, frameset_ok, ignore_lf;
    HNodes stack, formatting;
    HMode *templates;
    size_t templates_n, templates_cap;
    HNode *form, *head;
    bool started, eof_sent, previous_cr;
    unsigned char prescan[1024];
    size_t prescan_len;
    unsigned encoding;
    uint32_t decoder_cp, decoder_min, high_surrogate;
    unsigned decoder_need, decoder_seen, decoder_first_min, decoder_first_max;
    size_t decoder_offset, surrogate_offset;
    int utf16_byte;
    HBuf charset_label;
    uint32_t number;
    size_t reference_match, reference_index;
    bool reference_hex;
    void (*token_sink)(struct os64_html_parser *, const HToken *, void *);
    void *sink_context;
    /* Scripting (DOM.md, the parser with scripting on). `script` is the
     * element the parse is stopped at. Input that arrives while it is stopped
     * waits in `hold`, read from `hold_at`. The hold is input and not tree:
     * it is the heap's and not the arena's, so that a document costs the
     * same arena however its bytes were cut, and `max_bytes` is what bounds
     * it. `parsed` is how many bytes of the input have gone to the decoder
     * or been skipped as a byte order mark: the offset of the next one, and
     * below `prescan_len` the place the sniff window has been replayed to. */
    HNode *script;
    unsigned char *hold;
    size_t hold_len, hold_cap, hold_at, parsed;
    /* `ended`: the host said the input is over. `cut`: it ran past
     * `max_bytes`. Either is acted on when everything waiting has been
     * parsed. `straight`: os64_html_parser_finish is running, which does not
     * stop. `moved`: this call has moved the document's version. */
    bool ended, cut, straight, moved;
};

void *h_permanent(os64_html_parser_t *p, size_t size);
void *h_alloc(os64_html_parser_t *p, size_t size);
void h_free(HDoc *d, void *ptr);
/* The same two allocators for a caller with no parser: the failure is the
 * caller's answer (`*why`) and the document's `refusal`, which records how
 * the parse ended, is left alone. */
void *d_permanent(HDoc *d, size_t size, int64_t *why);
void *d_alloc(HDoc *d, size_t size, int64_t *why);
/* Give up a ledger block something may have borrowed: freed at once when no
 * pinned snapshot is older than the change that replaced it, else kept
 * until the last such pin lets go. Call it after bumping `version`. */
void d_retire(HDoc *d, void *ptr);
/* One more character on a TEXT node, which a snapshot taken between two
 * calls of the parser may be reading (core.c). */
bool h_text_put(os64_html_parser_t *p, HNode *n, uint32_t cp);
/* Parse what is waiting, as far as the next stop (core.c). */
void h_pump(os64_html_parser_t *p);
/* Said before a call of the parser parses its first byte, or the end of the
 * input: the document's version moves, once for the call (core.c). */
void h_moving(os64_html_parser_t *p);

/* What the parser asks of the verbs' own rules (dom.c). One that takes
 * `steps` adds what it walked, for the parser to charge as work.
 *
 * `d_place` links `node` under `parent` before `before` when the verbs'
 * validity rules allow it, and answers which rule did not. `d_unlink` takes
 * a node out of its parent, parting controls from forms as a verb's remove
 * does. Both keep the landmarks true. `d_place` leaves form owners alone:
 * the caller that has moved a node which had a parent says so afterwards
 * with `d_parted`, naming the root of the tree the node began in
 * (`d_root`, asked before the move). */
int64_t d_place(HDoc *d, HNode *parent, HNode *node, HNode *before, uint64_t *steps);
void d_unlink(HDoc *d, HNode *node, uint64_t *steps);
HNode *d_root(HNode *n, uint64_t *steps);
void d_parted(HDoc *d, HNode *moved, HNode *old_root, uint64_t *steps);
/* Seat the document's own `html` element if the document has no element. */
void d_seat_html(HDoc *d);
/* `to` takes the attribute list of `from`, which a parser's clone shares
 * unless a verb has made that list `from`'s own. */
bool d_attrs_inherit(HDoc *d, const HNode *from, HNode *to, int64_t *why);
/* One more attribute on an element that has none of that name. */
int64_t d_attr_add(HDoc *d, HNode *e, const HAttr *a);
/* How many documents the program has begun (core.c). Here so that a test can
 * set it near its end. */
extern uint32_t h_document_serial;
bool h_buf_put(os64_html_parser_t *p, HBuf *b, uint32_t cp);
bool h_buf_bytes(os64_html_parser_t *p, HBuf *b, const char *s, size_t n);
void h_buf_reset(HBuf *b);
char *h_copy(os64_html_parser_t *p, const char *s, size_t n);
bool h_work(os64_html_parser_t *p, uint64_t n);
void h_error(os64_html_parser_t *p, const char *name);
void h_refuse(os64_html_parser_t *p, int64_t status);
bool h_eq(const char *a, const char *b);
bool h_in(const char *name, const char *set);
size_t h_len(const char *s);
bool h_space(uint32_t cp);
bool h_alpha(uint32_t cp);
uint32_t h_lower(uint32_t cp);
bool h_nodes_push(os64_html_parser_t *p, HNodes *list, HNode *n);
HNode *h_current(os64_html_parser_t *p);
HNode *h_node(os64_html_parser_t *p, os64_html_node_kind_t kind);
void h_detach(HNode *n);
void h_attach(HNode *parent, HNode *before, HNode *n);
void h_tokenize(os64_html_parser_t *p, uint32_t cp);
void h_emit(os64_html_parser_t *p);
void h_tree(os64_html_parser_t *p, HToken *token);
void h_decode(os64_html_parser_t *p, unsigned char byte, size_t offset);
void h_decode_finish(os64_html_parser_t *p);
void h_encoding_start(os64_html_parser_t *p);
void h_codepoint(os64_html_parser_t *p, uint32_t cp);
void h_late_meta(os64_html_parser_t *p, HNode *n);
#endif
