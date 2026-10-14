#include "os64/browser.h"
#include "directory_internal.h"
#include "os64/work.h"
#include "os64/mem.h"
#include "os64/str.h"

#define BROWSER_WORK_RESERVE (64u * 1024u * 1024u)
#define BROWSER_CACHE_STATES 8u

typedef enum { REQUEST_NEW, REQUEST_REFRESH, REQUEST_HISTORY } request_kind_t;
typedef struct {
    char path[OS64_DIRECTORY_PATH];
    uint64_t generation, domain;
    request_kind_t kind;
    size_t target;
} browser_request_t;
typedef struct {
    char path[OS64_DIRECTORY_PATH];
    uint64_t domain, used;
    char filter[OS64_DIRECTORY_FILTER];
    bool hidden;
    os64_ui_data_sort_t sort;
    os64_directory_t *raw;
    os64_ui_selection_t selection;
    size_t top;
    int64_t left;
} history_t;
typedef struct browser_job {
    struct os64_browser *browser;
    browser_request_t request;
    os64_directory_t *previous;
} browser_job_t;
typedef struct {
    os64_directory_t *raw;
    os64_ui_data_t *data;
    os64_directory_places_t places;
    int64_t io_error;
    os64_ui_status_code_t status;
} browser_product_t;
struct os64_browser {
    os64_ui_dataview_t *view;
    os64_ui_selection_t selection;
    os64_work_pool_t *pool;
    os64_directory_t *current;
    history_t history[OS64_BROWSER_HISTORY];
    size_t count, cursor, inflight;
    uint64_t generation, next_domain, tick, revision;
    size_t cache_bytes;
    browser_request_t intent;
    bool has_intent, closing;
    os64_work_id_t live_id;
    browser_job_t *intent_job;
    size_t retired, retired_seen;
    char requested[OS64_DIRECTORY_PATH];
    char filter[OS64_DIRECTORY_FILTER];
    bool hidden;
    os64_directory_places_t places;
    os64_ui_status_code_t status;
    int64_t io_error;
};

static void copy_string(char *to, const char *from, size_t cap)
{
    size_t n=os64_strlen(from);
    if (n>=cap) n=cap-1;
    os64_memcpy(to,from,n); to[n]=0;
}
static os64_ui_data_sort_t current_sort(os64_browser_t *b)
{
    os64_ui_data_projection_t *p=os64_ui_selection_projection(&b->selection);
    os64_ui_data_sort_t order=os64_ui_data_projection_order(p);
    if (!order.column) order.column=1;
    return order;
}
static size_t state_bytes(const history_t *h)
{
    if (!h->raw) return 0;
    os64_ui_data_projection_t *p=os64_ui_selection_projection(&h->selection);
    os64_ui_data_t *d=os64_ui_data_projection_snapshot(p);
    return os64_directory_bytes(h->raw)+os64_ui_selection_bytes(&h->selection)+
        os64_ui_data_projection_bytes(p)+os64_ui_data_bytes(d);
}
static void clear_cache(os64_browser_t *b, history_t *h)
{
    size_t bytes=state_bytes(h);
    if (bytes<=b->cache_bytes) b->cache_bytes-=bytes;
    else b->cache_bytes=0;
    os64_directory_release(h->raw); h->raw=NULL;
    os64_ui_selection_dispose(&h->selection);
    h->top=0; h->left=0;
}
static void evict(os64_browser_t *b)
{
    for (;;) {
        size_t states=b->current?1:0, victim=OS64_BROWSER_HISTORY;
        os64_ui_data_projection_t *active=os64_ui_selection_projection(&b->selection);
        size_t total=b->cache_bytes+(b->current?os64_directory_bytes(b->current):0)+
            os64_ui_selection_bytes(&b->selection)+os64_ui_data_projection_bytes(active)+
            os64_ui_data_bytes(os64_ui_data_projection_snapshot(active));
        uint64_t oldest=UINT64_MAX;
        for (size_t i=0;i<b->count;++i) {
            history_t *h=&b->history[i];
            if (!h->raw) continue;
            ++states;
            if (i!=b->cursor && h->used<oldest) { oldest=h->used; victim=i; }
        }
        if (states<=BROWSER_CACHE_STATES && total<=OS64_BROWSER_CACHE_BYTES) break;
        if (victim==OS64_BROWSER_HISTORY) break;
        clear_cache(b,&b->history[victim]);
    }
}
static void release_job(void *input, void *output)
{
    browser_job_t *job=input;
    browser_product_t *product=output;
    if (product) {
        os64_directory_release(product->raw);
        os64_ui_data_release(product->data);
        os64_free(product);
    }
    if (job) {
        os64_directory_release(job->previous);
        os64_free(job);
    }
}
/* Pool callbacks can run before submit returns, including on pool failure.
 * Retire through the browser, whose lifetime covers the pool's joined workers;
 * the UI keeps a job ID and does not dereference pool-owned input. */
static void release_pool_job(void *input, void *output)
{
    browser_job_t *job=input;
    os64_browser_t *b=job->browser;
    release_job(input,output);
    __atomic_add_fetch(&b->retired,1,__ATOMIC_RELEASE);
}
static int64_t run_job(void *input, bool (*cancelled)(void *), void *ctx, void **out)
{
    browser_job_t *job=input;
    browser_product_t *product=os64_calloc(1,sizeof(*product));
    if (!product) return OS64_UI_NO_MEMORY;
    *out=product;
    product->status=os64_directory_read(job->request.path,job->request.domain,
        job->previous,cancelled,ctx,&product->raw,&product->io_error);
    if (!product->status && (!cancelled || !cancelled(ctx)))
        product->status=os64_directory_data_cancelled(product->raw,cancelled,ctx,&product->data);
    if (!product->status && (!cancelled || !cancelled(ctx)))
        os64_directory_places(&product->places);
    if (cancelled && cancelled(ctx)) product->status=OS64_UI_STALE;
    return product->status;
}
static void capture_choices(os64_browser_t *b)
{
    if (!b->count) return;
    history_t *h=&b->history[b->cursor];
    copy_string(h->filter,b->filter,sizeof(h->filter));
    h->hidden=b->hidden; h->sort=current_sort(b);
}
static os64_directory_t *previous_for(os64_browser_t *b,const browser_request_t *r)
{
    if (r->kind==REQUEST_REFRESH) return b->current;
    if (r->kind==REQUEST_HISTORY && r->target<b->count) return b->history[r->target].raw;
    return NULL;
}
static bool submit_intent(os64_browser_t *b)
{
    if (!b->has_intent || b->inflight>=2) return false;
    browser_job_t *job=b->intent_job;
    os64_work_t work={.run=run_job,.release=release_pool_job,.job=job,
        .reserve=BROWSER_WORK_RESERVE};
    os64_work_id_t id=os64_work_submit(b->pool,&work);
    if (!id) { b->status=OS64_UI_REFUSED; return false; }
    b->live_id=id; ++b->inflight;
    b->has_intent=false; b->intent_job=NULL;
    return true;
}
static void cancel_live(os64_browser_t *b)
{
    if (!b->live_id) return;
    os64_work_id_t id=b->live_id; b->live_id=0;
    os64_work_cancel(b->pool,id);
}

os64_ui_status_code_t os64_browser_create(os64_ui_dataview_t *view,
    const os64_ui_icon_provider_t *icons,int64_t window,uint32_t doorbell_bit,
    os64_browser_t **out)
{
    if (!out) return OS64_UI_INVALID;
    *out=NULL;
    if (!view || !window || !doorbell_bit || (doorbell_bit&(doorbell_bit-1)))
        return OS64_UI_INVALID;
    os64_browser_t *b=os64_calloc(1,sizeof(*b));
    if (!b) return OS64_UI_NO_MEMORY;
    b->view=view; b->next_domain=2; b->status=OS64_UI_OK;
    os64_ui_selection_init(&b->selection);
    os64_ui_data_column_t c={1,{"Name",4},OS64_UI_DATA_TEXT_KEY,260,false,true,true,true};
    os64_ui_data_t *data=NULL;
    os64_ui_data_projection_t *projection=NULL;
    os64_ui_status_code_t rc=os64_ui_data_create(1,&c,1,NULL,0,&data);
    if (!rc) rc=os64_ui_data_projection_identity(data,&projection);
    if (!rc) rc=os64_ui_selection_bind(&b->selection,projection,OS64_UI_DATA_RESET);
    bool attached=false;
    if (!rc) {
        rc=os64_ui_dataview_attach(view,&b->selection,icons);
        attached=rc==OS64_UI_OK;
    }
    os64_ui_data_projection_release(projection); os64_ui_data_release(data);
    if (!rc) {
        b->pool=os64_work_pool_create(2,2u*BROWSER_WORK_RESERVE,window,doorbell_bit);
        if (!b->pool) rc=OS64_UI_ERROR;
    }
    if (rc) {
        if (attached) os64_ui_dataview_dispose(view);
        os64_ui_selection_dispose(&b->selection);
        os64_free(b);
        return rc;
    }
    *out=b; return OS64_UI_OK;
}

static os64_ui_status_code_t request(os64_browser_t *b,const char *path,
    request_kind_t kind,size_t target)
{
    if (!b || b->closing || !path) return OS64_UI_INVALID;
    if (os64_work_pool_error(b->pool)) return OS64_UI_ERROR;
    if (b->generation==UINT64_MAX || b->next_domain==UINT64_MAX) return OS64_UI_LIMIT;
    char normal[OS64_DIRECTORY_PATH];
    const char *base=b->count?b->history[b->cursor].path:"/";
    os64_ui_status_code_t rc=os64_directory_path(base,path,normal);
    if (rc) return rc;
    if (kind==REQUEST_NEW && b->count && os64_streq(normal,base)) kind=REQUEST_REFRESH;
    bool fresh_domain=kind==REQUEST_NEW ||
        (kind==REQUEST_HISTORY && !b->history[target].raw);
    browser_request_t next={.generation=b->generation+1,.kind=kind,.target=target,
        .domain=fresh_domain?b->next_domain:
            kind==REQUEST_REFRESH?b->history[b->cursor].domain:b->history[target].domain};
    copy_string(next.path,normal,sizeof(next.path));
    browser_job_t *job=os64_calloc(1,sizeof(*job));
    if (!job) return OS64_UI_NO_MEMORY;
    job->browser=b; job->request=next; job->previous=previous_for(b,&next);
    os64_directory_retain(job->previous);
    os64_work_id_t admitted=0;
    if (b->inflight<2) {
        os64_work_t work={.run=run_job,.release=release_pool_job,.job=job,
            .reserve=BROWSER_WORK_RESERVE};
        admitted=os64_work_submit(b->pool,&work);
        if (!admitted) { release_job(job,NULL); return OS64_UI_REFUSED; }
    }
    if (fresh_domain) ++b->next_domain;
    if (b->has_intent) {
        release_job(b->intent_job,NULL);
        b->has_intent=false; b->intent_job=NULL;
    }
    cancel_live(b);
    b->generation=next.generation;
    b->intent=next;
    if (admitted) {b->live_id=admitted;++b->inflight;}
    else {b->has_intent=true;b->intent_job=job;}
    copy_string(b->requested,normal,sizeof(b->requested));
    b->status=OS64_UI_OK; b->io_error=0; ++b->revision;
    return OS64_UI_OK;
}
os64_ui_status_code_t os64_browser_navigate(os64_browser_t *b,const char *path)
{ return request(b,path,REQUEST_NEW,0); }
os64_ui_status_code_t os64_browser_refresh(os64_browser_t *b)
{ return b && b->count?request(b,b->history[b->cursor].path,REQUEST_REFRESH,b->cursor):OS64_UI_REFUSED; }
os64_ui_status_code_t os64_browser_back(os64_browser_t *b)
{ return b && b->count && b->cursor?request(b,b->history[b->cursor-1].path,REQUEST_HISTORY,b->cursor-1):OS64_UI_REFUSED; }
os64_ui_status_code_t os64_browser_forward(os64_browser_t *b)
{ return b && b->count && b->cursor+1<b->count?request(b,b->history[b->cursor+1].path,REQUEST_HISTORY,b->cursor+1):OS64_UI_REFUSED; }
os64_ui_status_code_t os64_browser_up(os64_browser_t *b)
{
    if (!b || !b->count) return OS64_UI_REFUSED;
    const char *path=b->history[b->cursor].path;
    if (os64_streq(path,"/")) return OS64_UI_REFUSED;
    return request(b,"..",REQUEST_NEW,0);
}

void os64_browser_cancel(os64_browser_t *b)
{
    if (!b || b->closing) return;
    if (b->has_intent) { release_job(b->intent_job,NULL); b->intent_job=NULL; b->has_intent=false; }
    cancel_live(b); b->requested[0]=0; ++b->revision;
}

static os64_ui_status_code_t selection_for(os64_browser_t *b,
    const browser_request_t *request, os64_ui_data_projection_t *p,
    os64_ui_selection_t *candidate, size_t *top, int64_t *left)
{
    os64_ui_selection_init(candidate); *top=0; *left=0;
    const os64_ui_selection_t *source=NULL;
    if (request->kind==REQUEST_REFRESH) {
        source=&b->selection;
        *top=os64_ui_dataview_top(b->view); *left=os64_ui_dataview_left(b->view);
    } else if (request->kind==REQUEST_HISTORY && request->target<b->count &&
               b->history[request->target].selection.state) {
        history_t *h=&b->history[request->target];
        source=&h->selection; *top=h->top; *left=h->left;
    }
    os64_ui_status_code_t rc=source?os64_ui_selection_clone(source,candidate):OS64_UI_OK;
    if (!rc) rc=os64_ui_selection_bind(candidate,p,source?OS64_UI_DATA_PRESERVE:OS64_UI_DATA_RESET);
    if (rc) os64_ui_selection_dispose(candidate);
    return rc;
}

static os64_ui_status_code_t publish_product(os64_browser_t *b,
    const browser_request_t *request,browser_product_t *product)
{
    if (product->status) { b->io_error=product->io_error; return product->status; }
    const char *filter=""; bool hidden=false;
    os64_ui_data_sort_t sort=current_sort(b);
    if (request->kind==REQUEST_REFRESH) { filter=b->filter; hidden=b->hidden; }
    else if (request->kind==REQUEST_HISTORY) {
        const history_t *h=&b->history[request->target];
        filter=h->filter; hidden=h->hidden; sort=h->sort;
    }
    os64_ui_data_projection_t *p=NULL;
    os64_ui_status_code_t rc=os64_directory_project(product->raw,product->data,
        filter,hidden,sort,&p);
    if (rc) return rc;
    os64_ui_selection_t candidate={0}, bookmark={0};
    size_t top=0; int64_t left=0;
    rc=selection_for(b,request,p,&candidate,&top,&left);
    os64_ui_data_projection_release(p);
    if (rc) return rc;
    bool leaving=request->kind!=REQUEST_REFRESH && b->count;
    size_t old_top=os64_ui_dataview_top(b->view);
    int64_t old_left=os64_ui_dataview_left(b->view);
    if (leaving) {
        rc=os64_ui_selection_clone(&b->selection,&bookmark);
        if (rc) { os64_ui_selection_dispose(&candidate); return rc; }
    }
    if (leaving) capture_choices(b);
    rc=os64_ui_dataview_restore(b->view,&candidate,top,left);
    os64_ui_selection_dispose(&candidate);
    if (rc) { os64_ui_selection_dispose(&bookmark); return rc; }

    if (leaving) {
        history_t *old=&b->history[b->cursor];
        clear_cache(b,old);
        old->selection=bookmark; old->raw=b->current;
        os64_directory_retain(old->raw);
        old->top=old_top; old->left=old_left;
        old->used=++b->tick;
        b->cache_bytes+=state_bytes(old);
    }
    if (request->kind==REQUEST_NEW) {
        for (size_t i=b->cursor+1;i<b->count;++i) clear_cache(b,&b->history[i]);
        b->count=b->count?b->cursor+1:0;
        if (b->count==OS64_BROWSER_HISTORY) {
            clear_cache(b,&b->history[0]);
            os64_memmove(&b->history[0],&b->history[1],
                (OS64_BROWSER_HISTORY-1)*sizeof(history_t));
            --b->count;
        }
        history_t *h=&b->history[b->count++];
        *h=(history_t){.domain=request->domain,.sort=sort};
        copy_string(h->path,request->path,sizeof(h->path));
        b->cursor=b->count-1;
        b->filter[0]=0; b->hidden=false;
    } else if (request->kind==REQUEST_HISTORY) {
        clear_cache(b,&b->history[request->target]);
        b->cursor=request->target;
        b->history[b->cursor].domain=request->domain;
        copy_string(b->filter,filter,sizeof(b->filter)); b->hidden=hidden;
    }
    os64_directory_release(b->current);
    b->current=product->raw; os64_directory_retain(b->current);
    b->places=product->places;
    b->history[b->cursor].used=++b->tick;
    capture_choices(b); evict(b);
    b->io_error=0; ++b->revision;
    return OS64_UI_OK;
}

os64_ui_status_code_t os64_browser_poll(os64_browser_t *b)
{
    if (!b || b->closing) return OS64_UI_INVALID;
    size_t retired=__atomic_load_n(&b->retired,__ATOMIC_ACQUIRE);
    if (retired>b->retired_seen) {
        size_t freed=retired-b->retired_seen;
        b->inflight=freed>b->inflight?0:b->inflight-freed;
        b->retired_seen=retired;
    }
    os64_ui_status_code_t last=b->status;
    os64_work_id_t id; int64_t verdict; void *input,*output;
    while (os64_work_reap(b->pool,&id,&verdict,&input,&output)) {
        browser_job_t *job=input;
        browser_product_t *product=output;
        if (b->inflight) --b->inflight;
        if (id==b->live_id && job->request.generation==b->generation) {
            b->live_id=0;
            last=product?publish_product(b,&job->request,product):
                verdict>=0?(os64_ui_status_code_t)verdict:OS64_UI_ERROR;
            b->status=last;
            b->requested[0]=0; ++b->revision;
        }
        release_job(job,product);
    }
    if (os64_work_pool_error(b->pool)) {
        if (b->has_intent) {release_job(b->intent_job,NULL);b->intent_job=NULL;b->has_intent=false;}
        cancel_live(b);b->requested[0]=0;
        b->status=OS64_UI_ERROR;last=OS64_UI_ERROR;
    }
    if (b->has_intent && !submit_intent(b) && b->status!=OS64_UI_OK)
        last=b->status;
    return last;
}

os64_ui_status_code_t os64_browser_filter(os64_browser_t *b,const char *filter,bool hidden)
{
    if (!b || !filter || b->closing) return OS64_UI_INVALID;
    size_t n=os64_strlen(filter);
    if (n>=OS64_DIRECTORY_FILTER) return OS64_UI_LIMIT;
    if (!b->current) {
        copy_string(b->filter,filter,sizeof(b->filter)); b->hidden=hidden;
        ++b->revision; return OS64_UI_OK;
    }
    os64_ui_data_projection_t *old=os64_ui_selection_projection(&b->selection);
    os64_ui_data_t *data=os64_ui_data_projection_snapshot(old);
    os64_ui_data_projection_t *p=NULL;
    os64_ui_status_code_t rc=os64_directory_project(b->current,data,filter,hidden,
        os64_ui_data_projection_order(old),&p);
    if (rc) return rc;
    os64_ui_selection_t candidate={0};
    rc=os64_ui_selection_clone(&b->selection,&candidate);
    if (!rc) rc=os64_ui_selection_bind(&candidate,p,OS64_UI_DATA_PRESERVE);
    os64_ui_data_projection_release(p);
    if (!rc) rc=os64_ui_dataview_restore(b->view,&candidate,
        os64_ui_dataview_top(b->view),os64_ui_dataview_left(b->view));
    os64_ui_selection_dispose(&candidate);
    if (!rc) {
        copy_string(b->filter,filter,sizeof(b->filter)); b->hidden=hidden;
        capture_choices(b); ++b->revision;
    }
    b->status=rc;
    return rc;
}
os64_ui_status_code_t os64_browser_activate(os64_browser_t *b,uint64_t domain,os64_ui_data_id_t id)
{
    if (!b || !b->current || domain!=os64_directory_domain(b->current)) return OS64_UI_STALE;
    const os64_directory_entry_t *entry=os64_directory_entry(b->current,id);
    if (!entry) return OS64_UI_STALE;
    if (!(entry->entry.flags&OS64_DE_DIR)) return OS64_UI_REFUSED;
    char path[OS64_DIRECTORY_PATH];
    os64_ui_status_code_t rc=os64_directory_join(os64_directory_location(b->current),entry->entry.name,path);
    return rc?rc:os64_browser_navigate(b,path);
}
void os64_browser_destroy(os64_browser_t *b)
{
    if (!b) return;
    b->closing=true;
    os64_work_pool_destroy(b->pool);
    if (b->has_intent) release_job(b->intent_job,NULL);
    for (size_t i=0;i<b->count;++i) clear_cache(b,&b->history[i]);
    os64_directory_release(b->current);
    os64_ui_dataview_dispose(b->view);
    os64_ui_selection_dispose(&b->selection);
    os64_free(b);
}
const char *os64_browser_location(const os64_browser_t *b)
{ return b&&b->count?b->history[b->cursor].path:""; }
const char *os64_browser_requested(const os64_browser_t *b)
{ return b&&b->requested[0]?b->requested:""; }
const char *os64_browser_filter_text(const os64_browser_t *b)
{ return b?b->filter:""; }
bool os64_browser_hidden(const os64_browser_t *b) { return b&&b->hidden; }
bool os64_browser_loading(const os64_browser_t *b)
{ return b && (b->has_intent || b->live_id); }
bool os64_browser_can_back(const os64_browser_t *b)
{ return b && b->count && b->cursor>0; }
bool os64_browser_can_forward(const os64_browser_t *b)
{ return b && b->count && b->cursor+1<b->count; }
const os64_directory_t *os64_browser_directory(const os64_browser_t *b)
{ return b?b->current:NULL; }
const os64_directory_places_t *os64_browser_places(const os64_browser_t *b)
{ return b?&b->places:NULL; }
os64_ui_status_code_t os64_browser_status(const os64_browser_t *b,int64_t *io_error)
{ if (io_error) *io_error=b?b->io_error:0; return b?b->status:OS64_UI_INVALID; }
uint64_t os64_browser_revision(const os64_browser_t *b) { return b?b->revision:0; }
