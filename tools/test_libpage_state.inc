// State regressions from PRs #98 and #100, including allocation recovery.
static const char *state_html =
    "<form method=post action=/send><select name=s>"
    "<option label=Short>A<option>B</select>"
    "<input type=radio name=r value=a checked>"
    "<input type=radio name=r value=b checked>"
    "<input name=t value=base><input type=file name=f value=fake.txt>"
    "<button>Send</button></form>";

static void state_expect(const char *name, bool condition)
{
    checks++;
    if (!condition)
        fail(name, "state contract failed");
}

static void state_value(const char *name, os64_page_t *page, int32_t index, const char *value)
{
    const os64_page_control_t *c = os64_page_control(page, index);
    state_expect(name, c != NULL && c->value_len == strlen(value) &&
                       strcmp(c->value, value) == 0);
}

static void state_body(const char *name, os64_page_t *page, const char *body)
{
    os64_page_request_t req;
    os64_page_what_t what = {OS64_PAGE_ACTIVATE_CONTROL, 5, 0, 0};
    os64_page_verdict_t got = os64_page_activate(page, what, &req);
    state_expect(name, got == OS64_PAGE_NAVIGATE && req.body != NULL &&
                       req.body_len == strlen(body) && !memcmp(req.body, body, strlen(body)));
    os64_page_request_free(&req);
}

static void state_initial(const char *name, os64_page_t *page)
{
    state_expect(name, !os64_page_incomplete(page) && os64_page_ncontrols(page) == 6);
    state_value(name, page, 0, "A");
    const os64_page_control_t *select = os64_page_control(page, 0);
    state_expect(name, select->noptions == 2 && select->options[0].selected &&
                       !select->options[1].selected &&
                       !strcmp(select->options[0].label, "Short"));
    state_expect(name, !os64_page_control(page, 1)->checked &&
                       os64_page_control(page, 2)->checked);
    state_value(name, page, 3, "base");
    state_value(name, page, 4, "");
    state_body(name, page, "s=A&r=b&t=base&f=");
}

static void state_group_reset(void)
{
    os64_html_document_t *doc = parse(state_html, "utf-8");
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    state_initial("state: initial defaults", page);
    state_expect("state: edit text", os64_page_set_text(page, 3, "a\r\nb", 4) == 0);
    state_value("state: shared text sanitizer", page, 3, "ab");
    state_expect("state: edit radio", os64_page_set_checked(page, 1, true) == 0);
    state_expect("state: edit select", os64_page_set_chosen(page, 0, 1, true) == 0);
    state_value("state: select publishes current value", page, 0, "B");
    state_body("state: edited submission", page, "s=B&r=a&t=ab&f=");
    state_expect("state: file cannot be text", os64_page_set_text(page, 4, "fake", 4) ==
                 -OS64_PAGE_REASON_WRONG_KIND);
    size_t before = allocations;
    fail_at = before + 1;
    state_expect("state: reset succeeds without memory", os64_page_reset(page, 0) == 0);
    fail_at = 0;
    state_expect("state: reset allocates nothing", allocations == before);
    state_initial("state: reset normalized defaults", page);
    os64_page_free(page);
    os64_html_document_free(doc);
}

static void state_tick_values(void)
{
    os64_html_document_t *doc=parse("<form method=post><input type=checkbox name=c checked>"
        "<input type=radio name=r checked><button>Send</button></form>","utf-8");
    os64_page_t *page=os64_page_build(doc,kPage,NULL);
    for (int32_t i=0;i<2;i++) {
        state_value("tick: default value",page,i,"on");
        state_expect("tick: edit value",os64_page_set_text(page,i,"edited",6)==0);
    }
    os64_page_request_t req;
    os64_page_what_t what={OS64_PAGE_ACTIVATE_CONTROL,2,0,0};
    state_expect("tick: submit current value",os64_page_activate(page,what,&req)==OS64_PAGE_NAVIGATE &&
        req.body != NULL && !strcmp((const char *)req.body,"c=edited&r=edited"));
    os64_page_request_free(&req);
    state_expect("tick: reset",os64_page_reset(page,0)==0);
    state_value("tick: reset checkbox",page,0,"on");
    state_value("tick: reset radio",page,1,"on");
    os64_page_free(page);os64_html_document_free(doc);
}

static void state_sanitizers(void)
{
    static const struct { const char *html, *edit, *want; } cases[] = {
        {"<input type=number value=1>", "abc", ""},
        {"<input type=email>", " a@example.com\r\n ", "a@example.com"},
        {"<input type=email multiple>", " a@b , c@d ", "a@b,c@d"},
        {"<input type=url>", " https://host/\r\n ", "https://host/"},
        {"<input type=color>", "#AbCdEf", "#abcdef"},
        {"<input type=date>", "2024-02-30", ""},
        {"<textarea>base</textarea>", "a\r\nb\rc", "a\nb\nc"}
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        os64_html_document_t *doc = parse(cases[i].html, "utf-8");
        os64_page_t *page = os64_page_build(doc, kPage, NULL);
        state_expect("state: sanitizer accepts edit",
                     os64_page_set_text(page, 0, cases[i].edit, strlen(cases[i].edit)) == 0);
        state_value("state: sanitized edit", page, 0, cases[i].want);
        state_expect("state: ownerless reset", os64_page_reset(page, -1) == 0);
        os64_page_free(page);
        os64_html_document_free(doc);
    }
}

static void state_selection_cases(void)
{
    value_case("H13", "state: default skips disabled option and optgroup",
               "<select><option disabled>A<optgroup disabled><option>B</optgroup>"
               "<option>C</select>", 0, "C");
    value_case("H13", "state: multirow single select has no default",
               "<select size=2><option>A<option>B</select>", 0, "");
    value_case("H13", "state: multiple select has no default",
               "<select multiple><option>A<option>B</select>", 0, "");
    value_case("H13", "state: all-disabled select has no default",
               "<select><option disabled>A<option disabled>B</select>", 0, "");
    value_case("H13", "state: last selected wins in single select",
               "<select><option selected>A<option selected>B</select>", 0, "B");
    value_case("H13", "state: HTML display size parsing",
               "<select size=' +2trailing'><option>A<option>B</select>", 0, "");
    value_case("H13", "state: invalid display size defaults to one",
               "<select size=bad><option>A<option>B</select>", 0, "A");
    run((PCase){.steps="I4", .name="state: invalid size still has placeholder validation",
                .html="<form><select required size=bad><option value=''>Choose<option>B</select>"
                      "<button>Send</button></form>", .index=1,
                .verdict=OS64_PAGE_NOTHING, .check_reason=true,
                .reason=OS64_PAGE_REASON_INVALID});
    os64_html_document_t *doc = parse(
        "<input type=radio form=f name=x checked><form id=f>"
        "<input type=radio name=x checked hidden>"
        "<input type=radio checked><input type=radio checked></form>"
        "<form><input type=radio name=x checked></form>", "utf-8");
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    state_expect("state: explicit owners and hidden radios share group",
                 !os64_page_control(page,0)->checked && os64_page_control(page,1)->checked);
    state_expect("state: nameless radios and separate owners stay independent",
                 os64_page_control(page,2)->checked && os64_page_control(page,3)->checked &&
                 os64_page_control(page,4)->checked);
    state_expect("state: change explicit radio", os64_page_set_checked(page,0,true) == 0);
    state_expect("state: reset group", os64_page_reset(page,0) == 0);
    state_expect("state: group defaults restored",
                 !os64_page_control(page,0)->checked && os64_page_control(page,1)->checked);
    os64_page_free(page); os64_html_document_free(doc);
}

static int64_t state_edit(os64_page_t *page, int op)
{
    if (op == 0)
        return os64_page_set_text(page, 3, "changed", 7);
    if (op == 1)
        return os64_page_set_chosen(page, 0, 1, true);
    return os64_page_set_checked(page, 1, true);
}

static void state_allocation_failures(void)
{
    os64_html_document_t *doc = parse(state_html, "utf-8");
    size_t before = allocations;
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    size_t builds = allocations - before;
    os64_page_free(page);
    fail_single = true;
    for (size_t point = 1; point <= builds; point++) {
        fail_at = allocations + point;
        page = os64_page_build(doc, kPage, NULL);
        fail_at = 0;
        if (page != NULL && !os64_page_incomplete(page)) {
            state_initial("state: successful build has every control and option", page);
        } else if (page != NULL) {
            os64_page_request_t req;
            os64_page_what_t what = {OS64_PAGE_ACTIVATE_CONTROL, 5, 0, 0};
            state_expect("state: incomplete page cannot submit",
                         os64_page_activate(page,what,&req) == OS64_PAGE_REFUSED &&
                         req.reason == OS64_PAGE_REASON_NO_MEMORY);
            os64_page_request_free(&req);
            state_expect("state: incomplete page cannot reset",
                         os64_page_reset(page,0) == -OS64_PAGE_REASON_NO_MEMORY);
        }
        os64_page_free(page);
    }
    for (int op = 0; op < 3; op++) {
        page = os64_page_build(doc,kPage,NULL);
        before = allocations;
        state_expect("state: count successful edit", state_edit(page,op) == 0);
        size_t edits = allocations - before;
        os64_page_free(page);
        for (size_t point = 1; point <= edits; point++) {
            page = os64_page_build(doc,kPage,NULL);
            fail_at = allocations + point;
            int64_t result = state_edit(page,op);
            fail_at = 0;
            state_expect("state: edit reports allocation failure", result == -OS64_PAGE_REASON_NO_MEMORY);
            state_initial("state: failed edit preserves published state",page);
            state_expect("state: edit recovers after failure", state_edit(page,op) == 0);
            state_expect("state: reset after recovered edit", os64_page_reset(page,0) == 0);
            state_initial("state: reset after recovered edit restores defaults",page);
            os64_page_free(page);
        }
    }
    fail_single = false;
    os64_html_document_free(doc);
}

static void state_large_group(void)
{
    char html[4096];
    size_t len = (size_t)snprintf(html,sizeof(html),"<form>");
    for (int i = 0; i < 32; i++)
        len += (size_t)snprintf(html+len,sizeof(html)-len,
                               "<input type=radio name=r %s>",i == 0 ? "checked" : "");
    snprintf(html+len,sizeof(html)-len,"</form>");
    os64_html_document_t *doc = parse(html,"utf-8");
    os64_page_t *page = os64_page_build(doc,kPage,NULL);
    size_t before = allocations;
    state_expect("state: large group edit",os64_page_set_checked(page,31,true) == 0);
    size_t count = allocations-before;
    os64_page_free(page);
    fail_single = true;
    for (size_t point = 1; point <= count; point++) {
        page = os64_page_build(doc,kPage,NULL);
        fail_at = allocations+point;
        int64_t result = os64_page_set_checked(page,31,true);
        fail_at = 0;
        state_expect("state: group reservation reports failure",result == -OS64_PAGE_REASON_NO_MEMORY);
        bool same = true;
        for (int i = 0; i < 32; i++)
            same = same && os64_page_control(page,i)->checked == (i == 0);
        state_expect("state: failed group edit preserves all members",same);
        state_expect("state: group edit recovers",os64_page_set_checked(page,31,true) == 0);
        same = true;
        for (int i = 0; i < 32; i++)
            same = same && os64_page_control(page,i)->checked == (i == 31);
        state_expect("state: recovered group has one selection",same);
        state_expect("state: group reset",os64_page_reset(page,0) == 0);
        same = true;
        for (int i = 0; i < 32; i++)
            same = same && os64_page_control(page,i)->checked == (i == 0);
        state_expect("state: reset restores entire group",same);
        os64_page_free(page);
    }
    fail_single = false;
    os64_html_document_free(doc);
}

static void state_long_reset(void)
{
    char html[6200];
    const char *prefix = "<form method=post><input name=t maxlength=1 value='";
    size_t n = strlen(prefix);
    memcpy(html,prefix,n);
    memset(html+n,'x',6000);
    strcpy(html+n+6000,"'><button>Send</button></form>");
    os64_html_document_t *doc = parse(html,"utf-8");
    os64_page_t *page = os64_page_build(doc,kPage,NULL);
    state_expect("state: long default edit",os64_page_set_text(page,0,"z",1) == 0);
    size_t before = allocations;
    fail_at = before+1;
    state_expect("state: long default reset",os64_page_reset(page,0) == 0);
    fail_at = 0;
    state_expect("state: long reset allocates nothing",allocations == before);
    const os64_page_control_t *c = os64_page_control(page,0);
    state_expect("state: long default intact",c->value_len == 6000 && c->value[5999] == 'x');
    os64_page_request_t req;
    os64_page_what_t what = {OS64_PAGE_ACTIVATE_CONTROL,1,0,0};
    // The value exceeds maxlength but reset cleared its dirty flag.
    state_expect("state: reset clears dirty value",os64_page_activate(page,what,&req) == OS64_PAGE_NAVIGATE);
    os64_page_request_free(&req);
    os64_page_free(page); os64_html_document_free(doc);
}

static void state_cases(void)
{
    state_group_reset();
    state_sanitizers();
    state_tick_values();
    state_selection_cases();
    state_allocation_failures();
    state_large_group();
    state_long_reset();
}
