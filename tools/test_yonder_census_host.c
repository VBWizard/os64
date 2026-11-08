// yonder's memory census (userland/apps/yonder/census.c; MEMORY_CENSUS.md)
// on the host: its sizes, its lines, its byte budgets at their worst case,
// the timeline's thinning, and which hosts a `census` entry names.
#include "census.h"
#include <stdio.h>
#include <string.h>

// fmt.c's printf family writes through os64_write; the census only formats
// into buffers, so the host's stdout stands in for it.
int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    (void)handle;
    return (int64_t)fwrite(buf, 1, len, stdout);
}

static unsigned checks, failures;
static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static void size_is(uint64_t bytes, const char *want)
{
    char got[32];
    yonder_census_size(bytes, got, sizeof(got));
    if (strcmp(got, want) != 0) printf("size %lu: got %s, want %s\n", (unsigned long)bytes, got, want);
    check(strcmp(got, want) == 0, "a size reads at three figures with its unit");
}

static void size_cases(void)
{
    size_is(0, "0");
    size_is(1023, "1023");
    size_is(1024, "1.00K");
    size_is(12345, "12.0K");
    size_is(3250586, "3.10M");
    size_is((uint64_t)1406601872, "1.31G");
    size_is((uint64_t)180 << 20, "180M");
    size_is((uint64_t)5 << 40, "5.00T");
    char tiny[4];
    size_t n = yonder_census_size((uint64_t)180 << 20, tiny, sizeof(tiny));
    check(n < sizeof(tiny) && tiny[n] == '\0', "a size never runs past its buffer");
}

static void summary_cases(void)
{
    yonder_census_sheet_t s = {0};
    s.heap_known = true;
    s.heap_mapped = (uint64_t)1300 << 20;
    s.heap_live = (uint64_t)1100 << 20;
    yonder_census_owner(&s, "dom", (uint64_t)48 << 20, 0, NULL);
    yonder_census_owner(&s, "script", (uint64_t)180 << 20, 0, NULL);
    yonder_census_owner(&s, "layout", (uint64_t)140 << 20, 390000, "boxes");
    char line[YONDER_CENSUS_SUMMARY_MAX];
    yonder_census_summary(&s, line, sizeof(line));
    check(!strcmp(line, "heap 1.26G (uncounted 732M); script 180M; layout 140M 390000 boxes; dom 48.0M"),
          "the summary: the process, what no owner counts, then the owners biggest first");
    if (strcmp(line, "heap 1.26G (uncounted 732M); script 180M; layout 140M 390000 boxes; dom 48.0M"))
        printf("  %s\n", line);

    // The worst case: every owner, the longest names and numbers there are.
    yonder_census_sheet_t w = {0};
    w.heap_known = true;
    w.heap_mapped = w.heap_live = UINT64_MAX / 2;
    static const char *const names[YONDER_CENSUS_OWNERS] = {
        "script", "pictures", "layout", "html", "dom", "cascade", "sheets", "page",
        "bodies", "canvas", "owner11", "owner12", "owner13", "owner14", "owner15", "owner16"};
    for (int i = 0; i < YONDER_CENSUS_OWNERS + 2; i++)
        yonder_census_owner(&w, names[i % YONDER_CENSUS_OWNERS], (uint64_t)999 << 30, UINT32_MAX, "nodes");
    check(w.nowners == YONDER_CENSUS_OWNERS, "a sheet holds its owners and no more");
    char big[4096];
    size_t n = yonder_census_summary(&w, big, sizeof(big));
    check(n < YONDER_CENSUS_SUMMARY_MAX && strlen(big) == n, "the worst summary keeps its budget");
    check(n >= 4 && !strcmp(big + n - 4, ";..."), "an owner left out is said");
    yonder_census_sheet_t none = {0};
    check(yonder_census_summary(&none, big, sizeof(big)) == 0 && big[0] == '\0', "no owners, no line");
    char tiny[4] = "xyz";
    check(yonder_census_summary(&s, tiny, sizeof(tiny)) == 0 && tiny[0] == '\0' && tiny[1] == 'y',
          "a buffer too small for any of it gets an empty line and nothing past its first byte");
}

static void detail_cases(void)
{
    yonder_census_sheet_t s = {0};
    yonder_census_dom_kind(&s, "retained", (uint64_t)7768704, 20231);
    yonder_census_dom_kind(&s, "wrapper", (uint64_t)38492928, 100242);
    yonder_census_dom_kind(&s, "timer", 0, 0);
    char line[1024];
    yonder_census_dom_line(&s, line, sizeof(line));
    check(!strcmp(line, "wrapper 36.7M 100242; retained 7.40M 20231"), "the dom line: kinds biggest first, empty ones left out");
    if (strcmp(line, "wrapper 36.7M 100242; retained 7.40M 20231")) printf("  %s\n", line);
    check(yonder_census_script_line(&s, line, sizeof(line)) == 0, "no script census, no script line");
    s.script_known = true;
    s.script.objects = 818799;
    s.script.object_bytes = (uint64_t)92 << 20;
    s.script.charged_bytes = (uint64_t)190 << 20;
    yonder_census_script_line(&s, line, sizeof(line));
    check(strstr(line, "objects 92.0M 818799;") == line && strstr(line, "charged 190M") != NULL,
          "the script line names what the heap holds and what it charges");
}

static void timeline_cases(void)
{
    yonder_census_timeline_t tl;
    yonder_census_timeline_init(&tl);
    yonder_census_sheet_t s = {0};
    s.heap_known = true;
    yonder_census_owner(&s, "script", 1 << 20, 0, NULL);
    check(yonder_census_timeline_due(&tl, 0), "the first sample is due at once");
    uint64_t ms = 0, last = 0;
    for (int i = 0; i < 1000; i++) {
        s.heap_mapped = (uint64_t)i << 20;
        s.cost_us = (uint64_t)i;
        if (yonder_census_timeline_due(&tl, ms)) {
            yonder_census_timeline_add(&tl, ms, &s);
            last = ms;
        }
        ms += 1000;
    }
    check(tl.n > 0 && tl.n <= YONDER_CENSUS_SAMPLES, "a timeline holds no more than its samples");
    check(tl.samples[0].ms == 0 && tl.samples[tl.n - 1].ms == last, "the first and the latest are kept");
    bool even = true;
    uint64_t gap = tl.n > 1 ? tl.samples[1].ms - tl.samples[0].ms : 0;
    for (int32_t i = 2; i < tl.n; i++)
        if (tl.samples[i].ms - tl.samples[i - 1].ms != gap) even = false;
    check(even && gap == tl.interval_ms, "the samples stay evenly spaced, at the interval");
    check(tl.interval_ms > YONDER_CENSUS_FIRST_INTERVAL_MS, "the interval grew as the timeline thinned");
    check(tl.cost_us_max >= 900, "the most a walk cost is kept");
    char line[8192];
    yonder_census_timeline_line(&tl, line, sizeof(line));
    check(strstr(line, "+0s heap 0 ") == line && strstr(line, "m") != NULL && strstr(line, "s heap") != NULL,
          "a timeline's times keep their seconds past two minutes");
    yonder_census_timeline_t one;
    yonder_census_timeline_init(&one);
    yonder_census_timeline_add(&one, 0, &s);
    yonder_census_state_line(&one, line, sizeof(line));
    check(strstr(line, "on; 1 sample every 10s;") == line, "one sample is one sample");
    yonder_census_timeline_t late;
    yonder_census_timeline_init(&late);
    yonder_census_timeline_add(&late, (uint64_t)130 * 1000, &s);
    yonder_census_timeline_add(&late, (uint64_t)3 * 3600000 + 5 * 60000, &s);
    yonder_census_timeline_line(&late, line, sizeof(line));
    check(strstr(line, "+2m10s heap") == line && strstr(line, "| +3h05m heap") != NULL,
          "minutes keep their seconds, and hours their minutes");

    // A large detail: every kind and every sample, at sizes of hundreds of
    // gigabytes and counts of billions. (diag_census holds the record to the
    // budget in code whatever the figures; this proves the lines themselves
    // leave room at the sizes a page could reach.)
    yonder_census_sheet_t w = {0};
    w.heap_known = true;
    w.heap_mapped = w.heap_live = UINT64_MAX / 2;
    for (int k = 0; k < YONDER_CENSUS_PARTS; k++) yonder_census_dom_kind(&w, "intersection", (uint64_t)999 << 30, UINT32_MAX);
    static const char *const owners[] = {"script", "dom", "html", "layout", "pictures", "cascade"};
    for (int k = 0; k < 6; k++) yonder_census_owner(&w, owners[k], (uint64_t)999 << 30, UINT32_MAX, "boxes");
    w.script_known = true;
    memset(&w.script, 0xff, sizeof(w.script));
    w.script.charged_bytes = SIZE_MAX / 2;
    yonder_census_timeline_t full;
    yonder_census_timeline_init(&full);
    for (int i = 0; i < YONDER_CENSUS_SAMPLES; i++) yonder_census_timeline_add(&full, (uint64_t)999999 * 3600000, &w);
    char a[8192], b[8192], c[8192], d[8192], e[512];
    size_t total = yonder_census_summary(&w, a, sizeof(a)) + yonder_census_dom_line(&w, b, sizeof(b)) +
                   yonder_census_script_line(&w, c, sizeof(c)) + yonder_census_timeline_line(&full, d, sizeof(d)) +
                   yonder_census_state_line(&full, e, sizeof(e));
    // Each line also carries its key ("memory timeline: ") and a newline.
    total += 5 * 20;
    if (total > YONDER_CENSUS_DETAIL_MAX) printf("  worst detail: %zu bytes\n", total);
    check(total <= YONDER_CENSUS_DETAIL_MAX, "the worst censused record keeps its budget");
}

static void match_cases(void)
{
    check(yonder_census_matches("example.com", "example.com"), "the host itself");
    check(yonder_census_matches("example.com", "www.example.com"), "a subdomain");
    check(yonder_census_matches("Example.COM.", "WWW.example.com"), "case and a trailing dot do not matter");
    check(!yonder_census_matches("ample.com", "example.com"), "a longer name is not a subdomain");
    check(!yonder_census_matches("www.example.com", "example.com"), "a parent is not its subdomain");
    check(yonder_census_matches("*", "anything.test"), "a star names every host");
    check(!yonder_census_matches("", "example.com") && !yonder_census_matches(NULL, "a"), "nothing names nothing");
}

int main(void)
{
    size_cases();
    summary_cases();
    detail_cases();
    timeline_cases();
    match_cases();
    printf("yonder census: %u checks, %u failed\n", checks, failures);
    return failures != 0;
}
