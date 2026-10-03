// Review regressions exercise public budgets, observations and node identities.
static os64_html_node_t *followup_element(os64_html_document_t *doc, const char *name,
                                         os64_html_node_t *parent)
{
    int64_t status = 0;
    os64_html_node_t *node = os64_html_create_element(doc, OS64_HTML_NS_HTML, name, &status);
    rebuild_expect("review: create node", node != NULL && status == OS64_HTML_OK);
    if (parent != NULL)
        rebuild_expect("review: attach node", os64_html_insert(doc, parent, node, NULL) == OS64_HTML_OK);
    return node;
}

static os64_html_node_t *followup_option(os64_html_document_t *doc, os64_html_node_t *parent,
                                        const char *value)
{
    os64_html_node_t *node = followup_element(doc, "option", parent);
    rebuild_expect("review: option value", os64_html_set_attr(doc, node, "value", value, strlen(value)) == 0);
    return node;
}

static RebuildFixture followup_reservation_fixture(size_t cap, os64_html_node_t **fresh)
{
    RebuildFixture f = rebuild_fixture("<input id=kept value=default>", cap);
    for (int i = 0; i < 65; i++) {
        os64_html_node_t *node = i == 0 ? rebuild_id(f.doc, "kept") : followup_element(f.doc, "input", NULL);
        rebuild_expect("review: populate live values", os64_page_node_set_value(f.state, node, "kept", 4) == 0);
    }
    *fresh = followup_element(f.doc, "input", NULL);
    return f;
}

static void followup_first_touch_cap(void)
{
    size_t heap = live;
    os64_html_node_t *fresh;
    RebuildFixture probe = followup_reservation_fixture(0, &fresh);
    // Room for the earlier small-table growth and setter scratch, but not
    // for duplicating the larger table when it already has spare slots.
    size_t cap = os64_page_state_bytes(probe.state) + 1536;
    rebuild_clear(&probe);
    RebuildFixture f = followup_reservation_fixture(cap, &fresh);
    uint64_t html_version = os64_html_version(f.doc);
    rebuild_expect("review: spare capacity first touch fits cap",
        os64_page_node_set_value(f.state, fresh, "new", 3) == 0);
    rebuild_node_value("review: first touch value", f.state, fresh, "new");
    rebuild_value("review: first touch preserves prior publication", f.page, rebuild_id(f.doc, "kept"), "kept");
    rebuild_expect("review: first touch leaves HTML alone", os64_html_version(f.doc) == html_version);
    rebuild_clear(&f);
    rebuild_expect("review: first touch releases heap", live == heap);
}

static RebuildFixture followup_batch_fixture(int count, os64_html_node_t **select)
{
    os64_html_node_t *unused;
    RebuildFixture f = followup_reservation_fixture(0, &unused);
    *select = followup_element(f.doc, "select", NULL);
    for (int i = 0; i < count; i++) {
        char value[24]; snprintf(value, sizeof(value), "choice-%d", i);
        followup_option(f.doc, *select, value);
    }
    return f;
}

static void followup_batch_failures(int count)
{
    os64_html_node_t *select;
    RebuildFixture f = followup_batch_fixture(count, &select);
    size_t before = allocations;
    rebuild_expect("review: batch assignment", os64_page_node_set_selected_index(f.state, select, count - 1) == 0);
    size_t points = allocations - before;
    char expected[24]; snprintf(expected, sizeof(expected), "choice-%d", count - 1);
    rebuild_node_value("review: batch selection", f.state, select, expected);
    rebuild_clear(&f);
    for (size_t point = 1; point <= points; point++) {
        size_t heap = live;
        f = followup_batch_fixture(count, &select);
        size_t bytes = os64_page_state_bytes(f.state);
        uint64_t state_version = os64_page_state_version(f.state), html_version = os64_html_version(f.doc);
        fail_single = true; fail_at = allocations + point;
        int64_t status = os64_page_node_set_selected_index(f.state, select, count - 1);
        fail_at = 0; fail_single = false;
        int32_t index = -99;
        rebuild_expect("review: batch cut refuses", status == -OS64_PAGE_REASON_NO_MEMORY);
        rebuild_expect("review: batch refusal retains selection",
            os64_page_node_selected_index(f.state, select, &index) == 0 && index == 0);
        rebuild_expect("review: batch refusal retains budget and revisions",
            os64_page_state_bytes(f.state) == bytes && os64_page_state_version(f.state) == state_version &&
            os64_html_version(f.doc) == html_version);
        rebuild_value("review: batch refusal retains publication", f.page, rebuild_id(f.doc, "kept"), "kept");
        rebuild_clear(&f);
        rebuild_expect("review: batch cut releases heap", live == heap);
    }
}

static void followup_sparse_defaults(void)
{
    size_t heap = live;
    const char *checkbox = "<input type=checkbox>", *option = "<option value=x>x";
    size_t count = 2048, a = strlen(checkbox), b = strlen(option);
    char *html = malloc(count * (a + b) + 64), *at = html;
    for (size_t i = 0; i < count; i++) { memcpy(at, checkbox, a); at += a; }
    memcpy(at, "<select id=s>", 13); at += 13;
    for (size_t i = 0; i < count; i++) { memcpy(at, option, b); at += b; }
    strcpy(at, "</select>");
    RebuildFixture f = rebuild_fixture(html, 1024);
    free(html);
    os64_html_node_t *select = rebuild_id(f.doc, "s"), *first = select->first_child;
    const os64_page_control_t *c = os64_page_control(f.page, os64_page_control_for(f.page, select));
    rebuild_expect("review: thousands of defaults fit small state cap",
        os64_page_ncontrols(f.page) == (int32_t)count + 1 && c->noptions == (int32_t)count &&
        c->options[0].selected && os64_page_state_bytes(f.state) < 1024);
    os64_html_node_t *checkbox_node = (os64_html_node_t *)os64_page_control(f.page, 0)->node;
    rebuild_expect("review: untouched checkbox default mutation",
        os64_html_set_attr(f.doc, checkbox_node, "checked", "", 0) == 0);
    rebuild_expect("review: move default-selected identity", os64_html_insert(f.doc, select, first, NULL) == 0);
    os64_page_t *fresh = os64_page_rebuild(f.page);
    c = os64_page_control(fresh, os64_page_control_for(fresh, select));
    int32_t index = -99;
    rebuild_expect("review: sparse default identity survives reorder",
        fresh != NULL && !os64_page_incomplete(fresh) && c->options[count - 1].node == first &&
        c->options[count - 1].selected && os64_page_node_selected_index(f.state, select, &index) == 0 &&
        index == (int32_t)count - 1);
    bool checked = false;
    rebuild_expect("review: sparse defaults publish to both live models",
        os64_page_control(f.page, 0)->checked && os64_page_control(fresh, 0)->checked &&
        os64_page_node_checked(f.state, checkbox_node, &checked) == 0 && checked);
    rebuild_expect("review: group assignment still obeys cap",
        os64_page_node_set_selected_index(f.state, select, 1) == -OS64_PAGE_REASON_NO_MEMORY);
    rebuild_expect("review: refused group assignment keeps default selection",
        os64_page_node_selected_index(f.state, select, &index) == 0 && index == (int32_t)count - 1);
    os64_page_free(fresh); rebuild_clear(&f);
    rebuild_expect("review: sparse defaults release heap", live == heap);
}

static void followup_equal_current_value(void)
{
    RebuildFixture f = rebuild_fixture("<input id=q type=range min=0 max=20>", 0);
    os64_html_node_t *q = rebuild_id(f.doc, "q");
    rebuild_expect("review: equal current value setup", os64_page_node_set_value(f.state, q, "10", 2) == 0);
    const char *old = NULL; size_t length = 0;
    rebuild_expect("review: initial dirty read", os64_page_node_value(f.state, q, &old, &length) == 0);
    rebuild_expect("review: change constraint", os64_html_set_attr(f.doc, q, "max", "15", 2) == 0);
    uint64_t version = os64_page_state_version(f.state);
    rebuild_expect("review: equal assignment after constraint", os64_page_node_set_value(f.state, q, "10", 2) == 0);
    size_t before = allocations;
    fail_at = allocations + 1;
    const char *value = NULL;
    int64_t status = os64_page_node_value(f.state, q, &value, &length);
    fail_at = 0;
    rebuild_expect("review: current dirty read needs no allocation",
        status == 0 && allocations == before && value == old && length == 2 && strcmp(value, "10") == 0);
    rebuild_expect("review: equal assignment keeps effective revision", os64_page_state_version(f.state) == version);
    rebuild_clear(&f);
}

static void followup_options_walk(void)
{
    RebuildFixture f = rebuild_fixture("<select id=s><option id=a value=A>A<option id=b value=B>B</select>", 0);
    os64_html_node_t *select = rebuild_id(f.doc, "s"), *a = rebuild_id(f.doc, "a");
    os64_html_node_t *expected[5] = {a, rebuild_id(f.doc, "b")};
    followup_option(f.doc, a, "nested-option");
    os64_html_node_t *div = followup_element(f.doc, "div", select);
    expected[2] = followup_option(f.doc, div, "E");
    followup_option(f.doc, followup_element(f.doc, "select", select), "nested-select");
    followup_option(f.doc, followup_element(f.doc, "datalist", select), "datalist");
    followup_option(f.doc, followup_element(f.doc, "hr", select), "hr");
    os64_html_node_t *group = followup_element(f.doc, "optgroup", select);
    rebuild_expect("review: disabled group", os64_html_set_attr(f.doc, group, "disabled", "", 0) == 0);
    expected[3] = followup_option(f.doc, group, "F");
    followup_option(f.doc, followup_element(f.doc, "optgroup", group), "nested-group");
    expected[4] = followup_option(f.doc, followup_element(f.doc, "div", group), "H");
    os64_page_t *fresh = os64_page_rebuild(f.page);
    const os64_page_control_t *c = os64_page_control(fresh, os64_page_control_for(fresh, select));
    rebuild_expect("review: shared option list excludes specified subtrees", fresh != NULL && c->noptions == 5);
    for (int i = 0; i < 5; i++) {
        rebuild_expect("review: model option node order", c->options[i].node == expected[i]);
        rebuild_expect("review: disabled group retained", c->options[i].disabled == (i >= 3));
        int32_t index = -99;
        rebuild_expect("review: select node index", os64_page_node_set_selected_index(f.state, select, i) == 0 &&
            os64_page_node_selected_index(f.state, select, &index) == 0 && index == i);
        const os64_html_attr_t *attr = os64_html_attr(expected[i], "value");
        rebuild_node_value("review: node value follows model option", f.state, select, attr->value);
        rebuild_value("review: model value agrees node value", fresh, select, attr->value);
        for (int j = 0; j < 5; j++)
            rebuild_expect("review: model selectedness agrees node index", c->options[j].selected == (i == j));
    }
    os64_page_free(fresh); rebuild_clear(&f);
}

static void rebuild_review_followup(void)
{
    followup_first_touch_cap();
    followup_batch_failures(16);
    followup_batch_failures(100);
    followup_sparse_defaults();
    followup_equal_current_value();
    followup_options_walk();
}
