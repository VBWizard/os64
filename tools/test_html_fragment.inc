/* Fragment expectations are hand trees or pinned upstream fixtures. Buffer
 * checks compare the public length/prefix contract, not serializer internals. */
static void fragment_safety(const unsigned char *data, size_t len, const char *name, bool scripting,
                            uint64_t budget, bool sweep);
static size_t fragment_cases, fragment_failures, fragment_work_cuts;
static HNode *fragment_context(os64_html_document_t *doc, const char *context)
{
    os64_html_ns_t ns = OS64_HTML_NS_HTML;
    if (!strncmp(context, "svg ", 4)) {
        ns = OS64_HTML_NS_SVG;
        context += 4;
    } else if (!strncmp(context, "math ", 5)) {
        ns = OS64_HTML_NS_MATHML;
        context += 5;
    }
    int64_t why = 0;
    HNode *n = os64_html_create_element(doc, ns, context, &why);
    if (!n || why)
        safety_fail("fragment context allocation");
    return n;
}
static os64_html_document_t *fragment_document(void)
{
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "utf-8";
    os64_html_document_t *doc = parse_raw((const unsigned char *)"<!doctype html>", 15, 0, &opt);
    if (!doc || doc->refusal)
        safety_fail("fragment host document");
    return doc;
}
static char *fragment_dump(os64_html_document_t *doc, HNode *fragment)
{
    char *dump = NULL;
    size_t len = 0, visited = 0;
    FILE *out = open_memstream(&dump, &len);
    if (!out)
        exit(2);
    dump_nodes(out, fragment->first_child, 0, &visited, doc->node_count);
    fclose(out);
    if (len && dump[len - 1] == '\n')
        dump[len - 1] = 0;
    return dump;
}
static char *serialization_check(const HNode *node, bool children, bool scripting)
{
    size_t allocation_count = allocations;
    size_t len = os64_html_serialize(node, children, scripting, NULL, 0);
    char *full = malloc(len + 1);
    if (!full)
        exit(2);
    memset(full, 0xa5, len + 1);
    if (os64_html_serialize(node, children, scripting, full, len + 1) != len ||
        full[len] || strlen(full) != len)
        safety_fail("serializer length/NUL");
    valid_utf8(full);
    size_t caps[] = {0, 1, len, len + 1, len + 9};
    for (size_t i = 0; i < H_ARRAY(caps); i++) {
        size_t cap = caps[i];
        unsigned char *small = malloc(cap + 8);
        if (!small)
            exit(2);
        memset(small, 0xa5, cap + 8);
        if (os64_html_serialize(node, children, scripting, (char *)small, cap) != len)
            safety_fail("serializer capped length");
        for (size_t j = cap; j < cap + 8; j++)
            if (small[j] != 0xa5)
                safety_fail("serializer cap overrun");
        if (cap) {
            size_t take = len < cap - 1 ? len : cap - 1;
            if (small[take] || memcmp(small, full, take))
                safety_fail("serializer capped prefix/NUL");
        }
        free(small);
    }
    if (allocations != allocation_count)
        safety_fail("serializer allocated");
    return full;
}
static void fragment_invariants(os64_html_document_t *doc, HNode *context, HNode *fragment)
{
    known_reset();
    known_add(context);
    known_add(fragment);
    everything_holds(doc);
    known_reset();
}
static void fragment_hand(const char *context_name, const char *input, bool scripting,
                          const char *want)
{
    os64_html_document_t *doc = fragment_document();
    HNode *context = fragment_context(doc, context_name);
    uint64_t version = os64_html_version(doc);
    int64_t why = INT64_MAX;
    HNode *fragment = os64_html_parse_fragment(doc, context, input, strlen(input), scripting, &why);
    check(fragment && why == OS64_HTML_OK, "fragment %s accepted (%lld)", context_name,
          (long long)why);
    if (fragment) {
        check(fragment->kind == OS64_HTML_FRAGMENT && !fragment->parent &&
                  os64_html_owns_node(doc, fragment), "fragment detached ownership");
        fragment_invariants(doc, context, fragment);
        char *serialized = serialization_check(fragment, true, scripting);
        check(!strcmp(serialized, want), "fragment %s serialization wanted [%s] got [%s]",
              context_name, want, serialized);
        free(serialized);
    }
    check(os64_html_version(doc) == version && !doc->refusal, "fragment invisible to host");
    os64_html_document_free(doc);
    check(!live, "fragment hand leak");
}
static void serializer_parent(const char *name, bool scripting, const char *want)
{
    os64_html_document_t *doc = fragment_document();
    HNode *parent = fragment_context(doc, name);
    int64_t why = 0;
    HNode *text = os64_html_create_text(doc, "&\xc2\xa0<>\"", 6, &why);
    check(text && !why && !os64_html_insert(doc, parent, text, NULL), "serializer text fixture");
    char *out = serialization_check(parent, true, scripting);
    check(!strcmp(out, want), "serializer %s mode %d wanted [%s] got [%s]", name, scripting, want, out);
    free(out);
    os64_html_document_free(doc);
    check(!live, "serializer parent leak");
}
static void fragment_utf8_checks(void)
{
    static const struct { const char *context; const char *data; size_t len; const char *want; } cases[] = {
        {"div", "a\0b", 3, "ab"},
        {"textarea", "a\0b", 3, "a\xef\xbf\xbd" "b"},
        {"style", "a\0b", 3, "a\xef\xbf\xbd" "b"},
        {"script", "a\0b", 3, "a\xef\xbf\xbd" "b"},
        {"plaintext", "a\0b", 3, "a\xef\xbf\xbd" "b"},
        {"svg svg", "a\0b", 3, "a\xef\xbf\xbd" "b"},
        {"div", "<!--a\0b-->", 10, "<!--a\xef\xbf\xbd" "b-->"},
        {"div", "<p a='x\0y'>z", 12, "<p a=\"x\xef\xbf\xbd" "y\">z</p>"},
        {"div", "\xe2\x82", 2, "\xef\xbf\xbd"},
        {"div", "\xc0\xaf", 2, "\xef\xbf\xbd\xef\xbf\xbd"},
        {"div", "\xed\xa0\x80", 3, "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"},
        {"div", "\xf0\x9f\x98\x80", 4, "\xf0\x9f\x98\x80"}
    };
    for (size_t i = 0; i < H_ARRAY(cases); i++) {
        os64_html_document_t *doc = fragment_document();
        HNode *context = fragment_context(doc, cases[i].context);
        int64_t why = 0;
        HNode *fragment = os64_html_parse_fragment(doc, context, cases[i].data, cases[i].len, false, &why);
        check(fragment && !why, "fragment explicit UTF8 input %zu", i);
        if (fragment) {
            char *out = serialization_check(fragment, true, false);
            check(!strcmp(out, cases[i].want), "fragment UTF8 replacement %zu", i);
            free(out);
        }
        os64_html_document_free(doc);
        check(!live, "UTF8 fragment leak");
    }
}
static void fragment_lifetime_checks(void)
{
    const char *source = "<!doctype html><form id=f><div id=c a=old><input id=q></div></form>";
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "utf-8";
    os64_html_document_t *doc = parse_raw((const unsigned char *)source, strlen(source), 0, &opt);
    HNode *context = find_named(doc->document, "div"), *control = find_named(doc->document, "input"), *form = find_named(doc->document, "form");
    check(context && control && control->form_owner == form, "live form fixture");
    const HAttr *attr = os64_html_attr(context, "a");
    const char *old = attr->value;
    os64_html_pin_t pin = os64_html_pin(doc);
    uint64_t version = os64_html_version(doc);
    int64_t why = 0;
    const char *input = "<form><b a=old>x</b></form><input>";
    HNode *fragment = os64_html_parse_fragment(doc, context, input, strlen(input), false, &why);
    check(fragment && !why && !find_named(fragment->first_child, "form"), "ancestor form suppresses nested form start");
    check(os64_html_version(doc) == version && control->form_owner == form &&
              os64_html_attr(context, "a") == attr && !strcmp(old, "old"), "fragment preserves live owner/context/pin bytes");
    if (fragment) {
        HNode *b = find_named(fragment->first_child, "b");
        const char *value = os64_html_attr(b, "a")->value;
        HNode *clone = os64_html_clone(doc, b, true, &why);
        check(clone && !why, "fragment clone accepted");
        check(!os64_html_set_attr(doc, b, "a", "new", 3) && !strcmp(value, "old") &&
                  !strcmp(os64_html_attr(clone, "a")->value, "old"), "packed fragment attribute COW preserves snapshot/clone");
        check(!os64_html_set_attr(doc, clone, "b", "fresh", 5) &&
                  !os64_html_attr(b, "b"), "clone attribute list independent");
        fragment_invariants(doc, context, fragment);
    }
    os64_html_unpin(doc, pin);
    os64_html_document_free(doc);
    check(!live, "fragment lifetime leak");
    opt.scripting = true;
    os64_html_parser_t *parser = os64_html_parser_new(&opt);
    source = "<!doctype html><div id=c a=old></div><script>x</script><p id=after>y";
    int64_t status = os64_html_parser_feed(parser, source, strlen(source));
    if (!status)
        status = os64_html_parser_end(parser);
    check(status == OS64_HTML_SCRIPT, "stopped fragment host fixture");
    doc = os64_html_parser_document(parser);
    context = find_named(doc->document, "div");
    version = os64_html_version(doc);
    HNode *script = (HNode *)os64_html_parser_script(parser);
    fragment = os64_html_parse_fragment(doc, context, "<script>z</script><b>after</b>", strlen("<script>z</script><b>after</b>"), true, &why);
    check(fragment && !why && find_named(fragment->first_child, "b"), "fragment completes while host is stopped");
    check(os64_html_version(doc) == version && os64_html_parser_script(parser) == script &&
              !find_named(doc->document, "p"), "fragment leaves stopped parser and pending input untouched");
    os64_html_parser_destroy(parser);
    check(!live, "stopped fragment leak");
}
static void fragment_budget_checks(void)
{
    for (unsigned cut = 0; cut < 6; cut++) {
        os64_html_document_t *doc = fragment_document();
        HNode *context = fragment_context(doc, "div");
        HDoc *private = (HDoc *)doc;
        private->budget = doc->arena_bytes + (size_t)cut * 128;
        doc->refusal = OS64_HTML_TOO_LARGE;
        uint64_t version = os64_html_version(doc), hash = validate(doc);
        size_t arena = doc->arena_bytes, nodes = doc->node_count, base_live = live;
    size_t peak = doc->peak_arena_bytes;
        os64_html_pin_t pin = os64_html_pin(doc);
        int64_t why = 0;
        HNode *fragment = os64_html_parse_fragment(doc, context, "<p a=x>hello</p>", 16, false, &why);
        check(!fragment && why == OS64_HTML_ARENA_EXHAUSTED, "tiny fragment arena refused %u", cut);
        check(doc->arena_bytes == arena && doc->node_count == nodes && doc->peak_arena_bytes == peak && live == base_live &&
                  validate(doc) == hash && os64_html_version(doc) == version &&
                  doc->refusal == OS64_HTML_TOO_LARGE, "tiny arena failure preserves refused/pinned document");
        os64_html_unpin(doc, pin);
        os64_html_document_free(doc);
        check(!live, "tiny arena fragment leak");
    }
    os64_html_document_t *doc = fragment_document();
    HNode *context = fragment_context(doc, "div");
    ((HDoc *)doc)->max_depth = 1;
    uint64_t version = os64_html_version(doc), hash = validate(doc);
    size_t arena = doc->arena_bytes, nodes = doc->node_count, base_live = live;
    size_t peak = doc->peak_arena_bytes;
    int64_t why = 0;
    const char *deep = "<div><div><div><div><div><div><div><div>x";
    HNode *fragment = os64_html_parse_fragment(doc, context, deep, strlen(deep), false, &why);
    check(!fragment && why == OS64_HTML_TOO_DEEP, "fragment depth refused");
    check(doc->arena_bytes == arena && doc->node_count == nodes && doc->peak_arena_bytes == peak && live == base_live &&
              os64_html_version(doc) == version && validate(doc) == hash, "fragment depth failure atomic");
    os64_html_document_free(doc);
    check(!live, "depth fragment leak");
}
static void fragment_namespace_checks(void)
{
    static const struct { const char *context, *input; os64_html_ns_t want; } cases[] = {
        {"svg path", "<circle/>", OS64_HTML_NS_SVG},
        {"svg foreignObject", "<b>x</b>", OS64_HTML_NS_HTML},
        {"math math", "<mi>x</mi>", OS64_HTML_NS_MATHML},
        {"math mtext", "<b>x</b>", OS64_HTML_NS_HTML},
        {"math mtext", "<mglyph/>", OS64_HTML_NS_MATHML}
    };
    for (size_t i = 0; i < H_ARRAY(cases); i++) {
        os64_html_document_t *doc = fragment_document();
        HNode *context = fragment_context(doc, cases[i].context);
        int64_t why = 0;
        HNode *fragment = os64_html_parse_fragment(doc, context, cases[i].input, strlen(cases[i].input), false, &why);
        check(fragment && !why && fragment->first_child && fragment->first_child->ns == cases[i].want,
              "fragment namespace integration %s", cases[i].context);
        if (fragment)
            fragment_invariants(doc, context, fragment);
        os64_html_document_free(doc);
        check(!live, "fragment namespace leak");
    }
    os64_html_document_t *doc = fragment_document();
    HNode *context = fragment_context(doc, "math annotation-xml");
    check(!os64_html_set_attr(doc, context, "encoding", "text/html", 9), "math context encoding fixture");
    int64_t why = 0;
    HNode *fragment = os64_html_parse_fragment(doc, context, "<b>x</b>", 8, false, &why);
    check(fragment && !why && fragment->first_child && fragment->first_child->ns == OS64_HTML_NS_HTML,
          "context attributes choose HTML integration");
    os64_html_document_free(doc);
    check(!live, "context attributes fragment leak");
}
static void serializer_expect(const HNode *node, bool children, bool scripting, const char *want,
                              const char *name)
{
    char *out = serialization_check(node, children, scripting);
    check(!strcmp(out, want), "serializer %s wanted [%s] got [%s]", name, want, out);
    free(out);
}
static void serializer_outer_checks(void)
{
    const char *source = "<!DOCTYPE html PUBLIC 'abc' 'xyz'><div a=x>one</div><p>two</p><template><b>t</b></template>";
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "utf-8";
    os64_html_document_t *doc = parse_raw((const unsigned char *)source, strlen(source), 0, &opt);
    HNode *div = find_named(doc->document, "div");
    serializer_expect(div, false, false, "<div a=\"x\">one</div>", "outer excludes next sibling");
    serializer_expect(div->first_child, false, false, "one", "text outer");
    HNode *templ = find_named(doc->document, "template");
    serializer_expect(templ, true, false, "<b>t</b>", "template inner");
    serializer_expect(templ, false, false, "<template><b>t</b></template>", "template outer");
    const char *document = "<!DOCTYPE html><html><head></head><body><div a=\"x\">one</div><p>two</p><template><b>t</b></template></body></html>";
    serializer_expect(doc->document, true, false, document, "document inner");
    serializer_expect(doc->document, false, false, document, "document outer no wrapper/public identifiers");
    HNode *context = fragment_context(doc, "div");
    int64_t why = 0;
    HNode *fragment = os64_html_parse_fragment(doc, context, "<b>x</b><i>y</i>", 16, false, &why);
    check(fragment && !why, "outer fragment fixture");
    serializer_expect(fragment, false, false, "<b>x</b><i>y</i>", "fragment outer no wrapper");
    HNode *void_node = fragment_context(doc, "br");
    HNode *text = os64_html_create_text(doc, "invisible", 9, &why);
    check(text && !why && !os64_html_insert(doc, void_node, text, NULL), "void child fixture");
    serializer_expect(void_node, true, false, "", "void inner empty");
    serializer_expect(void_node, false, false, "<br>", "void outer excludes artificial children");
    HNode *raw = fragment_context(doc, "script");
    text = os64_html_create_text(doc, "<&", 2, &why);
    check(text && !why && !os64_html_insert(doc, raw, text, NULL), "raw child fixture");
    serializer_expect(text, false, false, "<&", "raw-parent text outer");
    serializer_expect(NULL, true, false, "", "NULL inner");
    serializer_expect(NULL, false, false, "", "NULL outer");
    os64_html_document_free(doc);
    check(!live, "outer serializer leak");
}
static void fragment_checks(void)
{
    serializer_outer_checks();
    fragment_namespace_checks();
    fragment_utf8_checks();
    fragment_lifetime_checks();
    fragment_budget_checks();
    static const char *raw_parents[] = {"style", "script", "xmp", "iframe", "noembed", "noframes", "plaintext"};
    for (size_t i = 0; i < H_ARRAY(raw_parents); i++)
        serializer_parent(raw_parents[i], false, "&\xc2\xa0<>\"");
    serializer_parent("noscript", true, "&\xc2\xa0<>\"");
    serializer_parent("noscript", false, "&amp;&nbsp;&lt;&gt;\"");
    serializer_parent("textarea", true, "&amp;&nbsp;&lt;&gt;\"");
    fragment_hand("div", "<p a='&amp;&#160;&lt;&gt;&quot;'> &amp;&#160;&lt;&gt;\"</p>", false,
                  "<p a=\"&amp;&nbsp;&lt;&gt;&quot;\"> &amp;&nbsp;&lt;&gt;\"</p>");
    fragment_hand("div", "<br><hr><img><input><wbr>", false, "<br><hr><img><input><wbr>");
    fragment_hand("div", "<!--hello--><template><b>x</b></template>", false,
                  "<!--hello--><template><b>x</b></template>");
    fragment_hand("textarea", "&amp;<b>x</b></textarea>", false,
                  "&amp;&lt;b&gt;x&lt;/b&gt;&lt;/textarea&gt;");
    fragment_hand("title", "&amp;<b>x</b></title>", false,
                  "&amp;&lt;b&gt;x&lt;/b&gt;&lt;/title&gt;");
    static const char *raw[] = {"style", "xmp", "iframe", "noembed", "noframes", "script", "plaintext"};
    for (size_t i = 0; i < H_ARRAY(raw); i++)
        fragment_hand(raw[i], "&amp;<b>x</b>", true, "&amp;amp;&lt;b&gt;x&lt;/b&gt;");
    fragment_hand("noscript", "<b>x</b>&amp;", false, "<b>x</b>&amp;");
    fragment_hand("noscript", "<b>x</b>&amp;", true, "&lt;b&gt;x&lt;/b&gt;&amp;amp;");
    fragment_hand("caption", "</caption><div>", false, "<div></div>");
    fragment_hand("caption", "<tr><td>x", false, "x");
    fragment_hand("colgroup", "foo<col>", false, "<col>");
    fragment_hand("head", "<title>x</title>", false, "<title>x</title>");
    fragment_hand("html", "x", false, "<head></head><body>x</body>");
    fragment_hand("frameset", "</frameset><frame>", false, "<frame>");
    fragment_hand("td", "<b>x", false, "<b>x</b>");
    fragment_hand("thead", "<tr><td>x", false, "<tr><td>x</td></tr>");
    fragment_hand("tfoot", "<tr><td>x", false, "<tr><td>x</td></tr>");
    fragment_hand("table", "<tr><td>x", false, "<tbody><tr><td>x</td></tr></tbody>");
    fragment_hand("tbody", "<tr><td>x", false, "<tr><td>x</td></tr>");
    fragment_hand("tr", "<td>x", false, "<td>x</td>");
    fragment_hand("select", "<div>x<option>a<option>b", false, "x<option>a</option><option>b</option>");
    fragment_hand("template", "<tr><td>x", false, "<tr><td>x</td></tr>");
    fragment_hand("div", "<script>a</script><b>after</b>", true, "<script>a</script><b>after</b>");
    for (unsigned scripting = 0; scripting < 2; scripting++) {
        fragment_hand("svg svg", "</html><circle/>", scripting, "<circle></circle>");
        fragment_hand("svg path", "</html><circle/>", scripting, "<circle></circle>");
        fragment_hand("math math", "</html><mi/>", scripting, "<mi></mi>");
        fragment_hand("svg svg", "<g></html><circle/>", scripting, "<g><circle></circle></g>");
        const char *guard = "</html><circle/>";
        fragment_safety((const unsigned char *)guard, strlen(guard), "svg svg", scripting,
                        4096 + 256 * strlen(guard), true);
    }
    fragment_hand("svg svg", "<circle xlink:href='#x' xml:lang='en'/>", false,
                  "<circle xlink:href=\"#x\" xml:lang=\"en\"></circle>");
    fragment_hand("div", "\xef\xbb\xbf<meta charset=windows-1252>\xc3\xa9", false,
                  "\xef\xbb\xbf<meta charset=\"windows-1252\">\xc3\xa9");
    os64_html_document_t *doc = fragment_document(), *other = fragment_document();
    HNode *context = fragment_context(other, "div");
    uint64_t version = os64_html_version(doc);
    int64_t why = 0;
    check(!os64_html_parse_fragment(doc, context, "x", 1, false, &why) &&
              why == OS64_HTML_BAD_ARGUMENT, "foreign fragment context refused");
    check(!os64_html_parse_fragment(doc, NULL, "x", 1, false, &why) &&
              why == OS64_HTML_BAD_ARGUMENT, "NULL fragment context refused");
    check(os64_html_version(doc) == version && !doc->refusal, "refused fragment host unchanged");
    os64_html_document_free(other);
    os64_html_document_free(doc);
    check(!live, "fragment checks leak");
}
static uint64_t fragment_host_hash(os64_html_document_t *doc)
{
    char *dump = fragment_dump(doc, doc->document);
    uint64_t hash = hash_string(1469598103934665603ull, dump);
    free(dump);
    known_reset();
    known_add(doc->document);
    known_close();
    for (size_t i = 0; i < known.n; i++) {
        uintptr_t owner = (uintptr_t)known.list[i]->form_owner;
        hash = hash_bytes(hash, (const char *)&owner, sizeof(owner));
    }
    known_reset();
    hash = hash_bytes(hash, (const char *)&doc->parse_errors, sizeof(doc->parse_errors));
    hash = hash_bytes(hash, (const char *)&doc->work, sizeof(doc->work));
    hash = hash_bytes(hash, (const char *)&doc->input_bytes, sizeof(doc->input_bytes));
    hash = hash_bytes(hash, (const char *)&doc->quirks, sizeof(doc->quirks));
    hash = hash_string(hash, doc->charset);
    hash = hash_bytes(hash, (const char *)&doc->refusal, sizeof(doc->refusal));
    return hash;
}
static void fragment_safety(const unsigned char *data, size_t len, const char *name, bool scripting,
                            uint64_t budget, bool sweep)
{
    fail_at = 0;
    os64_html_document_t *doc = fragment_document();
    HNode *context = fragment_context(doc, name);
    uint64_t version = os64_html_version(doc), hash = fragment_host_hash(doc);
    size_t arena = doc->arena_bytes, nodes = doc->node_count, base_live = live;
    size_t peak = doc->peak_arena_bytes;
    size_t retired = os64_html_retired_bytes(doc);
    os64_html_pin_t pin = os64_html_pin(doc);
    uint64_t work = 0;
    int64_t why = 0;
    allocations = 0;
    HNode *fragment = h_parse_fragment(doc, context, (const char *)data, len, scripting,
                                      budget, &work, &why);
    size_t count = allocations;
    if (fragment) {
        if (why || work > budget)
            safety_fail("fragment success status/work");
        fragment_invariants(doc, context, fragment);
        char *serialized = serialization_check(fragment, true, scripting);
        free(serialized);
    } else {
        if (!why || why == OS64_HTML_SCRIPT || doc->arena_bytes != arena ||
            doc->node_count != nodes || doc->peak_arena_bytes != peak || live != base_live)
            safety_fail("fragment refused rollback");
    }
    if (os64_html_version(doc) != version || doc->refusal || fragment_host_hash(doc) != hash ||
        os64_html_retired_bytes(doc) != retired)
        safety_fail("fragment changed visible host or pin");
    os64_html_unpin(doc, pin);
    os64_html_document_free(doc);
    if (live)
        safety_fail("fragment baseline leak");
    if (!sweep)
        return;
    if (!fragment)
        safety_fail("ordinary fragment reference refused");
    for (size_t n = 1; n <= count; n++) {
        doc = fragment_document();
        context = fragment_context(doc, name);
        version = os64_html_version(doc);
        hash = fragment_host_hash(doc);
        arena = doc->arena_bytes;
        peak = doc->peak_arena_bytes;
        nodes = doc->node_count;
        base_live = live;
        pin = os64_html_pin(doc);
        allocations = 0;
        fail_at = n;
        fragment = h_parse_fragment(doc, context, (const char *)data, len, scripting,
                                    budget, NULL, &why);
        fail_at = 0;
        if (fragment || why != OS64_HTML_NO_MEMORY || doc->arena_bytes != arena ||
            doc->node_count != nodes || doc->peak_arena_bytes != peak || live != base_live || os64_html_version(doc) != version ||
            fragment_host_hash(doc) != hash || doc->refusal)
            safety_fail("fragment allocation failure rollback/status");
        os64_html_unpin(doc, pin);
        os64_html_document_free(doc);
        if (live)
            safety_fail("fragment allocation sweep leak");
        fragment_failures++;
    }
    for (uint64_t cut = 0; cut < work; cut++) {
        doc = fragment_document();
        context = fragment_context(doc, name);
        arena = doc->arena_bytes;
        peak = doc->peak_arena_bytes;
        nodes = doc->node_count;
        version = os64_html_version(doc);
        hash = fragment_host_hash(doc);
        base_live = live;
        pin = os64_html_pin(doc);
        uint64_t consumed = UINT64_MAX;
        fragment = h_parse_fragment(doc, context, (const char *)data, len, scripting,
                                    cut, &consumed, &why);
        if (fragment || why != OS64_HTML_WORK_EXHAUSTED || consumed > cut ||
            doc->arena_bytes != arena || doc->node_count != nodes || doc->peak_arena_bytes != peak || live != base_live ||
            os64_html_version(doc) != version || fragment_host_hash(doc) != hash || doc->refusal)
            safety_fail("fragment work cut rollback/status");
        os64_html_unpin(doc, pin);
        os64_html_document_free(doc);
        if (live)
            safety_fail("fragment work sweep leak");
        fragment_work_cuts++;
    }
    fragment_cases++;
}
/* These exclusions name parser transformations the standard warns about;
 * ordinary HTML and foreign trees are checked under their original context. */
static const char *fragment_roundtrip_reason(const HNode *node, bool scripting,
                                             bool in_form, bool in_anchor)
{
    for (const HNode *n = node; n; n = n->next) {
        if (n->kind == OS64_HTML_COMMENT) {
            if (strstr(n->text, "-->") || strstr(n->text, "--!>") || n->text[0] == '>')
                return "comment-termination";
        } else if (n->kind == OS64_HTML_ELEMENT) {
            bool html = n->ns == OS64_HTML_NS_HTML;
            bool form = html && !strcmp(n->name, "form");
            bool anchor = html && !strcmp(n->name, "a");
            if ((form && in_form) || (anchor && in_anchor))
                return "nested-form-or-anchor-parser-repair";
            if (html && !strcmp(n->name, "plaintext"))
                return "plaintext-absorbs-closing-markup";
            if (html && (!strcmp(n->name, "pre") || !strcmp(n->name, "textarea") || !strcmp(n->name, "listing")) &&
                n->first_child && n->first_child->kind == OS64_HTML_TEXT && n->first_child->text[0] == '\n')
                return "initial-lf-stripping";
            bool raw = html && (h_in(n->name, "style script xmp iframe noembed noframes") ||
                                 (scripting && !strcmp(n->name, "noscript")));
            bool rcdata = html && h_in(n->name, "textarea title");
            if (raw || rcdata) {
                for (const HNode *c = n->first_child; c; c = c->next) {
                    if (c->kind != OS64_HTML_TEXT)
                        return "raw-or-rcdata-nontext-child";
                    if (raw && strstr(c->text, "</"))
                        return "raw-text-end-tag-delimiter";
                }
            }
            if (html && !strcmp(n->name, "p")) {
                for (const HNode *c = n->first_child; c; c = c->next)
                    if (c->kind == OS64_HTML_ELEMENT && c->ns == OS64_HTML_NS_HTML &&
                        h_in(c->name, "address article aside blockquote div dl fieldset footer form h1 h2 h3 h4 h5 h6 header hgroup hr main nav ol p pre section table ul"))
                        return "paragraph-block-parser-repair";
            }
            const HNode *children = n->template_contents ? n->template_contents->first_child : n->first_child;
            const char *why = fragment_roundtrip_reason(children, scripting,
                                                        n->template_contents ? false : in_form || form,
                                                        n->template_contents ? false : in_anchor || anchor);
            if (why)
                return why;
        } else if (n->kind != OS64_HTML_TEXT)
            return "non-element-text-comment-node";
    }
    return NULL;
}
static void fragment_record(const unsigned char *data, size_t len, const char *name, bool scripting)
{
    os64_html_document_t *doc = fragment_document();
    HNode *context = fragment_context(doc, name);
    int64_t why = 0;
    HNode *fragment = os64_html_parse_fragment(doc, context, (const char *)data, len, scripting, &why);
    if (!fragment)
        safety_fail("reference fragment refusal");
    fragment_invariants(doc, context, fragment);
    char *dump = fragment_dump(doc, fragment);
    char *serialized = serialization_check(fragment, true, scripting);
    const char *excluded = NULL;
    if (!strcmp(name, "style") || !strcmp(name, "script") || !strcmp(name, "plaintext"))
        excluded = "raw-text-context";
    else
        excluded = fragment_roundtrip_reason(fragment->first_child, scripting, false, false);
    if (!excluded) {
        HNode *again = os64_html_parse_fragment(doc, context, serialized, strlen(serialized), scripting, &why);
        if (!again || why)
            safety_fail("fragment roundtrip parse refused");
        char *roundtrip = fragment_dump(doc, again);
        if (strcmp(dump, roundtrip))
            safety_fail("promised subset fragment roundtrip differs");
        free(roundtrip);
        fragment_invariants(doc, context, again);
    }
    fputs("{\"roundtrip_exclusion\":", stdout);
    json_string(excluded);
    fputs(",\"tree\":", stdout);
    json_string(dump);
    fputs(",\"serialized\":", stdout);
    json_string(serialized);
    printf(",\"refusal\":%lld}\n", (long long)why);
    free(serialized);
    free(dump);
    os64_html_document_free(doc);
    if (live)
        safety_fail("fragment reference leak");
}
