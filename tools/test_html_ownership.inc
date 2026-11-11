/* Native lifetime groups: sibling snapshots delay reclamation, while the
 * refusal remains recoverable after the snapshot owner releases its pin. */
static void t_ownership_groups(void)
{
    os64_html_document_t *a = parse("<body><div>A</div>"), *b = parse("<body>"), *foreign = parse("<body>");
    check(a && b && foreign, "ownership fixtures parse");
    if (!a || !b || !foreign) return;
    os64_html_group_t *g = os64_html_group_create(2u * 1024u * 1024u);
    check(g != NULL, "lifetime group created");
    if (!g) return;
    os64_html_pin_t old = os64_html_pin(a);
    check(!os64_html_group_add(g, a, 3), "first arena retains existing snapshot");
    size_t charged = os64_html_group_bytes(g);
    check(os64_html_group_add(g, a, 4) == OS64_HTML_BAD_ARGUMENT &&
          os64_html_group_bytes(g) == charged, "duplicate admission changes no accounting");
    os64_html_hold(b, b->body);
    check(os64_html_group_add(g, b, 7) == OS64_HTML_BAD_ARGUMENT, "later arena with borrower refused");
    os64_html_release(b, b->body);
    check(!os64_html_group_add(g, b, 7), "second independent arena admitted");
    check(os64_html_group_bytes(g) == charged + b->arena_bytes, "arena charged once on admission");
    check(os64_html_node_creation_realm(a->body) == 3 && os64_html_node_creation_realm(b->body) == 7 &&
          os64_html_node_document(b->body) == b->document, "native node metadata distinct");
    os64_html_group_t *small = os64_html_group_create(sizeof(*g));
    check(small && os64_html_group_add(small, foreign, 1) == OS64_HTML_ARENA_EXHAUSTED,
          "aggregate admission refusal precedes transfer");
    os64_html_group_free(small);
    os64_html_document_free(foreign);

    size_t saved_limit = g->limit, admission_bytes = g->bytes;
    g->limit = admission_bytes + sizeof(HDoc);
    int64_t limited_status = 0;
    HNode *limited = os64_html_parse_fragment(b,b->body,"<p>cap</p>",10,false,&limited_status);
    check(!limited && limited_status == OS64_HTML_ARENA_EXHAUSTED && g->bytes == admission_bytes &&
          b->refusal == OS64_HTML_OK, "aggregate staging cap restores ledgers without sticky refusal");
    g->limit = saved_limit;
    ((HDoc *)b)->budget = 512u * 1024u;
    const char *markup = "<section><p>retained sibling payload</p><input value=x></section>";
    size_t before = b->arena_bytes, turns = 0;
    uint64_t av = os64_html_version(a);
    int64_t status = 0;
    for (; turns < 10000; turns++) {
        HNode *fragment = os64_html_parse_fragment(b, b->body, markup, strlen(markup), false, &status);
        if (!fragment) break;
        HNode *root = fragment->first_child;
        check(os64_html_node_creation_realm(root) == 7 && os64_html_node_document(root) == b->document,
              "staged nodes preserve destination realm and document");
        check(!os64_html_insert(b, b->body, fragment, NULL) && !os64_html_remove(b, root), "sibling churn");
    }
    size_t stalled = b->arena_bytes;
    check(turns > 0 && turns < 10000 && status == OS64_HTML_ARENA_EXHAUSTED,
          "old A pin drives B to recoverable local refusal");
    check(os64_html_version(a) == av && b->refusal == OS64_HTML_OK, "B churn leaves A version and parse verdict alone");
    check(stalled > before && os64_html_retired_bytes(b) > 0, "B's own retained bytes remain charged");
    check(os64_html_group_bytes(g) == sizeof(*g) + a->arena_bytes + b->arena_bytes, "staging refusal restores aggregate charge");
    os64_html_unpin(a, old);
    check(b->arena_bytes == before && !os64_html_retired_bytes(b), "A release reclaims B storage");
    HNode *fragment = os64_html_parse_fragment(b, b->body, markup, strlen(markup), false, &status);
    check(fragment && !status, "B proceeds after releasing sibling pin");
    if (fragment) {
        HNode *root = fragment->first_child;
        check(!os64_html_insert(b, b->body, fragment, NULL) && !os64_html_remove(b, root), "post-refusal operation commits");
    }
    for (size_t side = 0; side < 2; side++) {
        os64_html_document_t *owner = side ? b : a, *borrower = side ? a : b;
        HNode *text = os64_html_create_text(owner,"sibling-held",12,NULL);
        check(text && !os64_html_insert(owner,owner->body,text,NULL), "cross-pin mutable text fixture");
        os64_html_pin_t pin = os64_html_pin(borrower);
        const char *bytes = text->text;
        check(pin && !os64_html_set_text(owner,text,"replacement",11) && !memcmp(bytes,"sibling-held",13),
              "a pin in either arena preserves the sibling's replaced buffer");
        os64_html_unpin(borrower,pin);
        check(!os64_html_remove(owner,text), "cross-pin mutable text reclaimed");
    }
    printf("ownership stalled pin: %zu turns, B %zu -> %zu -> %zu bytes; group peak %zu\n",
           turns, before, stalled, b->arena_bytes, os64_html_group_peak(g));
    os64_html_group_free(g);
    check(live == 0, "lifetime group frees all physical ledgers");
}

static void t_ownership_adoption(void)
{
    os64_html_document_t *a = parse("<body><section><p>alpha</p></section>"),
        *b = parse("<body><aside>B</aside>"), *c = parse("<body>"), *foreign = parse("<body>");
    os64_html_group_t *g = os64_html_group_create(4u * 1024u * 1024u);
    check(a && b && c && foreign && g, "adoption fixtures");
    check(!os64_html_group_add(g, a, 1) && !os64_html_group_add(g, b, 2) &&
          !os64_html_group_add(g, c, 3), "three native realms admitted");
    HNode *container = a->body->first_child, *paragraph = container->first_child;
    HNode *text = paragraph->first_child;
    size_t remaining_a = a->node_count - 3, remaining_b = b->node_count, remaining_c = c->node_count;
    os64_html_hold(a, container);
    uint32_t identity = container->document_id;
    uint64_t av = os64_html_version(a), bv = os64_html_version(b);
    check(os64_html_insert(b, text, container, NULL) == OS64_HTML_BAD_ARGUMENT &&
          container->parent == a->body && os64_html_version(a) == av && os64_html_version(b) == bv,
          "preflight failure leaves source intact");
    check(os64_html_adopt(foreign, foreign->document, container) == OS64_HTML_BAD_ARGUMENT &&
          container->parent == a->body, "adoption rejects unrelated storage");
    int64_t depth_status;
    const char *deep_html = "<div><span><b>x</b></span></div>";
    HNode *deep = os64_html_parse_fragment(a, container, deep_html, strlen(deep_html), false, &depth_status);
    check(deep && !depth_status, "adoption depth fixture");
    HNode *deep_root = deep->first_child;
    check(!os64_html_insert(a,a->body,deep,NULL), "adoption depth source attached");
    deep = deep_root;
    os64_html_hold(a, deep);
    size_t depth = ((HDoc *)b)->max_depth;
    ((HDoc *)b)->max_depth = 0;
    check(os64_html_adopt(b,b->document,deep) == OS64_HTML_TOO_DEEP &&
          deep->parent == a->body && os64_html_node_document(deep) == a->document, "adoption depth refusal preserves source document");
    ((HDoc *)b)->max_depth = depth;
    check(!os64_html_remove(a,deep), "adoption depth fixture removed");
    os64_html_release(a, deep);
    check(!os64_html_insert(b, b->body, container, NULL), "insertion implicitly adopts");
    check(container->document_id == identity && h_meta(container)->allocation_owner == (HDoc *)a &&
          os64_html_node_creation_realm(container) == 1 && os64_html_node_document(text) == b->document &&
          !os64_html_owns_node(a, text) && os64_html_owns_node(b, text), "identity realm storage and document separate");
    os64_html_pin_t pa = os64_html_pin(a), pb = os64_html_pin(b);
    const char *old = text->text;
    check(!os64_html_set_text(b, text, "bravo", 5) && !strcmp(old, "alpha"), "cross-arena snapshot retains text");
    check(!os64_html_set_attr(b, container, "data-move", "B", 1), "adopted attribute allocates in destination");
    const char *html = "<article><input form=x><span>born in B</span></article>";
    int64_t status;
    HNode *fragment = os64_html_parse_fragment(b, container, html, strlen(html), false, &status);
    HNode *born_b = fragment ? fragment->first_child : NULL;
    check(fragment && !status && !os64_html_insert(b, container, fragment, NULL), "descendants created in B");
    check(!os64_html_insert(c, c->body, container, NULL), "mixed subtree re-adopted to C");
    check(os64_html_node_creation_realm(container) == 1 && os64_html_node_creation_realm(born_b) == 2 &&
          os64_html_node_document(container) == c->document && os64_html_node_document(born_b) == c->document,
          "three-realm unwrapped descendant proof");
    check(os64_html_form_input_count(b) == 0 && os64_html_form_input_count(c) == 1,
          "logical form counters migrate independently of arenas");
    check(!os64_html_set_text(c, text, "charlie", 7) && !os64_html_set_attr(c, container, "data-move", "C", 1),
          "re-adopted payload replacement frees through physical ledger");
    os64_html_unpin(b, pb);
    check(!strcmp(old, "alpha"), "A pin still protects bytes after B releases");
    os64_html_unpin(a, pa);
    check(!os64_html_adopt(b, b->document, container) && !container->parent, "explicit adoption leaves detached root");
    os64_html_range_t *range = os64_html_range_new(b);
    check(range && !os64_html_range_set(range, 0, text, 1) && !os64_html_range_set(range, 1, text, 3),
          "range points into detached subtree");
    check(!os64_html_adopt(c, c->document, container), "detached subtree adopted with live range");
    check(!os64_html_replace_data(c, text, 0, 2, "z", 1), "destination mutation updates migrated range");
    const HNode *boundary;
    uint32_t offset;
    os64_html_range_get(range, 1, &boundary, &offset);
    check(boundary == text && offset == 2, "range registry followed adoption");
    check(!os64_html_range_set(range, 0, b->body, 0), "range setter crosses group documents");
    os64_html_range_get(range, 1, &boundary, &offset);
    check(boundary == b->body && offset == 0, "cross-document range setter collapses other boundary");
    os64_html_range_free(range);
    os64_html_release(c, container);
    check(os64_html_form_input_count(c) == 0, "mixed-tree collection removes destination counts");
    check(a->node_count == remaining_a && b->node_count == remaining_b && c->node_count == remaining_c,
          "mixed-tree collection charges each physical node ledger");
    os64_html_document_free(foreign);
    os64_html_group_free(g);
    check(live == 0, "adoption ledgers and range allocation released");
}

static void ownership_slot_note(void *opaque, const HNode *slot)
{
    (void)slot;
    (*(size_t *)opaque)++;
}
static void t_ownership_templates(void)
{
    os64_html_document_t *a = parse("<body><template><div><template><i>nested</i></template></div></template>"),
        *b = parse("<body>");
    os64_html_group_t *g = os64_html_group_create(4u * 1024u * 1024u);
    check(a && b && g && !os64_html_group_add(g,a,4) && !os64_html_group_add(g,b,5), "template arenas");
    HNode *ia = os64_html_create_document(a,NULL), *ib = os64_html_create_document(b,NULL);
    check(ia && ib && !os64_html_document_templates(a,a->document,ia) &&
          !os64_html_document_templates(b,b->document,ib), "template documents configured");
    HNode *host = a->body->first_child, *contents = host->template_contents;
    HNode *div = contents->first_child, *nested = div->first_child;
    check(os64_html_node_document(host) == a->document && os64_html_node_document(div) == ia &&
          os64_html_node_document(nested->template_contents->first_child) == ia,
          "parsed template content uses associated inert document");
    os64_html_hold(a,host);
    check(!os64_html_insert(b,b->body,host,NULL), "template host adopted");
    check(os64_html_node_document(host) == b->document && os64_html_node_document(contents) == ib &&
          os64_html_node_document(nested->template_contents->first_child) == ib &&
          os64_html_node_creation_realm(div) == 4, "nested template adoption preserves creation realm");
    HNode *fragment = os64_html_parse_fragment(b,div,"<b>new</b>",10,false,NULL);
    HNode *fresh = fragment ? fragment->first_child : NULL;
    check(fresh && os64_html_node_document(fresh) == ib && os64_html_node_creation_realm(fresh) == 5,
          "fragment in adopted template uses destination document and realm");
    check(!os64_html_insert(b,div,fragment,NULL), "inert fragment inserted");
    HNode *held_fragment = os64_html_create_fragment(b,NULL);
    os64_html_hold(b,held_fragment);
    check(!os64_html_insert(b,held_fragment,os64_html_create_text(b,"fragment",8,NULL),NULL) &&
          !os64_html_insert(b,div,held_fragment,NULL) && os64_html_node_document(held_fragment) == ib,
          "consumed fragment keeps destination node document");
    os64_html_release(b,held_fragment);
    held_fragment = os64_html_create_fragment(b,NULL);
    os64_html_hold(b,held_fragment);
    check(!os64_html_insert(b,div,held_fragment,NULL) && os64_html_node_document(held_fragment) == ib,
          "empty fragment adopts into windowless document");
    os64_html_release(b,held_fragment);
    HNode *shadow_host = os64_html_create_element(b,OS64_HTML_NS_HTML,"div",NULL);
    HNode *shadow = os64_html_attach_shadow(b,shadow_host,OS64_HTML_SHADOW_OPEN,NULL);
    HNode *child = os64_html_create_element(b,OS64_HTML_NS_HTML,"slot",NULL);
    check(shadow && child && !os64_html_insert(b,shadow,child,NULL) &&
          !os64_html_insert(b,b->body,shadow_host,NULL), "shadow adoption fixture");
    check(os64_html_adopt(a,a->document,shadow) == OS64_HTML_HIERARCHY &&
          os64_html_node_document(shadow) == b->document, "independent shadow adoption refused");
    check(!os64_html_insert(a,a->body,shadow_host,NULL) && os64_html_node_document(child) == a->document &&
          os64_html_node_creation_realm(child) == 5, "shadow descendants follow host document");
    HNode *light = os64_html_create_text(a,"assigned",8,NULL);
    check(light && !os64_html_insert(a,shadow_host,light,NULL), "adopted shadow assignment");
    ((HDoc *)a)->slot_dirty_all = true;
    ((HDoc *)a)->slot_dirty_n = 0;
    size_t notices = 0;
    check(!os64_html_slot_changes(a,ownership_slot_note,&notices) && notices == 1,
          "lost shadow marks recover across physical arenas");
    HNode *inert = os64_html_create_document(a,NULL);
    check(inert && !os64_html_adopt(a,inert,host) && os64_html_node_document(host) == inert,
          "windowless document is an explicit native owner");
    HNode *inert_host = os64_html_create_element(a,OS64_HTML_NS_HTML,"div",NULL);
    check(inert_host && !os64_html_adopt(a,inert,inert_host), "windowless shadow host adoption");
    HNode *inert_shadow = os64_html_attach_shadow(a,inert_host,OS64_HTML_SHADOW_OPEN,NULL);
    check(inert_shadow && os64_html_node_document(inert_shadow) == inert &&
          os64_html_node_creation_realm(inert_shadow) == 4, "new shadow root uses host's current document");
    fragment = os64_html_parse_fragment(a,host,"<strong>windowless</strong>",27,false,NULL);
    check(fragment && os64_html_node_document(fragment->first_child) == inert,
          "windowless fragment records document before wrapping");
    check(!os64_html_insert(a,host,fragment,NULL), "windowless fragment insertion");
    int64_t xml_status = 0;
    HDoc *xml = d_xml_document_new(1024u * 1024u,128,&xml_status);
    check(xml && !os64_html_group_add(g,&xml->pub,6), "XML arena admitted");
    HNode *xml_node = os64_html_create_element_ns(&xml->pub,"http://www.w3.org/1999/xhtml",NULL,"input",NULL);
    check(xml_node && !os64_html_insert(&xml->pub,xml->pub.document,xml_node,NULL) && xml_node->xml_document,
          "XML element starts in XML document");
    check(!os64_html_insert(b,b->body,xml_node,NULL) && !xml_node->xml_document &&
          os64_html_node_creation_realm(xml_node) == 6, "XML to HTML adoption updates document rules");
    check(!os64_html_insert(&xml->pub,xml->pub.document,xml_node,NULL) && xml_node->xml_document,
          "HTML to XML adoption restores XML document rules");
    os64_html_release(b,host);
    os64_html_group_free(g);
    check(live == 0, "template shadow and windowless teardown");
}
