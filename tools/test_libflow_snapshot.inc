// Snapshot instrumentation must not change a layout's answer. Use no cascade
// for history tests so the snapshot reader's lifetime can be observed alone.
static void snapshot_open(Sheeted *p, const char *html)
{
    sheeted_open(p, html, 800, 600);
    garb_cascade_free(p->cascade); p->cascade = NULL; p->env.cascade = NULL;
}
static flow_tree_t *snapshot_layout(Sheeted *p, const flow_tree_t *old,
                                   const flow_reuse_inputs_t *inputs)
{
    os64_page_t *model = os64_page_rebuild(p->page);
    flow_tree_t *tree = flow_layout_incremental(p->doc, model, 800, &p->env, old, inputs,
                                               NULL, NULL, NULL);
    os64_page_free(model);
    return tree;
}
static void snapshot_equal(Sheeted *p, flow_tree_t *tree)
{
    os64_page_t *model = os64_page_rebuild(p->page);
    flow_tree_t *whole = flow_layout(p->doc, model, 800, &p->env);
    expect("snapshot: complete comparison trees", tree && whole &&
           !flow_incomplete(tree) && !flow_incomplete(whole), NULL);
    if (tree && whole) {
        int64_t n = flow_dump(tree, NULL, 0), m = flow_dump(whole, NULL, 0);
        char *a = malloc((size_t)n + 1), *b = malloc((size_t)m + 1);
        flow_dump(tree, a, (size_t)n + 1); flow_dump(whole, b, (size_t)m + 1);
        expect("snapshot: public output equals independent whole layout", n == m && !strcmp(a, b), NULL);
        free(a); free(b);
    }
    flow_free(whole); os64_page_free(model);
}
static void snapshot_cases(void)
{
    Sheeted p;
    snapshot_open(&p, "<!doctype html><div id=left><p id=a>first</p></div>"
                     "<div id=right><p id=b>second</p></div>");
    flow_reuse_inputs_t input = {.resource_owner = p.doc, .state_owner = p.page};
    flow_tree_t *old = snapshot_layout(&p, NULL, &input);
    flow_reuse_stats_t stats = flow_reuse_stats(old);
    expect("snapshot: first layout captures bounded membership", old && stats.nodes_indexed > 8 &&
           stats.refused[FLOW_REUSE_NO_SOURCE] == 1 && !stats.source_ready, NULL);
    os64_html_node_t *left = (os64_html_node_t *)computed_find(p.doc->document, "left");
    os64_html_node_t *right = (os64_html_node_t *)computed_find(p.doc->document, "right");
    os64_html_node_t *a = (os64_html_node_t *)computed_find(p.doc->document, "a");
    os64_html_reader_t *other = os64_html_reader_new(p.doc, os64_html_version(p.doc));
    expect("snapshot: mutation applied", os64_html_set_attr(p.doc, left, "title", "one", 3) == OS64_HTML_OK, NULL);
    flow_tree_t *first = snapshot_layout(&p, old, &input);
    stats = flow_reuse_stats(first);
    uint64_t first_dirty = stats.dirty_subtrees;
    expect("snapshot: first read accumulates target and saved ancestors", stats.source_ready &&
           stats.dirty_nodes == 1 && first_dirty >= 3, NULL);
    const os64_html_change_entry_t *entries;
    expect("snapshot: independent reader still sees mutation", os64_html_changes(other, &entries) > 0 &&
           entries && entries->target == left, NULL);
    os64_html_reader_free(other);
    os64_html_set_attr(p.doc, right, "title", "two", 3);
    flow_tree_t *second = snapshot_layout(&p, old, &input);
    stats = flow_reuse_stats(second);
    expect("snapshot: older displayed source remembers earlier read", stats.source_ready &&
           stats.dirty_nodes == 2 && stats.dirty_subtrees > first_dirty, NULL);
    flow_tree_t *same = snapshot_layout(&p, old, &input);
    flow_reuse_stats_t again = flow_reuse_stats(same);
    expect("snapshot: no mutation does not reset cumulative dirt", again.dirty_nodes == stats.dirty_nodes &&
           again.dirty_subtrees == stats.dirty_subtrees, NULL);
    flow_free(same);
    os64_html_insert(p.doc, right, a, NULL);
    flow_tree_t *moved = snapshot_layout(&p, old, &input);
    stats = flow_reuse_stats(moved);
    expect("snapshot: moved node dirties saved membership", stats.source_ready && stats.dirty_nodes == 3, NULL);
    flow_free(first); flow_free(second); flow_free(moved);
    flow_tree_t *next = snapshot_layout(&p, old, &input);
    flow_free(old); old = next;
    snapshot_equal(&p, old); // Public data must outlive the source snapshot.

    size_t held = live, points = 0;
    for (int mode = 0; mode < 2; ++mode) {
        for (size_t at = 0; at <= points; ++at) {
            size_t before = allocations;
            fail_single = mode != 0; fail_at = at ? before + at : 0;
            flow_tree_t *trial = snapshot_layout(&p, old, &input);
            fail_at = 0; fail_single = false;
            if (!at) points = allocations - before;
            flow_free(trial);
            expect("snapshot: failed preparation/layout releases ownership", live == held, NULL);
        }
    }
    printf("libflow snapshots: %zu allocation positions, single and persistent failures\n", points);
    next = snapshot_layout(&p, old, &input);
    expect("snapshot: source survives allocation sweeps", flow_reuse_stats(next).source_ready, NULL);
    flow_free(next);
    flow_reuse_inputs_t changed = input; changed.fonts_revision++;
    next = snapshot_layout(&p, old, &changed);
    expect("snapshot: changed fonts refuse old snapshot", !flow_reuse_stats(next).source_ready &&
           flow_reuse_stats(next).refused[FLOW_REUSE_INPUTS_CHANGED], NULL);
    flow_free(next); changed = input; changed.replaced_revision++;
    next = snapshot_layout(&p, old, &changed);
    expect("snapshot: changed intrinsic resources refuse old snapshot",
           flow_reuse_stats(next).refused[FLOW_REUSE_INPUTS_CHANGED], NULL);
    flow_free(next); changed = input; changed.state_version++;
    next = snapshot_layout(&p, old, &changed);
    expect("snapshot: changed form state refuses old snapshot",
           flow_reuse_stats(next).refused[FLOW_REUSE_INPUTS_CHANGED], NULL);
    flow_free(next); changed = input; changed.resource_owner = &p;
    next = snapshot_layout(&p, old, &changed);
    expect("snapshot: owner identity prevents revision collisions",
           flow_reuse_stats(next).refused[FLOW_REUSE_INPUTS_CHANGED], NULL);
    flow_free(next);
    p.env.now_ms++;
    next = snapshot_layout(&p, old, &input);
    expect("snapshot: clock passage alone does not lose history readiness", flow_reuse_stats(next).source_ready, NULL);
    flow_free(next);
    os64_html_changes_limit(p.doc, 0);
    os64_html_set_attr(p.doc, left, "title", "lost", 4);
    next = snapshot_layout(&p, old, &input);
    expect("snapshot: loss refuses source", flow_reuse_stats(next).refused[FLOW_REUSE_HISTORY_LOST], NULL);
    flow_free(next);
    os64_html_changes_limit(p.doc, 65536);
    os64_html_set_attr(p.doc, left, "title", "after", 5);
    expect("snapshot: disqualified reader no longer retains log", os64_html_changes_bytes(p.doc) == 0, NULL);
    next = snapshot_layout(&p, old, &input);
    expect("snapshot: restoring log budget cannot rehabilitate old snapshot",
           flow_reuse_stats(next).refused[FLOW_REUSE_HISTORY_LOST], NULL);
    flow_free(old); snapshot_equal(&p, next); flow_free(next);
    sheeted_close(&p);

    char *html = malloc(50000); strcpy(html, "<!doctype html><div hidden>");
    for (int i = 0; i < 2000; ++i) strcat(html, "<span>x</span>");
    strcat(html, "</div><p>visible</p>"); snapshot_open(&p, html); free(html);
    p.env.max_arena_bytes = 65536;
    input = (flow_reuse_inputs_t){.resource_owner = p.doc};
    old = snapshot_layout(&p, NULL, &input);
    expect("snapshot: metadata cap misses without losing a complete layout", old && !flow_incomplete(old) &&
           flow_reuse_stats(old).refused[FLOW_REUSE_BUDGET] && !flow_reuse_stats(old).nodes_indexed, NULL);
    snapshot_equal(&p, old); flow_free(old); sheeted_close(&p);
    expect("snapshot: null tree stats are empty", !flow_reuse_stats(NULL).source_ready &&
           !flow_reuse_stats(NULL).nodes_indexed, NULL);
}

// Fresh pass 2 is the oracle for generated bytes and anonymous structure.
// These assertions inspect eligibility before any geometry copying is enabled.
static void correspondence_cases(void)
{
    Sheeted p;
    sheeted_open(&p, "<!doctype html><style>.red{color:red}</style>"
        "<div id=parent><p id=a>first <b>bold</b></p><p id=b>second</p></div>", 800, 600);
    flow_reuse_inputs_t input = {.resource_owner = p.doc};
    flow_tree_t *old = snapshot_layout(&p, NULL, &input);
    flow_tree_t *next = snapshot_layout(&p, old, &input);
    flow_reuse_stats_t s = flow_reuse_stats(next);
    expect("match: ordinary inline content qualifies", s.geometry_candidates >= 4 &&
           s.subtrees_equal == s.boxes_compared, NULL);
    flow_free(next);
    os64_html_node_t *parent = (os64_html_node_t *)computed_find(p.doc->document, "parent");
    os64_html_node_t *a = (os64_html_node_t *)computed_find(p.doc->document, "a");
    os64_html_node_t *b = (os64_html_node_t *)computed_find(p.doc->document, "b");
    os64_html_set_text(p.doc, a->first_child, "longer first ", 13);
    expect("match: text mutation cascade updated", garb_cascade_update(p.cascade), NULL);
    next = snapshot_layout(&p, old, &input); s = flow_reuse_stats(next);
    expect("match: changed text refuses while untouched sibling qualifies",
           s.refused[FLOW_REUSE_DIRTY] && s.geometry_candidates == 1, NULL);
    flow_free(next);
    os64_html_set_text(p.doc, a->first_child, "first ", 6);
    garb_cascade_update(p.cascade);
    next = snapshot_layout(&p, old, &input); s = flow_reuse_stats(next);
    expect("match: restored text does not erase snapshot history",
           s.refused[FLOW_REUSE_DIRTY] && s.geometry_candidates == 1, NULL);
    flow_free(next);
    os64_html_set_attr(p.doc, parent, "class", "red", 3);
    garb_cascade_update(p.cascade);
    next = snapshot_layout(&p, old, &input); s = flow_reuse_stats(next);
    expect("match: inherited style change refuses untouched child",
           s.refused[FLOW_REUSE_STYLE] && !s.geometry_candidates, NULL);
    flow_free(old); snapshot_equal(&p, next); old = next;
    os64_html_insert(p.doc, parent, b, a); garb_cascade_update(p.cascade);
    next = snapshot_layout(&p, old, &input); s = flow_reuse_stats(next);
    expect("match: reorder refuses touched membership", s.refused[FLOW_REUSE_DIRTY], NULL);
    flow_free(old); snapshot_equal(&p, next); flow_free(next); sheeted_close(&p);

    const char *cases[] = {
        "<!doctype html><div style='display:grid'><div><p>safe <b>bold</b></p></div></div>",
        "<!doctype html><div><span>before<div>block</div>after</span></div>",
        "<!doctype html><div style='animation:missing 1s'><p>animated ancestry</p></div>",
        "<!doctype html><ol id=list><li>one</li><li>two</li></ol>"
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        sheeted_open(&p, cases[i], 800, 600); input.resource_owner = p.doc;
        old = snapshot_layout(&p, NULL, &input);
        if (i == 3) {
            os64_html_node_t *list = (os64_html_node_t *)computed_find(p.doc->document, "list");
            os64_html_set_attr(p.doc, list, "start", "9", 1); garb_cascade_update(p.cascade);
        }
        next = snapshot_layout(&p, old, &input); s = flow_reuse_stats(next);
        if (i == 0) expect("match: ordinary descendant of grid item qualifies locally",
                            s.geometry_candidates == 1 && s.refused[FLOW_REUSE_BOX_ROLE], NULL);
        if (i == 1) expect("match: split inline excludes its enclosing subtree",
                            s.refused[FLOW_REUSE_BOX_ROLE] && s.geometry_candidates == 1, NULL);
        if (i == 2) expect("match: animation ancestry refuses matching geometry",
                            s.refused[FLOW_REUSE_MOTION] && !s.geometry_candidates, NULL);
        if (i == 3) expect("match: regenerated list marker bytes defeat unchanged child styles",
                            s.refused[FLOW_REUSE_STRUCTURE] && !s.geometry_candidates, NULL);
        flow_free(old); snapshot_equal(&p, next); flow_free(next); sheeted_close(&p);
    }
}
