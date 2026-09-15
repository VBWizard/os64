// activate.c — families F, G and J: THE DOOR.
//
// Everything a submission decides runs through here and nowhere else: who
// submits, whether the page said it may, what the entry list holds, which
// encoding it goes out in, and then the standard's own table of what each
// scheme and method does with the data. The wire never sees anything this
// did not produce.
//
// Family J is the last line of it: a request carries FACTS and no questions.
// `downgrade` says the values are leaving an encrypted page in the clear;
// `leaves_machine` says performing this puts bytes on a network. Whether to
// ask a person about either is the face's business, because that is about a
// person and a terminal and not about a page.

#include "internal.h"
#include "os64/fmt.h"

const char *os64_page_reason_name(os64_page_reason_t reason)
{
    switch (reason) {
    case OS64_PAGE_REASON_OK:
        return "no refusal";
    case OS64_PAGE_REASON_DIALOG:
        return "this form closes a dialog and sends nothing";
    case OS64_PAGE_REASON_DEFAULT_BUTTON_DISABLED:
        return "the button this form would press is disabled";
    case OS64_PAGE_REASON_IMPLICIT_BLOCKED:
        return "this form has no submit button and more than one field";
    case OS64_PAGE_REASON_DISABLED:
        return "the page disabled that control";
    case OS64_PAGE_REASON_NO_FORM:
        return "that control belongs to no form";
    case OS64_PAGE_REASON_INVALID:
        return "a field the form requires is not filled in";
    case OS64_PAGE_REASON_NO_SUBMISSION:
        return "pressing that control submits nothing";
    case OS64_PAGE_REASON_RESET:
        return "that control clears the form";
    case OS64_PAGE_REASON_NO_ANCHOR:
        return "this page has nothing by that name";
    case OS64_PAGE_REASON_NO_REFRESH:
        return "this page asks to be sent nowhere";
    case OS64_PAGE_REASON_BAD_ACTION:
        return "the address it names is not one that resolves";
    case OS64_PAGE_REASON_TOO_LONG:
        return "what it would send is longer than an address may be";
    case OS64_PAGE_REASON_SCHEME:
        return "a form cannot be sent to that kind of address";
    case OS64_PAGE_REASON_BODY_TOO_LONG:
        return "what it would send is larger than this program allows";
    case OS64_PAGE_REASON_NO_MEMORY:
        return "out of memory";
    case OS64_PAGE_REASON_NO_CONTROL:
        return "there is no such control on this page";
    case OS64_PAGE_REASON_WRONG_KIND:
        return "that control does not hold a value of that kind";
    }
    return "unknown refusal";
}

// ── What a request owns ─────────────────────────────────────────────────

static char *keep(const char *s)
{
    if (s == NULL)
        return NULL;
    size_t len = os64_strlen(s);
    char *out = os64_malloc(len + 1);
    if (out != NULL)
        os64_memcpy(out, s, len + 1);
    return out;
}

void os64_page_request_free(os64_page_request_t *request)
{
    if (request == NULL)
        return;
    // String and body storage belongs to the request, including after a
    // refusal. The anchor node is borrowed from the source document.
    os64_free((void *)request->url);
    os64_free((void *)request->fragment);
    os64_free((void *)request->content_type);
    os64_free((void *)request->body);
    os64_memset(request, 0, sizeof(*request));
    request->control = -1;
}

static void request_start(os64_page_request_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    out->control = -1;
}

static os64_page_verdict_t refuse(os64_page_request_t *out, os64_page_reason_t reason)
{
    out->reason = reason;
    return OS64_PAGE_REFUSED;
}

static os64_page_verdict_t nothing(os64_page_request_t *out, os64_page_reason_t reason)
{
    out->reason = reason;
    return OS64_PAGE_NOTHING;
}

// ── J. The facts ────────────────────────────────────────────────────────

static void state_facts(const os64_page_t *page, os64_page_request_t *out)
{
    os64_url_scheme_of(out->url, out->scheme, sizeof(out->scheme));
    // Performing this puts bytes on a network. A `mailto:` hands a draft to
    // something else, a `data:` is the page talking to itself, and a
    // `javascript:` is not an address at all.
    out->leaves_machine = os64_streq(out->scheme, "http") || os64_streq(out->scheme, "https") ||
                          os64_streq(out->scheme, "ftp") || os64_streq(out->scheme, "ws") ||
                          os64_streq(out->scheme, "wss");
    // The page was reached over https and this request is not, so whatever
    // was typed into it goes out where anyone can read it.
    out->downgrade =
        os64_streq(page->document_scheme, "https") && !os64_streq(out->scheme, "https");
}

// ── G and K. A reference the page named ─────────────────────────────────
//
// A LINK AND A DECLARED REFRESH GO THE SAME WAY, because the question is
// the same one: the page named an address, and what happens next depends on
// whether that address is the document we are already in. Two doors here
// would be the rule written twice, which is how a form's `#name` came to be
// dropped where a link's was kept.
static os64_page_verdict_t navigate_ref(const os64_page_t *page, const os64_page_ref_t *ref,
                                        bool same_document, os64_page_request_t *out)
{
    if (ref->refused != OS64_PAGE_REASON_OK)
        return refuse(out, ref->refused);
    if (ref->url == NULL)
        return refuse(out, OS64_PAGE_REASON_BAD_ACTION);

    // A reference that names THIS document and asks for a place inside it is
    // a MOVE and not a fetch, which is what a table of contents on a long
    // article is entirely made of. Without a `#name` it is the same document
    // asked for again, which is a fetch: that is what a reload IS.
    if (same_document && ref->has_fragment) {
        const char *name = ref->fragment != NULL ? ref->fragment : "";
        out->has_fragment = true;
        out->fragment = keep(name);
        if (out->fragment == NULL)
            return refuse(out, OS64_PAGE_REASON_NO_MEMORY);
        os64_page_reason_t reason = os64_page_resolve_fragment(page, name, &out->anchor);
        if (reason == OS64_PAGE_REASON_OK)
            return OS64_PAGE_FRAGMENT;
        return reason == OS64_PAGE_REASON_NO_ANCHOR ? nothing(out, reason) : refuse(out, reason);
    }

    out->url = keep(ref->url);
    if (out->url == NULL)
        return refuse(out, OS64_PAGE_REASON_NO_MEMORY);
    if (ref->has_fragment) {
        out->has_fragment = true;
        out->fragment = keep(ref->fragment != NULL ? ref->fragment : "");
        if (out->fragment == NULL)
            return refuse(out, OS64_PAGE_REASON_NO_MEMORY);
    }
    out->method = OS64_PAGE_METHOD_GET;
    state_facts(page, out);
    // WHICH SCHEMES A BROWSER FOLLOWS IS THE FACE'S LIST AND NOT THIS
    // LIBRARY'S: the standard's own table refuses an unknown scheme for a
    // form SUBMISSION and says nothing about a link, so the scheme is stated
    // and whoever can act on it decides.
    return OS64_PAGE_NAVIGATE;
}

static os64_page_verdict_t follow_link(const os64_page_t *page, int32_t index,
                                       os64_page_request_t *out)
{
    const os64_page_link_t *link = os64_page_link(page, index);
    if (link == NULL)
        return refuse(out, OS64_PAGE_REASON_NO_CONTROL);
    return navigate_ref(page, &link->href, link->same_document, out);
}

static os64_page_verdict_t follow_refresh(const os64_page_t *page, os64_page_request_t *out)
{
    const os64_page_refresh_t *refresh = os64_page_refresh(page);
    if (refresh == NULL)
        return nothing(out, OS64_PAGE_REASON_NO_REFRESH);
    return navigate_ref(page, &refresh->url, refresh->names_this_document, out);
}

// ── F. Where the data goes ──────────────────────────────────────────────

// Replace an address's query, which is what a GET form does to its action.
// The first '?' after the scheme is where a query starts — a host cannot
// hold one, so this is as true of `mailto:a@b?x` as of `http://h/p?x`.
//
// A form with enough fields can build a query longer than an address may be,
// and the answer is the same refusal family A gives a reference that will not
// fit: `OS64_URL_REF_MAX` is what os64 means by "as long as an address gets",
// and a caller handed something past it could only fail further down with
// less to say about why. NULL for either reason; the caller asks which.
static char *with_query(const char *url, const char *query, bool append, bool *too_long)
{
    *too_long = false;
    size_t head = 0;
    while (url[head] != '\0' && url[head] != '?')
        head++;
    size_t tail = os64_strlen(url + head);
    size_t query_len = os64_strlen(query);
    size_t want = head + (append ? tail : 0) + query_len + 2;
    if (want > OS64_URL_REF_MAX) {
        *too_long = true;
        return NULL;
    }
    char *out = os64_malloc(want);
    if (out == NULL)
        return NULL;
    os64_memcpy(out, url, head);
    size_t at = head;
    if (append && tail > 1) {
        // The address already asks something; this is one more question.
        os64_memcpy(out + at, url + head, tail);
        at += tail;
        out[at++] = '&';
    } else {
        out[at++] = '?';
    }
    os64_memcpy(out + at, query, query_len);
    out[at + query_len] = '\0';
    return out;
}

// `mailto:` GET puts the entry list in the address's headers, and a space
// there is `%20` rather than `+`: these are mail header values and not a
// query a server will decode.
static void plus_to_space(char *s, size_t len, size_t expanded)
{
    size_t write=expanded;
    s[write]='\0';
    while (len != 0) {
        char c=s[--len];
        if (c == '+') {
            s[--write]='0';s[--write]='2';s[--write]='%';
        } else s[--write]=c;
    }
}

// Mail text/plain uses UTF-8 percent encoding with the URL path encode set.
static char *mail_plain(const char *s, size_t len, size_t *out_len)
{
    if (len > (SIZE_MAX-1)/3)
        return NULL;
    char *out=os64_malloc(len*3+1);
    if (out == NULL)
        return NULL;
    static const char hex[]="0123456789ABCDEF";
    size_t at=0;
    for (size_t i=0;i<len;i++) {
        unsigned char c=(unsigned char)s[i];
        if (c<=0x20 || c>=0x7f || c=='"' || c=='#' || c=='<' || c=='>' ||
            c=='?' || c=='^' || c=='`' || c=='{' || c=='}') {
            out[at++]='%';out[at++]=hex[c>>4];out[at++]=hex[c&15];
        } else out[at++]=(char)c;
    }
    out[at]='\0';*out_len=at;
    return out;
}

static os64_page_verdict_t submit(const os64_page_t *page, os64_page_what_t what,
                                  os64_page_request_t *out)
{
    const os64_page_control_t *c = os64_page_control(page, what.index);
    if (c == NULL)
        return refuse(out, OS64_PAGE_REASON_NO_CONTROL);
    int32_t form = c->form, submitter = -1;
    os64_page_reason_t reason = OS64_PAGE_REASON_OK;
    if (!p_submitter(page, form, what, &submitter, &reason))
        return nothing(out, reason);

    const os64_page_ref_t *action = NULL;
    bool novalidate = false;
    p_effective(page, form, submitter, &action, &out->method, &out->enctype, &novalidate);

    // VALIDATION IS ACTED ON BEFORE THE METHOD IS, which is the standard's
    // order and matters for a `dialog` form: it still has to be filled in
    // before it closes.
    if (!novalidate && !p_validate(page, form, &out->control))
        return nothing(out, OS64_PAGE_REASON_INVALID);
    if (out->method == OS64_PAGE_METHOD_DIALOG)
        // The form closes a dialog. No server hears of it, and putting the
        // values in an address instead is how a password reaches a log.
        return nothing(out, OS64_PAGE_REASON_DIALOG);

    if (action->spelled && action->url == NULL)
        return refuse(out, action->refused != OS64_PAGE_REASON_OK ? action->refused
                                                                 : OS64_PAGE_REASON_BAD_ACTION);
    // An absent or empty action means the page the form is on.
    const char *target = action->url != NULL ? action->url : page->document_url;

    char scheme[OS64_URL_SCHEME_MAX];
    if (!os64_url_scheme_of(target, scheme, sizeof(scheme)))
        return refuse(out, OS64_PAGE_REASON_BAD_ACTION);

    bool http = os64_streq(scheme, "http") || os64_streq(scheme, "https");
    bool data = os64_streq(scheme, "data");
    bool mail = os64_streq(scheme, "mailto");
    bool bare = os64_streq(scheme, "ftp") || os64_streq(scheme, "javascript");
    if (!http && !data && !mail && !bare)
        // The standard's table has no row for it, and its "otherwise" is to
        // do nothing — which this reports by name rather than in silence.
        return refuse(out, OS64_PAGE_REASON_SCHEME);

    PEntries entries;
    if (!p_entry_list(page, form, submitter, what, &entries)) {
        p_entries_free(&entries);
        return refuse(out, OS64_PAGE_REASON_NO_MEMORY);
    }
    const char *encoding = p_encoding_for(page, form);

    // A form's `#name` is kept, exactly as a link's is: it was dropped here
    // once while a link's survived, which is one rule with two answers.
    if (action->has_fragment) {
        out->has_fragment = true;
        out->fragment = keep(action->fragment != NULL ? action->fragment : "");
        if (out->fragment == NULL) {
            p_entries_free(&entries);
            return refuse(out, OS64_PAGE_REASON_NO_MEMORY);
        }
    }

    // `ftp:` and `javascript:` take no data at all whatever the form said,
    // and neither does a `data:` POST. What is left is a plain navigation,
    // so the method the face should use is GET.
    if (bare || (data && out->method == OS64_PAGE_METHOD_POST)) {
        p_entries_free(&entries);
        out->method = OS64_PAGE_METHOD_GET;
        out->url = keep(target);
        if (out->url == NULL)
            return refuse(out, OS64_PAGE_REASON_NO_MEMORY);
        state_facts(page, out);
        return OS64_PAGE_NAVIGATE;
    }

    char *bytes = NULL, *type = NULL;
    size_t len = 0;
    os64_page_enctype_t enctype = out->enctype;
    // Every GET row that serializes entries uses urlencoding. Mail POST
    // also uses it unless the page explicitly requests a plain-text body.
    if (out->method == OS64_PAGE_METHOD_GET ||
        (mail && enctype != OS64_PAGE_ENCTYPE_TEXT_PLAIN))
        enctype = OS64_PAGE_ENCTYPE_URLENCODED;
    bool carries_body = out->method == OS64_PAGE_METHOD_POST && http;
    size_t limit = carries_body ? page->opt.max_body : OS64_URL_REF_MAX - 1;
    if (!p_serialise(&entries, enctype, mail && enctype == OS64_PAGE_ENCTYPE_TEXT_PLAIN ? "utf-8" : encoding, limit, &bytes, &len, &type,
                     &reason)) {
        p_entries_free(&entries);
        if (!carries_body && reason == OS64_PAGE_REASON_BODY_TOO_LONG)
            reason = OS64_PAGE_REASON_TOO_LONG;
        return refuse(out, reason);
    }
    p_entries_free(&entries);

    if (out->method == OS64_PAGE_METHOD_POST && http) {
        // The query the action names is left alone: a POST carries its data
        // in the body and says what it is in the content type.
        out->url = keep(target);
        out->body = bytes;
        out->body_len = len;
        out->content_type = type;
        if (out->url == NULL)
            return refuse(out, OS64_PAGE_REASON_NO_MEMORY);
        state_facts(page, out);
        return OS64_PAGE_NAVIGATE;
    }

    if (mail && out->method == OS64_PAGE_METHOD_POST) {
        if (enctype == OS64_PAGE_ENCTYPE_TEXT_PLAIN) {
            char *escaped=mail_plain(bytes,len,&len);
            os64_free(bytes);
            bytes=escaped;
            if (bytes == NULL) {
                os64_free(type);
                return refuse(out,OS64_PAGE_REASON_NO_MEMORY);
            }
        }
        // The body rides in the address as one more header.
        char *query = os64_malloc(len + 6);
        if (query == NULL) {
            os64_free(bytes);
            os64_free(type);
            return refuse(out, OS64_PAGE_REASON_NO_MEMORY);
        }
        os64_memcpy(query, "body=", 5);
        os64_memcpy(query + 5, bytes, len);
        query[len + 5] = '\0';
        bool too_long = false;
        out->url = with_query(target, query, true, &too_long);
        os64_free(query);
        os64_free(bytes);
        // No body goes out, so there is nothing for a content type to
        // describe: the bytes rode in the address.
        os64_free(type);
        out->method = OS64_PAGE_METHOD_GET;
        if (out->url == NULL)
            return refuse(out, too_long ? OS64_PAGE_REASON_TOO_LONG
                                        : OS64_PAGE_REASON_NO_MEMORY);
        state_facts(page, out);
        return OS64_PAGE_NAVIGATE;
    }

    // Everything left mutates the action's query: an http GET, a `data:`
    // GET, and a `mailto:` GET whose headers these become.
    if (mail) {
        // Grown for the substitution before it happens, since each `+`
        // becomes three bytes.
        size_t plusses = 0;
        for (size_t at = 0; at < len; at++)
            if (bytes[at] == '+')
                plusses++;
        char *roomy = os64_realloc(bytes, len + plusses * 2 + 1);
        if (roomy == NULL) {
            os64_free(bytes);
            os64_free(type);
            return refuse(out, OS64_PAGE_REASON_NO_MEMORY);
        }
        bytes = roomy;
        plus_to_space(bytes,len,len+plusses*2);
    }
    bool too_long = false;
    out->url = with_query(target, bytes, false, &too_long);
    os64_free(bytes);
    os64_free(type);   // the entry list went into the address, not a body
    if (out->url == NULL)
        return refuse(out, too_long ? OS64_PAGE_REASON_TOO_LONG
                                    : OS64_PAGE_REASON_NO_MEMORY);
    state_facts(page, out);
    return OS64_PAGE_NAVIGATE;
}

os64_page_verdict_t os64_page_activate(const os64_page_t *page, os64_page_what_t what,
                                       os64_page_request_t *out)
{
    if (out == NULL)
        return OS64_PAGE_REFUSED;
    request_start(out);
    if (page == NULL)
        return refuse(out, OS64_PAGE_REASON_NO_CONTROL);
    if (what.how == OS64_PAGE_ACTIVATE_LINK)
        return follow_link(page, what.index, out);
    if (what.how == OS64_PAGE_ACTIVATE_REFRESH)
        return follow_refresh(page, out);
    // A partial model cannot establish a complete entry list or submitter.
    if (page->incomplete)
        return refuse(out, OS64_PAGE_REASON_NO_MEMORY);
    return submit(page, what, out);
}
