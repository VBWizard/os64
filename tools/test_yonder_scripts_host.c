/* Exercise the actual browser rendering/control/queue code. Only OS services
 * and the font backend are hosted; libui caret/edit code is linked for real. */
#define main js_port_fixture_main
#include "test_js_port_host.c"
#undef main
#define main yonder_fixture_main
#include "../userland/apps/yonder/yonder.c"
#undef main
#include "test_libflow_fonts.h"
#include "os64/conf.h"
#define s settings_fixture
#include "../userland/apps/yonder/settings.c"
#undef s

int64_t os64_open(const char *p, const char *m) { (void)p; (void)m; return -1; }
int64_t os64_read(int32_t h, void *b, size_t n) { (void)h; (void)b; (void)n; return -1; }
int64_t os64_close(int32_t h) { (void)h; return 0; }
int64_t os64_stat(const char *p, os64_dirent_t *entry) { (void)p; (void)entry; return -1; }
/* The clocks stand still unless a case moves them: micros steps by
 * clock_step at every read (an overrun is a loop that reads it), and the
 * window's milliseconds are what a case sets. */
static int64_t clock_us=10000000, clock_step;
int64_t os64_micros(void) { return clock_us+=clock_step; }
uint64_t os64_heap_verify(void) { return 0; }
static char last_debug[256];
void os64_debug_log(const char *text) { snprintf(last_debug,sizeof(last_debug),"%s",text); }
void *os64_calloc(size_t n, size_t z) {
    if (z && n > SIZE_MAX/z) return NULL;
    void *p = os64_malloc(n*z);
    if (p) memset(p,0,n*z);
    return p;
}
const os64_font_backend_t *os64_freetype_backend_v1(void) { return flow_test_backend(); }
void os64_ui_theme_current(os64_ui_theme_t *theme, uint64_t *installed) {
    os64_ui_theme_defaults(theme); *installed=0;
}
int64_t os64_ticks(os64_ticks_t *ticks) { *ticks=(os64_ticks_t){.ticks=100,.per_second=1000}; return 0; }
static uint64_t now_ms=100;
uint64_t yonder_now_ms(void) { return now_ms; }
void yonder_ticker_set(yonder_ticker_t *ticker, uint64_t due) { (void)ticker; (void)due; }
/* THE POOL, FAKED. A stream case (below) lets one navigation be submitted
 * and plays its worker itself; every other case still treats a submission
 * as a mistake. */
static bool pool_open;
static os64_work_t pool_work;
static os64_work_id_t pool_ids, pool_cancelled;
static int64_t pool_error;
static int pool_fake;
/* A cancelled job is the pool's to release (work.h), and the window never
 * sees it again. */
/* A script's fetch job is kept until the case LANDS it (script_land) or the
 * window cancels it; the pool's own release then frees it. */
static struct { os64_work_id_t id; yonder_script_job_t *job; } script_jobs[32];
static int nscript_jobs;
static unsigned script_cancels;
/* A sheet's fetch job is held the same way, never run: a case that wants a
 * page waiting for its sheets gets one that waits until it is cancelled or
 * the window drops it. */
static struct { os64_work_id_t id; void *job; } sheet_jobs[32];
static int nsheet_jobs;
void os64_work_cancel(os64_work_pool_t *pool, os64_work_id_t id) {
    (void)pool;
    for(int i=0;i<nscript_jobs;i++) if(script_jobs[i].id==id) {
        os64_free(script_jobs[i].job); script_jobs[i]=script_jobs[--nscript_jobs]; script_cancels++; return;
    }
    for(int i=0;i<nsheet_jobs;i++) if(sheet_jobs[i].id==id) {
        os64_free(sheet_jobs[i].job); sheet_jobs[i]=sheet_jobs[--nsheet_jobs]; return;
    }
    pool_cancelled=id;
    if(id==pool_ids && pool_work.job!=NULL) {
        pool_work.release(pool_work.job,NULL); memset(&pool_work,0,sizeof(pool_work));
    }
}
os64_work_id_t os64_work_submit(os64_work_pool_t *pool, const os64_work_t *work) {
    (void)pool;
    if(!pool_open) { check(false,"unexpected background submission"); return 0; }
    if(*(const uint32_t *)work->job==YONDER_JOB_SCRIPT) {
        if(nscript_jobs==32) return 0;
        script_jobs[nscript_jobs].id=++pool_ids; script_jobs[nscript_jobs].job=work->job;
        return script_jobs[nscript_jobs++].id;
    }
    if(*(const uint32_t *)work->job==YONDER_JOB_SHEET) {
        if(nsheet_jobs==32) return 0;
        sheet_jobs[nsheet_jobs].id=++pool_ids; sheet_jobs[nsheet_jobs].job=work->job;
        return sheet_jobs[nsheet_jobs++].id;
    }
    pool_work=*work; return ++pool_ids;
}
bool os64_work_reap(os64_work_pool_t *pool, os64_work_id_t *id, int64_t *verdict, void **job,
                    void **product) {
    (void)pool; (void)id; (void)verdict; (void)job; (void)product; return false;
}
int64_t os64_work_pool_error(os64_work_pool_t *pool) { (void)pool; return pool_error; }
void os64_work_pool_destroy(os64_work_pool_t *pool) { (void)pool; }
/* The trip's release, as trip.c spells it: the job's request, its mailbox
 * reference, the job. trip.c itself is not linked (its run is a fetch). */
int64_t yonder_trip_run(void *job, bool (*cancelled)(void *), void *ctx, void **out) {
    (void)job; (void)cancelled; (void)ctx; (void)out;
    check(false,"unexpected worker execution"); return -1;
}
void yonder_trip_release(void *job, void *product) {
    (void)product;
    yonder_trip_t *trip=job;
    if(trip==NULL) return;
    os64_page_request_free(&trip->request);
    yonder_mail_drop(trip->mail);
    os64_free(trip);
}
/* THE MAILBOX, FAKED, single-threaded: the ring's own two-thread proof is
 * tools/test_yonder_stream_host.sh; here the test is the worker and posts
 * straight into what the window takes from, in order. */
typedef struct mail_chunk { struct mail_chunk *next; size_t len; uint8_t bytes[]; } mail_chunk_t;
struct yonder_mail {
    uint32_t refs; uint64_t generation;
    char progress[512]; bool progress_new;
    uint32_t asked; char question[512]; bool question_new;
    uint32_t answered; bool answered_yes; unsigned answers;
    way_head_t head; bool head_new;
    yonder_verdict_t verdict; bool verdict_new;
    mail_chunk_t *first, *last;
};
yonder_mail_t *yonder_mail_new(uint64_t generation) {
    yonder_mail_t *m=os64_calloc(1,sizeof(*m));
    if(m) { m->refs=1; m->generation=generation; }
    return m;
}
void yonder_mail_hold(yonder_mail_t *m) { m->refs++; }
void yonder_mail_drop(yonder_mail_t *m) {
    if(m==NULL || --m->refs!=0) return;
    while(m->first) { mail_chunk_t *c=m->first; m->first=c->next; os64_free(c); }
    os64_free(m);
}
uint64_t yonder_mail_generation(const yonder_mail_t *m) { return m->generation; }
void yonder_mail_progress(yonder_mail_t *m, const char *s) {
    os64_strcopy(m->progress,sizeof(m->progress),s); m->progress_new=true;
}
uint32_t yonder_mail_ask(yonder_mail_t *m, const char *q) {
    os64_strcopy(m->question,sizeof(m->question),q); m->question_new=true; return ++m->asked;
}
bool yonder_mail_wait(yonder_mail_t *m, uint32_t n, bool (*c)(void *), void *x) {
    (void)m; (void)n; (void)c; (void)x; check(false,"the window never waits"); return false;
}
bool yonder_mail_take_progress(yonder_mail_t *m, char *out, size_t cap) {
    bool fresh=m->progress_new; if(fresh) os64_strcopy(out,cap,m->progress);
    m->progress_new=false; return fresh;
}
bool yonder_mail_take_question(yonder_mail_t *m, uint32_t *n, char *out, size_t cap) {
    bool fresh=m->question_new;
    if(fresh) { os64_strcopy(out,cap,m->question); *n=m->asked; }
    m->question_new=false; return fresh;
}
void yonder_mail_answer(yonder_mail_t *m, uint32_t number, bool yes) {
    m->answered=number; m->answered_yes=yes; m->answers++;
}
void yonder_mail_post_head(yonder_mail_t *m, const way_head_t *h) { m->head=*h; m->head_new=true; }
bool yonder_mail_post(yonder_mail_t *m, const void *bytes, size_t len, bool (*c)(void *), void *x) {
    (void)c; (void)x;
    while(len>0) {
        size_t take=len<YONDER_STREAM_CHUNK ? len : YONDER_STREAM_CHUNK;
        mail_chunk_t *k=os64_malloc(sizeof(*k)+take);
        k->next=NULL; k->len=take; memcpy(k->bytes,bytes,take);
        if(m->last) m->last->next=k; else m->first=k;
        m->last=k; bytes=(const uint8_t *)bytes+take; len-=take;
    }
    return true;
}
void yonder_mail_post_verdict(yonder_mail_t *m, const yonder_verdict_t *v) { m->verdict=*v; m->verdict_new=true; }
bool yonder_mail_take_head(yonder_mail_t *m, way_head_t *out) {
    bool fresh=m->head_new; if(fresh) *out=m->head; m->head_new=false; return fresh;
}
size_t yonder_mail_take(yonder_mail_t *m, void *buf, size_t cap) {
    if(cap<YONDER_STREAM_CHUNK || m->first==NULL) return 0;
    mail_chunk_t *k=m->first; m->first=k->next; if(m->first==NULL) m->last=NULL;
    size_t len=k->len; memcpy(buf,k->bytes,len); os64_free(k); return len;
}
bool yonder_mail_take_verdict(yonder_mail_t *m, yonder_verdict_t *out) {
    bool fresh=m->verdict_new; if(fresh) *out=m->verdict; m->verdict_new=false; return fresh;
}
bool yonder_mail_streaming(yonder_mail_t *m) { return m->head_new || m->first!=NULL || m->verdict_new; }
#define JOB_STUB(kind) \
int64_t yonder_##kind##_run(void *job, bool (*cancelled)(void *), void *ctx, void **out) { \
    (void)job; (void)cancelled; (void)ctx; (void)out; \
    check(false,"unexpected worker execution"); return -1; \
} \
void yonder_##kind##_release(void *job, void *product) { \
    (void)job; (void)product; check(false,"unexpected worker product"); \
}
JOB_STUB(picture)
JOB_STUB(sheet)
int64_t yonder_script_run(void *job, bool (*cancelled)(void *), void *ctx, void **out) {
    (void)job; (void)cancelled; (void)ctx; (void)out;
    check(false,"unexpected worker execution"); return -1;
}
void yonder_script_release(void *job, void *product) {
    yonder_script_t *got=product;
    if(got) { os64_free(got->source); os64_free(got); }
    os64_free(job);
}
/* The worker's answer to the script job whose address ends with `tail`:
 * its source, or NULL for a fetch that failed. False when no such job is
 * out. */
static bool script_land(const char *tail, const char *source) {
    for(int i=0;i<nscript_jobs;i++) {
        const char *url=script_jobs[i].job->url;
        size_t n=strlen(url), t=strlen(tail);
        if(n<t || strcmp(url+n-t,tail)!=0) continue;
        os64_work_id_t id=script_jobs[i].id; yonder_script_job_t *job=script_jobs[i].job;
        script_jobs[i]=script_jobs[--nscript_jobs];
        yonder_script_t *got=os64_calloc(1,sizeof(*got));
        if(source) {
            got->ok=true; got->length=strlen(source);
            got->source=os64_malloc(got->length+1); memcpy(got->source,source,got->length+1);
        }
        os64_strcopy(got->url,sizeof(got->url),url);
        reaped(id,job,got);
        return true;
    }
    return false;
}
/* Reached from the doorbell's other bits, which no case rings. */
int64_t os64_thread_join(int32_t handle, int64_t *retval) { (void)handle; (void)retval; return -1; }
void os64_ui_settings_pump(os64_ui_settings_t *dialog) { (void)dialog; check(false,"no settings relay"); }
os64_image_status_t os64_image_sequence_next(os64_image_sequence_t *seq) {
    (void)seq; check(false,"no moving picture"); return OS64_IMAGE_OK;
}
void os64_image_free(os64_image_t *image) { os64_free(image->pixels); memset(image,0,sizeof(*image)); }
void os64_image_sequence_free(os64_image_sequence_t *sequence) { check(sequence==NULL,"no unexpected moving image"); }
const os64_image_frame_t *os64_image_sequence_frame(const os64_image_sequence_t *sequence) {
    (void)sequence; check(false,"unexpected animation read"); return NULL;
}
/* A file a case serves (open_local), or none. */
static const char *slurp_path, *slurp_bytes;
os64_slurp_status_t os64_slurp(const char *path, size_t cap, uint8_t **bytes, size_t *length) {
    (void)cap; *bytes=NULL; *length=0;
    if(slurp_path==NULL || !os64_streq(path,slurp_path)) return OS64_SLURP_NO_FILE;
    *length=strlen(slurp_bytes); *bytes=os64_malloc(*length+1); memcpy(*bytes,slurp_bytes,*length+1);
    return OS64_SLURP_OK;
}

bool os64_ui_theme_session(os64_ui_theme_t *theme, uint64_t *installed, uint64_t hint) {
    (void)theme; (void)installed; (void)hint; return false;
}
static char saved_scripts[8], saved_zoom[8], saved_seconds[8], settings_report[256];
int64_t os64_conf_get(const char *file, const char *key, char *out, size_t cap) {
    check(os64_streq(file,"yonder.conf"),"settings read Yonder configuration");
    const char *value=os64_streq(key,"scripts") ? saved_scripts :
        os64_streq(key,"zoom") ? saved_zoom : os64_streq(key,"script_seconds") ? saved_seconds : "";
    if(!value[0]) return OS64_CONF_NO_KEY;
    os64_strcopy(out,cap,value); return 0;
}
int64_t os64_conf_set(const char *file, const char *key, const char *value) {
    check(os64_streq(file,"yonder.conf"),"settings save Yonder configuration");
    if(os64_streq(key,"scripts")) os64_strcopy(saved_scripts,sizeof(saved_scripts),value);
    if(os64_streq(key,"zoom")) os64_strcopy(saved_zoom,sizeof(saved_zoom),value);
    if(os64_streq(key,"script_seconds")) os64_strcopy(saved_seconds,sizeof(saved_seconds),value);
    return 0;
}
void os64_ui_settings_report(os64_ui_settings_t *dialog, const char *text) {
    (void)dialog; os64_strcopy(settings_report,sizeof(settings_report),text);
}
void way_cache_enable(way_cache_t *cache, bool enabled) { (void)enabled; check(cache==NULL,"no test cache"); }
void way_cache_set_cap(way_cache_t *cache, uint64_t cap) { (void)cap; check(cache==NULL,"no test cache"); }

static os64_text_context_t *probe_text;
static os64_text_font_t *probe_font;
static bool probe_borrow_font;
static os64_font_family_cache_t *probe_family;
static os64_font_engine_t *budget_engine;
static unsigned budget_refusals, budget_partial_layouts, budget_retry_refusals;
static bool budget_active, budget_refuse_retry;
static os64_font_status_t budget_engine_create(const os64_font_engine_options_t *options,
    os64_font_engine_t **out) {
    os64_font_status_t status=flow_test_backend()->engine_create(options,out);
    if(status==OS64_FONT_OK) budget_engine=*out;
    return status;
}
static os64_font_status_t budget_face_open(os64_font_engine_t *engine,
    const uint8_t *bytes, size_t length, const os64_font_face_options_t *options,
    os64_font_face_t **out) {
    os64_font_engine_stats_t stats;
    os64_font_status_t status=flow_test_backend()->engine_stats(engine,&stats);
    if(status!=OS64_FONT_OK) return status;
    if(stats.live_faces>=3) {
        *out=NULL;budget_refusals++;
        return OS64_FONT_LIMIT;
    }
    return flow_test_backend()->face_open(engine,bytes,length,options,out);
}
flow_tree_t *__real_flow_layout(const os64_html_document_t *,const os64_page_t *,int32_t,
    const flow_env_t *);
flow_tree_t *__wrap_flow_layout(const os64_html_document_t *doc,const os64_page_t *model,
    int32_t width,const flow_env_t *env) {
    // A refusal after reclaiming the old geometry exercises the retry's
    // teardown and recovery, rather than the usual preserve-old-tree path.
    if(budget_active && budget_refuse_retry && g.page.tree==NULL) {
        budget_refuse_retry=false;budget_retry_refusals++;
        return NULL;
    }
    flow_tree_t *tree=__real_flow_layout(doc,model,width,env);
    if(budget_active && flow_incomplete(tree)) budget_partial_layouts++;
    return tree;
}
static void *probe_alloc(void *ctx, size_t n) { (void)ctx; return os64_malloc(n); }
static void probe_free(void *ctx, void *p, size_t n) { (void)ctx; (void)n; os64_free(p); }
static os64_font_status_t probe_fonts(void *ctx, const flow_family_list_t *family,
    bool bold, bool italic, uint32_t px, os64_text_font_t *const **fonts, size_t *n,
    os64_font_face_info_t *info) {
    (void)ctx; (void)family; (void)bold; (void)italic; (void)px;
    if(probe_family!=NULL) {
        os64_font_family_list_t list={.generic=OS64_FONT_FAMILY_SERIF};
        os64_font_role_view_t view;
        os64_font_status_t status=os64_font_family_open(probe_family,&list,bold,italic,px,&view);
        if(status!=OS64_FONT_OK) return status;
        *fonts=view.fonts;*n=view.font_count;*info=view.primary;
        return OS64_FONT_OK;
    }
    *fonts = &probe_font; *n=1; memset(info,0,sizeof(*info));
    info->ascent=12*64; info->descent=4*64; info->line_height=20*64;
    return OS64_FONT_OK;
}
static void probe_page(const char *html, bool scripts) {
    os64_memset(&g,0,sizeof(g));
    g.scripts_on=scripts; g.way.agent=YONDER_AGENT; g.settle_due=YONDER_NEVER;
    g.zoom=g.zoom_default=1000;
    os64_ui_init(&g.ui,NULL);
    if(probe_borrow_font)
        check(os64_ui_font_borrow_context(&g.ui,probe_text)==OS64_FONT_OK,
            "zoom fixture borrows real font context");
    os64_ui_panel(&g.root); os64_ui_panel(&g.view); g.view.focusable=true; os64_ui_label(&g.status,g.status_text);
    os64_ui_add_child(&g.root,&g.view); os64_ui_add_child(&g.root,&g.status);
    os64_ui_set_root(&g.ui,&g.root);
    g.view.bounds=(os64_gui_rect_t){0,0,800,600};
    g.page.way.doc=parse_file((const uint8_t *)html,strlen(html),scripts);
    g.page.scripting=scripts;
    os64_strcopy(g.page.way.url,sizeof(g.page.way.url),"https://fixture.test/page");
    g.page.way.model=os64_page_build(g.page.way.doc,g.page.way.url,NULL,NULL);
    check(g.page.way.model!=NULL,"fixture model builds");
    s_env.text=probe_text; s_env.fonts=probe_fonts; s_env.replaced_size=replaced_size;
    sheets_start(&g.page);
    check(page_lay_out(&g.page,800,600,g.zoom,&g.page),"fixture layout builds");
    g.page.model_version=g.page.rendered_version=os64_html_version(g.page.way.doc);
    g.page.state_version=os64_page_state_version(os64_page_shared_state(g.page.way.model));
    forms_build();
    g.script_ms=5000;
    if(scripts) {
        /* A finished document's inline scripts are queued as CONNECTED
         * scripts, in tree order: the shown page's ready list, one a turn. */
        g.page.scripts=scripts_host(g.page.way.doc,os64_page_shared_state(g.page.way.model),
            g.page.way.url,++g.pages_made);
        for (os64_html_node_t *n=g.page.way.doc->document;n;n=(os64_html_node_t *)next_within(n,g.page.way.doc->document))
            if(os64_dom_script_kind(n)==OS64_DOM_SCRIPT_CLASSIC && os64_html_attr(n,"src")==NULL)
                yonder_scripts_connected(g.page.scripts,n);
    }
}
static void probe_drop(void) {
    inputs_drop();
    yonder_scripts_free(g.page.scripts); g.page.scripts=NULL;
    forms_drop(); page_clear(&g.page); os64_ui_font_release(&g.ui);
}

static os64_html_node_t *probe_id(const char *id) {
    for (os64_html_node_t *n=g.page.way.doc->document;n;n=(os64_html_node_t *)next_within(n,g.page.way.doc->document)) {
        const os64_html_attr_t *a=os64_html_attr(n,"id");
        if(a && os64_streq(a->value,id)) return n;
    }
    return NULL;
}
static FormWidget *probe_field(const char *id) {
    return form_widget(os64_page_control_for(page_model(&g.page),probe_id(id)));
}
static bool probe_text_is(const char *id, const char *text) {
    os64_html_node_t *n=probe_id(id);
    return n && n->first_child && n->first_child->kind==OS64_HTML_TEXT &&
        os64_streq(n->first_child->text,text);
}
static void script_type_case(const char *attributes, bool runs) {
    char html[512];
    os64_snprintf(html,sizeof(html),"<p id=result>before</p><script %s>"
        "document.getElementById('result').textContent='ran';</script>",attributes);
    unsigned before=failures;
    probe_page(html,true);
    check(yonder_scripts_pending(g.page.scripts)==runs,"script type decides queue qualification");
    script_turn();
    check(probe_text_is("result",runs ? "ran" : "before"),"qualified script executes; other languages stay inert");
    check(!yonder_scripts_pending(g.page.scripts),"script type fixture consumes its schedule");
    probe_drop();
    if(failures!=before) fprintf(stderr,"script type attributes: %s\n",attributes);
}
static void script_types(void) {
    // Expected types come from MIME Sniffing's JavaScript essence list.
    static const char *const accepted[]={
        "application/ecmascript","application/javascript","application/x-ecmascript","application/x-javascript",
        "text/ecmascript","text/javascript","text/javascript1.0","text/javascript1.1","text/javascript1.2",
        "text/javascript1.3","text/javascript1.4","text/javascript1.5","text/jscript","text/livescript",
        "text/x-ecmascript","text/x-javascript"
    };
    for(size_t i=0;i<sizeof(accepted)/sizeof(accepted[0]);i++) {
        char attribute[128],upper[64];
        size_t n=strlen(accepted[i]);
        for(size_t j=0;j<=n;j++) {
            char c=accepted[i][j];
            upper[j]=c>='a' && c<='z' ? c+'A'-'a' : c;
        }
        os64_snprintf(attribute,sizeof(attribute),"type='%s'",accepted[i]);
        script_type_case(attribute,true);
        os64_snprintf(attribute,sizeof(attribute),"type='%s'",upper);
        script_type_case(attribute,true);
        os64_snprintf(attribute,sizeof(attribute),"type=' \t\n\f\r%s\r\f\n\t '",upper);
        script_type_case(attribute,true);
        if(strncmp(accepted[i],"text/",5)==0) {
            os64_snprintf(attribute,sizeof(attribute),"language='%s'",accepted[i]+5);
            script_type_case(attribute,true);
            os64_snprintf(attribute,sizeof(attribute),"language='%s'",upper+5);
            script_type_case(attribute,true);
        }
    }
    static const char *const defaults[]={"","type=''","language=''","type='' language=VBScript",
        "type='text/JavaScript' language=VBScript","type=' application/x-javascript ' language=VBScript"};
    for(size_t i=0;i<sizeof(defaults)/sizeof(defaults[0]);i++) script_type_case(defaults[i],true);
    static const char *const rejected[]={
        "type=' \t\n\f\r '","type=' \t ' language=JavaScript","type=module language=JavaScript",
        "type='text/javascript; charset=utf-8'","type='application/javascript;charset=utf-8'",
        "type=javascript","type=text/javascript1.6","type='text/ javascript'",
        "type='\vtext/javascript'","type='&#160;text/javascript&#160;'","type='text/java&#383;cript'",
        "language=VBScript","language=' JavaScript '","language='\tJavaScript'","language='text/javascript'",
        "language='application/javascript'","src='' type='text/JavaScript'"
    };
    for(size_t i=0;i<sizeof(rejected)/sizeof(rejected[0]);i++) script_type_case(rejected[i],false);

    probe_page("<p id=result>before</p><script>document.getElementById('next').setAttribute('language','VBScript');</script>"
        "<script id=next language=JavaScript>document.getElementById('result').textContent='ran';</script>",true);
    script_turn();script_turn();
    check(probe_text_is("result","before"),"queued script rechecks changed language before execution");
    probe_drop();
}
static void identity_and_edit(void) {
    probe_page("<h1 id=heading>before</h1><input id=field value=start>"
        "<script>var held=document.getElementById('field');"
        "document.getElementById('heading').textContent='first';</script>"
        "<script>if(held!==document.getElementById('field')||held.value!=='startQ')"
        "throw Error('lost edit/reference'); var added=document.createElement('input');"
        "added.id='added';document.body.insertBefore(added,held);"
        "Promise.resolve().then(()=>document.getElementById('heading').textContent='second');</script>",true);
    script_turn();
    check(probe_text_is("heading","first") && yonder_scripts_pending(g.page.scripts),
          "one script per turn, next script remains queued");
    FormWidget *field=probe_field("field");
    os64_ui_set_focus(&g.ui,field->w);
    os64_gui_event_t key={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii='Q'}};
    os64_ui_dispatch(&g.ui,&key);
    check(os64_streq(field->text,"startQ"),"real UI key edit remains in buffer");
    field->u.field.cursor=4; field->u.field.anchor=1; field->u.field.selected=true;
    forms_flush(); script_turn();
    check(probe_text_is("heading","second"),"Promise checkpoint completes before repaint");
    check(probe_field("field")==field && g.ui.focus==field->w &&
        field->u.field.cursor==4 && field->u.field.anchor==1 && field->u.field.selected &&
        os64_streq(field->text,"startQ"),"reordered controls keep widget, focus, caret, selection and edit");
    check(probe_field("added")!=NULL && widget_control(field)==1,"new control inserted before existing index");
    check(g.page.rendered_version==os64_html_version(g.page.way.doc),"rendered tree matches mutation");
    probe_drop();
}
static void scripted_zoom(void) {
    probe_borrow_font=true;
    os64_font_config_defaults(&s_fonts_conf);
    strcpy(s_fonts_conf.roles[OS64_FONT_CONFIG_WEB].face[0],"builtin");
    probe_page("<style>body{width:1800px}#anchor{margin-top:700px}#tail{height:1800px}"
        "@media(max-width:500px){#anchor{color:red}}</style>"
        "<div id=anchor>anchor<input id=field value=kept></div><div id=tail>end</div>",true);
    FormWidget *field=probe_field("field");
    check(field && field->w->app_face,"scripted form control uses page face");
    check(lay_out_page(&g.page,800,600) && s_controls_set!=NULL && s_controls_px==13,
        "initial layout lends controls their real font set");
    const flow_box_t *old_anchor=flow_box_for(g.page.tree,probe_id("anchor"));
    g.sy=old_anchor->rect.y+1;g.sx=200;
    int32_t old_offset=0;
    const os64_html_node_t *held_anchor=anchor_of(&old_offset);
    check(held_anchor!=NULL,"zoom fixture finds scroll anchor");
    os64_font_set_t *old_face=s_controls_set;
    flow_tree_t *old_tree=g.page.tree;
    check(os64_html_set_attr(g.page.way.doc,probe_id("anchor"),"class","changed",7)==OS64_HTML_OK,
        "native mutation owes script rebuild");
    zoom_to(2000);
    check(!lay_out_page(&g.page,800,600) && g.page.tree==old_tree && s_controls_set==old_face &&
        g.page.laid_zoom==1000,"owed rebuild refuses zoom before lending another controls face");
    check(script_rebuild() && g.page.laid_zoom==2000 && s_controls_px==26,
        "script rebuild publishes current zoom and controls face request");
    const flow_box_t *fresh_anchor=flow_box_for(g.page.tree,held_anchor);
    check(fresh_anchor && g.sy==fresh_anchor->rect.y-scale_zoom(old_offset,2000,1000) && g.sx==400,
        "script rebuild scales page scroll with zoom");
    check(probe_field("field")==field && field->w->app_face && os64_streq(field->text,"kept"),
        "zoomed script rebuild preserves control identity and value");
    check(flow_box_for(g.page.tree,probe_id("anchor"))->style->color==0xff0000,
        "script rebuild evaluates media queries in zoomed CSS viewport");
    probe_drop();
    os64_font_set_release(s_controls_set);s_controls_set=NULL;s_controls_px=0;
    memset(&s_fonts_conf,0,sizeof(s_fonts_conf));
    probe_borrow_font=false;
}
static void scripted_face_budget(void) {
    os64_text_context_t *saved_text=probe_text;
    os64_text_font_t *saved_font=probe_font;
    for(unsigned refused=0;refused<2;refused++) {
        os64_font_backend_t backend=*flow_test_backend();
        backend.engine_create=budget_engine_create;backend.face_open=budget_face_open;
        os64_text_options_t options={.memory={.alloc=probe_alloc,.free=probe_free},
            .backend=&backend,.memory_cap=8*1024*1024};
        check(os64_text_create(&options,&probe_text)==OS64_FONT_OK,"face-budget text context");
        check(os64_text_font_bitmap(probe_text,&probe_font)==OS64_FONT_OK,"face-budget bitmap font");
        const uint8_t face='S';
        os64_font_family_spec_t specs[OS64_FONT_FAMILY_COUNT]={0};
        for(size_t f=0;f<OS64_FONT_FAMILY_COUNT;f++)
            for(size_t s=0;s<OS64_FONT_FAMILY_STYLES;s++)
                specs[f].styles[s]=(os64_font_source_t){OS64_FONT_SOURCE_OUTLINE,&face,1};
        check(os64_font_family_cache_create(probe_text,specs,&probe_family,NULL,NULL)==OS64_FONT_OK,
            "face-budget uses real family cache");
        probe_borrow_font=true;
        os64_font_config_defaults(&s_fonts_conf);
        strcpy(s_fonts_conf.roles[OS64_FONT_CONFIG_WEB].face[0],"builtin");
        probe_page("<style>#one{font-size:16px}#two{font-size:20px}#three{font-size:24px}</style>"
            "<p id=one>one<input id=field value=kept></p><p id=two>two</p><p id=three>three</p>"
            "<script>document.getElementById('one').setAttribute('class','changed');</script>"
            "<script>document.getElementById('three').textContent='last';</script>",true);
        os64_font_engine_stats_t stats;
        check(flow_test_backend()->engine_stats(budget_engine,&stats)==OS64_FONT_OK &&
            stats.live_faces==3 && !flow_incomplete(g.page.tree),
            "old whole layout pins three sizes, filling face budget");
        FormWidget *field=probe_field("field");
        os64_js_outcome_t out;
        check(yonder_scripts_step(g.page.scripts,&out) && out.status==OS64_JS_OK,
            "script mutation owes a layout before zoom");
        zoom_to(2000);
        budget_refusals=budget_partial_layouts=budget_retry_refusals=0;
        budget_active=true;budget_refuse_retry=refused!=0;
        bool rebuilt=script_rebuild();
        if(refused) {
            check(!rebuilt && budget_retry_refusals==1 && g.page.tree==NULL && g.page.cascade==NULL,
                "refused reclaimed layout drops obsolete tree and cascade");
            check(g.page.rendered_version!=os64_html_version(g.page.way.doc) &&
                yonder_scripts_pending(g.page.scripts) && field->w->hidden,
                "refused retry keeps rebuild owed, queue pending and stale controls hidden");
            check(!lay_out_page(&g.page,800,600) && g.page.tree==NULL,
                "ordinary relayout cannot bypass owed DOM rebuild after reclaim");
            script_turn();
            check(probe_text_is("three","last") && !yonder_scripts_pending(g.page.scripts),
                "next UI turn recovers a tree before executing the queued script");
        } else {
            check(rebuilt,"staged font-budget rebuild succeeds");
        }
        check(budget_refusals>0 && budget_partial_layouts==1,
            "new zoom first exhausts faces retained by old geometry");
        check(g.page.tree && !flow_incomplete(g.page.tree) && g.page.laid_zoom==2000 &&
            g.page.rendered_version==os64_html_version(g.page.way.doc),
            "reclaim retry publishes whole current DOM at new zoom");
        check(probe_field("field")==field && os64_streq(field->text,"kept"),
            "face-budget retry preserves form widget and value");
        budget_active=budget_refuse_retry=false;
        probe_drop();
        os64_font_set_release(s_controls_set);s_controls_set=NULL;s_controls_px=0;
        os64_font_family_cache_destroy(probe_family);probe_family=NULL;
        os64_text_font_release(probe_font);
        check(os64_text_destroy(probe_text)==OS64_FONT_OK,"face-budget text and runs released");
    }
    probe_text=saved_text;probe_font=saved_font;probe_borrow_font=false;
    memset(&s_fonts_conf,0,sizeof(s_fonts_conf));
}
static void property_only(void) {
    probe_page("<input id=field value=before><input id=tick type=checkbox>"
        "<script>document.getElementById('field').value='property';"
        "document.getElementById('tick').checked=true;</script>",true);
    uint64_t version=os64_html_version(g.page.way.doc);
    flow_tree_t *old=g.page.tree;
    script_turn();
    check(os64_html_version(g.page.way.doc)==version && g.page.tree==old,
          "state-only changes do not rebuild HTML layout");
    check(os64_streq(probe_field("field")->text,"property") &&
        probe_field("tick")->u.check.checked,"state-only changes refresh control widgets");
    probe_drop();
}
static void script_lifecycle(void) {
    const char *page="<h1 id=heading>off</h1><noscript><input id=fallback></noscript>"
        "<script>throw Error('first failed');</script>"
        "<script src=absent.js>document.getElementById('heading').textContent='external';</script>"
        "<script type=application/json>this is data</script>"
        "<template><script>document.getElementById('heading').textContent='template';</script></template>"
        "<script>document.getElementById('heading').textContent='last';</script>";
    probe_page(page,false);
    script_turn();
    check(probe_text_is("heading","off") && probe_field("fallback"),"off runs no scripts and shows noscript fallback");
    probe_drop();
    probe_page(page,true);
    check(probe_field("fallback")==NULL,"on uses scripting noscript parse policy");
    os64_html_node_t *fallback=probe_id("heading")->next;
    check(fallback && fallback->tag==OS64_HTML_TAG_NOSCRIPT &&
        flow_box_for(g.page.tree,fallback)==NULL &&
        flow_box_for(g.page.tree,fallback->first_child)==NULL,
        "scripting mode hides raw noscript fallback text");
    script_turn();
    check(probe_text_is("heading","off") && yonder_scripts_pending(g.page.scripts) &&
          strstr(g.status_text,"first failed"),"exception reported; later classic script remains usable");
    script_turn();
    check(probe_text_is("heading","last") && !yonder_scripts_pending(g.page.scripts),
          "external, data and template scripts excluded; later inline runs");
    probe_drop();
    probe_page("<h1 id=heading>before</h1><script>var held=document.body;"
        "document.getElementById('heading').textContent='first';</script>"
        "<script>document.getElementById('heading').textContent='must not run';</script>",true);
    script_turn(); stop_trip(); script_turn();
    check(probe_text_is("heading","must not run") && g.page.scripts!=NULL,
        "the page on screen keeps running its scripts while a navigation starts (DOM.md: the old page stays live)");
    probe_drop();
    probe_page("<h1 id=heading>before</h1><script>function again(){return Promise.resolve().then(again)}again();</script>"
        "<script>document.getElementById('heading').textContent='must not run';</script>",true);
    clock_step=1000;
    script_turn();
    clock_step=0;
    check(!yonder_scripts_pending(g.page.scripts) && probe_text_is("heading","before") &&
          !yonder_scripts_alive(g.page.scripts) &&
          strstr(g.status_text,"execution stopped after 5 s (this page runs without script)"),
          "an endless Promise chain meets the task's deadline, retires the runtime and drops the queue");
    probe_drop();
}
static void native_refusals(void) {
    unsigned refused_model=0, refused_layout=0, survived=0;
    for(size_t cut=1;cut<=160;cut++) {
        probe_page("<style>h1{color:red}</style><h1 id=heading>old text</h1>"
            "<a id=old href=/old>old link</a><input id=field value=keep>",false);
        FormWidget *field=probe_field("field");
        os64_ui_set_focus(&g.ui,field->w);
        field->u.field.cursor=2;
        flow_tree_t *old_tree=g.page.tree;
        os64_html_node_t *heading=probe_id("heading");
        const char *old_text=heading->first_child->text;
        check(os64_html_set_text(g.page.way.doc,heading->first_child,"new text",8)==OS64_HTML_OK,"native mutation succeeds");
        fail_at=attempts+cut;
        bool rebuilt=script_rebuild();
        fail_at=0;
        if(!rebuilt) {
            check(g.page.tree==old_tree && os64_streq(old_text,"old text"),"failed rebuild retains usable pinned layout bytes");
            if(g.page.model_version!=os64_html_version(g.page.way.doc)) {
                refused_model++;
                os64_page_request_t request;
                check(os64_page_activate(page_model(&g.page),(os64_page_what_t){OS64_PAGE_ACTIVATE_LINK,0,0,0},
                    &request)==OS64_PAGE_REFUSED && request.reason==OS64_PAGE_REASON_STALE,
                    "stale model refuses activation after model allocation failure");
                os64_page_request_free(&request);
            } else refused_layout++;
            check(script_rebuild(),"retry completes without losing old page");
        } else survived++;
        check(probe_field("field")==field && field->u.field.cursor==2,"failure sweep preserves existing field identity/caret");
        probe_drop();
    }
    check(refused_model>0 && refused_layout>0 && survived>0,"allocation sweep crosses model, layout and successful rebuild paths");
    printf("Yonder refusal cuts: model=%u layout=%u completed=%u\n",refused_model,refused_layout,survived);
}

static bool settings_applied;
static uint32_t settings_zoom;
static void settings_zoom_capture(uint32_t zoom) { settings_zoom=zoom; }
static uint32_t settings_seconds;
static void settings_capture(const char *agent, bool enabled, uint32_t seconds) {
    check(os64_streq(agent,YONDER_AGENT),"settings preserves agent while applying scripts");
    settings_applied=enabled;
    settings_seconds=seconds;
}
static void switch_cases(void) {
    saved_scripts[0]=0;
    check(!yonder_settings_saved_scripts(),"missing saved choice defaults off");
    strcpy(saved_scripts,"bogus");
    check(!yonder_settings_saved_scripts(),"unrecognized saved choice defaults off");
    memset(&settings_fixture,0,sizeof(settings_fixture));
    strcpy(settings_fixture.field_buf,YONDER_AGENT);
    settings_fixture.use=settings_capture;
    settings_fixture.zoom_use=settings_zoom_capture;
    strcpy(settings_fixture.zoom_buf,"125");
    settings_fixture.scripts.checked=true;
    settings_fixture.limit.value=12;
    check(yonder_settings_saved_script_seconds()==5,"a missing script time limit is the 5 s lean");
    strcpy(saved_seconds,"61");
    check(yonder_settings_saved_script_seconds()==5,"a time limit outside 1 to 60 is the lean");
    saved_seconds[0]=0;
    apply(&settings_fixture.d,false);
    check(settings_seconds==12 && saved_seconds[0]==0,"Apply hands this window the slider's limit without saving it");
    check(settings_applied && os64_streq(saved_scripts,"bogus"),"Apply enables this window without saving default");
    check(settings_zoom==1250 && saved_zoom[0]==0,"Apply keeps zoom independent of saved default");
    apply(&settings_fixture.d,true);
    check(settings_applied && yonder_settings_saved_scripts(),"Save as default persists on for new windows");
    check(yonder_settings_saved_zoom()==1250,"Save persists zoom alongside script switch");
    check(yonder_settings_saved_script_seconds()==12 && os64_streq(saved_seconds,"12"),
        "Save persists the script time limit as script_seconds");
    settings_fixture.scripts.checked=false;
    apply(&settings_fixture.d,true);
    check(!settings_applied && !yonder_settings_saved_scripts() && os64_streq(saved_scripts,"off"),
        "Save as default persists off");
    probe_page("<script>var kept=document.body;</script><script>document.body.id='later';</script>",true);
    script_turn();
    settings_use(YONDER_AGENT,true,7);
    check(g.script_ms==7000 && g.page.scripts!=NULL,"a new limit reaches the window and keeps the page's scripts");
    settings_use(YONDER_AGENT,false,7);
    check(!g.scripts_on && g.page.scripts==NULL,"applying off drops queued scripts and runtime");
    script_turn();
    check(os64_html_attr(g.page.way.doc->body,"id")==NULL,"off cannot execute remaining script");
    check(strstr(g.status_text,"SCRIPTS ON")==NULL,"off clears status indicator");
    probe_drop();
    yonder_agents_release();
}

static void edit_and_kind_changes(void) {
    probe_page("<input id=field type=number><script>document.body.setAttribute('class','first');</script>"
        "<script>document.body.setAttribute('class','second');</script>",true);
    script_turn();
    FormWidget *field=probe_field("field");
    os64_ui_textfield_set(&g.ui,&field->u.field,"-");
    field->u.field.cursor=0;
    script_turn();
    check(probe_field("field")==field && os64_streq(field->text,"-") && field->u.field.cursor==0,
        "unrelated script preserves intermediate invalid numeric edit");
    probe_drop();
    probe_page("<input id=field value=old><script>document.getElementById('field').setAttribute('type','checkbox');</script>",true);
    FormWidget *old=probe_field("field");
    script_turn();
    check(probe_field("field")!=old && probe_field("field")->kind==FW_CHECK,
        "control kind changes replace the native widget");
    probe_drop();
    probe_page("<input id=field value=old><script>document.getElementById('field').setAttribute('type','hidden');</script>",true);
    script_turn();
    check(probe_field("field")==NULL,"hidden controls have no interactive widget");
    probe_drop();
}
static void password_edits(void) {
    const char *starts[]={"abc","a",""};
    for(size_t i=0;i<3;i++) {
        char html[600];
        os64_snprintf(html,sizeof(html),"<h1 id=heading>before</h1><form action=/send>"
            "<input id=secret type=password name=p value='%s'><button id=send>Send</button></form>"
            "<script>document.getElementById('heading').textContent=document.getElementById('secret').value;</script>",starts[i]);
        probe_page(html,true);
        FormWidget *secret=probe_field("secret");
        os64_ui_set_focus(&g.ui,secret->w);
        if(i==2) {
            os64_gui_event_t a={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii='a'}};
            os64_gui_event_t e={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii=(char)0xe9}};
            check(password_key(&a) && password_key(&e),"real password keys append ASCII and Latin-1");
            forms_flush();
        }
        os64_gui_event_t back={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii='\b'}};
        check(password_key(&back),"real password Backspace consumed");
        forms_flush();
        const os64_page_control_t *c=os64_page_control(page_model(&g.page),widget_control(secret));
        const char *want=i==0?"ab":i==1?"":"a";
        check(c->value_len==strlen(want) && memcmp(c->value,want,c->value_len)==0,
            "initial, empty and previously flushed UTF-8 password deletions reach native state");
        os64_page_request_t request;
        char destination[128];
        os64_snprintf(destination,sizeof(destination),"https://fixture.test/send?p=%s",want);
        check(os64_page_activate(page_model(&g.page),(os64_page_what_t){OS64_PAGE_ACTIVATE_CONTROL,
            widget_control(probe_field("send")),0,0},&request)==OS64_PAGE_NAVIGATE &&
            os64_streq(request.url,destination),
            "password deletion reaches actual form request");
        os64_page_request_free(&request);
        script_turn();
        check(want[0]?probe_text_is("heading",want):probe_id("heading")->first_child==NULL,
            "next queued script observes password deletion");
        probe_drop();
    }
    probe_page("<h1 id=heading>before</h1><form action=/send><input id=secret type=password name=p>"
        "<button id=send>Send</button></form>"
        "<script>var p=document.getElementById('secret');document.getElementById('heading').textContent="
        "p.value.length===2&&p.value.charCodeAt(1)===0?'nul-kept':'lost span';</script>",true);
    FormWidget *secret=probe_field("secret");
    // The DOM setter replaces NUL with U+FFFD; the native value API also
    // accepts explicit spans, so exercise that editor projection directly.
    check(os64_page_set_text(page_model(&g.page),widget_control(secret),"a\0b",3)==0,
        "native password value accepts explicit span");
    forms_sync_from_model(true);
    check(secret->secret_len==3 && memcmp(secret->secret,"a\0b",3)==0,
        "native password value projects its full embedded-NUL span");
    os64_ui_set_focus(&g.ui,secret->w);
    os64_gui_event_t back={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii='\b'}};
    check(password_key(&back),"delete password byte after embedded NUL");
    script_turn();
    check(probe_text_is("heading","nul-kept"),"password edit flush preserves embedded NUL through binding");
    os64_page_request_t request;
    check(os64_page_activate(page_model(&g.page),(os64_page_what_t){OS64_PAGE_ACTIVATE_CONTROL,
        widget_control(probe_field("send")),0,0},&request)==OS64_PAGE_NAVIGATE &&
        os64_streq(request.url,"https://fixture.test/send?p=a%00"),"password span reaches encoded form request");
    os64_page_request_free(&request);
    probe_drop();

}
static void reset_editors(void) {
    probe_page("<form id=one action=/send><input id=text name=t value=seed>"
        "<input id=secret type=password name=p value=seed><input id=number type=number name=n>"
        "<button id=reset type=reset>Reset</button><button id=send>Send</button></form>"
        "<form><input id=other value=other></form><input id=owned form=one name=ext value=owned>",false);
    FormWidget *text=probe_field("text"), *secret=probe_field("secret"), *number=probe_field("number");
    FormWidget *other=probe_field("other"), *owned=probe_field("owned"), *reset=probe_field("reset");
    os64_ui_textfield_set(&g.ui,&text->u.field,"pending");
    os64_ui_textfield_set(&g.ui,&number->u.field,"-");
    os64_ui_textfield_set(&g.ui,&owned->u.field,"pending external");
    os64_ui_textfield_set(&g.ui,&other->u.field,"keep other edit");
    os64_ui_set_focus(&g.ui,secret->w);
    os64_gui_event_t key={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii='Q'}};
    check(password_key(&key),"unflushed password edit before reset");
    os64_ui_set_focus(&g.ui,other->w);
    other->u.field.cursor=3; other->u.field.anchor=1; other->u.field.selected=true;
    button_clicked(reset->w,reset);
    check(os64_streq(text->text,"seed") && secret->secret_len==4 &&
        memcmp(secret->secret,"seed",4)==0 && os64_streq(number->text,"") &&
        os64_streq(owned->text,"owned"),"reset discards unflushed editors, including externally owned control");
    check(os64_streq(other->text,"keep other edit") && g.ui.focus==other->w &&
        other->u.field.cursor==3 && other->u.field.selected,"reset preserves another form's edit and caret");
    forms_flush();
    os64_page_request_t request;
    check(os64_page_activate(page_model(&g.page),(os64_page_what_t){OS64_PAGE_ACTIVATE_CONTROL,
        widget_control(probe_field("send")),0,0},&request)==OS64_PAGE_NAVIGATE &&
        strstr(request.url,"t=seed") && strstr(request.url,"p=seed") &&
        strstr(request.url,"ext=owned") && !strstr(request.url,"pending"),
        "submission after reset contains defaults rather than discarded edits");
    os64_page_request_free(&request);
    os64_ui_textfield_set(&g.ui,&text->u.field,"keep refused edit");
    check(os64_html_set_attr(g.page.way.doc,g.page.way.doc->body,"class","changed",7)==OS64_HTML_OK,
        "stale reset fixture mutation");
    button_clicked(reset->w,reset);
    check(os64_streq(text->text,"keep refused edit") &&
        strstr(g.status_text,os64_page_reason_name(OS64_PAGE_REASON_STALE)),
        "refused reset retains editor and reports native refusal");
    probe_drop();
}
static void noscript_author_styles(void) {
    const char *styles[]={"display:block", "display:block!important", "display:contents!important"};
    for(size_t i=0;i<3;i++) for(int scripting=0;scripting<2;scripting++) {
        char html[512];
        os64_snprintf(html,sizeof(html),"<h1>lead</h1><style>noscript{%s}</style>"
            "<noscript id=fallback style='%s'><input id=inside value=fallback></noscript>",styles[i],styles[i]);
        probe_page(html,scripting!=0);
        os64_html_node_t *fallback=probe_id("fallback");
        check(fallback!=NULL,"noscript style fixture parsed");
        if(scripting) {
            check(flow_box_for(g.page.tree,fallback)==NULL &&
                flow_box_for(g.page.tree,fallback->first_child)==NULL,
                "author and inline display rules cannot expose scripting-mode raw fallback");
        } else {
            check(probe_field("inside")!=NULL && flow_box_for(g.page.tree,probe_id("inside"))!=NULL,
                "same author rules retain usable fallback with scripting off");
        }
        probe_drop();
    }
}
static void reclaim_browser_holders(void) {
    probe_page("<body><script>document.body.innerHTML='<h1 id=heading>replacement</h1>';</script>"
        "<script>document.getElementById('heading').textContent='removed script ran';</script>",true);
    script_turn();
    check(probe_text_is("heading","replacement") && yonder_scripts_pending(g.page.scripts),
        "replacement drops queued script after publishing a fresh layout");
    script_turn();
    check(probe_text_is("heading","replacement") && !yonder_scripts_pending(g.page.scripts),
        "queue hold permits safely skipping detached script after old layout release");
    probe_drop();

    probe_page("<div id=scroll style='overflow:auto;width:50px;height:30px'><p>content</p></div>",false);
    os64_html_node_t *scroll=probe_id("scroll");
    size_t count=g.page.way.doc->node_count;
    page_keep_box_scroll(&g.page,scroll,(flow_point_t){3,4});
    page_keep_box_scroll(&g.page,scroll,(flow_point_t){5,6});
    check(g.page.nbox_scrolls==1,"scroll identity has one retained record");
    check(os64_html_remove(g.page.way.doc,scroll)==OS64_HTML_OK && script_rebuild(),
        "scroller removal replaces native model, cascade and layout");
    check(g.page.way.doc->node_count==count && scroll->parent==NULL &&
        g.page.box_scrolls[0].node==scroll && g.page.box_scrolls[0].at.x==5,
        "scroll record retains detached identity across snapshot replacement");
    probe_drop();

    probe_page("<button id=gone type=submit>old</button><button id=keep type=submit>keep</button>",false);
    FormWidget *gone=probe_field("gone");
    os64_html_node_t *button=probe_id("gone");
    count=g.page.way.doc->node_count;
    check(os64_html_remove(g.page.way.doc,button)==OS64_HTML_OK,"detach a widget-held button");
    os64_page_t *old=page_model(&g.page), *fresh=os64_page_rebuild(old);
    check(fresh!=NULL,"widget replacement model builds");
    g.page.way.model=fresh;
    os64_page_free(old);
    flow_free(g.page.tree);g.page.tree=NULL;
    garb_cascade_free(g.page.cascade);g.page.cascade=NULL;
    g.page.sheets_changed=true;
    // This fixture publishes the rebuilt model directly before exercising
    // widget refusal; the layout gate requires its matching document version.
    g.page.rendered_version=os64_html_version(page_doc(&g.page));
    check(page_lay_out(&g.page,800,600,g.zoom,NULL),"widget replacement layout builds");
    fail_at=attempts+1;
    forms_build();
    fail_at=0;
    check(g.nfw==2 && g.fw[0]==gone,"refused widget replacement retains the previous widget");
    // Release presentation/state owners before inspecting the retained widget:
    // neither an older nor a late model pin may mask its own node hold.
    flow_free(g.page.tree);g.page.tree=NULL;
    garb_cascade_free(g.page.cascade);g.page.cascade=NULL;
    os64_page_free(g.page.way.model);g.page.way.model=NULL;
    check(g.page.way.doc->node_count==count && gone->node==button && button->parent==NULL,
        "widget alone retains detached identity after snapshot and state teardown");
    forms_drop();
    check(g.page.way.doc->node_count==count-2,
        "widget destruction reclaims button and text before document teardown");
    probe_drop();
}

static void snapshot_actions(void) {
    probe_page("<a id=old href=/old>old link</a><p>after</p>",false);
    int32_t x=0,y=0;
    bool found=false;
    for(int32_t yy=0; yy<50 && !found; yy++)
        for(int32_t xx=0; xx<180 && !found; xx++)
            if(link_at(xx,yy)==0) {x=xx;y=yy;found=true;}
    check(found,"hit-testing text inside a link resolves its ancestor");
    os64_html_node_t *old=probe_id("old");
    int64_t status=0;
    os64_html_node_t *new_link=os64_html_create_element(g.page.way.doc,OS64_HTML_NS_HTML,"a",&status);
    check(new_link && os64_html_set_attr(g.page.way.doc,new_link,"href","/wrong",6)==OS64_HTML_OK &&
        os64_html_insert(g.page.way.doc,old->parent,new_link,old)==OS64_HTML_OK &&
        os64_html_set_attr(g.page.way.doc,old,"href","/new",4)==OS64_HTML_OK,"reorder and change native links");
    os64_page_t *model=os64_page_rebuild(page_model(&g.page));
    os64_page_free(g.page.way.model); g.page.way.model=model;
    g.page.model_version=os64_html_version(g.page.way.doc);
    os64_html_pin_t pins[512]; size_t n=0;
    while(n<512 && (pins[n]=os64_html_pin(g.page.way.doc))!=0) n++;
    check(n<512 && !script_rebuild(),"layout pin exhaustion retains old snapshot with current model");
    check(flow_model(g.page.tree)!=model && link_at(x,y)==1,"old layout link index resolves through retained model node");
    os64_page_request_t request;
    check(os64_page_activate(model,(os64_page_what_t){OS64_PAGE_ACTIVATE_LINK,link_at(x,y),0,0},&request)==
        OS64_PAGE_NAVIGATE && os64_streq(request.url,"https://fixture.test/new"),"click sends current href after failed layout");
    os64_page_request_free(&request);
    for(size_t i=0;i<n;i++) os64_html_unpin(g.page.way.doc,pins[i]);
    check(os64_html_remove(g.page.way.doc,old)==OS64_HTML_OK,"detach old link");
    model=os64_page_rebuild(page_model(&g.page));
    os64_page_free(g.page.way.model); g.page.way.model=model;
    g.page.model_version=os64_html_version(g.page.way.doc);
    check(link_at(x,y)==-1,"detached old box does not activate another current link");
    probe_drop();
}
static void stylesheet_reuse(void) {
    unsigned refused=0;
    for (size_t cut=0; cut<=64; cut++) {
        probe_page("<link rel=stylesheet href='/site.css'><link rel=stylesheet href='/site.css'>"
            "<h1 id=heading>old</h1>",false);
        const char css[]="@import '/child.css'; h1{color:#123456}";
        for (int32_t i=0; i<g.page.nsheets; i++) {
            Sheet *sheet=&g.page.sheets[i];
            bool child=strstr(sheet->url,"child.css")!=NULL;
            const char *text=child?"h1{font-weight:bold}":css;
            check(garb_parse_sheet_text(text,strlen(text),&sheet->parsed)==GARB_OK,"linked sheet fixture parses");
            sheet_ready(&g.page,i);
        }
        check(g.page.nsheets==4 && page_lay_out(&g.page,800,600,g.zoom,&g.page),"duplicate links and imports lay out");
        const void *arenas[4];
        for(int i=0; i<4; i++) arenas[i]=g.page.sheets[i].parsed.arena;
        check(os64_html_set_text(g.page.way.doc,probe_id("heading")->first_child,"new",3)==OS64_HTML_OK,
            "linked sheet fixture text changes");
        if(cut) fail_at=attempts+cut;
        bool complete=script_rebuild();
        fail_at=0;
        if(!complete) {
            refused++;
            check(g.page.sheets[0].parsed.arena==arenas[0] && g.page.sheets[0].ready,
                "failed recascade keeps original linked parse");
            check(script_rebuild(),"linked parse survives failure and retry");
        }
        bool transferred=g.page.nsheets==4;
        for(int i=0; i<g.page.nsheets; i++) {
            bool found=false;
            for(int j=0; j<4; j++) found |= g.page.sheets[i].parsed.arena==arenas[j];
            transferred &= found && g.page.sheets[i].ready && g.page.sheets[i].borrowed_from==NULL;
            for(int j=0; j<i; j++) transferred &= g.page.sheets[i].parsed.arena!=g.page.sheets[j].parsed.arena;
        }
        // An optional sheet metadata allocation can fail while geometry still
        // succeeds; that sheet is omitted and its unused old parse is released.
        if(complete && g.page.nsheets!=4) {
            check(g.page.nsheets<4,"sheet metadata refusal remains bounded");
        } else if(complete && !transferred) {
            bool omitted=false;
            for(int i=0;i<g.page.nsheets;i++) omitted |= !g.page.sheets[i].ready;
            check(omitted,"refused import metadata may omit a sheet");
        } else {
            check(transferred,"ready duplicate parses and imports transfer without aliasing ownership");
        }
        probe_drop();
    }
    check(refused>0,"linked stylesheet sweep reaches rollback");
}

static void bounded_resources(void) {
    probe_page("<img id=pic src=file:///old.gif><script>document.getElementById('pic').setAttribute('src','file:///new.gif');</script>",true);
    pictures_start(&g.page);
    check(g.page.npics==1,"initial image address catalogued");
    script_turn();
    check(g.page.npics==2 && os64_streq(g.page.pics[0].url,"file:///old.gif") &&
        os64_streq(g.page.pics[1].url,"file:///new.gif"),"catalog addresses outlive rebuilt model");
    g.page.picture_url_bytes=4*1024*1024;
    check(picture_for(&g.page,"file:///refused.gif")==-1,"catalog byte cap refuses new addresses");
    check(picture_for(&g.page,"file:///old.gif")==0,"cap still permits existing address lookup");
    probe_drop();
    probe_page("<script>var held=document.body;</script><script>document.body.id='later';</script>",true);
    yonder_scripts_free(g.page.scripts); g.page.scripts=NULL;
    fail_at=attempts+1;
    check(scripts_host(g.page.way.doc,os64_page_shared_state(page_model(&g.page)),g.page.way.url,1)==NULL,
        "a script host the heap refuses publishes nothing");
    fail_at=0;
    probe_drop();
    size_t count=4097, one=sizeof("<script></script>")-1;
    char *many=malloc(count*one+1);
    check(many!=NULL,"script count fixture allocation");
    for(size_t i=0;i<count;i++) memcpy(many+i*one,"<script></script>",one);
    many[count*one]='\0';
    probe_page(many,true); free(many);
    unsigned ran=0;
    os64_js_outcome_t out;
    while(yonder_scripts_step(g.page.scripts,&out)) ran++;
    check(ran==4096,"a page's scripts past the 4096th are not queued");
    probe_drop();
    size_t big=4*1024*1024+1;
    const char *head="<script>", *tail="</script><script>document.body.id='after';</script>";
    char *huge=malloc(strlen(head)+big+strlen(tail)+1);
    check(huge!=NULL,"script source fixture allocation");
    strcpy(huge,head); memset(huge+strlen(head),' ',big); strcpy(huge+strlen(head)+big,tail);
    probe_page(huge,true); free(huge);
    check(yonder_scripts_step(g.page.scripts,&out) && out.status==OS64_JS_EXCEPTION &&
        strstr(out.message,"longer than a script may be") && strstr(out.source_name,"#inline-1"),
        "an inline script past the source limit is reported in its place");
    check(yonder_scripts_step(g.page.scripts,&out) && out.status==OS64_JS_OK &&
        os64_html_attr(g.page.way.doc->body,"id") && os64_streq(os64_html_attr(g.page.way.doc->body,"id")->value,"after"),
        "the scripts after it still run");
    probe_drop();
}

/* ── INPUT EVENTS (docs/design/pending/DOM_D7.md § Input events) ────────
 * The window's real input sites — view_event, hover, the widgets' own
 * callbacks, key_event — with the queue run as the loop runs it. */
static void input_click_at(const char *id) {
    const flow_box_t *b=flow_box_for(g.page.tree,probe_id(id));
    int32_t x=g.view.bounds.x+b->rect.x+2-g.sx, y=g.view.bounds.y+b->rect.y+2-g.sy;
    os64_gui_event_t ev={.type=OS64_GUI_EVENT_MOUSE_BUTTON_DOWN};
    ev.mouse.x=x; ev.mouse.y=y; ev.mouse.button=OS64_GUI_MOUSE_LEFT;
    view_event(&g.view,&g.ui,&ev); inputs_run();
    ev.type=OS64_GUI_EVENT_MOUSE_BUTTON_UP;
    view_event(&g.view,&g.ui,&ev); inputs_run();
}
static void input_key(char ascii) {
    os64_gui_event_t ev={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii=ascii}};
    if(!key_event(&ev) && !password_key(&ev)) os64_ui_dispatch(&g.ui,&ev);
    inputs_run();
}
static bool status_says(const char *text) { return strstr(g.status_text,text)!=NULL; }
static void input_links(void) {
    probe_page("<p><a id=stay href=/away>stay</a> <a id=go href=/gone>go</a></p><p id=log>-</p>"
        "<script>var log=[];function note(n){log.push(n);document.getElementById('log').textContent=log.join('|');}"
        "document.getElementById('stay').onclick=function(){note('stay');return false};"
        "document.getElementById('go').addEventListener('mousedown',function(){note('down')});"
        "document.getElementById('go').addEventListener('mouseup',function(){note('up')});"
        "document.getElementById('go').addEventListener('click',function(e){note('go '+e.clientX+' '+e.isTrusted)});</script>",true);
    script_turn();
    input_click_at("stay");
    check(probe_text_is("log","stay") && !status_says("background workers"),
        "input: a click a handler cancels follows no link");
    input_click_at("go");
    const flow_box_t *b=flow_box_for(g.page.tree,probe_id("go"));
    char want[64]; snprintf(want,sizeof(want),"stay|down|up|go %d true",b->rect.x+2);
    check(probe_text_is("log",want),"input: mousedown, mouseup and click reach the element, with clientX and isTrusted");
    check(status_says("background workers"),"input: an uncancelled click follows its link after its listeners");
    probe_drop();
}
static void input_hover(void) {
    probe_page("<p id=quiet>no handlers here</p><script>var x=1;</script>",true);
    script_turn();
    uint64_t tasks=yonder_scripts_tasks(g.page.scripts);
    size_t before=attempts;
    const flow_box_t *b=flow_box_for(g.page.tree,probe_id("quiet"));
    for(int i=0;i<20;i++) { hover(b->rect.x+2+i,b->rect.y+2); inputs_run(); }
    check(yonder_scripts_tasks(g.page.scripts)==tasks && attempts-before<200,
        "input: pointer moves over a page that listens to nothing run no task");
    probe_drop();
    probe_page("<p><b id=a>first</b> <b id=b>second</b></p><p id=log>-</p>"
        "<script>var log=[];function note(n){log.push(n);document.getElementById('log').textContent=log.join('|');}"
        "document.getElementById('a').onmouseover=function(e){note('over a from '+(e.relatedTarget?e.relatedTarget.id:'none'))};"
        "document.getElementById('a').onmouseout=function(e){note('out a to '+e.relatedTarget.id)};"
        "document.getElementById('b').onmouseover=function(){note('over b')};</script>",true);
    script_turn();
    /* A listener's change relays the page out: each box is found again. */
    #define AT(id,dx) flow_box_for(g.page.tree,probe_id(id))->rect.x+(dx),flow_box_for(g.page.tree,probe_id(id))->rect.y+2
    hover(AT("a",2)); inputs_run();
    hover(AT("a",3)); inputs_run();
    hover(AT("b",2)); inputs_run();
    #undef AT
    check(probe_text_is("log","over a from none|out a to b|over b"),
        "input: mouseover and mouseout follow the element under the pointer, with relatedTarget");
    probe_drop();
}
static void input_forms(void) {
    probe_page("<form id=f action=/send><input id=field name=q><input id=tick type=checkbox name=t>"
        "<button id=send>send</button><button id=undo type=reset>undo</button></form><p id=log>-</p>"
        "<script>var log=[];function note(n){log.push(n);document.getElementById('log').textContent=log.join('|');}"
        "var f=document.getElementById('f'),q=document.getElementById('field');"
        "q.addEventListener('input',function(){note('input '+q.value)});"
        "q.addEventListener('change',function(){note('change '+q.value)});"
        "q.addEventListener('focus',function(){note('focus')});q.addEventListener('blur',function(){note('blur')});"
        "q.addEventListener('keydown',function(e){if(e.key==='x'){e.preventDefault();note('no x')}});"
        "f.onsubmit=function(){note('submit');return q.value==='go'};"
        "f.onreset=function(){note('reset');return false};"
        "document.getElementById('tick').onclick=function(){note('tick');return false};</script>",true);
    script_turn();
    FormWidget *field=probe_field("field");
    os64_ui_set_focus(&g.ui,field->w); inputs_run();
    input_key('a'); input_key('x'); input_key('b');
    check(os64_streq(field->text,"ab"),"input: a cancelled keydown is not typed");
    check(probe_text_is("log","focus|input a|no x|input ab"),
        "input: focus, then an input event per edit with the value already the script's");
    input_key('\r');
    check(probe_text_is("log","focus|input a|no x|input ab|change ab|submit") && !status_says("background workers"),
        "input: Enter fires change, then submit, which the handler cancels");
    os64_ui_set_focus(&g.ui,&g.view); inputs_run();
    check(probe_text_is("log","focus|input a|no x|input ab|change ab|submit|blur"),
        "input: blur, with no second change for an unchanged value");
    os64_ui_set_focus(&g.ui,field->w); inputs_run();
    input_key('c');
    os64_ui_set_focus(&g.ui,&g.view); inputs_run();
    check(probe_text_is("log","focus|input a|no x|input ab|change ab|submit|blur|focus|input abc|change abc|blur"),
        "input: a value changed since focus fires change before blur");
    os64_ui_textfield_set(&g.ui,&field->u.field,"ab"); forms_flush();
    FormWidget *undo=probe_field("undo");
    button_clicked(undo->w,undo); inputs_run();
    check(probe_text_is("log","focus|input a|no x|input ab|change ab|submit|blur|focus|input abc|change abc|blur|reset") &&
        os64_streq(field->text,"ab"),
        "input: a cancelled reset keeps the values");
    FormWidget *tick=probe_field("tick");
    tick->u.check.checked=true; check_changed(&tick->u.check,tick); inputs_run();
    check(!probe_field("tick")->u.check.checked && !os64_page_control(page_model(&g.page),
        os64_page_control_for(page_model(&g.page),probe_id("tick")))->checked,
        "input: a cancelled click puts the box back, in the widget and the model");
    os64_ui_textfield_set(&g.ui,&field->u.field,"go");
    FormWidget *send=probe_field("send");
    button_clicked(send->w,send); inputs_run();
    check(status_says("background workers"),"input: an uncancelled submit sends the form after its listeners");
    probe_drop();
}
static void input_cases(void) {
    input_links();
    input_hover();
    input_forms();
}

/* ── THE STREAM (docs/design/pending/DOM_D4.md) ─────────────────────────
 * The window's side: a navigation started, its head, body and verdict
 * posted by the test standing in for the worker, and stream_turn feeding
 * the parser a slice per turn until the page arrives through the same
 * arrive() a worker's page used to take. */
static void stream_window(void) {
    os64_memset(&g,0,sizeof(g));
    g.scripts_on=false; g.way.agent=YONDER_AGENT; g.way.name="yonder"; g.settle_due=YONDER_NEVER;
    g.zoom=g.zoom_default=1000;
    os64_ui_init(&g.ui,NULL);
    os64_ui_panel(&g.root);
    os64_ui_button(&g.back,"Back",NULL,NULL); os64_ui_button(&g.forward,"Forward",NULL,NULL);
    os64_ui_button(&g.reload,"Reload",NULL,NULL); os64_ui_button(&g.stop,"Stop",NULL,NULL);
    os64_ui_textfield(&g.field,g.field_buf,sizeof(g.field_buf),NULL,NULL,NULL);
    os64_ui_label(&g.status,g.status_text);
    g.view.cls=&kViewClass; g.view.focusable=true;
    os64_ui_scrollbar(&g.vbar,NULL,NULL); os64_ui_scrollbar(&g.hbar,NULL,NULL);
    os64_ui_panel(&g.qpanel); os64_ui_label(&g.qlabel,g.qtext);
    os64_ui_button(&g.qyes,"Yes",NULL,NULL); os64_ui_button(&g.qno,"No",NULL,NULL);
    os64_ui_widget_t *kids[]={&g.back,&g.forward,&g.reload,&g.stop,&g.field.w,&g.view,&g.vbar.w,
        &g.hbar.w,&g.qpanel,&g.qlabel,&g.qyes,&g.qno,&g.status};
    for(size_t i=0;i<sizeof(kids)/sizeof(kids[0]);i++) os64_ui_add_child(&g.root,kids[i]);
    g.root.bounds=(os64_gui_rect_t){0,0,860,640};
    os64_ui_set_root(&g.ui,&g.root);
    g.view.bounds=(os64_gui_rect_t){0,0,800,600};
    s_env.text=probe_text; s_env.fonts=probe_fonts; s_env.replaced_size=replaced_size;
    g.pool=(os64_work_pool_t *)&pool_fake;
    pool_open=true; pool_ids=0; pool_cancelled=0; pool_error=0; memset(&pool_work,0,sizeof(pool_work));
}
static void stream_window_drop(void) {
    pool_open=false;
    if(g.stream.active) stream_drop();
    if(pool_work.job!=NULL) { yonder_trip_release(pool_work.job,NULL); memset(&pool_work,0,sizeof(pool_work)); }
    inputs_drop(); forms_drop(); page_clear(&g.page); page_clear(&g.coming.page); os64_ui_font_release(&g.ui);
    while(nsheet_jobs>0) os64_free(sheet_jobs[--nsheet_jobs].job);
}
/* The worker's part: a head for `url`, the body in `chunk`-sized posts, the verdict. */
static way_head_t stream_head(const char *type, const char *charset, bool posted) {
    way_head_t h; memset(&h,0,sizeof(h));
    h.status=200; os64_strcopy(h.reason,sizeof(h.reason),"OK");
    os64_strcopy(h.content_type,sizeof(h.content_type),type);
    os64_strcopy(h.charset,sizeof(h.charset),charset);
    os64_strcopy(h.url,sizeof(h.url),"http://fixture.test/final");
    h.posted=posted; h.body=os64_streq(type,"text/html") ? WAY_BODY_HTML : WAY_BODY_TEXT;
    return h;
}
static void stream_post_body(const uint8_t *bytes, size_t len, size_t chunk) {
    for(size_t at=0;at<len;at+=chunk)
        yonder_mail_post(g.stream.mail,bytes+at,len-at<chunk ? len-at : chunk,NULL,NULL);
}
static void stream_post_verdict(bool page, os64_fetch_status_t fetch, const char *reason) {
    yonder_verdict_t v; memset(&v,0,sizeof(v));
    v.page=page; v.fetch=fetch; os64_strcopy(v.reason,sizeof(v.reason),reason);
    yonder_mail_post_verdict(g.stream.mail,&v);
}
static bool stream_same_tree(const uint8_t *bytes, size_t len) {
    os64_html_document_t *want=parse_file(bytes,len,false);
    const os64_html_document_t *got=page_doc(&g.page);
    char a[1<<17], b[1<<17];
    size_t na=want ? os64_html_serialize(want->document,true,false,a,sizeof(a)) : 0;
    size_t nb=got ? os64_html_serialize(got->document,true,false,b,sizeof(b)) : 1;
    bool same=want && got && na==nb && na<sizeof(a) && memcmp(a,b,na)==0;
    os64_html_document_free(want);
    return same;
}
static void stream_slices(void) {
    /* Five slot-sized chunks: one turn feeds four (the 64 KiB slice) and
     * asks to be rung again; the next finishes, with the verdict already
     * waiting behind the chunks. */
    stream_window();
    start_trip("http://fixture.test/p",NULL,NAV_GO,NULL);
    check(g.stream.active && g.nav.id==1 && pool_work.job!=NULL && g.stop.disabled==false,
        "stream: a navigation starts a stream and lights Stop");
    size_t len=5*YONDER_STREAM_CHUNK;
    uint8_t *body=os64_malloc(len);
    memset(body,'x',len);
    const char *open="<!doctype html><title>T</title><p>";
    memcpy(body,open,strlen(open));
    const char *close="</p><h1 id=t>end</h1>";
    memcpy(body+len-strlen(close),close,strlen(close));
    way_head_t h=stream_head("text/html","",false);
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_body(body,len,YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    check(stream_turn()==true && g.stream.active && g.page.tree==NULL,
        "stream: one slice feeds four chunks and asks for another turn");
    check(stream_turn()==false && !g.stream.active && g.page.tree!=NULL,
        "stream: the verdict behind the last chunk finishes the page");
    check(stream_same_tree(body,len),"stream: the tree is the one the same bytes parse to whole");
    check(os64_streq(g.page.way.note,"200 OK") && os64_streq(g.page.way.url,"http://fixture.test/final") &&
        !g.page.way.posted,"stream: the standing line and address are the head's");
    check(g.stop.disabled==true && g.reload.disabled==false,"stream: Stop goes out when the page is in");
    check(probe_text_is("t","end"),"stream: the last chunk's text is in the page");
    os64_free(body);
    stream_window_drop();
}
static void stream_reaped_first(void) {
    /* The job returns the moment its last post lands; the window may hear
     * the reap before it has parsed a byte. The parse goes on from the
     * mailbox the stream holds. */
    stream_window();
    start_trip("http://fixture.test/p",NULL,NAV_GO,NULL);
    way_head_t h=stream_head("text/html","utf-8",false);
    yonder_mail_post_head(g.stream.mail,&h);
    const char *html="<p id=t>caf\xC3\xA9</p>";
    stream_post_body((const uint8_t *)html,strlen(html),YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    reaped(1,pool_work.job,NULL); memset(&pool_work,0,sizeof(pool_work));
    check(g.nav.id==0 && g.stream.active && g.stop.disabled==false,
        "stream: a reaped job leaves the stream, and Stop, alive");
    check(stream_turn()==false && g.page.tree!=NULL && probe_text_is("t","caf\xC3\xA9"),
        "stream: the page arrives after its job is gone");
    stream_window_drop();
}
static void stream_text(void) {
    /* text/plain through the same parser, after <plaintext>, in the
     * encoding the first bytes decide. */
    stream_window();
    start_trip("http://fixture.test/t",NULL,NAV_GO,NULL);
    way_head_t h=stream_head("text/plain","",false);
    yonder_mail_post_head(g.stream.mail,&h);
    const char *text="\xEF\xBB\xBF" "plain <b>not bold</b> caf\xC3\xA9";
    stream_post_body((const uint8_t *)text,strlen(text),YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    check(stream_turn()==false && g.page.plain!=NULL && g.page.way.doc==NULL && g.page.way.text_utf8 &&
        page_doc(&g.page)==g.page.plain && g.page.tree!=NULL,
        "stream: text arrives as its own tree, UTF-8 by its byte order mark");
    const os64_html_node_t *pre=g.page.plain->body ? g.page.plain->body->first_child : NULL;
    check(pre && os64_streq(pre->name,"plaintext") && pre->first_child &&
        pre->first_child->kind==OS64_HTML_TEXT && strstr(pre->first_child->text,"<b>not bold</b>")!=NULL,
        "stream: the text is literal under plaintext");
    check(os64_streq(g.page.way.note,"200 OK"),"stream: a text page's standing line");
    stream_window_drop();
    /* No label, no mark, not JSON: windows-1252. */
    stream_window();
    start_trip("http://fixture.test/t",NULL,NAV_GO,NULL);
    h=stream_head("text/plain","",false);
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_body((const uint8_t *)"\x93quoted\x94",8,YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    check(stream_turn()==false && g.page.plain!=NULL && !g.page.way.text_utf8,
        "stream: unlabelled text reads as windows-1252");
    pre=g.page.plain->body ? g.page.plain->body->first_child : NULL;
    check(pre && pre->first_child && strstr(pre->first_child->text,"\xE2\x80\x9Cquoted\xE2\x80\x9D")!=NULL,
        "stream: a smart quote draws as one");
    stream_window_drop();
}
static void stream_no_page(void) {
    stream_window();
    start_trip("http://fixture.test/x",NULL,NAV_GO,NULL);
    stream_post_verdict(false,OS64_FETCH_OK," that is image/png, not a page - save it with:  os64get 'x'");
    check(stream_turn()==false && !g.stream.active && g.page.tree==NULL &&
        os64_streq(g.status_rest,"that is image/png, not a page - save it with:  os64get 'x'") &&
        g.stop.disabled==true,"stream: no page, libway's sentence, and Stop goes out");
    stream_window_drop();
}
static void stream_refused(void) {
    /* Deeper than twice the parser's stack: the parser refuses, the fetch
     * is cancelled, and the page arrives as far as it got, saying so. */
    stream_window();
    start_trip("http://fixture.test/deep",NULL,NAV_GO,NULL);
    way_head_t h=stream_head("text/html","",false);
    yonder_mail_post_head(g.stream.mail,&h);
    size_t depth=1200, len=depth*5+64;
    uint8_t *body=os64_malloc(len);
    size_t at=0;
    const char *lead="<p id=t>deep</p>";
    memcpy(body+at,lead,strlen(lead)); at+=strlen(lead);
    for(size_t i=0;i<depth;i++) { memcpy(body+at,"<div>",5); at+=5; }
    stream_post_body(body,at,YONDER_STREAM_CHUNK);
    bool arrived=false;
    for(int turns=0;turns<8 && g.stream.active;turns++) { stream_turn(); arrived=g.page.tree!=NULL; }
    check(arrived && pool_cancelled==1 && g.nav.id==0 && !g.stream.active,
        "stream: a parser refusal cancels the fetch and arrives with what it has");
    check(strstr(g.page.way.note,"200 OK - the page is bigger than this browser will parse (")!=NULL &&
        probe_text_is("t","deep"),"stream: the standing line names the refusal");
    os64_free(body);
    /* The verdict of a cancelled job never comes; the chunks it posted
     * before it noticed are nobody's now. */
    stream_window_drop();
}
static void stream_stopped(void) {
    stream_window();
    start_trip("http://fixture.test/p",NULL,NAV_GO,NULL);
    way_head_t h=stream_head("text/html","",false);
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_body((const uint8_t *)"<p>half",7,YONDER_STREAM_CHUNK);
    check(stream_turn()==false && g.stream.active && g.stream.parser!=NULL,"stream: a parse under way");
    stop_trip();
    check(!g.stream.active && g.stream.parser==NULL && g.stream.mail==NULL && pool_cancelled==1 &&
        g.nav.id==0 && g.stop.disabled==true,"stream: Stop drops the parse, cancels the fetch, lets go");
    stream_window_drop();
}
static void stream_pool_breaks(void) {
    stream_window();
    start_trip("http://fixture.test/p",NULL,NAV_GO,NULL);
    way_head_t h=stream_head("text/html","",false);
    yonder_mail_post_head(g.stream.mail,&h);
    stream_turn();
    pool_error=-1;
    os64_gui_event_t bell={.type=OS64_GUI_EVENT_DOORBELL,.doorbell={.mask=BELL_WORK}};
    on_doorbell(&g.ui,&bell);
    check(!g.stream.active && g.pool==NULL && g.nav.id==0 &&
        strstr(g.status_rest,"background workers stopped")!=NULL,
        "stream: a broken pool drops the stream with the pool");
    stream_window_drop();
}
static void stream_keeps_the_form(void) {
    /* The window's own copy of a form being sent reaches the page that
     * comes back, whose Reload then sends it again. */
    stream_window();
    os64_page_request_t request; memset(&request,0,sizeof(request));
    request.method=OS64_PAGE_METHOD_POST; request.control=-1;
    char *url=os64_malloc(32); strcpy(url,"http://fixture.test/send"); request.url=url;
    char *type=os64_malloc(40); strcpy(type,"application/x-www-form-urlencoded"); request.content_type=type;
    char *body=os64_malloc(4); memcpy(body,"q=1",4); request.body=body; request.body_len=3;
    start_trip(request.url,&request,NAV_GO,NULL);
    yonder_trip_t *trip=pool_work.job;
    check(g.stream.has_sent && os64_streq(g.stream.sent.url,"http://fixture.test/send") &&
        trip->has_request && os64_streq(trip->request.url,"http://fixture.test/send") &&
        trip->request.url!=g.stream.sent.url,"stream: the job and the window each hold a copy of the form");
    way_head_t h=stream_head("text/html","",true);
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_body((const uint8_t *)"<p>sent</p>",11,YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    check(stream_turn()==false && g.page.way.posted && g.page.sent.url!=NULL &&
        os64_streq(g.page.sent.url,"http://fixture.test/send") && g.page.sent.body_len==3,
        "stream: the reply to a form keeps the form");
    stream_window_drop();
}
static void stream_mail_wiring(void) {
    /* Progress and a worker's question still reach the status line and the
     * bar through the stream's mailbox; a new navigation answers No. */
    stream_window();
    start_trip("http://fixture.test/p",NULL,NAV_GO,NULL);
    yonder_mail_progress(g.stream.mail," reading 64 KB of http://fixture.test/p");
    os64_gui_event_t bell={.type=OS64_GUI_EVENT_DOORBELL,.doorbell={.mask=BELL_MAIL}};
    on_doorbell(&g.ui,&bell);
    check(os64_streq(g.status_rest,"reading 64 KB of http://fixture.test/p"),"stream: progress reaches the status line");
    /* A bell for a navigation that is not this one is not read. */
    g.generation++;
    yonder_mail_progress(g.stream.mail," reading 128 KB of somewhere else");
    on_doorbell(&g.ui,&bell);
    check(os64_streq(g.status_rest,"reading 64 KB of http://fixture.test/p"),"stream: another generation's mail is not read");
    g.generation--;
    uint32_t number=yonder_mail_ask(g.stream.mail," follow to http?");
    on_doorbell(&g.ui,&bell);
    check(g.asker==ASK_WORKER && g.bar.up && g.bar.number==number,"stream: a worker's question raises the bar");
    /* A new navigation CANCELS the job rather than answering it (YONDER.md
     * § Y3): the cancel ends the worker's wait, which is No. A replaced
     * question is the one that gets an explicit No. */
    yonder_mail_t *old=g.stream.mail; yonder_mail_hold(old);
    uint32_t second=yonder_mail_ask(old," and again?");
    on_doorbell(&g.ui,&bell);
    check(old->answers==1 && old->answered==number && !old->answered_yes && g.bar.number==second,
        "stream: a newer question answers the older one No");
    start_trip("http://fixture.test/q",NULL,NAV_GO,NULL);
    check(old->answers==1 && g.asker==ASK_NONE && !g.bar.up && pool_cancelled==1 && g.nav.id==2 &&
        g.stream.mail!=old,"stream: the next navigation cancels the job, lowers the bar and starts afresh");
    yonder_mail_drop(old);
    stream_window_drop();
}
static void stream_captures_mode(void) {
    /* The parser mode is the one the navigation started with, and a script's
     * stop is resumed at once: with scripting on, noscript is text and the
     * fallback control does not exist; off, it does. The script is a module
     * because the parser stops at any script's end tag while yonder queues
     * only classic ones: a queued script rings the window's doorbell, which
     * on the host is a raw syscall instruction nobody can answer. */
    /* The parser reads its first 1024 bytes for an encoding before it parses
     * any of them, so a stop inside that window would wait for finish, which
     * runs straight through; a comment carries the script past it so the
     * feed itself stops and the resume is the window's. */
    static char html[2048];
    os64_snprintf(html,sizeof(html),"<!doctype html><!-- %0*d --><noscript><input id=fallback></noscript>"
        "<script type=module>var one=1;</script><p id=t>page</p>",1200,0);
    for(int on=0;on<2;on++) {
        stream_window();
        g.scripts_on=on!=0;
        start_trip("http://fixture.test/mode",NULL,NAV_GO,NULL);
        g.scripts_on=!g.scripts_on;     /* changed after the start: the stream keeps what it captured */
        way_head_t h=stream_head("text/html","",false);
        yonder_mail_post_head(g.stream.mail,&h);
        stream_post_body((const uint8_t *)html,strlen(html),YONDER_STREAM_CHUNK);
        stream_post_verdict(true,OS64_FETCH_OK,"");
        g.scripts_on=on!=0;
        check(stream_turn()==false && g.page.tree!=NULL && g.page.scripting==(on!=0) &&
            os64_page_ncontrols(page_model(&g.page))==(on ? 0 : 1) && probe_text_is("t","page") &&
            pool_cancelled==0 && os64_streq(g.page.way.note,"200 OK"),
            on ? "stream: scripting on is captured at the start, and a script's stop is resumed, not refused"
               : "stream: scripting off is captured at the start");
        stream_window_drop();
    }
}
/* ── THE LOOP (docs/design/pending/DOM_D7.md) ───────────────────────────
 * Scripts run as the parser reaches them, through the stream: the test
 * plays the worker for the page and, through script_land, for every
 * `src` script's fetch. */
static char *pad_page(const char *rest) {
    size_t n=strlen(rest)+1400;
    char *html=malloc(n);
    /* A comment carries the first script past the parser's 1024-byte
     * encoding window, so the feed itself stops at it. */
    snprintf(html,n,"<!doctype html><!-- %01200d -->%s",0,rest);
    return html;
}
static void loop_page(const char *rest) {
    stream_window();
    g.scripts_on=true; g.script_ms=5000; now_ms=100;
    start_trip("http://fixture.test/p",NULL,NAV_GO,NULL);
    char *html=pad_page(rest);
    way_head_t h=stream_head("text/html","",false);
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_body((const uint8_t *)html,strlen(html),YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    free(html);
}
/* Turns until one says there is nothing more to do now; false if the
 * stream never said so (a loop that rings itself for ever). */
static bool loop_settle(void) {
    for(int i=0;i<200;i++) if(!stream_turn() && !coming_turn()) return true;
    return false;
}
static void loop_drop(void) {
    stream_window_drop();
    check(nscript_jobs==0,"loop: every script fetch was landed or cancelled");
    nscript_jobs=0;
}
static void loop_order(void) {
    loop_page("<html><head><script>var order=[];function note(n){order.push(n);var o=document.getElementById('out');"
        "if(o)o.textContent=order.join('|');}note('head inline');"
        "document.addEventListener('DOMContentLoaded',function(){note('DOMContentLoaded')});"
        "window.addEventListener('load',function(){note('load listener')});"
        "setTimeout(function(){note('timer')},50);</script>"
        "<script src=blocking.js></script><script defer src=deferred.js></script>"
        "<script async src=async.js></script><script type=module>note('module')</script>"
        "</head><body onload=\"note('body onload')\"><p id=out></p>"
        "<script>note('body inline');var s=document.createElement('script');"
        "s.textContent=\"note('connected')\";document.body.appendChild(s);</script>"
        "<script>note('body inline 2')</script></body></html>");
    check(loop_settle() && g.stream.stopped && nscript_jobs==1 && g.page.tree==NULL,
        "loop: the parse stops at a blocking src and waits for its fetch, ringing nothing");
    check(script_land("/blocking.js","note('blocking')"),"loop: the blocking script's fetch is out");
    check(stream_turn() && g.stream.stopped && nscript_jobs==2,
        "loop: one turn runs the blocking script, passes the defer and async ones (their fetches go out) "
        "and stops at the next inline script");
    check(script_land("/async.js","note('async')"),"loop: an async script is fetched beside the parse");
    /* The deferred script's source is in hand long before the parse ends,
     * and still waits for it. */
    check(script_land("/deferred.js","note('deferred')"),"loop: the deferred script's fetch was out");
    check(loop_settle() && !g.stream.active && g.page.tree!=NULL,"loop: the page arrives after its deferred script");
    check(probe_text_is("out","head inline|blocking|async|body inline|connected|body inline 2|deferred|"
        "DOMContentLoaded|load listener|body onload"),
        "loop: scripts, DOMContentLoaded and load run in the standard's order");
    now_ms=200;
    check(script_turn() && probe_text_is("out","head inline|blocking|async|body inline|connected|"
        "body inline 2|deferred|DOMContentLoaded|load listener|body onload|timer"),
        "loop: a timer a head script set fires on the shown page, and the page redraws");
    loop_drop();
}
static void loop_fetch_failed(void) {
    loop_page("<p id=out>start</p><script src=gone.js></script>"
        "<script>document.getElementById('out').textContent='after';</script>");
    check(loop_settle() && g.stream.stopped,"loop: stopped at a src that will fail");
    check(script_land("/gone.js",NULL),"loop: the failing fetch comes back");
    stream_turn();
    check(strstr(g.status_text,"could not fetch the script")!=NULL && strstr(g.status_text,"/gone.js")!=NULL,
        "loop: a script that could not be fetched is said, by its address");
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","after"),
        "loop: it is skipped and the parse carries on");
    loop_drop();
}
static void loop_overrun(void) {
    loop_page("<p id=out>before</p><script>for(;;){}</script>"
        "<script>document.getElementById('out').textContent='must not run';</script><p>tail</p>");
    stream_turn();                       /* stops at the first script */
    clock_step=1000;
    stream_turn();                       /* runs it: the deadline ends it */
    clock_step=0;
    check(strstr(g.status_text,"execution stopped after 5 s (this page runs without script)")!=NULL,
        "loop: a script that runs too long says so on the status line");
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","before") &&
        !yonder_scripts_alive(g.page.scripts),
        "loop: the parse goes on to the end without script, and the page arrives");
    check(strstr(g.status_text,"execution stopped after 5 s (this page runs without script)")!=NULL,
        "loop: the sentence is still on the status line once the page has arrived");
    loop_drop();
}
static void loop_navigates(void) {
    loop_page("<p>leaving</p><script>location.href='next.html';</script><p id=out>never shown</p>");
    loop_settle();
    check(g.stream.active && pool_work.job!=NULL &&
        os64_streq(((yonder_trip_t *)pool_work.job)->url,"http://fixture.test/next.html") && g.page.tree==NULL,
        "loop: a script's location= starts a navigation to the address resolved against the page");
    loop_drop();
    loop_page("<script>document.addEventListener('DOMContentLoaded',function(){location.replace('/elsewhere')});</script>"
        "<p id=out>never shown</p>");
    loop_settle();
    check(g.stream.active && g.page.tree==NULL &&
        os64_streq(((yonder_trip_t *)pool_work.job)->url,"http://fixture.test/elsewhere"),
        "loop: a navigation asked for at DOMContentLoaded replaces the page before it is shown");
    loop_drop();
}
static void loop_teardown(void) {
    const char *page="<script>var kept;document.addEventListener('click',function(e){kept=e});"
        "setTimeout(function(){},100000);</script><script src=never.js></script><p>x</p>";
    for(int how=0;how<3;how++) {
        loop_page(page);
        check(loop_settle() && g.stream.stopped && nscript_jobs==1,
            "loop: stopped at a script whose fetch never comes, with a listener and a timer");
        unsigned cancels=script_cancels;
        if(how==0) click_stop(NULL,NULL);
        else if(how==1) settings_use(YONDER_AGENT,false,5);
        if(how<2) check(!g.stream.active && script_cancels==cancels+1,
            how==0 ? "loop: Stop abandons the parse, drains the page's scripts and cancels its fetch"
                   : "loop: turning scripts off does the same");
        loop_drop();                    /* the third: the window's close, mid-stream */
    }
}
static void loop_limit(void) {
    loop_page("<script>setTimeout(function(){for(;;){}},10);</script><p id=out>x</p>");
    check(loop_settle() && g.page.tree!=NULL,"loop: a page with a runaway timer arrives");
    settings_use(YONDER_AGENT,true,1);
    check(g.script_ms==1000,"loop: Settings' limit reaches the window");
    now_ms=200; clock_step=500;
    script_turn();
    clock_step=0;
    check(strstr(g.status_text,"execution stopped after 1 s (this page runs without script)")!=NULL &&
        !yonder_scripts_alive(g.page.scripts),
        "loop: the next task runs under the limit Settings applied");
    loop_drop();
}
static void loop_local_file(void) {
    stream_window();
    g.scripts_on=true; g.script_ms=5000;
    slurp_path="/pages/local.html";
    slurp_bytes="<p id=out>disk</p><script>document.getElementById('out').textContent='ran from disk';</script>";
    open_address("/pages/local.html",NAV_GO,NULL,NULL);
    check(g.stream.active && g.stream.local!=NULL,"loop: a file takes the stream");
    check(loop_settle() && !g.stream.active && probe_text_is("out","ran from disk") &&
        os64_streq(g.page.way.url,"file:///pages/local.html") && g.page.way.note[0]=='\0',
        "loop: a page from disk runs its scripts as it is parsed and arrives with no standing line");
    slurp_path=NULL;
    loop_drop();
}
static void loop_handlers_only(void) {
    loop_page("<body onload=\"document.getElementById('h').textContent='onload ran '+(this===window)\">"
        "<h1 id=h>waiting</h1></body>");
    check(loop_settle() && g.page.tree!=NULL && g.page.scripts!=NULL && probe_text_is("h","onload ran true"),
        "loop: a page whose only script is <body onload> gets a runtime at load, and runs it as window's");
    loop_drop();
    loop_page("<p id=h>plain</p>");
    check(loop_settle() && g.page.scripts!=NULL && !yonder_scripts_listens(g.page.scripts,"click"),
        "loop: a page with no handler hears nothing, and makes no runtime to find that out");
    loop_drop();
}
static void loop_state_adopted(void) {
    loop_page("<input id=field value=markup><script>document.getElementById('field').value='set mid-parse';</script>"
        "<p>after</p>");
    check(loop_settle() && g.page.tree!=NULL && probe_field("field")!=NULL &&
        os64_streq(probe_field("field")->text,"set mid-parse"),
        "loop: a value a script set mid-parse is the one the arrived page's control shows");
    loop_drop();
}
static void loop_typed_value(void) {
    /* A tick that redraws before the person types, and one after. */
    loop_page("<input id=field value=><p id=out>-</p><script>var held=document.getElementById('field'),left=2;"
        "function tick(){if(left-->0){document.getElementById('out').textContent='wait '+left;setTimeout(tick,1000);return}"
        "document.getElementById('out').textContent='field='+held.value}tick();</script>");
    check(loop_settle() && g.page.tree!=NULL,"loop: a page with a field and a timer arrives");
    now_ms+=1000; script_turn();
    FormWidget *field=probe_field("field");
    os64_ui_set_focus(&g.ui,field->w); inputs_run();
    input_key('Q');
    now_ms+=1000; script_turn();
    now_ms+=1000; script_turn();
    check(probe_text_is("out","field=Q"),"loop: a timer reads what a person typed into an arrived page's field");
    loop_drop();
}
/* ── DOCUMENT.WRITE (docs/design/pending/DOM_D9.md) ─────────────────── */
/* An element of the page still arriving, by id. */
static const os64_html_node_t *stream_id(const char *id) {
    const os64_html_document_t *doc=os64_html_parser_document(g.stream.parser);
    for (const os64_html_node_t *n=doc->document;n;n=next_within(n,doc->document)) {
        const os64_html_attr_t *a=os64_html_attr(n,"id");
        if(a && os64_streq(a->value,id)) return n;
    }
    return NULL;
}
static void loop_write_order(void) {
    loop_page("<html><head><script>document.write('<p id=written>from the head</p>');</script></head>"
        "<body><p id=own>the body's own</p></body></html>");
    check(loop_settle() && g.page.tree!=NULL,"write: a page whose head script writes arrives");
    const os64_html_node_t *written=probe_id("written"),*own=probe_id("own");
    check(written && own && written->parent==own->parent && written->next==own,
        "write: the written paragraph is in the arrived tree, before the body's own");
    loop_drop();
    loop_page("<p id=out>start</p><script>document.write('<script>document.write(\"<b id=inner>inner</b>\")</scr'+'ipt>after');"
        "document.write(' and later');</script><i id=tail>tail</i>");
    check(loop_settle() && g.page.tree!=NULL,"write: a page whose written script writes arrives");
    const os64_html_node_t *inner=probe_id("inner"),*tail=probe_id("tail");
    check(inner && tail && inner->next && inner->next->kind==OS64_HTML_TEXT &&
        !strcmp(inner->next->text,"after and later") && inner->next->next==tail,
        "write: the written script's text lands before the text after it, the writer's later text behind");
    loop_drop();
}
static void loop_write_src(void) {
    loop_page("<p id=out>start</p><script>document.write('<script src=\"written.js\"></scr'+'ipt>');</script>"
        "<p id=after>after</p>");
    check(loop_settle() && g.stream.stopped && nscript_jobs==1 && stream_id("after")==NULL,
        "write: a written src script is fetched, and the parse waits for it");
    check(script_land("/written.js","document.getElementById('out').textContent='ran '+(document.getElementById('after')===null)"),
        "write: its fetch was out");
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","ran true"),
        "write: it runs before the parser goes on");
    loop_drop();
}
static void loop_write_refused(void) {
    /* A timer of the arriving page has no insertion point. */
    loop_page("<p id=out>-</p><script>setTimeout(function(){try{document.write('<b>timer</b>');"
        "document.getElementById('out').textContent='wrote'}catch(e){document.getElementById('out').textContent=e.name}},0);</script>"
        "<script src=slow.js></script><p id=tail>tail</p>");
    check(loop_settle() && g.stream.stopped,"write: stopped at a script whose fetch is out");
    now_ms+=10;
    loop_settle();
    check(script_land("/slow.js","1") && loop_settle() && g.page.tree!=NULL,"write: the page arrives");
    check(probe_text_is("out","InvalidStateError") && probe_id("tail")!=NULL,
        "write: from a timer of the arriving page, refused, and the parse unharmed");
    loop_drop();
    /* The shown page has no parser at all. */
    os64_js_outcome_t out;
    os64_dom_event_t click={.type="click",.bubbles=true,.cancelable=true};
    probe_page("<p id=late>-</p><script>window.onclick=function(){try{document.write('x')}catch(e){"
        "document.getElementById('late').textContent=e.name}}</script>",true);
    script_turn();
    yonder_scripts_dispatch(g.page.scripts,NULL,&click,NULL,&out);
    script_rebuild();
    check(probe_text_is("late","InvalidStateError"),"write: from the shown page, refused");
    probe_drop();
}
static void loop_write_cut(void) {
    loop_page("<p id=out>before</p><script>document.write('<div>'.repeat(600)+'deep');</script><p>tail</p>");
    check(loop_settle() && g.page.tree!=NULL,"write: a write past the depth limit still lets the page arrive");
    check(probe_text_is("out","before") && strstr(g.status_text,"bigger than this browser will parse (OS64_HTML_TOO_DEEP)")!=NULL,
        "write: the page arrives as far as it came, with the parser's sentence");
    loop_drop();
}
static void loop_write_audit(void) {
    s_script_audit=true;
    loop_page("<script>document.write('<p>12345</p>');</script>");
    stream_turn();
    stream_turn();
    check(strstr(last_debug,"wrote 12 bytes")!=NULL,"write: the audit line counts what a task wrote");
    loop_settle();
    s_script_audit=false;
    loop_drop();
}
static void loop_write_teardown(void) {
    /* Written text pending behind a written script whose fetch never comes. */
    const char *page="<script>document.write('<script src=\"never.js\"></scr'+'ipt><p>pending</p>');"
        "document.write('<p>more pending</p>');</script><p>x</p>";
    for(int how=0;how<3;how++) {
        loop_page(page);
        check(loop_settle() && g.stream.stopped && nscript_jobs==1 && stream_id("x")==NULL,
            "write: stopped at a written script, with written text pending behind it");
        if(how==0) click_stop(NULL,NULL);
        else if(how==1) settings_use(YONDER_AGENT,false,5);
        if(how<2) check(!g.stream.active,"write: the page is let go with its written text");
        loop_drop();
    }
}
static void loop_write_cases(void) {
    loop_write_order();
    loop_write_src();
    loop_write_refused();
    loop_write_cut();
    loop_write_audit();
    loop_write_teardown();
}
static void loop_downgrade(void) {
    probe_page("<p id=out>secure</p><script>location.href='http://insecure.test/';</script>",true);
    script_turn();
    check(g.asker==ASK_REQUEST && !status_says("background workers"),
        "loop: a script cannot take a person from https to http without the question bar");
    bar_forget();
    probe_drop();
}
// What a script changed is what the page uses: a src resolves against the
// document's base, a DOMContentLoaded edit reaches the arriving model, and
// an image's default action finds the image again after its listener.
static void loop_script_changes_hold(void) {
    loop_page("<base href='http://assets.test/js/'><script src='app.js'></script>");
    check(loop_settle() && nscript_jobs==1,"review base: source fetch submitted");
    check(nscript_jobs==1 && os64_streq(script_jobs[0].job->url,"http://assets.test/js/app.js"),
        "review base: source resolves against document base");
    loop_drop();
    loop_page("<form id=f action=/send></form><script>"
        "document.addEventListener('DOMContentLoaded',()=>{var i=document.createElement('input');"
        "i.id='late';document.getElementById('f').appendChild(i)});</script>");
    check(loop_settle() && g.page.tree!=NULL && probe_id("late")!=NULL,
        "review DCL: handler inserted control before arrival");
    check(os64_page_ncontrols(page_model(&g.page))==1 && probe_field("late")!=NULL,
        "review DCL: arrived model includes lifecycle mutation");
    loop_drop();
    probe_page("<form id=f action=/send><input id=pic type=image name=pic></form><script>"
        "var p=document.getElementById('pic');p.onclick=()=>{var i=document.createElement('input');"
        "i.id='first';document.getElementById('f').insertBefore(i,p)};</script>",true);
    script_turn();
    pool_open=true;g.pool=(os64_work_pool_t *)&pool_fake;
    input_queue(IN_CLICK,probe_id("pic"),NULL,0,0,false);inputs_run();
    check(g.nav.id!=0,"review image click: original image submits after control renumbering");
    stop_trip();pool_open=false;g.pool=NULL;probe_drop();
}
// What arrives is what the scripts left: a style a DOMContentLoaded listener
// or a waiting page's timer added is in the first paint, and a local page's
// script loads from beside it.
static void loop_arrival_follows_scripts(void) {
    loop_page("<p id=styled>visible</p><script>document.addEventListener('DOMContentLoaded',()=>{"
        "var s=document.createElement('style');s.textContent='#styled{display:none}';"
        "document.head.appendChild(s)});</script>");
    check(loop_settle() && g.page.tree!=NULL && os64_page_nsheets(page_model(&g.page))==1,
        "arrival: refreshed model includes lifecycle stylesheet");
    check(flow_box_for(g.page.tree,probe_id("styled"))==NULL,
        "arrival: lifecycle stylesheet applied before publication");
    loop_drop();
    stream_window();g.scripts_on=true;g.script_ms=5000;
    slurp_path="/pages/local.html";slurp_bytes="<script src='app.js'></script>";
    open_address("/pages/local.html",NAV_GO,NULL,NULL);
    check(loop_settle() && nscript_jobs==1 && os64_streq(script_jobs[0].job->url,"file:///pages/app.js"),
        "local page: relative external script beside local page fetches");
    slurp_path=NULL;loop_drop();
    /* The sibling: a timer edits the page while it waits for a sheet, and
     * the page shown takes its sheet table from the tree it has by then. */
    loop_page("<link rel=stylesheet href=wait.css><p id=styled>visible</p><script>"
        "setTimeout(()=>{var s=document.createElement('style');s.textContent='#styled{display:none}';"
        "document.head.appendChild(s)},10);</script>");
    check(loop_settle() && g.coming.active && g.coming.page.sheets_waiting==1,
        "coming page: waits for its linked sheet");
    uint64_t waited_serial=g.coming.page.serial;
    now_ms+=50;
    check(loop_settle() && g.coming.active &&
          os64_html_version(page_doc(&g.coming.page))!=g.coming.page.model_version,
        "coming page: its timer edited the tree while it waited");
    coming_show();
    check(g.page.tree!=NULL && os64_page_nsheets(page_model(&g.page))==2 && g.page.serial!=waited_serial,
        "coming page: shown with a sheet table restaged from the edited tree");
    check(probe_id("styled")!=NULL && flow_box_for(g.page.tree,probe_id("styled"))==NULL,
        "coming page: the timer's style applies to the first paint");
    loop_drop();
}
static void loop_cases(void) {
    loop_arrival_follows_scripts();
    loop_script_changes_hold();
    loop_state_adopted();
    loop_typed_value();
    loop_downgrade();
    loop_handlers_only();
    loop_order();
    loop_fetch_failed();
    loop_overrun();
    loop_navigates();
    loop_teardown();
    loop_limit();
    loop_local_file();
    loop_write_cases();
}
static void stream_cases(void) {
    stream_captures_mode();
    stream_slices();
    stream_reaped_first();
    stream_text();
    stream_no_page();
    stream_refused();
    stream_stopped();
    stream_pool_breaks();
    stream_keeps_the_form();
    stream_mail_wiring();
    loop_cases();
}

int main(void)
{
    os64_text_options_t options={.memory={.alloc=probe_alloc,.free=probe_free},
        .backend=flow_test_backend(),.memory_cap=8*1024*1024};
    check(os64_text_create(&options,&probe_text)==OS64_FONT_OK,"text context");
    check(os64_text_font_bitmap(probe_text,&probe_font)==OS64_FONT_OK,"bitmap face");
    script_types();
    identity_and_edit();
    scripted_zoom();
    scripted_face_budget();
    property_only();
    script_lifecycle();
    native_refusals();
    switch_cases();
    edit_and_kind_changes();
    password_edits();
    reset_editors();
    noscript_author_styles();
    reclaim_browser_holders();
    snapshot_actions();
    bounded_resources();
    stylesheet_reuse();
    input_cases();
    if(getenv("YONDER_NO_STREAM")==NULL) stream_cases();
    os64_text_font_release(probe_font);
    check(os64_text_destroy(probe_text)==OS64_FONT_OK,"all layout text released");
    check(live==0,"native and engine heap is empty");
    printf("Yonder scripted-page host: %u checks, %u failed\n",checks,failures);
    return failures?1:0;
}
