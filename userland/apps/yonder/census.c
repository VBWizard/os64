#include "census.h"
#include "os64/fmt.h"
#include "os64/str.h"

// The owners a timeline sample keeps, beside the process heap: the ones a
// page's growth has shown to move.
static const char *const kTimelineOwners[YONDER_CENSUS_TIMELINE_OWNERS] = {
    "script", "dom", "html", "layout", "pictures", "cascade",
};

void yonder_census_owner(yonder_census_sheet_t *sheet, const char *name, uint64_t bytes, uint64_t count,
                         const char *unit)
{
    if (sheet == NULL || name == NULL || sheet->nowners >= YONDER_CENSUS_OWNERS) return;
    sheet->owners[sheet->nowners++] = (yonder_census_entry_t){name, bytes, count, unit};
}

void yonder_census_dom_kind(yonder_census_sheet_t *sheet, const char *name, uint64_t bytes, uint64_t blocks)
{
    if (sheet == NULL || name == NULL || sheet->ndom >= YONDER_CENSUS_PARTS) return;
    sheet->dom[sheet->ndom++] = (yonder_census_entry_t){name, bytes, blocks, NULL};
}

size_t yonder_census_size(uint64_t bytes, char *out, size_t cap)
{
    static const char kUnits[] = "KMGT";
    if (out == NULL || cap == 0) return 0;
    if (bytes < 1024) return (size_t)os64_snprintf(out, cap, "%lu", (unsigned long)bytes);
    int u = 0;
    uint64_t unit = 1024;
    while (u < 3 && bytes >= unit * 1024) {
        unit *= 1024;
        u++;
    }
    // Hundredths of the unit, truncated, shown to three significant figures.
    uint64_t h = bytes / unit * 100 + bytes % unit * 100 / unit;
    int n;
    if (h < 1000)
        n = os64_snprintf(out, cap, "%lu.%02lu%c", (unsigned long)(h / 100), (unsigned long)(h % 100), kUnits[u]);
    else if (h < 10000)
        n = os64_snprintf(out, cap, "%lu.%lu%c", (unsigned long)(h / 100), (unsigned long)(h % 100 / 10), kUnits[u]);
    else
        n = os64_snprintf(out, cap, "%lu%c", (unsigned long)(h / 100), kUnits[u]);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

// Appends at `*at` while it fits; false, and nothing written, when it would
// not.
static bool put(char *out, size_t cap, size_t *at, const char *text)
{
    size_t n = os64_strlen(text);
    if (*at + n + 1 > cap) return false;
    os64_memcpy(out + *at, text, n);
    *at += n;
    out[*at] = '\0';
    return true;
}

// `name value[ count unit]`.
static void entry_text(const yonder_census_entry_t *e, bool counts, char *out, size_t cap)
{
    char size[16];
    yonder_census_size(e->bytes, size, sizeof(size));
    if (counts && e->unit != NULL && e->count != 0)
        os64_snprintf(out, cap, "%s %s %lu %s", e->name, size, (unsigned long)e->count, e->unit);
    else if (counts && e->unit == NULL && e->count != 0)
        os64_snprintf(out, cap, "%s %s %lu", e->name, size, (unsigned long)e->count);
    else
        os64_snprintf(out, cap, "%s %s", e->name, size);
}

// Indices of `n` entries, biggest first; ties keep their order.
static void by_size(const yonder_census_entry_t *entries, int32_t n, int32_t *order)
{
    for (int32_t i = 0; i < n; i++) order[i] = i;
    for (int32_t i = 1; i < n; i++) {
        int32_t k = order[i], j = i;
        while (j > 0 && entries[order[j - 1]].bytes < entries[k].bytes) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = k;
    }
}

size_t yonder_census_summary(const yonder_census_sheet_t *sheet, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';
    // Room for at least ";..." and its terminator, which the owners keep.
    if (sheet == NULL || cap < 5) return 0;
    if (cap > YONDER_CENSUS_SUMMARY_MAX) cap = YONDER_CENSUS_SUMMARY_MAX;
    size_t at = 0;
    char part[96], a[16], b[16];
    if (sheet->heap_known) {
        uint64_t counted = 0;
        for (int32_t i = 0; i < sheet->nowners; i++) counted += sheet->owners[i].bytes;
        yonder_census_size(sheet->heap_mapped, a, sizeof(a));
        yonder_census_size(sheet->heap_live > counted ? sheet->heap_live - counted : 0, b, sizeof(b));
        os64_snprintf(part, sizeof(part), "heap %s (uncounted %s)", a, b);
        put(out, cap, &at, part);
    }
    int32_t order[YONDER_CENSUS_OWNERS];
    by_size(sheet->owners, sheet->nowners, order);
    // Room is kept for the ";..." that says an owner was left out.
    for (int32_t i = 0; i < sheet->nowners; i++) {
        entry_text(&sheet->owners[order[i]], true, part, sizeof(part));
        size_t before = at;
        bool fits = (at == 0 || put(out, cap - 4, &at, "; ")) && put(out, cap - 4, &at, part);
        if (!fits) {
            at = before;
            out[at] = '\0';
            put(out, cap, &at, ";...");
            break;
        }
    }
    return at;
}

size_t yonder_census_dom_line(const yonder_census_sheet_t *sheet, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';
    if (sheet == NULL) return 0;
    int32_t order[YONDER_CENSUS_PARTS];
    by_size(sheet->dom, sheet->ndom, order);
    size_t at = 0;
    char part[96];
    for (int32_t i = 0; i < sheet->ndom; i++) {
        const yonder_census_entry_t *e = &sheet->dom[order[i]];
        if (e->bytes == 0) continue;
        entry_text(e, true, part, sizeof(part));
        if (!((at == 0 || put(out, cap, &at, "; ")) && put(out, cap, &at, part))) break;
    }
    return at;
}

size_t yonder_census_script_line(const yonder_census_sheet_t *sheet, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';
    if (sheet == NULL || !sheet->script_known) return 0;
    const os64_js_heap_census_t *s = &sheet->script;
    char o[16], p[16], st[16], at_[16], sh[16], f[16], c[16], bin[16], ch[16];
    yonder_census_size(s->object_bytes, o, sizeof(o));
    yonder_census_size(s->property_bytes, p, sizeof(p));
    yonder_census_size(s->string_bytes, st, sizeof(st));
    yonder_census_size(s->atom_bytes, at_, sizeof(at_));
    yonder_census_size(s->shape_bytes, sh, sizeof(sh));
    yonder_census_size(s->function_bytes, f, sizeof(f));
    yonder_census_size(s->function_code_bytes, c, sizeof(c));
    yonder_census_size(s->binary_bytes, bin, sizeof(bin));
    yonder_census_size(s->charged_bytes, ch, sizeof(ch));
    int n = os64_snprintf(out, cap,
                          "objects %s %lu; properties %s %lu; strings %s %lu; atoms %s %lu; shapes %s %lu; "
                          "functions %s %lu (code %s); arrays %lu (%lu fast elements); binary %s %lu; "
                          "blocks %lu; charged %s",
                          o, (unsigned long)s->objects, p, (unsigned long)s->properties, st,
                          (unsigned long)s->strings, at_, (unsigned long)s->atoms, sh, (unsigned long)s->shapes, f,
                          (unsigned long)s->functions, c, (unsigned long)s->arrays,
                          (unsigned long)s->fast_array_elements, bin, (unsigned long)s->binary_objects,
                          (unsigned long)s->blocks, ch);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

void yonder_census_timeline_init(yonder_census_timeline_t *tl)
{
    if (tl == NULL) return;
    os64_memset(tl, 0, sizeof(*tl));
    tl->interval_ms = YONDER_CENSUS_FIRST_INTERVAL_MS;
}

bool yonder_census_timeline_due(const yonder_census_timeline_t *tl, uint64_t ms)
{
    if (tl == NULL) return false;
    return tl->n == 0 || ms >= tl->samples[tl->n - 1].ms + tl->interval_ms;
}

static uint64_t owner_bytes(const yonder_census_sheet_t *sheet, const char *name)
{
    for (int32_t i = 0; i < sheet->nowners; i++)
        if (os64_streq(sheet->owners[i].name, name)) return sheet->owners[i].bytes;
    return 0;
}

void yonder_census_timeline_add(yonder_census_timeline_t *tl, uint64_t ms, const yonder_census_sheet_t *sheet)
{
    if (tl == NULL || sheet == NULL) return;
    if (tl->n == YONDER_CENSUS_SAMPLES) {
        // Keep the even samples: the first stays, and the newest is the one
        // about to be added.
        int32_t kept = 0;
        for (int32_t i = 0; i < tl->n; i += 2) tl->samples[kept++] = tl->samples[i];
        tl->n = kept;
        if (tl->interval_ms <= UINT32_MAX / 2) tl->interval_ms *= 2;
    }
    yonder_census_sample_t *s = &tl->samples[tl->n++];
    s->ms = ms;
    s->heap = sheet->heap_known ? sheet->heap_mapped : 0;
    for (int k = 0; k < YONDER_CENSUS_TIMELINE_OWNERS; k++) s->owners[k] = owner_bytes(sheet, kTimelineOwners[k]);
    if (sheet->cost_us > tl->cost_us_max) tl->cost_us_max = sheet->cost_us;
}

// "+40s", "+12m10s", "+3h05m": seconds under two minutes, minutes and
// seconds under two hours, hours and minutes past that.
static void elapsed(uint64_t ms, char *out, size_t cap)
{
    uint64_t s = ms / 1000;
    if (s < 120) os64_snprintf(out, cap, "+%lus", (unsigned long)s);
    else if (s < 120 * 60) os64_snprintf(out, cap, "+%lum%02lus", (unsigned long)(s / 60), (unsigned long)(s % 60));
    else os64_snprintf(out, cap, "+%luh%02lum", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
}

size_t yonder_census_timeline_line(const yonder_census_timeline_t *tl, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';
    if (tl == NULL) return 0;
    size_t at = 0;
    for (int32_t i = 0; i < tl->n; i++) {
        const yonder_census_sample_t *s = &tl->samples[i];
        char part[192], when[16], v[16];
        elapsed(s->ms, when, sizeof(when));
        yonder_census_size(s->heap, v, sizeof(v));
        size_t n = (size_t)os64_snprintf(part, sizeof(part), "%s heap %s", when, v);
        for (int k = 0; k < YONDER_CENSUS_TIMELINE_OWNERS && n < sizeof(part); k++) {
            yonder_census_size(s->owners[k], v, sizeof(v));
            n += (size_t)os64_snprintf(part + n, sizeof(part) - n, " %s %s", kTimelineOwners[k], v);
        }
        if (!((at == 0 || put(out, cap, &at, " | ")) && put(out, cap, &at, part))) break;
    }
    return at;
}

size_t yonder_census_state_line(const yonder_census_timeline_t *tl, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return 0;
    char every[16];
    elapsed(tl != NULL ? tl->interval_ms : 0, every, sizeof(every));
    int samples = tl != NULL ? (int)tl->n : 0;
    int n = os64_snprintf(out, cap, "on; %d sample%s every %s; a census walk cost %lu ms at most", samples,
                          samples == 1 ? "" : "s", every + 1,
                          (unsigned long)((tl != NULL ? tl->cost_us_max : 0) / 1000));
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

bool yonder_census_matches(const char *entry, const char *host)
{
    if (entry == NULL || host == NULL) return false;
    if (entry[0] == '*' && entry[1] == '\0') return true;
    size_t e = os64_strlen(entry), h = os64_strlen(host);
    while (e > 0 && entry[e - 1] == '.') e--;
    while (h > 0 && host[h - 1] == '.') h--;
    if (e == 0 || h < e) return false;
    for (size_t i = 0; i < e; i++)
        if (lower(entry[e - 1 - i]) != lower(host[h - 1 - i])) return false;
    return h == e || host[h - e - 1] == '.';
}
