/* WEB STORAGE (libdom/storage.c; dom.h § Web storage): a case per sentence
 * of the contract. The Storage object's shape and its strings; the quota
 * and the keys' cap; an opaque origin and an area with no provider; the
 * provider's protocol (identity at a task's first touch, load and replay,
 * the change list, the commit's timing, a full store, a failed one, the
 * departure commit, forgetting); the store's bytes (walk, merge); two
 * pages on one store; and every allocation the binding makes failed in
 * turn. */

/* storage.c's count of the maps' steps (libdom/internal.h). */
uint64_t d_storage_steps(void);
static void dom_storage_merge_work_cases(void);

/* assert(x, why), and throws(name, f): f throws an error named `name`.
 * The slice's other case files use it too. */
#define STORAGE_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}" \
    "function throws(n,f){let caught=false;try{f()}catch(e){caught=true;" \
    "assert(e.name===n,'wanted '+n+' got '+e.name+': '+e.message)}assert(caught,'no '+n);}"

/* Every allocation each operation makes, failed in turn in a page made
 * with `options`: the operation throws QuotaExceededError or does its
 * work, and the page's teardown accounts for every byte (fixture_free). */
static void binding_sweep(const char *what, const char *const *operations, size_t n,
                          const os64_dom_options_t *options)
{
    for (size_t op = 0; op < n; op++) {
        DomFixture f = fixture_new(options);
        if (!fixture_ready(&f)) break;
        size_t start = dom_attempts;
        dom_script(&f, operations[op], "sweep: an operation measured");
        size_t count = dom_attempts - start;
        printf("%s operation %zu allocation points: %zu\n", what, op, count);
        fixture_free(&f);
        for (size_t cut = 1; cut <= count; cut++) {
            f = fixture_new(options);
            if (!fixture_ready(&f)) break;
            char source[512];
            snprintf(source, sizeof(source),
                     "try{%s;globalThis.refused=false}catch(e){if(e.name!=='QuotaExceededError')throw e;globalThis.refused=true}",
                     operations[op]);
            size_t target = dom_attempts + cut;
            dom_fail_at = target;
            dom_script(&f, source, "sweep: an allocation cut");
            dom_fail_at = 0;
            dom_sweeps++;
            check(dom_attempts >= target, "sweep: the cut was reached");
            fixture_free(&f);
        }
    }
}

/* Every allocation each operation makes, the engine's among them, failed
 * in turn while it runs: whatever the outcome (an engine that ran out of
 * memory is the runtime's sticky failure, libjs's contract, not an
 * exception a script can catch), nothing crashes, nothing the binding made
 * is freed twice, and the teardown accounts for every byte, the engine's
 * included (fixture_free, under a runtime whose destruction asserts its
 * objects are gone). The operation is compiled before the sweep, as a
 * function a task then calls: the engine's own compiler is libjs's to
 * prove under a refused allocation, not this binding's. */
static int engine_sweep_run(DomFixture *f, const char *operation, size_t cut, size_t *count)
{
    char source[1024];
    snprintf(source, sizeof(source), "globalThis.op=function(){%s}", operation);
    if (!dom_script(f, source, "sweep: an operation compiled")) return -1;
    os64_js_outcome_t outcome;
    JSContext *ctx = os64_js_context(f->runtime, OS64_JS_ABI_ID, &outcome);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue op = JS_GetPropertyStr(ctx, global, "op");
    JS_FreeValue(ctx, global);
    if (os64_js_task_begin(f->runtime, OS64_JS_ABI_ID, "sweep", &outcome) != OS64_JS_OK) {
        JS_FreeValue(ctx, op);
        return -1;
    }
    size_t start = attempts;
    if (cut != 0) fail_at = attempts + cut;
    os64_js_call(f->runtime, OS64_JS_ABI_ID, op, JS_UNDEFINED, 0, NULL, NULL, &outcome);
    fail_at = 0;
    *count = attempts - start;
    os64_js_task_end(f->runtime, OS64_JS_ABI_ID, &outcome);
    JS_FreeValueRT(JS_GetRuntime(ctx), op);
    return 0;
}

static void engine_sweep(const char *what, const char *const *operations, size_t n,
                         const os64_dom_options_t *options)
{
    for (size_t op = 0; op < n; op++) {
        DomFixture f = fixture_new(options);
        if (!fixture_ready(&f)) break;
        size_t count = 0;
        engine_sweep_run(&f, operations[op], 0, &count);
        printf("%s operation %zu engine allocation points: %zu\n", what, op, count);
        fixture_free(&f);
        for (size_t cut = 1; cut <= count; cut++) {
            f = fixture_new(options);
            if (!fixture_ready(&f)) break;
            size_t reached = 0;
            engine_sweep_run(&f, operations[op], cut, &reached);
            dom_sweeps++;
            check(reached >= cut, "sweep: the engine's cut was reached");
            fixture_free(&f);
        }
    }
}

/* A provider's store, in memory: what a host keeps between pages. */
typedef struct {
    char *image;                        /* the harness's malloc */
    size_t length;
    uint64_t identity;
    unsigned identities, loads, commits;
    bool fail_load, fail_commit;
    /* load answers this length and writes nothing when it is not 0; with
     * outgrow, one byte more than the room it is given. */
    int64_t claimed;
    bool outgrow;
    os64_dom_storage_status_t forced;   /* answered by commit when not OK */
    char origin[128];
    char last_changes[4096];
    size_t last_changes_length;
} FakeStore;

/* `needle` somewhere in `length` bytes at `hay`, or NULL. */
static const char *bytes_find(const void *hay, size_t length, const char *needle, size_t needle_length)
{
    const char *at = hay;
    for (size_t i = 0; needle_length <= length && i <= length - needle_length; i++)
        if (memcmp(at + i, needle, needle_length) == 0) return at + i;
    return NULL;
}

static uint64_t storage_clock;
static uint64_t storage_now(void *opaque) { (void)opaque; return storage_clock; }

static uint64_t fake_identity(void *opaque, const char *origin)
{
    FakeStore *s = opaque;
    snprintf(s->origin, sizeof(s->origin), "%s", origin);
    s->identities++;
    return s->identity;
}

static int64_t fake_load(void *opaque, const char *origin, void *out, size_t cap)
{
    FakeStore *s = opaque;
    snprintf(s->origin, sizeof(s->origin), "%s", origin);
    s->loads++;
    if (s->fail_load) return -1;
    if (s->claimed != 0) return s->claimed;
    if (s->outgrow) return (int64_t)cap + 1;
    if (s->length <= cap && s->length != 0) memcpy(out, s->image, s->length);
    return (int64_t)s->length;
}

/* The merge's answer, the store's from here, kept in the harness's own heap
 * so a page's teardown can account for every byte of the binding's. */
static void fake_store_take(FakeStore *s, void *merged, size_t length)
{
    free(s->image);
    s->image = NULL;
    if (merged != NULL) {
        s->image = malloc(length);
        if (s->image == NULL) abort();
        memcpy(s->image, merged, length);
        dom_host_free(merged);
    }
    s->length = length;
    s->identity++;
}

static os64_dom_storage_status_t fake_commit(void *opaque, const char *origin, const void *changes,
                                             size_t length)
{
    FakeStore *s = opaque;
    snprintf(s->origin, sizeof(s->origin), "%s", origin);
    s->commits++;
    s->last_changes_length = length < sizeof(s->last_changes) ? length : sizeof(s->last_changes);
    memcpy(s->last_changes, changes, s->last_changes_length);
    if (s->fail_commit) return OS64_DOM_STORAGE_FAILED;
    if (s->forced != OS64_DOM_STORAGE_OK) return s->forced;
    void *merged;
    size_t merged_length;
    os64_dom_storage_status_t status = os64_dom_storage_merge(s->image, s->length, changes, length, &merged,
                                                              &merged_length);
    if (status != OS64_DOM_STORAGE_OK) return status;
    fake_store_take(s, merged, merged_length);
    return OS64_DOM_STORAGE_OK;
}

static void fake_store_free(FakeStore *s)
{
    free(s->image);
    memset(s, 0, sizeof(*s));
}

/* A page on `url` with `store` keeping both areas (NULL: none). */
static DomFixture storage_fixture(const char *url, FakeStore *store)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.url = url;
    options.now_ms = storage_now;
    /* The quota cases hold two quota-sized strings at once; the cap leaves
     * room for them beside the realm's own baseline, which is no part of
     * what they measure. */
    DomFixture f = fixture_new_capped(&options, (size_t)24 * 1024 * 1024);
    if (f.binding != NULL && store != NULL) {
        const os64_dom_storage_t provider = {fake_identity, fake_load, fake_commit, store};
        os64_dom_set_storage(f.binding, OS64_DOM_STORAGE_LOCAL, &provider);
        os64_dom_set_storage(f.binding, OS64_DOM_STORAGE_SESSION, &provider);
    }
    return f;
}

/* Another window's commit to the store: `records` merged onto it. */
static void fake_store_set(FakeStore *s, const char *records)
{
    void *merged;
    size_t merged_length;
    check(os64_dom_storage_merge(s->image, s->length, records, strlen(records), &merged, &merged_length) ==
          OS64_DOM_STORAGE_OK, "storage: another window's commit");
    fake_store_take(s, merged, merged_length);
}

static void dom_storage_shape_cases(void)
{
    DomFixture f = storage_fixture("http://example.test/page", NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "for(const s of [localStorage,sessionStorage]){"
        "assert(s===(s===localStorage?window.localStorage:window.sessionStorage),'one object');"
        "assert(s instanceof Storage&&Object.getPrototypeOf(s)===Storage.prototype);"
        "assert(Object.prototype.toString.call(s)==='[object Storage]');"
        "s.clear();assert(s.length===0&&s.key(0)===null&&s.getItem('a')===null);"
        "s.setItem('a','1');s.setItem('b',2);s.setItem('a','one');"
        "assert(s.length===2&&s.key(0)==='a'&&s.key(1)==='b'&&s.key(2)===null&&s.key(4294967296)==='a');"
        "assert(s.getItem('a')==='one'&&s.getItem('b')==='2'&&s.a==='one'&&s['b']==='2');"
        "assert('a' in s&&!('c' in s)&&s.hasOwnProperty('a'));"
        "assert(Object.keys(s).join()==='a,b');"
        "var d=Object.getOwnPropertyDescriptor(s,'a');assert(d.value==='one'&&d.writable&&d.enumerable&&d.configurable);"
        "s.c=3;assert(s.getItem('c')==='3'&&s.length===3);"
        "s.c={toString(){return 'four'}};assert(s.getItem('c')==='four');"
        "delete s.c;assert(s.getItem('c')===null&&s.length===2);"
        "s.removeItem('nothing');s.removeItem('b');assert(s.length===1&&s.key(0)==='a');"
        /* WebIDL's named property visibility: the prototype's names win a read,
         * and the setter takes every write. */
        "s.setItem('getItem','x');assert(typeof s.getItem==='function'&&s.getItem('getItem')==='x');"
        "assert(Object.keys(s).indexOf('getItem')<0&&!s.hasOwnProperty('getItem'));"
        "s.key='k';assert(typeof s.key==='function'&&s.getItem('key')==='k');"
        "delete s.getItem;assert(s.getItem('getItem')==='x');"
        "Object.defineProperty(s,'d',{value:'dv'});assert(s.getItem('d')==='dv');"
        "Object.defineProperty(s,'d',{value:{toString(){return 'again'}}});assert(s.d==='again');"
        "throws('TypeError',()=>Object.defineProperty(s,'e',{get(){return 1}}));"
        "assert(Reflect.defineProperty(s,'e',{get(){return 1}})===false&&s.getItem('e')===null);"
        "s[9]='nine';assert(s.getItem('9')==='nine'&&s[9]==='nine');"
        /* A Symbol is an ordinary property of the object. */
        "var k=Symbol();s[k]='sym';assert(s[k]==='sym'&&Object.getOwnPropertySymbols(s)[0]===k);"
        /* length counts every item; the keys a page can see leave out those
         * the prototype shadows, in the order they were set (WebIDL's
         * supported property names, which no index sorts first). */
        "assert(s.length===5&&Object.keys(s).join()==='a,d,9',Object.keys(s).join());"
        "Object.defineProperty(s,k,{value:'fixed',configurable:false});assert(s[k]==='sym'||s[k]==='fixed');"
        "var o=Object.create(s);o.inherited='i';assert(o.hasOwnProperty('inherited')&&s.getItem('inherited')===null);"
        "s.clear();assert(s.length===0&&Object.keys(s).length===0);"
        "throws('TypeError',()=>s.setItem('only one'));throws('TypeError',()=>s.getItem());"
        "throws('TypeError',()=>s.key());throws('TypeError',()=>s.removeItem());"
        "throws('TypeError',()=>Storage.prototype.getItem.call({},'a'));"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(Storage.prototype,'length').get.call(document));"
        "var t={toString(){throw Error('no')}};s.setItem('a','1');"
        "var r;try{s.setItem('a',t)}catch(e){r=e.message}assert(r==='no'&&s.getItem('a')==='1');"
        "s.clear();}"
        "throws('TypeError',()=>new Storage());throws('TypeError',()=>Storage());"
        "localStorage.setItem('shared','l');assert(sessionStorage.getItem('shared')===null);localStorage.clear();",
        "storage: the Storage interface, its named properties and its brand");
    /* Every code unit is kept: a lone surrogate, U+0000, a pair. */
    dom_script(&f, STORAGE_ASSERT
        "var odd=['\\ud800','\\udfff','a\\udc00','\\u0000','foo\\u0000bar','\\ud83c\\udf4d','\\ufffd','\\uffff'];"
        "for(const v of odd){localStorage.clear();localStorage.setItem(v,v);"
        "assert(localStorage.getItem(v)===v&&localStorage.key(0)===v&&localStorage[v]===v,'kept '+escape(v));}"
        "localStorage.clear();localStorage['foo\\u0000bar']=1;"
        "assert(!('foo\\u0000' in localStorage)&&!('foo' in localStorage)&&localStorage.getItem('foo\\u0000bar')==='1');"
        "localStorage.setItem('\\ud800','lone');localStorage.setItem('\\ufffd','replacement');"
        "assert(localStorage.length===3&&localStorage.getItem('\\ud800')==='lone');localStorage.clear();",
        "storage: strings keep every code unit, a lone surrogate and U+0000 included");
    fixture_free(&f);
}

/* The quota: two bytes per UTF-16 code unit of every key and value, 5 MiB
 * an area; and 65536 keys. A refused write changes nothing. */
static void dom_storage_quota_cases(void)
{
    DomFixture f = storage_fixture("http://example.test/", NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "var big='x'.repeat(5*1024*1024/2-1);"            /* with key 'k', exactly the quota */
        "localStorage.setItem('k',big);assert(localStorage.getItem('k').length===big.length);"
        "throws('QuotaExceededError',()=>localStorage.setItem('l','y'));"
        "assert(localStorage.length===1&&localStorage.getItem('l')===null);"
        "localStorage.setItem('k',big);"                    /* the same value is no change */
        /* The other area has its own quota. */
        "sessionStorage.setItem('k',big);assert(sessionStorage.getItem('k').length===big.length);"
        "sessionStorage.clear();localStorage.setItem('k','small');localStorage.setItem('l','y');"
        "assert(localStorage.length===2);localStorage.clear();big=null;",
        "storage: the quota is the area's, and a refused write changes nothing");
    /* A pair is two code units, one character: the count is UTF-16's. Two
     * code units short of the quota, a pair fits and a pair and one more
     * does not. */
    dom_script(&f, STORAGE_ASSERT
        "var near='x'.repeat(5*1024*1024/2-3);localStorage.setItem('k',near);"
        "localStorage.setItem('k',near+'\\ud83d\\ude00');"
        "throws('QuotaExceededError',()=>localStorage.setItem('k',near+'\\ud83d\\ude00a'));"
        "assert(localStorage.getItem('k').length===near.length+2);localStorage.clear();",
        "storage: the quota counts UTF-16 code units");
    dom_script(&f, STORAGE_ASSERT
        "for(var i=0;i<65536;i++)localStorage.setItem(''+i,'');"
        "throws('QuotaExceededError',()=>localStorage.setItem('one more',''));"
        "localStorage.setItem('0','still settable');assert(localStorage.length===65536);"
        "localStorage.removeItem('1');localStorage.setItem('one more','');assert(localStorage.length===65536);"
        "localStorage.clear();assert(localStorage.length===0);",
        "storage: an area holds at most 65536 keys");
    fixture_free(&f);
}

static void dom_storage_provider_cases(void)
{
    FakeStore store = {0};
    storage_clock = 1000;
    /* An opaque origin keeps its map for the page's life and asks nothing. */
    DomFixture f = storage_fixture("about:blank", &store);
    if (!fixture_ready(&f)) { fake_store_free(&store); return; }
    dom_script(&f, STORAGE_ASSERT "localStorage.setItem('a','1');sessionStorage.setItem('b','2');"
               "assert(localStorage.getItem('a')==='1');", "storage: an opaque origin's areas work");
    os64_dom_storage_settle(f.binding);
    check(store.identities == 0 && store.loads == 0 && store.commits == 0 &&
          os64_dom_storage_due(f.binding) == UINT64_MAX,
          "storage: an opaque origin's areas are never kept by the provider");
    fixture_free(&f);
    f = storage_fixture("file:///home/page.html", &store);
    if (fixture_ready(&f)) {
        dom_script(&f, "localStorage.setItem('a','1')", "storage: a file page's storage");
        os64_dom_drain(f.binding);
        check(store.commits == 0, "storage: a file page's origin is opaque, and kept nowhere");
        fixture_free(&f);
    }

    /* The first touch in a task asks the store's identity, and loads it
     * when it moved; a page's own changes ride over what it loads. */
    fake_store_set(&store, "os64 storage 1\n+1 3\nkold\n");
    f = storage_fixture("https://Example.TEST:443/a?b#c", &store);
    if (!fixture_ready(&f)) { fake_store_free(&store); return; }
    check(store.identities == 0, "storage: nothing is asked before a script touches an area");
    dom_script(&f, STORAGE_ASSERT "assert(localStorage.getItem('k')==='old');localStorage.getItem('k');",
               "storage: the store is read at the first touch");
    check(store.identities == 1 && store.loads == 1 && !strcmp(store.origin, "https://example.test"),
          "storage: one identity and one load for a task's touches, by the page's origin");
    dom_script(&f, "localStorage.getItem('k')", "storage: a second script without a settle");
    check(store.identities == 1, "storage: a host that does not settle is asked once");
    os64_dom_storage_settle(f.binding);
    dom_script(&f, "localStorage.length", "storage: a task after a settle");
    check(store.identities == 2 && store.loads == 1, "storage: the next task asks again, and an unmoved store is not read");
    dom_script(&f, STORAGE_ASSERT "localStorage.setItem('mine','1');localStorage.removeItem('k');"
               "assert(localStorage.length===1);", "storage: a page's own changes");
    /* Another window writes: the store moves; this page's changes are
     * replayed over what it reads. */
    fake_store_set(&store, "os64 storage 1\n+5 5\notherwrote\n+1 3\nkold\n");
    check(os64_dom_storage_due(f.binding) == 0, "storage: an area that never committed is due at once");
    storage_clock = 1000;
    os64_dom_storage_settle(f.binding);
    check(store.commits == 1, "storage: the settle commits what is due");
    check(store.last_changes_length > 15 && !memcmp(store.last_changes, "os64 storage 1\n", 15) &&
          bytes_find(store.last_changes, store.last_changes_length, "+4 1\nmine1\n", 11) != NULL &&
          bytes_find(store.last_changes, store.last_changes_length, "-1\nk\n", 5) != NULL &&
          bytes_find(store.last_changes, store.last_changes_length, "other", 5) == NULL,
          "storage: a commit hands over the change list, never the map");
    dom_script(&f, STORAGE_ASSERT "assert(localStorage.getItem('other')==='wrote','another window\\'s key');"
               "assert(localStorage.getItem('mine')==='1'&&localStorage.getItem('k')===null);",
               "storage: after its commit the page sees the store as it stands");
    /* Coalescing: at most one commit a second. */
    dom_script(&f, "localStorage.setItem('a','1')", "storage: a write after a commit");
    os64_dom_storage_settle(f.binding);
    check(store.commits == 1 && os64_dom_storage_due(f.binding) == 2000,
          "storage: the next commit waits OS64_DOM_STORAGE_COMMIT_MS after the last");
    storage_clock = 1999;
    os64_dom_storage_settle(f.binding);
    check(store.commits == 1, "storage: not a millisecond early");
    storage_clock = 2000;
    os64_dom_storage_settle(f.binding);
    check(store.commits == 2 && os64_dom_storage_due(f.binding) == UINT64_MAX,
          "storage: due, and nothing left to commit");
    /* clear() is a removal of the keys the page could see. */
    dom_script(&f, STORAGE_ASSERT "localStorage.clear();assert(localStorage.length===0);",
               "storage: clear");
    fake_store_set(&store, "os64 storage 1\n+5 3\nlaternew\n");
    storage_clock = 5000;
    os64_dom_storage_settle(f.binding);
    dom_script(&f, STORAGE_ASSERT "assert(localStorage.length===1&&localStorage.getItem('later')==='new');",
               "storage: a clear does not take a key another window added after it");
    /* Drain commits what is left, due or not. */
    os64_dom_storage_settle(f.binding);
    dom_script(&f, "sessionStorage.setItem('s','v');localStorage.setItem('z','last')",
               "storage: changes left at departure");
    unsigned before = store.commits;
    os64_dom_drain(f.binding);
    check(store.commits == before + 2, "storage: drain makes the last commit, both areas");
    fixture_free(&f);
    check(store.length != 0 && bytes_find(store.image, store.length, "+1 4\nzlast\n", 11) != NULL,
          "storage: the last commit reached the store");
    fake_store_free(&store);

    /* A full store: nothing kept, and the page reads the store back. */
    storage_clock = 0;
    f = storage_fixture("http://example.test/", &store);
    if (!fixture_ready(&f)) return;
    dom_script(&f, "localStorage.setItem('a','1')", "storage: a write the store will refuse");
    store.forced = OS64_DOM_STORAGE_FULL;
    os64_dom_storage_settle(f.binding);
    store.forced = OS64_DOM_STORAGE_OK;
    dom_script(&f, STORAGE_ASSERT "assert(localStorage.getItem('a')===null&&localStorage.length===0);",
               "storage: after a full store refused a commit, the page sees the store");
    check(os64_dom_storage_due(f.binding) == UINT64_MAX, "storage: a refused commit's changes are gone");
    /* A store that cannot be written: the area keeps to memory from here. */
    dom_script(&f, "localStorage.setItem('b','2')", "storage: a write the store cannot keep");
    store.fail_commit = true;
    storage_clock = 10000;
    os64_dom_storage_settle(f.binding);
    store.fail_commit = false;
    unsigned asked = store.identities, commits = store.commits;
    dom_script(&f, STORAGE_ASSERT "assert(localStorage.getItem('b')==='2');localStorage.setItem('c','3');",
               "storage: a failed store's area keeps the page's map");
    storage_clock = 20000;
    os64_dom_storage_settle(f.binding);
    check(store.identities == asked && store.commits == commits,
          "storage: an area whose store failed asks the provider nothing more");
    fixture_free(&f);
    fake_store_free(&store);

    /* A store that cannot be read, or is not in the format. */
    for (int how = 0; how < 2; how++) {
        if (how == 0) store.fail_load = true;
        else { store.image = malloc(5); if (store.image == NULL) abort(); memcpy(store.image, "junk\n", 5); store.length = 5; store.identity = 9; }
        f = storage_fixture("http://example.test/", &store);
        if (!fixture_ready(&f)) { fake_store_free(&store); return; }
        dom_script(&f, STORAGE_ASSERT "assert(localStorage.length===0);localStorage.setItem('a','1');"
                   "assert(localStorage.getItem('a')==='1');", "storage: an unreadable store");
        os64_dom_drain(f.binding);
        check(store.commits == 0, how == 0 ? "storage: a store that cannot be read is never written over"
                                           : "storage: a store not in the format is never written over");
        fixture_free(&f);
        fake_store_free(&store);
    }

    /* A store bigger than any image libdom writes is one it did not write:
     * never read, never written over. */
    store.claimed = (int64_t)64 * 1024 * 1024;
    f = storage_fixture("http://example.test/", &store);
    if (!fixture_ready(&f)) { fake_store_free(&store); return; }
    dom_script(&f, STORAGE_ASSERT "assert(localStorage.length===0);localStorage.setItem('a','1');"
               "assert(localStorage.getItem('a')==='1');", "storage: a store past any image");
    os64_dom_drain(f.binding);
    check(store.loads == 1 && store.commits == 0, "storage: a store past any image is never read or written over");
    fixture_free(&f);
    fake_store_free(&store);

    /* A store that grows every time it is asked: tried a few times, then
     * at the next task; the page's changes still reach it. */
    fake_store_set(&store, "os64 storage 1\n+5 6\nthereyonder\n");
    store.outgrow = true;
    f = storage_fixture("http://example.test/", &store);
    if (!fixture_ready(&f)) { fake_store_free(&store); return; }
    dom_script(&f, STORAGE_ASSERT "localStorage.setItem('mine','1');", "storage: a store that keeps growing");
    check(store.loads > 1 && store.loads <= 8, "storage: a growing store is asked again, a few times");
    unsigned growing = store.loads;
    os64_dom_storage_settle(f.binding);
    store.outgrow = false;
    dom_script(&f, STORAGE_ASSERT "assert(localStorage.getItem('there')==='yonder',String(localStorage.getItem('there')));"
               "assert(localStorage.getItem('mine')==='1',String(localStorage.getItem('mine')));",
               "storage: a store that stopped growing");
    check(store.loads == growing + 1, "storage: the next task reads the store once it holds still");
    fixture_free(&f);
    fake_store_free(&store);

    /* A key and a value holding the format's own bytes round-trip through
     * the store: the records are counted, never split on a byte. */
    f = storage_fixture("http://example.test/", &store);
    if (!fixture_ready(&f)) { fake_store_free(&store); return; }
    dom_script(&f, "localStorage.setItem('k\\n+1 1\\nx\\t','v\\n-1\\nk\\n');", "storage: the format's bytes in a key");
    os64_dom_drain(f.binding);
    fixture_free(&f);
    f = storage_fixture("http://example.test/", &store);
    if (!fixture_ready(&f)) { fake_store_free(&store); return; }
    dom_script(&f, STORAGE_ASSERT "assert(localStorage.length===1);"
               "assert(localStorage.getItem('k\\n+1 1\\nx\\t')==='v\\n-1\\nk\\n');",
               "storage: a key holding newlines, tabs and record heads round-trips");
    fixture_free(&f);
    fake_store_free(&store);

    /* Key after key set and removed in one task: the change list would
     * hold every removed key until its commit, so past the quota it is
     * committed early, and what reaches the store is the page's last word. */
    f = storage_fixture("http://example.test/", &store);
    if (!fixture_ready(&f)) { fake_store_free(&store); return; }
    dom_script(&f, STORAGE_ASSERT "var big='x'.repeat(200000);"
               "for(var i=0;i<40;i++){localStorage.setItem(i+big,'v');localStorage.removeItem(i+big)}"
               "localStorage.setItem('last','1');assert(localStorage.length===1);"
               "for(var j=0;j<10;j++)localStorage.setItem('k'+j+big,'');localStorage.clear();"
               "assert(localStorage.length===0);",
               "storage: a task that sets and removes key after key");
    unsigned early = store.commits;
    check(early >= 2, "storage: a change list past the quota is committed before it is due");
    os64_dom_drain(f.binding);
    check(store.length == 0 && store.commits >= early, "storage: and the store holds the page's last word");
    fixture_free(&f);
    fake_store_free(&store);

    /* Forgetting: the maps and their uncommitted changes go, and the next
     * touch reads the store. */
    f = storage_fixture("http://example.test/", &store);
    if (!fixture_ready(&f)) return;
    dom_script(&f, "localStorage.setItem('a','1')", "storage: a change to forget");
    os64_dom_storage_forget(f.binding);
    dom_script(&f, STORAGE_ASSERT "assert(localStorage.length===0);", "storage: forgotten");
    os64_dom_drain(f.binding);
    check(store.commits == 0, "storage: a forgotten change is never committed");
    fixture_free(&f);
    fake_store_free(&store);
}

/* fixture_free's teardown, for a page freed while another lives: the
 * other's storage is still allocated, so "everything came back" is the
 * last one's to check. */
static void storage_fixture_release(DomFixture *f)
{
    os64_dom_drain(f->binding);
    os64_js_destroy(f->runtime);
    os64_dom_free(f->binding);
    os64_page_state_free(f->state);
    os64_html_document_free(f->document);
    memset(f, 0, sizeof(*f));
}

/* Two pages on one store, each committing its own changes: nothing of one
 * is lost to the other but a key both wrote, where the later wins. */
static void dom_storage_two_windows_cases(void)
{
    FakeStore store = {0};
    storage_clock = 0;
    DomFixture a = storage_fixture("http://example.test/a", &store);
    DomFixture b = storage_fixture("http://example.test/b", &store);
    if (a.binding == NULL || b.binding == NULL) {
        check(false, "storage: two pages' bindings");
        storage_fixture_release(&a);
        fixture_free(&b);
        fake_store_free(&store);
        return;
    }
    dom_script(&a, "localStorage.getItem('x')", "storage: window A reads first");
    dom_script(&b, "localStorage.getItem('x')", "storage: window B reads first");
    dom_script(&a, "localStorage.setItem('a','from A');localStorage.setItem('both','A')", "storage: A writes");
    dom_script(&b, "localStorage.setItem('b','from B');localStorage.setItem('both','B')", "storage: B writes");
    os64_dom_storage_settle(a.binding);
    os64_dom_storage_settle(b.binding);
    dom_script(&a, STORAGE_ASSERT "assert(localStorage.getItem('a')==='from A'&&localStorage.getItem('b')==='from B');"
               "assert(localStorage.getItem('both')==='B');", "storage: A sees B's key, and B's later write wins");
    dom_script(&b, STORAGE_ASSERT "assert(localStorage.getItem('a')==='from A'&&localStorage.getItem('b')==='from B');",
               "storage: B sees A's key");
    storage_fixture_release(&a);
    fixture_free(&b);
    fake_store_free(&store);
}

/* The store's bytes: the walk and the merge. */
typedef struct { char seen[512]; size_t used; unsigned count; } Walked;
static bool walked(void *opaque, const char *key, size_t key_length, const char *value, size_t value_length)
{
    Walked *w = opaque;
    w->count++;
    w->used += (size_t)snprintf(w->seen + w->used, sizeof(w->seen) - w->used, "%s%.*s=%s%.*s;",
                                value == NULL ? "-" : "+", (int)key_length, key, value == NULL ? "" : "",
                                (int)value_length, value != NULL ? value : "");
    return w->count < 3;
}

static void dom_storage_bytes_cases(void)
{
    static const char kImage[] = "os64 storage 1\n+1 1\nab\n+2 0\ncd\n";
    Walked w = {{0}, 0, 0};
    check(os64_dom_storage_walk(kImage, sizeof(kImage) - 1, walked, &w) && !strcmp(w.seen, "+a=b;+cd=;"),
          "storage: the walk hands each record over, in order");
    static const char kChanges[] = "os64 storage 1\n-1\na\n+1 1\nxy\n+1 1\nzw\n+1 1\nqr\n";
    w = (Walked){{0}, 0, 0};
    check(os64_dom_storage_walk(kChanges, sizeof(kChanges) - 1, walked, &w) && w.count == 3 &&
          !strcmp(w.seen, "-a=;+x=y;+z=w;"), "storage: a removal has no value, and `each` may stop the walk");
    static const char *const kBad[] = {
        "", "os64 storage 2\n", "os64 storage 1\n+1 1\nab", "os64 storage 1\n+1 1\nabX",
        "os64 storage 1\n+1\nab\n", "os64 storage 1\n*1 1\nab\n", "os64 storage 1\n+01 1\nab\n",
        "os64 storage 1\n+9 1\nab\n", "os64 storage 1\n+ 1\nab\n", "os64 storage 1\n-1 1\na\n",
        "os64 storage 1\n+99999999999999 1\nab\n",
    };
    for (size_t i = 0; i < sizeof(kBad) / sizeof(kBad[0]); i++) {
        bool zero_lead = strstr(kBad[i], "+01 ") != NULL;
        bool ok = os64_dom_storage_walk(kBad[i], strlen(kBad[i]), NULL, NULL);
        /* A leading zero is still a number; the rest are not the format. */
        check(zero_lead ? ok : !ok, "storage: the walk refuses bytes not in the format");
    }
    void *out;
    size_t length;
    static const char kSets[] = "os64 storage 1\n+1 1\nxy\n+1 1\nzw\n+1 1\nqr\n";
    check(os64_dom_storage_merge(NULL, 0, kChanges, sizeof(kChanges) - 1, &out, &length) == OS64_DOM_STORAGE_OK &&
          length == sizeof(kSets) - 1 && !memcmp(out, kSets, length),
          "storage: a merge onto no store keeps the sets, in order");
    void *again;
    size_t again_length;
    static const char kMore[] = "os64 storage 1\n+1 2\nzWW\n-1\nx\n+1 0\nn\n";
    static const char kMerged[] = "os64 storage 1\n+1 1\nqr\n+1 2\nzWW\n+1 0\nn\n";
    check(os64_dom_storage_merge(out, length, kMore, sizeof(kMore) - 1, &again, &again_length) == OS64_DOM_STORAGE_OK &&
          again_length == sizeof(kMerged) - 1 && !memcmp(again, kMerged, again_length),
          "storage: a set keeps its key's place, a removal's place takes the last key, a new key goes last");
    dom_host_free(out);
    static const char kAllGone[] = "os64 storage 1\n-1\nz\n-1\nq\n-1\nn\n-5\nnever\n";
    check(os64_dom_storage_merge(again, again_length, kAllGone, sizeof(kAllGone) - 1, &out, &length) ==
          OS64_DOM_STORAGE_OK && out == NULL && length == 0, "storage: a store left empty is no image");
    check(os64_dom_storage_merge(again, again_length, "junk", 4, &out, &length) == OS64_DOM_STORAGE_FAILED &&
          out == NULL, "storage: changes not in the format are refused");
    check(os64_dom_storage_merge(kChanges, sizeof(kChanges) - 1, kMore, sizeof(kMore) - 1, &out, &length) ==
          OS64_DOM_STORAGE_FAILED && out == NULL, "storage: an image with a removal in it is not an image");
    check(os64_dom_storage_merge(NULL, 3, kMore, sizeof(kMore) - 1, &out, &length) == OS64_DOM_STORAGE_FAILED &&
          os64_dom_storage_merge(NULL, 0, NULL, 0, &out, &length) == OS64_DOM_STORAGE_FAILED &&
          os64_dom_storage_merge(NULL, 0, kMore, sizeof(kMore) - 1, NULL, &length) == OS64_DOM_STORAGE_FAILED,
          "storage: a merge without its arguments is refused");
    check(os64_dom_storage_merge("", 0, kMore, sizeof(kMore) - 1, &out, &length) == OS64_DOM_STORAGE_FAILED &&
          out == NULL, "storage: an empty file's bytes are no image (no store is NULL)");
    dom_host_free(again);
    /* Past the quota: refused when the merge would hold more than the
     * quota and more than the store did; a store already past it may
     * still shrink. */
    size_t big = OS64_DOM_STORAGE_QUOTA / 2;        /* code units: the whole quota */
    char *records = malloc(big + 64);
    if (records == NULL) abort();
    int head = snprintf(records, 64, "os64 storage 1\n+1 %zu\nk", big - 1);
    memset(records + head, 'v', big - 1);
    records[head + big - 1] = '\n';
    size_t records_length = (size_t)head + big;
    void *full;
    size_t full_length;
    check(os64_dom_storage_merge(NULL, 0, records, records_length, &full, &full_length) == OS64_DOM_STORAGE_OK,
          "storage: a store exactly at the quota is kept");
    check(os64_dom_storage_merge(full, full_length, "os64 storage 1\n+1 1\nab\n", 23, &out, &length) ==
          OS64_DOM_STORAGE_FULL && out == NULL, "storage: one more code unit past the quota is refused");
    check(os64_dom_storage_merge(full, full_length, "os64 storage 1\n-1\nk\n", 20, &out, &length) ==
          OS64_DOM_STORAGE_OK && out == NULL, "storage: a full store may always shrink");
    dom_host_free(full);
    free(records);
    dom_storage_merge_work_cases();
}

/* Which of the keys k0..k(n-1) a walk meets, and how often. */
typedef struct { unsigned char *seen; size_t n; bool stray; } KeysSeen;
static bool keys_seen(void *opaque, const char *key, size_t key_length, const char *value, size_t value_length)
{
    (void)value; (void)value_length;
    KeysSeen *s = opaque;
    size_t index = 0;
    if (key_length < 2 || key[0] != 'k') { s->stray = true; return true; }
    for (size_t i = 1; i < key_length; i++) index = index * 10 + (size_t)(key[i] - '0');
    if (index >= s->n || s->seen[index]++ != 0) s->stray = true;
    return true;
}

/* A change list's work is linear in its length: clear() over the most keys
 * an area may hold, and a scattered half removed, each counted in the
 * maps' steps (d_storage_steps), never timed. And what is left is exactly
 * the keys not removed, whatever order the removals came in. */
static void dom_storage_merge_work_cases(void)
{
    size_t n = 65536;
    char *image = malloc(32 + n * 24), *changes = malloc(32 + n * 24);
    if (image == NULL || changes == NULL) abort();
    size_t il = (size_t)sprintf(image, "os64 storage 1\n"), cl = (size_t)sprintf(changes, "os64 storage 1\n");
    for (size_t i = 0; i < n; i++) {
        char key[16];
        int kl = snprintf(key, sizeof(key), "k%zu", i);
        il += (size_t)sprintf(image + il, "+%d 0\n%s\n", kl, key);
    }
    for (size_t i = 0; i < n; i++) {
        char key[16];
        int kl = snprintf(key, sizeof(key), "k%zu", (i * 40503u) % n);
        cl += (size_t)sprintf(changes + cl, "-%d\n%s\n", kl, key);
    }
    void *out;
    size_t length;
    uint64_t before = d_storage_steps();
    check(os64_dom_storage_merge(image, il, changes, cl, &out, &length) == OS64_DOM_STORAGE_OK && out == NULL,
          "storage: a clear of the most keys an area holds empties the store");
    uint64_t steps = d_storage_steps() - before;
    check(steps < 64 * (uint64_t)n, "storage: and its work is linear in the keys");
    /* Every other key, in a scattered order: the rest are kept, once each. */
    cl = (size_t)sprintf(changes, "os64 storage 1\n");
    for (size_t i = 0; i < n; i++) {
        size_t k = (i * 40503u) % n;
        if (k % 2 != 0) continue;
        char key[16];
        int kl = snprintf(key, sizeof(key), "k%zu", k);
        cl += (size_t)sprintf(changes + cl, "-%d\n%s\n", kl, key);
    }
    check(os64_dom_storage_merge(image, il, changes, cl, &out, &length) == OS64_DOM_STORAGE_OK && out != NULL,
          "storage: half the keys removed");
    unsigned char *seen = calloc(n, 1);
    if (seen == NULL) abort();
    KeysSeen s = {seen, n, false};
    check(os64_dom_storage_walk(out, length, keys_seen, &s) && !s.stray, "storage: the walk meets each key once");
    bool exact = true;
    for (size_t i = 0; i < n; i++) exact = exact && seen[i] == (i % 2 != 0 ? 1 : 0);
    check(exact, "storage: and exactly the keys not removed are kept");
    dom_host_free(out);
    free(seen);
    free(image);
    free(changes);
}

/* Every allocation the binding makes for storage, failed in turn: the
 * script sees QuotaExceededError, the area is as it was, and every byte
 * comes back. */
static void dom_storage_allocation_cases(void)
{
    static const char *const kOperations[] = {
        "localStorage.setItem('new','value')", "localStorage.setItem('k','changed')",
        "localStorage.removeItem('k')", "localStorage.clear()", "localStorage.getItem('k')",
        "localStorage.newer='v'", "delete localStorage.k", "Object.keys(localStorage)",
        "sessionStorage.setItem('s','v')",
    };
    static const char kBefore[] =
        "globalThis.before=JSON.stringify(Object.entries(localStorage).concat(Object.entries(sessionStorage)))";
    FakeStore store = {0};
    for (int kept = 0; kept < 2; kept++)
        for (size_t op = 0; op < sizeof(kOperations) / sizeof(kOperations[0]); op++) {
            DomFixture f = storage_fixture("http://example.test/", kept ? &store : NULL);
            if (!fixture_ready(&f)) break;
            dom_script(&f, "localStorage.setItem('k','v');localStorage.setItem('k2','v2')", "storage: the sweep's fixture");
            os64_dom_storage_settle(f.binding);
            dom_script(&f, kBefore, "storage: the area before");
            size_t start = dom_attempts;
            dom_script(&f, kOperations[op], "storage: an operation measured");
            size_t count = dom_attempts - start;
            printf("Storage operation %zu%s allocation points: %zu\n", op, kept ? " (kept)" : "", count);
            fixture_free(&f);
            fake_store_free(&store);
            for (size_t cut = 1; cut <= count; cut++) {
                f = storage_fixture("http://example.test/", kept ? &store : NULL);
                if (!fixture_ready(&f)) break;
                dom_script(&f, "localStorage.setItem('k','v');localStorage.setItem('k2','v2')", "storage: the sweep's fixture");
                os64_dom_storage_settle(f.binding);
                dom_script(&f, kBefore, "storage: the area before");
                char source[512];
                snprintf(source, sizeof(source),
                         "try{%s;globalThis.refused=false}catch(e){if(e.name!=='QuotaExceededError')throw e;globalThis.refused=true}",
                         kOperations[op]);
                size_t target = dom_attempts + cut;
                dom_fail_at = target;
                int ran = dom_script(&f, source, "storage: an allocation cut");
                dom_fail_at = 0;
                dom_sweeps++;
                check(dom_attempts >= target, "storage: the cut was reached");
                if (ran)
                    dom_script(&f, STORAGE_ASSERT
                        "if(refused)assert(JSON.stringify(Object.entries(localStorage).concat(Object.entries(sessionStorage)))===before,"
                        "'a refused operation changed the area');", "storage: a refusal changes nothing");
                os64_dom_drain(f.binding);
                fixture_free(&f);
                fake_store_free(&store);
            }
        }
    /* A reload, cut at each allocation: the page sees the map it had or
     * the store with its own changes over it, never part of either, and
     * the next task reads the store again. */
    size_t reload_count = 0;
    storage_clock = 0;
    for (size_t cut = 0; cut <= reload_count; cut++) {
        DomFixture f = storage_fixture("http://example.test/", &store);
        if (!fixture_ready(&f)) break;
        dom_script(&f, "localStorage.setItem('k','v')", "storage: a reload's fixture");
        os64_dom_storage_settle(f.binding);
        dom_script(&f, "localStorage.setItem('mine','m')", "storage: an uncommitted change");
        fake_store_set(&store, "os64 storage 1\n+5 1\nother1\n");
        os64_dom_storage_settle(f.binding);         /* commits nothing: not yet due */
        size_t start = dom_attempts;
        if (cut != 0) dom_fail_at = dom_attempts + cut;
        dom_script(&f, "try{globalThis.seen=Object.entries(localStorage).map(e=>e.join('=')).sort().join()}"
                   "catch(e){if(e.name!=='QuotaExceededError')throw e;globalThis.seen='refused'}",
                   "storage: a touch that reloads");
        dom_fail_at = 0;
        if (cut == 0) {
            reload_count = dom_attempts - start;
            printf("Storage reload allocation points: %zu\n", reload_count);
        } else dom_sweeps++;
        dom_script(&f, STORAGE_ASSERT
            "assert(seen==='refused'||seen==='k=v,mine=m'||seen==='k=v,mine=m,other=1',seen);",
            "storage: a reload out of memory leaves the map whole");
        os64_dom_storage_settle(f.binding);
        dom_script(&f, STORAGE_ASSERT "assert(localStorage.getItem('other')==='1'&&localStorage.getItem('mine')==='m');",
                   "storage: the next task reads the store");
        fixture_free(&f);
        fake_store_free(&store);
    }

    /* The merge, cut at each allocation: refused, nothing handed back. */
    static const char kChanges[] = "os64 storage 1\n+1 1\nab\n-1\nc\n";
    size_t start = dom_attempts;
    void *out;
    size_t length;
    check(os64_dom_storage_merge(NULL, 0, kChanges, sizeof(kChanges) - 1, &out, &length) == OS64_DOM_STORAGE_OK,
          "storage: the merge measured");
    dom_host_free(out);
    size_t count = dom_attempts - start;
    for (size_t cut = 1; cut <= count; cut++) {
        dom_fail_at = dom_attempts + cut;
        os64_dom_storage_status_t status = os64_dom_storage_merge(NULL, 0, kChanges, sizeof(kChanges) - 1, &out, &length);
        dom_fail_at = 0;
        dom_sweeps++;
        check(status == OS64_DOM_STORAGE_FAILED && out == NULL, "storage: a merge out of memory is refused whole");
    }
    check(dom_live == 0, "storage: every allocation came back");
}

static void dom_storage_engine_cases(void)
{
    static const char *const kEngine[] = {
        "localStorage.setItem('a','1');localStorage.b='2';Object.keys(localStorage).join()+localStorage.getItem('a')",
        "sessionStorage.setItem('\\ud800','x');delete sessionStorage['\\ud800'];sessionStorage.length",
        "var k=Symbol();localStorage[k]=1;localStorage.clear();localStorage.key(0)",
    };
    engine_sweep("storage", kEngine, sizeof(kEngine) / sizeof(kEngine[0]), NULL);
}

/* Page code that runs inside a property lookup (a Proxy prototype's `has`
 * trap, which the named-property visibility check consults) and changes
 * the map: the answer is the key's own, looked up after the trap. */
static void dom_storage_page_code_cases(void)
{
    DomFixture f = storage_fixture("http://example.test/", NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "localStorage.setItem('a','1');localStorage.setItem('b','2');localStorage.setItem('c','3');"
        "var done=false;"
        "Object.setPrototypeOf(Storage.prototype,new Proxy({},{has:function(t,k){"
        "if(k==='b'&&!done){done=true;localStorage.removeItem('a');}return false;}}));"
        "assert(localStorage.b==='2',String(localStorage.b));"
        "done=false;localStorage.setItem('a','1');"
        "Object.setPrototypeOf(Storage.prototype,new Proxy({},{has:function(t,k){"
        "if(k==='c'&&!done){done=true;localStorage.removeItem('c');}return false;}}));"
        "assert(Object.getOwnPropertyDescriptor(localStorage,'c')===undefined,'a key the trap removed is gone');"
        "done=false;localStorage.setItem('c','3');"
        "Object.setPrototypeOf(Storage.prototype,new Proxy({},{has:function(t,k){"
        "if(!done){done=true;localStorage.clear();localStorage.setItem('z','9');}return false;}}));"
        "var names=Object.keys(localStorage);assert(names.length===names.filter(function(n,i){return names.indexOf(n)===i}).length,"
        "'no name twice');"
        "Object.setPrototypeOf(Storage.prototype,Object.prototype);",
        "storage: page code inside a lookup changes the map");
    fixture_free(&f);
}

/* A commit that could not even write its change list down (no memory)
 * waits COMMIT_MS for the next: the host's loop is told a time to wait
 * for, not "now" turn after turn. */
static void dom_storage_commit_memory_cases(void)
{
    FakeStore store = {0};
    storage_clock = 500;
    DomFixture f = storage_fixture("http://example.test/", &store);
    if (!fixture_ready(&f)) return;
    dom_script(&f, "localStorage.setItem('k',new Array(100001).join('v'))", "storage: a change too big to write down now");
    check(os64_dom_storage_due(f.binding) <= storage_clock, "storage: due at once");
    fail_size = strlen("os64 storage 1\n") + strlen("+1 100000\n") + 1 + 100000 + 1;
    os64_dom_storage_settle(f.binding);
    fail_size = 0;
    check(store.commits == 0 && os64_dom_storage_due(f.binding) == storage_clock + OS64_DOM_STORAGE_COMMIT_MS,
          "storage: a commit with no memory to write its list waits a second");
    storage_clock += OS64_DOM_STORAGE_COMMIT_MS;
    os64_dom_storage_settle(f.binding);
    check(store.commits == 1 && store.length != 0, "storage: and is made then");
    fixture_free(&f);
    fake_store_free(&store);
    storage_clock = 0;
}

/* The change list's bound holds whatever the commit manages: a change
 * that would pass it, when the early commit cannot make room, is refused
 * as a full store's would be. */
static void dom_storage_bound_cases(void)
{
    FakeStore store = {0};
    DomFixture f = storage_fixture("http://example.test/", &store);
    if (!fixture_ready(&f)) return;
    /* The early commit at the third key cannot write its list down (two
     * removals of 1,000,001-byte keys): no room is made. */
    fail_size = strlen("os64 storage 1\n") + 2 * (strlen("-1000001\n") + 1000001 + 1);
    dom_script(&f, STORAGE_ASSERT "var big='x'.repeat(1000000),stopped=-1;"
               "for(var i=0;i<6&&stopped<0;i++){try{localStorage.setItem(i+big,'');localStorage.removeItem(i+big)}"
               "catch(e){assert(e.name==='QuotaExceededError',e.name);stopped=i}}"
               "assert(stopped===2,'refused at the change that would pass the bound: '+stopped);"
               "assert(localStorage.length===0);",
               "storage: no memory for the early commit, and set and remove of key after big key");
    fail_size = 0;
    check(store.commits == 0, "storage: the early commit could not be made");
    os64_dom_drain(f.binding);
    check(store.commits == 1 && store.length == 0, "storage: what the page could change reached the store at drain");
    fixture_free(&f);
    fake_store_free(&store);

    /* clear() over sets not yet committed replaces their records, so it
     * asks for no more room than it takes: four sets fill the quota, the
     * list could not be written down for an early commit, and clear()
     * still fits. */
    FakeStore filled = {0};
    f = storage_fixture("http://example.test/", &filled);
    if (!fixture_ready(&f)) return;
    size_t value = OS64_DOM_STORAGE_QUOTA / 8 - 1;
    char script[512];
    snprintf(script, sizeof(script),
             STORAGE_ASSERT "var v='v'.repeat(%zu);for(var i=0;i<4;i++)localStorage.setItem(String(i),v);"
             "localStorage.clear();assert(localStorage.length===0);", value);
    fail_size = strlen("os64 storage 1\n") + 4 * (strlen("+1 \n") + strlen("655359") + 1 + value + 1);
    dom_script(&f, script, "storage: clear() over a full list of sets the commit could not write down");
    fail_size = 0;
    check(filled.commits == 0, "storage: clear() needed no early commit");
    os64_dom_drain(f.binding);
    check(filled.commits == 1 && filled.length == 0, "storage: and its removals reached the store at drain");
    fixture_free(&f);
    fake_store_free(&filled);
}

static void dom_storage_cases(void)
{
    dom_storage_shape_cases();
    dom_storage_page_code_cases();
    dom_storage_bound_cases();
    dom_storage_commit_memory_cases();
    dom_storage_quota_cases();
    dom_storage_provider_cases();
    dom_storage_two_windows_cases();
    dom_storage_bytes_cases();
    dom_storage_allocation_cases();
}
