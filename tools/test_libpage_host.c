// test_libpage_host.c — markup in, the REQUEST out, checked on the host.
//
// THIS FILE IS THE DURABLE ARTEFACT. The first draft of this work stored its
// specification reading inside a renderer marked throwaway, and sixty-two
// review findings' worth of it would have gone with the file. A case here
// outlives every front end written over the library: the same markup must
// produce the same bytes whether a page is drawn in cells or in pixels.
//
// It is organised by the families of LIBPAGE.md, and every case NAMES the
// steps of the standard it exercises. The step list is checked in below, so
// "are we conformant" is a number with an auditable denominator rather than
// a verdict to be argued: coverage is steps named by a case over steps
// listed, printed per family at the end.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html/html.h"
#include "page/page.h"

// libos64's formatter is linked for the URL resolver's sake and reaches for
// the write syscall on printing paths nothing here calls. Answering "all of
// it went out" keeps the link honest without inventing an output.
int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    (void)handle;
    (void)buf;
    return (int64_t)len;
}

// ── Allocation, and failing it on purpose ───────────────────────────────

static size_t allocations, fail_at, live;

void *os64_malloc(size_t size)
{
    allocations++;
    if (fail_at != 0 && allocations >= fail_at)
        return NULL;
    void *at = malloc(size != 0 ? size : 1);
    if (at != NULL)
        live++;
    return at;
}

void *os64_calloc(size_t count, size_t size)
{
    void *at = os64_malloc(count * size);
    if (at != NULL)
        memset(at, 0, count * size);
    return at;
}

void *os64_realloc(void *ptr, size_t size)
{
    allocations++;
    if (fail_at != 0 && allocations >= fail_at)
        return NULL;
    void *at = realloc(ptr, size != 0 ? size : 1);
    if (at != NULL && ptr == NULL)
        live++;
    return at;
}

void os64_free(void *ptr)
{
    if (ptr != NULL)
        live--;
    free(ptr);
}

// ── The steps, per family ───────────────────────────────────────────────
//
// Each is one rule of the standard that libpage decides. A case names the
// ones it exercises; anything with no case is what the coverage number is
// counting, so a step added here without a case LOWERS it on purpose.

typedef struct {
    const char *id;
    const char *rule;
    bool covered;
} PStep;

static PStep steps[] = {
    // A. The base and reference resolution (§4.2.3, URL §5)
    {"A1", "the first base with an href wins", false},
    {"A2", "an empty base href is the document's own address", false},
    {"A3", "a base in the body counts, wherever libhtml kept it", false},
    {"A4", "a relative reference resolves against the base", false},
    {"A5", "an empty reference names the document itself", false},
    {"A6", "a scheme-default port is never spelled into a link", false},
    {"A7", "a reference that does not resolve is a refusal", false},
    {"A8", "a reference longer than an address may be is a refusal", false},
    {"A9", "an absolute reference ignores the base", false},
    {"A10", "a scheme-relative reference takes the base's scheme", false},
    {"A11", "a query-only reference keeps the base's path", false},
    {"A12", "dot segments are resolved away", false},
    {"A13", "a fragment is carried beside the address, never in it", false},
    {"A14", "the fragment is matched decoded", false},
    {"A15", "a base that will not resolve leaves the document's own", false},
    {"A16", "an opaque scheme is kept as it was written", false},

    // B. Form owner (§4.10.17.3)
    {"B1", "the nearest form ancestor owns a control", false},
    {"B2", "form= names a form by id anywhere in the document", false},
    {"B3", "a form= naming nothing leaves the control in no form", false},
    {"B4", "form= outranks the nearest ancestor", false},
    {"B5", "an id that names a non-form leaves the control in no form", false},
    {"B6", "a control outside every form belongs to none", false},
    {"B7", "a parser-inserted owner can be outside the ancestor chain", false},

    // C. The submitter (§4.10.21.2, §4.10.21.3)
    {"C1", "input type=submit is a submitter", false},
    {"C2", "a button element with no type is a submitter", false},
    {"C3", "input type=image is a submitter", false},
    {"C4", "a button with type=button submits nothing", false},
    {"C5", "a button with type=reset clears the form", false},
    {"C6", "implicit submission fires the form's default button", false},
    {"C7", "the default button is the first in tree order", false},
    {"C8", "a default button inside a hidden subtree still applies", false},
    {"C9", "a default button bound by form= from document scope applies", false},
    {"C10", "a disabled default button does nothing", false},
    {"C11", "no submit button and two blocking fields does nothing", false},
    {"C12", "no submit button and one blocking field submits the form", false},
    {"C13", "readonly does not exempt a field from blocking", false},
    {"C14", "a disabled control activated does nothing", false},
    {"C15", "formaction overrules the form's action", false},
    {"C16", "formaction=\"\" is the page's own address", false},
    {"C17", "formmethod overrules the form's method", false},
    {"C18", "formenctype overrules the form's enctype", false},
    {"C19", "formnovalidate overrules the form's validation", false},

    // D. Constructing the entry list (§4.10.21.4)
    {"D1", "entries go out in tree order", false},
    {"D2", "a hidden input is in the same list as a visible one", false},
    {"D3", "a control under a hidden attribute is still submitted", false},
    {"D4", "a control with a datalist ancestor is skipped", false},
    {"D5", "a disabled control is skipped", false},
    {"D6", "a disabled fieldset ancestor disables its controls", false},
    {"D7", "the first legend of a disabled fieldset is exempt", false},
    {"D8", "a submit control that is not the submitter is skipped", false},
    {"D9", "an unchecked checkbox or radio is skipped", false},
    {"D10", "a control with no name is skipped", false},
    {"D11", "a select sends every selected option", false},
    {"D12", "a disabled option is never sent", false},
    {"D13", "a disabled optgroup disables its options", false},
    {"D14", "an option with no value sends its text", false},
    {"D15", "a tick with no value attribute sends on", false},
    {"D16", "a tick with value=\"\" sends the empty string", false},
    {"D17", "a radio group is one name under one owner", false},
    {"D18", "an image button sends name.x and name.y", false},
    {"D19", "a nameless image button sends x and y", false},
    {"D20", "a file input sends its filename", false},
    {"D21", "_charset_ sends the selected encoding", false},
    {"D22", "dirname sends the field's direction", false},
    {"D23", "dir=auto reads the value's first strong character", false},
    {"D24", "a readonly control is sent", false},
    {"D25", "line breaks are normalised to CRLF", false},

    // E. The encoding (§4.10.21.7-9)
    {"E1", "accept-charset picks the first label we support", false},
    {"E2", "an unsupported label is passed over", false},
    {"E3", "the document's encoding is the fallback", false},
    {"E4", "urlencoded's safe set is alphanumerics and *-._", false},
    {"E5", "urlencoded sends a space as +", false},
    {"E6", "an unencodable code point becomes a numeric reference", false},
    {"E7", "a numeric reference is then percent-encoded", false},
    {"E8", "multipart writes one part per entry", false},
    {"E9", "a multipart file part carries a filename", false},
    {"E10", "text/plain writes name=value lines", false},
    {"E11", "an invalid enctype is urlencoded", false},
    {"E12", "a UTF-16 selection becomes UTF-8 before anything is sent", false},

    // F. Method, action, and where the data goes (§4.10.21.3)
    {"F1", "an absent or invalid method is get", false},
    {"F2", "a dialog method sends nothing", false},
    {"F3", "a formmethod=dialog sends nothing", false},
    {"F4", "an absent action means the document's own address", false},
    {"F5", "http GET replaces the action's query", false},
    {"F6", "the action's fragment is kept", false},
    {"F7", "http POST carries the body and its content type", false},
    {"F8", "a data: GET mutates the query", false},
    {"F9", "a data: POST navigates with no data", false},
    {"F10", "ftp navigates with no data, whatever the method", false},
    {"F11", "javascript navigates with no data", false},
    {"F12", "mailto GET puts the list in the headers, + as %20", false},
    {"F13", "mailto POST puts the list in a body header", false},
    {"F14", "any other scheme is a refusal by name", false},
    {"F15", "a query longer than an address may be is a refusal", false},

    // G. Links and fragment navigation (§4.6, §7.4.2.2)
    {"G1", "a link to another address is a fetch", false},
    {"G2", "a link into this document is a move", false},
    {"G3", "# is the top of the document", false},
    {"G4", "#top is the top when nothing claims the name", false},
    {"G5", "an id claims a fragment before an a-name does", false},
    {"G6", "an old-style a-name claims a fragment", false},
    {"G7", "an unclaimed fragment moves nowhere", false},
    {"G8", "an area with an href is a link", false},

    // H. Value sanitization (§4.10.5.1)
    {"H1", "a single-line field has its line breaks removed", false},
    {"H2", "url and email strip surrounding whitespace too", false},
    {"H3", "a multiple email strips each address", false},
    {"H4", "hidden is verbatim", false},
    {"H5", "a textarea's value is its child text", false},
    {"H6", "a number that does not parse holds nothing", false},
    {"H7", "a date that does not parse holds nothing", false},
    {"H8", "a valid date is kept", false},
    {"H9", "a week is checked against the year's week count", false},
    {"H10", "a range with no value holds the middle of its span", false},
    {"H11", "a colour that is not one becomes black", false},
    {"H12", "an unknown type is text", false},
    {"H13", "a select holds its first selected option", false},
    {"H14", "an edit replaces the page's own value", false},

    // I. Constraint validation (§4.10.20)
    {"I1", "a required empty field blocks submission", false},
    {"I2", "novalidate skips validation", false},
    {"I3", "a required radio group needs one ticked", false},
    {"I4", "a required select needs a real option", false},
    {"I5", "readonly bars a control from validation", false},
    {"I9", "readonly bars nothing where readonly does not apply", false},
    {"I6", "disabled bars a control from validation", false},
    {"I7", "maxlength applies to what a person typed", false},
    {"I8", "validation runs before the method is looked at", false},

    // J. Facts, not questions
    {"J1", "an https page submitting to http is a downgrade", false},
    {"J2", "an https page submitting to https is not", false},
    {"J3", "http and https leave the machine", false},
    {"J4", "mailto and data do not", false},
    {"J5", "the scheme is spelled for the face", false},

    // K. The navigation a document declares (§7.11.3)
    {"K1", "a meta refresh with a time and an address declares one", false},
    {"K2", "the first VALID refresh in tree order wins", false},
    {"K3", "an invalid pragma leaves a later one free to win", false},
    {"K4", "a time with no address names this document", false},
    {"K5", "no digits and no full stop declares nothing", false},
    {"K6", "a leading full stop is a time of zero", false},
    {"K7", "digits and stops past the integer are ignored", false},
    {"K8", "a separator that is not ';' ',' or space declares nothing", false},
    {"K9", "the url keyword is optional", false},
    {"K10", "a partial url keyword becomes part of the address", false},
    {"K11", "a quoted address has its quotes stripped", false},
    {"K12", "an address that will not resolve declares nothing", false},
    {"K13", "the address resolves against the base", false},
    {"K14", "a refresh naming this document is marked as one", false},
    {"K15", "a refresh inside noscript counts", false},
    {"K16", "following a refresh carries the same facts a link does", false},
    {"K17", "a page that declares none has none to follow", false},
    {"K18", "http-equiv is matched without regard to case", false},
};

static PStep *step_named(const char *id)
{
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
        if (strcmp(steps[i].id, id) == 0)
            return &steps[i];
    fprintf(stderr, "FATAL: case names step %s, which is not in the list\n", id);
    exit(2);
}

static void cover(const char *ids)
{
    // A case may name several, comma separated.
    char copy[128];
    snprintf(copy, sizeof(copy), "%s", ids);
    for (char *at = copy; *at != '\0';) {
        char *end = strchr(at, ',');
        if (end != NULL)
            *end = '\0';
        step_named(at)->covered = true;
        if (end == NULL)
            break;
        at = end + 1;
    }
}

// ── Running one case ────────────────────────────────────────────────────

static int checks, failures;
static const char *kPage = "http://host/dir/page.html?old=1";

static void fail(const char *name, const char *fmt, ...)
{
    failures++;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL %s: ", name);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static os64_html_document_t *parse(const char *html, const char *charset)
{
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = charset;
    os64_html_parser_t *p = os64_html_parser_new(&opt);
    if (p == NULL)
        return NULL;
    if (os64_html_parser_feed(p, html, strlen(html)) != 0) {
        os64_html_parser_destroy(p);
        return NULL;
    }
    return os64_html_parser_finish(p);
}

typedef void (*PSetup)(os64_page_t *page);

typedef struct {
    const char *steps, *name, *html, *charset, *document;
    // WHAT WAS ACTIVATED, spelled so that leaving it out means pressing a
    // control: the enum's own zero is a link, and a case that forgot to say
    // would have quietly asked a different question.
    bool link, implicit, refresh;
    // The markup in this file is UTF-8 and is labelled so unless a case is
    // about the encoding ladder itself, which `sniff` hands to libhtml.
    bool sniff;
    // An index counting back from the end when negative; see `run`.
    int32_t index, x, y;
    PSetup setup;
    // What is expected. A NULL string is not checked.
    os64_page_verdict_t verdict;
    const char *url, *fragment, *body, *type;
    os64_page_reason_t reason;
    bool check_reason, want_anchor, check_anchor;
    // Family J's two facts, checked together when asked for, because a case
    // that cares about one of them cares about both.
    bool check_facts, downgrade, leaves;
} PCase;

static const char *verdict_name(os64_page_verdict_t v)
{
    return v == OS64_PAGE_NAVIGATE   ? "NAVIGATE"
           : v == OS64_PAGE_FRAGMENT ? "FRAGMENT"
           : v == OS64_PAGE_NOTHING  ? "NOTHING"
                                     : "REFUSED";
}

static void run(PCase c)
{
    checks++;
    cover(c.steps);
    os64_html_document_t *doc =
        parse(c.html, c.sniff ? c.charset : c.charset != NULL ? c.charset : "utf-8");
    if (doc == NULL) {
        fail(c.name, "the markup would not parse");
        return;
    }
    os64_page_t *page = os64_page_build(doc, c.document != NULL ? c.document : kPage, NULL);
    if (page == NULL) {
        fail(c.name, "no page");
        os64_html_document_free(doc);
        return;
    }
    if (os64_page_incomplete(page))
        fail(c.name, "the model came out incomplete");
    if (c.setup != NULL)
        c.setup(page);
    os64_page_activation_t how = c.link      ? OS64_PAGE_ACTIVATE_LINK
                                 : c.implicit ? OS64_PAGE_ACTIVATE_IMPLICIT
                                 : c.refresh  ? OS64_PAGE_ACTIVATE_REFRESH
                                              : OS64_PAGE_ACTIVATE_CONTROL;
    // A NEGATIVE index counts back from the end, so a case whose markup is
    // generated can name its submit button without counting the fields.
    int32_t index = c.index;
    if (index < 0)
        index += how == OS64_PAGE_ACTIVATE_LINK ? os64_page_nlinks(page)
                                                : os64_page_ncontrols(page);
    os64_page_what_t what = {how, index, c.x, c.y};
    os64_page_request_t request;
    os64_page_verdict_t got = os64_page_activate(page, what, &request);
    if (got != c.verdict)
        fail(c.name, "%s, want %s (reason: %s)", verdict_name(got), verdict_name(c.verdict),
             os64_page_reason_name(request.reason));
    else if (c.check_reason && request.reason != c.reason)
        fail(c.name, "reason |%s|, want |%s|", os64_page_reason_name(request.reason),
             os64_page_reason_name(c.reason));
    if (c.url != NULL && (request.url == NULL || strcmp(request.url, c.url) != 0))
        fail(c.name, "url |%s|, want |%s|", request.url != NULL ? request.url : "(none)", c.url);
    if (c.fragment != NULL &&
        (request.fragment == NULL || strcmp(request.fragment, c.fragment) != 0))
        fail(c.name, "fragment |%s|, want |%s|",
             request.fragment != NULL ? request.fragment : "(none)", c.fragment);
    if (c.body != NULL &&
        (request.body == NULL || request.body_len != strlen(c.body) ||
         memcmp(request.body, c.body, request.body_len) != 0))
        fail(c.name, "body |%.*s|, want |%s|", (int)request.body_len,
             request.body != NULL ? (const char *)request.body : "", c.body);
    if (c.type != NULL && (request.content_type == NULL || strcmp(request.content_type, c.type) != 0))
        fail(c.name, "content type |%s|, want |%s|",
             request.content_type != NULL ? request.content_type : "(none)", c.type);
    if (c.check_anchor && (request.anchor != NULL) != c.want_anchor)
        fail(c.name, "anchor %s", request.anchor != NULL ? "found" : "not found");
    if (c.check_facts && request.downgrade != c.downgrade)
        fail(c.name, "downgrade %d, want %d", (int)request.downgrade, (int)c.downgrade);
    if (c.check_facts && request.leaves_machine != c.leaves)
        fail(c.name, "leaves_machine %d, want %d", (int)request.leaves_machine, (int)c.leaves);
    os64_page_request_free(&request);
    os64_page_free(page);
    os64_html_document_free(doc);
}

// A model case: what the page MEANS, asked without activating anything.
static void model(const char *step_ids, const char *name, const char *html,
                  void (*check)(const char *name, const os64_page_t *page))
{
    checks++;
    cover(step_ids);
    os64_html_document_t *doc = parse(html, "utf-8");
    if (doc == NULL) {
        fail(name, "the markup would not parse");
        return;
    }
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    if (page == NULL) {
        fail(name, "no page");
        os64_html_document_free(doc);
        return;
    }
    check(name, page);
    os64_page_free(page);
    os64_html_document_free(doc);
}

#include "test_libpage_cases.inc"

int main(int argc, char **argv)
{
    bool sweep = argc > 1 && strcmp(argv[1], "--sweep") == 0;
    cases();
    if (sweep)
        allocation_sweep();
    // Coverage, per family, from the step list above.
    printf("libpage: %d cases, %d failed%s\n", checks, failures,
           live != 0 ? " (AND LEAKED)" : "");
    char family = 0;
    int32_t listed = 0, covered = 0;
    char report[512];
    size_t at = 0;
    for (size_t i = 0; i <= sizeof(steps) / sizeof(steps[0]); i++) {
        char next = i < sizeof(steps) / sizeof(steps[0]) ? steps[i].id[0] : 0;
        if (next != family && listed != 0) {
            at += (size_t)snprintf(report + at, sizeof(report) - at, " %c %d/%d", family, covered,
                                   listed);
            listed = covered = 0;
        }
        family = next;
        if (next == 0)
            break;
        listed++;
        if (steps[i].covered)
            covered++;
    }
    printf("libpage coverage:%s\n", report);
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
        if (!steps[i].covered)
            printf("  %s has no case: %s\n", steps[i].id, steps[i].rule);
    if (live != 0)
        fprintf(stderr, "libpage: %zu allocations never freed\n", live);
    return failures != 0 || live != 0 ? 1 : 0;
}
