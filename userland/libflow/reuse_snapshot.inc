// A layout's history cursor advances, but its origin version does not. Dirty
// bits therefore accumulate over the saved membership, never current parents.
// The owning tree pins the document while this index can name removed nodes.
typedef struct {
    const os64_html_node_t *node;
    size_t parent;
    const FBox *box;
    bool changed, dirty, ambiguous, animated;
} FReuseNode;

struct FReuseSnapshot {
    os64_html_reader_t *reader;
    flow_reuse_inputs_t inputs;
    flow_env_t env;
    const os64_html_document_t *doc;
    uint64_t generation, version;
    size_t count, slots, bytes;
    int32_t width;
    bool lost;
    // Nodes and hash slots follow in this allocation, charged together.
};

_Static_assert(sizeof(FReuseSnapshot) % _Alignof(FReuseNode) == 0,
               "snapshot node array must be aligned");

static FReuseNode *reuse_nodes(FReuseSnapshot *r)
{
    return (FReuseNode *)(r + 1);
}
static uint32_t *reuse_slots(FReuseSnapshot *r)
{
    return (uint32_t *)(reuse_nodes(r) + r->count);
}
static size_t reuse_hash(const os64_html_node_t *node, size_t slots)
{
    uint64_t x = (uintptr_t)node;
    x ^= x >> 33; x *= UINT64_C(0xff51afd7ed558ccd); x ^= x >> 33;
    return (size_t)x & (slots - 1);
}
static size_t reuse_find(FReuseSnapshot *r, const os64_html_node_t *node)
{
    if (!node) return SIZE_MAX;
    uint32_t *slots = reuse_slots(r);
    size_t at = reuse_hash(node, r->slots);
    while (slots[at]) {
        size_t i = slots[at] - 1;
        if (reuse_nodes(r)[i].node == node) return i;
        at = (at + 1) & (r->slots - 1);
    }
    return SIZE_MAX;
}
static void reuse_snapshot_free(FReuseSnapshot *r)
{
    if (!r) return;
    os64_html_reader_free(r->reader);
    os64_free(r);
}
static const os64_html_node_t *reuse_next(const os64_html_node_t *node)
{
    if (node->first_child) return node->first_child;
    while (node && !node->next) node = node->parent;
    return node ? node->next : NULL;
}
static void reuse_mark(FReuseSnapshot *r, const os64_html_node_t *node)
{
    size_t i = reuse_find(r, node);
    if (i != SIZE_MAX) reuse_nodes(r)[i].changed = reuse_nodes(r)[i].dirty = true;
}
static bool reuse_read(FReuseSnapshot *r, flow_reuse_stats_t *stats)
{
    if (r->lost) return false;
    const os64_html_change_entry_t *entry;
    if (os64_html_changes(r->reader, &entry) < 0) {
        r->lost = true;
        // A disqualified slow reader must stop retaining history immediately.
        os64_html_reader_free(r->reader); r->reader = NULL;
        return false;
    }
    for (; entry; entry = entry->next) {
        reuse_mark(r, entry->target);
        for (size_t i = 0; i < entry->nadded; ++i) reuse_mark(r, entry->added[i]);
        for (size_t i = 0; i < entry->nremoved; ++i) reuse_mark(r, entry->removed[i]);
    }
    // Preorder index: the saved parent precedes its children, even if current
    // DOM links have moved those children elsewhere or disconnected them.
    FReuseNode *nodes = reuse_nodes(r);
    for (size_t i = r->count; i-- > 0;) {
        if (nodes[i].dirty && nodes[i].parent != SIZE_MAX)
            nodes[nodes[i].parent].dirty = true;
        stats->dirty_nodes += nodes[i].changed;
        stats->dirty_subtrees += nodes[i].dirty;
    }
    return true;
}
static bool reuse_inputs_equal(const flow_reuse_inputs_t *a, const flow_reuse_inputs_t *b)
{
    return a->resource_owner && a->resource_owner == b->resource_owner &&
           a->fonts_revision == b->fonts_revision && a->replaced_revision == b->replaced_revision &&
           a->state_owner == b->state_owner && a->state_version == b->state_version;
}
static bool reuse_environment_equal(const flow_env_t *a, const flow_env_t *b)
{
    // ctx may name a temporary Page copy. The explicit resource owner/revisions
    // certify the resolver's answers across those copies; pointers alone cannot.
    return a->text == b->text && a->fonts == b->fonts && a->replaced_size == b->replaced_size &&
           a->viewport_font_px == b->viewport_font_px && a->default_generic == b->default_generic &&
           a->ink == b->ink && a->link_ink == b->link_ink && a->paper == b->paper &&
           a->max_arena_bytes == b->max_arena_bytes && a->viewport_height == b->viewport_height &&
           a->zoom == b->zoom && a->static_only == b->static_only && a->scripting == b->scripting &&
           a->svg == b->svg && a->ticks == b->ticks;
}
static void reuse_source_prepare(flow_tree_t *tree, const flow_tree_t *previous,
                                 int32_t width, const flow_env_t *env,
                                 const flow_reuse_inputs_t *inputs)
{
    flow_reuse_stats_t *stats = &tree->reuse_stats;
    if (!inputs->resource_owner) { stats->refused[FLOW_REUSE_INPUTS_CHANGED]++; return; }
    if (previous && previous->incomplete) { stats->refused[FLOW_REUSE_INCOMPLETE]++; return; }
    if (!previous || previous->doc != tree->doc || !previous->reuse) {
        stats->refused[FLOW_REUSE_NO_SOURCE]++; return;
    }
    FReuseSnapshot *r = previous->reuse;
    // Drain the source independently of an environment miss, so a long-lived
    // displayed tree does not lag unnecessarily behind hidden measurements.
    if (!reuse_read(r, stats)) { stats->refused[FLOW_REUSE_HISTORY_LOST]++; return; }
    if (r->width != width || r->doc != tree->doc ||
        r->generation != garb_cascade_generation(env->cascade) ||
        tree->doc->quirks == OS64_HTML_QUIRKS || !reuse_inputs_equal(&r->inputs, inputs) ||
        !reuse_environment_equal(&r->env, env)) {
        stats->refused[FLOW_REUSE_INPUTS_CHANGED]++; return;
    }
    stats->source_ready = true;
}
// Include atomic content in the identity inventory. A node with more than one
// box cannot select a source by identity alone; ordered anonymous children are
// matched separately. The box builder bounds this recursion by F_DEPTH_MAX.
static void reuse_index_boxes(FReuseSnapshot *r, const FBox *b)
{
    if (!b) return;
    size_t i = reuse_find(r, b->node);
    if (i != SIZE_MAX) {
        FReuseNode *n = &reuse_nodes(r)[i];
        if (n->box) n->ambiguous = true;
        else n->box = b;
    }
    for (const FBox *c = b->first; c; c = c->next) reuse_index_boxes(r, c);
    for (const FItem *it = b->items; it; it = it->next)
        if (it->content) reuse_index_boxes(r, it->content);
}
static FReuseSnapshot *reuse_snapshot_new(flow_tree_t *tree, int32_t width,
                                         const flow_env_t *env, const flow_reuse_inputs_t *inputs)
{
    flow_reuse_stats_t *stats = &tree->reuse_stats;
    if (!inputs->resource_owner) return NULL;
    if (tree->boxes->incomplete || tree->doc->refusal ||
        (tree->model && os64_page_incomplete(tree->model))) {
        stats->refused[FLOW_REUSE_INCOMPLETE]++; return NULL;
    }
    size_t cap = f_arena_budget(env), count = 0;
    if (cap < sizeof(FReuseSnapshot)) goto budget;
    for (const os64_html_node_t *n = tree->doc->document; n; n = reuse_next(n)) {
        if (count >= (cap - sizeof(FReuseSnapshot)) / sizeof(FReuseNode) || count >= UINT32_MAX / 2)
            goto budget;
        ++count;
    }
    size_t slots = 2;
    while (slots < count * 2) slots *= 2;
    size_t bytes = sizeof(FReuseSnapshot) + count * sizeof(FReuseNode);
    if (slots > (cap - bytes) / sizeof(uint32_t)) goto budget;
    bytes += slots * sizeof(uint32_t);
    FReuseSnapshot *r = os64_calloc(1, bytes);
    if (!r) { stats->refused[FLOW_REUSE_NO_MEMORY]++; return NULL; }
    *r = (FReuseSnapshot){.inputs = *inputs, .env = *env, .doc = tree->doc,
        .generation = tree->styles->cascade_generation, .version = os64_html_version(tree->doc),
        .count = count, .slots = slots, .bytes = bytes, .width = width};
    FReuseNode *nodes = reuse_nodes(r);
    uint32_t *hash = reuse_slots(r);
    size_t i = 0;
    for (const os64_html_node_t *n = tree->doc->document; n; n = reuse_next(n), ++i) {
        nodes[i] = (FReuseNode){.node = n, .parent = reuse_find(r, n->parent)};
        const FStyled *st = f_style_of(tree->styles, n);
        nodes[i].animated = (nodes[i].parent != SIZE_MAX && nodes[nodes[i].parent].animated) ||
                           (st && (st->motion || st->style.animations.nname || st->style.animating));
        size_t at = reuse_hash(n, slots);
        while (hash[at]) at = (at + 1) & (slots - 1);
        hash[at] = (uint32_t)i + 1;
    }
    reuse_index_boxes(r, tree->boxes->root);
    stats->nodes_indexed = count;
    return r;
budget:
    stats->refused[FLOW_REUSE_BUDGET]++;
    return NULL;
}
static void reuse_snapshot_finish(flow_tree_t *tree)
{
    FReuseSnapshot *r = tree->reuse;
    if (!r) return;
    if (tree->incomplete) {
        tree->reuse_stats.refused[FLOW_REUSE_INCOMPLETE]++;
        goto discard;
    }
    r->reader = os64_html_reader_new((os64_html_document_t *)tree->doc, r->version);
    if (r->reader) return;
    tree->reuse_stats.refused[FLOW_REUSE_NO_MEMORY]++;
discard:
    reuse_snapshot_free(r); tree->reuse = NULL;
    tree->reuse_stats.nodes_indexed = 0;
}
