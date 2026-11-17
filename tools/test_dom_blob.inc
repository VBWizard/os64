/* BLOB URLS (libdom/blob_url.c; dom.h § BLOB URLS; BLOB_URLS.md): the URL
 * createObjectURL makes and its origin, opaque included; the TypeErrors;
 * the entry libdom takes where the standard parses a URL — fetch(), the
 * Request constructor, xhr.open() and a followed link — so a revocation
 * after it does not reach the request; a revocation before it, which
 * does; a fragment ignored; the store's bound, its refusals and its
 * totals; an entry outliving the store; and two threads dropping
 * references at once. */

#include <pthread.h>

/* What the fake network saw of each request: its address and whether it
 * carried an entry, and the entry's bytes and type when it did. */
typedef struct {
    struct {
        char url[256], bytes[64], type[64];
        bool blob;
    } seen[16];
    size_t count;
} BlobNet;

static uint64_t blob_net_start(void *opaque, const os64_dom_request_t *q, uint32_t token)
{
    (void)token;
    BlobNet *n = opaque;
    if (n->count == sizeof(n->seen) / sizeof(n->seen[0])) return 0;
    size_t i = n->count++;
    snprintf(n->seen[i].url, sizeof(n->seen[i].url), "%s", q->url);
    n->seen[i].blob = q->blob != NULL;
    if (q->blob != NULL) {
        size_t len = os64_dom_blob_length(q->blob);
        snprintf(n->seen[i].bytes, sizeof(n->seen[i].bytes), "%.*s", (int)len, (const char *)os64_dom_blob_bytes(q->blob));
        snprintf(n->seen[i].type, sizeof(n->seen[i].type), "%s", os64_dom_blob_type(q->blob));
    }
    return 2000 + n->count;
}

static void blob_net_cancel(void *opaque, uint64_t id) { (void)opaque; (void)id; }

static bool blob_entropy(void *opaque, uint8_t *out, size_t length)
{
    (void)opaque;
    static uint8_t next;
    for (size_t i = 0; i < length; i++) out[i] = next++;
    return true;
}

static DomFixture blob_fixture(const char *url, os64_dom_blob_store_t *store, BlobNet *net)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.url = url;
    options.now_ms = storage_now;
    options.entropy = blob_entropy;
    options.blobs = store;
    DomFixture f = fixture_base(0, 0, 0);
    if (f.runtime != NULL && f.state != NULL) {
        os64_js_outcome_t outcome;
        f.binding = os64_dom_create(f.runtime, f.document, f.state, &options, &outcome);
    }
    if (f.binding != NULL && net != NULL) {
        const os64_dom_network_t network = {blob_net_start, blob_net_cancel, NULL, NULL, net, NULL};
        os64_dom_set_network(f.binding, &network);
    }
    return f;
}

/* The store is freed before the fixture, whose teardown counts every
 * native byte: what the binding's objects still hold is released by their
 * finalizers in the fixture's free, after the store is gone. */
static void blob_done(DomFixture *f, os64_dom_blob_store_t *store)
{
    os64_dom_blob_store_free(store);
    fixture_free(f);
}

static void blob_release_many(os64_dom_blob_t *b, int n)
{
    for (int i = 0; i < n; i++)
        os64_dom_blob_release(b);
}

static void *blob_releaser(void *arg)
{
    blob_release_many(arg, 5000);
    return NULL;
}

static void dom_blob_cases(void)
{
    /* The form, the origin, and a fresh uuid each time. */
    os64_dom_blob_store_t *store = os64_dom_blob_store_new(0);
    BlobNet net = {0};
    DomFixture f = blob_fixture("https://example.test/dir/page.html", store, &net);
    check(f.binding != NULL, "blob: the binding is made with a store");
    dom_script(&f, STORAGE_ASSERT
        "const u=URL.createObjectURL(new Blob(['hello'],{type:'text/plain'}));"
        "assert(/^blob:https:\\/\\/example\\.test\\/[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(u),u);"
        "assert(URL.createObjectURL(new Blob([]))!==u,'each name is new');"
        "let e;try{URL.createObjectURL({});}catch(x){e=x;}assert(e instanceof TypeError,'not a Blob');"
        "e=undefined;try{URL.createObjectURL();}catch(x){e=x;}assert(e instanceof TypeError,'no argument');"
        "e=undefined;try{URL.revokeObjectURL();}catch(x){e=x;}assert(e instanceof TypeError,'revoke needs one');"
        "assert(URL.revokeObjectURL('https://example.test/')===undefined,'not a blob: URL, ignored');"
        "assert(URL.revokeObjectURL('blob:https://other.test/x')===undefined,'none of ours, ignored');"
        "globalThis.u=u;", "blob: the URL's form");
    os64_dom_blob_usage_t used = os64_dom_blob_store_usage(store);
    check(used.entries == 2 && used.bytes == 5 && used.refused == 0, "blob: the store counts what it named");

    /* fetch() takes inside the call: revoked on the next line, it holds
     * the entry; revoked before, it holds none; a fragment is ignored. */
    dom_script(&f, "fetch(u);URL.revokeObjectURL(u);", "blob: fetch then revoke");
    check(net.count == 1 && net.seen[0].blob && !strcmp(net.seen[0].bytes, "hello") &&
          !strcmp(net.seen[0].type, "text/plain"), "blob: fetch() took the entry inside the call");
    used = os64_dom_blob_store_usage(store);
    check(used.entries == 1 && used.bytes == 0, "blob: the revocation removed the name");
    dom_script(&f, "fetch(u);", "blob: fetch after revoke");
    check(net.count == 2 && !net.seen[1].blob, "blob: a revoked URL names nothing");
    dom_script(&f,
        "const v=URL.createObjectURL(new Blob(['abc']));"
        "fetch(v+'#frag');"
        "const r=new Request(v);URL.revokeObjectURL(v);fetch(r);fetch(r.clone());"
        "const w=URL.createObjectURL(new Blob(['xyz'],{type:'a/b'}));"
        "const x=new XMLHttpRequest();x.open('GET',w);URL.revokeObjectURL(w);x.send();",
        "blob: a fragment, a Request, an XHR");
    check(net.count == 6, "blob: four more requests");
    check(net.count == 6 && net.seen[2].blob && !strcmp(net.seen[2].bytes, "abc"), "blob: a fragment is ignored");
    check(net.count == 6 && net.seen[3].blob && net.seen[4].blob, "blob: a Request and its clone took it at construction");
    check(net.count == 6 && net.seen[5].blob && !strcmp(net.seen[5].bytes, "xyz") && !strcmp(net.seen[5].type, "a/b"),
          "blob: xhr.open() took it before send()");

    /* A link clicked and revoked on the next line: the navigation ask
     * carries the entry. */
    dom_script(&f,
        "const d=URL.createObjectURL(new Blob(['save me']));"
        "const a=document.createElement('a');a.href=d;a.download='x.txt';document.body.appendChild(a);"
        "a.click();URL.revokeObjectURL(d);", "blob: the hand-rolled download");
    os64_dom_navigation_t ask;
    bool asked = os64_dom_take_navigation(f.binding, &ask);
    check(asked && ask.kind == OS64_DOM_NAVIGATE_FOLLOW && ask.blob != NULL &&
          os64_dom_blob_length(ask.blob) == 7 && !memcmp(os64_dom_blob_bytes(ask.blob), "save me", 7),
          "blob: the followed link's ask holds the entry taken in the click");
    if (asked) {
        os64_dom_blob_release(ask.blob);
        if (ask.node != NULL) os64_html_release(f.document, ask.node);
    }
    dom_script(&f, "location.href=URL.createObjectURL(new Blob(['p']));", "blob: a URL assigned");
    asked = os64_dom_take_navigation(f.binding, &ask);
    check(asked && ask.kind == OS64_DOM_NAVIGATE_URL && ask.blob != NULL, "blob: an assigned URL's ask holds its entry");
    if (asked) os64_dom_blob_release(ask.blob);

    /* The host's take, and an entry outliving its store. */
    os64_dom_blob_t *held = NULL;
    {
        dom_script(&f, "globalThis.k=URL.createObjectURL(new Blob(['kept']));fetch(k);", "blob: one to keep");
        const char *k = net.seen[net.count - 1].url;
        held = os64_dom_blob_url_take(store, k);
        check(held != NULL && os64_dom_blob_length(held) == 4, "blob: the host takes by the URL");
        check(os64_dom_blob_url_take(store, "blob:https://example.test/nothing") == NULL &&
              os64_dom_blob_url_take(store, "https://example.test/") == NULL && os64_dom_blob_url_take(NULL, k) == NULL,
              "blob: a take of nothing is NULL");
    }
    /* The store goes first, then the entry is read and let go, and only
     * then the fixture, whose teardown counts every byte still live. */
    os64_dom_blob_store_free(store);
    check(held != NULL && !memcmp(os64_dom_blob_bytes(held), "kept", 4), "blob: a held entry outlives its store");
    os64_dom_blob_release(held);
    fixture_free(&f);

    /* An opaque origin names "null". */
    store = os64_dom_blob_store_new(0);
    f = blob_fixture("file:///home/page.html", store, NULL);
    dom_script(&f, STORAGE_ASSERT
        "const u=URL.createObjectURL(new Blob(['x']));assert(u.startsWith('blob:null/'),u);", "blob: an opaque origin");
    blob_done(&f, store);

    /* The bound: a Blob that would take the store past it is refused with
     * a QuotaExceededError, counted, and nothing is named. */
    store = os64_dom_blob_store_new(10);
    f = blob_fixture("https://example.test/", store, NULL);
    dom_script(&f, STORAGE_ASSERT
        "URL.createObjectURL(new Blob(['0123456789']));"
        "let e;try{URL.createObjectURL(new Blob(['!']));}catch(x){e=x;}"
        "assert(e&&e.name==='QuotaExceededError',String(e));", "blob: the bound");
    used = os64_dom_blob_store_usage(store);
    check(used.entries == 1 && used.bytes == 10 && used.refused == 1, "blob: the refusal is counted, nothing named");
    blob_done(&f, store);

    /* No store: createObjectURL refuses. */
    f = blob_fixture("https://example.test/", NULL, NULL);
    dom_script(&f, STORAGE_ASSERT
        "let e;try{URL.createObjectURL(new Blob([]));}catch(x){e=x;}assert(e instanceof TypeError,'no store');",
        "blob: a binding with no store");
    fixture_free(&f);

    /* Two threads dropping references at once: the entry is freed once,
     * by whichever drops the last (ASan says if it is freed twice or
     * read after). */
    store = os64_dom_blob_store_new(0);
    BlobNet seen = {0};
    f = blob_fixture("https://example.test/", store, &seen);
    dom_script(&f, "globalThis.t=URL.createObjectURL(new Blob(['threads']));fetch(t);", "blob: one to share");
    os64_dom_blob_t *shared = seen.count == 1 ? os64_dom_blob_url_take(store, seen.seen[0].url) : NULL;
    check(shared != NULL, "blob: taken for the threads");
    if (shared != NULL) {
        for (int i = 0; i < 10000; i++) os64_dom_blob_hold(shared);
        pthread_t a, b;
        pthread_create(&a, NULL, blob_releaser, shared);
        pthread_create(&b, NULL, blob_releaser, shared);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        check(!memcmp(os64_dom_blob_bytes(shared), "threads", 7), "blob: alive after 10,000 releases on two threads");
        os64_dom_blob_release(shared);
    }
    blob_done(&f, store);
}
