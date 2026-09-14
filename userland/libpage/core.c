// core.c — the arena, the lookups, and the walk that builds the model.
//
// THE TREE IS WALKED AT BUILD AND NEVER AGAIN. A face draws by walking it
// itself and asks, per node, "which control is this?" — so every question
// has to be answerable from a table, because answering it with a second walk
// would be one rule with two implementations skipping different subtrees.
//
// Build takes three passes and they are ordered by what each needs from the
// last: the IDS first, because a `form=` names an element by one; the FORMS
// next, because a control's owner is settled as the control is met; the
// MODEL last.

#include "internal.h"

// ── The arena ───────────────────────────────────────────────────────────

struct PBlock {
    PBlock *next;
    size_t used, cap;
    unsigned char bytes[];
};

#define P_BLOCK_MIN 4096

void *p_arena_alloc(PArena *arena, size_t size)
{
    size = (size + 15u) & ~(size_t)15u;
    if (size == 0)
        size = 16;
    if (arena->blocks == NULL || arena->blocks->cap - arena->blocks->used < size) {
        size_t cap = size > P_BLOCK_MIN ? size : P_BLOCK_MIN;
        PBlock *block = os64_malloc(sizeof(*block) + cap);
        if (block == NULL)
            return NULL;
        block->next = arena->blocks;
        block->used = 0;
        block->cap = cap;
        arena->blocks = block;
    }
    void *at = arena->blocks->bytes + arena->blocks->used;
    arena->blocks->used += size;
    return at;
}

char *p_arena_copy(PArena *arena, const char *s, size_t n)
{
    char *out = p_arena_alloc(arena, n + 1);
    if (out == NULL)
        return NULL;
    if (n != 0)
        os64_memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

void p_arena_free(PArena *arena)
{
    PBlock *block = arena->blocks;
    arena->blocks = NULL;
    while (block != NULL) {
        PBlock *next = block->next;
        os64_free(block);
        block = next;
    }
}

void *p_alloc(os64_page_t *page, size_t size)
{
    return p_arena_alloc(&page->arena, size);
}

char *p_copy(os64_page_t *page, const char *s, size_t n)
{
    return p_arena_copy(&page->arena, s, n);
}

bool p_grow(void **items, int32_t *cap, int32_t count, size_t size)
{
    if (count < *cap)
        return true;
    int32_t want = *cap != 0 ? *cap * 2 : 16;
    void *bigger = os64_realloc(*items, (size_t)want * size);
    if (bigger == NULL)
        return false;
    *items = bigger;
    *cap = want;
    return true;
}

// ── The lookups ─────────────────────────────────────────────────────────

static size_t hash_ptr(const void *p)
{
    uint64_t h = (uint64_t)(uintptr_t)p;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return (size_t)h;
}

static size_t hash_str(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s != '\0'; s++) {
        h ^= (unsigned char)*s;
        h *= 0x100000001b3ull;
    }
    return (size_t)h;
}

// Kept under three quarters full, which is where linear probing stops being
// a lookup and starts being a walk.
static bool crowded(size_t count, size_t cap)
{
    return (count + 1) * 4 >= cap * 3;
}

static bool ptrmap_grow(PPtrMap *map)
{
    size_t cap = map->cap != 0 ? map->cap * 2 : 32;
    const void **keys = os64_calloc(cap, sizeof(*keys));
    int32_t *vals = os64_calloc(cap, sizeof(*vals));
    if (keys == NULL || vals == NULL) {
        os64_free(keys);
        os64_free(vals);
        return false;
    }
    for (size_t i = 0; i < map->cap; i++) {
        if (map->keys[i] == NULL)
            continue;
        size_t at = hash_ptr(map->keys[i]) & (cap - 1);
        while (keys[at] != NULL)
            at = (at + 1) & (cap - 1);
        keys[at] = map->keys[i];
        vals[at] = map->vals[i];
    }
    os64_free(map->keys);
    os64_free(map->vals);
    map->keys = keys;
    map->vals = vals;
    map->cap = cap;
    return true;
}

bool p_ptrmap_put(PPtrMap *map, const void *key, int32_t val)
{
    if (key == NULL)
        return true;
    if (crowded(map->count, map->cap) && !ptrmap_grow(map))
        return false;
    size_t at = hash_ptr(key) & (map->cap - 1);
    while (map->keys[at] != NULL) {
        if (map->keys[at] == key) {
            map->vals[at] = val;
            return true;
        }
        at = (at + 1) & (map->cap - 1);
    }
    map->keys[at] = key;
    map->vals[at] = val;
    map->count++;
    return true;
}

int32_t p_ptrmap_get(const PPtrMap *map, const void *key)
{
    if (key == NULL || map->cap == 0)
        return -1;
    size_t at = hash_ptr(key) & (map->cap - 1);
    while (map->keys[at] != NULL) {
        if (map->keys[at] == key)
            return map->vals[at];
        at = (at + 1) & (map->cap - 1);
    }
    return -1;
}

void p_ptrmap_free(PPtrMap *map)
{
    os64_free(map->keys);
    os64_free(map->vals);
    map->keys = NULL;
    map->vals = NULL;
    map->cap = map->count = 0;
}

static bool strmap_grow(PStrMap *map)
{
    size_t cap = map->cap != 0 ? map->cap * 2 : 32;
    const char **keys = os64_calloc(cap, sizeof(*keys));
    const void **vals = os64_calloc(cap, sizeof(*vals));
    if (keys == NULL || vals == NULL) {
        os64_free(keys);
        os64_free(vals);
        return false;
    }
    for (size_t i = 0; i < map->cap; i++) {
        if (map->keys[i] == NULL)
            continue;
        size_t at = hash_str(map->keys[i]) & (cap - 1);
        while (keys[at] != NULL)
            at = (at + 1) & (cap - 1);
        keys[at] = map->keys[i];
        vals[at] = map->vals[i];
    }
    os64_free(map->keys);
    os64_free(map->vals);
    map->keys = keys;
    map->vals = vals;
    map->cap = cap;
    return true;
}

static bool strmap_store(PStrMap *map, const char *key, const void *val, bool replace)
{
    if (key == NULL || val == NULL)
        return true;
    if (crowded(map->count, map->cap) && !strmap_grow(map))
        return false;
    size_t at = hash_str(key) & (map->cap - 1);
    while (map->keys[at] != NULL) {
        if (os64_streq(map->keys[at], key)) {
            if (replace)
                map->vals[at] = val;
            return true;
        }
        at = (at + 1) & (map->cap - 1);
    }
    map->keys[at] = key;
    map->vals[at] = val;
    map->count++;
    return true;
}

bool p_strmap_put(PStrMap *map, const char *key, const void *val)
{
    return strmap_store(map, key, val, false);
}

bool p_strmap_set(PStrMap *map, const char *key, const void *val)
{
    return strmap_store(map, key, val, true);
}

const void *p_strmap_get(const PStrMap *map, const char *key)
{
    if (key == NULL || map->cap == 0)
        return NULL;
    size_t at = hash_str(key) & (map->cap - 1);
    while (map->keys[at] != NULL) {
        if (os64_streq(map->keys[at], key))
            return map->vals[at];
        at = (at + 1) & (map->cap - 1);
    }
    return NULL;
}

void p_strmap_free(PStrMap *map)
{
    os64_free(map->keys);
    os64_free(map->vals);
    map->keys = NULL;
    map->vals = NULL;
    map->cap = map->count = 0;
}

// ── Reading the tree ────────────────────────────────────────────────────

const char *p_attr(const os64_html_node_t *n, const char *name)
{
    if (n == NULL || n->kind != OS64_HTML_ELEMENT)
        return NULL;
    const os64_html_attr_t *a = os64_html_attr(n, name);
    return a != NULL ? a->value : NULL;
}

bool p_has_attr(const os64_html_node_t *n, const char *name)
{
    return p_attr(n, name) != NULL;
}

bool p_is(const os64_html_node_t *n, os64_html_tag_t tag)
{
    return n != NULL && n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_HTML &&
           n->tag == tag;
}

const os64_html_node_t *p_ancestor(const os64_html_node_t *n, os64_html_tag_t tag)
{
    for (const os64_html_node_t *up = n != NULL ? n->parent : NULL; up != NULL; up = up->parent)
        if (p_is(up, tag))
            return up;
    return NULL;
}

// ── Gathering text ──────────────────────────────────────────────────────

static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

// Every text descendant in tree order, `script` subtrees left out because
// the standard leaves them out of an element's text. `out` NULL measures.
static void gather(const os64_html_node_t *n, char *out, size_t *len)
{
    for (; n != NULL; n = n->next) {
        if (n->kind == OS64_HTML_TEXT && n->text != NULL) {
            if (out != NULL)
                os64_memcpy(out + *len, n->text, n->text_len);
            *len += n->text_len;
        } else if (n->kind == OS64_HTML_ELEMENT && !p_is(n, OS64_HTML_TAG_SCRIPT)) {
            gather(n->first_child, out, len);
        }
    }
}

char *p_subtree_text_into(PArena *arena, const os64_html_node_t *n, bool collapse,
                          size_t *out_len)
{
    size_t len = 0;
    gather(n != NULL ? n->first_child : NULL, NULL, &len);
    char *text = p_arena_alloc(arena, len + 1);
    if (text == NULL)
        return NULL;
    size_t at = 0;
    gather(n != NULL ? n->first_child : NULL, text, &at);
    text[at] = '\0';
    // The collapse happens over the gathered bytes rather than during the
    // gather, so the measuring pass and the filling pass cannot disagree
    // about how many bytes a run of whitespace takes.
    if (collapse) {
        size_t write = 0;
        for (size_t read = 0; read < at; read++) {
            if (space(text[read])) {
                if (write != 0)
                    text[write++] = ' ';
                while (read + 1 < at && space(text[read + 1]))
                    read++;
                continue;
            }
            text[write++] = text[read];
        }
        while (write != 0 && text[write - 1] == ' ')
            write--;
        text[write] = '\0';
        at = write;
    }
    if (out_len != NULL)
        *out_len = at;
    return text;
}

char *p_subtree_text(os64_page_t *page, const os64_html_node_t *n, bool collapse, size_t *out_len)
{
    return p_subtree_text_into(&page->arena, n, collapse, out_len);
}

// ── Who a control belongs to, and whether the page took it away ─────────

// A disabled `fieldset` disables its descendants — EXCEPT those inside its
// first `legend` child, which is where the control that turns the group back
// on lives. The exception is per fieldset, so a control can sit in the
// legend of one and in the body of another.
static bool disabled_by_fieldset(const os64_html_node_t *n)
{
    for (const os64_html_node_t *up = n->parent; up != NULL; up = up->parent) {
        if (!p_is(up, OS64_HTML_TAG_FIELDSET) || !p_has_attr(up, "disabled"))
            continue;
        const os64_html_node_t *legend = NULL;
        for (const os64_html_node_t *c = up->first_child; c != NULL; c = c->next)
            if (p_is(c, OS64_HTML_TAG_LEGEND)) {
                legend = c;
                break;
            }
        bool sheltered = false;
        for (const os64_html_node_t *in = n; in != NULL && in != up; in = in->parent)
            if (in == legend) {
                sheltered = true;
                break;
            }
        if (!sheltered)
            return true;
    }
    return false;
}

static bool in_hidden_subtree(const os64_html_node_t *n)
{
    for (const os64_html_node_t *up = n; up != NULL; up = up->parent)
        if (up->kind == OS64_HTML_ELEMENT && p_has_attr(up, "hidden"))
            return true;
    return false;
}

// ── The pieces of a form the attributes spell ───────────────────────────

os64_page_options_t os64_page_options_default(void)
{
    os64_page_options_t opt = {0};
    // The same number libhtml takes for a page, because a submission is made
    // OF one: a body is not bounded by its document (percent-encoding can
    // treble a value), but a form that would send more than a page's worth is
    // past anything a reader typed and into something gone wrong.
    opt.max_body = 8u * 1024u * 1024u;
    return opt;
}


static os64_page_method_t method_of(const char *text)
{
    // An absent or invalid method is `get`, which is the standard's answer
    // for an enumerated attribute whose value it does not know.
    if (text == NULL)
        return OS64_PAGE_METHOD_GET;
    if (os64_streq_nocase(text, "post"))
        return OS64_PAGE_METHOD_POST;
    if (os64_streq_nocase(text, "dialog"))
        return OS64_PAGE_METHOD_DIALOG;
    return OS64_PAGE_METHOD_GET;
}

static os64_page_enctype_t enctype_of(const char *text)
{
    if (text == NULL)
        return OS64_PAGE_ENCTYPE_URLENCODED;
    if (os64_streq_nocase(text, "multipart/form-data"))
        return OS64_PAGE_ENCTYPE_MULTIPART;
    if (os64_streq_nocase(text, "text/plain"))
        return OS64_PAGE_ENCTYPE_TEXT_PLAIN;
    return OS64_PAGE_ENCTYPE_URLENCODED;
}

static bool element_kind(const os64_html_node_t *n, os64_page_element_t *out)
{
    if (p_is(n, OS64_HTML_TAG_INPUT))
        *out = OS64_PAGE_EL_INPUT;
    else if (p_is(n, OS64_HTML_TAG_BUTTON))
        *out = OS64_PAGE_EL_BUTTON;
    else if (p_is(n, OS64_HTML_TAG_SELECT))
        *out = OS64_PAGE_EL_SELECT;
    else if (p_is(n, OS64_HTML_TAG_TEXTAREA))
        *out = OS64_PAGE_EL_TEXTAREA;
    else
        return false;
    return true;
}

// §4.10.17.3. The `form=` attribute names an element by id ANYWHERE in the
// document, and the standard looks the id up first and asks whether what it
// found is a form second — so `<div id=f></div><form id=f>` leaves the
// control in NO form, because the div is what that id names. A name matching
// nothing leaves it in no form as well, and NOT in the nearest ancestor.
// Without form=, the parser's insertion association precedes ancestry: table
// parsing can leave a form empty while its controls still belong to it.
static int32_t form_owner(const os64_page_t *page, const os64_html_node_t *n)
{
    const char *named = p_attr(n, "form");
    if (named != NULL) {
        const void *node = p_strmap_get(&page->id_map, named);
        return node != NULL ? p_ptrmap_get(&page->form_map, node) : -1;
    }
    const os64_html_node_t *owner = n->form_owner ? n->form_owner : p_ancestor(n, OS64_HTML_TAG_FORM);
    return owner != NULL ? p_ptrmap_get(&page->form_map, owner) : -1;
}

// WHAT PRESSING A CONTROL DOES. A `button` element defaults to type submit,
// which is why it is a submitter through the same code an `input type=submit`
// is: that rule finished for one element and not the other is what crashed
// on a search box.
static void pressing(const os64_html_node_t *n, os64_page_element_t element,
                     os64_page_input_t input, bool *submits, bool *resets)
{
    if (element == OS64_PAGE_EL_BUTTON) {
        const char *type = p_attr(n, "type");
        *resets = type != NULL && os64_streq_nocase(type, "reset");
        *submits = !*resets && (type == NULL || !os64_streq_nocase(type, "button"));
        return;
    }
    *submits = input == OS64_PAGE_INPUT_SUBMIT || input == OS64_PAGE_INPUT_IMAGE;
    *resets = input == OS64_PAGE_INPUT_RESET;
}

// THE TEXT-LIKE INPUT TYPES, named once because two different rules name
// this same set and neither derives from the other: what BLOCKS IMPLICIT
// SUBMISSION (§4.10.21.3) and where `readonly` APPLIES at all (§4.10.5.1)
// are the same list in the standard today. Written down once so a change to
// one does not silently leave the other holding an old list; that they
// coincide is a fact about the list, not about the rules.
static bool text_like_input(os64_page_input_t input)
{
    switch (input) {
    case OS64_PAGE_INPUT_TEXT:
    case OS64_PAGE_INPUT_SEARCH:
    case OS64_PAGE_INPUT_TEL:
    case OS64_PAGE_INPUT_URL:
    case OS64_PAGE_INPUT_EMAIL:
    case OS64_PAGE_INPUT_PASSWORD:
    case OS64_PAGE_INPUT_DATE:
    case OS64_PAGE_INPUT_MONTH:
    case OS64_PAGE_INPUT_WEEK:
    case OS64_PAGE_INPUT_TIME:
    case OS64_PAGE_INPUT_DATETIME_LOCAL:
    case OS64_PAGE_INPUT_NUMBER:
        return true;
    default:
        return false;
    }
}

// The standard's "field that blocks implicit submission": it is about the
// TYPE and nothing else, so `readonly` does not exempt one.
static bool blocks_implicit(os64_page_element_t element, os64_page_input_t input)
{
    return element == OS64_PAGE_EL_INPUT && text_like_input(input);
}

// §4.10.20's bars. `readonly` is the surprising one and the reason it is
// worth writing down: it exempts a control from VALIDATION while leaving it
// SUBMITTED, because readonly is about who may change a value and not about
// whether the value counts. It bars only where it applies — a `readonly`
// spelled on a tick or a list means nothing, so it cannot excuse one from a
// rule either.
static bool barred(const os64_page_control_t *c)
{
    bool readonly_applies =
        c->element == OS64_PAGE_EL_TEXTAREA ||
        (c->element == OS64_PAGE_EL_INPUT && text_like_input(c->input));
    return c->disabled || (c->readonly && readonly_applies) || c->has_datalist_ancestor ||
           c->input == OS64_PAGE_INPUT_HIDDEN || c->submits || c->resets ||
           c->input == OS64_PAGE_INPUT_BUTTON;
}

static void read_overrides(os64_page_t *page, const os64_html_node_t *n,
                           os64_page_overrides_t *out)
{
    p_resolve(page, p_attr(n, "formaction"), &out->action);
    const char *method = p_attr(n, "formmethod");
    out->has_method = method != NULL;
    out->method = method_of(method);
    const char *enctype = p_attr(n, "formenctype");
    out->has_enctype = enctype != NULL;
    out->enctype = enctype_of(enctype);
    out->novalidate = p_has_attr(n, "formnovalidate");
}

// A `select`'s options in tree order, `optgroup` included. A DISABLED option
// is kept — that is what a "choose one" placeholder is — and never sent.
static void read_options(os64_page_t *page, const os64_html_node_t *n, os64_page_option_t **items,
                         int32_t *count, int32_t *cap, bool group_disabled)
{
    for (; n != NULL; n = n->next) {
        if (p_is(n, OS64_HTML_TAG_OPTGROUP)) {
            read_options(page, n->first_child, items, count, cap,
                         group_disabled || p_has_attr(n, "disabled"));
            continue;
        }
        if (!p_is(n, OS64_HTML_TAG_OPTION)) {
            read_options(page, n->first_child, items, count, cap, group_disabled);
            continue;
        }
        if (!p_grow((void **)items, cap, *count, sizeof(**items)))
            return;
        os64_page_option_t *option = &(*items)[*count];
        os64_memset(option, 0, sizeof(*option));
        option->node = n;
        option->label = p_subtree_text(page, n, true, NULL);
        if (option->label == NULL)
            option->label = "";
        const char *label = p_attr(n, "label");
        if (label != NULL && label[0] != '\0')
            option->label = label;
        const char *value = p_attr(n, "value");
        // Without a `value` an option sends its own words, which is how
        // every hand-written menu on the old web is spelled.
        option->value = value != NULL ? value : option->label;
        option->disabled = group_disabled || p_has_attr(n, "disabled");
        option->selected = p_has_attr(n, "selected");
        (*count)++;
    }
}

// ── The walk ────────────────────────────────────────────────────────────

static void note_id(os64_page_t *page, const os64_html_node_t *n)
{
    // EVERY `id` IS SOMEWHERE A FRAGMENT CAN LAND, and it is also what a
    // `form=` looks up, so one table holds the node an id names whatever
    // element that is. Asking whether it is a form comes after.
    const char *id = p_attr(n, "id");
    if (id != NULL && id[0] != '\0' && !p_strmap_put(&page->id_map, id, n))
        page->incomplete = true;
}

static void note_anchor_name(os64_page_t *page, const os64_html_node_t *n)
{
    // The old web's `<a name>`. A fragment matches an id first and one of
    // these second, whatever tree order says, which is why it is a separate
    // table and not a shared one.
    const char *name = p_attr(n, "name");
    if (name != NULL && name[0] != '\0' && !p_strmap_put(&page->aname_map, name, n))
        page->incomplete = true;
}

static void add_link(os64_page_t *page, const os64_html_node_t *n)
{
    if (!p_grow((void **)&page->links, &page->linkcap, page->nlinks, sizeof(*page->links))) {
        page->incomplete = true;
        return;
    }
    os64_page_link_t *link = &page->links[page->nlinks];
    os64_memset(link, 0, sizeof(*link));
    link->node = n;
    p_resolve(page, p_attr(n, "href"), &link->href);
    // It names THIS document, compared without the fragment — which is what
    // both sides of the comparison are, since neither canonical form carries
    // one. Following it is a move and not a fetch.
    link->same_document =
        link->href.url != NULL && os64_streq(link->href.url, page->document_url);
    if (!p_ptrmap_put(&page->link_map, n, page->nlinks))
        page->incomplete = true;
    page->nlinks++;
}

static void add_control(os64_page_t *page, const os64_html_node_t *n, os64_page_element_t element)
{
    if (!p_grow((void **)&page->controls, &page->controlcap, page->ncontrols,
                sizeof(*page->controls))) {
        page->incomplete = true;
        return;
    }
    os64_page_control_t *c = &page->controls[page->ncontrols];
    os64_memset(c, 0, sizeof(*c));
    c->node = n;
    c->element = element;
    c->input = element == OS64_PAGE_EL_INPUT ? p_input_type(n) : OS64_PAGE_INPUT_NONE;
    c->form = form_owner(page, n);
    c->name = p_attr(n, "name");
    c->value = p_page_value(page, n, element, c->input, &c->value_len);
    if (c->value == NULL) {
        page->incomplete = true;
        c->value = "";
        c->value_len = 0;
    }
    c->disabled = p_has_attr(n, "disabled") || disabled_by_fieldset(n);
    c->readonly = p_has_attr(n, "readonly");
    c->required = p_has_attr(n, "required");
    c->multiple = p_has_attr(n, "multiple");
    c->has_value_attribute = p_has_attr(n, "value");
    c->checked = p_has_attr(n, "checked");
    c->hidden_subtree = in_hidden_subtree(n);
    c->has_datalist_ancestor = p_ancestor(n, OS64_HTML_TAG_DATALIST) != NULL;
    c->blocks_implicit = blocks_implicit(element, c->input);
    c->dirname = p_attr(n, "dirname");
    pressing(n, element, c->input, &c->submits, &c->resets);
    c->barred_from_validation = barred(c);
    if (c->submits)
        read_overrides(page, n, &c->overrides);
    if (element == OS64_PAGE_EL_SELECT) {
        os64_page_option_t *items = NULL;
        int32_t count = 0, cap = 0;
        read_options(page, n->first_child, &items, &count, &cap, false);
        c->options = items;
        c->noptions = count;
        // A list HOLDS whichever option is picked, which is only knowable
        // once they have all been read. The first selected one is the value,
        // the same answer a script would get; a `multiple` list sends every
        // one of them, and that is family D's business rather than this
        // field's.
        for (int32_t i = 0; i < count; i++)
            if (items[i].selected) {
                c->value = items[i].value;
                c->value_len = os64_strlen(items[i].value);
                break;
            }
    }
    if (!p_ptrmap_put(&page->control_map, n, page->ncontrols))
        page->incomplete = true;
    page->ncontrols++;
}

static void collect_forms(os64_page_t *page, const os64_html_node_t *n)
{
    for (; n != NULL; n = n->next) {
        if (p_is(n, OS64_HTML_TAG_FORM)) {
            if (!p_grow((void **)&page->forms, &page->formcap, page->nforms,
                        sizeof(*page->forms))) {
                page->incomplete = true;
                return;
            }
            os64_page_form_t *form = &page->forms[page->nforms];
            os64_memset(form, 0, sizeof(*form));
            form->node = n;
            form->id = p_attr(n, "id");
            form->method = method_of(p_attr(n, "method"));
            form->enctype = enctype_of(p_attr(n, "enctype"));
            form->accept_charset = p_attr(n, "accept-charset");
            form->novalidate = p_has_attr(n, "novalidate");
            p_resolve(page, p_attr(n, "action"), &form->action);
            if (!p_ptrmap_put(&page->form_map, n, page->nforms))
                page->incomplete = true;
            page->nforms++;
        }
        collect_forms(page, n->first_child);
    }
}

static void collect_ids(os64_page_t *page, const os64_html_node_t *n)
{
    for (; n != NULL; n = n->next) {
        if (n->kind == OS64_HTML_ELEMENT) {
            note_id(page, n);
            if (p_is(n, OS64_HTML_TAG_A))
                note_anchor_name(page, n);
        }
        collect_ids(page, n->first_child);
    }
}

static void collect_model(os64_page_t *page, const os64_html_node_t *n)
{
    for (; n != NULL; n = n->next) {
        // A foreign element is not an HTML link or control however it is
        // spelled, but HTML nested INSIDE one still is — so the test is per
        // node and the walk never prunes a subtree for its namespace.
        if (n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_HTML) {
            if ((p_is(n, OS64_HTML_TAG_A) || p_is(n, OS64_HTML_TAG_AREA)) &&
                p_has_attr(n, "href"))
                add_link(page, n);
            // A `meta` inside `noscript` counts, and that is the case that
            // matters: with scripting off those contents ARE the document's,
            // which is the whole reason the element exists.
            if (!page->has_refresh && p_is(n, OS64_HTML_TAG_META))
                p_refresh_from(page, n);
            os64_page_element_t element;
            if (element_kind(n, &element))
                add_control(page, n, element);
        }
        // `template` contents are a separate fragment and belong to no form,
        // so the walk never follows them: they hang off their own pointer
        // rather than off first_child, which is what makes that free.
        collect_model(page, n->first_child);
    }
}

// Chain every radio sharing a name, so the rest of a group can be found from
// any one of them — to untick them when one is ticked, and to ask whether a
// required group has anything ticked at all. The owner is compared when the
// chain is walked, because two forms may use one name for two questions.
static bool chain_radios(os64_page_t *page)
{
    if (page->ncontrols == 0)
        return true;
    page->radio_next = os64_malloc((size_t)page->ncontrols * sizeof(*page->radio_next));
    if (page->radio_next == NULL)
        return false;
    for (int32_t i = 0; i < page->ncontrols; i++)
        page->radio_next[i] = -1;
    // Backwards, so the chain a name maps to comes out in tree order.
    for (int32_t i = page->ncontrols - 1; i >= 0; i--) {
        const os64_page_control_t *c = &page->controls[i];
        if (c->input != OS64_PAGE_INPUT_RADIO || c->name == NULL || c->name[0] == '\0')
            continue;
        const void *head = p_strmap_get(&page->radio_map, c->name);
        if (head != NULL)
            page->radio_next[i] = p_ptrmap_get(&page->control_map, head);
        if (!p_strmap_set(&page->radio_map, c->name, c->node))
            return false;
    }
    return true;
}

os64_page_t *os64_page_build(const os64_html_document_t *doc, const char *document_url,
                             const os64_page_options_t *opt)
{
    os64_page_t *page = os64_calloc(1, sizeof(*page));
    if (page == NULL)
        return NULL;
    page->doc = doc;
    page->opt = opt != NULL ? *opt : os64_page_options_default();
    if (page->opt.max_body == 0)
        page->opt.max_body = os64_page_options_default().max_body;

    if (!p_document_url(page, document_url) || !p_base(page)) {
        os64_page_free(page);
        return NULL;
    }
    const os64_html_node_t *root = doc != NULL ? doc->document : NULL;
    if (root == NULL && doc != NULL)
        root = doc->html;
    // The ids come first because a `form=` names one, the forms second
    // because a control's owner is settled as it is met, and the model last.
    collect_ids(page, root);
    collect_forms(page, root);
    collect_model(page, root);
    if (!chain_radios(page))
        page->incomplete = true;
    return page;
}

void os64_page_free(os64_page_t *page)
{
    if (page == NULL)
        return;
    for (int32_t i = 0; i < page->ncontrols; i++)
        os64_free((void *)page->controls[i].options);
    for (int32_t i = 0; i < page->nedits; i++) {
        os64_free(page->edits[i].text);
        os64_free(page->edits[i].chosen);
    }
    os64_free(page->edits);
    os64_free(page->links);
    os64_free(page->forms);
    os64_free(page->controls);
    os64_free(page->radio_next);
    p_ptrmap_free(&page->link_map);
    p_ptrmap_free(&page->form_map);
    p_ptrmap_free(&page->control_map);
    p_ptrmap_free(&page->edit_map);
    p_strmap_free(&page->id_map);
    p_strmap_free(&page->aname_map);
    p_strmap_free(&page->radio_map);
    p_arena_free(&page->arena);
    os64_free(page);
}

// ── Reading the model ───────────────────────────────────────────────────

bool os64_page_incomplete(const os64_page_t *page)
{
    return page != NULL && page->incomplete;
}

const char *os64_page_base(const os64_page_t *page)
{
    return page != NULL ? page->base_url : "";
}

const char *os64_page_document_url(const os64_page_t *page)
{
    return page != NULL ? page->document_url : "";
}

int32_t os64_page_nlinks(const os64_page_t *page)
{
    return page != NULL ? page->nlinks : 0;
}

const os64_page_link_t *os64_page_link(const os64_page_t *page, int32_t i)
{
    if (page == NULL || i < 0 || i >= page->nlinks)
        return NULL;
    return &page->links[i];
}

int32_t os64_page_link_for(const os64_page_t *page, const os64_html_node_t *node)
{
    return page != NULL ? p_ptrmap_get(&page->link_map, node) : -1;
}

int32_t os64_page_nforms(const os64_page_t *page)
{
    return page != NULL ? page->nforms : 0;
}

const os64_page_form_t *os64_page_form(const os64_page_t *page, int32_t i)
{
    if (page == NULL || i < 0 || i >= page->nforms)
        return NULL;
    return &page->forms[i];
}

int32_t os64_page_form_for(const os64_page_t *page, const os64_html_node_t *node)
{
    return page != NULL ? p_ptrmap_get(&page->form_map, node) : -1;
}

int32_t os64_page_ncontrols(const os64_page_t *page)
{
    return page != NULL ? page->ncontrols : 0;
}

const os64_page_control_t *os64_page_control(const os64_page_t *page, int32_t i)
{
    if (page == NULL || i < 0 || i >= page->ncontrols)
        return NULL;
    return &page->controls[i];
}

int32_t os64_page_control_for(const os64_page_t *page, const os64_html_node_t *node)
{
    return page != NULL ? p_ptrmap_get(&page->control_map, node) : -1;
}

const os64_html_node_t *os64_page_anchor(const os64_page_t *page, const char *decoded_fragment)
{
    if (page == NULL || decoded_fragment == NULL)
        return NULL;
    // An id anywhere beats an `<a name>` anywhere: the standard asks the
    // two questions in that order and not in tree order.
    const void *node = p_strmap_get(&page->id_map, decoded_fragment);
    if (node == NULL)
        node = p_strmap_get(&page->aname_map, decoded_fragment);
    return node;
}

// ── A person's edits ────────────────────────────────────────────────────

PEdit *p_edit_find(const os64_page_t *page, int32_t control)
{
    if (page == NULL || control < 0 || control >= page->ncontrols)
        return NULL;
    int32_t at = p_ptrmap_get(&page->edit_map, page->controls[control].node);
    return at >= 0 ? &page->edits[at] : NULL;
}

PEdit *p_edit_for(os64_page_t *page, int32_t control)
{
    PEdit *edit = p_edit_find(page, control);
    if (edit != NULL)
        return edit;
    if (!p_grow((void **)&page->edits, &page->editcap, page->nedits, sizeof(*page->edits)))
        return NULL;
    edit = &page->edits[page->nedits];
    os64_memset(edit, 0, sizeof(*edit));
    edit->node = page->controls[control].node;
    edit->on = -1;
    if (!p_ptrmap_put(&page->edit_map, edit->node, page->nedits))
        return NULL;
    page->nedits++;
    // The table may have moved under a realloc, so hand back the slot by
    // index rather than the pointer taken before the grow.
    return &page->edits[page->nedits - 1];
}

// The options array is the PAGE's storage and the model publishes it as
// const, so the writes below cast that away. It is the same memory either
// way; what the const buys is a face that cannot change a choice behind
// os64_page_set_chosen's back, which is where the radio and one-line-list
// rules live.
void p_publish(os64_page_t *page, int32_t control)
{
    os64_page_control_t *c = &page->controls[control];
    const PEdit *edit = p_edit_find(page, control);
    if (edit == NULL)
        return;
    if (edit->text != NULL) {
        c->value = edit->text;
        c->value_len = edit->text_len;
    }
    if (edit->on >= 0)
        c->checked = edit->on != 0;
    for (int32_t i = 0; i < c->noptions && i < edit->nchosen; i++)
        if (edit->chosen != NULL && edit->chosen[i] != 2)
            ((os64_page_option_t *)c->options)[i].selected = edit->chosen[i] != 0;
}

int64_t os64_page_set_text(os64_page_t *page, int32_t control, const char *utf8, size_t len)
{
    const os64_page_control_t *c = os64_page_control(page, control);
    if (c == NULL)
        return -OS64_PAGE_REASON_NO_CONTROL;
    if (c->element == OS64_PAGE_EL_SELECT)
        return -OS64_PAGE_REASON_WRONG_KIND;
    PEdit *edit = p_edit_for(page, control);
    if (edit == NULL)
        return -OS64_PAGE_REASON_NO_MEMORY;
    char *text = os64_malloc(len + 1);
    if (text == NULL)
        return -OS64_PAGE_REASON_NO_MEMORY;
    if (len != 0)
        os64_memcpy(text, utf8, len);
    text[len] = '\0';
    os64_free(edit->text);
    edit->text = text;
    edit->text_len = len;
    p_publish(page, control);
    return 0;
}

int64_t os64_page_set_checked(os64_page_t *page, int32_t control, bool on)
{
    const os64_page_control_t *c = os64_page_control(page, control);
    if (c == NULL)
        return -OS64_PAGE_REASON_NO_CONTROL;
    if (c->input != OS64_PAGE_INPUT_CHECKBOX && c->input != OS64_PAGE_INPUT_RADIO)
        return -OS64_PAGE_REASON_WRONG_KIND;
    PEdit *edit = p_edit_for(page, control);
    if (edit == NULL)
        return -OS64_PAGE_REASON_NO_MEMORY;
    edit->on = on ? 1 : 0;
    p_publish(page, control);
    // TICKING ONE RADIO UNTICKS ITS GROUP, which is one name under one
    // owner. Nothing else in the group is touched when a tick is cleared:
    // the standard has no rule that puts a tick back.
    if (on && c->input == OS64_PAGE_INPUT_RADIO && c->name != NULL && c->name[0] != '\0') {
        const void *head = p_strmap_get(&page->radio_map, c->name);
        for (int32_t at = head != NULL ? p_ptrmap_get(&page->control_map, head) : -1; at >= 0;
             at = page->radio_next[at]) {
            if (at == control || page->controls[at].form != c->form)
                continue;
            PEdit *other = p_edit_for(page, at);
            if (other == NULL)
                return -OS64_PAGE_REASON_NO_MEMORY;
            other->on = 0;
            p_publish(page, at);
        }
    }
    return 0;
}

int64_t os64_page_set_chosen(os64_page_t *page, int32_t control, int32_t option, bool on)
{
    const os64_page_control_t *c = os64_page_control(page, control);
    if (c == NULL)
        return -OS64_PAGE_REASON_NO_CONTROL;
    if (c->element != OS64_PAGE_EL_SELECT || option < 0 || option >= c->noptions)
        return -OS64_PAGE_REASON_WRONG_KIND;
    PEdit *edit = p_edit_for(page, control);
    if (edit == NULL)
        return -OS64_PAGE_REASON_NO_MEMORY;
    if (edit->chosen == NULL) {
        edit->chosen = os64_malloc((size_t)c->noptions);
        if (edit->chosen == NULL)
            return -OS64_PAGE_REASON_NO_MEMORY;
        os64_memset(edit->chosen, 2, (size_t)c->noptions);
        edit->nchosen = c->noptions;
    }
    // A list that is not `multiple` holds ONE choice, so picking one puts
    // every other one down — the same shape as a radio group, and for the
    // same reason: the page asked one question.
    if (on && !c->multiple)
        for (int32_t i = 0; i < edit->nchosen; i++)
            edit->chosen[i] = 0;
    edit->chosen[option] = on ? 1 : 0;
    p_publish(page, control);
    return 0;
}

int64_t os64_page_reset(os64_page_t *page, int32_t form)
{
    if (page == NULL)
        return -OS64_PAGE_REASON_NO_CONTROL;
    // FORGETTING AN EDIT IS THE WHOLE OF A RESET, because the model's values
    // are the page's own until somebody changes them. What is dropped here
    // is the change, and what is left is what the page wrote.
    for (int32_t i = 0; i < page->ncontrols; i++) {
        if (page->controls[i].form != form)
            continue;
        PEdit *edit = p_edit_find(page, i);
        if (edit == NULL)
            continue;
        os64_free(edit->text);
        os64_free(edit->chosen);
        edit->text = NULL;
        edit->text_len = 0;
        edit->chosen = NULL;
        edit->nchosen = 0;
        edit->on = -1;
        os64_page_control_t *c = &page->controls[i];
        c->value = p_page_value(page, c->node, c->element, c->input, &c->value_len);
        if (c->value == NULL) {
            page->incomplete = true;
            c->value = "";
            c->value_len = 0;
        }
        c->checked = p_has_attr(c->node, "checked");
        for (int32_t o = 0; o < c->noptions; o++)
            ((os64_page_option_t *)c->options)[o].selected =
                p_has_attr(c->options[o].node, "selected");
        if (c->element == OS64_PAGE_EL_SELECT)
            for (int32_t o = 0; o < c->noptions; o++)
                if (c->options[o].selected) {
                    c->value = c->options[o].value;
                    c->value_len = os64_strlen(c->options[o].value);
                    break;
                }
    }
    return 0;
}
