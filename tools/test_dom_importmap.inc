/* IMPORT MAPS, THE BINDING'S HALF (MODERN_WEB.md § Import maps, as built;
 * dom.h os64_dom_import_map): an import map's JSON parsed as one task, its entries
 * walked to the host in JSON.parse's order, and each failure "parse an
 * import map string" throws reported at the window before anything is
 * walked. And HTMLScriptElement.supports. */

typedef struct {
    char lines[2048];
    size_t used;
    int calls, refuse_at;
} MapWalkLog;

static bool map_walk_entry(void *opaque, const char *scope, const char *key, const char *value)
{
    MapWalkLog *log = opaque;
    log->calls++;
    if (log->refuse_at != 0 && log->calls == log->refuse_at) return false;
    log->used += (size_t)snprintf(log->lines + log->used, sizeof(log->lines) - log->used, "%s|%s|%s\n",
                                  scope != NULL ? scope : "-", key, value != NULL ? value : "(none)");
    return true;
}

static os64_js_status_t map_parse(DomFixture *f, const char *json, size_t length, MapWalkLog *log,
                                  os64_js_outcome_t *outcome)
{
    int refuse_at = log->refuse_at;
    memset(log, 0, sizeof(*log));
    log->refuse_at = refuse_at;
    const os64_dom_import_map_walk_t walk = {log, map_walk_entry};
    return os64_dom_import_map(f->binding, P2B_URL "#inline-1", json, length, &walk, outcome);
}

static void dom_import_map_cases(void)
{
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    p2b_script(&f,
        "globalThis.errors=[];addEventListener('error',e=>errors.push(e.error&&e.error.name));"
        "const S=HTMLScriptElement.supports;"
        "assert(typeof S==='function'&&S.length===1&&S.name==='supports');"
        "assert(S('importmap')===true&&S('module')===true&&S('classic')===true,'the three types this window runs');"
        "assert(!S('speculationrules')&&!S('IMPORTMAP')&&!S(' module')&&!S('')&&!S(),"
        "'nothing else, in no other case, and undefined is \"undefined\"');"
        "assert(S.call(null,'module')===true,'a static: this is nothing to it');",
        "importmap: HTMLScriptElement.supports");
    MapWalkLog log = {0};
    os64_js_outcome_t outcome;
    static const char kMap[] =
        "{\"imports\":{\"b\":\"/b.js\",\"a\":1,\"2\":\"/two.js\",\"n\":null},"
        "\"scopes\":{\"/s/\":{\"x\":\"./x.js\"},\"/t/\":{}},\"integrity\":{\"/b.js\":\"sha384-x\"},\"extra\":1}";
    check(map_parse(&f, kMap, sizeof(kMap) - 1, &log, &outcome) == OS64_JS_OK &&
          !strcmp(log.lines, "-|2|/two.js\n-|b|/b.js\n-|a|(none)\n-|n|(none)\n/s/|x|./x.js\n"),
          "importmap: the entries walked in JSON.parse's order, a value no string as none; integrity, an "
          "empty scope and an unknown key walk nothing");
    /* Each failure "parse an import map string" throws, reported at the
     * window, with nothing walked before it: the last throws for a scope
     * after an "imports" that was whole. */
    static const struct { const char *json, *name; } kFailures[] = {
        {"{imports:{}}", "SyntaxError"}, {"[]", "TypeError"}, {"null", "TypeError"}, {"\"x\"", "TypeError"},
        {"{\"imports\":[]}", "TypeError"}, {"{\"imports\":null}", "TypeError"}, {"{\"scopes\":1}", "TypeError"},
        {"{\"scopes\":{\"/a/\":\"x\"}}", "TypeError"}, {"{\"integrity\":[]}", "TypeError"},
        {"{\"imports\":{\"a\":\"/a.js\"},\"scopes\":{\"/s/\":[]}}", "TypeError"},
    };
    bool all = true;
    for (size_t i = 0; i < sizeof(kFailures) / sizeof(kFailures[0]); i++) {
        bool threw = map_parse(&f, kFailures[i].json, strlen(kFailures[i].json), &log, &outcome) ==
                         OS64_JS_EXCEPTION && log.calls == 0 && strstr(outcome.message, kFailures[i].name) != NULL &&
                     !strcmp(outcome.source_name, P2B_URL "#inline-1");
        if (!threw) fprintf(stderr, "import map failure %zu: %s\n", i, outcome.message);
        all = all && threw;
    }
    check(all, "importmap: a syntax error and every shape the parse refuses throw, under the script's name, "
               "before an entry is walked");
    p2b_script(&f,
        "assert(errors.join()==='SyntaxError,'+Array(9).fill('TypeError').join(),errors.join());errors.length=0;",
        "importmap: each failure fired the window's error with its exception");
    /* What a page put on Object.prototype is no part of the JSON. */
    p2b_script(&f, "Object.prototype.imports={a:'/evil.js'};Object.prototype.scopes=1;", "importmap: a page's pollution");
    check(map_parse(&f, "{}", 2, &log, &outcome) == OS64_JS_OK && log.calls == 0,
          "importmap: an inherited imports or scopes is not the map's");
    p2b_script(&f, "delete Object.prototype.imports;delete Object.prototype.scopes;", "importmap: the pollution goes");
    /* A NUL: a key holding one is passed over, a value holding one is none. */
    static const char kNul[] = "{\"imports\":{\"a\\u0000b\":\"/x\",\"c\":\"/y\\u0000z\",\"d\":\"/d\"}}";
    check(map_parse(&f, kNul, sizeof(kNul) - 1, &log, &outcome) == OS64_JS_OK &&
          !strcmp(log.lines, "-|c|(none)\n-|d|/d\n"),
          "importmap: a key holding a NUL is passed over, a value holding one is none");
    /* An entry the host cannot keep fails the parse: the page's map is too
     * large, and the runtime goes on. */
    log.refuse_at = 2;
    static const char kTwo[] = "{\"imports\":{\"a\":\"/a\",\"b\":\"/b\"}}";
    check(map_parse(&f, kTwo, sizeof(kTwo) - 1, &log, &outcome) == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "RangeError: The import map is larger than yonder keeps") != NULL,
          "importmap: an entry the host refuses fails the parse with a RangeError");
    log.refuse_at = 0;
    p2b_script(&f, "assert(errors.length===1,errors.join());", "importmap: and is reported");
    /* The text must end where it says: a host's slip is refused, not read. */
    static const char kLong[] = "{}xyz";
    check(map_parse(&f, kLong, 2, &log, &outcome) == OS64_JS_BAD_ARGUMENT,
          "importmap: a text not NUL-terminated at its length is refused");
    fixture_free(&f);
}

/* Every engine allocation of a parse that walks entries and of one that
 * throws, failed in turn: whatever the cut, the binding is left whole and
 * its teardown releases everything (fixture_free). */
static size_t import_map_sweep_run(const char *json, size_t cut)
{
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return 0;
    dom_script(&f, "addEventListener('error',e=>{globalThis.heard=e.error})", "importmap sweep: setup");
    MapWalkLog log = {0};
    os64_js_outcome_t outcome;
    size_t start = attempts;
    if (cut != 0) fail_at = attempts + cut;
    map_parse(&f, json, strlen(json), &log, &outcome);
    fail_at = 0;
    size_t count = attempts - start;
    fixture_free(&f);
    return count;
}

static void dom_import_map_sweep_cases(void)
{
    static const char *const kMaps[] = {
        "{\"imports\":{\"react\":\"/r.js\",\"react/\":\"/r/\"},\"scopes\":{\"/app/\":{\"x\":\"./x.js\"}}}",
        "{\"imports\":{\"a\":\"/a.js\"},\"scopes\":{\"/s/\":[]}}",
    };
    for (size_t m = 0; m < sizeof(kMaps) / sizeof(kMaps[0]); m++) {
        size_t points = import_map_sweep_run(kMaps[m], 0);
        printf("import map %zu allocation points: %zu\n", m, points);
        for (size_t cut = 1; cut <= points; cut++) {
            import_map_sweep_run(kMaps[m], cut);
            dom_sweeps++;
        }
    }
}
