// SHADOW_DOM.md: the page model over shadow trees. A shadow tree cannot
// come from markup (declarative roots are booked), so these cases make
// their trees with libhtml's verbs and then build the model.

static os64_html_node_t *shadow_input(os64_html_document_t *doc, os64_html_node_t *parent, const char *type,
                                      const char *name, bool checked)
{
    os64_html_node_t *n = os64_html_create_element(doc, OS64_HTML_NS_HTML, "input", NULL);
    os64_html_set_attr(doc, n, "type", type, os64_strlen(type));
    os64_html_set_attr(doc, n, "name", name, os64_strlen(name));
    if (checked)
        os64_html_set_attr(doc, n, "checked", "", 0);
    os64_html_insert(doc, parent, n, NULL);
    return n;
}

static void shadow_cases(void)
{
    // A radio group is per tree: two instances of one component, each with
    // its own form-less radio of one name, both checked, are two groups.
    const char *name = "shadow: two components' form-less radios of one name are two groups";
    checks++;
    os64_html_document_t *doc = parse("<body><div id=a></div><div id=b></div><input type=radio name=size checked>",
                                      "utf-8");
    os64_html_node_t *a = doc != NULL ? doc->body->first_child : NULL, *b = a != NULL ? a->next : NULL;
    os64_html_node_t *ra = a != NULL ? os64_html_attach_shadow(doc, a, OS64_HTML_SHADOW_OPEN, NULL) : NULL;
    os64_html_node_t *rb = b != NULL ? os64_html_attach_shadow(doc, b, OS64_HTML_SHADOW_OPEN, NULL) : NULL;
    if (ra == NULL || rb == NULL) {
        fail(name, "the fixture: no shadow roots");
    } else {
        os64_html_node_t *in_a = shadow_input(doc, ra, "radio", "size", true);
        os64_html_node_t *in_b = shadow_input(doc, rb, "radio", "size", true);
        os64_html_node_t *in_doc = b->next;
        os64_page_t *page = os64_page_build(doc, kPage, NULL, NULL);
        const os64_page_control_t *ca = page ? os64_page_control(page, os64_page_control_for(page, in_a)) : NULL;
        const os64_page_control_t *cb = page ? os64_page_control(page, os64_page_control_for(page, in_b)) : NULL;
        const os64_page_control_t *cd = page ? os64_page_control(page, os64_page_control_for(page, in_doc)) : NULL;
        if (ca == NULL || cb == NULL || cd == NULL)
            fail(name, "a control is missing from the model");
        else if (!ca->checked || !cb->checked || !cd->checked)
            fail(name, "checked: first %d, second %d, document's %d", ca->checked, cb->checked, cd->checked);
        os64_page_free(page);
    }
    os64_html_document_free(doc);

    // form= resolves in the control's own tree: a shadow tree's form of
    // that id, not the document's.
    name = "shadow: form= in a shadow tree names its own tree's form";
    checks++;
    doc = parse("<body><form id=f action=/doc></form><div id=h></div>", "utf-8");
    os64_html_node_t *host = doc != NULL ? doc->body->first_child->next : NULL;
    os64_html_node_t *root = host != NULL ? os64_html_attach_shadow(doc, host, OS64_HTML_SHADOW_OPEN, NULL) : NULL;
    if (root == NULL) {
        fail(name, "the fixture: no shadow root");
    } else {
        os64_html_node_t *form = os64_html_create_element(doc, OS64_HTML_NS_HTML, "form", NULL);
        os64_html_set_attr(doc, form, "id", "f", 1);
        os64_html_insert(doc, root, form, NULL);
        os64_html_node_t *field = shadow_input(doc, root, "text", "q", false);
        os64_html_set_attr(doc, field, "form", "f", 1);
        os64_page_t *page = os64_page_build(doc, kPage, NULL, NULL);
        const os64_page_control_t *c = page ? os64_page_control(page, os64_page_control_for(page, field)) : NULL;
        const os64_page_form_t *owner = c != NULL && c->form >= 0 ? os64_page_form(page, c->form) : NULL;
        if (owner == NULL || owner->node != form)
            fail(name, "the owner is %s", owner == NULL ? "none" : "the document's form");
        os64_page_free(page);
    }
    os64_html_document_free(doc);

    // A clonable host's clone carries its shadow controls' state, a shallow
    // clone's too (Codex #483).
    name = "shadow: a clone keeps an edited shadow input's value";
    checks++;
    doc = parse("<body><div id=h></div>", "utf-8");
    os64_page_state_t *state = doc != NULL ? os64_page_state_create(doc, 0) : NULL;
    host = doc != NULL ? doc->body->first_child : NULL;
    root = host != NULL ? os64_html_attach_shadow(doc, host, OS64_HTML_SHADOW_OPEN | OS64_HTML_SHADOW_CLONABLE, NULL)
                        : NULL;
    if (root == NULL || state == NULL) {
        fail(name, "the fixture");
    } else {
        os64_html_node_t *field = shadow_input(doc, root, "text", "q", false);
        os64_html_set_attr(doc, field, "value", "default", 7);
        os64_page_node_set_value(state, field, "edited", 6);
        for (int deep = 0; deep < 2; deep++) {
            os64_html_node_t *copy = os64_page_node_clone(state, host, deep != 0, NULL);
            const os64_html_node_t *cfield = copy && copy->shadow_root ? copy->shadow_root->first_child : NULL;
            const char *value = NULL;
            size_t len = 0;
            if (cfield == NULL || os64_page_node_value(state, cfield, &value, &len) != OS64_HTML_OK ||
                len != 6 || memcmp(value, "edited", 6) != 0)
                fail(name, "%s clone: %.*s", deep ? "deep" : "shallow", (int)len, value ? value : "");
        }
    }
    os64_page_state_free(state);
    os64_html_document_free(doc);

    // A host removed changes its shadow radios' owners: form= resolves only
    // while connected. The radio whose owner changed wins its new group
    // (Codex #483).
    name = "shadow: a removed host's radio joining a group clears the group's other";
    checks++;
    doc = parse("<body><div id=h></div>", "utf-8");
    state = doc != NULL ? os64_page_state_create(doc, 0) : NULL;
    host = doc != NULL ? doc->body->first_child : NULL;
    root = host != NULL ? os64_html_attach_shadow(doc, host, OS64_HTML_SHADOW_OPEN, NULL) : NULL;
    if (root == NULL || state == NULL) {
        fail(name, "the fixture");
    } else {
        os64_html_node_t *f = os64_html_create_element(doc, OS64_HTML_NS_HTML, "form", NULL);
        os64_html_set_attr(doc, f, "id", "f", 1);
        os64_html_insert(doc, root, f, NULL);
        os64_html_node_t *g = os64_html_create_element(doc, OS64_HTML_NS_HTML, "form", NULL);
        os64_html_insert(doc, root, g, NULL);
        os64_html_node_t *first = shadow_input(doc, g, "radio", "x", true);
        os64_html_set_attr(doc, first, "form", "f", 1);
        os64_html_node_t *second = shadow_input(doc, g, "radio", "x", true);
        bool a = false, b = false;
        os64_page_node_checked(state, first, &a);
        os64_page_node_checked(state, second, &b);
        if (!a || !b) fail(name, "the fixture: both checked in two groups (%d %d)", a, b);
        if (os64_page_node_remove(state, host) != OS64_HTML_OK) fail(name, "remove refused");
        os64_page_node_checked(state, first, &a);
        os64_page_node_checked(state, second, &b);
        if (!a || b) fail(name, "after removal: first %d, second %d", a, b);
        os64_page_node_insert(state, doc->body, host, NULL);
        os64_page_node_checked(state, first, &a);
        os64_page_node_checked(state, second, &b);
        if (!a || b) fail(name, "after reinsertion: first %d, second %d", a, b);
    }
    os64_page_state_free(state);
    os64_html_document_free(doc);
}
