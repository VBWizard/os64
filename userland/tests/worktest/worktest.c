#include "os64/os64.h"
#include "os64/ui.h"
#include "fetch/fetch.h"

#define WORK_OK   0x600C0000
#define WORK_FAIL 0x600C0001
#define WORK_SKIP 0x600C0002
#define LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#define CHECK(c) do { if (!(c)) { os64_printf("worktest: FAIL line %d: %s\n",__LINE__,#c); ++failures; } } while (0)
static unsigned failures;
static bool finished;
static int64_t watchdog(void *arg)
{
    (void)arg;
    for (unsigned i=0;i<450;++i) {
        if (LOAD(&finished)) return 0;
        os64_sleep(100);
    }
    os64_printf("worktest: FAIL watchdog expired\n");
    os64_exit(WORK_FAIL);
}

typedef struct { unsigned started, released, bytes; bool gate; } control_t;
typedef struct { control_t *control; unsigned number; bool wait; char url[256]; } job_t;
typedef struct { uint32_t crc; size_t length; } product_t;

static uint64_t now(void)
{ os64_ticks_t t; os64_ticks(&t); return t.ticks*1000/t.per_second; }
static bool await(unsigned *p, unsigned n)
{
    uint64_t end=now()+10000;
    while (LOAD(p)<n && now()<end) os64_sleep(10);
    CHECK(LOAD(p)>=n); return LOAD(p)>=n;
}
static void release(void *v, void *out)
{
    job_t *j=v; control_t *c=j->control;
    os64_free(out); os64_free(j);
    __atomic_add_fetch(&c->released,1,__ATOMIC_RELEASE);
}
static int64_t run(void *v, bool (*cancel)(void *), void *ctx, void **out)
{
    job_t *j=v; STORE(&j->control->started,1);
    product_t *p=os64_calloc(1,sizeof(*p)); *out=p;
    if (!p) return -1;
    if (j->url[0]) {
        os64_fetch_options_t opt={.max_body=2*1024*1024,.idle_ms=2000,
            .no_proxy=true,.cancelled=cancel,.ctx=ctx};
        os64_fetch_t *f=os64_fetch_open(j->url,&opt);
        if (!f) return -1;
        uint32_t crc=os64_crc32_begin(); char buf[4096]; int64_t n;
        while ((n=os64_fetch_read(f,buf,sizeof(buf)))>0) {
            crc=os64_crc32_update(crc,buf,(size_t)n); p->length+=(size_t)n;
            STORE(&j->control->bytes,(unsigned)p->length);
        }
        int64_t status=os64_fetch_status(f);
        p->crc=os64_crc32_end(crc);
        os64_fetch_close(f);
        return n<0 || status!=OS64_FETCH_OK ? -1 : (int64_t)j->number;
    }
    while (j->wait && !LOAD(&j->control->gate) && !cancel(ctx)) os64_sleep(10);
    // Heap allocation and deterministic computation both happen on workers.
    p->crc=j->number*3; p->length=j->number;
    return j->number;
}
static os64_work_id_t submit(os64_work_pool_t *p, control_t *c, unsigned i,
                              size_t reserve, bool wait, const char *url)
{
    job_t *j=os64_calloc(1,sizeof(*j)); CHECK(j!=NULL); if (!j) return 0;
    j->control=c; j->number=i; j->wait=wait;
    if (url) os64_snprintf(j->url,sizeof(j->url),"%s",url);
    os64_work_t w={run,release,j,reserve};
    os64_work_id_t id=os64_work_submit(p,&w);
    if (!id) os64_free(j); // Refusal retained caller ownership.
    return id;
}
static uint64_t heap_live(void)
{
    int64_t fd=os64_open("/proc/self/heap","r"); CHECK(fd>=0);
    char line[128]; uint64_t live=UINT64_MAX;
    while (fd>=0 && os64_readline((int32_t)fd,line,sizeof(line))==1)
        if (line[0]=='l' && line[1]=='i' && line[2]=='v' && line[3]=='e' && line[4]=='\t') {
            live=0; for (char *s=line+5; *s>='0' && *s<='9'; ++s) live=live*10+(unsigned)(*s-'0');
        }
    if (fd>=0) os64_close((int32_t)fd);
    CHECK(live!=UINT64_MAX); return live;
}
static void collect(os64_work_pool_t *p, os64_work_id_t expected, unsigned number)
{
    uint64_t end=now()+10000; os64_work_id_t id; int64_t verdict; void *j,*out;
    do {
        if (os64_work_reap(p,&id,&verdict,&j,&out)) {
            CHECK(id==expected && verdict==(int64_t)number);
            CHECK(out && ((product_t*)out)->crc==number*3);
            release(j,out); return;
        }
        os64_sleep(10);
    } while (now()<end);
    CHECK(false);
}
static void synthetic(int64_t win)
{
    // Exercise the user/kernel deadline boundary before testing admission.
    int32_t pipe[2]; CHECK(os64_pipe(pipe)==0); char byte=1, answer=0;
    CHECK(os64_read_for(pipe[0],&answer,1,0)==OS64_ERR_TIMEOUT);
    uint64_t start=now();
    CHECK(os64_read_for(pipe[0],&answer,1,40)==OS64_ERR_TIMEOUT);
    CHECK(now()-start>=40 && now()-start<2000);
    CHECK(os64_write(pipe[1],&byte,1)==1);
    CHECK(os64_read_for(pipe[0],&answer,1,0)==1 && answer==byte);
    os64_close(pipe[1]); CHECK(os64_read_for(pipe[0],&answer,1,0)==0);
    os64_close(pipe[0]);
    uint64_t before=heap_live();
    os64_work_pool_t *p=os64_work_pool_create(4,4,win,4); CHECK(p!=NULL); if (!p) return;
    control_t c[20]={0}; os64_work_id_t ids[20];
    for (unsigned i=0;i<20;++i) { ids[i]=submit(p,&c[i],i,1,true,NULL); CHECK(ids[i]); }
    for (unsigned i=0;i<4;++i) await(&c[i].started,1);
    for (int i=3;i>=0;--i) { STORE(&c[i].gate,true); collect(p,ids[i],(unsigned)i); }
    for (unsigned i=4;i<20;++i) { await(&c[i].started,1); STORE(&c[i].gate,true); collect(p,ids[i],i); }
    os64_work_pool_destroy(p);
    for (unsigned i=0;i<20;++i) CHECK(c[i].released==1);
    CHECK(heap_live()==before);
    os64_printf("worktest: twenty jobs, four workers, ordered products and heap checked\n");

    control_t cap[3]={0}; p=os64_work_pool_create(3,10,win,4); CHECK(p!=NULL); if (!p) return;
    os64_work_id_t a=submit(p,&cap[0],0,10,false,NULL);
    await(&cap[0].started,1); os64_sleep(100);
    os64_work_id_t b=submit(p,&cap[1],1,10,false,NULL), d=submit(p,&cap[2],2,1,false,NULL);
    os64_sleep(250); CHECK(!LOAD(&cap[1].started) && !LOAD(&cap[2].started));
    os64_work_cancel(p,a); await(&cap[0].released,1); await(&cap[1].started,1);
    os64_sleep(150); CHECK(!LOAD(&cap[2].started));
    collect(p,b,1); collect(p,d,2); os64_work_pool_destroy(p);
    CHECK(heap_live()==before);

    control_t full[OS64_WORK_MAX_JOBS+1]={0};
    p=os64_work_pool_create(1,1,win,4); CHECK(p!=NULL); if (!p) return;
    CHECK(submit(p,&full[0],0,1,true,NULL)); await(&full[0].started,1);
    for (unsigned i=1;i<OS64_WORK_MAX_JOBS;++i) {
        os64_work_id_t id=submit(p,&full[i],i,1,false,NULL); CHECK(id); os64_work_cancel(p,id);
    }
    CHECK(!submit(p,&full[OS64_WORK_MAX_JOBS],0,1,false,NULL));
    os64_work_pool_destroy(p);
    for (unsigned i=0;i<OS64_WORK_MAX_JOBS;++i) CHECK(full[i].released==1 && full[i].started==(i==0));
    CHECK(heap_live()==before); CHECK(os64_heap_verify()==0);
    os64_printf("worktest: cap through DONE, FIFO, queued/running cancellation, full table, destroy checked\n");
}

typedef struct {
    os64_work_pool_t *pool;
    unsigned reaped, seen, cancelled, frames;
} face_t;
static face_t *face; // UI-thread state; workers carry their own job contexts.
static void arrivals(os64_ui_t *ui, const os64_gui_event_t *event)
{
    (void)ui; if (!(event->doorbell.mask&4)) return;
    os64_work_id_t id; int64_t verdict; void *v,*out;
    while (os64_work_reap(face->pool,&id,&verdict,&v,&out)) {
        job_t *j=v; product_t *p=out; (void)id;
        CHECK(verdict==(int64_t)j->number && p);
        if (p) {
            // httptestd.py serves HELLO via gzip/redirect and the first
            // 200000 seeded BIG bytes via gzip-chunked.
            static const char hello[]="hello from the host, over HTTP.\n";
            uint32_t expected=os64_crc32_end(os64_crc32_update(os64_crc32_begin(),hello,sizeof(hello)-1));
            size_t length=sizeof(hello)-1;
            if (j->number%4==3) { expected=0xe26ffa7du; length=200000; }
            CHECK(p->crc==expected && p->length==length);
        }
        CHECK(!(face->seen&(1u<<j->number))); face->seen|=1u<<j->number;
        ++face->reaped; release(v,out);
    }
}
static void paint(os64_draw_ctx_t *ctx, face_t *f)
{
    os64_draw_fill_rect(&ctx->surf,(os64_gui_rect_t){0,0,480,210},0x172438);
    os64_draw_ctx_colors(ctx,0xeeeeee,0x172438); os64_draw_ctx_pen(ctx,18,18);
    os64_draw_ctx_text(ctx,"Background work / the face stays alive");
    char line[80]; os64_snprintf(line,sizeof(line),"%u CRC-checked arrivals   %u cancelled",f->reaped,f->cancelled);
    os64_draw_ctx_pen(ctx,18,48); os64_draw_ctx_text(ctx,line);
    for (unsigned i=0;i<20;++i)
        os64_draw_fill_rect(&ctx->surf,(os64_gui_rect_t){18+(int32_t)(i%10)*43,84+(int32_t)(i/10)*36,34,26},
            f->seen&(1u<<i)?0x47bd95:i==0&&f->cancelled?0xd5a14c:0x425269);
    os64_draw_fill_rect(&ctx->surf,(os64_gui_rect_t){18+(int32_t)(f->frames%42)*10,170,10,8},0x70b6ff);
    os64_draw_publish(ctx,NULL); ++f->frames;
}
static void fetching(int64_t win, const char *base, bool cancel_one)
{
    control_t c[20]={0}; os64_work_id_t ids[20]; uint64_t before=heap_live();
    face_t f={0}; face=&f; f.pool=os64_work_pool_create(4,4*1024*1024,win,4);
    CHECK(f.pool!=NULL); if (!f.pool) return;
    os64_draw_ctx_t draw; CHECK(os64_draw_ctx_init(&draw,win)==0);
    os64_ui_t ui; os64_ui_init(&ui,&draw); ui.on_doorbell=arrivals;
    const char *routes[]={"hello.txt","gzipped","redirect","gzip-chunked"};
    for (unsigned i=0;i<20;++i) {
        char url[256]; os64_snprintf(url,sizeof(url),"%s/%s",base,cancel_one&&i==0?"slow.txt":routes[i%4]);
        ids[i]=submit(f.pool,&c[i],i,1024*1024,false,url); CHECK(ids[i]);
    }
    os64_frame_clock_t clock; os64_frame_clock_init(&clock);
    // Leave the clock unbound so this automated fixture can time out even
    // when VT1 covers it; libui still dispatches the window's real events.
    uint64_t end=now()+30000;
    while (f.reaped+f.cancelled<20 && now()<end && !ui.quit) {
        os64_gui_event_t event;
        while (os64_gui_event_poll(win,&event)==1) os64_ui_dispatch(&ui,&event);
        if (cancel_one && !f.cancelled && LOAD(&c[0].bytes)>0) {
            os64_work_cancel(f.pool,ids[0]); f.cancelled=1;
        }
        paint(&draw,&f); os64_frame_wait(&clock,20);
        CHECK(!os64_work_pool_error(f.pool));
        if (os64_work_pool_error(f.pool)) break;
    }
    CHECK(f.reaped==(cancel_one?19u:20u));
    os64_work_pool_destroy(f.pool);
    for (unsigned i=0;i<20;++i) CHECK(c[i].released==1);
    CHECK(heap_live()==before); CHECK(os64_heap_verify()==0);
    os64_printf("worktest: fetch %u whole, %u cancelled, %u frames, heap restored\n",f.reaped,f.cancelled,f.frames);
    paint(&draw,&f); os64_sleep(5000); face=NULL;
}
int main(int argc, char **argv)
{
    int64_t win=os64_gui_window_create("Work pool",120,110,500,250,0);
    if (win==OS64_GUI_ERR_NOT_RUNNING) { os64_printf("worktest: SKIP (requires GUI)\n"); return WORK_SKIP; }
    CHECK(win>0); if (win<=0) return WORK_FAIL;
    int64_t guard=os64_thread(watchdog,NULL); CHECK(guard>=0);
    if (argc>1) fetching(win,argv[1],argc>2 && os64_streq(argv[2],"--cancel"));
    else synthetic(win);
    os64_gui_window_destroy(win);
    STORE(&finished,true);
    if (guard>=0) { os64_thread_join((int32_t)guard,NULL); os64_close((int32_t)guard); }
    os64_printf("worktest: %s (%u failures)\n",failures?"FAIL":"PASS",failures);
    return failures?WORK_FAIL:WORK_OK;
}
