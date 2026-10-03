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
        if (stage->clear) edit->text_clean = false;
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

static bool checked_raw(const os64_page_state_t *state, const os64_html_node_t *node)
{
    const PNodeState *edit = p_state_find(state, node);
    bool attribute = p_has_attr(node, "checked");
    return edit != NULL && edit->on >= 0 && (edit->on_dirty || edit->on_attr == attribute) ?
        edit->on != 0 : attribute;
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
    *on = checked_raw(state, node);
    if (*on && input == OS64_PAGE_INPUT_RADIO) {
        // Before any model exists, raw checked attributes still describe a
        // radio group: the last checked member in tree order wins. Stored
        // false records remain authoritative after a prior normalization.
        const os64_html_node_t *root = tree_root(node);
        for (const os64_html_node_t *at = tree_next(root, node); at != NULL; at = tree_next(root, at))
            if (radio_group_member(node, at) && checked_raw(state, at)) {
                *on = false;
                break;
            }
    }
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
// Attribute callers use os64_page_node_set_attr to apply each transition.
static bool normalize_current(PReserve *reserve, const os64_html_node_t *node,
                             os64_page_element_t element, os64_page_input_t input)
{
    os64_page_state_t *state = reserve->state;
    PNodeState *edit = p_state_find(state, node);
    uint64_t version = os64_html_version(state->doc);
    if (edit == NULL || edit->text == NULL || edit->text_version == version)
        return true;
    PArena temporary = {.budget = state};
    bool clear = p_value_is_attribute(element, input);
    bool clean_changed = false;
    if (edit->text_clean) {
        size_t baseline_len = 0;
        const char *baseline = p_child_text_into(&temporary, node, &baseline_len);
        if (baseline == NULL) { p_arena_free(&temporary); return false; }
        clear = !same_bytes(edit->cache, edit->cache_len, baseline, baseline_len);
        if (!clear) {
            PValueStage *stage = p_state_alloc(state, sizeof(*stage));
            if (stage == NULL) { p_arena_free(&temporary); return false; }
            os64_memset(stage, 0, sizeof(*stage));
            stage->edit = edit; stage->version = version;
            stage->next = reserve->values; reserve->values = stage;
            p_arena_free(&temporary);
            return true;
        }
        size_t normalized_len = 0;
        const char *normalized = p_sanitize_value(&temporary, node, element, input,
            baseline, baseline_len, &normalized_len);
        if (normalized == NULL) { p_arena_free(&temporary); return false; }
        clean_changed = !same_bytes(edit->text, edit->text_len, normalized, normalized_len);
    }
    size_t len = 0;
    const char *value = clear ? "" : p_sanitize_value(&temporary, node, element, input,
        edit->text, edit->text_len, &len);
    if (value == NULL) {
        p_arena_free(&temporary);
        return false;
    }
    bool changed = edit->text_clean ? clean_changed :
        !same_bytes(edit->text, edit->text_len, value, len);
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
    if (!normalize_current(&reserve, node, element, input)) {
        p_reserve_abort(&reserve);
        return -OS64_PAGE_REASON_NO_MEMORY;
    }
    if (!attribute && element != OS64_PAGE_EL_SELECT && edit != NULL && edit->text != NULL &&
        (reserve.values == NULL || !reserve.values->clear)) {
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
        raw = p_child_text_into(&temporary, node, &raw_len);
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
            edit->text_clean = false;
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
            before = p_child_text_into(&temporary, node, &before_len);
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
    edit->text_clean = false;
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
        if (!normalize_current(&reserve, c->node, c->element, c->input))
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
            edit->text_clean = false;
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

os64_html_document_t *os64_page_state_document(const os64_page_state_t *state)
{
    return state != NULL ? state->doc : NULL;
}

// A plan shadows existing records. New table slots and value bytes remain
// private until the HTML transaction succeeds; publication cannot allocate.
typedef struct PAttrStage {
    struct PAttrStage *next;
    PNodeState *original;
    PNodeState value;
    bool before_on, before_selected, value_staged;
    bool moving, select_trigger;
    const os64_html_node_t *owner_before;
} PAttrStage;

static PAttrStage *attr_stage(PReserve *reserve, PAttrStage **stages,
                              const os64_html_node_t *node)
{
    for (PAttrStage *at = *stages; at != NULL; at = at->next)
        if (at->value.node == node)
            return at;
    if (!p_reserve_node(reserve, node))
        return NULL;
    PNodeState *record = p_reserve_find(reserve, node);
    PAttrStage *stage = p_state_alloc(reserve->state, sizeof(*stage));
    if (stage == NULL)
        return NULL;
    stage->original = record;
    stage->value = *record;
    bool on_attr = p_has_attr(node, "checked"), selected_attr = p_has_attr(node, "selected");
    stage->before_on = record->on >= 0 && (record->on_dirty || record->on_attr == on_attr) ? record->on != 0 : on_attr;
    if (p_is(node, OS64_HTML_TAG_INPUT) &&
        (p_input_type(node) == OS64_PAGE_INPUT_RADIO || p_input_type(node) == OS64_PAGE_INPUT_CHECKBOX))
        (void)os64_page_node_checked(reserve->state, node, &stage->before_on);
    stage->before_selected = record->selected >= 0 && (record->selected_dirty || record->selected_attr == selected_attr) ? record->selected != 0 : selected_attr;
    stage->value_staged = false;
    stage->moving = stage->select_trigger = false;
    stage->owner_before = p_is(node, OS64_HTML_TAG_INPUT) && p_input_type(node) == OS64_PAGE_INPUT_RADIO ? node_owner(node) : NULL;
    stage->next = *stages;
    *stages = stage;
    return stage;
}

static void attr_stages_free(os64_page_state_t *state, PAttrStage *stages)
{
    while (stages != NULL) {
        PAttrStage *next = stages->next;
        // After publication value keeps the old pointers; before it, it keeps
        // the proposed pointers. Either way free only what differs.
        if (stages->value.text != stages->original->text)
            p_state_dealloc(state, stages->value.text);
        if (stages->value.cache != stages->original->cache)
            p_state_dealloc(state, stages->value.cache);
        p_state_dealloc(state, stages);
        stages = next;
    }
}

static bool attr_stage_value(os64_page_state_t *state, PAttrStage *stage,
                              const char *bytes, size_t len, bool dirty, bool user)
{
    char **slot = dirty ? &stage->value.text : &stage->value.cache;
    size_t *length = dirty ? &stage->value.text_len : &stage->value.cache_len;
    char *copy = *slot != NULL && same_bytes(*slot, *length, bytes, len) ? *slot :
        state_copy(state, bytes, len);
    if (copy == NULL)
        return false;
    *slot = copy;
    *length = len;
    stage->value_staged = true;
    if (dirty)
        stage->value.text_user = user;
    return true;
}

// ID lookup is evaluated against the proposed attribute view. The lookup
// still walks the actual tree, so a stack-local view never becomes a key.
static const os64_html_node_t *attr_owner(const os64_html_node_t *node,
                                         const os64_html_node_t *changed,
                                         const os64_html_node_t *view)
{
    const os64_html_node_t *attributes = node == changed ? view : node;
    const char *form = p_attr(attributes, "form");
    if (form == NULL)
        return attributes->form_owner != NULL ? attributes->form_owner : p_ancestor(node, OS64_HTML_TAG_FORM);
    const os64_html_node_t *root = tree_root(node);
    if (root->kind == OS64_HTML_DOCUMENT)
        for (const os64_html_node_t *at = root; at != NULL; at = tree_next(root, at)) {
            const char *id = p_attr(at == changed ? view : at, "id");
            if (id != NULL && os64_streq(id, form))
                return p_is(at, OS64_HTML_TAG_FORM) ? at : NULL;
        }
    return NULL;
}

static bool attr_radio_group(const os64_html_node_t *target, const os64_html_node_t *peer,
                              const os64_html_node_t *changed, const os64_html_node_t *view)
{
    const os64_html_node_t *a = target == changed ? view : target;
    const os64_html_node_t *b = peer == changed ? view : peer;
    const char *name = p_attr(a, "name"), *other = p_attr(b, "name");
    return p_is(b, OS64_HTML_TAG_INPUT) && p_input_type(b) == OS64_PAGE_INPUT_RADIO &&
        name != NULL && *name != '\0' && other != NULL && os64_streq(name, other) &&
        attr_owner(target, changed, view) == attr_owner(peer, changed, view);
}

static bool attr_plan_checked(os64_page_state_t *state, const PAttrStage *stages,
                               const os64_html_node_t *node)
{
    for (; stages != NULL; stages = stages->next)
        if (stages->value.node == node && stages->value.on >= 0)
            return stages->value.on != 0;
    bool on = false;
    (void)os64_page_node_checked(state, node, &on);
    return on;
}

// Match the list-of-options ancestry exclusions when an option attribute
// requests normalization; an excluded option has no select to update.
static const os64_html_node_t *option_select(const os64_html_node_t *option)
{
    bool group = false;
    for (const os64_html_node_t *at = option->parent; at != NULL; at = at->parent) {
        if (p_is(at, OS64_HTML_TAG_SELECT))
            return at;
        if (p_is(at, OS64_HTML_TAG_OPTION) || p_is(at, OS64_HTML_TAG_DATALIST) ||
            p_is(at, OS64_HTML_TAG_HR))
            return NULL;
        if (p_is(at, OS64_HTML_TAG_OPTGROUP)) {
            if (group)
                return NULL;
            group = true;
        }
    }
    return NULL;
}

// Capture the effective selectedness before a mutation changes the mode
// or list, including the single-line fallback before records exist.
static void attr_select_baseline(os64_page_state_t *state, PAttrStage *stages,
                                  const os64_html_node_t *select)
{
    bool multiple = p_has_attr(select, "multiple");
    const os64_html_node_t *last = NULL, *first = NULL;
    for (const os64_html_node_t *at = p_option_next(select, NULL); at != NULL; at = p_option_next(select, at)) {
        if (first == NULL && !p_option_disabled(select, at))
            first = at;
        if (option_selected(state, at))
            last = at;
    }
    const PNodeState *edit = p_state_find(state, select);
    os64_page_control_t control = {.node = select, .multiple = multiple};
    if (!multiple && last == NULL && (edit == NULL || !edit->selection_set) && p_select_one_line(&control))
        last = first;
    for (PAttrStage *stage = stages; stage != NULL; stage = stage->next)
        if (p_is(stage->value.node, OS64_HTML_TAG_OPTION) &&
            option_select(stage->value.node) == select) {
            bool selected = multiple ? option_selected(state, stage->value.node) : stage->value.node == last;
            stage->before_selected = selected;
            stage->value.selected = selected;
            stage->value.selected_attr = p_has_attr(stage->value.node, "selected");
        }
}

static bool attr_selection(PReserve *reserve, PAttrStage **stages,
                            const os64_html_node_t *select, const os64_html_node_t *select_view,
                            const os64_html_node_t *changed, const os64_html_node_t *changed_view)
{
    os64_page_state_t *state = reserve->state;
    if (!attr_stage(reserve, stages, select))
        return false;
    for (const os64_html_node_t *at = p_option_next(select, NULL); at != NULL; at = p_option_next(select, at))
        if (!attr_stage(reserve, stages, at))
            return false;
    attr_select_baseline(state, *stages, select);
    bool multiple = p_has_attr(select_view, "multiple");
    const os64_html_node_t *last = NULL, *first = NULL;
    bool preferred = p_is(changed, OS64_HTML_TAG_OPTION) &&
        !p_has_attr(changed, "selected") && p_has_attr(changed_view, "selected") &&
        (p_state_find(state, changed) == NULL || !p_state_find(state, changed)->selected_dirty);
    for (const os64_html_node_t *at = p_option_next(select, NULL); at != NULL; at = p_option_next(select, at)) {
        PAttrStage *stage = attr_stage(reserve, stages, at);
        const os64_html_node_t *view = at == changed ? changed_view : at;
        if (at == changed && !stage->value.selected_dirty &&
            p_has_attr(at, "selected") != p_has_attr(view, "selected"))
            stage->value.selected = p_has_attr(view, "selected");
        if (first == NULL && !p_option_disabled(select, at))
            first = at;
        if (stage->value.selected)
            last = at;
        stage->value.selected_attr = p_has_attr(view, "selected");
    }
    if (!multiple && preferred)
        last = changed;
    os64_page_control_t control = {.node = select_view, .multiple = multiple};
    if (!multiple && last == NULL && p_select_one_line(&control))
        last = first;
    if (!multiple)
        for (PAttrStage *stage = *stages; stage != NULL; stage = stage->next)
            if (p_is(stage->value.node, OS64_HTML_TAG_OPTION) &&
                option_select(stage->value.node) == select)
                stage->value.selected = stage->value.node == last;
    attr_stage(reserve, stages, select)->value.selection_set = false;
    return true;
}

int64_t os64_page_node_set_attr(os64_page_state_t *state, const os64_html_node_t *node,
                                const char *name, const char *value, size_t len, bool remove)
{
    if (state == NULL || !os64_html_owns_node(state->doc, node) ||
        node->kind != OS64_HTML_ELEMENT || name == NULL || *name == '\0' ||
        (!remove && value == NULL && len != 0) || (!remove && len == SIZE_MAX))
        return OS64_HTML_BAD_ARGUMENT;
    const os64_html_attr_t *existing = os64_html_attr(node, name);
    os64_html_attr_change_t changes[2] = {{name, value, len, remove}};
    if ((remove && existing == NULL) || (!remove && existing != NULL &&
        same_bytes(existing->value, os64_strlen(existing->value), value, len)))
        return os64_html_set_attrs(state->doc, (os64_html_node_t *)node, changes, 1);
    PArena temporary = {.budget = state};
    PReserve reserve = {.state = state};
    PAttrStage *stages = NULL;
    os64_html_node_t view = *node;
    // A prospective list borrows existing strings and owns only its records.
    // The requested bytes are terminated for the shared scalar sanitizers.
    char *terminated = remove ? NULL : p_arena_copy(&temporary, value != NULL ? value : "", len);
    if (!remove && terminated == NULL)
        goto no_memory;
    os64_html_attr_t **tail = &view.attrs;
    view.attrs = NULL;
    bool found = false;
    for (const os64_html_attr_t *a = node->attrs; a != NULL; a = a->next) {
        bool match = os64_streq(a->name, name);
        found |= match;
        if (match && remove)
            continue;
        os64_html_attr_t *copy = p_arena_alloc(&temporary, sizeof(*copy));
        if (copy == NULL)
            goto no_memory;
        *copy = *a;
        copy->next = NULL;
        if (match)
            copy->value = terminated;
        *tail = copy;
        tail = &copy->next;
    }
    if (!found && !remove) {
        os64_html_attr_t *copy = p_arena_alloc(&temporary, sizeof(*copy));
        if (copy == NULL)
            goto no_memory;
        *copy = (os64_html_attr_t){.name = name, .value = terminated};
        *tail = copy;
    }
    if (os64_streq(name, "form") && !remove)
        view.form_owner = NULL;
    size_t count = 1;
    PNodeState *edit = p_state_find(state, node);
    bool input_node = p_is(node, OS64_HTML_TAG_INPUT);
    os64_page_input_t before = input_node ? p_input_type(node) : OS64_PAGE_INPUT_NONE;
    os64_page_input_t after = input_node ? p_input_type(&view) : OS64_PAGE_INPUT_NONE;
    bool type = input_node && os64_streq(name, "type") && before != after;
    bool value_effect = input_node && (type || os64_streq(name, "value") ||
        (after == OS64_PAGE_INPUT_EMAIL && os64_streq(name, "multiple")) ||
        (after == OS64_PAGE_INPUT_RANGE && (os64_streq(name, "min") ||
            os64_streq(name, "max") || os64_streq(name, "step"))));
    if (value_effect) {
        PAttrStage *stage = attr_stage(&reserve, &stages, node);
        if (stage == NULL)
            goto no_memory;
        bool before_attr = p_value_is_attribute(OS64_PAGE_EL_INPUT, before);
        bool after_attr = p_value_is_attribute(OS64_PAGE_EL_INPUT, after);
        const char *raw = edit != NULL && edit->text != NULL ? edit->text : p_attr(node, "value");
        size_t raw_len = edit != NULL && edit->text != NULL ? edit->text_len :
            (raw != NULL ? os64_strlen(raw) : 0);
        if (raw == NULL)
            raw = "";
        if (type && !before_attr && before != OS64_PAGE_INPUT_FILE && after_attr && raw_len != 0) {
            // Normalize the old mode before transferring its current value.
            size_t old_len = 0;
            const char *old = p_sanitize_value(&temporary, node, OS64_PAGE_EL_INPUT, before,
                                               raw, raw_len, &old_len);
            if (old == NULL)
                goto no_memory;
            if (old_len != 0) {
                changes[count++] = (os64_html_attr_change_t){"value", old, old_len, false};
                raw = old;
                raw_len = old_len;
            } else {
                raw = p_attr(&view, "value");
                raw_len = raw != NULL ? os64_strlen(raw) : 0;
                if (raw == NULL) raw = "";
            }
        } else if (after_attr || (type && (before_attr || before == OS64_PAGE_INPUT_FILE)) ||
                   (edit == NULL || edit->text == NULL)) {
            raw = p_attr(&view, "value");
            if (raw == NULL && (after == OS64_PAGE_INPUT_CHECKBOX || after == OS64_PAGE_INPUT_RADIO))
                raw = "on";
            raw_len = raw != NULL ? os64_strlen(raw) : 0;
            if (raw == NULL) raw = "";
        }
        bool dirty = !after_attr && after != OS64_PAGE_INPUT_FILE &&
            edit != NULL && edit->text != NULL && !(type && (before_attr || before == OS64_PAGE_INPUT_FILE));
        size_t normalized_len = 0;
        const char *normalized = p_sanitize_value(&temporary, &view, OS64_PAGE_EL_INPUT, after,
                                                   raw, raw_len, &normalized_len);
        if (normalized == NULL || !attr_stage_value(state, stage, normalized, normalized_len, dirty,
                                                    dirty && edit->text_user))
            goto no_memory;
        if (!dirty) {
            stage->value.text = NULL;
            stage->value.text_len = 0;
            stage->value.text_user = false;
        }
    }
    bool radio_effect = os64_streq(name, "id") || (input_node &&
        (os64_streq(name, "checked") || type || os64_streq(name, "name") || os64_streq(name, "form")));
    if (radio_effect) {
        const os64_html_node_t *root = tree_root(node);
        // Preserve the old groups' effective checkedness as well as the new
        // group's winner. A raw earlier checked attribute must not revive
        // merely because the old winner leaves that group.
        for (const os64_html_node_t *at = root; at != NULL; at = tree_next(root, at))
            if (p_is(at, OS64_HTML_TAG_INPUT) && p_input_type(at) == OS64_PAGE_INPUT_RADIO) {
                PAttrStage *stage = attr_stage(&reserve, &stages, at);
                if (stage == NULL)
                    goto no_memory;
                stage->value.on = stage->before_on;
                stage->value.on_attr = p_has_attr(at, "checked");
            }
    }
    if (input_node && (os64_streq(name, "checked") || type || os64_streq(name, "name") || os64_streq(name, "form"))) {
        PAttrStage *stage = attr_stage(&reserve, &stages, node);
        if (stage == NULL)
            goto no_memory;
        bool checked = os64_streq(name, "checked") && (edit == NULL || !edit->on_dirty) ?
            p_has_attr(&view, "checked") : stage->before_on;
        stage->value.on = checked;
        stage->value.on_attr = p_has_attr(&view, "checked");
        if (after == OS64_PAGE_INPUT_RADIO && checked)
            for (const os64_html_node_t *at = tree_root(node); at != NULL; at = tree_next(tree_root(node), at))
                if (at != node && attr_radio_group(node, at, node, &view)) {
                    PAttrStage *peer = attr_stage(&reserve, &stages, at);
                    if (peer == NULL)
                        goto no_memory;
                    peer->value.on = 0;
                    peer->value.on_attr = p_has_attr(at, "checked");
                }
    }
    if (os64_streq(name, "id")) {
        const os64_html_node_t *root = tree_root(node);
        for (const os64_html_node_t *at = root; at != NULL; at = tree_next(root, at))
            if (p_is(at, OS64_HTML_TAG_INPUT) && p_input_type(at) == OS64_PAGE_INPUT_RADIO &&
                node_owner(at) != attr_owner(at, node, &view) && attr_plan_checked(state, stages, at)) {
                for (const os64_html_node_t *peer = root; peer != NULL; peer = tree_next(root, peer))
                    if (peer != at && attr_radio_group(at, peer, node, &view)) {
                        PAttrStage *stage = attr_stage(&reserve, &stages, peer);
                        if (stage == NULL)
                            goto no_memory;
                        stage->value.on = 0;
                        stage->value.on_attr = p_has_attr(peer, "checked");
                    }
            }
    }
    const os64_html_node_t *select = p_is(node, OS64_HTML_TAG_SELECT) ? node :
        p_is(node, OS64_HTML_TAG_OPTION) ? option_select(node) : NULL;
    if (select != NULL && ((select == node && (os64_streq(name, "multiple") || os64_streq(name, "size"))) ||
        (select != node && os64_streq(name, "selected"))))
        if (!attr_selection(&reserve, &stages, select, select == node ? &view : select, node, &view))
            goto no_memory;
    int64_t status = os64_html_set_attrs(state->doc, (os64_html_node_t *)node, changes, count);
    if (status != OS64_HTML_OK) {
        attr_stages_free(state, stages);
        p_reserve_abort(&reserve);
        p_arena_free(&temporary);
        return status;
    }
    p_reserve_commit(&reserve);
    bool changed = false;
    uint64_t version = os64_html_version(state->doc);
    for (PAttrStage *stage = stages; stage != NULL; stage = stage->next) {
        PNodeState *old = stage->original;
        PNodeState *fresh = &stage->value;
        changed |= (old->text == NULL) != (fresh->text == NULL) || old->text_user != fresh->text_user ||
            old->selection_set != fresh->selection_set ||
            (old->text != NULL && fresh->text != NULL && !same_bytes(old->text, old->text_len, fresh->text, fresh->text_len)) ||
            (fresh->on >= 0 && stage->before_on != (fresh->on != 0)) ||
            (fresh->selected >= 0 && stage->before_selected != (fresh->selected != 0));
        if (stage->value_staged) {
            fresh->text_version = version;
            if (fresh->text == NULL && fresh->cache != NULL) fresh->cache_version = version;
        }
        PNodeState previous = *old;
        fresh->next = old->next;
        *old = *fresh;
        stage->value = previous;
    }
    p_state_publish(state);
    attr_stages_free(state, stages);
    if (changed)
        p_state_changed(state);
    p_arena_free(&temporary);
    return OS64_HTML_OK;
no_memory:
    attr_stages_free(state, stages);
    p_reserve_abort(&reserve);
    p_arena_free(&temporary);
    return OS64_HTML_NO_MEMORY;
}

// Structural changes need reservations for both sides of a move. Template
// contents enter the queue as independent roots, preserving their form trees.
typedef struct PTreeRoot {
    struct PTreeRoot *next;
    const os64_html_node_t *node;
    bool moving;
} PTreeRoot;

static bool tree_plan_root(PArena *scratch, PTreeRoot **roots,
                           const os64_html_node_t *node, bool moving)
{
    if (node == NULL)
        return true;
    for (PTreeRoot *at = *roots; at != NULL; at = at->next)
        if (at->node == node && at->moving == moving)
            return true;
    PTreeRoot *root = p_arena_alloc(scratch, sizeof(*root));
    if (root == NULL)
        return false;
    *root = (PTreeRoot){.node = node, .moving = moving};
    PTreeRoot **tail = roots;
    while (*tail != NULL)
        tail = &(*tail)->next;
    *tail = root;
    return true;
}

static bool tree_plan_collect(PReserve *reserve, PArena *scratch,
                               PTreeRoot **roots, PAttrStage **stages)
{
    for (PTreeRoot *root = *roots; root != NULL; root = root->next)
        for (const os64_html_node_t *at = root->node; at != NULL; at = tree_next(root->node, at)) {
            if (at->template_contents != NULL &&
                !tree_plan_root(scratch, roots, at->template_contents, root->moving))
                return false;
            if (p_is(at, OS64_HTML_TAG_INPUT) || p_is(at, OS64_HTML_TAG_OPTION) ||
                p_is(at, OS64_HTML_TAG_SELECT) ||
                (p_is(at, OS64_HTML_TAG_TEXTAREA) && p_state_find(reserve->state, at) != NULL &&
                 p_state_find(reserve->state, at)->text_clean)) {
                PAttrStage *stage = attr_stage(reserve, stages, at);
                if (stage == NULL)
                    return false;
                stage->moving |= root->moving;
                if (p_is(at, OS64_HTML_TAG_INPUT)) {
                    stage->value.on = stage->before_on;
                    stage->value.on_attr = p_has_attr(at, "checked");
                }
                if (root->moving && p_is(at, OS64_HTML_TAG_OPTION)) {
                    stage->value.selected = stage->before_selected;
                    stage->value.selected_attr = p_has_attr(at, "selected");
                }
                if (root->moving && p_is(at, OS64_HTML_TAG_SELECT))
                    stage->select_trigger = true;
            }
        }
    return true;
}

static PAttrStage *tree_plan_find(PAttrStage *stages, const os64_html_node_t *node)
{
    for (; stages != NULL; stages = stages->next)
        if (stages->value.node == node)
            return stages;
    return NULL;
}

static void tree_plan_select(PAttrStage *stages, const os64_html_node_t *node)
{
    const os64_html_node_t *select = p_is(node, OS64_HTML_TAG_SELECT) ? node :
        p_ancestor(node, OS64_HTML_TAG_SELECT);
    PAttrStage *stage = tree_plan_find(stages, select);
    if (stage != NULL)
        stage->select_trigger = true;
}

static bool tree_radio_same(const os64_html_node_t *a, const os64_html_node_t *b)
{
    return tree_root(a) == tree_root(b) && radio_group_member(a, b);
}

static bool tree_radio_trigger(const PAttrStage *stage)
{
    return stage->moving || stage->owner_before != node_owner(stage->value.node);
}

static void tree_plan_normalize(os64_page_state_t *state, PAttrStage *stages)
{
    // Choose the last checked incoming/group-changed radio in final tree
    // order. Unchanged peers do not take priority over an inserted control.
    for (PAttrStage *stage = stages; stage != NULL; stage = stage->next) {
        const os64_html_node_t *node = stage->value.node;
        if (!p_is(node, OS64_HTML_TAG_INPUT) || p_input_type(node) != OS64_PAGE_INPUT_RADIO ||
            !stage->value.on || !tree_radio_trigger(stage))
            continue;
        const os64_html_node_t *root = tree_root(node);
        PAttrStage *winner = stage;
        for (const os64_html_node_t *at = root; at != NULL; at = tree_next(root, at)) {
            PAttrStage *candidate = tree_plan_find(stages, at);
            if (candidate != NULL && candidate->value.on && tree_radio_trigger(candidate) &&
                tree_radio_same(node, at))
                winner = candidate;
        }
        for (PAttrStage *peer = stages; peer != NULL; peer = peer->next)
            if (peer != winner && tree_radio_same(winner->value.node, peer->value.node))
                peer->value.on = 0;
    }
    for (PAttrStage *stage = stages; stage != NULL; stage = stage->next) {
        const os64_html_node_t *select = stage->value.node;
        if (!stage->select_trigger || !p_is(select, OS64_HTML_TAG_SELECT))
            continue;
        bool multiple = p_has_attr(select, "multiple");
        const os64_html_node_t *last = NULL, *first = NULL;
        bool incoming = false;
        for (const os64_html_node_t *at = p_option_next(select, NULL); at != NULL; at = p_option_next(select, at)) {
            PAttrStage *option = tree_plan_find(stages, at);
            if (first == NULL && !p_option_disabled(select, at))
                first = at;
            if (option != NULL && option->value.selected && (!incoming || option->moving)) {
                last = at;
                incoming |= option->moving;
            }
        }
        os64_page_control_t control = {.node = select, .multiple = multiple};
        // A changed child list runs the select's reset algorithm even when
        // an earlier property assignment deliberately cleared its selection.
        if (!multiple && last == NULL && p_select_one_line(&control))
            last = first;
        if (!multiple)
            for (const os64_html_node_t *at = p_option_next(select, NULL); at != NULL; at = p_option_next(select, at)) {
                PAttrStage *option = tree_plan_find(stages, at);
                if (option != NULL)
                    option->value.selected = at == last;
            }
        stage->value.selection_set = false;
    }
    (void)state;
}

// Compare the LF-normalized direct child text without allocating after the
// HTML commit. CRLF pairs can span adjacent Text nodes in the concatenation.
static bool textarea_value_equal(const os64_html_node_t *node, const char *value, size_t len)
{
    size_t offset = 0;
    bool cr = false;
    for (const os64_html_node_t *at = node->first_child; at != NULL; at = at->next)
        if (at->kind == OS64_HTML_TEXT)
            for (size_t i = 0; i < at->text_len; i++) {
                char byte = at->text[i];
                if (cr && byte == '\n') { cr = false; continue; }
                cr = byte == '\r';
                if (offset == len || value[offset++] != (cr ? '\n' : byte)) return false;
            }
    return offset == len;
}

static void tree_plan_publish(os64_page_state_t *state, PAttrStage *stages)
{
    bool changed = false;
    for (PAttrStage *stage = stages; stage != NULL; stage = stage->next) {
        PNodeState *old = stage->original;
        PNodeState *fresh = &stage->value;
        changed |= (fresh->on >= 0 && stage->before_on != (fresh->on != 0)) ||
            (fresh->selected >= 0 && stage->before_selected != (fresh->selected != 0)) ||
            old->selection_set != fresh->selection_set ||
            (old->text_clean && !fresh->text_clean &&
             !textarea_value_equal(fresh->node, old->text, old->text_len));
        fresh->next = old->next;
        PNodeState previous = *old;
        *old = *fresh;
        stage->value = previous;
    }
    p_state_publish(state);
    attr_stages_free(state, stages);
    if (changed)
        p_state_changed(state);
}

// Direct child changes reset a clean textarea's current value even if callers restore
// the old children before reading it again. Dirty values survive those changes.
static void tree_plan_textarea(PAttrStage *stages, const os64_html_node_t *parent)
{
    if (parent != NULL) {
        PAttrStage *stage = tree_plan_find(stages, parent);
        if (stage != NULL && stage->value.text_clean) {
            stage->value.text = stage->value.cache = NULL;
            stage->value.text_len = stage->value.cache_len = 0;
            stage->value.text_clean = false;
        }
    }
}

int64_t os64_page_node_set_text(os64_page_state_t *state, os64_html_node_t *node,
                                const char *bytes, size_t len)
{
    if (state == NULL || !os64_html_owns_node(state->doc, node))
        return OS64_HTML_BAD_ARGUMENT;
    if ((node->kind == OS64_HTML_TEXT || node->kind == OS64_HTML_COMMENT) &&
        (bytes != NULL || len == 0) && same_bytes(node->text, node->text_len, bytes, len))
        return os64_html_set_text(state->doc, node, bytes, len);
    PReserve reserve = {.state = state};
    PAttrStage *stages = NULL;
    if (node->parent != NULL) {
        const os64_html_node_t *at = node->parent;
        const PNodeState *edit = p_state_find(state, at);
        if (edit != NULL && edit->text_clean && attr_stage(&reserve, &stages, at) == NULL) {
            attr_stages_free(state, stages); p_reserve_abort(&reserve);
            return OS64_HTML_NO_MEMORY;
        }
    }
    tree_plan_textarea(stages, node->parent);
    uint64_t version = os64_html_version(state->doc);
    int64_t status = os64_html_set_text(state->doc, node, bytes, len);
    if (status == OS64_HTML_OK && version != os64_html_version(state->doc)) {
        p_reserve_commit(&reserve);
        tree_plan_publish(state, stages);
    } else {
        attr_stages_free(state, stages); p_reserve_abort(&reserve);
    }
    return status;
}

typedef enum { P_TREE_INSERT, P_TREE_REPLACE, P_TREE_REMOVE, P_TREE_CHILDREN } PTreeVerb;

static int64_t tree_change(os64_page_state_t *state, PTreeVerb verb,
                           const os64_html_node_t *parent, const os64_html_node_t *node,
                           const os64_html_node_t *reference)
{
    if (state == NULL || (parent != NULL && !os64_html_owns_node(state->doc, parent)) ||
        (node != NULL && !os64_html_owns_node(state->doc, node)) ||
        (reference != NULL && !os64_html_owns_node(state->doc, reference)))
        return OS64_HTML_BAD_ARGUMENT;
    if (verb == P_TREE_REMOVE && node == NULL)
        return OS64_HTML_BAD_ARGUMENT;
    if ((verb == P_TREE_INSERT || verb == P_TREE_REPLACE) && (parent == NULL || node == NULL))
        return OS64_HTML_BAD_ARGUMENT;
    if (verb == P_TREE_CHILDREN && (parent == NULL ||
        (parent->kind != OS64_HTML_ELEMENT && parent->kind != OS64_HTML_FRAGMENT) ||
        (node != NULL && (node->parent != NULL || node == parent))))
        return OS64_HTML_BAD_ARGUMENT;
    if (verb == P_TREE_REMOVE && node->parent == NULL)
        return os64_html_remove(state->doc, (os64_html_node_t *)node);
    if ((verb == P_TREE_INSERT && node->kind == OS64_HTML_FRAGMENT && node->first_child == NULL) ||
        (verb == P_TREE_REPLACE && node == reference))
        return verb == P_TREE_INSERT ? os64_html_insert(state->doc, (os64_html_node_t *)parent,
            (os64_html_node_t *)node, (os64_html_node_t *)reference) :
            os64_html_replace(state->doc, (os64_html_node_t *)parent, (os64_html_node_t *)node,
                              (os64_html_node_t *)reference);
    if (verb == P_TREE_CHILDREN && parent->first_child == NULL &&
        (node == NULL || (node->kind == OS64_HTML_FRAGMENT && node->first_child == NULL)))
        return OS64_HTML_OK;
    PReserve reserve = {.state = state};
    PArena scratch = {.budget = state};
    PTreeRoot *roots = NULL;
    PAttrStage *stages = NULL;
    if (!tree_plan_root(&scratch, &roots, state->doc->document, false) ||
        !tree_plan_root(&scratch, &roots, parent != NULL ? tree_root(parent) : NULL, false) ||
        !tree_plan_root(&scratch, &roots, node != NULL ? tree_root(node) : NULL, false) ||
        !tree_plan_root(&scratch, &roots, reference != NULL ? tree_root(reference) : NULL, false) ||
        !tree_plan_root(&scratch, &roots, node, true) ||
        !tree_plan_root(&scratch, &roots, verb == P_TREE_REPLACE ? reference : NULL, true))
        goto no_memory;
    if (verb == P_TREE_CHILDREN)
        for (const os64_html_node_t *at = parent->first_child; at != NULL; at = at->next)
            if (!tree_plan_root(&scratch, &roots, at, true))
                goto no_memory;
    if (!tree_plan_collect(&reserve, &scratch, &roots, &stages))
        goto no_memory;
    for (PAttrStage *stage = stages; stage != NULL; stage = stage->next)
        if (stage->moving && p_is(stage->value.node, OS64_HTML_TAG_OPTION))
            tree_plan_select(stages, stage->value.node);
    if (parent != NULL)
        tree_plan_select(stages, parent);
    for (PAttrStage *stage = stages; stage != NULL; stage = stage->next)
        if (stage->select_trigger)
            attr_select_baseline(state, stages, stage->value.node);
    tree_plan_textarea(stages, parent);
    if (node != NULL) tree_plan_textarea(stages, node->parent);
    if (verb == P_TREE_REPLACE && reference != NULL)
        tree_plan_textarea(stages, reference->parent);
    const os64_html_node_t *old_first = verb == P_TREE_CHILDREN ? parent->first_child : NULL;
    uint64_t version = os64_html_version(state->doc);
    int64_t status;
    if (verb == P_TREE_REMOVE)
        status = os64_html_remove(state->doc, (os64_html_node_t *)node);
    else if (verb == P_TREE_REPLACE)
        status = os64_html_replace(state->doc, (os64_html_node_t *)parent, (os64_html_node_t *)node,
                                   (os64_html_node_t *)reference);
    else if (node != NULL)
        status = os64_html_insert(state->doc, (os64_html_node_t *)parent, (os64_html_node_t *)node,
                                  (os64_html_node_t *)(verb == P_TREE_CHILDREN ? old_first : reference));
    else
        status = OS64_HTML_OK;
    if (status != OS64_HTML_OK) {
        attr_stages_free(state, stages);
        p_reserve_abort(&reserve);
        p_arena_free(&scratch);
        return status;
    }
    if (verb == P_TREE_CHILDREN)
        for (const os64_html_node_t *at = old_first; at != NULL;) {
            const os64_html_node_t *next = at->next;
            // A known child of an element/fragment cannot hit the document
            // root guard, and remove allocates nothing after preparation.
            (void)os64_html_remove(state->doc, (os64_html_node_t *)at);
            at = next;
        }
    if (version == os64_html_version(state->doc)) {
        attr_stages_free(state, stages);
        p_reserve_abort(&reserve);
    } else {
        tree_plan_normalize(state, stages);
        p_reserve_commit(&reserve);
        tree_plan_publish(state, stages);
    }
    p_arena_free(&scratch);
    return OS64_HTML_OK;
no_memory:
    attr_stages_free(state, stages);
    p_reserve_abort(&reserve);
    p_arena_free(&scratch);
    return OS64_HTML_NO_MEMORY;
}

int64_t os64_page_node_insert(os64_page_state_t *state, os64_html_node_t *parent,
                              os64_html_node_t *node, os64_html_node_t *before)
{
    return tree_change(state, P_TREE_INSERT, parent, node, before);
}

int64_t os64_page_node_replace(os64_page_state_t *state, os64_html_node_t *parent,
                               os64_html_node_t *node, os64_html_node_t *old)
{
    return tree_change(state, P_TREE_REPLACE, parent, node, old);
}

int64_t os64_page_node_remove(os64_page_state_t *state, os64_html_node_t *node)
{
    return tree_change(state, P_TREE_REMOVE, NULL, node, NULL);
}

int64_t os64_page_node_replace_children(os64_page_state_t *state,
                                       os64_html_node_t *parent,
                                       os64_html_node_t *replacement)
{
    return tree_change(state, P_TREE_CHILDREN, parent, replacement, NULL);
}

// Pairing the detached copies with their originals also visits template content
// fragments, which are separate trees rather than ordinary element children.
typedef struct PClonePair {
    struct PClonePair *next;
    const os64_html_node_t *source, *copy;
} PClonePair;

static bool clone_pair(PArena *scratch, PClonePair ***tail,
                        const os64_html_node_t *source, const os64_html_node_t *copy)
{
    PClonePair *pair = p_arena_alloc(scratch, sizeof(*pair));
    if (pair == NULL) return false;
    *pair = (PClonePair){.source = source, .copy = copy};
    **tail = pair;
    *tail = &pair->next;
    return true;
}

os64_html_node_t *os64_page_node_clone(os64_page_state_t *state,
                                       const os64_html_node_t *node, bool deep,
                                       int64_t *status)
{
    int64_t why = OS64_HTML_BAD_ARGUMENT;
    os64_html_node_t *copy = NULL;
    if (state == NULL || !os64_html_owns_node(state->doc, node) ||
        node->kind == OS64_HTML_DOCUMENT)
        goto done;
    copy = os64_html_clone(state->doc, node, deep, &why);
    if (copy == NULL) goto done;
    PReserve reserve = {.state = state};
    PArena scratch = {.budget = state};
    PClonePair *pairs = NULL, **tail = &pairs;
    if (!clone_pair(&scratch, &tail, node, copy)) goto no_memory;
    for (PClonePair *pair = pairs; pair != NULL; pair = pair->next) {
        const os64_html_node_t *source = pair->source, *target = pair->copy;
        bool input = p_is(source, OS64_HTML_TAG_INPUT);
        bool textarea = p_is(source, OS64_HTML_TAG_TEXTAREA);
        if (input || textarea) {
            if (!p_reserve_node(&reserve, target)) goto no_memory;
            PNodeState *fresh = p_reserve_find(&reserve, target);
            const PNodeState *old = p_state_find(state, source);
            os64_page_input_t type = input ? p_input_type(source) : OS64_PAGE_INPUT_NONE;
            os64_page_element_t element = input ? OS64_PAGE_EL_INPUT : OS64_PAGE_EL_TEXTAREA;
            if (input) {
                bool checked = checked_raw(state, source);
                if (type == OS64_PAGE_INPUT_RADIO)
                    (void)os64_page_node_checked(state, source, &checked);
                fresh->on = checked ? 1 : 0;
                fresh->on_dirty = old != NULL && old->on_dirty;
                fresh->on_attr = p_has_attr(target, "checked");
            }
            bool dirty = old != NULL && old->text != NULL && !old->text_clean;
            if (input && (p_value_is_attribute(element, type) || type == OS64_PAGE_INPUT_FILE))
                dirty = false;
            if (textarea || dirty) {
                size_t len = 0, normalized_len = 0;
                const char *raw = old != NULL && old->text != NULL ? old->text : NULL;
                if (raw != NULL) len = old->text_len;
                if (textarea && (raw == NULL || old->text_clean)) {
                    size_t child_len = 0;
                    const char *children = p_child_text_into(&scratch, source, &child_len);
                    if (children == NULL) goto no_memory;
                    if (raw == NULL || !same_bytes(old->cache, old->cache_len, children, child_len)) {
                        raw = children; len = child_len;
                    }
                }
                const char *normalized = p_sanitize_value(&scratch, source, element, type,
                    raw != NULL ? raw : "", len, &normalized_len);
                fresh->text = normalized != NULL ? state_copy(state, normalized, normalized_len) : NULL;
                if (fresh->text == NULL) goto no_memory;
                fresh->text_len = normalized_len;
                fresh->text_version = os64_html_version(state->doc);
                fresh->text_user = dirty && old->text_user;
                fresh->text_clean = textarea && !dirty;
                if (fresh->text_clean) {
                    size_t baseline_len = 0;
                    const char *baseline = p_child_text_into(&scratch, target, &baseline_len);
                    fresh->cache = baseline != NULL ? state_copy(state, baseline, baseline_len) : NULL;
                    if (fresh->cache == NULL) goto no_memory;
                    fresh->cache_len = baseline_len;
                    fresh->cache_version = fresh->text_version;
                }
            }
        }
        if (deep) {
            const os64_html_node_t *a = source->first_child, *b = target->first_child;
            for (; a != NULL && b != NULL; a = a->next, b = b->next)
                if (!clone_pair(&scratch, &tail, a, b)) goto no_memory;
            if (source->template_contents != NULL && target->template_contents != NULL &&
                !clone_pair(&scratch, &tail, source->template_contents, target->template_contents))
                goto no_memory;
        }
    }
    p_reserve_commit(&reserve);
    p_arena_free(&scratch);
    why = OS64_HTML_OK;
    goto done;
no_memory:
    // These records have never been published; abort normally only owns their
    // record storage, so release the staged property bytes first.
    for (PNodeState *edit = reserve.records; edit != NULL; edit = edit->next) {
        p_state_dealloc(state, edit->text);
        p_state_dealloc(state, edit->cache);
        edit->text = edit->cache = NULL;
    }
    p_reserve_abort(&reserve);
    p_arena_free(&scratch);
    copy = NULL;
    why = OS64_HTML_NO_MEMORY;
done:
    if (status != NULL) *status = why;
    return copy;
}
