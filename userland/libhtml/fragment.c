#include "internal.h"

/* The parser builds on an isolated ledger. Only a complete, reachable result
 * crosses into the caller's document; scaffolding and ignored token payloads
 * never become permanent storage in that document. */
typedef struct {
    const HNode *from;
    HNode *to;
} FPair;

static void answer(int64_t *status, int64_t why)
{
    if (status)
        *status = why;
}
static HDoc *isolated(HDoc *owner, size_t budget, int64_t *why)
{
    if (budget < sizeof(HDoc)) {
        *why = OS64_HTML_ARENA_EXHAUSTED;
        return NULL;
    }
    HDoc *d = os64_calloc(1, sizeof(*d));
    if (!d) {
        *why = OS64_HTML_NO_MEMORY;
        return NULL;
    }
    d->budget = budget;
    d->id = owner->id;
    d->version = owner->version;
    d->max_depth = owner->max_depth;
    d->pub.quirks = owner->pub.quirks;
    d->pub.arena_bytes = d->pub.peak_arena_bytes = sizeof(*d);
    return d;
}
static bool html_named(const HNode *n, const char *name)
{
    return n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_HTML && h_eq(n->name, name);
}
static bool size_add(os64_html_parser_t *p, size_t *size, size_t extra)
{
    if (extra > SIZE_MAX - *size) {
        h_refuse(p, OS64_HTML_ARENA_EXHAUSTED);
        return false;
    }
    *size += extra;
    return true;
}
static bool string_size(os64_html_parser_t *p, const char *str, size_t *size)
{
    if (!str)
        return true;
    size_t n = h_len(str);
    return h_work(p, n + 1) && size_add(p, size, n + 1);
}
static const char *pack_string(char **at, const char *from)
{
    if (!from)
        return NULL;
    char *out = *at;
    do {
        *(*at)++ = *from;
    } while (*from++);
    return out;
}
/* Original names and attributes belong to this node's single ledger block.
 * COW and clone copy these inline attributes, so another node cannot keep a
 * pointer into this allocation after its owner is reclaimed. */
static HNode *pack_node(os64_html_parser_t *p, const HNode *from, bool root)
{
    if (!h_work(p, 1))
        return NULL;
    size_t size = sizeof(HNode) + sizeof(HMeta), attrs = 0;
    if (!root) {
        for (const HAttr *a = from->attrs; a; a = a->next) {
            if (!h_work(p, 1) || !size_add(p, &size, sizeof(HAttr)))
                return NULL;
            attrs++;
        }
        if (!string_size(p, from->name, &size) || !string_size(p, from->public_id, &size) ||
            !string_size(p, from->system_id, &size))
            return NULL;
        for (const HAttr *a = from->attrs; a; a = a->next)
            if (!string_size(p, a->name, &size) || !string_size(p, a->value, &size))
                return NULL;
    }
    int64_t why = OS64_HTML_OK;
    HNode *n = d_node_alloc(p->d, size, &why);
    if (!n) {
        h_refuse(p, why);
        return NULL;
    }
    p->d->pub.node_count++;
    n->document_id = p->d->id;
    n->kind = root ? OS64_HTML_FRAGMENT : from->kind;
    if (root)
        return n;
    n->ns = from->ns;
    n->tag = from->tag;
    HAttr *records = (HAttr *)(h_meta(n) + 1);
    char *at = (char *)(records + attrs);
    n->name = pack_string(&at, from->name);
    n->public_id = pack_string(&at, from->public_id);
    n->system_id = pack_string(&at, from->system_id);
    size_t i = 0;
    for (const HAttr *a = from->attrs; a; a = a->next, i++) {
        HAttr *copy = records + i;
        copy->name = pack_string(&at, a->name);
        copy->value = pack_string(&at, a->value);
        copy->ns = a->ns; /* parser namespace URIs are static grammar strings */
        copy->next = i + 1 < attrs ? copy + 1 : NULL;
    }
    if (n->kind == OS64_HTML_ELEMENT) {
        n->attrs = attrs ? records : NULL;
        *h_word(n) = H_ATTRS_INLINE;
        p->d->form_inputs += d_form_input(n);
    }
    if (n->kind == OS64_HTML_TEXT || n->kind == OS64_HTML_COMMENT) {
        if (!h_work(p, from->text_len + 1))
            return NULL;
        if (from->text_len == SIZE_MAX) {
            h_refuse(p, OS64_HTML_ARENA_EXHAUSTED);
            return NULL;
        }
        char *text = h_alloc(p, from->text_len + 1);
        if (!text)
            return NULL;
        for (size_t j = 0; j < from->text_len; j++)
            text[j] = from->text[j];
        n->text = text;
        n->text_len = from->text_len;
        *h_word(n) = from->text_len + 1;
    }
    return n;
}
static HNode *copy_result(os64_html_parser_t *p, const HNode *root, size_t maximum)
{
    if (!maximum || maximum > SIZE_MAX / sizeof(FPair)) {
        h_refuse(p, OS64_HTML_ARENA_EXHAUSTED);
        return NULL;
    }
    FPair *pairs = h_alloc(p, maximum * sizeof(*pairs));
    if (!pairs)
        return NULL;
    HNode **map = h_alloc(p, maximum * sizeof(*map));
    size_t *depth = h_alloc(p, maximum * sizeof(*depth));
    if (!map || !depth)
        return NULL;
    pairs[0] = (FPair){root, pack_node(p, root, true)};
    if (!pairs[0].to)
        return NULL;
    size_t count = 1;
    /* Breadth first copying keeps the C stack independent of tree depth.
     * Template fragments are separate nodes with a host rather than parent. */
    for (size_t i = 0; i < count && !p->d->pub.refusal; i++) {
        const HNode *from = pairs[i].from;
        HNode *to = pairs[i].to;
        const HNode *child = from->template_contents ? from->template_contents : from->first_child;
        bool contents = from->template_contents != NULL;
        for (; child; child = contents ? NULL : child->next) {
            if (!h_work(p, 1))
                break;
            if (count == maximum) {
                h_refuse(p, OS64_HTML_ARENA_EXHAUSTED);
                break;
            }
            if (depth[i] >= h_depth_limit(p->d)) {
                h_refuse(p, OS64_HTML_TOO_DEEP);
                break;
            }
            HNode *copy = pack_node(p, child, false);
            if (!copy)
                break;
            pairs[count] = (FPair){child, copy};
            map[h_meta(child)->ordinal] = copy;
            depth[count++] = depth[i] + 1;
            if (contents) {
                to->template_contents = copy;
                *h_word(copy) = (size_t)to;
            } else
                h_attach(to, NULL, copy);
        }
    }
    for (size_t i = 1; i < count && !p->d->pub.refusal; i++) {
        if (!h_work(p, 1))
            break;
        const HNode *form = pairs[i].from->form_owner;
        /* Association only records a form in the isolated insertion tree.
         * Borrowed ancestor forms are suppression sentinels in another tree.
         * A form left unreachable by parser repair has no final map entry. */
        if (form) {
            uint64_t steps = 0;
            HNode *form_root = d_root((HNode *)form, &steps);
            if (!h_work(p, steps + 1))
                break;
            if (form_root != root->parent)
                continue;
            size_t ordinal = h_meta(form)->ordinal;
            if (ordinal < maximum && map[ordinal]) {
                pairs[i].to->form_owner = map[ordinal];
                p->d->records++;
            }
        }
    }
    HNode *out = p->d->pub.refusal ? NULL : pairs[0].to;
    h_free(p->d, pairs);
    h_free(p->d, map);
    h_free(p->d, depth);
    return out;
}
HNode *h_parse_fragment(os64_html_document_t *doc, const HNode *context,
                       const char *utf8, size_t len, bool scripting,
                       uint64_t max_work, uint64_t *work, int64_t *status)
{
    if (work)
        *work = 0;
    HDoc *owner = (HDoc *)doc;
    if (!owner || !context || context->kind != OS64_HTML_ELEMENT ||
        !os64_html_owns_node(doc, context) || (!utf8 && len)) {
        answer(status, OS64_HTML_BAD_ARGUMENT);
        return NULL;
    }
    int64_t why = OS64_HTML_OK;
    size_t remaining = owner->budget - owner->pub.arena_bytes;
    HDoc *temp = isolated(owner, remaining, &why), *stage = NULL;
    HNode *out = NULL;
    if (!temp)
        goto done;
    os64_html_parser_t bootstrap = {0};
    bootstrap.d = temp;
    os64_html_parser_t *p = h_alloc(&bootstrap, sizeof(*p));
    if (!p) {
        why = temp->pub.refusal;
        goto done;
    }
    p->d = temp;
    p->opt = os64_html_options_default();
    p->opt.max_bytes = len;
    p->opt.max_arena_bytes = remaining;
    p->opt.max_depth = owner->max_depth;
    p->opt.max_work = max_work;
    p->opt.scripting = scripting;
    p->fragment_context = context;
    p->frameset_ok = true;
    p->started = p->straight = true;
    p->utf16_byte = -1;
    /* Zero encoding is the decoder's UTF-8 state, without a sniff/BOM pass. */
    temp->root.kind = OS64_HTML_DOCUMENT;
    temp->html.kind = OS64_HTML_ELEMENT;
    temp->html.ns = OS64_HTML_NS_HTML;
    temp->html.tag = OS64_HTML_TAG_HTML;
    temp->html.name = "html";
    temp->root.document_id = temp->html.document_id = owner->id;
    temp->pub.document = &temp->root;
    temp->pub.html = &temp->html;
    temp->pub.node_count = 2;
    d_node_register(temp, &temp->root, H_NODE_EMBEDDED);
    d_node_register(temp, &temp->html, H_NODE_EMBEDDED);
    temp->parser = p;
    h_attach(&temp->root, NULL, &temp->html);
    if (!h_work(p, 1))
        goto parsed;
    if (!owner->max_depth) {
        h_refuse(p, OS64_HTML_TOO_DEEP);
        goto parsed;
    }
    if (!h_nodes_push(p, &p->stack, &temp->html))
        goto parsed;
    for (const HNode *n = context; n; n = n->parent) {
        if (!h_work(p, 1))
            goto parsed;
        if (html_named(n, "form")) {
            p->form = (HNode *)n; /* read-only sentinel; never on the open stack */
            break;
        }
    }
    if (context->ns == OS64_HTML_NS_HTML) {
        if (h_in(context->name, "title textarea"))
            p->state = T_RCDATA;
        else if (h_in(context->name, "style xmp iframe noembed noframes") ||
                 (scripting && h_eq(context->name, "noscript")))
            p->state = T_RAWTEXT;
        else if (h_eq(context->name, "script"))
            p->state = T_SCRIPT;
        else if (h_eq(context->name, "plaintext"))
            p->state = T_PLAIN;
    }
    h_fragment_start(p);
    for (size_t i = 0; i < len && !temp->pub.refusal; i++)
        h_decode(p, (unsigned char)utf8[i], i);
    temp->pub.input_bytes = len;
    h_decode_finish(p);
parsed:
    if (work)
        *work = temp->pub.work;
    why = temp->pub.refusal;
    if (why)
        goto done;
    stage = isolated(owner, remaining - temp->pub.arena_bytes, &why);
    if (!stage)
        goto done;
    os64_html_parser_t copier = {0};
    copier.d = stage;
    copier.opt.max_work = max_work;
    stage->pub.work = temp->pub.work;
    out = copy_result(&copier, &temp->html, temp->pub.node_count);
    if (work)
        *work = stage->pub.work;
    why = stage->pub.refusal;
    if (!out)
        goto done;
    size_t peak = temp->pub.arena_bytes + stage->pub.peak_arena_bytes;
    if (temp->pub.peak_arena_bytes > peak)
        peak = temp->pub.peak_arena_bytes;
    peak += owner->pub.arena_bytes;
    if (peak > owner->pub.peak_arena_bytes)
        owner->pub.peak_arena_bytes = peak;
    d_transfer_blocks(owner, stage);
done:
    if (stage)
        os64_html_document_free(&stage->pub);
    if (temp)
        os64_html_document_free(&temp->pub);
    answer(status, why);
    return why ? NULL : out;
}
os64_html_node_t *os64_html_parse_fragment(os64_html_document_t *doc,
                                           const os64_html_node_t *context,
                                           const char *utf8, size_t len,
                                           bool scripting, int64_t *status)
{
    uint64_t budget = len > (UINT64_MAX - 4096) / 256 ? UINT64_MAX : 4096 + 256 * (uint64_t)len;
    return h_parse_fragment(doc, context, utf8, len, scripting, budget, NULL, status);
}
