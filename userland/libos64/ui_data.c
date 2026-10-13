#include "os64/ui_data.h"
#include "ui_data_internal.h"
#include "os64/mem.h"
#include "os64/str.h"

struct os64_ui_data {
    size_t refs, count, columns, bytes, hash_capacity;
    uint64_t domain;
    os64_ui_data_column_t *schema;
    os64_ui_data_row_t *rows;
    os64_ui_data_cell_t *cells;
    uint32_t *hash;
};
struct os64_ui_data_projection {
    size_t refs, count, bytes;
    os64_ui_data_t *data;
    os64_ui_data_sort_t order;
    uint32_t *source, *position;
};
/* Charge the real heap capacity and its 16-byte block header, including
 * recycled blocks whose capacity exceeds the requested payload. */
static size_t allocation_charge(const void *p)
{
    size_t capacity=os64_malloc_size(p);
    return capacity>SIZE_MAX-16?SIZE_MAX:capacity+16;
}
static bool add_bytes(size_t *n, size_t amount, size_t limit)
{
    if (amount > limit || *n > limit - amount) return false;
    *n += amount;
    return true;
}
static os64_ui_status_code_t text_size(os64_ui_data_text_t t, size_t *n)
{
    if (t.length > OS64_UI_DATA_TEXT) return OS64_UI_LIMIT;
    if (t.length && !t.bytes) return OS64_UI_INVALID;
    for (size_t i = 0; i < t.length; ++i)
        if (!t.bytes[i] || t.bytes[i] == '\n') return OS64_UI_INVALID;
    return add_bytes(n, t.length + 1, OS64_UI_DATA_BYTES) ? OS64_UI_OK : OS64_UI_LIMIT;
}
static os64_ui_data_text_t copy_text(os64_ui_data_text_t t, char **blob)
{
    char *p = *blob;
    if (t.length) os64_memcpy(p, t.bytes, t.length);
    p[t.length] = 0;
    *blob += t.length + 1;
    return (os64_ui_data_text_t){p, t.length};
}
static size_t hash_id(uint64_t id)
{
    id ^= id >> 30; id *= UINT64_C(0xbf58476d1ce4e5b9);
    id ^= id >> 27; id *= UINT64_C(0x94d049bb133111eb);
    return (size_t)(id ^ (id >> 31));
}
static bool source_position(const os64_ui_data_t *d, uint64_t id, size_t *out)
{
    if (!d || !id) return false;
    size_t h = hash_id(id) & (d->hash_capacity - 1);
    while (d->hash[h]) {
        size_t i = d->hash[h] - 1;
        if (d->rows[i].id == id) { if (out) *out = i; return true; }
        h = (h + 1) & (d->hash_capacity - 1);
    }
    return false;
}
os64_ui_status_code_t os64_ui_data_create(uint64_t domain,
    const os64_ui_data_column_t *columns, size_t nc,
    const os64_ui_data_row_t *rows, size_t nr, os64_ui_data_t **out)
{
    if (!out) return OS64_UI_INVALID;
    *out = NULL;
    if (nr > OS64_UI_DATA_ROWS || nc > OS64_UI_DATA_COLUMNS) return OS64_UI_LIMIT;
    if (!domain || !nc || !columns || (nr && !rows)) return OS64_UI_INVALID;
    size_t cap = 1;
    while (cap < nr * 2) cap *= 2;
    size_t bytes = sizeof(os64_ui_data_t) + nc * sizeof(*columns) +
        nr * sizeof(*rows) + nr * nc * sizeof(os64_ui_data_cell_t) + cap * sizeof(uint32_t);
    if (bytes > OS64_UI_DATA_BYTES) return OS64_UI_LIMIT;
    bool required = false;
    size_t search_columns = 0;
    os64_ui_status_code_t rc;
    for (size_t c = 0; c < nc; ++c) {
        const os64_ui_data_column_t *col = &columns[c];
        if (!col->id || col->type < OS64_UI_DATA_TEXT_KEY || col->type > OS64_UI_DATA_SIGNED || col->width < 48 || col->width > 4096)
            return OS64_UI_INVALID;
        for (size_t j = 0; j < c; ++j) if (columns[j].id == col->id) return OS64_UI_DUPLICATE;
        required |= col->required;
        search_columns += col->search;
        if ((rc = text_size(col->title, &bytes))) return rc;
    }
    if (!required || search_columns != 1) return OS64_UI_INVALID;
    for (size_t r = 0; r < nr; ++r) {
        if (!rows[r].id || !rows[r].cells) return OS64_UI_INVALID;
        if ((rc = text_size(rows[r].search, &bytes)) || (rc = text_size(rows[r].icon, &bytes))) return rc;
        for (size_t c = 0; c < nc; ++c) {
            const os64_ui_data_cell_t *cell = &rows[r].cells[c];
            if ((rc = text_size(cell->display, &bytes))) return rc;
            if (cell->key.present) {
                if (cell->key.type != columns[c].type) return OS64_UI_INVALID;
                if (cell->key.type == OS64_UI_DATA_TEXT_KEY &&
                    (rc = text_size(cell->key.value.text, &bytes))) return rc;
            }
        }
    }
    os64_ui_data_t *d = os64_malloc(bytes);
    if (!d) return OS64_UI_NO_MEMORY;
    size_t charge=allocation_charge(d);
    if (charge>OS64_UI_DATA_BYTES) { os64_free(d); return OS64_UI_LIMIT; }
    *d = (os64_ui_data_t){.refs=1,.count=nr,.columns=nc,.bytes=charge,.domain=domain,.hash_capacity=cap};
    d->schema = (void *)(d + 1);
    d->rows = (void *)(d->schema + nc);
    d->cells = (void *)(d->rows + nr);
    d->hash = (void *)(d->cells + nr * nc);
    os64_memset(d->hash, 0, cap * sizeof(uint32_t));
    char *blob = (void *)(d->hash + cap);
    for (size_t c = 0; c < nc; ++c) {
        d->schema[c] = columns[c]; d->schema[c].title = copy_text(columns[c].title, &blob);
    }
    for (size_t r = 0; r < nr; ++r) {
        d->rows[r] = rows[r];
        size_t h = hash_id(rows[r].id) & (cap - 1);
        while (d->hash[h]) {
            if (d->rows[d->hash[h]-1].id == rows[r].id) { os64_free(d); return OS64_UI_DUPLICATE; }
            h = (h + 1) & (cap - 1);
        }
        d->hash[h] = (uint32_t)(r + 1);
        d->rows[r].search = copy_text(rows[r].search, &blob);
        d->rows[r].icon = copy_text(rows[r].icon, &blob);
        d->rows[r].cells = &d->cells[r * nc];
        for (size_t c = 0; c < nc; ++c) {
            os64_ui_data_cell_t *cell = &d->cells[r * nc + c];
            *cell = rows[r].cells[c]; cell->display = copy_text(cell->display, &blob);
            if (cell->key.present && cell->key.type == OS64_UI_DATA_TEXT_KEY)
                cell->key.value.text = copy_text(cell->key.value.text, &blob);
        }
    }
    *out = d;
    return OS64_UI_OK;
}
void os64_ui_data_retain(os64_ui_data_t *d) { if (d) ++d->refs; }
void os64_ui_data_release(os64_ui_data_t *d) { if (d && !--d->refs) os64_free(d); }
uint64_t os64_ui_data_domain(const os64_ui_data_t *d) { return d ? d->domain : 0; }
size_t os64_ui_data_count(const os64_ui_data_t *d) { return d ? d->count : 0; }
size_t os64_ui_data_columns(const os64_ui_data_t *d) { return d ? d->columns : 0; }
size_t os64_ui_data_bytes(const os64_ui_data_t *d) { return d ? d->bytes : 0; }
const os64_ui_data_column_t *os64_ui_data_column(const os64_ui_data_t *d, size_t c)
{ return d && c < d->columns ? &d->schema[c] : NULL; }
const os64_ui_data_row_t *os64_ui_data_row_at(const os64_ui_data_t *d, size_t r)
{ return d && r < d->count ? &d->rows[r] : NULL; }
const os64_ui_data_row_t *os64_ui_data_row(const os64_ui_data_t *d, uint64_t id)
{ size_t i; return source_position(d,id,&i) ? &d->rows[i] : NULL; }
static bool text_equal(os64_ui_data_text_t a, os64_ui_data_text_t b)
{ return a.length == b.length && !os64_memcmp(a.bytes,b.bytes,a.length); }
bool os64_ui_data_schema_equal(const os64_ui_data_t *a, const os64_ui_data_t *b)
{
    if (!a || !b || a->columns != b->columns) return false;
    for (size_t c = 0; c < a->columns; ++c) {
        const os64_ui_data_column_t *x = &a->schema[c], *y = &b->schema[c];
        if (x->id != y->id || x->type != y->type || x->width != y->width ||
            x->right != y->right || x->sortable != y->sortable || x->required != y->required ||
            x->search != y->search || !text_equal(x->title,y->title)) return false;
    }
    return true;
}
static os64_ui_status_code_t projection_new(os64_ui_data_t *d, size_t n, os64_ui_data_projection_t **out)
{
    *out = NULL;
    if (n>OS64_UI_DATA_ROWS) return OS64_UI_LIMIT;
    if (!d || n > d->count) return OS64_UI_INVALID;
    size_t bytes = sizeof(os64_ui_data_projection_t) + (n + d->count) * sizeof(uint32_t);
    if (bytes > OS64_UI_DATA_VIEW_BYTES) return OS64_UI_LIMIT;
    os64_ui_data_projection_t *p = os64_malloc(bytes);
    if (!p) return OS64_UI_NO_MEMORY;
    size_t charge=allocation_charge(p);
    if (charge>OS64_UI_DATA_VIEW_BYTES) { os64_free(p); return OS64_UI_LIMIT; }
    *p = (os64_ui_data_projection_t){.refs=1,.count=n,.bytes=charge,.data=d};
    p->source = (void *)(p + 1); p->position = p->source + n;
    for (size_t i = 0; i < d->count; ++i) p->position[i] = UINT32_MAX;
    os64_ui_data_retain(d); *out = p;
    return OS64_UI_OK;
}
os64_ui_status_code_t os64_ui_data_projection_create(os64_ui_data_t *d,
    const uint64_t *ids, size_t n, os64_ui_data_projection_t **out)
{
    if (!out) return OS64_UI_INVALID;
    *out = NULL;
    if (n && !ids) return OS64_UI_INVALID;
    os64_ui_data_projection_t *p;
    os64_ui_status_code_t rc = projection_new(d,n,&p);
    if (rc) return rc;
    for (size_t i = 0; i < n; ++i) {
        size_t at;
        if (!source_position(d,ids[i],&at)) { rc = OS64_UI_INVALID; break; }
        if (p->position[at] != UINT32_MAX) { rc = OS64_UI_DUPLICATE; break; }
        p->source[i] = (uint32_t)at; p->position[at] = (uint32_t)i;
    }
    if (rc) os64_ui_data_projection_release(p); else *out = p;
    return rc;
}
os64_ui_status_code_t os64_ui_data_projection_identity(os64_ui_data_t *d, os64_ui_data_projection_t **out)
{
    if (!out) return OS64_UI_INVALID;
    *out = NULL;
    if (!d) return OS64_UI_INVALID;
    os64_ui_status_code_t rc = projection_new(d,d->count,out);
    if (rc) return rc;
    for (size_t i = 0; i < d->count; ++i) (*out)->source[i] = (*out)->position[i] = (uint32_t)i;
    return OS64_UI_OK;
}
static unsigned char folded(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c; }
static int compare_text(os64_ui_data_text_t a, os64_ui_data_text_t b)
{
    size_t n = a.length < b.length ? a.length : b.length;
    for (size_t i = 0; i < n; ++i) {
        unsigned char x = folded((unsigned char)a.bytes[i]), y = folded((unsigned char)b.bytes[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return a.length < b.length ? -1 : a.length > b.length ? 1 : 0;
}
static int compare_rows(os64_ui_data_t *d, uint32_t a, uint32_t b, size_t column, bool desc)
{
    const os64_ui_data_row_t *x = &d->rows[a], *y = &d->rows[b];
    if (x->group != y->group) return x->group < y->group ? -1 : 1;
    const os64_ui_data_key_t *ka = &x->cells[column].key, *kb = &y->cells[column].key;
    if (ka->present != kb->present) return ka->present ? -1 : 1;
    int cmp = 0;
    if (ka->present) {
        if (ka->type == OS64_UI_DATA_TEXT_KEY) cmp = compare_text(ka->value.text,kb->value.text);
        else if (ka->type == OS64_UI_DATA_UNSIGNED)
            cmp = ka->value.u < kb->value.u ? -1 : ka->value.u > kb->value.u ? 1 : 0;
        else cmp = ka->value.i < kb->value.i ? -1 : ka->value.i > kb->value.i ? 1 : 0;
    }
    if (cmp) return desc ? (cmp < 0 ? 1 : -1) : cmp;
    return a < b ? -1 : a > b ? 1 : 0;
}
os64_ui_status_code_t os64_ui_data_projection_sort(const os64_ui_data_projection_t *p,
    os64_ui_data_sort_t order, os64_ui_data_projection_t **out)
{
    if (!out) return OS64_UI_INVALID;
    *out = NULL;
    if (!p) return OS64_UI_INVALID;
    size_t c = 0;
    while (c < p->data->columns && p->data->schema[c].id != order.column) ++c;
    if (c == p->data->columns || !p->data->schema[c].sortable) return OS64_UI_INVALID;
    if (p->bytes * 2 + p->count * sizeof(uint32_t) > OS64_UI_DATA_VIEW_BYTES) return OS64_UI_LIMIT;
    os64_ui_data_projection_t *q;
    os64_ui_status_code_t rc = projection_new(p->data,p->count,&q);
    if (rc) return rc;
    uint32_t *scratch = p->count ? os64_malloc(p->count * sizeof(uint32_t)) : NULL;
    if (p->count && !scratch) { os64_ui_data_projection_release(q); return OS64_UI_NO_MEMORY; }
    size_t peak=p->bytes+q->bytes+(scratch?allocation_charge(scratch):0);
    if (peak>OS64_UI_DATA_VIEW_BYTES) { os64_free(scratch); os64_ui_data_projection_release(q); return OS64_UI_LIMIT; }
    os64_memcpy(q->source,p->source,p->count * sizeof(uint32_t));
    for (size_t step = 1; step < p->count; step *= 2) {
        for (size_t start = 0; start < p->count; start += step * 2) {
            size_t mid = start + step < p->count ? start + step : p->count;
            size_t end = mid + step < p->count ? mid + step : p->count;
            size_t a = start, b = mid;
            for (size_t i = start; i < end; ++i)
                scratch[i] = b == end || (a < mid && compare_rows(p->data,q->source[a],q->source[b],c,order.descending) <= 0)
                    ? q->source[a++] : q->source[b++];
        }
        os64_memcpy(q->source,scratch,p->count * sizeof(uint32_t));
    }
    os64_free(scratch);
    for (size_t i = 0; i < q->count; ++i) q->position[q->source[i]] = (uint32_t)i;
    q->order = order; *out = q;
    return OS64_UI_OK;
}
void os64_ui_data_projection_retain(os64_ui_data_projection_t *p) { if (p) ++p->refs; }
void os64_ui_data_projection_release(os64_ui_data_projection_t *p)
{ if (p && !--p->refs) { os64_ui_data_release(p->data); os64_free(p); } }
os64_ui_data_t *os64_ui_data_projection_snapshot(const os64_ui_data_projection_t *p) { return p ? p->data : NULL; }
size_t os64_ui_data_projection_count(const os64_ui_data_projection_t *p) { return p ? p->count : 0; }
size_t os64_ui_data_projection_bytes(const os64_ui_data_projection_t *p) { return p ? p->bytes : 0; }
os64_ui_data_sort_t os64_ui_data_projection_order(const os64_ui_data_projection_t *p)
{ return p ? p->order : (os64_ui_data_sort_t){0}; }
uint64_t os64_ui_data_projection_id(const os64_ui_data_projection_t *p, size_t at)
{ return p && at < p->count ? p->data->rows[p->source[at]].id : 0; }
bool os64_ui_data_projection_position(const os64_ui_data_projection_t *p, uint64_t id, size_t *out)
{
    size_t source;
    if (!p || !source_position(p->data,id,&source) || p->position[source] == UINT32_MAX) return false;
    if (out) *out = p->position[source];
    return true;
}
static ui_selection_state *state(const os64_ui_selection_t *s) { return s ? s->state : NULL; }
void os64_ui_selection_init(os64_ui_selection_t *s) { if (s) s->state = NULL; }
os64_ui_status_code_t os64_ui_selection_dispose(os64_ui_selection_t *s)
{
    if (!s) return OS64_UI_INVALID;
    ui_selection_state *v = state(s);
    if (v && v->owner) return OS64_UI_BUSY;
    if (v) { os64_ui_data_projection_release(v->projection); os64_free(v); s->state = NULL; }
    return OS64_UI_OK;
}
os64_ui_data_projection_t *os64_ui_selection_projection(const os64_ui_selection_t *s) { return state(s) ? state(s)->projection : NULL; }
uint64_t os64_ui_selection_focus(const os64_ui_selection_t *s) { return state(s) ? state(s)->focus : 0; }
uint64_t os64_ui_selection_anchor(const os64_ui_selection_t *s) { return state(s) ? state(s)->anchor : 0; }
size_t os64_ui_selection_count(const os64_ui_selection_t *s) { return state(s) ? state(s)->count : 0; }
size_t os64_ui_selection_bytes(const os64_ui_selection_t *s) { return state(s) ? state(s)->bytes : 0; }
uint64_t os64_ui_selection_revision(const os64_ui_selection_t *s) { return state(s) ? state(s)->revision : 0; }
bool os64_ui_selection_contains(const os64_ui_selection_t *s, uint64_t id)
{ size_t at; ui_selection_state *v=state(s); return v && os64_ui_data_projection_position(v->projection,id,&at) && v->selected[at]; }
static os64_ui_status_code_t selection_new(os64_ui_data_projection_t *p, os64_ui_selection_t *out)
{
    size_t bytes = sizeof(ui_selection_state) + os64_ui_data_projection_count(p);
    ui_selection_state *v = os64_malloc(bytes);
    if (!v) return OS64_UI_NO_MEMORY;
    os64_memset(v,0,bytes); v->storage=bytes; v->bytes=allocation_charge(v);
    if (v->bytes+os64_ui_data_projection_bytes(p)>OS64_UI_DATA_VIEW_BYTES) { os64_free(v); return OS64_UI_LIMIT; }
    v->selected=(void *)(v+1); v->projection=p;
    os64_ui_data_projection_retain(p); out->state=v;
    return OS64_UI_OK;
}
os64_ui_status_code_t ui_selection_copy(const os64_ui_selection_t *s, os64_ui_selection_t *out)
{
    out->state=NULL;
    ui_selection_state *v=state(s);
    if (!v) return OS64_UI_INVALID;
    ui_selection_state *q=os64_malloc(v->storage);
    if (!q) return OS64_UI_NO_MEMORY;
    os64_memcpy(q,v,v->storage); q->bytes=allocation_charge(q);
    if (v->bytes+q->bytes+os64_ui_data_projection_bytes(v->projection)>OS64_UI_DATA_VIEW_BYTES) { os64_free(q); return OS64_UI_LIMIT; }
    q->selected=(void *)(q+1); q->owner=NULL;
    os64_ui_data_projection_retain(q->projection); out->state=q;
    return OS64_UI_OK;
}
void ui_selection_publish(os64_ui_selection_t *s, os64_ui_selection_t *candidate, void *owner)
{
    if (state(s)) state(s)->owner=NULL;
    os64_ui_selection_dispose(s); s->state=candidate->state; candidate->state=NULL;
    if (state(s)) state(s)->owner=owner;
}
os64_ui_status_code_t os64_ui_selection_bind(os64_ui_selection_t *s, os64_ui_data_projection_t *p, os64_ui_data_bind_t mode)
{
    if (!s || !p || mode < OS64_UI_DATA_PRESERVE || mode > OS64_UI_DATA_RESET) return OS64_UI_INVALID;
    ui_selection_state *old=state(s);
    if (old && old->owner) return OS64_UI_BUSY;
    if (old && old->revision == UINT64_MAX) return OS64_UI_LIMIT;
    bool preserve = old && mode == OS64_UI_DATA_PRESERVE &&
        os64_ui_data_domain(os64_ui_data_projection_snapshot(old->projection)) == os64_ui_data_domain(p->data);
    if (preserve && !os64_ui_data_schema_equal(os64_ui_data_projection_snapshot(old->projection),p->data)) return OS64_UI_INVALID;
    size_t peak = (old ? old->bytes + os64_ui_data_projection_bytes(old->projection) : 0) +
        sizeof(ui_selection_state) + p->count + (old && old->projection == p ? 0 : p->bytes);
    if (peak > OS64_UI_DATA_VIEW_BYTES) return OS64_UI_LIMIT;
    os64_ui_selection_t candidate={0};
    os64_ui_status_code_t rc=selection_new(p,&candidate);
    if (rc) return rc;
    ui_selection_state *v=state(&candidate);
    size_t actual=(old?old->bytes+os64_ui_data_projection_bytes(old->projection):0)+v->bytes+(old && old->projection==p?0:p->bytes);
    if (actual>OS64_UI_DATA_VIEW_BYTES) { os64_ui_selection_dispose(&candidate); return OS64_UI_LIMIT; }
    v->revision=old ? old->revision+1 : 1;
    if (preserve && p->count) {
        for (size_t i=0;i<p->count;++i) {
            uint64_t id=os64_ui_data_projection_id(p,i);
            if (os64_ui_selection_contains(s,id)) { v->selected[i]=1; ++v->count; }
        }
        v->focus=old->focus; v->anchor=old->anchor;
        if (v->focus && !os64_ui_data_projection_position(p,v->focus,NULL)) {
            size_t at=0; os64_ui_data_projection_position(old->projection,v->focus,&at);
            v->focus=os64_ui_data_projection_id(p,at<p->count?at:p->count-1);
        }
        if (v->anchor && !os64_ui_data_projection_position(p,v->anchor,NULL)) v->anchor=v->focus;
    }
    ui_selection_publish(s,&candidate,NULL);
    return OS64_UI_OK;
}
static void set_selected(ui_selection_state *v,size_t at,bool value)
{
    if (!!v->selected[at] == value) return;
    v->selected[at]=(unsigned char)value;
    if (value) ++v->count; else --v->count;
}
os64_ui_status_code_t os64_ui_selection_command(os64_ui_selection_t *s, os64_ui_selection_command_t cmd, uint64_t id)
{
    ui_selection_state *v=state(s);
    if (!v || cmd < OS64_UI_SELECTION_CHOOSE || cmd > OS64_UI_SELECTION_CONTEXT) return OS64_UI_INVALID;
    if (v->owner) return OS64_UI_BUSY;
    if (v->revision == UINT64_MAX) return OS64_UI_LIMIT;
    size_t at=0, n=v->projection->count;
    if (cmd != OS64_UI_SELECTION_ALL && cmd != OS64_UI_SELECTION_CLEAR &&
        !os64_ui_data_projection_position(v->projection,id,&at)) return OS64_UI_INVALID;
    if (cmd == OS64_UI_SELECTION_CONTEXT && !v->selected[at]) cmd=OS64_UI_SELECTION_CHOOSE;
    if (cmd == OS64_UI_SELECTION_CLEAR || cmd == OS64_UI_SELECTION_CHOOSE || cmd == OS64_UI_SELECTION_RANGE) {
        os64_memset(v->selected,0,n); v->count=0;
    }
    switch (cmd) {
    case OS64_UI_SELECTION_CLEAR: v->focus=v->anchor=0; break;
    case OS64_UI_SELECTION_ALL:
        os64_memset(v->selected,1,n); v->count=n;
        if (n && !v->focus) v->focus=os64_ui_data_projection_id(v->projection,0);
        if (n && !v->anchor) v->anchor=v->focus;
        break;
    case OS64_UI_SELECTION_FOCUS: case OS64_UI_SELECTION_CONTEXT: v->focus=id; break;
    case OS64_UI_SELECTION_CHOOSE: set_selected(v,at,true); v->focus=v->anchor=id; break;
    case OS64_UI_SELECTION_TOGGLE: set_selected(v,at,!v->selected[at]); v->focus=v->anchor=id; break;
    case OS64_UI_SELECTION_RANGE: case OS64_UI_SELECTION_UNION: {
        if (!v->anchor) v->anchor=v->focus?v->focus:id;
        size_t anchor=at; os64_ui_data_projection_position(v->projection,v->anchor,&anchor);
        size_t lo=at<anchor?at:anchor, hi=at>anchor?at:anchor;
        for (size_t i=lo;i<=hi;++i) set_selected(v,i,true);
        v->focus=id;
        break;
    }
    }
    ++v->revision;
    return OS64_UI_OK;
}
os64_ui_status_code_t os64_ui_selection_next(const os64_ui_selection_t *s,uint64_t revision,size_t *cursor,uint64_t *out)
{
    if (!cursor || !out) return OS64_UI_INVALID;
    *out=0;
    ui_selection_state *v=state(s);
    if (!v) return revision ? OS64_UI_STALE : OS64_UI_EMPTY;
    if (revision != v->revision) return OS64_UI_STALE;
    while (*cursor < v->projection->count) {
        size_t at=(*cursor)++;
        if (v->selected[at]) { *out=os64_ui_data_projection_id(v->projection,at); return OS64_UI_OK; }
    }
    return OS64_UI_EMPTY;
}
