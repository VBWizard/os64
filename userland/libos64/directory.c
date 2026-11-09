#include "os64/directory.h"
#include "directory_internal.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/date.h"
#include "os64/proc.h"
#include <limits.h>

struct os64_directory {
    size_t refs, count, bytes;
    uint64_t domain, high_water;
    bool incomplete;
    char path[OS64_DIRECTORY_PATH];
    os64_directory_entry_t entries[];
};

static size_t bounded_len(const char *s, size_t cap)
{
    size_t i = 0;
    if (!s) return cap;
    while (i < cap && s[i]) ++i;
    return i;
}

os64_ui_status_code_t os64_directory_path(const char *base, const char *input,
                                            char out[OS64_DIRECTORY_PATH])
{
    if (!out || !input || !*input) return OS64_UI_INVALID;
    size_t n = bounded_len(input, OS64_DIRECTORY_PATH);
    if (n == OS64_DIRECTORY_PATH) return OS64_UI_LIMIT;
    if (input[0] != '/') {
        if (!base || base[0] != '/') return OS64_UI_INVALID;
        if (bounded_len(base, OS64_DIRECTORY_PATH) == OS64_DIRECTORY_PATH)
            return OS64_UI_LIMIT;
    }
    char result[OS64_DIRECTORY_PATH];
    size_t used = 1;
    result[0] = '/';
    const char *parts[2] = { input[0] == '/' ? NULL : base, input };
    for (size_t p = 0; p < 2; ++p) {
        const char *s = parts[p];
        if (!s) continue;
        while (*s) {
            while (*s == '/') ++s;
            const char *start = s;
            while (*s && *s != '/') ++s;
            size_t len = (size_t)(s - start);
            if (!len || (len == 1 && start[0] == '.')) continue;
            if (len == 2 && start[0] == '.' && start[1] == '.') {
                if (used > 1) {
                    while (used > 1 && result[used - 1] != '/') --used;
                    if (used > 1) --used;
                }
                continue;
            }
            if (len > OS64_DIRENT_NAME_MAX || used + (used > 1) + len >= OS64_DIRECTORY_PATH)
                return OS64_UI_LIMIT;
            if (used > 1) result[used++] = '/';
            os64_memcpy(result + used, start, len);
            used += len;
        }
    }
    result[used] = 0;
    os64_memcpy(out, result, used + 1);
    return OS64_UI_OK;
}

os64_ui_status_code_t os64_directory_join(const char *base, const char *name,
                                            char out[OS64_DIRECTORY_PATH])
{
    size_t n = bounded_len(name, OS64_DIRECTORY_PATH);
    if (!n || n >= OS64_DIRECTORY_PATH || name[0] == '/' ||
        (n == 1 && name[0] == '.') ||
        (n == 2 && name[0] == '.' && name[1] == '.')) return OS64_UI_INVALID;
    for (size_t i = 0; i < n; ++i) if (name[i] == '/') return OS64_UI_INVALID;
    return os64_directory_path(base, name, out);
}

os64_ui_status_code_t os64_directory_home(char out[OS64_DIRECTORY_PATH])
{
    if (!out) return OS64_UI_INVALID;
    const char *home=os64_getenv("HOME");
    if (home && home[0]=='/' && !os64_directory_path("/",home,out))
        return OS64_UI_OK;
    os64_memcpy(out,"/home",6);
    return OS64_UI_OK;
}

static uint32_t hash_name(const char *name)
{
    uint32_t h = UINT32_C(2166136261);
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        h = (h ^ *p) * UINT32_C(16777619);
    return h;
}

static size_t slot(const os64_directory_t *d, const char *name, const uint32_t *table)
{
    size_t i = hash_name(name) & 32767u;
    while (table[i] && !os64_streq(d->entries[table[i] - 1].entry.name, name))
        i = (i + 1) & 32767u;
    return i;
}

static bool valid_name(const char *name)
{
    size_t n = bounded_len(name, OS64_DIRENT_NAME_MAX + 1);
    if (!n || n > OS64_DIRENT_NAME_MAX) return false;
    for (size_t i = 0; i < n; ++i) if (name[i] == '/') return false;
    return true;
}

os64_ui_status_code_t os64_directory_read(const char *path, uint64_t domain,
    const os64_directory_t *previous, bool (*cancelled)(void *), void *cancel_user,
    os64_directory_t **out, int64_t *io_error)
{
    if (!out) return OS64_UI_INVALID;
    *out = NULL;
    if (io_error) *io_error = 0;
    if (!domain || !path || path[0] != '/' ||
        bounded_len(path, OS64_DIRECTORY_PATH) == OS64_DIRECTORY_PATH ||
        (previous && previous->domain != domain)) return OS64_UI_INVALID;
    char normal[OS64_DIRECTORY_PATH];
    if (os64_directory_path("/",path,normal) || !os64_streq(path,normal))
        return OS64_UI_INVALID;
    if (cancelled && cancelled(cancel_user)) return OS64_UI_STALE;
    size_t bytes = sizeof(os64_directory_t) + OS64_DIRECTORY_ROWS * sizeof(os64_directory_entry_t);
    os64_directory_t *d = os64_malloc(bytes);
    uint32_t *seen = os64_calloc(32768, sizeof(uint32_t));
    uint32_t *old = previous ? os64_calloc(32768, sizeof(uint32_t)) : NULL;
    if (!d || !seen || (previous && !old)) {
        os64_free(d); os64_free(seen); os64_free(old);
        return OS64_UI_NO_MEMORY;
    }
    *d = (os64_directory_t){.refs = 1, .domain = domain,
        .high_water = previous ? previous->high_water : 0, .bytes = os64_malloc_size(d)};
    os64_memcpy(d->path, path, os64_strlen(path) + 1);
    if (previous) for (size_t i = 0; i < previous->count; ++i)
        old[slot(previous, previous->entries[i].entry.name, old)] = (uint32_t)(i + 1);
    int64_t h = os64_opendir(path);
    if (h < 0) {
        if (io_error) *io_error = h;
        os64_free(d); os64_free(seen); os64_free(old);
        return OS64_UI_ERROR;
    }
    os64_ui_status_code_t status = OS64_UI_OK;
    int64_t rc = 0;
    os64_dirent_t entry;
    while (true) {
        if (cancelled && cancelled(cancel_user)) { status = OS64_UI_STALE; break; }
        rc = os64_readdir((int32_t)h, &entry);
        if (rc != 1) { if (rc < 0) status = OS64_UI_ERROR; break; }
        if (!valid_name(entry.name) || (entry.flags & ~(OS64_DE_DIR|OS64_DE_MOUNT)) ||
            ((entry.flags&OS64_DE_MOUNT) && !(entry.flags&OS64_DE_DIR))) {
            status = OS64_UI_INVALID; break;
        }
        if (os64_streq(entry.name, ".") || os64_streq(entry.name, "..")) continue;
        char joined[OS64_DIRECTORY_PATH];
        status = os64_directory_join(path, entry.name, joined);
        if (status) break;
        size_t at = slot(d, entry.name, seen);
        if (seen[at]) { status = OS64_UI_DUPLICATE; break; }
        if (d->count == OS64_DIRECTORY_ROWS) { d->incomplete = true; break; }
        uint64_t id = 0;
        if (previous) {
            size_t match = slot(previous, entry.name, old);
            if (old[match]) id = previous->entries[old[match] - 1].id;
        }
        if (!id) {
            if (d->high_water == UINT64_MAX) { status = OS64_UI_LIMIT; break; }
            id = ++d->high_water;
        }
        d->entries[d->count] = (os64_directory_entry_t){.id = id, .entry = entry};
        seen[at] = (uint32_t)(++d->count);
    }
    int64_t close_rc = os64_close((int32_t)h);
    if (close_rc < 0 && !status) { status = OS64_UI_ERROR; rc = close_rc; }
    if (io_error && status == OS64_UI_ERROR) *io_error = rc;
    os64_free(seen); os64_free(old);
    if (status) { os64_free(d); return status; }
    *out = d;
    return OS64_UI_OK;
}

void os64_directory_retain(os64_directory_t *d)
{ if (d) __atomic_add_fetch(&d->refs, 1, __ATOMIC_RELAXED); }
void os64_directory_release(os64_directory_t *d)
{ if (d && __atomic_sub_fetch(&d->refs, 1, __ATOMIC_ACQ_REL) == 0) os64_free(d); }
const char *os64_directory_location(const os64_directory_t *d) { return d ? d->path : ""; }
uint64_t os64_directory_domain(const os64_directory_t *d) { return d ? d->domain : 0; }
size_t os64_directory_count(const os64_directory_t *d) { return d ? d->count : 0; }
size_t os64_directory_bytes(const os64_directory_t *d) { return d ? d->bytes : 0; }
bool os64_directory_incomplete(const os64_directory_t *d) { return d && d->incomplete; }
const os64_directory_entry_t *os64_directory_at(const os64_directory_t *d, size_t i)
{ return d && i < d->count ? &d->entries[i] : NULL; }
const os64_directory_entry_t *os64_directory_entry(const os64_directory_t *d, os64_ui_data_id_t id)
{
    if (!d || !id) return NULL;
    for (size_t i = 0; i < d->count; ++i) if (d->entries[i].id == id) return &d->entries[i];
    return NULL;
}

static os64_ui_data_text_t txt(const char *s)
{ return (os64_ui_data_text_t){s, os64_strlen(s)}; }
static size_t display_name(const char *raw, char out[OS64_DIRENT_NAME_MAX * 4 + 1])
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p; ++p) {
        if (*p < 32 || *p == 127) {
            out[used++] = '\\'; out[used++] = 'x';
            out[used++] = hex[*p >> 4]; out[used++] = hex[*p & 15];
        } else out[used++] = (char)*p;
    }
    out[used] = 0;
    return used;
}
static const char *kind(const os64_dirent_t *e)
{
    if (e->flags & OS64_DE_MOUNT) return "Mount";
    if (e->flags & OS64_DE_DIR) return "Folder";
    const char *dot = NULL;
    for (const char *p = e->name; *p; ++p) if (*p == '.') dot = p;
    if (!dot || dot == e->name) return "File";
    if (os64_streq(dot, ".txt") || os64_streq(dot, ".md")) return "Text file";
    if (os64_streq(dot, ".png") || os64_streq(dot, ".jpg") || os64_streq(dot, ".jpeg") ||
        os64_streq(dot, ".webp") || os64_streq(dot, ".bmp")) return "Image file";
    if (os64_streq(dot, ".c") || os64_streq(dot, ".h") || os64_streq(dot, ".s")) return "Source file";
    if (os64_streq(dot, ".pdf")) return "PDF document";
    return "File";
}
static size_t unsigned_text(uint64_t n, char out[32])
{
    char scratch[32]; size_t count = 0;
    do { scratch[count++] = (char)('0' + n % 10); n /= 10; } while (n);
    for (size_t i = 0; i < count; ++i) out[i] = scratch[count - 1 - i];
    out[count] = 0; return count;
}

os64_ui_status_code_t os64_directory_data_cancelled(const os64_directory_t *d,
    bool (*cancelled)(void *),void *cancel_user,os64_ui_data_t **out)
{
    if (!out) return OS64_UI_INVALID;
    *out = NULL;
    if (!d) return OS64_UI_INVALID;
    if (cancelled && cancelled(cancel_user)) return OS64_UI_STALE;
    os64_ui_data_column_t cols[] = {
        {1, {"Name",4}, OS64_UI_DATA_TEXT_KEY, 260, false, true, true, true},
        {2, {"Size",4}, OS64_UI_DATA_UNSIGNED, 104, true, true, false, false},
        {3, {"Type",4}, OS64_UI_DATA_TEXT_KEY, 124, false, true, false, false},
        {4, {"Modified",8}, OS64_UI_DATA_UNSIGNED, 172, false, true, false, false}
    };
    typedef struct { char name[OS64_DIRENT_NAME_MAX * 4 + 1], size[32], date[48]; } display_t;
    os64_ui_data_row_t *rows = os64_calloc(d->count ? d->count : 1, sizeof(*rows));
    os64_ui_data_cell_t *cells = os64_calloc(d->count ? d->count * 4 : 1, sizeof(*cells));
    display_t *shown = os64_calloc(d->count ? d->count : 1, sizeof(*shown));
    if (!rows || !cells || !shown) {
        os64_free(rows); os64_free(cells); os64_free(shown); return OS64_UI_NO_MEMORY;
    }
    for (size_t i = 0; i < d->count; ++i) {
        if ((i&63u)==0 && cancelled && cancelled(cancel_user)) {
            os64_free(rows); os64_free(cells); os64_free(shown);
            return OS64_UI_STALE;
        }
        const os64_dirent_t *e = &d->entries[i].entry;
        display_name(e->name, shown[i].name);
        rows[i] = (os64_ui_data_row_t){.id=d->entries[i].id,
            .group=(e->flags & OS64_DE_DIR) ? 0 : 1,
            .search=txt(shown[i].name), .icon=txt((e->flags & OS64_DE_DIR) ? "folder" : "file"),
            .cells=&cells[4*i]};
        cells[4*i].display=txt(shown[i].name);
        cells[4*i].key=(os64_ui_data_key_t){.present=true,.type=OS64_UI_DATA_TEXT_KEY,.value.text=txt(shown[i].name)};
        if (!(e->flags & OS64_DE_DIR)) {
            unsigned_text(e->size, shown[i].size);
            cells[4*i+1].display=txt(shown[i].size);
            cells[4*i+1].key=(os64_ui_data_key_t){.present=true,.type=OS64_UI_DATA_UNSIGNED,.value.u=e->size};
        }
        cells[4*i+2].display=txt(kind(e));
        cells[4*i+2].key=(os64_ui_data_key_t){.present=true,.type=OS64_UI_DATA_TEXT_KEY,.value.text=txt(kind(e))};
        if (e->mtime && e->mtime <= INT64_MAX) {
            os64_date_t calendar;
            if (os64_localtime((int64_t)e->mtime,&calendar) == 0 &&
                os64_strftime(shown[i].date,sizeof(shown[i].date),"%Y-%m-%d %H:%M",&calendar)) {
                cells[4*i+3].display=txt(shown[i].date);
                cells[4*i+3].key=(os64_ui_data_key_t){.present=true,.type=OS64_UI_DATA_UNSIGNED,.value.u=e->mtime};
            }
        }
    }
    os64_ui_status_code_t status=os64_ui_data_create(d->domain,cols,4,rows,d->count,out);
    os64_free(rows); os64_free(cells); os64_free(shown);
    return status;
}
os64_ui_status_code_t os64_directory_data(const os64_directory_t *d, os64_ui_data_t **out)
{ return os64_directory_data_cancelled(d,NULL,NULL,out); }

static unsigned char fold(unsigned char c)
{ return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c; }
static bool matches(const char *name, const char *filter)
{
    if (!*filter) return true;
    for (const unsigned char *at = (const unsigned char *)name; *at; ++at) {
        const unsigned char *a = at, *b = (const unsigned char *)filter;
        while (*a && *b && fold(*a) == fold(*b)) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}
os64_ui_status_code_t os64_directory_project(const os64_directory_t *d, os64_ui_data_t *data,
    const char *filter, bool hidden, os64_ui_data_sort_t order,
    os64_ui_data_projection_t **out)
{
    if (!out) return OS64_UI_INVALID;
    *out = NULL;
    if (!d || !data || os64_ui_data_domain(data) != d->domain ||
        os64_ui_data_count(data) != d->count || !filter) return OS64_UI_INVALID;
    if (bounded_len(filter, OS64_DIRECTORY_FILTER) == OS64_DIRECTORY_FILTER)
        return OS64_UI_LIMIT;
    os64_ui_data_id_t *ids = os64_malloc((d->count ? d->count : 1) * sizeof(*ids));
    if (!ids) return OS64_UI_NO_MEMORY;
    size_t count = 0;
    for (size_t i = 0; i < d->count; ++i) {
        const char *name = d->entries[i].entry.name;
        if ((!hidden && name[0] == '.') || !matches(name,filter)) continue;
        ids[count++] = d->entries[i].id;
    }
    os64_ui_data_projection_t *base = NULL;
    os64_ui_status_code_t rc = os64_ui_data_projection_create(data, ids, count, &base);
    os64_free(ids);
    if (!rc) rc = os64_ui_data_projection_sort(base, order, out);
    os64_ui_data_projection_release(base);
    return rc;
}

static void put_place(os64_directory_places_t *out, const char *label, const char *path)
{
    if (out->count == OS64_DIRECTORY_PLACES) { out->incomplete = true; return; }
    char shown[OS64_DIRENT_NAME_MAX * 4 + 1];
    size_t l = display_name(label,shown);
    size_t p = bounded_len(path, OS64_DIRECTORY_PATH);
    if (l >= sizeof(out->items[0].label) || p == OS64_DIRECTORY_PATH || !p) {
        out->incomplete = true; return;
    }
    os64_directory_place_t *place = &out->items[out->count++];
    os64_memcpy(place->label,shown,l+1); os64_memcpy(place->path,path,p+1);
}
void os64_directory_places(os64_directory_places_t *out)
{
    if (!out) return;
    os64_memset(out,0,sizeof(*out));
    put_place(out,"Root","/");
    char normal[OS64_DIRECTORY_PATH];
    os64_directory_home(normal);
    if (!os64_streq(normal,"/")) put_place(out,"Home",normal);
    out->favorite_count=out->count;
    int64_t h=os64_open("/sys/mounts","r");
    if (h<0) { out->unavailable=true; return; }
    char contents[16385]; size_t used=0; int64_t got=0;
    while (used<sizeof(contents)) {
        got=os64_read((int32_t)h,contents+used,sizeof(contents)-used);
        if (got<=0) break;
        used+=(size_t)got;
    }
    int64_t closed=os64_close((int32_t)h);
    if (got<0 || closed<0) { out->unavailable=true; return; }
    if (used==sizeof(contents)) { out->incomplete=true; --used; }
    contents[used]=0;
    char *line=contents;
    while (*line) {
        char *end=line;
        while (*end && *end!='\n') ++end;
        bool complete=*end=='\n';
        if (complete) *end++=0;
        if (*line && *line!='#' && complete) {
            char *field[12]; size_t count=0;
            char *cursor=line;
            while (*cursor && count<12) {
                while (*cursor==' ' || *cursor=='\t' || *cursor=='\r') ++cursor;
                if (!*cursor) break;
                field[count++]=cursor;
                while (*cursor && *cursor!=' ' && *cursor!='\t' && *cursor!='\r') ++cursor;
                if (*cursor) *cursor++=0;
            }
            if (count==12) {
                os64_unescape_field(field[0]);
                if (field[0][0]=='/' && !os64_directory_path("/",field[0],normal) &&
                    os64_streq(normal,field[0]) && !os64_streq(normal,"/")) {
                    const char *label=normal;
                    for (const char *p=normal; *p; ++p) if (*p=='/' && p[1]) label=p+1;
                    put_place(out,label,normal);
                }
            } else out->incomplete=true;
        } else if (*line && *line!='#') out->incomplete=true;
        line=end;
    }
}
