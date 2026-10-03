// state.c — live control properties keyed by node, shared by model snapshots.
// Reservations finish before dirty state changes; views publish before any
// replaced state bytes are freed. HTML pins govern tree storage separately.
#include "internal.h"
#include "os64/proc.h"

// The allocation header keeps frees exact, including scratch arena blocks.
// Alignment preserves the allocator's alignment for every returned object.
typedef union {
    size_t size;
    max_align_t alignment;
} PStateBlock;

void *p_state_alloc(os64_page_state_t *state, size_t size)
{
    if (state == NULL)
        return os64_malloc(size);
    if (size > SIZE_MAX - sizeof(PStateBlock))
        return NULL;
    size_t total = size + sizeof(PStateBlock);
    if (total > state->max_bytes - state->bytes)
        return NULL;
    PStateBlock *block = os64_malloc(total);
    if (block == NULL)
        return NULL;
    block->size = total;
    state->bytes += total;
    return block + 1;
}

void p_state_dealloc(os64_page_state_t *state, void *ptr)
{
    if (ptr == NULL)
        return;
    if (state == NULL) {
        os64_free(ptr);
        return;
    }
    PStateBlock *block = (PStateBlock *)ptr - 1;
    state->bytes -= block->size;
    os64_free(block);
}

os64_page_state_t *os64_page_state_create(os64_html_document_t *doc, size_t max_bytes)
{
    if (doc == NULL)
        return NULL;
    if (max_bytes == 0)
        max_bytes = OS64_PAGE_STATE_DEFAULT_MAX_BYTES;
    if (max_bytes < sizeof(os64_page_state_t))
        return NULL;
    os64_page_state_t *state = os64_calloc(1, sizeof(*state));
    if (state == NULL)
        return NULL;
    state->doc = doc;
    state->max_bytes = max_bytes;
    state->bytes = sizeof(*state);
    state->version = 1;
    return state;
}

void os64_page_state_free(os64_page_state_t *state)
{
    if (state == NULL)
        return;
    if (state->models != NULL)
        os64_exit((int32_t)OS64_PAGE_FATAL_EXIT);
    PNodeState *edit = state->records;
    while (edit != NULL) {
        PNodeState *next = edit->next;
        p_state_dealloc(state, edit->text);
        p_state_dealloc(state, edit->cache);
        p_state_dealloc(state, edit);
        edit = next;
    }
    p_state_dealloc(state, state->table);
    os64_free(state);
}

size_t os64_page_state_bytes(const os64_page_state_t *state)
{
    return state != NULL ? state->bytes : 0;
}

uint64_t os64_page_state_version(const os64_page_state_t *state)
{
    return state != NULL ? state->version : 0;
}

void p_state_changed(os64_page_state_t *state)
{
    if (++state->version == 0)
        os64_exit((int32_t)OS64_PAGE_FATAL_EXIT);
}

static size_t node_hash(const os64_html_node_t *node)
{
    uintptr_t n = (uintptr_t)node >> 4;
    n ^= n >> 23;
    n *= (uintptr_t)0x9e3779b97f4a7c15ULL;
    return (size_t)(n ^ (n >> 32));
}

static PNodeState **record_slot(PNodeState **table, size_t cap, const os64_html_node_t *node)
{
    size_t at = node_hash(node) & (cap - 1);
    while (table[at] != NULL && table[at]->node != node)
        at = (at + 1) & (cap - 1);
    return &table[at];
}

PNodeState *p_state_find(const os64_page_state_t *state, const os64_html_node_t *node)
{
    if (state == NULL || state->cap == 0)
        return NULL;
    return *record_slot(state->table, state->cap, node);
}

PNodeState *p_reserve_find(const PReserve *reserve, const os64_html_node_t *node)
{
    if (reserve->table != NULL)
        return *record_slot(reserve->table, reserve->cap, node);
    PNodeState *edit = p_state_find(reserve->state, node);
    if (edit != NULL)
        return edit;
    if (reserve->pending != NULL)
        return *record_slot(reserve->pending, reserve->pending_cap, node);
    for (edit = reserve->records; edit != NULL; edit = edit->next)
        if (edit->node == node)
            return edit;
    return NULL;
}

bool p_reserve_node(PReserve *reserve, const os64_html_node_t *node)
{
    os64_page_state_t *state = reserve->state;
    if (p_reserve_find(reserve, node) != NULL)
        return true;
    size_t cap = reserve->table != NULL ? reserve->cap : state->cap;
    size_t count = state->count + reserve->count;
    if (cap == 0)
        cap = 16;
    while (count + 1 > cap / 2) {
        if (cap > SIZE_MAX / 2)
            return false;
        cap *= 2;
    }
    size_t previous_cap = reserve->table != NULL ? reserve->cap : state->cap;
    if (cap != previous_cap) {
        if (cap > SIZE_MAX / sizeof(*reserve->table))
            return false;
        PNodeState **table = p_state_alloc(state, cap * sizeof(*table));
        if (table == NULL)
            return false;
        os64_memset(table, 0, cap * sizeof(*table));
        PNodeState **source = reserve->table != NULL ? reserve->table : state->table;
        size_t source_cap = reserve->table != NULL ? reserve->cap : state->cap;
        for (size_t i = 0; i < source_cap; i++)
            if (source[i] != NULL)
                *record_slot(table, cap, source[i]->node) = source[i];
        if (reserve->table == NULL)
            for (PNodeState *edit = reserve->records; edit != NULL; edit = edit->next)
                *record_slot(table, cap, edit->node) = edit;
        p_state_dealloc(state, reserve->table);
        reserve->table = table;
        reserve->cap = cap;
        p_state_dealloc(state, reserve->pending);
        reserve->pending = NULL;
        reserve->pending_cap = 0;
    } else if (reserve->table == NULL && reserve->count >= 8 &&
               (reserve->pending == NULL || reserve->count + 1 > reserve->pending_cap / 2)) {
        // A bounded small-list lookup makes one-node reservations cheap.
        // Larger batches index just their new records, not the live table.
        size_t pending_cap = reserve->pending_cap != 0 ? reserve->pending_cap * 2 : 32;
        if (pending_cap < reserve->pending_cap || pending_cap > SIZE_MAX / sizeof(*reserve->pending))
            return false;
        PNodeState **pending = p_state_alloc(state, pending_cap * sizeof(*pending));
        if (pending == NULL)
            return false;
        os64_memset(pending, 0, pending_cap * sizeof(*pending));
        for (PNodeState *edit = reserve->records; edit != NULL; edit = edit->next)
            *record_slot(pending, pending_cap, edit->node) = edit;
        p_state_dealloc(state, reserve->pending);
        reserve->pending = pending;
        reserve->pending_cap = pending_cap;
    }
    PNodeState *edit = p_state_alloc(state, sizeof(*edit));
    if (edit == NULL)
        return false;
    os64_memset(edit, 0, sizeof(*edit));
    edit->node = node;
    edit->on = edit->selected = -1;
    edit->next = reserve->records;
    reserve->records = edit;
    if (reserve->table != NULL)
        *record_slot(reserve->table, reserve->cap, node) = edit;
    else if (reserve->pending != NULL)
        *record_slot(reserve->pending, reserve->pending_cap, node) = edit;
    reserve->count++;
    return true;
}

struct PValueStage {
    PValueStage *next;
    PNodeState *edit;
    char *replacement, *previous;
    size_t len;
    uint64_t version;
    bool changed, clear;
};

static void values_discard(PReserve *reserve, bool applied)
{
    while (reserve->values != NULL) {
        PValueStage *stage = reserve->values;
        reserve->values = stage->next;
        p_state_dealloc(reserve->state, applied ? stage->previous : stage->replacement);
        p_state_dealloc(reserve->state, stage);
    }
}

static bool values_apply(PReserve *reserve)
{
    bool changed = false;
    for (PValueStage *stage = reserve->values; stage != NULL; stage = stage->next) {
        PNodeState *edit = stage->edit;
        if (stage->changed || stage->clear) {
            stage->previous = edit->text;
            edit->text = stage->replacement;
            edit->text_len = stage->len;
            stage->replacement = NULL;
        }
        edit->text_version = stage->version;
        changed |= stage->changed;
    }
    return changed;
}

void p_reserve_abort(PReserve *reserve)
{
    values_discard(reserve, false);
    while (reserve->records != NULL) {
        PNodeState *next = reserve->records->next;
        p_state_dealloc(reserve->state, reserve->records);
        reserve->records = next;
    }
    p_state_dealloc(reserve->state, reserve->table);
    p_state_dealloc(reserve->state, reserve->pending);
    reserve->table = NULL;
    reserve->pending = NULL;
    reserve->cap = reserve->count = reserve->pending_cap = 0;
}

void p_reserve_commit(PReserve *reserve)
{
    os64_page_state_t *state = reserve->state;
    bool replacement = reserve->table != NULL;
    if (reserve->table != NULL) {
        p_state_dealloc(state, state->table);
        state->table = reserve->table;
        state->cap = reserve->cap;
        reserve->table = NULL;
    }
    state->count += reserve->count;
    while (reserve->records != NULL) {
        PNodeState *next = reserve->records->next;
        if (!replacement)
            *record_slot(state->table, state->cap, reserve->records->node) = reserve->records;
        reserve->records->next = state->records;
        state->records = reserve->records;
        reserve->records = next;
    }
    p_state_dealloc(state, reserve->pending);
    reserve->pending = NULL;
    reserve->cap = reserve->count = reserve->pending_cap = 0;
}

void p_state_publish(os64_page_state_t *state)
{
    for (os64_page_t *page = state->models; page != NULL; page = page->state_next)
        if (page->initial_ready)
            for (int32_t i = 0; i < page->ncontrols; i++)
                p_publish(page, i);
}

static bool node_kind(os64_page_state_t *state, const os64_html_node_t *node,
                      os64_page_element_t *element, os64_page_input_t *input)
{
    if (state == NULL || !os64_html_owns_node(state->doc, node))
        return false;
    if (p_is(node, OS64_HTML_TAG_INPUT))
        *element = OS64_PAGE_EL_INPUT;
    else if (p_is(node, OS64_HTML_TAG_TEXTAREA))
        *element = OS64_PAGE_EL_TEXTAREA;
    else if (p_is(node, OS64_HTML_TAG_BUTTON))
        *element = OS64_PAGE_EL_BUTTON;
    else if (p_is(node, OS64_HTML_TAG_SELECT))
        *element = OS64_PAGE_EL_SELECT;
    else
        return false;
    *input = *element == OS64_PAGE_EL_INPUT ? p_input_type(node) : OS64_PAGE_INPUT_NONE;
    return true;
}

bool p_value_is_attribute(os64_page_element_t element, os64_page_input_t input)
{
    return element == OS64_PAGE_EL_BUTTON || (element == OS64_PAGE_EL_INPUT &&
        (input == OS64_PAGE_INPUT_HIDDEN || input == OS64_PAGE_INPUT_CHECKBOX ||
         input == OS64_PAGE_INPUT_RADIO || input == OS64_PAGE_INPUT_SUBMIT ||
         input == OS64_PAGE_INPUT_RESET || input == OS64_PAGE_INPUT_BUTTON ||
         input == OS64_PAGE_INPUT_IMAGE));
}

static bool same_bytes(const char *a, size_t na, const char *b, size_t nb)
{
    return na == nb && (na == 0 || os64_memcmp(a, b, na) == 0);
}

static const os64_html_node_t *tree_root(const os64_html_node_t *node)
{
    while (node->parent != NULL)
        node = node->parent;
    return node;
}

// Stay within root. Template contents are a separate tree, reached only
// when they themselves supplied the root, as form ownership requires.
static const os64_html_node_t *tree_next(const os64_html_node_t *root,
                                        const os64_html_node_t *node)
{
    if (node->first_child != NULL)
        return node->first_child;
    while (node != root && node->next == NULL)
        node = node->parent;
    return node != root ? node->next : NULL;
}

static const os64_html_node_t *node_owner(const os64_html_node_t *node)
{
    const char *form = p_attr(node, "form");
    if (form != NULL) {
        const os64_html_node_t *root = tree_root(node);
        // Explicit association is an ID lookup in the document tree.
        if (root->kind == OS64_HTML_DOCUMENT)
            for (const os64_html_node_t *at = root; at != NULL; at = tree_next(root, at)) {
                const char *id = p_attr(at, "id");
                if (id != NULL && os64_streq(id, form))
                    return p_is(at, OS64_HTML_TAG_FORM) ? at : NULL;
            }
        return NULL;
    }
    return node->form_owner != NULL ? node->form_owner : p_ancestor(node, OS64_HTML_TAG_FORM);
}

static bool radio_group_member(const os64_html_node_t *target, const os64_html_node_t *node)
{
    if (!p_is(node, OS64_HTML_TAG_INPUT) || p_input_type(node) != OS64_PAGE_INPUT_RADIO)
        return false;
    const char *name = p_attr(target, "name"), *other = p_attr(node, "name");
    return name != NULL && *name != '\0' && other != NULL && os64_streq(name, other) &&
        node_owner(target) == node_owner(node);
}

int64_t os64_page_node_checked(os64_page_state_t *state, const os64_html_node_t *node,
                               bool *on)
{
    os64_page_element_t element;
    os64_page_input_t input;
    if (on == NULL || !node_kind(state, node, &element, &input) ||
        element != OS64_PAGE_EL_INPUT ||
        (input != OS64_PAGE_INPUT_CHECKBOX && input != OS64_PAGE_INPUT_RADIO))
        return -OS64_PAGE_REASON_WRONG_KIND;
    const PNodeState *edit = p_state_find(state, node);
    bool attribute = p_has_attr(node, "checked");
    *on = edit != NULL && edit->on >= 0 && (edit->on_dirty || edit->on_attr == attribute) ?
        edit->on != 0 : attribute;
    return 0;
}

int64_t os64_page_node_set_checked(os64_page_state_t *state, const os64_html_node_t *node,
                                   bool on)
{
    bool before;
    if (os64_page_node_checked(state, node, &before) < 0)
        return -OS64_PAGE_REASON_WRONG_KIND;
    bool group = on && p_input_type(node) == OS64_PAGE_INPUT_RADIO;
    const os64_html_node_t *root = tree_root(node);
    PReserve reserve = {.state = state};
    if (!p_reserve_node(&reserve, node))
        goto no_memory;
    if (group)
        for (const os64_html_node_t *at = root; at != NULL; at = tree_next(root, at))
            if (radio_group_member(node, at) && !p_reserve_node(&reserve, at))
                goto no_memory;
    p_reserve_commit(&reserve);
    bool changed = before != on;
    if (group)
        for (const os64_html_node_t *at = root; at != NULL; at = tree_next(root, at))
            if (at != node && radio_group_member(node, at)) {
                bool checked = false;
                (void)os64_page_node_checked(state, at, &checked);
                changed |= checked;
                p_state_find(state, at)->on = 0;
                p_state_find(state, at)->on_dirty = true;
            }
    p_state_find(state, node)->on = on ? 1 : 0;
    p_state_find(state, node)->on_dirty = true;
    p_state_publish(state);
    if (changed)
        p_state_changed(state);
    return 0;
no_memory:
    p_reserve_abort(&reserve);
    return -OS64_PAGE_REASON_NO_MEMORY;
}

static bool option_selected(os64_page_state_t *state, const os64_html_node_t *option)
{
    const PNodeState *edit = p_state_find(state, option);
    bool attribute = p_has_attr(option, "selected");
    return edit != NULL && edit->selected >= 0 && (edit->selected_dirty || edit->selected_attr == attribute) ?
        edit->selected != 0 : attribute;
}

int64_t os64_page_node_selected_index(os64_page_state_t *state,
                                      const os64_html_node_t *node, int32_t *index)
{
    os64_page_element_t element;
    os64_page_input_t input;
    if (index == NULL || !node_kind(state, node, &element, &input) || element != OS64_PAGE_EL_SELECT)
        return -OS64_PAGE_REASON_WRONG_KIND;
    bool multiple = p_has_attr(node, "multiple");
    int32_t chosen = -1, first = -1, i = 0;
    for (const os64_html_node_t *at = p_option_next(node, NULL); at != NULL; at = p_option_next(node, at), i++) {
        if (first < 0 && !p_option_disabled(node, at))
            first = i;
        if (option_selected(state, at) && (!multiple || chosen < 0))
            chosen = i;
    }
    const PNodeState *edit = p_state_find(state, node);
    os64_page_control_t c = {.node = node, .multiple = multiple};
    if (chosen < 0 && (edit == NULL || !edit->selection_set) && p_select_one_line(&c))
        chosen = first;
    *index = chosen;
    return 0;
}

static int64_t select_assign(os64_page_state_t *state, const os64_html_node_t *node, int32_t index)
{
    PReserve reserve = {.state = state};
    if (!p_reserve_node(&reserve, node))
        goto no_memory;
    for (const os64_html_node_t *at = p_option_next(node, NULL); at != NULL; at = p_option_next(node, at))
        if (!p_reserve_node(&reserve, at))
            goto no_memory;
    int32_t before = -1;
    (void)os64_page_node_selected_index(state, node, &before);
    bool changed = false;
    int32_t i = 0;
    for (const os64_html_node_t *at = p_option_next(node, NULL); at != NULL; at = p_option_next(node, at), i++)
        changed |= option_selected(state, at) != (i == index);
    p_reserve_commit(&reserve);
    p_state_find(state, node)->selection_set = true;
    i = 0;
    for (const os64_html_node_t *at = p_option_next(node, NULL); at != NULL; at = p_option_next(node, at), i++)
    {
        p_state_find(state, at)->selected = i == index ? 1 : 0;
        p_state_find(state, at)->selected_dirty = true;
    }
    p_state_publish(state);
    int32_t after = -1;
    (void)os64_page_node_selected_index(state, node, &after);
    if (changed || before != after)
        p_state_changed(state);
    return 0;
no_memory:
    p_reserve_abort(&reserve);
    return -OS64_PAGE_REASON_NO_MEMORY;
}

int64_t os64_page_node_set_selected_index(os64_page_state_t *state,
                                          const os64_html_node_t *node, int32_t index)
{
    os64_page_element_t element;
    os64_page_input_t input;
    if (!node_kind(state, node, &element, &input) || element != OS64_PAGE_EL_SELECT)
        return -OS64_PAGE_REASON_WRONG_KIND;
    return select_assign(state, node, index);
}

static char *state_copy(os64_page_state_t *state, const char *s, size_t len)
{
    if (len == SIZE_MAX)
        return NULL;
    char *text = p_state_alloc(state, len + 1);
    if (text != NULL) {
        if (len != 0)
            os64_memcpy(text, s, len);
        text[len] = '\0';
    }
    return text;
}

// Version-only invalidation can normalize the current value/mode, but
// cannot reconstruct type transitions a caller made between observations.
// The state-aware mutation entrance is tracked in DEBTS.md under libpage.
static bool normalize_dirty(PReserve *reserve, const os64_html_node_t *node,
                             os64_page_element_t element, os64_page_input_t input)
{
    os64_page_state_t *state = reserve->state;
    PNodeState *edit = p_state_find(state, node);
    uint64_t version = os64_html_version(state->doc);
    if (edit == NULL || edit->text == NULL || edit->text_version == version)
        return true;
    PArena temporary = {.budget = state};
    bool clear = p_value_is_attribute(element, input);
    size_t len = 0;
    const char *value = clear ? "" : p_sanitize_value(&temporary, node, element, input,
        edit->text, edit->text_len, &len);
    if (value == NULL) {
        p_arena_free(&temporary);
        return false;
    }
    bool changed = !same_bytes(edit->text, edit->text_len, value, len);
    PValueStage *stage = p_state_alloc(state, sizeof(*stage));
    char *replacement = changed && !clear ? state_copy(state, value, len) : NULL;
    if (stage == NULL || (changed && !clear && replacement == NULL)) {
        p_state_dealloc(state, stage);
        p_state_dealloc(state, replacement);
        p_arena_free(&temporary);
        return false;
    }
    os64_memset(stage, 0, sizeof(*stage));
    stage->edit = edit;
    stage->replacement = replacement;
    stage->len = len;
    stage->version = version;
    stage->changed = changed;
    stage->clear = clear;
    stage->next = reserve->values;
    reserve->values = stage;
    p_arena_free(&temporary);
    return true;
}

int64_t os64_page_node_value(os64_page_state_t *state, const os64_html_node_t *node,
                             const char **value, size_t *len)
{
    os64_page_element_t element;
    os64_page_input_t input;
    if (value == NULL || len == NULL || !node_kind(state, node, &element, &input))
        return -OS64_PAGE_REASON_WRONG_KIND;
    PNodeState *edit = p_state_find(state, node);
    bool attribute = p_value_is_attribute(element, input);
    uint64_t version = os64_html_version(state->doc);
    PReserve reserve = {.state = state};
    if (!normalize_dirty(&reserve, node, element, input)) {
        p_reserve_abort(&reserve);
        return -OS64_PAGE_REASON_NO_MEMORY;
    }
    if (!attribute && element != OS64_PAGE_EL_SELECT && edit != NULL && edit->text != NULL) {
        bool staged = reserve.values != NULL;
        bool changed = values_apply(&reserve);
        if (staged)
            p_state_publish(state);
        values_discard(&reserve, true);
        if (changed)
            p_state_changed(state);
        *value = edit->text;
        *len = edit->text_len;
        return 0;
    }
    if (edit != NULL && edit->cache != NULL && edit->cache_version == version &&
        (element != OS64_PAGE_EL_SELECT || edit->cache_state_version == state->version)) {
        bool staged = reserve.values != NULL;
        bool changed = values_apply(&reserve);
        if (staged)
            p_state_publish(state);
        values_discard(&reserve, true);
        if (changed)
            p_state_changed(state);
        *value = edit->cache;
        *len = edit->cache_len;
        return 0;
    }
    PArena temporary = {.budget = state};
    size_t raw_len = 0;
    const char *raw = NULL;
    if (element == OS64_PAGE_EL_TEXTAREA) {
        raw = p_subtree_text_into(&temporary, node, false, &raw_len);
        if (raw == NULL)
            goto no_memory;
    } else if (element == OS64_PAGE_EL_SELECT) {
        int32_t chosen = -1, i = 0;
        (void)os64_page_node_selected_index(state, node, &chosen);
        for (const os64_html_node_t *at = p_option_next(node, NULL); at != NULL; at = p_option_next(node, at), i++)
            if (i == chosen) {
                raw = p_attr(at, "value");
                if (raw == NULL) {
                    raw = p_subtree_text_into(&temporary, at, true, &raw_len);
                    if (raw == NULL)
                        goto no_memory;
                } else {
                    raw_len = os64_strlen(raw);
                }
                break;
            }
    } else {
        raw = p_attr(node, "value");
        if (raw == NULL && (input == OS64_PAGE_INPUT_CHECKBOX || input == OS64_PAGE_INPUT_RADIO))
            raw = "on";
        raw_len = raw != NULL ? os64_strlen(raw) : 0;
    }
    if (raw == NULL)
        raw = "";
    size_t value_len = raw_len;
    const char *sanitized = element == OS64_PAGE_EL_SELECT ? raw :
        p_sanitize_value(&temporary, node, element, input, raw, raw_len, &value_len);
    if (sanitized == NULL)
        goto no_memory;
    // A select's value can change without its document version changing.
    // Keep an equal cache in place so repeated reads need no retained bytes.
    if (edit != NULL && edit->cache != NULL && same_bytes(edit->cache, edit->cache_len, sanitized, value_len)) {
        edit->cache_version = version;
        bool staged = reserve.values != NULL;
        bool changed = values_apply(&reserve);
        if (staged)
            p_state_publish(state);
        values_discard(&reserve, true);
        if (changed)
            p_state_changed(state);
        edit->cache_version = version;
        edit->cache_state_version = state->version;
        *value = edit->cache;
        *len = edit->cache_len;
        p_arena_free(&temporary);
        return 0;
    }
    char *cache = state_copy(state, sanitized, value_len);
    if (cache == NULL)
        goto no_memory;
    if (!p_reserve_node(&reserve, node)) {
        p_reserve_abort(&reserve);
        p_state_dealloc(state, cache);
        goto no_memory;
    }
    p_reserve_commit(&reserve);
    bool changed = values_apply(&reserve);
    edit = p_state_find(state, node);
    char *previous = edit->cache;
    edit->cache = cache;
    edit->cache_len = value_len;
    edit->cache_version = version;
    edit->cache_state_version = state->version;
    p_state_publish(state);
    p_state_dealloc(state, previous);
    values_discard(&reserve, true);
    if (changed)
        p_state_changed(state);
    *value = cache;
    *len = value_len;
    p_arena_free(&temporary);
    return 0;
no_memory:
    p_reserve_abort(&reserve);
    p_arena_free(&temporary);
    return -OS64_PAGE_REASON_NO_MEMORY;
}

static int64_t html_reason(int64_t status)
{
    return status == OS64_HTML_NO_MEMORY || status == OS64_HTML_ARENA_EXHAUSTED ||
        status == OS64_HTML_TOO_LARGE ? -OS64_PAGE_REASON_NO_MEMORY : -OS64_PAGE_REASON_WRONG_KIND;
}

int64_t os64_page_node_set_value(os64_page_state_t *state, const os64_html_node_t *node,
                                 const char *utf8, size_t len)
{
    return p_state_set_value(state, node, utf8, len, false);
}

int64_t p_state_set_value(os64_page_state_t *state, const os64_html_node_t *node,
                          const char *utf8, size_t len, bool user)
{
    os64_page_element_t element;
    os64_page_input_t input;
    if (len == SIZE_MAX || (utf8 == NULL && len != 0) || !node_kind(state, node, &element, &input))
        return -OS64_PAGE_REASON_WRONG_KIND;
    if (input == OS64_PAGE_INPUT_FILE && len != 0)
        return -OS64_PAGE_REASON_WRONG_KIND;
    if (element == OS64_PAGE_EL_SELECT) {
        PArena temporary = {.budget = state};
        int32_t chosen = -1, i = 0;
        for (const os64_html_node_t *at = p_option_next(node, NULL); at != NULL; at = p_option_next(node, at), i++) {
            const char *text = p_attr(at, "value");
            size_t text_len = text != NULL ? os64_strlen(text) : 0;
            if (text == NULL)
                text = p_subtree_text_into(&temporary, at, true, &text_len);
            if (text == NULL) {
                p_arena_free(&temporary);
                return -OS64_PAGE_REASON_NO_MEMORY;
            }
            if (same_bytes(text, text_len, utf8, len)) {
                chosen = i;
                break;
            }
        }
        p_arena_free(&temporary);
        return select_assign(state, node, chosen);
    }
    PArena temporary = {.budget = state};
    char *raw = p_arena_copy(&temporary, utf8 != NULL ? utf8 : "", len);
    size_t value_len = 0;
    const char *value = raw != NULL ? p_sanitize_value(&temporary, node, element, input, raw, len, &value_len) : NULL;
    char *text = value != NULL ? state_copy(state, value, value_len) : NULL;
    if (text == NULL) {
        p_arena_free(&temporary);
        return -OS64_PAGE_REASON_NO_MEMORY;
    }
    PReserve reserve = {.state = state};
    if (!p_reserve_node(&reserve, node)) {
        p_reserve_abort(&reserve);
        p_state_dealloc(state, text);
        p_arena_free(&temporary);
        return -OS64_PAGE_REASON_NO_MEMORY;
    }
    PNodeState *edit = p_state_find(state, node);
    bool attribute = p_value_is_attribute(element, input);
    bool changed;
    if (!attribute && edit != NULL && edit->text != NULL) {
        changed = !same_bytes(edit->text, edit->text_len, text, value_len);
        if (!changed) {
            bool origin_changed = edit->text_user != user;
            edit->text_user = user;
            edit->text_version = os64_html_version(state->doc);
            if (origin_changed)
                p_state_changed(state);
            p_reserve_abort(&reserve);
            p_state_dealloc(state, text);
            p_arena_free(&temporary);
            return 0;
        }
    } else {
        const char *before = p_attr(node, "value");
        size_t before_len = before != NULL ? os64_strlen(before) : 0;
        if (element == OS64_PAGE_EL_TEXTAREA)
            before = p_subtree_text_into(&temporary, node, false, &before_len);
        else if (before == NULL && (input == OS64_PAGE_INPUT_CHECKBOX || input == OS64_PAGE_INPUT_RADIO)) {
            before = "on";
            before_len = 2;
        }
        if (before == NULL && element != OS64_PAGE_EL_TEXTAREA)
            before = "";
        size_t normalized_len = 0;
        const char *normalized = before != NULL ? p_sanitize_value(&temporary, node, element, input,
            before, before_len, &normalized_len) : NULL;
        if (normalized == NULL) {
            p_reserve_abort(&reserve);
            p_state_dealloc(state, text);
            p_arena_free(&temporary);
            return -OS64_PAGE_REASON_NO_MEMORY;
        }
        changed = !same_bytes(normalized, normalized_len, text, value_len);
    }
    if (attribute) {
        int64_t status = os64_html_set_attr(state->doc, (os64_html_node_t *)node, "value", text, value_len);
        if (status != OS64_HTML_OK) {
            p_reserve_abort(&reserve);
            p_state_dealloc(state, text);
            p_arena_free(&temporary);
            return html_reason(status);
        }
    }
    p_reserve_commit(&reserve);
    edit = p_state_find(state, node);
    if (!attribute)
        changed |= edit->text_user != user;
    edit->text_user = user;
    char *previous = attribute ? edit->cache : edit->text;
    if (!attribute && edit->text == NULL && edit->cache != NULL &&
        edit->cache_version == os64_html_version(state->doc) &&
        same_bytes(edit->cache, edit->cache_len, text, value_len)) {
        p_state_dealloc(state, text);
        text = edit->cache;
        edit->cache = NULL;
        edit->cache_len = 0;
    }
    char *old_dirty = attribute ? edit->text : NULL;
    if (attribute)
        edit->text = NULL;
    if (attribute && previous != NULL && same_bytes(previous, edit->cache_len, text, value_len)) {
        p_state_dealloc(state, text);
        text = previous;
        previous = NULL;
    }
    if (attribute) {
        edit->cache = text;
        edit->cache_len = value_len;
        edit->cache_version = os64_html_version(state->doc);
    } else {
        edit->text = text;
        edit->text_len = value_len;
        edit->text_version = os64_html_version(state->doc);
    }
    p_state_publish(state);
    p_state_dealloc(state, previous);
    p_state_dealloc(state, old_dirty);
    if (changed)
        p_state_changed(state);
    p_arena_free(&temporary);
    return 0;
}

bool p_state_normalize(os64_page_t *page)
{
    if (page->state == NULL)
        return true;
    os64_page_state_t *state = page->state;
    PReserve reserve = {.state = state};
    // The candidate stages normalized live properties; defaults captured
    // separately remain the reset answer. Node records preserve selectedness
    // when option order changes without an attribute assignment.
    for (int32_t i = 0; i < page->ncontrols; i++) {
        os64_page_control_t *c = &page->controls[i];
        if (!normalize_dirty(&reserve, c->node, c->element, c->input))
            goto no_memory;
        p_publish(page, i);
        if (c->input == OS64_PAGE_INPUT_RADIO || c->input == OS64_PAGE_INPUT_CHECKBOX) {
            (void)os64_page_node_checked(state, c->node, &c->checked);
        }
        for (int32_t o = 0; o < c->noptions; o++) {
            ((os64_page_option_t *)c->options)[o].selected = option_selected(state, c->options[o].node);
        }
    }
    for (int32_t i = 0; i < page->ncontrols; i++) {
        os64_page_control_t *c = &page->controls[i];
        if (c->input == OS64_PAGE_INPUT_RADIO && page->group_head[i] == i) {
            int32_t last = -1;
            for (int32_t at = i; at >= 0; at = page->group_next[at])
                if (page->controls[at].checked) {
                    if (last >= 0)
                        page->controls[last].checked = false;
                    last = at;
                }
        }
        if (c->element == OS64_PAGE_EL_SELECT && !c->multiple) {
            int32_t last = -1;
            for (int32_t o = 0; o < c->noptions; o++)
                if (c->options[o].selected) {
                    if (last >= 0)
                        ((os64_page_option_t *)c->options)[last].selected = false;
                    last = o;
                }
            const PNodeState *edit = p_state_find(state, c->node);
            if (last < 0 && (edit == NULL || !edit->selection_set) && p_select_one_line(c))
                for (int32_t o = 0; o < c->noptions; o++)
                    if (!c->options[o].disabled) {
                        ((os64_page_option_t *)c->options)[o].selected = true;
                        break;
                    }
        }
    }
    // Default records are sparse: untouched markup needs a record when
    // normalization changes its checkedness or selectedness. Existing
    // records retain their identity and dirty flags through normalization.
    bool changed = false;
    for (int32_t i = 0; i < page->ncontrols; i++) {
        const os64_page_control_t *c = &page->controls[i];
        if (c->input == OS64_PAGE_INPUT_RADIO || c->input == OS64_PAGE_INPUT_CHECKBOX) {
            bool before = false;
            (void)os64_page_node_checked(state, c->node, &before);
            changed |= before != c->checked;
            if ((p_state_find(state, c->node) != NULL ||
                 c->checked != p_has_attr(c->node, "checked")) &&
                !p_reserve_node(&reserve, c->node))
                goto no_memory;
        }
        for (int32_t o = 0; o < c->noptions; o++) {
            changed |= option_selected(state, c->options[o].node) != c->options[o].selected;
            if ((p_state_find(state, c->options[o].node) != NULL ||
                 c->options[o].selected != p_has_attr(c->options[o].node, "selected")) &&
                !p_reserve_node(&reserve, c->options[o].node))
                goto no_memory;
        }
    }
    // No allocation follows this commit.
    p_reserve_commit(&reserve);
    changed |= values_apply(&reserve);
    for (int32_t i = 0; i < page->ncontrols; i++) {
        const os64_page_control_t *c = &page->controls[i];
        if (c->input == OS64_PAGE_INPUT_RADIO || c->input == OS64_PAGE_INPUT_CHECKBOX) {
            PNodeState *edit = p_state_find(state, c->node);
            if (edit != NULL) {
                edit->on = c->checked ? 1 : 0;
                edit->on_attr = p_has_attr(c->node, "checked");
            }
        }
        for (int32_t o = 0; o < c->noptions; o++) {
            PNodeState *edit = p_state_find(state, c->options[o].node);
            if (edit != NULL) {
                edit->selected = c->options[o].selected ? 1 : 0;
                edit->selected_attr = p_has_attr(c->options[o].node, "selected");
            }
        }
    }
    p_state_publish(state);
    values_discard(&reserve, true);
    if (changed)
        p_state_changed(state);
    return true;
no_memory:
    p_reserve_abort(&reserve);
    return false;
}

int64_t p_state_choose(os64_page_t *page, int32_t control, int32_t option, bool on)
{
    const os64_page_control_t *c = &page->controls[control];
    os64_page_state_t *state = page->state;
    PReserve reserve = {.state = state};
    if (!p_reserve_node(&reserve, c->node))
        goto no_memory;
    for (int32_t i = 0; i < c->noptions; i++)
        if (!p_reserve_node(&reserve, c->options[i].node))
            goto no_memory;
    // Work out the final fallback before publishing, so putting the sole
    // first choice down and receiving it back does not advance the revision.
    bool any = false;
    int32_t fallback = -1;
    for (int32_t i = 0; i < c->noptions; i++) {
        bool chosen = i == option ? on : (on && !c->multiple ? false : c->options[i].selected);
        any |= chosen;
        if (fallback < 0 && !c->options[i].disabled)
            fallback = i;
    }
    bool use_fallback = !on && !any && p_select_one_line(c);
    p_reserve_commit(&reserve);
    bool changed = false;
    p_state_find(state, c->node)->selection_set = true;
    for (int32_t i = 0; i < c->noptions; i++) {
        bool chosen = use_fallback ? i == fallback :
            (i == option ? on : (on && !c->multiple ? false : c->options[i].selected));
        changed |= chosen != c->options[i].selected;
        p_state_find(state, c->options[i].node)->selected = chosen ? 1 : 0;
        p_state_find(state, c->options[i].node)->selected_dirty = true;
    }
    p_state_publish(state);
    if (changed)
        p_state_changed(state);
    return 0;
no_memory:
    p_reserve_abort(&reserve);
    return -OS64_PAGE_REASON_NO_MEMORY;
}

void p_state_reset(os64_page_t *page, int32_t form)
{
    os64_page_state_t *state = page->state;
    if (state == NULL)
        return;
    bool changed = false;
    // Retirement slots retain old bytes across publication without making
    // reset allocate. No reader observes them as live values.
    for (int32_t i = 0; i < page->ncontrols; i++) {
        const os64_page_control_t *c = &page->controls[i];
        if (c->form != form)
            continue;
        const PInitial *initial = &page->initial[i];
        changed |= !same_bytes(c->value, c->value_len, initial->value, initial->value_len) ||
            c->checked != initial->checked;
        PNodeState *edit = p_state_find(state, c->node);
        if (edit != NULL) {
            changed |= edit->text_user;
            char *keep = NULL;
            if (edit->text != NULL && same_bytes(edit->text, edit->text_len,
                initial->value, initial->value_len))
                keep = edit->text;
            else if (edit->cache != NULL && same_bytes(edit->cache, edit->cache_len,
                initial->value, initial->value_len))
                keep = edit->cache;
            edit->retired_text = edit->text != keep ? edit->text : NULL;
            edit->retired_cache = edit->cache != keep ? edit->cache : NULL;
            edit->text = NULL;
            edit->text_user = false;
            edit->cache = keep;
            edit->text_len = 0;
            edit->cache_len = keep != NULL ? initial->value_len : 0;
            edit->cache_version = keep != NULL ? os64_html_version(state->doc) : 0;
            edit->cache_state_version = 0;
            edit->on = initial->checked ? 1 : 0;
            edit->on_dirty = false;
            edit->on_attr = p_has_attr(c->node, "checked");
            edit->selection_set = false;
        }
        for (int32_t o = 0; o < c->noptions; o++) {
            changed |= c->options[o].selected != (initial->selected[o] != 0);
            PNodeState *choice = p_state_find(state, c->options[o].node);
            if (choice != NULL) {
                choice->selected = initial->selected[o] ? 1 : 0;
                choice->selected_dirty = false;
                choice->selected_attr = p_has_attr(c->options[o].node, "selected");
            }
        }
    }
    p_state_publish(state);
    for (int32_t i = 0; i < page->ncontrols; i++)
        if (page->controls[i].form == form) {
            PNodeState *edit = p_state_find(state, page->controls[i].node);
            if (edit != NULL) {
                p_state_dealloc(state, edit->retired_text);
                p_state_dealloc(state, edit->retired_cache);
                edit->retired_text = edit->retired_cache = NULL;
            }
        }
    if (changed)
        p_state_changed(state);
}
