// test_importmap_host.c — yonder's import map (userland/apps/yonder/
// importmap.c) on the host: WPT's data-driven cases, the merge's rules, and
// every allocation failing in turn.
//
//   test_importmap_host <cases>     the cases tools/wpt/importmap_cases.py
//                                   wrote; one PASS or FAIL line each
//   test_importmap_host --merge     the merge's rules, HTML's own examples
//   test_importmap_host --scale     a merge and resolutions a page sizes,
//                                   held to a CPU bound
//   test_importmap_host --sweep     each allocation of a parse, two merges
//                                   and their resolutions failed in turn
//
// The data-driven cases' verdicts are judged against the expectation file
// by tools/test_importmap_host.sh; the other two modes judge themselves and
// answer 1 on any failure.

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "importmap.h"

int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    (void)handle;
    (void)buf;
    return (int64_t)len;
}

void os64_exit(int32_t code)
{
    fprintf(stderr, "importmap host fatal exit: 0x%08x\n", (unsigned)code);
    exit(3);
}

// ── Allocation, and failing it on purpose ───────────────────────────────

static size_t allocations, fail_at, live;

void *os64_malloc(size_t size)
{
    allocations++;
    if (fail_at != 0 && allocations == fail_at)
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
    if (fail_at != 0 && allocations == fail_at)
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

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

// ── The data-driven cases ───────────────────────────────────────────────

#define FIELDS 5
#define CAP 16384

// A line split at its tabs, each field decoded in place; a field that was
// \N is NULL. The count of fields.
static int split(char *line, char *fields[FIELDS])
{
    int n = 0;
    for (char *at = line; n < FIELDS;) {
        char *tab = strchr(at, '\t');
        if (tab != NULL)
            *tab = '\0';
        fields[n++] = at;
        if (tab == NULL)
            break;
        at = tab + 1;
    }
    for (int i = 0; i < n; i++) {
        if (strcmp(fields[i], "\\N") == 0) {
            fields[i] = NULL;
            continue;
        }
        char *out = fields[i];
        for (const char *in = fields[i]; *in != '\0'; in++) {
            if (*in == '\\' && in[1] != '\0') {
                in++;
                *out++ = *in == 't' ? '\t' : *in == 'n' ? '\n' : *in;
            } else {
                *out++ = *in;
            }
        }
        *out = '\0';
    }
    return n;
}

static void verdict(bool pass, const char *name, const char *what, const char *got)
{
    if (pass)
        printf("PASS\t%s: %s\n", name, what);
    else
        printf("FAIL\t%s: %s\t%s\n", name, what, got != NULL ? got : "");
}

static int data_cases(const char *path)
{
    FILE *in = fopen(path, "r");
    if (in == NULL) {
        fprintf(stderr, "test_importmap_host: cannot read %s\n", path);
        return 2;
    }
    static char line[4 * CAP], name[CAP], base[CAP], expected[CAP * 4], got[CAP * 4];
    yonder_import_map_t *parsed = NULL, *window = NULL;
    bool error = false, added = true;
    size_t expected_len = 0;
    while (fgets(line, sizeof(line), in) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        char *f[FIELDS];
        int n = split(line, f);
        if (strcmp(f[0], "CASE") == 0 && n == 2) {
            snprintf(name, sizeof(name), "%s", f[1]);
            parsed = yonder_import_map_new();
            window = NULL;
            error = false;
            added = parsed != NULL;
            expected[0] = '\0';
            expected_len = 0;
        } else if (strcmp(f[0], "MAP") == 0 && n == 2) {
            snprintf(base, sizeof(base), "%s", f[1]);
        } else if (strcmp(f[0], "ERROR") == 0) {
            error = true;
        } else if (strcmp(f[0], "ENTRY") == 0 && n == 4) {
            added = added && yonder_import_map_add(parsed, base, f[1], f[2], f[3]);
        } else if (strcmp(f[0], "DUMP") == 0 && (n == 4 || n == 5)) {
            expected_len += (size_t)snprintf(expected + expected_len, sizeof(expected) - expected_len,
                                             n == 4 ? "%s\t%s\t%s\n" : "%s\t%s\t%s\t%s\n", f[1], f[2], f[3],
                                             n == 5 ? f[4] : "");
        } else if (strcmp(f[0], "RESOLVE") == 0 && n == 4) {
            if (window == NULL) {
                window = yonder_import_map_new();
                if (!yonder_import_map_merge(window, parsed))
                    added = false;
                parsed = NULL;
                if (expected[0] != '\0') {
                    yonder_import_map_dump(window, got, sizeof(got));
                    verdict(added && strcmp(got, expected) == 0, name, "parse", got);
                    expected[0] = '\0';
                }
            }
            char what[2 * CAP];
            snprintf(what, sizeof(what), "%s from %s", f[2], f[1]);
            char *out;
            yonder_specifier_t r = yonder_import_map_resolve(window, f[1], f[2], &out);
            bool pass = f[3] == NULL ? r == YONDER_SPECIFIER_BARE || r == YONDER_SPECIFIER_BLOCKED
                                     : r == YONDER_SPECIFIER_OK && strcmp(out, f[3]) == 0;
            verdict(pass, name, what, r == YONDER_SPECIFIER_OK ? out
                                      : r == YONDER_SPECIFIER_BARE ? "(bare)"
                                      : r == YONDER_SPECIFIER_BLOCKED ? "(blocked)" : "(failed)");
            os64_free(out);
        } else if (strcmp(f[0], "END") == 0) {
            if (error) {
                printf("SKIP\t%s: libdom refuses its JSON\n", name);
            } else if (window == NULL) {
                // A case of the parse alone.
                window = yonder_import_map_new();
                if (!yonder_import_map_merge(window, parsed))
                    added = false;
                parsed = NULL;
                yonder_import_map_dump(window, got, sizeof(got));
                verdict(added && strcmp(got, expected) == 0, name, "parse", got);
            }
            yonder_import_map_free(parsed);
            yonder_import_map_free(window);
            parsed = window = NULL;
        } else {
            fprintf(stderr, "test_importmap_host: a line it cannot read: %s\n", f[0]);
            fclose(in);
            return 2;
        }
    }
    fclose(in);
    if (live != 0) {
        printf("FAIL\tthe cases left %zu allocations live\n", live);
        return 1;
    }
    return 0;
}

// ── The merge (HTML § 8.1.5.3, "merge existing and new import maps") ────

static const char *kPage = "https://example.com/app/index.html";

// A parse result of (scope, key, value) triples, ended by a NULL key.
static yonder_import_map_t *map_of(const char *const *entries)
{
    yonder_import_map_t *map = yonder_import_map_new();
    for (size_t i = 0; map != NULL && entries[i + 1] != NULL; i += 3)
        if (!yonder_import_map_add(map, kPage, entries[i], entries[i + 1], entries[i + 2]))
            check(false, entries[i + 1]);
    return map;
}

static bool resolves(yonder_import_map_t *window, const char *base, const char *specifier, const char *want)
{
    char *out;
    yonder_specifier_t r = yonder_import_map_resolve(window, base, specifier, &out);
    bool ok = want == NULL ? r != YONDER_SPECIFIER_OK : r == YONDER_SPECIFIER_OK && strcmp(out, want) == 0;
    os64_free(out);
    return ok;
}

// How `specifier` resolves from `base` through `window`, its answer let go.
static yonder_specifier_t resolution(yonder_import_map_t *window, const char *base, const char *specifier)
{
    char *out;
    yonder_specifier_t r = yonder_import_map_resolve(window, base, specifier, &out);
    os64_free(out);
    return r;
}

static int merge_cases(void)
{
    // Without a conflict or a resolution in the way, the two are one map.
    yonder_import_map_t *window = yonder_import_map_new();
    check(yonder_import_map_merge(window, map_of((const char *const[]){NULL, "/app/", "./original-app/", NULL, NULL})),
          "merge: a first map registers");
    check(yonder_import_map_merge(window, map_of((const char *const[]){NULL, "/app/helper", "./helper/index.mjs",
                                                                        "/js", "/app/", "./js-app/", NULL, NULL})),
          "merge: a second registers");
    check(resolves(window, "https://example.com/x.mjs", "/app/helper", "https://example.com/app/helper/index.mjs") &&
          resolves(window, "https://example.com/x.mjs", "/app/other", "https://example.com/app/original-app/other") &&
          resolves(window, "https://example.com/js", "/app/a", "https://example.com/app/js-app/a"),
          "merge: rules for /app/ and /app/helper both stand, and the scope with them");
    yonder_import_map_free(window);

    // The first map's rule for a key wins, in imports and in a scope.
    window = yonder_import_map_new();
    yonder_import_map_merge(window, map_of((const char *const[]){NULL, "a", "/a-1.mjs", "/s/", "b", "/b-1.mjs", NULL, NULL}));
    yonder_import_map_merge(window, map_of((const char *const[]){NULL, "a", "/a-2.mjs", NULL, "c", "/c-2.mjs",
                                                                  "/s/", "b", "/b-2.mjs", NULL, NULL}));
    check(resolves(window, kPage, "a", "https://example.com/a-1.mjs") &&
          resolves(window, kPage, "c", "https://example.com/c-2.mjs") &&
          resolves(window, "https://example.com/s/x.mjs", "b", "https://example.com/b-1.mjs"),
          "merge: a conflicting rule is the first map's, and a new key is added");
    yonder_import_map_free(window);

    // A rule that would change what was already resolved is dropped: in
    // imports, a key that begins with a resolved specifier.
    window = yonder_import_map_new();
    check(resolves(window, kPage, "/app/helper", "https://example.com/app/helper"),
          "merge: /app/helper resolves before any map");
    yonder_import_map_merge(window, map_of((const char *const[]){NULL, "/app/helper", "./helper/index.mjs",
                                                                  NULL, "lodash", "/lodash.js", NULL, NULL}));
    check(resolves(window, kPage, "/app/helper", "https://example.com/app/helper") &&
          resolves(window, kPage, "lodash", "https://example.com/lodash.js"),
          "merge: the rule for the resolved /app/helper is dropped, lodash's added");
    yonder_import_map_free(window);

    // In a scope, only for a script the scope applies to; a prefix rule
    // too, when it could have matched.
    window = yonder_import_map_new();
    check(resolves(window, "https://example.com/app/vendor/main.mjs", "/app/helper",
                   "https://example.com/app/helper"),
          "merge: /app/helper resolves from /app/vendor/main.mjs");
    yonder_import_map_merge(window, map_of((const char *const[]){
        "/app/", "/app/helper", "./helper/index.mjs", "/app/vendor/", "/app/", "./vendor_helper/",
        "/vendor/", "/app/helper", "./helper/vendor_index.mjs", NULL, "lodash", "/lodash.js", NULL, NULL}));
    check(resolves(window, "https://example.com/app/vendor/main.mjs", "/app/helper",
                   "https://example.com/app/helper") &&
          resolves(window, "https://example.com/vendor/main.mjs", "/app/helper",
                   "https://example.com/app/helper/vendor_index.mjs") &&
          resolves(window, kPage, "lodash", "https://example.com/lodash.js"),
          "merge: the two scopes over the resolving script drop their rules, the third keeps its own");
    yonder_import_map_free(window);

    // A bare name resolved through one map stays what it was.
    window = yonder_import_map_new();
    yonder_import_map_merge(window, map_of((const char *const[]){NULL, "react", "/r-18.js", NULL, NULL}));
    check(resolves(window, kPage, "react", "https://example.com/r-18.js"), "merge: react resolves through the map");
    yonder_import_map_merge(window, map_of((const char *const[]){NULL, "react", "/r-19.js", NULL,
                                                                  "react/", "/r-19/", NULL, NULL}));
    check(resolves(window, kPage, "react", "https://example.com/r-18.js") &&
          resolves(window, kPage, "react/jsx-runtime", NULL),
          "merge: react stays the first map's, and react/ (which begins with it) is dropped");
    yonder_import_map_free(window);

    // A null rule blocks without falling back; a bare name no rule maps
    // is BARE; a prefix that backtracks above itself is blocked.
    window = yonder_import_map_new();
    yonder_import_map_merge(window, map_of((const char *const[]){NULL, "x", NULL, NULL, "p/", "/pkg/", NULL, NULL}));
    check(resolution(window, kPage, "x") == YONDER_SPECIFIER_BLOCKED &&
          resolution(window, kPage, "y") == YONDER_SPECIFIER_BARE &&
          resolution(window, kPage, "p/../x") == YONDER_SPECIFIER_BLOCKED,
          "resolve: a null rule blocks, an unmapped bare name is bare, a backtrack blocks");
    yonder_import_map_free(window);

    // A data: URL carries its module in itself, so it is resolved at any
    // length, spelled as a short one is; a long address of any other kind
    // is past what yonder resolves, which is no bare name.
    static char spelled[3 * CAP], written[3 * CAP];
    memset(written, 0, sizeof(written));
    strcpy(written, "  DATA:text/javascript,export default 1;//");
    memset(written + strlen(written), 'x', 2 * CAP);
    strcpy(spelled, "data:text/javascript,export default 1;//");
    memset(spelled + strlen(spelled), 'x', 2 * CAP);
    check(resolves(NULL, kPage, written, spelled), "resolve: a long data: URL resolves, spelled as a short one is");
    window = yonder_import_map_new();
    yonder_import_map_merge(window, map_of((const char *const[]){NULL, "react", "/r.js", NULL, NULL}));
    check(resolves(window, spelled, "react", "https://example.com/r.js") &&
          resolves(window, spelled, "https://example.com/b.js", "https://example.com/b.js") &&
          resolution(window, spelled, "./b.js") == YONDER_SPECIFIER_BARE,
          "resolve: a long data: module imports a bare name and an absolute URL, and nothing relative");
    yonder_import_map_merge(window, map_of((const char *const[]){NULL, spelled, "/d.js", NULL, NULL}));
    check(resolves(window, kPage, written, "https://example.com/d.js"),
          "resolve: a rule for a long data: URL maps it");
    yonder_import_map_free(window);
    static char far[3 * CAP];
    strcpy(far, "https://example.com/");
    memset(far + strlen(far), 'f', 2 * CAP);
    check(resolution(NULL, kPage, far) == YONDER_SPECIFIER_FAILED &&
          resolution(NULL, kPage, far + strlen("https://example.com")) == YONDER_SPECIFIER_FAILED,
          "resolve: a long URL of another kind fails, absolute or relative, and is no bare name");

    // Without a window map a resolution is a plain URL's, recorded nowhere.
    check(resolves(NULL, kPage, "./a.js", "https://example.com/app/a.js") && resolves(NULL, kPage, "a", NULL),
          "resolve: no map is the empty map");
    // An absolute specifier is serialized as a relative one is, so the
    // module map has one key for one module however a page spells it.
    check(resolves(NULL, kPage, "HTTPS://Example.COM/app/./x/../a.js", "https://example.com/app/a.js") &&
          resolves(NULL, kPage, "https://example.com:443/app/a.js", "https://example.com/app/a.js"),
          "resolve: an absolute specifier's scheme, host, default port and dot segments are serialized");
    check(live == 0, "merge: nothing is left live");
    return failures != 0;
}

// ── At scale ────────────────────────────────────────────────────────────

// A merge runs where no task budget stops it, and a page sizes both sides
// of it: 4,096 resolutions made, then a map of 4,096 prefix rules, all
// under one 1,800-byte path, then 4,096 resolutions through them. Compared
// pair by pair that is tens of billions of byte comparisons; looked up it
// is a moment. The bound is CPU seconds under the sanitizers, which a scan
// over the pairs passes by minutes.
static int scale_cases(void)
{
    static char deep[1900], specifier[2048];
    memset(deep, 'd', 1800);
    deep[1800] = '\0';
    clock_t began = clock();
    yonder_import_map_t *window = yonder_import_map_new();
    for (int i = 0; i < 4096; i++) {
        snprintf(specifier, sizeof(specifier), "/%s/m%d.js", deep, i);
        check(resolution(window, kPage, specifier) == YONDER_SPECIFIER_OK, "scale: a resolution is made");
    }
    yonder_import_map_t *incoming = yonder_import_map_new();
    for (int i = 0; i < 4096; i++) {
        char key[2048];
        snprintf(key, sizeof(key), "/%s/k%d/", deep, i);
        check(yonder_import_map_add(incoming, kPage, NULL, key, "/lib/"), "scale: a rule is added");
    }
    check(yonder_import_map_merge(window, incoming), "scale: the merge registers");
    for (int i = 0; i < 4096; i++) {
        snprintf(specifier, sizeof(specifier), "/%s/k%d/x.js", deep, i);
        check(resolves(window, kPage, specifier, "https://example.com/lib/x.js"), "scale: a prefix rule resolves");
    }
    yonder_import_map_free(window);
    double seconds = (double)(clock() - began) / CLOCKS_PER_SEC;
    printf("importmap scale: 4096 resolutions, a 4096-rule merge and 4096 more resolutions in %.2f CPU s\n",
           seconds);
    check(seconds < 10.0, "scale: the merge and the resolutions look up, and do not scan");
    check(live == 0, "scale: nothing is left live");
    return failures != 0;
}

// ── Every allocation failing in turn ────────────────────────────────────

// A parse, two merges, and resolutions between and after them; true when
// everything that was asked for succeeded.
static bool scenario(void)
{
    bool ok = true;
    yonder_import_map_t *window = yonder_import_map_new();
    yonder_import_map_t *first = yonder_import_map_new();
    ok = window != NULL && first != NULL;
    ok = ok && yonder_import_map_add(first, kPage, NULL, "react", "/r.js") &&
         yonder_import_map_add(first, kPage, NULL, "react/", "/r/") &&
         yonder_import_map_add(first, kPage, "/app/", "x", "/x.js");
    ok = yonder_import_map_merge(window, first) && ok;
    for (int i = 0; i < 40; i++) {
        char specifier[32];
        snprintf(specifier, sizeof(specifier), "./m%d.js", i);
        ok = resolution(window, kPage, specifier) == YONDER_SPECIFIER_OK && ok;
    }
    ok = resolution(window, kPage, "react/jsx") == YONDER_SPECIFIER_OK && ok;
    yonder_import_map_t *second = yonder_import_map_new();
    ok = second != NULL && yonder_import_map_add(second, kPage, NULL, "vue", "/v.js") &&
         yonder_import_map_add(second, kPage, "/app/", "y", "/y.js") && ok;
    ok = yonder_import_map_merge(window, second) && ok;
    ok = resolution(window, "https://example.com/app/a.js", "y") == YONDER_SPECIFIER_OK && ok;
    yonder_import_map_free(window);
    return ok;
}

static int sweep(void)
{
    allocations = 0;
    fail_at = 0;
    check(scenario() && live == 0, "sweep: the scenario succeeds with every allocation answered");
    size_t total = allocations;
    for (size_t at = 1; at <= total; at++) {
        allocations = 0;
        fail_at = at;
        (void)scenario();
        fail_at = 0;
        if (live != 0) {
            char what[96];
            snprintf(what, sizeof(what), "sweep: allocation %zu failing leaves %zu live", at, live);
            check(false, what);
            live = 0;
        }
    }
    printf("importmap sweep: %zu allocations, each failed in turn\n", total);
    return failures != 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--merge") == 0)
        return merge_cases();
    if (argc == 2 && strcmp(argv[1], "--scale") == 0)
        return scale_cases();
    if (argc == 2 && strcmp(argv[1], "--sweep") == 0)
        return sweep();
    if (argc == 2)
        return data_cases(argv[1]);
    fprintf(stderr, "usage: test_importmap_host <cases> | --merge | --sweep\n");
    return 2;
}
