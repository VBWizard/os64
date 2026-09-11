#ifndef HTML_INTERNAL_H
#define HTML_INTERNAL_H
#include "html/html.h"
#include "os64/mem.h"
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
typedef struct {
    os64_html_document_t pub;
    HBlock *blocks;
    size_t budget;
    unsigned char *permanent;
    size_t permanent_left, permanent_chunk;
    HNode root, html;
} HDoc;
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
};

void *h_permanent(os64_html_parser_t *p, size_t size);
void *h_alloc(os64_html_parser_t *p, size_t size);
void h_free(HDoc *d, void *ptr);
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
