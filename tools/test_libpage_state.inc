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
    // A tick's value is the page's word about what it sends; nobody types it.
    for (int32_t i=0;i<2;i++) {
        state_value("tick: default value",page,i,"on");
        state_expect("tick: value takes no typing",
                     os64_page_set_text(page,i,"edited",6)==-OS64_PAGE_REASON_WRONG_KIND);
        state_value("tick: refused edit leaves value",page,i,"on");
    }
    os64_page_request_t req;
    os64_page_what_t what={OS64_PAGE_ACTIVATE_CONTROL,2,0,0};
    state_expect("tick: submit current value",os64_page_activate(page,what,&req)==OS64_PAGE_NAVIGATE &&
        req.body != NULL && !strcmp((const char *)req.body,"c=on&r=on"));
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

static void state_byte_spans(void)
{
    static const struct {
        const char *control, *edit;
        size_t edit_len;
        const char *want;
        size_t want_len;
        const char *body;
    } cases[] = {
#define SPAN(s) s, sizeof(s)-1
        {"<input name=q>", SPAN("a\0b\r\nc"), SPAN("a\0bc"), "q=a%00bc"},
        {"<input type=search name=q>", SPAN("a\0b"), SPAN("a\0b"), "q=a%00b"},
        {"<input type=tel name=q>", SPAN("a\0b"), SPAN("a\0b"), "q=a%00b"},
        {"<input type=password name=q>", SPAN("a\0b"), SPAN("a\0b"), "q=a%00b"},
        {"<input type=url name=q>", SPAN(" a\0b \r\n "), SPAN("a\0b"), "q=a%00b"},
        {"<input type=email name=q>", SPAN(" a\0b \r\n "), SPAN("a\0b"), "q=a%00b"},
        {"<input type=email multiple name=q>", SPAN(" a\0b , c@d "), SPAN("a\0b,c@d"), "q=a%00b%2Cc%40d"},
        {"<textarea name=q></textarea>", SPAN("a\0b\r\nc"), SPAN("a\0b\nc"), "q=a%00b%0D%0Ac"},
        {"<textarea name=q wrap=hard cols=2></textarea>", SPAN("a\0b"), SPAN("a\0b"), "q=a%00%0D%0Ab"},
        {"<input type=number name=q>", SPAN("12\0x"), SPAN(""), "q="},
        {"<input type=date name=q>", SPAN("2024-01-01\0x"), SPAN(""), "q="},
        {"<input type=month name=q>", SPAN("2024-01\0x"), SPAN(""), "q="},
        {"<input type=week name=q>", SPAN("2024-W01\0x"), SPAN(""), "q="},
        {"<input type=time name=q>", SPAN("12:30\0x"), SPAN(""), "q="},
        {"<input type=datetime-local name=q>", SPAN("2024-01-01T12:30\0x"), SPAN(""), "q="},
        {"<input type=range name=q>", SPAN("75\0x"), SPAN("50"), "q=50"},
        {"<input type=color name=q>", SPAN("#ff0000\0x"), SPAN("#000000"), "q=%23000000"},
        // The byte after the supplied span belongs to the caller, not the edit.
        {"<input name=q>", "a\0bX", 3, SPAN("a\0b"), "q=a%00b"},
        {"<input name=q>", SPAN("\0"), SPAN("\0"), "q=%00"},
#undef SPAN
    };
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        char html[256], name[80];
        snprintf(html,sizeof(html),"<form method=post>%s<button>Go</button></form>",cases[i].control);
        snprintf(name,sizeof(name),"byte span: case %zu",i);
        os64_html_document_t *doc=parse(html,"utf-8");
        os64_page_t *page=os64_page_build(doc,kPage,NULL);
        const os64_page_control_t *c=os64_page_control(page,0);
        const char *initial=c->value;size_t initial_len=c->value_len;
        state_expect(name,os64_page_set_text(page,0,cases[i].edit,cases[i].edit_len)==0);
        state_expect(name,c->value_len==cases[i].want_len &&
            !memcmp(c->value,cases[i].want,cases[i].want_len) && c->value[c->value_len]=='\0');
        os64_page_request_t req;
        os64_page_what_t what={OS64_PAGE_ACTIVATE_CONTROL,1,0,0};
        state_expect(name,os64_page_activate(page,what,&req)==OS64_PAGE_NAVIGATE &&
            req.body_len==strlen(cases[i].body) && !memcmp(req.body,cases[i].body,req.body_len));
        os64_page_request_free(&req);
        state_expect(name,os64_page_reset(page,0)==0 && c->value_len==initial_len &&
            !memcmp(c->value,initial,initial_len));
        os64_page_free(page);os64_html_document_free(doc);
    }
    static const char *enctypes[]={"text/plain","multipart/form-data"};
    for (size_t i=0;i<2;i++) {
        char html[256];
        snprintf(html,sizeof(html),"<form method=post enctype=%s><input name=q><button>Go</button></form>",enctypes[i]);
        os64_html_document_t *doc=parse(html,"utf-8");
        os64_page_t *page=os64_page_build(doc,kPage,NULL);
        state_expect("byte span: text edit",os64_page_set_text(page,0,"a\0b",3)==0);
        os64_page_request_t req;os64_page_what_t what={OS64_PAGE_ACTIVATE_CONTROL,1,0,0};
        state_expect("byte span: raw submission",os64_page_activate(page,what,&req)==OS64_PAGE_NAVIGATE);
        const char *needle=i==0 ? "q=a\0b\r\n" : "\r\n\r\na\0b\r\n";
        size_t len=i==0 ? 7 : 9;bool found=false;
        for (size_t at=0;at+len<=req.body_len;at++)
            if (!memcmp(req.body+at,needle,len)) found=true;
        state_expect("byte span: serializer retains NUL and suffix",found);
        os64_page_request_free(&req);
        os64_page_free(page);os64_html_document_free(doc);
    }
}

// The edit verbs refuse what the page took away, because every face would
// otherwise have to remember to (PR #98's disabled and readonly findings).
static void state_taken_away(void)
{
    os64_html_document_t *doc = parse(
        "<form method=post>"
        "<input name=ro readonly value=fixed>"                          // 0
        "<input name=off disabled value=gone>"                          // 1
        "<fieldset disabled><input type=checkbox name=box>"             // 2
        "<legend>no</legend></fieldset>"
        "<fieldset disabled><legend><input name=live value=l></legend>" // 3
        "</fieldset>"
        "<select name=pick><option value=a>A<option value=b disabled>B" // 4
        "<optgroup disabled><option value=c>C</optgroup></select>"
        "<select name=one><option value=x>X<option value=y>Y</select>"  // 5
        "<select name=rows size=2><option value=p selected>P"           // 6
        "<option value=q>Q</select>"
        "<select name=gone disabled><option>G<option>H</select>"        // 7
        "<input type=hidden name=h value=v>"                            // 8
        "<input type=submit name=s value=S>"                            // 9
        "<input type=radio name=rad disabled>"                          // 10
        "<textarea name=note readonly>kept</textarea>"                  // 11
        "<input type=checkbox name=tick readonly>"                      // 12
        "<button name=b>Go</button></form>",                            // 13
        "utf-8");
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    state_expect("taken: control count", os64_page_ncontrols(page) == 14);
    state_expect("taken: readonly text",
                 os64_page_set_text(page, 0, "x", 1) == -OS64_PAGE_REASON_READONLY);
    state_value("taken: readonly keeps value", page, 0, "fixed");
    state_expect("taken: disabled text",
                 os64_page_set_text(page, 1, "x", 1) == -OS64_PAGE_REASON_DISABLED);
    state_expect("taken: fieldset-disabled tick",
                 os64_page_set_checked(page, 2, true) == -OS64_PAGE_REASON_DISABLED);
    state_expect("taken: tick stays clear", !os64_page_control(page, 2)->checked);
    state_expect("taken: legend control is live", os64_page_set_text(page, 3, "m", 1) == 0);
    state_expect("taken: disabled option",
                 os64_page_set_chosen(page, 4, 1, true) == -OS64_PAGE_REASON_DISABLED);
    state_expect("taken: optgroup-disabled option",
                 os64_page_set_chosen(page, 4, 2, true) == -OS64_PAGE_REASON_DISABLED);
    state_value("taken: list keeps its answer", page, 4, "a");
    state_expect("taken: disabled option may be put down",
                 os64_page_set_chosen(page, 4, 1, false) == 0);
    state_expect("taken: drop-down cleared", os64_page_set_chosen(page, 5, 0, false) == 0);
    state_value("taken: drop-down is never empty", page, 5, "x");
    state_expect("taken: drop-down moved", os64_page_set_chosen(page, 5, 1, true) == 0 &&
                                           os64_page_set_chosen(page, 5, 1, false) == 0);
    state_value("taken: cleared drop-down holds its first", page, 5, "x");
    state_expect("taken: list box cleared", os64_page_set_chosen(page, 6, 0, false) == 0);
    state_value("taken: list box may hold nothing", page, 6, "");
    state_expect("taken: disabled list",
                 os64_page_set_chosen(page, 7, 1, true) == -OS64_PAGE_REASON_DISABLED);
    state_expect("taken: hidden value takes no typing",
                 os64_page_set_text(page, 8, "x", 1) == -OS64_PAGE_REASON_WRONG_KIND);
    state_expect("taken: submit value takes no typing",
                 os64_page_set_text(page, 9, "x", 1) == -OS64_PAGE_REASON_WRONG_KIND);
    state_expect("taken: disabled radio",
                 os64_page_set_checked(page, 10, true) == -OS64_PAGE_REASON_DISABLED);
    state_expect("taken: readonly textarea",
                 os64_page_set_text(page, 11, "x", 1) == -OS64_PAGE_REASON_READONLY);
    state_expect("taken: readonly means nothing on a tick",
                 os64_page_set_checked(page, 12, true) == 0);
    state_expect("taken: button value takes no typing",
                 os64_page_set_text(page, 13, "x", 1) == -OS64_PAGE_REASON_WRONG_KIND);
    state_expect("taken: refusal has a name",
                 strcmp(os64_page_reason_name(OS64_PAGE_REASON_READONLY), "unknown refusal") != 0);
    os64_page_free(page);
    os64_html_document_free(doc);
}

// A frame names a document, resolved by the same door as an href.
static void state_frames(void)
{
    os64_html_document_t *doc = parse(
        "<head><base href=\"http://other/dir/\"></head>"
        "<frameset><frame name=menu src=\" menu.html#top%20part \">"
        "<frame name=empty><frame src=\"\"></frameset>", "utf-8");
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    state_expect("frames: one link per frame with a src", os64_page_nlinks(page) == 2);
    const os64_page_link_t *link = os64_page_link(page, 0);
    state_expect("frames: resolved against the base",
                 link != NULL && link->href.url != NULL &&
                 strcmp(link->href.url, "http://other/dir/menu.html") == 0 &&
                 link->href.has_fragment && strcmp(link->href.fragment, "top part") == 0);
    link = os64_page_link(page, 1);
    state_expect("frames: an empty src is the base",
                 link != NULL && link->href.url != NULL &&
                 strcmp(link->href.url, "http://other/dir/") == 0);
    os64_page_free(page);
    os64_html_document_free(doc);
}

// An iframe is offered like a frame, but only when it names something: an
// empty src is the blank page, which is nowhere to go.
static void state_iframes(void)
{
    os64_html_document_t *doc = parse(
        "<body><a href=a.html>a</a><iframe src=\"ad.html#x\">fallback</iframe>"
        "<iframe></iframe><iframe src=\" \t\"></iframe>"
        "<iframe src=\"\"></iframe></body>", "utf-8");
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    state_expect("iframes: only the one that names a document", os64_page_nlinks(page) == 2);
    const os64_page_link_t *link = os64_page_link(page, 1);
    state_expect("iframes: resolved like a frame, fragment kept apart",
                 link != NULL && link->node != NULL &&
                 link->node->tag == OS64_HTML_TAG_IFRAME && link->href.url != NULL &&
                 strcmp(link->href.url, "http://host/dir/ad.html") == 0 &&
                 link->href.has_fragment && strcmp(link->href.fragment, "x") == 0);
    state_expect("iframes: the node names its link",
                 link != NULL && os64_page_link_for(page, link->node) == 1);
    os64_page_free(page);
    os64_html_document_free(doc);
}

// Every picture the page names, in tree order, resolved against the base;
// alt kept as written, absent and empty told apart; an empty src listed
// nowhere, because fetching the page itself as a picture is the bug.
static void state_images(void)
{
    os64_html_document_t *doc = parse(
        "<head><base href=\"http://pics/lib/\"></head><body>"
        "<img src=\" cat.png \" alt=\"A cat\">"
        "<img src=dog.gif alt=\"\">"
        "<img src=\"\" alt=empty><img alt=none><img src=\"  \">"
        "<form><input type=IMAGE src=\"/go.png\" name=go></form>"
        "<input type=submit src=x.png>"
        "<img src=\"http://[bad\">"
        "<svg><img src=foreign.png></svg>"
        "<template><img src=inert.png></template>"
        "</body>", "utf-8");
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    state_expect("images: every img and image input that names a src",
                 os64_page_nimages(page) == 5);
    const os64_page_image_t *image = os64_page_image(page, 0);
    state_expect("images: resolved against the base, space trimmed",
                 image != NULL && image->src.url != NULL &&
                 strcmp(image->src.url, "http://pics/lib/cat.png") == 0 &&
                 image->alt != NULL && strcmp(image->alt, "A cat") == 0);
    image = os64_page_image(page, 1);
    state_expect("images: an empty alt is empty, not absent",
                 image != NULL && image->alt != NULL && image->alt[0] == '\0');
    image = os64_page_image(page, 2);
    state_expect("images: an image button is a picture too",
                 image != NULL && image->node != NULL &&
                 image->node->tag == OS64_HTML_TAG_INPUT && image->alt == NULL &&
                 image->src.url != NULL && strcmp(image->src.url, "http://pics/go.png") == 0 &&
                 os64_page_control_for(page, image->node) >= 0);
    image = os64_page_image(page, 3);
    state_expect("images: an address that will not resolve is listed, refused",
                 image != NULL && image->src.url == NULL &&
                 image->src.refused != OS64_PAGE_REASON_OK);
    image = os64_page_image(page, 4);
    state_expect("images: HTML inside a foreign element still counts",
                 image != NULL && image->src.url != NULL &&
                 strcmp(image->src.url, "http://pics/lib/foreign.png") == 0);
    for (int32_t i = 0; i < os64_page_nimages(page); i++) {
        image = os64_page_image(page, i);
        state_expect("images: every node names its image",
                     image != NULL && os64_page_image_for(page, image->node) == i);
    }
    state_expect("images: out of range is NULL",
                 os64_page_image(page, -1) == NULL && os64_page_image(page, 5) == NULL &&
                 os64_page_image_for(page, NULL) < 0 && os64_page_image_for(NULL, NULL) < 0);
    os64_page_free(page);
    os64_html_document_free(doc);
}

// What a page writes in an href is percent-encoded part by part, as every
// browser does: a path in UTF-8, a query in the document's encoding, a
// fragment left for the matcher.
static void state_reference_bytes(void)
{
    static const struct { const char *html, *charset, *url, *fragment; } cases[] = {
        {"<a href='/wiki/H\xC3\xA5kon Wium'>x</a>", "utf-8",
         "http://host/wiki/H%C3%A5kon%20Wium", NULL},
        {"<a href='/a\"b<c>d`e{f}'>x</a>", "utf-8",
         "http://host/a%22b%3Cc%3Ed%60e%7Bf%7D", NULL},
        {"<a href='/s?q=caf\xC3\xA9 \"x\"&k=`{}`'>x</a>", "utf-8",
         "http://host/s?q=caf%C3%A9%20%22x%22&k=`{}`", NULL},
        // A windows-1252 page's query is windows-1252, and a character it
        // cannot hold is the markup that names it, escaped.
        {"<a href='/s?q=caf\xE9\x92&#9731;'>x</a>", "windows-1252",
         "http://host/s?q=caf%E9%92%26%239731%3B", NULL},
        // ...and its path is UTF-8 all the same.
        {"<a href='/caf\xE9'>x</a>", "windows-1252", "http://host/caf%C3%A9", NULL},
        {"<a href='/p#H\xC3\xA5kon'>x</a>", "utf-8", "http://host/p", "H\xC3\xA5kon"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        os64_html_document_t *doc = parse(cases[i].html, cases[i].charset);
        os64_page_t *page = os64_page_build(doc, kPage, NULL);
        const os64_page_link_t *link = os64_page_link(page, 0);
        char name[64];
        snprintf(name, sizeof(name), "reference bytes: case %zu", i);
        state_expect(name, link != NULL && link->href.url != NULL &&
                           strcmp(link->href.url, cases[i].url) == 0);
        if (link != NULL && link->href.url != NULL && strcmp(link->href.url, cases[i].url) != 0)
            fprintf(stderr, "    got %s\n", link->href.url);
        if (cases[i].fragment != NULL)
            state_expect(name, link != NULL && link->href.fragment != NULL &&
                               strcmp(link->href.fragment, cases[i].fragment) == 0);
        os64_page_free(page);
        os64_html_document_free(doc);
    }
}

// Per-control questions answered once. Each of these was N² before: a
// legendless disabled fieldset rescanned its children for every control, a
// radio group was walked whole at every member, and a `dir=auto` subtree
// was rescanned for every `dirname` control under it. At these sizes the
// old code would run for minutes; the suite would visibly stall.
static void state_scale(void)
{
    enum { N = 60000 };
    size_t cap = (size_t)N * 64 + 256, at = 0;
    char *html = malloc(cap);
    if (html == NULL)
        abort();
    at += (size_t)snprintf(html + at, cap - at, "<form method=post><fieldset disabled>");
    for (int i = 0; i < N; i++)
        at += (size_t)snprintf(html + at, cap - at, "<input name=f>");
    at += (size_t)snprintf(html + at, cap - at, "</fieldset>");
    for (int i = 0; i < N; i++)
        at += (size_t)snprintf(html + at, cap - at, "<input type=radio name=r required%s>",
                               i == N - 1 ? " checked" : "");
    at += (size_t)snprintf(html + at, cap - at, "<div dir=auto>");
    for (int i = 0; i < N / 4; i++)
        at += (size_t)snprintf(html + at, cap - at, "<input name=t dirname=d>");
    snprintf(html + at, cap - at, "</div><button>Go</button></form>");
    os64_html_document_t *doc = parse(html, "utf-8");
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    state_expect("scale: builds whole", page != NULL && !os64_page_incomplete(page));
    state_expect("scale: fieldset disables its first and last",
                 os64_page_control(page, 0)->disabled && os64_page_control(page, N - 1)->disabled);
    os64_page_request_t req;
    os64_page_what_t what = {OS64_PAGE_ACTIVATE_CONTROL, os64_page_ncontrols(page) - 1, 0, 0};
    state_expect("scale: submits", os64_page_activate(page, what, &req) == OS64_PAGE_NAVIGATE &&
                                   req.body != NULL);
    os64_page_request_free(&req);
    os64_page_free(page);
    os64_html_document_free(doc);
    free(html);

    // The rules, on small pages: a fieldset's exception is its OWN first
    // legend, nested ones included.
    doc = parse("<form><fieldset disabled><p>x</p>"
                "<legend><fieldset disabled><legend><input name=a></legend>"
                "<input name=b></fieldset></legend>"
                "<legend><input name=c></legend><input name=d></fieldset></form>", "utf-8");
    page = os64_page_build(doc, kPage, NULL);
    state_expect("fieldset: inner legend live", !os64_page_control(page, 0)->disabled);
    state_expect("fieldset: inner body off", os64_page_control(page, 1)->disabled);
    state_expect("fieldset: second legend off", os64_page_control(page, 2)->disabled);
    state_expect("fieldset: outer body off", os64_page_control(page, 3)->disabled);
    os64_page_free(page);
    os64_html_document_free(doc);

    // A group judged where its first unbarred member stands, over every
    // member: a disabled checked radio still answers it.
    doc = parse("<form><input name=t><input type=radio name=r disabled required>"
                "<input type=radio name=r><button>Go</button></form>", "utf-8");
    page = os64_page_build(doc, kPage, NULL);
    what = (os64_page_what_t){OS64_PAGE_ACTIVATE_CONTROL, 3, 0, 0};
    state_expect("radio group judged past a barred head",
                 os64_page_activate(page, what, &req) == OS64_PAGE_NOTHING &&
                 req.reason == OS64_PAGE_REASON_INVALID && req.control == 2);
    os64_page_request_free(&req);
    os64_page_free(page);
    os64_html_document_free(doc);
    doc = parse("<form><input type=radio name=r disabled required checked>"
                "<input type=radio name=r><button>Go</button></form>", "utf-8");
    page = os64_page_build(doc, kPage, NULL);
    what = (os64_page_what_t){OS64_PAGE_ACTIVATE_CONTROL, 2, 0, 0};
    state_expect("radio group answered by a disabled tick",
                 os64_page_activate(page, what, &req) == OS64_PAGE_NAVIGATE);
    os64_page_request_free(&req);
    os64_page_free(page);
    os64_html_document_free(doc);
    doc = parse("<form><input name=t><input type=radio name=r><input type=radio name=r required>"
                "<button>Go</button></form>", "utf-8");
    page = os64_page_build(doc, kPage, NULL);
    what = (os64_page_what_t){OS64_PAGE_ACTIVATE_CONTROL, 3, 0, 0};
    state_expect("radio group failure names its first member",
                 os64_page_activate(page, what, &req) == OS64_PAGE_NOTHING &&
                 req.reason == OS64_PAGE_REASON_INVALID && req.control == 1);
    os64_page_request_free(&req);
    os64_page_free(page);
    os64_html_document_free(doc);
}

// An empty `label` is no label; the words fall back to the text.
static void state_option_labels(void)
{
    os64_html_document_t *doc = parse("<select><option label='' value=x>Fallback"
                                      "<option label=Short value=y>Long</select>", "utf-8");
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    const os64_page_control_t *c = os64_page_control(page, 0);
    state_expect("option: empty label falls back to text",
                 c != NULL && c->noptions == 2 && strcmp(c->options[0].label, "Fallback") == 0 &&
                 strcmp(c->options[1].label, "Short") == 0);
    os64_page_free(page);
    os64_html_document_free(doc);
}

static void state_cases(void)
{
    state_option_labels();
    state_scale();
    state_taken_away();
    state_frames();
    state_iframes();
    state_images();
    state_reference_bytes();
    state_group_reset();
    state_sanitizers();
    state_tick_values();
    state_byte_spans();
    state_selection_cases();
    state_allocation_failures();
    state_large_group();
    state_long_reset();
}
