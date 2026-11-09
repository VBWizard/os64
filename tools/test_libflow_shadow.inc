// SHADOW_DOM.md § 5: rendering walks the flat tree. The trees are made with
// libhtml's verbs (no markup makes a shadow root), and these cases'
// cascades take no scopes, so no author sheet reaches a shadow tree: they
// ask what is laid out, not how it is styled (§ 6 is libgarb's suite's).

static os64_html_node_t *shadow_el(os64_html_document_t *doc, os64_html_node_t *parent, const char *tag,
                                   const char *slot, const char *text)
{
    os64_html_node_t *n = os64_html_create_element(doc, OS64_HTML_NS_HTML, tag, NULL);
    if (n == NULL)
        return NULL;
    if (slot != NULL)
        os64_html_set_attr(doc, n, tag[0] == 's' && tag[1] == 'l' ? "name" : "slot", slot, strlen(slot));
    if (text != NULL)
        os64_html_insert(doc, n, os64_html_create_text(doc, text, strlen(text), NULL), NULL);
    os64_html_insert(doc, parent, n, NULL);
    return n;
}

static void shadow_flow_cases(void)
{
    // A host whose light children go to two slots, one to none; a slot
    // with nodes assigned keeps its fallback out, an empty one draws its.
    Sheeted p;
    snapshot_open(&p, "<!doctype html><body><div id=h></div><p id=after>after</p>");
    os64_html_node_t *host = (os64_html_node_t *)computed_find(p.doc->document, "h");
    os64_html_node_t *root = host != NULL ? os64_html_attach_shadow(p.doc, host, OS64_HTML_SHADOW_OPEN, NULL) : NULL;
    expect("shadow: the fixture's root", root != NULL, NULL);
    if (root == NULL) {
        sheeted_close(&p);
        return;
    }
    os64_html_node_t *b = shadow_el(p.doc, host, "b", "a", "A");
    os64_html_node_t *i = shadow_el(p.doc, host, "i", NULL, "I");
    os64_html_node_t *u = shadow_el(p.doc, host, "u", "none", "U");
    os64_html_node_t *head = shadow_el(p.doc, root, "p", NULL, "head");
    shadow_el(p.doc, root, "slot", "a", NULL);
    os64_html_node_t *def = shadow_el(p.doc, root, "slot", NULL, NULL);
    os64_html_node_t *hidden_fallback = shadow_el(p.doc, def, "em", NULL, "fb");
    os64_html_node_t *empty = shadow_el(p.doc, root, "slot", "z", NULL);
    os64_html_node_t *shown_fallback = shadow_el(p.doc, empty, "strong", NULL, "shown");
    os64_html_node_t *loose = os64_html_create_text(p.doc, "rt", 2, NULL);
    os64_html_insert(p.doc, root, loose, NULL);

    flow_reuse_inputs_t input = {.resource_owner = p.doc, .state_owner = p.page};
    flow_tree_t *old = snapshot_layout(&p, NULL, &input);
    const flow_box_t *hb = flow_box_for(old, head), *bb = flow_box_for(old, b), *ib = flow_box_for(old, i);
    expect("shadow: a shadow tree lays out", hb != NULL, NULL);
    expect("shadow: a host's children render where their slots are, after the content before them",
           bb != NULL && ib != NULL && hb != NULL && bb->rect.y > hb->rect.y && ib->rect.x > bb->rect.x, NULL);
    expect("shadow: a host's child no slot takes renders nothing", flow_box_for(old, u) == NULL, NULL);
    expect("shadow: an assigned slot's fallback renders nothing", flow_box_for(old, hidden_fallback) == NULL, NULL);
    expect("shadow: an empty slot's fallback renders", flow_box_for(old, shown_fallback) != NULL, NULL);
    expect("shadow: text straight in a root renders", flow_box_for(old, loose) != NULL, NULL);
    snapshot_equal(&p, old);

    // The slot rule: moving a child to another slot, or out of every one,
    // changes the host's flat children; a relayout from the old tree must
    // equal a whole one.
    os64_html_set_attr(p.doc, i, "slot", "a", 1);
    flow_tree_t *moved = snapshot_layout(&p, old, &input);
    expect("shadow: a child moved to another slot is laid out there", moved != NULL &&
           flow_box_for(moved, i) != NULL && flow_box_for(moved, shown_fallback) != NULL, NULL);
    // The rule's own effect: the host and all it laid out are marked, not
    // just the child whose attribute changed. The output check beside it
    // is what makes the rule safe to refine later.
    flow_reuse_stats_t ms = moved != NULL ? flow_reuse_stats(moved) : (flow_reuse_stats_t){0};
    expect("shadow: a slot move marks the host's whole saved flat subtree", ms.source_ready && ms.dirty_nodes > 5,
           NULL);
    snapshot_equal(&p, moved);
    os64_html_set_attr(p.doc, u, "slot", "z", 1);
    flow_tree_t *filled = snapshot_layout(&p, moved, &input);
    expect("shadow: a slot that takes a child stops drawing its fallback", filled != NULL &&
           flow_box_for(filled, u) != NULL && flow_box_for(filled, shown_fallback) == NULL, NULL);
    snapshot_equal(&p, filled);
    os64_html_set_attr(p.doc, b, "slot", "gone", 4);
    flow_tree_t *dropped = snapshot_layout(&p, filled, &input);
    expect("shadow: a child no slot takes any more stops rendering", dropped != NULL &&
           flow_box_for(dropped, b) == NULL, NULL);
    snapshot_equal(&p, dropped);
    flow_free(old);
    flow_free(moved);
    flow_free(filled);
    flow_free(dropped);
    sheeted_close(&p);

    // Table parts assigned to a slot: the anonymous table's range is the
    // flat siblings', which a host child's real next sibling (white space
    // no slot takes, a child for another slot) is not.
    snapshot_open(&p, "<!doctype html><body><div id=th></div>");
    host = (os64_html_node_t *)computed_find(p.doc->document, "th");
    root = host != NULL ? os64_html_attach_shadow(p.doc, host, OS64_HTML_SHADOW_OPEN, NULL) : NULL;
    if (root != NULL) {
        os64_html_node_t *cell = shadow_el(p.doc, host, "td", "t", "cellay");
        os64_html_insert(p.doc, host, os64_html_create_text(p.doc, "\n", 1, NULL), NULL);
        shadow_el(p.doc, host, "span", "x", "S");
        shadow_el(p.doc, host, "td", "t", "cellcee");
        shadow_el(p.doc, root, "slot", "t", NULL);
        shadow_el(p.doc, root, "slot", "x", NULL);
        flow_tree_t *t = snapshot_layout(&p, NULL, &input);
        expect("shadow: a slotted table cell lays out in an anonymous table", t != NULL && flow_box_for(t, cell) != NULL,
               NULL);
        // Each cell's range ends at its flat sibling, so a cell after a
        // child for another slot is laid out once, not again inside the
        // range of the cell before it.
        int64_t n = t != NULL ? flow_dump(t, NULL, 0) : 0;
        char *dump = n > 0 ? malloc((size_t)n + 1) : NULL;
        int seen = 0;
        if (dump != NULL) {
            flow_dump(t, dump, (size_t)n + 1);
            for (const char *at = strstr(dump, "cellcee"); at != NULL; at = strstr(at + 1, "cellcee"))
                seen++;
        }
        expect("shadow: a slotted cell after another slot's child is laid out once", seen == 1, NULL);
        free(dump);
        flow_free(t);
    }
    sheeted_close(&p);

    // A host that is another host's child, moved between that host's slots:
    // the host round it is marked too (the rule's three hosts).
    snapshot_open(&p, "<!doctype html><body><div id=outer></div>");
    host = (os64_html_node_t *)computed_find(p.doc->document, "outer");
    root = host != NULL ? os64_html_attach_shadow(p.doc, host, OS64_HTML_SHADOW_OPEN, NULL) : NULL;
    if (root != NULL) {
        shadow_el(p.doc, root, "p", NULL, "first");
        shadow_el(p.doc, root, "slot", "a", NULL);
        shadow_el(p.doc, root, "p", NULL, "between");
        shadow_el(p.doc, root, "slot", "b", NULL);
        os64_html_node_t *inner = shadow_el(p.doc, host, "div", "a", NULL);
        os64_html_node_t *ir = inner != NULL ? os64_html_attach_shadow(p.doc, inner, OS64_HTML_SHADOW_OPEN, NULL) : NULL;
        if (ir != NULL) shadow_el(p.doc, ir, "span", NULL, "inside");
        flow_tree_t *was = snapshot_layout(&p, NULL, &input);
        os64_html_set_attr(p.doc, inner, "slot", "b", 1);
        flow_tree_t *now = snapshot_layout(&p, was, &input);
        flow_reuse_stats_t ns = now != NULL ? flow_reuse_stats(now) : (flow_reuse_stats_t){0};
        expect("shadow: a nested host's move marks the host round it", ns.source_ready && ns.dirty_nodes > 6, NULL);
        snapshot_equal(&p, now);
        flow_free(was);
        flow_free(now);
    }
    sheeted_close(&p);

    // A table host whose shadow tree holds a header row and a slot taking
    // light-DOM rows: the slot's rows are the table's own, so their columns
    // line up with the header's (Codex #486).
    snapshot_open(&p, "<!doctype html><body><div id=tbl></div>");
    host = (os64_html_node_t *)computed_find(p.doc->document, "tbl");
    root = host != NULL ? os64_html_attach_shadow(p.doc, host, OS64_HTML_SHADOW_OPEN, NULL) : NULL;
    if (root != NULL) {
        os64_html_node_t *table = shadow_el(p.doc, root, "table", NULL, NULL);
        os64_html_node_t *head = shadow_el(p.doc, table, "tr", NULL, NULL);
        shadow_el(p.doc, head, "td", NULL, "wide-header-cell");
        os64_html_node_t *h2 = shadow_el(p.doc, head, "td", NULL, "h");
        shadow_el(p.doc, table, "slot", NULL, NULL);
        os64_html_node_t *row = shadow_el(p.doc, host, "tr", NULL, NULL);
        shadow_el(p.doc, row, "td", NULL, "a");
        os64_html_node_t *r2 = shadow_el(p.doc, row, "td", NULL, "b");
        flow_tree_t *t = snapshot_layout(&p, NULL, &input);
        const flow_box_t *hb2 = t != NULL ? flow_box_for(t, h2) : NULL, *rb2 = t != NULL ? flow_box_for(t, r2) : NULL;
        expect("shadow: a slot's rows share the table's columns", hb2 != NULL && rb2 != NULL && hb2->rect.x == rb2->rect.x,
               NULL);
        flow_free(t);
    }
    sheeted_close(&p);

    // The chapter's descendant selectors match within one tree: a list in a
    // component inside a list is a first-level list (a disc), not `ul ul`.
    snapshot_open(&p, "<!doctype html><body><ul><li><div id=nav></div></li></ul>");
    host = (os64_html_node_t *)computed_find(p.doc->document, "nav");
    root = host != NULL ? os64_html_attach_shadow(p.doc, host, OS64_HTML_SHADOW_OPEN, NULL) : NULL;
    if (root != NULL) {
        os64_html_node_t *list = shadow_el(p.doc, root, "ul", NULL, NULL);
        shadow_el(p.doc, list, "li", NULL, "item");
        flow_tree_t *t = snapshot_layout(&p, NULL, &input);
        const flow_box_t *lb = t != NULL ? flow_box_for(t, list) : NULL;
        expect("shadow: `ul ul` does not reach into a shadow tree",
               lb != NULL && lb->style->list_style_type == FLOW_LIST_DISC, NULL);
        flow_free(t);
    }
    sheeted_close(&p);
}
