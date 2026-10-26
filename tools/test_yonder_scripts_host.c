/* Exercise the actual browser rendering/control/queue code. Only OS services
 * and the font backend are hosted; libui caret/edit code is linked for real. */
#define main js_port_fixture_main
#define os64_write port_os64_write
#include "test_js_port_host.c"
#undef os64_write
#undef main
#define main yonder_fixture_main
#include "../userland/apps/yonder/yonder.c"
#include "paint_cache.h"
#undef main
#include "test_libflow_fonts.h"
#include "../userland/libhtml/internal.h"
#include "os64/conf.h"
#include "os64/procfs.h"
#include "os64/js_engine.h"
os64_js_status_t __real_os64_js_create_with_teardown(const os64_js_config_t *,
    os64_js_teardown_policy_t, const char *, os64_js_runtime_t **, os64_js_outcome_t *);
#define s settings_fixture
#include "../userland/apps/yonder/settings.c"
#undef s
#include "yonder_host_menus.h"

static yonder_diag_t *diag_new_claimed(const char *url, uint32_t seq);

/* THE DISK, FAKED, for the page files (YONDER_DIAGNOSTICS.md): directories
 * and files in memory, reached only under a directory a case made or
 * seeded. Every other path opens nothing, as the window's fixtures always
 * had it. Handles from FAKE_HANDLE up are the fake's: written whole by
 * `w` and `x`, read from the start by `r`. */
#define FAKE_HANDLE 1000
#define FAKE_FILES 128
/* A file's modification time is the fake second it was last published in
 * (fake_second, which a case moves), as a real disk's is in whole
 * seconds. */
static struct { char path[256]; char *data; size_t len, read_at; bool used; uint64_t mtime; } fake_files[FAKE_FILES];
static char fake_dirs[16][256];
static int nfake_dirs, fake_writes, fake_dir_at;
static bool fake_fail_open;
/* A disk that writes a byte a call, or refuses a write, a sync or a
 * rename. */
static bool fake_short_write, fake_fail_write, fake_fail_sync, fake_fail_rename;
/* A disk error on the file at this path (NULL for none): its writes, or
 * its close, fail. */
static const char *fake_fail_write_at, *fake_fail_close_at;
/* The task this window is, and the tasks that are running (a temp's
 * owner is asked after through os64_proc_read); fake_live_zombie makes the
 * first of them a zombie. */
static uint64_t fake_task = 7, fake_live[4];
static bool fake_live_zombie;
/* fake_sync_hook runs once, at the next sync: another window's write
 * landing between this one's read and its rename. */
static void (*fake_sync_hook)(void);
static uint64_t fake_second = 1;
static const char *fake_parent(const char *path, char *out, size_t cap) {
    snprintf(out,cap,"%s",path); char *slash=strrchr(out,'/');
    if(slash==NULL) return NULL;
    if(slash==out) slash[1]='\0'; else *slash='\0';
    return out;
}
static bool fake_is_dir(const char *path) {
    for(int i=0;i<nfake_dirs;i++) if(!strcmp(fake_dirs[i],path)) return true;
    return false;
}
static int fake_file(const char *path) {
    for(int i=0;i<FAKE_FILES;i++) if(fake_files[i].used && !strcmp(fake_files[i].path,path)) return i;
    return -1;
}
static void fake_reset(void) {
    for(int i=0;i<FAKE_FILES;i++) free(fake_files[i].data);
    memset(fake_files,0,sizeof(fake_files)); nfake_dirs=0; fake_writes=0; fake_fail_open=false;
    fake_short_write=fake_fail_write=fake_fail_sync=fake_fail_rename=false;
    fake_task=7; memset(fake_live,0,sizeof(fake_live)); fake_live_zombie=false; fake_sync_hook=NULL;
    fake_second=1;
    fake_fail_write_at=fake_fail_close_at=NULL;
    snprintf(fake_dirs[nfake_dirs++],256,"/tmp");
}
static const char *fake_text(const char *path) {
    int f=fake_file(path); return f>=0 ? fake_files[f].data : NULL;
}
int64_t os64_open(const char *p, const char *m) {
    char parent[256];
    if(fake_fail_open || fake_parent(p,parent,sizeof(parent))==NULL || !fake_is_dir(parent) ||
       (strcmp(m,"w") && strcmp(m,"x") && strcmp(m,"r"))) return -1;
    int f=fake_file(p);
    if(!strcmp(m,"r")) {
        if(f<0) return -1;
        fake_files[f].read_at=0;
        return FAKE_HANDLE+f;
    }
    if(f>=0 && !strcmp(m,"x")) return -1;
    if(f<0) for(f=0;f<FAKE_FILES && fake_files[f].used;f++) {}
    if(f==FAKE_FILES) return -1;
    free(fake_files[f].data);
    fake_files[f].used=true; snprintf(fake_files[f].path,256,"%s",p);
    fake_files[f].data=calloc(1,1); fake_files[f].len=0;
    return FAKE_HANDLE+f;
}
int64_t os64_write(int32_t h, const void *b, size_t n) {
    if(h<FAKE_HANDLE) return port_os64_write(h,b,n);
    int f=h-FAKE_HANDLE;
    if(fake_fail_write) return -1;
    if(fake_fail_write_at!=NULL && !strcmp(fake_files[f].path,fake_fail_write_at)) return -1;
    if(fake_short_write && n>1) n=1;
    fake_files[f].data=realloc(fake_files[f].data,fake_files[f].len+n+1);
    memcpy(fake_files[f].data+fake_files[f].len,b,n); fake_files[f].len+=n;
    fake_files[f].data[fake_files[f].len]='\0';
    return (int64_t)n;
}
int64_t os64_read(int32_t h, void *b, size_t n) {
    if(h<FAKE_HANDLE || h>=FAKE_HANDLE+FAKE_FILES || !fake_files[h-FAKE_HANDLE].used) return -1;
    int f=h-FAKE_HANDLE;
    size_t left=fake_files[f].len-fake_files[f].read_at, take=n<left ? n : left;
    memcpy(b,fake_files[f].data+fake_files[f].read_at,take);
    fake_files[f].read_at+=take;
    return (int64_t)take;
}
int64_t os64_close(int32_t h) {
    int f=h-FAKE_HANDLE;
    return f>=0 && f<FAKE_FILES && fake_files[f].used && fake_fail_close_at!=NULL &&
           !strcmp(fake_files[f].path,fake_fail_close_at) ? -1 : 0;
}
int64_t os64_rename(const char *from, const char *to) {
    int f=fake_file(from);
    if(f<0 || fake_fail_rename) return -1;
    int old=fake_file(to);
    if(old>=0) { free(fake_files[old].data); memset(&fake_files[old],0,sizeof(fake_files[old])); }
    snprintf(fake_files[f].path,256,"%s",to);
    fake_files[f].mtime=fake_second;
    fake_writes++;
    return 0;
}
int64_t os64_sync(int32_t h) {
    (void)h;
    void (*hook)(void)=fake_sync_hook;
    fake_sync_hook=NULL;
    if(hook!=NULL) hook();
    return fake_fail_sync ? -1 : 0;
}
/* A file, or a directory with nothing in it, as os64_unlink removes either. */
int64_t os64_unlink(const char *p) {
    int f=fake_file(p);
    if(f<0) {
        char parent[256];
        for(int i=0;i<FAKE_FILES;i++)
            if(fake_files[i].used && fake_parent(fake_files[i].path,parent,sizeof(parent)) && !strcmp(parent,p))
                return -1;
        for(int i=0;i<nfake_dirs;i++) if(!strcmp(fake_dirs[i],p)) {
            memmove(fake_dirs[i],fake_dirs[i+1],(size_t)(nfake_dirs-i-1)*sizeof(fake_dirs[0])); nfake_dirs--; return 0;
        }
        return -1;
    }
    free(fake_files[f].data); memset(&fake_files[f],0,sizeof(fake_files[f])); return 0;
}
int64_t os64_mkdir(const char *p) {
    char parent[256];
    if(fake_is_dir(p) || fake_parent(p,parent,sizeof(parent))==NULL || !fake_is_dir(parent) || nfake_dirs==16)
        return -1;
    snprintf(fake_dirs[nfake_dirs++],256,"%s",p); return 0;
}
int64_t os64_stat(const char *p, os64_dirent_t *entry) {
    memset(entry,0,sizeof(*entry));
    if(fake_is_dir(p)) { entry->flags=OS64_DE_DIR; return 0; }
    int f=fake_file(p);
    if(f<0) return -1;
    entry->size=fake_files[f].len; entry->mtime=fake_files[f].mtime;
    return 0;
}
/* The window's paint asks the machine's memory when it makes its cache;
 * no case here paints the window's view, so the answer is never used. */
int64_t os64_memory(os64_memory_t *out) { memset(out,0,sizeof(*out)); return -1; }
/* A directory's listing: one directory open at a time, which is all the
 * window ever asks for. */
static char fake_listing[256];
int64_t os64_opendir(const char *p) {
    if(!fake_is_dir(p)) return -1;
    snprintf(fake_listing,sizeof(fake_listing),"%s",p); fake_dir_at=0; return FAKE_HANDLE-1;
}
int64_t os64_readdir(int32_t h, os64_dirent_t *entry) {
    (void)h;
    for(; fake_dir_at<FAKE_FILES; fake_dir_at++) {
        char parent[256];
        if(!fake_files[fake_dir_at].used ||
           strcmp(fake_parent(fake_files[fake_dir_at].path,parent,sizeof(parent)),fake_listing)) continue;
        memset(entry,0,sizeof(*entry));
        snprintf(entry->name,sizeof(entry->name),"%s",strrchr(fake_files[fake_dir_at].path,'/')+1);
        fake_dir_at++;
        return 1;
    }
    return 0;
}
uint64_t os64_taskid(void) { return fake_task; }
int32_t os64_proc_read(uint64_t pid, os64_proc_info_t *out) {
    memset(out,0,sizeof(*out));
    for(int i=0;i<4;i++) if(fake_live[i]!=0 && fake_live[i]==pid) {
        out->pid=pid; out->state=i==0 && fake_live_zombie ? OS64_PROC_ZOMBIE : OS64_PROC_ISLEEP;
        return 0;
    }
    return pid==fake_task ? (out->pid=pid, out->state=OS64_PROC_RUNNING, 0) : -1;
}
/* The clocks stand still unless a case moves them: micros steps by
 * clock_step at every read (an overrun is a loop that reads it) and by
 * geometry_layout_delay at every forced layout (a slow native layout), and
 * the window's milliseconds are what a case sets. */
static int64_t clock_us=10000000, clock_step, geometry_layout_delay;
static bool geometry_refuse_layout;
int64_t os64_micros(void) { return clock_us+=clock_step; }
/* THE SYSTEM'S ENTROPY, faked (libtls's door, which crypto.getRandomValues
 * reads through scripts.c): a fixed sequence from a fixed seed, so a page
 * that makes ids from it replays the same every run (REPLAY.md § 3), and
 * fake_entropy_fail refuses every ask, as a /dev/random that cannot be
 * read does. */
static uint64_t fake_entropy_state=0x9E3779B97F4A7C15ull;
static bool fake_entropy_fail;
bool os64_tls_entropy(void *out, size_t length) {
    if(fake_entropy_fail) return false;
    uint8_t *b=out;
    for(size_t i=0;i<length;i++) {
        fake_entropy_state^=fake_entropy_state<<13; fake_entropy_state^=fake_entropy_state>>7;
        fake_entropy_state^=fake_entropy_state<<17;
        b[i]=(uint8_t)(fake_entropy_state>>24);
    }
    return true;
}
uint64_t os64_heap_verify(void) { return 0; }
static char last_debug[256];
static size_t teardown_log_count;
static char teardown_log[OS64_JS_SOURCE_NAME_CAP + 128];
/* Every audit line a case asks to keep, one per line. */
static char audit_lines[16384];
static bool audit_keep;
void os64_debug_log(const char *text) {
    snprintf(last_debug,sizeof(last_debug),"%s",text);
    if (audit_keep) {
        size_t at=strlen(audit_lines);
        snprintf(audit_lines+at,sizeof(audit_lines)-at,"%s\n",text);
    }
    if (strstr(text, "reclaimed JavaScript teardown leak") != NULL) {
        teardown_log_count++;
        os64_strcopy(teardown_log, sizeof(teardown_log), text);
    }
}
void *os64_calloc(size_t n, size_t z) {
    if (z && n > SIZE_MAX/z) return NULL;
    void *p = os64_malloc(n*z);
    if (p) memset(p,0,n*z);
    return p;
}
/* libfetch, faked by its contract for the fetch job (yonder/fetch_job.c),
 * which runs here for real: the cases and the WPT runner say what each
 * address answers, and read back which requests a page made. */
#include "test_fetch_fake.inc"
/* The jar's and the preflight cache's locks yield under contention; this
 * host's one thread never contends. */
void os64_yield(void) {}
const os64_font_backend_t *os64_freetype_backend_v1(void) { return flow_test_backend(); }
void os64_ui_theme_current(os64_ui_theme_t *theme, uint64_t *installed) {
    os64_ui_theme_defaults(theme); *installed=0;
}
int64_t os64_ticks(os64_ticks_t *ticks) { *ticks=(os64_ticks_t){.ticks=100,.per_second=1000}; return 0; }
static uint64_t now_ms=100;
uint64_t yonder_now_ms(void) { return now_ms; }
/* The ticker's deadline, as the window last handed it over. */
static uint64_t ticker_due=YONDER_NEVER;
void yonder_ticker_set(yonder_ticker_t *ticker, uint64_t due) { (void)ticker; ticker_due=due; }
/* The ticker's counts, as a running ticker would give them. */
static yonder_ticker_counts_t ticker_counts;
yonder_ticker_counts_t yonder_ticker_counts(yonder_ticker_t *ticker) { return ticker!=NULL ? ticker_counts : (yonder_ticker_counts_t){0}; }
void yonder_ticker_counts_restart(yonder_ticker_t *ticker) { (void)ticker; }
/* THE POOL, FAKED. A stream case (below) lets one navigation be submitted
 * and plays its worker itself; every other case still treats a submission
 * as a mistake. */
static bool pool_open;
static os64_work_t pool_work;
static os64_work_id_t pool_ids, pool_cancelled;
/* The id the one held job (pool_work) was given: other jobs draw ids from
 * the same count, so the newest id need not be its. */
static os64_work_id_t pool_work_id;
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
static unsigned sheet_submits;      /* every sheet job the window ever sent */
/* A script's request (fetch, XMLHttpRequest) too: held until a case runs
 * it, the real fetch job against the fake libfetch (request_land). */
static struct { os64_work_id_t id; yonder_fetch_job_t *job; } request_jobs[64];
static int nrequest_jobs;
static unsigned request_cancels;
/* A font file's job too: held until a case answers it (font_land). */
static struct { os64_work_id_t id; yonder_webfont_job_t *job; } font_jobs[32];
static int nfont_jobs;
static unsigned font_cancels;
static bool font_refuse;            /* the pool will not take a font job */
/* The next opens of a font refused for room, as a full budget does. */
static int font_open_refusals;
/* The next opens of another size refused for room, likewise. */
static int font_like_refusals;
static os64_font_status_t font_like_status=OS64_FONT_LIMIT;   /* what a refusal says */
static bool font_like_refuse_full;  /* refused for room while the handle table is full */
os64_font_status_t __real_os64_text_font_open_like(const os64_text_font_t *, const os64_font_face_options_t *,
                                                   const os64_text_range_t *, size_t, os64_text_font_t **);
os64_font_status_t __wrap_os64_text_font_open_like(const os64_text_font_t *like, const os64_font_face_options_t *o,
                                                   const os64_text_range_t *r, size_t n, os64_text_font_t **out) {
    if(font_like_refusals>0) { font_like_refusals--; *out=NULL; return font_like_status; }
    if(font_like_refuse_full && s_web.nhandles==WEBFONT_HANDLES_MAX) { *out=NULL; return OS64_FONT_LIMIT; }
    return __real_os64_text_font_open_like(like,o,r,n,out);
}
os64_font_status_t __real_os64_text_font_open(os64_text_context_t *, const uint8_t *, size_t,
                                              const os64_font_face_options_t *, os64_text_font_t **);
os64_font_status_t __wrap_os64_text_font_open(os64_text_context_t *c, const uint8_t *bytes, size_t len,
                                              const os64_font_face_options_t *o, os64_text_font_t **out) {
    if(font_open_refusals>0) { font_open_refusals--; *out=NULL; return OS64_FONT_LIMIT; }
    return __real_os64_text_font_open(c,bytes,len,o,out);
}
/* A program that includes this file to drive pages of its own (the replay
 * runner, tools/test_yonder_replay_host.c) may run the pool itself: when
 * set, the first takes every submission, and the second every cancellation
 * of a job it holds, answering whether it did. */
static os64_work_id_t (*pool_submit_hook)(const os64_work_t *work);
static bool (*pool_cancel_hook)(os64_work_id_t id);
void os64_work_cancel(os64_work_pool_t *pool, os64_work_id_t id) {
    (void)pool;
    if(pool_cancel_hook!=NULL && pool_cancel_hook(id)) return;
    for(int i=0;i<nscript_jobs;i++) if(script_jobs[i].id==id) {
        os64_free(script_jobs[i].job); script_jobs[i]=script_jobs[--nscript_jobs]; script_cancels++; return;
    }
    for(int i=0;i<nsheet_jobs;i++) if(sheet_jobs[i].id==id) {
        os64_free(sheet_jobs[i].job); sheet_jobs[i]=sheet_jobs[--nsheet_jobs]; return;
    }
    for(int i=0;i<nfont_jobs;i++) if(font_jobs[i].id==id) {
        os64_free(font_jobs[i].job); font_jobs[i]=font_jobs[--nfont_jobs]; font_cancels++; return;
    }
    for(int i=0;i<nrequest_jobs;i++) if(request_jobs[i].id==id) {
        yonder_fetch_release(request_jobs[i].job,NULL); request_jobs[i]=request_jobs[--nrequest_jobs];
        request_cancels++; return;
    }
    pool_cancelled=id;
    if(id==pool_work_id && pool_work.job!=NULL) {
        pool_work.release(pool_work.job,NULL); memset(&pool_work,0,sizeof(pool_work));
    }
}
os64_work_id_t os64_work_submit(os64_work_pool_t *pool, const os64_work_t *work) {
    (void)pool;
    if(!pool_open) { check(false,"unexpected background submission"); return 0; }
    if(pool_submit_hook!=NULL) return pool_submit_hook(work);
    if(*(const uint32_t *)work->job==YONDER_JOB_SCRIPT) {
        if(nscript_jobs==32) return 0;
        script_jobs[nscript_jobs].id=++pool_ids; script_jobs[nscript_jobs].job=work->job;
        return script_jobs[nscript_jobs++].id;
    }
    if(*(const uint32_t *)work->job==YONDER_JOB_SHEET) {
        if(nsheet_jobs==32) return 0;
        sheet_submits++;
        sheet_jobs[nsheet_jobs].id=++pool_ids; sheet_jobs[nsheet_jobs].job=work->job;
        return sheet_jobs[nsheet_jobs++].id;
    }
    if(*(const uint32_t *)work->job==YONDER_JOB_FETCH) {
        if(nrequest_jobs==64) return 0;
        request_jobs[nrequest_jobs].id=++pool_ids; request_jobs[nrequest_jobs].job=work->job;
        return request_jobs[nrequest_jobs++].id;
    }
    if(*(const uint32_t *)work->job==YONDER_JOB_FONT) {
        if(nfont_jobs==32 || font_refuse) return 0;
        font_jobs[nfont_jobs].id=++pool_ids; font_jobs[nfont_jobs].job=work->job;
        return font_jobs[nfont_jobs++].id;
    }
    /* One slot: a picture that comes while another holds it is refused, as
     * a full pool refuses, and yonder frees it; taking it would drop the
     * held job's only reference. */
    if(*(const uint32_t *)work->job==YONDER_JOB_PICTURE && pool_work.job!=NULL &&
       *(const uint32_t *)pool_work.job==YONDER_JOB_PICTURE) return 0;
    pool_work=*work; pool_work_id=++pool_ids; return pool_work_id;
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
int64_t yonder_picture_run(void *job, bool (*cancelled)(void *), void *ctx, void **out) {
    (void)job; (void)cancelled; (void)ctx; (void)out;
    check(false,"unexpected worker execution"); return -1;
}
/* As picture.c releases a picture job and a product with no pixels: a case
 * that hands reaped() a picture makes both with os64_calloc. */
void yonder_picture_release(void *job, void *product) {
    yonder_picture_t *got=product;
    check(got==NULL || (got->image.pixels==NULL && got->sequence==NULL),"a released picture holds no pixels here");
    os64_free(got);
    os64_free(job);
}
int64_t yonder_webfont_run(void *job, bool (*cancelled)(void *), void *ctx, void **out) {
    (void)job; (void)cancelled; (void)ctx; (void)out;
    check(false,"unexpected worker execution"); return -1;
}
/* As webfont.c releases a font job and its SFNT. */
void yonder_webfont_release(void *job, void *product) {
    yonder_webfont_t *got=product;
    if(got) { os64_free(got->sfnt); os64_free(got); }
    os64_free(job);
}
int64_t yonder_sheet_run(void *job, bool (*cancelled)(void *), void *ctx, void **out) {
    (void)job; (void)cancelled; (void)ctx; (void)out;
    check(false,"unexpected worker execution"); return -1;
}
/* As sheet.c releases a sheet job and what it made. */
void yonder_sheet_release(void *job, void *product) {
    yonder_sheet_t *got=product;
    if(got) { garb_free(&got->parsed); os64_free(got->kept); os64_free(got); }
    os64_free(job);
}
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
static bool script_land_from(const char *tail, const char *source, const char *final);
static bool script_land(const char *tail, const char *source) { return script_land_from(tail,source,NULL); }
/* The same, the response having come from `final` after its redirects
 * (NULL: from the address asked for). */
static bool script_land_from(const char *tail, const char *source, const char *final) {
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
        os64_strcopy(got->url,sizeof(got->url),final!=NULL ? final : url);
        reaped(id,job,got);
        return true;
    }
    return false;
}
/* A moving picture a case plays (picture_tick_cases): its frame, and how
 * many times it was advanced. No other case has one. */
struct os64_image_sequence { os64_image_frame_t frame; uint32_t nexts; };
os64_image_status_t os64_image_sequence_next(os64_image_sequence_t *seq) {
    if(seq==NULL) { check(false,"no moving picture"); return OS64_IMAGE_OK; }
    seq->nexts++; return OS64_IMAGE_OK;
}
void os64_image_free(os64_image_t *image) { os64_free(image->pixels); memset(image,0,sizeof(*image)); }
void os64_image_sequence_free(os64_image_sequence_t *sequence) { check(sequence==NULL,"no unexpected moving image"); }
const os64_image_frame_t *os64_image_sequence_frame(const os64_image_sequence_t *sequence) {
    if(sequence==NULL) { check(false,"unexpected animation read"); return NULL; }
    return &sequence->frame;
}
/* A file a case serves (open_local), or none: `slurp_length` bytes, or a
 * string's when that is zero; and the fake disk's files (page storage
 * reads its stores this way), whole. fake_slurp_fail makes every read of
 * a fake file fail, as a disk that cannot be read does; reading the case's
 * file takes slurp_us on the clock. */
static const char *slurp_path, *slurp_bytes;
static size_t slurp_length;
static int64_t slurp_us;
static bool fake_slurp_fail;
os64_slurp_status_t os64_slurp(const char *path, size_t cap, uint8_t **bytes, size_t *length) {
    *bytes=NULL; *length=0;
    int f=fake_file(path);
    if(f>=0) {
        if(fake_slurp_fail) return OS64_SLURP_IO_ERROR;
        if(fake_files[f].len>cap) return OS64_SLURP_TOO_BIG;
        *bytes=os64_malloc(fake_files[f].len+1);
        if(*bytes==NULL) return OS64_SLURP_NO_MEMORY;
        *length=fake_files[f].len;
        memcpy(*bytes,fake_files[f].data,*length); (*bytes)[*length]=0;
        return OS64_SLURP_OK;
    }
    if(slurp_path==NULL || !os64_streq(path,slurp_path)) return OS64_SLURP_NO_FILE;
    clock_us+=slurp_us;
    *length=slurp_length!=0 ? slurp_length : strlen(slurp_bytes);
    *bytes=os64_malloc(*length+1); memcpy(*bytes,slurp_bytes,*length); (*bytes)[*length]=0;
    return OS64_SLURP_OK;
}

bool os64_ui_theme_session(os64_ui_theme_t *theme, uint64_t *installed, uint64_t hint) {
    (void)theme; (void)installed; (void)hint; return false;
}
static char saved_scripts[8], saved_zoom[8], saved_seconds[8], saved_diagnostics[256], saved_appearance[8],
    saved_animations[8], saved_motion[16], saved_keep_pages[16], saved_modern[16], saved_profile[300], saved_cookies[16],
    settings_report[256];
/* yonder.conf's saved agent, "" for none. */
static char saved_agent[300];
/* What reading yonder.conf's profile answers instead, when not 0. */
static int64_t profile_conf_failure;
int64_t os64_conf_get(const char *file, const char *key, char *out, size_t cap) {
    check(os64_streq(file,"yonder.conf"),"settings read Yonder configuration");
    const char *value=os64_streq(key,"scripts") ? saved_scripts :
        os64_streq(key,"zoom") ? saved_zoom : os64_streq(key,"script_seconds") ? saved_seconds :
        os64_streq(key,"diagnostics") ? saved_diagnostics : os64_streq(key,"appearance") ? saved_appearance :
        os64_streq(key,"animations") ? saved_animations : os64_streq(key,"motion") ? saved_motion :
        os64_streq(key,"keep_pages") ? saved_keep_pages : os64_streq(key,"modern") ? saved_modern :
        os64_streq(key,"profile") ? saved_profile : os64_streq(key,"agent") ? saved_agent :
        os64_streq(key,"cookies") ? saved_cookies : "";
    if(os64_streq(key,"profile") && profile_conf_failure!=0) return profile_conf_failure;
    if(!value[0]) return OS64_CONF_NO_KEY;
    /* A value too long for `out` is TRUNCATED, as the real reader says. */
    return os64_strcopy(out,cap,value)<cap ? 0 : OS64_CONF_TRUNCATED;
}
int64_t os64_conf_set(const char *file, const char *key, const char *value) {
    check(os64_streq(file,"yonder.conf"),"settings save Yonder configuration");
    if(os64_streq(key,"scripts")) os64_strcopy(saved_scripts,sizeof(saved_scripts),value);
    if(os64_streq(key,"zoom")) os64_strcopy(saved_zoom,sizeof(saved_zoom),value);
    if(os64_streq(key,"script_seconds")) os64_strcopy(saved_seconds,sizeof(saved_seconds),value);
    if(os64_streq(key,"appearance")) os64_strcopy(saved_appearance,sizeof(saved_appearance),value);
    if(os64_streq(key,"animations")) os64_strcopy(saved_animations,sizeof(saved_animations),value);
    if(os64_streq(key,"motion")) os64_strcopy(saved_motion,sizeof(saved_motion),value);
    if(os64_streq(key,"modern")) os64_strcopy(saved_modern,sizeof(saved_modern),value);
    if(os64_streq(key,"cookies")) os64_strcopy(saved_cookies,sizeof(saved_cookies),value);
    return 0;
}
void os64_ui_settings_report(os64_ui_settings_t *dialog, const char *text) {
    (void)dialog; os64_strcopy(settings_report,sizeof(settings_report),text);
}
/* document.cookie's door, faked: the jar's rules are libway's and are tested
 * there (test_way_jar.inc); here the window hands the page's address over
 * and what a script reads and sets crosses intact. */
static char fake_cookie_url[256], fake_cookie_set[256];
static int fake_cookie_encrypted=-1;    /* what the window said of the page's transport */
static const char *fake_cookie_read="session=abc; theme=dark";
static bool fake_cookie_short;          /* the jar says the string did not fit, whatever the room */
/* A jar that answers by the page's address and transport, as libway's
 * does (NULL: every page reads fake_cookie_read). */
static const char *(*fake_cookie_answer)(const char *page_url, bool encrypted);
size_t way_script_cookies(way_jar_t *jar, const char *page_url, bool encrypted, char *out, size_t cap, bool *whole) {
    (void)jar; *whole=!fake_cookie_short; snprintf(fake_cookie_url,sizeof(fake_cookie_url),"%s",page_url?page_url:"");
    fake_cookie_encrypted=encrypted;
    return (size_t)snprintf(out,cap,"%s",fake_cookie_answer!=NULL ? fake_cookie_answer(page_url,encrypted) : fake_cookie_read);
}
void way_script_cookie(way_jar_t *jar, const char *page_url, bool encrypted, const char *text, size_t len) {
    (void)jar; snprintf(fake_cookie_url,sizeof(fake_cookie_url),"%s",page_url?page_url:"");
    fake_cookie_encrypted=encrypted;
    snprintf(fake_cookie_set,sizeof(fake_cookie_set),"%.*s",(int)len,text);
}
void way_cache_enable(way_cache_t *cache, bool enabled) { (void)enabled; check(cache==NULL,"no test cache"); }
void way_cache_set_cap(way_cache_t *cache, uint64_t cap) { (void)cap; check(cache==NULL,"no test cache"); }
void way_cache_stats(way_cache_t *cache, way_cache_stats_t *out) { (void)out; check(cache==NULL,"no test cache"); }
const char *way_cache_dir(way_cache_t *cache) { check(cache==NULL,"no test cache"); return ""; }

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
/* A tick refused as one whose lists find no room is (flow_motion_tick). */
static bool tick_refuse;
bool __real_flow_motion_tick(flow_tree_t *,uint64_t,flow_point_t,
    void (*)(void *,const flow_box_t *,os64_gui_rect_t),void *,bool *);
bool __wrap_flow_motion_tick(flow_tree_t *tree,uint64_t now,flow_point_t scroll,
    void (*mark)(void *,const flow_box_t *,os64_gui_rect_t),void *ctx,bool *moved) {
    if(tick_refuse) { if(moved!=NULL) *moved=false; return false; }
    return __real_flow_motion_tick(tree,now,scroll,mark,ctx,moved);
}
flow_tree_t *__wrap_flow_layout(const os64_html_document_t *doc,const os64_page_t *model,
    int32_t width,const flow_env_t *env) {
    // A refusal after reclaiming the old geometry exercises the retry's
    // teardown and recovery, rather than the usual preserve-old-tree path.
    if(budget_active && budget_refuse_retry && g.page.tree==NULL) {
        budget_refuse_retry=false;budget_retry_refusals++;
        return NULL;
    }
    if(geometry_refuse_layout) return NULL;
    flow_tree_t *tree=__real_flow_layout(doc,model,width,env);
    clock_us+=geometry_layout_delay;
    if(budget_active && flow_incomplete(tree)) budget_partial_layouts++;
    return tree;
}
static void *probe_alloc(void *ctx, size_t n) { (void)ctx; return os64_malloc(n); }
static void probe_free(void *ctx, void *p, size_t n) { (void)ctx; (void)n; os64_free(p); }
/* Whether a case's layouts go through yonder's own resolver (page_fonts). */
static bool probe_web;
static os64_font_status_t probe_fonts(void *ctx, const flow_family_list_t *family,
    bool bold, bool italic, uint32_t px, os64_text_font_t *const **fonts, size_t *n,
    os64_font_face_info_t *info) {
    if(probe_web) return page_fonts(ctx,family,bold,italic,px,fonts,n,info);
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
static bool probe_modern_off;  /* the next probe_page's window: the modern switch off */
/* The window's agent's preset lines: not NULL, so a job handed none is told
 * from one handed the window's. */
static const way_identity_t s_window_identity={.hints="sec-ch-ua: \"Window\";v=\"1\"\r\n",.language="en"};
static void probe_page(const char *html, bool scripts) {
    os64_memset(&g,0,sizeof(g));
    g.scripts_on=scripts; g.way.agent=YONDER_AGENT; g.way.identity=&s_window_identity; g.settle_due=YONDER_NEVER;
    g.modern=g.page.modern=!probe_modern_off;
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
            g.page.way.url,false,++g.pages_made,g.page.diag,g.page.modern);
        for (os64_html_node_t *n=g.page.way.doc->document;n;n=(os64_html_node_t *)next_within(n,g.page.way.doc->document))
            if(os64_dom_script_kind(n)==OS64_DOM_SCRIPT_CLASSIC && os64_html_attr(n,"src")==NULL)
                yonder_scripts_connected(g.page.scripts,n);
    }
}
static void probe_drop(void) {
    inputs_drop();
    yonder_scripts_free(g.page.scripts); g.page.scripts=NULL;
    forms_drop(); page_clear(&g.page); os64_ui_font_release(&g.ui);
    way_history_clear(&g.way);
}

static os64_html_node_t *probe_id_in(const os64_html_document_t *doc, const char *id) {
    if(doc==NULL) return NULL;
    for (os64_html_node_t *n=doc->document;n;n=(os64_html_node_t *)next_within(n,doc->document)) {
        const os64_html_attr_t *a=os64_html_attr(n,"id");
        if(a && os64_streq(a->value,id)) return n;
    }
    return NULL;
}
static os64_html_node_t *probe_id(const char *id) { return probe_id_in(g.page.way.doc,id); }
/* Whether `node` carries `name` with exactly `value`. */
static bool attr_is(const os64_html_node_t *node, const char *name, const char *value) {
    const os64_html_attr_t *a=node!=NULL ? os64_html_attr(node,name) : NULL;
    return a!=NULL && os64_streq(a->value,value);
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
        probe_page("<style>#one{font-size:16px}#two{font-size:20px}#three{font-size:24px}"
            "@keyframes fade{to{opacity:0}}.changed{animation:fade 10s linear}</style>"
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
            // The discarded layout started the fade the script's class
            // names; never shown, it took its timeline with it, so the
            // fade starts at the layout that recovers, five seconds on,
            // not half run.
            uint64_t was_now=now_ms;
            now_ms+=5000;
            script_turn();
            check(probe_text_is("three","last") && !yonder_scripts_pending(g.page.scripts),
                "next UI turn recovers a tree before executing the queued script");
            const flow_box_t *faded=g.page.tree!=NULL ? flow_box_for(g.page.tree,probe_id("one")) : NULL;
            check(faded!=NULL && faded->style->opacity==1000,
                "a discarded layout's animations start at the layout that is shown");
            now_ms=was_now;
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
static int settings_dark=-1;
static void settings_dark_capture(bool dark) { settings_dark=dark; }
static int settings_run=-1, settings_reduce=-1, settings_modern=-1;
static void settings_motion_capture(bool run, bool reduce) { settings_run=run; settings_reduce=reduce; }
static void settings_modern_capture(bool modern) { settings_modern=modern; }
static int settings_cookies=-1;
static void settings_cookies_capture(yonder_cookies_mode_t mode) { settings_cookies=(int)mode; }
static const char *settings_cookies_problem_fake(void) { return "Cookies are not kept: /p/cookies is a folder, and yonder keeps them in a file of that name."; }
static const char *settings_cookies_problem_none(void) { return NULL; }
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
    settings_fixture.dark_use=settings_dark_capture;
    settings_fixture.motion_use=settings_motion_capture;
    settings_fixture.modern_use=settings_modern_capture;
    settings_fixture.modern.checked=true;
    strcpy(settings_fixture.zoom_buf,"125");
    saved_appearance[0]=0;
    check(!yonder_settings_saved_dark(),"pages are light unless yonder.conf says dark");
    /* The agent yonder starts with: none saved is yonder's own; a preset's
     * string from an earlier yonder is that preset now, lines and all; a
     * typed one is kept as typed. */
    saved_agent[0]=0;
    check(os64_streq(starting_agent(),YONDER_AGENT),"agent: none saved starts as yonder's own");
    strcpy(saved_agent,"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                       "Chrome/154.0.0.0 Safari/537.36");
    const char *started=starting_agent();
    check(started!=NULL && strstr(started,"Chrome/156.0.0.0")!=NULL && yonder_agent_identity(started)!=NULL,
          "agent: a saved Chrome 154 preset starts as today's Chrome preset, its lines with it");
    strcpy(saved_agent,"Mozilla/3.0 (compatible)");
    started=starting_agent();
    check(started!=NULL && os64_streq(started,"Mozilla/3.0 (compatible)") && yonder_agent_identity(started)==NULL,
          "agent: a typed one starts as typed");
    saved_agent[0]=0;
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
    check(settings_dark==0 && os64_streq(saved_appearance,"light"),"Save as default saves the appearance, light");
    settings_fixture.dark.checked=true;
    apply(&settings_fixture.d,false);
    check(settings_dark==1 && os64_streq(saved_appearance,"light"),"Apply turns this window dark without saving it");
    apply(&settings_fixture.d,true);
    check(os64_streq(saved_appearance,"dark") && yonder_settings_saved_dark(),"Save as default saves dark pages");
    /* The animations' switches: run unless `animations = off`, reduce only
     * on `motion = reduce`; Apply hands both over, Save writes both. */
    saved_animations[0]=saved_motion[0]=0;
    check(yonder_settings_saved_animations() && !yonder_settings_saved_reduce_motion(),
        "motion settings: animations run and motion is not reduced unless yonder.conf says so");
    strcpy(saved_animations,"OFF"); strcpy(saved_motion,"Reduce");
    check(!yonder_settings_saved_animations() && yonder_settings_saved_reduce_motion(),
        "motion settings: `animations = off` and `motion = reduce`, either case");
    strcpy(saved_animations,"bogus"); strcpy(saved_motion,"bogus");
    check(yonder_settings_saved_animations() && !yonder_settings_saved_reduce_motion(),
        "motion settings: any other word is the default");
    saved_animations[0]=saved_motion[0]=0;
    settings_fixture.run.checked=false; settings_fixture.reduce.checked=true;
    apply(&settings_fixture.d,false);
    check(settings_run==0 && settings_reduce==1 && saved_animations[0]==0 && saved_motion[0]==0,
        "motion settings: Apply hands this window both switches without saving them");
    apply(&settings_fixture.d,true);
    check(os64_streq(saved_animations,"off") && os64_streq(saved_motion,"reduce") &&
          !yonder_settings_saved_animations() && yonder_settings_saved_reduce_motion(),
        "motion settings: Save as default writes both");
    settings_fixture.run.checked=true; settings_fixture.reduce.checked=false;
    apply(&settings_fixture.d,true);
    check(settings_run==1 && settings_reduce==0 && os64_streq(saved_animations,"on") &&
          os64_streq(saved_motion,"no-preference") && yonder_settings_saved_animations() &&
          !yonder_settings_saved_reduce_motion(),
        "motion settings: and back");
    /* THE MODERN SWITCH (MODERN_WEB.md, rule 4): absent is on, present is
     * obeyed, anything else is on and said; Apply hands it over, Save
     * writes it. */
    const char *refused="unset";
    saved_modern[0]=0;
    check(yonder_settings_saved_modern(&refused) && refused==NULL,"modern: absent is on, and nothing is said");
    strcpy(saved_modern,"Off");
    check(!yonder_settings_saved_modern(&refused) && refused==NULL,"modern: `modern = off` is obeyed, either case");
    strcpy(saved_modern,"on");
    check(yonder_settings_saved_modern(&refused) && refused==NULL,"modern: `modern = on` is on");
    strcpy(saved_modern,"maybe");
    check(yonder_settings_saved_modern(&refused) && refused!=NULL && strstr(refused,"not on or off")!=NULL,
        "modern: any other word is on, refused loudly for the status line");
    saved_modern[0]=0;
    settings_fixture.modern.checked=false;
    apply(&settings_fixture.d,false);
    check(settings_modern==0 && saved_modern[0]==0 && strstr(settings_report,"modern web off")!=NULL,
        "modern: Apply hands this window the switch without saving it, and says so");
    apply(&settings_fixture.d,true);
    check(os64_streq(saved_modern,"off") && !yonder_settings_saved_modern(NULL),"modern: Save as default writes off");
    settings_fixture.modern.checked=true;
    apply(&settings_fixture.d,true);
    check(settings_modern==1 && os64_streq(saved_modern,"on") && strstr(settings_report,"modern")==NULL,
        "modern: and on");
    /* THE COOKIES' TWO BOXES: Accept and Keep are on, session and off;
     * Apply hands the mode over, Save writes its word; Clear cookies acts
     * at once. */
    settings_fixture.cookies_use=settings_cookies_capture;
    settings_fixture.accept.checked=true; settings_fixture.outlive.checked=false;
    saved_cookies[0]=0;
    apply(&settings_fixture.d,false);
    check(settings_cookies==YONDER_COOKIES_SESSION && saved_cookies[0]==0 &&
          strstr(settings_report,"cookies until yonder closes")!=NULL,
        "cookies: Accept without Keep is session, applied here, said, not saved");
    settings_fixture.accept.checked=false; settings_fixture.outlive.checked=true;
    apply(&settings_fixture.d,true);
    check(settings_cookies==YONDER_COOKIES_OFF && os64_streq(saved_cookies,"off") &&
          strstr(settings_report,"no cookies")!=NULL,"cookies: no Accept is off, whatever Keep says, and Save writes it");
    settings_fixture.accept.checked=true;
    apply(&settings_fixture.d,true);
    check(settings_cookies==YONDER_COOKIES_ON && os64_streq(saved_cookies,"on") && strstr(settings_report,"cookies")==NULL,
        "cookies: both is on, the default, and the line says nothing of it");
    saved_cookies[0]=0;
    /* A cookie file not kept is what the dialog says as it opens, in place
     * of the cache's holdings; with none, the cache's line stands. */
    settings_fixture.cache=NULL;
    yonder_settings_on_cookies_problem(settings_cookies_problem_fake);
    opening_say();
    check(strstr(settings_report,"is a folder")!=NULL,"Settings opens saying why the cookie file is not kept");
    yonder_settings_on_cookies_problem(settings_cookies_problem_none);
    opening_say();
    check(strstr(settings_report,"no memory for a cache")!=NULL,"and with the file kept, what the cache holds");
    yonder_settings_on_cookies_problem(cookies_problem);
    yonder_settings_on_clear_cookies(NULL);
    cookies_cleared(NULL,NULL);
    check(strstr(settings_report,"keeps no cookies to clear")!=NULL,"Clear cookies with no jar says so");
    yonder_settings_on_clear_cookies(cookies_clear);
    cookies_cleared(NULL,NULL);
    check(strstr(settings_report,"Cleared this window's cookies: 0 gone.")!=NULL,
        "Clear cookies with no file clears the window's jar and says so");
    /* THE PROFILE FOLDER: absent is /home/yogi/.yonder; a full path is
     * obeyed without its trailing slashes; anything else is the default,
     * said. */
    char profile[256];
    saved_profile[0]=0;
    check(yonder_settings_saved_profile(profile,sizeof(profile))==NULL && os64_streq(profile,"/home/yogi/.yonder"),
        "profile: absent is the default, and nothing is said");
    strcpy(saved_profile,"/home/me/browser//");
    check(yonder_settings_saved_profile(profile,sizeof(profile))==NULL && os64_streq(profile,"/home/me/browser"),
        "profile: a full path, without its trailing slashes");
    strcpy(saved_profile,"relative/place");
    const char *profile_refused=yonder_settings_saved_profile(profile,sizeof(profile));
    check(profile_refused!=NULL && strstr(profile_refused,"not a folder's full path")!=NULL &&
          os64_streq(profile,"/home/yogi/.yonder"), "profile: anything else is the default, refused loudly");
    saved_profile[0]='/'; memset(saved_profile+1,'p',280); saved_profile[281]=0;
    profile_refused=yonder_settings_saved_profile(profile,sizeof(profile));
    check(profile_refused!=NULL && strstr(profile_refused,"too long for a path")!=NULL &&
          os64_streq(profile,"/home/yogi/.yonder"), "profile: one too long for a path is the default, said so");
    profile_conf_failure=OS64_CONF_NO_MEMORY;
    profile_refused=yonder_settings_saved_profile(profile,sizeof(profile));
    profile_conf_failure=0;
    check(profile_refused!=NULL && strstr(profile_refused,"could not be read")!=NULL &&
          os64_streq(profile,"/home/yogi/.yonder"), "profile: a yonder.conf that cannot be read is the default, said so");
    saved_profile[0]=0;
    /* Clear page storage acts at once and says what it did. */
    yonder_settings_on_clear_storage(NULL);
    cleared(NULL,NULL);
    check(strstr(settings_report,"no page storage to clear")!=NULL,"Clear page storage with nothing to clear says so");
    yonder_settings_on_clear_storage(storage_clear);
    cleared(NULL,NULL);
    check(strstr(settings_report,"Page storage is not kept in this window")!=NULL,
        "Clear page storage in a window that keeps none says so");
    settings_fixture.dark.checked=false;
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

/* THE ANIMATIONS' LAYOUTS (PILE3.md § The animation model): the shown page
 * is laid out again when what the view shows next changes — quietly, held
 * to what its layouts cost, never for a covered window or with animations
 * off — on a timeline it keeps across its layouts. The fake clocks stand
 * still: now_ms is the window's, and a layout costs geometry_layout_delay. */
static int32_t motion_x(const char *id) {
    const flow_box_t *b=flow_box_for(g.page.tree,probe_id(id));
    return b!=NULL ? b->rect.x : -1;
}
static const char kMotionPage[]="<!doctype html><style>body{margin:0}"
    "@keyframes slide{from{margin-left:0}to{margin-left:100px}}"
    "#a{margin-left:20px;width:10px;height:10px;animation:slide 1s linear infinite}"
    "@media (prefers-reduced-motion: reduce){#a{animation:none}}"
    "#far{margin-top:3000px;width:10px;height:10px;animation:slide 1s steps(4) infinite}</style>"
    "<div id=a></div><div id=far></div>";

/* THE TICKS (PILE3.md § Animations in place): what only painting reads
 * moves between layouts, on a deadline of its own, paced and held to the
 * cost rule by its own frames; a layout due by then wins; what a tick
 * repaints is marked, cut to the view; controls inside a frame that moved
 * are placed again; and a scroll's paint shows the ticks at its time. */
static int32_t tick_x(const char *id) {
    const flow_box_t *b=flow_box_for(g.page.tree,probe_id(id));
    return b!=NULL ? flow_box_doc_rect(b,scroll_now()).x : -1;
}
static const char kTickPage[]="<!doctype html><style>body{margin:0}"
    "@keyframes go{to{transform:translateX(100px)}}@keyframes hue{to{background-color:blue}}"
    "#t{width:40px;height:20px;animation:go 1s linear infinite}"
    "#far{margin-top:3000px;width:10px;height:10px;background:red;animation:hue 1s linear infinite}"
    "</style><div id=t><input id=in size=2></div><div id=far></div>";
static void tick_cases(void) {
    bool was_run=s_animations, was_reduce=s_reduce_motion;
    s_animations=true; s_reduce_motion=false;
    clock_step=0; geometry_layout_delay=0;
    now_ms=100;
    probe_page(kTickPage,false);
    relayout(true);
    check(motion_next()==YONDER_NEVER && tick_next()==100 && ticker_due==100,
        "ticks: what only painting reads is due for a tick, not a layout");
    int32_t field_was=g.nfw==1 ? g.fw[0]->w->bounds.x : -1;
    uint64_t laid_began=g.motion_began;
    g.ui.any_dirty=false;
    now_ms=600;
    motion_tick();
    check(tick_x("t")==50 && g.ticked && g.tick_painting && g.tick_began==600 && g.motion_began==laid_began,
        "ticks: a tick moves its element at its moment and lays nothing out");
    check(g.nfw==1 && g.fw[0]->w->bounds.x==field_was+50,
        "ticks: a control inside a frame that moved is placed again");
    check(tick_next()==YONDER_NEVER,"ticks: a tick waiting for its paint is the only frame");
    os64_gui_rect_t d=g.ui.dirty, v=g.view.bounds;
    check(g.ui.any_dirty && d.x<=v.x && d.x+d.w>=v.x+90 && d.w<v.w && d.y+d.h<=v.y+40,
        "ticks: a tick marks where its element was and is, cut to the view, and none of what is out of it");
    clock_us+=3000; now_ms=603;
    motion_painted();
    check(g.tick_us==3000 && g.tick_ended==603 && !g.tick_painting && tick_next()==616,
        "ticks: a tick costs its arithmetic and its paint, and the next comes a frame after it began");
    now_ms=616;
    motion_tick();
    clock_us+=30000; now_ms=646;
    motion_painted();
    check(tick_next()==676,"ticks: a costlier tick waits as long again after it ends");
    /* A tick not taken is a layout instead, which is that tick's frame. */
    tick_refuse=true; geometry_layout_delay=4000; now_ms=676;
    uint64_t laid_was=g.motion_began, for_motion=s_cost.layouts_motion;
    motion_tick();
    tick_refuse=false; geometry_layout_delay=0;
    check(s_cost.layouts_motion==for_motion+1,
        "ticks: the record counts a tick's layout as one for animations");
    clock_us+=2000; now_ms=678;
    motion_painted();
    check(g.motion_began==laid_was && tick_x("t")==58 && g.tick_began==676 && g.tick_us==6000 &&
          tick_next()==692,
        "ticks: a tick not taken lays the page out instead, which with its paint is the tick's frame");
    /* A whole view marked for any reason — a box's scroll among them —
     * brings what it shows to now before it is painted; and a tick before
     * a paint that is not taken lays out instead, as the tick's frame. */
    now_ms=685;
    os64_ui_mark_dirty(&g.ui,&g.view);
    before_paint();
    check(tick_x("t")==59 && !g.tick_painting && g.tick_began==676,
        "ticks: a whole view marked to be painted shows the ticks at its time, and is no tick's frame");
    tick_refuse=true; now_ms=690;
    os64_ui_mark_dirty(&g.ui,&g.view);
    before_paint();
    tick_refuse=false;
    check(g.tick_painting && g.tick_began==690 && tick_x("t")==59,
        "ticks: one not taken lays out instead, as the tick's frame");
    motion_painted();
    g.page.diag=diag_new_claimed("http://t.test/",1);
    now_ms=710;
    motion_tick();
    clock_us+=201000; now_ms=911;
    motion_painted();
    bool said=motion_pause_said();
    char record[2048];
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(tick_next()==YONDER_NEVER && said &&
          strstr(g.status_text,"ANIMATIONS PAUSED: a tick's frame took 201 ms, over 200")!=NULL &&
          strstr(g.status_text,"SOME ")==NULL &&
          strstr(record,"animations: paused: a tick's frame took 201 ms, over 200")!=NULL,
        "ticks: a tick's frame over MOTION_COST_MS stops the ticks, and is said as one");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();

    /* A mark that reaches past the view is cut to it: a column taller than
     * the view, its colour moving, marks the view's part of it alone. */
    now_ms=500;
    probe_page("<!doctype html><style>body{margin:0}@keyframes hue{to{background-color:blue}}"
        "#col{width:10px;height:5000px;background:red;animation:hue 1s linear infinite}"
        "</style><div id=col></div>",false);
    relayout(true);
    g.ui.any_dirty=false;
    now_ms=700;
    motion_tick();
    d=g.ui.dirty; v=g.view.bounds;
    check(g.ui.any_dirty && d.y>=v.y && d.y+d.h<=v.y+v.h && d.w<v.w,
        "ticks: a mark that reaches past the view is cut to it");
    probe_drop();

    /* The canvas's owner, its box far shorter than the view: the whole
     * view is its background's, and the whole view is marked. */
    probe_page("<!doctype html><style>body{margin:0}@keyframes bg{to{background-color:blue}}"
        "html{animation:bg 1s infinite}</style><div style='height:20px'>short</div>",false);
    relayout(true);
    g.ui.any_dirty=false;
    s_whole_by=WHOLE_PARTS;
    now_ms=900;
    motion_tick();
    d=g.ui.dirty; v=g.view.bounds;
    check(g.ui.any_dirty && d.y<=v.y && d.y+d.h>=v.y+v.h && d.x<=v.x && d.x+d.w>=v.x+v.w,
        "ticks: the canvas's owner, shorter than the view, repaints the whole view");
    check(s_whole_by==WHOLE_BACKDROP && os64_ui_dirty_covers(&g.ui,v),
        "ticks: the canvas's background moving is a whole view of its own, not the parts'");
    s_whole_by=WHOLE_PARTS;
    probe_drop();

    /* Both halves on one page: a layout due wins; layouts that cost too
     * much leave the ticks moving, said as SOME; the canvas's owner repaints
     * the whole view; a scroll's paint ticks first, a picture's does not. */
    now_ms=1000;
    probe_page("<!doctype html><style>body{margin:0}"
        "@keyframes go{to{transform:translateX(100px)}}@keyframes slide{to{margin-left:100px}}"
        "@keyframes bg{to{background-color:blue}}html{animation:bg 1s infinite}"
        "#t{width:40px;height:20px;animation:go 1s linear infinite}"
        "#a{width:10px;height:10px;animation:slide 1s linear infinite}#tall{height:3000px}"
        "</style><div id=t></div><div id=a></div><div id=tall></div>",false);
    relayout(true);
    check(motion_next()==1000 && tick_next()==1000,"ticks: a page of both is due for both");
    now_ms=1100;
    motion_tick();
    check(g.motion_began==1100 && !g.tick_painting && tick_x("t")==10,
        "ticks: a layout due by then wins, and brings what ticks to its time");
    motion_painted();
    geometry_layout_delay=201000; now_ms=1200;
    motion_tick();
    clock_us+=1000;
    motion_painted();
    geometry_layout_delay=0;
    g.page.diag=diag_new_claimed("http://t.test/",2);
    said=motion_pause_said();
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(said && motion_next()==YONDER_NEVER &&
          strstr(g.status_text,"SOME ANIMATIONS PAUSED: a layout's frame took 202 ms, over 200")!=NULL &&
          strstr(record,"animations: some paused: a layout's frame took 202 ms, over 200")!=NULL,
        "ticks: layouts stopped by what they cost while ticks move are said as some");
    g.ui.any_dirty=false;
    now_ms=1500;
    motion_tick();
    check(tick_x("t")==50 && g.tick_began==1500,
        "ticks: a page whose layouts cost too much keeps its ticking animations moving");
    check(!motion_pause_said() && strstr(g.status_text,"SOME ANIMATIONS PAUSED")!=NULL,
        "ticks: a pause said stays said while the other half's frame waits for its paint");
    d=g.ui.dirty; v=g.view.bounds;
    check(g.ui.any_dirty && d.x<=v.x && d.y<=v.y && d.x+d.w>=v.x+v.w && d.y+d.h>=v.y+v.h,
        "ticks: the canvas's owner repaints the whole view");
    clock_us+=1000;
    motion_painted();
    now_ms=1700;
    g.ui.any_dirty=false;
    pictures_tick();
    before_paint();
    check(tick_x("t")==50,"ticks: a picture's paint does not tick");
    {
        /* Two parts in opposite corners: their box is the view, and the
         * list keeps them two small rectangles, which is no whole view. */
        static os64_ui_widget_t corner[2];
        corner[0].bounds=(os64_gui_rect_t){v.x,v.y,8,8};
        corner[1].bounds=(os64_gui_rect_t){v.x+v.w-8,v.y+v.h-8,8,8};
        g.ui.any_dirty=false;
        os64_ui_mark_dirty(&g.ui,&corner[0]);
        os64_ui_mark_dirty(&g.ui,&corner[1]);
        d=g.ui.dirty;
        before_paint();
        check(tick_x("t")==50 && g.ui.damage_count==2 && d.x==v.x && d.y==v.y && d.w==v.w && d.h==v.h &&
              !os64_ui_dirty_covers(&g.ui,v),
            "ticks: parts far apart do not tick, though their box spans the view");
    }
    scroll_to(0,1);
    before_paint();
    check(tick_x("t")==70 && g.tick_began==1500 && !g.tick_painting,
        "ticks: a scroll's paint shows the ticks at its time, and is no tick's frame");
    /* With the layouts stopped, a tick not taken stops the ticks, said
     * where it failed. */
    tick_refuse=true; now_ms=1800;
    motion_tick();
    tick_refuse=false;
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(tick_next()==YONDER_NEVER && g.motion_began==1200 &&
          strstr(g.status_text,"No room to move the page's animations between layouts")!=NULL &&
          strstr(record,"No room to move the page's animations between layouts")!=NULL,
        "ticks: with the layouts stopped, a tick not taken stops the ticks and says so");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
    s_animations=was_run; s_reduce_motion=was_reduce;
    clock_step=0;
}

/* A LAYOUT REPAINTS WHAT IT CHANGED (YONDER_PERF.md § perf-4): a layout
 * for the animations alone whose changes are drawn in place marks the
 * changed element, not the view, and the record counts it; one that moves
 * a box marks the whole view, as before. */
static const char kPartPage[]="<!doctype html><style>body{margin:0}"
    "@keyframes glow{to{color:red}}@keyframes grow{to{width:300px}}"
    "#c{width:40px;height:20px;animation:glow 1s linear infinite}"
    "#w{width:40px;height:20px}</style><div id=c>x</div><div id=w>x</div>";
static const char kGrowPage[]="<!doctype html><style>body{margin:0}"
    "@keyframes grow{to{width:300px}}#w{width:40px;height:20px;animation:grow 1s linear infinite}"
    "</style><div id=w>x</div>";

static void motion_part_cases(void) {
    bool was_run=s_animations, was_reduce=s_reduce_motion;
    char was_dir[sizeof(s_diag.dir)];
    memcpy(was_dir,s_diag.dir,sizeof(was_dir));
    snprintf(s_diag.dir,sizeof(s_diag.dir),"/tmp/diag");   /* what refused is named only with a record to write */
    s_animations=true; s_reduce_motion=false;
    clock_step=0; geometry_layout_delay=0;
    for(int grow=0;grow<2;grow++) {
        now_ms=100;
        probe_page(grow?kGrowPage:kPartPage,false);
        relayout(true);
        motion_painted();
        g.ui.any_dirty=false;
        s_whole_by=WHOLE_PARTS;     /* as a paint leaves it */
        uint64_t parts_was=s_cost.layouts_parts;
        uint64_t refused_was[REFUSED_COUNT];
        memcpy(refused_was,s_cost.refused,sizeof(refused_was));
        now_ms=400;
        motion_tick();
        os64_gui_rect_t d=g.ui.dirty, v=g.view.bounds;
        bool whole=d.x<=v.x && d.y<=v.y && d.x+d.w>=v.x+v.w && d.y+d.h>=v.y+v.h;
        if(!grow)
            check(g.ui.any_dirty && !whole && d.h<=40 && s_cost.layouts_parts==parts_was+1 &&
                  s_whole_by==WHOLE_PARTS,
                "layout parts: a colour's frame marks its element, not the view, and is counted");
        else
            check(g.ui.any_dirty && whole && s_cost.layouts_parts==parts_was &&
                  s_cost.refused[FLOW_PARTS_MOVED]==refused_was[FLOW_PARTS_MOVED]+1 && s_cost.refused_last==FLOW_PARTS_MOVED &&
                  strcmp(s_cost.refused_what,"div#w")==0 && s_whole_by==WHOLE_REFUSED,
                "layout parts: a frame that moves a box marks the whole view, counted as a box moved, "
                "named, and the paint's whole view laid to it");
        s_whole_by=WHOLE_PARTS;
        motion_painted();
        if(grow) {
            /* With no record to write, the refusal is counted and nothing
             * is named: no string is made at every frame for nobody. */
            s_diag.dir[0]='\0';
            memcpy(refused_was,s_cost.refused,sizeof(refused_was));
            snprintf(s_cost.refused_what,sizeof(s_cost.refused_what),"stale");
            now_ms=700;
            motion_tick();
            check(s_cost.refused[FLOW_PARTS_MOVED]==refused_was[FLOW_PARTS_MOVED]+1 &&
                  s_cost.refused_what[0]=='\0',
                "layout parts: without diagnostics a refusal is counted, and what refused is not named");
            snprintf(s_diag.dir,sizeof(s_diag.dir),"/tmp/diag");
            motion_painted();
        }
        if(!grow) {
            /* A sheet that changed waits for no layout of its own: the next
             * frame's layout reads another cascade, so it marks the view. */
            g.page.sheets_changed=true;
            g.ui.any_dirty=false;
            parts_was=s_cost.layouts_parts;
            memcpy(refused_was,s_cost.refused,sizeof(refused_was));
            now_ms=700;
            motion_tick();
            d=g.ui.dirty;
            whole=d.x<=v.x && d.y<=v.y && d.x+d.w>=v.x+v.w && d.y+d.h>=v.y+v.h;
            check(g.ui.any_dirty && whole && s_cost.layouts_parts==parts_was &&
                  s_cost.refused[REFUSED_CASCADE]==refused_was[REFUSED_CASCADE]+1 && s_cost.refused_last==REFUSED_CASCADE &&
                  s_cost.refused_what[0]=='\0',
                "layout parts: a frame laid out from another cascade marks the whole view, counted so");
            motion_painted();
            /* A picture's size or a face that came in, waiting for its
             * settle, is laid out by the frame's layout too, and the parts
             * know only the animations: that frame marks the view. */
            for(int k=0;k<2;k++) {
                if(k==0) g.pictures_moved=true; else s_web.moved=true;
                g.ui.any_dirty=false;
                parts_was=s_cost.layouts_parts;
                memcpy(refused_was,s_cost.refused,sizeof(refused_was));
                now_ms=1000+300*(uint64_t)k;
                motion_tick();
                d=g.ui.dirty;
                whole=d.x<=v.x && d.y<=v.y && d.x+d.w>=v.x+v.w && d.y+d.h>=v.y+v.h;
                check(g.ui.any_dirty && whole && s_cost.layouts_parts==parts_was &&
                      !g.pictures_moved && !s_web.moved && s_cost.refused[REFUSED_LOADS]==refused_was[REFUSED_LOADS]+1 &&
                      s_cost.refused_last==REFUSED_LOADS,
                    k==0?"layout parts: a frame that also lays out a picture's size marks the whole view"
                        :"layout parts: a frame that also lays out a face that came in marks the whole view");
                motion_painted();
            }
        }
        probe_drop();
    }
    memcpy(s_diag.dir,was_dir,sizeof(was_dir));
    s_animations=was_run; s_reduce_motion=was_reduce;
}

static void motion_cases(void) {
    bool was_run=s_animations, was_reduce=s_reduce_motion;
    s_animations=true; s_reduce_motion=false;
    clock_step=0; geometry_layout_delay=0;
    now_ms=100;
    probe_page(kMotionPage,false);
    check(g.page.timeline==NULL && motion_next()==YONDER_NEVER,
        "motion: a layout not made to be shown starts nothing, and nothing is due");
    uint64_t for_motion=s_cost.layouts_motion;
    relayout(true);
    check(g.page.timeline!=NULL && motion_x("a")==0 && motion_next()==100 && ticker_due==100,
        "motion: the shown page's layout starts its animations, and one moving is due at once");
    check(s_cost.layouts_motion==for_motion,
        "motion: the record does not count the page's own layout as one for animations");
    char status[sizeof(g.status_text)];
    os64_strcopy(status,sizeof(status),g.status_text);
    now_ms=600; geometry_layout_delay=5000;
    g.laid_ms=37;
    motion_tick();
    check(motion_x("a")==50 && g.motion_laid && g.motion_began==600 && g.motion_us==5000 &&
          os64_streq(status,g.status_text) && g.laid_ms==37,
        "motion: a tick lays the page out at its moment, quietly, and keeps what it cost apart");
    check(s_cost.layouts_motion==for_motion+1,
        "motion: the record counts a layout for the animations as one");
    check(motion_next()==YONDER_NEVER && tick_next()==YONDER_NEVER,
        "motion: a frame waiting for its paint is the only frame, of either half");
    motion_painted();
    pictures_schedule();
    check(motion_next()==616 && ticker_due==616,
        "motion: the next comes a frame after the last began when twice its cost is sooner");
    geometry_layout_delay=40000; now_ms=616;
    motion_tick();
    motion_painted();
    check(motion_x("a")==52 && motion_next()==696,
        "motion: a costlier layout (51.6 px, at the nearest pixel) waits twice its cost after it ends");
    now_ms=650;
    motion_tick();
    check(g.motion_began==616,"motion: a ring before an animation's layout is due lays nothing out");
    /* A frame's paint is part of its cost: a layout of 5 ms at 700 whose
     * paint ends at 730, 30 ms of micros later, costs 35 ms and ends at
     * 730, so the next waits until 800. */
    geometry_layout_delay=5000; now_ms=700;
    motion_tick();
    clock_us+=30000; now_ms=730;
    motion_painted();
    check(g.motion_began==700 && g.motion_us==35000 && g.motion_ended==730 && !g.motion_painting &&
          motion_next()==800,
        "motion: a frame costs its layout and its paint, and ends when it is painted");
    clock_us+=10000; now_ms=740;
    motion_painted();
    check(g.motion_us==35000 && g.motion_ended==730,
        "motion: a paint with no frame before it costs nothing");
    g.covered=true;
    pictures_schedule();
    check(motion_next()==YONDER_NEVER && ticker_due==YONDER_NEVER,"motion: a covered window lays out for none");
    g.covered=false;
    geometry_layout_delay=201000; now_ms=800;
    g.page.diag=diag_new_claimed("http://m.test/",1);
    motion_tick();
    check(g.motion_began==800 && motion_next()==YONDER_NEVER,
        "motion: a page whose frame for an animation took more than MOTION_COST_MS lays out for none");
    say_laid_out(g.laid_ms);
    check(!motion_pause_said() && strstr(g.status_text,"ANIMATIONS PAUSED")==NULL,
        "motion: a frame is judged when its paint ends, not before, whatever says the line meanwhile");
    clock_us+=30000;
    motion_painted();
    bool said=motion_pause_said();
    char record[2048];
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(said && strstr(g.status_text,"ANIMATIONS PAUSED: a layout's frame took 231 ms, over 200")!=NULL &&
          strstr(record,"animations: paused: a layout's frame took 231 ms, over 200")!=NULL,
        "motion: the frame that stops the animations is said, on the standing line and in the record, "
        "and the line is to be painted again");
    motion_painted();
    check(!motion_pause_said(),"motion: a paint with no frame says nothing new");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    geometry_layout_delay=0;
    probe_drop();

    /* Before its first layout for an animation, the page's own last layout
     * stands for the cost; one that fails is the page's last. */
    now_ms=1000;
    probe_page(kMotionPage,false);
    relayout(true);
    g.page.diag=diag_new_claimed("http://m.test/",2);
    say_laid_out(MOTION_COST_MS+1);
    check(motion_next()==YONDER_NEVER,"motion: a page whose own layout was too slow lays out for no animation");
    check(strstr(g.status_text,"ANIMATIONS PAUSED: the layout took 201 ms, over 200")!=NULL,
        "motion: before its first frame, the page's own layout is what is said to cost too much");
    say_laid_out(MOTION_COST_MS);
    check(motion_next()==1000,"motion: one at the limit still does");
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(g.status_text,"ANIMATIONS PAUSED")==NULL && strstr(record,"animations: running")!=NULL,
        "motion: animations running again are said so, and the record follows");
    say_laid_out(MOTION_COST_MS+1);
    s_animations=false;
    say_laid_out(MOTION_COST_MS+1);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(g.status_text,"ANIMATIONS PAUSED")==NULL && strstr(record,"animations: off")!=NULL,
        "motion: animations turned off are not paused by what a frame costs, and the record says off");
    s_animations=true;
    say_laid_out(MOTION_COST_MS);
    geometry_refuse_layout=true;
    motion_tick();
    geometry_refuse_layout=false;
    check(g.page.tree!=NULL && g.motion_us==UINT64_MAX && motion_next()==YONDER_NEVER,
        "motion: a layout for an animation that fails keeps the page and is its last");
    say_laid_out(g.laid_ms);
    check(strstr(g.status_text,"ANIMATIONS PAUSED")==NULL,
        "motion: a frame that failed is said where it failed, not as one that cost too much");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
    probe_page("<!doctype html><p>nothing moves</p>",false);
    relayout(true);
    say_laid_out(MOTION_COST_MS+1);
    check(strstr(g.status_text,"ANIMATIONS PAUSED")==NULL,
        "motion: a slow page with no animations has none paused");
    probe_drop();
    /* A pause ends when nothing is left to move, and the record says so:
     * "none", not "running" (flow_motion_pending asks whether any change
     * is still to come, not whether anything was ever animated). */
    now_ms=20000;
    probe_page("<!doctype html><style>@keyframes tint{to{color:red}}"
        "p{animation:tint 1s linear 1}</style><p>once</p>",false);
    relayout(true);
    g.page.diag=diag_new_claimed("http://m.test/",3);
    say_laid_out(MOTION_COST_MS+1);
    check(strstr(g.status_text,"ANIMATIONS PAUSED")!=NULL,
        "motion: a slow page with an animation still to run pauses it");
    now_ms=25000;
    relayout(true);
    say_laid_out(MOTION_COST_MS+1);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(g.status_text,"ANIMATIONS PAUSED")==NULL && strstr(record,"animations: none")!=NULL,
        "motion: a pause ends once its animations have run out, and the record says none are left");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
    /* The pause is the page's, not the view's: an animation in place and
     * out of view is paused all the same, and scrolling changes nothing. */
    probe_page("<!doctype html><style>body{margin:0}@keyframes tint{to{color:red}}"
        "#far{margin-top:3000px;height:10px;animation:tint 1s linear infinite}</style>"
        "<div id=far>b</div>",false);
    relayout(true);
    say_laid_out(MOTION_COST_MS+1);
    check(strstr(g.status_text,"ANIMATIONS PAUSED: the layout took 201 ms, over 200")!=NULL,
        "motion: an animation out of view is paused by what the page costs");
    scroll_to(0,3000);
    check(!motion_pause_said() &&
          strstr(g.status_text,"ANIMATIONS PAUSED: the layout took 201 ms, over 200")!=NULL,
        "motion: scrolling it into view says the same, and nothing is said again");
    /* An address as long as an address may be leaves the notices whole. */
    char *was=strdup(g.page.way.url);
    memset(g.page.way.url,'a',sizeof(g.page.way.url)-1);
    g.page.way.url[sizeof(g.page.way.url)-1]='\0';
    g.scripts_on=true;
    say_laid_out(MOTION_COST_MS+1);
    size_t len=strlen(g.status_text);
    const char *modes="SCRIPTS ON | ANIMATIONS PAUSED: the layout took 201 ms, over 200 | aaa";
    check(len>=strlen("in 201 ms") && strcmp(g.status_text+len-strlen("in 201 ms"),"in 201 ms")==0 &&
          strncmp(g.status_text,modes,strlen(modes))==0,
        "motion: a long address gives way to the modes before it and the notices after it");
    status_rest("Page says: hello");
    check(strcmp(g.status_text,"SCRIPTS ON | ANIMATIONS PAUSED: the layout took 201 ms, over 200 | "
                 "Page says: hello")==0,
        "motion: a message in the status row keeps the pause said before it");
    g.scripts_on=false;
    os64_strcopy(g.page.way.url,sizeof(g.page.way.url),was);
    free(was);
    probe_drop();
    /* A paused animation is not one the cost rule stops. */
    probe_page("<!doctype html><style>@keyframes tint{to{color:red}}"
        "#a{animation:tint 1s linear infinite paused}</style><div id=a>a</div>",false);
    relayout(true);
    say_laid_out(MOTION_COST_MS+1);
    check(strstr(g.status_text,"ANIMATIONS PAUSED")==NULL,
        "motion: an animation the page paused is not paused by what a frame costs");
    probe_drop();
    /* A script's change publishes the tree an animation starts in, which no
     * layout of the shown page's says: the loop's next look says it. */
    now_ms=2000;
    probe_page("<!doctype html><style>@keyframes slide{to{margin-left:100px}}"
        ".go{animation:slide 1s infinite}</style><div id=a></div>"
        "<script>document.getElementById('a').className='go'</script>",true);
    relayout(true);
    say_laid_out(MOTION_COST_MS+1);
    check(strstr(g.status_text,"ANIMATIONS PAUSED")==NULL,
        "motion: a slow page with nothing to move has nothing paused");
    script_turn(); script_turn();
    check(motion_pause_said() &&
          strstr(g.status_text,"ANIMATIONS PAUSED: the layout took 201 ms, over 200")!=NULL,
        "motion: an animation a script starts on a slow page is said to be paused");
    probe_drop();

    /* What the view shows: an animation that only repaints where its
     * element is waits out of view, and in view one in steps is due now,
     * at the frame rate while it runs; one that moves its element is due
     * wherever it is, since a later frame may carry it into view. */
    now_ms=1000;
    probe_page("<!doctype html><style>body{margin:0}@keyframes tint{to{color:red}}"
        "#near{height:10px;animation:tint 1s linear infinite}"
        "#far{margin-top:3000px;height:10px;animation:tint 1s steps(4) infinite}</style>"
        "<div id=near>a</div><div id=far>b</div>",false);
    relayout(true);
    scroll_to(0,3000);
    check(motion_next()==1000,"motion: in view, a stepped animation, while it runs, is due now");
    scroll_to(0,1000);
    check(motion_next()==YONDER_NEVER,"motion: an animation in place, out of view, is due for nothing");
    probe_drop();
    now_ms=1000;
    probe_page(kMotionPage,false);
    relayout(true);
    scroll_to(0,1000);
    check(motion_next()==1000,"motion: an animation that moves its element is due out of view");
    scroll_to(0,0);

    /* The switches. Off, nothing is due — even while the page on screen is
     * one laid out with its animations, its new layout having failed. */
    geometry_refuse_layout=true;
    motion_use(false,false);
    geometry_refuse_layout=false;
    check(g.page.tree!=NULL && motion_x("a")==0 && motion_next()==YONDER_NEVER,
        "motion: animations off, nothing is due, even on the layout made with them");
    relayout(true);
    check(motion_x("a")==20 && motion_next()==YONDER_NEVER && g.page.timeline!=NULL,
        "motion: animations off lay the page out as its rules say, nothing due, the timeline kept");
    now_ms=1750;
    motion_use(true,false);
    check(motion_x("a")==75,"motion: on again, an animation is as far along as its start says");
    motion_use(true,true);
    check(motion_x("a")==20 && garb_cascade_env(g.page.cascade).reduced_motion,
        "motion: Reduce motion has the page told so, and the page's rule turns its animation off");
    now_ms=1800;
    motion_use(true,false);
    check(motion_x("a")==0,"motion: an animation its rules name again starts over");
    probe_drop();

    /* A script removes the animated element: the next complete layout
     * drops its record and lets the node go. */
    now_ms=2000;
    probe_page("<!doctype html><style>@keyframes slide{to{margin-left:100px}}"
        "#a{animation:slide 1s infinite}</style><div id=a></div>"
        "<script>document.getElementById('a').remove()</script>",true);
    relayout(true);
    check(motion_next()==2000,"motion: the element to be removed animates");
    script_turn(); script_turn();
    check(probe_id("a")==NULL && motion_next()==YONDER_NEVER,
        "motion: once the element is removed, nothing is due");
    probe_drop();

    /* A script's change is laid out on the page's timeline, which the
     * rebuild makes when the page has none, and the animation it starts is
     * the page's once the rebuild is published: laid out again later, it
     * has run since. */
    now_ms=2500;
    probe_page("<!doctype html><style>body{margin:0}@keyframes slide{to{margin-left:100px}}"
        ".go{animation:slide 1s linear infinite}</style><div id=a></div>"
        "<script>document.getElementById('a').className='go'</script>",true);
    script_turn(); script_turn();
    check(g.page.timeline!=NULL && motion_next()==2500,
        "motion: a script's change starts an animation on the page's timeline");
    now_ms=2750;
    relayout(true);
    check(motion_x("a")==25,"motion: and the rebuild's animation runs on in the layouts after it");
    probe_drop();

    /* A script's change owes the page a rebuild, and until it is made no
     * frame is due: page_lay_out refuses any other layout, and a frame
     * refused would count as one past every limit, the page's last. Once
     * it is made, frames are due again. */
    now_ms=3000;
    probe_page("<!doctype html><style>@keyframes slide{to{margin-left:100px}}"
        "#a{animation:slide 1s infinite}</style><div id=a></div>"
        "<script>document.getElementById('a').title='changed'</script>",true);
    relayout(true);
    os64_js_outcome_t out;
    check(motion_next()==3000 && yonder_scripts_step(g.page.scripts,&out) && out.status==OS64_JS_OK &&
          motion_next()==YONDER_NEVER,
        "motion: while a script's change waits for its rebuild, nothing is due");
    now_ms=3100;
    motion_tick();
    check(script_rebuild() && g.motion_us!=UINT64_MAX && motion_next()==3100,
        "motion: once the rebuild is made, frames are due again");
    probe_drop();
    s_animations=was_run; s_reduce_motion=was_reduce;
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
    check(scripts_host(g.page.way.doc,os64_page_shared_state(page_model(&g.page)),g.page.way.url,false,1,NULL,true)==NULL,
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
/* A press and a release on a control's own widget, through libui, as a
 * person's click on it arrives. */
static void ui_click_on(const char *id) {
    FormWidget *fw=probe_field(id);
    g.root.bounds=g.view.bounds;        /* the fixture's root has no size of its own */
    os64_gui_event_t ev={.type=OS64_GUI_EVENT_MOUSE_BUTTON_DOWN};
    ev.mouse.x=fw->w->bounds.x+2; ev.mouse.y=fw->w->bounds.y+2; ev.mouse.button=OS64_GUI_MOUSE_LEFT;
    os64_ui_dispatch(&g.ui,&ev); inputs_run();
    ev.type=OS64_GUI_EVENT_MOUSE_BUTTON_UP;
    os64_ui_dispatch(&g.ui,&ev); inputs_run();
}
static void ui_key_space(void) {
    os64_gui_event_t ev={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii=' '}};
    os64_ui_dispatch(&g.ui,&ev); inputs_run();
    ev.type=OS64_GUI_EVENT_KEY_UP;
    os64_ui_dispatch(&g.ui,&ev); inputs_run();
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
/* A `javascript:` link (MODERN_WEB.md slice 0c): the person's click runs
 * the address's code in the page, by the door an inline script runs
 * through, so it is on the page's record as a script; no request is made
 * and libway is not asked. The page's own script defines what the link
 * calls, as a login dropdown's does. */
static void javascript_page(const char *html, bool scripts) {
    probe_page(html,scripts);
    g.page.diag=diag_new_claimed("https://fixture.test/page",1);
    if(!scripts) return;
    yonder_scripts_free(g.page.scripts);
    g.page.scripts=scripts_host(g.page.way.doc,os64_page_shared_state(g.page.way.model),
        g.page.way.url,false,++g.pages_made,g.page.diag,g.page.modern);
    for (os64_html_node_t *n=g.page.way.doc->document;n;n=(os64_html_node_t *)next_within(n,g.page.way.doc->document))
        if(os64_dom_script_kind(n)==OS64_DOM_SCRIPT_CLASSIC && os64_html_attr(n,"src")==NULL)
            yonder_scripts_connected(g.page.scripts,n);
    script_turn();
}
static bool javascript_record_has(const char *line) {
    static char record[8192];
    yonder_diag_render(g.page.diag,record,sizeof(record));
    return strstr(record,line)!=NULL;
}
static void javascript_drop(void) {
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
}
static void input_javascript_links(void) {
    static const char page[]=
        "<p><a id=login href=\"javascript:toggle('open');\">log in</a></p>"
        "<p><a id=coded href=\"javascript:note(%27a%20b#c%27)\">coded</a></p>"
        "<p><a id=twice href=\"javascript:note('%2541#%2541')\">twice</a></p>"
        "<p><a id=broken href=\"javascript:nothere()\">broken</a></p>"
        "<p><a id=nothing href=\"javascript:void(0)\">nothing</a></p><p id=log>-</p>"
        "<script>function toggle(how){document.getElementById('log').textContent=how}"
        "function note(text){document.getElementById('log').textContent=text}</script>";
    javascript_page(page,true);
    uint64_t requests=g.nav.id;
    input_click_at("login");
    check(probe_text_is("log","-"),"javascript link: the code waits for the page's next turn, as a task");
    check(script_turn() && probe_text_is("log","open"),"javascript link: a click runs the address's code in the page");
    check(g.nav.id==requests && !javascript_record_has("link refused"),
        "javascript link: no request is made and libway refuses nothing");
    check(javascript_record_has("script javascript:#inline-1: ran in"),
        "javascript link: the page's record lists it as a script");
    input_click_at("coded");
    check(script_turn() && probe_text_is("log","a b#c"),
        "javascript link: the address is percent-decoded, its fragment included");
    /* The fragment arrives decoded (libpage's request): it is not decoded
     * a second time, so an escaped percent sign stays one. */
    input_click_at("twice");
    check(script_turn() && probe_text_is("log","%41#%41"),
        "javascript link: the fragment is decoded once, as the address is");
    input_click_at("broken");
    script_turn();
    check(javascript_record_has("FAILED script javascript:#inline-4: ReferenceError"),
        "javascript link: a script that throws is a FAILED script line");
    check(javascript_record_has("script javascript:#inline-4: failed in"),
        "javascript link: and its task is listed as failed");
    /* The failure's badge lays the window out again, and the fixture's
     * root has no size of its own: the view is put back where it was. */
    g.view.bounds=(os64_gui_rect_t){0,0,800,600};
    input_click_at("nothing");
    check(script_turn() && javascript_record_has("script javascript:#inline-5: ran in"),
        "javascript link: void(0) runs, and does nothing");
    /* The page's own click() follows the link by the same door. */
    yonder_scripts_t *host=g.page.scripts;
    check(yonder_scripts_address(host,"javascript:toggle('clicked')",NULL) && script_turn() &&
          probe_text_is("log","clicked"),"javascript link: queued directly, the same task");
    /* A byte the decoding makes that is not UTF-8 is U+FFFD; an escape cut
     * short, or not hex, stays as written. */
    check(yonder_scripts_address(host,"JavaScript:note('%FF|%4|%zz|%')",NULL) && script_turn() &&
          probe_text_is("log","\xEF\xBF\xBD|%4|%zz|%"),"javascript link: decoded as the standard decodes it");
    size_t cap=os64_js_default_limits().source_bytes;
    char *long_url=malloc(cap+32);
    if(long_url==NULL) abort();
    memcpy(long_url,"javascript:",11);
    memset(long_url+11,'1',cap+1);
    long_url[11+cap+1]='\0';
    check(!yonder_scripts_address(host,long_url,NULL),"javascript link: code longer than a script may be is not queued");
    long_url[11+cap-1]='\0';
    check(!yonder_scripts_address(host,long_url,"12"),"javascript link: nor is one its fragment takes past it");
    free(long_url);
    check(!yonder_scripts_address(NULL,"javascript:1",NULL) && !yonder_scripts_address(host,NULL,NULL),
          "javascript link: no host or no address queues nothing");
    yonder_scripts_depart(host);
    check(!yonder_scripts_address(host,"javascript:1",NULL),"javascript link: a departed page's host queues nothing");
    javascript_drop();

    javascript_page(page,false);
    input_click_at("login");
    check(probe_text_is("log","-") && status_says("scripts are off") && g.nav.id==0 &&
          !javascript_record_has("link refused"),
        "javascript link: with scripts off the link does nothing, and the status line says why");
    javascript_drop();
}
/* libdom's attribute backstop (MODERN_WEB.md slice 0d): an attribute an
 * Attr stood for, removed beneath the binding by a verb outside it, is a
 * broken rule the page's record names, once. */
static void input_attribute_backstop(void) {
    javascript_page("<p id=box class=old>box</p><script>var k=document.getElementById('box')"
        ".getAttributeNode('class');</script>",true);
    os64_html_node_t *box=probe_id("box");
    check(box!=NULL && os64_page_node_set_attr(os64_page_shared_state(g.page.way.model),box,"class",NULL,0,true)==0,
        "attribute backstop: a host verb removes the attribute beneath the binding");
    yonder_scripts_t *host=g.page.scripts;
    check(yonder_scripts_address(host,"javascript:void(k.value+k.value)",NULL) && script_turn(),
        "attribute backstop: the page reads the Attr twice");
    check(javascript_record_has("FAILED page: an attribute left its element outside libdom (1)"),
        "attribute backstop: the record names the broken rule, once");
    javascript_drop();
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
/* A button whose default action is nothing (no form, or type=button) is the
 * page's script's to act on: its click runs, and the status line does not
 * say the press was refused. */
static void input_script_buttons(void) {
    probe_page("<button id=loose>pdf</button><button id=bad>bad</button><form action=/send><input name=q>"
        "<button id=plain type=button>plain</button></form><p id=log>-</p>"
        "<script>var log=[];function note(n){log.push(n);document.getElementById('log').textContent=log.join('|');}"
        "document.getElementById('loose').onclick=function(){note('loose')};"
        "document.getElementById('bad').onclick=function(){throw new Error('handler boom')};"
        "document.getElementById('plain').onclick=function(){note('plain')};</script>",true);
    script_turn();
    os64_strcopy(g.status_text,sizeof(g.status_text),"-");
    FormWidget *loose=probe_field("loose"),*plain=probe_field("plain");
    button_clicked(loose->w,loose); inputs_run();
    button_clicked(plain->w,plain); inputs_run();
    check(probe_text_is("log","loose|plain") && !status_says("belongs to no form") && !status_says("submits nothing"),
          "input: a formless button and a type=button are the script's, with no refusal on the status line");
    /* A handler that fails is what the status line says, as the shop
     * demo's PDF button failed (WebAssembly): no reason overwrites it. */
    FormWidget *bad=probe_field("bad");
    button_clicked(bad->w,bad); inputs_run();
    check(status_says("handler boom") && !status_says("belongs to no form"),
          "input: a formless button's failing handler is the status line's last word");
    probe_drop();
    /* With scripts off nothing else will act on it, so the reason is why
     * nothing happened. */
    probe_page("<button id=loose>pdf</button>",false);
    FormWidget *off=probe_field("loose");
    button_clicked(off->w,off); inputs_run();
    check(status_says("belongs to no form"),"input: without scripts, a formless button says why it did nothing");
    probe_drop();
}
/* A one-line select is a dropdown (DROPDOWN.md); a select with a size is a
 * listbox. A click opens the list with the options' labels and the
 * disabled one refused; a pick of another option sets the model and fires
 * input and change once; a pick of the option it has fires nothing. */
static void input_select_dropdown(void) {
    probe_page("<select id=sort><option value=az>Name (A to Z)<option value=za>Name (Z to A)"
        "<option disabled>Retired<option value=lohi>Price (low to high)</select>"
        "<select id=many size=3><option>1<option>2<option>3</select><p id=log>-</p>"
        "<script>var log=[];var s=document.getElementById('sort');"
        "s.addEventListener('input',function(){log.push('input '+s.value)});"
        "s.addEventListener('change',function(){log.push('change '+s.value);"
        "document.getElementById('log').textContent=log.join('|')});</script>",true);
    script_turn();
    FormWidget *sort=probe_field("sort"),*many=probe_field("many");
    check(sort!=NULL && sort->kind==FW_DROPDOWN && many!=NULL && many->kind==FW_LIST,
          "select: a one-line select is a dropdown, one with a size a listbox");
    void *menus=g.ui.menu;
    g.ui.menu=&yonder_host_list;            /* the hosts' session (yonder_host_menus.c) */
    yonder_host_list=(yonder_host_list_t){0};
    os64_gui_event_t down={.type=OS64_GUI_EVENT_MOUSE_BUTTON_DOWN};
    down.mouse.button=OS64_GUI_MOUSE_LEFT;
    sort->w->cls->event(sort->w,&g.ui,&down);
    check(yonder_host_list.opens==1 && yonder_host_list.count==4 && yonder_host_list.selected==0 &&
          !strcmp(yonder_host_list.labels[3],"Price (low to high)") && !yonder_host_list.enabled[2] &&
          yonder_host_list.enabled[3],
          "select: a click opens the list, on its value, the disabled option refused");
    check(yonder_host_list_pick(3) && (inputs_run(),1) && probe_text_is("log","input lohi|change lohi") &&
          sort->u.drop.selected==3,
          "select: a pick of another option is the model's choice, and input and change once");
    sort->w->cls->event(sort->w,&g.ui,&down);
    check(yonder_host_list_pick(3) && (inputs_run(),1) && probe_text_is("log","input lohi|change lohi"),
          "select: a pick of the option it has fires nothing");
    g.ui.menu=menus;
    probe_drop();
    /* The options reordered while the list is open, count and value the
     * same: the list, a copy of the old order, closes. */
    probe_page("<select id=sort><option>A<option>B<option>C</select>"
        "<script>setTimeout(function(){var s=document.getElementById('sort');"
        "s.insertBefore(s.options[2],s.options[1])},0)</script>",true);
    script_turn();
    sort=probe_field("sort");
    g.ui.menu=&yonder_host_list;
    yonder_host_list=(yonder_host_list_t){0};
    sort->w->cls->event(sort->w,&g.ui,&down);
    check(yonder_host_list.open,"select: its list is open before the page reorders it");
    for(int i=0;i<5&&yonder_host_list.open;i++) { script_turn(); script_rebuild(); }
    const os64_page_control_t *c=os64_page_control(page_model(&g.page),widget_control(sort));
    check(c!=NULL && c->noptions==3 && !strcmp(c->options[1].label,"C") &&
          !yonder_host_list.open && yonder_host_list.detaches==1,
          "select: options reordered beneath an open list close it");
    g.ui.menu=menus;
    probe_drop();
    /* An option disabled beneath an open list closes it too; and a pick the
     * model refuses (the disabled option, picked from a list drawn before)
     * fires nothing. */
    probe_page("<select id=sort><option>A<option>B<option>C</select><p id=log>-</p>"
        "<script>var s=document.getElementById('sort');"
        "s.addEventListener('change',function(){document.getElementById('log').textContent='changed'});"
        "setTimeout(function(){s.options[2].disabled=true},0)</script>",true);
    script_turn();
    sort=probe_field("sort");
    g.ui.menu=&yonder_host_list;
    yonder_host_list=(yonder_host_list_t){0};
    sort->w->cls->event(sort->w,&g.ui,&down);
    void (*pick)(void *,size_t)=yonder_host_list.pick;
    void *user=yonder_host_list.user;
    for(int i=0;i<5&&yonder_host_list.open;i++) { script_turn(); script_rebuild(); }
    check(!yonder_host_list.open && yonder_host_list.detaches==1,
          "select: an option disabled beneath an open list closes it");
    pick(user,2);inputs_run();
    check(probe_text_is("log","-") && sort->u.drop.selected==0,
          "select: a pick of an option the model refuses fires nothing and keeps the value");
    g.ui.menu=menus;
    probe_drop();
}
static void input_cases(void) {
    g.seq=2;g.key_seq=2;g.pressed_link=9;
    yonder_bar_raise(&g.bar,1);yonder_bar_settled(&g.bar);
    check(yonder_bar_press(&g.bar,BAR_ON_YES)==BAR_NOTHING,"loss: question press arms");
    input_lost();
    check(g.seq==0 && g.key_seq==0 && g.pressed_link==-1 && g.pressed_node==NULL,
          "loss: page press and key decoders reset");
    check(yonder_bar_release(&g.bar,BAR_ON_YES)==BAR_NOTHING,
          "loss: later release cannot answer an old question press");
    yonder_bar_lower(&g.bar);

    input_links();
    input_javascript_links();
    input_attribute_backstop();
    input_hover();
    input_forms();
    input_script_buttons();
    input_select_dropdown();
}

/* ── THE STREAM (docs/design/pending/DOM_D4.md) ─────────────────────────
 * The window's side: a navigation started, its head, body and verdict
 * posted by the test standing in for the worker, and stream_turn feeding
 * the parser a slice per turn until the page arrives through the same
 * arrive() a worker's page used to take. */
static void stream_window(void) {
    os64_memset(&g,0,sizeof(g));
    g.scripts_on=false; g.way.agent=YONDER_AGENT; g.way.identity=&s_window_identity; g.way.name="yonder";
    g.settle_due=YONDER_NEVER;
    g.modern=true;
    g.zoom=g.zoom_default=1000;
    os64_ui_init(&g.ui,NULL);
    os64_ui_panel(&g.root);
    os64_ui_button(&g.back,"Back",NULL,NULL); os64_ui_button(&g.forward,"Forward",NULL,NULL);
    os64_ui_button(&g.reload,"Reload",NULL,NULL); os64_ui_button(&g.stop,"Stop",NULL,NULL);
    os64_ui_textfield(&g.field,g.field_buf,sizeof(g.field_buf),NULL,NULL,NULL);
    os64_ui_label(&g.status,g.status_text);
    os64_ui_label(&g.badge,g.badge_text);
    g.view.cls=&kViewClass; g.view.focusable=true;
    os64_ui_scrollbar(&g.vbar,NULL,NULL); os64_ui_scrollbar(&g.hbar,NULL,NULL);
    os64_ui_panel(&g.qpanel); os64_ui_label(&g.qlabel,g.qtext);
    os64_ui_button(&g.qyes,"Yes",NULL,NULL); os64_ui_button(&g.qno,"No",NULL,NULL);
    os64_ui_widget_t *kids[]={&g.back,&g.forward,&g.reload,&g.stop,&g.field.w,&g.view,&g.vbar.w,
        &g.hbar.w,&g.qpanel,&g.qlabel,&g.qyes,&g.qno,&g.badge,&g.status};
    for(size_t i=0;i<sizeof(kids)/sizeof(kids[0]);i++) os64_ui_add_child(&g.root,kids[i]);
    g.root.bounds=(os64_gui_rect_t){0,0,860,640};
    os64_ui_set_root(&g.ui,&g.root);
    g.view.bounds=(os64_gui_rect_t){0,0,800,600};
    s_env.text=probe_text; s_env.fonts=probe_fonts; s_env.replaced_size=replaced_size;
    g.pool=(os64_work_pool_t *)&pool_fake;
    pool_open=true; pool_ids=0; pool_work_id=0; pool_cancelled=0; pool_error=0; memset(&pool_work,0,sizeof(pool_work));
}
static void stream_window_drop(void) {
    pool_open=false;
    if(g.stream.active) stream_drop();
    if(pool_work.job!=NULL) { yonder_trip_release(pool_work.job,NULL); memset(&pool_work,0,sizeof(pool_work)); }
    inputs_drop(); forms_drop(); page_clear(&g.page); page_clear(&g.coming.page); os64_ui_font_release(&g.ui);
    way_history_clear(&g.way);
    while(nsheet_jobs>0) os64_free(sheet_jobs[--nsheet_jobs].job);
}
/* The real window layout participates in font adoption: a larger interface
 * face must grow its rows, without replacing the page controls' lent face
 * or losing a partially edited address. An aborted plan moves nothing. */
static os64_font_set_t *interface_test_set(uint32_t px)
{
    static const uint8_t face = 'A';
    os64_font_role_spec_t specs[OS64_FONT_ROLE_COUNT] = {0};
    specs[OS64_FONT_ROLE_UI].primary = (os64_font_source_t){OS64_FONT_SOURCE_OUTLINE, &face, 1};
    specs[OS64_FONT_ROLE_UI].pixel_height = px;
    os64_font_set_t *set = NULL;
    check(os64_font_set_prepare(os64_ui_font_context(&g.ui), specs, &set) == OS64_FONT_OK,
          "interface font: candidate opens");
    return set;
}

static void interface_font_cases(void)
{
    stream_window();
    layout();
    int32_t initial_h = g.back.bounds.h;
    os64_font_set_t *web = interface_test_set(13);
    check(os64_ui_font_app(&g.ui, web, OS64_FONT_ROLE_UI) == OS64_FONT_OK,
          "interface font: page controls have their own face");
    int32_t web_h = os64_ui_font_row_height(&g.ui, OS64_UI_FONT_APP);
    check(os64_ui_font_planner(&g.ui, interface_font_plan, interface_font_commit,
                              interface_font_discard, NULL) == OS64_FONT_OK,
          "interface font: window planner installed before adoption");
    os64_font_consumer_t consumer;
    os64_ui_font_consumer(&g.ui, &consumer);
    os64_font_set_t *large = interface_test_set(24);
    check(os64_font_adopt(large, &consumer, 1, NULL) == OS64_FONT_OK,
          "interface font: larger startup face is accepted");
    interface_font_finish();
    check(g.back.bounds.h > initial_h && g.field.w.bounds.h == g.back.bounds.h &&
          g.status.bounds.h == g.back.bounds.h && g.view.bounds.y >= g.back.bounds.y + g.back.bounds.h,
          "interface font: toolbar, address and status grow without overlapping the page");
    os64_ui_textfield_set(&g.ui, &g.field, "http://fixture.test/unfinished");
    os64_ui_set_focus(&g.ui, &g.field.w);
    g.field.cursor = 12; g.field.anchor = 7; g.field.selected = true;
    os64_gui_rect_t before = g.field.w.bounds;
    int32_t old_row = os64_ui_font_row_height(&g.ui, OS64_FONT_ROLE_UI);
    os64_font_set_t *larger = interface_test_set(32);
    void *plan = NULL;
    check(consumer.prepare(consumer.user, larger, &plan) == OS64_FONT_OK,
          "interface font: a live change can be staged");
    check(!memcmp(&g.field.w.bounds, &before, sizeof(before)) &&
          os64_ui_widget_planned_bounds(&g.field.w).h > before.h,
          "interface font: staging measures the new face without moving live bounds");
    consumer.abort(consumer.user, plan);
    check(!g.field.w.bounds_staged_valid && !memcmp(&g.field.w.bounds, &before, sizeof(before)) &&
          os64_ui_font_row_height(&g.ui, OS64_FONT_ROLE_UI) == old_row && !s_interface_resize_due,
          "interface font: abort retains the live face and geometry");
    check(os64_font_adopt(larger, &consumer, 1, NULL) == OS64_FONT_OK,
          "interface font: a live larger face is accepted");
    interface_font_finish();
    check(g.field.w.bounds.h > before.h && g.relayout_due &&
          g.ui.focus == &g.field.w && g.field.cursor == 12 && g.field.anchor == 7 && g.field.selected &&
          os64_streq(g.field_buf, "http://fixture.test/unfinished"),
          "interface font: live change preserves address text, focus and selection and resizes the viewport");
    check(os64_ui_font_row_height(&g.ui, OS64_UI_FONT_APP) == web_h &&
          s_env.viewport_font_px == 16 && g.zoom == 1000,
          "interface font: page control face, default text size and zoom are unchanged");
    before = g.field.w.bounds;
    os64_font_set_t *huge = interface_test_set(96);
    check(os64_font_adopt(huge, &consumer, 1, NULL) == OS64_FONT_LIMIT &&
          !memcmp(&g.field.w.bounds, &before, sizeof(before)) && !g.back.bounds_staged_valid &&
          !s_interface_resize_due,
          "interface font: a face wider than the toolbar fits is refused atomically");
    os64_font_set_release(huge);
    os64_font_set_release(larger);
    os64_font_set_release(large);
    os64_font_set_release(web);
    stream_window_drop();
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
/* Ctrl+L: the keys go to the address field with all of it selected, from
 * wherever focus was, and what is typed next replaces the address. Ctrl+Alt+L
 * and a plain l are not it; Ctrl+A in the field selects all by the same door. */
static void address_key_cases(void) {
    stream_window();
    os64_ui_textfield_set(&g.ui,&g.field,"http://fixture.test/somewhere");
    os64_ui_set_focus(&g.ui,&g.view);
    os64_gui_event_t ev={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii='L'&0x1F,.modifiers=OS64_GUI_MOD_CTRL|OS64_GUI_MOD_ALT}};
    check(!address_event(&ev) && g.ui.focus==&g.view,"Ctrl+Alt+L is not Ctrl+L");
    ev.key.modifiers=0; ev.key.ascii='l';
    check(!address_event(&ev) && g.ui.focus==&g.view,"a plain l is not Ctrl+L");
    ev.key.modifiers=OS64_GUI_MOD_CTRL; ev.key.ascii='L'&0x1F;
    check(address_event(&ev) && g.ui.focus==&g.field.w && g.field.selected && g.field.anchor==0 &&
          g.field.cursor==g.field.len && g.field.len==strlen("http://fixture.test/somewhere"),
        "Ctrl+L: the address field has the keys, all of it selected");
    os64_gui_event_t typed={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii='x'}};
    os64_ui_dispatch(&g.ui,&typed);
    check(os64_streq(g.field_buf,"x"),"Ctrl+L: what is typed next replaces the address");
    os64_ui_textfield_set(&g.ui,&g.field,"http://fixture.test/again");
    os64_gui_event_t all={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii='A'&0x1F,.modifiers=OS64_GUI_MOD_CTRL}};
    os64_ui_dispatch(&g.ui,&all);
    check(g.field.selected && g.field.anchor==0 && g.field.cursor==g.field.len,"Ctrl+A in the field selects all of it");
    stream_window_drop();
}
static void stream_slices(void) {
    /* Five slot-sized chunks: one turn feeds four (the 64 KiB slice) and
     * asks to be rung again; the next finishes, with the verdict already
     * waiting behind the chunks. */
    stream_window();
    /* What the last page's paints kept (YONDER_PERF.md § perf-2). */
    g.paint_cache=yonder_paint_cache_new(YONDER_PAINT_CACHE_FLOOR);
    paint_cache_begin(g.paint_cache);
    (void)paint_cache_make(g.paint_cache,(const uint8_t *)"k",1,paint_cache_hash((const uint8_t *)"k",1),64);
    /* And the buffers they left idle (§ perf-5). */
    g.paint_scratch=yonder_scratch_new();
    yonder_scratch_give(g.paint_scratch,yonder_scratch_take(g.paint_scratch,YONDER_SCRATCH_FLOOR));
    check(yonder_scratch_counts(g.paint_scratch).idle==1,"stream: the last page's paint left a buffer idle");
    start_trip("http://fixture.test/p",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    check(g.stream.active && g.nav.id==1 && pool_work.job!=NULL && g.stop.disabled==false,
        "stream: a navigation starts a stream and lights Stop");
    {
        const yonder_trip_t *t=pool_work.job;
        check(t->metadata.dest==WAY_DEST_DOCUMENT && t->metadata.site==WAY_SITE_NONE && t->metadata.activation &&
              t->identity==g.way.identity,
            "who: the browser's own navigation is site none and a person's, with the agent's lines");
    }
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
    check(yonder_paint_cache_counts(g.paint_cache).entries==0,
        "stream: what the last page's paints kept is let go when this one is shown");
    check(yonder_scratch_counts(g.paint_scratch).idle==0 && yonder_scratch_counts(g.paint_scratch).idle_bytes==0,
        "stream: the buffers the last page's paints left idle are freed when this one is shown");
    yonder_paint_cache_free(g.paint_cache);
    g.paint_cache=NULL;
    yonder_scratch_free(g.paint_scratch);
    g.paint_scratch=NULL;
    stream_window_drop();
    /* The same five chunks with the verdict still to come: the worker rang
     * once per post and a stalled socket rings nothing more, so the slice
     * that left a chunk in the ring asks for the turn that reads it. */
    stream_window();
    start_trip("http://fixture.test/p",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_body(body,len,YONDER_STREAM_CHUNK);
    check(stream_turn()==true && g.stream.active && yonder_mail_streaming(g.stream.mail),
        "stream: a slice at its budget asks for another turn before the verdict");
    check(stream_turn()==false && g.stream.active && !yonder_mail_streaming(g.stream.mail),
        "stream: the ring read empty, the turn waits for the verdict");
    stream_post_verdict(true,OS64_FETCH_OK,"");
    check(stream_turn()==false && !g.stream.active && g.page.tree!=NULL && probe_text_is("t","end"),
        "stream: the verdict then finishes the page");
    os64_free(body);
    stream_window_drop();
}
static void stream_reaped_first(void) {
    /* The job returns the moment its last post lands; the window may hear
     * the reap before it has parsed a byte. The parse goes on from the
     * mailbox the stream holds. */
    stream_window();
    start_trip("http://fixture.test/p",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
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
    start_trip("http://fixture.test/t",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    way_head_t h=stream_head("text/plain","",false);
    yonder_mail_post_head(g.stream.mail,&h);
    /* Posted a byte at a time: the mark arrives in three reads, and the
     * accent after it in two. */
    const char *text="\xEF\xBB\xBF" "plain <b>not bold</b> caf\xC3\xA9";
    stream_post_body((const uint8_t *)text,strlen(text),1);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    check(stream_turn()==false && g.page.plain!=NULL && g.page.way.doc==NULL && g.page.way.text_utf8 &&
        page_doc(&g.page)==g.page.plain && g.page.tree!=NULL,
        "stream: text arrives as its own tree, UTF-8 by its byte order mark, however the wire cut it");
    const os64_html_node_t *pre=g.page.plain->body ? g.page.plain->body->first_child : NULL;
    check(pre && os64_streq(pre->name,"plaintext") && pre->first_child &&
        pre->first_child->kind==OS64_HTML_TEXT && strstr(pre->first_child->text,"<b>not bold</b>")!=NULL,
        "stream: the text is literal under plaintext");
    check(pre && pre->first_child && strstr(pre->first_child->text,"caf\xC3\xA9")!=NULL &&
        strstr(pre->first_child->text,"\xC3\xAF")==NULL,
        "stream: the mark is not drawn and the accent is whole");
    check(os64_streq(g.page.way.note,"200 OK"),"stream: a text page's standing line");
    stream_window_drop();
    /* A body shorter than a mark: judged at the end, on what there is. */
    stream_window();
    start_trip("http://fixture.test/t",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    h=stream_head("text/plain","",false);
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_body((const uint8_t *)"hi",2,1);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    check(stream_turn()==false && g.page.plain!=NULL && !g.page.way.text_utf8,
        "stream: a two-byte text arrives, windows-1252");
    pre=g.page.plain->body ? g.page.plain->body->first_child : NULL;
    check(pre && pre->first_child && os64_streq(pre->first_child->text,"hi"),
        "stream: both of its bytes are in the page");
    stream_window_drop();
    /* No label, no mark, not JSON: windows-1252. */
    stream_window();
    start_trip("http://fixture.test/t",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
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
    start_trip("http://fixture.test/x",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    stream_post_verdict(false,OS64_FETCH_OK," that is image/png, not a page - save it with:  os64get 'x'");
    check(stream_turn()==false && !g.stream.active && g.page.tree==NULL &&
        os64_streq(g.status_rest,"that is image/png, not a page - save it with:  os64get 'x'") &&
        g.stop.disabled==true,"stream: no page, libway's sentence, and Stop goes out");
    stream_window_drop();
}
/* A picture asked for by itself: the window writes its page (image_page),
 * the picture alone on a dark page, titled by its file; nothing of the
 * reply's body is read. */
static void stream_picture_page(void) {
    stream_window();
    start_trip("http://fixture.test/shots/dwm&co.png?v=1",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    way_head_t h=stream_head("image/png","",false);
    os64_strcopy(h.url,sizeof(h.url),"http://fixture.test/shots/dwm&co.png?v=1");
    h.body=WAY_BODY_IMAGE;
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    /* The fake pool keeps one job: the picture's will take the trip's slot,
     * so the trip's is held here and released at the end. */
    void *trip=pool_work.job;
    check(stream_turn()==false && g.page.tree!=NULL && g.page.plain==NULL,
        "picture page: a picture asked for by itself arrives as a page");
    const os64_html_document_t *doc=page_doc(&g.page);
    const os64_html_node_t *img=NULL;
    for(const os64_html_node_t *n=doc->document;n;) {
        if(n->kind==OS64_HTML_ELEMENT&&n->name&&!strcmp(n->name,"img")) { img=n; break; }
        if(n->first_child){n=n->first_child;continue;}
        while(n&&!n->next)n=n->parent;
        if(n)n=n->next;
    }
    const os64_html_attr_t *src=img?os64_html_attr(img,"src"):NULL;
    check(src!=NULL && os64_streq(src->value,"http://fixture.test/shots/dwm&co.png?v=1"),
        "picture page: its one picture is the address asked for, escaped and read back whole");
    char title[64]={0};
    const os64_html_node_t *t=doc->head?doc->head->first_child:NULL;
    while(t&&!(t->kind==OS64_HTML_ELEMENT&&t->name&&!strcmp(t->name,"title"))) t=t->next;
    if(t&&t->first_child&&t->first_child->kind==OS64_HTML_TEXT) os64_strcopy(title,sizeof(title),t->first_child->text);
    check(os64_streq(title,"dwm&co.png"),"picture page: titled by its file's name");
    /* The picture is fetched as any page's is: the job in the pool is the
     * picture's, for the address asked for, released as a picture's. */
    const yonder_picture_job_t *job=pool_work.job;
    check(job!=NULL && job->kind==YONDER_JOB_PICTURE &&
          os64_streq(job->url,"http://fixture.test/shots/dwm&co.png?v=1"),
        "picture page: its picture is fetched like any page's");
    check(job!=NULL && job->kind==YONDER_JOB_PICTURE && job->hooks.metadata.dest==WAY_DEST_IMAGE &&
          job->hooks.metadata.mode==WAY_MODE_NO_CORS,
        "who: a picture is a no-cors image");
    /* This harness runs no picture job; the job is freed as a sheet job is. */
    if(pool_work.job!=NULL && pool_work.job!=trip) { os64_free(pool_work.job); memset(&pool_work,0,sizeof(pool_work)); }
    if(trip!=NULL && pool_work.job!=trip) yonder_trip_release(trip,NULL);
    stream_window_drop();
}
/* An address typed into the window is the browser's own navigation: site
 * none, and a person's (Fetch Metadata § 2.3, § 2.4). */
static void stream_typed_is_the_browsers(void) {
    stream_window();
    open_address("fixture.test/typed",NAV_GO,NULL,NULL);
    const yonder_trip_t *t=pool_work.job;
    check(t!=NULL && os64_streq(t->url,"http://fixture.test/typed") && t->metadata.site==WAY_SITE_NONE &&
          t->metadata.activation && t->metadata.dest==WAY_DEST_DOCUMENT,
        "who: an address typed is the browser's own and a person's");
    stream_window_drop();
}
static void stream_refused(void) {
    /* Deeper than twice the parser's stack: the parser refuses, the fetch
     * is cancelled, and the page arrives as far as it got, saying so. */
    stream_window();
    start_trip("http://fixture.test/deep",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
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
    start_trip("http://fixture.test/p",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
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
    start_trip("http://fixture.test/p",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
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
    start_trip(request.url,&request,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
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
    /* The page's own location.reload() of it asks first, then sends it
     * again as the page's: its origin, nobody's activation. */
    os64_strcopy(g.page.sent.scheme,sizeof(g.page.sent.scheme),"http");   /* as libpage spells a sent form's */
    reload_as(WAY_BY_PAGE);
    check(g.asker==ASK_REQUEST,"who: a page's reload of a form's reply asks first");
    bar_answered(true);
    const yonder_trip_t *again=pool_work.job;
    check(again!=NULL && again->kind==YONDER_JOB_TRIP && again->has_request && !again->metadata.activation &&
          again->metadata.site==WAY_SITE_SAME_ORIGIN && os64_streq(again->metadata.origin.host,"fixture.test"),
        "who: a page's reload of a form's reply resends as the page's");
    /* The browser's Reload of it replays the entry's own asking (here an
     * address given), as a reload. */
    click_reload(NULL,NULL);
    check(g.asker==ASK_REQUEST,"who: the browser's reload of a form's reply asks first");
    bar_answered(true);
    again=pool_work.job;
    check(again!=NULL && again->kind==YONDER_JOB_TRIP && again->has_request && again->metadata.activation &&
          again->metadata.site==WAY_SITE_NONE && again->metadata.reload,
        "who: the browser's reload of a form's reply replays its entry, as a reload");
    stream_window_drop();
}
static void stream_mail_wiring(void) {
    /* Progress and a worker's question still reach the status line and the
     * bar through the stream's mailbox; a new navigation answers No. */
    stream_window();
    start_trip("http://fixture.test/p",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
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
    start_trip("http://fixture.test/q",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
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
        start_trip("http://fixture.test/mode",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
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
static bool loop_encrypted;     /* the next loop_page's head: arrived encrypted */
static bool loop_modern_off;    /* the next loop_page's window: the modern switch off */
static bool loop_scripts_off;   /* the next loop_page's window: scripts off */
/* The next loop_page's address as asked for, and as its head says it ended
 * after redirects: NULL for the usual pair. */
static const char *loop_asked, *loop_final;
static void loop_page(const char *rest) {
    stream_window();
    g.scripts_on=!loop_scripts_off; g.script_ms=5000; now_ms=100;
    g.modern=!loop_modern_off;
    start_trip(loop_asked!=NULL ? loop_asked : "http://fixture.test/p",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    char *html=pad_page(rest);
    way_head_t h=stream_head("text/html","",false);
    if(loop_final!=NULL) os64_strcopy(h.url,sizeof(h.url),loop_final);
    h.encrypted=loop_encrypted;
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
    /* The async script landed while the parse had its next script in hand,
     * so it runs when the parse next waits: after the parse, before the
     * deferred script. The inline module is deferred too (HTML defers a
     * parser-inserted module), after the defer script before it. */
    check(probe_text_is("out","head inline|blocking|body inline|connected|body inline 2|async|deferred|module|"
        "DOMContentLoaded|load listener|body onload"),
        "loop: scripts, DOMContentLoaded and load run in the standard's order");
    now_ms=200;
    check(script_turn() && probe_text_is("out","head inline|blocking|body inline|connected|"
        "body inline 2|async|deferred|module|DOMContentLoaded|load listener|body onload|timer"),
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
    if(pool_work.job!=NULL) {
        const yonder_trip_t *t=pool_work.job;
        check(t->metadata.site==WAY_SITE_SAME_ORIGIN && !t->metadata.activation &&
              os64_streq(t->metadata.origin.host,"fixture.test"),
            "who: a script's navigation is the page's, from its origin, and nobody's");
    }
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
static void loop_write_byte_cut(void) {
    loop_page("<p id=out>before</p><script>document.write('abcdef');</script>"
        "<b id=held>held tail</b><script>document.getElementById('out').textContent='later ran'</script>");
    stream_turn();
    check(g.stream.stopped && g.stream.parser!=NULL,"write cut: stopped before the truncated write");
    g.stream.parser->opt.max_bytes=os64_html_parser_document(g.stream.parser)->input_bytes+3;
    check(loop_settle() && g.page.tree!=NULL,"write cut: the byte-cut page arrives");
    check(probe_text_is("out","before") && probe_id("held")==NULL,
        "write cut: nothing behind the cut is built, and its later script never runs");
    loop_drop();
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
    loop_write_byte_cut();
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
/* document.cookie reads and sets the browser's cookies for the page's own
 * address (the head's: http://fixture.test/final). */
static void loop_document_cookie(void) {
    fake_cookie_url[0]=fake_cookie_set[0]=0;
    loop_page("<p id=out>x</p><script>document.getElementById('out').textContent=document.cookie;"
        "document.cookie='seen=1; path=/';</script>");
    check(loop_settle() && probe_text_is("out","session=abc; theme=dark"),
        "document.cookie: a script reads the cookies the window hands it");
    check(os64_streq(fake_cookie_set,"seen=1; path=/") && os64_streq(fake_cookie_url,"http://fixture.test/final"),
        "document.cookie: a script's cookie goes to the jar for the page's own address");
    check(fake_cookie_encrypted==0,"document.cookie: a page that arrived in the clear says so to the jar");
    loop_drop();
    /* The transport's own answer travels with the page, never the scheme's. */
    loop_encrypted=true; fake_cookie_encrypted=-1;
    loop_page("<script>document.cookie='x=1';</script>");
    check(loop_settle() && fake_cookie_encrypted==1,"document.cookie: a page that arrived encrypted says so");
    loop_drop();
    loop_encrypted=false;
}
static void loop_script_changes_hold(void) {
    loop_page("<base href='http://assets.test/js/'><script src='app.js'></script>");
    check(loop_settle() && nscript_jobs==1,"review base: source fetch submitted");
    check(nscript_jobs==1 && os64_streq(script_jobs[0].job->url,"http://assets.test/js/app.js"),
        "review base: source resolves against document base");
    check(nscript_jobs==1 && script_jobs[0].job->hooks.metadata.dest==WAY_DEST_SCRIPT &&
          script_jobs[0].job->hooks.metadata.mode==WAY_MODE_NO_CORS &&
          os64_streq(script_jobs[0].job->hooks.metadata.origin.host,"fixture.test") &&
          script_jobs[0].job->hooks.identity==g.way.identity,
        "who: a script's source is a no-cors script, asked by the page's origin, with the agent's lines");
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
    check(pool_work.job!=NULL && *(const uint32_t *)pool_work.job==YONDER_JOB_TRIP &&
          ((const yonder_trip_t *)pool_work.job)->metadata.activation &&
          ((const yonder_trip_t *)pool_work.job)->metadata.site!=WAY_SITE_NONE,
        "who: a form a person sent is the page's site and a person's");
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
    loop_page("<div id=measured style='width:123px;height:20px'></div><script>"
        "document.addEventListener('DOMContentLoaded',function(){var p=document.getElementById('measured');"
        "p.setAttribute('data-width',p.offsetWidth)});</script>");
    check(loop_settle(),"arrival: lifecycle measurement settles");
    const os64_html_attr_t *measured=page_doc(&g.page)!=NULL ? os64_html_attr(probe_id("measured"),"data-width") : NULL;
    check(measured!=NULL && os64_streq(measured->value,"123"),
        "arrival: DOMContentLoaded geometry uses its arriving document");
    loop_drop();
    stream_window();g.scripts_on=true;g.script_ms=5000;
    slurp_path="/pages/local.html";slurp_bytes="<script src='app.js'></script>";
    open_address("/pages/local.html",NAV_GO,NULL,NULL);
    check(loop_settle() && nscript_jobs==1 && os64_streq(script_jobs[0].job->url,"file:///pages/app.js"),
        "local page: relative external script beside local page fetches");
    slurp_path=NULL;loop_drop();
    /* The sibling: a timer edits the page while it waits for a sheet, and
     * the page shown takes its sheet table from the tree it has by then. */
    /* The link is below the script, which would otherwise wait for it
     * (DOM_D7.md § D7d). */
    loop_page("<p id=styled>visible</p><script>"
        "setTimeout(()=>{var s=document.createElement('style');s.textContent='#styled{display:none}';"
        "document.head.appendChild(s)},10);</script><link rel=stylesheet href=wait.css>");
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
// The parse comes first while it has input: a parser-blocking script runs
// before a 0 ms timer an earlier one set, and that timer before
// DOMContentLoaded, which HTML queues behind it.
static void loop_parse_first(void) {
    loop_page("<p id=out></p><script>var o=[];function n(x){o.push(x);"
        "document.getElementById('out').textContent=o.join('|')}"
        "document.addEventListener('DOMContentLoaded',function(){n('DOMContentLoaded')});"
        "setTimeout(function(){n('f')},0);</script><script>n('g')</script><p>tail</p>");
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","g|f|DOMContentLoaded"),
        "loop: an inline script runs before a 0 ms timer set ahead of it, and the timer before DOMContentLoaded");
    loop_drop();
    /* A 0 ms chain set after the parse ended cannot hold the page back,
     * with the clock moving as it does on a machine: 10 ms a turn, past
     * the 4 ms a nested timer is clamped to, so one is due every turn. */
    loop_page("<p id=out>-</p><script>document.addEventListener('DOMContentLoaded',function(){"
        "document.getElementById('out').textContent='arrived'});"
        "function again(){setTimeout(again,0)}setTimeout(again,0);</script>");
    for(int i=0;i<200 && g.stream.active;i++) { now_ms+=10; stream_turn(); }
    check(!g.stream.active && g.page.tree!=NULL && probe_text_is("out","arrived"),
        "loop: a page that keeps setting 0 ms timers still arrives");
    loop_drop();
}
// While the parse waits for a blocking `src`, a landed async script runs and
// a landed deferred one does not; a deferred fetch still out holds the page.
static void loop_waits(void) {
    const char *note="<p id=out></p><script>var o=[];function n(x){o.push(x);"
        "document.getElementById('out').textContent=o.join('|')}</script>";
    char page[1024];
    snprintf(page,sizeof(page),"%s<script defer src=d.js></script><script async src=a.js></script>"
        "<script src=b.js></script><p>tail</p>",note);
    loop_page(page);
    check(loop_settle() && g.stream.stopped && nscript_jobs==3,
        "loop: stopped at a blocking src with a deferred and an async fetch out");
    check(script_land("/d.js","n('deferred')") && script_land("/a.js","n('async')"),
        "loop: the deferred and async sources land while the blocking one is out");
    check(loop_settle() && g.stream.stopped,"loop: the parse still waits for its blocking src");
    check(script_land("/b.js","n('blocking')") && loop_settle() && g.page.tree!=NULL &&
          probe_text_is("out","async|blocking|deferred"),
        "loop: the async script ran while the parse waited, the deferred one after the parse");
    loop_drop();
    snprintf(page,sizeof(page),"%s<script defer src=late.js></script><p>tail</p>",note);
    loop_page(page);
    check(loop_settle() && g.stream.active && g.page.tree==NULL && nscript_jobs==1,
        "loop: a deferred fetch still out holds the page after the parse");
    check(script_land("/late.js","n('late')") && loop_settle() && g.page.tree!=NULL &&
          probe_text_is("out","late"),
        "loop: the page arrives once its deferred script has run");
    loop_drop();
}
// A fragment-only location from a page not yet shown is a place in that
// page, never a fetch of it: from the parse, at DOMContentLoaded, and from
// a page waiting for its sheets.
static void loop_hash_unshown(void) {
    const char *at_dcl="<h2 id=top>top</h2><h2 id=x>x</h2><script>"
        "document.addEventListener('DOMContentLoaded',function(){location.hash='x'});</script>";
    const char *mid_parse="<h2 id=x>x</h2><script>location.hash='x';</script><p>tail</p>";
    const char *pages[2]={at_dcl,mid_parse};
    for(int i=0;i<2;i++) {
        loop_page(pages[i]);
        /* A fragment navigation: the page arrives at the address with its
         * fragment, the entry it left behind it in the session. */
        check(loop_settle() && !g.stream.active && g.page.tree!=NULL &&
              os64_streq(g.page.way.url,"http://fixture.test/final#x") && g.way.depth>=1 &&
              os64_streq(g.way.history[g.way.depth-1].url,"http://fixture.test/final"),
            i==0 ? "loop: location.hash at DOMContentLoaded arrives instead of fetching the page again"
                 : "loop: location.hash mid-parse arrives instead of fetching the page again");
        check(!status_says("fragment") && !status_says("anchor"),
            "loop: the page arrives at the fragment the script named");
        loop_drop();
    }
    loop_page("<h2 id=x>x</h2><script>"
        "setTimeout(function(){location.hash='x'},10);</script><link rel=stylesheet href=wait.css>");
    check(loop_settle() && g.coming.active,"loop: a page waiting for its sheet");
    now_ms+=50;
    check(loop_settle() && g.coming.active && !g.stream.active && g.coming.has_fragment &&
          os64_streq(g.coming.fragment,"x"),
        "loop: its timer's location.hash is kept for its arrival, not fetched");
    coming_show();
    check(g.page.tree!=NULL,"loop: the waiting page is shown");
    loop_drop();
}
// A page whose only handler is an onclick makes its runtime at the click,
// not at DOMContentLoaded or load, which no attribute in it names.
static void loop_handler_runtime_at_click(void) {
    loop_page("<p id=h onclick=\"this.textContent='clicked'\">plain</p>");
    check(loop_settle() && g.page.tree!=NULL && g.page.scripts!=NULL,"loop: a page with one onclick arrives");
    check(!yonder_scripts_listens(g.page.scripts,"DOMContentLoaded") &&
          yonder_scripts_listens(g.page.scripts,"click"),
        "loop: before a runtime, a handler page hears click and not DOMContentLoaded");
    /* A runtime costs a few hundred KiB of engine heap; a click on a page
     * that already has one costs a few KiB. */
    size_t before=live_bytes;
    input_click_at("h");
    check(probe_text_is("h","clicked") && live_bytes>before+100*1024,
        "loop: the runtime is made by the click, the first event an attribute names");
    loop_drop();
}
/* A script that queues a job and then throws (google.com's inline-1):
 * the job still runs after it, as HTML's checkpoint runs it, and the page's
 * `load` is still delivered; a later script is not refused as busy. */
static void loop_throw_after_queue(void) {
    loop_page("<p id=job>no</p><p id=load>no</p><p id=after>no</p>"
        "<script>Promise.resolve().then(function(){document.getElementById('job').textContent='ran'});"
        "null.x;</script>"
        "<script>document.getElementById('after').textContent='ran'</script>"
        "<script>window.addEventListener('load',function(){document.getElementById('load').textContent='ran'})</script>");
    check(loop_settle() && g.page.tree!=NULL,"loop: a page whose script throws after queueing a job arrives");
    check(probe_text_is("job","ran"),"loop: the job a throwing script queued still runs");
    check(probe_text_is("after","ran"),"loop: the next script is not refused as busy");
    check(probe_text_is("load","ran"),"loop: the page's load is delivered");
    check(strstr(g.status_text,"queued jobs")==NULL,"loop: nothing says the previous turn has queued jobs");
    loop_drop();
}
/* The same for a string timer (Quinn, #240): libdom runs its source, and
 * a throw after a queued job must not stall the timers after it. */
static void loop_string_timer_throws(void) {
    loop_page("<p id=job>no</p><p id=next>no</p>"
        "<script>setTimeout(\"Promise.resolve().then(function(){document.getElementById('job').textContent='ran'});"
        "null.x\",10);setTimeout(function(){document.getElementById('next').textContent='ran'},20);</script>");
    check(loop_settle() && g.page.tree!=NULL,"loop: a page with a throwing string timer arrives");
    now_ms=200;
    for(int i=0;i<4;i++) script_turn();
    check(probe_text_is("job","ran"),"loop: the job a throwing string timer queued still runs");
    check(probe_text_is("next","ran"),"loop: the timer after it is not refused as busy");
    check(strstr(g.status_text,"queued jobs")==NULL,"loop: nothing says the previous turn has queued jobs");
    loop_drop();
}
static void loop_modern(void);
/* SLICE 1b IN THE WINDOW (MODERN_WEB.md § Slice 1b): the binding's view,
 * computed-style and history providers are yonder's own, so what a page's
 * script asks reaches the window's layout, scroll and session, and what
 * the window does reaches the script as the events the brief rules. */

/* The tasks a shown page is owed run, as the loop runs them: false when
 * one never ends. */
static bool view_turns(void) {
    for(int i=0;i<64;i++) if(!script_turn()) return true;
    return false;
}
/* getComputedStyle reads the page's laid-out boxes: on a page still
 * arriving (its measured layout) and on the page shown. */
static void loop_view_computed(void) {
    loop_page("<style>body{margin:0}#b{width:200px;height:30px;color:rgb(10,20,30);padding:5%}"
        "#h{display:none}</style><div id=b></div><div id=h><span id=s></span></div>"
        "<p id=early>-</p><p id=late>-</p><script>"
        "function read(){var cs=getComputedStyle(document.getElementById('b'));"
        "return [cs.width,cs.color,cs.paddingLeft,getComputedStyle(document.getElementById('s')).display,"
        "getComputedStyle(document.createElement('i')).color==='',cs.length>100].join('|')}"
        "document.getElementById('early').textContent=read();"
        "onload=function(){document.getElementById('b').style.width='321px';"
        "document.getElementById('late').textContent=read()}</script>");
    check(loop_settle() && g.page.tree!=NULL,"view: a page asking for computed styles arrives");
    // 5% of the view's width: the body has no margin.
    char padding[32], early[96], late[96];
    garb_css_number(g.view.bounds.w*0.05,padding,sizeof(padding));
    snprintf(early,sizeof(early),"200px|rgb(10, 20, 30)|%spx|inline|true|true",padding);
    snprintf(late,sizeof(late),"321px|rgb(10, 20, 30)|%spx|inline|true|true",padding);
    check(probe_text_is("early",early),
        "view: getComputedStyle on a page still arriving reads its measured layout");
    check(probe_text_is("late",late),
        "view: on the page shown it reads the layout made current, a script's change included");
    loop_drop();
}
/* A script's scroll moves g.view's, and any scroll of the view owes the
 * page a scroll event, at the document and bubbling to window. */
static void loop_view_scroll(void) {
    loop_page("<style>body{margin:0;height:3000px}</style><p id=out>-</p><p id=size>-</p><script>"
        "var n=0;addEventListener('scroll',function(e){n++;document.getElementById('out').textContent="
        "'scrolled '+scrollY+' '+n+' '+(e.target===document)});"
        "document.getElementById('size').textContent=[innerWidth,innerHeight,outerWidth>0].join(',');"
        "onload=function(){scrollTo({top:300,behavior:'smooth'})}</script>");
    check(loop_settle() && g.page.tree!=NULL && g.sy==300,"view: a script's scrollTo scrolls the page shown");
    char size[48];
    snprintf(size,sizeof(size),"%d,%d,true",(int)g.view.bounds.w,(int)g.view.bounds.h);
    check(probe_text_is("size",size),"view: innerWidth and innerHeight are the view in CSS pixels");
    check(view_turns() && probe_text_is("out","scrolled 300 1 true"),"view: the scroll is a task's event");
    scroll_to(0,500);
    check(view_turns() && probe_text_is("out","scrolled 500 2 true"),"view: a person's scroll is one too");
    check(view_turns() && probe_text_is("out","scrolled 500 2 true"),"view: and only once");
    loop_drop();
    // A scroll asked for before the page is shown is made when it is.
    loop_page("<style>body{margin:0;height:3000px}</style><script>scrollTo(0,400)</script><p>tail</p>");
    check(loop_settle() && g.page.tree!=NULL && g.sy==400,"view: a scroll asked for while the page arrived is made");
    loop_drop();
}
/* A resize owes resize, and a list whose answer moved its change. */
static void loop_view_media(void) {
    loop_page("<p id=out>-</p><script>var log=[];var m=matchMedia('(min-width: 700px)');"
        "function o(){document.getElementById('out').textContent=log.join(',')}"
        "m.onchange=function(e){log.push('change:'+e.matches+':'+e.media);o()};"
        "addEventListener('resize',function(){log.push('resize:'+innerWidth+':'+m.matches);o()});"
        "log.push('matches:'+m.matches);o()</script>");
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","matches:true"),"media: judged at the view");
    g.view.bounds.w=600;
    check(view_turns() && probe_text_is("out","matches:true,resize:600:false,change:false:(min-width: 700px)"),
        "media: a resize fires resize, then the list's change");
    loop_drop();
}
/* The three routes and their events (MODERN_WEB_BRIEFS.md § Slice 1b). */
static void loop_view_history(void) {
    loop_page("<p id=out>-</p><script>var log=[];"
        "function o(){document.getElementById('out').textContent=log.join(',')}"
        "addEventListener('popstate',function(e){log.push('pop:'+JSON.stringify(e.state)+':'+location.pathname+location.hash);o()});"
        "addEventListener('hashchange',function(e){log.push('hash:'+e.oldURL.split('#')[1]+'>'+e.newURL.split('#')[1]);o()});"
        "onload=function(){history.pushState({n:1},'','/one');history.pushState({n:2},'','#frag');"
        "log.push('len:'+history.length);o()}</script>");
    check(loop_settle() && g.page.tree!=NULL && os64_streq(g.page.way.url,"http://fixture.test/one#frag") &&
          g.way.depth==2 && os64_streq(g.field_buf,"http://fixture.test/one#frag"),
        "history: a state update moves the address, the field and the session");
    check(view_turns() && probe_text_is("out","len:3"),
        "history: pushState to another fragment fires neither popstate nor hashchange");
    os64_work_id_t submitted=pool_ids;
    click_back(NULL,NULL);
    check(!g.stream.active && pool_ids==submitted && os64_streq(g.page.way.url,"http://fixture.test/one"),
        "history: Back into a pushed entry fetches nothing");
    check(view_turns() && probe_text_is("out","len:3,pop:{\"n\":1}:/one,hash:frag>undefined"),
        "history: a traversal fires popstate with the entry's state, then hashchange");
    click_forward(NULL,NULL);
    check(view_turns() && probe_text_is("out","len:3,pop:{\"n\":1}:/one,hash:frag>undefined,"
                                              "pop:{\"n\":2}:/one#frag,hash:undefined>frag"),
        "history: Forward the same");
    click_back(NULL,NULL); click_back(NULL,NULL);
    check(!g.stream.active && pool_ids==submitted && os64_streq(g.page.way.url,"http://fixture.test/final") &&
          g.way.ahead==2,
        "history: back to the entry the page arrived at");
    loop_drop();
    // A person's fragment link: an entry, the scroll, then popstate and
    // hashchange; a link to a name no element has changes the address
    // and scrolls nowhere.
    loop_page("<style>body{margin:0}</style><a id=l href=#target>go</a><a id=r href=#route>route</a>"
        "<div style=height:2000px></div><h2 id=target>t</h2><p id=out>-</p><script>var log=[];"
        "addEventListener('popstate',function(e){log.push('pop:'+e.state+':'+(scrollY>0))});"
        "addEventListener('hashchange',function(e){log.push('hash:'+location.hash);"
        "document.getElementById('out').textContent=log.join(',')})</script>");
    check(loop_settle() && g.page.tree!=NULL,"history: a page of fragment links arrives");
    input_click_at("l");
    check(g.sy>0 && g.way.depth==1 && os64_streq(g.page.way.url,"http://fixture.test/final#target"),
        "history: a fragment link scrolls and adds an entry");
    check(view_turns() && probe_text_is("out","pop:null:true,hash:#target"),
        "history: then popstate and hashchange, the scroll already made");
    scroll_to(0,0);
    input_click_at("r");
    check(g.sy==0 && g.way.depth==2 && os64_streq(g.page.way.url,"http://fixture.test/final#route"),
        "history: a name nothing claims is still a navigation, scrolling nowhere");
    check(view_turns() && probe_text_is("out","pop:null:true,hash:#target,pop:null:false,hash:#route"),
        "history: a router hears it");
    scroll_to(0,300);
    click_back(NULL,NULL);
    check(!g.stream.active && g.sy==0 && os64_streq(g.page.way.url,"http://fixture.test/final#target"),
        "history: Back puts the scroll back where it was when the entry was left");
    click_forward(NULL,NULL);
    check(!g.stream.active && g.sy==300 && os64_streq(g.page.way.url,"http://fixture.test/final#route"),
        "history: and Forward to where it was on the entry Back left");
    loop_drop();
    // A page that keeps its own scroll is not scrolled by a traversal.
    loop_page("<style>body{margin:0;height:3000px}</style><script>history.scrollRestoration='manual';"
        "onload=function(){history.pushState(null,'','#a')}</script>");
    check(loop_settle() && g.page.tree!=NULL && g.way.depth==1,"history: a manual page pushes an entry");
    scroll_to(0,700);
    click_back(NULL,NULL);
    check(g.sy==700 && os64_streq(g.page.way.url,"http://fixture.test/final"),
        "history: scrollRestoration manual leaves the scroll where it is");
    loop_drop();
    // Entries a page made before it was shown join the session when it is.
    loop_page("<p id=out>-</p><script>history.pushState(1,'','?a');history.pushState(2,'','?b');history.back();"
        "addEventListener('popstate',function(e){document.getElementById('out').textContent="
        "e.state+':'+location.search+':'+history.length})</script><p>tail</p>");
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","1:?a:3"),
        "history: a page arriving traverses its own entries");
    check(g.way.depth==1 && os64_streq(g.way.history[0].url,"http://fixture.test/final") && g.way.ahead==1 &&
          os64_streq(g.page.way.url,"http://fixture.test/final?a") &&
          os64_streq(g.way.forward[0].url,"http://fixture.test/final?b") &&
          os64_streq(g.way.forward[0].state,"2"),
        "history: and they join the session when it is shown, the one ahead with its state");
    loop_drop();
}
/* Short of memory at any allocation of a state update, a replacement, a
 * fragment navigation and a traversal, the session and the page still
 * agree: history.length counts the entries the window keeps and
 * location.href is the address the window shows. (The harness's last
 * check counts what a cut leaked.) */
static void loop_view_history_short(void) {
    static const char page[]="<style>body{margin:0}p{height:20px;margin:0}</style>"
        "<p id=a onclick=\"history.pushState({n:1},'','/one');history.replaceState({n:2},'','#r');"
        "location.hash='h';history.back()\">a</p>"
        "<p id=c onclick=\"document.getElementById('out').textContent=history.length+'|'+location.href\">c</p>"
        "<p id=out>-</p><script>var ready=1</script>";
    size_t count=0;
    unsigned asked=0, retired=0, moved=0;
    for(size_t cut=0;cut==0 || cut<=count;cut++) {
        loop_page(page);
        if(!loop_settle() || g.page.tree==NULL) { check(false,"history short: the page arrives"); loop_drop(); return; }
        size_t start=attempts;
        if(cut) fail_at=attempts+cut;
        input_click_at("a");
        view_turns();
        if(!cut) check(g.way.depth==1 && g.way.ahead==1 && os64_streq(g.page.way.url,"http://fixture.test/one#r"),
                       "history short: the baseline lands one back from the fragment");
        moved+=g.way.depth>0;
        click_back(NULL,NULL);            // a person's traversal, under the same cut
        view_turns();
        fail_at=0;
        if(!cut) count=attempts-start;
        if(!cut) check(g.way.depth==0 && os64_streq(g.page.way.url,"http://fixture.test/final"),
                       "history short: and Back to where the page arrived");
        view_turns();
        // A cut the engine took retires the page's scripts (scripts.h):
        // there is no page left to ask.
        if(!yonder_scripts_alive(g.page.scripts)) { retired++; loop_drop(); continue; }
        input_click_at("c");
        view_turns();
        char want[OS64_FETCH_URL_MAX+32];
        os64_snprintf(want,sizeof(want),"%d|%s",(int)(g.way.depth+1+g.way.ahead),g.page.way.url);
        bool agree=probe_text_is("out",want);
        asked++;
        if(!agree) {
            os64_html_node_t *o=probe_id("out");
            fprintf(stderr,"history short: cut %zu: page says %s, window %s\n",cut,
                    o && o->first_child ? o->first_child->text : "?",want);
        }
        check(agree,"history short: the page and the window agree on the entries and the address");
        loop_drop();
    }
    check(count>0 && moved>0 && asked>retired,"history short: the sweep reaches the session's allocations");
}
/* The body of the page the trip in flight fetched (loop_page's, without
 * starting a trip). */
static void post_arriving(const char *rest) {
    char *html=pad_page(rest);
    way_head_t h=stream_head("text/html","",false);
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_body((const uint8_t *)html,strlen(html),YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    free(html);
}
/* Whether the session is `back` behind the page on screen (oldest first,
 * by address), the page at `here`, and `ahead` before it (nearest first);
 * each list is addresses joined by `,` after http://fixture.test. */
static bool session_is(const char *back, const char *here, const char *ahead) {
    char got_back[512]="", got_ahead[512]="";
    for(int32_t i=0;i<g.way.depth;i++)
        os64_snprintf(got_back+strlen(got_back),sizeof(got_back)-strlen(got_back),"%s%s",i?",":"",
                      g.way.history[i].url+strlen("http://fixture.test"));
    for(int32_t i=g.way.ahead-1;i>=0;i--)
        os64_snprintf(got_ahead+strlen(got_ahead),sizeof(got_ahead)-strlen(got_ahead),"%s%s",
                      i<g.way.ahead-1?",":"",g.way.forward[i].url+strlen("http://fixture.test"));
    bool same=os64_streq(got_back,back) && os64_streq(g.page.way.url+strlen("http://fixture.test"),here) &&
              os64_streq(got_ahead,ahead);
    if(!same)
        fprintf(stderr,"session: [%s] %s [%s], wanted [%s] %s [%s]\n",got_back,
                g.page.way.url+strlen("http://fixture.test"),got_ahead,back,here,ahead);
    return same;
}
/* The cold reader's findings on slice 1b, each a case (the orchestrator's
 * rulings, MODERN_WEB_BRIEFS.md § Slice 1b). */
static void loop_view_owed(void) {
    // A hashchange carries the addresses it was owed with, and a script's
    // fragment navigation fires its popstate there and then, with the
    // fragment entry's state.
    loop_page("<p id=out>-</p><script>var log=[];function o(){document.getElementById('out').textContent=log.join(',')}"
        "addEventListener('hashchange',function(e){log.push(e.oldURL.split('#')[1]+'>'+e.newURL.split('#')[1]);o()});"
        "addEventListener('popstate',function(e){log.push('pop:'+JSON.stringify(e.state)+location.hash);o()});"
        "onload=function(){location.hash='a';location.hash='b';history.pushState({n:1},'');log.push('then');o()}</script>");
    check(loop_settle() && view_turns() && probe_text_is("out","pop:null#a,pop:null#b,then,undefined>a,a>b"),
        "owed: each hashchange carries its own addresses, each popstate its entry's state, at once");
    loop_drop();
    // The hash a page already has is no navigation: no entry, no event.
    loop_page("<p id=out>-</p><script>var log=[];function o(){document.getElementById('out').textContent=log.join(',')}"
        "addEventListener('popstate',function(){log.push('pop');o()});"
        "addEventListener('hashchange',function(){log.push('hash');o()});"
        "onload=function(){location.hash='a';setTimeout(function(){log.push('then');location.hash='a';o()},10)}</script>");
    check(loop_settle() && view_turns(),"owed: the hash page arrives");
    now_ms+=100;
    check(loop_settle() && view_turns() && probe_text_is("out","pop,hash,then") && g.way.depth==1,
        "owed: setting the hash it has changes nothing (HTML's hash setter)");
    loop_drop();
}
static void loop_view_refetched(void) {
    // A fetched Back gives the document its entry's state; so does a
    // reload, which is the same entry again.
    loop_page("<p id=out>-</p><script>onload=function(){history.pushState({k:1},'','/kept')}</script>");
    check(loop_settle() && view_turns() && os64_streq(g.page.way.url,"http://fixture.test/kept"),
        "refetched: the page pushes its state");
    start_trip("http://fixture.test/other",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p>other</p>");
    check(loop_settle() && view_turns() && g.way.depth==2,"refetched: another page");
    click_back(NULL,NULL);
    check(g.stream.active,"refetched: Back to the pushed entry fetches it");
    post_arriving("<p id=out>-</p><script>document.getElementById('out').textContent="
        "JSON.stringify(history.state)+':'+history.length</script>");
    check(loop_settle() && view_turns() && probe_text_is("out","{\"k\":1}:3") &&
          g.page.entry_state!=NULL && os64_streq(g.page.entry_state,"{\"k\":1}"),
        "refetched: the document that comes back reads the entry's state (HTML's restore)");
    click_reload(NULL,NULL);
    post_arriving("<p id=out>-</p><script>document.getElementById('out').textContent=JSON.stringify(history.state)</script>");
    check(loop_settle() && view_turns() && probe_text_is("out","{\"k\":1}"),
        "refetched: a reload is the same entry, its state with it");
    loop_drop();
}
static void loop_view_identity(void) {
    // A fragment link on the page on screen while Back fetches another
    // entry: the traversal lands on the entry it named, and the change the
    // page made meanwhile never joins the session (HTML's traversal queue
    // finalizes it only while its document is still the active one).
    loop_page("<p>P</p><script>history.replaceState(null,'','/P')</script>");
    check(loop_settle() && view_turns(),"identity: P");
    start_trip("http://fixture.test/A",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<style>body{margin:0}</style><a id=l href=#sec>go</a><div style=height:2000px></div>"
        "<h2 id=sec>s</h2><script>history.replaceState(null,'','/A')</script>");
    check(loop_settle() && view_turns() && session_is("/P","/A",""),"identity: on A, P behind");
    click_back(NULL,NULL);
    input_click_at("l");
    check(g.stream.active && session_is("/P","/A#sec",""),
        "identity: the link moves A's address, and not the session, while P is fetched");
    post_arriving("<p>P again</p><script>history.replaceState(null,'','/P')</script>");
    check(loop_settle() && view_turns() && session_is("","/P","/A"),
        "identity: Back lands on the entry it named, A ahead as it was");
    // The same change with the traversal stopped instead joins the session.
    click_forward(NULL,NULL);
    post_arriving("<a id=l href=#sec>go</a><script>history.replaceState(null,'','/A')</script>");
    check(loop_settle() && view_turns() && session_is("/P","/A",""),"identity: Forward to A");
    click_back(NULL,NULL);
    input_click_at("l");
    stop_trip();
    check(!g.stream.active && session_is("/P,/A","/A#sec",""),
        "identity: a stopped traversal leaves the change the page made, as its entry");
    loop_drop();
}
static void unshown_back_case(const char *when, const char *script) {
    char body[512];
    loop_page("<p id=out>P</p><script>history.replaceState(null,'','/P')</script>");
    check(loop_settle() && view_turns(),"unshown back: P");
    start_trip("http://fixture.test/A",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    os64_snprintf(body,sizeof(body),"<p id=out>A</p><script>history.replaceState(null,'','/A');%s</script><p>tail</p>",
                  script);
    post_arriving(body);
    loop_settle();
    view_turns();
    if(g.stream.active) {                   // `load`'s Back fetches P
        const yonder_trip_t *t=pool_work.job;
        char who[160];
        os64_snprintf(who,sizeof(who),"unshown back: history.back() %s is the page's own, nobody's",when);
        check(t!=NULL && t->kind==YONDER_JOB_TRIP && !t->metadata.activation &&
              t->metadata.site==WAY_SITE_SAME_ORIGIN,who);
        post_arriving("<p id=out>P</p><script>history.replaceState(null,'','/P')</script>");
        loop_settle();
        view_turns();
    }
    char what[160];
    os64_snprintf(what,sizeof(what),"unshown back: history.back() %s lands on P, A ahead",when);
    check(probe_text_is("out","P") && session_is("","/P","/A"),what);
    loop_drop();
}
/* A page not yet shown going back past the page on screen fetches that
 * entry (unshown_go's session_go): the page's own traversal, nobody's. */
static void unshown_back_past(void) {
    loop_page("<p id=out>S</p><script>history.replaceState(null,'','/S')</script>");
    check(loop_settle() && view_turns(),"unshown back past: S");
    start_trip("http://fixture.test/P",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p id=out>P</p><script>history.replaceState(null,'','/P')</script>");
    check(loop_settle() && view_turns() && session_is("/S","/P",""),"unshown back past: P");
    start_trip("http://fixture.test/A",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p id=out>A</p><script>history.replaceState(null,'','/A');history.go(-2)</script><p>tail</p>");
    loop_settle();
    view_turns();
    const yonder_trip_t *t=g.stream.active ? pool_work.job : NULL;
    check(t!=NULL && t->kind==YONDER_JOB_TRIP && strstr(t->url,"/S")!=NULL && !t->metadata.activation &&
          t->metadata.site==WAY_SITE_SAME_ORIGIN,
        "unshown back past: history.go(-2) from A fetches S as the page's own, nobody's");
    loop_drop();
}
static void loop_view_unshown_back(void) {
    unshown_back_past();
    // One rule wherever the page is in its arrival: its navigation
    // committed when its response came, so Back from it is P.
    unshown_back_case("during the parse","history.back()");
    unshown_back_case("at DOMContentLoaded","document.addEventListener('DOMContentLoaded',function(){history.back()})");
    unshown_back_case("at load","onload=function(){history.back()}");
    unshown_back_case("in a later task","onload=function(){setTimeout(function(){history.back()},0)}");
}
static void loop_view_passed_over(void) {
    // A traversal over more than one entry: only the entry left takes the
    // scroll the person is at; the ones passed over keep their own.
    loop_page("<style>body{margin:0;height:5000px}</style><p id=x>x</p>");
    check(loop_settle() && view_turns(),"passed over: a tall page");
    scroll_to(0,100); page_update(&g.page,"http://fixture.test/final#a",NULL,0,true,true);
    scroll_to(0,200); page_update(&g.page,"http://fixture.test/final#b",NULL,0,true,true);
    scroll_to(0,300);
    session_go(-2,WAY_BY_BROWSER,NULL);
    check(os64_streq(g.page.way.url,"http://fixture.test/final") && g.sy==100,"passed over: go(-2) lands at 100");
    session_go(1,WAY_BY_BROWSER,NULL);
    check(os64_streq(g.page.way.url,"http://fixture.test/final#a") && g.sy==200,
        "passed over: Forward to the entry passed over is at its own 200");
    session_go(1,WAY_BY_BROWSER,NULL);
    check(os64_streq(g.page.way.url,"http://fixture.test/final#b") && g.sy==300,
        "passed over: and the one left is at 300");
    loop_drop();
}
/* Short of memory at any allocation of a fetched Back that brings a state
 * back (the copy session_go takes, the arriving page's, the host's keep
 * and the binding's), the arrived page and the window agree on its state:
 * what history.state reads is the window's Page.entry_state. */
static void loop_view_refetch_short(void) {
    size_t clicked=0, arrived=0;
    unsigned asked=0, kept=0;
    // The cuts fall in the click (1 to `clicked`) or the arrival after it
    // (the rest); posting the page is the fake mailbox's and is not cut.
    for(size_t cut=0;cut==0 || cut<=clicked+arrived;cut++) {
        loop_page("<script>onload=function(){history.pushState({k:1},'','/kept')}</script>");
        if(!loop_settle() || !view_turns()) { check(false,"refetch short: the page arrives"); loop_drop(); return; }
        start_trip("http://fixture.test/other",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
        post_arriving("<p>other</p>");
        loop_settle();
        view_turns();
        size_t start=attempts;
        if(cut && cut<=clicked) fail_at=attempts+cut;
        click_back(NULL,NULL);
        fail_at=0;
        if(!cut) clicked=attempts-start;
        if(g.stream.active)
            post_arriving("<p id=out>-</p><script src=wait.js></script><script>document.getElementById('out')"
                          ".textContent=JSON.stringify(history.state)</script>");
        // The arrival is cut through to its last script: the stream's host
        // made, the state kept for its binding, and the scripts compiled
        // and run (a compile that runs out of memory is an exception,
        // libjs patch 0010). Landing `wait.js` is the worker's, and is not.
        start=attempts;
        if(cut>clicked) fail_at=attempts+(cut-clicked);
        loop_settle();
        size_t settled=attempts-start, left=fail_at>attempts ? fail_at-attempts : 0;
        fail_at=0;
        script_land("wait.js","0");
        start=attempts;
        if(left) fail_at=attempts+left;
        loop_settle();
        fail_at=0;
        if(!cut) arrived=settled+(attempts-start);
        view_turns();
        // A page whose own script could not write its answer under the
        // cut (no text, or the parse's `-`) is not asked.
        os64_html_node_t *out=probe_id("out");
        const char *said=out && out->first_child && out->first_child->kind==OS64_HTML_TEXT ? out->first_child->text : "";
        if(g.page.tree!=NULL && os64_streq(g.page.way.url,"http://fixture.test/final") &&
           yonder_scripts_alive(g.page.scripts) && (os64_streq(said,"null") || said[0]=='{')) {
            asked++;
            const char *state=g.page.entry_state;
            bool agree=state==NULL ? probe_text_is("out","null") : probe_text_is("out",state);
            kept+=state!=NULL;
            if(!agree) {
                os64_html_node_t *o=probe_id("out");
                fprintf(stderr,"refetch short: cut %zu (%zu in the click): page says %s, window %s\n",cut,clicked,
                        o && o->first_child ? o->first_child->text : "?",state ? state : "NULL");
            }
            check(agree,"refetch short: the page reads the state the window keeps");
        }
        loop_drop();
    }
    check(clicked>0 && asked>0 && kept>0,"refetch short: the sweep reaches the arrivals that keep a state");
}
/* The corpus replay's suspects: what a page reaches through what this
 * slice hands it must be there. statcounter's load listener focuses
 * `document.defaultView`; a page asks `instanceof CSSStyleDeclaration` of a
 * computed style. */
static void loop_view_reached(void) {
    loop_page("<p id=out>-</p><script>addEventListener('load',function(){var t=document.defaultView;"
        "t&&t.focus();document.getElementById('out').textContent=(getComputedStyle(document.body) instanceof "
        "CSSStyleDeclaration)+':'+(document.body.style instanceof CSSStyleDeclaration)})</script>");
    char record[8192];
    check(loop_settle() && view_turns() && probe_text_is("out","true:true"),
        "reached: window.focus() from defaultView, and CSSStyleDeclaration for both declarations");
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"FAILED")==NULL,"reached: the load listener does not fail");
    loop_drop();
}
/* Codex's round on slice 1b. A history delta of any WebIDL `long` and a
 * scroll of any double: INT32_MIN has no int32_t magnitude, and a rounded
 * coordinate past the bound must not be cast. */
static void loop_view_extremes(void) {
    loop_page("<style>body{margin:0;height:5000px}</style><p id=out>-</p><script>"
        "var log=[];function o(){document.getElementById('out').textContent=log.join(',')}"
        "onload=function(){history.pushState(1,'','#a');history.pushState(2,'','#b');"
        "setTimeout(function(){history.go(-2147483648);"
        "setTimeout(function(){history.go(2147483647);"
        "setTimeout(function(){history.go(Math.pow(2,53)+2);"
        "setTimeout(function(){history.go(-(Math.pow(2,53)+2));"
        "setTimeout(function(){log.push(location.hash);scrollTo(2147483647.6,-2147483648.7);log.push(scrollY===0);"
        "scrollBy(1e308,1e308);log.push(scrollY>0);scrollTo(-1e308,1e308);log.push(scrollX===0&&scrollY>0);o()},10)},10)},10)},10)},10)}</script>");
    check(loop_settle() && view_turns(),"extremes: the page arrives");
    for(int i=0;i<8;i++) { now_ms+=20; loop_settle(); view_turns(); }
    {
        os64_html_node_t *o=probe_id("out");
        if(!probe_text_is("out",",true,true,true"))
            fprintf(stderr,"extremes: page says %s at %s, depth %d ahead %d\n",o && o->first_child ? o->first_child->text : "?",
                    g.page.way.url,(int)g.way.depth,(int)g.way.ahead);
    }
    check(probe_text_is("out",",true,true,true") && os64_streq(g.page.way.url,"http://fixture.test/final") &&
          g.way.depth==0 && g.way.ahead==2,
        "extremes: deltas past any session are nothing; 2^53+2 is 2 and -(2^53+2) is -2 (WebIDL long)");
    check(g.sy>0 && g.sx==0,"extremes: scrolls past every bound are held to the page");
    loop_drop();
    // The same delta from a page not yet shown: nothing, and it arrives.
    loop_page("<p id=out>P</p>");
    check(loop_settle() && view_turns(),"extremes: P");
    start_trip("http://fixture.test/A",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    // Two scripts, two tasks: in one task the two would add up to -1.
    post_arriving("<p id=out>A</p><script>history.go(-2147483648)</script><script>history.go(2147483647)</script>"
                  "<p>tail</p>");
    loop_settle();
    view_turns();
    if(!probe_text_is("out","A") || g.way.depth!=1)
        fprintf(stderr,"extremes arriving: %s depth %d stream %d\n",g.page.way.url,(int)g.way.depth,(int)g.stream.active);
    check(probe_text_is("out","A") && g.way.depth==1,"extremes: from an arriving page too, and the page arrives");
    loop_drop();
}
/* history.forward() past the entries an arriving page made: the same one
 * rule as Back. The page arrives by a Back to T, P ahead of it; forward
 * from T is P, the page on screen, at every point in the arrival. */
static void unshown_forward_case(const char *when, const char *script) {
    char body[512];
    loop_page("<p id=out>T</p><script>history.replaceState(null,'','/T')</script>");
    check(loop_settle() && view_turns(),"unshown forward: T");
    start_trip("http://fixture.test/P",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p id=out>P</p><script>history.replaceState(null,'','/P')</script>");
    check(loop_settle() && view_turns() && session_is("/T","/P",""),"unshown forward: on P, T behind");
    click_back(NULL,NULL);
    os64_snprintf(body,sizeof(body),"<p id=out>T</p><script>history.replaceState(null,'','/T');%s</script><p>tail</p>",
                  script);
    post_arriving(body);
    loop_settle();
    view_turns();
    if(g.stream.active) {                   // `load`'s Forward fetches P
        post_arriving("<p id=out>P</p><script>history.replaceState(null,'','/P')</script>");
        loop_settle();
        view_turns();
    }
    char what[160];
    os64_snprintf(what,sizeof(what),"unshown forward: history.forward() %s lands on P, T behind",when);
    check(probe_text_is("out","P") && session_is("/T","/P",""),what);
    loop_drop();
}
static void loop_view_unshown_forward(void) {
    unshown_forward_case("during the parse","history.forward()");
    unshown_forward_case("at DOMContentLoaded","document.addEventListener('DOMContentLoaded',function(){history.forward()})");
    unshown_forward_case("at load","onload=function(){history.forward()}");
    unshown_forward_case("in a later task","onload=function(){setTimeout(function(){history.forward()},0)}");
    // A new way's page is the newest entry: forward from it is nothing,
    // and it arrives.
    loop_page("<p id=out>P</p><script>history.replaceState(null,'','/P')</script>");
    check(loop_settle() && view_turns(),"unshown forward: P");
    start_trip("http://fixture.test/A",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p id=out>A</p><script>history.replaceState(null,'','/A');history.forward()</script><p>tail</p>");
    check(loop_settle() && view_turns() && probe_text_is("out","A") && session_is("/P","/A",""),
        "unshown forward: from a new way's page, nothing is ahead, and it arrives");
    loop_drop();
}
/* A page fetched by Back that pushes before it is shown: the traversal
 * lands on the entry it was fetched for, and the push is from that entry
 * (it lets go of what was ahead, P), as HTML runs the two. */
static void loop_view_pushed_while_arriving(void) {
    loop_page("<p id=out>T</p><script>history.replaceState({t:1},'','/T')</script>");
    check(loop_settle() && view_turns(),"pushed while arriving: T");
    start_trip("http://fixture.test/P",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p id=out>P</p><script>history.replaceState(null,'','/P')</script>");
    check(loop_settle() && view_turns() && session_is("/T","/P",""),"pushed while arriving: on P");
    click_back(NULL,NULL);
    post_arriving("<p id=out>-</p><script>var was=JSON.stringify(history.state);"
        "history.replaceState(history.state,'','/T');history.pushState({n:2},'','/T2');document.getElementById('out').textContent=was+':'+history.length</script>");
    check(loop_settle() && view_turns() && session_is("/T","/T2","") && probe_text_is("out","{\"t\":1}:2") &&
          g.way.depth==1 && g.way.history[0].state!=NULL && os64_streq(g.way.history[0].state,"{\"t\":1}"),
        "pushed while arriving: the target entry once, the push after it, nothing ahead");
    click_back(NULL,NULL);
    check(!g.stream.active && session_is("","/T","/T2"),"pushed while arriving: and Back to T is in the document");
    loop_drop();
    // Pushed and then back among its own: it arrives at the target with its
    // push ahead, and P, which the push let go of, is not.
    loop_page("<p id=out>T</p><script>history.replaceState({t:1},'','/T')</script>");
    check(loop_settle() && view_turns(),"pushed and back: T");
    start_trip("http://fixture.test/P",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p id=out>P</p><script>history.replaceState(null,'','/P')</script>");
    check(loop_settle() && view_turns() && session_is("/T","/P",""),"pushed and back: on P");
    click_back(NULL,NULL);
    post_arriving("<p id=out>T</p><script>history.replaceState(history.state,'','/T');"
        "history.pushState({n:2},'','/T2')</script><script>history.back()</script><p>tail</p>");
    check(loop_settle() && view_turns() && session_is("","/T","/T2"),
        "pushed and back: the push's entry ahead, and P let go as the push let it go");
    loop_drop();
}
/* The second cold read of Codex round 1's fixes. A page that pushed before
 * it was shown let go of what was ahead of its entry: forward() past its
 * own entries is nothing, and it arrives, whether it came by Back or by a
 * reload. */
static void loop_view_pushed_then_forward(void) {
    loop_page("<p id=out>T</p><script>history.replaceState(null,'','/T')</script>");
    check(loop_settle() && view_turns(),"pushed then forward: T");
    start_trip("http://fixture.test/P",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p id=out>P</p><script>history.replaceState(null,'','/P')</script>");
    check(loop_settle() && view_turns() && session_is("/T","/P",""),"pushed then forward: on P, T behind");
    click_back(NULL,NULL);
    post_arriving("<p id=out>T</p><script>history.replaceState(null,'','/T');history.pushState(null,'','/T2');"
        "history.forward()</script><p>tail</p>");
    check(loop_settle() && view_turns() && !g.stream.active && probe_text_is("out","T") &&
          session_is("/T","/T2",""),
        "pushed then forward: by Back, the push let P go, and T arrives at its push");
    loop_drop();
    // The same from a reload of T with P ahead.
    loop_page("<p id=out>T</p><script>history.replaceState(null,'','/T')</script>");
    check(loop_settle() && view_turns(),"pushed then forward: T again");
    start_trip("http://fixture.test/P",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p id=out>P</p><script>history.replaceState(null,'','/P')</script>");
    check(loop_settle() && view_turns() && session_is("/T","/P",""),"pushed then forward: on P again");
    click_back(NULL,NULL);
    post_arriving("<p id=out>T</p><script>history.replaceState(null,'','/T')</script>");
    check(loop_settle() && view_turns() && session_is("","/T","/P"),"pushed then forward: on T, P ahead");
    click_reload(NULL,NULL);
    post_arriving("<p id=out>R</p><script>history.replaceState(null,'','/T');history.pushState(null,'','/T2');"
        "history.forward()</script><p>tail</p>");
    check(loop_settle() && view_turns() && !g.stream.active && probe_text_is("out","R") &&
          session_is("/T","/T2",""),
        "pushed then forward: by a reload, the same");
    loop_drop();
}
/* history.length read by an arriving page after it pushed counts only up
 * to its own entry, and its own: the push let go of what was ahead. It
 * reads the same while it arrives and once it is shown. */
static void loop_view_pushed_length(void) {
    static const char page[]="<p id=out>-</p><script>var log=[];history.replaceState(null,'','/T');"
        "history.pushState(null,'','/T2');log.push(history.length);"
        "onload=function(){log.push(history.length);document.getElementById('out').textContent=log.join()}</script>";
    loop_page("<p id=out>T</p><script>history.replaceState(null,'','/T')</script>");
    check(loop_settle() && view_turns(),"pushed length: T");
    start_trip("http://fixture.test/P",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p id=out>P</p><script>history.replaceState(null,'','/P')</script>");
    check(loop_settle() && view_turns() && session_is("/T","/P",""),"pushed length: on P, T behind");
    click_back(NULL,NULL);
    post_arriving(page);
    check(loop_settle() && view_turns() && session_is("/T","/T2","") && probe_text_is("out","2,2"),
        "pushed length: by Back, 2 while arriving and 2 once shown");
    loop_drop();
    // A reload of T with P ahead: the entry on screen again, then its push.
    loop_page("<p id=out>T</p><script>history.replaceState(null,'','/T')</script>");
    check(loop_settle() && view_turns(),"pushed length: T again");
    start_trip("http://fixture.test/P",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p id=out>P</p><script>history.replaceState(null,'','/P')</script>");
    check(loop_settle() && view_turns(),"pushed length: on P again");
    click_back(NULL,NULL);
    post_arriving("<p id=out>T</p><script>history.replaceState(null,'','/T')</script>");
    check(loop_settle() && view_turns() && session_is("","/T","/P"),"pushed length: on T, P ahead");
    click_reload(NULL,NULL);
    post_arriving(page);
    check(loop_settle() && view_turns() && session_is("/T","/T2","") && probe_text_is("out","2,2"),
        "pushed length: by a reload, the same");
    loop_drop();
}
/* A new way's page that pushed and goes back past its own entries before
 * it is shown: its commit let go of what was ahead of the page on screen,
 * so the session keeps its entries ahead and not F. */
static void loop_view_new_way_back(void) {
    loop_page("<p id=out>S</p><script>history.replaceState(null,'','/S')</script>");
    check(loop_settle() && view_turns(),"new way back: S");
    start_trip("http://fixture.test/F",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p id=out>F</p><script>history.replaceState(null,'','/F')</script>");
    check(loop_settle() && view_turns(),"new way back: F");
    click_back(NULL,NULL);
    post_arriving("<p id=out>S</p><script>history.replaceState(null,'','/S')</script>");
    check(loop_settle() && view_turns() && session_is("","/S","/F"),"new way back: on S, F ahead");
    start_trip("http://fixture.test/A",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p id=out>A</p><script>history.replaceState(null,'','/A');history.pushState(null,'','/A2');"
        "history.go(-2)</script><p>tail</p>");
    check(loop_settle() && view_turns() && !g.stream.active && probe_text_is("out","S") &&
          session_is("","/S","/A,/A2"),
        "new way back: go(-2) from A2 is S, A and A2 ahead, F let go");
    loop_drop();
}
/* A page not yet shown that navigates by address does so from its own
 * entry, which the navigation replaces: replace() does, and so does an
 * ordinary navigation before the page has completely loaded (HTML's
 * Location-object navigate). A redirector reached by a link leaves the
 * page it was reached from behind; one reached by Back is replaced, so
 * Back past it is the entry before it. */
static void unshown_redirect_case(const char *how) {
    char body[256], what[160];
    loop_page("<p id=out>S</p><script>history.replaceState(null,'','/S')</script>");
    check(loop_settle() && view_turns(),"unshown redirect: S");
    start_trip("http://fixture.test/R",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    os64_snprintf(body,sizeof(body),"<p id=out>R</p><script>history.replaceState(null,'','/R');%s</script><p>tail</p>",how);
    post_arriving(body);
    loop_settle();
    view_turns();
    os64_snprintf(what,sizeof(what),"unshown redirect: %s from a new way's page fetches X",how);
    check(g.stream.active && !probe_text_is("out","R"),what);
    post_arriving("<p id=out>X</p><script>history.replaceState(null,'','/X')</script>");
    os64_snprintf(what,sizeof(what),"unshown redirect: %s from a new way's page replaces it, S behind",how);
    check(loop_settle() && view_turns() && probe_text_is("out","X") && session_is("/S","/X",""),what);
    loop_drop();
}
static void unshown_redirect_back_case(const char *how) {
    char body[256], what[160];
    loop_page("<p id=out>U</p><script>history.replaceState(null,'','/U')</script>");
    check(loop_settle() && view_turns(),"unshown redirect back: U");
    start_trip("http://fixture.test/T",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p id=out>T</p><script>history.replaceState(null,'','/T')</script>");
    check(loop_settle() && view_turns(),"unshown redirect back: T");
    start_trip("http://fixture.test/S",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p id=out>S</p><script>history.replaceState(null,'','/S')</script>");
    check(loop_settle() && view_turns() && session_is("/U,/T","/S",""),"unshown redirect back: on S");
    click_back(NULL,NULL);
    os64_snprintf(body,sizeof(body),"<p id=out>T</p><script>history.replaceState(null,'','/T');%s</script><p>tail</p>",how);
    post_arriving(body);
    loop_settle();
    view_turns();
    post_arriving("<p id=out>X</p><script>history.replaceState(null,'','/X')</script>");
    os64_snprintf(what,sizeof(what),"unshown redirect back: %s from T reached by Back replaces T, S ahead",how);
    check(loop_settle() && view_turns() && probe_text_is("out","X") && session_is("/U","/X","/S"),what);
    click_back(NULL,NULL);
    post_arriving("<p id=out>U</p><script>history.replaceState(null,'','/U')</script>");
    os64_snprintf(what,sizeof(what),"unshown redirect back: %s, and Back from X is U",how);
    check(loop_settle() && view_turns() && probe_text_is("out","U") && session_is("","/U","/X,/S"),what);
    loop_drop();
}
static void loop_view_unshown_redirect(void) {
    unshown_redirect_case("location.replace('/X')");
    unshown_redirect_case("location.href='/X'");
    unshown_redirect_back_case("location.href='/X'");
    unshown_redirect_back_case("location.replace('/X')");
    unshown_redirect_back_case("document.addEventListener('DOMContentLoaded',function(){location.href='/X'})");
    // A redirector that pushed first: the push's entry is the one replaced,
    // and the entry it pushed from stays behind it.
    loop_page("<p id=out>S</p><script>history.replaceState(null,'','/S')</script>");
    check(loop_settle() && view_turns(),"unshown redirect pushed: S");
    start_trip("http://fixture.test/R",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    post_arriving("<p id=out>R</p><script>history.replaceState(null,'','/R');history.pushState(null,'','/R2');"
        "location.replace('/X')</script><p>tail</p>");
    loop_settle();
    view_turns();
    post_arriving("<p id=out>X</p><script>history.replaceState(null,'','/X')</script>");
    check(loop_settle() && view_turns() && probe_text_is("out","X") && session_is("/S,/R","/X",""),
        "unshown redirect pushed: X replaces R2, R and S behind");
    loop_drop();
}
/* The third cold read: a page shown has not completely loaded until its
 * `load` task is over, so a redirect from onload, <body onload> or a form
 * a script sends there replaces the page's entry, as HTML's
 * Location-object navigate and form submission do. A timer after load and
 * a person's click push. `how` is R's markup after its own name. */
static void loading_redirect_case(const char *how, const char *what, bool replaces) {
    char body[512], line[200];
    loop_page("<p id=out>S</p><script>history.replaceState(null,'','/S')</script>");
    check(loop_settle() && view_turns(),"loading redirect: S");
    start_trip("http://fixture.test/R",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    os64_snprintf(body,sizeof(body),"<p id=out>R</p><script>history.replaceState(null,'','/R')</script>%s",how);
    post_arriving(body);
    loop_settle();
    view_turns();
    if(!g.stream.active && probe_id("l")!=NULL)
        input_click_at("l");
    for(int i=0;i<4 && !g.stream.active;i++) { now_ms+=20; loop_settle(); view_turns(); }
    os64_snprintf(line,sizeof(line),"loading redirect: %s fetches X",what);
    check(g.stream.active,line);
    post_arriving("<p id=out>X</p><script>history.replaceState(null,'','/X')</script>");
    os64_snprintf(line,sizeof(line),"loading redirect: %s %s",what,replaces ? "replaces R, S behind" : "pushes, R behind");
    check(loop_settle() && view_turns() && probe_text_is("out","X") &&
          (replaces ? session_is("/S","/X","") : session_is("/S,/R","/X","")),line);
    if(replaces) {
        click_back(NULL,NULL);
        post_arriving("<p id=out>S</p><script>history.replaceState(null,'','/S')</script>");
        os64_snprintf(line,sizeof(line),"loading redirect: %s, and Back from X is S",what);
        check(loop_settle() && view_turns() && probe_text_is("out","S") && session_is("","/S","/X"),line);
    }
    loop_drop();
}
/* WHO ASKED, for a page's own navigation (Fetch Metadata § 2.3 and § 2.4):
 * the page's script, in a task after its load, with `markup` on the page
 * and `how` the script. Its site is from the page's own origin and it is
 * nobody's: no person's activation, whatever kind of navigation it is. */
static void page_initiator_case(const char *markup, const char *how, const char *what, const char *to) {
    char body[512], line[200];
    loop_page("<p id=out>S</p><script>history.replaceState(null,'','/S')</script>");
    check(loop_settle() && view_turns(),"page initiator: S");
    start_trip("http://fixture.test/R",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    os64_snprintf(body,sizeof(body),"<p id=out>R</p>%s<script>history.replaceState(null,'','/R');"
                  "onload=function(){setTimeout(function(){%s},0)}</script>",markup,how);
    post_arriving(body);
    loop_settle();
    view_turns();
    for(int i=0;i<4 && !g.stream.active;i++) { now_ms+=20; loop_settle(); view_turns(); }
    const yonder_trip_t *t=g.stream.active ? pool_work.job : NULL;
    size_t n=t!=NULL ? strlen(t->url) : 0, k=strlen(to);
    os64_snprintf(line,sizeof(line),"who: %s goes to %s as the page's own, and nobody's",what,to);
    check(t!=NULL && t->kind==YONDER_JOB_TRIP && n>=k && strcmp(t->url+n-k,to)==0 &&
          t->metadata.site==WAY_SITE_SAME_ORIGIN && !t->metadata.activation &&
          os64_streq(t->metadata.origin.host,"fixture.test"),line);
    loop_drop();
}
/* THE BROWSER'S RELOAD AND BACK REPLAY THE ENTRY'S OWN ASKING, as Chrome
 * 156 and Firefox 158 do (capture-26-chrome-win-get-clicked,
 * capture-27-chrome-win-get-root, capture-43-firefox-linux-get-clicked,
 * capture-44-firefox-linux-get-root). */
static const yonder_trip_t *trip_now(void) {
    return g.stream.active && pool_work.job!=NULL && *(const uint32_t *)pool_work.job==YONDER_JOB_TRIP
        ? pool_work.job : NULL;
}
static void loop_view_buttons_replay(void) {
    loop_page("<a id=l href=/C>c</a><script>history.replaceState(null,'','/T')</script>");
    check(loop_settle() && view_turns(),"replay: T, an address given");
    input_click_at("l");
    post_arriving("<p>C</p><script>history.replaceState(null,'','/C')</script>");
    check(loop_settle() && view_turns() && session_is("/T","/C",""),"replay: C, a link a person clicked");
    click_reload(NULL,NULL);
    const yonder_trip_t *t=trip_now();
    check(t!=NULL && t->metadata.site==WAY_SITE_SAME_ORIGIN && t->metadata.activation && t->metadata.reload &&
          os64_streq(t->metadata.origin.host,"fixture.test"),
        "replay: Reload of a clicked entry is the click again, as a reload");
    post_arriving("<p>C</p><script>history.replaceState(null,'','/C')</script>");
    check(loop_settle() && view_turns(),"replay: C again");
    check(!g.page.asked.reload && g.page.asked.activation,"replay: the reloaded entry keeps the click, not the reload");
    click_back(NULL,NULL);
    t=trip_now();
    check(t!=NULL && t->metadata.site==WAY_SITE_NONE && t->metadata.activation && !t->metadata.reload,
        "replay: Back to a typed entry is the address given again");
    post_arriving("<p>T</p><script>history.replaceState(null,'','/T')</script>");
    check(loop_settle() && view_turns(),"replay: T again");
    loop_drop();

    // An entry a page's script navigated to: Back replays the page's
    // asking, nobody's.
    loop_page("<p>S</p><script>history.replaceState(null,'','/S');"
              "onload=function(){setTimeout(function(){location.href='/P'},0)}</script>");
    loop_settle();
    view_turns();
    for(int i=0;i<4 && !g.stream.active;i++) { now_ms+=20; loop_settle(); view_turns(); }
    check(trip_now()!=NULL && !trip_now()->metadata.activation,"replay: the script's navigation to P");
    post_arriving("<a id=l href=/Q>q</a><script>history.replaceState(null,'','/P')</script>");
    check(loop_settle() && view_turns(),"replay: P, a script's");
    input_click_at("l");
    post_arriving("<p>Q</p><script>history.replaceState(null,'','/Q')</script>");
    check(loop_settle() && view_turns() && session_is("/S,/P","/Q",""),"replay: Q, a click from P");
    click_back(NULL,NULL);
    t=trip_now();
    check(t!=NULL && t->metadata.site==WAY_SITE_SAME_ORIGIN && !t->metadata.activation &&
          os64_streq(t->metadata.origin.host,"fixture.test"),
        "replay: Back to a script-navigated entry is the page's again, nobody's");
    loop_drop();
}
static void loop_view_page_initiators(void) {
    page_initiator_case("","location.reload()","location.reload()","/R");
    page_initiator_case("","history.back()","history.back()","/S");
    page_initiator_case("","history.go(-1)","history.go(-1)","/S");
    page_initiator_case("<a id=l href=/X>x</a>","document.getElementById('l').click()","a script's click()","/X");
    page_initiator_case("<form action=/X><input name=a value=1></form>","document.forms[0].submit()",
                        "a script's submit()","/X?a=1");
}
static void loop_view_loading_redirect(void) {
    loading_redirect_case("<script>onload=function(){location.href='/X'}</script>","onload's href",true);
    loading_redirect_case("<body onload=\"location.replace('/X')\">","<body onload>'s replace",true);
    loading_redirect_case("<form method=post action=/X><input name=a value=1></form>"
        "<script>onload=function(){document.forms[0].submit()}</script>","a form sent in onload",true);
    loading_redirect_case("<script>onload=function(){setTimeout(function(){location.href='/X'},0)}</script>",
        "a timer after load",false);
    loading_redirect_case("<a id=l href=/X>x</a>","a person's click once it is shown",false);
}
/* A page at its DOMContentLoaded whose replacing navigation cannot start
 * (the pool refuses the trip) still arrives, and with the entries its
 * scripts made. */
static os64_work_id_t refuse_trips(const os64_work_t *work) {
    if(*(const uint32_t *)work->job==YONDER_JOB_TRIP) return 0;
    pool_submit_hook=NULL;
    os64_work_id_t id=os64_work_submit(NULL,work);
    pool_submit_hook=refuse_trips;
    return id;
}
static void loop_view_replacing_refused(void) {
    loop_page("<p id=out>S</p><script>history.replaceState(null,'','/S')</script>");
    check(loop_settle() && view_turns(),"replacing refused: S");
    start_trip("http://fixture.test/R",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    pool_submit_hook=refuse_trips;
    post_arriving("<p id=out>R</p><script>history.replaceState(null,'','/R');history.pushState(null,'','/R2');"
        "document.addEventListener('DOMContentLoaded',function(){location.href='/X'})</script>");
    bool settled=loop_settle();
    pool_submit_hook=NULL;
    check(settled && view_turns() && !g.stream.active && probe_text_is("out","R") && session_is("/S,/R","/R2",""),
        "replacing refused: the page still arrives, with the entry it pushed from");
    loop_drop();
}
/* Codex round 2 on slice 1b. `scripting` answers the document's own state,
 * in a sheet and in matchMedia: enabled when the page's scripts are on,
 * none when they are off. */
static void loop_view_media_scripting(void) {
    static const char sheet[]="<style>#on,#off{display:none}@media (scripting){#on{display:block}}"
        "@media (scripting: none){#off{display:block}}</style><p id=on>on</p><p id=off>off</p>";
    for(int scripts=0;scripts<2;scripts++) {
        probe_page(sheet,scripts!=0);
        bool on=flow_box_for(g.page.tree,probe_id("on"))!=NULL, off=flow_box_for(g.page.tree,probe_id("off"))!=NULL;
        check(scripts ? on && !off : off && !on,
            scripts ? "media scripting: a sheet's (scripting) holds with scripts on"
                    : "media scripting: and (scripting: none) with them off");
        probe_drop();
    }
    loop_page("<p id=out>-</p><script>document.getElementById('out').textContent=[matchMedia('(scripting)').matches,"
        "matchMedia('(scripting: enabled)').matches,matchMedia('(scripting: none)').matches].join()</script>");
    check(loop_settle() && view_turns() && probe_text_is("out","true,true,false"),
        "media scripting: matchMedia answers enabled for a page whose scripts run");
    loop_drop();
}
/* Codex round 2: device-width, device-height and device-aspect-ratio are
 * the screen's (what `screen` reports), not the viewport's, in a sheet and
 * in matchMedia, and a window's size does not move them. A window that
 * read no screen answers libdom's default, as `screen` does, which is
 * what a replay answers. */
static void loop_view_media_device(void) {
    static const char page[]="<style>#d{display:none}@media (min-device-width: 1900px){#d{display:block}}</style>"
        "<p id=d>d</p><p id=out>-</p><script>var log=[];function o(){log.push([screen.width,"
        "matchMedia('(device-width: '+screen.width+'px)').matches,matchMedia('(device-height: '+screen.height+'px)').matches,"
        "matchMedia('(width: '+screen.width+'px)').matches].join(':'));document.getElementById('out').textContent=log.join()}"
        "o();setTimeout(o,100)</script>";
    loop_page(page);
    check(loop_settle() && view_turns() && probe_text_is("out","1024:true:true:false") &&
          flow_box_for(g.page.tree,probe_id("d"))==NULL,
        "media device: with no screen read, the default's, as screen reports it");
    loop_drop();
    s_screen=(os64_dom_screen_t){1920,1080,1920,1040,24};
    loop_page(page);
    bool shown=loop_settle() && view_turns() && probe_text_is("out","1920:true:true:false") &&
               flow_box_for(g.page.tree,probe_id("d"))!=NULL;
    check(shown,"media device: the screen's size, in a sheet and in matchMedia, not the viewport's");
    g.view.bounds.w=640;
    now_ms+=200; loop_settle(); view_turns();
    check(probe_text_is("out","1920:true:true:false,1920:true:true:false"),
        "media device: and a narrower window does not move it");
    loop_drop();
    s_screen=(os64_dom_screen_t){0};
}
/* The fourth cold read: `resolution` and the WebKit pixel ratio answer the
 * zoom, as devicePixelRatio does, in a sheet and in matchMedia. */
static void loop_view_media_resolution(void) {
    loop_page("<style>#r{display:none}@media (min-resolution: 2dppx){#r{display:block}}</style><p id=r>r</p>"
        "<p id=out>-</p><script>document.getElementById('out').textContent=[devicePixelRatio,"
        "matchMedia('(min-resolution: 2dppx)').matches,matchMedia('(-webkit-min-device-pixel-ratio: 2)').matches,"
        "matchMedia('(resolution: '+devicePixelRatio+'dppx)').matches].join()</script>");
    g.zoom=2000;
    check(loop_settle() && view_turns() && probe_text_is("out","2,true,true,true") &&
          flow_box_for(g.page.tree,probe_id("r"))!=NULL,
        "media resolution: at 200% the resolution is 2dppx, as devicePixelRatio says");
    loop_drop();
    loop_page("<p id=out>-</p><script>document.getElementById('out').textContent=[devicePixelRatio,"
        "matchMedia('(min-resolution: 2dppx)').matches,matchMedia('(-webkit-device-pixel-ratio: 1)').matches].join()</script>");
    check(loop_settle() && view_turns() && probe_text_is("out","1,false,true"),
        "media resolution: and at 100%, 1dppx");
    loop_drop();
}
static void loop_view_cold2_cases(void) {
    loop_view_media_resolution();
    loop_view_media_device();
    loop_view_media_scripting();
    loop_view_replacing_refused();
    loop_view_loading_redirect();
    loop_view_page_initiators();
    loop_view_buttons_replay();
    loop_view_pushed_then_forward();
    loop_view_pushed_length();
    loop_view_new_way_back();
    loop_view_unshown_redirect();
}
static void loop_view_cold_cases(void) {
    loop_view_cold2_cases();
    loop_view_extremes();
    loop_view_unshown_forward();
    loop_view_pushed_while_arriving();
    loop_view_reached();
    loop_view_refetch_short();
    loop_view_owed();
    loop_view_refetched();
    loop_view_identity();
    loop_view_unshown_back();
    loop_view_passed_over();
}
/* With the switch off the names are absent, and the window's own history
 * (a fragment link's entry) is the window's whatever the switch says. */
static void loop_view_off(void) {
    loop_modern_off=true;
    loop_page("<a id=l href=#t>t</a><h2 id=t>t</h2><p id=out>-</p><script>var heard=0;"
        "addEventListener('popstate',function(){heard++});addEventListener('scroll',function(){heard++});"
        "document.getElementById('out').textContent=[typeof matchMedia,typeof getComputedStyle,"
        "typeof history.pushState,typeof scrollTo,'scrollingElement' in document,typeof innerWidth].join()+':'+heard"
        "</script>");
    loop_modern_off=false;
    check(loop_settle() && g.page.tree!=NULL &&
          probe_text_is("out","undefined,undefined,undefined,undefined,false,undefined:0"),
        "modern off: the view's, the computed style's and the history's names are absent");
    input_click_at("l");
    check(view_turns() && g.way.depth==1 && os64_streq(g.page.way.url,"http://fixture.test/final#t"),
        "modern off: a fragment link is still an entry of the window's");
    check(view_turns() && probe_text_is("out","undefined,undefined,undefined,undefined,false,undefined:0"),
        "modern off: nothing is dispatched");
    loop_drop();
}
static void loop_view_cases(void) {
    loop_view_computed();
    loop_view_scroll();
    loop_view_media();
    loop_view_history();
    loop_view_history_short();
    loop_view_cold_cases();
    loop_view_off();
}
static void loop_cases(void) {
    loop_view_cases();
    loop_throw_after_queue();
    loop_string_timer_throws();
    loop_parse_first();
    loop_waits();
    loop_hash_unshown();
    loop_handler_runtime_at_click();
    loop_arrival_follows_scripts();
    loop_script_changes_hold();
    loop_document_cookie();
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
    loop_modern();
}
/* THE MODERN SWITCH, in the window: a page takes the switch its navigation
 * found, so its script surface has the arc's names or not, its record says
 * which (`modern: on` or `off`), and flipping the switch reaches the next
 * page and not the one on screen. */
static void loop_modern(void) {
    static const char page[]="<p id=out>start</p><script>document.getElementById('out').textContent="
        "typeof Element+' '+typeof Node+' '+('Element' in window);</script>";
    char record[8192];
    loop_page(page);
    check(loop_settle() && g.page.tree!=NULL && g.page.modern && probe_text_is("out","function function true"),
        "modern on: a page's scripts see the arc's names");
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"\nmodern: on\n")!=NULL,"modern on: the record says so, as a replay reads it");
    check(!garb_cascade_env(g.page.cascade).modern_off,"modern on: the page's cascade knows the arc's properties");
    modern_use(false);
    check(g.page.modern && !g.modern,"modern: Settings' switch leaves the page on screen as it was made");
    start_trip("http://fixture.test/next",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    check(g.stream.active && !g.stream.modern,"modern: the next navigation takes the switch as it now stands");
    loop_drop();
    loop_modern_off=true;
    loop_page(page);
    loop_modern_off=false;
    check(loop_settle() && g.page.tree!=NULL && !g.page.modern && probe_text_is("out","undefined undefined false"),
        "modern off: the arc's names do not exist, so feature detection finds them missing");
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"\nmodern: off\n")!=NULL,"modern off: the record says so, so it is never read as a regression");
    check(garb_cascade_env(g.page.cascade).modern_off,"modern off: the page's cascade does not know the arc's properties");
    loop_drop();
}

/* A page that arrives starts its animations at the layout that shows it,
 * and is judged by its own costs: the page before it was too slow to
 * animate. The page it replaced goes with its timeline. */
static void stream_motion(void) {
    stream_window();
    now_ms=5000;
    g.motion_laid=true; g.motion_us=UINT64_MAX;
    start_trip("http://fixture.test/p",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    static const char page[]="<!doctype html><style>@keyframes slide{to{margin-left:100px}}"
        "#a{animation:slide 1s infinite}</style><div id=a>x</div>";
    way_head_t h=stream_head("text/html","",false);
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_body((const uint8_t *)page,strlen(page),YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    while(stream_turn()) {}
    check(g.page.tree!=NULL && g.page.timeline!=NULL && !g.motion_laid && motion_next()==5000,
        "motion: an arriving page starts its animations at the layout that shows it, judged by its own cost");
    stream_window_drop();
}

static void stream_cases(void) {
    stream_motion();
    stream_captures_mode();
    stream_slices();
    stream_typed_is_the_browsers();
    stream_reaped_first();
    stream_text();
    stream_no_page();
    stream_picture_page();
    stream_refused();
    stream_stopped();
    stream_pool_breaks();
    stream_keeps_the_form();
    stream_mail_wiring();
    loop_cases();
}

static void geometry_cases(void)
{
    probe_page("<!doctype html><style>html,body{margin:0;padding:0}"
        "#outer{position:relative;width:200px;height:120px;border:4px solid;padding:8px}"
        "#box{position:absolute;left:20px;top:30px;width:100px;height:40px;padding:3px;border:2px solid}"
        "#fixed{position:fixed;left:11px;top:13px;width:17px;height:19px}"
        "</style><div id=outer><div id=box></div></div><div id=fixed></div><script>"
        "function eq(a,b){if(a!==b)throw Error(a+' != '+b)};"
        "var box=document.getElementById('box');var outer=document.getElementById('outer');"
        "eq(box.offsetWidth,110);eq(box.offsetHeight,50);eq(box.clientWidth,106);eq(box.clientHeight,46);"
        "eq(box.clientLeft,2);eq(box.clientTop,2);eq(box.offsetParent,outer);eq(box.offsetLeft,20);eq(box.offsetTop,30);"
        "var first=box.getBoundingClientRect();eq(first.width,110);eq(first.height,50);eq(first.right-first.left,110);"
        "box.setAttribute('style','width:140px');eq(box.offsetWidth,150);eq(first.width,110);"
        "var fresh=box.getBoundingClientRect();eq(fresh.width,150);eq(box.offsetWidth,150);"
        "eq(document.documentElement.clientWidth,800);eq(document.documentElement.clientHeight,600);"
        "var hidden=document.createElement('div');eq(hidden.offsetWidth,0);eq(hidden.offsetParent,null);"
        "eq(hidden.getBoundingClientRect().width,0);"
        "document.body.appendChild(hidden);hidden.setAttribute('style','display:none');eq(hidden.offsetHeight,0);"
        "box.setAttribute('data-geometry-pass','yes');"
        "</script>",true);
    check(script_turn(), "geometry script task completes and rebuilds");
    const os64_html_attr_t *passed=os64_html_attr(probe_id("box"),"data-geometry-pass");
    check(passed!=NULL && os64_streq(passed->value,"yes"), "geometry script reaches every numeric assertion");
    check(strstr(g.status_text,"forced 2 layouts") != NULL, "status reports forced layouts and repeated reads reuse them");
    os64_dom_geometry_t value;
    const os64_html_node_t *fixed=probe_id("fixed");
    check(yonder_geometry_snapshot(g.page.way.doc,g.page.tree,fixed,800,600,1000,
          (flow_point_t){7,9},&value) && value.x==11 && value.y==13 &&
          value.width==17 && value.offset_left==11 && value.offset_top==13 && value.offset_parent==NULL, "fixed viewport geometry ignores page scrolling");
    const os64_html_node_t *box=probe_id("box");
    check(yonder_geometry_snapshot(g.page.way.doc,g.page.tree,box,800,600,1000,
          (flow_point_t){7,9},&value) && value.x==17 && value.y==25,
          "ordinary bounding rectangle subtracts page scroll");
    g.zoom=2000;
    check(lay_out_page(&g.page,800,600), "geometry zoom layout");
    check(yonder_geometry_snapshot(g.page.way.doc,g.page.tree,box,800,600,2000,
          (flow_point_t){0,0},&value) && value.width==150 && value.client_width==146 &&
          value.offset_left==20, "geometry unzooms device layout to CSS pixels");
    probe_drop();
}

static void geometry_body_cases(void)
{
    probe_page("<!doctype html><div id=box style='height:20px'></div>",false);
    os64_dom_geometry_t value;
    check(yonder_geometry_snapshot(g.page.way.doc,g.page.tree,probe_id("box"),800,600,1000,
        (flow_point_t){0,0},&value) && value.offset_parent==g.page.way.doc->body &&
        value.offset_left==8 && value.offset_top==8,
        "static body offsets retain the default body margin from the document origin");
    probe_drop();
    probe_page("<!doctype html><style>body{position:relative;margin:8px;padding:5px;border:3px solid}</style>"
        "<div id=box style='height:20px'></div>",false);
    check(yonder_geometry_snapshot(g.page.way.doc,g.page.tree,probe_id("box"),800,600,1000,
        (flow_point_t){0,0},&value) && value.offset_parent==g.page.way.doc->body &&
        value.offset_left==5 && value.offset_top==5,
        "positioned body offsets still use its padding edge");
    probe_drop();
}

static void geometry_publish_cases(void)
{
    probe_page("<!doctype html><style>html,body{margin:0;padding:0}</style>"
        "<input id=field style='display:block;width:50vw;height:20px'><div style='height:900px;width:3000px'></div>",true);
    FormWidget *field=probe_field("field");
    int32_t old_width=field->w->bounds.w;
    g.view.bounds.w=400;
    g.relayout_due=true;
    g.sy=1000000;
    os64_dom_geometry_t value;
    check(script_geometry(g.page.way.doc,probe_id("field"),&value),"geometry forces a pending resize layout");
    const flow_box_t *box=flow_box_for(g.page.tree,probe_id("field"));
    os64_gui_rect_t rect=flow_box_doc_rect(box,scroll_now());
    check(field->w->bounds.w==rect.w && field->w->bounds.w<old_width,
        "forced geometry publishes resized control bounds before the pending relayout");
    check(g.sy<=page_height()-g.view.bounds.h,"forced geometry clamps the scroll to the new layout");
    check(g.vbar.total==page_height() && g.vbar.visible==g.view.bounds.h && g.vbar.pos==g.sy &&
        g.hbar.total==page_width() && g.hbar.visible==g.view.bounds.w && g.hbar.pos==g.sx,
        "forced geometry publishes scrollbar ranges and positions");
    g.sx=10;g.sy=0;g.zoom=2000;
    check(script_geometry(g.page.way.doc,probe_id("field"),&value) && g.sx==20,
        "forced geometry scales horizontal scroll when publishing a pending zoom");
    probe_drop();
}

static void geometry_scroll_cases(void)
{
    probe_page("<!doctype html><style>html,body{margin:0;padding:0}"
        "#outer{width:100px;height:80px;overflow:auto}"
        "#inner{width:100px;height:160px;overflow:auto}"
        "#target{margin-top:300px;width:30px;height:20px}"
        "</style><div id=outer><div id=inner><div id=target></div></div></div>",false);
    int32_t outer=flow_scroller_for(g.page.tree,probe_id("outer"));
    int32_t inner=flow_scroller_for(g.page.tree,probe_id("inner"));
    check(outer>=0 && inner>=0,"geometry nested scroll frames exist");
    flow_scroll_set(g.page.tree,outer,(flow_point_t){0,30});
    flow_scroll_set(g.page.tree,inner,(flow_point_t){0,40});
    os64_dom_geometry_t value;
    check(yonder_geometry_snapshot(g.page.way.doc,g.page.tree,probe_id("target"),800,600,1000,
        (flow_point_t){0,0},&value) && value.y==230 && value.offset_top==300,
        "nested scrolling moves viewport rect without changing offset position");
    probe_drop();
    probe_page("<!doctype html><style>html,body{margin:0;padding:0}"
        "#outer{width:100px;height:80px;overflow:auto}"
        "#before{height:100px}#target{position:sticky;top:0;height:20px}#after{height:300px}"
        "</style><div id=outer><div id=before></div><div id=target></div><div id=after></div></div>",false);
    outer=flow_scroller_for(g.page.tree,probe_id("outer"));
    flow_scroll_set(g.page.tree,outer,(flow_point_t){0,120});
    check(yonder_geometry_snapshot(g.page.way.doc,g.page.tree,probe_id("target"),800,600,1000,
        (flow_point_t){0,0},&value) && value.y==0 && value.offset_top==120,
        "sticky offset preserves current placement while ignoring container scroll");
    probe_drop();
}

static void geometry_fragment_cases(void)
{
    probe_page("<!doctype html><style>html,body{margin:0;padding:0}"
        "#wrap{width:40px}#target{font-size:16px}"
        "</style><div id=wrap><span id=target>aaaa aaaa aaaa</span></div>",false);
    os64_dom_geometry_t value;
    check(yonder_geometry_snapshot(g.page.way.doc,g.page.tree,probe_id("target"),800,600,1000,
        (flow_point_t){0,0},&value) && value.height>16 && value.width<=40 &&
        value.offset_height==value.height && value.client_width==0 && value.client_height==0,
        "multiline inline unions fragments and has no client box");
    probe_drop();
    probe_page("<!doctype html><style>html,body{margin:0;padding:0}"
        "table{border-spacing:0}caption{height:20px}td{padding:0;height:10px;width:30px}"
        "</style><table id=target><caption></caption><tr><td></td></tr></table>",false);
    check(yonder_geometry_snapshot(g.page.way.doc,g.page.tree,probe_id("target"),800,600,1000,
        (flow_point_t){0,0},&value) && value.height==30 && value.offset_height==30 && value.client_height==10,
        "table rectangle includes caption while client height describes table box");
    probe_drop();
}

static void geometry_failure_cases(void)
{
    probe_page("<div id=box style='width:100px;height:20px'></div><script>"
        "var box=document.getElementById('box');box.setAttribute('style','width:150px;height:20px');"
        "try{box.offsetWidth;box.setAttribute('data-stale','returned')}catch(e){"
        "if(e.name!=='InvalidStateError')throw e;box.setAttribute('data-refused','yes')}"
        "</script><script>if(box.offsetWidth!==150)throw Error('retry');"
        "box.setAttribute('data-retried','yes')</script>",true);
    uint64_t before=g.page.rendered_version;
    geometry_refuse_layout=true;
    script_turn();
    const os64_html_node_t *box=probe_id("box");
    check(os64_html_attr(box,"data-stale")==NULL && os64_html_attr(box,"data-refused")!=NULL &&
          g.page.rendered_version==before, "failed forced layout refuses stale dimensions and keeps its rendered version");
    check(yonder_scripts_geometry_stats(g.page.scripts,false).layouts==1,
          "failed forced layout is counted");
    geometry_refuse_layout=false;
    script_turn();
    check(os64_html_attr(box,"data-retried")!=NULL, "fresh geometry succeeds after an allocation refusal");
    probe_drop();
    probe_page("<div id=box></div><script>var box=document.getElementById('box');"
        "box.setAttribute('style','width:150px;height:20px');"
        "try{box.offsetWidth}catch(e){};try{box.offsetWidth}catch(e){};"
        "</script><script>box.setAttribute('data-after','bad')</script>",true);
    /* One native layout a second longer than the page's whole task budget. */
    int64_t overrun_us=((int64_t)g.script_ms+1000)*1000;
    geometry_layout_delay=overrun_us;
    script_turn();
    geometry_layout_delay=0;
    clock_us=10000000;
    os64_dom_geometry_stats_t stats=yonder_scripts_geometry_stats(g.page.scripts,false);
    check(stats.layouts==1 && stats.elapsed_us==(uint64_t)overrun_us && !yonder_scripts_pending(g.page.scripts),
          "native overrun retires the task queue and preserves its layout telemetry");
    char overrun_ms[32];
    snprintf(overrun_ms,sizeof(overrun_ms),"%ld.000 ms",(long)(overrun_us/1000));
    check(strstr(g.status_text,"execution stopped after")!=NULL &&
          strstr(g.status_text,overrun_ms)!=NULL, "status reports the charged native overrun with the overrun sentence");
    check(os64_html_attr(probe_id("box"),"data-after")==NULL,
          "later scripts do not run after a native geometry deadline");
    probe_drop();
}

static os64_js_runtime_t *teardown_fixture_runtime;
os64_js_status_t __wrap_os64_js_create_with_teardown(const os64_js_config_t *config,
    os64_js_teardown_policy_t policy, const char *abi, os64_js_runtime_t **out,
    os64_js_outcome_t *outcome)
{
    os64_js_status_t result = __real_os64_js_create_with_teardown(config, policy, abi, out, outcome);
    if (result == OS64_JS_OK) teardown_fixture_runtime = *out;
    return result;
}

static void reporting_teardown(void)
{
    check(yonder_scripts_teardown_leaks() == 0, "ordinary browser fixtures have no teardown leaks");
    check(teardown_log_count == 0, "ordinary browser retirement emits no reclaimed-leak log");
    size_t before = live;
    probe_page("<p id='heading'>keep the window</p><script>globalThis.held=document.getElementById('heading')</script>", true);
    os64_js_outcome_t outcome;
    check(yonder_scripts_step(g.page.scripts, &outcome) && outcome.status == OS64_JS_OK,
          "browser teardown fixture script runs");
    JSContext *context = os64_js_context(teardown_fixture_runtime, OS64_JS_ABI_ID, &outcome);
    JSValue global = JS_GetGlobalObject(context);
    JSValue held = JS_GetPropertyStr(context, global, "held");
    check(JS_IsObject(held), "browser fixture deliberately loses a retained DOM wrapper");
    JS_FreeValue(context, global);
    /* The lost value cannot be touched after destroy. Native holds must still
     * drain even though this wrapper's finalizer cannot run. */
    probe_drop();
    teardown_fixture_runtime = NULL;
    check(yonder_scripts_teardown_leaks() == 1 && live == before,
          "browser leak is counted and engine/native storage is reclaimed");
    unsigned long reclaimed_blocks = 0, reclaimed_bytes = 0;
    check(teardown_log_count == 1 &&
          sscanf(teardown_log,
                 "Yonder: reclaimed JavaScript teardown leak at https://fixture.test/page (%lu blocks, %lu bytes)",
                 &reclaimed_blocks, &reclaimed_bytes) == 2 && reclaimed_blocks > 0 && reclaimed_bytes > 0,
          "kernel log receives the reclaimed leak's page URL and block/byte totals once");
    /* Reuse the rendered text so the shared glyph cache does not grow while
     * we compare page-owned storage against the pre-navigation baseline. */
    probe_page("<p id='heading'>keep the window</p><script>globalThis.nextPage=true</script>", true);
    check(yonder_scripts_step(g.page.scripts, &outcome) && outcome.status == OS64_JS_OK,
          "browser can execute the next page after a reclaimed leak");
    probe_drop();
    teardown_fixture_runtime = NULL;
    check(yonder_scripts_teardown_leaks() == 1 && live == before,
          "clean navigation neither adds a leak nor retains page storage");
    check(teardown_log_count == 1, "clean next-page retirement emits no additional reclaimed-leak log");
}

/* The worker's answer to the sheet job whose address ends with `tail`: the
 * sheet `css`, or NULL for a fetch that failed. False when no such job is
 * out. */
static bool sheet_land(const char *tail, const char *css) {
    for(int i=0;i<nsheet_jobs;i++) {
        yonder_sheet_job_t *job=sheet_jobs[i].job;
        size_t n=strlen(job->url), t=strlen(tail);
        if(n<t || strcmp(job->url+n-t,tail)!=0) continue;
        check(job->hooks.metadata.dest==WAY_DEST_STYLE && job->hooks.metadata.mode==WAY_MODE_NO_CORS,
            "who: a sheet is asked for as a no-cors style");
        os64_work_id_t id=sheet_jobs[i].id;
        sheet_jobs[i]=sheet_jobs[--nsheet_jobs];
        yonder_sheet_t *got=os64_calloc(1,sizeof(*got));
        if(css!=NULL) got->ok=garb_parse_sheet_text(css,strlen(css),&got->parsed)==GARB_OK;
        os64_strcopy(got->url,sizeof(got->url),job->url);
        reaped(id,job,got);
        return true;
    }
    return false;
}
static bool sheet_out(const char *tail) {
    for(int i=0;i<nsheet_jobs;i++) {
        const char *url=((yonder_sheet_job_t *)sheet_jobs[i].job)->url;
        size_t n=strlen(url), t=strlen(tail);
        if(n>=t && strcmp(url+n-t,tail)==0) return true;
    }
    return false;
}

/* THE SHEETS BEFORE A SCRIPT (DOM_D7.md § The cut, D7d): the parse sends
 * for the sheets it finds as it finds them, and a script the parse is
 * stopped at waits for those still out. Each script writes the box's
 * width into data-w; the sheet makes it 300. */
#define D7D_MEASURE "<div id=box style='height:10px'></div><script>var b=document.getElementById('box');" \
    "b.setAttribute('data-w',String(b.offsetWidth))</script>"
static const char *box_width(void) {
    const os64_html_node_t *box=probe_id_in(page_doc(&g.page),"box");
    const os64_html_attr_t *a=box!=NULL ? os64_html_attr(box,"data-w") : NULL;
    return a!=NULL ? a->value : "";
}
/* The box's data-w wherever the page is now: still streaming, waiting for
 * its sheets, or shown. */
static const char *stream_box_width(void) {
    const os64_html_document_t *doc=g.stream.parser!=NULL ? os64_html_parser_document(g.stream.parser)
        : g.coming.active ? page_doc(&g.coming.page) : page_doc(&g.page);
    const os64_html_node_t *box=probe_id_in(doc,"box");
    const os64_html_attr_t *a=box!=NULL ? os64_html_attr(box,"data-w") : NULL;
    return a!=NULL ? a->value : "";
}
static void sheets_wait_cases(void) {
    /* A slow link above a measuring script: sent for while the body is
     * still in the mailbox, the script waits, and answers styled. */
    /* More body than one of the mailbox's chunks holds. */
    static char page[3*YONDER_STREAM_CHUNK], tail[2*YONDER_STREAM_CHUNK];
    memset(tail,'x',sizeof(tail)-1); tail[sizeof(tail)-1]='\0';
    snprintf(page,sizeof(page),"<link rel=stylesheet href=s.css>" D7D_MEASURE "<p>%s</p>",tail);
    unsigned submits=sheet_submits;
    loop_page(page);
    check(loop_settle() && g.stream.stopped && sheet_out("/s.css") && g.stream.mail->first!=NULL,
        "sheets: a link is sent for at the first stop, while the body is still in the mailbox");
    check(stream_box_width()[0]=='\0',"sheets: the script below it waits for it");
    check(sheet_land("/s.css","#box{width:300px}") && loop_settle() && g.page.tree!=NULL &&
          os64_streq(box_width(),"300"),
        "sheets: the sheet lands, and the script measures the styled box");
    check(sheet_submits==submits+1,"sheets: one job for the sheet across the page's life, from the stop to the arrival");
    check(g.page.nsheets==1 && g.page.sheets[0].ready && flow_box_for(g.page.tree,probe_id("box"))->rect.w==300,
        "sheets: the arrived page has the sheet the stream sent for, in its cascade");
    loop_drop();
    /* A link in the first slice is sent for after that slice, with no
     * stop to find it: the first script is further down than one slice
     * reaches (Wikipedia's head has its links after its first scripts). */
    static char deep[8*YONDER_STREAM_CHUNK], filler[6*YONDER_STREAM_CHUNK];
    memset(filler,'y',sizeof(filler)-1); filler[sizeof(filler)-1]='\0';
    snprintf(deep,sizeof(deep),"<link rel=stylesheet href=s.css><p>%s</p>" D7D_MEASURE,filler);
    loop_page(deep);
    stream_turn();
    check(!g.stream.stopped && sheet_out("/s.css") && g.stream.mail->first!=NULL,
        "sheets: a slice that reveals a link sends for it, with no stop and the body still coming");
    loop_drop();
    /* A style element's @import on the stream's page resolves against the
     * base a model would have, and the script below waits for it too. */
    loop_page("<base href=http://other.test/css/><style>@import 'i.css';</style>" D7D_MEASURE "<p>tail</p>");
    check(loop_settle() && g.stream.stopped && sheet_out("http://other.test/css/i.css") &&
          stream_box_width()[0]=='\0',
        "sheets: a style's @import is sent for against the page's base, and holds the script below");
    check(sheet_land("/i.css","#box{width:300px}") && loop_settle() && os64_streq(stream_box_width(),"300"),
        "sheets: and the script measures what it imported");
    loop_drop();
    /* A style that straddles the slice boundary is taken whole: its text
     * so far is not parsed as the sheet (Fable's probe, PR #233). */
    static char split[8*YONDER_STREAM_CHUNK], comment[5*YONDER_STREAM_CHUNK];
    memset(comment,'z',sizeof(comment)-1); comment[sizeof(comment)-1]='\0';
    snprintf(split,sizeof(split),"<style>/*%s*/ #box{width:300px}</style>" D7D_MEASURE "<p>tail</p>",comment);
    loop_page(split);
    check(loop_settle() && g.page.tree!=NULL && os64_streq(box_width(),"300") &&
          flow_box_for(g.page.tree,probe_id("box"))->rect.w==300,
        "sheets: a style split by a slice is parsed whole, for the script and on the shown page");
    loop_drop();
    /* A sheet that never lands holds one wait, not one per script: at
     * expiry it lapses, and neither a later script nor the first paint
     * waits for it again. */
    loop_page("<link rel=stylesheet href=never.css><script>var one=1</script>" D7D_MEASURE "<p>tail</p>");
    check(loop_settle() && g.stream.stopped && stream_box_width()[0]=='\0',"sheets: the first script waits");
    now_ms+=SHEETS_WAIT_MS;
    check(loop_settle() && strcmp(stream_box_width(),"")!=0 && g.page.tree!=NULL && !g.coming.active,
        "sheets: at expiry the sheet lapses; the second script runs at once and the page is shown without it");
    loop_drop();
    /* The same script above the link runs at once. */
    loop_page(D7D_MEASURE "<link rel=stylesheet href=s.css><p>tail</p>");
    check(loop_settle() && !g.stream.active && strcmp(stream_box_width(),"")!=0 &&
          strcmp(stream_box_width(),"300")!=0,
        "sheets: a script above the link does not wait for it");
    loop_drop();
    /* A print sheet holds nothing; a failed one holds nothing once it fails. */
    loop_page("<link rel=stylesheet media=print href=p.css>" D7D_MEASURE "<p>tail</p>");
    check(loop_settle() && !g.stream.active && sheet_out("/p.css") && strcmp(stream_box_width(),"")!=0,
        "sheets: a link whose media does not hold on this glass is fetched and not waited for");
    loop_drop();
    loop_page("<link rel=stylesheet href=gone.css>" D7D_MEASURE "<p>tail</p>");
    check(loop_settle() && g.stream.stopped && sheet_land("/gone.css",NULL) && loop_settle() &&
          g.page.tree!=NULL && strcmp(box_width(),"")!=0,
        "sheets: a link whose fetch fails holds nothing from then on");
    loop_drop();
    /* The wait is bounded: past SHEETS_WAIT_MS the script runs with what came. */
    loop_page("<link rel=stylesheet href=slow.css>" D7D_MEASURE "<p>tail</p>");
    check(loop_settle() && g.stream.stopped && stream_box_width()[0]=='\0',"sheets: waiting for a slow sheet");
    now_ms+=SHEETS_WAIT_MS;
    check(loop_settle() && strcmp(stream_box_width(),"")!=0 && strcmp(stream_box_width(),"300")!=0,
        "sheets: the wait expires and the script runs with the sheets that came");
    loop_drop();
    /* Each stop waits its own SHEETS_WAIT_MS, not what is left of an
     * earlier one's. */
    loop_page("<link rel=stylesheet href=a.css><script>var one=1</script>"
        "<link rel=stylesheet href=b.css>" D7D_MEASURE "<p>tail</p>");
    check(loop_settle() && g.stream.stopped,"sheets: the first script waits");
    now_ms+=SHEETS_WAIT_MS-1000;
    check(sheet_land("/a.css","") && loop_settle() && g.stream.stopped && sheet_out("/b.css"),
        "sheets: its sheet lands, it runs, and the second stops behind the next link");
    now_ms+=2000;
    check(loop_settle() && g.stream.stopped && stream_box_width()[0]=='\0',
        "sheets: the second script's wait starts at its own stop");
    loop_drop();
    /* A style a mid-parse script adds is in the arrived page's table once;
     * a style a mid-parse script removes is not in its cascade. */
    loop_page("<style id=gone>#box{width:50px}</style><script>"
        "var s=document.createElement('style');s.textContent='#box{width:120px}';document.head.appendChild(s);"
        "document.getElementById('gone').remove();</script><div id=box style='height:10px'></div><script></script><p>tail</p>");
    check(loop_settle() && g.page.tree!=NULL && flow_box_for(g.page.tree,probe_id("box"))->rect.w==120,
        "sheets: a style a script adds applies and one it removes does not");
    int added=0;
    for(int32_t e=0;e<g.page.nsheets;e++) added+=g.page.sheets[e].node!=NULL;
    check(added==2,"sheets: each style element has one entry, however many looks found it");
    loop_drop();
    /* Found out of document order, applied in it: the style a script puts
     * above an earlier one loses to it, as it comes first. */
    loop_page("<style id=a>#box{width:200px}</style><script>var s=document.createElement('style');"
        "s.textContent='#box{width:100px}';document.head.insertBefore(s,document.getElementById('a'));</script>"
        "<div id=box style='height:10px'></div><script></script><p>tail</p>");
    check(loop_settle() && g.page.tree!=NULL && flow_box_for(g.page.tree,probe_id("box"))->rect.w==200,
        "sheets: the cascade takes the sheets in document order, not the order they were found");
    loop_drop();
    /* The end of the parse looks again: a style below the last script is
     * in the table DOMContentLoaded measures against. */
    loop_page("<script>document.addEventListener('DOMContentLoaded',function(){var b=document.getElementById('box');"
        "b.setAttribute('data-w',String(b.offsetWidth))})</script>"
        "<style>#box{width:300px}</style><div id=box style='height:10px'></div>");
    check(loop_settle() && g.page.tree!=NULL && os64_streq(box_width(),"300"),
        "sheets: the end of the parse finds the sheets below the last script, before DOMContentLoaded");
    loop_drop();
    /* A written link before a written script holds it (D9). */
    loop_page("<script>document.write('<link rel=stylesheet href=w.css><div id=box style=\"height:10px\"></div>"
        "<scr'+'ipt>var b=document.getElementById(\"box\");b.setAttribute(\"data-w\",String(b.offsetWidth))</scr'+'ipt>')"
        "</script><p>tail</p>");
    check(loop_settle() && g.stream.stopped && sheet_out("/w.css") && stream_box_width()[0]=='\0',
        "sheets: a written link before a written script holds it");
    check(sheet_land("/w.css","#box{width:300px}") && loop_settle() && g.page.tree!=NULL &&
          os64_streq(box_width(),"300"),"sheets: and the written script measures it styled");
    loop_drop();
    /* Teardown mid-wait, three ways: Stop, scripts off, the window's close. */
    for(int how=0;how<3;how++) {
        size_t before=live;
        loop_page("<link rel=stylesheet href=s.css>" D7D_MEASURE "<p>tail</p>");
        check(loop_settle() && g.stream.stopped && sheet_out("/s.css"),"sheets: waiting for a sheet");
        if(how==0) click_stop(NULL,NULL);
        else if(how==1) settings_use(YONDER_AGENT,false,5);
        if(how<2) check(!g.stream.active && nsheet_jobs==0,
            how==0 ? "sheets: Stop mid-wait cancels the sheet's job"
                   : "sheets: turning scripts off mid-wait cancels it too");
        loop_drop();
        check(nsheet_jobs==0,"sheets: no sheet job outlives its page");
        (void)before;
    }
}

/* THE JOIN (DOM_D7.md § The cut, D7c): D8's reporting teardown, D10's
 * geometry and D7's loop, proven together through the real stream. */

/* Whether the audit kept a line for a task of kind `what` that forced
 * exactly `layouts` layouts. */
static bool audit_forced(const char *what, unsigned layouts) {
    char head[96], count[48];
    snprintf(head,sizeof(head),"yonder: task %s",what);
    snprintf(count,sizeof(count),"forced %u layouts in",layouts);
    for(const char *line=audit_lines;*line;) {
        const char *end=strchr(line,'\n');
        size_t n=end ? (size_t)(end-line) : strlen(line);
        char one[512];
        snprintf(one,sizeof(one),"%.*s",(int)n,line);
        char after=one[strlen(head)];
        if(strncmp(one,head,strlen(head))==0 && (after==' ' || after==':') && strstr(one,count)!=NULL)
            return true;
        line+=n+(end!=NULL);
    }
    return false;
}

/* The lost-wrapper page of D8 with a listener installed and a timer
 * pending: its retirement, by the window's close mid-life, is one log line
 * and one count, the next page runs, and nothing is left live. */
static void join_lost_wrapper(void) {
    const char *quiet="<p id=heading>keep the window</p><script>globalThis.nextPage=true;"
        "document.getElementById('heading').setAttribute('data-ran','yes')</script>";
    /* Warm the shared glyph cache with the text both pages draw. */
    loop_page(quiet);
    check(loop_settle() && g.page.tree!=NULL,"join: the warm-up page arrives");
    loop_drop();
    size_t leaks=yonder_scripts_teardown_leaks(), logged=teardown_log_count, before=live;
    loop_page("<p id=heading>keep the window</p><script>globalThis.held=document.getElementById('heading');"
        "document.addEventListener('click',function(){held.textContent='clicked'});"
        "setTimeout(function(){held.textContent='late'},100000);</script>");
    check(loop_settle() && g.page.tree!=NULL && yonder_scripts_alive(g.page.scripts) &&
          yonder_scripts_timer_next(g.page.scripts)!=YONDER_NEVER,
        "join: the lost-wrapper page arrives with a listener and a timer pending");
    os64_js_outcome_t outcome;
    JSContext *context=os64_js_context(teardown_fixture_runtime,OS64_JS_ABI_ID,&outcome);
    JSValue global=JS_GetGlobalObject(context);
    JSValue held=JS_GetPropertyStr(context,global,"held");
    check(JS_IsObject(held),"join: the page's wrapper is deliberately lost");
    JS_FreeValue(context,global);
    loop_drop();
    teardown_fixture_runtime=NULL;
    check(yonder_scripts_teardown_leaks()==leaks+1 && teardown_log_count==logged+1 &&
          strstr(teardown_log,"at http://fixture.test/final (")!=NULL,
        "join: its retirement is one count and one log line naming the page");
    check(live==before,"join: the listener, the timer and the lost wrapper leave nothing live");
    loop_page(quiet);
    check(loop_settle() && g.page.tree!=NULL && os64_html_attr(probe_id("heading"),"data-ran")!=NULL,
        "join: the next page runs its script");
    loop_drop();
    teardown_fixture_runtime=NULL;
    check(yonder_scripts_teardown_leaks()==leaks+1 && live==before,
        "join: the next page neither leaks nor keeps anything");
}

/* Measurements on pages not on screen: the parse so far at the view's size,
 * DOMContentLoaded's laid-out number, and a page waiting for its sheets.
 * None of them is published. */
static void join_unshown_geometry(void) {
    /* The widths come from a style element, which a page with no sheet
     * table yet is measured against. */
    loop_page("<head><style>body{margin:0}#box{width:50%;height:10px}#later{width:120px;padding:4px}</style>"
        "<script>document.documentElement.setAttribute('data-w',"
        "String(document.documentElement.clientWidth))</script></head>"
        "<body><div id=box></div><script>var b=document.getElementById('box');"
        "b.setAttribute('data-w',String(b.offsetWidth));"
        "b.setAttribute('data-later',String(document.getElementById('later')===null));"
        "document.addEventListener('DOMContentLoaded',function(){"
        "document.getElementById('later').setAttribute('data-w',"
        "String(document.getElementById('later').offsetWidth))});"
        "</script><div id=later></div></body>");
    check(loop_settle() && g.page.tree!=NULL,"join: the measuring page arrives");
    const os64_html_node_t *root=page_doc(&g.page)->html;
    /* The view's width, and half of it: the style element took the body's
     * margins away. */
    char view[16], half[16];
    snprintf(view,sizeof(view),"%d",(int)g.view.bounds.w);
    snprintf(half,sizeof(half),"%d",(int)g.view.bounds.w/2);
    check(root!=NULL && attr_is(root,"data-w",view),
        "join: a head script, before there is a body, measures the view's width");
    check(attr_is(probe_id("box"),"data-w",half) &&
          attr_is(probe_id("box"),"data-later","true"),
        "join: a mid-parse script measures an element before it at the view's size, the rest unparsed");
    check(attr_is(probe_id("later"),"data-w","128"),
        "join: a DOMContentLoaded listener reads the laid-out number");
    check(s_measured.doc==NULL,"join: the measured layout is let go once the page is shown");
    loop_drop();
    /* A page waiting for its sheet measures itself beside itself: the
     * sheet it has applies, and nothing of its own is laid out. */
    loop_page("<style>#box{width:200px}</style><div id=box></div><script>"
        "setTimeout(function(){var b=document.getElementById('box');"
        "b.setAttribute('data-w',String(b.offsetWidth))},10);</script><link rel=stylesheet href=wait.css>");
    check(loop_settle() && g.coming.active,"join: a page waits for its sheet");
    now_ms+=50;
    check(loop_settle() && g.coming.active && g.coming.page.tree==NULL && g.coming.page.cascade==NULL &&
          attr_is(probe_id_in(page_doc(&g.coming.page),"box"),"data-w","200"),
        "join: its timer measures it at the view's size, and the waiting page is not laid out");
    coming_show();
    check(g.page.tree!=NULL && s_measured.doc==NULL,"join: the waiting page is shown");
    loop_drop();
}

/* Every task kind counts the layouts it forced from zero and says so in
 * the audit: a blocking script, a connected one, a ready one,
 * DOMContentLoaded, load, a timer and an event. */
static void join_every_kind_counts(void) {
    s_script_audit=true;
    audit_keep=true;
    audit_lines[0]='\0';
    loop_page("<div id=box style='width:10px;height:10px'></div><script>var b=document.getElementById('box');"
        "function m(w){b.setAttribute('style','height:10px;width:'+w+'px');return b.offsetWidth}"
        "m(11);document.addEventListener('DOMContentLoaded',function(){m(12)});"
        "window.addEventListener('load',function(){m(13)});setTimeout(function(){m(14)},10);"
        "b.addEventListener('click',function(){m(15)});"
        "var s=document.createElement('script');s.textContent='m(16)';document.body.appendChild(s);"
        "</script><script async src=a.js></script><script src=b.js></script><p>tail</p>");
    check(loop_settle() && g.stream.stopped && script_land("/a.js","m(17)") && script_land("/b.js","0"),
        "join: every kind of task is on its way");
    check(loop_settle() && g.page.tree!=NULL,"join: the counting page arrives");
    now_ms+=50;
    check(script_turn() && status_says("Script forced 1 layouts in"),
        "join: a timer that forces a layout says so on the status line");
    input_click_at("box");
    check(attr_is(probe_id("box"),"style","height:10px;width:15px"),
        "join: the click listener ran");
    check(audit_forced("script http://fixture.test/final#inline-1",1) &&
          audit_forced("script http://fixture.test/final#inline-2",1) &&
          audit_forced("script http://fixture.test/a.js",1),
        "join: blocking, connected and ready scripts each count their own layout");
    check(audit_forced("DOMContentLoaded",1) && audit_forced("load",1) && audit_forced("timer",1) &&
          audit_forced("click",1),
        "join: DOMContentLoaded, load, a timer and an event each count their own layout");
    audit_keep=false;
    s_script_audit=false;
    loop_drop();
}

/* A click listener that runs out its time inside a forced layout: the
 * overrun sentence after the count, the runtime retired, and the link it
 * was on still followed by its default action. */
static void join_click_overrun(void) {
    loop_page("<a id=go href=next.html>go</a><div id=box></div><script>"
        "document.getElementById('go').addEventListener('click',function(){"
        "var b=document.getElementById('box');b.setAttribute('style','width:300px');b.offsetWidth;"
        "b.setAttribute('data-after','bad')});</script>");
    check(loop_settle() && g.page.tree!=NULL && !g.stream.active,"join: the overrunning page arrives");
    geometry_layout_delay=((int64_t)g.script_ms+1000)*1000;
    input_click_at("go");
    geometry_layout_delay=0;
    clock_us=10000000;
    /* The navigation the link starts writes the status line after it; the
     * page keeps what its scripts said. */
    const char *said=yonder_scripts_note(g.page.scripts);
    check(said!=NULL && strstr(said,"Script forced 1 layouts in")==said &&
          strstr(said,"; Script click event: execution stopped after 5 s (this page runs without script)")!=NULL,
        "join: the overrun inside a forced layout gets the count and the overrun sentence");
    check(!yonder_scripts_alive(g.page.scripts) && os64_html_attr(probe_id("box"),"data-after")==NULL,
        "join: the runtime is retired and the listener stops where it was");
    check(g.stream.active && pool_work.job!=NULL &&
          os64_streq(((yonder_trip_t *)pool_work.job)->url,"http://fixture.test/next.html"),
        "join: the link still navigates by its default action");
    if(pool_work.job!=NULL) {
        const yonder_trip_t *t=pool_work.job;
        check(t->metadata.site==WAY_SITE_SAME_ORIGIN && t->metadata.activation &&
              os64_streq(t->metadata.origin.host,"fixture.test"),
            "who: a link a person clicked is the page's site and a person's");
    }
    loop_drop();
}

/* One measured layout at a time: measuring a second page lets go of the
 * first's, whichever page it was, so both documents free cleanly and
 * nothing is left live. */
static void join_one_measured_layout(void) {
    stream_window();
    static const char a[]="<!doctype html><style>p{width:100px}</style><p id=p>first</p>";
    static const char b[]="<!doctype html><style>p{width:200px}</style><p id=p>second</p>";
    os64_html_document_t *docs[2]={parse_file((const uint8_t *)a,sizeof(a)-1,true),
                                   parse_file((const uint8_t *)b,sizeof(b)-1,true)};
    size_t before=live;
    os64_page_state_t *states[2];
    bool measured=true;
    for(int i=0;i<2;i++) {
        states[i]=os64_page_state_create(docs[i],0);
        Page page; memset(&page,0,sizeof(page));
        page.way.doc=docs[i]; page.scripting=true;
        measured=measured && measured_layout(&page,states[i],"http://fixture.test/",800,600);
    }
    check(measured && s_measured.doc==docs[1],"join: a second page's measurement replaces the first's");
    measured_forget(docs[1]);
    for(int i=0;i<2;i++) { os64_page_state_free(states[i]); os64_html_document_free(docs[i]); }
    check(live<=before && s_measured.doc==NULL,
        "join: both documents free with nothing of the first measurement left live");
    stream_window_drop();
}

static void join_cases(void) {
    sheets_wait_cases();
    join_one_measured_layout();
    join_unshown_geometry();
    join_every_kind_counts();
    join_click_overrun();
    join_lost_wrapper();
}

/* A frame a script inserts is the stand-in a parsed one is: it names its
 * document, and a click there goes to it, however the script gave it its
 * address. The `src` property, the way scripts most often do, is the
 * frames row's (libdom frames.c), so with the modern switch off it makes a
 * property of the script's own and the frame names nothing, as before;
 * setAttribute and innerHTML name it either way. */
static void inserted_frame_case(const char *how, const char *html, bool modern, bool named)
{
    unsigned before=failures;
    probe_modern_off=!modern;
    probe_page(html,true);
    probe_modern_off=false;
    while(yonder_scripts_pending(g.page.scripts)) script_turn();
    const os64_html_node_t *f=probe_id("f");
    int32_t link=f!=NULL ? os64_page_link_for(page_model(&g.page),f) : -1;
    const os64_page_link_t *l=link>=0 ? os64_page_link(page_model(&g.page),link) : NULL;
    if(named) {
        check(l!=NULL && l->href.url!=NULL && os64_streq(l->href.url,"https://fixture.test/poll"),
            "inserted frame: it names its document as a parsed one does");
        check(link>=0 && link_at(20,20)==link,"inserted frame: a click on its stand-in goes to its document");
    } else {
        check(f!=NULL && link<0 && link_at(20,20)<0,
            "inserted frame: with the switch off, src set as a property names nothing, as before");
    }
    probe_drop();
    if(failures!=before) fprintf(stderr,"inserted frame: %s, the switch %s\n",how,modern ? "on" : "off");
}
/* The page in the window painted whole into an 800 by 600 screen, through
 * the real verbs (view_paint's panes): its pixels, the caller's to free. */
static uint32_t *stand_in_shot(void) {
    os64_draw_ctx_t screen={0};
    screen.surf.width=screen.surf.pitch_px=800; screen.surf.height=600;
    screen.surf.pixels=os64_calloc(800u*600u,sizeof(uint32_t));
    g.paint_cache=yonder_paint_cache_new(32u<<20); g.paint_scratch=yonder_scratch_new();
    g.ui.dirty=g.view.bounds;
    view_paint(&g.view,&screen,NULL);
    yonder_paint_cache_free(g.paint_cache); g.paint_cache=NULL;
    yonder_scratch_free(g.paint_scratch); g.paint_scratch=NULL;
    return screen.surf.pixels;
}
/* THE STAND-INS' OWN PAINTING (yonder.c's pane_stand_in, stand_in_label,
 * glass_stand_in), which the yonder host's recorder stands in for: what a
 * label says, that a canvas has no paper, and what laying labels out costs. */
static void stand_in_paint_cases(void)
{
    /* A full-viewport canvas over the page, as confetti and snow are: the
     * page shows through, and only the outline and the corner label are
     * the stand-in's. */
    probe_page("<!doctype html><style>html,body{margin:0;background:#fff}p{margin:200px 0 0 300px}"
        "canvas{position:fixed;left:0;top:0;width:800px;height:600px}</style>"
        "<p>Readable text</p><canvas id=c></canvas>",true);
    uint32_t *px=stand_in_shot();
    int dark=0, grey=0;
    for(int32_t y=195;y<240;y++) for(int32_t x=295;x<420;x++) {
        uint32_t p=px[(size_t)y*800+(size_t)x]&0xffffffu;
        dark+=(p&0xff)<0x80;
        grey+=p==0xf0f0f0u;
    }
    check(dark>20 && grey==0,"stand-in: text under a full-viewport canvas is drawn, and no panel covers it");
    check((px[(size_t)400*800+400]&0xffffffu)==0xffffffu,"stand-in: a canvas's middle is the page's own paper");
    check((px[(size_t)300*800+0]&0xffffffu)!=0xffffffu,"stand-in: a canvas is outlined at its edge");
    const flow_box_t *box=flow_box_for(g.page.tree,probe_id("c"));
    char text[128]="";
    if(box!=NULL) stand_in_text(box,text,sizeof(text));
    check(os64_streq(text,"Canvas: not supported"),"stand-in: a canvas's label says it is not supported");
    os64_free(px); probe_drop();

    probe_page("<!doctype html><iframe id=f src=/poll></iframe>",true);
    box=flow_box_for(g.page.tree,probe_id("f")); text[0]='\0';
    if(box!=NULL) stand_in_text(box,text,sizeof(text));
    check(os64_streq(text,"Frame: https://fixture.test/poll"),"stand-in: a frame's label names its document");
    px=stand_in_shot();
    check((px[(size_t)100*800+150]&0xffffffu)==0xf0f0f0u,"stand-in: a frame keeps its panel");
    os64_free(px); probe_drop();

    /* A canvas wholly out of view, and one whose top strip is above the
     * view, lay no label out. */
    probe_page("<!doctype html><style>body{margin:0}#off{position:absolute;top:3000px}"
        "#up{position:absolute;top:-200px;height:400px}</style><canvas id=off></canvas><canvas id=up></canvas>",true);
    uint64_t before=s_stand_in_labels;
    px=stand_in_shot();
    check(s_stand_in_labels==before,"stand-in: a canvas out of view, or with its top out of view, makes no label");
    os64_free(px); probe_drop();

    /* Two thousand tiny canvases in view lay their label out once a paint. */
    static char many[2000*40+128];
    size_t at=(size_t)snprintf(many,sizeof(many),"<!doctype html><style>body{margin:0}</style>");
    for(int i=0;i<2000;i++) at+=(size_t)snprintf(many+at,sizeof(many)-at,"<canvas width=2 height=2></canvas>");
    probe_page(many,true);
    before=s_stand_in_labels;
    px=stand_in_shot();
    check(s_stand_in_labels-before==1,"stand-in: two thousand canvases in view cost one label a paint");
    os64_free(px); probe_drop();
}

static void inserted_frame_cases(void)
{
    for(int modern=0;modern<2;modern++) {
        inserted_frame_case("parsed","<iframe id=f src=/poll></iframe>",modern,true);
        inserted_frame_case("parsed, a block","<iframe id=f src=/poll style=display:block></iframe>",modern,true);
        inserted_frame_case("src set after insertion",
            "<script>var f=document.createElement('iframe');f.id='f';document.body.appendChild(f);f.src='/poll';</script>",
            modern,modern);
        inserted_frame_case("src set before insertion",
            "<script>var f=document.createElement('iframe');f.id='f';f.src='/poll';document.body.appendChild(f);</script>",
            modern,modern);
        inserted_frame_case("setAttribute",
            "<script>var f=document.createElement('iframe');f.id='f';f.setAttribute('src','/poll');"
            "document.body.appendChild(f);</script>",modern,true);
        inserted_frame_case("innerHTML",
            "<div id=d></div><script>document.getElementById('d').innerHTML='<iframe id=f src=/poll></iframe>';</script>",
            modern,true);
    }
}

static void classic_browser_cases(void)
{
    probe_page("<!doctype html><style>body{margin:0}img{display:block;width:200px;height:100px;"
        "padding:10px;border:5px solid}#clip{position:absolute;left:300px;top:0;"
        "width:1000px;height:200px;clip:rect(10px,100px,80px,20px)}"
        "#child{width:900px;height:100px}</style><img id=picture usemap='#pixels'>"
        "<map name=pixels><area id=first coords='0,0,40,40' href='/first' title='first' "
        "onmouseover=\"this.setAttribute('data-title',this.title);this.setAttribute('data-event',event.type)\">"
        "<area id=circle shape=circle coords='80,20,10' href='/circle'>"
        "<area id=triangle shape=poly coords='100,0,140,0,120,40' href='/triangle'>"
        "<area id=fallback shape=default href='/rest'></map><div id=clip><div id=child></div></div>",true);
    const os64_html_node_t *link=NULL;
    check(element_at(20,20,&link)==probe_id("first") && link==probe_id("first"),
        "image-map: content origin excludes border and padding");
    check(link_at(20,20)==os64_page_link_for(page_model(&g.page),probe_id("first")),
        "image-map: area link goes through native libpage activation");
    check(element_at(95,35,NULL)==probe_id("circle"),"image-map: circle hit");
    check(element_at(135,25,NULL)==probe_id("triangle"),"image-map: polygon hit");
    check(element_at(190,80,NULL)==probe_id("fallback"),"image-map: default hit");
    check(element_at(5,5,NULL)==probe_id("picture"),"image-map: border has no region");
    os64_dom_event_t mouse={.type="mouseover",.bubbles=true,.kind=OS64_DOM_EVENT_MOUSE,
        .client_x=20,.client_y=20,.page_coordinates=true,.page_x=20,.page_y=420};
    os64_js_outcome_t outcome;
    check(yonder_scripts_dispatch(g.page.scripts,probe_id("first"),&mouse,NULL,&outcome)==OS64_JS_OK,
        "image-map: inline area handler dispatches");
    const os64_html_attr_t *mark=os64_html_attr(probe_id("first"),"data-event");
    check(mark!=NULL && os64_streq(mark->value,"mouseover"),"legacy window.event names active event");
    mark=os64_html_attr(probe_id("first"),"data-title");
    check(mark!=NULL && os64_streq(mark->value,"first"),"area title reflects tooltip source");
    const flow_box_t *box=flow_box_for(g.page.tree,probe_id("clip"));
    check(box!=NULL && box->clipped,"legacy clip attaches to positioned box");
    check(element_at(325,20,NULL)==probe_id("child"),"legacy clip: inside descendant hits");
    check(element_at(305,20,NULL)!=probe_id("child"),"legacy clip: outside descendant excluded");
    check(flow_width(g.page.tree)<=800,"legacy clip: clipped far edge does not widen page");
    check(script_rebuild(),"classic widgets: handler edits published before zoom");
    g.zoom=2000;
    check(lay_out_page(&g.page,800,600),"classic widgets: zoomed layout builds");
    check(element_at(40,40,NULL)==probe_id("first") && element_at(190,70,NULL)==probe_id("circle"),
        "image-map: zoom unscales content coordinates");
    box=flow_box_for(g.page.tree,probe_id("clip"));
    os64_gui_rect_t clipped=flow_box_doc_clip(box,scroll_now());
    check(clipped.x==640 && clipped.y==20 && clipped.w==160 && clipped.h==140,
        "legacy clip: CSS sides scale with browser zoom");
    probe_drop();
    probe_page("<!DOCTYPE HTML PUBLIC '-//W3C//DTD HTML 4.01 Transitional//EN'>"
        "<div id=pane></div><script>var p=document.getElementById('pane');"
        "p.style.position='absolute';p.style.left=50;p.style.top=20;p.style.width=100;p.style.height=80;"
        "p.style.clip='rect(10px,90px,70px,5px)';"
        "p.setAttribute('data-width',p.offsetWidth);"
        "document.onmousemove=function(e){p.setAttribute('data-coordinates',e.pageY+':'+event.y+':'+document.body.scrollTop)};"
        "</script>",true);
    check(script_turn(),"classic controls: quirks unitless style task runs");
    mark=os64_html_attr(probe_id("pane"),"data-width");
    check(mark!=NULL && os64_streq(mark->value,"100"),"classic controls: unitless quirks width is laid out");
    g.sy=400;
    mouse=(os64_dom_event_t){.type="mousemove",.bubbles=true,.kind=OS64_DOM_EVENT_MOUSE,
        .client_y=20,.page_coordinates=true,.page_y=420};
    check(yonder_scripts_dispatch(g.page.scripts,page_doc(&g.page)->body,&mouse,NULL,&outcome)==OS64_JS_OK,
        "classic controls: mouse tracking task runs");
    mark=os64_html_attr(probe_id("pane"),"data-coordinates");
    check(mark!=NULL && os64_streq(mark->value,"420:20:400"),"classic controls: page, client and body scroll coordinates agree");
    probe_drop();
}
/* What reads the host's own files directly is the suite's alone: a unit
 * that includes this file for its fakes (YONDER_HOST_FAKES_ONLY) gets none
 * of it, so the replay runner's every file goes through its own door. */
#ifndef YONDER_HOST_FAKES_ONLY
/* Optional consumer proof reads locally downloaded primary scripts. Site
 * sources stay outside the repository; regular regressions are original. */
static char *classic_source(const char *directory, const char *name)
{
    char path[4096];snprintf(path,sizeof(path),"%s/%s",directory,name);
    FILE *file=fopen(path,"rb");
    if(file==NULL) { check(false,"consumer source opens");return NULL; }
    fseek(file,0,SEEK_END);long length=ftell(file);rewind(file);
    if(length<0 || length>2*1024*1024) { fclose(file);check(false,"consumer source size");return NULL; }
    char *source=malloc((size_t)length+1);
    if(source==NULL) { fclose(file);return NULL; }
    size_t read=fread(source,1,(size_t)length,file);fclose(file);source[read]='\0';
    check(read==(size_t)length,"consumer source reads");
    return source;
}
static void classic_downloaded_cases(void)
{
    const char *directory=getenv("D11_CLASSIC_SOURCES");
    if(directory==NULL) return;
    char *source=classic_source(directory,"lileks.html");
    if(source==NULL) return;
    probe_page(source,true);free(source);
    check(script_turn(),"Lileks: original inline script runs");
    os64_js_outcome_t out;
    os64_dom_event_t load={.type="load"};
    check(yonder_scripts_dispatch(g.page.scripts,NULL,&load,NULL,&out)==OS64_JS_OK,
        "Lileks: original preload handler constructs images");
    os64_html_node_t *image=NULL;
    for(os64_html_node_t *n=page_doc(&g.page)->document;n;n=(os64_html_node_t *)next_within(n,page_doc(&g.page)->document)) {
        const os64_html_attr_t *name=os64_html_attr(n,"name");
        if(name && os64_streq(name->value,"Image8")) { image=n;break; }
    }
    check(image!=NULL,"Lileks: original named rollover image exists");
    if(image!=NULL) {
        os64_dom_event_t over={.type="mouseover",.bubbles=true,.kind=OS64_DOM_EVENT_MOUSE};
        check(yonder_scripts_dispatch(g.page.scripts,image,&over,NULL,&out)==OS64_JS_OK,
            "Lileks: original mouseover handler runs");
        const os64_html_attr_t *src=os64_html_attr(image,"src");
        check(src!=NULL && strstr(src->value,"dtownb.jpg")!=NULL,"Lileks: original handler swaps src");
        over.type="mouseout";
        check(yonder_scripts_dispatch(g.page.scripts,image,&over,NULL,&out)==OS64_JS_OK,
            "Lileks: original restore handler runs");
        src=os64_html_attr(image,"src");
        check(src!=NULL && strstr(src->value,"dtownb.jpg")==NULL,"Lileks: original handler restores src");
    }
    probe_drop();
    source=classic_source(directory,"museum-date.js");
    if(source==NULL) return;
    size_t size=strlen(source)+256;char *html=malloc(size);
    snprintf(html,size,"<div id=date><script>%s</script></div>",source);free(source);
    loop_page(html);free(html);
    check(loop_settle() && page_doc(&g.page)!=NULL,"Museum: original date script arrives through document.write");
    if(page_doc(&g.page)!=NULL) {
        const flow_box_t *date=flow_box_for(g.page.tree,probe_id("date"));
        check(date!=NULL && date->rect.h>0,"Museum: original date has visible output");
    }
    loop_drop();
    source=classic_source(directory,"museum-countdown.js");
    if(source==NULL) return;
    size=strlen(source)+512;html=malloc(size);
    snprintf(html,size,"<form name=cform><input id=display name=disp></form><script>%s"
        "then=new Date(Date.now()+86400000);runMany();</script>",source);free(source);
    probe_page(html,true);free(html);check(script_turn(),"Museum: original countdown function updates named field");
    const char *value=NULL;size_t value_length=0;
    check(os64_page_node_value(os64_page_shared_state(page_model(&g.page)),probe_id("display"),&value,&value_length)==OS64_HTML_OK,
        "Museum: countdown live input value reads");
    check(value!=NULL && strstr(value,"To Bond 22 Premiere:")!=NULL,"Museum: countdown future branch reaches live input state");
    probe_drop();
    source=classic_source(directory,"million-gsc3.js");
    if(source==NULL) return;
    size=strlen(source)+1024;html=malloc(size);
    snprintf(html,size,"<!DOCTYPE HTML PUBLIC '-//W3C//DTD HTML 4.01 Transitional//EN'>"
        "<style>#een{position:absolute;width:2000px;height:2000px}</style>"
        "<div id=f></div><div id=een></div><div id=neg></div><div id=d>"
        "<span id=xcoord></span><span id=ycoord></span></div>"
        "<img id=sn><img id=sz><img id=so><button id=zoom onclick='tz()'>zoom</button>"
        "<script>%s</script>",source);free(source);
    probe_page(html,true);free(html);check(script_turn(),"Million: original gsc3 script initializes tracking");
    os64_dom_event_t click={.type="click",.bubbles=true};
    check(yonder_scripts_dispatch(g.page.scripts,probe_id("zoom"),&click,NULL,&out)==OS64_JS_OK,
        "Million: original zoom control runs");
    os64_dom_event_t move={.type="mousemove",.bubbles=true,.kind=OS64_DOM_EVENT_MOUSE,
        .client_x=100,.client_y=100,.page_coordinates=true,.page_x=100,.page_y=100};
    check(yonder_scripts_dispatch(g.page.scripts,page_doc(&g.page)->body,&move,NULL,&out)==OS64_JS_OK,
        "Million: original mouse tracking and zoom task runs");
    const os64_html_attr_t *style=os64_html_attr(probe_id("een"),"style");
    check(style!=NULL && strstr(style->value,"clip:rect(")!=NULL,"Million: original zoom sets a clip rectangle");
    check(script_rebuild(),"Million: original zoom edits render through the browser");
    probe_drop();
}
#endif

typedef struct {
    const os64_gui_event_t *events;
    size_t count, at;
} HoverQueue;
static int64_t hover_poll(void *opaque,os64_gui_event_t *event)
{
    HoverQueue *queue=opaque;
    if(queue->at==queue->count) return 0;
    *event=queue->events[queue->at++];return 1;
}
/* P5 regression: the description must become drawable while motion remains
 * queued. Check that sampling reduction preserves input boundaries, rather
 * than imposing a host-speed deadline which cannot describe the P5. */
static void hover_batch_cases(void)
{
    os64_gui_event_t samples[1000];
    for(unsigned i=0;i<1000;i++) samples[i]=(os64_gui_event_t){
        .type=OS64_GUI_EVENT_MOUSE_MOVE,.mouse={.x=20+(int32_t)(i%100),.y=20}};
    probe_page("<!DOCTYPE HTML PUBLIC '-//W3C//DTD HTML 4.01 Transitional//EN'>"
        "<style>body{margin:0}img{display:block;width:200px;height:100px}"
        "#popup{position:absolute;top:150px}</style><img usemap='#regions'><map name=regions>"
        "<area coords='0,0,200,100' title='site' onmouseover=\"document.getElementById('popup').title=this.title\">"
        "</map><div id=popup></div><script>var moves=0;document.onmousemove=function(e){"
        "var popup=document.getElementById('popup');var width=document.body.offsetWidth;"
        "popup.style.left=e.pageX+10;popup.textContent=(++moves)+':'+e.clientX;};</script>",true);
    check(script_turn(),"hover burst: page's tracker installs");
    HoverQueue queue={samples,1000,0};
    unsigned turns=0,dispatched=0;
    while(queue.at<queue.count) {
        YonderEventBatch batch={0};os64_gui_event_t event;
        while(yonder_event_batch_next(&batch,&event,hover_poll,&queue)) {
            dispatched++;
            hover(event.mouse.x,event.mouse.y);inputs_run();
        }
        turns++;
        check(batch.consumed<=YONDER_EVENT_BATCH_MAX,"hover burst: turn has bounded input work");
        if(turns==1) {
            check(queue.at<queue.count && probe_text_is("popup","1:51"),
                "hover burst: current popup rendered before queued motion drains");
            check(g.page.rendered_version==os64_html_version(page_doc(&g.page)),
                "hover burst: popup layout is current at first rendering boundary");
        }
    }
    check(turns==32 && dispatched==32 && probe_text_is("popup","32:119"),
        "hover burst: thousand samples collapse to current positions and final description");
    probe_drop();
    os64_gui_event_t ordered[]={
        {.type=OS64_GUI_EVENT_MOUSE_MOVE,.mouse={.x=1}},
        {.type=OS64_GUI_EVENT_MOUSE_MOVE,.mouse={.x=2}},
        {.type=OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,.mouse={.x=3,.buttons=1}},
        {.type=OS64_GUI_EVENT_MOUSE_MOVE,.mouse={.x=4,.buttons=1}},
        {.type=OS64_GUI_EVENT_MOUSE_MOVE,.mouse={.x=5,.buttons=1}},
        {.type=OS64_GUI_EVENT_MOUSE_BUTTON_UP,.mouse={.x=6}},
        {.type=OS64_GUI_EVENT_MOUSE_MOVE,.mouse={.x=7}},
        {.type=OS64_GUI_EVENT_MOUSE_MOVE,.mouse={.x=8,.modifiers=OS64_GUI_MOD_SHIFT}},
        {.type=OS64_GUI_EVENT_MOUSE_WHEEL,.mouse={.dy=-2}},
        {.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii='a'}},
        {.type=OS64_GUI_EVENT_DOORBELL,.doorbell={.mask=BELL_TICK}},
        {.type=OS64_GUI_EVENT_POINTER_STATE,.pointer={.inside=false}},
        {.type=OS64_GUI_EVENT_MOUSE_MOVE,.mouse={.x=9}},
    };
    queue=(HoverQueue){ordered,sizeof(ordered)/sizeof(*ordered),1};
    YonderEventBatch batch={.held=true,.pending=ordered[0]};os64_gui_event_t event;
    unsigned index=1;
    while(yonder_event_batch_next(&batch,&event,hover_poll,&queue)) {
        const os64_gui_event_t *expected=index<queue.count ? &ordered[index] : NULL;
        bool same=expected!=NULL && event.type==expected->type;
        if(same) switch(event.type) {
            case OS64_GUI_EVENT_MOUSE_WHEEL: same=event.mouse.dx==expected->mouse.dx && event.mouse.dy==expected->mouse.dy;break;
            case OS64_GUI_EVENT_KEY_DOWN: same=event.key.ascii==expected->key.ascii;break;
            case OS64_GUI_EVENT_DOORBELL: same=event.doorbell.mask==expected->doorbell.mask;break;
            case OS64_GUI_EVENT_POINTER_STATE: same=event.pointer.inside==expected->pointer.inside;break;
            default: same=event.mouse.x==expected->mouse.x && event.mouse.buttons==expected->mouse.buttons &&
                event.mouse.modifiers==expected->mouse.modifiers;break;
        }
        check(same,
            "hover burst: click, drag, modifier, key, timer and pointer state order preserved");
        index++;
    }
    check(index==queue.count,"hover burst: lookahead delivers every nonmerged event");
    /* Read by a dispatcher (yonder_event_batch_draining): the cap answers
     * "none" with events still queued, except while the dispatcher waits
     * for every queue to drain after a destroy, when only an empty queue
     * says so. Forty keys, none of which merge. */
    os64_gui_event_t keys[40];
    for(unsigned i=0;i<40;i++) keys[i]=(os64_gui_event_t){.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii=(uint8_t)('a'+i%26)}};
    for(int quarantine=0;quarantine<2;quarantine++) {
        queue=(HoverQueue){keys,40,0};
        batch=(YonderEventBatch){0};
        unsigned read=0;
        while(yonder_event_batch_draining(&batch,&event,hover_poll,&queue,quarantine!=0)) read++;
        check(quarantine ? read==40 && batch.drained : read==YONDER_EVENT_BATCH_MAX && !batch.drained,
              quarantine ? "dispatcher batch: during a quarantine only an empty queue answers none"
                         : "dispatcher batch: otherwise the cap ends the reading with the queue not drained");
    }
    /* A batch that found the queue empty, then a destroy that queues an
     * event and begins a quarantine: the old empty answer does not stand,
     * and the event is read in the same turn. Outside a quarantine it waits
     * for the next turn's batch. */
    for(int quarantine=0;quarantine<2;quarantine++) {
        queue=(HoverQueue){keys,0,0};
        batch=(YonderEventBatch){0};
        check(!yonder_event_batch_draining(&batch,&event,hover_poll,&queue,false) && batch.drained,
              "dispatcher batch: an empty queue is found empty");
        queue.count=1;
        bool read=yonder_event_batch_draining(&batch,&event,hover_poll,&queue,quarantine!=0);
        check(quarantine ? read && queue.at==1 : !read && queue.at==0,
              quarantine ? "dispatcher batch: a quarantine asks an emptied queue again"
                         : "dispatcher batch: otherwise an emptied batch waits for the next turn");
    }
}

#include <time.h>
static double hover_seconds(void)
{
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec+ts.tv_nsec/1e9;
}
#ifndef YONDER_HOST_FAKES_ONLY
static void classic_hover_profile(void)
{
    const char *directory=getenv("D11_HOVER_PROFILE");if(directory==NULL) return;
    char *source=classic_source(directory,"million-primary.html");if(source==NULL) return;
    double began=hover_seconds();probe_page(source,true);free(source);
    fprintf(stderr,"HOVER profile load %.3f s links=%d\n",hover_seconds()-began,os64_page_nlinks(page_model(&g.page)));
    began=hover_seconds();script_turn();fprintf(stderr,"HOVER script %.3f s\n",hover_seconds()-began);
    const os64_html_node_t *area=NULL;
    for(const os64_html_node_t *n=page_doc(&g.page)->document;n;n=next_within(n,page_doc(&g.page)->document))
        if(n->tag==OS64_HTML_TAG_AREA) {area=n;break;}
    os64_dom_event_t ev={.type="mouseover",.bubbles=true,.kind=OS64_DOM_EVENT_MOUSE};os64_js_outcome_t out;
    began=hover_seconds();yonder_scripts_dispatch(g.page.scripts,area,&ev,NULL,&out);
    fprintf(stderr,"HOVER over %.3f s outcome=%d %s\n",hover_seconds()-began,out.status,out.message);
    for(int i=0;i<3;i++) {
        ev.type="mousemove";ev.client_x=100+i;ev.client_y=100;ev.page_coordinates=true;ev.page_x=100+i;ev.page_y=100;
        began=hover_seconds();yonder_scripts_dispatch(g.page.scripts,area,&ev,NULL,&out);
        fprintf(stderr,"HOVER move %.3f s outcome=%d %s\n",hover_seconds()-began,out.status,out.message);
        began=hover_seconds();check(page_model_refresh(&g.page),"profile model refresh");
        fprintf(stderr,"HOVER model %.3f s\n",hover_seconds()-began);
        began=hover_seconds();check(script_rebuild(),"profile rendering rebuild");
        fprintf(stderr,"HOVER render %.3f s\n",hover_seconds()-began);
    }
    probe_drop();
}
#endif

/* ── THE PAGE FILES (YONDER_DIAGNOSTICS.md) ───────────────────────────── */
/* The text of the file `name` in `dir`, or NULL. YONDER_DIAG_DUMP=1 prints
 * each one a case reads, which is how a failing expectation is read. */
static const char *diag_file(const char *dir, const char *name) {
    char path[256]; snprintf(path,sizeof(path),"%s/%s",dir,name);
    if(getenv("YONDER_DIAG_DUMP")) fprintf(stderr,"--- %s\n%s---\n",path,fake_text(path)?fake_text(path):"(none)\n");
    return fake_text(path);
}
static bool has(const char *text, const char *needle) { return text!=NULL && strstr(text,needle)!=NULL; }
/* A page's record as a load makes one (diag_begin): its name claimed in
 * the directory yonder.conf names, when it names one, since a record with
 * no claim is never written. */
static yonder_diag_t *diag_new_claimed(const char *url, uint32_t seq) {
    yonder_diag_t *d=yonder_diag_new(url,seq,0);
    if(d!=NULL && s_diag.dir[0]!='\0') yonder_diag_claim(d,s_diag.dir,false);
    return d;
}
/* The keyword rule: MISSING and FAILED begin record lines, sit on the
 * verdict line, and appear nowhere else, in every file any case wrote. */
static void diag_tokens_exclusive(void) {
    unsigned files=0, bad=0;
    for(int f=0;f<FAKE_FILES;f++) {
        if(!fake_files[f].used || strstr(fake_files[f].path,"/diag")==NULL) continue;
        files++;
        const char *line=fake_files[f].data; int n=0;
        while(*line) {
            const char *end=strchr(line,'\n'); size_t len=end?(size_t)(end-line):strlen(line);
            char one[2048]; snprintf(one,sizeof(one),"%.*s",(int)(len<sizeof(one)-1?len:sizeof(one)-1),line);
            n++;
            /* A token appears once in a record line, at its head; on the
             * verdict line only when that token has records. */
            bool verdict=n==2 && !strncmp(one,"verdict: ",9);
            const char *tokens[]={"MISSING","FAILED"};
            for(int k=0;k<2;k++) {
                const char *at=strstr(one,tokens[k]);
                if(at==NULL) continue;
                bool head=at==one && strstr(at+1,tokens[k])==NULL;
                /* The verdict names it after its count, which is not zero. */
                const char *digits=at-1;
                unsigned long count=0, scale=1;
                while(digits>one+9 && digits[-1]>='0' && digits[-1]<='9') { count+=(unsigned long)(digits[-1]-'0')*scale; scale*=10; digits--; }
                bool named=verdict && strstr(at+1,tokens[k])==NULL && at[-1]==' ' && digits<at-1 && count!=0;
                if(!head && !named) { bad++; fprintf(stderr,"token outside its place in %s: %s\n",fake_files[f].path,one); }
            }
            if(n==2 && !verdict) { bad++; fprintf(stderr,"line two is not the verdict in %s\n",fake_files[f].path); }
            line+=len+(end?1:0);
        }
    }
    check(files>0 && bad==0,"diag: the tokens begin record lines and appear nowhere else, in every file written");
}
/* A picture yonder does not decode is recorded by its format. */
static void diag_picture_case(void) {
    static const uint8_t webp[]={'R','I','F','F',0,0,0,0,'W','E','B','P'};
    static const uint8_t avif[]={0,0,0,0x1c,'f','t','y','p','a','v','i','f'};
    static const uint8_t tiff[]={'M','M',0,'*'}, ico[]={0,0,1,0};
    const char *svg="<?xml version=\"1.0\"?>\n<!-- a logo -->\n<svg xmlns=\"http://www.w3.org/2000/svg\">";
    check(os64_streq(yonder_diag_image_format(webp,sizeof(webp)),"webp") &&
        os64_streq(yonder_diag_image_format(avif,sizeof(avif)),"avif") &&
        os64_streq(yonder_diag_image_format(tiff,sizeof(tiff)),"tiff") &&
        os64_streq(yonder_diag_image_format(ico,sizeof(ico)),"ico") &&
        os64_streq(yonder_diag_image_format((const uint8_t *)svg,strlen(svg)),"svg") &&
        os64_streq(yonder_diag_image_format((const uint8_t *)"MM",2),"unknown"),
        "diag: an undecoded picture is named by its first bytes, an SVG past its prolog");
    probe_page("<img src=file:///logo.svg><img src=file:///broken.png><img src=http://x.test/gone.png>"
        "<img src=http://x.test/far.png><img src=file:///absent.png>",true);
    pictures_start(&g.page);
    g.page.diag=diag_new_claimed("http://x.test/",1);
    for(int i=0;i<5;i++) g.page.pics[i].state=PIC_WAITING;
    g.page.waiting=g.page.in_flight=5;
    yonder_picture_job_t job={.kind=YONDER_JOB_PICTURE,.index=0,.generation=g.page_serial};
    yonder_picture_t gone={.status=OS64_IMAGE_UNKNOWN_FORMAT,.format="svg"};
    picture_arrived(&job,&gone);
    job.index=1;
    yonder_picture_t broken={.status=OS64_IMAGE_MALFORMED,.why="would not decode (malformed image)"};
    picture_arrived(&job,&broken);
    job.index=2;
    yonder_picture_t missing={.status=OS64_IMAGE_NO_FILE,.why="HTTP 404"};
    picture_arrived(&job,&missing);
    job.index=3;
    yonder_picture_t unreached={.status=OS64_IMAGE_IO_ERROR,.why="did not fetch (tls-failed)"};
    picture_arrived(&job,&unreached);
    /* The last one in rewrites the file, so it names every picture that
     * failed while the page is still up; it comes through the doorbell. */
    char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
    check(!has(diag_file("/tmp/diag",name),"FAILED picture"),"diag: the file is not written while pictures are out");
    yonder_picture_job_t *last=os64_calloc(1,sizeof(*last));
    yonder_picture_t *unread=os64_calloc(1,sizeof(*unread));
    *last=job; last->index=4;
    unread->status=OS64_IMAGE_IO_ERROR;
    os64_strcopy(unread->why,sizeof(unread->why),"could not be read");
    reaped(0,last,unread);
    char text[4096]; yonder_diag_render(g.page.diag,text,sizeof(text));
    const char *file=diag_file("/tmp/diag",name);
    yonder_picture_t animated={.status=OS64_IMAGE_UNSUPPORTED,.format="webp",.why="animated"};
    picture_failed(&g.page,"file:///animation.webp",&animated,NULL);
    char webp_record[4096]; yonder_diag_render(g.page.diag,webp_record,sizeof(webp_record));
    check(has(webp_record,"MISSING image webp (1)") &&
          has(webp_record,"picture file:///animation.webp: webp, animated"),
          "diag: unsupported WebP keeps both the format census and animation reason");
    check(g.page.waiting==0 && has(file,"\nwritten: when its pictures and fonts were in\n") &&
        has(file,"\npictures: 5, 0 shown, 5 could not be read, 0 past the memory kept, 0 still coming, 0 not asked for\n") &&
        has(file,"FAILED picture-fetch: file:///absent.png"),
        "diag: the last picture in rewrites the file, every failure named");
    /* A picture of a page already gone takes nothing to zero: no write. */
    int writes=fake_writes;
    yonder_picture_job_t *stale=os64_calloc(1,sizeof(*stale));
    *stale=job; stale->generation=g.page_serial-1;
    reaped(0,stale,NULL);
    check(fake_writes==writes,"diag: a picture that settles nothing writes nothing");
    bool all_failed=true;
    for(int i=0;i<5;i++) all_failed=all_failed && g.page.pics[i].state==PIC_FAILED;
    check(has(text,"\nverdict: 1 MISSING, 4 FAILED\nMISSING image svg (1)\n") && all_failed,
        "diag: a picture in a format yonder lacks is MISSING image; one whose fetch failed otherwise is FAILED picture-fetch");
    check(has(text,"\npicture file:///logo.svg: svg, which yonder does not decode\n") &&
        has(text,"\nFAILED picture-fetch: file:///broken.png: would not decode (malformed image) (1)\n") &&
        has(text,"\nFAILED picture-fetch: http://x.test/gone.png: HTTP 404 (1)\n") &&
        has(text,"\nFAILED picture-fetch: http://x.test/far.png: did not fetch (tls-failed) (1)\n") &&
        has(text,"\nFAILED picture-fetch: file:///absent.png: could not be read (1)\n"),
        "diag: every picture not shown is named by its address, with the worker's why");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
}

/* COOKIES LEFT OUT FOR WANT OF ROOM (way/jar.h): one FAILED line a host,
 * its count the most one request left out, from the page's own fetch (the
 * trip's verdict) and from a fetch the page made (a picture's hooks). */
static void diag_cookies_case(void) {
    yonder_diag_t *d=yonder_diag_new("http://x.test/",1,0);
    way_cookies_short_t s={3,"a.test"};
    diag_cookies_short(d,&s);
    s.left=2; diag_cookies_short(d,&s);
    way_cookies_short_t none={0,""};
    diag_cookies_short(d,&none);
    char text[4096]; yonder_diag_render(d,text,sizeof(text));
    check(has(text,"\nverdict: 1 FAILED\nFAILED cookies: left out of a request to a.test (3)\n"),
        "cookies: one line a host, the most one request left out; a load that left none out says nothing");
    s.left=7; diag_cookies_short(d,&s);
    yonder_diag_render(d,text,sizeof(text));
    check(has(text,"FAILED cookies: left out of a request to a.test (7)\n"),"cookies: a worse request raises the count");
    yonder_diag_free(d);

    /* The page's own fetch: the verdict carries the leg's note. */
    stream_window();
    start_trip("http://fixture.test/p",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    way_head_t h=stream_head("text/html","",false);
    yonder_mail_post_head(g.stream.mail,&h);
    const char *body="<!doctype html><p>in";
    stream_post_body((const uint8_t *)body,strlen(body),YONDER_STREAM_CHUNK);
    yonder_verdict_t v; memset(&v,0,sizeof(v));
    v.page=true; v.fetch=OS64_FETCH_OK;
    v.cookies_short=(way_cookies_short_t){4,"fixture.test"};
    yonder_mail_post_verdict(g.stream.mail,&v);
    for(int i=0;i<8 && stream_turn();i++) {}
    check(!g.stream.active && g.page.diag!=NULL,"cookies: the page arrived");
    yonder_diag_render(g.page.diag,text,sizeof(text));
    check(has(text,"FAILED cookies: left out of a request to fixture.test (4)\n"),
        "cookies: the page's own request that left cookies out is on its record");
    stream_window_drop();

    /* A picture the page asked for: its hooks' note, on the page shown. */
    probe_page("<img src=http://x.test/a.png>",true);
    pictures_start(&g.page);
    g.page.diag=diag_new_claimed("http://x.test/",2);
    g.page.pics[0].state=PIC_WAITING;
    g.page.waiting=g.page.in_flight=1;
    yonder_picture_job_t job={.kind=YONDER_JOB_PICTURE,.index=0,.generation=g.page_serial};
    job.hooks.cookies_short=(way_cookies_short_t){2,"x.test"};
    yonder_picture_t missing={.status=OS64_IMAGE_NO_FILE,.why="HTTP 404"};
    picture_arrived(&job,&missing);
    yonder_diag_render(g.page.diag,text,sizeof(text));
    check(has(text,"FAILED cookies: left out of a request to x.test (2)\n"),
        "cookies: a picture's request that left cookies out is on the page's record");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
}

/* The pictures never handed to a worker, and the one past what a page's
 * pictures may cost, are named too: every picture not shown is. */
static void diag_picture_unshown_case(void) {
    probe_page("<img src=file:///kept.png><img src=file:///asked.png>",true);
    pictures_start(&g.page);
    g.page.diag=diag_new_claimed("http://x.test/",1);
    g.page.pics[0].state=PIC_WAITING;
    g.page.waiting=g.page.in_flight=1;
    g.page.kept_bytes=PICTURES_KEPT_MAX;
    yonder_picture_job_t job={.kind=YONDER_JOB_PICTURE,.index=0,.generation=g.page_serial};
    yonder_picture_t big={.status=OS64_IMAGE_OK,.cost=1};
    picture_arrived(&job,&big);
    g.page.pics[1].state=PIC_QUEUED; g.page.next_pic=1; g.page.waiting=1;
    pictures_feed(&g.page);         /* this harness has no worker pool */
    char text[2048]; yonder_diag_render(g.page.diag,text,sizeof(text));
    check(g.page.pics[0].state==PIC_NOT_KEPT &&
          has(text,"\npicture file:///kept.png: past the memory kept for the page's pictures\n") &&
          !has(text,"FAILED picture: file:///kept.png"),
        "diag: a picture past the memory kept is named, and is no failure");
    check(g.page.pics[1].state==PIC_FAILED && has(text,"\nFAILED picture: file:///asked.png: no worker to fetch it (1)\n"),
        "diag: a picture no worker was asked for is FAILED picture, by its address");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
}

/* With no workers, every picture is named as it is gathered, and that
 * settles the page's pictures: the record is written then, badge with it.
 * Workers that stop with pictures out leave each of those named. */
static void diag_picture_workerless_case(void) {
    probe_page("<img src=file:///one.png><img src=file:///two.png>",true);
    g.page.diag=diag_new_claimed("http://w.test/",1);
    g.badge_missing=g.badge_failed=UINT32_MAX;
    pictures_start(&g.page);        /* this harness has no worker pool */
    char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
    const char *file=diag_file("/tmp/diag",name);
    check(g.page.pics[0].state==PIC_FAILED && g.page.pics[1].state==PIC_FAILED && g.page.waiting==0 &&
          has(file,"\nwritten: when its pictures and fonts were in\n") &&
          has(file,"FAILED picture: file:///one.png: no worker to fetch it (1)") &&
          has(file,"FAILED picture: file:///two.png: no worker to fetch it (1)") &&
          !strcmp(g.badge_text,"FAILED 2"),
        "diag: with no workers each picture is named, the record written and the badge told");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
    probe_page("<img src=file:///out.png>",true);
    pictures_start(&g.page);        /* the table, before the record exists */
    g.page.diag=diag_new_claimed("http://w.test/",2);
    g.page.pics[0].state=PIC_WAITING; g.page.waiting=g.page.in_flight=1;
    /* What the record learned silently since arrival reaches the badge when
     * the last picture is in, even one that arrives whole. */
    g.badge_missing=g.badge_failed=0; g.badge_text[0]=0;
    yonder_diag_missing(g.page.diag,"element","canvas",1);
    yonder_picture_job_t *last=os64_calloc(1,sizeof(*last));
    yonder_picture_t *whole=os64_calloc(1,sizeof(*whole));
    last->kind=YONDER_JOB_PICTURE; last->index=0; last->generation=g.page_serial;
    whole->status=OS64_IMAGE_OK;
    g.page.pics[0].moves=false;     /* sized: its arrival moves nothing */
    reaped(0,last,whole);
    check(g.page.pics[0].state==PIC_SHOWN && !strcmp(g.badge_text,"MISSING 1"),
        "diag: the rewrite when the last picture is in tells the badge");
    g.page.pics[0].state=PIC_WAITING; g.page.waiting=g.page.in_flight=1;
    pictures_orphaned(&g.page);
    char text[1024]; yonder_diag_render(g.page.diag,text,sizeof(text));
    check(g.page.pics[0].state==PIC_FAILED && g.page.waiting==0 &&
          has(text,"FAILED picture: file:///out.png: the workers stopped (1)"),
        "diag: a picture out when the workers stopped is named");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
    /* One with the workers and one still queued behind them: both named. */
    probe_page("<img src=file:///with.png><img src=file:///behind.png>",true);
    pictures_start(&g.page);
    g.page.diag=diag_new_claimed("http://w.test/",3);
    g.page.pics[0].state=PIC_WAITING; g.page.pics[1].state=PIC_QUEUED;
    g.page.next_pic=1; g.page.waiting=2; g.page.in_flight=1;
    pictures_orphaned(&g.page);
    yonder_diag_render(g.page.diag,text,sizeof(text));
    check(g.page.pics[0].state==PIC_FAILED && g.page.pics[1].state==PIC_FAILED && g.page.waiting==0 &&
          has(text,"FAILED picture: file:///with.png: the workers stopped (1)") &&
          has(text,"FAILED picture: file:///behind.png: no worker to fetch it (1)"),
        "diag: a picture still queued when the workers stopped is named too");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
}

/* Pictures the page names that never reach the table are on the record;
 * those still coming when the page is left are counted; a layout that
 * stopped partway is one standing failure however often it is written. */
static void diag_picture_unasked_case(void) {
    probe_page("<img src='data:image/png;base64,iVBORw0KGgo='><img src='http://[bad'>"
        "<img src=file:///slow.png><img src='x:asset'>",true);
    g.page.diag=diag_new_claimed("http://u.test/",1);
    pictures_start(&g.page);        /* no workers: slow.png is refused too */
    char text[4096]; yonder_diag_render(g.page.diag,text,sizeof(text));
    check(has(text,"MISSING picture-scheme data (1)") && has(text,"FAILED picture: http://[bad: the address was refused (") &&
          has(text,"MISSING picture-scheme x (1)") && has(text,"not asked for") && g.page.unasked==3,
        "diag: a picture refused before the table is on the record, by its scheme (one letter is one) or its refusal");
    /* Met with nothing out, the record is written then; met again (a
     * script's rebuild), it is the same picture and records nothing more. */
    char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
    check(has(diag_file("/tmp/diag",name),"MISSING picture-scheme data (1)"),
        "diag: a refusal met with no picture out writes the record");
    pictures_start(&g.page);
    yonder_diag_render(g.page.diag,text,sizeof(text));
    check(g.page.unasked==3 && has(text,"MISSING picture-scheme data (1)") &&
          has(text,"FAILED picture: http://[bad: the address was refused (the address it names is not one that resolves) (1)"),
        "diag: a refused picture met again is the same picture");
    /* Left with one still out: counted, not failed. */
    g.page.pics[0].state=PIC_WAITING;
    diag_observe(&g.page,"when the page was left");
    yonder_diag_render(g.page.diag,text,sizeof(text));
    check(has(text,"0 past the memory kept, 1 still coming, 3 not asked for\n"),
        "diag: a picture still coming when the page is left is counted so");
    yonder_diag_failed_seen(g.page.diag,"layout","stopped");
    yonder_diag_failed_seen(g.page.diag,"layout","stopped");
    yonder_diag_render(g.page.diag,text,sizeof(text));
    check(has(text,"FAILED layout: stopped (1)\n"),"diag: a standing failure is counted once however often it is seen");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
}

/* A sheet's background in a scheme nothing fetches is refused before the
 * table too, and on the record. */
/* A line that met no room is counted once, however often it is asked
 * for: a condition looked at each layout, a section written again whole. */
static void diag_lost_distinct_case(void) {
    yonder_diag_t *d=yonder_diag_new("http://fixture.test/",1,0);
    static char text[131072];
    char key[32];
    for(int i=0;i<YONDER_DIAG_FACTS_MAX;i++) { snprintf(key,sizeof(key),"fact %d",i); yonder_diag_fact(d,key,"x"); }
    for(int i=0;i<YONDER_DIAG_RECORDS_MAX;i++) { snprintf(key,sizeof(key),"n%d",i); yonder_diag_missing_seen(d,"font",key,1); }
    for(int round=0;round<3;round++) {
        yonder_diag_fact(d,"font face 1","one");
        yonder_diag_fact(d,"font face 2","two");
        yonder_diag_missing_seen(d,"font","late",1);
        yonder_diag_forget(d,"font ");
    }
    yonder_diag_fact(d,"font face 1","one");
    yonder_diag_render(d,text,sizeof(text));
    check(strstr(text,"\nfacts not kept: 1\n")!=NULL && strstr(text,"\nmissing names not kept: 1\n")!=NULL,
          "diag: a line that met no room is counted once, however often it is asked for");
    yonder_diag_forget(d,"fact 1");
    yonder_diag_fact(d,"font face 1","one");
    yonder_diag_render(d,text,sizeof(text));
    check(strstr(text,"\nfont face 1: one\n")!=NULL && strstr(text,"facts not kept")==NULL,
          "diag: a line given room after all is no longer counted as not kept");
    yonder_diag_free(d);
}
/* Past the lines the record can tell apart, it says there are more and
 * never counts one twice. */
static void diag_lost_overflow_case(void) {
    yonder_diag_t *d=yonder_diag_new("http://fixture.test/",1,0);
    static char text[262144], once[256];
    char key[32];
    for(int i=0;i<YONDER_DIAG_FACTS_MAX;i++) { snprintf(key,sizeof(key),"fact %d",i); yonder_diag_fact(d,key,"x"); }
    for(int i=0;i<5000;i++) { snprintf(key,sizeof(key),"lost %d",i); yonder_diag_fact(d,key,"x"); }
    yonder_diag_render(d,text,sizeof(text));
    const char *line=strstr(text,"\nfacts not kept: ");
    snprintf(once,sizeof(once),"%.60s",line!=NULL ? line : "");
    yonder_diag_fact(d,"lost 4999","x");
    yonder_diag_fact(d,"lost 7",  "x");
    yonder_diag_render(d,text,sizeof(text));
    check(strstr(text,"\nfacts not kept: 4096, and more not counted\n")!=NULL && strstr(text,once)!=NULL,
          "diag: past the lines it can tell apart, the record says there are more and counts none twice");
    yonder_diag_free(d);
}
/* A line is found by its key, not by walking what the record holds: a
 * page's failures fill the FAILED lines and the lost ones (512 and 4,096),
 * and 100,000 asks across them, each a new or a repeated failure, cost a
 * few probes each (MODERN_WEB.md § P2a, as built: the cold read's freeze). */
static void diag_lookup_bound_case(void) {
    yonder_diag_t *d=yonder_diag_new("http://fixture.test/",1,0);
    char key[48];
    uint64_t before=yonder_diag_steps();
    for(int i=0;i<100000;i++) {
        snprintf(key,sizeof(key),"unhandled rejection: %d",i%6000);
        yonder_diag_failed(d,"script click event",key);
    }
    uint64_t steps=yonder_diag_steps()-before;
    check(yonder_diag_failed_lines(d)==YONDER_DIAG_RECORDS_MAX && steps<100000u*8,
          "diag: 100,000 failures over 6,000 lines are found in under eight probes each");
    if(steps>=100000u*8) fprintf(stderr,"diag lookup steps: %llu\n",(unsigned long long)steps);
    yonder_diag_free(d);
}
/* A page let go before it was shown takes the families staged for it. */
static void diag_arriving_released_case(void) {
    static Page p;
    memset(&p,0,sizeof(p));
    p.diag=diag_new_claimed("http://fixture.test/",77);
    s_arriving.staged=true; s_arriving.seq=yonder_diag_seq(p.diag);
    webfonts_served(&s_arriving.families,"Staged",6,WF_SERVED_NONE,"Test Sans");
    diag_leave(&p);
    check(!s_arriving.staged && s_arriving.families.n==0 && s_arriving.families.f==NULL,
          "diag: a page let go before it was shown takes its staged families with it");
}
/* A fact whose value had no memory to keep says so: not empty, and not
 * the value before it standing as if it were this one. */
static void diag_fact_short_case(void) {
    yonder_diag_t *d=yonder_diag_new("http://fixture.test/",1,0);
    static char text[8192];
    yonder_diag_fact(d,"kept","first");
    fail_at=attempts+1;
    yonder_diag_fact(d,"kept","second");
    fail_at=0;
    yonder_diag_render(d,text,sizeof(text));
    check(strstr(text,"\nkept: no memory to write this line\n")!=NULL,"diag: a value with no memory to keep is said to be gone");
    yonder_diag_fact(d,"kept","third");
    yonder_diag_render(d,text,sizeof(text));
    check(strstr(text,"\nkept: third\n")!=NULL,"diag: and the next value that fits is the line again");
    unsigned empty=0;
    for(size_t cut=1;cut<=6;cut++) {
        char key[16]; snprintf(key,sizeof(key),"new%u",(unsigned)cut);
        fail_at=attempts+cut;
        yonder_diag_fact(d,key,"value");
        fail_at=0;
        yonder_diag_render(d,text,sizeof(text));
        char empty_line[32]; snprintf(empty_line,sizeof(empty_line),"\n%s: \n",key);
        empty+=strstr(text,empty_line)!=NULL;
    }
    check(empty==0,"diag: a new fact short of memory is never an empty line");
    yonder_diag_free(d);
}
static void diag_css_unasked_case(void) {
    /* A page whose only picture is refused before the table: nothing will
     * ever come in to write the record, so meeting it writes it. */
    probe_page("<img src='data:image/png;base64,iVBORw0KGgo='>",true);
    g.page.diag=diag_new_claimed("http://only.test/",1);
    pictures_start(&g.page);
    char only[64]; yonder_diag_file_name(g.page.diag,only,sizeof(only));
    check(g.page.npics==0 && has(diag_file("/tmp/diag",only),"MISSING picture-scheme data (1)"),
        "diag: a page whose only picture is refused writes the record when it meets it");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
    probe_page("<style>p{background-image:url('data:image/png;base64,AAAA')}</style><p>x</p>",true);
    g.page.diag=diag_new_claimed("http://c.test/",1);
    css_pictures(&g.page);
    css_pictures(&g.page);          /* every layout meets it again */
    char text[2048]; yonder_diag_render(g.page.diag,text,sizeof(text));
    check(has(text,"MISSING picture-scheme data (1)") && g.page.unasked==1,
        "diag: a sheet's picture refused before the table is on the record, once");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
}

/* A page laid out again and again cascades its sheets each time (GARB.md
 * § The cost). A cascade that comes up short — here @media nested past the
 * cascade's bound — is the page's failure, ONE line however many of its
 * layouts meet it. And a big sheet is the same after every layout as
 * before the first, every layout styling the whole page: three cascades of
 * this 1 MiB sheet would fill GARB_ARENA_MAX, were what a cascade reads
 * still kept in the sheet, and the case makes nine. */
static void diag_style_case(void) {
    static const char kShort[]="the cascade came up short (out of room, or nested too deep): "
        "part of the page's styles were not applied";
    char line[256], text[4096];
    char deep[512]; int at=0;
    at+=snprintf(deep+at,sizeof(deep)-(size_t)at,"<style>");
    for(int i=0;i<17;i++) at+=snprintf(deep+at,sizeof(deep)-(size_t)at,"@media all{");
    at+=snprintf(deep+at,sizeof(deep)-(size_t)at,"p{color:red}");
    for(int i=0;i<17;i++) at+=snprintf(deep+at,sizeof(deep)-(size_t)at,"}");
    snprintf(deep+at,sizeof(deep)-(size_t)at,"</style><p>x</p>");
    probe_page(deep,false);
    g.page.diag=diag_new_claimed("http://style.test/",1);
    for(int w=700;w>=500;w-=100)
        check(page_lay_out(&g.page,w,600,g.zoom,&g.page),"diag: a page whose cascade comes up short is laid out");
    snprintf(line,sizeof(line),"\nFAILED style: %s (1)\n",kShort);
    yonder_diag_render(g.page.diag,text,sizeof(text));
    check(garb_cascade_incomplete(g.page.cascade) && has(text,line) && has(text,"verdict: 1 FAILED\n"),
        "diag: a cascade that came up short is one FAILED style line, however many layouts met it");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
    /* A rebuild a script's change stages is laid out on a copy of the page,
     * which borrows its record: that cascade says it too. */
    snprintf(deep+at,sizeof(deep)-(size_t)at,"</style><p id=x>x</p>");
    probe_page(deep,false);
    g.page.diag=diag_new_claimed("http://staged.test/",1);
    check(os64_html_set_attr(g.page.way.doc,probe_id("x"),"title","y",1)==OS64_HTML_OK && script_rebuild(),
        "diag: a script's change is laid out on a staged copy of the page");
    yonder_diag_render(g.page.diag,text,sizeof(text));
    check(has(text,line),"diag: the staged copy's short cascade is on the page's record");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();

    static const char kRule[]=".b%d:not(:disabled).on,.b%d:active,.show>.b%d.t{color:#fff;"
        "background-color:#07f;border-color:#07f;box-shadow:0 0 0 .2rem rgba(0,123,255,.5)}\n"
        "@media (min-width:%dpx){.c%d{flex:0 0 8.3%%;max-width:8.3%%}}\n";
    size_t cap=((size_t)1<<20)+8192, len=0;
    char *html=malloc(cap);
    len+=(size_t)snprintf(html,cap,"<style>");
    int rules=0;
    while(len+512<((size_t)1<<20)) { len+=(size_t)snprintf(html+len,cap-len,kRule,rules,rules,rules,300+rules%300,rules%12); rules++; }
    len+=(size_t)snprintf(html+len,cap-len,"</style>");
    int elements=0;
    for(int i=0;i<rules;i+=rules/48+1,elements++) len+=(size_t)snprintf(html+len,cap-len,"<p class='b%d on'>%d</p>",i,i);
    for(int k=0;k<12;k++,elements++) len+=(size_t)snprintf(html+len,cap-len,"<div class=c%d>%d</div>",k,k);
    probe_page(html,false);
    free(html);
    g.page.diag=diag_new_claimed("http://big.test/",1);
    const garb_parsed_t *sheet=g.page.nsheets==1 ? &g.page.sheets[0].parsed : NULL;
    size_t parsed=sheet!=NULL ? os64_arena_stats(sheet->arena).used_bytes : 0;
    bool whole=sheet!=NULL && !sheet->incomplete;
    for(int i=0;i<8 && whole;i++) {
        whole=page_lay_out(&g.page,780-i*20,600,g.zoom,&g.page) && !garb_cascade_incomplete(g.page.cascade) &&
            !sheet->incomplete && os64_arena_stats(sheet->arena).used_bytes==parsed;
        int styled=0;
        for(const os64_html_node_t *n=g.page.way.doc->body->first_child;n!=NULL;n=n->next)
            styled+=n->kind==OS64_HTML_ELEMENT && garb_style_for(g.page.cascade,n).n>0;
        whole=whole && styled==elements;
    }
    yonder_diag_render(g.page.diag,text,sizeof(text));
    check(whole && !has(text,"FAILED style"),
        "diag: a 1 MiB sheet laid out at eight widths styles the whole page each time, and stays as it was parsed");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
}

/* Text, a line and a marker carry their element's style, background and
 * all, but nothing is drawn behind them: a moving picture behind a box is
 * not on screen when only the text overflowing the box is, and marks only
 * the box's part, not a marker hung outside it. */
static void picture_text_cases(void) {
    static struct os64_image_sequence fake;
    static char html[2048];
    int32_t n=os64_snprintf(html,sizeof(html),"<!doctype html><style>body{margin:0}"
        "#c{height:20px;width:100px;background:url(c.gif)}"
        "ul{margin:0 0 0 60px;padding:0}li{background:url(m.gif);list-style-position:outside}"
        "</style><div id=c>");
    for(int i=0;i<60;i++) n+=os64_snprintf(html+n,sizeof(html)-(size_t)n,"line %d<br>",i);
    os64_snprintf(html+n,sizeof(html)-(size_t)n,"</div><div style='height:3000px'></div>"
        "<ul><li>x</li></ul>");
    probe_page(html,false);
    pictures_start(&g.page);
    css_pictures(&g.page);
    int32_t c=-1, m=-1;
    for(int32_t k=0;k<g.page.npics;k++) {
        memset(&fake,0,sizeof(fake));
        fake.frame.delay_ms=100; fake.frame.width=fake.frame.height=1;
        g.page.pics[k].state=PIC_SHOWN; g.page.pics[k].moving=&fake;
        g.page.pics[k].playing=true; g.page.pics[k].due=1000;
        if(strstr(g.page.pics[k].url,"c.gif")) c=k;
        if(strstr(g.page.pics[k].url,"m.gif")) m=k;
    }
    check(c>=0 && m>=0 && g.page.npics<=8,"picture text: two moving pictures");
    bool seen[8], mark[8]={false};
    /* Scrolled past the box, into the lines that overflow it. */
    g.sy=200;
    pictures_seen(seen,NULL);
    check(!seen[c] && !picture_on_screen(c,false),
          "picture text: a box scrolled away is not on screen for its overflowing text");
    g.ui.any_dirty=false; g.ui.dirty=(os64_gui_rect_t){0,0,0,0};
    mark[c]=true;
    pictures_seen(seen,mark);
    (void)picture_on_screen(c,true);
    check(!g.ui.any_dirty,"picture text: nor is its overflowing text marked");
    /* The list item's marker hangs left of it, at 60px: its part only. */
    const flow_box_t *li=NULL;
    for(os64_html_node_t *e=g.page.way.doc->document;e!=NULL;e=(os64_html_node_t *)next_within(e,g.page.way.doc->document))
        if(e->tag==OS64_HTML_TAG_LI) li=flow_box_for(g.page.tree,e);
    check(li!=NULL,"picture text: the list item's box");
    if(li!=NULL) {
        os64_gui_rect_t r=flow_box_doc_rect(li,scroll_now());
        g.sy=r.y-100;
        os64_gui_rect_t at=flow_box_doc_rect(li,scroll_now());
        memset(mark,0,sizeof(mark)); mark[m]=true;
        g.ui.any_dirty=false; g.ui.dirty=(os64_gui_rect_t){0,0,0,0};
        pictures_seen(seen,mark);
        bool one=g.ui.any_dirty && g.ui.dirty.x>=g.view.bounds.x+at.x;
        g.ui.any_dirty=false; g.ui.dirty=(os64_gui_rect_t){0,0,0,0};
        (void)picture_on_screen(m,true);
        bool apart=g.ui.any_dirty && g.ui.dirty.x>=g.view.bounds.x+at.x;
        check(at.x==60 && seen[m] && one && apart,
              "picture text: a marker hung outside its box is not marked for the box's picture");
    }
    g.sy=0;
    for(int32_t k=0;k<g.page.npics;k++) g.page.pics[k].moving=NULL;
    probe_drop();
}

/* A picture behind a rounded box follows its curve, through yonder's own
 * face: a picture of one opaque blue pixel paints every pixel as the same
 * box's blue colour does — the edge's shares as well — and a patterned
 * picture paints, outside the corners' squares, every pixel as the same
 * box square does. */
static void round_picture_glass_cases(void) {
    static uint32_t blue=0xff0000ffu, pattern[4]={0xffff0000u,0xff00ff00u,0xff0000ffu,0xffffff00u};
    static const char *const kPages[4]={
        "#c{border-radius:30px 12px 40px 0/20px 30px 25px 0;background:url(p.gif)}",
        "#c{border-radius:30px 12px 40px 0/20px 30px 25px 0;background:#0000ff}",
        "#c{border-radius:30px 12px 40px 0/20px 30px 25px 0;background:url(q.gif)}",
        "#c{background:url(q.gif)}"};
    uint32_t *shot[4]={NULL};
    for(int i=0;i<4;i++) {
        static char html[512];
        os64_snprintf(html,sizeof(html),"<!doctype html><style>html{background:#fff}body{margin:0}"
            "#c{margin:10px 20px;width:160px;height:80px}%s</style><div id=c></div>",kPages[i]);
        probe_page(html,false);
        pictures_start(&g.page); css_pictures(&g.page);
        for(int32_t k=0;k<g.page.npics;k++) {
            bool q=strstr(g.page.pics[k].url,"q.gif")!=NULL;
            g.page.pics[k].image.pixels=os64_malloc(q ? sizeof(pattern) : sizeof(blue));
            os64_memcpy(g.page.pics[k].image.pixels,q ? pattern : &blue,q ? sizeof(pattern) : sizeof(blue));
            g.page.pics[k].image.width=g.page.pics[k].image.height=q ? 2 : 1;
            g.page.pics[k].state=PIC_SHOWN;
        }
        os64_draw_ctx_t screen={0};
        screen.surf.width=screen.surf.pitch_px=800; screen.surf.height=600;
        screen.surf.pixels=os64_calloc(800u*600u,sizeof(uint32_t));
        g.paint_cache=yonder_paint_cache_new(32u<<20); g.paint_scratch=yonder_scratch_new();
        g.ui.dirty=g.view.bounds;
        view_paint(&g.view,&screen,NULL);
        yonder_paint_cache_free(g.paint_cache); g.paint_cache=NULL;
        yonder_scratch_free(g.paint_scratch); g.paint_scratch=NULL;
        shot[i]=screen.surf.pixels;
        probe_drop();
    }
    /* The corners' squares, from the radii above, in the page. */
    const os64_gui_rect_t sq[3]={{20,10,30,20},{168,10,12,30},{140,65,40,25}};
    int most=0, differ=0;
    for(int32_t y=0;y<120;y++)
        for(int32_t x=0;x<220;x++) {
            size_t at=(size_t)y*800+(size_t)x;
            for(int s=0;s<=16;s+=8) {
                int d=abs((int)((shot[0][at]>>s)&0xff)-(int)((shot[1][at]>>s)&0xff));
                most=d>most ? d : most;
            }
            bool corner=false;
            for(int k=0;k<3;k++)
                corner=corner || (x>=sq[k].x && x<sq[k].x+sq[k].w && y>=sq[k].y && y<sq[k].y+sq[k].h);
            if(!corner && shot[2][at]!=shot[3][at]) differ++;
        }
    check(most<=1,"round picture: a blue picture's corners are the blue colour's, share for share");
    check(differ==0,"round picture: outside its corners a rounded picture is the square one's");
    check(shot[0][(size_t)10*800+20]==shot[1][(size_t)10*800+20] &&
          (shot[0][(size_t)10*800+20]&0xffffffu)==0xffffffu,
          "round picture: the corner's own pixel is the page's, not the picture's");
    for(int i=0;i<4;i++) os64_free(shot[i]);
}

/* A last picture that moves the page is written for after the page is laid
 * out again, so the record says what that layout came to. */
/* The ticks of moving pictures (YONDER_PERF.md): one pass over the page
 * answers and marks what asking each picture apart does, for every kind
 * of place a picture is shown; a tick advances the pictures due and on
 * screen; and a frame's next deadline lands on the grid, so two pictures
 * of one delay share their ticks. */
static void picture_tick_cases(void) {
    static struct os64_image_sequence fakes[8];
    probe_page("<!doctype html><style>body{margin:0;background:url(d.gif)}"
               "#c{width:50px;height:30px;background:url(c.gif)}</style>"
               "<img src=a.gif width=20 height=20><div id=c></div>"
               "<table background=e.gif width=40 height=20><tr><td>x</td></tr></table>"
               "<table background=f.gif style='background-image:url(g.gif)' width=40 height=20><tr><td>y</td></tr></table>"
               "<div style='height:3000px'></div><img src=b.gif width=20 height=20>",false);
    pictures_start(&g.page);
    css_pictures(&g.page);
    check(g.page.npics==7 && g.page.npics<=8,"picture ticks: seven pictures, in every kind of place");
    for(int32_t k=0;k<g.page.npics && k<8;k++) {
        memset(&fakes[k],0,sizeof(fakes[k]));
        fakes[k].frame.delay_ms=100; fakes[k].frame.width=fakes[k].frame.height=1;
        g.page.pics[k].state=PIC_SHOWN; g.page.pics[k].moving=&fakes[k];
        g.page.pics[k].playing=true; g.page.pics[k].due=1000;
    }
    bool seen[8], mark[8];
    pictures_seen(seen,NULL);
    bool same=true, offscreen=false;
    for(int32_t k=0;k<g.page.npics;k++) {
        same=same && seen[k]==picture_on_screen(k,false);
        if(strstr(g.page.pics[k].url,"b.gif")!=NULL) offscreen=!seen[k];
    }
    check(same && offscreen,"picture ticks: one pass answers what asking each picture apart does");
    /* A `background` attribute a sheet's picture replaces is not shown, so
     * its picture is not on screen — asked either way — while the sheet's is. */
    bool hidden_seen=true, shown_seen=false;
    for(int32_t k=0;k<g.page.npics;k++) {
        if(strstr(g.page.pics[k].url,"f.gif")) hidden_seen=seen[k] || picture_on_screen(k,false);
        if(strstr(g.page.pics[k].url,"g.gif")) shown_seen=seen[k] && picture_on_screen(k,false);
    }
    check(!hidden_seen && shown_seen,"picture ticks: an attribute's picture a sheet replaces is not on screen");
    bool marks_same=true;
    for(int32_t k=0;k<g.page.npics;k++) {
        memset(mark,0,sizeof(mark)); mark[k]=true;
        g.ui.any_dirty=false; g.ui.dirty=(os64_gui_rect_t){0,0,0,0};
        pictures_seen(seen,mark);
        bool any1=g.ui.any_dirty; os64_gui_rect_t d1=g.ui.dirty;
        g.ui.any_dirty=false; g.ui.dirty=(os64_gui_rect_t){0,0,0,0};
        (void)picture_on_screen(k,true);
        marks_same=marks_same && any1==g.ui.any_dirty && (!any1 || (d1.x==g.ui.dirty.x && d1.y==g.ui.dirty.y &&
                   d1.w==g.ui.dirty.w && d1.h==g.ui.dirty.h));
    }
    check(marks_same,"picture ticks: one pass marks what asking each picture apart marks");
    /* Due at two ticks of the clock, both advanced by the tick after, each
     * then due its delay after the tick that showed it. */
    int32_t a=-1, c=-1, b=-1;
    for(int32_t k=0;k<g.page.npics;k++) {
        if(strstr(g.page.pics[k].url,"a.gif")) a=k;
        if(strstr(g.page.pics[k].url,"c.gif")) c=k;
        if(strstr(g.page.pics[k].url,"b.gif")) b=k;
    }
    g.page.pics[a].due=1000; g.page.pics[c].due=1010;
    now_ms=1010;
    pictures_tick();
    check(fakes[a].nexts==1 && fakes[c].nexts==1 && fakes[b].nexts==0,
          "picture ticks: a tick advances the pictures due and on screen, not one scrolled away");
    check(g.page.pics[a].due==1110 && g.page.pics[c].due==1110,
          "picture ticks: each next frame is due its delay after the tick that showed it");
    /* A past moment the loop meets itself waits for the clock's next tick,
     * unless a picture is due at the same moment: then it is rung for now. */
    {
        uint64_t settle=g.settle_due;
        for(int32_t k=0;k<g.page.npics;k++) g.page.pics[k].playing=(k==a);
        now_ms=2000; g.settle_due=1500; g.page.pics[a].due=1500;
        pictures_schedule();
        check(ticker_due==1500,"picture ticks: a picture due with the settle is rung for at once");
        g.page.pics[a].due=1600;
        pictures_schedule();
        check(ticker_due==2001,"picture ticks: the settle alone waits for the clock's next tick");
        g.settle_due=settle;
    }
    for(int32_t k=0;k<g.page.npics;k++) { g.page.pics[k].moving=NULL; g.page.pics[k].playing=false; }
    now_ms=0;
    probe_drop();
}

static void diag_picture_moves_case(void) {
    probe_page("<img src=file:///grows.png>",true);
    pictures_start(&g.page);
    g.page.diag=diag_new_claimed("http://m.test/",1);
    g.page.pics[0].state=PIC_WAITING; g.page.waiting=g.page.in_flight=1;
    g.pictures_moved=true;
    yonder_picture_job_t *job=os64_calloc(1,sizeof(*job));
    yonder_picture_t *got=os64_calloc(1,sizeof(*got));
    job->kind=YONDER_JOB_PICTURE; job->index=0; job->generation=g.page_serial;
    got->status=OS64_IMAGE_OK;
    reaped(0,job,got);
    char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
    check(g.page.record_owed && !has(diag_file("/tmp/diag",name),"when its pictures and fonts were in"),
        "diag: the last picture that moves the page waits for its layout to be written");
    pictures_settle();
    check(!g.page.record_owed && has(diag_file("/tmp/diag",name),"\nwritten: when its pictures and fonts were in\n"),
        "diag: once the page is laid out again, the record is written");
    /* The workers stopping after such a picture waits for the layout too. */
    g.page.pics[0].state=PIC_WAITING; g.page.waiting=g.page.in_flight=1;
    yonder_diag_fact(g.page.diag,"written","before");
    diag_write(g.page.diag);
    g.pictures_moved=true;
    pictures_orphaned(&g.page);
    check(g.page.record_owed && has(diag_file("/tmp/diag",name),"\nwritten: before\n"),
        "diag: a stopped pool after a moving picture waits for its layout to be written");
    pictures_settle();
    check(!g.page.record_owed && has(diag_file("/tmp/diag",name),"the workers stopped"),
        "diag: and is written once the page is laid out again");
    yonder_diag_free(g.page.diag); g.page.diag=NULL;
    probe_drop();
}

static void diag_unit_cases(void) {
    yonder_diag_t *d=yonder_diag_new("http://Example.COM:8080/FAILED?x=MISSING",3,0);
    char text[4096];
    yonder_diag_render(d,text,sizeof(text));
    check(!strcmp(text,"address: http://Example.COM:8080/F%41ILED?x=M%49SSING\nverdict: clean\n" YONDER_DIAG_END "\n"),
        "diag: a clean record's verdict, and a token in a plain line spelled with its second letter encoded");
    yonder_diag_missing(d,"global","fetch",1); yonder_diag_missing(d,"global","fetch",2);
    yonder_diag_missing_seen(d,"element","canvas",3); yonder_diag_missing_seen(d,"element","canvas",2);
    yonder_diag_failed(d,"script a.js","line one\nFAILED x\\y"); yonder_diag_failed(d,"script a.js","line one\nFAILED x\\y");
    yonder_diag_fact(d,"bytes","1"); yonder_diag_fact(d,"bytes","2");
    yonder_diag_render(d,text,sizeof(text));
    check(has(text,"\nverdict: 2 MISSING, 1 FAILED\nMISSING global fetch (3)\nMISSING element canvas (3)\n"
        "FAILED script a.js: line one\\x0AF%41ILED x\\\\y (2)\nbytes: 2\n"),
        "diag: asks add, a whole tally holds its most, a failure counts, a line breaker and a token in data are escaped, a fact is replaced");
    yonder_diag_t *one=yonder_diag_new("http://x.test/",1,0);
    yonder_diag_failed(one,"script MISSING.js","Error: MISSING config");
    yonder_diag_render(one,text,sizeof(text));
    check(has(text,"\nverdict: 1 FAILED\nFAILED script M%49SSING.js: Error: M%49SSING config (1)\n") &&
        strstr(text,"MISSING")==NULL,
        "diag: the verdict names only a token with records, and record data spelling the other is encoded");
    yonder_diag_free(one);
    check(yonder_diag_missing_lines(d)==2 && yonder_diag_failed_lines(d)==1,"diag: the verdict's numbers are lines");
    char name[64];
    check(yonder_diag_file_name(d,name,sizeof(name)) && !strcmp(name,"example.com-0003.txt"),
        "diag: the file is named by the host, lower case, without its port, and the sequence");
    yonder_diag_free(d);
    d=yonder_diag_new("file:///pages/a.html",12345,0);
    check(yonder_diag_file_name(d,name,sizeof(name)) && !strcmp(name,"file-12345.txt"),"diag: a file's page is `file`");
    yonder_diag_free(d);
    d=yonder_diag_new("about:blank",1,0);
    check(yonder_diag_file_name(d,name,sizeof(name)) && !strcmp(name,"page-0001.txt"),"diag: no host is `page`");
    for(unsigned i=0;i<YONDER_DIAG_RECORDS_MAX+3;i++) { char n[16]; snprintf(n,sizeof(n),"g%u",i); yonder_diag_missing(d,"global",n,1); }
    size_t need=yonder_diag_render(d,NULL,0); char *big=malloc(need+1); yonder_diag_render(d,big,need+1);
    check(yonder_diag_missing_lines(d)==YONDER_DIAG_RECORDS_MAX && has(big,"\nmissing names not kept: 3\n"),
        "diag: past the table's room, a name is counted on a plain line rather than kept");
    free(big); yonder_diag_free(d);
    fake_reset();
    snprintf(fake_dirs[nfake_dirs++],256,"/tmp/seq");
    int64_t h=os64_open("/tmp/seq/a.example-0041.txt","w"); os64_close((int32_t)h);
    h=os64_open("/tmp/seq/notes.txt","w"); os64_close((int32_t)h);
    h=os64_open("/tmp/seq/b-x-0007.txt","w"); os64_close((int32_t)h);
    check(yonder_diag_next_seq("/tmp/seq")==42 && yonder_diag_next_seq("/nowhere")==1,
        "diag: a run continues the directory's sequence, and starts at one where there is none");
}
/* yonder.conf names a directory: made under a parent that exists, said
 * once when it cannot be. */
static void diag_setting_cases(void) {
    fake_reset();
    memset(&s_diag,0,sizeof(s_diag)); s_diag.next=1;
    snprintf(saved_diagnostics,sizeof(saved_diagnostics),"/nowhere/pages");
    const char *said=diag_dir_open();
    check(said!=NULL && has(said,"/nowhere/pages cannot be made") && s_diag.dir[0]=='\0',
        "diag: a directory whose parent is missing is said, and no files are written");
    snprintf(saved_diagnostics,sizeof(saved_diagnostics),"/tmp/diag/");
    said=diag_dir_open();
    check(said==NULL && !strcmp(s_diag.dir,"/tmp/diag") && fake_is_dir("/tmp/diag") && s_diag.next==1,
        "diag: a missing directory under a parent that exists is made, its trailing slash dropped");
    saved_diagnostics[0]='\0';
}
static void diag_page_cases(void) {
    uint32_t seq;
    /* A clean page: a script that uses only what is there. */
    loop_page("<p id=out>x</p><script>document.getElementById('out').textContent='ran'</script>");
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","ran"),"diag: a clean scripted page arrives");
    char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
    const char *text=diag_file("/tmp/diag",name);
    check(has(text,"address: http://fixture.test/p\nverdict: clean\n") && !has(text,"MISSING") && !has(text,"FAILED") &&
        has(text,"\nwritten: when the page arrived\n") && has(text,"\nfinal address: http://fixture.test/final\n") &&
        has(text,"\nscript http://fixture.test/final#inline-1: ran in 0 us\n") && !has(text,"script load"),
        "diag: a clean page's file says clean, carries no token, and says what ran and how long the "
        "engine took over it, on the clock that stood still");
    check(g.badge_text[0]=='\0' && g.badge.hidden,"diag: a clean page shows no badge");
    loop_drop();
    check(has(diag_file("/tmp/diag",name),"\nwritten: when the page was left\n"),"diag: leaving rewrites the file");

    /* typeof noSuchGlobal: recorded, and the page's path unchanged. */
    loop_page("<p id=out>x</p><script>document.getElementById('out').textContent="
        "String('noSuchGlobal' in window)+' '+typeof noSuchGlobal+' '+(window.noSuchGlobal===undefined)+' '+(self.noSuchGlobal===undefined)</script>");
    check(loop_settle() && probe_text_is("out","false undefined true true"),
        "diag: the page that asks for noSuchGlobal sees what it always saw");
    yonder_diag_file_name(g.page.diag,name,sizeof(name)); text=diag_file("/tmp/diag",name);
    check(has(text,"\nverdict: 1 MISSING\nMISSING global noSuchGlobal (3)\n") && !has(text,"global self"),
        "diag: `typeof noSuchGlobal`, `window.noSuchGlobal` and `self.noSuchGlobal` are one MISSING line counting all three asks");
    check(!strcmp(g.badge_text,"MISSING 1") && !g.badge.hidden,"diag: the badge shows the file's own token and count");
    loop_drop();

    /* A dying script, and a timer that dies after the page arrived. */
    loop_page("<p id=out>x</p><script>nothere();</script>"
        "<script>setTimeout(function(){null.x},50)</script>");
    check(loop_settle() && g.page.tree!=NULL,"diag: a page whose script dies arrives");
    yonder_diag_file_name(g.page.diag,name,sizeof(name));
    char arrival[4096]; snprintf(arrival,sizeof(arrival),"%s",diag_file("/tmp/diag",name));
    check(has(arrival,"\nverdict: 1 MISSING, 1 FAILED\nMISSING global nothere (1)\nFAILED script http://fixture.test/final#inline-1: ") &&
        has(arrival,"nothere"),
        "diag: a dying script writes a FAILED line by its source name, and the verdict counts it");
    now_ms=200;
    check(script_turn(),"diag: the timer fires on the shown page");
    check(!strcmp(g.badge_text,"MISSING 1  FAILED 2"),"diag: the badge follows a failure after arrival");
    check(!strcmp(diag_file("/tmp/diag",name),arrival),"diag: the arrival write is not touched by what came after");
    loop_drop();
    text=diag_file("/tmp/diag",name);
    check(has(text,"\nverdict: 1 MISSING, 2 FAILED\n") && has(text,"FAILED script http://fixture.test/final#timer-1: TypeError"),
        "diag: the timer's death is in the departure write");

    /* A canvas by its kind; a skipped property. A module script runs now
     * (modules.c), and an import map registers (importmap.c): neither is
     * missing. */
    loop_page("<style>p{contain:paint;background-image:conic-gradient(red,blue);background-clip:text}"
        "h1{display:inline frob}</style><p id=out>x</p><canvas></canvas><canvas></canvas>"
        "<my-widget></my-widget><script type=importmap>{}</script><script type=module>1</script>");
    check(loop_settle() && g.page.tree!=NULL,"diag: a page with a module, canvases and a custom element arrives");
    yonder_diag_file_name(g.page.diag,name,sizeof(name)); text=diag_file("/tmp/diag",name);
    check(!has(text,"MISSING script importmap") && !has(text,"MISSING script module") &&
        has(text,"MISSING element canvas (2)\n") &&
        has(text,"MISSING element my-widget (1)\n") && has(text,"MISSING css-property contain (1)\n") &&
        has(text,"MISSING css-value background-image: conic-gradient (1)\n") &&
        has(text,"MISSING css-value display: inline frob (1)\n") && !has(text,"background-clip"),
        "diag: an element drawn as nothing, a property the cascade skipped and a value it dropped or "
        "approximated are each recorded; a module, run, an import map, registered, and a clip to the text, "
        "drawn, are not");
    loop_drop();
    text=diag_file("/tmp/diag",name);
    check(has(text,"MISSING element canvas (2)\n"),"diag: looking again at departure does not count twice");

    /* A frame's document is not loaded into it, and an embed makes no box:
     * both are recorded. An object shows its fallback and is not. */
    loop_page("<p>x</p><iframe src=/poll></iframe><iframe></iframe><embed src=a.swf>"
        "<object data=b.swf>fallback</object>");
    check(loop_settle() && g.page.tree!=NULL,"diag: a page with frames, an embed and an object arrives");
    yonder_diag_file_name(g.page.diag,name,sizeof(name)); text=diag_file("/tmp/diag",name);
    check(has(text,"MISSING element iframe (2)\n") && has(text,"MISSING element embed (1)\n") &&
        !has(text,"MISSING element object"),
        "diag: frames and embeds are recorded as drawn as nothing, an object with its fallback is not");
    check(!has(text,"unsupported"),"diag: a frame, whose stand-in takes a click to its document, is never unsupported");
    loop_drop();

    /* A canvas is the page's failure, one line for the kind however many
     * there are, a script's own among them; with scripts off it is its
     * fallback, and no failure. */
    loop_page("<canvas></canvas><canvas width=10 height=10></canvas>"
        "<script>document.body.appendChild(document.createElement('canvas'))</script>");
    check(loop_settle() && g.page.tree!=NULL,"diag: a page with canvases arrives");
    yonder_diag_file_name(g.page.diag,name,sizeof(name)); text=diag_file("/tmp/diag",name);
    check(has(text,"\nFAILED unsupported: canvas (1)\n") && has(text,"MISSING element canvas (3)\n") &&
        !has(text,"FAILED unsupported: canvas (2)"),
        "diag: three canvases, one a script made, are one FAILED line, and their MISSING count stays");
    loop_drop();
    text=diag_file("/tmp/diag",name);
    check(has(text,"\nFAILED unsupported: canvas (1)\n"),"diag: looking again at departure keeps one line");
    loop_page("<p>x</p><script>document.body.appendChild(document.createElement('canvas'))</script>");
    check(loop_settle() && g.page.tree!=NULL,"diag: a page whose script makes its only canvas arrives");
    yonder_diag_file_name(g.page.diag,name,sizeof(name)); text=diag_file("/tmp/diag",name);
    check(has(text,"\nFAILED unsupported: canvas (1)\n"),"diag: a canvas a script inserted is the page's failure");
    loop_drop();
    loop_scripts_off=true;
    loop_page("<canvas>fallback</canvas>");
    loop_scripts_off=false;
    check(loop_settle() && g.page.tree!=NULL,"diag: a canvas page with scripts off arrives");
    yonder_diag_file_name(g.page.diag,name,sizeof(name)); text=diag_file("/tmp/diag",name);
    check(!has(text,"unsupported") && has(text,"MISSING element canvas (1)\n"),
        "diag: with scripts off a canvas shows its fallback and is no failure");
    loop_drop();
    loop_page("<p>x</p><svg><canvas></canvas></svg>");
    check(loop_settle() && g.page.tree!=NULL,"diag: a page with a canvas inside an svg arrives");
    yonder_diag_file_name(g.page.diag,name,sizeof(name)); text=diag_file("/tmp/diag",name);
    check(!has(text,"unsupported"),"diag: an svg's canvas is no HTML canvas, and no failure");
    loop_drop();

    /* An address that spells a token keeps it out of the plain lines. */
    stream_window(); g.scripts_on=true;
    start_trip("http://fixture.test/FAILED/MISSING",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    seq=yonder_diag_seq(g.stream.diag);
    {
        way_head_t h=stream_head("text/html","",false);
        os64_strcopy(h.url,sizeof(h.url),"http://fixture.test/FAILED/MISSING");
        yonder_mail_post_head(g.stream.mail,&h);
        const char *body="<p>tokens</p>";
        stream_post_body((const uint8_t *)body,strlen(body),YONDER_STREAM_CHUNK);
        stream_post_verdict(true,OS64_FETCH_OK,"");
    }
    check(loop_settle() && g.page.tree!=NULL,"diag: a page whose address spells both tokens arrives");
    snprintf(name,sizeof(name),"fixture.test-%04u.txt",(unsigned)seq);
    check(has(diag_file("/tmp/diag",name),"\nfinal address: http://fixture.test/F%41ILED/M%49SSING\n") &&
        has(diag_file("/tmp/diag",name),"\nverdict: clean\n"),
        "diag: a fact's value that spells a token is encoded, and the page still reads clean");
    loop_drop();

    /* A fetch that is not a page: its own file, FAILED. */
    stream_window(); g.scripts_on=true;
    start_trip("http://fixture.test/gone",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    seq=yonder_diag_seq(g.stream.diag);
    stream_post_verdict(false,OS64_FETCH_OK,"No such page: 404 Not Found");
    loop_settle();
    snprintf(name,sizeof(name),"fixture.test-%04u.txt",(unsigned)seq);
    check(has(diag_file("/tmp/diag",name),"\nFAILED page: No such page: 404 Not Found (1)\n"),
        "diag: a load that never became a page writes its failure");
    loop_drop();
}
/* A page's `bytes:` is its own body's, the second of two in one window. */
static void diag_bytes_case(void) {
    loop_page("<p>first page, padded</p>");
    check(loop_settle() && g.page.tree!=NULL,"diag bytes: the first page arrives");
    start_trip("http://fixture.test/second",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    way_head_t h=stream_head("text/html","",false);
    yonder_mail_post_head(g.stream.mail,&h);
    const char *body="<!doctype html><p>second</p>";
    stream_post_body((const uint8_t *)body,strlen(body),YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    check(loop_settle() && g.page.tree!=NULL,"diag bytes: the second page arrives");
    char text[4096]; yonder_diag_render(g.page.diag,text,sizeof(text));
    char want[32]; snprintf(want,sizeof(want),"\nbytes: %zu\n",strlen(body));
    if(!has(text,want)) fprintf(stderr,"%s",text);
    check(has(text,want),"diag bytes: the second page's file counts its own body, not the first's");
    check(has(text,"\nstatus: 200 OK\n") && strstr(text,"FAILED")==NULL,
        "diag status: a page's status is a plain line, and a 200 is no failure");
    start_trip("http://fixture.test/busy",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    h=stream_head("text/html","",false);
    h.status=429; os64_strcopy(h.reason,sizeof(h.reason),"Too Many Requests");
    yonder_mail_post_head(g.stream.mail,&h);
    body="<!doctype html><p>slow down</p>";
    stream_post_body((const uint8_t *)body,strlen(body),YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    check(loop_settle() && g.page.tree!=NULL,"diag status: an error page arrives, and is shown");
    yonder_diag_render(g.page.diag,text,sizeof(text));
    check(has(text,"\nverdict: 1 FAILED\nFAILED page: HTTP 429 Too Many Requests (1)\n") &&
        has(text,"\nstatus: 429 Too Many Requests\n"),
        "diag status: an error page is a FAILED page, not a clean one");
    loop_drop();
}

/* A src script's whole address names it in the record, however long: as
 * long as Google's front-end bundle's (3,666 bytes) is fetched, and named. */
static void diag_long_source_case(void) {
    static char html[4200], path[3700], tail[3720], want[3800];
    memset(path,'a',sizeof(path)-1); path[sizeof(path)-1]=0;
    snprintf(html,sizeof(html),"<p>x</p><script src=/%s.js></script>",path);
    loop_page(html);
    check(loop_settle() && g.stream.stopped,"diag source: the parse waits for a long src");
    snprintf(tail,sizeof(tail),"/%s.js",path);
    check(script_land(tail,"nothere()"),"diag source: its fetch is out and lands");
    loop_settle();
    char *text=malloc(16384); yonder_diag_render(g.page.diag,text,16384);
    snprintf(want,sizeof(want),"FAILED script http://fixture.test/%s.js: ReferenceError",path);
    check(has(text,want),"diag source: a long src script is named by its whole address");
    free(text);
    loop_drop();
}

/* THE SCRIPT LINE'S TIME (MODERN_WEB.md slice 7's instrument): the engine's
 * evaluation of a script, on the harness's clock, which a forced layout
 * moves by geometry_layout_delay and nothing else moves here. */
static void diag_script_time_case(void) {
    geometry_layout_delay=2500;
    loop_page("<p id=out>x</p><script>document.getElementById('out').textContent=String(document.body.offsetWidth>0)</script>"
        "<script>nothere()</script>");
    check(loop_settle() && probe_text_is("out","true"),"diag time: a script that measures runs");
    geometry_layout_delay=0;
    char text[4096]; yonder_diag_render(g.page.diag,text,sizeof(text));
    check(has(text,"\nscript http://fixture.test/final#inline-1: ran in 2500 us\n") &&
        has(text,"\nscript http://fixture.test/final#inline-2: failed in 0 us\n"),
        "diag time: a script's line says how long the engine took over it, the layout it forced included, "
        "and a script that died says it too");
    loop_drop();
}

/* A sheet job answered as sheet.c answers it: `css` parsed under `label`
 * and the document's encoding, and, for a window that keeps its pages,
 * carrying back the bytes it was parsed from and the label. NULL `css` is a
 * fetch that failed. False when no such job is out. */
static bool sheet_land_as_worker(const char *tail, const char *css, const char *label) {
    for(int i=0;i<nsheet_jobs;i++) {
        yonder_sheet_job_t *job=sheet_jobs[i].job;
        size_t n=strlen(job->url), t=strlen(tail);
        if(n<t || strcmp(job->url+n-t,tail)!=0) continue;
        os64_work_id_t id=sheet_jobs[i].id;
        sheet_jobs[i]=sheet_jobs[--nsheet_jobs];
        yonder_sheet_t *got=os64_calloc(1,sizeof(*got));
        os64_strcopy(got->url,sizeof(got->url),job->url);
        if(css!=NULL) {
            got->ok=garb_parse_sheet((const uint8_t *)css,strlen(css),label[0] ? label : NULL,
                job->environment[0] ? job->environment : NULL,&got->parsed)==GARB_OK;
            os64_strcopy(got->label,sizeof(got->label),label);
            if(got->ok && job->keep) {
                got->kept=os64_malloc(strlen(css)); memcpy(got->kept,css,strlen(css)); got->kept_len=strlen(css);
            }
        }
        reaped(id,job,got);
        return true;
    }
    return false;
}
/* A file on the fake disk, its bytes and length; NULL when there is none. */
static const char *fake_bytes(const char *path, size_t *len) {
    int f=fake_file(path); *len=f>=0 ? fake_files[f].len : 0;
    return f>=0 ? fake_files[f].data : NULL;
}
/* Whether any file on the fake disk is a temporary one still standing. */
static bool fake_temp_left(void) {
    for(int f=0;f<FAKE_FILES;f++)
        if(fake_files[f].used && strlen(fake_files[f].path)>4 &&
           !strcmp(fake_files[f].path+strlen(fake_files[f].path)-4,".new")) return true;
    return false;
}
static unsigned fake_count(const char *suffix) {
    unsigned n=0;
    for(int f=0;f<FAKE_FILES;f++) {
        size_t a=strlen(fake_files[f].path), b=strlen(suffix);
        n+=fake_files[f].used && a>=b && !strcmp(fake_files[f].path+a-b,suffix);
    }
    return n;
}
/* keep_pages starting a run, in a fresh /tmp/kept. */
static void keep_begin(bool on) {
    fake_reset();
    memset(&s_diag,0,sizeof(s_diag)); s_diag.next=1;
    snprintf(saved_diagnostics,sizeof(saved_diagnostics),"/tmp/kept");
    snprintf(saved_keep_pages,sizeof(saved_keep_pages),"%s",on ? "on" : "off");
    check(diag_dir_open()==NULL && s_diag.keep==on,"keep: keep_pages is read with the diagnostics directory");
}
/* Jars for the cookie cases (diag_keep_cases): one holding a Secure cookie
 * it hands only to an https page, one that has heard the response's own
 * Set-Cookie (`fake_cookie_heard`), and one that hands back what the page's
 * script set beside what it held. */
static bool fake_cookie_heard;
static const char *cookie_jar_secure(const char *url, bool encrypted) {
    return encrypted && !strncmp(url,"https://fixture.test/",21) ? "secure=1; plain=1" : "plain=1";
}
static const char *cookie_jar_heard(const char *url, bool encrypted) {
    (void)url; (void)encrypted;
    return fake_cookie_heard ? "heard=1" : "";
}
static const char *cookie_jar_scripted(const char *url, bool encrypted) {
    static char text[300];
    (void)url; (void)encrypted;
    snprintf(text,sizeof(text),"first=1%s%s",fake_cookie_set[0] ? "; " : "",fake_cookie_set);
    return text;
}
static void keep_end(void) {
    saved_diagnostics[0]=saved_keep_pages[0]='\0';
    memset(&s_diag,0,sizeof(s_diag)); s_diag.next=1;
    fake_reset();
}

/* THE PAGE KEPT (keep_pages, diag.h): the bytes as the parser was fed them,
 * each script and sheet as the window received it, the index naming them,
 * and the facts a replay rebuilds the head from. */
static void diag_keep_cases(void) {
    keep_begin(true);
    stream_window(); g.scripts_on=true; g.script_ms=5000; now_ms=100;
    start_trip("http://fixture.test/kept",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    uint32_t seq=yonder_diag_seq(g.stream.diag);
    way_head_t h=stream_head("text/html","windows-1252",false);
    h.encrypted=true; os64_strcopy(h.url,sizeof(h.url),"https://fixture.test/final");
    yonder_mail_post_head(g.stream.mail,&h);
    /* A byte that is a character only in the page's encoding, a NUL, and
     * a body cut into many small chunks: the kept file is the bytes
     * whatever the chunks and whatever they decode to. */
    char *body=pad_page("<link rel=stylesheet href=k.css><p id=out>caf\xE9</p><p>\x01" "a\xFF" "z</p>"
        "<script src=k.js></script><script src=gone.js></script><p id=after>end</p>");
    size_t len=strlen(body);
    body[len-2]='\0';                   /* a NUL inside the page's last tag */
    stream_post_body((const uint8_t *)body,len,97);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    check(loop_settle() && g.stream.stopped,"keep: the page stops at its script");
    check(sheet_land_as_worker("/k.css","@import 'i.css'; #out{color:red}","utf-8"),"keep: its sheet lands");
    check(sheet_land_as_worker("/i.css","p{margin:0}",""),"keep: the sheet's @import lands");
    check(script_land("/k.js","document.getElementById('out').textContent='ran'"),"keep: its script lands");
    check(loop_settle() && g.stream.stopped && script_land("/gone.js",NULL),"keep: the next script's fetch fails");
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","ran"),"keep: the page arrives");
    char base[64], path[128]; size_t n;
    snprintf(base,sizeof(base),"/tmp/kept/fixture.test-%04u",(unsigned)seq);
    snprintf(path,sizeof(path),"%s.html",base);
    const char *kept=fake_bytes(path,&n);
    check(kept!=NULL && n==len && memcmp(kept,body,len)==0,
        "keep: the page's bytes are kept as the parser was fed them, undecoded, in however many chunks");
    snprintf(path,sizeof(path),"%s.d/index",base);
    const char *index=fake_text(path);
    check(index!=NULL && !strncmp(index,"# yonder kept index 2\n",22) &&
        has(index,"\n1\tsheet\thttps://fixture.test/k.css\tcame\thttps://fixture.test/k.css\tutf-8\t0\n"
        "2\tsheet\thttps://fixture.test/i.css\tcame\thttps://fixture.test/i.css\t\t1\n"
        "3\tscript\thttps://fixture.test/k.js\tcame\thttps://fixture.test/k.js\t\t1\n"
        "4\tscript\thttps://fixture.test/gone.js\tfailed\thttps://fixture.test/gone.js\t\t2\n") &&
        has(index,"fonts and pictures are not kept"),
        "keep: the index names its version, then each sheet and script in the order it came back, whether "
        "it came, its addresses, label and job, under a header saying what is not kept");
    snprintf(path,sizeof(path),"%s.d/4.js",base);
    check(fake_text(path)==NULL,"keep: a script that did not come has a line and no file");
    snprintf(path,sizeof(path),"%s.d/1.css",base);
    check(fake_text(path)!=NULL && !strcmp(fake_text(path),"@import 'i.css'; #out{color:red}"),
        "keep: a sheet is kept as the bytes it was parsed from");
    snprintf(path,sizeof(path),"%s.d/3.js",base);
    check(fake_text(path)!=NULL && !strcmp(fake_text(path),"document.getElementById('out').textContent='ran'"),
        "keep: a script is kept as the source the page ran");
    char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
    const char *text=diag_file("/tmp/kept",name);
    check(has(text,"\nstatus: 200 OK\nfinal address: https://fixture.test/final\nbody: html\n"
        "content-type: text/html\ncharset: windows-1252\nencrypted: yes\n") && !has(text,"\nscripts: off\n") &&
        has(text,"\nkept: whole\n"),
        "keep: the page file carries the facts a replay rebuilds the head from");
    check(!fake_temp_left(),"keep: no temporary file is left");
    loop_drop();
    free(body);

    /* A page let go unshown: its sheet never comes, and another address is
     * asked for while it waits. It was kept when its parse ended. */
    stream_window(); g.scripts_on=true; g.script_ms=5000; now_ms=100;
    start_trip("http://fixture.test/unshown",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    seq=yonder_diag_seq(g.stream.diag);
    h=stream_head("text/html","",false);
    yonder_mail_post_head(g.stream.mail,&h);
    const char *waits="<link rel=stylesheet href=never.css><p>waiting</p>";
    stream_post_body((const uint8_t *)waits,strlen(waits),YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    check(loop_settle() && g.coming.active && g.page.tree==NULL,"keep: the page waits for its sheet");
    start_trip("http://fixture.test/elsewhere",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    snprintf(base,sizeof(base),"/tmp/kept/fixture.test-%04u",(unsigned)seq);
    snprintf(path,sizeof(path),"%s.html",base);
    kept=fake_bytes(path,&n);
    snprintf(name,sizeof(name),"fixture.test-%04u.txt",(unsigned)seq);
    check(kept!=NULL && n==strlen(waits) && memcmp(kept,waits,n)==0 &&
        has(diag_file("/tmp/kept",name),"\nwritten: when the page was left\n"),
        "keep: a page let go unshown is kept, and its record is written at its departure");
    /* A stream let go before its verdict keeps nothing, and leaves nothing. */
    seq=yonder_diag_seq(g.stream.diag);
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_body((const uint8_t *)"<p>half",7,YONDER_STREAM_CHUNK);
    (void)stream_turn();
    stop_trip();
    snprintf(path,sizeof(path),"/tmp/kept/fixture.test-%04u.html",(unsigned)seq);
    snprintf(name,sizeof(name),"fixture.test-%04u.txt",(unsigned)seq);
    check(fake_text(path)==NULL && !fake_temp_left() && diag_file("/tmp/kept",name)!=NULL,
        "keep: a page stopped before all its bytes came keeps none, its temporary file goes, and its record is written");
    loop_drop();

    /* No room for the page's folder: no name with fresh kept files can be
     * claimed, the page is not whole, its record says so, and the log and
     * the status line say so once a run. */
    stream_window(); g.scripts_on=true; g.script_ms=5000; now_ms=100;
    while(nfake_dirs<16) { snprintf(fake_dirs[nfake_dirs],256,"/full/%d",nfake_dirs); nfake_dirs++; }
    start_trip("http://fixture.test/nofolder",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    check(s_diag.keep_said && has(last_debug,"could not be kept in /tmp/kept"),
        "keep failed: a page whose kept files cannot be claimed is said as it starts");
    h=stream_head("text/html","",false);
    yonder_mail_post_head(g.stream.mail,&h);
    const char *needs="<p id=out>x</p><script src=s.js></script>";
    stream_post_body((const uint8_t *)needs,strlen(needs),YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    check(loop_settle() && g.stream.stopped,"keep failed: the page stops at its script");
    check(script_land("/s.js","1") && loop_settle() && g.page.tree!=NULL,"keep failed: the page arrives all the same");
    yonder_diag_file_name(g.page.diag,name,sizeof(name));
    text=diag_file("/tmp/kept",name);
    snprintf(path,sizeof(path),"/tmp/kept/%.*s.html",(int)(strlen(name)-4),name);
    check(has(text,"\nkept: not whole: a step of keeping the page failed\n") && fake_text(path)==NULL &&
        !fake_temp_left(),
        "keep failed: the record says the page is not whole, and nothing more of it is kept");
    loop_drop();
    keep_end();

    /* The token spelling is injective: an address that holds `M%49SSING`
     * of its own and one that holds MISSING are written apart. */
    {
        yonder_diag_t *d=yonder_diag_new("http://fixture.test/a?x=M%49SSING&y=MISSING&z=%2",1,0);
        yonder_diag_fact(d,"literal","%25 and 75%");
        char text[512]; yonder_diag_render(d,text,sizeof(text));
        const char *want="address: http://fixture.test/a?x=M%2549SSING&y=M%49SSING&z=%2\n";
        check(!strncmp(text,want,strlen(want)),
            "diag: a `%` a reader would take for part of a spelling is written %25, so a literal M%49SSING "
            "never reads back as the token, and any other `%` is written as it is");
        check(has(text,"\nliteral: %2525 and 75%\n"),"diag: a literal %25 is written %2525, a lone `%` as it is");
        yonder_diag_free(d);
    }

    /* A head fact has room the page's other plain lines cannot take. */
    {
        yonder_diag_t *d=yonder_diag_new("http://fixture.test/many",1,0);
        for(unsigned i=0;i<YONDER_DIAG_FACTS_MAX+5;i++) { char k[16]; snprintf(k,sizeof(k),"script %u",i); yonder_diag_fact(d,k,"ran"); }
        yonder_diag_head_fact(d,"body","html");
        yonder_diag_fact(d,"one more","lost");
        char *big=malloc(1<<16); yonder_diag_render(d,big,1<<16);
        check(has(big,"\nbody: html\n") && !has(big,"one more") && has(big,"\nfacts not kept: 6\n") &&
              has(big,"\n" YONDER_DIAG_LINES_LOST "\n"),
            "keep: a head fact is kept on a page whose plain lines fill the table, which still counts the rest, "
            "and says it lost lines");
        free(big); yonder_diag_free(d);
        /* Exactly the ordinary quota, and the head besides, one head fact
         * set before them all (as a claim sets `kept lines:`): the head's
         * room is its own, so nothing is lost and the record ends whole. */
        d=yonder_diag_new("http://fixture.test/full",1,0);
        yonder_diag_head_fact(d,"kept lines","0");
        for(unsigned i=0;i<YONDER_DIAG_FACTS_MAX;i++) { char k[16]; snprintf(k,sizeof(k),"script %u",i); yonder_diag_fact(d,k,"ran"); }
        static const char *const kHead[]={"status","final address","body","content-type","charset","encrypted",
            "TLS","scripts","laid out at","kept","kept lines"};
        for(size_t i=0;i<sizeof(kHead)/sizeof(kHead[0]);i++) yonder_diag_head_fact(d,kHead[i],"x");
        big=malloc(1<<16); yonder_diag_render(d,big,1<<16);
        check(has(big,"\nscript 511: ran\n") && has(big,"\nkept lines: x\n") && !has(big,"not kept") &&
              !has(big,YONDER_DIAG_LINES_LOST) && strlen(big)>4 && !strcmp(big+strlen(big)-4,YONDER_DIAG_END "\n"),
              "keep: 512 ordinary facts and the head's together lose nothing, and the record ends with `end`");
        free(big); yonder_diag_free(d);
    }

    /* THE NAME IS CLAIMED: another yonder's record at the next number, a
     * page's bytes left from before at the one after, and a folder at the
     * one after that are each passed over; the record and what is kept
     * beside it take the first number all of whose names are fresh, and
     * touch nothing of the others'. */
    keep_begin(true);
    uint32_t next=s_diag.next;
    char other[64], left[64], folder[64];
    snprintf(other,sizeof(other),"/tmp/kept/fixture.test-%04u.txt",(unsigned)next);
    snprintf(left,sizeof(left),"/tmp/kept/fixture.test-%04u.html",(unsigned)next+1);
    snprintf(folder,sizeof(folder),"/tmp/kept/fixture.test-%04u.d",(unsigned)next+2);
    int64_t fh=os64_open(other,"w"); os64_write((int32_t)fh,"address: another yonder's\n",26); os64_close((int32_t)fh);
    fh=os64_open(left,"w"); os64_write((int32_t)fh,"<p>left from before",19); os64_close((int32_t)fh);
    os64_mkdir(folder);
    loop_page("<p id=out>x</p><script src=c.js></script>");
    check(loop_settle() && script_land("/c.js","1") && loop_settle() && g.page.tree!=NULL,
        "claim: the page arrives");
    check(yonder_diag_seq(g.page.diag)==next+3 && s_diag.next==next+4,"claim: the record takes the first fresh number");
    snprintf(path,sizeof(path),"/tmp/kept/fixture.test-%04u.d/index",(unsigned)next+3);
    char html_path[64]; snprintf(html_path,sizeof(html_path),"/tmp/kept/fixture.test-%04u.html",(unsigned)next+3);
    check(!strcmp(fake_text(other),"address: another yonder's\n") && !strcmp(fake_text(left),"<p>left from before") &&
        fake_is_dir(folder) && fake_text(path)!=NULL && fake_text(html_path)!=NULL,
        "claim: the others' files are untouched, and the kept bytes and index are under the claimed name");
    loop_drop();
    /* A page that fetched nothing to keep leaves no folder behind. */
    loop_page("<p>nothing to fetch</p>");
    seq=yonder_diag_seq(g.stream.diag);
    check(loop_settle() && g.page.tree!=NULL,"claim: a page with nothing to fetch arrives");
    loop_drop();
    snprintf(path,sizeof(path),"/tmp/kept/fixture.test-%04u.d",(unsigned)seq);
    snprintf(html_path,sizeof(html_path),"/tmp/kept/fixture.test-%04u.html",(unsigned)seq);
    check(!fake_is_dir(path) && fake_text(html_path)!=NULL,"claim: its empty folder goes, its bytes stay");
    keep_end();

    /* A NAME BESIDE ANOTHER'S KEPT FILES IS NOT FRESH, kept or not: with
     * keep_pages off, a number whose page bytes or folder are there is
     * passed over too, so a record is never paired with another load's kept
     * page. The claimed file holds only `verdict: unwritten` and the
     * claim's nonce until the record is written. */
    keep_begin(false);
    next=s_diag.next;
    snprintf(left,sizeof(left),"/tmp/kept/fixture.test-%04u.html",(unsigned)next);
    snprintf(folder,sizeof(folder),"/tmp/kept/fixture.test-%04u.d",(unsigned)next+1);
    fh=os64_open(left,"w"); os64_write((int32_t)fh,"<p>another's",12); os64_close((int32_t)fh);
    os64_mkdir(folder);
    loop_page("<p>revisit</p>");
    snprintf(path,sizeof(path),"/tmp/kept/fixture.test-%04u.txt",(unsigned)next+2);
    check(yonder_diag_seq(g.stream.diag)==next+2 && fake_text(path)!=NULL &&
          has(fake_text(path),"\n" YONDER_DIAG_UNWRITTEN " ") && !has(fake_text(path),"verdict: clean"),
          "claim, keep off: a number beside another's kept bytes or folder is passed over, and the claim reads unwritten");
    check(loop_settle() && g.page.tree!=NULL,"claim, keep off: the page arrives");
    loop_drop();
    check(has(fake_text(path),"\nverdict: clean\n") && !strcmp(fake_text(left),"<p>another's") &&
          fake_is_dir(folder),"claim, keep off: the record replaces its placeholder, and touches nothing of theirs");
    keep_end();

    /* THE NAME IS PROVED OURS BY THE CLAIM'S NONCE: a claimed file removed
     * and claimed again by another yonder (its placeholder, its nonce) is
     * not written over, at arrival or at departure. */
    keep_begin(false);
    loop_page("<p>reclaimed</p>");
    {
        char name[64], theirs[160]; yonder_diag_file_name(g.stream.diag,name,sizeof(name));
        snprintf(path,sizeof(path),"/tmp/kept/%s",name);
        snprintf(theirs,sizeof(theirs),"address: http://other.test/\n" YONDER_DIAG_UNWRITTEN " 0123456789abcdef\n");
        check(os64_unlink(path)==0,"claim: its placeholder is removed from under it");
        fh=os64_open(path,"x"); os64_write((int32_t)fh,theirs,strlen(theirs)); os64_close((int32_t)fh);
        check(loop_settle() && g.page.tree!=NULL,"claim: the page arrives under a name another claimed");
        loop_drop();
        check(fake_text(path)!=NULL && !strcmp(fake_text(path),theirs),
              "claim: a name another yonder claimed after ours went is not written over");
    }
    keep_end();

    /* EVERY NAME TAKEN: with each of the claim's numbers holding another's
     * record, the load has no claim, and nothing of it is written, neither
     * over one of theirs nor beside it; when the record would have been
     * written, the log and the status line say so. */
    for(int keep=0;keep<2;keep++) {
        keep_begin(keep==1);
        next=s_diag.next;
        for(unsigned i=0;i<YONDER_DIAG_CLAIM_TRIES;i++) {
            snprintf(other,sizeof(other),"/tmp/kept/fixture.test-%04u.txt",(unsigned)next+i);
            fh=os64_open(other,"w"); os64_write((int32_t)fh,"address: another's\n",19); os64_close((int32_t)fh);
        }
        audit_lines[0]='\0'; audit_keep=true;
        loop_page("<p>taken</p>");
        check(yonder_diag_claimed(g.stream.diag)==NULL && s_diag.next==next+1,
              keep ? "claim, keeping: every name taken, the load has no claim"
                   : "claim: every name taken, the load has no claim");
        check(loop_settle() && g.page.tree!=NULL,keep ? "claim, keeping: the page arrives" : "claim: the page arrives");
        audit_keep=false;
        char said[128]; snprintf(said,sizeof(said),"No page file for this page: none of 64 names from %u on",(unsigned)next);
        check(s_diag.claim_said && has(audit_lines,said) && has(g.status_text,said),
              keep ? "claim, keeping: when the record would be written, the log and the status line say it is not"
                   : "claim: when the record would be written, the log and the status line say it is not");
        loop_drop();
        unsigned untouched=0;
        for(unsigned i=0;i<YONDER_DIAG_CLAIM_TRIES;i++) {
            snprintf(other,sizeof(other),"/tmp/kept/fixture.test-%04u.txt",(unsigned)next+i);
            untouched+=fake_text(other)!=NULL && !strcmp(fake_text(other),"address: another's\n");
        }
        check(untouched==YONDER_DIAG_CLAIM_TRIES && fake_count(".txt")==YONDER_DIAG_CLAIM_TRIES &&
              fake_count(".html")==0 && fake_count("/index")==0 && !fake_temp_left(),
              keep ? "claim, keeping: nothing of the page is written, and no record of theirs is touched"
                   : "claim: nothing of the page is written, and no record of theirs is touched");
        keep_end();
    }

    /* A PLACEHOLDER THAT IS NOT WHOLE IS NO CLAIM: with no memory for its
     * text, or a write or a close of it that fails, its file is removed
     * and its number passed over, and the claim is made on the next. */
    {
        static const char *const kHow[]={"no memory for the placeholder","a write of the placeholder that fails",
                                         "a close of the placeholder that fails"};
        for(int how=0;how<3;how++) {
            keep_begin(false);
            char passed[64], taken[64];
            snprintf(passed,sizeof(passed),"/tmp/kept/p.test-%04u.txt",1u);
            snprintf(taken,sizeof(taken),"/tmp/kept/p.test-%04u.txt",2u);
            yonder_diag_t *d=yonder_diag_new("http://p.test/",1,0);
            if(how==0) fail_at=attempts+1;
            if(how==1) fake_fail_write_at=passed;
            if(how==2) fake_fail_close_at=passed;
            int64_t got=d!=NULL ? yonder_diag_claim(d,"/tmp/kept",false) : -9;
            fail_at=0; fake_fail_write_at=fake_fail_close_at=NULL;
            char what[160]; snprintf(what,sizeof(what),"claim: %s is no claim; its file goes and the next number is claimed",
                                     kHow[how]);
            check(got==0 && yonder_diag_seq(d)==2 && fake_text(passed)==NULL &&
                  has(fake_text(taken),"\n" YONDER_DIAG_UNWRITTEN " ") && yonder_diag_write(yonder_diag_claimed(d))==0 &&
                  has(fake_text(taken),"\nverdict: clean\n"),what);
            yonder_diag_free(d);
            keep_end();
        }
    }

    /* THE INPUTS A REPLAY READS THE PAGE WITH (REPLAY.md § 3), written at
     * the claim: the agent, the clock and its zone, the window; and the
     * cookies a script could read, at the head. A setting changed before the record is written
     * makes the record one no replay can read the page again by. */
    keep_begin(true);
    loop_page("<p>inputs</p>");
    check(loop_settle() && g.page.tree!=NULL,"inputs: the page arrives");
    {
        char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        uint32_t zoom=g.zoom;
        g.zoom=zoom+100;
        loop_drop();
        g.zoom=zoom;
        const char *text=diag_file("/tmp/kept",name);
        check(has(text,"\nagent: ") && has(text,"\ncookies: session=abc; theme=dark\n") &&
              has(text,"\nclock: 1700000000370\n") && has(text,"\ntz: ") && has(text,"\nzoom: 1000\n"),
              "inputs: the agent, the script-visible cookies, the clock and its zone are on the record");
        check(has(text,"\n" YONDER_DIAG_UNJUDGED "settings changed during the load\n"),
              "inputs: a setting changed during the load marks the record unjudged");
    }
    keep_end();
    /* Each other change to the window during the load marks it too (the
     * scripts switch, the agent, and a page laid out in another view than
     * its claim's), and a load in an unchanged window is not marked; nor
     * is a view that moves after the page arrived, as it does when
     * yonder's own bars come and go, which a replay reproduces. */
    for(int change=0;change<5;change++) {
        keep_begin(true);
        int32_t width=g.view.bounds.w;
        loop_page("<p>window</p>");
        /* A claim made in another view than the one the page is then laid
         * out in: only the arrival can see it, since the departure finds
         * the window as the page was laid out. */
        if(change==4) {
            check(g.stream.diag!=NULL,"inputs: the page has not arrived when its claim's view is set");
            yonder_diag_head_fact(g.stream.diag,"laid out at","1x1 px");
        }
        check(loop_settle() && g.page.tree!=NULL,"inputs: the page arrives in its window");
        char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        bool scripts=g.scripts_on; const char *agent=g.way.agent;
        if(change==1) g.scripts_on=!scripts;
        if(change==2) g.way.agent="another/1";
        if(change==3) g.view.bounds.w+=40;
        loop_drop();
        g.scripts_on=scripts; g.way.agent=agent; g.view.bounds.w=width;
        const char *text=diag_file("/tmp/kept",name);
        static const char *const kChange[]={"nothing changed","the scripts switch","the agent",
                                            "the view moving after the arrival","a claim in another view"};
        char what[96]; snprintf(what,sizeof(what),"inputs: %s %s the record unjudged",kChange[change],
                                change==0 || change==3 ? "leaves" : "marks");
        check(text!=NULL && has(text,"\n" YONDER_DIAG_UNJUDGED "settings changed during the load\n")==
              (change!=0 && change!=3),what);
        keep_end();
    }
    /* Cookies the jar cannot hand over whole are not written cut short:
     * the record says it lost lines. */
    keep_begin(true);
    fake_cookie_short=true;
    loop_page("<p>cookies</p>");
    check(loop_settle() && g.page.tree!=NULL,"inputs: a page whose cookies do not fit arrives");
    {
        char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        loop_drop();
        fake_cookie_short=false;
        const char *text=diag_file("/tmp/kept",name);
        check(text!=NULL && !has(text,"\ncookies: ") && has(text,"\n" YONDER_DIAG_LINES_LOST "\n"),
              "inputs: cookies that do not fit are not written, and the record says it lost lines");
    }
    keep_end();
    /* THE COOKIES ARE THE FINAL ADDRESS'S, TAKEN AT THE HEAD (REPLAY.md
     * § 3): a load asked for in the clear and redirected to https carries
     * the Secure cookie its page's script reads there; a response's own
     * Set-Cookie, heard after the claim and before the head, is on it; and
     * a cookie the page's first script sets is not, since the record says
     * what the jar held at the head. */
    for(int how=0;how<3;how++) {
        static const char *const kWant[]={"\ncookies: secure=1; plain=1\n","\ncookies: heard=1\n","\ncookies: first=1\n"};
        static const char *const kWhat[]={
            "inputs: a load redirected from http to https records the cookies its page reads there, Secure ones too",
            "inputs: a cookie the response itself sets is on the record",
            "inputs: a cookie the page's first script sets is not on the record"};
        keep_begin(false);
        fake_cookie_heard=false; fake_cookie_set[0]='\0';
        fake_cookie_answer=how==0 ? cookie_jar_secure : how==1 ? cookie_jar_heard : cookie_jar_scripted;
        stream_window(); g.scripts_on=true; g.script_ms=5000; now_ms=100;
        start_trip("http://fixture.test/p",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
        if(how==1) fake_cookie_heard=true;
        way_head_t h=stream_head("text/html","",false);
        if(how==0) { os64_strcopy(h.url,sizeof(h.url),"https://fixture.test/final"); h.encrypted=true; }
        yonder_mail_post_head(g.stream.mail,&h);
        char *html=pad_page(how==2 ? "<p>c</p><script>document.cookie='late=1'</script>" : "<p>c</p>");
        stream_post_body((const uint8_t *)html,strlen(html),YONDER_STREAM_CHUNK);
        stream_post_verdict(true,OS64_FETCH_OK,"");
        free(html);
        check(loop_settle() && g.page.tree!=NULL,"inputs: a page whose cookies the jar answers by address arrives");
        char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        loop_drop();
        fake_cookie_answer=NULL;
        check(has(diag_file("/tmp/kept",name),kWant[how]) && (how!=2 || has(fake_cookie_set,"late=1")),kWhat[how]);
        keep_end();
    }
    /* THE CLAIM IS PROVED AGAIN BEFORE A WRITE: a claimed record removed
     * while its page is up is not written again under a name that may be
     * anyone's now. */
    keep_begin(false);
    loop_page("<p>gone</p>");
    check(loop_settle() && g.page.tree!=NULL,"claim: the page arrives");
    {
        char name[64], path[128]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        snprintf(path,sizeof(path),"/tmp/kept/%s",name);
        check(os64_unlink(path)==0,"claim: its record is removed from under it");
        loop_drop();
        check(fake_text(path)==NULL,"claim: a record whose file is gone is not written again");
    }
    keep_end();

    /* The setting off: the page files are written and nothing is kept. */
    keep_begin(false);
    loop_page("<p>plain</p>");
    check(loop_settle() && g.page.tree!=NULL,"keep off: the page arrives");
    loop_drop();
    check(fake_count(".txt")==1 && fake_count(".html")==0 && fake_count("/index")==0 && nfake_dirs==2,
        "keep off: the page file is written and nothing beside it");
    keep_end();
}

/* The font job whose address ends with `tail` answered: an SFNT of
 * `bytes`, or no file and `why`. False when no such job is out. */
static bool font_land(const char *tail, const char *bytes, const char *why) {
    for(int i=0;i<nfont_jobs;i++) {
        yonder_webfont_job_t *job=font_jobs[i].job;
        size_t a=strlen(job->url),b=strlen(tail);
        if(a<b || strcmp(job->url+a-b,tail)!=0) continue;
        os64_work_id_t id=font_jobs[i].id;
        font_jobs[i]=font_jobs[--nfont_jobs];
        yonder_webfont_t *got=os64_calloc(1,sizeof(*got));
        if(bytes!=NULL) {
            got->ok=true; got->len=got->got=strlen(bytes); got->sfnt=os64_malloc(got->len);
            memcpy(got->sfnt,bytes,got->len); got->container=YONDER_FONT_SFNT; got->fetched=true;
        } else {
            os64_strcopy(got->why,sizeof(got->why),why);
        }
        reaped(id,job,got);
        return true;
    }
    return false;
}
/* The font a page's first text box was drawn in: the identity of its
 * first glyph's face, 0 for none. */
static uint64_t first_text_font(void) {
    for(const flow_box_t *b=g.page.tree!=NULL ? flow_root(g.page.tree) : NULL;b!=NULL;) {
        if(b->kind==FLOW_BOX_TEXT && b->run!=NULL) {
            os64_text_run_view_t v;
            if(os64_text_run_view(b->run,&v)==OS64_FONT_OK && v.glyph_count>0) return v.glyphs[0].font_identity;
        }
        if(b->first) { b=b->first; continue; }
        while(b!=NULL && b->next==NULL) b=b->parent;
        b=b!=NULL ? b->next : NULL;
    }
    return 0;
}
static int32_t web_file(const char *url) {
    for(int32_t i=0;i<s_web.nfiles;i++) if(!strcmp(s_web.files[i].url,url)) return i;
    return -1;
}
/* A page with web fonts, through yonder's resolver, on a window with workers. */
static void web_page_scripted(const char *html, bool scripts) {
    probe_web=true;
    probe_page(html,scripts);
    g.page.diag=diag_new_claimed(g.page.way.url,1);
    check(g.page.diag!=NULL && page_lay_out(&g.page,800,600,g.zoom,&g.page),"web fonts: the page laid out on its record");
    g.pool=(os64_work_pool_t *)&pool_fake; pool_open=true;
    webfonts_feed(true);
}
static void web_page(const char *html) { web_page_scripted(html,false); }
static void web_drop(void) {
    webfonts_leave();
    pool_open=false; g.pool=NULL;
    probe_drop();
    probe_web=false;
    while(nfont_jobs>0) os64_free(font_jobs[--nfont_jobs].job);
}
static const os64_html_node_t *whole_find(const os64_html_node_t *n, const char *id) {
    for(;n!=NULL;n=n->next) {
        const os64_html_attr_t *a=n->kind==OS64_HTML_ELEMENT?os64_html_attr(n,"id"):NULL;
        if(a!=NULL && a->value!=NULL && strcmp(a->value,id)==0) return n;
        const os64_html_node_t *in=whole_find(n->first_child,id);
        if(in!=NULL) return in;
    }
    return NULL;
}

/* WHAT MADE A PAINT WHOLE (YONDER_PERF.md § perf-6): a paint of the whole
 * view is counted by the first whole mark since the last paint, else by
 * the parts when they reach across the view, else by libui's own marks;
 * the layouts for the animations that marked it, by why, and the last one
 * named; all of it one record line, and on each reading. */
static void whole_view_cases(void) {
    fake_reset();
    memset(&s_diag,0,sizeof(s_diag)); s_diag.next=1;
    snprintf(saved_diagnostics,sizeof(saved_diagnostics),"/tmp/diag");
    check(diag_dir_open()==NULL,"whole views: the record has a directory");
    web_page("<div id=x class=' a  b'>t</div><p id=y>u</p>");
    static char rec[8192];
    os64_gui_rect_t was_view=g.view.bounds;
    int32_t was_sx=g.sx, was_sy=g.sy;
    g.view.bounds=(os64_gui_rect_t){0,40,800,560};
    g.sx=0; g.sy=100;
    os64_gui_rect_t view=g.view.bounds, small={10,50,20,20};

    /* The first whole mark since the paint names it; the paint takes it. */
    s_whole_by=WHOLE_PARTS; g.ui.any_dirty=false;
    mark_whole(WHOLE_SCRIPT,false);
    mark_whole(WHOLE_SCROLL,true);
    check(s_whole_by==WHOLE_SCRIPT && g.ui.any_dirty,"whole views: the first whole mark since the paint names it");
    cost_reset();
    paint_counted(&view,1,WHOLE_SCRIPT,(os64_gui_rect_t){0,0,0,0});
    paint_counted(&view,1,WHOLE_PARTS,view);
    paint_counted(&view,1,WHOLE_PARTS,small);
    paint_counted(&view,1,WHOLE_PARTS,(os64_gui_rect_t){0,0,0,0});
    paint_counted(&small,1,WHOLE_SCROLL,small);
    paint_counted(&(os64_gui_rect_t){0,0,800,600},1,WHOLE_PARTS,(os64_gui_rect_t){-5,0,900,700});
    check(s_cost.paints==6 && s_cost.whole==5 && s_cost.whole_by[WHOLE_SCRIPT]==1 &&
          s_cost.whole_by[WHOLE_PARTS]==2 && s_cost.whole_by[WHOLE_TOOLKIT]==2 && s_cost.whole_by[WHOLE_SCROLL]==0 &&
          s_cost.pixels==5ull*800*560+400,
          "whole views: a whole paint is its mark's, the parts' when they span the view, the toolkit's when not; "
          "a part of the view is no whole view");
    /* Two parts in opposite corners: their box is the view, and they are
       still two small paints, no whole view, each counted. */
    paint_counted((os64_gui_rect_t[]){{0,40,10,10},{790,590,10,10}},2,WHOLE_PARTS,view);
    /* A list with one rectangle that is the view is a whole view, once. */
    paint_counted((os64_gui_rect_t[]){{0,40,10,10},{0,40,800,560}},2,WHOLE_PARTS,view);
    check(s_cost.paints==8 && s_cost.whole==6 && s_cost.whole_by[WHOLE_PARTS]==3 &&
          s_cost.pixels==6ull*800*560+400+300,
          "whole views: parts far apart are small paints, however far their box reaches; "
          "one rectangle that is the view makes the paint whole once");

    /* The parts reach what they mark, in the window, together. */
    s_parts_reach=(os64_gui_rect_t){0,0,0,0};
    mark_part((os64_gui_rect_t){0,100,10,10});
    mark_part((os64_gui_rect_t){790,650,10,10});
    check(s_parts_reach.x==0 && s_parts_reach.y==40 && s_parts_reach.w==800 && s_parts_reach.h==560,
          "whole views: the parts' reach is their marks' union, in the window");
    s_parts_reach=(os64_gui_rect_t){0,0,0,0};
    /* One part as big as the view (body's starfield under an html with a
     * background of its own) is a whole view of its own, and no reach. */
    s_whole_by=WHOLE_PARTS; g.ui.any_dirty=false;
    mark_part((os64_gui_rect_t){g.sx,g.sy,800,560});
    check(s_whole_by==WHOLE_BIG_PART && s_parts_reach.w==0 && os64_ui_dirty_covers(&g.ui,view),
          "whole views: a part as big as the view is said as one");
    s_whole_by=WHOLE_PARTS; g.ui.any_dirty=false;
    mark_part((os64_gui_rect_t){g.sx,g.sy,800,559});
    check(s_whole_by==WHOLE_PARTS && s_parts_reach.h==559,
          "whole views: a part a row short of the view is a part");
    s_parts_reach=(os64_gui_rect_t){0,0,0,0};

    /* What refused, named as a selector would: tag, id, every class; the
     * element holding a text; the property a layout places. */
    const os64_html_node_t *x=whole_find(page_doc(&g.page)->document,"x");
    char what[96];
    flow_parts_said_t said={FLOW_PARTS_MOVED,x,-1};
    parts_what(what,sizeof(what),&said);
    bool named=strcmp(what,"div#x.a.b")==0;
    said.node=x!=NULL?x->first_child:NULL;
    parts_what(what,sizeof(what),&said);
    named=named && strcmp(what,"text in div#x.a.b")==0;
    said=(flow_parts_said_t){FLOW_PARTS_PLACED,x,GARB_Z_INDEX};
    parts_what(what,sizeof(what),&said);
    named=named && strcmp(what,"div#x.a.b, z-index")==0;
    said=(flow_parts_said_t){FLOW_PARTS_PLACED,NULL,GARB_Z_INDEX};
    parts_what(what,sizeof(what),&said);
    named=named && strcmp(what,"z-index")==0;
    said=(flow_parts_said_t){FLOW_PARTS_DEEP,NULL,-1};
    parts_what(what,sizeof(what),&said);
    named=named && what[0]=='\0';
    char tiny[6];
    said=(flow_parts_said_t){FLOW_PARTS_PLACED,x,GARB_Z_INDEX};
    parts_what(tiny,sizeof(tiny),&said);
    named=named && strcmp(tiny,"div#x")==0;
    check(x!=NULL && named,"whole views: what refused is named as a selector would, cut to its room");

    /* The record line, and on the reading. */
    cost_reset();
    s_cost.paints=10; s_cost.whole=4;
    s_cost.whole_by[WHOLE_PARTS]=1; s_cost.whole_by[WHOLE_REFUSED]=3;
    s_cost.refused[FLOW_PARTS_MOVED]=2; s_cost.refused[REFUSED_VIEW]=1;
    s_cost.refused_last=FLOW_PARTS_MOVED;
    snprintf(s_cost.refused_what,sizeof(s_cost.refused_what),"div#w");
    s_cost.rects=13; s_cost.rects_most=3;
    g.ui.damage_collapses=s_cost.collapses_from+2;
    diag_paint(&g.page);
    yonder_diag_render(g.page.diag,rec,sizeof(rec));
    static const char kLine[]="whole views: 4 of 10 paints: 1 parts that together covered the view, 3 layouts for "
        "animations that refused their parts; the dirty list: 13 rectangles, 3 at most in a paint, 2 full lists "
        "made one; layouts for animations that marked the whole view, by why: 2 a box "
        "moved, 1 the view moved with it; the last: a box moved, div#w";
    char want[400];
    snprintf(want,sizeof(want),"\n%s\n",kLine);
    check(strstr(rec,want)!=NULL,"whole views: by what marked them, and the layouts by why, the last named");
    diag_paint_reading(&g.page,7);
    yonder_diag_render(g.page.diag,rec,sizeof(rec));
    snprintf(want,sizeof(want)," | %s\n",kLine);
    check(strstr(rec,"\npaint reading 7: at y 100: ")!=NULL && strstr(rec,want)!=NULL,
          "whole views: a reading carries them after its stages");
    /* Whole views with no layout among them say no reasons; none at all
     * leaves no line. */
    cost_reset();
    s_cost.paints=3; s_cost.whole=2; s_cost.whole_by[WHOLE_SCROLL]=2;
    diag_paint(&g.page);
    yonder_diag_render(g.page.diag,rec,sizeof(rec));
    check(strstr(rec,"\nwhole views: 2 of 3 paints: 2 scrolls; the dirty list: 0 rectangles, 0 at most in a "
                     "paint, 0 full lists made one\n")!=NULL,
          "whole views: none from a layout for animations says no reasons");
    cost_reset();
    s_cost.paints=3;
    diag_paint(&g.page);
    yonder_diag_render(g.page.diag,rec,sizeof(rec));
    check(strstr(rec,"\nwhole views:")==NULL,"whole views: no whole view, no line");

    g.view.bounds=was_view; g.sx=was_sx; g.sy=was_sy;
    s_whole_by=WHOLE_PARTS;
    web_drop();
    saved_diagnostics[0]='\0';
    memset(&s_diag,0,sizeof(s_diag)); s_diag.next=1;
}

/* The paint counts on the record (YONDER_PERF.md § Measure first): what
 * nothing painted says, a paint's stages in milliseconds at the counter's
 * rate, and `d` writing the record on asking and starting them again. */
static void paint_cost_cases(void) {
    fake_reset();
    memset(&s_diag,0,sizeof(s_diag)); s_diag.next=1;
    snprintf(saved_diagnostics,sizeof(saved_diagnostics),"/tmp/diag");
    check(diag_dir_open()==NULL,"paint counts: the record has a directory");
    web_page("<p>hello");
    static char rec[8192];
    cost_reset();
    diag_paint(&g.page);
    yonder_diag_render(g.page.diag,rec,sizeof(rec));
    check(strstr(rec,"\npaint: nothing painted in 0.0 s; 0 ticks, 0 schedules; too short a span to time; 0 layouts, 0 of them for animations, 0 of those repainting only what they changed; ")!=NULL &&
          strstr(rec,"paint stages")==NULL,
          "paint counts: nothing painted says so, and a span too short to time has no stages");
    check(lay_out_page(&g.page,800,600) && s_cost.layouts==1 && s_cost.total[PS_LAYOUTS]>0,
          "paint counts: a layout is counted, and timed");
    cost_reset();
    /* Two paints of 12 ms each in render at a counter of 1000 a microsecond,
     * over a span of 1000 s: the real counter runs on while the test does,
     * and that drift must be too small to move a tenth of a millisecond. */
    s_cost.from_cycles=cycles()-1000000000000ull;
    s_cost.from_us=os64_micros()-1000000000;
    s_cost.paints=2; s_cost.whole=1; s_cost.pixels=3000000; s_cost.ticks=4; s_cost.schedules=5;
    s_cost.layouts=3; s_cost.layouts_motion=2; s_cost.layouts_parts=1;
    s_cost.waited=250000000000ull;   /* a quarter of the span asleep */
    s_cost.total[PS_SCHEDULE]=5000000;
    s_cost.total[PS_RENDER]=24000000; s_cost.most[PS_RENDER]=13000000;
    s_cost.total[PS_VIEW]=20000000; s_cost.total[PS_TEXT]=4000000; s_cost.total[PS_TICKS]=8000000;
    /* Laying is inside the stages that lay (YONDER_PERF.md § perf-3): said
     * on its own, and not taken from the view's rest a second time. */
    s_cost.total[PS_LAID]=2000000;
    diag_paint(&g.page);
    yonder_diag_render(g.page.diag,rec,sizeof(rec));
    check(strstr(rec,"\npaint: 2 paints in 1000.0 s (1 whole views), 3.0 Mpx of the view; 4 ticks, 5 schedules; "
                     "the main loop busy 75%; 3 layouts, 2 of them for animations, 1 of those repainting only what they changed; frames late at most 0 ms, 0 more than 30 ms; the longest wait 0 ms; no paint cache; 0 script tasks: 0 scripts, 0 timers, 0 events; 0 bells taken; the ticker not running; deadlines already past when handed to it: settle 0, "
                     "layouts 0, ticks 0, arrival 0, stream 0, timers 0, pictures 0\n")!=NULL &&
          strstr(rec,", schedule (a call) 1.0 (0.0)")!=NULL &&
          strstr(rec,"paint stages: ms a paint (most): render 12.0 (13.0), view 10.0 (0.0)")!=NULL &&
          strstr(rec,", text 2.0 (0.0)")!=NULL && strstr(rec,", ticks (a tick) 2.0 (0.0)")!=NULL &&
          strstr(rec,", laid 1.0 (0.0), publish")!=NULL &&
          strstr(rec,"; window 2.0, the view's rest 8.0\n")!=NULL,
          "paint counts: each stage in milliseconds a paint, the window and the view's rest beside them");
    /* The paint cache's counts beside them (YONDER_PERF.md § perf-2), which
     * start again with the paint counts while what it holds stays. */
    g.paint_cache=yonder_paint_cache_new(YONDER_PAINT_CACHE_FLOOR);
    paint_cache_begin(g.paint_cache);
    (void)paint_cache_make(g.paint_cache,(const uint8_t *)"k",1,paint_cache_hash((const uint8_t *)"k",1),2000);
    diag_paint(&g.page);
    yonder_diag_render(g.page.diag,rec,sizeof(rec));
    check(strstr(rec,"; the paint cache: 0 blocks found, 1 made, 0 let go for room; pictures worked out without it "
                     "0 times for room, 0 for memory; 2 KiB held in 1 blocks, of 32768 KiB; ")!=NULL,
          "paint counts: what the paint cache found, made, let go and held");
    {
        /* The counts the checks below read are kept across the restart. */
        __typeof__(s_cost) before=s_cost;
        cost_reset();
        check(yonder_paint_cache_counts(g.paint_cache).made==0 && yonder_paint_cache_counts(g.paint_cache).entries==1,
              "paint counts: the cache's counts start again with them, and what it holds stays");
        s_cost=before;
    }
    yonder_paint_cache_free(g.paint_cache);
    g.paint_cache=NULL;
    /* A running ticker says what it did. */
    g.ticker=(yonder_ticker_t *)&ticker_counts;
    ticker_counts=(yonder_ticker_counts_t){.rings=9,.wakes=7,.rang_late_most=3,.overslept_most=2};
    diag_paint(&g.page);
    yonder_diag_render(g.page.diag,rec,sizeof(rec));
    g.ticker=NULL;
    check(strstr(rec,"; 0 bells taken; the ticker rang 9 times, woke 7 times, rang at most 3 ms late, slept at "
                     "most 2 ms past its time; deadlines already past when handed to it: settle 0,")!=NULL,
          "paint counts: the ticker's own counts, while it runs");
    /* A deadline already past when it is handed to the ticker is counted
     * against where it came from. */
    {
        uint64_t settle=g.settle_due;
        now_ms=5000; g.settle_due=4000;
        pictures_schedule();
        check(s_cost.past[PAST_SETTLE]==1 && s_cost.past[PAST_PICTURES]==0,
              "paint counts: a deadline already past is counted against its source");
        g.settle_due=settle; now_ms=0;
    }
    /* `d` in the view writes the record on asking and starts the counts again. */
    {
        os64_gui_event_t ev={0};
        ev.key.ascii='d';
        check(view_key(&ev)==KEY_RECORD,"paint counts: d in the view asks for the record");
        diag_asked();
        char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        const char *file=diag_file("/tmp/diag",name);
        check(has(file,"\nwritten: when asked\n") && has(file,"\npaint reading 1: at y 0: 2 paints in ") &&
              has(file," | ms a paint (most): render 12.0 (13.0)") && s_cost.paints==0 && s_cost.total[PS_RENDER]==0,
              "paint counts: d keeps the counts as a reading, writes the record, and starts them again");
        s_cost.paints=1; s_cost.total[PS_RENDER]=5000000;
        s_cost.from_cycles=cycles()-1000000000000ull; s_cost.from_us=os64_micros()-1000000000;
        diag_asked();
        file=diag_file("/tmp/diag",name);
        check(has(file,"\npaint reading 1: at y 0: 2 paints in ") && has(file,"\npaint reading 2: at y 0: 1 paints in ") &&
              s_cost_readings==2,
              "paint counts: each d adds a reading, and the ones before it stay");
    }
    /* A `d` whose file cannot be written keeps its reading and the counts:
     * the next `d` writes both. */
    {
        char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        s_cost.paints=3; s_cost.total[PS_RENDER]=5000000;
        s_cost.from_cycles=cycles()-1000000000000ull; s_cost.from_us=os64_micros()-1000000000;
        fake_fail_open=true;
        diag_asked();
        fake_fail_open=false;
        check(s_cost_readings==3 && s_cost.paints==3 && has(g.status_rest,"Paint reading 3 kept, but the page file "
              "could not be written in /tmp/diag; the counts go on."),
              "paint counts: a d whose file is not written keeps the counts going and says so");
        diag_asked();
        const char *file=diag_file("/tmp/diag",name);
        check(has(file,"\npaint reading 3: at y 0: 3 paints in ") && has(file,"\npaint reading 4: at y 0: 3 paints in ") &&
              s_cost.paints==0,
              "paint counts: the next d writes the kept reading and its own, then starts again");
        /* No page files to write: d says so and keeps nothing. */
        char dir[sizeof(s_diag.dir)];
        memcpy(dir,s_diag.dir,sizeof(dir));
        s_diag.dir[0]='\0';
        diag_asked();
        check(s_cost_readings==4 && has(g.status_rest,"No page files: set diagnostics in yonder.conf to write them."),
              "paint counts: d with no page files says so, and keeps no reading");
        memcpy(s_diag.dir,dir,sizeof(dir));
    }
    /* Every script task the loop runs is counted by its kind; an interval
     * whose counter stepped back counts as nothing, and the reading says so. */
    {
        cost_reset();
        task_cost("script"); task_cost("timer"); task_cost("timer"); task_cost("click"); task_cost("load");
        check(s_cost.scripts==1 && s_cost.timers==2 && s_cost.events==2,
              "paint counts: a script, a timer and an event are each a task of their kind");
        uint64_t before=s_cost.backward;
        check(cycles_since(cycles()+1000000000000ull)==0 && s_cost.backward==before+1,
              "paint counts: a counter read behind its start is an interval of nothing, counted");
        s_cost.paints=1; s_cost.from_cycles=cycles()-1000000000000ull; s_cost.from_us=os64_micros()-1000000000;
        diag_paint(&g.page);
        yonder_diag_render(g.page.diag,rec,sizeof(rec));
        check(strstr(rec,"; the counter stepped back 1 times (the cores' counters disagree: these times are suspect); ")!=NULL &&
              strstr(rec,"; 5 script tasks: 1 scripts, 2 timers, 2 events;")!=NULL,
              "paint counts: the reading names a counter that stepped back, and the tasks by kind");
        /* A span whose counter stepped back across it has no rate: the line
         * says so, not that nothing was painted. */
        s_cost.from_cycles=cycles()+1000000000000ull;
        diag_paint(&g.page);
        yonder_diag_render(g.page.diag,rec,sizeof(rec));
        check(strstr(rec,"\npaint: 1 paints in 1000.0 s (0 whole views), 0.0 Mpx of the view; 0 ticks, 0 schedules; "
                         "the counter stepped back across the span (the cores' counters disagree): no times; ")!=NULL &&
              strstr(rec,"; 5 script tasks: 1 scripts, 2 timers, 2 events;")!=NULL,
              "paint counts: a span the counter stepped back across is said, and its counts are kept");
        /* Work with nothing painted is still counted on the record, timed. */
        cost_reset();
        task_cost("timer"); s_cost.layouts=2;
        s_cost.from_cycles=cycles()-1000000000000ull; s_cost.from_us=os64_micros()-1000000000;
        diag_paint(&g.page);
        yonder_diag_render(g.page.diag,rec,sizeof(rec));
        check(strstr(rec,"\npaint: nothing painted in 1000.0 s; 0 ticks, 0 schedules; the main loop busy ")!=NULL &&
              strstr(rec,"; 2 layouts, ")!=NULL && strstr(rec,"; 1 script tasks: 0 scripts, 1 timers, 0 events;")!=NULL &&
              strstr(rec,"\npaint stages: ms a paint (most):")!=NULL,
              "paint counts: a span with nothing painted keeps its work and its stages");
    }
    web_drop();
    saved_diagnostics[0]='\0';
    memset(&s_diag,0,sizeof(s_diag)); s_diag.next=1;
    /* The shown page's script tasks are counted by kind: a script, then
     * the timers it set. And a timer already due is the loop's to run at
     * its turn: the ticker is handed the clock's next tick, never the past
     * — or its bell, met among the events, rings again and again. */
    probe_page("<p id=out>x</p><script>setTimeout(function(){document.getElementById('out').textContent='y'},0);"
               "setTimeout(function(){},0)</script>",true);
    cost_reset();
    check(script_turn() && s_cost.scripts==1 && s_cost.timers==0,"paint counts: a script is counted as a script task");
    now_ms+=10;
    check(script_turn() && s_cost.timers==1 && probe_text_is("out","y"),"paint counts: a timer is counted as a timer task");
    pictures_schedule();
    check(ticker_due==now_ms+1 && s_cost.past[PAST_TIMERS]>=1,
          "timer ring: a due timer is handed to the ticker as the next tick, not the past");
    check(script_turn() && s_cost.timers==2,"timer ring: and the loop's turn runs it");
    check(!script_turn() && s_cost.scripts+s_cost.timers==3,"paint counts: a turn with no task counts none");
    probe_drop();
}
/* EVERY HOLDER OF A CASCADE, ACROSS CASCADES (GARB.md § The cost): what a
 * cascade reads lives as long as that cascade, so whatever a tree, the
 * timeline, the web fonts and the record keep of one must be gone or copied
 * before the next takes its place. Animations with calc() in their
 * keyframes run and tick, a face with two sources is fetched (the first
 * fails, the second lands), and the page is cascaded again at each step: a
 * resize, a staged rebuild, a layout refused outright (its new cascade
 * freed, the old tree kept), and one that comes up short (shown partial).
 * ASan is the judge; the checks say each step left a page to show. */
static void cascade_swap_case(void) {
    bool was_run=s_animations, was_reduce=s_reduce_motion;
    s_animations=true; s_reduce_motion=false;
    clock_step=0; geometry_layout_delay=0; now_ms=100;
    static const char kPage[]="<!doctype html><style>body{margin:0}"
        "@font-face{font-family:\"Web Face\";src:url(w.ttf) format(\"truetype\"),url(w2.ttf)}"
        "@keyframes go{from{transform:translateX(calc(10px + 1%))}to{transform:translateX(calc(100px + 5%))}}"
        "@keyframes slide{from{margin-left:calc(1px + 1vw)}to{margin-left:calc(50px + 10vw)}}"
        "@keyframes tint{to{color:rgb(255 0 0);background-color:hsl(120 50% 50%)}}"
        "p{font-family:\"Web Face\", Times New Roman, serif;animation:slide 1s linear infinite,tint 2s infinite}"
        "#t{width:calc(40px + 2vw);height:20px;animation:go 1s linear infinite}"
        "@media (max-width:700px){#t{animation:go 2s linear infinite}p{font-family:Web Face, sans-serif}}"
        "</style><div id=t>x</div><p id=a>hello world</p>";
    web_page_scripted(kPage,false);
    relayout(true);
    check(g.page.timeline!=NULL && g.page.cascade!=NULL,"cascade swaps: the page's animations run");
    int steps=0, swapped=0;
    bool partial=false;
    for(int i=0;i<7;i++) {
        now_ms+=150;
        motion_tick(); motion_painted();
        const garb_cascade_t *was=g.page.cascade;
        g.view.bounds.w=800-i*40;
        if(i==4) geometry_refuse_layout=true;           /* refused: the new cascade goes, the old tree stays */
        if(i==5) s_env.max_arena_bytes=4096;            /* short: a partial tree is shown */
        relayout(true);
        if(i==5) partial=g.page.tree!=NULL && flow_incomplete(g.page.tree);
        geometry_refuse_layout=false; s_env.max_arena_bytes=0;
        now_ms+=50; motion_tick(); motion_painted();
        if(i==1) { (void)os64_html_set_attr(g.page.way.doc,probe_id("t"),"title","y",1); (void)script_rebuild(); }
        if(i==2) { (void)font_land("w.ttf",NULL,"HTTP 404"); pictures_settle(); }
        if(i==3) { (void)font_land("w2.ttf","A",NULL); pictures_settle(); }
        swapped+=g.page.cascade!=was;
        steps+=g.page.tree!=NULL && g.page.cascade!=NULL && g.page.timeline!=NULL;
    }
    relayout(true);
    motion_tick(); motion_painted();
    diag_fonts(&g.page);
    char record[8192]; yonder_diag_render(g.page.diag,record,sizeof(record));
    check(steps==7 && swapped>=5 && partial && g.page.tree!=NULL && !flow_incomplete(g.page.tree) &&
          strstr(record,"verdict")!=NULL,
          "cascade swaps: resizes, a staged rebuild, fonts landing, a refused and a partial layout each leave a page to show");
    web_drop();
    s_animations=was_run; s_reduce_motion=was_reduce;
}

static void webfont_cases(void) {
    /* Serif and mono are Test Serif; sans is Test Sans, a system family a
     * page can name apart from the generic. */
    static const uint8_t serif='S', sans='A';
    os64_font_family_spec_t specs[OS64_FONT_FAMILY_COUNT]={0};
    for(size_t f=0;f<OS64_FONT_FAMILY_COUNT;f++)
        for(size_t st=0;st<OS64_FONT_FAMILY_STYLES;st++)
            specs[f].styles[st]=(os64_font_source_t){OS64_FONT_SOURCE_OUTLINE,
                f==OS64_FONT_FAMILY_SANS ? &sans : &serif,1};
    check(os64_font_family_cache_create(probe_text,specs,&s_faces.families,NULL,NULL)==OS64_FONT_OK,
          "web fonts: the system's families");
    s_faces.text=probe_text;
    char record[4096];
    /* A face wanted, fetched, opened and drawn: its address against the
     * page's base, the text laid out first in the system's face. */
    web_page("<style>@font-face{font-family:Web;src:url(fonts/web.ttf)}p{font-family:Web,Web}</style><p>hello");
    int32_t f=web_file("https://fixture.test/fonts/web.ttf");
    check(s_web.nfiles==1 && f==0 && s_web.files[0].state==WF_WAITING && nfont_jobs==1,
          "web fonts: a face is wanted and asked for");
    uint64_t before=first_text_font();
    check(before!=0,"web fonts: the text is drawn at once, in the fallback");
    check(font_land("web.ttf","A",NULL) && s_web.files[0].state==WF_READY && s_web.moved && s_web.waiting==0,
          "web fonts: the file arrives and opens");
    pictures_settle();
    check(!s_web.moved && first_text_font()!=before && first_text_font()!=0,
          "web fonts: the page is laid out again in the face, named twice and listed once");

    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"MISSING font")==NULL,"web fonts: a family a face serves is not missing");
    diag_fonts(&g.page);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"\nfonts: 1 faces (0 past the most a page keeps), 1 files: 1 open, 0 failed, 0 coming\n")!=NULL &&
          strstr(record,"\nfont face 1: Web 400 normal: https://fixture.test/fonts/web.ttf listed (SFNT, 1 bytes)\n")!=NULL &&
          strstr(record,"\nfont family Web: the page's faces\n")!=NULL,
          "web fonts: the record says each face, its file, and what served the family");
    web_drop();
    /* The file is written again when the last font is in, as when the
     * last picture is: after the layout that font moved. */
    fake_reset();
    memset(&s_diag,0,sizeof(s_diag)); s_diag.next=1;
    snprintf(saved_diagnostics,sizeof(saved_diagnostics),"/tmp/diag");
    check(diag_dir_open()==NULL,"web fonts: the record has a directory");
    web_page("<style>@font-face{font-family:Web;src:url(gone.ttf)}p{font-family:Web}</style><p>hello");
    {
        char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        check(!has(diag_file("/tmp/diag",name),"FAILED font"),"web fonts: no file while a font is out");
        check(font_land("gone.ttf",NULL,"HTTP 404") && g.page.record_owed && !has(diag_file("/tmp/diag",name),"FAILED font"),
              "web fonts: the last font in owes the record until the page is laid out again");
        pictures_settle();
        const char *file=diag_file("/tmp/diag",name);
        check(!g.page.record_owed && has(file,"\nwritten: when its pictures and fonts were in\n") &&
              has(file,"FAILED font-fetch: https://fixture.test/gone.ttf: HTTP 404") &&
              has(file,"\nfont face 1: Web 400 normal: https://fixture.test/gone.ttf failed\n"),
              "web fonts: then the file names the font that failed");
    }
    web_drop();
    /* The same for a font that ARRIVES: the last thing in owes the record
     * whether it opened or failed, and an opened face moves the page, so
     * the record waits for that layout too. */
    fake_reset();
    memset(&s_diag,0,sizeof(s_diag)); s_diag.next=1;
    check(diag_dir_open()==NULL,"web fonts: the record has a directory again");
    web_page("<style>@font-face{font-family:Web;src:url(ok.ttf)}p{font-family:Web}</style><p>hello");
    {
        char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        check(font_land("ok.ttf","A",NULL) && g.page.record_owed &&
              !has(diag_file("/tmp/diag",name),"when its pictures and fonts were in"),
              "web fonts: the last font in, opened, owes the record until the page is laid out again");
        pictures_settle();
        check(!g.page.record_owed &&
              has(diag_file("/tmp/diag",name),"\nwritten: when its pictures and fonts were in\n"),
              "web fonts: then the record is written");
    }
    web_drop();
    /* A source the layout cannot keep is refused silently on the record,
     * and the badge is told after that layout (webfonts_feed). Reached
     * here through memory: the face's address is a length nothing else on
     * the page allocates, so failing that size fails only the copy of it
     * the font table keeps (webfonts_add). */
    {
        char html[1024], src[701];
        memset(src,'k',sizeof(src)-1); src[sizeof(src)-1]='\0';
        snprintf(html,sizeof(html),"<style>@font-face{font-family:Web;src:url(%s.ttf)}p{font-family:Web}</style><p>hello",src);
        g.badge_missing=g.badge_failed=0; g.badge_text[0]=0;
        fail_size=strlen("https://fixture.test/")+strlen(src)+strlen(".ttf")+1;
        web_page(html);
        fail_size=0;
        check(s_web.nfiles==0 && yonder_diag_failed_lines(g.page.diag)==1 &&
              strstr(g.badge_text,"FAILED 1")!=NULL && !g.badge.hidden,
              "web fonts: a source the layout could not keep is told on the badge");
        web_drop();
    }
    /* A face's next source, wanted by the layout after its first failed,
     * is waited for too: the file is not written while it is out. */
    fake_reset();
    check(diag_dir_open()==NULL,"web fonts: next source: the record has a directory");
    web_page("<style>@font-face{font-family:Web;src:url(a.ttf),url(b.ttf)}p{font-family:Web}</style><p>hello");
    {
        char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        check(font_land("a.ttf",NULL,"HTTP 404"),"web fonts: next source: the first fails");
        pictures_settle();
        check(s_web.waiting==1 && g.page.record_owed && !has(diag_file("/tmp/diag",name),"when its pictures and fonts were in"),
              "web fonts: next source: the record waits for the source the layout wanted");
        check(font_land("b.ttf","A",NULL),"web fonts: next source: it arrives");
        pictures_settle();
        const char *file=diag_file("/tmp/diag",name);
        check(!g.page.record_owed && has(file,"\nwritten: when its pictures and fonts were in\n") &&
              has(file,"https://fixture.test/b.ttf listed (SFNT, 1 bytes)\n"),
              "web fonts: next source: then the file says what it came to");
    }
    web_drop();
    /* A font the pool will not take fails as the layout's feed meets it,
     * with no job to come back: the record is owed all the same. */
    fake_reset();
    check(diag_dir_open()==NULL,"web fonts: refused job: the record has a directory");
    font_refuse=true;
    web_page("<style>@font-face{font-family:Web;src:url(r.ttf)}p{font-family:Web}</style><p>hello");
    font_refuse=false;
    {
        char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        check(s_web.waiting==0 && g.page.record_owed && !has(diag_file("/tmp/diag",name),"when its pictures and fonts were in"),
              "web fonts: refused job: the record is owed until the page is laid out again");
        pictures_settle();
        const char *file=diag_file("/tmp/diag",name);
        check(!g.page.record_owed && has(file,"\nwritten: when its pictures and fonts were in\n") &&
              has(file,"FAILED font: https://fixture.test/r.ttf: the workers would not take it"),
              "web fonts: refused job: then the file names it");
    }
    web_drop();
    /* A page arriving with no faces of its own has its families on the
     * record it arrives with: it is not laid out again to learn them. */
    fake_reset();
    check(diag_dir_open()==NULL,"web fonts: arrival: the record has a directory");
    probe_web=true;
    loop_page("<style>p{font-family:Nope,'test serif'}</style><p>hello</p>");
    check(loop_settle() && g.page.tree!=NULL,"web fonts: arrival: the page arrives");
    {
        char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        const char *file=diag_file("/tmp/diag",name);
        check(has(file,"\nwritten: when the page arrived\n") && has(file,"\nfont family Nope: no face; the list begins with ") &&
              has(file,"\nfont family test serif: a system family\n") && has(file,"\nMISSING font Nope"),
              "web fonts: arrival: the families it asked for are on its first write");
    }
    while(nfont_jobs>0) os64_free(font_jobs[--nfont_jobs].job);
    loop_drop();
    probe_web=false;
    fake_reset();
    /* An arriving page's families are its latest layout's: one an
     * earlier layout of it found is not on its first write. */
    check(diag_dir_open()==NULL,"web fonts: arrival, own answers: the record has a directory");
    probe_web=true;
    s_arriving.staged=true; s_arriving.seq=s_diag.next;
    webfonts_served(&s_arriving.families,"Stale",5,WF_SERVED_NONE,"Test Sans");
    loop_page("<style>p{font-family:Nope}</style><p>hello</p>");
    check(loop_settle() && g.page.tree!=NULL,"web fonts: arrival, own answers: the page arrives");
    {
        char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        const char *file=diag_file("/tmp/diag",name);
        check(has(file,"\nfont family Nope: ") && !has(file,"font family Stale"),
              "web fonts: arrival: the families are the arriving page's latest layout's");
    }
    while(nfont_jobs>0) os64_free(font_jobs[--nfont_jobs].job);
    loop_drop();
    probe_web=false;
    fake_reset();
    /* A page arriving with faces draws in none of them until it is shown:
     * a name they answer is theirs on the arrival write, none in the
     * list, and not missing. */
    check(diag_dir_open()==NULL,"web fonts: arrival with faces: the record has a directory");
    probe_web=true;
    loop_page("<style>@font-face{font-family:Web;src:url(w.ttf)}p{font-family:Web,Nope}</style><p>hello</p>");
    check(loop_settle() && g.page.tree!=NULL,"web fonts: arrival with faces: the page arrives");
    {
        char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        const char *file=diag_file("/tmp/diag",name);
        check(has(file,"\nwritten: when the page arrived\n") &&
              has(file,"\nfont family Web: the page's faces, none in the list, which begins with ") &&
              !has(file,"MISSING font Web") && has(file,"\nMISSING font Nope"),
              "web fonts: arrival with faces: a name its faces answer is theirs on its first write");
    }
    while(nfont_jobs>0) os64_free(font_jobs[--nfont_jobs].job);
    loop_drop();
    probe_web=false;
    fake_reset();
    /* A page with faces is owed the write after its first layout on
     * screen, even when that layout asks for no file whose coming would
     * write it; and a face drawn from no source says every one. */
    check(diag_dir_open()==NULL,"web fonts: first layout owed: the record has a directory");
    probe_web=true;
    loop_page("<style>@font-face{font-family:Unused;src:url(u1.ttf),url(u2.ttf)}</style><p>hello</p>");
    check(loop_settle() && g.page.tree!=NULL,"web fonts: first layout owed: the page arrives");
    {
        char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        check(has(diag_file("/tmp/diag",name),"u1.ttf not asked for yet: a page is laid out in its faces once it is shown; "
                                              "http://fixture.test/u2.ttf not asked for yet"),
              "web fonts: on the arrival write, a face drawn from no source says every one");
        pictures_settle();
        const char *file=diag_file("/tmp/diag",name);
        check(g.page.record_owed==false && has(file,"\nwritten: when its pictures and fonts were in\n") &&
              has(file,"u1.ttf never asked for: no text asked for it, or its lists had no place; "
                       "http://fixture.test/u2.ttf never asked for"),
              "web fonts: the first layout on screen writes the record, though it asked for no file");
    }
    while(nfont_jobs>0) os64_free(font_jobs[--nfont_jobs].job);
    loop_drop();
    probe_web=false;
    fake_reset();
    /* On the arrival write, a face whose range meets the page's text is
     * not "not needed", and its source is not asked for yet, not never. */
    check(diag_dir_open()==NULL,"web fonts: arrival census: the record has a directory");
    probe_web=true;
    loop_page("<style>@font-face{font-family:Cjk;src:url(cjk.ttf);unicode-range:U+4E00-9FFF}"
              "p{font-family:Cjk}</style><p>&#x4E2D;</p>");
    check(loop_settle() && g.page.tree!=NULL,"web fonts: arrival census: the page arrives");
    {
        char name[64]; yonder_diag_file_name(g.page.diag,name,sizeof(name));
        const char *file=diag_file("/tmp/diag",name);
        check(has(file,"\nwritten: when the page arrived\n") && !has(file,"not needed") &&
              has(file,"cjk.ttf not asked for yet: a page is laid out in its faces once it is shown\n"),
              "web fonts: arrival census: a face whose range meets the text is not yet asked for");
    }
    while(nfont_jobs>0) os64_free(font_jobs[--nfont_jobs].job);
    loop_drop();
    probe_web=false;
    fake_reset();
    saved_diagnostics[0]='\0';
    memset(&s_diag,0,sizeof(s_diag)); s_diag.next=1;
    /* The departure write is made before the page's fonts are let go, so
     * the file a page leaves names them. */
    fake_reset();
    memset(&s_diag,0,sizeof(s_diag)); s_diag.next=1;
    snprintf(saved_diagnostics,sizeof(saved_diagnostics),"/tmp/diag");
    check(diag_dir_open()==NULL,"web fonts: departure: the record has a directory");
    probe_web=true;
    loop_page("<style>@font-face{font-family:Web;src:url(w.ttf)}p{font-family:Web}</style><p>hello</p>");
    check(loop_settle(),"web fonts: departure: the page arrives");
    pictures_settle();
    check(font_land("w.ttf","A",NULL),"web fonts: departure: its face arrives");
    pictures_settle();
    {
        char first[64]; yonder_diag_file_name(g.page.diag,first,sizeof(first));
        start_trip("http://fixture.test/second",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
        way_head_t h=stream_head("text/html","",false);
        yonder_mail_post_head(g.stream.mail,&h);
        const char *body="<!doctype html><p>second</p>";
        stream_post_body((const uint8_t *)body,strlen(body),YONDER_STREAM_CHUNK);
        stream_post_verdict(true,OS64_FETCH_OK,"");
        check(loop_settle() && g.page.tree!=NULL,"web fonts: departure: the next page arrives");
        const char *file=diag_file("/tmp/diag",first);
        check(has(file,"\nwritten: when the page was left\n") &&
              has(file,"\nfont face 1: Web 400 normal: http://fixture.test/w.ttf listed (SFNT, 1 bytes)\n"),
              "web fonts: the departure write names the fonts of the page it leaves");
        check(s_web.nfiles==0,"web fonts: and the next page starts with none");
    }
    while(nfont_jobs>0) os64_free(font_jobs[--nfont_jobs].job);
    loop_drop();
    probe_web=false;
    fake_reset();
    saved_diagnostics[0]='\0';
    memset(&s_diag,0,sizeof(s_diag)); s_diag.next=1;
    /* A face whose range the text does not meet is not fetched; a family
     * with neither a face nor a system face is missing; the system's own
     * name is not. */
    web_page("<style>@font-face{font-family:Cjk;src:url(cjk.ttf);unicode-range:U+4E00-9FFF}</style>"
             "<p style='font-family:Cjk'>hello</p><p style='font-family:Nope'>x</p>"
             "<p style='font-family:\"test serif\"'>y</p>");
    check(s_web.nfiles==0 && nfont_jobs==0,"web fonts: a range the text does not meet is not fetched");
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"MISSING font Nope")!=NULL && strstr(record,"MISSING font Cjk")==NULL &&
          strstr(record,"MISSING font test")==NULL,
          "web fonts: MISSING font is a family nothing serves");
    diag_fonts(&g.page);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"\nfont face 1: Cjk 400 normal U+4E00-9FFF: not needed: its range is not in the page's text\n")!=NULL &&
          strstr(record,"\nfont family Cjk: the page's faces, none in the list, which begins with Test Sans\n")!=NULL &&
          strstr(record,"\nfont family Nope: no face; the list begins with Test Sans\n")!=NULL &&
          strstr(record,"\nfont family test serif: a system family\n")!=NULL,
          "web fonts: the record says a face not needed, and what drew each family");
    web_drop();
    web_page("<style>@font-face{font-family:Cjk;src:url(cjk.ttf);unicode-range:U+4E00-9FFF}"
             "p{font-family:Cjk}</style><p>\xe4\xb8\xad</p>");
    check(s_web.nfiles==1 && nfont_jobs==1,"web fonts: a range the text meets is fetched");
    web_drop();
    /* The first 256 code points, and a meeting range after one that does
     * not, are met too. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf);unicode-range:U+61-7F}p{font-family:Web}</style><p>hello");
    check(s_web.nfiles==1,"web fonts: a range in the first 256 code points meets");
    web_drop();
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf);unicode-range:U+4E00-9FFF,U+3000-30FF,U+0-FF}"
             "p{font-family:Web}</style><p>hello");
    check(s_web.nfiles==1,"web fonts: a range after ranges that miss meets");
    web_drop();
    /* A face with more ranges than a text font takes is never fetched;
     * one with as many is. */
    for(int over=0;over<2;over++) {
        static char big[80000];
        size_t at=(size_t)snprintf(big,sizeof(big),"<style>@font-face{font-family:Web;src:url(w.ttf);unicode-range:");
        int n=(int)OS64_TEXT_RANGES_MAX+over;
        for(int i=0;i<n;i++) at+=(size_t)snprintf(big+at,sizeof(big)-at,"%sU+%X",i?",":"",(unsigned)(2*i));
        snprintf(big+at,sizeof(big)-at,"}p{font-family:Web}</style><p>b");
        web_page(big);
        check(s_web.nfiles==(over?0:1),over ? "web fonts: past the ranges a text font takes, a face is not fetched"
                                            : "web fonts: as many ranges as a text font takes, a face is fetched");
        if(over) {
            diag_fonts(&g.page);
            yonder_diag_render(g.page.diag,record,sizeof(record));
            check(strstr(record,": passed over: 4097 ranges, past the 4096 a font takes\n")!=NULL,
                  "web fonts: on the record, a face past the ranges says so");
        }
        web_drop();
    }
    /* A list fetches only the faces it has places for: one coming holds
     * its place, and one that fails gives it to the next. */
    {
        char many[2048]; size_t at=0;
        at+=(size_t)snprintf(many+at,sizeof(many)-at,"<style>");
        for(int i=0;i<10;i++) at+=(size_t)snprintf(many+at,sizeof(many)-at,"@font-face{font-family:F%d;src:url(f%d.ttf)}",i,i);
        at+=(size_t)snprintf(many+at,sizeof(many)-at,"p{font-family:");
        for(int i=0;i<10;i++) at+=(size_t)snprintf(many+at,sizeof(many)-at,"%sF%d",i?",":"",i);
        snprintf(many+at,sizeof(many)-at,"}</style><p>hello");
        web_page(many);
        int32_t asked=s_web.nfiles;
        check(asked>=1 && asked<(int32_t)OS64_TEXT_FALLBACK_MAX && web_file("https://fixture.test/f0.ttf")==0,
              "web fonts: the faces a list has no place for are not fetched");
        pictures_settle();
        check(s_web.nfiles==asked,"web fonts: a face coming holds its place at the next layout");
        check(font_land("f0.ttf",NULL,"HTTP 404"),"web fonts: places: the first face fails");
        pictures_settle();
        char next[64]; snprintf(next,sizeof(next),"https://fixture.test/f%d.ttf",(int)asked);
        check(s_web.nfiles==asked+1 && web_file(next)==asked,"web fonts: and the next face takes its place");
        web_drop();
        /* A size that will not open, not for room, gives its place too. */
        web_page(many);
        for(int32_t i=0;i<asked;i++) {
            char tail[24]; snprintf(tail,sizeof(tail),"f%d.ttf",(int)i);
            while(font_land(tail,"A",NULL)) {}
        }
        check(s_web.waiting==0,"web fonts: places: every file asked for arrives");
        font_like_status=OS64_FONT_UNSUPPORTED; font_like_refusals=1;
        pictures_settle();
        font_like_status=OS64_FONT_LIMIT; font_like_refusals=0;
        check(s_web.nfiles==asked+1 && web_file(next)==asked,
              "web fonts: a size that will not open gives its place to the next face");
        diag_fonts(&g.page);
        yonder_diag_render(g.page.diag,record,sizeof(record));
        check(strstr(record,"\nfont face 1: F0 400 normal: https://fixture.test/f0.ttf listed (SFNT, 1 bytes), "
                            "but a size that would not open\n")!=NULL,
              "web fonts: on the record, a face one of whose sizes would not open says so");
        web_drop();
        /* Refused for room, it says that instead. */
        web_page(many);
        for(int32_t i=0;i<asked;i++) {
            char tail[24]; snprintf(tail,sizeof(tail),"f%d.ttf",(int)i);
            while(font_land(tail,"A",NULL)) {}
        }
        font_like_refusals=1;
        pictures_settle();
        font_like_refusals=0;
        diag_fonts(&g.page);
        yonder_diag_render(g.page.diag,record,sizeof(record));
        check(strstr(record,"\nfont face 1: F0 400 normal: https://fixture.test/f0.ttf listed (SFNT, 1 bytes), "
                            "but a size refused for room\n")!=NULL,
              "web fonts: on the record, a face one of whose sizes was refused for room says so");
        web_drop();
    }
    /* A face whose range misses the text takes nothing, even when another
     * rule fetched its file. */
    web_page("<style>@font-face{font-family:Z;src:url(s.ttf)}"
             "@font-face{font-family:Cjk;src:url(s.ttf);unicode-range:U+4E00-9FFF}"
             "p{font-family:Z,Cjk}</style><p>hello");
    check(font_land("s.ttf","A",NULL),"web fonts: shared: the file arrives");
    pictures_settle();
    check(s_web.nhandles==1,"web fonts: a face whose range misses the text is not opened from a shared file");
    diag_fonts(&g.page);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"\nfont face 2: Cjk 400 normal U+4E00-9FFF: not needed: its range is not in the page's text\n")!=NULL,
          "web fonts: on the record, a face whose range misses is not needed, though its file came");
    web_drop();
    /* The workers stopping fails a file out and lays the page out again
     * for the face's next source. */
    web_page("<style>@font-face{font-family:Web;src:url(a.ttf),url(b.ttf)}p{font-family:Web}</style><p>hello");
    s_web.moved=false;
    webfonts_orphaned();
    check(s_web.files[0].state==WF_FAILED && s_web.moved,"web fonts: a file out when the workers stop fails and moves the page");
    web_drop();
    /* MISSING font keeps the whole name: two long ones that begin alike
     * are two. */
    {
        static char page[2048], one[400], two[400];
        memset(one,'n',300); one[300]='1'; one[301]=0;
        memset(two,'n',300); two[300]='2'; two[301]=0;
        snprintf(page,sizeof(page),"<style>@font-face{font-family:Web;src:url(w.ttf)}</style>"
                 "<p style=\"font-family:'%s'\">a</p><p style=\"font-family:'%s'\">b</p>",one,two);
        web_page(page);
        static char rec[8192], want1[420], want2[420];
        yonder_diag_render(g.page.diag,rec,sizeof(rec));
        snprintf(want1,sizeof(want1),"MISSING font %s",one);
        snprintf(want2,sizeof(want2),"MISSING font %s",two);
        check(strstr(rec,want1)!=NULL && strstr(rec,want2)!=NULL,"web fonts: MISSING font keeps a long name whole");
        web_drop();
    }
    /* A range is met by the document's own code points, not their
     * neighbours: text of U+0061 alone meets U+61 and U+0-FFFF, not U+62,
     * U+0-60 or U+62-FFFF. */
    {
        static const struct { const char *range; bool met; } kRanges[] = {
            {"U+61",true},{"U+62",false},{"U+0-60",false},{"U+0-FFFF",true},{"U+62-FFFF",false},
            {"U+40-7F",true},{"U+0-3F,U+61",true},{"U+20-10FFFF",true},{"U+80-10FFFF",false},
        };
        for(size_t r=0;r<sizeof(kRanges)/sizeof(kRanges[0]);r++) {
            char page[512];
            snprintf(page,sizeof(page),"<style>@font-face{font-family:Web;src:url(w.ttf);unicode-range:%s}"
                     "p{font-family:Web}</style><p>a",kRanges[r].range);
            web_page(page);
            char what[96]; snprintf(what,sizeof(what),"web fonts: text of U+0061 %s %s",
                                    kRanges[r].met?"meets":"does not meet",kRanges[r].range);
            check(s_web.nfiles==(kRanges[r].met?1:0),what);
            web_drop();
        }
    }
    /* A source that cannot be interned is passed over for a later source
     * already in hand. */
    web_page_scripted("<style>@font-face{font-family:Z;src:url(shared.ttf)}"
                      "@font-face{font-family:W;src:url(new.ttf),url(shared.ttf)}</style>"
                      "<p id=t style='font-family:Z'>hello</p>"
                      "<script>document.getElementById('t').style.fontFamily='W'</script>",true);
    check(font_land("shared.ttf","A",NULL),"web fonts: intern: the shared file arrives");
    pictures_settle();
    {
        uint64_t shared=first_text_font();
        const char *why;
        while(webfonts_add("https://fixture.test/filler",&why) >= 0) {
            s_web.files[s_web.nfiles-1].state=WF_FAILED; s_web.waiting--;
        }
        os64_js_outcome_t out;
        check(yonder_scripts_step(g.page.scripts,&out) && out.status==OS64_JS_OK && script_rebuild(),
              "web fonts: intern: the text changes family with the table full");
        check(web_file("https://fixture.test/new.ttf")<0 && first_text_font()==shared,
              "web fonts: a source that cannot be interned passes to one already in hand");
        diag_fonts(&g.page);
        char rec[8192];
        yonder_diag_render(g.page.diag,rec,sizeof(rec));
        check(strstr(rec,"FAILED font: https://fixture.test/new.ttf: past the font files a page keeps")!=NULL &&
              strstr(rec,"\nfont face 2: W 400 normal: https://fixture.test/new.ttf failed; "
                         "https://fixture.test/shared.ttf listed (SFNT, 1 bytes)\n")!=NULL &&
              g.page.record_owed && !strcmp(g.badge_text,"FAILED 1"),
              "web fonts: on the record, a source the table could not keep failed, and the badge says so");
    }
    web_drop();
    /* A held file whose reclaiming layout fails is failed and let go, not
     * held for a layout that may not come. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf)}p{font-family:Web}</style><p>hello");
    font_open_refusals=1;
    g.badge_missing=g.badge_failed=0; g.badge_text[0]=0;
    check(font_land("w.ttf","A",NULL) && s_web.files[0].state==WF_HELD && s_web.held_bytes>0,"web fonts: unhold: held");
    {
        os64_html_pin_t pins[512]; size_t n=0;
        while(n<512 && (pins[n]=os64_html_pin(g.page.way.doc))!=0) n++;
        pictures_settle();
        check(n<512 && s_web.files[0].state==WF_FAILED && s_web.held_bytes==0 && s_web.files[0].sfnt==NULL &&
              strstr(g.badge_text,"FAILED")!=NULL,
              "web fonts: a held file whose layout fails is failed and its bytes let go");
        for(size_t i=0;i<n;i++) os64_html_unpin(g.page.way.doc,pins[i]);
    }
    font_open_refusals=0;
    web_drop();
    /* A layout that adds a MISSING font tells the badge as it is shown. */
    web_page_scripted("<style>@font-face{font-family:Web;src:url(w.ttf)}</style><p>a</p>"
                      "<script>var q=document.createElement('p');q.style.fontFamily='Nope';q.textContent='x';"
                      "document.body.appendChild(q)</script>",true);
    check(strstr(g.badge_text,"MISSING")==NULL,"web fonts: badge: nothing missing first");
    {
        os64_js_outcome_t out;
        check(yonder_scripts_step(g.page.scripts,&out) && out.status==OS64_JS_OK,"web fonts: badge: the script adds a family");
        g.badge_missing=g.badge_failed=0; g.badge_text[0]=0;
        badge_follow();
        check(strstr(g.badge_text,"MISSING")==NULL,"web fonts: badge: still nothing missing before the rebuild");
        check(script_rebuild(),"web fonts: badge: the page is rebuilt");
    }
    check(strstr(g.badge_text,"MISSING 1")!=NULL,"web fonts: a layout that adds a MISSING font tells the badge when it is shown");
    web_drop();
    /* The face's next source after one that failed since the layout on
     * screen is not asked for yet: the failed layout asks for nothing, and
     * the next layout will. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf),url(w2.ttf)}p{font-family:Web}</style><p>hello");
    font_open_refusals=1;
    check(font_land("w.ttf","A",NULL) && s_web.files[0].state==WF_HELD,"web fonts: held then failed: held");
    {
        os64_html_pin_t pins[512]; size_t n=0;
        while(n<512 && (pins[n]=os64_html_pin(g.page.way.doc))!=0) n++;
        pictures_settle();
        for(size_t i=0;i<n;i++) os64_html_unpin(g.page.way.doc,pins[i]);
    }
    font_open_refusals=0;
    diag_fonts(&g.page);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(s_web.files[0].state==WF_FAILED &&
          strstr(record,"w.ttf failed; https://fixture.test/w2.ttf not asked for yet: the source before it failed "
                        "since the page was laid out\n")!=NULL,
          "web fonts: on the record, a source after one that failed since the layout is not asked for yet");
    check(lay_out_page(&g.page,800,600) && web_file("https://fixture.test/w2.ttf")>=0,
          "web fonts: held then failed: the next layout asks for the next source");
    web_drop();
    /* A late failure is not undone by an older one after it: the layout
     * stopped at the source that failed since, so the sources past every
     * failure are not asked for yet. */
    web_page("<style>@font-face{font-family:X;src:url(b.ttf)}@font-face{font-family:Y;src:url(a.ttf),url(b.ttf),url(c.ttf)}"
             "</style><p style='font-family:X'>x</p><p style='font-family:Y'>y</p>");
    check(font_land("b.ttf",NULL,"HTTP 404"),"web fonts: sticky: b fails early");
    pictures_settle();
    font_open_refusals=1;
    check(font_land("a.ttf","A",NULL) && s_web.files[web_file("https://fixture.test/a.ttf")].state==WF_HELD,
          "web fonts: sticky: a is held");
    {
        os64_html_pin_t pins[512]; size_t n=0;
        while(n<512 && (pins[n]=os64_html_pin(g.page.way.doc))!=0) n++;
        pictures_settle();
        for(size_t i=0;i<n;i++) os64_html_unpin(g.page.way.doc,pins[i]);
    }
    font_open_refusals=0;
    diag_fonts(&g.page);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"a.ttf failed; https://fixture.test/b.ttf failed; https://fixture.test/c.ttf not asked for yet: "
                        "the source before it failed since the page was laid out\n")!=NULL,
          "web fonts: on the record, a late failure is not undone by an older one after it");
    web_drop();
    /* A composite whose latest rules cannot serve falls back to an
     * earlier one, past the eight a list keeps. */
    {
        char many[4096]; size_t at=0;
        at+=(size_t)snprintf(many+at,sizeof(many)-at,"<style>@font-face{font-family:W;src:url(ok.ttf)}");
        for(int i=0;i<9;i++)
            at+=(size_t)snprintf(many+at,sizeof(many)-at,"@font-face{font-family:W;src:url(c%d.ttf);unicode-range:U+4E00-9FFF}",i);
        snprintf(many+at,sizeof(many)-at,"p{font-family:W}</style><p>hello");
        web_page(many);
        check(s_web.nfiles==1 && web_file("https://fixture.test/ok.ttf")==0,
              "web fonts: past nine later rules that cannot serve, the earliest is wanted");
        web_drop();
    }
    /* With the handle table full, the least lately used goes before the
     * open, so its room is there for the new size. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf)}p{font-family:Web}</style><p>hello");
    check(font_land("w.ttf","A",NULL),"web fonts: lru: the file arrives");
    pictures_settle();
    {
        const garb_font_face_t *faces; bool room;
        check(garb_cascade_font_faces(g.page.cascade,&faces,NULL)==1,"web fonts: lru: one face");
        for(uint32_t px=100;s_web.nhandles<WEBFONT_HANDLES_MAX;px++) (void)webfonts_handle(0,&faces[0],px,&room);
        font_like_refuse_full=true;
        check(webfonts_handle(0,&faces[0],999,&room)!=NULL && s_web.nhandles==WEBFONT_HANDLES_MAX,
              "web fonts: a full table lets its least lately used go before the open");
        font_like_refuse_full=false;
    }
    web_drop();
    /* A layout that fails still asks for the files it wanted. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf)}p{font-family:Web}</style><p>hello");
    {
        const char *why;
        int32_t late=webfonts_add("https://fixture.test/late.ttf",&why);
        os64_html_pin_t pins[512]; size_t n=0;
        while(n<512 && (pins[n]=os64_html_pin(g.page.way.doc))!=0) n++;
        s_web.moved=true;
        pictures_settle();
        check(n<512 && late>=0 && s_web.files[late].state==WF_WAITING,"web fonts: a failed layout still asks for what it queued");
        for(size_t i=0;i<n;i++) os64_html_unpin(g.page.way.doc,pins[i]);
    }
    web_drop();
    web_page_scripted("<style>@font-face{font-family:Web;src:url(w.ttf)}p{font-family:Web}</style>"
                      "<p id=t>hello</p><script>document.getElementById('t').textContent='changed'</script>",true);
    {
        const char *why;
        int32_t late=webfonts_add("https://fixture.test/late.ttf",&why);
        os64_js_outcome_t out;
        check(yonder_scripts_step(g.page.scripts,&out) && out.status==OS64_JS_OK,"web fonts: a script changes the page");
        os64_page_t *model=os64_page_rebuild(page_model(&g.page));
        os64_page_free(g.page.way.model); g.page.way.model=model;
        g.page.model_version=os64_html_version(g.page.way.doc);
        os64_html_pin_t pins[512]; size_t n=0;
        while(n<512 && (pins[n]=os64_html_pin(g.page.way.doc))!=0) n++;
        check(n<512 && !script_rebuild() && late>=0 && s_web.files[late].state==WF_WAITING,
              "web fonts: a failed rebuild still asks for what it queued");
        for(size_t i=0;i<n;i++) os64_html_unpin(g.page.way.doc,pins[i]);
    }
    web_drop();
    /* On the record: a face past its list's places was never asked for,
     * and says why it may not have been. */
    {
        char many[2048]; size_t at=0;
        at+=(size_t)snprintf(many+at,sizeof(many)-at,"<style>");
        for(int i=0;i<10;i++) at+=(size_t)snprintf(many+at,sizeof(many)-at,"@font-face{font-family:F%d;src:url(f%d.ttf)}",i,i);
        snprintf(many+at,sizeof(many)-at,"p{font-family:F0,F1,F2,F3,F4,F5,F6,F7,F8,F9}</style><p>hello");
        web_page(many);
        diag_fonts(&g.page);
        yonder_diag_render(g.page.diag,record,sizeof(record));
        check(strstr(record,"f9.ttf never asked for: no text asked for it, or its lists had no place\n")!=NULL,
              "web fonts: on the record, a face with no place was never asked for");
        web_drop();
    }
    /* On the record, a family of 509 bytes is whole, and so is the rest of
     * its line: with its weight it passes 512 by one, where a buffer of
     * that size would have been written past. */
    {
        static char page[4096], fam[700], rec[16384];
        memset(fam,'x',509); fam[509]=0;
        snprintf(page,sizeof(page),"<style>@font-face{font-family:'%s';src:url(x.ttf);unicode-range:U+0-FF,U+100-1FF}"
                 "p{font-family:'%s'}</style><p>hello",fam,fam);
        web_page(page);
        diag_fonts(&g.page);
        yonder_diag_render(g.page.diag,rec,sizeof(rec));
        static char want[800];
        snprintf(want,sizeof(want),"\nfont face 1: %s 400 normal U+0-FF,U+100-1FF: https://fixture.test/x.ttf ",fam);
        check(strstr(rec,want)!=NULL,"web fonts: on the record, a long family is whole on its line, and the line goes on");
        web_drop();
    }
    /* Two faces of one file: the face no text asked for is not listed
     * because the other's file came. */
    web_page("<style>@font-face{font-family:A;src:url(s.ttf)}@font-face{font-family:B;src:url(s.ttf)}"
             "p{font-family:A}</style><p>hello");
    check(font_land("s.ttf","A",NULL),"web fonts: one file, two faces: it arrives");
    diag_fonts(&g.page);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"\nfont face 1: A 400 normal: https://fixture.test/s.ttf came (SFNT, 1 bytes), not listed since it came\n")!=NULL,
          "web fonts: on the record, a file not yet laid out in is not listed");
    pictures_settle();
    diag_fonts(&g.page);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"\nfont face 1: A 400 normal: https://fixture.test/s.ttf listed (SFNT, 1 bytes)\n")!=NULL &&
          strstr(record,"\nfont face 2: B 400 normal: https://fixture.test/s.ttf came (SFNT, 1 bytes), not listed: "
                        "no text asked for it, or its lists had no place\n")!=NULL,
          "web fonts: on the record, a face is listed by its own use, not by its file's");
    /* The section is written whole: a face line no rule names now goes,
     * and the faces stay before the families. */
    yonder_diag_fact(g.page.diag,"font face 9","a rule that is gone");
    diag_fonts(&g.page);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    {
        const char *face=strstr(record,"\nfont face 1: "), *fam=strstr(record,"\nfont family A: ");
        check(strstr(record,"font face 9")==NULL && face!=NULL && fam!=NULL && face<fam,
              "web fonts: on the record, the faces are the cascade's now, before the families");
    }
    web_drop();
    /* Past the families the record keeps, it says some are not on it. */
    {
        static char page[32768], rec[262144];
        size_t at=0;
        at+=(size_t)snprintf(page+at,sizeof(page)-at,"<style>@font-face{font-family:Web;src:url(w.ttf)}</style>");
        for(int i=0;i<520;i++) at+=(size_t)snprintf(page+at,sizeof(page)-at,"<p style='font-family:N%d'>a</p>",i);
        web_page(page);
        diag_fonts(&g.page);
        yonder_diag_render(g.page.diag,rec,sizeof(rec));
        check(strstr(rec,"; more families than the 512 the record keeps, the rest on no line here\n")!=NULL &&
              strstr(rec,"MISSING font N519")==NULL && strstr(rec,"MISSING font N511")!=NULL,
              "web fonts: on the record, families past the 512 kept are on no line, and the fonts line says so");
        web_drop();
    }
    /* A family line names the font its run's list begins with: a page
     * face the list puts first, not the system's family after it. */
    web_page("<style>@font-face{font-family:A;src:url(a.ttf)}p{font-family:A,Nope}</style><p>hello");
    /* The fixture's face of byte S is Test Serif; the system's sans is Test Sans. */
    check(font_land("a.ttf","S",NULL),"web fonts: list begins: the face arrives");
    pictures_settle();
    diag_fonts(&g.page);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"\nfont family Nope: no face; the list begins with Test Serif\n")!=NULL,
          "web fonts: on the record, a family line names the font its list begins with");
    web_drop();
    /* With memory short anywhere in a layout or a write, the record never
     * claims the 512 were passed, and every family is on it or said not
     * to be. */
    {
        unsigned short_said=0;
        for(size_t cut=1;cut<=400;cut++) {
            web_page("<style>@font-face{font-family:Web;src:url(w.ttf)}</style>"
                     "<p style='font-family:Aa'>a</p><p style='font-family:Bb'>b</p>");
            /* A fresh record, so the lines are this layout's alone. */
            yonder_diag_free(g.page.diag);
            g.page.diag=diag_new_claimed(g.page.way.url,1);
            fail_at=attempts+cut;
            bool laid=lay_out_page(&g.page,800,600);
            diag_fonts(&g.page);
            fail_at=0;
            static char rec[16384];
            yonder_diag_render(g.page.diag,rec,sizeof(rec));
            bool told=strstr(rec,"not kept")!=NULL;
            short_said+=strstr(rec,"families not kept: no memory for them")!=NULL ||
                        strstr(rec,"some lines not kept: no memory to name them")!=NULL;
            bool missing_told=!laid || (strstr(rec,"MISSING font Aa")!=NULL && strstr(rec,"MISSING font Bb")!=NULL) ||
                              strstr(rec,"families not kept: no memory for them, nor a line here")!=NULL ||
                              strstr(rec,"some lines not kept")!=NULL || strstr(rec,"missing names not kept")!=NULL;
            check(strstr(rec,"more families than")==NULL &&
                  ((strstr(rec,"\nfont family Aa: ")!=NULL && strstr(rec,"\nfont family Bb: ")!=NULL) || told) &&
                  missing_told,
                  "web fonts: short of memory, the record never claims the cap and says what it lost");
            web_drop();
        }
        check(short_said>0,"web fonts: the memory sweep reaches the families' own allocations");
    }
    /* A long list whose scratch the resolver had no memory for: if the
     * layout is shown, the record says its families went. */
    {
        static char page[4096];
        size_t at=0;
        at+=(size_t)snprintf(page+at,sizeof(page)-at,"<style>");
        for(int i=0;i<40;i++) at+=(size_t)snprintf(page+at,sizeof(page)-at,"@font-face{font-family:F%d;src:url(f%d.ttf)}",i,i);
        at+=(size_t)snprintf(page+at,sizeof(page)-at,"p{font-family:F0");
        for(int i=1;i<40;i++) at+=(size_t)snprintf(page+at,sizeof(page)-at,",F%d",i);
        snprintf(page+at,sizeof(page)-at,"}</style><p>hello");
        unsigned silent=0, reached=0;
        for(size_t cut=1;cut<=300;cut++) {
            web_page(page);
            fail_at=attempts+cut;
            bool laid=lay_out_page(&g.page,800,600);
            fail_at=0;
            diag_fonts(&g.page);
            static char rec[32768];
            yonder_diag_render(g.page.diag,rec,sizeof(rec));
            bool told=strstr(rec,"families not kept")!=NULL || strstr(rec,"some lines not kept")!=NULL ||
                      strstr(rec,"facts not kept")!=NULL;
            reached+=strstr(rec,"families not kept: no memory for them")!=NULL;
            silent+=laid && strstr(rec,"\nfont family F39: ")==NULL && !told;
            web_drop();
        }
        check(silent==0 && reached>0,"web fonts: a list's scratch short of memory is said on the record");
        /* Every lookup's scratch failing through one layout, which goes on
         * and is shown without the list's answers: the record says so. */
        web_page(page);
        fail_size=40*sizeof(os64_font_family_name_t);
        bool laid=lay_out_page(&g.page,800,600);
        fail_size=0;
        diag_fonts(&g.page);
        static char rec2[32768];
        yonder_diag_render(g.page.diag,rec2,sizeof(rec2));
        check(laid && strstr(rec2,"\nfont family F0: ")==NULL &&
              strstr(rec2,"families not kept: no memory for them, nor a line here")!=NULL,
              "web fonts: a layout shown without a list's answers says its families went");
        web_drop();
    }
    /* The system's family refused for room through a layout that goes on
     * and is shown: its families went, and the record says so. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf)}</style><p style='font-family:Nope'>hello</p>");
    {
        uint32_t zoom=g.zoom;
        g.zoom=zoom*2;
        font_open_refusals=100000;
        bool laid=lay_out_page(&g.page,800,600);
        font_open_refusals=0;
        g.zoom=zoom;
        diag_fonts(&g.page);
        yonder_diag_render(g.page.diag,record,sizeof(record));
        check(laid && strstr(record,"families not kept: no memory for them, nor a line here")!=NULL &&
              strstr(record,"\nfont family Nope: ")==NULL,
              "web fonts: the system's family refused for room, a shown layout says its families went");
    }
    web_drop();
    /* A name with no memory to copy it only flags its layout's table:
     * nothing reaches the record before the layout's tree is shown, and
     * then the fonts line says families went. */
    {
        WebFontFamilies t={0};
        webfonts_served(&t,"First",5,WF_SERVED_NONE,"Test Sans");
        fail_at=attempts+1;
        webfonts_served(&t,"Gone",4,WF_SERVED_NONE,"Test Sans");
        fail_at=0;
        check(t.n==1 && t.short_of_memory && !t.full,
              "web fonts: a family with no memory to keep flags its table, and is not taken for the cap");
        webfonts_families_free(&t);
    }
    /* A page let go before it was shown writes its fonts too: its faces as
     * its rules declare them, none asked for, whatever the page on screen
     * fetched, and the families its layout staged, MISSING lines and all. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf)}p{font-family:Web,Nope}</style><p>hello");
    {
        static Page fresh;
        fresh=g.page;
        memset(&g.page,0,sizeof(g.page));
        /* Its own record, so its MISSING line is the staged one's. */
        yonder_diag_free(fresh.diag);
        fresh.diag=diag_new_claimed("http://fixture.test/fresh",99);
        s_arriving.staged=true; s_arriving.seq=yonder_diag_seq(fresh.diag);
        webfonts_served(&s_arriving.families,"Nope",4,WF_SERVED_NONE,"Test Sans");
        diag_observe(&fresh,"when the page was left");
        static char rec[8192];
        yonder_diag_render(fresh.diag,rec,sizeof(rec));
        check(strstr(rec,"\nfonts: 1 faces (0 past the most a page keeps), 0 files: 0 open, 0 failed, 0 coming\n")!=NULL &&
              strstr(rec,"\nfont face 1: Web 400 normal: https://fixture.test/w.ttf not asked for yet")!=NULL &&
              strstr(rec,"\nfont family Nope: no face; the list begins with Test Sans\n")!=NULL &&
              strstr(rec,"MISSING font Nope (1)")!=NULL,
              "web fonts: a page let go before it was shown writes its own fonts, not the shown page's");
        webfonts_families_free(&s_arriving.families);
        memset(&s_arriving,0,sizeof(s_arriving));
        g.page=fresh;
    }
    web_drop();
    /* A face's range is met against the page's text as it is at the
     * write, after a script changed it. */
    web_page_scripted("<style>@font-face{font-family:Cjk;src:url(cjk.ttf);unicode-range:U+4E00-9FFF}"
                      "p{font-family:Cjk}</style><p id=t>hello</p>"
                      "<script>document.getElementById('t').textContent='\xe4\xb8\xad'</script>",true);
    diag_fonts(&g.page);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"Cjk 400 normal U+4E00-9FFF: not needed")!=NULL,"web fonts: census at the write: not needed first");
    {
        os64_js_outcome_t out;
        check(yonder_scripts_step(g.page.scripts,&out) && out.status==OS64_JS_OK,"web fonts: census at the write: the text changes");
    }
    diag_fonts(&g.page);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"Cjk 400 normal U+4E00-9FFF: not needed")==NULL,
          "web fonts: on the record, a face's range meets the text as it is at the write");
    web_drop();
    /* A layout's answers are its own: what a layout that was never shown
     * found is not the record's at the next. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf)}p{font-family:Web}</style><p>a");
    webfonts_served(&s_web.laying,"Stale",5,WF_SERVED_NONE,"Test Sans");
    check(lay_out_page(&g.page,800,600),"web fonts: own answers: the page is laid out again");
    diag_fonts(&g.page);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"\nfont family Stale: ")==NULL && strstr(record,"\nfont family Web: ")!=NULL,
          "web fonts: on the record, a layout's families are its own");
    web_drop();
    /* A name one run lists a face of is the page's faces, whichever runs
     * came before or after it. */
    {
        static char page[4096];
        size_t at=0;
        at+=(size_t)snprintf(page+at,sizeof(page)-at,"<style>@font-face{font-family:Web;src:url(w.ttf)}");
        for(int i=0;i<10;i++) at+=(size_t)snprintf(page+at,sizeof(page)-at,"@font-face{font-family:A%d;src:url(a%d.ttf)}",i,i);
        at+=(size_t)snprintf(page+at,sizeof(page)-at,"</style><p style='font-family:A0");
        for(int i=1;i<10;i++) at+=(size_t)snprintf(page+at,sizeof(page)-at,",A%d",i);
        at+=(size_t)snprintf(page+at,sizeof(page)-at,",Web'>first</p><p style='font-family:Web'>second</p>"
                             "<p style='font-family:A0");
        for(int i=1;i<10;i++) at+=(size_t)snprintf(page+at,sizeof(page)-at,",A%d",i);
        snprintf(page+at,sizeof(page)-at,",Web;font-size:20px'>third</p>");
        web_page(page);
        for(int round=0;round<4;round++) {
            while(font_land("w.ttf","A",NULL)) {}
            for(int i=0;i<10;i++) { char tail[24]; snprintf(tail,sizeof(tail),"a%d.ttf",i); while(font_land(tail,"A",NULL)) {} }
            pictures_settle();
        }
        diag_fonts(&g.page);
        static char rec[16384];
        yonder_diag_render(g.page.diag,rec,sizeof(rec));
        check(strstr(rec,"\nfont family A9: the page's faces, none in the list")!=NULL,
              "web fonts: best answer: the first run's list has no place for its last names");
        check(strstr(rec,"\nfont family Web: the page's faces\n")!=NULL,
              "web fonts: on the record, a family is the page's faces when any run listed one");
        web_drop();
    }
    /* A face line is whole, however many sources it walks. */
    {
        static char page[16384], rec[32768];
        size_t at=0;
        at+=(size_t)snprintf(page+at,sizeof(page)-at,"<style>@font-face{font-family:Web;src:");
        for(int i=0;i<30;i++) {
            at+=(size_t)snprintf(page+at,sizeof(page)-at,"url(");
            for(int k=0;k<120;k++) page[at++]='a';
            at+=(size_t)snprintf(page+at,sizeof(page)-at,"%d.woff2) format('woff2'),",i);
        }
        snprintf(page+at,sizeof(page)-at,"url(last.ttf)}p{font-family:Web}</style><p>hello");
        web_page(page);
        diag_fonts(&g.page);
        yonder_diag_render(g.page.diag,rec,sizeof(rec));
        check(strstr(rec,"29.woff2; https://fixture.test/last.ttf coming\n")!=NULL,
              "web fonts: on the record, a face line walks every source, however long");
        web_drop();
    }
    /* A layout whose text asks for no font still begins the faces' state
     * afresh: a face listed by the last tree is not listed by this one. */
    web_page_scripted("<style>@font-face{font-family:A;src:url(s.ttf)}p{font-family:A}</style><p>hello</p>"
                      "<script>document.body.style.display='none'</script>",true);
    check(font_land("s.ttf","A",NULL),"web fonts: no lookup: the file arrives");
    pictures_settle();
    diag_fonts(&g.page);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"s.ttf listed (SFNT, 1 bytes)\n")!=NULL,"web fonts: no lookup: the face is listed first");
    {
        os64_js_outcome_t out;
        check(yonder_scripts_step(g.page.scripts,&out) && out.status==OS64_JS_OK && script_rebuild(),
              "web fonts: no lookup: the script hides the text");
    }
    diag_fonts(&g.page);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"s.ttf came (SFNT, 1 bytes), not listed: no text asked for it")!=NULL,
          "web fonts: on the record, a tree whose text asks for no font lists no face");
    web_drop();
    /* MISSING font counts the most lookups one layout made for the name,
     * not one, and not one more each layout. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf)}</style>"
             "<p style='font-family:Nope,Nope'>a</p><p style='font-family:Nope;font-size:20px'>b</p>");
    {
        yonder_diag_render(g.page.diag,record,sizeof(record));
        const char *m=strstr(record,"MISSING font Nope (");
        int first=m!=NULL ? atoi(m+strlen("MISSING font Nope (")) : 0;
        check(lay_out_page(&g.page,800,600),"web fonts: missing count: laid out again");
        yonder_diag_render(g.page.diag,record,sizeof(record));
        m=strstr(record,"MISSING font Nope (");
        int again=m!=NULL ? atoi(m+strlen("MISSING font Nope (")) : 0;
        check(first>=3 && again==first,"web fonts: MISSING font counts one layout's lookups, once");
    }
    web_drop();
    /* Every name of a long list that a face answers is on the record. */
    {
        static char page[8192], rec[32768];
        size_t at=0;
        at+=(size_t)snprintf(page+at,sizeof(page)-at,"<style>");
        for(int i=0;i<40;i++) at+=(size_t)snprintf(page+at,sizeof(page)-at,"@font-face{font-family:F%d;src:url(f%d.ttf)}",i,i);
        at+=(size_t)snprintf(page+at,sizeof(page)-at,"p{font-family:F0");
        for(int i=1;i<40;i++) at+=(size_t)snprintf(page+at,sizeof(page)-at,",F%d",i);
        snprintf(page+at,sizeof(page)-at,"}</style><p>hello");
        web_page(page);
        diag_fonts(&g.page);
        yonder_diag_render(g.page.diag,rec,sizeof(rec));
        check(strstr(rec,"\nfont family F39: the page's faces")!=NULL,
              "web fonts: on the record, a list's names past 32 are there too");
        web_drop();
    }
    /* On the record, a family's whole name: two long ones that begin
     * alike are two lines. */
    {
        static char page[2048], one[400], two[400], want[460];
        memset(one,'m',200); one[200]='1'; one[201]=0;
        memset(two,'m',200); two[200]='2'; two[201]=0;
        snprintf(page,sizeof(page),"<style>@font-face{font-family:Web;src:url(w.ttf)}</style>"
                 "<p style=\"font-family:'%s'\">a</p><p style=\"font-family:'%s'\">b</p>",one,two);
        web_page(page);
        diag_fonts(&g.page);
        static char rec[8192];
        yonder_diag_render(g.page.diag,rec,sizeof(rec));
        snprintf(want,sizeof(want),"\nfont family %s: no face; the list begins with",one);
        bool first=strstr(rec,want)!=NULL;
        snprintf(want,sizeof(want),"\nfont family %s: no face; the list begins with",two);
        check(first && strstr(rec,want)!=NULL,"web fonts: on the record, a family keeps its whole name");
        web_drop();
    }
    /* A name written again counts its file once: the family after the
     * repeats still has a place. */
    web_page("<style>@font-face{font-family:W;src:url(w.ttf)}@font-face{font-family:X;src:url(x.ttf)}"
             "p{font-family:W,W,W,W,W,W,W,W,X}</style><p>hello");
    check(s_web.nfiles==2 && web_file("https://fixture.test/x.ttf")==1,
          "web fonts: a file coming holds one place, however often its name is written");
    web_drop();
    /* A format() yonder does not read is passed over; a failed file sends
     * the next layout to the next source. */
    web_page("<style>@font-face{font-family:Web;src:url(a.woff2) format('woff2'),url(b.ttf),url(c.ttf)}"
             "p{font-family:Web}</style><p>hello");
    check(s_web.nfiles==1 && web_file("https://fixture.test/b.ttf")==0,
          "web fonts: an unread format is passed over unfetched");
    check(font_land("b.ttf",NULL,"HTTP 404") && s_web.files[0].state==WF_FAILED && s_web.moved,
          "web fonts: a failed file");
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"FAILED font-fetch: https://fixture.test/b.ttf: HTTP 404")!=NULL,
          "web fonts: the failure is on the record");
    pictures_settle();
    check(web_file("https://fixture.test/c.ttf")==1 && s_web.files[1].state==WF_WAITING,
          "web fonts: the next source is wanted");
    check(font_land("c.ttf","Q",NULL) && s_web.files[1].state==WF_FAILED,
          "web fonts: bytes the engine will not open fail too");
    diag_fonts(&g.page);
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"\nfont face 1: Web 400 normal: format(woff2) not read: a.woff2; https://fixture.test/b.ttf failed; "
                        "https://fixture.test/c.ttf failed\n")!=NULL,
          "web fonts: the record walks a face's sources to where it stands");
    web_drop();
    /* Style, then weight: each asks only for its own face. */
    const char *kFaces="<style>@font-face{font-family:W;src:url(plain.ttf)}"
        "@font-face{font-family:W;src:url(italic.ttf);font-style:italic}"
        "@font-face{font-family:W;src:url(bold.ttf);font-weight:700}"
        "@font-face{font-family:W;src:url(light.ttf);font-weight:300}</style>";
    char html[1024];
    snprintf(html,sizeof(html),"%s<p style='font-family:W'>a",kFaces);
    web_page(html);
    check(s_web.nfiles==1 && web_file("https://fixture.test/plain.ttf")==0,"web fonts: normal 400 is the plain face");
    web_drop();
    snprintf(html,sizeof(html),"%s<p style='font-family:W;font-style:italic'>a",kFaces);
    web_page(html);
    check(s_web.nfiles==1 && web_file("https://fixture.test/italic.ttf")==0,"web fonts: italic is the italic face");
    web_drop();
    snprintf(html,sizeof(html),"%s<p style='font-family:W;font-weight:bold'>a",kFaces);
    web_page(html);
    check(s_web.nfiles==1 && web_file("https://fixture.test/bold.ttf")==0,"web fonts: bold is the bold face");
    web_drop();
    /* With no italic face, an italic ask takes the normal one. */
    web_page("<style>@font-face{font-family:W;src:url(plain.ttf)}</style><p style='font-family:W;font-style:italic'>a");
    check(s_web.nfiles==1,"web fonts: italic falls to normal");
    web_drop();
    /* A composite: both rules wanted, the later first. */
    web_page("<style>@font-face{font-family:W;src:url(one.ttf)}@font-face{font-family:W;src:url(two.ttf)}"
             "p{font-family:W}</style><p>a");
    check(s_web.nfiles==2 && web_file("https://fixture.test/two.ttf")==0 && web_file("https://fixture.test/one.ttf")==1,
          "web fonts: a composite's rules are all wanted, the later first");
    web_drop();
    /* A composite past the places a list has: the latest that fit are
     * wanted, a name written twice counting its files once, at most
     * WEBFONTS_AT_ONCE out at a time; the rest follow as each comes in,
     * and the page lays out in them. */
    {
        char many[2048]; size_t at=0;
        at+=snprintf(many+at,sizeof(many)-at,"<style>");
        for(int i=0;i<9;i++) at+=snprintf(many+at,sizeof(many)-at,"@font-face{font-family:W;src:url(f%d.ttf)}",i);
        snprintf(many+at,sizeof(many)-at,"p{font-family:W,W}</style><p>a");
        web_page(many);
        uint64_t fallback=first_text_font();
        int32_t fit=s_web.nfiles;
        check(fit>WEBFONTS_AT_ONCE && fit<8 && web_file("https://fixture.test/f8.ttf")==0 &&
              web_file("https://fixture.test/f0.ttf")<0 &&
              nfont_jobs==WEBFONTS_AT_ONCE && s_web.in_flight==WEBFONTS_AT_ONCE,
              "web fonts: a composite's latest that fit are wanted, at most WEBFONTS_AT_ONCE out at a time");
        check(nfont_jobs>0 && font_jobs[0].job->hooks.metadata.dest==WAY_DEST_FONT &&
              font_jobs[0].job->hooks.metadata.mode==WAY_MODE_NO_CORS,
              "who: a font is asked for as a no-cors font (yonder runs no CORS check on one)");
        for(int round=0;round<9 && nfont_jobs>0;round++) {
            char tail[16]; snprintf(tail,sizeof(tail),"%s",font_jobs[0].job->url+strlen(font_jobs[0].job->url)-6);
            check(font_land(tail,"A",NULL),"web fonts: each file arrives");
        }
        check(s_web.waiting==0 && nfont_jobs==0,"web fonts: the rest followed as each came in");
        pictures_settle();
        check(g.page.tree!=NULL && !flow_incomplete(g.page.tree) && first_text_font()!=0 &&
              first_text_font()!=fallback,
              "web fonts: the faces that fit, a name twice, and the list lays out in them");
        web_drop();
    }
    /* Another page laid out beside the shown one — a page arriving, its
     * sheets still coming — draws none of the shown page's faces and
     * wants none. */
    web_page("<style>@font-face{font-family:Web;src:url(web.ttf)}p{font-family:Web}</style><p>hello");
    check(font_land("web.ttf","A",NULL),"web fonts: the shown page's face is open");
    {
        Page other=g.page;
        other.way.doc=parse_file((const uint8_t *)"<p>other",8,false);
        flow_family_name_t web={"Web",3};
        flow_family_list_t ask={.names=&web,.count=1,.generic=FLOW_GENERIC_SERIF};
        os64_text_font_t *const *list; size_t n; os64_font_face_info_t info;
        s_laying.cascade=g.page.cascade; s_laying.entry=g.page.cascade_entry;
        int32_t files=s_web.nfiles; uint32_t handles=(uint32_t)s_web.nhandles;
        check(page_fonts(&other,&ask,false,false,16,&list,&n,&info)==OS64_FONT_OK &&
              s_web.nfiles==files && (uint32_t)s_web.nhandles==handles && strcmp(info.family,"Test Serif")==0,
              "web fonts: another page draws the system's face and wants nothing");
        check(page_fonts(&g.page,&ask,false,false,16,&list,&n,&info)==OS64_FONT_OK &&
              strcmp(info.family,"Test Sans")==0,
              "web fonts: the shown page draws its own face");
        s_laying.cascade=NULL; s_laying.entry=NULL;
        os64_html_document_free(other.way.doc);
    }
    web_drop();
    /* The names' written order: a system family written first stays
     * first, the page's face its fallback; written after, it is the
     * page's face's. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf)}</style><p style=\"font-family:'test serif',Web\">a</p>");
    {
        uint64_t system=first_text_font();
        check(font_land("w.ttf","A",NULL),"web fonts: order: the face arrives");
        pictures_settle();
        check(first_text_font()==system,"web fonts: a system family written first stays first");
    }
    web_drop();
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf)}</style><p style=\"font-family:Web,'test serif'\">a</p>");
    {
        uint64_t system=first_text_font();
        check(font_land("w.ttf","A",NULL),"web fonts: order: the face arrives");
        pictures_settle();
        check(first_text_font()!=system,"web fonts: a face written first is drawn first");
    }
    web_drop();
    /* A script's change lays the page out again in its staged copy: the
     * faces stay, and the copy is the shown page's. */
    web_page_scripted("<style>@font-face{font-family:Web;src:url(w.ttf)}p{font-family:Web}</style>"
                      "<p id=t>hello</p><script>document.getElementById('t').textContent='changed'</script>",true);
    check(font_land("w.ttf","A",NULL),"web fonts: rebuild: the face arrives");
    pictures_settle();
    {
        uint64_t web=first_text_font();
        os64_js_outcome_t out;
        check(yonder_scripts_step(g.page.scripts,&out) && out.status==OS64_JS_OK && script_rebuild(),
              "web fonts: a script changes the page and it is rebuilt");
        check(first_text_font()==web && s_web.nfiles==1,"web fonts: the rebuilt page keeps its face");
    }
    web_drop();
    /* A face a script brings is asked for by the rebuild that lays it out. */
    web_page_scripted("<p id=t>hello</p><script>var s=document.createElement('style');"
                      "s.textContent='@font-face{font-family:Web;src:url(s.ttf)}p{font-family:Web}';"
                      "document.body.appendChild(s)</script>",true);
    {
        os64_js_outcome_t out;
        check(s_web.nfiles==0 && yonder_scripts_step(g.page.scripts,&out) && out.status==OS64_JS_OK && script_rebuild(),
              "web fonts: a script adds a face and the page is rebuilt");
        check(web_file("https://fixture.test/s.ttf")==0 && s_web.files[0].state==WF_WAITING && nfont_jobs==1,
              "web fonts: the rebuild asks for the script's face");
    }
    web_drop();
    /* A file the pool will not take fails, and the page is laid out again
     * so the face's next source is wanted. */
    font_refuse=true;
    g.badge_missing=g.badge_failed=0; g.badge_text[0]=0;
    web_page("<style>@font-face{font-family:Web;src:url(a.ttf),url(b.ttf)}p{font-family:Web}</style><p>a");
    font_refuse=false;
    check(s_web.files[0].state==WF_FAILED && s_web.moved,"web fonts: a refused file fails and moves the page");
    check(strstr(g.badge_text,"FAILED 1")!=NULL,"web fonts: and the badge is told");
    pictures_settle();
    check(web_file("https://fixture.test/b.ttf")==1 && s_web.files[1].state==WF_WAITING,
          "web fonts: and the next source is asked for");
    web_drop();
    /* An open refused for room is held, and opened after the next layout
     * — not by the feed that follows the arrival, while the old tree still
     * holds the room. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf)}p{font-family:Web}</style><p>hello");
    {
        uint64_t fallback=first_text_font();
        font_open_refusals=1;
        check(font_land("w.ttf","A",NULL) && s_web.files[0].state==WF_HELD && s_web.moved,
              "web fonts: an open refused for room holds the file");
        diag_fonts(&g.page);
        char held[4096]; yonder_diag_render(g.page.diag,held,sizeof(held));
        check(strstr(held,"https://fixture.test/w.ttf came (SFNT, 1 bytes), held for room to open")!=NULL &&
              strstr(held,"1 files: 0 open, 0 failed, 1 coming")!=NULL,
              "web fonts: the record says a file held for room");
        pictures_settle();
        check(s_web.files[0].state==WF_READY && s_web.moved,"web fonts: after the layout, it opens");
        pictures_settle();
        check(first_text_font()!=fallback,"web fonts: and is drawn");
        font_open_refusals=0;
    }
    web_drop();
    /* A held open that succeeds in the feed after a settle's own layout
     * arms the next settle rather than waiting for an event. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf)}p{font-family:Web}</style><p>hello");
    font_open_refusals=1;
    check(font_land("w.ttf","A",NULL) && s_web.files[0].state==WF_HELD,"web fonts: armed: held");
    pictures_settle();
    check(s_web.files[0].state==WF_READY && s_web.moved && g.settle_due!=YONDER_NEVER,
          "web fonts: a face the feed opened after the settle's layout arms the next settle");
    font_open_refusals=0;
    web_drop();
    /* Refused again after the layout, the held file fails: its bytes go,
     * the badge is told, and the face's next source is wanted. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf),url(v.ttf)}p{font-family:Web}</style><p>hello");
    font_open_refusals=2;
    g.badge_missing=g.badge_failed=0; g.badge_text[0]=0;
    check(font_land("w.ttf","A",NULL) && s_web.files[0].state==WF_HELD,"web fonts: refused twice: held");
    pictures_settle();
    check(s_web.files[0].state==WF_FAILED && s_web.held_bytes==0 && s_web.files[0].sfnt==NULL &&
          strstr(g.badge_text,"FAILED 1")!=NULL && s_web.moved,
          "web fonts: refused again after the layout, the held file fails and the badge is told");
    pictures_settle();
    check(web_file("https://fixture.test/v.ttf")==1 && s_web.files[1].state==WF_WAITING,
          "web fonts: and the face's next source is asked for");
    font_open_refusals=0;
    web_drop();
    /* Past the bytes held, a file refused room is failed. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf)}p{font-family:Web}</style><p>hello");
    s_web.held_bytes=WEBFONT_HELD_MAX;
    font_open_refusals=1;
    check(font_land("w.ttf","A",NULL) && s_web.files[0].state==WF_FAILED,
          "web fonts: past the held bytes, a file refused room fails");
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"FAILED font-fetch: https://fixture.test/w.ttf: no room to open it")!=NULL,
          "web fonts: and says why");
    font_open_refusals=0;
    s_web.held_bytes=0;
    web_drop();
    /* A size refused for room gets one more layout, and only one. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf)}p{font-family:Web}</style><p>hello");
    {
        uint64_t fallback=first_text_font();
        check(font_land("w.ttf","A",NULL),"web fonts: reopen: the file is open");
        font_like_refusals=1000;
        pictures_settle();
        check(first_text_font()==fallback && s_web.moved && g.settle_due!=YONDER_NEVER,
              "web fonts: a size refused for room is drawn in the fallback, and laid out again");
        font_like_refusals=0;
        pictures_settle();
        check(first_text_font()!=fallback,"web fonts: the retry draws the face");
        /* The sizes the page holds let go, so the next layout opens them. */
        for(int32_t i=0;i<s_web.nhandles;i++) os64_text_font_release(s_web.handles[i].font);
        s_web.nhandles=0;
        font_like_refusals=1000;
        s_web.moved=true;
        pictures_settle();
        check(s_web.moved,"web fonts: a refused size asks for its one retry");
        webfonts_feed(false);           /* an arrival's feed between: no second retry */
        pictures_settle();
        check(!s_web.moved,"web fonts: refused again, the fallback stands");
        /* In the same geometry, another size refused for room is another
         * first refusal, and gets its own. */
        {
            const garb_font_face_t *faces; bool room;
            check(garb_cascade_font_faces(g.page.cascade,&faces,NULL)==1 &&
                  webfonts_handle(0,&faces[0],77,&room)==NULL && room,"web fonts: a second size is refused for room");
            s_web.moved=false;
            webfonts_feed(true);
            check(s_web.moved,"web fonts: a second size refused in one geometry gets its own retry");
        }
        /* Another size is another first refusal: zoomed, it gets its own. */
        g.zoom=1250;
        s_web.moved=true;
        pictures_settle();
        check(s_web.moved,"web fonts: a new size refused for room gets its own retry");
        pictures_settle();
        check(!s_web.moved,"web fonts: and refused again, its fallback stands");
        /* The retries spent fill the table: another geometry still gets
         * its own. */
        for(int32_t r=s_web.nretried;r<WEBFONT_REFUSED_MAX;r++) s_web.retried[r]=0x5eed0000u+(uint64_t)r;
        s_web.nretried=WEBFONT_REFUSED_MAX;
        g.zoom=1500;
        s_web.moved=true;
        pictures_settle();
        check(s_web.moved,"web fonts: with the retries spent filling the table, a new geometry's size gets its retry");
        pictures_settle();
        check(!s_web.moved,"web fonts: and its fallback stands after");
        g.zoom=1000;
        font_like_refusals=0;
    }
    web_drop();
    /* The provider's fallbacks come after a page face named after the
     * system family; a second system family named is no MISSING font; a
     * system family named past 32 names is found. */
    web_page("<style>@font-face{font-family:Web;src:url(w.ttf)}</style>"
             "<p style=\"font-family:'test serif',Web,'test sans'\">a</p>");
    check(font_land("w.ttf","A",NULL),"web fonts: fallbacks: the face arrives");
    pictures_settle();
    {
        flow_family_name_t names[]={{"test serif",10},{"Web",3}};
        flow_family_list_t ask={.names=names,.count=2,.generic=FLOW_GENERIC_SERIF};
        os64_text_font_t *const *list; size_t n; os64_font_face_info_t info, second;
        s_laying.cascade=g.page.cascade; s_laying.entry=g.page.cascade_entry;
        check(page_fonts(&g.page,&ask,false,false,16,&list,&n,&info)==OS64_FONT_OK && n>=3 &&
              os64_text_font_info(list[1],&second)==OS64_FONT_OK && strcmp(info.family,"Test Serif")==0 &&
              strcmp(second.family,"Test Sans")==0,
              "web fonts: the system face, then the page face named after it, then the fallbacks");
        s_laying.cascade=NULL; s_laying.entry=NULL;
    }
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"MISSING font test")==NULL,"web fonts: a second system family named is not missing");
    web_drop();
    {
        char many[4096]; size_t at=0;
        at+=snprintf(many+at,sizeof(many)-at,"<style>@font-face{font-family:Web;src:url(w.ttf)}</style><p style=\"font-family:");
        for(int i=0;i<34;i++) at+=snprintf(many+at,sizeof(many)-at,"n%d,",i);
        snprintf(many+at,sizeof(many)-at,"'test sans'\">a</p>");
        web_page(many);
        flow_family_name_t names[36]; char text[36][16];
        for(int i=0;i<34;i++) { snprintf(text[i],16,"n%d",i); names[i]=(flow_family_name_t){text[i],(uint32_t)strlen(text[i])}; }
        names[34]=(flow_family_name_t){"test sans",9};
        flow_family_list_t ask={.names=names,.count=35,.generic=FLOW_GENERIC_SERIF};
        os64_text_font_t *const *list; size_t n; os64_font_face_info_t info;
        s_laying.cascade=g.page.cascade; s_laying.entry=g.page.cascade_entry;
        check(page_fonts(&g.page,&ask,false,false,16,&list,&n,&info)==OS64_FONT_OK && strcmp(info.family,"Test Sans")==0,
              "web fonts: a system family named past 32 names is found");
        s_laying.cascade=NULL; s_laying.entry=NULL;
        web_drop();
    }
    /* A face in a linked sheet resolves against the sheet. */
    probe_web=true;
    loop_page("<link rel=stylesheet href=css/s.css><p style='font-family:Web'>x</p>");
    check(loop_settle() && sheet_land("/css/s.css","@font-face{font-family:Web;src:url(f.ttf)}") && loop_settle(),
          "web fonts: the page arrives with its sheet");
    check(s_web.nfiles==0 && s_web.moved,"web fonts: laid out before it was shown, the page wants no face yet");
    pictures_settle();
    check(web_file("http://fixture.test/css/f.ttf")>=0 && nfont_jobs==1,
          "web fonts: shown, it is laid out again and wants its face, against its sheet");
    webfonts_leave();
    while(nfont_jobs>0) os64_free(font_jobs[--nfont_jobs].job);
    loop_drop();
    probe_web=false;
    /* Leaving the page cancels what is out; a file for a page gone is let go. */
    web_page("<style>@font-face{font-family:Web;src:url(web.ttf)}p{font-family:Web}</style><p>hello");
    yonder_webfont_job_t *stale=os64_calloc(1,sizeof(*stale));
    *stale=*font_jobs[0].job; stale->generation=g.page_serial+1;
    yonder_webfont_t *late=os64_calloc(1,sizeof(*late));
    reaped(0,stale,late);
    check(s_web.files[0].state==WF_WAITING && s_web.waiting==1,"web fonts: another page's file is let go");
    unsigned cancels=font_cancels;
    webfonts_leave();
    check(font_cancels==cancels+1 && s_web.nfiles==0 && s_web.waiting==0,"web fonts: leaving cancels and clears");
    web_drop();
    cascade_swap_case();
    os64_font_family_cache_destroy(s_faces.families);
    s_faces.families=NULL; s_faces.text=NULL;
}

/* ── PAGE STORAGE IN THE WINDOW (storage.h, MODERN_WEB.md § Slice 1a) ──
 * localStorage in its file under the profile folder: made, written at a
 * task's end, coalesced, written at departure, full; sessionStorage across
 * a navigation in the window; a profile that cannot be made; Clear page
 * storage; and the timing and the screen a page reads. */
#define STORE_FILE "/tmp/profile/storage/http%3A%2F%2Ffixture.test"
/* The page's next navigation in this window, as loop_page's but keeping
 * the window, to `address`, which the head says the page came from. */
static void storage_page_as(const char *address, const char *rest, NavKind kind) {
    start_trip(address,NULL,NULL,kind,WAY_BY_BROWSER,NULL);
    char *html=pad_page(rest);
    way_head_t h=stream_head("text/html","",false);
    os64_strcopy(h.url,sizeof(h.url),address);
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_body((const uint8_t *)html,strlen(html),YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    free(html);
}
static void storage_page(const char *address, const char *rest) {
    storage_page_as(address,rest,NAV_GO);
}
/* Whether the store file holds `key` as `value` (NULL: holds no `key`). */
typedef struct { const char *key, *value; bool found, right; } StoreLook;
static bool store_look(void *opaque, const char *key, size_t key_length, const char *value, size_t value_length) {
    StoreLook *l=opaque;
    if(strlen(l->key)!=key_length || memcmp(l->key,key,key_length)) return true;
    l->found=true;
    l->right=l->value!=NULL && value!=NULL && strlen(l->value)==value_length && !memcmp(l->value,value,value_length);
    return false;
}
static bool store_holds(const char *key, const char *value) {
    int f=fake_file(STORE_FILE);
    if(f<0) return false;
    StoreLook l={key,value,false,false};
    if(!os64_dom_storage_walk(fake_files[f].data,fake_files[f].len,store_look,&l)) return false;
    return value==NULL ? !l.found : l.found && l.right;
}
static void storage_window_cases(void) {
    fake_reset();
    s_storage=yonder_storage_new("/tmp/profile");
    s_storage_said[0]='\0';
    loop_page("<p id=out>start</p><script>localStorage.setItem('a','1');"
        "setTimeout(function(){localStorage.setItem('b','2')},50);"
        "setTimeout(function(){localStorage.setItem('c','3')},5000);</script>");
    check(loop_settle() && g.page.tree!=NULL,"storage: the page arrives");
    check(fake_is_dir("/tmp/profile") && fake_is_dir("/tmp/profile/storage") && store_holds("a","1"),
        "storage: the profile folder is made, and a task's write is in the origin's file at its end");
    int writes=fake_writes;
    now_ms=150;
    check(script_turn() && fake_writes==writes && store_holds("b",NULL),
        "storage: a write within a second of the last commit waits");
    check(yonder_scripts_storage_due(g.page.scripts)==1100,"storage: and is due a second after it");
    now_ms=1100;
    storage_turn();
    check(fake_writes==writes+1 && store_holds("b","2") && store_holds("a","1"),
        "storage: the loop commits it when it is due, merged onto the file");
    /* Another window writes the file: this page reads it at its next task. */
    fake_second++;
    {
        static const char kOther[]="os64 storage 1\n+1 1\nab\n+1 1\nb2\n+5 1\nother1\n";
        int f=fake_file(STORE_FILE);
        free(fake_files[f].data);
        fake_files[f].data=malloc(sizeof(kOther)); memcpy(fake_files[f].data,kOther,sizeof(kOther));
        fake_files[f].len=sizeof(kOther)-1; fake_files[f].mtime=fake_second;
    }
    now_ms=5100;
    check(script_turn() && store_holds("other","1") && store_holds("c","3"),
        "storage: a commit merges onto what another window wrote");
    storage_page("http://fixture.test/two","<p id=out>start</p><script>"
        "sessionStorage.setItem('s','kept');localStorage.setItem('d','4');</script>");
    check(loop_settle() && g.page.tree!=NULL,"storage: the second page arrives");
    /* A second write, in a task within a second of the first commit, is
     * left for the departure commit. */
    storage_page("http://fixture.test/three","<p id=out>start</p><script>"
        "document.getElementById('out').textContent=sessionStorage.getItem('s')+' '+localStorage.getItem('d');"
        "localStorage.setItem('first','1');"
        "document.addEventListener('DOMContentLoaded',function(){localStorage.setItem('e','5')});</script>");
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","kept 4"),
        "storage: sessionStorage survives a navigation in the window, and localStorage is the file's");
    check(store_holds("first","1") && store_holds("e",NULL),"storage: the second write waits for its second");
    storage_page("http://elsewhere.test/","<p id=out>start</p><script>"
        "document.getElementById('out').textContent=String(sessionStorage.getItem('s'))+' '+localStorage.length;</script>");
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","null 0"),
        "storage: another origin has its own areas");
    check(store_holds("e","5"),"storage: the page that left made its departure commit");
    check(fake_file("/tmp/profile/storage/http%3A%2F%2Felsewhere.test")<0,
        "storage: an origin that stored nothing has no file");
    /* Clear page storage: the files go, and so do the pages' maps. */
    char line[160];
    storage_clear(line,sizeof(line));
    check(fake_file(STORE_FILE)<0 && strstr(line,"1 stored site gone")!=NULL,
        "storage: Clear page storage removes the origins' files and says how many");
    storage_page("http://fixture.test/four","<p id=out>start</p><script>"
        "document.getElementById('out').textContent=String(sessionStorage.getItem('s'))+' '+localStorage.length;</script>");
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","null 0"),
        "storage: after a clear a page finds nothing, either area");
    loop_drop();
    yonder_storage_free(s_storage);
    s_storage=NULL;
}
static void storage_full_cases(void) {
    fake_reset();
    s_storage=yonder_storage_new("/tmp/profile");
    loop_page("<p id=out>start</p><script>localStorage.setItem('mine','x');"
        "setTimeout(function(){localStorage.setItem('later','y')},400);</script>");
    check(loop_settle() && g.page.tree!=NULL && store_holds("mine","x"),"storage full: the page's first write");
    now_ms=500;
    check(script_turn() && store_holds("later",NULL),"storage full: a second write, not yet due");
    /* Another window fills the store to its quota before it is. */
    size_t units=OS64_DOM_STORAGE_QUOTA/2-6;            /* 'k' and 'mine'='x' with it: the whole quota */
    char *records=malloc(units+128);
    int head=snprintf(records,128,"os64 storage 1\n+4 1\nminex\n+1 %zu\nk",units);
    memset(records+head,'v',units); records[head+units]='\n';
    int f=fake_file(STORE_FILE);
    free(fake_files[f].data);
    fake_files[f].data=records; fake_files[f].len=(size_t)head+units+1; fake_files[f].mtime=++fake_second;
    now_ms=1100;
    storage_turn();
    char record[8192];
    yonder_diag_render(g.page.diag,record,sizeof(record));
    check(strstr(record,"FAILED storage: localStorage of http://fixture.test is full; not kept: later")!=NULL,
        "storage full: the commit keeps nothing and the record names the key it could not keep");
    check(store_holds("later",NULL) && store_holds("mine","x"),"storage full: the store is as the other window left it");
    loop_drop();
    yonder_storage_free(s_storage);
    s_storage=NULL;
    /* A profile whose folder cannot be made: the page's storage works for
     * its life, and the status line says why it is not kept. */
    fake_reset();
    s_storage=yonder_storage_new("/nowhere/profile");
    s_storage_said[0]='\0';
    loop_page("<p id=out>start</p><script>localStorage.setItem('a','1');"
        "document.getElementById('out').textContent=localStorage.getItem('a');</script>");
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","1"),"storage: a page whose store cannot be kept still works");
    storage_turn();
    check(strstr(g.status_text,"Page storage is not kept: /nowhere/profile/storage cannot be made.")!=NULL,
        "storage: the status line says the profile folder cannot be made");
    check(!fake_is_dir("/nowhere"),"storage: nothing is made outside a parent that exists");
    loop_drop();
    yonder_storage_free(s_storage);
    s_storage=NULL;
}
/* THE PAGE DEPARTS (yonder.c's page_departs): a write its page made within
 * the coalescing second is in the store before the next page's scripts run,
 * whatever took the window away: a link, Reload, Back, Forward, a file.
 * sessionStorage's with it. */
static void storage_departure_page(void) {
    fake_reset();
    s_storage=yonder_storage_new("/tmp/profile");
    loop_page("<p id=out>start</p><script>localStorage.setItem('a','1');sessionStorage.setItem('a','1');"
        "setTimeout(function(){localStorage.setItem('tok','x');sessionStorage.setItem('tok','y')},50);</script>");
    check(loop_settle() && g.page.tree!=NULL,"departure: the first page arrives");
    now_ms=150;
    check(script_turn() && store_holds("a","1") && store_holds("tok",NULL),
        "departure: the timer's write waits for its second");
}
static void storage_departure_cases(void) {
    static const struct { NavKind kind; const char *name; } kPaths[]={
        {NAV_GO,"departure: a link's next page reads what the last page wrote"},
        {NAV_RELOAD,"departure: Reload's page reads what the last page wrote"},
        {NAV_BACK,"departure: Back's page reads what the last page wrote"},
        {NAV_FORWARD,"departure: Forward's page reads what the last page wrote"},
    };
    for(size_t i=0;i<sizeof(kPaths)/sizeof(kPaths[0]);i++) {
        storage_departure_page();
        storage_page_as("http://fixture.test/two","<p id=out>start</p><script>"
            "document.getElementById('out').textContent=localStorage.getItem('tok')+' '+sessionStorage.getItem('tok');"
            "</script>",kPaths[i].kind);
        check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","x y"),kPaths[i].name);
        loop_drop();
        yonder_storage_free(s_storage);
        s_storage=NULL;
    }
    /* A file: the write is in the store as the file's parse begins. */
    storage_departure_page();
    slurp_path="/pages/local.html";
    slurp_bytes="<p id=out>disk</p>";
    open_address("/pages/local.html",NAV_GO,NULL,NULL);
    check(store_holds("tok","x"),"departure: a file's page begins after the last page's write is kept");
    check(loop_settle(),"departure: the file arrives");
    slurp_path=NULL;
    loop_drop();
    yonder_storage_free(s_storage);
    s_storage=NULL;
    /* A navigation that never becomes a page leaves the page where it is:
     * nothing departs, and the write commits when it falls due. */
    storage_departure_page();
    start_trip("http://fixture.test/gone",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    stream_post_verdict(false,OS64_FETCH_DIAL_FAILED,"No road to fixture.test");
    loop_settle();
    check(store_holds("tok",NULL),"departure: a failed navigation is no departure");
    now_ms=1200;
    storage_turn();
    check(store_holds("tok","x"),"departure: and the write commits when due");
    loop_drop();
    yonder_storage_free(s_storage);
    s_storage=NULL;
}
/* THE STORE'S DISK, BADLY BEHAVED: yonder's provider verbs called as
 * libdom calls them, against a file that is missing, a folder, unreadable,
 * not libdom's, too big for the room or for any image; a disk that writes a
 * byte at a time or refuses a write, a sync or a rename; an origin too long
 * to name a file, one whose store is spelled like another's temp; the same
 * call twice; and every allocation failed in turn. What is on the disk is
 * always a whole image, the old or the new, and no temp is left behind. */
static void fake_put(const char *path, const char *bytes, size_t length) {
    int f=fake_file(path);
    if(f<0) for(f=0;f<FAKE_FILES && fake_files[f].used;f++) {}
    free(fake_files[f].data);
    fake_files[f].used=true; snprintf(fake_files[f].path,256,"%s",path);
    fake_files[f].data=malloc(length+1); memcpy(fake_files[f].data,bytes,length); fake_files[f].data[length]='\0';
    fake_files[f].len=length; fake_files[f].mtime=++fake_second;
}
static int fake_temps(void) {
    int n=0;
    for(int i=0;i<FAKE_FILES;i++) if(fake_files[i].used && strstr(fake_files[i].path,".new")!=NULL) n++;
    return n;
}
static bool problem_has(const yonder_storage_t *st, const char *text) {
    const char *p=yonder_storage_problem(st);
    return p!=NULL && strstr(p,text)!=NULL;
}
static void storage_disk_cases(void) {
    static const char kOne[]="os64 storage 1\n+1 1\nk1\n", kTwo[]="os64 storage 1\n+1 1\nk2\n";
    static const char kGone[]="os64 storage 1\n-1\nk\n";
    const os64_dom_storage_area_t L=OS64_DOM_STORAGE_LOCAL, S=OS64_DOM_STORAGE_SESSION;
    const char *origin="http://fixture.test";
    char out[64];
    fake_reset();
    yonder_storage_t *st=yonder_storage_new("/tmp/profile");
    check(yonder_storage_identity(st,L,origin)==0 && yonder_storage_load(st,L,origin,out,sizeof(out))==0 &&
          yonder_storage_problem(st)==NULL,"storage disk: no file is an empty store, and no problem");
    check(yonder_storage_commit(st,L,origin,kGone,strlen(kGone),NULL)==OS64_DOM_STORAGE_OK &&
          fake_file(STORE_FILE)<0,"storage disk: removing from no store leaves no file");
    check(yonder_storage_commit(st,L,origin,kOne,strlen(kOne),NULL)==OS64_DOM_STORAGE_OK && store_holds("k","1") &&
          yonder_storage_identity(st,L,origin)!=0,"storage disk: a commit makes the file");
    int writes=fake_writes;
    check(yonder_storage_commit(st,L,origin,kOne,strlen(kOne),NULL)==OS64_DOM_STORAGE_OK && store_holds("k","1") &&
          fake_writes==writes+1 && fake_file(STORE_FILE)>=0 && fake_files[fake_file(STORE_FILE)].len==strlen(kOne),
          "storage disk: the same commit twice writes the same image");
    int64_t length=yonder_storage_load(st,L,origin,out,sizeof(out));
    check(length==(int64_t)strlen(kOne) && !memcmp(out,kOne,strlen(kOne)),"storage disk: a load reads the image");
    memset(out,'z',sizeof(out));
    check(yonder_storage_load(st,L,origin,out,4)==(int64_t)strlen(kOne) && out[0]=='z',
          "storage disk: a store bigger than the room answers its length and writes nothing");
    fake_short_write=true;
    check(yonder_storage_commit(st,L,origin,kTwo,strlen(kTwo),NULL)==OS64_DOM_STORAGE_OK && store_holds("k","2"),
          "storage disk: a disk that writes a byte at a time is written whole");
    fake_short_write=false;
    /* A write, a sync, a rename refused: the old image stays, whole, and
     * the temp goes. */
    bool *refusals[]={&fake_fail_write,&fake_fail_sync,&fake_fail_rename};
    const char *names[]={"write","sync","rename"};
    for(size_t i=0;i<3;i++) {
        char name[96];
        *refusals[i]=true;
        os64_dom_storage_status_t status=yonder_storage_commit(st,L,origin,kOne,strlen(kOne),NULL);
        *refusals[i]=false;
        snprintf(name,sizeof(name),"storage disk: a refused %s keeps the old image, leaves no temp, and says so",names[i]);
        check(status==OS64_DOM_STORAGE_FAILED && store_holds("k","2") && fake_temps()==0 &&
              problem_has(st,"storage/http%3A%2F%2Ffixture.test cannot be written."),name);
    }
    /* A file that cannot be read: never written over. */
    fake_slurp_fail=true;
    check(yonder_storage_load(st,L,origin,out,sizeof(out))<0 && problem_has(st,"cannot be read."),
          "storage disk: an unreadable store loads as broken and says so");
    check(yonder_storage_commit(st,L,origin,kOne,strlen(kOne),NULL)==OS64_DOM_STORAGE_FAILED,
          "storage disk: and refuses a commit");
    fake_slurp_fail=false;
    check(store_holds("k","2"),"storage disk: the unreadable store is as it was");
    /* Bigger than any image libdom writes: answered by its size alone. */
    int f=fake_file(STORE_FILE);
    size_t real=fake_files[f].len;
    fake_files[f].len=(size_t)64*1024*1024;
    check(yonder_storage_load(st,L,origin,out,sizeof(out))>(int64_t)(32*1024*1024),
          "storage disk: a store past the cap answers a length libdom refuses, unread");
    fake_files[f].len=real;
    /* Not libdom's format. */
    fake_put(STORE_FILE,"hello\n",6);
    check(yonder_storage_commit(st,L,origin,kOne,strlen(kOne),NULL)==OS64_DOM_STORAGE_FAILED &&
          problem_has(st,"its file is not one yonder wrote") && !strcmp(fake_text(STORE_FILE),"hello\n"),
          "storage disk: a file not in the format is never written over");
    /* An empty file where the store should be: there, and no image. */
    fake_put(STORE_FILE,"",0);
    check(yonder_storage_load(st,L,origin,out,sizeof(out))<0 && problem_has(st,"its file is not one yonder wrote"),
          "storage disk: an empty file at the store's name loads as broken");
    check(yonder_storage_commit(st,L,origin,kOne,strlen(kOne),NULL)==OS64_DOM_STORAGE_FAILED &&
          fake_file(STORE_FILE)>=0 && fake_files[fake_file(STORE_FILE)].len==0 && fake_temps()==0,
          "storage disk: and is never written over");
    /* A folder where the file should be. */
    (void)os64_unlink(STORE_FILE);
    snprintf(fake_dirs[nfake_dirs++],256,"%s",STORE_FILE);
    check(yonder_storage_load(st,L,origin,out,sizeof(out))<0 && problem_has(st,"cannot be read."),
          "storage disk: a folder at the store's name loads as broken");
    check(yonder_storage_commit(st,L,origin,kOne,strlen(kOne),NULL)==OS64_DOM_STORAGE_FAILED && fake_temps()==0 &&
          fake_file(STORE_FILE)<0,"storage disk: and is never written over");
    nfake_dirs--;
    /* An origin whose store name is another's temp name as spelled before
     * the '+': the other's store is untouched. */
    static const char kOther[]="/tmp/profile/storage/http%3A%2F%2Ffixture.test.7.7.new";
    fake_put(kOther,kTwo,strlen(kTwo));
    check(yonder_storage_commit(st,L,origin,kOne,strlen(kOne),NULL)==OS64_DOM_STORAGE_OK && store_holds("k","1") &&
          fake_text(kOther)!=NULL && !strcmp(fake_text(kOther),kTwo),
          "storage disk: no origin's store is another's temp");
    (void)os64_unlink(kOther);
    /* An origin too long to name a file, with its temp, in a path. */
    char longest[300]="http://";
    memset(longest+7,'a',240); longest[247]='\0';
    check(yonder_storage_commit(st,L,longest,kOne,strlen(kOne),NULL)==OS64_DOM_STORAGE_FAILED &&
          problem_has(st,"its address is too long to name a file") &&
          yonder_storage_load(st,L,longest,out,sizeof(out))<0 && yonder_storage_identity(st,L,longest)==0,
          "storage disk: an origin too long to name a file is kept in no file, and the problem names it");
    yonder_storage_t *relative=yonder_storage_new("profile");
    check(yonder_storage_commit(relative,L,origin,kOne,strlen(kOne),NULL)==OS64_DOM_STORAGE_FAILED &&
          problem_has(relative,"the profile folder is not a full path (profile)"),
          "storage disk: a profile that is not a full path is said to be so");
    yonder_storage_free(relative);
    char deep[256]="/";
    memset(deep+1,'d',250); deep[251]='\0';
    yonder_storage_t *tall=yonder_storage_new(deep);
    check(yonder_storage_commit(tall,L,origin,kOne,strlen(kOne),NULL)==OS64_DOM_STORAGE_FAILED &&
          problem_has(tall,"the profile folder's path is too long"),
          "storage disk: a profile with no room for its storage folder is said to be too long");
    yonder_storage_free(tall);
    /* A file in the way of the profile folder. */
    yonder_storage_t *blocked=yonder_storage_new("/tmp/blocked/profile");
    fake_put("/tmp/blocked","x",1);
    check(yonder_storage_commit(blocked,L,origin,kOne,strlen(kOne),NULL)==OS64_DOM_STORAGE_FAILED &&
          problem_has(blocked,"/tmp/blocked/profile/storage cannot be made."),
          "storage disk: a file where a folder must be refuses the commit");
    yonder_storage_free(blocked);
    /* The last key removed: the file goes; a file another window removed
     * first is gone as asked. */
    check(yonder_storage_commit(st,L,origin,kGone,strlen(kGone),NULL)==OS64_DOM_STORAGE_OK && fake_file(STORE_FILE)<0,
          "storage disk: a store left empty is removed");
    /* Clear twice, and the sessions with it. */
    check(yonder_storage_commit(st,S,origin,kOne,strlen(kOne),NULL)==OS64_DOM_STORAGE_OK &&
          yonder_storage_commit(st,L,origin,kOne,strlen(kOne),NULL)==OS64_DOM_STORAGE_OK,"storage disk: both areas kept");
    check(yonder_storage_clear(st)==1 && yonder_storage_clear(st)==0 &&
          yonder_storage_identity(st,S,origin)==0 &&
          yonder_storage_load(st,S,origin,out,sizeof(out))==0 && yonder_storage_problem(st)==NULL,
          "storage disk: Clear page storage twice: the files and the sessions go once, and the problem with them");
    /* Every allocation failed in turn, each area: an answer of OK or
     * FAILED, the store whole before or after, no temp, nothing leaked. */
    for(int area=0;area<2;area++) {
        bool swept=false, whole=true, leaked=false;
        for(size_t cut=1;cut<200 && !swept;cut++) {
            fake_reset();
            yonder_storage_t *sw=yonder_storage_new("/tmp/profile");
            if(yonder_storage_commit(sw,area?S:L,origin,kOne,strlen(kOne),NULL)!=OS64_DOM_STORAGE_OK) { whole=false; break; }
            size_t before=live;
            attempts=0; fail_at=cut;
            os64_dom_storage_status_t status=yonder_storage_commit(sw,area?S:L,origin,kTwo,strlen(kTwo),NULL);
            int64_t loaded=yonder_storage_load(sw,area?S:L,origin,out,sizeof(out));
            swept=attempts<cut;
            fail_at=0;
            const char *want=status==OS64_DOM_STORAGE_OK ? kTwo : kOne;
            if(status!=OS64_DOM_STORAGE_OK && status!=OS64_DOM_STORAGE_FAILED) whole=false;
            if(area==0 && (!store_holds("k",status==OS64_DOM_STORAGE_OK ? "2" : "1") || fake_temps()!=0)) whole=false;
            if(loaded>=0 && (loaded!=(int64_t)strlen(want) || memcmp(out,want,strlen(want)))) whole=false;
            if(live!=before) leaked=true;
            yonder_storage_free(sw);
        }
        check(swept && whole && !leaked,area ? "storage disk: sessionStorage's commit and load with every allocation failed in turn"
                                             : "storage disk: localStorage's commit and load with every allocation failed in turn");
    }
    yonder_storage_free(st);
    yonder_storage_free(NULL);
    fake_reset();
}
/* A DEPARTED PAGE RUNS NOTHING MORE (HTML unloads its document): its queued
 * timer, due while the next page streams in, neither runs nor writes, now
 * or at the departed page's teardown. */
static void storage_departed_tasks_case(void) {
    fake_reset();
    s_storage=yonder_storage_new("/tmp/profile");
    loop_page("<p id=out>start</p><script>localStorage.setItem('a','1');"
        "setTimeout(function(){localStorage.setItem('late','1');document.title='ran'},500);</script>");
    check(loop_settle() && g.page.tree!=NULL,"departed: the first page arrives");
    yonder_scripts_t *departing=g.page.scripts;
    storage_page_as("http://fixture.test/two","<p id=out>start</p><script>"
        "document.getElementById('out').textContent=String(localStorage.getItem('late'));</script>",NAV_GO);
    /* The next page's parse begins: the page on screen departs. */
    for(int i=0;i<5 && g.stream.parser==NULL && g.stream.active;i++) stream_turn();
    check(g.page.scripts==departing && !yonder_scripts_alive(departing),
          "departed: the page on screen is unloaded as the next one's parse begins");
    now_ms+=1000;
    (void)script_turn();
    check(store_holds("late",NULL),"departed: and wrote nothing");
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","null") && store_holds("late",NULL) &&
          store_holds("a","1"),"departed: nor at its teardown, and the next page reads the store without it");
    loop_drop();
    yonder_storage_free(s_storage);
    s_storage=NULL;
}
/* TWO YONDERS, ONE STORE (storage.c § Two yonders): another window's
 * commit lands between this one's read and its rename; the store's
 * identity, read again before the rename, sees it, and the merge is made
 * again onto what that window wrote, so neither's changes are lost. */
static yonder_storage_t *other_window;
static const char kOtherChange[]="os64 storage 1\n+5 1\nother1\n";
/* The other window, task 8, commits. */
static void other_commits(void) {
    uint64_t me=fake_task;
    fake_task=8;
    fake_second++;          /* a later second: its store's identity moves */
    check(yonder_storage_commit(other_window,OS64_DOM_STORAGE_LOCAL,"http://fixture.test",kOtherChange,
                                sizeof(kOtherChange)-1,NULL)==OS64_DOM_STORAGE_OK,"two yonders: the other window commits");
    fake_task=me;
}
static void storage_two_yonder_cases(void) {
    static const char kMine[]="os64 storage 1\n+4 1\nmine1\n";
    const os64_dom_storage_area_t L=OS64_DOM_STORAGE_LOCAL;
    const char *origin="http://fixture.test";
    fake_reset();
    yonder_storage_t *st=yonder_storage_new("/tmp/profile");
    other_window=yonder_storage_new("/tmp/profile");
    check(yonder_storage_commit(st,L,origin,"os64 storage 1\n+4 1\nseed1\n",26,NULL)==OS64_DOM_STORAGE_OK,
          "two yonders: a store to start from");
    fake_sync_hook=other_commits;
    check(yonder_storage_commit(st,L,origin,kMine,sizeof(kMine)-1,NULL)==OS64_DOM_STORAGE_OK &&
          store_holds("seed","1") && store_holds("other","1") && store_holds("mine","1") && fake_temps()==0,
          "two yonders: a store another window moved before the rename is merged onto again, and neither loses");
    yonder_storage_free(st);
    yonder_storage_free(other_window);
    other_window=NULL;
    fake_reset();
}
/* Clear page storage removes every store, one unlink each; a window that
 * commits after it publishes only its own changes. */
static void storage_clear_cases(void) {
    const os64_dom_storage_area_t L=OS64_DOM_STORAGE_LOCAL;
    fake_reset();
    yonder_storage_t *st=yonder_storage_new("/tmp/profile");
    other_window=yonder_storage_new("/tmp/profile");
    check(yonder_storage_commit(st,L,"http://fixture.test","os64 storage 1\n+4 1\nseed1\n",26,NULL)==
          OS64_DOM_STORAGE_OK &&
          yonder_storage_commit(st,L,"http://two.test","os64 storage 1\n+4 1\nseed1\n",26,NULL)==OS64_DOM_STORAGE_OK,
          "clear: two stores");
    check(yonder_storage_clear(st)==2 && fake_file(STORE_FILE)<0 &&
          fake_file("/tmp/profile/storage/http%3A%2F%2Ftwo.test")<0,"clear: every store goes");
    other_commits();
    check(store_holds("other","1") && store_holds("seed",NULL),
          "clear: a window committing after it publishes its own changes, and nothing that was cleared");
    check(yonder_storage_commit(st,L,"http://two.test","os64 storage 1\n+4 1\nseed1\n",26,NULL)==OS64_DOM_STORAGE_OK,
          "clear: a store to clear from Settings");
    yonder_storage_t *window_store=s_storage;
    s_storage=st;
    char line[200];
    storage_clear(line,sizeof(line));
    s_storage=window_store;
    check(strstr(line,"Cleared page storage: 2 stored sites gone.")!=NULL,line);
    yonder_storage_free(st);
    yonder_storage_free(other_window);
    other_window=NULL;
    fake_reset();
}
/* What a crash leaves: temps whose task is gone (or is this one, or a
 * zombie). The first touch of the profile's stores removes them, and Clear
 * page storage does, counting only stores; a live window's temp stays. */
/* THE COOKIE FILE (cookies.h), on the fake disk with the real jar: read at
 * start, written after changes at most once a second, the three switches,
 * Clear cookies, two windows sharing one file, a file that cannot be read
 * never written over, a write that fails said and tried again later. */
#define COOKIE_FILE "/tmp/profile/cookies"
static void cookie_hear(way_jar_t *jar, const char *line) {
    os64_url_t u; os64_url_parse("https://k.test/",&u);
    way_jar_hear(jar,&u,true,line,strlen(line),js_test_clock.epoch);
}
static const char *cookie_sent(way_jar_t *jar) {
    static char out[1024];
    os64_url_t u; os64_url_parse("https://k.test/",&u);
    way_jar_cookies(jar,&u,true,js_test_clock.epoch,out,sizeof(out),NULL);
    return out;
}
static void cookie_file_seed(const char *text) {
    int f=fake_file(COOKIE_FILE);
    if(f<0) { for(f=0;fake_files[f].used;f++) {} fake_files[f].used=true; snprintf(fake_files[f].path,256,"%s",COOKIE_FILE); }
    free(fake_files[f].data);
    fake_files[f].data=strdup(text); fake_files[f].len=strlen(text); fake_files[f].mtime=++fake_second;
}
/* Another window's write, landing between this one's read and rename. */
static void cookie_racer(void) {
    char text[4096];
    snprintf(text,sizeof(text),"%sk.test\tFALSE\t/\tFALSE\t1800000000\tracer\t1\n",fake_text(COOKIE_FILE));
    cookie_file_seed(text);
}
static void cookie_file_cases(void) {
    yonder_cookies_mode_t mode;
    check(yonder_cookies_mode_of("ON",&mode) && mode==YONDER_COOKIES_ON && yonder_cookies_mode_of("Session",&mode) &&
          mode==YONDER_COOKIES_SESSION && yonder_cookies_mode_of("off",&mode) && mode==YONDER_COOKIES_OFF &&
          !yonder_cookies_mode_of("maybe",&mode),"cookie file: the three words, without case, and nothing else");
    /* Read at start: a file another run left. */
    fake_reset();
    snprintf(fake_dirs[nfake_dirs++],256,"/tmp/profile");
    cookie_file_seed("# Netscape HTTP Cookie File\n#HttpOnly_k.test\tFALSE\t/\tTRUE\t1800000000\tsid\tkept\n"
                     "k.test\tFALSE\t/\tFALSE\t1600000000\told\t1\n");
    way_jar_t *jar=way_jar_new();
    yonder_cookies_t *k=yonder_cookies_new(jar,"/tmp/profile",YONDER_COOKIES_ON);
    check(k!=NULL && os64_streq(cookie_sent(jar),"sid=kept") && yonder_cookies_due(k)==YONDER_NEVER,
        "cookie file: on, the file is read at start, its expired line not, and nothing is owed");
    /* A change is written at the loop's next turn, then at most once a
     * second however many come. */
    int writes=fake_writes;
    cookie_hear(jar,"a=1; Max-Age=100000");
    check(yonder_cookies_due(k)==0,"cookie file: a change is due at once");
    yonder_cookies_turn(k,5000);
    const char *text=fake_text(COOKIE_FILE);
    check(fake_writes==writes+1 && text!=NULL && strstr(text,"\ta\t1\n")!=NULL && strstr(text,"\tsid\tkept\n")!=NULL &&
          strstr(text,"sign in as you")!=NULL && strstr(text,"\told\t")==NULL,
        "cookie file: the write merges the change onto the file, its header saying what it is");
    for(int i=0;i<1000;i++) { char line[48]; snprintf(line,sizeof(line),"loop=%d; Max-Age=100000",i); cookie_hear(jar,line); }
    check(yonder_cookies_due(k)==5000+YONDER_COOKIES_WRITE_MS,"cookie file: the next is due a second after the last");
    yonder_cookies_turn(k,5500);
    check(fake_writes==writes+1,"cookie file: a thousand changes within the second write nothing yet");
    yonder_cookies_turn(k,6000);
    check(fake_writes==writes+2 && strstr(fake_text(COOKIE_FILE),"\tloop\t999\n")!=NULL &&
          yonder_cookies_due(k)==YONDER_NEVER,"cookie file: and one write at the second carries the last of them");
    /* The window's wiring: the loop's schedule hands the ticker the next
     * write's moment, and the loop's turn makes it. */
    s_cookies=k;
    cookie_hear(jar,"wired=1; Max-Age=100000");
    now_ms=6100;
    pictures_schedule();
    check(ticker_due==6000+YONDER_COOKIES_WRITE_MS,"cookie file: the ticker is handed the write's moment");
    now_ms=6000+YONDER_COOKIES_WRITE_MS;
    cookies_turn();
    check(strstr(fake_text(COOKIE_FILE),"\twired\t1\n")!=NULL,"cookie file: and the loop's turn writes it");
    s_cookies=NULL;
    /* A session cookie is no change the file must hear. */
    cookie_hear(jar,"s=session");
    check(yonder_cookies_due(k)==YONDER_NEVER,"cookie file: a session cookie owes the file nothing");
    /* Another window writes the file: this one follows it at its next look. */
    cookie_file_seed("k.test\tFALSE\t/\tFALSE\t1800000000\tother\t2\n");
    yonder_cookies_look(k);
    check(os64_streq(cookie_sent(jar),"s=session; other=2"),
        "cookie file: a look follows another window's file, what it let go and what it set");
    /* Two windows on one file, each with its own jar. */
    way_jar_t *jar2=way_jar_new();
    yonder_cookies_t *k2=yonder_cookies_new(jar2,"/tmp/profile",YONDER_COOKIES_ON);
    cookie_hear(jar2,"second=2; Max-Age=100000");
    cookie_hear(jar,"first=1; Max-Age=100000");
    yonder_cookies_turn(k2,7000);
    fake_second++;
    yonder_cookies_turn(k,9000);
    text=fake_text(COOKIE_FILE);
    check(strstr(text,"\tfirst\t1\n")!=NULL && strstr(text,"\tsecond\t2\n")!=NULL && strstr(text,"\tother\t2\n")!=NULL,
        "cookie file: two windows' writes merge, neither losing the other's");
    check(strstr(cookie_sent(jar),"second=2")!=NULL,"cookie file: a window follows the file it wrote, the other's in it");
    yonder_cookies_look(k2);
    check(strstr(cookie_sent(jar2),"first=1")!=NULL,"cookie file: and the first window's is the second's at its look");
    /* A write racing another window's, which publishes between this one's
     * read and its rename: the file is read again, and both are kept. And
     * flush writes what is owed, due or not. */
    cookie_hear(jar,"race=1; Max-Age=100000");
    fake_sync_hook=cookie_racer;
    yonder_cookies_flush(k);
    text=fake_text(COOKIE_FILE);
    check(fake_sync_hook==NULL && strstr(text,"\trace\t1\n")!=NULL && strstr(text,"\tracer\t1\n")!=NULL,
        "cookie file: a write another window's overtook is merged again, and flush writes what is owed");
    /* Clear cookies: the jar and the file. */
    int64_t gone=yonder_cookies_clear(k);
    text=fake_text(COOKIE_FILE);
    check(gone>0 && way_jar_count(jar)==0 && text!=NULL && strchr(text,'\t')==NULL &&
          strncmp(text,"# Netscape HTTP Cookie File\n",28)==0,"cookie file: Clear cookies empties the jar and the file");
    yonder_cookies_look(k2);
    check(strstr(cookie_sent(jar2),"first=")==NULL && strstr(cookie_sent(jar2),"second=")==NULL,
        "cookie file: and the other window follows the cleared file");
    /* Session: memory only, never written, and the file kept as it was
     * until yonder leaves. */
    cookie_file_seed("k.test\tFALSE\t/\tFALSE\t1800000000\tleft\t1\n");
    yonder_cookies_set_mode(k,YONDER_COOKIES_SESSION);
    cookie_hear(jar,"b=2; Max-Age=100000");
    writes=fake_writes;
    yonder_cookies_turn(k,20000);
    check(fake_writes==writes && yonder_cookies_due(k)==YONDER_NEVER && strstr(fake_text(COOKIE_FILE),"\tleft\t1\n")!=NULL &&
          strstr(cookie_sent(jar),"b=2")!=NULL,"cookie file: session keeps cookies in memory and never writes them");
    /* Back on before yonder leaves: nothing was lost, the file is followed,
     * and what was set meanwhile is owed. */
    yonder_cookies_set_mode(k,YONDER_COOKIES_ON);
    check(strstr(cookie_sent(jar),"left=1")!=NULL && yonder_cookies_due(k)!=YONDER_NEVER,
        "cookie file: on again follows the file and owes it the session's changes");
    yonder_cookies_turn(k,30000);
    check(strstr(fake_text(COOKIE_FILE),"\tb\t2\n")!=NULL,"cookie file: and writes them");
    /* Off: none sent, none kept, the file emptied. */
    yonder_cookies_set_mode(k,YONDER_COOKIES_OFF);
    cookie_hear(jar,"c=3; Max-Age=100000");
    check(cookie_sent(jar)[0]=='\0' && way_jar_count(jar)==0 && strchr(fake_text(COOKIE_FILE),'\t')==NULL &&
          yonder_cookies_due(k)==YONDER_NEVER,"cookie file: off sends none, keeps none, and empties the file");
    yonder_cookies_free(k); way_jar_free(jar);
    yonder_cookies_free(k2); way_jar_free(jar2);
    /* Off at start empties a file a run before left. */
    cookie_file_seed("k.test\tFALSE\t/\tFALSE\t1800000000\tsid\tx\n");
    jar=way_jar_new();
    k=yonder_cookies_new(jar,"/tmp/profile",YONDER_COOKIES_OFF);
    check(strchr(fake_text(COOKIE_FILE),'\t')==NULL && cookie_sent(jar)[0]=='\0',"cookie file: off at start empties the file");
    yonder_cookies_free(k); way_jar_free(jar);
    /* Session as yonder leaves: the file emptied, the window's cookies its
     * own until then. */
    cookie_file_seed("k.test\tFALSE\t/\tFALSE\t1800000000\tsid\tx\n");
    jar=way_jar_new();
    k=yonder_cookies_new(jar,"/tmp/profile",YONDER_COOKIES_ON);
    yonder_cookies_set_mode(k,YONDER_COOKIES_SESSION);
    check(strstr(fake_text(COOKIE_FILE),"\tsid\tx\n")!=NULL,"cookie file: switching to session leaves the file until yonder leaves");
    yonder_cookies_flush(k);
    check(strchr(fake_text(COOKIE_FILE),'\t')==NULL && os64_streq(cookie_sent(jar),"sid=x"),
        "cookie file: and session empties it as yonder leaves");
    yonder_cookies_free(k); way_jar_free(jar);
    /* Session at start empties a file a session that crashed left. */
    cookie_file_seed("k.test\tFALSE\t/\tFALSE\t1800000000\tsid\tx\n");
    jar=way_jar_new();
    k=yonder_cookies_new(jar,"/tmp/profile",YONDER_COOKIES_SESSION);
    check(strchr(fake_text(COOKIE_FILE),'\t')==NULL && cookie_sent(jar)[0]=='\0',"cookie file: session at start empties the file");
    yonder_cookies_free(k); way_jar_free(jar);
    /* A file that cannot be read is never written over, and says so. */
    cookie_file_seed("k.test\tFALSE\t/\tFALSE\t1800000000\tsid\tx\n");
    jar=way_jar_new();
    fake_slurp_fail=true;
    k=yonder_cookies_new(jar,"/tmp/profile",YONDER_COOKIES_ON);
    cookie_hear(jar,"a=1; Max-Age=100000");
    yonder_cookies_turn(k,1000);
    fake_slurp_fail=false;
    check(strstr(fake_text(COOKIE_FILE),"\tsid\tx\n")!=NULL && yonder_cookies_problem(k)!=NULL &&
          strstr(yonder_cookies_problem(k),"cannot be read")!=NULL,"cookie file: unreadable, it is not written over");
    /* A write the disk refuses is said, and tried again ten seconds on. */
    check(yonder_cookies_due(k)==1000+YONDER_COOKIES_RETRY_MS,"cookie file: a write that failed waits ten seconds");
    fake_fail_rename=true;
    yonder_cookies_turn(k,1000+YONDER_COOKIES_RETRY_MS);
    check(strstr(yonder_cookies_problem(k),"cannot be written")!=NULL &&
          yonder_cookies_due(k)==1000+2*YONDER_COOKIES_RETRY_MS,"cookie file: a refused write is said and waits to try again");
    fake_fail_rename=false;
    yonder_cookies_turn(k,1000+2*YONDER_COOKIES_RETRY_MS);
    check(yonder_cookies_problem(k)==NULL && strstr(fake_text(COOKIE_FILE),"\ta\t1\n")!=NULL,
        "cookie file: and the next write, which takes, clears it");
    yonder_cookies_free(k); way_jar_free(jar);
    /* A folder by the file's name (a person made it, expecting one): said
     * for what it is, and neither removed nor written over. */
    fake_reset();
    snprintf(fake_dirs[nfake_dirs++],256,"/tmp/profile");
    snprintf(fake_dirs[nfake_dirs++],256,COOKIE_FILE);
    jar=way_jar_new();
    k=yonder_cookies_new(jar,"/tmp/profile",YONDER_COOKIES_ON);
    cookie_hear(jar,"a=1; Max-Age=100000");
    yonder_cookies_turn(k,1000);
    check(fake_is_dir(COOKIE_FILE) && yonder_cookies_problem(k)!=NULL &&
          strstr(yonder_cookies_problem(k),"is a folder")!=NULL,"cookie file: a folder in its place is said as one, and left alone");
    yonder_cookies_free(k); way_jar_free(jar);
    /* A look whose follow runs short of memory is not the file followed:
     * the next look reads it again. */
    fake_reset();
    snprintf(fake_dirs[nfake_dirs++],256,"/tmp/profile");
    cookie_file_seed("k.test\tFALSE\t/\tFALSE\t1800000000\tsid\tkept\n");
    jar=way_jar_new();
    k=yonder_cookies_new(jar,"/tmp/profile",YONDER_COOKIES_ON);
    cookie_file_seed("k.test\tFALSE\t/\tFALSE\t1800000000\tsid\tkept\nk.test\tFALSE\t/\tFALSE\t1800000000\tother\t2\n");
    fail_at=attempts+2;                 /* the file's bytes, then the follow's line */
    yonder_cookies_look(k);
    fail_at=0;
    check(strstr(cookie_sent(jar),"other=2")==NULL,"cookie file: a follow short of memory takes nothing");
    yonder_cookies_look(k);
    check(strstr(cookie_sent(jar),"other=2")!=NULL,"cookie file: and the next look reads the file again");
    yonder_cookies_free(k); way_jar_free(jar);
    /* A dead window's temp with no file beside it (a crash before the
     * first rename) is taken by Clear cookies all the same. */
    fake_reset();
    snprintf(fake_dirs[nfake_dirs++],256,"/tmp/profile");
    fake_live[0]=99;
    {
        int f; for(f=0;fake_files[f].used;f++) {}
        fake_files[f].used=true; snprintf(fake_files[f].path,256,COOKIE_FILE "+42.3.new"); fake_files[f].data=strdup("x");
    }
    jar=way_jar_new();
    k=yonder_cookies_new(jar,"/tmp/profile",YONDER_COOKIES_ON);
    check(yonder_cookies_clear(k)==0 && fake_file(COOKIE_FILE "+42.3.new")<0,
        "cookie file: Clear takes a dead window's temp when there is no file");
    yonder_cookies_free(k); way_jar_free(jar);
    /* The profile folder is made by the first write, as page storage's. */
    fake_reset();
    jar=way_jar_new();
    k=yonder_cookies_new(jar,"/tmp/profile",YONDER_COOKIES_ON);
    cookie_hear(jar,"a=1; Max-Age=100000");
    yonder_cookies_turn(k,1000);
    check(fake_is_dir("/tmp/profile") && fake_text(COOKIE_FILE)!=NULL,"cookie file: the first write makes the folder");
    yonder_cookies_free(k); way_jar_free(jar);
    /* A temp a dead window left is removed at the first write. */
    fake_live[0]=99;
    cookie_file_seed("");
    {
        int f; for(f=0;fake_files[f].used;f++) {}
        fake_files[f].used=true; snprintf(fake_files[f].path,256,COOKIE_FILE "+42.1.new"); fake_files[f].data=strdup("x");
        for(f=0;fake_files[f].used;f++) {}
        fake_files[f].used=true; snprintf(fake_files[f].path,256,COOKIE_FILE "+99.1.new"); fake_files[f].data=strdup("x");
    }
    jar=way_jar_new();
    k=yonder_cookies_new(jar,"/tmp/profile",YONDER_COOKIES_ON);
    cookie_hear(jar,"a=1; Max-Age=100000");
    yonder_cookies_turn(k,1000);
    check(fake_file(COOKIE_FILE "+42.1.new")<0 && fake_file(COOKIE_FILE "+99.1.new")>=0,
        "cookie file: a dead window's temp goes at the first write, a live one's stays");
    /* Clear cookies takes a dead window's temp too, whenever it was left:
     * it holds cookies. */
    {
        int f; for(f=0;fake_files[f].used;f++) {}
        fake_files[f].used=true; snprintf(fake_files[f].path,256,COOKIE_FILE "+42.2.new"); fake_files[f].data=strdup("x");
    }
    (void)yonder_cookies_clear(k);
    check(fake_file(COOKIE_FILE "+42.2.new")<0 && fake_file(COOKIE_FILE "+99.1.new")>=0,
        "cookie file: Clear cookies takes a dead window's temp, and leaves a live one's");
    yonder_cookies_free(k); way_jar_free(jar);
    /* yonder.conf's switch, and Settings' two boxes for it. */
    const char *refused="unset";
    saved_cookies[0]=0;
    check(yonder_settings_saved_cookies(&refused)==YONDER_COOKIES_ON && refused==NULL,"cookies: absent is on");
    strcpy(saved_cookies,"Session");
    check(yonder_settings_saved_cookies(&refused)==YONDER_COOKIES_SESSION && refused==NULL,"cookies: session, any case");
    strcpy(saved_cookies,"off");
    check(yonder_settings_saved_cookies(&refused)==YONDER_COOKIES_OFF,"cookies: off");
    strcpy(saved_cookies,"never");
    check(yonder_settings_saved_cookies(&refused)==YONDER_COOKIES_ON && refused!=NULL &&
          strstr(refused,"not on, session or off")!=NULL,"cookies: anything else is on, said");
    saved_cookies[0]=0;
}
static void storage_orphan_cases(void) {
    fake_reset();
    snprintf(fake_dirs[nfake_dirs++],256,"/tmp/profile");
    snprintf(fake_dirs[nfake_dirs++],256,"/tmp/profile/storage");
    static const char kImage[]="os64 storage 1\n+1 1\nk1\n";
    fake_live[0]=8;
    fake_put(STORE_FILE,kImage,sizeof(kImage)-1);
    fake_put(STORE_FILE "+9.3.new",kImage,sizeof(kImage)-1);       /* task 9: gone */
    fake_put(STORE_FILE "+7.1.new",kImage,sizeof(kImage)-1);       /* this task's, from before it was */
    fake_put(STORE_FILE "+8.2.new",kImage,sizeof(kImage)-1);       /* task 8: live, mid-commit */
    /* Names a dead or this task's number follows a '+' in, that are not a
     * temp by its whole spelling (a lock an older yonder left among them):
     * never ours to remove. */
    static const char *const kNotTemps[]={
        "/tmp/profile/storage/notes+9", "/tmp/profile/storage/x+9.1.new.bak", "/tmp/profile/storage/x+9.new",
        "/tmp/profile/storage/x+.1.new", "/tmp/profile/storage/x+09.1.new", "/tmp/profile/storage/x+7.01.new",
        "/tmp/profile/storage/+9.1.new", "/tmp/profile/storage/x+9.1.newer",
        "/tmp/profile/storage/http%3A%2F%2Fother.test+lock",
    };
    for(size_t i=0;i<sizeof(kNotTemps)/sizeof(kNotTemps[0]);i++) fake_put(kNotTemps[i],"mine",4);
    yonder_storage_t *st=yonder_storage_new("/tmp/profile");
    (void)yonder_storage_identity(st,OS64_DOM_STORAGE_LOCAL,"http://fixture.test");
    check(fake_file(STORE_FILE "+9.3.new")<0 && fake_file(STORE_FILE "+7.1.new")<0,
          "orphans: the first touch removes the temps a crash left");
    check(fake_file(STORE_FILE "+8.2.new")>=0 && fake_file(STORE_FILE)>=0,
          "orphans: a live window's temp, and the store, stay");
    fake_put(STORE_FILE "+9.4.new",kImage,sizeof(kImage)-1);
    fake_put(STORE_FILE "+8.5.new",kImage,sizeof(kImage)-1);
    fake_live_zombie=true;      /* task 8 died, its parent not yet told: its temps are a crash's */
    fake_put(STORE_FILE "+8.2.new",kImage,sizeof(kImage)-1);
    check(yonder_storage_clear(st)==1 && fake_file(STORE_FILE "+8.5.new")<0 && fake_file(STORE_FILE "+8.2.new")<0,
          "orphans: a zombie's temps are a crash's");
    fake_live_zombie=false;
    fake_put(STORE_FILE,kImage,sizeof(kImage)-1);
    fake_put(STORE_FILE "+8.2.new",kImage,sizeof(kImage)-1);
    check(yonder_storage_clear(st)==1 && fake_file(STORE_FILE)<0 && fake_file(STORE_FILE "+9.4.new")<0 &&
          fake_file(STORE_FILE "+8.2.new")>=0,
          "orphans: Clear removes the stores and a crash's temps, and counts only stores");
    bool kept=true;
    for(size_t i=0;i<sizeof(kNotTemps)/sizeof(kNotTemps[0]);i++) kept=kept && fake_file(kNotTemps[i])>=0;
    check(kept,"orphans: a name that only looks like a temp outlives the sweep and Clear");
    yonder_storage_free(st);
    fake_reset();
}
/* The redirects the head says libway followed on the asked origin are the
 * page's redirectCount, both spellings. */
static void timing_redirect_case(void) {
    fake_reset();
    stream_window();
    g.scripts_on=true; g.script_ms=5000; now_ms=100;
    start_trip("http://fixture.test/p",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
    char *html=pad_page("<p id=out>start</p><script>document.getElementById('out').textContent="
        "performance.navigation.redirectCount+' '+performance.getEntriesByType('navigation')[0].redirectCount;</script>");
    way_head_t h=stream_head("text/html","",false);
    h.redirects=2;
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_body((const uint8_t *)html,strlen(html),YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    free(html);
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","2 2"),
          "timing: the head's same-origin redirects are the page's redirectCount");
    loop_drop();
}
/* The page reads its navigation's timing and the screen it is on. */
static void timing_window_cases(void) {
    fake_reset();
    clock_us=10000000;
    loop_page("<p id=out>start</p><script>var t=performance.timing;"
        "document.getElementById('out').textContent=[t.navigationStart>0,t.fetchStart===t.navigationStart,"
        "t.responseStart>=t.fetchStart,t.domLoading>=t.responseStart,t.domInteractive,t.loadEventEnd,"
        "performance.now()>=0,screen.width,devicePixelRatio,performance.navigation.type].join();"
        "window.addEventListener('load',function(){setTimeout(function(){var t=performance.timing;"
        "document.getElementById('out').textContent=[t.domInteractive>=t.domLoading,"
        "t.domContentLoadedEventEnd>=t.domContentLoadedEventStart,t.domComplete>=t.domInteractive,"
        "t.loadEventEnd>=t.loadEventStart,t.loadEventStart>0].join()},10)});</script>");
    check(loop_settle() && g.page.tree!=NULL,"timing: the page arrives");
    check(probe_text_is("out","true,true,true,true,0,0,true,1024,1,0"),
        "timing: a page's script reads its navigation's milestones so far, the screen and the zoom");
    now_ms+=20;
    check(script_turn() && probe_text_is("out","true,true,true,true,true"),
        "timing: after its load, the page reads every milestone");
    loop_drop();
}
/* A page from disk: its navigation begins before the read, and its
 * response is the read. */
static void timing_file_case(void) {
    stream_window();
    g.scripts_on=true; g.script_ms=5000;
    slurp_path="/pages/local.html";
    slurp_bytes="<p id=out>start</p><script>var t=performance.timing;document.getElementById('out').textContent="
        "[t.fetchStart===t.navigationStart,t.responseStart-t.fetchStart<10,t.responseEnd-t.responseStart>=50].join();</script>";
    slurp_us=50000;
    open_address("/pages/local.html",NAV_GO,NULL,NULL);
    slurp_us=0;
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out","true,true,true"),
        "timing: a file's response starts as its read does and ends when the read has");
    slurp_path=NULL;
    loop_drop();
}
/* With the switch off, none of slice 1a's names exists. */
static void storage_modern_off_cases(void) {
    loop_modern_off=true;
    loop_page("<p id=out>start</p><script>document.getElementById('out').textContent="
        "[typeof localStorage,typeof sessionStorage,typeof Storage,typeof performance,typeof URL,"
        "typeof URLSearchParams,typeof TextEncoder,typeof TextDecoder,typeof screen,typeof devicePixelRatio,"
        "typeof btoa,typeof atob].join();</script>");
    loop_modern_off=false;
    check(loop_settle() && g.page.tree!=NULL && probe_text_is("out",
        "undefined,undefined,undefined,undefined,undefined,undefined,undefined,undefined,undefined,undefined,undefined,undefined"),
        "modern off: storage, performance, URL, the encoders, screen and base64 do not exist");
    loop_drop();
}
static void storage_cases(void) {
    storage_window_cases();
    storage_full_cases();
    storage_departure_cases();
    storage_disk_cases();
    storage_two_yonder_cases();
    storage_departed_tasks_case();
    storage_clear_cases();
    storage_orphan_cases();
    cookie_file_cases();
    timing_window_cases();
    timing_redirect_case();
    timing_file_case();
    storage_modern_off_cases();
}

static void diag_cases(void) {
    diag_unit_cases();
    diag_setting_cases();
    diag_page_cases();
    diag_bytes_case();
    diag_picture_case();
    diag_cookies_case();
    diag_picture_unshown_case();
    diag_picture_workerless_case();
    diag_picture_unasked_case();
    diag_css_unasked_case();
    diag_style_case();
    diag_lost_distinct_case();
    picture_tick_cases();
    picture_text_cases();
    round_picture_glass_cases();
    diag_fact_short_case();
    diag_lost_overflow_case();
    diag_lookup_bound_case();
    diag_arriving_released_case();
    diag_picture_moves_case();
    diag_long_source_case();
    diag_script_time_case();
    diag_tokens_exclusive();
    diag_keep_cases();
    /* Setting absent: no file, while the badge still counts. */
    int writes=fake_writes;
    memset(&s_diag,0,sizeof(s_diag)); s_diag.next=1;
    loop_page("<script>nothere()</script>");
    check(loop_settle() && fake_writes==writes && !strcmp(g.badge_text,"MISSING 1  FAILED 1"),
        "diag: with the setting absent nothing is written, and the badge still counts");
    loop_drop();
    fake_reset();
}

/* A menu's invisible checkbox (MediaWiki Vector: absolute, width and
 * height 100%, opacity 0) covers its menu and nothing below it, so a link
 * under the menu is still the link under the pointer. */
static void covered_link_cases(void) {
    probe_page("<!doctype html><style>body{margin:0} nav{position:relative;height:40px}"
        "nav input{position:absolute;top:0;left:0;width:100%;height:100%;opacity:0;margin:0}"
        "p{margin:0}</style><nav><input type=checkbox id=menu><span>More</span></nav>"
        "<p><a id=below href=/below>below the menu</a></p>",false);
    const flow_box_t *menu=flow_box_for(g.page.tree,probe_id("menu"));
    os64_gui_rect_t r=menu!=NULL?flow_box_doc_rect(menu,(flow_point_t){0,0}):(os64_gui_rect_t){0,0,0,0};
    check(menu!=NULL&&r.h==40,"covered link: the checkbox is as tall as its menu");
    check(link_at(20,50)==os64_page_link_for(page_model(&g.page),probe_id("below")),
        "covered link: the link below the menu is the one under the pointer");
    forms_place();
    check(probe_field("menu")!=NULL && !probe_field("menu")->w->hidden &&
          probe_field("menu")->w->cls->paint==NULL && probe_field("menu")->w->cls->event!=NULL,
        "invisible gadget: a nameless control in no form paints nothing, and its widget stays");
    int32_t menu_control=os64_page_control_for(page_model(&g.page),probe_id("menu"));
    ui_click_on("menu");
    bool on=os64_page_control(page_model(&g.page),menu_control)->checked;
    ui_key_space();
    check(on && !os64_page_control(page_model(&g.page),menu_control)->checked &&
          probe_field("menu")->w->focused,
        "invisible gadget: a click ticks it and takes the focus, and Space unticks it, as any checkbox");
    probe_drop();
    /* With scripts, the tick is the box's click (which sees it ticked, as
     * a widget's does), then input and change. */
    probe_page("<!doctype html><style>body{margin:0} input{opacity:0;width:40px;height:40px;margin:0}</style>"
        "<input type=checkbox id=menu><p id=out>-</p><script>var m=document.getElementById('menu'),"
        "o=document.getElementById('out'),seen='';['click','input','change'].forEach(function(t){"
        "m.addEventListener(t,function(){seen+=t+(m.checked?'+':'-')+' ';o.textContent=seen;});});"
        "</script>",true);
    script_turn();
    forms_place();
    ui_click_on("menu");
    check(probe_text_is("out","click+ input+ change+ "),
        "invisible gadget: with scripts its click sees it ticked, then input and change");
    probe_drop();
    /* A custom checkbox's real input is a field: drawn, though invisible,
     * whether a form or a name makes it one. */
    probe_page("<!doctype html><style>body{margin:0} input{opacity:0}</style>"
        "<form><input type=checkbox id=formed></form><input type=checkbox id=named name=agree>"
        "<input type=checkbox id=plain style='opacity:1'><input type=checkbox id=blank name=''>",false);
    forms_place();
    check(!probe_field("formed")->w->hidden && !probe_field("named")->w->hidden &&
          !probe_field("plain")->w->hidden,
        "invisible field: in a form or named, an invisible control is drawn; a visible one always is");
    /* What a navigation sends when the agent's preset names no list (way_open
     * composes it): every picture type libimage decodes, whole, inside what
     * libfetch will send — a longer Accept is every such navigation refused. */
    char accept[OS64_FETCH_ACCEPT_MAX];
    way_accept_compose(YONDER_ACCEPT,OS64_IMAGE_ACCEPT,accept,sizeof(accept));
    check(!strncmp(accept,"text/html, application/xhtml+xml, text/*;q=0.8, image/png;q=0.5, ",64) &&
          strstr(accept,"image/x-portable-pixmap;q=0.5")!=NULL && strlen(accept)<OS64_FETCH_ACCEPT_MAX,
        "navigation accept: pages first, then every picture type libimage decodes at half the weight");
    check(probe_field("blank")!=NULL && probe_field("blank")->w->cls->paint==NULL,
        "invisible gadget: an empty name submits nothing, so it names nothing");
    probe_drop();
    /* A control the ellipsis cuts away is not there to be pointed at
     * (flow_box_cut_away), so its widget is not drawn over the ellipsis:
     * after abcd, 32 of a 40-wide line, a checkbox in an inline-block with
     * a transform — stacked, a layer of its own — is past the limit; one
     * kept as the line's first atom is drawn. */
    probe_page("<!doctype html><style>body{margin:0} div{width:40px;overflow:hidden;"
        "white-space:nowrap;text-overflow:ellipsis} input{margin:0;width:12px;height:6px}"
        " span{display:inline-block;transform:translate(1px,0)}</style>"
        "<div>abcd<span><input type=checkbox name=past id=past></span></div>"
        "<div><span><input type=checkbox name=kept id=kept></span>abcdefghij</div>",false);
    forms_place();
    check(probe_field("past")!=NULL && probe_field("past")->w->hidden,
        "ellipsis: a control past the cut has no widget over the ellipsis");
    check(probe_field("kept")!=NULL && !probe_field("kept")->w->hidden,
        "ellipsis: a control the cut keeps has its widget");
    probe_drop();
}

#include "test_yonder_requests.inc"
#include "test_yonder_modules.inc"
#include "test_yonder_window.inc"
#include "test_yonder_document.inc"

/* The window's name: the page's title through fit_title. A control
 * character the page spells is a '?', never a byte the kernel would log
 * raw; past ASCII is one '?' a character; a long title is cut with "...". */
static void title_fit_cases(void)
{
    char out[OS64_GUI_TITLE_MAX];
    fit_title("A\x1b[2J\x7f" "B", out);
    check(os64_streq(out, "A?[2J?B - yonder"), "title: an ESC and a DEL become '?'");
    fit_title("Caf\xc3\xa9", out);
    check(os64_streq(out, "Caf? - yonder"), "title: a character past ASCII is one '?'");
    fit_title("An extremely long page title that cannot fit", out);
    check(os64_strlen(out) == OS64_GUI_TITLE_MAX - 1 &&
          os64_streq(out, "An extremely long p... - yonder"), "title: cut with an ellipsis to the capacity");
    const char *page = "<!doctype html><title>\x1b]0;x\x07 Name\t</title><body></body>";
    os64_html_options_t opt = os64_html_options_default();
    os64_html_parser_t *p = os64_html_parser_new(&opt);
    check(p != NULL, "title: a parser");
    if (p == NULL)
        return;
    os64_html_parser_feed(p, page, os64_strlen(page));
    os64_html_document_t *doc = os64_html_parser_finish(p);
    char named[120];
    title_of(doc, named, sizeof(named));
    fit_title(named, out);
    check(os64_streq(out, "?]0;x? Name - yonder"), "title: a page's controls never reach the window");
    os64_html_document_free(doc);
}

/* A unit that includes this file for the window and its fakes alone (the
 * WPT runner, the replay runner) defines YONDER_HOST_FAKES_ONLY and brings
 * its own main. */
#ifndef YONDER_HOST_FAKES_ONLY
int main(void)
{
    os64_text_options_t options={.memory={.alloc=probe_alloc,.free=probe_free},
        .backend=flow_test_backend(),.memory_cap=8*1024*1024};
    check(os64_text_create(&options,&probe_text)==OS64_FONT_OK,"text context");
    check(os64_text_font_bitmap(probe_text,&probe_font)==OS64_FONT_OK,"bitmap face");
    classic_browser_cases();
    address_key_cases();
    interface_font_cases();
    inserted_frame_cases();
    stand_in_paint_cases();
    covered_link_cases();
    classic_downloaded_cases();
    hover_batch_cases();
    classic_hover_profile();
    geometry_cases();
    geometry_body_cases();
    geometry_publish_cases();
    geometry_scroll_cases();
    geometry_fragment_cases();
    geometry_failure_cases();
    script_types();
    identity_and_edit();
    scripted_zoom();
    scripted_face_budget();
    property_only();
    script_lifecycle();
    native_refusals();
    switch_cases();
    motion_cases();
    motion_part_cases();
    tick_cases();
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
    reporting_teardown();
    if(getenv("YONDER_NO_STREAM")==NULL) join_cases();
    if(getenv("YONDER_NO_STREAM")==NULL) diag_cases();
    if(getenv("YONDER_NO_STREAM")==NULL) storage_cases();
    if(getenv("YONDER_NO_STREAM")==NULL) request_cases();
    if(getenv("YONDER_NO_STREAM")==NULL) module_cases();
    if(getenv("YONDER_NO_STREAM")==NULL) window_cases();
    if(getenv("YONDER_NO_STREAM")==NULL) document_cases();
    webfont_cases();
    paint_cost_cases();
    whole_view_cases();
    title_fit_cases();
    os64_text_font_release(probe_font);
    check(os64_text_destroy(probe_text)==OS64_FONT_OK,"all layout text released");
    check(live==0,"native and engine heap is empty");
    printf("Yonder scripted-page host: %u checks, %u failed\n",checks,failures);
    return failures?1:0;
}
#endif
