// test_libpage_cases.inc — the corpus, family by family.
//
// Every case is written from the standard's own steps, and the ones marked
// with a round number are the review findings of the first draft, kept here
// because a finding that is only a fixed line of code is a finding that can
// come back. Each names the steps of the step list it exercises.
//
// Nothing here draws anything. That is the point of the seam: these cases
// are about what LEAVES THE MACHINE, so they outlive whatever is drawing.

// ── A. The base and reference resolution ────────────────────────────────

static void family_a(void)
{
    run((PCase){.steps = "A4,G1",
                .name = "A: relative link",
                .html = "<a href=next.html>x</a>",
                .link = true,
                .url = "http://host/dir/next.html"});
    run((PCase){.steps = "A9",
                .name = "A: absolute link ignores the base",
                .html = "<base href='http://elsewhere/'><a href='http://other/p'>x</a>",
                .link = true,
                .url = "http://other/p"});
    run((PCase){.steps = "A10",
                .name = "A: scheme-relative link",
                .html = "<a href='//cdn/x'>x</a>",
                .link = true,
                .url = "http://cdn/x"});
    run((PCase){.steps = "A11",
                .name = "A: query-only reference keeps the path",
                .html = "<a href='?page=2'>x</a>",
                .link = true,
                .url = "http://host/dir/page.html?page=2"});
    run((PCase){.steps = "A12",
                .name = "A: dot segments",
                .html = "<a href='../up/./there.html'>x</a>",
                .link = true,
                .url = "http://host/up/there.html"});
    run((PCase){.steps = "A1",
                .name = "A: the first base wins",
                .html = "<base href='http://one/a/'><base href='http://two/b/'><a href=x>l</a>",
                .link = true,
                .url = "http://one/a/x"});
    // Round 5. `<base href="">` resolves to the document's own address and
    // OUTRANKS a later base that names somewhere else.
    run((PCase){.steps = "A2",
                .name = "A: an empty base href is the document (R5)",
                .html = "<base href=''><base href='http://two/b/'><a href=x>l</a>",
                .link = true,
                .url = "http://host/dir/x"});
    // Round 7. libhtml leaves a misplaced base where it found it, so the
    // body is as good a place to look as the head.
    run((PCase){.steps = "A3",
                .name = "A: a base in the body (R7)",
                .html = "<body><p>text<base href='http://late/'><a href=x>l</a>",
                .link = true,
                .url = "http://late/x"});
    // Round 3. An EMPTY reference names the document it appeared on, which
    // is every reload link on the old web.
    run((PCase){.steps = "A5,G2,G3",
                .name = "A: an empty href is this page (R3)",
                .html = "<a href=''>reload</a>",
                .link = true,
                .verdict = OS64_PAGE_NAVIGATE,
                .url = "http://host/dir/page.html?old=1"});
    // The `:443` finding: a port the scheme already implies is not spelled,
    // or every link on the page names an address nobody wrote.
    run((PCase){.steps = "A6",
                .name = "A: a default port is never spelled",
                .html = "<a href='https://host:443/p'>x</a>",
                .link = true,
                .url = "https://host/p"});
    run((PCase){.steps = "A6",
                .name = "A: a default port on the document is dropped",
                .html = "<a href=x>l</a>",
                .document = "http://host:80/dir/page.html",
                .link = true,
                .url = "http://host/dir/x"});
    run((PCase){.steps = "A6",
                .name = "A: a port that is not the default rides along",
                .html = "<a href=x>l</a>",
                .document = "http://host:8080/dir/page.html",
                .link = true,
                .url = "http://host:8080/dir/x"});
    run((PCase){.steps = "A7",
                .name = "A: a reference that does not resolve is refused",
                .html = "<a href='http://bad host/p'>x</a>",
                .link = true,
                .verdict = OS64_PAGE_REFUSED,
                .reason = OS64_PAGE_REASON_BAD_ACTION,
                .check_reason = true});
    run((PCase){.steps = "A16",
                .name = "A: an opaque scheme is kept verbatim",
                .html = "<a href='mailto:someone@example.com'>x</a>",
                .link = true,
                .url = "mailto:someone@example.com",
                .check_facts = true,
                .leaves = false});
    run((PCase){.steps = "A13",
                .name = "A: a fragment rides beside the address",
                .html = "<a href='http://other/p#place'>x</a>",
                .link = true,
                .url = "http://other/p",
                .fragment = "place"});
    run((PCase){.steps = "A14,G5",
                .name = "A: the fragment is matched decoded (R7)",
                .html = "<h2 id='a name'>h</h2><a href='#a%20name'>x</a>",
                .link = true,
                .verdict = OS64_PAGE_FRAGMENT,
                .fragment = "a name",
                .check_anchor = true,
                .want_anchor = true});
    run((PCase){.steps = "A15",
                .name = "A: a base that will not resolve leaves the document's",
                .html = "<base href='http://bad host/'><a href=x>l</a>",
                .link = true,
                .url = "http://host/dir/x"});
}

static void long_reference(void)
{
    // A8. An action too long to resolve was once sent to the page's own host
    // instead, which is a different host to be wrong about (R7).
    static char html[4096];
    size_t at = (size_t)snprintf(html, sizeof(html), "<a href='http://host/");
    while (at < sizeof(html) - 32)
        html[at++] = 'x';
    snprintf(html + at, sizeof(html) - at, "'>x</a>");
    run((PCase){.steps = "A8",
                .name = "A: a reference too long is refused, not redirected (R7)",
                .html = html,
                .link = true,
                .verdict = OS64_PAGE_REFUSED,
                .reason = OS64_PAGE_REASON_TOO_LONG,
                .check_reason = true});
}

// ── B. Form owner ───────────────────────────────────────────────────────

static void owner_is(const char *name, const os64_page_t *page, int32_t control, int32_t want)
{
    const os64_page_control_t *c = os64_page_control(page, control);
    if (c == NULL)
        fail(name, "no control %d", control);
    else if (c->form != want)
        fail(name, "control %d belongs to form %d, want %d", control, c->form, want);
}

static void check_nearest(const char *name, const os64_page_t *page)
{
    owner_is(name, page, 0, 0);
}

static void check_named(const char *name, const os64_page_t *page)
{
    owner_is(name, page, 0, 0);
}

static void check_named_nothing(const char *name, const os64_page_t *page)
{
    owner_is(name, page, 0, -1);
}

static void check_named_outranks(const char *name, const os64_page_t *page)
{
    // Two forms: the control sits inside the first and names the second.
    owner_is(name, page, 0, 1);
}

static void check_id_is_not_a_form(const char *name, const os64_page_t *page)
{
    owner_is(name, page, 0, -1);
}

static void check_no_form(const char *name, const os64_page_t *page)
{
    owner_is(name, page, 0, -1);
}

static void family_b(void)
{
    model("B1", "B: the nearest form ancestor", "<form><div><input name=q></div></form>",
          check_nearest);
    model("B2", "B: form= names a form anywhere", "<form id=f action=/s></form><input form=f name=q>",
          check_named);
    // Round 4. A `form=` that matches nothing leaves the control in NO form,
    // and NOT in the nearest ancestor.
    model("B3", "B: form= naming nothing is no form (R4)",
          "<form id=f></form><form><input form=nope name=q></form>", check_named_nothing);
    // The standard looks the ID up first and asks whether it is a form
    // second, so an earlier element with that id wins and loses.
    model("B5", "B: an id that is not a form is no form",
          "<div id=f></div><form id=f></form><input form=f name=q>", check_id_is_not_a_form);
    model("B6", "B: a control outside every form", "<input name=q>", check_no_form);
}

// The B4 case needs a control that sits in one form and names another, which
// the markup above cannot spell in one line; this is the real shape.
static void family_b_outranks(void)
{
    model("B4", "B: a control inside one form naming another",
          "<form action=/first><input form=second name=q></form><form id=second action=/s></form>",
          check_named_outranks);
}

// ── C. The submitter, and what it overrules ─────────────────────────────

static void family_c(void)
{
    run((PCase){.steps = "C1,F5",
                .name = "C: input type=submit",
                .html = "<form action=/s><input name=q value=cat><input type=submit></form>",
                .index = 1,
                .url = "http://host/s?q=cat"});
    // Round 3. A `button` element defaults to type submit, and this rule
    // finished for `input` and not for `button` crashed on a search box.
    run((PCase){.steps = "C2",
                .name = "C: a bare button submits (R3)",
                .html = "<form action=/s><input name=q value=cat><button>Go</button></form>",
                .index = 1,
                .url = "http://host/s?q=cat"});
    run((PCase){.steps = "C4",
                .name = "C: type=button submits nothing",
                .html = "<form action=/s><button type=button>x</button></form>",
                .index = 0,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_NO_SUBMISSION,
                .check_reason = true});
    run((PCase){.steps = "C5",
                .name = "C: type=reset clears the form",
                .html = "<form action=/s><button type=reset>x</button></form>",
                .index = 0,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_RESET,
                .check_reason = true});
    run((PCase){.steps = "C14",
                .name = "C: a disabled control does nothing",
                .html = "<form action=/s><input type=submit disabled></form>",
                .index = 0,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_DISABLED,
                .check_reason = true});
    run((PCase){.steps = "C6,C7",
                .name = "C: implicit submission fires the default button",
                .html = "<form action=/s><input name=q value=cat>"
                        "<input type=submit name=a value=1><input type=submit name=b value=2>"
                        "</form>",
                .implicit = true,
                .index = 0,
                .url = "http://host/s?q=cat&a=1"});
    // Round 6. A default button inside a subtree the page marked `hidden` is
    // drawn nowhere and is still the button a form submits through, so its
    // method and action apply even though no cursor can reach it.
    run((PCase){.steps = "C8,C17",
                .name = "C: a hidden default button still applies (R6)",
                .html = "<form action=/s><input name=q value=cat>"
                        "<div hidden><input type=submit formmethod=post></div></form>",
                .implicit = true,
                .index = 0,
                .url = "http://host/s",
                .body = "q=cat"});
    // Round 7. And one bound to the form by `form=` from document scope.
    run((PCase){.steps = "C9",
                .name = "C: a default button bound by form= (R7)",
                .html = "<form id=f action=/s><input name=q value=cat></form>"
                        "<input type=submit form=f formaction=/other>",
                .implicit = true,
                .index = 0,
                .url = "http://host/other?q=cat"});
    // Predicted after round seven, and open in the first draft: a DISABLED
    // default button means a browser does nothing at all.
    run((PCase){.steps = "C10",
                .name = "C: a disabled default button does nothing",
                .html = "<form action=/s><input name=pw value=secret>"
                        "<input type=submit disabled></form>",
                .implicit = true,
                .index = 0,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_DEFAULT_BUTTON_DISABLED,
                .check_reason = true});
    run((PCase){.steps = "C11",
                .name = "C: two blocking fields and no button does nothing",
                .html = "<form action=/s><input name=a><input name=b></form>",
                .implicit = true,
                .index = 0,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_IMPLICIT_BLOCKED,
                .check_reason = true});
    run((PCase){.steps = "C12",
                .name = "C: one blocking field and no button submits",
                .html = "<form action=/s><input name=q value=cat></form>",
                .implicit = true,
                .index = 0,
                .url = "http://host/s?q=cat"});
    // A DEPARTURE from the first draft, whose round-5 truth pass let a
    // readonly box stand aside: the standard's rule is about the TYPE.
    run((PCase){.steps = "C13",
                .name = "C: readonly does not exempt a blocking field",
                .html = "<form action=/s><input name=a readonly><input name=b></form>",
                .implicit = true,
                .index = 0,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_IMPLICIT_BLOCKED,
                .check_reason = true});
    run((PCase){.steps = "C15",
                .name = "C: formaction overrules",
                .html = "<form action=/s><input name=q value=1>"
                        "<input type=submit formaction=/other></form>",
                .index = 1,
                .url = "http://host/other?q=1"});
    // Round 4. `formaction=""` is the PAGE's own address, so presence
    // decides and not the string.
    run((PCase){.steps = "C16",
                .name = "C: formaction=\"\" is this page (R4)",
                .html = "<form action=/s><input name=q value=1>"
                        "<input type=submit formaction=''></form>",
                .index = 1,
                .url = "http://host/dir/page.html?q=1"});
    // Round 2. A GET form with a `formmethod=post` button is a POST, and
    // sending it as a GET puts whatever was typed in a server's log.
    run((PCase){.steps = "C17,F7",
                .name = "C: formmethod=post on a GET form (R2)",
                .html = "<form action=/s><input name=pw value=secret>"
                        "<input type=submit formmethod=post></form>",
                .index = 1,
                .url = "http://host/s",
                .body = "pw=secret",
                .type = "application/x-www-form-urlencoded"});
    run((PCase){.steps = "C18,E10",
                .name = "C: formenctype overrules",
                .html = "<form action=/s method=post><input name=q value=cat>"
                        "<input type=submit formenctype='text/plain'></form>",
                .index = 1,
                .body = "q=cat\r\n",
                .type = "text/plain"});
    run((PCase){.steps = "C19,I2",
                .name = "C: formnovalidate overrules",
                .html = "<form action=/s><input name=q required>"
                        "<input type=submit formnovalidate></form>",
                .index = 1,
                .url = "http://host/s?q="});
}

// ── D. Constructing the entry list ──────────────────────────────────────

static void family_d(void)
{
    // The truth pass of round 5: visible controls, `type=hidden` and
    // everything under a `hidden` attribute are ONE list in tree order,
    // because the order is what a server sees when two share a name.
    run((PCase){.steps = "D1,D2,D3",
                .name = "D: one list in tree order (R5)",
                .html = "<form action=/s><input type=hidden name=a value=1>"
                        "<input name=b value=2><div hidden><input name=c value=3></div>"
                        "<input type=hidden name=d value=4><input type=submit></form>",
                .index = 4,
                .url = "http://host/s?a=1&b=2&c=3&d=4"});
    run((PCase){.steps = "D4",
                .name = "D: a datalist's input is not an answer",
                .html = "<form action=/s><datalist><input name=hint value=x></datalist>"
                        "<input name=q value=1><input type=submit></form>",
                .index = 2,
                .url = "http://host/s?q=1"});
    run((PCase){.steps = "D5",
                .name = "D: a disabled control is skipped",
                .html = "<form action=/s><input name=a value=1 disabled>"
                        "<input name=b value=2><input type=submit></form>",
                .index = 2,
                .url = "http://host/s?b=2"});
    // Round 7 found the HIDDEN path without the fieldset rule, which is the
    // half-rule this library exists to make impossible.
    run((PCase){.steps = "D6",
                .name = "D: a disabled fieldset disables its controls (R7)",
                .html = "<form action=/s><fieldset disabled><input name=a value=1>"
                        "<div hidden><input name=b value=2></div></fieldset>"
                        "<input name=c value=3><input type=submit></form>",
                .index = 3,
                .url = "http://host/s?c=3"});
    run((PCase){.steps = "D7",
                .name = "D: the first legend of a disabled fieldset is exempt",
                .html = "<form action=/s><fieldset disabled>"
                        "<legend><b>t</b><input name=a value=1></legend>"
                        "<legend><input name=b value=2></legend>"
                        "<input name=c value=3></fieldset><input type=submit></form>",
                .index = 3,
                .url = "http://host/s?a=1"});
    run((PCase){.steps = "D8",
                .name = "D: a submit that is not the submitter is skipped",
                .html = "<form action=/s><input type=submit name=a value=1>"
                        "<input type=submit name=b value=2></form>",
                .index = 1,
                .url = "http://host/s?b=2"});
    run((PCase){.steps = "D9,D15",
                .name = "D: ticks",
                .html = "<form action=/s><input type=checkbox name=a checked>"
                        "<input type=checkbox name=b><input type=radio name=c checked>"
                        "<input type=submit></form>",
                .index = 3,
                .url = "http://host/s?a=on&c=on"});
    // Round 5. `value=""` asked for the empty string, and sending `on` for
    // it picks a different branch on the far side.
    run((PCase){.steps = "D16",
                .name = "D: a tick with value=\"\" (R5)",
                .html = "<form action=/s><input type=checkbox name=a value='' checked>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "http://host/s?a="});
    run((PCase){.steps = "D10",
                .name = "D: a control with no name is not sent",
                .html = "<form action=/s><input value=1><input name=b value=2>"
                        "<input type=submit></form>",
                .index = 2,
                .url = "http://host/s?b=2"});
    run((PCase){.steps = "D11,D14",
                .name = "D: a select sends every selected option",
                .html = "<form action=/s><select name=pick multiple>"
                        "<option selected>one<option value=2 selected>two<option>three"
                        "</select><input type=submit></form>",
                .index = 1,
                .url = "http://host/s?pick=one&pick=2"});
    run((PCase){.steps = "D12",
                .name = "D: a disabled option is never sent",
                .html = "<form action=/s><select name=pick multiple>"
                        "<option selected disabled>one<option value=2 selected>two"
                        "</select><input type=submit></form>",
                .index = 1,
                .url = "http://host/s?pick=2"});
    run((PCase){.steps = "D13",
                .name = "D: a disabled optgroup disables its options",
                .html = "<form action=/s><select name=pick multiple>"
                        "<optgroup disabled><option value=1 selected>a</optgroup>"
                        "<option value=2 selected>b</select><input type=submit></form>",
                .index = 1,
                .url = "http://host/s?pick=2"});
    // Round 7. A radio GROUP is one name under one OWNER, searched over the
    // whole document: two groups with one name in two forms are two answers,
    // and a local server's access log is what proved it.
    run((PCase){.steps = "D17",
                .name = "D: two radio groups, one name, two forms (R7)",
                .html = "<form action=/A><input type=radio name=x value=1 checked>"
                        "<input type=submit></form>"
                        "<form action=/B><input type=radio name=x value=2 checked>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "http://host/A?x=1"});
    run((PCase){.steps = "D17",
                .name = "D: and the second form sends its own (R7)",
                .html = "<form action=/A><input type=radio name=x value=1 checked>"
                        "<input type=submit></form>"
                        "<form action=/B><input type=radio name=x value=2 checked>"
                        "<input type=submit></form>",
                .index = 3,
                .url = "http://host/B?x=2"});
    run((PCase){.steps = "C3,D18",
                .name = "D: an image button sends where it was clicked",
                .html = "<form action=/s><input type=image name=go></form>",
                .index = 0,
                .x = 12,
                .y = 34,
                .url = "http://host/s?go.x=12&go.y=34"});
    // Round 7. A nameless image button sends bare `x` and `y`, which is
    // settled before the "no name, nothing sent" rule can reach it.
    run((PCase){.steps = "D19",
                .name = "D: a nameless image button sends x and y (R7)",
                .html = "<form action=/s><input type=image value=ignored></form>",
                .index = 0,
                .x = 1,
                .y = 2,
                .url = "http://host/s?x=1&y=2"});
    run((PCase){.steps = "D20",
                .name = "D: a file input with no file",
                .html = "<form action=/s><input type=file name=up><input type=submit></form>",
                .index = 1,
                .url = "http://host/s?up="});
    // Round 7 found this one going out EMPTY: the field is the FORM's answer
    // about its encoding, whatever the page left in its value.
    run((PCase){.steps = "D21,E3",
                .name = "D: _charset_ sends the encoding (R7)",
                .html = "<meta charset=utf-8><form action=/s>"
                        "<input type=hidden name=_charset_ value=''>"
                        "<input type=submit></form>",
                .charset = NULL,
                .index = 1,
                .url = "http://host/s?_charset_=utf-8"});
    run((PCase){.steps = "D24",
                .name = "D: a readonly control IS sent",
                .html = "<form action=/s><input name=q value=fixed readonly>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "http://host/s?q=fixed"});
    // Round 5's `%0A`: a lone newline in a textarea goes out as CRLF, which
    // is what every other browser sends and what a server splitting on CRLF
    // needs in order to see two lines.
    run((PCase){.steps = "D25",
                .name = "D: line breaks become CRLF (R5)",
                .html = "<form action=/s><textarea name=t>one\ntwo</textarea>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "http://host/s?t=one%0D%0Atwo"});
    run((PCase){.steps = "D22",
                .name = "D: dirname sends a direction",
                .html = "<form action=/s><input name=q value=hi dirname=q.dir>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "http://host/s?q=hi&q.dir=ltr"});
    // The Unicode table's reason for existing: `dir=auto` means read the
    // text and see, and the FIRST STRONG character decides. The value here
    // is a phone number and then a Hebrew alef.
    run((PCase){.steps = "D23",
                .name = "D: dir=auto reads the first strong character",
                .charset = "utf-8",
                .html = "<form action=/s><input name=q dir=auto value='555 \xd7\x90'"
                        " dirname=q.dir><input type=submit></form>",
                .index = 1,
                .url = "http://host/s?q=555+%D7%90&q.dir=rtl"});
    run((PCase){.steps = "D23",
                .name = "D: dir=auto with Latin text first",
                .charset = "utf-8",
                .html = "<form action=/s><input name=q dir=auto value='A \xd7\x90'"
                        " dirname=q.dir><input type=submit></form>",
                .index = 1,
                .url = "http://host/s?q=A+%D7%90&q.dir=ltr"});
}

// ── E. The encoding ─────────────────────────────────────────────────────

static void family_e(void)
{
    run((PCase){.steps = "E4",
                .name = "E: the urlencoded safe set",
                .html = "<form action=/s><input name=k value='a*b-c.d_e~f'>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "http://host/s?k=a*b-c.d_e%7Ef"});
    run((PCase){.steps = "E5",
                .name = "E: a space is a plus",
                .html = "<form action=/s><input name=k value='two words'>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "http://host/s?k=two+words"});
    run((PCase){.steps = "E1",
                .name = "E: accept-charset picks a label",
                .html = "<form action=/s accept-charset='windows-1252'>"
                        "<input type=hidden name=_charset_><input type=submit></form>",
                .index = 1,
                .url = "http://host/s?_charset_=windows-1252"});
    run((PCase){.steps = "E2",
                .name = "E: an unsupported label is passed over",
                .html = "<form action=/s accept-charset='shift_jis utf-8'>"
                        "<input type=hidden name=_charset_><input type=submit></form>",
                .index = 1,
                .url = "http://host/s?_charset_=utf-8"});
    // The join between the two questions, and the classic mistake: an
    // unencodable code point becomes `&#NNN;` and THOSE BYTES are then
    // percent-encoded like any others.
    run((PCase){.steps = "E6,E7",
                .name = "E: a numeric reference is percent-encoded too",
                .charset = "utf-8",
                .html = "<form action=/s accept-charset='windows-1252'>"
                        "<input name=k value='\xce\xa9'><input type=submit></form>",
                .index = 1,
                .url = "http://host/s?k=%26%23937%3B"});
    // And a code point windows-1252 CAN hold goes out as its one byte.
    run((PCase){.steps = "E6",
                .name = "E: windows-1252 holds the euro sign",
                .charset = "utf-8",
                .html = "<form action=/s accept-charset='windows-1252'>"
                        "<input name=k value='\xe2\x82\xac'><input type=submit></form>",
                .index = 1,
                .url = "http://host/s?k=%80"});
    run((PCase){.steps = "E12",
                .name = "E: a UTF-16 selection becomes UTF-8",
                .html = "<form action=/s accept-charset='utf-16'>"
                        "<input type=hidden name=_charset_><input type=submit></form>",
                .index = 1,
                .url = "http://host/s?_charset_=utf-8"});
    run((PCase){.steps = "E11",
                .name = "E: an invalid enctype is urlencoded",
                .html = "<form action=/s method=post enctype='application/json'>"
                        "<input name=q value=1><input type=submit></form>",
                .index = 1,
                .body = "q=1",
                .type = "application/x-www-form-urlencoded"});
    run((PCase){.steps = "E10",
                .name = "E: text/plain writes lines",
                .html = "<form action=/s method=post enctype='text/plain'>"
                        "<input name=a value=1><input name=b value='two words'>"
                        "<input type=submit></form>",
                .index = 2,
                .body = "a=1\r\nb=two words\r\n",
                .type = "text/plain"});
}

static void multipart_case(void)
{
    // E8 and E9. The boundary is derived from the content, so the bytes are
    // the same twice and a case can name them. What is checked here is the
    // SHAPE: one part per entry, a file part carrying a filename, and the
    // terminator.
    checks++;
    cover("E8,E9");
    const char *html = "<form action=/s method=post enctype='multipart/form-data'>"
                       "<input name=a value=1><input type=file name=up>"
                       "<input type=submit></form>";
    os64_html_document_t *doc = parse(html, "utf-8");
    os64_page_t *page = doc != NULL ? os64_page_build(doc, kPage, NULL) : NULL;
    if (page == NULL) {
        fail("E: multipart", "no page");
        os64_html_document_free(doc);
        return;
    }
    os64_page_what_t what = {OS64_PAGE_ACTIVATE_CONTROL, 2, 0, 0};
    os64_page_request_t request;
    const char *name = "E: multipart";
    if (os64_page_activate(page, what, &request) != OS64_PAGE_NAVIGATE) {
        fail(name, "not a navigation");
    } else {
        const char *body = request.body;
        char boundary[128] = "";
        if (request.content_type == NULL ||
            strncmp(request.content_type, "multipart/form-data; boundary=", 30) != 0)
            fail(name, "content type |%s|",
                 request.content_type != NULL ? request.content_type : "(none)");
        else
            snprintf(boundary, sizeof(boundary), "%s", request.content_type + 30);
        char want[1024];
        snprintf(want, sizeof(want),
                 "--%s\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n1\r\n"
                 "--%s\r\nContent-Disposition: form-data; name=\"up\"; filename=\"\"\r\n"
                 "Content-Type: application/octet-stream\r\n\r\n\r\n"
                 "--%s--\r\n",
                 boundary, boundary, boundary);
        if (body == NULL || request.body_len != strlen(want) ||
            memcmp(body, want, request.body_len) != 0)
            fail(name, "body\n|%.*s|\nwant\n|%s|", (int)request.body_len,
                 body != NULL ? body : "", want);
    }
    os64_page_request_free(&request);
    os64_page_free(page);
    os64_html_document_free(doc);
}

// ── F. Method, action, and where the data goes ──────────────────────────

static void family_f(void)
{
    run((PCase){.steps = "F1",
                .name = "F: an invalid method is get",
                .html = "<form action=/s method=put><input name=q value=1>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "http://host/s?q=1"});
    // Predicted after round seven, and open in the first draft: a `dialog`
    // form sends NOTHING to any server, and the first draft put the password
    // in a query string.
    run((PCase){.steps = "F2",
                .name = "F: method=dialog sends nothing",
                .html = "<form action=/s method=dialog><input name=pw value=secret>"
                        "<input type=submit></form>",
                .index = 1,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_DIALOG,
                .check_reason = true});
    run((PCase){.steps = "F3",
                .name = "F: formmethod=dialog sends nothing",
                .html = "<form action=/s><input name=pw value=secret>"
                        "<input type=submit formmethod=dialog></form>",
                .index = 1,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_DIALOG,
                .check_reason = true});
    run((PCase){.steps = "F4",
                .name = "F: no action means this page",
                .html = "<form><input name=q value=1><input type=submit></form>",
                .index = 1,
                .url = "http://host/dir/page.html?q=1"});
    // Round 2. A form's `#name` was dropped where a link's was kept, which
    // is one rule with two answers.
    run((PCase){.steps = "F6",
                .name = "F: the action's fragment is kept (R2)",
                .html = "<form action='/s#results'><input name=q value=1>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "http://host/s?q=1",
                .fragment = "results"});
    run((PCase){.steps = "F7,J1",
                .name = "F: POST carries a body",
                .html = "<form action='http://plain/s' method=post><input name=pw value=secret>"
                        "<input type=submit></form>",
                .document = "https://host/dir/page.html",
                .index = 1,
                .url = "http://plain/s",
                .body = "pw=secret",
                .check_facts = true,
                .downgrade = true,
                .leaves = true});
    run((PCase){.steps = "F8",
                .name = "F: a data: GET mutates the query",
                .html = "<form action='data:text/plain,hi'><input name=q value=1>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "data:text/plain,hi?q=1",
                .check_facts = true,
                .downgrade = false,
                .leaves = false});
    run((PCase){.steps = "F9",
                .name = "F: a data: POST carries no data",
                .html = "<form action='data:text/plain,hi' method=post><input name=q value=1>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "data:text/plain,hi"});
    run((PCase){.steps = "F10",
                .name = "F: ftp carries no data",
                .html = "<form action='ftp://files/pub' method=post><input name=q value=1>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "ftp://files/pub",
                .check_facts = true,
                .downgrade = false,
                .leaves = true});
    run((PCase){.steps = "F11",
                .name = "F: javascript carries no data",
                .html = "<form action='javascript:void 0'><input name=q value=1>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "javascript:void 0"});
    // `mailto:` GET puts the list in the headers, and a space there is
    // `%20`: these are header values, not a query a server decodes.
    run((PCase){.steps = "F12,J4",
                .name = "F: mailto GET writes headers",
                .html = "<form action='mailto:a@b'><input name=subject value='two words'>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "mailto:a@b?subject=two%20words",
                .check_facts = true,
                .downgrade = false,
                .leaves = false});
    run((PCase){.steps = "F13",
                .name = "F: mailto POST writes a body header",
                .html = "<form action='mailto:a@b' method=post><input name=q value=1>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "mailto:a@b?body=q=1"});
    run((PCase){.steps = "F14",
                .name = "F: another scheme is refused by name",
                .html = "<form action='gopher://host/1/'><input name=q value=1>"
                        "<input type=submit></form>",
                .index = 1,
                .verdict = OS64_PAGE_REFUSED,
                .reason = OS64_PAGE_REASON_SCHEME,
                .check_reason = true});
}

static void long_query(void)
{
    // F15. A form with enough fields builds a query longer than an address
    // may be, and that is the same refusal family A gives a reference that
    // will not fit — not an address handed on for something further down to
    // choke on with less to say about why.
    static char html[8192];
    size_t at = (size_t)snprintf(html, sizeof(html), "<form action=/s>");
    for (int32_t i = 0; at < sizeof(html) - 64; i++)
        at += (size_t)snprintf(html + at, sizeof(html) - at,
                               "<input name=field%d value=padding>", i);
    snprintf(html + at, sizeof(html) - at, "<input type=submit></form>");
    run((PCase){.steps = "F15",
                .name = "F: a query longer than an address is refused",
                .html = html,
                .index = -1,
                .verdict = OS64_PAGE_REFUSED,
                .reason = OS64_PAGE_REASON_TOO_LONG,
                .check_reason = true});
}

// ── G. Links and fragment navigation ────────────────────────────────────

static void family_g(void)
{
    run((PCase){.steps = "G2,G5",
                .name = "G: a link into this document is a move",
                .html = "<h2 id=part>h</h2><a href='#part'>x</a>",
                .link = true,
                .verdict = OS64_PAGE_FRAGMENT,
                .fragment = "part",
                .check_anchor = true,
                .want_anchor = true});
    run((PCase){.steps = "G3",
                .name = "G: # is the top",
                .html = "<a href='#'>x</a>",
                .link = true,
                .verdict = OS64_PAGE_FRAGMENT,
                .fragment = "",
                .check_anchor = true,
                .want_anchor = false});
    run((PCase){.steps = "G4",
                .name = "G: #top is the top when nothing claims it",
                .html = "<a href='#top'>x</a>",
                .link = true,
                .verdict = OS64_PAGE_FRAGMENT,
                .fragment = "top",
                .check_anchor = true,
                .want_anchor = false});
    run((PCase){.steps = "G4",
                .name = "G: ...unless something does",
                .html = "<div id=top>t</div><a href='#top'>x</a>",
                .link = true,
                .verdict = OS64_PAGE_FRAGMENT,
                .check_anchor = true,
                .want_anchor = true});
    run((PCase){.steps = "G6",
                .name = "G: an old-style a-name claims a fragment",
                .html = "<a name=here></a><a href='#here'>x</a>",
                .link = true,
                .verdict = OS64_PAGE_FRAGMENT,
                .check_anchor = true,
                .want_anchor = true});
    run((PCase){.steps = "G7",
                .name = "G: an unclaimed fragment moves nowhere",
                .html = "<a href='#nosuch'>x</a>",
                .link = true,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_NO_ANCHOR,
                .check_reason = true});
    run((PCase){.steps = "G8",
                .name = "G: an area is a link",
                .html = "<map><area href=target.html></map>",
                .link = true,
                .url = "http://host/dir/target.html"});
    run((PCase){.steps = "G1,J3,J5",
                .name = "G: a link elsewhere is a fetch",
                .html = "<a href='https://other/p'>x</a>",
                .link = true,
                .url = "https://other/p",
                .check_facts = true,
                .downgrade = false,
                .leaves = true});
}

// ── H. Value sanitization ───────────────────────────────────────────────

static const char *h_name;
static const char *h_want;
static int32_t h_control;

static void check_value(const char *name, const os64_page_t *page)
{
    const os64_page_control_t *c = os64_page_control(page, h_control);
    if (c == NULL)
        fail(name, "no control %d", h_control);
    else if (strlen(h_want) != c->value_len || memcmp(c->value, h_want, c->value_len) != 0)
        fail(name, "value |%.*s|, want |%s|", (int)c->value_len, c->value, h_want);
}

static void value_case(const char *step_ids, const char *name, const char *html, int32_t control,
                       const char *want)
{
    h_name = name;
    h_want = want;
    h_control = control;
    model(step_ids, name, html, check_value);
}

static void family_h(void)
{
    value_case("H1", "H: a single-line field loses its line breaks",
               "<input name=q value='a\nb'>", 0, "ab");
    value_case("H2", "H: url strips its ends", "<input type=url name=q value=' x '>", 0, "x");
    value_case("H3", "H: a multiple email strips each address",
               "<input type=email name=q multiple value='a@b , c@d'>", 0, "a@b,c@d");
    value_case("H4", "H: hidden is verbatim", "<input type=hidden name=q value=' keep me '>", 0,
               " keep me ");
    value_case("H5", "H: a textarea's value is its text",
               "<textarea name=t>  two   spaces  </textarea>", 0, "  two   spaces  ");
    value_case("H6", "H: a number that is not one holds nothing",
               "<input type=number name=q value=abc>", 0, "");
    value_case("H6", "H: a number that is one is kept",
               "<input type=number name=q value=-1.5e3>", 0, "-1.5e3");
    value_case("H7", "H: a date that is not one holds nothing",
               "<input type=date name=q value='2026-02-30'>", 0, "");
    value_case("H8", "H: a leap day is a date", "<input type=date name=q value='2024-02-29'>", 0,
               "2024-02-29");
    value_case("H8", "H: and not in a common year", "<input type=date name=q value='2025-02-29'>",
               0, "");
    value_case("H9", "H: a week is checked against the year",
               "<input type=week name=q value='2026-W53'>", 0, "");
    value_case("H9", "H: a year with 53 weeks has one",
               "<input type=week name=q value='2026-W52'>", 0, "2026-W52");
    value_case("H10", "H: a range with no value holds the middle",
               "<input type=range name=q min=1 max=10>", 0, "5.5");
    value_case("H10", "H: a range's default span",
               "<input type=range name=q>", 0, "50");
    value_case("H10", "H: a range whose max is below its min",
               "<input type=range name=q min=5 max=1>", 0, "5");
    value_case("H11", "H: a colour that is not one is black",
               "<input type=color name=q value=red>", 0, "#000000");
    value_case("H11", "H: a colour is lowercased",
               "<input type=color name=q value='#AABBCC'>", 0, "#aabbcc");
    value_case("H12", "H: an unknown type is text",
               "<input type=quantum name=q value='a\nb'>", 0, "ab");
    value_case("H13", "H: a select holds its first selected option",
               "<select name=p><option>one<option selected>two</select>", 0, "two");
}

static void edit_text(os64_page_t *page)
{
    os64_page_set_text(page, 0, "typed", 5);
}

static void tick_second(os64_page_t *page)
{
    os64_page_set_checked(page, 1, true);
}

static void choose_third(os64_page_t *page)
{
    os64_page_set_chosen(page, 0, 2, true);
}

static void type_too_much(os64_page_t *page)
{
    os64_page_set_text(page, 0, "far too many characters", 23);
}

static void family_h_edits(void)
{
    run((PCase){.steps = "H14",
                .name = "H: an edit replaces the page's value",
                .html = "<form action=/s><input name=q value=page>"
                        "<input type=submit></form>",
                .index = 1,
                .setup = edit_text,
                .url = "http://host/s?q=typed"});
    // Ticking one radio unticks its group, because the page asked one
    // question and a server must not be told two answers.
    run((PCase){.steps = "D17",
                .name = "H: ticking one radio unticks the group",
                .html = "<form action=/s><input type=radio name=x value=1 checked>"
                        "<input type=radio name=x value=2><input type=submit></form>",
                .index = 2,
                .setup = tick_second,
                .url = "http://host/s?x=2"});
    run((PCase){.steps = "D11",
                .name = "H: choosing in a one-line list replaces the choice",
                .html = "<form action=/s><select name=p><option value=1 selected>a"
                        "<option value=2>b<option value=3>c</select>"
                        "<input type=submit></form>",
                .index = 1,
                .setup = choose_third,
                .url = "http://host/s?p=3"});
}

// ── I. Constraint validation ────────────────────────────────────────────

static void family_i(void)
{
    run((PCase){.steps = "I1",
                .name = "I: a required empty field blocks",
                .html = "<form action=/s><input name=q required><input type=submit></form>",
                .index = 1,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_INVALID,
                .check_reason = true});
    run((PCase){.steps = "I2",
                .name = "I: novalidate skips it",
                .html = "<form action=/s novalidate><input name=q required>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "http://host/s?q="});
    run((PCase){.steps = "I3",
                .name = "I: a required radio group needs one ticked",
                .html = "<form action=/s><input type=radio name=x value=1 required>"
                        "<input type=radio name=x value=2><input type=submit></form>",
                .index = 2,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_INVALID,
                .check_reason = true});
    run((PCase){.steps = "I3",
                .name = "I: ...and any member's tick satisfies it",
                .html = "<form action=/s><input type=radio name=x value=1 required>"
                        "<input type=radio name=x value=2 checked><input type=submit></form>",
                .index = 2,
                .url = "http://host/s?x=2"});
    run((PCase){.steps = "I4",
                .name = "I: a required select needs a real option",
                .html = "<form action=/s><select name=p required>"
                        "<option value='' selected>choose<option value=1>a</select>"
                        "<input type=submit></form>",
                .index = 1,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_INVALID,
                .check_reason = true});
    run((PCase){.steps = "I5",
                .name = "I: readonly bars a control from validation",
                .html = "<form action=/s><input name=q required readonly>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "http://host/s?q="});
    // A `readonly` spelled on a tick means nothing, so it cannot excuse one
    // from a rule either: the bar applies only where the attribute does.
    run((PCase){.steps = "I9",
                .name = "I: readonly on a tick bars nothing",
                .html = "<form action=/s><input type=checkbox name=a required readonly>"
                        "<input type=submit></form>",
                .index = 1,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_INVALID,
                .check_reason = true});
    run((PCase){.steps = "I6",
                .name = "I: disabled bars it too (and sends nothing)",
                .html = "<form action=/s><input name=q required disabled>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "http://host/s?"});
    // The length limits hang on the DIRTY value: a page that ships a value
    // longer than its own maxlength still submits it.
    run((PCase){.steps = "I7",
                .name = "I: maxlength applies to what was typed",
                .html = "<form action=/s><input name=q maxlength=4>"
                        "<input type=submit></form>",
                .index = 1,
                .setup = type_too_much,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_INVALID,
                .check_reason = true});
    run((PCase){.steps = "I7",
                .name = "I: ...and not to what the page shipped",
                .html = "<form action=/s><input name=q maxlength=4 value=toolong>"
                        "<input type=submit></form>",
                .index = 1,
                .url = "http://host/s?q=toolong"});
    // Validation runs BEFORE the method is looked at, which is the
    // standard's order and shows up on a dialog form.
    run((PCase){.steps = "I8",
                .name = "I: validation precedes the dialog check",
                .html = "<form action=/s method=dialog><input name=q required>"
                        "<input type=submit></form>",
                .index = 1,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_INVALID,
                .check_reason = true});
}

// ── J. Facts, not questions ─────────────────────────────────────────────

static void family_j(void)
{
    // Round 1 and round 2. The downgrade is judged against where the PAGE
    // came from and never against its base, which a page can move to http
    // while the page that collected the values stays encrypted.
    run((PCase){.steps = "J1",
                .name = "J: an https page to an http action is a downgrade (R1)",
                .html = "<form action='http://plain/s'><input name=pw value=secret>"
                        "<input type=submit></form>",
                .document = "https://host/dir/page.html",
                .index = 1,
                .check_facts = true,
                .downgrade = true,
                .leaves = true});
    run((PCase){.steps = "J1",
                .name = "J: a moved base does not hide a downgrade (R2)",
                .html = "<base href='http://plain/'><form action=/s>"
                        "<input name=pw value=secret><input type=submit></form>",
                .document = "https://host/dir/page.html",
                .index = 1,
                .url = "http://plain/s?pw=secret",
                .check_facts = true,
                .downgrade = true,
                .leaves = true});
    run((PCase){.steps = "J2",
                .name = "J: https to https is not a downgrade",
                .html = "<form action=/s><input name=q value=1><input type=submit></form>",
                .document = "https://host/dir/page.html",
                .index = 1,
                .url = "https://host/s?q=1",
                .check_facts = true,
                .downgrade = false,
                .leaves = true});
}

// ── K. The navigation a document declares ───────────────────────────────

static const char *k_want_url;
static uint32_t k_want_seconds;
static bool k_want_declared, k_want_this_document, k_want_spelled;

static void check_refresh(const char *name, const os64_page_t *page)
{
    const os64_page_refresh_t *refresh = os64_page_refresh(page);
    if ((refresh != NULL) != k_want_declared) {
        fail(name, "%s a refresh", refresh != NULL ? "declares" : "declares no");
        return;
    }
    if (refresh == NULL)
        return;
    if (refresh->seconds != k_want_seconds)
        fail(name, "%u seconds, want %u", refresh->seconds, k_want_seconds);
    if (k_want_url != NULL && strcmp(refresh->url.url, k_want_url) != 0)
        fail(name, "to |%s|, want |%s|", refresh->url.url, k_want_url);
    if (refresh->names_this_document != k_want_this_document)
        fail(name, "names_this_document %d", (int)refresh->names_this_document);
    if (refresh->url.spelled != k_want_spelled)
        fail(name, "spelled %d", (int)refresh->url.spelled);
}

static void refresh_case(const char *step_ids, const char *name, const char *html,
                         uint32_t seconds, const char *url, bool this_document, bool spelled)
{
    k_want_declared = url != NULL || this_document;
    k_want_seconds = seconds;
    k_want_url = url;
    k_want_this_document = this_document;
    k_want_spelled = spelled;
    model(step_ids, name, html, check_refresh);
}

static void declares_none(const char *step_ids, const char *name, const char *html)
{
    k_want_declared = false;
    k_want_url = NULL;
    model(step_ids, name, html, check_refresh);
}

// THE PAGE THAT STARTED IT. DuckDuckGo's HTML results wrap every link in a
// click logger that answers 200 with no redirect and no text: a script for
// a browser that runs one, and a refresh in `noscript` for a browser that
// does not. This is those bytes, and we are the reader the fallback was
// written for.
#define DDG_TARGET "https://en.wikipedia.org/wiki/Hobbyist_operating_system"
static const char *kDuckDuckGoRedirect =
    "<html><head><meta name='referrer' content='origin'></head><body>"
    "<script language='JavaScript'>window.parent.location.replace(\"" DDG_TARGET "\");</script>"
    "<noscript><META http-equiv='refresh' content=\"0;URL=" DDG_TARGET "\"></noscript>"
    "</body></html>";

static void family_k(void)
{
    refresh_case("K1,K15,K18", "K: DuckDuckGo's noscript redirect", kDuckDuckGoRedirect, 0,
                 DDG_TARGET, false, true);
    // And the whole point of the exercise: a page with nothing to DRAW is
    // not a page with nothing to SAY.
    run((PCase){.steps = "K16",
                .name = "K: following it goes where the page said",
                .html = kDuckDuckGoRedirect,
                .refresh = true,
                .url = DDG_TARGET,
                .check_facts = true,
                .downgrade = false,
                .leaves = true});
    refresh_case("K1", "K: a plain refresh",
                 "<meta http-equiv=refresh content='5;url=/next'>", 5, "http://host/next", false,
                 true);
    // The form the pragma was INVENTED for, before anything could script a
    // reload: a scoreboard asking for itself again in thirty seconds.
    refresh_case("K4,K14", "K: a time with no address is this page",
                 "<meta http-equiv=refresh content='30'>", 30,
                 "http://host/dir/page.html?old=1", true, false);
    refresh_case("K14", "K: and so is naming this page outright",
                 "<meta http-equiv=refresh content='0;url=page.html?old=1'>", 0,
                 "http://host/dir/page.html?old=1", true, true);
    refresh_case("K13", "K: the address resolves against the base",
                 "<base href='http://elsewhere/deep/'>"
                 "<meta http-equiv=refresh content='0;url=../up'>",
                 0, "http://elsewhere/up", false, true);
    refresh_case("K2", "K: the first valid one wins",
                 "<meta http-equiv=refresh content='1;url=/first'>"
                 "<meta http-equiv=refresh content='2;url=/second'>",
                 1, "http://host/first", false, true);
    // An INVALID pragma sets nothing, which is what leaves a later valid one
    // free to win — the standard's flag is set by success, not by presence.
    refresh_case("K3", "K: an invalid one does not block a later one",
                 "<meta http-equiv=refresh content='url=/first'>"
                 "<meta http-equiv=refresh content='2;url=/second'>",
                 2, "http://host/second", false, true);
    refresh_case("K6", "K: a leading full stop is a time of zero",
                 "<meta http-equiv=refresh content='.5;url=/next'>", 0, "http://host/next", false,
                 true);
    refresh_case("K7", "K: digits and stops past the integer are ignored",
                 "<meta http-equiv=refresh content='1.5.9;url=/next'>", 1, "http://host/next",
                 false, true);
    refresh_case("K9", "K: the url keyword is optional",
                 "<meta http-equiv=refresh content='0;/next'>", 0, "http://host/next", false, true);
    refresh_case("K9", "K: and whitespace serves as the separator",
                 "<meta http-equiv=refresh content='0 /next'>", 0, "http://host/next", false, true);
    refresh_case("K11", "K: quotes are stripped",
                 "<meta http-equiv=refresh content=\"0;url='/next'\">", 0, "http://host/next",
                 false, true);
    refresh_case("K11", "K: and an unmatched quote is not",
                 "<meta http-equiv=refresh content=\"0;url='/next\">", 0, "http://host/next",
                 false, true);
    // THE STANDARD'S OWN QUIRK, and the reason this grammar is not tidied:
    // leaving the keyword scan early keeps the candidate that was in hand
    // BEFORE the scan started, so this asks for an address called `used=x`.
    refresh_case("K10", "K: a partial url keyword is part of the address",
                 "<meta http-equiv=refresh content='0;used=x'>", 0, "http://host/dir/used=x",
                 false, true);
    refresh_case("K10", "K: and so is a bare `url` with no equals",
                 "<meta http-equiv=refresh content='0;url'>", 0, "http://host/dir/url", false,
                 true);
    declares_none("K5", "K: no digits and no full stop",
                  "<meta http-equiv=refresh content='url=/next'>");
    declares_none("K5", "K: nor an empty content",
                  "<meta http-equiv=refresh content=''>");
    declares_none("K8", "K: a separator that is not one",
                  "<meta http-equiv=refresh content='0:/next'>");
    declares_none("K12", "K: an address that will not resolve",
                  "<meta http-equiv=refresh content='0;url=http://bad host/x'>");
    declares_none("K1", "K: a pragma that is not refresh",
                  "<meta http-equiv=content-type content='text/html; charset=utf-8'>");
    declares_none("K1", "K: a meta with no content",
                  "<meta http-equiv=refresh>");
    declares_none("K1", "K: an ordinary meta",
                  "<meta name=refresh content='0;url=/next'>");
    run((PCase){.steps = "K17",
                .name = "K: a page that declares none has none to follow",
                .html = "<p>text",
                .refresh = true,
                .verdict = OS64_PAGE_NOTHING,
                .reason = OS64_PAGE_REASON_NO_REFRESH,
                .check_reason = true});
    // The privacy question a refresh raises and a redirect does not: this
    // hop is invisible to the fetch, so the FACE has to ask about it.
    run((PCase){.steps = "K16,J1",
                .name = "K: a refresh off an encrypted page is a downgrade",
                .html = "<meta http-equiv=refresh content='0;url=http://plain/x'>",
                .document = "https://host/dir/page.html",
                .refresh = true,
                .url = "http://plain/x",
                .check_facts = true,
                .downgrade = true,
                .leaves = true});
}

// ── The pathological page, and the allocation sweep ─────────────────────

static const char *kPathological =
    "<html><body><ul><li><ul><li><ul><li><ul><li><ul><li><pre>"
    "  kept   spaces  \n</pre></ul></ul></ul></ul></ul>"
    "<table><form action=/outer><tr><td><input name=intable value=1>"
    "<form action=/inner><input name=inner value=2></form></td></tr></form></table>"
    "<b><i>unclosed"
    "<form action=/s accept-charset='x-nope windows-1252' method=post>"
    "<select name=empty></select>"
    "<textarea name=t readonly>line\nline</textarea>"
    "<fieldset disabled><legend><b>bold</b><input name=inlegend value=3></legend>"
    "<input name=off value=4></fieldset>"
    "<div hidden><input type=submit formmethod=dialog></div>"
    "<input type=image name=go><input type=submit name=go2>"
    "</form>";

static void pathological(void)
{
    // It exists to be walked without falling over, and to hold every shape
    // that has ever produced a finding at once.
    checks++;
    os64_html_document_t *doc = parse(kPathological, "utf-8");
    os64_page_t *page = doc != NULL ? os64_page_build(doc, kPage, NULL) : NULL;
    if (page == NULL) {
        fail("pathological", "no page");
        os64_html_document_free(doc);
        return;
    }
    // Its hidden default button carries `formmethod=dialog`, so an implicit
    // submission of that form sends NOTHING — the shape of the three
    // findings predicted after round seven, in one page.
    int32_t last = os64_page_ncontrols(page) - 1;
    for (int32_t i = 0; i <= last; i++) {
        os64_page_what_t what = {OS64_PAGE_ACTIVATE_IMPLICIT, i, 3, 4};
        os64_page_request_t request;
        os64_page_activate(page, what, &request);
        os64_page_request_free(&request);
        what.how = OS64_PAGE_ACTIVATE_CONTROL;
        os64_page_activate(page, what, &request);
        os64_page_request_free(&request);
    }
    for (int32_t i = 0; i < os64_page_nlinks(page); i++) {
        os64_page_what_t what = {OS64_PAGE_ACTIVATE_LINK, i, 0, 0};
        os64_page_request_t request;
        os64_page_activate(page, what, &request);
        os64_page_request_free(&request);
    }
    os64_page_free(page);
    os64_html_document_free(doc);
}

// THE SWEEP HAS TO REACH THE PATHS THAT ALLOCATE. The pathological page is
// mostly refusals, and a page that refuses allocates almost nothing — so
// these pages submit for real, across all three enctypes and the schemes
// that each do something different with the data, and each of them carries
// a value that has to be transcoded and a name that has to be normalised.
static const char *const kSweepPages[] = {
    "<form action=/s method=post enctype='multipart/form-data' accept-charset='windows-1252'>"
    "<input name=a value='two words \xce\xa9'><textarea name=t>one\ntwo</textarea>"
    "<input type=file name=up><select name=p multiple><option selected>one<option selected>two"
    "</select><input type=image name=go><input type=submit></form>",
    "<form action='mailto:a@b' method=post enctype='text/plain'>"
    "<input name=subject value='two words'><input type=submit></form>",
    "<form action='mailto:a@b'><input name=subject value='two words'>"
    "<input type=submit></form>",
    "<form action='data:text/plain,x'><input name=q value=1><input type=submit></form>",
    "<base href='http://elsewhere/deep/'><a href='../up?q=1#frag'>l</a>"
    "<div id=here></div><a href='#here'>m</a><a href='#nosuch'>n</a>",
    // A refresh resolves an address like anything else, and the sweep is
    // where an allocation on that path fails.
    "<meta http-equiv=refresh content='0;url=/somewhere/else'>"
    "<noscript><meta http-equiv=refresh content='5'></noscript>",
};

// Everything the sweep does, once, so the first pass can count the
// allocations a whole run takes and every pass after it can fail one.
static void exercise(void)
{
    size_t count = sizeof(kSweepPages) / sizeof(kSweepPages[0]);
    for (size_t which = 0; which <= count; which++) {
        // The pathological page first, then every page that submits for real.
        os64_html_document_t *doc =
            parse(which == 0 ? kPathological : kSweepPages[which - 1], "utf-8");
        if (doc == NULL)
            continue;
        os64_page_t *page = os64_page_build(doc, kPage, NULL);
        if (page != NULL) {
            for (int32_t i = 0; i < os64_page_ncontrols(page); i++) {
                os64_page_set_text(page, i, "typed \xce\xa9", 8);
                os64_page_set_checked(page, i, true);
                os64_page_set_chosen(page, i, 0, true);
                os64_page_what_t what = {OS64_PAGE_ACTIVATE_CONTROL, i, 1, 2};
                os64_page_request_t request;
                os64_page_activate(page, what, &request);
                os64_page_request_free(&request);
                what.how = OS64_PAGE_ACTIVATE_IMPLICIT;
                os64_page_activate(page, what, &request);
                os64_page_request_free(&request);
            }
            for (int32_t i = 0; i < os64_page_nlinks(page); i++) {
                os64_page_what_t what = {OS64_PAGE_ACTIVATE_LINK, i, 0, 0};
                os64_page_request_t request;
                os64_page_activate(page, what, &request);
                os64_page_request_free(&request);
            }
            os64_page_what_t refresh = {OS64_PAGE_ACTIVATE_REFRESH, 0, 0, 0};
            os64_page_request_t request;
            os64_page_activate(page, refresh, &request);
            os64_page_request_free(&request);
            os64_page_reset(page, 0);
            os64_page_free(page);
        }
        os64_html_document_free(doc);
    }
}

static void allocation_sweep(void)
{
    // EVERY ALLOCATION FAILS IN TURN, and what must never happen is a leak,
    // a crash, or a request that points at something it does not own. A page
    // that came out short says so through os64_page_incomplete; it does not
    // pretend to be whole.
    //
    // The count comes from a pass with nothing failing, because a pass with
    // an early failure allocates almost nothing — reading the ceiling off
    // one of those would sweep a handful of allocations and call it done.
    allocations = 0;
    fail_at = 0;
    exercise();
    size_t worst = allocations;
    for (size_t at = 1; at <= worst; at++) {
        allocations = 0;
        fail_at = at;
        exercise();
        if (live != 0) {
            fail_at = 0;
            fprintf(stderr, "FAIL sweep: %zu allocations leaked with failure at %zu\n", live, at);
            failures++;
            return;
        }
    }
    fail_at = 0;
    printf("libpage sweep: each of %zu allocations failed in turn, nothing leaked\n", worst);
}

static void cases(void)
{
    family_a();
    long_reference();
    family_b();
    family_b_outranks();
    family_c();
    family_d();
    family_e();
    multipart_case();
    family_f();
    long_query();
    family_g();
    family_h();
    family_h_edits();
    family_i();
    family_j();
    family_k();
    pathological();
}
